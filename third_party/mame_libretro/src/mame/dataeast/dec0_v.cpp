// license:BSD-3-Clause
// copyright-holders:Bryan McPhail
/***************************************************************************

  Dec0 Video emulation - Bryan McPhail, mish@tendril.co.uk

*********************************************************************/

#include "emu.h"
#include "dec0.h"
#include "mame_retrodepth_hook.h"

// --- RetroDepth layer export ---
// Shared by every screen_update_* variant in this file (hbarrel/bandit/
// baddudes/robocop/birdtry/slyspy/automat/secretab): rather than replicate
// each function's own dynamic bg/fg tilegen swap (m_pri-dependent in several
// of them), export by fixed device slot -- tilegen[2] is the opaque backdrop
// wherever present, tilegen[1] the mid layer, tilegen[0] the near
// foreground/text layer (always drawn last, after sprites, in every variant
// above), sprites in between. This is an approximation of the true per-frame
// priority order for the handful of titles that swap bg/fg via m_pri, but is
// a good default and avoids duplicating each function's own logic here.
template <typename TilegenArray>
static void dec0_retrodepth_export_impl(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect,
                                         TilegenArray &tilegen, deco_mxc06_device &spritegen,
                                         u16 *buffered_spriteram, palette_device &palette, bool bootleg_sprites)
{
	if (!retrodepth_active())
		return;

	const rectangle &vis = screen.visible_area();
	const int bw = bitmap.width(), bh = bitmap.height();

	static bitmap_ind16 s_bg, s_mid, s_fg, s_sprites;
	if (s_bg.width() != bw || s_bg.height() != bh) {
		s_bg.allocate(bw, bh);
		s_mid.allocate(bw, bh);
		s_fg.allocate(bw, bh);
		s_sprites.allocate(bw, bh);
	}

	if (tilegen[2]) {
		s_bg.fill(0xffff, cliprect);
		tilegen[2]->draw(screen, s_bg, cliprect, TILEMAP_DRAW_OPAQUE, 0);
	}
	if (tilegen[1]) {
		s_mid.fill(0xffff, cliprect);
		tilegen[1]->draw(screen, s_mid, cliprect, 0, 0);
	}
	if (tilegen[0]) {
		s_fg.fill(0xffff, cliprect);
		tilegen[0]->draw(screen, s_fg, cliprect, 0, 0);
	}

	s_sprites.fill(0xffff, cliprect);
	if (bootleg_sprites)
		spritegen.draw_sprites_bootleg(screen, s_sprites, cliprect, buffered_spriteram, 0x800 / 2);
	else
		spritegen.draw_sprites(screen, s_sprites, cliprect, buffered_spriteram, 0x800 / 2);

	if (cliprect.max_y < vis.max_y)
		return;

	const rgb_t *colors = palette.palette()->entry_list_adjusted();
	const int n_pens = (int)palette.palette()->num_colors();

	static uint32_t pal_data[256 * 16];
	memset(pal_data, 0, sizeof(pal_data));
	int copy_count = (n_pens < 256 * 16) ? n_pens : (256 * 16);
	for (int i = 0; i < copy_count; ++i)
		pal_data[i] = (uint32_t)colors[i];
	retrodepth_write_palette_data(pal_data, 256 * 16);

	const int w = vis.width(), h = vis.height();
	std::vector<uint32_t> packed(w * h);
	std::vector<uint16_t> owners(w * h);

	auto write_layer = [&](uint32_t z_order, const char *name, bitmap_ind16 &bmp, bool has_owners) {
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
		retrodepth_write_layer(z_order, name, packed.data(), has_owners ? owners.data() : nullptr, w, h);
	};

	if (tilegen[2]) write_layer(0, "background", s_bg, false);
	write_layer(1, "midground", s_mid, true);
	write_layer(2, "sprites", s_sprites, true);
	write_layer(3, "foreground", s_fg, true);

	retrodepth_commit();
}

/******************************************************************************/

void dec0_8751_state::hbarrel_colpri_cb(u32 &colour, u32 &pri_mask)
{
	pri_mask = GFX_PMASK_4; // above background, foreground
	if (BIT(colour, 3))
	{
		pri_mask |= GFX_PMASK_2; // behind foreground
	}
}

