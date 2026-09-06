#include "psx_gpu_frame.h"

#include <GLES3/gl3.h>

#include <mutex>

namespace qrd {
namespace {

std::mutex g_mutex;

PsxGpuFrame g_published;
GLsync g_published_fence = nullptr;
bool g_has_published = false;

// The frame the renderer is sampling, kept here so both eyes of one XR frame
// get the same one.
PsxGpuFrame g_held;
// Set by begin_xr_frame(), cleared once a new frame has been taken, so only the
// first eye of an XR frame can advance to a newer image.
bool g_xr_frame_open = false;

// Slot the renderer is currently sampling. The emulator must not draw into it.
int g_held_slot = -1;
// Slot the emulator last handed out, so rotation does not immediately reuse it.
int g_last_acquired = -1;
uint64_t g_sequence = 0;

} // namespace

int psx_gpu_frame_acquire_slot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 1; i <= kPsxGpuFrameSlots; ++i) {
        const int candidate = (g_last_acquired + i) % kPsxGpuFrameSlots;
        if (candidate == g_held_slot) continue;
        // Do not overwrite a published-but-not-yet-taken frame either; the
        // renderer may still pick it up.
        if (g_has_published && candidate == g_published.slot && candidate != g_last_acquired) continue;
        g_last_acquired = candidate;
        return candidate;
    }
    return -1;
}

void psx_gpu_frame_publish(const PsxGpuFrame& frame, void* fence) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // A frame that was published but never taken is superseded; its fence would
    // otherwise leak.
    if (g_published_fence) glDeleteSync(g_published_fence);
    g_published = frame;
    g_published.sequence = ++g_sequence;
    g_published_fence = static_cast<GLsync>(fence);
    g_has_published = true;
}

void psx_gpu_frame_begin_xr_frame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_xr_frame_open = true;
}

bool psx_gpu_frame_acquire_latest(PsxGpuFrame& out) {
    GLsync fence = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_xr_frame_open || !g_has_published) {
            // Second eye of this XR frame, or the emulator has not published
            // since the last one (it runs slower than the compositor, and is
            // frozen outright while a menu is open). Either way the held frame
            // is what this eye must draw — taking anything else, or nothing,
            // breaks the stereo pair.
            out = g_held;
            return g_held.color_texture != 0;
        }
        g_held = g_published;
        g_held_slot = g_published.slot;
        out = g_held;
        fence = g_published_fence;
        g_published_fence = nullptr;
        g_has_published = false;
        g_xr_frame_open = false;
    }

    if (fence) {
        // The emulator's draws live in the other context; without this the
        // renderer can sample a partially written texture.
        glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
    }
    return out.color_texture != 0;
}

void psx_gpu_frame_reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_published_fence) glDeleteSync(g_published_fence);
    g_published_fence = nullptr;
    g_published = PsxGpuFrame{};
    g_held = PsxGpuFrame{};
    g_has_published = false;
    g_xr_frame_open = false;
    g_held_slot = -1;
    g_last_acquired = -1;
}

} // namespace qrd
