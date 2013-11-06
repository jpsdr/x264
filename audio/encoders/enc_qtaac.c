/*
   This file contains the code from the following software:
   <http://tmkk.pv.land.to/qtaacenc/>.

   Copyright (c) 2010-2011 tmkk <tmkk@smoug.net>

   For the complete copyright notice and disclaimer
   by the original author, see COPYING.qtaacenc.
*/

/* escape conflicting float_t and redefining NAN, defined in fp.h of QuickTime SDK. */
#define float_t escape_float_t
#define NAN escape_NAN
#include "audio/encoders.h"
#include "filters/audio/internal.h"
#undef float_t
#undef NAN

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include <QTML.h>
#include <QuickTimeComponents.h>

#include <assert.h>

typedef struct
{
    int is_vbr;
    int he_flag;
    int encoder_mode;
    int encoder_quality;
    int bitrate_index;
    int vbr_quality;
    int samplerate_index;
    int channel_configuration_index;
} CodecConfig;

typedef struct enc_qtaac_t
{
    ComponentInstance ci;

    audio_info_t info;
    hnd_t filter_chain;
    audio_packet_t *in;

    int finishing;
    int64_t last_sample;
    uint8_t *samplebuffer;
    uint8_t *buffer;
    int bufsize;
    int64_t last_dts;

    int desired_brval;
    CodecConfig config;
} enc_qtaac_t;

enum
{
    BitRateControlMode_LC_ABR            = 0,
    BitRateControlMode_LC_TrueVBR        = 1,
    BitRateControlMode_LC_ConstrainedVBR = 2,
    BitRateControlMode_LC_CBR            = 3,
};

enum
{
    BitRateControlMode_HE_CBR            = 0,
    BitRateControlMode_HE_ABR            = 1,
    BitRateControlMode_HE_ConstrainedVBR = 2,
};

enum
{
    EncoderQuality_Medium  = 0,
    EncoderQuality_High    = 1,
    EncoderQuality_Highest = 2,
};

static const char * const encoder_quality_names[] = {
    "medium", "high", "highest",
};

static const char * const encoder_mode_names[][4] = {
    { "ABR", NULL, "CVBR", "CBR", },
    { "CBR", "ABR", "CVBR", },
};

static int available_quality_values[] = {
    0, 9 ,18 ,27 ,36 ,45 ,54 ,63 ,73, 82, 91, 100, 109, 118, 127,
};

#define Q_VALUES_COUNT (sizeof(available_quality_values)/sizeof(int))

static int get_channel_configuration_index_he( int channels )
{
    switch( channels )
    {
        case 1:  return  0;
        case 2:  return  1;
        case 4:  return  2;
        case 6:  return  3;
        case 8:  return  4;
        default: return -1;
    }
}

static int get_channel_configuration_index_lc( int channels )
{
    switch( channels )
    {
        case 1:  return  0;
        case 2:  return  1;
        case 4:  return  2;
        case 5:  return  4;
        case 6:  return  5;
        case 7:  return  7;
        case 8:  return  9;
        default: return -1;
    }
}

static int get_channel_configuration_index( int channels, int he_flag )
{
    return !he_flag ? get_channel_configuration_index_lc( channels )
                    : get_channel_configuration_index_he( channels );
}

static int get_samplerate_index_lc( int samplerate )
{
    switch( samplerate )
    {
        case  8000: return  1;
        case 11025: return  2;
        case 12000: return  3;
        case 16000: return  4;
        case 22050: return  5;
        case 24000: return  6;
        case 32000: return  7;
        case 44100: return  8;
        case 48000: return  9;
        default:    return -1;
    }
}

static int get_samplerate_index_he( int samplerate )
{
    switch( samplerate )
    {
        case 32000: return  1;
        case 44100: return  2;
        case 48000: return  3;
        case 88200: return  4;
        case 96000: return  5;
        default:    return -1;
    }
}

static int get_samplerate_index( int samplerate, int he_flag )
{
    return !he_flag ? get_samplerate_index_lc( samplerate )
                    : get_samplerate_index_he( samplerate );
}

