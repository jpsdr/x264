/*****************************************************************************
 * avs.c: avisynth input
 *****************************************************************************
 * Copyright (C) 2009-2016 x264 project
 *
 * Authors: Steven Walters <kemuri9@gmail.com>
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

#include "input.h"
#if USE_AVXSYNTH
#include <dlfcn.h>
#if SYS_MACOSX
#define avs_open() dlopen( "libavxsynth.dylib", RTLD_NOW )
#else
#define avs_open() dlopen( "libavxsynth.so", RTLD_NOW )
#endif
#define avs_close dlclose
#define avs_address dlsym
#else
#include <windows.h>
#define avs_open() LoadLibraryW( L"avisynth" )
#define avs_close FreeLibrary
#define avs_address GetProcAddress
#endif
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "avs", __VA_ARGS__ )

#if HAVE_AUDIO
#include "audio/audio.h"
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

#if HAVE_SWSCALE
#include <libavutil/pixfmt.h>
#endif

/* AvxSynth doesn't have yv24, yv16, yv411, or y8, so disable them. */
#if USE_AVXSYNTH
#define avs_is_yv24( vi ) 0
#define avs_is_yv16( vi ) 0
#define avs_is_yv411( vi ) 0
#define avs_is_y8( vi ) 0
#endif

/* maximum size of the sequence of filters to try on non script files */
#define AVS_MAX_SEQUENCE 5

