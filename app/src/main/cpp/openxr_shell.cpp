#include "openxr_shell.h"
#include "button_map.h"
#include "settings_io.h"
#include "vr_state_code.h"

#include <android/log.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>
#include <sys/stat.h>
#include <dirent.h>

#define LOG_TAG "QuestRetroDepthXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace qrd {

namespace {

constexpr float k_min_vr_resolution_scale = 0.25f;
constexpr float k_max_vr_resolution_scale = 4.0f;

static float clamp_vr_resolution_scale(float scale) {
    return std::clamp(scale, k_min_vr_resolution_scale, k_max_vr_resolution_scale);
}

static float snap_vr_resolution_scale(float scale) {
    const float clamped = clamp_vr_resolution_scale(scale);
    return std::round(clamped * 4.0f) * 0.25f;
}

static uint32_t scaled_eye_extent(uint32_t recommended, float scale) {
    const float scaled = std::round((float)recommended * snap_vr_resolution_scale(scale));
    return std::max<uint32_t>(1u, (uint32_t)scaled);
}

} // namespace

// ============================================================
// Impl — all OpenXR + EGL + GL objects
// ============================================================
struct OpenXrShell::Impl {
    // OpenXR
    XrInstance       instance    = XR_NULL_HANDLE;
    XrSystemId       system_id   = XR_NULL_SYSTEM_ID;
    XrSession        session     = XR_NULL_HANDLE;
    XrSessionState   session_state = XR_SESSION_STATE_UNKNOWN;
    bool             session_running = false;
    XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    PFN_xrInitializeLoaderKHR                 pfn_init_loader      = nullptr;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR  pfn_get_gl_reqs      = nullptr;

    // XR_FB_display_refresh_rate (optional)
    PFN_xrEnumerateDisplayRefreshRatesFB pfn_enum_refresh   = nullptr;
    PFN_xrRequestDisplayRefreshRateFB    pfn_set_refresh    = nullptr;
    std::vector<float>                   available_rates;    // sorted ascending
    bool                                 has_refresh_ext    = false;

    // XR_FB_passthrough (optional)
    PFN_xrCreatePassthroughFB       pfn_create_passthrough       = nullptr;
    PFN_xrDestroyPassthroughFB      pfn_destroy_passthrough      = nullptr;
    PFN_xrPassthroughStartFB        pfn_start_passthrough        = nullptr;
    PFN_xrPassthroughPauseFB        pfn_pause_passthrough        = nullptr;
    PFN_xrCreatePassthroughLayerFB  pfn_create_passthrough_layer = nullptr;
    PFN_xrDestroyPassthroughLayerFB pfn_destroy_passthrough_layer = nullptr;
    PFN_xrPassthroughLayerPauseFB   pfn_pause_passthrough_layer  = nullptr;
    PFN_xrPassthroughLayerResumeFB  pfn_resume_passthrough_layer = nullptr;
    XrPassthroughFB                 passthrough                  = XR_NULL_HANDLE;
    XrPassthroughLayerFB            passthrough_layer            = XR_NULL_HANDLE;
    bool                            has_passthrough_ext          = false;
    bool                            has_alpha_blend_ext          = false;
    bool                            supports_alpha_blend_mode    = false;
    bool                            supports_passthrough         = false;
    bool                            passthrough_running          = false;
    bool                            passthrough_layer_running    = false;

    // EGL
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLConfig  egl_config  = nullptr;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;

    // Reference space
    XrSpace app_space   = XR_NULL_HANDLE;
    XrSpace view_space  = XR_NULL_HANDLE; // XR_REFERENCE_SPACE_TYPE_VIEW — used to locate HMD
    XrSpace local_space = XR_NULL_HANDLE; // XR_REFERENCE_SPACE_TYPE_LOCAL, identity — never replaced

    // Last-known HMD pose in app_space (updated each render frame, used for recenter)
    XrPosef last_hmd_pose{{0,0,0,1},{0,0,0}};

    // Per-eye stereo swapchains
    static constexpr int k_eye_count = 2;
    struct Eye {
        XrSwapchain swapchain  = XR_NULL_HANDLE;
        uint32_t    width      = 0;
        uint32_t    height     = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<EyeFbo> fbos;   // one FBO per swapchain image
    } eye[k_eye_count];

    // Last projection views (filled by render_frame, used by run loop for xrEndFrame)
    XrCompositionLayerProjectionView last_proj_views[k_eye_count]{};

    // Renderer
    GlesRenderer renderer;

    // Actions
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction act_lstick   = XR_NULL_HANDLE; // Vector2f
    XrAction act_rstick   = XR_NULL_HANDLE;
    XrAction act_lclick   = XR_NULL_HANDLE; // Boolean (thumbstick click)
    XrAction act_rclick   = XR_NULL_HANDLE;
    XrAction act_a        = XR_NULL_HANDLE; // Boolean
    XrAction act_b        = XR_NULL_HANDLE;
    XrAction act_x        = XR_NULL_HANDLE;
    XrAction act_y        = XR_NULL_HANDLE;
    XrAction act_ltrig    = XR_NULL_HANDLE; // Float
    XrAction act_rtrig    = XR_NULL_HANDLE;
    XrAction act_lgrip    = XR_NULL_HANDLE;
    XrAction act_rgrip    = XR_NULL_HANDLE;
    XrAction act_menu     = XR_NULL_HANDLE; // Boolean (left menu)
    XrAction act_lpose    = XR_NULL_HANDLE; // Pose — left controller grip
    XrAction act_rpose    = XR_NULL_HANDLE; // Pose — right controller grip
    XrAction act_laim     = XR_NULL_HANDLE; // Pose — left controller aim (pointing direction)
    XrAction act_raim     = XR_NULL_HANDLE; // Pose — right controller aim (pointing direction)
    XrAction act_haptic_l = XR_NULL_HANDLE; // Vibration output — left controller
    XrAction act_haptic_r = XR_NULL_HANDLE; // Vibration output — right controller
    XrSpace  lhand_space  = XR_NULL_HANDLE;
    XrSpace  rhand_space  = XR_NULL_HANDLE;
    XrSpace  laim_space   = XR_NULL_HANDLE;
    XrSpace  raim_space   = XR_NULL_HANDLE;
};

// ============================================================
// Helpers
// ============================================================
static bool xr_ok(XrResult r, const char* msg) {
    if (r == XR_SUCCESS) return true;
    LOGE("OpenXR error %d: %s", (int)r, msg);
    return false;
}

enum class PanelKind {
    MainMenu,
    Browser,
    Layers,
    Settings,
    Code,
    CtrlMap,
};

struct PanelMetrics {
    int   tex_w = 0;
    int   tex_h = 0;
    float world_w = 0.0f;
    float world_h = 0.0f;
};

static PanelMetrics panel_metrics(PanelKind kind) {
    switch (kind) {
    case PanelKind::MainMenu: return {1024, 1536, 0.80f, 1.20f};
    case PanelKind::Browser:  return {1536, 1536, 1.20f, 1.20f};
    case PanelKind::Layers:   return {1120, 1280, 0.88f, 0.88f * (1280.0f / 1120.0f)};
    case PanelKind::Settings: return {1280, 2176, 1.10f, 1.10f * (2176.0f / 1280.0f)};
    case PanelKind::Code:     return {1536,  768, 1.20f, 0.60f};
    case PanelKind::CtrlMap:  return {1408, 1536, 1.20f, 1.20f * (1536.0f / 1408.0f)};
    }
    return {};
}

static void upload_panel_texture(GLuint& tex_out, int tex_w, int tex_h, const std::vector<uint8_t>& rgba) {
    if ((int)rgba.size() != tex_w * tex_h * 4) return;
    if (!tex_out) {
        glGenTextures(1, &tex_out);
    }
    glBindTexture(GL_TEXTURE_2D, tex_out);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static float pick_default_refresh_rate(const std::vector<float>& rates) {
    if (rates.empty()) return 0.0f;
    auto has_rate = [&](float target) {
        for (float r : rates) {
            if (std::abs(r - target) < 0.5f) return true;
        }
        return false;
    };
    if (has_rate(72.0f)) return 72.0f;
    if (has_rate(90.0f)) return 90.0f;
    return rates.front();
}

static int next_layer_auto_dup_percent(int current) {
    static constexpr int k_cycle[] = { -1, 500, 400, 300, 200, 150, 125, 75, 50, 25, 0 };
    for (int i = 0; i < (int)(sizeof(k_cycle) / sizeof(k_cycle[0])); ++i) {
        if (k_cycle[i] == current) {
            return k_cycle[(i + 1) % (int)(sizeof(k_cycle) / sizeof(k_cycle[0]))];
        }
    }
    return k_cycle[0];
}

static std::string layer_auto_dup_label(int percent) {
    if (percent < 0) return "OFF";
    return std::to_string(percent) + "%";
}

static LayerFilterMode next_layer_filter_mode(LayerFilterMode mode) {
    switch (mode) {
    case LayerFilterMode::ShowAll: return LayerFilterMode::Z;
    case LayerFilterMode::Z:       return LayerFilterMode::Per;
    case LayerFilterMode::Per:     return LayerFilterMode::Hybrid;
    case LayerFilterMode::Hybrid:  return LayerFilterMode::ShowAll;
    }
    return LayerFilterMode::ShowAll;
}

static const char* layer_filter_mode_label(LayerFilterMode mode) {
    switch (mode) {
    case LayerFilterMode::ShowAll: return "SHOW ALL";
    case LayerFilterMode::Z:       return "Z";
    case LayerFilterMode::Per:     return "PER";
    case LayerFilterMode::Hybrid:  return "HYBRID";
    }
    return "SHOW ALL";
}

static bool is_snes_filter_capable_config(const GameConfig& config) {
    return config.game == "snes";
}

static int layer_index_by_id(const GameConfig& config, const char* id) {
    for (int i = 0; i < (int)config.layers.size(); ++i) {
        if (config.layers[i].id == id) return i;
    }
    return -1;
}

static GameConfig subset_config_by_ids(const GameConfig& src,
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

static GameConfig make_snes_config_for_filter(LayerFilterMode mode) {
    const GameConfig base = GameConfig::make_default_snes();
    auto enable_all_by_default = [](GameConfig cfg) {
        for (auto& layer : cfg.layers) {
            layer.default_enabled = true;
            layer.default_ambilight = true;
        }
        return cfg;
    };
    switch (mode) {
    case LayerFilterMode::ShowAll:
        return base;
    case LayerFilterMode::Z:
        return subset_config_by_ids(base, {
            "backdrop", "bg_far_lo", "sprite_p0", "bg_far_hi", "sprite_p1",
            "bg1_lo", "bg0_lo", "sprite_p2", "bg1_hi", "bg0_hi", "sprite_p3",
        });
    case LayerFilterMode::Per:
        return subset_config_by_ids(base, {
            "pc_bg4", "pc_bg3", "pc_bg2", "pc_bg1", "pc_obj",
        });
    case LayerFilterMode::Hybrid:
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
    return base;
}

static uint32_t layer_capture_mask_for_mode(LayerFilterMode mode) {
    switch (mode) {
    case LayerFilterMode::ShowAll: return 0x1Fu;
    case LayerFilterMode::Z:       return 0u;
    case LayerFilterMode::Per:     return 0x1Fu;
    case LayerFilterMode::Hybrid:  return 0x1Fu;
    }
    return 0x1Fu;
}

static GameConfig default_config_for_backend(BackendKind kind, LayerFilterMode snes_mode) {
    switch (kind) {
    case BackendKind::Snes:
        return make_snes_config_for_filter(snes_mode);
    case BackendKind::Genesis:
        return GameConfig::make_default_genesis();
    }
    return GameConfig::make_flat();
}

static uint32_t layer_capture_mask_for_config(const GameConfig& config,
                                              const std::vector<bool>* enabled = nullptr) {
    uint32_t mask = 0u;
    for (int i = 0; i < (int)config.layers.size(); ++i) {
        if (enabled && i < (int)enabled->size() && !(*enabled)[i]) continue;
        const auto& layer = config.layers[i];
        if (layer.extraction_type != ExtractionType::PerLayerCapture) continue;
        if (layer.layer_index < 0 || layer.layer_index >= 32) continue;
        mask |= (1u << layer.layer_index);
    }
    return mask;
}

static std::string normalized_code_string(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '-' || c == '_') continue;
        s += (char)toupper((unsigned char)c);
    }
    return s;
}

static VrState effective_render_state(const VrState& state) {
    VrState render_state = state;
    if (!render_state.immersive_beta_enabled) {
        // Keep the current rendering path unchanged when the beta is off.
        // Future immersive-only knobs should be neutralized here.
        render_state.screen_curve = 0.0f;
        render_state.tilt_x = 0.0f;
        render_state.tilt_y = 0.0f;
        render_state.solid_stack = false;
    } else {
        render_state.screen_curve = std::clamp(render_state.screen_curve, -0.5f, 0.5f);
        if (std::abs(render_state.screen_curve) < 0.001f) {
            render_state.screen_curve = -0.30f;
        }
        render_state.tilt_x = std::clamp(render_state.tilt_x, -0.35f, 0.35f);
        render_state.tilt_y = std::clamp(render_state.tilt_y, -0.35f, 0.35f);
    }
    return render_state;
}

static int baseline_copy_count(const LayerFrame& frame) {
    return frame.copies.empty() ? GlesRenderer::k_max_copies : (int)frame.copies.size();
}

static float baseline_copy_step(const LayerFrame& frame) {
    if (!frame.copies.empty() && frame.copies.back() > 0.0f) {
        return frame.copies.back() / (float)frame.copies.size();
    }
    return GlesRenderer::k_default_copy_step;
}

static void rebuild_copy_offsets(std::vector<float>& copies, int copy_count, float copy_step) {
    if (copy_count <= 0) {
        copies.clear();
        return;
    }
    copies.resize(copy_count);
    for (int i = 0; i < copy_count; ++i) {
        copies[i] = (float)(i + 1) * copy_step;
    }
}

static bool is_snes_discovery_config(const GameConfig& config) {
    return config.game == "snes"
        && config.layers.size() == 16
        && config.layers[11].id == "pc_bg4"
        && config.layers[15].id == "pc_obj";
}

static bool is_snes_hybrid_config(const GameConfig& config) {
    return config.game == "snes"
        && config.layers.size() == 6
        && config.layers[0].id == "pc_bg3"
        && config.layers[1].id == "pc_obj"
        && config.layers[2].id == "pc_bg1"
        && config.layers[3].id == "pc_bg2"
        && config.layers[4].id == "pc_bg4"
        && config.layers[5].id == "backdrop";
}

static std::vector<int> default_layer_order_for_config(const GameConfig& config) {
    const int n = (int)config.layers.size();
    std::vector<int> order;
    order.reserve(n);

    if (is_snes_discovery_config(config)) {
        for (int i = 10; i >= 0; --i) order.push_back(i);   // current z stack, near -> far
        for (int i = 15; i >= 11; --i) order.push_back(i);  // experimental captures, near -> far
        return order;
    }

    if (is_snes_hybrid_config(config)) {
        for (int i = 0; i < n; ++i) order.push_back(i);
        return order;
    }

    for (int i = n - 1; i >= 0; --i) order.push_back(i);
    return order;
}

static void ensure_layer_runtime_state_matches_config(const GameConfig& config,
                                                      std::vector<std::string>& layer_names,
                                                      std::vector<int>& layer_order,
                                                      std::vector<bool>& layer_enabled,
                                                      std::vector<bool>& layer_ambilight) {
    const int n = (int)config.layers.size();

    layer_names.clear();
    layer_names.reserve(n);
    for (const auto& lc : config.layers)
        layer_names.push_back(lc.id.empty() ? "Layer" : lc.id);

    std::vector<int> default_order = default_layer_order_for_config(config);
    if ((int)layer_order.size() != n) {
        std::vector<int> merged;
        merged.reserve(n);
        std::vector<bool> seen(n, false);

        for (int idx : layer_order) {
            if (idx >= 0 && idx < n && !seen[idx]) {
                merged.push_back(idx);
                seen[idx] = true;
            }
        }
        for (int idx : default_order) {
            if (idx >= 0 && idx < n && !seen[idx]) {
                merged.push_back(idx);
                seen[idx] = true;
            }
        }
        layer_order = std::move(merged);
    }

    const std::size_t old_enabled = layer_enabled.size();
    layer_enabled.resize(n, true);
    for (std::size_t i = old_enabled; i < layer_enabled.size(); ++i)
        layer_enabled[i] = config.layers[i].default_enabled;

    const std::size_t old_amb = layer_ambilight.size();
    layer_ambilight.resize(n, true);
    for (std::size_t i = old_amb; i < layer_ambilight.size(); ++i)
        layer_ambilight[i] = config.layers[i].default_ambilight;
}

static void apply_layer_auto_dup_visible(std::vector<LayerFrame>& layer_frames,
                                         int auto_dup_percent) {
    if (auto_dup_percent < 0 || layer_frames.empty()) {
        return;
    }

    const int anchor_count = baseline_copy_count(layer_frames[0]);
    const int far_target = std::clamp((int)std::lround((double)anchor_count * (double)auto_dup_percent / 100.0),
                                      0, 64);
    const int n = (int)layer_frames.size();

    for (int i = 0; i < n; ++i) {
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        const int target_count = std::clamp(
            (int)std::lround(anchor_count + (far_target - anchor_count) * t), 0, 64);
        rebuild_copy_offsets(layer_frames[i].copies, target_count,
                             baseline_copy_step(layer_frames[i]));
    }
}

static void compact_visible_layer_depths(std::vector<LayerFrame>& layer_frames) {
    const int n = (int)layer_frames.size();
    if (n < 2) return;

    float near_d = layer_frames[0].depth_meters;
    float far_d = layer_frames[0].depth_meters;
    for (const auto& lf : layer_frames) {
        near_d = std::min(near_d, lf.depth_meters);
        far_d = std::max(far_d, lf.depth_meters);
    }

    for (int i = 0; i < n; ++i) {
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        layer_frames[i].depth_meters = near_d + (far_d - near_d) * t;
    }
}

static void apply_layer_auto_dup_visible(std::vector<LayerFrame*>& layer_frames,
                                         int auto_dup_percent) {
    if (auto_dup_percent < 0 || layer_frames.empty() || !layer_frames[0]) {
        return;
    }

    const int anchor_count = baseline_copy_count(*layer_frames[0]);
    const int far_target = std::clamp((int)std::lround((double)anchor_count * (double)auto_dup_percent / 100.0),
                                      0, 64);
    const int n = (int)layer_frames.size();

    for (int i = 0; i < n; ++i) {
        if (!layer_frames[i]) continue;
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        const int target_count = std::clamp(
            (int)std::lround(anchor_count + (far_target - anchor_count) * t), 0, 64);
        rebuild_copy_offsets(layer_frames[i]->copies, target_count,
                             baseline_copy_step(*layer_frames[i]));
    }
}

static void compact_visible_layer_depths(std::vector<LayerFrame*>& layer_frames) {
    const int n = (int)layer_frames.size();
    if (n < 2) return;

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

    for (int i = 0; i < n; ++i) {
        if (!layer_frames[i]) continue;
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        layer_frames[i]->depth_meters = near_d + (far_d - near_d) * t;
    }
}

static int anchor_layer_index(const GameConfig& config, const std::vector<int>& layer_order) {
    if (config.layers.empty()) return -1;
    if (!layer_order.empty()) {
        const int orig = layer_order[0];
        if (orig >= 0 && orig < (int)config.layers.size()) return orig;
    }
    return 0;
}

static int current_base_copy_count(const GameConfig& config, const std::vector<int>& layer_order) {
    if (config.layers.empty()) return 0;
    const int idx = anchor_layer_index(config, layer_order);
    if (idx < 0) return 0;
    const auto& layer = config.layers[idx];
    return layer.copies.empty() ? GlesRenderer::k_max_copies : (int)layer.copies.size();
}

static float current_copy_step(const LayerConfig& layer) {
    if (!layer.copies.empty() && layer.copies.back() > 0.0f) {
        return layer.copies.back() / (float)layer.copies.size();
    }
    return GlesRenderer::k_default_copy_step;
}

static void set_all_layer_copy_counts(GameConfig& config, int copy_count) {
    const int clamped = std::clamp(copy_count, 1, 32);
    for (auto& layer : config.layers) {
        rebuild_copy_offsets(layer.copies, clamped, current_copy_step(layer));
    }
}

static std::string copy_count_status_text(const GameConfig& config,
                                          const std::vector<int>& layer_order,
                                          int auto_dup_percent) {
    const int base = current_base_copy_count(config, layer_order);
    if (auto_dup_percent < 0) {
        return "Copies: " + std::to_string(base) + "  AutoDup: OFF";
    }
    const int far = std::clamp((int)std::lround((double)base * (double)auto_dup_percent / 100.0), 0, 64);
    return "Copies: " + std::to_string(base) + "  AutoDup: "
        + std::to_string(auto_dup_percent) + "% (far " + std::to_string(far) + ")";
}

static bool try_decode_snes_state_code(const std::string& raw,
                                       VrState& vs,
                                       LayerFilterMode& mode_out,
                                       GameConfig& cfg_out,
                                       std::vector<int>& order_out,
                                       std::vector<bool>& ena_out,
                                       std::vector<bool>& amb_out) {
    const std::string s = normalized_code_string(raw);
    if (s.empty()) return false;

    struct Candidate {
        LayerFilterMode mode;
        int len;
        bool legacy;
    };
    static const Candidate candidates[] = {
        {LayerFilterMode::ShowAll, 38, false},
        {LayerFilterMode::Z,       28, false},
        {LayerFilterMode::Per,     16, false},
        {LayerFilterMode::Hybrid,  18, false},
        {LayerFilterMode::Z,       27, true},
        {LayerFilterMode::ShowAll, 37, true},
    };

    for (const Candidate& c : candidates) {
        if ((int)s.size() != c.len) continue;
        GameConfig cfg = make_snes_config_for_filter(c.mode);
        std::vector<int> order;
        std::vector<bool> ena;
        std::vector<bool> amb;
        int decoded_mode = -1;
        const bool ok = vr_state_decode(
            s, vs, &cfg, &order, &ena, &amb, c.legacy ? nullptr : &decoded_mode);
        if (!ok) continue;
        if (!c.legacy && decoded_mode != (int)c.mode) continue;
        mode_out = c.mode;
        cfg_out = std::move(cfg);
        order_out = std::move(order);
        ena_out = std::move(ena);
        amb_out = std::move(amb);
        return true;
    }

    return false;
}

static bool sniff_settings_layer_mode(const std::string& path,
                                      LayerFilterMode& mode_out,
                                      int& num_layers_out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    mode_out = LayerFilterMode::Hybrid;
    num_layers_out = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if (strcmp(key, "layer_filter_mode") == 0) {
            const int mode = atoi(val);
            if (mode >= 0 && mode <= (int)LayerFilterMode::Hybrid) {
                mode_out = (LayerFilterMode)mode;
            }
        } else if (strcmp(key, "num_layers") == 0) {
            num_layers_out = atoi(val);
        }
    }
    fclose(f);

    if (num_layers_out == 11) mode_out = LayerFilterMode::Z;
    else if (num_layers_out == 16) mode_out = LayerFilterMode::ShowAll;

    return true;
}

