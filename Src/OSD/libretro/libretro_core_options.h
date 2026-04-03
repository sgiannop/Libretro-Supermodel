#include "libretro.h"
#include "libretro_cbs.h"
#include <cstdlib>
#include <cstring>

// --- Core Options ---
static struct retro_core_option_v2_category option_cats[] = {
   {
      "video",
      "Video",
      "Configure graphics and rendering options."
   },
   {
      "audio", 
      "Audio",
      "Configure audio settings."
   },
   {
      "input",
      "Input",
      "Configure input and control settings."
   },
   {
      "cpu",
      "CPU",
      "Configure CPU performance and emulation settings."
   },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs[] = {
   // Video
   {
      "supermodel_resolution",
      "Internal Resolution",
      NULL,
      "Render at higher internal resolution for improved image quality. Higher values require more GPU power.",
      NULL,
      "video",
      {
         { "native", "Native (496x384)" },
         { "2x",     "2x (992x768)" },
         { "3x",     "3x (1488x1152)" },
         { "4x",     "4x (1984x1536)" },
         { NULL, NULL },
      },
      "native"
   },
   {
      "supermodel_wide_screen",
      "Widescreen Hack",
      NULL,
      "Enable widescreen rendering for supported games. May cause graphical glitches.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "supermodel_vsync",
      "VSync",
      NULL,
      "Synchronize frame rendering with display refresh rate.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "supermodel_crosshairs",
      "Show Crosshairs",
      NULL,
      "Display crosshairs for light gun games.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   // Input
   {
      "supermodel_service_buttons",
      "Service / Test Button Mapping",
      NULL,
      "Choose which buttons trigger Service and Test (coin door) functions.",
      NULL,
      "input",
      {
         { "shoulders", "L/R + L2/R2 (Shoulders)" },
         { "sticks",    "L3/R3 (Stick Click)" },
         { NULL, NULL },
      },
      "shoulders"
   },
   {
      "supermodel_force_feedback",
      "Force Feedback",
      NULL,
      "Enable force feedback for racing games (requires compatible hardware).",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "supermodel_analog_sensitivity",
      "Analog Sensitivity",
      NULL,
      "Adjust sensitivity of analog controls (steering, etc.).",
      NULL,
      "input",
      {
         { "50",  "50%" },
         { "75",  "75%" },
         { "100", "100%" },
         { "125", "125%" },
         { "150", "150%" },
         { NULL, NULL },
      },
      "100"
   },
   // Audio
   {
      "supermodel_sound_volume",
      "Sound Volume",
      NULL,
      "Adjust overall sound volume.",
      NULL,
      "audio",
      {
         { "25",  "25%" },
         { "50",  "50%" },
         { "75",  "75%" },
         { "100", "100%" },
         { NULL, NULL },
      },
      "100"
   },
   {
      "supermodel_music_volume",
      "Music Volume", 
      NULL,
      "Adjust music volume separately from sound effects.",
      NULL,
      "audio",
      {
         { "25",  "25%" },
         { "50",  "50%" },
         { "75",  "75%" },
         { "100", "100%" },
         { NULL, NULL },
      },
      "100"
   },
   // CPU
   {
      "supermodel_ppc_frequency",
      "PowerPC CPU Frequency",
      NULL,
      "Adjust PowerPC CPU frequency to trade cycle accuracy for performance on low-end hardware. 'Auto' uses defaults based on game stepping.",
      NULL,
      "cpu",
      {
         { "auto", "Auto (Default)" },
         { "50",   "50 MHz" },
         { "66",   "66 MHz" },
         { "100",  "100 MHz" },
         { "133",  "133 MHz" },
         { "166",  "166 MHz" },
         { "200",  "200 MHz" },
         { NULL, NULL },
      },
      "auto"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

// --- Helper: Read Core Option ---
static const char* option_get(const char* key, const char* default_value)
{
   struct retro_variable var = { key, NULL };
   if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return var.value;
   return default_value;
}

// Cache for parsed options
#include "CoreOptionsTypes.h"

// --- Update Core Options ---
void update_core_options(void)
{
   const char* resolution = option_get("supermodel_resolution", "native");
   if (strcmp(resolution, "2x") == 0)
      g_options.resolution_multiplier = 2;
   else if (strcmp(resolution, "3x") == 0)
      g_options.resolution_multiplier = 3;
   else if (strcmp(resolution, "4x") == 0)
      g_options.resolution_multiplier = 4;
   else
      g_options.resolution_multiplier = 1;

   g_options.widescreen = strcmp(option_get("supermodel_wide_screen", "disabled"), "enabled") == 0;
   g_options.vsync = strcmp(option_get("supermodel_vsync", "enabled"), "enabled") == 0;
   g_options.crosshairs = strcmp(option_get("supermodel_crosshairs", "enabled"), "enabled") == 0;

   g_options.sound_volume = atoi(option_get("supermodel_sound_volume", "100"));
   g_options.music_volume = atoi(option_get("supermodel_music_volume", "100"));

   g_options.service_on_sticks = strcmp(option_get("supermodel_service_buttons", "shoulders"), "sticks") == 0;
   g_options.force_feedback = strcmp(option_get("supermodel_force_feedback", "disabled"), "enabled") == 0;
   g_options.analog_sensitivity = atoi(option_get("supermodel_analog_sensitivity", "100"));

   // Parse PowerPC frequency option
   const char* ppc_freq = option_get("supermodel_ppc_frequency", "auto");
   if (strcmp(ppc_freq, "auto") == 0)
      g_options.ppc_frequency = 0;  // 0 = auto (use game.stepping defaults)
   else if (strcmp(ppc_freq, "50") == 0)
      g_options.ppc_frequency = 50;
   else if (strcmp(ppc_freq, "66") == 0)
      g_options.ppc_frequency = 66;
   else if (strcmp(ppc_freq, "100") == 0)
      g_options.ppc_frequency = 100;
   else if (strcmp(ppc_freq, "133") == 0)
      g_options.ppc_frequency = 133;
   else if (strcmp(ppc_freq, "166") == 0)
      g_options.ppc_frequency = 166;
   else if (strcmp(ppc_freq, "200") == 0)
      g_options.ppc_frequency = 200;
   else
      g_options.ppc_frequency = 0;  // Default to auto on unrecognized value

   // if (log_cb)
   // {
   //    log_cb(RETRO_LOG_INFO, "[Supermodel] Options updated: Resolution=%dx, Widescreen=%d\n",
   //           g_options.resolution_multiplier, g_options.widescreen);
   // }
}