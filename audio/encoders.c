#include "audio/encoders.h"

#include <assert.h>
#include <stdlib.h>

typedef struct {
    const char *codec;
    const char *name;
    const audio_encoder_t *encoder;
} audio_encoder_entry_t;

const audio_encoder_entry_t registered_audio_encoders[] = {
#if HAVE_AUDIO
    { "raw",        "raw",               &audio_encoder_raw, },
#if HAVE_LAME
    { "mp3",        "lame",              &audio_encoder_lame, },
#endif
#if HAVE_QT_AAC
    { "aac",        "qtaac",             &audio_encoder_qtaac, },
#endif
#if HAVE_FAAC
    { "aac",        "faac",              &audio_encoder_faac, },
#endif
#if HAVE_LAVF
#if HAVE_NONFREE
    { "aac",        "libfdk_aac",        &audio_encoder_lavc, },
#endif
    { "aac",        "libvo_aacenc",      &audio_encoder_lavc, },
    { "aac",        "aac",               &audio_encoder_lavc, },
#if HAVE_NONFREE
    { "aac",        "libaacplus",        &audio_encoder_lavc, },
#endif
    { "ac3",        "ac3",               &audio_encoder_lavc, },
    { "alac",       "alac",              &audio_encoder_lavc, },
    { "amrwb",      "libvo_amrwbenc",    &audio_encoder_lavc, },
    { "mp2",        "mp2",               &audio_encoder_lavc, },
    { "pcm_f32be",  "pcm_f32be",         &audio_encoder_lavc, },
    { "pcm_f32le",  "pcm_f32le",         &audio_encoder_lavc, },
    { "pcm_f64be",  "pcm_f64be",         &audio_encoder_lavc, },
    { "pcm_f64le",  "pcm_f64le",         &audio_encoder_lavc, },
    { "pcm_s16be",  "pcm_s16be",         &audio_encoder_lavc, },
    { "pcm_s16le",  "pcm_s16le",         &audio_encoder_lavc, },
    { "pcm_s24be",  "pcm_s24be",         &audio_encoder_lavc, },
    { "pcm_s24le",  "pcm_s24le",         &audio_encoder_lavc, },
    { "pcm_s32be",  "pcm_s32be",         &audio_encoder_lavc, },
    { "pcm_s32le",  "pcm_s32le",         &audio_encoder_lavc, },
    { "pcm_s8",     "pcm_s8",            &audio_encoder_lavc, },
    { "pcm_u16be",  "pcm_u16be",         &audio_encoder_lavc, },
    { "pcm_u16le",  "pcm_u16le",         &audio_encoder_lavc, },
    { "pcm_u24be",  "pcm_u24be",         &audio_encoder_lavc, },
    { "pcm_u24le",  "pcm_u24le",         &audio_encoder_lavc, },
    { "pcm_u32be",  "pcm_u32be",         &audio_encoder_lavc, },
    { "pcm_u32le",  "pcm_u32le",         &audio_encoder_lavc, },
    { "pcm_u8",     "pcm_u8",            &audio_encoder_lavc, },
    { "vorbis",     "libvorbis",         &audio_encoder_lavc, },
    { "vorbis",     "vorbis",            &audio_encoder_lavc, },
    { "amrnb",      "libopencore_amrnb", &audio_encoder_lavc, },
#endif
#if HAVE_AMRWB_3GPP
    { "amrwb",      "amrnb_3gpp",        &audio_encoder_amrwb_3gpp, },
#endif
#endif /* HAVE_AUDIO */
    { NULL, },
};

struct aenc_t
{
    const audio_encoder_t *enc;
    hnd_t handle;
    hnd_t filters;
};

hnd_t x264_audio_encoder_open( const audio_encoder_t *encoder, hnd_t filter_chain, const char *opts )
{
    assert( encoder && filter_chain );
    struct aenc_t *enc = calloc( 1, sizeof( struct aenc_t ) );
    enc->enc           = encoder;
    enc->handle        = encoder->init( filter_chain, opts );
    enc->filters       = filter_chain;

    return !enc->handle ? NULL : enc;
}

audio_info_t *x264_audio_encoder_info( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_info( enc->handle );
}

audio_packet_t *x264_audio_encode_frame( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_next_packet( enc->handle );
}

void x264_audio_encoder_skip_samples( hnd_t encoder, uint64_t samplecount )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->skip_samples( enc->handle, samplecount );
}

audio_packet_t *x264_audio_encoder_finish( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->finish( enc->handle );
}

void x264_audio_free_frame( hnd_t encoder, audio_packet_t *frame )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->free_packet( enc->handle, frame );
}

void x264_audio_encoder_close( hnd_t encoder )
{
    if( !encoder )
        return;
    struct aenc_t *enc = encoder;

    enc->enc->close( enc->handle );
    x264_af_close( enc->filters );
    free( enc );
}

