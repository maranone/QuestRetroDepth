#pragma once

#include <cstdint>
#include <vector>

namespace qrd {

// Per-pixel depth for one PSX frame, aligned 1:1 with FrameOutput::rgba8888.
//
// QRD's PSX mode is a depth-displaced screen: SwanStation's software renderer
// still produces the colour image (so PSX rasterisation stays exactly correct),
// and this buffer says how far back each of those pixels should sit. The XR
// renderer displaces a dense screen mesh by it, which is what gives the picture
// real stereo depth without QRD reimplementing the PS1 GPU.
//
// depth is "far-ness": 0 = on the screen plane (HUD, text, 2D sprites),
// 255 = the furthest geometry in this frame. The range is renormalised per
// frame from PGXP's view-space W, so it adapts to whatever scale a game draws
// in. Stored as bytes because it uploads straight into a GL_R8 texture and 1/255
// of the depth range is well under a millimetre at any comfortable screen size.
struct PsxDepthFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> depth;
    // False when the frame carried too little 3D geometry to be worth
    // displacing (menus, FMV, 2D games). The renderer then presents a flat
    // screen rather than inventing depth from noise.
    bool has_geometry = false;
};

// Which depth value sits on the screen plane. Displacement is
// (far_ness - pivot) * range, so the scene extends both ways around it:
//
//   0.0  screen plane is the near end, everything extrudes away (the original)
//   1.0  screen plane is the far end, everything extrudes toward the viewer
//   0.5  centred: near half comes forward, far half recedes
//
// Read by both the depth resolve and the screen mesh, and they must agree: the
// resolve writes this value for pixels PGXP never resolved (semi-transparent
// batches, HUD, text — the core only depth-writes opaque geometry), so those
// land exactly on the screen plane for any pivot instead of snapping to an
// extreme. Overridable at runtime with debug.qrd.psxpivot.
constexpr float kPsxDepthPivotDefault = 0.25f;

} // namespace qrd
