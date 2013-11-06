#include "filters/audio/internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

#if USE_AVXSYNTH
#include <dlfcn.h>
#if SYS_MACOSX
#define avs_open dlopen( "libavxsynth.dylib", RTLD_NOW )
#else
#define avs_open dlopen( "libavxsynth.so", RTLD_NOW )
#endif
#define avs_close dlclose
#define avs_address dlsym
#else
#include <windows.h>
#define avs_open LoadLibrary( "avisynth" )
#define avs_close FreeLibrary
#define avs_address GetProcAddress
#endif

#define AVSC_NO_DECLSPEC
#undef EXTERN_C
#if USE_AVXSYNTH
#include "extras/avxsynth_c.h"
#else
#include "extras/avisynth_c.h"
#endif
#define AVSC_DECLARE_FUNC(name) name##_func name

/* AVS uses a versioned interface to control backwards compatibility */
/* YV12 support is required, which was added in 2.5 */
#define AVS_INTERFACE_25 2
#define DEFAULT_BUFSIZE 192000 // 1 second of 48khz 32bit audio
                               // same as AVCODEC_MAX_AUDIO_FRAME_SIZE

#define LOAD_AVS_FUNC(name, continue_on_fail)\
{\
    h->func.name = (void*)avs_address( h->library, #name );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

typedef struct avs_source_t
{
    AUDIO_FILTER_COMMON

    AVS_Clip *clip;
    AVS_ScriptEnvironment *env;
    HMODULE library;
    enum SampleFmt sample_fmt;

    int64_t num_samples;
    int eof;
    uint8_t *buffer;
    intptr_t bufsize;

    struct
    {
        AVSC_DECLARE_FUNC( avs_clip_get_error );
        AVSC_DECLARE_FUNC( avs_create_script_environment );
        AVSC_DECLARE_FUNC( avs_delete_script_environment );
        AVSC_DECLARE_FUNC( avs_get_error );
        AVSC_DECLARE_FUNC( avs_get_audio );
        AVSC_DECLARE_FUNC( avs_get_video_info );
        AVSC_DECLARE_FUNC( avs_function_exists );
        AVSC_DECLARE_FUNC( avs_invoke );
        AVSC_DECLARE_FUNC( avs_release_clip );
        AVSC_DECLARE_FUNC( avs_release_value );
        AVSC_DECLARE_FUNC( avs_take_clip );
    } func;
} avs_source_t;

const audio_filter_t audio_filter_avs;

static int x264_audio_avs_load_library( avs_source_t *h )
{
    h->library = avs_open;
    if( !h->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error, 0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_error, 1 );
    LOAD_AVS_FUNC( avs_get_audio, 0 );
    LOAD_AVS_FUNC( avs_get_video_info, 0 );
    LOAD_AVS_FUNC( avs_function_exists, 0 );
    LOAD_AVS_FUNC( avs_invoke, 0 );
    LOAD_AVS_FUNC( avs_release_clip, 0 );
    LOAD_AVS_FUNC( avs_release_value, 0 );
    LOAD_AVS_FUNC( avs_take_clip, 0 );
    return 0;
fail:
    avs_close( h->library );
    return -1;
}

static void update_clip( avs_source_t *h, const AVS_VideoInfo **vi, AVS_Value *res )
{
    h->func.avs_release_clip( h->clip );
    h->clip = h->func.avs_take_clip( *res, h->env );
    *vi = h->func.avs_get_video_info( h->clip );
    return;
}

#if !USE_AVXSYNTH
static AVS_Value check_avisource( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value arg = avs_new_value_string( filename );
    AVS_Value res;

    x264_cli_log( "avs", X264_LOG_INFO, "trying AVISource for audio ..." );
    res = h->func.avs_invoke( h->env, "AVISource", arg, NULL );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg );
    return res;
}

// FFAudioSource(string source, int track)
static AVS_Value check_ffms( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value res;
    x264_cli_log( "avs", X264_LOG_INFO, "trying FFAudioSource for audio ..." );
    if( !h->func.avs_function_exists( h->env, "FFAudioSource" ) )
    {
        x264_cli_printf( X264_LOG_INFO, " not found\n" );
        res = avs_new_value_error( "not found" );
        return res;
    }

    AVS_Value arg_array = avs_new_value_array( (AVS_Value []){ avs_new_value_string( filename ),
                                                 avs_new_value_int( track ) }, 2);
    const char *arg_names[2] = { NULL, "track" };

    res = h->func.avs_invoke( h->env, "FFAudioSource", arg_array, arg_names );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg_array );
    return res;
}

