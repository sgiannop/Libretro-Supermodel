#include <compat/msvc.h>
#include <math.h>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <rhash.h>
#include "libretro_cbs.h"
#include "Game.h"
#include <Inputs/Inputs.h>

#include "libretro_core_options.h"
#include "LibretroBlockFileMemory.h"
retro_input_state_t dbg_input_state_cb = 0;


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
#endif /* HAVE_LIGHTREC */

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

//Fast Save States exclude string labels from variables in the savestate, and are at least 20% faster.
extern "C" {
    extern bool FastSaveStates;
}

const int DEFAULT_STATE_SIZE = 16 * 1024 * 1024;

static bool libretro_supports_option_categories = false;
static bool libretro_supports_bitmasks = false;
static unsigned libretro_msg_interface_version = 0;

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
retro_log_printf_t log_cb;
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

// Switchable memory cards
static int memcard_left_index = 0;
static int memcard_left_index_old;
static int memcard_right_index = 1;
static int memcard_right_index_old;

unsigned cd_2x_speedup = 1;
bool cd_async = false;
bool cd_warned_slow = false;
int64_t cd_slow_timeout = 8000; // microseconds

// If true, PAL games will run at 60fps
bool fast_pal = false;
unsigned image_height = 0;

#ifdef HAVE_LIGHTREC
enum DYNAREC psx_dynarec;
bool psx_dynarec_invalidate;
uint8_t psx_mmap = 0;
uint8_t *psx_mem = NULL;
uint8_t *psx_bios = NULL;
uint8_t *psx_scratch = NULL;
#if defined(HAVE_ASHMEM)
int memfd;
#endif
#endif

int32_t EventCycles = 128;
uint8_t spu_samples = 1;

// CPU overclock factor (or 0 if disabled)
int32_t psx_overclock_factor = 0;
// GPU rasterizer overclock shift
unsigned psx_gpu_overclock_shift = 0;

// Sets how often (in number of output frames/retro_run invocations)
// the internal framerace counter should be updated if
// display_internal_framerate is true.
#define INTERNAL_FPS_SAMPLE_PERIOD 64

static int psx_skipbios;
static int override_bios;

bool psx_gte_overclock;
//enum dither_mode psx_gpu_dither_mode;

//iCB: PGXP options
unsigned int psx_pgxp_mode;
int psx_pgxp_2d_tol;
unsigned int psx_pgxp_vertex_caching;
unsigned int psx_pgxp_texture_correction;
unsigned int psx_pgxp_nclip;
// \iCB

#define NEGCON_RANGE 0x7FFF

char retro_save_directory[4096];
char retro_base_directory[4096];
char retro_cd_base_directory[4096];
static char retro_cd_path[4096];
char retro_cd_base_name[4096];
#ifdef _WIN32
   static char retro_slash = '\\';
#else
   static char retro_slash = '/';
#endif

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
uint8_t psx_gpu_upscale_shift;
uint8_t psx_gpu_upscale_shift_hw;
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





#define PSX_EVENT_MAXTS             0x20000000


