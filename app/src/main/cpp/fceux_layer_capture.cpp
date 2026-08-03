/* Per-pixel visible-source capture for FCEUmm NES PPU.
 *
 * Source IDs:  0 = backdrop, 1 = BG tile, 2 = sprite.
 *
 * fceux_lc_capture_line() is called from ppu.c per scanline after CopySprites()
 * and before the colour-emphasis loop, so sprite flag bits are still readable.
 *
 * Pixel classification (XBuf after CopySprites, before colour emphasis):
 *   target[x] bit6 set   → transparent BG/unrendered backdrop colour
 *   target[x]  0x00–0x3F → BG tile pixel (NES palette index, bit6 clear)
 *   target[x]  0x40–0x7F → behind-BG sprite was composited here (BG transparent)
 *
 *   spr_buf[x] == 0x80   → no sprite on this column
 *   spr_buf[x] bit6 == 0 → front-priority sprite pixel
 *   spr_buf[x] bit6 == 1 → behind-BG sprite pixel
 *
 * Combined rule:
 *   sprite in spr_buf, front-priority              → source_id = 2
 *   sprite in spr_buf, behind-BG, BG transparent  → source_id = 2
 *   sprite in spr_buf, behind-BG, BG opaque       → source_id = 1  (BG won)
 *   no sprite, target bit6 set                     → source_id = 0  (backdrop)
 *   no sprite, target bit6 clear                   → source_id = 1  (BG tile)
 */
#include "fceux_layer_capture.h"
#include <string.h>
#include <android/log.h>
#define QRD_LC_TAG "QRD_NES_LC"

// NES NTSC screen dimensions
static const int k_nes_w = 256;
static const int k_nes_h = 240;

static uint8_t  s_buf[k_nes_w * k_nes_h];
static int      s_valid  = 0;
static uint32_t s_lc_mask = 0;

extern "C" void fceux_lc_set_capture_mask(uint32_t mask)
{
    s_lc_mask = mask & 0x7u;
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

    uint8_t* dst = s_buf + scanline * k_nes_w;
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
        // Debug: count source IDs in the buffer
        int cnt[4] = {0, 0, 0, 0};
        for (int i = 0; i < k_nes_w * k_nes_h; ++i) {
            int id = s_buf[i];
            if (id < 3) cnt[id]++;
            else cnt[3]++;
        }
        static int s_log_frame = 0;
        if ((s_log_frame++ % 120) == 0) {
            __android_log_print(ANDROID_LOG_DEBUG, QRD_LC_TAG,
                "frame src0(backdrop)=%d src1(tile)=%d src2(spr)=%d other=%d",
                cnt[0], cnt[1], cnt[2], cnt[3]);
        }
    }
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
