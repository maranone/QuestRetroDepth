/* QuestRetroDepth: misc small compatibility shims for the vendored
 * lr-yabasanshiro Saturn core.
 *
 * Note: cdbase.c's CHD (.chd disc image) support needs libchdr's chd_*
 * functions, declared via the "chd.h" it includes. We don't vendor a
 * separate libchdr for Saturn -- the PicoDrive core already statically
 * links one (third_party/picodrive/pico/cd/libchdr/), and both are the
 * same upstream project, so those symbols resolve for free at link time
 * once this .so links both cores; no stub needed here. This app's ROM
 * pipeline only ever hands the core .cue+.bin anyway (see
 * QuestVrActivity.kt's extract7zRom()), so the CHD path is untested but
 * also never exercised.
 */
#include <libchdr/chd.h>
#include <stdarg.h>
#include <android/log.h>
#include "vdp2.h"
#include "memory.h"
#include "ygl.h"  /* for SAT2YAB1/SAT2YAB2 -- macros only, no GL symbols needed */

/* Vdp2ColorRamGetColor(): normally defined in vidogl.c/ygles.c (both
 * GL-only, not built here) but called directly by vidsoft.c (software
 * renderer, always built). Ported verbatim from vidogl.c's implementation
 * -- pure VDP2 color-RAM decode, no GL involved. */
u32 Vdp2ColorRamGetColor(u32 colorindex, int alpha)
{
    switch (Vdp2Internal.ColorMode)
    {
    case 0:
    case 1:
    {
        u32 tmp;
        colorindex <<= 1;
        tmp = T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
        return SAT2YAB1(alpha, tmp);
    }
    case 2:
    {
        u32 tmp1, tmp2;
        colorindex <<= 2;
        colorindex &= 0xFFF;
        tmp1 = T2ReadWord(Vdp2ColorRam, colorindex);
        tmp2 = T2ReadWord(Vdp2ColorRam, colorindex + 2);
        return SAT2YAB2(alpha, tmp1, tmp2);
    }
    default: break;
    }
    return 0;
}

/* GetFileDescriptorPath(): cdbase.c calls this to resolve Android
 * content-provider "/proc/self/fd/N" paths to a real filesystem path. This
 * app's ROM pipeline always hands the core a plain extracted-cache file
 * path, never a raw fd path, so this branch is dead code for us -- stub it
 * to "not found" so cdbase.c's normal fopen_utf8() path is used instead. */
const char *GetFileDescriptorPath(const char *fileName) {
    (void)fileName;
    return NULL;
}

/* yprintf(): debug-log helper normally defined in vidogl.c (GL-only, not
 * built here). Route it to logcat instead of dropping it. */
int yprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_DEBUG, "QuestRetroDepth", fmt, args);
    va_end(args);
    return 0;
}

/* YabNanosleep(): normally in thr-linux.cpp (excluded -- its whole YabThread*
 * API duplicates thr-rthreads.c, which is what this build actually uses),
 * but vdp2.cpp's frame limiter calls it directly regardless of thread
 * backend. Trivial passthrough to POSIX nanosleep. */
#include <time.h>
#include <stdint.h>
int YabNanosleep(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    return nanosleep(&ts, NULL);
}

/* GL-renderer stubs. yabause.c/vdp2.cpp/libretro.c reference these symbols
 * unconditionally even when the GL renderer (VIDOGL, vidogl.c) is compiled
 * out -- the calls are runtime-conditional on g_vidcoretype/VIDCore
 * selection (see libretro.c's forced VIDCORE_SOFT default in this build),
 * but the symbols still need to *link*. Since this build only ever
 * activates the software renderer, none of these actually execute; they
 * only need to exist and return a harmless "nothing happened" result. */
char *getLastShaderError(void) { return NULL; }

void VIDOGLVdp2DrawStart(void) {}
void VIDOGLVdp2DrawEnd(void) {}
void VIDOGLVdp2DrawScreens(void) {}

#include <stdbool.h>
#include <glsm/glsm.h>
bool glsm_ctl(enum glsm_state_ctl state, void *data) {
    (void)state; (void)data;
    return false; /* "didn't happen" -- callers all treat this as a soft failure */
}

#include "libretro.h"
struct retro_hw_render_callback hw_render;
