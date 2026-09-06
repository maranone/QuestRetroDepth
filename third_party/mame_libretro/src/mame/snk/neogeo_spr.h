// license:BSD-3-Clause
// copyright-holders:Bryan McPhail,Ernesto Corvi,Andrew Prime,Zsolt Vasvari
// thanks-to:Fuzz
#ifndef MAME_SNK_NEOGEO_SPR_H
#define MAME_SNK_NEOGEO_SPR_H

#pragma once

#include <array>
#include <deque>

// todo, move these back, currently the sprite code needs some of the values tho
#define NEOGEO_HTOTAL                           (0x180)
#define NEOGEO_HBEND                            (0x01c) // verified from https://wiki.neogeodev.org/index.php?title=Display_timing
#define NEOGEO_HBSTART                          (0x15c)
#define NEOGEO_VTOTAL                           (0x108)
#define NEOGEO_VBEND                            (0x010)
#define NEOGEO_VBSTART                          (0x0f0)
#define NEOGEO_VSSTART                          (0x100)

// todo, sort out what needs to be public and make the rest private/protected
class neosprite_base_device : public device_t, public device_video_interface
{
public:
	// fix layer bankswitch type
	enum
	{
		FIX_BANKTYPE_STD = 0,
		FIX_BANKTYPE_GAROU = 1,
		FIX_BANKTYPE_KOF2000 = 2
	};

	virtual void optimize_sprite_data();
	virtual void set_optimized_sprite_data(u8* sprdata, u32 mask);

	void draw_fixed_layer(bitmap_rgb32 &bitmap, int scanline);
	void draw_sprites(bitmap_rgb32 &bitmap, int scanline);

