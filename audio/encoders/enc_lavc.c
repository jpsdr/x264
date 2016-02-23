#include "audio/encoders.h"
#include "filters/audio/internal.h"
#undef DECLARE_ALIGNED
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavresample/avresample.h"
#include "libavutil/channel_layout.h"

#include <assert.h>

typedef struct enc_lavc_t
{
    audio_info_t info;
    audio_info_t preinfo;
    hnd_t        filter_chain;
    int          finishing;
    int64_t      last_sample;
    int          buf_size;
    int64_t      last_dts;

    AVCodecContext         *ctx;
    enum AVSampleFormat     smpfmt;
    AVFrame                *frame;
    AVAudioResampleContext *avr;
} enc_lavc_t;

static int is_encoder_available( const char *name, void **priv )
{
    avcodec_register_all();
    AVCodec *enc = NULL;

    if( (enc = avcodec_find_encoder_by_name( name )) )
    {
        if( priv )
            *priv = enc;
        return 0;
    }

    return -1;
}

#define MODE_VBR     0x01
#define MODE_BITRATE 0x02
#define MODE_IGNORED MODE_VBR|MODE_BITRATE

static const struct {
    enum AVCodecID id;
    const char *name;
    uint8_t mode;
    float default_brval;
} ffcodecs[] = {
   /* AV_CODEC_ID,           name,        allowed mode,  default quality/bitrate */
    { AV_CODEC_ID_MP2,       "mp2",       MODE_BITRATE,    112 },
    { AV_CODEC_ID_VORBIS,    "vorbis",    MODE_VBR,        5.0 },
    { AV_CODEC_ID_AAC,       "aac",       MODE_BITRATE,     96 },
    { AV_CODEC_ID_AC3,       "ac3",       MODE_BITRATE,     96 },
    { AV_CODEC_ID_EAC3,      "eac3",      MODE_BITRATE,     96 },
    { AV_CODEC_ID_ALAC,      "alac",      MODE_IGNORED,     64 },
    { AV_CODEC_ID_AMR_NB,    "amrnb",     MODE_BITRATE,   12.2 },
    { AV_CODEC_ID_AMR_WB,    "amrwb",     MODE_BITRATE,  12.65 },
    { AV_CODEC_ID_PCM_F32BE, "pcm_f32be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_F32LE, "pcm_f32le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_F64BE, "pcm_f64be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_F64LE, "pcm_f64le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S16BE, "pcm_s16be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S16LE, "pcm_s16le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S24BE, "pcm_s24be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S24LE, "pcm_s24le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S32BE, "pcm_s32be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S32LE, "pcm_s32le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S8,    "pcm_s8",    MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U16BE, "pcm_u16be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U16LE, "pcm_u16le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U24BE, "pcm_u24be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U24LE, "pcm_u24le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U32BE, "pcm_u32be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U32LE, "pcm_u32le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U8,    "pcm_u8",    MODE_IGNORED,      0 },
    { AV_CODEC_ID_NONE,      NULL, },
};

static int get_linesize( int nb_channels, int nb_samples, enum AVSampleFormat sample_fmt )
{
    int linesize;
    av_samples_get_buffer_size( &linesize, nb_channels, nb_samples, sample_fmt, 0 );
    return linesize;
}

static int resample_audio( AVAudioResampleContext *avr, AVFrame *frame, audio_packet_t *pkt )
{
    int channels = av_get_channel_layout_nb_channels( frame->channel_layout );
        if( channels == 0 )
        {
            int channels = 2;
        }
    int out_linesize = get_linesize( channels, frame->nb_samples, frame->format );
    int in_linesize  = get_linesize( pkt->channels, pkt->samplecount, AV_SAMPLE_FMT_FLTP );
    if( avresample_convert( avr, (void **)frame->data, out_linesize, frame->nb_samples,
                                 (void **)pkt->samples, in_linesize, pkt->samplecount ) < 0 )
        return -1;

    int planes = av_sample_fmt_is_planar( frame->format ) ? channels : 1;
    for( int i = 0; i < planes; i++ )
        frame->linesize[i] = out_linesize;

    return 0;
}

static int encode_audio( AVCodecContext *ctx, audio_packet_t *out, AVFrame *frame )
{
    AVPacket avpkt;
    av_init_packet( &avpkt );
    avpkt.data = NULL;
    avpkt.size = 0;

    int got_packet = 0;

    if( avcodec_encode_audio2( ctx, &avpkt, frame, &got_packet ) < 0 )
        return -1;

    if( got_packet )
    {
        out->size = avpkt.size;
        memcpy( out->data, avpkt.data, avpkt.size );
    }
    else
        out->size = 0;

    return 0;
}