// ============================================================
// Public API
// ============================================================
OpenXrShell::~OpenXrShell() { stop(nullptr); }

void OpenXrShell::set_frame_provider(FrameProvider p) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_frame_provider = std::move(p);
}
void OpenXrShell::set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_status = s;
}
std::string OpenXrShell::status() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::string out = m_status;
    if (m_impl && m_impl->session != XR_NULL_HANDLE) {
        char diag[384];
        const float refresh = m_active_refresh_rate > 0.0f
            ? m_active_refresh_rate
            : pick_default_refresh_rate(m_impl->available_rates);
        const float budget_ms = refresh > 0.0f ? (1000.0f / refresh) : 0.0f;
        const bool over_budget = budget_ms > 0.0f && m_avg_render_ms > budget_ms;
        std::snprintf(diag, sizeof(diag),
            "\n\nRuntime\n"
            "refresh %.0f Hz  eye %ux%u\n"
            "render %.2f ms avg  %.2f ms last  %.2f ms max\n"
            "budget %.2f ms  %s",
            refresh,
            m_impl->eye[0].width,
            m_impl->eye[0].height,
            m_avg_render_ms,
            m_last_render_ms,
            m_max_render_ms,
            budget_ms,
            over_budget ? "over budget" : "within budget");
        out += diag;
    }
    return out;
}
void OpenXrShell::randomize()      { m_randomize_pending = true; }
void OpenXrShell::load_preset(int i) { m_preset_load_pending = i; }
void OpenXrShell::save_preset(int i) { m_preset_save_pending = i; }

std::string OpenXrShell::vr_state_summary() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "beta=%s\n"
        "γ=%.2f  con=%.2f  sat=%.2f\n"
        "3D=%s  depth=%s  up=%s\n"
        "pass=%s  amb=%s  round=%.2f  curve=%.2f",
        m_vr_state.immersive_beta_enabled ? "on" : "off",
        m_vr_state.gamma, m_vr_state.contrast, m_vr_state.saturation,
        m_vr_state.layers_3d?"on":"off",
        m_vr_state.depthmap?"on":"off",
        m_vr_state.upscale?"on":"off",
        m_vr_state.shadows?"on":"off",
        m_vr_state.ambilight?"on":"off",
        m_vr_state.roundness, m_vr_state.screen_curve);
    return buf;
}

std::string OpenXrShell::get_state_code() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    const int filter_mode = is_snes_filter_capable_config(m_config) ? (int)m_layer_filter_mode : -1;
    return vr_state_encode(m_vr_state, &m_config, &m_layer_order, &m_layer_enabled, &m_layer_ambilight,
                           filter_mode);
}

bool OpenXrShell::apply_state_code(const std::string& code) {
    VrState test = {};
    if (is_snes_filter_capable_config(m_config)) {
        LayerFilterMode test_mode = LayerFilterMode::ShowAll;
        GameConfig test_cfg;
        std::vector<int> test_order;
        std::vector<bool> test_enabled;
        std::vector<bool> test_ambilight;
        if (!try_decode_snes_state_code(code, test, test_mode, test_cfg, test_order, test_enabled, test_ambilight))
            return false;
    } else if (!vr_state_decode(code, test)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pending_code = code;
    }
    m_apply_code_pending.store(true);
    return true;
}

void OpenXrShell::sync_layer_capture_mask() {
    LayerCaptureMaskCtrl mask_fn;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        mask_fn = m_layer_capture_mask_ctrl;
    }
    if (mask_fn) {
        const uint32_t mask = is_snes_filter_capable_config(m_config)
            ? layer_capture_mask_for_mode(m_layer_filter_mode)
            : layer_capture_mask_for_config(m_config, &m_layer_enabled);
        mask_fn(mask);
    }
}

void OpenXrShell::apply_layer_filter_mode(LayerFilterMode mode, bool restore_saved_state) {
    m_layer_filter_mode = mode;
    if (restore_saved_state && m_saved_layer_mode_state.valid && m_saved_layer_mode_state.mode == mode) {
        m_config = m_saved_layer_mode_state.config;
        m_layer_order = m_saved_layer_mode_state.order;
        m_layer_enabled = m_saved_layer_mode_state.enabled;
        m_layer_ambilight = m_saved_layer_mode_state.ambilight;
    } else {
        m_config = default_config_for_backend(m_current_backend_kind, mode);
        m_layer_order.clear();
        m_layer_enabled.clear();
        m_layer_ambilight.clear();
    }
    ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
    sync_layer_capture_mask();
    m_layer_panel_dirty = true;
}

bool OpenXrShell::start(JavaVM* vm, JNIEnv* env, jobject activity, std::string& status_out) {
    stop(env);
    if (!vm || !env || !activity) {
        status_out = "OpenXR start failed: invalid Android context.";
        set_status(status_out);
        return false;
    }
    m_vm              = vm;
    m_activity_global = env->NewGlobalRef(activity);
    m_stop_requested  = false;
    m_running         = true;
    m_active_refresh_rate = 0.0f;
    m_last_render_ms = 0.0f;
    m_avg_render_ms = 0.0f;
    m_max_render_ms = 0.0f;
    m_render_sample_count = 0;

    m_layer_filter_mode = LayerFilterMode::Hybrid;
    m_saved_layer_mode_state.valid = false;
    m_config      = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
    m_presets     = make_default_vr_presets();
    m_button_map  = default_button_map_for_backend(m_current_backend_kind);
    m_load_global_pending = true; // load global settings on first XR frame
    m_open_menu_on_startup = true; // show main menu on startup
    ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
    sync_layer_capture_mask();

    set_status("Starting OpenXR shell...");
    m_thread = std::thread([this]() { run(); });
    status_out = status();
    return true;
}

void OpenXrShell::stop(JNIEnv* env) {
    m_stop_requested = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;

    if (m_activity_global) {
        JNIEnv* del_env = env;
        bool detach = false;
        if (!del_env && m_vm &&
            m_vm->GetEnv(reinterpret_cast<void**>(&del_env), JNI_VERSION_1_6) != JNI_OK) {
            if (m_vm->AttachCurrentThread(&del_env, nullptr) == JNI_OK) detach = true;
        }
        if (del_env) del_env->DeleteGlobalRef(m_activity_global);
        if (detach && m_vm) m_vm->DetachCurrentThread();
        m_activity_global = nullptr;
    }
}

// ============================================================
// Init chain
// ============================================================
bool OpenXrShell::initialize_loader() {
    if (!m_vm || !m_activity_global) {
        set_status("Loader init failed: no VM/activity.");
        return false;
    }
    if (!m_impl) m_impl = new Impl();

    if (!xr_ok(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_impl->pfn_init_loader)), "get xrInitializeLoaderKHR")
        || !m_impl->pfn_init_loader) {
        set_status("Loader init failed: xrInitializeLoaderKHR unavailable.");
        return false;
    }
    XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM      = m_vm;
    info.applicationContext = m_activity_global;
    if (!xr_ok(m_impl->pfn_init_loader(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)),
            "xrInitializeLoaderKHR")) {
        set_status("Loader init failed.");
        return false;
    }
    return true;
}

