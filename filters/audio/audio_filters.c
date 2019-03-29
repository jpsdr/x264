#include "filters/audio/internal.h"

#include <assert.h>

audio_info_t *x264_af_get_info( hnd_t handle )
{
    return &((audio_hnd_t*)handle)->info;
}

audio_filter_t *x264_af_get_filter( char *name )
{
#if HAVE_AUDIO
#define CHECK( filter )                                 \
    extern audio_filter_t audio_filter_##filter;        \
    if ( !strcmp( name, audio_filter_##filter.name ) )  \
        return &audio_filter_##filter
#if HAVE_LAVF
    CHECK( lavf );
#endif
#if HAVE_AVS
    CHECK( avs );
#endif
#undef CHECKFLT
#undef CHECK
#endif /* HAVE_AUDIO */
    return NULL;
}

audio_packet_t *x264_af_get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    audio_hnd_t *h = handle;
    audio_packet_t *out = h->self->get_samples( h, first_sample, last_sample );
    if( out )
    {
        out->owner = h;
        return out;
    }
    return 0;
}

void x264_af_free_packet( audio_packet_t *pkt )
{
    if( !pkt )
        return;
    audio_hnd_t *owner = pkt->owner;
    if( owner )
        owner->self->free_packet( owner, pkt );
    else
    {
        if( pkt->priv )
            free( pkt->priv );
        if( pkt->data )
            free( pkt->data );
        if( pkt->samples && pkt->channels )
            x264_af_free_buffer( pkt->samples, pkt->channels );
        free( pkt );
    }
}

void x264_af_close( hnd_t chain )
{
    audio_hnd_t *h = chain;
    if( h->prev )
        x264_af_close( h->prev );
    h->self->close( h );
}
