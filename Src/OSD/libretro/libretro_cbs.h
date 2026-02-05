#ifndef __LIBRETRO_CBS_H
#define __LIBRETRO_CBS_H

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

retro_video_refresh_t video_cb = NULL;
retro_environment_t environ_cb = NULL;
retro_audio_sample_t audio_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_input_state_t input_state_cb = NULL;

#ifdef __cplusplus
}
#endif

#endif
