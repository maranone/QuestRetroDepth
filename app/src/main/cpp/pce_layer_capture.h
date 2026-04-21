#pragma once
/* QuestRetroDepth — PC Engine per-layer capture (visible-source approach).
 *
 * Called from vdc.c DrawDisplayBG() / DrawSprites() after compositing.
 * pce_lc_capture_line() classifies each pixel as backdrop (0), bg_plane (1),
 * or sprites (2) and stores the result in the visible-source ID buffer.
 *
 * Screen dimensions: 512 wide × 243 tall (internal VDC surface).
 * The hook passes target_offset + width so only the active columns are written.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal VDC surface dimensions */
#define PCE_LC_W 512
#define PCE_LC_H 243

/* Source IDs stored in the visible-source buffer */
#define PCE_LC_SRC_BACKDROP 0
#define PCE_LC_SRC_BG       1
#define PCE_LC_SRC_SPRITE   2

/* Called at frame start (before retro_run) to reset the buffer to backdrop. */
void pce_lc_frame_begin(void);

/*
 * Called once per rendered scanline from vdc.c (inside DrawDisplayBG).
 *
 * row     - scanline index within the VDC surface [0, PCE_LC_H)
 * x_off   - first column written in this scanline (target_offset)
 * width   - number of columns written
 * bg      - pointer to bg_linebuf[BG_XOffset&7 + source_offset]: palette indices,
 *           lower nibble non-zero = opaque BG pixel
 * spr     - pointer to spr_linebuf[0x20 + source_offset]: sprite data,
 *           non-zero = opaque sprite pixel (bit 15 = high-priority)
 * cr_mode - vdc->CR & 0xC0:
 *             0xC0 = BG+SPR both enabled
 *             0x80 = BG only
 *             0x40 = SPR only
 *             0x00 = neither (all backdrop)
 */
void pce_lc_capture_line(int row, int x_off, int width,
                         const uint8_t*  bg,
                         const uint16_t* spr,
                         unsigned        cr_mode);

/* Returns pointer to the visible_source_id buffer:
 *   PCE_LC_H * PCE_LC_W bytes, row-major.
 *   Each byte: 0 = backdrop, 1 = bg_plane, 2 = sprite. */
const uint8_t* pce_lc_get_visible_source(unsigned* out_w, unsigned* out_h);

/* Copies the visible-source buffer into a destination frame size.
 * Smaller outputs are taken as a centered crop of the internal VDC surface.
 * Returns 1 when the copy succeeds, 0 otherwise.
 */
int pce_lc_copy_visible_source(uint8_t* dst, unsigned dst_w, unsigned dst_h);

#ifdef __cplusplus
}
#endif
