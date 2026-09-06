#pragma once
/* QuestRetroDepth — Sega Saturn per-layer capture (Yaba Sanshiro 2 / lr-yabasanshiro core).
 *
 * Called once per frame from src/titan/titan.c's TitanRender(), right before it
 * composites the six per-plane `PixelData` buffers it already maintains
 * (NBG0-3, RBG0, VDP1 sprites) into the final display buffer. Titan gives each
 * pixel an explicit `priority` field where 0 means "never written this frame"
 * (i.e. transparent) -- a much more reliable transparency signal than the
 * previous Mednafen-based core's sentinel-color test.
 *
 * Unlike the old per-scanline Mednafen hook, this is a single post-frame hook:
 * titan.c decodes its own PixelData arrays (it has the struct definition,
 * this header doesn't need it) and writes ARGB8888 straight into the buffers
 * this file owns.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7 semantic layer slots, matching every other backend's LayerConfig
 * convention in this app. Titan itself only has 6 physical slots (no
 * separate RBG1 buffer -- RBG1 mode reuses the NBG0 slot in hardware, same
 * as the previous core's shared-buffer handling), so SATURN_LAYER_RBG1 is
 * always left empty/untouched by the capture hook. */
#define SATURN_LAYER_COUNT 7
#define SATURN_LAYER_NBG0 0
#define SATURN_LAYER_NBG1 1
#define SATURN_LAYER_NBG2 2
#define SATURN_LAYER_NBG3 3
#define SATURN_LAYER_RBG0 4
#define SATURN_LAYER_RBG1 5
#define SATURN_LAYER_VDP1 6

/* Fixed capture-buffer geometry: a generous upper bound covering every VDP2
 * display mode. Titan's own buffers are sized to the live vdp2width/height
 * (which changes with hi-res/interlace mode), so the capture hook crops to
 * this bound and the caller (SaturnLibretroBackend) crops again to the
 * frame's real width/height -- same approach as the previous core. */
#define SATURN_LC_MAX_W 704
#define SATURN_LC_MAX_H 512

/* Called once per frame (before retro_run()) to clear every layer's capture
 * buffer to fully transparent and reset recorded widths to 0. */
void saturn_lc_frame_begin(void);

/* Returns a pointer to this layer's write buffer (SATURN_LC_MAX_H rows of
 * SATURN_LC_MAX_W pixels, row-major, ARGB8888, caller decodes and writes
 * directly) -- or nullptr if `layer` is out of range or outside the current
 * capture mask (skip the decode work entirely in that case). */
uint32_t* saturn_lc_get_write_buffer(int layer);

/* Records the actual width/height this layer was captured at this frame
 * (both clamped to SATURN_LC_MAX_W/H by the caller before calling this).
 * Must be called after writing into the buffer from saturn_lc_get_write_buffer(). */
void saturn_lc_set_layer_dims(int layer, int width, int height);

/* Returns a pointer to the ARGB8888 capture buffer for the given layer:
 * SATURN_LC_MAX_H rows of SATURN_LC_MAX_W pixels each, row-major. Returns
 * nullptr if layer is out of range. Always non-null otherwise (buffers are
 * allocated once at startup, not lazily per frame). */
const uint32_t* saturn_lc_get_layer_pixels(int layer);

/* Returns the width this layer was actually captured at this frame (the
 * `width` last passed to saturn_lc_set_layer_dims() for this layer). 0 if the
 * layer was never captured this frame (disabled, or outside the capture mask). */
int saturn_lc_get_layer_width(int layer);

/* Controls which capture layers are active. Bit n corresponds to SATURN_LAYER_n.
 * Layers outside the mask are left untouched (skips the decode work) --
 * saturn_lc_get_write_buffer() returns nullptr for a masked-out layer. */
void saturn_lc_set_layer_capture_mask(uint32_t mask);
uint32_t saturn_lc_get_layer_capture_mask(void);

#ifdef __cplusplus
}
#endif