	// --- RetroDepth synthesized z-buffer ---
	// Neo Geo has no tilemap planes to capture (neogeo_v.cpp's screen_update is
	// just backdrop fill + sprites + fix layer), so depth is fabricated instead:
	// every sprite pixel also records which parallax plane its sprite belongs to,
	// producing a per-pixel depth channel shaped like the real one snes9x reports.
	// rd_set_depth_target() must be called before the frame's first draw_sprites()
	// and cleared (nullptr) afterwards; rd_compute_slot_depths() rebuilds the
	// slot->plane table once per frame.
	void rd_set_depth_target(u8 *base, int stride) { m_rd_depth_base = base; m_rd_depth_stride = stride; }
	void rd_compute_slot_depths();
	// --- RetroDepth capped true independent per-object capture ---
	// The shared z-buffer above only tags which depth a pixel's WINNER
	// belongs to -- when a later sprite draws over an earlier one, the
	// earlier sprite's color is gone from the one shared `bitmap`, and
	// slicing by z afterward can't recover it (the "occlusion hole" bug).
	//
	// Grouping strategy: COLUMN OCCUPANCY, not palette or draw order. The
	// screen is divided into 32 columns of 16px each (the hardware's own
	// sprite-column granularity -- see the RUN comment below). Each column
	// has a "level" counter, reset to 0 every frame. When a run (one head +
	// its chained columns, see below) is about to draw, its capture slot is
	// 1 + the HIGHEST level among every column it touches -- i.e. "how many
	// things have already been drawn in any column I overlap" -- and every
	// column it touches is then bumped to that new level. This directly
	// targets the actual occlusion-hole condition (does this object's
	// footprint overlap something already captured?) instead of using a
	// proxy for "probably a different object":
	//   - Two objects that never share a column (e.g. HUD digit vs. a
	//     character elsewhere) can both land in the lowest level/slot --
	//     no wasted buckets.
	//   - Two objects that genuinely overlap always land in different
	//     slots, regardless of palette, guaranteeing no hole between them.
	// It's column-granularity, not full 2D -- it ignores whether the two
	// objects' Y ranges actually intersect within a shared column, so it
	// can occasionally open a slot that wasn't strictly necessary. It will
	// never under-separate two overlapping objects, which is the property
	// that actually matters here.
	//
	// Three things tried before landing here, in order, each discarded:
	//   - Z-range "buckets" (32 fixed depth ranges): rendered black
	//     on-device for an unresolved reason.
	//   - First N DISTINCT OBJECTS in draw order (one capture slot per
	//     object): rendered as vertical bars building up left-to-right,
	//     because Neo Geo has no real BG tilemap -- every background is
	//     itself built from hundreds of small sprite tiles, drawn
	//     first/lowest-slot, so "first N objects" just captured N
	//     background tiles one at a time instead of anything meaningful.
	//   - Grouping by PALETTE BANK (every sprite sharing a bank -> one
	//     plane): fixed the vertical-bar problem (whole characters/bg
	//     tile-sets stayed together), but two genuinely unrelated,
	//     non-overlapping objects that merely shared a bank were forced
	//     into the same plane, and -- more importantly -- two DIFFERENT-
	//     palette objects that DID overlap were never guaranteed
	//     separation, since palette number has nothing to do with whether
	//     two objects' pixels actually collide.
	//
	// RUN = one head slot (y_control bit 6 clear) plus every immediately-
	// following chained slot (bit 6 set) -- the same grouping the hardware
	// itself uses to place a wide sprite's 16px-wide columns side by side
	// (see draw_sprites()'s x accumulation). rd_compute_slot_depths()
	// mirrors that same x/zoom bookkeeping to find each slot's column,
	// buffers the current run's columns+slot-numbers, and commits the
	// run's decision (one shared capture slot for every column in it) the
	// moment the NEXT head is reached -- so a wide/tall sprite is still
	// never torn across depths, exactly like m_rd_slot_z's inheritance.
	static constexpr int RD_DRAW_CAP = 30;
	// OCCUP uses a rolling history at the Neo Geo frame rate.  The
	// history stores occupancy by raw collision level, not by VRAM slot: slot
	// numbers are recycled by the hardware and are therefore unsafe object IDs.
	static constexpr int RD_OCCUP_HISTORY_FRAMES = 60;
	// OCCUPUD is the hysteresis variant: a raw level must remain supported for
	// this many decisions before earning a layer, while an existing layer gets
	// a longer grace period before it is removed.
	static constexpr int RD_OCCUPUD_PROMOTE_FRAMES = 1;
	static constexpr int RD_OCCUPUD_DEMOTE_FRAMES = 60;
	static constexpr int RD_OCCUPUD_MAPPING_FRAMES = 60;
	// COUNTER mode deliberately exports a fixed layer budget while we validate
	// the slot-order grouping. Empty planes are harmless and keep QRD's depth
	// stack from changing size when the Neo Geo list has a companion state.
	static constexpr int RD_COUNTER_FIXED_LAYERS = 9;
	static constexpr u8 RD_CAPTURE_UNASSIGNED = 0xFF;
	static constexpr int RD_CAPTURE_COLUMNS = 32; // 0x200 (max x) >> 4
	static constexpr int RD_CAPTURE_MAX_RUN_LEN = 64; // generous vs. any real chain length
	// The collision level (1..RD_DRAW_CAP) that claimed each capture slot
	// this frame, pre-scaled into an 8-bit z_order (1..254, leaving 0 for
	// "neogeo_base"/farthest and 255 for "neogeo_fix"/nearest) -- valid for
	// b < m_rd_capture_next. neogeo_v.cpp reads this to give each exported
	// "neogeo_drawN" layer a real depth ordering: a column-collision level
	// is itself a solid front-to-back signal (a higher level only exists
	// because something else was already drawn under it in one of its
	// columns, i.e. it was drawn later -> hardware-guaranteed frontmost).
	u8 rd_capture_slot_zorder(int b) const {
		return (b >= 0 && b < RD_DRAW_CAP) ? m_rd_capture_slot_zorder[b] : 0;
	}
	// The raw VRAM slot number of whichever run FIRST claimed this bucket
	// this frame (i.e. its head slot -- see the RUN comment above). A
	// bucket can be shared by several non-overlapping same-level runs (see
	// the per-level bucket reuse comment above), so this is only ONE
	// representative slot, not an exhaustive list -- good enough as a
	// debug label to correlate what's on screen with the real hardware
	// slot driving it, which "which bucket index (0..RD_DRAW_CAP-1) got
	// exported as neogeo_drawN" on its own does NOT tell you.
	u16 rd_capture_slot_headnum(int b) const {
		return (b >= 0 && b < RD_DRAW_CAP) ? m_rd_capture_slot_headnum[b] : 0;
	}
	// How many buckets were actually claimed THIS frame (0..RD_DRAW_CAP) --
	// buckets b >= this were never written to level_bucket[] this frame, so
	// their z_order/headnum are stale leftovers from whenever they were last
	// used and their capture pixels are all-transparent. neogeo_v.cpp's
	// export loop must stop here, not at the fixed RD_DRAW_CAP, or every
	// frame exports a full 30 "neogeo_drawN" layers regardless of how many
	// are actually in use -- exactly the empty/ghost-labelled layers the
	// hysteresis ratchet makes more common (merging more runs into fewer
	// buckets leaves more buckets unclaimed).
	int rd_capture_active_count() const { return m_rd_capture_next; }
	// OCCUPXY's draw-time pixel experiment: claim a compact capture bucket for
	// a mapped per-pixel overdraw level as soon as that level is encountered.
	u8 rd_claim_pixel_capture_bucket(u8 mapped_level, u16 headnum_hint) {
		if (mapped_level < 1) mapped_level = 1;
		if (mapped_level > RD_DRAW_CAP) mapped_level = RD_DRAW_CAP;
		if (m_rd_occupxy_bucket_for_level[mapped_level] == RD_CAPTURE_UNASSIGNED &&
			m_rd_capture_next < RD_DRAW_CAP)
		{
			const u8 bucket = (u8)m_rd_capture_next++;
			m_rd_occupxy_bucket_for_level[mapped_level] = bucket;
			m_rd_capture_slot_zorder[bucket] =
				(u8)(1 + ((uint32_t)(mapped_level - 1) * 253) / (RD_DRAW_CAP - 1));
			m_rd_capture_slot_headnum[bucket] = headnum_hint;
		}
		return m_rd_occupxy_bucket_for_level[mapped_level];
	}
	// True when COUNTER rejected this frame's candidate layer publication as
	// a one-frame lower/empty companion state.
	bool rd_counter_hold_frame() const { return m_rd_counter_hold_frame; }
	// Hysteresis ratchet window (frames) -- see the big comment on
	// m_rd_slot_allowed_level below. It is fixed by neogeo_v.cpp in the
	// hardcoded renderer path.
	int rd_hysteresis_window_frames() const { return m_rd_hysteresis_window_frames; }
	void rd_set_hysteresis_window(int frames) {
		if (frames < 1) frames = 1;
		if (frames > 600) frames = 600;
		m_rd_hysteresis_window_frames = frames;
	}

