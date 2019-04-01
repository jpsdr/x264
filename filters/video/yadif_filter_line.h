/*****************************************************************************
 * yadif_filter_line.h: yadif (yet another deinterlacing filter)
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

#include <stdint.h>

struct yadif_context {
    uint8_t *output;
    const uint8_t *previous;
    const uint8_t *current;
    const uint8_t *next;
    int mode;
    int width;
    int stride;
    int parity;
};

typedef void (*filter_line_func)( struct yadif_context *yctx );

filter_line_func get_filter_func( unsigned int cpu, int high_depth );
