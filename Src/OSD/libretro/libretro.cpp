#include <compat/msvc.h>
#include <math.h>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <rhash.h>
#include "libretro_cbs.h"
#include "Game.h"
#include <Inputs/Inputs.h>
#include "LibretroBlockFileMemory.h"
#if defined(HAVE_ASHMEM) || defined(HAVE_SHM)
#include <errno.h>
#endif
#include <vector>
#define ISHEXDEC ((codeLine[cursor]>='0') && (codeLine[cursor]<='9')) || ((codeLine[cursor]>='a') && (codeLine[cursor]<='f')) || ((codeLine[cursor]>='A') && (codeLine[cursor]<='F'))
#ifdef HAVE_LIGHTREC
#include <sys/mman.h>
#ifdef HAVE_ASHMEM
#include <sys/ioctl.h>
#include <linux/ashmem.h>
#include <dlfcn.h>
#endif
#if defined(HAVE_SHM) || defined(HAVE_ASHMEM)
#include <sys/stat.h>
#include <fcntl.h>
#endif
#ifdef HAVE_WIN_SHM
#include <windows.h>
#endif
#endif                                                                                    /* HAVE_LIGHTREC */
#if __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <mach/shared_region.h>
#include <sys/attr.h>
#define __MACOS__ 1
#define MACOS_VM_BASE (SHARED_REGION_BASE+SHARED_REGION_SIZE+ATTR_VOL_RESERVED_SIZE)
#endif
#include "ugui_tools.h"
#endif
#include <stdarg.h>
#include <ctype.h>
#include <string>
#include "libretro_options.h"
#include <cassert>
#include "LibretroWrapper.h"
#include "Pkgs/GL/glew.h"
#include "../../Graphics/SuperAA.h"

extern "C" {
    extern bool FastSaveStates;                                                           // Fast Save States exclude string labels from variables 
}                                                                                         // in the savestate, and are at least 20% faster.

retro_video_refresh_t video_cb = NULL;
retro_environment_t environ_cb = NULL;
retro_audio_sample_t audio_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_input_state_t input_state_cb = NULL;
retro_input_state_t dbg_input_state_cb = 0;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
retro_log_printf_t log_cb;
struct retro_hw_render_callback hw_render;
struct retro_perf_callback perf_cb;

const int DEFAULT_STATE_SIZE = 16 * 1024 * 1024;
static bool libretro_supports_option_categories = false;
static bool libretro_supports_bitmasks = false;
static unsigned libretro_msg_interface_version = 0;
static unsigned frame_count = 0;
static unsigned internal_frame_count = 0;
static bool display_internal_framerate = false;
static bool display_notifications = true;
static bool allow_frame_duping = false;
static unsigned image_offset = 0;
static unsigned image_crop = 0;
static bool enable_memcard1 = false;
static bool enable_variable_serialization_size = false;
static int frame_width = 0;
static int frame_height = 0;
static bool gui_inited = false;
static bool gui_show = false;
static char bios_path[4096];
static bool firmware_found = false;
static bool overscan;
static double last_sound_rate;
bool setting_apply_analog_toggle  = false;
bool setting_apply_analog_default = false;
bool use_mednafen_memcard0_method = false;
unsigned cd_2x_speedup = 1;
bool cd_async = false;
bool cd_warned_slow = false;
bool fast_pal = false;                                                                             // If true, PAL games will run at 60fps
unsigned image_height = 0;
int64_t cd_slow_timeout = 8000;                                                                    // microseconds
int32_t EventCycles = 128;
uint8_t spu_samples = 1;
int32_t psx_overclock_factor = 0;                                                                  // CPU overclock factor (or 0 if disabled)
unsigned psx_gpu_overclock_shift = 0;                                                              // GPU rasterizer overclock shift
static int override_bios;
char retro_save_directory[4096];
char retro_base_directory[4096];
char retro_cd_base_directory[4096];
static char retro_cd_path[4096];
char retro_cd_base_name[4096];

enum
{
   REGION_JP = 0,
   REGION_NA = 1,
   REGION_EU = 2,
};