static bool firmware_is_present(unsigned region)
{
   static const size_t list_size = 10;
   const char *bios_name_list[list_size];
   const char *bios_sha1 = NULL;

   log_cb(RETRO_LOG_INFO, "Checking if required firmware is present...\n");

   /* SHA1 and alternate BIOS names sourced from
   https://github.com/mamedev/mame/blob/master/src/mame/drivers/psx.cpp */


   if (override_bios)
   {
      if (override_bios == 1)
      {
			bios_name_list[0] = "psxonpsp660.bin";
			bios_name_list[1] = "PSXONPSP660.bin";
			bios_name_list[2] = NULL;
			bios_sha1 = "96880D1CA92A016FF054BE5159BB06FE03CB4E14";
      }
		
      else if (override_bios == 2)
      {
			bios_name_list[0] = "ps1_rom.bin";
			bios_name_list[1] = "PS1_ROM.bin";
			bios_name_list[2] = NULL;
			bios_sha1 = "C40146361EB8CF670B19FDC9759190257803CAB7";
      }
	   
      size_t i;
      for (i = 0; i < list_size; ++i)
      {
         if (!bios_name_list[i])
            break;

         int r = snprintf(bios_path, sizeof(bios_path), "%s%c%s", retro_base_directory, retro_slash, bios_name_list[i]);
         if (r >= 4096)
         {
            bios_path[4095] = '\0';
            log_cb(RETRO_LOG_ERROR, "Firmware path longer than 4095: %s\n", bios_path);
            break;
         }

         if (filestream_exists(bios_path))
         {
            firmware_found = true;
            break;
         }
      }

      if (firmware_found)
      {	
         char obtained_sha1[41];
         sha1_calculate(bios_path, obtained_sha1);
         if (strcmp(obtained_sha1, bios_sha1))
         {
            log_cb(RETRO_LOG_WARN, "Override firmware found but has invalid SHA1: %s\n", bios_path);
            log_cb(RETRO_LOG_WARN, "Expected SHA1: %s\n", bios_sha1);
            log_cb(RETRO_LOG_WARN, "Obtained SHA1: %s\n", obtained_sha1);
            log_cb(RETRO_LOG_WARN, "Unsupported firmware may cause emulation glitches.\n");
            return true;
         }

         log_cb(RETRO_LOG_INFO, "Override firmware found: %s\n", bios_path);
         log_cb(RETRO_LOG_INFO, "Override firmware SHA1: %s\n", obtained_sha1);

         return true;
      }
      log_cb(RETRO_LOG_WARN, "Override firmware is missing: %s\n", bios_name_list[0]);
      log_cb(RETRO_LOG_WARN, "Fallback to region specific firmware.\n");
   }


   if (region == REGION_JP)
   {
      bios_name_list[0] = "scph5500.bin";
      bios_name_list[1] = "SCPH5500.bin";
      bios_name_list[2] = "SCPH-5500.bin";
      bios_name_list[3] = NULL;
      bios_sha1 = "B05DEF971D8EC59F346F2D9AC21FB742E3EB6917";
   }
   else if (region == REGION_NA)
   {
      bios_name_list[0] = "scph5501.bin";
      bios_name_list[1] = "SCPH5501.bin";
      bios_name_list[2] = "SCPH-5501.bin";
      bios_name_list[3] = "scph5503.bin";
      bios_name_list[4] = "SCPH5503.bin";
      bios_name_list[5] = "SCPH-5503.bin";
      bios_name_list[6] = "scph7003.bin";
      bios_name_list[7] = "SCPH7003.bin";
      bios_name_list[8] = "SCPH-7003.bin";
      bios_name_list[9] = NULL;
      bios_sha1 = "0555C6FAE8906F3F09BAF5988F00E55F88E9F30B";
   }
   else if (region == REGION_EU)
   {
      bios_name_list[0] = "scph5502.bin";
      bios_name_list[1] = "SCPH5502.bin";
      bios_name_list[2] = "SCPH-5502.bin";
      bios_name_list[3] = "scph5552.bin";
      bios_name_list[4] = "SCPH5552.bin";
      bios_name_list[5] = "SCPH-5552.bin";
      bios_name_list[6] = NULL;
      bios_sha1 = "F6BC2D1F5EB6593DE7D089C425AC681D6FFFD3F0";
   }

   size_t i;
   for (i = 0; i < list_size; ++i)
   {
      if (!bios_name_list[i])
         break;

      int r = snprintf(bios_path, sizeof(bios_path), "%s%c%s", retro_base_directory, retro_slash, bios_name_list[i]);
      if (r >= 4096)
      {
         bios_path[4095] = '\0';
         log_cb(RETRO_LOG_ERROR, "Firmware path longer than 4095: %s\n", bios_path);
         break;
      }

      if (filestream_exists(bios_path))
      {
         firmware_found = true;
         break;
      }
   }

   if (!firmware_found)
   {
      char s[4096];

      log_cb(RETRO_LOG_ERROR, "Firmware is missing: %s\n", bios_name_list[0]);
      s[4095] = '\0';

      snprintf(s, sizeof(s), "Firmware is missing:\n\n%s", bios_name_list[0]);

      gui_show = true;

      return false;
   }

   char obtained_sha1[41];
   sha1_calculate(bios_path, obtained_sha1);
   if (strcmp(obtained_sha1, bios_sha1))
   {
      log_cb(RETRO_LOG_WARN, "Firmware found but has invalid SHA1: %s\n", bios_path);
      log_cb(RETRO_LOG_WARN, "Expected SHA1: %s\n", bios_sha1);
      log_cb(RETRO_LOG_WARN, "Obtained SHA1: %s\n", obtained_sha1);
      log_cb(RETRO_LOG_WARN, "Unsupported firmware may cause emulation glitches.\n");
      return true;
   }

   log_cb(RETRO_LOG_INFO, "Firmware found: %s\n", bios_path);
   log_cb(RETRO_LOG_INFO, "Firmware SHA1: %s\n", obtained_sha1);

   return true;
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





#include <stdarg.h>
#include <ctype.h>
#include <string>
#include "libretro_options.h"
#include <cassert>
#include "LibretroWrapper.h"
#include "Pkgs/GL/glew.h"
#include "../../Graphics/SuperAA.h"

bool setting_apply_analog_toggle  = false;
bool setting_apply_analog_default = false;
bool use_mednafen_memcard0_method = false;


#if PSX_DBGPRINT_ENABLE
static unsigned psx_dbg_level = 0;

void PSX_DBG(unsigned level, const char *format, ...)
{
   if(psx_dbg_level >= level)
   {
      va_list ap;
      va_start(ap, format);
      vprintf(format, ap);
      va_end(ap);
   }
}
#else
static unsigned const psx_dbg_level = 0;
#endif

/* Based off(but not the same as) public-domain "JKISS" PRNG. */
struct MDFN_PseudoRNG
{
   uint32_t x,y,z,c;
   uint64_t lcgo;
};

static MDFN_PseudoRNG PSX_PRNG;

uint32_t PSX_GetRandU32(uint32_t mina, uint32_t maxa)
{
   uint32_t tmp;
   const uint32_t range_m1 = maxa - mina;
   uint32_t range_mask = range_m1;

   range_mask |= range_mask >> 1;
   range_mask |= range_mask >> 2;
   range_mask |= range_mask >> 4;
   range_mask |= range_mask >> 8;
   range_mask |= range_mask >> 16;

   do
   {
      uint64_t t = 4294584393ULL * PSX_PRNG.z + PSX_PRNG.c;

      PSX_PRNG.x = 314527869 * PSX_PRNG.x + 1234567;
      PSX_PRNG.y ^= PSX_PRNG.y << 5;
      PSX_PRNG.y ^= PSX_PRNG.y >> 7;
      PSX_PRNG.y ^= PSX_PRNG.y << 22;
      PSX_PRNG.c = t >> 32;
      PSX_PRNG.z = t;
      PSX_PRNG.lcgo = (19073486328125ULL * PSX_PRNG.lcgo) + 1;
      tmp = ((PSX_PRNG.x + PSX_PRNG.y + PSX_PRNG.z) ^ (PSX_PRNG.lcgo >> 16)) & range_mask;
   } while(tmp > range_m1);

   return(mina + tmp);
}

static std::vector<char *> CDInterfaces;  // FIXME: Cleanup on error out.
static std::vector<char*> *cdifs = NULL;
static std::vector<const char *> cdifs_scex_ids;

static bool eject_state;

static bool CD_TrayOpen;
int CD_SelectedDisc;     // -1 for no disc

static bool CD_IsPBP = false;
extern int PBP_DiscCount;
/* The global value PBP_DiscCount is set to
 * zero when loading single-disk PBP files.
 * We therefore have to maintain a separate
 * 'physical' disk count, otherwise the
 * frontend disk control interface will fail */
static int PBP_PhysicalDiscCount;

typedef struct
{
   unsigned initial_index;
   std::string initial_path;
   std::vector<std::string> image_paths;
   std::vector<std::string> image_labels;
} disk_control_ext_info_t;

static disk_control_ext_info_t disk_control_ext_info;

static uint64_t Memcard_PrevDC[8];
static int64_t Memcard_SaveDelay[8];


#ifdef HAVE_LIGHTREC
/* Size of Expansion 1 (8MB) */
#define PSX_EXPANSION1_SIZE        0x800000U
/* Base address of Expansion 1 */
#define PSX_EXPANSION1_BASE        0x1F000000U

/* Mednafen splits the expansion in two buffers (PIOMem and TextMem). That's not
 * super convenient for us so I'm going to copy both of them in one contiguous
 * buffer */
const uint8_t *PSX_LoadExpansion1(void) {
   static uint8_t *expansion1 = NULL;

   if (expansion1 == NULL) {
      expansion1 = new uint8_t[PSX_EXPANSION1_SIZE];
   }

   /* Let's read 32bits at a time to speed things up a bit */
   uint32_t *p = reinterpret_cast<uint32_t *>(expansion1);

   for (unsigned i = 0; i < PSX_EXPANSION1_SIZE / 4; i++) {
      p[i] = PSX_MemPeek32(PSX_EXPANSION1_BASE + i * 4);
   }

   return expansion1;
}
#endif

static uint32_t TextMem_Start;
static std::vector<uint8_t> TextMem;

static const uint32_t SysControl_Mask[9] = { 0x00ffffff, 0x00ffffff, 0xffffffff, 0x2f1fffff,
                                             0xffffffff, 0x2f1fffff, 0x2f1fffff, 0xffffffff,
                                             0x0003ffff };

static const uint32_t SysControl_OR[9] = { 0x1f000000, 0x1f000000, 0x00000000, 0x00000000,
                                           0x00000000, 0x00000000, 0x00000000, 0x00000000,
                                           0x00000000 };

static struct
{
   union
   {
      struct
      {
         uint32_t PIO_Base;   // 0x1f801000  // BIOS Init: 0x1f000000, Writeable bits: 0x00ffffff(assumed, verify), FixedOR = 0x1f000000
         uint32_t Unknown0;   // 0x1f801004  // BIOS Init: 0x1f802000, Writeable bits: 0x00ffffff, FixedOR = 0x1f000000
         uint32_t Unknown1;   // 0x1f801008  // BIOS Init: 0x0013243f, ????
         uint32_t Unknown2;   // 0x1f80100c  // BIOS Init: 0x00003022, Writeable bits: 0x2f1fffff, FixedOR = 0x00000000

         uint32_t BIOS_Mapping;  // 0x1f801010  // BIOS Init: 0x0013243f, ????
         uint32_t SPU_Delay;  // 0x1f801014  // BIOS Init: 0x200931e1, Writeable bits: 0x2f1fffff, FixedOR = 0x00000000 - Affects bus timing on access to SPU
         uint32_t CDC_Delay;  // 0x1f801018  // BIOS Init: 0x00020843, Writeable bits: 0x2f1fffff, FixedOR = 0x00000000
         uint32_t Unknown4;   // 0x1f80101c  // BIOS Init: 0x00070777, ????
         uint32_t Unknown5;   // 0x1f801020  // BIOS Init: 0x00031125(but rewritten with other values often), Writeable bits: 0x0003ffff, FixedOR = 0x00000000 -- Possibly CDC related
      };
      uint32_t Regs[9];
   };
} SysControl;

static unsigned DMACycleSteal = 0;   // Doesn't need to be saved in save states, since it's calculated in the ForceEventUpdates() call chain.

void PSX_SetDMACycleSteal(unsigned stealage)
{
   if (stealage > 200) // Due to 8-bit limitations in the CPU core.
      stealage = 200;

   DMACycleSteal = stealage;
}

//
// Event stuff
//

static int32_t Running; // Set to -1 when not desiring exit, and 0 when we are.

struct event_list_entry
{
   uint32_t which;
   int32_t event_time;
   event_list_entry *prev;
   event_list_entry *next;
};

static event_list_entry events[10];

static void EventReset(void)
{
   unsigned i;
   for(i = 0; i < PSX_EVENT__COUNT; i++)
   {
      events[i].which = i;

      if(i == PSX_EVENT__SYNFIRST)
        events[i].event_time = (int32_t)0x80000000;
      else if(i == PSX_EVENT__SYNLAST)
         events[i].event_time = 0x7FFFFFFF;
      else
         events[i].event_time = PSX_EVENT_MAXTS;

      events[i].prev = (i > 0) ? &events[i - 1] : NULL;
      events[i].next = (i < (PSX_EVENT__COUNT - 1)) ? &events[i + 1] : NULL;
   }
}

//static void RemoveEvent(event_list_entry *e)
//{
// e->prev->next = e->next;
// e->next->prev = e->prev;
//}

static void RebaseTS(const int32_t timestamp)
{
   unsigned i;
   for(i = 0; i < PSX_EVENT__COUNT; i++)
   {
      if(i == PSX_EVENT__SYNFIRST || i == PSX_EVENT__SYNLAST)
         continue;

      assert(events[i].event_time > timestamp);
      events[i].event_time -= timestamp;
   }

   //PSX_CPU->SetEventNT(events[PSX_EVENT__SYNFIRST].next->event_time);
}

void PSX_SetEventNT(const int type, const int32_t next_timestamp)
{
   event_list_entry *e = &events[type];

   if(next_timestamp < e->event_time)
   {
      event_list_entry *fe = e;

      do
      {
         fe = fe->prev;
      }while(next_timestamp < fe->event_time);

      // Remove this event from the list, temporarily of course.
      e->prev->next = e->next;
      e->next->prev = e->prev;

      // Insert into the list, just after "fe".
      e->prev = fe;
      e->next = fe->next;
      fe->next->prev = e;
      fe->next = e;

      e->event_time = next_timestamp;
   }
   else if(next_timestamp > e->event_time)
   {
      event_list_entry *fe = e;

      do
      {
         fe = fe->next;
      } while(next_timestamp > fe->event_time);

      // Remove this event from the list, temporarily of course
      e->prev->next = e->next;
      e->next->prev = e->prev;

      // Insert into the list, just BEFORE "fe".
      e->prev = fe->prev;
      e->next = fe;
      fe->prev->next = e;
      fe->prev = e;

      e->event_time = next_timestamp;
   }

   //PSX_CPU->SetEventNT(events[PSX_EVENT__SYNFIRST].next->event_time & Running);
}

// Called from debug.cpp too.
void ForceEventUpdates(const int32_t timestamp)
{
   // PSX_SetEventNT(PSX_EVENT_GPU, GPU_Update(timestamp));
   // PSX_SetEventNT(PSX_EVENT_CDC, PSX_CDC->Update(timestamp));

   // PSX_SetEventNT(PSX_EVENT_TIMER, TIMER_Update(timestamp));

   // PSX_SetEventNT(PSX_EVENT_DMA, DMA_Update(timestamp));

   // PSX_SetEventNT(PSX_EVENT_FIO, PSX_FIO->Update(timestamp));

   // PSX_CPU->SetEventNT(events[PSX_EVENT__SYNFIRST].next->event_time);
}

bool /*MDFN_FASTCALL PSX*/_EventHandler(const int32_t timestamp)
{
   // event_list_entry *e = events[PSX_EVENT__SYNFIRST].next;

   // while(timestamp >= e->event_time)   // If Running = 0, PSX_EventHandler() may be called even if there isn't an event per-se, so while() instead of do { ... } while
   // {
   //    int32_t nt;
   //    event_list_entry *prev = e->prev;

   //    switch(e->which)
   //    {
   //       default:
   //          abort();
   //       case PSX_EVENT_GPU:
   //          nt = GPU_Update(e->event_time);
   //          break;
   //       case PSX_EVENT_CDC:
   //          nt = PSX_CDC->Update(e->event_time);
   //          break;
   //       case PSX_EVENT_TIMER:
   //          nt = TIMER_Update(e->event_time);
   //          break;
   //       case PSX_EVENT_DMA:
   //          nt = DMA_Update(e->event_time);
   //          break;
   //       case PSX_EVENT_FIO:
   //          nt = PSX_FIO->Update(e->event_time);
   //          break;
   //    }

   //    PSX_SetEventNT(e->which, nt);

   //    // Order of events can change due to calling PSX_SetEventNT(), this prev business ensures we don't miss an event due to reordering.
   //    e = prev->next;
   // }

   return(Running);
}


void PSX_RequestMLExit(void)
{
   Running = 0;
   // PSX_CPU->SetEventNT(0);
}


//
// End event stuff
//


/* Remember to update MemPeek<>() and MemPoke<>() when we change address decoding in MemRW() */
template<typename T, bool IsWrite, bool Access24> static INLINE void MemRW(int32_t &timestamp, uint32_t A, uint32_t &V)
{
#if 0
   if(IsWrite)
      printf("Write%d: %08x(orig=%08x), %08x\n", (int)(sizeof(T) * 8), A & mask[A >> 29], A, V);
   else
      printf("Read%d: %08x(orig=%08x)\n", (int)(sizeof(T) * 8), A & mask[A >> 29], A);
#endif
}

void /*MDFN_FASTCALL*/ PSX_MemWrite8(int32_t timestamp, uint32_t A, uint32_t V)
{
   //MemRW<uint8_t, true, false>(timestamp, A, V);
}

void /*MDFN_FASTCALL*/ PSX_MemWrite16(int32_t timestamp, uint32_t A, uint32_t V)
{
   //MemRW<uint16_t, true, false>(timestamp, A, V);
}

void /*MDFN_FASTCALL*/ PSX_MemWrite24(int32_t timestamp, uint32_t A, uint32_t V)
{
   //MemRW<uint32_t, true, true>(timestamp, A, V);
}

void /*MDFN_FASTCALL*/ PSX_MemWrite32(int32_t timestamp, uint32_t A, uint32_t V)
{
   //MemRW<uint32_t, true, false>(timestamp, A, V);
}

uint8_t /*MDFN_FASTCALL*/ PSX_MemRead8(int32_t &timestamp, uint32_t A)
{
   uint32_t V;

   //MemRW<uint8_t, false, false>(timestamp, A, V);

   return(V);
}

uint16_t /*MDFN_FASTCALL*/ PSX_MemRead16(int32_t &timestamp, uint32_t A)
{
   uint32_t V;

   //MemRW<uint16_t, false, false>(timestamp, A, V);

   return(V);
}

uint32_t /*MDFN_FASTCALL*/ PSX_MemRead24(int32_t &timestamp, uint32_t A)
{
   uint32_t V;

   //MemRW<uint32_t, false, true>(timestamp, A, V);

   return(V);
}

uint32_t /*MDFN_FASTCALL*/ PSX_MemRead32(int32_t &timestamp, uint32_t A)
{
   uint32_t V;

   //MemRW<uint32_t, false, false>(timestamp, A, V);

   return(V);
}

// FIXME: Add PSX_Reset() and FrontIO::Reset() so that emulated input devices don't get power-reset on reset-button reset.
static void PSX_Power(void)
{
   unsigned i;

   PSX_PRNG.x = 123456789;
   PSX_PRNG.y = 987654321;
   PSX_PRNG.z = 43219876;
   PSX_PRNG.c = 6543217;
   PSX_PRNG.lcgo = 0xDEADBEEFCAFEBABEULL;

   cd_warned_slow = false;

  
}


void PSX_GPULineHook(const int32_t timestamp, const int32_t line_timestamp, bool vsync, uint32_t *pixels, const char* const format, const unsigned width, const unsigned pix_clock_offset, const unsigned pix_clock, const unsigned pix_clock_divider, const unsigned surf_pitchinpix, const unsigned upscale_factor)
{
   //PSX_FIO->GPULineHook(timestamp, line_timestamp, vsync, pixels, format, width, pix_clock_offset, pix_clock, pix_clock_divider, surf_pitchinpix, upscale_factor);
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

static bool TestMagicCD(std::vector<char *> *_CDInterfaces)
{
   uint8_t buf[2048];
   //TOC toc;
   int dt;

   //TOC_Clear(&toc);

   //(*_CDInterfaces)[0]->ReadTOC(&toc);

   //dt = TOC_FindTrackByLBA(&toc, 4);

   // if(dt > 0 && !(toc.tracks[dt].control & 0x4))
   //    return(false);

   // if((*_CDInterfaces)[0]->ReadSector(buf, 4, 1) != 0x2)
   //    return(false);

   // if(strncmp((char *)buf + 10, "Licensed  by", strlen("Licensed  by")))
   //    return(false);

   return(true);
}


static void SetDiscWrapper(const bool CD_TrayOpen) {
    char *cdif = NULL;
    const char *disc_id = NULL;
    if (CD_SelectedDisc >= 0 && !CD_TrayOpen) {
        // only allow one pbp file to be loaded (at index 0)
        if (CD_IsPBP) {
            cdif = (*cdifs)[0];
            disc_id = cdifs_scex_ids[0];
        } else {
            cdif = (*cdifs)[CD_SelectedDisc];
            disc_id = cdifs_scex_ids[CD_SelectedDisc];
        }
    }

    //PSX_CDC->SetDisc(CD_TrayOpen, cdif, disc_id);
}

#ifdef HAVE_LIGHTREC
/* MAP_FIXED_NOREPLACE allows base 0 to work if "sysctl vm.mmap_min_addr = 0"
 was used. Base 0 will perform better by directly mapping emulated addresses
 to host addresses. If MAP_FIXED_NOREPLACE is not available we should not use
 MAP_FIXED, since it can cause strange crashes by unmapping memory mappings. */
#ifndef MAP_FIXED_NOREPLACE
#ifdef USE_FIXED
#define MAP_FIXED_NOREPLACE MAP_FIXED
#else
#define MAP_FIXED_NOREPLACE 0
#endif
#endif

static const uintptr_t supported_io_bases[] = {
#if !__MACOS__
	static_cast<uintptr_t>(0x00000000),
	static_cast<uintptr_t>(0x10000000),
	static_cast<uintptr_t>(0x20000000),
	static_cast<uintptr_t>(0x30000000),
#else
   static_cast<uintptr_t>(MACOS_VM_BASE),
#endif
	static_cast<uintptr_t>(0x40000000),
	static_cast<uintptr_t>(0x50000000),
	static_cast<uintptr_t>(0x60000000),
	static_cast<uintptr_t>(0x70000000),
	static_cast<uintptr_t>(0x80000000),
	static_cast<uintptr_t>(0x90000000),
   /* Some platforms need higher address base for mmap to work */
#if UINTPTR_MAX == UINT64_MAX
	static_cast<uintptr_t>(0x100000000),
	static_cast<uintptr_t>(0x200000000),
	static_cast<uintptr_t>(0x300000000),
	static_cast<uintptr_t>(0x400000000),
	static_cast<uintptr_t>(0x500000000),
	static_cast<uintptr_t>(0x600000000),
	static_cast<uintptr_t>(0x700000000),
	static_cast<uintptr_t>(0x800000000),
	static_cast<uintptr_t>(0x900000000),
#endif
};

#define RAM_SIZE 0x200000
#define BIOS_SIZE 0x80000
#define SCRATCH_SIZE 0x400
#define SHM_SIZE RAM_SIZE+BIOS_SIZE+SCRATCH_SIZE

#ifdef HAVE_WIN_SHM
#define MAP(addr, size, fd, offset) \
	MapViewOfFileEx(fd, FILE_MAP_ALL_ACCESS, 0, offset, size, addr)
#define UNMAP(addr, size) UnmapViewOfFile(addr)
#define MFAILED NULL
#define NUM_MEM 4
#elif defined(HAVE_SHM) || defined(HAVE_ASHMEM)
#define MAP(addr, size, fd, offset) \
	mmap(addr,size, PROT_READ | PROT_WRITE, \
		MAP_SHARED | MAP_FIXED_NOREPLACE, fd, offset)
#define UNMAP(addr, size) munmap(addr, size)
#define MFAILED MAP_FAILED
#define NUM_MEM 4
#else
#define MAP(addr, size, fd, offset) \
	mmap(addr,size, PROT_READ | PROT_WRITE, \
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)
#define UNMAP(addr, size) munmap(addr, size)
#define MFAILED MAP_FAILED
#define NUM_MEM 1
#endif

int lightrec_init_mmap()
{
	int r = 0, i, j;
	uintptr_t base;
	void *bios, *scratch, *map;

/* open memfd and set size */
#ifdef HAVE_ASHMEM
	memfd = open("/dev/ashmem", O_RDWR);

	if (memfd < 0) {
		/* Android 10+ / API 29+ gives EACCES (permission denied) opening /dev/ashmem
		 * fallback to ASharedMemory_create available since Android 8 / API 26 */
		if(errno == EACCES) {
			void *lib;
			int (*create)(const char*, size_t);
			int (*setProt)(int, int);
			char *error1, *error2;

			dlerror();      /* Clear any existing error */
			lib = dlopen("libandroid.so", RTLD_NOW);
			if (lib == NULL) {
				log_cb(RETRO_LOG_ERROR, "Failed to dlopen: %s\n", dlerror());
				return 0;
			}

			*(void **)(&create) = dlsym(lib, "ASharedMemory_create");
			error1 = dlerror();
			*(void **)(&setProt) = dlsym(lib, "ASharedMemory_setProt");
			error2 = dlerror();

			if (error1 == NULL)
				memfd = (*create)("lightrec_memfd",SHM_SIZE);

			if (memfd < 0) {
				log_cb(RETRO_LOG_ERROR, "Failed to ASharedMemory_create: %s\n",
							(error1 != NULL) ? error1 : strerror(errno));
				dlclose(lib);
				return 0;
			}

			if (error2 != NULL || (((*setProt)(memfd, PROT_READ|PROT_WRITE)) < 0))
				log_cb(RETRO_LOG_ERROR, "Failed to ASharedMemory_setProt: %s\n",
							(error2 != NULL) ? error2 : strerror(errno));

			dlclose(lib);
		} else {
			log_cb(RETRO_LOG_ERROR, "Failed to create ASHMEM: %s\n", strerror(errno));
			return 0;
		}
	} else {
		ioctl(memfd, ASHMEM_SET_NAME, "lightrec_memfd");
		ioctl(memfd, ASHMEM_SET_SIZE, SHM_SIZE);
	}
#endif
#ifdef HAVE_SHM
	int memfd;
	const char *shm_name = "/lightrec_memfd_beetle";

	memfd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);

	if (memfd < 0 && errno == EEXIST) {
		shm_unlink(shm_name);
		memfd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	}

	if (memfd < 0) {
		log_cb(RETRO_LOG_ERROR, "Failed to create SHM: %s\n", strerror(errno));
		return 0;
	}

	/* unlink ASAP to prevent leaving a file in shared memory if we crash */
	shm_unlink(shm_name);

	if (ftruncate(memfd, SHM_SIZE) < 0) {
		log_cb(RETRO_LOG_ERROR, "Could not truncate SHM size: %s\n", strerror(errno));
		goto close_return;
	}
#endif
#ifdef HAVE_WIN_SHM
	HANDLE memfd;

	memfd = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, SHM_SIZE, NULL);

	if (memfd == NULL) {
		log_cb(RETRO_LOG_ERROR, "Failed to create WIN_SHM: %s (%d)\n", strerror(errno), GetLastError());
		return 0;
	}
#endif

	/* Try to map at various base addresses*/
	for (i = 0; i < ARRAY_SIZE(supported_io_bases); i++) {
		base = supported_io_bases[i];
		bios = (void *)(base + 0x1fc00000);
		scratch = (void *)(base + 0x1f800000);

		for (j = 0; j < NUM_MEM; j++) {
			map = MAP((void *)(base + j * RAM_SIZE), RAM_SIZE, memfd, 0);
			if (map == MFAILED)
				break;
			else if (map != (void *)(base + j * RAM_SIZE))
			{
				//not at expected address, reject it
				UNMAP(map, RAM_SIZE);
				break;
			}
		}

		/* Impossible to map using this base */
		if (j == 0)
			continue;

		/* All mirrors mapped - we got a match! */
		if (j == NUM_MEM)
		{
			psx_mem = (uint8_t *)base;

			map = MAP(bios, BIOS_SIZE, memfd, RAM_SIZE);
			if (map == MFAILED)
				goto err_unmap;

			psx_bios = (uint8_t *)map;

			if (map != bios)
				goto err_unmap_bios;

			map = MAP(scratch, SCRATCH_SIZE, memfd, RAM_SIZE+BIOS_SIZE);
			if (map == MFAILED)
				goto err_unmap_bios;

			psx_scratch = (uint8_t *)map;

			if (map != scratch)
				goto err_unmap_scratch;

			r = NUM_MEM;

			goto close_return;
		}

err_unmap_scratch:
		if(psx_scratch){
			UNMAP(psx_scratch, SCRATCH_SIZE);
			psx_scratch = NULL;
		}
err_unmap_bios:
		if(psx_bios){
			UNMAP(psx_bios, BIOS_SIZE);
			psx_bios = NULL;
		}
err_unmap:
		/* Clean up any mapped ram or mirrors and try again */
		for (; j > 0; j--)
			UNMAP((void *)(base + (j - 1) * RAM_SIZE), RAM_SIZE);

		psx_mem = NULL;
	}

	if (i == ARRAY_SIZE(supported_io_bases)) {
		log_cb(RETRO_LOG_WARN, "Unable to mmap on any base address, dynarec will be slower\n");
	}

close_return:
#ifdef HAVE_SHM
	close(memfd);
#endif
#ifdef HAVE_WIN_SHM
	CloseHandle(memfd);
#endif
	return r;
}