bool OpenXrShell::create_instance() {
    uint32_t ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());

    auto has = [&](const char* name) {
        return std::any_of(exts.begin(), exts.end(), [&](const XrExtensionProperties& e) {
            return std::string_view(e.extensionName) == name;
        });
    };
    if (!has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) ||
        !has(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        set_status("OpenXR: missing required extensions.");
        return false;
    }
    m_impl->has_refresh_ext = has("XR_FB_display_refresh_rate");
    m_impl->has_passthrough_ext = has(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    m_impl->has_alpha_blend_ext = has(XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME);

    std::vector<const char*> enabled_exts = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    if (m_impl->has_refresh_ext)
        enabled_exts.push_back("XR_FB_display_refresh_rate");
    if (m_impl->has_passthrough_ext)
        enabled_exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (m_impl->has_alpha_blend_ext)
        enabled_exts.push_back(XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM       = m_vm;
    android_info.applicationActivity = m_activity_global;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_info;
    std::strncpy(ci.applicationInfo.applicationName, "QuestRetroDepth", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "QuestRetroDepth", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion    = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount         = (uint32_t)enabled_exts.size();
    ci.enabledExtensionNames         = enabled_exts.data();

    if (!xr_ok(xrCreateInstance(&ci, &m_impl->instance), "xrCreateInstance")) {
        set_status("OpenXR instance creation failed.");
        return false;
    }
    if (!xr_ok(xrGetInstanceProcAddr(m_impl->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_impl->pfn_get_gl_reqs)), "get GL reqs fn")
        || !m_impl->pfn_get_gl_reqs) {
        set_status("OpenXR: xrGetOpenGLESGraphicsRequirementsKHR unavailable.");
        return false;
    }
    if (m_impl->has_refresh_ext) {
        xrGetInstanceProcAddr(m_impl->instance, "xrEnumerateDisplayRefreshRatesFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_impl->pfn_enum_refresh));
        xrGetInstanceProcAddr(m_impl->instance, "xrRequestDisplayRefreshRateFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_impl->pfn_set_refresh));
    }
    return true;
}

bool OpenXrShell::create_system() {
    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_ok(xrGetSystem(m_impl->instance, &info, &m_impl->system_id), "xrGetSystem")) {
        set_status("OpenXR system unavailable.");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateEnvironmentBlendModes(m_impl->instance, m_impl->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    if (n > 0) {
        std::vector<XrEnvironmentBlendMode> modes(n);
        xrEnumerateEnvironmentBlendModes(m_impl->instance, m_impl->system_id,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data());
        m_impl->supports_alpha_blend_mode =
            std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != modes.end();
        auto it = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        m_impl->blend_mode = (it != modes.end()) ? *it : modes.front();
    }
    if (m_impl->has_passthrough_ext) {
        XrSystemPassthroughPropertiesFB passthrough_props{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        props.next = &passthrough_props;
        if (xrGetSystemProperties(m_impl->instance, m_impl->system_id, &props) == XR_SUCCESS) {
            m_impl->supports_passthrough = passthrough_props.supportsPassthrough == XR_TRUE;
        }
    }
    return true;
}

bool OpenXrShell::create_graphics_context() {
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!xr_ok(m_impl->pfn_get_gl_reqs(m_impl->instance, m_impl->system_id, &req), "GL reqs")) {
        set_status("OpenXR GL requirements query failed.");
        return false;
    }
    m_impl->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_impl->egl_display == EGL_NO_DISPLAY) { set_status("EGL: no display."); return false; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_impl->egl_display, &major, &minor)) { set_status("EGL init failed."); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, 0x00000040, // EGL_OPENGL_ES3_BIT_KHR
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_NONE
    };
    EGLint n_cfg = 0;
    if (!eglChooseConfig(m_impl->egl_display, cfg_attribs, &m_impl->egl_config, 1, &n_cfg) || n_cfg == 0) {
        set_status("EGL config failed."); return false;
    }
    const EGLint pbuf[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    m_impl->egl_surface = eglCreatePbufferSurface(m_impl->egl_display, m_impl->egl_config, pbuf);
    if (m_impl->egl_surface == EGL_NO_SURFACE) { set_status("EGL pbuffer failed."); return false; }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    m_impl->egl_context = eglCreateContext(m_impl->egl_display, m_impl->egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (m_impl->egl_context == EGL_NO_CONTEXT) { set_status("EGL context failed."); return false; }
    if (!eglMakeCurrent(m_impl->egl_display, m_impl->egl_surface, m_impl->egl_surface, m_impl->egl_context)) {
        set_status("EGL make current failed."); return false;
    }
    return true;
}

bool OpenXrShell::create_session() {
    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = m_impl->egl_display;
    binding.config  = m_impl->egl_config;
    binding.context = m_impl->egl_context;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next     = &binding;
    sci.systemId = m_impl->system_id;
    if (!xr_ok(xrCreateSession(m_impl->instance, &sci, &m_impl->session), "xrCreateSession")) {
        set_status("OpenXR session creation failed.");
        return false;
    }
    init_passthrough();
    return true;
}

bool OpenXrShell::init_passthrough() {
    if (!m_impl || !m_impl->has_passthrough_ext || !m_impl->supports_passthrough)
        return false;

    auto get_proc = [&](const char* name, auto& out) {
        return xrGetInstanceProcAddr(
                   m_impl->instance, name,
                   reinterpret_cast<PFN_xrVoidFunction*>(&out)) == XR_SUCCESS && out;
    };

    if (!get_proc("xrCreatePassthroughFB", m_impl->pfn_create_passthrough) ||
        !get_proc("xrDestroyPassthroughFB", m_impl->pfn_destroy_passthrough) ||
        !get_proc("xrPassthroughStartFB", m_impl->pfn_start_passthrough) ||
        !get_proc("xrPassthroughPauseFB", m_impl->pfn_pause_passthrough) ||
        !get_proc("xrCreatePassthroughLayerFB", m_impl->pfn_create_passthrough_layer) ||
        !get_proc("xrDestroyPassthroughLayerFB", m_impl->pfn_destroy_passthrough_layer) ||
        !get_proc("xrPassthroughLayerPauseFB", m_impl->pfn_pause_passthrough_layer) ||
        !get_proc("xrPassthroughLayerResumeFB", m_impl->pfn_resume_passthrough_layer)) {
        LOGE("Passthrough extension present but required functions are unavailable.");
        return false;
    }

    XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    if (!xr_ok(m_impl->pfn_create_passthrough(m_impl->session, &pci, &m_impl->passthrough),
               "xrCreatePassthroughFB")) {
        return false;
    }

    XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    lci.passthrough = m_impl->passthrough;
    lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    if (!xr_ok(m_impl->pfn_create_passthrough_layer(m_impl->session, &lci, &m_impl->passthrough_layer),
               "xrCreatePassthroughLayerFB")) {
        m_impl->pfn_destroy_passthrough(m_impl->passthrough);
        m_impl->passthrough = XR_NULL_HANDLE;
        return false;
    }

    m_impl->pfn_pause_passthrough_layer(m_impl->passthrough_layer);
    m_impl->pfn_pause_passthrough(m_impl->passthrough);
    m_impl->passthrough_running = false;
    m_impl->passthrough_layer_running = false;
    return true;
}

void OpenXrShell::sync_passthrough_state() {
    if (!m_impl) return;
    const bool want = m_vr_state.shadows;
    const bool available =
        m_impl->passthrough != XR_NULL_HANDLE &&
        m_impl->passthrough_layer != XR_NULL_HANDLE &&
        m_impl->pfn_start_passthrough &&
        m_impl->pfn_pause_passthrough &&
        m_impl->pfn_resume_passthrough_layer &&
        m_impl->pfn_pause_passthrough_layer;

    if (!available) return;

    if (want) {
        if (!m_impl->passthrough_running) {
            const XrResult r = m_impl->pfn_start_passthrough(m_impl->passthrough);
            if (r == XR_SUCCESS) m_impl->passthrough_running = true;
            else LOGE("xrPassthroughStartFB failed: %d", (int)r);
        }
        if (!m_impl->passthrough_layer_running) {
            const XrResult r = m_impl->pfn_resume_passthrough_layer(m_impl->passthrough_layer);
            if (r == XR_SUCCESS) m_impl->passthrough_layer_running = true;
            else LOGE("xrPassthroughLayerResumeFB failed: %d", (int)r);
        }
    } else {
        if (m_impl->passthrough_layer_running) {
            m_impl->pfn_pause_passthrough_layer(m_impl->passthrough_layer);
            m_impl->passthrough_layer_running = false;
        }
        if (m_impl->passthrough_running) {
            m_impl->pfn_pause_passthrough(m_impl->passthrough);
            m_impl->passthrough_running = false;
        }
    }
}

bool OpenXrShell::passthrough_active() const {
    return m_impl &&
           m_vr_state.shadows &&
           m_impl->passthrough_layer != XR_NULL_HANDLE &&
           m_impl->passthrough_layer_running;
}

bool OpenXrShell::create_reference_space() {
    XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType                  = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w  = 1.0f;
    if (!xr_ok(xrCreateReferenceSpace(m_impl->session, &rsi, &m_impl->app_space), "xrCreateReferenceSpace")) {
        set_status("OpenXR reference space failed.");
        return false;
    }
    // VIEW space — forward direction of HMD (used to locate HMD pose in world).
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(m_impl->session, &rsi, &m_impl->view_space); // non-fatal if unsupported

    // Static LOCAL space at identity — never replaced, used as world-anchor for recenter.
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.position    = {0,0,0};
    rsi.poseInReferenceSpace.orientation = {0,0,0,1};
    xrCreateReferenceSpace(m_impl->session, &rsi, &m_impl->local_space); // non-fatal if unsupported
    return true;
}

bool OpenXrShell::create_swapchains() {
    // Enumerate stereo view configs
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(m_impl->instance, m_impl->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    if (view_count < 2) { set_status("OpenXR: stereo views unavailable."); return false; }
    std::vector<XrViewConfigurationView> vcv(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(m_impl->instance, m_impl->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, vcv.data());

    // Choose swapchain format
    uint32_t fmt_count = 0;
    xrEnumerateSwapchainFormats(m_impl->session, 0, &fmt_count, nullptr);
    std::vector<int64_t> fmts(fmt_count);
    xrEnumerateSwapchainFormats(m_impl->session, fmt_count, &fmt_count, fmts.data());
    int64_t chosen_fmt = fmts[0];
    for (int64_t preferred : { (int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8 }) {
        if (std::find(fmts.begin(), fmts.end(), preferred) != fmts.end()) {
            chosen_fmt = preferred;
            break;
        }
    }

    for (int eye = 0; eye < Impl::k_eye_count; ++eye) {
        auto& e = m_impl->eye[eye];
        e.width  = scaled_eye_extent(vcv[eye].recommendedImageRectWidth,  m_vr_state.vr_resolution_scale);
        e.height = scaled_eye_extent(vcv[eye].recommendedImageRectHeight, m_vr_state.vr_resolution_scale);

        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format     = chosen_fmt;
        sci.sampleCount = 1;
        sci.width      = e.width;
        sci.height     = e.height;
        sci.faceCount  = 1;
        sci.arraySize  = 1;
        sci.mipCount   = 1;
        if (!xr_ok(xrCreateSwapchain(m_impl->session, &sci, &e.swapchain), "xrCreateSwapchain")) {
            set_status("Swapchain creation failed.");
            return false;
        }
        uint32_t img_count = 0;
        xrEnumerateSwapchainImages(e.swapchain, 0, &img_count, nullptr);
        e.images.assign(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(e.swapchain, img_count, &img_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()));

        // Create one FBO per swapchain image
        e.fbos.resize(img_count);
        for (uint32_t k = 0; k < img_count; ++k) {
            e.fbos[k] = m_impl->renderer.make_eye_fbo(
                e.images[k].image, (int)e.width, (int)e.height);
        }
    }
    return true;
}

void OpenXrShell::destroy_swapchains() {
    if (!m_impl) return;
    for (int eye = 0; eye < Impl::k_eye_count; ++eye) {
        auto& e = m_impl->eye[eye];
        for (auto& fbo : e.fbos) m_impl->renderer.destroy_eye_fbo(fbo);
        e.fbos.clear();
        e.images.clear();
        e.width = 0;
        e.height = 0;
        if (e.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(e.swapchain);
            e.swapchain = XR_NULL_HANDLE;
        }
    }
}

bool OpenXrShell::init_actions() {
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(asi.actionSetName,          "retrodepth", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(asi.localizedActionSetName, "RetroDepth", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    asi.priority = 0;
    if (!xr_ok(xrCreateActionSet(m_impl->instance, &asi, &m_impl->action_set), "xrCreateActionSet"))
        return false;

    // Helper to create one action
    auto make_action = [&](XrAction& out, const char* name, XrActionType type) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        std::strncpy(aci.actionName,          name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(aci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        aci.actionType     = type;
        aci.countSubactionPaths = 0;
        return xrCreateAction(m_impl->action_set, &aci, &out) == XR_SUCCESS;
    };

    if (!make_action(m_impl->act_lstick, "lstick",  XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
    if (!make_action(m_impl->act_rstick, "rstick",  XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
    if (!make_action(m_impl->act_lclick, "lclick",  XR_ACTION_TYPE_BOOLEAN_INPUT))  return false;
    if (!make_action(m_impl->act_rclick, "rclick",  XR_ACTION_TYPE_BOOLEAN_INPUT))  return false;
    if (!make_action(m_impl->act_a,      "btn_a",   XR_ACTION_TYPE_BOOLEAN_INPUT))  return false;
    if (!make_action(m_impl->act_b,      "btn_b",   XR_ACTION_TYPE_BOOLEAN_INPUT))  return false;
    if (!make_action(m_impl->act_x,      "btn_x",   XR_ACTION_TYPE_BOOLEAN_INPUT))  return false;
    if (!make_action(m_impl->act_y,      "btn_y",   XR_ACTION_TYPE_BOOLEAN_INPUT))  return false;
    if (!make_action(m_impl->act_ltrig,  "ltrig",   XR_ACTION_TYPE_FLOAT_INPUT))    return false;
    if (!make_action(m_impl->act_rtrig,  "rtrig",   XR_ACTION_TYPE_FLOAT_INPUT))    return false;
    if (!make_action(m_impl->act_lgrip,  "lgrip",   XR_ACTION_TYPE_FLOAT_INPUT))    return false;
    if (!make_action(m_impl->act_rgrip,  "rgrip",   XR_ACTION_TYPE_FLOAT_INPUT))    return false;
    if (!make_action(m_impl->act_menu,     "menu",      XR_ACTION_TYPE_BOOLEAN_INPUT))    return false;
    if (!make_action(m_impl->act_lpose,   "lpose",     XR_ACTION_TYPE_POSE_INPUT))       return false;
    if (!make_action(m_impl->act_rpose,   "rpose",     XR_ACTION_TYPE_POSE_INPUT))       return false;
    if (!make_action(m_impl->act_laim,    "laim",      XR_ACTION_TYPE_POSE_INPUT))       return false;
    if (!make_action(m_impl->act_raim,    "raim",      XR_ACTION_TYPE_POSE_INPUT))       return false;
    if (!make_action(m_impl->act_haptic_l,"haptic_l",  XR_ACTION_TYPE_VIBRATION_OUTPUT)) return false;
    if (!make_action(m_impl->act_haptic_r,"haptic_r",  XR_ACTION_TYPE_VIBRATION_OUTPUT)) return false;

    // Oculus Touch bindings
    XrPath oculus_path;
    xrStringToPath(m_impl->instance,
        "/interaction_profiles/oculus/touch_controller", &oculus_path);

    auto path = [&](const char* s) {
        XrPath p; xrStringToPath(m_impl->instance, s, &p); return p;
    };

    XrActionSuggestedBinding bindings[] = {
        {m_impl->act_lstick, path("/user/hand/left/input/thumbstick")},
        {m_impl->act_rstick, path("/user/hand/right/input/thumbstick")},
        {m_impl->act_lclick, path("/user/hand/left/input/thumbstick/click")},
        {m_impl->act_rclick, path("/user/hand/right/input/thumbstick/click")},
        {m_impl->act_a,      path("/user/hand/right/input/a/click")},
        {m_impl->act_b,      path("/user/hand/right/input/b/click")},
        {m_impl->act_x,      path("/user/hand/left/input/x/click")},
        {m_impl->act_y,      path("/user/hand/left/input/y/click")},
        {m_impl->act_ltrig,  path("/user/hand/left/input/trigger/value")},
        {m_impl->act_rtrig,  path("/user/hand/right/input/trigger/value")},
        {m_impl->act_lgrip,  path("/user/hand/left/input/squeeze/value")},
        {m_impl->act_rgrip,  path("/user/hand/right/input/squeeze/value")},
        {m_impl->act_menu,   path("/user/hand/left/input/menu/click")},
        {m_impl->act_lpose,  path("/user/hand/left/input/grip/pose")},
        {m_impl->act_rpose,  path("/user/hand/right/input/grip/pose")},
        {m_impl->act_laim,     path("/user/hand/left/input/aim/pose")},
        {m_impl->act_raim,     path("/user/hand/right/input/aim/pose")},
        {m_impl->act_haptic_l, path("/user/hand/left/output/haptic")},
        {m_impl->act_haptic_r, path("/user/hand/right/output/haptic")},
    };

    XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile     = oculus_path;
    suggest.suggestedBindings      = bindings;
    suggest.countSuggestedBindings = (uint32_t)std::size(bindings);
    xrSuggestInteractionProfileBindings(m_impl->instance, &suggest);

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.actionSets      = &m_impl->action_set;
    attach.countActionSets = 1;
    xrAttachSessionActionSets(m_impl->session, &attach);

    // Create action spaces for controller grip poses (used in edit mode).
    XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asci.poseInActionSpace.orientation.w = 1.0f;
    asci.action = m_impl->act_lpose;
    xrCreateActionSpace(m_impl->session, &asci, &m_impl->lhand_space);
    asci.action = m_impl->act_rpose;
    xrCreateActionSpace(m_impl->session, &asci, &m_impl->rhand_space);
    asci.action = m_impl->act_laim;
    xrCreateActionSpace(m_impl->session, &asci, &m_impl->laim_space);
    asci.action = m_impl->act_raim;
    xrCreateActionSpace(m_impl->session, &asci, &m_impl->raim_space);
    return true;
}

bool OpenXrShell::init_renderer() {
    std::string err;
    if (!m_impl->renderer.init(err)) {
        set_status("GLES renderer init failed: " + err);
        return false;
    }
    return true;
}

// ============================================================
// Run loop
// ============================================================
void OpenXrShell::run() {
    LOGI("run: start");
#define INIT_STEP(fn) (LOGI("run: " #fn), fn())
    bool ok = INIT_STEP(initialize_loader) &&
              INIT_STEP(create_instance)   &&
              INIT_STEP(create_system)     &&
              INIT_STEP(create_graphics_context) &&
              INIT_STEP(create_session)    &&
              INIT_STEP(create_reference_space) &&
              INIT_STEP(init_actions)      &&
              INIT_STEP(init_renderer);
#undef INIT_STEP
    LOGI("run: init chain done, ok=%d, status=%s", (int)ok, m_status.c_str());

    if (!ok) { shutdown(); m_running = false; return; }

    set_status("OpenXR shell running.");
    LOGI("run: entering render loop");

    bool exit = false;
    bool first_frame_since_session = true;
    while (!m_stop_requested && !exit) {
        poll_events(exit);
        if (exit) break;
        if (!m_impl || !m_impl->session_running) {
            static int debug_loop_count = 0;
            if (++debug_loop_count % 100 == 0) {
                LOGI("run: waiting for session (state=%d, running=%d)", 
                     (int)(m_impl ? m_impl->session_state : -1), 
                     (int)(m_impl && m_impl->session_running));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Open main menu on first frame after session starts
        if (first_frame_since_session && m_open_menu_on_startup.load()) {
            m_open_menu_on_startup = false;
            open_rom_menu();
            // Freeze emulator while menu is open
            EmuFreezeCtrl freeze_fn;
            { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
            if (freeze_fn) freeze_fn(true);
        }
        first_frame_since_session = false;
        if (!m_impl || !m_impl->session_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Lazy swapchain creation (needs session in READY state first)
        if (m_impl->eye[0].swapchain == XR_NULL_HANDLE) {
            LOGI("run: creating swapchains (first time after session start)");
            if (!create_swapchains()) { set_status("Swapchain init failed."); break; }
            LOGI("run: swapchains created OK");
        }

        XrFrameWaitInfo  fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState     fs{XR_TYPE_FRAME_STATE};
        XrResult wait_res = xrWaitFrame(m_impl->session, &fwi, &fs);
        if (wait_res != XR_SUCCESS) { 
            LOGE("run: xrWaitFrame failed, result=%d", (int)wait_res);
            set_status("xrWaitFrame failed."); break; 
        }

        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        XrResult begin_res = xrBeginFrame(m_impl->session, &fbi);
        if (begin_res != XR_SUCCESS) {
            LOGE("run: xrBeginFrame failed, result=%d", (int)begin_res);
            set_status("xrBeginFrame failed."); break; 
        }

        apply_pending_vr_changes();
        m_frame_predicted_time = fs.predictedDisplayTime; // used by poll_actions for controller locate
        if (m_impl->action_set != XR_NULL_HANDLE) poll_actions();

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerPassthroughFB passthrough_layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerAlphaBlendFB alpha_blend{XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB};
        XrCompositionLayerProjectionView proj_views[2]{};

        if (fs.shouldRender) {
            render_frame(fs.predictedDisplayTime);

            for (int eye = 0; eye < 2; ++eye) {
                proj_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            }
            // The render_frame fills the actual view poses; we set them below after locateViews.
            // (They were stored in render_frame — see below.)
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime          = fs.predictedDisplayTime;
        const bool pt_active = passthrough_active();
        fei.environmentBlendMode = (pt_active && m_impl->supports_alpha_blend_mode)
            ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
            : m_impl->blend_mode;
        if (pt_active) {
            passthrough_layer.space = XR_NULL_HANDLE;
            passthrough_layer.layerHandle = m_impl->passthrough_layer;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthrough_layer));
        }
        // Build projection layer using the view data that render_frame stored
        if (fs.shouldRender &&
            m_impl->eye[0].swapchain != XR_NULL_HANDLE) {
            // Fill projection views from stored data in the Impl
            // (We stash them during render_frame via a small array on the stack — pass by Impl.)
            // For simplicity, the locateViews + swapchain submit is done entirely inside render_frame,
            // and we reconstruct the layer here.
            proj_layer.space      = m_impl->app_space;
            proj_layer.viewCount  = 2;
            proj_layer.views      = proj_views;
            // proj_views are filled inside render_frame; we expose them via m_impl->last_proj_views.
            for (int e = 0; e < 2; ++e)
                proj_views[e] = m_impl->last_proj_views[e]; // see Impl extension below
            if (pt_active) {
                proj_layer.layerFlags =
                    XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                if (m_impl->has_alpha_blend_ext) {
                    alpha_blend.srcFactorColor = XR_BLEND_FACTOR_SRC_ALPHA_FB;
                    alpha_blend.dstFactorColor = XR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA_FB;
                    alpha_blend.srcFactorAlpha = XR_BLEND_FACTOR_ONE_FB;
                    alpha_blend.dstFactorAlpha = XR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA_FB;
                    proj_layer.next = &alpha_blend;
                }
            }
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj_layer));
        }
        fei.layerCount = (uint32_t)layers.size();
        fei.layers     = layers.data();
        if (xrEndFrame(m_impl->session, &fei) != XR_SUCCESS) { set_status("xrEndFrame failed."); break; }
    }

    shutdown();
    m_running = false;
}

// ============================================================
// poll_events
// ============================================================
void OpenXrShell::poll_events(bool& exit) {
    if (!m_impl || m_impl->instance == XR_NULL_HANDLE) { exit = true; return; }
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_impl->instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            LOGI("poll_events: session state changed to %d (running=%d)", (int)sc->state, (int)m_impl->session_running);
            m_impl->session_state = sc->state;
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult begin_res = xrBeginSession(m_impl->session, &sbi);
                LOGI("poll_events: xrBeginSession result=%d", (int)begin_res);
                if (begin_res == XR_SUCCESS) {
                    m_impl->session_running = true;
                    // Enumerate supported display refresh rates
                    if (m_impl->has_refresh_ext && m_impl->pfn_enum_refresh) {
                        uint32_t cnt = 0;
                        m_impl->pfn_enum_refresh(m_impl->session, 0, &cnt, nullptr);
                        if (cnt > 0) {
                            m_impl->available_rates.resize(cnt);
                            m_impl->pfn_enum_refresh(m_impl->session, cnt, &cnt, m_impl->available_rates.data());
                            std::sort(m_impl->available_rates.begin(), m_impl->available_rates.end());
                            std::string rates;
                            for (float r : m_impl->available_rates) {
                                if (!rates.empty()) rates += ", ";
                                char buf[16];
                                std::snprintf(buf, sizeof(buf), "%.0f", r);
                                rates += buf;
                            }
                            LOGI("Display refresh rates: [%s]", rates.c_str());
                        }
                    }
                    if (m_desired_refresh_rate <= 0.0f) {
                        m_desired_refresh_rate = pick_default_refresh_rate(m_impl->available_rates);
                    }
                    m_apply_refresh_pending = true; // apply desired rate once session is live
                    set_status("OpenXR session running.");
                }
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                if (m_impl->session_running) {
                    xrEndSession(m_impl->session);
                    m_impl->session_running = false;
                }
            } else if (sc->state == XR_SESSION_STATE_EXITING ||
                       sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                exit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            exit = true; break;
        default: break;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (m_stop_requested && m_impl->session_running)
        xrRequestExitSession(m_impl->session);
}

// ============================================================
// recenter_to_hmd — snap canvas to current gaze direction
// ============================================================
void OpenXrShell::recenter_to_hmd() {
    // Locate the VIEW space (= HMD orientation) in the static local_space.
    // local_space is a plain LOCAL space created once at identity and never modified,
    // so this gives a true world-space HMD pose regardless of what app_space is doing.
    XrPosef hmd_world{{0,0,0,1},{0,0,0}};
    if (m_impl->view_space != XR_NULL_HANDLE && m_impl->local_space != XR_NULL_HANDLE) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        XrTime t = (XrTime)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (xrLocateSpace(m_impl->view_space, m_impl->local_space, t, &loc) == XR_SUCCESS &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            hmd_world = loc.pose;
        }
    } else {
        // Fallback: use last known pose (original behaviour, less accurate after canvas moves)
        hmd_world = m_impl->last_hmd_pose;
    }

    // Extract yaw from the true world-space HMD orientation.
    // OpenXR convention: Y-up, -Z forward.
    const XrQuaternionf& q = hmd_world.orientation;
    float siny = 2.0f * (q.w * q.y + q.x * q.z);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
    float yaw  = std::atan2f(siny, cosy);

    // Build a LOCAL space whose origin is at the HMD position, rotated by yaw.
    // Canvas at (0,0,-depth) in this space appears centred in the user's view.
    const XrVector3f& p = hmd_world.position;

    XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.position    = { p.x, p.y, p.z };
    rsi.poseInReferenceSpace.orientation = { 0.0f, std::sinf(yaw * 0.5f), 0.0f,
                                             std::cosf(yaw * 0.5f) };

    if (m_impl->app_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_impl->app_space);
        m_impl->app_space = XR_NULL_HANDLE;
    }
    xrCreateReferenceSpace(m_impl->session, &rsi, &m_impl->app_space);

    // Reset canvas offsets — canvas is now centred by construction.
    m_canvas_x = 0.0f;  m_canvas_y  = 0.0f;
    m_canvas_az = 0.0f; m_canvas_el = 0.0f;
}

// ============================================================
// ============================================================
// JNI helper: call a Kotlin method that returns IntArray pixels,
// convert ARGB→RGBA, upload to a GL texture.
// ============================================================
static void rebuild_panel_tex(JavaVM* vm, jobject activity,
                               const char* method,
                               jobjectArray names_arr,
                               const std::vector<jvalue>& extra_args,
                               const char* sig,
                               int tex_w, int tex_h,
                               GLuint& tex_out, bool& dirty_out)
{
    JNIEnv* env = nullptr;
    bool detach = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, method, sig);
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(cls); if (detach) vm->DetachCurrentThread(); return; }

    // Build args: names_arr first, then extra args
    std::vector<jvalue> args;
    jvalue v0; v0.l = names_arr; args.push_back(v0);
    for (const auto& a : extra_args) args.push_back(a);

    auto pixels = (jintArray)env->CallObjectMethodA(activity, mid, args.data());
    env->DeleteLocalRef(cls);

    if (!pixels || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (detach) vm->DetachCurrentThread();
        return;
    }

    jsize count = env->GetArrayLength(pixels);
    if (count == tex_w * tex_h) {
        jint* raw = env->GetIntArrayElements(pixels, nullptr);
        if (raw) {
            std::vector<uint8_t> rgba(count * 4);
            for (jsize i = 0; i < count; ++i) {
                jint a = raw[i];
                rgba[i*4+0] = (a >> 16) & 0xFF; // R
                rgba[i*4+1] = (a >>  8) & 0xFF; // G
                rgba[i*4+2] = (a      ) & 0xFF; // B
                rgba[i*4+3] = (a >> 24) & 0xFF; // A
            }
            upload_panel_texture(tex_out, tex_w, tex_h, rgba);
            env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
        }
        dirty_out = false;
    }
    env->DeleteLocalRef(pixels);
    if (detach) vm->DetachCurrentThread();
}

// ============================================================
// rebuild_layer_panel_texture
// ============================================================
void OpenXrShell::rebuild_layer_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    int n = (int)m_layer_names.size();
    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray names_arr = env->NewObjectArray(n, str_cls, nullptr);
    // Reorder names so display order matches m_layer_order
    for (int i = 0; i < n; ++i) {
        int orig = (i < (int)m_layer_order.size()) ? m_layer_order[i] : i;
        jstring js = env->NewStringUTF(
            (orig < (int)m_layer_names.size()) ? m_layer_names[orig].c_str() : "?");
        env->SetObjectArrayElement(names_arr, i, js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(str_cls);

    // Build enabled array in display order
    jbooleanArray enabled_arr = env->NewBooleanArray(n);
    {
        std::vector<jboolean> ev(n);
        for (int i = 0; i < n; ++i) {
            int orig = (i < (int)m_layer_order.size()) ? m_layer_order[i] : i;
            ev[i] = (orig < (int)m_layer_enabled.size()) ? m_layer_enabled[orig] : JNI_TRUE;
        }
        env->SetBooleanArrayRegion(enabled_arr, 0, n, ev.data());
    }

    // Build ambilight array in display order
    jbooleanArray ambilight_arr = env->NewBooleanArray(n);
    {
        std::vector<jboolean> av(n);
        for (int i = 0; i < n; ++i) {
            int orig = (i < (int)m_layer_order.size()) ? m_layer_order[i] : i;
            av[i] = (orig < (int)m_layer_ambilight.size()) ? m_layer_ambilight[orig] : JNI_TRUE;
        }
        env->SetBooleanArrayRegion(ambilight_arr, 0, n, av.data());
    }

    const PanelMetrics metrics = panel_metrics(PanelKind::Layers);
    std::vector<jvalue> args;
    jvalue a; a.l = enabled_arr;           args.push_back(a);
    jvalue b; b.l = ambilight_arr;         args.push_back(b);
    jvalue c; c.i = m_layer_panel_grabbed; args.push_back(c);
    jvalue d; d.i = m_layer_panel_hovered; args.push_back(d); // drop-target row
    jvalue e; e.i = metrics.tex_w;         args.push_back(e);
    jvalue f; f.i = metrics.tex_h;         args.push_back(f);
    jstring auto_dup_label = env->NewStringUTF(layer_auto_dup_label(m_layer_auto_dup_percent).c_str());
    jstring filter_label = env->NewStringUTF(layer_filter_mode_label(m_layer_filter_mode));
    jvalue g; g.z = m_emu_frozen_display;   args.push_back(g); // frozen state for play/pause button
    jvalue h; h.l = auto_dup_label;         args.push_back(h);
    jvalue i; i.l = filter_label;           args.push_back(i);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderLayerPanelBitmap", names_arr, args,
                      "([Ljava/lang/String;[Z[ZIIIIZLjava/lang/String;Ljava/lang/String;)[I",
                      metrics.tex_w, metrics.tex_h, m_layer_panel_tex, m_layer_panel_dirty);

    env->DeleteLocalRef(names_arr);
    env->DeleteLocalRef(enabled_arr);
    env->DeleteLocalRef(ambilight_arr);
    env->DeleteLocalRef(auto_dup_label);
    env->DeleteLocalRef(filter_label);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_settings_panel_texture
// ============================================================
void OpenXrShell::rebuild_settings_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    // Build name/value/isBool arrays for 19 settings (5 bools + 4 floats + 1 cycle + 3 perf + 5 action buttons + 1 ctrlmap)
    // Rows 0-4: bools, Rows 5-8: floats, Row 9: Refresh Hz, Rows 10-12: perf settings, Rows 13-17: action buttons, Row 18: ctrlmap
    struct SettingDef { const char* name; bool is_bool; };
    static const SettingDef defs[19] = {
        {"Immersive Beta", true }, {"Upscale",      true },
        {"Ambilight",    true }, {"Passthrough",  true }, {"Depthmap",     true },
        {"Gamma",        false}, {"Contrast",     false}, {"Saturation",   false},
        {"Brightness",   false},
        {"Refresh Hz",   false},
        {"Auto FrameSkip", true},
        {"Emu Res Scale", false},
        {"VR Res Scale",  false},
        // Action buttons (rows 13-17) — rendered specially in Kotlin
        {"Reset Settings",  false},
        {"Save Game",       false},
        {"Save Global",     false},
        {"Load Game",       false},
        {"Load Global",     false},
        // Controller map button (row 18)
        {"Controller Map",  false},
    };

    // Determine display refresh rate label
    char refresh_label[32];
    if (m_impl && !m_impl->available_rates.empty()) {
        float disp = pick_default_refresh_rate(m_impl->available_rates);
        if (m_desired_refresh_rate > 0.0f) {
            float best_d = 1e9f;
            for (float r : m_impl->available_rates)
                if (std::abs(r - m_desired_refresh_rate) < best_d) { best_d = std::abs(r - m_desired_refresh_rate); disp = r; }
        }
        snprintf(refresh_label, sizeof(refresh_label), "%.0f Hz", disp);
    } else {
        if (m_desired_refresh_rate > 0.0f)
            snprintf(refresh_label, sizeof(refresh_label), "%.0f Hz", m_desired_refresh_rate);
        else
            snprintf(refresh_label, sizeof(refresh_label), "Default");
    }

    char val_bufs[19][64];
    snprintf(val_bufs[0], sizeof(val_bufs[0]), "%s", m_vr_state.immersive_beta_enabled ? "ON" : "OFF");
    snprintf(val_bufs[1], sizeof(val_bufs[1]), "%s", m_vr_state.upscale    ? "ON" : "OFF");
    snprintf(val_bufs[2], sizeof(val_bufs[2]), "%s", m_vr_state.ambilight  ? "ON" : "OFF");
    snprintf(val_bufs[3], sizeof(val_bufs[3]), "%s", m_vr_state.shadows    ? "ON" : "OFF");
    snprintf(val_bufs[4], sizeof(val_bufs[4]), "%s", m_vr_state.depthmap   ? "ON" : "OFF");
    snprintf(val_bufs[5], sizeof(val_bufs[5]), "%.2f", m_vr_state.gamma);
    snprintf(val_bufs[6], sizeof(val_bufs[6]), "%.2f", m_vr_state.contrast);
    snprintf(val_bufs[7], sizeof(val_bufs[7]), "%.2f", m_vr_state.saturation);
    snprintf(val_bufs[8], sizeof(val_bufs[8]), "%.2f", m_vr_state.brightness);
    snprintf(val_bufs[9], sizeof(val_bufs[9]), "%s", refresh_label);
    snprintf(val_bufs[10], sizeof(val_bufs[10]), "%s", m_vr_state.auto_frame_skip ? "ON" : "OFF");
    snprintf(val_bufs[11], sizeof(val_bufs[11]), "%dx", m_vr_state.emu_resolution_scale);
    snprintf(val_bufs[12], sizeof(val_bufs[12]), "%.2fx", m_vr_state.vr_resolution_scale);
    // Action button rows 13-17, controller map row 18
    snprintf(val_bufs[13], sizeof(val_bufs[13]), "ACTION"); // Reset
    snprintf(val_bufs[14], sizeof(val_bufs[14]), "ACTION"); // Save Game
    snprintf(val_bufs[15], sizeof(val_bufs[15]), "ACTION"); // Save Global
    snprintf(val_bufs[16], sizeof(val_bufs[16]), "ACTION"); // Load Game
    snprintf(val_bufs[17], sizeof(val_bufs[17]), "ACTION"); // Load Global
    snprintf(val_bufs[18], sizeof(val_bufs[18]), "ACTION"); // Controller Map

    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) { env->ExceptionClear(); if (detach) m_vm->DetachCurrentThread(); return; }
    jobjectArray names_arr  = env->NewObjectArray(19, str_cls, nullptr);
    jobjectArray values_arr = env->NewObjectArray(19, str_cls, nullptr);
    jbooleanArray bool_arr  = env->NewBooleanArray(19);
    std::vector<jboolean> bv(19);
    for (int i = 0; i < 19; ++i) {
        jstring jn = env->NewStringUTF(defs[i].name);
        jstring jv = env->NewStringUTF(val_bufs[i]);
        env->SetObjectArrayElement(names_arr, i, jn);
        env->SetObjectArrayElement(values_arr, i, jv);
        env->DeleteLocalRef(jn);
        env->DeleteLocalRef(jv);
        bv[i] = defs[i].is_bool ? JNI_TRUE : JNI_FALSE;
    }
    env->SetBooleanArrayRegion(bool_arr, 0, 19, bv.data());
    env->DeleteLocalRef(str_cls);

    const PanelMetrics metrics = panel_metrics(PanelKind::Settings);

    // Encode current share-code to display in panel title
    const GameConfig* cfg = m_config.layers.empty() ? nullptr : &m_config;
    const int filter_mode = (cfg && is_snes_filter_capable_config(*cfg)) ? (int)m_layer_filter_mode : -1;
    std::string share_code = vr_state_encode(
        m_vr_state, cfg, &m_layer_order, &m_layer_enabled, &m_layer_ambilight, filter_mode);
    jstring jcode = env->NewStringUTF(share_code.c_str());

    std::vector<jvalue> args;
    jvalue a; a.l = values_arr;              args.push_back(a);
    jvalue b; b.l = bool_arr;               args.push_back(b);
    jvalue c; c.i = m_settings_panel_hovered; args.push_back(c);
    jvalue d; d.i = m_settings_panel_area;    args.push_back(d);
    jvalue e; e.i = metrics.tex_w;           args.push_back(e);
    jvalue f; f.i = metrics.tex_h;           args.push_back(f);
    jvalue g; g.l = jcode;                   args.push_back(g);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderSettingsPanelBitmap", names_arr, args,
                      "([Ljava/lang/String;[Ljava/lang/String;[ZIIIILjava/lang/String;)[I",
                      metrics.tex_w, metrics.tex_h, m_settings_panel_tex, m_settings_panel_dirty);

    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(jcode);
    env->DeleteLocalRef(names_arr);
    env->DeleteLocalRef(values_arr);
    env->DeleteLocalRef(bool_arr);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_code_panel_texture
// Key layout (37 keys): 0-9, A-Z, ⌫
// ============================================================
void OpenXrShell::rebuild_code_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    // Generate current share code for display
    const int filter_mode = is_snes_filter_capable_config(m_config) ? (int)m_layer_filter_mode : -1;
    std::string current_code = vr_state_encode(
        m_vr_state, &m_config, &m_layer_order, &m_layer_enabled, &m_layer_ambilight, filter_mode);

    const PanelMetrics metrics = panel_metrics(PanelKind::Code);

    jstring j_input   = env->NewStringUTF(m_code_input_buf.c_str());
    jstring j_current = env->NewStringUTF(current_code.c_str());
    jclass  cls       = env->GetObjectClass(m_activity_global);
    jmethodID mid     = env->GetMethodID(cls, "renderCodePanelBitmap",
                                          "(Ljava/lang/String;Ljava/lang/String;III)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(j_input);
        env->DeleteLocalRef(j_current);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    auto pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid, j_input, j_current, (jint)m_code_panel_hovered,
        (jint)metrics.tex_w, (jint)metrics.tex_h);
    env->DeleteLocalRef(j_input);
    env->DeleteLocalRef(j_current);

    if (!pixels || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jsize count = env->GetArrayLength(pixels);
    if (count == metrics.tex_w * metrics.tex_h) {
        jint* raw = env->GetIntArrayElements(pixels, nullptr);
        if (raw) {
            std::vector<uint8_t> rgba(count * 4);
            for (jsize i = 0; i < count; ++i) {
                jint a = raw[i];
                rgba[i*4+0] = (a >> 16) & 0xFF;
                rgba[i*4+1] = (a >>  8) & 0xFF;
                rgba[i*4+2] = (a      ) & 0xFF;
                rgba[i*4+3] = (a >> 24) & 0xFF;
            }
            upload_panel_texture(m_code_panel_tex, metrics.tex_w, metrics.tex_h, rgba);
            env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
        }
        m_code_panel_dirty = false;
    }
    env->DeleteLocalRef(pixels);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_ctrlmap_panel_texture
// ============================================================
void OpenXrShell::rebuild_ctrlmap_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::CtrlMap);
    constexpr int n  = SNES_BUTTON_COUNT;

    // Build parallel arrays: emulated button names and current Quest bindings.
    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray snes_names  = env->NewObjectArray(n, str_cls, nullptr);
    jobjectArray quest_names = env->NewObjectArray(n, str_cls, nullptr);
    for (int i = 0; i < n; ++i) {
        jstring jn = env->NewStringUTF(button_name_for_backend(m_current_backend_kind, i));
        jstring jq = env->NewStringUTF(qi_name(m_button_map[i]));
        env->SetObjectArrayElement(snes_names,  i, jn);
        env->SetObjectArrayElement(quest_names, i, jq);
        env->DeleteLocalRef(jn);
        env->DeleteLocalRef(jq);
    }
    env->DeleteLocalRef(str_cls);

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderCtrlMapPanelBitmap",
        "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;IIII)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->DeleteLocalRef(snes_names);
        env->DeleteLocalRef(quest_names);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jstring title = env->NewStringUTF(button_map_title_for_backend(m_current_backend_kind));
    jintArray pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid,
        title, snes_names, quest_names,
        (jint)m_ctrlmap_panel_hovered, (jint)m_ctrlmap_selected_row,
        (jint)metrics.tex_w, (jint)metrics.tex_h);

    env->DeleteLocalRef(title);
    env->DeleteLocalRef(snes_names);
    env->DeleteLocalRef(quest_names);

    if (pixels && !env->ExceptionCheck()) {
        jsize count = env->GetArrayLength(pixels);
        if (count == metrics.tex_w * metrics.tex_h) {
            jint* raw = env->GetIntArrayElements(pixels, nullptr);
            if (raw) {
                std::vector<uint8_t> rgba(count * 4);
                for (jsize i = 0; i < count; ++i) {
                    jint a = raw[i];
                    rgba[i*4+0] = (a >> 16) & 0xFF;
                    rgba[i*4+1] = (a >>  8) & 0xFF;
                    rgba[i*4+2] = (a      ) & 0xFF;
                    rgba[i*4+3] = (a >> 24) & 0xFF;
                }
                upload_panel_texture(m_ctrlmap_panel_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_ctrlmap_panel_dirty = false;
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (pixels) env->DeleteLocalRef(pixels);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_main_menu_texture
// ============================================================
void OpenXrShell::rebuild_main_menu_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::MainMenu);
    static const char* k_menu_items[] = {
        "Open ROM",
        "Settings",
        "Layers",
        "Mappings",
        "View/Enter Code",
        "Stop Emulation",
        "Exit"
    };
    constexpr int k_item_count = 7;

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray items_arr = env->NewObjectArray(k_item_count, str_cls, nullptr);
    for (int i = 0; i < k_item_count; ++i) {
        jstring js = env->NewStringUTF(k_menu_items[i]);
        env->SetObjectArrayElement(items_arr, i, js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(str_cls);

    // Get currently loaded ROM name for display
    jstring j_rom = env->NewStringUTF(m_current_rom_name.c_str());

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderMainMenuPanelBitmap",
        "([Ljava/lang/String;IIILjava/lang/String;)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->DeleteLocalRef(items_arr);
        env->DeleteLocalRef(j_rom);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jintArray pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid,
        items_arr, (jint)m_main_menu_hovered, (jint)metrics.tex_w, (jint)metrics.tex_h, j_rom);

    env->DeleteLocalRef(items_arr);
    env->DeleteLocalRef(j_rom);

    if (pixels && !env->ExceptionCheck()) {
        jsize count = env->GetArrayLength(pixels);
        if (count == metrics.tex_w * metrics.tex_h) {
            jint* raw = env->GetIntArrayElements(pixels, nullptr);
            if (raw) {
                std::vector<uint8_t> rgba(count * 4);
                for (jsize i = 0; i < count; ++i) {
                    jint a = raw[i];
                    rgba[i*4+0] = (a >> 16) & 0xFF;
                    rgba[i*4+1] = (a >>  8) & 0xFF;
                    rgba[i*4+2] = (a      ) & 0xFF;
                    rgba[i*4+3] = (a >> 24) & 0xFF;
                }
                upload_panel_texture(m_main_menu_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_main_menu_dirty = false;
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (pixels) env->DeleteLocalRef(pixels);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// Settings persistence
// ============================================================

std::string OpenXrShell::get_settings_dir() {
    if (!m_settings_dir.empty()) return m_settings_dir;
    if (!m_vm || !m_activity_global) return "";

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return "";

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "getSettingsDirectory", "()Ljava/lang/String;");
    if (mid) {
        jstring js = (jstring)env->CallObjectMethod(m_activity_global, mid);
        if (js) {
            const char* cstr = env->GetStringUTFChars(js, nullptr);
            if (cstr) { m_settings_dir = cstr; env->ReleaseStringUTFChars(js, cstr); }
            env->DeleteLocalRef(js);
        }
    }
    env->DeleteLocalRef(cls);
    if (detach) m_vm->DetachCurrentThread();
    return m_settings_dir;
}

void OpenXrShell::set_current_rom(const std::string& rom_filename) {
    // Strip extension to get a clean stem for use as settings filename
    m_current_rom_name = rom_filename;
    auto dot = m_current_rom_name.rfind('.');
    if (dot != std::string::npos) m_current_rom_name = m_current_rom_name.substr(0, dot);
    // Sanitize: replace characters that are invalid in filenames
    for (auto& c : m_current_rom_name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    m_current_game_name.clear();
    m_load_game_pending = true;
}

void OpenXrShell::set_current_backend_kind(BackendKind kind) {
    m_current_backend_kind = kind;
    if (!is_snes_filter_capable_config(m_config) && kind == BackendKind::Genesis) {
        m_layer_filter_mode = LayerFilterMode::Hybrid;
    }
    m_config = default_config_for_backend(kind, m_layer_filter_mode);
    m_button_map = default_button_map_for_backend(kind);
    m_saved_layer_mode_state.valid = false;
    m_layer_order.clear();
    m_layer_enabled.clear();
    m_layer_ambilight.clear();
    ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
    sync_layer_capture_mask();
    m_layer_panel_dirty = true;
    m_settings_panel_dirty = true;
}

void OpenXrShell::set_current_game_name(const std::string& name) {
    m_current_game_name = name;
}

void OpenXrShell::reset_settings() {
    const float prev_vr_scale = m_vr_state.vr_resolution_scale;
    m_vr_state   = VrState{};
    m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
    m_layer_filter_mode = LayerFilterMode::Hybrid;
    m_config     = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
    m_button_map = default_button_map_for_backend(m_current_backend_kind);
    m_layer_auto_dup_percent = 25;
    m_saved_layer_mode_state.valid = false;
    m_layer_order.clear();
    m_layer_enabled.clear();
    m_layer_ambilight.clear();
    ensure_layer_runtime_state_matches_config(m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
    sync_layer_capture_mask();
    if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.auto_frame_skip);
    m_settings_panel_dirty = true;
    m_layer_panel_dirty    = true;
    m_ctrlmap_panel_dirty  = true;
    if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
}

void OpenXrShell::save_settings(bool game_scope) {
    std::string dir = get_settings_dir();
    if (dir.empty()) return;
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/" + (game_scope ? (m_current_rom_name + ".ini") : "global.ini");
    // Only persist refresh_rate in global scope (it's a device-level setting, not per-game)
    float rr = game_scope ? 0.0f : m_desired_refresh_rate;
    const int filter_mode = is_snes_filter_capable_config(m_config) ? (int)m_layer_filter_mode : -1;
    settings_save(path, m_vr_state, m_config, m_layer_order, m_layer_enabled, m_layer_ambilight,
                  filter_mode, m_layer_auto_dup_percent, rr, &m_button_map, m_current_backend_kind);
    m_saved_layer_mode_state.valid = is_snes_filter_capable_config(m_config);
    m_saved_layer_mode_state.mode = m_layer_filter_mode;
    m_saved_layer_mode_state.config = m_config;
    m_saved_layer_mode_state.order = m_layer_order;
    m_saved_layer_mode_state.enabled = m_layer_enabled;
    m_saved_layer_mode_state.ambilight = m_layer_ambilight;
    set_status(std::string(game_scope ? "Game" : "Global") + " settings saved.");
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static std::string strip_ini_version(const std::string& ini_name) {
    // Remove (USA), (Europe), (Japan), etc. from .ini name for matching
    static const char* regions[] = {
        "(USA)", "(Europe)", "(Japan)", "(World)", "(Rev A)", "(Rev B)",
        "(USA, Europe)", "(USA, Japan)", "(Europe, Japan)", "(World)",
        "(JU)", "(JE)", "(UE)", "(USA)", "(EUR)", "(JAP)"
    };
    std::string base = ini_name;
    for (const char* r : regions) {
        size_t pos = base.find(r);
        if (pos != std::string::npos) {
            base.erase(pos, strlen(r));
            break;
        }
    }
    while (!base.empty() && (base.back() == ' ' || base.back() == '_' || base.back() == '-')) {
        base.pop_back();
    }
    return base;
}

void OpenXrShell::load_settings(bool game_scope) {
    const float prev_vr_scale = m_vr_state.vr_resolution_scale;
    std::string dir = get_settings_dir();
    if (dir.empty()) return;
    if (!game_scope) {
        std::string path = dir + "/global.ini";
        float loaded_rr = -1.0f;
        int loaded_mode = -1;
        m_layer_auto_dup_percent = 25;
        LayerFilterMode sniffed_mode = LayerFilterMode::Hybrid;
        int sniffed_layers = -1;
        sniff_settings_layer_mode(path, sniffed_mode, sniffed_layers);
        if (is_snes_filter_capable_config(m_config)) {
            m_layer_filter_mode = sniffed_mode;
            m_config = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
        }
        if (!settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled, m_layer_ambilight,
                           &loaded_mode, &m_layer_auto_dup_percent, &loaded_rr, &m_button_map,
                           m_current_backend_kind)) return;
        if (loaded_mode >= 0 && loaded_mode <= (int)LayerFilterMode::Hybrid && is_snes_filter_capable_config(m_config)) {
            m_layer_filter_mode = (LayerFilterMode)loaded_mode;
            m_config = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
            m_layer_order.clear();
            m_layer_enabled.clear();
            m_layer_ambilight.clear();
            settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled, m_layer_ambilight,
                          nullptr, &m_layer_auto_dup_percent, &loaded_rr, &m_button_map,
                          m_current_backend_kind);
        }
        if (loaded_rr >= 0.0f) {
            m_desired_refresh_rate = loaded_rr;
            m_apply_refresh_pending = true;
        }
        ensure_layer_runtime_state_matches_config(
            m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
    } else {
        // Try exact match first (e.g., "Super Mario World (USA).ini")
        std::string path = dir + "/" + m_current_rom_name + ".ini";
        bool loaded = file_exists(path.c_str());
        float loaded_rr = -1.0f;
        int loaded_mode = -1;
        m_layer_auto_dup_percent = 25;
        if (loaded) {
            LayerFilterMode sniffed_mode = LayerFilterMode::Hybrid;
            int sniffed_layers = -1;
            sniff_settings_layer_mode(path, sniffed_mode, sniffed_layers);
            if (is_snes_filter_capable_config(m_config)) {
                m_layer_filter_mode = sniffed_mode;
                m_config = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
            }
            loaded = settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled,
                                   m_layer_ambilight, &loaded_mode, &m_layer_auto_dup_percent, nullptr, &m_button_map,
                                   m_current_backend_kind);
            if (loaded && loaded_mode >= 0 && loaded_mode <= (int)LayerFilterMode::Hybrid && is_snes_filter_capable_config(m_config)) {
                m_layer_filter_mode = (LayerFilterMode)loaded_mode;
                m_config = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
                m_layer_order.clear();
                m_layer_enabled.clear();
                m_layer_ambilight.clear();
                loaded = settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled,
                                       m_layer_ambilight, nullptr, &m_layer_auto_dup_percent, nullptr, &m_button_map,
                                       m_current_backend_kind);
            }
        }
        // If no exact match and we have header-derived game name, scan for any version match
        if (!loaded && !m_current_game_name.empty()) {
            std::string base_key = strip_ini_version(m_current_game_name);
            // Scan settings directory for matching .ini files
            void* dir_handle = opendir(dir.c_str());
            if (dir_handle) {
                struct dirent* entry;
                while ((entry = readdir(static_cast<DIR*>(dir_handle))) != nullptr) {
                    std::string fname = entry->d_name;
                    if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".ini") {
                        std::string ini_name = fname.substr(0, fname.size() - 4);
                        if (strip_ini_version(ini_name) == base_key) {
                            path = dir + "/" + fname;
                            LayerFilterMode sniffed_mode = LayerFilterMode::Hybrid;
                            int sniffed_layers = -1;
                            sniff_settings_layer_mode(path, sniffed_mode, sniffed_layers);
                            if (is_snes_filter_capable_config(m_config)) {
                                m_layer_filter_mode = sniffed_mode;
                                m_config = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
                            }
                            loaded = settings_load(path, m_vr_state, m_config, m_layer_order,
                                                 m_layer_enabled, m_layer_ambilight,
                                                 &loaded_mode, &m_layer_auto_dup_percent, nullptr, &m_button_map,
                                                 m_current_backend_kind);
                            if (loaded && loaded_mode >= 0 && loaded_mode <= (int)LayerFilterMode::Hybrid
                                && is_snes_filter_capable_config(m_config)) {
                                m_layer_filter_mode = (LayerFilterMode)loaded_mode;
                                m_config = default_config_for_backend(m_current_backend_kind, m_layer_filter_mode);
                                m_layer_order.clear();
                                m_layer_enabled.clear();
                                m_layer_ambilight.clear();
                                loaded = settings_load(path, m_vr_state, m_config, m_layer_order,
                                                       m_layer_enabled, m_layer_ambilight,
                                                       nullptr, &m_layer_auto_dup_percent, nullptr, &m_button_map,
                                                       m_current_backend_kind);
                            }
                            break;
                        }
                    }
                }
                closedir(static_cast<DIR*>(dir_handle));
            }
        }
        if (!loaded) return; // no settings found
        ensure_layer_runtime_state_matches_config(
            m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
    }
    sync_layer_capture_mask();
    m_saved_layer_mode_state.valid = is_snes_filter_capable_config(m_config);
    m_saved_layer_mode_state.mode = m_layer_filter_mode;
    m_saved_layer_mode_state.config = m_config;
    m_saved_layer_mode_state.order = m_layer_order;
    m_saved_layer_mode_state.enabled = m_layer_enabled;
    m_saved_layer_mode_state.ambilight = m_layer_ambilight;
    m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
    if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.auto_frame_skip);
    m_settings_panel_dirty  = true;
    m_layer_panel_dirty     = true;
    m_ctrlmap_panel_dirty   = true;
    if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
    set_status(std::string(game_scope ? "Game" : "Global") + " settings loaded.");
}

// ============================================================
// open_rom_menu — place panel in front of HMD, scan ROM dir
// ============================================================
void OpenXrShell::open_rom_menu() {
    const PanelMetrics main_metrics     = panel_metrics(PanelKind::MainMenu);
    const PanelMetrics code_metrics     = panel_metrics(PanelKind::Code);

    // Get ROM directory from Kotlin activity
    if (m_rom_dir.empty() && m_vm && m_activity_global) {
        JNIEnv* env = nullptr;
        bool detach = false;
        if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
        }
        if (env) {
            jclass cls = env->GetObjectClass(m_activity_global);
            jmethodID mid = env->GetMethodID(cls, "getRomDirectory", "()Ljava/lang/String;");
            if (mid) {
                jstring js = (jstring)env->CallObjectMethod(m_activity_global, mid);
                if (js) {
                    const char* cstr = env->GetStringUTFChars(js, nullptr);
                    if (cstr) { m_rom_dir = cstr; env->ReleaseStringUTFChars(js, cstr); }
                    env->DeleteLocalRef(js);
                }
            }
        }
        if (detach) m_vm->DetachCurrentThread();
    }

    m_rom_browser.scan(m_rom_dir);

    // Place main menu panel centred in front of HMD at eye height
    const XrQuaternionf& q = m_impl->last_hmd_pose.orientation;
    const XrVector3f&    p = m_impl->last_hmd_pose.position;

    // Extract yaw only — upright panels
    float siny = 2.0f * (q.w * q.y + q.x * q.z);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
    float yaw  = std::atan2f(siny, cosy);

    const XrQuaternionf orient = { 0.0f, std::sinf(yaw * 0.5f), 0.0f, std::cosf(yaw * 0.5f) };

    // Forward vector for this yaw
    const float fwd_x   = -std::sinf(yaw);
    const float fwd_z   = -std::cosf(yaw);
    constexpr float dist = 1.1f;

    const float cx = p.x + fwd_x * dist;
    const float cy = p.y;
    const float cz = p.z + fwd_z * dist;

    // Main menu panel (centred)
    m_main_menu_pose.position    = { cx, cy, cz };
    m_main_menu_pose.orientation = orient;
    m_main_menu_dirty            = true;
    m_main_menu_hovered          = -1;

    // Place sub-panel poses at the same location (they'll appear here when activated)
    m_panel_pose.position    = { cx, cy, cz };
    m_panel_pose.orientation = orient;

    m_layer_panel_pose.position    = { cx, cy, cz };
    m_layer_panel_pose.orientation = orient;
    m_layer_panel_dirty            = true;

    m_settings_panel_pose.position    = { cx, cy, cz };
    m_settings_panel_pose.orientation = orient;
    m_settings_panel_dirty            = true;

    // Code panel — above the main menu position
    const float hh_menu = main_metrics.world_h * 0.5f;
    const float hh_code = code_metrics.world_h * 0.5f;
    constexpr float gap        = 0.05f;
    m_code_panel_pose.position    = { cx, cy + hh_menu + gap + hh_code, cz };
    m_code_panel_pose.orientation = orient;
    m_code_panel_dirty            = true;
    m_code_input_buf.clear();

    // Ctrlmap panel: same position as settings panel
    m_ctrlmap_panel_pose          = m_settings_panel_pose;
    m_ctrlmap_panel_dirty         = true;
    m_ctrlmap_mode                = false;
    m_ctrlmap_selected_row        = -1;
    m_ctrlmap_panel_hovered       = -1;

    m_menu_open  = true;
    m_active_sub_panel = 0; // show main menu
    m_laser_hit  = false;
    m_laser_panel = -1;
}

// ============================================================
// fire_haptic — short vibration on left or right controller
// amplitude: 0-1, duration_ms: milliseconds
// ============================================================
void OpenXrShell::fire_haptic(bool right, float amplitude, int duration_ms) {
    XrAction act = right ? m_impl->act_haptic_r : m_impl->act_haptic_l;
    if (act == XR_NULL_HANDLE || !m_impl->session) return;
    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude  = amplitude;
    vib.duration   = (XrDuration)duration_ms * 1'000'000LL; // ms → ns
    vib.frequency  = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
    hai.type   = XR_TYPE_HAPTIC_ACTION_INFO;
    hai.action = act;
    xrApplyHapticFeedback(m_impl->session, &hai, (XrHapticBaseHeader*)&vib);
}

// ============================================================
// poll_actions — controller input + VR adjustments
// ============================================================
void OpenXrShell::poll_actions() {
    XrActiveActionSet aas{};
    aas.actionSet     = m_impl->action_set;
    aas.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.activeActionSets      = &aas;
    sync.countActiveActionSets = 1;
    if (xrSyncActions(m_impl->session, &sync) != XR_SUCCESS) return;

    // ---- helpers ----------------------------------------------------------------
    auto get_vec2 = [&](XrAction a, float& x, float& y) {
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
        if (xrGetActionStateVector2f(m_impl->session, &gi, &s) == XR_SUCCESS && s.isActive) {
            x = s.currentState.x; y = s.currentState.y; return true;
        }
        x = y = 0; return false;
    };
    auto get_bool = [&](XrAction a) {
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
        return (xrGetActionStateBoolean(m_impl->session, &gi, &s) == XR_SUCCESS
                && s.isActive && s.currentState);
    };
    auto get_float = [&](XrAction a) {
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
        return (xrGetActionStateFloat(m_impl->session, &gi, &s) == XR_SUCCESS && s.isActive)
               ? s.currentState : 0.0f;
    };
    // Locate a controller in app_space; returns false if invalid.
    auto get_controller_pos = [&](XrSpace hand_space, XrVector3f& out) -> bool {
        if (hand_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE)
            return false;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(hand_space, m_impl->app_space, m_frame_predicted_time, &loc) != XR_SUCCESS)
            return false;
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if ((loc.locationFlags & needed) != needed) return false;
        out = loc.pose.position;
        return true;
    };

    auto get_controller_pose = [&](XrSpace hand_space, XrPosef& out) -> bool {
        if (hand_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE)
            return false;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(hand_space, m_impl->app_space, m_frame_predicted_time, &loc) != XR_SUCCESS)
            return false;
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((loc.locationFlags & needed) != needed) return false;
        out = loc.pose;
        return true;
    };

    // ---- menu button (left controller ☰) = toggle ROM browser panel ------------
    bool menu_btn = get_bool(m_impl->act_menu);
    if (menu_btn && !m_menu_prev) {
        if (m_menu_open) {
            if (m_active_sub_panel != 0 || m_ctrlmap_mode) {
                // Go back to main menu from any sub-panel
                m_ctrlmap_mode = false;
                m_active_sub_panel = 0;
                m_main_menu_dirty = true;
                m_main_menu_hovered = -1;
                m_ctrlmap_selected_row = -1;
                m_ctrlmap_panel_hovered = -1;
                m_settings_panel_hovered = -1;
                fire_haptic(false, 0.25f, 30);
            } else {
                m_menu_open     = false;
                m_ctrlmap_mode  = false;
                m_laser_hit     = false;
                // Unfreeze emulator when closing menu
                EmuFreezeCtrl freeze_fn;
                { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                if (freeze_fn) freeze_fn(false);
            }
        } else {
            m_edit_mode = false; // exit edit mode when entering menu
            open_rom_menu();
            // Freeze emulator while menu is open
            EmuFreezeCtrl freeze_fn;
            { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
            if (freeze_fn) freeze_fn(true);
        }
        fire_haptic(false, 0.35f, 50); // left controller click feedback
    }
    m_menu_prev = menu_btn;

    // ---- laser + multi-panel hover (runs every frame while menu open, or layers panel shown over game) -----------
    if ((m_menu_open || m_active_sub_panel == 2) && m_impl->raim_space != XR_NULL_HANDLE) {
        XrPosef aim{};
        if (get_controller_pose(m_impl->raim_space, aim)) {
            const XrVector3f& O   = aim.position;
            const XrQuaternionf& aq = aim.orientation;

            // Aim forward = rotate (0,0,-1) by aim quaternion
            XrVector3f D;
            D.x = -2.0f*(aq.x*aq.z + aq.w*aq.y);
            D.y =  2.0f*(aq.w*aq.x - aq.y*aq.z);
            D.z =  2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f;

            m_laser_origin = O;
            constexpr float k_laser_max = 4.0f;

            // Panels to test: main menu mode, ctrlmap mode, or normal sub-panel mode
            struct PanelDesc { const XrPosef* pose; float w; float h; int idx; };
            const PanelMetrics main_metrics     = panel_metrics(PanelKind::MainMenu);
            const PanelMetrics browser_metrics  = panel_metrics(PanelKind::Browser);
            const PanelMetrics layer_metrics    = panel_metrics(PanelKind::Layers);
            const PanelMetrics settings_metrics = panel_metrics(PanelKind::Settings);
            const PanelMetrics code_metrics     = panel_metrics(PanelKind::Code);
            const PanelMetrics ctrlmap_metrics  = panel_metrics(PanelKind::CtrlMap);

            // Determine which panels are visible based on current mode
            PanelDesc* descs = nullptr;
            int descs_count = 0;

            if (m_ctrlmap_mode) {
                // Ctrlmap mode: only ctrlmap panel
                static PanelDesc descs_ctrlmap[1] = {
                    { &m_ctrlmap_panel_pose,  0.0f, 0.0f, k_panel_ctrlmap  },
                };
                descs_ctrlmap[0].w = ctrlmap_metrics.world_w;
                descs_ctrlmap[0].h = ctrlmap_metrics.world_h;
                descs = descs_ctrlmap;
                descs_count = 1;
            } else if (m_active_sub_panel == 0) {
                // Main menu showing: main menu + code panel
                static PanelDesc descs_main[2] = {
                    { &m_main_menu_pose,  0.0f, 0.0f, k_panel_main_menu },
                    { &m_code_panel_pose, 0.0f, 0.0f, k_panel_code      },
                };
                descs_main[0].w = main_metrics.world_w;
                descs_main[0].h = main_metrics.world_h;
                descs_main[1].w = code_metrics.world_w;
                descs_main[1].h = code_metrics.world_h;
                descs = descs_main;
                descs_count = 2;
            } else {
                // A sub-panel is active: show only that panel (+ code panel for browser)
                static PanelDesc descs_browser[2] = {
                    { &m_panel_pose,        0.0f, 0.0f, k_panel_browser  },
                    { &m_code_panel_pose,   0.0f, 0.0f, k_panel_code     },
                };
                static PanelDesc descs_layers[1] = {
                    { &m_layer_panel_pose,  0.0f, 0.0f, k_panel_layers   },
                };
                static PanelDesc descs_settings[1] = {
                    { &m_settings_panel_pose, 0.0f, 0.0f, k_panel_settings },
                };
                static PanelDesc descs_ctrlmap_sub[1] = {
                    { &m_ctrlmap_panel_pose,  0.0f, 0.0f, k_panel_ctrlmap  },
                };
                static PanelDesc descs_code[1] = {
                    { &m_code_panel_pose,    0.0f, 0.0f, k_panel_code      },
                };

                descs_browser[0].w = browser_metrics.world_w;
                descs_browser[0].h = browser_metrics.world_h;
                descs_browser[1].w = code_metrics.world_w;
                descs_browser[1].h = code_metrics.world_h;
                descs_layers[0].w = layer_metrics.world_w;
                descs_layers[0].h = layer_metrics.world_h;
                descs_settings[0].w = settings_metrics.world_w;
                descs_settings[0].h = settings_metrics.world_h;
                descs_ctrlmap_sub[0].w = ctrlmap_metrics.world_w;
                descs_ctrlmap_sub[0].h = ctrlmap_metrics.world_h;
                descs_code[0].w = code_metrics.world_w;
                descs_code[0].h = code_metrics.world_h;

                switch (m_active_sub_panel) {
                    case 1: descs = descs_browser;   descs_count = 2; break;
                    case 2: descs = descs_layers;    descs_count = 1; break;
                    case 3: descs = descs_settings;  descs_count = 1; break;
                    case 4: descs = descs_code;      descs_count = 1; break;
                    case 5: descs = descs_ctrlmap_sub; descs_count = 1; break;
                    default: break;
                }
            }

            int   best_panel = -1;
            float best_t     = k_laser_max;
            float best_u = 0, best_v = 0;
            XrVector3f best_H{};

            for (int di = 0; di < descs_count; ++di) {
                const auto& pd = descs[di];
                const XrQuaternionf& pq = pd.pose->orientation;
                // Panel normal = +Z of panel
                XrVector3f N;
                N.x = 2.0f*(pq.w*pq.y + pq.x*pq.z);
                N.y = 2.0f*(pq.y*pq.z - pq.w*pq.x);
                N.z = 1.0f - 2.0f*pq.x*pq.x - 2.0f*pq.y*pq.y;

                const XrVector3f& P = pd.pose->position;
                float dN = D.x*N.x + D.y*N.y + D.z*N.z;
                if (std::abs(dN) < 0.001f) continue;
                float t = ((P.x-O.x)*N.x + (P.y-O.y)*N.y + (P.z-O.z)*N.z) / dN;
                if (t <= 0.01f || t >= best_t) continue;

                // Hit point UV
                XrVector3f H = { O.x+t*D.x, O.y+t*D.y, O.z+t*D.z };
                XrVector3f right;
                right.x = 1.0f - 2.0f*(pq.y*pq.y + pq.z*pq.z);
                right.y = 2.0f*(pq.x*pq.y + pq.w*pq.z);
                right.z = 2.0f*(pq.x*pq.z - pq.w*pq.y);
                XrVector3f up;
                up.x = 2.0f*(pq.x*pq.y - pq.w*pq.z);
                up.y = 1.0f - 2.0f*(pq.x*pq.x + pq.z*pq.z);
                up.z = 2.0f*(pq.y*pq.z + pq.w*pq.x);
                float dx = H.x-P.x, dy = H.y-P.y, dz = H.z-P.z;
                float u = (dx*right.x + dy*right.y + dz*right.z) / (pd.w * 0.5f) * 0.5f + 0.5f;
                float v = -(dx*up.x   + dy*up.y   + dz*up.z)    / (pd.h * 0.5f) * 0.5f + 0.5f;
                if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;

                best_panel = pd.idx;
                best_t     = t;
                best_u     = u;
                best_v     = v;
                best_H     = H;
            }

            m_laser_panel = best_panel;
            m_laser_hit_u = best_u;
            m_laser_hit_v = best_v;
            if (best_panel >= 0) {
                m_laser_hit = true;
                m_laser_end = best_H;
            } else {
                m_laser_hit = false;
                m_laser_end = { O.x+D.x*k_laser_max, O.y+D.y*k_laser_max, O.z+D.z*k_laser_max };
            }

            // Route hover to appropriate panel
            if (best_panel == k_panel_main_menu) {
                // Map v → main menu row
                // Layout: title bar (88px) + 7 menu item rows
                // kH = 840; titleH = 88; total_rows = 7
                constexpr int   k_total_rows  = 7;
                constexpr float k_title_v     = 88.0f / 1200.0f;
                int row = -1;
                if (best_v > k_title_v) {
                    float row_h = (1.0f - k_title_v) / k_total_rows;
                    row = (int)((best_v - k_title_v) / row_h);
                    if (row >= k_total_rows) row = k_total_rows - 1;
                }
                if (row != m_main_menu_hovered) {
                    m_main_menu_hovered = row;
                }
            } else if (best_panel == k_panel_browser) {
                m_rom_browser.set_hover_uv(best_u, best_v);
            } else if (best_panel == k_panel_layers) {
                // Map v → display row (Kotlin renders at 896×1024)
                // Title bar is 88px out of 1280, rows: n layer rows + action rows
                constexpr float k_title_v = 88.0f / 1280.0f;
                int n = (int)m_layer_names.size();
                int totalRows = n + (is_snes_filter_capable_config(m_config) ? 3 : 2);
                int row = -1;
                if (best_v > k_title_v && totalRows > 0) {
                    float row_h = (1.0f - k_title_v) / totalRows;
                    row = (int)((best_v - k_title_v) / row_h);
                    if (row >= totalRows) row = totalRows - 1;
                }
                if (row != m_layer_panel_hovered) {
                    m_layer_panel_hovered = row;
                }
            } else if (best_panel == k_panel_settings) {
                // Map v → settings row; determine left/right area
                // Kotlin renders at 1024×1730 with 19 rows (5 bools + 4 floats + 1 cycle + 3 perf + 5 actions + 1 ctrlmap)
                constexpr int k_num_settings = 19;
                constexpr float k_title_v = 88.0f / 1730.0f;
                int row = -1;
                int area = 0;
                if (best_v > k_title_v) {
                    float row_h = (1.0f - k_title_v) / k_num_settings;
                    row = (int)((best_v - k_title_v) / row_h);
                    if (row >= k_num_settings) row = k_num_settings - 1;
                }
                if (best_u < 0.22f)      area = 1; // minus
                else if (best_u > 0.78f) area = 2; // plus
                if (row != m_settings_panel_hovered || area != m_settings_panel_area) {
                    m_settings_panel_hovered = row;
                    m_settings_panel_area    = area;
                    // No longer mark dirty — highlight is drawn as a separate quad
                }
            } else if (best_panel == k_panel_ctrlmap) {
                // Map v → ctrlmap row
                // Layout: title bar (88px) + 12 snes-button rows + 6 action rows
                // kH = 1100; titleH = 88; total_rows = 18
                constexpr int   k_total_rows  = 18; // 12 snes + 6 actions
                constexpr float k_title_v     = 88.0f / 1536.0f;
                int row = -1;
                if (best_v > k_title_v) {
                    float row_h = (1.0f - k_title_v) / k_total_rows;
                    row = (int)((best_v - k_title_v) / row_h);
                    if (row >= k_total_rows) row = k_total_rows - 1;
                }
                if (row != m_ctrlmap_panel_hovered) {
                    m_ctrlmap_panel_hovered = row;
                    // No longer mark dirty — highlight is drawn as a separate quad
                }
            } else if (best_panel == k_panel_code) {
                // Layout: title bar + 4 key rows (0-9, A-J, K-T, U-Z+⌫)
                // key indices: 0-35 = alphanumeric, 36 = backspace, -1 = none/title
                // Title bar height: 52 / 384 of panel height
                constexpr float k_title_v = 52.0f / 384.0f;
                constexpr int k_cols = 10;
                int key = -1;
                if (best_v > k_title_v) {
                    float row_frac = (best_v - k_title_v) / (1.0f - k_title_v);
                    int row = (int)(row_frac * 4.0f);
                    if (row > 3) row = 3;
                    int col = (int)(best_u * k_cols);
                    if (col >= k_cols) col = k_cols - 1;
                    if (row == 0) key = col;           // 0-9
                    else if (row == 1) key = 10 + col; // A-J
                    else if (row == 2) key = 20 + col; // K-T
                    else { // row 3: U-Z (6 keys) + ⌫
                        if (col < 6) key = 30 + col;
                        else         key = 36;
                    }
                }
                if (key != m_code_panel_hovered) {
                    m_code_panel_hovered = key;
                    // No longer mark dirty — highlight is drawn as a separate quad
                }
            }
        }
    }

    // ---- left thumbstick click = 3-way cycle: game → edit → layers → game ----
    bool lclick = get_bool(m_impl->act_lclick);
    if (lclick && !m_lstick_click_prev) {
        if (!m_edit_mode && m_active_sub_panel != 2) {
            // State 1: Game → Edit Mode
            m_edit_mode = true;
            m_menu_open = false;
            m_active_sub_panel = 0;
            m_laser_hit = false;
            // Initialize layer panel pose for later use
            m_layer_panel_pose = m_main_menu_pose;
            m_layer_panel_dirty = true;
            // Freeze emulator while edit mode is active
            EmuFreezeCtrl freeze_fn;
            { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
            if (freeze_fn) freeze_fn(true);
            // Snapshot canvas state at entry
            m_edit_canvas_x  = m_canvas_x;
            m_edit_canvas_y  = m_canvas_y;
            m_edit_canvas_az = m_canvas_az;
            m_edit_canvas_el = m_canvas_el;

            // Snapshot left aim direction for translation reference
            m_edit_laim_ref_valid = false;
            if (m_impl->laim_space != XR_NULL_HANDLE) {
                XrPosef laim{};
                if (get_controller_pose(m_impl->laim_space, laim)) {
                    const XrQuaternionf& aq = laim.orientation;
                    m_edit_laim_ref_dir = {
                        -2.0f*(aq.x*aq.z + aq.w*aq.y),
                         2.0f*(aq.w*aq.x - aq.y*aq.z),
                         2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f
                    };
                    m_edit_laim_ref_valid = true;
                }
            }

            // Snapshot right aim az/el for sphere reference
            m_edit_raim_ref_valid = false;
            if (m_impl->raim_space != XR_NULL_HANDLE) {
                XrPosef raim{};
                if (get_controller_pose(m_impl->raim_space, raim)) {
                    const XrQuaternionf& aq = raim.orientation;
                    XrVector3f D = {
                        -2.0f*(aq.x*aq.z + aq.w*aq.y),
                         2.0f*(aq.w*aq.x - aq.y*aq.z),
                         2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f
                    };
                    m_edit_raim_ref_az = std::atan2f(D.x, -D.z);
                    float horiz = sqrtf(D.x*D.x + D.z*D.z);
                    m_edit_raim_ref_el = std::atan2f(D.y, horiz);
                    m_edit_raim_ref_valid = true;
                }
            }
        } else if (m_edit_mode) {
            // State 2: Edit Mode → Layer Panel
            m_edit_mode = false;
            m_active_sub_panel = 2;
            // Place layer panel where main menu would be (centered)
            m_layer_panel_pose = m_main_menu_pose;
            m_layer_panel_dirty = true;
            // Track frozen state for play/pause button display
            {
                EmuFreezeCtrl freeze_fn;
                { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                if (freeze_fn) {
                    freeze_fn(false);
                    m_emu_frozen_display = false;
                }
            }
            fire_haptic(true, 0.25f, 30);
        } else if (m_active_sub_panel == 2) {
            // State 3: Layer Panel → Game
            m_active_sub_panel = 0;
            fire_haptic(true, 0.2f, 25);
        }
    }
    m_lstick_click_prev = lclick;

    XrTime now = (XrTime)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    constexpr XrTime k_fire_interval = 80'000'000; // 80 ms

    // ---- Layer panel follows left controller (continuous tracking) ----
    // Panel stays fixed in world space, no rotation
    if (m_active_sub_panel == 2 && m_impl->lhand_space != XR_NULL_HANDLE) {
        XrPosef lhand{};
        if (get_controller_pose(m_impl->lhand_space, lhand)) {
            // Position: follow controller with fixed offset in world axes
            // 35cm to the left, 15cm down, 55cm forward (negative Z = forward)
            m_layer_panel_pose.position.x = lhand.position.x - 0.35f;
            m_layer_panel_pose.position.y = lhand.position.y - 0.15f;
            m_layer_panel_pose.position.z = lhand.position.z - 0.55f;

            // Orientation: fixed identity (no rotation - panel faces forward in world)
            m_layer_panel_pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
        }
    }

    if (m_edit_mode) {
        // ==============================================================
        // EDIT MODE — canvas repositioning + VR adjustments
        // ==============================================================

        // --- Left controller laser → XY canvas translation ---
        // The left aim ray is cast onto a frontal plane at the canvas depth.
        // Canvas x/y shifts so the ray's hit point stays where the laser points,
        // relative to where it was pointing when edit mode was entered.
        if (m_edit_laim_ref_valid && m_impl->laim_space != XR_NULL_HANDLE) {
            XrPosef laim{};
            if (get_controller_pose(m_impl->laim_space, laim)) {
                const XrQuaternionf& aq = laim.orientation;
                // Current aim direction
                XrVector3f D = {
                    -2.0f*(aq.x*aq.z + aq.w*aq.y),
                     2.0f*(aq.w*aq.x - aq.y*aq.z),
                     2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f
                };
                const XrVector3f& O = laim.position;
                float depth = 1.5f;
                if (!m_config.layers.empty()) depth = m_config.layers[0].depth_meters;
                // Delta in aim direction maps to canvas translation at canvas depth
                float ddx = D.x - m_edit_laim_ref_dir.x;
                float ddy = D.y - m_edit_laim_ref_dir.y;
                m_canvas_x = m_edit_canvas_x + ddx * depth;
                m_canvas_y = m_edit_canvas_y + ddy * depth;

                // Update edit-mode left laser visuals
                constexpr float k_laser_len = 3.0f;
                m_edit_laser_l_origin = O;
                m_edit_laser_l_end = { O.x + D.x*k_laser_len, O.y + D.y*k_laser_len, O.z + D.z*k_laser_len };
            }
        }

        // --- Right controller laser → spherical canvas placement ---
        // The right aim ray direction defines az/el on a sphere of radius = canvas depth.
        // The canvas always faces toward the controller (normal = -ray_dir), so it works
        // lying down, upside-down, or at any orientation.
        if (m_edit_raim_ref_valid && m_impl->raim_space != XR_NULL_HANDLE) {
            XrPosef raim{};
            if (get_controller_pose(m_impl->raim_space, raim)) {
                const XrQuaternionf& aq = raim.orientation;
                XrVector3f D = {
                    -2.0f*(aq.x*aq.z + aq.w*aq.y),
                     2.0f*(aq.w*aq.x - aq.y*aq.z),
                     2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f
                };
                // Current aim az/el
                float cur_az = std::atan2f(D.x, -D.z);
                float horiz  = sqrtf(D.x*D.x + D.z*D.z);
                float cur_el = std::atan2f(D.y, horiz);
                // Apply delta from entry reference
                m_canvas_az = m_edit_canvas_az + (cur_az - m_edit_raim_ref_az);
                m_canvas_el = m_edit_canvas_el - (cur_el - m_edit_raim_ref_el);
                // No clamping — full sphere accessible
                // Wrap az to [-π, π]
                constexpr float k_pi = 3.14159265f;
                while (m_canvas_az >  k_pi) m_canvas_az -= 2.0f * k_pi;
                while (m_canvas_az < -k_pi) m_canvas_az += 2.0f * k_pi;
                m_canvas_el = std::max(-k_pi * 0.49f, std::min(k_pi * 0.49f, m_canvas_el));

                // Update edit-mode right laser visuals
                float depth = 1.5f;
                if (!m_config.layers.empty()) depth = m_config.layers[0].depth_meters;
                const XrVector3f& O = raim.position;
                m_edit_laser_r_origin = O;
                m_edit_laser_r_end = { O.x + D.x*depth, O.y + D.y*depth, O.z + D.z*depth };
            }
        }

        // Right stick X → spread
        float rx = 0, ry = 0;
        get_vec2(m_impl->act_rstick, rx, ry);
        float lx = 0, ly = 0;
        get_vec2(m_impl->act_lstick, lx, ly);
        constexpr float k_adj_thresh = 0.3f;
        if (std::abs(rx) > k_adj_thresh && (now - m_last_depth_fire > k_fire_interval)) {
            m_last_depth_fire = now;
            float spread_scale = 1.0f + rx * 0.05f;
            float near_d = 1.0f;
            for (const auto& lc : m_config.layers) near_d = std::min(near_d, lc.depth_meters);
            for (auto& lc : m_config.layers)
                lc.depth_meters = std::max(0.10f, near_d + (lc.depth_meters - near_d) * spread_scale);
        }

        // Right trigger → depth closer, right grip → depth farther
        float rtrig = get_float(m_impl->act_rtrig);
        float rgrip = get_float(m_impl->act_rgrip);
        if ((rtrig > 0.3f || rgrip > 0.3f) && (now - m_last_depth_fire > k_fire_interval)) {
            m_last_depth_fire = now;
            float delta = (rtrig - rgrip) * 0.25f;
            for (auto& lc : m_config.layers)
                lc.depth_meters = std::max(0.10f, lc.depth_meters - delta);
        }

        // Left trigger → wider, left grip → narrower
        float lw  = get_float(m_impl->act_ltrig);
        float lnr = get_float(m_impl->act_lgrip);
        if ((lw > 0.3f || lnr > 0.3f) && (now - m_last_width_fire > k_fire_interval)) {
            m_last_width_fire = now;
            float delta = (lw - lnr) * 0.25f;
            for (auto& lc : m_config.layers)
                lc.quad_width_meters = std::max(0.50f, lc.quad_width_meters + delta);
        }

        // Left stick X → duplicate copy count down/up.
        if (std::abs(lx) > k_adj_thresh && (now - m_last_copy_fire > k_fire_interval)) {
            m_last_copy_fire = now;
            const int delta = (lx > 0.0f) ? 1 : -1;
            const int next_count = std::clamp(current_base_copy_count(m_config, m_layer_order) + delta, 1, 32);
            set_all_layer_copy_counts(m_config, next_count);
            set_status(copy_count_status_text(m_config, m_layer_order, m_layer_auto_dup_percent));
        }

        // Right stick click → randomize
        bool rclick = get_bool(m_impl->act_rclick);
        if (rclick && !m_rstick_click_prev) randomize();
        m_rstick_click_prev = rclick;

        // Zero out game input in edit mode
        {
            std::lock_guard<std::mutex> lk(m_input_mutex);
            m_input_state = EmulatorInputState{};
        }

    } else if (m_menu_open || m_active_sub_panel == 2) {
        // ==============================================================
        // PANEL MODE — multi-panel dispatch
        // (also runs when layers panel is open over live game via thumbstick)
        // ==============================================================
        // Helper: step one emulator frame so layers refresh after a change while frozen
        auto do_step_one = [&]() {
            EmuStepOne step_fn;
            { std::lock_guard<std::mutex> lk(m_mutex); step_fn = m_emu_step_one; }
            if (step_fn) step_fn();
        };

        float rtrig     = get_float(m_impl->act_rtrig);
        bool  rtrig_now = rtrig > 0.5f;
        bool  rtrig_rising = rtrig_now && !m_rtrig_prev;
        float rx = 0, ry = 0;
        get_vec2(m_impl->act_rstick, rx, ry);

        XrTime now_panel = (XrTime)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        constexpr XrTime k_scroll_interval  = 300'000'000; // 300 ms
        constexpr XrTime k_setting_interval = 150'000'000; // 150 ms

        if (m_laser_panel == k_panel_main_menu) {
            // ---- Main menu ---------------------------------------------------
            int row = m_main_menu_hovered;
            if (rtrig_rising && m_laser_hit && row >= 0) {
                fire_haptic(true, 0.3f, 30);
                switch (row) {
                    case 0: // Open ROM → show browser panel (centered, no main menu)
                        m_active_sub_panel = 1;
                        m_ctrlmap_mode = false;
                        m_panel_pose = m_main_menu_pose;
                        m_rom_browser.dirty(); // ensure browser is refreshed
                        break;
                    case 1: // Settings → show settings panel (centered, no main menu)
                        m_active_sub_panel = 3;
                        m_ctrlmap_mode = false;
                        m_settings_panel_pose = m_main_menu_pose;
                        m_settings_panel_dirty = true;
                        rebuild_settings_panel_texture();
                        break;
                    case 2: // Layers → show layers panel (centered, no main menu)
                        m_active_sub_panel = 2;
                        m_ctrlmap_mode = false;
                        m_layer_panel_pose = m_main_menu_pose;
                        m_layer_panel_dirty = true;
                        rebuild_layer_panel_texture();
                        break;
                    case 3: // Mappings → show ctrlmap panel
                        m_active_sub_panel = 5;
                        m_ctrlmap_mode = true;
                        m_ctrlmap_panel_dirty = true;
                        m_ctrlmap_selected_row = -1;
                        m_ctrlmap_panel_hovered = -1;
                        break;
                    case 4: { // View/Enter Code → show code panel (centered, no main menu, no browser)
                        m_active_sub_panel = 4;
                        m_ctrlmap_mode = false;
                        m_code_panel_dirty = true;
                        m_code_input_buf.clear();
                        m_code_panel_pose = m_main_menu_pose;
                        break;
                    }
                    case 5: { // Stop Emulation → close menu, stop current ROM
                        m_menu_open     = false;
                        m_ctrlmap_mode  = false;
                        m_active_sub_panel = 0;
                        m_laser_hit     = false;
                        EmuFreezeCtrl freeze_fn;
                        { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                        if (freeze_fn) freeze_fn(false);
                        set_status("Emulation stopped.");
                        break;
                    }
                    case 6: { // Exit → close app
                        m_menu_open     = false;
                        m_ctrlmap_mode  = false;
                        m_active_sub_panel = 0;
                        m_laser_hit     = false;
                        EmuFreezeCtrl freeze_fn;
                        { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                        if (freeze_fn) freeze_fn(false);
                        // Call exitApp on Kotlin side
                        if (m_vm && m_activity_global) {
                            JNIEnv* env = nullptr;
                            bool detach = false;
                            if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                                if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
                            }
                            if (env) {
                                jclass cls = env->GetObjectClass(m_activity_global);
                                jmethodID mid = env->GetMethodID(cls, "exitApp", "()V");
                                if (mid) env->CallVoidMethod(m_activity_global, mid);
                                env->DeleteLocalRef(cls);
                                if (detach) m_vm->DetachCurrentThread();
                            }
                        }
                        break;
                    }
                    default: break;
                }
            }

        } else if (m_laser_panel == k_panel_browser) {
            // ---- ROM browser ------------------------------------------------
            if (rtrig_rising && m_laser_hit && !m_rom_browser.empty()) {
                if (m_rom_browser.hovered_is_dir()) {
                    fire_haptic(true, 0.3f, 40); // soft click for navigation
                    m_rom_browser.enter_hovered();
                } else {
                    const std::string& path = m_rom_browser.hovered_path();
                    if (!path.empty()) {
                        fire_haptic(true, 0.7f, 100); // strong "launch" buzz
                        RomLoader loader;
                        { std::lock_guard<std::mutex> lk(m_mutex); loader = m_rom_loader; }
                        if (loader) {
                            std::string err;
                            bool ok = loader(path, err);
                            set_status(ok ? ("Loaded: " + path.substr(path.rfind('/') + 1))
                                          : ("Load failed: " + err));
                        }
                        m_menu_open     = false;
                        m_ctrlmap_mode  = false;
                        m_active_sub_panel = 0;
                        m_laser_hit     = false;
                        // Unfreeze emulator — new ROM is loading
                        EmuFreezeCtrl freeze_fn;
                        { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                        if (freeze_fn) freeze_fn(false);
                    }
                }
            }
            // Scroll with stick Y
            if (std::abs(ry) > 0.6f && now_panel - m_last_depth_fire > k_scroll_interval) {
                m_last_depth_fire = now_panel;
                m_rom_browser.scroll(ry > 0 ? -1 : 1);
            }

        } else if (m_laser_panel == k_panel_layers) {
            // ---- Layer order panel (with play/pause + auto-dup + optional filter row) -----------
            int n = (int)m_layer_names.size();
            int row = m_layer_panel_hovered;
            const bool has_filter_row = is_snes_filter_capable_config(m_config);

            // Play/Pause button: row == n
            if (rtrig_rising && m_laser_hit && row == n) {
                EmuFreezeCtrl freeze_fn;
                { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                if (freeze_fn) {
                    m_emu_frozen_display = !m_emu_frozen_display;
                    freeze_fn(m_emu_frozen_display);
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.35f, 40);
                }
            }
            // Auto-dup cycle button: row == n + 1
            else if (rtrig_rising && m_laser_hit && row == n + 1) {
                m_layer_auto_dup_percent = next_layer_auto_dup_percent(m_layer_auto_dup_percent);
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.35f, 40);
                do_step_one();
            }
            else if (has_filter_row && rtrig_rising && m_laser_hit && row == n + 2) {
                apply_layer_filter_mode(next_layer_filter_mode(m_layer_filter_mode), true);
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.35f, 40);
                do_step_one();
            }
            // Layer row interactions (rows 0 to n-1)
            else if (rtrig_rising && m_laser_hit && row >= 0 && row < n) {
                int orig = m_layer_order[row];
                if (m_laser_hit_u > 0.80f) {
                    // Toggle ambilight (right 20% = AMB button)
                    if (orig < (int)m_layer_ambilight.size())
                        m_layer_ambilight[orig] = !m_layer_ambilight[orig];
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_u > 0.60f) {
                    // Toggle visibility (middle 20% = ON/OFF button)
                    if (orig < (int)m_layer_enabled.size())
                        m_layer_enabled[orig] = !m_layer_enabled[orig];
                    sync_layer_capture_mask();
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else {
                    // Start drag (left 60%)
                    m_layer_panel_grabbed = row;
                    m_layer_panel_dirty   = true;
                    fire_haptic(true, 0.25f, 20);
                }
            }

            // Drop on trigger release — reorder to wherever the laser is pointing
            if (!rtrig_now && m_layer_panel_grabbed >= 0) {
                int src = m_layer_panel_grabbed;
                int dst = (m_laser_hit && row >= 0 && row < n) ? row : src;
                if (src != dst) {
                    // Collect depths in current display order before shuffling
                    std::vector<float> depths(n);
                    for (int i = 0; i < n; ++i)
                        depths[i] = (m_layer_order[i] < (int)m_config.layers.size())
                                    ? m_config.layers[m_layer_order[i]].depth_meters : 1.5f;

                    // Rotate m_layer_order: move src slot to dst slot
                    if (src < dst) {
                        for (int i = src; i < dst; ++i)
                            std::swap(m_layer_order[i], m_layer_order[i + 1]);
                    } else {
                        for (int i = src; i > dst; --i)
                            std::swap(m_layer_order[i], m_layer_order[i - 1]);
                    }

                    // Re-sort depths ascending and assign in new display order
                    // so top row = nearest depth, bottom row = farthest depth
                    std::sort(depths.begin(), depths.end());
                    for (int i = 0; i < n; ++i) {
                        int orig = m_layer_order[i];
                        if (orig < (int)m_config.layers.size())
                            m_config.layers[orig].depth_meters = depths[i];
                    }
                }
                m_layer_panel_grabbed = -1;
                m_layer_panel_dirty   = true;
                if (src != dst) {
                    fire_haptic(true, 0.4f, 50); // dropped + reordered
                    do_step_one();
                }
            }

            // Mark dirty when hover changes while dragging (visual drop-target updates)
            static int s_prev_hover = -1;
            if (m_layer_panel_grabbed >= 0 && row != s_prev_hover) {
                m_layer_panel_dirty = true;
                s_prev_hover = row;
            }
            if (m_layer_panel_grabbed < 0) s_prev_hover = -1;

        } else if (m_laser_panel == k_panel_settings) {
            // ---- Settings panel ---------------------------------------------
            // Bools  (rows 0-4): trigger anywhere → toggle
            // Floats (rows 5-8): trigger on left 22% → dec, right 22% → inc
            //                    hold trigger + stick X for continuous tweak
            // Actions (rows 9-13): trigger → fire action
            auto adjust_setting = [&](int row, int dir) {
                // dir: -1 = dec, +1 = inc (ignored for bool rows)
                auto clamp = [](float v, float lo, float hi) {
                    return v < lo ? lo : v > hi ? hi : v;
                };
                constexpr float step = 0.1f;
                switch (row) {
                    case 0: m_vr_state.immersive_beta_enabled = !m_vr_state.immersive_beta_enabled; m_settings_panel_dirty=true; break;
                    case 1: m_vr_state.upscale     = !m_vr_state.upscale;     m_settings_panel_dirty=true; break;
                    case 2: m_vr_state.ambilight   = !m_vr_state.ambilight;   m_settings_panel_dirty=true; break;
                    case 3:
                        m_vr_state.shadows = !m_vr_state.shadows;
                        m_settings_panel_dirty = true;
                        sync_passthrough_state();
                        set_status(m_vr_state.shadows
                            ? (passthrough_active() ? "Passthrough ON" : "Passthrough unavailable on this OpenXR runtime.")
                            : "Passthrough OFF");
                        break;
                    case 4: m_vr_state.depthmap    = !m_vr_state.depthmap;    m_settings_panel_dirty=true; break;
                    case 5: m_vr_state.gamma       = clamp(m_vr_state.gamma      + dir*step, 0.5f, 2.0f); m_settings_panel_dirty=true; break;
                    case 6: m_vr_state.contrast    = clamp(m_vr_state.contrast   + dir*step, 0.5f, 2.0f); m_settings_panel_dirty=true; break;
                    case 7: m_vr_state.saturation  = clamp(m_vr_state.saturation + dir*step, 0.0f, 2.0f); m_settings_panel_dirty=true; break;
                    case 8: m_vr_state.brightness  = clamp(m_vr_state.brightness + dir*step, 0.5f, 2.0f); m_settings_panel_dirty=true; break;
                    case 9: {
                        // Refresh rate: cycle through available rates; dir=+1 → higher, dir=-1 → lower
                        if (!m_impl->available_rates.empty()) {
                            // Find current index (default to highest)
                            int cur_idx = (int)m_impl->available_rates.size() - 1;
                            if (m_desired_refresh_rate > 0.0f) {
                                float best_dist = 1e9f;
                                for (int ri = 0; ri < (int)m_impl->available_rates.size(); ++ri) {
                                    float d = std::abs(m_impl->available_rates[ri] - m_desired_refresh_rate);
                                    if (d < best_dist) { best_dist = d; cur_idx = ri; }
                                }
                            }
                            int new_idx = std::clamp(cur_idx + dir, 0, (int)m_impl->available_rates.size() - 1);
                            m_desired_refresh_rate = m_impl->available_rates[new_idx];
                            m_apply_refresh_pending = true;
                            m_settings_panel_dirty = true;
                        }
                        break;
                    }
                    case 10: m_vr_state.auto_frame_skip = !m_vr_state.auto_frame_skip; m_settings_panel_dirty=true; 
                        if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.auto_frame_skip);
                        break;
                    case 11: m_vr_state.emu_resolution_scale = std::clamp(m_vr_state.emu_resolution_scale + dir, 1, 4); m_settings_panel_dirty=true; break;
                    case 12: {
                        const int steps = std::clamp((int)std::lround(m_vr_state.vr_resolution_scale * 4.0f) + dir, 1, 16);
                        const float new_scale = steps * 0.25f;
                        if (std::abs(new_scale - m_vr_state.vr_resolution_scale) > 0.001f) {
                            m_vr_state.vr_resolution_scale = new_scale;
                            destroy_swapchains();
                        }
                        m_settings_panel_dirty = true;
                        break;
                    }
                    default: break;
                }
            };

            int row = m_settings_panel_hovered;
            if (rtrig_rising && m_laser_hit && row >= 0) {
                if (row <= 4) {
                    adjust_setting(row, 0);
                    fire_haptic(true, 0.3f, 25);
                    do_step_one();
                } else if (row <= 9) {
                    int dir = (m_settings_panel_area == 1) ? -1 : 1;
                    adjust_setting(row, dir);
                    fire_haptic(true, 0.2f, 15);
                    do_step_one();
                } else if (row == 10) {
                    // Auto FrameSkip — bool toggle
                    adjust_setting(row, 0);
                    fire_haptic(true, 0.3f, 25);
                    do_step_one();
                } else if (row <= 12) {
                    // Emu/VR Res Scale — int sliders
                    int dir = (m_settings_panel_area == 1) ? -1 : 1;
                    adjust_setting(row, dir);
                    fire_haptic(true, 0.2f, 15);
                    do_step_one();
                } else {
                    // Action buttons (rows 13-17) and ctrlmap button (row 18)
                    switch (row) {
                        case 13: m_settings_action_pending = 5; break; // Reset
                        case 14: m_settings_action_pending = 1; break; // Save Game
                        case 15: m_settings_action_pending = 2; break; // Save Global
                        case 16: m_settings_action_pending = 3; break; // Load Game
                        case 17: m_settings_action_pending = 4; break; // Load Global
                        case 18: {
                            // Open controller map panel (replace current sub-panel)
                            m_ctrlmap_mode            = true;
                            m_active_sub_panel        = 0; // ctrlmap_mode takes precedence
                            m_ctrlmap_panel_dirty     = true;
                            m_ctrlmap_selected_row    = -1;
                            m_ctrlmap_panel_hovered   = -1;
                            m_settings_panel_hovered  = -1;
                            m_settings_panel_dirty    = true;
                            // Place ctrlmap panel where the settings panel is
                            m_ctrlmap_panel_pose = m_settings_panel_pose;
                            break;
                        }
                        default: break;
                    }
                    fire_haptic(true, 0.4f, 40);
                }
            }
            // Continuous adjustment while holding trigger + stick X (only for float rows and int sliders)
            if (rtrig_now && ((row >= 5 && row <= 8) || (row >= 11 && row <= 12)) && std::abs(rx) > 0.5f
                && now_panel - m_last_settings_fire > k_setting_interval) {
                m_last_settings_fire = now_panel;
                adjust_setting(row, rx > 0 ? 1 : -1);
                do_step_one();
            }
        } else if (m_laser_panel == k_panel_ctrlmap) {
            // ---- Controller map panel ---------------------------------------
            int n = SNES_BUTTON_COUNT;
            int row = m_ctrlmap_panel_hovered;

            if (rtrig_rising && m_laser_hit && row >= 0) {
                if (row < n) {
                    // Top section: emulated button rows — select row to remap
                    if (m_ctrlmap_selected_row == row) {
                        // Deselect
                        m_ctrlmap_selected_row = -1;
                    } else {
                        m_ctrlmap_selected_row = row;
                    }
                    m_ctrlmap_panel_dirty = true;
                    fire_haptic(true, 0.3f, 25);
                } else {
                    // Bottom action buttons: row n=Reset, n+1=Load Game, n+2=Load Global, n+3=Save Game, n+4=Save Global, n+5=Back
                    int action_row = row - n;
                    switch (action_row) {
                        case 0: // Reset
                            m_button_map = default_button_map_for_backend(m_current_backend_kind);
                            m_ctrlmap_panel_dirty = true;
                            set_status("Button map reset to defaults.");
                            fire_haptic(true, 0.5f, 50);
                            break;
                        case 1: // Load Game
                            m_settings_action_pending = 3; // reuse load_game
                            fire_haptic(true, 0.4f, 40);
                            break;
                        case 2: // Load Global
                            m_settings_action_pending = 4; // reuse load_global
                            fire_haptic(true, 0.4f, 40);
                            break;
                        case 3: // Save Game
                            m_settings_action_pending = 1; // reuse save_game
                            fire_haptic(true, 0.4f, 40);
                            break;
                        case 4: // Save Global
                            m_settings_action_pending = 2; // reuse save_global
                            fire_haptic(true, 0.4f, 40);
                            break;
                        case 5: // Back → return to main menu
                            m_ctrlmap_mode          = false;
                            m_active_sub_panel      = 0;
                            m_ctrlmap_selected_row  = -1;
                            m_ctrlmap_panel_hovered = -1;
                            m_main_menu_dirty       = true;
                            fire_haptic(true, 0.3f, 30);
                            break;
                        default: break;
                    }
                }
            }

            // Stick X/Y to cycle through Quest inputs when a row is selected
            if (m_ctrlmap_selected_row >= 0 && m_ctrlmap_selected_row < n) {
                if (now_panel - m_last_settings_fire > k_setting_interval) {
                    if (std::abs(rx) > 0.6f || std::abs(ry) > 0.6f) {
                        m_last_settings_fire = now_panel;
                        int& binding = m_button_map[m_ctrlmap_selected_row];
                        int delta = (rx > 0.6f || ry > 0.6f) ? 1 : -1;
                        binding = (binding + delta + QI_COUNT) % QI_COUNT;
                        m_ctrlmap_panel_dirty = true;
                        fire_haptic(true, 0.2f, 15);
                    }
                }
            }

        } else if (m_laser_panel == k_panel_code) {
            // ---- Code-input panel -------------------------------------------
            // key indices: 0-35 = alphanumeric, 36 = backspace
            static const char* k_code_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            constexpr int k_backspace = 36;
            const int max_len = is_snes_filter_capable_config(m_config) ? 38 : (5 + 2 * (int)m_config.layers.size());
            int key = m_code_panel_hovered;
            if (rtrig_rising && m_laser_hit && key >= 0) {
                if (key == k_backspace) {
                    if (!m_code_input_buf.empty()) {
                        m_code_input_buf.pop_back();
                        m_code_panel_dirty = true;
                        fire_haptic(true, 0.2f, 15);
                    }
                } else if (key < 36 && (int)m_code_input_buf.size() < max_len) {
                    m_code_input_buf += k_code_chars[key];
                    m_code_panel_dirty = true;
                    fire_haptic(true, 0.25f, 20);

                    VrState test = {};
                    bool valid = false;
                    if (is_snes_filter_capable_config(m_config)) {
                        LayerFilterMode test_mode = LayerFilterMode::ShowAll;
                        GameConfig test_cfg;
                        std::vector<int> test_order;
                        std::vector<bool> test_enabled;
                        std::vector<bool> test_ambilight;
                        valid = try_decode_snes_state_code(
                            m_code_input_buf, test, test_mode, test_cfg, test_order, test_enabled, test_ambilight);
                    } else {
                        valid = vr_state_decode(m_code_input_buf, test, &m_config);
                    }
                    if (valid) {
                        apply_state_code(m_code_input_buf);
                        set_status("Code applied: " + m_code_input_buf);
                        m_code_input_buf.clear();
                        m_settings_panel_dirty = true;
                        fire_haptic(true, 0.6f, 60);
                    } else if ((int)m_code_input_buf.size() >= max_len) {
                        set_status("Invalid code");
                        m_code_input_buf.clear();
                        m_code_panel_dirty = true;
                    }
                }
            }
        }

        m_rtrig_prev = rtrig_now;
        // Zero game input while panel is open
        { std::lock_guard<std::mutex> lk(m_input_mutex); m_input_state = EmulatorInputState{}; }

    } else {
        // ==============================================================
        // NORMAL (GAME) MODE — button_map-driven SNES controller input
        // ==============================================================
        {
            std::lock_guard<std::mutex> lk(m_input_mutex);

            // Read all Quest physical inputs once
            float lx = 0, ly = 0, rx2 = 0, ry2 = 0;
            get_vec2(m_impl->act_lstick, lx, ly);
            get_vec2(m_impl->act_rstick, rx2, ry2);
            constexpr float k_thresh = 0.5f;

            // Build a lookup: QuestInput → bool state
            bool qi_state[QI_COUNT] = {};
            qi_state[QI_A]            = get_bool(m_impl->act_a);
            qi_state[QI_B]            = get_bool(m_impl->act_b);
            qi_state[QI_X]            = get_bool(m_impl->act_x);
            qi_state[QI_Y]            = get_bool(m_impl->act_y);
            qi_state[QI_RTRIG]        = get_float(m_impl->act_rtrig) > 0.5f;
            qi_state[QI_LTRIG]        = get_float(m_impl->act_ltrig) > 0.5f;
            qi_state[QI_RGRIP]        = get_float(m_impl->act_rgrip) > 0.7f;
            qi_state[QI_LGRIP]        = get_float(m_impl->act_lgrip) > 0.7f;
            qi_state[QI_RSTICK_UP]    = ry2 >  k_thresh;
            qi_state[QI_RSTICK_DOWN]  = ry2 < -k_thresh;
            qi_state[QI_RSTICK_LEFT]  = rx2 < -k_thresh;
            qi_state[QI_RSTICK_RIGHT] = rx2 >  k_thresh;
            qi_state[QI_LSTICK_UP]    = ly >  k_thresh;
            qi_state[QI_LSTICK_DOWN]  = ly < -k_thresh;
            qi_state[QI_LSTICK_LEFT]  = lx < -k_thresh;
            qi_state[QI_LSTICK_RIGHT] = lx >  k_thresh;

            auto mapped = [&](int snes_btn) -> bool {
                int qi = m_button_map[snes_btn];
                return (qi > QI_NONE && qi < QI_COUNT) ? qi_state[qi] : false;
            };

            bool btn_b      = mapped(SNES_B);
            bool btn_a      = mapped(SNES_A);
            bool btn_y      = mapped(SNES_Y);
            bool btn_x      = mapped(SNES_X);
            bool btn_l      = mapped(SNES_L);
            bool btn_r      = mapped(SNES_R);
            bool btn_start  = mapped(SNES_START);
            bool btn_select = mapped(SNES_SELECT);

            // D-pad from button map (can also be mapped to sticks above)
            bool dpad_up    = mapped(SNES_UP);
            bool dpad_down  = mapped(SNES_DOWN);
            bool dpad_left  = mapped(SNES_LEFT);
            bool dpad_right = mapped(SNES_RIGHT);

            // Haptic click on any button press (rising edge)
            bool any_new_press =
                (btn_b  && !m_input_state.button_b)  ||
                (btn_a  && !m_input_state.button_a)  ||
                (btn_y  && !m_input_state.button_y)  ||
                (btn_x  && !m_input_state.button_x)  ||
                (btn_l  && !m_input_state.button_l)  ||
                (btn_r  && !m_input_state.button_r)  ||
                (btn_start && !m_input_state.button_start);
            if (any_new_press) fire_haptic(true, 0.2f, 18);

            m_input_state.dpad_up     = dpad_up;
            m_input_state.dpad_down   = dpad_down;
            m_input_state.dpad_left   = dpad_left;
            m_input_state.dpad_right  = dpad_right;
            m_input_state.button_b      = btn_b;
            m_input_state.button_a      = btn_a;
            m_input_state.button_y      = btn_y;
            m_input_state.button_x      = btn_x;
            m_input_state.button_l      = btn_l;
            m_input_state.button_r      = btn_r;
            m_input_state.button_start  = btn_start;
            m_input_state.button_select = btn_select;
        }

        // Right stick click → recenter
        bool rclick = get_bool(m_impl->act_rclick);
        if (rclick && !m_rstick_click_prev) recenter_to_hmd();
        m_rstick_click_prev = rclick;
        m_rtrig_prev = false;
    }
}

// ============================================================
// apply_pending_vr_changes (called at frame start on XR thread)
// ============================================================
void OpenXrShell::apply_pending_vr_changes() {
    bool visual_change = false;
    if (m_randomize_pending.exchange(false)) {
        m_vr_state.randomize(m_config, m_rng);
        m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
        set_status("Randomized!\n" + vr_state_summary());
        visual_change = true;
    }
    int load_idx = m_preset_load_pending.exchange(-1);
    if (load_idx >= 0 && load_idx < (int)m_presets.size()) {
        const float prev_vr_scale = m_vr_state.vr_resolution_scale;
        m_vr_state = m_presets[load_idx];
        m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
        m_vr_state.apply_to_config(m_config);
        if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
        set_status("Loaded preset " + std::to_string(load_idx + 1));
        visual_change = true;
    }
    int save_idx = m_preset_save_pending.exchange(-1);
    if (save_idx >= 0 && save_idx < (int)m_presets.size()) {
        m_presets[save_idx] = m_vr_state;
        set_status("Saved preset " + std::to_string(save_idx + 1));
    }
    // Settings I/O actions from settings panel
    int action = m_settings_action_pending.exchange(0);
    switch (action) {
        case 1: save_settings(true);  m_settings_panel_dirty = true; break; // save game
        case 2: save_settings(false); m_settings_panel_dirty = true; break; // save global
        case 3: load_settings(true);  visual_change = true; break; // load game
        case 4: load_settings(false); visual_change = true; break; // load global
        case 5: reset_settings();     visual_change = true; break; // reset
        default: break;
    }
    // Share-code apply
    if (m_apply_code_pending.exchange(false)) {
        std::string code;
        { std::lock_guard<std::mutex> lk(m_mutex); code = m_pending_code; }
        VrState decoded{};
        bool ok = false;
        if (is_snes_filter_capable_config(m_config)) {
            LayerFilterMode decoded_mode = LayerFilterMode::ShowAll;
            GameConfig decoded_cfg;
            std::vector<int> decoded_order;
            std::vector<bool> decoded_enabled;
            std::vector<bool> decoded_ambilight;
            ok = try_decode_snes_state_code(
                code, decoded, decoded_mode, decoded_cfg, decoded_order, decoded_enabled, decoded_ambilight);
            if (ok) {
                m_layer_filter_mode = decoded_mode;
                m_config = std::move(decoded_cfg);
                m_layer_order = std::move(decoded_order);
                m_layer_enabled = std::move(decoded_enabled);
                m_layer_ambilight = std::move(decoded_ambilight);
                ensure_layer_runtime_state_matches_config(
                    m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
                sync_layer_capture_mask();
            }
        } else {
            ok = vr_state_decode(code, decoded, &m_config, &m_layer_order, &m_layer_enabled, &m_layer_ambilight);
        }
        if (ok) {
            const float prev_vr_scale = m_vr_state.vr_resolution_scale;
            m_vr_state = decoded;
            m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
            if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
            if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.auto_frame_skip);
            sync_layer_capture_mask();
            m_saved_layer_mode_state.valid = is_snes_filter_capable_config(m_config);
            m_saved_layer_mode_state.mode = m_layer_filter_mode;
            m_saved_layer_mode_state.config = m_config;
            m_saved_layer_mode_state.order = m_layer_order;
            m_saved_layer_mode_state.enabled = m_layer_enabled;
            m_saved_layer_mode_state.ambilight = m_layer_ambilight;
            m_settings_panel_dirty = true;
            m_layer_panel_dirty    = true;
            set_status("Code applied: " + code);
            visual_change = true;
        }
    }
    // Load global on startup (first frame after init)
    if (m_load_global_pending.exchange(false)) {
        load_settings(false); // silently skips if no file
    }
    // Load game settings when a new ROM is set
    if (m_load_game_pending.exchange(false)) {
        load_settings(true); // silently skips if no file
    }
    sync_passthrough_state();
    // Apply requested display refresh rate
    if (m_apply_refresh_pending.exchange(false)) {
        if (m_impl->has_refresh_ext && m_impl->pfn_set_refresh
                && !m_impl->available_rates.empty() && m_impl->session_running) {
            float target = m_desired_refresh_rate;
            if (target <= 0.0f) {
                target = pick_default_refresh_rate(m_impl->available_rates);
            } else {
                // Find closest available rate to the requested one
                float best = m_impl->available_rates[0];
                for (float r : m_impl->available_rates)
                    if (std::abs(r - target) < std::abs(best - target)) best = r;
                target = best;
            }
            m_impl->pfn_set_refresh(m_impl->session, target);
            m_active_refresh_rate = target;
            LOGI("Display refresh rate set to %.0f Hz", target);
        }
    }

    // If a visual change happened while frozen (menu open), step one emulator frame
    // so the layer processor sees fresh data and updates the display.
    if (visual_change && m_menu_open) {
        EmuStepOne step_fn;
        { std::lock_guard<std::mutex> lk(m_mutex); step_fn = m_emu_step_one; }
        if (step_fn) step_fn();
    }
}

// ============================================================
// render_frame
// ============================================================
void OpenXrShell::render_frame(XrTime predicted_time) {
    if (!m_impl || m_impl->eye[0].swapchain == XR_NULL_HANDLE) return;

    // Locate views
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime           = predicted_time;
    vli.space                 = m_impl->app_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{ {XR_TYPE_VIEW}, {XR_TYPE_VIEW} };
    uint32_t vc = 2;
    if (xrLocateViews(m_impl->session, &vli, &vs, 2, &vc, views) != XR_SUCCESS) return;

    // Track HMD pose: average of both eye positions, orientation from left eye.
    // Used when recentering the canvas to face the current gaze direction.
    if (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) {
        auto& lp = views[0].pose.position;
        auto& rp = views[1].pose.position;
        m_impl->last_hmd_pose.position = { (lp.x+rp.x)*0.5f,
                                           (lp.y+rp.y)*0.5f,
                                           (lp.z+rp.z)*0.5f };
        m_impl->last_hmd_pose.orientation = views[0].pose.orientation;

        // Auto-recenter once we have a valid HMD pose, so the canvas starts
        // directly in front of where the user is looking at app launch.
        if (!m_initial_recenter_done) {
            m_initial_recenter_done = true;
            recenter_to_hmd();
        }
    }

    // Get latest frame from emulator
    EmulatorInputState input_to_use;
    {
        std::lock_guard<std::mutex> lk(m_input_mutex);
        input_to_use = m_input_state;
    }
    FrameProvider provider;
    { std::lock_guard<std::mutex> lk(m_mutex); provider = m_frame_provider; }
    bool have_frame = false;
    const uint64_t prev_frame_seq = m_cached_frame_seq;
    if (provider) have_frame = provider(m_cached_frame_out, input_to_use, m_cached_frame_seq);
    const bool frame_updated = have_frame && (m_cached_frame_seq != prev_frame_seq);

    // Process layers only when the emulator publishes a new frame. XR may render
    // at 72/90/120 Hz, so rebuilding Genesis layer buffers every XR frame is wasteful.
    m_render_layer_refs.clear();
    if (!have_frame) {
        if (!m_menu_open && m_active_sub_panel != 2) {
            return;
        }
    }
    if (!m_menu_open && m_cached_frame_out.width > 0 && m_cached_frame_out.height > 0 && frame_updated) {
        // Update dynamic z-splits every ~0.5s to track moving sprites
        static uint32_t s_histogram[256] = {};
        static int s_frame_skip = 0;
        if (!m_cached_frame_out.zbuffer.empty()) {
            ++s_frame_skip;
            if (s_frame_skip >= 5) {  // check every 5 frames (~0.08s)
                s_frame_skip = 0;
                memset(s_histogram, 0, sizeof(s_histogram));
                const uint8_t* zptr = m_cached_frame_out.zbuffer.data();
                size_t npix = static_cast<size_t>(m_cached_frame_out.width) * m_cached_frame_out.height;
                for (size_t i = 0; i < npix && i < m_cached_frame_out.zbuffer.size(); ++i) {
                    s_histogram[zptr[i]]++;
                }
                m_config.update_z_splits(s_histogram);
            }
        }

        LayerProcessor proc(m_config);
        const uint8_t* zbuf = m_cached_frame_out.zbuffer.empty() ? nullptr : m_cached_frame_out.zbuffer.data();
        proc.process_into(
            m_cached_layer_frames,
            m_cached_frame_out.rgba8888.data(),
            (int)m_cached_frame_out.width,
            (int)m_cached_frame_out.height,
            zbuf,
            &m_cached_frame_out);
    }

    // Sync layer names/order/enabled when layer count changes (new ROM loaded)
    if (!m_cached_layer_frames.empty() && m_cached_layer_frames.size() != m_layer_names.size()) {
        ensure_layer_runtime_state_matches_config(
            m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight);
        m_layer_panel_dirty = true;
    }

    // Apply layer order, enabled flags, and ambilight flags
    if (!m_menu_open && !m_layer_order.empty() && m_layer_order.size() == m_cached_layer_frames.size()) {
        m_render_layer_refs.reserve(m_cached_layer_frames.size());
        for (int orig : m_layer_order) {
            if (orig < (int)m_layer_enabled.size() && m_layer_enabled[orig]) {
                auto& lf = m_cached_layer_frames[orig];
                lf.contrib_ambilight = (orig < (int)m_layer_ambilight.size())
                                       ? (bool)m_layer_ambilight[orig] : true;
                m_render_layer_refs.push_back(&lf);
            }
        }
    } else if (!m_menu_open) {
        m_render_layer_refs.reserve(m_cached_layer_frames.size());
        for (auto& lf : m_cached_layer_frames) m_render_layer_refs.push_back(&lf);
    }

    apply_layer_auto_dup_visible(m_render_layer_refs, m_layer_auto_dup_percent);
    compact_visible_layer_depths(m_render_layer_refs);

    // ---- Rebuild panel textures on GL thread (one per frame to avoid spike) ----
    // Throttle: JNI bitmap render is expensive; don't rebuild more than once per 100 ms.
    constexpr XrTime k_panel_rebuild_interval = 100'000'000; // 100 ms in nanoseconds
    if (!m_menu_open && m_active_sub_panel == 2) {
        // Layers panel open over live game (thumbstick shortcut, no menu)
        if (m_layer_panel_dirty &&
            (m_frame_predicted_time - m_last_layer_fire) >= k_panel_rebuild_interval) {
            rebuild_layer_panel_texture();
            m_last_layer_fire = m_frame_predicted_time;
        }
    }
    if (m_menu_open) {
        if (m_ctrlmap_mode) {
            // Only rebuild ctrlmap while in ctrlmap mode
            if (m_ctrlmap_panel_dirty &&
                (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_ctrlmap_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 0) {
            // Main menu showing: rebuild main menu or code panel
            if (m_main_menu_dirty &&
                (m_frame_predicted_time - m_last_main_menu_fire) >= k_panel_rebuild_interval) {
                rebuild_main_menu_texture();
                m_last_main_menu_fire = m_frame_predicted_time;
            } else if (m_code_panel_dirty &&
                       (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_code_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 1) {
            // Browser sub-panel: rebuild browser or code panel
            if (m_rom_browser.dirty() &&
                (m_frame_predicted_time - m_last_rom_fire) >= k_panel_rebuild_interval) {
                m_rom_browser.rebuild_texture(m_vm, m_activity_global);
                m_last_rom_fire = m_frame_predicted_time;
            } else if (m_code_panel_dirty &&
                       (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_code_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 2) {
            // Layers sub-panel
            if (m_layer_panel_dirty &&
                (m_frame_predicted_time - m_last_layer_fire) >= k_panel_rebuild_interval) {
                rebuild_layer_panel_texture();
                m_last_layer_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 3) {
            // Settings sub-panel
            if (m_settings_panel_dirty &&
                (m_frame_predicted_time - m_last_settings_fire) >= k_panel_rebuild_interval) {
                rebuild_settings_panel_texture();
                m_last_settings_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 4) {
            // Code panel (standalone)
            if (m_code_panel_dirty &&
                (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_code_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 5) {
            // Ctrlmap sub-panel
            if (m_ctrlmap_panel_dirty &&
                (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_ctrlmap_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        }
    }

    // ---- Build overlay ------------------------------------------------------
    OverlayInfo overlay;
    const PanelMetrics main_metrics     = panel_metrics(PanelKind::MainMenu);
    const PanelMetrics browser_metrics  = panel_metrics(PanelKind::Browser);
    const PanelMetrics layer_metrics    = panel_metrics(PanelKind::Layers);
    const PanelMetrics settings_metrics = panel_metrics(PanelKind::Settings);
    const PanelMetrics code_metrics     = panel_metrics(PanelKind::Code);
    const PanelMetrics ctrlmap_metrics  = panel_metrics(PanelKind::CtrlMap);
    if (m_menu_open) {
        if (m_ctrlmap_mode) {
            // Controller map panel only
            auto& cm     = overlay.panels[0];
            cm.tex       = m_ctrlmap_panel_tex;
            cm.pose      = m_ctrlmap_panel_pose;
            cm.w         = ctrlmap_metrics.world_w;
            cm.h         = ctrlmap_metrics.world_h;
            overlay.panel_count = 1;

            // Highlight for ctrlmap panel
            if (m_ctrlmap_panel_hovered >= 0) {
                constexpr float k_title_v = 88.0f / 1536.0f;
                constexpr int k_total_rows = 18;
                int row = m_ctrlmap_panel_hovered;
                if (row >= 0 && row < k_total_rows) {
                    float row_h = (1.0f - k_title_v) / k_total_rows;
                    overlay.highlight.panel_idx = 0;
                    overlay.highlight.u0 = 0.0f;
                    overlay.highlight.u1 = 1.0f;
                    overlay.highlight.v0 = k_title_v + row * row_h;
                    overlay.highlight.v1 = k_title_v + (row + 1) * row_h;
                    overlay.highlight.r = 0.16f;
                    overlay.highlight.g = 0.16f;
                    overlay.highlight.b = 0.39f;
                    overlay.highlight.alpha = 0.35f;
                }
            }
        } else if (m_active_sub_panel == 0) {
            // Main menu only (centred)
            auto& mm     = overlay.panels[0];
            mm.tex       = m_main_menu_tex;
            mm.pose      = m_main_menu_pose;
            mm.w         = main_metrics.world_w;
            mm.h         = main_metrics.world_h;
            overlay.panel_count = 1;

            // Highlight for main menu
            if (m_laser_panel == k_panel_main_menu && m_main_menu_hovered >= 0) {
                constexpr int k_total_rows = 7;
                constexpr float k_title_v = 88.0f / 1200.0f;
                int row = m_main_menu_hovered;
                if (row >= 0 && row < k_total_rows) {
                    float row_h = (1.0f - k_title_v) / k_total_rows;
                    overlay.highlight.panel_idx = 0;
                    overlay.highlight.u0 = 0.0f;
                    overlay.highlight.u1 = 1.0f;
                    overlay.highlight.v0 = k_title_v + row * row_h;
                    overlay.highlight.v1 = k_title_v + (row + 1) * row_h;
                    overlay.highlight.r = 0.16f;
                    overlay.highlight.g = 0.24f;
                    overlay.highlight.b = 0.39f;
                    overlay.highlight.alpha = 0.35f;
                }
            }
        } else {
            // Sub-panel only (no main menu) — centered
            if (m_active_sub_panel == 1) {
                // ROM browser
                auto& bp = overlay.panels[0];
                bp.tex   = m_rom_browser.texture();
                bp.pose  = m_panel_pose;
                bp.w     = browser_metrics.world_w;
                bp.h     = browser_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for browser
                if (m_laser_panel == k_panel_browser && m_rom_browser.hovered_index() >= 0) {
                    int row = m_rom_browser.hovered_index();
                    int n = m_rom_browser.count();
                    int visible = m_rom_browser.visible_count();
                    int scroll = m_rom_browser.scroll_offset();
                    if (row >= 0 && n > 0 && visible > 0) {
                        constexpr float k_title_v = 88.0f / 1536.0f;
                        float content_h = 1.0f - k_title_v;
                        float row_h = content_h / visible; // Match Kotlin's dynamic row height
                        int row_in_view = row - scroll;
                        if (row_in_view >= 0 && row_in_view < visible) {
                            float row_v0 = k_title_v + row_in_view * row_h;
                            float row_v1 = row_v0 + row_h;
                            if (row_v1 <= 1.0f) {
                                overlay.highlight.panel_idx = 0;
                                overlay.highlight.u0 = 0.0f;
                                overlay.highlight.u1 = 1.0f;
                                overlay.highlight.v0 = row_v0;
                                overlay.highlight.v1 = row_v1;
                                overlay.highlight.r = 0.18f;
                                overlay.highlight.g = 0.39f;
                                overlay.highlight.b = 0.75f;
                                overlay.highlight.alpha = 0.35f;
                            }
                        }
                    }
                }
            } else if (m_active_sub_panel == 2) {
                // Layers panel
                auto& lp = overlay.panels[0];
                lp.tex   = m_layer_panel_tex;
                lp.pose  = m_layer_panel_pose;
                lp.w     = layer_metrics.world_w;
                lp.h     = layer_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for layers
                if (m_laser_panel == k_panel_layers && m_layer_panel_hovered >= 0) {
                    int n = (int)m_layer_names.size();
                    int totalRows = n + (is_snes_filter_capable_config(m_config) ? 3 : 2);
                    int row = m_layer_panel_hovered;
                    if (row >= 0) {
                    constexpr float k_title_v = 88.0f / 1280.0f;
                        float row_h = totalRows > 0 ? (1.0f - k_title_v) / totalRows : 0.1f;
                        overlay.highlight.panel_idx = 0;
                        overlay.highlight.u0 = 0.0f;
                        overlay.highlight.u1 = 1.0f;
                        overlay.highlight.v0 = k_title_v + row * row_h;
                        overlay.highlight.v1 = k_title_v + (row + 1) * row_h;
                        overlay.highlight.r = 0.18f;
                        overlay.highlight.g = 0.39f;
                        overlay.highlight.b = 0.75f;
                        overlay.highlight.alpha = 0.35f;
                    }
                }
            } else if (m_active_sub_panel == 3) {
                // Settings panel
                auto& sp = overlay.panels[0];
                sp.tex   = m_settings_panel_tex;
                sp.pose  = m_settings_panel_pose;
                sp.w     = settings_metrics.world_w;
                sp.h     = settings_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for settings
                if (m_laser_panel == k_panel_settings && m_settings_panel_hovered >= 0) {
                    constexpr int k_num_settings = 19;
                    constexpr float k_title_v = 88.0f / 1730.0f;
                    int row = m_settings_panel_hovered;
                    if (row >= 0 && row < k_num_settings) {
                        float row_h = (1.0f - k_title_v) / k_num_settings;
                        overlay.highlight.panel_idx = 0;
                        overlay.highlight.u0 = 0.0f;
                        overlay.highlight.u1 = 1.0f;
                        overlay.highlight.v0 = k_title_v + row * row_h;
                        overlay.highlight.v1 = k_title_v + (row + 1) * row_h;
                        overlay.highlight.r = 0.16f;
                        overlay.highlight.g = 0.24f;
                        overlay.highlight.b = 0.39f;
                        overlay.highlight.alpha = 0.35f;
                    }
                }
            } else if (m_active_sub_panel == 4) {
                // Code panel (standalone, taller - shows code + keyboard)
                auto& cp = overlay.panels[0];
                cp.tex   = m_code_panel_tex;
                cp.pose  = m_code_panel_pose;
                cp.w     = code_metrics.world_w;
                cp.h     = code_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for code panel (larger title area for code display)
                if (m_laser_panel == k_panel_code && m_code_panel_hovered >= 0) {
                    int key = m_code_panel_hovered;
                    constexpr float k_title_v = 80.0f / 768.0f;
                    constexpr int k_cols = 10;
                    int key_row = 0, key_col = 0;
                    if (key < 10)      { key_row = 0; key_col = key; }
                    else if (key < 20) { key_row = 1; key_col = key - 10; }
                    else if (key < 30) { key_row = 2; key_col = key - 20; }
                    else if (key <= 36) { key_row = 3; key_col = (key < 36) ? (key - 30) : 6; }
                    float key_area = 1.0f - k_title_v;
                    float row_h = key_area / 4.0f;
                    float col_w = 1.0f / k_cols;
                    overlay.highlight.panel_idx = 0;
                    overlay.highlight.u0 = key_col * col_w;
                    overlay.highlight.u1 = (key_col + 1) * col_w;
                    overlay.highlight.v0 = k_title_v + key_row * row_h;
                    overlay.highlight.v1 = k_title_v + (key_row + 1) * row_h;
                    overlay.highlight.r = 0.20f;
                    overlay.highlight.g = 0.51f;
                    overlay.highlight.b = 0.82f;
                    overlay.highlight.alpha = 0.45f;
                }
            } else if (m_active_sub_panel == 5) {
                // Ctrlmap panel
                auto& cm = overlay.panels[0];
                cm.tex   = m_ctrlmap_panel_tex;
                cm.pose  = m_ctrlmap_panel_pose;
                cm.w     = ctrlmap_metrics.world_w;
                cm.h     = ctrlmap_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for ctrlmap
                if (m_laser_panel == k_panel_ctrlmap && m_ctrlmap_panel_hovered >= 0) {
                    constexpr float k_title_v = 88.0f / 1536.0f;
                    constexpr int k_total_rows = 18;
                    int row = m_ctrlmap_panel_hovered;
                    if (row >= 0 && row < k_total_rows) {
                        float row_h = (1.0f - k_title_v) / k_total_rows;
                        overlay.highlight.panel_idx = 0;
                        overlay.highlight.u0 = 0.0f;
                        overlay.highlight.u1 = 1.0f;
                        overlay.highlight.v0 = k_title_v + row * row_h;
                        overlay.highlight.v1 = k_title_v + (row + 1) * row_h;
                        overlay.highlight.r = 0.16f;
                        overlay.highlight.g = 0.16f;
                        overlay.highlight.b = 0.39f;
                        overlay.highlight.alpha = 0.35f;
                    }
                }
            }
        }

        overlay.show_laser    = true;
        overlay.laser_origin  = m_laser_origin;
        overlay.laser_end     = m_laser_end;
        overlay.laser_hit       = m_laser_hit;
        overlay.laser_hit_u     = m_laser_hit_u;
        overlay.laser_hit_v     = m_laser_hit_v;
        overlay.laser_hit_panel = m_laser_panel;
    } else if (m_edit_mode) {
        // Edit mode: show both controller lasers (no panels)
        overlay.show_laser  = true;
        overlay.laser_origin = m_edit_laser_r_origin;
        overlay.laser_end    = m_edit_laser_r_end;

        overlay.show_laser2   = true;
        overlay.laser2_origin = m_edit_laser_l_origin;
        overlay.laser2_end    = m_edit_laser_l_end;
    } else if (m_active_sub_panel == 2) {
        // Layers panel over live game (thumbstick shortcut, menu closed)
        auto& lp = overlay.panels[0];
        lp.tex   = m_layer_panel_tex;
        lp.pose  = m_layer_panel_pose;
        lp.w     = layer_metrics.world_w;
        lp.h     = layer_metrics.world_h;
        overlay.panel_count = 1;

        if (m_laser_panel == k_panel_layers && m_layer_panel_hovered >= 0) {
            int n = (int)m_layer_names.size();
            int totalRows = n + (is_snes_filter_capable_config(m_config) ? 3 : 2);
            int row = m_layer_panel_hovered;
            constexpr float k_title_v = 88.0f / 1280.0f;
            float row_h = totalRows > 0 ? (1.0f - k_title_v) / totalRows : 0.1f;
            overlay.highlight.panel_idx = 0;
            overlay.highlight.u0 = 0.0f;
            overlay.highlight.u1 = 1.0f;
            overlay.highlight.v0 = k_title_v + row * row_h;
            overlay.highlight.v1 = k_title_v + (row + 1) * row_h;
            overlay.highlight.r = 0.18f;
            overlay.highlight.g = 0.39f;
            overlay.highlight.b = 0.75f;
            overlay.highlight.alpha = 0.35f;
        }

        overlay.show_laser    = true;
        overlay.laser_origin  = m_laser_origin;
        overlay.laser_end     = m_laser_end;
        overlay.laser_hit       = m_laser_hit;
        overlay.laser_hit_u     = m_laser_hit_u;
        overlay.laser_hit_v     = m_laser_hit_v;
        overlay.laser_hit_panel = m_laser_panel;
    }

    const auto render_start = std::chrono::steady_clock::now();

    const VrState render_state = effective_render_state(m_vr_state);

    // Render each eye
    for (int eye = 0; eye < 2; ++eye) {
        auto& e = m_impl->eye[eye];

        uint32_t img_idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (xrAcquireSwapchainImage(e.swapchain, &ai, &img_idx) != XR_SUCCESS) continue;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (xrWaitSwapchainImage(e.swapchain, &wi) != XR_SUCCESS) {
            xrReleaseSwapchainImage(e.swapchain, nullptr); continue;
        }

        const Mat4 view = Mat4::view_from_pose(views[eye].pose);
        const Mat4 proj = Mat4::proj_from_fov(views[eye].fov, 0.05f, 100.0f);

        // Pass eye position for laser billboard
        overlay.laser_eye = views[eye].pose.position;

        // Background tint: dark green in edit mode, dark yellow in menu mode, default otherwise
        float bg_r = 0.01f, bg_g = 0.01f, bg_b = 0.02f;
        if (m_edit_mode)   { bg_r = 0.00f; bg_g = 0.04f; bg_b = 0.01f; } // dark green
        else if (m_menu_open) { bg_r = 0.04f; bg_g = 0.03f; bg_b = 0.00f; } // dark yellow

        const bool pt_active = passthrough_active();
        m_impl->renderer.render_eye(e.fbos[img_idx], view, proj, m_render_layer_refs, render_state,
                                    m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el,
                                    (m_menu_open || m_edit_mode || m_active_sub_panel == 2) ? &overlay : nullptr,
                                    bg_r, bg_g, bg_b, pt_active ? 0.0f : 1.0f);

        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(e.swapchain, &ri);

        // Fill projection view for submission
        m_impl->last_proj_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        m_impl->last_proj_views[eye].pose       = views[eye].pose;
        m_impl->last_proj_views[eye].fov        = views[eye].fov;
        m_impl->last_proj_views[eye].subImage.swapchain         = e.swapchain;
        m_impl->last_proj_views[eye].subImage.imageRect.offset  = {0, 0};
        m_impl->last_proj_views[eye].subImage.imageRect.extent  = {(int32_t)e.width, (int32_t)e.height};
    }

    const auto render_end = std::chrono::steady_clock::now();
    const float render_ms = std::chrono::duration<float, std::milli>(render_end - render_start).count();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_last_render_ms = render_ms;
        m_render_sample_count++;
        if (m_render_sample_count == 1) {
            m_avg_render_ms = render_ms;
            m_max_render_ms = render_ms;
        } else {
            const float sample_count = static_cast<float>(m_render_sample_count);
            m_avg_render_ms += (render_ms - m_avg_render_ms) / sample_count;
            m_max_render_ms = std::max(m_max_render_ms, render_ms);
        }
    }
}

// ============================================================
// Shutdown
// ============================================================
void OpenXrShell::shutdown() {
    if (!m_impl) return;

    // Free panel textures (GL context still current at this point)
    if (m_main_menu_tex)      { glDeleteTextures(1, &m_main_menu_tex);      m_main_menu_tex      = 0; }
    if (m_layer_panel_tex)    { glDeleteTextures(1, &m_layer_panel_tex);    m_layer_panel_tex    = 0; }
    if (m_settings_panel_tex) { glDeleteTextures(1, &m_settings_panel_tex); m_settings_panel_tex = 0; }
    if (m_code_panel_tex)     { glDeleteTextures(1, &m_code_panel_tex);     m_code_panel_tex     = 0; }
    if (m_ctrlmap_panel_tex)  { glDeleteTextures(1, &m_ctrlmap_panel_tex);  m_ctrlmap_panel_tex  = 0; }
    m_rom_browser.destroy_texture();

    m_impl->renderer.shutdown();

    destroy_swapchains();
    if (m_impl->lhand_space != XR_NULL_HANDLE) { xrDestroySpace(m_impl->lhand_space); m_impl->lhand_space = XR_NULL_HANDLE; }
    if (m_impl->rhand_space != XR_NULL_HANDLE) { xrDestroySpace(m_impl->rhand_space); m_impl->rhand_space = XR_NULL_HANDLE; }
    if (m_impl->laim_space  != XR_NULL_HANDLE) { xrDestroySpace(m_impl->laim_space);  m_impl->laim_space  = XR_NULL_HANDLE; }
    if (m_impl->raim_space  != XR_NULL_HANDLE) { xrDestroySpace(m_impl->raim_space);  m_impl->raim_space  = XR_NULL_HANDLE; }
    if (m_impl->view_space  != XR_NULL_HANDLE) { xrDestroySpace(m_impl->view_space);  m_impl->view_space  = XR_NULL_HANDLE; }
    if (m_impl->action_set != XR_NULL_HANDLE) { xrDestroyActionSet(m_impl->action_set); m_impl->action_set = XR_NULL_HANDLE; }
    if (m_impl->app_space  != XR_NULL_HANDLE) { xrDestroySpace(m_impl->app_space); m_impl->app_space = XR_NULL_HANDLE; }
    if (m_impl->passthrough_layer != XR_NULL_HANDLE && m_impl->pfn_destroy_passthrough_layer) {
        m_impl->pfn_destroy_passthrough_layer(m_impl->passthrough_layer);
        m_impl->passthrough_layer = XR_NULL_HANDLE;
    }
    if (m_impl->passthrough != XR_NULL_HANDLE && m_impl->pfn_destroy_passthrough) {
        m_impl->pfn_destroy_passthrough(m_impl->passthrough);
        m_impl->passthrough = XR_NULL_HANDLE;
    }
    if (m_impl->session    != XR_NULL_HANDLE) { xrDestroySession(m_impl->session); m_impl->session = XR_NULL_HANDLE; }
    if (m_impl->instance   != XR_NULL_HANDLE) { xrDestroyInstance(m_impl->instance); m_impl->instance = XR_NULL_HANDLE; }

    if (m_impl->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_impl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_impl->egl_context != EGL_NO_CONTEXT) eglDestroyContext(m_impl->egl_display, m_impl->egl_context);
        if (m_impl->egl_surface != EGL_NO_SURFACE) eglDestroySurface(m_impl->egl_display, m_impl->egl_surface);
        eglTerminate(m_impl->egl_display);
    }

    delete m_impl;
    m_impl = nullptr;
}

} // namespace qrd