static int get_mode_index( char *mode, int he_flag )
{
    if( !strcmp( mode, "cbr" ) )
        return !he_flag ? BitRateControlMode_LC_CBR : BitRateControlMode_HE_CBR;
    else if( !strcmp( mode, "abr" ) )
        return !he_flag ? BitRateControlMode_LC_ABR : BitRateControlMode_HE_ABR;
    else if( !strcmp( mode, "cvbr" ) )
        return !he_flag ? BitRateControlMode_LC_ConstrainedVBR : BitRateControlMode_HE_ConstrainedVBR;
    else
        return !he_flag ? BitRateControlMode_LC_ABR : BitRateControlMode_HE_ABR;
}

static int find_nearest_index( int value, int *val_list, int num )
{
    int i;

    if( num <= 0 )
        return -1;

    if( value <= val_list[0] )
        return 0;
    else if( value >= val_list[num-1] )
        return num-1;

    for( i=0; i<num-1; i++ )
    {
        int center = ( val_list[i] + val_list[i+1] )>>1;

        if( val_list[i] <= value && value < center )
            return i;
        else if( center <= value && value < val_list[i+1] )
            return i+1;
    }

    return -1;
}

static int get_bitrate_values_range( CFArrayRef config, CFStringRef type, int *val_list, int max_num )
{
    CFDictionaryRef settings = (CFDictionaryRef)CFArrayGetValueAtIndex( config, 0 );
    CFArrayRef      params   = (CFArrayRef)CFDictionaryGetValue( settings, CFSTR("parameters") );
    CFIndex         i;

    for ( i = 0; i < CFArrayGetCount( params ); i++ )
    {
        CFDictionaryRef key = (CFDictionaryRef)CFArrayGetValueAtIndex( params, i );

        if( CFStringCompare( (CFStringRef)CFDictionaryGetValue( key, CFSTR("key") ), CFSTR("Bit Rate"), 0 ) )
        {
            continue;
        }
        else
        {
            CFArrayRef values = NULL;

            values = (CFArrayRef)CFDictionaryGetValue( key, type );

            if ( !values )
                return 0;
            else
            {
                CFIndex j;

                if( CFArrayGetCount(values) > max_num )
                    return -1;

                for( j = 1; j < CFArrayGetCount(values); j++ )
                    val_list[j-1] = CFStringGetIntValue( (CFStringRef)CFArrayGetValueAtIndex( values, j ) );

                return j-1;
            }
        }
    }

    return 0;
}

#define SETVALUE( name, type, value ) {\
    CFMutableDictionaryRef paramkey = CFDictionaryCreateMutableCopy( NULL, 0, param );\
    CFNumberRef num = CFNumberCreate( NULL, type, &value );\
    CFDictionarySetValue( paramkey, CFSTR(name), num );\
    CFRelease( num );\
    CFArrayAppendValue( new_params, paramkey );\
    CFRelease( paramkey );\
}

static CFArrayRef configure_codec_settings_array( CFArrayRef current_array, CodecConfig *config )
{
    CFMutableDictionaryRef current_settings = CFDictionaryCreateMutableCopy( NULL, 0, (CFDictionaryRef)CFArrayGetValueAtIndex( current_array, 0 ) );
    CFArrayRef               current_params = CFDictionaryGetValue( current_settings, CFSTR("parameters") );
    CFMutableArrayRef            new_params = CFArrayCreateMutableCopy( NULL, 0, current_params );
    CFMutableArrayRef             new_array = CFArrayCreateMutable( NULL, 0, &kCFTypeArrayCallBacks );
    CFArrayRef                    result;
    CFIndex i;

    for( i = 0; i<CFArrayGetCount( current_params ); i++ )
    {
        CFDictionaryRef param           = CFArrayGetValueAtIndex( current_params, i );

        if( !CFStringCompare( (CFStringRef)CFDictionaryGetValue( param, CFSTR("key") ), CFSTR("Channel Configuration"), 0 ) )
            SETVALUE( "current value", kCFNumberSInt32Type, config->channel_configuration_index )
        else if( !CFStringCompare( (CFStringRef)CFDictionaryGetValue( param, CFSTR("key") ), CFSTR("Sample Rate"), 0 ) )
            SETVALUE( "current value", kCFNumberSInt32Type, config->samplerate_index )
        else if( !CFStringCompare( (CFStringRef)CFDictionaryGetValue( param, CFSTR("key") ), CFSTR("Target Format"), 0 ) )
            SETVALUE( "current value", kCFNumberSInt32Type, config->encoder_mode )
        else if( !CFStringCompare( (CFStringRef)CFDictionaryGetValue( param, CFSTR("key") ), CFSTR("Quality"), 0 ) )
            SETVALUE( "current value", kCFNumberSInt32Type, config->encoder_quality )
        else if( !CFStringCompare( (CFStringRef)CFDictionaryGetValue( param, CFSTR("key") ), CFSTR("Bit Rate"), 0 ) )
        {
            if( !config->is_vbr )
                SETVALUE( "current value", kCFNumberSInt32Type, config->bitrate_index )
            else
                SETVALUE( "slider value", kCFNumberSInt32Type, config->vbr_quality )
        }
        else
            CFArrayAppendValue( new_params, param );
    }

    CFDictionarySetValue( current_settings, CFSTR("parameters"), new_params );
    CFRelease( new_params );
    CFArrayAppendValue( new_array, current_settings );
    CFRelease( current_settings );
    result = CFArrayCreateCopy( NULL, new_array );
    CFRelease( new_array );
    CFRelease( current_array );
    return result;
}

