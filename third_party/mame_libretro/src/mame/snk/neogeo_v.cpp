// license:BSD-3-Clause
// copyright-holders:Bryan McPhail,Ernesto Corvi,Andrew Prime,Zsolt Vasvari
// thanks-to:Fuzz
/***************************************************************************

    Neo-Geo hardware

****************************************************************************/

#include "emu.h"
#include "neogeo.h"
#include "video/resnet.h"
#include "mame_retrodepth_hook.h"
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define VERBOSE     (0)

// RetroDepth debug aid: burn a small number into the top-left corner of a
// captured layer's own pixel buffer, so the layer's capture-bucket index is
// visible on-device at the exact same time as its content, in whichever 3D
// slot that layer ends up rendered at -- no separate UI/overlay needed, no
// font asset, just a hardcoded 3x5 pixel digit table stamped directly into
// the ARGB buffer before it's exported.
static void rd_stamp_number(uint32_t *buf, int w, int h, int number, int x0, int y0, int scale)
{
	static const uint8_t kDigitFont[10][5] = {
		{0b111, 0b101, 0b101, 0b101, 0b111}, // 0
		{0b010, 0b110, 0b010, 0b010, 0b111}, // 1
		{0b111, 0b001, 0b111, 0b100, 0b111}, // 2
		{0b111, 0b001, 0b111, 0b001, 0b111}, // 3
		{0b101, 0b101, 0b111, 0b001, 0b001}, // 4
		{0b111, 0b100, 0b111, 0b001, 0b111}, // 5
		{0b111, 0b100, 0b111, 0b101, 0b111}, // 6
		{0b111, 0b001, 0b010, 0b010, 0b010}, // 7
		{0b111, 0b101, 0b111, 0b101, 0b111}, // 8
		{0b111, 0b101, 0b111, 0b001, 0b111}, // 9
	};
	if (number < 0 || w <= 0 || h <= 0) return;
	char digits[8];
	const int n = std::snprintf(digits, sizeof(digits), "%d", number);
	if (n <= 0) return;

	const int digit_w = 3 * scale;
	const int digit_h = 5 * scale;
	const int spacing = scale;
	const int total_w = n * digit_w + (n - 1) * spacing;

	// Opaque black backdrop box (with a 1px margin) so the digits stay
	// legible regardless of what's underneath.
	for (int by = -1; by <= digit_h; ++by)
		for (int bx = -1; bx <= total_w; ++bx)
		{
			const int px = x0 + bx, py = y0 + by;
			if (px < 0 || py < 0 || px >= w || py >= h) continue;
			buf[(size_t)py * w + px] = 0xFF000000u;
		}

	int cursor_x = x0;
	for (int di = 0; di < n; ++di)
	{
		const int d = digits[di] - '0';
		if (d < 0 || d > 9) { cursor_x += digit_w + spacing; continue; }
		for (int row = 0; row < 5; ++row)
		{
			const uint8_t bits = kDigitFont[d][row];
			for (int col = 0; col < 3; ++col)
			{
				if (!(bits & (1 << (2 - col)))) continue;
				for (int sy = 0; sy < scale; ++sy)
					for (int sx = 0; sx < scale; ++sx)
					{
						const int px = cursor_x + col * scale + sx;
						const int py = y0 + row * scale + sy;
						if (px < 0 || py < 0 || px >= w || py >= h) continue;
						buf[(size_t)py * w + px] = 0xFFFFFFFFu;
					}
			}
		}
		cursor_x += digit_w + spacing;
	}
}


/*************************************
 *
 *  Palette handling
 *
 *************************************/

