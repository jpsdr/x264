/*****************************************************************************
 * yadif.c: yadif (yet another deinterlacing filter)
 *****************************************************************************
 * Copyright (C) 2006 Michael Niedermayer <michaelni@gmx.at>
 * Avisynth port (C) 2007 Alexander G. Balakhnin aka Fizick  http://avisynth.org.ru
 * x264 port (C) 2013 James Darnley <james.darnley@gmail.com>
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

#include <string.h>
#include "filters/video/video.h"
#include "filters/video/yadif_filter_line.h"

#define NAME "yadif"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

cli_vid_filter_t yadif_filter;
filter_line_func filter_line;

typedef struct {
    hnd_t prev_handle;
    cli_vid_filter_t prev_filter;
    int mode;
    int tff;
    cli_pic_t buffer;
    int64_t pts;
    const x264_cli_csp_t *csp;
} yadif_handle_t;

/***********************
*         Help         *
***********************/

static void help( int longhelp )
{
    printf( "      "NAME":[mode][,order]\n" );
    if( !longhelp )
        return;
    printf(
"            Deinterlaces the picture using MPlayer's yadif\n"
"            mode: sets the deinterlacing mode\n"
"            0 - single-rate deinterlacing (default)\n"
"            1 - double-rate deinterlacing (bob)\n"
"            2 - single-rate deinterlacing without spacial interlacing check\n"
"            3 - double-rate deinterlacing without spacial interlacing check\n"
"            order: forces the field order\n"
"            tff - top-field first\n"
"            bff - bottom-field first\n" );
}

/***********************
*         Init         *
***********************/

static int unsupported_colorspace( int csp )
{
    switch( csp )
    {
        case X264_CSP_HIGH_DEPTH|X264_CSP_I420:
        case X264_CSP_HIGH_DEPTH|X264_CSP_I422:
        case X264_CSP_HIGH_DEPTH|X264_CSP_I444:
        case X264_CSP_HIGH_DEPTH|X264_CSP_YV12:
        case X264_CSP_I420:
        case X264_CSP_I422:
        case X264_CSP_I444:
        case X264_CSP_YV12:
            return 0;
        default:
            return 1;
    }
}

static int yadif_init( hnd_t *handle, cli_vid_filter_t *filter,
                       video_info_t *info, x264_param_t *param, char *opt_string )
{
    yadif_handle_t *h;

    FAIL_IF_ERROR( unsupported_colorspace( info->csp ),
                   "Only planar YCbCr images supported\n" )

    h = calloc( 1, sizeof(yadif_handle_t) );
    if(!h)
        return -1;

    if( x264_cli_pic_alloc( &h->buffer, info->csp, info->width, info->height ) )
        return -1;

    h->mode = 0;
    h->tff = info->tff;

    if( opt_string )
    {
        char *opt;
        static const char *optlist[] = { "mode", "order", NULL };
        char **opts = x264_split_options( opt_string, optlist );

        opt = x264_get_option( "mode", opts );
        if( opt )
        {
            h->mode = x264_otoi( opt, -1 );
            if( h->mode < 0 || h->mode > 3 )
            {
                x264_cli_log( NAME, X264_LOG_WARNING,
                              "Invalid mode (%s), ignoring\n", opt );
                h->mode = 0;
            }
        }

        opt = x264_get_option( "order", opts );
        if( opt )
        {
            if( !strcmp( opt, "top" ) || !strcmp( opt, "tff" ) )
                h->tff = 1;
            else if( !strcmp( opt, "bottom" ) || !strcmp( opt, "bff" ) )
                h->tff = 0;
            else
                x264_cli_log( NAME, X264_LOG_WARNING,
                              "Unknown order (%s), ignoring\n", opt );
        }
        x264_free_string_array( opts );
    }

    if( x264_init_vid_filter( "cache", handle, filter, info, param, (void*)3 ) )
        return -1;

    if( h->mode&1 )
    {
        info->num_frames *= 2;
        info->fps_num *= 2;
        info->timebase_den *= 2;
    }

    info->interlaced = 0;
    h->csp = x264_cli_get_csp( info->csp );
    h->prev_filter = *filter;
    h->prev_handle = *handle;
    *handle = h;
    *filter = yadif_filter;

    filter_line = get_filter_func( param->cpu, info->csp&X264_CSP_HIGH_DEPTH );

    x264_cli_log( NAME, X264_LOG_INFO, "%s-rate deinterlacing "
                  "%s spatial interlacing check, %s-field first\n",
                  (h->mode&1) ? "double"  : "single",
                  (h->mode&2) ? "without" : "with",
                  (h->tff)    ? "top"     : "bottom" );

    return 0;
}

/***********************
*    Process Frames    *
***********************/

