// license:BSD-3-Clause
// copyright-holders:Bryan McPhail,Ernesto Corvi,Andrew Prime,Zsolt Vasvari
// thanks-to:Fuzz
/* NeoGeo sprites (and fixed text layer) */

#include "emu.h"
#include "neogeo_spr.h"
#include "screen.h"
#include "mame_retrodepth_hook.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif


// pure virtual functions
//const device_type NEOGEO_SPRITE_BASE = device_creator<neosprite_base_device>;

neosprite_base_device::neosprite_base_device(
		const machine_config &mconfig,
		device_type type,
		const char *tag,
		device_t *owner,
		u32 clock)
	: device_t(mconfig, type, tag, owner, clock)
	, device_video_interface(mconfig, *this)
	, m_bppshift(4)
	, m_region_zoomy(*this, "zoomy")
{
}

void neosprite_base_device::device_start()
{
	m_videoram = std::make_unique<u16[]>(0x8000 + 0x800);
	m_videoram_drawsource = m_videoram.get();

	/* clear allocated memory */
	memset(m_videoram.get(), 0x00, (0x8000 + 0x800) * sizeof(u16));

	create_sprite_line_timer();
	create_auto_animation_timer();

	/* initialize values that are not modified on a reset */
	m_vram_offset = 0;
	m_vram_read_buffer = 0;
	m_vram_modulo = 0;
	m_auto_animation_speed = 0;
	m_auto_animation_disabled = 0;
	m_auto_animation_counter = 0;
	m_auto_animation_frame_counter = 0;


	/* register for state saving */
	save_pointer(NAME(m_videoram), 0x8000 + 0x800);
	save_item(NAME(m_vram_offset));
	save_item(NAME(m_vram_read_buffer));
	save_item(NAME(m_vram_modulo));
	save_item(NAME(m_fixed_layer_source));

	save_item(NAME(m_auto_animation_speed));
	save_item(NAME(m_auto_animation_disabled));
	save_item(NAME(m_auto_animation_counter));
	save_item(NAME(m_auto_animation_frame_counter));
}

void neosprite_base_device::device_reset()
{
	//m_sprite_gfx_address_mask = 0;
	optimize_sprite_data();
	m_rd_occup_history.clear();
	m_rd_occup_weight_sum.fill(0);

	start_sprite_line_timer();
	start_auto_animation_timer();
}


/*************************************
 *
 *  Video RAM access
 *
 *************************************/

void neosprite_base_device::set_videoram_offset(u16 data)
{
	m_vram_offset = (data & 0x8000 ? data & 0x87ff : data);

	/* the read happens right away */
	m_vram_read_buffer = m_videoram[m_vram_offset];
}


u16 neosprite_base_device::get_videoram_data()
{
	return m_vram_read_buffer;
}


void neosprite_base_device::set_videoram_data(u16 data)
{
	m_videoram[m_vram_offset] = data;

	/* auto increment/decrement the current offset - A15 is NOT affected */
	set_videoram_offset((m_vram_offset & 0x8000) | ((m_vram_offset + m_vram_modulo) & 0x7fff));
}


void neosprite_base_device::set_videoram_modulo(u16 data)
{
	m_vram_modulo = data;
}


u16 neosprite_base_device::get_videoram_modulo()
{
	return m_vram_modulo;
}


/*************************************
 *
 *  Auto animation
 *
 *************************************/

void neosprite_base_device::set_auto_animation_speed(u8 data)
{
	m_auto_animation_speed = data;
}


void neosprite_base_device::set_auto_animation_disabled(u8 data)
{
	m_auto_animation_disabled = data;
}


u8 neosprite_base_device::get_auto_animation_counter()
{
	return m_auto_animation_counter;
}


TIMER_CALLBACK_MEMBER(neosprite_base_device::auto_animation_timer_callback)
{
	if (m_auto_animation_frame_counter == 0)
	{
		m_auto_animation_frame_counter = m_auto_animation_speed;
		m_auto_animation_counter += 1;
	}
	else
		m_auto_animation_frame_counter = m_auto_animation_frame_counter - 1;

	m_auto_animation_timer->adjust(screen().time_until_pos(NEOGEO_VSSTART));
}


void neosprite_base_device::create_auto_animation_timer()
{
	m_auto_animation_timer = timer_alloc(FUNC(neosprite_base_device::auto_animation_timer_callback), this);
}


void neosprite_base_device::start_auto_animation_timer()
{
	m_auto_animation_timer->adjust(screen().time_until_pos(NEOGEO_VSSTART));
}


/*************************************
 *
 *  Fixed layer
 *
 *************************************/

void neosprite_base_device::set_fixed_layer_source(u8 data)
{
	m_fixed_layer_source = data;
}


void neosprite_base_device::set_fixed_layer_bank_type(u8 data)
{
	m_fixed_layer_bank_type = data;
}


void neosprite_base_device::draw_fixed_layer(bitmap_rgb32 &bitmap, int scanline)
{
	assert((m_fixed_layer_source && m_region_fixed != nullptr) || (m_region_fixedbios != nullptr));

	u8* gfx_base = m_fixed_layer_source ? m_region_fixed : m_region_fixedbios->base();
	const u32 addr_mask = ( m_fixed_layer_source ? m_region_fixed_size : m_region_fixedbios->bytes() ) - 1;
	const u16 *video_data = &m_videoram_drawsource[0x7000 | (scanline >> 3)];
	u32 *pixel_addr = &bitmap.pix(scanline, NEOGEO_HBEND);

	int garouoffsets[34]{};
	const bool banked = m_fixed_layer_source && (addr_mask > 0x1ffff);

	/* thanks to Mr K for the garou & kof2000 banking info */
	/* Build line banking table for Garou & MS3 before starting render */
	if (banked && m_fixed_layer_bank_type == FIX_BANKTYPE_GAROU)
	{
		int garoubank = 0;
		int k = 0;
		int y = 0;
		while (y < 32)
		{
			if (m_videoram_drawsource[0x7500 + k] == 0x0200 && (m_videoram_drawsource[0x7580 + k] & 0xff00) == 0xff00)
			{
				garoubank = m_videoram_drawsource[0x7580 + k] & 3;
				garouoffsets[y++] = garoubank;
			}
			garouoffsets[y++] = garoubank;
			k += 2;
		}
	}

	for (int x = 0; x < 40; x++)
	{
		const u16 code_and_palette = *video_data;
		u16 code = code_and_palette & 0x0fff;

		if (banked)
		{
			int y = scanline >> 3;
			switch (m_fixed_layer_bank_type)
			{
			case FIX_BANKTYPE_GAROU:
				/* Garou, MSlug 3 */
				code += 0x1000 * (garouoffsets[(y - 2) & 31] ^ 3);
				break;
			case FIX_BANKTYPE_KOF2000:
				code += 0x1000 * (((m_videoram_drawsource[0x7500 + ((y - 1) & 31) + 32 * (x / 6)] >> (5 - (x % 6)) * 2) & 3) ^ 3);
				break;
			}
		}

		{
			const int gfx_offset = ((code << 5) | (scanline & 0x07)) & addr_mask;

			const pen_t *char_pens = &m_pens[code_and_palette >> 12 << m_bppshift];

			static const u32 pix_offsets[] = { 0x10, 0x18, 0x00, 0x08 };

			for (int i = 0; i < 4; i++)
			{
				draw_fixed_layer_2pixels(pixel_addr, gfx_offset + pix_offsets[i], gfx_base, char_pens);
			}
		}
		video_data = video_data + 0x20;
	}
}


inline void neosprite_base_device::draw_fixed_layer_2pixels(u32*&pixel_addr, int offset, u8* gfx_base, const pen_t* char_pens)
{
	const u8 data = gfx_base[offset];

	if (data & 0x0f)
		*pixel_addr = char_pens[data & 0x0f];
	pixel_addr++;

	if (data & 0xf0)
		*pixel_addr = char_pens[(data & 0xf0) >> 4];
	pixel_addr++;

}

/*************************************
 *
 *  Sprite hardware
 *
 *************************************/

static constexpr u32 MAX_SPRITES_PER_SCREEN = 381;
static constexpr u32 MAX_SPRITES_PER_LINE = 96;


/* horizontal zoom table - verified on real hardware */
static const u16 zoom_x_tables[16] =
{ 0x0080, 0x0880, 0x0888, 0x2888, 0x288a, 0x2a8a, 0x2aaa, 0xaaaa, 0xaaea, 0xbaea, 0xbaeb, 0xbbeb, 0xbbef, 0xfbef, 0xfbff, 0xffff };


inline bool neosprite_base_device::sprite_on_scanline(int scanline, int y, int rows)
{
	return (rows == 0) || (rows >= 0x20) || ((scanline - y) & 0x1ff) < (rows * 0x10);
}


