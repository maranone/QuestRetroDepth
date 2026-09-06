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

    // Single global content-bounds rect (min/max over all non-transparent pixels), plus a
    // per-row and per-column edge profile for silhouette-following side faces — all in one
    // pass, no clustering/labeling involved. Only worth computing when the layer actually has
    // empty margins to trim (wedge_eligible); a solid full-frame layer's content already fills
    // the whole rect and its edges are opaque all the way across.
    frame.content_bounds_valid = false;
    frame.edge_lr.clear();
    frame.edge_tb.clear();
    if (frame.wedge_eligible && frame.width > 0 && frame.height > 0) {
        int min_x = frame.width, min_y = frame.height, max_x = -1, max_y = -1;
        std::vector<int> row_min_x(frame.height, -1), row_max_x(frame.height, -1);
        std::vector<int> col_min_y(frame.width, -1), col_max_y(frame.width, -1);
        for (int y = 0; y < frame.height; ++y) {
            const std::size_t row = (std::size_t)y * frame.width;
            for (int x = 0; x < frame.width; ++x) {
                if (frame.rgba[(row + x) * 4 + 3] <= 10) continue;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (row_min_x[y] < 0 || x < row_min_x[y]) row_min_x[y] = x;
                if (x > row_max_x[y]) row_max_x[y] = x;
                if (col_min_y[x] < 0 || y < col_min_y[x]) col_min_y[x] = y;
                if (y > col_max_y[x]) col_max_y[x] = y;
            }
        }
        if (max_x >= min_x && max_y >= min_y) {
            frame.content_bounds = {min_x, min_y, max_x, max_y};
            frame.content_bounds_valid = true;

            const float inv_w = 1.0f / (float)frame.width;
            const float inv_h = 1.0f / (float)frame.height;

            frame.edge_lr.assign((std::size_t)frame.height * 2, -1.0f);
            for (int y = 0; y < frame.height; ++y) {
                if (row_min_x[y] < 0) continue;
                frame.edge_lr[(std::size_t)y * 2 + 0] = (float)row_min_x[y] * inv_w;
                frame.edge_lr[(std::size_t)y * 2 + 1] = (float)(row_max_x[y] + 1) * inv_w;
            }
            // Fill gap rows (fully transparent at that height) from the nearest valid row.
            for (int y = 0; y < frame.height; ++y) {
                if (frame.edge_lr[(std::size_t)y * 2] >= 0.0f) continue;
                int left = y - 1, right = y + 1;
                while (left >= 0 && frame.edge_lr[(std::size_t)left * 2] < 0.0f) --left;
                while (right < frame.height && frame.edge_lr[(std::size_t)right * 2] < 0.0f) ++right;
                const int src = (left >= 0) ? left : (right < frame.height ? right : -1);
                if (src >= 0) {
                    frame.edge_lr[(std::size_t)y * 2 + 0] = frame.edge_lr[(std::size_t)src * 2 + 0];
                    frame.edge_lr[(std::size_t)y * 2 + 1] = frame.edge_lr[(std::size_t)src * 2 + 1];
                } else {
                    frame.edge_lr[(std::size_t)y * 2 + 0] = 0.5f;
                    frame.edge_lr[(std::size_t)y * 2 + 1] = 0.5f;
                }
            }

            frame.edge_tb.assign((std::size_t)frame.width * 2, -1.0f);
            for (int x = 0; x < frame.width; ++x) {
                if (col_min_y[x] < 0) continue;
                frame.edge_tb[(std::size_t)x * 2 + 0] = (float)col_min_y[x] * inv_h;
                frame.edge_tb[(std::size_t)x * 2 + 1] = (float)(col_max_y[x] + 1) * inv_h;
            }
            for (int x = 0; x < frame.width; ++x) {
                if (frame.edge_tb[(std::size_t)x * 2] >= 0.0f) continue;
                int left = x - 1, right = x + 1;
                while (left >= 0 && frame.edge_tb[(std::size_t)left * 2] < 0.0f) --left;
                while (right < frame.width && frame.edge_tb[(std::size_t)right * 2] < 0.0f) ++right;
                const int src = (left >= 0) ? left : (right < frame.width ? right : -1);
                if (src >= 0) {
                    frame.edge_tb[(std::size_t)x * 2 + 0] = frame.edge_tb[(std::size_t)src * 2 + 0];
                    frame.edge_tb[(std::size_t)x * 2 + 1] = frame.edge_tb[(std::size_t)src * 2 + 1];
                } else {
                    frame.edge_tb[(std::size_t)x * 2 + 0] = 0.5f;
                    frame.edge_tb[(std::size_t)x * 2 + 1] = 0.5f;
                }
            }
        }
    }
}

