#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include "lame/lame.h"
#include <assert.h>

typedef struct enc_lame_t
{
    audio_info_t info;
    hnd_t filter_chain;
    int64_t packet_count;

    int finishing;
    lame_global_flags *lame;
    int64_t last_sample;
    int64_t last_dts;
    uint8_t *buffer;
    size_t buf_index;
    size_t bufsize;
    audio_packet_t *in;
} enc_lame_t;

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    audio_hnd_t *chain = filter_chain;
    if( chain->info.channels > 2 )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "only mono or stereo audio is supported\n" );
        return 0;
    }

    char **opts = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, "samplerate", NULL } );
    if( !opts )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "wrong audio options.\n" );
        return 0;
    }

    enc_lame_t *h   = calloc( 1, sizeof( enc_lame_t ) );
    h->filter_chain = chain;
    h->info         = chain->info;
    h->info.codec_name = "mp3";

    int is_vbr  = x264_otob( x264_get_option( "is_vbr", opts ), 1 );
    float brval;
    if( is_vbr )
        brval = x264_otof( x264_get_option( "bitrate", opts ), 6.0 );
    else
        brval = x264_otof( x264_get_option( "bitrate", opts ), 128 ); // dummy default value, must never be used

    int quality = x264_otof( x264_get_option( "quality", opts ), 0 );

    h->info.samplerate = x264_otof( x264_get_option( "samplerate", opts ), chain->info.samplerate );

    x264_free_string_array( opts );

    h->info.extradata      = NULL;
    h->info.extradata_size = 0;

    h->lame = lame_init();
    // lame expects floats to be in the same range as shorts, our floats are -1..1 so tell it to scale
    lame_set_scale( h->lame, 32768 );
    lame_set_in_samplerate( h->lame, chain->info.samplerate );
    lame_set_out_samplerate( h->lame, h->info.samplerate );
    lame_set_num_channels( h->lame, h->info.channels );
    lame_set_quality( h->lame, quality );
    lame_set_VBR( h->lame, vbr_default );

    if( !is_vbr )
    {
        lame_set_VBR( h->lame, vbr_off );
        lame_set_brate( h->lame, (int) brval );
    }
    else
        lame_set_VBR_quality( h->lame, brval );

    lame_set_bWriteVbrTag( h->lame, 0 );

    lame_init_params( h->lame );

    h->info.framelen   = lame_get_framesize( h->lame );
    h->info.framesize  = h->info.framelen * 2;
    h->info.chansize   = 2;
    h->info.samplesize = 2 * h->info.channels;
    h->info.depth      = 16;
    h->info.timebase   = (timebase_t) { 1, h->info.samplerate };
    h->info.last_delta = h->info.framelen;

    h->bufsize = 125 * h->info.framelen / 100 + 7200; // from lame.h, largest frame that the encoding functions may return
    h->buffer = malloc( h->bufsize );
    h->buf_index = 0;
    h->last_dts = INVALID_DTS;

    x264_cli_log( "audio", X264_LOG_INFO, "opened lame mp3 encoder (%s: %g%s, quality: %d, samplerate: %dhz)\n",
                  ( !is_vbr ? "bitrate" : "VBR" ), brval,
                  ( !is_vbr ? "kbps" : "" ), quality, h->info.samplerate );

    return h;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_lame_t *h = handle;

    return &h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

/* Ripped from ffmpeg's libmp3lame.c */
static const int samplerates_tab[] = {
    44100, 48000,  32000, 22050, 24000, 16000, 11025, 12000, 8000, 0,
};

