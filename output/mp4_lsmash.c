/*****************************************************************************
 * mp4_lsmash.c: mp4 muxer using L-SMASH
 *****************************************************************************
 * Copyright (C) 2003-2017 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
 *          Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *          Takashi Hirata <silverfilain@gmail.com>
 *          golgol7777 <golgol7777@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "common/common.h"
#include "output.h"
#include <lsmash.h>
#include <lsmash_importer.h>

#define H264_NALU_LENGTH_SIZE 4

/*******************/

#define USE_LSMASH_IMPORTER 0

#if ( defined(HAVE_AUDIO) && HAVE_AUDIO ) || ( defined(USE_LSMASH_IMPORTER) && USE_LSMASH_IMPORTER )
#define HAVE_ANY_AUDIO 1
#else
#define HAVE_ANY_AUDIO 0
#endif

/*******************/

#define MP4_LOG_ERROR( ... )                x264_cli_log( "mp4", X264_LOG_ERROR, __VA_ARGS__ )
#define MP4_LOG_WARNING( ... )              x264_cli_log( "mp4", X264_LOG_WARNING, __VA_ARGS__ )
#define MP4_LOG_INFO( ... )                 x264_cli_log( "mp4", X264_LOG_INFO, __VA_ARGS__ )
#define MP4_FAIL_IF_ERR( cond, ... )        FAIL_IF_ERR( cond, "mp4", __VA_ARGS__ )

/* For close_file() */
#define MP4_LOG_IF_ERR( cond, ... )\
do\
{\
    if( cond )\
    {\
        MP4_LOG_ERROR( __VA_ARGS__ );\
    }\
} while( 0 )

/* For open_file() */
#define MP4_FAIL_IF_ERR_EX( cond, ... )\
do\
{\
    if( cond )\
    {\
        remove_mp4_hnd( p_mp4 );\
        MP4_LOG_ERROR( __VA_ARGS__ );\
        return -1;\
    }\
} while( 0 )

/*******************/

#if HAVE_ANY_AUDIO

#if HAVE_AUDIO
#include "audio/encoders.h"
#endif

typedef struct
{
    int i_numframe;
    uint32_t i_track;
    uint32_t i_sample_entry;
    uint64_t i_video_timescale;    /* For interleaving. */
    lsmash_audio_summary_t *summary;
    lsmash_codec_type_t codec_type;
#if HAVE_AUDIO
    audio_info_t *info;
    hnd_t encoder;
    int has_sbr;
    int b_copy;
    int b_mdct;
#else
    mp4sys_importer_t* p_importer;
    uint32_t last_delta;
#endif
} mp4_audio_hnd_t;
#endif /* #if HAVE_ANY_AUDIO */

typedef struct
{
    lsmash_root_t *p_root;
    lsmash_brand_type major_brand;
    lsmash_video_summary_t *summary;
    int i_brand_3gpp;
    int b_brand_m4a;
    int b_brand_qt;
    int b_stdout;
    char *psz_chapter;
    int b_add_bom;
    char *psz_language;
    uint32_t i_movie_timescale;
    uint32_t i_video_timescale;
    uint32_t i_track;
    uint32_t i_sample_entry;
    uint64_t i_time_inc;
    int64_t i_start_offset;
    uint64_t i_first_cts;
    uint64_t i_prev_dts;
    uint32_t i_sei_size;
    uint8_t *p_sei_buffer;
    int i_numframe;
    int64_t i_init_delta;
    int i_delay_frames;
    int b_dts_compress;
    int i_dts_compress_multiplier;
    int b_use_recovery;
    int i_recovery_frame_cnt;
    int i_max_frame_num;
    uint64_t i_last_intra_cts;
    uint32_t i_display_width;
    uint32_t i_display_height;
    int b_no_remux;
    int b_no_pasp;
    int b_force_display_size;
    int b_fragments;
    lsmash_file_parameters_t file_param;
	lsmash_scale_method scale_method;
#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *audio_hnd;
#endif
    int b_no_progress;
} mp4_hnd_t;

/*******************/

static void set_recovery_param( mp4_hnd_t *p_mp4, x264_param_t *p_param )
{
    p_mp4->b_use_recovery = p_param->b_open_gop || p_param->b_intra_refresh;
    if( !p_mp4->b_use_recovery )
        return;

    /* almost copied from x264_sps_init in encoder/set.c */
    int i_num_reorder_frames = p_param->i_bframe_pyramid ? 2 : p_param->i_bframe ? 1 : 0;
    int i_num_ref_frames = X264_MIN(X264_REF_MAX, X264_MAX4(p_param->i_frame_reference, 1 + i_num_reorder_frames,
                           p_param->i_bframe_pyramid ? 4 : 1, p_param->i_dpb_size));
    i_num_ref_frames -= p_param->i_bframe_pyramid == X264_B_PYRAMID_STRICT;
    if( p_param->i_keyint_max == 1 )
        i_num_ref_frames = 0;

    p_mp4->i_max_frame_num = i_num_ref_frames * (!!p_param->i_bframe_pyramid+1) + 1;
    if( p_param->b_intra_refresh )
    {
        p_mp4->i_recovery_frame_cnt = X264_MIN( ( p_param->i_width + 15 ) / 16 - 1, p_param->i_keyint_max ) + p_param->i_bframe - 1;
        p_mp4->i_max_frame_num = X264_MAX( p_mp4->i_max_frame_num, p_mp4->i_recovery_frame_cnt + 1 );
    }

    int i_log2_max_frame_num = 4;
    while( (1 << i_log2_max_frame_num) <= p_mp4->i_max_frame_num )
        i_log2_max_frame_num++;

    p_mp4->i_max_frame_num = 1 << i_log2_max_frame_num;
}

