#include "mgba_layer_capture.h"
#include <cstring>

// Flag masks from mGBA software-private.h (must stay in sync)
static constexpr uint32_t kFlagIsBackground = 0x08000000u;
static constexpr uint32_t kFlagIndex        = 0x30000000u;
static constexpr int      kOffsetIndex      = 28;
// IS_WRITABLE: pixel was actually drawn (not the unwritten/backdrop sentinel)
static constexpr uint32_t kIsWritableMask   = 0xFE000000u;

static uint8_t g_visible_source[MGBA_LC_H][MGBA_LC_W];

void mgba_lc_clear() {
    // 5 = backdrop — default when nothing has been captured yet
    memset(g_visible_source, 5, sizeof(g_visible_source));
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
