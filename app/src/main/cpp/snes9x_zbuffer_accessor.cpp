// Compiled as part of snes9x_libretro (which has snes9x.h / gfx.h on its
// include path).  Exposes a plain-C function so snes_libretro_backend.cpp
// can read GFX.ZBuffer without including snes9x internal headers.

#include "snes9x.h"
#include "gfx.h"

#include <cstdint>
#include <cstddef>

extern "C"
const uint8_t* snes9x_get_zbuffer_for_frame(const void* video_data,
                                             unsigned*   out_stride) {
    if (!GFX.ZBuffer || !GFX.Screen || !video_data) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }

    const unsigned stride = GFX.RealPPL; // pixels per row (= MAX_SNES_WIDTH = 512)
    if (out_stride) *out_stride = stride;

    // video_data = GFX.Screen + overscan_offset * (Pitch / sizeof(uint16))
    // GFX.Pitch is in bytes.  Use byte arithmetic to find the matching z-buffer row.
    const ptrdiff_t byte_offset =
        static_cast<const uint8_t*>(video_data) -
        reinterpret_cast<const uint8_t*>(GFX.Screen);

    const ptrdiff_t row_offset =
        (byte_offset >= 0 && GFX.Pitch > 0)
            ? (byte_offset / static_cast<ptrdiff_t>(GFX.Pitch))
            : 0;

    return GFX.ZBuffer + row_offset * stride;
}