std::vector<LayerFrame> LayerProcessor::process(const uint32_t* src, int w, int h,
                                                  const uint8_t* zbuf,
                                                  const qrd::FrameOutput* frame,
                                                  bool build_object_boxes,
                                                  bool build_extrude_runs,
                                                  bool use_zbuffer_depths) {
    std::vector<LayerFrame> result;
    process_into(result, src, w, h, zbuf, frame, build_object_boxes, build_extrude_runs, use_zbuffer_depths);
    return result;
}

void LayerProcessor::prepare_frame(LayerFrame& f, const LayerConfig& lc, int w, int h, bool clear_pixels) {
    f.id                = lc.id;
    f.depth_meters      = lc.depth_meters;
    f.quad_width_meters = lc.quad_width_meters;
    f.copies            = lc.copies;
    f.geometry_mode         = lc.geometry_mode;
    f.box_thickness_meters  = lc.box_thickness_meters;
    f.split_pixels          = lc.split_pixels;
    f.repeat_count          = lc.repeat_count;
    f.scatter_range         = lc.scatter_range;
    f.y_depth_range         = lc.y_depth_range;
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
    f.zbuffer_depth_valid = false;
    f.object_boxes.clear();
}

bool LayerProcessor::can_use_genesis_hybrid_fast_path(const qrd::FrameOutput* frame, int w, int h) const {
    if (!frame || m_config.game != "genesis") return false;
    const int layer_count = (int)m_config.layers.size();
    if (layer_count <= 0 || (int)frame->layers.size() < layer_count) return false;
    const std::size_t npix = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (frame->visible_source_id.size() < npix) return false;

    for (int i = 0; i < layer_count; ++i) {
        const auto& lc = m_config.layers[i];
        if (lc.extraction_type != ExtractionType::VisibleSourceHybrid || lc.layer_index != i) return false;
        if (frame->layers[i].rgba.size() < npix) return false;
    }
    return true;
}

void LayerProcessor::process_genesis_hybrid_fast(std::vector<LayerFrame>& result,
                                                 const uint32_t* src,
                                                 const qrd::FrameOutput* frame,
                                                 int w, int h) {
    const int layer_count = (int)m_config.layers.size();
    const std::size_t npix = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    result.resize(layer_count);

    for (int i = 0; i < layer_count; ++i) {
        const auto& lc = m_config.layers[i];
        auto& out = result[i];
        prepare_frame(out, lc, w, h, true);
        const auto& src_rgba = frame->layers[i].rgba;
        if (!src_rgba.empty()) {
            const std::size_t copy_pix = std::min(npix, src_rgba.size());
            std::memcpy(out.rgba.data(), src_rgba.data(),
                        copy_pix * sizeof(uint32_t));
        }
    }

    if (!src) return;
    for (std::size_t i = 0; i < npix; ++i) {
        const uint8_t src_id = frame->visible_source_id[i];
        if (src_id >= (uint8_t)layer_count) continue;
        LayerFrame& out = result[src_id];
        to_rgba(src[i], out.rgba.data() + i * 4u);
        out.rgba[i * 4u + 3u] = 255;
    }
}

