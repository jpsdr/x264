#include "audio/encoders.h"
#include "filters/audio/internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

#include <lsmash.h>
#include <lsmash_importer.h>

typedef struct lsmash_source_t
{
    AUDIO_FILTER_COMMON

    importer_t* importer;
    lsmash_audio_summary_t* summary;

    int64_t frame_count;
    int64_t last_dts;
} lsmash_source_t;

const audio_filter_t audio_filter_lsmash;

static int lsmash_init( hnd_t *handle, const char *opt_str )
{
    assert( opt_str );
    assert( !(*handle) ); // This must be the first filter
    char **opts = x264_split_options( opt_str, (const char*[]){ "filename", "track", NULL } );

    if( !opts )
        return -1;

    char *filename = x264_get_option( "filename", opts );

    if( !filename )
    {
        x264_cli_log( "lsmash", X264_LOG_ERROR, "no filename given.\n" );
        goto fail2;
    }
    if( !strcmp( filename, "-" ) )
    {
        x264_cli_log( "lsmash", X264_LOG_ERROR, "pipe input is not supported.\n" );
        goto fail2;
    }

    INIT_FILTER_STRUCT( audio_filter_lsmash, lsmash_source_t );

    h->importer = lsmash_importer_open( filename, "auto" );
    if( !h->importer )
        goto error;

    h->summary = (lsmash_audio_summary_t *)lsmash_duplicate_summary( h->importer, 1 );

    if( h->summary->summary_type != LSMASH_SUMMARY_TYPE_AUDIO )
    {
        AF_LOG_ERR( h, "unsupported stream type.\n" );
        goto error;
    }

    memset( &h->info, 0, sizeof(audio_info_t) );

    if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
    {
        /* Investigate what CODEC is used. */
        lsmash_mp4sys_object_type_indication objectTypeIndication = lsmash_mp4sys_get_object_type_indication( (lsmash_summary_t *)h->summary );
        switch( objectTypeIndication )
        {
            case MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3:
                if( h->summary->aot == MP4A_AUDIO_OBJECT_TYPE_ALS )
                    h->info.codec_name = "als";
                else
                    h->info.codec_name = "aac";
                break;
            case MP4SYS_OBJECT_TYPE_Audio_ISO_11172_3:
            case MP4SYS_OBJECT_TYPE_Audio_ISO_13818_3:
                if( h->summary->aot == MP4A_AUDIO_OBJECT_TYPE_Layer_3 )
                    h->info.codec_name = "mp3";
                else if( h->summary->aot == MP4A_AUDIO_OBJECT_TYPE_Layer_2 )
                    h->info.codec_name = "mp2";
                else
                    h->info.codec_name = "mp1";
                break;
            default :
                AF_LOG_ERR( h, "unknown audio stream type.\n" );
                goto error;
                break;
        }
    }
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_SAMR_AUDIO ) )
        h->info.codec_name = "amrnb";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_SAWB_AUDIO ) )
        h->info.codec_name = "amrwb";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_AC_3_AUDIO ) )
        h->info.codec_name = "ac3";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_EC_3_AUDIO ) )
        h->info.codec_name = "eac3";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSC_AUDIO )
          || lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSE_AUDIO )
          || lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSH_AUDIO )
          || lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSL_AUDIO ) )
    {
        h->info.codec_name = "dca";
        audio_dts_info_t *dts_info = malloc( sizeof( audio_dts_info_t ) );
        if( !dts_info )
            goto error;
        dts_info->coding_name = h->summary->sample_type;
        h->info.opaque = dts_info;
    }
    else
    {
        AF_LOG_ERR( h, "unknown audio stream type.\n" );
        goto error;
    }

    h->info.samplerate     = h->summary->frequency;
    h->info.channels       = h->summary->channels;
    h->info.framelen       = h->summary->samples_in_frame;
    h->info.chansize       = h->summary->sample_size >> 3;
    h->info.samplesize     = h->info.chansize * h->info.channels;
    h->info.framesize      = h->info.framelen * h->info.samplesize;
    h->info.depth          = h->summary->sample_size;
    h->info.timebase       = (timebase_t){ 1, h->summary->frequency };
    h->info.last_delta     = h->info.framelen;
    h->info.priming        = 0;     /* No one can detect this. */

    uint32_t num_extensions = lsmash_count_codec_specific_data( (lsmash_summary_t *)h->summary );
    if( num_extensions )
    {
        h->info.extradata_type = EXTRADATA_TYPE_LSMASH;
        h->info.extradata = malloc( num_extensions * sizeof(lsmash_codec_specific_t *) );
        if( !h->info.extradata )
            AF_LOG_ERR( h, "malloc failed!\n" );
        h->info.extradata_size = num_extensions * sizeof(lsmash_codec_specific_t *);
        for( uint32_t i = 0; i < num_extensions; i++ )
        {
            lsmash_codec_specific_t **extradata = (lsmash_codec_specific_t **)h->info.extradata;
            lsmash_codec_specific_t *src = lsmash_get_codec_specific_data( (lsmash_summary_t *)h->summary, i + 1 );
            lsmash_codec_specific_t *dst = lsmash_convert_codec_specific_format( src, LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED );
            extradata[i] = dst;
        }
    }

    h->frame_count = 0;
    h->last_dts    = 0;

    x264_free_string_array( opts );

    x264_cli_log( "lsmash", X264_LOG_INFO, "opened L-SMASH importer for %s audio stream copy.\n", h->info.codec_name );

    return 0;