void lightrec_free_mmap()
{
	for (int i = 0; i < NUM_MEM; i++)
		UNMAP((void *)((uintptr_t)psx_mem + i * RAM_SIZE), RAM_SIZE);

	UNMAP(psx_bios, BIOS_SIZE);
	UNMAP(psx_scratch, SCRATCH_SIZE);

#ifdef HAVE_ASHMEM
	/* android shared memory is not pinned by mmap, it dies on close */
	close(memfd);
#endif
}
#endif /* HAVE_LIGHTREC */

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

/* Forward declarations, required for disk control
 * 'set initial disk' functionality */
static unsigned disk_get_num_images(void);
static void CDInsertEject(void);
static void CDEject(void);

static void InitCommon(std::vector<char *> *_CDInterfaces, const bool EmulateMemcards = true, const bool WantPIOMem = false)
{
   unsigned region, i;
   bool emulate_memcard[8];
   bool emulate_multitap[2];
   int sls, sle;

#if PSX_DBGPRINT_ENABLE
   psx_dbg_level = MDFN_GetSettingUI("psx.dbg_level");
#endif

   for(i = 0; i < 8; i++)
   {
      char buf[64];
      snprintf(buf, sizeof(buf), "psx.input.port%u.memcard", i + 1);
      emulate_memcard[i] = EmulateMemcards /*&& MDFN_GetSettingB(buf)*/;
   }

   if (!enable_memcard1) {
     emulate_memcard[1] = false;
   }

   emulate_multitap[0]; //= setting_psx_multitap_port_1;
   emulate_multitap[1]; //= setting_psx_multitap_port_2;

   cdifs  = _CDInterfaces;
   //region = CalcDiscSCEx();

   // if(!MDFN_GetSettingB("psx.region_autodetect"))
   //    region = MDFN_GetSettingI("psx.region_default");

   // sls = MDFN_GetSettingI((region == REGION_EU) ? "psx.slstartp" : "psx.slstart");
   // sle = MDFN_GetSettingI((region == REGION_EU) ? "psx.slendp" : "psx.slend");

   if(sls > sle)
   {
      int tmp = sls;
      sls = sle;
      sle = tmp;
   }

   // PSX_CPU = new PS_CPU();
   // PSX_SPU = new PS_SPU();

   // GPU_Init(region == REGION_EU, sls, sle, psx_gpu_upscale_shift);

   // PSX_CDC = new PS_CDC();
   // PSX_FIO = new FrontIO(emulate_memcard, emulate_multitap);
   // PSX_FIO->SetAMCT(setting_psx_analog_toggle);
   // for(unsigned i = 0; i < 2; i++)
   // {
   //    char buf[64];
   //    snprintf(buf, sizeof(buf), "psx.input.port%u.gun_chairs", i + 1);
   //    PSX_FIO->SetCrosshairsColor(i, MDFN_GetSettingUI(buf));
   // }

   // input_set_fio(PSX_FIO);

   // DMA_Init();

   // GPU_FillVideoParams(&EmulatedPSX);

   // switch (psx_gpu_dither_mode)
   // {
   //    case DITHER_NATIVE:
   //       GPU_set_dither_upscale_shift(psx_gpu_upscale_shift);
   //       break;
   //    case DITHER_UPSCALED:
   //       GPU_set_dither_upscale_shift(0);
   //       break;
   //    case DITHER_OFF:
   //       break;
   // }

   // PGXP_SetModes(psx_pgxp_mode | psx_pgxp_vertex_caching | psx_pgxp_texture_correction | psx_pgxp_nclip);

   // CD_TrayOpen        = true;
   // CD_SelectedDisc    = -1;

   // if(cdifs)
   // {
   //    CD_TrayOpen     = false;
   //    CD_SelectedDisc = 0;

   //    /* Attempt to set initial disk index */
   //    if ((disk_control_ext_info.initial_index > 0) &&
   //        (disk_control_ext_info.initial_index < disk_get_num_images()))
   //       if (disk_control_ext_info.initial_index <
   //             disk_control_ext_info.image_paths.size())
   //          if (string_is_equal(
   //                disk_control_ext_info.image_paths[disk_control_ext_info.initial_index].c_str(),
   //                disk_control_ext_info.initial_path.c_str()))
   //             CD_SelectedDisc = (int)disk_control_ext_info.initial_index;
   // }

   // PSX_CDC->SetDisc(true, NULL, NULL);

   // /* Multi-disk PBP files cause additional complication
   //  * here, since the first disk is always loaded by default */
   // if(CD_IsPBP && (CD_SelectedDisc > 0))
   // {
   //    CDEject();
   //    CDInsertEject();
   // }
   // else
   //    SetDiscWrapper(CD_TrayOpen);

#ifdef HAVE_LIGHTREC
   psx_mmap = lightrec_init_mmap();

   if(psx_mmap > 0)
   {
      MainRAM = new(psx_mem) MultiAccessSizeMem<RAM_SIZE, uint32_t, false>();
      ScratchRAM = new(psx_scratch) MultiAccessSizeMem<SCRATCH_SIZE, uint32_t, false>();
      BIOSROM = new(psx_bios) MultiAccessSizeMem<BIOS_SIZE, uint32_t, false>();
   }
   else
#endif
   {
      // MainRAM = new MultiAccessSizeMem<2048 * 1024, uint32_t, false>();
      // ScratchRAM = new MultiAccessSizeMem<1024, uint32_t, false>();
      // BIOSROM = new MultiAccessSizeMem<512 * 1024, uint32_t, false>();
   }

   // PIOMem  = NULL;

   // if(WantPIOMem)
   //    PIOMem = new MultiAccessSizeMem<65536, uint32_t, false>();

   // for(uint32_t ma = 0x00000000; ma < 0x00800000; ma += 2048 * 1024)
   // {
   //    PSX_CPU->SetFastMap(MainRAM->data32, 0x00000000 + ma, 2048 * 1024);
   //    PSX_CPU->SetFastMap(MainRAM->data32, 0x80000000 + ma, 2048 * 1024);
   //    PSX_CPU->SetFastMap(MainRAM->data32, 0xA0000000 + ma, 2048 * 1024);
   // }

   // PSX_CPU->SetFastMap(BIOSROM->data32, 0x1FC00000, 512 * 1024);
   // PSX_CPU->SetFastMap(BIOSROM->data32, 0x9FC00000, 512 * 1024);
   // PSX_CPU->SetFastMap(BIOSROM->data32, 0xBFC00000, 512 * 1024);

   // if(PIOMem)
   // {
   //    PSX_CPU->SetFastMap(PIOMem->data32, 0x1F000000, 65536);
   //    PSX_CPU->SetFastMap(PIOMem->data32, 0x9F000000, 65536);
   //    PSX_CPU->SetFastMap(PIOMem->data32, 0xBF000000, 65536);
   // }


   // MDFNMP_Init(1024, ((uint64)1 << 29) / 1024);
   // MDFNMP_AddRAM(2048 * 1024, 0x00000000, MainRAM->data8);
#if 0
   MDFNMP_AddRAM(1024, 0x1F800000, ScratchRAM.data8);
#endif

   RFILE *BIOSFile;

   if(firmware_is_present(region))
   {
      BIOSFile      = filestream_open(bios_path,
            RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
   }
   else
   {
      const char *biospath_sname;

      if(region == REGION_JP)
         biospath_sname = "psx.bios_jp";
      else if(region == REGION_EU)
         biospath_sname = "psx.bios_eu";
      else if(region == REGION_NA)
         biospath_sname = "psx.bios_na";
      else
         abort();

      const char *biospath;// = MDFN_MakeFName(MDFNMKF_FIRMWARE,
            //0, MDFN_GetSettingS(biospath_sname));

      BIOSFile      = filestream_open(biospath,
            RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
   }

      if (BIOSFile)
      {
         //filestream_read(BIOSFile, BIOSROM->data8, 512 * 1024);
         filestream_close(BIOSFile);
      }

   i = 0;

   if (!use_mednafen_memcard0_method)
   {
      //PSX_FIO->LoadMemcard(0);
      i = 1;
   }

   for(; i < 8; i++)
   {
      char ext[64];
      const char *memcard = NULL;
      if (i == 0)
         snprintf(ext, sizeof(ext), "%d.mcr", memcard_left_index);
      else if (i == 1)
         snprintf(ext, sizeof(ext), "%d.mcr", memcard_right_index);
      else
         snprintf(ext, sizeof(ext), "%d.mcr", i);
      // memcard = MDFN_MakeFName(MDFNMKF_SAV, 0, ext);
      // PSX_FIO->LoadMemcard(i, memcard);
   }

   for(i = 0; i < 8; i++)
   {
      //Memcard_PrevDC[i] = PSX_FIO->GetMemcardDirtyCount(i);
      Memcard_SaveDelay[i] = -1;
   }

	//input_init_calibration();

#ifdef WANT_DEBUGGER
   DBG_Init();
#endif
   PSX_Power();
}

static bool LoadEXE(const uint8_t *data, const uint32_t size, bool ignore_pcsp = false)
{
   uint32_t PC;       // = MDFN_de32lsb<false>(&data[0x10]);
   uint32_t SP ;      // = MDFN_de32lsb<false>(&data[0x30]);
   uint32_t TextStart;// = MDFN_de32lsb<false>(&data[0x18]);
   uint32_t TextSize ;// = MDFN_de32lsb<false>(&data[0x1C]);

   if(ignore_pcsp)
      log_cb(RETRO_LOG_DEBUG, "TextStart=0x%08x\nTextSize=0x%08x\n", TextStart, TextSize);
   else
      log_cb(RETRO_LOG_DEBUG, "PC=0x%08x\nSP=0x%08x\nTextStart=0x%08x\nTextSize=0x%08x\n", PC, SP, TextStart, TextSize);

   TextStart &= 0x1FFFFF;

   if(TextSize > 2048 * 1024)
   {
      //MDFN_Error(0, "Text section too large");
      return false;
   }

   if(TextSize > (size - 0x800))
   {
      //MDFN_Error(0, "Text section recorded size is larger than data available in file.  Header=0x%08x, Available=0x%08x", TextSize, size - 0x800);
      return false;
   }

   if(TextSize < (size - 0x800))
   {
      //MDFN_Error(0, "Text section recorded size is smaller than data available in file.  Header=0x%08x, Available=0x%08x", TextSize, size - 0x800);
      return false;
   }

   if(!TextMem.size())
   {
      TextMem_Start = TextStart;
      TextMem.resize(TextSize);
   }

   if(TextStart < TextMem_Start)
   {
      uint32_t old_size = TextMem.size();

      //printf("RESIZE: 0x%08x\n", TextMem_Start - TextStart);

      TextMem.resize(old_size + TextMem_Start - TextStart);
      memmove(&TextMem[TextMem_Start - TextStart], &TextMem[0], old_size);

      TextMem_Start = TextStart;
   }

   if(TextMem.size() < (TextStart - TextMem_Start + TextSize))
      TextMem.resize(TextStart - TextMem_Start + TextSize);

   memcpy(&TextMem[TextStart - TextMem_Start], data + 0x800, TextSize);

   // BIOS patch
   //BIOSROM->WriteU32(0x6990, (3 << 26) | ((0xBF001000 >> 2) & ((1 << 26) - 1)));
#if 0
   BIOSROM->WriteU32(0x691C, (3 << 26) | ((0xBF001000 >> 2) & ((1 << 26) - 1)));
#endif

   uint8_t *po;

   //po = &PIOMem->data8[0x0800];

   //MDFN_en32lsb<false>(po, (0x0 << 26) | (31 << 21) | (0x8 << 0)); // JR
   po += 4;
   //MDFN_en32lsb<false>(po, 0); // NOP(kinda)
   po += 4;

   //po = &PIOMem->data8[0x1000];

   // Load cacheable-region target PC into r2
   //MDFN_en32lsb<false>(po, (0xF << 26) | (0 << 21) | (1 << 16) | (0x9F001010 >> 16));      // LUI
   po += 4;
   //MDFN_en32lsb<false>(po, (0xD << 26) | (1 << 21) | (2 << 16) | (0x9F001010 & 0xFFFF));   // ORI
   po += 4;

   // Jump to r2
   //MDFN_en32lsb<false>(po, (0x0 << 26) | (2 << 21) | (0x8 << 0));  // JR
   po += 4;
   //MDFN_en32lsb<false>(po, 0); // NOP(kinda)
   po += 4;

   //
   // 0x9F001010:
   //

   // Load source address into r8
   uint32_t sa = 0x9F000000 + 65536;
   //MDFN_en32lsb<false>(po, (0xF << 26) | (0 << 21) | (1 << 16) | (sa >> 16));  // LUI
   po += 4;
   //MDFN_en32lsb<false>(po, (0xD << 26) | (1 << 21) | (8 << 16) | (sa & 0xFFFF));  // ORI
   po += 4;

   // Load dest address into r9
   //MDFN_en32lsb<false>(po, (0xF << 26) | (0 << 21) | (1 << 16)  | (TextMem_Start >> 16));  // LUI
   po += 4;
   //MDFN_en32lsb<false>(po, (0xD << 26) | (1 << 21) | (9 << 16) | (TextMem_Start & 0xFFFF));   // ORI
   po += 4;

   // Load size into r10
   //MDFN_en32lsb<false>(po, (0xF << 26) | (0 << 21) | (1 << 16)  | (TextMem.size() >> 16)); // LUI
   po += 4;
   //MDFN_en32lsb<false>(po, (0xD << 26) | (1 << 21) | (10 << 16) | (TextMem.size() & 0xFFFF));    // ORI
   po += 4;

   //
   // Loop begin
   //

   //MDFN_en32lsb<false>(po, (0x24 << 26) | (8 << 21) | (1 << 16));  // LBU to r1
   po += 4;

   //MDFN_en32lsb<false>(po, (0x08 << 26) | (10 << 21) | (10 << 16) | 0xFFFF);   // Decrement size
   po += 4;

   //MDFN_en32lsb<false>(po, (0x28 << 26) | (9 << 21) | (1 << 16));  // SB from r1
   po += 4;

   //MDFN_en32lsb<false>(po, (0x08 << 26) | (8 << 21) | (8 << 16) | 0x0001);  // Increment source addr
   po += 4;

   //MDFN_en32lsb<false>(po, (0x05 << 26) | (0 << 21) | (10 << 16) | (-5 & 0xFFFF));
   po += 4;
   //MDFN_en32lsb<false>(po, (0x08 << 26) | (9 << 21) | (9 << 16) | 0x0001);  // Increment dest addr
   po += 4;

   //
   // Loop end
   //

   // Load SP into r29
   if(ignore_pcsp)
   {
      po += 16;
   }
   else
   {
      //MDFN_en32lsb<false>(po, (0xF << 26) | (0 << 21) | (1 << 16)  | (SP >> 16)); // LUI
      po += 4;
      //MDFN_en32lsb<false>(po, (0xD << 26) | (1 << 21) | (29 << 16) | (SP & 0xFFFF));    // ORI
      po += 4;

      // Load PC into r2
      //MDFN_en32lsb<false>(po, (0xF << 26) | (0 << 21) | (1 << 16)  | ((PC >> 16) | 0x8000));      // LUI
      po += 4;
      //MDFN_en32lsb<false>(po, (0xD << 26) | (1 << 21) | (2 << 16) | (PC & 0xFFFF));   // ORI
      po += 4;
   }

   // Half-assed instruction cache flush. ;)
   for(unsigned i = 0; i < 1024; i++)
   {
      //MDFN_en32lsb<false>(po, 0);
      po += 4;
   }



   // Jump to r2
   //MDFN_en32lsb<false>(po, (0x0 << 26) | (2 << 21) | (0x8 << 0));  // JR
   po += 4;
   //MDFN_en32lsb<false>(po, 0); // NOP(kinda)
   po += 4;

#ifdef HAVE_LIGHTREC
   /* Reload Expansion1 copy */
   PSX_LoadExpansion1();
#endif

   return true;
}

static int Load(const char *name, RFILE *fp)
{
   int64_t size     = filestream_get_size(fp);
   const bool IsPSF = false;
   char image_label[4096];

   image_label[0] = '\0';

   if(!TestMagic(name, fp, size))
   {
      //MDFN_Error(0, "File format is unknown to module psx..");
      return -1;
   }

   InitCommon(NULL, !IsPSF, true);

   TextMem.resize(0);

   if(size >= 0x800)
   {
      int64_t len     = size;
      uint8_t *header = (uint8_t*)malloc(len * sizeof(uint8_t));

      filestream_read_file(name, (void**)&header, &len);

      if (!LoadEXE(header, len))
         return -1;

      free(header);
   }

   disk_control_ext_info.image_paths.push_back(name);
   extract_basename(image_label, name, sizeof(image_label));
   disk_control_ext_info.image_labels.push_back(image_label);

   return(1);
}

static int LoadCD(std::vector<char *> *_CDInterfaces)
{
   InitCommon(_CDInterfaces);

   if (psx_skipbios == 1)
   //BIOSROM->WriteU32(0x6990, 0);

   //EmulatedPSX.GameType = GMT_CDROM;

   return(1);
}

static void Cleanup(void)
{
   TextMem.resize(0);

   // if(PSX_CDC)
   //    delete PSX_CDC;
   // PSX_CDC = NULL;

   // if(PSX_SPU)
   //    delete PSX_SPU;
   // PSX_SPU = NULL;

   // GPU_Destroy();

   // if(PSX_CPU)
   //    delete PSX_CPU;
   // PSX_CPU = NULL;

   // if(PSX_FIO)
   //    delete PSX_FIO;
   // PSX_FIO = NULL;
   // input_set_fio(NULL);

   // DMA_Kill();

#ifdef HAVE_LIGHTREC
   MainRAM = NULL;
   ScratchRAM = NULL;
   BIOSROM = NULL;
   if(psx_mmap > 0)
      lightrec_free_mmap();
#else
   // if(MainRAM)
   //    delete MainRAM;
   // MainRAM = NULL;

   // if(ScratchRAM)
   //    delete ScratchRAM;
   // ScratchRAM = NULL;

   // if(BIOSROM)
   //    delete BIOSROM;
   // BIOSROM = NULL;
#endif

   // if(PIOMem)
   //    delete PIOMem;
   // PIOMem = NULL;

   cdifs = NULL;
}

static void CloseGame(void)
{
   int i;

   for (i = 0; i < 8; i++)
   {
      if (i == 0 && !use_mednafen_memcard0_method)
      {
         //PSX_FIO->SaveMemcard(i);
         continue;
      }

      // If there's an error saving one memcard, don't skip trying to save the other, since it might succeed and
      // we can reduce potential data loss!
      try
      {
         char ext[64];
         const char *memcard = NULL;
         if (i == 0)
            snprintf(ext, sizeof(ext), "%d.mcr", memcard_left_index);
         else if (i == 1)
            snprintf(ext, sizeof(ext), "%d.mcr", memcard_right_index);
         else
            snprintf(ext, sizeof(ext), "%d.mcr", i);
         //memcard = MDFN_MakeFName(MDFNMKF_SAV, 0, ext);
         //PSX_FIO->SaveMemcard(i, memcard);
      }
      catch(std::exception &e)
      {
      }
   }

   Cleanup();
}

static void CDInsertEject(void)
{
   CD_TrayOpen = !CD_TrayOpen;

   for(unsigned disc = 0; disc < cdifs->size(); disc++)
   {
      // if(!(*cdifs)[disc]->Eject(CD_TrayOpen))
      //    CD_TrayOpen = !CD_TrayOpen;
   }

   SetDiscWrapper(CD_TrayOpen);
}

static void CDEject(void)
{
   if(!CD_TrayOpen)
      CDInsertEject();
}

static void CDSelect(void)
{
   if(cdifs && CD_TrayOpen)
   {
      int disc_count = (CD_IsPBP ? PBP_PhysicalDiscCount : (int)cdifs->size());

      CD_SelectedDisc = (CD_SelectedDisc + 1) % (disc_count + 1);

      if(CD_SelectedDisc == disc_count)
         CD_SelectedDisc = -1;
   }
}

extern "C" int StateAction(char *sm, int load, int data_only)
{
   // SFORMAT StateRegs[] =
   // {
   //    SFVAR(CD_TrayOpen),
   //    SFVAR(CD_SelectedDisc),
   //    SFARRAYN(MainRAM->data8, 1024 * 2048, "MainRAM.data8"),
   //    SFARRAY32(SysControl.Regs, 9),
   //    SFVAR(PSX_PRNG.lcgo),
   //    SFVAR(PSX_PRNG.x),
   //    SFVAR(PSX_PRNG.y),
   //    SFVAR(PSX_PRNG.z),
   //    SFVAR(PSX_PRNG.c),
   //    SFEND
   // };


   int ret;// = MDFNSS_StateAction(sm, load, data_only, StateRegs, "MAIN");

   // Call SetDisc() BEFORE we load CDC state, since SetDisc() has emulation side effects.  We might want to clean this up in the future.
   if(load)
   {
      if(CD_IsPBP)
      {
         if(!cdifs || CD_SelectedDisc >= PBP_PhysicalDiscCount)
            CD_SelectedDisc = -1;

         CDEject();
         CDInsertEject();
      } else {
         if(!cdifs || CD_SelectedDisc >= (int)cdifs->size())
            CD_SelectedDisc = -1;

         SetDiscWrapper(CD_TrayOpen);
      }
   }

   // TODO: Remember to increment dirty count in memory card state loading routine.

   //ret &= PSX_CPU->StateAction(sm, load, data_only);
   //ret &= DMA_StateAction(sm, load, data_only);
   //ret &= TIMER_StateAction(sm, load, data_only);
   //ret &= SIO_StateAction(sm, load, data_only);

   //ret &= PSX_CDC->StateAction(sm, load, data_only);
   //ret &= MDEC_StateAction(sm, load, data_only);
   //ret &= GPU_StateAction(sm, load, data_only);
   //ret &= PSX_SPU->StateAction(sm, load, data_only);

   //ret &= PSX_FIO->StateAction(sm, load, data_only);

   //ret &= IRQ_StateAction(sm, load, data_only); // Do it last.

   if(load)
   {
      ForceEventUpdates(0); // FIXME to work with debugger step mode.
   }

   return(ret);
}

static void DoSimpleCommand(int cmd)
{
   switch(cmd)
   {
      // case MDFN_MSC_RESET:
      //    PSX_Power();
      //    break;
      // case MDFN_MSC_POWER:
      //    PSX_Power();
      //    break;
      // case MDFN_MSC_INSERT_DISK:
      //    CDInsertEject();
      //    break;
      // case MDFN_MSC_SELECT_DISK:
      //    CDSelect();
      //    break;
      // case MDFN_MSC_EJECT_DISK:
      //    CDEject();
      //    break;
   }
}

static void GSCondCode(char* patch, const char* cc, const unsigned len, const uint32_t addr, const uint16_t val)
{
   // char tmp[256];

   // if(patch->conditions.size() > 0)
   //    patch->conditions.append(", ");

   // if(len == 2)
   //    snprintf(tmp, 256, "%u L 0x%08x %s 0x%04x", len, addr, cc, val & 0xFFFFU);
   // else
   //    snprintf(tmp, 256, "%u L 0x%08x %s 0x%02x", len, addr, cc, val & 0xFFU);

   // patch->conditions.append(tmp);
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

//    patch->bigendian = false;
//    patch->compare = 0;

//    if(patch->type == 'T')
//    {
//       if(code_type != 0x80)
//       log_cb(RETRO_LOG_ERROR, "Unrecognized GameShark code type for second part to copy bytes code.\n");

//       patch->addr = cl >> 16;
//       return(false);
//    }

//    switch(code_type)
//    {
//    default:
//       log_cb(RETRO_LOG_ERROR, "GameShark code type 0x%02X is currently not supported.\n", code_type);
//       return(false);

//    // TODO:
//    case 0x10:   // 16-bit increment
//       patch->length = 2;
//       patch->type = 'A';
//       patch->addr = cl >> 16;
//       patch->val = cl & 0xFFFF;
//       return(false);

//    case 0x11:   // 16-bit decrement
//       patch->length = 2;
//       patch->type = 'A';
//       patch->addr = cl >> 16;
//       patch->val = (0 - cl) & 0xFFFF;
//       return(false);

//    case 0x20:   // 8-bit increment
//       patch->length = 1;
//       patch->type = 'A';
//       patch->addr = cl >> 16;
//       patch->val = cl & 0xFF;
//       return(false);

//    case 0x21:   // 8-bit decrement
//       patch->length = 1;
//       patch->type = 'A';
//       patch->addr = cl >> 16;
//       patch->val = (0 - cl) & 0xFF;
//       return(false);
//    //
//    //
//    //

//    case 0x30:   // 8-bit constant
//       patch->length = 1;
//       patch->type = 'R';
//       patch->addr = cl >> 16;
//       patch->val = cl & 0xFF;
//       return(false);

//    case 0x80:   // 16-bit constant
//       patch->length = 2;
//       patch->type = 'R';
//       patch->addr = cl >> 16;
//       patch->val = cl & 0xFFFF;
//       return(false);

//    case 0x50:   // Repeat thingy
//    {
//       const uint8_t wcount = (cl >> 24) & 0xFF;
//       const uint8_t addr_inc = (cl >> 16) & 0xFF;
//       const uint8_t val_inc = (cl >> 0) & 0xFF;

//       patch->mltpl_count = wcount;
//       patch->mltpl_addr_inc = addr_inc;
//       patch->mltpl_val_inc = val_inc;
//    }
//    return(true);

//    case 0xC2:   // Copy
//    {
//       const uint16_t ccount = cl & 0xFFFF;

//       patch->type = 'T';
//       patch->val = 0;
//       patch->length = 1;

//       patch->copy_src_addr = cl >> 16;
//       patch->copy_src_addr_inc = 1;

//       patch->mltpl_count = ccount;
//       patch->mltpl_addr_inc = 1;
//       patch->mltpl_val_inc = 0;
//    }
//    return(true);

//   case 0xD0:   // 16-bit == condition
//    GSCondCode(patch, "==", 2, cl >> 16, cl);
//    return(true);

//   case 0xD1:   // 16-bit != condition
//    GSCondCode(patch, "!=", 2, cl >> 16, cl);
//    return(true);

//   case 0xD2:   // 16-bit < condition
//    GSCondCode(patch, "<", 2, cl >> 16, cl);
//    return(true);

//   case 0xD3:   // 16-bit > condition
//    GSCondCode(patch, ">", 2, cl >> 16, cl);
//    return(true);



//   case 0xE0:   // 8-bit == condition
//    GSCondCode(patch, "==", 1, cl >> 16, cl);
//    return(true);

//   case 0xE1:   // 8-bit != condition
//    GSCondCode(patch, "!=", 1, cl >> 16, cl);
//    return(true);

//   case 0xE2:   // 8-bit < condition
//    GSCondCode(patch, "<", 1, cl >> 16, cl);
//    return(true);

//   case 0xE3:   // 8-bit > condition
//    GSCondCode(patch, ">", 1, cl >> 16, cl);
//    return(true);

//  }
}


/* end of Mednafen psx.cpp */











































//forward decls
//extern void Emulate(EmulateSpecStruct *espec);

static bool overscan;
static double last_sound_rate;

#ifdef NEED_DEINTERLACER
static bool PrevInterlaced;
static Deinterlacer deint;
#endif

//static MDFN_Surface *surf = NULL;

static void alloc_surface(void)
{
   //MDFN_PixelFormat pix_fmt(MDFN_COLORSPACE_RGB, 16, 8, 0, 24);
   uint32_t width;  //= MEDNAFEN_CORE_GEOMETRY_MAX_W;
   uint32_t height; //= content_is_pal ? MEDNAFEN_CORE_GEOMETRY_MAX_H : 480;

   //width  <<= GPU_get_upscale_shift();
   //height <<= GPU_get_upscale_shift();

   // if (surf != NULL)
   //    delete surf;

   // surf = new MDFN_Surface(NULL, width, height, width, pix_fmt);
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

static bool disk_get_eject_state(void)
{
   return eject_state;
}

static unsigned disk_get_image_index(void)
{
   // PSX global. Hacky.
   return CD_SelectedDisc;
}

static bool disk_set_image_index(unsigned index)
{
   CD_SelectedDisc = index;
   if (CD_SelectedDisc > disk_get_num_images())
      CD_SelectedDisc = disk_get_num_images();

   // Very hacky. CDSelect command will increment first.
   CD_SelectedDisc--;

   //DoSimpleCommand(MDFN_MSC_SELECT_DISK);
   return true;
}

static bool disk_add_image_index(void)
{
   if(CD_IsPBP)
      return false;

   cdifs->push_back(NULL);
   disk_control_ext_info.image_paths.push_back("");
   disk_control_ext_info.image_labels.push_back("");
   return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
	if (string_is_empty(path))
		return false;

	disk_control_ext_info.initial_index = index;
	disk_control_ext_info.initial_path  = path;

	return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
	if (len < 1)
		return false;

	if ((index < disk_get_num_images()) &&
		 (index < disk_control_ext_info.image_paths.size()))
	{
		if (!string_is_empty(disk_control_ext_info.image_paths[index].c_str()))
		{
			strlcpy(path, disk_control_ext_info.image_paths[index].c_str(), len);
			return true;
		}
	}

	return false;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
	if (len < 1)
		return false;

	if ((index < disk_get_num_images()) &&
		 (index < disk_control_ext_info.image_labels.size()))
	{
		if (!string_is_empty(disk_control_ext_info.image_labels[index].c_str()))
		{
			strlcpy(label, disk_control_ext_info.image_labels[index].c_str(), len);
			return true;
		}
	}

	return false;
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
   uint64_t serialization_quirks = RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
   unsigned dci_version          = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;

   const char *dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s", dir);
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      // If save directory is defined use it, otherwise use system directory
      if (dir)
         snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", dir);
      else
         snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }
   else
   {
      /* TODO: Add proper fallback */
      log_cb(RETRO_LOG_WARN, "Save directory is not defined. Fallback on using SYSTEM directory ...\n");
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }

   check_system_specs();
}

void retro_reset(void)
{
   //DoSimpleCommand(MDFN_MSC_RESET);
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

#ifdef EMSCRIPTEN
static bool cdimagecache = true;
#else
static bool cdimagecache = false;
#endif

static bool boot = true;

// shared memory cards support
static bool shared_memorycards = false;

static bool has_new_geometry = false;
static bool has_new_timing = false;

uint8_t analog_combo[2] = {0};
uint8_t analog_combo_hold = 0;

extern void PSXDitherApply(bool);

static void check_variables(bool startup)
{
   struct retro_variable var = {0};

#ifndef EMSCRIPTEN
   var.key = BEETLE_OPT(cd_access_method);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "sync") == 0)
      {
         cdimagecache = false;
         cd_async = false;
      }
      else if (strcmp(var.value, "async") == 0)
      {
         cdimagecache = false;
         cd_async = true;
      }
      else if (strcmp(var.value, "precache") == 0)
      {
         cdimagecache = true;
         cd_async = false;
      }
   }
#endif

#ifdef HAVE_LIGHTREC
   var.key = BEETLE_OPT(cpu_dynarec);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "execute") == 0)
         psx_dynarec = DYNAREC_EXECUTE;
      else if (strcmp(var.value, "execute_one") == 0)
         psx_dynarec = DYNAREC_EXECUTE_ONE;
      else if (strcmp(var.value, "run_interpreter") == 0)
         psx_dynarec = DYNAREC_RUN_INTERPRETER;
      else
         psx_dynarec = DYNAREC_DISABLED;
   }
   else
      psx_dynarec = DYNAREC_DISABLED;

   var.key = BEETLE_OPT(dynarec_invalidate);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "full") == 0)
         psx_dynarec_invalidate = false;
      else if (strcmp(var.value, "dma") == 0)
         psx_dynarec_invalidate = true;
   }
   else
      psx_dynarec_invalidate = false;

   var.key = BEETLE_OPT(dynarec_eventcycles);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
	EventCycles = atoi(var.value);
   }
   else
      EventCycles = 128;

   var.key = BEETLE_OPT(dynarec_spu_samples);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
	spu_samples = atoi(var.value);
   }
   else
      spu_samples = 1;