/* HB always keeps pf2 on top of pf3, no need explicitly support priority register */
u32 dec0_8751_state::screen_update_hbarrel(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	screen.priority().fill(0,cliprect);
	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	m_tilegen[2]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 1);
	m_tilegen[1]->draw(screen, bitmap, cliprect, 0, 2);
	m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 4);
	m_spritegen->draw_sprites(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, false);
	return 0;
}

void dec0_8751_state::bandit_colpri_cb(u32 &colour, u32 &pri_mask)
{
	pri_mask = 0; // above all
	if (m_pri == 0)
	{
		pri_mask |= GFX_PMASK_4; // behind foreground
	}

	// ending and some gameplay portions (wood roads, with trees covering sprites) uses pri == 7
	// TODO: no way to make the truck go below the (sprite) car but above the girl in background
	if (m_pri == 7)
	{
		pri_mask |= GFX_PMASK_4 | GFX_PMASK_2;
	}
}

u32 dec0_8751_state::screen_update_bandit(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	screen.priority().fill(0,cliprect);
	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	if (m_pri == 7)
	{
		m_tilegen[1]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 1);
		m_tilegen[2]->draw(screen, bitmap, cliprect, 0, 2);
		m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 4);
	}
	else
	{
		m_tilegen[2]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 1);
		m_tilegen[1]->draw(screen, bitmap, cliprect, 0, 2);
		m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 4);
	}

	m_spritegen->draw_sprites(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, false);
	return 0;
}

/******************************************************************************/

void dec0_state::baddudes_tile_cb(tile_data &tileinfo, u32 &tile, u32 &colour, u32 &flags)
{
	tileinfo.group = BIT(colour, 3);
}

u32 drgninjab_state::screen_update_baddudes(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	/* WARNING: inverted wrt Midnight Resistance */
	const u8 fg = BIT(m_pri, 0) ? 1 : 2;
	const u8 bg = BIT(m_pri, 0) ? 2 : 1;
	m_tilegen[bg]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 0);
	m_tilegen[fg]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0 | TILEMAP_DRAW_LAYER1, 0);

	if (BIT(m_pri, 1))
		m_tilegen[bg]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0, 0); // upper 8 pens of upper 8 priority marked tiles /* Foreground pens only */

	m_spritegen->draw_sprites(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2);

	if (BIT(m_pri, 2))
		m_tilegen[fg]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0, 0); // upper 8 pens of upper 8 priority marked tiles /* Foreground pens only */

	m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 0);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, false);
	return 0;
}


/******************************************************************************/

/* WARNING: inverted wrt Midnight Resistance */
/* Robocop uses it only for the title screen, so this might be just */
/* completely wrong. The top 8 bits of the register might mean */
/* something (they are 0x80 in midres, 0x00 here) */
void dec0_state::robocop_colpri_cb(u32 &colour, u32 &pri_mask)
{
	pri_mask = 0; // above background, foreground
	if (BIT(m_pri, 1))
	{
		const u32 trans = BIT(m_pri, 2) ? 0x08 : 0x00;
		if ((colour & 0x08) == trans)
			pri_mask |= GFX_PMASK_2; // behind foreground
	}
}

void dec0_state::midres_colpri_cb(u32 &colour, u32 &pri_mask)
{
	pri_mask = 0; // above background, foreground
	if (BIT(m_pri, 1))
	{
		const u32 trans = BIT(m_pri, 2) ? 0x00 : 0x08;
		if ((colour & 0x08) == trans)
			pri_mask |= GFX_PMASK_2; // behind foreground
	}
}

u32 dec0_state::screen_update_robocop(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	screen.priority().fill(0,cliprect);

	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	const u8 fg = BIT(m_pri, 0) ? 2 : 1;
	const u8 bg = BIT(m_pri, 0) ? 1 : 2;
	m_tilegen[bg]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 1);
	m_tilegen[fg]->draw(screen, bitmap, cliprect, 0, 2);
	m_spritegen->draw_sprites(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2);
	m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 0);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, false);
	return 0;
}