error:
    AF_LOG_ERR( h, "error opening the L-SMASH importer.\n" );
fail:
    if( h->summary )
    {
        lsmash_cleanup_summary( (lsmash_summary_t *)h->summary );
        h->summary = NULL;
    }
    if( h->importer )
    {
        lsmash_importer_close( h->importer );
        h->importer = NULL;
    }
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

static void lsmash_close( hnd_t handle )
{
    assert( handle );
    lsmash_source_t *h = handle;

    if( h->summary )
        lsmash_cleanup_summary( (lsmash_summary_t *)h->summary );
    if( h->importer )
        lsmash_importer_close( h->importer );
    if( h->info.opaque )
        free( h->info.opaque );
    if( h )
        free( h );
}

static audio_packet_t *get_next_au( hnd_t handle )
{
    lsmash_source_t *h = handle;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
        return NULL;
    out->info        = h->info;
    out->channels    = h->info.channels;
    out->samplecount = h->info.framelen;
    out->dts         = h->last_dts;

    lsmash_sample_t sample = { 0 };
    lsmash_sample_alloc( &sample, h->summary->max_au_length );

    int ret = lsmash_importer_get_access_unit( h->importer, 1, &sample );

    out->size = sample.length;
    out->data = sample.data;

    if( ret || !out->size )
    {
        if( !out->size )
            h->info.last_delta = lsmash_importer_get_last_delta( h->importer, 1 );
        x264_af_free_packet( out );
        return NULL;
    }

    h->last_dts += h->info.framelen;
    h->frame_count++;

    return out;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    lsmash_source_t *h = handle;

    if( samplecount < h->info.framelen )
        return; // Nothing to do due to low accuracy
    audio_packet_t *pkt;
    uint64_t samples_skipped = 0;
    while( samples_skipped <= ( samplecount - h->info.framelen ) && ( pkt = get_next_au( h ) ) )
    {
        samples_skipped += pkt->samplecount;
        free_packet( h, pkt );
    }
}

static audio_info_t *get_info( hnd_t handle )
{
    audio_hnd_t *h = handle;
    return &h->info;
}

static hnd_t copy_init( hnd_t filter_chain, const char *opts )
{
    assert( filter_chain );
    audio_hnd_t *chain = filter_chain;
    if( chain->self == &audio_filter_lsmash )
    {
        return chain;
    }
    fprintf( stderr, "lsmash [error]: attempted to enter copy mode with a non-empty filter chain!\n" ); // as far as CLI users see, lavf isn't a filter
    return NULL;
}

static audio_packet_t *copy_finish( hnd_t handle )
{
    return get_next_au( handle );
}

static void copy_close( hnd_t handle )
{
    // do nothing or a double-free will happen when the filter chain is freed
}

const audio_filter_t audio_filter_lsmash =
{
    .name        = "lsmash",
    .description = "Demuxes raw audio streams using L-SMASH importer",
    .help        = "Arguments: filename",
    .init        = lsmash_init,
    .close       = lsmash_close
};

const audio_encoder_t audio_copy_lsmash =
{
    .init            = copy_init,
    .get_next_packet = get_next_au,
    .get_info        = get_info,
    .skip_samples    = skip_samples,
    .finish          = copy_finish,
    .free_packet     = free_packet,
    .close           = copy_close
};
