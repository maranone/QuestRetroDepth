// neogeo_palette_debug.h — one-shot diagnostic capture for the Neo Geo
// palette->depth pipeline: arms automatically when a Metal Slug ROM loads,
// watches 60 seconds of real gameplay for the frame with the most distinct
// active sprite palettes, then dumps one JPG swatch per palette bank present
// that frame to <sdcard root>/mame/qrd_temp/, so they can be pulled to a PC
// and eyeballed (see mame_retrodepth_hook.h's g_active_palettes/g_palette_argb
// and neogeo_spr.cpp's rd_compute_slot_depths() for where the source data
// comes from).
#pragma once

#include <string>

#include "emulator_backend.h"

namespace qrd {

// Call once per ROM load (MameBackend::load_content). No-op unless rom_path's
// filename looks like a Metal Slug set (mslug*).
void neogeo_palette_debug_maybe_arm(const std::string& rom_path);

// Call once per newly-committed MAME frame (MameBackend::pull_named_layers,
// after pull_synthesized_zbuffer() has populated frame.zbuffer), passing the
// composite frame just produced. No-op unless a capture window is currently
// armed.
void neogeo_palette_debug_tick(const FrameOutput& frame);

} // namespace qrd
