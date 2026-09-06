#pragma once

#include <string>

namespace qrd {

// A GL context on the emulation thread that shares objects with the OpenXR
// renderer's context.
//
// SwanStation's hardware renderer issues GL calls from inside retro_run(),
// which QRD runs on g_emu_thread — but the OpenXR renderer owns the GL context
// and lives on its own thread. Moving emulation onto the render thread would
// couple the 60.0988 Hz emulation clock to the XR frame loop, so instead the
// emulation thread gets its own context in the same share group. Textures and
// FBOs created by either side are visible to both.
//
// Call psx_gl_context_capture_host() once from the render thread while its
// context is current; everything else runs on the emulation thread.
void psx_gl_context_capture_host();

// True once capture_host() has run, i.e. a shared context can be created.
bool psx_gl_context_host_available();

// Creates the shared context if needed and makes it current on the calling
// thread. Call this on whichever thread is about to drive the core, and pair it
// with psx_gl_context_release(). Returns false (with a reason) if the platform
// refuses, which is not fatal: the caller falls back to the software renderer.
bool psx_gl_context_ensure_current(std::string& error_out);

// Releases the context from the calling thread without destroying it, putting
// back whatever that thread had current before ensure_current() took over.
//
// An EGL context is current on at most one thread at a time, and the core is
// driven from three: ROM loading and warm-up run on the JNI thread, per-frame
// emulation on g_emu_thread, and thumbnail capture on the ROM-preview worker.
// All are serialised by g_backend_mutex, so the context is bound around each
// stretch of core work and released after, rather than being pinned to
// whichever thread happened to create it.
//
// Restoring rather than clearing matters because the JNI thread is also the XR
// render thread: it owns the on-screen context, and dropping it to no context
// mid-frame silently voids every GL call the renderer makes afterwards.
void psx_gl_context_release();

// Destroys the shared context. Safe to call even if none was created.
void psx_gl_context_destroy();

} // namespace qrd
