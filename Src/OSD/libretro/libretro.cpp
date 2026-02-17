#include <compat/msvc.h>
#include <math.h>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <string>
#include <Inputs/Inputs.h>
#include "libretro_cbs.h"
#include "Game.h"
#include "LibretroBlockFileMemory.h"
#include "LibretroWrapper.h"
#include "Pkgs/GL/glew.h"
#include "../../Graphics/SuperAA.h"
#include "libretro_core_options.h"

// --- Global Variables ---
retro_video_refresh_t video_cb = NULL;
retro_environment_t environ_cb = NULL;
retro_audio_sample_t audio_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_input_state_t input_state_cb = NULL;
retro_log_printf_t log_cb;

struct retro_hw_render_callback hw_render;
static LibretroWrapper wrapper = LibretroWrapper();

// Path buffers
char retro_save_directory[4096];
char retro_base_directory[4096];

// Optimization: Cache last known resolution to avoid redundant updates
static unsigned last_width = 0;
static unsigned last_height = 0;
#define NVRAM_BUFFER_SIZE (0x20000 + 2048) 
static uint8_t g_nvram_buffer[NVRAM_BUFFER_SIZE];
// Optimization: Cache save state size
static size_t g_cached_serialize_size = 0;

// --- Logging Helper ---
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

// --- Core Lifecycle ---

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;

   const char *dir = NULL;

   // 1. Setup Config/System Directory
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s/supermodel/Config", dir);
   }
   else
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "Config");
   }

   // 2. Setup Save Directory
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s/supermodel/Saves", dir);
   }
   else
   {
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }

   log_cb(RETRO_LOG_INFO, "[Supermodel] Config Path: %s\n", retro_base_directory);
   log_cb(RETRO_LOG_INFO, "[Supermodel] Save Path: %s\n", retro_save_directory);
}

void retro_deinit(void)
{
    // Clean up if necessary
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Supermodel";
   info->library_version  = "v0.3a-libretro";
   info->need_fullpath    = true;
   // Explicitly removed 7z to avoid user confusion until solid archives are supported
   info->valid_extensions = "zip|chd"; 
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width   = 496;
   info->geometry.base_height  = 384;
   info->geometry.max_width    = 496 * 4;
   info->geometry.max_height   = 384 * 4;
   info->geometry.aspect_ratio = g_options.widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);

   info->timing.fps         = 57.53;
   info->timing.sample_rate = 44100.0;
}

// --- OpenGL Context Management ---

void context_reset(void)
{
    auto emu = wrapper.getEmulator();
    if (!emu) return;

    emu->PauseThreads();
    wrapper.InitGL();
    
    // CRITICAL FIX: Force 1-byte alignment for textures.
    // This prevents the "split-screen" / ghosting on legacy drivers 
    // when handling NPOT resolutions like 496x384.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    emu->ResumeThreads();
}

void context_destroy(void)
{
   // GL Cleanup handled by wrapper destructor usually
}