	// --- Layer-composition mode: which algorithm collapses Table 1's raw,
	// stateless per-frame collision levels down into the final bucket count.
	// Different strategies remain compiled into the renderer for experiments;
	// the active production path is selected by the hardcoded mode in
	// neogeo_v.cpp.
	enum class RdLayerMode : int
	{
		Deque = 0,   // existing temporal per-slot streak/high-water-mark ratchet
		HardQ,       // fixed even quantization bands, no state, hard cap
		Occup,       // occupancy-weighted merge of sparse raw levels, dynamic count capped at N
		DenseR,      // dense-rank remap of the distinct raw levels present this frame, capped at N
		MinGate,     // minimum run-size gate: small runs collapse into nearest kept level
		TileOcc,     // Table 1's column occupancy, but resolved into a real (column, 16px tile-row)
		             // grid instead of one strip spanning the whole screen height -- see the big
		             // comment on TileOcc's Stage 2 case in rd_compute_slot_depths() for why a
		             // chained object's true footprint is exact here, no per-pixel sampling needed
		TileOccPer,  // same grid as TileOcc, but a tile only "claims" its cell once its own opaque
		             // pixel count crosses a percentage threshold (50%) instead of just being
		             // non-empty -- filters out the sliver overlaps that split one visual object
		             // across two layers when a mostly-transparent tile edge grazes a neighbor
		TileOccGraph,// merges independent runs into same-object groups (union-find over a dilated
		             // occupancy grid, 1-tile-gap tolerance) BEFORE assigning depth, then levels
		             // those groups via a real overlap graph + topological longest-path instead of
		             // a running per-cell maximum -- fixes both "one object split across runs" and
		             // "transitive stacking through an unrelated bridging object"
		StableGraph, // TileOccGraph's spatial ordering with one-frame object matching and a
		             // bounded +/-1-level temporal deadband to suppress animation jitter
		Painter,     // layers assembled straight from hardware draw order: consecutive small
		             // runs first chain into one OBJECT (atomic placement -- a character can
		             // never be torn across layers or mixed into another object's bucket),
		             // then objects are placed in slot order against a stack of layer
		             // footprints: overlapping an existing layer opens/joins the next-nearer
		             // one, a burst's shrapnel is one object = one layer, and a per-object
		             // anti-flash ratchet (instant up, slow confirmed down, blink-tolerant)
		             // keeps assignments from strobing frame to frame
		Counter,      // full-frame counter grouping from the slot analysis: a new layer
		             // starts when both the screen-X counter and the sprite-number jump
		             // counter change together; empty and one-layer companion states are
		             // discarded after a richer frame has been established
		Code,         // Counter with an adjacent same-tile-code guard: a candidate
		             // boundary cannot split a contiguous run using the same tile code
		OccupUd,      // OCCUP with per-level promotion/demotion hysteresis
		OccupXY,      // OCCUPUD using the 16px X/Y occupancy grid instead of
		             // the original full-height X-column occupancy
		Count
	};
	// OCCUPXY uses compact sprite layers 0..5.  Layer 6 is reserved by the
	// application for the separately exported neogeo_fix plane.
	static constexpr int kRdLayerModeMaxBuckets = 6; // cap for HardQ/Occup/DenseR/MinGate/OccupXY
	RdLayerMode rd_layer_mode() const { return m_rd_layer_mode; }
	const char *rd_layer_mode_name() const {
		switch (m_rd_layer_mode) {
			case RdLayerMode::Deque:   return "DEQUE";
			case RdLayerMode::HardQ:   return "HARDQ";
			case RdLayerMode::Occup:   return "OCCUP";
			case RdLayerMode::DenseR:  return "DENSER";
			case RdLayerMode::MinGate: return "MINGATE";
			case RdLayerMode::TileOcc:      return "TILEOCC";
			case RdLayerMode::TileOccPer:   return "TILEOCCPER";
			case RdLayerMode::TileOccGraph: return "TILEOCCGRAPH";
			case RdLayerMode::StableGraph:  return "STABLEGRAPH";
			case RdLayerMode::Painter:      return "PAINTER";
			case RdLayerMode::Counter:      return "COUNTER";
			case RdLayerMode::Code:         return "CODE";
			case RdLayerMode::OccupUd:      return "OCCUPUD";
			case RdLayerMode::OccupXY:      return "OCCUPXY";
			default:                        return "?";
		}
	}
	void rd_set_layer_mode(int mode) {
		if (mode < 0) mode = 0;
		if (mode >= (int)RdLayerMode::Count) mode = (int)RdLayerMode::Count - 1;
		const RdLayerMode next = (RdLayerMode)mode;
		if (next != m_rd_layer_mode) {
			// A layer level from another composition algorithm is not a valid
			// temporal anchor for StableGraph. Start that mode from a clean
			// spatial graph, then begin matching on its next frame.
			m_rd_stable_previous_objects.clear();
			m_rd_stable_previous_frame = 0;
			// Counter mode has its own one-frame confirmation state. Do not let
			// a slot's group from another mode become an accidental anchor when
			// the user cycles into COUNTER.
			std::fill(std::begin(m_rd_counter_previous_group), std::end(m_rd_counter_previous_group), 0);
			std::fill(std::begin(m_rd_counter_stable_group), std::end(m_rd_counter_stable_group), 0);
			std::fill(std::begin(m_rd_counter_slot_last_seen_frame), std::end(m_rd_counter_slot_last_seen_frame), 0);
			m_rd_counter_published_layer_count = 0;
			m_rd_counter_low_frame_streak = 0;
			m_rd_counter_hold_frame = false;
			m_rd_occup_history.clear();
			m_rd_occup_weight_sum.fill(0);
			m_rd_occup_level_active.fill(0);
			m_rd_occup_promote_streak.fill(0);
			m_rd_occup_demote_streak.fill(0);
		}
		m_rd_layer_mode = next;
	}
	int rd_cycle_layer_mode() {
		m_rd_layer_mode = (RdLayerMode)(((int)m_rd_layer_mode + 1) % (int)RdLayerMode::Count);
		return (int)m_rd_layer_mode;
	}
	// base points at RD_DRAW_CAP consecutive planes, each width*height
	// pixels (capture_plane_stride apart), row-major within a plane
	// (stride = row pitch in pixels). Same full-canvas-per-slot shape as
	// the bucket experiment this replaces -- already proven not to hurt
	// FPS at 32 planes, so 30 is comfortably cheap too.
	void rd_set_capture_target(u32 *base, int stride, int capture_plane_stride) {
		m_rd_capture_base = base; m_rd_capture_stride = stride; m_rd_capture_plane_stride = capture_plane_stride;
	}

