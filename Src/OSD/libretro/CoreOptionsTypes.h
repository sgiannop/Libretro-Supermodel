#ifndef SUPERMODEL_CORE_OPTIONS_TYPES_H
#define SUPERMODEL_CORE_OPTIONS_TYPES_H

struct CoreOptions {
   int resolution_multiplier;
   bool widescreen;
   bool vsync;
   bool crosshairs;
   bool force_feedback;
   int analog_sensitivity;
   int sound_volume;
   int music_volume;
   bool service_on_sticks;
};

extern CoreOptions g_options;

#endif
