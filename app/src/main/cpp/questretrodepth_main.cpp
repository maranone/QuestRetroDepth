#include <jni.h>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <cctype>
#include <cstring>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#define LOG_TAG "QuestRetroDepthXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#include "emulator_backend.h"
#include "experimental_rumble.h"
#include "game_config.h"
#include "gles_renderer.h"
#include "layer_processor.h"
#include "mat4.h"
#include "openxr_shell.h"
#include "presentation_shared.h"
#include "settings_io.h"
#include "vr_state.h"

namespace {

std::mutex                         g_backend_mutex;
std::unique_ptr<qrd::EmulatorBackend> g_backend;
std::optional<qrd::BackendKind>    g_backend_kind;
qrd::OpenXrShell                   g_openxr_shell;
std::string                        g_last_status;
std::string                        g_last_loaded_rom_filename; // filename only, for prefs persistence
std::string                        g_last_loaded_rom_prefs_name;
std::string                        g_last_loaded_game_name;
std::string                        g_last_working_rom_path;   // full path of last successful load
std::optional<qrd::BackendKind>    g_last_working_backend_kind;

// JNI context kept for cross-thread callbacks (set in nativeStartVr)
static JavaVM* g_vm               = nullptr;
static jobject g_activity_global  = nullptr;
static AAssetManager* g_asset_manager = nullptr;
static qrd::ExperimentalRumbleManager g_experimental_rumble;
static std::string g_rumble_root;

// Ask Kotlin to extract an archive (zip/7z) and return the path to the ROM inside.
// Falls through to the original path if extraction is not needed or fails.
std::string prepare_rom_path(const std::string& raw_path) {
    const auto dot = raw_path.rfind('.');
    if (dot == std::string::npos) return raw_path;
    std::string ext = raw_path.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext != ".zip" && ext != ".7z") return raw_path; // already a raw ROM

    if (!g_vm || !g_activity_global) return raw_path;