void neogeo_base_state::create_rgb_lookups()
{
	static const int resistances[] = {3900, 2200, 1000, 470, 220};

	/* compute four sets of weights - with or without the pulldowns -
	   ensuring that we use the same scaler for all */
	double weights_normal[5];
	double scaler = compute_resistor_weights(0, 255, -1,
											5, resistances, weights_normal, 0, 0,
											0, nullptr, nullptr, 0, 0,
											0, nullptr, nullptr, 0, 0);

	double weights_dark[5];
	compute_resistor_weights(0, 255, scaler,
							5, resistances, weights_dark, 8200, 0,
							0, nullptr, nullptr, 0, 0,
							0, nullptr, nullptr, 0, 0);

	double weights_shadow[5];
	compute_resistor_weights(0, 255, scaler,
							5, resistances, weights_shadow, 150, 0,
							0, nullptr, nullptr, 0, 0,
							0, nullptr, nullptr, 0, 0);

	double weights_dark_shadow[5];
	compute_resistor_weights(0, 255, scaler,
							5, resistances, weights_dark_shadow, 1.0 / ((1.0 / 8200) + (1.0 / 150)), 0,
							0, nullptr, nullptr, 0, 0,
							0, nullptr, nullptr, 0, 0);

	for (int i = 0; i < 32; i++)
	{
		int const i4 = BIT(i, 4);
		int const i3 = BIT(i, 3);
		int const i2 = BIT(i, 2);
		int const i1 = BIT(i, 1);
		int const i0 = BIT(i, 0);
		m_palette_lookup[i][0] = combine_weights(weights_normal, i0, i1, i2, i3, i4);
		m_palette_lookup[i][1] = combine_weights(weights_dark, i0, i1, i2, i3, i4);
		m_palette_lookup[i][2] = combine_weights(weights_shadow, i0, i1, i2, i3, i4);
		m_palette_lookup[i][3] = combine_weights(weights_dark_shadow, i0, i1, i2, i3, i4);
	}
}

void neogeo_base_state::set_pens()
{
	const pen_t *pen_base = m_palette->pens() + m_palette_bank + (m_screen_shadow ? 0x2000 : 0);
	m_sprgen->set_pens(pen_base);
	m_bg_pen = pen_base + 0xfff;
}


void neogeo_base_state::set_screen_shadow(int state)
{
	m_screen_shadow = state;
	set_pens();
}


void neogeo_base_state::set_palette_bank(int state)
{
	m_palette_bank = state ? 0x1000 : 0;
	set_pens();
}


uint16_t neogeo_base_state::paletteram_r(offs_t offset)
{
	return m_paletteram[m_palette_bank + offset];
}


void neogeo_base_state::paletteram_w(offs_t offset, uint16_t data)
{
	offset += m_palette_bank;
	m_paletteram[offset] = data;

	uint8_t const dark = data >> 15;
	uint8_t const r = ((data >> 14) & 0x1) | ((data >> 7) & 0x1e);
	uint8_t const g = ((data >> 13) & 0x1) | ((data >> 3) & 0x1e);
	uint8_t const b = ((data >> 12) & 0x1) | ((data << 1) & 0x1e);

	m_palette->set_pen_color(offset,
								m_palette_lookup[r][dark],
								m_palette_lookup[g][dark],
								m_palette_lookup[b][dark]); // normal

	m_palette->set_pen_color(offset + 0x2000,
								m_palette_lookup[r][dark+2],
								m_palette_lookup[g][dark+2],
								m_palette_lookup[b][dark+2]); // shadow
}


/*************************************
 *
 *  Video system start
 *
 *************************************/

void neogeo_base_state::video_start()
{
	create_rgb_lookups();

	m_paletteram.resize(0x1000 * 2, 0);

	m_screen_shadow = false;
	m_palette_bank = 0;

	save_item(NAME(m_paletteram));
	save_item(NAME(m_screen_shadow));
	save_item(NAME(m_palette_bank));

	set_pens();
}


/*************************************
 *
 *  Video system reset
 *
 *************************************/

void neogeo_base_state::video_reset()
{
}


/*************************************
 *
 *  Video update
 *
 *************************************/

