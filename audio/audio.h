#ifndef AUDIO_AUDIO_H_
#define AUDIO_AUDIO_H_

#include <stdint.h>
#include "x264cli.h"
#include "filters/audio/audio_filters.h"

enum AudioTrack
{
    TRACK_ANY  = -1,
    TRACK_NONE = -2
};

hnd_t x264_audio_open_from_file( char *preferred_filter_name, char *path, int trackno );

#endif /* AUDIO_AUDIO_H_ */
