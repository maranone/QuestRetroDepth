#include "mgba_layer_capture.h"
#include <cstring>

// Flag masks from mGBA software-private.h (must stay in sync)
static constexpr uint32_t kFlagIsBackground = 0x08000000u;
static constexpr uint32_t kFlagIndex        = 0x30000000u;
static constexpr int      kOffsetIndex      = 28;
// FLAG_UNWRITTEN from mGBA software-private.h/video-software.h — the backdrop
// sentinel is this EXACT bit pattern (priority=3, index=3, is_background=1,
// reblend=1), not "any of these bits set". A real composited pixel (e.g. an
// OBJ at priority 0 with no reblend) can legitimately have all of these bits
// clear, so a bitmask-presence check misclassifies it as backdrop.
static constexpr uint32_t kUnwrittenSentinel = 0xFC000000u;
static constexpr uint16_t kGbObjMask        = 0x20u;

static uint8_t  g_visible_source[MGBA_LC_H][MGBA_LC_W];
static uint8_t  g_gb_visible_source[MGBA_GB_LC_H][MGBA_GB_LC_W];
static uint16_t g_per_bg_pixels[4][MGBA_LC_H * MGBA_LC_W];
static uint8_t  g_per_bg_mask[4][MGBA_LC_H * MGBA_LC_W];
static uint16_t g_obj_pixels[MGBA_LC_H * MGBA_LC_W];
static uint8_t  g_obj_mask[MGBA_LC_H * MGBA_LC_W];
static int      g_current_y = -1;

void mgba_lc_clear() {
    // 5 = backdrop — default when nothing has been captured yet.
    // g_visible_source/g_gb_visible_source are recomputed unconditionally every
    // frame (mgba_lc_capture_scanline / mgba_gb_lc_capture_scanline run on both
    // the cached and full-redraw scanline paths), so clearing them here is safe.
    //
    // g_per_bg_pixels/g_per_bg_mask/g_obj_pixels/g_obj_mask are NOT cleared here:
    // mGBA's own scanline cache skips redrawing (and thus skips mgba_lc_bg_pixel/
    // mgba_lc_obj_pixel) for scanlines whose IO registers are unchanged since
    // last frame, so wiping them every frame would erase still-valid capture
    // data for those scanlines. They are cleared per-scanline instead, in
    // mgba_lc_begin_scanline_redraw(), right before that scanline is redrawn.
    memset(g_visible_source, 5, sizeof(g_visible_source));
    memset(g_gb_visible_source, 0, sizeof(g_gb_visible_source));
    g_current_y = -1;
}

void mgba_lc_capture_scanline(const uint32_t* row, int y) {
    if (y < 0 || y >= MGBA_LC_H) return;
    uint8_t* dst = g_visible_source[y];
    for (int x = 0; x < MGBA_LC_W; ++x) {
        const uint32_t px = row[x];
        if ((px & kUnwrittenSentinel) == kUnwrittenSentinel) {
            dst[x] = 5; // backdrop / unwritten
        } else if (px & kFlagIsBackground) {
            dst[x] = static_cast<uint8_t>((px >> kOffsetIndex) & 3); // BG0-3
        } else {
            dst[x] = 4; // OBJ (sprite)
        }
    }
}

const uint8_t* mgba_lc_get_visible_source() {
    return &g_visible_source[0][0];
}

void mgba_gb_lc_capture_scanline(const uint16_t* row, int y, int window_x) {
    if (!row || y < 0 || y >= MGBA_GB_LC_H) return;
    if (window_x < 0) window_x = 0;
    if (window_x > MGBA_GB_LC_W) window_x = MGBA_GB_LC_W;

    uint8_t* dst = g_gb_visible_source[y];
    for (int x = 0; x < MGBA_GB_LC_W; ++x) {
        const uint16_t px = row[x];
        if (px & kGbObjMask) {
            dst[x] = 4; // OBJ
        } else {
            dst[x] = static_cast<uint8_t>((x >= window_x) ? 1 : 0); // window or BG
        }
    }
}

const uint8_t* mgba_gb_lc_get_visible_source() {
    return &g_gb_visible_source[0][0];
}

void mgba_lc_begin_scanline(int y) {
    g_current_y = (y >= 0 && y < MGBA_LC_H) ? y : -1;
}

void mgba_lc_begin_scanline_redraw(int y) {
    if (y >= 0 && y < MGBA_LC_H) {
        for (int bg = 0; bg < 4; ++bg) {
            memset(&g_per_bg_pixels[bg][y * MGBA_LC_W], 0, MGBA_LC_W * sizeof(uint16_t));
            memset(&g_per_bg_mask[bg][y * MGBA_LC_W], 0, MGBA_LC_W * sizeof(uint8_t));
        }
        memset(&g_obj_pixels[y * MGBA_LC_W], 0, MGBA_LC_W * sizeof(uint16_t));
        memset(&g_obj_mask[y * MGBA_LC_W], 0, MGBA_LC_W * sizeof(uint8_t));
    }
    mgba_lc_begin_scanline(y);
}

void mgba_lc_obj_pixel(int x, uint16_t color_rgb565) {
    if (g_current_y < 0 || x < 0 || x >= MGBA_LC_W) return;
    const int idx = g_current_y * MGBA_LC_W + x;
    g_obj_pixels[idx] = color_rgb565;
    g_obj_mask[idx] = 1;
}

const uint16_t* mgba_lc_get_obj_pixels() {
    return g_obj_pixels;
}

const uint8_t* mgba_lc_get_obj_mask() {
    return g_obj_mask;
}

void mgba_lc_bg_pixel(int bg_index, int x, uint16_t color_rgb565) {
    if (g_current_y < 0 || bg_index < 0 || bg_index > 3) return;
    if (x < 0 || x >= MGBA_LC_W) return;
    const int idx = g_current_y * MGBA_LC_W + x;
    g_per_bg_pixels[bg_index][idx] = color_rgb565;
    g_per_bg_mask[bg_index][idx] = 1;
}

const uint16_t* mgba_lc_get_bg_pixels(int bg_index) {
    if (bg_index < 0 || bg_index > 3) return nullptr;
    return g_per_bg_pixels[bg_index];
}

const uint8_t* mgba_lc_get_bg_mask(int bg_index) {
    if (bg_index < 0 || bg_index > 3) return nullptr;
    return g_per_bg_mask[bg_index];
}
