#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include <assert.h>

typedef struct enc_raw_t
{
    audio_info_t info;
    int finishing;
    hnd_t filter_chain;
    int64_t last_sample;
} enc_raw_t;

static hnd_t init( hnd_t filter_chain, const char *opts )
{
    assert( filter_chain );
    enc_raw_t *h = calloc( 1, sizeof( enc_raw_t ) );
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->info = chain->info;

    h->info.codec_name     = "raw";
    h->info.extradata      = NULL;
    h->info.extradata_size = 0;
    h->info.framelen       = h->info.framelen ? h->info.framelen : 1;
    h->info.depth          = 16;
    h->info.chansize       = h->info.depth / 8;
    h->info.samplesize     = h->info.chansize * h->info.channels;
    h->info.framesize      = h->info.framelen * h->info.samplesize;
    h->info.timebase       = (timebase_t) { 1, h->info.samplerate };

    x264_cli_log( "audio", X264_LOG_INFO, "opened raw encoder (%dbits, %dch, %dhz)\n",
                  h->info.chansize * 8, h->info.channels, h->info.samplerate );
    return h;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_raw_t *h = handle;

    return &h->info;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_raw_t *h = handle;
    if( h->finishing )
        return NULL;

    audio_packet_t *smp = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen );
    if( !smp )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    memcpy( out, smp, sizeof( audio_packet_t ) );
    out->info        = h->info;
    out->samples     = NULL;
    out->samplecount = smp->samplecount;
    out->data        = x264_af_interleave2( SMPFMT_S16, smp->samples, smp->channels, smp->samplecount );
    out->size        = smp->samplecount * h->info.samplesize;
    out->dts         = h->last_sample;
    out->info.last_delta = smp->samplecount;
    h->last_sample  += smp->samplecount;
    x264_af_free_packet( smp );

    return out;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_raw_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t handle )
{
    ((enc_raw_t*)handle)->finishing = 1;
    return NULL;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    x264_af_free_packet( packet );
}

static void raw_close( hnd_t handle )
{
    free( handle );
}

static void raw_help( const char * const codec_name )
{
    printf( "      * raw encoder help\n" );
    printf( "        Directly pass the decoded PCM samples (in native endian) to muxer.\n" );
    printf( "        All audio options except for --acodec and --audiofile are ignored.\n" );
    printf( "\n" );
}

const audio_encoder_t audio_encoder_raw =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = raw_close,
    .show_help       = raw_help,
    .is_valid_encoder = NULL
};