OSStatus configure_quicktime_component( enc_qtaac_t *h )
{
    assert( h );
    audio_hnd_t *chain = h->filter_chain;
    AudioStreamBasicDescription indesc, outdesc;
    CFArrayRef config_array=NULL;

    memset( &indesc, 0, sizeof(indesc) );
    indesc.mSampleRate = chain->info.samplerate;
    indesc.mFormatID = kAudioFormatLinearPCM;
    indesc.mFormatFlags = kLinearPCMFormatFlagIsFloat | kAudioFormatFlagsNativeEndian;
    indesc.mBytesPerPacket = 4 * chain->info.channels;
    indesc.mFramesPerPacket = 1;
    indesc.mBytesPerFrame = indesc.mBytesPerPacket * indesc.mFramesPerPacket;
    indesc.mChannelsPerFrame = chain->info.channels;
    indesc.mBitsPerChannel = 32;

    if( QTSetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_InputBasicDescription, sizeof(indesc), &indesc ) != noErr )
        goto error;

    memset( &outdesc, 0, sizeof(outdesc) );
    outdesc.mSampleRate = h->info.samplerate;
    outdesc.mFormatID = h->config.he_flag ? kAudioFormatMPEG4AAC_HE : kAudioFormatMPEG4AAC;
    outdesc.mChannelsPerFrame = h->info.channels;

    if( QTSetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_BasicDescription, sizeof(outdesc), &outdesc ) != noErr )
        goto error;

    if( QTGetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_CodecSpecificSettingsArray, sizeof(config_array), &config_array, NULL ) != noErr )
        goto error;

    if( (h->config.samplerate_index = get_samplerate_index( h->info.samplerate, h->config.he_flag )) < 0 ||
        (h->config.channel_configuration_index = get_channel_configuration_index( h->info.channels, h->config.he_flag )) < 0 )
        goto error;

    /* Encoder bitrate control mode should be set before retrieving available bitrate ranges since they depend on mode. */
    /* So if not TrueVBR, configure with dummy bitrate once and re-configure with validated bitrate again. */
    if( !h->config.is_vbr )
    {
        CodecConfig config_tmp = h->config;
        config_tmp.bitrate_index = 0;

        if( ( config_array = configure_codec_settings_array( config_array, &h->config ) ) == NULL )
            goto error;
        if( QTSetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_CodecSpecificSettingsArray, sizeof(config_array), &config_array) != noErr )
            goto error;
        if( QTGetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_CodecSpecificSettingsArray, sizeof(config_array), &config_array, NULL ) != noErr )
        goto error;
    }

    if( !h->config.is_vbr )
    {
        int count;
        int values[64];
        int real_brval = h->desired_brval;

        if( ( count = get_bitrate_values_range( config_array, CFSTR("limited values"), values, 64 ) ) < 0 )
            goto error;

        if( real_brval <= values[0] ) real_brval = values[0];
        if( real_brval >= values[count-1] ) real_brval = values[count-1];

        if( ( count = get_bitrate_values_range( config_array, CFSTR("available values"), values, 64 ) ) < 0 )
            goto error;

        h->config.bitrate_index = find_nearest_index( real_brval, values, count ) + 1;
        real_brval = values[ h->config.bitrate_index-1 ];

        if( real_brval != h->desired_brval )
        {
            x264_cli_log( "qtaac", X264_LOG_WARNING, "audio bitrate is rounded to nearest available value\n" );
            h->desired_brval = real_brval;
        }
    }
    else
    {
        int real_brval = h->desired_brval;

        if( real_brval < 0 ) real_brval = 0;
        if( real_brval > 127 ) real_brval = 127;

        real_brval = available_quality_values[ find_nearest_index( real_brval, available_quality_values, Q_VALUES_COUNT ) ];

        h->config.vbr_quality = real_brval;

        if( real_brval != h->desired_brval )
        {
            x264_cli_log( "qtaac", X264_LOG_WARNING, "audio quality is rounded to nearest available value\n" );
            h->desired_brval = real_brval;
        }
    }

    if( ( config_array = configure_codec_settings_array( config_array, &h->config ) ) == NULL )
        goto error;

    if( QTSetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_CodecSpecificSettingsArray, sizeof(config_array), &config_array) != noErr )
        goto error;

    return noErr;

