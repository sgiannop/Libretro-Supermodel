/*
 * Drop-in GLEW replacement for the Supermodel libretro port.
 * Uses glsym from libretro-common for GL extension loading —
 * no external GLEW dependency required on any platform.
 *
 * Android uses its own shim at Src/OSD/Android/include/GL/glew.h.
 */

#ifndef SUPERMODEL_LIBRETRO_GLEW_SHIM_H
#define SUPERMODEL_LIBRETRO_GLEW_SHIM_H

#include <glsym/glsym.h>

/* Fallback for GLAPIENTRY — should be defined by <GL/gl.h> from glsym,
   but some MinGW cross-compile environments may not propagate it */
#ifndef GLAPIENTRY
#  ifdef _WIN32
#    define GLAPIENTRY __stdcall
#  else
#    define GLAPIENTRY
#  endif
#endif

#ifdef _WIN32
/* windows.h (included by the glsym chain above) pollutes the namespace with
   macros that shadow C++ class method names used in Supermodel.  Undef the
   known offenders so that CThread::CreateSemaphore etc. resolve correctly. */
#undef interface
#undef CreateSemaphore
#undef CreateMutex
#undef CreateThread
#undef DeleteFile
#undef GetMessage
#undef LoadImage
#undef MessageBox
#undef PlaySound
#undef SendMessage
#undef PostMessage
#undef LoadLibrary
#undef GetObject
#undef DrawText
#endif

#ifndef ANDROID
#include <GL/glu.h>
#endif

/* GLEW compatibility stubs — replaced by rglgen_resolve_symbols() at init */
#define GLEW_OK             0u
#define GLEW_NO_ERROR       0u
#define GLEW_VERSION_2_0    1
#define GLEW_VERSION_3_0    1
#define GLEW_VERSION_4_1    1

static inline unsigned int   glewInit(void)                          { return GLEW_OK; }
static inline const char*    glewGetErrorString(unsigned int e)      { (void)e; return ""; }

#endif /* SUPERMODEL_LIBRETRO_GLEW_SHIM_H */
