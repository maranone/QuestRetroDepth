#pragma once
/*
 * Per-pixel visible-source AND true independent per-layer RGBA capture for
 * FCEUmm NES PPU.
 *
 * Source IDs:  0 = backdrop, 1 = BG tile, 2 = sprite.
 *
 * Two-pass design (mirrors picodrive_sms_layer_capture.cpp's approach):
 *   1. fceux_lc_capture_bg_line() is called from ppu.c BEFORE CopySprites(),
 *      snapshotting the raw BG-only scanline so a sprite drawing on top of a
 *      BG tile never destroys that BG pixel's color -- without this, slicing
 *      the final composited frame by visible_source_id after the fact can't
 *      recover an occluded BG pixel (the "occlusion hole" bug).
 *   2. fceux_lc_capture_line() is called from ppu.c per scanline after
 *      CopySprites() and before the colour-emphasis loop (unchanged from
 *      before), filling the visible_source_id buffer AND -- new -- an
 *      independent raw sprite-color snapshot.
 *   3. fceux_lc_finalize_frame() (called from fceux_backend.cpp once per
 *      frame, after retro_run()) converts both raw palette-index snapshots
 *      to RGBA8888 via fceux_lc_palette_lookup() (libretro.c's retro_palette
 *      table, the exact same one used for the main composited frame).
 *
 * Pixel encoding (XBuf / sprlinebuf before colour emphasis):
 *   target[x] bit 6 set      : transparent BG/unrendered backdrop colour
 *   target[x]  0x00–0x3F     : opaque BG tile pixel (NES palette index)
 *   target[x]  0x40–0x7F     : behind-BG sprite or backdrop colour
 *   spr_buf[x] == 0x80       : no sprite pixel on this column
 *   spr_buf[x] & 0x40 == 0   : front-priority sprite pixel
 *   spr_buf[x] & 0x40 != 0   : behind-BG sprite pixel
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Called from libretro.c -- looks up a NES palette index (0-63) in the same
 * retro_palette[] table used for the main composited frame, returning a
 * packed RGB565 pixel (FRONTEND_SUPPORTS_RGB565 is defined for this build). */
uint32_t fceux_lc_palette_lookup(uint8_t index);

/* Called from ppu.c BEFORE CopySprites() mutates target. backdrop_index is
 * PALRAM[0], the actual NES palette entry used for transparent BG pixels. */
void fceux_lc_capture_bg_line(int scanline,
                               const uint8_t* target,
                               int width,
                               uint8_t backdrop_index);

/* Called from ppu.c after CopySprites(), before the colour-emphasis loop.
 * spr_buf == NULL when SpriteON is false (treat all columns as no-sprite). */
void fceux_lc_capture_line(int scanline,
                            const uint8_t* target,
                            const uint8_t* spr_buf,
                            int width);

/* Enable / disable per-layer RGBA capture.  Bit 0 = BG layer, bit 1 = sprite layer. */
void fceux_lc_set_capture_mask(uint32_t mask);

/* Called from fceux_backend.cpp once per frame, after the last scanline's
 * fceux_lc_capture_line() call, to convert the raw palette-index snapshots
 * into the two independent RGBA8888 layer buffers. */
void fceux_lc_finalize_frame(void);

/* Returns the 256×240 per-pixel source-ID buffer (0/1/2), sets *out_w / *out_h.
 * Returns NULL if no frame has been captured yet. */
const uint8_t* fceux_lc_get_visible_source(unsigned* out_w, unsigned* out_h);

/* Returns a pointer to the internal 256x240 RGBA8888 buffer for one
 * independent layer (0 = BG incl. backdrop fill, 1 = sprites), sets out_w
 * and out_h. Returns NULL if unavailable (mask bit off / not finalized). */
const uint32_t* fceux_lc_get_layer_rgba(int li, unsigned* out_w, unsigned* out_h);

#ifdef __cplusplus
}
#endif
