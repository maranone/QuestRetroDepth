#pragma once
#include <cstdint>

// Number of capturable hardware layers:
//   indices 0-3 = BG0-BG3 (snes9x BG array order)
//   index 4     = OBJ (sprites)
#define SNES9X_LAYER_COUNT 5

extern "C" {

// Returns a pointer to the raw RGB565 pixel data for the given layer captured
// during the most-recently rendered frame.  Stride is GFX.RealPPL (= 512).
// Pixels where the layer has no opaque tile are zero (black, but the mask
// says "transparent" for those positions).
// Returns nullptr when GFX is not initialised.
const uint16_t* snes9x_get_layer_pixels(int layer, unsigned* out_stride);

// Returns a pointer to the per-pixel opacity mask for the given layer.
// 1 = opaque (a tile pixel was drawn here), 0 = transparent (palette index 0
// or no tile at all).  Same stride as the pixel buffer (GFX.RealPPL).
const uint8_t* snes9x_get_layer_mask(int layer, unsigned* out_stride);

// Called once per frame (at S9xStartScreenRefresh time) to clear all layer
// capture buffers to transparent (full MAX_SNES_WIDTH × MAX_SNES_HEIGHT).
void snes9x_clear_layer_capture();

// Controls which capture layers are active. Bit 0-4 correspond to BG0-BG3, OBJ.
void snes9x_set_layer_capture_mask(uint32_t mask);
uint32_t snes9x_get_layer_capture_mask();

// Write one pixel to the specified capture layer (called from tileimpl).
// offset is the flat array offset (row * stride + col) into the per-layer
// buffer.  color is the RGB565 value from GFX.ScreenColors[Pix].
void snes9x_layer_capture_put(int8_t layer_idx, uint32_t offset, uint16_t color);

} // extern "C"