const audio_encoder_t *x264_audio_encoder_by_name( const char *name, int mode, const char **used_enc )
{
    const audio_encoder_entry_t *cur = NULL;
    const audio_encoder_t *ret = NULL;

    for( int i=0; registered_audio_encoders[i].codec; i++ )
    {
        char ffprefixed_name[32] = { 0 };
        int is_lavc = 0;

        cur = &registered_audio_encoders[i];
#if HAVE_AUDIO && HAVE_LAVF
        is_lavc = !!( cur->encoder == &audio_encoder_lavc );
#endif
        if( !strcmp( name, mode == QUERY_CODEC ? cur->codec : cur->name ) )
            ret = cur->encoder;
        else if( ( mode == QUERY_ENCODER ) && is_lavc )
        {
            snprintf( ffprefixed_name, 31, "ff%s", cur->name );
            if( !strcmp( name, ffprefixed_name ) )
                ret = cur->encoder;
        }

        if( ret )
        {
            if( !ret->is_valid_encoder )
                break;
            else if( !ret->is_valid_encoder( cur->name, NULL ) )
                break;
            else
                ret = NULL;
        }
    }

    if( used_enc )
        *used_enc = cur->name;

    return ret;
}

const audio_encoder_t *x264_select_audio_encoder( const char *encoder, char* allowed_list[], const char **used_enc )
{
    if( !encoder )
        return NULL;
    if( allowed_list )
    {
        if( !strcmp( encoder, "auto" ) )
        {
            const audio_encoder_t *enc;
            for( int i = 0; allowed_list[i] != NULL; i++ )
            {
                enc = x264_audio_encoder_by_name( allowed_list[i], QUERY_CODEC, used_enc );
                if( enc )
                    return enc;
            }
            return NULL;
        }
        else
        {
            const audio_encoder_t *enc;
            for( int i = 0; allowed_list[i] != NULL; i++ )
            {
                if( !strcmp( encoder, allowed_list[i] ) )
                {
                    enc = x264_audio_encoder_by_name( allowed_list[i], QUERY_CODEC, used_enc );
                    if( enc )
                        return enc;
                }
            }
        }
    }
    return x264_audio_encoder_by_name( encoder, QUERY_ENCODER, used_enc );
}

#define INDENT "                              "

void x264_audio_encoder_list_codecs( int longhelp )
{
    if( longhelp < 1 )
        return;

    const char *prev_name = "";
    int len = strlen( INDENT ) + 6;

    printf( INDENT "    - " );
    for( int i=0; registered_audio_encoders[i].codec; i++ )
    {
        const char *codec_name = registered_audio_encoders[i].codec;

        if( x264_audio_encoder_by_name( codec_name, QUERY_CODEC, NULL ) &&
            strcmp( prev_name, codec_name ) )
        {
            printf( "%s", codec_name );
            len += strlen( codec_name );
            prev_name = codec_name;

            if( registered_audio_encoders[i+1].codec )
            {
                if( len >= 80 - strlen( ", " ) )
                {
                    printf( ",\n" INDENT "      " );
                    len = strlen( INDENT ) + 6;
                }
                else
                    printf( ", " );
            }
        }
    }
    printf( "\n" );
    return;
}

void x264_audio_encoder_list_encoders( int longhelp )
{
    if( longhelp < 2 )
        return;

    int len = strlen( INDENT ) + 6;

    printf( INDENT "    - " );
    for( int i=0; registered_audio_encoders[i].codec; i++ )
    {
        const char *encoder_name = registered_audio_encoders[i].name;
        const audio_encoder_t UNUSED *enc = registered_audio_encoders[i].encoder;
        int is_lavc = 0;

#if HAVE_AUDIO && HAVE_LAVF
        is_lavc = !!( enc == &audio_encoder_lavc );
#endif

        if( x264_audio_encoder_by_name( encoder_name, QUERY_ENCODER, NULL ) )
        {
            printf( "%s%s", (is_lavc ? "(ff)" : "" ), encoder_name );
            len += strlen( encoder_name ) + ( is_lavc ? 4 : 0 );

            if( registered_audio_encoders[i+1].codec )
            {
                if( len >= 80 - strlen( ", " ) )
                {
                    printf( ",\n" INDENT "      " );
                    len = strlen( INDENT ) + 6;
                }
                else
                    printf( ", " );
            }
        }
    }
    printf( "\n" );
    return;
}

#undef INDENT

void x264_audio_encoder_show_help( int longhelp )
{
    if( longhelp < 2 )
    {
        printf( "      For the encoder specific helps, see --fullhelp.\n" );
        return;
    }

    printf( "      Encoder specific helps:\n" );
#if !HAVE_AUDIO
    printf( "            There is no available audio encoder in this x264 build.\n" );
    return;
#endif
    for( int i=0; registered_audio_encoders[i].codec; i++ )
    {
        const audio_encoder_entry_t *cur = &registered_audio_encoders[i];
        const audio_encoder_t *enc = NULL;

        enc = x264_audio_encoder_by_name( cur->name, QUERY_ENCODER, NULL );

        if( !enc || !enc->show_help )
            continue;
        enc->show_help( cur->name );
    }

    return;
}

#include "filters/audio/internal.h"

hnd_t x264_audio_copy_open( hnd_t handle )
{
    assert( handle );
#define IFRET( dec )                                                                \
        extern const audio_encoder_t audio_copy_ ## dec;                            \
        if( !strcmp( #dec, h->self->name ) )                                        \
            return x264_audio_encoder_open( &( audio_copy_ ## dec ), handle, NULL );
#if HAVE_AUDIO
    audio_hnd_t UNUSED *h = handle;
#if HAVE_LAVF
    IFRET( lavf );
#endif
#endif // HAVE_AUDIO
#undef IFRET
    return NULL;
}
