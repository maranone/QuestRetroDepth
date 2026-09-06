// license:BSD-3-Clause
// copyright-holders:K.Wilkins
/* video hardware for Namco System II */

#include "emu.h"
#include "namcos2.h"
#include "mame_retrodepth_hook.h"

namespace {
// Shared by screen_update_luckywld/screen_update_sgunner below: re-runs the
// same tilemap/roz/road priority loop into an offscreen "background"
// bitmap (opaque -- always fully painted by the black_pen fill + mixing
// loop) and the sprite chip into a second, transparency-sentineled bitmap,
// then exports both as two RetroDepth layers. The real `bitmap` passed to
// screen_update is untouched by any of this -- normal video_refresh still
// gets the ordinary composited frame.
template <typename BgDrawFn, typename SprDrawFn>
void namcos2_export_layers(device_palette_interface& c116, screen_device& screen,
                            const rectangle& cliprect, const rectangle& vis,
                            BgDrawFn&& draw_bg, SprDrawFn&& draw_sprites) {
    const int bw = cliprect.width() >= vis.width() ? cliprect.width() : vis.width();
    const int bh = cliprect.height() >= vis.height() ? cliprect.height() : vis.height();
    constexpr uint16_t kTransSentinel = 0xffff;

    static bitmap_ind16 s_bg, s_sprites;
    if (s_bg.width() != bw || s_bg.height() != bh) {
        s_bg.allocate(bw, bh);
        s_sprites.allocate(bw, bh);
    }

    s_bg.fill(c116.black_pen(), cliprect);
    draw_bg(s_bg);

    s_sprites.fill(kTransSentinel, cliprect);
    draw_sprites(s_sprites);

    if (cliprect.max_y < vis.max_y) return; // not the last scanline strip yet

    const int w = vis.width(), h = vis.height();
    std::vector<uint32_t> packed(w * h);
    std::vector<uint16_t> owners(w * h);

    auto write_layer = [&](uint32_t z_order, const char* name,
                           bitmap_ind16& bmp, bool has_trans) {
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                uint16_t pen = bmp.pix(vis.min_y + row, vis.min_x + col);
                int idx = row * w + col;
                if (has_trans && pen == kTransSentinel) {
                    packed[idx] = 0x00000001u; // transparent sentinel
                    owners[idx] = RD_OWNER_NONE;
                } else {
                    packed[idx] = (uint32_t)c116.pen(pen);
                    owners[idx] = has_trans ? (uint16_t)(pen / 16) : RD_OWNER_NONE;
                }
            }
        }
        retrodepth_write_layer(z_order, name, packed.data(),
                               has_trans ? owners.data() : nullptr, w, h);
    };

    write_layer(0, "namco_bg",      s_bg,       false);
    write_layer(1, "namco_sprites", s_sprites,  true);

    retrodepth_commit();
}
} // namespace

