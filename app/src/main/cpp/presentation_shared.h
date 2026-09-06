#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "game_config.h"
#include "gles_renderer.h"
#include "layer_processor.h"
#include "vr_state.h"

namespace qrd::presentation {

inline VrState default_vr_state_for_backend(BackendKind kind) {
    VrState vs;
    vs.gamma = 1.5f;
    vs.contrast = 1.1f;
    vs.saturation = 1.0f;
    vs.brightness = 1.0f;
    vs.immersive_beta_enabled = false;
    vs.layers_3d = false;
    vs.depth_mode = DepthMode::PixelExtrude;
    vs.upscale_mode = UpscaleMode::Off;
    vs.shadows = false;
    vs.ambilight = true;
    vs.perspective_comp = true;
    // Parallax peek on at the first step (k_parallax_steps[1]) for every
    // backend -- see VrState::parallax_ratio.
    vs.parallax_ratio = 0.005f;
    // Controller models are useful for tutorials, but should not be part of
    // the normal presentation default. PSX native mode is also enabled by
    // default; its renderer is already isolated from the shell overlay.
    vs.show_controller_models = false;
    vs.audio_spatial_mode = 2;
    vs.audio_screen_lock = true;
    switch (kind) {
    case BackendKind::Genesis: vs.vr_resolution_scale = 1.5f; break;
    case BackendKind::Gba:
    case BackendKind::Gb:      vs.vr_resolution_scale = 1.0f; break;
    default:                   vs.vr_resolution_scale = 1.0f; break;
    }
    return vs;
}

// Per-layer yaw used by the layer deck (see OpenXrShell's left-stick click
// cycle). Bookshelf model, NOT a fan-out: every layer stays exactly where it
// already sits on the canvas -- no sideways swing, no orbit around the viewer
// -- and instead rotates in place about its own vertical axis, so the stack
// opens like the covers of a book standing on a shelf and you see each layer
// edge-on/angled rather than face-on. Deeper slots turn progressively further,
// all in the same direction, so no layer hides the one behind it.
//
// Purely a viewing/picking aid; back-to-front depth order is unaffected.
// Shared between the CPU-side raycast hit-test (openxr_shell.cpp) and the
// GPU-side per-layer draw (gles_renderer.cpp) so the laser picking a layer
// always agrees with how it's actually drawn.
inline float layer_deck_yaw(int slot, int total_slots, float spread_scale = 1.0f) {
    if (total_slots <= 1 || slot < 0) return 0.0f;
    // Normalized by slot count (not a fixed per-slot step) so the last layer
    // always reaches k_max_yaw_rad * spread_scale regardless of how many
    // layers are in the stack -- spread_scale is a direct 0..1 fraction of
    // the full opening angle (1.0 = the deepest layer turned nearly edge-on).
    constexpr float k_max_yaw_rad = 1.35f; // ~77 degrees
    const float norm = (float)slot / (float)(total_slots - 1); // 0..1
    return norm * k_max_yaw_rad * spread_scale;
}

inline int layer_index_by_id(const GameConfig& config, const char* id) {
    for (int i = 0; i < (int)config.layers.size(); ++i) {
        if (config.layers[i].id == id) return i;
    }
    return -1;
}

inline GameConfig subset_config_by_ids(const GameConfig& src,
                                       std::initializer_list<const char*> ids) {
    GameConfig cfg;
    cfg.game = src.game;
    cfg.virtual_width = src.virtual_width;
    cfg.virtual_height = src.virtual_height;
    cfg.quad_y_meters = src.quad_y_meters;
    cfg.dynamic_layers = src.dynamic_layers;
    cfg.layers.reserve(ids.size());
    for (const char* id : ids) {
        const int idx = layer_index_by_id(src, id);
        if (idx >= 0) cfg.layers.push_back(src.layers[idx]);
    }
    return cfg;
}

inline GameConfig make_snes_config_for_filter(int mode) {
    const GameConfig base = GameConfig::make_default_snes();
    auto enable_all_by_default = [](GameConfig cfg) {
        for (auto& layer : cfg.layers) {
            layer.default_enabled = true;
            layer.default_ambilight = true;
        }
        return cfg;
    };
    switch (mode) {
    case 0:
        return base;
    case 1:
        return subset_config_by_ids(base, {
            "backdrop", "bg_far_lo", "sprite_p0", "bg_far_hi", "sprite_p1",
            "bg1_lo", "bg0_lo", "sprite_p2", "bg1_hi", "bg0_hi", "sprite_p3",
        });
    case 2:
        return subset_config_by_ids(base, {
            "pc_bg4", "pc_bg3", "pc_bg2", "pc_bg1", "pc_obj",
        });
    case 3:
    default:
        {
            GameConfig cfg = enable_all_by_default(subset_config_by_ids(base, {
                "pc_bg3", "pc_obj", "pc_bg1", "pc_bg2", "pc_bg4", "backdrop",
            }));
            for (auto& layer : cfg.layers) {
                if (layer.id == "pc_bg3") {
                    layer.extraction_type = ExtractionType::VisibleSourceFinal;
                    layer.layer_index = 2;
                } else if (layer.id == "pc_obj") {
                    layer.extraction_type = ExtractionType::VisibleSourceFinal;
                    layer.layer_index = 4;
                } else if (layer.id == "pc_bg1") {
                    layer.extraction_type = ExtractionType::VisibleSourceFinal;
                    layer.layer_index = 0;
                } else if (layer.id == "pc_bg2") {
                    layer.extraction_type = ExtractionType::PerLayerCapture;
                    layer.layer_index = 1;
                } else if (layer.id == "pc_bg4") {
                    layer.extraction_type = ExtractionType::VisibleSourceFinal;
                    layer.layer_index = 3;
                } else if (layer.id == "backdrop") {
                    layer.extraction_type = ExtractionType::VisibleSourceFinal;
                    layer.layer_index = 5;
                }
            }
            for (auto& layer : cfg.layers) {
                layer.depth_meters += 3.0f;
            }
            return cfg;
        }
    }
}

inline GameConfig default_config_for_backend(BackendKind kind, int snes_mode = 3) {
    switch (kind) {
    case BackendKind::Snes:    return make_snes_config_for_filter(snes_mode);
    case BackendKind::Genesis: return GameConfig::make_default_genesis();
    case BackendKind::Gba:     return GameConfig::make_default_gba();
    case BackendKind::Gb:      return GameConfig::make_default_gb();
    case BackendKind::Nes:     return GameConfig::make_default_nes();
    case BackendKind::Pce:     return GameConfig::make_default_pce();
    case BackendKind::Sms:     return GameConfig::make_default_sms();
    case BackendKind::Mame:    return GameConfig::make_default_mame_full_frame();
    case BackendKind::Saturn: return GameConfig::make_default_saturn();
    case BackendKind::Psx:     return GameConfig::make_default_psx();
    }
    return GameConfig::make_flat();
}

inline bool is_snes_discovery_config(const GameConfig& config) {
    return config.game == "snes"
        && config.layers.size() == 16
        && config.layers[11].id == "pc_bg4"
        && config.layers[15].id == "pc_obj";
}

inline bool is_snes_hybrid_config(const GameConfig& config) {
    return config.game == "snes"
        && config.layers.size() == 6
        && config.layers[0].id == "pc_bg3"
        && config.layers[1].id == "pc_obj"
        && config.layers[2].id == "pc_bg1"
        && config.layers[3].id == "pc_bg2"
        && config.layers[4].id == "pc_bg4"
        && config.layers[5].id == "backdrop";
}

inline std::vector<int> default_layer_order_for_config(const GameConfig& config) {
    const int n = (int)config.layers.size();
    std::vector<int> order;
    order.reserve(n);

    if (is_snes_discovery_config(config)) {
        for (int i = 10; i >= 0; --i) order.push_back(i);
        for (int i = 15; i >= 11; --i) order.push_back(i);
        return order;
    }

    if (is_snes_hybrid_config(config)) {
        for (int i = 0; i < n; ++i) order.push_back(i);
        return order;
    }

    if (config.game == "mame_neogeo") {
        // See the matching comment on openxr_shell.cpp's own copy of this
        // function: Neo Geo's layer list grows permanently over the play
        // session in roughly-but-not-exactly z order, so show it sorted by
        // real z_min instead of the generic reverse-creation-order fallback.
        for (int i = 0; i < n; ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return config.layers[a].z_min < config.layers[b].z_min;
        });
        return order;
    }

