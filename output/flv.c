/*****************************************************************************
 * flv.c: flv muxer
 *****************************************************************************
 * Copyright (C) 2009-2017 x264 project
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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
#include "flv_bytestream.h"
#include "audio/encoders.h"
#if HAVE_AUDIO
#include <lsmash.h>
#endif

#define CHECK(x)\
do {\
    if( (x) < 0 )\
        return -1;\
} while( 0 )

#if HAVE_AUDIO
typedef struct
{
    audio_info_t *info;
    hnd_t encoder;
    int header;
    int codecid;
    int stereo;
    int64_t lastdts;
} flv_audio_hnd_t;
#endif

typedef struct
{
    flv_buffer *c;

    uint8_t *sei;
    int sei_len;

    int64_t i_fps_num;
    int64_t i_fps_den;
    int64_t i_framenum;

    uint64_t i_framerate_pos;
    uint64_t i_duration_pos;
    uint64_t i_filesize_pos;
    uint64_t i_bitrate_pos;

    uint8_t b_write_length;
    int64_t i_prev_dts;
    int64_t i_prev_cts;
    int64_t i_delay_time;
    int64_t i_init_delta;
    int i_delay_frames;

    double d_timebase;
    int b_vfr_input;
    int b_dts_compress;

    unsigned start;

#if HAVE_AUDIO
    flv_audio_hnd_t *a_flv;
#endif
} flv_hnd_t;

#if HAVE_AUDIO
static int audio_init( hnd_t handle, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    // TODO: support adpcm_swf, pcm and aac

    hnd_t henc;

    if( !strcmp( audio_enc, "copy" ) )
        henc = x264_audio_copy_open( filters );
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, (char*[]){ "mp3", "aac", "raw", NULL }, &used_enc );
        FAIL_IF_ERR( !encoder, "flv", "unable to select audio encoder\n" );

        snprintf( audio_params, MAX_ARGS, "%s,codec=%s", audio_parameters, used_enc );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }
    FAIL_IF_ERR( !henc, "flv", "error opening audio encoder\n" );
    flv_hnd_t *p_flv = handle;
    flv_audio_hnd_t *a_flv = p_flv->a_flv = calloc( 1, sizeof( flv_audio_hnd_t ) );
    a_flv->lastdts = INVALID_DTS;
    audio_info_t *info = a_flv->info = x264_audio_encoder_info( henc );

    int header = 0;
    if ( !strcmp( info->codec_name, "raw" ) )
        a_flv->codecid = FLV_CODECID_RAW;
    else if( !strcmp( info->codec_name, "mp3" ) )
        a_flv->codecid = FLV_CODECID_MP3;
    else if( !strcmp( info->codec_name, "aac" ) )
        a_flv->codecid = FLV_CODECID_AAC;
    else
    {
        x264_cli_log( "flv", X264_LOG_ERROR, "unsupported audio codec\n" );
        goto error;
    }

    header |= a_flv->codecid;
    a_flv->stereo = info->channels == 2;

    if( a_flv->codecid == FLV_CODECID_AAC )
        header |= FLV_SAMPLERATE_44100HZ | FLV_SAMPLESSIZE_16BIT | FLV_STEREO;
    else
    {
        switch( info->samplerate )
        {
            case 5512:
            case 8000:
                header |= FLV_SAMPLERATE_SPECIAL;
                break;
            case 11025:
                header |= FLV_SAMPLERATE_11025HZ;
                break;
            case 22050:
                header |= FLV_SAMPLERATE_22050HZ;
                break;
            case 44100:
                header |= FLV_SAMPLERATE_44100HZ;
                break;
            default:
                x264_cli_log( "flv", X264_LOG_ERROR, "unsupported %dhz sample rate\n", info->samplerate );
                goto error;
        }

        switch( info->chansize )
        {
            case 1:
                header |= FLV_SAMPLESSIZE_8BIT;
                break;
            case 2:
                header |= FLV_SAMPLESSIZE_16BIT;
                break;
            default:
                x264_cli_log( "flv", X264_LOG_ERROR, "%d-bit audio not supported\n", info->chansize * 8 );
                goto error;
        }

        switch( info->channels )
        {
            case 1:
                header |= FLV_MONO;
                break;
            case 2:
                header |= FLV_STEREO;
                break;
            default:
                x264_cli_log( "flv", X264_LOG_ERROR, "%d-channel audio not supported\n", info->channels );
                goto error;
        }
    }

    a_flv->header   = header;
    a_flv->encoder  = henc;

    return 1;

    error:
    x264_audio_encoder_close( henc );
    free( p_flv->a_flv );
    p_flv->a_flv = NULL;

    return -1;
}
#endif

static int write_header( flv_buffer *c, int audio )
{
    flv_put_tag( c, "FLV" );                // Signature
    flv_put_byte( c, 1 );                   // Version
    flv_put_byte( c, 1 | (audio ? 4 : 0) ); // Video + Audio (if requested)
    flv_put_be32( c, 9 );                   // DataOffset
    flv_put_be32( c, 0 );                   // PreviousTagSize 0

    return flv_flush_data( c );
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    flv_hnd_t *p_flv = calloc( 1, sizeof(flv_hnd_t) );
    int ret = -1;
    if( p_flv )
    {
        flv_buffer *c = flv_create_writer( psz_filename );
        if( c )
        {
            ret=0;
#if HAVE_AUDIO
            ret = audio_init( p_flv, audio_filters, audio_enc, audio_params );
            FAIL_IF_ERR( ret < 0, "flv", "unable to init audio output\n" );
            if((ret>=0) && !write_header( c,ret ))
#else
            if((ret==0) && !write_header( c,ret ))
#endif								
            {
                p_flv->c = c;
                p_flv->b_dts_compress = opt->use_dts_compress;
                *p_handle = p_flv;
#if HAVE_AUDIO
				return ret;
#else
				return 0;
#endif				
            }
#if HAVE_AUDIO
            if (ret>=0) ret=-1;
#else
            if (ret==0) ret=-1;		
#endif
            fclose( c->fp );
            free( c->data );
            free( c );
        }
        free( p_flv );
    }


    *p_handle = NULL;
    return ret;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;

    flv_put_byte( c, FLV_TAG_TYPE_META ); // Tag Type "script data"

    int start = c->d_cur;
    flv_put_be24( c, 0 ); // data length
    flv_put_be24( c, 0 ); // timestamp
    flv_put_be32( c, 0 ); // reserved

    flv_put_byte( c, AMF_DATA_TYPE_STRING );
    flv_put_amf_string( c, "onMetaData" );

    flv_put_byte( c, AMF_DATA_TYPE_MIXEDARRAY );
    flv_put_be32( c, 7 );

    flv_put_amf_string( c, "width" );
    flv_put_amf_double( c, p_param->i_width );

    flv_put_amf_string( c, "height" );
    flv_put_amf_double( c, p_param->i_height );

    flv_put_amf_string( c, "framerate" );

    if( !p_param->b_vfr_input )
        flv_put_amf_double( c, (double)p_param->i_fps_num / p_param->i_fps_den );
    else
    {
        p_flv->i_framerate_pos = c->d_cur + c->d_total + 1;
        flv_put_amf_double( c, 0 ); // written at end of encoding
    }

    flv_put_amf_string( c, "videocodecid" );
    flv_put_amf_double( c, FLV_CODECID_H264 );

    flv_put_amf_string( c, "duration" );
    p_flv->i_duration_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "filesize" );
    p_flv->i_filesize_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "videodatarate" );
    p_flv->i_bitrate_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

#if HAVE_AUDIO
    if( p_flv->a_flv )
    {
        flv_audio_hnd_t *a_flv = p_flv->a_flv;
        flv_put_amf_string( c, "audiocodecid" );
        flv_put_amf_double( c, a_flv->codecid >> FLV_AUDIO_CODECID_OFFSET );
        flv_put_amf_string( c, "audiosamplesize" );
        flv_put_amf_double( c, a_flv->info->chansize * 8 );
        flv_put_amf_string( c, "audiosamplerate" );
        flv_put_amf_double( c, a_flv->info->samplerate );
        flv_put_amf_string( c, "stereo" );
        flv_put_amf_bool  ( c, a_flv->stereo );
        if( a_flv->codecid == FLV_CODECID_RAW )
        {
            // this is slightly inaccurate for some fps and samplerate conbinations
            if( !p_param->b_vfr_input )
                a_flv->info->framelen = (double)a_flv->info->samplerate * p_param->i_fps_den / p_param->i_fps_num + 0.5;
            else
                a_flv->info->framelen = (double)a_flv->info->samplerate * p_param->i_timebase_num / p_param->i_timebase_den + 0.5;
        }
    }
#endif

    flv_put_amf_string( c, "" );
    flv_put_byte( c, AMF_END_OF_OBJECT );

    unsigned length = c->d_cur - start;
    flv_rewrite_amf_be24( c, length - 10, start );

    flv_put_be32( c, length + 1 ); // tag length

    p_flv->i_fps_num = p_param->i_fps_num;
    p_flv->i_fps_den = p_param->i_fps_den;
    p_flv->d_timebase = (double)p_param->i_timebase_num / p_param->i_timebase_den;
    p_flv->b_vfr_input = p_param->b_vfr_input;
    p_flv->i_delay_frames = p_param->i_bframe ? (p_param->i_bframe_pyramid ? 2 : 1) : 0;

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    flv_hnd_t *p_flv = handle;
#if HAVE_AUDIO
    flv_audio_hnd_t *a_flv = p_flv->a_flv;
#endif
    flv_buffer *c = p_flv->c;

    int sps_size = p_nal[0].i_payload;
    int pps_size = p_nal[1].i_payload;
    int sei_size = p_nal[2].i_payload;

    // SEI
    /* It is within the spec to write this as-is but for
     * mplayer/ffmpeg playback this is deferred until before the first frame */

    p_flv->sei = malloc( sei_size );
    if( !p_flv->sei )
        return -1;
    p_flv->sei_len = sei_size;

    memcpy( p_flv->sei, p_nal[2].p_payload, sei_size );

    // SPS
    uint8_t *sps = p_nal[0].p_payload + 4;

    flv_put_byte( c, FLV_TAG_TYPE_VIDEO );
    flv_put_be24( c, 0 ); // rewrite later
    flv_put_be24( c, 0 ); // timestamp
    flv_put_byte( c, 0 ); // timestamp extended
    flv_put_be24( c, 0 ); // StreamID - Always 0
    p_flv->start = c->d_cur; // needed for overwriting length

    flv_put_byte( c, 7 | FLV_FRAME_KEY ); // Frametype and CodecID
    flv_put_byte( c, 0 ); // AVC sequence header
    flv_put_be24( c, 0 ); // composition time

    flv_put_byte( c, 1 );      // version
    flv_put_byte( c, sps[1] ); // profile
    flv_put_byte( c, sps[2] ); // profile
    flv_put_byte( c, sps[3] ); // level
    flv_put_byte( c, 0xff );   // 6 bits reserved (111111) + 2 bits nal size length - 1 (11)
    flv_put_byte( c, 0xe1 );   // 3 bits reserved (111) + 5 bits number of sps (00001)

    flv_put_be16( c, sps_size - 4 );
    flv_append_data( c, sps, sps_size - 4 );

    // PPS
    flv_put_byte( c, 1 ); // number of pps
    flv_put_be16( c, pps_size - 4 );
    flv_append_data( c, p_nal[1].p_payload + 4, pps_size - 4 );

    // FRExt fields
    if( sps[1] == 100 || sps[1] == 110 || sps[1] == 122 || sps[1] == 144 )
    {
        flv_put_byte( c, 0xfd );   // 6 bits reserved (111111) + 2 bits chroma format indicator (1)
        flv_put_byte( c, (BIT_DEPTH-8) | 0xf8 );   // 5 bits reserved (11111) + 3 bits bit depth of the samples in the luma arrays
        flv_put_byte( c, (BIT_DEPTH-8) | 0xf8 );   // 5 bits reserved (11111) + 3 bits bit depth of the samples in the chroma arrays
        flv_put_byte( c, 0 );      // number of spsext
    }

    // rewrite data length info
    unsigned length = c->d_cur - p_flv->start;
    flv_rewrite_amf_be24( c, length, p_flv->start - 10 );
    flv_put_be32( c, length + 11 ); // Last tag size

