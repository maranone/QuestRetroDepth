#include "openxr_shell.h"
#include "psx_gl_context.h"
#include "psx_gpu_frame.h"
#include "audio_processor.h"
#include "button_map.h"
#include "experimental_rumble.h"
#include "imgui.h"
#include "imgui_bridge.h"
#include "mame_driver_catalog.h"
#include "mame_layer_capture.h"
#include "panel_layout.h"
#include "presentation_shared.h"
#include "rom_title_db.h"
#include "settings_io.h"
#include "vr_state_code.h"

#include <android/log.h>
#include <android/asset_manager.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <random>
#include <climits>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define LOG_TAG "QuestRetroDepthXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// TEMPORARY dev aid while iterating on ROM preview capture: wipes the entire
// preview cache every time a folder is opened, so a stale .qrp from an
// earlier build can never mask whether a capture-logic change actually
// worked. The preview pipeline is settled now, so this is off — caching
// persists across sessions/builds; use "Wipe Settings" in the main menu to
// force a full recache if ever needed.
static constexpr bool kDebugForceRecacheOnFolderOpen = false;
// Temporary test switch: keep ROM previews available but do not start the
// background thumbnail scraper or archive extraction. Set false to restore
// normal thumbnail generation; cached thumbnails remain intact either way.
static constexpr bool kDebugDisableRomThumbnailer = false;
// Superseded: "Darker" side-color mode is now the universal shipped default
// for every layer on every backend (see presentation_shared.h's
// ensure_layer_runtime_state_matches_config()), so this Neo Geo-only forced
// override is no longer needed — kept at false (a no-op) rather than
// deleted, in case a future experiment wants a per-backend forced override
// again.
static constexpr bool kDebugNeoGeoBlack3dSides = false;

namespace qrd {

extern "C" AAssetManager* qrd_get_asset_manager();

namespace {

constexpr float k_min_vr_resolution_scale = 0.25f;
constexpr float k_max_vr_resolution_scale = 4.0f;
// Manual layer panel DistPlus/ThickPlus ceilings. Previously 8.0f/1.0f — raised to an
// effectively unbounded practical limit per user request ("no limit or extremely big limit").
constexpr float k_layer_dist_max      = 1000.0f;
constexpr float k_layer_thickness_max = 1000.0f;
constexpr float k_dashboard_scale_max = 1000.0f; // was 6.0f
constexpr float k_dashboard_pos_max   = 1000.0f; // was 2.0f
// Unified menu's ★-favorited row keys. Declared up here (rather than down by
// the rest of the menu's row-primitive code, near draw_unified_menu) so
// save_settings()/load_settings() — defined earlier in this file — can
// serialize/restore it without a forward-declaration ordering problem.
std::set<std::string> s_favorites;

constexpr int k_save_state_slot_count = 3;
constexpr int k_settings_row_count = 30;
constexpr const char* k_lightgun_calibration_file = "lightgun_calibration.ini";
// VrState::side_panel_mode values, cycled via the "Side Panels" settings row.
constexpr int kSidePanelOff      = 0; // nothing shown
constexpr int kSidePanelHelp     = 1; // static instructions (the original content)
constexpr int kSidePanelSettings = 2; // real Settings panel (left) + quick-edit presets (right)
constexpr int kSidePanelPerf     = 3; // FPS/CPU/RAM/GPU
constexpr int kSidePanelBgColor  = 4; // background color/gradient picker (not yet implemented)
constexpr int kSidePanelThemes   = 5; // UI theme picker
constexpr int kSidePanelModeCount = 5;

// Number of credit rows (entries + section headers) shown on screen at
// once; the rest scroll via the right stick, same as the Homebrew list.
constexpr int kCreditsVisibleRows = 9;

// Background Color presets: 8 solid colors (id 0-7) + 8 gradients (id 8-15, {r,g,b} bottom/floor
// paired with {r,g,b} top/sky — the "typical Unity/Unreal default skybox" look). Fed into the
// existing sky-dome band renderer (see the SkyDomeInfo construction near render_state.environment
// _sphere_mode below) — band[0] = top of dome, band[11] = bottom, so gradients interpolate
// top-color at band 0 down to bottom-color at band 11.
struct BgSolidPreset { float r, g, b; };
struct BgGradientPreset { float top_r, top_g, top_b; float bot_r, bot_g, bot_b; };
static const BgSolidPreset kBgSolidPresets[8] = {
    {0.00f, 0.00f, 0.00f}, // black
    {0.05f, 0.05f, 0.06f}, // near-black gray
    {0.35f, 0.35f, 0.38f}, // mid gray
    {0.75f, 0.76f, 0.78f}, // light gray
    {0.02f, 0.05f, 0.12f}, // navy
    {0.03f, 0.10f, 0.06f}, // forest green
    {0.14f, 0.03f, 0.10f}, // deep purple
    {0.16f, 0.08f, 0.02f}, // warm brown
};
static const BgGradientPreset kBgGradientPresets[8] = {
    {0.35f, 0.60f, 0.90f,  0.15f, 0.18f, 0.20f}, // day sky -> gray floor
    {0.05f, 0.08f, 0.20f,  0.02f, 0.02f, 0.03f}, // night sky -> near-black floor
    {0.95f, 0.55f, 0.25f,  0.10f, 0.05f, 0.08f}, // sunset orange -> dark floor
    {0.70f, 0.35f, 0.80f,  0.08f, 0.04f, 0.12f}, // dusk purple -> dark floor
    {0.55f, 0.85f, 0.95f,  0.20f, 0.45f, 0.20f}, // sky blue -> grass green
    {0.90f, 0.90f, 0.85f,  0.85f, 0.82f, 0.65f}, // overcast -> sand
    {0.02f, 0.02f, 0.05f,  0.05f, 0.15f, 0.25f}, // space -> deep teal
    {0.10f, 0.12f, 0.16f,  0.35f, 0.10f, 0.10f}, // twilight -> lava red
};
constexpr const char* k_autosave_file_name = "autosave.state";
constexpr const char* k_save_automation_file_name = "save_automation.ini";

static VrState default_vr_state_for_backend(BackendKind kind) {
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
    vs.side_panel_mode = kSidePanelOff;
    vs.perspective_comp = true;
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

static const char* backend_storage_subdir(BackendKind kind) {
    switch (kind) {
    case BackendKind::Genesis: return "genesis";
    case BackendKind::Gba:     return "gba";
    case BackendKind::Gb:      return "gb";
    case BackendKind::Nes:     return "nes";
    case BackendKind::Pce:     return "pce";
    case BackendKind::Psx:     return "psx";
    case BackendKind::Sms:     return "sms";
    default:                   return "snes";
    }
}

// Adaptive hold-to-repeat interval for a Minus/Plus button held for `held_ns` nanoseconds
// continuously on the SAME button. Starts slow (deliberate single steps) and accelerates the
// longer the trigger stays down, so a quick tap still gives one precise step but a long hold
// eats a big range fast. Tiers/thresholds are arbitrary starting points — tune freely.
static XrTime adaptive_repeat_interval_ns(XrTime held_ns) {
    constexpr XrTime k_tier1_interval = 150'000'000; //  150 ms — first few seconds
    constexpr XrTime k_tier2_interval =  75'000'000; //   75 ms — after k_tier2_at
    constexpr XrTime k_tier3_interval =  25'000'000; //   25 ms — after k_tier3_at
    constexpr XrTime k_tier2_at = 3'000'000'000; // 3 s held
    constexpr XrTime k_tier3_at = 6'000'000'000; // 6 s held
    if (held_ns >= k_tier3_at) return k_tier3_interval;
    if (held_ns >= k_tier2_at) return k_tier2_interval;
    return k_tier1_interval;
}

// Active per-layer geometry modes, in cycle order. Floor/Ceiling (pure single-row tile),
// AutoYDepth, and Billboard were dismissed from the cycle — SplitFloor/SplitCeiling (renamed
// Floor/Ceiling below) already supersede the old pure Floor/Ceiling behaviour by attaching a
// floor/ceiling band to a full standing box instead of replacing the whole layer with one.
static const LayerGeometryMode kActiveGeomModes[] = {
    LayerGeometryMode::Box,
    LayerGeometryMode::Symmetric,
    LayerGeometryMode::SplitFloor,
    LayerGeometryMode::SplitCeiling,
    LayerGeometryMode::Repeat,
    LayerGeometryMode::Room,
    LayerGeometryMode::DepthScatter,
    LayerGeometryMode::SizeThickness,
};
static constexpr int kActiveGeomModeCount =
    (int)(sizeof(kActiveGeomModes) / sizeof(kActiveGeomModes[0]));

static LayerGeometryMode next_geom_mode(LayerGeometryMode cur) {
    int idx = 0;
    for (int i = 0; i < kActiveGeomModeCount; ++i) {
        if (kActiveGeomModes[i] == cur) { idx = i; break; }
    }
    return kActiveGeomModes[(idx + 1) % kActiveGeomModeCount];
}

static const char* geom_mode_label(LayerGeometryMode m) {
    switch (m) {
        case LayerGeometryMode::Box:           return "BOX";
        case LayerGeometryMode::Symmetric:     return "SYMMETRIC";
        case LayerGeometryMode::SplitFloor:    return "FLOOR";
        case LayerGeometryMode::SplitCeiling:  return "CEILING";
        case LayerGeometryMode::Repeat:        return "REPEAT";
        case LayerGeometryMode::Room:          return "ROOM";
        case LayerGeometryMode::DepthScatter:  return "SCATTER";
        case LayerGeometryMode::SizeThickness: return "SIZE";
        default:                               return "BOX";
    }
}

static std::uint64_t monotonic_time_ms() {
    using Clock = std::chrono::steady_clock;
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}

static std::string save_state_default_label(int slot) {
    char label[16];
    std::snprintf(label, sizeof(label), "LOAD %d", slot + 1);
    return label;
}

static std::string autosave_interval_label(int seconds) {
    switch (seconds) {
        case 300: return "5m";
        case 60: return "1m";
        case 30: return "30s";
        case 5: return "5s";
        default: return "Off";
    }
}

static int next_autosave_interval_seconds(int current) {
    switch (current) {
        case 0: return 300;
        case 300: return 60;
        case 60: return 30;
        case 30: return 5;
        default: return 0;
    }
}

static DepthMode cycle_depth_mode(DepthMode current, int dir) {
    static constexpr DepthMode k_order[] = {
        DepthMode::Off, DepthMode::WholeLayer, DepthMode::BoundingBox,
        DepthMode::PixelExtrude, DepthMode::PixelFx, DepthMode::ZBuffer,
    };
    constexpr int count = (int)(sizeof(k_order) / sizeof(k_order[0]));
    int idx = 0;
    for (int i = 0; i < count; ++i) {
        if (k_order[i] == current) { idx = i; break; }
    }
    idx = (idx + (dir < 0 ? -1 : 1) + count) % count;
    return k_order[idx];
}

// Keep OFF available, then start with the requested subtle 0.005 parallax step.
static constexpr float k_parallax_steps[] = {0.0f, 0.005f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};
static constexpr int   k_parallax_step_count = 7;

static const char* parallax_label(float r) {
    if (r <= 0.0f)   return "OFF";
    if (r < 0.0275f) return "1:0.005";
    if (r < 0.075f)  return "1:0.05";
    if (r < 0.175f)  return "1:0.1";
    if (r < 0.375f)  return "1:0.25";
    if (r < 0.75f)   return "1:0.5";
    return "1:1";
}

static std::string compact_settings_target(std::string name, std::size_t max_len = 18) {
    for (char& c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc >= 127) c = '?';
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() <= max_len) return name;
    if (max_len <= 3) return name.substr(0, max_len);
    return name.substr(0, max_len - 3) + "...";
}

static std::string sanitize_ascii_label(std::string text) {
    for (char& c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc >= 127) c = ' ';
    }
    std::size_t a = 0;
    while (a < text.size() && std::isspace((unsigned char)text[a])) ++a;
    std::size_t b = text.size();
    while (b > a && std::isspace((unsigned char)text[b - 1])) --b;
    text = text.substr(a, b - a);
    return text.empty() ? "Unknown" : text;
}

static std::string sanitize_ascii_filename(std::string text) {
    for (char& c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc >= 127 ||
            c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    std::size_t a = 0;
    while (a < text.size() && std::isspace((unsigned char)text[a])) ++a;
    std::size_t b = text.size();
    while (b > a && std::isspace((unsigned char)text[b - 1])) --b;
    text = text.substr(a, b - a);
    return text.empty() ? "Unknown" : text;
}

static std::string format_save_state_timestamp(std::time_t ts) {
    if (ts <= 0) return {};
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &ts);
#else
    localtime_r(&ts, &local_tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y%m%d-%H:%M", &local_tm) == 0) return {};
    return buf;
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

    // XR_FB_render_model (optional) -- lets us load the runtime's own
    // controller glTF model instead of shipping/baking our own; see
    // ControllerModel::load() in gles_renderer.cpp.
    PFN_xrEnumerateRenderModelPathsFB    pfn_enum_render_model_paths = nullptr;
    PFN_xrGetRenderModelPropertiesFB     pfn_get_render_model_props  = nullptr;
    PFN_xrLoadRenderModelFB              pfn_load_render_model       = nullptr;
    bool                                 has_render_model_ext        = false;

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
    ImGuiBridge  imgui_bridge;
    // TEMP TESTING: session-only (never saved) toggle so the old real panel
    // system can be compared against the in-progress ImGui migration while
    // it's being built — B+Y chord flips it, see render_frame()'s ImGui test
    // block. Delete alongside that block once real panel migration lands.
    // Default true: the new ImGui menu is shown and the old panel system is
    // suppressed by default (see the render_frame() guard and poll_actions()'s
    // menu-button guard, both keyed off this same flag) — the "Show Old Menu"
    // row in the ImGui menu (draw_row(), label "Show Old Menu") is the only
    // way to flip it back for comparison, session-only, never persisted.
    bool debug_show_new_ui = true;

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

static void set_hover_highlight(OverlayInfo& overlay, int panel_idx,
                                const PanelLayoutItem& item,
                                float r, float g, float b, float a) {
    overlay.highlight.panel_idx = panel_idx;
    overlay.highlight.u0 = item.rect.u0;
    overlay.highlight.u1 = item.rect.u1;
    overlay.highlight.v0 = item.rect.v0;
    overlay.highlight.v1 = item.rect.v1;
    overlay.highlight.r = r;
    overlay.highlight.g = g;
    overlay.highlight.b = b;
    overlay.highlight.alpha = a;
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

struct HelpItem {
    std::string input;
    std::string action;
};

struct HelpModel {
    std::string title;
    std::vector<HelpItem> items;

    std::string key() const {
        std::string out = title;
        out.push_back('\n');
        for (const auto& item : items) {
            out += item.input;
            out.push_back('=');
            out += item.action;
            out.push_back('\n');
        }
        return out;
    }
};

static void add_help(HelpModel& model, const char* input, const char* action) {
    model.items.push_back({input, action});
}

static void add_mapped_game_controls(HelpModel& model, BackendKind kind, const ButtonMap& button_map) {
    for (int i = 0; i < SNES_BUTTON_COUNT; ++i) {
        std::string action = std::string(button_name_for_backend(kind, i)) + " game input";
        add_help(model, qi_name(button_map[i]), action.c_str());
    }
}

static XrVector3f vec_cross(const XrVector3f& a, const XrVector3f& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static XrVector3f vec_scale(const XrVector3f& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

static XrVector3f vec_add(const XrVector3f& a, const XrVector3f& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static XrVector3f vec_normalize(XrVector3f v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 0.0001f) return {0.0f, 0.0f, 1.0f};
    return {v.x / len, v.y / len, v.z / len};
}

static void pose_axes(const XrPosef& pose, XrVector3f& right, XrVector3f& up, XrVector3f& normal) {
    const XrQuaternionf& q = pose.orientation;
    right.x = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    right.y = 2.0f * (q.x * q.y + q.w * q.z);
    right.z = 2.0f * (q.x * q.z - q.w * q.y);
    up.x = 2.0f * (q.x * q.y - q.w * q.z);
    up.y = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    up.z = 2.0f * (q.y * q.z + q.w * q.x);
    normal.x = 2.0f * (q.x * q.z + q.w * q.y);
    normal.y = 2.0f * (q.y * q.z - q.w * q.x);
    normal.z = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
}

// Yaw-locked pose centred in front of the current head pose (upright, ignores
// pitch/roll). Same math as open_rom_menu()'s main-menu placement — used so
// wing-style panels anchor to where the player is actually looking instead of
// a stale/default-identity pose.
static XrPosef yaw_locked_pose_in_front(const XrPosef& hmd_pose, float dist) {
    const XrQuaternionf& q = hmd_pose.orientation;
    const XrVector3f&    p = hmd_pose.position;

    float siny = 2.0f * (q.w * q.y + q.x * q.z);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
    float yaw  = std::atan2f(siny, cosy);

    XrPosef out{};
    out.orientation = { 0.0f, std::sinf(yaw * 0.5f), 0.0f, std::cosf(yaw * 0.5f) };

    const float fwd_x = -std::sinf(yaw);
    const float fwd_z = -std::cosf(yaw);
    out.position = { p.x + fwd_x * dist, p.y, p.z + fwd_z * dist };
    return out;
}

static XrQuaternionf quat_from_axes(XrVector3f right, XrVector3f up, XrVector3f normal) {
    right = vec_normalize(right);
    up = vec_normalize(up);
    normal = vec_normalize(normal);

    const float m00 = right.x,  m01 = up.x,  m02 = normal.x;
    const float m10 = right.y,  m11 = up.y,  m12 = normal.y;
    const float m20 = right.z,  m21 = up.z,  m22 = normal.z;
    XrQuaternionf q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static XrVector3f quat_rotate_vec(const XrQuaternionf& q, const XrVector3f& v) {
    // v' = v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v)
    const XrVector3f qv{ q.x, q.y, q.z };
    XrVector3f t;
    t.x = qv.y*v.z - qv.z*v.y + q.w*v.x;
    t.y = qv.z*v.x - qv.x*v.z + q.w*v.y;
    t.z = qv.x*v.y - qv.y*v.x + q.w*v.z;
    XrVector3f out;
    out.x = v.x + 2.0f*(qv.y*t.z - qv.z*t.y);
    out.y = v.y + 2.0f*(qv.z*t.x - qv.x*t.z);
    out.z = v.z + 2.0f*(qv.x*t.y - qv.y*t.x);
    return out;
}

static XrQuaternionf quat_multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    // a * b (apply b first, then a) -- Hamilton product.
    XrQuaternionf out;
    out.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    out.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    out.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    out.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return out;
}

// Shortest-arc rotation that maps unit vector `from` onto unit vector `to`.
// Used by the lightgun recenter action: computes the single corrective
// rotation (equivalent to a combined yaw+pitch delta, no roll) needed so the
// controller's current raw aim direction lines up with the game-screen
// center, then that same delta is applied to every later frame's raw aim
// and to the gun model's render orientation, so the model visually keeps
// pointing at the declared center as the physical controller angle changes.
static XrQuaternionf quat_from_to(const XrVector3f& from, const XrVector3f& to) {
    const float dot = std::clamp(from.x*to.x + from.y*to.y + from.z*to.z, -1.0f, 1.0f);
    if (dot > 0.999999f) return {0,0,0,1};
    if (dot < -0.999999f) {
        // 180 degrees: pick any axis orthogonal to `from`.
        XrVector3f axis = std::abs(from.x) < 0.9f ? XrVector3f{1,0,0} : XrVector3f{0,1,0};
        const float d = axis.x*from.x + axis.y*from.y + axis.z*from.z;
        axis = { axis.x - from.x*d, axis.y - from.y*d, axis.z - from.z*d };
        axis = vec_normalize(axis);
        return { axis.x, axis.y, axis.z, 0.0f };
    }
    XrVector3f axis;
    axis.x = from.y*to.z - from.z*to.y;
    axis.y = from.z*to.x - from.x*to.z;
    axis.z = from.x*to.y - from.y*to.x;
    XrQuaternionf q{ axis.x, axis.y, axis.z, 1.0f + dot };
    const float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (len > 0.00001f) { q.x /= len; q.y /= len; q.z /= len; q.w /= len; }
    return q;
}

static bool game_canvas_anchor_pose(const std::vector<LayerFrame*>& frames,
                                    const GameConfig& config,
                                    float canvas_x,
                                    float canvas_y,
                                    float canvas_az,
                                    float canvas_el,
                                    float canvas_scale,
                                    XrPosef& pose_out,
                                    float& width_out) {
    float depth = 1.5f;
    float width = 2.56f;
    bool have = false;
    const LayerFrame* reference_frame = nullptr;
    const LayerConfig* reference_config = nullptr;
    for (const LayerFrame* frame : frames) {
        if (!frame) continue;
        if (!reference_frame || frame->depth_meters < reference_frame->depth_meters) reference_frame = frame;
        have = true;
    }
    if (!have) {
        for (const auto& layer : config.layers) {
            if (!reference_config || layer.depth_meters < reference_config->depth_meters) reference_config = &layer;
            have = true;
        }
    }
    if (!have) return false;

    // Depth and width must come from the same visual layer. Mixing the nearest
    // depth with the widest (often perspective-compensated far-layer) width
    // produces a plane that is centered correctly but whose corners miss the
    // screen. The renderer also uses one coherent layer descriptor at the
    // front of the stack for this anchor.
    depth = reference_frame ? reference_frame->depth_meters :
            (reference_config ? reference_config->depth_meters : depth);
    width = reference_frame ? reference_frame->quad_width_meters :
            (reference_config ? reference_config->quad_width_meters : width);

    const float cos_el = std::cos(canvas_el);
    const float sin_el = std::sin(canvas_el);
    const float cos_az = std::cos(canvas_az);
    const float sin_az = std::sin(canvas_az);
    const XrVector3f normal = {sin_az * cos_el, sin_el, -cos_az * cos_el};
    const XrVector3f right  = {cos_az, 0.0f, sin_az};
    const XrVector3f up     = {-sin_az * sin_el, cos_el, -cos_az * sin_el};

    pose_out.position = {
        depth * sin_az * cos_el + canvas_x,
        depth * sin_el + canvas_y,
       -depth * cos_az * cos_el
    };
    pose_out.orientation = quat_from_axes(right, up, normal);
    width_out = width * canvas_scale;
    return true;
}

static bool game_canvas_lightgun_surface(const std::vector<LayerFrame*>& frames,
                                         const GameConfig& config,
                                         float canvas_x,
                                         float canvas_y,
                                         float canvas_az,
                                         float canvas_el,
                                         float canvas_scale,
                                         const VrState& render_state,
                                         int frame_width,
                                         int frame_height,
                                         LightgunSurface& surface_out) {
    XrPosef anchor_pose{};
    float canvas_w = 0.0f;
    if (!game_canvas_anchor_pose(frames, config, canvas_x, canvas_y, canvas_az, canvas_el,
                                 canvas_scale, anchor_pose, canvas_w) ||
        frame_width <= 0 || frame_height <= 0) return false;

    // Match the renderer's integer upscale/pixel-grid snap. The reference
    // layer used by the anchor is the first nearest layer, which is also the
    // grid source selected by render_eye().
    float effective_x = canvas_x;
    float effective_y = canvas_y;
    float effective_scale = canvas_scale;
    if (render_state.upscale_mode != UpscaleMode::Off) {
        for (const LayerFrame* frame : frames) {
            if (!frame || frame->width <= 0 || frame->quad_width_meters <= 0.0f) continue;
            effective_scale = std::max(1.0f, std::round(canvas_scale));
            const float grid = (frame->quad_width_meters / (float)frame->width) * effective_scale;
            if (grid > 0.0f) {
                effective_x = std::round(canvas_x / grid) * grid;
                effective_y = std::round(canvas_y / grid) * grid;
            }
            break;
        }
    }
    if (std::abs(effective_scale - canvas_scale) > 0.0001f ||
        std::abs(effective_x - canvas_x) > 0.0001f ||
        std::abs(effective_y - canvas_y) > 0.0001f) {
        if (!game_canvas_anchor_pose(frames, config, effective_x, effective_y, canvas_az, canvas_el,
                                     effective_scale, anchor_pose, canvas_w)) return false;
    }

    XrVector3f base_right{}, base_up{}, base_normal{};
    pose_axes(anchor_pose, base_right, base_up, base_normal);
    surface_out.right = base_right;
    surface_out.up = base_up;
    surface_out.normal = base_normal;
    surface_out.center = anchor_pose.position;
    surface_out.width = canvas_w;
    surface_out.height = canvas_w * (float)frame_height / (float)frame_width;
    surface_out.screen_curve = render_state.immersive_beta_enabled
        ? std::clamp(render_state.screen_curve, -1.0f, 1.0f) : 0.0f;

    if (render_state.immersive_beta_enabled) {
        const float cx = std::cos(render_state.tilt_x);
        const float sx = std::sin(render_state.tilt_x);
        const float cy = std::cos(render_state.tilt_y);
        const float sy = std::sin(render_state.tilt_y);
        const XrVector3f pitched_up = lightgun_add(lightgun_scale(base_up, cx),
                                                    lightgun_scale(base_normal, sx));
        const XrVector3f pitched_normal = lightgun_add(lightgun_scale(base_normal, cx),
                                                        lightgun_scale(base_up, -sx));
        // This is the same order as kImmersiveLayerVS: pitch around the
        // horizontal screen axis, then yaw the pitched frame around Y.
        surface_out.right = lightgun_add(lightgun_scale(base_right, cy),
                                         lightgun_scale(pitched_normal, -sy));
        surface_out.up = pitched_up;
        surface_out.normal = lightgun_add(lightgun_scale(base_right, sy),
                                          lightgun_scale(pitched_normal, cy));
    }
    return true;
}

struct LiveLayerSurface {
    XrVector3f center = {0, 0, 0};
    XrVector3f right = {1, 0, 0};
    XrVector3f up = {0, 1, 0};
    XrVector3f normal = {0, 0, -1};
    float width = 0.0f;
    float height = 0.0f;
};

// CPU copy of the layer front-face placement used by kLayerVS/kBoxLayerVS.
// It intentionally describes the real game layer plane, not an ImGui panel.
static bool build_live_layer_surface(const LayerFrame& frame,
                                     const VrState& render_state,
                                     float canvas_x,
                                     float canvas_y,
                                     float canvas_az,
                                     float canvas_el,
                                     float canvas_scale,
                                     LiveLayerSurface& out,
                                     // Layer-deck bookshelf yaw about the
                                     // layer's own vertical axis; see
                                     // presentation::layer_deck_yaw and the
                                     // matching uLayerYaw in kLayerVS.
                                     float layer_yaw = 0.0f) {
    if (frame.width <= 0 || frame.height <= 0 || frame.quad_width_meters <= 0.0f) return false;

    const int rotate_mode = render_state.rotate_screen & 3;
    const bool rotate_odd = rotate_mode == 1 || rotate_mode == 3;
    const float qw = frame.quad_width_meters;
    const float qh = rotate_odd
        ? qw * (float)frame.width / (float)frame.height
        : qw * (float)frame.height / (float)frame.width;

    if (render_state.surface_mode != 0) {
        // MUST match the table/ceiling branches in kLayerVS, kImmersiveLayerVS
        // and kBoxLayerVS. This is what the laser is actually tested against,
        // so if it drifts from the shaders the pointer stops lining up with
        // the picture.
        constexpr float kTableDistance = 1.15f;
        constexpr float kTableHeight   = -0.62f;
        constexpr float kCeilingHeight =  0.78f;
        const bool ceiling = (render_state.surface_mode == 2);
        const float t_cos = std::cos(canvas_az);
        const float t_sin = std::sin(canvas_az);
        out.center = {kTableDistance * t_sin + canvas_x,
                      (ceiling ? kCeilingHeight : kTableHeight) + canvas_y,
                      -kTableDistance * t_cos};
        out.right  = {t_cos, 0.0f, t_sin};
        out.up     = ceiling ? XrVector3f{-t_sin, 0.0f, t_cos}
                             : XrVector3f{ t_sin, 0.0f, -t_cos};
        out.normal = {0.0f, ceiling ? -1.0f : 1.0f, 0.0f};
    } else {
        const float cos_el = std::cos(canvas_el);
        const float sin_el = std::sin(canvas_el);
        const float cos_az = std::cos(canvas_az);
        const float sin_az = std::sin(canvas_az);
        XrVector3f base_normal = {sin_az * cos_el, sin_el, -cos_az * cos_el};
        XrVector3f base_right = {cos_az, 0.0f, sin_az};
        const XrVector3f base_up = {-sin_az * sin_el, cos_el, -cos_az * sin_el};

        // Centre comes from the UN-yawed normal: the bookshelf yaw turns the
        // layer in place, it never moves it along the sphere.
        out.center = {
            frame.depth_meters * base_normal.x + canvas_x,
            frame.depth_meters * base_normal.y + canvas_y,
            frame.depth_meters * base_normal.z,
        };

        if (layer_yaw != 0.0f) {
            const float cyw = std::cos(layer_yaw);
            const float syw = std::sin(layer_yaw);
            const XrVector3f yawed_right = {
                base_right.x * cyw - base_normal.x * syw,
                base_right.y * cyw - base_normal.y * syw,
                base_right.z * cyw - base_normal.z * syw,
            };
            base_normal = {
                base_right.x * syw + base_normal.x * cyw,
                base_right.y * syw + base_normal.y * cyw,
                base_right.z * syw + base_normal.z * cyw,
            };
            base_right = yawed_right;
        }
        out.right = base_right;
        out.up = base_up;
        out.normal = base_normal;

        if (render_state.immersive_beta_enabled || render_state.permacurve) {
            const float ctx = std::cos(render_state.tilt_x);
            const float stx = std::sin(render_state.tilt_x);
            const XrVector3f pitched_up = {
                base_up.x * ctx + base_normal.x * stx,
                base_up.y * ctx + base_normal.y * stx,
                base_up.z * ctx + base_normal.z * stx,
            };
            const XrVector3f pitched_normal = {
                base_normal.x * ctx - base_up.x * stx,
                base_normal.y * ctx - base_up.y * stx,
                base_normal.z * ctx - base_up.z * stx,
            };
            const float cty = std::cos(render_state.tilt_y);
            const float sty = std::sin(render_state.tilt_y);
            out.right = {
                base_right.x * cty - pitched_normal.x * sty,
                base_right.y * cty - pitched_normal.y * sty,
                base_right.z * cty - pitched_normal.z * sty,
            };
            out.up = pitched_up;
            out.normal = {
                base_right.x * sty + pitched_normal.x * cty,
                base_right.y * sty + pitched_normal.y * cty,
                base_right.z * sty + pitched_normal.z * cty,
            };
        }
    }
    out.width = qw * canvas_scale;
    out.height = qh * canvas_scale;
    return out.width > 0.0f && out.height > 0.0f;
}

static bool intersect_live_layer_surface(const LiveLayerSurface& surface,
                                         const XrVector3f& origin,
                                         const XrVector3f& direction,
                                         float& out_screen_u,
                                         float& out_screen_v,
                                         float& out_distance,
                                         XrVector3f& out_hit) {
    const float denom = direction.x * surface.normal.x +
                        direction.y * surface.normal.y +
                        direction.z * surface.normal.z;
    if (std::abs(denom) < 0.001f) return false;
    const XrVector3f delta = {
        surface.center.x - origin.x,
        surface.center.y - origin.y,
        surface.center.z - origin.z,
    };
    const float distance = (delta.x * surface.normal.x +
                           delta.y * surface.normal.y +
                           delta.z * surface.normal.z) / denom;
    if (distance <= 0.01f || distance >= 8.0f) return false;

    out_hit = {
        origin.x + direction.x * distance,
        origin.y + direction.y * distance,
        origin.z + direction.z * distance,
    };
    const XrVector3f from_center = {
        out_hit.x - surface.center.x,
        out_hit.y - surface.center.y,
        out_hit.z - surface.center.z,
    };
    const float local_x = from_center.x * surface.right.x +
                          from_center.y * surface.right.y +
                          from_center.z * surface.right.z;
    const float local_y = from_center.x * surface.up.x +
                          from_center.y * surface.up.y +
                          from_center.z * surface.up.z;
    if (std::abs(local_x) > surface.width * 0.5f ||
        std::abs(local_y) > surface.height * 0.5f) return false;
    out_screen_u = local_x / surface.width + 0.5f;
    out_screen_v = 0.5f - local_y / surface.height;
    out_distance = distance;
    return true;
}

static void add_help_wings(OverlayInfo& overlay,
                           GLuint tex,
                           const XrPosef& anchor,
                           const XrPosef& hmd_pose,
                           const PanelMetrics& help_metrics) {
    if (!tex || overlay.panel_count + 2 > OverlayInfo::k_max_panels) return;
    XrVector3f anchor_right{}, anchor_up{}, anchor_normal{};
    pose_axes(anchor, anchor_right, anchor_up, anchor_normal);

    constexpr float k_help_distance_m = 1.1f;
    const XrVector3f hmd_pos = hmd_pose.position;
    const XrVector3f world_up = {0.0f, 1.0f, 0.0f};

    auto add_wing = [&](bool left) {
        const XrVector3f wing_normal = left ? anchor_right : vec_scale(anchor_right, -1.0f);
        const XrVector3f wing_right = vec_cross(world_up, wing_normal);
        const XrVector3f side = left ? vec_scale(anchor_right, -k_help_distance_m)
                                     : vec_scale(anchor_right,  k_help_distance_m);
        XrPosef pose{};
        pose.position = vec_add(hmd_pos, side);
        pose.orientation = quat_from_axes(wing_right, world_up, wing_normal);
        auto& panel = overlay.panels[overlay.panel_count++];
        panel.tex = tex;
        panel.pose = pose;
        panel.w = help_metrics.world_w;
        panel.h = help_metrics.world_h;
    };

    add_wing(true);
    add_wing(false);
}

// Computes the world-space poses for the two manual-dashboard wing panels
// (left = dashboard controls, right = layer management), 1.1m to either side
// of the current head position. Called once per frame from the main input/
// update path so the stored poses (used for laser hit-testing) always match
// what gets rendered this same frame via add_dashboard_wings() below.
static void compute_dashboard_wing_poses(const XrPosef& anchor, const XrPosef& hmd_pose,
                                          XrPosef& left_pose, XrPosef& right_pose) {
    XrVector3f anchor_right{}, anchor_up{}, anchor_normal{};
    pose_axes(anchor, anchor_right, anchor_up, anchor_normal);

    constexpr float k_dashboard_distance_m = 1.1f;
    const XrVector3f hmd_pos = hmd_pose.position;
    const XrVector3f world_up = {0.0f, 1.0f, 0.0f};

    // Left wing (dashboard left panel)
    {
        const XrVector3f wing_normal = anchor_right;
        const XrVector3f wing_right = vec_cross(world_up, wing_normal);
        const XrVector3f side = vec_scale(anchor_right, -k_dashboard_distance_m);
        left_pose.position = vec_add(hmd_pos, side);
        left_pose.orientation = quat_from_axes(wing_right, world_up, wing_normal);
    }

    // Right wing (layer panel)
    {
        const XrVector3f wing_normal = vec_scale(anchor_right, -1.0f);
        const XrVector3f wing_right = vec_cross(world_up, wing_normal);
        const XrVector3f side = vec_scale(anchor_right, k_dashboard_distance_m);
        right_pose.position = vec_add(hmd_pos, side);
        right_pose.orientation = quat_from_axes(wing_right, world_up, wing_normal);
    }
}

// Places the always-visible Side Panels mode-select bar below eye level, in front of the player
// along the direction from the headset toward the game canvas (so it stays roughly in front
// during normal play, the same convention the help/dashboard wings use), facing back at the
// viewer. Called every frame so it follows the player around.
// Two copies of the Side Panels mode-select bar, sitting at the SAME left/right offset as the
// Help/Settings/Perf side wings (add_help_wings' own placement), just dropped down below them —
// so it reads as "the wings, lower down" rather than a separate front-and-center HUD element.
// Either copy can be clicked; they always show identical content.
static void compute_side_bar_wing_poses(const XrPosef& anchor, const XrPosef& hmd_pose,
                                         XrPosef& left_pose, XrPosef& right_pose,
                                         float drop_m = 0.55f) {
    constexpr float k_wing_distance_m = 1.1f; // matches k_help_distance_m in add_help_wings
    // drop_m defaults to 0.55m below eye level, matching the original Side
    // Panels mode-select bar this was written for — but the new unified menu's
    // Left/Right Side placement mode reuses this same horizontal wing-offset
    // math and needs to sit at eye level (same height as Follow Headset), so it
    // passes drop_m=0 explicitly rather than inheriting that bar's own drop.
    XrVector3f anchor_right, anchor_up, anchor_normal;
    pose_axes(anchor, anchor_right, anchor_up, anchor_normal);

    const XrVector3f hmd_pos = hmd_pose.position;
    const XrVector3f world_up = {0.0f, 1.0f, 0.0f};

    auto make_wing = [&](bool left) -> XrPosef {
        const XrVector3f wing_normal = left ? anchor_right : vec_scale(anchor_right, -1.0f);
        const XrVector3f wing_right  = vec_cross(world_up, wing_normal);
        const XrVector3f side = left ? vec_scale(anchor_right, -k_wing_distance_m)
                                      : vec_scale(anchor_right,  k_wing_distance_m);
        XrPosef pose{};
        pose.position = { hmd_pos.x + side.x, hmd_pos.y + side.y - drop_m, hmd_pos.z + side.z };
        pose.orientation = quat_from_axes(wing_right, world_up, wing_normal);
        return pose;
    };

    left_pose  = make_wing(true);
    right_pose = make_wing(false);
}

static void add_dashboard_wings(OverlayInfo& overlay,
                                GLuint left_tex,
                                const PanelMetrics& left_metrics,
                                GLuint right_tex,
                                const PanelMetrics& right_metrics,
                                const XrPosef& left_pose,
                                const XrPosef& right_pose) {
    if (!left_tex || !right_tex || overlay.panel_count + 2 > OverlayInfo::k_max_panels) return;

    {
        auto& panel = overlay.panels[overlay.panel_count++];
        panel.tex = left_tex;
        panel.pose = left_pose;
        panel.w = left_metrics.world_w;
        panel.h = left_metrics.world_h;
    }
    {
        auto& panel = overlay.panels[overlay.panel_count++];
        panel.tex = right_tex;
        panel.pose = right_pose;
        panel.w = right_metrics.world_w;
        panel.h = right_metrics.world_h;
    }
}

static HelpModel build_help_model(bool menu_open,
                                  int active_sub_panel,
                                  bool ctrlmap_mode,
                                  bool edit_mode,
                                  BackendKind backend_kind,
                                  const ButtonMap& button_map) {
    HelpModel model;
    if (edit_mode) {
        model.title = "Edit controls";
        add_help(model, "Left thumbstick click", "Leave manual edit and return to the game.");
        add_help(model, "Left controller aim", "Move the canvas left, right, up, and down.");
        add_help(model, "Right controller aim", "Rotate and reposition the canvas around you.");
        add_help(model, "Right stick X", "Change layer depth spread. Right is more spread, left is less spread.");
        add_help(model, "Right trigger", "Move layers closer.");
        add_help(model, "Right grip", "Move layers farther away.");
        add_help(model, "Left trigger", "Make the screen wider.");
        add_help(model, "Left grip", "Make the screen narrower.");
        add_help(model, "Left stick Y", "Change overall screen size without changing distance. Up is bigger, down is smaller.");
        add_help(model, "Left stick X", "Change duplicate copy count. Right is more copies, left is fewer copies.");
        add_help(model, "Right stick click", "Toggle passthrough.");
        add_help(model, "Left menu button", "Exit edit mode and open the main menu.");
        return model;
    }

    if (!menu_open && active_sub_panel == 7) {
        model.title = "Quick edit controls";
        add_help(model, "Right controller aim", "Point at a quick settings, layers, or manual button.");
        add_help(model, "Right trigger", "Apply the pointed preset or open the pointed manual editor.");
        add_help(model, "Left thumbstick click", "Close the quick edit panel and return to the game.");
        add_help(model, "Left menu button", "Open the main menu.");
        return model;
    }

    if (!menu_open && active_sub_panel == 2) {
        model.title = "Layer controls";
        add_help(model, "Right controller aim", "Point at a layer row or action row.");
        add_help(model, "Right trigger", "Select the pointed row.");
        add_help(model, "Right trigger on a layer", "Drag the layer, toggle visibility, or toggle ambilight depending on the pointed zone.");
        add_help(model, "Release right trigger", "Drop a grabbed layer at the pointed row.");
        add_help(model, "Play/Pause row", "Freeze or resume the emulator while editing layers.");
        add_help(model, "Auto Dup row", "Cycle automatic duplicate depth percentages.");
        add_help(model, "Layer Filter row", "Cycle the layer extraction mode when available.");
        add_help(model, "Left thumbstick click", "Return to the game.");
        add_help(model, "Left menu button", "Open the main menu.");
        return model;
    }

    if (!menu_open && active_sub_panel == 3) {
        model.title = "Settings controls";
        add_help(model, "Right controller aim", "Point at a setting or action row.");
        add_help(model, "Right trigger", "Toggle bool rows or press the pointed minus, plus, or action zone.");
        add_help(model, "Hold right trigger + right stick X", "Continuously adjust numeric rows.");
        add_help(model, "Back row", "Return to the quick edit panel when opened from Quick Edit.");
        add_help(model, "Left thumbstick click", "Return to the game.");
        add_help(model, "Left menu button", "Open the main menu.");
        return model;
    }

    if (menu_open) {
        if (ctrlmap_mode || active_sub_panel == 6) {
            model.title = "Mapping controls";
            add_help(model, "Right controller aim", "Point at a button row or action row.");
            add_help(model, "Right trigger", "Select or activate the pointed row.");
            add_help(model, "Right trigger on a button", "Select that emulated button for remapping.");
            add_help(model, "Right stick X/Y", "Cycle the selected button through Quest inputs.");
            add_help(model, "Reset Defaults", "Restore the default map for the current backend.");
            add_help(model, "Load/Save rows", "Load or save game/global settings, including button mappings.");
            add_help(model, "Back row or left menu", "Return to the main menu.");
            return model;
        }

        switch (active_sub_panel) {
        case 0:
            model.title = "Main menu controls";
            add_help(model, "Right controller aim", "Point at a menu row.");
            add_help(model, "Right trigger", "Open the pointed menu item.");
            add_help(model, "Open ROM", "Show the ROM browser.");
            add_help(model, "Settings", "Show visual and performance settings.");
            add_help(model, "Mappings", "Show controller mappings.");
            add_help(model, "Save States", "Open the per-game save-state panel.");
            add_help(model, "Left menu button", "Close the menu and return to the game.");
            break;
        case 1:
            model.title = "ROM browser controls";
            add_help(model, "Right controller aim", "Point at a folder or ROM.");
            add_help(model, "Right trigger", "Open the pointed folder or load the pointed ROM.");
            add_help(model, "Either stick Y", "Scroll the ROM list one row at a time.");
            add_help(model, "Either stick X", "Jump one page through the ROM list.");
            add_help(model, "Code panel", "Point and trigger alphanumeric keys to enter a share code.");
            add_help(model, "Left menu button", "Return to the main menu.");
            break;
        case 2:
            model.title = "Layer controls";
            add_help(model, "Right controller aim", "Point at a layer row or action row.");
            add_help(model, "Right trigger", "Select the pointed row.");
            add_help(model, "Release right trigger", "Drop a grabbed layer at the pointed row.");
            add_help(model, "Play/Pause row", "Freeze or resume the emulator while editing layers.");
            add_help(model, "Auto Dup row", "Cycle automatic duplicate depth percentages.");
            add_help(model, "Layer Filter row", "Cycle the layer extraction mode when available.");
            add_help(model, "Left menu button", "Return to the main menu.");
            break;
        case 3:
            model.title = "Settings controls";
            add_help(model, "Right controller aim", "Point at a setting or action row.");
            add_help(model, "Right trigger", "Toggle bool rows or press the pointed minus/plus/action zone.");
            add_help(model, "Hold right trigger + right stick X", "Continuously adjust numeric rows.");
            add_help(model, "Settings action rows", "Run the named settings save/load/reset action.");
            add_help(model, "Back row or left menu", "Return to the main menu.");
            break;
        case 4:
            model.title = "Save-state controls";
            add_help(model, "Right controller aim", "Point at a save slot or automation row.");
            add_help(model, "Right trigger", "Load the selected top-row slot, save into the selected bottom-row slot, or cycle the pointed automation row.");
            add_help(model, "Top row", "Shows timestamp labels for occupied slots and disabled LOAD labels for empty slots.");
            add_help(model, "Bottom row", "Always overwrites SAVE 1, SAVE 2, or SAVE 3 immediately.");
            add_help(model, "Autosave / Load Last Save rows", "Change global startup and autosave behavior.");
            add_help(model, "Left menu button", "Return to the main menu.");
            break;
        case 5:
            model.title = "Code entry controls";
            add_help(model, "Right controller aim", "Point at a key.");
            add_help(model, "Right trigger", "Type the pointed key.");
            add_help(model, "Backspace key", "Delete the last typed character.");
            add_help(model, "Complete valid code", "Apply the decoded state automatically.");
            add_help(model, "Left menu button", "Return to the main menu.");
            break;
        default:
            model.title = "Panel controls";
            add_help(model, "Right controller aim", "Point at panel rows.");
            add_help(model, "Right trigger", "Activate the pointed row.");
            add_help(model, "Left menu button", "Return to the main menu or game.");
            break;
        }
        return model;
    }

    model.title = (backend_kind == BackendKind::Genesis) ? "Genesis game controls" :
                  (backend_kind == BackendKind::Gba)     ? "GBA game controls" :
                  (backend_kind == BackendKind::Gb)      ? "GB/GBC game controls" :
                                                           "SNES game controls";
    add_mapped_game_controls(model, backend_kind, button_map);
    add_help(model, "Right stick directions", "Also act as D-pad directions unless those right-stick directions are remapped.");
    add_help(model, "Hold left grip", "Enter free roam to reposition the game environment (disabled while Lightgun is in the left hand).");
    add_help(model, "Hold right grip in free roam", "Grab the layer under the right laser; only that layer blinks and other controls are locked.");
    add_help(model, "Either thumbstick left/right", "While holding right grip, move the selected layer one stack slot per stick movement.");
    add_help(model, "Left thumbstick click", "Open quick edit.");
    add_help(model, "Left thumbstick hold (2s)", "Force lightgun mode on/off and reset the game, for scope/gun ROMs not auto-detected.");
    add_help(model, "Left menu button", "Open the main menu.");
    add_help(model, "Right stick click", "Recenter the screen to your current view.");
    return model;
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

// Neo Geo (snk/neogeo.cpp). Generated from the driver's own GAME() list -- the
// minimal set of shortname prefixes covering all 282 machines it defines, so
// clones match their parent's entry. Unlike the families above, Neo Geo has no
// hardware layers to capture, so it uses the synthesized-z-buffer config (see
// GameConfig::make_default_mame_neogeo()) and needs DepthMode::ZBuffer.
static bool is_neogeo_mame_rom_name(const std::string& rom_name) {
    static const char* const kPrefixes[] = {
        "2020bb", "3countb", "alpham2", "androdun", "aodk", "aof", "b2b", "bakatono",
        "bangbead", "bjourney", "blazstar", "breakers", "breakrev", "bstars", "burningf",
        "crswd2bl", "crsword", "ct2k3sa", "ct2k3sp", "cthd2003", "ctomaday", "cyberlip",
        "diggerma", "doubledr", "dragonsh", "eightman", "fatfursp", "fatfury1", "fatfury2",
        "fatfury3", "fbfrenzy", "fightfev", "flipshot", "froman2b", "fswords", "galaxyfg",
        "ganryu", "garou", "ghostlop", "goalx3", "gowcaizr", "gpilots", "gururin", "ironclad",
        "irrmaze", "janshin", "jockeygp", "joyjoy", "kabukikl", "karnovr", "kf10thep",
        "kf2k2mp", "kf2k2pla", "kf2k2pls", "kf2k3bl", "kf2k3pl", "kf2k3upl", "kf2k5uni",
        "kizuna", "kof10th", "kof2000", "kof2001", "kof2002", "kof2003", "kof2k4se", "kof94",
        "kof95", "kof96", "kof97", "kof98", "kof99", "kog", "kotm", "lans2004", "lastblad",
        "lastbld2", "lasthope", "lastsold", "lbowling", "legendos", "lresort", "magdrop2",
        "magdrop3", "maglord", "mahretsu", "marukodq", "matrim", "miexchng", "minasan",
        "moshougi", "ms4plus", "ms5plus", "mslug", "mutnat", "mvstemp", "nam1975", "ncombat",
        "ncommand", "neobombe", "neocup98", "neodrift", "neomrdo", "ninjamas", "nitd",
        "overtop", "panicbom", "pbobbl2n", "pbobblen", "pgoal", "pnyaa", "popbounc", "preisle2",
        "pspikes2", "pulstar", "puzzldpr", "puzzledp", "quizdai2", "quizdais", "quizkof",
        "ragnagrd", "rbff1", "rbff2", "rbffspec", "ridhero", "roboarmy", "rotd", "s1945p",
        "samsh5sp", "samsho", "savagere", "sbp", "sdodgeb", "sengoku", "shocktr2", "shocktro",
        "socbrawl", "sonicwi2", "sonicwi3", "spinmast", "ssideki", "stakwin", "strhoop",
        "superspy", "svc", "tophuntr", "tpgolf", "trally", "turfmast", "twinspri", "twsoc96",
        "viewpoin", "vliner", "wakuwak7", "wh1", "wh2", "whp", "wjammers", "zedblade",
        "zintrckb", "zupapa",
    };
    for (const char* prefix : kPrefixes) {
        if (rom_name.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

static GameConfig config_for_mame_rom_name(const std::string& rom_name) {
    // Console media names are not MAME shortnames. The official Saturn root
    // is authoritative, so "Panzer Dragoon.cue" and similarly named files
    // still receive the Saturn source-plane profile.
    std::string lowered_path = rom_name;
    for (char& c : lowered_path) c = (char)std::tolower((unsigned char)c);
    if (lowered_path.find("/roms/saturn/") != std::string::npos ||
        lowered_path.find("\\roms\\saturn\\") != std::string::npos)
        return GameConfig::make_default_mame_saturn();

    // The ROM loader supplies the archive filename (for example "tmnt.zip"),
    // while the driver catalog contains MAME shortnames ("tmnt"). Normalize
    // to a lowercase basename/stem before both Neo Geo and hardware-family
    // matching. Neo Geo's prefix matching happened to tolerate the extension;
    // the exact-match CPS/Konami/etc. tables did not and fell back to flat 2D.
    std::string short_name = rom_name;
    const auto slash = short_name.find_last_of("/\\");
    if (slash != std::string::npos) short_name.erase(0, slash + 1);
    const auto dot = short_name.find_last_of('.');
    if (dot != std::string::npos) short_name.erase(dot);
    for (char& c : short_name)
        c = (char)std::tolower((unsigned char)c);

    const auto classification = qrd::classify_mame_driver(short_name);
    switch (classification.profile) {
    case qrd::MameLayerProfile::Cps:
        return GameConfig::make_default_mame_cps();
    case qrd::MameLayerProfile::Konami:
        return GameConfig::make_default_mame_konami();
    case qrd::MameLayerProfile::Sega16B:
        return GameConfig::make_default_mame_segas16b();
    case qrd::MameLayerProfile::Dec0:
        return GameConfig::make_default_mame_dec0();
    case qrd::MameLayerProfile::Gp9001:
        return GameConfig::make_default_mame_gp9001();
    case qrd::MameLayerProfile::NeoGeo:
        return GameConfig::make_default_mame_neogeo();
    case qrd::MameLayerProfile::Saturn:
        return GameConfig::make_default_mame_saturn();
    case qrd::MameLayerProfile::Taito:
        return GameConfig::make_default_mame_taito();
    case qrd::MameLayerProfile::Namco:
        return GameConfig::make_default_mame_namco();
    case qrd::MameLayerProfile::KonamiLethal:
        return GameConfig::make_default_mame_konami_lethal();
    case qrd::MameLayerProfile::TaitoTc0100:
        return GameConfig::make_default_mame_taito_tc0100();
    case qrd::MameLayerProfile::TaitoTc0480:
        return GameConfig::make_default_mame_taito_tc0480();
    case qrd::MameLayerProfile::Unico:
        return GameConfig::make_default_mame_unico();
    case qrd::MameLayerProfile::Oneshot:
        return GameConfig::make_default_mame_oneshot();
    case qrd::MameLayerProfile::Lordgun:
        return GameConfig::make_default_mame_lordgun();
    case qrd::MameLayerProfile::Seta2:
        return GameConfig::make_default_mame_seta2();
    case qrd::MameLayerProfile::Segaybd:
        return GameConfig::make_default_mame_segaybd();
    case qrd::MameLayerProfile::Bbusters:
        return GameConfig::make_default_mame_bbusters();
    case qrd::MameLayerProfile::Nycaptor:
        return GameConfig::make_default_mame_nycaptor();
    case qrd::MameLayerProfile::FullFrame:
        break;
    }

    // Keep the complete Neo Geo clone table above as a compatibility fallback
    // for entries absent from the generated MAME profile asset.
    if (is_neogeo_mame_rom_name(short_name))
        return GameConfig::make_default_mame_neogeo();
    return GameConfig::make_default_mame_full_frame();
}

static GameConfig default_config_for_backend(BackendKind kind, LayerFilterMode snes_mode) {
    switch (kind) {
    case BackendKind::Snes:
        return make_snes_config_for_filter(snes_mode);
    case BackendKind::Genesis:
        return GameConfig::make_default_genesis();
    case BackendKind::Gba:
        return GameConfig::make_default_gba();
    case BackendKind::Gb:
        return GameConfig::make_default_gb();
    case BackendKind::Nes:
        return GameConfig::make_default_nes();
    case BackendKind::Pce:
        return GameConfig::make_default_pce();
    case BackendKind::Sms:
        return GameConfig::make_default_sms();
    case BackendKind::Mame:
        return GameConfig::make_default_mame_full_frame();
    case BackendKind::Saturn:
        return GameConfig::make_default_saturn();
    case BackendKind::Psx:
        return GameConfig::make_default_psx();
    }
    return GameConfig::make_flat();
}

static uint32_t layer_capture_mask_for_config(const GameConfig& config,
                                              const std::vector<bool>* enabled = nullptr) {
    uint32_t mask = 0u;
    for (int i = 0; i < (int)config.layers.size(); ++i) {
        if (enabled && i < (int)enabled->size() && !(*enabled)[i]) continue;
        const auto& layer = config.layers[i];
        if (layer.extraction_type != ExtractionType::PerLayerCapture &&
            layer.extraction_type != ExtractionType::VisibleSourceHybrid &&
            layer.extraction_type != ExtractionType::VisibleSourceFinal) continue;
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
    constexpr float kImmersiveScreenCurve = -0.90f;
    if (!render_state.immersive_beta_enabled) {
        // Keep the current rendering path unchanged when the beta is off.
        // Future immersive-only knobs should be neutralized here.
        render_state.screen_curve = 0.0f;
        render_state.tilt_x = 0.0f;
        render_state.tilt_y = 0.0f;
        render_state.solid_stack = false;
    } else {
        render_state.screen_curve = std::clamp(kImmersiveScreenCurve, -1.0f, 1.0f);
        render_state.tilt_x = std::clamp(render_state.tilt_x, -0.35f, 0.35f);
        render_state.tilt_y = std::clamp(render_state.tilt_y, -0.35f, 0.35f);
    }
    return render_state;
}

static std::array<float, 4> lerp_rgba(const std::array<float, 4>& a,
                                      const std::array<float, 4>& b,
                                      float t) {
    std::array<float, 4> out{};
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    return out;
}

static bool sample_environment_band(const LayerFrame& frame,
                                    int y0,
                                    int y1,
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

static bool build_environment_sphere_sample_from_layer(
    const LayerFrame& frame,
    EnvironmentSphereMode mode,
    OpenXrShell::EnvironmentSphereSample& out_sample) {
    out_sample.valid = false;
    if (!frame.has_pixels || frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
    if (mode == EnvironmentSphereMode::Off) return false;
    constexpr int kBandCount = 12;
    const int sample_y0 = 0;
    const int sample_y1 = (mode == EnvironmentSphereMode::SkyOnly)
        ? std::max(1, frame.height / 2)
        : frame.height;
    const int sample_h = std::max(1, sample_y1 - sample_y0);
    std::array<bool, kBandCount> valid{};
    for (int i = 0; i < kBandCount; ++i) {
        const int y0 = sample_y0 + (sample_h * i) / kBandCount;
        const int y1 = sample_y0 + (sample_h * (i + 1)) / kBandCount;
        valid[i] = sample_environment_band(frame, y0, std::max(y0 + 1, y1), out_sample.bands[i]);
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
        for (int i = 6; i < kBandCount; ++i) {
            out_sample.bands[i] = {out_sample.bands[5][0], out_sample.bands[5][1], out_sample.bands[5][2], 0.0f};
        }
    }
    out_sample.valid = true;
    return true;
}

static void smooth_environment_sphere_sample(OpenXrShell::EnvironmentSphereSample& current,
                                             const OpenXrShell::EnvironmentSphereSample& target,
                                             float blend) {
    if (!target.valid) {
        for (auto& band : current.bands) band[3] *= (1.0f - blend);
        current.valid = false;
        for (const auto& band : current.bands) {
            if (band[3] > 0.01f) { current.valid = true; break; }
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

static bool layer_has_bright_samples(const LayerFrame& frame, int& bright_samples_out) {
    bright_samples_out = 0;
    if (!frame.has_pixels || frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
    const int x_step = 4;
    const int y_step = 4;
    for (int y = 0; y < frame.height; y += y_step) {
        for (int x = 0; x < frame.width; x += x_step) {
            const std::size_t idx = (std::size_t)(y * frame.width + x) * 4u;
            const uint8_t alpha = frame.rgba[idx + 3];
            if (alpha < 32) continue;
            const uint8_t bright = std::max({frame.rgba[idx + 0], frame.rgba[idx + 1], frame.rgba[idx + 2]});
            if (bright > 20) ++bright_samples_out;
        }
    }
    return bright_samples_out > 0;
}

static bool is_blackout_candidate(const std::vector<LayerFrame*>& frames, int& bright_samples_out) {
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

static float blackout_reveal_pulse_scale(XrTime now, XrTime start_time) {
    constexpr float kPulseDurationNs = 120000000.0f;
    const float t = std::clamp((float)(now - start_time) / kPulseDurationNs, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);
    return 1.015f - 0.015f * ease;
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

    if (config.game == "mame_neogeo") {
        // Neo Geo's layer list isn't a fixed hand-authored set (unlike every
        // other backend here) -- it grows permanently over the play session
        // as update_z_splits() discovers new distinct z-values, appended in
        // whatever order they were first observed (roughly but not exactly
        // ascending z). The generic reverse-creation-order fallback below
        // has no relationship to actual depth at all, so show layers sorted
        // by their real z_min instead. Reversed from the original
        // descending sort -- render order came out back-to-front, so index 0
        // is now lowest z (farthest, e.g. backdrop) and the last index is
        // highest z (nearest, e.g. fix), matching the accordion depth
        // assignment (index 0 = k_neogeo_slab_near) in openxr_shell.cpp's
        // render loop.
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

    // Genesis previously persisted the raw config index order (far -> near) in some
    // paths. The current UI/render contract is display order = near -> far, so
    // migrate that legacy identity order back to the canonical default order.
    if (!needs_order_repair && config.game == "genesis" && n > 1) {
        bool is_identity = true;
        for (int i = 0; i < n; ++i) {
            if (layer_order[i] != i) {
                is_identity = false;
                break;
            }
        }
        if (is_identity) {
            layer_order = default_order;
        }
    } else if (needs_order_repair) {
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

    const std::size_t old_enabled = layer_enabled.size();
    layer_enabled.resize(n, true);
    for (std::size_t i = old_enabled; i < layer_enabled.size(); ++i)
        layer_enabled[i] = config.layers[i].default_enabled;

    const std::size_t old_amb = layer_ambilight.size();
    layer_ambilight.resize(n, true);
    for (std::size_t i = old_amb; i < layer_ambilight.size(); ++i)
        layer_ambilight[i] = config.layers[i].default_ambilight;
}

static void ensure_layer_runtime_state_matches_frames(const std::vector<LayerFrame>& frames,
                                                      std::vector<std::string>& layer_names,
                                                      std::vector<int>& layer_order,
                                                      std::vector<bool>& layer_enabled,
                                                      std::vector<bool>& layer_ambilight) {
    const int n = (int)frames.size();
    layer_names.clear();
    layer_names.reserve(n);
    for (const auto& frame : frames)
        layer_names.push_back(frame.id.empty() ? "Layer" : frame.id);

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

    if (needs_order_repair) {
        layer_order.clear();
        layer_order.reserve(n);
        for (int i = 0; i < n; ++i) layer_order.push_back(i);
    }

    layer_enabled.resize(n, true);
    layer_ambilight.resize(n, true);
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

static void sync_cached_layer_geometry_from_config(std::vector<LayerFrame>& layer_frames,
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

// Perspective compensation: run on the final render refs (after layer reorder and
// compact_visible_layer_depths) so the reference and scales reflect the actual
// depths and order used for rendering. The nearest layer keeps its quad_width
// unchanged (scale=1); farther layers have quad_width scaled up proportionally
// so they subtend the same visual angle as the nearest layer.
static void apply_perspective_comp_to_refs(std::vector<LayerFrame*>& refs) {
    const int n = (int)refs.size();
    if (n <= 1) return;
    // Find nearest (smallest depth)
    int ref = -1;
    for (int i = 0; i < n; ++i) {
        if (!refs[i]) continue;
        if (ref < 0 || refs[i]->depth_meters < refs[ref]->depth_meters) ref = i;
    }
    if (ref < 0 || refs[ref]->depth_meters < 0.01f) return;
    const float ref_depth = refs[ref]->depth_meters;
    const float ref_width = refs[ref]->quad_width_meters;
    for (int i = 0; i < n; ++i) {
        if (!refs[i]) continue;
        refs[i]->quad_width_meters = ref_width * (refs[i]->depth_meters / ref_depth);
        refs[i]->persp_comp_scale = 1.0f; // no UV crop needed — let content stretch
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
    const int clamped = std::clamp(copy_count, 1, 100);
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

static std::vector<OpenXrShell::QuickSettingsPreset> make_quick_settings_presets() {
    using Preset = OpenXrShell::QuickSettingsPreset;
    return {
        Preset{"Arcade Close", 0.0f, 0.0f, 0.0f, 0.02f, 1.00f, 0.92f, 1.42f, 2.45f, 8,
               false, UpscaleMode::Off, true, false, DepthMode::Off, false, 1.12f, 0.92f, 0.82f, 1.00f},
        Preset{"Cinema Wide", 0.0f, 0.0f, 0.0f, 0.06f, 1.35f, 1.15f, 1.90f, 3.15f, 10,
               false, UpscaleMode::PixelArt, true, false, DepthMode::Off, false, 1.06f, 1.04f, 0.92f, 1.02f},
        Preset{"Depth Punch", 0.0f, 0.0f, 0.0f, 0.00f, 1.08f, 0.78f, 2.35f, 2.70f, 14,
               true, UpscaleMode::Off, true, false, DepthMode::WholeLayer, true, 1.18f, 0.98f, 0.86f, 1.00f},
        Preset{"Lounge Right", 0.32f, 0.0f, 0.28f, -0.05f, 0.96f, 1.05f, 1.70f, 2.55f, 7,
               false, UpscaleMode::Off, false, true, DepthMode::Off, false, 1.10f, 0.90f, 0.76f, 0.98f},
        Preset{"Poster Left", -0.38f, 0.0f, -0.34f, 0.10f, 0.88f, 1.00f, 1.55f, 2.35f, 6,
               false, UpscaleMode::PixelArt, true, false, DepthMode::Off, false, 1.03f, 1.08f, 0.95f, 1.04f},
    };
}

static std::string trim_copy(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    std::size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string sanitize_preset_name(const std::string& s, const std::string& fallback) {
    std::string out = trim_copy(s);
    for (char& c : out) {
        if ((unsigned char)c < 32 || c == '=' || c == '\n' || c == '\r') c = ' ';
    }
    out = trim_copy(out);
    return out.empty() ? fallback : out;
}

static std::string escape_preset_signature(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back((char)c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "default" : out;
}

static std::string quick_settings_presets_path(const std::string& root_dir);
static std::string quick_layers_presets_dir(const std::string& root_dir, BackendKind kind);
static std::string quick_layer_presets_path(const std::string& root_dir, BackendKind kind, const std::string& signature);
static void write_quick_settings_presets_file(const std::string& path,
                                              const std::vector<OpenXrShell::QuickSettingsPreset>& presets);
static void load_quick_settings_presets_file(const std::string& path,
                                             std::vector<OpenXrShell::QuickSettingsPreset>& presets);
static void write_quick_layer_presets_file(const std::string& path,
                                           const std::vector<OpenXrShell::QuickLayerPreset>& presets);
static void load_quick_layer_presets_file(const std::string& path,
                                          std::vector<OpenXrShell::QuickLayerPreset>& presets);
static OpenXrShell::QuickLayerPreset make_default_quick_layer_preset(const GameConfig& config);

static std::string quick_layer_signature(BackendKind backend_kind,
                                         LayerFilterMode filter_mode,
                                         const GameConfig& config) {
    std::string sig = backend_storage_subdir(backend_kind);
    if (backend_kind == BackendKind::Snes) {
        sig += ":";
        sig += layer_filter_mode_label(filter_mode);
    }
    sig += ":";
    for (const auto& layer : config.layers) {
        sig += layer.id;
        sig += "|";
    }
    return sig;
}

static std::vector<OpenXrShell::QuickLayerPreset> make_quick_layer_presets_for_signature(
    const std::string& signature, const GameConfig& config) {
    using Preset = OpenXrShell::QuickLayerPreset;
    const Preset default_preset = make_default_quick_layer_preset(config);
    if (signature.rfind("genesis:", 0) == 0) {
        return {
            default_preset,
            {"Sprites Pop",
             {"sprites_high","sprites_low","plane_a_high","plane_a_low","plane_b_high","plane_b_low","background"},
             {true,true,true,true,true,true,true},
             {true,true,false,false,false,false,false}},
            {"Backdrop Calm",
             {"plane_a_high","sprites_high","plane_a_low","sprites_low","plane_b_high","plane_b_low","background"},
             {true,true,true,true,true,true,true},
             {true,false,true,false,false,false,true}},
            {"Foreground Focus",
             {"sprites_high","plane_a_high","sprites_low","plane_a_low","plane_b_high","plane_b_low","background"},
             {true,true,true,true,false,false,true},
             {true,true,false,false,false,false,false}},
            {"Minimal Mix",
             {"sprites_high","sprites_low","plane_a_high","plane_a_low","plane_b_high","plane_b_low","background"},
             {true,true,true,false,false,false,false},
             {false,false,false,false,false,false,false}},
        };
    }
    if (signature.find("snes:HYBRID:") == 0) {
        return {
            default_preset,
            {"Character Focus",
             {"pc_obj","pc_bg1","pc_bg3","pc_bg2","pc_bg4","backdrop"},
             {true,true,true,true,true,true},
             {true,false,true,false,false,false}},
            {"Backdrop Heavy",
             {"pc_bg1","pc_obj","pc_bg2","pc_bg3","pc_bg4","backdrop"},
             {true,true,true,true,true,true},
             {true,true,false,false,false,true}},
            {"Foreground Clean",
             {"pc_obj","pc_bg1","pc_bg2","pc_bg3","pc_bg4","backdrop"},
             {true,true,true,false,false,true},
             {true,false,false,false,false,false}},
            {"Minimal Mix",
             {"pc_obj","pc_bg1","pc_bg3","backdrop","pc_bg2","pc_bg4"},
             {true,true,true,false,false,false},
             {false,false,false,false,false,false}},
        };
    }
    if (signature.find("snes:PER:") == 0) {
        return {
            default_preset,
            {"Sprites + FG",
             {"pc_obj","pc_bg1","pc_bg2","pc_bg3","pc_bg4"},
             {true,true,true,false,false},
             {true,true,false,false,false}},
            {"Layer Study",
             {"pc_bg4","pc_bg3","pc_bg2","pc_bg1","pc_obj"},
             {true,true,true,true,true},
             {false,false,false,false,false}},
            {"OBJ Punch",
             {"pc_obj","pc_bg2","pc_bg1","pc_bg3","pc_bg4"},
             {true,true,true,true,true},
             {true,false,false,false,false}},
            {"Background Only",
             {"pc_bg1","pc_bg2","pc_bg3","pc_bg4","pc_obj"},
             {true,true,true,true,false},
             {false,false,false,false,false}},
        };
    }
    if (signature.find("snes:Z:") == 0) {
        return {
            default_preset,
            {"Sprite Front",
             {"sprite_p3","sprite_p2","bg0_hi","bg1_hi","sprite_p1","bg0_lo","bg1_lo","bg_far_hi","sprite_p0","bg_far_lo","backdrop"},
             {true,true,true,true,true,true,true,true,true,true,true},
             {true,true,false,false,false,false,false,false,false,false,false}},
            {"Background Sweep",
             {"bg0_hi","bg1_hi","bg0_lo","bg1_lo","bg_far_hi","bg_far_lo","sprite_p3","sprite_p2","sprite_p1","sprite_p0","backdrop"},
             {true,true,true,true,true,true,true,true,true,true,true},
             {true,true,true,true,true,true,false,false,false,false,true}},
            {"Sprite Study",
             {"sprite_p3","sprite_p2","sprite_p1","sprite_p0","bg0_hi","bg1_hi","bg0_lo","bg1_lo","bg_far_hi","bg_far_lo","backdrop"},
             {true,true,true,true,true,true,false,false,false,false,true},
             {false,false,false,false,false,false,false,false,false,false,false}},
            {"Flat Backdrop",
             {"bg0_hi","bg1_hi","sprite_p3","sprite_p2","bg0_lo","bg1_lo","sprite_p1","bg_far_hi","sprite_p0","bg_far_lo","backdrop"},
             {true,true,true,true,true,true,true,true,true,true,true},
             {false,false,false,false,false,false,false,false,false,false,true}},
        };
    }
    if (signature.find("snes:SHOW ALL:") == 0) {
        return {
            {"Discovery Default",
             {"sprite_p3","bg0_hi","bg1_hi","sprite_p2","bg0_lo","bg1_lo","sprite_p1","bg_far_hi","sprite_p0","bg_far_lo","backdrop","pc_obj","pc_bg1","pc_bg2","pc_bg3","pc_bg4"},
             {true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false},
             {true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false}},
            {"Capture Compare",
             {"pc_obj","pc_bg1","pc_bg2","pc_bg3","pc_bg4","sprite_p3","bg0_hi","bg1_hi","sprite_p2","bg0_lo","bg1_lo","sprite_p1","bg_far_hi","sprite_p0","bg_far_lo","backdrop"},
             {true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true},
             {false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false}},
            {"Sprite Reveal",
             {"pc_obj","sprite_p3","sprite_p2","sprite_p1","sprite_p0","bg0_hi","bg1_hi","bg0_lo","bg1_lo","bg_far_hi","bg_far_lo","backdrop","pc_bg1","pc_bg2","pc_bg3","pc_bg4"},
             {true,true,true,true,true,true,true,true,true,true,true,true,false,false,false,false},
             {true,true,true,true,true,false,false,false,false,false,false,false,false,false,false,false}},
            {"Background Survey",
             {"pc_bg1","pc_bg2","pc_bg3","pc_bg4","bg0_hi","bg1_hi","bg0_lo","bg1_lo","bg_far_hi","bg_far_lo","backdrop","pc_obj","sprite_p3","sprite_p2","sprite_p1","sprite_p0"},
             {true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false},
             {true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false}},
            {"Minimal All",
             {"sprite_p3","bg0_hi","bg1_hi","bg0_lo","bg1_lo","backdrop","pc_obj","pc_bg1","pc_bg2","pc_bg3","pc_bg4","sprite_p2","sprite_p1","sprite_p0","bg_far_hi","bg_far_lo"},
             {true,true,true,true,true,true,true,true,false,false,false,false,false,false,false,false},
             {false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false}},
        };
    }
    if (signature.rfind("nes:", 0) == 0) {
        return {
            default_preset,
            {"Sprites Pop",
             {"sprites","bg_near","bg_mid","bg_far","backdrop"},
             {true,true,true,true,true},
             {true,true,false,false,false}},
            {"Backdrop Calm",
             {"bg_far","bg_mid","sprites","bg_near","backdrop"},
             {true,true,true,true,true},
             {true,false,false,false,true}},
            {"Layer Study",
             {"backdrop","bg_far","bg_mid","bg_near","sprites"},
             {true,true,true,true,true},
             {false,false,false,false,false}},
            {"Minimal Mix",
             {"sprites","backdrop","bg_far","bg_mid","bg_near"},
             {true,true,false,false,false},
             {false,false,false,false,false}},
        };
    }
    if (signature.rfind("sms:", 0) == 0) {
        return {
            default_preset,
            {"Sprites Pop",
             {"sprites","bg_plane","backdrop"},
             {true,true,true},
             {true,false,false}},
            {"Backdrop Calm",
             {"bg_plane","sprites","backdrop"},
             {true,true,true},
             {false,false,true}},
            {"Minimal Mix",
             {"sprites","backdrop","bg_plane"},
             {true,true,false},
             {false,false,false}},
        };
    }
    if (signature.rfind("pce:", 0) == 0) {
        return {
            default_preset,
            {"Sprites Pop",
             {"sprites","bg_plane","backdrop"},
             {true,true,true},
             {true,false,false}},
            {"Backdrop Calm",
             {"bg_plane","sprites","backdrop"},
             {true,true,true},
             {false,false,true}},
            {"Minimal Mix",
             {"sprites","backdrop","bg_plane"},
             {true,true,false},
             {false,false,false}},
        };
    }
    if (signature.rfind("gba:", 0) == 0) {
        return {
            default_preset,
            {"OBJ Punch",
             {"obj","bg0","bg1","bg2","bg3"},
             {true,true,true,true,true},
             {true,true,false,false,false}},
            {"Layer Study",
             {"bg3","bg2","bg1","obj","bg0"},
             {true,true,true,true,true},
             {false,false,false,false,false}},
            {"Backdrop Heavy",
             {"bg3","bg2","obj","bg1","bg0"},
             {true,true,true,true,true},
             {true,true,false,false,false}},
            {"Minimal Mix",
             {"obj","bg0","bg1","bg2","bg3"},
             {true,true,false,false,false},
             {false,false,false,false,false}},
        };
    }
    if (signature.rfind("gb:", 0) == 0) {
        return {
            default_preset,
            {"Sprites Pop",
             {"obj","window","bg"},
             {true,true,true},
             {true,false,false}},
            {"Window Focus",
             {"window","obj","bg"},
             {true,true,true},
             {true,false,false}},
            {"Minimal",
             {"obj","bg","window"},
             {true,true,false},
             {false,false,false}},
        };
    }
    return {};
}

static OpenXrShell::QuickLayerPreset make_default_quick_layer_preset(const GameConfig& config) {
    OpenXrShell::QuickLayerPreset preset;
    preset.name = "Default";
    const std::vector<int> order = default_layer_order_for_config(config);
    preset.ordered_ids.reserve(order.size());
    preset.enabled.reserve(order.size());
    preset.ambilight.reserve(order.size());
    for (const int layer_idx : order) {
        if (layer_idx < 0 || layer_idx >= (int)config.layers.size()) continue;
        const LayerConfig& layer = config.layers[layer_idx];
        preset.ordered_ids.push_back(layer.id);
        preset.enabled.push_back(layer.default_enabled);
        preset.ambilight.push_back(layer.default_ambilight);
    }
    return preset;
}

static OpenXrShell::QuickSettingsPreset make_default_quick_settings_preset(BackendKind kind,
                                                                           const GameConfig& config) {
    OpenXrShell::QuickSettingsPreset preset;
    const VrState defaults = presentation::default_vr_state_for_backend(kind);
    preset.name = "Default";
    preset.canvas_x = 0.0f;
    preset.canvas_y = 0.0f;
    preset.canvas_az = 0.0f;
    preset.canvas_el = 0.0f;
    preset.canvas_scale = 1.0f;
    preset.immersive_beta_enabled = defaults.immersive_beta_enabled;
    preset.upscale_mode = defaults.upscale_mode;
    preset.ambilight = defaults.ambilight;
    preset.passthrough = defaults.shadows;
    preset.depth_mode = defaults.depth_mode;
    preset.layers_3d = defaults.layers_3d;
    preset.gamma = defaults.gamma;
    preset.contrast = defaults.contrast;
    preset.saturation = defaults.saturation;
    preset.brightness = defaults.brightness;
    preset.perspective_comp = defaults.perspective_comp;
    preset.environment_sphere_mode = defaults.environment_sphere_mode;

    if (!config.layers.empty()) {
        float near_depth = config.layers[0].depth_meters;
        float far_depth = config.layers[0].depth_meters;
        float quad_width = config.layers[0].quad_width_meters;
        for (const auto& layer : config.layers) {
            near_depth = std::min(near_depth, layer.depth_meters);
            far_depth = std::max(far_depth, layer.depth_meters);
            quad_width = layer.quad_width_meters;
        }
        preset.near_depth = near_depth;
        preset.far_depth = far_depth;
        preset.quad_width = quad_width;
    }

    const std::vector<int> default_order = default_layer_order_for_config(config);
    preset.copy_count = current_base_copy_count(config, default_order);
    return preset;
}

static void refresh_default_quick_settings_preset(BackendKind kind,
                                                  const GameConfig& config,
                                                  std::vector<OpenXrShell::QuickSettingsPreset>& presets) {
    if (kind != BackendKind::Snes && kind != BackendKind::Genesis) return;
    if (presets.empty()) {
        presets = make_quick_settings_presets();
    }
    presets[0] = make_default_quick_settings_preset(kind, config);
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

void OpenXrShell::set_rom_load_stage(const std::string& path, const std::string& stage) {
    bool did_preview_update = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_preview_extract_path.empty() && path == m_preview_extract_path) {
            m_preview_extract_message = stage;
            did_preview_update = true;
        }
    }
    if (!m_rom_load_in_progress.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (path != m_rom_load_path) return;
        m_rom_load_message = stage;
    }
    m_rom_load_panel_dirty.store(true, std::memory_order_release);
    (void)did_preview_update;
}

void OpenXrShell::set_rom_load_progress(const std::string& path, int percent,
                                        int file_index, int file_total, const std::string& file_name) {
    percent = std::clamp(percent, 0, 100);
    const std::string msg = (file_total <= 1)
        ? ("EXTRACTING ROM " + std::to_string(percent) + "%")
        : ("EXTRACTING ROM " + std::to_string(file_index) + "/" + std::to_string(file_total) +
           (percent < 100 ? ("  " + file_name + " " + std::to_string(percent) + "%") : ""));
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_preview_extract_path.empty() && path == m_preview_extract_path) {
            m_preview_extract_message = msg;
        }
    }
    if (!m_rom_load_in_progress.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (path != m_rom_load_path) return;
        if (file_total <= 1) {
            m_rom_load_message = "EXTRACTING ROM " + std::to_string(percent) + "%";
        } else {
            // The Kotlin extractor always sends a final 100% update for each
            // file before starting the next, so this is the one place a file
            // is recorded as done -- guarded by index so it happens exactly once.
            if (percent >= 100 && (int)m_rom_load_extract_history.size() < file_index) {
                m_rom_load_extract_history.push_back("  " + file_name + " done");
            }
            std::string full_msg = "EXTRACTING ROM " + std::to_string(file_index) + "/" + std::to_string(file_total);
            for (const auto& line : m_rom_load_extract_history) full_msg += "\n" + line;
            if (percent < 100) full_msg += "\n  " + file_name + " " + std::to_string(percent) + "%";
            m_rom_load_message = full_msg;
        }
    }
    m_rom_load_panel_dirty.store(true, std::memory_order_release);
}

void OpenXrShell::set_preview_extract_target(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_preview_extract_path = path;
    m_preview_extract_message.clear();
}

void OpenXrShell::set_preview_extract_done(const std::string& path, const std::string& extracted_path) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_preview_extract_path == path) {
        m_preview_extract_path.clear();
        m_preview_extract_message.clear();
    }
    if (m_library_preview_path == path && !extracted_path.empty()) {
        struct stat st{};
        if (stat(extracted_path.c_str(), &st) == 0) {
            double mb = (double)st.st_size / (1024.0 * 1024.0);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.2f MB", mb);
            m_library_preview_uncompressed_size_str = buf;
        }
    }
}

std::string OpenXrShell::preview_extract_status(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (path.empty() || path != m_preview_extract_path) return std::string();
    return m_preview_extract_message;
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
void OpenXrShell::request_quick_settings_preset_save(int i) { m_quick_settings_save_request_pending = i; }
void OpenXrShell::request_quick_layer_preset_save(int i) { m_quick_layers_save_request_pending = i; }
void OpenXrShell::submit_quick_preset_name(int kind, int slot, const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_quick_named_save_name = name;
    }
    m_quick_named_save_slot_pending = slot;
    m_quick_named_save_kind_pending = kind;
}
void OpenXrShell::cancel_quick_preset_name(int, int) {
    m_quick_preset_dialog_open = false;
    m_pending_quick_preset_kind = -1;
    m_pending_quick_preset_slot = -1;
}

void OpenXrShell::request_rom_search_dialog(const std::string& current_text) {
    // Kept for submit_rom_search_text()/cancel_rom_search_text()'s JNI plumbing
    // (still-valid infrastructure) but no longer called from the Library tab —
    // confirmed on-device that a plain Android AlertDialog never actually
    // renders while the app holds an immersive OpenXR session (it's a normal
    // 2D window; the VR compositor doesn't show it), so clicking Search
    // silently did nothing visible. draw_library_rom_list() now uses a real
    // in-VR on-screen keyboard (draw_rom_search_keyboard()) instead.
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (env) {
        jclass cls = env->GetObjectClass(m_activity_global);
        jmethodID mid = env->GetMethodID(cls, "showRomSearchDialog", "(Ljava/lang/String;)V");
        if (mid) {
            jstring js = env->NewStringUTF(current_text.c_str());
            env->CallVoidMethod(m_activity_global, mid, js);
            env->DeleteLocalRef(js);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(cls);
        if (detach) m_vm->DetachCurrentThread();
    }
}

void OpenXrShell::submit_rom_search_text(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_search_result = text;
    }
    m_rom_search_result_pending = true;
}

void OpenXrShell::cancel_rom_search_text() {
    // No pending flag set — draw_library_rom_list() just keeps whatever was
    // already in its filter buffer, same as dismissing any other dialog.
}

namespace {
constexpr int k_code_panel_title_cancel_id = 100;
constexpr int k_code_panel_title_space_id = 101;
constexpr int k_code_panel_title_confirm_id = 102;
constexpr int k_quick_name_max_len = 24;
}

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
        depth_mode_label(m_vr_state.depth_mode),
        upscale_mode_label(m_vr_state.upscale_mode),
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
    MameOccupancyCtrl occupancy_fn;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        mask_fn = m_layer_capture_mask_ctrl;
        occupancy_fn = m_mame_occupancy_ctrl;
    }
    if (mask_fn) {
        const uint32_t mask = is_snes_filter_capable_config(m_config)
            ? layer_capture_mask_for_mode(m_layer_filter_mode)
            : layer_capture_mask_for_config(m_config, &m_layer_enabled);
        mask_fn(mask);
    }
    if (occupancy_fn) {
        const bool enable = m_current_backend_kind == BackendKind::Mame &&
                            m_mame_composition_mode == 1 &&
                            m_config.game != "mame_neogeo";
        occupancy_fn(enable);
    }
}

void OpenXrShell::set_mame_composition_mode(int mode) {
    if (m_current_backend_kind != BackendKind::Mame) return;
    mode = std::clamp(mode, 0, 1);
    if (mode == 1 && (!m_mame_occupancy_available || !m_mame_occupancy_eligible ||
                      m_config.game == "mame_neogeo")) {
        m_mame_composition_mode = 0;
        set_status("OCCUPXY is available only for MAME titles with no usable named layers.");
        return;
    }
    if (m_mame_composition_mode == mode) return;
    m_mame_composition_mode = mode;
    m_config = (mode == 1)
        ? GameConfig::make_default_mame_occupxy()
        : config_for_mame_rom_name(m_current_mame_path_hint.empty()
                                       ? m_current_rom_name : m_current_mame_path_hint);
    m_layer_order.clear();
    m_layer_enabled.clear();
    m_layer_ambilight.clear();
    m_layer_side_color.clear();
    presentation::ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled,
        m_layer_ambilight, m_layer_side_color);
    reset_emulation_cache_for_rom_change();
    sync_layer_capture_mask();
    refresh_quick_layer_presets();
    m_layer_panel_dirty = true;
    set_status(mode == 1 ? "MAME OCCUPXY fallback enabled." : "MAME flat composition restored.");
}

void OpenXrShell::apply_layer_filter_mode(LayerFilterMode mode, bool restore_saved_state) {
    m_layer_filter_mode = mode;
    if (restore_saved_state && m_saved_layer_mode_state.valid && m_saved_layer_mode_state.mode == mode) {
        m_config = m_saved_layer_mode_state.config;
        m_layer_order = m_saved_layer_mode_state.order;
        m_layer_enabled = m_saved_layer_mode_state.enabled;
        m_layer_ambilight = m_saved_layer_mode_state.ambilight;
        m_layer_side_color = m_saved_layer_mode_state.side_color;
    } else {
        m_config = m_current_backend_kind == BackendKind::Mame
            ? config_for_mame_rom_name(m_current_mame_path_hint.empty()
                                           ? m_current_rom_name : m_current_mame_path_hint)
            : presentation::default_config_for_backend(m_current_backend_kind, (int)mode);
        m_layer_order.clear();
        m_layer_enabled.clear();
        m_layer_ambilight.clear();
        m_layer_side_color.clear();
    }
    presentation::ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
    if (m_vm && m_activity_global) {
        JNIEnv* env = nullptr;
        bool detach = false;
        if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
        }
        if (env) {
            jclass cls = env->GetObjectClass(m_activity_global);
            jmethodID mid = env->GetMethodID(cls, "setUiThemeId", "(I)V");
            if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_ui_theme);
            env->DeleteLocalRef(cls);
            if (detach) m_vm->DetachCurrentThread();
        }
    }
    sync_layer_capture_mask();
    refresh_quick_layer_presets();
    m_layer_panel_dirty = true;
}

bool OpenXrShell::start_common(JavaVM* vm, JNIEnv* env, jobject activity, bool open_menu_on_startup,
                               int autosave_interval_seconds, bool load_last_save_enabled,
                               std::string& status_out) {
    stop(env);
    if (!vm || !env || !activity) {
        status_out = "OpenXR start failed: invalid Android context.";
        set_status(status_out);
        return false;
    }
    m_vm              = vm;
    m_activity_global = env->NewGlobalRef(activity);
    {
        jclass cls = env->GetObjectClass(m_activity_global);
        jmethodID mid = env->GetMethodID(cls, "setUiThemeId", "(I)V");
        if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_ui_theme);
        env->DeleteLocalRef(cls);
    }
    m_stop_requested  = false;
    m_running         = true;
    m_active_refresh_rate = 0.0f;
    m_last_render_ms = 0.0f;
    m_avg_render_ms = 0.0f;
    m_max_render_ms = 0.0f;
    m_render_sample_count = 0;
    m_layer_filter_mode = LayerFilterMode::Hybrid;
    m_saved_layer_mode_state.valid = false;
    m_vr_state    = presentation::default_vr_state_for_backend(m_current_backend_kind);
    m_config      = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
    m_presets     = make_default_vr_presets();
    m_quick_settings_presets = make_quick_settings_presets();
    {
        const std::string root_dir = get_settings_dir();
        if (!root_dir.empty()) {
            load_quick_settings_presets_file(quick_settings_presets_path(root_dir), m_quick_settings_presets);
        }
    }
    refresh_default_quick_settings_preset(m_current_backend_kind, m_config, m_quick_settings_presets);
    m_quick_layer_presets = make_quick_layer_presets_for_signature(
        quick_layer_signature(m_current_backend_kind, m_layer_filter_mode, m_config), m_config);
    m_button_map  = default_button_map_for_backend(m_current_backend_kind);
    m_autosave_interval_seconds = autosave_interval_seconds;
    m_load_last_save_enabled = load_last_save_enabled;
    load_last_rom_setting();
    m_last_autosave_time_ms = monotonic_time_ms();
    m_load_global_pending = true; // load global settings on first XR frame
    m_open_menu_on_startup = open_menu_on_startup;
    m_autoload_latest_save_pending = false;
    m_request_open_menu = false;
    m_request_open_homebrew = false;
    presentation::ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
    sync_layer_capture_mask();
    reset_panel_pose_defaults();

    set_status("Starting OpenXR shell...");
    m_thread = std::thread([this]() { run(); });
    status_out = status();
    return true;
}

bool OpenXrShell::start(JavaVM* vm, JNIEnv* env, jobject activity, bool open_menu_on_startup,
                        int autosave_interval_seconds, bool load_last_save_enabled,
                        std::string& status_out) {
    return start_common(vm, env, activity, open_menu_on_startup,
                        autosave_interval_seconds, load_last_save_enabled, status_out);
}

void OpenXrShell::request_open_main_menu() {
    m_request_open_menu = true;
}

void OpenXrShell::request_open_homebrew() {
    m_request_open_homebrew = true;
}

void OpenXrShell::reset_panel_pose_defaults() {
    m_main_menu_pose = {{0,0,0,1},{0,0,-1}};
    m_quick_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_layer_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_settings_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_save_state_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_code_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_ctrlmap_panel_pose = {{0,0,0,1},{0,0,-1}};
    m_homebrew_panel_pose = {{0,0,0,1},{0,0,-1}};
}

void OpenXrShell::mark_visual_state_dirty() {
    m_quick_panel_dirty = true;
    m_layer_panel_dirty = true;
    m_settings_panel_dirty = true;
    m_main_menu_dirty = true;
    m_save_state_panel_dirty = true;
    m_code_panel_dirty = true;
    m_ctrlmap_panel_dirty = true;
    m_hw_dirty = true;
}

void OpenXrShell::stop(JNIEnv* env) {
    m_stop_requested = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_rom_load_thread.joinable()) m_rom_load_thread.join();
    m_running = false;

    // Settings only persist via the explicit Save Game/Global Settings
    // buttons (System > Config Files) — nothing autosaves on a running
    // change, so any menu edit made without pressing one of those is
    // otherwise silently lost the moment the app exits. stop() only runs on
    // a genuine app shutdown (QuestVrActivity's onDestroy()/exitApp(), both
    // via nativeStopVr — never on a transient pause), so it's a safe,
    // single place to save both scopes on the way out. The thread is
    // already joined above, so there's no concurrent access to m_vr_state.
    save_settings(/*game_scope=*/false);
    if (!m_current_rom_name.empty()) save_settings(/*game_scope=*/true);

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
    m_impl->has_render_model_ext = has(XR_FB_RENDER_MODEL_EXTENSION_NAME);

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
    if (m_impl->has_render_model_ext)
        enabled_exts.push_back(XR_FB_RENDER_MODEL_EXTENSION_NAME);

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
    // The PSX hardware renderer needs a context in this share group, created on
    // the emulation thread. This is the only place the XR context is guaranteed
    // current, so capture it here.
    psx_gl_context_capture_host();
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
        LOGI("swapchain eye %d: %ux%u (recommended %ux%u, scale %.2fx)", eye,
             e.width, e.height, vcv[eye].recommendedImageRectWidth,
             vcv[eye].recommendedImageRectHeight, m_vr_state.vr_resolution_scale);

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
    // Non-fatal: the ImGui panel migration is additive infra. If it fails to
    // init (shouldn't, on any GLES3 device this app already targets), the app
    // still runs with the existing Kotlin-canvas panels untouched.
    std::string imgui_err;
    if (!m_impl->imgui_bridge.init(imgui_err)) {
        LOGE("ImGuiBridge init failed: %s", imgui_err.c_str());
    }
    // Controller render models load later, from poll_events() right after
    // xrBeginSession() succeeds -- XR_FB_render_model calls require a
    // running session, which doesn't exist yet at this point.
    return true;
}

// ============================================================
// XR_FB_render_model -- load the runtime's own left/right controller glTF
// models (same models the system Home menu uses) so gles_renderer can draw
// them in place of a hand-built mesh. Runs once, on the render thread (GL
// context already current here), right after renderer.init().
// ============================================================
void OpenXrShell::load_controller_render_models() {
    if (!m_impl || !m_impl->has_render_model_ext) return;

    auto get_proc = [&](const char* name, auto& out) {
        return xrGetInstanceProcAddr(
                   m_impl->instance, name,
                   reinterpret_cast<PFN_xrVoidFunction*>(&out)) == XR_SUCCESS && out;
    };
    if (!get_proc("xrEnumerateRenderModelPathsFB", m_impl->pfn_enum_render_model_paths) ||
        !get_proc("xrGetRenderModelPropertiesFB", m_impl->pfn_get_render_model_props) ||
        !get_proc("xrLoadRenderModelFB", m_impl->pfn_load_render_model)) {
        LOGE("XR_FB_render_model present but required functions are unavailable.");
        return;
    }

    uint32_t path_count = 0;
    m_impl->pfn_enum_render_model_paths(m_impl->session, 0, &path_count, nullptr);
    if (path_count == 0) return;
    std::vector<XrRenderModelPathInfoFB> paths(path_count, {XR_TYPE_RENDER_MODEL_PATH_INFO_FB});
    if (!xr_ok(m_impl->pfn_enum_render_model_paths(m_impl->session, path_count, &path_count, paths.data()),
               "xrEnumerateRenderModelPathsFB")) {
        return;
    }

    // Controller-model paths are exposed at /model_fb/controller/{left,right}
    // -- resolve each XrPath's string form so we can find them by name rather
    // than assuming array order/count is fixed across runtime versions.
    auto path_to_string = [&](XrPath p) -> std::string {
        uint32_t len = 0;
        xrPathToString(m_impl->instance, p, 0, &len, nullptr);
        if (len == 0) return {};
        std::string s(len, '\0');
        xrPathToString(m_impl->instance, p, len, &len, s.data());
        s.resize(len > 0 ? len - 1 : 0); // drop the API's trailing NUL
        return s;
    };

    for (const XrRenderModelPathInfoFB& info : paths) {
        const std::string path_str = path_to_string(info.path);
        int hand = -1; // 0=left, 1=right
        if (path_str == "/model_fb/controller/left") hand = 0;
        else if (path_str == "/model_fb/controller/right") hand = 1;
        if (hand < 0) continue;

        XrRenderModelPropertiesFB props{XR_TYPE_RENDER_MODEL_PROPERTIES_FB};
        if (!xr_ok(m_impl->pfn_get_render_model_props(m_impl->session, info.path, &props),
                   "xrGetRenderModelPropertiesFB")) {
            continue;
        }

        XrRenderModelLoadInfoFB load_info{XR_TYPE_RENDER_MODEL_LOAD_INFO_FB};
        load_info.modelKey = props.modelKey;
        XrRenderModelBufferFB buffer{XR_TYPE_RENDER_MODEL_BUFFER_FB};
        // First call with bufferCapacityInput=0 to get the required size.
        if (!xr_ok(m_impl->pfn_load_render_model(m_impl->session, &load_info, &buffer),
                   "xrLoadRenderModelFB (size query)")) {
            continue;
        }
        std::vector<uint8_t> glb(buffer.bufferCountOutput);
        buffer.bufferCapacityInput = (uint32_t)glb.size();
        buffer.buffer = glb.data();
        if (!xr_ok(m_impl->pfn_load_render_model(m_impl->session, &load_info, &buffer),
                   "xrLoadRenderModelFB")) {
            continue;
        }
        glb.resize(buffer.bufferCountOutput);

        if (!m_impl->renderer.load_controller_model(hand, glb)) {
            LOGE("Failed to parse %s controller render model glTF.", hand == 0 ? "left" : "right");
        }
    }
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
            m_emu_frozen_display = false;
        }
        first_frame_since_session = false;
        if (!m_impl || !m_impl->session_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // A resolution-scale change tears the eye swapchains down here, between
        // frames, and the lazy creation just below rebuilds them at the new
        // size on this same iteration.
        if (m_pending_swapchain_rebuild) {
            m_pending_swapchain_rebuild = false;
            destroy_swapchains();
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
                    // XR_FB_render_model calls need a running session (an
                    // XR_ERROR_SESSION_NOT_RUNNING otherwise) -- init_renderer()
                    // runs before this, so load here instead, once.
                    if (!m_controller_models_loaded) {
                        load_controller_render_models();
                        m_controller_models_loaded = true;
                    }
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
    if (!mid) {
        LOGE("rebuild_panel_tex: GetMethodID failed for %s sig=%s", method, sig);
        env->ExceptionClear(); env->DeleteLocalRef(cls); if (detach) vm->DetachCurrentThread(); return;
    }

    // Build args: names_arr first, then extra args
    std::vector<jvalue> args;
    jvalue v0; v0.l = names_arr; args.push_back(v0);
    for (const auto& a : extra_args) args.push_back(a);

    auto pixels = (jintArray)env->CallObjectMethodA(activity, mid, args.data());
    env->DeleteLocalRef(cls);

    if (!pixels || env->ExceptionCheck()) {
        LOGE("rebuild_panel_tex: %s call failed (pixels=%p exc=%d)", method, (void*)pixels, (int)env->ExceptionCheck());
        env->ExceptionClear();
        if (detach) vm->DetachCurrentThread();
        return;
    }

    jsize count = env->GetArrayLength(pixels);
    LOGE("rebuild_panel_tex: %s ok count=%d expect=%d tex_out(before)=%u", method, (int)count, tex_w*tex_h, tex_out);
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

    // TEMP TESTING: m_debug_hide_empty_layers -- filter m_layer_order down to only
    // indices with actual pixel content this frame (LayerFrame::has_pixels) before
    // building any of the display arrays below, so an empty layer doesn't appear in
    // the manual layers panel at all (not just "shown but rendering nothing").
    std::vector<int> display_order;
    if (m_debug_hide_empty_layers) {
        display_order.reserve(m_layer_order.size());
        for (int orig : m_layer_order) {
            const bool has_pixels = orig >= 0 && orig < (int)m_cached_layer_frames.size()
                ? m_cached_layer_frames[orig].has_pixels : true;
            if (has_pixels) display_order.push_back(orig);
        }
    } else {
        display_order = m_layer_order;
    }

    int n = (int)display_order.size();
    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray names_arr = env->NewObjectArray(n, str_cls, nullptr);
    // Reorder names so display order matches m_layer_order
    for (int i = 0; i < n; ++i) {
        int orig = (i < (int)display_order.size()) ? display_order[i] : i;
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
            int orig = (i < (int)display_order.size()) ? display_order[i] : i;
            ev[i] = (orig < (int)m_layer_enabled.size()) ? m_layer_enabled[orig] : JNI_TRUE;
        }
        env->SetBooleanArrayRegion(enabled_arr, 0, n, ev.data());
    }

    // Build ambilight array in display order
    jbooleanArray ambilight_arr = env->NewBooleanArray(n);
    {
        std::vector<jboolean> av(n);
        for (int i = 0; i < n; ++i) {
            int orig = (i < (int)display_order.size()) ? display_order[i] : i;
            av[i] = (orig < (int)m_layer_ambilight.size()) ? m_layer_ambilight[orig] : JNI_TRUE;
        }
        env->SetBooleanArrayRegion(ambilight_arr, 0, n, av.data());
    }

    // Build side-color-override array in display order
    jintArray side_color_arr = env->NewIntArray(n);
    {
        std::vector<jint> sv(n);
        for (int i = 0; i < n; ++i) {
            int orig = (i < (int)display_order.size()) ? display_order[i] : i;
            sv[i] = (m_config.game == "mame_neogeo" && kDebugNeoGeoBlack3dSides)
                ? 6
                : ((orig < (int)m_layer_side_color.size()) ? m_layer_side_color[orig] : 0);
        }
        env->SetIntArrayRegion(side_color_arr, 0, n, sv.data());
    }

    // Build depth array in display order
    jfloatArray depth_arr = env->NewFloatArray(n);
    {
        std::vector<jfloat> dv(n);
        for (int i = 0; i < n; ++i) {
            int orig = (i < (int)display_order.size()) ? display_order[i] : i;
            dv[i] = (orig < (int)m_config.layers.size())
                    ? m_config.layers[orig].depth_meters : 1.5f;
        }
        env->SetFloatArrayRegion(depth_arr, 0, n, dv.data());
    }

    // Build geometry-mode label array in display order.
    jclass str_cls2 = env->FindClass("java/lang/String");
    jobjectArray geom_mode_arr = env->NewObjectArray(n, str_cls2, nullptr);
    for (int i = 0; i < n; ++i) {
        int orig = (i < (int)display_order.size()) ? display_order[i] : i;
        const char* label = "BOX";
        if (orig < (int)m_config.layers.size()) {
            label = geom_mode_label(m_config.layers[orig].geometry_mode);
        }
        jstring js = env->NewStringUTF(label);
        env->SetObjectArrayElement(geom_mode_arr, i, js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(str_cls2);

    // Build thickness array in display order (0 = auto-derive, shown as "AUTO" in Kotlin).
    // Split mode repurposes this same slot to carry split_pixels instead (Kotlin picks the
    // display format based on the geometry-mode label it already received above).
    jfloatArray thickness_arr = env->NewFloatArray(n);
    {
        std::vector<jfloat> tv(n);
        for (int i = 0; i < n; ++i) {
            int orig = (i < (int)display_order.size()) ? display_order[i] : i;
            if (orig < (int)m_config.layers.size()) {
                const auto& lc = m_config.layers[orig];
                switch (lc.geometry_mode) {
                    case LayerGeometryMode::SplitFloor:
                    case LayerGeometryMode::SplitCeiling:
                    case LayerGeometryMode::Room:
                        tv[i] = (float)lc.split_pixels;
                        break;
                    case LayerGeometryMode::Repeat:
                        tv[i] = (float)(lc.repeat_count > 0 ? lc.repeat_count : 3);
                        break;
                    case LayerGeometryMode::DepthScatter:
                        tv[i] = lc.scatter_range;
                        break;
                    case LayerGeometryMode::AutoYDepth:
                        tv[i] = lc.y_depth_range;
                        break;
                    default:
                        tv[i] = lc.box_thickness_meters;
                        break;
                }
            } else {
                tv[i] = 0.0f;
            }
        }
        env->SetFloatArrayRegion(thickness_arr, 0, n, tv.data());
    }

    const PanelMetrics metrics = panel_metrics(PanelKind::Layers);
    const bool has_filter_row = is_snes_filter_capable_config(m_config);
    m_layer_panel_layout = make_layers_layout(n, has_filter_row);
    std::vector<jvalue> args;
    jvalue a; a.l = enabled_arr;               args.push_back(a);
    jvalue b; b.l = ambilight_arr;             args.push_back(b);
    jvalue sc; sc.l = side_color_arr;          args.push_back(sc);
    jvalue c; c.i = m_layer_panel_grabbed;     args.push_back(c);
    jvalue d; d.i = m_layer_panel_hovered;     args.push_back(d); // drop-target row
    jvalue da; da.l = depth_arr;               args.push_back(da);
    jvalue ds; ds.i = m_layer_depth_selected;  args.push_back(ds);
    jvalue e; e.i = metrics.tex_w;             args.push_back(e);
    jvalue f; f.i = metrics.tex_h;             args.push_back(f);
    jstring auto_dup_label = env->NewStringUTF(layer_auto_dup_label(m_layer_auto_dup_percent).c_str());
    jstring filter_label = env->NewStringUTF(layer_filter_mode_label(m_layer_filter_mode));
    jvalue g; g.z = m_emu_frozen_display;      args.push_back(g);
    jvalue h; h.l = auto_dup_label;            args.push_back(h);
    jvalue i; i.l = filter_label;              args.push_back(i);
    jvalue j; j.z = has_filter_row ? JNI_TRUE : JNI_FALSE; args.push_back(j);
    jvalue k; k.l = geom_mode_arr;             args.push_back(k);
    jvalue l; l.l = thickness_arr;             args.push_back(l);
    // Neo Geo only: current layer-composition mode name, shown as a small
    // pill in the top-right of the title bar (see PanelRole::LayerModeToggle
    // in panel_layout.cpp). Empty string on every other game -- Kotlin skips
    // drawing the pill when the label is empty.
	const char* ng_profile_label = m_config.game == "mame_neogeo" ? "OCCUPXY" : "";
    jstring layer_mode_label = env->NewStringUTF(ng_profile_label);
    jvalue mm; mm.l = layer_mode_label;        args.push_back(mm);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderLayerPanelBitmap", names_arr, args,
                      "([Ljava/lang/String;[Z[Z[III[FIIIZLjava/lang/String;Ljava/lang/String;Z[Ljava/lang/String;[FLjava/lang/String;)[I",
                      metrics.tex_w, metrics.tex_h, m_layer_panel_tex, m_layer_panel_dirty);

    env->DeleteLocalRef(names_arr);
    env->DeleteLocalRef(enabled_arr);
    env->DeleteLocalRef(ambilight_arr);
    env->DeleteLocalRef(side_color_arr);
    env->DeleteLocalRef(depth_arr);
    env->DeleteLocalRef(auto_dup_label);
    env->DeleteLocalRef(filter_label);
    env->DeleteLocalRef(geom_mode_arr);
    env->DeleteLocalRef(thickness_arr);
    env->DeleteLocalRef(layer_mode_label);
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

    // Build name/value/isBool arrays.
    // Rows 0-2, 4, 6-7, 14, 17, 19-21: bools; Rows 3, 5, 16, 22: cycle/stepped;
    // Rows 8-13, 15: numeric; Rows 23-29: action buttons.
    struct SettingDef { const char* name; bool is_bool; };
    static const SettingDef defs[k_settings_row_count] = {
        {"Curve Screen", true }, {"Upscale",      false},
        {"Ambilight",    false}, {"",              false}, {"Passthrough",  true }, {"Parallax",     false},
        {"Experimental Rumble", true},
        {"Persp. Comp.", true},
        {"Gamma",        false}, {"Contrast",     false}, {"Saturation",   false},
        {"Brightness",   false},
        {"Refresh Hz",   false},
        {"VR Res Scale",  false},
        {"Depth Mode",     false},
        {"Y-Depth Spread", false},
        {"Audio Spatial",  false},
        {"Audio direction locked to screen", true},
        {"Side Panels", false}, // cycles Help/Settings/Perf Overlay/Background Color
        // 3D box/floor/ceiling/symmetric/split geometry vs. the old flat card-stack duplication.
        // Default on (matches VrState::real_geometry_boxes's default) — this is just the visible
        // toggle for it, easy to flip back to the old style if needed.
        {"3D Geometry",     true},
        // Real-geometry side faces follow the sprite's actual per-row/per-column silhouette
        // instead of one fixed edge column repeated. Off by default (extra cost, opt-in).
        {"Silhouette Sides", true},
        {"ROM Previews", true},
        {"Gun Model", false}, // cycles Pistol/Low-poly/Scope lightgun overlay model
        // Action buttons (rows 23-29) — rendered specially in Kotlin
        {"Calibrate Lightgun", false},
        {"Reset Settings",  false},
        {"",       false},
        {"",       false},
        {"",       false},
        {"",       false},
        {"← Back",          false},
    };

    const std::string game_target = compact_settings_target(
        !m_current_game_name.empty() ? m_current_game_name
                                     : (!m_current_rom_name.empty() ? m_current_rom_name : "Game"));
    const std::string backend_target = compact_settings_target(std::string(backend_kind_name(m_current_backend_kind)));
    const bool has_active_rom = !m_current_rom_name.empty();
    std::array<std::string, k_settings_row_count> dynamic_names;
    for (int i = 0; i < k_settings_row_count; ++i) dynamic_names[i] = defs[i].name;
    dynamic_names[25] = "Save " + game_target + " Settings";
    dynamic_names[26] = "Save " + backend_target + " Global Settings";
    dynamic_names[27] = "Load " + game_target + " Settings";
    dynamic_names[28] = "Load " + backend_target + " Global Settings";

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

    std::string rumble_status;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        rumble_status = m_experimental_rumble_status;
    }

    char val_bufs[k_settings_row_count][64];
    snprintf(val_bufs[0], sizeof(val_bufs[0]), "%s", m_vr_state.immersive_beta_enabled ? "ON" : "OFF");
    snprintf(val_bufs[1], sizeof(val_bufs[1]), "%s", upscale_mode_label(m_vr_state.upscale_mode));
    snprintf(val_bufs[2], sizeof(val_bufs[2]), "%s", m_vr_state.ambilight ? ambilight_placement_label(m_vr_state.ambilight_placement) : "OFF");
    snprintf(val_bufs[3], sizeof(val_bufs[3]), "%s", "");
    snprintf(val_bufs[4], sizeof(val_bufs[4]), "%s", m_vr_state.shadows    ? "ON" : "OFF");
    snprintf(val_bufs[5], sizeof(val_bufs[5]), "%s", parallax_label(m_vr_state.parallax_ratio));
    snprintf(val_bufs[6], sizeof(val_bufs[6]), "%s", m_experimental_rumble_enabled ? rumble_status.c_str() : "OFF");
    snprintf(val_bufs[7], sizeof(val_bufs[7]), "%s", m_vr_state.perspective_comp ? "ON" : "OFF");
    snprintf(val_bufs[8], sizeof(val_bufs[8]), "%.2f", m_vr_state.gamma);
    snprintf(val_bufs[9], sizeof(val_bufs[9]), "%.2f", m_vr_state.contrast);
    snprintf(val_bufs[10], sizeof(val_bufs[10]), "%.2f", m_vr_state.saturation);
    snprintf(val_bufs[11], sizeof(val_bufs[11]), "%.2f", m_vr_state.brightness);
    snprintf(val_bufs[12], sizeof(val_bufs[12]), "%s", refresh_label);
    snprintf(val_bufs[13], sizeof(val_bufs[13]), "%.2fx", m_vr_state.vr_resolution_scale);
    snprintf(val_bufs[14], sizeof(val_bufs[14]), "%s", depth_mode_label(m_vr_state.depth_mode));
    snprintf(val_bufs[15], sizeof(val_bufs[15]), "%.2fm", m_vr_state.sprite_y_depth_spread);
    { static const char* k_spatial_labels[] = {"OFF", "WIDE", "SPATIAL", "SPAT+HAP"};
      snprintf(val_bufs[16], sizeof(val_bufs[16]), "%s", k_spatial_labels[m_vr_state.audio_spatial_mode]); }
    snprintf(val_bufs[17], sizeof(val_bufs[17]), "%s", m_vr_state.audio_screen_lock ? "ON" : "OFF");
    { static const char* k_side_panel_labels[] = {"OFF", "HELP", "SETT", "PERF", "BGC"};
      int spm = std::clamp(m_vr_state.side_panel_mode, 0, kSidePanelModeCount - 1);
      snprintf(val_bufs[18], sizeof(val_bufs[18]), "%s", k_side_panel_labels[spm]); }
    snprintf(val_bufs[19], sizeof(val_bufs[19]), "%s", m_vr_state.real_geometry_boxes ? "ON" : "OFF");
    snprintf(val_bufs[20], sizeof(val_bufs[20]), "%s", m_vr_state.silhouette_sides ? "ON" : "OFF");
    snprintf(val_bufs[21], sizeof(val_bufs[21]), "%s",
             (m_vr_state.rom_preview_enabled && !kDebugDisableRomThumbnailer) ? "ON" : "OFF");
    { static const char* k_gun_model_labels[] = {"PISTOL", "LOW-POLY", "SCOPE"};
      int gm = std::clamp(m_vr_state.gun_model, 0, 2);
      snprintf(val_bufs[22], sizeof(val_bufs[22]), "%s", k_gun_model_labels[gm]); }
    // Action button rows 23-29
    snprintf(val_bufs[23], sizeof(val_bufs[23]), "%s",
             (m_gun_capable && m_gun_hand != 0) ? "ACTION" : "DISABLED"); // Calibrate
    snprintf(val_bufs[24], sizeof(val_bufs[24]), "ACTION"); // Reset
    snprintf(val_bufs[25], sizeof(val_bufs[25]), "%s", has_active_rom ? "ACTION" : "DISABLED"); // Save Game
    snprintf(val_bufs[26], sizeof(val_bufs[26]), "%s", has_active_rom ? "ACTION" : "DISABLED"); // Save Global
    snprintf(val_bufs[27], sizeof(val_bufs[27]), "%s", has_active_rom ? "ACTION" : "DISABLED"); // Load Game
    snprintf(val_bufs[28], sizeof(val_bufs[28]), "%s", has_active_rom ? "ACTION" : "DISABLED"); // Load Global
    snprintf(val_bufs[29], sizeof(val_bufs[29]), "ACTION"); // Back

    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) { env->ExceptionClear(); if (detach) m_vm->DetachCurrentThread(); return; }
    jobjectArray names_arr  = env->NewObjectArray(k_settings_row_count, str_cls, nullptr);
    jobjectArray values_arr = env->NewObjectArray(k_settings_row_count, str_cls, nullptr);
    jbooleanArray bool_arr  = env->NewBooleanArray(k_settings_row_count);
    std::vector<jboolean> bv(k_settings_row_count);
    for (int i = 0; i < k_settings_row_count; ++i) {
        const char* name_ptr = dynamic_names[i].empty() ? defs[i].name : dynamic_names[i].c_str();
        jstring jn = env->NewStringUTF(name_ptr);
        jstring jv = env->NewStringUTF(val_bufs[i]);
        env->SetObjectArrayElement(names_arr, i, jn);
        env->SetObjectArrayElement(values_arr, i, jv);
        env->DeleteLocalRef(jn);
        env->DeleteLocalRef(jv);
        bv[i] = defs[i].is_bool ? JNI_TRUE : JNI_FALSE;
    }
    env->SetBooleanArrayRegion(bool_arr, 0, k_settings_row_count, bv.data());
    env->DeleteLocalRef(str_cls);

    const PanelMetrics metrics = panel_metrics(PanelKind::Settings);
    m_settings_panel_layout = make_settings_layout(k_settings_row_count);

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
// rebuild_dashboard_left_panel_texture
// ============================================================
void OpenXrShell::rebuild_dashboard_left_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    // Dashboard left panel: 7 rows
    // 0: Screen Size, 1: Near Distance, 2: Far Distance, 3: X Position, 4: Y Position, 5: Dup Count, 6: Dup Spacing
    constexpr int row_count = 7;
    static const char* row_names[row_count] = {
        "Screen Size", "Near Distance", "Far Distance", "X Position", "Y Position", "Dup Count", "Dup Spacing"
    };

    char val_bufs[row_count][64];
    snprintf(val_bufs[0], sizeof(val_bufs[0]), "%.2fx", m_canvas_scale);
    // Near: first visible layer depth, Far: last layer depth
    float near_d = 1.0f, far_d = 2.0f;
    if (!m_config.layers.empty()) {
        near_d = m_config.layers[0].depth_meters;
        far_d = m_config.layers.back().depth_meters;
    }
    snprintf(val_bufs[1], sizeof(val_bufs[1]), "%.2fm", near_d);
    snprintf(val_bufs[2], sizeof(val_bufs[2]), "%.2fm", far_d);
    snprintf(val_bufs[3], sizeof(val_bufs[3]), "%.2fm", m_canvas_x);
    snprintf(val_bufs[4], sizeof(val_bufs[4]), "%.2fm", m_canvas_y);
    snprintf(val_bufs[5], sizeof(val_bufs[5]), "%d", current_base_copy_count(m_config, m_layer_order));
    snprintf(val_bufs[6], sizeof(val_bufs[6]), "%.4f", m_dashboard_duplication_spacing);

    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) { env->ExceptionClear(); if (detach) m_vm->DetachCurrentThread(); return; }
    jobjectArray names_arr  = env->NewObjectArray(row_count, str_cls, nullptr);
    jobjectArray values_arr = env->NewObjectArray(row_count, str_cls, nullptr);
    for (int i = 0; i < row_count; ++i) {
        jstring jn = env->NewStringUTF(row_names[i]);
        jstring jv = env->NewStringUTF(val_bufs[i]);
        env->SetObjectArrayElement(names_arr, i, jn);
        env->SetObjectArrayElement(values_arr, i, jv);
        env->DeleteLocalRef(jn);
        env->DeleteLocalRef(jv);
    }
    env->DeleteLocalRef(str_cls);

    const PanelMetrics metrics = panel_metrics(PanelKind::DashboardLeft);
    m_dashboard_left_panel_layout = make_manual_dashboard_left_layout();

    std::vector<jvalue> args;
    jvalue a; a.l = values_arr;                    args.push_back(a);
    jvalue b; b.i = m_dashboard_left_panel_hovered; args.push_back(b);
    jvalue c; c.i = metrics.tex_w;                args.push_back(c);
    jvalue d; d.i = metrics.tex_h;                args.push_back(d);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderDashboardLeftPanelBitmap", names_arr, args,
                      "([Ljava/lang/String;[Ljava/lang/String;III)[I",
                      metrics.tex_w, metrics.tex_h, m_dashboard_left_panel_tex, m_dashboard_left_panel_dirty);

    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(names_arr);
    env->DeleteLocalRef(values_arr);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_save_state_panel_texture
// ============================================================
void OpenXrShell::rebuild_save_state_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    refresh_save_state_slots();
    const PanelMetrics metrics = panel_metrics(PanelKind::SaveStates);
    m_save_state_panel_layout = make_save_state_layout();

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray load_labels = env->NewObjectArray(k_save_state_slot_count, str_cls, nullptr);
    jobjectArray save_labels = env->NewObjectArray(k_save_state_slot_count, str_cls, nullptr);
    jbooleanArray load_enabled = env->NewBooleanArray(k_save_state_slot_count);
    std::vector<jboolean> enabled(k_save_state_slot_count, JNI_FALSE);

    for (int i = 0; i < k_save_state_slot_count; ++i) {
        const std::string load_label =
            (i < (int)m_save_state_slots.size()) ? m_save_state_slots[i].label : save_state_default_label(i);
        jstring jl = env->NewStringUTF(load_label.c_str());
        jstring js = env->NewStringUTF((std::string("SAVE ") + std::to_string(i + 1)).c_str());
        env->SetObjectArrayElement(load_labels, i, jl);
        env->SetObjectArrayElement(save_labels, i, js);
        env->DeleteLocalRef(jl);
        env->DeleteLocalRef(js);
        enabled[i] = (i < (int)m_save_state_slots.size() && m_save_state_slots[i].occupied) ? JNI_TRUE : JNI_FALSE;
    }
    env->SetBooleanArrayRegion(load_enabled, 0, k_save_state_slot_count, enabled.data());
    env->DeleteLocalRef(str_cls);

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderSaveStatePanelBitmap",
        "(Ljava/lang/String;[Ljava/lang/String;[Z[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;III)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(load_labels);
        env->DeleteLocalRef(load_enabled);
        env->DeleteLocalRef(save_labels);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jstring title = env->NewStringUTF(m_current_rom_name.c_str());
    jstring autosave = env->NewStringUTF(autosave_interval_label(m_autosave_interval_seconds).c_str());
    jstring autoload = env->NewStringUTF(m_load_last_save_enabled ? "On" : "Off");
    jintArray pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid, title, load_labels, load_enabled, save_labels, autosave, autoload,
        (jint)m_save_state_panel_hovered, (jint)metrics.tex_w, (jint)metrics.tex_h);

    env->DeleteLocalRef(title);
    env->DeleteLocalRef(autosave);
    env->DeleteLocalRef(autoload);
    env->DeleteLocalRef(load_labels);
    env->DeleteLocalRef(load_enabled);
    env->DeleteLocalRef(save_labels);

    if (pixels && !env->ExceptionCheck()) {
        jsize count = env->GetArrayLength(pixels);
        if (count == metrics.tex_w * metrics.tex_h) {
            jint* raw = env->GetIntArrayElements(pixels, nullptr);
            if (raw) {
                std::vector<uint8_t> rgba(count * 4);
                for (jsize i = 0; i < count; ++i) {
                    jint a = raw[i];
                    rgba[i * 4 + 0] = (a >> 16) & 0xFF;
                    rgba[i * 4 + 1] = (a >> 8) & 0xFF;
                    rgba[i * 4 + 2] = a & 0xFF;
                    rgba[i * 4 + 3] = (a >> 24) & 0xFF;
                }
                upload_panel_texture(m_save_state_panel_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_save_state_panel_dirty = false;
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (pixels) env->DeleteLocalRef(pixels);
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

    const int panel_mode = m_code_panel_quick_name_mode ? 1 : 0;
    std::string secondary_text;
    if (m_code_panel_quick_name_mode) {
        const char* kind_label = m_pending_quick_preset_kind == 0 ? "Settings" : "Layers";
        secondary_text = std::string(kind_label) + " Slot " + std::to_string(m_pending_quick_preset_slot + 1);
    } else {
        const int filter_mode = is_snes_filter_capable_config(m_config) ? (int)m_layer_filter_mode : -1;
        secondary_text = vr_state_encode(
            m_vr_state, &m_config, &m_layer_order, &m_layer_enabled, &m_layer_ambilight, filter_mode);
    }

    const PanelMetrics metrics = panel_metrics(PanelKind::Code);
    m_code_panel_layout = make_code_layout();

    jstring j_input   = env->NewStringUTF(m_code_input_buf.c_str());
    jstring j_current = env->NewStringUTF(secondary_text.c_str());
    jclass  cls       = env->GetObjectClass(m_activity_global);
    jmethodID mid     = env->GetMethodID(cls, "renderCodePanelBitmap",
                                          "(ILjava/lang/String;Ljava/lang/String;III)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(j_input);
        env->DeleteLocalRef(j_current);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    auto pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid, (jint)panel_mode, j_input, j_current, (jint)m_code_panel_hovered,
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
    m_ctrlmap_panel_layout = make_ctrlmap_layout(n, 6);

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
// rebuild_help_panel_texture
// ============================================================
void OpenXrShell::rebuild_help_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    HelpModel model = build_help_model(
        m_menu_open, m_active_sub_panel, m_ctrlmap_mode, m_edit_mode,
        m_current_backend_kind, m_button_map);
    m_help_panel_key = model.key();

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::Help);
    const int n = (int)model.items.size();
    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) {
        env->ExceptionClear();
        if (detach) m_vm->DetachCurrentThread();
        return;
    }
    jobjectArray input_arr = env->NewObjectArray(n, str_cls, nullptr);
    jobjectArray action_arr = env->NewObjectArray(n, str_cls, nullptr);
    for (int i = 0; i < n; ++i) {
        jstring ji = env->NewStringUTF(model.items[i].input.c_str());
        jstring ja = env->NewStringUTF(model.items[i].action.c_str());
        env->SetObjectArrayElement(input_arr, i, ji);
        env->SetObjectArrayElement(action_arr, i, ja);
        env->DeleteLocalRef(ji);
        env->DeleteLocalRef(ja);
    }
    env->DeleteLocalRef(str_cls);

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderHelpPanelBitmap",
        "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;II)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(input_arr);
        env->DeleteLocalRef(action_arr);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jstring title = env->NewStringUTF(model.title.c_str());
    jintArray pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid, title, input_arr, action_arr,
        (jint)metrics.tex_w, (jint)metrics.tex_h);

    env->DeleteLocalRef(title);
    env->DeleteLocalRef(input_arr);
    env->DeleteLocalRef(action_arr);

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
                upload_panel_texture(m_help_panel_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_help_panel_dirty = false;
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (pixels) env->DeleteLocalRef(pixels);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_rom_hint_texture
// Same JNI path as the help panel (renderHelpPanelBitmap), but with fixed
// content — the two least discoverable controls — instead of the live
// context-sensitive help model.
// ============================================================
void OpenXrShell::rebuild_rom_hint_texture() {
    if (!m_vm || !m_activity_global) return;

    HelpModel model;
    bool rom_loading = false;
    std::string rom_load_message;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        rom_loading = m_rom_load_in_progress.load(std::memory_order_acquire);
        rom_load_message = m_rom_load_message;
    }
    if (rom_loading) {
        model.title = "Loading ROM";
        add_help(model, "Please wait", rom_load_message.c_str());
        m_rom_hint_rendered_text = "__ROM_LOAD__" + rom_load_message;
    } else if (!m_rom_hint_override_text.empty()) {
        model.title = "Notice";
        add_help(model, "", m_rom_hint_override_text.c_str());
        m_rom_hint_rendered_text = m_rom_hint_override_text;
    } else {
        // No override and no active load: the post-load "Tips" panel is
        // disabled — nothing to build, nothing to show.
        m_rom_hint_rendered_text.clear();
        m_rom_hint_hide_at_ms = 0;
        return;
    }

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::Help);
    const int n = (int)model.items.size();
    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) {
        env->ExceptionClear();
        if (detach) m_vm->DetachCurrentThread();
        return;
    }
    jobjectArray input_arr = env->NewObjectArray(n, str_cls, nullptr);
    jobjectArray action_arr = env->NewObjectArray(n, str_cls, nullptr);
    for (int i = 0; i < n; ++i) {
        jstring ji = env->NewStringUTF(model.items[i].input.c_str());
        jstring ja = env->NewStringUTF(model.items[i].action.c_str());
        env->SetObjectArrayElement(input_arr, i, ji);
        env->SetObjectArrayElement(action_arr, i, ja);
        env->DeleteLocalRef(ji);
        env->DeleteLocalRef(ja);
    }
    env->DeleteLocalRef(str_cls);

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderHelpPanelBitmap",
        "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;II)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(input_arr);
        env->DeleteLocalRef(action_arr);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jstring title = env->NewStringUTF(model.title.c_str());
    jintArray pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid, title, input_arr, action_arr,
        (jint)metrics.tex_w, (jint)metrics.tex_h);

    env->DeleteLocalRef(title);
    env->DeleteLocalRef(input_arr);
    env->DeleteLocalRef(action_arr);

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
                upload_panel_texture(m_rom_hint_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_rom_hint_dirty = false;
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (pixels) env->DeleteLocalRef(pixels);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// rebuild_perf_overlay_texture — reuses renderHelpPanelBitmap() with live numbers formatted as
// input/action pairs. GPU frame-time shows "N/A" when GL_EXT_disjoint_timer_query isn't
// supported on this device, rather than a fabricated number.
// ============================================================
void OpenXrShell::rebuild_perf_overlay_texture() {
    if (!m_vm || !m_activity_global) return;

    char fps_buf[32], avg10_buf[32], avg30_buf[32], avg60_buf[32], layers_buf[32], objects_buf[32], pixels_buf[32], cpu_buf[32], ram_buf[32], gpu_buf[32], cpu_render_buf[32];
    snprintf(fps_buf, sizeof(fps_buf), "%.0f", m_perf_fps_instant);
    snprintf(avg10_buf, sizeof(avg10_buf), "%.1f", m_perf_fps_avg10s);
    snprintf(avg30_buf, sizeof(avg30_buf), "%.1f", m_perf_fps_avg30s);
    snprintf(avg60_buf, sizeof(avg60_buf), "%.1f", m_perf_fps_avg60s);
    snprintf(layers_buf, sizeof(layers_buf), "%d", m_perf_layer_count);
    const double benchmark_fps = m_perf_fps_avg10s > 0.0f ? m_perf_fps_avg10s : m_perf_fps_instant;
    snprintf(objects_buf, sizeof(objects_buf), "%.0f", m_perf_object_count * benchmark_fps);
    const double pixels_per_second = (double)m_perf_estimated_pixels * benchmark_fps;
    if (pixels_per_second >= 1000000.0) snprintf(pixels_buf, sizeof(pixels_buf), "%.1fM", pixels_per_second / 1000000.0);
    else if (pixels_per_second >= 1000.0) snprintf(pixels_buf, sizeof(pixels_buf), "%.1fk", pixels_per_second / 1000.0);
    else snprintf(pixels_buf, sizeof(pixels_buf), "%.0f", pixels_per_second);
    snprintf(cpu_buf, sizeof(cpu_buf), "%.0f%%", m_perf_cpu_percent);
    snprintf(ram_buf, sizeof(ram_buf), "%.0f MB", m_perf_ram_mb);
    const float gpu_ms = m_impl ? m_impl->renderer.get_last_gpu_ms() : -1.0f;
    if (gpu_ms >= 0.0f) snprintf(gpu_buf, sizeof(gpu_buf), "%.2f ms", gpu_ms);
    else                snprintf(gpu_buf, sizeof(gpu_buf), "N/A");
    snprintf(cpu_render_buf, sizeof(cpu_render_buf), "%.2f ms", m_last_render_ms);

    HelpModel model;
    model.title = "Perf";
    add_help(model, "FPS", fps_buf);
    add_help(model, "Avg FPS (10s)", avg10_buf);
    add_help(model, "Avg FPS (30s)", avg30_buf);
    add_help(model, "Avg FPS (1m)", avg60_buf);
    add_help(model, "Visible layers", layers_buf);
    add_help(model, "3D objects/sec", objects_buf);
    add_help(model, "Est. pixels/sec", pixels_buf);
    add_help(model, "Lighting", (m_vr_state.depth_mode == DepthMode::PixelFx) ? "PIXEL" : "AMBIENT");
    add_help(model, "RAM", ram_buf);
    add_help(model, "GPU frame time", gpu_buf);
    add_help(model, "CPU submit time", cpu_render_buf);

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::Help);
    const int n = (int)model.items.size();
    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) {
        env->ExceptionClear();
        if (detach) m_vm->DetachCurrentThread();
        return;
    }
    jobjectArray input_arr = env->NewObjectArray(n, str_cls, nullptr);
    jobjectArray action_arr = env->NewObjectArray(n, str_cls, nullptr);
    for (int i = 0; i < n; ++i) {
        jstring ji = env->NewStringUTF(model.items[i].input.c_str());
        jstring ja = env->NewStringUTF(model.items[i].action.c_str());
        env->SetObjectArrayElement(input_arr, i, ji);
        env->SetObjectArrayElement(action_arr, i, ja);
        env->DeleteLocalRef(ji);
        env->DeleteLocalRef(ja);
    }
    env->DeleteLocalRef(str_cls);

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderHelpPanelBitmap",
        "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;II)[I");
    env->DeleteLocalRef(cls);
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(input_arr);
        env->DeleteLocalRef(action_arr);
        if (detach) m_vm->DetachCurrentThread();
        return;
    }

    jstring title = env->NewStringUTF(model.title.c_str());
    jintArray pixels = (jintArray)env->CallObjectMethod(
        m_activity_global, mid, title, input_arr, action_arr,
        (jint)metrics.tex_w, (jint)metrics.tex_h);

    env->DeleteLocalRef(title);
    env->DeleteLocalRef(input_arr);
    env->DeleteLocalRef(action_arr);

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
                upload_panel_texture(m_perf_overlay_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_perf_overlay_dirty = false;
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (pixels) env->DeleteLocalRef(pixels);
    if (detach) m_vm->DetachCurrentThread();
}

// ============================================================
// update_perf_stats — FPS every frame; CPU%/RAM sampled every ~500ms (reading /proc has real
// cost, not worth paying every frame for numbers that don't need per-frame precision anyway).
// ============================================================
void OpenXrShell::update_perf_stats() {
    using Clock = std::chrono::steady_clock;
    const double now_s = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();

    m_perf_frame_times.push_back(now_s);
    m_perf_layer_count = 0;
    m_perf_object_count = 0;
    m_perf_estimated_pixels = 0;
    for (const auto& frame : m_cached_layer_frames) {
        if (!frame.has_pixels) continue;
        ++m_perf_layer_count;
        m_perf_object_count += (int)frame.object_boxes.size();
        if (frame.content_bounds_valid) {
            const int w = std::max(0, frame.content_bounds.max_x - frame.content_bounds.min_x + 1);
            const int h = std::max(0, frame.content_bounds.max_y - frame.content_bounds.min_y + 1);
            m_perf_estimated_pixels += (std::uint64_t)w * (std::uint64_t)h;
        }
    }
    while (!m_perf_frame_times.empty() && now_s - m_perf_frame_times.front() > 60.0) {
        m_perf_frame_times.pop_front();
    }
    if (m_perf_frame_times.size() >= 2) {
        const double dt = now_s - m_perf_frame_times[m_perf_frame_times.size() - 2];
        if (dt > 0.0) m_perf_fps_instant = (float)(1.0 / dt);
        auto average_for = [&](double seconds) -> float {
            auto first = m_perf_frame_times.end();
            for (auto it = m_perf_frame_times.begin(); it != m_perf_frame_times.end(); ++it) {
                if (now_s - *it <= seconds) { first = it; break; }
            }
            if (first == m_perf_frame_times.end() || first == m_perf_frame_times.end() - 1) return 0.0f;
            const double window = now_s - *first;
            const auto frames = std::distance(first, m_perf_frame_times.end()) - 1;
            return window > 0.0 ? (float)(frames / window) : 0.0f;
        };
        m_perf_fps_avg10s = average_for(10.0);
        m_perf_fps_avg30s = average_for(30.0);
        m_perf_fps_avg60s = average_for(60.0);
    }

    if (now_s - m_perf_last_sample_wall < 0.5) return;

    // CPU%: delta of (utime+stime) jiffies from /proc/self/stat over the wall-clock delta,
    // normalized by core count to a 0-100 scale. comm (field 2) can itself contain spaces/
    // parens, so skip past the last ')' before splitting the rest by field index.
    long cpu_jiffies = -1;
    {
        std::ifstream f("/proc/self/stat");
        std::string line;
        if (f && std::getline(f, line)) {
            const auto close = line.rfind(')');
            if (close != std::string::npos) {
                long utime = 0, stime = 0;
                // Fields after comm: (3)state (4)ppid ... (14)utime (15)stime — 11 fields after
                // state to reach utime.
                std::istringstream iss(line.substr(close + 2));
                std::string field;
                for (int i = 0; i < 13 && (iss >> field); ++i) {}
                if (iss >> utime >> stime) cpu_jiffies = utime + stime;
            }
        }
    }
    if (cpu_jiffies >= 0 && m_perf_last_cpu_jiffies >= 0) {
        const long clk_tck = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
        const long ncores  = sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1;
        const double wall_dt = now_s - m_perf_last_sample_wall;
        const double cpu_dt  = (double)(cpu_jiffies - m_perf_last_cpu_jiffies) / (double)clk_tck;
        if (wall_dt > 0.0) {
            m_perf_cpu_percent = (float)std::clamp(cpu_dt / wall_dt / (double)ncores * 100.0, 0.0, 100.0);
        }
    }
    if (cpu_jiffies >= 0) m_perf_last_cpu_jiffies = cpu_jiffies;

    // RAM: process resident set size from /proc/self/status.
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (f && std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                long kb = 0;
                std::istringstream(line.substr(6)) >> kb;
                m_perf_ram_mb = (float)kb / 1024.0f;
                break;
            }
        }
    }

    m_perf_last_sample_wall = now_s;
}

// ============================================================
// rebuild_homebrew_panel_texture
// ============================================================
void OpenXrShell::rebuild_homebrew_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::Homebrew);
    jclass cls = env->GetObjectClass(m_activity_global);

    jintArray pixels = nullptr;
    if (m_hw_view == 0) {
        jmethodID mid = env->GetMethodID(cls, "renderHomebrewListBitmap", "(IIIZII)[I");
        if (mid) {
            pixels = (jintArray)env->CallObjectMethod(
                m_activity_global, mid,
                (jint)m_hw_hovered, (jint)m_hw_scroll, (jint)m_hw_feed,
                (jboolean)m_hw_loading,
                (jint)metrics.tex_w, (jint)metrics.tex_h);
        }
    } else {
        jmethodID mid = env->GetMethodID(cls, "renderHomebrewDetailBitmap", "(IZII)[I");
        if (mid) {
            pixels = (jintArray)env->CallObjectMethod(
                m_activity_global, mid,
                (jint)m_hw_selected, (jboolean)m_hw_downloading,
                (jint)metrics.tex_w, (jint)metrics.tex_h);
        }
    }
    env->DeleteLocalRef(cls);

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
                upload_panel_texture(m_hw_tex, metrics.tex_w, metrics.tex_h, rgba);
                env->ReleaseIntArrayElements(pixels, raw, JNI_ABORT);
            }
        }
        m_hw_dirty = false;
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
        "Save States",
        "Settings",
        "Mappings",
        "Wipe Settings",
        "Credits",
        "Exit"
    };
    constexpr int k_item_count = 7;
    m_main_menu_layout = make_main_menu_layout(k_item_count);

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray items_arr = env->NewObjectArray(k_item_count, str_cls, nullptr);
    for (int i = 0; i < k_item_count; ++i) {
        const char* label = k_menu_items[i];
        if (i == 4 && m_frame_predicted_time < m_wipe_settings_done_until) label = "Done";
        else if (i == 4 && m_wipe_settings_armed) label = "Tap again to confirm";
        jstring js = env->NewStringUTF(label);
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

void OpenXrShell::rebuild_quick_edit_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    constexpr int k_visible_layer_presets = 5;
    const PanelMetrics metrics = panel_metrics(PanelKind::QuickEdit);
    m_quick_panel_layout = make_quick_edit_layout((int)m_quick_settings_presets.size(),
                                                  (int)m_quick_layer_presets.size());

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray settings_arr = env->NewObjectArray((jsize)m_quick_settings_presets.size(), str_cls, nullptr);
    for (int i = 0; i < (int)m_quick_settings_presets.size(); ++i) {
        jstring js = env->NewStringUTF(m_quick_settings_presets[i].name.c_str());
        env->SetObjectArrayElement(settings_arr, i, js);
        env->DeleteLocalRef(js);
    }

    jobjectArray layers_arr = env->NewObjectArray(k_visible_layer_presets, str_cls, nullptr);
    jbooleanArray enabled_arr = env->NewBooleanArray(k_visible_layer_presets);
    std::vector<jboolean> enabled(k_visible_layer_presets, JNI_FALSE);
    for (int i = 0; i < k_visible_layer_presets; ++i) {
        std::string label = "Unavailable";
        if (i < (int)m_quick_layer_presets.size() && !m_quick_layer_presets[i].name.empty()) {
            label = m_quick_layer_presets[i].name;
            enabled[i] = JNI_TRUE;
        }
        jstring js = env->NewStringUTF(label.c_str());
        env->SetObjectArrayElement(layers_arr, i, js);
        env->DeleteLocalRef(js);
    }
    env->SetBooleanArrayRegion(enabled_arr, 0, k_visible_layer_presets, enabled.data());
    env->DeleteLocalRef(str_cls);

    int hovered_settings_load = -1;
    int hovered_settings_save = -1;
    int hovered_layers_load = -1;
    int hovered_layers_save = -1;
    int hovered_action = -1;
    if (m_laser_panel == k_panel_quick_edit && m_laser_hit_has_item) {
        switch (m_laser_hit_item.role) {
            case PanelRole::QuickSettingsPreset: hovered_settings_load = m_laser_hit_item.id; break;
            case PanelRole::QuickSettingsSave: hovered_settings_save = m_laser_hit_item.id; break;
            case PanelRole::QuickLayersPreset: hovered_layers_load = m_laser_hit_item.id; break;
            case PanelRole::QuickLayersSave: hovered_layers_save = m_laser_hit_item.id; break;
            case PanelRole::QuickResetSettings: hovered_action = 0; break;
            case PanelRole::QuickManualEdit: hovered_action = 1; break;
            case PanelRole::QuickManualVisual: hovered_action = 2; break;
            case PanelRole::QuickResetLayers: hovered_action = 3; break;
            case PanelRole::QuickManualLayers: hovered_action = 4; break;
            default: break;
        }
    }

    std::vector<jvalue> args;
    jvalue a; a.l = layers_arr; args.push_back(a);
    jvalue b; b.l = enabled_arr; args.push_back(b);
    jvalue c; c.i = hovered_settings_load; args.push_back(c);
    jvalue d; d.i = hovered_settings_save; args.push_back(d);
    jvalue e; e.i = hovered_layers_load; args.push_back(e);
    jvalue f; f.i = hovered_layers_save; args.push_back(f);
    jvalue g; g.i = hovered_action; args.push_back(g);
    jvalue h; h.i = metrics.tex_w; args.push_back(h);
    jvalue i; i.i = metrics.tex_h; args.push_back(i);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderQuickEditPanelBitmap", settings_arr, args,
                      "([Ljava/lang/String;[Ljava/lang/String;[ZIIIIIII)[I",
                      metrics.tex_w, metrics.tex_h, m_quick_panel_tex, m_quick_panel_dirty);

    env->DeleteLocalRef(settings_arr);
    env->DeleteLocalRef(layers_arr);
    env->DeleteLocalRef(enabled_arr);
    if (detach) m_vm->DetachCurrentThread();
}

// -----------------------------------------------------------------------
// rebuild_side_bar_texture
// -----------------------------------------------------------------------
void OpenXrShell::rebuild_side_bar_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::SidePanelBar);
    static const char* kLabels[] = {"OFF", "HELP", "SETT", "PERF", "BGC"};
    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray labels_arr = env->NewObjectArray(kSidePanelModeCount, str_cls, nullptr);
    for (int i = 0; i < kSidePanelModeCount; ++i) {
        jstring js = env->NewStringUTF(kLabels[i]);
        env->SetObjectArrayElement(labels_arr, i, js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(str_cls);

    std::vector<jvalue> args;
    jvalue a; a.i = std::clamp(m_vr_state.side_panel_mode, 0, kSidePanelModeCount - 1); args.push_back(a);
    jvalue b; b.i = m_side_bar_hovered_id; args.push_back(b);
    jvalue c; c.i = metrics.tex_w; args.push_back(c);
    jvalue d; d.i = metrics.tex_h; args.push_back(d);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderSidePanelBarBitmap", labels_arr, args,
                      "([Ljava/lang/String;IIII)[I",
                      metrics.tex_w, metrics.tex_h, m_side_bar_tex, m_side_bar_dirty);

    env->DeleteLocalRef(labels_arr);
    if (detach) m_vm->DetachCurrentThread();
}

// -----------------------------------------------------------------------
// rebuild_bg_color_panel_texture
// -----------------------------------------------------------------------
void OpenXrShell::rebuild_bg_color_panel_texture() {
    if (!m_vm || !m_activity_global) return;

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::BgColorPanel);
    auto pack = [](float r, float g, float b) -> jint {
        auto c = [](float v) { return (int)std::clamp(v * 255.0f, 0.0f, 255.0f); };
        return (jint)(0xFF000000u | (c(r) << 16) | (c(g) << 8) | c(b));
    };
    // 24 packed ARGB colors: [0-7]=solid presets, [8-15]=gradient top colors, [16-23]=gradient
    // bottom colors — lets Kotlin draw the grid (including real gradients) without duplicating
    // the preset tables itself.
    jintArray colors_arr = env->NewIntArray(24);
    std::vector<jint> colors(24);
    for (int i = 0; i < 8; ++i) colors[i] = pack(kBgSolidPresets[i].r, kBgSolidPresets[i].g, kBgSolidPresets[i].b);
    for (int i = 0; i < 8; ++i) colors[8 + i]  = pack(kBgGradientPresets[i].top_r, kBgGradientPresets[i].top_g, kBgGradientPresets[i].top_b);
    for (int i = 0; i < 8; ++i) colors[16 + i] = pack(kBgGradientPresets[i].bot_r, kBgGradientPresets[i].bot_g, kBgGradientPresets[i].bot_b);
    env->SetIntArrayRegion(colors_arr, 0, 24, colors.data());

    int hovered_id = -1;
    if (m_laser_panel == k_panel_bg_color && m_laser_hit_has_item &&
        m_laser_hit_item.role == PanelRole::BgColorSelect) {
        hovered_id = m_laser_hit_item.id;
    }

    std::vector<jvalue> args;
    jvalue a; a.i = std::clamp(m_vr_state.bg_preset_index, -1, 15); args.push_back(a);
    jvalue b; b.i = hovered_id; args.push_back(b);
    jvalue c; c.i = metrics.tex_w; args.push_back(c);
    jvalue d; d.i = metrics.tex_h; args.push_back(d);

    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderBgColorPanelBitmap", reinterpret_cast<jobjectArray>(colors_arr), args,
                      "([IIIII)[I",
                      metrics.tex_w, metrics.tex_h, m_bg_color_panel_tex, m_bg_color_panel_dirty);

    env->DeleteLocalRef(colors_arr);
    if (detach) m_vm->DetachCurrentThread();
}

void OpenXrShell::rebuild_themes_panel_texture() {
    if (!m_vm || !m_activity_global) return;
    const PanelMetrics metrics = panel_metrics(PanelKind::ThemesPanel);
    if (m_themes_panel_layout.items.empty()) m_themes_panel_layout = make_themes_layout();

    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray unused_labels = env->NewObjectArray(0, str_cls, nullptr);
    env->DeleteLocalRef(str_cls);
    std::vector<jvalue> args;
    jvalue a; a.i = static_cast<int>(m_ui_theme); args.push_back(a);
    jvalue b; b.i = m_themes_panel_hovered; args.push_back(b);
    jvalue c; c.i = metrics.tex_w; args.push_back(c);
    jvalue d; d.i = metrics.tex_h; args.push_back(d);
    rebuild_panel_tex(m_vm, m_activity_global,
                      "renderThemesPanelBitmap", unused_labels, args,
                      "([Ljava/lang/String;IIII)[I",
                      metrics.tex_w, metrics.tex_h, m_themes_panel_tex,
                      m_themes_panel_dirty);
    env->DeleteLocalRef(unused_labels);
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

void OpenXrShell::persist_save_automation_settings() {
    const std::string dir = get_settings_dir();
    if (dir.empty()) return;
    mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/" + k_save_automation_file_name;
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "autosave_interval_seconds=%d\n", m_autosave_interval_seconds);
    std::fprintf(f, "load_last_save=%d\n", m_load_last_save_enabled ? 1 : 0);
    std::fprintf(f, "load_last_rom=%d\n", m_load_last_rom_enabled ? 1 : 0);
    fclose(f);
}

void OpenXrShell::load_last_rom_setting() {
    const std::string dir = get_settings_dir();
    if (dir.empty()) return;
    const std::string path = dir + "/" + k_save_automation_file_name;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "load_last_rom=", 14) == 0) {
            m_load_last_rom_enabled = std::atoi(line + 14) != 0;
            break;
        }
    }
    fclose(f);
}

void OpenXrShell::set_current_rom(const std::string& rom_filename) {
    // Strip extension to get a clean stem for use as settings filename
    m_current_rom_name = rom_filename;
    auto dot = m_current_rom_name.rfind('.');
    if (dot != std::string::npos) m_current_rom_name = m_current_rom_name.substr(0, dot);
    m_current_rom_name = sanitize_ascii_filename(m_current_rom_name);
    m_current_game_name.clear();
    refresh_save_state_slots();
    m_save_state_panel_dirty = true;
    m_last_autosave_time_ms = monotonic_time_ms();
    m_load_game_pending = true;
    m_autoload_latest_save_pending = m_load_last_save_enabled;
    show_rom_hint();
    // A freshly created backend starts every channel at its own default
    // (full volume) — re-push the user's saved sliders onto the new instance.
    apply_audio_channel_volumes();
    apply_auto_frame_skip();
    apply_psx_gpu_resolution();
    apply_psx_texture_filter();
}

void OpenXrShell::show_rom_hint() {
    // Rebuild whenever the override text differs from what's cached -- the
    // "static content, build once" fast path only holds for the plain Tips
    // panel (empty override).
    if (m_rom_hint_tex == 0 || m_rom_hint_rendered_text != m_rom_hint_override_text)
        m_rom_hint_dirty = true;
    m_rom_hint_hide_at_ms = monotonic_time_ms() + 5000;
}

void OpenXrShell::request_current_rom_reload() {
    if (m_current_rom_path.empty()) return;
    start_async_rom_preparation(m_current_rom_path);
}

void OpenXrShell::set_current_backend_kind(BackendKind kind, const std::string& rom_name_hint) {
    const BackendKind previous_kind = m_current_backend_kind;
    m_current_backend_kind = kind;
    if (!rom_name_hint.empty()) m_current_rom_path = rom_name_hint;
    m_mame_composition_mode = 0; // session/ROM scoped by design
    m_mame_occupancy_eligible = false;
    m_mame_occupancy_available = false;
    m_mame_occupancy_valid = false;
    reset_emulation_cache_for_rom_change();
    if (!is_snes_filter_capable_config(m_config) && kind == BackendKind::Genesis) {
        m_layer_filter_mode = LayerFilterMode::Hybrid;
    }
    // Keep global performance preference state across ROM/backend switches;
    // the MAME core needs this value during the load that follows, and the
    // per-game settings pass may override it afterward if one is saved.
    const bool afs_snes = m_vr_state.auto_frame_skip_snes;
    const bool afs_genesis = m_vr_state.auto_frame_skip_genesis;
    const bool afs_mame = m_vr_state.auto_frame_skip_mame;
    const bool afs_saturn = m_vr_state.auto_frame_skip_saturn;
    const bool afs_pce = m_vr_state.auto_frame_skip_pce;
    const bool afs_gba = m_vr_state.auto_frame_skip_gba;
    const int psx_gpu_resolution =
        (m_vr_state.psx_gpu_resolution == 1 ||
         m_vr_state.psx_gpu_resolution == 2 ||
         m_vr_state.psx_gpu_resolution == 4)
            ? m_vr_state.psx_gpu_resolution : 4;
    m_vr_state = presentation::default_vr_state_for_backend(kind);
    m_vr_state.auto_frame_skip_snes = afs_snes;
    m_vr_state.auto_frame_skip_genesis = afs_genesis;
    m_vr_state.auto_frame_skip_mame = afs_mame;
    m_vr_state.auto_frame_skip_saturn = afs_saturn;
    m_vr_state.auto_frame_skip_pce = afs_pce;
    m_vr_state.auto_frame_skip_gba = afs_gba;
    m_vr_state.psx_gpu_resolution = psx_gpu_resolution;
    m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
    const std::string& classify_name = !rom_name_hint.empty() ? rom_name_hint : m_current_rom_name;
    if (kind == BackendKind::Mame) {
        m_current_mame_path_hint = classify_name;
        m_config = config_for_mame_rom_name(classify_name);
    } else {
        m_current_mame_path_hint.clear();
        m_config = presentation::default_config_for_backend(kind, (int)m_layer_filter_mode);
    }
    m_gun_capable_auto = rom_is_lightgun_capable(kind, classify_name);
    m_gun_manual_override = false; // manual override doesn't carry over across ROM loads
    m_gun_capable = m_gun_capable_auto;
    // Whether a second gun exists for this system/game, and a clean start for
    // the toggle itself -- the backend is reset to single-gun on load too.
    m_dual_gun_supported = backend_supports_dual_gun(kind, classify_name);
    m_dual_gun_enabled = false;
    m_gun2_render_show = false;
    m_gun2_trigger_prev = false;
    m_gun_hand = m_gun_capable ? 1 : 0; // default to right-handed; player can cycle via left-stick click
    // Never carry an in-progress calibration wizard across ROM/backend changes.
    // A previous gun title could otherwise leave the new game stuck waiting
    // for calibration input even when it is not a lightgun title.
    m_gun_calibration_active = false;
    m_gun_calibration_profile_active = false;
    m_gun_calibration_wait_release = false;
    m_gun_calibration_release_required = false;
    m_gun_calibration_target = -1;
    m_gun_calibration_sample_frames = 0;
    m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
    refresh_default_quick_settings_preset(m_current_backend_kind, m_config, m_quick_settings_presets);
    m_button_map = default_button_map_for_backend(kind);
    m_saved_layer_mode_state.valid = false;
    m_layer_order.clear();
    m_layer_enabled.clear();
    m_layer_ambilight.clear();
    m_layer_side_color.clear();
    presentation::ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
    sync_layer_capture_mask();
    refresh_quick_layer_presets();
    if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
    m_layer_panel_dirty = true;
    m_settings_panel_dirty = true;
    refresh_save_state_slots();
    m_save_state_panel_dirty = true;
}

void OpenXrShell::reset_emulation_cache_for_rom_change() {
    m_cached_frame_out = {};
    m_cached_frame_seq = 0;
    m_cached_layer_frames.clear();
    m_render_layer_refs.clear();
    m_live_layer_grabbed_slot = -1;
    m_live_layer_lstick_move_dir = 0;
    m_live_layer_rstick_move_dir = 0;
    m_live_layer_hovered_slot = -1;
    m_live_layer_flash_slot = -1;
    m_live_layer_flash_until = 0;
}

void OpenXrShell::update_live_layer_canvas_interaction(const VrState& render_state) {
    m_live_layer_laser_hit = false;
    m_live_layer_hovered_slot = -1;
    if ((!m_locomotion_active && !m_layer_deck_active) || m_current_rom_name.empty() ||
        m_layer_order.empty()) return;

    XrPosef aim{};
    if (m_impl->raim_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE) return;
    XrSpaceLocation aim_location{XR_TYPE_SPACE_LOCATION};
    if (xrLocateSpace(m_impl->raim_space, m_impl->app_space, m_frame_predicted_time,
                      &aim_location) != XR_SUCCESS) return;
    const XrSpaceLocationFlags aim_flags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((aim_location.locationFlags & aim_flags) != aim_flags) return;
    aim = aim_location.pose;
    const XrVector3f origin = aim.position;
    const XrVector3f direction = quat_rotate_vec(aim.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
    m_live_layer_laser_origin = origin;
    m_live_layer_laser_end = {
        origin.x + direction.x * 3.0f,
        origin.y + direction.y * 3.0f,
        origin.z + direction.z * 3.0f,
    };

    auto slot_for_ref = [&](const LayerFrame* ref, int ref_index) {
        if (ref_index >= 0 && ref_index < (int)m_render_layer_slot.size()) {
            return m_render_layer_slot[ref_index];
        }
        int orig = -1;
        for (int i = 0; i < (int)m_cached_layer_frames.size(); ++i) {
            if (&m_cached_layer_frames[i] == ref) {
                orig = i;
                break;
            }
        }
        for (int slot = 0; slot < (int)m_layer_order.size(); ++slot) {
            if (m_layer_order[slot] == orig) return slot;
        }
        return -1;
    };

    struct Hit {
        bool valid = false;
        int slot = -1;
        float screen_u = 0.5f;
        float screen_v = 0.5f;
        float distance = 1.0e9f;
        XrVector3f point = {0, 0, 0};
    } opaque_hit;

    // Before a selection starts, choose the closest layer whose full quad the
    // laser crosses -- deliberately NOT gated on opaque pixel content
    // (no live_layer_has_opaque_pixel() check) so an empty/unpopulated layer,
    // or a sparse one, can still be picked by pointing anywhere in its
    // rectangle, not just at drawn pixels.
    for (int i = 0; i < (int)m_render_layer_refs.size(); ++i) {
        LayerFrame* frame = m_render_layer_refs[i];
        const int slot = slot_for_ref(frame, i);
        if (!frame || slot < 0 || frame->is_ui_bar) continue;
        LiveLayerSurface surface{};
        // Layer-deck bookshelf: pick against the same turned-in-place plane the
        // renderer actually draws this layer at (see gles_renderer.cpp's
        // per-layer layer_yaw), so the laser hits where you see it.
        const float pick_yaw = m_layer_deck_active
            ? presentation::layer_deck_yaw(slot, (int)m_layer_order.size(), m_layer_deck_spread)
            : 0.0f;
        if (!build_live_layer_surface(*frame, render_state, m_canvas_x, m_canvas_y,
                                      m_canvas_az, m_canvas_el, m_canvas_scale, surface,
                                      pick_yaw)) continue;
        float u = 0.0f, v = 0.0f, distance = 0.0f;
        XrVector3f point{};
        if (!intersect_live_layer_surface(surface, origin, direction, u, v, distance, point)) continue;
        if (distance >= opaque_hit.distance) continue;
        opaque_hit = {true, slot, u, v, distance, point};
    }

    // Releasing right grip ends the selection and leaves a short flash on the
    // last selected layer. Layer movement is committed immediately, one slot
    // per horizontal stick gesture; controller motion never drags it.
    if (m_live_layer_grabbed_slot >= 0 && !m_layer_grab_held) {
        const int total = (int)m_layer_order.size();
        const int last_slot = std::clamp(m_live_layer_grabbed_slot, 0, std::max(0, total - 1));
        m_live_layer_flash_slot = total > 0 ? last_slot : -1;
        m_live_layer_flash_until = m_frame_predicted_time + 350000000;
        m_live_layer_grabbed_slot = -1;
        m_live_layer_lstick_move_dir = 0;
        m_live_layer_rstick_move_dir = 0;
    }

    if (m_live_layer_grabbed_slot >= 0 && m_layer_grab_held) {
        // Keep the laser endpoint on the selected layer for feedback, but do
        // not use controller motion to move or reorder it.
        const int src = m_live_layer_grabbed_slot;
        const int orig = src >= 0 && src < (int)m_layer_order.size() ? m_layer_order[src] : -1;
        if (orig >= 0 && orig < (int)m_cached_layer_frames.size()) {
            LiveLayerSurface surface{};
            const float grabbed_yaw = m_layer_deck_active
                ? presentation::layer_deck_yaw(src, (int)m_layer_order.size(), m_layer_deck_spread)
                : 0.0f;
            if (build_live_layer_surface(m_cached_layer_frames[orig], render_state,
                                          m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el,
                                          m_canvas_scale, surface, grabbed_yaw)) {
                float u = 0.0f, v = 0.0f, distance = 0.0f;
                XrVector3f point{};
                if (intersect_live_layer_surface(surface, origin, direction, u, v, distance, point)) {
                    m_live_layer_laser_hit = true;
                    m_live_layer_laser_end = point;
                }
            }
        }
        m_live_layer_hovered_slot = src;
        m_live_layer_flash_slot = src;
        if (m_live_layer_flash_until < m_frame_predicted_time)
            m_live_layer_flash_until = m_frame_predicted_time + 100000000;

        // Either controller's horizontal stick moves the selected layer one
        // stack slot. The edge latch makes a held deflection a single move;
        // returning to neutral arms that controller for the next move.
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
        auto read_stick = [&](XrAction action, float& x, float& y) {
            XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
            XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
            get_info.action = action;
            if (xrGetActionStateVector2f(m_impl->session, &get_info, &state) != XR_SUCCESS ||
                !state.isActive) {
                x = y = 0.0f;
                return;
            }
            x = state.currentState.x;
            y = state.currentState.y;
        };
        read_stick(m_impl->act_lstick, lx, ly);
        read_stick(m_impl->act_rstick, rx, ry);
        constexpr float k_move_threshold = 0.55f;
        auto stick_direction = [&](float x) -> int {
            return std::abs(x) > k_move_threshold ? (x < 0.0f ? -1 : 1) : 0;
        };
        auto move_selected_layer = [&](int delta, bool right_hand) {
            const int count = (int)m_layer_order.size();
            const int from = m_live_layer_grabbed_slot;
            if (count <= 0 || from < 0 || from >= count) return;
            const int to = std::clamp(from + delta, 0, count - 1);
            if (to == from) return;

            const int moved = m_layer_order[from];
            m_layer_order.erase(m_layer_order.begin() + from);
            m_layer_order.insert(m_layer_order.begin() + to, moved);
            m_live_layer_grabbed_slot = to;
            m_live_layer_hovered_slot = to;
            m_live_layer_flash_slot = to;
            m_live_layer_flash_until = m_frame_predicted_time + 180000000;
            // Fractions belong to physical depth slots, not to content, so a
            // live reorder must not move the user's slot-depth arrangement.
            m_layer_panel_dirty = true;
            m_quick_panel_dirty = true;
            fire_haptic(right_hand, 0.25f, 35);
        };
        const int ldir = stick_direction(lx);
        const int rdir = stick_direction(rx);
        if (ldir != 0 && m_live_layer_lstick_move_dir == 0)
            move_selected_layer(ldir, false);
        if (rdir != 0 && m_live_layer_rstick_move_dir == 0)
            move_selected_layer(rdir, true);
        m_live_layer_lstick_move_dir = ldir;
        m_live_layer_rstick_move_dir = rdir;
    } else {
        m_live_layer_lstick_move_dir = 0;
        m_live_layer_rstick_move_dir = 0;
        if (opaque_hit.valid) {
            m_live_layer_hovered_slot = opaque_hit.slot;
            m_live_layer_laser_hit = true;
            m_live_layer_laser_end = opaque_hit.point;
            if (m_layer_grab_pressed) {
                m_live_layer_grabbed_slot = opaque_hit.slot;
                m_live_layer_flash_slot = opaque_hit.slot;
                m_live_layer_flash_until = m_frame_predicted_time + 350000000;
                fire_haptic(true, 0.25f, 35);
            }
        }
    }
}

void OpenXrShell::append_live_layer_canvas_guides(OverlayInfo& overlay,
                                                   const VrState& render_state) {
    const bool layer_pick_mode = m_locomotion_active || m_layer_deck_active;
    const bool selected = layer_pick_mode && m_live_layer_grabbed_slot >= 0 && m_layer_grab_held;
    const bool flashing = layer_pick_mode && m_live_layer_flash_slot >= 0 &&
                          m_frame_predicted_time < m_live_layer_flash_until;
    // Layer-deck also always shows a faint gray placeholder on every empty
    // (no-pixel-content) layer, fanned out same as everything else, so an
    // unpopulated layer is still visible/pickable instead of invisible.
    if (!selected && !flashing && !m_layer_deck_active) return;

    const int total = (int)m_layer_order.size();
    if (total <= 0) return;
    const float time_s = (float)m_frame_predicted_time * 1.0e-9f;
    const float blink = 0.5f + 0.5f * std::sinf(time_s * 18.0f);
    for (int slot = 0; slot < total; ++slot) {
        const int orig = m_layer_order[slot];
        if (orig < 0 || orig >= (int)m_cached_layer_frames.size()) continue;
        const LayerFrame& frame = m_cached_layer_frames[orig];
        if (frame.is_ui_bar) continue;
        LiveLayerSurface surface{};
        const float guide_yaw = m_layer_deck_active
            ? presentation::layer_deck_yaw(slot, total, m_layer_deck_spread)
            : 0.0f;
        if (!build_live_layer_surface(frame, render_state, m_canvas_x, m_canvas_y,
                                      m_canvas_az, m_canvas_el, m_canvas_scale, surface,
                                      guide_yaw)) continue;

        LayerCanvasGuide guide;
        guide.center = surface.center;
        guide.right = surface.right;
        guide.up = surface.up;
        guide.normal = surface.normal;
        guide.width = surface.width;
        guide.height = surface.height;
        guide.r = 0.52f;
        guide.g = 0.56f;
        guide.b = 0.62f;
        guide.alpha = 0.0f;
        // Empty layers get a persistent faint gray placeholder in layer-deck
        // mode so they stay visible/pickable even with nothing drawn on them.
        if (m_layer_deck_active && !frame.has_pixels) {
            guide.alpha = 0.18f;
        }
        // The selected/flashed layer gets a brighter guide on top of that.
        if (slot == m_live_layer_flash_slot && (selected || flashing)) {
            guide.r = 1.0f;
            guide.g = 0.78f;
            guide.b = 0.16f;
            guide.alpha = 0.28f + 0.30f * blink;
        }
        if (guide.alpha > 0.001f) overlay.live_layer_guides.push_back(guide);
    }
}

void OpenXrShell::set_current_game_name(const std::string& name) {
    m_current_game_name = sanitize_ascii_label(name);
}

void OpenXrShell::enqueue_haptic(bool right, float amplitude, int duration_ms) {
    enqueue_haptic(QueuedHapticEvent{right, amplitude, duration_ms});
}

void OpenXrShell::set_experimental_rumble_status(const std::string& status) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_experimental_rumble_status = status.empty() ? "OFF" : status;
    m_settings_panel_dirty = true;
}

void OpenXrShell::enqueue_haptic(const QueuedHapticEvent& event) {
    using Clock = std::chrono::steady_clock;
    const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    // event.start_ms is the queue-sequencing offset computed by evaluate_frame()
    const uint64_t base_ms = now_ms + (uint64_t)std::max(0, event.start_ms);
    const float amp = event.amplitude;
    const int dur = std::max(1, event.duration_ms);
    const bool R = event.right;   // primary controller
    const bool L = !R;

    // Push one timed haptic pulse
    auto push = [&](bool right, float a, int dur_ms, int offset_ms) {
        QueuedHapticEvent e = event;
        e.right = right;
        e.amplitude = std::min(1.0f, a);
        e.duration_ms = std::max(1, dur_ms);
        e.delay_ms = offset_ms;
        e.due_time_ms = base_ms + (uint64_t)std::max(0, offset_ms);
        e.pattern = RumbleWavePattern::Single;
        m_pending_haptics.push_back(e);
    };

    std::lock_guard<std::mutex> lk(m_mutex);

    switch (event.effect) {

    case RumbleEffect::FadeOut: {
        // Strong impact that decays: like getting hit and stunned
        const int seg = std::max(20, dur / 3);
        push(R, amp * 1.00f, seg, 0);
        push(L, amp * 0.90f, seg, seg / 2);
        push(R, amp * 0.60f, seg, seg);
        push(L, amp * 0.30f, seg, seg + seg / 2);
        push(R, amp * 0.20f, seg, seg * 2);
        break;
    }

    case RumbleEffect::FadeIn: {
        // Builds up: like powering up or gaining a life
        const int seg = std::max(20, dur / 3);
        push(R, amp * 0.25f, seg, 0);
        push(L, amp * 0.25f, seg, seg / 3);
        push(R, amp * 0.60f, seg, seg);
        push(L, amp * 0.60f, seg, seg + seg / 3);
        push(R, amp * 1.00f, seg, seg * 2);
        push(L, amp * 1.00f, seg, seg * 2 + seg / 3);
        break;
    }

    case RumbleEffect::FadeInOut: {
        // Dramatic swell: life lost, level complete
        const int q = std::max(15, dur / 4);
        push(R, amp * 0.50f, q,     0);
        push(L, amp * 0.50f, q,     q / 2);
        push(R, amp * 1.00f, q * 2, q);
        push(L, amp * 1.00f, q * 2, q + q / 2);
        push(R, amp * 0.50f, q,     q * 3);
        push(L, amp * 0.50f, q,     q * 3 + q / 2);
        break;
    }

    case RumbleEffect::Burst: {
        // Machine-gun: rapid alternating L/R pulses
        constexpr int n_pulses = 7;
        const int pulse_dur = std::max(10, dur / (n_pulses + 3));
        const int gap = std::max(4, (dur - n_pulses * pulse_dur) / n_pulses);
        for (int i = 0; i < n_pulses; ++i) {
            const bool side = (i % 2 == 0) ? R : L;
            // Slightly softer on the first and last pulse for a natural ramp
            const float a = (i == 0 || i == n_pulses - 1) ? amp * 0.70f : amp;
            push(side, a, pulse_dur, i * (pulse_dur + gap));
        }
        break;
    }

    case RumbleEffect::Heartbeat: {
        // Thud-thud ... thud-thud (dramatic double-beat, like a heartbeat)
        const int half = std::max(60, dur / 2);
        const int beat = std::max(35, half / 3);
        const int echo = std::max(20, beat / 2);
        // First beat pair
        push(L, amp,         beat, 0);
        push(R, amp,         beat, 15);
        push(L, amp * 0.55f, echo, beat + 25);
        push(R, amp * 0.55f, echo, beat + 40);
        // Second beat pair after the silent gap
        push(L, amp,         beat, half);
        push(R, amp,         beat, half + 15);
        push(L, amp * 0.55f, echo, half + beat + 25);
        push(R, amp * 0.55f, echo, half + beat + 40);
        break;
    }

    case RumbleEffect::GunRecoil: {
        // A compact, maximum-strength kick on the gun hand, with a much softer
        // delayed counter-pulse in the other hand. The short envelope is meant
        // to feel like the controller snapping back after a rifle shot.
        const int kick = std::max(28, std::min(48, dur / 2));
        push(R, amp,         kick,     0);
        push(L, amp * 0.30f, 18,       8);
        push(R, amp * 0.62f, 22,       kick + 6);
        break;
    }

    case RumbleEffect::GunMachinegun: {
        // Three identical tight automatic bursts. Keep the gun hand dominant
        // while the supporting hand gets small delayed pulses to sell the
        // vibration travelling through a two-handed grip.
        constexpr int n_bursts = 3;
        constexpr int n_pulses = 7;
        const int burst_dur = std::max(1, dur / n_bursts);
        const int pulse = std::max(16, burst_dur / 16);
        const int spacing = pulse + std::max(8, burst_dur / 28);
        for (int burst = 0; burst < n_bursts; ++burst) {
            const int burst_offset = burst * burst_dur;
            for (int i = 0; i < n_pulses; ++i) {
                const int offset = burst_offset + i * spacing;
                const float a = (i == 0 || i == n_pulses - 1) ? amp * 0.78f : amp;
                push(R, a, pulse, offset);
                push(L, amp * 0.22f, std::max(8, pulse / 2), offset + pulse / 2);
            }
        }
        break;
    }

    case RumbleEffect::GunRevolver: {
        // A revolver-style mechanical swell: cylinder/hammer build-up, a
        // full-strength shot, then a short decay.
        constexpr float levels[] = {0.22f, 0.40f, 0.64f, 1.00f, 0.82f, 0.56f, 0.30f};
        constexpr int level_count = (int)(sizeof(levels) / sizeof(levels[0]));
        const int pulse = std::max(20, dur / 12);
        const int spacing = pulse + std::max(10, dur / 30);
        for (int i = 0; i < level_count; ++i) {
            const int offset = i * spacing;
            push(R, amp * levels[i], pulse, offset);
            if (i >= 2 && i <= 4)
                push(L, amp * levels[i] * 0.24f, std::max(10, pulse / 2), offset + pulse / 2);
        }
        break;
    }

    case RumbleEffect::Normal:
    default: {
        // Stereo sweep: primary fires first, secondary follows at mid-point
        const int half = std::max(1, dur / 2);
        if (event.pattern == RumbleWavePattern::Both) {
            push(L, amp,       dur, 0);
            push(R, amp,       dur, half);
            push(R, amp,       dur, dur);
            push(L, amp,       dur, dur + half);
        } else {
            push(R, amp,        dur, 0);
            push(L, amp * 0.6f, dur, half);
        }
        break;
    }

    } // switch
}

void OpenXrShell::load_lightgun_calibration() {
    if (m_gun_calibration_loaded) return;
    m_gun_calibration_loaded = true;
    m_gun_calibration_profiles.clear();

    const std::string dir = get_settings_dir();
    if (dir.empty()) return;
    std::ifstream file(dir + "/" + k_lightgun_calibration_file);
    if (!file) return;

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const size_t split = line.find('=');
        if (split == std::string::npos) continue;
        values[line.substr(0, split)] = line.substr(split + 1);
    }
    auto get_int = [&](const std::string& key, int fallback) {
        auto it = values.find(key);
        int out = fallback;
        return it != values.end() && std::sscanf(it->second.c_str(), "%d", &out) == 1 ? out : fallback;
    };
    auto get_float = [&](const std::string& key, float fallback) {
        auto it = values.find(key);
        float out = fallback;
        return it != values.end() && std::sscanf(it->second.c_str(), "%f", &out) == 1 ? out : fallback;
    };

    const int count = std::clamp(get_int("profile_count", 0), 0, 16);
    for (int i = 0; i < count; ++i) {
        const std::string p = "profile_" + std::to_string(i) + "_";
        LightgunCalibrationProfile profile;
        profile.hand = get_int(p + "hand", 0);
        profile.backend = get_int(p + "backend", 0);
        profile.frame_width = get_int(p + "frame_width", 0);
        profile.frame_height = get_int(p + "frame_height", 0);
        profile.upscale_mode = get_int(p + "upscale_mode", 0);
        profile.canvas_x = get_float(p + "canvas_x", 0.0f);
        profile.canvas_y = get_float(p + "canvas_y", 0.0f);
        profile.canvas_az = get_float(p + "canvas_az", 0.0f);
        profile.canvas_el = get_float(p + "canvas_el", 0.0f);
        profile.canvas_scale = get_float(p + "canvas_scale", 1.0f);
        profile.screen_curve = get_float(p + "screen_curve", 0.0f);
        profile.tilt_x = get_float(p + "tilt_x", 0.0f);
        profile.tilt_y = get_float(p + "tilt_y", 0.0f);
        profile.world_scale = get_float(p + "world_scale", 1.0f);
        profile.world_forward_offset = get_float(p + "world_forward_offset", 0.0f);
        for (int point = 0; point < LightgunCalibrationProfile::kPointCount; ++point) {
            profile.raw[point].u = get_float(p + "raw_" + std::to_string(point) + "_u", 0.0f);
            profile.raw[point].v = get_float(p + "raw_" + std::to_string(point) + "_v", 0.0f);
        }
        if (lightgun_profile_valid(profile)) m_gun_calibration_profiles.push_back(profile);
    }
}

void OpenXrShell::save_lightgun_calibration() {
    const std::string dir = get_settings_dir();
    if (dir.empty()) return;
    mkdir(dir.c_str(), 0755);
    std::ofstream file(dir + "/" + k_lightgun_calibration_file, std::ios::trunc);
    if (!file) return;
    const size_t count = std::min<size_t>(m_gun_calibration_profiles.size(), 16);
    file << "version=1\nprofile_count=" << count << "\n";
    for (size_t i = 0; i < count; ++i) {
        const auto& p = m_gun_calibration_profiles[i];
        const std::string key = "profile_" + std::to_string(i) + "_";
        file << key << "hand=" << p.hand << '\n'
             << key << "backend=" << p.backend << '\n'
             << key << "frame_width=" << p.frame_width << '\n'
             << key << "frame_height=" << p.frame_height << '\n'
             << key << "upscale_mode=" << p.upscale_mode << '\n'
             << key << "canvas_x=" << p.canvas_x << '\n'
             << key << "canvas_y=" << p.canvas_y << '\n'
             << key << "canvas_az=" << p.canvas_az << '\n'
             << key << "canvas_el=" << p.canvas_el << '\n'
             << key << "canvas_scale=" << p.canvas_scale << '\n'
             << key << "screen_curve=" << p.screen_curve << '\n'
             << key << "tilt_x=" << p.tilt_x << '\n'
             << key << "tilt_y=" << p.tilt_y << '\n'
             << key << "world_scale=" << p.world_scale << '\n'
             << key << "world_forward_offset=" << p.world_forward_offset << '\n';
        for (int point = 0; point < LightgunCalibrationProfile::kPointCount; ++point) {
            file << key << "raw_" << point << "_u=" << p.raw[point].u << '\n'
                 << key << "raw_" << point << "_v=" << p.raw[point].v << '\n';
        }
    }
}

void OpenXrShell::clear_lightgun_calibration() {
    m_gun_calibration_profiles.clear();
    m_gun_calibration_profile_active = false;
    m_gun_calibration_active = false;
    m_gun_calibration_wait_release = false;
    m_gun_calibration_release_required = false;
    m_gun_calibration_target = -1;
    m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
    m_gun_calibration_loaded = true;
    const std::string dir = get_settings_dir();
    if (!dir.empty()) std::remove((dir + "/" + k_lightgun_calibration_file).c_str());
}

void OpenXrShell::begin_lightgun_calibration() {
    load_lightgun_calibration();
    m_gun_calibration_active = true;
    m_gun_calibration_profile_active = false;
    m_gun_calibration_wait_release = true;
    m_gun_calibration_release_required = false;
    m_gun_calibration_target = 0;
    m_gun_calibration_sample_frames = 0;
    m_gun_calibration_sample_sum = {};
    m_gun_calibration_trigger_prev = true;
    m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
    // Leave the settings panel behind so the five targets are visible over
    // the frozen game frame. The user can reopen Settings after completion.
    m_menu_open = false;
    m_active_sub_panel = 0;
    m_laser_hit = false;
    // Do NOT turn the new UI off here: the panels are already suppressed for
    // the duration by the m_gun_calibration_active guard in render_frame(),
    // and switching debug_show_new_ui left the session stuck in the old menu.
    EmuFreezeCtrl freeze_fn;
    { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
    if (freeze_fn) {
        m_emu_frozen_display = true;
        freeze_fn(true);
    }
    m_settings_panel_dirty = true;
    set_status("Lightgun calibration: release trigger, then aim at TOP-LEFT and pull trigger.");
}

void OpenXrShell::finish_lightgun_calibration() {
    LightgunCalibrationProfile profile = m_gun_calibration_capture_context;
    profile.raw = m_gun_calibration_captured;
    if (!lightgun_profile_valid(profile)) {
        set_status("Lightgun calibration failed; please try again.");
        m_gun_calibration_active = false;
    } else {
        m_gun_calibration_profiles.erase(
            std::remove_if(m_gun_calibration_profiles.begin(), m_gun_calibration_profiles.end(),
                [&](const LightgunCalibrationProfile& old) {
                    return old.matches(profile.hand, profile.backend, profile.frame_width,
                        profile.frame_height, profile.upscale_mode, profile.canvas_x, profile.canvas_y,
                        profile.canvas_az, profile.canvas_el, profile.canvas_scale,
                        profile.screen_curve, profile.tilt_x, profile.tilt_y,
                        profile.world_scale, profile.world_forward_offset);
                }), m_gun_calibration_profiles.end());
        m_gun_calibration_profiles.push_back(profile);
        save_lightgun_calibration();
        m_gun_calibration_active_profile = profile;
        m_gun_calibration_profile_active = true;
        m_gun_calibration_active = false;
        set_status("Lightgun calibrated. All five aim points are mapped.");
    }
    m_gun_calibration_wait_release = false;
    m_gun_calibration_release_required = true;
    m_gun_calibration_target = -1;
    // "Calibrate Gun 2" borrows m_gun_hand so the five-point capture reads the
    // other controller; give it back now that the profile is stored under that
    // hand. Player one keeps whichever hand it had.
    if (m_gun_calibration_restore_hand != 0) {
        m_gun_hand = m_gun_calibration_restore_hand;
        m_gun_calibration_restore_hand = 0;
    }
    m_gun_calibration_sample_frames = 0;
    m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
    EmuFreezeCtrl freeze_fn;
    { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
    if (freeze_fn) {
        m_emu_frozen_display = false;
        freeze_fn(false);
    }
    m_settings_panel_dirty = true;
}

void OpenXrShell::reset_settings() {
    const float prev_vr_scale = m_vr_state.vr_resolution_scale;
    m_vr_state   = presentation::default_vr_state_for_backend(m_current_backend_kind);
    m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
    m_layer_filter_mode = LayerFilterMode::Hybrid;
    m_config     = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
    refresh_default_quick_settings_preset(m_current_backend_kind, m_config, m_quick_settings_presets);
    m_button_map = default_button_map_for_backend(m_current_backend_kind);
    m_layer_auto_dup_percent = 75;
    m_experimental_rumble_enabled = true;
    m_saved_layer_mode_state.valid = false;
    clear_lightgun_calibration();
    m_layer_order.clear();
    m_layer_enabled.clear();
    m_layer_ambilight.clear();
    m_layer_side_color.clear();
    presentation::ensure_layer_runtime_state_matches_config(m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
    sync_layer_capture_mask();
    refresh_quick_layer_presets();
    if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
    if (m_on_experimental_rumble_changed) m_on_experimental_rumble_changed(m_experimental_rumble_enabled);
    m_settings_panel_dirty = true;
    m_layer_panel_dirty    = true;
    m_ctrlmap_panel_dirty  = true;
    refresh_save_state_slots();
    m_save_state_panel_dirty = true;
    persist_save_automation_settings();
    if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
}

void OpenXrShell::wipe_all_settings() {
    if (m_library_live_preview_active) stop_library_live_preview();
    end_rom_preview_session(false);
    m_rom_preview.clear_cache();
    const std::string root_dir = get_settings_dir();
    if (!root_dir.empty()) settings_wipe_all_ini(root_dir);
    m_rom_browser.clear_recent();
    if (m_extracted_rom_cache_clearer) m_extracted_rom_cache_clearer();
    reset_settings();
    m_quick_settings_presets = make_quick_settings_presets();
    refresh_default_quick_settings_preset(m_current_backend_kind, m_config, m_quick_settings_presets);
    const std::string signature = quick_layer_signature(m_current_backend_kind, m_layer_filter_mode, m_config);
    m_quick_layer_presets = make_quick_layer_presets_for_signature(signature, m_config);
    m_quick_panel_dirty = true;
    m_main_menu_dirty = true;
    // Uses m_frame_predicted_time (OpenXR's predicted display time), the
    // same clock domain the VR render loop's periodic panel-rebuild check
    // compares against — mixing it with std::chrono::steady_clock here would
    // compare two unrelated clocks and could make the "Done" label appear to
    // never expire.
    constexpr XrTime k_wipe_done_label_duration = 2'000'000'000; // 2s
    m_wipe_settings_done_until = m_frame_predicted_time + k_wipe_done_label_duration;
    set_status("All settings, ROM preview thumbnails, and extracted-archive cache wiped; defaults restored.");
}

static std::string system_settings_dir(const std::string& root, BackendKind kind);

void OpenXrShell::save_settings(bool game_scope) {
    std::string dir = get_settings_dir();
    if (dir.empty()) return;
    mkdir(dir.c_str(), 0755);
    std::string system_dir = system_settings_dir(dir, m_current_backend_kind);
    mkdir(system_dir.c_str(), 0755);
    std::string path = system_dir + "/" + (game_scope ? (m_current_rom_name + ".ini") : "global.ini");
    // Only persist refresh_rate in global scope (it's a device-level setting, not per-game)
    float rr = game_scope ? 0.0f : m_desired_refresh_rate;
    const int filter_mode = is_snes_filter_capable_config(m_config) ? (int)m_layer_filter_mode : -1;
    // Serialize the unified menu's favorites (a session-only std::set until now)
    // into the one VrState field the flat .ini format can actually store.
    {
        std::string csv;
        for (const std::string& k : s_favorites) {
            if (!csv.empty()) csv += ';';
            csv += k;
        }
        m_vr_state.menu_favorites_csv = csv;
    }
    // Depth Arrangement widget (Layers > Stack): serialize the session-only
    // m_layer_slot_fraction array/UI floats into the VrState fields the .ini
    // format can store. Per-game (this file's own layer count is what the
    // fractions are meaningful against) — see load_settings' matching restore,
    // which discards the CSV if the loaded layer count doesn't match.
    {
        std::string csv;
        for (float f : m_layer_slot_fraction) {
            if (!csv.empty()) csv += ';';
            csv += std::to_string(f);
        }
        m_vr_state.layer_slot_fractions_csv = csv;
        m_vr_state.canvas_depth_meters_ui = m_canvas_depth_meters_ui;
        m_vr_state.thickness_overlap_ui = m_thickness_overlap_ui;
    }
    settings_save(path, m_vr_state, m_config, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color,
                  filter_mode, m_layer_auto_dup_percent, rr, m_experimental_rumble_enabled, &m_button_map,
                  m_current_backend_kind);
    if (!game_scope) {
        ui_theme_save(dir + "/ui_theme.ini", static_cast<int>(m_ui_theme));
    }
    m_saved_layer_mode_state.valid = is_snes_filter_capable_config(m_config);
    m_saved_layer_mode_state.mode = m_layer_filter_mode;
    m_saved_layer_mode_state.config = m_config;
    m_saved_layer_mode_state.order = m_layer_order;
    m_saved_layer_mode_state.enabled = m_layer_enabled;
    m_saved_layer_mode_state.ambilight = m_layer_ambilight;
    m_saved_layer_mode_state.side_color = m_layer_side_color;
    set_status(std::string(game_scope ? "Game" : "Global") + " settings saved.");
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static const char* backend_settings_subdir(BackendKind kind) {
    switch (kind) {
    case BackendKind::Genesis: return "genesis";
    case BackendKind::Nes:     return "nes";
    case BackendKind::Gba:     return "gba";
    case BackendKind::Gb:      return "gb";
    case BackendKind::Pce:     return "pce";
    case BackendKind::Psx:     return "psx";
    case BackendKind::Sms:     return "sms";
    default:                   return "snes";
    }
}

static std::string system_settings_dir(const std::string& root, BackendKind kind) {
    return root + "/" + backend_settings_subdir(kind);
}

static std::string quick_settings_presets_path(const std::string& root_dir) {
    return root_dir + "/quick_settings_presets.ini";
}

static std::string quick_layers_presets_dir(const std::string& root_dir, BackendKind kind) {
    return system_settings_dir(root_dir, kind) + "/quick_layers";
}

static std::string quick_layer_presets_path(const std::string& root_dir, BackendKind kind, const std::string& signature) {
    return quick_layers_presets_dir(root_dir, kind) + "/" + escape_preset_signature(signature) + ".ini";
}

static std::string join_ids(const std::vector<std::string>& ids) {
    std::string out;
    for (int i = 0; i < (int)ids.size(); ++i) {
        if (i) out.push_back(',');
        out += ids[i];
    }
    return out;
}

static std::vector<std::string> split_ids(const std::string& s) {
    std::vector<std::string> out;
    std::string current;
    for (char c : s) {
        if (c == ',') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

static std::string join_bools(const std::vector<bool>& values) {
    std::string out;
    out.reserve(values.size());
    for (bool v : values) out.push_back(v ? '1' : '0');
    return out;
}

static std::vector<bool> parse_bools(const std::string& s) {
    std::vector<bool> out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '0' || c == '1') out.push_back(c == '1');
    }
    return out;
}

static void write_quick_settings_presets_file(const std::string& path,
                                              const std::vector<OpenXrShell::QuickSettingsPreset>& presets) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    for (int i = 0; i < (int)presets.size(); ++i) {
        const auto& p = presets[i];
        std::fprintf(f, "name_%d=%s\n", i, p.name.c_str());
        std::fprintf(f, "canvas_x_%d=%.6f\n", i, p.canvas_x);
        std::fprintf(f, "canvas_y_%d=%.6f\n", i, p.canvas_y);
        std::fprintf(f, "canvas_az_%d=%.6f\n", i, p.canvas_az);
        std::fprintf(f, "canvas_el_%d=%.6f\n", i, p.canvas_el);
        std::fprintf(f, "canvas_scale_%d=%.6f\n", i, p.canvas_scale);
        std::fprintf(f, "near_depth_%d=%.6f\n", i, p.near_depth);
        std::fprintf(f, "far_depth_%d=%.6f\n", i, p.far_depth);
        std::fprintf(f, "quad_width_%d=%.6f\n", i, p.quad_width);
        std::fprintf(f, "copy_count_%d=%d\n", i, p.copy_count);
        std::fprintf(f, "immersive_beta_enabled_%d=%d\n", i, p.immersive_beta_enabled ? 1 : 0);
        std::fprintf(f, "upscale_%d=%d\n", i, (int)p.upscale_mode);
        std::fprintf(f, "ambilight_%d=%d\n", i, p.ambilight ? 1 : 0);
        std::fprintf(f, "passthrough_%d=%d\n", i, p.passthrough ? 1 : 0);
        std::fprintf(f, "depth_mode_%d=%d\n", i, (int)p.depth_mode);
        std::fprintf(f, "layers_3d_%d=%d\n", i, p.layers_3d ? 1 : 0);
        std::fprintf(f, "gamma_%d=%.6f\n", i, p.gamma);
        std::fprintf(f, "contrast_%d=%.6f\n", i, p.contrast);
        std::fprintf(f, "saturation_%d=%.6f\n", i, p.saturation);
        std::fprintf(f, "brightness_%d=%.6f\n", i, p.brightness);
        std::fprintf(f, "perspective_comp_%d=%d\n", i, p.perspective_comp ? 1 : 0);
        std::fprintf(f, "environment_sphere_mode_%d=%d\n", i, (int)p.environment_sphere_mode);
    }
    fclose(f);
}

static void load_quick_settings_presets_file(const std::string& path,
                                             std::vector<OpenXrShell::QuickSettingsPreset>& presets) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const std::string key = line;
        std::string value = eq + 1;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        int idx = -1;
        if (std::sscanf(key.c_str(), "name_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].name = sanitize_preset_name(value, presets[idx].name);
        } else if (std::sscanf(key.c_str(), "canvas_x_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].canvas_x = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "canvas_y_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].canvas_y = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "canvas_az_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].canvas_az = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "canvas_el_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].canvas_el = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "canvas_scale_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].canvas_scale = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "near_depth_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].near_depth = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "far_depth_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].far_depth = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "quad_width_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].quad_width = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "copy_count_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].copy_count = std::atoi(value.c_str());
        } else if (std::sscanf(key.c_str(), "immersive_beta_enabled_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].immersive_beta_enabled = std::atoi(value.c_str()) != 0;
        } else if (std::sscanf(key.c_str(), "upscale_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].upscale_mode = (UpscaleMode)std::clamp(std::atoi(value.c_str()), 0, 2);
        } else if (std::sscanf(key.c_str(), "ambilight_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].ambilight = std::atoi(value.c_str()) != 0;
        } else if (std::sscanf(key.c_str(), "passthrough_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].passthrough = std::atoi(value.c_str()) != 0;
        } else if (std::sscanf(key.c_str(), "depth_mode_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].depth_mode = (DepthMode)std::clamp(std::atoi(value.c_str()), 0, 5);
        } else if (std::sscanf(key.c_str(), "depthmap_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].depth_mode = std::atoi(value.c_str()) != 0 ? DepthMode::WholeLayer : DepthMode::Off;
        } else if (std::sscanf(key.c_str(), "layers_3d_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].layers_3d = std::atoi(value.c_str()) != 0;
        } else if (std::sscanf(key.c_str(), "gamma_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].gamma = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "contrast_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].contrast = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "saturation_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].saturation = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "brightness_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].brightness = (float)std::atof(value.c_str());
        } else if (std::sscanf(key.c_str(), "perspective_comp_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].perspective_comp = std::atoi(value.c_str()) != 0;
        } else if (std::sscanf(key.c_str(), "environment_sphere_mode_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].environment_sphere_mode = (EnvironmentSphereMode)std::clamp(std::atoi(value.c_str()), 0, 3);
        } else if (std::sscanf(key.c_str(), "sky_dome_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].environment_sphere_mode = std::atoi(value.c_str()) != 0 ? EnvironmentSphereMode::SkyOnly : EnvironmentSphereMode::Off;
        }
    }
    fclose(f);
}

static void write_quick_layer_presets_file(const std::string& path,
                                           const std::vector<OpenXrShell::QuickLayerPreset>& presets) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    for (int i = 0; i < (int)presets.size(); ++i) {
        const auto& p = presets[i];
        std::fprintf(f, "name_%d=%s\n", i, p.name.c_str());
        std::fprintf(f, "ordered_ids_%d=%s\n", i, join_ids(p.ordered_ids).c_str());
        std::fprintf(f, "enabled_%d=%s\n", i, join_bools(p.enabled).c_str());
        std::fprintf(f, "ambilight_%d=%s\n", i, join_bools(p.ambilight).c_str());
        if (!p.depths.empty()) {
            std::string ds;
            for (int j = 0; j < (int)p.depths.size(); ++j) {
                if (j > 0) ds += ',';
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.4f", p.depths[j]);
                ds += buf;
            }
            std::fprintf(f, "depths_%d=%s\n", i, ds.c_str());
        }
    }
    fclose(f);
}

static void load_quick_layer_presets_file(const std::string& path,
                                          std::vector<OpenXrShell::QuickLayerPreset>& presets) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const std::string key = line;
        std::string value = eq + 1;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        int idx = -1;
        if (std::sscanf(key.c_str(), "name_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].name = sanitize_preset_name(value, presets[idx].name);
        } else if (std::sscanf(key.c_str(), "ordered_ids_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].ordered_ids = split_ids(value);
        } else if (std::sscanf(key.c_str(), "enabled_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].enabled = parse_bools(value);
        } else if (std::sscanf(key.c_str(), "ambilight_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].ambilight = parse_bools(value);
        } else if (std::sscanf(key.c_str(), "depths_%d", &idx) == 1 && idx >= 0 && idx < (int)presets.size()) {
            presets[idx].depths.clear();
            std::string tok;
            for (size_t pos = 0, end; pos <= value.size(); pos = end + 1) {
                end = value.find(',', pos);
                if (end == std::string::npos) end = value.size();
                tok = value.substr(pos, end - pos);
                if (!tok.empty()) {
                    try { presets[idx].depths.push_back(std::stof(tok)); } catch (...) {}
                }
            }
        }
    }
    fclose(f);
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
    const std::string system_dir = system_settings_dir(dir, m_current_backend_kind);
    if (!game_scope) {
        std::string path = system_dir + "/global.ini";
        if (!file_exists(path.c_str())) path = dir + "/global.ini";
        int loaded_theme = 0;
        if (ui_theme_load(dir + "/ui_theme.ini", loaded_theme)) {
            m_ui_theme = clamp_ui_theme(loaded_theme);
        } else if (file_exists(path.c_str())) {
            // Existing installations without the new preference keep the proven Classic UI.
            m_ui_theme = UiThemeId::Classic;
        }
        // The saved choice was only ever applied to the OLD Kotlin panels'
        // dirty-texture-redraw path — the new ImGui menu's own style (imgui_bridge's
        // apply_theme()) was never re-synced on load, so a previously-picked theme
        // showed as selected in the picker but the menu still rendered in whatever
        // theme happened to be the ImGui default until you re-picked it by hand.
        m_impl->imgui_bridge.apply_theme((int)m_ui_theme);
        m_themes_panel_dirty = true;
        float loaded_rr = -1.0f;
        bool loaded_rumble = true;
        int loaded_mode = -1;
        m_layer_auto_dup_percent = 75;
        LayerFilterMode sniffed_mode = LayerFilterMode::Hybrid;
        int sniffed_layers = -1;
        sniff_settings_layer_mode(path, sniffed_mode, sniffed_layers);
        if (is_snes_filter_capable_config(m_config)) {
            m_layer_filter_mode = sniffed_mode;
            m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
        }
        if (!settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color,
                           &loaded_mode, &m_layer_auto_dup_percent, &loaded_rr, &loaded_rumble, &m_button_map,
                           m_current_backend_kind)) {
            // Keep the already-loaded theme visible even when no global settings file exists yet.
            if (m_vm && m_activity_global) {
                JNIEnv* env = nullptr;
                bool detach = false;
                if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                    if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
                }
                if (env) {
                    jclass cls = env->GetObjectClass(m_activity_global);
                    jmethodID mid = env->GetMethodID(cls, "setUiThemeId", "(I)V");
                    if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_ui_theme);
                    env->DeleteLocalRef(cls);
                    if (detach) m_vm->DetachCurrentThread();
                }
            }
            return;
        }
        if (loaded_mode >= 0 && loaded_mode <= (int)LayerFilterMode::Hybrid && is_snes_filter_capable_config(m_config)) {
            m_layer_filter_mode = (LayerFilterMode)loaded_mode;
            m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
            m_layer_order.clear();
            m_layer_enabled.clear();
            m_layer_ambilight.clear();
        m_layer_side_color.clear();
            settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color,
                          nullptr, &m_layer_auto_dup_percent, &loaded_rr, &loaded_rumble, &m_button_map,
                          m_current_backend_kind);
        }
        if (!is_snes_filter_capable_config(m_config) &&
            sniffed_layers > 0 &&
            sniffed_layers != (int)m_config.layers.size()) {
            m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
            m_layer_order.clear();
            m_layer_enabled.clear();
            m_layer_ambilight.clear();
        m_layer_side_color.clear();
        }
        m_experimental_rumble_enabled = loaded_rumble;
        if (loaded_rr >= 0.0f) {
            m_desired_refresh_rate = loaded_rr;
            m_apply_refresh_pending = true;
        }
        presentation::ensure_layer_runtime_state_matches_config(
            m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
    } else {
        // Try exact match first (e.g., "Super Mario World (USA).ini")
        std::string path = system_dir + "/" + m_current_rom_name + ".ini";
        if (!file_exists(path.c_str())) path = dir + "/" + m_current_rom_name + ".ini";
        bool loaded = file_exists(path.c_str());
        float loaded_rr = -1.0f;
        bool loaded_rumble = m_experimental_rumble_enabled;
        int loaded_mode = -1;
        m_layer_auto_dup_percent = 75;
        if (loaded) {
            LayerFilterMode sniffed_mode = LayerFilterMode::Hybrid;
            int sniffed_layers = -1;
            sniff_settings_layer_mode(path, sniffed_mode, sniffed_layers);
            if (is_snes_filter_capable_config(m_config)) {
                m_layer_filter_mode = sniffed_mode;
                m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
            }
            loaded = settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled,
                                   m_layer_ambilight, m_layer_side_color, &loaded_mode, &m_layer_auto_dup_percent, nullptr, &loaded_rumble, &m_button_map,
                                   m_current_backend_kind);
            if (loaded && loaded_mode >= 0 && loaded_mode <= (int)LayerFilterMode::Hybrid && is_snes_filter_capable_config(m_config)) {
                m_layer_filter_mode = (LayerFilterMode)loaded_mode;
                m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
                m_layer_order.clear();
                m_layer_enabled.clear();
                m_layer_ambilight.clear();
        m_layer_side_color.clear();
                loaded = settings_load(path, m_vr_state, m_config, m_layer_order, m_layer_enabled,
                                       m_layer_ambilight, m_layer_side_color, nullptr, &m_layer_auto_dup_percent, nullptr, &loaded_rumble, &m_button_map,
                                       m_current_backend_kind);
            }
            if (loaded &&
                !is_snes_filter_capable_config(m_config) &&
                sniffed_layers > 0 &&
                sniffed_layers != (int)m_config.layers.size()) {
                m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
                m_layer_order.clear();
                m_layer_enabled.clear();
                m_layer_ambilight.clear();
                m_layer_side_color.clear();
            }
        }
        // If no exact match and we have header-derived game name, scan for any version match
        if (!loaded && !m_current_game_name.empty()) {
            std::string base_key = strip_ini_version(m_current_game_name);
            // Scan settings directory for matching .ini files
            std::string scan_dir = system_dir;
            void* dir_handle = opendir(scan_dir.c_str());
            if (!dir_handle) {
                scan_dir = dir;
                dir_handle = opendir(scan_dir.c_str());
            }
            if (dir_handle) {
                struct dirent* entry;
                while ((entry = readdir(static_cast<DIR*>(dir_handle))) != nullptr) {
                    std::string fname = entry->d_name;
                    if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".ini") {
                        std::string ini_name = fname.substr(0, fname.size() - 4);
                        if (strip_ini_version(ini_name) == base_key) {
                            path = scan_dir + "/" + fname;
                            LayerFilterMode sniffed_mode = LayerFilterMode::Hybrid;
                            int sniffed_layers = -1;
                            sniff_settings_layer_mode(path, sniffed_mode, sniffed_layers);
                            if (is_snes_filter_capable_config(m_config)) {
                                m_layer_filter_mode = sniffed_mode;
                                m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
                            }
                            loaded = settings_load(path, m_vr_state, m_config, m_layer_order,
                                                 m_layer_enabled, m_layer_ambilight, m_layer_side_color,
                                                 &loaded_mode, &m_layer_auto_dup_percent, nullptr, &loaded_rumble, &m_button_map,
                                                 m_current_backend_kind);
                            if (loaded && loaded_mode >= 0 && loaded_mode <= (int)LayerFilterMode::Hybrid
                                && is_snes_filter_capable_config(m_config)) {
                                m_layer_filter_mode = (LayerFilterMode)loaded_mode;
                                m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
                                m_layer_order.clear();
                                m_layer_enabled.clear();
                                m_layer_ambilight.clear();
                                m_layer_side_color.clear();
                                loaded = settings_load(path, m_vr_state, m_config, m_layer_order,
                                                       m_layer_enabled, m_layer_ambilight, m_layer_side_color,
                                                       nullptr, &m_layer_auto_dup_percent, nullptr, &loaded_rumble, &m_button_map,
                                                       m_current_backend_kind);
                            }
                            if (loaded &&
                                !is_snes_filter_capable_config(m_config) &&
                                sniffed_layers > 0 &&
                                sniffed_layers != (int)m_config.layers.size()) {
                                m_config = presentation::default_config_for_backend(m_current_backend_kind, (int)m_layer_filter_mode);
                                m_layer_order.clear();
                                m_layer_enabled.clear();
                                m_layer_ambilight.clear();
        m_layer_side_color.clear();
                            }
                            break;
                        }
                    }
                }
                closedir(static_cast<DIR*>(dir_handle));
            }
        }
        if (!loaded) return; // no settings found
        m_experimental_rumble_enabled = loaded_rumble;
        presentation::ensure_layer_runtime_state_matches_config(
            m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
        // Depth Arrangement widget: restore m_layer_slot_fraction from the CSV
        // saved for this game, but only if the count matches the layer count
        // just loaded — a mismatch (different core, or the widget was never
        // touched last time this game was saved) means the CSV isn't meaningful
        // for this layer stack, so fall through to the widget's own default
        // even-spacing re-seed (draw_depth_arrangement_widget's size-mismatch
        // check) rather than misapplying stale fractions to the wrong layers.
        m_canvas_depth_meters_ui = m_vr_state.canvas_depth_meters_ui;
        m_thickness_overlap_ui   = m_vr_state.thickness_overlap_ui;
        if (!m_vr_state.layer_slot_fractions_csv.empty()) {
            std::vector<float> loaded_fractions;
            std::stringstream csv(m_vr_state.layer_slot_fractions_csv);
            std::string tok;
            while (std::getline(csv, tok, ';')) {
                if (!tok.empty()) loaded_fractions.push_back((float)atof(tok.c_str()));
            }
            if (loaded_fractions.size() == m_layer_order.size()) {
                m_layer_slot_fraction = std::move(loaded_fractions);
            }
        }
    }
    if (m_vm && m_activity_global) {
        JNIEnv* env = nullptr;
        bool detach = false;
        if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
        }
        if (env) {
            jclass cls = env->GetObjectClass(m_activity_global);
            jmethodID mid = env->GetMethodID(cls, "setUiThemeId", "(I)V");
            if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_ui_theme);
            env->DeleteLocalRef(cls);
            if (detach) m_vm->DetachCurrentThread();
        }
    }
    sync_layer_capture_mask();
    refresh_quick_layer_presets();
    m_saved_layer_mode_state.valid = is_snes_filter_capable_config(m_config);
    m_saved_layer_mode_state.mode = m_layer_filter_mode;
    m_saved_layer_mode_state.config = m_config;
    m_saved_layer_mode_state.order = m_layer_order;
    m_saved_layer_mode_state.enabled = m_layer_enabled;
    m_saved_layer_mode_state.ambilight = m_layer_ambilight;
    m_saved_layer_mode_state.side_color = m_layer_side_color;
    m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
    if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
    if (m_on_experimental_rumble_changed) m_on_experimental_rumble_changed(m_experimental_rumble_enabled);
    // Applies a saved "off" preference (Kotlin's own bgmEnabled defaults true,
    // matching VrState::bgm_enabled's default, so there's nothing to do when
    // it's on). Only ever needs to disable — the Audio > Music toggle itself
    // handles the enable case in-session, and there's no legitimate path
    // where a fresh load should resurrect music the user turned off.
    if (!m_vr_state.bgm_enabled) call_activity_void("bgmDisable");
    // Kotlin's bgmUserVolume has its own default (see QuestVrActivity.kt) —
    // push VrState::bgm_volume down whenever it differs so a fresh install's
    // default (or a saved value) always wins over Kotlin's own.
    if (m_vr_state.bgm_volume != 1.0f) call_activity_float("bgmSetVolume", m_vr_state.bgm_volume);
    apply_audio_channel_volumes();
    apply_auto_frame_skip();
    apply_psx_gpu_resolution();
    apply_psx_texture_filter();
    m_settings_panel_dirty  = true;
    m_layer_panel_dirty     = true;
    m_ctrlmap_panel_dirty   = true;
    m_save_state_panel_dirty = true;
    if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
    set_status(std::string(game_scope ? "Game" : "Global") + " settings loaded.");
}

void OpenXrShell::refresh_save_state_slots() {
    m_save_state_slots.assign(k_save_state_slot_count, {});
    for (int i = 0; i < k_save_state_slot_count; ++i) {
        m_save_state_slots[i].label = save_state_default_label(i);
    }

    if (m_settings_dir.empty()) {
        m_settings_dir = get_settings_dir();
    }
    if (m_settings_dir.empty() || m_current_rom_name.empty()) return;

    const std::string root_dir = m_settings_dir + "/" + backend_storage_subdir(m_current_backend_kind);
    const std::string state_dir = root_dir + "/savestates/" + m_current_rom_name;
    for (int i = 0; i < k_save_state_slot_count; ++i) {
        const std::string path = state_dir + "/slot" + std::to_string(i + 1) + ".state";
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) continue;
        m_save_state_slots[i].occupied = true;
        m_save_state_slots[i].timestamp_epoch_seconds = (std::uint64_t)st.st_mtime;
        const std::string label = format_save_state_timestamp(st.st_mtime);
        if (!label.empty()) m_save_state_slots[i].label = label;
    }
}

void OpenXrShell::apply_audio_channel_volumes() {
    AudioChannelVolumeApplier fn;
    { std::lock_guard<std::mutex> lk(m_mutex); fn = m_audio_channel_volume_applier; }
    if (fn) fn(m_vr_state);
}

void OpenXrShell::apply_auto_frame_skip() {
    AutoFrameSkipApplier fn;
    { std::lock_guard<std::mutex> lk(m_mutex); fn = m_auto_frame_skip_applier; }
    if (fn) fn(m_vr_state);
}

void OpenXrShell::apply_psx_render_path() {
    PsxRenderPathCtrl fn;
    { std::lock_guard<std::mutex> lk(m_mutex); fn = m_psx_render_path_ctrl; }
    if (fn) fn(m_vr_state.psx_render_path);
}

void OpenXrShell::apply_psx_texture_filter() {
    PsxTextureFilterApplier fn;
    { std::lock_guard<std::mutex> lk(m_mutex); fn = m_psx_texture_filter_applier; }
    if (fn) fn(m_vr_state.psx_texture_filter);
}

void OpenXrShell::apply_vr_resolution_scale() {
    const float snapped = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
    m_vr_state.vr_resolution_scale = snapped;
    m_pending_swapchain_rebuild = true;
}

void OpenXrShell::apply_psx_gpu_resolution() {
    PsxGpuResolutionApplier fn;
    { std::lock_guard<std::mutex> lk(m_mutex); fn = m_psx_gpu_resolution_applier; }
    if (fn) fn(m_vr_state.psx_gpu_resolution);
}

void OpenXrShell::close_current_rom(int save_slot) {
    if (m_current_rom_name.empty()) return;
    if (save_slot >= 0) {
        std::string err;
        if (!save_state_to_slot(save_slot, err)) {
            set_status("Save failed, closing anyway: " + err);
        }
    }
    RomCloser closer_fn;
    { std::lock_guard<std::mutex> lk(m_mutex); closer_fn = m_rom_closer; }
    if (closer_fn) closer_fn();
    m_current_rom_name.clear();
    m_current_game_name.clear();
    refresh_save_state_slots();
    set_status("ROM closed.");
}

bool OpenXrShell::save_state_to_slot(int slot, std::string& error_out) {
    if (slot < 0 || slot >= k_save_state_slot_count) {
        error_out = "Invalid save slot.";
        return false;
    }
    if (m_current_rom_name.empty()) {
        error_out = "Load a ROM before using save states.";
        return false;
    }
    const std::string dir = get_settings_dir();
    if (dir.empty()) {
        error_out = "Settings directory unavailable.";
        return false;
    }
    const std::string path =
        dir + "/" + backend_storage_subdir(m_current_backend_kind) + "/savestates/" +
        m_current_rom_name + "/slot" + std::to_string(slot + 1) + ".state";
    return save_state_to_path(path, error_out);
}

bool OpenXrShell::load_state_from_slot(int slot, std::string& error_out) {
    if (slot < 0 || slot >= k_save_state_slot_count) {
        error_out = "Invalid save slot.";
        return false;
    }
    if (m_current_rom_name.empty()) {
        error_out = "Load a ROM before using save states.";
        return false;
    }
    const std::string dir = get_settings_dir();
    if (dir.empty()) {
        error_out = "Settings directory unavailable.";
        return false;
    }
    const std::string path =
        dir + "/" + backend_storage_subdir(m_current_backend_kind) + "/savestates/" +
        m_current_rom_name + "/slot" + std::to_string(slot + 1) + ".state";
    return load_state_from_path(path, error_out);
}

bool OpenXrShell::save_state_to_path(const std::string& path, std::string& error_out) {
    SaveStateCapture capture;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        capture = m_save_state_capture;
    }
    if (!capture) {
        error_out = "Save-state capture unavailable.";
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!capture(bytes, error_out)) return false;
    if (bytes.empty()) {
        error_out = "Savestate capture returned no data.";
        return false;
    }

    if (!m_current_rom_name.empty()) {
        const std::string dir = get_settings_dir();
        if (!dir.empty()) {
            const std::string backend_dir = dir + "/" + backend_storage_subdir(m_current_backend_kind);
            const std::string save_root = backend_dir + "/savestates";
            const std::string rom_dir = save_root + "/" + m_current_rom_name;
            mkdir(backend_dir.c_str(), 0755);
            mkdir(save_root.c_str(), 0755);
            mkdir(rom_dir.c_str(), 0755);
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error_out = "Failed to open savestate file for writing.";
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    out.close();
    if (!out) {
        error_out = "Failed to write savestate file.";
        return false;
    }

    refresh_save_state_slots();
    m_save_state_panel_dirty = true;
    error_out.clear();
    return true;
}

bool OpenXrShell::load_state_from_path(const std::string& path, std::string& error_out) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        error_out = "Savestate file is missing or empty.";
        return false;
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error_out = "Failed to open savestate file.";
        return false;
    }
    const std::streamsize size = in.tellg();
    if (size <= 0) {
        error_out = "Savestate file is empty.";
        return false;
    }
    std::vector<uint8_t> bytes((std::size_t)size, 0);
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        error_out = "Failed to read savestate file.";
        return false;
    }

    SaveStateApply apply;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        apply = m_save_state_apply;
    }
    if (!apply) {
        error_out = "Save-state load unavailable.";
        return false;
    }
    if (!apply(bytes.data(), bytes.size(), error_out)) return false;

    refresh_save_state_slots();
    m_save_state_panel_dirty = true;
    error_out.clear();
    return true;
}

bool OpenXrShell::try_load_latest_state(std::string& loaded_name_out, std::string& error_out, bool& found_any) {
    found_any = false;
    loaded_name_out.clear();
    if (m_current_rom_name.empty()) {
        error_out = "Load a ROM before using save states.";
        return false;
    }

    const std::string dir = get_settings_dir();
    if (dir.empty()) {
        error_out = "Settings directory unavailable.";
        return false;
    }
    const std::string state_dir =
        dir + "/" + backend_storage_subdir(m_current_backend_kind) + "/savestates/" + m_current_rom_name;

    std::string best_path;
    std::string best_name;
    std::time_t best_mtime = 0;
    auto consider = [&](const std::string& name) {
        const std::string path = state_dir + "/" + name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) return;
        if (!found_any || st.st_mtime > best_mtime) {
            found_any = true;
            best_mtime = st.st_mtime;
            best_path = path;
            best_name = name;
        }
    };

    consider(k_autosave_file_name);
    for (int i = 0; i < k_save_state_slot_count; ++i) {
        consider("slot" + std::to_string(i + 1) + ".state");
    }

    if (!found_any) {
        error_out.clear();
        return false;
    }
    if (!load_state_from_path(best_path, error_out)) return false;
    loaded_name_out = best_name;
    return true;
}

void OpenXrShell::maybe_run_autosave() {
    if (m_autosave_interval_seconds <= 0 || m_current_rom_name.empty()) return;
    if (m_menu_open || m_emu_frozen_display) return;
    if (m_autosave_in_progress.load(std::memory_order_relaxed)) return;

    const std::uint64_t now_ms = monotonic_time_ms();
    const std::uint64_t interval_ms = (std::uint64_t)m_autosave_interval_seconds * 1000ull;
    if (m_last_autosave_time_ms != 0 && (now_ms - m_last_autosave_time_ms) < interval_ms) return;

    const std::string dir = get_settings_dir();
    if (dir.empty()) return;

    SaveStateCapture capture;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        capture = m_save_state_capture;
    }
    if (!capture) return;

    std::string path =
        dir + "/" + backend_storage_subdir(m_current_backend_kind) + "/savestates/" +
        m_current_rom_name + "/" + k_autosave_file_name;

    m_last_autosave_time_ms = now_ms;
    m_autosave_in_progress.store(true, std::memory_order_relaxed);

    std::thread([this,
                 path    = std::move(path),
                 capture = std::move(capture)]() {
        std::vector<uint8_t> bytes;
        std::string err;
        if (capture(bytes, err) && !bytes.empty()) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          (std::streamsize)bytes.size());
                out.close();
            }
        }
        m_autosave_in_progress.store(false, std::memory_order_relaxed);
    }).detach();
}

// ============================================================
// open_rom_menu — place panel in front of HMD, scan ROM dir
// ============================================================
float OpenXrShell::rom_transition_alpha() {
    constexpr std::uint64_t kFadeOutMs = 360;
    constexpr std::uint64_t kFadeInMs  = 480;
    if (m_rom_transition_phase == RomTransitionPhase::None) return 0.0f;

    const std::uint64_t elapsed = monotonic_time_ms() - m_rom_transition_start_ms;
    if (m_rom_transition_phase == RomTransitionPhase::FadeOut) {
        return std::clamp((float)elapsed / (float)kFadeOutMs, 0.0f, 1.0f);
    }

    const float progress = std::clamp((float)elapsed / (float)kFadeInMs, 0.0f, 1.0f);
    if (progress >= 1.0f) {
        m_rom_transition_phase = RomTransitionPhase::None;
        m_rom_transition_start_ms = 0;
        return 0.0f;
    }
    return 1.0f - progress;
}

void OpenXrShell::start_async_rom_preparation(const std::string& path) {
    if (path.empty() || m_rom_load_in_progress.exchange(true, std::memory_order_acq_rel)) return;

    m_rom_transition_phase = RomTransitionPhase::FadeOut;
    m_rom_transition_start_ms = monotonic_time_ms();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_load_path = path;
        m_rom_load_message = "STOPPING THUMBNAILER";
        m_rom_load_prepared_path.clear();
        m_rom_load_error.clear();
        m_rom_load_ok = false;
        m_rom_load_extract_history.clear();
    }
    m_rom_load_completion_pending.store(false, std::memory_order_release);
    m_rom_load_panel_dirty.store(true, std::memory_order_release);
    LOGI("ROM async preparation: begin raw=%s", path.c_str());

    m_rom_load_thread = std::thread([this, path] {
        // This join happens off the XR thread. If the thumbnail worker is
        // currently decompressing a large archive, the headset keeps
        // rendering the loading panel instead of appearing frozen.
        m_rom_preview.clear_live();
        m_rom_preview.stop();

        set_rom_load_stage(path, "EXTRACTING ROM");
        RomPreparer preparer;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            preparer = m_rom_preparer;
        }
        std::string prepared = path;
        if (preparer) {
            try {
                prepared = preparer(path);
            } catch (...) {
                prepared = path;
                LOGE("ROM async preparation: preparer threw for '%s'", path.c_str());
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rom_load_prepared_path = prepared;
        }
        set_rom_load_stage(path, "READY TO START");
        LOGI("ROM async preparation: ready raw=%s prepared=%s", path.c_str(), prepared.c_str());
        m_rom_load_completion_pending.store(true, std::memory_order_release);
        m_rom_load_panel_dirty.store(true, std::memory_order_release);
    });
}

void OpenXrShell::poll_rom_load_completion() {
    if (!m_rom_load_completion_pending.load(std::memory_order_acquire)) return;
    // Let the fade-out reach true black before joining the worker or calling
    // retro_load_game(), both of which can briefly stall the XR thread.
    if (rom_transition_alpha() < 1.0f) return;
    if (!m_rom_load_completion_pending.exchange(false, std::memory_order_acq_rel)) return;
    if (m_rom_load_thread.joinable()) m_rom_load_thread.join();

    std::string raw_path;
    std::string prepared_path;
    RomLoader loader;
    RomPreparedPathPublisher publisher;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        raw_path = m_rom_load_path;
        prepared_path = m_rom_load_prepared_path;
        loader = m_rom_loader;
        publisher = m_rom_prepared_path_publisher;
        m_rom_load_message = "STARTING GAME";
    }
    m_rom_load_panel_dirty.store(true, std::memory_order_release);

    // The preview worker is now stopped, so this is quick and can safely
    // commit the browser transition on the XR thread. The prepared-path
    // handoff makes the loader reuse the extracted file instead of unpacking
    // the archive a second time.
    end_rom_preview_session(true);
    call_activity_void("bgmStopImmediate");
    if (publisher) publisher(raw_path, prepared_path.empty() ? raw_path : prepared_path);
    std::string error;
    const bool ok = loader && loader(raw_path, error);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_load_ok = ok;
        m_rom_load_error = error;
    }
    m_rom_load_in_progress.store(false, std::memory_order_release);
    m_rom_load_release_required.store(true, std::memory_order_release);
    m_rom_load_panel_dirty.store(true, std::memory_order_release);
    m_rom_transition_phase = RomTransitionPhase::FadeIn;
    m_rom_transition_start_ms = monotonic_time_ms();

    if (ok) {
        m_rom_browser.record_recent(raw_path);
        set_status("Loaded: " + raw_path.substr(raw_path.rfind('/') + 1));
        m_menu_open = false;
        m_ctrlmap_mode = false;
        m_active_sub_panel = 0;
        m_laser_hit = false;
        m_emu_frozen_display = false;
    } else {
        set_status("Load failed: " + error);
        // Keep the browser open so the user can retry or choose another ROM.
        m_laser_hit = false;
    }
    LOGI("ROM async preparation: load finished ok=%d raw=%s err=%s",
         ok ? 1 : 0, raw_path.c_str(), error.c_str());
}

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

    m_rom_browser.set_recent_store(get_settings_dir());
    m_rom_browser.scan(m_rom_dir);
    // Enter the ROM-browser session for every entry path, including the
    // controller menu button used while a game is running.  Previously this
    // was only done from the main-menu "Open ROM" row, which left the active
    // emulator running behind the browser (and allowed it to contend with
    // thumbnail work).
    begin_rom_preview_session();

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

    m_save_state_panel_pose.position    = { cx, cy, cz };
    m_save_state_panel_pose.orientation = orient;
    m_save_state_panel_dirty            = true;
    m_save_state_panel_hovered          = -1;
    refresh_save_state_slots();

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
    m_settings_return_to_quick = false;
    m_active_sub_panel = 0; // show main menu
    m_laser_hit  = false;
    m_laser_panel = -1;
}

bool OpenXrShell::begin_rom_preview_session() {
    if (m_rom_preview_session_active) return false;
    // A previous session may have been interrupted while its worker was
    // between jobs. Stop it before unloading/replacing the gameplay backend;
    // configure() will start a fresh worker below when previews are enabled.
    m_rom_preview.stop();
    // The emulator thread clears the published double-buffer, but the XR
    // shell has its own processed-frame cache. Drop it at the same transition
    // so no pixels from the just-dismissed game can be submitted while the
    // shelf is open.
    reset_emulation_cache_for_rom_change();
    RomPreviewCapture capture;
    RomPreviewSessionBegin begin;
    { std::lock_guard<std::mutex> lk(m_mutex);
      capture = m_rom_preview_capture;
      begin = m_rom_preview_begin; }
    std::string error;
    if (begin && !begin(error)) {
        if (!error.empty()) set_status("3D shelf unavailable: " + error);
        return false;
    }
    m_rom_preview_session_active = true;
    // Unloading the game is the browser-session guarantee.  Thumbnail
    // capture is an optional feature and must not decide whether the game is
    // suspended while the user browses.
    if (kDebugDisableRomThumbnailer) {
        m_rom_preview.set_enabled(false);
    } else if (m_vr_state.rom_preview_enabled && capture) {
        m_rom_preview.configure(get_settings_dir(), std::move(capture));
        m_rom_preview.set_enabled(true);
        refresh_rom_preview_jobs();
    }
    return true;
}

void OpenXrShell::refresh_rom_preview_jobs() {
    if (!m_rom_preview_session_active || !m_vr_state.rom_preview_enabled) return;
    m_rom_preview.set_visible(m_rom_browser.visible_rom_paths());
}

void OpenXrShell::enter_folder_and_queue_caching() {
    // Folder navigation is always instant, and so is live hover-preview.
    // This used to hold the live preview off until every ROM in the folder
    // had settled, which on a 60-ROM folder meant hovering did nothing at
    // all for a very long time. A live request now genuinely preempts the
    // in-flight background job (see enqueue_locked's m_force_cancel), so
    // hovering can be served immediately and caching simply resumes as soon
    // as the laser leaves the shelf.
    const std::string target = m_rom_browser.peek_hovered_target_dir();
    m_rom_browser.enter_hovered();
    if (kDebugForceRecacheOnFolderOpen) m_rom_preview.invalidate_all();
    // Drop any not-yet-started background cache jobs left over from the
    // previous folder, and interrupt whatever ROM is currently being
    // captured (even one immune to ordinary hover-preemption) — otherwise it
    // keeps running on the folder we just left for its entire capture
    // window before this folder's jobs ever get a turn, which is what made
    // the thumbnailer look "stuck" after switching folders.
    m_rom_preview.clear_pending_background_jobs();
    refresh_rom_preview_jobs();

    if (!m_rom_preview_session_active || !m_vr_state.rom_preview_enabled || target.empty()) return;
    auto paths = m_rom_browser.list_dir_rom_paths(target);
    if (paths.empty()) return;
    m_rom_preview.set_visible(paths);
}

void OpenXrShell::end_rom_preview_session(bool committed) {
    if (!m_rom_preview_session_active) return;
    m_rom_preview.stop();
    RomPreviewSessionEnd end;
    { std::lock_guard<std::mutex> lk(m_mutex); end = m_rom_preview_end; }
    if (end) end(committed);
    m_rom_preview_session_active = false;
}

// ============================================================
// Flat Library list hover-dwell live preview — reuses the exact same
// RomPreviewManager/session/BGM-duck machinery the 3D Shelf's laser-hover
// path drives (see the k_panel_browser branch in poll_actions() and
// m_bgm_live_active's consumer in draw_unified_menu()), just triggered by a
// 0.5s ImGui hover-dwell on a row in draw_library_rom_list() instead of a
// world-space laser hit. m_library_preview_session_active tracks whether
// THIS call site opened the session, so it only tears it down if it was the
// one that opened it (the 3D Shelf is assumed mutually exclusive with the
// flat list being drawn at all).
// ============================================================
void OpenXrShell::start_library_live_preview(const std::string& path) {
    if (path.empty()) return;
    if (!m_rom_preview_session_active) {
        if (!begin_rom_preview_session()) {
            m_library_live_preview_active = false;
            return;
        }
        m_library_preview_session_active = true;
    }
    m_rom_preview.request_live(path);
    m_bgm_live_active = true;
    m_library_live_preview_active = true;
}

void OpenXrShell::update_library_live_preview() {
    if (!m_library_live_preview_active) return;
    RomPreviewSnapshot snap;
    if (m_rom_preview.get_live(snap) && m_rom_preview.live_path() == m_library_preview_path &&
        !snap.layers.empty()) {
        const RomPreviewLayer& base = snap.layers[0];
        if (m_impl && base.width > 0 && base.height > 0 && !base.rgba.empty()) {
            m_library_preview_has_frame = true;
            m_library_preview_layer_count = (int)snap.layers.size();
            // One texture per layer, for the world-space depth diorama
            // (build_library_preview_diorama()). There is no flat copy any
            // more: ImGui can only show one flat image, so the quads are the
            // preview rather than a 3D garnish next to a 2D duplicate.
            int uploaded = 0;
            m_library_preview_aspect =
                (base.height > 0) ? (float)base.width / (float)base.height : 4.0f / 3.0f;
            for (const auto& layer : snap.layers) {
                if (uploaded >= GlesRenderer::k_max_library_preview_layers) break;
                if (layer.width <= 0 || layer.height <= 0 || layer.rgba.empty()) continue;
                // A layer that is fully transparent this frame would render as
                // an invisible quad that still costs a draw; skip it and let
                // the remaining layers close the gap.
                bool has_visible = false;
                for (std::size_t px = 3; px < layer.rgba.size(); px += 4) {
                    if (layer.rgba[px] != 0) { has_visible = true; break; }
                }
                if (!has_visible) continue;
                m_impl->renderer.update_library_preview_layer_texture(
                    uploaded, layer.rgba, layer.width, layer.height);
                ++uploaded;
            }
            m_library_preview_diorama_layers = uploaded;
            static int s_upload_log = 0;
            if ((s_upload_log++ % 90) == 0) {
                // Per-layer opacity census. A layer dropped because it is
                // fully transparent and a layer that is fully OPAQUE are
                // opposite bugs with the same symptom ("no depth"): the
                // first leaves one quad, the second leaves a front quad
                // that hides every quad behind it. opaque=0 means the
                // former; opaque==total means the latter.
                std::string census;
                for (std::size_t sl = 0; sl < snap.layers.size(); ++sl) {
                    const auto& L = snap.layers[sl];
                    std::size_t opaque = 0, total = L.rgba.size() / 4u;
                    for (std::size_t px = 3; px < L.rgba.size(); px += 4)
                        if (L.rgba[px] != 0) ++opaque;
                    char buf[96];
                    snprintf(buf, sizeof(buf), " [%zu]%dx%d opaque=%zu/%zu",
                             sl, L.width, L.height, opaque, total);
                    census += buf;
                }
                LOGI("diorama upload: snapshot_layers=%zu uploaded=%d%s",
                     snap.layers.size(), uploaded, census.c_str());
            }
        }
    }
    const std::string extract_status = preview_extract_status(m_library_preview_path);
    if (!extract_status.empty()) m_library_preview_uncompressed_size_str.clear(); // extraction in progress: size not known yet
}

void OpenXrShell::stop_library_live_preview() {
    m_rom_preview.clear_live();
    m_bgm_live_active = false;
    if (m_library_preview_session_active) {
        end_rom_preview_session(false);
        m_library_preview_session_active = false;
    }
    if (m_impl) m_impl->renderer.clear_library_preview_layers();
    m_library_live_preview_active = false;
    m_library_preview_has_frame = false;
    m_library_preview_layer_count = 0;
    m_library_preview_diorama_layers = 0;
    m_library_preview_rect_valid = false;
    m_library_preview_reveal_t = 0.0f;
}

// ============================================================
// build_library_preview_diorama — the Library preview's depth. ImGui renders
// one flat texture, so a layered preview cannot live inside the menu image;
// it is drawn as real world-space quads floating beside the menu panel, one
// per emulated hardware layer, each stepped toward the headset. A ROM whose
// backend exposes no separable layers simply gets one quad, which reads as a
// normal flat preview rather than as a broken diorama.
// ============================================================
void OpenXrShell::build_library_preview_diorama(OverlayInfo& overlay,
                                                 const XrPosef& menu_pose,
                                                 float menu_w, float menu_h,
                                                 float alpha) {
    static int s_diorama_log = 0;
    if ((s_diorama_log++ % 90) == 0) {
        LOGI("diorama: layers=%d reveal=%.2f alpha=%.2f rect_valid=%d rect=(%.3f,%.3f)-(%.3f,%.3f)",
             m_library_preview_diorama_layers, m_library_preview_reveal_t, alpha,
             m_library_preview_rect_valid ? 1 : 0,
             m_library_preview_rect_u0, m_library_preview_rect_v0,
             m_library_preview_rect_u1, m_library_preview_rect_v1);
    }
    if (!m_impl || m_library_preview_diorama_layers <= 0) return;
    if (m_library_preview_reveal_t <= 0.01f) return;

    // Panel basis from its orientation, same extraction the laser hit-test uses.
    const XrQuaternionf& q = menu_pose.orientation;
    XrVector3f right{1.0f - 2.0f*(q.y*q.y + q.z*q.z),
                     2.0f*(q.x*q.y + q.z*q.w),
                     2.0f*(q.x*q.z - q.y*q.w)};
    XrVector3f up{2.0f*(q.x*q.y - q.z*q.w),
                  1.0f - 2.0f*(q.x*q.x + q.z*q.z),
                  2.0f*(q.y*q.z + q.x*q.w)};
    XrVector3f fwd{2.0f*(q.w*q.y + q.x*q.z),
                   2.0f*(q.y*q.z - q.w*q.x),
                   1.0f - 2.0f*(q.x*q.x + q.y*q.y)};

    // Anchored to the preview slot inside the menu: the stack stands out of
    // the rectangle draw_rom_preview_sidebar() reserved for it. u/v are that
    // slot's normalised position on the panel texture (v grows downward, world
    // up is +up).
    //
    // The first attempt at this looked flat, but not because the anchoring was
    // wrong -- it also shrank the depth step to 0.02m at the same time, and at
    // that spacing a stack sitting on the panel's own plane is indistinguishable
    // from a flat image. Anchoring and separation are independent, so the step
    // below stays at the value that reads well: the layers still march out
    // toward the headset, they just start from inside the menu.
    if (!m_library_preview_rect_valid) return;
    const float u_c = (m_library_preview_rect_u0 + m_library_preview_rect_u1) * 0.5f;
    const float v_c = (m_library_preview_rect_v0 + m_library_preview_rect_v1) * 0.5f;
    const float card_w = (m_library_preview_rect_u1 - m_library_preview_rect_u0) * menu_w;
    const float card_h = (m_library_preview_rect_v1 - m_library_preview_rect_v0) * menu_h;
    if (card_w <= 0.0f || card_h <= 0.0f) return;
    const float off_r = (u_c - 0.5f) * menu_w;
    const float off_u = -(v_c - 0.5f) * menu_h;

    const XrVector3f centre{
        menu_pose.position.x + right.x * off_r + up.x * off_u,
        menu_pose.position.y + right.y * off_r + up.y * off_u,
        menu_pose.position.z + right.z * off_r + up.z * off_u,
    };

    // Aim the stack at the headset, same as the floating card did: a rotation
    // of +theta about local X tilts the panel normal (+Z) downward, so theta is
    // the negated elevation of the headset as seen from the card. Near the
    // menu's own height this is a small correction, but the stack now stands
    // well clear of the panel, so it still matters.
    XrQuaternionf card_orientation = menu_pose.orientation;
    if (m_impl) {
        const XrVector3f& hmd = m_impl->last_hmd_pose.position;
        const XrVector3f to_hmd{hmd.x - centre.x, hmd.y - centre.y, hmd.z - centre.z};
        const float a_up  = to_hmd.x * up.x  + to_hmd.y * up.y  + to_hmd.z * up.z;
        const float a_fwd = to_hmd.x * fwd.x + to_hmd.y * fwd.y + to_hmd.z * fwd.z;
        if (std::fabs(a_up) > 1e-5f || std::fabs(a_fwd) > 1e-5f) {
            const float theta = -std::atan2(a_up, a_fwd);
            const XrQuaternionf pitch{std::sinf(theta * 0.5f), 0.0f, 0.0f, std::cosf(theta * 0.5f)};
            card_orientation = quat_multiply(menu_pose.orientation, pitch);
        }
    }

    // Layer 0 lifts just clear of the panel surface so it does not z-fight the
    // menu texture; each later layer steps toward the headset. The reveal fade
    // drives the separation too, so the stack opens out as it appears.
    constexpr float kBaseLift  = 0.006f;
    constexpr float kDepthStep = 0.105f;
    const float step = kDepthStep * m_library_preview_reveal_t;

    for (int li = 0; li < m_library_preview_diorama_layers; ++li) {
        if (overlay.panel_count >= OverlayInfo::k_max_panels) break;
        const GLuint tex = m_impl->renderer.library_preview_layer_texture(li);
        if (!tex) continue;
        const float depth = kBaseLift + step * (float)li;
        PanelInfo& pi = overlay.panels[overlay.panel_count++];
        pi.tex = tex;
        pi.pose.orientation = card_orientation;
        pi.pose.position = {
            centre.x + fwd.x * depth,
            centre.y + fwd.y * depth,
            centre.z + fwd.z * depth,
        };
        pi.w = card_w;
        pi.h = card_h;
        pi.alpha = alpha * m_library_preview_reveal_t;
        // Always paint after the menu panel, and strictly back-to-front
        // among themselves. The distance sort alone put the rear layers
        // BEHIND the menu -- they sit only millimetres in front of it but
        // well off its centre, which measures as farther from the eye, so
        // the menu painted over them and they vanished.
        pi.draw_order = 1 + li;
    }
}

void OpenXrShell::open_homebrew_panel(bool fetch_feed) {
    m_active_sub_panel = k_panel_homebrew;
    m_ctrlmap_mode = false;
    m_homebrew_panel_pose = m_main_menu_pose;
    m_hw_view = 0;
    m_hw_hovered = -1;
    m_hw_scroll = 0;
    m_hw_loading = true;
    m_hw_downloading = false;
    m_hw_dirty = true;
    if (fetch_feed && m_vm && m_activity_global) {
        JNIEnv* env = nullptr;
        bool detach = false;
        if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
        }
        if (env) {
            jclass cls = env->GetObjectClass(m_activity_global);
            jmethodID mid = env->GetMethodID(cls, "homebrewFetchFeed", "(I)V");
            if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_hw_feed);
            env->DeleteLocalRef(cls);
            if (detach) m_vm->DetachCurrentThread();
        }
    }
    rebuild_homebrew_panel_texture();
}

void OpenXrShell::open_credits_panel() {
    m_active_sub_panel = k_panel_credits;
    m_ctrlmap_mode = false;
    m_credits_panel_pose = m_main_menu_pose;
    m_credits_hovered = -1;
    m_credits_scroll = 0;
    m_credits_link_armed_index = -1;
    // Re-parsed every open (cheap, tiny file) so edits to credits.txt show up
    // on the next APK rebuild without needing any other code change.
    load_credits_entries();
    m_credits_dirty = true;
    rebuild_credits_panel_texture();
}

void OpenXrShell::load_credits_entries() {
    m_credit_entries.clear();
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    std::string text;
    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "readCreditsAsset", "()Ljava/lang/String;");
    if (mid) {
        jstring js = (jstring)env->CallObjectMethod(m_activity_global, mid);
        if (js) {
            const char* chars = env->GetStringUTFChars(js, nullptr);
            if (chars) { text = chars; env->ReleaseStringUTFChars(js, chars); }
            env->DeleteLocalRef(js);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    if (detach) m_vm->DetachCurrentThread();

    // Format: blank lines and lines starting with '#' are ignored;
    // "[Section]" becomes a header row; "Name|Detail|URL" becomes an entry
    // (URL optional). See assets/credits.txt for the authoritative doc.
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue; // blank
        size_t end = line.find_last_not_of(" \t");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            CreditRow row;
            row.name = line.substr(1, line.size() - 2);
            row.is_header = true;
            m_credit_entries.push_back(std::move(row));
            continue;
        }
        CreditRow row;
        const size_t p1 = line.find('|');
        if (p1 == std::string::npos) { row.name = line; m_credit_entries.push_back(std::move(row)); continue; }
        row.name = line.substr(0, p1);
        const size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) { row.detail = line.substr(p1 + 1); }
        else { row.detail = line.substr(p1 + 1, p2 - p1 - 1); row.url = line.substr(p2 + 1); }
        m_credit_entries.push_back(std::move(row));
    }
    LOGI("load_credits_entries: parsed %zu rows", m_credit_entries.size());
}

// Reads assets/help.txt through the Kotlin side, exactly like
// load_credits_entries() above, and parses it into m_help_entries. Runs once
// (m_help_loaded), the first time the Help tab is drawn — an empty parse is
// still "loaded", so a missing asset doesn't retry the JNI round-trip every
// frame.
void OpenXrShell::load_help_entries() {
    m_help_loaded = true;
    m_help_entries.clear();
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    std::string text;
    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "readHelpAsset", "()Ljava/lang/String;");
    if (mid) {
        jstring js = (jstring)env->CallObjectMethod(m_activity_global, mid);
        if (js) {
            const char* chars = env->GetStringUTFChars(js, nullptr);
            if (chars) { text = chars; env->ReleaseStringUTFChars(js, chars); }
            env->DeleteLocalRef(js);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    if (detach) m_vm->DetachCurrentThread();

    // Format: '#' comments and blank lines ignored; "[Section]" switches the
    // section every following line belongs to; '-' bullet, '!' tip, '>' dim,
    // '~' spacer, anything else a wrapped paragraph. See assets/help.txt for
    // the authoritative doc. Continuation lines (a wrapped bullet indented on
    // the next line) are folded into the previous row so the .txt can stay
    // hand-wrapped while ImGui does the real wrapping.
    std::istringstream stream(text);
    std::string line;
    std::string section;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue; // blank
        size_t end = line.find_last_not_of(" \t");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        if (section.empty()) continue; // stray line before any [Section]

        // A wrapped continuation: indented, not itself a marker, and there is a
        // previous text row in this same section to append to.
        const bool marker = (line[0] == '-' || line[0] == '!' || line[0] == '>' || line[0] == '~');
        if (indented && !marker && !m_help_entries.empty() &&
            m_help_entries.back().section == section &&
            m_help_entries.back().kind != HelpRow::Kind::Spacer) {
            m_help_entries.back().text += " ";
            m_help_entries.back().text += line;
            continue;
        }

        HelpRow row;
        row.section = section;
        if (line == "~") {
            row.kind = HelpRow::Kind::Spacer;
        } else if (marker && line[0] != '~') {
            row.kind = (line[0] == '-')   ? HelpRow::Kind::Bullet
                     : (line[0] == '!')   ? HelpRow::Kind::Tip
                                          : HelpRow::Kind::Dim;
            size_t body = line.find_first_not_of(" \t", 1);
            row.text = (body == std::string::npos) ? std::string() : line.substr(body);
        } else {
            row.text = line;
        }
        m_help_entries.push_back(std::move(row));
    }
    LOGI("load_help_entries: parsed %zu rows", m_help_entries.size());
}

// draw_help_group — Help > <section>. One group per [Section] in help.txt.
void OpenXrShell::draw_help_group(const char* section) {
    if (!m_help_loaded) load_help_entries();
    bool any = false;
    for (size_t i = 0; i < m_help_entries.size(); ++i) {
        const HelpRow& row = m_help_entries[i];
        if (row.section != section) continue;
        any = true;
        ImGui::PushID((int)i);
        switch (row.kind) {
            case HelpRow::Kind::Spacer:
                ImGui::Spacing();
                break;
            case HelpRow::Kind::Bullet:
                ImGui::Bullet();
                ImGui::TextWrapped("%s", row.text.c_str());
                break;
            case HelpRow::Kind::Tip:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.82f, 0.30f, 1.0f));
                ImGui::TextWrapped("%s", row.text.c_str());
                ImGui::PopStyleColor();
                break;
            case HelpRow::Kind::Dim:
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", row.text.c_str());
                ImGui::PopStyleColor();
                break;
            case HelpRow::Kind::Para:
            default:
                ImGui::TextWrapped("%s", row.text.c_str());
                break;
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("No help text for this topic (assets/help.txt missing or empty).");
}

void OpenXrShell::rebuild_credits_panel_texture() {
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (!env) return;

    const PanelMetrics metrics = panel_metrics(PanelKind::Credits);
    const int total = (int)m_credit_entries.size();
    const int visible = std::min(kCreditsVisibleRows, total);
    const int first = std::max(0, std::min(m_credits_scroll, std::max(0, total - visible)));
    const bool has_more_up = first > 0;
    const bool has_more_down = first + visible < total;
    m_credits_window_first = first;
    m_credits_window_visible = visible;

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray names = env->NewObjectArray(visible, str_cls, nullptr);
    jobjectArray details = env->NewObjectArray(visible, str_cls, nullptr);
    jbooleanArray has_link = env->NewBooleanArray(visible);
    jbooleanArray is_header = env->NewBooleanArray(visible);
    {
        std::vector<jboolean> link_flags(visible), header_flags(visible);
        for (int i = 0; i < visible; ++i) {
            const CreditRow& entry = m_credit_entries[first + i];
            jstring jn = env->NewStringUTF(entry.name.c_str());
            env->SetObjectArrayElement(names, i, jn);
            env->DeleteLocalRef(jn);
            jstring jd = env->NewStringUTF(entry.detail.c_str());
            env->SetObjectArrayElement(details, i, jd);
            env->DeleteLocalRef(jd);
            link_flags[i] = !entry.url.empty() ? JNI_TRUE : JNI_FALSE;
            header_flags[i] = entry.is_header ? JNI_TRUE : JNI_FALSE;
        }
        env->SetBooleanArrayRegion(has_link, 0, visible, link_flags.data());
        env->SetBooleanArrayRegion(is_header, 0, visible, header_flags.data());
    }
    env->DeleteLocalRef(str_cls);

    jclass cls = env->GetObjectClass(m_activity_global);
    jmethodID mid = env->GetMethodID(cls, "renderCreditsPanelBitmap",
        "([Ljava/lang/String;[Ljava/lang/String;[Z[ZIZZII)[I");
    if (env->ExceptionCheck()) {
        // A mismatched signature makes GetMethodID throw NoSuchMethodError as
        // a *pending* exception rather than just returning null — leaving it
        // set and making further JNI calls (even harmless DeleteLocalRef)
        // aborts the process under ART, so it must be cleared immediately.
        env->ExceptionClear();
        mid = nullptr;
    }
    if (mid) {
        jintArray result = (jintArray)env->CallObjectMethod(
            m_activity_global, mid, names, details, has_link, is_header,
            (jint)m_credits_hovered, (jboolean)has_more_up, (jboolean)has_more_down,
            (jint)metrics.tex_w, (jint)metrics.tex_h);
        env->DeleteLocalRef(names);
        env->DeleteLocalRef(details);
        env->DeleteLocalRef(has_link);
        env->DeleteLocalRef(is_header);
        if (result && !env->ExceptionCheck()) {
            jsize count = env->GetArrayLength(result);
            if (count == metrics.tex_w * metrics.tex_h) {
                jint* raw = env->GetIntArrayElements(result, nullptr);
                if (raw) {
                    std::vector<uint8_t> rgba((size_t)count * 4);
                    for (jsize i = 0; i < count; ++i) {
                        jint a = raw[i];
                        rgba[i*4+0] = (a >> 16) & 0xFF;
                        rgba[i*4+1] = (a >>  8) & 0xFF;
                        rgba[i*4+2] = (a      ) & 0xFF;
                        rgba[i*4+3] = (a >> 24) & 0xFF;
                    }
                    upload_panel_texture(m_credits_tex, metrics.tex_w, metrics.tex_h, rgba);
                    env->ReleaseIntArrayElements(result, raw, JNI_ABORT);
                }
            }
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        if (result) env->DeleteLocalRef(result);
    } else {
        env->DeleteLocalRef(names);
        env->DeleteLocalRef(details);
        env->DeleteLocalRef(has_link);
        env->DeleteLocalRef(is_header);
    }
    env->DeleteLocalRef(cls);
    if (detach) m_vm->DetachCurrentThread();
    m_credits_dirty = false;
    // Rebuild the hit-test layout to match the currently visible window
    // (+1 for the always-present Back row) so clicks map to the right entry.
    m_credits_panel_layout = make_credits_layout(visible + 1);
}

void OpenXrShell::call_activity_void(const char* method) {
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (env) {
        jclass cls = env->GetObjectClass(m_activity_global);
        jmethodID mid = env->GetMethodID(cls, method, "()V");
        if (mid) env->CallVoidMethod(m_activity_global, mid);
        else if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
        if (detach) m_vm->DetachCurrentThread();
    }
}

void OpenXrShell::call_activity_float(const char* method, float value) {
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (env) {
        jclass cls = env->GetObjectClass(m_activity_global);
        jmethodID mid = env->GetMethodID(cls, method, "(F)V");
        if (mid) env->CallVoidMethod(m_activity_global, mid, (jfloat)value);
        else if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
        if (detach) m_vm->DetachCurrentThread();
    }
}

void OpenXrShell::open_credits_link(int entry_index) {
    if (entry_index < 0 || entry_index >= (int)m_credit_entries.size()) return;
    const std::string& url = m_credit_entries[entry_index].url;
    if (url.empty()) return;
    if (!m_vm || !m_activity_global) return;
    JNIEnv* env = nullptr;
    bool detach = false;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
    }
    if (env) {
        jclass cls = env->GetObjectClass(m_activity_global);
        jmethodID mid = env->GetMethodID(cls, "creditsOpenLink", "(Ljava/lang/String;)V");
        if (mid) {
            jstring js = env->NewStringUTF(url.c_str());
            env->CallVoidMethod(m_activity_global, mid, js);
            env->DeleteLocalRef(js);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(cls);
        if (detach) m_vm->DetachCurrentThread();
    }
}

void OpenXrShell::enter_manual_edit_mode() {
    auto locate_pose = [&](XrSpace hand_space, XrPosef& out) -> bool {
        if (hand_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE) return false;
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

    m_edit_mode = true;
    m_menu_open = false;
    m_active_sub_panel = 0;
    m_laser_hit = false;
    m_quick_panel_dirty = true;
    m_layer_panel_pose = m_quick_panel_pose;
    m_edit_canvas_x  = m_canvas_x;
    m_edit_canvas_y  = m_canvas_y;
    m_edit_canvas_az = m_canvas_az;
    m_edit_canvas_el = m_canvas_el;

    m_edit_laim_ref_valid = false;
    if (m_impl->laim_space != XR_NULL_HANDLE) {
        XrPosef laim{};
        if (locate_pose(m_impl->laim_space, laim)) {
            const XrQuaternionf& aq = laim.orientation;
            m_edit_laim_ref_dir = {
                -2.0f*(aq.x*aq.z + aq.w*aq.y),
                 2.0f*(aq.w*aq.x - aq.y*aq.z),
                 2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f
            };
            m_edit_laim_ref_valid = true;
        }
    }

    m_edit_raim_ref_valid = false;
    if (m_impl->raim_space != XR_NULL_HANDLE) {
        XrPosef raim{};
        if (locate_pose(m_impl->raim_space, raim)) {
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
}

void OpenXrShell::apply_quick_settings_preset(int idx) {
    if (idx < 0 || idx >= (int)m_quick_settings_presets.size()) return;
    const QuickSettingsPreset& preset = m_quick_settings_presets[idx];
    const float current_canvas_y = m_canvas_y;
    m_canvas_x = preset.canvas_x;
    m_canvas_y = current_canvas_y;
    m_canvas_az = preset.canvas_az;
    m_canvas_el = preset.canvas_el;
    m_canvas_scale = std::clamp(preset.canvas_scale, 0.25f, 6.0f);

    m_vr_state.immersive_beta_enabled = preset.immersive_beta_enabled;
    m_vr_state.upscale_mode = preset.upscale_mode;
    m_vr_state.ambilight = preset.ambilight;
    m_vr_state.shadows = preset.passthrough;
    m_vr_state.depth_mode = preset.depth_mode;
    m_vr_state.layers_3d = preset.layers_3d;
    m_vr_state.gamma = preset.gamma;
    m_vr_state.contrast = preset.contrast;
    m_vr_state.saturation = preset.saturation;
    m_vr_state.brightness = preset.brightness;
    m_vr_state.perspective_comp = preset.perspective_comp;
    m_vr_state.environment_sphere_mode = preset.environment_sphere_mode;

    const int n = (int)m_config.layers.size();
    if (n > 0) {
        std::vector<int> idxs(n);
        for (int i = 0; i < n; ++i) idxs[i] = i;
        std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
            return m_config.layers[a].depth_meters < m_config.layers[b].depth_meters;
        });
        for (int rank = 0; rank < n; ++rank) {
            const float t = (n > 1) ? (float)rank / (float)(n - 1) : 0.0f;
            LayerConfig& layer = m_config.layers[idxs[rank]];
            layer.depth_meters = preset.near_depth + (preset.far_depth - preset.near_depth) * t;
            layer.quad_width_meters = preset.quad_width;
        }
        set_all_layer_copy_counts(m_config, preset.copy_count);
    }

    sync_passthrough_state();
    m_settings_panel_dirty = true;
    m_layer_panel_dirty = true;
    m_quick_panel_dirty = true;
    set_status("Quick settings: " + preset.name);
}

bool OpenXrShell::apply_quick_layer_preset(int idx, std::string& status_out) {
    if (idx < 0 || idx >= (int)m_quick_layer_presets.size()) {
        status_out = "No quick layer preset available here.";
        return false;
    }
    const QuickLayerPreset& preset = m_quick_layer_presets[idx];
    if (preset.ordered_ids.empty()) {
        status_out = "No quick layer preset available here.";
        return false;
    }

    std::vector<int> order;
    std::vector<bool> enabled(m_config.layers.size(), true);
    std::vector<bool> ambi(m_config.layers.size(), true);
    for (const std::string& id : preset.ordered_ids) {
        const int idx = layer_index_by_id(m_config, id.c_str());
        if (idx >= 0) order.push_back(idx);
    }
    if ((int)order.size() != (int)m_config.layers.size()) {
        status_out = "Quick layer preset does not match this layer set.";
        return false;
    }
    for (int i = 0; i < (int)preset.ordered_ids.size() && i < (int)preset.enabled.size(); ++i) {
        const int layer_idx = layer_index_by_id(m_config, preset.ordered_ids[i].c_str());
        if (layer_idx >= 0 && layer_idx < (int)enabled.size()) enabled[layer_idx] = preset.enabled[i];
    }
    for (int i = 0; i < (int)preset.ordered_ids.size() && i < (int)preset.ambilight.size(); ++i) {
        const int layer_idx = layer_index_by_id(m_config, preset.ordered_ids[i].c_str());
        if (layer_idx >= 0 && layer_idx < (int)ambi.size()) ambi[layer_idx] = preset.ambilight[i];
    }
    if (!preset.depths.empty()) {
        for (int i = 0; i < (int)preset.ordered_ids.size() && i < (int)preset.depths.size(); ++i) {
            const int layer_idx = layer_index_by_id(m_config, preset.ordered_ids[i].c_str());
            if (layer_idx >= 0 && layer_idx < (int)m_config.layers.size())
                m_config.layers[layer_idx].depth_meters = preset.depths[i];
        }
    }
    m_layer_order = std::move(order);
    m_layer_enabled = std::move(enabled);
    m_layer_ambilight = std::move(ambi);
    presentation::ensure_layer_runtime_state_matches_config(
        m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
    m_layer_panel_dirty = true;
    m_quick_panel_dirty = true;
    status_out = "Quick layers: " + preset.name;
    return true;
}

void OpenXrShell::refresh_quick_layer_presets() {
    const std::string signature = quick_layer_signature(m_current_backend_kind, m_layer_filter_mode, m_config);
    m_quick_layer_presets = make_quick_layer_presets_for_signature(signature, m_config);
    const std::string root_dir = get_settings_dir();
    if (!root_dir.empty()) {
        load_quick_layer_presets_file(
            quick_layer_presets_path(root_dir, m_current_backend_kind, signature),
            m_quick_layer_presets);
    }
    m_quick_panel_layout = make_quick_edit_layout((int)m_quick_settings_presets.size(),
                                                  (int)m_quick_layer_presets.size());
    m_quick_panel_dirty = true;
}

void OpenXrShell::reset_quick_settings_presets() {
    m_quick_settings_presets = make_quick_settings_presets();
    refresh_default_quick_settings_preset(m_current_backend_kind, m_config, m_quick_settings_presets);
    const std::string root_dir = get_settings_dir();
    if (!root_dir.empty()) {
        std::remove(quick_settings_presets_path(root_dir).c_str());
    }
    m_quick_panel_dirty = true;
    set_status("Quick settings presets reset.");
}

void OpenXrShell::reset_quick_layer_presets() {
    const std::string signature = quick_layer_signature(m_current_backend_kind, m_layer_filter_mode, m_config);
    m_quick_layer_presets = make_quick_layer_presets_for_signature(signature, m_config);
    const std::string root_dir = get_settings_dir();
    if (!root_dir.empty()) {
        std::remove(quick_layer_presets_path(root_dir, m_current_backend_kind, signature).c_str());
    }
    m_quick_panel_dirty = true;
    set_status("Quick layer presets reset.");
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

// Pick a fresh muzzle tint for the low-poly pistol. Hues are spread evenly
// around the wheel with a random start so consecutive shots stay visibly
// distinct, and saturation/value are kept high so the tip reads as a flash.
void OpenXrShell::roll_gun_muzzle_color() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> hue_dist(0.0f, 1.0f);
    const float h = hue_dist(rng) * 6.0f;
    const float f = h - std::floor(h);
    constexpr float s = 0.85f, v = 1.0f;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    float r, g, b;
    switch ((int)h % 6) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    m_gun_muzzle_color[0] = r;
    m_gun_muzzle_color[1] = g;
    m_gun_muzzle_color[2] = b;
}

void OpenXrShell::fire_lightgun_vibration(bool right, int mode_in) {
    const int mode = std::clamp(mode_in, 0, VrState::kGunVibrationModeCount - 1);
    if (mode == 0) return;

    QueuedHapticEvent event;
    event.right = right;
    event.amplitude = 1.0f; // patterns scale their supporting pulses below this peak
    switch (mode) {
        case 1:
            event.effect = RumbleEffect::GunRecoil;
            event.duration_ms = 90;
            break;
        case 2:
            event.effect = RumbleEffect::GunMachinegun;
            event.duration_ms = 810; // three 270 ms bursts
            break;
        case 3:
            event.effect = RumbleEffect::GunRevolver;
            event.duration_ms = 900; // stretch the complete rise/impact/decay to 3x
            break;
        default:
            return;
    }
    enqueue_haptic(event);
}

void OpenXrShell::flush_pending_haptics() {
    using Clock = std::chrono::steady_clock;
    const auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    std::vector<QueuedHapticEvent> ready;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_pending_haptics.empty()) return;
        auto it = m_pending_haptics.begin();
        while (it != m_pending_haptics.end()) {
            if (it->due_time_ms <= now_ms) {
                ready.push_back(*it);
                it = m_pending_haptics.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& event : ready) {
        fire_haptic(event.right, event.amplitude, event.duration_ms);
    }
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

    // Motion-control gesture detection runs on EVERY frame, not just in game
    // mode, because their live readouts in the Experimental panel are only
    // visible while a menu is open -- and the game-input path below is skipped
    // in exactly that state. Detection only updates latch state; the latches
    // are read into qi_state[] further down, so nothing reaches the emulator
    // while a panel is up.
    update_dpad_headset();
    update_air_wheel();
    update_air_jump();
    update_air_fighter();

    // A foreground ROM preparation job completes its archive extraction off
    // the XR thread. Commit the prepared path and the backend transition at
    // the start of a frame, after the worker has released the thumbnailer.
    poll_rom_load_completion();

    // Recover if the browser panel was entered by a UI path that only changed
    // m_active_sub_panel (for example the main-menu "Open ROM" row).  The
    // browser must always own a preview session: that session unloads the
    // gameplay backend and starts RomPreviewManager's worker.  Without this
    // guard the shelf could remain visible with the old game still running
    // behind it and every card would stay on the #loading placeholder.
    if (m_menu_open && m_active_sub_panel == 1 &&
        !m_rom_preview_session_active) {
        LOGI("ROM browser: preview session missing; starting it now");
        if (begin_rom_preview_session()) refresh_rom_preview_jobs();
    }

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

    // Real controller-model state (see GlesRenderer::draw_controller_model()):
    // aim pose + raw button/trigger/stick values for both hands. Anchored to
    // the AIM pose, not the grip pose -- this is the same space the real
    // in-app laser pointer and the lightgun model (m_gun_render_pose, built
    // from raim_space/laim_space above) already use, and multiple grip-pose
    // attempts here all came out visibly wrong on-device, so this keeps the
    // controller model consistent with every other hand-attached visual in
    // the app rather than introducing a second, differently-tilted frame.
    // Only bothers with the extra xrLocateSpace calls while the toggle is on.
    if (m_vr_state.show_controller_models) {
        m_ctrl_pose_valid[0] = get_controller_pose(m_impl->laim_space, m_ctrl_pose[0]);
        m_ctrl_pose_valid[1] = get_controller_pose(m_impl->raim_space, m_ctrl_pose[1]);
        m_ctrl_btn_a[0] = get_bool(m_impl->act_x);     // left controller: X
        m_ctrl_btn_b[0] = get_bool(m_impl->act_y);     // left controller: Y
        m_ctrl_btn_a[1] = get_bool(m_impl->act_a);     // right controller: A
        m_ctrl_btn_b[1] = get_bool(m_impl->act_b);     // right controller: B
        m_ctrl_trigger[0] = get_float(m_impl->act_ltrig);
        m_ctrl_trigger[1] = get_float(m_impl->act_rtrig);
        m_ctrl_grip[0] = get_float(m_impl->act_lgrip);
        m_ctrl_grip[1] = get_float(m_impl->act_rgrip);
        get_vec2(m_impl->act_lstick, m_ctrl_stick_x[0], m_ctrl_stick_y[0]);
        get_vec2(m_impl->act_rstick, m_ctrl_stick_x[1], m_ctrl_stick_y[1]);
        m_ctrl_stick_click[0] = get_bool(m_impl->act_lclick);
        m_ctrl_stick_click[1] = get_bool(m_impl->act_rclick);
    } else {
        m_ctrl_pose_valid[0] = m_ctrl_pose_valid[1] = false;
        m_ctrl_stick_click[0] = m_ctrl_stick_click[1] = false;
    }

    // A right-grip hold during free roam reserves the rest of the controls for
    // selecting/moving a layer. Include the raw value so the lock applies on
    // the same frame the grip is pressed, before the edge state is updated
    // below.
    const bool left_hand_lightgun = m_gun_capable && m_gun_hand == 2;
    const bool live_layer_grip_lock = (m_locomotion_active || m_layer_deck_active) && !left_hand_lightgun &&
        (m_layer_grab_held || m_live_layer_grabbed_slot >= 0 ||
         get_float(m_impl->act_rgrip) > 0.5f);

    // While a ROM is being prepared/loading, keep the browser visible and
    // absorb controller input. This prevents repeated trigger presses from
    // starting overlapping loads while still allowing the render loop to show
    // the live progress panel. Require a full trigger/menu release before
    // accepting the next action after completion.
    if (m_rom_load_in_progress.load(std::memory_order_acquire) ||
        m_rom_load_release_required.load(std::memory_order_acquire)) {
        const bool trigger_held = get_float(m_impl->act_rtrig) > 0.5f;
        const bool menu_held = get_bool(m_impl->act_menu);
        if (!m_rom_load_in_progress.load(std::memory_order_acquire) &&
            !trigger_held && !menu_held) {
            m_rom_load_release_required.store(false, std::memory_order_release);
        }
        m_laser_hit = false;
        m_laser_panel = -1;
        m_rtrig_prev = trigger_held;
        m_menu_prev = menu_held;
        return;
    }

    // ---- menu button (left controller ☰) = toggle ROM browser panel ------------
    bool menu_btn = get_bool(m_impl->act_menu);
    // While the new ImGui menu is active (debug_show_new_ui, default on), the
    // physical menu button now opens/closes THAT menu (m_menu_open gates the
    // new menu's render_frame() block the same way it always gated the old
    // one) instead of being a dead button — the old system's own state machine
    // below is skipped entirely in this case, since it doesn't apply.
    if (!live_layer_grip_lock && menu_btn && !m_menu_prev && m_impl->debug_show_new_ui) {
        m_menu_open = !m_menu_open;
        fire_haptic(false, 0.35f, 50);
    } else if (!live_layer_grip_lock && menu_btn && !m_menu_prev && !m_impl->debug_show_new_ui) {
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
                // Leaving the menu without launching a ROM must end the
                // preview session too. Its begin callback unloads the saved
                // backend, so this restores the previous game/state before
                // gameplay resumes; otherwise closing the shelf could leave
                // Neo Geo with no active backend and keep the session marked
                // active for the next menu opening.
                if (m_library_live_preview_active) stop_library_live_preview();
                end_rom_preview_session(false);
                m_menu_open     = false;
                m_ctrlmap_mode  = false;
                m_laser_hit     = false;
                m_emu_frozen_display = false;
            }
        } else {
            m_edit_mode = false; // exit edit mode when entering menu
            open_rom_menu();
            m_emu_frozen_display = false;
        }
        fire_haptic(false, 0.35f, 50); // left controller click feedback
    }
    m_menu_prev = menu_btn;

    // ---- Manual dashboard wings follow head yaw (continuous tracking) ----
    // Recomputed every frame from the current head pose so the hit-test
    // descriptors (m_dashboard_left_pose / m_dashboard_right_pose) always
    // match what add_dashboard_wings() renders this same frame.
    if (m_active_sub_panel == k_panel_manual_dashboard) {
        compute_dashboard_wing_poses(m_main_menu_pose, m_impl->last_hmd_pose,
                                      m_dashboard_left_pose, m_dashboard_right_pose);
    }

    // ---- Gameplay-time interactive side content follows head yaw too ----
    // Recomputed every frame, same reasoning as the dashboard wings above, so the raycast
    // pre-check below and the render pass agree on where these panels are. Two things live here:
    // the always-visible Side Panels mode-select bar (bottom, follows the player, works
    // regardless of side_panel_mode — even Off), and, only in Settings mode, the two interactive
    // side wings (real Settings panel + quick-edit panel).
    bool side_bar_ray_hit      = false;
    int  side_bar_ray_hit_id   = -1;
    bool side_settings_ray_hit = false;
    bool bg_color_ray_hit      = false;
    bool themes_ray_hit        = false;
    if (!m_menu_open) {
        XrPosef anchor_pose{};
        bool have_anchor = false;
        float canvas_w = 0.0f;
        have_anchor = game_canvas_anchor_pose(m_render_layer_refs, m_config,
                                               m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el,
                                               m_canvas_scale, anchor_pose, canvas_w);
        if (have_anchor) {
            compute_side_bar_wing_poses(anchor_pose, m_impl->last_hmd_pose,
                                        m_side_bar_left_pose, m_side_bar_right_pose);
            if (m_vr_state.side_panel_mode == kSidePanelSettings) {
                compute_dashboard_wing_poses(anchor_pose, m_impl->last_hmd_pose,
                                              m_side_settings_left_pose, m_side_settings_right_pose);
            } else if (m_vr_state.side_panel_mode == kSidePanelBgColor) {
                compute_dashboard_wing_poses(anchor_pose, m_impl->last_hmd_pose,
                                              m_bg_color_left_pose, m_bg_color_right_pose);
            } else if (m_vr_state.side_panel_mode == kSidePanelThemes) {
                compute_dashboard_wing_poses(anchor_pose, m_impl->last_hmd_pose,
                                              m_themes_left_pose, m_themes_right_pose);
            }
            // Cheap pre-check: does the right controller's aim ray currently hit any of these?
            // Only if it does do we enter "panel mode" below (which pauses emulator input
            // forwarding while active) — so gameplay input keeps flowing normally except for the
            // instant the player is actually pointing at one of these panels.
            if (m_impl->raim_space != XR_NULL_HANDLE) {
                XrPosef aim{};
                if (get_controller_pose(m_impl->raim_space, aim)) {
                    const XrVector3f& O = aim.position;
                    const XrQuaternionf& aq = aim.orientation;
                    XrVector3f D;
                    D.x = -2.0f*(aq.x*aq.z + aq.w*aq.y);
                    D.y =  2.0f*(aq.w*aq.x - aq.y*aq.z);
                    D.z =  2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f;

                    // ray_hit_rect: returns true + fills u,v (0..1) if the aim ray hits the given
                    // world-space rect (position/orientation from `pose`, size w x h metres).
                    auto ray_hit_rect = [&](const XrPosef& pose, float w, float h, float& out_u, float& out_v) -> bool {
                        const XrQuaternionf& pq = pose.orientation;
                        XrVector3f N;
                        N.x = 2.0f*(pq.w*pq.y + pq.x*pq.z);
                        N.y = 2.0f*(pq.y*pq.z - pq.w*pq.x);
                        N.z = 1.0f - 2.0f*pq.x*pq.x - 2.0f*pq.y*pq.y;
                        const XrVector3f& P = pose.position;
                        float dN = D.x*N.x + D.y*N.y + D.z*N.z;
                        if (std::abs(dN) < 0.001f) return false;
                        float t = ((P.x-O.x)*N.x + (P.y-O.y)*N.y + (P.z-O.z)*N.z) / dN;
                        if (t <= 0.01f || t >= 4.0f) return false;
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
                        float u = (dx*right.x + dy*right.y + dz*right.z) / (w * 0.5f) * 0.5f + 0.5f;
                        float v = -(dx*up.x   + dy*up.y   + dz*up.z)    / (h * 0.5f) * 0.5f + 0.5f;
                        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
                        out_u = u; out_v = v;
                        return true;
                    };

                    const PanelMetrics bar_metrics = panel_metrics(PanelKind::SidePanelBar);
                    float bar_u = 0.0f, bar_v = 0.0f;
                    if (ray_hit_rect(m_side_bar_left_pose, bar_metrics.world_w, bar_metrics.world_h, bar_u, bar_v)) {
                        side_bar_ray_hit = true;
                        m_side_bar_hit_is_left = true;
                        side_bar_ray_hit_id = std::clamp((int)(bar_u * kSidePanelModeCount), 0, kSidePanelModeCount - 1);
                    } else if (ray_hit_rect(m_side_bar_right_pose, bar_metrics.world_w, bar_metrics.world_h, bar_u, bar_v)) {
                        side_bar_ray_hit = true;
                        m_side_bar_hit_is_left = false;
                        side_bar_ray_hit_id = std::clamp((int)(bar_u * kSidePanelModeCount), 0, kSidePanelModeCount - 1);
                    } else if (m_vr_state.side_panel_mode == kSidePanelSettings) {
                        const PanelMetrics side_settings_metrics = panel_metrics(PanelKind::Settings);
                        const PanelMetrics side_quick_metrics    = panel_metrics(PanelKind::QuickEdit);
                        const XrPosef* wing_poses[2] = { &m_side_settings_left_pose, &m_side_settings_right_pose };
                        const float wing_w[2] = { side_settings_metrics.world_w, side_quick_metrics.world_w };
                        const float wing_h[2] = { side_settings_metrics.world_h, side_quick_metrics.world_h };
                        float wu = 0.0f, wv = 0.0f;
                        for (int wi = 0; wi < 2 && !side_settings_ray_hit; ++wi) {
                            if (ray_hit_rect(*wing_poses[wi], wing_w[wi], wing_h[wi], wu, wv)) {
                                side_settings_ray_hit = true;
                            }
                        }
                    } else if (m_vr_state.side_panel_mode == kSidePanelBgColor) {
                        const PanelMetrics bg_metrics = panel_metrics(PanelKind::BgColorPanel);
                        float wu = 0.0f, wv = 0.0f;
                        if (ray_hit_rect(m_bg_color_left_pose, bg_metrics.world_w, bg_metrics.world_h, wu, wv)) {
                            bg_color_ray_hit = true;
                            m_bg_color_hit_is_left = true;
                        } else if (ray_hit_rect(m_bg_color_right_pose, bg_metrics.world_w, bg_metrics.world_h, wu, wv)) {
                            bg_color_ray_hit = true;
                            m_bg_color_hit_is_left = false;
                        }
                    } else if (m_vr_state.side_panel_mode == kSidePanelThemes) {
                        const PanelMetrics theme_metrics = panel_metrics(PanelKind::ThemesPanel);
                        float wu = 0.0f, wv = 0.0f;
                        if (ray_hit_rect(m_themes_left_pose, theme_metrics.world_w, theme_metrics.world_h, wu, wv)) {
                            themes_ray_hit = true;
                            m_themes_hit_is_left = true;
                        } else if (ray_hit_rect(m_themes_right_pose, theme_metrics.world_w, theme_metrics.world_h, wu, wv)) {
                            themes_ray_hit = true;
                            m_themes_hit_is_left = false;
                        }
                    }
                }
            }
        }
    }
    m_side_bar_hovered_id = side_bar_ray_hit ? side_bar_ray_hit_id : -1;
    if ((m_side_bar_hovered_id >= 0) != m_side_bar_was_hovered) {
        m_side_bar_dirty = true;
    }
    m_side_bar_was_hovered = (m_side_bar_hovered_id >= 0);
    if (!themes_ray_hit && m_themes_panel_hovered != -1) {
        m_themes_panel_hovered = -1;
        m_themes_panel_dirty = true;
    }
    // Not hovering (or mode isn't Settings): make sure a stale hit from an earlier frame doesn't
    // linger and cause the render pass to draw a laser / dispatch a click against nothing.
    if (!m_menu_open && !side_settings_ray_hit &&
        (m_laser_panel == k_panel_settings || m_laser_panel == k_panel_quick_edit)) {
        m_laser_hit = false;
    }
    if (!m_menu_open && !bg_color_ray_hit && m_laser_panel == k_panel_bg_color) {
        m_laser_hit = false;
    }
    if (!m_menu_open && !themes_ray_hit && m_laser_panel == k_panel_themes) {
        m_laser_hit = false;
    }
    if (!m_menu_open && !side_bar_ray_hit && m_laser_panel == k_panel_side_bar) {
        m_laser_hit = false;
    }

    // ---- laser + multi-panel hover (runs for menu panels and standalone quick/manual panels) -----------
    // Guarded by !debug_show_new_ui: this block doesn't just render the old
    // panels (that's already guarded, separately, in render_frame()) — it also
    // computes laser hits against their (otherwise invisible) world-space rects
    // and dispatches real value changes from clicks/drags (adjust_setting() and
    // friends, below). Without this guard, pointing/clicking in the new ImGui
    // menu could silently also register as a hit on an old hidden widget
    // sitting at the same world position — root cause of a reported bug where
    // using the new Layers tab dropped VR Res Scale (the old Settings panel's
    // row 13), since both panels sit in roughly the same "in front of the
    // player" spot. Also finally closes the "Side panels" residual-visibility
    // gap flagged after Round 3 (side_settings/side_bar/bg_color/themes_ray_hit
    // are that same leftover system).
    // One source of truth for "a panel UI currently owns the laser" -- the full
    // menu, one of the standalone quick/manual panels, or the laser actually
    // resting on one of the side panels. Shared by the laser-dispatch block
    // here, the PANEL MODE input branch below, and render_frame's layer-update
    // throttle, so all three agree on when the panels are in use.
    const bool panel_ui_owns_laser =
        m_menu_open || m_active_sub_panel == k_panel_manual_dashboard ||
        m_active_sub_panel == 2 || m_active_sub_panel == 3 || m_active_sub_panel == 7 ||
        side_settings_ray_hit || side_bar_ray_hit || bg_color_ray_hit || themes_ray_hit;
    m_panel_ui_active = panel_ui_owns_laser;
    if (!m_impl->debug_show_new_ui && panel_ui_owns_laser
        && m_impl->raim_space != XR_NULL_HANDLE) {
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
            const PanelMetrics save_state_metrics = panel_metrics(PanelKind::SaveStates);
            const PanelMetrics code_metrics     = panel_metrics(PanelKind::Code);
            const PanelMetrics ctrlmap_metrics  = panel_metrics(PanelKind::CtrlMap);
            const PanelMetrics quick_metrics    = panel_metrics(PanelKind::QuickEdit);
            const PanelMetrics dashboard_left_metrics = panel_metrics(PanelKind::DashboardLeft);

            // Determine which panels are visible based on current mode
            PanelDesc* descs = nullptr;
            int descs_count = 0;

            if (!m_menu_open && side_bar_ray_hit) {
                // Always-visible Side Panels mode-select bar — reuses k_panel_side_bar so the
                // dispatch below can identify it regardless of what side_panel_mode currently is.
                const PanelMetrics bar_metrics = panel_metrics(PanelKind::SidePanelBar);
                static PanelDesc descs_side_bar[2] = {
                    { &m_side_bar_left_pose,  0.0f, 0.0f, k_panel_side_bar },
                    { &m_side_bar_right_pose, 0.0f, 0.0f, k_panel_side_bar },
                };
                descs_side_bar[0].w = bar_metrics.world_w;
                descs_side_bar[0].h = bar_metrics.world_h;
                descs_side_bar[1].w = bar_metrics.world_w;
                descs_side_bar[1].h = bar_metrics.world_h;
                descs = descs_side_bar;
                descs_count = 2;
            } else if (!m_menu_open && m_vr_state.side_panel_mode == kSidePanelSettings && side_settings_ray_hit) {
                // Gameplay-time interactive side panels: left wing is the real Settings panel,
                // right wing is the quick-edit panel — reuses their normal panel IDs so the
                // existing k_panel_settings / k_panel_quick_edit dispatch below works unchanged.
                static PanelDesc descs_side_settings[2] = {
                    { &m_side_settings_left_pose,  0.0f, 0.0f, k_panel_settings   },
                    { &m_side_settings_right_pose, 0.0f, 0.0f, k_panel_quick_edit },
                };
                descs_side_settings[0].w = settings_metrics.world_w;
                descs_side_settings[0].h = settings_metrics.world_h;
                descs_side_settings[1].w = quick_metrics.world_w;
                descs_side_settings[1].h = quick_metrics.world_h;
                descs = descs_side_settings;
                descs_count = 2;
            } else if (!m_menu_open && m_vr_state.side_panel_mode == kSidePanelBgColor && bg_color_ray_hit) {
                // Gameplay-time interactive Background Color panel: same texture at both wings.
                const PanelMetrics bg_metrics = panel_metrics(PanelKind::BgColorPanel);
                static PanelDesc descs_bg_color[2] = {
                    { &m_bg_color_left_pose,  0.0f, 0.0f, k_panel_bg_color },
                    { &m_bg_color_right_pose, 0.0f, 0.0f, k_panel_bg_color },
                };
                descs_bg_color[0].w = bg_metrics.world_w;
                descs_bg_color[0].h = bg_metrics.world_h;
                descs_bg_color[1].w = bg_metrics.world_w;
                descs_bg_color[1].h = bg_metrics.world_h;
                descs = descs_bg_color;
                descs_count = 2;
            } else if (!m_menu_open && m_vr_state.side_panel_mode == kSidePanelThemes && themes_ray_hit) {
                const PanelMetrics theme_metrics = panel_metrics(PanelKind::ThemesPanel);
                static PanelDesc descs_themes[2] = {
                    { &m_themes_left_pose, 0.0f, 0.0f, k_panel_themes },
                    { &m_themes_right_pose, 0.0f, 0.0f, k_panel_themes },
                };
                descs_themes[0].w = theme_metrics.world_w;
                descs_themes[0].h = theme_metrics.world_h;
                descs_themes[1].w = theme_metrics.world_w;
                descs_themes[1].h = theme_metrics.world_h;
                descs = descs_themes;
                descs_count = 2;
            } else if (m_ctrlmap_mode) {
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
            } else if (m_active_sub_panel == k_panel_manual_dashboard) {
                // Manual dashboard: dashboard left control panel on the left wing,
                // layer management on the right wing (reusing layer panel texture).
                static PanelDesc descs_dashboard[2] = {
                    { &m_dashboard_left_pose,  0.0f, 0.0f, k_panel_manual_dashboard },
                    { &m_dashboard_right_pose, 0.0f, 0.0f, k_panel_layers },
                };
                descs_dashboard[0].w = dashboard_left_metrics.world_w;
                descs_dashboard[0].h = dashboard_left_metrics.world_h;
                descs_dashboard[1].w = layer_metrics.world_w;
                descs_dashboard[1].h = layer_metrics.world_h;
                descs = descs_dashboard;
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
                static PanelDesc descs_quick[1] = {
                    { &m_quick_panel_pose, 0.0f, 0.0f, k_panel_quick_edit },
                };
                static PanelDesc descs_save_state[1] = {
                    { &m_save_state_panel_pose, 0.0f, 0.0f, k_panel_save_state },
                };
                static PanelDesc descs_ctrlmap_sub[1] = {
                    { &m_ctrlmap_panel_pose,  0.0f, 0.0f, k_panel_ctrlmap  },
                };
                static PanelDesc descs_code[1] = {
                    { &m_code_panel_pose,    0.0f, 0.0f, k_panel_code      },
                };
                static PanelDesc descs_homebrew[1] = {
                    { &m_homebrew_panel_pose, 0.0f, 0.0f, k_panel_homebrew },
                };
                static PanelDesc descs_credits[1] = {
                    { &m_credits_panel_pose, 0.0f, 0.0f, k_panel_credits },
                };

                descs_browser[0].w = browser_metrics.world_w;
                descs_browser[0].h = browser_metrics.world_h;
                descs_browser[1].w = code_metrics.world_w;
                descs_browser[1].h = code_metrics.world_h;
                descs_layers[0].w = layer_metrics.world_w;
                descs_layers[0].h = layer_metrics.world_h;
                descs_settings[0].w = settings_metrics.world_w;
                descs_settings[0].h = settings_metrics.world_h;
                descs_quick[0].w = quick_metrics.world_w;
                descs_quick[0].h = quick_metrics.world_h;
                descs_save_state[0].w = save_state_metrics.world_w;
                descs_save_state[0].h = save_state_metrics.world_h;
                descs_ctrlmap_sub[0].w = ctrlmap_metrics.world_w;
                descs_ctrlmap_sub[0].h = ctrlmap_metrics.world_h;
                descs_code[0].w = code_metrics.world_w;
                descs_code[0].h = code_metrics.world_h;
                {
                    const PanelMetrics hw_m = panel_metrics(PanelKind::Homebrew);
                    descs_homebrew[0].w = hw_m.world_w;
                    descs_homebrew[0].h = hw_m.world_h;
                }
                {
                    const PanelMetrics credits_m = panel_metrics(PanelKind::Credits);
                    descs_credits[0].w = credits_m.world_w;
                    descs_credits[0].h = credits_m.world_h;
                }

                switch (m_active_sub_panel) {
                    case 1: descs = descs_browser;   descs_count = 2; break;
                    case 2: descs = descs_layers;    descs_count = 1; break;
                    case 3: descs = descs_settings;  descs_count = 1; break;
                    case 4: descs = descs_save_state; descs_count = 1; break;
                    case 5: descs = descs_code;      descs_count = 1; break;
                    case 6: descs = descs_ctrlmap_sub; descs_count = 1; break;
                    case 7: descs = descs_quick;     descs_count = 1; break;
                    case k_panel_homebrew: descs = descs_homebrew; descs_count = 1; break;
                    case k_panel_credits: descs = descs_credits; descs_count = 1; break;
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

            // Route hover through the panel layout source of truth.
            m_laser_hit_has_item = false;
            auto assign_hit = [&](const PanelLayout& layout) -> const PanelLayoutItem* {
                const PanelLayoutItem* item = layout.hit(best_u, best_v);
                if (item) {
                    m_laser_hit_item = *item;
                    m_laser_hit_has_item = true;
                }
                return item;
            };

            if (best_panel == k_panel_main_menu) {
                if (m_main_menu_layout.items.empty()) m_main_menu_layout = make_main_menu_layout(7);
                const PanelLayoutItem* item = assign_hit(m_main_menu_layout);
                int row = item ? item->row : -1;
                if (row != m_main_menu_hovered) m_main_menu_hovered = row;
            } else if (best_panel == k_panel_quick_edit) {
                if (m_quick_panel_layout.items.empty()) {
                    m_quick_panel_layout = make_quick_edit_layout((int)m_quick_settings_presets.size(),
                                                                  (int)m_quick_layer_presets.size());
                }
                assign_hit(m_quick_panel_layout);
            } else if (best_panel == k_panel_side_bar) {
                if (m_side_bar_layout.items.empty()) m_side_bar_layout = make_side_panel_bar_layout();
                assign_hit(m_side_bar_layout);
            } else if (best_panel == k_panel_bg_color) {
                if (m_bg_color_panel_layout.items.empty()) m_bg_color_panel_layout = make_bg_color_layout();
                const PanelLayoutItem* item = assign_hit(m_bg_color_panel_layout);
                const int hover_id = item ? item->id : -1;
                if (hover_id != m_bg_color_last_hover_id) {
                    m_bg_color_last_hover_id = hover_id;
                    m_bg_color_panel_dirty = true;
                }
            } else if (best_panel == k_panel_themes) {
                if (m_themes_panel_layout.items.empty()) m_themes_panel_layout = make_themes_layout();
                const PanelLayoutItem* item = assign_hit(m_themes_panel_layout);
                const int hover_id = item ? item->id : -1;
                if (hover_id != m_themes_panel_hovered) {
                    m_themes_panel_hovered = hover_id;
                    m_themes_panel_dirty = true;
                }
            } else if (best_panel == k_panel_browser) {
                PanelLayout layout = make_browser_layout(m_rom_browser.visible_count(), m_rom_browser.scroll_offset());
                assign_hit(layout);
                m_rom_browser.set_hover_uv(best_u, best_v);
            } else if (best_panel == k_panel_layers) {
                m_layer_panel_layout = make_layers_layout((int)m_layer_names.size(), is_snes_filter_capable_config(m_config));
                const PanelLayoutItem* item = assign_hit(m_layer_panel_layout);
                int row = item ? item->row : -1;
                if (row != m_layer_panel_hovered) m_layer_panel_hovered = row;
            } else if (best_panel == k_panel_settings) {
                if (m_settings_panel_layout.items.empty()) m_settings_panel_layout = make_settings_layout(k_settings_row_count);
                const PanelLayoutItem* item = assign_hit(m_settings_panel_layout);
                int row = item ? item->row : -1;
                int area = 0;
                if (item && item->role == PanelRole::Minus) area = 1;
                else if (item && item->role == PanelRole::Plus) area = 2;
                m_settings_panel_hovered_id = item ? item->id : -1;
                if (row != m_settings_panel_hovered || area != m_settings_panel_area) {
                    m_settings_panel_hovered = row;
                    m_settings_panel_area = area;
                }
            } else if (best_panel == k_panel_save_state) {
                if (m_save_state_panel_layout.items.empty()) m_save_state_panel_layout = make_save_state_layout();
                const PanelLayoutItem* item = assign_hit(m_save_state_panel_layout);
                const int cell = item ? item->id : -1;
                if (cell != m_save_state_panel_hovered) m_save_state_panel_hovered = cell;
            } else if (best_panel == k_panel_ctrlmap) {
                if (m_ctrlmap_panel_layout.items.empty()) m_ctrlmap_panel_layout = make_ctrlmap_layout(SNES_BUTTON_COUNT, 6);
                const PanelLayoutItem* item = assign_hit(m_ctrlmap_panel_layout);
                int row = item ? item->row : -1;
                if (row != m_ctrlmap_panel_hovered) m_ctrlmap_panel_hovered = row;
            } else if (best_panel == k_panel_code) {
                if (m_code_panel_layout.items.empty()) m_code_panel_layout = make_code_layout();
                const PanelLayoutItem* item = assign_hit(m_code_panel_layout);
                int hovered = item ? item->id : -1;
                if (hovered != m_code_panel_hovered) m_code_panel_hovered = hovered;
            } else if (best_panel == k_panel_homebrew) {
                m_homebrew_panel_layout = make_homebrew_layout(/* entry_count */ 0, m_hw_view);
                const PanelLayoutItem* item = assign_hit(m_homebrew_panel_layout);
                int row = item ? item->row : -1;
                if (row != m_hw_hovered) {
                    m_hw_hovered = row;
                    m_hw_dirty = true;
                }
            } else if (best_panel == k_panel_credits) {
                // m_credits_panel_layout is (re)built in rebuild_credits_panel_texture(),
                // which runs whenever the panel opens or its content changes.
                const PanelLayoutItem* item = assign_hit(m_credits_panel_layout);
                int row = item ? item->row : -1;
                if (row != m_credits_hovered) {
                    m_credits_hovered = row;
                    m_credits_dirty = true;
                }
            } else if (best_panel == k_panel_manual_dashboard) {
                // Dashboard left panel (global controls)
                m_dashboard_left_panel_layout = make_manual_dashboard_left_layout();
                const PanelLayoutItem* item = assign_hit(m_dashboard_left_panel_layout);
                int row = item ? item->row : -1;
                if (row != m_dashboard_left_panel_hovered) m_dashboard_left_panel_hovered = row;
            } else if (best_panel == k_panel_layers && m_active_sub_panel == k_panel_manual_dashboard) {
                // Dashboard right panel (layer management, reuses layer layout)
                m_layer_panel_layout = make_layers_layout((int)m_layer_names.size(), is_snes_filter_capable_config(m_config));
                const PanelLayoutItem* item = assign_hit(m_layer_panel_layout);
                int row = item ? item->row : -1;
                if (row != m_layer_panel_hovered) m_layer_panel_hovered = row;
            } else {
                m_laser_hit_has_item = false;
            }
        }
    }

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

    // ---- World locomotion mode entry: left thumbstick click ----
    // A plain click toggles: gameplay <-> free-roam (turn/pan/throttle the
    // whole environment, plus the right-grip layer pick/nudge below). No hold
    // timing involved -- every click is a single, unambiguous discrete step,
    // so there's no threshold to misjudge.
    //
    // The layer-deck bookshelf used to be a third step in this cycle. It is
    // disabled (k_layer_deck_enabled) and no longer reachable; its code is
    // kept intact behind that flag so it can be brought back by flipping the
    // one constant rather than re-implementing it.
    // Left grip is no longer involved at all, so it stays a real,
    // user-mappable gameplay button (Controller Map) with zero risk of the
    // shell stealing it out from under a game.
    //
    // Left stick: left/right rotates our facing (right = as if we turned our head/headset
    // right, so the world swings left); up/down throttles forward/backward, same as the
    // triggers below (replaces the old scale-the-world binding — scale is no longer on the
    // left stick).
    // Right stick: left/right/up/down pans the screen, inverted to match stick direction
    // (push right = screen content moves right, i.e. the view point moves left).
    // Triggers: right = throttle forward, left = throttle backward, along the headset's actual
    // look direction (yaw and pitch), like piloting a submarine.
    const bool lclick_loco = get_bool(m_impl->act_lclick);
    if (live_layer_grip_lock) {
        // Keep click edge state aligned while the layer gesture owns input.
        m_lstick_click_prev = lclick_loco;
    } else if (m_menu_open || m_edit_mode || m_active_sub_panel == 2 || m_active_sub_panel == 3 ||
               m_active_sub_panel == 7 || m_current_rom_name.empty()) {
        // The full menu / Edit Mode / the manual quick sub-panels own the left
        // stick/grip for their own input; free-roam and the layer deck cannot
        // be entered from there. Same while browsing with no ROM loaded --
        // there's no scene/layers to roam or fan out yet. A plain click just
        // closes back to gameplay (a no-op while the full menu owns
        // dismissal itself, or while browsing).
        m_locomotion_active = false;
        m_layer_deck_active = false;
        if (lclick_loco && !m_lstick_click_prev && !m_menu_open) {
            m_edit_mode = false;
            m_active_sub_panel = 0;
            m_settings_return_to_quick = false;
            fire_haptic(true, 0.2f, 25);
        }
        m_lstick_click_prev = lclick_loco;
    } else {
        if (lclick_loco && !m_lstick_click_prev) {
            if (!m_locomotion_active && !m_layer_deck_active) {
                m_locomotion_active = true;
                fire_haptic(true, 0.25f, 30);
            } else if (m_locomotion_active) {
                m_locomotion_active = false;
                // Layer deck disabled: click out of free-roam goes straight
                // back to gameplay instead of stopping at the bookshelf.
                m_layer_deck_active = k_layer_deck_enabled;
                fire_haptic(true, k_layer_deck_enabled ? 0.5f : 0.2f,
                            k_layer_deck_enabled ? 80 : 25);
            } else {
                m_layer_deck_active = false;
                fire_haptic(true, 0.2f, 25);
            }
        }
        m_lstick_click_prev = lclick_loco;
    }

    // Keep the layer-grab edge separate from libretro button mapping. Once
    // free-roam/layer-deck is active, emulator input is already suppressed,
    // which makes this control safe to reserve for live layer selection.
    // Layer-deck uses right trigger (feels more natural for a grab-and-drag);
    // free-roam keeps right grip, since right trigger there is still doing
    // double duty as forward throttle (see below) and reusing it for grab
    // would silently break throttling whenever a layer happens to be under
    // the laser.
    const bool layer_grab_now = m_layer_deck_active
        ? get_float(m_impl->act_rtrig) > 0.5f
        : get_float(m_impl->act_rgrip) > 0.5f;
    m_layer_grab_pressed = layer_grab_now && !m_layer_grab_held;
    m_layer_grab_held = layer_grab_now;
    if (!m_locomotion_active && !m_layer_deck_active) {
        // Leaving both modes exits the live canvas interaction; do not carry
        // a half-finished layer selection into the next session.
        m_live_layer_grabbed_slot = -1;
        m_live_layer_lstick_move_dir = 0;
        m_live_layer_rstick_move_dir = 0;
        m_live_layer_hovered_slot = -1;
        m_live_layer_flash_slot = -1;
        m_live_layer_flash_until = 0;
    }

    // Layer-deck spread control: either stick's left/right deflection dials
    // the bookshelf opening angle live, so the stack can sit anywhere from
    // flat/face-on to nearly edge-on instead of a fixed angle.
    if (m_layer_deck_active) {
        float dlx = 0.0f, dly = 0.0f, drx = 0.0f, dry = 0.0f;
        get_vec2(m_impl->act_lstick, dlx, dly);
        get_vec2(m_impl->act_rstick, drx, dry);
        const float deck_x = std::abs(dlx) > std::abs(drx) ? dlx : drx;
        constexpr float k_deck_spread_thresh = 0.15f;
        constexpr float k_deck_spread_speed  = 0.6f; // spread fraction/s at full deflection
        if (std::abs(deck_x) > k_deck_spread_thresh) {
            const float dt = std::clamp((float)(now - m_last_locomotion_time) / 1e9f, 0.0f, 0.1f);
            m_layer_deck_spread = std::clamp(
                m_layer_deck_spread + deck_x * k_deck_spread_speed * dt, 0.0f, 1.0f);
        }
        m_last_locomotion_time = now;
    }

    if (!m_edit_mode) {
        constexpr float k_pi          = 3.14159265f;
        constexpr float k_turn_speed  = 1.0f;  // rad/s
        constexpr float k_pan_speed   = 1.2f;  // m/s

        const bool  grip_held  = m_locomotion_active;
        // Debug-only: while free-roam is active on Neo Geo, the left
        // Neo Geo composition settings are fixed in the MAME core.
        float lx = 0, ly = 0;
        if (grip_held && !layer_grab_now) {
            if (m_locomotion_l_was_active) {
                const float dt = std::clamp(
                    (float)(now - m_last_locomotion_time) / 1e9f, 0.0f, 0.1f);
                get_vec2(m_impl->act_lstick, lx, ly);
                float rx = 0, ry = 0;
                get_vec2(m_impl->act_rstick, rx, ry);

                // Negated: pushing right should feel like turning our own head/headset to the
                // right, which swings the world (and canvas) to the LEFT in view — not like
                // grabbing and tilting the canvas itself to the right.
                m_canvas_az    -= lx * k_turn_speed * dt;   // left/right: rotate our facing
                while (m_canvas_az >  k_pi) m_canvas_az -= 2.0f * k_pi;
                while (m_canvas_az < -k_pi) m_canvas_az += 2.0f * k_pi;

                // Negated on both axes: right controller pan felt backwards versus stick input.
                m_canvas_x -= rx * k_pan_speed * dt;   // left/right: pan the screen
                m_canvas_y -= ry * k_pan_speed * dt;   // up/down: pan the screen
            } else {
                get_vec2(m_impl->act_lstick, lx, ly);
            }
            m_last_locomotion_time = now;
        }
        // A right-grip layer gesture pauses normal free-roam movement. Mark
        // locomotion as inactive for the stick integrator so releasing the
        // right grip cannot cause a movement jump on the next frame.
        m_locomotion_l_was_active = grip_held && !layer_grab_now;

        // ---- Throttle: left grip held + a trigger (or left stick up/down) pushes the player
        // along the direction the right controller is pointing (a laser shows that direction
        // while grip is held), like piloting a submarine. Right trigger / stick up = forward,
        // left trigger / stick down = backward. ----
        m_thrust_laser_active = false;
        if (grip_held && m_impl->raim_space != XR_NULL_HANDLE) {
            XrPosef raim{};
            if (get_controller_pose(m_impl->raim_space, raim)) {
                const XrQuaternionf& aq = raim.orientation;
                // Aim forward = rotate (0,0,-1) by the aim quaternion — same convention as the
                // menu laser's aim ray.
                XrVector3f d;
                d.x = -2.0f*(aq.x*aq.z + aq.w*aq.y);
                d.y =  2.0f*(aq.w*aq.x - aq.y*aq.z);
                d.z =  2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f;
                m_thrust_laser_origin = raim.position;
                m_thrust_laser_dir    = d;
                m_thrust_laser_active = true;
            }
        }

        const float rtrig_loco = get_float(m_impl->act_rtrig);
        const float ltrig_loco = get_float(m_impl->act_ltrig);
        const float throttle   = std::clamp(rtrig_loco - ltrig_loco + ly, -1.0f, 1.0f);
        if (grip_held && !layer_grab_now && m_thrust_laser_active && std::abs(throttle) > 0.05f) {
            if (m_throttle_was_active) {
                const float dt = std::clamp(
                    (float)(now - m_last_throttle_time) / 1e9f, 0.0f, 0.1f);
                constexpr float k_throttle_speed = 2.0f;  // m/s at full trigger

                const XrVector3f& fwd = m_thrust_laser_dir;
                const float s = throttle * k_throttle_speed * dt;

                m_canvas_x             -= fwd.x * s;
                m_canvas_y             -= fwd.y * s;
                m_world_forward_offset -= fwd.z * s;
            }
            m_last_throttle_time = now;
            m_throttle_was_active = true;
        } else {
            m_throttle_was_active = false;
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
        float lx = 0, ly = 0, rx = 0, ry = 0;
        get_vec2(m_impl->act_lstick, lx, ly);
        get_vec2(m_impl->act_rstick, rx, ry);
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

        // Left stick Y -> screen size without changing distance or canvas position.
        if (std::abs(ly) > k_adj_thresh && (now - m_last_width_fire > k_fire_interval)) {
            m_last_width_fire = now;
            const float scale = 1.0f + ly * 0.05f;
            m_canvas_scale = std::clamp(m_canvas_scale * scale, 0.25f, 6.0f);
        }

        // Left stick X → duplicate copy count down/up.
        if (std::abs(lx) > k_adj_thresh && (now - m_last_copy_fire > k_fire_interval)) {
            m_last_copy_fire = now;
            const int delta = (lx > 0.0f) ? 1 : -1;
            const int next_count = std::clamp(current_base_copy_count(m_config, m_layer_order) + delta, 1, 100);
            set_all_layer_copy_counts(m_config, next_count);
            set_status(copy_count_status_text(m_config, m_layer_order, m_layer_auto_dup_percent));
        }

        // Right stick click → passthrough toggle
        bool rclick = get_bool(m_impl->act_rclick);
        if (rclick && !m_rstick_click_prev) {
            m_vr_state.shadows = !m_vr_state.shadows;
            m_settings_panel_dirty = true;
            sync_passthrough_state();
            set_status(m_vr_state.shadows
                ? (passthrough_active() ? "Passthrough ON" : "Passthrough unavailable on this OpenXR runtime.")
                : "Passthrough OFF");
            fire_haptic(true, 0.25f, 30);
        }
        m_rstick_click_prev = rclick;

        // Zero out game input in edit mode
        {
            std::lock_guard<std::mutex> lk(m_input_mutex);
            m_input_state = EmulatorInputState{};
        }

    } else if (!live_layer_grip_lock && panel_ui_owns_laser) {
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
        float lx = 0, ly = 0, rx = 0, ry = 0;
        get_vec2(m_impl->act_lstick, lx, ly);
        get_vec2(m_impl->act_rstick, rx, ry);

        XrTime now_panel = (XrTime)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        constexpr XrTime k_browser_row_scroll_interval  = 300'000'000; // 300 ms
        constexpr XrTime k_browser_page_scroll_interval =  60'000'000; //  60 ms
        constexpr XrTime k_setting_interval = 150'000'000; // 150 ms

        // ROM browser scrolling is a thumbstick action on the browser panel
        // itself, not something that should require the right-hand laser to
        // be actively pointed at it (selecting an item still does). Handle it
        // unconditionally whenever the browser is the active sub-panel so it
        // keeps working while the laser is aimed elsewhere or off any panel.
        if (m_active_sub_panel == 1) {
            const float scroll_y = (std::abs(ry) >= std::abs(ly)) ? ry : ly;
            const float scroll_x = (std::abs(rx) >= std::abs(lx)) ? rx : lx;

            // Vertical = row scroll, horizontal = page scroll.
            if (std::abs(scroll_y) > 0.6f &&
                now_panel - m_last_browser_row_scroll_fire > k_browser_row_scroll_interval) {
                m_last_browser_row_scroll_fire = now_panel;
                m_rom_browser.scroll(scroll_y > 0 ? -1 : 1);
                refresh_rom_preview_jobs();
            } else if (std::abs(scroll_x) > 0.6f &&
                       now_panel - m_last_browser_page_scroll_fire > k_browser_page_scroll_interval) {
                m_last_browser_page_scroll_fire = now_panel;
                m_rom_browser.scroll_page(scroll_x > 0 ? 1 : -1);
                refresh_rom_preview_jobs();
            }
        }

        if (m_laser_panel == k_panel_main_menu) {
            // ---- Main menu ---------------------------------------------------
            int row = m_main_menu_hovered;
            if (rtrig_rising && m_laser_hit && row >= 0) {
                fire_haptic(true, 0.3f, 30);
                if (row != 4 && m_wipe_settings_armed) {
                    m_wipe_settings_armed = false;
                    m_main_menu_dirty = true;
                }
                switch (row) {
                    case 0: // Open ROM → show browser panel (centered, no main menu)
                        m_active_sub_panel = 1;
                        m_ctrlmap_mode = false;
                        m_panel_pose = m_main_menu_pose;
                        if (!m_rom_dir.empty()) m_rom_browser.scan(m_rom_dir);
                        m_rom_browser.dirty(); // ensure browser is refreshed
                        // This path is different from the controller-menu
                        // shortcut: it only changed the visible sub-panel and
                        // never entered the preview session.  That left the
                        // old game running behind the shelf and, more
                        // importantly, meant RomPreviewManager was never
                        // configured, so no thumbnail worker existed.
                        if (!m_rom_preview_session_active) {
                            begin_rom_preview_session();
                            refresh_rom_preview_jobs();
                        }
                        break;
                    case 1: // Save States
                        m_active_sub_panel = 4;
                        m_ctrlmap_mode = false;
                        m_save_state_panel_pose = m_main_menu_pose;
                        m_save_state_panel_hovered = -1;
                        refresh_save_state_slots();
                        m_save_state_panel_dirty = true;
                        break;
                    case 2: // Settings → show settings panel (centered, no main menu)
                        m_active_sub_panel = 3;
                        m_ctrlmap_mode = false;
                        m_settings_return_to_quick = false;
                        m_settings_panel_pose = m_main_menu_pose;
                        m_settings_panel_dirty = true;
                        break;
                    case 3: // Mappings → show ctrlmap panel
                        m_active_sub_panel = 6;
                        m_ctrlmap_mode = true;
                        m_ctrlmap_panel_dirty = true;
                        m_ctrlmap_selected_row = -1;
                        m_ctrlmap_panel_hovered = -1;
                        break;
                    case 4: { // Wipe Settings → arm, then confirm on second tap
                        constexpr XrTime k_wipe_arm_timeout = 4'000'000'000; // 4 s
                        if (m_wipe_settings_armed && (now_panel - m_wipe_settings_arm_time) < k_wipe_arm_timeout) {
                            m_wipe_settings_armed = false;
                            wipe_all_settings();
                        } else {
                            m_wipe_settings_armed = true;
                            m_wipe_settings_arm_time = now_panel;
                            m_main_menu_dirty = true;
                            set_status("Tap Wipe Settings again to permanently erase all saved settings.");
                        }
                        break;
                    }
                    case 5: // Credits
                        open_credits_panel();
                        break;
                    case 6: { // Exit → close app
                        m_menu_open     = false;
                        m_ctrlmap_mode  = false;
                        m_active_sub_panel = 0;
                        m_laser_hit     = false;
                        EmuFreezeCtrl freeze_fn;
                        { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                        if (freeze_fn) freeze_fn(false);
                        m_emu_frozen_display = false;
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

        } else if (m_laser_panel == k_panel_quick_edit) {
            if (rtrig_rising && m_laser_hit_has_item) {
                switch (m_laser_hit_item.role) {
                    case PanelRole::QuickSettingsPreset:
                        apply_quick_settings_preset(m_laser_hit_item.id);
                        fire_haptic(true, 0.3f, 30);
                        break;
                    case PanelRole::QuickSettingsSave:
                        if (!m_quick_preset_dialog_open) {
                            request_quick_settings_preset_save(m_laser_hit_item.id);
                            fire_haptic(true, 0.2f, 20);
                        }
                        break;
                    case PanelRole::QuickLayersPreset: {
                        std::string status;
                        const bool ok = apply_quick_layer_preset(m_laser_hit_item.id, status);
                        set_status(status);
                        fire_haptic(true, ok ? 0.3f : 0.2f, ok ? 30 : 20);
                        break;
                    }
                    case PanelRole::QuickLayersSave:
                        if (!m_quick_preset_dialog_open) {
                            request_quick_layer_preset_save(m_laser_hit_item.id);
                            fire_haptic(true, 0.2f, 20);
                        }
                        break;
                    case PanelRole::QuickResetSettings:
                        m_quick_settings_reset_pending = 1;
                        fire_haptic(true, 0.25f, 25);
                        break;
                    case PanelRole::QuickResetLayers:
                        m_quick_layers_reset_pending = 1;
                        fire_haptic(true, 0.25f, 25);
                        break;
                    case PanelRole::QuickManualEdit:
                        enter_manual_edit_mode();
                        fire_haptic(true, 0.3f, 30);
                        break;
                    case PanelRole::QuickManualVisual:
                        m_active_sub_panel = 3;
                        m_settings_return_to_quick = true;
                        m_settings_panel_pose = m_quick_panel_pose;
                        m_settings_panel_dirty = true;
                        fire_haptic(true, 0.3f, 30);
                        break;
                    case PanelRole::QuickManualLayers:
                        m_active_sub_panel = 2;
                        m_layer_panel_pose = m_quick_panel_pose;
                        m_layer_panel_dirty = true;
                        fire_haptic(true, 0.3f, 30);
                        break;
                    default:
                        break;
                }
            }
        } else if (m_laser_panel == k_panel_side_bar) {
            // ---- Always-visible Side Panels mode-select bar ------------------
            if (rtrig_rising && m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::SidePanelSelect) {
                m_vr_state.side_panel_mode = std::clamp(m_laser_hit_item.id, 0, kSidePanelModeCount - 1);
                m_side_bar_dirty = true;
                m_settings_panel_dirty = true;
                m_help_panel_dirty = true;
                m_perf_overlay_dirty = true;
                m_quick_panel_dirty = true;
                m_bg_color_panel_dirty = true;
                m_themes_panel_dirty = true;
                fire_haptic(true, 0.3f, 25);
                if (m_on_vr_state_changed) {
                    m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
                }
            }
        } else if (m_laser_panel == k_panel_bg_color) {
            // ---- Background Color preset grid ------------------
            if (rtrig_rising && m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::BgColorSelect) {
                const int new_idx = std::clamp(m_laser_hit_item.id, 0, 15);
                // Tap the already-active preset to turn Background Color off (back to whatever
                // Environment Sphere would otherwise show) — otherwise there'd be no way to
                // clear the choice without reaching into settings_io by hand.
                m_vr_state.bg_preset_index = (m_vr_state.bg_preset_index == new_idx) ? -1 : new_idx;
                m_bg_color_panel_dirty = true;
                fire_haptic(true, 0.3f, 25);
                if (m_vr_state.bg_preset_index >= 0) {
                    set_status("Background preset " + std::to_string(m_vr_state.bg_preset_index + 1) + " active");
                } else {
                    set_status("Background color reset");
                }
                if (m_on_vr_state_changed) {
                    m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
                }
            }
        } else if (m_laser_panel == k_panel_themes) {
            if (rtrig_rising && m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::ThemeSelect) {
                m_ui_theme = clamp_ui_theme(m_laser_hit_item.id);
                const std::string theme_dir = get_settings_dir();
                if (!theme_dir.empty()) ui_theme_save(theme_dir + "/ui_theme.ini", static_cast<int>(m_ui_theme));
                if (m_vm && m_activity_global) {
                    JNIEnv* env = nullptr;
                    bool detach = false;
                    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                        if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
                    }
                    if (env) {
                        jclass cls = env->GetObjectClass(m_activity_global);
                        jmethodID mid = env->GetMethodID(cls, "setUiThemeId", "(I)V");
                        if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_ui_theme);
                        env->DeleteLocalRef(cls);
                        if (detach) m_vm->DetachCurrentThread();
                    }
                }
                m_themes_panel_dirty = true;
                m_main_menu_dirty = true;
                m_help_panel_dirty = true;
                m_perf_overlay_dirty = true;
                m_quick_panel_dirty = true;
                m_settings_panel_dirty = true;
                m_layer_panel_dirty = true;
                m_save_state_panel_dirty = true;
                m_code_panel_dirty = true;
                m_ctrlmap_panel_dirty = true;
                fire_haptic(true, 0.3f, 25);
                set_status(std::string("UI theme: ") + ui_theme_name(m_ui_theme));
            }
        } else if (m_laser_panel == k_panel_browser) {
            // ---- ROM browser ------------------------------------------------
            if (rtrig_rising && m_laser_hit && !m_rom_browser.empty()) {
                if (m_rom_browser.hovered_is_dir()) {
                    fire_haptic(true, 0.3f, 40); // soft click for navigation
                    enter_folder_and_queue_caching();
                } else {
                    const std::string path = m_rom_browser.hovered_path();
                    if (!path.empty()) {
                        fire_haptic(true, 0.7f, 100); // strong "launch" buzz
                        RomLoader loader;
                        { std::lock_guard<std::mutex> lk(m_mutex); loader = m_rom_loader; }
                        if (loader) {
                            // Preparation (including potentially hundreds of
                            // megabytes of ZIP/7z extraction) runs away from
                            // the XR thread. The browser stays rendered and
                            // accepts no further selections until it finishes.
                            start_async_rom_preparation(path);
                        }
                        // Keep the browser/overlay alive until the async
                        // preparation and final backend commit complete.
                        m_laser_hit = false;
                    }
                }
            }

        } else if (m_laser_panel == k_panel_layers) {
            // ---- Layer order panel (with play/pause + auto-dup + optional filter row) -----------
            int n = (int)m_layer_names.size();
            int row = m_layer_panel_hovered;
            const bool has_filter_row = is_snes_filter_capable_config(m_config);

            // Layer-composition mode toggle (Neo Geo only): top-right corner
            // of the title bar, independent of the row list below (its
            // layout item carries row == -1).
            if (rtrig_rising && m_laser_hit && m_laser_hit_has_item &&
                m_laser_hit_item.role == PanelRole::LayerModeToggle &&
                m_config.game == "mame_neogeo") {
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.3f, 30);
            }
            // Play/Pause button: row == n
            else if (rtrig_rising && m_laser_hit && row == n) {
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
            // Reset depths button: row == n + 2
            else if (rtrig_rising && m_laser_hit && row == n + 2) {
                even_spread_layer_depths(m_config.layers);
                m_layer_depth_selected = -1;
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.35f, 40);
                do_step_one();
            }
            else if (has_filter_row && rtrig_rising && m_laser_hit && row == n + 3) {
                apply_layer_filter_mode(next_layer_filter_mode(m_layer_filter_mode), true);
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.35f, 40);
                do_step_one();
            }
            // Layer row interactions (rows 0 to n-1)
            else if (rtrig_rising && m_laser_hit && row >= 0 && row < n) {
                int orig = m_layer_order[row];
                if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Ambilight) {
                    // Toggle ambilight (right 20% = AMB button)
                    if (orig < (int)m_layer_ambilight.size())
                        m_layer_ambilight[orig] = !m_layer_ambilight[orig];
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::SideColor) {
                    // Cycle side/back face color override: Ori/Black/White/Red/Green/Blue/Darker
                    if (orig < (int)m_layer_side_color.size())
                        m_layer_side_color[orig] = (m_layer_side_color[orig] + 1) % 7;
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Visibility) {
                    // Toggle visibility (middle 20% = ON/OFF button)
                    if (orig < (int)m_layer_enabled.size())
                        m_layer_enabled[orig] = !m_layer_enabled[orig];
                    sync_layer_capture_mask();
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::GeometryMode
                           && orig < (int)m_config.layers.size()) {
                    auto& lc = m_config.layers[orig];
                    lc.geometry_mode = next_geom_mode(lc.geometry_mode);
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_has_item &&
                           (m_laser_hit_item.role == PanelRole::DistMinus ||
                            m_laser_hit_item.role == PanelRole::DistPlus ||
                            m_laser_hit_item.role == PanelRole::ThickMinus ||
                            m_laser_hit_item.role == PanelRole::ThickPlus)) {
                    // Handled below by the held-repeat block (needs rtrig_now, not just rising).
                } else {
                    // Left 60%: toggle depth-edit mode, also start drag
                    if (m_layer_depth_selected == row) {
                        m_layer_depth_selected = -1;
                    } else {
                        m_layer_depth_selected = row;
                    }
                    m_layer_panel_grabbed = row;
                    m_layer_panel_dirty   = true;
                    fire_haptic(true, 0.25f, 20);
                }
            }

            // DistMinus/DistPlus/ThickMinus/ThickPlus: held (not just tapped), adaptive-repeat
            // while the trigger stays down and the laser stays on the SAME button (see
            // adaptive_repeat_interval_ns). Independent of the rtrig_rising block above since a
            // sustained hold needs to keep firing across many frames, not just once on press.
            {
                const bool adj_is_dist_minus  = m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::DistMinus;
                const bool adj_is_dist_plus   = m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::DistPlus;
                const bool adj_is_thick_minus = m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::ThickMinus;
                const bool adj_is_thick_plus  = m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::ThickPlus;
                const bool adj_target_valid = m_laser_hit && row >= 0 && row < n &&
                    (adj_is_dist_minus || adj_is_dist_plus || adj_is_thick_minus || adj_is_thick_plus);
                const PanelRole adj_role = adj_is_dist_minus ? PanelRole::DistMinus
                                          : adj_is_dist_plus  ? PanelRole::DistPlus
                                          : adj_is_thick_minus ? PanelRole::ThickMinus
                                          : adj_is_thick_plus  ? PanelRole::ThickPlus
                                                                : PanelRole::None;
                if (rtrig_now && adj_target_valid) {
                    if (rtrig_rising || row != m_layer_adj_hold_row || adj_role != m_layer_adj_hold_role) {
                        m_layer_adj_hold_start = now_panel;
                        m_layer_adj_hold_row   = row;
                        m_layer_adj_hold_role  = adj_role;
                        m_last_layer_adj_fire  = 0; // fire immediately on the new target
                    }
                } else {
                    m_layer_adj_hold_row = -1;
                }
                if (rtrig_now && adj_target_valid
                    && now_panel - m_last_layer_adj_fire >
                       adaptive_repeat_interval_ns(now_panel - m_layer_adj_hold_start)) {
                    m_last_layer_adj_fire = now_panel;
                    const int orig = m_layer_order[row];
                    if (orig < (int)m_config.layers.size()) {
                        if (adj_is_dist_minus) {
                            float& d = m_config.layers[orig].depth_meters;
                            d = std::max(0.3f, d - 0.05f);
                        } else if (adj_is_dist_plus) {
                            float& d = m_config.layers[orig].depth_meters;
                            d = std::min(k_layer_dist_max, d + 0.05f);
                        } else if (adj_is_thick_minus) {
                            auto& lc = m_config.layers[orig];
                            if (lc.geometry_mode == LayerGeometryMode::SplitFloor ||
                                lc.geometry_mode == LayerGeometryMode::SplitCeiling ||
                                lc.geometry_mode == LayerGeometryMode::Room) {
                                lc.split_pixels = std::max(1, (lc.split_pixels > 0 ? lc.split_pixels : 1) - 1);
                            } else if (lc.geometry_mode == LayerGeometryMode::Repeat) {
                                lc.repeat_count = std::clamp((lc.repeat_count > 0 ? lc.repeat_count : 3) - 1, 2, 8);
                            } else if (lc.geometry_mode == LayerGeometryMode::DepthScatter) {
                                lc.scatter_range = std::max(0.0f, lc.scatter_range - 0.05f);
                            } else if (lc.geometry_mode == LayerGeometryMode::AutoYDepth) {
                                lc.y_depth_range = std::max(0.0f, lc.y_depth_range - 0.05f);
                            } else {
                                float& t = lc.box_thickness_meters;
                                t = std::max(-k_layer_thickness_max, t - 0.01f);
                            }
                        } else if (adj_is_thick_plus) {
                            auto& lc = m_config.layers[orig];
                            if (lc.geometry_mode == LayerGeometryMode::SplitFloor ||
                                lc.geometry_mode == LayerGeometryMode::SplitCeiling ||
                                lc.geometry_mode == LayerGeometryMode::Room) {
                                lc.split_pixels = std::min(512, (lc.split_pixels > 0 ? lc.split_pixels : 1) + 1);
                            } else if (lc.geometry_mode == LayerGeometryMode::Repeat) {
                                lc.repeat_count = std::clamp((lc.repeat_count > 0 ? lc.repeat_count : 3) + 1, 2, 8);
                            } else if (lc.geometry_mode == LayerGeometryMode::DepthScatter) {
                                lc.scatter_range = std::min(5.0f, lc.scatter_range + 0.05f);
                            } else if (lc.geometry_mode == LayerGeometryMode::AutoYDepth) {
                                lc.y_depth_range = std::min(5.0f, lc.y_depth_range + 0.05f);
                            } else {
                                float& t = lc.box_thickness_meters;
                                t = std::min(k_layer_thickness_max, t + 0.01f);
                            }
                        }
                        m_layer_panel_dirty = true;
                        fire_haptic(true, 0.25f, 20);
                        do_step_one();
                    }
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

            // Right stick Y: adjust depth of selected layer (rate-limited)
            if (m_layer_depth_selected >= 0 && m_layer_depth_selected < n
                && std::abs(ry) > 0.5f
                && now_panel - m_last_layer_fire > k_setting_interval) {
                m_last_layer_fire = now_panel;
                const int orig = m_layer_order[m_layer_depth_selected];
                if (orig < (int)m_config.layers.size()) {
                    constexpr float step = 0.05f;
                    float& d = m_config.layers[orig].depth_meters;
                    d += (ry > 0 ? -step : step); // stick up = closer = decrease depth
                    if (d < 0.05f) d = 0.05f;
                    if (d > 10.0f) d = 10.0f;
                    m_layer_panel_dirty = true;
                    do_step_one();
                }
            }

        } else if (m_laser_panel == k_panel_manual_dashboard) {
            // ---- Manual Dashboard Left Panel (Global Controls) ----
            // All 7 rows have +/- buttons: Screen Size, Near, Far, X, Y, Dup Count, Dup Spacing
            // Held (not just tapped) — keeps repeating while the trigger stays down and the laser
            // stays on the SAME button, at an adaptive rate that ramps up the longer it's held
            // (see adaptive_repeat_interval_ns): starts deliberate, accelerates for big sweeps.
            int row = m_dashboard_left_panel_hovered;
            const bool dash_is_minus = m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Minus;
            const bool dash_is_plus  = m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Plus;
            const bool dash_target_valid = m_laser_hit && row >= 0 && row < 7 && (dash_is_minus || dash_is_plus);
            if (rtrig_now && dash_target_valid) {
                // A fresh press, or the laser landed on a different button mid-hold, restarts the
                // ramp from tier 1 — only a sustained hold on ONE button accelerates.
                if (rtrig_rising || row != m_dashboard_hold_row || dash_is_plus != m_dashboard_hold_is_plus) {
                    m_dashboard_hold_start    = now_panel;
                    m_dashboard_hold_row      = row;
                    m_dashboard_hold_is_plus  = dash_is_plus;
                    m_last_dashboard_fire     = 0; // fire immediately on the new target
                }
            } else {
                m_dashboard_hold_row = -1; // released or moved off any button — next press restarts ramp
            }
            if (rtrig_now && dash_target_valid
                && now_panel - m_last_dashboard_fire > adaptive_repeat_interval_ns(now_panel - m_dashboard_hold_start)) {
                m_last_dashboard_fire = now_panel;
                if (dash_is_minus) {
                    // Minus button clicked
                    auto adjust_dashboard_setting = [&](int r, int dir) {
                        if (dir == 0) return;
                        switch (r) {
                            case 0: // Screen Size
                                m_canvas_scale = std::clamp(m_canvas_scale * (dir > 0 ? 1.1f : 0.91f), 0.25f, k_dashboard_scale_max);
                                m_dashboard_left_panel_dirty = true;
                                break;
                            case 1: // Near Distance
                                {
                                    float near_d = !m_config.layers.empty() ? m_config.layers[0].depth_meters : 1.0f;
                                    near_d = std::clamp(near_d + dir*0.05f, 0.05f, k_layer_dist_max);
                                    for (auto& lc : m_config.layers) {
                                        if (lc.depth_meters < 5.0f) lc.depth_meters = near_d;
                                    }
                                    m_layer_panel_dirty = true;
                                    m_dashboard_left_panel_dirty = true;
                                }
                                break;
                            case 2: // Far Distance
                                {
                                    float far_d = !m_config.layers.empty() ? m_config.layers.back().depth_meters : 2.0f;
                                    far_d = std::clamp(far_d + dir*0.05f, 0.05f, k_layer_dist_max);
                                    for (auto& lc : m_config.layers) {
                                        if (lc.depth_meters >= 5.0f) lc.depth_meters = far_d;
                                    }
                                    m_layer_panel_dirty = true;
                                    m_dashboard_left_panel_dirty = true;
                                }
                                break;
                            case 3: // X Position
                                m_canvas_x = std::clamp(m_canvas_x + dir*0.05f, -k_dashboard_pos_max, k_dashboard_pos_max);
                                m_dashboard_left_panel_dirty = true;
                                break;
                            case 4: // Y Position
                                m_canvas_y = std::clamp(m_canvas_y + dir*0.05f, -k_dashboard_pos_max, k_dashboard_pos_max);
                                m_dashboard_left_panel_dirty = true;
                                break;
                            case 5: // Duplication Count
                                {
                                    const int next_count = std::clamp(current_base_copy_count(m_config, m_layer_order) + dir, 1, 100);
                                    set_all_layer_copy_counts(m_config, next_count);
                                    m_layer_panel_dirty = true;
                                    m_dashboard_left_panel_dirty = true;
                                }
                                break;
                            case 6: // Duplication Spacing
                                m_dashboard_duplication_spacing = std::clamp(m_dashboard_duplication_spacing + dir*0.0005f, 0.0001f, 0.02f);
                                m_dashboard_left_panel_dirty = true;
                                break;
                        }
                    };
                    adjust_dashboard_setting(row, -1);
                    fire_haptic(true, 0.2f, 15);
                    do_step_one();
                } else if (dash_is_plus) {
                    // Plus button clicked
                    auto adjust_dashboard_setting = [&](int r, int dir) {
                        if (dir == 0) return;
                        switch (r) {
                            case 0: // Screen Size
                                m_canvas_scale = std::clamp(m_canvas_scale * (dir > 0 ? 1.1f : 0.91f), 0.25f, k_dashboard_scale_max);
                                m_dashboard_left_panel_dirty = true;
                                break;
                            case 1: // Near Distance
                                {
                                    float near_d = !m_config.layers.empty() ? m_config.layers[0].depth_meters : 1.0f;
                                    near_d = std::clamp(near_d + dir*0.05f, 0.05f, k_layer_dist_max);
                                    for (auto& lc : m_config.layers) {
                                        if (lc.depth_meters < 5.0f) lc.depth_meters = near_d;
                                    }
                                    m_layer_panel_dirty = true;
                                    m_dashboard_left_panel_dirty = true;
                                }
                                break;
                            case 2: // Far Distance
                                {
                                    float far_d = !m_config.layers.empty() ? m_config.layers.back().depth_meters : 2.0f;
                                    far_d = std::clamp(far_d + dir*0.05f, 0.05f, k_layer_dist_max);
                                    for (auto& lc : m_config.layers) {
                                        if (lc.depth_meters >= 5.0f) lc.depth_meters = far_d;
                                    }
                                    m_layer_panel_dirty = true;
                                    m_dashboard_left_panel_dirty = true;
                                }
                                break;
                            case 3: // X Position
                                m_canvas_x = std::clamp(m_canvas_x + dir*0.05f, -k_dashboard_pos_max, k_dashboard_pos_max);
                                m_dashboard_left_panel_dirty = true;
                                break;
                            case 4: // Y Position
                                m_canvas_y = std::clamp(m_canvas_y + dir*0.05f, -k_dashboard_pos_max, k_dashboard_pos_max);
                                m_dashboard_left_panel_dirty = true;
                                break;
                            case 5: // Duplication Count
                                {
                                    const int next_count = std::clamp(current_base_copy_count(m_config, m_layer_order) + dir, 1, 100);
                                    set_all_layer_copy_counts(m_config, next_count);
                                    m_layer_panel_dirty = true;
                                    m_dashboard_left_panel_dirty = true;
                                }
                                break;
                            case 6: // Duplication Spacing
                                m_dashboard_duplication_spacing = std::clamp(m_dashboard_duplication_spacing + dir*0.0005f, 0.0001f, 0.02f);
                                m_dashboard_left_panel_dirty = true;
                                break;
                        }
                    };
                    adjust_dashboard_setting(row, 1);
                    fire_haptic(true, 0.2f, 15);
                    do_step_one();
                }
            }
        } else if (m_laser_panel == k_panel_layers && m_active_sub_panel == k_panel_manual_dashboard) {
            // ---- Manual Dashboard Right Panel (Layer Management, reuses layer panel logic) ----
            int n = (int)m_layer_names.size();
            int row = m_layer_panel_hovered;
            const bool has_filter_row = is_snes_filter_capable_config(m_config);

            // Layer-composition mode toggle (Neo Geo only): top-right corner
            // of the title bar, independent of the row list below (its
            // layout item carries row == -1).
            if (rtrig_rising && m_laser_hit && m_laser_hit_has_item &&
                m_laser_hit_item.role == PanelRole::LayerModeToggle &&
                m_config.game == "mame_neogeo") {
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.3f, 30);
            }
            // Play/Pause button: row == n
            else if (rtrig_rising && m_laser_hit && row == n) {
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
            // Reset depths button: row == n + 2
            else if (rtrig_rising && m_laser_hit && row == n + 2) {
                even_spread_layer_depths(m_config.layers);
                m_layer_depth_selected = -1;
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.35f, 40);
                do_step_one();
            }
            else if (has_filter_row && rtrig_rising && m_laser_hit && row == n + 3) {
                apply_layer_filter_mode(next_layer_filter_mode(m_layer_filter_mode), true);
                m_layer_panel_dirty = true;
                fire_haptic(true, 0.35f, 40);
                do_step_one();
            }
            // Layer row interactions (rows 0 to n-1)
            else if (rtrig_rising && m_laser_hit && row >= 0 && row < n) {
                int orig = m_layer_order[row];
                if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Ambilight) {
                    if (orig < (int)m_layer_ambilight.size())
                        m_layer_ambilight[orig] = !m_layer_ambilight[orig];
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::SideColor) {
                    if (orig < (int)m_layer_side_color.size())
                        m_layer_side_color[orig] = (m_layer_side_color[orig] + 1) % 7;
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Visibility) {
                    if (orig < (int)m_layer_enabled.size())
                        m_layer_enabled[orig] = !m_layer_enabled[orig];
                    sync_layer_capture_mask();
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.3f, 30);
                    do_step_one();
                } else {
                    if (m_layer_depth_selected == row) {
                        m_layer_depth_selected = -1;
                    } else {
                        m_layer_depth_selected = row;
                    }
                    m_layer_panel_grabbed = row;
                    m_layer_panel_dirty = true;
                    fire_haptic(true, 0.25f, 20);
                }
            }

            if (!rtrig_now && m_layer_panel_grabbed >= 0) {
                int src = m_layer_panel_grabbed;
                int dst = (m_laser_hit && row >= 0 && row < n) ? row : src;
                if (src != dst) {
                    std::vector<float> depths(n);
                    for (int i = 0; i < n; ++i)
                        depths[i] = (m_layer_order[i] < (int)m_config.layers.size())
                                    ? m_config.layers[m_layer_order[i]].depth_meters : 1.5f;

                    if (src < dst) {
                        for (int i = src; i < dst; ++i)
                            std::swap(m_layer_order[i], m_layer_order[i + 1]);
                    } else {
                        for (int i = src; i > dst; --i)
                            std::swap(m_layer_order[i], m_layer_order[i - 1]);
                    }

                    std::sort(depths.begin(), depths.end());
                    for (int i = 0; i < n; ++i) {
                        int orig = m_layer_order[i];
                        if (orig < (int)m_config.layers.size())
                            m_config.layers[orig].depth_meters = depths[i];
                    }
                }
                m_layer_panel_grabbed = -1;
                m_layer_panel_dirty = true;
                if (src != dst) {
                    fire_haptic(true, 0.4f, 50);
                    do_step_one();
                }
            }

            static int s_prev_hover_dashboard = -1;
            if (m_layer_panel_grabbed >= 0 && row != s_prev_hover_dashboard) {
                m_layer_panel_dirty = true;
                s_prev_hover_dashboard = row;
            }
            if (m_layer_panel_grabbed < 0) s_prev_hover_dashboard = -1;

            if (m_layer_depth_selected >= 0 && m_layer_depth_selected < n
                && std::abs(ry) > 0.5f
                && now_panel - m_last_layer_fire > k_setting_interval) {
                m_last_layer_fire = now_panel;
                const int orig = m_layer_order[m_layer_depth_selected];
                if (orig < (int)m_config.layers.size()) {
                    constexpr float step = 0.05f;
                    float& d = m_config.layers[orig].depth_meters;
                    d += (ry > 0 ? -step : step);
                    if (d < 0.05f) d = 0.05f;
                    if (d > 10.0f) d = 10.0f;
                    m_layer_panel_dirty = true;
                    do_step_one();
                }
            }
        } else if (m_laser_panel == k_panel_settings) {
            // ---- Settings panel ---------------------------------------------
            // Bools  (rows 0, 2, 5): trigger anywhere → toggle
            // Cycle  (rows 1, 3, 4): trigger on left/right zones → previous/next
            // Floats (rows 6-9): trigger on left 22% → dec, right 22% → inc
            //                    hold trigger + stick X for continuous tweak
            // Actions (rows 14-19): trigger → fire action
            auto adjust_setting = [&](int row, int dir) {
                // dir: -1 = dec, +1 = inc (ignored for bool rows)
                auto clamp = [](float v, float lo, float hi) {
                    return v < lo ? lo : v > hi ? hi : v;
                };
                constexpr float step = 0.1f;
                switch (row) {
                    case 0: m_vr_state.immersive_beta_enabled = !m_vr_state.immersive_beta_enabled; m_settings_panel_dirty=true; break;
                    case 1:
                        m_vr_state.upscale_mode = cycle_upscale_mode(m_vr_state.upscale_mode, dir);
                        m_settings_panel_dirty = true;
                        break;
                    case 2:
                        m_vr_state.ambilight_placement = (AmbilightPlacement)(((int)m_vr_state.ambilight_placement + (dir < 0 ? 3 : 1)) % 4);
                        m_vr_state.ambilight = true; m_settings_panel_dirty=true; break;
                    case 3: break;
                    case 4:
                        m_vr_state.shadows = !m_vr_state.shadows;
                        m_settings_panel_dirty = true;
                        sync_passthrough_state();
                        set_status(m_vr_state.shadows
                            ? (passthrough_active() ? "Passthrough ON" : "Passthrough unavailable on this OpenXR runtime.")
                            : "Passthrough OFF");
                        break;
                    case 5: {
                        int cur = 0;
                        float best_delta = 1e9f;
                        for (int s = 0; s < k_parallax_step_count; ++s) {
                            const float delta = std::fabs(m_vr_state.parallax_ratio - k_parallax_steps[s]);
                            if (delta < best_delta) { best_delta = delta; cur = s; }
                        }
                        const int d = (dir == 0 ? 1 : dir);
                        cur = (cur + d + k_parallax_step_count) % k_parallax_step_count;
                        m_vr_state.parallax_ratio = k_parallax_steps[cur];
                        m_settings_panel_dirty = true;
                        break;
                    }
                    case 6:
                        m_experimental_rumble_enabled = !m_experimental_rumble_enabled;
                        m_settings_panel_dirty = true;
                        if (m_on_experimental_rumble_changed) m_on_experimental_rumble_changed(m_experimental_rumble_enabled);
                        break;
                    case 7:
                        m_vr_state.perspective_comp = !m_vr_state.perspective_comp;
                        m_settings_panel_dirty = true;
                        break;
                    case 8: m_vr_state.gamma       = clamp(m_vr_state.gamma      + dir*step, 0.5f, 2.0f); m_settings_panel_dirty=true; break;
                    case 9: m_vr_state.contrast    = clamp(m_vr_state.contrast   + dir*step, 0.5f, 2.0f); m_settings_panel_dirty=true; break;
                    case 10: m_vr_state.saturation = clamp(m_vr_state.saturation + dir*step, 0.0f, 2.0f); m_settings_panel_dirty=true; break;
                    case 11: m_vr_state.brightness = clamp(m_vr_state.brightness + dir*step, 0.5f, 2.0f); m_settings_panel_dirty=true; break;
                    case 12: {
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
                    case 13: {
                        const int steps = std::clamp((int)std::lround(m_vr_state.vr_resolution_scale * 4.0f) + dir, 1, 16);
                        const float new_scale = steps * 0.25f;
                        if (std::abs(new_scale - m_vr_state.vr_resolution_scale) > 0.001f) {
                            m_vr_state.vr_resolution_scale = new_scale;
                            destroy_swapchains();
                        }
                        m_settings_panel_dirty = true;
                        break;
                    }
                    case 14:
                        m_vr_state.depth_mode = cycle_depth_mode(m_vr_state.depth_mode, dir);
                        m_settings_panel_dirty = true;
                        break;
                    case 15: m_vr_state.sprite_y_depth_spread = clamp(m_vr_state.sprite_y_depth_spread + dir*step, 0.0f, 2.0f); m_settings_panel_dirty=true; break;
                    case 16: m_vr_state.audio_spatial_mode = (m_vr_state.audio_spatial_mode + (dir == 0 ? 1 : dir) + 4) % 4; m_settings_panel_dirty=true; break;
                    case 17: m_vr_state.audio_screen_lock = !m_vr_state.audio_screen_lock; m_settings_panel_dirty=true; break;
                    case 18:
                        m_vr_state.side_panel_mode = (m_vr_state.side_panel_mode + 1) % kSidePanelModeCount;
                        m_settings_panel_dirty = true;
                        m_help_panel_dirty = true;
                        m_perf_overlay_dirty = true;
                        m_quick_panel_dirty = true;
                        break;
                    case 19:
                        m_vr_state.real_geometry_boxes = !m_vr_state.real_geometry_boxes;
                        m_settings_panel_dirty = true;
                        break;
                    case 20:
                        m_vr_state.silhouette_sides = !m_vr_state.silhouette_sides;
                        m_settings_panel_dirty = true;
                        break;
                    case 21:
                        m_vr_state.rom_preview_enabled = !m_vr_state.rom_preview_enabled;
                        m_rom_preview.set_enabled(m_vr_state.rom_preview_enabled);
                        m_settings_panel_dirty = true;
                        break;
                    case 22:
                        m_vr_state.gun_model = (m_vr_state.gun_model + 1) % 3;
                        m_settings_panel_dirty = true;
                        break;
                    default: break;
                }
                if (m_on_vr_state_changed) {
                    m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
                }
            };

            int row = m_settings_panel_hovered;
            if (rtrig_rising && m_laser_hit && row >= 0) {
                if (row == 18) {
                    if (m_settings_panel_hovered_id >= 0) {
                        m_vr_state.side_panel_mode = std::clamp(m_settings_panel_hovered_id, 0, kSidePanelModeCount - 1);
                        m_settings_panel_dirty = true;
                        m_help_panel_dirty = true;
                        m_perf_overlay_dirty = true;
                        m_quick_panel_dirty = true;
                        if (m_on_vr_state_changed) {
                            m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
                        }
                    }
                    fire_haptic(true, 0.3f, 25);
                    do_step_one();
                } else if (row == 0 || row == 2 || row == 4 || row == 5 || row == 6 || row == 7 || row == 16 || row == 19 || row == 20 || row == 21 || row == 22) {
                    adjust_setting(row, 0);
                    fire_haptic(true, 0.3f, 25);
                    do_step_one();
                } else if (row == 1 || row == 3) {
                    if (m_settings_panel_area != 0) {
                        const int dir = (m_settings_panel_area == 1) ? -1 : 1;
                        adjust_setting(row, dir);
                        fire_haptic(true, 0.2f, 15);
                        do_step_one();
                    }
                } else if (row <= 12) {
                    if (m_settings_panel_area == 0) {
                        // Numeric/cycle rows only act on the visible minus/plus zones.
                    } else {
                    int dir = (m_settings_panel_area == 1) ? -1 : 1;
                    adjust_setting(row, dir);
                    fire_haptic(true, 0.2f, 15);
                    do_step_one();
                    }
                } else if (row == 13 || row == 14 || row == 15) {
                    if (m_settings_panel_area == 0) {
                        // Numeric/cycle rows only act on the visible minus/plus zones.
                    } else {
                    int dir = (m_settings_panel_area == 1) ? -1 : 1;
                    adjust_setting(row, dir);
                    fire_haptic(true, 0.2f, 15);
                    do_step_one();
                    }
                } else {
                    // Action buttons (rows 23-29)
                    switch (row) {
                        case 23:
                            if (m_gun_capable && m_gun_hand != 0) {
                                m_settings_action_pending = 6; // Calibrate lightgun
                            }
                            break;
                        case 24:
                            m_settings_action_pending = 5; break; // Reset
                        case 25:
                            if (m_current_rom_name.empty()) {
                                set_status("Load a ROM before saving game settings.");
                                fire_haptic(true, 0.2f, 20);
                                break;
                            }
                            m_settings_action_pending = 1; break; // Save Game
                        case 26:
                            if (m_current_rom_name.empty()) {
                                set_status("Load a ROM before saving global settings.");
                                fire_haptic(true, 0.2f, 20);
                                break;
                            }
                            m_settings_action_pending = 2; break; // Save Global
                        case 27:
                            if (m_current_rom_name.empty()) {
                                set_status("Load a ROM before loading game settings.");
                                fire_haptic(true, 0.2f, 20);
                                break;
                            }
                            m_settings_action_pending = 3; break; // Load Game
                        case 28:
                            if (m_current_rom_name.empty()) {
                                set_status("Load a ROM before loading global settings.");
                                fire_haptic(true, 0.2f, 20);
                                break;
                            }
                            m_settings_action_pending = 4; break; // Load Global
                        case 29: // Back
                            m_active_sub_panel        = m_settings_return_to_quick ? k_panel_quick_edit : 0;
                            m_settings_panel_hovered = -1;
                            m_settings_panel_area    = 0;
                            m_main_menu_dirty        = !m_settings_return_to_quick;
                            m_quick_panel_dirty      = m_settings_return_to_quick;
                            m_settings_return_to_quick = false;
                            break;
                        default: break;
                    }
                    fire_haptic(true, 0.4f, 40);
                }
            }
            // Continuous adjustment while holding trigger + stick X (only for float rows and int sliders)
            if (rtrig_now && ((row >= 8 && row <= 11) || row == 13 || row == 15 || row == 17) && std::abs(rx) > 0.5f
                && now_panel - m_last_settings_fire > k_setting_interval) {
                m_last_settings_fire = now_panel;
                adjust_setting(row, rx > 0 ? 1 : -1);
                do_step_one();
            }
        } else if (m_laser_panel == k_panel_save_state) {
            int cell = m_save_state_panel_hovered;
            if (rtrig_rising && m_laser_hit && cell >= 0) {
                std::string err;
                if (cell < (k_save_state_slot_count * 2)) {
                    const int slot = cell % k_save_state_slot_count;
                    if (cell < k_save_state_slot_count) {
                        if (slot >= (int)m_save_state_slots.size() || !m_save_state_slots[slot].occupied) {
                            set_status("Slot " + std::to_string(slot + 1) + " is empty.");
                            fire_haptic(true, 0.2f, 20);
                        } else if (load_state_from_slot(slot, err)) {
                            set_status("Loaded state " + std::to_string(slot + 1) + ".");
                            m_menu_open = false;
                            m_ctrlmap_mode = false;
                            m_active_sub_panel = 0;
                            m_laser_hit = false;
                            EmuFreezeCtrl freeze_fn;
                            { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                            if (freeze_fn) freeze_fn(false);
                            m_emu_frozen_display = false;
                            fire_haptic(true, 0.45f, 50);
                        } else {
                            set_status("Load failed: " + err);
                            fire_haptic(true, 0.2f, 20);
                        }
                    } else {
                        if (save_state_to_slot(slot, err)) {
                            set_status("Saved state " + std::to_string(slot + 1) + ".");
                            fire_haptic(true, 0.35f, 40);
                        } else {
                            set_status("Save failed: " + err);
                            fire_haptic(true, 0.2f, 20);
                        }
                    }
                } else if (cell == 6) {
                    m_autosave_interval_seconds = next_autosave_interval_seconds(m_autosave_interval_seconds);
                    m_last_autosave_time_ms = monotonic_time_ms();
                    persist_save_automation_settings();
                    m_save_state_panel_dirty = true;
                    set_status("Autosave every " + autosave_interval_label(m_autosave_interval_seconds) + ".");
                    fire_haptic(true, 0.3f, 30);
                } else if (cell == 7) {
                    m_load_last_save_enabled = !m_load_last_save_enabled;
                    persist_save_automation_settings();
                    m_save_state_panel_dirty = true;
                    set_status(std::string("Load last save ") + (m_load_last_save_enabled ? "ON." : "OFF."));
                    fire_haptic(true, 0.3f, 30);
                }
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
            const int max_len = m_code_panel_quick_name_mode
                ? k_quick_name_max_len
                : (is_snes_filter_capable_config(m_config) ? 38 : (5 + 2 * (int)m_config.layers.size()));
            int key = m_code_panel_hovered;
            if (rtrig_rising && m_laser_hit && key >= 0) {
                if (m_code_panel_quick_name_mode && m_laser_hit_has_item) {
                    switch (m_laser_hit_item.role) {
                        case PanelRole::CodeCancel:
                            cancel_quick_preset_name(m_pending_quick_preset_kind, m_pending_quick_preset_slot);
                            m_code_input_buf.clear();
                            m_code_panel_quick_name_mode = false;
                            m_active_sub_panel = k_panel_quick_edit;
                            m_quick_panel_dirty = true;
                            m_code_panel_dirty = true;
                            set_status("Preset rename canceled.");
                            fire_haptic(true, 0.2f, 20);
                            break;
                        case PanelRole::CodeSpace:
                            if ((int)m_code_input_buf.size() < max_len) {
                                m_code_input_buf.push_back(' ');
                                m_code_panel_dirty = true;
                                fire_haptic(true, 0.2f, 15);
                            }
                            break;
                        case PanelRole::CodeConfirm:
                            submit_quick_preset_name(
                                m_pending_quick_preset_kind, m_pending_quick_preset_slot, m_code_input_buf);
                            m_code_input_buf.clear();
                            m_code_panel_quick_name_mode = false;
                            m_active_sub_panel = k_panel_quick_edit;
                            m_quick_panel_dirty = true;
                            m_code_panel_dirty = true;
                            fire_haptic(true, 0.35f, 35);
                            break;
                        case PanelRole::Key:
                            break;
                        default:
                            break;
                    }
                }
                if (m_laser_hit_has_item && m_laser_hit_item.role == PanelRole::Key) {
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

                        if (!m_code_panel_quick_name_mode) {
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
            }
        } else if (m_laser_panel == k_panel_credits) {
            // ---- Credits panel -----------------------------------------------
            if (rtrig_rising && m_laser_hit_has_item) {
                const int row = m_laser_hit_item.row;
                fire_haptic(true, 0.3f, 30);
                if (row == m_credits_window_visible) {
                    // Back row (appended after the visible entries)
                    m_active_sub_panel = 0;
                    m_main_menu_dirty = true;
                    m_credits_link_armed_index = -1;
                } else if (row >= 0 && row < m_credits_window_visible) {
                    const int entry_index = m_credits_window_first + row;
                    constexpr XrTime k_credits_link_arm_timeout = 4'000'000'000; // 4s
                    if (m_credits_link_armed_index == entry_index &&
                        (m_frame_predicted_time - m_credits_link_arm_time) < k_credits_link_arm_timeout) {
                        m_credits_link_armed_index = -1;
                        open_credits_link(entry_index);
                    } else if (entry_index >= 0 && entry_index < (int)m_credit_entries.size() &&
                               !m_credit_entries[entry_index].url.empty()) {
                        m_credits_link_armed_index = entry_index;
                        m_credits_link_arm_time = m_frame_predicted_time;
                        set_status("Opens in browser: " + m_credit_entries[entry_index].url + " - tap again to continue.");
                    }
                }
            }
            // Scroll with the right stick, same as the Homebrew list.
            {
                float ry_credits = 0.0f, dummy_x_credits = 0.0f;
                get_vec2(m_impl->act_rstick, dummy_x_credits, ry_credits);
                const int total = (int)m_credit_entries.size();
                const int max_scroll = std::max(0, total - kCreditsVisibleRows);
                if (ry_credits > 0.5f && m_credits_scroll > 0) {
                    m_credits_scroll = std::max(0, m_credits_scroll - 1);
                    m_credits_dirty = true;
                } else if (ry_credits < -0.5f && m_credits_scroll < max_scroll) {
                    m_credits_scroll = std::min(max_scroll, m_credits_scroll + 1);
                    m_credits_dirty = true;
                }
            }
        } else if (m_laser_panel == k_panel_homebrew) {
            // ---- Homebrew panel ---------------------------------------------
            if (rtrig_rising && m_laser_hit_has_item) {
                const int row = m_laser_hit_item.row;
                fire_haptic(true, 0.3f, 30);
                if (m_hw_view == 0) {
                    // List view
                    const int total_rows = (int)m_homebrew_panel_layout.items.size();
                    // Determine entry count from layout (row 0 = feed toggle, last row = back)
                    const int entry_count = std::max(0, total_rows - 2);
                    if (row == 0) {
                        // Feed selector dialog
                        if (m_vm && m_activity_global) {
                            JNIEnv* env = nullptr;
                            bool detach = false;
                            if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                                if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
                            }
                            if (env) {
                                jclass cls = env->GetObjectClass(m_activity_global);
                                jmethodID mid = env->GetMethodID(cls, "showHomebrewFeedDialog", "(I)V");
                                if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_hw_feed);
                                env->DeleteLocalRef(cls);
                                if (detach) m_vm->DetachCurrentThread();
                            }
                        }
                    } else if (row == total_rows - 1) {
                        // Back button (last row)
                        m_active_sub_panel = 0;
                        m_main_menu_dirty = true;
                    } else {
                        // Entry row → switch to detail view
                        m_hw_selected = m_hw_scroll + row - 1;
                        m_hw_view = 1;
                        m_hw_dirty = true;
                    }
                } else {
                    // Detail view rows: 0=back, 1=download/delete, 2=open website, 3=back (fallback)
                    if (row == 0 || row == 3) {
                        // Back
                        m_hw_view = 0;
                        m_hw_dirty = true;
                    } else if (row == 1) {
                        // Download or delete
                        if (m_vm && m_activity_global) {
                            JNIEnv* env = nullptr;
                            bool detach = false;
                            if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                                if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
                            }
                            if (env) {
                                jclass cls = env->GetObjectClass(m_activity_global);
                                jmethodID mid_check = env->GetMethodID(cls, "isHomebrewDownloaded", "(I)Z");
                                jboolean downloaded = mid_check ? env->CallBooleanMethod(m_activity_global, mid_check, (jint)m_hw_selected) : JNI_FALSE;
                                if (downloaded) {
                                    jmethodID mid = env->GetMethodID(cls, "homebrewDelete", "(I)V");
                                    if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_hw_selected);
                                } else {
                                    jmethodID mid = env->GetMethodID(cls, "homebrewDownload", "(I)V");
                                    if (mid) {
                                        env->CallVoidMethod(m_activity_global, mid, (jint)m_hw_selected);
                                        m_hw_downloading = true;
                                    }
                                }
                                env->DeleteLocalRef(cls);
                                if (detach) m_vm->DetachCurrentThread();
                            }
                        }
                        m_hw_dirty = true;
                    } else if (row == 2) {
                        // Open website
                        if (m_vm && m_activity_global) {
                            JNIEnv* env = nullptr;
                            bool detach = false;
                            if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                                if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) detach = true;
                            }
                            if (env) {
                                jclass cls = env->GetObjectClass(m_activity_global);
                                jmethodID mid = env->GetMethodID(cls, "homebrewOpenWebsite", "(I)V");
                                if (mid) env->CallVoidMethod(m_activity_global, mid, (jint)m_hw_selected);
                                env->DeleteLocalRef(cls);
                                if (detach) m_vm->DetachCurrentThread();
                            }
                        }
                    }
                }
            }
            // Scroll in list view
            if (m_hw_view == 0) {
                float ry_hw = 0.0f;
                float dummy_x_hw = 0.0f;
                get_vec2(m_impl->act_rstick, dummy_x_hw, ry_hw);
                if (ry_hw > 0.5f && m_hw_scroll > 0) {
                    m_hw_scroll--;
                    m_hw_dirty = true;
                } else if (ry_hw < -0.5f) {
                    m_hw_scroll++;
                    m_hw_dirty = true;
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

            // While free-roam ("fly mode" — turn/scale/pan/throttle the environment) or
            // the layer deck is active, the same buttons players reach for (sticks,
            // triggers) would otherwise also fire as SNES input. Zero all emulator
            // input while either is active.
            if (m_locomotion_active || m_layer_deck_active) {
                m_input_state = EmulatorInputState{};
            } else {

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
            // Suppressed while that hand's laser is on a unified-menu panel (see
            // m_right/left_hand_menu_hover, set by render_frame()'s panel block)
            // so pulling the trigger to click a menu row doesn't ALSO fire as a
            // real emulator button press on whatever this backend maps RTRIG/LTRIG
            // to. One-frame-stale, matching the field comments' documented lag.
            qi_state[QI_RTRIG]        = !m_right_hand_menu_hover && get_float(m_impl->act_rtrig) > 0.5f;
            qi_state[QI_LTRIG]        = !m_left_hand_menu_hover  && get_float(m_impl->act_ltrig) > 0.5f;
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

            // Motion-control latches (computed once per frame in
            // update_dpad_headset() / update_air_wheel(), before the menu
            // branch, so their live readouts keep working while a panel is
            // open) feed qi_state here -- BEFORE the button map runs, so every
            // gesture is remappable exactly like a stick direction.
            qi_state[QI_HEAD_UP]    = m_dpad_headset_up;
            qi_state[QI_HEAD_DOWN]  = m_dpad_headset_down;
            qi_state[QI_HEAD_LEFT]  = m_dpad_headset_left;
            qi_state[QI_HEAD_RIGHT] = m_dpad_headset_right;
            qi_state[QI_WHEEL_LEFT]      = m_air_wheel.steer_left;
            qi_state[QI_WHEEL_RIGHT]     = m_air_wheel.steer_right;
            qi_state[QI_WHEEL_ACCEL]     = m_air_wheel.accel;
            qi_state[QI_WHEEL_BRAKE]     = m_air_wheel.brake;
            qi_state[QI_WHEEL_GEAR_UP]   = m_air_wheel.gear_up;
            qi_state[QI_WHEEL_GEAR_DOWN] = m_air_wheel.gear_down;
            qi_state[QI_WHEEL_HANDBRAKE] = m_air_wheel.handbrake;
            qi_state[QI_WHEEL_BIKE]      = m_air_wheel.bike;
            qi_state[QI_AIR_JUMP]        = m_air_jump.jump;
            qi_state[QI_AIR_CROUCH]      = m_air_jump.crouch;
            // Air Fighter: the currently-playing sequence step, plus any
            // real-time charge direction being held down.
            {
                unsigned m = 0;
                const auto& mac = m_air_fighter.macro;
                if (mac.cur >= 0 && mac.cur < mac.step_count) m = mac.steps[mac.cur];
                // A charge holds Down as well as the side: that is the real
                // down-back charge, and it is what makes both the across and
                // the up finish available from one held position.
                if (m_air_fighter.charge_dir > 0)      m |= FB_RIGHT | FB_DOWN;
                else if (m_air_fighter.charge_dir < 0) m |= FB_LEFT  | FB_DOWN;
                qi_state[QI_FIGHT_UP]    = (m & FB_UP)    != 0;
                qi_state[QI_FIGHT_DOWN]  = (m & FB_DOWN)  != 0;
                qi_state[QI_FIGHT_LEFT]  = (m & FB_LEFT)  != 0;
                qi_state[QI_FIGHT_RIGHT] = (m & FB_RIGHT) != 0;
                qi_state[QI_FIGHT_PUNCH] = (m & FB_PUNCH) != 0;
                qi_state[QI_FIGHT_KICK]  = (m & FB_KICK)  != 0;
                qi_state[QI_FIGHT_PUNCH_HARD] = (m & FB_PUNCH_HARD) != 0;
                qi_state[QI_FIGHT_KICK_HARD]  = (m & FB_KICK_HARD)  != 0;
            }

            // Each motion also stands in for a physical input (VrState::
            // motion_bind), so it flows through the ordinary Controller Map
            // rather than needing its own console-button assignment: turning
            // the wheel left reads as LS Left, and whatever this game maps LS
            // Left to is what actually happens. ORed on, so the real stick
            // keeps working alongside the gesture.
            //
            // Is motion i switched on right now? Suppression below has to key
            // off this rather than off the latch, because a claimed input must
            // stay dead even between gestures -- otherwise the stick would
            // still work whenever you were not actively turning the wheel.
            const auto motion_active = [&](int i) -> bool {
                const bool head  = m_vr_state.dpad_headset_enabled;
                const bool wheel = m_vr_state.air_wheel_enabled;
                switch (i) {
                    case 0: case 1: case 2: case 3: return head;
                    case 4: case 5:  return wheel && m_vr_state.air_wheel_steer_enabled;
                    case 6:          return wheel && m_vr_state.air_wheel_accel_enabled;
                    case 7:          return wheel && m_vr_state.air_wheel_brake_enabled;
                    case 8: case 11: return wheel && m_vr_state.air_wheel_gear_enabled;
                    case 9:          return wheel && m_vr_state.air_wheel_handbrake_enabled;
                    case 10:         return wheel && m_vr_state.air_wheel_bike_enabled;
                    case 12: case 13: return m_vr_state.air_jump_enabled;
                    case 14: case 15: case 16: case 17: case 18: case 19:
                    case 20: case 21:
                        return m_vr_state.air_fighter_enabled;
                    default:         return false;
                }
            };
            const auto valid_target = [](int t) {
                // A motion standing in for another motion would be circular.
                return t > QI_NONE && t < VrState::kMotionBindFirst;
            };

            // Exclusive Motion Input: clear every physical input an enabled
            // motion has claimed, BEFORE the motions write to them, so the real
            // stick or button no longer registers on its own.
            if (m_vr_state.motion_exclusive) {
                for (int i = 0; i < VrState::kMotionBindCount; ++i) {
                    const int target = m_vr_state.motion_bind[i];
                    if (motion_active(i) && valid_target(target)) qi_state[target] = false;
                }
            }
            for (int i = 0; i < VrState::kMotionBindCount; ++i) {
                const int motion_qi = VrState::kMotionBindFirst + i;
                if (motion_qi >= QI_COUNT || !qi_state[motion_qi]) continue;
                const int target = m_vr_state.motion_bind[i];
                if (valid_target(target)) qi_state[target] = true;
            }

            // Dual wielding hands the second controller to player two, so its
            // buttons and trigger must stop driving player one's pad — without
            // this, player two pressing "join" also presses whatever player
            // one has that button mapped to, and both players fight over one
            // set of inputs.
            const bool dual_guns_live =
                m_gun_capable && m_gun_hand != 0 && m_dual_gun_enabled;
            const bool p2_hand_is_right = dual_guns_live && (m_gun_hand == 2);
            const auto qi_belongs_to_p2 = [&](int qi) -> bool {
                if (!dual_guns_live) return false;
                switch (qi) {
                case QI_A: case QI_B: case QI_RTRIG: case QI_RGRIP:
                case QI_RSTICK_UP: case QI_RSTICK_DOWN:
                case QI_RSTICK_LEFT: case QI_RSTICK_RIGHT:
                    return p2_hand_is_right;
                case QI_X: case QI_Y: case QI_LTRIG: case QI_LGRIP:
                case QI_LSTICK_UP: case QI_LSTICK_DOWN:
                case QI_LSTICK_LEFT: case QI_LSTICK_RIGHT:
                    return !p2_hand_is_right;
                default:
                    return false; // head tilt, wheel, fighter motions: not a hand
                }
            };

            auto mapped = [&](int snes_btn) -> bool {
                int qi = m_button_map[snes_btn];
                if (qi <= QI_NONE || qi >= QI_COUNT) return false;
                if (qi_belongs_to_p2(qi)) return false;
                return qi_state[qi];
            };
            auto input_mapped_elsewhere = [&](int qi, int expected_snes_btn) -> bool {
                for (int i = 0; i < SNES_BUTTON_COUNT; ++i) {
                    if (i != expected_snes_btn && m_button_map[i] == qi) return true;
                }
                return false;
            };

            bool btn_b      = mapped(SNES_B);
            bool btn_a      = mapped(SNES_A);
            bool btn_y      = mapped(SNES_Y);
            bool btn_x      = mapped(SNES_X);
            bool btn_l      = mapped(SNES_L);
            bool btn_r      = mapped(SNES_R);
            bool btn_start  = mapped(SNES_START);
            bool btn_select = mapped(SNES_SELECT);
            bool btn_c      = mapped(SATURN_C);
            bool btn_z      = mapped(SATURN_Z);

            // D-pad from button map (can also be mapped to sticks above)
            bool dpad_up    = mapped(SNES_UP);
            bool dpad_down  = mapped(SNES_DOWN);
            bool dpad_left  = mapped(SNES_LEFT);
            bool dpad_right = mapped(SNES_RIGHT);
            // Same player-two exclusion as mapped() above.
            const auto raw_stick = [&](int qi) { return !qi_belongs_to_p2(qi) && qi_state[qi]; };
            if (!input_mapped_elsewhere(QI_RSTICK_UP, SNES_UP))       dpad_up    = dpad_up    || raw_stick(QI_RSTICK_UP);
            if (!input_mapped_elsewhere(QI_RSTICK_DOWN, SNES_DOWN))   dpad_down  = dpad_down  || raw_stick(QI_RSTICK_DOWN);
            if (!input_mapped_elsewhere(QI_RSTICK_LEFT, SNES_LEFT))   dpad_left  = dpad_left  || raw_stick(QI_RSTICK_LEFT);
            if (!input_mapped_elsewhere(QI_RSTICK_RIGHT, SNES_RIGHT)) dpad_right = dpad_right || raw_stick(QI_RSTICK_RIGHT);
            // (Head tilt and wheel steering no longer need a d-pad fallback
            // here: they stand in for LS Left/Right etc. via motion_bind above,
            // so they arrive through mapped() like any stick input.)

            // Haptic click on any button press (rising edge)
            bool any_new_press =
                (btn_b  && !m_input_state.button_b)  ||
                (btn_a  && !m_input_state.button_a)  ||
                (btn_y  && !m_input_state.button_y)  ||
                (btn_x  && !m_input_state.button_x)  ||
                (btn_l  && !m_input_state.button_l)  ||
                (btn_r  && !m_input_state.button_r)  ||
                (btn_c  && !m_input_state.button_c)  ||
                (btn_z  && !m_input_state.button_z)  ||
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
            m_input_state.button_c      = btn_c;
            m_input_state.button_z      = btn_z;

            // ---- Lightgun aiming (Virtua Cop/Saturn via MAME, Super Scope, Zapper) ----
            // Raycasts the chosen controller's aim ray (m_gun_hand: 1=right, 2=left) against
            // the game screen quad (same plane math as the menu laser hit-test above, but
            // against the gameplay canvas anchor instead of a UI panel), corrected by
            // m_gun_recenter_quat, and feeds the hit point through as libretro
            // RETRO_DEVICE_LIGHTGUN screen coordinates. Also drives the blocky gun model
            // rendered attached to that controller (m_gun_render_pose/show).
            if (m_gun_hand != 0) {
                const bool right_hand = (m_gun_hand == 1);
                XrSpace aim_space = right_hand ? m_impl->raim_space : m_impl->laim_space;
                const bool reload_btn   = right_hand ? qi_state[QI_B] : qi_state[QI_Y];
                const bool trigger_btn  = right_hand ? qi_state[QI_RTRIG] : qi_state[QI_LTRIG];
                const bool calibration_was_active = m_gun_calibration_active;

                bool have_aim = false;
                bool offscreen = true;
                bool aiming_offscreen_margin = false;
                int16_t gx = 0, gy = 0;
                m_gun_render_show = false;
                m_gun_calibration_surface_valid = false;
                load_lightgun_calibration();
                if (aim_space != XR_NULL_HANDLE) {
                    XrPosef aim{};
                    if (get_controller_pose(aim_space, aim)) {
                        const XrVector3f& O = aim.position;
                        const VrState render_state = effective_render_state(m_vr_state);
                        LightgunSurface surface{};
                        if (game_canvas_lightgun_surface(
                                m_render_layer_refs, m_config, m_canvas_x, m_canvas_y,
                                m_canvas_az, m_canvas_el, m_canvas_scale, render_state,
                                m_cached_frame_out.width, m_cached_frame_out.height, surface)) {
                            m_gun_calibration_surface = surface;
                            m_gun_calibration_surface_valid = true;

                            LightgunCalibrationProfile context;
                            context.hand = m_gun_hand;
                            context.backend = (int)m_current_backend_kind;
                            context.frame_width = m_cached_frame_out.width;
                            context.frame_height = m_cached_frame_out.height;
                            context.upscale_mode = (int)m_vr_state.upscale_mode;
                            context.canvas_x = m_canvas_x;
                            context.canvas_y = m_canvas_y;
                            context.canvas_az = m_canvas_az;
                            context.canvas_el = m_canvas_el;
                            context.canvas_scale = m_canvas_scale;
                            context.screen_curve = surface.screen_curve;
                            context.tilt_x = render_state.tilt_x;
                            context.tilt_y = render_state.tilt_y;
                            context.world_scale = m_world_scale;
                            context.world_forward_offset = m_world_forward_offset;

                            m_gun_calibration_profile_active = false;
                            if (!m_gun_calibration_active) {
                                for (auto it = m_gun_calibration_profiles.rbegin();
                                     it != m_gun_calibration_profiles.rend(); ++it) {
                                    if (it->matches(context.hand, context.backend, context.frame_width,
                                            context.frame_height, context.upscale_mode,
                                            context.canvas_x, context.canvas_y, context.canvas_az,
                                            context.canvas_el, context.canvas_scale, context.screen_curve,
                                            context.tilt_x, context.tilt_y, context.world_scale,
                                            context.world_forward_offset)) {
                                        m_gun_calibration_active_profile = *it;
                                        m_gun_calibration_profile_active = true;
                                        break;
                                    }
                                }
                            }

                            if (m_gun_calibration_active && m_gun_calibration_target == 0 &&
                                m_gun_calibration_sample_frames == 0) {
                                m_gun_calibration_capture_context = context;
                            }

                            const XrVector3f raw_D = quat_rotate_vec(
                                aim.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
                            // The old quick-recenter-on-X/A gesture used to live here
                            // (press X/A while aiming to snap m_gun_recenter_quat to the
                            // screen center). Removed: it stole X/A away from real game
                            // input during Zapper/Super Scope play on backends where X/A
                            // are actual buttons, and the real 5-point calibration-profile
                            // system plus the explicit "Recenter Aim" button (Controls >
                            // Lightgun) now fully supersede it.
                            const XrVector3f D = m_gun_calibration_active || m_gun_calibration_profile_active
                                ? raw_D : quat_rotate_vec(m_gun_recenter_quat, raw_D);
                            const XrQuaternionf corrected_orientation =
                                quat_multiply(m_gun_calibration_active || m_gun_calibration_profile_active
                                    ? XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f} : m_gun_recenter_quat,
                                    aim.orientation);
                            m_gun_render_show = true;
                            m_gun_render_pose.position = O;
                            m_gun_render_pose.orientation = corrected_orientation;

                            LightgunUv uv{};
                            bool raw_offscreen = true;
                            if (lightgun_raycast(O, D, surface, uv, raw_offscreen)) {
                                have_aim = true;
                                offscreen = raw_offscreen;

                                if (m_gun_calibration_active) {
                                    // Calibration may intentionally capture a
                                    // point just outside the visible canvas.
                                    // This measures the controller's raw aim
                                    // on the same mathematical screen surface;
                                    // the resulting mapping moves it back onto
                                    // the requested visible target later.
                                    const bool calibration_captureable =
                                        lightgun_calibration_uv_captureable(uv);
                                    if (m_gun_calibration_wait_release && !trigger_btn) {
                                        m_gun_calibration_wait_release = false;
                                    }
                                    if (!m_gun_calibration_wait_release && trigger_btn &&
                                        !m_gun_calibration_trigger_prev && calibration_captureable) {
                                        m_gun_calibration_sample_sum = uv;
                                        m_gun_calibration_sample_frames = 1;
                                    } else if (!m_gun_calibration_wait_release && trigger_btn &&
                                               m_gun_calibration_sample_frames > 0 &&
                                               calibration_captureable) {
                                        m_gun_calibration_sample_sum.u += uv.u;
                                        m_gun_calibration_sample_sum.v += uv.v;
                                        ++m_gun_calibration_sample_frames;
                                    }
                                    if (m_gun_calibration_sample_frames >= 6) {
                                        const float inv = 1.0f / (float)m_gun_calibration_sample_frames;
                                        m_gun_calibration_captured[m_gun_calibration_target] = {
                                            m_gun_calibration_sample_sum.u * inv,
                                            m_gun_calibration_sample_sum.v * inv
                                        };
                                        if (m_gun_calibration_target + 1 < LightgunCalibrationProfile::kPointCount) {
                                            ++m_gun_calibration_target;
                                            m_gun_calibration_wait_release = true;
                                            m_gun_calibration_sample_frames = 0;
                                            m_gun_calibration_sample_sum = {};
                                            static const char* labels[] = {"TOP-RIGHT", "BOTTOM-RIGHT", "BOTTOM-LEFT", "CENTER"};
                                            set_status(std::string("Calibration: release, then aim at ") +
                                                       labels[m_gun_calibration_target - 1] + " and pull trigger.");
                                        } else {
                                            finish_lightgun_calibration();
                                        }
                                    }
                                } else if (m_gun_calibration_profile_active) {
                                    uv = lightgun_apply_calibration(m_gun_calibration_active_profile, uv);
                                }

                                const float mapped_u = std::clamp(uv.u, 0.0f, 1.0f);
                                const float mapped_v = std::clamp(uv.v, 0.0f, 1.0f);
                                gx = (int16_t)((mapped_u * 2.0f - 1.0f) * 0x7FFF);
                                gy = (int16_t)((mapped_v * 2.0f - 1.0f) * 0x7FFF);
                                // Screen reload gate (opt-in, the persisted setting retains its
                                // old gun_offscreen_reload_enabled name for compatibility): any
                                // on-screen aim holds reload/hide. The margin is only used to
                                // distinguish a deliberate off-screen release from a near-edge
                                // shot.
                                constexpr float kOffscreenReloadMargin = 0.08f; // ~8% past each edge
                                aiming_offscreen_margin =
                                    uv.u < -kOffscreenReloadMargin || uv.u > 1.0f + kOffscreenReloadMargin ||
                                    uv.v < -kOffscreenReloadMargin || uv.v > 1.0f + kOffscreenReloadMargin;
                            }
                        }
                    }
                }
                m_input_state.gun_active    = true;
                m_input_state.gun_screen_x  = gx;
                m_input_state.gun_screen_y  = gy;
                m_input_state.gun_offscreen = !have_aim || offscreen;
                if (m_gun_calibration_release_required && !trigger_btn)
                    m_gun_calibration_release_required = false;
                m_input_state.gun_trigger   = (calibration_was_active ||
                                               m_gun_calibration_release_required)
                    ? false : trigger_btn;
                const bool gun_trigger_now = m_input_state.gun_trigger;
                if (gun_trigger_now && !m_gun_trigger_prev) {
                    fire_lightgun_vibration(right_hand, m_vr_state.gun_vibration_mode);
                    // Keep the visual envelope on the same timeline as the
                    // selected haptic effect. Mode 0 still gets the original
                    // sharp pistol recoil so disabling vibration does not
                    // remove the useful visual feedback.
                    m_gun_animation_mode = std::clamp(m_vr_state.gun_vibration_mode, 1,
                                                      VrState::kGunVibrationModeCount - 1);
                    m_gun_animation_start = m_frame_predicted_time;
                    roll_gun_muzzle_color();
                    m_gun_muzzle_burst = 0;
                }

                const XrTime anim_elapsed_ns = std::max<XrTime>(
                    0, m_frame_predicted_time - m_gun_animation_start);
                const float anim_elapsed_s = (float)anim_elapsed_ns * 1.0e-9f;
                const auto kick_envelope = [](float elapsed, float start, float length) {
                    const float t = (elapsed - start) / length;
                    return t >= 0.0f && t < 1.0f ? (1.0f - t) * (1.0f - t) : 0.0f;
                };

                // Scope rifle muzzle heat: a quick ramp up to glowing, then a
                // slower cool-down back to cold steel. Machinegun mode runs it
                // once per burst so the barrel flares three times.
                const auto heat_envelope = [](float elapsed, float start) {
                    constexpr float kRise = 0.09f, kFall = 0.34f;
                    const float t = elapsed - start;
                    if (t < 0.0f) return 0.0f;
                    if (t < kRise) return t / kRise;
                    const float cool = (t - kRise) / kFall;
                    return cool < 1.0f ? (1.0f - cool) * (1.0f - cool) : 0.0f;
                };

                m_gun_tilt = 0.0f;
                switch (m_gun_animation_mode) {
                    case 2: { // Machinegun: the slide cycles through each burst.
                        // Mirrors RumbleEffect::GunMachinegun exactly: 3 bursts
                        // 270 ms apart, each 7 pulses spaced 25 ms, so the
                        // visible slide chatter lands on the felt pulses.
                        constexpr float kBurstPeriod  = 0.27f;
                        constexpr float kPulseSpacing = 0.025f;
                        constexpr int   kBursts       = 3;
                        constexpr int   kPulses       = 7;
                        constexpr float kBurstLen     = kPulses * kPulseSpacing;
                        m_gun_recoil = 0.0f;
                        m_gun_muzzle_heat = std::max({
                            heat_envelope(anim_elapsed_s, 0.00f),
                            heat_envelope(anim_elapsed_s, kBurstPeriod),
                            heat_envelope(anim_elapsed_s, 2.0f * kBurstPeriod)});
                        for (int burst = 0; burst < kBursts; ++burst) {
                            const float in_burst = anim_elapsed_s - burst * kBurstPeriod;
                            if (in_burst < 0.0f || in_burst >= kBurstLen) continue;
                            // Per-pulse saw: snap back fast, return over the pulse.
                            const float phase = std::fmod(in_burst, kPulseSpacing) / kPulseSpacing;
                            const float chatter = (1.0f - phase) * (1.0f - phase);
                            // Slight taper so the first/last pulses sit lower,
                            // matching the softened pulses in the haptic pattern.
                            const float taper = 0.80f + 0.20f *
                                std::sin(3.14159265359f * (in_burst / kBurstLen));
                            m_gun_recoil = std::max(m_gun_recoil, chatter * taper);
                            // One fresh muzzle colour per burst, so a full
                            // machinegun trigger pull flashes three colours.
                            if (burst > m_gun_muzzle_burst) {
                                m_gun_muzzle_burst = burst;
                                roll_gun_muzzle_color();
                            }
                        }
                        break;
                    }
                    case 3: { // Revolver: rise with the mechanical swell, then settle.
                        constexpr float kTiltDuration = 0.78f;
                        const float t = std::clamp(anim_elapsed_s / kTiltDuration, 0.0f, 1.0f);
                        constexpr float kPi = 3.14159265359f;
                        m_gun_tilt = 0.16f * std::sin(kPi * t); // about 9 degrees upward
                        // A small slide kick at the full-strength part of the swell.
                        m_gun_recoil = kick_envelope(anim_elapsed_s, 0.315f, 0.18f);
                        m_gun_muzzle_heat = heat_envelope(anim_elapsed_s, 0.315f);
                        break;
                    }
                    case 1:
                    default: // Standard sharp recoil.
                        m_gun_recoil = kick_envelope(anim_elapsed_s, 0.0f, 0.15f);
                        m_gun_muzzle_heat = heat_envelope(anim_elapsed_s, 0.0f);
                }
                m_gun_trigger_prev = gun_trigger_now;
                const bool screen_reload_active = !calibration_was_active &&
                    m_vr_state.gun_offscreen_reload_enabled && have_aim && !aiming_offscreen_margin;
                m_input_state.gun_reload = calibration_was_active ? false :
                    (reload_btn || (screen_reload_active && m_vr_state.gun_offscreen_reload_button == 0));
                // Custom-mapped target (1-10 -> A/B/X/Y/L/R/Start/Select/C/Z): for games whose
                // "reload" is a normal joypad button rather than the lightgun reload signal
                // above. ORed onto whatever that button's own held state already was this frame.
                if (screen_reload_active) {
                    switch (m_vr_state.gun_offscreen_reload_button) {
                        case 1:  m_input_state.button_a      = true; break;
                        case 2:  m_input_state.button_b      = true; break;
                        case 3:  m_input_state.button_x      = true; break;
                        case 4:  m_input_state.button_y      = true; break;
                        case 5:  m_input_state.button_l      = true; break;
                        case 6:  m_input_state.button_r      = true; break;
                        case 7:  m_input_state.button_start  = true; break;
                        case 8:  m_input_state.button_select = true; break;
                        case 9:  m_input_state.button_c      = true; break;
                        case 10: m_input_state.button_z      = true; break;
                        default: break; // 0 = handled via gun_reload above
                    }
                }
                if (m_gun_calibration_active) m_gun_calibration_trigger_prev = trigger_btn;

                // ---- Second gun (dual wielding) --------------------------------
                // Two-player gun titles expect a gun in each port, so the other
                // controller drives player two: its own aim, trigger and Start.
                // Deliberately not a second copy of the block above — player two
                // needs aim and buttons, not the calibration capture state
                // machine, the haptic envelope or the gun-model animation, all of
                // which are single-instance and stay with player one. Calibration
                // profiles are already keyed by hand, so gun two picks up its own
                // profile here with no extra storage.
                m_input_state.gun2_active = false;
                if (m_dual_gun_enabled && !m_gun_calibration_active &&
                    m_gun_calibration_surface_valid) {
                    const bool p2_right = !right_hand;
                    const int p2_hand = p2_right ? 1 : 2;
                    XrSpace p2_aim_space = p2_right ? m_impl->raim_space : m_impl->laim_space;
                    XrPosef p2_aim{};
                    if (p2_aim_space != XR_NULL_HANDLE && get_controller_pose(p2_aim_space, p2_aim)) {
                        const VrState p2_render_state = effective_render_state(m_vr_state);
                        const XrVector3f& p2_O = p2_aim.position;
                        const XrVector3f p2_D = quat_rotate_vec(
                            p2_aim.orientation, XrVector3f{0.0f, 0.0f, -1.0f});

                        // Same surface as player one — one screen, two aims.
                        LightgunUv p2_uv{};
                        bool p2_raw_offscreen = true;
                        const bool p2_have_aim = lightgun_raycast(
                            p2_O, p2_D, m_gun_calibration_surface, p2_uv, p2_raw_offscreen);
                        if (p2_have_aim) {
                            for (auto it = m_gun_calibration_profiles.rbegin();
                                 it != m_gun_calibration_profiles.rend(); ++it) {
                                if (it->matches(p2_hand, (int)m_current_backend_kind,
                                        m_cached_frame_out.width, m_cached_frame_out.height,
                                        (int)m_vr_state.upscale_mode,
                                        m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el,
                                        m_canvas_scale, m_gun_calibration_surface.screen_curve,
                                        p2_render_state.tilt_x, p2_render_state.tilt_y,
                                        m_world_scale, m_world_forward_offset)) {
                                    p2_uv = lightgun_apply_calibration(*it, p2_uv);
                                    break;
                                }
                            }
                        }
                        // Player one stays active while aiming away from the
                        // screen (it just reports offscreen, which is what an
                        // offscreen reload needs). Player two does the same, so
                        // its gun model no longer blinks out of existence the
                        // moment the ray leaves the canvas.
                        const float p2_u = std::clamp(p2_uv.u, 0.0f, 1.0f);
                        const float p2_v = std::clamp(p2_uv.v, 0.0f, 1.0f);
                        m_input_state.gun2_active    = true;
                        m_input_state.gun2_screen_x  = (int16_t)((p2_u * 2.0f - 1.0f) * 0x7FFF);
                        m_input_state.gun2_screen_y  = (int16_t)((p2_v * 2.0f - 1.0f) * 0x7FFF);
                        m_input_state.gun2_offscreen = !p2_have_aim || p2_raw_offscreen;
                        m_input_state.gun2_trigger =
                            p2_right ? qi_state[QI_RTRIG] : qi_state[QI_LTRIG];
                        m_input_state.gun2_reload =
                            p2_right ? qi_state[QI_B] : qi_state[QI_Y];
                        // Player two's own Start, which is the point of a real
                        // second port: it can join and pause by itself. On a
                        // GunCon there is no Start pin -- the gun's A button is
                        // what Point Blank and Time Crisis read as start/join --
                        // so this drives both, and each backend picks the one
                        // its peripheral actually has.
                        m_input_state.gun2_button_start =
                            p2_right ? qi_state[QI_A] : qi_state[QI_X];
                        m_input_state.gun2_button_a = m_input_state.gun2_button_start;
                        // The gun's second face button (GunCon B). Grip, since
                        // A/X and B/Y are already start and reload.
                        m_input_state.gun2_button_b =
                            p2_right ? qi_state[QI_RGRIP] : qi_state[QI_LGRIP];
                        // Player two gets its own kick, on its own
                        // controller, from its own vibration setting -- and
                        // its own visible recoil, started by its own trigger.
                        if (m_input_state.gun2_trigger && !m_gun2_trigger_prev) {
                            fire_lightgun_vibration(p2_right, m_vr_state.gun2_vibration_mode);
                            m_gun2_animation_start = m_frame_predicted_time;
                        }
                        m_gun2_trigger_prev = m_input_state.gun2_trigger;
                        {
                            const XrTime elapsed_ns = std::max<XrTime>(
                                0, m_frame_predicted_time - m_gun2_animation_start);
                            const float t = (float)elapsed_ns * 1.0e-9f / 0.15f;
                            m_gun2_recoil = (m_gun2_animation_start != 0 && t < 1.0f)
                                ? (1.0f - t) * (1.0f - t) : 0.0f;
                        }
                        m_gun2_render_show = true;
                        m_gun2_render_pose = p2_aim;
                    }
                }
                if (!m_input_state.gun2_active) {
                    m_gun2_render_show = false;
                    m_gun2_trigger_prev = false;
                    m_gun2_recoil = 0.0f;
                    m_gun2_animation_start = 0;
                }
            } else {
                m_input_state.gun_active = false;
                m_input_state.gun2_active = false;
                m_gun2_render_show = false;
                m_gun_trigger_prev = false;
                m_gun_recoil = 0.0f;
                m_gun_tilt = 0.0f;
                m_gun_animation_start = 0;
                m_gun_muzzle_burst = -1;
                m_gun_muzzle_heat = 0.0f;
                m_gun_muzzle_color[0] = 0.55f;
                m_gun_muzzle_color[1] = 0.56f;
                m_gun_muzzle_color[2] = 0.58f;
                m_gun_render_show = false;
                m_gun_calibration_surface_valid = false;
            }

            } // else (grip not held)
        }

        if (!live_layer_grip_lock) {
            // Right stick click: short press (on release) → recenter, hold ≥500 ms → toggle
            // passthrough fires immediately at the 500ms mark (no need to release), matching Edit
            // Mode's right-stick-click binding.
            bool rclick = get_bool(m_impl->act_rclick);
            if (rclick && !m_rstick_click_prev) {
                m_rclick_press_time = m_frame_predicted_time;
                m_rclick_passthrough_fired = false;
            } else if (rclick && m_rstick_click_prev && !m_rclick_passthrough_fired) {
                const XrTime held_ns = m_frame_predicted_time - m_rclick_press_time;
                if (held_ns >= 500'000'000LL) {
                    m_rclick_passthrough_fired = true;
                    m_vr_state.shadows = !m_vr_state.shadows;
                    m_settings_panel_dirty = true;
                    sync_passthrough_state();
                    set_status(m_vr_state.shadows
                        ? (passthrough_active() ? "Passthrough ON" : "Passthrough unavailable on this OpenXR runtime.")
                        : "Passthrough OFF");
                    fire_haptic(true, 0.25f, 30);
                }
            } else if (!rclick && m_rstick_click_prev) {
                if (!m_rclick_passthrough_fired) recenter_to_hmd();
                m_rclick_press_time = 0;
            }
            m_rstick_click_prev = rclick;
        } else {
            // Do not let a stick-click gesture leak through the layer lock.
            m_rstick_click_prev = get_bool(m_impl->act_rclick);
            m_rclick_press_time = 0;
            m_rclick_passthrough_fired = false;
        }
        m_rtrig_prev = false;
    }
}

// ============================================================
// apply_pending_vr_changes (called at frame start on XR thread)
// ============================================================
void OpenXrShell::apply_pending_vr_changes() {
    bool visual_change = false;
    if (m_request_open_menu.exchange(false)) {
        open_rom_menu();
        m_emu_frozen_display = false;
        visual_change = true;
    }
    if (m_request_open_homebrew.exchange(false)) {
        // Homebrew opening is currently disabled from the user-facing flow.
    }
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
    if (m_quick_settings_reset_pending.exchange(0) != 0) {
        reset_quick_settings_presets();
    }
    if (m_quick_layers_reset_pending.exchange(0) != 0) {
        reset_quick_layer_presets();
    }
    int quick_settings_slot = m_quick_settings_save_request_pending.exchange(-1);
    if (quick_settings_slot >= 0 && quick_settings_slot < (int)m_quick_settings_presets.size() && !m_quick_preset_dialog_open) {
        m_quick_preset_dialog_open = true;
        m_pending_quick_preset_kind = 0;
        m_pending_quick_preset_slot = quick_settings_slot;
        m_code_panel_quick_name_mode = true;
        m_code_input_buf = m_quick_settings_presets[quick_settings_slot].name;
        m_code_panel_pose = m_quick_panel_pose;
        m_active_sub_panel = k_panel_code;
        m_code_panel_dirty = true;
        set_status("Type a settings preset name, then press Save.");
    }
    int quick_layers_slot = m_quick_layers_save_request_pending.exchange(-1);
    if (quick_layers_slot >= 0 && quick_layers_slot < (int)m_quick_layer_presets.size() && !m_quick_preset_dialog_open) {
        m_quick_preset_dialog_open = true;
        m_pending_quick_preset_kind = 1;
        m_pending_quick_preset_slot = quick_layers_slot;
        m_code_panel_quick_name_mode = true;
        m_code_input_buf = m_quick_layer_presets[quick_layers_slot].name;
        m_code_panel_pose = m_quick_panel_pose;
        m_active_sub_panel = k_panel_code;
        m_code_panel_dirty = true;
        set_status("Type a layer preset name, then press Save.");
    }
    const int named_save_kind = m_quick_named_save_kind_pending.exchange(-1);
    const int named_save_slot = m_quick_named_save_slot_pending.exchange(-1);
    if (named_save_kind >= 0 && named_save_slot >= 0) {
        std::string entered_name;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            entered_name = m_quick_named_save_name;
            m_quick_named_save_name.clear();
        }
        m_quick_preset_dialog_open = false;
        m_pending_quick_preset_kind = -1;
        m_pending_quick_preset_slot = -1;
        m_code_panel_quick_name_mode = false;
        if (named_save_kind == 0 && named_save_slot < (int)m_quick_settings_presets.size()) {
            auto& preset = m_quick_settings_presets[named_save_slot];
            preset.name = sanitize_preset_name(
                entered_name,
                preset.name.empty() ? ("Settings " + std::to_string(named_save_slot + 1)) : preset.name);
            preset.canvas_x = m_canvas_x;
            preset.canvas_y = m_canvas_y;
            preset.canvas_az = m_canvas_az;
            preset.canvas_el = m_canvas_el;
            preset.canvas_scale = m_canvas_scale;
            preset.near_depth = 1.0f;
            preset.far_depth = 1.0f;
            preset.quad_width = m_config.layers.empty() ? 2.56f : m_config.layers[0].quad_width_meters;
            preset.copy_count = current_base_copy_count(m_config, m_layer_order);
            preset.immersive_beta_enabled = m_vr_state.immersive_beta_enabled;
            preset.upscale_mode = m_vr_state.upscale_mode;
            preset.ambilight = m_vr_state.ambilight;
            preset.passthrough = m_vr_state.shadows;
            preset.depth_mode = m_vr_state.depth_mode;
            preset.layers_3d = m_vr_state.layers_3d;
            preset.gamma = m_vr_state.gamma;
            preset.contrast = m_vr_state.contrast;
            preset.saturation = m_vr_state.saturation;
            preset.brightness = m_vr_state.brightness;
            preset.environment_sphere_mode = m_vr_state.environment_sphere_mode;
            if (!m_config.layers.empty()) {
                float near_depth = m_config.layers[0].depth_meters;
                float far_depth = m_config.layers[0].depth_meters;
                for (const auto& layer : m_config.layers) {
                    near_depth = std::min(near_depth, layer.depth_meters);
                    far_depth = std::max(far_depth, layer.depth_meters);
                }
                preset.near_depth = near_depth;
                preset.far_depth = far_depth;
            }
            const std::string root_dir = get_settings_dir();
            if (!root_dir.empty()) {
                mkdir(root_dir.c_str(), 0755);
                write_quick_settings_presets_file(quick_settings_presets_path(root_dir), m_quick_settings_presets);
            }
            set_status("Saved quick settings preset: " + preset.name);
            m_quick_panel_dirty = true;
        } else if (named_save_kind == 1 && named_save_slot < (int)m_quick_layer_presets.size()) {
            auto& preset = m_quick_layer_presets[named_save_slot];
            preset.name = sanitize_preset_name(
                entered_name,
                preset.name.empty() ? ("Layers " + std::to_string(named_save_slot + 1)) : preset.name);
            preset.ordered_ids.clear();
            preset.enabled.clear();
            preset.ambilight.clear();
            preset.depths.clear();
            for (int display_idx = 0; display_idx < (int)m_layer_order.size(); ++display_idx) {
                const int orig = m_layer_order[display_idx];
                if (orig < 0 || orig >= (int)m_config.layers.size()) continue;
                preset.ordered_ids.push_back(m_config.layers[orig].id);
                preset.enabled.push_back(orig < (int)m_layer_enabled.size() ? m_layer_enabled[orig] : true);
                preset.ambilight.push_back(orig < (int)m_layer_ambilight.size() ? m_layer_ambilight[orig] : true);
                preset.depths.push_back(m_config.layers[orig].depth_meters);
            }
            const std::string root_dir = get_settings_dir();
            if (!root_dir.empty()) {
                mkdir(root_dir.c_str(), 0755);
                const std::string system_dir = system_settings_dir(root_dir, m_current_backend_kind);
                mkdir(system_dir.c_str(), 0755);
                const std::string dir = quick_layers_presets_dir(root_dir, m_current_backend_kind);
                mkdir(dir.c_str(), 0755);
                write_quick_layer_presets_file(
                    quick_layer_presets_path(
                        root_dir, m_current_backend_kind,
                        quick_layer_signature(m_current_backend_kind, m_layer_filter_mode, m_config)),
                    m_quick_layer_presets);
            }
            set_status("Saved quick layer preset: " + preset.name);
            m_quick_panel_dirty = true;
        }
    }
    // Settings I/O actions from settings panel
    int action = m_settings_action_pending.exchange(0);
    switch (action) {
        case 1: save_settings(true);  m_settings_panel_dirty = true; break; // save game
        case 2: save_settings(false); m_settings_panel_dirty = true; break; // save global
        case 3: load_settings(true);  visual_change = true; break; // load game
        case 4: load_settings(false); visual_change = true; break; // load global
        case 5: reset_settings();     visual_change = true; break; // reset
        case 6: begin_lightgun_calibration(); break;
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
                presentation::ensure_layer_runtime_state_matches_config(
                    m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
                sync_layer_capture_mask();
                refresh_quick_layer_presets();
            }
        } else {
            ok = vr_state_decode(code, decoded, &m_config, &m_layer_order, &m_layer_enabled, &m_layer_ambilight);
        }
        if (ok) {
            const float prev_vr_scale = m_vr_state.vr_resolution_scale;
            m_vr_state = decoded;
            m_vr_state.vr_resolution_scale = snap_vr_resolution_scale(m_vr_state.vr_resolution_scale);
            if (std::abs(prev_vr_scale - m_vr_state.vr_resolution_scale) > 0.001f) destroy_swapchains();
            if (m_on_vr_state_changed) m_on_vr_state_changed(m_vr_state.audio_spatial_mode);
            sync_layer_capture_mask();
            m_saved_layer_mode_state.valid = is_snes_filter_capable_config(m_config);
            m_saved_layer_mode_state.mode = m_layer_filter_mode;
            m_saved_layer_mode_state.config = m_config;
            m_saved_layer_mode_state.order = m_layer_order;
            m_saved_layer_mode_state.enabled = m_layer_enabled;
            m_saved_layer_mode_state.ambilight = m_layer_ambilight;
    m_saved_layer_mode_state.side_color = m_layer_side_color;
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
    if (m_autoload_latest_save_pending.exchange(false)) {
        std::string loaded_name;
        std::string err;
        bool found_any = false;
        if (try_load_latest_state(loaded_name, err, found_any)) {
            const std::string display = (loaded_name == k_autosave_file_name) ? "autosave" : loaded_name;
            set_status("Loaded " + display + ".");
            visual_change = true;
        } else if (found_any) {
            set_status("Autoload failed: " + err);
        }
    }
    maybe_run_autosave();
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
    // Screen-lock: compute head yaw relative to screen and head pitch, store atomically
    if (m_vr_state.audio_screen_lock && m_impl) {
        const XrPosef& hmd = m_impl->last_hmd_pose;
        const float qx = hmd.orientation.x, qy = hmd.orientation.y,
                    qz = hmd.orientation.z, qw = hmd.orientation.w;
        const float fwd_x =  2.f*(qx*qz - qw*qy);
        const float fwd_y =  2.f*(qy*qz + qw*qx);
        const float fwd_z = -(1.f - 2.f*(qx*qx + qy*qy));
        const float dx = m_panel_pose.position.x - hmd.position.x;
        const float dz = m_panel_pose.position.z - hmd.position.z;
        const float yaw   = atan2f(fwd_x*dz - fwd_z*dx, fwd_x*dx + fwd_z*dz);
        const float pitch = asinf(std::clamp(fwd_y, -1.f, 1.f));
        constexpr float kMaxYaw = 1.22f, kMaxPitch = 0.785f;
        g_audio_processor.screen_yaw.store(std::clamp(yaw/kMaxYaw,   -1.f, 1.f), std::memory_order_relaxed);
        g_audio_processor.head_pitch.store(std::clamp(pitch/kMaxPitch,-1.f, 1.f), std::memory_order_relaxed);
    } else {
        g_audio_processor.screen_yaw.store(0.f, std::memory_order_relaxed);
        g_audio_processor.head_pitch.store(0.f, std::memory_order_relaxed);
    }
    if (m_vr_state.audio_spatial_mode == 3) {
        const float rms = g_audio_processor.bass_rms.load(std::memory_order_relaxed);
        if (rms > 0.05f) {
            fire_haptic(false, rms, 16);
            fire_haptic(true,  rms, 16);
        }
    }
    flush_pending_haptics();
}

// ============================================================
// draw_unified_menu — rail + tab framework for the new menu redesign, plus
// the row primitives (Step 3 of the Panel UI Migration plan) that every tab's
// real content will eventually be built from: Toggle, Slider, Cycle (shown
// as a pill list, not blind-cycled), Button, Danger — each with a ★ favorite
// toggle for free. Rows are declared once in kRows[] (mirrors the mockup's
// DATA/ROW_INDEX split) and rendered by the same draw_row() whether they're
// showing in their normal tab/group or pulled into the Favorites tab.
//
// Only a representative row or two per group is wired up so far — most
// groups still show the old placeholder text. Value/favorite state is
// session-only (static locals) until real settings move in; the one
// exception is "Show Old Menu", which is wired straight to the real
// debug_show_new_ui flag so it's a genuine replacement for the B+Y chord.
// ============================================================
namespace {
struct UnifiedMenuGroup { const char* name; };
struct UnifiedMenuTab { const char* name; std::initializer_list<UnifiedMenuGroup> groups; };

const UnifiedMenuTab kUnifiedMenuTabs[] = {
    {"Library",  {{"Browse & Launch"}}},
    {"Saves", {{"Slots"}}},
    {"Layers",   {{"Stack"}, {"Camera Position"}, {"Depth Effects"}, {"Composition Mode"}, {"Filter"}}},
    {"Visuals",  {{"Color Grading"}, {"Performance"}, {"Frame Skip"}, {"Display"}, {"Ambilight"}, {"Environment"}}},
    {"Audio",    {{"Spatial Audio"}, {"Music"}, {"Channels"}}},
    // Lightgun lives under Experimental > Motion Controls only -- it is one of
    // the motion features, and having it in two places meant two rows that
    // looked independent but edited the same state.
    {"Controls", {{"Controller Map"}, {"Haptics"}}},
    {"Interface",{{"Placement"}, {"Background"}, {"Theme"}}},
    {"Experimental", {{"Motion Controls"}, {"Presentation"}, {"Neo Geo"}, {"PSX"}}},
    // Help and Credits sit between Experimental and System so they remain
    // directly reachable from the rail without being buried under admin
    // settings. Help's groups must match the [Section] names in
    // assets/help.txt — the text itself lives there, not here.
    {"Help", {{"Getting Started"}, {"Controls"}, {"Layers & Depth"},
              {"Menu Tips"}, {"Saves & Settings"}, {"Troubleshooting"}}},
    {"Credits", {{"Credits"}}},
    {"System",   {{"Presets"}, {"Config Files"}, {"Danger Zone"}, {"Developer Preview"}, {"Exit"}}},
};

enum class RowKind { Toggle, Slider, Cycle, Button, Danger };

struct RowDef {
    const char* tab;
    const char* group;
    const char* label;
    RowKind kind;
    std::vector<const char*> cycle_opts{}; // Cycle only
    float slider_min = 0.0f, slider_max = 1.0f; // Slider only
    const char* danger_meta = "";               // Danger only
};

const RowDef kRows[] = {
    {"Visuals", "Performance", "VR Res Scale",     RowKind::Slider, {}, 0.25f, 4.0f},
    // Auto Frame Skip moved to its own dedicated "Frame Skip" group
    // (draw_frame_skip_group()) since it's per-core now, not one bool.
    // Color Grading: real VrState fields (gamma/contrast/saturation/brightness),
    // same clamp ranges as the old Settings panel's adjust_setting() cases 8-11.
    {"Visuals", "Color Grading", "Gamma",          RowKind::Slider, {}, 0.5f, 2.0f},
    {"Visuals", "Color Grading", "Contrast",       RowKind::Slider, {}, 0.5f, 2.0f},
    {"Visuals", "Color Grading", "Saturation",     RowKind::Slider, {}, 0.0f, 2.0f},
    {"Visuals", "Color Grading", "Brightness",     RowKind::Slider, {}, 0.5f, 2.0f},
    // Real VrState::tilt_x/tilt_y — radians, clamped to ±0.35 wherever they're
    // applied (see the clamp right after menu input handling below).
    {"Visuals", "Color Grading", "Tilt X",         RowKind::Slider, {}, -0.35f, 0.35f},
    {"Visuals", "Color Grading", "Tilt Y",         RowKind::Slider, {}, -0.35f, 0.35f},
    // Real VrState::perspective_comp/parallax_ratio — previously only reachable
    // via the old Settings panel's adjust_setting() cases (val_bufs[5]/[7]).
    {"Layers", "Depth Effects", "Perspective Compensation", RowKind::Toggle},
    {"Experimental", "PSX", "PSX Renderer",  RowKind::Cycle,
     {"Zero-Copy", "Readback", "Software"}},
    {"Layers", "Depth Effects", "Parallax Peek",   RowKind::Cycle,
        {"Off", "1:0.005", "1:0.05", "1:0.1", "1:0.25", "1:0.5", "1:1"}},
    // Left Side/Right Side removed as Position choices: that placement is now
    // always-on during gameplay (both sides simultaneously, mirroring the
    // full menu) rather than a single on-demand-menu placement choice — see
    // the automatic side-panel block in render_frame().
    {"Interface", "Placement", "Position",         RowKind::Cycle,
        {"Follow Left Hand", "Follow Right Hand", "Follow Headset"}},
    {"Interface", "Placement", "Transparency",     RowKind::Cycle,
        {"Automatic", "25%", "50%", "100%"}},
    // Real VrState::audio_spatial_mode (0-3), same order as the field's own comment.
    {"Audio", "Spatial Audio", "Spatial Audio",    RowKind::Cycle,
        {"Off", "Wide", "Spatial EQ", "Spatial EQ + Haptics"}},
    // Real VrState::audio_screen_lock — previously old Settings panel row 17.
    {"Audio", "Spatial Audio", "Audio Direction Locked to Screen", RowKind::Toggle},
    // Real VrState::bgm_enabled, driving the actual Kotlin-side menu-music
    // MediaPlayer (QuestVrActivity.kt's bgmEnable()/bgmDisable()) — didn't
    // exist as a user-facing control anywhere before (music always played).
    {"Audio", "Music", "Background Music",         RowKind::Toggle},
    // Real VrState::bgm_volume — menu-music volume, independent of the
    // emulator/ROM audio volume. Applied live to Kotlin's MediaPlayer via
    // OpenXrShell::call_activity_float("bgmSetVolume", ...).
    {"Audio", "Music", "Music Volume",             RowKind::Slider, {}, 0.0f, 1.0f},
    // Real VrState::ambilight_placement/ambilight — previously old Settings
    // panel row 2. Matches that row's exact behaviour: picking any placement
    // also force-enables ambilight (there's no separate global on/off here —
    // per-layer Ambilight toggles, Layers > Stack, are what actually
    // contribute or not).
    {"Visuals", "Ambilight", "Ambilight Placement", RowKind::Cycle,
        {"Screen", "Floor", "Ceiling", "All"}},
    // Real VrState::shadows (internal name; user-facing meaning is Passthrough
    // camera mode) — previously old Settings panel row 4.
    {"Visuals", "Environment", "Passthrough",      RowKind::Toggle},
    // Real VrState::immersive_beta_enabled — previously old Settings panel row 0.
    {"Visuals", "Display", "Curve Screen",         RowKind::Toggle},
    // Real VrState::depth_mode — previously old Settings panel row 14.
    {"Visuals", "Display", "Depth Mode",           RowKind::Cycle,
        {"Off", "Layer", "BBox", "Pixel", "ZBuf", "Pixel FX"}},
    // Real VrState::real_geometry_boxes/silhouette_sides — previously old
    // Settings panel rows 19/20.
    {"Visuals", "Display", "3D Geometry",          RowKind::Toggle},
    {"Visuals", "Display", "Silhouette Sides",     RowKind::Toggle},
    // Real VrState::sprite_y_depth_spread — previously old Settings panel row 15.
    {"Visuals", "Display", "Y-Depth Spread",       RowKind::Slider, {}, 0.0f, 2.0f},
    // Refresh Hz (old Settings panel row 12) isn't a kRows row — it needs
    // m_impl->available_rates/m_desired_refresh_rate, which draw_row()'s
    // free-function signature doesn't carry — drawn inline in
    // draw_unified_menu()'s Visuals > Display dispatch instead.
    //
    // Experimental Rumble (old row 6) isn't a kRows row either, for the same
    // reason (m_experimental_rumble_enabled/m_on_experimental_rumble_changed
    // aren't VrState fields) — drawn inline in Controls > Haptics instead.
    // Real VrState::upscale_mode (UpscaleMode enum: Off/PixelArt/Fsr).
    // Real VrState::surface_mode -- lays the whole scene on a flat surface
    // instead of the normal upright canvas. Table is a cocktail cabinet you
    // look down into; Ceiling is the same thing overhead, looking up. Lives
    // under Experimental rather than Visuals > Display because it restages
    // the scene rather than adjusting how it is drawn.
    {"Experimental", "Presentation", "Surface Mode", RowKind::Cycle,
        {"Off", "Table", "Ceiling"}},
    // Real VrState::rotate_screen -- for natively-portrait arcade boards
    // (e.g. 1941) that would otherwise render squished into the normal
    // landscape quad. Experimental because it is only implemented in the
    // flat-quad and immersive layer shaders: kBoxLayerVS has no
    // uRotateMode at all, so with 3D Geometry on the CPU still swaps the
    // quad to a portrait aspect while the box path lays its geometry and
    // samples its UVs unrotated, and the frame comes apart. Turning 3D
    // Geometry off routes through kLayerVS and rotates correctly.
    {"Experimental", "Presentation", "Rotate Screen", RowKind::Cycle,
        {"Off", "90", "180", "270"}},
    {"Layers", "Filter", "Upscale Mode",            RowKind::Cycle,
        {"Off", "Pixel Art", "FSR Sharpen"}},
    // Session-only generic MAME fallback. The OCCUPXY option is enabled by
    // draw_unified_menu only after the backend has observed 30 frames with
    // zero/one usable named layers. Grouped under Experimental > Neo Geo with
    // the rest of the Neo Geo/MAME layer-composition work.
    {"Experimental", "Neo Geo", "MAME Composition", RowKind::Cycle,
        {"Flat", "OCCUPXY"}},
    // Real VrState::bg_preset_index (-1=unset, 0-7=kBgSolidPresets, 8-15=
    // kBgGradientPresets). Names below match each preset's actual RGB values —
    // see kBgSolidPresets/kBgGradientPresets' own inline color-name comments —
    // instead of the "Solid N"/"Gradient N" placeholders this replaced.
    {"Interface", "Background", "Background",      RowKind::Cycle,
        {"Default",
         "Black", "Near-Black Gray", "Mid Gray", "Light Gray",
         "Navy", "Forest Green", "Deep Purple", "Warm Brown",
         "Day Sky", "Night Sky", "Sunset", "Dusk Purple",
         "Sky & Grass", "Overcast & Sand", "Space & Teal", "Twilight & Lava"}},
    {"System", "Developer Preview", "Show Old Menu", RowKind::Toggle},
};

std::string row_key(const RowDef& r) {
    return std::string(r.tab) + "|" + r.group + "|" + r.label;
}

// s_favorites is declared earlier in this file (near k_save_state_slot_count)
// so save_settings()/load_settings() can reach it too.
std::map<std::string, bool>      s_toggle_state;
std::map<std::string, float>     s_slider_state;
std::map<std::string, int>       s_cycle_state;

// draw_row() is a free function with no access to the shell, so the PSX
// Renderer row exchanges state through these: the shell publishes whether PSX
// is the live backend before drawing, and polls the dirty flag after.
bool g_psx_row_backend_active = false;
bool g_psx_row_path_changed = false;
// Same channel for the VR Res Scale slider: the eye swapchains have to be
// rebuilt for a new scale to mean anything, and only the shell can do that.
// Without this the slider moved its value and nothing else happened, which is
// why it read as a dead control.
bool g_vr_res_scale_changed = false;

void draw_favorite_star(const std::string& key) {
    // A bare SameLine() puts the star right after whatever content preceded it
    // on that row — since every row's content is a different width (a toggle
    // vs. a slider vs. a multi-pill cycle row), the star ends up at a different
    // x per row, reading as misaligned/messy. Anchor it to a fixed column at
    // the right edge of the window instead, same for every row.
    // Width sized from the actual button content (like the pill-row fix above)
    // rather than a guessed constant — a guessed 36px clipped the closing "]"
    // at this font size. Also reserves space for the vertical scrollbar when
    // the content is tall enough to need one, since GetWindowContentRegionMax()
    // doesn't already exclude it.
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const float star_w = ImGui::CalcTextSize("[*]").x + pad.x * 2.0f;
    const float scrollbar_w = ImGui::GetScrollMaxY() > 0.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - star_w - scrollbar_w - 8.0f);
    const bool active = s_favorites.count(key) != 0;
    // ASCII, not the Unicode star glyphs (U+2605/U+2606): ImGui's baked default
    // font only covers Basic Latin/Latin-1, so anything outside that range hits
    // the missing-glyph fallback and renders as "?" — matches what was reported.
    if (ImGui::SmallButton(active ? "[*]" : "[ ]")) {
        if (active) s_favorites.erase(key); else s_favorites.insert(key);
    }
}

// debug_show_new_ui: pointer to OpenXrShell::Impl::debug_show_new_ui — passed
// in, rather than reached via a shell reference, since Impl is private to
// OpenXrShell and this is a free function.
// vs: pointer to the real OpenXrShell::m_vr_state — VrState's fields are
// public (a plain settings struct), so this free function can bind directly
// to real settings (Gamma/Contrast/Saturation/Brightness/3D Shelf/Spatial
// Audio below) by label match, same pattern as debug_show_new_ui. Rows not
// special-cased here still use placeholder session state until they're
// ported the same way.
void draw_row(bool* debug_show_new_ui, VrState* vs, const RowDef& r, bool show_provenance,
              int* mame_composition_mode = nullptr, bool mame_occupancy_eligible = false,
              bool* bgm_toggle_changed = nullptr, bool* bgm_volume_changed = nullptr) {
    const std::string key = row_key(r);
    ImGui::PushID(key.c_str());

    switch (r.kind) {
        case RowKind::Toggle: {
            if (strcmp(r.label, "Show Old Menu") == 0 && debug_show_new_ui) {
                bool show_old = !*debug_show_new_ui;
                // "##Show Old Menu" keeps the ImGui widget ID stable (matches
                // row_key()/s_favorites, which key off r.label, not this display
                // string) while the visible text also names the same B+Y chord
                // that toggles this from anywhere, without opening the menu.
                if (ImGui::Checkbox("Show Old Menu (press B and Y simultaneously)##Show Old Menu", &show_old))
                    *debug_show_new_ui = !show_old;
            } else if (strcmp(r.label, "Background Music") == 0 && vs) {
                // bgm_toggle_changed tells draw_unified_menu() to call the real
                // Kotlin-side bgmEnable()/bgmDisable() (MediaPlayer control, not
                // a VrState field Kotlin already reads) — see there.
                if (ImGui::Checkbox(r.label, &vs->bgm_enabled) && bgm_toggle_changed)
                    *bgm_toggle_changed = true;
            } else {
                bool* target = nullptr;
                if (vs) {
                    if (strcmp(r.label, "Perspective Compensation") == 0) target = &vs->perspective_comp;
                    else if (strcmp(r.label, "Audio Direction Locked to Screen") == 0) target = &vs->audio_screen_lock;
                    else if (strcmp(r.label, "Passthrough") == 0) target = &vs->shadows;
                    else if (strcmp(r.label, "Curve Screen") == 0) target = &vs->immersive_beta_enabled;
                    else if (strcmp(r.label, "3D Geometry") == 0) target = &vs->real_geometry_boxes;
                    else if (strcmp(r.label, "Silhouette Sides") == 0) target = &vs->silhouette_sides;
                }
                if (!target) target = &s_toggle_state[key];
                ImGui::Checkbox(r.label, target);
            }
            break;
        }
        case RowKind::Slider: {
            float* target = nullptr;
            if (vs) {
                if (strcmp(r.label, "Gamma") == 0) target = &vs->gamma;
                else if (strcmp(r.label, "Contrast") == 0) target = &vs->contrast;
                else if (strcmp(r.label, "Saturation") == 0) target = &vs->saturation;
                else if (strcmp(r.label, "Brightness") == 0) target = &vs->brightness;
                else if (strcmp(r.label, "VR Res Scale") == 0) target = &vs->vr_resolution_scale;
                else if (strcmp(r.label, "Y-Depth Spread") == 0) target = &vs->sprite_y_depth_spread;
                else if (strcmp(r.label, "Tilt X") == 0) target = &vs->tilt_x;
                else if (strcmp(r.label, "Tilt Y") == 0) target = &vs->tilt_y;
                else if (strcmp(r.label, "Music Volume") == 0) target = &vs->bgm_volume;
            }
            if (!target) {
                if (!s_slider_state.count(key)) s_slider_state[key] = (r.slider_min + r.slider_max) * 0.5f;
                target = &s_slider_state[key];
            }
            if (ImGui::SliderFloat(r.label, target, r.slider_min, r.slider_max)) {
                if (strcmp(r.label, "Music Volume") == 0 && bgm_volume_changed) {
                    *bgm_volume_changed = true;
                } else if (strcmp(r.label, "VR Res Scale") == 0) {
                    g_vr_res_scale_changed = true;
                }
            }
            break;
        }
        case RowKind::Cycle: {
            ImGui::Text("%s", r.label);
            // ImGui::Selectable's size.x=0 means "stretch to fill the rest of the
            // line", not "size to text" — with several Selectables chained via
            // SameLine() on one row, every one after the first claims the entire
            // remaining line width as its hitbox, so their invisible bounding
            // boxes overlap and clicks land on the wrong option (or the first
            // option's oversized box eats everything after it). This is what made
            // multi-choice pill rows unusable. Fix: give each pill an explicit
            // width sized to its own text (plus the frame padding ImGui already
            // adds around it) instead of leaving it open-ended.
            const ImVec2 pad = ImGui::GetStyle().FramePadding;
            auto pill_width = [&](const char* opt) {
                return ImGui::CalcTextSize(opt).x + pad.x * 2.0f;
            };
            // Long option lists (Background's 17 presets, Side Color's 6, etc.)
            // used to run off the right edge and under the favorite star, since
            // every pill was chained with an unconditional SameLine(). Standard
            // ImGui "wrap if it doesn't fit" idiom instead: after drawing a pill,
            // only SameLine() the next one if it would still land before the
            // right edge (with room reserved for the star column) — otherwise it
            // drops to a new line, same as text word-wrap.
            const float star_reserve = ImGui::CalcTextSize("[*]").x + pad.x * 2.0f + 16.0f;
            const float wrap_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - star_reserve;
            auto wrap_same_line = [&](size_t i, size_t count) {
                if (i + 1 >= count) return;
                const float next_w = pill_width(r.cycle_opts[i + 1]);
                const float next_x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w;
                if (next_x2 < wrap_x2) ImGui::SameLine();
            };
            if (vs && strcmp(r.label, "Depth Mode") == 0) {
                const int cur = (int)vs->depth_mode;
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0)))
                        vs->depth_mode = (DepthMode)i;
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            if (vs && strcmp(r.label, "Surface Mode") == 0) {
                const int cur = std::clamp(vs->surface_mode, 0, 2);
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0)))
                        vs->surface_mode = (int)i;
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            if (vs && strcmp(r.label, "Rotate Screen") == 0) {
                const int cur = vs->rotate_screen & 3;
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0)))
                        vs->rotate_screen = (int)i;
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            if (vs && strcmp(r.label, "PSX Renderer") == 0) {
                const bool is_psx = g_psx_row_backend_active;
                const int cur = std::clamp(vs->psx_render_path, 0, 2);
                ImGui::BeginDisabled(!is_psx);
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0,
                                          ImVec2(pill_width(r.cycle_opts[i]), 0)) && is_psx) {
                        vs->psx_render_path = (int)i;
                        g_psx_row_path_changed = true;
                    }
                    wrap_same_line(i, r.cycle_opts.size());
                }
                ImGui::EndDisabled();
                if (is_psx && cur == 2)
                    ImGui::TextDisabled("  Software applies on the next ROM load");
                else if (is_psx)
                    ImGui::TextDisabled("  Switching to/from Software needs a ROM reload");
                break;
            }
            if (vs && strcmp(r.label, "Ambilight Placement") == 0) {
                // Matches the old Settings panel row's exact behaviour:
                // picking any placement also force-enables ambilight — there's
                // no separate global on/off here (Layers > Stack's per-layer
                // Ambilight toggles are what actually contribute or not).
                const int cur = (int)vs->ambilight_placement;
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0))) {
                        vs->ambilight_placement = (AmbilightPlacement)i;
                        vs->ambilight = true;
                    }
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            if (strcmp(r.label, "MAME Composition") == 0) {
                const bool is_mame = mame_composition_mode != nullptr;
                const int cur = is_mame ? std::clamp(*mame_composition_mode, 0, 1) : 0;
                ImGui::BeginDisabled(!is_mame);
                if (ImGui::Selectable("Flat", cur == 0, 0,
                                      ImVec2(pill_width("Flat"), 0)) && is_mame)
                    *mame_composition_mode = 0;
                ImGui::SameLine();
                ImGui::BeginDisabled(!is_mame || !mame_occupancy_eligible);
                if (ImGui::Selectable("OCCUPXY", cur == 1, 0,
                                      ImVec2(pill_width("OCCUPXY"), 0)) && is_mame && mame_occupancy_eligible)
                    *mame_composition_mode = 1;
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (is_mame && !mame_occupancy_eligible)
                    ImGui::TextDisabled("  Available after 30 stable flat-layer frames");
                break;
            }
            // "Background" needs a -1..15 <-> 0..16 offset (bg_preset_index's own
            // "-1 = unset" convention) that doesn't fit the plain int* pattern below.
            if (vs && strcmp(r.label, "Background") == 0) {
                const int cur = vs->bg_preset_index + 1;
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0))) vs->bg_preset_index = (int)i - 1;
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            if (vs && strcmp(r.label, "Parallax Peek") == 0) {
                // k_parallax_steps/parallax_label (near the top of this file) are
                // the exact same table the old Settings panel's cycle used —
                // find the closest step to the stored float rather than assuming
                // an exact match, since it can also be set by a share-code/.ini.
                int cur = 0;
                float best_delta = std::fabs(vs->parallax_ratio - k_parallax_steps[0]);
                for (int s = 1; s < k_parallax_step_count; ++s) {
                    const float delta = std::fabs(vs->parallax_ratio - k_parallax_steps[s]);
                    if (delta < best_delta) { best_delta = delta; cur = s; }
                }
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0)))
                        vs->parallax_ratio = k_parallax_steps[i];
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            if (vs && strcmp(r.label, "Upscale Mode") == 0) {
                const int cur = (int)vs->upscale_mode;
                for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                    const bool selected = ((int)i == cur);
                    if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0))) vs->upscale_mode = (UpscaleMode)i;
                    wrap_same_line(i, r.cycle_opts.size());
                }
                break;
            }
            int* sel = nullptr;
            if (vs && strcmp(r.label, "Spatial Audio") == 0) sel = &vs->audio_spatial_mode;
            else if (vs && strcmp(r.label, "Position") == 0) sel = &vs->menu_position_mode;
            else if (vs && strcmp(r.label, "Transparency") == 0) sel = &vs->menu_transparency_mode;
            if (!sel) sel = &s_cycle_state[key]; // defaults to 0 (first option)
            for (size_t i = 0; i < r.cycle_opts.size(); ++i) {
                bool selected = ((int)i == *sel);
                if (ImGui::Selectable(r.cycle_opts[i], selected, 0, ImVec2(pill_width(r.cycle_opts[i]), 0))) *sel = (int)i;
                wrap_same_line(i, r.cycle_opts.size());
            }
            break;
        }
        case RowKind::Button:
            ImGui::Button(r.label);
            break;
        case RowKind::Danger:
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", r.label);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", r.danger_meta);
            ImGui::SameLine();
            ImGui::Button("Wipe"); // TODO: real wipe action + live count/size once file scanning lands
            break;
    }

    draw_favorite_star(key);
    if (show_provenance) ImGui::TextDisabled("  %s > %s", r.tab, r.group);
    ImGui::PopID();
}

// Per-layer control templates (Layers > Stack, below the depth track) — real
// values, not placeholders: Visibility -> m_layer_enabled, Ambilight ->
// m_layer_ambilight, Side Color -> m_layer_side_color (0=Original..5=Blue, 6=Darker,
// matching gles_renderer.cpp's side_color_mode_to_rgb exactly). Favorited
// under key "LAYER::{game}::{layer}|{control}" so a stale favorite from a
// different game (or a since-renamed/removed layer) can be told apart from
// a live one — see draw_one_layer_control's `disabled` handling and its use
// from the Favorites tab in draw_unified_menu().
struct LayerControlDef { const char* label; RowKind kind; std::vector<const char*> cycle_opts{}; };
const LayerControlDef kLayerControls[] = {
    {"Visibility", RowKind::Toggle},
    {"Ambilight",  RowKind::Toggle},
    {"Side Color", RowKind::Cycle, {"Original", "Black", "White", "Red", "Green", "Blue", "Darker"}},
};

std::string layer_row_key(const std::string& game, const std::string& layer_name, const char* label) {
    return "LAYER::" + game + "::" + layer_name + "|" + label;
}

// orig < 0 (or disabled == true) means "this layer/game isn't currently
// loaded" — the control still renders (so its favorite can be un-starred)
// but is inert and shows why.
void draw_one_layer_control(const std::string& game, const std::string& layer_name, const LayerControlDef& def,
                             int orig, std::vector<bool>* enabled_vec, std::vector<bool>* ambilight_vec,
                             std::vector<int>* side_color_vec, bool disabled) {
    disabled = disabled || orig < 0;
    const std::string key = layer_row_key(game, layer_name, def.label);
    ImGui::PushID(key.c_str());
    ImGui::BeginDisabled(disabled);

    if (def.kind == RowKind::Toggle) {
        std::vector<bool>* vec = (strcmp(def.label, "Visibility") == 0) ? enabled_vec : ambilight_vec;
        bool val = (!disabled && vec && orig < (int)vec->size()) ? (bool)(*vec)[orig] : false;
        if (ImGui::Checkbox(def.label, &val) && !disabled && vec && orig < (int)vec->size()) (*vec)[orig] = val;
    } else if (def.kind == RowKind::Cycle) {
        ImGui::Text("%s", def.label);
        int cur = (!disabled && side_color_vec && orig < (int)side_color_vec->size()) ? (*side_color_vec)[orig] : 0;
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        auto w_of = [&](const char* opt) { return ImGui::CalcTextSize(opt).x + pad.x * 2.0f; };
        // Same wrap-if-it-doesn't-fit idiom as draw_row()'s RowKind::Cycle case —
        // 6 options plus this row's own favorite star ran off the right edge.
        const float star_reserve = w_of("[*]") + 16.0f;
        const float wrap_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - star_reserve;
        for (size_t i = 0; i < def.cycle_opts.size(); ++i) {
            const bool sel = ((int)i == cur);
            // See the RowKind::Cycle case in draw_row() for why an explicit
            // per-pill width (not size.x=0) is required here.
            if (ImGui::Selectable(def.cycle_opts[i], sel, 0, ImVec2(w_of(def.cycle_opts[i]), 0)) &&
                !disabled && side_color_vec && orig < (int)side_color_vec->size()) {
                (*side_color_vec)[orig] = (int)i;
            }
            if (i + 1 < def.cycle_opts.size()) {
                const float next_x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w_of(def.cycle_opts[i + 1]);
                if (next_x2 < wrap_x2) ImGui::SameLine();
            }
        }
    }

    ImGui::EndDisabled();
    draw_favorite_star(key);
    if (disabled) ImGui::TextDisabled("  Only available while playing %s > %s", game.c_str(), layer_name.c_str());
    ImGui::PopID();
}

bool draw_group_rows(bool* debug_show_new_ui, VrState* vs, const char* tab, const char* group,
                     int* mame_composition_mode = nullptr, bool mame_occupancy_eligible = false,
                     bool* bgm_toggle_changed = nullptr, bool* bgm_volume_changed = nullptr) {
    bool drew_any = false;
    for (const RowDef& r : kRows) {
        if (strcmp(r.tab, tab) == 0 && strcmp(r.group, group) == 0) {
            draw_row(debug_show_new_ui, vs, r, /*show_provenance=*/false,
                     mame_composition_mode, mame_occupancy_eligible, bgm_toggle_changed,
                     bgm_volume_changed);
            drew_any = true;
        }
    }
    return drew_any;
}
} // namespace

// ============================================================
// draw_depth_arrangement_widget — one draggable handle per real, currently-
// loaded slot (m_layer_order.size(), named via m_layer_names). A handle
// can't cross a neighbor (plus a thickness margin, unless Thickness Overlap
// is on); Canvas Depth rescales the displayed meters (informational only —
// see apply_slot_fraction_layer_depths, which places layers within the live
// per-frame envelope, not this canvas value); Evenly Distribute resets to
// default even spacing.
//
// Live once slot_fractions_active (m_layer_slot_fraction sized to match
// m_layer_order): each fraction is a RELATIVE position within whatever the
// live near/far depth envelope is that frame, applied on top of it every
// frame — not a frozen absolute depth. This intentionally leaves the legacy
// per-layer depth field's known-buggy writers (reorder's redistribute-evenly,
// the dashboard's Near/Far Distance buttons) untouched but inert while the
// widget is active; replacing those call sites outright is a separate,
// larger follow-up.
// ============================================================
void OpenXrShell::draw_depth_arrangement_widget() {
    const int total = (int)m_layer_order.size();
    if (total <= 0) {
        ImGui::TextDisabled("No layers loaded - open a ROM to see its stack here.");
        return;
    }

    if ((int)m_layer_slot_fraction.size() != total) {
        m_layer_slot_fraction.assign(total, 0.0f);
        // A fraction is a RELATIVE position (0=nearest..1=farthest) within
        // whatever the live near/far depth envelope is this frame (see
        // apply_slot_fraction_layer_depths) -- not an absolute canvas-metres
        // depth. Seed each slot's fraction from where its layer's real current
        // depth_meters (m_cached_layer_frames) already sits within that same
        // envelope, so opening the widget reproduces the current on-screen
        // arrangement exactly, and subsequent frames' envelope can keep
        // moving live (z-buffer backends) while the user's relative
        // arrangement stays put.
        float near_d = 0.0f, far_d = 0.0f;
        bool have_range = false;
        for (int i = 0; i < total; ++i) {
            const int orig = m_layer_order[i];
            if (orig < 0 || orig >= (int)m_cached_layer_frames.size()) continue;
            const float d = m_cached_layer_frames[orig].depth_meters;
            if (!have_range) { near_d = far_d = d; have_range = true; }
            else { near_d = std::min(near_d, d); far_d = std::max(far_d, d); }
        }
        const float span = far_d - near_d;
        for (int i = 0; i < total; ++i) {
            const int orig = m_layer_order[i];
            if (have_range && span > 1e-4f && orig >= 0 && orig < (int)m_cached_layer_frames.size()) {
                m_layer_slot_fraction[i] = std::clamp(
                    (m_cached_layer_frames[orig].depth_meters - near_d) / span, 0.0f, 1.0f);
            } else {
                m_layer_slot_fraction[i] = (total > 1) ? (float)i / (float)(total - 1) : 0.0f;
            }
        }
    }

    ImGui::Text("Canvas Depth");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##canvasdepth", &m_canvas_depth_meters_ui, 0.5f, 5.0f, "%.1fm");
    ImGui::SameLine();
    if (ImGui::Button("Evenly Distribute")) {
        for (int i = 0; i < total; ++i) m_layer_slot_fraction[i] = (float)(i + 1) / (float)(total + 1);
    }
    ImGui::Checkbox("Thickness Overlap", &m_thickness_overlap_ui);
    ImGui::TextDisabled("Drag a handle - it can't cross its neighbors.");

    struct Rgb { int r, g, b; };
    static const Rgb kColors[] = {
        {0, 229, 255}, {255, 47, 208}, {201, 139, 255}, {47, 214, 108}, {255, 184, 77}, {255, 107, 107},
    };

    std::vector<float> thick(total, 0.035f);
    for (int i = 0; i < total; ++i) {
        int orig = m_layer_order[i];
        if (orig >= 0 && orig < (int)m_config.layers.size()) {
            thick[i] = std::clamp(m_config.layers[orig].box_thickness_meters / std::max(0.1f, m_canvas_depth_meters_ui),
                                   0.01f, 0.12f);
        }
    }

    const float track_h = 56.0f;
    ImVec2 track_min = ImGui::GetCursorScreenPos();
    const float track_w = ImGui::GetContentRegionAvail().x;
    ImVec2 track_max(track_min.x + track_w, track_min.y + track_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(track_min, track_max, IM_COL32(30, 34, 44, 255), 4.0f);
    dl->AddRect(track_min, track_max, IM_COL32(0, 180, 200, 120), 4.0f);

    ImGui::PushID("depthbar");
    for (int i = 0; i < total; ++i) {
        const Rgb& c = kColors[i % (int)std::size(kColors)];
        const float frac = m_layer_slot_fraction[i];
        const float band_lo = std::max(0.0f, frac - thick[i]);
        const float band_hi = std::min(1.0f, frac + thick[i]);
        dl->AddRectFilled(ImVec2(track_min.x + band_lo * track_w, track_min.y),
                           ImVec2(track_min.x + band_hi * track_w, track_max.y),
                           IM_COL32(c.r, c.g, c.b, 55));
        const float hx = track_min.x + frac * track_w;
        dl->AddLine(ImVec2(hx, track_min.y), ImVec2(hx, track_max.y), IM_COL32(c.r, c.g, c.b, 255), 3.0f);

        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(hx - 8.0f, track_min.y));
        ImGui::InvisibleButton("handle", ImVec2(16.0f, track_h));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const float new_frac = (ImGui::GetIO().MousePos.x - track_min.x) / track_w;
            const float margin_lo = m_thickness_overlap_ui ? 0.0f : (i > 0 ? (thick[i] + thick[i - 1]) * 0.5f : 0.0f);
            const float margin_hi = m_thickness_overlap_ui ? 0.0f : (i < total - 1 ? (thick[i] + thick[i + 1]) * 0.5f : 0.0f);
            const float lower = (i == 0) ? 0.0f : m_layer_slot_fraction[i - 1] + margin_lo;
            const float upper = (i == total - 1) ? 1.0f : m_layer_slot_fraction[i + 1] - margin_hi;
            m_layer_slot_fraction[i] = std::clamp(new_frac, lower, upper);
        }
        ImGui::PopID();
    }
    ImGui::PopID();

    ImGui::Dummy(ImVec2(0, track_h + 6));
    ImGui::TextDisabled("Near");
    ImGui::SameLine(track_w - 30.0f);
    ImGui::TextDisabled("Far");
    ImGui::Separator();

    ImGui::TextDisabled("Reorder: Up/Down swaps which content sits in this depth slot -");
    ImGui::TextDisabled("the slot's own depth/thickness above stays put, only its content moves.");
    ImGui::PushID("reorder");
    for (int i = 0; i < total; ++i) {
        int orig = m_layer_order[i];
        // "(empty slot)" — a real, expected state (a depth slot with no content
        // assigned), not a font/rendering glitch. Was a bare "?" before, which
        // read as broken/missing text rather than an actual empty-slot state.
        const char* name = (orig >= 0 && orig < (int)m_layer_names.size()) ? m_layer_names[orig].c_str() : "(empty slot)";
        ImGui::PushID(i);
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Up")) std::swap(m_layer_order[i], m_layer_order[i - 1]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == total - 1);
        if (ImGui::SmallButton("Down")) std::swap(m_layer_order[i], m_layer_order[i + 1]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        // Percentage only, not meters — the fraction is relative to the live
        // per-frame depth envelope (apply_slot_fraction_layer_depths), which
        // isn't a fixed real-world distance, so a meters figure here would be
        // a misleading approximation rather than the actual render depth.
        ImGui::Text("%s - %.0f%% (near -> far)", name, m_layer_slot_fraction[i] * 100.0f);
        ImGui::PopID();
    }
    ImGui::PopID();
}

// ============================================================
// draw_layer_control_rows — one Visibility/Ambilight/Side Color group per
// real, currently-loaded layer, each favoritable and bound straight to the
// real engine arrays (m_layer_enabled/m_layer_ambilight/m_layer_side_color) —
// no placeholder state, unlike most of the rest of the menu so far.
// ============================================================
void OpenXrShell::draw_layer_control_rows() {
    ImGui::Separator();
    for (size_t slot = 0; slot < m_layer_order.size(); ++slot) {
        const int orig = m_layer_order[slot];
        if (orig < 0 || orig >= (int)m_layer_names.size()) continue;
        const std::string& name = m_layer_names[orig];
        // Each layer collapses its own Visibility/Ambilight/Side Color rows
        // (matching the mockup's per-group CollapsingHeader pattern already
        // used elsewhere in the menu) instead of always showing every layer's
        // controls flattened out — a long layer stack was an unscrollable wall
        // of rows otherwise. ImGui::PushID keeps each layer's open/closed state
        // independent even if two layers share the same name.
        ImGui::PushID(name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.08f, 0.24f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.45f, 0.12f, 0.34f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.9f, 1.0f));
        const bool open = ImGui::CollapsingHeader(name.c_str());
        ImGui::PopStyleColor(3);
        if (open) {
            for (const LayerControlDef& def : kLayerControls) {
                draw_one_layer_control(m_config.game, name, def, orig,
                                        &m_layer_enabled, &m_layer_ambilight, &m_layer_side_color,
                                        /*disabled=*/false);
            }
        }
        ImGui::PopID();
    }
}

// ============================================================
// draw_config_files_group — System > Config Files. save_settings()/
// load_settings() are private OpenXrShell members, so unlike everything
// above (free functions taking a VrState*/bool* pointer) this group is
// rendered directly by a member function that can call them — same pattern
// already used for the Layers > Stack special-case below.
// ============================================================
void OpenXrShell::draw_config_files_group() {
    if (ImGui::Button("Save Game Settings")) save_settings(/*game_scope=*/true);
    ImGui::SameLine();
    if (ImGui::Button("Load Game Settings")) load_settings(/*game_scope=*/true);
    if (ImGui::Button("Save Global Settings")) save_settings(/*game_scope=*/false);
    ImGui::SameLine();
    if (ImGui::Button("Load Global Settings")) load_settings(/*game_scope=*/false);
}

// ============================================================
// draw_theme_row — Interface > Theme. Binds to the REAL m_ui_theme
// (ui_theme.h's UiThemeId: Classic/PremiumRetroTech/Glass/Arcade) instead of
// a second, ImGui-only palette — keeps the new menu and the old Kotlin
// panels under one real theme choice. Persisted immediately on change via
// the same ui_theme_save() path the old Themes panel already uses (also
// covered by save_settings()/load_settings(), see draw_config_files_group).
// Known gap: unlike the old panel, doesn't push a JNI notification to
// refresh already-built Kotlin panel bitmaps immediately — they'll pick up
// the new theme next time they're rebuilt.
// ============================================================
void OpenXrShell::draw_theme_row() {
    ImGui::Text("UI Theme");
    const int current = (int)m_ui_theme;
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    for (int i = 0; i < kUiThemeCount; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool selected = (i == current);
        // See the RowKind::Cycle case in draw_row() for why an explicit
        // per-pill width (not size.x=0, which stretches and breaks hit-testing
        // on every option after the first) is required here.
        const char* name = ui_theme_name((UiThemeId)i);
        const float w = ImGui::CalcTextSize(name).x + pad.x * 2.0f;
        if (ImGui::Selectable(name, selected, 0, ImVec2(w, 0)) && i != current) {
            m_ui_theme = clamp_ui_theme(i);
            m_impl->imgui_bridge.apply_theme((int)m_ui_theme);
            const std::string theme_dir = get_settings_dir();
            if (!theme_dir.empty()) ui_theme_save(theme_dir + "/ui_theme.ini", (int)m_ui_theme);
        }
    }
}

// ============================================================
// draw_camera_position_group — Layers > Camera Position. Real X/Y pan +
// zoom, bound directly to m_canvas_x/m_canvas_y/m_canvas_scale (the same
// rigid world-space anchor game_canvas_anchor_pose() already uses) — this is
// the actual "camera" the earlier depth-vs-camera design discussion settled
// on, not a new concept.
// ============================================================
void OpenXrShell::draw_camera_position_group() {
    // Ranges follow the clamps the rest of the code already enforces on these
    // same fields: scale 0.25..6.0 (apply_preset / the pinch-zoom path), pan
    // +/-2m of practical travel inside the much looser k_dashboard_pos_max
    // safety clamp. Value formats match the old dashboard panel's readouts.
    ImGui::Text("Pan X");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##canvasx", &m_canvas_x, -2.0f, 2.0f, "%.2fm");
    ImGui::Text("Pan Y");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##canvasy", &m_canvas_y, -2.0f, 2.0f, "%.2fm");
    ImGui::Text("Zoom");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##canvasscale", &m_canvas_scale, 0.25f, 6.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        m_canvas_x = 0.0f;
        m_canvas_y = 0.0f;
        m_canvas_scale = 1.0f;
    }
}

// ============================================================
// draw_credits_group — Credits. Real data: reuses the exact same
// m_credit_entries/load_credits_entries()/open_credits_link() the old
// Credits panel used (parsed from assets/credits.txt, so edits there show up
// here too without any other code change) — not a re-typed copy.
// ============================================================
void OpenXrShell::draw_credits_group() {
    if (m_credit_entries.empty()) load_credits_entries();
    for (size_t i = 0; i < m_credit_entries.size(); ++i) {
        const CreditRow& row = m_credit_entries[i];
        ImGui::PushID((int)i);
        if (row.is_header) {
            if (i > 0) ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1.0f), "%s", row.name.c_str());
            ImGui::Separator();
        } else if (!row.url.empty()) {
            if (ImGui::Selectable(row.name.c_str())) open_credits_link((int)i);
            if (!row.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", row.detail.c_str());
            }
        } else {
            ImGui::TextWrapped("%s", row.name.c_str());
            if (!row.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", row.detail.c_str());
            }
        }
        ImGui::PopID();
    }
    if (m_credit_entries.empty()) ImGui::TextDisabled("No credits found (assets/credits.txt missing or empty).");
}

// ============================================================
// draw_library_rom_list — Library > Browse & Launch. A real searchable list
// over the SAME m_rom_browser the 3D shelf/flat-panel already use — not a
// second, parallel ROM-listing system. Replaces the plan's remaining "still
// Kotlin-backed flat browser" gap for this tab specifically (the 3D shelf
// itself was already confirmed pure C++ and untouched).
// ============================================================
void OpenXrShell::draw_library_rom_list() {
    // A live preview spins up a second temporary emulator backend; running
    // that alongside an already-active game corrupts/crashes the app. Gate
    // browsing behind closing (or saving+closing) the active game first.
    if (!m_current_rom_name.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Library can't be browsed while a game is open.");
        ImGui::Spacing();
        const ImVec2 close_pad(24.0f, 14.0f);
        const ImVec2 close_size(ImGui::CalcTextSize("Close ROM").x + close_pad.x * 2.0f, 44.0f);
        const ImVec2 save_close_size(
            ImGui::CalcTextSize("Save State and Close ROM").x + close_pad.x * 2.0f, 44.0f);
        if (ImGui::Button("Close ROM", close_size)) {
            close_current_rom(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save State and Close ROM", save_close_size)) {
            close_current_rom(0);
        }
        return;
    }
    // Preview generation is click-triggered (see the Selectable() handler
    // below), not hover-triggered — selecting a ROM row calls
    // start_library_live_preview() directly, which opens its own preview
    // session independent of the (now-removed) 3D Shelf view.
    static char s_filter[128] = "";
    // A plain Android AlertDialog (the earlier showRomSearchDialog() approach)
    // never actually composites into the immersive OpenXR scene — confirmed
    // on-device via logcat (it's created and shown successfully, JNI side,
    // but nothing appears in the headset). So instead of a system keyboard
    // dialog, Search toggles a real in-VR virtual keyboard drawn below,
    // laser+trigger clickable exactly like every other row in this menu.
    char btn_label[160];
    std::snprintf(btn_label, sizeof(btn_label), "Search: %s##romsearchbtn",
                  s_filter[0] ? s_filter : "(tap to type)");
    if (ImGui::Button(btn_label)) m_rom_search_keyboard_open = true;
    if (s_filter[0]) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) s_filter[0] = '\0';
    }
    if (m_rom_search_keyboard_open) {
        draw_rom_search_keyboard(s_filter, sizeof(s_filter));
    }

    std::string needle = s_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    // While the filter is empty, this is the normal current-folder listing
    // (dirs + files, navigable). As soon as text is typed, it switches to a
    // recursive search across the whole library — scanned once per root and
    // cached, then re-filtered in-memory every frame as the query changes,
    // rather than re-walking disk on every keystroke.
    static std::vector<qrd::RomEntry> s_search_corpus;
    static std::string s_search_corpus_root;
    const bool searching = !needle.empty();
    if (searching) {
        const std::string& root = m_rom_browser.root_dir();
        if (s_search_corpus_root != root || s_search_corpus.empty()) {
            s_search_corpus = m_rom_browser.scan_recursive(root);
            s_search_corpus_root = root;
        }
    }
    const std::vector<qrd::RomEntry>& entries = searching ? s_search_corpus : m_rom_browser.entries();

    std::vector<int> filtered;
    filtered.reserve(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (needle.empty()) { filtered.push_back(i); continue; }
        std::string name = entries[i].display_name.empty() ? entries[i].name : entries[i].display_name;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.find(needle) != std::string::npos) filtered.push_back(i);
    }
    ImGui::TextDisabled("%d %s%s", (int)filtered.size(), filtered.size() == 1 ? "entry" : "entries",
                         searching ? " (all folders)" : "");

    ImGui::BeginChild("##romlist", ImVec2(0, 560), true); // doubled per request, fits more rows at once
    bool any_rom_hovered_this_frame = false;
    // ImGuiListClipper over the FILTERED index list (not m_entries directly) —
    // the standard pattern for a clipped list whose visible count varies with
    // a search filter, so scroll math stays correct as the filter changes.
    ImGuiListClipper clipper;
    clipper.Begin((int)filtered.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int idx = filtered[row];
            const qrd::RomEntry& e = entries[idx];
            const std::string label = e.is_dir
                ? ("[DIR] " + (e.name.empty() ? std::string("?") : e.name))
                : (e.display_name.empty() ? e.name : e.display_name);
            ImGui::PushID(idx);
            const bool selected_row = !e.is_dir && m_library_preview_path == e.path;
            if (ImGui::Selectable(label.c_str(), selected_row)) {
                if (e.is_dir) {
                    fire_haptic(true, 0.3f, 40);
                    m_rom_browser.set_hovered_index(idx);
                    enter_folder_and_queue_caching();
                } else if (!e.path.empty()) {
                    // Per request: clicking a ROM no longer launches it
                    // straight away — it selects the row (immediately
                    // populating the info sidebar + starting the real live
                    // preview, no hover/dwell involved) and waits for the
                    // "PLAY" button in draw_rom_preview_sidebar() to confirm
                    // the actual launch.
                    m_rom_search_keyboard_open = false;
                    if (m_library_preview_path != e.path) {
                        // Switching the selected ROM must NOT tear down the whole
                        // preview session (stop_library_live_preview() would call
                        // end_rom_preview_session(), which unloads/reloads the
                        // shared preview backend) -- that round-trip is what made
                        // clicking a new ROM feel delayed. request_live() inside
                        // start_library_live_preview() already retargets the live
                        // job on the existing session; just clear the stale frame
                        // so the old ROM's image doesn't linger during the swap.
                        if (m_impl) m_impl->renderer.clear_library_preview_layers();
                        m_library_preview_has_frame = false;
                        m_library_preview_layer_count = 0;
                        m_library_preview_reveal_t = 0.0f;
                        m_library_preview_path = e.path;
                        m_library_preview_name = e.display_name.empty() ? e.name : e.display_name;
                        // Best-effort "system" — the ROM's parent folder name, since
                        // there's no cheap real backend-kind-from-path lookup to
                        // derive it from actual emulator core identity.
                        {
                            size_t slash = e.path.find_last_of("/\\");
                            std::string parent = (slash == std::string::npos) ? std::string() : e.path.substr(0, slash);
                            size_t slash2 = parent.find_last_of("/\\");
                            m_library_preview_system = (slash2 == std::string::npos) ? parent : parent.substr(slash2 + 1);
                        }
                        struct stat st{};
                        if (stat(e.path.c_str(), &st) == 0) {
                            double mb = (double)st.st_size / (1024.0 * 1024.0);
                            char buf[64];
                            snprintf(buf, sizeof(buf), "%.2f MB", mb);
                            m_library_preview_size_str = buf;
                        } else {
                            m_library_preview_size_str.clear();
                        }
                        auto ends_with_ci = [](const std::string& s, const char* ext) {
                            const size_t el = strlen(ext);
                            if (s.size() < el) return false;
                            const char* tail = s.c_str() + s.size() - el;
                            for (size_t i = 0; i < el; ++i)
                                if (std::tolower((unsigned char)tail[i]) != std::tolower((unsigned char)ext[i])) return false;
                            return true;
                        };
                        m_library_preview_is_archive = ends_with_ci(e.path, ".zip") || ends_with_ci(e.path, ".7z");
                        m_library_preview_uncompressed_size_str.clear();
                        fire_haptic(true, 0.3f, 40);
                        start_library_live_preview(e.path);
                    }
                }
            }
            if (!e.is_dir && ImGui::IsItemHovered()) {
                any_rom_hovered_this_frame = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    (void)any_rom_hovered_this_frame; // no longer clears the selection on unhover — see PLAY button
}

// ============================================================
// draw_rom_search_keyboard — real in-VR virtual keyboard for the Library
// search box, laser+trigger clickable like every other ImGui row. Replaces
// the old showRomSearchDialog() Android-AlertDialog approach, which is
// invisible while an immersive OpenXR session is running (confirmed via
// logcat: the dialog is created and shown successfully JNI-side, but the VR
// compositor never draws it). Each key press writes straight into the
// caller's filter buffer, so filtering updates live, key by key — no
// separate "submit" step.
// ============================================================
void OpenXrShell::draw_rom_search_keyboard(char* filter, size_t filter_size) {
    ImGui::Separator();
    // Real QWERTY stagger, centered as a block (not left-anchored — a
    // left-anchored stagger looks lopsided inside a wide panel). Row widths
    // in key-units: row0/row1 = 10, row2 = 9 (+0.5 indent), row3 = 7
    // (+1.5 indent) — indents measured from row1's left edge, same as a
    // real keyboard, so every row's block is 10 key-units wide overall.
    static const char* kRows[4] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM",
    };
    static const float kIndentKeys[4] = { 0.0f, 0.0f, 0.5f, 1.5f };
    const float key_size = 44.0f;
    const float spacing = 6.0f;
    const float key_stride = key_size + spacing;
    const float block_width = 10.0f * key_stride - spacing;

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float base_x = ImGui::GetCursorPosX() + std::max(0.0f, (avail_w - block_width) * 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    for (int row = 0; row < 4; ++row) {
        const char* keys = kRows[row];
        ImGui::SetCursorPosX(base_x + kIndentKeys[row] * key_stride);
        for (int i = 0; keys[i] != '\0'; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, spacing);
            char key_label[3] = { keys[i], '\0', '\0' };
            ImGui::PushID(row * 100 + i);
            if (ImGui::Button(key_label, ImVec2(key_size, key_size))) {
                size_t len = std::strlen(filter);
                if (len + 1 < filter_size) {
                    filter[len] = keys[i];
                    filter[len + 1] = '\0';
                }
            }
            ImGui::PopID();
        }
    }

    ImGui::SetCursorPosX(base_x);
    if (ImGui::Button("Space", ImVec2(key_stride * 5.0f - spacing, key_size))) {
        size_t len = std::strlen(filter);
        if (len + 1 < filter_size) { filter[len] = ' '; filter[len + 1] = '\0'; }
    }
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button("Backspace", ImVec2(key_stride * 3.0f - spacing, key_size))) {
        size_t len = std::strlen(filter);
        if (len > 0) filter[len - 1] = '\0';
    }
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button("Close", ImVec2(key_stride * 2.0f - spacing, key_size))) {
        m_rom_search_keyboard_open = false;
    }
    ImGui::PopStyleVar();
    ImGui::Separator();
}

// ============================================================
// draw_rom_preview_sidebar — Library's persistent right-hand sidebar. Text
// (name/system/size) is instant on hover; after a 0.5s hover-dwell on the
// SAME row (see draw_library_rom_list()'s dwell-timer block), a real live
// preview kicks in via start_library_live_preview()/update_library_live_preview()
// — the exact same RomPreviewManager/session/BGM-duck machinery the 3D
// Shelf's laser-hover path uses, just triggered from this flat list. The
// image fades in via m_library_preview_reveal_t as the filler text fades
// out, in place of ImGui position-tweening (no such primitive exists here).
// ============================================================
void OpenXrShell::draw_rom_preview_sidebar() {
    if (m_library_preview_path.empty()) {
        ImGui::TextDisabled("Click a ROM in Library to see its details here.");
        return;
    }

    ImGui::TextWrapped("%s", m_library_preview_name.c_str());
    ImGui::Spacing();
    if (!m_library_preview_system.empty()) {
        ImGui::TextDisabled("System:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", m_library_preview_system.c_str());
    }
    if (!m_library_preview_size_str.empty()) {
        ImGui::TextDisabled("Size:");
        ImGui::SameLine();
        ImGui::Text("%s", m_library_preview_size_str.c_str());
    }
    std::string extract_status;
    if (m_library_preview_is_archive) {
        extract_status = preview_extract_status(m_library_preview_path);
        ImGui::TextDisabled("Archive:");
        ImGui::SameLine();
        if (!extract_status.empty()) {
            ImGui::TextWrapped("%s", extract_status.c_str());
        } else if (!m_library_preview_uncompressed_size_str.empty()) {
            ImGui::TextWrapped("Decompressed");
        } else {
            ImGui::TextWrapped("Needs decompressing");
        }
        if (!m_library_preview_uncompressed_size_str.empty()) {
            ImGui::TextDisabled("Uncompressed:");
            ImGui::SameLine();
            ImGui::Text("%s", m_library_preview_uncompressed_size_str.c_str());
        }
    }
    if (m_library_preview_has_frame) {
        ImGui::TextDisabled("Layers:");
        ImGui::SameLine();
        ImGui::Text("%d", m_library_preview_layer_count);
    }

    // Reveal step: advance toward 1.0 once actually playing, else decay —
    // simple linear per-frame step (no easing helper exists in this codebase).
    const float target = (m_library_live_preview_active && m_library_preview_has_frame) ? 1.0f : 0.0f;
    const float step = ImGui::GetIO().DeltaTime * 4.0f; // ~0.25s fade
    if (m_library_preview_reveal_t < target) m_library_preview_reveal_t = std::min(target, m_library_preview_reveal_t + step);
    else if (m_library_preview_reveal_t > target) m_library_preview_reveal_t = std::max(target, m_library_preview_reveal_t - step);

    ImGui::Spacing();
    if (m_library_preview_reveal_t < 1.0f) {
        ImGui::TextDisabled("Loading live preview...");
    }
    // The preview lives here, but not as an ImGui image: it is drawn as real
    // world-space layer quads standing out of this rectangle toward the
    // headset (build_library_preview_diorama()). ImGui renders one flat
    // texture, so a 2D copy could only ever be a depthless duplicate. All this
    // does is reserve the space and record where it ended up.
    if (m_library_preview_has_frame && m_library_preview_diorama_layers > 0) {
        const float avail_w = ImGui::GetContentRegionAvail().x;
        float h = avail_w / (m_library_preview_aspect > 0.01f ? m_library_preview_aspect
                                                             : 4.0f / 3.0f);
        constexpr float kMaxImageH = 260.0f;
        if (h > kMaxImageH) h = kMaxImageH;
        ImGui::Dummy(ImVec2(avail_w, h));
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (disp.x > 0.0f && disp.y > 0.0f) {
            m_library_preview_rect_u0 = rmin.x / disp.x;
            m_library_preview_rect_v0 = rmin.y / disp.y;
            m_library_preview_rect_u1 = rmax.x / disp.x;
            m_library_preview_rect_v1 = rmax.y / disp.y;
            m_library_preview_rect_valid = true;
        }
    } else {
        m_library_preview_rect_valid = false;
    }

    // Confirm step: selecting a ROM (click in draw_library_rom_list()) only
    // previews it now — this is the explicit launch the user asked for
    // instead of a click immediately booting the game.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const std::string play_path = m_library_preview_path;
    // Archive extraction is performed by the live-preview worker after the
    // ROM is selected. Keep the launch button visible, but prevent a second
    // foreground preparation path from starting while that extraction is in
    // progress. The Archive row above already displays the per-file status.
    const bool waiting_for_extraction = m_library_preview_is_archive && !extract_status.empty();
    ImGui::BeginDisabled(waiting_for_extraction);
    if (ImGui::Button("PLAY", ImVec2(ImGui::GetContentRegionAvail().x, 48.0f))) {
        RomLoader loader;
        { std::lock_guard<std::mutex> lk(m_mutex); loader = m_rom_loader; }
        if (loader && !play_path.empty()) {
            if (m_library_live_preview_active) stop_library_live_preview();
            fire_haptic(true, 0.7f, 100);
            start_async_rom_preparation(play_path);
        }
    }
    ImGui::EndDisabled();
}

// ============================================================
// draw_controls_group — Controls > Controller Map. Real remap grid over
// m_button_map (button_map.h's qrd::ButtonMap — the SAME array the old
// Controller Map panel and save_settings()/load_settings() already read/write,
// via btn_map_<backend>_%d keys in settings_io.h), not a placeholder.
// ============================================================
void OpenXrShell::draw_controls_group() {
    ImGui::TextDisabled("%s", qrd::button_map_title_for_backend(m_current_backend_kind));
    ImGui::TextDisabled("Pick the physical input that presses each button.");
    ImGui::Separator();
    for (int b = 0; b < qrd::SNES_BUTTON_COUNT; ++b) {
        const char* name = qrd::button_name_for_backend(m_current_backend_kind, b);
        // "Unused"/"?" slots don't exist on this backend's real pad — skip
        // them rather than showing a remap row for a button that does nothing.
        if (strcmp(name, "Unused") == 0 || strcmp(name, "?") == 0) continue;
        ImGui::PushID(b);
        // One combo per button rather than a wrapped grid of every input:
        // with motion controls added the input list is long enough that the
        // grid ran several lines per button and the whole panel became a wall
        // of pills to scan. The combo also makes the CURRENT binding the thing
        // you read first, which is what you actually want when reviewing a map.
        ImGui::Text("%s", name);
        ImGui::SameLine(120.0f);
        ImGui::SetNextItemWidth(180.0f);
        const int cur = (m_button_map[b] > qrd::QI_NONE && m_button_map[b] < qrd::QI_COUNT)
            ? m_button_map[b] : qrd::QI_NONE;
        if (ImGui::BeginCombo("##map", qrd::qi_name(cur))) {
            for (int qi = 0; qi < qrd::QI_COUNT; ++qi) {
                if (ImGui::Selectable(qrd::qi_name(qi), cur == qi)) {
                    m_button_map[b] = qi;
                    fire_haptic(true, 0.5f, 40);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
}

// ============================================================
// ============================================================
// update_air_wheel — Experimental > Motion Controls > Air Wheel.
//
// Reads both controller poses as a single two-handed frame and latches each
// enabled motion into its QI_WHEEL_* slot. Called from poll_actions() while
// qi_state[] is being built, i.e. BEFORE the button map runs, so every gesture
// is remappable exactly like a stick direction.
//
// Neutral (the "hands resting on the wheel" pose) is captured the first frame
// the feature is live and re-captured by recenter_air_wheel(); every distance
// motion is measured as a delta from it, so arm length and seating position
// don't matter.
// ============================================================
// locate_hand_poses — both controller grip poses in app space, or false if
// either is untracked (hands behind the back, controller asleep). Shared by
// every motion feature so they all fail the same way.
bool OpenXrShell::locate_hand_poses(XrPosef& left, XrPosef& right) {
    auto locate = [&](XrSpace hand_space, XrPosef& out) -> bool {
        if (hand_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE) return false;
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
    return locate(m_impl->lhand_space, left) && locate(m_impl->rhand_space, right);
}

// queue_fight_macro — start playing a directional sequence. Ignored while one
// is already running, so a single arm swing cannot stack two moves.
void OpenXrShell::queue_fight_macro(const unsigned* steps, int count, const char* name) {
    if (m_air_fighter.macro.cur >= 0) return;
    count = std::min(count, FightMacro::kMaxSteps);
    for (int i = 0; i < count; ++i) m_air_fighter.macro.steps[i] = steps[i];
    m_air_fighter.macro.step_count = count;
    m_air_fighter.macro.cur = 0;
    m_air_fighter.macro.step_ends_at =
        (float)m_frame_predicted_time * 1.0e-9f + VrState::kFightStepSeconds;
    m_air_fighter.last_move = name;
    fire_haptic(true, 0.8f, 60);
}

// update_air_fighter — Experimental > Motion Controls > Air Fighter.
//
// Two halves: recognise an arm motion, then play the matching directional
// sequence back over the next few frames.
//
// The side a move comes out is taken from WHERE THE MOTION FINISHES relative
// to the player's own facing, never from the game's memory. Finish a
// quarter-circle on the left and it plays Down, Down+Left, Left, Punch; finish
// on the right and it mirrors. That removes any need to know which side of the
// screen the character is standing on -- the player can already see that, and
// they simply throw the fireball the way they want it to go.
void OpenXrShell::update_air_fighter() {
    if (!m_vr_state.air_fighter_enabled) {
        m_air_fighter = AirFighterState{};
        return;
    }
    const float now_s = (float)m_frame_predicted_time * 1.0e-9f;

    // ---- advance any sequence already playing ----
    FightMacro& mac = m_air_fighter.macro;
    if (mac.cur >= 0 && now_s >= mac.step_ends_at) {
        if (++mac.cur >= mac.step_count) {
            mac.cur = -1;
            // Short cooldown after a move so the arm's return travel does not
            // immediately read as another gesture.
            m_air_fighter.rearm_at = now_s + 0.25f;
        } else {
            mac.step_ends_at = now_s + VrState::kFightStepSeconds;
        }
    }

    XrPosef lhand{}, rhand{};
    if (!locate_hand_poses(lhand, rhand)) {
        m_air_fighter.last_valid = false;
        m_air_fighter.charge_dir = 0;
        return;
    }

    // Player-relative axes, so "left of the screen" means the player's left
    // rather than a fixed world direction -- they can stand however they like.
    const XrQuaternionf& hq = m_impl->last_hmd_pose.orientation;
    const XrVector3f head = m_impl->last_hmd_pose.position;
    const XrVector3f right{
        1.0f - 2.0f * (hq.y * hq.y + hq.z * hq.z),
        2.0f * (hq.x * hq.y + hq.z * hq.w),
        2.0f * (hq.x * hq.z - hq.y * hq.w)};
    const XrVector3f fwd{
        -2.0f * (hq.x * hq.z + hq.y * hq.w),
        -2.0f * (hq.y * hq.z - hq.x * hq.w),
        -(1.0f - 2.0f * (hq.x * hq.x + hq.y * hq.y))};
    const auto dot = [](const XrVector3f& a, const XrVector3f& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const auto rel = [&](const XrVector3f& p) {
        return XrVector3f{p.x - head.x, p.y - head.y, p.z - head.z};
    };

    const XrVector3f lp = lhand.position, rp = rhand.position;
    float dt = 0.0f;
    XrVector3f lv{0,0,0}, rv{0,0,0};
    if (m_air_fighter.last_valid) {
        dt = now_s - m_air_fighter.last_time;
        if (dt > 1.0e-4f && dt < 0.25f) {
            lv = {(lp.x - m_air_fighter.last_l.x) / dt,
                  (lp.y - m_air_fighter.last_l.y) / dt,
                  (lp.z - m_air_fighter.last_l.z) / dt};
            rv = {(rp.x - m_air_fighter.last_r.x) / dt,
                  (rp.y - m_air_fighter.last_r.y) / dt,
                  (rp.z - m_air_fighter.last_r.z) / dt};
        } else {
            dt = 0.0f; // frame hitch or the menu freeze; skip this sample
        }
    }
    m_air_fighter.last_l = lp;
    m_air_fighter.last_r = rp;
    m_air_fighter.last_time = now_s;
    m_air_fighter.last_valid = true;
    if (dt <= 0.0f) return;

    const bool busy = mac.cur >= 0 || now_s < m_air_fighter.rearm_at;

    // ---- Charge move (Sonic Boom style) --------------------------------
    // The charge is held FOR REAL, in real time: the game needs to see the
    // back direction pressed for its full ~2s charge, so we hold that
    // direction while the hand stays out to the side rather than trying to
    // fake it inside a macro. Only the release is a sequence.
    const bool charge_any = m_vr_state.fight_charge_across_enabled ||
                            m_vr_state.fight_charge_up_enabled;
    if (charge_any) {
        // Whichever hand is further out to a side owns the charge.
        const float lx = dot(rel(lp), right), rx = dot(rel(rp), right);
        const int hand = std::fabs(lx) > std::fabs(rx) ? 0 : 1;
        const float hx = hand == 0 ? lx : rx;
        const XrVector3f& hv = hand == 0 ? lv : rv;
        const int side = hx > m_vr_state.fight_charge_distance ? 1
                       : hx < -m_vr_state.fight_charge_distance ? -1 : 0;

        if (side != 0 && (m_air_fighter.charge_dir != side || m_air_fighter.charge_hand != hand)) {
            m_air_fighter.charge_dir = side;
            m_air_fighter.charge_hand = hand;
            m_air_fighter.charge_since = now_s;
            m_air_fighter.charge_ready = false;
        } else if (side == m_air_fighter.charge_dir && side != 0) {
            if (now_s - m_air_fighter.charge_since >= m_vr_state.fight_charge_seconds &&
                !m_air_fighter.charge_ready) {
                m_air_fighter.charge_ready = true;
                fire_haptic(hand == 1, 0.4f, 40); // "charged" tick
            }
        }

        // Once charged, the SAME held position offers two moves, decided by
        // how you finish -- which is why the charge holds Down as well as the
        // side (see the qi_state block): a real down-back charge stores the
        // horizontal and vertical charges at the same time.
        //
        // Note the release test does not wait for the hand to come back to
        // centre. An uppercut finishes with the hand still out to the side, so
        // requiring that would make the up-finish unreachable.
        if (m_air_fighter.charge_ready && !busy) {
            const float across = dot(hv, right) * (float)(-m_air_fighter.charge_dir);
            if (m_vr_state.fight_charge_up_enabled && hv.y > m_vr_state.fight_charge_speed) {
                // Finished upward: Up + Kick.
                const unsigned release = FB_UP | FB_KICK;
                const unsigned steps[] = {release, release};
                queue_fight_macro(steps, 2, "Charge Up + Kick");
                m_air_fighter.charge_dir = 0;
                m_air_fighter.charge_ready = false;
            } else if (m_vr_state.fight_charge_across_enabled &&
                       across > m_vr_state.fight_charge_speed) {
                // Finished across: opposite direction + Punch.
                const unsigned release = (m_air_fighter.charge_dir > 0 ? FB_LEFT : FB_RIGHT) | FB_PUNCH;
                const unsigned steps[] = {release, release};
                queue_fight_macro(steps, 2, "Charge Across + Punch");
                m_air_fighter.charge_dir = 0;
                m_air_fighter.charge_ready = false;
            }
        }
        // Hand simply returned to centre without a committed release: the
        // charge is spent, same as letting go of back in the real game.
        if (side == 0 && m_air_fighter.charge_dir != 0 && !m_air_fighter.charge_ready) {
            m_air_fighter.charge_dir = 0;
        }
    } else {
        m_air_fighter.charge_dir = 0;
        m_air_fighter.charge_ready = false;
    }

    if (busy) return;

    // ---- Dragon Punch: one hand swings up ------------------------------
    // Left hand aims the move left, right hand aims it right, exactly as the
    // arm travels. Checked before the quarter-circle because an uppercut also
    // carries some forward motion and would otherwise match both.
    if (m_vr_state.fight_dp_enabled) {
        const bool l_up = lv.y > m_vr_state.fight_dp_speed;
        const bool r_up = rv.y > m_vr_state.fight_dp_speed;
        if (l_up != r_up) { // one hand only; both is something else
            const unsigned dir  = l_up ? FB_LEFT : FB_RIGHT;
            const unsigned diag = FB_DOWN | dir;
            // Forward, Down, Down-Forward + Punch.
            const unsigned steps[] = {dir, FB_DOWN, diag, (unsigned)(diag | FB_PUNCH)};
            queue_fight_macro(steps, 4, l_up ? "Dragon Punch Left" : "Dragon Punch Right");
            return;
        }
    }

    // ---- Quarter-Circle + Punch: both hands thrust forward -------------
    if (m_vr_state.fight_qc_enabled) {
        const float lf = dot(lv, fwd), rf = dot(rv, fwd);
        if (lf > m_vr_state.fight_qc_speed && rf > m_vr_state.fight_qc_speed) {
            // Side is decided by where the hands END UP, not by the game.
            const float mid_x = (dot(rel(lp), right) + dot(rel(rp), right)) * 0.5f;
            const unsigned dir  = mid_x < 0.0f ? FB_LEFT : FB_RIGHT;
            const unsigned diag = FB_DOWN | dir;
            const unsigned steps[] = {FB_DOWN, diag, dir, (unsigned)(dir | FB_PUNCH)};
            queue_fight_macro(steps, 4,
                              mid_x < 0.0f ? "Quarter-Circle Left" : "Quarter-Circle Right");
            return;
        }
    }

    // ---- Quarter-Circle + Kick: one hand thrust sideways ---------------
    // Direction is simply the way you swing: strike to your left and it plays
    // Down, Down+Left, Left, Kick. Checked before normals, whose forward-speed
    // test a sideways strike would not satisfy anyway, and skipped while a
    // charge is held so it cannot steal the charge release.
    if (m_vr_state.fight_qck_enabled && m_air_fighter.charge_dir == 0) {
        // Sideways must dominate: a hook or an off-centre jab has plenty of
        // lateral travel, so speed alone is not enough to tell them apart.
        const auto sideways = [&](const XrVector3f& v) -> float {
            const float lat = dot(v, right);
            const float other = std::sqrt(dot(v, fwd) * dot(v, fwd) + v.y * v.y);
            if (std::fabs(lat) < m_vr_state.fight_qck_speed) return 0.0f;
            if (std::fabs(lat) < other * m_vr_state.fight_qck_purity) return 0.0f;
            return lat;
        };
        const float lx = sideways(lv), rx = sideways(rv);
        const bool l_hit = lx != 0.0f;
        const bool r_hit = rx != 0.0f;
        if (l_hit != r_hit) { // one hand only
            const float across = l_hit ? lx : rx;
            const unsigned dir  = across < 0.0f ? FB_LEFT : FB_RIGHT;
            const unsigned diag = FB_DOWN | dir;
            const unsigned steps[] = {FB_DOWN, diag, dir, (unsigned)(dir | FB_KICK)};
            queue_fight_macro(steps, 4,
                              across < 0.0f ? "Quarter-Circle Kick Left"
                                            : "Quarter-Circle Kick Right");
            return;
        }
    }

    // ---- Normal attacks: one hand thrust ------------------------------
    // Last, so a two-hand thrust is a quarter-circle and an upward swing is a
    // Dragon Punch -- a normal is what is left over.
    //
    // Light and heavy differ only in what the hand does after the strike, so
    // the strike opens a short window and the strength is resolved when it
    // expires. That costs the light attack fight_hold_seconds of latency;
    // there is no way around it, because at the moment of impact the two are
    // physically identical.
    if (m_vr_state.fight_punch_enabled || m_vr_state.fight_kick_enabled) {
        if (m_air_fighter.pending && now_s >= m_air_fighter.pending_until) {
            const XrVector3f& hp = m_air_fighter.pending_hand == 0 ? lp : rp;
            const float now_reach = dot(rel(hp), fwd);
            // Still out near where it landed = heavy; snapped back = light.
            const bool held = now_reach > m_air_fighter.pending_reach - 0.06f;
            m_air_fighter.pending = false;
            const bool kick = m_air_fighter.pending_kick;
            const unsigned bit = kick ? (held ? FB_KICK_HARD : FB_KICK)
                                      : (held ? FB_PUNCH_HARD : FB_PUNCH);
            const char* name = kick ? (held ? "Heavy Kick" : "Light Kick")
                                    : (held ? "Heavy Punch" : "Light Punch");
            const unsigned steps[] = {bit, bit};
            queue_fight_macro(steps, 2, name);
            return;
        }
        if (!m_air_fighter.pending) {
            const float lf = dot(lv, fwd), rf = dot(rv, fwd);
            const bool l_hit = lf > m_vr_state.fight_punch_speed;
            const bool r_hit = rf > m_vr_state.fight_punch_speed;
            if (l_hit != r_hit) { // one hand; both is a quarter-circle
                const int hand = l_hit ? 0 : 1;
                const XrVector3f& hv2 = l_hit ? lv : rv;
                const float f = l_hit ? lf : rf;
                // Angled down past the ratio makes it a kick. Upward is left
                // alone -- Dragon Punch owns that axis.
                const bool kick = (-hv2.y) > f * m_vr_state.fight_kick_ratio;
                const bool want = kick ? m_vr_state.fight_kick_enabled
                                       : m_vr_state.fight_punch_enabled;
                if (want) {
                    if (!m_vr_state.fight_heavy_enabled) {
                        // No heavy variant to wait for, so there is nothing to
                        // decide: fire immediately with zero added latency.
                        const unsigned bit = kick ? FB_KICK : FB_PUNCH;
                        const unsigned steps[] = {bit, bit};
                        queue_fight_macro(steps, 2, kick ? "Kick" : "Punch");
                        return;
                    }
                    m_air_fighter.pending       = true;
                    m_air_fighter.pending_kick  = kick;
                    m_air_fighter.pending_hand  = hand;
                    m_air_fighter.pending_until = now_s + m_vr_state.fight_hold_seconds;
                    m_air_fighter.pending_reach = dot(rel(l_hit ? lp : rp), fwd);
                }
            }
        }
    } else {
        m_air_fighter.pending = false;
    }
}


// update_air_jump — Experimental > Motion Controls > Air Jump / Crouch.
//
// A sudden rise of the headset holds "jump", a sudden drop holds "crouch".
// Same two-stage shape as the Air Wheel brake: SPEED triggers it, POSITION
// sustains it -- so ducking behind cover and staying down keeps crouch held,
// while standing back up releases it. Slowly straightening from a slouch never
// triggers anything, which is what makes it usable while just sitting there.
//
// Neutral is a slow running average of head height rather than a fixed value:
// it self-calibrates to the player and follows them sitting down mid-session.
// It is frozen while either input is held, or the head is moving, so a long
// crouch cannot drag the neutral down and release itself.
void OpenXrShell::update_air_jump() {
    if (!m_vr_state.air_jump_enabled) {
        m_air_jump = AirJumpState{};
        return;
    }
    const float head_y = m_impl->last_hmd_pose.position.y;
    const float now_s  = (float)m_frame_predicted_time * 1.0e-9f;

    if (!m_air_jump.neutral_valid) {
        m_air_jump.neutral_height = head_y;
        m_air_jump.neutral_valid  = true;
        m_air_jump.last_y = head_y;
        m_air_jump.last_time = now_s;
        m_air_jump.last_time_valid = true;
        return;
    }

    float speed = 0.0f; // metres/second, positive = rising
    if (m_air_jump.last_time_valid) {
        const float dt = now_s - m_air_jump.last_time;
        // Reject frame hitches and the menu freeze, which would otherwise
        // produce a huge velocity from a stale sample.
        if (dt > 1.0e-4f && dt < 0.25f) speed = (head_y - m_air_jump.last_y) / dt;
    }
    m_air_jump.last_y = head_y;
    m_air_jump.last_time = now_s;
    m_air_jump.last_time_valid = true;

    const float offset = head_y - m_air_jump.neutral_height;
    const float margin = m_vr_state.air_jump_hold_margin;

    // Jump: triggered rising fast, held while the head stays high.
    if (speed > m_vr_state.air_jump_speed) {
        m_air_jump.jump = true;
        m_air_jump.jump_hold_until = now_s + VrState::kAirJumpMinHold;
        m_air_jump.crouch = false; // cannot be both at once
    } else if (m_air_jump.jump && now_s > m_air_jump.jump_hold_until && offset < margin) {
        m_air_jump.jump = false;
    }
    // Crouch: the mirror image.
    if (-speed > m_vr_state.air_jump_speed) {
        m_air_jump.crouch = true;
        m_air_jump.crouch_hold_until = now_s + VrState::kAirJumpMinHold;
        m_air_jump.jump = false;
    } else if (m_air_jump.crouch && now_s > m_air_jump.crouch_hold_until && offset > -margin) {
        m_air_jump.crouch = false;
    }

    // Adaptive neutral, frozen whenever something is held or the head is
    // moving -- see the note above about a long crouch releasing itself.
    constexpr float kStillSpeed = 0.12f; // m/s
    if (!m_air_jump.jump && !m_air_jump.crouch && std::fabs(speed) < kStillSpeed) {
        constexpr float kDrift = 0.004f; // ~4s time constant
        m_air_jump.neutral_height += (head_y - m_air_jump.neutral_height) * kDrift;
    }
}

// update_dpad_headset — Experimental > Motion Controls > D-Pad Headset.
//
// Roll (ear toward a shoulder) drives left/right, pitch drives up/down, and
// because the two axes latch independently they combine into diagonals. Yaw is
// deliberately ignored so turning to look around never steers.
//
// Called every frame from poll_actions() before the menu/game branch, so the
// live readout in the Experimental panel keeps updating while that panel is
// open. Only latch state is touched here; poll_actions' game path copies it
// into qi_state[QI_HEAD_*].
void OpenXrShell::update_dpad_headset() {
    if (!m_vr_state.dpad_headset_enabled) {
        m_dpad_headset_left = m_dpad_headset_right = false;
        m_dpad_headset_up   = m_dpad_headset_down  = false;
        return;
    }
    const XrQuaternionf& hq = m_impl->last_hmd_pose.orientation;
    const float sinr_cosp = 2.0f * (hq.w * hq.z + hq.x * hq.y);
    const float cosr_cosp = 1.0f - 2.0f * (hq.y * hq.y + hq.z * hq.z);
    const float roll  = std::atan2(sinr_cosp, cosr_cosp);
    const float sinp  = std::clamp(2.0f * (hq.w * hq.x - hq.y * hq.z), -1.0f, 1.0f);
    const float pitch = std::asin(sinp);

    const float engage = std::clamp(m_vr_state.dpad_headset_threshold,
                                    VrState::kDpadHeadsetThresholdMin,
                                    VrState::kDpadHeadsetThresholdMax);
    const float release = engage * VrState::kDpadHeadsetRelease;
    // Held directions only need to stay above the release band; new ones must
    // cross the full threshold.
    const auto latch = [&](float value, bool& held_pos, bool& held_neg) {
        held_pos = value > (held_pos ? release : engage);
        held_neg = -value > (held_neg ? release : engage);
    };
    latch(roll,  m_dpad_headset_left, m_dpad_headset_right);
    latch(pitch, m_dpad_headset_up,   m_dpad_headset_down);
}

void OpenXrShell::update_air_wheel() {
    if (!m_vr_state.air_wheel_enabled) {
        // Drop every latch so nothing sticks down when the feature goes off.
        m_air_wheel = AirWheelState{};
        return;
    }

    auto locate = [&](XrSpace hand_space, XrPosef& out) -> bool {
        if (hand_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE) return false;
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

    XrPosef lhand{}, rhand{};
    if (!locate(m_impl->lhand_space, lhand) || !locate(m_impl->rhand_space, rhand)) {
        // Tracking lost (hands behind the back, controller asleep): release
        // everything rather than freezing the last held input on screen.
        m_air_wheel = AirWheelState{};
        return;
    }

    const XrVector3f lp = lhand.position, rp = rhand.position;
    const XrVector3f mid{(lp.x + rp.x) * 0.5f, (lp.y + rp.y) * 0.5f, (lp.z + rp.z) * 0.5f};
    const XrVector3f axis{rp.x - lp.x, rp.y - lp.y, rp.z - lp.z};
    const float separation = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);

    // Chest anchor: straight down from the headset, then low-passed. The fixed
    // alpha gives roughly a 0.7s time constant at Quest frame rates -- slow
    // enough to ignore leaning, fast enough to follow you walking around.
    const XrVector3f hmd = m_impl->last_hmd_pose.position;
    const XrVector3f chest{hmd.x, hmd.y - 0.35f, hmd.z};
    if (!m_air_wheel.anchor_valid) {
        m_air_wheel.anchor = chest;
        m_air_wheel.anchor_valid = true;
    } else {
        constexpr float kAlpha = 0.02f;
        m_air_wheel.anchor.x += (chest.x - m_air_wheel.anchor.x) * kAlpha;
        m_air_wheel.anchor.y += (chest.y - m_air_wheel.anchor.y) * kAlpha;
        m_air_wheel.anchor.z += (chest.z - m_air_wheel.anchor.z) * kAlpha;
    }
    const XrVector3f reach_v{mid.x - m_air_wheel.anchor.x,
                             mid.y - m_air_wheel.anchor.y,
                             mid.z - m_air_wheel.anchor.z};
    const float reach = std::sqrt(reach_v.x * reach_v.x + reach_v.y * reach_v.y +
                                  reach_v.z * reach_v.z);

    // Capture the neutral pose once, so every motion below is a delta from
    // however this particular player happens to hold their hands.
    if (!m_air_wheel.neutral_valid) {
        m_air_wheel.neutral_reach  = reach;
        m_air_wheel.neutral_height = mid.y;
        m_air_wheel.neutral_valid  = true;
    }

    // Shared engage/release latch: cross the threshold to arm, fall back under
    // kAirWheelRelease of it to let go, so a hand hovering at the line does not
    // chatter the input on and off every frame.
    const auto latch = [](float value, float threshold, bool& held) {
        const float limit = held ? threshold * VrState::kAirWheelRelease : threshold;
        held = value > limit;
    };

    // Steering: roll of the line between the hands. Right hand rising is a
    // left turn, matching a real wheel.
    if (m_vr_state.air_wheel_steer_enabled && separation > 0.05f) {
        const float roll = std::asin(std::clamp(axis.y / separation, -1.0f, 1.0f));
        latch( roll, m_vr_state.air_wheel_steer_threshold, m_air_wheel.steer_left);
        latch(-roll, m_vr_state.air_wheel_steer_threshold, m_air_wheel.steer_right);
    } else {
        m_air_wheel.steer_left = m_air_wheel.steer_right = false;
    }

    // Inward speed of the hands, for the brake gesture below. Positive means
    // pulling toward the chest.
    const float now_s = (float)m_frame_predicted_time * 1.0e-9f;
    float pull_speed = 0.0f;
    if (m_air_wheel.last_time_valid) {
        const float dt = now_s - m_air_wheel.last_time;
        // Guard against a frame hitch or the emulator freeze producing a huge
        // dt (and a nonsense velocity from a stale sample).
        if (dt > 1.0e-4f && dt < 0.25f)
            pull_speed = (m_air_wheel.last_reach - reach) / dt;
    }
    m_air_wheel.last_reach = reach;
    m_air_wheel.last_time  = now_s;
    m_air_wheel.last_time_valid = true;

    const float push = reach - m_air_wheel.neutral_reach;

    // Brake: triggered by SPEED, not position. Yanking the wheel in fires it;
    // easing in slowly does not, so a gentle pull just drops out of the accel
    // threshold above and coasts. Held for a minimum time (a yank lasts only a
    // few frames) and then for as long as the hands stay pulled in, so it
    // behaves like a pedal you keep down rather than a one-shot tap.
    if (m_vr_state.air_wheel_brake_enabled) {
        if (pull_speed > m_vr_state.air_wheel_brake_speed) {
            m_air_wheel.brake = true;
            m_air_wheel.brake_hold_until = now_s + VrState::kAirWheelBrakeMinHold;
        } else if (m_air_wheel.brake) {
            const bool still_pulled_in =
                push < -m_vr_state.air_wheel_push_threshold * VrState::kAirWheelRelease;
            if (now_s > m_air_wheel.brake_hold_until && !still_pulled_in)
                m_air_wheel.brake = false;
        }
    } else {
        m_air_wheel.brake = false;
    }

    // Accelerate: held down constantly, released only while braking -- the
    // arcade-racer convention. Computed AFTER the brake so it sees this
    // frame's brake state rather than last frame's. There is no accelerate
    // gesture and no accelerate threshold: pushing the wheel out does nothing,
    // pulling it in fast is what changes the state.
    m_air_wheel.accel = m_vr_state.air_wheel_accel_enabled && !m_air_wheel.brake;

    // Gear: point the RIGHT controller at the floor to shift up, at the
    // ceiling to shift down -- a quick flick of the shifter hand while the
    // left stays on the wheel. Measured as the Y of the controller's forward
    // axis (-1 straight down, +1 straight up) rather than an Euler pitch, so
    // it stays well-behaved when the hand is also rolled or yawed.
    if (m_vr_state.air_wheel_gear_enabled) {
        const XrQuaternionf& q = rhand.orientation;
        // Rotate the grip pose's forward axis (-Z) by q; we only need Y.
        const float fwd_y = -(2.0f * (q.y * q.z + q.w * q.x));
        latch(-fwd_y, m_vr_state.air_wheel_gear_threshold, m_air_wheel.gear_up);
        latch( fwd_y, m_vr_state.air_wheel_gear_threshold, m_air_wheel.gear_down);
    } else {
        m_air_wheel.gear_up = m_air_wheel.gear_down = false;
    }

    // Handbrake: both hands lifted, like yanking a lever up.
    if (m_vr_state.air_wheel_handbrake_enabled)
        latch(mid.y - m_air_wheel.neutral_height,
              m_vr_state.air_wheel_handbrake_threshold, m_air_wheel.handbrake);
    else m_air_wheel.handbrake = false;

    // Bike accelerate: twist both grips forward/down like a throttle. Uses the
    // average pitch of the two controllers, so it reads a genuine twist rather
    // than one hand drifting.
    if (m_vr_state.air_wheel_bike_enabled) {
        const auto pitch_of = [](const XrQuaternionf& q) {
            return std::asin(std::clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f));
        };
        const float twist = -(pitch_of(lhand.orientation) + pitch_of(rhand.orientation)) * 0.5f;
        latch(twist, m_vr_state.air_wheel_bike_threshold, m_air_wheel.bike);
    } else m_air_wheel.bike = false;

    // Adaptive neutral: let the rest pose drift toward wherever the hands
    // actually sit, so the wheel ends up wherever is comfortable rather than
    // wherever it was first captured.
    //
    // Critically, this only runs while NOTHING is latched and the hands are
    // near still. Drifting during a held input would walk the neutral out to
    // meet the hands and silently release the very input being held -- hold
    // accelerate down a long straight and it would quietly stop accelerating.
    if (m_vr_state.air_wheel_adaptive_neutral) {
        // Accelerate is deliberately NOT in this list: it is now held down
        // permanently, so including it would freeze the drift forever and
        // silently disable adaptive neutral altogether.
        const bool any_latched = m_air_wheel.steer_left || m_air_wheel.steer_right ||
                                 m_air_wheel.brake ||
                                 m_air_wheel.gear_up || m_air_wheel.gear_down ||
                                 m_air_wheel.handbrake || m_air_wheel.bike;
        constexpr float kStillSpeed = 0.15f; // m/s
        if (!any_latched && std::fabs(pull_speed) < kStillSpeed) {
            // ~4s time constant: far slower than any deliberate gesture, so it
            // tracks posture without ever chasing a movement.
            constexpr float kDrift = 0.004f;
            m_air_wheel.neutral_reach  += (reach - m_air_wheel.neutral_reach) * kDrift;
            m_air_wheel.neutral_height += (mid.y - m_air_wheel.neutral_height) * kDrift;
        }
    }

}

// recenter_air_wheel — forget the captured neutral so the next frame re-reads
// it from wherever the hands currently are.
void OpenXrShell::recenter_air_wheel() {
    m_air_wheel.neutral_valid = false;
    m_air_wheel.anchor_valid  = false;
}

// ============================================================
// Experimental tab helpers.
//
// experimental_section() is the shared shape for every feature under
// Experimental: an enable checkbox sitting ON the collapsing header row, with
// the body always expandable regardless of the checkbox. Rationale — gating
// the whole section behind the checkbox hides what the feature even offers
// until you commit to it, and makes the panel reflow under the laser pointer
// every time you toggle something; burying the checkbox inside the section
// instead hides the on/off state whenever the section is collapsed. Putting it
// on the header row keeps state visible while collapsed, keeps the contents
// previewable while disabled, and never moves rows around.
//
// Returns true when the body should be drawn. The caller wraps the body in
// BeginDisabled(!enabled) so a disabled feature reads as inert but legible.
// ============================================================
static bool experimental_section(const char* id, const char* label, bool* enabled) {
    ImGui::PushID(id);
    ImGui::Checkbox("##enable", enabled);
    ImGui::SameLine();
    const bool open = ImGui::CollapsingHeader(label);
    ImGui::PopID();
    return open;
}

// draw_experimental_notice — the standing warning at the top of the tab.
void OpenXrShell::draw_experimental_notice() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.72f, 0.20f, 1.0f));
    ImGui::TextWrapped("These features are unfinished and may misbehave, hurt "
                       "performance, or change between builds.");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Everything here is off by default and saved per-game "
                        "like any other setting. If something breaks, turn it "
                        "off here first.");
    ImGui::Separator();
}

// draw_motion_binding — one "this motion presses <button>" selector,
// shared by the D-Pad Headset directions and every Air Wheel motion.
//
// The button map is button -> QuestInput, so assigning a motion means finding
// whichever console button currently points at this QI (that is the current
// selection) and, on a change, clearing every button holding it before setting
// the new one. Writing m_button_map here is deliberate: it is the exact array
// the Controller Map panel edits and that save_settings() already persists per
// game, so a binding made in this panel is a normal binding in every respect.
void OpenXrShell::draw_motion_binding(int qi, const char* lamp_label) {
    if (qi < 0) return;
    const int idx = qi - VrState::kMotionBindFirst;
    if (idx < 0 || idx >= VrState::kMotionBindCount) return;
    ImGui::PushID(qi);

    // "[LEFT]" -> "LEFT" so the row reads as a label, not a lamp.
    char clean[24];
    snprintf(clean, sizeof(clean), "%s", lamp_label ? lamp_label : "");
    for (char* c = clean; *c; ++c) if (*c == '[' || *c == ']') *c = ' ';

    ImGui::Text("%s acts as", clean);
    ImGui::SameLine(140.0f);
    ImGui::SetNextItemWidth(150.0f);
    int& bind = m_vr_state.motion_bind[idx];
    const char* cur = (bind > qrd::QI_NONE && bind < VrState::kMotionBindFirst)
        ? qrd::qi_name(bind) : "--- (nothing)";
    if (ImGui::BeginCombo("##bind", cur)) {
        if (ImGui::Selectable("--- (nothing)", bind == qrd::QI_NONE)) bind = qrd::QI_NONE;
        // Only real physical inputs are offered: letting a motion stand in for
        // another motion would be circular, so the list stops before the
        // gesture inputs begin.
        for (int t = 1; t < VrState::kMotionBindFirst; ++t) {
            if (ImGui::Selectable(qrd::qi_name(t), bind == t)) {
                bind = t;
                fire_haptic(true, 0.5f, 40);
            }
        }
        ImGui::EndCombo();
    }
    // Show where that lands on this console, so the whole chain is visible
    // without opening the Controller Map.
    if (bind > qrd::QI_NONE && bind < VrState::kMotionBindFirst) {
        const char* lands_on = nullptr;
        for (int b = 0; b < qrd::SNES_BUTTON_COUNT; ++b) {
            if (m_button_map[b] != bind) continue;
            const char* n = qrd::button_name_for_backend(m_current_backend_kind, b);
            if (strcmp(n, "Unused") == 0 || strcmp(n, "?") == 0) continue;
            lands_on = n;
            break;
        }
        ImGui::SameLine();
        if (lands_on) ImGui::TextDisabled("-> %s", lands_on);
        else          ImGui::TextDisabled("-> (not mapped on this console)");
    }
    ImGui::PopID();
}

// draw_motion_controls_group — Experimental > Motion Controls. Houses the
// pointing-device features: Air Controller (aim-to-steer) and Lightgun (the
// existing calibrated gun, mirrored here from Controls > Lightgun).
void OpenXrShell::draw_motion_controls_group() {
    // Group-wide, because it is a statement about how motion relates to the
    // controller as a whole rather than a property of any one gesture.
    ImGui::Checkbox("Exclusive Motion Input", &m_vr_state.motion_exclusive);
    ImGui::TextDisabled("Inputs claimed by an enabled motion stop responding on "
                        "the controller, so only the motion drives them. "
                        "Anything no motion claims keeps working normally.");
    if (m_vr_state.motion_exclusive) {
        // Spell out exactly what has gone dead -- this silently disables parts
        // of the physical controller, and finding that out mid-game would be
        // baffling.
        char list[256] = {0};
        bool any = false;
        const bool head  = m_vr_state.dpad_headset_enabled;
        const bool wheel = m_vr_state.air_wheel_enabled;
        const bool on[VrState::kMotionBindCount] = {
            head, head, head, head,
            wheel && m_vr_state.air_wheel_steer_enabled,
            wheel && m_vr_state.air_wheel_steer_enabled,
            wheel && m_vr_state.air_wheel_accel_enabled,
            wheel && m_vr_state.air_wheel_brake_enabled,
            wheel && m_vr_state.air_wheel_gear_enabled,
            wheel && m_vr_state.air_wheel_handbrake_enabled,
            wheel && m_vr_state.air_wheel_bike_enabled,
            wheel && m_vr_state.air_wheel_gear_enabled,
            m_vr_state.air_jump_enabled, m_vr_state.air_jump_enabled,
            m_vr_state.air_fighter_enabled, m_vr_state.air_fighter_enabled,
            m_vr_state.air_fighter_enabled, m_vr_state.air_fighter_enabled,
            m_vr_state.air_fighter_enabled, m_vr_state.air_fighter_enabled,
            m_vr_state.air_fighter_enabled, m_vr_state.air_fighter_enabled,
        };
        for (int i = 0; i < VrState::kMotionBindCount; ++i) {
            const int t = m_vr_state.motion_bind[i];
            if (!on[i] || t <= qrd::QI_NONE || t >= VrState::kMotionBindFirst) continue;
            const char* n = qrd::qi_name(t);
            if (strstr(list, n)) continue; // two motions can share a target
            if (any) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
            strncat(list, n, sizeof(list) - strlen(list) - 1);
            any = true;
        }
        if (any) ImGui::TextColored(ImVec4(1.00f, 0.72f, 0.20f, 1.0f),
                                    "Disabled on the controller: %s", list);
        else     ImGui::TextDisabled("Nothing claimed yet - enable a motion below.");
    }
    ImGui::Separator();
    if (experimental_section("DpadHeadset", "D-Pad Headset",
                             &m_vr_state.dpad_headset_enabled)) {
        ImGui::BeginDisabled(!m_vr_state.dpad_headset_enabled);
        ImGui::TextWrapped("Tilt your head to hold a d-pad direction. Ear toward "
                           "a shoulder holds left or right, looking up or down "
                           "holds up or down, and tilting both ways at once "
                           "holds a diagonal. The controller keeps working "
                           "normally at the same time.");
        ImGui::Spacing();
        ImGui::Text("Tilt Threshold");
        float deg = m_vr_state.dpad_headset_threshold * 57.2957795f;
        if (ImGui::SliderFloat("##DpadHeadsetThreshold", &deg,
                               VrState::kDpadHeadsetThresholdMin * 57.2957795f,
                               VrState::kDpadHeadsetThresholdMax * 57.2957795f,
                               "%.0f deg")) {
            m_vr_state.dpad_headset_threshold = std::clamp(deg / 57.2957795f,
                VrState::kDpadHeadsetThresholdMin, VrState::kDpadHeadsetThresholdMax);
        }
        ImGui::TextDisabled("Lower reacts sooner but picks up head shake; higher "
                            "ignores fidgeting but asks for a bigger lean. "
                            "Releases at 60%% of this so a held direction "
                            "doesn't flicker.");
        // Live readout: the only practical way to tune this is to lean while
        // watching which directions light up.
        ImGui::Spacing();
        // Combined name first ("UP+LEFT"), so a diagonal reads as the single
        // input the game actually receives rather than two lit words to
        // assemble by eye. The per-direction lamps below still show which axis
        // is doing what while you tune the threshold.
        {
            char combo[24] = {0};
            const char* vert = m_dpad_headset_up ? "UP" : (m_dpad_headset_down ? "DOWN" : nullptr);
            const char* horz = m_dpad_headset_left ? "LEFT" : (m_dpad_headset_right ? "RIGHT" : nullptr);
            if (vert && horz)      snprintf(combo, sizeof(combo), "%s+%s", vert, horz);
            else if (vert)         snprintf(combo, sizeof(combo), "%s", vert);
            else if (horz)         snprintf(combo, sizeof(combo), "%s", horz);
            ImGui::Text("Now holding:");
            ImGui::SameLine();
            if (combo[0]) ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "%s", combo);
            else          ImGui::TextDisabled("(centred)");
        }
        const struct { const char* name; bool on; } dirs[] = {
            {"UP", m_dpad_headset_up}, {"DOWN", m_dpad_headset_down},
            {"LEFT", m_dpad_headset_left}, {"RIGHT", m_dpad_headset_right},
        };
        // Per-direction button assignment. Unbound, each direction falls back
        // to the matching d-pad direction; bind it here and that takes over --
        // useful for games where the camera or menu is on face buttons rather
        // than the pad.
        ImGui::Spacing();
        ImGui::TextDisabled("Assign each direction. Leave unassigned to use the d-pad.");
        draw_motion_binding(QI_HEAD_UP,    "UP");
        draw_motion_binding(QI_HEAD_DOWN,  "DOWN");
        draw_motion_binding(QI_HEAD_LEFT,  "LEFT");
        draw_motion_binding(QI_HEAD_RIGHT, "RIGHT");
        ImGui::Spacing();
        ImGui::TextDisabled("Axes:");
        for (const auto& d : dirs) {
            ImGui::SameLine();
            if (d.on) ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "%s", d.name);
            else      ImGui::TextDisabled("%s", d.name);
        }
        ImGui::EndDisabled();
    }

    // Turning the wheel on re-captures neutral from wherever your hands are,
    // which is what the old explicit Recenter button was for.
    const bool wheel_was_on = m_vr_state.air_wheel_enabled;
    const bool wheel_open = experimental_section("AirWheel", "Air Wheel",
                                                 &m_vr_state.air_wheel_enabled);
    if (m_vr_state.air_wheel_enabled && !wheel_was_on) recenter_air_wheel();
    if (wheel_open) {
        ImGui::BeginDisabled(!m_vr_state.air_wheel_enabled);
        ImGui::TextWrapped("Hold both controllers as if gripping a wheel. Each "
                           "motion below is separate: enable only the ones a "
                           "game needs, then assign them in the Controller Map "
                           "like any other input.");
        ImGui::Checkbox("Adaptive Neutral", &m_vr_state.air_wheel_adaptive_neutral);
        ImGui::TextDisabled("Lets the rest position drift to wherever your hands "
                            "settle, so the wheel ends up comfortable. Pauses "
                            "while a motion is held, so nothing releases itself.");
        ImGui::Separator();

        // One row per motion: enable checkbox, threshold slider, live lamp.
        // Kept in a table so the sliders line up and the panel doesn't reflow
        // as motions are enabled and disabled.
        struct MotionRow {
            const char* id; const char* label; const char* map_name;
            bool*  enabled; float* threshold;
            float  min, max; const char* fmt;
            // Two lamps for the paired motions (steering, gear), one for the
            // rest; neg/pos are the labels shown when each latch is live.
            const bool* live_neg; const char* neg_label;
            const bool* live_pos; const char* pos_label;
            const char* hint;
            // The QuestInput each half of the motion latches into. Assigning a
            // console button here writes straight into m_button_map, the same
            // array the Controller Map panel edits -- so a motion bound here
            // shows up there too, and per-game .ini files already persist it.
            int qi_neg; int qi_pos;
        };
        const MotionRow rows[] = {
            {"Steer", "Steering", "Wheel L / Wheel R",
             &m_vr_state.air_wheel_steer_enabled, &m_vr_state.air_wheel_steer_threshold,
             0.08f, 0.60f, "%.2f rad",
             &m_air_wheel.steer_left, "[LEFT]", &m_air_wheel.steer_right, "[RIGHT]",
             "Roll the wheel. Right hand higher turns left.",
             QI_WHEEL_LEFT, QI_WHEEL_RIGHT},
            {"Accel", "Accelerate", "Accel",
             &m_vr_state.air_wheel_accel_enabled, nullptr,
             0.0f, 0.0f, "",
             nullptr, nullptr, &m_air_wheel.accel, "[ON]",
             "Held down constantly. Released only while braking - no gesture "
             "needed to accelerate.",
             -1, QI_WHEEL_ACCEL},
            {"Brake", "Brake", "Brake",
             &m_vr_state.air_wheel_brake_enabled, &m_vr_state.air_wheel_brake_speed,
             0.20f, 2.00f, "%.2f m/s",
             nullptr, nullptr, &m_air_wheel.brake, "[ON]",
             "Yank the wheel in FAST to brake. Pull in slowly and you just "
             "stop accelerating (coast). Slider is the speed needed, not a distance.",
             -1, QI_WHEEL_BRAKE},
            {"Gear", "Gear", "Gear Up / Gear Dn",
             &m_vr_state.air_wheel_gear_enabled, &m_vr_state.air_wheel_gear_threshold,
             0.20f, 0.90f, "%.2f",
             &m_air_wheel.gear_up, "[UP]", &m_air_wheel.gear_down, "[DOWN]",
             "Right hand: point at the floor to shift up, at the ceiling to shift down.",
             QI_WHEEL_GEAR_UP, QI_WHEEL_GEAR_DOWN},
            {"Handbrake", "Handbrake", "Handbrake",
             &m_vr_state.air_wheel_handbrake_enabled, &m_vr_state.air_wheel_handbrake_threshold,
             0.05f, 0.40f, "%.2f m",
             nullptr, nullptr, &m_air_wheel.handbrake, "[ON]",
             "Lift both hands, like yanking a lever up.",
             -1, QI_WHEEL_HANDBRAKE},
            {"Bike", "Bike Accelerate", "Bike Accel",
             &m_vr_state.air_wheel_bike_enabled, &m_vr_state.air_wheel_bike_threshold,
             0.10f, 0.80f, "%.2f rad",
             nullptr, nullptr, &m_air_wheel.bike, "[ON]",
             "Twist both grips forward, like a motorcycle throttle.",
             -1, QI_WHEEL_BIKE},
        };
        for (const MotionRow& r : rows) {
            ImGui::PushID(r.id);
            ImGui::Checkbox(r.label, r.enabled);
            ImGui::SameLine();
            const ImVec4 lit(0.35f, 1.0f, 0.45f, 1.0f);
            if (r.live_neg && *r.live_neg)      ImGui::TextColored(lit, "%s", r.neg_label);
            else if (r.live_pos && *r.live_pos) ImGui::TextColored(lit, "%s", r.pos_label);
            else                                ImGui::TextDisabled("[  ]");
            ImGui::BeginDisabled(!*r.enabled);
            ImGui::TextDisabled("%s", r.hint);
            // Accelerate has no threshold -- it is not a gesture any more.
            if (r.threshold) ImGui::SliderFloat("##thr", r.threshold, r.min, r.max, r.fmt);
            // Which console button this motion presses. Writes m_button_map
            // directly, so it is the same binding the Controller Map shows and
            // the same one saved per-game -- just reachable from here, where
            // you are actually deciding what the motion is for.
            draw_motion_binding(r.qi_neg, r.neg_label);
            draw_motion_binding(r.qi_pos, r.pos_label);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }

    if (experimental_section("AirFighter", "Air Fighter", &m_vr_state.air_fighter_enabled)) {
        ImGui::BeginDisabled(!m_vr_state.air_fighter_enabled);
        ImGui::TextWrapped("Arm motions play back as timed directional "
                           "sequences. Which side a move comes out is decided "
                           "by where your motion finishes, so you simply throw "
                           "it the way you want it to go - nothing has to know "
                           "which side of the screen your character is on.");
        ImGui::Spacing();
        ImGui::Text("Last move:");
        ImGui::SameLine();
        if (m_air_fighter.macro.cur >= 0)
            ImGui::TextColored(ImVec4(0.35f,1.0f,0.45f,1.0f), "%s (step %d/%d)",
                               m_air_fighter.last_move, m_air_fighter.macro.cur + 1,
                               m_air_fighter.macro.step_count);
        else if (m_air_fighter.last_move[0])
            ImGui::TextDisabled("%s", m_air_fighter.last_move);
        else
            ImGui::TextDisabled("(none yet)");
        if (m_air_fighter.charge_dir != 0) {
            ImGui::SameLine();
            if (m_air_fighter.charge_ready)
                ImGui::TextColored(ImVec4(0.35f,1.0f,0.45f,1.0f), "  CHARGED");
            else
                ImGui::TextColored(ImVec4(1.0f,0.72f,0.20f,1.0f), "  charging...");
        }
        ImGui::Separator();

        ImGui::Checkbox("Quarter-Circle + Punch", &m_vr_state.fight_qc_enabled);
        ImGui::TextDisabled("Thrust BOTH hands forward. Finishing left plays "
                            "Down, Down+Left, Left, Punch; finishing right mirrors it.");
        ImGui::SliderFloat("##qc", &m_vr_state.fight_qc_speed, 0.40f, 3.00f, "%.2f m/s");

        ImGui::Checkbox("Dragon Punch", &m_vr_state.fight_dp_enabled);
        ImGui::TextDisabled("Swing ONE hand up. Left hand aims left, right hand "
                            "aims right. Plays Forward, Down, Down+Forward, Punch.");
        ImGui::SliderFloat("##dp", &m_vr_state.fight_dp_speed, 0.40f, 3.00f, "%.2f m/s");

        ImGui::Checkbox("Charge Across + Punch", &m_vr_state.fight_charge_across_enabled);
        ImGui::Checkbox("Charge Up + Kick", &m_vr_state.fight_charge_up_enabled);
        ImGui::TextDisabled("Hold a hand out to one side to charge. The charge "
                            "holds that direction AND Down for real, so one held "
                            "position offers both: sweep across, or swing up. "
                            "A buzz tells you it is charged.");
        ImGui::SliderFloat("##chdist", &m_vr_state.fight_charge_distance, 0.15f, 0.60f, "%.2f m out");
        ImGui::SliderFloat("##chtime", &m_vr_state.fight_charge_seconds, 0.50f, 4.00f, "%.2f s held");
        ImGui::SliderFloat("##chspd", &m_vr_state.fight_charge_speed, 0.40f, 3.00f, "%.2f m/s sweep");

        ImGui::Checkbox("Normal Punch", &m_vr_state.fight_punch_enabled);
        ImGui::TextDisabled("Thrust ONE hand straight ahead.");
        ImGui::Checkbox("Normal Kick", &m_vr_state.fight_kick_enabled);
        ImGui::TextDisabled("Thrust ONE hand angled downward.");
        ImGui::Checkbox("Heavy Variants", &m_vr_state.fight_heavy_enabled);
        ImGui::TextDisabled("Leave the arm extended for the heavy version. "
                            "Turning this OFF makes light attacks fire instantly "
                            "with no decide delay at all.");
        ImGui::BeginDisabled(!m_vr_state.fight_punch_enabled && !m_vr_state.fight_kick_enabled);
        ImGui::SliderFloat("##npspd", &m_vr_state.fight_punch_speed, 0.40f, 3.00f, "%.2f m/s");
        ImGui::BeginDisabled(!m_vr_state.fight_heavy_enabled);
        ImGui::SliderFloat("##nphold", &m_vr_state.fight_hold_seconds, 0.06f, 0.40f, "%.2f s decide");
        ImGui::EndDisabled();
        ImGui::SliderFloat("##npkick", &m_vr_state.fight_kick_ratio, 0.20f, 1.50f, "%.2f down ratio");
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextDisabled("Sequence components - these are what the moves press.");
        draw_motion_binding(QI_FIGHT_UP,    "UP");
        draw_motion_binding(QI_FIGHT_DOWN,  "DOWN");
        draw_motion_binding(QI_FIGHT_LEFT,  "LEFT");
        draw_motion_binding(QI_FIGHT_RIGHT, "RIGHT");
        draw_motion_binding(QI_FIGHT_PUNCH, "PUNCH");
        draw_motion_binding(QI_FIGHT_KICK,  "KICK");
        draw_motion_binding(QI_FIGHT_PUNCH_HARD, "PUNCH HARD");
        draw_motion_binding(QI_FIGHT_KICK_HARD,  "KICK HARD");
        ImGui::EndDisabled();
    }

    if (experimental_section("AirJump", "Air Jump / Crouch", &m_vr_state.air_jump_enabled)) {
        ImGui::BeginDisabled(!m_vr_state.air_jump_enabled);
        ImGui::TextWrapped("Bob your head up sharply to hold Jump, drop down "
                           "sharply to hold Crouch. Both keep holding while "
                           "your head stays away from its resting height, so "
                           "staying ducked keeps crouch down. Standing up "
                           "slowly never triggers anything.");
        ImGui::Text("Trigger Speed");
        ImGui::SliderFloat("##ajspeed", &m_vr_state.air_jump_speed, 0.30f, 2.50f, "%.2f m/s");
        ImGui::TextDisabled("How fast your head must move. Higher ignores "
                            "fidgeting and nodding.");
        ImGui::Text("Hold Distance");
        ImGui::SliderFloat("##ajhold", &m_vr_state.air_jump_hold_margin, 0.02f, 0.30f, "%.2f m");
        ImGui::TextDisabled("How far from resting height still counts as held.");
        ImGui::Spacing();
        ImGui::Text("Now holding:");
        ImGui::SameLine();
        if (m_air_jump.jump)        ImGui::TextColored(ImVec4(0.35f,1.0f,0.45f,1.0f), "JUMP");
        else if (m_air_jump.crouch) ImGui::TextColored(ImVec4(0.35f,1.0f,0.45f,1.0f), "CROUCH");
        else                        ImGui::TextDisabled("(resting)");
        draw_motion_binding(QI_AIR_JUMP,   "JUMP");
        draw_motion_binding(QI_AIR_CROUCH, "CROUCH");
        ImGui::EndDisabled();
    }

    // The lightgun is feature-complete but still lives here as the other half
    // of Motion Controls; the checkbox drives the same m_gun_manual_override /
    // m_gun_hand transition the "Force Lightgun Mode" row performs.
    // Both this section header and draw_lightgun_group()'s own "Force Lightgun
    // Mode" checkbox drive m_gun_manual_override. Snapshot it first so we can
    // tell which one the user touched: syncing unconditionally from the header
    // copy wrote a stale value back over whatever the inner checkbox had just
    // set, which made Force Lightgun Mode inert inside this section.
    const bool gun_was = m_gun_manual_override;
    bool gun_on = m_gun_manual_override;
    if (experimental_section("Lightgun", "Lightgun", &gun_on)) {
        ImGui::BeginDisabled(!m_gun_manual_override);
        draw_lightgun_group();
        ImGui::EndDisabled();
    }
    const bool header_changed = (gun_on != gun_was);
    const bool group_changed = (m_gun_manual_override != gun_was);
    if (header_changed || group_changed) {
        // The header wins only when it is the one that moved; otherwise keep
        // what the group already applied.
        m_gun_manual_override = header_changed ? gun_on : m_gun_manual_override;
        m_gun_capable = m_gun_capable_auto || m_gun_manual_override;
        if (!m_gun_capable) {
            m_gun_hand = 0;
        } else if (m_gun_hand == 0) {
            m_gun_hand = 1; // default to right hand, as Force Lightgun Mode does
        }
        m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
        EmuSetGunMode gun_mode_fn;
        { std::lock_guard<std::mutex> lk(m_mutex); gun_mode_fn = m_gun_mode_ctrl; }
        if (gun_mode_fn) gun_mode_fn(m_gun_capable);
        fire_haptic(true, 0.7f, 150);
    }
}

// draw_lightgun_group — Controls > Lightgun. Every real lightgun option found
// in the codebase, not just calibration: Force Lightgun Mode
// (m_gun_manual_override, otherwise only auto-detected via
// rom_is_lightgun_capable()), Hand (m_gun_hand: Off/Right/Left, otherwise only
// cycled via a left-thumbstick-click chord), Gun Model (m_vr_state.gun_model:
// Pistol/Low-poly/Scope, the actual on-screen overlay shape — otherwise only in
// the old Settings panel), Shot Vibration (m_vr_state.gun_vibration_mode:
// Off/Recoil/Machinegun/Revolver), and Recenter Aim (m_gun_recenter_quat,
// otherwise only via an in-game trigger-hold gesture),
// via an in-game trigger-hold gesture), and the pre-existing five-point
// calibration. Each toggle/cycle here replicates the exact same state
// transitions their original controller-chord/old-panel equivalents perform
// (see poll_actions()'s left-thumbstick-click handling around gun_manual_override
// and gun_hand), not a simplified reimplementation.
// ============================================================
void OpenXrShell::draw_lightgun_group() {
    if (ImGui::Checkbox("Force Lightgun Mode", &m_gun_manual_override)) {
        m_gun_capable = m_gun_capable_auto || m_gun_manual_override;
        if (!m_gun_capable) {
            m_gun_hand = 0;
        } else if (m_gun_hand == 0) {
            m_gun_hand = 1; // default to right hand
        }
        m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
        EmuSetGunMode gun_mode_fn;
        { std::lock_guard<std::mutex> lk(m_mutex); gun_mode_fn = m_gun_mode_ctrl; }
        if (gun_mode_fn) gun_mode_fn(m_gun_capable);
        fire_haptic(true, 0.7f, 150);
    }
    // Custom-drawn group (not a kRows RowDef), so it doesn't get a star for
    // free the way generic rows do — draw one explicitly. The Favorites tab
    // special-cases this same "Controls|Lightgun|" key prefix to re-draw the
    // whole group there (see s_active_tab == -1 below), since there's no
    // single-row RowDef to redraw in isolation.
    draw_favorite_star("Controls|Lightgun|Force Lightgun Mode");
    ImGui::TextDisabled("Auto-detected: %s", m_gun_capable_auto ? "Yes (this ROM)" : "No");

    if (!m_gun_capable) {
        ImGui::TextDisabled("Enable Force Lightgun Mode (or load a ROM this app recognizes) first.");
        return;
    }

    ImGui::Text("Hand");
    static const char* kHandOpts[3] = {"Off", "Right", "Left"};
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    ImGui::PushID("LightgunHand");
    for (int i = 0; i < 3; ++i) {
        const bool selected = (m_gun_hand == i);
        const float w = ImGui::CalcTextSize(kHandOpts[i]).x + pad.x * 2.0f;
        if (ImGui::Selectable(kHandOpts[i], selected, 0, ImVec2(w, 0)) && m_gun_hand != i) {
            m_gun_hand = i;
            m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
            fire_haptic(true, 0.25f, 30);
        }
        if (i + 1 < 3) ImGui::SameLine();
    }
    ImGui::PopID();

    ImGui::Text("Gun Model");
    static const char* kModelOpts[3] = {"Pistol", "Low-poly", "Scope"};
    const int cur_model = std::clamp(m_vr_state.gun_model, 0, 2);
    ImGui::PushID("LightgunModel");
    for (int i = 0; i < 3; ++i) {
        const bool selected = (cur_model == i);
        const float w = ImGui::CalcTextSize(kModelOpts[i]).x + pad.x * 2.0f;
        if (ImGui::Selectable(kModelOpts[i], selected, 0, ImVec2(w, 0))) m_vr_state.gun_model = i;
        if (i + 1 < 3) ImGui::SameLine();
    }
    ImGui::PopID();

    ImGui::Text("Shot Vibration");
    ImGui::PushID("LightgunShotVibration");
    for (int i = 0; i < VrState::kGunVibrationModeCount; ++i) {
        const char* opt = VrState::kGunVibrationModeNames[i];
        const bool selected = (m_vr_state.gun_vibration_mode == i);
        const float w = ImGui::CalcTextSize(opt).x + pad.x * 2.0f;
        if (ImGui::Selectable(opt, selected, 0, ImVec2(w, 0)) && !selected) {
            m_vr_state.gun_vibration_mode = i;
            fire_haptic(true, 0.25f, 30);
        }
        if (i + 1 < VrState::kGunVibrationModeCount) ImGui::SameLine();
    }
    ImGui::PopID();
    ImGui::TextDisabled("Trigger pull vibration: Off, a sharp rifle-style recoil, an automatic burst, or a revolver-style rise/full kick/decay. Patterns use full controller strength.");

    ImGui::Checkbox("On-Screen Reload Hold", &m_vr_state.gun_offscreen_reload_enabled);
    ImGui::TextDisabled("Aim onto the screen to hold reload/hide; aim off any edge to release. Adds to the reload button, doesn't replace it. Useful for Time Crisis and similar games.");
    if (m_vr_state.gun_offscreen_reload_enabled) {
        ImGui::Text("Screen Reload Target");
        const float wrap_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        for (int i = 0; i < VrState::kGunOffscreenReloadButtonCount; ++i) {
            const char* opt = VrState::kGunOffscreenReloadButtonNames[i];
            const bool selected = (m_vr_state.gun_offscreen_reload_button == i);
            const float w = ImGui::CalcTextSize(opt).x + pad.x * 2.0f;
            if (ImGui::Selectable(opt, selected, 0, ImVec2(w, 0))) m_vr_state.gun_offscreen_reload_button = i;
            if (i + 1 < VrState::kGunOffscreenReloadButtonCount) {
                const char* next_opt = VrState::kGunOffscreenReloadButtonNames[i + 1];
                const float next_w = ImGui::CalcTextSize(next_opt).x + pad.x * 2.0f;
                const float next_x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w;
                if (next_x2 < wrap_x2) ImGui::SameLine();
            }
        }
        ImGui::TextDisabled("Lightgun Reload = standard reload signal (most games). Pick a button instead only if this game's reload/hide is a normal button.");
    }

    if (m_gun_hand == 0) {
        ImGui::TextDisabled("Select a hand above to calibrate/recenter.");
        return;
    }
    // Dual wielding: a second gun on whichever port this system keeps one on,
    // so two-player gun titles see a real player two -- each controller aiming
    // its own gun with its own trigger and Start. Offered only where the core
    // actually has a second gun device (backend_supports_dual_gun): PSX,
    // Saturn and MAME gun titles, plus SNES Justifier games. The Super Scope
    // and the NES Zapper have no two-player mode to expose.
    ImGui::BeginDisabled(!m_dual_gun_supported);
    if (ImGui::Checkbox("Two Guns (dual wield)", &m_dual_gun_enabled)) {
        EmuSetDualGunMode dual_fn;
        { std::lock_guard<std::mutex> lk(m_mutex); dual_fn = m_dual_gun_mode_ctrl; }
        if (dual_fn) dual_fn(m_dual_gun_enabled);
        fire_haptic(true, 0.7f, 150);
    }
    ImGui::EndDisabled();
    if (!m_dual_gun_supported) {
        ImGui::TextDisabled("This system has no second gun port for this game.");
    }
    draw_favorite_star("Controls|Lightgun|Two Guns");

    // Player two's own model and kick. Only meaningful with Two Guns on, so it
    // is greyed rather than hidden -- you can see what the second hand would
    // get before committing to enabling it.
    ImGui::BeginDisabled(!m_dual_gun_enabled);
    ImGui::TextDisabled("Player Two");
    ImGui::Text("Gun Model");
    ImGui::PushID("Lightgun2Model");
    {
        const ImVec2 pad2 = ImGui::GetStyle().FramePadding;
        static const char* kModelOpts2[3] = {"Pistol", "Low-poly", "Scope"};
        const int cur2 = std::clamp(m_vr_state.gun2_model, 0, 2);
        for (int i = 0; i < 3; ++i) {
            const float w = ImGui::CalcTextSize(kModelOpts2[i]).x + pad2.x * 2.0f;
            if (ImGui::Selectable(kModelOpts2[i], cur2 == i, 0, ImVec2(w, 0)))
                m_vr_state.gun2_model = i;
            if (i + 1 < 3) ImGui::SameLine();
        }
    }
    ImGui::PopID();
    ImGui::Text("Shot Vibration");
    ImGui::PushID("Lightgun2Vibration");
    {
        const ImVec2 pad2 = ImGui::GetStyle().FramePadding;
        for (int i = 0; i < VrState::kGunVibrationModeCount; ++i) {
            const char* opt = VrState::kGunVibrationModeNames[i];
            const bool sel = (m_vr_state.gun2_vibration_mode == i);
            const float w = ImGui::CalcTextSize(opt).x + pad2.x * 2.0f;
            if (ImGui::Selectable(opt, sel, 0, ImVec2(w, 0)) && !sel) {
                m_vr_state.gun2_vibration_mode = i;
                // Preview on the hand that will actually feel it.
                fire_haptic(m_gun_hand != 1, 0.25f, 30);
            }
            if (i + 1 < VrState::kGunVibrationModeCount) ImGui::SameLine();
        }
    }
    ImGui::PopID();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Player 2 uses the other controller. Calibrate each gun separately.");

    ImGui::TextWrapped("Five points correct controller aim drift: top-left, top-right, bottom-right, bottom-left, then center.");
    // Calibration profiles are keyed by hand, so calibrating gun two is the
    // same five-point flow run with the other hand as the active one. Swapping
    // m_gun_hand for the duration reuses the whole capture path rather than
    // duplicating that state machine, and it is restored when the flow ends.
    if (ImGui::Button(m_dual_gun_enabled ? "Calibrate Gun 1" : "Calibrate Lightgun")) {
        m_gun_calibration_restore_hand = 0;
        begin_lightgun_calibration();
    }
    if (m_dual_gun_enabled) {
        ImGui::SameLine();
        if (ImGui::Button("Calibrate Gun 2")) {
            // Aim with the other controller for this run, then hand back.
            m_gun_calibration_restore_hand = m_gun_hand;
            m_gun_hand = (m_gun_hand == 1) ? 2 : 1;
            begin_lightgun_calibration();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Hold steady; release trigger between points.");
    if (ImGui::Button("Recenter Aim")) {
        // Same reset the in-game trigger-hold recenter gesture performs
        // (poll_actions()'s recenter_btn handling) — clears any accumulated
        // aim offset back to identity.
        m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
        fire_haptic(true, 0.4f, 60);
    }
}

// ============================================================
// draw_save_state_group — Save States > Slots. Real ImGui port of the old
// bitmap-canvas "Save States" panel (renderSaveStatePanelBitmap in
// QuestVrActivity.kt, click handler at m_laser_panel == k_panel_save_state
// above) — same underlying slot logic (refresh_save_state_slots(),
// save_state_to_slot()/load_state_from_slot(), m_autosave_interval_seconds,
// m_load_last_save_enabled), just drawn as a real ImGui window instead of a
// hand-rasterized bitmap.
// ============================================================
void OpenXrShell::draw_save_state_group() {
    refresh_save_state_slots();

    const bool has_rom = !m_current_rom_name.empty();
    if (has_rom) {
        ImGui::TextWrapped("%s", m_current_rom_name.c_str());
    } else {
        ImGui::TextDisabled("No ROM loaded - load a game to save or load a state.");
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(!has_rom);
    ImGui::Text("Load");
    for (int i = 0; i < k_save_state_slot_count; ++i) {
        ImGui::PushID(i);
        const bool occupied = i < (int)m_save_state_slots.size() && m_save_state_slots[i].occupied;
        const std::string label = i < (int)m_save_state_slots.size() ? m_save_state_slots[i].label
                                                                       : ("Slot " + std::to_string(i + 1));
        ImGui::BeginDisabled(!occupied);
        if (ImGui::Button(label.c_str(), ImVec2(180.0f, 44.0f))) {
            std::string err;
            if (load_state_from_slot(i, err)) {
                set_status("Loaded state " + std::to_string(i + 1) + ".");
                m_menu_open = false;
                m_ctrlmap_mode = false;
                m_active_sub_panel = 0;
                EmuFreezeCtrl freeze_fn;
                { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
                if (freeze_fn) freeze_fn(false);
                m_emu_frozen_display = false;
                fire_haptic(true, 0.45f, 50);
            } else {
                set_status("Load failed: " + err);
                fire_haptic(true, 0.2f, 20);
            }
        }
        ImGui::EndDisabled();
        if (i + 1 < k_save_state_slot_count) ImGui::SameLine();
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Text("Save");
    for (int i = 0; i < k_save_state_slot_count; ++i) {
        ImGui::PushID(100 + i);
        if (ImGui::Button(("Slot " + std::to_string(i + 1)).c_str(), ImVec2(180.0f, 44.0f))) {
            std::string err;
            if (save_state_to_slot(i, err)) {
                set_status("Saved state " + std::to_string(i + 1) + ".");
                fire_haptic(true, 0.35f, 40);
            } else {
                set_status("Save failed: " + err);
                fire_haptic(true, 0.2f, 20);
            }
        }
        if (i + 1 < k_save_state_slot_count) ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Same wrap-if-it-doesn't-fit pill-row pattern every other Cycle row in
    // this menu uses (Depth Mode, Ambilight Placement, ...), not a single
    // advance-on-click Button.
    ImGui::Text("Autosave Every");
    {
        static const int kIntervals[] = {0, 300, 60, 30, 5};
        constexpr int kIntervalCount = (int)(sizeof(kIntervals) / sizeof(kIntervals[0]));
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        const float star_reserve = ImGui::CalcTextSize("[*]").x + pad.x * 2.0f + 16.0f;
        const float wrap_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - star_reserve;
        for (int i = 0; i < kIntervalCount; ++i) {
            const std::string opt_str = autosave_interval_label(kIntervals[i]);
            const char* opt = opt_str.c_str();
            const bool selected = (m_autosave_interval_seconds == kIntervals[i]);
            const float w = ImGui::CalcTextSize(opt).x + pad.x * 2.0f;
            if (ImGui::Selectable(opt, selected, 0, ImVec2(w, 0))) {
                m_autosave_interval_seconds = kIntervals[i];
                m_last_autosave_time_ms = monotonic_time_ms();
                persist_save_automation_settings();
                set_status("Autosave every " + autosave_interval_label(m_autosave_interval_seconds) + ".");
                fire_haptic(true, 0.3f, 30);
            }
            if (i + 1 < kIntervalCount) {
                const std::string next_opt = autosave_interval_label(kIntervals[i + 1]);
                const float next_w = ImGui::CalcTextSize(next_opt.c_str()).x + pad.x * 2.0f;
                const float next_x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w;
                if (next_x2 < wrap_x2) ImGui::SameLine();
            }
        }
    }
    if (ImGui::Checkbox("Load Last ROM", &m_load_last_rom_enabled)) {
        persist_save_automation_settings();
        set_status(std::string("Load last ROM ") + (m_load_last_rom_enabled ? "ON." : "OFF."));
        fire_haptic(true, 0.3f, 30);
    }
    ImGui::TextDisabled("When on, the app boots straight into your last-played ROM on launch instead of opening the menu.");
    ImGui::Spacing();
    if (ImGui::Checkbox("Load Last Save", &m_load_last_save_enabled)) {
        persist_save_automation_settings();
        set_status(std::string("Load last save ") + (m_load_last_save_enabled ? "ON." : "OFF."));
        fire_haptic(true, 0.3f, 30);
    }
    ImGui::TextDisabled("When on, opening a ROM auto-loads its most recent save-state slot.");
}

// ============================================================
// draw_presets_group — System > Presets. Real m_presets (5 slots, seeded by
// make_default_vr_presets() in vr_state.h — a full VrState snapshot per slot,
// NOT the emulator save-state system, which is a separate m_save_state_slots/
// k_save_state_slot_count concept). load_preset()/save_preset() are the
// existing real member functions (already used elsewhere, e.g. the old
// Settings panel) — this just gives the new menu a real UI over them.
// ============================================================
void OpenXrShell::draw_presets_group() {
    static const char* kPresetNames[5] = {
        "Balanced", "Compressed Depth", "Exaggerated Depth", "3D Layers + Extrusion", "Crisp Upscale + Ambilight"
    };
    for (int i = 0; i < (int)m_presets.size(); ++i) {
        ImGui::PushID(i);
        ImGui::Text("%s", (i < 5) ? kPresetNames[i] : "Preset");
        ImGui::SameLine();
        if (ImGui::SmallButton("Load")) load_preset(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save")) save_preset(i);
        ImGui::PopID();
    }
}

// ============================================================
// draw_danger_zone_group — System > Danger Zone > "Wipe All Settings". Real
// file count/size (scans get_settings_dir() for .ini files) and a real,
// two-click-confirmed delete — arms on the first click ("Wipe" -> "Confirm?"),
// only deletes on a second click while still armed, and disarms automatically
// if you navigate away (s_armed is reset whenever this isn't the frame right
// after arming — see the frame-id check below) so a stray click days later
// can't land on an already-armed button.
// ============================================================
namespace {
// Recursively walks `dir`, adding every regular file's path to `out_files`
// and its size to `out_bytes`. Used both to report a wipe's live count/size
// and (via delete_paths below) to actually perform it.
void scan_dir_recursive(const std::string& dir, std::vector<std::string>& out_files, long long& out_bytes) {
    DIR* dh = opendir(dir.c_str());
    if (!dh) return;
    struct dirent* ent;
    while ((ent = readdir(dh)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(full, out_files, out_bytes);
        } else {
            out_files.push_back(full);
            out_bytes += st.st_size;
        }
    }
    closedir(dh);
}

int delete_paths(const std::vector<std::string>& paths) {
    int deleted = 0;
    for (const std::string& p : paths) if (remove(p.c_str()) == 0) deleted++;
    return deleted;
}

// One arm/confirm wipe row: shows a live "N files - X.X KB" label, arms on
// the first click ("Wipe" -> "Confirm?"), and only fires `on_confirm` on a
// second click within 4s of arming (auto-disarms after that so a stray click
// days later can't land on an armed button).
void draw_wipe_row(const char* label, int file_count, long long total_bytes,
                    const std::function<void()>& on_confirm) {
    ImGui::PushID(label);
    const double kb = total_bytes / 1024.0;
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", label);
    ImGui::SameLine();
    if (kb >= 1024.0) {
        ImGui::TextDisabled("%d file%s - %.1f MB", file_count, file_count == 1 ? "" : "s", kb / 1024.0);
    } else {
        ImGui::TextDisabled("%d file%s - %.1f KB", file_count, file_count == 1 ? "" : "s", kb);
    }

    static std::map<std::string, std::chrono::steady_clock::time_point> s_armed_times;
    const std::string key = label;
    auto it = s_armed_times.find(key);
    bool armed = it != s_armed_times.end();
    if (armed && std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count() > 4.0f) {
        s_armed_times.erase(it);
        armed = false;
    }

    if (ImGui::Button(armed ? "Confirm? This can't be undone" : "Wipe")) {
        if (!armed) {
            s_armed_times[key] = std::chrono::steady_clock::now();
        } else {
            on_confirm();
            s_armed_times.erase(key);
        }
    }
    ImGui::PopID();
}
} // namespace

void OpenXrShell::draw_danger_zone_group() {
    const std::string dir = get_settings_dir();
    static const char* kBackendSubdirs[] = {"snes", "genesis", "nes", "gba", "gb", "pce", "sms"};

    // Profiles: every per-game/global .ini under the settings dir (not recursive
    // into savestates/ — those are counted separately below).
    std::vector<std::string> ini_paths;
    long long ini_bytes = 0;
    if (!dir.empty()) {
        std::vector<std::string> to_scan = {dir};
        for (const char* sub : kBackendSubdirs) to_scan.push_back(dir + "/" + sub);
        for (const std::string& d : to_scan) {
            DIR* dh = opendir(d.c_str());
            if (!dh) continue;
            struct dirent* ent;
            while ((ent = readdir(dh)) != nullptr) {
                const std::string name = ent->d_name;
                if (name.size() < 4 || name.substr(name.size() - 4) != ".ini") continue;
                const std::string full = d + "/" + name;
                struct stat st{};
                if (stat(full.c_str(), &st) == 0) {
                    ini_bytes += st.st_size;
                    ini_paths.push_back(full);
                }
            }
            closedir(dh);
        }
    }

    // Save states/save games: <settings>/<backend>/savestates/** for every backend.
    std::vector<std::string> save_state_files;
    long long save_state_bytes = 0;
    if (!dir.empty()) {
        for (const char* sub : kBackendSubdirs) {
            scan_dir_recursive(dir + "/" + sub + "/savestates", save_state_files, save_state_bytes);
        }
    }

    // Temporary uncompressed files extracted from zipped ROMs live in the
    // app's Kotlin-owned cache dir, not the settings dir — query Kotlin.
    ExtractedRomCacheStats stats_fn;
    ExtractedRomCacheClearer clearer_fn;
    { std::lock_guard<std::mutex> lk(m_mutex);
      stats_fn = m_extracted_rom_cache_stats;
      clearer_fn = m_extracted_rom_cache_clearer; }
    int extract_count = 0;
    long long extract_bytes = 0;
    if (stats_fn) { const auto s = stats_fn(); extract_count = s.first; extract_bytes = s.second; }

    draw_wipe_row("Wipe Profiles", (int)ini_paths.size(), ini_bytes, [this, ini_paths]() {
        const int deleted = delete_paths(ini_paths);
        // Deleting the .ini files alone leaves the live VrState/config exactly
        // as it was — reset it too so the menu reflects defaults immediately,
        // matching the old main menu's wipe_all_settings() behaviour instead
        // of requiring a relaunch to see anything change.
        reset_settings();
        set_status("Wiped " + std::to_string(deleted) + " profile file" + (deleted == 1 ? "" : "s") +
                   "; live settings reset to defaults.");
    });
    ImGui::Spacing();
    draw_wipe_row("Wipe Uncompressed ROM Cache", extract_count, extract_bytes, [this, clearer_fn]() {
        if (clearer_fn) clearer_fn();
        set_status("Wiped the uncompressed-ROM cache.");
    });
    ImGui::Spacing();
    draw_wipe_row("Wipe Save States", (int)save_state_files.size(), save_state_bytes, [this, save_state_files]() {
        const int deleted = delete_paths(save_state_files);
        set_status("Wiped " + std::to_string(deleted) + " save state" + (deleted == 1 ? "" : "s"));
        refresh_save_state_slots();
    });
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int all_count = (int)ini_paths.size() + (int)save_state_files.size() + extract_count;
    const long long all_bytes = ini_bytes + save_state_bytes + extract_bytes;
    draw_wipe_row("Wipe All", all_count, all_bytes,
                  [this, ini_paths, save_state_files, clearer_fn]() {
        const int deleted = delete_paths(ini_paths) + delete_paths(save_state_files);
        if (clearer_fn) clearer_fn();
        // Same live-reset as "Wipe Profiles" above — see its comment.
        reset_settings();
        set_status("Wiped everything: " + std::to_string(deleted) + " file" + (deleted == 1 ? "" : "s") +
                   " plus the uncompressed-ROM cache; live settings reset to defaults.");
        refresh_save_state_slots();
    });
}

void OpenXrShell::draw_exit_group() {
    ImGui::TextWrapped("Quit QuestRetroDepth and return to the Quest home.");
    ImGui::Spacing();
    AppExiter exiter_fn;
    { std::lock_guard<std::mutex> lk(m_mutex); exiter_fn = m_app_exiter; }
    // The surrounding collapsing header is also named "Exit". Keep the
    // visible caption, but add a hidden suffix so ImGui does not assign both
    // widgets the same ID in this window.
    if (ImGui::Button("Exit##ExitAppButton", ImVec2(160.0f, 44.0f))) {
        if (exiter_fn) exiter_fn();
    }
}

// ============================================================
// draw_audio_channels_group — Audio > Channels. Real per-core ROM audio
// channel splitting: each backend mixes its own sound chip(s) internally
// (see e.g. snes_set_channel_volume/genesis_set_channel_volume/etc.), so
// this just edits the stored sliders and re-pushes them whenever anything
// changes. The master toggle is the safety net: off means every channel is
// forced to 1.0 in apply_audio_channel_volumes() regardless of what the
// sliders below say, so audio is exactly what it always was.
// ============================================================
namespace {
// One core's block of per-channel sliders. Returns true if any slider moved.
bool draw_channel_volume_sliders(const char* core_label, float* volumes, const char* const* labels, int count, bool enabled) {
    bool changed = false;
    ImGui::TextDisabled("%s", core_label);
    ImGui::BeginDisabled(!enabled);
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        if (ImGui::SliderFloat(labels[i], &volumes[i], 0.0f, 1.0f)) changed = true;
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    return changed;
}
} // namespace

void OpenXrShell::draw_audio_channels_group() {
    ImGui::TextWrapped("Split each core's ROM audio into its individual sound-chip channels, "
                        "so music/SFX/voice can be balanced independently of each other.");
    ImGui::Spacing();
    bool any_changed = false;
    if (ImGui::Checkbox("Split ROM Audio", &m_vr_state.audio_channel_split_enabled)) any_changed = true;
    ImGui::TextDisabled("Off = exactly today's behavior, regardless of the sliders below.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool en = m_vr_state.audio_channel_split_enabled;
    static const char* kSnesLabels[8]    = {"Voice 1","Voice 2","Voice 3","Voice 4","Voice 5","Voice 6","Voice 7","Voice 8"};
    static const char* kGenesisLabels[2] = {"FM (Music)","PSG (Square/Noise)"};
    static const char* kNesLabels[5]     = {"Pulse 1","Pulse 2","Triangle","Noise","DMC"};
    static const char* kGbaLabels[3]     = {"PSG (Classic)","Direct Sound A","Direct Sound B"};
    static const char* kPceLabels[6]     = {"Channel 1","Channel 2","Channel 3","Channel 4","Channel 5","Channel 6"};

    if (draw_channel_volume_sliders("SNES - SPC700 Voices", m_vr_state.snes_voice_volume, kSnesLabels, 8, en)) any_changed = true;
    ImGui::Spacing();
    if (draw_channel_volume_sliders("Genesis / SMS / Game Gear", m_vr_state.genesis_channel_volume, kGenesisLabels, 2, en)) any_changed = true;
    ImGui::Spacing();
    if (draw_channel_volume_sliders("NES - APU Channels", m_vr_state.nes_channel_volume, kNesLabels, 5, en)) any_changed = true;
    ImGui::Spacing();
    if (draw_channel_volume_sliders("Game Boy / GBA", m_vr_state.gba_channel_volume, kGbaLabels, 3, en)) any_changed = true;
    ImGui::Spacing();
    if (draw_channel_volume_sliders("PC Engine - PSG Channels", m_vr_state.pce_channel_volume, kPceLabels, 6, en)) any_changed = true;
    ImGui::Spacing();
    ImGui::TextDisabled("MAME arcade audio isn't included - its sound hardware varies too much "
                         "per game for a generic per-channel split.");

    if (any_changed) apply_audio_channel_volumes();
}

// ============================================================
// draw_frame_skip_group — Visuals > Frame Skip. One checkbox per core that
// actually has a frameskip hook (see set_auto_frame_skip() in each
// *_backend.cpp) — NES/FCEUmm has none at all, so it's omitted entirely
// rather than shown disabled with nothing to explain.
// ============================================================
void OpenXrShell::draw_frame_skip_group() {
    ImGui::TextWrapped("Let each core skip frames under load to keep speed up, independently per core.");
    ImGui::Spacing();
    bool any_changed = false;
    if (ImGui::Checkbox("SNES", &m_vr_state.auto_frame_skip_snes)) any_changed = true;
    if (ImGui::Checkbox("Genesis / SMS / Game Gear", &m_vr_state.auto_frame_skip_genesis)) any_changed = true;
    if (ImGui::Checkbox("MAME (Arcade)", &m_vr_state.auto_frame_skip_mame)) any_changed = true;
    if (ImGui::Checkbox("Saturn", &m_vr_state.auto_frame_skip_saturn)) any_changed = true;
    if (ImGui::Checkbox("PC Engine", &m_vr_state.auto_frame_skip_pce)) any_changed = true;
    ImGui::TextDisabled("PC Engine's is genuinely adaptive (audio-buffer driven).");
    if (ImGui::Checkbox("Game Boy / GBA", &m_vr_state.auto_frame_skip_gba)) any_changed = true;
    ImGui::TextDisabled("Game Boy/GBA has no adaptive mode -- this fixes it at a skip-2 rate (~20fps) instead.");
    ImGui::Spacing();
    ImGui::TextDisabled("NES has no frameskip option in its core at all, so there's no toggle for it.");

    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    auto pill_width = [&](const char* opt) {
        return ImGui::CalcTextSize(opt).x + pad.x * 2.0f;
    };

    // Eye-buffer resolution, right next to the emulator's internal resolution
    // because the two trade against each other: the internal scale sharpens
    // what the emulator draws, this sharpens everything the headset actually
    // shows, and on a 256x224 source the second one is usually what you feel.
    // Discrete pills rather than only the Visuals > Performance slider, so it
    // can be A/B'd quickly mid-game; both write the same VrState field.
    ImGui::Spacing();
    ImGui::Text("VR Resolution Scale");
    static constexpr float k_vr_res_scales[] = {0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    static constexpr const char* k_vr_res_labels[] = {"0.75x", "1x", "1.25x", "1.5x", "1.75x", "2x"};
    ImGui::PushID("VrResolutionScale");
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool selected =
            std::abs(m_vr_state.vr_resolution_scale - k_vr_res_scales[i]) < 0.001f;
        if (ImGui::Selectable(k_vr_res_labels[i], selected, 0,
                              ImVec2(pill_width(k_vr_res_labels[i]), 0))) {
            m_vr_state.vr_resolution_scale = k_vr_res_scales[i];
            apply_vr_resolution_scale();
        }
    }
    ImGui::PopID();
    ImGui::TextDisabled("Renders each eye above/below the headset's recommended size. Applies immediately; above 1.5x expect frame drops on Quest 2.");

    if (any_changed) apply_auto_frame_skip();
}

// ============================================================
// draw_psx_group -- Experimental > PSX. Every PSX-specific control in one
// place: the renderer path (a kRows row), SwanStation's internal GPU
// resolution and its texture upscaler. The last two used to sit in
// Visuals > Frame Skip, which is where they landed for their performance
// cost rather than because they belonged there.
// ============================================================
void OpenXrShell::draw_psx_group() {
    const bool psx_active = m_current_backend_kind == BackendKind::Psx;
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    auto pill_width = [&](const char* opt) {
        return ImGui::CalcTextSize(opt).x + pad.x * 2.0f;
    };
    draw_group_rows(&m_impl->debug_show_new_ui, &m_vr_state, "Experimental", "PSX");
    // SwanStation's internal GPU resolution: trades image quality for CPU/GPU
    // cost, changeable while the PSX core is active.
    ImGui::Text("PSX GPU Resolution");
    static constexpr int k_psx_resolutions[] = {1, 2, 4, 8, 16};
    static constexpr const char* k_psx_resolution_labels[] = {"1x", "2x", "4x", "8x", "16x"};
    // Both this row and the VR Resolution Scale row below label their pills
    // "1x"/"2x", and an ImGui widget's id is its label within the current id
    // scope -- without a scope per row they collide and ImGui asserts.
    ImGui::PushID("PsxGpuResolution");
    for (int i = 0; i < 5; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool selected = m_vr_state.psx_gpu_resolution == k_psx_resolutions[i];
        ImGui::BeginDisabled(!psx_active || i >= 3);
        if (ImGui::Selectable(k_psx_resolution_labels[i], selected, 0,
                              ImVec2(pill_width(k_psx_resolution_labels[i]), 0))) {
            m_vr_state.psx_gpu_resolution = k_psx_resolutions[i];
            apply_psx_gpu_resolution();
    apply_psx_texture_filter();
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    ImGui::TextDisabled("Higher values improve 3D detail but increase software-renderer load. 8x and 16x are disabled on Quest.");
    if (!psx_active) ImGui::TextDisabled("Load a PSX ROM to change this setting.");

    // SwanStation's own texture upscaler. This is the knob for "the textures
    // look chunky" -- the resolution scale above only sharpens polygon edges
    // and rasterisation, because PS1 textures are stored at their native size
    // and simply get magnified. Its effect grows with the resolution scale,
    // and it applies live (the core rebuilds its shaders between frames).
    ImGui::Spacing();
    ImGui::Text("PSX Texture Filtering");
    ImGui::PushID("PsxTextureFilter");
    for (int i = 0; i < VrState::kPsxTextureFilterCount; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool selected = m_vr_state.psx_texture_filter == i;
        ImGui::BeginDisabled(!psx_active);
        if (ImGui::Selectable(VrState::kPsxTextureFilterNames[i], selected, 0,
                              ImVec2(pill_width(VrState::kPsxTextureFilterNames[i]), 0))) {
            m_vr_state.psx_texture_filter = i;
            apply_psx_texture_filter();
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    ImGui::TextDisabled("Smooths magnified textures. xBR and JINC2 cost the most GPU; \"NB\" variants skip edge blending, which avoids halos around sprites.");

}

void OpenXrShell::draw_unified_menu() {
    g_psx_row_backend_active = (m_current_backend_kind == BackendKind::Psx);
    struct PsxRenderPathFlush {
        OpenXrShell* shell;
        ~PsxRenderPathFlush() {
            if (!g_psx_row_path_changed) return;
            g_psx_row_path_changed = false;
            shell->apply_psx_render_path();
        }
    } psx_render_path_flush{this};
    struct VrResScaleFlush {
        OpenXrShell* shell;
        ~VrResScaleFlush() {
            if (!g_vr_res_scale_changed) return;
            g_vr_res_scale_changed = false;
            shell->apply_vr_resolution_scale();
        }
    } vr_res_scale_flush{this};

    static int  s_active_tab = -1; // -1 = Favorites, else index into kUnifiedMenuTabs
    static bool s_favorites_seeded = false;
    if (!s_favorites_seeded) {
        // Restore favorites saved in a previous session (populated into
        // m_vr_state.menu_favorites_csv by whatever load_settings() call already
        // ran at startup — see settings_io.h's "menu_favorites_csv" read branch).
        std::stringstream csv(m_vr_state.menu_favorites_csv);
        std::string tok;
        while (std::getline(csv, tok, ';')) {
            if (!tok.empty()) s_favorites.insert(tok);
        }
        // Land on Favorites first whenever anything's pinned, same as the mockup.
        s_active_tab = s_favorites.empty() ? 0 : -1;
        s_favorites_seeded = true;
    }

    // Title bar — matches the old Kotlin main menu's branding (RetroDepth with
    // a red R / green D, build label right-aligned) so the new menu doesn't
    // read as a random unbranded replacement.
    {
        static const char kTitle[] = "RetroDepth";
        // Kept in sync by hand with QuestVrActivity.kt's MAIN_MENU_BUILD_LABEL —
        // there's no shared source for this string across the JNI boundary.
        static const char* k_build_label = "b009 - Trigger Happy";
        ImGui::SetWindowFontScale(1.6f); // bigger title text than the rest of the menu
        for (const char* ch = kTitle; *ch; ++ch) {
            ImVec4 col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            if (*ch == 'R') col = ImVec4(0.92f, 0.24f, 0.24f, 1.0f);
            else if (*ch == 'D') col = ImVec4(0.27f, 0.86f, 0.39f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(ch, ch + 1);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 0.0f);
        }
        ImGui::SetWindowFontScale(1.0f); // back to normal for the build label + rest of the menu
        ImGui::NewLine();
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(k_build_label).x - 24.0f);
        ImGui::TextColored(ImVec4(0.47f, 0.82f, 1.0f, 0.8f), "%s", k_build_label);
        ImGui::Separator();
    }

    ImGui::BeginChild("##rail", ImVec2(180, 0), true);
    char fav_label[48];
    std::snprintf(fav_label, sizeof(fav_label), "[*] Favs (%d)", (int)s_favorites.size());
    if (ImGui::Selectable(fav_label, s_active_tab == -1)) s_active_tab = -1;
    ImGui::Separator();
    for (int i = 0; i < (int)std::size(kUnifiedMenuTabs); ++i) {
        if (ImGui::Selectable(kUnifiedMenuTabs[i].name, s_active_tab == i)) s_active_tab = i;
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Sidebar only takes up space while a ROM is actually being hovered/
    // previewed — showing it on every tab regardless of hover state (an
    // earlier version of this) made it look like a permanently-stuck panel
    // extension rather than a hover preview.
    constexpr float kPreviewSidebarW = 390.0f;
    const bool show_preview_sidebar = !m_library_preview_path.empty();
    bool bgm_toggle_changed = false;
    bool bgm_volume_changed = false;
    ImGui::BeginChild("##content", ImVec2(show_preview_sidebar ? -kPreviewSidebarW : 0, 0), true);
    if (s_active_tab == -1) {
        if (s_favorites.empty()) {
            ImGui::TextDisabled("No favorites pinned yet.");
            ImGui::TextWrapped("Star any control and it'll show up here.");
        } else {
            ImGui::Text("Favorites");
            ImGui::Separator();
            const int mame_mode_before_favorites = m_mame_composition_mode;
            for (const RowDef& r : kRows) {
                if (s_favorites.count(row_key(r))) draw_row(
                    &m_impl->debug_show_new_ui, &m_vr_state, r, /*show_provenance=*/true,
                    m_current_backend_kind == BackendKind::Mame ? &m_mame_composition_mode : nullptr,
                    m_mame_occupancy_eligible, &bgm_toggle_changed,
                    &bgm_volume_changed);
            }
            // Layer-scoped favorites: reconstructed from the key alone (not from
            // kRows, since layers are dynamic per game) — live and bound to the
            // real engine arrays if the favorited game+layer is currently loaded,
            // otherwise rendered disabled with a note, never silently misattached
            // to whatever happens to be loaded now. See draw_one_layer_control.
            for (const std::string& key : s_favorites) {
                if (key.rfind("LAYER::", 0) != 0) continue;
                const size_t bar = key.find('|');
                if (bar == std::string::npos) continue;
                const std::string head  = key.substr(7, bar - 7); // strip "LAYER::" prefix
                const std::string label = key.substr(bar + 1);
                const size_t sep = head.find("::");
                if (sep == std::string::npos) continue;
                const std::string game  = head.substr(0, sep);
                const std::string layer = head.substr(sep + 2);

                const LayerControlDef* def = nullptr;
                for (const LayerControlDef& d : kLayerControls) {
                    if (label == d.label) { def = &d; break; }
                }
                if (!def) continue; // stale — control template no longer exists

                int orig = -1;
                if (game == m_config.game) {
                    for (int o : m_layer_order) {
                        if (o >= 0 && o < (int)m_layer_names.size() && m_layer_names[o] == layer) { orig = o; break; }
                    }
                }
                draw_one_layer_control(game, layer, *def, orig,
                                        &m_layer_enabled, &m_layer_ambilight, &m_layer_side_color,
                                        /*disabled=*/false);
            }
            // Lightgun favorites: draw_lightgun_group() is hand-rolled (not
            // built from kRows RowDefs like most tabs), so there's no single
            // row to redraw here — star it once and show the whole group,
            // same as any other custom-drawn group would need to be handled.
            for (const std::string& key : s_favorites) {
                if (key.rfind("Controls|Lightgun|", 0) != 0) continue;
                ImGui::Text("Lightgun");
                ImGui::Separator();
                draw_lightgun_group();
                break;
            }
            // Refresh Hz / Experimental Rumble: also hand-drawn (not kRows
            // RowDefs, see the comments where each is drawn), so redraw the
            // one real row here rather than a whole group.
            if (s_favorites.count("Visuals|Display|Refresh Hz")) {
                ImGui::PushID("FavRefreshHz");
                ImGui::Text("Refresh Hz");
                if (m_impl && !m_impl->available_rates.empty()) {
                    float cur_disp = pick_default_refresh_rate(m_impl->available_rates);
                    if (m_desired_refresh_rate > 0.0f) {
                        float best_d = 1e9f;
                        for (float r : m_impl->available_rates)
                            if (std::fabs(r - m_desired_refresh_rate) < best_d) { best_d = std::fabs(r - m_desired_refresh_rate); cur_disp = r; }
                    }
                    const ImVec2 pad = ImGui::GetStyle().FramePadding;
                    for (size_t i = 0; i < m_impl->available_rates.size(); ++i) {
                        char label[16];
                        std::snprintf(label, sizeof(label), "%.0f Hz", m_impl->available_rates[i]);
                        const bool selected = std::fabs(m_impl->available_rates[i] - cur_disp) < 0.5f;
                        const float w = ImGui::CalcTextSize(label).x + pad.x * 2.0f;
                        if (ImGui::Selectable(label, selected, 0, ImVec2(w, 0))) {
                            m_desired_refresh_rate = m_impl->available_rates[i];
                            m_apply_refresh_pending = true;
                        }
                        if (i + 1 < m_impl->available_rates.size()) ImGui::SameLine();
                    }
                }
                draw_favorite_star("Visuals|Display|Refresh Hz");
                ImGui::PopID();
            }
            if (s_favorites.count("Controls|Haptics|Experimental Rumble")) {
                ImGui::PushID("FavExperimentalRumble");
                if (ImGui::Checkbox("Experimental Rumble", &m_experimental_rumble_enabled)) {
                    if (m_on_experimental_rumble_changed) m_on_experimental_rumble_changed(m_experimental_rumble_enabled);
                }
                draw_favorite_star("Controls|Haptics|Experimental Rumble");
                ImGui::PopID();
            }
            if (mame_mode_before_favorites != m_mame_composition_mode)
                set_mame_composition_mode(m_mame_composition_mode);
        }
    } else {
        const UnifiedMenuTab& tab = kUnifiedMenuTabs[s_active_tab];
        ImGui::Text("%s", tab.name);
        ImGui::Separator();
        if (strcmp(tab.name, "Experimental") == 0) draw_experimental_notice();
        const bool help_tab = (strcmp(tab.name, "Help") == 0);
        if (help_tab) {
            ImGui::TextWrapped("New here? Start with Getting Started. Open a topic to read it.");
            ImGui::Separator();
        }
        bool first_group = true;
        for (const UnifiedMenuGroup& g : tab.groups) {
            // Every other tab opens all its groups; Help is pure reading
            // material, so only the first topic is expanded — otherwise the
            // whole manual dumps out at once and nothing is skimmable.
            ImGuiTreeNodeFlags header_flags =
                (!help_tab || first_group) ? ImGuiTreeNodeFlags_DefaultOpen : 0;
            first_group = false;
            if (ImGui::CollapsingHeader(g.name, header_flags)) {
                if (help_tab) {
                    draw_help_group(g.name);
                } else if (strcmp(tab.name, "Layers") == 0 && strcmp(g.name, "Stack") == 0) {
                    draw_depth_arrangement_widget();
                    draw_layer_control_rows();
                } else if (strcmp(tab.name, "Layers") == 0 && strcmp(g.name, "Camera Position") == 0) {
                    draw_camera_position_group();
                } else if (strcmp(tab.name, "Layers") == 0 && strcmp(g.name, "Composition Mode") == 0) {
                    const int before = m_mame_composition_mode;
                    draw_group_rows(&m_impl->debug_show_new_ui, &m_vr_state, tab.name, g.name,
                                    m_current_backend_kind == BackendKind::Mame ? &m_mame_composition_mode : nullptr,
                                    m_mame_occupancy_eligible);
                    if (before != m_mame_composition_mode)
                        set_mame_composition_mode(m_mame_composition_mode);
                } else if (strcmp(tab.name, "System") == 0 && strcmp(g.name, "Config Files") == 0) {
                    draw_config_files_group();
                } else if (strcmp(tab.name, "Interface") == 0 && strcmp(g.name, "Theme") == 0) {
                    draw_theme_row();
                } else if (strcmp(tab.name, "Library") == 0 && strcmp(g.name, "Browse & Launch") == 0) {
                    draw_library_rom_list();
                } else if (strcmp(tab.name, "Controls") == 0 && strcmp(g.name, "Controller Map") == 0) {
                    draw_controls_group();
                } else if (strcmp(tab.name, "Experimental") == 0 && strcmp(g.name, "PSX") == 0) {
                    draw_psx_group();
                } else if (strcmp(tab.name, "Experimental") == 0 && strcmp(g.name, "Motion Controls") == 0) {
                    draw_motion_controls_group();
                } else if (strcmp(tab.name, "Saves") == 0 && strcmp(g.name, "Slots") == 0) {
                    draw_save_state_group();
                } else if (strcmp(tab.name, "System") == 0 && strcmp(g.name, "Presets") == 0) {
                    draw_presets_group();
                } else if (strcmp(tab.name, "System") == 0 && strcmp(g.name, "Danger Zone") == 0) {
                    draw_danger_zone_group();
                } else if (strcmp(tab.name, "System") == 0 && strcmp(g.name, "Exit") == 0) {
                    draw_exit_group();
                } else if (strcmp(tab.name, "Audio") == 0 && strcmp(g.name, "Channels") == 0) {
                    draw_audio_channels_group();
                } else if (strcmp(tab.name, "Visuals") == 0 && strcmp(g.name, "Frame Skip") == 0) {
                    draw_frame_skip_group();
                } else if (strcmp(tab.name, "Visuals") == 0 && strcmp(g.name, "Display") == 0) {
                    draw_group_rows(&m_impl->debug_show_new_ui, &m_vr_state, tab.name, g.name);
                    // Refresh Hz isn't a kRows row (needs m_impl->available_rates/
                    // m_desired_refresh_rate directly — see the comment by kRows'
                    // Display entries) — drawn inline here instead.
                    ImGui::PushID("RefreshHz");
                    ImGui::Text("Refresh Hz");
                    if (m_impl && !m_impl->available_rates.empty()) {
                        float cur_disp = pick_default_refresh_rate(m_impl->available_rates);
                        if (m_desired_refresh_rate > 0.0f) {
                            float best_d = 1e9f;
                            for (float r : m_impl->available_rates)
                                if (std::fabs(r - m_desired_refresh_rate) < best_d) { best_d = std::fabs(r - m_desired_refresh_rate); cur_disp = r; }
                        }
                        const ImVec2 pad = ImGui::GetStyle().FramePadding;
                        for (size_t i = 0; i < m_impl->available_rates.size(); ++i) {
                            char label[16];
                            std::snprintf(label, sizeof(label), "%.0f Hz", m_impl->available_rates[i]);
                            const bool selected = std::fabs(m_impl->available_rates[i] - cur_disp) < 0.5f;
                            const float w = ImGui::CalcTextSize(label).x + pad.x * 2.0f;
                            if (ImGui::Selectable(label, selected, 0, ImVec2(w, 0))) {
                                m_desired_refresh_rate = m_impl->available_rates[i];
                                m_apply_refresh_pending = true;
                            }
                            if (i + 1 < m_impl->available_rates.size()) ImGui::SameLine();
                        }
                    } else {
                        ImGui::TextDisabled("No alternate rates reported by this runtime.");
                    }
                    draw_favorite_star("Visuals|Display|Refresh Hz");
                    ImGui::PopID();
                } else if (strcmp(tab.name, "Controls") == 0 && strcmp(g.name, "Haptics") == 0) {
                    // Experimental Rumble isn't a kRows row (m_experimental_rumble_enabled/
                    // m_on_experimental_rumble_changed aren't VrState fields) — drawn
                    // inline here instead.
                    ImGui::PushID("ExperimentalRumble");
                    if (ImGui::Checkbox("Experimental Rumble", &m_experimental_rumble_enabled)) {
                        if (m_on_experimental_rumble_changed) m_on_experimental_rumble_changed(m_experimental_rumble_enabled);
                    }
                    draw_favorite_star("Controls|Haptics|Experimental Rumble");
                    ImGui::PopID();

                    // Real VrState::show_controller_models -- not a kRows row
                    // since this whole group is hand-drawn (see the branch
                    // above), same inline-checkbox pattern as Experimental
                    // Rumble just above.
                    ImGui::PushID("ShowControllerModels");
                    ImGui::Checkbox("Show Controller Models", &m_vr_state.show_controller_models);
                    draw_favorite_star("Controls|Haptics|Show Controller Models");
                    ImGui::PopID();
                } else if (strcmp(tab.name, "Credits") == 0 && strcmp(g.name, "Credits") == 0) {
                    draw_credits_group();
                } else if (!draw_group_rows(&m_impl->debug_show_new_ui, &m_vr_state, tab.name, g.name,
                                            nullptr, false, &bgm_toggle_changed,
                                            &bgm_volume_changed)) {
                    ImGui::TextDisabled("(placeholder - settings move in here next)");
                }
            }
        }
    }
    if (bgm_toggle_changed) call_activity_void(m_vr_state.bgm_enabled ? "bgmEnable" : "bgmDisable");
    if (bgm_volume_changed) call_activity_float("bgmSetVolume", m_vr_state.bgm_volume);

    // Leaving the Library tab (including into Favorites): clear the hover
    // metadata so a stale ROM name/size doesn't keep showing in the sidebar
    // on an unrelated tab, and tear down any live preview exactly like
    // leaving the 3D Shelf does.
    {
        const bool on_library_tab = (s_active_tab >= 0 && s_active_tab < (int)std::size(kUnifiedMenuTabs) &&
                                      strcmp(kUnifiedMenuTabs[s_active_tab].name, "Library") == 0);
        if (!on_library_tab && !m_library_preview_path.empty()) {
            if (m_library_live_preview_active) stop_library_live_preview();
            m_library_preview_path.clear();
            m_library_preview_name.clear();
            m_library_preview_system.clear();
            m_library_preview_size_str.clear();
            m_library_preview_uncompressed_size_str.clear();
            m_library_preview_is_archive = false;
            m_library_hover_dwell_path.clear();
            m_library_hover_dwell_start = -1.0f;
        }
        if (!on_library_tab) m_rom_search_keyboard_open = false;
    }
    ImGui::EndChild();

    update_library_live_preview();

    if (show_preview_sidebar) {
        ImGui::SameLine();
        ImGui::BeginChild("##preview_sidebar", ImVec2(0, 0), true);
        draw_rom_preview_sidebar();
        ImGui::EndChild();
    }
}

// ============================================================
// render_frame
// ============================================================
void OpenXrShell::render_frame(XrTime predicted_time) {
    if (!m_impl || m_impl->eye[0].swapchain == XR_NULL_HANDLE) return;

    // The PSX screen-layer toggle changes both the backend capture mode and
    // the app-side layer configuration. Apply it before fetching the next
    // emulator frame so the first frame after a toggle uses matching slots.

    // Background music: edge-detect on the two states that should drive it
    // rather than hooking every one of the many call sites that assign
    // m_menu_open / request_live/clear_live — this catches all of them for free.
    // kAutoPauseOnMenuEnabled is currently false (see below), so opening the
    // menu no longer freezes/silences a loaded ROM's own audio — only offer
    // bgm when there's no ROM loaded to already be making noise, otherwise
    // the two just play on top of each other.
    const bool bgm_should_play = m_menu_open && m_current_rom_name.empty();
    if (bgm_should_play != m_bgm_should_play_prev) {
        if (bgm_should_play) call_activity_void("bgmEnterMenu");
        else call_activity_void("bgmExitMenu");
        m_bgm_should_play_prev = bgm_should_play;
    }
    if (m_bgm_live_active != m_bgm_live_active_prev) {
        if (m_bgm_live_active) call_activity_void("bgmDuck");
        else call_activity_void("bgmUnduck");
        m_bgm_live_active_prev = m_bgm_live_active;
    }

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
    if (m_current_backend_kind == BackendKind::Mame) {
        m_mame_occupancy_available = m_cached_frame_out.mame_occupancy_available;
        m_mame_occupancy_valid = m_cached_frame_out.mame_occupancy_valid;
        m_mame_occupancy_eligible =
            m_cached_frame_out.mame_occupancy_available &&
            m_cached_frame_out.mame_occupancy_eligible && m_config.game != "mame_neogeo";
    } else {
        m_mame_occupancy_available = false;
        m_mame_occupancy_valid = false;
        m_mame_occupancy_eligible = false;
    }

    // Process layers only when the emulator publishes a new frame. XR may render
    // at 72/90/120 Hz, so rebuilding Genesis layer buffers every XR frame is wasteful.
    m_render_layer_refs.clear();
    if (!have_frame) {
        if (!m_menu_open && m_active_sub_panel != 2 && m_active_sub_panel != 3 && m_active_sub_panel != 7) {
            return;
        }
    }
    // Layer extraction is the expensive per-frame CPU work (per-pixel
    // classification, blob/run building, then a texture re-upload per layer).
    // It used to be skipped outright while the menu was open, which is why the
    // game appeared frozen behind the panels even though the emu thread was
    // still stepping and producing audio -- only the picture was stale.
    //
    // Now it keeps running while a panel UI is up, but only on every Nth
    // published emulator frame. That reads as live motion behind the panels
    // while leaving most of the XR frame's budget to rasterizing the panels
    // themselves, which is what has to stay responsive to the laser.
    //
    // Throttled for the full menu and for the side/quick panels alike
    // (m_panel_ui_active, set during input handling). The side panels' share of
    // that flag is laser-driven, so pointing away from them -- or closing them
    // -- drops straight back to full-rate extraction on the next frame.
    const bool panels_in_use = m_menu_open || m_panel_ui_active;
    bool process_layers =
        frame_updated && m_cached_frame_out.width > 0 && m_cached_frame_out.height > 0;
    if (!panels_in_use) {
        m_menu_layer_update_counter = 0;
    } else if (process_layers) {
        constexpr int k_menu_layer_update_stride = 4; // ~15 Hz behind the menu
        if (++m_menu_layer_update_counter < k_menu_layer_update_stride) {
            process_layers = false;
        } else {
            m_menu_layer_update_counter = 0;
        }
    }
    if (process_layers) {
        using Clock = std::chrono::steady_clock;
        const auto extraction_start = Clock::now();
        // Update dynamic z-splits every ~0.5s to track moving sprites.
        // Accumulates across every frame in the window (not just a single
        // snapshot frame) -- sampling only the Nth frame meant any frame
        // where a layer's content happened to be momentarily empty (e.g. a
        // blinking HUD/fix layer) made update_z_splits() shrink the layer
        // list, then grow it back a window later: visible as layers
        // flickering in and out rather than smoothly tracking motion.
        static uint32_t s_histogram[256] = {};
        static int s_frame_skip = 0;
        if (!m_cached_frame_out.zbuffer.empty()) {
            ++s_frame_skip;
            const uint8_t* zptr = m_cached_frame_out.zbuffer.data();
            size_t npix = static_cast<size_t>(m_cached_frame_out.width) * m_cached_frame_out.height;
            for (size_t i = 0; i < npix && i < m_cached_frame_out.zbuffer.size(); ++i) {
                s_histogram[zptr[i]]++;
            }
            if (s_frame_skip >= 5) {  // check every 5 frames (~0.08s)
                s_frame_skip = 0;
                static uint32_t s_zsplit_log_count = 0;
                if (s_zsplit_log_count < 2000) {
                    // Temporary diagnostic: which raw z-values (0..63) actually
                    // showed up this window, before update_z_splits() maps them
                    // onto the fixed layer set -- to check whether the synthesis
                    // is really only emitting the handful of constants we seeded
                    // (2/8/18/28/38/48/52) or something wider.
                    ++s_zsplit_log_count;
                    char zlist[512];
                    int zlist_len = 0;
                    int distinct = 0;
                    for (int z = 0; z < 64 && zlist_len < (int)sizeof(zlist) - 8; ++z) {
                        if (s_histogram[z] == 0) continue;
                        ++distinct;
                        zlist_len += snprintf(zlist + zlist_len, sizeof(zlist) - zlist_len,
                                               "%d:%u ", z, s_histogram[z]);
                    }
                    __android_log_print(ANDROID_LOG_INFO, "QuestRetroDepthXR",
                        "ZBUFFER_DEBUG: zbuffer_size=%zu layers=%zu dynamic=%d depth_mode=%d distinct_z=%d [%s]",
                        m_cached_frame_out.zbuffer.size(), m_config.layers.size(),
                        (int)m_config.dynamic_layers, (int)m_vr_state.depth_mode, distinct, zlist);
                }
                m_config.update_z_splits(s_histogram);
                memset(s_histogram, 0, sizeof(s_histogram));
            }
        }

        const bool zbuffer_mode         = m_vr_state.depth_mode == DepthMode::ZBuffer;
        const bool build_object_boxes   = m_vr_state.depth_mode == DepthMode::BoundingBox;
        const bool build_extrude_runs   = is_pixel_geometry_mode(m_vr_state.depth_mode) || zbuffer_mode;
        {
            LayerProcessor proc(m_config);
            const uint8_t* zbuf = m_cached_frame_out.zbuffer.empty() ? nullptr : m_cached_frame_out.zbuffer.data();
            proc.process_into(
                m_cached_layer_frames,
                m_cached_frame_out.rgba8888.data(),
                (int)m_cached_frame_out.width,
                (int)m_cached_frame_out.height,
                zbuf,
                &m_cached_frame_out,
                build_object_boxes,
                build_extrude_runs,
                zbuffer_mode);
        }

        const bool visible_source_backend =
            m_current_backend_kind == BackendKind::Nes ||
            m_current_backend_kind == BackendKind::Gba ||
            m_current_backend_kind == BackendKind::Gb ||
            m_current_backend_kind == BackendKind::Pce ||
            m_current_backend_kind == BackendKind::Sms;
        if (visible_source_backend) {
            static int visible_source_empty_log_counter = 0;
            bool any_visible_source_layer = false;
            for (const auto& layer : m_cached_layer_frames) {
                if (layer.has_pixels) {
                    any_visible_source_layer = true;
                    break;
                }
            }
            if (!any_visible_source_layer) {
                const bool should_log_empty = (++visible_source_empty_log_counter % 120) == 1;
                if (should_log_empty) {
                    std::array<int, 8> source_counts{};
                    const std::size_t npix = static_cast<std::size_t>(m_cached_frame_out.width) * m_cached_frame_out.height;
                    const std::size_t count = std::min(npix, m_cached_frame_out.visible_source_id.size());
                    for (std::size_t i = 0; i < count; ++i) {
                        const uint8_t src_id = m_cached_frame_out.visible_source_id[i];
                        if (src_id < source_counts.size()) {
                            source_counts[src_id]++;
                        }
                    }
                    const char* backend_name = "visible-source";
                    switch (m_current_backend_kind) {
                    case BackendKind::Nes: backend_name = "NES"; break;
                    case BackendKind::Gba: backend_name = "GBA"; break;
                    case BackendKind::Gb:  backend_name = "GB/GBC"; break;
                    case BackendKind::Pce: backend_name = "PCE"; break;
                    case BackendKind::Sms: backend_name = "SMS/GG"; break;
                    default: break;
                    }
                    LOGE("%s layered extraction produced no visible layers for %ux%u frame; visible_source bytes=%zu counts=[0:%d 1:%d 2:%d 3:%d 4:%d 5:%d]",
                         backend_name,
                         m_cached_frame_out.width,
                         m_cached_frame_out.height,
                         m_cached_frame_out.visible_source_id.size(),
                         source_counts[0],
                         source_counts[1],
                         source_counts[2],
                         source_counts[3],
                         source_counts[4],
                         source_counts[5]);
                }
                if (visible_source_backend &&
                    !m_cached_layer_frames.empty() &&
                    !m_cached_frame_out.rgba8888.empty()) {
                    GameConfig fallback_cfg = GameConfig::make_flat();
                    fallback_cfg.virtual_width = (int)m_cached_frame_out.width;
                    fallback_cfg.virtual_height = (int)m_cached_frame_out.height;
                    fallback_cfg.quad_y_meters = m_config.quad_y_meters;
                    fallback_cfg.layers[0].id = m_cached_layer_frames[0].id;
                    fallback_cfg.layers[0].depth_meters = m_cached_layer_frames[0].depth_meters;
                    fallback_cfg.layers[0].quad_width_meters = m_cached_layer_frames[0].quad_width_meters;
                    fallback_cfg.layers[0].copies = m_cached_layer_frames[0].copies;

                    std::vector<LayerFrame> fallback_frames;
                    LayerProcessor fallback_proc(fallback_cfg);
                    fallback_proc.process_into(
                        fallback_frames,
                        m_cached_frame_out.rgba8888.data(),
                        (int)m_cached_frame_out.width,
                        (int)m_cached_frame_out.height,
                        nullptr,
                        &m_cached_frame_out,
                        build_object_boxes,
                        build_extrude_runs);

                    if (!fallback_frames.empty()) {
                        m_cached_layer_frames[0] = std::move(fallback_frames[0]);
                        for (std::size_t i = 1; i < m_cached_layer_frames.size(); ++i) {
                            m_cached_layer_frames[i].has_pixels = false;
                            m_cached_layer_frames[i].wedge_eligible = false;
                            m_cached_layer_frames[i].bbox_eligible = false;
                            m_cached_layer_frames[i].object_boxes.clear();
                            std::fill(m_cached_layer_frames[i].rgba.begin(),
                                      m_cached_layer_frames[i].rgba.end(), 0u);
                        }
                        if (should_log_empty) {
                            const char* backend_name = "visible-source";
                            switch (m_current_backend_kind) {
                            case BackendKind::Nes: backend_name = "NES"; break;
                            case BackendKind::Gba: backend_name = "GBA"; break;
                            case BackendKind::Gb:  backend_name = "GB/GBC"; break;
                            case BackendKind::Pce: backend_name = "PCE"; break;
                            case BackendKind::Sms: backend_name = "SMS/GG"; break;
                            default: break;
                            }
                            LOGI("%s render fallback engaged: flat full-frame extraction for %ux%u frame",
                                 backend_name, m_cached_frame_out.width, m_cached_frame_out.height);
                        }
                    }
                }
            } else {
                visible_source_empty_log_counter = 0;
            }
        }

        if (m_current_backend_kind == BackendKind::Genesis &&
            !m_cached_layer_frames.empty() &&
            m_cached_layer_frames[0].has_pixels) {
            bool have_non_backdrop_pixels = false;
            for (std::size_t i = 1; i < m_cached_layer_frames.size(); ++i) {
                if (m_cached_layer_frames[i].has_pixels) {
                    have_non_backdrop_pixels = true;
                    break;
                }
            }
            if (!have_non_backdrop_pixels) {
                GameConfig fallback_cfg = GameConfig::make_flat();
                fallback_cfg.layers[0].id = m_cached_layer_frames[0].id;
                fallback_cfg.layers[0].depth_meters = m_cached_layer_frames[0].depth_meters;
                fallback_cfg.layers[0].quad_width_meters = m_cached_layer_frames[0].quad_width_meters;
                fallback_cfg.layers[0].copies = m_cached_layer_frames[0].copies;

                std::vector<LayerFrame> fallback_frames;
                LayerProcessor fallback_proc(fallback_cfg);
                fallback_proc.process_into(
                    fallback_frames,
                    m_cached_frame_out.rgba8888.data(),
                    (int)m_cached_frame_out.width,
                    (int)m_cached_frame_out.height,
                    nullptr,
                    &m_cached_frame_out,
                    build_object_boxes,
                    build_extrude_runs);

                if (!fallback_frames.empty()) {
                    m_cached_layer_frames[0] = std::move(fallback_frames[0]);
                    for (std::size_t i = 1; i < m_cached_layer_frames.size(); ++i) {
                        m_cached_layer_frames[i].has_pixels = false;
                        m_cached_layer_frames[i].wedge_eligible = false;
                        m_cached_layer_frames[i].bbox_eligible = false;
                        m_cached_layer_frames[i].object_boxes.clear();
                        std::fill(m_cached_layer_frames[i].rgba.begin(),
                                  m_cached_layer_frames[i].rgba.end(), 0u);
                    }
                    LOGI("Genesis render fallback engaged: backdrop-only extraction for %ux%u frame",
                         m_cached_frame_out.width, m_cached_frame_out.height);
                }
            }
        }

        for (auto& layer : m_cached_layer_frames) {
            layer.content_revision = m_cached_frame_seq;
        }

        const float extraction_ms = std::chrono::duration<float, std::milli>(
            Clock::now() - extraction_start).count();
        static int genesis_extract_log_ctr = 0;
        if (m_current_backend_kind == BackendKind::Genesis &&
            (++genesis_extract_log_ctr % 120 == 0 || extraction_ms > 4.0f)) {
            LOGI("Genesis perf: layer_extract=%.2f ms frame=%ux%u layers=%zu",
                 extraction_ms,
                 m_cached_frame_out.width,
                 m_cached_frame_out.height,
                 m_cached_layer_frames.size());
        }
    }

    presentation::sync_cached_layer_geometry_from_config(m_cached_layer_frames, m_config);

    // Keep runtime layer state aligned with the active config. This also repairs
    // legacy Genesis identity order during autoload paths that do not rebuild UI state.
    {
        const std::vector<int> prev_order = m_layer_order;
        const std::vector<bool> prev_enabled = m_layer_enabled;
        const std::vector<bool> prev_ambilight = m_layer_ambilight;
        const std::size_t prev_name_count = m_layer_names.size();
        presentation::ensure_layer_runtime_state_matches_config(
            m_config, m_layer_names, m_layer_order, m_layer_enabled, m_layer_ambilight, m_layer_side_color);
        if (m_layer_order != prev_order ||
            m_layer_enabled != prev_enabled ||
            m_layer_ambilight != prev_ambilight ||
            m_layer_names.size() != prev_name_count) {
            refresh_quick_layer_presets();
            m_layer_panel_dirty = true;
        }
    }

    // Depth Arrangement widget (Layers > Stack, unified menu): once its
    // fraction array matches the current stack size — i.e. the widget has
    // actually been touched for this game/core — its per-SLOT fraction
    // becomes the authoritative depth source, overriding
    // LayerConfig::depth_meters at render time rather than rewriting it.
    // This deliberately leaves the legacy per-layer depth field (and its
    // known-buggy writers: reorder's redistribute-evenly, and the
    // dashboard's Near/Far Distance buttons) untouched but inert while the
    // widget is active — replacing those call sites outright is a separate,
    // larger follow-up.
    const bool slot_fractions_active =
        !m_layer_slot_fraction.empty() && m_layer_slot_fraction.size() == m_layer_order.size();

    // Apply layer order, enabled flags, and ambilight flags. Previously gated on !m_menu_open,
    // which left m_render_layer_refs empty for the whole time the menu was open instead of
    // tracking the live layer state — so perspective comp and depth compaction (which run
    // unconditionally every frame) went from "nothing" to "everything" in a single frame right
    // when the menu closed, with no continuity between whatever was showing before/behind the
    // menu and the freshly rebuilt list. Rebuilding every frame regardless of menu state keeps
    // this list continuous, so there's no discontinuous jump for perspective comp to trip on.
    // Parallel to m_render_layer_refs: which depth-arrangement SLOT each pushed
    // ref occupies, so apply_slot_fraction_layer_depths (below, once refs are
    // final) can look up the right fraction per ref even though disabled/empty
    // layers are skipped and don't get a 1:1 push -> slot correspondence.
    m_render_layer_slot.clear();
    if (!m_layer_order.empty() && m_layer_order.size() == m_cached_layer_frames.size()) {
        m_render_layer_refs.reserve(m_cached_layer_frames.size());
        m_render_layer_slot.reserve(m_cached_layer_frames.size());
        for (size_t slot = 0; slot < m_layer_order.size(); ++slot) {
            const int orig = m_layer_order[slot];
            if (orig < (int)m_layer_enabled.size() && m_layer_enabled[orig]) {
                auto& lf = m_cached_layer_frames[orig];
                // TEMP TESTING: m_debug_hide_empty_layers -- see the matching filter in
                // rebuild_layer_panel_texture(). Skip an empty layer here too so it isn't
                // even submitted for rendering, not just hidden from the panel list.
                if (m_debug_hide_empty_layers && !lf.has_pixels) continue;
                lf.contrib_ambilight = (orig < (int)m_layer_ambilight.size())
                                       ? (bool)m_layer_ambilight[orig] : true;
                lf.side_color_mode = (m_config.game == "mame_neogeo" && kDebugNeoGeoBlack3dSides)
                                     ? 6
                                     : ((orig < (int)m_layer_side_color.size())
                                        ? m_layer_side_color[orig] : 0);
                m_render_layer_refs.push_back(&lf);
                m_render_layer_slot.push_back((int)slot);
            }
        }
    } else {
        m_render_layer_refs.reserve(m_cached_layer_frames.size());
        m_render_layer_slot.reserve(m_cached_layer_frames.size());
        for (int i = 0; i < static_cast<int>(m_cached_layer_frames.size()); ++i) {
            if (m_config.game == "mame_neogeo" && kDebugNeoGeoBlack3dSides)
                m_cached_layer_frames[i].side_color_mode = 6;
            m_render_layer_refs.push_back(&m_cached_layer_frames[i]);
            m_render_layer_slot.push_back(i);
        }
    }

    presentation::apply_layer_auto_dup_visible(m_render_layer_refs, m_layer_auto_dup_percent);
    // Backends with explicit per-layer depths (VisibleSourceFinal) must not have
    // their depths redistributed — compact is only needed for z-buffer-derived
    // systems (Genesis, SNES) where all depths may cluster at the same value.
    {
        static int s_depth_log = 0;
        if ((s_depth_log++ % 120) == 0) {
            for (int _i = 0; _i < (int)m_render_layer_refs.size(); ++_i) {
                const LayerFrame* _lf = m_render_layer_refs[_i];
                LOGI("QRD_DEPTH render_ref[%d] id=%s depth=%.3fm has_pixels=%d",
                     _i, _lf ? _lf->id.c_str() : "(null)",
                     _lf ? _lf->depth_meters : 0.f,
                     _lf ? (int)_lf->has_pixels : 0);
            }
        }
    }
    const bool explicit_depth_backend =
        m_current_backend_kind == BackendKind::Nes ||
        m_current_backend_kind == BackendKind::Gba ||
        m_current_backend_kind == BackendKind::Gb  ||
        m_current_backend_kind == BackendKind::Pce ||
        m_current_backend_kind == BackendKind::Sms;
    if (m_config.game == "mame_neogeo") {
        // "neogeo_fix" (insert-coin/HUD text) is captured from a completely
        // separate re-render in neogeo_v.cpp, decoupled from the sprite
        // capture-slot system entirely -- but it's still subject to the same
        // enable/hide-when-empty gating every other layer goes through above
        // (m_layer_enabled / m_debug_hide_empty_layers), which observed data
        // shows keeps it out of m_render_layer_refs almost every frame (0 of
        // 167 sampled snapshots over a 90s capture had it present). Force it
        // into the render list unconditionally here, bypassing both gates --
        // it is a hardcoded reserved layer, not a toggle-able layer.
        // Its actual visibility per-pixel still comes from its own alpha
        // (transparent when the game draws nothing there), so this can't
        // show a stale/wrong image, only fixes IF it renders at all.
        {
            bool have_fix = false;
            for (LayerFrame* lf : m_render_layer_refs) {
                if (lf && lf->id == "neogeo_fix") { have_fix = true; break; }
            }
            if (!have_fix) {
                for (auto& lf : m_cached_layer_frames) {
                    if (lf.id == "neogeo_fix") { m_render_layer_refs.push_back(&lf); break; }
                }
            }
        }
        // Depth is derived from each layer's REAL z_order this frame
        // (mame_layer_z_order(), 0..255, exported by neogeo_v.cpp via
        // rd_capture_slot_zorder() for the 30 "neogeo_drawN" layers, 0 for
        // "neogeo_base", 255 for "neogeo_fix") rather than array order,
        // since which object claims which capture slot changes every frame.
        //
        // A plain linear map of that raw 0..255 value onto the metres range
        // (what this used to do) preserves ORDER but not SPACING: if two
        // active layers' raw z_order values happen to be numerically close
        // (e.g. two adjacent column-collision levels), they'd land only a
        // hair apart in depth, since the map is absolute against the full
        // 0..255 span rather than relative to how many layers are actually
        // on screen this frame -- "almost the same depth as another" was
        // exactly this. Fix: RANK the active layers by their real z_order
        // (so ordering still tracks the live per-frame value, not stale
        // array order) and then space adjacent ranks apart by a fixed
        // minimum slab thickness, same guarantee apply_accordion_layer_depths
        // used to give from array order alone.
        //
        // Anchored at the FARTHEST end, not the nearest: rank_from_far
        // (0 = farthest/lowest z_order) counts UP from a fixed far depth, so
        // the farthest layer always lands at the exact same depth regardless
        // of how many layers are active this frame -- anchoring at the
        // nearest end instead (as this originally did) made the far anchor
        // drift with active-layer count (e.g. 15 active layers pushed the
        // farthest one much farther back than 7 active layers would), which
        // read as layers "spawning all over the place" frame to frame as
        // the active count naturally fluctuates. Only the near end now
        // shifts with count, which is expected/harmless. "neogeo_fix" is
        // assigned to a reserved fixed layer below, regardless of its own
        // rank or the number of dynamic layers currently visible.
        constexpr float k_neogeo_depth_far      = 2.15f;  // metres, farthest active layer -- fixed regardless of count
        constexpr float k_neogeo_slab_thickness = 0.12f;  // TEMP: exaggerated for pattern-spotting, normally 0.05f
        struct RankEntry { LayerFrame* lf; uint32_t z_order; };
        std::vector<RankEntry> rank_entries;
        rank_entries.reserve(m_render_layer_refs.size());
        for (LayerFrame* lf : m_render_layer_refs) {
            if (!lf) continue;
            // The fix plane is not part of the dynamic 0..5 sprite-layer
            // ranking. It occupies the reserved fixed layer 6 below.
            if (lf->id == "neogeo_fix") continue;
            uint32_t z_order = 0;
            bool found = false;
            const int n = mame_layer_count();
            for (int i = 0; i < n; ++i) {
                const char* nm = mame_layer_name(i);
                if (nm && lf->id == nm) { z_order = mame_layer_z_order(i); found = true; break; }
            }
            if (!found) continue;
            rank_entries.push_back({lf, z_order});
        }
        std::stable_sort(rank_entries.begin(), rank_entries.end(),
                          [](const RankEntry& a, const RankEntry& b) { return a.z_order < b.z_order; });
        for (size_t i = 0; i < rank_entries.size(); ++i) {
            const size_t rank_from_far = i; // 0 = farthest (lowest z_order)
            rank_entries[i].lf->depth_meters = k_neogeo_depth_far - (float)rank_from_far * k_neogeo_slab_thickness;
        }

        // Hardcoded: "insert coin" / HUD (id "neogeo_fix", RD_Z_FIX in neogeo_spr.h)
        // always occupies reserved layer index 6. Dynamic OCCUPXY layers occupy
        // indices 0..5; the fix plane does not move closer/farther when fewer
        // dynamic layers happen to be visible.
        {
            LayerFrame* fix_lf = nullptr;
            for (LayerFrame* lf : m_render_layer_refs) {
                if (!lf) continue;
                if (lf->id == "neogeo_fix") { fix_lf = lf; break; }
            }
            if (fix_lf) {
                constexpr size_t kNeogeoFixLayerIndex = 6;
                fix_lf->depth_meters = k_neogeo_depth_far -
                                       (float)kNeogeoFixLayerIndex * k_neogeo_slab_thickness;
            }
        }
    } else if (slot_fractions_active) {
        // Depth Arrangement widget active: place each layer at its arranged
        // fraction WITHIN the current live near/far envelope (recomputed from
        // this frame's actual depths, same as compact_visible_layer_depths),
        // rather than an absolute canvas-metres depth. Keeps z-buffer backends'
        // per-frame-dynamic depth ordering live under the widget instead of
        // freezing a stale snapshot, and keeps the near/far spread in the same
        // tight range perspective compensation already expects, instead of an
        // arbitrary canvas-metres span that could balloon the far layer's width.
        presentation::apply_slot_fraction_layer_depths(m_render_layer_refs, m_render_layer_slot,
                                                        m_layer_slot_fraction);
    } else if (!explicit_depth_backend) {
        presentation::compact_visible_layer_depths(m_render_layer_refs);
    }
    if (m_config.game == "mame_neogeo") {
        // Desktop RetroDepth's PC renderer never scales a Neo Geo layer's quad width by
        // depth -- every group layer is the same canonical 320x224 canvas, drawn on the
        // same quad width, and only its pixel alpha differs from the others (see the
        // palette-group exporter). Depth separation alone (the accordion stack above)
        // does the work; the headset's own projection makes farther slabs read as
        // smaller, the same way any real object recedes with distance -- there's no
        // extra width compensation layered on top. Applying apply_perspective_comp_to_refs
        // here instead (as every other backend does) actively fights that: it inflates a
        // far layer's width to make it read the SAME apparent size as the nearest one,
        // and with the accordion's now much wider depth range (up to ~250 layers x 0.05m)
        // that scale ratio can reach ~19x, which is what showed up on-device as layers
        // that don't visually "line up" even though they share a config-side uniform
        // quad_width_meters (game_config.cpp's apply_uniform_width_and_copies). So: leave
        // every layer's width exactly as sync_cached_layer_geometry_from_config set it
        // from config (uniform already) and skip persp-comp scaling entirely.
        for (LayerFrame* lf : m_render_layer_refs) if (lf) lf->persp_comp_scale = 1.0f;
    } else if (m_vr_state.perspective_comp) {
        presentation::apply_perspective_comp_to_refs(m_render_layer_refs);
    } else {
        for (LayerFrame* lf : m_render_layer_refs) if (lf) lf->persp_comp_scale = 1.0f;
    }
    // TEMP DEBUG: dump every render layer's id/depth/width once per ~1s to
    // trace the backdrop/fix size-mismatch bug.
    if (m_current_backend_kind == BackendKind::Mame) {
        static int s_pc_log_ctr = 0;
        if (++s_pc_log_ctr % 60 == 0) {
            __android_log_print(ANDROID_LOG_INFO, "QuestRetroDepthXR",
                "PERSPCOMP_DEBUG: on=%d n=%d", (int)m_vr_state.perspective_comp,
                (int)m_render_layer_refs.size());
            for (size_t ci = 0; ci < m_render_layer_refs.size() && ci < (size_t)m_layer_order.size(); ++ci) {
                LayerFrame* lf = m_render_layer_refs[ci];
                if (!lf) continue;
                int orig = m_layer_order[ci];
                const char* id = (orig >= 0 && orig < (int)m_config.layers.size())
                    ? m_config.layers[orig].id.c_str() : "?";
                __android_log_print(ANDROID_LOG_INFO, "QuestRetroDepthXR",
                    "PERSPCOMP_DEBUG:   id=%s depth=%.4f qw=%.4f", id, lf->depth_meters, lf->quad_width_meters);
            }
        }
    }

    const bool blackout_detector_allowed =
        !m_menu_open &&
        !m_edit_mode &&
        !passthrough_active() &&
        m_active_sub_panel != 2 &&
        m_active_sub_panel != 3 &&
        m_active_sub_panel != 7 &&
        !m_render_layer_refs.empty();

    if (!blackout_detector_allowed) {
        m_blackout_reveal_phase = BlackoutRevealPhase::Normal;
        m_blackout_candidate_frames = 0;
        m_blackout_visible_frames = 0;
        m_blackout_reveal_start_time = 0;
        m_blackout_reveal_layer_ids.clear();
    } else if (frame_updated) {
        int bright_samples = 0;
        const bool blackout_candidate = presentation::is_blackout_candidate(m_render_layer_refs, bright_samples);
        if (blackout_candidate) {
            ++m_blackout_candidate_frames;
            m_blackout_visible_frames = 0;
        } else {
            m_blackout_candidate_frames = 0;
            m_blackout_visible_frames = (bright_samples >= 8) ? (m_blackout_visible_frames + 1) : 0;
        }

        if (m_blackout_reveal_phase == BlackoutRevealPhase::RevealAnimating &&
            m_blackout_candidate_frames >= 2) {
            m_blackout_reveal_phase = BlackoutRevealPhase::BlackoutLatched;
            m_blackout_reveal_start_time = 0;
            m_blackout_reveal_layer_ids.clear();
        }

        switch (m_blackout_reveal_phase) {
            case BlackoutRevealPhase::Normal:
                if (m_blackout_candidate_frames >= 2) {
                    m_blackout_reveal_phase = BlackoutRevealPhase::BlackoutLatched;
                    m_blackout_reveal_start_time = 0;
                    m_blackout_reveal_layer_ids.clear();
                }
                break;
            case BlackoutRevealPhase::BlackoutLatched:
                if (m_blackout_visible_frames >= 2) {
                    std::vector<const LayerFrame*> revealable;
                    revealable.reserve(m_render_layer_refs.size());
                    for (const LayerFrame* lf : m_render_layer_refs) {
                        if (!lf || !lf->has_pixels || lf->width <= 0 || lf->height <= 0 || lf->rgba.empty()) continue;
                        revealable.push_back(lf);
                    }
                    std::sort(revealable.begin(), revealable.end(), [](const LayerFrame* a, const LayerFrame* b) {
                        return a->depth_meters > b->depth_meters;
                    });
                    m_blackout_reveal_layer_ids.clear();
                    for (const LayerFrame* lf : revealable) m_blackout_reveal_layer_ids.push_back(lf->id);
                    if (m_blackout_reveal_layer_ids.size() >= 2) {
                        m_blackout_reveal_phase = BlackoutRevealPhase::RevealAnimating;
                        m_blackout_reveal_start_time = m_frame_predicted_time;
                    } else {
                        m_blackout_reveal_phase = BlackoutRevealPhase::Normal;
                        m_blackout_reveal_start_time = 0;
                        m_blackout_reveal_layer_ids.clear();
                    }
                }
                break;
            case BlackoutRevealPhase::RevealAnimating:
                break;
            case BlackoutRevealPhase::RevealCooldown:
                break;
        }
    }

    if (frame_updated || m_environment_sphere_sample_mode != m_vr_state.environment_sphere_mode) {
        EnvironmentSphereSample target_sample{};
        bool build_ok = presentation::build_environment_sample_from_visible_layers(
            m_render_layer_refs, m_vr_state.environment_sphere_mode, target_sample);
        presentation::smooth_environment_sample(m_environment_sphere_sample, target_sample, 0.15f);
        m_environment_sphere_sample_mode = m_vr_state.environment_sphere_mode;
        static int sky_build_log = 0;
        if (++sky_build_log % 120 == 1) {
            LOGE("SKY_DBG build_ok=%d target.valid=%d smooth.valid=%d refs=%zu mode=%d",
                 (int)build_ok, (int)target_sample.valid, (int)m_environment_sphere_sample.valid,
                 m_render_layer_refs.size(), (int)m_vr_state.environment_sphere_mode);
        }
    }

    // ---- Auto-pause the emulator while any menu/panel UI or edit mode is open ----
    // Edge-triggered so it doesn't fight the manual play/pause button on the
    // layer panel.
    // Disabled per user report: entering/leaving menus while this was active
    // was suspected to be the actual cause of a perceived slowdown/judder
    // issue (rather than the permacurve render-path change it was initially
    // attributed to). Left in place (not deleted) in case it needs
    // revisiting; flip kAutoPauseOnMenuEnabled back to true to restore it.
    constexpr bool kAutoPauseOnMenuEnabled = false;
    if (kAutoPauseOnMenuEnabled) {
        const bool ui_open = m_menu_open || m_ctrlmap_mode || m_edit_mode ||
                              m_active_sub_panel != 0;
        if (ui_open != m_auto_pause_ui_open_prev) {
            EmuFreezeCtrl freeze_fn;
            { std::lock_guard<std::mutex> lk(m_mutex); freeze_fn = m_emu_freeze_ctrl; }
            if (freeze_fn) {
                freeze_fn(ui_open);
                m_emu_frozen_display = ui_open;
            }
            m_auto_pause_ui_open_prev = ui_open;
        }
    }

    // ---- Rebuild panel textures on GL thread (one per frame to avoid spike) ----
    // Throttle: JNI bitmap render is expensive; don't rebuild more than once per 100 ms.
    constexpr XrTime k_panel_rebuild_interval = 100'000'000; // 100 ms in nanoseconds
    {
        const std::string help_key = build_help_model(
            m_menu_open, m_active_sub_panel, m_ctrlmap_mode, m_edit_mode,
            m_current_backend_kind, m_button_map).key();
        if (help_key != m_help_panel_key) {
            m_help_panel_dirty = true;
        }
        if (m_help_panel_dirty &&
            (m_frame_predicted_time - m_last_help_fire) >= k_panel_rebuild_interval) {
            rebuild_help_panel_texture();
            m_last_help_fire = m_frame_predicted_time;
        }
    }
    if (m_rom_load_panel_dirty.exchange(false, std::memory_order_acq_rel)) {
        m_rom_hint_dirty = true;
        if (m_rom_load_in_progress.load(std::memory_order_acquire)) {
            m_rom_hint_hide_at_ms = UINT64_MAX;
        } else {
            m_rom_hint_hide_at_ms = 0;
        }
    }
    if (m_rom_hint_dirty &&
        (m_frame_predicted_time - m_last_rom_hint_fire) >= k_panel_rebuild_interval) {
        rebuild_rom_hint_texture();
        m_last_rom_hint_fire = m_frame_predicted_time;
    }
    if (m_vr_state.side_panel_mode == kSidePanelPerf) {
        update_perf_stats();
        constexpr XrTime k_perf_overlay_refresh = 250'000'000; // 250 ms — readable, not jittery
        if ((m_frame_predicted_time - m_last_perf_overlay_fire) >= k_perf_overlay_refresh) {
            rebuild_perf_overlay_texture();
            m_last_perf_overlay_fire = m_frame_predicted_time;
        }
    }
    // Settings side-panel mode: left wing is the REAL Settings panel (same texture/content as
    // the centered menu — "inception", since it's the panel that controls this very cycle) and
    // right wing is the existing quick-edit panel (presets/resets/jump buttons), both reused
    // verbatim — only their POSITION differs (side wings instead of the centered menu panel)
    // when shown passively during gameplay, independent of whether either panel is separately
    // open as the real centered menu.
    if (m_vr_state.side_panel_mode == kSidePanelSettings) {
        if (m_active_sub_panel != k_panel_quick_edit &&
            m_quick_panel_dirty &&
            (m_frame_predicted_time - m_last_quick_panel_fire) >= k_panel_rebuild_interval) {
            rebuild_quick_edit_panel_texture();
            m_last_quick_panel_fire = m_frame_predicted_time;
        }
        if (!(m_menu_open && m_active_sub_panel == 3) &&
            m_settings_panel_dirty &&
            (m_frame_predicted_time - m_last_settings_fire) >= k_panel_rebuild_interval) {
            rebuild_settings_panel_texture();
            m_last_settings_fire = m_frame_predicted_time;
        }
    }
    // Always-visible Side Panels mode-select bar: rebuilds independent of side_panel_mode (it's
    // shown even when Off) and independent of the menu, but only while gameplay's actually
    // showing it (!m_menu_open matches the render-time gate below).
    if (!m_menu_open) {
        if (m_side_bar_dirty &&
            (m_frame_predicted_time - m_last_side_bar_fire) >= k_panel_rebuild_interval) {
            rebuild_side_bar_texture();
            m_last_side_bar_fire = m_frame_predicted_time;
        }
        if (m_vr_state.side_panel_mode == kSidePanelBgColor &&
            m_bg_color_panel_dirty &&
            (m_frame_predicted_time - m_last_bg_color_fire) >= k_panel_rebuild_interval) {
            rebuild_bg_color_panel_texture();
            m_last_bg_color_fire = m_frame_predicted_time;
        }
        if (m_vr_state.side_panel_mode == kSidePanelThemes &&
            m_themes_panel_dirty &&
            (m_frame_predicted_time - m_last_themes_fire) >= k_panel_rebuild_interval) {
            rebuild_themes_panel_texture();
            m_last_themes_fire = m_frame_predicted_time;
        }
    }
    if (m_active_sub_panel == k_panel_manual_dashboard) {
        if (m_dashboard_left_panel_dirty &&
            (m_frame_predicted_time - m_last_dashboard_fire) >= k_panel_rebuild_interval) {
            rebuild_dashboard_left_panel_texture();
            m_last_dashboard_fire = m_frame_predicted_time;
        }
        if (m_layer_panel_dirty &&
            (m_frame_predicted_time - m_last_layer_fire) >= k_panel_rebuild_interval) {
            rebuild_layer_panel_texture();
            m_last_layer_fire = m_frame_predicted_time;
        }
    } else if (!m_menu_open && m_active_sub_panel == 2) {
        // Layers panel open over live game (thumbstick shortcut, no menu)
        if (m_layer_panel_dirty &&
            (m_frame_predicted_time - m_last_layer_fire) >= k_panel_rebuild_interval) {
            rebuild_layer_panel_texture();
            m_last_layer_fire = m_frame_predicted_time;
        }
    } else if (!m_menu_open && m_active_sub_panel == 3) {
        if (m_settings_panel_dirty &&
            (m_frame_predicted_time - m_last_settings_fire) >= k_panel_rebuild_interval) {
            rebuild_settings_panel_texture();
            m_last_settings_fire = m_frame_predicted_time;
        }
    } else if (!m_menu_open && m_active_sub_panel == 7) {
        if (m_quick_panel_dirty &&
            (m_frame_predicted_time - m_last_quick_panel_fire) >= k_panel_rebuild_interval) {
            rebuild_quick_edit_panel_texture();
            m_last_quick_panel_fire = m_frame_predicted_time;
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
            // Keep the "Done" label alive (and force one final rebuild right
            // after it expires to revert it) even if nothing else marks the
            // main menu dirty in the meantime.
            if (m_wipe_settings_done_until != 0) {
                if (m_frame_predicted_time < m_wipe_settings_done_until) m_main_menu_dirty = true;
                else { m_wipe_settings_done_until = 0; m_main_menu_dirty = true; }
            }
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
            // Save-state sub-panel
            if (m_save_state_panel_dirty &&
                (m_frame_predicted_time - m_last_settings_fire) >= k_panel_rebuild_interval) {
                rebuild_save_state_panel_texture();
                m_last_settings_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 5) {
            // Code panel (standalone)
            if (m_code_panel_dirty &&
                (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_code_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == 6) {
            // Ctrlmap sub-panel
            if (m_ctrlmap_panel_dirty &&
                (m_frame_predicted_time - m_last_code_fire) >= k_panel_rebuild_interval) {
                rebuild_ctrlmap_panel_texture();
                m_last_code_fire = m_frame_predicted_time;
            }
        } else if (m_active_sub_panel == k_panel_homebrew) {
            // Homebrew sub-panel
            if (m_hw_dirty &&
                (m_frame_predicted_time - m_last_hw_fire) >= k_panel_rebuild_interval) {
                rebuild_homebrew_panel_texture();
                m_last_hw_fire = m_frame_predicted_time;
            }
        }
    }

    // ---- Build overlay ------------------------------------------------------
    OverlayInfo overlay;
    const PanelMetrics main_metrics     = panel_metrics(PanelKind::MainMenu);
    const PanelMetrics browser_metrics  = panel_metrics(PanelKind::Browser);
    const PanelMetrics layer_metrics    = panel_metrics(PanelKind::Layers);
    const PanelMetrics settings_metrics = panel_metrics(PanelKind::Settings);
    const PanelMetrics save_state_metrics = panel_metrics(PanelKind::SaveStates);
    const PanelMetrics code_metrics     = panel_metrics(PanelKind::Code);
    const PanelMetrics ctrlmap_metrics  = panel_metrics(PanelKind::CtrlMap);
    const PanelMetrics quick_metrics    = panel_metrics(PanelKind::QuickEdit);
    const PanelMetrics help_metrics     = panel_metrics(PanelKind::Help);
    const PanelMetrics themes_metrics   = panel_metrics(PanelKind::ThemesPanel);
    // While testing the new ImGui menu, suppress every old-menu panel branch
    // below (Main Menu/CtrlMap/Dashboard/EditMode/Layers-Settings-QuickEdit-
    // standalone) regardless of how it got triggered — the button-press guard
    // in poll_actions() only stops *new* m_menu_open opens, not e.g. edit mode
    // or a sub-panel left active from before. See debug_show_new_ui.
    if (!m_impl->debug_show_new_ui) {
    if (m_menu_open && m_active_sub_panel != k_panel_manual_dashboard) {
        if (m_ctrlmap_mode) {
            // Controller map panel only
            auto& cm     = overlay.panels[0];
            cm.tex       = m_ctrlmap_panel_tex;
            cm.pose      = m_ctrlmap_panel_pose;
            cm.w         = ctrlmap_metrics.world_w;
            cm.h         = ctrlmap_metrics.world_h;
            overlay.panel_count = 1;

            // Highlight for ctrlmap panel
            if (m_laser_panel == k_panel_ctrlmap && m_laser_hit_has_item) {
                set_hover_highlight(overlay, 0, m_laser_hit_item, 0.16f, 0.16f, 0.39f, 0.35f);
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
            if (m_laser_panel == k_panel_main_menu && m_laser_hit_has_item) {
                set_hover_highlight(overlay, 0, m_laser_hit_item, 0.16f, 0.24f, 0.39f, 0.35f);
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
                if (m_laser_panel == k_panel_browser && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);
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
                if (m_laser_panel == k_panel_layers && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);
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
                if (m_laser_panel == k_panel_settings && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.16f, 0.24f, 0.39f, 0.35f);
                }
            } else if (m_active_sub_panel == 4) {
                // Save-state panel
                auto& ssp = overlay.panels[0];
                ssp.tex   = m_save_state_panel_tex;
                ssp.pose  = m_save_state_panel_pose;
                ssp.w     = save_state_metrics.world_w;
                ssp.h     = save_state_metrics.world_h;
                overlay.panel_count = 1;

                if (m_laser_panel == k_panel_save_state && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);
                }
            } else if (m_active_sub_panel == 5) {
                // Code panel (standalone, taller - shows code + keyboard)
                auto& cp = overlay.panels[0];
                cp.tex   = m_code_panel_tex;
                cp.pose  = m_code_panel_pose;
                cp.w     = code_metrics.world_w;
                cp.h     = code_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for code panel (larger title area for code display)
                if (m_laser_panel == k_panel_code && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.20f, 0.51f, 0.82f, 0.45f);
                }
            } else if (m_active_sub_panel == 6) {
                // Ctrlmap panel
                auto& cm = overlay.panels[0];
                cm.tex   = m_ctrlmap_panel_tex;
                cm.pose  = m_ctrlmap_panel_pose;
                cm.w     = ctrlmap_metrics.world_w;
                cm.h     = ctrlmap_metrics.world_h;
                overlay.panel_count = 1;

                // Highlight for ctrlmap
                if (m_laser_panel == k_panel_ctrlmap && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.16f, 0.16f, 0.39f, 0.35f);
                }
            } else if (m_active_sub_panel == k_panel_homebrew) {
                // Homebrew panel
                const PanelMetrics hw_metrics = panel_metrics(PanelKind::Homebrew);
                auto& hp = overlay.panels[0];
                hp.tex   = m_hw_tex;
                hp.pose  = m_homebrew_panel_pose;
                hp.w     = hw_metrics.world_w;
                hp.h     = hw_metrics.world_h;
                overlay.panel_count = 1;

                if (m_laser_panel == k_panel_homebrew && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);
                }
            } else if (m_active_sub_panel == k_panel_credits) {
                // Credits panel
                if (m_credits_dirty) rebuild_credits_panel_texture();
                const PanelMetrics credits_metrics = panel_metrics(PanelKind::Credits);
                auto& crp = overlay.panels[0];
                crp.tex  = m_credits_tex;
                crp.pose = m_credits_panel_pose;
                crp.w    = credits_metrics.world_w;
                crp.h    = credits_metrics.world_h;
                overlay.panel_count = 1;

                if (m_laser_panel == k_panel_credits && m_laser_hit_has_item) {
                    set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);
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
    } else if (m_active_sub_panel == k_panel_manual_dashboard) {
        // Manual dashboard: left wing = dashboard left controls, right wing = layer management (reuses layer panel texture)
        const PanelMetrics dashboard_left_metrics = panel_metrics(PanelKind::DashboardLeft);
        add_dashboard_wings(overlay,
                           m_dashboard_left_panel_tex,
                           dashboard_left_metrics,
                           m_layer_panel_tex,
                           layer_metrics,
                           m_dashboard_left_pose,
                           m_dashboard_right_pose);

        // Highlight hovered items
        if (m_laser_panel == k_panel_manual_dashboard && m_laser_hit_has_item)
            set_hover_highlight(overlay, 0, m_laser_hit_item, 0.16f, 0.24f, 0.39f, 0.35f);
        else if (m_laser_panel == k_panel_layers && m_laser_hit_has_item)
            set_hover_highlight(overlay, 1, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);

        overlay.show_laser = true;
        overlay.laser_origin = m_laser_origin;
        overlay.laser_end = m_laser_end;
        overlay.laser_hit = m_laser_hit;
        overlay.laser_hit_u = m_laser_hit_u;
        overlay.laser_hit_v = m_laser_hit_v;
        overlay.laser_hit_panel = m_laser_panel;
    } else if (m_edit_mode) {
        // Edit mode: show both controller lasers (no panels)
        overlay.show_laser  = true;
        overlay.laser_origin = m_edit_laser_r_origin;
        overlay.laser_end    = m_edit_laser_r_end;

        overlay.show_laser2   = true;
        overlay.laser2_origin = m_edit_laser_l_origin;
        overlay.laser2_end    = m_edit_laser_l_end;
    } else if (m_active_sub_panel == 2 || m_active_sub_panel == 3 || m_active_sub_panel == 7) {
        PanelInfo* panel = &overlay.panels[0];
        if (m_active_sub_panel == 2) {
            panel->tex = m_layer_panel_tex;
            panel->pose = m_layer_panel_pose;
            panel->w = layer_metrics.world_w;
            panel->h = layer_metrics.world_h;
        } else if (m_active_sub_panel == 3) {
            panel->tex = m_settings_panel_tex;
            panel->pose = m_settings_panel_pose;
            panel->w = settings_metrics.world_w;
            panel->h = settings_metrics.world_h;
        } else {
            panel->tex = m_quick_panel_tex;
            panel->pose = m_quick_panel_pose;
            panel->w = quick_metrics.world_w;
            panel->h = quick_metrics.world_h;
        }
        overlay.panel_count = 1;

        if (m_laser_panel == k_panel_layers && m_laser_hit_has_item) {
            set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.39f, 0.75f, 0.35f);
        } else if (m_laser_panel == k_panel_settings && m_laser_hit_has_item) {
            set_hover_highlight(overlay, 0, m_laser_hit_item, 0.16f, 0.24f, 0.39f, 0.35f);
        } else if (m_laser_panel == k_panel_quick_edit && m_laser_hit_has_item) {
            set_hover_highlight(overlay, 0, m_laser_hit_item, 0.18f, 0.42f, 0.26f, 0.35f);
        }

        overlay.show_laser    = true;
        overlay.laser_origin  = m_laser_origin;
        overlay.laser_end     = m_laser_end;
        overlay.laser_hit       = m_laser_hit;
        overlay.laser_hit_u     = m_laser_hit_u;
        overlay.laser_hit_v     = m_laser_hit_v;
        overlay.laser_hit_panel = m_laser_panel;
    }
    }

    // Side panels: cycled via the "Side Panels" settings row (VrState::side_panel_mode) —
    // Off (nothing shown), Help (static instructions), Settings ("inception": the real Settings
    // panel on the left — the very panel that controls this cycle — plus the existing quick-edit
    // panel's presets/resets/jump buttons on the right, both reused verbatim), Perf Overlay
    // (moved here from a left-controller-attached panel), Background Color (not yet implemented).
    // All resolve to the same anchor pose (the first overlay panel, or the game canvas if none);
    // Help/Perf mirror one texture to both wings via add_help_wings, Settings puts a DIFFERENT
    // texture on each wing via add_dashboard_wings.
    // Old side-panel wings (Help/Settings/Perf/BgColor/Themes) are fully replaced
    // by the automatic mirrored unified-menu side panels (see render_menu_panel_instance
    // above) while the new UI is active — only reachable now via the B+Y old-menu chord.
    if (!m_impl->debug_show_new_ui && m_vr_state.side_panel_mode != kSidePanelOff) {
        bool have_anchor = false;
        XrPosef anchor_pose{};
        if (overlay.panel_count > 0 && overlay.panels[0].tex) {
            anchor_pose = overlay.panels[0].pose;
            have_anchor = true;
        } else {
            float canvas_w = 0.0f;
            have_anchor = game_canvas_anchor_pose(m_render_layer_refs, m_config,
                                                   m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el,
                                                   m_canvas_scale, anchor_pose, canvas_w);
        }
        if (have_anchor) {
            if (m_vr_state.side_panel_mode == kSidePanelHelp && m_help_panel_tex) {
                add_help_wings(overlay, m_help_panel_tex, anchor_pose, m_impl->last_hmd_pose, help_metrics);
            } else if (m_vr_state.side_panel_mode == kSidePanelPerf && m_perf_overlay_tex) {
                add_help_wings(overlay, m_perf_overlay_tex, anchor_pose, m_impl->last_hmd_pose, help_metrics);
            } else if (m_vr_state.side_panel_mode == kSidePanelSettings &&
                       m_active_sub_panel != k_panel_quick_edit && m_active_sub_panel != 3 &&
                       m_settings_panel_tex && m_quick_panel_tex) {
                // Use the SAME poses computed during input processing (m_side_settings_left/
                // right_pose), not a fresh recompute here — they must match exactly, or the
                // laser raycast (done at input time) and what's drawn here would disagree about
                // where these panels are, causing clicks to land in the wrong place.
                const int side_wing_base = overlay.panel_count;
                add_dashboard_wings(overlay, m_settings_panel_tex, settings_metrics,
                                     m_quick_panel_tex, quick_metrics,
                                     m_side_settings_left_pose, m_side_settings_right_pose);
                // Laser + hover highlight: only while gameplay is active and the ray is actually
                // hitting one of these two wings (m_laser_hit was set by the input-time raycast
                // pre-check) — no laser shown otherwise, per design.
                if (!m_menu_open && m_laser_hit &&
                    (m_laser_panel == k_panel_settings || m_laser_panel == k_panel_quick_edit) &&
                    side_wing_base + 1 < OverlayInfo::k_max_panels) {
                    const int hit_idx = (m_laser_panel == k_panel_settings) ? side_wing_base : side_wing_base + 1;
                    if (m_laser_hit_has_item) {
                        set_hover_highlight(overlay, hit_idx, m_laser_hit_item, 0.16f, 0.24f, 0.39f, 0.35f);
                    }
                    overlay.show_laser    = true;
                    overlay.laser_origin  = m_laser_origin;
                    overlay.laser_end     = m_laser_end;
                    overlay.laser_hit     = m_laser_hit;
                    overlay.laser_hit_u   = m_laser_hit_u;
                    overlay.laser_hit_v   = m_laser_hit_v;
                    overlay.laser_hit_panel = m_laser_panel;
                }
            } else if (m_vr_state.side_panel_mode == kSidePanelBgColor && m_bg_color_panel_tex) {
                // Background Color: same preset-grid texture at both wings — reuses the SAME
                // poses computed during input processing (m_bg_color_left/right_pose) so the
                // raycast and what's drawn here agree.
                const int bg_wing_base = overlay.panel_count;
                add_dashboard_wings(overlay, m_bg_color_panel_tex, panel_metrics(PanelKind::BgColorPanel),
                                     m_bg_color_panel_tex, panel_metrics(PanelKind::BgColorPanel),
                                     m_bg_color_left_pose, m_bg_color_right_pose);
                if (!m_menu_open && m_laser_hit && m_laser_panel == k_panel_bg_color &&
                    bg_wing_base + 1 < OverlayInfo::k_max_panels) {
                    const int hit_idx = m_bg_color_hit_is_left ? bg_wing_base : bg_wing_base + 1;
                    if (m_laser_hit_has_item) {
                        set_hover_highlight(overlay, hit_idx, m_laser_hit_item, 0.6f, 0.5f, 0.9f, 0.30f);
                    }
                    overlay.show_laser    = true;
                    overlay.laser_origin  = m_laser_origin;
                    overlay.laser_end     = m_laser_end;
                    overlay.laser_hit     = m_laser_hit;
                    overlay.laser_hit_u   = m_laser_hit_u;
                    overlay.laser_hit_v   = m_laser_hit_v;
                    overlay.laser_hit_panel = m_laser_panel;
                }
            } else if (m_vr_state.side_panel_mode == kSidePanelThemes && m_themes_panel_tex) {
                const int theme_wing_base = overlay.panel_count;
                add_dashboard_wings(overlay, m_themes_panel_tex, themes_metrics,
                                     m_themes_panel_tex, themes_metrics,
                                     m_themes_left_pose, m_themes_right_pose);
                if (!m_menu_open && m_laser_hit && m_laser_panel == k_panel_themes &&
                    theme_wing_base + 1 < OverlayInfo::k_max_panels) {
                    const int hit_idx = m_themes_hit_is_left ? theme_wing_base : theme_wing_base + 1;
                    if (m_laser_hit_has_item)
                        set_hover_highlight(overlay, hit_idx, m_laser_hit_item, 0.35f, 0.55f, 0.95f, 0.28f);
                    overlay.show_laser = true;
                    overlay.laser_origin = m_laser_origin;
                    overlay.laser_end = m_laser_end;
                    overlay.laser_hit = m_laser_hit;
                    overlay.laser_hit_u = m_laser_hit_u;
                    overlay.laser_hit_v = m_laser_hit_v;
                    overlay.laser_hit_panel = m_laser_panel;
                }
            }
        }
    }

    // Always-visible Side Panels mode-select bar: shown regardless of side_panel_mode (even
    // Off), so a player who's turned every side panel off can still reach the selector without
    // opening the menu. Duplicated at both the left and right wing positions (same spot as the
    // Help/Settings/Perf side panels, just lower) so it's reachable turning either way. Uses the
    // SAME poses computed during input processing, for the same reasoning as the wings above.
    // Same replacement as above: the old always-visible mode-select bar is now
    // only shown for the old-menu (B+Y) fallback — the new UI's automatic side
    // panels give direct access to everything this bar used to select between.
    if (!m_impl->debug_show_new_ui && !m_menu_open && m_side_bar_tex &&
        overlay.panel_count + 1 < OverlayInfo::k_max_panels) {
        const PanelMetrics bar_metrics = panel_metrics(PanelKind::SidePanelBar);
        const int bar_left_idx = overlay.panel_count;
        auto& bar_left = overlay.panels[overlay.panel_count++];
        bar_left.tex  = m_side_bar_tex;
        bar_left.pose = m_side_bar_left_pose;
        bar_left.w    = bar_metrics.world_w;
        bar_left.h    = bar_metrics.world_h;
        const int bar_right_idx = overlay.panel_count;
        auto& bar_right = overlay.panels[overlay.panel_count++];
        bar_right.tex  = m_side_bar_tex;
        bar_right.pose = m_side_bar_right_pose;
        bar_right.w    = bar_metrics.world_w;
        bar_right.h    = bar_metrics.world_h;
        if (m_laser_hit && m_laser_panel == k_panel_side_bar) {
            const int hit_idx = m_side_bar_hit_is_left ? bar_left_idx : bar_right_idx;
            if (m_laser_hit_has_item) {
                set_hover_highlight(overlay, hit_idx, m_laser_hit_item, 0.5f, 0.8f, 0.5f, 0.25f);
            }
            overlay.show_laser      = true;
            overlay.laser_origin    = m_laser_origin;
            overlay.laser_end       = m_laser_end;
            overlay.laser_hit       = m_laser_hit;
            overlay.laser_hit_u     = m_laser_hit_u;
            overlay.laser_hit_v     = m_laser_hit_v;
            overlay.laser_hit_panel = m_laser_panel;
        }
    }

    // Foreground ROM preparation status: keep this visible over the browser
    // while extraction/loading runs off the XR thread.
    if (m_rom_load_in_progress.load(std::memory_order_acquire) && m_rom_hint_tex &&
        overlay.panel_count + 1 <= OverlayInfo::k_max_panels) {
        auto& loading_panel = overlay.panels[overlay.panel_count++];
        loading_panel.tex = m_rom_hint_tex;
        loading_panel.pose = m_main_menu_pose;
        loading_panel.w = help_metrics.world_w;
        loading_panel.h = help_metrics.world_h;
    }

    // ROM-load tips: only while the menu is closed (it would just clutter/overlap the real
    // menu panels otherwise) and only for the 5s window after the ROM finished loading.
    if (!m_rom_load_in_progress.load(std::memory_order_acquire) &&
        !m_menu_open && m_rom_hint_tex && m_rom_hint_hide_at_ms != 0) {
        if (monotonic_time_ms() < m_rom_hint_hide_at_ms) {
            if (overlay.panel_count + 1 <= OverlayInfo::k_max_panels) {
                const XrPosef hint_pose = yaw_locked_pose_in_front(m_impl->last_hmd_pose, 1.3f);
                auto& panel = overlay.panels[overlay.panel_count++];
                panel.tex = m_rom_hint_tex;
                panel.pose = hint_pose;
                panel.w = help_metrics.world_w;
                panel.h = help_metrics.world_h;
            }
        } else {
            m_rom_hint_hide_at_ms = 0;
        }
    }

    const auto render_start = std::chrono::steady_clock::now();

    const VrState render_state = effective_render_state(m_vr_state);
    const bool pt_active = passthrough_active();
    std::vector<LayerFrame*> reveal_render_layer_refs = m_render_layer_refs;
    float render_canvas_scale = m_canvas_scale;

    if (m_blackout_reveal_phase == BlackoutRevealPhase::RevealAnimating) {
        constexpr float kRevealDurationNs = 500000000.0f;
        const float progress = std::clamp(
            (float)(m_frame_predicted_time - m_blackout_reveal_start_time) / kRevealDurationNs,
            0.0f, 1.0f);
        const int reveal_count = std::clamp(
            (int)std::ceil(progress * (float)m_blackout_reveal_layer_ids.size()),
            1,
            (int)m_blackout_reveal_layer_ids.size());
        reveal_render_layer_refs.clear();
        for (LayerFrame* lf : m_render_layer_refs) {
            if (!lf) continue;
            auto it = std::find(m_blackout_reveal_layer_ids.begin(),
                                m_blackout_reveal_layer_ids.begin() + reveal_count,
                                lf->id);
            if (it != m_blackout_reveal_layer_ids.begin() + reveal_count) {
                reveal_render_layer_refs.push_back(lf);
            }
        }
        if (progress >= 1.0f) {
            m_blackout_reveal_phase = BlackoutRevealPhase::RevealCooldown;
            m_blackout_reveal_start_time = m_frame_predicted_time;
        }
    } else if (m_blackout_reveal_phase == BlackoutRevealPhase::RevealCooldown) {
        render_canvas_scale *= presentation::blackout_reveal_pulse_scale(m_frame_predicted_time, m_blackout_reveal_start_time);
        if ((m_frame_predicted_time - m_blackout_reveal_start_time) >= 120000000) {
            m_blackout_reveal_phase = BlackoutRevealPhase::Normal;
            m_blackout_reveal_start_time = 0;
            m_blackout_reveal_layer_ids.clear();
        }
    }

    // Layer-deck bookshelf: resolve each drawn layer's real stack slot (not
    // just its position in reveal_render_layer_refs, which can be filtered/
    // reordered during a blackout-reveal transition) so the yaw the renderer
    // applies always matches the slot the laser picks against.
    std::vector<int> layer_deck_slots;
    if (m_layer_deck_active) {
        layer_deck_slots.reserve(reveal_render_layer_refs.size());
        for (LayerFrame* lf : reveal_render_layer_refs) {
            int slot = -1;
            if (lf) {
                int orig = -1;
                for (int k = 0; k < (int)m_cached_layer_frames.size(); ++k) {
                    if (&m_cached_layer_frames[k] == lf) { orig = k; break; }
                }
                for (int s = 0; s < (int)m_layer_order.size(); ++s) {
                    if (m_layer_order[s] == orig) { slot = s; break; }
                }
            }
            layer_deck_slots.push_back(slot);
        }
    }
    const int layer_deck_slot_count = (int)m_layer_order.size();

    const bool suppress_environment_sphere =
        m_blackout_reveal_phase == BlackoutRevealPhase::BlackoutLatched ||
        m_blackout_reveal_phase == BlackoutRevealPhase::RevealAnimating;
    SkyDomeInfo environment_info{};
    const SkyDomeInfo* environment_ptr = nullptr;
    if (!pt_active && !suppress_environment_sphere && m_vr_state.bg_preset_index >= 0) {
        // User-chosen Background Color preset takes priority over the content-derived
        // Environment Sphere sampling below — solid presets fill every band the same color,
        // gradients interpolate top(sky)->bottom(floor) linearly across the 12 bands.
        environment_info.enabled = true;
        environment_info.mode = EnvironmentSphereMode::FullSphere;
        environment_info.opaque_override = true;
        const int idx = m_vr_state.bg_preset_index;
        constexpr int kBandCount = 12;
        if (idx < 8) {
            const BgSolidPreset& p = kBgSolidPresets[idx];
            for (int i = 0; i < kBandCount; ++i) environment_info.bands[i] = {p.r, p.g, p.b, 1.0f};
        } else {
            const BgGradientPreset& p = kBgGradientPresets[idx - 8];
            for (int i = 0; i < kBandCount; ++i) {
                const float t = (float)i / (float)(kBandCount - 1); // 0 at top, 1 at bottom
                environment_info.bands[i] = {
                    p.top_r + (p.bot_r - p.top_r) * t,
                    p.top_g + (p.bot_g - p.top_g) * t,
                    p.top_b + (p.bot_b - p.top_b) * t,
                    1.0f
                };
            }
        }
        environment_ptr = &environment_info;
    } else if (false && !pt_active &&
        !suppress_environment_sphere &&
        render_state.environment_sphere_mode != EnvironmentSphereMode::Off &&
        m_environment_sphere_sample.valid) {
        environment_info.enabled = true;
        environment_info.mode = render_state.environment_sphere_mode;
        environment_info.bands = m_environment_sphere_sample.bands;
        environment_ptr = &environment_info;
    }
    {
        static int sky_gate_log = 0;
        if (++sky_gate_log % 120 == 1) {
            LOGE("SKY_DBG gate: pt=%d suppress=%d mode=%d valid=%d ptr=%s",
                 (int)pt_active, (int)suppress_environment_sphere,
                 (int)render_state.environment_sphere_mode,
                 (int)m_environment_sphere_sample.valid,
                 environment_ptr ? "SET" : "null");
        }
    }

    // World locomotion: apply persistent scale/forward-offset to every rendered layer once per
    // frame (both eyes read the same transformed values) — scaling shrinks/grows the whole
    // environment around the viewer origin; the forward offset shifts every layer's depth as if
    // the viewer had walked into or out of the scene.
    if (m_world_scale != 1.0f || m_world_forward_offset != 0.0f) {
        for (LayerFrame* lf : reveal_render_layer_refs) {
            if (!lf) continue;
            lf->quad_width_meters *= m_world_scale;
            lf->depth_meters = lf->depth_meters * m_world_scale - m_world_forward_offset;
            // Duplication-copy spacing is stored in absolute metres too — scale it along with
            // everything else, or shrinking/growing the world visibly pulls the copies apart
            // from (or squashes them into) the rest of the now-smaller/bigger layer.
            for (float& c : lf->copies) c *= m_world_scale;
        }
    }

    // Live layer reordering is driven by the right-hand laser intersecting the
    // actual rendered layer planes. This runs after the final live depth/width
    // arrangement and world-locomotion transform, so the hit and the guides
    // stay on the same geometry the renderer is about to draw.
    update_live_layer_canvas_interaction(render_state);
    append_live_layer_canvas_guides(overlay, render_state);

    // Thrust-direction laser: visualize where the right controller is pointing while left grip
    // is held, so the throttle direction is visible. Only when no other laser already claimed
    // the overlay this frame (menu/panel interaction takes priority).
    if (m_thrust_laser_active && !overlay.show_laser) {
        constexpr float k_thrust_laser_len = 3.0f;
        overlay.show_laser   = true;
        overlay.laser_origin = m_thrust_laser_origin;
        overlay.laser_end    = {
            m_thrust_laser_origin.x + m_thrust_laser_dir.x * k_thrust_laser_len,
            m_thrust_laser_origin.y + m_thrust_laser_dir.y * k_thrust_laser_len,
            m_thrust_laser_origin.z + m_thrust_laser_dir.z * k_thrust_laser_len,
        };
        overlay.laser_hit = false;
    }
    if ((m_locomotion_active || m_layer_deck_active) && m_live_layer_laser_hit) {
        overlay.show_laser = true;
        overlay.laser_origin = m_live_layer_laser_origin;
        overlay.laser_end = m_live_layer_laser_end;
        overlay.laser_hit = true;
    }

    // Real controller models (XR_FB_render_model), for tutorial recordings --
    // see poll_actions() for where m_ctrl_* is populated and
    // GlesRenderer::draw_controller_model() for how it's rendered.
    overlay.show_controller_models = m_vr_state.show_controller_models;
    for (int hand = 0; hand < 2; ++hand) {
        overlay.controller_pose[hand]        = m_ctrl_pose[hand];
        overlay.controller_pose_valid[hand]  = m_ctrl_pose_valid[hand];
        overlay.controller_btn_a[hand]       = m_ctrl_btn_a[hand];
        overlay.controller_btn_b[hand]       = m_ctrl_btn_b[hand];
        overlay.controller_trigger[hand]     = m_ctrl_trigger[hand];
        overlay.controller_grip[hand]        = m_ctrl_grip[hand];
        overlay.controller_stick_x[hand]     = m_ctrl_stick_x[hand];
        overlay.controller_stick_y[hand]     = m_ctrl_stick_y[hand];
        overlay.controller_stick_click[hand] = m_ctrl_stick_click[hand];
    }

    // Blocky lightgun model, attached to whichever controller m_gun_hand selected.
    overlay.show_gun  = m_gun_render_show;
    overlay.gun_pose  = m_gun_render_pose;
    overlay.gun_trigger = m_input_state.gun_trigger;
    overlay.show_gun2 = m_gun2_render_show;
    overlay.gun2_trigger = m_input_state.gun2_trigger;
    overlay.gun2_recoil = m_gun2_recoil;
    overlay.gun2_pose = m_gun2_render_pose;
    overlay.gun_recoil  = m_gun_recoil;
    overlay.gun_tilt    = m_gun_tilt;
    overlay.gun_model = m_vr_state.gun_model;
    overlay.gun2_model = m_vr_state.gun2_model;
    overlay.gun_muzzle_color[0] = m_gun_muzzle_color[0];
    overlay.gun_muzzle_color[1] = m_gun_muzzle_color[1];
    overlay.gun_muzzle_color[2] = m_gun_muzzle_color[2];
    overlay.gun_muzzle_heat     = m_gun_muzzle_heat;
    if (m_gun_calibration_active && m_gun_calibration_surface_valid &&
        m_gun_calibration_target >= 0) {
        const LightgunUv target = lightgun_calibration_target(m_gun_calibration_target);
        overlay.show_calibration_target = true;
        overlay.calibration_target_center = lightgun_add(
            lightgun_surface_point(m_gun_calibration_surface, target.u, target.v),
            lightgun_scale(m_gun_calibration_surface.normal, 0.006f));
        overlay.calibration_target_right = m_gun_calibration_surface.right;
        overlay.calibration_target_up = m_gun_calibration_surface.up;
        overlay.calibration_target_radius = std::min(
            m_gun_calibration_surface.width, m_gun_calibration_surface.height) * 0.045f;
    }

    // ------------------------------------------------------------------
    // TEMPORARY: Dear ImGui panel-migration infra smoke test (see the ImGui
    // panel migration plan). A single fixed-position test quad, hit-tested
    // independently of the real PanelKind/PanelRole system so this can't
    // regress any existing panel. Once the input bridge and first real
    // panel migration land, this block is deleted.
    // ------------------------------------------------------------------
    if (m_impl->imgui_bridge.is_initialized()) {
        // The old real panel system used to also be reachable via a B+Y
        // controller chord here; that's been removed — the ImGui menu's
        // "Show Old Menu" row (debug_show_new_ui, session-only, never
        // persisted) is now the only way to bring it back.

        constexpr int   kTestFboW = 1400, kTestFboH = 1089; // bumped for the larger font — see ImGuiBridge::init()
        constexpr float kTestW = 0.9f, kTestH = 0.7f;
        constexpr float kTestDist = 1.1f; // metres in front of the HMD

        // Billboard in front of the player: yaw-only (ignore pitch/roll) so the
        // panel stays upright and at a fixed comfortable distance/height no
        // matter which way the player is looking, rather than sitting at a
        // fixed world-space point that can end up anywhere relative to them.
        const XrPosef& hmd = m_impl->last_hmd_pose;
        const XrQuaternionf& hq = hmd.orientation;
        const float fwd_x = -2.0f*(hq.x*hq.z + hq.w*hq.y);
        const float fwd_z =  2.0f*hq.x*hq.x + 2.0f*hq.y*hq.y - 1.0f;
        const float raw_yaw = std::atan2f(fwd_x, -fwd_z);
        // Smooth the yaw the panel billboards to, not the raw per-frame HMD yaw:
        // following the headset 1:1 reads as shaky/jittery on every small head
        // movement, since even tiny tracking noise immediately re-angles the
        // panel. Exponential smoothing (shortest-angle, to avoid a jump across
        // the +-pi wrap) makes the panel "catch up" over a few frames instead —
        // still tracks where you're looking, just without the twitchiness. Only
        // the billboard orientation is smoothed here; Follow Left/Right Hand and
        // Left/Right Side (below) override position (or the whole pose) with
        // their own already-stable source and don't need this.
        static bool  s_yaw_init = false;
        static float s_smoothed_yaw = 0.0f;
        // A dead zone, not a delay: a small head turn within kYawDeadzone never
        // moves the panel at all (it's just reading a nearby part of it, not
        // turning to look elsewhere). The moment you turn past that margin it
        // starts following immediately (no wait), but very slowly, so it still
        // reads as "catching up" rather than snapping.
        constexpr float kYawDeadzone = 0.26f; // ~15 degrees — was ~5, widened per request
        if (!s_yaw_init) {
            s_smoothed_yaw = raw_yaw;
            s_yaw_init = true;
        } else {
            float delta = raw_yaw - s_smoothed_yaw;
            while (delta >  3.14159265f) delta -= 2.0f * 3.14159265f;
            while (delta < -3.14159265f) delta += 2.0f * 3.14159265f;
            if (std::fabs(delta) >= kYawDeadzone) {
                s_smoothed_yaw += delta * 0.008f; // even slower catch-up — was 0.02, slowed further per request
            }
        }
        const float yaw = s_smoothed_yaw;
        // Panel faces back toward the player, i.e. its forward normal is
        // -forward(yaw) — that's a yaw rotation of -yaw, not +yaw.
        const float half_panel_yaw = -yaw * 0.5f;
        XrPosef kTestPose{};
        kTestPose.orientation = {0.0f, std::sinf(half_panel_yaw), 0.0f, std::cosf(half_panel_yaw)};
        kTestPose.position = {
            hmd.position.x + std::sinf(yaw) * kTestDist,
            hmd.position.y,
            hmd.position.z - std::cosf(yaw) * kTestDist,
        };

        auto test_get_float = [&](XrAction a) {
            XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
            return (xrGetActionStateFloat(m_impl->session, &gi, &s) == XR_SUCCESS && s.isActive)
                   ? s.currentState : 0.0f;
        };
        auto test_get_vec2 = [&](XrAction a, float& x, float& y) {
            XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
            if (xrGetActionStateVector2f(m_impl->session, &gi, &s) == XR_SUCCESS && s.isActive) {
                x = s.currentState.x; y = s.currentState.y;
            } else {
                x = 0.0f; y = 0.0f;
            }
        };
        auto test_get_controller_pose = [&](XrSpace hand_space, XrPosef& out) -> bool {
            if (hand_space == XR_NULL_HANDLE || m_impl->app_space == XR_NULL_HANDLE) return false;
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            if (xrLocateSpace(hand_space, m_impl->app_space, predicted_time, &loc) != XR_SUCCESS) return false;
            const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((loc.locationFlags & needed) != needed) return false;
            out = loc.pose;
            return true;
        };

        // Interface > Placement > Position: overrides the default headset-billboard
        // kTestPose computed above. Follow Left/Right Hand tracks that controller's
        // *position* only — orientation deliberately stays the upright, face-the-
        // player billboard already computed above (CLAUDE.md: panels in this app use
        // a fixed world orientation, never a controller's raw grip orientation).
        // Earlier bug: directly assigning hand_pose.orientation to the panel meant a
        // relaxed/downward-pointing hand could leave the panel lying flat, facing the
        // ceiling — this is the fix. Left Side/Right Side used to be selectable here
        // too; removed as Position choices since that placement is now always-on
        // during gameplay (see the automatic side-panel block below), not a choice
        // for this single on-demand, menu-button-triggered panel.
        {
            const int position_mode = m_vr_state.menu_position_mode;
            if (position_mode == 0 || position_mode == 1) { // Follow Left/Right Hand
                const bool left = (position_mode == 0);
                XrPosef hand_pose{};
                if (test_get_controller_pose(left ? m_impl->laim_space : m_impl->raim_space, hand_pose)) {
                    // Small offset toward the player along the same forward direction
                    // used for the headset billboard (fwd_x/fwd_z, computed above from
                    // HMD yaw) — keeps the panel just off the hand without adopting the
                    // hand's own (potentially arbitrary) facing direction.
                    constexpr float kHandOffset = 0.12f;
                    kTestPose.position = {
                        hand_pose.position.x + std::sinf(yaw) * kHandOffset,
                        hand_pose.position.y - 0.03f,
                        hand_pose.position.z - std::cosf(yaw) * kHandOffset,
                    };
                    // orientation intentionally left as the billboard computed above.
                }
            }
            // position_mode == 2 (Follow Headset) needs no override — that's the
            // billboard already computed above.
        }

        // Raycast one hand's aim ray against a given panel pose; returns true and
        // fills u/v (0-1) on a hit. Same math as the real panel raycaster
        // (openxr_shell.cpp ~7627-7655), just against a single fixed-size panel
        // instead of iterating PanelDesc candidates. Parameterized by pose (not
        // closed over a single global one) so it works for the on-demand
        // menu-button panel and the two always-on gameplay side panels alike.
        auto test_raycast_hand = [&](XrSpace aim_space, const XrPosef& pose, float& out_u, float& out_v,
                                      XrVector3f& out_origin, XrVector3f& out_hit) -> bool {
            if (aim_space == XR_NULL_HANDLE) return false;
            XrPosef aim{};
            if (!test_get_controller_pose(aim_space, aim)) return false;
            const XrVector3f& O = aim.position;
            out_origin = O;
            const XrQuaternionf& aq = aim.orientation;
            XrVector3f D;
            D.x = -2.0f*(aq.x*aq.z + aq.w*aq.y);
            D.y =  2.0f*(aq.w*aq.x - aq.y*aq.z);
            D.z =  2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f;

            const XrQuaternionf& pq = pose.orientation;
            XrVector3f N; N.x = 2.0f*(pq.w*pq.y + pq.x*pq.z); N.y = 2.0f*(pq.y*pq.z - pq.w*pq.x); N.z = 1.0f - 2.0f*pq.x*pq.x - 2.0f*pq.y*pq.y;
            const XrVector3f& P = pose.position;
            float dN = D.x*N.x + D.y*N.y + D.z*N.z;
            if (std::abs(dN) <= 0.001f) return false;
            float t = ((P.x-O.x)*N.x + (P.y-O.y)*N.y + (P.z-O.z)*N.z) / dN;
            if (t <= 0.01f) return false;

            XrVector3f H{ O.x+t*D.x, O.y+t*D.y, O.z+t*D.z };
            XrVector3f right; right.x = 1.0f - 2.0f*(pq.y*pq.y + pq.z*pq.z); right.y = 2.0f*(pq.x*pq.y + pq.w*pq.z); right.z = 2.0f*(pq.x*pq.z - pq.w*pq.y);
            XrVector3f up; up.x = 2.0f*(pq.x*pq.y - pq.w*pq.z); up.y = 1.0f - 2.0f*(pq.x*pq.x + pq.z*pq.z); up.z = 2.0f*(pq.y*pq.z + pq.w*pq.x);
            float dx = H.x-P.x, dy = H.y-P.y, dz = H.z-P.z;
            float u =  (dx*right.x + dy*right.y + dz*right.z) / (kTestW * 0.5f) * 0.5f + 0.5f;
            float v = -(dx*up.x    + dy*up.y    + dz*up.z)    / (kTestH * 0.5f) * 0.5f + 0.5f;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
            out_u = u; out_v = v;
            out_hit = H;
            return true;
        };

        // Per-panel persistent laser-claim state. instance 0 = on-demand
        // menu-button panel; 1/2 = the two always-on gameplay side panels. Only
        // {0} or {1,2} is ever live in a given frame (mutually exclusive by
        // m_menu_open), but each instance keeps its own slot so switching
        // between them never inherits a stale hand claim from another panel.
        struct MenuPanelState {
            int  active_hand = 0; // 0=none, 1=right, 2=left
            bool right_trig_prev = false, left_trig_prev = false;
            bool right_hover_prev = false, left_hover_prev = false;
        };
        static MenuPanelState s_panel_state[3];

        // Per-instance last-rendered texture, cached so an instance that isn't
        // driven with a real ImGui frame this tick (see "contested" below)
        // still has something correct of its own to display, instead of a
        // torn/foreign texture from whichever instance rendered last into a
        // shared FBO (that WAS the bug: ImGuiBridge used to have a single
        // shared output texture, so two panels drawn in the same tick both
        // ended up displaying whichever one rendered last — now each instance
        // gets its own FBO slot, see ImGuiBridge::end_frame's `slot` param).
        static GLuint s_last_tex[3] = {};

        struct PanelInteraction {
            float mouse_x = -1.0f, mouse_y = -1.0f;
            bool  mouse_down = false;
            bool  hovered = false; // either hand's laser is on this panel right now
            float placement_alpha = 1.0f;
            float scroll_wheel = 0.0f;
        };

        // Computes this instance's hover/claim/click/alpha state for `pose` —
        // cheap (raycasts only, no ImGui calls), safe to run for every visible
        // instance every tick regardless of whether it ends up actually being
        // drawn this tick.
        auto compute_panel_interaction = [&](int instance, const XrPosef& pose) -> PanelInteraction {
            MenuPanelState& st = s_panel_state[instance];
            PanelInteraction out;
            // Interface > Placement > Transparency: Automatic ramps opacity up as
            // either laser ray's closest approach to the panel shrinks, hitting full
            // opacity once actually intersecting it (right_hit/left_hit below) — pure
            // ray-distance, no gaze/dwell involved, per the settled UX design. Applies
            // identically to the gameplay side panels, not just the on-demand one.
            {
                float ru = 0, rv = 0, lu = 0, lv = 0;
                XrVector3f r_origin{}, r_hit{}, l_origin{}, l_hit{};
                const bool right_hit = test_raycast_hand(m_impl->raim_space, pose, ru, rv, r_origin, r_hit);
                const bool left_hit  = test_raycast_hand(m_impl->laim_space, pose, lu, lv, l_origin, l_hit);
                out.hovered = right_hit || left_hit;
                // OR in (not overwrite) — reset once per frame before the instance
                // loop below, so any panel this hand is currently pointing at keeps
                // that hand's trigger reserved for the menu instead of also firing
                // as real emulator/game input (see m_right/left_hand_menu_hover).
                if (right_hit) m_right_hand_menu_hover = true;
                if (left_hit)  m_left_hand_menu_hover  = true;

                {
                    const int transparency_mode = m_vr_state.menu_transparency_mode;
                    if (transparency_mode == 1) out.placement_alpha = 0.25f;
                    else if (transparency_mode == 2) out.placement_alpha = 0.50f;
                    else if (transparency_mode == 3) out.placement_alpha = 1.00f;
                    else { // Automatic
                        if (right_hit || left_hit) {
                            out.placement_alpha = 1.0f;
                        } else {
                            auto ray_dist_to_point = [&](XrSpace aim_space, const XrVector3f& point) -> float {
                                XrPosef aim{};
                                if (!test_get_controller_pose(aim_space, aim)) return 1e9f;
                                const XrVector3f& O = aim.position;
                                const XrQuaternionf& aq = aim.orientation;
                                const XrVector3f D{ -2.0f*(aq.x*aq.z + aq.w*aq.y),
                                                      2.0f*(aq.w*aq.x - aq.y*aq.z),
                                                      2.0f*aq.x*aq.x + 2.0f*aq.y*aq.y - 1.0f };
                                const XrVector3f PO{ point.x - O.x, point.y - O.y, point.z - O.z };
                                float dot = PO.x*D.x + PO.y*D.y + PO.z*D.z;
                                if (dot < 0.0f) dot = 0.0f; // behind the controller — treat as far
                                const XrVector3f closest{ O.x + D.x*dot, O.y + D.y*dot, O.z + D.z*dot };
                                const float dx = point.x-closest.x, dy = point.y-closest.y, dz = point.z-closest.z;
                                return std::sqrtf(dx*dx + dy*dy + dz*dz);
                            };
                            const float dist_r = ray_dist_to_point(m_impl->raim_space, pose.position);
                            const float dist_l = ray_dist_to_point(m_impl->laim_space, pose.position);
                            const float min_dist = std::min(dist_r, dist_l);
                            // Exponential falloff: 5% at rest, ramping toward 100% as the
                            // ray's closest approach nears the panel.
                            out.placement_alpha = std::clamp(0.05f + 0.95f * std::exp(-min_dist * 2.5f), 0.05f, 1.0f);
                        }
                    }
                }

                const bool right_trig = test_get_float(m_impl->act_rtrig) > 0.5f;
                const bool left_trig  = test_get_float(m_impl->act_ltrig)  > 0.5f;
                const bool right_trig_rising = right_trig && !st.right_trig_prev;
                const bool left_trig_rising  = left_trig  && !st.left_trig_prev;

                const bool right_hover_rising = right_hit && !st.right_hover_prev;
                const bool left_hover_rising  = left_hit  && !st.left_hover_prev;

                // Laser auto-shows on hover: whichever hand starts pointing at the
                // panel first claims it (no trigger needed), and stays claimed even
                // if the other hand also starts hovering meanwhile — only relinquished
                // once the claiming hand truly stops pointing at the panel (below), or
                // explicitly handed off by the other hand's trigger-pull while pointing
                // at the panel (an intentional "I'm taking over" action, not a passive
                // re-claim from merely hovering).
                if (st.active_hand == 0) {
                    if (right_hover_rising) st.active_hand = 1;
                    else if (left_hover_rising) st.active_hand = 2;
                }
                if (right_trig_rising && right_hit) st.active_hand = 1;
                else if (left_trig_rising && left_hit) st.active_hand = 2;
                // Relinquish only when the claiming hand truly stops pointing at the panel.
                if (st.active_hand == 1 && !right_hit) st.active_hand = 0;
                if (st.active_hand == 2 && !left_hit)  st.active_hand = 0;

                if (st.active_hand == 1 && right_hit) {
                    out.mouse_x = ru * kTestFboW; out.mouse_y = rv * kTestFboH; out.mouse_down = right_trig;
                    overlay.show_laser = true; overlay.laser_origin = r_origin; overlay.laser_end = r_hit;
                } else if (st.active_hand == 2 && left_hit) {
                    out.mouse_x = lu * kTestFboW; out.mouse_y = lv * kTestFboH; out.mouse_down = left_trig;
                    overlay.show_laser = true; overlay.laser_origin = l_origin; overlay.laser_end = l_hit;
                }

                // TEMP DEBUG for the "side panels don't react to trigger" report —
                // logs only on a state change so it doesn't spam. Remove once resolved.
                {
                    static int  s_dbg_last_active[3] = {-99, -99, -99};
                    static bool s_dbg_last_rh[3] = {}, s_dbg_last_lh[3] = {};
                    static bool s_dbg_last_rt[3] = {}, s_dbg_last_lt[3] = {};
                    static bool s_dbg_last_md[3] = {};
                    if (st.active_hand != s_dbg_last_active[instance] || right_hit != s_dbg_last_rh[instance] ||
                        left_hit != s_dbg_last_lh[instance] || right_trig != s_dbg_last_rt[instance] ||
                        left_trig != s_dbg_last_lt[instance] || out.mouse_down != s_dbg_last_md[instance]) {
                        LOGI("MENU_PANEL_DBG inst=%d active=%d right_hit=%d left_hit=%d right_trig=%d left_trig=%d "
                             "rrise=%d lrise=%d mouse=(%.0f,%.0f) down=%d",
                             instance, st.active_hand, right_hit, left_hit, right_trig, left_trig,
                             right_trig_rising, left_trig_rising, out.mouse_x, out.mouse_y, out.mouse_down);
                        s_dbg_last_active[instance] = st.active_hand;
                        s_dbg_last_rh[instance] = right_hit; s_dbg_last_lh[instance] = left_hit;
                        s_dbg_last_rt[instance] = right_trig; s_dbg_last_lt[instance] = left_trig;
                        s_dbg_last_md[instance] = out.mouse_down;
                    }
                }

                st.right_trig_prev = right_trig;
                st.left_trig_prev  = left_trig;
                st.right_hover_prev = right_hit;
                st.left_hover_prev  = left_hit;
            }

            // Thumbstick up/down scrolls the active hand's panel content — same
            // deadzone/scale a normal mouse wheel notch would produce, read from
            // whichever hand currently owns the laser (mirrors mouse_x/y/down above).
            {
                float sx = 0.0f, sy = 0.0f;
                if (st.active_hand == 1) test_get_vec2(m_impl->act_rstick, sx, sy);
                else if (st.active_hand == 2) test_get_vec2(m_impl->act_lstick, sx, sy);
                if (std::fabs(sy) > 0.15f) out.scroll_wheel = sy * 0.5f;
            }
            return out;
        };

        // Actually drives a real ImGui frame for `instance` at `pose` using a
        // previously-computed PanelInteraction, and caches the resulting
        // texture in s_last_tex[instance]. Only call this for an instance that
        // "wins" this tick (see the contested-instance logic below) — an
        // instance that doesn't get a real frame simply keeps showing its
        // last cached texture via push_panel(), which is now safe since each
        // instance renders into its OWN FBO slot (ImGuiBridge::end_frame's
        // `slot` param), not a shared one.
        auto draw_panel_imgui = [&](int instance, const PanelInteraction& in) {
            m_impl->imgui_bridge.begin_frame(kTestFboW, kTestFboH, in.mouse_x, in.mouse_y, in.mouse_down,
                                              in.scroll_wheel);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2((float)kTestFboW, (float)kTestFboH));
            ImGui::PushID(instance);
            ImGui::Begin("QuestRetroDepth Menu", nullptr,
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoTitleBar);
            draw_unified_menu();
            ImGui::End();
            ImGui::PopID();
            GLuint imgui_tex = m_impl->imgui_bridge.end_frame(m_impl->renderer, kTestFboW, kTestFboH, instance);
            if (imgui_tex) s_last_tex[instance] = imgui_tex;
        };

        auto push_panel = [&](const XrPosef& pose, GLuint tex, float alpha) {
            if (tex && overlay.panel_count < OverlayInfo::k_max_panels) {
                PanelInfo& pi = overlay.panels[overlay.panel_count++];
                pi.tex   = tex;
                pi.pose  = pose;
                pi.w     = kTestW;
                pi.h     = kTestH;
                pi.alpha = alpha;
            }
        };

        // Hidden via the B+Y chord above: skip entirely when the old menu system
        // is active, so the old real panel system stays unobstructed when active.
        // Reset once per frame -- set back to true below (per-hand, OR'd across
        // however many panel instances render this frame) only while that hand's
        // laser is actually on a menu panel right now.
        m_right_hand_menu_hover = false;
        m_left_hand_menu_hover  = false;
        // The five-point lightgun calibration needs an unobstructed view of the
        // frozen game frame and its targets, so no menu panel is drawn while it
        // runs. This used to switch debug_show_new_ui off instead, which handed
        // the session back to the old bitmap panel system and never handed it
        // back — the menu came back as the old UI for the rest of the session.
        if (m_impl->debug_show_new_ui && !m_gun_calibration_active) {
            if (m_menu_open) {
                // On-demand full menu, opened by the physical left-controller menu
                // button — single instance, positioned per Interface > Placement > Position.
                // Only one instance is ever live here, so no contention possible.
                const PanelInteraction in0 = compute_panel_interaction(0, kTestPose);
                draw_panel_imgui(0, in0);
                push_panel(kTestPose, s_last_tex[0], in0.placement_alpha);
                // Library hover-dwell preview's depth: real world-space layer
                // quads beside the menu, since ImGui itself is one flat image.
                build_library_preview_diorama(overlay, kTestPose, kTestW, kTestH,
                                              in0.placement_alpha);
            } else if (!m_current_rom_name.empty() && !m_edit_mode && !m_ctrlmap_mode &&
                       m_active_sub_panel == 0 && !m_locomotion_active && !m_layer_deck_active) {
                // Gameplay, menu button not pressed: the SAME unified menu is now
                // automatically shown mirrored at both side-wing positions (the same
                // world-fixed spot the old side-panel bar used to occupy, reusing
                // compute_side_bar_wing_poses with drop_m=0 for eye-level placement)
                // instead of requiring an explicit open — replaces the old
                // always-visible side-panel bar/wings system entirely (see the
                // now-gated-off `!m_menu_open && m_side_bar_tex` block and the
                // `side_panel_mode != kSidePanelOff` block below, both now
                // `!debug_show_new_ui`-only). Each side keeps its own laser-claim
                // state (instances 1/2), and its own Automatic-transparency ramp
                // based on that side's own laser proximity, so a hand near one
                // side doesn't affect the other's alpha or claim.
                XrPosef anchor_pose{};
                float canvas_w = 0.0f;
                const bool have_anchor = game_canvas_anchor_pose(
                    m_render_layer_refs, m_config, m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el,
                    m_canvas_scale, anchor_pose, canvas_w);
                XrPosef wing_l{}, wing_r{};
                compute_side_bar_wing_poses(have_anchor ? anchor_pose : hmd, hmd, wing_l, wing_r, 0.0f);

                const PanelInteraction in1 = compute_panel_interaction(1, wing_l);
                const PanelInteraction in2 = compute_panel_interaction(2, wing_r);

                // "Dumb screenshot until hovered": a side panel that's not currently
                // being pointed at doesn't need a real ImGui frame every tick at
                // all — its content isn't changing on its own, and no one's reading
                // it closely at ~8% opacity. So it just keeps showing its last
                // cached texture (near-free: push_panel only, no ImGui/GL work) and
                // sits at a fixed low "ghost" alpha, bypassing the Interface >
                // Placement > Transparency setting (that picker is for the
                // on-demand panel; these two are meant to read as inert until
                // touched, not follow a user-chosen opacity while idle). The
                // moment either hand's laser lands on one, THAT one alone wakes up
                // to a real per-tick ImGui frame at full opacity — this also
                // doubles as the fix for the two panels' shared-ImGui-context
                // click-edge corruption (see the "contested" note below): with at
                // most one side ever hovered by a given hand at a time, and full
                // drive gated on hover, the two essentially never both drive in
                // the same tick under normal single-hand use.
                constexpr float kGhostAlpha = 0.08f;
                static bool s_ever_drawn[3] = {false, false, false}; // force one real frame each, so there's something to ghost
                bool want1 = in1.hovered || !s_ever_drawn[1];
                bool want2 = in2.hovered || !s_ever_drawn[2];

                // Rare edge case: both hands pointing at (and clicking) DIFFERENT
                // panels in the exact same tick. Only one can safely drive this
                // tick without corrupting the other's click-edge state (see the
                // earlier note in this file about the shared ImGui context) — the
                // loser just waits one tick, imperceptible for a menu click.
                static int s_driving_side = 0; // 0=none, 1=left, 2=right
                const bool contested = in1.mouse_down && in2.mouse_down;
                bool drive1 = want1, drive2 = want2;
                if (contested) {
                    if (s_driving_side != 1 && s_driving_side != 2) s_driving_side = 1;
                    drive1 = (s_driving_side == 1);
                    drive2 = (s_driving_side == 2);
                } else {
                    s_driving_side = 0;
                }

                if (drive1) { draw_panel_imgui(1, in1); s_ever_drawn[1] = true; }
                if (drive2) { draw_panel_imgui(2, in2); s_ever_drawn[2] = true; }
                push_panel(wing_l, s_last_tex[1], in1.hovered ? in1.placement_alpha : kGhostAlpha);
                push_panel(wing_r, s_last_tex[2], in2.hovered ? in2.placement_alpha : kGhostAlpha);
            }
        }
    }

    // Parallax peek: compute per-eye-independent head offsets once before the loop.
    float parallax_yaw = 0.0f, parallax_pitch = 0.0f;
    if (m_vr_state.parallax_ratio > 0.0f) {
        const XrQuaternionf& q = m_impl->last_hmd_pose.orientation;
        const float siny  = 2.0f * (q.w * q.y + q.x * q.z);
        const float cosy  = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
        const float sinp  = 2.0f * (q.w * q.x - q.y * q.z);
        const float hmd_yaw   = std::atan2f(siny, cosy);
        const float hmd_pitch = std::asinf(std::clamp(sinp, -1.0f, 1.0f));
        parallax_yaw   = (m_canvas_az - hmd_yaw)   * m_vr_state.parallax_ratio;
        parallax_pitch = (hmd_pitch - m_canvas_el) * m_vr_state.parallax_ratio;
    }


    // Render each eye
    m_impl->renderer.begin_gpu_timer();
    // Latch the PSX zero-copy frame for this XR frame so both eyes sample the
    // same emulator image rather than whatever happens to be published as each
    // eye is drawn.
    qrd::psx_gpu_frame_begin_xr_frame();
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

        // Background tint: use the selected app-world preset as the clear-color fallback first.
        // The sky dome is drawn immediately after the clear, but this fallback keeps the whole
        // app-owned projection coherent if a dome edge or runtime clip boundary is exposed.
        float bg_r = 0.01f, bg_g = 0.01f, bg_b = 0.02f;
        if (!pt_active && m_vr_state.bg_preset_index >= 0 && m_vr_state.bg_preset_index < 16) {
            const int idx = m_vr_state.bg_preset_index;
            if (idx < 8) {
                const BgSolidPreset& p = kBgSolidPresets[idx];
                bg_r = p.r;
                bg_g = p.g;
                bg_b = p.b;
            } else {
                const BgGradientPreset& p = kBgGradientPresets[idx - 8];
                // The dome supplies the actual vertical gradient; use its midpoint as the
                // clear fallback so no exposed framebuffer area jumps to the old dark tint.
                bg_r = (p.top_r + p.bot_r) * 0.5f;
                bg_g = (p.top_g + p.bot_g) * 0.5f;
                bg_b = (p.top_b + p.bot_b) * 0.5f;
            }
        } else if (m_edit_mode)   { bg_r = 0.00f; bg_g = 0.04f; bg_b = 0.01f; } // dark green
        else if (m_menu_open) { bg_r = 0.04f; bg_g = 0.03f; bg_b = 0.00f; } // dark yellow
        else if (m_active_sub_panel == 2) { bg_r = 0.00f; bg_g = 0.035f; bg_b = 0.04f; } // dark teal
        else if (m_active_sub_panel == 3) { bg_r = 0.01f; bg_g = 0.03f; bg_b = 0.04f; }
        else if (m_active_sub_panel == 7) { bg_r = 0.05f; bg_g = 0.03f; bg_b = 0.01f; }

        const bool overlay_active = overlay.panel_count > 0 || overlay.show_laser || overlay.show_laser2 ||
                                     overlay.show_gun || overlay.show_gun2 || overlay.show_calibration_target;
        m_impl->renderer.render_eye(e.fbos[img_idx], view, proj, reveal_render_layer_refs,
                                    m_cached_frame_out.psx_depth.get(), render_state,
                                    m_canvas_x, m_canvas_y, m_canvas_az, m_canvas_el, render_canvas_scale,
                                    overlay_active ? &overlay : nullptr,
                                    environment_ptr,
                                    bg_r, bg_g, bg_b, pt_active ? 0.0f : 1.0f,
                                    pt_active,
                                    parallax_yaw, parallax_pitch,
                                    m_impl->last_hmd_pose.position.x,
                                    m_impl->last_hmd_pose.position.y,
                                    m_impl->last_hmd_pose.position.z,
                                    rom_transition_alpha(),
                                    layer_deck_slots,
                                    layer_deck_slot_count,
                                    m_layer_deck_spread);

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
    m_impl->renderer.end_gpu_timer();

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
    if (m_save_state_panel_tex) { glDeleteTextures(1, &m_save_state_panel_tex); m_save_state_panel_tex = 0; }
    if (m_code_panel_tex)     { glDeleteTextures(1, &m_code_panel_tex);     m_code_panel_tex     = 0; }
    if (m_ctrlmap_panel_tex)  { glDeleteTextures(1, &m_ctrlmap_panel_tex);  m_ctrlmap_panel_tex  = 0; }
    if (m_help_panel_tex)     { glDeleteTextures(1, &m_help_panel_tex);     m_help_panel_tex     = 0; }
    if (m_rom_hint_tex)       { glDeleteTextures(1, &m_rom_hint_tex);       m_rom_hint_tex        = 0; }
    if (m_perf_overlay_tex)   { glDeleteTextures(1, &m_perf_overlay_tex);   m_perf_overlay_tex    = 0; }
    if (m_side_bar_tex)       { glDeleteTextures(1, &m_side_bar_tex);       m_side_bar_tex          = 0; }
    if (m_bg_color_panel_tex) { glDeleteTextures(1, &m_bg_color_panel_tex); m_bg_color_panel_tex    = 0; }
    if (m_themes_panel_tex)   { glDeleteTextures(1, &m_themes_panel_tex);   m_themes_panel_tex      = 0; }
    m_rom_browser.destroy_texture();

    m_impl->imgui_bridge.shutdown();
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