#if HAVE_AUDIO
static int set_channel_layout( mp4_audio_hnd_t *p_audio )
{
#define AV_CH_LAYOUT_MONO              (AV_CH_FRONT_CENTER)
#define AV_CH_LAYOUT_STEREO            (AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT)
#define AV_CH_LAYOUT_2_1               (AV_CH_LAYOUT_STEREO|AV_CH_BACK_CENTER)
#define AV_CH_LAYOUT_SURROUND          (AV_CH_LAYOUT_STEREO|AV_CH_FRONT_CENTER)
#define AV_CH_LAYOUT_4POINT0           (AV_CH_LAYOUT_SURROUND|AV_CH_BACK_CENTER)
#define AV_CH_LAYOUT_2_2               (AV_CH_LAYOUT_STEREO|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT)
#define AV_CH_LAYOUT_QUAD              (AV_CH_LAYOUT_STEREO|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT)
#define AV_CH_LAYOUT_5POINT0           (AV_CH_LAYOUT_SURROUND|AV_CH_SIDE_LEFT|AV_CH_SIDE_RIGHT)
#define AV_CH_LAYOUT_5POINT1           (AV_CH_LAYOUT_5POINT0|AV_CH_LOW_FREQUENCY)
#define AV_CH_LAYOUT_5POINT0_BACK      (AV_CH_LAYOUT_SURROUND|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT)
#define AV_CH_LAYOUT_5POINT1_BACK      (AV_CH_LAYOUT_5POINT0_BACK|AV_CH_LOW_FREQUENCY)
#define AV_CH_LAYOUT_7POINT0           (AV_CH_LAYOUT_5POINT0|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT)
#define AV_CH_LAYOUT_7POINT1           (AV_CH_LAYOUT_5POINT1|AV_CH_BACK_LEFT|AV_CH_BACK_RIGHT)
#define AV_CH_LAYOUT_7POINT1_WIDE      (AV_CH_LAYOUT_5POINT1_BACK|AV_CH_FRONT_LEFT_OF_CENTER|AV_CH_FRONT_RIGHT_OF_CENTER)
#define AV_CH_LAYOUT_STEREO_DOWNMIX    (AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT)

    lsmash_qt_audio_channel_layout_t temp;
    temp.channelLayoutTag = QT_CHANNEL_LAYOUT_UNKNOWN;
    temp.channelBitmap    = 0;

    /* Lavcodec always returns SMPTE/ITU-R channel order, but its copying doesn't do reordering. */
    if( lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_ALAC_AUDIO ) && p_audio->info->channels <= 8 )
    {
        static const lsmash_channel_layout_tag channel_table[] = {
            QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP,
            QT_CHANNEL_LAYOUT_ALAC_MONO,
            QT_CHANNEL_LAYOUT_ALAC_STEREO,
            QT_CHANNEL_LAYOUT_ALAC_3_0,
            QT_CHANNEL_LAYOUT_ALAC_4_0,
            QT_CHANNEL_LAYOUT_ALAC_5_0,
            QT_CHANNEL_LAYOUT_ALAC_5_1,
            QT_CHANNEL_LAYOUT_ALAC_6_1,
            QT_CHANNEL_LAYOUT_ALAC_7_1
        };
        temp.channelLayoutTag = channel_table[p_audio->info->channels];
    }
    else if( !p_audio->b_copy )
    {
        temp.channelLayoutTag = QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP;
        temp.channelBitmap    = p_audio->info->chanlayout;
        /* Avisynth input doesn't return channel order, so we guess it from the number of channels. */
        if( !p_audio->info->chanlayout && p_audio->info->channels <= 8 )
        {
            static const lsmash_channel_layout_tag channel_table[] = {
                QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP,
                QT_CHANNEL_LAYOUT_ITU_1_0,
                QT_CHANNEL_LAYOUT_ITU_2_0,
                QT_CHANNEL_LAYOUT_ITU_3_0,
                QT_CHANNEL_LAYOUT_ITU_3_1,
                QT_CHANNEL_LAYOUT_ITU_3_2,
                QT_CHANNEL_LAYOUT_ITU_3_2_1,
                QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP,
                QT_CHANNEL_LAYOUT_ITU_3_4_1
            };
            temp.channelLayoutTag = channel_table[p_audio->info->channels];
        }
    }
    else if( lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
    {
        /* Channel order is unknown, so we guess it from ffmpeg's channel layout flags. */
        static const lsmash_qt_audio_channel_layout_t channel_table[] = {
            { AV_CH_LAYOUT_MONO,           QT_CHANNEL_LAYOUT_MONO },
            { AV_CH_LAYOUT_STEREO,         QT_CHANNEL_LAYOUT_STEREO },
            { AV_CH_LAYOUT_STEREO_DOWNMIX, QT_CHANNEL_LAYOUT_STEREO },
            { AV_CH_LAYOUT_2_1,            QT_CHANNEL_LAYOUT_AAC_3_0 },
            { AV_CH_LAYOUT_SURROUND,       QT_CHANNEL_LAYOUT_AAC_3_0 },
            { AV_CH_LAYOUT_4POINT0,        QT_CHANNEL_LAYOUT_AAC_4_0 },
            { AV_CH_LAYOUT_2_2,            QT_CHANNEL_LAYOUT_AAC_QUADRAPHONIC },
            { AV_CH_LAYOUT_QUAD,           QT_CHANNEL_LAYOUT_AAC_QUADRAPHONIC },
            { AV_CH_LAYOUT_5POINT0,        QT_CHANNEL_LAYOUT_AAC_5_0 },
            { AV_CH_LAYOUT_5POINT0_BACK,   QT_CHANNEL_LAYOUT_AAC_5_0 },
            { AV_CH_LAYOUT_5POINT1,        QT_CHANNEL_LAYOUT_AAC_5_1 },
            { AV_CH_LAYOUT_5POINT1_BACK,   QT_CHANNEL_LAYOUT_AAC_5_1 },
            { AV_CH_LAYOUT_7POINT0,        QT_CHANNEL_LAYOUT_AAC_7_0 },
            { AV_CH_LAYOUT_7POINT1,        QT_CHANNEL_LAYOUT_AAC_7_1 },
            { AV_CH_LAYOUT_7POINT1_WIDE,   QT_CHANNEL_LAYOUT_AAC_7_1 }
        };
        for( int i = 0; i < sizeof(channel_table) / sizeof(lsmash_qt_audio_channel_layout_t); i++ )
            if( p_audio->info->chanlayout == channel_table[i].channelBitmap )
            {
                temp.channelLayoutTag = channel_table[i].channelLayoutTag;
                break;
            }
    }

    if( temp.channelLayoutTag != QT_CHANNEL_LAYOUT_UNKNOWN )
    {
        lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_AUDIO_CHANNEL_LAYOUT,
                                                                               LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( !specific )
        {
            MP4_LOG_ERROR( "failed to allocate memory for channel layout info.\n" );
            return -1;
        }
        *(lsmash_qt_audio_channel_layout_t *)specific->data.structured = temp;
        if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific ) )
        {
            lsmash_destroy_codec_specific_data( specific );
            return -1;
        }
        lsmash_destroy_codec_specific_data( specific );
    }

    return 0;
}
#endif

#if HAVE_ANY_AUDIO
static void remove_audio_hnd( mp4_audio_hnd_t *p_audio )
{
    if( p_audio->summary )
    {
        /* WARNING: You should not rely on this if you created summary in your own code. */
        lsmash_cleanup_summary( (lsmash_summary_t *)p_audio->summary );
        p_audio->summary = NULL;
    }
#if HAVE_AUDIO
    if( p_audio->encoder )
    {
        x264_audio_encoder_close( p_audio->encoder );
        p_audio->encoder = NULL;
    }
#else
    if( p_audio->p_importer )
    {
        mp4sys_importer_close( p_audio->p_importer );
        p_audio->p_importer = NULL;
    }
#endif
    free( p_audio );
}
#endif

static void remove_mp4_hnd( hnd_t handle )
{
    mp4_hnd_t *p_mp4 = handle;
    if( !p_mp4 )
        return;
    lsmash_cleanup_summary( (lsmash_summary_t *)p_mp4->summary );
    lsmash_close_file( &p_mp4->file_param );
    lsmash_destroy_root( p_mp4->p_root );
    free( p_mp4->p_sei_buffer );
#if HAVE_ANY_AUDIO
    if( p_mp4->audio_hnd )
    {
        remove_audio_hnd( p_mp4->audio_hnd );
        p_mp4->audio_hnd = NULL;
    }
#endif
    free( p_mp4 );
}

#if HAVE_ANY_AUDIO

static int aac_init( mp4_audio_hnd_t *p_audio )
{
    p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
    audio_aac_info_t *aacinfo = p_audio->info->opaque;
    if( aacinfo )
        p_audio->has_sbr = aacinfo->has_sbr;
    else
        p_audio->has_sbr = 0; // SBR presence isn't specified, so assume implicit signaling
    p_audio->b_mdct = 1;
    if( p_audio->info->extradata && p_audio->info->extradata_size > 0 && p_audio->info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
    {
        lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG,
                                                                               LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( !specific )
        {
            MP4_LOG_ERROR( "failed to allocate memory for MPEG-4 audio specific info.\n" );
            return -1;
        }
        lsmash_mp4sys_decoder_parameters_t *param = (lsmash_mp4sys_decoder_parameters_t *)specific->data.structured;
        param->objectTypeIndication = MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3;
        param->streamType           = MP4SYS_STREAM_TYPE_AudioStream;
        if( lsmash_set_mp4sys_decoder_specific_info( param, p_audio->info->extradata, p_audio->info->extradata_size ) )
        {
            lsmash_destroy_codec_specific_data( specific );
            MP4_LOG_ERROR( "failed to set up decoder specific info for MPEG-4 audio.\n" );
            return -1;
        }
        int err = lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific );
        lsmash_destroy_codec_specific_data( specific );
        if( err )
        {
            MP4_LOG_ERROR( "failed to add MPEG-4 audio specific info.\n" );
            return -1;
        }
    }
    return 0;
}

static int mpeg12_layer_init( mp4_audio_hnd_t *p_audio )
{
    p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
    p_audio->b_mdct = !strcmp( p_audio->info->codec_name, "mp3" );
    lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG,
                                                                           LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
    if( !specific )
    {
        MP4_LOG_ERROR( "failed to allocate memory for MPEG-1/2 layer audio specific info.\n" );
        return -1;
    }
    lsmash_mp4sys_decoder_parameters_t *param = (lsmash_mp4sys_decoder_parameters_t *)specific->data.structured;
    if( p_audio->info->samplerate >= 32000 )
        param->objectTypeIndication = MP4SYS_OBJECT_TYPE_Audio_ISO_11172_3; /* Legacy Interface */
    else
        param->objectTypeIndication = MP4SYS_OBJECT_TYPE_Audio_ISO_13818_3; /* Legacy Interface */
    param->streamType = MP4SYS_STREAM_TYPE_AudioStream;
    if( lsmash_set_mp4sys_decoder_specific_info( param, p_audio->info->extradata, p_audio->info->extradata_size ) )
    {
        lsmash_destroy_codec_specific_data( specific );
        MP4_LOG_ERROR( "failed to set up decoder specific info for MPEG-1/2 layer audio.\n" );
        return -1;
    }
    int err = lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific );
    lsmash_destroy_codec_specific_data( specific );
    if( err )
    {
        MP4_LOG_ERROR( "failed to add MPEG-1/2 layer audio specific info.\n" );
        return -1;
    }
    return 0;
}

