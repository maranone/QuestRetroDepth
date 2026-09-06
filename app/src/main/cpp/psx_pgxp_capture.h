#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace qrd {

// Depth-only capture of SwanStation's GPU command stream.
//
// QRD does not re-rasterise PSX primitives: the core's software renderer still
// produces the one true colour image. All we take from the command stream is
// enough geometry to reconstruct a per-pixel depth map (screen-space outline
// plus PGXP's view-space W), which then displaces the screen mesh in the XR
// renderer. That is why there are no texpage/CLUT/blend fields here — nothing
// downstream samples VRAM, so reproducing PSX texturing is not our problem.
//
// Produced on the emulation thread, drained by PsxLibretroBackend on the same
// thread before the frame is published. Never touched by the XR/GL thread.
enum class PsxPgxpCaptureKind : uint8_t {
    Triangle = 0,
    Quad = 1,
    Line = 2,
    Sprite = 3,
};

struct PsxPgxpCaptureVertex {
    // x/y are VRAM draw-space coordinates (drawing offset already applied).
    float x = 0.0f;
    float y = 0.0f;
    // PGXP view-space W. Only meaningful when valid_w is set; the PS1 hands the
    // GPU pre-projected coordinates, so this is the only depth signal there is.
    float w = 0.0f;
    bool valid_w = false;
};

struct PsxPgxpCapturePrimitive {
    PsxPgxpCaptureKind kind = PsxPgxpCaptureKind::Triangle;
    uint8_t vertex_count = 0;
    std::array<PsxPgxpCaptureVertex, 4> vertices{};
    // Semi-transparent primitives are usually effects (shadows, glows) drawn on
    // top of real geometry. Writing them into the depth map punches holes in
    // otherwise solid surfaces, so the rasteriser skips them.
    bool semi_transparent = false;
};

// The VRAM rectangle currently being scanned out. Draw coordinates are in VRAM
// space, so without this the depth map cannot be aligned to the video frame —
// double-buffered games flip between VRAM y=0 and y=256 every frame.
struct PsxPgxpCaptureDisplay {
    uint16_t vram_left = 0;
    uint16_t vram_top = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    bool valid = false;
};

void psx_pgxp_capture_set_enabled(bool enabled);
bool psx_pgxp_capture_enabled();
void psx_pgxp_capture_reset();
void psx_pgxp_capture_emit(const PsxPgxpCapturePrimitive& primitive);
void psx_pgxp_capture_set_display(uint16_t vram_left, uint16_t vram_top,
                                  uint16_t width, uint16_t height);
void psx_pgxp_capture_take(std::vector<PsxPgxpCapturePrimitive>& out,
                           PsxPgxpCaptureDisplay& display_out);

} // namespace qrd
