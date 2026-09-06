// Generic, opt-in OCCUPXY capture for MAME's shared drawgfx path.
//
// This is intentionally independent from the driver-specific
// mame_retrodepth_hook.h copies. It records pixels that changed during each
// drawgfx/drawgfxzoom call, assigns them to a small adaptive source grid, and
// publishes six far-to-near buckets at the libretro frame boundary. Anything
// not observed is handled by questretrodepth's residual safety layer.
#pragma once

#include "../lib/util/bitmap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace mame_retrodepth_occupancy_detail {

constexpr int kBucketCount = 6;
constexpr int kMaxGridDim = 64;
constexpr int kPreferredCellPixels = 16;

struct FrameSet {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t grid_w = 0;
    uint32_t grid_h = 0;
    std::array<std::vector<uint32_t>, kBucketCount> buckets;
    std::vector<uint8_t> occupancy;
    uint32_t draw_count = 0;
    uint32_t pixel_count = 0;
    bool valid = false;

    void reset(uint32_t w, uint32_t h) {
        width = w;
        height = h;
        const uint32_t cell_w = std::max<uint32_t>(
            kPreferredCellPixels, (w + kMaxGridDim - 1) / kMaxGridDim);
        const uint32_t cell_h = std::max<uint32_t>(
            kPreferredCellPixels, (h + kMaxGridDim - 1) / kMaxGridDim);
        grid_w = std::max<uint32_t>(1, (w + cell_w - 1) / cell_w);
        grid_h = std::max<uint32_t>(1, (h + cell_h - 1) / cell_h);
        occupancy.assign((size_t)grid_w * grid_h, 0u);
        for (auto& bucket : buckets)
            bucket.assign((size_t)w * h, 0u);
        draw_count = 0;
        pixel_count = 0;
        valid = false;
    }
};

inline FrameSet g_sets[2];
inline int g_write_idx = 0;
inline std::atomic<bool> g_enabled{false};
inline bool g_have_frame = false;

inline bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

inline void begin_frame(uint32_t width, uint32_t height) {
    if (!enabled()) {
        g_have_frame = false;
        return;
    }
    g_sets[g_write_idx].reset(width, height);
    g_have_frame = width != 0 && height != 0;
}

inline void commit_frame() {
    if (!enabled() || !g_have_frame) return;
    g_sets[g_write_idx].valid = true;
    g_write_idx ^= 1;
    g_sets[g_write_idx].valid = false;
    g_have_frame = false;
}

struct ChangedPixel {
    int x;
    int y;
    uint32_t argb;
};

inline void record_draw(const std::vector<ChangedPixel>& changed) {
    if (!enabled() || !g_have_frame || changed.empty()) return;
    FrameSet& set = g_sets[g_write_idx];
    const uint32_t cell_w = std::max<uint32_t>(1, (set.width + set.grid_w - 1) / set.grid_w);
    const uint32_t cell_h = std::max<uint32_t>(1, (set.height + set.grid_h - 1) / set.grid_h);
    std::vector<std::pair<size_t, int>> cell_buckets;
    cell_buckets.reserve(changed.size());
    for (const ChangedPixel& pixel : changed) {
        if (pixel.x < 0 || pixel.y < 0 || (uint32_t)pixel.x >= set.width ||
            (uint32_t)pixel.y >= set.height) continue;
        const uint32_t gx = std::min<uint32_t>(set.grid_w - 1, (uint32_t)pixel.x / cell_w);
        const uint32_t gy = std::min<uint32_t>(set.grid_h - 1, (uint32_t)pixel.y / cell_h);
        const size_t cell = (size_t)gy * set.grid_w + gx;
        int bucket = -1;
        for (const auto& entry : cell_buckets) {
            if (entry.first == cell) { bucket = entry.second; break; }
        }
        if (bucket < 0) {
            bucket = std::min<int>(kBucketCount - 1, set.occupancy[cell]);
            cell_buckets.emplace_back(cell, bucket);
        }
        set.buckets[bucket][(size_t)pixel.y * set.width + (size_t)pixel.x] =
            pixel.argb | 0xff000000u;
        ++set.pixel_count;
    }
    for (const auto& entry : cell_buckets) {
        if (set.occupancy[entry.first] != 0xffu) ++set.occupancy[entry.first];
    }
}