	// Depth values are a u8 (0-255); questretrodepth's dynamic z-split scan
	// (find_occupied_z() in game_config.cpp) walks the full range and
	// creates one real layer per DISTINCT value it finds this window -- so
	// app-side layer count is NOT capped here, it just follows however many
	// distinct z's actually get stamped.
	//
	// RD_Z_SOURCE_DRAW_ORDER selects WHICH signal a sprite's z comes from
	// (see rd_compute_slot_depths() in neogeo_spr.cpp):
	//   true  (default) -- draw order / VRAM slot number. Neo Geo has no real
	//                       depth buffer, but every game MUST draw its
	//                       sprites back-to-front (painter's algorithm) for
	//                       compositing to look right at all -- so "later
	//                       slot = drawn later = in front" is a hardware-
	//                       guaranteed invariant true for EVERY game, unlike
	//                       palette bank number (see below).
	//   false -- palette bank. A sprite's own palette index (0-255) scaled
	//            into the sprite z range. Only correlates with actual visual
	//            depth by coincidence of how a given game's artists happened
	//            to number their palette banks -- confirmed to require the
	//            OPPOSITE mapping direction on different games (Metal Slug
	//            vs KOF97), since palette numbering is an arbitrary
	//            per-game art-pipeline convention, not something the
	//            hardware enforces. Kept only for quick comparison/revert.
	static constexpr bool RD_Z_SOURCE_DRAW_ORDER = true;
	// RD_USE_FULL_PALETTE_RESOLUTION selects between two RANGES for the
	// palette-based mapping above (only relevant when RD_Z_SOURCE_DRAW_ORDER
	// is false) -- see rd_compute_slot_depths():
	//   true  (default) -- 1:1 mode: each sprite's own palette bank (0-255)
	//                       is scaled into a near-full-resolution sprite z
	//                       range, so distinct palettes land on distinct z's
	//                       almost everywhere -- "an exact replica of where
	//                       in z each palette goes".
	//   false -- legacy banded mode: palette scaled into a much narrower
	//            49-value range, collapsing many palettes onto shared z
	//            bands. Kept only so this can be reverted quickly if the
	//            1:1 mapping looks worse on some games.
	static constexpr bool RD_USE_FULL_PALETTE_RESOLUTION = true;

