/*****************************************************************************
 * hqdn3d.c: x264 hqdn3d filter
 *****************************************************************************
 * Copyright (C) 2003 Daniel Moreno <comac@comac.darktech.org>
 * Avisynth port (C) 2005 Loren Merritt <lorenm@u.washington.edu>
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

#include <math.h>
#include "video.h"

#define NAME "hqdn3d"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )
#define ABS(A) ( (A) > 0 ? (A) : -(A) )
#define PARAM1_DEFAULT 4.0
#define PARAM2_DEFAULT 3.0
#define PARAM3_DEFAULT 6.0

cli_vid_filter_t hqdn3d_filter;

typedef struct {
    hnd_t prev_hnd;
    cli_vid_filter_t prev_filter;
    int coefs[4][512*16];
    unsigned int *line;
    unsigned short *frame[3];
    int first_frame;
    const x264_cli_csp_t *csp;
} hqdn3d_hnd_t;

static void help(int longhelp)
{
    printf("      "NAME":ls,cs,lt,ct\n");
    if(!longhelp)
        return;
    printf(
"            Denoises the image using mplayer's hqdn3d filter\n"
"            The four arguments are floats and are optional\n"
"            If any options are omitted, they will assume a\n"
"            value based on previous options that you did specify\n"
"            - ls = luma spatial filter strength [%.1lf]\n"
"            - cs = chroma spatial filter strength [%.1lf]\n"
"            - lt = luma temporal filter strength [%.1lf]\n"
"            - ct = chroma temporal filter strength [%.1lf]\n",
    PARAM1_DEFAULT, PARAM2_DEFAULT, PARAM3_DEFAULT,
    PARAM3_DEFAULT * PARAM2_DEFAULT / PARAM1_DEFAULT);
}

static void precalc_coefs(int *ct, double dist25)
{
    double c, simil, gamma_d = log(0.25) / log(1.0 - dist25/255.0 - 0.00001);
    for(int i = -256*16; i < 256*16; i++) {
        simil = 1.0 - ABS(i) / (16*255.0);
        c = pow(simil, gamma_d) * 65536.0 * (double)i / 16.0;
        ct[16*256+i] = (int)((c<0) ? (c-0.5) : (c+0.5));
    }
    ct[0] = (dist25 != 0);
}

static int init(hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info,
                x264_param_t *param, char *opt_string)
{
    double lum_spac, lum_tmp, chrom_spac, chrom_tmp;
    const x264_cli_csp_t *csp = x264_cli_get_csp(info->csp);

    FAIL_IF_ERROR(!(info->csp == X264_CSP_I420 || info->csp == X264_CSP_I422
        || info->csp == X264_CSP_I444 || info->csp == X264_CSP_YV12),
        "Only planar YUV images supported\n")

    hqdn3d_hnd_t *h = calloc(1, sizeof(hqdn3d_hnd_t));
    if(!h)
        return -1;

    h->line = calloc(1, info->width*sizeof(int));
    for(int i = 0; i < 3; i++)
    h->frame[i] = malloc(info->width * csp->width[i]
                         * info->height * csp->width[i]
                         * sizeof(short));
    if(!h->line || !h->frame[0] || !h->frame[1] || !h->frame[2])
        return -1;

    if(opt_string) {
        switch(sscanf(opt_string, "%lf,%lf,%lf,%lf",
                      &lum_spac, &chrom_spac, &lum_tmp, &chrom_tmp)) {
        case 1:
            lum_tmp = PARAM3_DEFAULT * lum_spac / PARAM1_DEFAULT;
            chrom_spac = PARAM2_DEFAULT * lum_spac / PARAM1_DEFAULT;
            chrom_tmp = lum_tmp * chrom_spac / lum_spac;
            break;
        case 2:
            lum_tmp = PARAM3_DEFAULT * lum_spac / PARAM1_DEFAULT;
        case 3:
            chrom_tmp = lum_tmp * chrom_spac / lum_spac;
        case 4:
            break;
        default:
            lum_spac = PARAM1_DEFAULT;
            lum_tmp = PARAM3_DEFAULT;
            chrom_spac = PARAM2_DEFAULT;
            chrom_tmp = lum_tmp * chrom_spac / lum_spac;
        }
    } else {
        lum_spac = PARAM1_DEFAULT;
        lum_tmp = PARAM3_DEFAULT;
        chrom_spac = PARAM2_DEFAULT;
        chrom_tmp = lum_tmp * chrom_spac / lum_spac;
    }

    precalc_coefs(h->coefs[0], lum_spac);
    precalc_coefs(h->coefs[1], lum_tmp);
    precalc_coefs(h->coefs[2], chrom_spac);
    precalc_coefs(h->coefs[3], chrom_tmp);

    x264_cli_log(NAME, X264_LOG_INFO,
        "using strengths %.1lf,%.1lf,%.1lf,%.1lf\n",
        lum_spac, chrom_spac, lum_tmp, chrom_tmp);

    h->csp = csp;
    h->first_frame = 1;
    h->prev_filter = *filter;
    h->prev_hnd = *handle;
    *handle = h;
    *filter = hqdn3d_filter;
    return 0;
}

static inline unsigned int lpm(unsigned int prev_mul,
                               unsigned int curr_mul, int* coef)
{
    int d_mul = prev_mul-curr_mul;
    unsigned int d = ((d_mul+0x10007FF)/(65536/16));
    return curr_mul + coef[d];
}

static void denoise_temporal(unsigned char *frame, unsigned short *frame_ant,
                             int w, int h, int stride, int *temporal)
{
    intptr_t x, y;
    unsigned int pixel_dst;
    for(y = 0; y < h; y++) {
        for(x = 0; x < w; x++) {
            pixel_dst = lpm(frame_ant[x]<<8, frame[x]<<16, temporal);
            frame_ant[x] = ((pixel_dst+0x1000007F)>>8);
            frame[x] = ((pixel_dst+0x10007FFF)>>16);
        }
        frame += stride;
        frame_ant += w;
    }
}

static void denoise_spacial(unsigned char *frame, unsigned int *line_ant,
                            int w, int h, int stride, int *spacial)
{
    intptr_t x, y, line_offs = 0;
    unsigned int pixel_ant, pixel_dst;

    /* First pixel has no left nor top neighbor. */
    pixel_dst = line_ant[0] = pixel_ant = frame[0]<<16;
    frame[0] = ((pixel_dst+0x10007FFF)>>16);

    /* First line has no top neighbor, only left. */
    for(x = 1; x < w; x++) {
        pixel_dst = line_ant[x] = lpm(pixel_ant, frame[x]<<16, spacial);
        frame[x] = ((pixel_dst+0x10007FFF)>>16);
    }

    for(y = 1; y < h; y++) {
        line_offs += stride;
        /* First pixel on each line doesn't have previous pixel */
        pixel_ant = frame[line_offs]<<16;
        pixel_dst = line_ant[0] = lpm(line_ant[0], pixel_ant, spacial);
        frame[line_offs] = ((pixel_dst+0x10007FFF)>>16);

        for(x = 1; x < w; x++) {
            /* The rest are normal */
            pixel_ant = lpm(pixel_ant, frame[line_offs+x]<<16, spacial);
            pixel_dst = line_ant[x] = lpm(line_ant[x], pixel_ant, spacial);
            frame[line_offs+x] = ((pixel_dst+0x10007FFF)>>16);
        }
    }
}