    if (config.game == "mame_taito") {
        // The Taito display contract is near -> far:
        // taito_sprites, taito_fg, full_frame, taito_bg.
        static constexpr const char* kTaitoOrder[] = {
            "taito_sprites", "taito_fg", "full_frame", "taito_bg",
        };
        std::vector<bool> used(n, false);
        for (const char* wanted : kTaitoOrder) {
            for (int i = 0; i < n; ++i) {
                if (!used[i] && config.layers[i].id == wanted) {
                    order.push_back(i);
                    used[i] = true;
                    break;
                }
            }
        }
        for (int i = n - 1; i >= 0; --i) {
            if (!used[i]) order.push_back(i);
        }
        return order;
    }

    for (int i = n - 1; i >= 0; --i) order.push_back(i);
    return order;
}

inline void ensure_layer_runtime_state_matches_config(const GameConfig& config,
                                                      std::vector<std::string>& layer_names,
                                                      std::vector<int>& layer_order,
                                                      std::vector<bool>& layer_enabled,
                                                      std::vector<bool>& layer_ambilight,
                                                      std::vector<int>& layer_side_color) {
    const int n = (int)config.layers.size();

    layer_names.clear();
    layer_names.reserve(n);
    for (const auto& lc : config.layers) {
        layer_names.push_back(lc.id.empty() ? "Layer" : lc.id);
    }

    const std::vector<int> default_order = default_layer_order_for_config(config);
    bool needs_order_repair = ((int)layer_order.size() != n);
    if (!needs_order_repair) {
        std::vector<bool> seen(n, false);
        for (int idx : layer_order) {
            if (idx < 0 || idx >= n || seen[idx]) {
                needs_order_repair = true;
                break;
            }
            seen[idx] = true;
        }
    }
    if (!needs_order_repair && config.game == "genesis" && n > 1) {
        bool is_identity = true;
        for (int i = 0; i < n; ++i) {
            if (layer_order[i] != i) {
                is_identity = false;
                break;
            }
        }
        if (is_identity) needs_order_repair = true;
    }
    if (needs_order_repair) layer_order = default_order;

    // Taito's full-frame image is a safety net between the foreground and
    // background planes. Repair only that relative position so the user's
    // ordering of the actual source planes is preserved.
    if (config.game == "mame_taito") {
        int full_frame_idx = -1;
        int bg_idx = -1;
        for (int i = 0; i < n; ++i) {
            if (config.layers[i].id == "full_frame") full_frame_idx = i;
            if (config.layers[i].id == "taito_bg") bg_idx = i;
        }
        if (full_frame_idx >= 0 && bg_idx >= 0) {
            auto full_it = std::find(layer_order.begin(), layer_order.end(), full_frame_idx);
            if (full_it != layer_order.end()) layer_order.erase(full_it);
            auto bg_it = std::find(layer_order.begin(), layer_order.end(), bg_idx);
            if (bg_it != layer_order.end()) layer_order.insert(bg_it, full_frame_idx);
        }
    }

    if ((int)layer_enabled.size() != n) {
        layer_enabled.resize(n, true);
        for (int i = 0; i < n; ++i) layer_enabled[i] = config.layers[i].default_enabled;
    }
    if ((int)layer_ambilight.size() != n) {
        layer_ambilight.resize(n, true);
        for (int i = 0; i < n; ++i) layer_ambilight[i] = config.layers[i].default_ambilight;
    }
    if ((int)layer_side_color.size() != n) {
        // 6 = Darker: the shipped default for every layer on every backend —
        // a real depth cue on extruded sides (halfway between the sprite's
        // own colors and pure black) without a flat-black silhouette some
        // found too heavy. Still just the seed value: a saved .ini/preset or
        // an explicit per-layer choice always overrides it as before.
        layer_side_color.resize(n, 6);
    }
}