#define ISCODEC( name ) (!strcmp( h->info.codec_name, #name ))

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    enc_lavc_t *h = calloc( 1, sizeof( enc_lavc_t ) );
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->preinfo = h->info = chain->info;

    char **opts = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, "profile", "cutoff", NULL } );
    if( !opts )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "wrong audio options.\n" );
        return NULL;
    }

    const char *codecname = x264_get_option( "codec", opts );
    RETURN_IF_ERR( !codecname, "lavc", NULL, "codec not specified" );

    avcodec_register_all();

    AVCodec *codec = NULL;
    RETURN_IF_ERR( is_encoder_available( codecname, (void **)&codec ),
                   "lavc", NULL, "codec %s not supported or compiled in\n", codecname );

    int i, j;
    h->info.codec_name = NULL;
    for( i = 0; ffcodecs[i].id != AV_CODEC_ID_NONE; i++ )
    {
        if( codec->id == ffcodecs[i].id )
        {
            h->info.codec_name = ffcodecs[i].name;
            break;
        }
    }
    RETURN_IF_ERR( !h->info.codec_name, "lavc", NULL, "failed to set codec name for muxer\n" );

    for( j = 0; codec->sample_fmts[j] != -1; j++ )
    {
        // Prefer floats...
        if( codec->sample_fmts[j] == AV_SAMPLE_FMT_FLT )
        {
            h->smpfmt = AV_SAMPLE_FMT_FLT;
            break;
        }
        else if( h->smpfmt < codec->sample_fmts[j] ) // or the best possible sample format (is this really The Right Thing?)
            h->smpfmt = codec->sample_fmts[j];
    }
if( h->info.chanlayout == 0 )
{
        h->info.chanlayout = av_get_default_channel_layout( h->info.channels );
}

    h->ctx                  = avcodec_alloc_context3( NULL );
    h->ctx->sample_fmt      = h->smpfmt;
    h->ctx->sample_rate     = h->info.samplerate;
    h->ctx->channels        = h->info.channels;
    h->ctx->channel_layout  = h->info.chanlayout;
    h->ctx->time_base       = (AVRational){ 1, h->ctx->sample_rate };

    AVDictionary *avopts = NULL;
    av_dict_set( &avopts, "flags", "global_header", 0 ); // aac
    av_dict_set( &avopts, "strict", "-2", 0 );           // aac
    av_dict_set( &avopts, "reservoir", "1", 0 );         // mp3

    char *acutoff = x264_otos( x264_get_option( "cutoff", opts ), NULL );
    if( acutoff )
        av_dict_set( &avopts, "cutoff", acutoff, 0 );

    char *aprofile = x264_otos( x264_get_option( "profile", opts ), NULL );
    if( !strcmp(codecname, "libaacplus") )
        aprofile = "aac_he";
    if( aprofile )
    {
        av_dict_set( &avopts, "profile", aprofile, 0 );
        if( ISCODEC( aac ) && strstr( aprofile, "he" ) )
        {
            audio_aac_info_t *aacinfo = malloc( sizeof( audio_aac_info_t ) );
            aacinfo->has_sbr          = 1;
            h->info.opaque            = aacinfo;
        }
    }
    else if( ISCODEC( aac ) )
        av_dict_set( &avopts, "profile", "aac_low", 0 ); // TODO: decide by bitrate / quality

    int is_vbr = x264_otob( x264_get_option( "is_vbr", opts ), ffcodecs[i].mode & MODE_VBR ? 1 : 0 );

    RETURN_IF_ERR( ( ( !(ffcodecs[i].mode & MODE_BITRATE) && !is_vbr ) || ( !(ffcodecs[i].mode & MODE_VBR) && is_vbr ) ) && ( strcmp(codecname, "libfdk_aac") ),
                   "lavc", NULL, "libavcodec's %s encoder doesn't allow %s mode.\n", codecname, is_vbr ? "VBR" : "bitrate" );

    float default_brval = is_vbr ? ffcodecs[i].default_brval : ffcodecs[i].default_brval * h->ctx->channels;
    float brval = x264_otof( x264_get_option( "bitrate", opts ), default_brval );

    h->ctx->compression_level = x264_otof( x264_get_option( "quality", opts ), FF_COMPRESSION_DEFAULT );

    x264_free_string_array( opts );

    if( is_vbr )
    {
        av_dict_set( &avopts, "flags", "qscale", 0 );
        h->ctx->global_quality = FF_QP2LAMBDA * brval;
    }
    else
        h->ctx->bit_rate = lrintf( brval * 1000.0f );

    RETURN_IF_ERR( avcodec_open2( h->ctx, codec, &avopts ), "lavc", NULL, "could not open the %s encoder\n", codec->name );

    /* Set up frame buffer. */
    h->frame = avcodec_alloc_frame();
    if( !h->frame )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "avcodec_alloc_frame failed!\n" );
        goto error;
    }

    if( h->ctx->frame_size == 0 )
        /* frame_size == 0 indicates the actual frame size is based on the buf_size passed to avcodec_encode_audio2(). */
        h->ctx->frame_size = 1; /* arbitrary */

    h->frame->format         = h->ctx->sample_fmt;
    h->frame->channel_layout = h->ctx->channel_layout;
    h->frame->nb_samples     = h->ctx->frame_size;

    if( avcodec_default_get_buffer( h->ctx, h->frame ) < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "could not get frame buffer\n" );
        goto error;
    }

    /* Set up resampler. */
    h->avr = avresample_alloc_context();
    if( !h->avr )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "avresample_alloc_context failed!\n" );
        goto error;
    }

    av_opt_set_int( h->avr, "in_channel_layout",  h->info.chanlayout,       0 );
    av_opt_set_int( h->avr, "in_sample_fmt",      AV_SAMPLE_FMT_FLTP,       0 );
    av_opt_set_int( h->avr, "in_sample_rate",     h->info.samplerate,       0 );
    av_opt_set_int( h->avr, "out_channel_layout", h->frame->channel_layout, 0 );
    av_opt_set_int( h->avr, "out_sample_fmt",     h->frame->format,         0 );
    av_opt_set_int( h->avr, "out_sample_rate",    h->frame->sample_rate,    0 );

    av_opt_set_int( h->avr, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0 );

    if( avresample_open( h->avr ) < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "could not open resampler\n" );
        goto error;
    }

    h->info.framelen        = h->ctx->frame_size;
    h->info.timebase        = (timebase_t) { 1, h->ctx->sample_rate };
    h->info.last_delta      = h->info.framelen;
    h->info.depth           = av_get_bits_per_sample( h->ctx->codec->id );
    h->info.chansize        = IS_LPCM_CODEC_ID( h->ctx->codec->id )
                            ? h->info.depth / 8
                            : av_get_bytes_per_sample( h->ctx->sample_fmt );
    h->info.samplesize      = h->info.chansize * h->info.channels;
    h->info.framesize       = h->info.framelen * h->info.samplesize;

    h->buf_size = av_samples_get_buffer_size( NULL, h->ctx->channels, h->ctx->frame_size, h->ctx->sample_fmt, 0 ) * 3 / 2;
    h->last_dts = INVALID_DTS;

    h->info.extradata       = h->ctx->extradata;
    h->info.extradata_size  = h->ctx->extradata_size;

    x264_cli_log( "audio", X264_LOG_INFO, "opened libavcodec's %s encoder (%s%.1f%s, %dbits, %dch, %dhz)\n", codec->name,
                  is_vbr ? "V" : "", brval, is_vbr ? "" : "kbps", h->info.chansize * 8, h->info.channels, h->info.samplerate );
    return h;

