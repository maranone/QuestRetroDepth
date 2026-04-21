#include "mgba_layer_capture.h"
#include <cstring>

// Flag masks from mGBA software-private.h (must stay in sync)
static constexpr uint32_t kFlagIsBackground = 0x08000000u;
static constexpr uint32_t kFlagIndex        = 0x30000000u;
static constexpr int      kOffsetIndex      = 28;
// IS_WRITABLE: pixel was actually drawn (not the unwritten/backdrop sentinel)
static constexpr uint32_t kIsWritableMask   = 0xFE000000u;
static constexpr uint16_t kGbObjMask        = 0x20u;

static uint8_t g_visible_source[MGBA_LC_H][MGBA_LC_W];
static uint8_t g_gb_visible_source[MGBA_GB_LC_H][MGBA_GB_LC_W];

void mgba_lc_clear() {
    // 5 = backdrop — default when nothing has been captured yet
    memset(g_visible_source, 5, sizeof(g_visible_source));
    memset(g_gb_visible_source, 0, sizeof(g_gb_visible_source));
}

void mgba_lc_capture_scanline(const uint32_t* row, int y) {
    if (y < 0 || y >= MGBA_LC_H) return;
    uint8_t* dst = g_visible_source[y];
    for (int x = 0; x < MGBA_LC_W; ++x) {
        const uint32_t px = row[x];
        if (!(px & kIsWritableMask)) {
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