uint32_t neogeo_base_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	// --- RetroDepth synthesized z-buffer ---
	// Neo Geo has no tilemap planes to capture the way CPS1/Konami/System 16 do:
	// the entire display is this function -- a flat backdrop fill, one sprite
	// pass, and the fix (text/HUD) layer. So rather than exporting named layers,
	// fabricate the per-pixel depth channel snes9x reports natively, and let
	// questretrodepth's existing ZBuffer path slice the composite frame by it.
	// See neosprite_base_device::rd_compute_slot_depths() for how a sprite's
	// plane is chosen.
	const rectangle &vis = screen.visible_area();
	const bool rd_on = retrodepth_active();

	static std::vector<uint8_t> s_depth;
	static int s_depth_stride = 0;

	// Capped true independent capture experiment (see rd_claim_capture_slot()'s
	// comment in neogeo_spr.h): RD_DRAW_CAP full-canvas planes, one per
	// distinct sprite actually claimed this frame (first-come, real draw
	// order, no z-range bucketing). Backdrop and the fix layer are NOT
	// captured independently here -- they, plus every sprite beyond the
	// cap, stay only in the ordinary flat composite exported below as
	// "neogeo_base", so nothing goes missing, only the first N sprites get
	// real depth separation.
	static std::vector<uint32_t> s_captures;
	static int s_capture_stride = 0, s_capture_plane_stride = 0;
	constexpr uint32_t k_capture_transparent = 0x00000001u; // same sentinel as k_fix_sentinel below

	if (rd_on)
	{
		const int bw = bitmap.width(), bh = bitmap.height();
		if (s_depth_stride != bw || s_depth.size() != (size_t)bw * bh)
		{
			s_depth_stride = bw;
			s_depth.assign((size_t)bw * bh, neosprite_base_device::RD_Z_BACKDROP);
		}
		if (s_capture_stride != bw || s_capture_plane_stride != bw * bh ||
		    s_captures.size() != (size_t)bw * bh * neosprite_base_device::RD_DRAW_CAP)
		{
			s_capture_stride = bw;
			s_capture_plane_stride = bw * bh;
			s_captures.assign((size_t)bw * bh * neosprite_base_device::RD_DRAW_CAP, k_capture_transparent);
		}

		// Start of frame: the backdrop covers everything not drawn over, and the
		// slot->plane table (and the capture-slot claim table) only need
		// rebuilding once per frame.
		if (cliprect.min_y <= vis.min_y)
		{
			std::fill(s_depth.begin(), s_depth.end(), neosprite_base_device::RD_Z_BACKDROP);
			std::fill(s_captures.begin(), s_captures.end(), k_capture_transparent);
			// Hardcoded Neo Geo composition settings. These used to be mutable
			// through the QRD JSON/profile bridge; keeping them here makes the
			// normal emulation path self-contained and allocation-free.
			m_sprgen->rd_set_hysteresis_window(30);
			m_sprgen->rd_set_layer_mode((int)neosprite_base_device::RdLayerMode::OccupXY);
			m_sprgen->rd_compute_slot_depths();
		}

		m_sprgen->rd_set_depth_target(s_depth.data(), s_depth_stride);
		m_sprgen->rd_set_capture_target(s_captures.data(), s_capture_stride, s_capture_plane_stride);
	}

	// fill with background color first
	bitmap.fill(*m_bg_pen, cliprect);

	m_sprgen->draw_sprites(bitmap, cliprect.min_y);

	if (rd_on)
	{
		m_sprgen->rd_set_depth_target(nullptr, 0);
		m_sprgen->rd_set_capture_target(nullptr, 0, 0);
	}

	m_sprgen->draw_fixed_layer(bitmap, cliprect.min_y);

	if (rd_on)
	{
		// The fix layer draws over sprites and is a single depth, so instead of
		// threading a depth pointer through draw_fixed_layer_2pixels() as well,
		// re-render this scanline of it into a scratch row and stamp RD_Z_FIX
		// wherever it actually put a pixel.
		static bitmap_rgb32 s_fix;
		if (s_fix.width() != bitmap.width() || s_fix.height() != bitmap.height())
			s_fix.allocate(bitmap.width(), bitmap.height());

		const int line = cliprect.min_y;
		constexpr uint32_t k_fix_sentinel = 0x00000001u;

		s_fix.fill(k_fix_sentinel, cliprect);
		m_sprgen->draw_fixed_layer(s_fix, line);

		uint8_t *drow = s_depth.data() + (size_t)line * s_depth_stride;
		for (int x = cliprect.min_x; x <= cliprect.max_x; ++x)
		{
			if (s_fix.pix(line, x) != k_fix_sentinel)
				drow[x] = neosprite_base_device::RD_Z_FIX;
		}

		// End of frame: hand over the visible-area crop, sized to match the
		// composite frame the libretro video callback ships.
		if (cliprect.max_y >= vis.max_y)
		{
			const int w = vis.width(), h = vis.height();
			static std::vector<uint8_t> s_out;
			s_out.resize((size_t)w * h);

			for (int row = 0; row < h; ++row)
			{
				std::memcpy(&s_out[(size_t)row * w],
				            s_depth.data() + (size_t)(vis.min_y + row) * s_depth_stride + vis.min_x,
				            w);
			}

			// Debug-only: raw resolved palette table (256 banks * 16 colors), for
			// the palette-swatch capture tool (questretrodepth's
			// neogeo_palette_debug.cpp). Same pen_base construction as set_pens()
			// above, so these are the exact ARGB values sprites actually draw with.
			{
				const pen_t *pal_pen_base = m_palette->pens() + m_palette_bank + (m_screen_shadow ? 0x2000 : 0);
				static uint32_t s_pal_argb[256 * 16];
				for (int i = 0; i < 256 * 16; ++i)
					s_pal_argb[i] = (uint32_t)pal_pen_base[i];
				retrodepth_write_palette_data(s_pal_argb, 256 * 16);
			}

			retrodepth_write_zbuffer(s_out.data(), (uint32_t)w, (uint32_t)h);

			// "neogeo_base": the ordinary flat composite (backdrop + every
			// sprite + fix layer, unchanged), exported as one plain layer so
			// nothing is missing from the scene -- only the first
			// RD_DRAW_CAP sprites additionally get pulled out to their own
			// real depth below. Alpha is forced opaque; MAME's bitmap_rgb32
			// pixels don't carry a meaningful top byte.
			//
			// DEBUG: kNeogeoHideBaseLayer skips exporting it entirely, so
			// only the RD_DRAW_CAP captured "neogeo_drawN" layers + fix are
			// visible -- useful for isolating the real per-slot depth
			// capture from the flat catch-all while pattern-spotting.
			// Anything not among the first RD_DRAW_CAP captured slots this
			// frame simply won't be drawn at all while this is on. Flip back
			// to false to restore the normal "nothing missing" behavior.
			constexpr bool kNeogeoHideBaseLayer = true;
			if (!kNeogeoHideBaseLayer)
			{
				static std::vector<uint32_t> s_base_out;
				s_base_out.resize((size_t)w * h);
				for (int row = 0; row < h; ++row)
				{
					uint32_t *dst = &s_base_out[(size_t)row * w];
					for (int col = 0; col < w; ++col)
						dst[col] = bitmap.pix(vis.min_y + row, vis.min_x + col) | 0xFF000000u;
				}
				retrodepth_write_layer(0, "neogeo_base", s_base_out.data(), nullptr, (uint32_t)w, (uint32_t)h);
			}

			// "neogeo_fix": the HUD/text/"insert coin" layer, captured on its
			// own (s_fix already accumulated the whole frame -- see the
			// per-scanline fill+draw above) so the app can force it to always
			// be the single nearest layer, as an explicit exception to the
			// palette-based ordering below (it isn't a sprite and has no
			// palette bank in that sense).
			{
				static std::vector<uint32_t> s_fix_out;
				s_fix_out.resize((size_t)w * h);
				for (int row = 0; row < h; ++row)
				{
					uint32_t *dst = &s_fix_out[(size_t)row * w];
					for (int col = 0; col < w; ++col)
					{
						const uint32_t px = s_fix.pix(vis.min_y + row, vis.min_x + col);
						// Besides the sentinel (never drawn this frame), also
						// treat a pure-black fix pixel as transparent: real
						// Neo Geo games commonly paint solid-black fix tiles
						// down the left/right overscan columns as a border
						// mask, which is invisible on real hardware (same
						// color as everything else outside the visible
						// area) but shows up here as opaque black vertical
						// bars once neogeo_fix is pulled out to its own
						// forced-nearest depth in VR.
						const bool is_border_black = (px & 0x00FFFFFFu) == 0u;
						dst[col] = (px == 0x00000001u || is_border_black) ? 0u : (px | 0xFF000000u);
					}
				}
				retrodepth_write_layer(255, "neogeo_fix", s_fix_out.data(), nullptr, (uint32_t)w, (uint32_t)h);
			}

			// Export each capped, true-independent capture slot as a named
			// layer, through the exact same generic path CPS1/CPS2/Konami
			// already use -- this is what actually fixes the occlusion hole
			// for the first RD_DRAW_CAP sprites, vs. the zbuffer above which
			// still just tags one shared composite.
			{
				static std::vector<uint32_t> s_capture_out;
				s_capture_out.resize((size_t)w * h);
				static char s_capture_names[neosprite_base_device::RD_DRAW_CAP][16];
				static bool s_names_init = false;
				if (!s_names_init)
				{
					for (int b = 0; b < neosprite_base_device::RD_DRAW_CAP; ++b)
						std::snprintf(s_capture_names[b], sizeof(s_capture_names[b]), "neogeo_draw%d", b);
					s_names_init = true;
				}
				// Only export buckets actually claimed THIS frame -- b >=
				// active_count never got a level_bucket[] entry this frame,
				// so its z_order/headnum are stale leftovers and its capture
				// pixels are all-transparent. Exporting those anyway used to
				// mean every frame wrote a fixed 30 "neogeo_drawN" layers no
				// matter how many were really in use, which shows up as
				// empty layers floating in the depth stack (worse with the
				// hysteresis ratchet, since merging runs into fewer buckets
				// leaves more buckets unclaimed). Not calling
				// retrodepth_write_layer() for an unclaimed bucket lets
				// retrodepth_commit()'s stale-clear drop it from this
				// frame's read set entirely, same mechanism that already
				// makes a layer disappear when a driver just stops drawing
				// it.
				const int active_count = m_sprgen->rd_capture_active_count();
				for (int b = 0; b < active_count; ++b)
				{
					const uint32_t *plane = s_captures.data() + (size_t)b * s_capture_plane_stride;
					for (int row = 0; row < h; ++row)
					{
						std::memcpy(&s_capture_out[(size_t)row * w],
						            plane + (size_t)(vis.min_y + row) * s_capture_stride + vis.min_x,
						            w * sizeof(uint32_t));
					}
					// z_order = the column-collision level that claimed this
					// slot this frame, scaled into 1..254 -- questretrodepth
					// reads this to order these layers front-to-back by real
					// occupancy depth (0 furthest/neogeo_base, 255
					// nearest/neogeo_fix), not by arbitrary claim order.
					// DEBUG: stamp the real VRAM slot number on the LEFT (not
					// the bucket index -- see rd_capture_slot_headnum()) and
					// this bucket's own layer index (b, i.e. which
					// "neogeo_drawN" this is) on the RIGHT, so both are
					// visible on-device at the same time as the layer's
					// content. Vertical position is scattered per-slot (a
					// cheap multiplicative hash of the slot number, stable
					// frame to frame for the same object) rather than fixed
					// at the same corner for every layer -- with many layers
					// stacked in depth, fixed-position labels all land on
					// the same screen X/Y and overlap into unreadable mush
					// when viewed head-on; different rows per layer keep
					// them legible.
					{
						const u16 headnum = m_sprgen->rd_capture_slot_headnum(b);
						const uint32_t hash = (uint32_t)headnum * 2654435761u;
						const int label_h = 5 * 3;
						const int y_range = (h > label_h + 4) ? (h - label_h - 4) : 1;
						const int y0 = 2 + (int)(hash % (uint32_t)y_range);
						rd_stamp_number(s_capture_out.data(), w, h, headnum, 2, y0, 3);

						// Right-side label: this bucket's own layer index.
						// Right-align it with a fixed margin instead of
						// hashing its X, since unlike the slot number this
						// value is small and bounded (0..RD_DRAW_CAP-1) and
						// there's no benefit to scattering it horizontally.
						// Width matches rd_stamp_number()'s own glyph
						// geometry at scale=3 (digit_w=9, spacing=3) plus its
						// 1px backdrop margin on each side.
						const int digits = (b >= 10) ? 2 : 1;
						const int total_w = digits * 9 + (digits - 1) * 3;
						const int x0 = w - 3 - total_w;
						if (x0 >= 0)
							rd_stamp_number(s_capture_out.data(), w, h, (u16)b, x0, y0, 3);
					}
					retrodepth_write_layer((uint32_t)m_sprgen->rd_capture_slot_zorder(b), s_capture_names[b],
					                        s_capture_out.data(), nullptr, (uint32_t)w, (uint32_t)h);
				}
			}

			retrodepth_commit();
		}
	}

	return 0;
}


