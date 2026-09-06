// mame_layer_capture.h — render-thread read side for MAME layer export.
// Write side (called from MAME driver code) is mame_retrodepth_hook.h.
//
// Mirrors the existing per-core capture accessor pattern in this project
// (snes9x_layer_capture.h, mgba_layer_capture.h, pce_layer_capture.h) but
// keyed by name/z_order instead of a fixed small enum, since MAME layers
// vary per driver (CPS1/CPS2: background/scroll3/scroll2/scroll1/sprites;
// Konami: scroll2/scroll1/scroll0/sprites; NeoGeo: background/grp0-3/fix).

#pragma once
#include <cstdint>

extern "C" {

// Number of layers written by MAME for the frame most recently committed.
int mame_layer_count();

// Layer name at index (e.g. "background", "scroll1", "sprites"), valid
// until the next retrodepth_commit() call on the emulator thread.
const char* mame_layer_name(int index);

// Draw order: 0 = furthest back.
uint32_t mame_layer_z_order(int index);

// ARGB8888 pixel buffer for the layer at index. Returns nullptr if index is
// out of range or the layer has no pixel data yet.
const uint32_t* mame_layer_pixels(int index, uint32_t* out_width, uint32_t* out_height);

// Per-pixel palette-owner ids for the layer at index (used for sprite/tile
// palette-group routing, e.g. NeoGeo). Returns nullptr when the layer has no
// owner data (e.g. CPS1/CPS2's opaque "background" fill).
const uint16_t* mame_layer_owners(int index);

// Increments every time retrodepth_commit() runs. Compare against a
// previously-seen value to detect a new frame is ready.
uint32_t mame_frame_id();

// Synthesized per-pixel depth (0..63), for drivers with no separable hardware
// layers to export. Neo Geo is the only one: its whole display is a backdrop
// fill plus one sprite pass plus the fix layer, so neogeo_v.cpp fabricates a
// depth channel shaped like the real one snes9x reports and the app feeds it
// through the same FrameOutput::zbuffer path. Returns nullptr with zeroed
// dimensions for every driver that exports named layers instead.
const uint8_t* mame_zbuffer(uint32_t* out_width, uint32_t* out_height);

// Generic MAME OCCUPXY capture from the shared drawgfx path. Buckets are
// ordered far-to-near and use logical ARGB8888 (0xAARRGGBB).
void mame_occupancy_set_enabled(int enabled);
int mame_occupancy_enabled();
int mame_occupancy_available();
int mame_occupancy_valid();
int mame_occupancy_bucket_count();
const uint32_t* mame_occupancy_bucket_pixels(int bucket,
                                             uint32_t* out_width,
                                             uint32_t* out_height);
uint32_t mame_occupancy_draw_count();
uint32_t mame_occupancy_pixel_count();

// Debug-only (Neo Geo, see neogeo_palette_debug.cpp): the raw resolved
// palette table (256 banks * 16 colors, ARGB8888), and which of the 256
// banks had an active sprite this frame. Not called by the normal render
// path. Only defined/exported by the snk/ driver build -- nullptr/0 for
// every other MAME driver family.
const uint32_t* mame_palette_argb_table();
uint32_t mame_active_palette_count();
const uint8_t* mame_active_palette_list();

} // extern "C"