void LayerProcessor::process_into(std::vector<LayerFrame>& result,
                                  const uint32_t* src, int w, int h,
                                  const uint8_t* zbuf,
                                  const qrd::FrameOutput* frame,
                                  bool build_object_boxes,
                                  bool build_extrude_runs,
                                  bool use_zbuffer_depths) {
    if (can_use_genesis_hybrid_fast_path(frame, w, h)) {
        process_genesis_hybrid_fast(result, src, frame, w, h);
        for (auto& frame_out : result) {
            finalize_frame(frame_out);
            if (build_object_boxes)    compute_object_boxes(frame_out);
            if (build_extrude_runs)    compute_extrude_runs(frame_out);
        }
        if (use_zbuffer_depths) assign_zbuffer_object_depths(result, zbuf, w, h);
        return;
    }

    result.resize(m_config.layers.size());

    // Collect indices of ZBuffer layers so we can fill them in a single pass.
    std::vector<int> zbuf_indices;

    for (int i = 0; i < (int)m_config.layers.size(); ++i) {
        const auto& lc = m_config.layers[i];
        auto& out = result[i];
        switch (lc.extraction_type) {
        case ExtractionType::FullFrame:
            fill_full_frame(out, lc, src, w, h,
                frame ? frame->depth_map.data() : nullptr,
                frame ? frame->depth_map.size() : 0);
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
        case ExtractionType::MameOccupancyResidual: {
            fill_mame_occupancy_residual(out, lc, src, frame, w, h);
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
                fill_full_frame(f, lc, src, w, h,
                    frame ? frame->depth_map.data() : nullptr,
                    frame ? frame->depth_map.size() : 0);
            }
        }
    }

    // OCCUPXY is intentionally conservative. A bucket is retained only when
    // its nearest captured colour agrees with the final framebuffer. Any
    // disagreement clears all generated buckets at that pixel and leaves the
    // final colour in the residual layer, so an incomplete instrumented path
    // can never punch a hole in the displayed image.
    if (m_config.game == "mame_occupxy" && frame &&
        frame->mame_occupancy_available && frame->mame_occupancy_valid && src) {
        int residual_idx = -1;
        for (int i = 0; i < (int)m_config.layers.size(); ++i) {
            if (m_config.layers[i].extraction_type == ExtractionType::MameOccupancyResidual) {
                residual_idx = i;
                break;
            }
        }
        const std::size_t npix = static_cast<std::size_t>(w) * h;
        for (std::size_t p = 0; p < npix; ++p) {
            int nearest = -1;
            for (int i = (int)result.size() - 1; i >= 0; --i) {
                const auto& lc = m_config.layers[i];
                if (lc.extraction_type != ExtractionType::PerLayerCapture) continue;
                const auto& rgba = result[i].rgba;
                if (rgba.size() >= (p + 1u) * 4u && rgba[p * 4u + 3u] > 5u) {
                    nearest = i;
                    break;
                }
            }

            uint8_t final_rgba[4];
            to_rgba(src[p], final_rgba);
            bool proven = false;
            if (nearest >= 0) {
                const auto& rgba = result[nearest].rgba;
                proven = rgba[p * 4u + 0u] == final_rgba[0] &&
                         rgba[p * 4u + 1u] == final_rgba[1] &&
                         rgba[p * 4u + 2u] == final_rgba[2];
            }
            if (proven) {
                if (residual_idx >= 0 &&
                    result[residual_idx].rgba.size() >= (p + 1u) * 4u)
                    result[residual_idx].rgba[p * 4u + 3u] = 0u;
            } else {
                for (int i = 0; i < (int)result.size(); ++i) {
                    if (m_config.layers[i].extraction_type != ExtractionType::PerLayerCapture) continue;
                    if (result[i].rgba.size() < (p + 1u) * 4u) continue;
                    std::memset(result[i].rgba.data() + p * 4u, 0, 4u);
                }
                if (residual_idx >= 0 &&
                    result[residual_idx].rgba.size() >= (p + 1u) * 4u) {
                    uint8_t* dst = result[residual_idx].rgba.data() + p * 4u;
                    dst[0] = final_rgba[0]; dst[1] = final_rgba[1];
                    dst[2] = final_rgba[2]; dst[3] = 255u;
                }
            }
        }
    }

    for (auto& frame_out : result) {
        finalize_frame(frame_out);
        if (build_object_boxes)    compute_object_boxes(frame_out);
        if (build_extrude_runs)    compute_extrude_runs(frame_out);
    }
    if (use_zbuffer_depths) assign_zbuffer_object_depths(result, zbuf, w, h);
}