static int als_init( mp4_audio_hnd_t *p_audio )
{
    p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
    if( p_audio->info->extradata && p_audio->info->extradata_size > 0 && p_audio->info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
    {
        lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG,
                                                                               LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( !specific )
        {
            MP4_LOG_ERROR( "failed to allocate memory for Apple lossless audio specific info.\n" );
            return -1;
        }
        lsmash_mp4sys_decoder_parameters_t *param = (lsmash_mp4sys_decoder_parameters_t *)specific->data.structured;
        param->objectTypeIndication = MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3;
        param->streamType           = MP4SYS_STREAM_TYPE_AudioStream;
        if( lsmash_set_mp4sys_decoder_specific_info( param, p_audio->info->extradata, p_audio->info->extradata_size ) )
        {
            lsmash_destroy_codec_specific_data( specific );
            MP4_LOG_ERROR( "failed to set up decoder specific info for Apple lossless audio.\n" );
            return -1;
        }
        int err = lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific );
        lsmash_destroy_codec_specific_data( specific );
        if( err )
        {
            MP4_LOG_ERROR( "failed to add Apple lossless audio specific info data.\n" );
            return -1;
        }
    }
    return 0;
}

static int alac_init( mp4_audio_hnd_t *p_audio )
{
    p_audio->codec_type = ISOM_CODEC_TYPE_ALAC_AUDIO;
    if( p_audio->info->extradata && p_audio->info->extradata_size > 0 && p_audio->info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
    {
        lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_AUDIO_ALAC,
                                                                               LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED );
        if( !specific )
        {
            MP4_LOG_ERROR( "failed to allocate memory for Apple lossless audio specific info.\n" );
            return -1;
        }
        specific->data.unstructured = p_audio->info->extradata;
        specific->size              = p_audio->info->extradata_size;
        int err = lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific );
        specific->data.unstructured = NULL; /* Avoid double freeing extradata. */
        lsmash_destroy_codec_specific_data( specific );
        if( err )
        {
            MP4_LOG_ERROR( "failed to add Apple lossless audio specific info data.\n" );
            return -1;
        }
    }
    return 0;
}

#endif /* #if HAVE_ANY_AUDIO */

