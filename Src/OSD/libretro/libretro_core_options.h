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
      "network",
      "Network / Link Play",
      "Configure arcade link cable emulation."
   },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs[] = {
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
   {
      "supermodel_net_mode",
      "Link Mode",
      NULL,
      "Set this machine as Master or Slave for linked games (Scud Race, Daytona 2). Set IP in Supermodel.ini.",
      NULL,
      "network",
      {
         { "single", "Single Machine" },
         { "master", "Master (Host)" },
         { "slave",  "Slave (Client)" },
         { NULL, NULL }
      },
      "single"
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
struct CoreOptions {
   int resolution_multiplier;
   bool widescreen;
   bool vsync;
   bool crosshairs;
   bool force_feedback;
   int analog_sensitivity;
   int sound_volume;
   int music_volume;
   const char* net_mode;
};

CoreOptions g_options = { 1, false, true, true, false, 100, 100, 100, "single" };

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
   g_options.force_feedback = strcmp(option_get("supermodel_force_feedback", "disabled"), "enabled") == 0;
   
   g_options.analog_sensitivity = atoi(option_get("supermodel_analog_sensitivity", "100"));
   g_options.sound_volume = atoi(option_get("supermodel_sound_volume", "100"));
   g_options.music_volume = atoi(option_get("supermodel_music_volume", "100"));
   
   g_options.net_mode = option_get("supermodel_net_mode", "single");

   // if (log_cb)
   // {
   //    log_cb(RETRO_LOG_INFO, "[Supermodel] Options updated: Resolution=%dx, Widescreen=%d, Network=%s\n", 
   //           g_options.resolution_multiplier, g_options.widescreen, g_options.net_mode);
   // }
}