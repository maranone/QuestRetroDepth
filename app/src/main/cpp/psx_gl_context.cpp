#include "psx_gl_context.h"

#include <EGL/egl.h>
#include <android/log.h>

#include <atomic>
#include <mutex>

namespace qrd {
namespace {

constexpr const char* kLogTag = "QuestRetroDepth";

// Written once on the render thread, read on the emulation thread. Plain atomics
// rather than a mutex: capture happens during XR startup, long before the
// emulation thread asks for a context.
std::atomic<void*> g_host_display{nullptr};
std::atomic<void*> g_host_context{nullptr};

// Guards the three handles below. They are reached from the emulation thread
// (per-frame), the JNI/render thread (ROM loads) and the ROM-preview worker,
// which g_backend_mutex serialises at the call sites — but not while the
// handles themselves are being created or torn down.
std::mutex g_mutex;

EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLDisplay g_display = EGL_NO_DISPLAY;

// What the calling thread had current before it took the shared context.
// The render thread owns the on-screen XR context and drives ROM loads on
// itself, so releasing to EGL_NO_CONTEXT would leave it with no context at all
// and every later GL call in the frame loop would be dropped.
thread_local EGLContext t_prev_context = EGL_NO_CONTEXT;
thread_local EGLSurface t_prev_draw = EGL_NO_SURFACE;
thread_local EGLSurface t_prev_read = EGL_NO_SURFACE;
thread_local EGLDisplay t_prev_display = EGL_NO_DISPLAY;
thread_local bool t_saved = false;

void save_current_binding() {
    if (t_saved) return;
    t_prev_context = eglGetCurrentContext();
    t_prev_draw = eglGetCurrentSurface(EGL_DRAW);
    t_prev_read = eglGetCurrentSurface(EGL_READ);
    t_prev_display = eglGetCurrentDisplay();
    t_saved = true;
}

} // namespace

void psx_gl_context_capture_host() {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "PSX GL: no current EGL context to share from; "
                            "hardware renderer will be unavailable");
        return;
    }
    g_host_display.store(display, std::memory_order_release);
    g_host_context.store(context, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "PSX GL: captured host EGL context for sharing");
}

bool psx_gl_context_host_available() {
    return g_host_context.load(std::memory_order_acquire) != EGL_NO_CONTEXT;
}

bool psx_gl_context_ensure_current(std::string& error_out) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_context != EGL_NO_CONTEXT) {
        // Already built; just re-assert currency. eglMakeCurrent is cheap when
        // the context is already current on this thread.
        if (eglGetCurrentContext() == g_context) return true;
        save_current_binding();
        if (eglMakeCurrent(g_display, g_surface, g_surface, g_context)) return true;
        t_saved = false;
        error_out = "PSX GL: eglMakeCurrent failed on existing shared context.";
        return false;
    }

    auto* host_display = g_host_display.load(std::memory_order_acquire);
    auto* host_context = g_host_context.load(std::memory_order_acquire);
    if (!host_context) {
        error_out = "PSX GL: no host GL context captured yet.";
        return false;
    }
    g_display = static_cast<EGLDisplay>(host_display);

    eglBindAPI(EGL_OPENGL_ES_API);
    // Matches the XR renderer's config: a pbuffer-capable ES3 config. The core
    // renders into its own FBOs, so the surface only exists because EGL will
    // not make a context current without one.
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        // EGL_OPENGL_ES3_BIT_KHR; the KHR name needs eglext.h, and this matches
        // the literal the XR renderer's own config request uses.
        EGL_RENDERABLE_TYPE, 0x00000040,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (!eglChooseConfig(g_display, config_attribs, &config, 1, &config_count) || config_count == 0) {
        error_out = "PSX GL: eglChooseConfig found no ES3 pbuffer config.";
        return false;
    }

    const EGLint pbuffer_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g_surface = eglCreatePbufferSurface(g_display, config, pbuffer_attribs);
    if (g_surface == EGL_NO_SURFACE) {
        error_out = "PSX GL: eglCreatePbufferSurface failed.";
        return false;
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    // Sharing with the XR context is the entire point: the resolved depth and
    // colour textures the core produces here must be readable by the renderer.
    g_context = eglCreateContext(g_display, config, static_cast<EGLContext>(host_context), context_attribs);
    if (g_context == EGL_NO_CONTEXT) {
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
        error_out = "PSX GL: eglCreateContext failed (shared ES3 context refused).";
        return false;
    }

    save_current_binding();
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        eglDestroyContext(g_display, g_context);
        eglDestroySurface(g_display, g_surface);
        g_context = EGL_NO_CONTEXT;
        g_surface = EGL_NO_SURFACE;
        t_saved = false;
        error_out = "PSX GL: eglMakeCurrent failed on new shared context.";
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "PSX GL: shared ES3 context current on emulation thread");
    return true;
}

void psx_gl_context_release() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT) return;
    if (eglGetCurrentContext() != g_context) return;
    if (t_saved && t_prev_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(t_prev_display, t_prev_draw, t_prev_read, t_prev_context);
    } else {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    t_saved = false;
}

void psx_gl_context_destroy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_display == EGL_NO_DISPLAY) return;
    if (eglGetCurrentContext() == g_context && t_saved && t_prev_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(t_prev_display, t_prev_draw, t_prev_read, t_prev_context);
    } else {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    t_saved = false;
    if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
    if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
    g_context = EGL_NO_CONTEXT;
    g_surface = EGL_NO_SURFACE;
    g_display = EGL_NO_DISPLAY;
}

} // namespace qrd
