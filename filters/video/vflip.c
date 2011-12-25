/*****************************************************************************
 * crop.c: vertical flip video filter
 *****************************************************************************
 * Copyright (C) 2003-2014 x264 project
 *
 * Authors: James Darnley <james.darnley@gmail.com>
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

#include "internal.h"
#include "video.h"
#define NAME "vflip"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

cli_vid_filter_t vflip_filter;

typedef struct
{
    hnd_t prev_handle;
    cli_vid_filter_t prev_filter;
} vflip_handle;

static void help( int longhelp )
{
    printf( "      "NAME"  vertically flips the frame\n" );
}

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
    vflip_handle *h = calloc( 1, sizeof(vflip_handle) );
    if( !h )
        return -1;

    info->csp ^= X264_CSP_VFLIP;

    h->prev_filter = *filter;
    h->prev_handle = *handle;
    *handle = h;
    *filter = vflip_filter;

    return 0;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
    vflip_handle *h = handle;
    return h->prev_filter.get_frame( h->prev_handle, output, frame );
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    vflip_handle *h = handle;
    return h->prev_filter.release_frame( h->prev_handle, pic, frame );
}

static void free_filter( hnd_t handle )
{
    vflip_handle *h = handle;
    h->prev_filter.free( h->prev_handle );
    free( h );
}

cli_vid_filter_t vflip_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