error:
    x264_cli_log( "qtaac", X264_LOG_ERROR, "failed to configure quicktime component\n" );
    if( config_array ) CFRelease( config_array );
    return -1;
}

static OSStatus check_quicktime_version( void )
{
    long version = 0;

    InitializeQTML( 0 );
    EnterMovies();

    Gestalt( gestaltQuickTimeVersion, &version );
    if( version < 0x07650000 ) {
        return -1;
    }

    return noErr;
}

#ifndef MAX_PATH
#define MAX_PATH (260)
#endif

static OSStatus register_quicktime_component( void )
{
    HKEY hKey;
    char *subKey = "SOFTWARE\\Apple Computer, Inc.\\QuickTime";
    char *entry = "QTSysDir";
    DWORD size;
    char path[MAX_PATH+1];

    if( RegOpenKeyEx( HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &hKey ) == ERROR_SUCCESS )
    {
        if( RegQueryValueEx( hKey, entry, NULL, NULL, NULL, &size ) == ERROR_SUCCESS )
        {
            if( size > MAX_PATH - strlen( "QuickTimeAudioSupport.qtx" ) )
            {
                RegCloseKey( hKey );
                return -1;
            }
            char value[MAX_PATH+1];
            RegQueryValueEx( hKey, entry, NULL, NULL, (LPBYTE)value, &size );
            RegCloseKey( hKey );
            snprintf( path, MAX_PATH, "%sQuickTimeAudioSupport.qtx", value );
        }
        else
        {
            RegCloseKey( hKey );
            return -1;
        }
    }
    else
    {
        RegCloseKey( hKey );
        return -1;
    }

    if( GetFileAttributes( path ) != -1 ) {
        ComponentDescription cd;
        cd.componentType = 'aenc';
        cd.componentSubType = kAudioFormatMPEG4AAC_HE;
        cd.componentManufacturer = kAppleManufacturer;
        cd.componentFlags = 0;
        cd.componentFlagsMask = 0;
        ComponentResult (*ComponentRoutine)( ComponentParameters *, Handle );
        HMODULE h = LoadLibrary( path );

        ComponentRoutine = (ComponentResult(__cdecl *)( ComponentParameters *, Handle ) )GetProcAddress( h, "ACMP4AACHighEfficiencyEncoderEntry" );
        RegisterComponent( &cd, ComponentRoutine, 0, NULL, NULL, NULL );

        FreeLibrary( h );
    }
    else
    {
        return -1;
    }

    return noErr;
}

static void read_AudioSpecificConfig( UInt8 *esds_buf, UInt32 size, UInt8 **asc, UInt32 *asc_size )
{
    *asc_size = 0;
    *asc  = NULL;

    while( size > 0 || !(*asc) )
    {
        UInt8 tag;
        UInt32 tag_size = 0;
        int i;

        tag = *esds_buf;
        esds_buf++;
        size--;

        for( i=0; i<4; i++ )
        {
            tag_size = (tag_size << 7) | (esds_buf[i] & 0x7F);
            if( !(esds_buf[i] >> 7) )
            {
                esds_buf += i+1;
                size -= i+1;
                break;
            }
        }

        switch( tag )
        {
            case 0x03:
                esds_buf += 3;
                size -= 3;
                continue;
            case 0x04:
                esds_buf += 13;
                size -= 13;
                continue;
            case 0x05:
                *asc = esds_buf;
                *asc_size = tag_size;
                esds_buf += tag_size;
                size -= tag_size;
                continue;
            default:
                esds_buf += tag_size;
                size -= tag_size;
                continue;
        }
    }
}