#if HAVE_AUDIO
    if( a_flv && a_flv->codecid == FLV_CODECID_AAC )
    {
        FAIL_IF_ERR( !a_flv->info->extradata, "flv", "audio codec is AAC but extradata is NULL\n" );

        uint8_t *extradata;
        uint32_t extradata_size;
        if( a_flv->info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
        {
            extradata = a_flv->info->extradata;
            extradata_size = a_flv->info->extradata_size;
        }
        else if( a_flv->info->extradata_type == EXTRADATA_TYPE_LSMASH )
        {
            lsmash_codec_specific_t *orig = NULL;
            uint32_t num_extensions = a_flv->info->extradata_size / sizeof(lsmash_codec_specific_t *);
            for( uint32_t i = 0; i < num_extensions; i++ )
            {
                orig = ((lsmash_codec_specific_t **)a_flv->info->extradata)[i];
                if( orig && orig->type == LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG )
                    break;
            }
            FAIL_IF_ERR( !orig, "flv", "no extradata for AAC found!\n" );

            assert( orig->format == LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED );

            lsmash_codec_specific_t *conv = lsmash_convert_codec_specific_format( orig, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
            FAIL_IF_ERR( !conv, "flv", "failed to convert format of AAC specific info.\n" );

            lsmash_mp4sys_decoder_parameters_t *param = (lsmash_mp4sys_decoder_parameters_t *)conv->data.structured;
            assert( param->objectTypeIndication == MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3 );

            int err = lsmash_get_mp4sys_decoder_specific_info( param, &extradata, &extradata_size );
            lsmash_destroy_codec_specific_data( conv );
            FAIL_IF_ERR( err, "flv", "failed to get AAC specific info.\n" );
        }
        else
        {
            x264_cli_log( "flv", X264_LOG_ERROR, "unknown extradata type.\n" );
            return -1;
        }

        flv_put_byte( c, FLV_TAG_TYPE_AUDIO );
        flv_put_be24( c, 2 + a_flv->info->extradata_size );
        flv_put_be24( c, 0 );
        flv_put_byte( c, 0 );
        flv_put_be24( c, 0 );

        flv_put_byte( c, a_flv->header );
        flv_put_byte( c, 0 );
        flv_append_data( c, a_flv->info->extradata, a_flv->info->extradata_size );
        flv_put_be32( c, 11 + 2 + a_flv->info->extradata_size );
    }
#endif

    CHECK( flv_flush_data( c ) );

    return sei_size + sps_size + pps_size;
}

#if HAVE_AUDIO
static int write_audio( flv_hnd_t *p_flv, int64_t video_dts, int finish )
{
    flv_audio_hnd_t *a_flv = p_flv->a_flv;
    flv_buffer *c = p_flv->c;

    assert( a_flv );

    int aac = a_flv->codecid == FLV_CODECID_AAC;
    if( a_flv->lastdts == INVALID_DTS )
    {
        if( video_dts > 0 )
            x264_audio_encoder_skip_samples( a_flv->encoder, video_dts * a_flv->info->samplerate / 1000 );
        a_flv->lastdts = video_dts; // first frame (nonzero if --seek is used)
    }
    audio_packet_t *frame;
    int frames = 0;
    while( a_flv->lastdts <= video_dts || video_dts < 0 )
    {
        if( finish )
            frame = x264_audio_encoder_finish( a_flv->encoder );
        else if( !(frame = x264_audio_encode_frame( a_flv->encoder )) )
        {
            finish = 1;
            continue;
        }

        if( !frame )
            break;

        assert( frame->dts >= 0 ); // Guard against encoders that don't give proper DTS
        a_flv->lastdts = x264_from_timebase( frame->dts, frame->info.timebase, 1000 );

        flv_put_byte( c, FLV_TAG_TYPE_AUDIO );
        flv_put_be24( c, 1 + aac + frame->size );
        flv_put_be24( c, (int32_t) a_flv->lastdts );
        flv_put_byte( c, (int32_t) a_flv->lastdts >> 24 );
        flv_put_be24( c, 0 );

        flv_put_byte( c, a_flv->header );
        if( aac )
            flv_put_byte( c, 1 );
        flv_append_data( c, frame->data, frame->size );

        flv_put_be32( c, 11 + 1 + aac + frame->size );

        x264_audio_free_frame( a_flv->encoder, frame );

        CHECK( flv_flush_data( c ) );
        ++frames;
    }
    return frames;
}
#endif

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;

#define convert_timebase_ms( timestamp, timebase ) (int64_t)((timestamp) * (timebase) * 1000 + 0.5)

    if( !p_flv->i_framenum )
    {
        p_flv->i_delay_time = p_picture->i_dts * -1;
        if( !p_flv->b_dts_compress && p_flv->i_delay_time )
            x264_cli_log( "flv", X264_LOG_INFO, "initial delay %"PRId64" ms\n",
                          convert_timebase_ms( p_picture->i_pts + p_flv->i_delay_time, p_flv->d_timebase ) );
    }

    int64_t dts;
    int64_t cts;
    int64_t offset;

    if( p_flv->b_dts_compress )
    {
        if( p_flv->i_framenum == 1 )
            p_flv->i_init_delta = convert_timebase_ms( p_picture->i_dts + p_flv->i_delay_time, p_flv->d_timebase );
        dts = p_flv->i_framenum > p_flv->i_delay_frames
            ? convert_timebase_ms( p_picture->i_dts, p_flv->d_timebase )
            : p_flv->i_framenum * p_flv->i_init_delta / (p_flv->i_delay_frames + 1);
        cts = convert_timebase_ms( p_picture->i_pts, p_flv->d_timebase );
    }
    else
    {
        dts = convert_timebase_ms( p_picture->i_dts + p_flv->i_delay_time, p_flv->d_timebase );
        cts = convert_timebase_ms( p_picture->i_pts + p_flv->i_delay_time, p_flv->d_timebase );
    }
    offset = cts - dts;

    if( p_flv->i_framenum )
    {
        if( p_flv->i_prev_dts == dts )
            x264_cli_log( "flv", X264_LOG_WARNING, "duplicate DTS %"PRId64" generated by rounding\n"
                          "               decoding framerate cannot exceed 1000fps\n", dts );
        if( p_flv->i_prev_cts == cts )
            x264_cli_log( "flv", X264_LOG_WARNING, "duplicate CTS %"PRId64" generated by rounding\n"
                          "               composition framerate cannot exceed 1000fps\n", cts );
    }
    p_flv->i_prev_dts = dts;
    p_flv->i_prev_cts = cts;

    // A new frame - write packet header
    flv_put_byte( c, FLV_TAG_TYPE_VIDEO );
    flv_put_be24( c, 0 ); // calculated later
    flv_put_be24( c, (int32_t) dts );
    flv_put_byte( c, (int32_t) dts >> 24 );
    flv_put_be24( c, 0 );

    p_flv->start = c->d_cur;
    flv_put_byte( c, p_picture->b_keyframe ? FLV_FRAME_KEY : FLV_FRAME_INTER );
    flv_put_byte( c, 1 ); // AVC NALU
    flv_put_be24( c, offset );

    if( p_flv->sei )
    {
        flv_append_data( c, p_flv->sei, p_flv->sei_len );
        free( p_flv->sei );
        p_flv->sei = NULL;
    }
    flv_append_data( c, p_nalu, i_size );

    unsigned length = c->d_cur - p_flv->start;
    flv_rewrite_amf_be24( c, length, p_flv->start - 10 );
    flv_put_be32( c, 11 + length ); // Last tag size
    CHECK( flv_flush_data( c ) );

#if HAVE_AUDIO
    FAIL_IF_ERR( p_flv->a_flv && write_audio( p_flv, dts, 0 ) < 0, "flv", "error writing audio\n" );
#endif

    p_flv->i_framenum++;

    return i_size;
}