// --- Game Loading ---
bool retro_load_game(const struct retro_game_info *info)
{
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL; 
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = true;
   hw_render.stencil = true;
   hw_render.bottom_left_origin = true;             // GL standard

   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
       log_cb(RETRO_LOG_ERROR, "[Supermodel] HW Render Context negotiation failed.\n");
       return false;
   }
   
   update_core_options();
   wrapper.InitializePaths(retro_base_directory);
   wrapper.setHwRender(hw_render); 

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   log_cb(RETRO_LOG_INFO, "[Supermodel] Loading ROM: %s\n", info->path);
      
   int emulation = wrapper.Emulate(info->path);
   if (emulation != 0) return false;
   wrapper.SetWidescreen(g_options.widescreen);
   wrapper.SetServiceOnSticks(g_options.service_on_sticks);
   wrapper.SuperModelInit(wrapper.getGame());

   return true;
}
void retro_unload_game(void)
{
   // Save NVRAM to buffer before shutdown (RetroArch will write it to .srm)
   if (wrapper.getEmulator() != nullptr)
   {
       log_cb(RETRO_LOG_INFO, "[Supermodel] Saving NVRAM to .srm file\n");
       
       CBlockFileMemory memFile(g_nvram_buffer, NVRAM_BUFFER_SIZE);
       wrapper.getEmulator()->SaveNVRAM(&memFile);
       memFile.Finish();
   }
   
   wrapper.ShutDownSupermodel();
}
void retro_run(void)
{
   // Check if options were changed
   bool options_updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated)
   {
      int  old_multiplier = g_options.resolution_multiplier;
      bool old_widescreen = g_options.widescreen;
      bool old_service_on_sticks = g_options.service_on_sticks;

      update_core_options();

      if (g_options.widescreen != old_widescreen)
      {
         wrapper.SetWidescreen(g_options.widescreen);

         // Tell RetroArch the aspect ratio changed
         struct retro_game_geometry geometry;
         geometry.base_width   = last_width  ? last_width  : 496;
         geometry.base_height  = last_height ? last_height : 384;
         geometry.max_width    = 496 * 4;
         geometry.max_height   = 384 * 4;
         geometry.aspect_ratio = g_options.widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);
         environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
      }

      if (g_options.resolution_multiplier != old_multiplier)
      {
         last_width  = 0;
         last_height = 0;
      }

      if (g_options.service_on_sticks != old_service_on_sticks)
         wrapper.SetServiceOnSticks(g_options.service_on_sticks);
      }

   // NVRAM Loading: Do this on first frame, AFTER RetroArch has loaded .srm
   static bool first_run = true;
   if (first_run)
   {
      first_run = false;
        
      log_cb(RETRO_LOG_INFO, "[Supermodel] First frame - checking NVRAM buffer...\n");
        
      // Check if buffer has valid block file data (first 16 bytes shouldn't all be zero)
      bool has_nvram = false;
      for (int i = 0; i < 16 && !has_nvram; i++)
         has_nvram = (g_nvram_buffer[i] != 0);
        
      if (has_nvram)
      {
         log_cb(RETRO_LOG_INFO, "[Supermodel] Loading NVRAM from .srm file\n");
            
         CBlockFileMemory memFile(g_nvram_buffer, NVRAM_BUFFER_SIZE);
         wrapper.getEmulator()->LoadNVRAM(&memFile);
      }
      else
      {
         log_cb(RETRO_LOG_INFO, "[Supermodel] No NVRAM data found, using defaults\n");
      }
   }

    if (input_poll_cb) input_poll_cb();

   // Apply resolution multiplier from core options - always use NATIVE resolution as base
   const unsigned NATIVE_WIDTH = 496;
   const unsigned NATIVE_HEIGHT = 384;

   unsigned target_w = NATIVE_WIDTH * g_options.resolution_multiplier;
   unsigned target_h = NATIVE_HEIGHT * g_options.resolution_multiplier;

   // OPTIMIZATION: Only update screen size if it actually changed.
   if (target_w != last_width || target_h != last_height) {
      wrapper.UpdateScreenSize(target_w, target_h);
      
      struct retro_game_geometry geometry;
      geometry.base_width   = target_w;
      geometry.base_height  = target_h;
      geometry.max_width    = 496 * 4;
      geometry.max_height   = 384 * 4;
      geometry.aspect_ratio = g_options.widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f); // ← fix
      
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
      
      last_width  = target_w;
      last_height = target_h;
   }

   // CRITICAL FOR WINDOWS: Reset GL state BEFORE Supermodel renders
   // Windows drivers are strict and don't tolerate corrupted state
   GLuint sm_fbo = wrapper.getSuperModelFBO();
   glBindFramebuffer(GL_FRAMEBUFFER, sm_fbo);

   // Reset all potentially problematic state
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_STENCIL_TEST);
   glEnable(GL_DEPTH_TEST);       // Supermodel needs depth
   glDepthFunc(GL_LESS);
   glDepthMask(GL_TRUE);
   glDisable(GL_BLEND);
   glDisable(GL_CULL_FACE);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

   // CRITICAL FIX: Set viewport to match render target size
   // RetroArch may have changed it, so we must set it here
   glViewport(0, 0, target_w, target_h);
   glScissor(0, 0, target_w, target_h);

   // Clear the framebuffer (critical for Windows)
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClearDepth(1.0);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

   // Remove the viewport logging - we don't need it anymore
   Game game = wrapper.getGame();
   wrapper.Inputs->Poll(&game, 0, 0, target_w, target_h);

   // Run Supermodel with clean GL state
   wrapper.Supermodel(game);

   // CRITICAL FIX: Supermodel changes viewport during rendering
   // Force it back to correct dimensions for blitting
   glViewport(0, 0, target_w, target_h);

   // CHECK VIEWPORT AFTER FIX
   GLint viewport_after_render[4];
   glGetIntegerv(GL_VIEWPORT, viewport_after_render);
   log_cb(RETRO_LOG_INFO, "[After Viewport Fix] Viewport: %d,%d,%d,%d\n",
         viewport_after_render[0], viewport_after_render[1], viewport_after_render[2], viewport_after_render[3]);

   // CRITICAL FOR WINDOWS: Ensure all rendering is complete
   glFlush();

   // Now prepare for blit to RetroArch's framebuffer
   GLuint ra_fbo = wrapper.getHwRender().get_current_framebuffer();
   
   glBindFramebuffer(GL_READ_FRAMEBUFFER, sm_fbo);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ra_fbo);

   // Disable tests that might interfere with blit
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_STENCIL_TEST);
   glDisable(GL_DEPTH_TEST);

   // Blit from Internal Supermodel FBO to RetroArch's Front Buffer
   glBlitFramebuffer(
       0, 0, target_w, target_h,       // Source (Standard Orientation: 0 to H)
       0, 0, target_w, target_h,       // Destination
       GL_COLOR_BUFFER_BIT,
       GL_LINEAR                       
   );

   // Reset to RetroArch's framebuffer
   glBindFramebuffer(GL_FRAMEBUFFER, ra_fbo); 
   // Signal frame is ready
   video_cb(RETRO_HW_FRAME_BUFFER_VALID, target_w, target_h, 0);
}

