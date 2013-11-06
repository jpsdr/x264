/*****************************************************************************
 * matroska_ebml.h: matroska muxer utilities
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

#ifndef X264_MATROSKA_EBML_H
#define X264_MATROSKA_EBML_H

/* Matroska display size units from the spec */
#define DS_PIXELS        0
#define DS_CM            1
#define DS_INCHES        2
#define DS_ASPECT_RATIO  3

#define MK_MAX_TRACKS 4

typedef enum {
    MK_TRACK_VIDEO = 1,
    MK_TRACK_AUDIO = 2
} mk_track_type;

typedef enum {
    MK_LACING_NONE  = 0,
    MK_LACING_XIPH  = 1,
    MK_LACING_FIXED = 2,
    MK_LACING_EBML  = 3
} mk_lacing_type;

typedef struct {
    unsigned width;
    unsigned height;
    unsigned display_width;
    unsigned display_height;
    int display_size_units;
    int stereo_mode;
} mk_video_info_t;

typedef struct {
    unsigned channels;
    unsigned samplerate;
    unsigned output_samplerate;
    unsigned bit_depth;
} mk_audio_info_t;

typedef union {
    mk_video_info_t v;
    mk_audio_info_t a;
} mk_track_info_t;

typedef struct {
    mk_track_type type;
    mk_lacing_type lacing;
    unsigned id;
    const char *codec_id;
    void *codec_private;
    unsigned codec_private_size;
    int64_t default_frame_duration;
    mk_track_info_t info;
} mk_track_t;

typedef struct mk_writer mk_writer;

mk_writer *mk_create_writer( const char *filename );

int mk_write_header( mk_writer *w, const char *writing_app, int64_t timescale,
                     mk_track_t *tracks, int track_count );
int mk_start_frame( mk_writer *w );
int mk_end_frame( mk_writer *w, uint32_t track_id );
int mk_add_frame_data( mk_writer *w, const void *data, unsigned size );
int mk_set_frame_flags( mk_writer *w, int64_t timestamp, int keyframe, int skippable, uint32_t track_id );
int mk_close( mk_writer *w, int64_t *last_delta );

/* Supported (or may be supported in the future) audio codecs in x264's matroska muxer. */
/* We do not care about A_MS/ACM (avi compatible) type codecs.                          */
/* For reference, see http://haali.su/mkv/codecs.pdf                                    */
#if HAVE_AUDIO
#define MK_AUDIO_TAG_AAC       "A_AAC"
#define MK_AUDIO_TAG_AC3       "A_AC3"
#define MK_AUDIO_TAG_EAC3      "A_EAC3"
#define MK_AUDIO_TAG_DTS       "A_DTS"
#define MK_AUDIO_TAG_FLAC      "A_FLAC"
#define MK_AUDIO_TAG_MP1       "A_MPEG/L1"
#define MK_AUDIO_TAG_MP2       "A_MPEG/L2"
#define MK_AUDIO_TAG_MP3       "A_MPEG/L3"
#define MK_AUDIO_TAG_PCM_FLOAT "A_PCM/FLOAT/IEEE"
#define MK_AUDIO_TAG_PCM_LE    "A_PCM/INT/LIT"
#define MK_AUDIO_TAG_PCM_BE    "A_PCM/INT/BIG" /* Do we need this? */
#define MK_AUDIO_TAG_MLP       "A_MLP"
#define MK_AUDIO_TAG_TRUEHD    "A_TRUEHD"
#define MK_AUDIO_TAG_TTA       "A_TTA1"
#define MK_AUDIO_TAG_VORBIS    "A_VORBIS"
#endif /* HAVE_AUDIO */

#endif /* X264_MATROSKA_EBML_H */
