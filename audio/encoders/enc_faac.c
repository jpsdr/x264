#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include <faac.h>
#include <assert.h>

typedef struct enc_faac_t
{
    audio_info_t info;
    hnd_t filter_chain;
    int64_t packet_count;

    int finishing;
    faacEncHandle faac;
    int64_t last_sample;
    int64_t last_dts;
    void *samplebuffer;
    size_t bufsize;
    audio_packet_t *in;
} enc_faac_t;

static const int faac_channel_map[][8] = {
 { 0, },
 { 0, 1, },
 { 2, 0, 1, },
 { 2, 0, 1, 3, }, // seems faac assumes L R C S for 4ch by default
 { 2, 0, 1, 3, 4, },
 { 2, 0, 1, 4, 5, 3, },
 { 2, 0, 1, 6, 4, 5, 3, },
 { 2, 0, 1, 6, 7, 4, 5, 3, },
};

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    audio_hnd_t *chain = filter_chain;

    if( chain->info.channels > 8 )
    {
        x264_cli_log( "faac", X264_LOG_ERROR, "channels > 8 is not supported\n" );
        return NULL;
    }

    char **opts = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, "cutoff", "midside", "tns", "shortctl", NULL } );
    if( !opts )
    {
        x264_cli_log( "faac", X264_LOG_ERROR, "wrong audio options.\n" );
        return NULL;
    }

    enc_faac_t *h      = calloc( 1, sizeof( enc_faac_t ) );
    h->filter_chain    = chain;
    h->info            = chain->info;
    h->info.codec_name = "aac";
    h->info.samplerate = chain->info.samplerate;
    h->info.channels   = chain->info.channels;

    int is_vbr  = x264_otob( x264_get_option( "is_vbr", opts ), 1 );
    float brval;
    if( is_vbr )
        brval = x264_otof( x264_get_option( "bitrate", opts ), 100 );
    else
        brval = x264_otof( x264_get_option( "bitrate", opts ), 128 ); // dummy default value, must never be used

    int cutoff   = x264_clip3( x264_otoi( x264_get_option( "cutoff", opts ), -1 ), 0, h->info.samplerate >> 1 );
    int midside  = x264_otob( x264_get_option( "midside", opts ), 1 );
    int tns      = x264_otob( x264_get_option( "tns", opts ), 0 );
    int shortctl = x264_clip3( x264_otoi( x264_get_option( "shortctl", opts ), SHORTCTL_NORMAL ), SHORTCTL_NORMAL, SHORTCTL_NOLONG );

    x264_free_string_array( opts );

    unsigned long int samplesInput, maxBytesOutput;
    if( !( h->faac = faacEncOpen( h->info.samplerate, h->info.channels,
                                  &samplesInput, &maxBytesOutput ) ) )
    {
        x264_cli_log( "faac", X264_LOG_ERROR, "failed to open encoder\n" );
        goto error;
    }

    h->info.framelen = samplesInput / h->info.channels;
    h->bufsize       = maxBytesOutput;

    faacEncConfigurationPtr config = faacEncGetCurrentConfiguration( h->faac );

    config->aacObjectType = LOW;
    config->mpegVersion   = MPEG4;
    config->useTns        = tns;
    config->shortctl      = shortctl;
    config->allowMidside  = midside;
    config->bandWidth     = cutoff;
    config->outputFormat  = 0; /* RAW aac bytestream without ADTS header */
    config->inputFormat   = FAAC_INPUT_FLOAT;
    if ( is_vbr )
    {
        config->quantqual = x264_clip3( brval, 10, 500 );
        config->bitRate   = 0;
    }
    else
        config->bitRate   = 1000.0f * brval / h->info.channels; /* bitrate per channel */

    config->useLfe        = !!(h->info.channels > 5);
    if( h->info.channels > 3 )
    {
        for( int i=0; i<h->info.channels; i++ )
            config->channel_map[i] = faac_channel_map[h->info.channels-1][i];
    }

    if ( !faacEncSetConfiguration( h->faac, config ) )
    {
        x264_cli_log( "faac", X264_LOG_ERROR, "failed to configure encoder\n" );
        goto error;
    }

    unsigned char *asc;
    long unsigned int asc_size;
    faacEncGetDecoderSpecificInfo( h->faac, &asc, &asc_size );
    if( !asc || asc_size <= 0 )
    {
        x264_cli_log( "faac", X264_LOG_ERROR, "failed to retrieve decoder specific info\n" );
        goto error;
    }
    h->info.extradata       = asc;
    h->info.extradata_size  = asc_size;

    h->info.chansize   = 4;
    h->info.samplesize = 4 * h->info.channels;
    h->info.framesize  = h->info.framelen * h->info.samplesize;
    h->info.depth      = 32;
    h->info.timebase   = (timebase_t) { 1, h->info.samplerate };
    h->info.last_delta = h->info.framelen;

    h->last_dts = INVALID_DTS;
    h->samplebuffer = NULL;

    x264_cli_log( "audio", X264_LOG_INFO, "opened faac encoder (%s: %g%s, samplerate: %dhz)\n",
                  ( !is_vbr ? "bitrate" : "VBR" ), brval,
                  ( !is_vbr ? "kbps" : "" ), h->info.samplerate );

    return h;

