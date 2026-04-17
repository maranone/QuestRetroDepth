#include "layer_processor.h"
#include <algorithm>
#include <cstring>
#include <cmath>

LayerProcessor::LayerProcessor(const GameConfig& config)
    : m_config(config) {}

// snes9x backend stores uint32 as 0xAARRGGBB logical = [B,G,R,A] bytes on little-endian.
// We output [R,G,B,A] so GL_RGBA textures show the right colours.
void LayerProcessor::to_rgba(uint32_t src_pixel, uint8_t* out) {
    out[0] = static_cast<uint8_t>((src_pixel >> 16) & 0xFF); // R
    out[1] = static_cast<uint8_t>((src_pixel >>  8) & 0xFF); // G
    out[2] = static_cast<uint8_t>( src_pixel        & 0xFF); // B
    out[3] = static_cast<uint8_t>((src_pixel >> 24) & 0xFF); // A
}

bool LayerProcessor::color_match(uint32_t src_pixel, const LayerConfig& lc) {
    const uint8_t sr = static_cast<uint8_t>((src_pixel >> 16) & 0xFF);
    const uint8_t sg = static_cast<uint8_t>((src_pixel >>  8) & 0xFF);
    const uint8_t sb = static_cast<uint8_t>( src_pixel        & 0xFF);
    const int t = lc.tolerance;
    return std::abs((int)sr - (int)lc.color[0]) <= t &&
           std::abs((int)sg - (int)lc.color[1]) <= t &&
           std::abs((int)sb - (int)lc.color[2]) <= t;
}

void LayerProcessor::finalize_frame(LayerFrame& frame) {
    bool saw_opaque = false;
    bool saw_transparent = false;
    const std::size_t npix = frame.rgba.size() / 4;
    for (std::size_t i = 0; i < npix; ++i) {
        const uint8_t a = frame.rgba[i * 4 + 3];
        saw_opaque = saw_opaque || (a >= 250);
        saw_transparent = saw_transparent || (a <= 5);
        if (saw_opaque && saw_transparent) break;
    }
    frame.has_pixels = saw_opaque;
    frame.wedge_eligible = saw_opaque && saw_transparent;
    frame.bbox_eligible = false;
    frame.object_boxes.clear();
}

std::vector<LayerFrame> LayerProcessor::process(const uint32_t* src, int w, int h,
                                                  const uint8_t* zbuf,
                                                  const qrd::FrameOutput* frame,
                                                  bool build_object_boxes) {
    std::vector<LayerFrame> result;
    process_into(result, src, w, h, zbuf, frame, build_object_boxes);
    return result;
}

void LayerProcessor::prepare_frame(LayerFrame& f, const LayerConfig& lc, int w, int h, bool clear_pixels) {
    f.id                = lc.id;
    f.depth_meters      = lc.depth_meters;
    f.quad_width_meters = lc.quad_width_meters;
    f.copies            = lc.copies;
    f.width             = w;
    f.height            = h;
    const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    if (clear_pixels) {
        f.rgba.assign(bytes, 0u);
    } else if (f.rgba.size() != bytes) {
        f.rgba.resize(bytes);
    }
    f.has_pixels = false;
    f.wedge_eligible = false;
    f.bbox_eligible = false;
    f.object_boxes.clear();
}

void LayerProcessor::process_into(std::vector<LayerFrame>& result,
                                  const uint32_t* src, int w, int h,
                                  const uint8_t* zbuf,
                                  const qrd::FrameOutput* frame,
                                  bool build_object_boxes) {
    result.resize(m_config.layers.size());

    // Collect indices of ZBuffer layers so we can fill them in a single pass.
    std::vector<int> zbuf_indices;

    for (int i = 0; i < (int)m_config.layers.size(); ++i) {
        const auto& lc = m_config.layers[i];
        auto& out = result[i];
        switch (lc.extraction_type) {
        case ExtractionType::FullFrame:
            fill_full_frame(out, lc, src, w, h);
            break;
        case ExtractionType::Region:
            fill_region(out, lc, src, w, h);
            break;
        case ExtractionType::ColorKey:
            fill_color_key(out, lc, src, w, h, false);
            break;
        case ExtractionType::ColorKeyInverted:
            fill_color_key(out, lc, src, w, h, true);
            break;
        case ExtractionType::ZBuffer: {
            // Allocate the frame now (transparent), fill pixels in the single pass below.
            prepare_frame(out, lc, w, h, true);
            zbuf_indices.push_back(i);
            break;
        }
        case ExtractionType::PerLayerCapture: {
            fill_per_layer_capture(out, lc, frame, w, h);
            break;
        }
        case ExtractionType::VisibleSourceFinal: {
            fill_visible_source_final(out, lc, src, frame, w, h);
            break;
        }
        case ExtractionType::VisibleSourceHybrid: {
            fill_visible_source_hybrid(out, lc, src, frame, w, h);
            break;
        }
        }
    }

    // Single pass over all pixels for all ZBuffer layers.
    if (!zbuf_indices.empty()) {
        if (zbuf)
            extract_all_zbuffer_layers(result, zbuf_indices, src, zbuf, w, h);
        else {
            // No z-buffer — fall back: every ZBuffer layer shows the full frame.
            for (int ri : zbuf_indices) {
                auto& f = result[ri];
                const auto& lc = m_config.layers[ri];
                fill_full_frame(f, lc, src, w, h);
            }
        }
    }

    for (auto& frame_out : result) {
        finalize_frame(frame_out);
        if (build_object_boxes) compute_object_boxes(frame_out);
    }
}

