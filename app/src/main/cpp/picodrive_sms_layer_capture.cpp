/* Per-pixel visible-source capture for PicoDrive SMS / Mode 4 renderer.
 *
 * Source IDs:  0 = backdrop, 1 = BG tile, 2 = sprite.
 *
 * picodrive_sms_lc_capture_line() is called from mode4.c per scanline.
 * picodrive_sms_lc_get_visible_source() is called from picodrive_backend.cpp
 * after retro_run() completes, before the frame is consumed.
 *
 * HighCol pixel encoding (picodrive SMS Mode 4):
 *   byte == 0        : backdrop — no tile or sprite was drawn here
 *   byte & 0x10      : sprite pixel  (palette index in low nibble)
 *   byte != 0, no    : BG tile pixel
 */
#include "picodrive_sms_layer_capture.h"
#include <string.h>

// SMS screen is 256 × 192 (standard). GG is 160 × 144 (handled by the
// backend's dimension check — GG frames are ignored here).
static const int k_sms_w = 256;
static const int k_sms_h = 192;
static const int k_gg_x = 48;
static const int k_gg_y = 48;
static const int k_gg_w = 160;
static const int k_gg_h = 144;

static uint8_t s_buf[k_sms_w * k_sms_h];
static int     s_valid = 0; // set after at least one frame has been captured

extern "C" void picodrive_sms_lc_capture_line(int scanline,
                                               const unsigned char* high_col,
                                               int width)
{
    if (scanline < 0 || scanline >= k_sms_h) return;
    if (width > k_sms_w) width = k_sms_w;

    uint8_t* dst = s_buf + scanline * k_sms_w;
    for (int x = 0; x < width; ++x) {
        const unsigned char b = high_col[x];
        if (b == 0)
            dst[x] = 0; // backdrop
        else if (b & 0x10u)
            dst[x] = 2; // sprite
        else
            dst[x] = 1; // BG tile
    }
    s_valid = 1;
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