enum
{
   PSX_EVENT__SYNFIRST = 0,
   PSX_EVENT_GPU,
   PSX_EVENT_CDC,
   //PSX_EVENT_SPU,
   PSX_EVENT_TIMER,
   PSX_EVENT_DMA,
   PSX_EVENT_FIO,
   PSX_EVENT__SYNLAST,
   PSX_EVENT__COUNT
};
bool content_is_pal = false;
uint8_t widescreen_hack;
uint8_t widescreen_hack_aspect_ratio_setting;
int line_render_mode;
int filter_mode;
bool opaque_check;
bool semitrans_check;
int crop_overscan = 0;
enum core_timing_fps_modes
{
   FORCE_PROGRESSIVE_TIMING = 0,
   FORCE_INTERLACED_TIMING,
   AUTO_TOGGLE_TIMING
};
enum core_timing_fps_modes core_timing_fps_mode;// = AUTO_TOGGLE_TIMING;
bool currently_interlaced = true;
bool interlace_setting_dirty = false;
uint8_t startup_frame_count = 0;
int aspect_ratio_setting = 0;
bool aspect_ratio_dirty = false;
bool is_monkey_hero = false;
int setting_initial_scanline = 0;
int setting_initial_scanline_pal = 0;
int setting_last_scanline = 239;
int setting_last_scanline_pal = 287;
int setting_crosshair_color_p1 = 0xFF0000;
int setting_crosshair_color_p2 = 0x0080FF;
static std::vector<char*> *cdifs = NULL;
static std::vector<const char *> cdifs_scex_ids;
static bool eject_state;
static bool CD_IsPBP = false;
extern int PBP_DiscCount;
static int PBP_PhysicalDiscCount;
static uint64_t Memcard_PrevDC[8];
static int64_t Memcard_SaveDelay[8];
static uint32_t TextMem_Start;
static std::vector<uint8_t> TextMem;
static unsigned DMACycleSteal = 0;                          // Doesn't need to be saved in save states, since it's calculated in the ForceEventUpdates() call chain.
static int32_t Running;                                     // Set to -1 when not desiring exit, and 0 when we are.
#define PSX_EVENT_MAXTS             0x20000000
#define INTERNAL_FPS_SAMPLE_PERIOD 64                       // Sets how often (in number of output frames/retro_run invocations).
#define NEGCON_RANGE 0x7FFF                                 // the internal framerace counter should be updated if.
#ifdef _WIN32                                               // display_internal_framerate is true.
   static char retro_slash = '\\';
#else
   static char retro_slash = '/';
#endif

static bool firmware_is_present(unsigned region)
{
   static const size_t list_size = 10;
   const char *bios_name_list[list_size];
   const char *bios_sha1 = NULL;

   log_cb(RETRO_LOG_INFO, "Checking if required firmware is present...\n");
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - strlen(buf) - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

static bool TestMagic(const char *name, RFILE *fp, int64_t size)
{
   uint8_t header[8];

   if (size < 0x800)
      return(false);

   filestream_read(fp, header, 8);

   if (
         (header[0] == 'P') &&
         (header[1] == 'S') &&
         (header[2] == '-') &&
         (header[3] == 'X') &&
         (header[4] == ' ') &&
         (header[5] == 'E') &&
         (header[6] == 'X') &&
         (header[7] == 'E')
      )
      return(true);

   return(true);
}

/* LED interface */
static retro_set_led_state_t led_state_cb = NULL;
static unsigned int retro_led_state[2] = {0};
static void retro_led_interface(void)
{
   /* 0: Power
    * 1: CD */

   unsigned int led_state[2] = {0};
   unsigned int l            = 0;

   led_state[0] = (!Running) ? 1 : 0;
   //led_state[1] = (PSX_CDC->DriveStatus > 0) ? 1 : 0;

   for (l = 0; l < sizeof(led_state)/sizeof(led_state[0]); l++)
   {
      if (retro_led_state[l] != led_state[l])
      {
         retro_led_state[l] = led_state[l];
         led_state_cb(l, led_state[l]);
      }
   }
}


static bool DecodeGS(const std::string& cheat_string, char* patch)
{
   uint64_t code = 0;
   unsigned nybble_count = 0;

   for(unsigned i = 0; i < cheat_string.size(); i++)
   {
      if(cheat_string[i] == ' ' || cheat_string[i] == '-' || cheat_string[i] == ':' || cheat_string[i] == '+')
         continue;

      nybble_count++;
      code <<= 4;

      if(cheat_string[i] >= '0' && cheat_string[i] <= '9')
         code |= cheat_string[i] - '0';
      else if(cheat_string[i] >= 'a' && cheat_string[i] <= 'f')
         code |= cheat_string[i] - 'a' + 0xA;
      else if(cheat_string[i] >= 'A' && cheat_string[i] <= 'F')
         code |= cheat_string[i] - 'A' + 0xA;
      else
      {
         if(cheat_string[i] & 0x80)
            log_cb(RETRO_LOG_ERROR, "Invalid character in GameShark code..\n");
         else
            log_cb(RETRO_LOG_ERROR, "Invalid character in GameShark code: %c.\n", cheat_string[i]);
         return false;
      }
   }

   if(nybble_count != 12)
   {
      log_cb(RETRO_LOG_ERROR, "GameShark code is of an incorrect length.\n");
      return false;
   }

   const uint8_t code_type = code >> 40;
   const uint64_t cl = code & 0xFFFFFFFFFFULL;

}


static void check_system_specs(void)
{
   // Hints that we need a fairly powerful system to run this.
   unsigned level = 15;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

static unsigned disk_get_num_images(void)
{
   if(cdifs)
      return CD_IsPBP ? PBP_PhysicalDiscCount : cdifs->size();
   return 0;
}

static bool disk_set_eject_state(bool ejected)
{
   if (ejected == eject_state)
      return false;

   //DoSimpleCommand(ejected ? MDFN_MSC_EJECT_DISK : MDFN_MSC_INSERT_DISK);
   eject_state = ejected;
   return true;
}


static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;

   const char *dir = NULL;

   // 1. Setup the Config/System Directory
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      // Construct: [system_dir]/supermodel/Config
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s/supermodel/Config", dir);
   }
   else
   {
      log_cb(RETRO_LOG_WARN, "System directory not defined. Using local 'Config' folder.\n");
      snprintf(retro_base_directory, sizeof(retro_base_directory), "Config");
   }

   // 2. Setup the Save Directory (NVRAM/SRAM)
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s/supermodel/Saves", dir);
   }
   else
   {
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }

   log_cb(RETRO_LOG_INFO, "Supermodel Config Path: %s\n", retro_base_directory);
   log_cb(RETRO_LOG_INFO, "Supermodel Save Path: %s\n", retro_save_directory);

   check_system_specs();
}