#if HAVE_AUDIO
static int audio_init( hnd_t handle, cli_output_opt_t *opt, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    // TODO: support other audio format
    hnd_t henc;
    int copy = 0;

    if( !strcmp( audio_enc, "copy" ) )
    {
        henc = x264_audio_copy_open( filters );
        copy = 1;
    }
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        char *codec_list[] = { "aac", "mp3", "ac3", "alac", "raw", "amrnb", "amrwb",
                               "pcm_f32be", "pcm_f32le", "pcm_f64be", "pcm_f64le",
                               "pcm_s16be", "pcm_s16le", "pcm_s24be", "pcm_s24le",
                               "pcm_s32be", "pcm_s32le", "pcm_s8", "pcm_u8", NULL };
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, codec_list, &used_enc );
        MP4_FAIL_IF_ERR( !encoder, "unable to select audio encoder.\n" );

        snprintf( audio_params, MAX_ARGS, "%s,codec=%s", audio_parameters, used_enc );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }

    MP4_FAIL_IF_ERR( !henc, "error opening audio encoder.\n" );
    mp4_hnd_t *p_mp4 = handle;
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd = calloc( 1, sizeof( mp4_audio_hnd_t ) );
    audio_info_t *info = p_audio->info = x264_audio_encoder_info( henc );
    p_audio->b_copy = copy;
    if( p_audio->b_copy )
        info->priming = opt->priming;

    p_audio->summary = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    if( !p_audio->summary )
    {
        MP4_LOG_ERROR( "failed to allocate memory for summary information of audio.\n" );
        goto error;
    }

    switch( p_mp4->major_brand )
    {
        case ISOM_BRAND_TYPE_3GP6 :
        case ISOM_BRAND_TYPE_3G2A :
            if( !strcmp( info->codec_name, "amrnb" ) )
                p_audio->codec_type = ISOM_CODEC_TYPE_SAMR_AUDIO;
            else if( !strcmp( info->codec_name, "amrwb" ) )
                p_audio->codec_type = ISOM_CODEC_TYPE_SAWB_AUDIO;
        case ISOM_BRAND_TYPE_MP42 :
            if( !strcmp( info->codec_name, "aac" ) )
            {
                if( aac_init( p_audio ) )
                    goto error;
                p_mp4->b_brand_m4a = 1;
            }
            else if( !strcmp( info->codec_name, "als" ) )
            {
                if( als_init( p_audio ) )
                    goto error;
                p_mp4->b_brand_m4a = 1;
            }
            else if( ( !strcmp( info->codec_name, "mp3" ) || !strcmp( info->codec_name, "mp2" ) || !strcmp( info->codec_name, "mp1" ) )
                     && info->samplerate >= 16000 ) /* freq <16khz is MPEG-2.5. */
            {
                if( mpeg12_layer_init( p_audio ) )
                    goto error;
            }
            else if( !strcmp( info->codec_name, "ac3" ) )
            {
                p_audio->codec_type = ISOM_CODEC_TYPE_AC_3_AUDIO;
                p_audio->b_mdct = 1;
                if( !info->extradata || info->extradata_size == 0 )
                {
                    MP4_LOG_ERROR( "no frame to create AC-3 specific info.\n" );
                    goto error;
                }
                else if( info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
                {
                    /* from lavf input
                     * Create AC3SpecificBox from AC-3 syncframe by L-SMASH's AC-3 parser. */
                    lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_AUDIO_AC_3,
                                                                                           LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
                    if( !specific )
                    {
                        MP4_LOG_ERROR( "failed to allocate memory for AC-3 specific info.\n" );
                        goto error;
                    }
                    lsmash_ac3_specific_parameters_t *param = (lsmash_ac3_specific_parameters_t *)specific->data.structured;
                    if( lsmash_setup_ac3_specific_parameters_from_syncframe( param, info->extradata, info->extradata_size ) )
                    {
                        MP4_LOG_ERROR( "failed to set up AC-3 specific info.\n" );
                        goto error;
                    }
                    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific ) )
                    {
                        lsmash_destroy_codec_specific_data( specific );
                        MP4_LOG_ERROR( "failed to add AC-3 specific info data.\n" );
                        goto error;
                    }
                    lsmash_destroy_codec_specific_data( specific );
                }
            }
            else if( !strcmp( info->codec_name, "eac3" ) )
            {
                p_audio->codec_type = ISOM_CODEC_TYPE_EC_3_AUDIO;
                p_audio->b_mdct = 1;
                if( !info->extradata || info->extradata_size == 0 )
                {
                    MP4_LOG_ERROR( "no frame to create Enhanced AC-3 specific info.\n" );
                    goto error;
                }
                else if( info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
                {
                    /* from lavf input
                     * Create EC3SpecificBox from Enhanced AC-3 syncframes by L-SMASH's Enhanced AC-3 parser. */
                    lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_AUDIO_EC_3,
                                                                                           LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
                    if( !specific )
                    {
                        MP4_LOG_ERROR( "failed to allocate memory for Enhanced AC-3 specific info.\n" );
                        goto error;
                    }
                    lsmash_eac3_specific_parameters_t *param = (lsmash_eac3_specific_parameters_t *)specific->data.structured;
                    if( lsmash_setup_eac3_specific_parameters_from_frame( param, info->extradata, info->extradata_size ) )
                    {
                        lsmash_destroy_codec_specific_data( specific );
                        MP4_LOG_ERROR( "failed to set up Enhanced AC-3 specific info.\n" );
                        goto error;
                    }
                    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific ) )
                    {
                        lsmash_destroy_codec_specific_data( specific );
                        MP4_LOG_ERROR( "failed to add Enhanced AC-3 specific info data.\n" );
                        goto error;
                    }
                    lsmash_destroy_codec_specific_data( specific );
                }
            }
            else if( !strcmp( info->codec_name, "alac" ) )
            {
                if( alac_init( p_audio ) )
                    goto error;
                p_mp4->b_brand_m4a = 1;
            }
            else if( !strcmp( info->codec_name, "dca" ) )
            {
                audio_dts_info_t *dts_info = info->opaque;
                if( dts_info )
                    /* from L-SMASH importer */
                    p_audio->codec_type = dts_info->coding_name;
                else if( info->extradata && info->extradata_size > 0 && info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
                {
                    /* from lavf input
                     * Create DTSSpecificBox from DTS audio frame by L-SMASH's DTS parser. */
                    lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_AUDIO_DTS,
                                                                                           LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
                    if( !specific )
                    {
                        MP4_LOG_ERROR( "failed to allocate memory for DTS specific info.\n" );
                        goto error;
                    }
                    lsmash_dts_specific_parameters_t *param = (lsmash_dts_specific_parameters_t *)specific->data.structured;
                    if( lsmash_setup_dts_specific_parameters_from_frame( param, info->extradata, info->extradata_size ) )
                    {
                        MP4_LOG_ERROR( "failed to parse DTS audio frame.\n" );
                        goto error;
                    }

                    p_audio->codec_type = lsmash_dts_get_codingname( param );

                    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific ) )
                    {
                        lsmash_destroy_codec_specific_data( specific );
                        MP4_LOG_ERROR( "failed to add DTS specific info data.\n" );
                        goto error;
                    }
                    lsmash_destroy_codec_specific_data( specific );
                }
                else
                {
                    MP4_LOG_ERROR( "no frame to create DTS specific info.\n" );
                    goto error;
                }
                p_audio->b_mdct = !!lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_DTSE_AUDIO );
            }
            break;
        case ISOM_BRAND_TYPE_QT :
            if( !strcmp( info->codec_name, "aac" ) )
            {
                if( aac_init( p_audio ) )
                    goto error;
            }
            else if( !strcmp( info->codec_name, "alac" ) )
            {
                if( alac_init( p_audio ) )
                    goto error;
            }
            else if( p_audio->b_copy )
                break;      /* We haven't supported LPCM copying yet. */
            else
            {
                typedef struct {
                    const char *name;
                    lsmash_codec_type_t codec_type;
                    lsmash_qt_audio_format_specific_flags_t lpcm;
                } qt_lpcm_detail;

                const qt_lpcm_detail qt_lpcm_table[] = {
#ifdef WORDS_BIGENDIAN
                    { "raw",        QT_CODEC_TYPE_TWOS_AUDIO, { QT_AUDIO_FORMAT_FLAG_BIG_ENDIAN | QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
#else
                    { "raw",        QT_CODEC_TYPE_SOWT_AUDIO, { QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
#endif
                    { "pcm_f32be",  QT_CODEC_TYPE_FL32_AUDIO, { QT_AUDIO_FORMAT_FLAG_FLOAT | QT_AUDIO_FORMAT_FLAG_BIG_ENDIAN } },
                    { "pcm_f32le",  QT_CODEC_TYPE_FL32_AUDIO, { QT_AUDIO_FORMAT_FLAG_FLOAT } },
                    { "pcm_f64be",  QT_CODEC_TYPE_FL64_AUDIO, { QT_AUDIO_FORMAT_FLAG_FLOAT | QT_AUDIO_FORMAT_FLAG_BIG_ENDIAN } },
                    { "pcm_f64le",  QT_CODEC_TYPE_FL64_AUDIO, { QT_AUDIO_FORMAT_FLAG_FLOAT } },
                    { "pcm_s16be",  QT_CODEC_TYPE_TWOS_AUDIO, { QT_AUDIO_FORMAT_FLAG_BIG_ENDIAN | QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_s16le",  QT_CODEC_TYPE_SOWT_AUDIO, { QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_s24be",  QT_CODEC_TYPE_IN24_AUDIO, { QT_AUDIO_FORMAT_FLAG_BIG_ENDIAN | QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_s24le",  QT_CODEC_TYPE_IN24_AUDIO, { QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_s32be",  QT_CODEC_TYPE_IN32_AUDIO, { QT_AUDIO_FORMAT_FLAG_BIG_ENDIAN | QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_s32le",  QT_CODEC_TYPE_IN32_AUDIO, { QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_s8",     QT_CODEC_TYPE_TWOS_AUDIO, { QT_AUDIO_FORMAT_FLAG_SIGNED_INTEGER } },
                    { "pcm_u8",     QT_CODEC_TYPE_RAW_AUDIO,  { 0 } }
                };

                for( int i = 0; i < sizeof(qt_lpcm_table)/sizeof(qt_lpcm_detail); i++ )
                    if( !strcmp( info->codec_name, qt_lpcm_table[i].name ) )
                    {
                        p_audio->codec_type = qt_lpcm_table[i].codec_type;
                        lsmash_codec_specific_t *specific = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_AUDIO_FORMAT_SPECIFIC_FLAGS,
                                                                                               LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
                        if( !specific )
                        {
                            MP4_LOG_ERROR( "failed to allocate memory for LPCM format specific flags.\n" );
                            goto error;
                        }
                        *(lsmash_qt_audio_format_specific_flags_t *)specific->data.structured = qt_lpcm_table[i].lpcm;
                        if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, specific ) )
                        {
                            lsmash_destroy_codec_specific_data( specific );
                            MP4_LOG_ERROR( "failed to add LPCM specific data.\n" );
                            goto error;
                        }
                        lsmash_destroy_codec_specific_data( specific );
                        break;
                    }
            }
            break;
        default :
            break;
    }

    if( lsmash_check_codec_type_identical( p_audio->codec_type, LSMASH_CODEC_TYPE_UNSPECIFIED ) )
    {
        MP4_LOG_ERROR( "unsupported audio codec '%s'.\n", info->codec_name );
        goto error;
    }

    if( info->extradata_type == EXTRADATA_TYPE_LSMASH )
    {
        uint32_t num_extensions = info->extradata_size / sizeof(lsmash_codec_specific_t *);
        for( uint32_t i = 0; i < num_extensions; i++ )
        {
            lsmash_codec_specific_t **extradata = (lsmash_codec_specific_t **)info->extradata;
            if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_audio->summary, extradata[i] ) )
            {
                MP4_LOG_ERROR( "failed to add CODEC specific info extension.\n" );
                goto error;
            }
        }
    }

    p_audio->encoder = henc;

    return 1;

error:
    x264_audio_encoder_close( henc );
    free( p_mp4->audio_hnd );
    p_mp4->audio_hnd = NULL;

    return -1;
}
#endif /* #if HAVE_AUDIO */

#if HAVE_ANY_AUDIO
static int set_param_audio( mp4_hnd_t* p_mp4, uint64_t i_media_timescale, lsmash_track_mode track_mode )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;

    /* Create a audio track. */
    p_audio->i_track = lsmash_create_track( p_mp4->p_root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    MP4_FAIL_IF_ERR( !p_audio->i_track, "failed to create a audio track.\n" );

#if HAVE_AUDIO
    if( p_mp4->major_brand == ISOM_BRAND_TYPE_QT
     || lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_ALAC_AUDIO ) )
        MP4_FAIL_IF_ERR( set_channel_layout( p_audio ), "failed to set up channel layout.\n" );
    p_audio->summary->sample_type      = p_audio->codec_type;
    p_audio->summary->max_au_length    = ( 1 << 13 ) - 1;
    p_audio->summary->frequency        = p_audio->info->samplerate;
    p_audio->summary->channels         = p_audio->info->channels;
    p_audio->summary->sample_size      = p_audio->info->depth;
    p_audio->summary->samples_in_frame = p_audio->info->framelen;
    if( lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
    {
        if( !strcmp( p_audio->info->codec_name, "als" ) )
            p_audio->summary->aot = MP4A_AUDIO_OBJECT_TYPE_ALS;
        else
            p_audio->summary->aot = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
        p_audio->summary->sbr_mode = p_audio->has_sbr ? MP4A_AAC_SBR_BACKWARD_COMPATIBLE : MP4A_AAC_SBR_NOT_SPECIFIED;
    }
/*    else if( lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_SAMR_AUDIO )
          || lsmash_check_codec_type_identical( p_audio->codec_type, ISOM_CODEC_TYPE_SAWB_AUDIO ) )
    {
            MP4_FAIL_IF_ERR( mp4sys_amr_create_damr( p_audio->summary ),
                             "failed to create AMR specific info.\n" );
    }*/
#else
    /*
     * NOTE: Retrieve audio summary, which will be used for lsmash_add_sample_entry() as audio parameters.
     * Currently, our ADTS importer never recognize SBR (HE-AAC).
     * Thus, in this sample, if the source contains SBR, it'll be coded as implicit signaling for SBR.
     * If you want to use explicit signaling of SBR, change the sbr_mode in summary and
     * call lsmash_setup_AudioSpecificConfig() to reconstruct ASC within the summary.
     */
    /*
     * WARNING: If you wish to allocate summary in your code, you have to allocate ASC too,
     * and never use lsmash_cleanup_audio_summary(), unless L-SMASH is compiled integrated with your code.
     * Because malloc() and free() have to be used as pair from EXACTLY SAME standard C library.
     * Otherwise you may cause bugs which you hardly call to mind.
     */
    p_audio->summary = (lsmash_audio_summary_t *)mp4sys_duplicate_summary( p_audio->p_importer, 1 );
    MP4_FAIL_IF_ERR( !p_audio->summary, "failed to duplicate summary information.\n" );
    p_audio->codec_type = p_audio->summary->sample_type;
#endif /* #if HAVE_AUDIO #else */

    /* Set sound track parameters. */
    lsmash_track_parameters_t track_param;
    lsmash_initialize_track_parameters( &track_param );
    track_param.mode = track_mode;
    MP4_FAIL_IF_ERR( lsmash_set_track_parameters( p_mp4->p_root, p_audio->i_track, &track_param ),
                     "failed to set track parameters for audio.\n" );
    p_audio->i_video_timescale = i_media_timescale;

    /* Set sound media parameters. */
    lsmash_media_parameters_t media_param;
    lsmash_initialize_media_parameters( &media_param );
    media_param.timescale = p_audio->summary->frequency;
    media_param.ISO_language = lsmash_pack_iso_language( p_mp4->psz_language );
    media_param.media_handler_name = "L-SMASH Sound Media Handler";
    media_param.roll_grouping = !!p_audio->info->priming;
    if( p_mp4->b_brand_qt )
        media_param.data_handler_name = "L-SMASH URL Data Handler";
    MP4_FAIL_IF_ERR( lsmash_set_media_parameters( p_mp4->p_root, p_audio->i_track, &media_param ),
                     "failed to set media parameters for audio.\n" );

    p_audio->i_sample_entry = lsmash_add_sample_entry( p_mp4->p_root, p_audio->i_track, p_audio->summary );
    MP4_FAIL_IF_ERR( !p_audio->i_sample_entry,
                     "failed to add sample_entry for audio.\n" );

    return 0;
}

static int write_audio_frames( mp4_hnd_t *p_mp4, double video_dts, int finish )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    assert( p_audio );

#if HAVE_AUDIO
    audio_packet_t *frame;
#endif

    /* FIXME: This is just a sample implementation. */
    for(;;)
    {
        uint64_t audio_timestamp = (uint64_t)p_audio->i_numframe * p_audio->summary->samples_in_frame;
        /*
         * means while( audio_dts <= video_dts )
         * FIXME: I wonder if there's any way more effective.
         */

        if( !video_dts && p_mp4->b_fragments && !finish )
        {
            lsmash_edit_t edit;
            edit.duration   = ISOM_EDIT_DURATION_UNKNOWN32;     /* QuickTime doesn't support 64bit duration. */
            edit.start_time = p_audio->info->priming;
            edit.rate       = ISOM_EDIT_MODE_NORMAL;
            MP4_LOG_IF_ERR( lsmash_create_explicit_timeline_map( p_mp4->p_root, p_audio->i_track, edit ),
                            "failed to set timeline map for audio.\n" );
        }

        if( !finish && ((audio_timestamp / (double)p_audio->summary->frequency > video_dts) || !video_dts) )
            break;

        /* read a audio frame */
#if HAVE_AUDIO
        if( finish )
            frame = x264_audio_encoder_finish( p_audio->encoder );
        else if( !(frame = x264_audio_encode_frame( p_audio->encoder )) )
        {
            finish = 1;
            continue;
        }

        if( !frame )
            break;

        lsmash_sample_t *p_sample = lsmash_create_sample( frame->size );
        MP4_FAIL_IF_ERR( !p_sample,
                         "failed to create a audio sample data.\n" );
        memcpy( p_sample->data, frame->data, frame->size );
        x264_audio_free_frame( p_audio->encoder, frame );
        p_sample->prop.pre_roll.distance = p_audio->b_mdct;
#else
        /* FIXME: mp4sys_importer_get_access_unit() returns 1 if there're any changes in stream's properties.
           If you want to support them, you have to retrieve summary again, and make some operation accordingly. */
        lsmash_sample_t *p_sample = lsmash_create_sample( p_audio->summary->max_au_length );
        MP4_FAIL_IF_ERR( !p_sample,
                         "failed to create a audio sample data.\n" );
        MP4_FAIL_IF_ERR( mp4sys_importer_get_access_unit( p_audio->p_importer, 1, p_sample ),
                         "failed to retrieve frame data from importer.\n" );
        if( p_sample->length == 0 )
        {
            p_audio->last_delta = mp4sys_importer_get_last_delta( p_audio->p_importer, 1 );
            lsmash_delete_sample( p_sample );
            break; /* end of stream */
        }
        else
            p_audio->last_delta = p_audio->summary->samples_in_frame;
#endif
        p_sample->dts = p_sample->cts = audio_timestamp;
        p_sample->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
        p_sample->index = p_audio->i_sample_entry;
        MP4_FAIL_IF_ERR( lsmash_append_sample( p_mp4->p_root, p_audio->i_track, p_sample ),
                         "failed to append a audio sample.\n" );

        p_audio->i_numframe++;
    }
    return 0;
}

