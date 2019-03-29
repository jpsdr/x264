#include "filters/audio/internal.h"
#include <stdint.h>
#include <math.h>

float **x264_af_get_buffer( unsigned channels, unsigned samplecount )
{
    float **samples = malloc( sizeof( float* ) * channels );
    for( int i = 0; i < channels; i++ ) {
        samples[i] = malloc( sizeof( float ) * samplecount );
    }
    return samples;
}

int x264_af_resize_buffer( float **buffer, unsigned channels, unsigned samplecount )
{
    for( int c = 0; c < channels; c++ )
    {
        if( !(buffer[c] = realloc( buffer[c], sizeof( float ) * samplecount )) )
            return -1;
    }
    return 0;
}

int x264_af_resize_fill_buffer( float **buffer, unsigned out_samplecount, unsigned channels, unsigned in_samplecount, float value )
{
    if( x264_af_resize_buffer( buffer, channels, out_samplecount ) )
        return -1;
    for( int c = 0; c < channels; c++ )
        for( int s = in_samplecount; s < out_samplecount; s++ )
            buffer[c][s] = value;
    return 0;
}

float **x264_af_dup_buffer( float **buffer, unsigned channels, unsigned samplecount )
{
    float **buf = x264_af_get_buffer( channels, samplecount );
    for( int c = 0; c < channels; c++ )
        memcpy( buf[c], buffer[c], samplecount );
    return buf;
}

void x264_af_free_buffer( float **buffer, unsigned channels )
{
    if( !buffer )
        return;
    for( int c = 0; c < channels; c++ )
        free( buffer[c] );
    free( buffer );
}

int x264_af_cat_buffer( float **buf, unsigned bufsamples, float **in, unsigned insamples, unsigned channels )
{
    if( x264_af_resize_buffer( buf, channels, bufsamples + insamples ) < 0 )
        return -1;
    for( int c = 0; c < channels; c++ )
        for( int s = 0; s < insamples; s++ )
            buf[c][bufsamples+s] = in[c][s];
    return 0;
}

static inline int x264_is_interleaved_format(int fmt)
{
    return fmt <= SMPFMT_DBL;
}

static inline int x264_interleaved_format(int fmt)
{
    if (fmt >= SMPFMT_U8P && fmt <= SMPFMT_DBLP)
        fmt -= (SMPFMT_U8P - SMPFMT_U8);
    return fmt;
}

float **x264_af_deinterleave ( float *samples, unsigned channels, unsigned samplecount )
{
    float **deint = x264_af_get_buffer( channels, samplecount );
    for( int s = 0; s < samplecount; s++ )
        for( int c = 0; c < channels; c++ )
            deint[c][s] = samples[s*channels + c];
    return deint;
}

float *x264_af_interleave ( float **in, unsigned channels, unsigned samplecount )
{
    float *inter = malloc( sizeof( float ) * channels * samplecount );
    for( int c = 0; c < channels; c++ )
        for( int s = 0; s < samplecount; s++ )
            inter[s*channels + c] = in[c][s];
    return inter;
}

float **x264_af_deinterleave2( uint8_t *samples, enum SampleFmt fmt, unsigned channels, unsigned samplecount )
{
    float  *in  = (float*) x264_af_convert( SMPFMT_FLT, samples, fmt, channels, samplecount );
    float **out;

    if (x264_is_interleaved_format(fmt))
        out = x264_af_deinterleave( in, channels, samplecount );
    else {
        unsigned i, j;
        out = x264_af_get_buffer( channels, samplecount );
        for (i = 0; i < channels; ++i) {
            float *base = &in[i * samplecount];
            for (j = 0; j < samplecount; ++j)
                out[i][j] = base[j];
        }
    }
    free( in );
    return out;
}

uint8_t *x264_af_interleave2( enum SampleFmt outfmt, float **in, unsigned channels, unsigned samplecount )
{
    float   *tmp = x264_af_interleave( in, channels, samplecount );
    uint8_t *out = x264_af_convert( outfmt, (uint8_t*) tmp, SMPFMT_FLT, channels, samplecount );
    free( tmp );
    return out;
}

uint8_t *x264_af_interleave3( enum SampleFmt outfmt, float **in, unsigned channels, unsigned samplecount, int *map )
{
    void *map_tmp[8];
    for( int i=0; i<channels; i++ )
        map_tmp[i] = in[i];
    for( int i=0; i<channels; i++ )
        in[i] = map_tmp[map[i]];
    float   *tmp = x264_af_interleave( in, channels, samplecount );
    uint8_t *out = x264_af_convert( outfmt, (uint8_t*) tmp, SMPFMT_FLT, channels, samplecount );
    free( tmp );
    return out;
}

