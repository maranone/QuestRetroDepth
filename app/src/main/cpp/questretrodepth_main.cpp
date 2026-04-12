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
#include <android/asset_manager_jni.h>
#include <android/log.h>

#define LOG_TAG "QuestRetroDepthXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#include "emulator_backend.h"
#include "experimental_rumble.h"
#include "openxr_shell.h"

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
    if (path_has_segment(rom_path, "/roms/genesis/") || path_has_segment(rom_path, "\\roms\\genesis\\"))
        return qrd::BackendKind::Genesis;
    if (path_has_segment(rom_path, "/roms/snes/") || path_has_segment(rom_path, "\\roms\\snes\\"))
        return qrd::BackendKind::Snes;

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
        path_has_segment(raw_path, "/roms/snes/") || path_has_segment(raw_path, "\\roms\\snes\\");
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
static constexpr int64_t k_genesis_frame_ns = 16'666'667LL; // 60 Hz default for Genesis phase 1.

static int64_t current_backend_frame_ns() {
    if (g_backend_kind.has_value() && *g_backend_kind == qrd::BackendKind::Genesis)
        return k_genesis_frame_ns;
    return k_snes_frame_ns;
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
                        const auto rumble_events = g_experimental_rumble.evaluate_frame(*backend);
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

    auto* backend = ensure_backend(detect_backend_kind_from_path(rom_path));
    if (!backend) return make_jstring(env, "Backend creation failed.");

    std::string error;
    if (!backend->load_content(rom_path, error))
        return make_jstring(env, "ROM load failed\n\n" + error);

    qrd::EmulatorInputState input;
    backend->step_frame(input, error);
    g_last_status = "ROM loaded\n\n" + rom_path + "\n\n" + backend->backend_name();
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
        if (frame.width > 0 && !frame.rgba8888.empty()) {
            g_cached_frame     = frame;
            g_has_cached_frame = true;
        }
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