static int close_file_audio( mp4_hnd_t* p_mp4, double actual_duration )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    double media_duration = actual_duration / p_mp4->i_movie_timescale + (double)p_audio->info->priming / p_audio->summary->frequency;
    MP4_LOG_IF_ERR( ( write_audio_frames( p_mp4, media_duration, 0 ) || // FIXME: I wonder why is this needed?
                      write_audio_frames( p_mp4, 0, 1 ) ),
                    "failed to flush audio frame(s).\n" );
    uint32_t last_delta;
    if( lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_RAW_AUDIO )
     || lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_SOWT_AUDIO )
     || lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_TWOS_AUDIO )
     || lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_FL64_AUDIO )
     || lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_FL32_AUDIO )
     || lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_IN24_AUDIO )
     || lsmash_check_codec_type_identical( p_audio->codec_type, QT_CODEC_TYPE_IN32_AUDIO ) )
        last_delta = 1;     /* Actual sample duration of each LPCMFrame is one. */
    else
#if HAVE_AUDIO
        last_delta = p_audio->info->last_delta;
#else
        last_delta = p_audio->last_delta;
#endif
    MP4_LOG_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_audio->i_track, last_delta ),
                    "failed to flush the rest of audio samples.\n" );
    actual_duration = ((p_audio->i_numframe - 1) * p_audio->summary->samples_in_frame) + last_delta - p_audio->info->priming;
    if( actual_duration )
        actual_duration *= (double)p_mp4->i_movie_timescale / p_audio->summary->frequency;
    lsmash_edit_t edit;
    edit.duration   = actual_duration;
    edit.start_time = p_audio->info->priming;
    edit.rate       = ISOM_EDIT_MODE_NORMAL;
    if( !p_mp4->b_fragments )
    {
        MP4_LOG_IF_ERR( lsmash_create_explicit_timeline_map( p_mp4->p_root, p_audio->i_track, edit ),
                        "failed to set timeline map for audio.\n" );
    }
    else if( !p_mp4->b_stdout )
        MP4_LOG_IF_ERR( lsmash_modify_explicit_timeline_map( p_mp4->p_root, p_audio->i_track, 1, edit ),
                        "failed to update timeline map for audio.\n" );
    return 0;
}
#endif /* #if HAVE_ANY_AUDIO */

typedef struct {
    int64_t start;
    int no_progress;
} remux_cb_param;

int remux_callback( void* param, uint64_t done, uint64_t total )
{
    remux_cb_param *cb_param = (remux_cb_param*)param;
    if( cb_param->no_progress && done != total )
        return 0;
    int64_t elapsed = x264_mdate() - cb_param->start;
    double byterate = done / ( elapsed / 1000000. );
    fprintf( stderr, "remux [%5.2lf%%], %"PRIu64"/%"PRIu64" KiB, %u KiB/s, ",
        done*100./total, done/1024, total/1024, (unsigned)byterate/1024 );
    if( done == total )
    {
        unsigned sec = (unsigned)( elapsed / 1000000 );
        fprintf( stderr, "total elapsed %u:%02u:%02u\n\n", sec/3600, (sec/60)%60, sec%60 );
    }
    else
    {
        unsigned eta = (unsigned)( (total - done) / byterate );
        fprintf( stderr, "eta %u:%02u:%02u\r", eta/3600, (eta/60)%60, eta%60 );
    }
    fflush( stderr ); // needed in windows
    return 0;
}