static hnd_t qtaac_init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );

    char **opts     = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, "samplerate", "sbr", "mode", NULL } );
    if( !opts )
    {
        x264_cli_log( "qtaac", X264_LOG_ERROR, "wrong audio options.\n" );
        return NULL;
    }

    audio_hnd_t *chain = filter_chain;
    enc_qtaac_t *h = calloc( 1, sizeof( enc_qtaac_t ) );
    h->filter_chain = chain;
    h->info = chain->info;
    h->info.codec_name = "aac";

    h->config.he_flag = x264_otob( x264_get_option( "sbr", opts ), 0 );

    if( get_channel_configuration_index( chain->info.channels, h->config.he_flag ) < 0 ||
        get_samplerate_index( chain->info.samplerate, h->config.he_flag ) < 0 )
    {
        x264_cli_log( "qtaac", X264_LOG_ERROR, "unsupported input samplerate or channel configuration\n" );
        goto error;
    }

    h->config.is_vbr  = x264_otob( x264_get_option( "is_vbr", opts ), h->config.he_flag ? 0 : 1 );

    if( h->config.is_vbr )
        h->config.encoder_mode = BitRateControlMode_LC_TrueVBR;
    else
    {
        char *mode = x264_otos( x264_get_option( "mode", opts ), "abr" );
        h->config.encoder_mode = get_mode_index( mode, h->config.he_flag );
    }

    if( h->config.is_vbr )
        h->desired_brval = x264_otof( x264_get_option( "bitrate", opts ), 63 );
    else
        h->desired_brval = x264_otof( x264_get_option( "bitrate", opts ), h->config.he_flag ? 64 : 128 );

    h->config.encoder_quality = x264_otof( x264_get_option( "quality", opts ), EncoderQuality_Medium );
    h->info.samplerate = x264_otof( x264_get_option( "samplerate", opts ), chain->info.samplerate );

    x264_free_string_array( opts );

    if( h->info.samplerate > chain->info.samplerate )
    {
        x264_cli_log( "qtaac", X264_LOG_ERROR, "Output samplerate greater than input is not supported, set another value or remove --samplerate switch\n" );
        goto error;
    }
    if( get_samplerate_index( h->info.samplerate, h->config.he_flag ) < 0 )
    {
        x264_cli_log( "qtaac", X264_LOG_ERROR, "Invalid samplerate %dhz, set another value or remove --samplerate switch\n", h->info.samplerate );
        goto error;
    }

    if( check_quicktime_version() != noErr ||
        register_quicktime_component() != noErr ||
        OpenADefaultComponent( StandardCompressionType, StandardCompressionSubTypeAudio, &h->ci ) != noErr )
        goto error;

    if( configure_quicktime_component( h ) != noErr )
        goto error;

    AudioStreamBasicDescription indesc, outdesc;

    if( QTGetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_InputBasicDescription, sizeof(indesc), &indesc, NULL ) != noErr )
        goto error;

    if( QTGetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_BasicDescription, sizeof(outdesc), &outdesc, NULL ) != noErr )
        goto error;

    h->info.samplerate     = outdesc.mSampleRate;
    h->info.timebase       = (timebase_t){ 1, h->info.samplerate };
    h->info.channels       = outdesc.mChannelsPerFrame;
    h->info.framelen       = outdesc.mFramesPerPacket;
    h->info.chansize       = 4;
    h->info.samplesize     = h->info.channels * h->info.chansize;
    h->info.framesize      = h->info.framelen * h->info.samplesize;
    h->info.depth          = 32;
    h->info.last_delta     = h->info.framelen;

    audio_aac_info_t *aacinfo = malloc( sizeof( audio_aac_info_t ) );
    aacinfo->has_sbr          = !!h->config.he_flag;
    h->info.opaque            = aacinfo;

    UInt32 size;
    if( QTGetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_MaximumOutputPacketSize, sizeof(size), &size, NULL ) != noErr )
        goto error;

    h->last_dts = INVALID_DTS;
    h->finishing = 0;
    h->samplebuffer = NULL;
    h->bufsize = size;
    if( ( h->buffer = malloc( h->bufsize )) == NULL )
        goto error;

    UInt8 esds_buf[1024];
    size = 1024;
    if( QTGetComponentProperty( h->ci, kQTPropertyClass_SCAudio, kQTSCAudioPropertyID_MagicCookie, size, esds_buf, &size ) != noErr )
        goto error;

    UInt8 *asc;
    UInt32 asc_size;

    read_AudioSpecificConfig( esds_buf, size, &asc, &asc_size );
    if( asc_size <= 0 || !asc )
        goto error;
    h->info.extradata      = calloc( 1, asc_size );
    h->info.extradata_size = asc_size;
    memcpy( h->info.extradata, asc, asc_size );

    x264_cli_log( "audio", X264_LOG_INFO, "opened qtaac encoder (%s %s: %d%s, quality: %s, samplerate: %dhz)\n",
                  ( !h->config.he_flag ? "AAC-LC" : "AAC-HE" ),
                  ( !h->config.is_vbr ? encoder_mode_names[!!h->config.he_flag][h->config.encoder_mode] : "VBR" ),
                  h->desired_brval, ( !h->config.is_vbr ? "kbps" : "" ),
                  encoder_quality_names[h->config.encoder_quality], h->info.samplerate );

    return h;

