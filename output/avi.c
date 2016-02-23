/*****************************************************************************
 * avi.c: x264 avi output module (using libavformat)
 *****************************************************************************
 * Copyright (C) 2003-2014 x264 project
 *
 * Authors: Anton Mitrofanov <BugMaster@narod.ru>
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
 *****************************************************************************/

#include "output.h"
#undef DECLARE_ALIGNED
#include <libavformat/avformat.h>

typedef struct
{
    AVFormatContext *mux_fc;
    AVStream *video_stm;
    uint8_t *data;
    unsigned d_max;
    unsigned d_cur;
} avi_hnd_t;

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    avi_hnd_t *h = handle;

    if( !h )
        return 0;

    if( h->data )
    {
        free( h->data );
        h->data = NULL;
    }

    if( h->mux_fc && h->video_stm )
    {
        av_write_trailer( h->mux_fc );
        av_freep( &h->video_stm->codec->extradata );
        av_freep( &h->video_stm->codec );
        av_freep( &h->video_stm );
    }

    if( h->mux_fc && h->mux_fc->pb )
    {
        avio_close( h->mux_fc->pb );
        h->mux_fc->pb = NULL;
    }

    if( h->mux_fc )
        av_freep( &h->mux_fc );

    free( h );

    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt )
{
    avi_hnd_t *h;
    AVOutputFormat *mux_fmt;

    *p_handle = NULL;

    FILE *fh = x264_fopen( psz_filename, "w" );
    if( !fh )
        return -1;
    int b_regular = x264_is_regular_file( fh );
    fclose( fh );
    FAIL_IF_ERR( !b_regular, "avi", "AVI output is incompatible with non-regular file `%s'\n", psz_filename )

    if( !(h = malloc( sizeof(avi_hnd_t) )) )
        return -1;
    memset( h, 0, sizeof(avi_hnd_t) );

    av_register_all();
    mux_fmt = av_guess_format( "avi", NULL, NULL );
    if( !mux_fmt )
    {
        close_file( h, 0, 0 );
        return -1;
    }

    h->mux_fc = avformat_alloc_context();
    if( !h->mux_fc )
    {
        close_file( h, 0, 0 );
        return -1;
    }
    h->mux_fc->oformat = mux_fmt;
    snprintf( h->mux_fc->filename, sizeof(h->mux_fc->filename), "%s", psz_filename );

    if( avio_open( &h->mux_fc->pb, psz_filename, AVIO_FLAG_WRITE ) < 0 )
    {
        close_file( h, 0, 0 );
        return -1;
    }

    *p_handle = h;

    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    avi_hnd_t *h = handle;
    AVCodecContext *c;

    if( !h->mux_fc || h->video_stm )
        return -1;

    h->video_stm = avformat_new_stream( h->mux_fc, 0 );
    if( !h->video_stm )
        return -1;

    c = h->video_stm->codec;
    c->flags |= p_param->b_repeat_headers ? 0 : CODEC_FLAG_GLOBAL_HEADER;
    c->time_base.num = p_param->i_timebase_num;
    c->time_base.den = p_param->i_timebase_den;
    c->width = p_param->i_width;
    c->height = p_param->i_height;
    c->pix_fmt = PIX_FMT_YUV420P;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    c->codec_id = CODEC_ID_H264;
    c->codec_tag = MKTAG('H','2','6','4');

    if( !(c->flags & CODEC_FLAG_GLOBAL_HEADER) && avformat_write_header( h->mux_fc, NULL ) )
        return -1;

    return 0;
}

static int write_buffer( avi_hnd_t *h, uint8_t *p_nalu, int i_size )
{
    unsigned ns = h->d_cur + i_size;

    if( !h->data || ns > h->d_max )
    {
        void *dp;
        unsigned dn = 16;

        while( ns > dn )
            dn <<= 1;

        dp = realloc( h->data, dn );
        if( !dp )
            return -1;

        h->data = dp;
        h->d_max = dn;
    }

    memcpy( h->data + h->d_cur, p_nalu, i_size );
    h->d_cur = ns;

    return i_size;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    avi_hnd_t *h = handle;
    AVCodecContext *c;
    int i_size = p_nal[0].i_payload + p_nal[1].i_payload + p_nal[2].i_payload;

    if( !h->mux_fc || !h->video_stm )
        return -1;

    c = h->video_stm->codec;
    if( c->flags & CODEC_FLAG_GLOBAL_HEADER )
    {
        c->extradata_size = i_size - p_nal[2].i_payload;
        av_freep( &c->extradata );
        c->extradata = av_malloc( c->extradata_size );
        if( !c->extradata )
            return -1;
        /* Write the SPS/PPS to the extradata */
        memcpy( c->extradata, p_nal[0].p_payload, c->extradata_size );
        /* Write the SEI as part of the first frame */
        if( write_buffer( h, p_nal[2].p_payload, p_nal[2].i_payload ) < 0 )
            return -1;
        if( avformat_write_header( h->mux_fc, NULL ) )
            return -1;
    }
    else
        if( write_buffer( h, p_nal[0].p_payload, i_size ) < 0 )
            return -1;

    return i_size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    avi_hnd_t *h = handle;
    AVPacket pkt;

    if( !h->mux_fc || !h->video_stm )
        return -1;

    av_init_packet(&pkt);
    pkt.stream_index = h->video_stm->index;
    pkt.flags |= p_picture->b_keyframe ? AV_PKT_FLAG_KEY : 0;
    if( h->d_cur )
    {
        if( write_buffer( h, p_nalu, i_size ) < 0 )
            return -1;
        pkt.data = h->data;
        pkt.size = h->d_cur;
    }
    else
    {
        pkt.data = p_nalu;
        pkt.size = i_size;
    }
    pkt.pts = AV_NOPTS_VALUE; //av_rescale_q( p_picture->i_pts, h->video_stm->codec->time_base, h->video_stm->time_base );
    pkt.dts = AV_NOPTS_VALUE; //av_rescale_q( p_picture->i_dts, h->video_stm->codec->time_base, h->video_stm->time_base );
    if( av_interleaved_write_frame( h->mux_fc, &pkt ) )
        return -1;

    h->d_cur = 0;

    return i_size;
}

const cli_output_t avi_output = { open_file, set_param, write_headers, write_frame, close_file };