void LayerProcessor::assign_zbuffer_object_depths(std::vector<LayerFrame>& frames,
                                                   const uint8_t* zbuf, int w, int h) {
    if (!zbuf || w <= 0 || h <= 0) return;
    const std::size_t npix = (std::size_t)w * (std::size_t)h;
    uint8_t zmin = 255, zmax = 0; bool any = false;
    for (const auto& f : frames) {
        if (f.rgba.size() < npix * 4u) continue;
        for (std::size_t p = 0; p < npix; ++p) if (f.rgba[p * 4u + 3u] > 5) {
            zmin = std::min(zmin, zbuf[p]); zmax = std::max(zmax, zbuf[p]); any = true;
        }
    }
    if (!any) return;
    float near_d = 1e9f, far_d = -1e9f;
    for (const auto& lc : m_config.layers) { near_d = std::min(near_d, lc.depth_meters); far_d = std::max(far_d, lc.depth_meters); }
    if (!(near_d < far_d)) { near_d = 0.5f; far_d = 3.0f; }
    const float span = (float)std::max(1, (int)zmax - (int)zmin);
    for (std::size_t fi = 0; fi < frames.size(); ++fi) {
        auto& f = frames[fi];
        // ZBUF is a dense surface, not a collection of object depths. Store the
        // emulator value as 0=far .. 255=near for the renderer's depth mesh.
        f.depth_map.resize(npix);
        f.depth_meters = (near_d + far_d) * 0.5f;
        for (std::size_t p = 0; p < npix; ++p) {
            const float t = ((float)zbuf[p] - (float)zmin) / span;
            f.depth_map[p] = (uint8_t)std::lround(std::clamp(t, 0.0f, 1.0f) * 255.0f);
        }
        // The renderer's per-pixel Y-depth relief mesh pushes each pixel an
        // ADDITIONAL up to +/-uYDepthSpread/2 metres (2.5m total range for
        // this ZBuffer path) toward/away from the camera based on this dv,
        // on top of (not instead of) the layer's own depth_meters -- but
        // its on-screen WIDTH is sized once for depth_meters alone (see
        // perspective compensation in openxr_shell.cpp), never re-derived
        // for that extra push. For a layer whose z-buffer content is a
        // single uniform value (z_min == z_max -- true for every Neo Geo
        // auto-created z-band layer, see GameConfig::update_z_splits()),
        // dv is IDENTICAL for every pixel in the layer, so this relief adds
        // no actual internal depth variation -- it just uniformly shoves
        // the whole flat layer to a different distance than its width was
        // computed for. Worst at the z-extremes (dv near 0 or 1, i.e.
        // backdrop and fix): observed on-device as those two specific
        // layers rendering hugely mis-sized relative to everything else
        // ("two overlapping copies of the scene at very different sizes").
        // Only enable the relief for layers that actually have internal
        // z-range to relieve.
        const bool uniform_z_layer = fi < m_config.layers.size() &&
            m_config.layers[fi].z_min == m_config.layers[fi].z_max;
        f.zbuffer_depth_valid = f.has_pixels && !uniform_z_layer;
        if (f.object_boxes.empty() || f.rgba.size() < npix * 4u) continue;
        for (auto& box : f.object_boxes) {
            std::vector<uint8_t> values;
            for (int y = std::max(0, box.min_y); y <= std::min(h - 1, box.max_y); ++y)
                for (int x = std::max(0, box.min_x); x <= std::min(w - 1, box.max_x); ++x) {
                    const std::size_t p = (std::size_t)y * (std::size_t)w + (std::size_t)x;
                    if (f.rgba[p * 4u + 3u] > 5) values.push_back(zbuf[p]);
                }
            if (!values.empty()) {
                auto mid = values.begin() + values.size() / 2; std::nth_element(values.begin(), mid, values.end());
                const float t = ((float)*mid - (float)zmin) / span;
                box.depth_meters = far_d + (near_d - far_d) * t; f.zbuffer_depth_valid = true;
            }
        }
    }
}