/*************************************
 *
 *  Video control
 *
 *************************************/

uint16_t neogeo_base_state::get_video_control()
{
	/*
	    The format of this very important location is:  AAAA AAAA A??? BCCC

	    A is the raster line counter. mosyougi relies solely on this to do the
	      raster effects on the title screen; sdodgeb loops waiting for the top
	      bit to be 1; zedblade heavily depends on it to work correctly (it
	      checks the top bit in the IRQ2 handler).
	    B is definitely a PAL/NTSC flag. (LSPC2 only) Evidence:
	      1) trally changes the position of the speed indicator depending on
	         it (0 = lower 1 = higher).
	      2) samsho3 sets a variable to 60 when the bit is 0 and 50 when it's 1.
	         This is obviously the video refresh rate in Hz.
	      3) samsho3 sets another variable to 256 or 307. This could be the total
	         screen height (including vblank), or close to that.
	      Some games (e.g. lstbld2, samsho3) do this (or similar):
	      bclr    #$0, $3c000e.l
	      when the bit is set, so 3c000e (whose function is unknown) has to be
	      related
	    C animation counter lower 3 bits
	*/

	// the vertical counter chain goes from 0xf8 - 0x1ff
	uint16_t v_counter = m_screen->vpos() + 0x100;

	if (v_counter >= 0x200)
		v_counter = v_counter - NEOGEO_VTOTAL;

	uint16_t const ret = (v_counter << 7) | (m_sprgen->get_auto_animation_counter() & 0x0007);

	if (!machine().side_effects_disabled())
	{
		if (VERBOSE)
			logerror("%s: video_control read (%04x)\n", machine().describe_context(), ret);
	}

	return ret;
}