	static constexpr u8  RD_Z_BACKDROP    = 2;
	static constexpr u8  RD_Z_FIX         = 255;
	static constexpr u8  RD_Z_SPRITE_MIN  = 3;
	static constexpr u8  RD_Z_SPRITE_MAX_FULL   = 254; // 1:1 mode
	static constexpr u8  RD_Z_SPRITE_MAX_BANDED = 51;  // legacy banded mode
	static constexpr u8  RD_Z_SPRITE_MAX  = RD_USE_FULL_PALETTE_RESOLUTION
	                                             ? RD_Z_SPRITE_MAX_FULL
	                                             : RD_Z_SPRITE_MAX_BANDED;
	static constexpr int RD_SPRITE_PLANES = RD_Z_SPRITE_MAX - RD_Z_SPRITE_MIN + 1;

	void set_videoram_offset(u16 data);
	u16 get_videoram_data();
	void set_videoram_data(u16 data);
	void set_videoram_modulo(u16 data);
	u16 get_videoram_modulo();

	void set_auto_animation_speed(u8 data);
	void set_auto_animation_disabled(u8 data);
	u8 get_auto_animation_counter();

	void set_fixed_layer_source(u8 data);
	void set_fixed_layer_bank_type(u8 data);

	virtual void set_sprite_region(u8* region_sprites, u32 region_sprites_size);
	void set_fixed_regions(u8* fix_cart, u32 fix_cart_size, memory_region* fix_bios);
	void set_pens(const pen_t* pens);

protected:
	neosprite_base_device(
			const machine_config &mconfig,
			device_type type,
			const char *tag,
			device_t *owner,
			u32 clock);

	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

