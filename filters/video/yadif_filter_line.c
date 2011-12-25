/*****************************************************************************
 * yadif_filter_line.c: yadif (yet another deinterlacing filter)
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

#include "common/common.h"
#include "config.h"
#include "filters/video/yadif_filter_line.h"
#include "x264.h"

#if HAVE_MMX
#   include "x86/yadif_filter_line.h"
#endif

#define CHECK(j) \
    score = abs( cur[-refs-1+ j] - cur[+refs-1- j] ) \
          + abs( cur[-refs  + j] - cur[+refs  - j] ) \
          + abs( cur[-refs+1+ j] - cur[+refs+1- j] ); \
    if( score < spatial_score ) \
    { \
        spatial_score = score; \
        spatial_pred  = (cur[-refs  + j] + cur[+refs  - j]) >> 1;

#define FILTER \
    for( int x = 0; x < w; x++ ) \
    { \
        int score; \
        int c = cur[-refs]; \
        int d = (prev2[0] + next2[0]) >> 1; \
        int e = cur[+refs]; \
        int temporal_diff0 = abs( prev2[0] - next2[0] ); \
        int temporal_diff1 = (abs( prev[-refs] - c ) \
                             + abs( prev[+refs] - e )) >> 1; \
        int temporal_diff2 = (abs( next[-refs] - c ) \
                             + abs( next[+refs] - e )) >> 1; \
        int diff = X264_MAX3( temporal_diff0>>1, temporal_diff1, temporal_diff2 ); \
        int spatial_pred = (c + e) >> 1; \
        int spatial_score = abs( cur[-refs-1] - cur[+refs-1] ) \
                          + abs( c-e ) \
                          + abs( cur[-refs+1] - cur[+refs+1] ) \
                          - 1; \
 \
        CHECK(-1) CHECK(-2) } } \
        CHECK( 1) CHECK( 2) } } \
 \
        if( yctx->mode < 2 ) \
        { \
            int b   = (prev2[-2*refs] + next2[-2*refs]) >> 1; \
            int f   = (prev2[+2*refs] + next2[+2*refs]) >> 1; \
            int max = X264_MAX3( d-e, d-c, X264_MIN( b-c, f-e ) ); \
            int min = X264_MIN3( d-e, d-c, X264_MAX( b-c, f-e ) ); \
            diff    = X264_MAX3( diff, min, -max ); \
        } \
 \
        if( spatial_pred > d + diff ) \
           spatial_pred = d + diff; \
        else if( spatial_pred < d - diff ) \
           spatial_pred = d - diff; \
 \
        dst[0] = spatial_pred; \
 \
        dst++; \
        cur++; \
        prev++; \
        next++; \
        prev2++; \
        next2++; \
    }

static void filter_line_c( struct yadif_context *yctx )
{
    int w = yctx->width;
    intptr_t refs = yctx->stride;
    uint8_t *dst = yctx->output;
    const uint8_t *prev  = yctx->previous;
    const uint8_t *cur   = yctx->current;
    const uint8_t *next  = yctx->next;
    const uint8_t *prev2 = yctx->parity ? prev : cur ;
    const uint8_t *next2 = yctx->parity ? cur  : next;

    FILTER
}

static void filter_line_c_16bit( struct yadif_context *yctx )
{
    int w = yctx->width;
    intptr_t refs = yctx->stride;
    uint16_t *dst = (uint16_t *)yctx->output;
    const uint16_t *prev  = (uint16_t *)yctx->previous;
    const uint16_t *cur   = (uint16_t *)yctx->current;
    const uint16_t *next  = (uint16_t *)yctx->next;
    const uint16_t *prev2 = yctx->parity ? prev : cur ;
    const uint16_t *next2 = yctx->parity ? cur  : next;

    FILTER
}

filter_line_func get_filter_func( unsigned int cpu, int high_depth ) {
    filter_line_func ret = filter_line_c;
#if defined ARCH_X86 && defined __GNUC__ && defined HAVE_MMX
    if( cpu & X264_CPU_MMXEXT )
        ret = filter_line_mmx2;
    if( cpu & (X264_CPU_SSE2|X264_CPU_SSE2_IS_SLOW|X264_CPU_SSE2_IS_FAST) )
        ret = filter_line_sse2;
    if( cpu & X264_CPU_SSSE3 )
        ret = filter_line_ssse3;
#endif
    if( high_depth )
        ret = filter_line_c_16bit;
    return ret;
}