error:
    if( h->ctx )
    {
        avcodec_close( h->ctx );
        av_free( h->ctx );
    }
    if( h->frame )
        av_free( h->frame );
    if( h->avr )
        avresample_free( &h->avr );
    free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_lavc_t *h = handle;

    return &h->info;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lavc_t *h = handle;
    if( h->finishing )
        return NULL;
    assert( h->ctx );

    audio_packet_t *smp = NULL;
    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->buf_size );

    while( out->size == 0 )
    {
        smp = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen );
        if( !smp )
            goto error; // not an error but need same handling

        out->samplecount = smp->samplecount;
        out->channels    = smp->channels;

        /* x264_af_interleave2 allocates only samplecount * channels * bytes per sample per single channel, */
        /* but lavc expects sample buffer contains h->ctx->frame_size samples (at least, alac encoder does). */
        /* If codec has capabilitiy to accept samples < default frame length, need to modify frame_size to */
        /* specify real sample counts in sample buffer, and if not, need to padding buffer. */
        if( smp->samplecount < h->info.framelen )
        {
            h->finishing = 1;
            if( !(smp->flags & AUDIO_FLAG_EOF) )
            {
                x264_cli_log( "lavc", X264_LOG_ERROR, "samples too few but not EOF???\n" );
                goto error;
            }

            if( h->ctx->codec->capabilities & CODEC_CAP_SMALL_LAST_FRAME )
                h->ctx->frame_size = h->info.last_delta = smp->samplecount;
            else
            {
                if( x264_af_resize_fill_buffer( smp->samples, h->info.framelen, h->info.channels, smp->samplecount, 0.0f ) )
                {
                    x264_cli_log( "lavc", X264_LOG_ERROR, "failed to expand buffer.\n" );
                    goto error;
                }
                smp->samplecount = h->info.last_delta = h->info.framelen;
            }
        }

        if( h->last_dts == INVALID_DTS )
            h->last_dts = h->last_sample;
        h->last_sample += smp->samplecount;

        h->frame->nb_samples  = smp->samplecount;

        if( resample_audio( h->avr, h->frame, smp ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "error resampling audio!\n" );
            goto error;
        }

        if( encode_audio( h->ctx, out, h->frame ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
            goto error;
        }

        x264_af_free_packet( smp );
    }

    if( out->size < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
        goto error;
    }

    out->dts     = h->last_dts;
    h->last_dts += h->info.framelen;
    return out;