inline void sync_cached_layer_geometry_from_config(std::vector<LayerFrame>& layer_frames,
                                                   const GameConfig& config) {
    const int n = std::min((int)layer_frames.size(), (int)config.layers.size());
    for (int i = 0; i < n; ++i) {
        const auto& lc = config.layers[i];
        auto& lf = layer_frames[i];
        lf.depth_meters = lc.depth_meters;
        lf.quad_width_meters = lc.quad_width_meters;
        lf.copies = lc.copies;
        lf.persp_comp_scale = 1.0f;
    }
}

inline int baseline_copy_count(const LayerFrame& frame) {
    return frame.copies.empty() ? GlesRenderer::k_max_copies : (int)frame.copies.size();
}

inline float baseline_copy_step(const LayerFrame& frame) {
    if (!frame.copies.empty() && frame.copies.back() > 0.0f) {
        return frame.copies.back() / (float)frame.copies.size();
    }
    return GlesRenderer::k_default_copy_step;
}

inline void rebuild_copy_offsets(std::vector<float>& copies, int copy_count, float copy_step) {
    if (copy_count <= 0) {
        copies.clear();
        return;
    }
    copies.resize(copy_count);
    for (int i = 0; i < copy_count; ++i) {
        copies[i] = (float)(i + 1) * copy_step;
    }
}