/*******************/

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    mp4_hnd_t *p_mp4 = handle;

    if( !p_mp4 )
        return 0;

    if( p_mp4->p_root )
    {
        double actual_duration = 0;     /* FIXME: This may be inside block of "if( p_mp4->i_track )" if audio does not use this. */
        if( p_mp4->i_track )
        {
            /* Flush the rest of samples and add the last sample_delta. */
            uint32_t last_delta = largest_pts - second_largest_pts;
            MP4_LOG_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_mp4->i_track, (last_delta ? last_delta : 1) * p_mp4->i_time_inc ),
                            "failed to flush the rest of samples.\n" );

            if( p_mp4->i_movie_timescale != 0 && p_mp4->i_video_timescale != 0 )    /* avoid zero division */
                actual_duration = ((double)((largest_pts + last_delta) * p_mp4->i_time_inc) / p_mp4->i_video_timescale) * p_mp4->i_movie_timescale;
            else
                MP4_LOG_ERROR( "timescale is broken.\n" );

            /*
             * Declare the explicit time-line mapping.
             * A segment_duration is given by movie timescale, while a media_time that is the start time of this segment
             * is given by not the movie timescale but rather the media timescale.
             * The reason is that ISO media have two time-lines, presentation and media time-line,
             * and an edit maps the presentation time-line to the media time-line.
             * According to QuickTime file format specification and the actual playback in QuickTime Player,
             * if the Edit Box doesn't exist in the track, the ratio of the summation of sample durations and track's duration becomes
             * the track's media_rate so that the entire media can be used by the track.
             * So, we add Edit Box here to avoid this implicit media_rate could distort track's presentation timestamps slightly.
             * Note: Any demuxers should follow the Edit List Box if it exists.
             */
            lsmash_edit_t edit;
            edit.duration   = actual_duration;
            edit.start_time = p_mp4->i_first_cts;
            edit.rate       = ISOM_EDIT_MODE_NORMAL;
            if( !p_mp4->b_fragments )
            {
                MP4_LOG_IF_ERR( lsmash_create_explicit_timeline_map( p_mp4->p_root, p_mp4->i_track, edit ),
                                "failed to set timeline map for video.\n" );
            }
            else if( !p_mp4->b_stdout )
                MP4_LOG_IF_ERR( lsmash_modify_explicit_timeline_map( p_mp4->p_root, p_mp4->i_track, 1, edit ),
                                "failed to update timeline map for video.\n" );
        }

#if HAVE_ANY_AUDIO
        MP4_LOG_IF_ERR( p_mp4->audio_hnd && p_mp4->audio_hnd->i_track && close_file_audio( p_mp4, actual_duration ),
                        "failed to close audio.\n" );
#endif

        if( p_mp4->psz_chapter && (p_mp4->major_brand != ISOM_BRAND_TYPE_QT) )
            MP4_LOG_IF_ERR( lsmash_set_tyrant_chapter( p_mp4->p_root, p_mp4->psz_chapter, p_mp4->b_add_bom ), "failed to set chapter list.\n" );

        if( !p_mp4->b_no_remux )
        {
            remux_cb_param cb_param;
            cb_param.no_progress = p_mp4->b_no_progress;
            cb_param.start = x264_mdate();
            lsmash_adhoc_remux_t remux_info;
            remux_info.func = remux_callback;
            remux_info.buffer_size = 4*1024*1024; // 4MiB
            remux_info.param = &cb_param;
            MP4_LOG_IF_ERR( lsmash_finish_movie( p_mp4->p_root, &remux_info ), "failed to finish movie.\n" );
        }
        else
            MP4_LOG_IF_ERR( lsmash_finish_movie( p_mp4->p_root, NULL ), "failed to finish movie.\n" );
    }

    remove_mp4_hnd( p_mp4 ); /* including lsmash_destroy_root( p_mp4->p_root ); */

    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    *p_handle = NULL;

    int b_regular = strcmp( psz_filename, "-" );
    b_regular = b_regular && x264_is_regular_file_path( psz_filename );
    if( b_regular )
    {
        FILE *fh = x264_fopen( psz_filename, "wb" );
        MP4_FAIL_IF_ERR( !fh, "cannot open output file `%s'.\n", psz_filename );
        b_regular = x264_is_regular_file( fh );
        fclose( fh );
    }

    mp4_hnd_t *p_mp4 = calloc( 1, sizeof(mp4_hnd_t) );
    MP4_FAIL_IF_ERR( !p_mp4, "failed to allocate memory for muxer information.\n" );

    p_mp4->b_dts_compress = opt->use_dts_compress;

    if( opt->mux_mov )
    {
        p_mp4->major_brand = ISOM_BRAND_TYPE_QT;
        p_mp4->b_brand_qt = 1;
    }
    else if( opt->mux_3gp )
    {
        p_mp4->major_brand = ISOM_BRAND_TYPE_3GP6;
        p_mp4->i_brand_3gpp = 1;
    }
    else if( opt->mux_3g2 )
    {
        p_mp4->major_brand = ISOM_BRAND_TYPE_3G2A;
        p_mp4->i_brand_3gpp = 2;
    }
    else
        p_mp4->major_brand = ISOM_BRAND_TYPE_MP42;

    if( opt->chapter )
    {
        p_mp4->psz_chapter = opt->chapter;
        p_mp4->b_add_bom   = opt->add_bom;
        FILE *fh = x264_fopen( p_mp4->psz_chapter, "rb" );
        MP4_FAIL_IF_ERR_EX( !fh, "can't open `%s'\n", p_mp4->psz_chapter );
        fclose( fh );
    }
    p_mp4->psz_language         = opt->language;
    p_mp4->b_no_pasp            = opt->no_sar;
    p_mp4->b_no_remux           = opt->no_remux;
    p_mp4->i_display_width      = opt->display_width * (1<<16);
    p_mp4->i_display_height     = opt->display_height * (1<<16);
    p_mp4->b_force_display_size = p_mp4->i_display_height || p_mp4->i_display_height;
    p_mp4->scale_method         = p_mp4->b_force_display_size ? ISOM_SCALE_METHOD_FILL : ISOM_SCALE_METHOD_MEET;
    p_mp4->b_use_recovery = 0; // we don't really support recovery
    p_mp4->b_fragments    = !b_regular || opt->fragments;
    p_mp4->b_stdout       = !strcmp( psz_filename, "-" );

    p_mp4->p_root = lsmash_create_root();
    MP4_FAIL_IF_ERR_EX( !p_mp4->p_root, "failed to create root.\n" );

    MP4_FAIL_IF_ERR_EX( lsmash_open_file( psz_filename, 0, &p_mp4->file_param ) < 0, "failed to open an output file.\n" );
    if( p_mp4->b_fragments )
        p_mp4->file_param.mode |= LSMASH_FILE_MODE_FRAGMENTED;

    p_mp4->summary = (lsmash_video_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_VIDEO );
    MP4_FAIL_IF_ERR_EX( !p_mp4->summary,
                        "failed to allocate memory for summary information of video.\n" );
    p_mp4->summary->sample_type = ISOM_CODEC_TYPE_AVC1_VIDEO;

#if HAVE_ANY_AUDIO
#if HAVE_AUDIO
    MP4_FAIL_IF_ERR_EX( audio_init( p_mp4, opt, audio_filters, audio_enc, audio_params ) < 0, "unable to init audio output.\n" );
#else
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd = (mp4_audio_hnd_t *)malloc( sizeof(mp4_audio_hnd_t) );
    MP4_FAIL_IF_ERR_EX( !p_audio, "failed to allocate memory for audio muxing information.\n" );
    memset( p_audio, 0, sizeof(mp4_audio_hnd_t) );
    p_audio->p_importer = mp4sys_importer_open( "x264_audio_test.adts", "auto" );
    if( !p_audio->p_importer )
    {
        free( p_audio );
        p_mp4->audio_hnd = NULL;
    }
#endif
    if( !p_mp4->audio_hnd )
        MP4_LOG_INFO( "audio muxing feature is disabled.\n" );