error:
    if( smp )
        x264_af_free_packet( smp );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_lavc_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t handle )
{
    enc_lavc_t *h = handle;

    h->finishing = 1;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info     = h->info;
    out->channels = h->info.channels;
    out->data     = malloc( h->buf_size );

    if( encode_audio( h->ctx, out, NULL ) < 0 )
        goto error;

    if( out->size <= 0 )
        goto error;

    out->dts = h->last_dts;
    out->samplecount = h->info.framelen;
    h->last_dts     += h->info.framelen;
    return out;

error:
    x264_af_free_packet( out );
    return NULL;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static void lavc_close( hnd_t handle )
{
    enc_lavc_t *h = handle;

    avcodec_close( h->ctx );
    av_free( h->ctx );
    av_free( h->frame );
    avresample_free( &h->avr );
    free( h );
}

static void lavc_help_aac( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        --aquality        VBR quality\n" );
    printf( "                          Cannot be used for HE-AAC and possible values are:\n" );
    printf( "                             1, 2, 3, 4, 5\n" );
    printf( "                          1 is lowest and 5 is highest.\n" );
    printf( "        --abitrate        Enables bitrate mode [192]\n" );
    printf( "                          Bitrate should be one of the discrete preset values depending on\n" );
    printf( "                          profile, channels count, and samplerate.\n" );
    printf( "                          Examples for typical configurations\n" );
    printf( "                           - for 44100Hz to 48000Hz with 1ch\n" );
    printf( "                             LC: 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256\n" );
    printf( "                             HE: 12, 16, 24, 32, 40\n" );
    printf( "                           - for 44100Hz to 48000Hz with 2ch\n" );
    printf( "                             LC: 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320, 384, 512\n" );
    printf( "                             HE: 32, 40, 48, 56, 64, 80, 96, 112, 128\n" );
    printf( "                           - for 64000Hz to 96000Hz with 2ch\n" );
    printf( "                             LC: 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320, 384, 512, 768, 1024\n" );
    printf( "                             HE: 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 256\n" );
    printf( "                           - for 44100Hz to 48000Hz with 5.1ch\n" );
    printf( "                             LC: 160, 192, 224, 256, 288, 320, 384, 448, 512, 576, 640, 768, 1024, 1280\n");
    printf( "                             HE: 80, 96, 112, 128, 160, 192, 256\n");
    printf( "                           - for 64000Hz to 96000Hz with 5.1ch\n" );
    printf( "                             LC: 256, 288, 320, 384, 448, 512, 576, 640, 768, 1024, 1280, 1600, 1920, 2560\n");
    printf( "                             HE: 128, 160, 192, 256, 320, 384, 512, 640\n");
    printf( "                          The lower samplerate, the lower min/max values are applied\n" );
    printf( "        --aextraopt       Profile and bitrate mode\n" );
    printf( "                             cutoff: set cutoff in Hz\n" );
    printf( "                             profile : profile for aac codec [\"aac_low\"]\n" );
    printf( "                                       \"aac_low\", \"aac_he\"\n" );
    printf( "\n" );
}

static void lavc_help_amrnb( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        Accepts only mono (1ch), 8000Hz audio and not capable of quality based VBR\n" );
    printf( "        --abitrate        Only one of the values below can be acceptable\n" );
    printf( "                             4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2\n" );
    printf( "\n" );
}

static void lavc_help( const char * const encoder_name )
{
    AVCodec *enc = NULL;

    if( is_encoder_available( encoder_name, (void **)&enc ) )
        return;

#define SHOWHELP( encoder, helpname ) if( !strcmp( enc->name, #encoder ) ) lavc_help_##helpname ( #encoder );
    SHOWHELP( libfdk_aac, aac );
    SHOWHELP( libopencore_amrnb, amrnb );
#undef SHOWHELP

    return;
}

const audio_encoder_t audio_encoder_lavc =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = lavc_close,
    .show_help       = lavc_help,
    .is_valid_encoder = is_encoder_available
};