#endif

   var.key = BEETLE_OPT(cpu_freq_scale);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int scale_percent = atoi(var.value);

      if (scale_percent == 100)
         psx_overclock_factor = 0;
      else
         psx_overclock_factor = ((scale_percent /*<< OVERCLOCK_SHIFT*/) + 50) / 100;
   }
   else
      psx_overclock_factor = 0;

   // Need to adjust the CPU<->GPU frequency ratio if the overclocking changes
   //GPU_RecalcClockRatio();

   var.key = BEETLE_OPT(gte_overclock);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         psx_gte_overclock = true;
      else if (strcmp(var.value, "disabled") == 0)
         psx_gte_overclock = false;
   }
   else
      psx_gte_overclock = false;

   var.key = BEETLE_OPT(gpu_overclock);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         unsigned val = atoi(var.value);

         // Upscale must be a power of two
         assert((val & (val - 1)) == 0);

         // Crappy "ffs" implementation since the standard function is not
         // widely supported by libc in the wild
         uint8_t n;
         for (n = 0; (val & 1) == 0; ++n)
            {
               val >>= 1;
            }
         psx_gpu_overclock_shift = n;
      }
   else
      psx_gpu_overclock_shift = 0;

   var.key = BEETLE_OPT(skip_bios);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         psx_skipbios = 1;
      else
         psx_skipbios = 0;
   }

   var.key = BEETLE_OPT(override_bios);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
      {
         override_bios = 0;
      }
      else if (!strcmp(var.value, "psxonpsp"))
	  {
         override_bios = 1;
      }
      else if (!strcmp(var.value, "ps1_rom"))
      {
         override_bios = 2;
      }
   }

   var.key = BEETLE_OPT(widescreen_hack);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
      {
         if (widescreen_hack == false)
            has_new_geometry = true;
         widescreen_hack = true;
      }
      else if (strcmp(var.value, "disabled") == 0)
      {
         if (widescreen_hack == true)
            has_new_geometry = true;
         widescreen_hack = false;
      }
   }
   else
   {
      if (widescreen_hack == true)
         has_new_geometry = true;
      widescreen_hack = false;
   }

   var.key = BEETLE_OPT(widescreen_hack_aspect_ratio);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "16:10"))
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 0)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 0;
      }
      else if (!strcmp(var.value, "16:9"))
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 1)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 1;
      }
      else if (!strcmp(var.value, "18:9"))
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 2)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 2;
      }
      else if (!strcmp(var.value, "19:9"))
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 3)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 3;
      }
      else if (!strcmp(var.value, "20:9"))
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 4)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 4;
      }
      else if (!strcmp(var.value, "21:9")) // 64:27
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 5)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 5;
      }
      else if (!strcmp(var.value, "32:9"))
      {
         if (!startup && widescreen_hack_aspect_ratio_setting != 6)
            has_new_geometry = true;
         widescreen_hack_aspect_ratio_setting = 6;
      }
   }
   else
   {
      if (!startup && widescreen_hack_aspect_ratio_setting != 1)
         has_new_geometry = true;
      widescreen_hack_aspect_ratio_setting = 1;
   }

   var.key = BEETLE_OPT(pal_video_timing_override);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      bool want_fast_pal = (strcmp(var.value, "enabled") == 0);

      if (want_fast_pal != fast_pal) {
         fast_pal = want_fast_pal;
         has_new_timing = true;
      }
   }

   var.key = BEETLE_OPT(analog_calibration);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "enabled") == 0)
   //       input_enable_calibration(true);
   //    else if (strcmp(var.value, "disabled") == 0)
   //       input_enable_calibration(false);
   // }
   // else
   //    input_enable_calibration(false);

   var.key = BEETLE_OPT(core_timing_fps);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "force_progressive") == 0)
      {
         if (!startup && core_timing_fps_mode != FORCE_PROGRESSIVE_TIMING)
            has_new_timing = true;

         core_timing_fps_mode = FORCE_PROGRESSIVE_TIMING;
      }
      else if (strcmp(var.value, "force_interlaced") == 0)
      {
         if (!startup && core_timing_fps_mode != FORCE_INTERLACED_TIMING)
            has_new_timing = true;

         core_timing_fps_mode = FORCE_INTERLACED_TIMING;
      }
      else // auto toggle setting, timing changes are allowed
      {
         if (!startup && core_timing_fps_mode != AUTO_TOGGLE_TIMING)
            has_new_timing = true;

         core_timing_fps_mode = AUTO_TOGGLE_TIMING;
      }
   }

   var.key = BEETLE_OPT(aspect_ratio);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "corrected"))
      {
         if (!startup && aspect_ratio_setting != 0)
            has_new_geometry = true;
         aspect_ratio_setting = 0;
      }
      else if (!strcmp(var.value, "uncorrected"))
      {
         if (!startup && aspect_ratio_setting != 1)
            has_new_geometry = true;
         aspect_ratio_setting = 1;
      }
      else if (!strcmp(var.value, "4:3"))
      {
         if (!startup && aspect_ratio_setting != 2)
            has_new_geometry = true;
         aspect_ratio_setting = 2;
      }
      else if (!strcmp(var.value, "ntsc"))
      {
         if (!startup && aspect_ratio_setting != 3)
            has_new_geometry = true;
         aspect_ratio_setting = 3;
      }
   }

   if (startup)
   {
      bool hw_renderer = false;

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES) || defined(HAVE_VULKAN)
      var.key = BEETLE_OPT(renderer);
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "hardware") || !strcmp(var.value, "hardware_gl") || !strcmp(var.value, "hardware_vk"))
         {
            hw_renderer = true;
         }
      }