static void interpolate( uint8_t *dst, uint8_t *cur0, uint8_t *cur2, int w,
                         int high_depth )
{
    if( high_depth )
    {
        uint16_t *dst_16 = (uint16_t *)dst;
        uint16_t *cur0_16 = (uint16_t *)cur0;
        uint16_t *cur2_16 = (uint16_t *)cur2;
        for( int x = 0; x < w; x++ )
            dst_16[x] = (cur0_16[x] + cur2_16[x] + 1)>>1;
    }
    else
        for( int x = 0; x < w; x++ )
            dst[x] = (cur0[x] + cur2[x] + 1)>>1;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame_out )
{
    yadif_handle_t *h = handle;
    struct yadif_context yctx;
    cli_pic_t prev, cur, next;

    int df       = x264_cli_csp_depth_factor( h->buffer.img.csp );
    int ret      = 0;
    int tff      = h->tff;
    int frame_in = (h->mode&1) ? frame_out/2 : frame_out;
    int parity   = (h->mode&1) ? (frame_out&1) ^ (1^tff) : (tff^1);

    if( frame_in == 0 )
    {
        ret |= h->prev_filter.get_frame( h->prev_handle, &prev, frame_in );
        ret |= h->prev_filter.get_frame( h->prev_handle, &cur, frame_in );
        ret |= h->prev_filter.get_frame( h->prev_handle, &next, frame_in+1 );
    }
    else
    {
        ret |= h->prev_filter.get_frame( h->prev_handle, &prev, frame_in-1 );
        ret |= h->prev_filter.get_frame( h->prev_handle, &cur, frame_in );
        if ( h->prev_filter.get_frame( h->prev_handle, &next, frame_in+1 ) )
            ret |= h->prev_filter.get_frame( h->prev_handle, &next, frame_in );
    }
    if( ret )
        return ret;

    *output = h->buffer;
    output->pts = h->pts;
    output->duration = cur.duration;
    h->pts += cur.duration;

    yctx.mode = h->mode;

    for( int i = 0; i < 3; i++ )
    {
        int width  = cur.img.width  * h->csp->width[i];
        int height = cur.img.height * h->csp->height[i];
        int stride = cur.img.stride[i];

        int y = 0;
        if( (y^parity)&1 )
            /* duplicate 1 */
            memcpy( output->img.plane[i],
                    cur.img.plane[i]+stride, width*df );
        else
            /* copy original */
            memcpy( output->img.plane[i],
                    cur.img.plane[i], width*df );

        y = 1;
        if( (y^parity)&1 )
            /* interpolate 0 and 2 */
            interpolate( output->img.plane[i]+stride,
                         cur.img.plane[i],
                         cur.img.plane[i]+2*stride, width, df/2 );
        else
            /* copy original */
            memcpy( output->img.plane[i]+stride,
                    cur.img.plane[i]+stride, width*df );

        yctx.width  = width;
        yctx.stride = stride;

        for( y = 2; y < height-2; y++ )
        {
            if( (y^parity)&1 )
            {
                yctx.output   = output->img.plane[i] + y*stride;
                yctx.previous = prev.img.plane[i]    + y*stride;
                yctx.current  = cur.img.plane[i]     + y*stride;
                yctx.next     = next.img.plane[i]    + y*stride;
                yctx.parity   = parity^tff;
                x264_stack_align( filter_line, &yctx );
            }
            else
                memcpy( output->img.plane[i]+y*stride,
                        cur.img.plane[i]+y*stride, width*df );
        }

        y = height-2;
        if( (y^parity)&1 )
            /* interpolate h-3 and h-1 */
            interpolate( output->img.plane[i]+y*stride,
                         cur.img.plane[i]+(y-1)*stride,
                         cur.img.plane[i]+(y+1)*stride, width, df/2 );
        else
            /* copy original */
            memcpy( output->img.plane[i]+y*stride,
                    cur.img.plane[i]+y*stride, width*df );

        y = height-1;
        if( (y^parity)&1 )
            /* duplicate h-2 */
            memcpy( output->img.plane[i]+y*stride,
                    cur.img.plane[i]+(y-1)*stride, width*df );
        else
            /* copy original */
            memcpy( output->img.plane[i]+y*stride,
                    cur.img.plane[i]+(y-1)*stride, width*df );
    }
    x264_emms();

    if( !(h->mode&1) && !frame_out )
        return 0;
    else if( (h->mode&1) && (frame_out < 3 || frame_out&1) )
        return 0;

    return h->prev_filter.release_frame( h->prev_handle, &prev, frame_in-1 );
}

/***********************
*         Free         *
***********************/

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    return 0;
}

static void free_filter( hnd_t handle )
{
    yadif_handle_t *h = handle;

    h->prev_filter.free( h->prev_handle );
    x264_cli_pic_clean( &h->buffer );
    free( h );
}

cli_vid_filter_t yadif_filter = { NAME, help, yadif_init, get_frame, release_frame, free_filter, NULL };