void neosprite_base_device::draw_sprites(bitmap_rgb32 &bitmap, int scanline)
{
	int max_sprite_index;
	int y = 0;
	int x = 0;
	int rows = 0;
	int zoom_y = 0;
	int zoom_x = 0;
	u16 *sprite_list;

	/* select the active list */
	if (BIT(scanline, 0))
		sprite_list = &m_videoram_drawsource[0x8680];
	else
		sprite_list = &m_videoram_drawsource[0x8600];

	/* optimization -- find last non-zero entry and only draw that many +1
	   sprite.  This is not 100% correct as the hardware will keep drawing
	   the #0 sprite over and over, but we need the speed */
	for (max_sprite_index = (MAX_SPRITES_PER_LINE - 1); max_sprite_index >= 0; max_sprite_index--)
	{
		if (sprite_list[max_sprite_index] != 0)
			break;
	}

	/* add the +1 now, just in case the 0 at the end is real sprite */
	if (max_sprite_index != (MAX_SPRITES_PER_LINE - 1))
		max_sprite_index = max_sprite_index + 1;

	for (int sprite_index = 0; sprite_index <= max_sprite_index; sprite_index++)
	{
		const u16 sprite_number = sprite_list[sprite_index] & 0x01ff;
		const u16 y_control = m_videoram_drawsource[0x8200 | sprite_number];
		const u16 zoom_control = m_videoram_drawsource[0x8000 | sprite_number];

		/* if chained, go to next X coordinate and get new X zoom */
		if (BIT(y_control, 6))
		{
			x = (x + zoom_x + 1) & 0x01ff;
			zoom_x = (zoom_control >> 8) & 0x0f;
		}
		/* new block */
		else
		{
			y = 0x200 - (y_control >> 7);
			x = m_videoram_drawsource[0x8400 | sprite_number] >> 7;

			zoom_y = (zoom_control & 0xff);

			zoom_x = (zoom_control >> 8) & 0x0f;
			rows = y_control & 0x3f;
		}

		/* skip if falls completely outside the screen */
		if ((x >= 0x140) && (x <= 0x1f0))
			continue;

		/* double check the Y coordinate, in case somebody modified the sprite coordinate
		   since we buffered it */
		if (sprite_on_scanline(scanline, y, rows))
		{
			const int sprite_line = (scanline - y) & 0x1ff;
			int zoom_line = sprite_line & 0xff;
			bool invert = BIT(sprite_line, 8);

			if (invert)
				zoom_line ^= 0xff;

			if (rows > 0x20)
			{
				zoom_line = zoom_line % ((zoom_y + 1) << 1);

				if (zoom_line > zoom_y)
				{
					zoom_line = ((zoom_y + 1) << 1) - 1 - zoom_line;
					invert = !invert;
				}
			}

			const u8 sprite_y_and_tile = m_region_zoomy[(zoom_y << 8) | zoom_line];

			int sprite_y = sprite_y_and_tile & 0x0f;
			int tile = sprite_y_and_tile >> 4;

			if (invert)
			{
				sprite_y ^= 0x0f;
				tile ^= 0x1f;
			}

			const offs_t attr_and_code_offs = (sprite_number << 6) | (tile << 1);
			const u16 attr = m_videoram_drawsource[attr_and_code_offs + 1];
			u32 code = ((attr << 12) & 0xf0000) | m_videoram_drawsource[attr_and_code_offs];

			/* substitute auto animation bits */
			if (!m_auto_animation_disabled)
			{
				if (BIT(attr, 3))
					code = (code & ~0x07) | (m_auto_animation_counter & 0x07);
				else if (BIT(attr, 2))
					code = (code & ~0x03) | (m_auto_animation_counter & 0x03);
			}

			/* vertical flip? */
			if (BIT(attr, 1))
				sprite_y ^= 0x0f;

			u16 zoom_x_table = zoom_x_tables[zoom_x];

			/* compute offset in gfx ROM and mask it to the number of bits available */
			int gfx_base = ((code << 8) | (sprite_y << 4)) & m_sprite_gfx_address_mask;

			const pen_t *line_pens = &m_pens[attr >> 8 << m_bppshift];

			// RetroDepth: record this slot's palette bank -- rd_compute_slot_depths()
			// maps it directly to a z value (see m_rd_slot_palette's comment in
			// neogeo_spr.h).
			m_rd_slot_palette[sprite_number] = (u8)(attr >> 8);

			int x_inc;

			/* horizontal flip? */
			if (BIT(attr, 0))
			{
				gfx_base = gfx_base + 0x0f;
				x_inc = -1;
			}
			else
				x_inc = 1;

			// RetroDepth: every pixel this sprite writes also records the parallax
			// plane its slot was assigned. The depth pointer advances in lockstep
			// with pixel_addr so it stays aligned through zoom skips and flips.
			const u8 rd_z = m_rd_slot_z[sprite_number];
			// Capped true independent capture (see the comment block on
			// m_rd_capture_slot in neogeo_spr.h): rd_compute_slot_depths()
			// already resolved which of the first RD_DRAW_CAP real objects
			// (with chained columns merged into their head) this slot
			// belongs to, once per frame -- just look it up here.
			u8 capture_slot = m_rd_capture_base ? m_rd_capture_slot[sprite_number] : RD_CAPTURE_UNASSIGNED;
			u32 *capture_base_row = (capture_slot != RD_CAPTURE_UNASSIGNED)
				? m_rd_capture_base + (size_t)capture_slot * m_rd_capture_plane_stride + scanline * m_rd_capture_stride
				: nullptr;

			// OCCUPXY is the exact slot -> row -> pixel experiment. The ordinary
			// capture path assigns one plane to a whole slot/run. OCCUPXY instead
			// counts the opaque writes that have already landed at this screen
			// pixel, maps that overdraw level through the rolling-average table, and
			// writes the pixel into the corresponding independent capture plane.
			auto capture_drawn_pixel = [&](u32 *drawn_pixel, u32 *fixed_capture_addr)
			{
				if (!m_rd_capture_base)
					return;
				if (m_rd_layer_mode != RdLayerMode::OccupXY)
				{
					if (fixed_capture_addr)
						*fixed_capture_addr = *drawn_pixel | 0xFF000000u;
					return;
				}

				if (scanline < 0 || scanline >= bitmap.height())
					return;
				u32 *row_base = &bitmap.pix(scanline, 0);
				const int pixel_x = (int)(drawn_pixel - row_base);
				if (pixel_x < 0 || pixel_x >= bitmap.width() || pixel_x >= 512)
					return;

				const size_t pixel_index = (size_t)scanline * 512 + (size_t)pixel_x;
				u8 &draw_depth = m_rd_occupxy_pixel_draw_depth[pixel_index];
				const u8 raw_level = (u8)std::min<int>((int)draw_depth + 1, RD_DRAW_CAP);
				draw_depth = raw_level;
				const u8 mapped_level = m_rd_occupxy_level_map[raw_level]
					? m_rd_occupxy_level_map[raw_level] : 1;
				const u8 pixel_capture_slot = rd_claim_pixel_capture_bucket(mapped_level, sprite_number);
				if (pixel_capture_slot == RD_CAPTURE_UNASSIGNED)
					return;

				u32 *pixel_capture = m_rd_capture_base
					+ (size_t)pixel_capture_slot * m_rd_capture_plane_stride
					+ (size_t)scanline * m_rd_capture_stride
					+ (size_t)pixel_x;
				*pixel_capture = *drawn_pixel | 0xFF000000u;
			};

			/* draw the line - no wrap-around */
			if (x <= 0x01f0)
			{
				u32 *pixel_addr = &bitmap.pix(scanline, x + NEOGEO_HBEND);
				u8 *depth_addr = m_rd_depth_base
					? m_rd_depth_base + scanline * m_rd_depth_stride + x + NEOGEO_HBEND
					: nullptr;
				u32 *capture_addr = capture_base_row ? capture_base_row + x + NEOGEO_HBEND : nullptr;

				for (int i = 0; i < 0x10; i++)
				{
					if (BIT(zoom_x_table, 15))
					{
						if (draw_pixel(gfx_base, pixel_addr, line_pens))
						{
							if (depth_addr) *depth_addr = rd_z;
							// True independent capture: write the same pixel
							// this sprite just drew into its own claimed
							// plane too, so it survives here even though a
							// later-drawn sprite may overwrite the shared
							// `bitmap` at this exact pixel.
							capture_drawn_pixel(pixel_addr, capture_addr);
						}

						pixel_addr++;
						if (depth_addr) depth_addr++;
						if (capture_addr) capture_addr++;
					}

					zoom_x_table <<= 1;
					if (zoom_x_table == 0)
						break;

					gfx_base += x_inc;
				}
			}
			/* wrap-around */
			else
			{
				const int x_save = x;
				u32 *pixel_addr = &bitmap.pix(scanline, NEOGEO_HBEND);
				u8 *depth_addr = m_rd_depth_base
					? m_rd_depth_base + scanline * m_rd_depth_stride + NEOGEO_HBEND
					: nullptr;
				u32 *capture_addr = capture_base_row ? capture_base_row + NEOGEO_HBEND : nullptr;

				for (int i = 0; i < 0x10; i++)
				{
					if (BIT(zoom_x_table, 15))
					{
						if (x >= 0x200)
						{
							if (draw_pixel(gfx_base, pixel_addr, line_pens))
							{
								if (depth_addr) *depth_addr = rd_z;
								capture_drawn_pixel(pixel_addr, capture_addr);
							}

							pixel_addr++;
							if (depth_addr) depth_addr++;
							if (capture_addr) capture_addr++;
						}

						x++;
					}

					zoom_x_table <<= 1;
					if (zoom_x_table == 0)
						break;

					gfx_base += x_inc;
				}
				x = x_save;
			}
		}
	}
}


