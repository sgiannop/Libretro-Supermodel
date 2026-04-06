#ifndef SUPERMODEL_CORE_OPTIONS_TYPES_H
#define SUPERMODEL_CORE_OPTIONS_TYPES_H

struct CoreOptions {
   float resolution_multiplier;
   bool widescreen;
   bool vsync;
   bool crosshairs;
   bool force_feedback;
   int analog_sensitivity;
   int sound_volume;
   int music_volume;
   bool service_on_sticks;
   int ppc_frequency;
   int frameskip;
   bool sound_enable;
};

extern CoreOptions g_options;

#endif