#endif

      var.key = BEETLE_OPT(internal_resolution);
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         uint8_t new_upscale_shift;
         uint8_t val = atoi(var.value);

         // Upscale must be a power of two
         assert((val & (val - 1)) == 0);

         // Crappy "ffs" implementation since the standard function is not
         // widely supported by libc in the wild
         for (new_upscale_shift = 0; (val & 1) == 0; ++new_upscale_shift)
            val >>= 1;
         psx_gpu_upscale_shift_hw = new_upscale_shift;
      }
      else
         psx_gpu_upscale_shift_hw = 0;
      
      if (hw_renderer)
         psx_gpu_upscale_shift = 0;
      else
         psx_gpu_upscale_shift = psx_gpu_upscale_shift_hw;
   }
   else
   {
      //rsx_intf_refresh_variables();

      var.key = BEETLE_OPT(internal_resolution);
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         uint8_t new_upscale_shift;
         uint8_t val = atoi(var.value);

         // Upscale must be a power of two
         assert((val & (val - 1)) == 0);

         // Crappy "ffs" implementation since the standard function is not
         // widely supported by libc in the wild
         for (new_upscale_shift = 0; (val & 1) == 0; ++new_upscale_shift)
            val >>= 1;
         psx_gpu_upscale_shift_hw = new_upscale_shift;
      }
      else
         psx_gpu_upscale_shift_hw = 0;

      // switch (rsx_intf_is_type())
      // {
      //    case RSX_SOFTWARE:
      //       psx_gpu_upscale_shift = psx_gpu_upscale_shift_hw;
      //       break;
      //    case RSX_OPENGL:
      //    case RSX_VULKAN:
      //       psx_gpu_upscale_shift = 0;
      //       break;
      // }
   }

   var.key = BEETLE_OPT(dither_mode);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "1x(native)") == 0)
   //       psx_gpu_dither_mode = DITHER_NATIVE;
   //    else if (strcmp(var.value, "internal resolution") == 0)
   //       psx_gpu_dither_mode = DITHER_UPSCALED;
   //    else if (strcmp(var.value, "disabled") == 0)
   //       psx_gpu_dither_mode = DITHER_OFF;
   // }
   // else
   //    psx_gpu_dither_mode = DITHER_NATIVE;

   // // iCB: PGXP settings
   // var.key = BEETLE_OPT(pgxp_mode);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "disabled") == 0)
   //       psx_pgxp_mode = PGXP_MODE_NONE;
   //    else if (strcmp(var.value, "memory only") == 0)
   //       psx_pgxp_mode = PGXP_MODE_MEMORY | PGXP_MODE_GTE;
   //    else if (strcmp(var.value, "memory + CPU") == 0)
   //       psx_pgxp_mode = PGXP_MODE_MEMORY | PGXP_MODE_GTE | PGXP_MODE_CPU;
   // }
   // else
   //    psx_pgxp_mode = PGXP_MODE_NONE;

   var.key = BEETLE_OPT(pgxp_2d_tol);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         psx_pgxp_2d_tol = -1;
      else
         psx_pgxp_2d_tol = atoi(var.value);
   }
   else
      psx_pgxp_2d_tol = -1;

   var.key = BEETLE_OPT(pgxp_vertex);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "disabled") == 0)
   //       psx_pgxp_vertex_caching = PGXP_MODE_NONE;
   //    else if (strcmp(var.value, "enabled") == 0)
   //       psx_pgxp_vertex_caching = PGXP_VERTEX_CACHE;
   // }
   // else
   //    psx_pgxp_vertex_caching = PGXP_MODE_NONE;

   // var.key = BEETLE_OPT(pgxp_texture);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "disabled") == 0)
   //       psx_pgxp_texture_correction = PGXP_MODE_NONE;
   //    else if (strcmp(var.value, "enabled") == 0)
   //       psx_pgxp_texture_correction = PGXP_TEXTURE_CORRECTION;
   // }
   // else
   //    psx_pgxp_texture_correction = PGXP_MODE_NONE;
   // // \iCB

   // var.key = BEETLE_OPT(pgxp_nclip);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "disabled") == 0)
   //       psx_pgxp_nclip = PGXP_MODE_NONE;
   //    else if (strcmp(var.value, "enabled") == 0)
   //       psx_pgxp_nclip = PGXP_NCLIP_IMPL;
   // }
   // else
   //    psx_pgxp_nclip = PGXP_MODE_NONE;

   var.key = BEETLE_OPT(line_render);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         line_render_mode = 0;
      else if (strcmp(var.value, "default") == 0)
         line_render_mode = 1;
      else if (strcmp(var.value, "aggressive") == 0)
         line_render_mode = 2;
   }

   var.key = BEETLE_OPT(filter);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int old_filter_mode = filter_mode;
      if (strcmp(var.value, "nearest") == 0)
         filter_mode = 0;
      else if (strcmp(var.value, "xBR") == 0)
         filter_mode = 1;
      else if (strcmp(var.value, "SABR") == 0)
         filter_mode = 2;
      else if (strcmp(var.value, "bilinear") == 0)
         filter_mode = 3;
      else if (strcmp(var.value, "3-point") == 0)
         filter_mode = 4;
      else if (strcmp(var.value, "JINC2") == 0)
         filter_mode = 5;

      if(filter_mode != old_filter_mode)
      {
         opaque_check = true;
         semitrans_check = true;
         old_filter_mode = filter_mode;
      }
   }

   var.key = BEETLE_OPT(analog_toggle);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if ((strcmp(var.value, "disabled") == 0)
            /*&& setting_psx_analog_toggle*/)
      {
         //setting_psx_analog_toggle = 0;
         setting_apply_analog_toggle = true;
         setting_apply_analog_default = false;
      }
      else if ((strcmp(var.value, "enabled") == 0)
            && (/*!setting_psx_analog_toggle ||*/ setting_apply_analog_default))
      {
         //setting_psx_analog_toggle = 1;
         setting_apply_analog_toggle = true;
         setting_apply_analog_default = false;
      }
      else if ((strcmp(var.value, "enabled-analog") == 0)
            && (/*!setting_psx_analog_toggle ||*/ !setting_apply_analog_default))
      {
         //setting_psx_analog_toggle = 1;
         setting_apply_analog_toggle = true;
         setting_apply_analog_default = true;
      }

      /* No need to apply if going to do it in InitCommon */
      if (startup)
         setting_apply_analog_toggle = false;
   }

   var.key = BEETLE_OPT(analog_toggle_combo);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "l1+l2+r1+r2+start+select") == 0)
      {
         analog_combo[0] = 0x09;
         analog_combo[1] = 0x0f;
      }
      else if (strcmp(var.value, "l1+r1+select") == 0)
      {
         analog_combo[0] = 0x01;
         analog_combo[1] = 0x0c;
      }
      else if (strcmp(var.value, "l1+r1+start") == 0)
      {
         analog_combo[0] = 0x08;
         analog_combo[1] = 0x0c;
      }
      else if (strcmp(var.value, "l1+r1+l3") == 0)
      {
         analog_combo[0] = 0x02;
         analog_combo[1] = 0x0c;
      }
      else if (strcmp(var.value, "l1+r1+r3") == 0)
      {
         analog_combo[0] = 0x04;
         analog_combo[1] = 0x0c;
      }
      else if (strcmp(var.value, "l2+r2+select") == 0)
      {
         analog_combo[0] = 0x01;
         analog_combo[1] = 0x03;
      }
      else if (strcmp(var.value, "l2+r2+start") == 0)
      {
         analog_combo[0] = 0x08;
         analog_combo[1] = 0x03;
      }
      else if (strcmp(var.value, "l2+r2+l3") == 0)
      {
         analog_combo[0] = 0x02;
         analog_combo[1] = 0x03;
      }
      else if (strcmp(var.value, "l2+r2+r3") == 0)
      {
         analog_combo[0] = 0x04;
         analog_combo[1] = 0x03;
      }
      else if (strcmp(var.value, "l3+r3") == 0)
      {
         analog_combo[0] = 0x06;
         analog_combo[1] = 0x00;
      }
   }

   var.key = BEETLE_OPT(analog_toggle_hold);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      analog_combo_hold = atoi(var.value);
   }

   var.key = BEETLE_OPT(crosshair_color_p1);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "red") == 0)
         setting_crosshair_color_p1 = 0xFF0000;
      else if (strcmp(var.value, "blue") == 0)
         setting_crosshair_color_p1 = 0x0080FF;
      else if (strcmp(var.value, "green") == 0)
         setting_crosshair_color_p1 = 0x00FF00;
      else if (strcmp(var.value, "orange") == 0)
         setting_crosshair_color_p1 = 0xFF8000;
      else if (strcmp(var.value, "yellow") == 0)
         setting_crosshair_color_p1 = 0xFFFF00;
      else if (strcmp(var.value, "cyan") == 0)
         setting_crosshair_color_p1 = 0x00FFFF;
      else if (strcmp(var.value, "pink") == 0)
         setting_crosshair_color_p1 = 0xFF00FF;
      else if (strcmp(var.value, "purple") == 0)
         setting_crosshair_color_p1 = 0x8000FF;
      else if (strcmp(var.value, "black") == 0)
         setting_crosshair_color_p1 = 0x000000;
      else if (strcmp(var.value, "white") == 0)
         setting_crosshair_color_p1 = 0xFFFFFF;
   }

   var.key = BEETLE_OPT(crosshair_color_p2);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "red") == 0)
         setting_crosshair_color_p2 = 0xFF0000;
      else if (strcmp(var.value, "blue") == 0)
         setting_crosshair_color_p2 = 0x0080FF;
      else if (strcmp(var.value, "green") == 0)
         setting_crosshair_color_p2 = 0x00FF00;
      else if (strcmp(var.value, "orange") == 0)
         setting_crosshair_color_p2 = 0xFF8000;
      else if (strcmp(var.value, "yellow") == 0)
         setting_crosshair_color_p2 = 0xFFFF00;
      else if (strcmp(var.value, "cyan") == 0)
         setting_crosshair_color_p2 = 0x00FFFF;
      else if (strcmp(var.value, "pink") == 0)
         setting_crosshair_color_p2 = 0xFF00FF;
      else if (strcmp(var.value, "purple") == 0)
         setting_crosshair_color_p2 = 0x8000FF;
      else if (strcmp(var.value, "black") == 0)
         setting_crosshair_color_p2 = 0x000000;
      else if (strcmp(var.value, "white") == 0)
         setting_crosshair_color_p2 = 0xFFFFFF;
   }

   var.key = BEETLE_OPT(enable_multitap_port1);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "enabled") == 0)
   //       setting_psx_multitap_port_1 = true;
   //    else if (strcmp(var.value, "disabled") == 0)
   //       setting_psx_multitap_port_1 = false;
   // }

   // var.key = BEETLE_OPT(enable_multitap_port2);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "enabled") == 0)
   //       setting_psx_multitap_port_2 = true;
   //    else if (strcmp(var.value, "disabled") == 0)
   //       setting_psx_multitap_port_2 = false;
   // }

   // var.key = BEETLE_OPT(mouse_sensitivity);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   //    input_set_mouse_sensitivity(atoi(var.value));

   // var.key = BEETLE_OPT(gun_cursor);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "off") == 0)
   //       input_set_gun_cursor(FrontIO::SETTING_GUN_CROSSHAIR_OFF);
   //    else if (strcmp(var.value, "cross") == 0)
   //       input_set_gun_cursor(FrontIO::SETTING_GUN_CROSSHAIR_CROSS);
   //    else if (strcmp(var.value, "dot") == 0)
   //       input_set_gun_cursor(FrontIO::SETTING_GUN_CROSSHAIR_DOT);
   // }

   // var.key = BEETLE_OPT(gun_input_mode);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "lightgun") == 0)
   //       gun_input_mode = SETTING_GUN_INPUT_LIGHTGUN;
   //    else if (strcmp(var.value, "touchscreen") == 0)
   //       gun_input_mode = SETTING_GUN_INPUT_POINTER;
   // }
   // else
   //    gun_input_mode = SETTING_GUN_INPUT_LIGHTGUN;

   // var.key = BEETLE_OPT(negcon_deadzone);
   // input_set_negcon_deadzone(0);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    input_set_negcon_deadzone((int)(atoi(var.value) * 0.01f * NEGCON_RANGE));
   // }

   // var.key = BEETLE_OPT(negcon_response);
   // input_set_negcon_linearity(1);
   // if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   // {
   //    if (strcmp(var.value, "quadratic") == 0)
   //       input_set_negcon_linearity(2);
   //    else if (strcmp(var.value, "cubic") == 0)
   //       input_set_negcon_linearity(3);
   // }

   // Initial scanline NTSC
   var.key = BEETLE_OPT(initial_scanline);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int new_scanline_value = atoi(var.value);
      if (setting_initial_scanline != new_scanline_value)
      {
         has_new_geometry = true;
         setting_initial_scanline = new_scanline_value;
      }
   }

   // Last scanline NTSC
   var.key = BEETLE_OPT(last_scanline);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int new_scanline_value = atoi(var.value);
      if (setting_last_scanline != new_scanline_value)
      {
         has_new_geometry = true;
         setting_last_scanline = new_scanline_value;
      }
   }

   // Initial scanline PAL
   var.key = BEETLE_OPT(initial_scanline_pal);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int new_scanline_value = atoi(var.value);
      if (setting_initial_scanline_pal != new_scanline_value)
      {
         has_new_geometry = true;
         setting_initial_scanline_pal = new_scanline_value;
      }
   }

   // Last scanline PAL
   var.key = BEETLE_OPT(last_scanline_pal);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int new_scanline_value = atoi(var.value);
      if (setting_last_scanline_pal != new_scanline_value)
      {
         has_new_geometry = true;
         setting_last_scanline_pal = new_scanline_value;
      }
   }

   // if(setting_psx_multitap_port_1 && setting_psx_multitap_port_2)
   //    input_set_player_count(8);
   // else if (setting_psx_multitap_port_1 || setting_psx_multitap_port_2)
   //    input_set_player_count(5);
   // else
   //    input_set_player_count(2);

   /* Memcards (startup only) */
   if (startup)
   {
      var.key = BEETLE_OPT(use_mednafen_memcard0_method);
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "libretro"))
            use_mednafen_memcard0_method = false;
         else if (!strcmp(var.value, "mednafen"))
            use_mednafen_memcard0_method = true;
      }

      var.key = BEETLE_OPT(enable_memcard1);
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "enabled"))
            enable_memcard1 = true;
         else if (!strcmp(var.value, "disabled"))
            enable_memcard1 = false;
      }

      var.key = BEETLE_OPT(shared_memory_cards);
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "enabled"))
         {
            // if(use_mednafen_memcard0_method)
               shared_memorycards = true;
            // else
               // MDFND_DispMessage(3, RETRO_LOG_WARN,
                     // RETRO_MESSAGE_TARGET_ALL, RETRO_MESSAGE_TYPE_NOTIFICATION,
                     // "Memory Card 0 Method not set to Mednafen; shared memory cards could not be enabled.");
         }
         else if (!strcmp(var.value, "disabled"))
         {
            shared_memorycards = false;
         }
      }
   }
   /* End Memcards */

   var.key = BEETLE_OPT(frame_duping);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
      {
         bool can_dupe = false;
         if (environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe))
            allow_frame_duping = can_dupe;
      }
      else if (strcmp(var.value, "disabled") == 0)
         allow_frame_duping = false;
   }
   else
      allow_frame_duping = false;

   var.key = BEETLE_OPT(display_internal_fps);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         display_internal_framerate = true;
      else if (strcmp(var.value, "disabled") == 0)
         display_internal_framerate = false;
   }
   else
      display_internal_framerate = false;

   var.key = BEETLE_OPT(display_osd);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         display_notifications = true;
      else if (strcmp(var.value, "disabled") == 0)
         display_notifications = false;
   }
   else
      display_notifications = true;

   var.key = BEETLE_OPT(crop_overscan);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int old_crop_overscan = crop_overscan;
      if (strcmp(var.value, "disabled") == 0)
         crop_overscan = 0;
      else if (strcmp(var.value, "static") == 0)
         crop_overscan = 1;
      else if (strcmp(var.value, "smart") == 0)
         crop_overscan = 2;

      if (crop_overscan != old_crop_overscan)
         has_new_geometry = true;
   }

   var.key = BEETLE_OPT(image_offset);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         image_offset = 0;
      else
         image_offset = atoi(var.value);
   }

   var.key = BEETLE_OPT(image_crop);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         image_crop = 0;
      else
         image_crop = atoi(var.value);
   }

   var.key = BEETLE_OPT(cd_fastload);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      uint8_t val = var.value[0] - '0';
      if (var.value[1] != 'x')
      {
         val  = (var.value[0] - '0') * 10;
         val += var.value[1] - '0';
      }
      // Value is a multiplier from the native 2x, so we divide by two
      cd_2x_speedup = val / 2;
   }
   else
      cd_2x_speedup = 1;

   var.key = BEETLE_OPT(memcard_left_index);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      memcard_left_index_old = memcard_left_index;
      memcard_left_index     = atoi(var.value);
   }

   var.key = BEETLE_OPT(memcard_right_index);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      memcard_right_index_old = memcard_right_index;
      memcard_right_index     = atoi(var.value);
   }

   var.key = BEETLE_OPT(deinterlacer);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      // if (strcmp(var.value, "bob") == 0)
      //    deint.SetType(Deinterlacer::DEINT_BOB);
      // else
      //    deint.SetType(Deinterlacer::DEINT_WEAVE);
   }
}