void LayerProcessor::fill_full_frame(LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h) {
    prepare_frame(f, lc, w, h, false);
    for (int i = 0; i < w * h; ++i) {
        to_rgba(src[i], &f.rgba[i * 4]);
        f.rgba[i * 4 + 3] = 255; // fully opaque
    }
}

LayerFrame LayerProcessor::extract_full_frame(const LayerConfig& lc, const uint32_t* src, int w, int h) {
    LayerFrame f;
    fill_full_frame(f, lc, src, w, h);
    finalize_frame(f);
    return f;
}

void LayerProcessor::fill_visible_source_final(
    LayerFrame& f,
    const LayerConfig& lc, const uint32_t* src, const qrd::FrameOutput* frame, int w, int h)
{
    prepare_frame(f, lc, w, h, true);

    const std::size_t npix = static_cast<std::size_t>(w) * h;
    if (!src || !frame || frame->visible_source_id.size() < npix) {
        return;
    }

    const uint8_t wanted = static_cast<uint8_t>(lc.layer_index);
    const bool suppress_black_backdrop = (lc.id == "backdrop" && wanted == 5);
    for (std::size_t i = 0; i < npix; ++i) {
        if (frame->visible_source_id[i] != wanted) continue;
        if (suppress_black_backdrop) {
            const uint32_t px = src[i];
            const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFF);
            const uint8_t g = static_cast<uint8_t>((px >>  8) & 0xFF);
            const uint8_t b = static_cast<uint8_t>( px        & 0xFF);
            if (r <= 8 && g <= 8 && b <= 8) continue;
        }
        to_rgba(src[i], &f.rgba[i * 4]);
        f.rgba[i * 4 + 3] = 255;
    }
}

LayerFrame LayerProcessor::extract_visible_source_final(
    const LayerConfig& lc, const uint32_t* src, const qrd::FrameOutput* frame, int w, int h)
{
    LayerFrame f;
    fill_visible_source_final(f, lc, src, frame, w, h);
    finalize_frame(f);
    return f;
}

void LayerProcessor::fill_visible_source_hybrid(
    LayerFrame& f,
    const LayerConfig& lc, const uint32_t* src, const qrd::FrameOutput* frame, int w, int h)
{
    prepare_frame(f, lc, w, h, true);

    const int li = lc.layer_index;
    const std::size_t npix = static_cast<std::size_t>(w) * h;

    if (frame && li >= 0 && li < (int)frame->layers.size() && !frame->layers[li].rgba.empty()) {
        const auto& src_rgba = frame->layers[li].rgba;
        const std::size_t copy_pix = std::min(npix, src_rgba.size());
        if (copy_pix > 0) {
            std::memcpy(f.rgba.data(), src_rgba.data(), copy_pix * sizeof(uint32_t));
        }
    }

    if (!src || !frame || frame->visible_source_id.size() < npix) {
        return;
    }

    const uint8_t wanted = static_cast<uint8_t>(li);
    for (std::size_t i = 0; i < npix; ++i) {
        if (frame->visible_source_id[i] != wanted) continue;
        to_rgba(src[i], &f.rgba[i * 4]);
        f.rgba[i * 4 + 3] = 255;
    }
}

LayerFrame LayerProcessor::extract_visible_source_hybrid(
    const LayerConfig& lc, const uint32_t* src, const qrd::FrameOutput* frame, int w, int h)
{
    LayerFrame f;
    fill_visible_source_hybrid(f, lc, src, frame, w, h);
    finalize_frame(f);
    return f;
}

void LayerProcessor::fill_region(LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h) {
    int rx = std::clamp(lc.rect[0], 0, w);
    int ry = std::clamp(lc.rect[1], 0, h);
    int rw = std::clamp(lc.rect[2], 0, w - rx);
    int rh = std::clamp(lc.rect[3], 0, h - ry);

    prepare_frame(f, lc, w, h, true);

    for (int y = ry; y < ry + rh; ++y) {
        for (int x = rx; x < rx + rw; ++x) {
            const int di = (y * w + x) * 4;
            to_rgba(src[y * w + x], &f.rgba[di]);
            f.rgba[di + 3] = 255;
        }
    }
}

LayerFrame LayerProcessor::extract_region(const LayerConfig& lc, const uint32_t* src, int w, int h) {
    LayerFrame f;
    fill_region(f, lc, src, w, h);
    finalize_frame(f);
    return f;
}

