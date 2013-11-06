#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include "typedef.h"
#include "enc_if.h"
#include <math.h>
#include <assert.h>

typedef struct enc_amrwb_3gpp_t
{
    audio_info_t info;
    hnd_t filter_chain;
    int64_t packet_count;

    void* amrwb_3gpp;
    Word16 dtx;
    Word16 mode;

    int finishing;
    int64_t last_sample;
    int64_t last_dts;
    size_t bufsize;
} enc_amrwb_3gpp_t;

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    audio_hnd_t *chain = filter_chain;

    if( chain->info.channels != 1 )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "only 1ch is supported\n" );
        return NULL;
    }
    if( chain->info.samplerate != 16000 )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "only 16kHz is supported\n" );
        return NULL;
    }

    char **opts = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, "dtx", NULL } );
    if( !opts )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "wrong audio options.\n" );
        return NULL;
    }

    enc_amrwb_3gpp_t *h     = calloc( 1, sizeof( enc_amrwb_3gpp_t ) );
    h->filter_chain         = chain;
    h->info                 = chain->info;
    h->info.codec_name      = "amrwb";
    h->info.chansize        = 2;
    h->info.samplesize      = 2 * h->info.channels;
    h->info.framelen        = L_FRAME16k;
    h->info.framesize       = h->info.framelen * h->info.samplesize;
    h->info.depth           = 16;
    h->info.timebase        = (timebase_t) { 1, h->info.samplerate };
    h->info.last_delta      = h->info.framelen;
    h->bufsize              = NB_SERIAL_MAX;

    h->info.extradata       = NULL; /* These are created in muxer modules. AMR-WB does not have general structure. */
    h->info.extradata_size  = 0;

    h->dtx = (Word16)x264_otob( x264_get_option( "dtx", opts ), 1 );
    float bitrate = x264_otof( x264_get_option( "bitrate", opts ), 23.85 );
    switch( lrintf( bitrate ) )
    {
    case 7: /* 6.60 */
        h->mode = 0; break;
    case 9: /* 8.85 */
        h->mode = 1; break;
    case 13: /* 12.65 */
        h->mode = 2; break;
    case 14: /* 14.25 */
        h->mode = 3; break;
    case 16: /* 15.85 */
        h->mode = 4; break;
    case 18: /* 18.25 */
        h->mode = 5; break;
    case 20: /* 19.85 */
        h->mode = 6; break;
    case 23: /* 23.05 */
        h->mode = 7; break;
    case 24: /* 23.85 */
        h->mode = 8; break;
    default:
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "invalid bitrate (%f) .\n", bitrate );
        goto error;
        break;
    }

    x264_free_string_array( opts );

    h->amrwb_3gpp = E_IF_init();
    if( !h->amrwb_3gpp )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "failed to initialize 3gpp amrwb encoder.\n" );
        goto error;
    }

    h->last_dts = INVALID_DTS;

    const char *bitrate_tbl[] = { "6.60", "8.85", "12.65", "14.25", "15.85", "18.25", "19.85", "23.05", "23.85" };
    x264_cli_log( "audio", X264_LOG_INFO, "opened amrwb_3gpp encoder (%skbps, dtx=%s)\n",
                  bitrate_tbl[h->mode], h->dtx ? "on" : "off", h->info.samplerate );

    return h;

error:
    if( h->amrwb_3gpp )
        E_IF_exit( h->amrwb_3gpp );
    if( h )
        free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_amrwb_3gpp_t *h = handle;
    return &h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_amrwb_3gpp_t *h = handle;
    audio_packet_t *in = NULL;

    if( h->finishing )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->bufsize );

    if( h->finishing )
        goto error; // Not an error here but it'd do the same handling

    if( !( in = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen ) ) )
        goto error;
    /* ensure buffer length */
    if( in->samplecount < h->info.framelen )
    {
        if( !(in->flags & AUDIO_FLAG_EOF) )
        {
            x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "samples too few but not EOF???\n" );
            goto error;
        }
        h->finishing = 1;
        if( x264_af_resize_fill_buffer( in->samples, h->info.framelen, h->info.channels, in->samplecount, 0.0f ) )
        {
            x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "failed to expand buffer.\n" );
            goto error;
        }
    }
    if( h->last_dts == INVALID_DTS )
        h->last_dts = h->last_sample;
    h->last_sample += in->samplecount;
    in->samplecount = h->info.framelen;

    /* convert to integer */
    void *samplebuffer = x264_af_interleave2( SMPFMT_S16, in->samples, h->info.channels, in->samplecount );
    x264_af_free_packet( in );
    in = NULL;

    out->size = E_IF_encode( h->amrwb_3gpp, h->mode, samplebuffer, out->data, h->dtx );
    free( samplebuffer );
    if( out->size == 0 )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "failed to encode audio.\n" );
        goto error;
    }

    out->dts = h->last_dts;
    h->last_dts += h->info.framelen;
    return out;

error:
    if( in )
        x264_af_free_packet( in );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_amrwb_3gpp_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t encoder )
{
    return NULL;
}

static void amrwb_3gpp_close( hnd_t handle )
{
    enc_amrwb_3gpp_t *h = handle;

    E_IF_exit( h->amrwb_3gpp );
    if( h->info.extradata )
        free( h->info.extradata );
    free( h );
}

static void amrwb_3gpp_help( const char * const codec_name )
{
    printf( "      * amrwb_3gpp encoder help\n" );
    printf( "        --abitrate        Bitrate in kbits/s. [23.85]\n" );
    printf( "        --dtx             Use DTX comfort noise generation [1].\n" );
}

const audio_encoder_t audio_encoder_amrwb_3gpp =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = amrwb_3gpp_close,
    .show_help       = amrwb_3gpp_help,
    .is_valid_encoder = NULL
};