#ifdef NEED_CD
static void ReadM3U(std::vector<std::string> &file_list, std::string path, unsigned depth = 0)
{
   std::string dir_path;
   char linebuf[2048];
   RFILE *fp = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (fp == NULL)
      return;

   MDFN_GetFilePathComponents(path, &dir_path);

   while(filestream_gets(fp, linebuf, sizeof(linebuf)) != NULL)
   {
      std::string efp;

      if(linebuf[0] == '#')
         continue;
      string_trim_whitespace_right(linebuf);
      if(linebuf[0] == 0)
         continue;

      efp = MDFN_EvalFIP(dir_path, std::string(linebuf), false);

      if(efp.size() >= 4 && efp.substr(efp.size() - 4) == ".m3u")
      {
         if(efp == path)
         {
            log_cb(RETRO_LOG_ERROR, "M3U at \"%s\" references self.\n", efp.c_str());
            goto end;
         }

         if(depth == 99)
         {
            log_cb(RETRO_LOG_ERROR, "M3U load recursion too deep!\n");
            goto end;
         }

         ReadM3U(file_list, efp, depth++);
      }
      else
         file_list.push_back(efp);
   }

end:
   filestream_close(fp);
}

// TODO: LoadCommon()

static bool MDFNI_LoadCD(const char *devicename)
{
   log_cb(RETRO_LOG_INFO, "Loading \"%s\"\n", devicename);

   try
   {
      size_t devicename_len = strlen(devicename);
      if(devicename && devicename_len > 4 && !strcasecmp(devicename + devicename_len - 4, ".m3u"))
      {
         ReadM3U(disk_control_ext_info.image_paths, devicename);

         for(unsigned i = 0; i < disk_control_ext_info.image_paths.size(); i++)
         {
            char image_label[4096];
            bool success = true;

            image_label[0] = '\0';

            CDIF *image  = CDIF_Open(
                  &success, disk_control_ext_info.image_paths[i].c_str(), false, cdimagecache);
            CDInterfaces.push_back(image);

            extract_basename(
                  image_label, disk_control_ext_info.image_paths[i].c_str(),
                  sizeof(image_label));
            disk_control_ext_info.image_labels.push_back(image_label);
         }
      }
      else if(devicename && devicename_len > 4 && !strcasecmp(devicename + devicename_len - 4, ".pbp"))
      {
         bool success = true;
         CDIF *image  = CDIF_Open(&success, devicename, false, cdimagecache);
         CD_IsPBP     = true;
         CDInterfaces.push_back(image);

         /* CDIF_Open() sets PBP_DiscCount, so we can populate
          * image_paths/image_labels here */
         PBP_PhysicalDiscCount = (PBP_DiscCount == 0) ?
               1 : PBP_DiscCount;

         for(unsigned i = 0; i < PBP_PhysicalDiscCount; i++)
         {
            /* image_name is at most 4096 - 4 (removing ".pbp")
             * gives label room to add index and quiets gcc warnings */
            char image_name[4092];
            char image_label[4096];

            image_name[0]  = '\0';
            image_label[0] = '\0';

            /* All 'disks' have the same path when using
             * multi-disk PBP files */
            disk_control_ext_info.image_paths.push_back(devicename);

            /* Label is name+index */
            extract_basename(image_name, devicename, sizeof(image_name));
            snprintf(image_label, sizeof(image_label), "%s #%u", image_name, i + 1);
            disk_control_ext_info.image_labels.push_back(image_label);
         }
      }
      else
      {
         char image_label[4096];
         bool success = true;

         image_label[0] = '\0';

         bool cache = cdimagecache;
         /* don't precache if physical cdrom, will take way too long and be unresponive */
         if (cdimagecache && devicename && !strncasecmp(devicename, "cdrom:", 6)) {
            cache = false;
            log_cb(RETRO_LOG_INFO, "Skipping Pre-Cache due to using physical media: %s\n", devicename);
         }

         CDIF *image  = CDIF_Open(&success, devicename, false, cache);
         if (!success)
            return false;

         CDInterfaces.push_back(image);

         disk_control_ext_info.image_paths.push_back(devicename);
         extract_basename(image_label, devicename, sizeof(image_label));
         disk_control_ext_info.image_labels.push_back(image_label);
      }
   }
   catch(std::exception &e)
   {
      log_cb(RETRO_LOG_ERROR, "Error opening CD.\n");
      return false;
   }

#ifdef DEBUG
   // Print out a track list for all discs.
   for(unsigned i = 0; i < CDInterfaces.size(); i++)
   {
      TOC toc;
      TOC_Clear(&toc);

      CDInterfaces[i]->ReadTOC(&toc);

      log_cb(RETRO_LOG_DEBUG, "CD %d Layout:\n", i + 1);

      for(int32_t track = toc.first_track; track <= toc.last_track; track++)
      {
         log_cb(RETRO_LOG_DEBUG, "Track %2d, LBA: %6d  %s\n", track, toc.tracks[track].lba, (toc.tracks[track].control & 0x4) ? "DATA" : "AUDIO");
      }

      log_cb(RETRO_LOG_DEBUG, "Leadout: %6d\n", toc.tracks[100].lba);
   }
#endif

   if(!(LoadCD(&CDInterfaces)))
   {
      for(unsigned i = 0; i < CDInterfaces.size(); i++)
         delete CDInterfaces[i];
      CDInterfaces.clear();

      disk_control_ext_info.initial_index = 0;
      disk_control_ext_info.initial_path.clear();
      disk_control_ext_info.image_paths.clear();
      disk_control_ext_info.image_labels.clear();

      return false;
   }

   //MDFNI_SetLayerEnableMask(~0ULL);

   MDFN_LoadGameCheats(NULL);
   MDFNMP_InstallReadPatches();

   return true;
}
#endif