template <typename BitmapType>
inline uint32_t pixel_to_argb(const BitmapType& bitmap, typename BitmapType::pixel_t pixel) {
    if constexpr (sizeof(typename BitmapType::pixel_t) >= 4) {
        return static_cast<uint32_t>(pixel);
    } else {
        // Indexed MAME bitmaps carry their palette on the bitmap. This keeps
        // the generic path useful for drivers that render through bitmap_ind16
        // while leaving the original bitmap untouched.
        const palette_t* palette = bitmap.palette();
        if (palette)
            return static_cast<uint32_t>(palette->entry_adjusted_color(static_cast<uint32_t>(pixel)));
        return 0xff000000u | static_cast<uint32_t>(pixel);
    }
}

template <typename BitmapType>
class draw_scope {
public:
    draw_scope(BitmapType& bitmap, const rectangle& requested,
               const rectangle& draw_region)
        : m_bitmap(bitmap) {
        if (!enabled() || !bitmap.valid()) return;
        m_rect = requested;
        m_rect &= draw_region;
        m_rect &= bitmap.cliprect();
        m_rect &= rectangle(0, (int)g_sets[g_write_idx].width - 1,
                            0, (int)g_sets[g_write_idx].height - 1);
        if (m_rect.empty()) return;
        m_before.resize((size_t)m_rect.width() * m_rect.height());
        for (int y = m_rect.top(); y <= m_rect.bottom(); ++y)
            for (int x = m_rect.left(); x <= m_rect.right(); ++x)
                m_before[(size_t)(y - m_rect.top()) * m_rect.width() +
                         (size_t)(x - m_rect.left())] = bitmap.pix(y, x);
        ++g_sets[g_write_idx].draw_count;
        m_active = true;
    }

    ~draw_scope() {
        if (!m_active) return;
        std::vector<ChangedPixel> changed;
        changed.reserve(m_before.size() / 4u + 1u);
        for (int y = m_rect.top(); y <= m_rect.bottom(); ++y) {
            for (int x = m_rect.left(); x <= m_rect.right(); ++x) {
                const size_t index = (size_t)(y - m_rect.top()) * m_rect.width() +
                                     (size_t)(x - m_rect.left());
                const auto pixel = m_bitmap.pix(y, x);
                if (pixel == m_before[index]) continue;
                changed.push_back({x, y, pixel_to_argb(m_bitmap, pixel)});
            }
        }
        record_draw(changed);
    }

private:
    BitmapType& m_bitmap;
    rectangle m_rect;
    std::vector<typename BitmapType::pixel_t> m_before;
    bool m_active = false;
};

inline const FrameSet& read_set() {
    return g_sets[g_write_idx ^ 1];
}

} // namespace mame_retrodepth_occupancy_detail

// Public functions are emitted from every translation unit that includes this
// header, matching the existing header-only MAME layer hook convention.
#define RD_OCCUPANCY_EXPORT extern "C" __attribute__((visibility("default"), used)) inline

RD_OCCUPANCY_EXPORT void mame_occupancy_set_enabled(int enabled) {
    using namespace mame_retrodepth_occupancy_detail;
    g_enabled.store(enabled != 0, std::memory_order_release);
    g_have_frame = false;
    if (!mame_retrodepth_occupancy_detail::enabled()) {
        g_sets[0] = FrameSet{};
        g_sets[1] = FrameSet{};
    }
}

RD_OCCUPANCY_EXPORT int mame_occupancy_enabled() {
    return mame_retrodepth_occupancy_detail::g_enabled.load(std::memory_order_acquire) ? 1 : 0;
}

RD_OCCUPANCY_EXPORT int mame_occupancy_available() { return 1; }

RD_OCCUPANCY_EXPORT int mame_occupancy_valid() {
    return mame_retrodepth_occupancy_detail::read_set().valid ? 1 : 0;
}

RD_OCCUPANCY_EXPORT int mame_occupancy_bucket_count() {
    return mame_retrodepth_occupancy_detail::kBucketCount;
}

RD_OCCUPANCY_EXPORT const uint32_t* mame_occupancy_bucket_pixels(
    int bucket, uint32_t* out_width, uint32_t* out_height) {
    using namespace mame_retrodepth_occupancy_detail;
    const FrameSet& set = read_set();
    if (out_width) *out_width = set.width;
    if (out_height) *out_height = set.height;
    if (bucket < 0 || bucket >= kBucketCount || !set.valid) return nullptr;
    const auto& pixels = set.buckets[(size_t)bucket];
    return pixels.empty() ? nullptr : pixels.data();
}

RD_OCCUPANCY_EXPORT uint32_t mame_occupancy_draw_count() {
    return mame_retrodepth_occupancy_detail::read_set().draw_count;
}

RD_OCCUPANCY_EXPORT uint32_t mame_occupancy_pixel_count() {
    return mame_retrodepth_occupancy_detail::read_set().pixel_count;
}

#undef RD_OCCUPANCY_EXPORT
