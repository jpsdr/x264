#ifndef FILTERS_AUDIO_INTERNAL_H_
#define FILTERS_AUDIO_INTERNAL_H_

#include "filters/audio/audio_filters.h"

#define AUDIO_FILTER_COMMON     \
    const audio_filter_t *self; \
    audio_info_t info;          \
    struct audio_hnd_t *prev;

#define INIT_FILTER_STRUCT(filterstruct, structname)            \
    structname *h;                                              \
    do                                                          \
    {                                                           \
        h = calloc( 1, sizeof( structname ) );                  \
        if( !h )                                                \
            goto fail;                                          \
        h->self = &filterstruct;                                \
        h->prev = *handle;                                      \
        if( h->prev )                                           \
            h->info = h->prev->info;                            \
        *handle = h;                                            \
    } while( 0 )

#define IS_LPCM_CODEC_ID( id ) ((id) >= 0x10000 && (id) < 0x11000)

// Generic audio handle (used to access fields from AUDIO_FILTER_COMMON)
typedef struct audio_hnd_t
{
    AUDIO_FILTER_COMMON
} audio_hnd_t;

#define AF_LOG( handle, level, ... ) x264_cli_log( ((audio_hnd_t*)handle)->self->name, (level), __VA_ARGS__ )

#define AF_LOG_ERR( handle, ... )  AF_LOG( (handle), X264_LOG_ERROR  , __VA_ARGS__ )
#define AF_LOG_WARN( handle, ... ) AF_LOG( (handle), X264_LOG_WARNING, __VA_ARGS__ )

enum SampleFmt {
    SMPFMT_NONE = -1,
    SMPFMT_U8,
    SMPFMT_S16,
    SMPFMT_S32,
    SMPFMT_FLT,
    SMPFMT_DBL,

    SMPFMT_U8P,
    SMPFMT_S16P,
    SMPFMT_S32P,
    SMPFMT_FLTP,
    SMPFMT_DBLP
};

float  **x264_af_get_buffer   ( unsigned channels, unsigned samplecount );
int      x264_af_resize_buffer( float **buffer, unsigned channels, unsigned samplecount );
int      x264_af_resize_fill_buffer( float **buffer, unsigned out_samplecount, unsigned channels, unsigned in_samplecount, float value );
void     x264_af_free_buffer  ( float **buffer, unsigned channels );
float  **x264_af_dup_buffer   ( float **buffer, unsigned channels, unsigned samplecount );
int      x264_af_cat_buffer   ( float **buf, unsigned bufsamples, float **in, unsigned insamples, unsigned channels );

float  **x264_af_deinterleave ( float *samples, unsigned channels, unsigned samplecount );
float   *x264_af_interleave   ( float **in, unsigned channels, unsigned samplecount );

float  **x264_af_deinterleave2( uint8_t *samples, enum SampleFmt fmt, unsigned channels, unsigned samplecount );
uint8_t *x264_af_interleave2  ( enum SampleFmt outfmt, float **in, unsigned channels, unsigned samplecount );
uint8_t *x264_af_convert      ( enum SampleFmt outfmt, uint8_t *in, enum SampleFmt fmt, unsigned channels, unsigned samplecount );

uint8_t *x264_af_interleave3  ( enum SampleFmt outfmt, float **in, unsigned channels, unsigned samplecount, int *map );

#endif /* FILTERS_AUDIO_INTERNAL_H_ */
