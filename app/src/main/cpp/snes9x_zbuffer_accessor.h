#pragma once
#include <cstdint>
#include <cstddef>

// Thin C-linkage accessor so snes_libretro_backend.cpp can read
// GFX.ZBuffer without pulling in all of snes9x.h.
extern "C" {

// Returns a pointer to the first rendered z-buffer row that corresponds
// to the video frame at `video_data` (which is the same pointer passed to
// retro_video_refresh_t).  Returns nullptr if GFX is not initialised.
// out_stride: number of uint8 elements per row in the z-buffer (= GFX.RealPPL).
const uint8_t* snes9x_get_zbuffer_for_frame(const void* video_data,
                                             unsigned*   out_stride);

} // extern "C"
