/* Per-pixel visible-source and per-layer RGBA capture for PicoDrive SMS / Mode 4 renderer.
 *
 * Called from mode4.c per scanline (under PICODRIVE_QRD_SMS_LAYER_CAPTURE).
 * Source-ID values:  0 = backdrop, 1 = BG tile, 2 = sprite.
 * Layer indices:     0 = BG tilemap, 1 = sprites.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from PicoFrameStartSMS() to zero layer buffers for the new frame. */
void picodrive_sms_lc_frame_begin(void);

/* Enable / disable per-layer RGBA capture.  Bits 0-1 select layers 0-1. */
void picodrive_sms_lc_set_capture_mask(uint32_t mask);

/* Called from DrawDisplayM4() AFTER BG tiles, BEFORE DrawSpritesM4().
 * Snapshots HighCol (palette indices) so sprites can be isolated later.
 * high_col: Pico.est.HighCol + 8  (pixel 0..width-1 of the scanline).
 * width:    256 for SMS standard mode.
 */
void picodrive_sms_lc_capture_bg_line(int scanline,
                                      const unsigned char* high_col,
                                      int width);

/* Called from DrawDisplayM4() after DrawSpritesM4(), before column masking.
 * high_col: Pico.est.HighCol + 8  (pixel 0..width-1 of the scanline).
 * width:    256 for SMS standard mode.
 * Fills visible_source AND records which pixels changed since capture_bg_line.
 */
void picodrive_sms_lc_capture_spr_line(int scanline,
                                       const unsigned char* high_col,
                                       int width);

/* Called from PicoLineSMS() AFTER FinalizeLineSMS() so HighPal is current.
 * Converts the raw palette-index snapshots to per-layer RGBA8888 buffers.
 * high_pal: Pico.est.HighPal (RGB565 palette, 64 entries).
 */
void picodrive_sms_lc_finalize_line(int scanline, const uint16_t* high_pal);

/* Returns the per-pixel source-ID buffer filled during the last frame.
 * out_w / out_h receive the buffer dimensions.
 * Returns NULL if no valid data is available.
 */
const uint8_t* picodrive_sms_lc_get_visible_source(unsigned* out_w,
                                                    unsigned* out_h);

/* Copies the last captured visible-source frame into a destination geometry.
 * Supports SMS native 256x192 and GG native 160x144 LCD output.
 * Returns 1 on success, 0 otherwise.
 */
int picodrive_sms_lc_copy_visible_source(uint8_t* dst, unsigned dst_w, unsigned dst_h);

/* Copies one RGBA layer into dst, handling SMS and GG geometry.
 * li: layer index (0=BG, 1=sprites).
 * Returns 1 on success, 0 otherwise.
 */
int picodrive_sms_lc_copy_layer_rgba(int li, uint32_t* dst,
                                     unsigned dst_w, unsigned dst_h);

#ifdef __cplusplus
}
#endif
