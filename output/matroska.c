/*****************************************************************************
 * matroska.c: matroska muxer
 *****************************************************************************
 * Copyright (C) 2005-2017 x264 project
 *
 * Authors: Mike Matsnev <mike@haali.su>
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

#include "output.h"
#include "matroska_ebml.h"
#if HAVE_AUDIO
#include "audio/encoders.h"
#endif

#if HAVE_AUDIO
typedef struct
{
    audio_info_t *info;
    hnd_t encoder;
    int64_t lastdts;
} mkv_audio_hnd_t;
#endif

typedef struct
{
    mk_writer *w;
    mk_track_t tracks[MK_MAX_TRACKS];
    uint32_t i_track_count;
    uint32_t i_video_track;
    char b_writing_frame;
    uint32_t i_timebase_num;
    uint32_t i_timebase_den;
#if HAVE_AUDIO
    mkv_audio_hnd_t *a_mkv;
    uint32_t i_audio_track;
#endif
} mkv_hnd_t;

#if HAVE_AUDIO
static int audio_init( hnd_t handle, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    hnd_t henc;

    if( !strcmp( audio_enc, "copy" ) )
        henc = x264_audio_copy_open( filters );
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, (char*[]){ "ac3", "aac", "vorbis", "mp3", "raw", NULL }, &used_enc );
        FAIL_IF_ERR( !encoder, "mkv", "unable to select audio encoder\n" );

        snprintf( audio_params, MAX_ARGS, "%s,codec=%s", audio_parameters, used_enc );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }
    FAIL_IF_ERR( !henc, "mkv", "error opening audio encoder\n" );

    mkv_hnd_t *p_mkv = handle;
    mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv = calloc( 1, sizeof( mkv_audio_hnd_t ) );
    if( !a_mkv )
    {
        x264_cli_log( "mkv", X264_LOG_ERROR, "malloc failed!\n" );
        goto error;
    }

    a_mkv->lastdts = INVALID_DTS;
    a_mkv->encoder = henc;
    a_mkv->info    = x264_audio_encoder_info( henc );

    return 1;

error:
    x264_audio_encoder_close( henc );
    if( p_mkv->a_mkv )
        free( p_mkv->a_mkv );
    p_mkv->a_mkv = NULL;

    return -1;
}
#endif

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    *p_handle = NULL;
    mkv_hnd_t *p_mkv = calloc( 1, sizeof(mkv_hnd_t) );
    if( !p_mkv )
        return -1;

    p_mkv->w = mk_create_writer( psz_filename );
    if( !p_mkv->w )
    {
        free( p_mkv );
        return -1;
    }

#if HAVE_AUDIO
    FAIL_IF_ERR( audio_init( p_mkv, audio_filters, audio_enc, audio_params ) < 0,
                 "mkv", "unable to init audio output\n" );
#endif

    *p_handle = p_mkv;

    return 0;
}


#define STEREO_COUNT 7
static const uint8_t stereo_modes[STEREO_COUNT] = {5,9,7,1,3,13,0};
static const uint8_t stereo_w_div[STEREO_COUNT] = {1,2,1,2,1,1,1};
static const uint8_t stereo_h_div[STEREO_COUNT] = {1,1,2,1,2,1,1};

static int set_video_track( mkv_hnd_t *p_mkv, x264_param_t *p_param )
{
    mk_track_t *vtrack = &p_mkv->tracks[1];
    mk_video_info_t *v = &vtrack->info.v;
    int64_t dw, dh;

    vtrack->type = MK_TRACK_VIDEO;
    vtrack->lacing = MK_LACING_NONE;
    vtrack->codec_id = "V_MPEG4/ISO/AVC";
    vtrack->id = p_mkv->i_video_track = p_mkv->i_track_count = 1;

    if( p_param->i_fps_num > 0 && !p_param->b_vfr_input )
        vtrack->default_frame_duration = (int64_t)p_param->i_fps_den * 1e9 / p_param->i_fps_num;
    else
        vtrack->default_frame_duration = 0;

    dw=v->width = p_param->i_width;
    dh=v->height = p_param->i_height;
    v->display_size_units = DS_PIXELS;
    v->stereo_mode = -1;
    if( p_param->i_frame_packing >= 0 && p_param->i_frame_packing < STEREO_COUNT )
    {
        v->stereo_mode = stereo_modes[p_param->i_frame_packing];
        dw /= stereo_w_div[p_param->i_frame_packing];
        dh /= stereo_h_div[p_param->i_frame_packing];
    }
    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height
        && p_param->vui.i_sar_width != p_param->vui.i_sar_height )
    {
        if( p_param->vui.i_sar_width > p_param->vui.i_sar_height )
        {
            dw = dw * p_param->vui.i_sar_width / p_param->vui.i_sar_height;
        }
        else
        {
            dh = dh * p_param->vui.i_sar_height / p_param->vui.i_sar_width;
        }
    }
    v->display_width = (int)dw;
    v->display_height = (int)dh;

    if( !v->width || !v->height ||
        !v->display_width || !v->display_height )
        return -1;

    p_mkv->i_timebase_num = p_param->i_timebase_num;
    p_mkv->i_timebase_den = p_param->i_timebase_den;

    return 0;
}

#if HAVE_AUDIO
static int codec_private_required( const char *codec )
{
    if( !strcmp( codec, MK_AUDIO_TAG_TTA  ) ||
        !strcmp( codec, MK_AUDIO_TAG_VORBIS ) ||
        !strcmp( codec, MK_AUDIO_TAG_FLAC ) ||
        !strcmp( codec, MK_AUDIO_TAG_AAC ) )
        return 1;

    return 0;
}

static int set_audio_track( mkv_hnd_t *p_mkv, x264_param_t *p_param )
{
    mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv;
    audio_info_t *info = a_mkv->info;
    mk_track_t *atrack = &p_mkv->tracks[++p_mkv->i_track_count];
    mk_audio_info_t *a = &atrack->info.a;

    atrack->id = p_mkv->i_audio_track = p_mkv->i_track_count;
    atrack->type = MK_TRACK_AUDIO;
    atrack->lacing = MK_LACING_NONE;

    if( !strcmp( info->codec_name, "aac" ) )
        atrack->codec_id = MK_AUDIO_TAG_AAC;
    else if( !strcmp( info->codec_name, "ac3" ) )
        atrack->codec_id = MK_AUDIO_TAG_AC3;
    else if( !strcmp( info->codec_name, "eac3" ) )
        atrack->codec_id = MK_AUDIO_TAG_EAC3;
    else if( !strcmp( info->codec_name, "dca" ) )
        atrack->codec_id = MK_AUDIO_TAG_DTS;
    else if( !strcmp( info->codec_name, "vorbis" ) )
        atrack->codec_id = MK_AUDIO_TAG_VORBIS;
    else if( !strcmp( info->codec_name, "mp3" ) )
        atrack->codec_id = MK_AUDIO_TAG_MP3;
    else if( !strcmp( info->codec_name, "mp2" ) )
        atrack->codec_id = MK_AUDIO_TAG_MP2;
    else if( !strcmp( info->codec_name, "mp1" ) )
        atrack->codec_id = MK_AUDIO_TAG_MP1;
    else if( !strcmp( info->codec_name, "mlp" ) )
        atrack->codec_id = MK_AUDIO_TAG_MLP;
    else if( !strcmp( info->codec_name, "truehd" ) )
        atrack->codec_id = MK_AUDIO_TAG_TRUEHD;
    else if( !strcmp( info->codec_name, "tta" ) )
        atrack->codec_id = MK_AUDIO_TAG_TTA;
    else if( !strcmp( info->codec_name, "raw" ) )
        atrack->codec_id = MK_AUDIO_TAG_PCM_LE;
    else
    {
        x264_cli_log( "mkv", X264_LOG_ERROR, "unsupported audio codec\n" );
        return -1;
    }

    a->samplerate            = info->samplerate;
    a->channels              = info->channels;

    if( !strcmp( atrack->codec_id, MK_AUDIO_TAG_AAC ) )
    {
        audio_aac_info_t *aacinfo = info->opaque;
        if( aacinfo && aacinfo->has_sbr )
        {
            a->output_samplerate = info->samplerate;
            a->samplerate       /= 2;
        }
    }

    if( !strcmp( atrack->codec_id, MK_AUDIO_TAG_PCM_LE ) )
    {
        a->bit_depth         = info->chansize * 8;

        // this is slightly inaccurate for some fps and samplerate conbinations
        if( !p_param->b_vfr_input )
            info->framelen = (double)a_mkv->info->samplerate * p_param->i_fps_den / p_param->i_fps_num + 0.5;
        else
            info->framelen = (double)a_mkv->info->samplerate * p_param->i_timebase_num / p_param->i_timebase_den + 0.5;
    }

    atrack->default_frame_duration = x264_from_timebase( info->framelen, info->timebase, 1000000000 );

    if( codec_private_required( atrack->codec_id ) )
    {
        if( !info->extradata_size || !info->extradata )
        {
            x264_cli_log( "mkv", X264_LOG_ERROR, "no extradata found!\n" );
            return -1;
        }
        atrack->codec_private_size = info->extradata_size;
        atrack->codec_private = malloc( info->extradata_size );
        if( !atrack->codec_private )
            return -1;
        memcpy( atrack->codec_private, info->extradata, info->extradata_size );
    }

    return 0;
}
#endif

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    mkv_hnd_t   *p_mkv = handle;

    FAIL_IF_ERR( set_video_track( p_mkv, p_param ), "mkv", "failed to create video track\n" );

#if HAVE_AUDIO
    FAIL_IF_ERR( p_mkv->a_mkv && set_audio_track( p_mkv, p_param ), "mkv", "failed to create audio track\n" );
#endif

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    mkv_hnd_t *p_mkv = handle;
    mk_track_t *vtrack = &p_mkv->tracks[p_mkv->i_video_track];

    int sps_size = p_nal[0].i_payload - 4;
    int pps_size = p_nal[1].i_payload - 4;
    int sei_size = p_nal[2].i_payload;

    uint8_t *sps = p_nal[0].p_payload + 4;
    uint8_t *pps = p_nal[1].p_payload + 4;
    uint8_t *sei = p_nal[2].p_payload;

    int ret;
    uint8_t *avcC;

    vtrack->codec_private_size = 5 + 1 + 2 + sps_size + 1 + 2 + pps_size;
    vtrack->codec_private = malloc( vtrack->codec_private_size );
    if( !vtrack->codec_private )
        return -1;
    avcC = vtrack->codec_private;

    avcC[0] = 1;
    avcC[1] = sps[1];
    avcC[2] = sps[2];
    avcC[3] = sps[3];
    avcC[4] = 0xff; // nalu size length is four bytes
    avcC[5] = 0xe1; // one sps

    avcC[6] = sps_size >> 8;
    avcC[7] = sps_size;

    memcpy( avcC+8, sps, sps_size );

    avcC[8+sps_size] = 1; // one pps
    avcC[9+sps_size] = pps_size >> 8;
    avcC[10+sps_size] = pps_size;

    memcpy( avcC+11+sps_size, pps, pps_size );

    ret = mk_write_header( p_mkv->w, "x264" X264_VERSION, 50000,
                           p_mkv->tracks, p_mkv->i_track_count );


    if( ret < 0 )
        return ret;

    // SEI

    if( mk_start_frame( p_mkv->w ) < 0 )
            return -1;
    p_mkv->b_writing_frame = 1;

    if( mk_add_frame_data( p_mkv->w, sei, sei_size ) < 0 )
        return -1;

    return sei_size + sps_size + pps_size;
}

#if HAVE_AUDIO
static int write_audio( mkv_hnd_t *p_mkv, int64_t video_dts, int finish )
{
    mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv;

    assert( a_mkv );

    if( a_mkv->lastdts == INVALID_DTS )
    {
        if( video_dts > 0 )
            x264_audio_encoder_skip_samples( a_mkv->encoder, video_dts * a_mkv->info->samplerate / 1000000000.0 );
        a_mkv->lastdts = video_dts; // first frame (nonzero if --seek is used)
    }

    audio_packet_t *frame;
    int frames = 0;
    while( a_mkv->lastdts <= video_dts || video_dts < 0 )
    {
        if( finish )
            frame = x264_audio_encoder_finish( a_mkv->encoder );
        else if( !(frame = x264_audio_encode_frame( a_mkv->encoder )) )
        {
            finish = 1;
            continue;
        }

        if( !frame )
            break;

        assert( frame->dts >= 0 ); // Guard against encoders that don't give proper DTS
        a_mkv->lastdts = x264_from_timebase( frame->dts, frame->info.timebase, 1000000000 );

        if( mk_start_frame( p_mkv->w ) < 0 )
            return -1;

        if( mk_add_frame_data( p_mkv->w, frame->data, frame->size ) < 0 )
            return -1;

        if( mk_set_frame_flags( p_mkv->w, a_mkv->lastdts, 1, 0, p_mkv->i_audio_track ) < 0 )
            return -1;

        if( mk_end_frame( p_mkv->w, p_mkv->i_audio_track ) < 0 )
            return -1;

        x264_audio_free_frame( a_mkv->encoder, frame );

        ++frames;
    }

    return frames;
}
#endif

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    mkv_hnd_t *p_mkv = handle;
    int skip = 0;

    int64_t i_stamp = (int64_t)((p_picture->i_pts * 1e9 * p_mkv->i_timebase_num / p_mkv->i_timebase_den) + 0.5);

    if( p_mkv->b_writing_frame )
    {
        if( mk_add_frame_data( p_mkv->w, p_nalu, i_size ) < 0 ||
            mk_set_frame_flags( p_mkv->w, i_stamp, p_picture->b_keyframe, p_picture->i_type == X264_TYPE_B, p_mkv->i_video_track ) < 0 ||
            mk_end_frame( p_mkv->w, p_mkv->i_video_track ) < 0 )
            return -1;
        p_mkv->b_writing_frame = 0;
        skip = 1;
    }

#if HAVE_AUDIO
    FAIL_IF_ERR( p_mkv->a_mkv && write_audio( p_mkv, i_stamp, 0 ) < 0, "mkv", "error writing audio\n" );
#endif

    if( !skip )
    {
        if( mk_start_frame( p_mkv->w ) < 0 ||
            mk_add_frame_data( p_mkv->w, p_nalu, i_size ) < 0 ||
            mk_set_frame_flags( p_mkv->w, i_stamp, p_picture->b_keyframe, p_picture->i_type == X264_TYPE_B, p_mkv->i_video_track ) < 0 ||
            mk_end_frame( p_mkv->w, p_mkv->i_video_track ) < 0 )
                return -1;
    }

    return i_size;
}

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    mkv_hnd_t *p_mkv = handle;
    int ret;
    int64_t i_last_delta[MK_MAX_TRACKS];

    i_last_delta[p_mkv->i_video_track] = p_mkv->i_timebase_den ? (int64_t)(((largest_pts - second_largest_pts) * 1e9 * p_mkv->i_timebase_num / p_mkv->i_timebase_den) + 0.5) : 0;

#if HAVE_AUDIO
    if( p_mkv->a_mkv )
    {
        mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv;
        FAIL_IF_ERR( a_mkv && write_audio( p_mkv, -1, 1 ) < 0, "mkv", "error flushing audio\n" );
        i_last_delta[p_mkv->i_audio_track] = x264_from_timebase( a_mkv->info->last_delta, a_mkv->info->timebase, 1000000000 );
        x264_audio_encoder_close( p_mkv->a_mkv->encoder );
    }
#endif

    ret = mk_close( p_mkv->w, i_last_delta );

#if HAVE_AUDIO
    if( p_mkv->a_mkv )
        free( p_mkv->a_mkv );
#endif

    int i;
    for( i=1; i<=p_mkv->i_track_count; i++ )
    {
        if( p_mkv->tracks[i].codec_private_size )
            free( p_mkv->tracks[i].codec_private );
    }

    free( p_mkv );

    return ret;
}

const cli_output_t mkv_output = { open_file, set_param, write_headers, write_frame, close_file };
