#pragma once
/* QuestRetroDepth — GBA per-layer capture (visible-source approach).
 *
 * Called from inside mGBA's GBAVideoSoftwareRendererDrawScanline()
 * immediately after GBAVideoSoftwareRendererPostprocessBuffer().
 * At that point softwareRenderer->row[] holds 32-bit pixels with priority
 * flags in the upper bits, colour in the lower 16 bits:
 *   bits 30-31: FLAG_PRIORITY
 *   bits 28-29: FLAG_INDEX  (BG index 0-3)
 *   bit  27:    FLAG_IS_BACKGROUND
 *   bits 0-23:  RGB colour
 *   IS_WRITABLE = (pixel & 0xFE000000) != 0
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* GBA screen dimensions (GBAVideoHorizontalPixels / VerticalPixels enum values) */
#define MGBA_LC_W 240
#define MGBA_LC_H 160

/* GB/GBC screen dimensions */
#define MGBA_GB_LC_W 160
#define MGBA_GB_LC_H 144

/* Number of logical layer capture sources:
 *   0-3 = BG0-BG3, 4 = OBJ, 5 = backdrop
 */
#define MGBA_LC_SOURCES 6

/* Clear capture buffers at the start of a new frame. Call before retro_run(). */
void mgba_lc_clear(void);

/* Called from the mGBA renderer for each scanline after compositing.
 * row: GBA_VIDEO_HORIZONTAL_PIXELS (240) uint32_t values, each carrying
 *      priority flags in the upper bits and RGB565 colour in the lower bits.
 * y:   scanline index [0, GBA_VIDEO_VERTICAL_PIXELS). */
void mgba_lc_capture_scanline(const uint32_t* row, int y);

/* Returns pointer to the visible_source_id buffer:
 *   MGBA_LC_H * MGBA_LC_W bytes, row-major.
 *   Each byte: 0-3 = BG0-BG3, 4 = OBJ, 5 = backdrop. */
const uint8_t* mgba_lc_get_visible_source(void);

/* Called from the GB/GBC software renderer once a composed scanline is ready.
 * row:      GB renderer row[] values before palette lookup
 * y:        scanline index [0, MGBA_GB_LC_H)
 * window_x: first on-screen x position covered by the window, or >= MGBA_GB_LC_W
 *           when the window is inactive for this scanline.
 *
 * Source IDs written into the GB/GBC visible-source buffer:
 *   0 = BG, 1 = window, 4 = OBJ
 */
void mgba_gb_lc_capture_scanline(const uint16_t* row, int y, int window_x);

/* Returns pointer to the GB/GBC visible_source_id buffer:
 *   MGBA_GB_LC_H * MGBA_GB_LC_W bytes, row-major.
 *   Each byte: 0 = BG, 1 = window, 4 = OBJ. */
const uint8_t* mgba_gb_lc_get_visible_source(void);

#ifdef __cplusplus
}
#endif