//DirectShowSource (string filename, bool "audio", bool "video" )
static AVS_Value check_directshowsource( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value res;
    x264_cli_log( "avs", X264_LOG_INFO, "trying DirectShowSource for audio ..." );
    if( !h->func.avs_function_exists( h->env, "DirectShowSource" ) )
    {
        x264_cli_printf( X264_LOG_INFO, " not found\n" );
        res = avs_new_value_error( "not found" );
        return res;
    }

    AVS_Value arg_array = avs_new_value_array( (AVS_Value []){ avs_new_value_string( filename ),
                                                 avs_new_value_bool( 0 ),
                                                 avs_new_value_bool( 1 ) }, 3);
    const char *arg_names[3] = { NULL, "video", "audio" };

    res = h->func.avs_invoke( h->env, "DirectShowSource", arg_array, arg_names );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg_array );
    return res;
}

static void avs_audio_build_filter_sequence( const char *ext, int track,
    AVS_Value (*filters[])( hnd_t handle, const char *filename, int track ) )
{
    int i = 0;
    if( track != TRACK_ANY )
    {
        // audio track selection doesn't work except for ffms
        filters[i++] = check_ffms;
    }
    else if( !strcmp( ext, "avi" ) )
    {
        // try AVISource first
        filters[i++] = check_avisource;
        filters[i++] = check_ffms;
        filters[i++] = check_directshowsource;
    }
    else if( !strcmp( ext, "wma" ) || !strcmp( ext, "wmv" ) || !strcmp( ext, "asf" ) )
    {
        filters[i++] = check_directshowsource;
        filters[i++] = check_ffms;
    }
    else
    {
        filters[i++] = check_ffms;
        filters[i++] = check_directshowsource;
    }
    filters[i] = NULL;
    return;
}
#endif

#define GOTO_IF( cond, label, ... ) \
if( cond ) \
{ \
    x264_cli_log( "avs", X264_LOG_ERROR, __VA_ARGS__ ); \
    goto label; \
}

static int init( hnd_t *handle, const char *opt_str )
{
    assert( opt_str );
    assert( !(*handle) ); // This must be the first filter
    char **opts = x264_split_options( opt_str, (const char*[]){ "filename", "track", NULL } );

    if( !opts )
        return -1;

    char *filename = x264_get_option( "filename", opts );
#if !USE_AVXSYNTH
    char *filename_ext = get_filename_extension( filename );
#endif
    int track = x264_otoi( x264_get_option( "track", opts ), TRACK_ANY );

    GOTO_IF( track == TRACK_NONE, fail2, "no valid track requested ('any', 0 or a positive integer)\n" )
#if USE_AVXSYNTH
    GOTO_IF( track > 0, fail2, "only script imports are supported by this filter\n" )
#endif
    GOTO_IF( !filename, fail2, "no filename given\n" )
    GOTO_IF( !x264_is_regular_file_path( filename ), fail2, "reading audio from non-regular files is not supported\n" )

    INIT_FILTER_STRUCT( audio_filter_avs, avs_source_t );

    GOTO_IF( x264_audio_avs_load_library( h ), error, "failed to load avisynth\n" )
    h->env = h->func.avs_create_script_environment( AVS_INTERFACE_25 );
    if( h->func.avs_get_error )
    {
        const char *error = h->func.avs_get_error( h->env );
        GOTO_IF( error, error, "%s\n", error );
    }

#if USE_AVXSYNTH
    AVS_Value arg = avs_new_value_string( filename );
    AVS_Value res = h->func.avs_invoke( h->env, "Import", arg, NULL );
    h->func.avs_release_value( arg );
    GOTO_IF( avs_is_error( res ), error, "%s\n", avs_as_string( res ) )
#else
    AVS_Value res = avs_void;
    if( !strcmp( filename_ext, "avs" ) )
    {
        // normal avs script
        AVS_Value arg = avs_new_value_string( filename );
        res = h->func.avs_invoke( h->env, "Import", arg, NULL );
        h->func.avs_release_value( arg );
        GOTO_IF( avs_is_error( res ), error, "%s\n", avs_as_string( res ) )
    }
    else
    {
        AVS_Value (*filters[4])( hnd_t handle, const char *filename, int track );
        int i;

        avs_audio_build_filter_sequence( filename_ext, track, filters );

        for( i = 0; filters[i]; i++ )
        {
            res = filters[i]( h, filename, track );
            if( !avs_is_error( res ) )
                break;
        }
        GOTO_IF( !filters[i], error, "no working input filter is found for audio input\n" )
    }
#endif

    GOTO_IF( !avs_is_clip( res ), error, "no valid clip is found\n" )
    h->clip = h->func.avs_take_clip( res, h->env );

    const AVS_VideoInfo *vi = h->func.avs_get_video_info( h->clip );
    GOTO_IF( !avs_has_audio( vi ), error, "no valid audio track is found\n" )

    // video is unneeded, so disable it if any
    res = h->func.avs_invoke( h->env, "KillVideo", res, NULL );
    update_clip( h, &vi, &res );

    switch( avs_sample_type( vi ) )
    {
      case AVS_SAMPLE_INT16:
        h->sample_fmt = SMPFMT_S16;
        break;
      case AVS_SAMPLE_INT32:
        h->sample_fmt = SMPFMT_S32;
        break;
      case AVS_SAMPLE_FLOAT:
        h->sample_fmt = SMPFMT_FLT;
        break;
      case AVS_SAMPLE_INT8:
        h->sample_fmt = SMPFMT_U8;
        break;
      case AVS_SAMPLE_INT24:
      default:
        h->sample_fmt = SMPFMT_NONE;
        break;
    }

    if( h->sample_fmt == SMPFMT_NONE )
    {
        x264_cli_log( "avs", X264_LOG_INFO, "detected %dbit sample format, converting to float\n", avs_bytes_per_channel_sample( vi )*8 );
        res = h->func.avs_invoke( h->env, "ConvertAudioToFloat", res, NULL );
        GOTO_IF( avs_is_error( res ), error, "failed to convert audio sample format\n" )
        update_clip( h, &vi, &res );
        h->sample_fmt = SMPFMT_FLT;
    }

    h->func.avs_release_value( res );

    h->info.samplerate     = avs_samples_per_second( vi );
    h->info.channels       = avs_audio_channels( vi );
    h->info.framelen       = 1;
    h->info.chansize       = avs_bytes_per_channel_sample( vi );
    h->info.samplesize     = h->info.chansize * h->info.channels;
    h->info.framesize      = h->info.samplesize;
    h->info.depth          = h->info.chansize;
    h->info.timebase       = (timebase_t){ 1, h->info.samplerate };

    h->num_samples = vi->num_audio_samples;
    h->bufsize = DEFAULT_BUFSIZE;
    h->buffer = malloc( h->bufsize );

    x264_free_string_array( opts );
    return 0;

error:
    AF_LOG_ERR( h, "error opening audio\n" );
fail:
    if( h )
        free( h );
    *handle = NULL;
fail2:
    x264_free_string_array( opts );
    return -1;
}