#define LOAD_AVS_FUNC(name, continue_on_fail)\
{\
    h->func.name = (void*)avs_address( h->library, #name );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

typedef struct
{
    AVS_Clip *clip;
    AVS_ScriptEnvironment *env;
    void *library;
    int num_frames;
    struct
    {
        AVSC_DECLARE_FUNC( avs_clip_get_error );
        AVSC_DECLARE_FUNC( avs_create_script_environment );
        AVSC_DECLARE_FUNC( avs_delete_script_environment );
        AVSC_DECLARE_FUNC( avs_get_error );
        AVSC_DECLARE_FUNC( avs_get_frame );
        AVSC_DECLARE_FUNC( avs_get_video_info );
        AVSC_DECLARE_FUNC( avs_function_exists );
        AVSC_DECLARE_FUNC( avs_invoke );
        AVSC_DECLARE_FUNC( avs_release_clip );
        AVSC_DECLARE_FUNC( avs_release_value );
        AVSC_DECLARE_FUNC( avs_release_video_frame );
        AVSC_DECLARE_FUNC( avs_take_clip );
    } func;
#if HAVE_AUDIO
    char *filename;
    int has_audio;
#endif
} avs_hnd_t;

/* load the library and functions we require from it */
static int x264_avs_load_library( avs_hnd_t *h )
{
    h->library = avs_open();
    if( !h->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error, 0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_error, 1 );
    LOAD_AVS_FUNC( avs_get_frame, 0 );
    LOAD_AVS_FUNC( avs_get_video_info, 0 );
    LOAD_AVS_FUNC( avs_function_exists, 0 );
    LOAD_AVS_FUNC( avs_invoke, 0 );
    LOAD_AVS_FUNC( avs_release_clip, 0 );
    LOAD_AVS_FUNC( avs_release_value, 0 );
    LOAD_AVS_FUNC( avs_release_video_frame, 0 );
    LOAD_AVS_FUNC( avs_take_clip, 0 );
    return 0;
fail:
    avs_close( h->library );
    return -1;
}

/* generate a filter sequence to try based on the filename extension */
static void avs_build_filter_sequence( char *filename_ext, const char *filter[AVS_MAX_SEQUENCE+1] )
{
    int i = 0;
#if USE_AVXSYNTH
    const char *all_purpose[] = { "FFVideoSource", 0 };
#else
    const char *all_purpose[] = { "LWLibavVideoSource", "FFmpegSource2", "DSS2", "DirectShowSource", 0 };
    if( !strcasecmp( filename_ext, "vpy" ) )
        filter[i++] = "VSImport";
    if( !strcasecmp( filename_ext, "avi" ) || !strcasecmp( filename_ext, "vpy" ) )
    {
        filter[i++] = "AVISource";
        filter[i++] = "HBVFWSource";
    }
    if( !strcasecmp( filename_ext, "mp4" ) || !strcasecmp( filename_ext, "mov" ) || !strcasecmp( filename_ext, "qt" ) ||
        !strcasecmp( filename_ext, "3gp" ) || !strcasecmp( filename_ext, "3g2" ) )
        filter[i++] = "LSMASHVideoSource";
    if( !strcasecmp( filename_ext, "d2v" ) )
        filter[i++] = "MPEG2Source";
    if( !strcasecmp( filename_ext, "dga" ) )
        filter[i++] = "AVCSource";
    if( !strcasecmp( filename_ext, "dgi" ) )
        filter[i++] = "DGSource";
#endif
    for( int j = 0; all_purpose[j] && i < AVS_MAX_SEQUENCE; j++ )
        filter[i++] = all_purpose[j];
}

static AVS_Value update_clip( avs_hnd_t *h, const AVS_VideoInfo **vi, AVS_Value res, AVS_Value release )
{
    h->func.avs_release_clip( h->clip );
    h->clip = h->func.avs_take_clip( res, h->env );
    h->func.avs_release_value( release );
    *vi = h->func.avs_get_video_info( h->clip );
    return res;
}

static float get_avs_version( avs_hnd_t *h )
{
/* AvxSynth has its version defined starting at 4.0, even though it's based on
   AviSynth 2.5.8. This is troublesome for get_avs_version and working around
   the new colorspaces in 2.6.  So if AvxSynth is detected, explicitly define
   the version as 2.58. */
#if USE_AVXSYNTH
    return 2.58f;
#else
    FAIL_IF_ERROR( !h->func.avs_function_exists( h->env, "VersionNumber" ), "VersionNumber does not exist\n" )
    AVS_Value ver = h->func.avs_invoke( h->env, "VersionNumber", avs_new_value_array( NULL, 0 ), NULL );
    FAIL_IF_ERROR( avs_is_error( ver ), "unable to determine avisynth version: %s\n", avs_as_error( ver ) )
    FAIL_IF_ERROR( !avs_is_float( ver ), "VersionNumber did not return a float value\n" );
    float ret = avs_as_float( ver );
    h->func.avs_release_value( ver );
    return ret;
#endif
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    FILE *fh = x264_fopen( psz_filename, "r" );
    if( !fh )
        return -1;
    int b_regular = x264_is_regular_file( fh );
    fclose( fh );
    FAIL_IF_ERROR( !b_regular, "AVS input is incompatible with non-regular file `%s'\n", psz_filename );

    avs_hnd_t *h = malloc( sizeof(avs_hnd_t) );
    if( !h )
        return -1;
    FAIL_IF_ERROR( x264_avs_load_library( h ), "failed to load avisynth\n" )
    h->env = h->func.avs_create_script_environment( AVS_INTERFACE_25 );
    if( h->func.avs_get_error )
    {
        const char *error = h->func.avs_get_error( h->env );
        FAIL_IF_ERROR( error, "%s\n", error );
    }
    float avs_version = get_avs_version( h );
    if( avs_version <= 0 )
        return -1;
    x264_cli_log( "avs", X264_LOG_DEBUG, "using avisynth version %.2f\n", avs_version );

#ifdef _WIN32
    /* Avisynth doesn't support Unicode filenames. */
    char ansi_filename[MAX_PATH];
    FAIL_IF_ERROR( !x264_ansi_filename( psz_filename, ansi_filename, MAX_PATH, 0 ), "invalid ansi filename\n" );
    AVS_Value arg = avs_new_value_string( ansi_filename );
#else
    AVS_Value arg = avs_new_value_string( psz_filename );
#endif

    AVS_Value res;
    char *filename_ext = get_filename_extension( psz_filename );

    if( !strcasecmp( filename_ext, "avs" ) )
    {
        res = h->func.avs_invoke( h->env, "Import", arg, NULL );
        FAIL_IF_ERROR( avs_is_error( res ), "%s\n", avs_as_string( res ) )
        /* check if the user is using a multi-threaded script and apply distributor if necessary.
           adapted from avisynth's vfw interface */
        AVS_Value mt_test = h->func.avs_invoke( h->env, "GetMTMode", avs_new_value_bool( 0 ), NULL );
        int mt_mode = avs_is_int( mt_test ) ? avs_as_int( mt_test ) : 0;
        h->func.avs_release_value( mt_test );
        if( mt_mode > 0 && mt_mode < 5 )
        {
            AVS_Value temp = h->func.avs_invoke( h->env, "Distributor", res, NULL );
            h->func.avs_release_value( res );
            res = temp;
        }
    }
    else /* non script file */
    {
        /* cycle through known source filters to find one that works */
        const char *filter[AVS_MAX_SEQUENCE+1] = { 0 };
        avs_build_filter_sequence( filename_ext, filter );
        int i;
        for( i = 0; filter[i]; i++ )
        {
            x264_cli_log( "avs", X264_LOG_INFO, "trying %s... ", filter[i] );
            if( !h->func.avs_function_exists( h->env, filter[i] ) )
            {
                x264_cli_printf( X264_LOG_INFO, "not found\n" );
                continue;
            }
            if( !strncasecmp( filter[i], "FFmpegSource", 12 ) )
            {
                x264_cli_printf( X264_LOG_INFO, "indexing... " );
                fflush( stderr );
            }
            res = h->func.avs_invoke( h->env, filter[i], arg, NULL );
            if( !avs_is_error( res ) )
            {
                x264_cli_printf( X264_LOG_INFO, "succeeded\n" );
                break;
            }
            x264_cli_printf( X264_LOG_INFO, "failed\n" );
        }
        FAIL_IF_ERROR( !filter[i], "unable to find source filter to open `%s'\n", psz_filename )
        if( !strcasecmp( filter[i], "HBVFWSource" ) )
            opt->bit_depth = 16;
    }
    FAIL_IF_ERROR( !avs_is_clip( res ), "`%s' didn't return a video clip\n", psz_filename )
    h->clip = h->func.avs_take_clip( res, h->env );
    const AVS_VideoInfo *vi = h->func.avs_get_video_info( h->clip );
    FAIL_IF_ERROR( !avs_has_video( vi ), "`%s' has no video data\n", psz_filename )

#if HAVE_AUDIO
    h->filename = strdup( psz_filename );
    h->has_audio = !!avs_has_audio( vi );
#endif

    /* if the clip is made of fields instead of frames, call weave to make them frames */
    if( avs_is_field_based( vi ) )
    {
        x264_cli_log( "avs", X264_LOG_WARNING, "detected fieldbased (separated) input, weaving to frames\n" );
        AVS_Value tmp = h->func.avs_invoke( h->env, "Weave", res, NULL );
        FAIL_IF_ERROR( avs_is_error( tmp ), "couldn't weave fields into frames\n" )
        res = update_clip( h, &vi, tmp, res );
        info->interlaced = 1;
        info->tff = avs_is_tff( vi );
    }
#if !HAVE_SWSCALE
    /* if swscale is not available, convert the CSP if necessary */
    FAIL_IF_ERROR( avs_version < 2.6f && (opt->output_csp == X264_CSP_I422 || opt->output_csp == X264_CSP_I444),
                   "avisynth >= 2.6 is required for i422/i444 output\n" )
    if( (opt->output_csp == X264_CSP_I420 && !avs_is_yv12( vi )) || (opt->output_csp == X264_CSP_I422 && !avs_is_yv16( vi )) ||
        (opt->output_csp == X264_CSP_I444 && !avs_is_yv24( vi )) || (opt->output_csp == X264_CSP_RGB && !avs_is_rgb( vi )) )
    {

        const char *csp = opt->output_csp == X264_CSP_I420 ? "YV12" :
                          opt->output_csp == X264_CSP_I422 ? "YV16" :
                          opt->output_csp == X264_CSP_I444 ? "YV24" : "RGB";
        x264_cli_log( "avs", X264_LOG_WARNING, "converting input clip to %s\n", csp );
        FAIL_IF_ERROR( opt->output_csp < X264_CSP_I444 && (vi->width&1),
                       "input clip width not divisible by 2 (%dx%d)\n", vi->width, vi->height )
        FAIL_IF_ERROR( opt->output_csp == X264_CSP_I420 && info->interlaced && (vi->height&3),
                       "input clip height not divisible by 4 (%dx%d)\n", vi->width, vi->height )
        FAIL_IF_ERROR( (opt->output_csp == X264_CSP_I420 || info->interlaced) && (vi->height&1),
                       "input clip height not divisible by 2 (%dx%d)\n", vi->width, vi->height )
        char conv_func[14];
        snprintf( conv_func, sizeof(conv_func), "ConvertTo%s", csp );
        char matrix[7] = "";
        int arg_count = 2;
        /* if doing a rgb <-> yuv conversion then range is handled via 'matrix'. though it's only supported in 2.56+ */
        if( avs_version >= 2.56f && ((opt->output_csp == X264_CSP_RGB && avs_is_yuv( vi )) || (opt->output_csp != X264_CSP_RGB && avs_is_rgb( vi ))) )
        {
            // if converting from yuv, then we specify the matrix for the input, otherwise use the output's.
            int use_pc_matrix = avs_is_yuv( vi ) ? opt->input_range == RANGE_PC : opt->output_range == RANGE_PC;
            strcpy( matrix, use_pc_matrix ? "PC." : "Rec" );
            strcat( matrix, ( vi->width > 1024 || vi->height > 576 ) ? "709" : "601" );
            arg_count++;
            // notification that the input range has changed to the desired one
            opt->input_range = opt->output_range;
        }
        const char *arg_name[] = { NULL, "interlaced", "matrix" };
        AVS_Value arg_arr[3];
        arg_arr[0] = res;
        arg_arr[1] = avs_new_value_bool( info->interlaced );
        arg_arr[2] = avs_new_value_string( matrix );
        AVS_Value res2 = h->func.avs_invoke( h->env, conv_func, avs_new_value_array( arg_arr, arg_count ), arg_name );
        FAIL_IF_ERROR( avs_is_error( res2 ), "couldn't convert input clip to %s\n", csp )
        res = update_clip( h, &vi, res2, res );
    }
    /* if swscale is not available, change the range if necessary. This only applies to YUV-based CSPs however */
    if( avs_is_yuv( vi ) && opt->output_range != RANGE_AUTO && ((opt->input_range == RANGE_PC) != opt->output_range) )
    {
        const char *levels = opt->output_range ? "TV->PC" : "PC->TV";
        x264_cli_log( "avs", X264_LOG_WARNING, "performing %s conversion\n", levels );
        AVS_Value arg_arr[2];
        arg_arr[0] = res;
        arg_arr[1] = avs_new_value_string( levels );
        const char *arg_name[] = { NULL, "levels" };
        AVS_Value res2 = h->func.avs_invoke( h->env, "ColorYUV", avs_new_value_array( arg_arr, 2 ), arg_name );
        FAIL_IF_ERROR( avs_is_error( res2 ), "couldn't convert range: %s\n", avs_as_error( res2 ) )
        res = update_clip( h, &vi, res2, res );
        // notification that the input range has changed to the desired one
        opt->input_range = opt->output_range;
    }
#endif
    if( opt->bit_depth > 8 && opt->bit_depth != BIT_DEPTH && opt->bit_depth != 16 )
    {
        AVS_Value arg_arr[] = { res, avs_new_value_int( 0 ), avs_new_value_int( 0 ), avs_new_value_int( 0 ),
                                     avs_new_value_int( 0 ), avs_new_value_int( 0 ),
                                     avs_new_value_int( opt->bit_depth ), avs_new_value_int( 2 ),
                                     avs_new_value_int( BIT_DEPTH ), avs_new_value_int( BIT_DEPTH > 8 ? 2 : 0 ) };
        const char *arg_name[] = { NULL, "Y", "Cb", "Cr",
                                          "grainY", "grainC",
                                          "input_depth", "input_mode",
                                          "output_depth", "output_mode" };
        AVS_Value res2 = h->func.avs_invoke( h->env, "f3kdb", avs_new_value_array( arg_arr, 10 ), arg_name );
        x264_cli_log( "avs", X264_LOG_WARNING, "performing bit depth conversion using f3kdb: %d->%d\n", opt->bit_depth, BIT_DEPTH );
        FAIL_IF_ERROR( avs_is_error( res2 ), "couldn't convert bit depth: %s\n", avs_as_error( res2 ) )
        res = update_clip( h, &vi, res2, res );
        // notification that the input bit depth has changed to the desired one
        opt->bit_depth = BIT_DEPTH;
    }

    h->func.avs_release_value( res );

    info->width   = vi->width;
    info->height  = vi->height;
    info->fps_num = vi->fps_numerator;
    info->fps_den = vi->fps_denominator;
    h->num_frames = info->num_frames = vi->num_frames;
    info->thread_safe = 1;
    if( avs_is_rgb32( vi ) )
        info->csp = X264_CSP_BGRA | X264_CSP_VFLIP;
    else if( avs_is_rgb24( vi ) )
        info->csp = X264_CSP_BGR | X264_CSP_VFLIP;
    else if( avs_is_yv24( vi ) )
        info->csp = X264_CSP_I444;
    else if( avs_is_yv16( vi ) )
        info->csp = X264_CSP_I422;
    else if( avs_is_yv12( vi ) )
        info->csp = X264_CSP_I420;
#if HAVE_SWSCALE
    else if( avs_is_yuy2( vi ) )
        info->csp = AV_PIX_FMT_YUYV422 | X264_CSP_OTHER;
    else if( avs_is_yv411( vi ) )
        info->csp = AV_PIX_FMT_YUV411P | X264_CSP_OTHER;
    else if( avs_is_y8( vi ) )
        info->csp = AV_PIX_FMT_GRAY8 | X264_CSP_OTHER;
#endif
    else
        info->csp = X264_CSP_NONE;
    info->vfr = 0;

    if( opt->bit_depth > 8 )
    {
        FAIL_IF_ERROR( info->width & 3, "avisynth 16bit hack requires that width is at least mod4\n" )
        x264_cli_log( "avs", X264_LOG_INFO, "avisynth 16bit hack enabled\n" );
        info->csp |= X264_CSP_HIGH_DEPTH;
        info->width >>= 1;
        if( opt->bit_depth == BIT_DEPTH )
        {
            /* HACK: totally skips depth filter to prevent dither error */
            info->csp |= X264_CSP_SKIP_DEPTH_FILTER;
        }
    }

    *p_handle = h;
    return 0;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    if( x264_cli_pic_alloc( pic, X264_CSP_NONE, width, height ) )
        return -1;
    pic->img.csp = csp;
    const x264_cli_csp_t *cli_csp = x264_cli_get_csp( csp );
    if( cli_csp )
        pic->img.planes = cli_csp->planes;
#if HAVE_SWSCALE
    else if( csp == (AV_PIX_FMT_YUV411P | X264_CSP_OTHER) )
        pic->img.planes = 3;
    else
        pic->img.planes = 1; //y8 and yuy2 are one plane
#endif
    return 0;
}

static int read_frame( cli_pic_t *pic, hnd_t handle, int i_frame )
{
    static const int plane[3] = { AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    avs_hnd_t *h = handle;
    if( i_frame >= h->num_frames )
        return -1;
    AVS_VideoFrame *frm = pic->opaque = h->func.avs_get_frame( h->clip, i_frame );
    const char *err = h->func.avs_clip_get_error( h->clip );
    FAIL_IF_ERROR( err, "%s occurred while reading frame %d\n", err, i_frame )
    for( int i = 0; i < pic->img.planes; i++ )
    {
        /* explicitly cast away the const attribute to avoid a warning */
        pic->img.plane[i] = (uint8_t*)avs_get_read_ptr_p( frm, plane[i] );
        pic->img.stride[i] = avs_get_pitch_p( frm, plane[i] );
    }
    return 0;
}

static int release_frame( cli_pic_t *pic, hnd_t handle )
{
    avs_hnd_t *h = handle;
    h->func.avs_release_video_frame( pic->opaque );
    return 0;
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    memset( pic, 0, sizeof(cli_pic_t) );
}

static int close_file( hnd_t handle )
{
    avs_hnd_t *h = handle;
    h->func.avs_release_clip( h->clip );
    if( h->func.avs_delete_script_environment )
        h->func.avs_delete_script_environment( h->env );
    avs_close( h->library );
#if HAVE_AUDIO
    free( h->filename );
#endif
    free( h );
    return 0;
}

#if HAVE_AUDIO
static hnd_t open_audio( hnd_t handle, int track )
{
    avs_hnd_t *h = handle;
    if( !h->has_audio )
        return NULL;
    return x264_audio_open_from_file( "avs", h->filename, track );
}

const cli_input_t avs_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file, open_audio };
#else
const cli_input_t avs_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
#endif