	void create_auto_animation_timer();
	void start_auto_animation_timer();

	void create_sprite_line_timer();
	void start_sprite_line_timer();

	void parse_sprites(int scanline);

	inline bool sprite_on_scanline(int scanline, int y, int rows);

	// Returns true when a pixel was actually written (i.e. the source pixel was
	// not transparent), so draw_sprites() knows whether to record depth for it.
	virtual bool draw_pixel(int romaddr, u32* dst, const pen_t *line_pens) = 0;

	virtual void draw_fixed_layer_2pixels(u32*&pixel_addr, int offset, u8* gfx_base, const pen_t* char_pens);

	TIMER_CALLBACK_MEMBER(auto_animation_timer_callback);
	TIMER_CALLBACK_MEMBER(sprite_line_timer_callback);

	u32 get_region_mask(u8* rgn, u32 rgn_size);

	u8* m_region_sprites = nullptr; u32 m_region_sprites_size = 0;
	u8* m_region_fixed = nullptr; u32 m_region_fixed_size = 0;
	memory_region* m_region_fixedbios = nullptr;
	const pen_t   *m_pens = nullptr;

	std::unique_ptr<u16[]>     m_videoram;
	u16     *m_videoram_drawsource = nullptr;

	u16     m_vram_offset = 0;
	u16     m_vram_read_buffer = 0;
	u16     m_vram_modulo = 0;

	u32     m_sprite_gfx_address_mask = 0;

	u8      m_auto_animation_speed = 0;
	u8      m_auto_animation_disabled = 0;
	u8      m_auto_animation_counter = 0;
	u8      m_auto_animation_frame_counter = 0;

	u8      m_fixed_layer_source = 0;
	u8      m_fixed_layer_bank_type = 0;

	emu_timer  *m_auto_animation_timer = nullptr;
	emu_timer  *m_sprite_line_timer = nullptr;

	int m_bppshift; // 4 for 4bpp gfx (NeoGeo) 8 for 8bpp gfx (Midas)

	required_region_ptr<u8> m_region_zoomy;

	// RetroDepth z-buffer target for the frame in progress (nullptr = disabled),
	// plus the per-slot plane table rd_compute_slot_depths() fills in. Indexed by
	// VRAM sprite slot, which parse_sprites() walks in ascending order -- so slot
	// number is both the hardware's draw order and a stable per-object identity
	// across frames.
	u8 *m_rd_depth_base = nullptr;
	int m_rd_depth_stride = 0;
	u8  m_rd_slot_z[381] = {};

	// See rd_set_capture_target()/rd_claim_capture_slot() above.
	u32 *m_rd_capture_base = nullptr;
	int m_rd_capture_stride = 0;
	int m_rd_capture_plane_stride = 0;
	u8  m_rd_capture_slot[381] = {};
	int m_rd_capture_next = 0;
	u8  m_rd_capture_slot_zorder[RD_DRAW_CAP] = {};
	u16 m_rd_capture_slot_headnum[RD_DRAW_CAP] = {};
	u8  m_rd_occupxy_bucket_for_level[RD_DRAW_CAP + 1] = {};
	static constexpr int RD_OCCUPXY_PIXEL_TRACK_SIZE = 512 * 512;
	std::array<u8, RD_OCCUPXY_PIXEL_TRACK_SIZE> m_rd_occupxy_pixel_draw_depth{};
	std::array<u8, RD_DRAW_CAP + 1> m_rd_occupxy_level_map{};

