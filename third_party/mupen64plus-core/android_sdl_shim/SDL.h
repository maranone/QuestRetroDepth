/* Minimal SDL.h shim for Android NDK builds of mupen64plus-core.
   Provides the small surface of SDL2 that main.c and workqueue.c use. */
#pragma once
#include "SDL_thread.h"
#include <unistd.h>
#include <time.h>

/* SDL init flags used by frontend.c */
#define SDL_INIT_VIDEO  0x00000020u
#define SDL_INIT_AUDIO  0x00000010u
#define SDL_INIT_EVENTS 0x00004000u

static inline void SDL_PumpEvents(void) {}
static inline int  SDL_WasInit(Uint32 flags) { (void)flags; return 0; }
static inline void SDL_Quit(void) {}
static inline int  SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
static inline void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
static inline const char* SDL_GetError(void) { return ""; }
static inline const char* SDL_getenv(const char* n) { (void)n; return NULL; }
static inline int SDL_atoi(const char* s) { return s ? atoi(s) : 0; }

static inline void SDL_Delay(Uint32 ms) {
    usleep((useconds_t)ms * 1000u);
}

static inline Uint32 SDL_GetTicks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint32)(ts.tv_sec * 1000u + (Uint32)(ts.tv_nsec / 1000000));
}