    JNIEnv* env  = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_vm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    jclass   cls = env->GetObjectClass(g_activity_global);
    jmethodID mid = env->GetMethodID(cls, "prepareRomFileForNative",
                                     "(Ljava/lang/String;)Ljava/lang/String;");
    std::string result = raw_path;
    if (mid) {
        jstring jpath   = env->NewStringUTF(raw_path.c_str());
        auto    jresult = (jstring)env->CallObjectMethod(g_activity_global, mid, jpath);
        env->DeleteLocalRef(jpath);
        if (jresult) {
            const char* chars = env->GetStringUTFChars(jresult, nullptr);
            if (chars) { result = chars; env->ReleaseStringUTFChars(jresult, chars); }
            env->DeleteLocalRef(jresult);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    if (attached) g_vm->DetachCurrentThread();
    return result;
}

std::string lowercase_copy(std::string text) {
    for (auto& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}

std::string basename_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool path_has_segment(const std::string& path, const char* segment) {
    return lowercase_copy(path).find(segment) != std::string::npos;
}

bool path_ends_with(const std::string& path, const char* ext) {
    const std::string lower = lowercase_copy(path);
    const std::string lower_ext = lowercase_copy(ext);
    return lower.size() >= lower_ext.size() &&
           lower.compare(lower.size() - lower_ext.size(), lower_ext.size(), lower_ext) == 0;
}

qrd::BackendKind detect_backend_kind_from_path(const std::string& rom_path) {
    // Folder-based detection takes highest priority
    if (path_has_segment(rom_path, "/roms/genesis/") || path_has_segment(rom_path, "\\roms\\genesis\\"))
        return qrd::BackendKind::Genesis;
    if (path_has_segment(rom_path, "/roms/snes/") || path_has_segment(rom_path, "\\roms\\snes\\"))
        return qrd::BackendKind::Snes;
    if (path_has_segment(rom_path, "/roms/gba/") || path_has_segment(rom_path, "\\roms\\gba\\"))
        return qrd::BackendKind::Gba;
    if (path_has_segment(rom_path, "/roms/gb/") || path_has_segment(rom_path, "\\roms\\gb\\") ||
        path_has_segment(rom_path, "/roms/gbc/") || path_has_segment(rom_path, "\\roms\\gbc\\"))
        return qrd::BackendKind::Gb;
    if (path_has_segment(rom_path, "/roms/nes/") || path_has_segment(rom_path, "\\roms\\nes\\"))
        return qrd::BackendKind::Nes;
    if (path_has_segment(rom_path, "/roms/pce/") || path_has_segment(rom_path, "\\roms\\pce\\"))
        return qrd::BackendKind::Pce;
    if (path_has_segment(rom_path, "/roms/sms/") || path_has_segment(rom_path, "\\roms\\sms\\"))
        return qrd::BackendKind::Sms;
    if (path_has_segment(rom_path, "/roms/gg/") || path_has_segment(rom_path, "\\roms\\gg\\"))
        return qrd::BackendKind::Sms;

    // Extension-based detection
    if (path_ends_with(rom_path, ".gba"))
        return qrd::BackendKind::Gba;
    if (path_ends_with(rom_path, ".gb") || path_ends_with(rom_path, ".gbc"))
        return qrd::BackendKind::Gb;
    if (path_ends_with(rom_path, ".nes"))
        return qrd::BackendKind::Nes;
    if (path_ends_with(rom_path, ".pce"))
        return qrd::BackendKind::Pce;
    if (path_ends_with(rom_path, ".sms") || path_ends_with(rom_path, ".gg"))
        return qrd::BackendKind::Sms;
    if (path_ends_with(rom_path, ".md") || path_ends_with(rom_path, ".bin") ||
        path_ends_with(rom_path, ".gen") || path_ends_with(rom_path, ".smd")) {
        return qrd::BackendKind::Genesis;
    }
    return qrd::BackendKind::Snes;
}

qrd::BackendKind resolve_backend_kind(const std::string& raw_path, const std::string& prepared_path) {
    const auto raw_kind = detect_backend_kind_from_path(raw_path);
    const auto prepared_kind = detect_backend_kind_from_path(prepared_path);

    const bool raw_has_system_folder =
        path_has_segment(raw_path, "/roms/genesis/") || path_has_segment(raw_path, "\\roms\\genesis\\") ||
        path_has_segment(raw_path, "/roms/snes/") || path_has_segment(raw_path, "\\roms\\snes\\") ||
        path_has_segment(raw_path, "/roms/nes/") || path_has_segment(raw_path, "\\roms\\nes\\") ||
        path_has_segment(raw_path, "/roms/pce/") || path_has_segment(raw_path, "\\roms\\pce\\") ||
        path_has_segment(raw_path, "/roms/sms/") || path_has_segment(raw_path, "\\roms\\sms\\") ||
        path_has_segment(raw_path, "/roms/gg/")  || path_has_segment(raw_path, "\\roms\\gg\\") ||
        path_has_segment(raw_path, "/roms/gba/") || path_has_segment(raw_path, "\\roms\\gba\\") ||
        path_has_segment(raw_path, "/roms/gb/")  || path_has_segment(raw_path, "\\roms\\gb\\") ||
        path_has_segment(raw_path, "/roms/gbc/") || path_has_segment(raw_path, "\\roms\\gbc\\");
    if (raw_has_system_folder) return raw_kind;

    return prepared_kind;
}

qrd::EmulatorBackend* ensure_backend(qrd::BackendKind wanted) {
    if (!g_backend || !g_backend_kind.has_value() || *g_backend_kind != wanted) {
        g_backend.reset();
        g_backend = qrd::create_backend(wanted);
        g_backend_kind = wanted;
        g_last_status = g_backend
            ? std::string("Backend ready: ") + g_backend->backend_name()
            : (std::string("Backend creation failed for ") + qrd::backend_kind_name(wanted) + ".");
    }
    return g_backend.get();
}

jstring make_jstring(JNIEnv* env, const std::string& text) {
    return env->NewStringUTF(text.c_str());
}

// ============================================================
// Dedicated emulator thread — runs snes9x at exactly 60.0988 Hz
// independently of the VR render rate (72/90/120 Hz).
// The XR thread just reads the latest completed frame.
// ============================================================
static std::thread              g_emu_thread;
static std::atomic<bool>        g_emu_stop{true};
static std::mutex               g_input_write_mutex;
static qrd::EmulatorInputState  g_pending_input;

// Double buffer: emu thread writes back, XR thread reads front. FrameOutput
// owns vectors, so publish/read must be guarded while copying.
static std::mutex               g_frame_mutex;
static qrd::FrameOutput         g_frame_buf[2];
static std::atomic<int>         g_frame_front{0};
static std::atomic<bool>        g_has_frame{false};
static std::atomic<uint64_t>    g_frame_seq{0};

// Emulator freeze control: when frozen, the emu thread skips step_frame.
// step_one: step exactly one frame then re-freeze.
static std::atomic<bool> g_emu_frozen{false};
static std::atomic<bool> g_emu_step_one{false};
static std::atomic<bool> g_auto_frame_skip{false};

static constexpr int64_t k_snes_frame_ns    = 16'639'267LL; // 1/60.0988 s (NTSC)
static constexpr int64_t k_genesis_frame_ns = 16'666'667LL; // 60 Hz
static constexpr int64_t k_gba_frame_ns     = 16'743'022LL; // 1/59.7275 s (GBA NTSC)

static int64_t current_backend_frame_ns() {
    if (!g_backend_kind.has_value()) return k_snes_frame_ns;
    switch (*g_backend_kind) {
    case qrd::BackendKind::Genesis: return k_genesis_frame_ns;
    case qrd::BackendKind::Gba:
    case qrd::BackendKind::Gb:      return k_gba_frame_ns;
    case qrd::BackendKind::Nes:     return k_snes_frame_ns;
    case qrd::BackendKind::Pce:     return k_snes_frame_ns; // PCE NTSC ~59.82 Hz
    default:                        return k_snes_frame_ns;
    }
}

static void emu_thread_main() {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now();
    static int log_ctr = 0;

    while (!g_emu_stop.load(std::memory_order_relaxed)) {
        bool frozen   = g_emu_frozen.load(std::memory_order_acquire);
        bool step_one = frozen && g_emu_step_one.exchange(false, std::memory_order_acq_rel);

        if (!frozen || step_one) {
            {
                std::lock_guard<std::mutex> lock(g_backend_mutex);
                auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
                if (backend) {
                    backend->set_auto_frame_skip(g_auto_frame_skip.load(std::memory_order_acquire));
                    qrd::EmulatorInputState inp;
                    { std::lock_guard<std::mutex> il(g_input_write_mutex); inp = g_pending_input; }

                    std::string err;
                    if (backend->step_frame(inp, err)) {
                        const uint64_t rumble_now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        const auto rumble_events = g_experimental_rumble.evaluate_frame(*backend, rumble_now_ms);
                        const auto& frame = backend->frame_output();
                        if (frame.width > 0 && !frame.rgba8888.empty()) {
                            {
                                std::lock_guard<std::mutex> fl(g_frame_mutex);
                                int back = 1 - g_frame_front.load(std::memory_order_relaxed);
                                g_frame_buf[back] = frame;
                                g_frame_front.store(back, std::memory_order_release);
                                g_frame_seq.fetch_add(1, std::memory_order_release);
                                g_has_frame.store(true, std::memory_order_release);
                            }
                            if (log_ctr++ % 600 == 0)
                                LOGI("emu_thread: %ux%u running", frame.width, frame.height);
                        }
                        for (const auto& event : rumble_events) {
                            g_openxr_shell.enqueue_haptic(event);
                        }
                    } else if (!err.empty() && log_ctr++ % 300 == 0) {
                        LOGI("emu_thread: step_frame failed: %s", err.c_str());
                    }
                }
            }
        }

        const auto frame_duration = std::chrono::nanoseconds(current_backend_frame_ns());
        const auto now = Clock::now();
        deadline += frame_duration;
        if (deadline < now) {
            // Loading/switching ROMs can leave the emu thread behind; don't keep
            // catching up with stale input after the game resumes.
            deadline = now;
        } else {
            std::this_thread::sleep_until(deadline);
        }
    }
}

static void start_emu_thread() {
    if (g_emu_thread.joinable()) return;
    g_emu_stop.store(false);
    g_emu_thread = std::thread(emu_thread_main);
}

static void stop_emu_thread() {
    g_emu_frozen.store(false);  // unfreeze so the thread can exit cleanly
    g_emu_stop.store(true);
    if (g_emu_thread.joinable()) g_emu_thread.join();
}

// XR render thread: feed latest input, return latest completed frame immediately.
bool frame_provider_for_vr(qrd::FrameOutput& out_frame, qrd::EmulatorInputState& input,
                           uint64_t& last_seen_seq) {
    { std::lock_guard<std::mutex> il(g_input_write_mutex); g_pending_input = input; }
    if (!g_has_frame.load(std::memory_order_acquire)) return false;
    const uint64_t seq = g_frame_seq.load(std::memory_order_acquire);
    if (seq != last_seen_seq) {
        std::lock_guard<std::mutex> fl(g_frame_mutex);
        const uint64_t locked_seq = g_frame_seq.load(std::memory_order_acquire);
        int front = g_frame_front.load(std::memory_order_acquire);
        out_frame = g_frame_buf[front];
        last_seen_seq = locked_seq;
    }
    return out_frame.width > 0 && !out_frame.rgba8888.empty();
}

// Legacy cached-frame kept for the 2-D launcher path (nativeStepFrame etc.)
static bool g_has_cached_frame = false;
static qrd::FrameOutput g_cached_frame;

using MobileEnvironmentSample = qrd::OpenXrShell::EnvironmentSphereSample;
using MobileBlackoutRevealPhase = qrd::OpenXrShell::BlackoutRevealPhase;

struct MobileRendererState {
    std::mutex mutex;
    GlesRenderer renderer;
    bool renderer_ready = false;
    bool surface_ready = false;
    int surface_width = 0;
    int surface_height = 0;
    qrd::BackendKind backend_kind = qrd::BackendKind::Snes;
    GameConfig config = qrd::presentation::default_config_for_backend(qrd::BackendKind::Snes, 3);
    VrState vr_state{};
    qrd::FrameOutput cached_frame;
    uint64_t last_frame_seq = 0;
    std::vector<LayerFrame> layer_frames;
    std::vector<LayerFrame*> render_refs;
    qrd::OpenXrShell::EnvironmentSphereSample environment_sample;
    qrd::OpenXrShell::BlackoutRevealPhase reveal_phase = qrd::OpenXrShell::BlackoutRevealPhase::Normal;
    int blackout_candidate_frames = 0;
    int blackout_visible_frames = 0;
    int64_t reveal_start_ns = 0;
    std::vector<std::string> reveal_layer_ids;
    std::vector<std::string> layer_names;
    std::vector<int> layer_order;
    std::vector<bool> layer_enabled;
    std::vector<bool> layer_ambilight;
    int layer_auto_dup_percent = 75;
    int snes_filter_mode = 3;
    float orbit_yaw = 0.0f;
    float orbit_pitch = 0.12f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    float zoom = 1.0f;
} g_mobile_renderer;

static int64_t monotonic_time_ns() {
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static VrState default_mobile_vr_state(qrd::BackendKind kind) {
    return qrd::presentation::default_vr_state_for_backend(kind);
}

static GameConfig default_game_config_for_backend(qrd::BackendKind kind) {
    return qrd::presentation::default_config_for_backend(kind, 3);
}

static std::string get_settings_directory_from_activity(JNIEnv* env, jobject activity) {
    if (!env || !activity) return {};
    jclass cls = env->GetObjectClass(activity);
    if (!cls) return {};
    jmethodID mid = env->GetMethodID(cls, "getSettingsDirectory", "()Ljava/lang/String;");
    std::string out;
    if (mid) {
        jstring js = (jstring)env->CallObjectMethod(activity, mid);
        if (js) {
            const char* cstr = env->GetStringUTFChars(js, nullptr);
            if (cstr) {
                out = cstr;
                env->ReleaseStringUTFChars(js, cstr);
            }
            env->DeleteLocalRef(js);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    return out;
}

static const char* mobile_backend_settings_subdir(qrd::BackendKind kind) {
    switch (kind) {
    case qrd::BackendKind::Genesis: return "genesis";
    case qrd::BackendKind::Nes:     return "nes";
    case qrd::BackendKind::Gba:     return "gba";
    case qrd::BackendKind::Gb:      return "gb";
    case qrd::BackendKind::Pce:     return "pce";
    case qrd::BackendKind::Sms:     return "sms";
    default:                        return "snes";
    }
}

static bool mobile_sniff_settings_layer_mode(const std::string& path, int& mode_out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    mode_out = 3;
    int num_layers_out = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if (strcmp(key, "layer_filter_mode") == 0) {
            const int mode = atoi(val);
            if (mode >= 0 && mode <= 3) mode_out = mode;
        } else if (strcmp(key, "num_layers") == 0) {
            num_layers_out = atoi(val);
        }
    }
    fclose(f);
    if (num_layers_out == 11) mode_out = 1;
    else if (num_layers_out == 16) mode_out = 0;
    return true;
}

static void mobile_load_shared_settings_locked(JNIEnv* env, jobject activity, qrd::BackendKind kind) {
    const std::string root_dir = get_settings_directory_from_activity(env, activity);
    if (root_dir.empty()) return;
    const std::string system_dir = root_dir + "/" + mobile_backend_settings_subdir(kind);
    std::string path = system_dir + "/global.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        path = root_dir + "/global.ini";
        f = fopen(path.c_str(), "r");
    }
    if (!f) return;
    fclose(f);

    int sniffed_mode = 3;
    mobile_sniff_settings_layer_mode(path, sniffed_mode);
    g_mobile_renderer.snes_filter_mode = sniffed_mode;
    g_mobile_renderer.config = qrd::presentation::default_config_for_backend(kind, sniffed_mode);
    g_mobile_renderer.vr_state = qrd::presentation::default_vr_state_for_backend(kind);
    g_mobile_renderer.layer_names.clear();
    g_mobile_renderer.layer_order.clear();
    g_mobile_renderer.layer_enabled.clear();
    g_mobile_renderer.layer_ambilight.clear();
    qrd::presentation::ensure_layer_runtime_state_matches_config(
        g_mobile_renderer.config,
        g_mobile_renderer.layer_names,
        g_mobile_renderer.layer_order,
        g_mobile_renderer.layer_enabled,
        g_mobile_renderer.layer_ambilight);

    settings_load(
        path,
        g_mobile_renderer.vr_state,
        g_mobile_renderer.config,
        g_mobile_renderer.layer_order,
        g_mobile_renderer.layer_enabled,
        g_mobile_renderer.layer_ambilight,
        &g_mobile_renderer.snes_filter_mode,
        &g_mobile_renderer.layer_auto_dup_percent);

    if (kind == qrd::BackendKind::Snes) {
        g_mobile_renderer.config = qrd::presentation::default_config_for_backend(kind, g_mobile_renderer.snes_filter_mode);
        settings_load(
            path,
            g_mobile_renderer.vr_state,
            g_mobile_renderer.config,
            g_mobile_renderer.layer_order,
            g_mobile_renderer.layer_enabled,
            g_mobile_renderer.layer_ambilight,
            nullptr,
            &g_mobile_renderer.layer_auto_dup_percent);
    }

    qrd::presentation::ensure_layer_runtime_state_matches_config(
        g_mobile_renderer.config,
        g_mobile_renderer.layer_names,
        g_mobile_renderer.layer_order,
        g_mobile_renderer.layer_enabled,
        g_mobile_renderer.layer_ambilight);
}

static std::vector<int> mobile_default_layer_order_for_config(const GameConfig& config) {
    const int n = (int)config.layers.size();
    std::vector<int> order;
    order.reserve(n);
    for (int i = n - 1; i >= 0; --i) order.push_back(i);
    return order;
}

static void mobile_sync_cached_layer_geometry_from_config(std::vector<LayerFrame>& layer_frames,
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

static int mobile_baseline_copy_count(const LayerFrame& frame) {
    return frame.copies.empty() ? GlesRenderer::k_max_copies : (int)frame.copies.size();
}

static float mobile_baseline_copy_step(const LayerFrame& frame) {
    if (!frame.copies.empty() && frame.copies.back() > 0.0f) {
        return frame.copies.back() / (float)frame.copies.size();
    }
    return GlesRenderer::k_default_copy_step;
}

static void mobile_rebuild_copy_offsets(std::vector<float>& copies, int copy_count, float copy_step) {
    if (copy_count <= 0) {
        copies.clear();
        return;
    }
    copies.resize(copy_count);
    for (int i = 0; i < copy_count; ++i) copies[i] = (float)(i + 1) * copy_step;
}

static void mobile_apply_layer_auto_dup_visible(std::vector<LayerFrame*>& layer_frames, int auto_dup_percent) {
    if (auto_dup_percent < 0 || layer_frames.empty() || !layer_frames[0]) return;
    const int anchor_count = mobile_baseline_copy_count(*layer_frames[0]);
    const int far_target = std::clamp((int)std::lround((double)anchor_count * (double)auto_dup_percent / 100.0), 0, 64);
    const int n = (int)layer_frames.size();
    for (int i = 0; i < n; ++i) {
        if (!layer_frames[i]) continue;
        const float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        const int target_count = std::clamp((int)std::lround(anchor_count + (far_target - anchor_count) * t), 0, 64);
        mobile_rebuild_copy_offsets(layer_frames[i]->copies, target_count, mobile_baseline_copy_step(*layer_frames[i]));
    }
}

static void mobile_compact_visible_layer_depths(std::vector<LayerFrame*>& layer_frames) {
    if (layer_frames.size() < 2) return;
    LayerFrame* first = nullptr;
    for (LayerFrame* lf : layer_frames) if (lf) { first = lf; break; }
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

static void mobile_apply_perspective_comp_to_refs(std::vector<LayerFrame*>& refs) {
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

static bool mobile_sample_environment_band(const LayerFrame& frame, int y0, int y1,
                                           std::array<float, 4>& out_color) {
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
    const int clamped_y0 = std::clamp(y0, 0, frame.height - 1);
    const int clamped_y1 = std::clamp(y1, clamped_y0 + 1, frame.height);
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    for (int y = clamped_y0; y < clamped_y1; y += 4) {
        for (int x = 0; x < frame.width; x += 4) {
            const std::size_t idx = (std::size_t)(y * frame.width + x) * 4u;
            const float alpha = frame.rgba[idx + 3] / 255.0f;
            if (alpha <= 0.05f) continue;
            r += (frame.rgba[idx + 0] / 255.0f) * alpha;
            g += (frame.rgba[idx + 1] / 255.0f) * alpha;
            b += (frame.rgba[idx + 2] / 255.0f) * alpha;
            a += alpha;
        }
    }
    if (a <= 0.0f) return false;
    out_color = {std::clamp(r / a, 0.0f, 1.0f), std::clamp(g / a, 0.0f, 1.0f), std::clamp(b / a, 0.0f, 1.0f), 1.0f};
    return true;
}

static bool mobile_build_environment_sample_from_layer(const LayerFrame& frame,
                                                       EnvironmentSphereMode mode,
                                                       MobileEnvironmentSample& out_sample) {
    out_sample.valid = false;
    if (!frame.has_pixels || frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
    if (mode == EnvironmentSphereMode::Off) return false;
    constexpr int kBandCount = 12;
    const int sample_h = (mode == EnvironmentSphereMode::SkyOnly) ? std::max(1, frame.height / 2) : frame.height;
    std::array<bool, kBandCount> valid{};
    for (int i = 0; i < kBandCount; ++i) {
        const int y0 = (sample_h * i) / kBandCount;
        const int y1 = (sample_h * (i + 1)) / kBandCount;
        valid[i] = mobile_sample_environment_band(frame, y0, std::max(y0 + 1, y1), out_sample.bands[i]);
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

static std::array<float, 4> mobile_lerp_rgba(const std::array<float, 4>& a,
                                             const std::array<float, 4>& b,
                                             float t) {
    return {
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
        a[2] + (b[2] - a[2]) * t,
        a[3] + (b[3] - a[3]) * t,
    };
}

static void mobile_smooth_environment_sample(MobileEnvironmentSample& current,
                                             const MobileEnvironmentSample& target,
                                             float blend) {
    if (!target.valid) {
        for (auto& band : current.bands) band[3] *= (1.0f - blend);
        current.valid = false;
        for (const auto& band : current.bands) if (band[3] > 0.01f) { current.valid = true; break; }
        return;
    }
    if (!current.valid) {
        current = target;
        return;
    }
    for (int i = 0; i < (int)current.bands.size(); ++i) current.bands[i] = mobile_lerp_rgba(current.bands[i], target.bands[i], blend);
    current.valid = true;
}

static bool mobile_layer_has_bright_samples(const LayerFrame& frame, int& bright_samples_out) {
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

static bool mobile_is_blackout_candidate(const std::vector<LayerFrame*>& frames, int& bright_samples_out) {
    bright_samples_out = 0;
    bool any_pixels = false;
    for (const LayerFrame* lf : frames) {
        if (!lf || !lf->has_pixels || lf->width <= 0 || lf->height <= 0 || lf->rgba.empty()) continue;
        any_pixels = true;
        int layer_bright = 0;
        mobile_layer_has_bright_samples(*lf, layer_bright);
        bright_samples_out += layer_bright;
    }
    if (!any_pixels) return true;
    return bright_samples_out == 0;
}

static float mobile_blackout_reveal_pulse_scale(int64_t now_ns, int64_t start_ns) {
    constexpr float kPulseDurationNs = 120000000.0f;
    const float t = std::clamp((float)(now_ns - start_ns) / kPulseDurationNs, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);
    return 1.015f - 0.015f * ease;
}

static Mat4 make_perspective_matrix(float fov_y_radians, float aspect, float near_z, float far_z) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect; p.m[1] = 0; p.m[2] = 0; p.m[3] = 0;
    p.m[4] = 0; p.m[5] = f; p.m[6] = 0; p.m[7] = 0;
    p.m[8] = 0; p.m[9] = 0; p.m[10] = (far_z + near_z) / (near_z - far_z); p.m[11] = -1.0f;
    p.m[12] = 0; p.m[13] = 0; p.m[14] = (2.0f * far_z * near_z) / (near_z - far_z); p.m[15] = 0;
    return p;
}

static Mat4 make_look_at_matrix(float eye_x, float eye_y, float eye_z,
                                float center_x, float center_y, float center_z,
                                float up_x, float up_y, float up_z) {
    float fx = center_x - eye_x;
    float fy = center_y - eye_y;
    float fz = center_z - eye_z;
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 0.0001f) { fx /= fl; fy /= fl; fz /= fl; }

    float sx = fy * up_z - fz * up_y;
    float sy = fz * up_x - fx * up_z;
    float sz = fx * up_y - fy * up_x;
    const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl > 0.0001f) { sx /= sl; sy /= sl; sz /= sl; }

    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;

    Mat4 m{};
    m.m[0] = sx; m.m[1] = ux; m.m[2] = -fx; m.m[3] = 0.0f;
    m.m[4] = sy; m.m[5] = uy; m.m[6] = -fy; m.m[7] = 0.0f;
    m.m[8] = sz; m.m[9] = uz; m.m[10] = -fz; m.m[11] = 0.0f;
    m.m[12] = -(sx * eye_x + sy * eye_y + sz * eye_z);
    m.m[13] = -(ux * eye_x + uy * eye_y + uz * eye_z);
    m.m[14] = fx * eye_x + fy * eye_y + fz * eye_z;
    m.m[15] = 1.0f;
    return m;
}

static void mobile_reset_render_config_locked(qrd::BackendKind kind) {
    g_mobile_renderer.backend_kind = kind;
    g_mobile_renderer.snes_filter_mode = 3;
    g_mobile_renderer.config = default_game_config_for_backend(kind);
    g_mobile_renderer.vr_state = default_mobile_vr_state(kind);
    g_mobile_renderer.layer_frames.clear();
    g_mobile_renderer.render_refs.clear();
    g_mobile_renderer.last_frame_seq = 0;
    g_mobile_renderer.environment_sample = {};
    g_mobile_renderer.reveal_phase = qrd::OpenXrShell::BlackoutRevealPhase::Normal;
    g_mobile_renderer.blackout_candidate_frames = 0;
    g_mobile_renderer.blackout_visible_frames = 0;
    g_mobile_renderer.reveal_start_ns = 0;
    g_mobile_renderer.reveal_layer_ids.clear();
    g_mobile_renderer.layer_names.clear();
    g_mobile_renderer.layer_order.clear();
    g_mobile_renderer.layer_enabled.clear();
    g_mobile_renderer.layer_ambilight.clear();
    g_mobile_renderer.layer_auto_dup_percent = 75;
    qrd::presentation::ensure_layer_runtime_state_matches_config(
        g_mobile_renderer.config,
        g_mobile_renderer.layer_names,
        g_mobile_renderer.layer_order,
        g_mobile_renderer.layer_enabled,
        g_mobile_renderer.layer_ambilight);
}

} // namespace

// ============================================================
// QuestRetroDepthActivity (2-D launcher)
// ============================================================

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeGetBuildInfo(
    JNIEnv* env, jobject)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    auto* backend = ensure_backend(qrd::BackendKind::Snes);
    std::string text =
        "QuestRetroDepth\n\n"
        "Backend: ";
    text += backend ? backend->backend_name() : "none";
    text += "\n\nPress 'Enter VR' to launch the 3-D view.\n"
            "Left thumbstick click = RANDOMIZE\n"
            "Right stick = adjust depth / spread\n"
            "L-trigger / L-grip = widen / narrow\n"
            "A/B/X/Y + face buttons = game input";
    g_last_status = backend ? (std::string("Backend ready: ") + backend->backend_name())
                            : "Backend creation failed.";
    return make_jstring(env, text);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeLoadRom(
    JNIEnv* env, jobject, jstring path)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    const char* raw = env->GetStringUTFChars(path, nullptr);
    std::string rom_path = raw ? raw : "";
    env->ReleaseStringUTFChars(path, raw);

    const qrd::BackendKind kind = detect_backend_kind_from_path(rom_path);
    auto* backend = ensure_backend(kind);
    if (!backend) return make_jstring(env, "Backend creation failed.");

    std::string error;
    if (!backend->load_content(rom_path, error))
        return make_jstring(env, "ROM load failed\n\n" + error);

    qrd::EmulatorInputState input;
    backend->step_frame(input, error);
    const auto& frame = backend->frame_output();
    if (frame.width == 0 || frame.rgba8888.empty()) {
        g_last_status = "ROM load failed\n\nBackend loaded but emitted no video frame.\nCheck logcat for backend errors.";
        return make_jstring(env, g_last_status);
    }
    {
        std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
        mobile_reset_render_config_locked(kind);
        g_mobile_renderer.cached_frame = frame;
        if (g_activity_global) {
            mobile_load_shared_settings_locked(env, g_activity_global, kind);
        }
    }
    g_last_status = "ROM loaded\n\n" + rom_path + "\n\n" + backend->backend_name();
    start_emu_thread();
    return make_jstring(env, g_last_status);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeStepFrame(
    JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
    if (!backend) return JNI_FALSE;
    std::string error;
    qrd::EmulatorInputState input;
    if (!backend->step_frame(input, error)) {
        g_last_status = "Frame step failed\n\n" + error;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeGetFrameWidth(
    JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
    return backend ? (jint)backend->frame_output().width : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeGetFrameHeight(
    JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
    return backend ? (jint)backend->frame_output().height : 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeCopyFramePixels(
    JNIEnv* env, jobject, jintArray out_pixels)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
    if (!backend) return JNI_FALSE;
    const auto& frame = backend->frame_output();
    const jsize frame_size = (jsize)frame.rgba8888.size();
    if (frame_size <= 0 || env->GetArrayLength(out_pixels) < frame_size) return JNI_FALSE;
    env->SetIntArrayRegion(out_pixels, 0, frame_size,
        reinterpret_cast<const jint*>(frame.rgba8888.data()));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeGetLastStatus(
    JNIEnv* env, jobject)
{
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    if (g_last_status.empty()) g_last_status = "No status yet.";
    return make_jstring(env, g_last_status);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeStartMobile(
    JNIEnv* env, jobject, jobject activity)
{
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    g_vm = vm;
    if (g_activity_global) env->DeleteGlobalRef(g_activity_global);
    g_activity_global = env->NewGlobalRef(activity);
    {
        std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
        mobile_reset_render_config_locked(g_last_working_backend_kind.value_or(qrd::BackendKind::Snes));
        mobile_load_shared_settings_locked(env, activity, g_mobile_renderer.backend_kind);
    }
    start_emu_thread();
    g_last_status = "Mobile renderer ready.";
    return make_jstring(env, g_last_status);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeStopMobile(
    JNIEnv*, jobject)
{
    stop_emu_thread();
    std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
    g_mobile_renderer.surface_ready = false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeOnMobileSurfaceCreated(
    JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
    std::string err;
    if (!g_mobile_renderer.renderer_ready) {
        g_mobile_renderer.renderer_ready = g_mobile_renderer.renderer.init(err);
        if (!g_mobile_renderer.renderer_ready) {
            g_last_status = "Mobile GLES init failed: " + err;
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeOnMobileSurfaceChanged(
    JNIEnv*, jobject, jint width, jint height)
{
    std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
    g_mobile_renderer.surface_width = (int)width;
    g_mobile_renderer.surface_height = (int)height;
    g_mobile_renderer.surface_ready = width > 0 && height > 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeSetMobileCamera(
    JNIEnv*, jobject, jfloat orbit_yaw, jfloat orbit_pitch, jfloat pan_x, jfloat pan_y, jfloat zoom)
{
    std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
    g_mobile_renderer.orbit_yaw = orbit_yaw;
    g_mobile_renderer.orbit_pitch = std::clamp((float)orbit_pitch, -1.1f, 1.1f);
    g_mobile_renderer.pan_x = pan_x;
    g_mobile_renderer.pan_y = pan_y;
    g_mobile_renderer.zoom = std::clamp((float)zoom, 0.55f, 2.5f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeSetMobileInputState(
    JNIEnv*, jobject,
    jboolean up, jboolean down, jboolean left, jboolean right,
    jboolean a, jboolean b, jboolean x, jboolean y,
    jboolean l, jboolean r, jboolean start, jboolean select)
{
    std::lock_guard<std::mutex> il(g_input_write_mutex);
    g_pending_input.dpad_up = up == JNI_TRUE;
    g_pending_input.dpad_down = down == JNI_TRUE;
    g_pending_input.dpad_left = left == JNI_TRUE;
    g_pending_input.dpad_right = right == JNI_TRUE;
    g_pending_input.button_a = a == JNI_TRUE;
    g_pending_input.button_b = b == JNI_TRUE;
    g_pending_input.button_x = x == JNI_TRUE;
    g_pending_input.button_y = y == JNI_TRUE;
    g_pending_input.button_l = l == JNI_TRUE;
    g_pending_input.button_r = r == JNI_TRUE;
    g_pending_input.button_start = start == JNI_TRUE;
    g_pending_input.button_select = select == JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeSetMobilePaused(
    JNIEnv*, jobject, jboolean paused)
{
    g_emu_frozen.store(paused == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestRetroDepthActivity_nativeOnMobileDrawFrame(
    JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> ml(g_mobile_renderer.mutex);
    if (!g_mobile_renderer.renderer_ready || !g_mobile_renderer.surface_ready) return;

    const uint64_t seq = g_frame_seq.load(std::memory_order_acquire);
    if (g_has_frame.load(std::memory_order_acquire) && seq != g_mobile_renderer.last_frame_seq) {
        {
            std::lock_guard<std::mutex> fl(g_frame_mutex);
            const int front = g_frame_front.load(std::memory_order_acquire);
            g_mobile_renderer.cached_frame = g_frame_buf[front];
            g_mobile_renderer.last_frame_seq = g_frame_seq.load(std::memory_order_acquire);
        }

        LayerProcessor proc(g_mobile_renderer.config);
        const auto& frame = g_mobile_renderer.cached_frame;
        const uint8_t* zbuf = frame.zbuffer.empty() ? nullptr : frame.zbuffer.data();
        const bool build_object_boxes = g_mobile_renderer.vr_state.depth_mode == DepthMode::BoundingBox;
        proc.process_into(
            g_mobile_renderer.layer_frames,
            frame.rgba8888.data(),
            (int)frame.width,
            (int)frame.height,
            zbuf,
            &frame,
            build_object_boxes);

        qrd::presentation::sync_cached_layer_geometry_from_config(g_mobile_renderer.layer_frames, g_mobile_renderer.config);
        qrd::presentation::ensure_layer_runtime_state_matches_config(
            g_mobile_renderer.config,
            g_mobile_renderer.layer_names,
            g_mobile_renderer.layer_order,
            g_mobile_renderer.layer_enabled,
            g_mobile_renderer.layer_ambilight);
        g_mobile_renderer.render_refs.clear();
        const std::vector<int>& order = g_mobile_renderer.layer_order;
        g_mobile_renderer.render_refs.reserve(g_mobile_renderer.layer_frames.size());
        if (!order.empty() && order.size() == g_mobile_renderer.layer_frames.size()) {
            for (int idx : order) {
                if (idx < 0 || idx >= (int)g_mobile_renderer.layer_frames.size()) continue;
                if (idx < (int)g_mobile_renderer.layer_enabled.size() && !g_mobile_renderer.layer_enabled[idx]) continue;
                auto& lf = g_mobile_renderer.layer_frames[idx];
                lf.contrib_ambilight = (idx < (int)g_mobile_renderer.layer_ambilight.size())
                    ? (bool)g_mobile_renderer.layer_ambilight[idx] : true;
                g_mobile_renderer.render_refs.push_back(&lf);
            }
        } else {
            for (auto& lf : g_mobile_renderer.layer_frames) {
                lf.contrib_ambilight = true;
                g_mobile_renderer.render_refs.push_back(&lf);
            }
        }
        qrd::presentation::apply_layer_auto_dup_visible(
            g_mobile_renderer.render_refs,
            g_mobile_renderer.layer_auto_dup_percent);
        qrd::presentation::compact_visible_layer_depths(g_mobile_renderer.render_refs);
        if (g_mobile_renderer.vr_state.perspective_comp) {
            qrd::presentation::apply_perspective_comp_to_refs(g_mobile_renderer.render_refs);
        } else {
            for (LayerFrame* lf : g_mobile_renderer.render_refs) if (lf) lf->persp_comp_scale = 1.0f;
        }

        int reveal_phase = (int)g_mobile_renderer.reveal_phase;
        qrd::presentation::update_blackout_reveal_state(
            reveal_phase,
            g_mobile_renderer.blackout_candidate_frames,
            g_mobile_renderer.blackout_visible_frames,
            g_mobile_renderer.reveal_start_ns,
            g_mobile_renderer.reveal_layer_ids,
            !g_mobile_renderer.render_refs.empty(),
            true,
            g_mobile_renderer.render_refs,
            monotonic_time_ns());
        g_mobile_renderer.reveal_phase = (qrd::OpenXrShell::BlackoutRevealPhase)reveal_phase;

        qrd::OpenXrShell::EnvironmentSphereSample target_sample{};
        const LayerFrame* source_layer = nullptr;
        float source_depth = -1.0f;
        for (const LayerFrame* lf : g_mobile_renderer.render_refs) {
            if (!lf || !lf->has_pixels || lf->rgba.empty()) continue;
            if (lf->depth_meters >= source_depth) {
                source_depth = lf->depth_meters;
                source_layer = lf;
            }
        }
        if (source_layer) {
            qrd::presentation::build_environment_sample_from_layer(
                *source_layer, g_mobile_renderer.vr_state.environment_sphere_mode, target_sample);
        }
        qrd::presentation::smooth_environment_sample(g_mobile_renderer.environment_sample, target_sample, 0.15f);
    }

    std::vector<LayerFrame*> render_refs = g_mobile_renderer.render_refs;
    float canvas_scale = g_mobile_renderer.zoom;
    const int64_t now_ns = monotonic_time_ns();
    if (g_mobile_renderer.reveal_phase == qrd::OpenXrShell::BlackoutRevealPhase::RevealAnimating) {
        constexpr float kRevealDurationNs = 500000000.0f;
        const float progress = std::clamp((float)(now_ns - g_mobile_renderer.reveal_start_ns) / kRevealDurationNs, 0.0f, 1.0f);
        const int reveal_count = std::clamp((int)std::ceil(progress * (float)g_mobile_renderer.reveal_layer_ids.size()),
                                            1, (int)g_mobile_renderer.reveal_layer_ids.size());
        render_refs.clear();
        for (LayerFrame* lf : g_mobile_renderer.render_refs) {
            if (!lf) continue;
            auto it = std::find(g_mobile_renderer.reveal_layer_ids.begin(),
                                g_mobile_renderer.reveal_layer_ids.begin() + reveal_count,
                                lf->id);
            if (it != g_mobile_renderer.reveal_layer_ids.begin() + reveal_count) render_refs.push_back(lf);
        }
        if (progress >= 1.0f) {
            g_mobile_renderer.reveal_phase = qrd::OpenXrShell::BlackoutRevealPhase::RevealCooldown;
            g_mobile_renderer.reveal_start_ns = now_ns;
        }
    } else if (g_mobile_renderer.reveal_phase == qrd::OpenXrShell::BlackoutRevealPhase::RevealCooldown) {
        canvas_scale *= qrd::presentation::blackout_reveal_pulse_scale(now_ns, g_mobile_renderer.reveal_start_ns);
        if ((now_ns - g_mobile_renderer.reveal_start_ns) >= 120000000LL) {
            g_mobile_renderer.reveal_phase = qrd::OpenXrShell::BlackoutRevealPhase::Normal;
            g_mobile_renderer.reveal_start_ns = 0;
            g_mobile_renderer.reveal_layer_ids.clear();
        }
    }

    const float aspect = std::max(1.0f, (float)g_mobile_renderer.surface_width) /
                         std::max(1.0f, (float)g_mobile_renderer.surface_height);
    const Mat4 proj = make_perspective_matrix(60.0f * 3.14159265358979323846f / 180.0f, aspect, 0.05f, 100.0f);
    const float target_x = g_mobile_renderer.pan_x;
    const float target_y = g_mobile_renderer.pan_y;
    const float target_z = -2.2f;
    const float distance = 2.3f / std::max(0.55f, g_mobile_renderer.zoom);
    const float eye_x = target_x + std::sin(g_mobile_renderer.orbit_yaw) * std::cos(g_mobile_renderer.orbit_pitch) * distance;
    const float eye_y = target_y + std::sin(g_mobile_renderer.orbit_pitch) * distance;
    const float eye_z = target_z + std::cos(g_mobile_renderer.orbit_yaw) * std::cos(g_mobile_renderer.orbit_pitch) * distance;
    const Mat4 view = make_look_at_matrix(eye_x, eye_y, eye_z, target_x, target_y, target_z, 0.0f, 1.0f, 0.0f);

    SkyDomeInfo environment_info{};
    const SkyDomeInfo* environment_ptr = nullptr;
    if (g_mobile_renderer.reveal_phase != qrd::OpenXrShell::BlackoutRevealPhase::BlackoutLatched &&
        g_mobile_renderer.reveal_phase != qrd::OpenXrShell::BlackoutRevealPhase::RevealAnimating &&
        g_mobile_renderer.vr_state.environment_sphere_mode != EnvironmentSphereMode::Off &&
        g_mobile_renderer.environment_sample.valid) {
        environment_info.enabled = true;
        environment_info.mode = g_mobile_renderer.vr_state.environment_sphere_mode;
        environment_info.bands = g_mobile_renderer.environment_sample.bands;
        environment_ptr = &environment_info;
    }

    EyeFbo target_fbo{};
    target_fbo.fbo = 0;
    target_fbo.width = g_mobile_renderer.surface_width;
    target_fbo.height = g_mobile_renderer.surface_height;
    g_mobile_renderer.renderer.render_eye(
        target_fbo,
        view,
        proj,
        render_refs,
        g_mobile_renderer.vr_state,
        g_mobile_renderer.pan_x,
        g_mobile_renderer.pan_y,
        0.0f,
        0.0f,
        canvas_scale,
        nullptr,
        environment_ptr,
        0.01f, 0.01f, 0.02f, 1.0f,
        false);
}

// ============================================================
// QuestVrActivity (OpenXR VR view)
// ============================================================

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeStartVr(
    JNIEnv* env, jobject, jobject activity, jboolean open_menu_on_startup,
    jint autosave_interval_seconds, jboolean load_last_save_enabled)
{
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    g_vm = vm;
    if (g_activity_global) env->DeleteGlobalRef(g_activity_global);
    g_activity_global = env->NewGlobalRef(activity);
    g_asset_manager = nullptr;
    {
        jclass activity_cls = env->GetObjectClass(activity);
        jmethodID get_assets = env->GetMethodID(activity_cls, "getAssets", "()Landroid/content/res/AssetManager;");
        jmethodID get_rumble_dir = env->GetMethodID(activity_cls, "getRumbleDirectory", "()Ljava/lang/String;");
        if (get_assets) {
            jobject asset_manager_obj = env->CallObjectMethod(activity, get_assets);
            if (asset_manager_obj) {
                g_asset_manager = AAssetManager_fromJava(env, asset_manager_obj);
                env->DeleteLocalRef(asset_manager_obj);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (get_rumble_dir) {
            jstring jr = (jstring)env->CallObjectMethod(activity, get_rumble_dir);
            if (jr) {
                const char* chars = env->GetStringUTFChars(jr, nullptr);
                if (chars) {
                    g_rumble_root = chars;
                    env->ReleaseStringUTFChars(jr, chars);
                }
                env->DeleteLocalRef(jr);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(activity_cls);
    }
    g_experimental_rumble.set_user_root(g_rumble_root);
    std::string rumble_error;
    if (!g_experimental_rumble.load_catalog(g_asset_manager, rumble_error) && !rumble_error.empty()) {
        LOGI("experimental rumble catalog load failed: %s", rumble_error.c_str());
    }
    g_openxr_shell.set_experimental_rumble_status(g_experimental_rumble.active_status());

    g_openxr_shell.set_frame_provider(frame_provider_for_vr);

    // Wire up emulator freeze controls so the XR thread can pause/resume the emu thread.
    g_openxr_shell.set_emu_freeze_ctrl([](bool freeze) {
        g_emu_frozen.store(freeze, std::memory_order_release);
        if (!freeze) g_emu_step_one.store(false, std::memory_order_release); // cancel any pending step
    });
    g_openxr_shell.set_emu_step_one([]() {
        g_emu_step_one.store(true, std::memory_order_release);
    });

    // Wire up vr_state callbacks so settings changes propagate to emulator thread
    g_openxr_shell.set_on_vr_state_changed([](bool auto_frame_skip) {
        g_auto_frame_skip.store(auto_frame_skip, std::memory_order_release);
    });
    g_openxr_shell.set_on_experimental_rumble_changed([](bool enabled) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        g_experimental_rumble.set_enabled(enabled);
        if (g_backend_kind.has_value()) {
            g_experimental_rumble.on_rom_loaded(
                g_asset_manager,
                *g_backend_kind,
                g_last_loaded_rom_filename,
                g_last_loaded_game_name);
        }
        g_openxr_shell.set_experimental_rumble_status(g_experimental_rumble.active_status());
    });
    g_openxr_shell.set_layer_capture_mask_ctrl([](uint32_t mask) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
        if (backend) backend->set_layer_capture_mask(mask);
    });
    g_openxr_shell.set_save_state_capture([](std::vector<uint8_t>& out, std::string& err) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
        if (!backend) {
            err = "Backend unavailable";
            return false;
        }
        return backend->save_state(out, err);
    });
    g_openxr_shell.set_save_state_apply([](const void* data, std::size_t size, std::string& err) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
        if (!backend) {
            err = "Backend unavailable";
            return false;
        }
        return backend->load_state(data, size, err);
    });

    // Give the shell a ROM loader so it can load ROMs from the browser.
    // Extracts archives via Kotlin, restores the previous ROM if load fails
    // (snes9x clears its state on failed load, which would cause "no ROM loaded").
    g_openxr_shell.set_rom_loader([](const std::string& raw_path, std::string& err) -> bool {
        const std::string path = prepare_rom_path(raw_path);
        const auto wanted_kind = resolve_backend_kind(raw_path, path);
        std::string backend_name;
        std::string game_name;
        {
            std::lock_guard<std::mutex> lock(g_backend_mutex);
            auto* backend = ensure_backend(wanted_kind);
            if (!backend) { err = "Backend unavailable"; return false; }

            if (!backend->load_content(path, err)) {
                // Restore last working ROM so the display doesn't go blank
                if (!g_last_working_rom_path.empty() && g_last_working_backend_kind == wanted_kind) {
                    std::string restore_err;
                    backend->load_content(g_last_working_rom_path, restore_err);
                    qrd::EmulatorInputState ei;
                    backend->step_frame(ei, restore_err);
                }
                return false;
            }

            qrd::EmulatorInputState empty_input;
            backend->step_frame(empty_input, err); // prime — ignore failure
            const auto& frame = backend->frame_output();
            if (frame.width > 0 && !frame.rgba8888.empty()) {
                g_cached_frame     = frame;
                g_has_cached_frame = true;
            }
            g_last_working_rom_path    = path;
            g_last_working_backend_kind = wanted_kind;
            g_last_loaded_rom_filename = basename_from_path(path);
            g_last_loaded_rom_prefs_name = basename_from_path(raw_path);
            auto info = backend->get_rom_header_info();
            if (info.has_header && !info.game_name.empty()) {
                game_name = info.game_name;
            }
            g_last_loaded_game_name = game_name;
            g_experimental_rumble.on_rom_loaded(g_asset_manager, wanted_kind, g_last_loaded_rom_filename, game_name);
            g_openxr_shell.set_experimental_rumble_status(g_experimental_rumble.active_status());
            backend_name = backend->backend_name();
        }

        g_openxr_shell.set_current_backend_kind(wanted_kind);
        g_openxr_shell.set_current_rom(g_last_loaded_rom_filename);
        if (!game_name.empty()) {
            g_openxr_shell.set_current_game_name(game_name);
        }
        LOGI("ROM loader: raw=%s prepared=%s backend=%s",
             raw_path.c_str(), path.c_str(), backend_name.c_str());
        start_emu_thread();
        return true;
    });

    std::string status;
    g_openxr_shell.start(vm, env, activity,
                         open_menu_on_startup == JNI_TRUE,
                         (int)autosave_interval_seconds,
                         load_last_save_enabled == JNI_TRUE,
                         status);
    return make_jstring(env, status);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeGetVrStatus(
    JNIEnv* env, jobject)
{
    return make_jstring(env, g_openxr_shell.status());
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeOpenMainMenu(
    JNIEnv*, jobject)
{
    g_openxr_shell.request_open_main_menu();
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeOpenHomebrew(
    JNIEnv*, jobject)
{
    g_openxr_shell.request_open_homebrew();
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeStopVr(
    JNIEnv* env, jobject)
{
    stop_emu_thread();
    g_experimental_rumble.reset_runtime();
    g_openxr_shell.stop(env);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeRandomize(
    JNIEnv*, jobject)
{
    g_openxr_shell.randomize();
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeLoadPreset(
    JNIEnv*, jobject, jint idx)
{
    g_openxr_shell.load_preset((int)idx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeSavePreset(
    JNIEnv*, jobject, jint idx)
{
    g_openxr_shell.save_preset((int)idx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeSubmitQuickPresetName(
    JNIEnv* env, jobject, jint kind, jint slot, jstring name)
{
    const char* raw = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    const std::string value = raw ? raw : "";
    if (name && raw) env->ReleaseStringUTFChars(name, raw);
    g_openxr_shell.submit_quick_preset_name((int)kind, (int)slot, value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeCancelQuickPresetName(
    JNIEnv*, jobject, jint kind, jint slot)
{
    g_openxr_shell.cancel_quick_preset_name((int)kind, (int)slot);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeGetVrStateSummary(
    JNIEnv* env, jobject)
{
    return make_jstring(env, g_openxr_shell.vr_state_summary());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeLoadRom(
    JNIEnv* env, jobject, jstring path, jstring source_name)
{
    const char* raw = env->GetStringUTFChars(path, nullptr);
    std::string rom_path = raw ? raw : "";
    env->ReleaseStringUTFChars(path, raw);
    const char* raw_source = source_name ? env->GetStringUTFChars(source_name, nullptr) : nullptr;
    std::string prefs_name = raw_source ? raw_source : "";
    if (source_name && raw_source) env->ReleaseStringUTFChars(source_name, raw_source);

    const auto wanted_kind = detect_backend_kind_from_path(rom_path);
    std::string game_name;
    {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        auto* backend = ensure_backend(wanted_kind);
        if (!backend) return make_jstring(env, "Backend creation failed.");

        std::string error;
        if (!backend->load_content(rom_path, error))
            return make_jstring(env, "ROM load failed\n\n" + error);

        qrd::EmulatorInputState input;
        backend->step_frame(input, error); // prime — ignore failure
        const auto& frame = backend->frame_output();
        if (frame.width == 0 || frame.rgba8888.empty()) {
            return make_jstring(env, "ROM load failed\n\nBackend loaded but emitted no video frame.\nCheck logcat for backend errors.");
        }
        g_cached_frame     = frame;
        g_has_cached_frame = true;
        g_last_working_rom_path = rom_path;
        g_last_working_backend_kind = wanted_kind;
        g_last_loaded_rom_filename = basename_from_path(rom_path);
        g_last_loaded_rom_prefs_name = prefs_name.empty() ? g_last_loaded_rom_filename : prefs_name;
        auto info = backend->get_rom_header_info();
        if (info.has_header && !info.game_name.empty()) {
            game_name = info.game_name;
        }
        g_last_loaded_game_name = game_name;
        g_experimental_rumble.on_rom_loaded(g_asset_manager, wanted_kind, g_last_loaded_rom_filename, game_name);
        g_openxr_shell.set_experimental_rumble_status(g_experimental_rumble.active_status());
    }

    g_openxr_shell.set_current_backend_kind(wanted_kind);
    g_openxr_shell.set_current_rom(g_last_loaded_rom_filename);
    if (!game_name.empty()) {
        g_openxr_shell.set_current_game_name(game_name);
    }
    g_last_status = "ROM loaded: " + g_last_loaded_rom_filename;
    start_emu_thread();
    return make_jstring(env, g_last_status);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeGetLastLoadedRomFilename(
    JNIEnv* env, jobject)
{
    return make_jstring(env, g_last_loaded_rom_prefs_name.empty() ? g_last_loaded_rom_filename
                                                                  : g_last_loaded_rom_prefs_name);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeGetStateCode(
    JNIEnv* env, jobject)
{
    return make_jstring(env, g_openxr_shell.get_state_code());
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeHomebrewDataReady(
    JNIEnv*, jobject)
{
    g_openxr_shell.homebrew_data_ready();
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeHomebrewDownloadComplete(
    JNIEnv*, jobject, jint /*entryIdx*/)
{
    g_openxr_shell.homebrew_download_complete();
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeSetHomebrewFeed(
    JNIEnv*, jobject, jint idx)
{
    g_openxr_shell.set_homebrew_feed((int)idx);
}

// Returns true if the code was valid and applied.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeApplyStateCode(
    JNIEnv* env, jobject, jstring jcode)
{
    const char* raw = env->GetStringUTFChars(jcode, nullptr);
    std::string code = raw ? raw : "";
    env->ReleaseStringUTFChars(jcode, raw);
    return g_openxr_shell.apply_state_code(code) ? JNI_TRUE : JNI_FALSE;
}