	// Hysteresis RATCHET (3rd design -- two TABLES instead of one smoothed
	// value. The first design, a per-slot "floor" resisting only moving
	// nearer, couldn't merge two slots of the same character that simply
	// never computed the same raw level to begin with. The second, a
	// staircase that promoted one level at a time, fixed that but still
	// smoothed a slot's OWN computed level, which turned out fragile: a
	// slot reappearing after a brief absence reads a fresh raw level with no
	// memory behind it, so even with symmetric proof on both directions the
	// mechanism was still fighting its own signal. A later attempt (a plain
	// climb from level 1, one step per proven window) was tried and rolled
	// back per explicit request in favor of restoring this design.
	//
	// This design cleanly separates the two questions instead:
	//   TABLE 1 -- "what level does this slot's geometry say it's on RIGHT
	//   NOW" -- raw_level, computed exactly like the original design with NO
	//   smoothing at all (col_level below is bumped with the raw level, not
	//   any ratcheted one) -- a pure, stateless, per-frame signal.
	//   TABLE 2 -- "has this slot proven it consistently belongs at the
	//   level Table 1 says" -- m_rd_slot_want_level/m_rd_slot_want_streak
	//   track, per slot, a HIGH-WATER MARK of the deepest raw level it's
	//   asked for and how many consecutive frames it's remained present
	//   (not how many frames it asked for that EXACT value -- a blinking
	//   sprite whose raw level oscillates frame to frame keeps its streak
	//   alive as long as it never exceeds the mark by more than a step up,
	//   instead of resetting to 1 on every wobble and permanently living in
	//   the unproven fallback path).
	//
	// Each frame, a run's actual rendered (bucket-assigned) level is decided
	// by Table 2's verdict on its Table 1 request:
	//   - Streak >= RD_HYSTERESIS_WINDOW_FRAMES: it's WON the level outright
	//     -- render at raw_level, and this frame that level counts as
	//     already proven for anyone else.
	//   - Streak not yet enough, but SOME OTHER slot has already won this
	//     exact level earlier in this same frame's processing: allowed to
	//     join it anyway -- the level's legitimacy this frame was already
	//     established, no need to independently re-prove something another
	//     slot already earned (this is what lets a whole group of slots
	//     belonging to one object converge onto the same level quickly once
	//     any one of them has proven it, without everyone paying the full
	//     window individually).
	//   - Otherwise: not entitled to raw_level yet, and nobody's vouching
	//     for it this frame either -- falls back to ONE level short of what
	//     it actually wants (its own target, not the frame's ceiling),
	//     further capped by the ceiling so it can never land on/past a
	//     level that hasn't legitimately been established by an actual
	//     winner or piggybacker.
	//
	// A slot number is only a reliable per-object identity for as long as
	// it's continuously drawn: m_rd_frame_counter/m_rd_slot_last_seen_frame
	// detect ANY gap at all (even one missed frame) between this slot
	// number being drawn -- since the hardware is free to repurpose an
	// inactive slot number for a totally different sprite with no signal to
	// this code that identity changed, any gap wipes want_level/want_streak
	// immediately, so the streak requires a genuinely unbroken run of
	// consecutive frames rather than merely "recently active".
	// See rd_hysteresis_window_frames()/rd_set_hysteresis_window() above.
	int m_rd_hysteresis_window_frames = 30;
	u8  m_rd_slot_want_level[381] = {}; // 0 = not yet seen; high-water mark of the raw level being proven
	u16 m_rd_slot_want_streak[381] = {}; // consecutive frames continuously present while proving want_level
	u32 m_rd_frame_counter = 0;
	u32 m_rd_slot_last_seen_frame[381] = {};
	RdLayerMode m_rd_layer_mode = RdLayerMode::OccupXY;

	// OCCUP temporal smoothing. Each entry contains the current frame's member
	// slot count for raw collision levels 1..RD_DRAW_CAP. Keeping the sum beside
	// the deque makes the per-frame update O(RD_DRAW_CAP), without rescanning
	// all recent frames. The resulting historical level support is used to decide
	// which compressed layer ranks exist, while the current frame still supplies
	// the pixels assigned to those ranks.
	std::deque<std::array<uint16_t, RD_DRAW_CAP + 1>> m_rd_occup_history;
	std::array<uint32_t, RD_DRAW_CAP + 1> m_rd_occup_weight_sum{};
	std::array<uint8_t, RD_DRAW_CAP + 1> m_rd_occup_level_active{};
	std::array<uint8_t, RD_DRAW_CAP + 1> m_rd_occup_promote_streak{};
	std::array<uint8_t, RD_DRAW_CAP + 1> m_rd_occup_demote_streak{};
	// OCCUPUD's simple temporal mapping hold. A changed raw-level mapping is
	// not accepted until it has remained unchanged for a full confirmation
	// window; this stabilizes layer content without pretending raw levels are
	// permanent object IDs.
	std::array<uint8_t, RD_DRAW_CAP + 1> m_rd_occupud_stable_mapping{};
	std::array<uint8_t, RD_DRAW_CAP + 1> m_rd_occupud_mapping_candidate{};
	std::array<uint8_t, RD_DRAW_CAP + 1> m_rd_occupud_mapping_streak{};