void neogeo_base_state::set_video_control(uint16_t data)
{
	if (VERBOSE) logerror("%s: video control write %04x\n", machine().describe_context(), data);

	m_sprgen->set_auto_animation_speed(data >> 8);
	m_sprgen->set_auto_animation_disabled(BIT(data, 3));

	set_display_position_interrupt_control(data & 0x00f0);
}


uint16_t neogeo_base_state::video_register_r(address_space &space, offs_t offset, uint16_t mem_mask)
{
	uint16_t ret;

	// accessing the LSB only is not mapped
	if (mem_mask == 0x00ff)
		ret = unmapped_r(space) & 0x00ff;
	else
	{
		switch (offset)
		{
		default:
		case 0x00:
		case 0x01: ret = m_sprgen->get_videoram_data(); break;
		case 0x02: ret = m_sprgen->get_videoram_modulo(); break;
		case 0x03: ret = get_video_control(); break;
		}
	}

	return ret;
}


void neogeo_base_state::video_register_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	// accessing the LSB only is not mapped
	if (mem_mask != 0x00ff)
	{
		// accessing the MSB only stores same data in MSB and LSB
		if (mem_mask == 0xff00)
			data = (data & 0xff00) | (data >> 8);

		switch (offset)
		{
		case 0x00: m_sprgen->set_videoram_offset(data); break;
		case 0x01: m_sprgen->set_videoram_data(data); break;
		case 0x02: m_sprgen->set_videoram_modulo(data); break;
		case 0x03: set_video_control(data); break;
		case 0x04: set_display_counter_msb(data); break;
		case 0x05: set_display_counter_lsb(data); break;
		case 0x06: acknowledge_interrupt(data); break;
		case 0x07: break; // d0: pause timer for 32 lines when in PAL mode (LSPC2 only)
		}
	}
}