void retro_reset(void)
{
   
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

static bool boot = true;

// shared memory cards support
static bool shared_memorycards = false;

static bool has_new_geometry = false;
static bool has_new_timing = false;

uint8_t analog_combo[2] = {0};
uint8_t analog_combo_hold = 0;

static bool retro_set_geometry(void)
{
   struct retro_system_av_info new_av_info;

   retro_get_system_av_info(&new_av_info);
   return environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &new_av_info);
}

static LibretroWrapper wrapper = LibretroWrapper();

void retro_run(void)
{
    if (input_poll_cb) input_poll_cb();

    unsigned target_w = wrapper.getXRes();
    unsigned target_h = wrapper.getYRes();

    wrapper.UpdateScreenSize(target_w, target_h);                                   // 1. Sync dimensions

    Game game = wrapper.getGame();
    wrapper.Inputs->Poll(&game, 0, 0, target_w, target_h);
    
    wrapper.Supermodel(game);                                                       // 2. Run the emulator

   
    glDisable(GL_SCISSOR_TEST);                                                     // --- CRITICAL ADDITION HERE ---
    glDisable(GL_STENCIL_TEST);                                                     // After Supermodel finishes rendering, it might leave the Scissor 
    glDisable(GL_DEPTH_TEST);                                                       // or Viewport messed up for the Blit operation.
    glViewport(0, 0, target_w, target_h);

    GLuint ra_fbo = wrapper.getHwRender().get_current_framebuffer();                // 3. Get the Framebuffers
    GLuint sm_fbo = wrapper.getSuperAA()->GetTargetID();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sm_fbo);                                 // 4. BLIT
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ra_fbo);

    glBlitFramebuffer(
        0, target_h, target_w, 0,                                                   // Source (Flipped)
        0, 0, target_w, target_h,                                                   // Destination
        GL_COLOR_BUFFER_BIT,
        GL_LINEAR                                                                   // Use LINEAR for smoother resizing
    );

    glBindFramebuffer(GL_FRAMEBUFFER, ra_fbo); 
    video_cb(RETRO_HW_FRAME_BUFFER_VALID, target_w, target_h, 0);
}

void context_reset(void)                                                            // This is called when the GL context is created or reset
{                                                                                   // You might need to re-initialize Supermodel's shaders here
    auto emu = wrapper.getEmulator();
    if (!emu)
        return;

    // Pause emulation threads (safe for GL teardown)
    emu->PauseThreads();

   
    wrapper.InitGL();                                                               // Recreate ONLY GL resources

    // Resume emulation
    emu->ResumeThreads();
}                                                                              

void context_destroy(void) 
{
   //TODO                                                                           // Cleanup GL resources
}