// --- Save States ---

size_t retro_serialize_size(void)
{
    if (g_cached_serialize_size > 0)
        return g_cached_serialize_size;
    
    if (wrapper.getEmulator() != nullptr)
    {
        CBlockFileCounter counter;
        wrapper.getEmulator()->SaveState(&counter);
        g_cached_serialize_size = counter.GetSize();
    }
    return g_cached_serialize_size;
}

bool retro_serialize(void* data, size_t size) {
    if (!data || size == 0) return false;
    CBlockFileMemory mem(data, size);
    wrapper.getEmulator()->SaveState(&mem);
    mem.Finish();
    return true;
}

bool retro_unserialize(const void* data, size_t size)
{
    if (!data || size == 0) return false;
    CBlockFileMemory mem(const_cast<void*>(data), size);
    wrapper.getEmulator()->LoadState(&mem);
    return true;
}

// --- Input Descriptors & Callbacks ---
void set_input_descriptors(bool service_on_sticks)
{
   struct retro_input_descriptor desc[] = {
      // Player 1 - D-Pad
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },

      // Player 1 - Face buttons
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Punch / Accelerate" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Kick / Brake" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Guard / View Change" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Escape / Shift Up" },

      // Player 1 - Start / Coin
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" },

      // Player 1 - Shoulder buttons (role depends on mapping option)
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,
            service_on_sticks ? "Gear Shift 1" : "Service (Test Menu)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,
            service_on_sticks ? "Gear Shift 2" : "Test Button" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,
            service_on_sticks ? "Gear Shift 3" : "Service 2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,
            service_on_sticks ? "Gear Shift 4" : "Test Button 2" },

      // Player 1 - Stick clicks (role depends on mapping option)
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,
            service_on_sticks ? "Service (Test Menu)" : "L3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,
            service_on_sticks ? "Test Button" : "R3" },

      // Player 1 - Analog
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Steering / Move X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Accelerator / Move Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Brake" },

      // Player 2 - D-Pad
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "P2 D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "P2 D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "P2 D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "P2 D-Pad Down" },

      // Player 2 - Face buttons
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "P2 Punch / Accelerate" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "P2 Kick / Brake" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "P2 Guard / View Change" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "P2 Escape / Shift Up" },

      // Player 2 - Start / Coin
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "P2 Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "P2 Coin" },

      { 0 },
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   // 1. VFS Interface
   struct retro_vfs_interface_info vfs_iface_info;
   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   // 2. ✅ REGISTER CORE OPTIONS (This was missing!)
   // Ensure option_cats and option_defs are defined ABOVE this function
   struct retro_core_options_v2 options_v2;
   options_v2.categories = option_cats;
   options_v2.definitions = option_defs;
   environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);

   // 3. Variable Update Check
   bool dummy = false;
   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &dummy);
   
   // 4. Input Descriptors
   set_input_descriptors(cb); 
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

// --- Stubs (Unused) ---

void retro_reset(void) {}
bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) { return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char *) {}
void* retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM)
    {
        // Serialize NVRAM to buffer on every access
        if (wrapper.getEmulator() != nullptr)
        {
            CBlockFileMemory memFile(g_nvram_buffer, NVRAM_BUFFER_SIZE);
            wrapper.getEmulator()->SaveNVRAM(&memFile);
            memFile.Finish();
        }
        return g_nvram_buffer;
    }
    return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
    return (id == RETRO_MEMORY_SAVE_RAM) ? NVRAM_BUFFER_SIZE : 0;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }