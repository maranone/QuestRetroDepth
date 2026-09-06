// mame_retrodepth_hook.h — in-process layer export for MAME running inside
// questretrodepth.
//
// Identical copy of capcom/mame_retrodepth_hook.h (see that file for the
// full rationale) for the Namco System 2 family (bubbletr, gollygho,
// luckywld, sgunner, sgunner2, ...). Header-only + `inline` so every
// family's copy of this file can coexist in the same build without a
// duplicate-symbol error -- only one MAME driver is ever loaded/running at
// a time, so sharing the same global state across families is harmless.
//
// The read-side accessor functions (mame_layer_*) are declared extern "C" so
// they get stable, unmangled names -- the questretrodepth app links against
// mame_libretro.so and needs to find them by name. They must also stay
// exported through src/osd/libretro/libretro-internal/link.T's version
// script (extended with a `mame_*` global pattern alongside `retro_*`).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static constexpr uint32_t RD_MAX_LAYERS  = 8;
static constexpr uint32_t RD_THUMB_DIM   = 32;
static constexpr uint16_t RD_OWNER_NONE  = 0xFFFFu;

struct RDPaletteRoute {
    uint8_t route[256];      // palette_index -> group (0-3); 0xFF = grp0
    uint8_t thumb_requested; // 1 = editor open, render per-palette thumbnails
};

namespace mame_retrodepth_detail {

struct LayerBuf {
    std::string name;
    uint32_t z_order = 0;
    uint32_t width = 0, height = 0;
    bool has_owner = false;
    std::vector<uint32_t> pixels;
    std::vector<uint16_t> owners;
};

// Two full sets of layer buffers: the write set is what the emulator thread
// is currently filling in (write_layer calls), the read set is the last
// fully committed frame the render thread pulls from. commit() flips which
// index is which. No lock -- same convention already used by
// snes9x_layer_capture.cpp/etc in this project: a torn single frame is
// visually harmless for pixel buffers like these, and the emulator/render
// threads never touch the same std::vector at the same instant since the
// flip is a single index write.
inline std::vector<LayerBuf> g_sets[2];
inline int g_write_idx = 0;
inline uint32_t g_frame_id = 0;

// Synthesized per-pixel depth channel, double-buffered on the same commit
// flip as the layer sets above. Neo Geo has no hardware layers at all (see
// neogeo_v.cpp -- the whole screen is backdrop + sprites + fix), so instead
// of exporting layers it fabricates a z-buffer the way snes9x reports its
// real one, and questretrodepth's existing ZBuffer depth path slices the
// composite frame by it. Empty for every driver that exports layers instead.
struct ZBuf {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> z;
};
inline ZBuf g_zbufs[2];

inline RDPaletteRoute g_palette_route = {};
inline uint32_t g_palette_argb[256 * 16] = {};

} // namespace mame_retrodepth_detail

inline void retrodepth_init() {
    // No-op: buffers are lazily sized on first write_layer call.
}

inline bool retrodepth_active() {
    return true;
}

inline void retrodepth_write_layer(uint32_t z_order, const char* name,
                                    const uint32_t* argb_pixels,
                                    const uint16_t* owner_ids,
                                    uint32_t width, uint32_t height) {
    using namespace mame_retrodepth_detail;
    auto& set = g_sets[g_write_idx];
    LayerBuf* buf = nullptr;
    for (auto& l : set) {
        if (l.name == name) { buf = &l; break; }
    }
    if (!buf) {
        set.emplace_back();
        buf = &set.back();
        buf->name = name;
    }

    buf->z_order = z_order;
    buf->width = width;
    buf->height = height;
    size_t px_count = (size_t)width * height;

    buf->pixels.resize(px_count);
    if (argb_pixels)
        std::memcpy(buf->pixels.data(), argb_pixels, px_count * sizeof(uint32_t));

    buf->has_owner = owner_ids != nullptr;
    if (buf->has_owner) {
        buf->owners.resize(px_count);
        std::memcpy(buf->owners.data(), owner_ids, px_count * sizeof(uint16_t));
    } else {
        buf->owners.clear();
    }
}

inline void retrodepth_write_zbuffer(const uint8_t* z, uint32_t width, uint32_t height) {
    using namespace mame_retrodepth_detail;
    auto& buf = g_zbufs[g_write_idx];
    buf.width = width;
    buf.height = height;
    const size_t count = (size_t)width * height;
    buf.z.resize(count);
    if (z && count)
        std::memcpy(buf.z.data(), z, count);
}

