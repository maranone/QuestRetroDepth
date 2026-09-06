// Compiled as part of snes9x_libretro (which has snes9x.h / gfx.h on its
// include path).  Provides per-layer pixel capture buffers that tileimpl
// dual-writes into during the normal render pass.

#include "snes9x.h"
#include "gfx.h"
#include "snes9x_layer_capture.h"

#include <cstddef>
#include <cstring>

// Raw RGB565 pixel data per layer (same stride as GFX.Screen = GFX.RealPPL).
static uint16_t g_layer_pixels[SNES9X_LAYER_COUNT][MAX_SNES_WIDTH * MAX_SNES_HEIGHT];
// 1 = opaque tile pixel written here, 0 = transparent / no tile.
static uint8_t  g_layer_mask  [SNES9X_LAYER_COUNT][MAX_SNES_WIDTH * MAX_SNES_HEIGHT];
static uint32_t g_capture_mask = (1u << SNES9X_LAYER_COUNT) - 1u;

extern "C" {

void snes9x_clear_layer_capture() {
    for (int i = 0; i < SNES9X_LAYER_COUNT; ++i) {
        if ((g_capture_mask & (1u << i)) == 0) continue;
        memset(g_layer_pixels[i], 0, sizeof(g_layer_pixels[i]));
        memset(g_layer_mask[i],   0, sizeof(g_layer_mask[i]));
    }
    GFX.CaptureLayerIndex = -1;
}

void snes9x_set_layer_capture_mask(uint32_t mask) {
    g_capture_mask = mask & ((1u << SNES9X_LAYER_COUNT) - 1u);
}

uint32_t snes9x_get_layer_capture_mask() {
    return g_capture_mask;
}

void snes9x_layer_capture_put(int8_t layer_idx, uint32_t offset, uint16_t color) {
    // offset is already the flat index into the stride-512 buffer.
    // Bounds-check: offset < SNES9X_LAYER_COUNT buffer size.
    if ((unsigned)layer_idx < SNES9X_LAYER_COUNT &&
        (g_capture_mask & (1u << layer_idx)) != 0 &&
        offset < MAX_SNES_WIDTH * MAX_SNES_HEIGHT)
    {
        g_layer_pixels[layer_idx][offset] = color;
        g_layer_mask  [layer_idx][offset] = 1;
    }
}

// video_data may point into the middle of GFX.Screen when the core crops
// overscan. The capture buffers share GFX.Screen's row layout, so bias them by
// the same number of rows the z-buffer accessor uses; otherwise every captured
// layer is displaced vertically against the composited frame.
static ptrdiff_t capture_row_offset(const void* video_data) {
    if (!video_data || !GFX.Screen || GFX.Pitch <= 0) return 0;
    const ptrdiff_t byte_offset =
        static_cast<const uint8_t*>(video_data) -
        reinterpret_cast<const uint8_t*>(GFX.Screen);
    return (byte_offset >= 0) ? (byte_offset / static_cast<ptrdiff_t>(GFX.Pitch)) : 0;
}

const uint16_t* snes9x_get_layer_pixels(int layer, const void* video_data, unsigned* out_stride) {
    if (layer < 0 || layer >= SNES9X_LAYER_COUNT) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }
    const unsigned stride = GFX.RealPPL;
    if (out_stride) *out_stride = stride;
    return g_layer_pixels[layer] + capture_row_offset(video_data) * stride;
}

const uint8_t* snes9x_get_layer_mask(int layer, const void* video_data, unsigned* out_stride) {
    if (layer < 0 || layer >= SNES9X_LAYER_COUNT) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }
    const unsigned stride = GFX.RealPPL;
    if (out_stride) *out_stride = stride;
    return g_layer_mask[layer] + capture_row_offset(video_data) * stride;
}

} // extern "C"