void LayerProcessor::fill_full_frame(LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h,
                                     const uint8_t* depth_map_src, std::size_t depth_map_npix) {
    prepare_frame(f, lc, w, h, false);
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    for (std::size_t i = 0; i < npix; ++i) {
        to_rgba(src[i], &f.rgba[i * 4]);
        f.rgba[i * 4 + 3] = 255; // fully opaque
    }
    if (depth_map_src && depth_map_npix == npix)
        f.depth_map.assign(depth_map_src, depth_map_src + npix);
    else
        f.depth_map.clear();
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

    // Propagate Y-depth map for sprite layers when available
    const auto& src_dmap = frame->layers[li].depth_map;
    if (!src_dmap.empty()) {
        const std::size_t dcopy = std::min(npix, src_dmap.size());
        f.depth_map.resize(npix, 0u);
        std::memcpy(f.depth_map.data(), src_dmap.data(), dcopy);
    } else {
        f.depth_map.clear();
    }
}
void LayerProcessor::fill_mame_occupancy_residual(
    LayerFrame& f, const LayerConfig& lc, const uint32_t* src,
    const qrd::FrameOutput* frame, int w, int h)
{
    if (!src) {
        prepare_frame(f, lc, w, h, true);
        return;
    }
    // Invalid, unavailable, or uncommitted capture data is an immediate flat
    // fallback. The mode remains selected so the next valid frame can retry.
    if (!frame || !frame->mame_occupancy_available ||
        !frame->mame_occupancy_valid) {
        fill_full_frame(f, lc, src, w, h);
        return;
    }
    // Start as a complete safety net. process_into() removes only pixels that
    // have been proven by an instrumented bucket.
    fill_full_frame(f, lc, src, w, h);
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

    bool key_black[256] = {};
    for (int ri : zbuf_indices) {
        const auto& lc = m_config.layers[ri];
        const int zlo = lc.z_min;
        const int zhi = std::min((int)lc.z_max, 255);
        for (int z = zlo; z <= zhi; ++z) {
            z_to_frame[z] = static_cast<int8_t>(ri);
            key_black[z] = lc.zbuffer_key_black;
        }
    }

    constexpr int k_black_threshold = 12; // per-channel; catches near-black too, not just pure 0
    const int npix = w * h;
    for (int i = 0; i < npix; ++i) {
        const uint8_t z = zbuf[i];
        const int8_t fi = z_to_frame[z];
        if (fi < 0) continue;
        uint8_t rgba[4];
        to_rgba(src[i], rgba);
        if (key_black[z] && rgba[0] < k_black_threshold && rgba[1] < k_black_threshold &&
            rgba[2] < k_black_threshold) {
            continue; // leave transparent
        }
        LayerFrame& lf = frames[fi];
        uint8_t* dst = lf.rgba.data() + i * 4;
        dst[0] = rgba[0]; dst[1] = rgba[1]; dst[2] = rgba[2];
        dst[3] = 255; // opaque
        lf.has_pixels = true;
    }

    // Fix layer "black bar" cropping (desktop RetroDepth's shmem_reader.cpp:224-244
    // equivalent): the fix layer's quad spans the whole canvas like every other
    // z-band layer, but its real content (insert-coin text, HUD digits) is usually
    // a small region -- everything else is either genuinely transparent (fix drew
    // nothing there) or was just black-keyed above. Left as a full-canvas quad,
    // that untrimmed empty margin still costs a full-size draw and can show as
    // stray bars if anything ever un-keys it. Trim the quad's actual content to
    // its tight horizontal bounding box: scan for the leftmost/rightmost column
    // that has ANY opaque pixel (using the same k_alpha_threshold as
    // compute_object_boxes/desktop RetroDepth's kAlphaOpaqueThreshold=5), and
    // clear alpha outside that range so the wedge/object-box logic downstream
    // sees only the real content span, not the full frame width.
    constexpr uint8_t k_alpha_threshold = 5;
    for (int ri : zbuf_indices) {
        if (!m_config.layers[ri].zbuffer_key_black) continue;
        LayerFrame& lf = frames[ri];
        if (!lf.has_pixels || lf.rgba.size() < (std::size_t)npix * 4u) continue;

        int left = -1, right = -1;
        for (int x = 0; x < w && left < 0; ++x) {
            for (int y = 0; y < h; ++y) {
                if (lf.rgba[((std::size_t)y * w + x) * 4u + 3u] > k_alpha_threshold) { left = x; break; }
            }
        }
        if (left < 0) continue; // nothing actually opaque this frame
        for (int x = w - 1; x >= left; --x) {
            bool found = false;
            for (int y = 0; y < h; ++y) {
                if (lf.rgba[((std::size_t)y * w + x) * 4u + 3u] > k_alpha_threshold) { found = true; break; }
            }
            if (found) { right = x; break; }
        }

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < left; ++x) lf.rgba[((std::size_t)y * w + x) * 4u + 3u] = 0;
            for (int x = right + 1; x < w; ++x) lf.rgba[((std::size_t)y * w + x) * 4u + 3u] = 0;
        }
    }
}