#endif
    p_mp4->b_no_progress = opt->no_progress;

    *p_handle = p_mp4;

    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t i_media_timescale;

    set_recovery_param( p_mp4, p_param );

    p_mp4->i_delay_frames = p_param->i_bframe ? (p_param->i_bframe_pyramid ? 2 : 1) : 0;
    p_mp4->i_dts_compress_multiplier = p_mp4->b_dts_compress * p_mp4->i_delay_frames + 1;

    i_media_timescale = (uint64_t)p_param->i_timebase_den * p_mp4->i_dts_compress_multiplier;
    p_mp4->i_time_inc = (uint64_t)p_param->i_timebase_num * p_mp4->i_dts_compress_multiplier;
    MP4_FAIL_IF_ERR( i_media_timescale > UINT32_MAX, "MP4 media timescale %"PRIu64" exceeds maximum\n", i_media_timescale );

    /* Select brands. */
    lsmash_brand_type brands[11] = { 0 };
    uint32_t minor_version = 0;
    uint32_t brand_count = 0;
    if( p_mp4->b_brand_qt )
    {
        brands[brand_count++] = ISOM_BRAND_TYPE_QT;
        p_mp4->i_brand_3gpp = 0;
        p_mp4->b_brand_m4a = 0;
        p_mp4->b_use_recovery = 0;      /* Disable sample grouping. */
    }
    else
    {
        if( p_mp4->i_brand_3gpp >= 1 )
            brands[brand_count++] = ISOM_BRAND_TYPE_3GP6;
        if( p_mp4->i_brand_3gpp == 2 )
        {
            brands[brand_count++] = ISOM_BRAND_TYPE_3G2A;
            minor_version = 0x00010000;
        }
        brands[brand_count++] = ISOM_BRAND_TYPE_MP42;
        brands[brand_count++] = ISOM_BRAND_TYPE_MP41;
        if( p_mp4->b_brand_m4a )
        {
            brands[brand_count++] = ISOM_BRAND_TYPE_M4V;
            brands[brand_count++] = ISOM_BRAND_TYPE_M4A;
        }
        brands[brand_count++] = ISOM_BRAND_TYPE_ISOM;
#if HAVE_ANY_AUDIO
        if( p_mp4->audio_hnd || p_mp4->b_use_recovery )
#else
        if( p_mp4->b_use_recovery )
#endif
        {
            brands[brand_count++] = ISOM_BRAND_TYPE_AVC1;   /* sdtp, sgpd, sbgp and visual roll recovery grouping */
#if HAVE_ANY_AUDIO
            if( p_mp4->audio_hnd )
                brands[brand_count++] = ISOM_BRAND_TYPE_ISO2;   /* audio roll recovery grouping */
#endif
            if( p_param->b_open_gop )
                brands[brand_count++] = ISOM_BRAND_TYPE_ISO6;   /* cslg and visual random access grouping */
        }
    }

    /* Set file */
    lsmash_file_parameters_t *file_param = &p_mp4->file_param;
    file_param->major_brand   = brands[0];
    file_param->brands        = brands;
    file_param->brand_count   = brand_count;
    file_param->minor_version = 0;
    MP4_FAIL_IF_ERR( !lsmash_set_file( p_mp4->p_root, file_param ), "failed to add an output file into a ROOT.\n" );

    /* Set movie parameters. */
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
//    movie_param.minor_version = minor_version;
    MP4_FAIL_IF_ERR( lsmash_set_movie_parameters( p_mp4->p_root, &movie_param ),
                     "failed to set movie parameters.\n" );
    p_mp4->i_movie_timescale = lsmash_get_movie_timescale( p_mp4->p_root );
    MP4_FAIL_IF_ERR( !p_mp4->i_movie_timescale, "movie timescale is broken.\n" );

    if( p_mp4->b_brand_m4a )
    {
        lsmash_itunes_metadata_t itunes_metadata;
        itunes_metadata.item         = ITUNES_METADATA_ITEM_ENCODING_TOOL;
        itunes_metadata.type         = ITUNES_METADATA_TYPE_NONE;
        itunes_metadata.value.string = "x264 "X264_POINTVER;
        itunes_metadata.meaning      = NULL;
        itunes_metadata.name         = NULL;
        MP4_FAIL_IF_ERR( lsmash_set_itunes_metadata( p_mp4->p_root, itunes_metadata ), "failed to set metadata\n" );
    }

    /* Create a video track. */
    p_mp4->i_track = lsmash_create_track( p_mp4->p_root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    MP4_FAIL_IF_ERR( !p_mp4->i_track, "failed to create a video track.\n" );

    p_mp4->summary->width = p_param->i_width;
    p_mp4->summary->height = p_param->i_height;
    if( !p_mp4->b_force_display_size )
    {
        p_mp4->i_display_width = p_param->i_width << 16;
        p_mp4->i_display_height = p_param->i_height << 16;
    }
    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height )
    {
        if( !p_mp4->b_force_display_size )
        {
            double sar = (double)p_param->vui.i_sar_width / p_param->vui.i_sar_height;
            if( sar > 1.0 )
                p_mp4->i_display_width *= sar;
            else
                p_mp4->i_display_height /= sar;
        }
        if( !p_mp4->b_no_pasp )
        {
            p_mp4->summary->par_h = p_param->vui.i_sar_width;
            p_mp4->summary->par_v = p_param->vui.i_sar_height;
        }
    }
    p_mp4->summary->color.primaries_index = p_param->vui.i_colorprim;
    p_mp4->summary->color.transfer_index  = p_param->vui.i_transfer;
    p_mp4->summary->color.matrix_index    = p_param->vui.i_colmatrix >= 0 ? p_param->vui.i_colmatrix : ISOM_MATRIX_INDEX_UNSPECIFIED;
    p_mp4->summary->color.full_range      = p_param->vui.b_fullrange >= 0 ? p_param->vui.b_fullrange : 0;

    /* Set video track parameters. */
    lsmash_track_parameters_t track_param;
    lsmash_initialize_track_parameters( &track_param );
    lsmash_track_mode track_mode = ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW;
    if( p_mp4->b_brand_qt )
        track_mode |= QT_TRACK_IN_POSTER;
    track_param.mode = track_mode;
    track_param.display_width = p_mp4->i_display_width;
    track_param.display_height = p_mp4->i_display_height;
    track_param.aperture_modes = p_mp4->b_brand_qt && !p_mp4->b_no_pasp;
    MP4_FAIL_IF_ERR( lsmash_set_track_parameters( p_mp4->p_root, p_mp4->i_track, &track_param ),
                     "failed to set track parameters for video.\n" );

    /* Set video media parameters. */
    lsmash_media_parameters_t media_param;
    lsmash_initialize_media_parameters( &media_param );
    media_param.timescale = i_media_timescale;
    media_param.ISO_language = lsmash_pack_iso_language( p_mp4->psz_language );
    media_param.media_handler_name = "L-SMASH Video Media Handler";
    if( p_mp4->b_brand_qt )
        media_param.data_handler_name = "L-SMASH URL Data Handler";
    if( p_mp4->major_brand != ISOM_BRAND_TYPE_QT )
    {
        media_param.roll_grouping = p_param->b_intra_refresh;
        media_param.rap_grouping = p_param->b_open_gop;
    }
    MP4_FAIL_IF_ERR( lsmash_set_media_parameters( p_mp4->p_root, p_mp4->i_track, &media_param ),
                     "failed to set media parameters for video.\n" );
    p_mp4->i_video_timescale = lsmash_get_media_timescale( p_mp4->p_root, p_mp4->i_track );
    MP4_FAIL_IF_ERR( !p_mp4->i_video_timescale, "media timescale for video is broken.\n" );

#if HAVE_ANY_AUDIO
    MP4_FAIL_IF_ERR( p_mp4->audio_hnd && set_param_audio( p_mp4, i_media_timescale, track_mode ),
                     "failed to set audio param\n" );