static bool MDFNI_LoadGame(const char *name)
{
   RFILE *GameFile = NULL;
   size_t name_len = strlen(name);

   if(name_len > 3 && (
      !strcasecmp(name + name_len - 3, "cue") ||
      !strcasecmp(name + name_len - 3, "ccd") ||
      !strcasecmp(name + name_len - 3, "toc") ||
      !strcasecmp(name + name_len - 3, "m3u") ||
      !strcasecmp(name + name_len - 3, "chd") ||
      !strcasecmp(name + name_len - 3, "pbp")
      ))
    return NULL;//MDFNI_LoadCD(name);

   GameFile = filestream_open(name,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if(!GameFile)
      goto error;

   if(Load(name, GameFile) <= 0)
      goto error;

   filestream_close(GameFile);
   GameFile   = NULL;

   return true;

error:
   if (GameFile)
      filestream_close(GameFile);
   GameFile     = NULL;

   return false;
}

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

    // 1. Sync dimensions
    wrapper.UpdateScreenSize(target_w, target_h);

    Game game = wrapper.getGame();
    wrapper.Inputs->Poll(&game, 0, 0, target_w, target_h);
    
    // 2. Run the emulator
    wrapper.Supermodel(game);

    // --- CRITICAL ADDITION HERE ---
    // After Supermodel finishes rendering, it might leave the Scissor 
    // or Viewport messed up for the Blit operation.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, target_w, target_h);
    // ------------------------------

    // 3. Get the Framebuffers
    GLuint ra_fbo = wrapper.getHwRender().get_current_framebuffer();
    GLuint sm_fbo = wrapper.getSuperAA()->GetTargetID();

    // 4. BLIT
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sm_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ra_fbo);

    glBlitFramebuffer(
        0, target_h, target_w, 0,      // Source (Flipped)
        0, 0, target_w, target_h,      // Destination
        GL_COLOR_BUFFER_BIT,
        GL_LINEAR                      // Use LINEAR for smoother resizing
    );

    glBindFramebuffer(GL_FRAMEBUFFER, ra_fbo); 
    video_cb(RETRO_HW_FRAME_BUFFER_VALID, target_w, target_h, 0);
}

