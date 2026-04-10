// Compiled as part of snes9x_libretro. Exposes the main-screen source-ownership
// buffer without requiring the frontend to include snes9x internals.

#include "snes9x.h"
#include "gfx.h"

#include <cstddef>
#include <cstdint>

extern "C"
const uint8_t* snes9x_get_visible_source_for_frame(const void* video_data,
                                                   unsigned* out_stride) {
    if (!GFX.VisibleSourceBuffer || !GFX.Screen || !video_data) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }

    const unsigned stride = GFX.RealPPL;
    if (out_stride) *out_stride = stride;

    const ptrdiff_t byte_offset =
        static_cast<const uint8_t*>(video_data) -
        reinterpret_cast<const uint8_t*>(GFX.Screen);

    const ptrdiff_t row_offset =
        (byte_offset >= 0 && GFX.Pitch > 0)
            ? (byte_offset / static_cast<ptrdiff_t>(GFX.Pitch))
            : 0;

    return GFX.VisibleSourceBuffer + row_offset * stride;
}
