#pragma once

#include <cstdint>

namespace qrd {

// Zero-copy handoff of the PSX frame from the emulation thread to the XR thread.
//
// The two threads have separate GL contexts in one share group, so textures
// created by the emulator are directly samplable by the renderer — no readback,
// no CPU copies, no re-upload. Only the identifiers and a fence cross the
// thread boundary.
//
// Slots rotate so the emulator can render frame N while the renderer is still
// sampling N-1. The renderer holds one slot at a time; the emulator never picks
// the held slot.
struct PsxGpuFrame {
    unsigned color_texture = 0;
    unsigned depth_texture = 0;
    // Size of the displayed image, i.e. display size times the internal scale.
    int width = 0;
    int height = 0;
    // Origin of that image inside the shared texture. The scanned-out window is
    // NOT always at the texture origin: Time Crisis page-flips between two
    // buffers in VRAM (display y alternating 8 and 248 of a 512-row texture),
    // so sampling from 0,0 shows the wrong band and the picture reads as
    // cropped. Games whose display sits at 0,0 are unaffected, which is why
    // this stayed hidden.
    int x = 0;
    int y = 0;
    // Full size of the colour texture. The slots are allocated for the core's
    // *max* geometry (VRAM-sized, 1024x512), while any given frame scans out
    // only width x height of it, anchored at the bottom-left in GL's
    // origin-at-bottom convention. Sampling the whole texture instead of that
    // sub-rectangle shows raw VRAM with the game in one corner.
    int tex_width = 0;
    int tex_height = 0;
    // False when this frame carried too little 3D geometry to displace.
    bool has_depth = false;
    int slot = -1;
    uint64_t sequence = 0;
};

// Number of rotating slots. Three is enough to keep the emulator from ever
// waiting: one being rendered, one published, one held by the renderer.
constexpr int kPsxGpuFrameSlots = 3;

// --- emulation thread ---

// Returns a slot index not currently held by the renderer, or -1 if none is
// free (which should not happen with three slots, but is handled rather than
// asserted since a stalled renderer must not corrupt a live frame).
int psx_gpu_frame_acquire_slot();

// Publishes a finished slot. `fence` is a GLsync created after the last draw
// into the slot; ownership passes to this module.
void psx_gpu_frame_publish(const PsxGpuFrame& frame, void* fence);

// --- XR thread ---

// Opens a new XR frame: the next acquire may take a newly published frame.
// Call once per XR frame, before the eye loop.
//
// Both eyes must sample the same emulator frame. The emulator publishes at
// 60.0988 Hz and the compositor renders at 72 Hz, so without this latch a frame
// landing between the two eye draws gives each eye a different image — the
// stereo pair stops fusing.
void psx_gpu_frame_begin_xr_frame();

// Hands back the frame to sample. Takes a newly published one at most once per
// XR frame; every later call within the same XR frame (i.e. the second eye)
// returns the frame already held. Waits on the producing fence so sampling
// cannot race the emulator's writes. Returns false when nothing is held yet.
bool psx_gpu_frame_acquire_latest(PsxGpuFrame& out);

// Drops any held/published state. Call when the PSX backend goes away; the GL
// objects themselves belong to the backend.
void psx_gpu_frame_reset();

} // namespace qrd