inline void retrodepth_commit() {
    using namespace mame_retrodepth_detail;
    // Publish the buffers the emulator thread just filled to the read side.
    g_write_idx ^= 1;
    ++g_frame_id;
    // Clear the (now-write) set's stale contents so a layer that stops being
    // drawn this frame doesn't linger from two frames ago.
    g_sets[g_write_idx].clear();
    g_zbufs[g_write_idx].z.clear();
    g_zbufs[g_write_idx].width = g_zbufs[g_write_idx].height = 0;
}

inline void retrodepth_read_palette_route(RDPaletteRoute* out) {
    *out = mame_retrodepth_detail::g_palette_route;
}

inline void retrodepth_write_palette_data(const uint32_t* argb_data, uint32_t count) {
    if (count > 256 * 16) count = 256 * 16;
    std::memcpy(mame_retrodepth_detail::g_palette_argb, argb_data, count * sizeof(uint32_t));
}

inline void retrodepth_write_palette_thumbs(const uint32_t* /*pixels*/, uint32_t /*pal_idx*/) {
    // Not consumed yet -- questretrodepth has no per-palette routing UI for
    // MAME layers. Kept as a no-op stub so a NeoGeo driver patch (which
    // calls this when thumb_requested is set, which we never set) would
    // still link cleanly if/when it's ported.
}

// ---------------------------------------------------------------------------
// Read side for the renderer (declarations mirrored in questretrodepth's
// app/src/main/cpp/mame_layer_capture.h)
// ---------------------------------------------------------------------------

// These accessors are never called from inside mame_libretro.so itself --
// only questretrodepth's app code calls them, after dlopen/dlsym-ing (or
// directly linking) this .so. Being `inline` with no internal caller means
// the compiler is otherwise free to never emit them at all, so force both
// emission and default (non-hidden) visibility explicitly, on top of the
// `mame_*` pattern added to link.T's version script.
#define RD_EXPORT extern "C" __attribute__((visibility("default"), used)) inline

extern "C" {

RD_EXPORT int mame_layer_count() {
    return (int)mame_retrodepth_detail::g_sets[mame_retrodepth_detail::g_write_idx ^ 1].size();
}

RD_EXPORT const char* mame_layer_name(int index) {
    const auto& set = mame_retrodepth_detail::g_sets[mame_retrodepth_detail::g_write_idx ^ 1];
    if (index < 0 || index >= (int)set.size()) return "";
    return set[(size_t)index].name.c_str();
}

RD_EXPORT uint32_t mame_layer_z_order(int index) {
    const auto& set = mame_retrodepth_detail::g_sets[mame_retrodepth_detail::g_write_idx ^ 1];
    if (index < 0 || index >= (int)set.size()) return 0;
    return set[(size_t)index].z_order;
}

RD_EXPORT const uint32_t* mame_layer_pixels(int index, uint32_t* out_width, uint32_t* out_height) {
    const auto& set = mame_retrodepth_detail::g_sets[mame_retrodepth_detail::g_write_idx ^ 1];
    if (index < 0 || index >= (int)set.size()) {
        if (out_width) *out_width = 0;
        if (out_height) *out_height = 0;
        return nullptr;
    }
    const auto& l = set[(size_t)index];
    if (out_width) *out_width = l.width;
    if (out_height) *out_height = l.height;
    return l.pixels.empty() ? nullptr : l.pixels.data();
}

RD_EXPORT const uint16_t* mame_layer_owners(int index) {
    const auto& set = mame_retrodepth_detail::g_sets[mame_retrodepth_detail::g_write_idx ^ 1];
    if (index < 0 || index >= (int)set.size()) return nullptr;
    const auto& l = set[(size_t)index];
    return l.has_owner && !l.owners.empty() ? l.owners.data() : nullptr;
}

RD_EXPORT uint32_t mame_frame_id() {
    return mame_retrodepth_detail::g_frame_id;
}

// Synthesized per-pixel depth for drivers that fabricate one (Neo Geo).
// Returns nullptr and zeroed dimensions for every driver that exports named
// layers instead.
RD_EXPORT const uint8_t* mame_zbuffer(uint32_t* out_width, uint32_t* out_height) {
    const auto& buf = mame_retrodepth_detail::g_zbufs[mame_retrodepth_detail::g_write_idx ^ 1];
    if (out_width) *out_width = buf.width;
    if (out_height) *out_height = buf.height;
    return buf.z.empty() ? nullptr : buf.z.data();
}

} // extern "C"

#undef RD_EXPORT