static int rewrite_amf_double( FILE *fp, uint64_t position, double value )
{
    uint64_t x = endian_fix64( flv_dbl2int( value ) );
    return !fseek( fp, position, SEEK_SET ) && fwrite( &x, 8, 1, fp ) == 1 ? 0 : -1;
}

#undef CHECK
#define CHECK(x)\
do {\
    if( (x) < 0 )\
        goto error;\
} while( 0 )

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    int ret = -1;
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;

#if HAVE_AUDIO
    if( p_flv->a_flv )
    {
        FAIL_IF_ERR( p_flv->a_flv && write_audio( p_flv, -1, 1 ) < 0, "flv", "error flushing audio\n" );
        x264_audio_encoder_close( p_flv->a_flv->encoder );
    }
#endif

    CHECK( flv_flush_data( c ) );

    double total_duration = (2 * largest_pts - second_largest_pts) * p_flv->d_timebase;

    if( x264_is_regular_file( c->fp ) && total_duration > 0 )
    {
        double framerate;
        uint64_t filesize = ftell( c->fp );

        if( p_flv->i_framerate_pos )
        {
            framerate = (double)p_flv->i_framenum / total_duration;
            CHECK( rewrite_amf_double( c->fp, p_flv->i_framerate_pos, framerate ) );
        }

        CHECK( rewrite_amf_double( c->fp, p_flv->i_duration_pos, total_duration ) );
        CHECK( rewrite_amf_double( c->fp, p_flv->i_filesize_pos, filesize ) );
        CHECK( rewrite_amf_double( c->fp, p_flv->i_bitrate_pos, filesize * 8 / ( total_duration * 1000 ) ) );
    }
    ret = 0;

error:
    fclose( c->fp );
    free( c->data );
    free( c );

#if HAVE_AUDIO
    if( p_flv->a_flv )
        free( p_flv->a_flv );
#endif
    free( p_flv );

    return ret;
}

const cli_output_t flv_output = { open_file, set_param, write_headers, write_frame, close_file };
