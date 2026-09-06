#pragma once

namespace qrd {

// Handle to SwanStation's hardware-renderer depth buffer.
//
// GPU_HW keeps a GL_DEPTH_COMPONENT32F texture alongside its VRAM colour
// target and, with PGXP enabled, writes real per-pixel depth into it — the
// feature QRD would otherwise have to rasterise on the CPU. The texture lives
// in (upscaled) VRAM space, and the scanned-out display is a sub-rectangle of
// it, so the offsets below are what make it addressable.
//
// Published from the core's UpdateDisplay() on the emulation thread and
// consumed by PsxLibretroBackend on the same thread, in the same GL context.
struct PsxHwDepthInfo {
    unsigned depth_texture = 0;
    int texture_width = 0;
    int texture_height = 0;
    // Display rectangle within the depth texture, already multiplied by the
    // renderer's resolution scale.
    int display_x = 0;
    int display_y = 0;
    int display_width = 0;
    int display_height = 0;
    // Region the core just drew into, same scaling. Games double-buffer, so
    // this is usually *not* the displayed rectangle: at UpdateDisplay() time
    // the depth buffer holds the frame being drawn, while the scanned-out
    // buffer's depth was wiped by ClearDepthBuffer() when that drawing began.
    // Resolving the displayed rect therefore reads nothing but the clear
    // value; the depth has to be resolved here and paired with this buffer
    // when it is scanned out a frame later.
    int draw_x = 0;
    int draw_y = 0;
    int draw_width = 0;
    int draw_height = 0;
    // False when the core is not maintaining PGXP depth this frame, in which
    // case the depth texture holds nothing meaningful.
    bool pgxp_depth = false;
    bool valid = false;
};

void psx_hw_depth_publish(const PsxHwDepthInfo& info);
PsxHwDepthInfo psx_hw_depth_take();

} // namespace qrd