static void denoise(unsigned char *frame, unsigned int *line_ant,
                    unsigned short *frame_ant, int w, int h, int stride,
                    int *spacial, int *temporal)
{
    intptr_t x, y, line_offs = 0;
    unsigned int pixel_ant,pixel_dst;

    if(!spacial[0]) {
        denoise_temporal(frame, frame_ant, w, h, stride, temporal);
        return;
    }
    if(!temporal[0]) {
        denoise_spacial(frame, line_ant, w, h, stride, spacial);
        return;
    }

    /* First pixel has no left nor top neightbour. Only previous frame */
    line_ant[0] = pixel_ant = frame[0]<<16;
    pixel_dst = lpm(frame_ant[0]<<8, pixel_ant, temporal);
    frame_ant[0] = ((pixel_dst+0x1000007F)/256);
    frame[0] = ((pixel_dst+0x10007FFF)/65536);

    /* Fist line has no top neightbour. Only left one for each pixel and
     * last frame */
    for (x = 1; x < w; x++){
        line_ant[x] = pixel_ant = lpm(pixel_ant, frame[x]<<16, spacial);
        pixel_dst = lpm(frame_ant[x]<<8, pixel_ant, temporal);
        frame_ant[x] = ((pixel_dst+0x1000007F)/256);
        frame[x] = ((pixel_dst+0x10007FFF)/65536);
    }

    for (y = 1; y < h; y++){
        unsigned short* line_prev = &frame_ant[y*w];
        line_offs += stride;
        /* First pixel on each line doesn't have previous pixel */
        pixel_ant = frame[line_offs]<<16;
        line_ant[0] = lpm(line_ant[0], pixel_ant, spacial);
        pixel_dst = lpm(line_prev[0]<<8, line_ant[0], temporal);
        line_prev[0] = ((pixel_dst+0x1000007F)/256);
        frame[line_offs] = ((pixel_dst+0x10007FFF)/65536);

        for (x = 1; x < w; x++){
            /* The rest are normal */
            pixel_ant = lpm(pixel_ant, frame[line_offs+x]<<16, spacial);
            line_ant[x] = lpm(line_ant[x], pixel_ant, spacial);
            pixel_dst = lpm(line_prev[x]<<8, line_ant[x], temporal);
            line_prev[x] = ((pixel_dst+0x1000007F)/256);
            frame[line_offs+x] = ((pixel_dst+0x10007FFF)/65536);
        }
    }
}

static void init_data(uint8_t *source, unsigned short *dest,
                      int width, int height, int stride)
{
    for(int y = 0; y < height; y++)
        for(int x = 0; x < width; x++)
            dest[y*width+x] = source[y*stride+x]<<8;
}

static int get_frame(hnd_t handle, cli_pic_t *out, int frame)
{
    hqdn3d_hnd_t *h = handle;

    if(h->prev_filter.get_frame(h->prev_hnd, out, frame))
        return -1;

    for(int i = 0; i < 3; i++) {
        int width = out->img.width * h->csp->width[i];
        int height = out->img.height * h->csp->height[i];
        int stride = out->img.stride[i];
        if(h->first_frame)
            init_data(out->img.plane[i], h->frame[i], width, height, stride);
        denoise(out->img.plane[i], h->line, h->frame[i], width, height, stride,
            h->coefs[(i+1)&2], h->coefs[((i+1)&2)+1]);
    }

    h->first_frame = 0;
    return 0;
}

static int release_frame(hnd_t handle, cli_pic_t *pic, int frame)
{
    hqdn3d_hnd_t *h = handle;
    return h->prev_filter.release_frame(h->prev_hnd, pic, frame);
}

static void free_filter(hnd_t handle)
{
    hqdn3d_hnd_t *h = handle;
    h->prev_filter.free(h->prev_hnd);
    free(h->line);
    for(int i = 0; i < 3; i++)
        free(h->frame[i]);
    free(h);
}

cli_vid_filter_t hqdn3d_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