error:
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_qtaac_t *h = handle;

    return &h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static int qt_channel_map[][8] = {
 { 0, },
 { 0, 1, },
 { 2, 0, 1, },
 { 0, 1, 2, 3, },
 { 2, 0, 1, 3, 4, },
 { 2, 0, 1, 4, 5, 3, },
 { 2, 0, 1, 6, 4, 5, 3, },
 { 2, 0, 1, 6, 7, 4, 5, 3, },
};

static OSStatus pcmInputDataProc( ComponentInstance ci,
                          UInt32 *ioNumberDataPackets,
                          AudioBufferList *ioData,
                          AudioStreamPacketDescription **outDataPacketDescription,
                          void *inRefCon )
{
    enc_qtaac_t *h = inRefCon;
    UInt32 requested_packets = *ioNumberDataPackets;

    if( h->finishing || ( h->in && ( h->in->flags & AUDIO_FLAG_EOF ) ) )
        goto eof_reached;

    if( h->in )
        x264_af_free_packet( h->in );

    if( !( h->in = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen ) ) )
        goto eof_reached;

    if( h->in->samplecount < requested_packets )
        h->in->flags |= AUDIO_FLAG_EOF;

    if( h->last_dts == INVALID_DTS )
        h->last_dts = h->last_sample;
    h->last_sample += h->in->samplecount;

    if( h->samplebuffer )
        free( h->samplebuffer );
    h->samplebuffer = x264_af_interleave3( SMPFMT_FLT, h->in->samples, h->info.channels, h->in->samplecount, qt_channel_map[h->info.channels-1] );

    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = h->info.channels;
    ioData->mBuffers[0].mDataByteSize = h->info.samplesize * h->in->samplecount;
    ioData->mBuffers[0].mData = h->samplebuffer;

    *ioNumberDataPackets = h->in->samplecount;

    return noErr;

eof_reached:
    h->finishing = 1;
    if( h->in )
        x264_af_free_packet( h->in );
    h->in = NULL;
    ioData->mNumberBuffers = 0;
    ioData->mBuffers[0].mData = NULL;
    ioData->mBuffers[0].mDataByteSize = 0;
    *ioNumberDataPackets = 0;

    return noErr;
}