u32 automat_state::screen_update_automat(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	screen.priority().fill(0,cliprect);

	// layer enables seem different... where are they?

	// the bootleg doesn't write these registers, I think they're hardcoded?, so fake them for compatibility with our implementation..
	m_tilegen[0]->ctrlreg_w(0,0x0003, 0x00ff); // 8x8
	m_tilegen[0]->ctrlreg_w(1,0x0003, 0x00ff);
	m_tilegen[0]->ctrlreg_w(2,0x0000, 0x00ff);
	m_tilegen[0]->ctrlreg_w(3,0x0001, 0x00ff); // dimensions

	m_tilegen[1]->ctrlreg_w(0,0x0082, 0x00ff); // 16x16
	m_tilegen[1]->ctrlreg_w(1,0x0000, 0x00ff);
	m_tilegen[1]->ctrlreg_w(2,0x0000, 0x00ff);
	m_tilegen[1]->ctrlreg_w(3,0x0001, 0x00ff); // dimensions

	m_tilegen[2]->ctrlreg_w(0,0x0082, 0x00ff); // 16x16
	m_tilegen[2]->ctrlreg_w(1,0x0003, 0x00ff);
	m_tilegen[2]->ctrlreg_w(2,0x0000, 0x00ff);
	m_tilegen[2]->ctrlreg_w(3,0x0001, 0x00ff); // dimensions

	// scroll registers got written elsewhere, copy them across
	m_tilegen[0]->scrollreg_w(0,0x0000, 0xffff); // no scroll?
	m_tilegen[0]->scrollreg_w(1,0x0000, 0xffff); // no scroll?

	m_tilegen[1]->scrollreg_w(0,m_automat_scroll_regs[3] - 0x010a, 0xffff);
	m_tilegen[1]->scrollreg_w(1,m_automat_scroll_regs[2], 0xffff);

	m_tilegen[2]->scrollreg_w(0,m_automat_scroll_regs[1] - 0x0108, 0xffff);
	m_tilegen[2]->scrollreg_w(1,m_automat_scroll_regs[0], 0xffff);


	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	const u8 fg = BIT(m_pri, 0) ? 2 : 1;
	const u8 bg = BIT(m_pri, 0) ? 1 : 2;
	m_tilegen[bg]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 1);
	m_tilegen[fg]->draw(screen, bitmap, cliprect, 0, 2);
	m_spritegen->draw_sprites_bootleg(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2); // TODO : RAM size
	m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 0);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, true);
	return 0;
}


/******************************************************************************/

u32 dec0_8751_state::screen_update_birdtry(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	/* This game doesn't have the extra playfield chip on the game board, but
	the palette does show through. */
	bitmap.fill(m_palette->pen(768), cliprect);
	m_tilegen[1]->draw(screen, bitmap, cliprect, 0, 0);
	m_spritegen->draw_sprites(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2);
	m_tilegen[0]->draw(screen, bitmap, cliprect, 0, 0);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, false);
	return 0;
}


/******************************************************************************/

u32 slyspy_state::screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	m_tilegen[2]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 0);
	m_tilegen[1]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0 | TILEMAP_DRAW_LAYER1, 0);

	m_spritegen->draw_sprites(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2);

	/* Redraw top 8 pens of top 8 palettes over sprites */
	if (BIT(m_pri, 7))
		m_tilegen[1]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0, 0); // upper 8 pens of upper 8 priority marked tiles

	m_tilegen[0]->draw(screen, bitmap, cliprect, 0,0);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, false);
	return 0;
}