static inline int samplesize( enum SampleFmt fmt )
{
    switch( fmt )
    {
    case SMPFMT_U8:
    case SMPFMT_U8P:
        return 1;
    case SMPFMT_S16:
    case SMPFMT_S16P:
        return 2;
    case SMPFMT_S32:
    case SMPFMT_S32P:
    case SMPFMT_FLT:
    case SMPFMT_FLTP:
        return 4;
    case SMPFMT_DBL:
    case SMPFMT_DBLP:
        return 8;
    default:
        return 0;
    }
}

#define CLIPFUN( num, type, min, max )                                  \
    static inline type clip##num( int64_t i ) {                         \
        return (type)( ( i > max ) ? max : ( ( i < min ) ? min : i ) ); \
    }
CLIPFUN( 8,  uint8_t, 0,         UINT8_MAX )
CLIPFUN( 16, int16_t, INT16_MIN, INT16_MAX )
CLIPFUN( 32, int32_t, INT32_MIN, INT32_MAX )
#undef CLIPFUN

uint8_t *x264_af_convert( enum SampleFmt outfmt, uint8_t *in, enum SampleFmt fmt, unsigned channels, unsigned samplecount )
{
    int totalsamples = channels * samplecount;
    int sz = samplesize( outfmt ) * totalsamples;
    uint8_t *out = malloc( sz );
    if( !out )
        return NULL;

    fmt = x264_interleaved_format(fmt);
    outfmt = x264_interleaved_format(outfmt);

    if( fmt == outfmt )
    {
        memcpy( out, in, sz );
        return out;
    }

#define CONVERT( ifmt, ofmt, otype, expr )                  \
    if( ifmt == fmt && ofmt == outfmt ) {                   \
        for( int i = 0; i < totalsamples; i++ )             \
        {                                                   \
            ((otype*)out)[i] = (otype)expr;                 \
        }                                                   \
        return out;                                         \
    }
#define INPUT( itype ) (((itype*)in)[i])

    CONVERT( SMPFMT_U8,  SMPFMT_S16, int16_t, (INPUT( uint8_t ) - 0x80) << 8 );
    CONVERT( SMPFMT_U8,  SMPFMT_S32, int32_t, (INPUT( uint8_t ) - 0x80) << 24 );
    CONVERT( SMPFMT_U8,  SMPFMT_FLT, float,   (INPUT( uint8_t ) - 0x80) * (1.0 / (1<<7)) );
    CONVERT( SMPFMT_U8,  SMPFMT_DBL, double,  (INPUT( uint8_t ) - 0x80) * (1.0 / (1<<7)) );
    CONVERT( SMPFMT_S16, SMPFMT_U8,  uint8_t, (INPUT( int16_t ) >> 8) + 0x80 );
    CONVERT( SMPFMT_S16, SMPFMT_S32, int32_t,  INPUT( int16_t ) << 16 );
    CONVERT( SMPFMT_S16, SMPFMT_FLT, float,    INPUT( int16_t ) * (1.0 / (1<<15)) );
    CONVERT( SMPFMT_S16, SMPFMT_DBL, double,   INPUT( int16_t ) * (1.0 / (1<<15)) );
    CONVERT( SMPFMT_S32, SMPFMT_U8,  uint8_t, (INPUT( int32_t ) >> 24) + 0x80 );
    CONVERT( SMPFMT_S32, SMPFMT_S16, int16_t,  INPUT( int32_t ) >> 16 );
    CONVERT( SMPFMT_S32, SMPFMT_FLT, float,    INPUT( int32_t ) * (1.0 / (1<<31)) );
    CONVERT( SMPFMT_S32, SMPFMT_DBL, double,   INPUT( int32_t ) * (1.0 / (1<<31)) );
    CONVERT( SMPFMT_FLT, SMPFMT_U8,  uint8_t,  clip8( lrintf(  INPUT( float )  * (1<<7) ) + 0x80 ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S16, int16_t, clip16( lrintf(  INPUT( float )  * (1<<15) ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S32, int32_t, clip32( llrintf( INPUT( float )  * (1U<<31) ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_DBL, double,   INPUT( float ) );
    CONVERT( SMPFMT_DBL, SMPFMT_U8,  uint8_t,  clip8( lrintf(  INPUT( double ) * (1<<7) ) + 0x80 ) );
    CONVERT( SMPFMT_DBL, SMPFMT_S16, int16_t, clip16( lrintf(  INPUT( double ) * (1<<15) ) ) );
    CONVERT( SMPFMT_DBL, SMPFMT_S32, int32_t, clip32( llrintf( INPUT( double ) * (1U<<31) ) ) );
    CONVERT( SMPFMT_DBL, SMPFMT_FLT, float,    INPUT( double ) );
#undef INPUT
#undef CONVERT
    free( out );
    return NULL;
}