static audio_packet_t *fill_buffer( enc_qtaac_t *h )
{
    audio_packet_t *out = NULL;

    UInt32 npackets = 1;
    AudioStreamPacketDescription desc = { .mStartOffset = 0, };
    AudioBufferList list = { .mNumberBuffers = 1,
                             .mBuffers = {
                                 { .mNumberChannels = h->info.channels,
                                   .mDataByteSize = h->bufsize,
                                   .mData = h->buffer, },
                                 },
                           };

    OSStatus err = SCAudioFillBuffer( h->ci, pcmInputDataProc, h, &npackets, &list, &desc );

    if( err || desc.mDataByteSize == 0 || npackets == 0 )
        return NULL;

    out = calloc( 1, sizeof(audio_packet_t) );
    out->info = h->info;
    out->size = desc.mDataByteSize;
    out->data = malloc( desc.mDataByteSize );
    memcpy( out->data, h->buffer, out->size );

    return out;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_qtaac_t *h = handle;
    audio_packet_t *out = NULL;

    if( h->finishing )
        return NULL;

    do
    {
        if( !( out = fill_buffer( h ) ) )
            continue;

    } while( !out && !h->finishing );

    out->dts = h->last_dts;
    h->last_dts += h->info.framelen;

    return out;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_qtaac_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t encoder )
{
    enc_qtaac_t *h = encoder;
    audio_packet_t *out = NULL;

    h->finishing = 1;

    if( !( out = fill_buffer( h ) ) )
        return NULL;

    out->dts = h->last_dts;
    h->last_dts += h->info.framelen;

    return out;
}

static void qtaac_close( hnd_t handle )
{
    assert( handle );
    enc_qtaac_t *h = handle;

    if( h->ci )
        CloseComponent( h->ci );
    if( h->buffer )
        free( h->buffer );
    if( h->samplebuffer )
        free( h->samplebuffer );
    if( h->info.extradata )
        free( h->info.extradata );
    if( h->info.opaque )
        free( h->info.opaque );
    if( h->in )
        x264_af_free_packet( h->in );
    free( h );
}

static void qtaac_help( const char * const encoder_name )
{
    printf( "      * qtaac encoder help\n" );
    printf( "        --aquality        VBR quality [63]\n" );
    printf( "                          Cannot be used for HE-AAC and possible values are:\n" );
    printf( "                             0, 9 ,18 ,27 ,36 ,45 ,54 ,63 ,73, 82, 91, 100, 109, 118, 127\n" );
    printf( "                          0 is lowest and 127 is highest.\n" );
    printf( "        --abitrate        Enables bitrate mode\n" );
    printf( "                          Bitrate should be one of the discrete preset values depending on\n" );
    printf( "                          profile, channels count, and samplerate.\n" );
    printf( "                          Examples for typical configurations\n" );
    printf( "                           - for 44100Hz or 48000Hz with 1ch\n" );
    printf( "                             LC: 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256\n" );
    printf( "                             HE: 16, 24, 32, 40\n" );
    printf( "                           - for 44100Hz or 48000Hz with 2ch\n" );
    printf( "                             LC: 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320\n" );
    printf( "                             HE: 32, 40, 48, 56, 64, 80\n" );
    printf( "                           - for 44100Hz or 48000Hz with 5.1ch\n" );
    printf( "                             LC: 160, 192, 224, 256, 288, 320, 384, 448, 512, 576, 640, 768\n");
    printf( "                             HE: 80, 96, 112, 128, 160, 192\n");
    printf( "                          The lower samplerate, the lower min/max values are applied\n" );
    printf( "        --asamplerate     Output samplerate\n" );
    printf( "                             LC: 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000\n" );
    printf( "                             HE: 32000, 44100, 48000\n" );
    printf( "                          Samplerate greater than input is not supported\n" );
    printf( "        --acodec-quality  Encoder's internal complexity [0]\n" );
    printf( "                             0 (medium), 1 (high), 2 (highest)\n" );
    printf( "        --aextraopt       Profile and bitrate mode\n" );
    printf( "                             sbr  : enable HE-AAC encoding [0]\n" );
    printf( "                             mode : bitrate control mode [abr]\n" );
    printf( "                                    \"abr\", \"cbr\", \"cvbr\"\n" );
    printf( "\n" );
    printf( "        --aquality/--abitrate setting may be changed inside codec due to its\n" );
    printf( "        limitations and extreme resampling settings (e.g. 48000->8000) may not work.\n" );
    printf( "        If something goes wrong, it will result in a failure of codec initialization.\n" );
    printf( "\n" );
}

int is_quicktime_available( const char * const encoder_name, void **priv )
{

    if( !strcmp( encoder_name, "qtaac" ) && check_quicktime_version() == noErr )
        return 0;

    return -1;
}

const audio_encoder_t audio_encoder_qtaac =
{
    .init            = qtaac_init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = qtaac_close,
    .show_help       = qtaac_help,
    .is_valid_encoder = is_quicktime_available
};