void namcos2_base_state::TilemapCB(u16 code, int &tile, int &mask)
{
	mask = code;
	/* The order of bits needs to be corrected to index the right tile  14 15 11 12 13 */
	tile = bitswap<16>(code, 13, 12, 11, 15, 14, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
}

void finallap_state::TilemapCB_finalap2(u16 code, int &tile, int &mask)
{
	mask = code;
	tile = bitswap<15>(code, 13, 12, 11, 14, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
}

/**
 * m_gfx_ctrl selects a bank of 128 sprites within spriteram
 *
 * m_gfx_ctrl also supplies palette and priority information that is applied to the output of the
 *            Namco System 2 ROZ chip
 *
 * -xxx ---- ---- ---- roz priority
 * ---- xxxx ---- ---- roz palette
 * ---- ---- xxxx ---- always zero?
 * ---- ---- ---- xxxx sprite bank
 */
u16 metlhawk_state::gfx_ctrl_r()
{
	return m_gfx_ctrl;
}

void metlhawk_state::gfx_ctrl_w(offs_t offset, u16 data, u16 mem_mask)
{
	COMBINE_DATA(&m_gfx_ctrl);
}

/**************************************************************************/

u8 namcos2_base_state::c116_r(offs_t offset)
{
	if ((offset & 0x1800) == 0x1800)
	{
		/* palette register */
		offset &= 0x180f;

		/* registers 6,7: unmapped? */
		if (offset > 0x180b) return 0xff; // fix for finallap boot
	}
	return m_c116->read(offset);
}

/**************************************************************************/

void finallap_state::create_shadow_table()
{
	/* set table for sprite color == 0x0f */
	for (int i = 0; i < 16 * 256; i++)
	{
		m_c116->shadow_table()[i] = i + 0x2000;
	}
}

u32 metlhawk_state::sprite_pri_callback_ns2(u32 pri)
{
	return pri;
}

u32 namcos2_state::sprite_pri_callback_ns2(u32 pri)
{
	// only low 3 bits are used
	return pri & 7;
}

bool metlhawk_state::sprite_mix_callback_ns2(u16 &dest, u8 &destpri, u16 colbase, u16 src, u32 primask)
{
	if (destpri <= primask)
	{
		if ((src & 0xff) != 0xff)
		{
			if (src == 0xffe)
			{
				if (dest & 0x1000)
					dest |= 0x800;
				else
					dest = m_c116->black_pen();
			}
			else
			{
				dest = colbase + src;
			}
			destpri = primask;
			return true;
		}
	}
	return false;
}

bool sgunner_state::sprite_mix_callback_c355(u16 &dest, u8 &destpri, u16 colbase, u16 src, int srcpri, int pri)
{
	if (destpri <= srcpri)
	{
		if ((src & 0xff) != 0xff)
		{
			if (src == 0xffe)
			{
				dest |= 0x800;
			}
			else
			{
				dest = colbase + src;
			}
			destpri = srcpri;
			return true;
		}
	}
	return false;
}

/**************************************************************************/

void finallap_state::video_start()
{
	metlhawk_state::video_start();

	create_shadow_table();

	save_item(NAME(m_gfx_ctrl));
}

void namcos2_base_state::apply_clip(rectangle &clip, const rectangle &cliprect)
{
	clip.min_x = m_c116->get_reg(0) - 0x4a;
	clip.max_x = m_c116->get_reg(1) - 0x4a - 1;
	clip.min_y = m_c116->get_reg(2) - 0x21;
	clip.max_y = m_c116->get_reg(3) - 0x21 - 1;
	/* intersect with master clip rectangle */
	clip &= cliprect;
}

u32 namcos2_state::screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	rectangle clip;

	bitmap.fill(m_c116->black_pen(), cliprect);
	screen.priority().fill(0, cliprect);
	apply_clip(clip, cliprect);

	/* HACK: enable ROZ layer only if it has priority > 0 */
	// Phelios contradicts with this so disabled
	// (level 0 ROZ is actually used by stages 2, 3 and 4 at very least)
	//bool roz_enable = ((m_gfx_ctrl & 0x7000) ? true : false);

	for (int pri = 0; pri < 8; pri++)
	{
		m_c123tmap->draw(screen, bitmap, clip, pri, pri, 0);

		//if (roz_enable)
		{
			if (((m_gfx_ctrl & 0x7000) >> 12) == pri)
			{
				m_ns2roz->draw_roz(screen, bitmap, clip, m_gfx_ctrl, pri, 0);
			}
		}
	}
	m_ns2sprite->draw(screen, bitmap, clip, m_gfx_ctrl);
	return 0;
}

/**************************************************************************/

u32 finallap_state::screen_update_finallap(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	rectangle clip;

	bitmap.fill(m_c116->black_pen(), cliprect);
	screen.priority().fill(0, cliprect);
	apply_clip(clip, cliprect);

	for (int pri = 0; pri < 16; pri++)
	{
		if ((pri & 1) == 0)
		{
			m_c123tmap->draw(screen, bitmap, clip, pri / 2, pri, 0);
		}
		m_c45_road->draw(screen, bitmap, clip, pri, pri, 0);
	}
	m_ns2sprite->draw(screen, bitmap, clip, m_gfx_ctrl);
	return 0;
}

/**************************************************************************/

void sgunner_state::RozCB_luckywld(u16 code, int &tile, int &mask, int which)
{
	mask = code;

	u16 mangle = bitswap<11>(code & 0x31ff, 13, 12, 8, 7, 6, 5, 4, 3, 2, 1, 0);
	switch ((code >> 9) & 7)
	{
	case 0x00: mangle += 0x1c00; break; // Plus, NOT OR
	case 0x01: mangle |= 0x0800; break;
	case 0x02: mangle |= 0x0000; break;
	default: break;
	}

	tile = mangle;
}

u32 sgunner_state::screen_update_luckywld(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	rectangle clip;

	bitmap.fill(m_c116->black_pen(), cliprect);
	screen.priority().fill(0, cliprect);
	apply_clip(clip, cliprect);

	for (int pri = 0; pri < 16; pri++)
	{
		if ((pri & 1) == 0)
		{
			m_c123tmap->draw(screen, bitmap, clip, pri / 2, pri, 0);
		}
		m_c45_road->draw(screen, bitmap, clip, pri, pri, 0);

		if (m_c169roz)
			m_c169roz->draw(screen, bitmap, clip, pri, pri, 0);
	}
	m_c355spr->draw(screen, bitmap, clip);

	if (retrodepth_active()) {
		namcos2_export_layers(*m_c116, screen, cliprect, screen.visible_area(),
			[&](bitmap_ind16& bmp) {
				for (int pri = 0; pri < 16; pri++) {
					if ((pri & 1) == 0)
						m_c123tmap->draw(screen, bmp, clip, pri / 2, pri, 0);
					m_c45_road->draw(screen, bmp, clip, pri, pri, 0);
					if (m_c169roz)
						m_c169roz->draw(screen, bmp, clip, pri, pri, 0);
				}
			},
			[&](bitmap_ind16& bmp) { m_c355spr->draw(screen, bmp, clip); });
	}
	return 0;
}

/**************************************************************************/

u32 sgunner_state::screen_update_sgunner(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	rectangle clip;

	bitmap.fill(m_c116->black_pen(), cliprect);
	screen.priority().fill(0, cliprect);
	apply_clip(clip, cliprect);

	for (int pri = 0; pri < 8; pri++)
		m_c123tmap->draw(screen, bitmap, clip, pri, pri, 0);

	m_c355spr->draw(screen, bitmap, clip);

	if (retrodepth_active()) {
		namcos2_export_layers(*m_c116, screen, cliprect, screen.visible_area(),
			[&](bitmap_ind16& bmp) {
				for (int pri = 0; pri < 8; pri++)
					m_c123tmap->draw(screen, bmp, clip, pri, pri, 0);
			},
			[&](bitmap_ind16& bmp) { m_c355spr->draw(screen, bmp, clip); });
	}
	return 0;
}


/**************************************************************************/

void metlhawk_state::RozCB_metlhawk(u16 code, int &tile, int &mask, int which)
{
	mask = code;
	tile = bitswap<13>(code & 0x1fff, 11, 10, 9, 12, 8, 7, 6, 5, 4, 3, 2, 1, 0);
}

u32 metlhawk_state::screen_update_metlhawk(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	rectangle clip;

	bitmap.fill(m_c116->black_pen(), cliprect);
	screen.priority().fill(0, cliprect);
	apply_clip(clip, cliprect);

	for (int pri = 0; pri < 16; pri++)
	{
		if ((pri & 1) == 0)
		{
			m_c123tmap->draw(screen, bitmap, clip, pri / 2, pri, 0);
		}
		m_c169roz->draw(screen, bitmap, clip, pri, pri, 0);
	}
	m_ns2sprite->draw(screen, bitmap, clip, 0);
	return 0;
}