#endif

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    mp4_hnd_t *p_mp4 = handle;

    uint32_t sps_size = p_nal[0].i_payload - H264_NALU_LENGTH_SIZE;
    uint32_t pps_size = p_nal[1].i_payload - H264_NALU_LENGTH_SIZE;
    uint32_t sei_size = p_nal[2].i_payload;

    uint8_t *sps = p_nal[0].p_payload + H264_NALU_LENGTH_SIZE;
    uint8_t *pps = p_nal[1].p_payload + H264_NALU_LENGTH_SIZE;
    uint8_t *sei = p_nal[2].p_payload;

    lsmash_codec_specific_t *cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264,
                                                                     LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );

    lsmash_h264_specific_parameters_t *param = (lsmash_h264_specific_parameters_t *)cs->data.structured;
    param->lengthSizeMinusOne = H264_NALU_LENGTH_SIZE - 1;

    /* SPS
     * The remaining parameters are automatically set by SPS. */
    if( lsmash_append_h264_parameter_set( param, H264_PARAMETER_SET_TYPE_SPS, sps, sps_size ) )
    {
        MP4_LOG_ERROR( "failed to append SPS.\n" );
        return -1;
    }

    /* PPS */
    if( lsmash_append_h264_parameter_set( param, H264_PARAMETER_SET_TYPE_PPS, pps, pps_size ) )
    {
        MP4_LOG_ERROR( "failed to append PPS.\n" );
        return -1;
    }

    if( lsmash_add_codec_specific_data( (lsmash_summary_t *)p_mp4->summary, cs ) )
    {
        MP4_LOG_ERROR( "failed to add H.264 specific info.\n" );
        return -1;
    }

    lsmash_destroy_codec_specific_data( cs );

    /* Additional extensions */
    if( p_mp4->major_brand != ISOM_BRAND_TYPE_QT )
    {
        /* Bitrate info */
        cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264_BITRATE,
                                                LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
        if( cs )
            lsmash_add_codec_specific_data( (lsmash_summary_t *)p_mp4->summary, cs );
        lsmash_destroy_codec_specific_data( cs );

        if( !p_mp4->b_no_pasp )
        {
            /* Sample scale method */
            cs = lsmash_create_codec_specific_data( LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_SAMPLE_SCALE,
                                                    LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
            if( cs )
            {
                lsmash_isom_sample_scale_t *data = (lsmash_isom_sample_scale_t *)cs->data.structured;
                data->scale_method    = p_mp4->scale_method;
                data->constraint_flag = 1;
                lsmash_add_codec_specific_data( (lsmash_summary_t *)p_mp4->summary, cs );
            }
            lsmash_destroy_codec_specific_data( cs );
        }
    }

    p_mp4->i_sample_entry = lsmash_add_sample_entry( p_mp4->p_root, p_mp4->i_track, p_mp4->summary );
    MP4_FAIL_IF_ERR( !p_mp4->i_sample_entry,
                     "failed to add sample entry for video.\n" );

    /* SEI */
    p_mp4->p_sei_buffer = malloc( sei_size );
    MP4_FAIL_IF_ERR( !p_mp4->p_sei_buffer,
                     "failed to allocate sei transition buffer.\n" );
    memcpy( p_mp4->p_sei_buffer, sei, sei_size );
    p_mp4->i_sei_size = sei_size;

    return sei_size + sps_size + pps_size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t dts, cts;

    if( !p_mp4->i_numframe )
    {
        p_mp4->i_start_offset = p_picture->i_dts * -1;
        p_mp4->i_first_cts = p_mp4->b_dts_compress ? 0 : p_mp4->i_start_offset * p_mp4->i_time_inc;
        if( p_mp4->psz_chapter && (p_mp4->b_brand_qt || p_mp4->b_brand_m4a) )
            MP4_FAIL_IF_ERR( lsmash_create_reference_chapter_track( p_mp4->p_root, p_mp4->i_track, p_mp4->psz_chapter ),
                             "failed to create reference chapter track.\n" );
        if( p_mp4->b_fragments )
        {
            lsmash_edit_t edit;
            edit.duration   = ISOM_EDIT_DURATION_UNKNOWN32;     /* QuickTime doesn't support 64bit duration. */
            edit.start_time = p_mp4->i_first_cts;
            edit.rate       = ISOM_EDIT_MODE_NORMAL;
            MP4_LOG_IF_ERR( lsmash_create_explicit_timeline_map( p_mp4->p_root, p_mp4->i_track, edit ),
                            "failed to set timeline map for video.\n" );
        }
    }

    lsmash_sample_t *p_sample = lsmash_create_sample( i_size + p_mp4->i_sei_size );
    MP4_FAIL_IF_ERR( !p_sample,
                     "failed to create a video sample data.\n" );

    if( p_mp4->p_sei_buffer )
    {
        memcpy( p_sample->data, p_mp4->p_sei_buffer, p_mp4->i_sei_size );
        free( p_mp4->p_sei_buffer );
        p_mp4->p_sei_buffer = NULL;
    }

    memcpy( p_sample->data + p_mp4->i_sei_size, p_nalu, i_size );
    p_mp4->i_sei_size = 0;

    if( p_mp4->b_dts_compress )
    {
        if( p_mp4->i_numframe == 1 )
            p_mp4->i_init_delta = (p_picture->i_dts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
        dts = p_mp4->i_numframe > p_mp4->i_delay_frames
            ? p_picture->i_dts * p_mp4->i_time_inc
            : p_mp4->i_numframe * (p_mp4->i_init_delta / p_mp4->i_dts_compress_multiplier);
        cts = p_picture->i_pts * p_mp4->i_time_inc;
    }
    else
    {
        dts = (p_picture->i_dts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
        cts = (p_picture->i_pts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
    }

    p_sample->dts = dts;
    p_sample->cts = cts;
    p_sample->index = p_mp4->i_sample_entry;
    p_sample->prop.ra_flags = p_picture->i_type == X264_TYPE_IDR ? ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC : ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
    if( p_mp4->b_use_recovery || p_mp4->b_brand_qt )
    {
        p_sample->prop.independent = IS_X264_TYPE_I( p_picture->i_type ) ? ISOM_SAMPLE_IS_INDEPENDENT : ISOM_SAMPLE_IS_NOT_INDEPENDENT;
        p_sample->prop.disposable = p_picture->i_type == X264_TYPE_B ? ISOM_SAMPLE_IS_DISPOSABLE : ISOM_SAMPLE_IS_NOT_DISPOSABLE;
        p_sample->prop.redundant = ISOM_SAMPLE_HAS_NO_REDUNDANCY;
        if( p_mp4->b_use_recovery )
        {
            p_sample->prop.leading = !IS_X264_TYPE_B( p_picture->i_type ) || p_sample->cts >= p_mp4->i_last_intra_cts
                                   ? ISOM_SAMPLE_IS_NOT_LEADING : ISOM_SAMPLE_IS_UNDECODABLE_LEADING;
            if( p_sample->prop.independent == ISOM_SAMPLE_IS_INDEPENDENT )
                p_mp4->i_last_intra_cts = p_sample->cts;
            p_sample->prop.post_roll.identifier = p_picture->i_frame_num % p_mp4->i_max_frame_num;
            if( p_picture->b_keyframe && p_picture->i_type != X264_TYPE_IDR )
            {
                /* A picture with Recovery Point SEI */
                p_sample->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_POST_ROLL_START;
                p_sample->prop.post_roll.complete = (p_sample->prop.post_roll.identifier + p_mp4->i_recovery_frame_cnt) % p_mp4->i_max_frame_num;
            }
        }
        else if( p_picture->i_type == X264_TYPE_I || p_picture->i_type == X264_TYPE_P || p_picture->i_type == X264_TYPE_BREF )
            p_sample->prop.allow_earlier = QT_SAMPLE_EARLIER_PTS_ALLOWED;
        if( p_picture->i_type == X264_TYPE_I && p_picture->b_keyframe && p_mp4->i_recovery_frame_cnt == 0 )
            p_sample->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_OPEN_RAP;
    }

    x264_cli_log( "mp4", X264_LOG_DEBUG, "coded: %d, frame_num: %d, key: %s, type: %s, independ: %s, dispose: %s, lead: %s\n",
                  p_mp4->i_numframe, p_picture->i_frame_num, p_picture->b_keyframe ? "yes" : "no",
                  p_picture->i_type == X264_TYPE_P ? "P" : p_picture->i_type == X264_TYPE_B ? "b" :
                  p_picture->i_type == X264_TYPE_BREF ? "B" : p_picture->i_type == X264_TYPE_IDR ? "I" :
                  p_picture->i_type == X264_TYPE_I ? "i" : p_picture->i_type == X264_TYPE_KEYFRAME ? "K" : "N",
                  p_sample->prop.independent == ISOM_SAMPLE_IS_INDEPENDENT ? "yes" : "no",
                  p_sample->prop.disposable == ISOM_SAMPLE_IS_DISPOSABLE ? "yes" : "no",
                  p_sample->prop.leading == ISOM_SAMPLE_IS_UNDECODABLE_LEADING || p_sample->prop.leading == ISOM_SAMPLE_IS_DECODABLE_LEADING ? "yes" : "no" );

#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    if( p_audio )
        if( write_audio_frames( p_mp4, p_sample->dts / (double)p_audio->i_video_timescale, 0 ) )
            return -1;
#endif

    if( p_mp4->b_fragments && p_mp4->i_numframe && p_sample->prop.ra_flags != ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
    {
        MP4_FAIL_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_mp4->i_track, p_sample->dts - p_mp4->i_prev_dts ),
                         "failed to flush the rest of samples.\n" );
#if HAVE_ANY_AUDIO
        if( p_audio )
            MP4_FAIL_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_audio->i_track, p_audio->summary->samples_in_frame ),
                             "failed to flush the rest of samples for audio.\n" );
#endif
        MP4_FAIL_IF_ERR( lsmash_create_fragment_movie( p_mp4->p_root ),
                         "failed to create a movie fragment.\n" );
    }

    /* Append data per sample. */
    MP4_FAIL_IF_ERR( lsmash_append_sample( p_mp4->p_root, p_mp4->i_track, p_sample ),
                     "failed to append a video frame.\n" );

    p_mp4->i_prev_dts = dts;
    p_mp4->i_numframe++;

    return i_size;
}

const cli_output_t mp4_output = { open_file, set_param, write_headers, write_frame, close_file };