error:
    if( h->faac )
        faacEncClose( h->faac );
    if( h )
        free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_faac_t *h = handle;

    return &h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_faac_t *h = handle;
    int ret;

    if( h->finishing )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->bufsize );

    do
    {
        if( h->in && h->in->flags & AUDIO_FLAG_EOF )
        {
            h->finishing = 1;
            goto error; // Not an error here but it'd do the same handling
        }
        x264_af_free_packet( h->in );

        if( !( h->in = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen ) ) )
            goto error;
        if( h->last_dts == INVALID_DTS )
            h->last_dts = h->last_sample;
        h->last_sample += h->in->samplecount;

        if( h->samplebuffer )
            free( h->samplebuffer );
        h->samplebuffer = x264_af_interleave2( SMPFMT_FLT, h->in->samples, h->info.channels, h->in->samplecount );
        for( int i=0; i<h->in->samplecount*h->info.channels; i++ )
            ((float *)h->samplebuffer)[i] *= 32768.0f;

        ret = faacEncEncode( h->faac, h->samplebuffer, h->in->samplecount*h->info.channels, out->data, h->bufsize );
        if( ret < 0 )
        {
            x264_cli_log( "faac", X264_LOG_ERROR, "failed to encode audio\n" );
            goto error;
        }
        out->size = ret;

    } while( ret <= 0 );

    out->dts = h->last_dts;
    h->last_dts += h->info.framelen;
    return out;

error:
    if( h->in )
    {
        x264_af_free_packet( h->in );
        h->in = NULL;
    }
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_faac_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t encoder )
{
    enc_faac_t *h = encoder;
    int ret;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->bufsize );

    ret = faacEncEncode( h->faac, NULL, 0, out->data, h->bufsize );
    if( ret <= 0 )
        goto error;
    out->size = ret;
    out->dts = h->last_dts;
    h->last_dts += h->info.framelen;
    return out;

error:
    x264_af_free_packet( out );
    return NULL;
}

static void faac_close( hnd_t handle )
{
    enc_faac_t *h = handle;

    faacEncClose( h->faac );
    if( h->in )
        x264_af_free_packet( h->in );
    if( h->samplebuffer )
        free( h->samplebuffer );
    if( h->info.extradata )
        free( h->info.extradata );
    free( h );
}

static void faac_help( const char * const encoder_name )
{
    printf( "      * faac encoder help\n" );
    printf( "        --aquality        VBR quality [100]\n" );
    printf( "                             10 (lowest) to 500 (highest)\n" );
    printf( "        --abitrate        Bitrate in kbits/s [128]\n" );
    printf( "                          Roughly 32-320 are achieved at typical\n" );
    printf( "                          2ch, 44100Hz, 16bit PCM audio\n" );
    printf( "        --aextraopt       Should be used only for extremely fine tunes\n" );
    printf( "                             cutoff: set cutoff in Hz [auto]\n" );
    printf( "                             midside: use of M/S stereo [1 (on)]\n" );
    printf( "                             tns: use of temporal noise shaping [0 (off)]\n" );
    printf( "                             shortctl: enforce block type [0 (auto)]\n" );
    printf( "                                - 0 (auto), 1 (no SHORT), 2 (no LONG)\n" );
    printf( "\n" );
}

const audio_encoder_t audio_encoder_faac =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = faac_close,
    .show_help       = faac_help,
    .is_valid_encoder = NULL
};
