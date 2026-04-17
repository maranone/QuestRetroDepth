/* Per-pixel visible-source capture for PicoDrive SMS / Mode 4 renderer.
 *
 * Called from mode4.c per scanline (under PICODRIVE_QRD_SMS_LAYER_CAPTURE).
 * Source-ID values:  0 = backdrop, 1 = BG tile, 2 = sprite.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from DrawDisplayM4() after DrawSpritesM4(), before column masking.
 * high_col: Pico.est.HighCol + 8  (pixel 0..width-1 of the scanline).
 * width:    256 for SMS standard mode.
 */
void picodrive_sms_lc_capture_line(int scanline,
                                    const unsigned char* high_col,
                                    int width);

/* Returns the per-pixel source-ID buffer filled during the last frame.
 * out_w / out_h receive the buffer dimensions.
 * Returns NULL if no valid data is available.
 */
const uint8_t* picodrive_sms_lc_get_visible_source(unsigned* out_w,
                                                    unsigned* out_h);

#ifdef __cplusplus
}
#endif
