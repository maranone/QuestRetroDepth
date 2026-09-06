/* Per-pixel visible-source and true independent per-layer RGBA capture for
 * FCEUmm NES PPU. See fceux_layer_capture.h for the two-pass design.
 */
#include "fceux_layer_capture.h"
#include <string.h>
#include <android/log.h>
#define QRD_LC_TAG "QRD_NES_LC"

static const int k_nes_w = 256;
static const int k_nes_h = 240;

static uint8_t  s_buf[k_nes_w * k_nes_h];
static int      s_valid  = 0;
static uint32_t s_lc_mask = 0;

/* Raw BG palette-index snapshot, taken BEFORE CopySprites() mutates target. */
static uint8_t  s_bg_raw[k_nes_w * k_nes_h];
/* Raw sprite palette-index snapshot: 0xFF = no sprite at this pixel. */
static uint8_t  s_spr_raw[k_nes_w * k_nes_h];
/* Independent per-layer RGBA8888: [0] = BG (+backdrop fill), [1] = sprites.
 * The packed value is AABBGGRR so its little-endian bytes are [R,G,B,A],
 * matching the shared LayerCapture upload contract. */
static uint32_t s_layer_bufs[2][k_nes_w * k_nes_h];
static int      s_layer_valid = 0;

/* Raw layer capture packing: AABBGGRR, so memory order is [R,G,B,A]. */
static inline uint32_t rgba_from_rgb565_packed(uint32_t px565) {
    const uint8_t r = (uint8_t)(((px565 >> 11) & 0x1Fu) * 255u / 31u);
    const uint8_t g = (uint8_t)(((px565 >> 5)  & 0x3Fu) * 255u / 63u);
    const uint8_t b = (uint8_t)( (px565        & 0x1Fu) * 255u / 31u);
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

extern "C" void fceux_lc_set_capture_mask(uint32_t mask)
{
    s_lc_mask = mask & 0x3u;
}

extern "C" void fceux_lc_capture_bg_line(int scanline,
                                           const uint8_t* target,
                                           int width,
                                           uint8_t backdrop_index)
{
    if (scanline < 0 || scanline >= k_nes_h) return;
    if (!target || width <= 0) return;
    if (width > k_nes_w) width = k_nes_w;
    if (scanline == 0) s_layer_valid = 0;
    uint8_t* dst = s_bg_raw + scanline * k_nes_w;
    for (int x = 0; x < width; ++x) {
        const uint8_t raw = target[x];
        // FCEUmm sets bit 6 on transparent/unrendered BG pixels. A raw
        // 0xFF sentinel is also possible before the scanline is filled; both
        // must resolve to PALRAM[0], not palette entry 0x3F.
        dst[x] = (raw & 0x40u) ? backdrop_index : (uint8_t)(raw & 0x3Fu);
    }
    if (width < k_nes_w) memset(dst + width, backdrop_index, (size_t)(k_nes_w - width));
}

extern "C" void fceux_lc_capture_line(int scanline,
                                       const uint8_t* target,
                                       const uint8_t* spr_buf,
                                       int width)
{
    if (scanline < 0 || scanline >= k_nes_h) return;
    if (scanline == 0) {
        memset(s_buf, 0xFF, sizeof(s_buf));
        s_valid = 0;
    }
    if (!target || width <= 0) return;
    if (width > k_nes_w) width = k_nes_w;

    uint8_t* dst  = s_buf     + scanline * k_nes_w;
    uint8_t* spr  = s_spr_raw + scanline * k_nes_w;
    memset(spr, 0xFF, (size_t)k_nes_w);

    for (int x = 0; x < width; ++x) {
        const uint8_t bg = target[x];
        const uint8_t sp = spr_buf ? spr_buf[x] : (uint8_t)0x80u;

        uint8_t src;
        if (sp != 0x80u) {
            // A sprite pixel exists on this column.
            if (!(sp & 0x40u)) {
                // Front-priority sprite: always wins compositing.
                src = 2;
            } else {
                // Behind-BG sprite: wins only where BG is transparent.
                // After CopySprites, the sprite value (0x40–0x7F) was written
                // to target iff the BG pixel had bit6 set (was transparent/0xFF).
                src = (bg & 0x40u) ? 2 : 1;
            }
            if (src == 2) spr[x] = bg; // post-sprite target[x] holds the sprite's own color here
        } else {
            // No sprite here. FCEUmm marks transparent BG/unrendered backdrop
            // colour with bit 6 set; opaque BG tile pixels have bit 6 clear.
            src = (bg & 0x40u) ? 0 : 1;
        }
        dst[x] = src;
    }
    if (width < k_nes_w) {
        memset(dst + width, 0xFF, (size_t)(k_nes_w - width));
    }
    if (scanline == k_nes_h - 1) {
        s_valid = 1;
        static int s_log_frame = 0;
        if ((s_log_frame++ % 120) == 0) {
            __android_log_print(ANDROID_LOG_DEBUG, QRD_LC_TAG, "frame captured (visible-source + per-layer)");
        }
    }
}

extern "C" void fceux_lc_finalize_frame(void)
{
    if (!s_valid) return;
    const int do_bg  = (s_lc_mask & 1u) != 0;
    const int do_spr = (s_lc_mask & 2u) != 0;
    if (!do_bg && !do_spr) return;

    for (int i = 0; i < k_nes_w * k_nes_h; ++i) {
        if (do_bg) {
            // BG layer is fully opaque -- the backdrop color fills tile gaps
            // the same way SMS's picodrive_sms_lc_finalize_line() does.
            s_layer_bufs[0][i] = rgba_from_rgb565_packed(fceux_lc_palette_lookup(s_bg_raw[i]));
        }
        if (do_spr) {
            const uint8_t idx = s_spr_raw[i];
            s_layer_bufs[1][i] = (idx == 0xFFu)
                ? 0u
                : rgba_from_rgb565_packed(fceux_lc_palette_lookup(idx));
        }
    }
    s_layer_valid = 1;
}

extern "C" const uint8_t* fceux_lc_get_visible_source(unsigned* out_w, unsigned* out_h)
{
    if (!s_valid) {
        static int s_null_log = 0;
        if ((s_null_log++ % 120) == 0)
            __android_log_print(ANDROID_LOG_WARN, QRD_LC_TAG, "get_visible_source: s_valid=0 (scanline 239 not reached yet)");
        return 0;
    }
    *out_w = (unsigned)k_nes_w;
    *out_h = (unsigned)k_nes_h;
    return s_buf;
}

extern "C" const uint32_t* fceux_lc_get_layer_rgba(int li, unsigned* out_w, unsigned* out_h)
{
    if (!s_layer_valid || li < 0 || li >= 2 || !(s_lc_mask & (1u << li))) return 0;
    *out_w = (unsigned)k_nes_w;
    *out_h = (unsigned)k_nes_h;
    return s_layer_bufs[li];
}
