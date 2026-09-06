#include "saturn_layer_capture.h"

#include <cstddef>

namespace {

constexpr std::size_t kBufPixels = static_cast<std::size_t>(SATURN_LC_MAX_W) * SATURN_LC_MAX_H;

uint32_t g_layers[SATURN_LAYER_COUNT][kBufPixels];
int g_layer_width[SATURN_LAYER_COUNT] = {};
uint32_t g_capture_mask = 0xFFFFFFFFu;

} // namespace

extern "C" {

void saturn_lc_frame_begin(void) {
    for (auto& layer : g_layers) {
        for (auto& px : layer) px = 0u;
    }
    for (auto& w : g_layer_width) w = 0;
}

uint32_t* saturn_lc_get_write_buffer(int layer) {
    if (layer < 0 || layer >= SATURN_LAYER_COUNT) return nullptr;
    if (!((g_capture_mask >> layer) & 1u)) return nullptr;
    return g_layers[layer];
}

void saturn_lc_set_layer_dims(int layer, int width, int height) {
    if (layer < 0 || layer >= SATURN_LAYER_COUNT) return;
    (void)height; // only width is exposed/needed by the resampling consumer today
    g_layer_width[layer] = width;
}

const uint32_t* saturn_lc_get_layer_pixels(int layer) {
    if (layer < 0 || layer >= SATURN_LAYER_COUNT) return nullptr;
    return g_layers[layer];
}

int saturn_lc_get_layer_width(int layer) {
    if (layer < 0 || layer >= SATURN_LAYER_COUNT) return 0;
    return g_layer_width[layer];
}

void saturn_lc_set_layer_capture_mask(uint32_t mask) { g_capture_mask = mask; }
uint32_t saturn_lc_get_layer_capture_mask(void) { return g_capture_mask; }

} // extern "C"
