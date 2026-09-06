/* Android stub implementations for SDL-dependent subsystems that are
   excluded from the NDK build (eventloop, screenshot, lirc, vidext). */

#include "src/api/m64p_types.h"
#include "src/api/m64p_vidext.h"
#include <string.h>

/* ── vidext stubs (api/vidext.c excluded — uses SDL video APIs) ── *
 * The Android frontend provides the real EGL-backed implementation through
 * CoreOverrideVidExt. Keep these exports complete because GLideN64 resolves
 * them with dlsym() during PluginStartup. */
static m64p_video_extension_functions g_video_functions;
static int g_video_extension_active = 0;

int    VidExt_InFullscreenMode(void)    { return 0; }
int    VidExt_VideoRunning(void)        { return g_video_extension_active; }

m64p_error OverrideVideoFunctions(m64p_video_extension_functions *f) {
    if (f == NULL)
        return M64ERR_INPUT_ASSERT;
    if (f->Functions < 17)
        return M64ERR_INPUT_INVALID;
    if (f->VidExtFuncInit == NULL ||
        f->VidExtFuncInitWithRenderMode == NULL ||
        f->VidExtFuncQuit == NULL ||
        f->VidExtFuncListModes == NULL ||
        f->VidExtFuncListRates == NULL ||
        f->VidExtFuncSetMode == NULL ||
        f->VidExtFuncSetModeWithRate == NULL ||
        f->VidExtFuncGLGetProc == NULL ||
        f->VidExtFuncGLSetAttr == NULL ||
        f->VidExtFuncGLGetAttr == NULL ||
        f->VidExtFuncGLSwapBuf == NULL ||
        f->VidExtFuncSetCaption == NULL ||
        f->VidExtFuncToggleFS == NULL ||
        f->VidExtFuncResizeWindow == NULL ||
        f->VidExtFuncGLGetDefaultFramebuffer == NULL ||
        f->VidExtFuncVKGetSurface == NULL ||
        f->VidExtFuncVKGetInstanceExtensions == NULL) {
        memset(&g_video_functions, 0, sizeof(g_video_functions));
        g_video_extension_active = 0;
        return M64ERR_SUCCESS;
    }

    memcpy(&g_video_functions, f, sizeof(g_video_functions));
    g_video_extension_active = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_Init(void) {
    return g_video_extension_active ? g_video_functions.VidExtFuncInit() : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_InitWithRenderMode(m64p_render_mode r) {
    return g_video_extension_active ? g_video_functions.VidExtFuncInitWithRenderMode(r) : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_Quit(void) {
    return g_video_extension_active ? g_video_functions.VidExtFuncQuit() : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size *a, int *n) {
    if (g_video_extension_active)
        return g_video_functions.VidExtFuncListModes(a, n);
    if (n) *n = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size s, int *n, int *r) {
    if (g_video_extension_active)
        return g_video_functions.VidExtFuncListRates(s, n, r);
    (void)s;
    (void)r;
    if (n) *n = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetVideoMode(int w,int h,int b,m64p_video_mode m,m64p_video_flags f) {
    return g_video_extension_active ? g_video_functions.VidExtFuncSetMode(w, h, b, m, f) : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetVideoModeWithRate(int w,int h,int rr,int b,m64p_video_mode m,m64p_video_flags f) {
    return g_video_extension_active ? g_video_functions.VidExtFuncSetModeWithRate(w, h, rr, b, m, f) : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ResizeWindow(int w,int h) {
    return g_video_extension_active ? g_video_functions.VidExtFuncResizeWindow(w, h) : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetCaption(const char *t) {
    return g_video_extension_active ? g_video_functions.VidExtFuncSetCaption(t) : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ToggleFullScreen(void) {
    return g_video_extension_active ? g_video_functions.VidExtFuncToggleFS() : M64ERR_SUCCESS;
}

EXPORT m64p_function CALL VidExt_GL_GetProcAddress(const char *p) {
    return g_video_extension_active ? g_video_functions.VidExtFuncGLGetProc(p) : NULL;
}

EXPORT m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr a, int v) {
    return g_video_extension_active ? g_video_functions.VidExtFuncGLSetAttr(a, v) : M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr a, int *v) {
    if (g_video_extension_active)
        return g_video_functions.VidExtFuncGLGetAttr(a, v);
    if (v) *v = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_GL_SwapBuffers(void) {
    return g_video_extension_active ? g_video_functions.VidExtFuncGLSwapBuf() : M64ERR_SUCCESS;
}

EXPORT uint32_t CALL VidExt_GL_GetDefaultFramebuffer(void) {
    return g_video_extension_active ? g_video_functions.VidExtFuncGLGetDefaultFramebuffer() : 0;
}

EXPORT m64p_error CALL VidExt_VK_GetSurface(void **s, void *i) {
    return g_video_extension_active ? g_video_functions.VidExtFuncVKGetSurface(s, i) : M64ERR_SYSTEM_FAIL;
}

EXPORT m64p_error CALL VidExt_VK_GetInstanceExtensions(const char **e[], uint32_t *n) {
    if (g_video_extension_active)
        return g_video_functions.VidExtFuncVKGetInstanceExtensions(e, n);
    if (n) *n = 0;
    return M64ERR_SYSTEM_FAIL;
}

/* eventloop stubs */
int  event_set_core_defaults(void) { return 1; }
void event_initialize(void) {}
void event_sdl_keydown(int keysym, int keymod) { (void)keysym; (void)keymod; }
void event_sdl_keyup(int keysym, int keymod)   { (void)keysym; (void)keymod; }
int  event_gameshark_active(void) { return 0; }
void event_set_gameshark(int active) { (void)active; }

/* screenshot stubs */
void ScreenshotRomOpen(void) {}
void TakeScreenshot(int iFrameNumber) { (void)iFrameNumber; }

/* lirc stubs */
void lircStart(void) {}
void lircStop(void) {}
void lircCheckInput(void) {}