inline void apply_layer_auto_dup_visible(std::vector<LayerFrame*>& layer_frames, int auto_dup_percent) {
    if (auto_dup_percent < 0 || layer_frames.empty() || !layer_frames[0]) return;
    const int anchor_count = baseline_copy_count(*layer_frames[0]);
    const int far_target = std::clamp((int)std::lround((double)anchor_count * (double)auto_dup_percent / 100.0), 0, 64);
    const int n = (int)layer_frames.size();
    for (int i = 0; i < n; ++i) {
        if (!layer_frames[i]) continue;
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        const int target_count = std::clamp((int)std::lround(anchor_count + (far_target - anchor_count) * t), 0, 64);
        rebuild_copy_offsets(layer_frames[i]->copies, target_count, baseline_copy_step(*layer_frames[i]));
    }
}

inline void compact_visible_layer_depths(std::vector<LayerFrame*>& layer_frames) {
    if (layer_frames.size() < 2) return;
    LayerFrame* first = nullptr;
    for (LayerFrame* lf : layer_frames) {
        if (lf) {
            first = lf;
            break;
        }
    }
    if (!first) return;
    float near_d = first->depth_meters;
    float far_d = first->depth_meters;
    for (LayerFrame* lf : layer_frames) {
        if (!lf) continue;
        near_d = std::min(near_d, lf->depth_meters);
        far_d = std::max(far_d, lf->depth_meters);
    }
    for (int i = 0; i < (int)layer_frames.size(); ++i) {
        if (!layer_frames[i]) continue;
        const float t = (layer_frames.size() > 1) ? (float)i / (float)(layer_frames.size() - 1) : 0.0f;
        layer_frames[i]->depth_meters = near_d + (far_d - near_d) * t;
    }
}

// Depth Arrangement widget support: same near/far-envelope idea as
// compact_visible_layer_depths above, but instead of always spacing layers
// evenly (t = i/(N-1)), each layer lands at the user-arranged fraction for
// its own depth SLOT (m_layer_order position, via slot_of_ref -- parallel to
// layer_frames). Deliberately recomputes near_d/far_d from each layer's
// CURRENT depth_meters every call (same as compact_visible_layer_depths, not
// a frozen snapshot) so a z-buffer backend's per-frame-dynamic depths keep
// moving live and the widget's arrangement rides on top of that live
// envelope, instead of the widget freezing an absolute canvas-metres depth
// that can drift arbitrarily far from the envelope the renderer (and
// perspective compensation, which scales quad width by depth ratio) is
// tuned for -- that absolute-depth approach is what caused both a stale
// layer order once gameplay moved on and a hugely oversized far layer.
inline void apply_slot_fraction_layer_depths(std::vector<LayerFrame*>& layer_frames,
                                             const std::vector<int>& slot_of_ref,
                                             const std::vector<float>& slot_fraction) {
    if (layer_frames.empty() || layer_frames.size() != slot_of_ref.size()) return;
    LayerFrame* first = nullptr;
    for (LayerFrame* lf : layer_frames) {
        if (lf) { first = lf; break; }
    }
    if (!first) return;
    float near_d = first->depth_meters;
    float far_d = first->depth_meters;
    for (LayerFrame* lf : layer_frames) {
        if (!lf) continue;
        near_d = std::min(near_d, lf->depth_meters);
        far_d = std::max(far_d, lf->depth_meters);
    }
    for (size_t i = 0; i < layer_frames.size(); ++i) {
        if (!layer_frames[i]) continue;
        const int slot = slot_of_ref[i];
        const float t = (slot >= 0 && slot < (int)slot_fraction.size())
                       ? slot_fraction[slot]
                       : ((layer_frames.size() > 1) ? (float)i / (float)(layer_frames.size() - 1) : 0.0f);
        layer_frames[i]->depth_meters = near_d + (far_d - near_d) * t;
    }
}

