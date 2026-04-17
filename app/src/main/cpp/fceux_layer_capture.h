#pragma once
/*
 * Per-pixel visible-source capture for FCEUmm NES PPU.
 *
 * Source IDs: 0 = backdrop, 1 = BG tile, 2 = sprite.
 *
 * fceux_lc_capture_line() is called from ppu.c per scanline after CopySprites()
 * but before the colour-emphasis post-processing loop, so the raw sprite flag
 * bits are still intact.
 *
 * fceux_lc_get_visible_source() is called from fceux_backend.cpp after
 * retro_run() completes, before the frame is consumed.
 *
 * Pixel encoding (XBuf / sprlinebuf before colour emphasis):
 *   target[x] == 0xFF        : unrendered (backdrop — no BG tile drawn here)
 *   target[x]  0x00–0x3F     : BG tile pixel (NES palette index, bit 6 clear)
 *   target[x]  0x40–0x7F     : behind-BG sprite written here (BG was transparent)
 *   spr_buf[x] == 0x80       : no sprite pixel on this column
 *   spr_buf[x] & 0x40 == 0   : front-priority sprite pixel
 *   spr_buf[x] & 0x40 != 0   : behind-BG sprite pixel
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Called from ppu.c after CopySprites(), before the colour-emphasis loop.
 * spr_buf == NULL when SpriteON is false (treat all columns as no-sprite). */
void fceux_lc_capture_line(int scanline,
                            const uint8_t* target,
                            const uint8_t* spr_buf,
                            int width);

/* Returns the 256×240 per-pixel source-ID buffer (0/1/2), sets *out_w / *out_h.
 * Returns NULL if no frame has been captured yet. */
const uint8_t* fceux_lc_get_visible_source(unsigned* out_w, unsigned* out_h);

#ifdef __cplusplus
}
#endif
