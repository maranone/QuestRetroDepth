// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    Sega System 16B hardware

***************************************************************************/

#include "emu.h"
#include "segas16b.h"
#include "mame_retrodepth_hook.h"



//-------------------------------------------------
//  video_start - initialize the video system
//-------------------------------------------------

void segas16b_state::video_start()
{
	// initialize the tile/text layers
	m_segaic16vid->tilemap_init( 0, m_tilemap_type, 0x000, 0, 2);
}


//-------------------------------------------------
//  screen_update - render all graphics
//-------------------------------------------------

uint32_t segas16b_state::screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	// if no drawing is happening, fill with black and get out
	if (!m_segaic16vid->m_display_enable)
	{
		bitmap.fill(m_palette->black_pen(), cliprect);
		return 0;
	}

	// start the sprites drawing
	if (m_sprites.found())
		m_sprites->draw_async(cliprect);

	// reset priorities
	screen.priority().fill(0, cliprect);

	// draw background opaquely first, not setting any priorities
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_BACKGROUND, 0 | TILEMAP_DRAW_OPAQUE, 0x00);
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_BACKGROUND, 1 | TILEMAP_DRAW_OPAQUE, 0x00);

	// draw background again, just to set the priorities on non-transparent pixels
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_BACKGROUND, 0, 0x01);
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_BACKGROUND, 1, 0x02);

	// draw foreground
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_FOREGROUND, 0, 0x02);
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_FOREGROUND, 1, 0x04);

	// text layer
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_TEXT, 0, 0x04);
	m_segaic16vid->tilemap_draw( screen, bitmap, cliprect, 0, segaic16_video_device::TILEMAP_TEXT, 1, 0x08);

	// mix in sprites
	if (!m_sprites)
		return 0;
	bitmap_ind16 &sprites = m_sprites->bitmap();
	m_sprites->iterate_dirty_rects(
			cliprect,
			[this, &screen, &bitmap, &sprites] (rectangle const &rect)
			{
				for (int y = rect.min_y; y <= rect.max_y; y++)
				{
					uint16_t *const dest = &bitmap.pix(y);
					uint16_t const *const src = &sprites.pix(y);
					uint8_t const *const pri = &screen.priority().pix(y);
					for (int x = rect.min_x; x <= rect.max_x; x++)
					{
						// only process written pixels
						uint16_t const pix = src[x];
						if (pix != 0xffff)
						{
							// compare sprite priority against tilemap priority
							int const priority = (pix >> 10) & 3;
							if ((1 << priority) > pri[x])
							{
								if ((pix & 0x03f0) == 0x03f0)
								{
									// if the color is set to maximum, shadow pixels underneath us
									dest[x] += m_palette_entries;
								}
								else
								{
									// otherwise, just add in sprite palette base
									dest[x] = m_spritepalbase | (pix & 0x3ff);
								}
							}
						}
					}
				}
			});

	// --- RetroDepth layer export ---
	if (retrodepth_active())
	{
		const rectangle& vis = screen.visible_area();
		const int bw = bitmap.width(), bh = bitmap.height();

		static bitmap_ind16 s_bg, s_fg, s_text;
		if (s_bg.width() != bw || s_bg.height() != bh) {
			s_bg.allocate(bw, bh);
			s_fg.allocate(bw, bh);
			s_text.allocate(bw, bh);
		}

		// Re-run each layer's own draw calls into a private scratch bitmap so
		// we can export it in isolation -- tilemap_draw only reads VRAM/scroll
		// state (aside from screen.priority(), which is scratch here too and
		// safe to clobber since the real sprite mix above already consumed it
		// for this frame).
		s_bg.fill(0xffff, cliprect);
		screen.priority().fill(0, cliprect);
		m_segaic16vid->tilemap_draw(screen, s_bg, cliprect, 0, segaic16_video_device::TILEMAP_BACKGROUND, 0 | TILEMAP_DRAW_OPAQUE, 0x00);
		m_segaic16vid->tilemap_draw(screen, s_bg, cliprect, 0, segaic16_video_device::TILEMAP_BACKGROUND, 1 | TILEMAP_DRAW_OPAQUE, 0x00);

		s_fg.fill(0xffff, cliprect);
		screen.priority().fill(0, cliprect);
		m_segaic16vid->tilemap_draw(screen, s_fg, cliprect, 0, segaic16_video_device::TILEMAP_FOREGROUND, 0, 0x00);
		m_segaic16vid->tilemap_draw(screen, s_fg, cliprect, 0, segaic16_video_device::TILEMAP_FOREGROUND, 1, 0x00);

		s_text.fill(0xffff, cliprect);
		screen.priority().fill(0, cliprect);
		m_segaic16vid->tilemap_draw(screen, s_text, cliprect, 0, segaic16_video_device::TILEMAP_TEXT, 0, 0x00);
		m_segaic16vid->tilemap_draw(screen, s_text, cliprect, 0, segaic16_video_device::TILEMAP_TEXT, 1, 0x00);

		if (cliprect.max_y >= vis.max_y)
		{
			const rgb_t* colors = m_palette->palette()->entry_list_adjusted();
			const int n_pens = (int)m_palette->palette()->num_colors();

			static uint32_t pal_data[256 * 16];
			memset(pal_data, 0, sizeof(pal_data));
			int copy_count = (n_pens < 256 * 16) ? n_pens : (256 * 16);
			for (int i = 0; i < copy_count; ++i)
				pal_data[i] = (uint32_t)colors[i];
			retrodepth_write_palette_data(pal_data, 256 * 16);

			const int w = vis.width(), h = vis.height();
			std::vector<uint32_t> packed(w * h);
			std::vector<uint16_t> owners(w * h);

			auto write_layer = [&](uint32_t z_order, const char* name,
			                       bitmap_ind16& bmp, bool has_owners) {
				for (int row = 0; row < h; ++row) {
					for (int col = 0; col < w; ++col) {
						uint16_t pen = bmp.pix(vis.min_y + row, vis.min_x + col);
						int idx = row * w + col;
						bool transparent = pen == 0xffff;
						if (has_owners && transparent)
							packed[idx] = 0x00000001u;
						else
							packed[idx] = (!transparent && pen < (uint16_t)n_pens) ? (uint32_t)colors[pen] : 0u;
						if (has_owners)
							owners[idx] = (!transparent) ? (uint16_t)(pen / 16) : RD_OWNER_NONE;
					}
				}
				retrodepth_write_layer(z_order, name, packed.data(),
				                       has_owners ? owners.data() : nullptr, w, h);
			};

			write_layer(0, "background", s_bg,   false);
			write_layer(1, "foreground", s_fg,   true);
			write_layer(2, "text",       s_text, true);

			// Sprites: already rendered in m_sprites->bitmap() above, no need
			// to redraw -- same 0xffff "unwritten" sentinel as tilemaps.
			if (m_sprites)
			{
				bitmap_ind16& spr = m_sprites->bitmap();
				for (int row = 0; row < h; ++row) {
					for (int col = 0; col < w; ++col) {
						int idx = row * w + col;
						uint16_t pix = spr.pix(vis.min_y + row, vis.min_x + col);
						bool transparent = (pix == 0xffff);
						uint16_t pen = transparent ? 0 : (uint16_t)(m_spritepalbase | (pix & 0x3ff));
						packed[idx] = transparent ? 0x00000001u : ((pen < (uint16_t)n_pens) ? (uint32_t)colors[pen] : 0u);
						owners[idx] = transparent ? RD_OWNER_NONE : (uint16_t)(pix & 0x3ff) / 16;
					}
				}
				retrodepth_write_layer(3, "sprites", packed.data(), owners.data(), w, h);
			}

			retrodepth_commit();
		}
	}

	return 0;
}