// "Accordion" / bookshelf depth layout: instead of squeezing however many layers are
// currently visible into whatever [near_d, far_d] span their raw formula-derived depths
// happen to occupy (compact_visible_layer_depths, above -- which can leave layers bunched
// imperceptibly close together if their source values were already close), give every
// layer a fixed depth "slab" of slab_thickness metres and stack them back-to-back with no
// gap, starting at near_d. So layer count grows the total depth span instead of being
// squeezed into a fixed one -- looking from the side you'd see a stack of equal-thickness
// book spines, not a handful of near-coplanar sheets. layer_frames is assumed to already be
// in near-to-far order (index 0 = nearest) -- e.g. mame_neogeo's default_layer_order_for_config
// z_min-descending sort, which puts the highest z (nearest) first.
inline void apply_accordion_layer_depths(std::vector<LayerFrame*>& layer_frames,
                                         float near_d, float slab_thickness) {
    int i = 0;
    for (LayerFrame* lf : layer_frames) {
        if (!lf) continue;
        lf->depth_meters = near_d + (float)i * slab_thickness;
        ++i;
    }
}

inline void apply_perspective_comp_to_refs(std::vector<LayerFrame*>& refs) {
    if (refs.size() <= 1) return;
    int ref = -1;
    for (int i = 0; i < (int)refs.size(); ++i) {
        if (!refs[i]) continue;
        if (ref < 0 || refs[i]->depth_meters < refs[ref]->depth_meters) ref = i;
    }
    if (ref < 0 || refs[ref]->depth_meters < 0.01f) return;
    const float ref_depth = refs[ref]->depth_meters;
    const float ref_width = refs[ref]->quad_width_meters;
    for (LayerFrame* lf : refs) {
        if (!lf) continue;
        lf->quad_width_meters = ref_width * (lf->depth_meters / ref_depth);
        lf->persp_comp_scale = 1.0f;
    }
}

inline std::array<float, 4> lerp_rgba(const std::array<float, 4>& a,
                                      const std::array<float, 4>& b,
                                      float t) {
    std::array<float, 4> out{};
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    return out;
}

inline bool sample_environment_band(const LayerFrame& frame, int y0, int y1,
                                    std::array<float, 4>& out_color) {
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
    const int x_step = std::max(1, frame.width / 64);
    const int y_step = std::max(1, frame.height / 64);
    const int clamped_y0 = std::clamp(y0, 0, frame.height);
    const int clamped_y1 = std::clamp(y1, clamped_y0 + 1, frame.height);
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    float count = 0.0f;
    for (int y = clamped_y0; y < clamped_y1; y += y_step) {
        for (int x = 0; x < frame.width; x += x_step) {
            const std::size_t idx = (std::size_t)(y * frame.width + x) * 4u;
            const float alpha = frame.rgba[idx + 3] / 255.0f;
            if (alpha <= 0.05f) continue;
            r += (frame.rgba[idx + 0] / 255.0f) * alpha;
            g += (frame.rgba[idx + 1] / 255.0f) * alpha;
            b += (frame.rgba[idx + 2] / 255.0f) * alpha;
            a += alpha;
            count += 1.0f;
        }
    }
    if (count <= 0.0f || a <= 0.0f) return false;
    out_color = {
        std::clamp(r / a, 0.0f, 1.0f),
        std::clamp(g / a, 0.0f, 1.0f),
        std::clamp(b / a, 0.0f, 1.0f),
        1.0f
    };
    return true;
}