u32 automat_state::screen_update_secretab(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	// layer enables seem different... where are they?

	// the bootleg doesn't write these registers, I think they're hardcoded?, so fake them for compatibility with our implementation..
	m_tilegen[0]->ctrlreg_w(0,0x0003, 0x00ff); // 8x8
	m_tilegen[0]->ctrlreg_w(1,0x0003, 0x00ff);
	m_tilegen[0]->ctrlreg_w(2,0x0000, 0x00ff);
	m_tilegen[0]->ctrlreg_w(3,0x0001, 0x00ff); // dimensions

	m_tilegen[1]->ctrlreg_w(0,0x0082, 0x00ff); // 16x16
	m_tilegen[1]->ctrlreg_w(1,0x0000, 0x00ff);
	m_tilegen[1]->ctrlreg_w(2,0x0000, 0x00ff);
	m_tilegen[1]->ctrlreg_w(3,0x0001, 0x00ff); // dimensions

	m_tilegen[2]->ctrlreg_w(0,0x0082, 0x00ff); // 16x16
	m_tilegen[2]->ctrlreg_w(1,0x0003, 0x00ff);
	m_tilegen[2]->ctrlreg_w(2,0x0000, 0x00ff);
	m_tilegen[2]->ctrlreg_w(3,0x0001, 0x00ff); // dimensions

	// scroll registers got written elsewhere, copy them across
	m_tilegen[0]->scrollreg_w(0,0x0000, 0xffff); // no scroll?
	m_tilegen[0]->scrollreg_w(1,0x0000, 0xffff); // no scroll?

	m_tilegen[1]->scrollreg_w(0,m_automat_scroll_regs[3] - 0x010a, 0xffff);
	m_tilegen[1]->scrollreg_w(1,m_automat_scroll_regs[2], 0xffff);

	m_tilegen[2]->scrollreg_w(0,m_automat_scroll_regs[1] - 0x0108, 0xffff);
	m_tilegen[2]->scrollreg_w(1,m_automat_scroll_regs[0], 0xffff);

	const bool flip = m_tilegen[0]->get_flip_state();
	m_tilegen[0]->set_flip_screen(flip);
	m_tilegen[1]->set_flip_screen(flip);
	m_tilegen[2]->set_flip_screen(flip);
	m_spritegen->set_flip_screen(flip);

	m_tilegen[2]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_OPAQUE, 0);
	m_tilegen[1]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0 | TILEMAP_DRAW_LAYER1, 0);

	m_spritegen->draw_sprites_bootleg(screen, bitmap, cliprect, m_buffered_spriteram, 0x800/2); // TODO : RAM size

	/* Redraw top 8 pens of top 8 palettes over sprites */
	if (BIT(m_pri, 7))
		m_tilegen[1]->draw(screen, bitmap, cliprect, TILEMAP_DRAW_LAYER0, 0); // upper 8 pens of upper 8 priority marked tiles

	m_tilegen[0]->draw(screen, bitmap, cliprect, 0,0);
	dec0_retrodepth_export_impl(screen, bitmap, cliprect, m_tilegen, *m_spritegen, m_buffered_spriteram, *m_palette, true);
	return 0;
}


/******************************************************************************/

void dec0_state::priority_w(offs_t offset, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_pri);
}

VIDEO_START_MEMBER(dec0_state,dec0_nodma)
{
	save_item(NAME(m_pri));
	m_buffered_spriteram = m_spriteram->live();
	save_pointer(NAME(m_buffered_spriteram), 0x800/2);

	m_pri = 0;
}

VIDEO_START_MEMBER(dec0_state,dec0)
{
	save_item(NAME(m_pri));
	m_buffered_spriteram = m_spriteram->buffer();
	save_pointer(NAME(m_buffered_spriteram), 0x800/2);

	m_pri = 0;
}

VIDEO_START_MEMBER(drgninjab_state,baddudes)
{
	VIDEO_START_CALL_MEMBER(dec0);
	m_tilegen[1]->set_transmask(0, 0xffff, 0x0001);
	m_tilegen[1]->set_transmask(1, 0x00ff, 0xff01);
	m_tilegen[2]->set_transmask(0, 0xffff, 0x0001);
	m_tilegen[2]->set_transmask(1, 0x00ff, 0xff01);
}

VIDEO_START_MEMBER(dec0_state,slyspy)
{
	VIDEO_START_CALL_MEMBER(dec0_nodma);
	m_tilegen[1]->set_transmask(0, 0xffff, 0x0001);
	m_tilegen[1]->set_transmask(1, 0x00ff, 0xff01);
}
