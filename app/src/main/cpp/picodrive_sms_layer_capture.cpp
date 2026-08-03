/* Per-pixel visible-source and per-layer RGBA capture for PicoDrive SMS / Mode 4.
 *
 * Two-pass design:
 *   1. capture_bg_line  — snapshot HighCol BEFORE DrawSpritesM4 (BG+backdrop)
 *   2. capture_spr_line — compare post-sprites HighCol vs snapshot; changed
 *                         pixels are sprites.  Also fills visible_source.
 *   3. finalize_line    — convert raw palette-index snapshots to RGBA8888 using
 *                         the current HighPal (called after FinalizeLineSMS).
 *
 * Source IDs in visible_source:  0 = backdrop, 1 = BG tile, 2 = sprite.
 * Layer indices:                  0 = BG tilemap (+backdrop), 1 = sprites.
 */
#include "picodrive_sms_layer_capture.h"
#include <string.h>

static const int k_sms_w = 256;
static const int k_sms_h = 192;
static const int k_gg_w  = 160;
static const int k_gg_h  = 144;
static const int k_gg_x  = 0;
static const int k_gg_y  = 24;

/* Visible-source buffer: 0=backdrop, 1=BG tile, 2=sprite */
static uint8_t  s_buf[k_sms_w * k_sms_h];
static int      s_valid = 0;

/* BG snapshot (raw HighCol bytes) taken before DrawSpritesM4. */
static uint8_t  s_bg_raw[k_sms_w * k_sms_h];
/* Sprite raw: 0xFF = no sprite at this pixel, else HighCol byte. */
static uint8_t  s_spr_raw[k_sms_w * k_sms_h];
/* Per-layer RGBA8888: [0]=BG, [1]=sprites. */
static uint32_t s_layer_bufs[2][k_sms_w * k_sms_h];
static uint32_t s_lc_mask    = 0;
static int      s_layer_valid = 0;

/* Same convention as Genesis draw.c: returns ABGR in memory = RGBA as OpenGL reads it. */
static inline uint32_t sms_rgba_from_rgb565(uint16_t px)
{
    uint8_t r = (uint8_t)(((px >> 11) & 0x1fu) * 255u / 31u);
    uint8_t g = (uint8_t)(((px >>  5) & 0x3fu) * 255u / 63u);
    uint8_t b = (uint8_t)( (px        & 0x1fu) * 255u / 31u);
    return 0xff000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

extern "C" void picodrive_sms_lc_frame_begin(void)
{
    s_layer_valid = 0;
    memset(s_spr_raw, 0xFF, sizeof(s_spr_raw));
}

extern "C" void picodrive_sms_lc_set_capture_mask(uint32_t mask)
{
    s_lc_mask = mask & 0x3u;
}

extern "C" void picodrive_sms_lc_capture_bg_line(int scanline,
                                                  const unsigned char* high_col,
                                                  int width)
{
    if (scanline < 0 || scanline >= k_sms_h) return;
    if (width > k_sms_w) width = k_sms_w;
    memcpy(s_bg_raw + scanline * k_sms_w, high_col, (size_t)width);
}

extern "C" void picodrive_sms_lc_capture_spr_line(int scanline,
                                                   const unsigned char* high_col,
                                                   int width)
{
    if (scanline < 0 || scanline >= k_sms_h) return;
    if (width > k_sms_w) width = k_sms_w;

    uint8_t*       vis = s_buf     + scanline * k_sms_w;
    uint8_t*       spr = s_spr_raw + scanline * k_sms_w;
    const uint8_t* bg  = s_bg_raw  + scanline * k_sms_w;

    for (int x = 0; x < width; ++x) {
        const unsigned char b = high_col[x];

        /* visible_source: 0=backdrop, 1=BG tile, 2=sprite */
        if (b == 0)
            vis[x] = 0;
        else if (b & 0x10u)
            vis[x] = 2;
        else
            vis[x] = 1;

        /* Sprite isolation: pixel changed since BG snapshot → sprite drew here. */
        spr[x] = (b != bg[x]) ? b : (uint8_t)0xFF;
    }
    s_valid = 1;
}

extern "C" void picodrive_sms_lc_finalize_line(int scanline,
                                                const uint16_t* high_pal)
{
    if (scanline < 0 || scanline >= k_sms_h) return;
    if (!high_pal) return;

    const int      row     = scanline * k_sms_w;
    const uint8_t* bg_row  = s_bg_raw  + row;
    const uint8_t* spr_row = s_spr_raw + row;
    uint32_t*      bg_dst  = s_layer_bufs[0] + row;
    uint32_t*      spr_dst = s_layer_bufs[1] + row;

    const int do_bg  = (s_lc_mask & 1u) != 0;
    const int do_spr = (s_lc_mask & 2u) != 0;

    for (int x = 0; x < k_sms_w; ++x) {
        /* BG layer: fully opaque — backdrop color fills the gaps between tiles. */
        if (do_bg)
            bg_dst[x] = sms_rgba_from_rgb565(high_pal[bg_row[x] & 0x3fu]);

        /* Sprite layer: transparent where no sprite drew. */
        if (do_spr) {
            const uint8_t idx = spr_row[x];
            spr_dst[x] = (idx == 0xFFu)
                ? 0u
                : sms_rgba_from_rgb565(high_pal[idx & 0x3fu]);
        }
    }
    s_layer_valid = 1;
}

extern "C" const uint8_t* picodrive_sms_lc_get_visible_source(unsigned* out_w,
                                                               unsigned* out_h)
{
    if (!s_valid) return 0;
    *out_w = (unsigned)k_sms_w;
    *out_h = (unsigned)k_sms_h;
    return s_buf;
}

extern "C" int picodrive_sms_lc_copy_visible_source(uint8_t* dst,
                                                     unsigned dst_w,
                                                     unsigned dst_h)
{
    if (!s_valid || !dst) return 0;

    if (dst_w == (unsigned)k_sms_w && dst_h == (unsigned)k_sms_h) {
        memcpy(dst, s_buf, (size_t)k_sms_w * k_sms_h);
        return 1;
    }
    if (dst_w == (unsigned)k_gg_w && dst_h == (unsigned)k_gg_h) {
        for (int y = 0; y < k_gg_h; ++y) {
            memcpy(dst + (size_t)y * k_gg_w,
                   s_buf + (size_t)(y + k_gg_y) * k_sms_w + k_gg_x,
                   k_gg_w);
        }
        return 1;
    }
    return 0;
}

extern "C" int picodrive_sms_lc_copy_layer_rgba(int li, uint32_t* dst,
                                                 unsigned dst_w, unsigned dst_h)
{
    if (!s_layer_valid || !dst || li < 0 || li >= 2) return 0;
    if (!(s_lc_mask & (1u << li))) return 0;

    const uint32_t* src = s_layer_bufs[li];

    if (dst_w == (unsigned)k_sms_w && dst_h == (unsigned)k_sms_h) {
        memcpy(dst, src, (size_t)k_sms_w * k_sms_h * sizeof(uint32_t));
        return 1;
    }
    if (dst_w == (unsigned)k_gg_w && dst_h == (unsigned)k_gg_h) {
        for (int y = 0; y < k_gg_h; ++y) {
            memcpy(dst + (size_t)y * k_gg_w,
                   src + (size_t)(y + k_gg_y) * k_sms_w + k_gg_x,
                   k_gg_w * sizeof(uint32_t));
        }
        return 1;
    }
    return 0;
}