// Source rows of a layer that feed the 12 environment bands, per mode:
//   Sky    → top half    (colours the upper hemisphere)
//   Ground → bottom half (colours the lower hemisphere)
//   Full   → whole image (colours the full 180°, pole to pole)
// Band 0 is always the topmost sampled row and band 11 the bottommost, so the shader's
// band→elevation rule is identical in every mode — only the span differs.
inline void environment_sample_rows(EnvironmentSphereMode mode, int height,
                                    int& out_y0, int& out_y1) {
    const int h = std::max(1, height);
    switch (mode) {
        case EnvironmentSphereMode::SkyOnly:
            out_y0 = 0;
            out_y1 = std::max(1, h / 2);
            break;
        case EnvironmentSphereMode::Ground:
            out_y0 = std::min(h - 1, h / 2);
            out_y1 = h;
            break;
        default:
            out_y0 = 0;
            out_y1 = h;
            break;
    }
}

// A layer feeds the environment sphere only when it has usable pixels AND its per-layer
// ambilight toggle (the AMB button in the layer panel) is on.
inline bool layer_feeds_environment(const LayerFrame* lf) {
    return lf && lf->has_pixels && lf->contrib_ambilight
        && lf->width > 0 && lf->height > 0 && !lf->rgba.empty();
}

template <typename Sample>
inline bool build_environment_sample_from_layer(const LayerFrame& frame,
                                                EnvironmentSphereMode mode,
                                                Sample& out_sample) {
    out_sample.valid = false;
    if (!layer_feeds_environment(&frame)) return false;
    if (mode == EnvironmentSphereMode::Off) return false;
    constexpr int kBandCount = 12;
    int sample_y0 = 0;
    int sample_y1 = frame.height;
    environment_sample_rows(mode, frame.height, sample_y0, sample_y1);
    const int sample_h = std::max(1, sample_y1 - sample_y0);
    std::array<bool, kBandCount> valid{};
    for (int i = 0; i < kBandCount; ++i) {
        const int y0 = sample_y0 + (sample_h * i) / kBandCount;
        const int y1 = sample_y0 + (sample_h * (i + 1)) / kBandCount;
        valid[i] = sample_environment_band(frame, y0, std::max(y0 + 1, y1), out_sample.bands[i]);
    }
    // Treat the image as three ambient source zones: top screen, screen centre, bottom
    // screen. Sky/Ground deliberately use one representative color instead of spreading
    // the whole image over the hemisphere, which makes their placement unambiguous.
    if (mode == EnvironmentSphereMode::SkyOnly) {
        const auto c = out_sample.bands[0];
        for (int i = 0; i < 6; ++i) out_sample.bands[i] = c;
    } else if (mode == EnvironmentSphereMode::Ground) {
        const auto c = out_sample.bands[11];
        for (int i = 6; i < kBandCount; ++i) out_sample.bands[i] = c;
    }
    bool any_valid = false;
    for (bool ok : valid) any_valid = any_valid || ok;
    if (!any_valid) return false;
    for (int i = 0; i < kBandCount; ++i) {
        if (valid[i]) continue;
        int left = i - 1;
        int right = i + 1;
        while (left >= 0 && !valid[left]) --left;
        while (right < kBandCount && !valid[right]) ++right;
        if (left >= 0) out_sample.bands[i] = out_sample.bands[left];
        else if (right < kBandCount) out_sample.bands[i] = out_sample.bands[right];
    }

    if (mode == EnvironmentSphereMode::SkyOnly) {
        const auto c = out_sample.bands[0];
        for (int i = 0; i < 6; ++i) out_sample.bands[i] = c;
    } else if (mode == EnvironmentSphereMode::Ground) {
        const auto c = out_sample.bands[11];
        for (int i = 6; i < kBandCount; ++i) out_sample.bands[i] = c;
    }
    // No hemisphere masking here — all 12 bands stay meaningful in every mode, and the
    // shader restricts Sky/Ground to their hemisphere and fades them out at the horizon.
    out_sample.valid = true;
    return true;
}