	// Counter mode's one-frame confirmation state. The candidate group is the
	// cumulative both_changed counter from the current full-frame slot walk.
	// A slot adopts a changed group only after the same candidate is observed on
	// two consecutive frames; until then it keeps its last confirmed group.
	u8  m_rd_counter_previous_group[381] = {};
	u8  m_rd_counter_stable_group[381] = {};
	u32 m_rd_counter_slot_last_seen_frame[381] = {};
	int m_rd_counter_published_layer_count = 0;
	int m_rd_counter_low_frame_streak = 0;
	bool m_rd_counter_hold_frame = false;

	// StableGraph's deliberately short-lived temporal anchors. These are
	// object footprints, not raw VRAM-slot levels: the graph is allowed to
	// split/merge runs and then only uses the previous level to damp tiny
	// frame-to-frame changes. State is discarded on mode changes or gaps.
	struct RdStableObject
	{
		std::array<u32, 32> bits{};
		u16 head_slot = 0;
		u8 level = 0;
	};
	std::vector<RdStableObject> m_rd_stable_previous_objects;
	u32 m_rd_stable_previous_frame = 0;

	// Per-slot palette index (attr >> 8, the same 8-bit palette bank used for
	// pen lookup), refreshed every time the slot is actually drawn. This is
	// the ONLY signal depth is derived from: rd_compute_slot_depths() maps
	// each slot's own palette directly and statelessly into a z value --
	// z = f(palette), a fixed proportional scale, no history, no windowing,
	// no per-slot memory. Palette 200 always maps to the same z whether it's
	// the only sprite on screen or one of 300, and whether this is frame 1
	// or frame 100000 -- the mapping doesn't depend on anything except the
	// palette number itself, so there is nothing for it to reorder/flicker.
	u8  m_rd_slot_palette[381] = {};
};


class neosprite_regular_device : public neosprite_base_device
{
public:
	neosprite_regular_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);
	virtual bool draw_pixel(int romaddr, u32* dst, const pen_t *line_pens) override;
	virtual void set_sprite_region(u8* region_sprites, u32 region_sprites_size) override;

};

DECLARE_DEVICE_TYPE(NEOGEO_SPRITE_REGULAR, neosprite_regular_device)


class neosprite_optimized_device : public neosprite_base_device
{
public:
	neosprite_optimized_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);
	virtual void optimize_sprite_data() override;
	virtual void set_optimized_sprite_data(u8* sprdata, u32 mask) override;
	virtual bool draw_pixel(int romaddr, u32* dst, const pen_t *line_pens) override;
	std::vector<u8> m_sprite_gfx;
	u8* m_spritegfx8;

private:
	u32 optimize_helper(std::vector<u8> &spritegfx, u8* region_sprites, u32 region_sprites_size);
};

DECLARE_DEVICE_TYPE(NEOGEO_SPRITE_OPTIMZIED, neosprite_optimized_device)


class neosprite_midas_device : public neosprite_base_device
{
public:
	neosprite_midas_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

	virtual bool draw_pixel(int romaddr, u32* dst, const pen_t *line_pens) override;

	std::unique_ptr<u16[]> m_videoram_buffer;
	void buffer_vram();
	virtual void draw_fixed_layer_2pixels(u32*&pixel_addr, int offset, u8* gfx_base, const pen_t* char_pens) override;
	virtual void set_sprite_region(u8* region_sprites, u32 region_sprites_size) override;

	protected:
	virtual void device_start() override ATTR_COLD;

};

DECLARE_DEVICE_TYPE(NEOGEO_SPRITE_MIDAS, neosprite_midas_device)

#endif // MAME_SNK_NEOGEO_SPR_H