void LayerProcessor::compute_object_boxes(LayerFrame& frame) {
    constexpr int k_alpha_threshold = 5;
    // Not a functional limit — GPU-side rendering (SSBO) has no fixed box count.
    // This only guards against a pathological frame with thousands of 1px blobs.
    constexpr std::size_t k_object_box_safety_cap = 4096;

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

        if (frame.object_boxes.size() >= k_object_box_safety_cap) {
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
void LayerProcessor::compute_extrude_runs(LayerFrame& frame) {
    constexpr int k_alpha_threshold = 5;
    frame.object_boxes.clear();
    frame.bbox_eligible = false;
    // Unlike compute_object_boxes/wedge, a fully-opaque layer (no transparency at all) is fine
    // here — it just becomes a handful of full-width row-runs, not thousands of boxes — so this
    // only requires at least one opaque pixel (has_pixels), not wedge_eligible's opaque+transparent
    // combo. That combo excludes solid backgrounds entirely, which was silently producing zero
    // boxes (and looking like nothing had changed) for any solid background layer.
    if (!frame.has_pixels || frame.width <= 0 || frame.height <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const std::size_t npix = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (frame.rgba.size() < npix * 4u) return;

    auto opaque_at = [&](int x, int y) -> bool {
        return frame.rgba[(static_cast<std::size_t>(y) * (std::size_t)w + (std::size_t)x) * 4u + 3u]
               > k_alpha_threshold;
    };

    // Row-wise runs: one thin box per contiguous stretch of opaque pixels along a row.
    std::vector<ObjectBoundingBox> row_runs;
    for (int y = 0; y < h; ++y) {
        int run_start = -1;
        for (int x = 0; x < w; ++x) {
            const bool op = opaque_at(x, y);
            if (op && run_start < 0) run_start = x;
            if (!op && run_start >= 0) {
                row_runs.push_back({run_start, y, x - 1, y});
                run_start = -1;
            }
        }
        if (run_start >= 0) row_runs.push_back({run_start, y, w - 1, y});
    }

    // Column-wise runs: same idea, swept the other axis.
    std::vector<ObjectBoundingBox> col_runs;
    for (int x = 0; x < w; ++x) {
        int run_start = -1;
        for (int y = 0; y < h; ++y) {
            const bool op = opaque_at(x, y);
            if (op && run_start < 0) run_start = y;
            if (!op && run_start >= 0) {
                col_runs.push_back({x, run_start, x, y - 1});
                run_start = -1;
            }
        }
        if (run_start >= 0) col_runs.push_back({x, run_start, x, h - 1});
    }

    // Sparse layers (starfields, scattered particle sprites, etc.) skip the run-merge entirely
    // and get one box per opaque pixel instead. A single isolated pixel is already its own run
    // on both axes, so merging isn't why those look flat — but a thin MULTI-pixel run (e.g. a
    // 3-star horizontal cluster) still gets welded into one elongated 1px-tall/wide box today,
    // and a box that thin reads as just a front/back pair with an invisible edge-on side wall.
    // Per-pixel boxes sidestep that: every opaque pixel gets its own tiny box with 4 real side
    // faces, no matter how it clusters with its neighbours.
    // Only applied when the pixel count is small enough to stay well under the SSBO's practical
    // budget — a solid/dense layer (a full background) falls back to the merged runs exactly as
    // before, so this can't blow up box count for anything but genuinely sparse content.
    constexpr std::size_t k_per_pixel_cap = 4096;
    std::size_t opaque_count = 0;
    for (const auto& r : row_runs) opaque_count += (std::size_t)(r.max_x - r.min_x + 1);
    if (opaque_count > 0 && opaque_count <= k_per_pixel_cap) {
        std::vector<ObjectBoundingBox> per_pixel;
        per_pixel.reserve(opaque_count);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (opaque_at(x, y)) per_pixel.push_back({x, y, x, y});
            }
        }
        frame.object_boxes = std::move(per_pixel);
    } else {
        // Both axes voxelize exactly the same set of opaque pixels — pick whichever produced
        // fewer boxes this frame (content-dependent: tall sprites merge better as column runs,
        // wide terrain/background strips merge better as row runs), so we're never stuck paying
        // for the worse axis.
        frame.object_boxes = (col_runs.size() < row_runs.size()) ? std::move(col_runs) : std::move(row_runs);
    }
    frame.bbox_eligible = !frame.object_boxes.empty();
}