template <typename Sample>
inline bool build_environment_sample_from_visible_layers(const std::vector<LayerFrame*>& frames,
                                                         EnvironmentSphereMode mode,
                                                         Sample& out_sample) {
    out_sample.valid = false;
    if (mode == EnvironmentSphereMode::Off) return false;

    const LayerFrame* farthest_valid = nullptr;
    float near_depth = 0.0f;
    float far_depth = 0.0f;
    bool have_depth_range = false;
    int valid_layer_count = 0;

    for (const LayerFrame* lf : frames) {
        if (!layer_feeds_environment(lf)) continue;
        if (!farthest_valid || lf->depth_meters > farthest_valid->depth_meters) {
            farthest_valid = lf;
        }
        if (!have_depth_range) {
            near_depth = far_depth = lf->depth_meters;
            have_depth_range = true;
        } else {
            near_depth = std::min(near_depth, lf->depth_meters);
            far_depth = std::max(far_depth, lf->depth_meters);
        }
        ++valid_layer_count;
    }

    if (!farthest_valid) return false;
    // Every mode gets the multi-layer weighted blend; the single-layer path is only a
    // fallback for when there is genuinely just one contributing layer.
    if (valid_layer_count <= 1) {
        return build_environment_sample_from_layer(*farthest_valid, mode, out_sample);
    }

    constexpr int kBandCount = 12;
    std::array<bool, kBandCount> valid{};
    std::array<std::array<float, 5>, kBandCount> accum{};

    for (const LayerFrame* lf : frames) {
        if (!layer_feeds_environment(lf)) continue;

        const float norm_depth = (far_depth > near_depth)
            ? std::clamp((lf->depth_meters - near_depth) / (far_depth - near_depth), 0.0f, 1.0f)
            : 1.0f;
        const float layer_weight = 0.15f + 0.85f * std::pow(norm_depth, 1.5f);

        int sample_y0 = 0;
        int sample_y1 = lf->height;
        environment_sample_rows(mode, lf->height, sample_y0, sample_y1);
        const int sample_h = std::max(1, sample_y1 - sample_y0);

        for (int i = 0; i < kBandCount; ++i) {
            const int y0 = sample_y0 + (sample_h * i) / kBandCount;
            const int y1 = sample_y0 + (sample_h * (i + 1)) / kBandCount;
            std::array<float, 4> band{};
            if (!sample_environment_band(*lf, y0, std::max(y0 + 1, y1), band)) continue;
            accum[i][0] += band[0] * layer_weight;
            accum[i][1] += band[1] * layer_weight;
            accum[i][2] += band[2] * layer_weight;
            accum[i][3] += band[3] * layer_weight;
            accum[i][4] += layer_weight;
            valid[i] = true;
        }
    }

    bool any_valid = false;
    for (int i = 0; i < kBandCount; ++i) {
        if (!valid[i] || accum[i][4] <= 0.0f) continue;
        out_sample.bands[i] = {
            std::clamp(accum[i][0] / accum[i][4], 0.0f, 1.0f),
            std::clamp(accum[i][1] / accum[i][4], 0.0f, 1.0f),
            std::clamp(accum[i][2] / accum[i][4], 0.0f, 1.0f),
            std::clamp(accum[i][3] / accum[i][4], 0.0f, 1.0f),
        };
        any_valid = true;
    }
    if (!any_valid) return false;

    for (int i = 0; i < kBandCount; ++i) {
        if (valid[i]) continue;
        int left = i - 1;
        int right = i + 1;
        while (left >= 0 && !valid[left]) --left;
        while (right < kBandCount && !valid[right]) ++right;
        if (left >= 0) out_sample.bands[i] = out_sample.bands[left];
        else if (right < kBandCount) out_sample.bands[i] = out_sample.bands[right];
    }

    out_sample.valid = true;
    return true;
}

template <typename Sample>
inline void smooth_environment_sample(Sample& current, const Sample& target, float blend) {
    if (!target.valid) {
        for (auto& band : current.bands) band[3] *= (1.0f - blend);
        current.valid = false;
        for (const auto& band : current.bands) {
            if (band[3] > 0.01f) {
                current.valid = true;
                break;
            }
        }
        return;
    }
    if (!current.valid) {
        current = target;
        return;
    }
    for (int i = 0; i < (int)current.bands.size(); ++i) {
        current.bands[i] = lerp_rgba(current.bands[i], target.bands[i], blend);
    }
    current.valid = true;
}

