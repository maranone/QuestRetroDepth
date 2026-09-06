#include "neogeo_palette_debug.h"

#include "mame_layer_capture.h"
#include "mame_backend.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party_stb/stb_image_write.h"

#ifdef __ANDROID__
#include <android/log.h>
#define QRD_PDBG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "QRD_PaletteDebug", __VA_ARGS__)
#else
#define QRD_PDBG_LOG(...) ((void)0)
#endif

namespace qrd {
namespace {

constexpr int kCaptureSeconds = 60;

// Mirror of the sprite palette->z formula in
// third_party/mame_libretro/src/mame/snk/neogeo_spr.h/.cpp
// (rd_compute_slot_depths(), RD_USE_FULL_PALETTE_RESOLUTION == true branch).
// Kept in sync by hand -- this is debug tooling, not the render path, so a
// mismatch here just means a stale canvas isolation, not a gameplay bug.
constexpr uint8_t kZBackdrop     = 2;
constexpr uint8_t kZFix          = 255;
constexpr uint8_t kZSpriteMin    = 3;
constexpr uint8_t kZSpriteMax    = 254;
constexpr int      kSpritePlanes = kZSpriteMax - kZSpriteMin + 1;

uint8_t z_for_palette(uint8_t pal) {
    return (uint8_t)(kZSpriteMax - ((int)pal * (kSpritePlanes - 1)) / 255);
}

bool  g_armed          = false;
bool  g_done_this_run   = false;
std::chrono::steady_clock::time_point g_arm_time;

int                    g_best_count = -1;
std::vector<uint8_t>   g_best_active_list;
uint32_t               g_best_w = 0, g_best_h = 0;
std::vector<uint32_t>  g_best_rgba;   // snapshot of the busiest frame's composite
std::vector<uint8_t>   g_best_zbuf;   // and its per-pixel synthesized z

bool make_dir(const std::string& path) {
    if (path.empty()) return false;
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    return errno == EEXIST;
}

void make_dir_recursive(const std::string& dir) {
    std::string cur;
    std::size_t pos = 0;
    if (!dir.empty() && dir[0] == '/') { cur = "/"; pos = 1; }
    while (pos <= dir.size()) {
        std::size_t next = dir.find('/', pos);
        if (next == std::string::npos) next = dir.size();
        cur += dir.substr(pos, next - pos);
        if (!cur.empty()) make_dir(cur);
        cur += "/";
        pos = next + 1;
    }
}

std::string basename_lower(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    for (char& c : base) c = (char)std::tolower((unsigned char)c);
    return base;
}

void reset_capture_state() {
    g_best_count = -1;
    g_best_active_list.clear();
    g_best_w = g_best_h = 0;
    g_best_rgba.clear();
    g_best_zbuf.clear();
}

// Isolates whatever the composite frame drew using z-value `z` onto an
// opaque black canvas the same size as the frame -- so stacking every
// active palette's isolation back together reconstructs the full screen.
// frame.rgba8888 is 0xAARRGGBB (same bit positions as MAME's source
// XRGB8888 word, see MameBackend::handle_video_frame()'s comment).
void write_canvas_isolation_jpg(const std::string& path, uint32_t w, uint32_t h,
                                const uint32_t* rgba, const uint8_t* zbuf, uint8_t z) {
    std::vector<uint8_t> rgb((size_t)w * h * 3, 0);
    for (size_t p = 0; p < (size_t)w * h; ++p) {
        if (zbuf[p] != z) continue;
        const uint32_t px = rgba[p];
        rgb[p * 3 + 0] = (uint8_t)((px >> 16) & 0xFF);
        rgb[p * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);
        rgb[p * 3 + 2] = (uint8_t)(px & 0xFF);
    }
    stbi_write_jpg(path.c_str(), (int)w, (int)h, 3, rgb.data(), 92);
}

void flush_capture() {
    const std::string root = mame_system_directory();
    if (root.empty() || g_best_active_list.empty() || g_best_rgba.empty() || g_best_zbuf.empty()) {
        QRD_PDBG_LOG("capture window elapsed but nothing to write "
                     "(root_empty=%d active=%zu rgba=%zu zbuf=%zu)",
                     root.empty(), g_best_active_list.size(), g_best_rgba.size(), g_best_zbuf.size());
        return;
    }
    const std::string out_dir = root + "/mame/qrd_temp";
    make_dir_recursive(out_dir);

    std::vector<uint8_t> sorted = g_best_active_list;
    std::sort(sorted.begin(), sorted.end());

    char summary_path[512];
    std::snprintf(summary_path, sizeof(summary_path), "%s/summary.txt", out_dir.c_str());
    if (FILE* f = std::fopen(summary_path, "w")) {
        std::fprintf(f, "Busiest frame: %d distinct active palette banks, canvas %ux%u\n",
                     (int)sorted.size(), g_best_w, g_best_h);
        std::fprintf(f, "Each canvas_XXX.jpg isolates only the pixels drawn with that palette\n");
        std::fprintf(f, "bank (rest black) -- stack them together to reconstruct the full frame.\n");
        std::fprintf(f, "Palette indices (decimal) -> z value used for isolation:\n");
        for (uint8_t p : sorted) {
            std::fprintf(f, "%d -> z=%d\n", (int)p, (int)z_for_palette(p));
        }
        std::fclose(f);
    }

    int written = 0;
    for (uint8_t pal : sorted) {
        const uint8_t z = z_for_palette(pal);
        char fname[512];
        std::snprintf(fname, sizeof(fname), "%s/canvas_%03d.jpg", out_dir.c_str(), (int)pal);
        write_canvas_isolation_jpg(fname, g_best_w, g_best_h,
                                   g_best_rgba.data(), g_best_zbuf.data(), z);
        ++written;
    }

    // Also isolate backdrop and fix, even though they're not in the
    // per-palette active list -- they're useful reference layers when
    // reconstructing the full screen from the pieces.
    {
        char fname[512];
        std::snprintf(fname, sizeof(fname), "%s/canvas_backdrop.jpg", out_dir.c_str());
        write_canvas_isolation_jpg(fname, g_best_w, g_best_h,
                                   g_best_rgba.data(), g_best_zbuf.data(), kZBackdrop);
        std::snprintf(fname, sizeof(fname), "%s/canvas_fix.jpg", out_dir.c_str());
        write_canvas_isolation_jpg(fname, g_best_w, g_best_h,
                                   g_best_rgba.data(), g_best_zbuf.data(), kZFix);
    }

    // Full composite, for a side-by-side reference of what the pieces should add up to.
    {
        std::vector<uint8_t> rgb((size_t)g_best_w * g_best_h * 3);
        for (size_t p = 0; p < (size_t)g_best_w * g_best_h; ++p) {
            const uint32_t px = g_best_rgba[p];
            rgb[p * 3 + 0] = (uint8_t)((px >> 16) & 0xFF);
            rgb[p * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);
            rgb[p * 3 + 2] = (uint8_t)(px & 0xFF);
        }
        char fname[512];
        std::snprintf(fname, sizeof(fname), "%s/canvas_full_composite.jpg", out_dir.c_str());
        stbi_write_jpg(fname, (int)g_best_w, (int)g_best_h, 3, rgb.data(), 92);
    }

    QRD_PDBG_LOG("wrote %d per-palette canvas isolations + backdrop/fix/full-composite + "
                 "summary.txt to %s", written, out_dir.c_str());
}

} // namespace

void neogeo_palette_debug_maybe_arm(const std::string& rom_path) {
    const std::string base = basename_lower(rom_path);
    if (base.rfind("mslug", 0) != 0) return; // not a Metal Slug set -- leave any prior run alone

    g_armed = true;
    g_done_this_run = false;
    g_arm_time = std::chrono::steady_clock::now();
    reset_capture_state();
    QRD_PDBG_LOG("armed: capturing busiest frame's palette canvases over the next %d seconds",
                kCaptureSeconds);
}

void neogeo_palette_debug_tick(const FrameOutput& frame) {
    if (!g_armed || g_done_this_run) return;

    const uint32_t count = mame_active_palette_count();
    if ((int)count > g_best_count &&
        !frame.rgba8888.empty() && !frame.zbuffer.empty() &&
        frame.zbuffer.size() == frame.rgba8888.size()) {
        const uint8_t* list = mame_active_palette_list();
        if (list) {
            g_best_count = (int)count;
            g_best_active_list.assign(list, list + count);
            g_best_w = frame.width;
            g_best_h = frame.height;
            g_best_rgba = frame.rgba8888;
            g_best_zbuf = frame.zbuffer;
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - g_arm_time;
    if (elapsed >= std::chrono::seconds(kCaptureSeconds)) {
        flush_capture();
        g_armed = false;
        g_done_this_run = true;
    }
}

} // namespace qrd