// frame bitrate table in kbits/s
static const int bitrate_tab[2][3][15] = {
    {  {   0,  32,  64,  96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
       {   0,  32,  48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384},
       {   0,  32,  40,  48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320},
    },
    {  {   0,  32,  48,  56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256},
       {   0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160},
       {   0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160},
    },
};

// frame length in samples/frame
static const int samples_per_frame_tab[2][3] = {
    {  384,     1152,    1152 },
    {  384,     1152,     576 },
};

static const int bits_per_slot_tab[3] = {
    32, 8, 8,
};

static int const mode_tab[4] = {
    2, 3, 1, 0,
};

static int mp3len( uint8_t *data )
{
    uint32_t header       = ((data[0]<<24) | (data[1] << 16) | (data[2] << 8) | data[3]);
    int layer_id          = 3 - ((header >> 17) & 0x03);
    int bitrate_id        = ((header >> 12) & 0x0f);
    int samplerate_id     = ((header >> 10) & 0x03);
    int bits_per_slot     = bits_per_slot_tab[layer_id];
    int mode              = mode_tab[(header >> 19) & 0x03];
    int mpeg_id           = !!(mode > 0) ;
    int is_pad            = ((header >> 9) & 0x01);
    int samplerate        = samplerates_tab[samplerate_id]>>mode;
    int samples_per_frame = samples_per_frame_tab[mpeg_id][layer_id];
    int bitrate           = bitrate_tab[mpeg_id][layer_id][bitrate_id]*1000;

    if ( (( header >> 21 ) & 0x7ff) != 0x7ff || mode == 3 || layer_id == 3 || samplerate_id == 3 ) {
        return -1;
    }

    return samples_per_frame * bitrate / (bits_per_slot * samplerate) + is_pad;
}

static int get_next_mp3frame( hnd_t handle, uint8_t *data )
{
    enc_lame_t *h = handle;
    int outlen;

    if( h->buf_index < 4 )
        return 0;

    outlen = mp3len( h->buffer );

    if( (outlen <= 0) || (outlen > h->buf_index) )
        return 0;

    memcpy( data, h->buffer, outlen );
    h->buf_index -= outlen;
    memmove( h->buffer, h->buffer+outlen, h->buf_index);

    return outlen;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lame_t *h = handle;
    int len;

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

        len = lame_encode_buffer_float( h->lame, h->in->samples[0], h->in->samples[1],
                                              h->in->samplecount, h->buffer + h->buf_index, h->bufsize - h->buf_index );

        if( len < 0 )
            goto error;

        h->buf_index += len;

        out->size = get_next_mp3frame( h, out->data );
    } while( !out->size );

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
    ((enc_lame_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t encoder )
{
    enc_lame_t *h = encoder;
    int len;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->bufsize );

    len = lame_encode_flush( h->lame, h->buffer + h->buf_index, h->bufsize - h->buf_index );
    if( len < 0 )
        goto error;

    h->buf_index += len;

    out->size = get_next_mp3frame( h, out->data );
    if( !out->size )
        goto error;

    out->dts = h->last_dts;
    h->last_dts += h->info.framelen;
    return out;

error:
    x264_af_free_packet( out );
    return NULL;
}

static void mp3_close( hnd_t handle )
{
    enc_lame_t *h = handle;

    lame_close( h->lame );
    if( h->in )
        x264_af_free_packet( h->in );
    if( h->buffer )
        free( h->buffer );
    free( h );
}

static void mp3_help( const char * const encoder_name )
{
    printf( "      * lame encoder help\n" );
    printf( "        --aquality        VBR quality [6]\n" );
    printf( "                             9 (lowest) to 0 (highest)\n" );
    printf( "        --abitrate        Enables CBR mode. Bitrate should be one of the values below\n" );
    printf( "                           - for 32000Hz or 44100Hz or 48000Hz\n" );
    printf( "                             32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320\n" );
    printf( "                           - for 16000Hz or 22050Hz or 24000Hz\n" );
    printf( "                             8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160\n" );
    printf( "                           - for 8000Hz or 11025Hz or 12000Hz\n" );
    printf( "                             8, 16, 24, 32, 40, 48, 56, 64\n" );
    printf( "        --asamplerate     Output samplerate. Should be one of the values below\n" );
    printf( "                             8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000\n" );
    printf( "        --acodec-quality  Internal algorithmic complexity [0]\n" );
    printf( "                             9 (poor quality) to 0 (best quality)\n" );
    printf( "\n" );
}

const audio_encoder_t audio_encoder_lame =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = mp3_close,
    .show_help       = mp3_help,
    .is_valid_encoder = NULL
};