inline bool layer_has_bright_samples(const LayerFrame& frame, int& bright_samples_out) {
    bright_samples_out = 0;
    if (!frame.has_pixels || frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
    for (int y = 0; y < frame.height; y += 4) {
        for (int x = 0; x < frame.width; x += 4) {
            const std::size_t idx = (std::size_t)(y * frame.width + x) * 4u;
            if (frame.rgba[idx + 3] < 32) continue;
            const uint8_t bright = std::max({frame.rgba[idx + 0], frame.rgba[idx + 1], frame.rgba[idx + 2]});
            if (bright > 20) ++bright_samples_out;
        }
    }
    return bright_samples_out > 0;
}

inline bool is_blackout_candidate(const std::vector<LayerFrame*>& frames, int& bright_samples_out) {
    bright_samples_out = 0;
    bool any_pixels = false;
    for (const LayerFrame* lf : frames) {
        if (!lf || !lf->has_pixels || lf->width <= 0 || lf->height <= 0 || lf->rgba.empty()) continue;
        any_pixels = true;
        int layer_bright = 0;
        layer_has_bright_samples(*lf, layer_bright);
        bright_samples_out += layer_bright;
    }
    if (!any_pixels) return true;
    return bright_samples_out == 0;
}

inline float blackout_reveal_pulse_scale(int64_t now, int64_t start_time) {
    constexpr float kPulseDurationNs = 120000000.0f;
    const float t = std::clamp((float)(now - start_time) / kPulseDurationNs, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);
    return 1.015f - 0.015f * ease;
}

inline void update_blackout_reveal_state(int& phase,
                                         int& blackout_candidate_frames,
                                         int& blackout_visible_frames,
                                         int64_t& reveal_start_time,
                                         std::vector<std::string>& reveal_layer_ids,
                                         bool detector_allowed,
                                         bool frame_updated,
                                         const std::vector<LayerFrame*>& render_refs,
                                         int64_t now_time) {
    if (!detector_allowed) {
        phase = 0;
        blackout_candidate_frames = 0;
        blackout_visible_frames = 0;
        reveal_start_time = 0;
        reveal_layer_ids.clear();
        return;
    }
    if (!frame_updated) return;

    int bright_samples = 0;
    const bool blackout_candidate = is_blackout_candidate(render_refs, bright_samples);
    if (blackout_candidate) {
        ++blackout_candidate_frames;
        blackout_visible_frames = 0;
    } else {
        blackout_candidate_frames = 0;
        blackout_visible_frames = (bright_samples >= 8) ? (blackout_visible_frames + 1) : 0;
    }

    if (phase == 2 && blackout_candidate_frames >= 2) {
        phase = 1;
        reveal_start_time = 0;
        reveal_layer_ids.clear();
    }

    switch (phase) {
    case 0:
        if (blackout_candidate_frames >= 2) {
            phase = 1;
            reveal_start_time = 0;
            reveal_layer_ids.clear();
        }
        break;
    case 1:
        if (blackout_visible_frames >= 2) {
            std::vector<const LayerFrame*> revealable;
            revealable.reserve(render_refs.size());
            for (const LayerFrame* lf : render_refs) {
                if (!lf || !lf->has_pixels || lf->width <= 0 || lf->height <= 0 || lf->rgba.empty()) continue;
                revealable.push_back(lf);
            }
            std::sort(revealable.begin(), revealable.end(), [](const LayerFrame* a, const LayerFrame* b) {
                return a->depth_meters > b->depth_meters;
            });
            reveal_layer_ids.clear();
            for (const LayerFrame* lf : revealable) reveal_layer_ids.push_back(lf->id);
            if (reveal_layer_ids.size() >= 2) {
                phase = 2;
                reveal_start_time = now_time;
            } else {
                phase = 0;
                reveal_start_time = 0;
                reveal_layer_ids.clear();
            }
        }
        break;
    default:
        break;
    }
}

} // namespace qrd::presentation