void neosprite_base_device::rd_compute_slot_depths()
{
	// No windowing, no clustering, no ranking, no history. Each sprite's z is
	// a direct, stateless, proportional scale of ONE input value into
	// [RD_Z_SPRITE_MIN, RD_Z_SPRITE_MAX] -- fixed function, nothing to
	// reorder, nothing to flicker. See RD_Z_SOURCE_DRAW_ORDER's comment in
	// neogeo_spr.h for which input and why. This channel only feeds the
	// legacy zbuffer-fallback config now; it's independent of the capture
	// grouping below.
	u8 z = RD_Z_SPRITE_MIN;

	memset(m_rd_capture_slot, RD_CAPTURE_UNASSIGNED, sizeof(m_rd_capture_slot));
	memset(m_rd_occupxy_bucket_for_level, RD_CAPTURE_UNASSIGNED, sizeof(m_rd_occupxy_bucket_for_level));
	m_rd_occupxy_pixel_draw_depth.fill(0);
	m_rd_occupxy_level_map.fill(1);
	m_rd_capture_next = 0;
	m_rd_frame_counter++;

	// ---- STAGE 1: TABLE 1 -- pure, stateless column-occupancy run
	// detection. Identical no matter which m_rd_layer_mode is active (see
	// RdLayerMode in neogeo_spr.h): collects every run's raw_level plus
	// which VRAM slots belong to it, so Stage 2 below can plug in whichever
	// algorithm turns those raw levels into final buckets without
	// re-deriving the collision math per mode.
	u8 col_level[RD_CAPTURE_COLUMNS] = {};

	// start_row/tile_rows: TileOcc mode's per-run vertical footprint, in 16px
	// tile units. Neo Geo composites an object as N horizontally-chained
	// slots that all share ONE y/rows value from the head slot (see
	// draw_sprites() -- only `if (!BIT(y_control, 6))` decodes y/rows; a
	// chained continuation just inherits them), so every run's true vertical
	// extent is this single rectangle -- no per-slot Y decoding needed.
	struct RdRun
	{
		u8 raw_level;
		u16 head_slot;
		int slot_off;
		int slot_len;
		int start_row;
		int tile_rows;
		// Screen-space ordering fields used by COUNTER. These are the same
		// wrapped hardware coordinates used by draw_sprites(), so the
		// comparisons remain stable even when an object starts off-screen.
		int x_start;
		int x_end;
		int y_start;
		int y_end;
	};
	std::vector<RdRun> runs;
	std::vector<u16> run_slot_pool;
	std::vector<u8>  run_col_pool; // parallel to run_slot_pool -- each member's column, for TileOcc
	runs.reserve(64);
	run_slot_pool.reserve(256);
	run_col_pool.reserve(256);

	u8  run_columns[RD_CAPTURE_MAX_RUN_LEN];
	u16 run_slots[RD_CAPTURE_MAX_RUN_LEN];
	int run_len = 0;
	bool run_active = false;
	int cur_run_start_row = 0;
	int cur_run_tile_rows = 0;
	int cur_run_x_start = 0;
	int cur_run_x_end = 0;
	int cur_run_y_start = 0;

	auto finalize_run_stage1 = [&]()
	{
		if (run_len == 0) return;
		u8 raw_level = 0; // 0 = invisible / didn't fit in RD_DRAW_CAP this frame
		if (run_active)
		{
			u8 max_level = 0;
			for (int i = 0; i < run_len; i++)
				max_level = std::max(max_level, col_level[run_columns[i]]);
			const u8 r = (u8)(max_level + 1);
			if (r <= RD_DRAW_CAP)
			{
				raw_level = r;
				for (int i = 0; i < run_len; i++)
					col_level[run_columns[i]] = raw_level;
			}
		}
		RdRun run{
			raw_level,
			run_slots[0],
			(int)run_slot_pool.size(),
			run_len,
			cur_run_start_row,
			cur_run_tile_rows,
			cur_run_x_start,
			cur_run_x_end,
			cur_run_y_start,
			cur_run_y_start + cur_run_tile_rows * 0x10 - 1
		};
		for (int i = 0; i < run_len; i++)
		{
			run_slot_pool.push_back(run_slots[i]);
			run_col_pool.push_back(run_columns[i]);
		}
		runs.push_back(run);
		run_len = 0;
		run_active = false;
	};

	// Debug-only: which palette banks had an active sprite this frame, for
	// the palette-swatch capture tool (questretrodepth's
	// neogeo_palette_debug.cpp). Cheap enough (256-entry dedup, <400 slots)
	// to just always run rather than gate behind a flag.
	bool seen[256] = {};
	uint8_t active_list[256];
	uint32_t active_count = 0;

	// Mirrors draw_sprites()'s own x/zoom_x bookkeeping (not its Y/scanline
	// logic -- this pass isn't scanline-scoped) purely to recover each
	// slot's screen column for the occupancy grouping above.
	int x = 0, zoom_x = 0;

	for (u16 slot = 0; slot < MAX_SPRITES_PER_SCREEN; slot++)
	{
		const u16 y_control = m_videoram_drawsource[0x8200 | slot];
		const u16 zoom_control = m_videoram_drawsource[0x8000 | slot];
		const bool is_head = BIT(~y_control, 6);

		if (is_head)
		{
			finalize_run_stage1();
			x = m_videoram_drawsource[0x8400 | slot] >> 7;
			zoom_x = (zoom_control >> 8) & 0x0f;
			run_active = (y_control & 0x3f) != 0;
			cur_run_x_start = x;
			cur_run_y_start = 0x200 - (y_control >> 7);

			// Same y/rows decode as draw_sprites()'s head branch, for
			// TileOcc's per-run tile-row footprint -- see the RdRun
			// comment above. 0x200 - (y_control>>7) can reach exactly
			// 0x200, so mask to 0x1ff before dividing into 16px tile rows
			// (kTileGridRows == 0x200/16, so the masked value already
			// lands in range with no extra wrap needed).
			cur_run_start_row = ((0x200 - (y_control >> 7)) & 0x1ff) >> 4;
			cur_run_tile_rows = y_control & 0x3f;

			const u8 pal = m_rd_slot_palette[slot];
			if (RD_Z_SOURCE_DRAW_ORDER)
			{
				// Ascending: slot 0 (drawn first, so farthest back on real hardware) maps to
				// RD_Z_SPRITE_MIN, slot MAX_SPRITES_PER_SCREEN-1 (drawn last, so frontmost) maps
				// to RD_Z_SPRITE_MAX -- directly mirrors painter's-algorithm draw order, which is
				// the one signal guaranteed consistent across every game (see neogeo_spr.h).
				z = (u8)(RD_Z_SPRITE_MIN
				       + ((uint32_t)slot * (uint32_t)(RD_SPRITE_PLANES - 1)) / (MAX_SPRITES_PER_SCREEN - 1));
			}
			else
			{
				// Reversed palette->z mapping: palette 0 lands on RD_Z_SPRITE_MAX instead of
				// RD_Z_SPRITE_MIN, and palette 255 on RD_Z_SPRITE_MIN instead of RD_Z_SPRITE_MAX.
				z = (u8)(RD_Z_SPRITE_MAX - (pal * (RD_SPRITE_PLANES - 1)) / 255);
			}

			if (!seen[pal] && active_count < 256)
			{
				seen[pal] = true;
				active_list[active_count++] = pal;
			}
		}
		else
		{
			x = (x + zoom_x + 1) & 0x01ff;
			zoom_x = (zoom_control >> 8) & 0x0f;
		}

		if (run_len < RD_CAPTURE_MAX_RUN_LEN)
		{
			cur_run_x_end = x;
			run_columns[run_len] = (u8)((x >> 4) & (RD_CAPTURE_COLUMNS - 1));
			run_slots[run_len] = slot;
			run_len++;
		}

		m_rd_slot_z[slot] = z;
	}
	finalize_run_stage1();

	retrodepth_write_active_palettes(active_list, active_count);

	// ---- STAGE 2: turn each run's raw_level into a final bucket, per the
	// active m_rd_layer_mode. One capture bucket PER (mapped) LEVEL, not per
	// run -- every run mapped to the same level shares one bucket, exactly
	// mirroring how a plain palette-group or object-group bucket would be
	// reused across non-conflicting draws. Without this, each run would claim
	// its own fresh bucket regardless of level, reproducing the discarded
	// "first N objects" vertical-bar bug.
	const int n = (int)runs.size();
	std::vector<u8> run_bucket(n, RD_CAPTURE_UNASSIGNED);
	std::array<std::array<u8, RD_CAPTURE_COLUMNS>, MAX_SPRITES_PER_SCREEN> xy_slot_row_level{};
	u8 level_bucket[RD_DRAW_CAP + 1];
	std::fill(std::begin(level_bucket), std::end(level_bucket), RD_CAPTURE_UNASSIGNED);

	auto claim_bucket_for_level = [&](u8 level, u16 headnum_hint) -> u8
	{
		if (level < 1) level = 1;
		if (level > RD_DRAW_CAP) level = RD_DRAW_CAP;
		if (level_bucket[level] == RD_CAPTURE_UNASSIGNED && m_rd_capture_next < RD_DRAW_CAP)
		{
			level_bucket[level] = (u8)m_rd_capture_next++;
			// Scale the 1..RD_DRAW_CAP mapped level into 1..254, so it never
			// collides with neogeo_base's 0 or neogeo_fix's 255.
			m_rd_capture_slot_zorder[level_bucket[level]] =
				(u8)(1 + ((uint32_t)(level - 1) * 253) / (RD_DRAW_CAP - 1));
			// Debug label: remember which real VRAM slot first claimed this
			// bucket -- see rd_capture_slot_headnum().
			m_rd_capture_slot_headnum[level_bucket[level]] = headnum_hint;
		}
		return level_bucket[level];
	};

	// Shared by every TileOcc* mode (TileOcc, TileOccPer, TileOccGraph,
	// StableGraph) --
	// is (slot, tile_idx)'s tile "real content" or negligible padding?
	// min_opaque_pixels (out of 256) is the only knob: TileOcc uses 1 (any
	// content at all counts, same as a plain blank/non-blank test);
	// TileOccPer/TileOccGraph/StableGraph use a real percentage (currently 50%
	// of 256)
	// so a sliver of overlap doesn't count as a genuine claim.
	//
	// This is a cheap approximation, not a full replication of the real
	// per-scanline renderer: the real hardware picks each display row's
	// SOURCE tile via a zoom_y lookup table (m_region_zoomy), which can
	// stretch/compress under Y-zoom -- reusing or skipping source tiles.
	// This ignores that entirely and treats tile-row R as literally
	// chained-slot `slot`'s own R-th sub-tile entry (same VRAM addressing
	// the scanline decode uses when zoom_y == 0), exact for unzoomed
	// sprites, wrong under Y-zoom stretch/compression. tile_idx is a 5-bit
	// field (0..31) in that addressing scheme -- draw_sprites() derives it
	// as `sprite_y_and_tile >> 4` (0..15) then `tile ^= 0x1f` on vertical
	// invert (16..31), and (sprite_number << 6) gives 64 words = 32 tiles
	// x 2 words per slot. Anything outside that returns "fully occupied"
	// (the safe, always-claims-the-cell fallback) since this approximation
	// has no way to resolve it.
	constexpr int kTileIdxCount = 32;
	auto tile_occupied = [&](u16 slot, int tile_idx, int min_opaque_pixels) -> bool
	{
		if (tile_idx < 0 || tile_idx >= kTileIdxCount) return true;
		if (slot >= MAX_SPRITES_PER_SCREEN) return true;
		const offs_t attr_and_code_offs = (slot << 6) | (tile_idx << 1);
		const u16 attr = m_videoram_drawsource[attr_and_code_offs + 1];
		const u32 code = ((attr << 12) & 0xf0000) | m_videoram_drawsource[attr_and_code_offs];
		const pen_t *line_pens = &m_pens[attr >> 8 << m_bppshift];
		int opaque = 0;
		for (int sy = 0; sy < 16; sy++)
		{
			const int gfx_base = ((code << 8) | (sy << 4)) & m_sprite_gfx_address_mask;
			for (int sx = 0; sx < 16; sx++)
			{
				u32 dummy;
				if (draw_pixel(gfx_base + sx, &dummy, line_pens))
				{
					opaque++;
					if (opaque >= min_opaque_pixels) return true;
				}
			}
		}
		return false;
	};

	// Table 1's own idea (how many things already claimed the cells I
	// touch, plus one for me), resolved into a real 2D grid of 16px tiles
	// instead of one strip spanning the whole screen height, so two runs
	// sharing a column no longer collide unless they ALSO overlap in
	// tile-rows. Every run's vertical footprint (start_row/tile_rows,
	// decoded in Stage 1) is the object's true rectangle, since a chained
	// object always shares one y/rows value across every member slot on
	// real hardware. Shared by TileOcc and TileOccPer -- min_opaque_pixels
	// is the only difference between the two (see tile_occupied() above).
	auto run_tile_grid_pass = [&](int min_opaque_pixels)
	{
		constexpr int kTileGridRows = 0x200 / 16; // 32, matches RD_CAPTURE_COLUMNS' own masking range
		u8 cell_level[kTileGridRows][RD_CAPTURE_COLUMNS] = {};

		for (int ri = 0; ri < n; ri++)
		{
			const RdRun &run = runs[ri];
			if (run.raw_level == 0) continue;
			const int rows_to_mark = std::min(run.tile_rows, kTileGridRows);

			// Occupied-ness only depends on (slot, tile_idx), not on where
			// that tile lands in the grid -- compute it once per member
			// here and reuse for both the collision read and the mark.
			bool occupied[RD_CAPTURE_MAX_RUN_LEN][kTileGridRows];
			for (int i = 0; i < run.slot_len; i++)
			{
				const u16 slot = run_slot_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
					occupied[i][rr] = tile_occupied(slot, rr, min_opaque_pixels);
			}

			u8 max_level = 0;
			for (int i = 0; i < run.slot_len; i++)
			{
				const u8 col = run_col_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
				{
					if (!occupied[i][rr]) continue;
					const int row = (run.start_row + rr) & (kTileGridRows - 1);
					max_level = std::max(max_level, cell_level[row][col]);
				}
			}
			const u8 grid_level = (u8)(max_level + 1);
			for (int i = 0; i < run.slot_len; i++)
			{
				const u8 col = run_col_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
				{
					if (!occupied[i][rr]) continue;
					const int row = (run.start_row + rr) & (kTileGridRows - 1);
					cell_level[row][col] = grid_level;
				}
			}
			run_bucket[ri] = claim_bucket_for_level(grid_level, run.head_slot);
		}
	};

	// OCCUPXY uses the same 16px X/Y occupancy calculation as TILEOCC, but
	// feeds those per-run levels into OCCUP/OCCUPUD's history and hysteresis.
	// Keep this separate from run_tile_grid_pass(): that pass assigns buckets
	// immediately, while OCCUPXY must first collect the grid-derived levels so
	// its occupancy history can decide how to merge them.
	auto compute_tile_raw_levels = [&](int min_opaque_pixels) -> std::vector<u8>
	{
		constexpr int kTileGridRows = 0x200 / 16;
		u8 cell_level[kTileGridRows][RD_CAPTURE_COLUMNS] = {};
		std::vector<u8> levels(n, 0);

		for (int ri = 0; ri < n; ri++)
		{
			const RdRun &run = runs[ri];
			if (run.raw_level == 0) continue;
			const int rows_to_mark = std::min(run.tile_rows, kTileGridRows);
			bool occupied[RD_CAPTURE_MAX_RUN_LEN][kTileGridRows];
			for (int i = 0; i < run.slot_len; i++)
			{
				const u16 slot = run_slot_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
					occupied[i][rr] = tile_occupied(slot, rr, min_opaque_pixels);
			}

			u8 max_level = 0;
			for (int i = 0; i < run.slot_len; i++)
			{
				const u8 col = run_col_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
				{
					if (!occupied[i][rr]) continue;
					const int row = (run.start_row + rr) & (kTileGridRows - 1);
					max_level = std::max(max_level, cell_level[row][col]);
				}
			}

			const u8 grid_level = (u8)std::min<int>(max_level + 1, RD_DRAW_CAP);
			levels[ri] = grid_level;
			for (int i = 0; i < run.slot_len; i++)
			{
				const u16 slot = run_slot_pool[run.slot_off + i];
				const u8 col = run_col_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
				{
					if (!occupied[i][rr]) continue;
					const int row = (run.start_row + rr) & (kTileGridRows - 1);
					xy_slot_row_level[slot][rr] = grid_level;
					cell_level[row][col] = grid_level;
				}
			}
		}
		return levels;
	};

	// Opaque-pixel threshold (out of 256) TileOccGraph/StableGraph use to decide a tile
	// is real content rather than padding. 128 = 50%, inherited from
	// TileOccPer. Measured on mslug: this is a minor knob -- ~75% of real
	// sprite tiles come in at 90-100% opacity, so the threshold only
	// reclassifies the sparse tail (1px vs 50% moved the frame's layer count
	// by about one).
	constexpr int kRdGraphMinOpaquePixels = 128;
	// -1 = no Phase A merging (Phase B's overlap graph alone). Measured on
	// mslug: dilated merging (radius 1) chained the whole background into a
	// single screen-spanning object (168 runs -> 1 object), leaving nothing
	// to overlap and collapsing every frame to one layer.
	constexpr int kRdGraphMergeRadius = -1;

	// merge_radius: -1 = Phase A disabled entirely (every run is its own
	// object, Phase B alone does the work); 0 = merge only runs that share
	// an actual occupied cell; 1 = merge across a 1-tile gap (dilated).
	// Measured on mslug, 0 and 1 both collapse the frame to a single layer
	// (see kRdGraphMergeRadius) -- the parameter is kept so the experiment
	// is one constant away from being repeatable.
	auto run_tile_graph_pass = [&](int min_opaque_pixels, int merge_radius, bool stabilize = false)
	{
		// PHASE A -- same-object run merging. TileOcc/TileOccPer assign
		// depth per RUN, but one visual object (body + weapon + shadow +
		// accessories) can be several independent VRAM runs; splitting
		// them onto different levels is the exact bug this mode exists
		// to fix. Merge runs into "objects" via union-find over a
		// DILATED (1-tile-gap tolerance) adjacency index built from each
		// run's own occupied-cell footprint, so a weapon sitting right
		// next to (not necessarily touching) a body still merges.
		constexpr int kTileGridRows = 0x200 / 16;

		// A single un-merged run is only eligible to PARTICIPATE in
		// merging (as either side of a union) if its own footprint is
		// small -- roughly character-part sized. Union-find is
		// transitively closed: without this cap, one wide run that
		// legitimately touches many genuinely-separate objects at once
		// (a floor, a wall, a background band -- present in nearly
		// every real scene) acts as a bridge that unions the ENTIRE
		// frame into one giant object, collapsing everything onto a
		// single flat layer. A run that fails this cap still gets its
		// own object below (just never merges with anything), and
		// Phase B's overlap graph still places it correctly relative
		// to everything else via genuine overlap, it just can't glue
		// unrelated things together in Phase A anymore.
		constexpr int kMaxMergeColumns = 6;
		constexpr int kMaxMergeCells   = 40;

		std::vector<std::array<u32, kTileGridRows>> run_bits(n);
		std::vector<bool> run_merge_eligible(n, false);
		for (int ri = 0; ri < n; ri++)
		{
			run_bits[ri].fill(0);
			const RdRun &run = runs[ri];
			if (run.raw_level == 0) continue;
			const int rows_to_mark = std::min(run.tile_rows, kTileGridRows);
			for (int i = 0; i < run.slot_len; i++)
			{
				const u16 slot = run_slot_pool[run.slot_off + i];
				const u8 col = run_col_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
				{
					if (!tile_occupied(slot, rr, min_opaque_pixels)) continue;
					const int row = (run.start_row + rr) & (kTileGridRows - 1);
					run_bits[ri][row] |= (1u << col);
				}
			}

			int cell_count = 0;
			u32 cols_seen = 0;
			for (int row = 0; row < kTileGridRows; row++)
			{
				cell_count += std::popcount(run_bits[ri][row]);
				cols_seen |= run_bits[ri][row];
			}
			const int col_span = std::popcount(cols_seen);
			run_merge_eligible[ri] = cell_count > 0 && cell_count <= kMaxMergeCells && col_span <= kMaxMergeColumns;
		}

		std::vector<int> parent(n);
		for (int i = 0; i < n; i++) parent[i] = i;
		auto find_root = [&](int x) -> int
		{
			while (parent[x] != x)
			{
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		auto unite = [&](int a, int b)
		{
			a = find_root(a); b = find_root(b);
			if (a != b) parent[a] = b;
		};

		// Dilated-cell -> first run seen there; any later run landing on
		// the same dilated cell gets unioned with it. Dilation is only
		// used for deciding what merges -- Phase B's real overlap test
		// below uses each object's un-dilated bits.
		std::unordered_map<int, int> dilated_owner;
		dilated_owner.reserve(256);
		if (merge_radius >= 0)
		{
			for (int ri = 0; ri < n; ri++)
			{
				if (!run_merge_eligible[ri]) continue;
				for (int row = 0; row < kTileGridRows; row++)
				{
					const u32 word = run_bits[ri][row];
					if (word == 0) continue;
					for (int col = 0; col < RD_CAPTURE_COLUMNS; col++)
					{
						if (!((word >> col) & 1u)) continue;
						for (int dr = -merge_radius; dr <= merge_radius; dr++)
						{
							const int rr2 = (row + dr) & (kTileGridRows - 1);
							for (int dc = -merge_radius; dc <= merge_radius; dc++)
							{
								const int cc2 = (col + dc) & (RD_CAPTURE_COLUMNS - 1);
								const int key = rr2 * RD_CAPTURE_COLUMNS + cc2;
								auto it = dilated_owner.find(key);
								if (it == dilated_owner.end()) dilated_owner.emplace(key, ri);
								else unite(ri, it->second);
							}
						}
					}
				}
			}
		}

		struct RdObject
		{
			std::array<u32, kTileGridRows> bits{};
			int draw_order = 0;
			u16 head_slot = 0;
			std::vector<int> member_runs;
		};
		std::unordered_map<int, RdObject> groups;
		for (int ri = 0; ri < n; ri++)
		{
			if (runs[ri].raw_level == 0) continue;
			RdObject &obj = groups[find_root(ri)];
			if (obj.member_runs.empty()) {
				obj.draw_order = ri;
				obj.head_slot = runs[ri].head_slot;
			}
			else obj.draw_order = std::min(obj.draw_order, ri);
			obj.member_runs.push_back(ri);
			for (int row = 0; row < kTileGridRows; row++)
				obj.bits[row] |= run_bits[ri][row];
		}
		std::vector<RdObject> objects;
		objects.reserve(groups.size());
		for (auto &kv : groups) objects.push_back(std::move(kv.second));
		std::sort(objects.begin(), objects.end(),
			[](const RdObject &a, const RdObject &b) { return a.draw_order < b.draw_order; });

		// PHASE B -- overlap graph + topological longest-path leveling.
		// Objects are already in draw order (lowest member run index),
		// so a simple forward pass is a valid topological walk: an
		// object's level depends ONLY on objects it actually,
		// geometrically overlaps (a real bitset intersection), never on
		// a per-cell running maximum that could have been bumped by
		// something this object never touches -- this is what avoids
		// the old "wide bridging object smears an inflated level into
		// unrelated territory" transitive-stacking effect.
		const int m = (int)objects.size();
		std::vector<u8> obj_level(m, 0);
		for (int oi = 0; oi < m; oi++)
		{
			u8 max_pred_level = 0;
			for (int oj = 0; oj < oi; oj++)
			{
				bool overlap = false;
				for (int row = 0; row < kTileGridRows; row++)
				{
					if (objects[oi].bits[row] & objects[oj].bits[row]) { overlap = true; break; }
				}
				if (overlap) max_pred_level = std::max(max_pred_level, obj_level[oj]);
			}
			obj_level[oi] = (u8)(max_pred_level + 1);
		}

		if (stabilize)
		{
			// StableGraph stabilizes complete spatial objects, not individual
			// VRAM slots. Its history is intentionally limited to the previous
			// frame, so a slot reused for another object cannot carry a stale
			// depth through a gap or a mode change.
			std::vector<int> matched_previous(m, -1);
			const bool continuous = m_rd_stable_previous_frame != 0 &&
				(m_rd_frame_counter - m_rd_stable_previous_frame) == 1;
			if (continuous && !m_rd_stable_previous_objects.empty())
			{
				struct RdMatch { int current; int previous; int score; };
				std::vector<RdMatch> matches;
				matches.reserve((size_t)m * m_rd_stable_previous_objects.size());

				auto bit_count = [](const std::array<u32, kTileGridRows> &bits) -> int
				{
					int count = 0;
					for (u32 word : bits) count += std::popcount(word);
					return count;
				};
				std::vector<int> current_counts(m, 0);
				for (int oi = 0; oi < m; oi++) current_counts[oi] = bit_count(objects[oi].bits);
				std::vector<int> previous_counts(m_rd_stable_previous_objects.size(), 0);
				for (size_t pi = 0; pi < m_rd_stable_previous_objects.size(); pi++)
					previous_counts[pi] = bit_count(m_rd_stable_previous_objects[pi].bits);

				for (int oi = 0; oi < m; oi++)
				{
					if (current_counts[oi] == 0) continue;
					for (size_t pi = 0; pi < m_rd_stable_previous_objects.size(); pi++)
					{
						if (previous_counts[pi] == 0) continue;
						int intersection = 0;
						for (int row = 0; row < kTileGridRows; row++)
							intersection += std::popcount(objects[oi].bits[row] &
								m_rd_stable_previous_objects[pi].bits[row]);
						if (intersection < 2) continue;
						const int smaller = std::min(current_counts[oi], previous_counts[pi]);
						const int coverage = (intersection * 100) / std::max(1, smaller);
						const bool same_head = objects[oi].head_slot ==
							m_rd_stable_previous_objects[pi].head_slot;
						if (!same_head && coverage < 35) continue;
						// Same-head matches win first; otherwise prefer the
						// strongest footprint coverage.
						const int score = (same_head ? 1000000 : 0) +
							coverage * 1000 + intersection;
						matches.push_back({oi, (int)pi, score});
					}
				}
				std::sort(matches.begin(), matches.end(), [](const RdMatch &a, const RdMatch &b)
				{
					if (a.score != b.score) return a.score > b.score;
					if (a.current != b.current) return a.current < b.current;
					return a.previous < b.previous;
				});
				std::vector<bool> used_current(m, false);
				std::vector<bool> used_previous(m_rd_stable_previous_objects.size(), false);
				for (const RdMatch &match : matches)
				{
					if (used_current[match.current] || used_previous[match.previous]) continue;
					used_current[match.current] = true;
					used_previous[match.previous] = true;
					matched_previous[match.current] = match.previous;
				}
			}

			// The graph level is the minimum legal level for the current
			// frame. A previous level is accepted only inside a one-level
			// deadband; then the overlap constraints are applied again so
			// stabilization can never reverse painter order.
			std::vector<u8> final_level(m, 1);
			for (int oi = 0; oi < m; oi++)
			{
				int chosen = obj_level[oi];
				const int pi = matched_previous[oi];
				if (pi >= 0)
				{
					const int previous_level = m_rd_stable_previous_objects[pi].level;
					const int delta = previous_level - chosen;
					if (delta >= -1 && delta <= 1) chosen = previous_level;
				}

				int required = 1;
				for (int oj = 0; oj < oi; oj++)
				{
					bool overlap = false;
					for (int row = 0; row < kTileGridRows; row++)
					{
						if (objects[oi].bits[row] & objects[oj].bits[row]) { overlap = true; break; }
					}
					if (overlap) required = std::max(required, (int)final_level[oj] + 1);
				}
				chosen = std::clamp(std::max(chosen, required), 1, RD_DRAW_CAP);
				final_level[oi] = (u8)chosen;

				const u8 bucket = claim_bucket_for_level(final_level[oi], objects[oi].head_slot);
				for (int ri : objects[oi].member_runs) run_bucket[ri] = bucket;
			}

			m_rd_stable_previous_objects.clear();
			m_rd_stable_previous_objects.reserve(objects.size());
			for (int oi = 0; oi < m; oi++)
			{
				RdStableObject previous;
				previous.bits = objects[oi].bits;
				previous.head_slot = objects[oi].head_slot;
				previous.level = final_level[oi];
				m_rd_stable_previous_objects.push_back(previous);
			}
			m_rd_stable_previous_frame = m_rd_frame_counter;
		}
		else
		{
			for (int oi = 0; oi < m; oi++)
			{
				const u8 bucket = claim_bucket_for_level(obj_level[oi], objects[oi].head_slot);
				for (int ri : objects[oi].member_runs) run_bucket[ri] = bucket;
			}
		}
	};

	// PAINTER -- the original memory-free design (the one that produced
	// deep 5-level stacks), plus exactly ONE structural fix kept from the
	// later iterations: object grouping with atomic bucket assignment, so
	// one visual object's chains can never be split across buckets (the
	// "part of the hostage's body captured into the player's layer with a
	// vertical seam" bug). Everything temporal that was layered on after
	// that -- per-slot ratchets, home-layer policies, footprint gates, the
	// screen-space level field -- was MEASURED trading the depth away (down
	// to 2 shallow levels) without actually removing the flicker, so it is
	// all gone: placement is pure per-frame greedy in hardware draw order.
	// Runs that OVERLAP the group being built do not chain into it, so an
	// explosion's mutually-overlapping shrapnel still split into their own
	// groups and staircase nearer (the "pure 3D" spread this version was
	// praised for). Also kept, both orthogonal to placement: the fixed
	// 6-layer budget (static accordion depths / static "insert coin"
	// nearest layer) and the QRD_PLVL occupancy metric.
	auto run_painter_pass = [&](int min_opaque_pixels)
	{
		constexpr int kTileGridRows = 0x200 / 16;
		// "Small" = character-part-sized footprint; empty footprints (every
		// tile under the opacity threshold) count as small on purpose: those
		// are exactly the sparse fringe pieces that must follow their
		// neighbor.
		constexpr int kMaxSmallCols  = 6;
		constexpr int kMaxSmallCells = 40;
		// Run-index distances: consecutive-ish runs are "the same object
		// still being drawn"; within a burst, shrapnel runs land within a
		// few indices of the members they overlap.
		constexpr int kAdjacentRunGap = 2;
		constexpr int kClusterRunGap  = 8;
		constexpr int kPainterMaxLayers = 6;

		std::vector<std::array<u32, kTileGridRows>> run_bits(n);
		std::vector<bool> run_small(n, false);
		for (int ri = 0; ri < n; ri++)
		{
			run_bits[ri].fill(0);
			const RdRun &run = runs[ri];
			if (run.raw_level == 0) continue;
			const int rows_to_mark = std::min(run.tile_rows, kTileGridRows);
			for (int i = 0; i < run.slot_len; i++)
			{
				const u16 slot = run_slot_pool[run.slot_off + i];
				const u8 col = run_col_pool[run.slot_off + i];
				for (int rr = 0; rr < rows_to_mark; rr++)
				{
					if (!tile_occupied(slot, rr, min_opaque_pixels)) continue;
					const int row = (run.start_row + rr) & (kTileGridRows - 1);
					run_bits[ri][row] |= (1u << col);
				}
			}
			int cell_count = 0;
			u32 cols_seen = 0;
			for (int row = 0; row < kTileGridRows; row++)
			{
				cell_count += std::popcount(run_bits[ri][row]);
				cols_seen |= run_bits[ri][row];
			}
			run_small[ri] = cell_count <= kMaxSmallCells && std::popcount(cols_seen) <= kMaxSmallCols;
		}

		auto bits_overlap = [&](const std::array<u32, kTileGridRows> &a,
		                        const std::array<u32, kTileGridRows> &b) -> bool
		{
			for (int row = 0; row < kTileGridRows; row++)
				if (a[row] & b[row]) return true;
			return false;
		};

		// ---- STEP 1: group consecutive small runs into OBJECTS (atomic
		// placement -- the pixel-mixing fix). A run that overlaps the group
		// being built does NOT chain (shrapnel spread); wide runs stay
		// singletons (parallax planes must never fuse).
		struct PainterGroup
		{
			std::array<u32, kTileGridRows> bits{};
			std::vector<int> members; // run indices, ascending
			int first_ri = 0;
			int last_ri = 0;
			bool small_chain = false;
		};
		std::vector<PainterGroup> groups;
		for (int ri = 0; ri < n; ri++)
		{
			const RdRun &run = runs[ri];
			if (run.raw_level == 0) continue;
			const bool can_chain = !groups.empty() && groups.back().small_chain &&
				run_small[ri] && (ri - groups.back().last_ri) <= kAdjacentRunGap &&
				!bits_overlap(run_bits[ri], groups.back().bits);
			if (!can_chain)
			{
				PainterGroup g;
				g.first_ri = ri;
				g.small_chain = run_small[ri];
				groups.push_back(std::move(g));
			}
			PainterGroup &g = groups.back();
			for (int row = 0; row < kTileGridRows; row++)
				g.bits[row] |= run_bits[ri][row];
			g.members.push_back(ri);
			g.last_ri = ri;
		}

		// ---- STEP 2: place groups with the ORIGINAL greedy rules. --------
		struct PainterLayer
		{
			std::array<u32, kTileGridRows> bits{};
			std::vector<int> member_runs; // run indices of everything here, ascending
		};
		std::vector<PainterLayer> layers;
		int last_group_last_ri = -1; // last placed group's final run index...
		int last_layer = -1;         // ...and the layer it landed in
		int level_count[kPainterMaxLayers] = {};

		for (const PainterGroup &g : groups)
		{
			// Highest layer the group's footprint overlaps (layers above it
			// are guaranteed clear, so joining l_overlap+1 is always safe).
			int l_overlap = -1;
			for (int li = (int)layers.size() - 1; li >= 0; li--)
			{
				if (bits_overlap(g.bits, layers[li].bits)) { l_overlap = li; break; }
			}

			const bool follows_previous = g.small_chain && last_group_last_ri >= 0 &&
				(g.first_ri - last_group_last_ri) <= kAdjacentRunGap;

			int target;
			if (l_overlap >= 0)
			{
				// Cluster rule: colliding with a small run of that layer drawn
				// just before us = same visual burst, merge into it.
				bool cluster = false;
				if (g.small_chain)
				{
					for (auto it = layers[l_overlap].member_runs.rbegin();
					     it != layers[l_overlap].member_runs.rend(); ++it)
					{
						if (g.first_ri - *it > kClusterRunGap) break; // ascending: older = farther
						if (run_small[*it] && bits_overlap(g.bits, run_bits[*it])) { cluster = true; break; }
					}
				}
				if (cluster)
					target = l_overlap;
				else if (follows_previous && last_layer > l_overlap)
					target = last_layer; // keep following the object we're part of
				else
					target = l_overlap + 1;
			}
			else
			{
				target = (follows_previous && last_layer >= 0) ? last_layer : 0;
			}
			target = std::min(target, kPainterMaxLayers - 1);

			if (target >= (int)layers.size())
				layers.emplace_back();
			for (int row = 0; row < kTileGridRows; row++)
				layers[target].bits[row] |= g.bits[row];
			for (int ri : g.members)
				layers[target].member_runs.push_back(ri);
			last_group_last_ri = g.last_ri;
			last_layer = target;
			level_count[target]++;

			// Every member run of the object gets the SAME bucket -- the
			// atomicity that makes intra-object seams/mixing impossible.
			for (int ri : g.members)
				run_bucket[ri] = claim_bucket_for_level((u8)(target + 1), runs[ri].head_slot);
		}

#ifdef __ANDROID__
		// Occupancy metric (QRD_PLVL): how many groups landed on each level,
		// every ~5s -- the metric that catches a depth collapse, which pure
		// transition-stability tracking was measured missing entirely.
		if ((m_rd_frame_counter % 300) == 0)
		{
			__android_log_print(ANDROID_LOG_INFO, "QRD_PLVL",
				"f=%u lv: %d %d %d %d %d %d",
				m_rd_frame_counter,
				level_count[0], level_count[1], level_count[2],
				level_count[3], level_count[4], level_count[5]);
		}
#endif

		// Fixed layer budget: claim every level each frame so the exported
		// layer set (count and zorders) never changes -- unused levels export
		// as fully-transparent planes; keeps the accordion depths (and the
		// always-nearest fix layer) static.
		for (int level = 1; level <= kPainterMaxLayers; level++)
			claim_bucket_for_level((u8)level, 0);
	};

	// COUNTER/CODE -- the full-frame experiment promoted from the offline slot
	// report. Walk the active sprite runs in hardware slot order. COUNTER keeps
	// the original X-wrap + sprite-number-gap rule as a comparison mode.
	// CODE uses the amended rule:
	//   x_changed    = (x_start < previous x_start)
	//   boundary     = x_changed
	// The tile code and Y signal remain diagnostic only. The resulting
	// cumulative boundary number is the
	// candidate layer ID, folded into a fixed nine-plane output budget.
	//
	// This is intentionally a separate mode rather than a replacement for the
	// existing algorithms. CODE is deliberately immediate and slot-local: it
	// does not merge chained members, wait for temporal confirmation, or hold
	// empty/collapsed companion frames.
	std::array<u8, MAX_SPRITES_PER_SCREEN> counter_slot_bucket{};
	auto run_counter_pass = [&](bool use_x_only_formula)
	{
		m_rd_counter_hold_frame = false;
		counter_slot_bucket.fill(RD_CAPTURE_UNASSIGNED);
		int x_counter = 0;
		int y_counter = 0;
		int sprite_jump_counter = 0;
		int code_changed_counter = 0;
		int both_changed_counter = 0;

		// Build one frame-wide record per VRAM sprite number. The hardware uses
		// separate even/odd scanline lists, and the same sprite can therefore
		// appear at a different list_slot on alternating scanlines. CODE must
		// not use that transient position as the sprite identity. Instead, walk
		// both real hardware lists, collect observations keyed by sprite_number,
		// and use the most common value for each field after the whole frame has
		// been seen. The result is still based on the real draw lists, but the
		// formula below receives one deterministic row per VRAM slot.
		struct CounterRow
		{
			bool seen = false;
			u16 sprite_number = 0;
			int x_start = 0;
			int x_end = 0;
			int y_start = 0;
			int y_end = 0;
			u32 code = 0;
		};
		struct CounterEvidence
		{
			bool seen = false;
			int observations = 0;
			std::unordered_map<int, int> x_start_counts;
			std::unordered_map<int, int> x_end_counts;
			std::unordered_map<int, int> y_start_counts;
			std::unordered_map<int, int> y_end_counts;
			std::unordered_map<u32, int> code_counts;
		};
		std::array<CounterEvidence, MAX_SPRITES_PER_SCREEN> evidence{};

		for (int scanline = NEOGEO_VBEND; scanline < NEOGEO_VBSTART; scanline++)
		{
			u16 *sprite_list = BIT(scanline, 0)
				? &m_videoram_drawsource[0x8680]
				: &m_videoram_drawsource[0x8600];
			int max_sprite_index;
			// Match draw_sprites(): process the last non-zero list entry plus
			// one padded entry because sprite 0 can itself be real.
			for (max_sprite_index = (MAX_SPRITES_PER_LINE - 1);
				 max_sprite_index >= 0; max_sprite_index--)
			{
				if (sprite_list[max_sprite_index] != 0)
					break;
			}
			if (max_sprite_index != (MAX_SPRITES_PER_LINE - 1))
				max_sprite_index++;

			int draw_y = 0;
			int draw_rows = 0;
			int zoom_y = 0;
			int zoom_x = 0;
			int x = 0;

			for (int list_slot = 0; list_slot <= max_sprite_index; list_slot++)
			{
				const u16 sprite_number = sprite_list[list_slot] & 0x01ff;
				const u16 y_control = m_videoram_drawsource[0x8200 | sprite_number];
				const u16 zoom_control = m_videoram_drawsource[0x8000 | sprite_number];

				if (BIT(y_control, 6))
				{
					x = (x + zoom_x + 1) & 0x01ff;
					zoom_x = (zoom_control >> 8) & 0x0f;
				}
				else
				{
					draw_y = 0x200 - (y_control >> 7);
					x = m_videoram_drawsource[0x8400 | sprite_number] >> 7;
					zoom_y = zoom_control & 0xff;
					zoom_x = (zoom_control >> 8) & 0x0f;
					draw_rows = y_control & 0x3f;
				}

				// Keep this in the same order as draw_sprites(): an off-screen
				// horizontal sprite is not evidence for CODE even if its Y range
				// happens to cross this scanline.
				if ((x >= 0x140) && (x <= 0x1f0)) continue;
				if (!sprite_on_scanline(scanline, draw_y, draw_rows)) continue;

				int tile_width = 0;
				for (u16 bits = zoom_x_tables[zoom_x]; bits; bits <<= 1)
					tile_width += BIT(bits, 15);

				// Same wrapped-to-visible conversion as
				// neogeo_slot_debug_report.py's screen_bounds().
				int display_x = x;
				for (int offset : { 0, -0x200, 0x200 })
				{
					const int candidate_x = x + offset;
					if (candidate_x <= 319 && candidate_x + tile_width - 1 >= 0)
					{
						display_x = candidate_x;
						break;
					}
				}

				const int raw_y = draw_y & 0x1ff;
				const int scanline_delta = (scanline - raw_y) & 0x1ff;
				const int timing_y_start = scanline - scanline_delta;
				// Resolve the same tile code that the sprite renderer uses for
				// this scanline. Code is a graphic-tile identifier, not an object
				// ID; CODE uses changes in it as one half of its split signal.
				const int sprite_line = (scanline - draw_y) & 0x1ff;
				int zoom_line = sprite_line & 0xff;
				bool invert = BIT(sprite_line, 8);
				if (invert)
					zoom_line ^= 0xff;
				if (draw_rows > 0x20)
				{
					zoom_line = zoom_line % ((zoom_y + 1) << 1);
					if (zoom_line > zoom_y)
					{
						zoom_line = ((zoom_y + 1) << 1) - 1 - zoom_line;
						invert = !invert;
					}
				}
				const u8 sprite_y_and_tile = m_region_zoomy[(zoom_y << 8) | zoom_line];
				int tile = sprite_y_and_tile >> 4;
				if (invert)
					tile ^= 0x1f;
				const offs_t attr_and_code_offs = (sprite_number << 6) | (tile << 1);
				const u16 attr = m_videoram_drawsource[attr_and_code_offs + 1];
				u32 code = ((attr << 12) & 0xf0000) |
					m_videoram_drawsource[attr_and_code_offs];
				// Match draw_sprites()'s automatic tile animation substitution so
				// the evidence describes the actual tile being rendered.
				if (!m_auto_animation_disabled)
				{
					if (BIT(attr, 3))
						code = (code & ~0x07) | (m_auto_animation_counter & 0x07);
					else if (BIT(attr, 2))
						code = (code & ~0x03) | (m_auto_animation_counter & 0x03);
				}

				CounterEvidence &slot = evidence[sprite_number];
				slot.seen = true;
				slot.observations++;
				slot.x_start_counts[display_x]++;
				slot.x_end_counts[display_x + tile_width - 1]++;
				slot.y_start_counts[timing_y_start - NEOGEO_VBEND]++;
				slot.y_end_counts[timing_y_start + draw_rows * 0x10 - 1 - NEOGEO_VBEND]++;
				slot.code_counts[code]++;
			}
		}

		auto most_common_int = [](const std::unordered_map<int, int> &counts, int fallback)
		{
			int best_value = fallback;
			int best_count = -1;
			for (const auto &entry : counts)
			{
				if (entry.second > best_count ||
					(entry.second == best_count && entry.first < best_value))
				{
					best_value = entry.first;
					best_count = entry.second;
				}
			}
			return best_value;
		};
		auto most_common_code = [](const std::unordered_map<u32, int> &counts, u32 fallback)
		{
			u32 best_value = fallback;
			int best_count = -1;
			for (const auto &entry : counts)
			{
				if (entry.second > best_count ||
					(entry.second == best_count && entry.first < best_value))
				{
					best_value = entry.first;
					best_count = entry.second;
				}
			}
			return best_value;
		};

		std::array<CounterRow, MAX_SPRITES_PER_SCREEN> slot_rows{};
		int counter_row_count = 0;
		for (u16 sprite_number = 0; sprite_number < MAX_SPRITES_PER_SCREEN; sprite_number++)
		{
			const CounterEvidence &slot = evidence[sprite_number];
			if (!slot.seen) continue;
			CounterRow &row = slot_rows[sprite_number];
			row.seen = true;
			row.sprite_number = sprite_number;
			row.x_start = most_common_int(slot.x_start_counts, 0);
			row.x_end = most_common_int(slot.x_end_counts, row.x_start);
			row.y_start = most_common_int(slot.y_start_counts, 0);
			row.y_end = most_common_int(slot.y_end_counts, row.y_start);
			row.code = most_common_code(slot.code_counts, 0);
			counter_row_count++;
		}

		// Candidate levels are collected first, then assigned to VRAM slots.
		// Processing is now in canonical VRAM sprite-number order. A sprite's
		// even/odd list position no longer determines its identity or its place
		// in the CODE sequence.
		u8 candidate_level[MAX_SPRITES_PER_SCREEN] = {};
		bool have_previous = false;
		int previous_x_start = 0;
		int previous_y_start = 0;
		int previous_y_end = 0;
		u32 previous_code = 0;
		u16 previous_sprite_number = 0;
		for (const CounterRow &row : slot_rows)
		{
			if (!row.seen) continue;
			const bool x_changed = use_x_only_formula
				? (have_previous && row.x_start < previous_x_start)
				: (have_previous && row.x_start <= previous_x_start);
			const bool y_changed = use_x_only_formula
				? (have_previous && row.y_end < previous_y_end)
				: (have_previous && row.y_end < previous_y_end &&
				   (row.y_start != previous_y_start || row.y_end != previous_y_end));
			const bool sprite_jump_changed = have_previous &&
				row.sprite_number > (u16)(previous_sprite_number + 1);
			const bool code_changed = have_previous && row.code != previous_code;

			if (x_changed) x_counter++;
			if (y_changed) y_counter++;
			if (sprite_jump_changed) sprite_jump_counter++;
			if (code_changed) code_changed_counter++;
			const bool both_changed = use_x_only_formula
				? x_changed
				: (x_changed && sprite_jump_changed);
			if (both_changed) both_changed_counter++;

			const u8 level = (u8)std::min(both_changed_counter + 1,
				RD_COUNTER_FIXED_LAYERS);
			candidate_level[row.sprite_number] = std::max(candidate_level[row.sprite_number], level);
			have_previous = true;
			previous_x_start = row.x_start;
			previous_y_start = row.y_start;
			previous_y_end = row.y_end;
			previous_code = row.code;
			previous_sprite_number = row.sprite_number;
		}

		// The Neo Geo list can present an alternating companion state on the
		// next video frame. A rows=1 result is the padded empty-list entry, so it
		// is never allowed to replace a real frame. Likewise, once a richer
		// frame has been established, a one-layer result is treated as the
		// collapsed companion state even when it contains the 96-row list.
		// Keep the measured count separate from the fixed output count. The
		// measured value still tells us whether this is the collapsed companion
		// state that must be discarded; the published capture set is always nine
		// planes below.
		const int measured_layer_count =
			std::min(both_changed_counter + 1, RD_COUNTER_FIXED_LAYERS);
		const bool empty_list_frame = counter_row_count <= 1;
		const bool collapsed_counter_frame = measured_layer_count <= 1 &&
			m_rd_counter_published_layer_count > 1;
		const bool reject_collapsed_frame = !use_x_only_formula &&
			(empty_list_frame || collapsed_counter_frame);
		bool hold_lower_frame = false;
		if (use_x_only_formula)
		{
			// CODE publishes this frame's result immediately. These fields are
			// retained only for diagnostics and must not feed back into selection.
			m_rd_counter_low_frame_streak = 0;
			m_rd_counter_published_layer_count = measured_layer_count;
		}
		else if (reject_collapsed_frame)
		{
			// Keep the richer published anchor indefinitely; the companion state
			// may last many frames, as observed in the live trace.
			m_rd_counter_low_frame_streak++;
			hold_lower_frame = true;
		}
		else if (measured_layer_count < m_rd_counter_published_layer_count)
		{
			m_rd_counter_low_frame_streak++;
			hold_lower_frame = m_rd_counter_low_frame_streak < 2;
			if (!hold_lower_frame)
				m_rd_counter_published_layer_count = measured_layer_count;
		}
		else
		{
			m_rd_counter_low_frame_streak = 0;
			m_rd_counter_published_layer_count = std::max(
				m_rd_counter_published_layer_count, measured_layer_count);
		}
		m_rd_counter_hold_frame = hold_lower_frame;

		// COUNTER resolves one decision per run and copies it to chained members.
		// CODE instead resolves each VRAM slot independently below.
		for (int ri = 0; ri < n; ri++)
		{
			const RdRun &run = runs[ri];
			if (run.raw_level == 0) continue;
			const u16 head_slot = run.head_slot;
			if (use_x_only_formula)
			{
				// No chain grouping and no temporal confirmation: every member
				// uses its own current candidate immediately.
				for (int i = 0; i < run.slot_len; i++)
				{
					const u16 slot = run_slot_pool[run.slot_off + i];
					const u8 candidate = candidate_level[slot] ? candidate_level[slot] : 1;
					counter_slot_bucket[slot] = claim_bucket_for_level(candidate, slot);
				}
				continue;
			}
			u8 candidate = 0;
			for (int i = 0; i < run.slot_len; i++)
				candidate = std::max(candidate, candidate_level[run_slot_pool[run.slot_off + i]]);
			if (candidate == 0) candidate = 1;
			const bool continuous =
				m_rd_counter_slot_last_seen_frame[head_slot] != 0 &&
				(m_rd_frame_counter - m_rd_counter_slot_last_seen_frame[head_slot]) == 1;
			const u8 stable = m_rd_counter_stable_group[head_slot];
			u8 selected = (!continuous ||
				m_rd_counter_previous_group[head_slot] == candidate) ? candidate : stable;
			if (hold_lower_frame)
			{
				// Common slots keep their last confirmed group. A slot appearing
				// only in the rejected companion state has no identity to match,
				// so place it in the far end of the retained stack rather than
				// collapsing the whole frame into layer 0.
				selected = continuous ? stable : (u8)std::max(1, m_rd_counter_published_layer_count);
				selected = (u8)std::min((int)selected, RD_DRAW_CAP);
			}

			for (int i = 0; i < run.slot_len; i++)
			{
				const u16 slot = run_slot_pool[run.slot_off + i];
				m_rd_counter_previous_group[slot] = candidate;
				m_rd_counter_stable_group[slot] = selected;
				m_rd_counter_slot_last_seen_frame[slot] = m_rd_frame_counter;
			}
			run_bucket[ri] = claim_bucket_for_level(selected, head_slot);
		}
		// Always materialize the complete diagnostic budget. This makes the
		// exported layer count exactly nine even when the current counter pass
		// only found fewer distinct boundaries; unused planes remain transparent.
		for (int level = 1; level <= RD_COUNTER_FIXED_LAYERS; level++)
			claim_bucket_for_level((u8)level, 0);

#ifdef __ANDROID__
		if (reject_collapsed_frame || hold_lower_frame)
		{
			__android_log_print(ANDROID_LOG_INFO, "QRD_COUNTER_DROP",
				"f=%u rows=%d x=%d y=%d code=%d jumps=%d both=%d measured=%d published=%d empty=%d collapsed=%d hold=%d low_streak=%d",
				m_rd_frame_counter, counter_row_count, x_counter, y_counter,
				code_changed_counter, sprite_jump_counter, both_changed_counter, measured_layer_count,
				m_rd_counter_published_layer_count, (int)empty_list_frame,
				(int)collapsed_counter_frame, (int)hold_lower_frame,
				m_rd_counter_low_frame_streak);
		}
		if ((m_rd_frame_counter % 300) == 0)
		{
			__android_log_print(ANDROID_LOG_INFO, "QRD_COUNTER",
				"f=%u x=%d y=%d code=%d jumps=%d both=%d measured=%d fixed=%d hold=%d low_streak=%d",
				m_rd_frame_counter, x_counter, y_counter, code_changed_counter,
				sprite_jump_counter, both_changed_counter, measured_layer_count, RD_COUNTER_FIXED_LAYERS,
				(int)hold_lower_frame,
				m_rd_counter_low_frame_streak);
		}
#endif
	};
	if (m_rd_layer_mode == RdLayerMode::OccupXY)
	{
		// Replace only the raw level consumed by the OCCUP pass. The rest of
		// OCCUPXY stays identical to OCCUPUD: same weighting, history,
		// threshold, promotion/demotion, and mapping hold.
		const std::vector<u8> xy_levels = compute_tile_raw_levels(1);
		for (int ri = 0; ri < n; ri++)
			runs[ri].raw_level = xy_levels[ri];
	}
	switch (m_rd_layer_mode)
	{
		case RdLayerMode::Deque:
		{
			// Existing temporal per-slot streak/high-water-mark ratchet --
			// see the big Table 1/Table 2 comment on m_rd_slot_want_level in
			// neogeo_spr.h.
			u8 ceiling_now = 0;
			bool won_this_frame[RD_DRAW_CAP + 1] = {};
			for (int ri = 0; ri < n; ri++)
			{
				const RdRun &run = runs[ri];
				if (run.raw_level == 0) continue;
				const u8 raw_level = run.raw_level;
				const u16 head_slot = run.head_slot;
				u8 &want_level = m_rd_slot_want_level[head_slot];
				u16 &want_streak = m_rd_slot_want_streak[head_slot];
				u32 &last_seen = m_rd_slot_last_seen_frame[head_slot];

				// A slot number is only a reliable per-object identity for
				// as long as it's continuously drawn -- ANY gap (even one
				// frame) is treated as the hardware potentially having
				// handed this slot number to a different object, so any
				// inherited proof is wiped immediately rather than only
				// after a long absence.
				if (want_level != 0 && (m_rd_frame_counter - last_seen) != 1)
				{
					want_level = 0;
					want_streak = 0;
				}
				last_seen = m_rd_frame_counter;

				// want_level is a HIGH-WATER MARK, not an exact-match target
				// -- see the header comment for why (blinking/oscillating
				// sprites keep their streak instead of resetting to 1).
				if (want_level == 0)
				{
					want_level = raw_level;
					want_streak = 1;
				}
				else
				{
					if (raw_level > want_level) want_level = raw_level;
					if (want_streak < 0xFFFF) want_streak++;
				}

				u8 eff_level;
				if (want_streak >= (u16)m_rd_hysteresis_window_frames)
				{
					eff_level = want_level;
					won_this_frame[want_level] = true;
				}
				else if (won_this_frame[raw_level])
				{
					eff_level = raw_level;
				}
				else
				{
					const u8 near_cap = (raw_level > 1) ? (u8)(raw_level - 1) : (u8)1;
					eff_level = std::min(std::max<u8>(ceiling_now, 1), near_cap);
				}
				ceiling_now = std::max(ceiling_now, eff_level);
				run_bucket[ri] = claim_bucket_for_level(eff_level, head_slot);
			}
			break;
		}
		case RdLayerMode::HardQ:
		{
			// Fixed, stateless quantization: fold the full 1..RD_DRAW_CAP
			// raw range down into kRdLayerModeMaxBuckets even bands. No
			// memory, no merging logic -- a hard ceiling on layer count no
			// matter how noisy Table 1's raw levels are this frame.
			constexpr int N = kRdLayerModeMaxBuckets;
			for (int ri = 0; ri < n; ri++)
			{
				const RdRun &run = runs[ri];
				if (run.raw_level == 0) continue;
				const int band = 1 + ((int)(run.raw_level - 1) * N) / RD_DRAW_CAP;
				run_bucket[ri] = claim_bucket_for_level((u8)std::min(band, N), run.head_slot);
			}
			break;
		}
		case RdLayerMode::DenseR:
		{
			// Dense rank of the distinct raw levels actually present this
			// frame -- uses exactly as many buckets as there are distinct
			// depths (no gaps from unused raw values), capped at N; extra
			// distinct levels beyond the cap all collapse onto the last
			// (farthest) bucket rather than being dropped.
			constexpr int N = kRdLayerModeMaxBuckets;
			bool present[RD_DRAW_CAP + 1] = {};
			for (const RdRun &run : runs) if (run.raw_level) present[run.raw_level] = true;
			u8 rank_of_level[RD_DRAW_CAP + 1] = {};
			int rank = 0;
			for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
				if (present[lvl]) rank_of_level[lvl] = (u8)std::min(++rank, N);
			for (int ri = 0; ri < n; ri++)
			{
				const RdRun &run = runs[ri];
				if (run.raw_level == 0) continue;
				run_bucket[ri] = claim_bucket_for_level(rank_of_level[run.raw_level], run.head_slot);
			}
			break;
		}
		case RdLayerMode::Occup:
		case RdLayerMode::OccupUd:
		case RdLayerMode::OccupXY:
		{
			// Occupancy-weighted merge with temporal smoothing: a raw level
			// backed by only a sliver of the current frame's content gets folded
			// into a neighboring, better-supported level. The support decision
			// comes from a rolling history, so a one-frame list change cannot
			// immediately renumber all the layers behind it. OCCUPUD/OCCUPXY add
			// asymmetric per-level confirmation on top of that signal.
			constexpr int N = kRdLayerModeMaxBuckets;
			const bool use_occupud_hysteresis =
				m_rd_layer_mode == RdLayerMode::OccupUd ||
				m_rd_layer_mode == RdLayerMode::OccupXY;
			int weight[RD_DRAW_CAP + 1] = {};
			if (m_rd_layer_mode == RdLayerMode::OccupXY)
			{
				// In OCCUPXY, occupancy is measured in occupied 16px tile rows
				// rather than whole hardware runs, matching the granularity used
				// by the per-row capture map below.
				for (u16 slot = 0; slot < MAX_SPRITES_PER_SCREEN; slot++)
					for (int row = 0; row < RD_CAPTURE_COLUMNS; row++)
						if (xy_slot_row_level[slot][row])
							weight[xy_slot_row_level[slot][row]]++;
			}
			else
			{
				for (const RdRun &run : runs)
					if (run.raw_level) weight[run.raw_level] += run.slot_len;
			}

			std::array<uint16_t, RD_DRAW_CAP + 1> sample{};
			for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
				sample[lvl] = (uint16_t)std::min(weight[lvl], 0xffff);
			if (m_rd_occup_history.size() >= RD_OCCUP_HISTORY_FRAMES)
			{
				const auto &oldest = m_rd_occup_history.front();
				for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
					m_rd_occup_weight_sum[lvl] -= oldest[lvl];
				m_rd_occup_history.pop_front();
			}
			for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
				m_rd_occup_weight_sum[lvl] += sample[lvl];
			m_rd_occup_history.push_back(sample);

			uint64_t total_weight_history = 0;
			for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
				total_weight_history += m_rd_occup_weight_sum[lvl];
			// Require the configured percentage of all supported sprite slots
			// over the rolling history before a raw level earns its own layer.
			// Comparing products avoids rounding the average and is equivalent
			// to a ceiling threshold.
			constexpr int occupancy_percent = 5;

			if (use_occupud_hysteresis)
			{
				// Keep each raw level's decision independently. This lets an
				// already-established layer survive a short list variation, while
				// a genuinely persistent new level can earn one extra layer without
				// making the whole stack jump at once.
				for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
				{
					const bool historically_supported = total_weight_history > 0 &&
						(uint64_t)m_rd_occup_weight_sum[lvl] * 100 >=
						total_weight_history * (uint64_t)occupancy_percent;
					if (historically_supported)
					{
						m_rd_occup_demote_streak[lvl] = 0;
						if (m_rd_occup_level_active[lvl])
						{
							m_rd_occup_promote_streak[lvl] = 0;
						}
						else
						{
							if (m_rd_occup_promote_streak[lvl] < RD_OCCUPUD_PROMOTE_FRAMES)
								m_rd_occup_promote_streak[lvl]++;
							if (m_rd_occup_promote_streak[lvl] >= RD_OCCUPUD_PROMOTE_FRAMES)
							{
								m_rd_occup_level_active[lvl] = 1;
								m_rd_occup_promote_streak[lvl] = 0;
							}
						}
					}
					else
					{
						m_rd_occup_promote_streak[lvl] = 0;
						if (!m_rd_occup_level_active[lvl])
						{
							m_rd_occup_demote_streak[lvl] = 0;
						}
						else
						{
							if (m_rd_occup_demote_streak[lvl] < RD_OCCUPUD_DEMOTE_FRAMES)
								m_rd_occup_demote_streak[lvl]++;
							if (m_rd_occup_demote_streak[lvl] >= RD_OCCUPUD_DEMOTE_FRAMES)
							{
								m_rd_occup_level_active[lvl] = 0;
								m_rd_occup_demote_streak[lvl] = 0;
							}
						}
					}
				}
			}
			u8 mapped_level[RD_DRAW_CAP + 1] = {};
			int kept = 0;
			u8 last_kept_level = 1;
			for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
			{
				const bool historically_supported = total_weight_history > 0 &&
					(uint64_t)m_rd_occup_weight_sum[lvl] * 100 >=
					total_weight_history * (uint64_t)occupancy_percent;
				const bool level_is_kept = use_occupud_hysteresis
					? (m_rd_occup_level_active[lvl] != 0)
					: historically_supported;
				if (level_is_kept && kept < N)
				{
					kept++;
					last_kept_level = (u8)lvl;
					mapped_level[lvl] = last_kept_level;
				}
				else
				{
					// Too sparse historically (or already at the cap) -- fold
					// into the most recent kept level. Notice that historically
					// supported levels are counted even when absent THIS frame;
					// that preserves the rank of a higher level while the lower
					// level briefly disappears.
					mapped_level[lvl] = last_kept_level;
				}
			}
			if (use_occupud_hysteresis)
			{
				// Hold the previous mapping for each raw level. This is deliberately
				// a small, cheap stabilizer: it reduces layer-content swapping, but
				// does not claim that a raw level is a permanent object identity.
				for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
				{
					const u8 desired = mapped_level[lvl] ? mapped_level[lvl] : 1;
					u8 &stable = m_rd_occupud_stable_mapping[lvl];
					u8 &candidate = m_rd_occupud_mapping_candidate[lvl];
					u8 &streak = m_rd_occupud_mapping_streak[lvl];
					if (stable == 0)
					{
						stable = desired;
						candidate = desired;
						streak = 0;
					}
					else if (desired == stable)
					{
						candidate = desired;
						streak = 0;
					}
					else
					{
						if (candidate != desired)
						{
							candidate = desired;
							streak = 1;
						}
						else if (streak < RD_OCCUPUD_MAPPING_FRAMES)
						{
							streak++;
						}
						if (streak >= RD_OCCUPUD_MAPPING_FRAMES)
						{
							stable = desired;
							streak = 0;
						}
					}
					mapped_level[lvl] = stable;
				}
			}
		#ifdef __ANDROID__
			if ((m_rd_frame_counter % 300) == 0)
			{
				__android_log_print(ANDROID_LOG_INFO, "QRD_OCCUP_HISTORY",
					"f=%u frames=%zu supported=%d total=%llu threshold=%d%% mode=%s pixel_capture=%d",
					m_rd_frame_counter, m_rd_occup_history.size(), kept,
					(unsigned long long)total_weight_history, occupancy_percent,
					m_rd_layer_mode == RdLayerMode::OccupXY ? "OCCUPXY" :
					use_occupud_hysteresis ? "OCCUPUD" : "OCCUP",
					m_rd_layer_mode == RdLayerMode::OccupXY ? 1 : 0);
			}
		#endif
			if (m_rd_layer_mode == RdLayerMode::OccupXY)
			{
				// The exact pixel path claims buckets during draw_sprites(). Keep
				// only the rolling-average raw-level -> compact-layer mapping here;
				// preclaiming slot/row buckets would collapse the result back to the
				// coarse vertical-strip behavior this mode is testing.
				for (int lvl = 1; lvl <= RD_DRAW_CAP; lvl++)
					m_rd_occupxy_level_map[lvl] = mapped_level[lvl] ? mapped_level[lvl] : 1;
			}
			else
			{
				for (int ri = 0; ri < n; ri++)
				{
					const RdRun &run = runs[ri];
					if (run.raw_level == 0) continue;
					run_bucket[ri] = claim_bucket_for_level(mapped_level[run.raw_level], run.head_slot);
				}
			}
			break;
		}
		case RdLayerMode::MinGate:
		{
			// Minimum run-size gate: a run only earns its raw level a fresh
			// bucket if its OWN member-slot count meets the threshold;
			// smaller runs (the broken/spurious kind) fold into the nearest
			// already-kept level instead. Cheaper, more local version of
			// Occup -- decided per-run as we go rather than by a full-frame
			// weight pass, so within a single frame an early small run can
			// fold into a fallback that a later, bigger run at the same raw
			// level then supersedes for everyone processed after it.
			constexpr int kMinRunSlots = 2;
			u8 mapped_level[RD_DRAW_CAP + 1] = {};
			bool kept_at[RD_DRAW_CAP + 1] = {};
			u8 last_kept_level = 1;
			for (int ri = 0; ri < n; ri++)
			{
				const RdRun &run = runs[ri];
				if (run.raw_level == 0) continue;
				const u8 lvl = run.raw_level;
				if (!kept_at[lvl] && run.slot_len >= kMinRunSlots)
				{
					kept_at[lvl] = true;
					mapped_level[lvl] = lvl;
					last_kept_level = lvl;
				}
				else if (!kept_at[lvl])
				{
					mapped_level[lvl] = last_kept_level;
				}
				run_bucket[ri] = claim_bucket_for_level(mapped_level[lvl], run.head_slot);
			}
			break;
		}
		case RdLayerMode::TileOcc:
			run_tile_grid_pass(1); // any opaque pixel at all counts as occupied
			break;
		case RdLayerMode::TileOccPer:
			run_tile_grid_pass(128); // 50% of 256 pixels
			break;
		case RdLayerMode::TileOccGraph:
			run_tile_graph_pass(kRdGraphMinOpaquePixels, kRdGraphMergeRadius);
			break;
		case RdLayerMode::StableGraph:
			run_tile_graph_pass(kRdGraphMinOpaquePixels, kRdGraphMergeRadius, true);
			break;
		case RdLayerMode::Painter:
			run_painter_pass(kRdGraphMinOpaquePixels);
			break;
		case RdLayerMode::Counter:
			run_counter_pass(false);
			break;
		case RdLayerMode::Code:
			run_counter_pass(true);
			break;
		default:
			break;
	}

	// ---- STAGE 3: stamp every slot in every run with its run's final bucket.
	for (int ri = 0; ri < n; ri++)
	{
		const RdRun &run = runs[ri];
		for (int i = 0; i < run.slot_len; i++)
		{
			const u16 slot = run_slot_pool[run.slot_off + i];
			m_rd_capture_slot[slot] = (m_rd_layer_mode == RdLayerMode::Code)
				? counter_slot_bucket[slot]
				: run_bucket[ri];
		}
	}

}

void neosprite_base_device::parse_sprites(int scanline)
{
	int y = 0;
	int rows = 0;
	u16 *sprite_list;

	int active_sprite_count = 0;

	/* select the active list */
	if (BIT(scanline, 0))
		sprite_list = &m_videoram_drawsource[0x8680];
	else
		sprite_list = &m_videoram_drawsource[0x8600];

	/* scan all sprites */
	for (u16 sprite_number = 0; sprite_number < MAX_SPRITES_PER_SCREEN; sprite_number++)
	{
		const u16 y_control = m_videoram_drawsource[0x8200 | sprite_number];

		/* if not chained, get Y position and height, otherwise use previous values */
		if (BIT(~y_control, 6))
		{
			y = 0x200 - (y_control >> 7);
			rows = y_control & 0x3f;
		}

		/* skip sprites with 0 rows */
		if (rows == 0)
			continue;

		if (!sprite_on_scanline(scanline, y, rows))
			continue;

		/* sprite is on this scanline, add it to active list */
		*sprite_list = sprite_number;

		sprite_list++;

		/* increment sprite count, and if we reached the max, bail out */
		active_sprite_count++;

		if (active_sprite_count == MAX_SPRITES_PER_LINE)
			break;
	}

	/* fill the rest of the sprite list with 0, including one extra entry */
	memset(sprite_list, 0, sizeof(sprite_list[0]) * (MAX_SPRITES_PER_LINE - active_sprite_count + 1));
}


TIMER_CALLBACK_MEMBER(neosprite_base_device::sprite_line_timer_callback)
{
	int scanline = param;

	/* we are at the beginning of a scanline -
	   we need to draw the previous scanline and parse the sprites on the current one */
	if (scanline != 0)
		screen().update_partial(scanline - 1);

	parse_sprites(scanline);

	/* let's come back at the beginning of the next line */
	scanline = (scanline + 1) % NEOGEO_VTOTAL;

	m_sprite_line_timer->adjust(screen().time_until_pos(scanline), scanline);
}


void neosprite_base_device::create_sprite_line_timer()
{
	m_sprite_line_timer = timer_alloc(FUNC(neosprite_base_device::sprite_line_timer_callback), this);
}


void neosprite_base_device::start_sprite_line_timer()
{
	m_sprite_line_timer->adjust(screen().time_until_pos(0));
}


u32 neosprite_base_device::get_region_mask(u8* rgn, u32 rgn_size)
{
	/* convert the sprite graphics data into a format that
	   allows faster blitting */

	/* get mask based on the length rounded up to the nearest
	   power of 2 */
	u32 mask = 0xffffffff;

	const u32 len = rgn_size;

	for (u32 bit = 0x80000000; bit != 0; bit >>= 1)
	{
		if ((len * 2 - 1) & bit)
			break;

		mask >>= 1;
	}

	return mask;
}

void neosprite_base_device::optimize_sprite_data()
{
	// this does nothing in this implementation, it's used by neosprite_optimized_device
	// m_sprite_gfx_address_mask gets set when the GFX region is assigned
	return;
}

void neosprite_base_device::set_optimized_sprite_data(u8* sprdata, u32 mask)
{
	return;
}

// these are for passing in pointers from the main system
void neosprite_base_device::set_sprite_region(u8* region_sprites, u32 region_sprites_size)
{
	m_region_sprites = region_sprites;
	m_region_sprites_size = region_sprites_size;
}

void neosprite_base_device::set_fixed_regions(u8* fix_cart, u32 fix_cart_size, memory_region* fix_bios)
{
	m_region_fixed = fix_cart;
	m_region_fixed_size = fix_cart_size;
	m_region_fixedbios = fix_bios;
}

void neosprite_base_device::set_pens(const pen_t* pens)
{
	m_pens = pens;
}



/*********************************************************************************************************************************/
/* Regular NeoGeo sprite handling - drawing directly from ROM (or RAM on NeoCD)                                                  */
/*                                                                                                                               */
/* note, we don't currently use this implementation, reference only                                                              */
/* if we do it will be important to ensure that all sprite regions are ^2 sizes - the optimized routine automatically allocates  */
/* ^2 sized regions when pre-decoding, but obviously we don't here, so if we want to be safe we'll have to adjust the actual     */
/* regions          (alternatively I could add an additional size check in the draw routine, but that would be slower)           */
/*********************************************************************************************************************************/

DEFINE_DEVICE_TYPE(NEOGEO_SPRITE_REGULAR, neosprite_regular_device, "neosprite_reg", "Neo-Geo Sprites (regular)")

neosprite_regular_device::neosprite_regular_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: neosprite_base_device(mconfig, NEOGEO_SPRITE_REGULAR, tag, owner, clock)
{
}

void neosprite_regular_device::set_sprite_region(u8* region_sprites, u32 region_sprites_size)
{
	m_region_sprites = region_sprites;
	m_region_sprites_size = region_sprites_size;

	const u32 mask = get_region_mask(m_region_sprites, m_region_sprites_size);
	const u32 proper_size = (mask + 1) >>1;

	printf("lengths %08x %08x m_region_sprites", region_sprites_size, proper_size);

	if (m_region_sprites_size != proper_size)
	{
		fatalerror("please use power of 2 region sizes with neosprite_base_device to ensure masking works correctly");
	}

	m_sprite_gfx_address_mask = mask;
}

inline bool neosprite_regular_device::draw_pixel(int romaddr, u32* dst, const pen_t *line_pens)
{
	u8 const *const src = m_region_sprites + (((romaddr &~0xff)>>1) | (((romaddr&0x8)^0x8)<<3) | ((romaddr & 0xf0)  >> 2));
	const int x = romaddr & 0x7;

	const u8 gfx = (BIT(src[0x3], x) << 3) |
						(BIT(src[0x1], x) << 2) |
						(BIT(src[0x2], x) << 1) |
						(BIT(src[0x0], x) << 0);

	if (gfx)
	{
		*dst = line_pens[gfx];
		return true;
	}
	return false;
}



/*********************************************************************************************************************************/
/* Regular NeoGeo sprite handling with pre-decode optimization                                                                   */
/*                                                                                                                               */
/* this is closer to the old MAME implementation where the 4bpp graphics have been expanded to an easier to draw 8bpp format     */
/* for additional speed                                                                                                          */
/*********************************************************************************************************************************/

DEFINE_DEVICE_TYPE(NEOGEO_SPRITE_OPTIMZIED, neosprite_optimized_device, "neosprite_opt", "Neo-Geo Sprites (optimized)")

neosprite_optimized_device::neosprite_optimized_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: neosprite_base_device(mconfig, NEOGEO_SPRITE_OPTIMZIED, tag, owner, clock)
	, m_spritegfx8(nullptr)
{
}

u32 neosprite_optimized_device::optimize_helper(std::vector<u8> &spritegfx, u8* region_sprites, u32 region_sprites_size)
{
	// convert the sprite graphics data into a format that allows faster blitting

	const u32 mask = get_region_mask(region_sprites, region_sprites_size);

	spritegfx.resize(mask + 1);

	const u8 *src = region_sprites;
	u8 *dest = &spritegfx[0];

	for (unsigned i = 0; i < region_sprites_size; i += 0x80, src += 0x80)
	{
		for (unsigned y = 0; y < 0x10; y++)
		{
			for (unsigned x = 0; x < 8; x++)
			{
				*(dest++) = (BIT(src[0x43 | (y << 2)], x) << 3) |
				((BIT(src[0x41 | (y << 2)], x)) << 2) |
				((BIT(src[0x42 | (y << 2)], x)) << 1) |
				((BIT(src[0x40 | (y << 2)], x)) << 0);
			}

			for (unsigned x = 0; x < 8; x++)
			{
				*(dest++) = (BIT(src[0x03 | (y << 2)], x) << 3) |
				(BIT(src[0x01 | (y << 2)], x) << 2) |
				(BIT(src[0x02 | (y << 2)], x) << 1) |
				(BIT(src[0x00 | (y << 2)], x) << 0);
			}
		}
	}

	return mask;
}


void neosprite_optimized_device::optimize_sprite_data()
{
	m_sprite_gfx_address_mask = optimize_helper(m_sprite_gfx, m_region_sprites, m_region_sprites_size);
	m_spritegfx8 = &m_sprite_gfx[0];
}

void neosprite_optimized_device::set_optimized_sprite_data(u8* sprdata, u32 mask)
{
	m_sprite_gfx_address_mask = mask;
	m_spritegfx8 = &sprdata[0];
}

inline bool neosprite_optimized_device::draw_pixel(int romaddr, u32* dst, const pen_t *line_pens)
{
	const u8 gfx = m_spritegfx8[romaddr];

	if (gfx)
	{
		*dst = line_pens[gfx];
		return true;
	}
	return false;
}


/*********************************************************************************************************************************/
/* MIDAS specific sprite handling                                                                                                */
/*                                                                                                                               */
/* this is used by the neogeo/midas.cpp hardware which is a reengineered NeoGeo, it has 8bbp tiles instead of 4bpp tiles         */
/* and uploads the zoom table.  The additional videoram buffering is a guess because 'hammer' is very glitchy without it         */
/*********************************************************************************************************************************/

DEFINE_DEVICE_TYPE(NEOGEO_SPRITE_MIDAS, neosprite_midas_device, "midassprite", "MIDAS Sprites")


neosprite_midas_device::neosprite_midas_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: neosprite_base_device(mconfig, NEOGEO_SPRITE_MIDAS, tag, owner, clock)
{
	m_bppshift = 8;
}


inline bool neosprite_midas_device::draw_pixel(int romaddr, u32* dst, const pen_t *line_pens)
{
	u8 const *const src = m_region_sprites + (((romaddr &~0xff)) | (((romaddr&0x8)^0x8)<<4) | ((romaddr & 0xf0)  >> 1));
	const int x = romaddr & 0x7;

	const u8 gfx =   (BIT(src[0x7],  x) << 7) |
						(BIT(src[0x6], x) << 6) |
						(BIT(src[0x5], x) << 5) |
						(BIT(src[0x4], x) << 4) |
						(BIT(src[0x3], x) << 3) |
						(BIT(src[0x2], x) << 2) |
						(BIT(src[0x1], x) << 1) |
						(BIT(src[0x0], x) << 0);

	if (gfx)
	{
		*dst = line_pens[gfx];
		return true;
	}
	return false;
}


void neosprite_midas_device::device_start()
{
	neosprite_base_device::device_start();

	m_videoram_buffer = std::make_unique<u16[]>(0x8000 + 0x800);
	m_videoram_drawsource = m_videoram_buffer.get();

	memset(m_videoram_buffer.get(), 0x00, (0x8000 + 0x800) * sizeof(u16));

}

void neosprite_midas_device::buffer_vram()
{
	memcpy(m_videoram_buffer.get(), m_videoram.get(), (0x8000 + 0x800) * sizeof(u16));
}

inline void neosprite_midas_device::draw_fixed_layer_2pixels(u32*&pixel_addr, int offset, u8* gfx_base, const pen_t* char_pens)
{
	u8 data = ((gfx_base[(offset * 2) + 0] & 0x0f) << 0) | ((gfx_base[(offset * 2) + 1] & 0x0f) << 4);
	if (data)
		*pixel_addr = char_pens[data];
	pixel_addr++;

	data = ((gfx_base[(offset * 2) + 0] & 0xf0) >> 4) | ((gfx_base[(offset * 2) + 1] & 0xf0) << 0);
	if (data)
		*pixel_addr = char_pens[data];
	pixel_addr++;
}

void neosprite_midas_device::set_sprite_region(u8* region_sprites, u32 region_sprites_size)
{
	m_region_sprites = region_sprites;
	m_region_sprites_size = region_sprites_size;
	m_sprite_gfx_address_mask = get_region_mask(m_region_sprites, m_region_sprites_size);
}