bool retro_load_game(const struct retro_game_info *info)
{
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL; 
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = true;
   hw_render.stencil = true; 

   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
       return false;
   }
   
   wrapper.InitializePaths(retro_base_directory);
   wrapper.setHwRender(hw_render); 

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   fprintf(stderr, "SUPERMODEL: Content Path: %s\n", info->path);
      
   int emulation = wrapper.Emulate(info->path);
   wrapper.SuperModelInit(wrapper.getGame());

   return true;
}

void retro_unload_game(void)
{
   wrapper.ShutDownSupermodel();
}

static bool retro_set_system_av_info(void)
{
   struct retro_system_av_info new_av_info;

   retro_get_system_av_info(&new_av_info);
   return environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &new_av_info);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Supermodel";
   info->library_version  = "v0.3a-libretro";
   info->need_fullpath    = true;
   info->valid_extensions = "zip|7z|chd";
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width   = 496;
   info->geometry.base_height  = 384;
   info->geometry.max_width    = 496;
   info->geometry.max_height   = 384;
   info->geometry.aspect_ratio = 4.0f / 3.0f;

   // CHANGE THIS: Match your MODEL3_FPS exactly
   info->timing.fps            = 57.53; 
   info->timing.sample_rate    = 44100.0;
}

void retro_deinit(void)
{
   libretro_supports_option_categories = false;
   libretro_supports_bitmasks = false;
}

unsigned retro_get_region(void)
{
   return content_is_pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void set_input_descriptors(retro_environment_t environ_cb) {
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Punch / Accelerate" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Kick / Brake" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin / Service" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Steering / Move X" },
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}
void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   // 1. Setup VFS (Virtual File System)
   // This allows the core to use Libretro's file abstraction
   struct retro_vfs_interface_info vfs_iface_info;
   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   // 2. Setup LED Interface (Optional, but kept from your snippet)
   struct retro_led_interface led_interface;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &led_interface))
      if (led_interface.set_led_state)
         led_state_cb = led_interface.set_led_state;

   // 3. Variable/Option Support
   // We will define our own options later. For now, we tell the frontend 
   // we want to be notified of option changes.
   bool dummy = false;
   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &dummy);
   
   // 4. Input Descriptors
   // This is where you call that "stolen" descriptor logic we discussed.
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

// This tells the core which device is plugged into which port (Joypad, Mouse, etc.)
void retro_set_controller_port_device(unsigned port, unsigned device)
{
    // For now, we only support 2 ports. 
    // Supermodel internally handles its own mapping, so we just log this for now.
    log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u\n", device, port);
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
   dbg_input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;

   //rsx_intf_set_video_refresh(cb);
}
static size_t g_cached_serialize_size = 0;

size_t retro_serialize_size(void)
{
    // If we already know the size, return it IMMEDIATELY.
    // No SaveState() calls, no side effects.
    if (g_cached_serialize_size > 0)
        return g_cached_serialize_size;

    
    if (wrapper.getEmulator() != nullptr)                               // This block should only run ONCE in the lifetime of the core                   
    {
        CBlockFileCounter counter;
        wrapper.getEmulator()->SaveState(&counter);
        g_cached_serialize_size = counter.GetSize();
        
        // Log it so you know it happened only once
        fprintf(stderr, "[Libretro] Static Save Size Initialized: %zu\n", g_cached_serialize_size);
    }
    
    return g_cached_serialize_size;
}

bool retro_serialize(void* data, size_t size) {
    if (!data || size == 0) return false;
    CBlockFileMemory mem(data, size);
    wrapper.getEmulator()->SaveState(&mem);
    mem.Finish();                                                       // Crucial! Sets the final block's length
    return true;
}

bool retro_unserialize(const void* data, size_t size)
{
    if (!data || size == 0) return false;

    
    CBlockFileMemory mem(const_cast<void*>(data), size);                // Construct the memory wrapper around the data RetroArch gave us
    wrapper.getEmulator()->LoadState(&mem);                             // We call LoadState directly. 
    return true;                                                        // Because retro_serialize_size now returns a cached value, 
}                                                                      // no SaveState logic interfered with the internal state before this call.
bool UsingFastSavestates(void)
{
   int flags;
   if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &flags))
      return flags & 4;
   return false;
}

static uint8_t g_saveRam[SAVE_RAM_SIZE];

void* retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM)
        return g_saveRam;
    return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM)
        return SAVE_RAM_SIZE;
    return 0;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char * codeLine)
{
   
}