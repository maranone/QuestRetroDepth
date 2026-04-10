// Compiled as part of snes9x_libretro (which has snes9x.h / gfx.h on its
// include path).  Provides per-layer pixel capture buffers that tileimpl
// dual-writes into during the normal render pass.

#include "snes9x.h"
#include "gfx.h"
#include "snes9x_layer_capture.h"

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

const uint16_t* snes9x_get_layer_pixels(int layer, unsigned* out_stride) {
    if (layer < 0 || layer >= SNES9X_LAYER_COUNT) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }
    if (out_stride) *out_stride = GFX.RealPPL;
    return g_layer_pixels[layer];
}

const uint8_t* snes9x_get_layer_mask(int layer, unsigned* out_stride) {
    if (layer < 0 || layer >= SNES9X_LAYER_COUNT) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }
    if (out_stride) *out_stride = GFX.RealPPL;
    return g_layer_mask[layer];
}

} // extern "C"
