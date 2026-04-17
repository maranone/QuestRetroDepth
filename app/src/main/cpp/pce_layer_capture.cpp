#include "pce_layer_capture.h"
#include <cstring>

static uint8_t g_buf[PCE_LC_H][PCE_LC_W];
static unsigned g_w = PCE_LC_W;
static unsigned g_h = PCE_LC_H;

void pce_lc_frame_begin() {
    // Default everything to backdrop; only rendered columns are overwritten.
    memset(g_buf, PCE_LC_SRC_BACKDROP, sizeof(g_buf));
}

void pce_lc_capture_line(int row, int x_off, int width,
                         const uint8_t*  bg,
                         const uint16_t* spr,
                         unsigned        cr_mode) {
    if (row < 0 || row >= PCE_LC_H) return;
    if (x_off < 0) x_off = 0;
    if (x_off >= PCE_LC_W) return;
    if (x_off + width > PCE_LC_W) width = PCE_LC_W - x_off;

    uint8_t* dst = &g_buf[row][x_off];

    const bool bg_on  = (cr_mode & 0x80) != 0;
    const bool spr_on = (cr_mode & 0x40) != 0;

    for (int i = 0; i < width; ++i) {
        const uint8_t  bg_px  = bg[i];
        const uint16_t spr_px = spr[i];

        // Sprite wins when:
        //   sprite layer enabled AND sprite pixel present (non-zero)
        //   AND (high-priority OR BG pixel transparent)
        const bool bg_opaque  = bg_on  && (bg_px  & 0x0Fu);
        const bool spr_opaque = spr_on && (spr_px != 0u);

        if (spr_opaque && ((spr_px & 0x8000u) || !bg_opaque)) {
            dst[i] = PCE_LC_SRC_SPRITE;
        } else if (bg_opaque) {
            dst[i] = PCE_LC_SRC_BG;
        } else {
            dst[i] = PCE_LC_SRC_BACKDROP;
        }
    }
}

const uint8_t* pce_lc_get_visible_source(unsigned* out_w, unsigned* out_h) {
    if (out_w) *out_w = g_w;
    if (out_h) *out_h = g_h;
    return &g_buf[0][0];
}