void LayerProcessor::fill_color_key(LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h, bool invert) {
    prepare_frame(f, lc, w, h, false);

    for (int i = 0; i < w * h; ++i) {
        const bool match  = color_match(src[i], lc);
        // ColorKey:         match → transparent, non-match → opaque
        // ColorKeyInverted: non-match → transparent, match → opaque
        const bool opaque = (match == invert);
        to_rgba(src[i], &f.rgba[i * 4]);
        f.rgba[i * 4 + 3] = opaque ? 255 : 0;
    }
}

LayerFrame LayerProcessor::extract_color_key(const LayerConfig& lc, const uint32_t* src, int w, int h, bool invert) {
    LayerFrame f;
    fill_color_key(f, lc, src, w, h, invert);
    finalize_frame(f);
    return f;
}

void LayerProcessor::fill_per_layer_capture(
    LayerFrame& f,
    const LayerConfig& lc, const qrd::FrameOutput* frame, int w, int h)
{
    prepare_frame(f, lc, w, h, true);

    const int li = lc.layer_index;
    if (!frame || li < 0 || li >= (int)frame->layers.size() ||
        frame->layers[li].rgba.empty()) {
        // No capture available — return transparent frame (nothing shown).
        return;
    }

    // The capture RGBA is already in [R,G,B,A] order (uint32 little-endian:
    // byte0=R, byte1=G, byte2=B, byte3=A) as written by snes_libretro_backend.
    // PerLayerCapture already has correct alpha (255=opaque, 0=transparent) for each layer.
    const auto& src_rgba = frame->layers[li].rgba;
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    const std::size_t copy_pix = std::min(npix, src_rgba.size());
    if (copy_pix > 0) {
        std::memcpy(f.rgba.data(), src_rgba.data(), copy_pix * sizeof(uint32_t));
    }
}

LayerFrame LayerProcessor::extract_per_layer_capture(
    const LayerConfig& lc, const qrd::FrameOutput* frame, int w, int h)
{
    LayerFrame f;
    fill_per_layer_capture(f, lc, frame, w, h);
    finalize_frame(f);
    return f;
}

void LayerProcessor::extract_all_zbuffer_layers(std::vector<LayerFrame>& frames,
                                                  const std::vector<int>& zbuf_indices,
                                                  const uint32_t* src,
                                                  const uint8_t* zbuf, int w, int h) {
    // Build a fast lookup: z_value → frame index in `frames` (-1 = no layer).
    // z values range 0–255 but snes9x only uses 0–63 in practice.
    int8_t z_to_frame[256];
    std::fill(z_to_frame, z_to_frame + 256, int8_t(-1));

    for (int ri : zbuf_indices) {
        const auto& lc = m_config.layers[ri];
        const int zlo = lc.z_min;
        const int zhi = std::min((int)lc.z_max, 255);
        for (int z = zlo; z <= zhi; ++z)
            z_to_frame[z] = static_cast<int8_t>(ri);
    }

    const int npix = w * h;
    for (int i = 0; i < npix; ++i) {
        const int8_t fi = z_to_frame[zbuf[i]];
        if (fi < 0) continue;
        LayerFrame& lf = frames[fi];
        uint8_t* dst = lf.rgba.data() + i * 4;
        to_rgba(src[i], dst);
        dst[3] = 255; // opaque
        lf.has_pixels = true;
    }
}

void LayerProcessor::compute_object_boxes(LayerFrame& frame) {
    constexpr int k_alpha_threshold = 5;
    constexpr std::size_t k_max_object_boxes = 64;

    if (!frame.wedge_eligible || frame.width <= 0 || frame.height <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const std::size_t npix = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (frame.rgba.size() < npix * 4u) return;

    std::vector<uint8_t> visited(npix, 0u);
    std::vector<int> queue;
    queue.reserve(256);

    auto enqueue_if_valid = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h) return;
        const std::size_t ni = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        if (visited[ni]) return;
        if (frame.rgba[ni * 4u + 3u] <= k_alpha_threshold) return;
        visited[ni] = 1u;
        queue.push_back(static_cast<int>(ni));
    };

    for (std::size_t seed = 0; seed < npix; ++seed) {
        if (visited[seed]) continue;
        if (frame.rgba[seed * 4u + 3u] <= k_alpha_threshold) continue;

        if (frame.object_boxes.size() >= k_max_object_boxes) {
            frame.object_boxes.clear();
            frame.bbox_eligible = false;
            return;
        }

        queue.clear();
        visited[seed] = 1u;
        queue.push_back(static_cast<int>(seed));

        int min_x = static_cast<int>(seed % w);
        int max_x = min_x;
        int min_y = static_cast<int>(seed / w);
        int max_y = min_y;

        for (std::size_t q = 0; q < queue.size(); ++q) {
            const int idx = queue[q];
            const int x = idx % w;
            const int y = idx / w;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);

            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    enqueue_if_valid(x + ox, y + oy);
                }
            }
        }

        frame.object_boxes.push_back({min_x, min_y, max_x, max_y});
    }

    frame.bbox_eligible = !frame.object_boxes.empty();
}