void context_reset(void) {
    // This is called when the GL context is created or reset
    // You might need to re-initialize Supermodel's shaders here
    wrapper.InitGL();

}

void context_destroy(void) {
    // Cleanup GL resources
}
static struct retro_hw_render_callback hw_render;
bool retro_load_game(const struct retro_game_info *info)
{
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL; 
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = true;
   hw_render.stencil = true; 

   // 3. Give the address to RetroArch. 
   // CRITICAL: RetroArch writes the function pointers directly into this memory address!
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
       return false;
   }

   // 4. NOW pass the populated struct (or pointer) to your wrapper
   // wrapper.setHwRender(hw_render); 
   // Better yet, just let wrapper access the global 'hw_render' or pass by pointer.
   wrapper.setHwRender(hw_render); 

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   fprintf(stderr, "SUPERMODEL: Content Path: %s\n", info->path);
   
   // 5. Do NOT call InitGL here immediately if it relies on get_current_framebuffer.
   // The context might not be active yet. The 'context_reset' callback will signal 
   // when it is safe to call InitGL.
   
   int emulation = wrapper.Emulate(info->path);
   //Game game = wrapper.getGame();

   //wrapper.SuperModelInit(wrapper.getGame());
   return true;

   std::string tocbasepath[4096];

   std::string baseDir = retro_cd_base_directory ? retro_cd_base_directory : "";
   std::string baseName = retro_cd_base_name ? retro_cd_base_name : "";

   // Build the string dynamically
   std::string tocPath = baseDir + retro_slash + baseName + ".toc";
   if (filestream_exists(tocPath.c_str())) 
   {
      snprintf(retro_cd_path, sizeof(retro_cd_path), "%s", tocbasepath);
   }
   else {
      snprintf(retro_cd_path, sizeof(retro_cd_path), "%s", info->path);
   }

   check_variables(true);

   // if (!MDFNI_LoadGame(retro_cd_path))
   //    return false;

   //MDFN_LoadGameCheats(NULL);
   //MDFNMP_InstallReadPatches();

   // Determine content_is_pal before calling alloc_surface()
   // unsigned disc_region = CalcDiscSCEx();
   // content_is_pal = (disc_region == REGION_EU);

   // alloc_surface();

#ifdef NEED_DEINTERLACER
   PrevInterlaced = false;
   deint.ClearState();
#endif

   //input_init();

   boot = false;

   frame_count = 0;
   internal_frame_count = 0;

   // MDFNI_LoadGame() has been called and surface has been allocated,
   // we can now perform firmware check
   bool force_software_renderer = false;
   if (!firmware_found)
   {
      /* TODO - We're forcing the sw renderer to show the ugui error message. Figure out
      how to copy the ugui framebuffer to the hardware renderer side with rsx_intf calls,
      so we don't have to force this anymore. */
      force_software_renderer = true;

#ifdef HAVE_LIGHTREC
      /* Do not run lightrec if firmware is not found, recompiling garbage is bad*/
      psx_dynarec = DYNAREC_DISABLED;
#endif
   }


   /* Hide irrelevant scanline core options for current content */
   struct retro_core_option_display option_display;
   option_display.visible = false;
   if (content_is_pal)
   {
      option_display.key = BEETLE_OPT(initial_scanline);
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
      option_display.key = BEETLE_OPT(last_scanline);
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
   }
   else
   {
      option_display.key = BEETLE_OPT(initial_scanline_pal);
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
      option_display.key = BEETLE_OPT(last_scanline_pal);
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
   }

   return true;//ret;
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
   info->library_name     = MEDNAFEN_CORE_NAME;
#ifdef GIT_VERSION
   info->library_version  = MEDNAFEN_CORE_VERSION GIT_VERSION;
#else
   info->library_version  = MEDNAFEN_CORE_VERSION;
#endif
   info->need_fullpath    = true;
   info->valid_extensions = MEDNAFEN_CORE_EXTENSIONS;
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   //sx_intf_get_system_av_info(info);

   // Sega Model 3 native resolution is 496x384
   info->geometry.base_width   = 496;
   info->geometry.base_height  = 384;
   info->geometry.max_width    = 496;
   info->geometry.max_height   = 384;
   info->geometry.aspect_ratio = 4.0f / 3.0f;

   // Model 3 runs at 60Hz
   info->timing.fps           = 60.0;
   info->timing.sample_rate    = 44100.0;
}

void retro_deinit(void)
{
   // delete surf;
   // surf = NULL;

   libretro_supports_option_categories = false;
   libretro_supports_bitmasks = false;
}

unsigned retro_get_region(void)
{
   // simias: should I override this when fast_pal is set?
   //
   // I'm not entirely sure what's that used for.
   return content_is_pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}


void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   struct retro_led_interface led_interface;
   environ_cb = cb;

   libretro_supports_option_categories = false;
   libretro_set_core_options(environ_cb, &libretro_supports_option_categories);

   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   if (environ_cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &led_interface))
      if (led_interface.set_led_state && !led_state_cb)
         led_state_cb = led_interface.set_led_state;

   //input_set_env(cb);

   //rsx_intf_set_environment(cb);
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

    // This block should only run ONCE in the lifetime of the core
    if (wrapper.getEmulator() != nullptr)
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
    mem.Finish(); // <--- Crucial! Sets the final block's length
    return true;
}

bool retro_unserialize(const void* data, size_t size)
{
    if (!data || size == 0) return false;

    // Construct the memory wrapper around the data RetroArch gave us
    CBlockFileMemory mem(const_cast<void*>(data), size);
    
    // We call LoadState directly. 
    // Because retro_serialize_size now returns a cached value, 
    // no SaveState logic interfered with the internal state before this call.
    wrapper.getEmulator()->LoadState(&mem);
    return true;
}
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