static void free_packet( hnd_t handle, audio_packet_t *pkt )
{
    pkt->owner = NULL;
    x264_af_free_packet( pkt );
}

static struct audio_packet_t *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    avs_source_t *h = handle;
    assert( first_sample >= 0 && last_sample > first_sample );
    int64_t nsamples = last_sample - first_sample;

    if( h->eof )
        return NULL;

    if( h->num_samples <= last_sample )
    {
        nsamples = h->num_samples - first_sample;
        h->eof = 1;
    }

    audio_packet_t *pkt = calloc( 1, sizeof( audio_packet_t ) );
    pkt->info           = h->info;
    pkt->dts            = first_sample;
    pkt->channels       = h->info.channels;
    pkt->samplecount    = nsamples;
    pkt->size           = pkt->samplecount * h->info.samplesize;

    if( h->func.avs_get_audio( h->clip, h->buffer, first_sample, nsamples ) )
        goto fail;

    pkt->samples = x264_af_deinterleave2( h->buffer, h->sample_fmt, pkt->channels, pkt->samplecount );

    if( h->eof )
        pkt->flags |= AUDIO_FLAG_EOF;

    return pkt;

fail:
    x264_af_free_packet( pkt );
    return NULL;
}

static void avs_close_file( hnd_t handle )
{
    assert( handle );
    avs_source_t *h = handle;
    h->func.avs_release_clip( h->clip );
    if( h->func.avs_delete_script_environment )
        h->func.avs_delete_script_environment( h->env );
    avs_close( h->library );
    free( h->buffer );
    free( h );
}

const audio_filter_t audio_filter_avs =
{
        .name        = "avs",
#if USE_AVXSYNTH
        .description = "Retrieve PCM samples from specified audio track with AvxSynth",
#else
        .description = "Retrieve PCM samples from specified audio track with AviSynth",
#endif
        .help        = "Arguments: filename",
        .init        = init,
        .get_samples = get_samples,
        .free_packet = free_packet,
        .close       = avs_close_file
};
