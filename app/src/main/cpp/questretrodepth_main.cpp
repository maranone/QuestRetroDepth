#include <jni.h>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <sys/system_properties.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sys/stat.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#define LOG_TAG "QuestRetroDepthXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#include "audio_processor.h"
#include "emulator_backend.h"
#include "experimental_rumble.h"
#include "mgba_backend.h"
#include "mame_backend.h"
#include "mame_driver_catalog.h"
#include "pce_backend.h"
#include "psx_gl_context.h"
#include "psx_libretro_backend.h"
#include "saturn_libretro_backend.h"
#include "openxr_shell.h"
#include "presentation_shared.h"
#include "rom_title_db.h"
#include "settings_io.h"
#include "vr_state.h"

AAssetManager* g_qrd_asset_manager = nullptr;
extern "C" AAssetManager* qrd_get_asset_manager() { return g_qrd_asset_manager; }

// Per-core ROM audio channel volume setters (Audio > Channels). Each is a
// free function with C linkage backed by file-static state inside that
// core's own audio mixer — see the per-core hooks added in
// third_party/{snes9x,picodrive,fceumm,mgba,beetle-pce}/... for where each
// one actually applies its multiply. Safe to call at any time, even with no
// backend instance loaded (the state they write into is global, not
// per-instance), and calling every core's setter regardless of which one is
// actually active is harmless — only the currently-running core's mixer
// ever reads its own values.
extern "C" {
void snes_set_channel_volume(int channel, float volume);
void genesis_set_channel_volume(int channel, float volume);
void nes_set_channel_volume(int channel, float volume);
void GBAudioSetChannelVolume(int channel, float volume);
void pce_psg_set_channel_volume(int channel, float volume);
}

namespace {

std::mutex                         g_backend_mutex;
// Serializes the whole ROM/preview transition, not just individual backend
// calls. A thumbnail capture can replace the global backend and keep it for
// many frames; without this outer lock a JNI ROM load could interleave with
// that capture and leave the second load frozen or crash the process.
std::mutex                         g_rom_transition_mutex;
std::unique_ptr<qrd::EmulatorBackend> g_backend;
std::optional<qrd::BackendKind>    g_backend_kind;
std::string                        g_preview_saved_rom_path;
std::optional<qrd::BackendKind>    g_preview_saved_backend_kind;
std::vector<uint8_t>               g_preview_saved_state;
bool                               g_preview_session_saved = false;
bool                               g_preview_had_active = false;
qrd::OpenXrShell                   g_openxr_shell;
std::mutex                         g_status_mutex;
std::string                        g_last_status;
std::string                        g_last_loaded_rom_filename; // filename only, for prefs persistence
std::string                        g_last_loaded_rom_prefs_name;
std::string                        g_last_loaded_game_name;
std::string                        g_last_working_rom_path;   // full path of last successful load
std::optional<qrd::BackendKind>    g_last_working_backend_kind;
// A foreground preparation job may finish on the worker thread while the
// actual backend load is deliberately committed on the XR thread. This
// one-shot handoff prevents the 500 MB extraction from blocking rendering and
// also prevents the loader from extracting the same archive a second time.
std::mutex                         g_prepared_rom_mutex;
std::string                        g_prepared_rom_raw_path;
std::string                        g_prepared_rom_path;

// JNI context kept for cross-thread callbacks (set in nativeStartVr)
static JavaVM* g_vm               = nullptr;
static jobject g_activity_global  = nullptr;
static AAssetManager*& g_asset_manager = g_qrd_asset_manager;
static qrd::ExperimentalRumbleManager g_experimental_rumble;
static std::string g_rumble_root;

void set_last_status(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_last_status = status;
}

std::string get_last_status_copy() {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    if (g_last_status.empty()) return "No status yet.";
    return g_last_status;
}

// Ask Kotlin to extract an archive (zip/7z) and return the path to the ROM inside.
// Falls through to the original path if extraction is not needed or fails.
std::string prepare_rom_path(const std::string& raw_path) {
    {
        std::lock_guard<std::mutex> lock(g_prepared_rom_mutex);
        if (raw_path == g_prepared_rom_raw_path && !g_prepared_rom_path.empty()) {
            const std::string prepared = g_prepared_rom_path;
            g_prepared_rom_raw_path.clear();
            g_prepared_rom_path.clear();
            return prepared;
        }
    }
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

// Ask Kotlin to delete every cached extracted-archive directory (Wipe Settings).
void clear_extracted_rom_cache() {
    if (!g_vm || !g_activity_global) return;

    JNIEnv* env  = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_vm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    jclass    cls = env->GetObjectClass(g_activity_global);
    jmethodID mid = env->GetMethodID(cls, "clearExtractedRomCache", "()V");
    if (mid) {
        env->CallVoidMethod(g_activity_global, mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    if (attached) g_vm->DetachCurrentThread();
}

// Ask Kotlin how much the extracted-archive cache holds (Danger Zone display).
// Returns {file_count, total_bytes}, both 0 on any failure.
std::pair<int, long long> extracted_rom_cache_stats() {
    if (!g_vm || !g_activity_global) return {0, 0};

    JNIEnv* env  = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_vm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    std::pair<int, long long> result{0, 0};
    jclass    cls = env->GetObjectClass(g_activity_global);
    jmethodID mid = env->GetMethodID(cls, "extractedRomCacheStats", "()Ljava/lang/String;");
    if (mid) {
        auto jresult = (jstring)env->CallObjectMethod(g_activity_global, mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); jresult = nullptr; }
        if (jresult) {
            const char* chars = env->GetStringUTFChars(jresult, nullptr);
            if (chars) {
                const std::string s = chars;
                const size_t bar = s.find('|');
                if (bar != std::string::npos) {
                    result.first = atoi(s.substr(0, bar).c_str());
                    result.second = atoll(s.substr(bar + 1).c_str());
                }
                env->ReleaseStringUTFChars(jresult, chars);
            }
            env->DeleteLocalRef(jresult);
        }
    }
    env->DeleteLocalRef(cls);
    if (attached) g_vm->DetachCurrentThread();
    return result;
}

void publish_prepared_rom(const std::string& raw_path, const std::string& prepared_path) {
    std::lock_guard<std::mutex> lock(g_prepared_rom_mutex);
    g_prepared_rom_raw_path = raw_path;
    g_prepared_rom_path = prepared_path;
}

// Depth order for a backend's raw per-layer captures. Returns the layer's
// depth in metres for backend layer index `layer_index`, or a large sentinel
// when the config does not describe it (unknown layers sort to the back, so
// they never jump in front of a plane whose depth is actually known).
static float preview_layer_depth_for_index(const GameConfig& cfg, int layer_index) {
    for (const auto& lc : cfg.layers) {
        if (lc.extraction_type != ExtractionType::PerLayerCapture) continue;
        if (lc.layer_index == layer_index) return lc.depth_meters;
    }
    return 1.0e9f;
}

// Reorders a freshly captured snapshot from raw backend capture order into the
// core's real front-to-back layout (farthest plane first), using the same
// per-backend GameConfig the in-game renderer builds its layers from. Anything
// the config does not cover keeps its relative capture order.
static void order_preview_layers_by_config_depth(qrd::RomPreviewSnapshot& snap,
                                                  const std::vector<int>& source_index,
                                                  qrd::BackendKind kind) {
    if (snap.layers.size() < 2 || source_index.size() != snap.layers.size()) return;
    GameConfig cfg;
    switch (kind) {
    case qrd::BackendKind::Snes:    cfg = GameConfig::make_default_snes();    break;
    case qrd::BackendKind::Genesis: cfg = GameConfig::make_default_genesis(); break;
    case qrd::BackendKind::Sms:     cfg = GameConfig::make_default_sms();     break;
    case qrd::BackendKind::Nes:     cfg = GameConfig::make_default_nes();     break;
    case qrd::BackendKind::Gba:     cfg = GameConfig::make_default_gba();     break;
    case qrd::BackendKind::Gb:      cfg = GameConfig::make_default_gb();      break;
    case qrd::BackendKind::Pce:     cfg = GameConfig::make_default_pce();     break;
    default: return; // MAME/Saturn/PSX previews are not built from indexed plane captures
    }

    const size_t n = snap.layers.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return preview_layer_depth_for_index(cfg, source_index[a]) >
               preview_layer_depth_for_index(cfg, source_index[b]);
    });

    std::vector<qrd::RomPreviewLayer> reordered;
    reordered.reserve(n);
    for (size_t i : order) reordered.push_back(std::move(snap.layers[i]));
    snap.layers = std::move(reordered);
}

// Converts one emulator frame into a RomPreviewSnapshot, preferring the
// per-layer capture but falling back to the composed frame when a core's
// layer output is present but entirely transparent (GB/PCE). Returns false
// if the frame has nothing displayable yet.
bool snapshot_from_frame(const qrd::FrameOutput& frame, qrd::RomPreviewSnapshot& out,
                          qrd::BackendKind kind) {
    if (frame.width == 0 || frame.height == 0 || frame.rgba8888.empty()) return false;
    qrd::RomPreviewSnapshot candidate;
    candidate.source_width = frame.width;
    candidate.source_height = frame.height;
    // NOTE: these two sources use different uint32 packings and must not
    // share one extraction order — mixing them up shows as inverted/swapped
    // R and B (exactly what a per-layer thumbnail looked like before this
    // fix). frame.rgba8888 (the composed frame) is written by
    // rgba_from_rgb565()/rgba_from_xrgb8888(): 0xFF000000 | (R<<16) | (G<<8)
    // | B. frame.layers[].rgba (the raw per-layer capture) is written by
    // snes_libretro_backend's handle_video_frame as (A<<24) | (B<<16) |
    // (G<<8) | R — documented in layer_processor.cpp's
    // fill_per_layer_capture() as "byte0=R, byte1=G, byte2=B, byte3=A".
    // Backend layer index each copied layer came from, so the ordering pass
    // below can look its real depth up in the core's own GameConfig.
    std::vector<int> candidate_source_index;
    auto copy_layer = [&](int source_index, const std::vector<uint32_t>& src,
                          const std::vector<uint8_t>* depth,
                          bool is_raw_layer_capture) {
        const size_t expected = static_cast<size_t>(frame.width) * frame.height;
        if (src.size() != expected) return;
        qrd::RomPreviewLayer layer;
        layer.width = (int)frame.width;
        layer.height = (int)frame.height;
        layer.rgba.resize(src.size() * 4u);
        for (size_t p = 0; p < src.size(); ++p) {
            const uint32_t v = src[p];
            if (is_raw_layer_capture) {
                layer.rgba[p * 4u + 0] = (uint8_t)(v & 0xff);
                layer.rgba[p * 4u + 1] = (uint8_t)((v >> 8) & 0xff);
                layer.rgba[p * 4u + 2] = (uint8_t)((v >> 16) & 0xff);
                layer.rgba[p * 4u + 3] = (uint8_t)((v >> 24) & 0xff);
            } else {
                layer.rgba[p * 4u + 0] = (uint8_t)((v >> 16) & 0xff);
                layer.rgba[p * 4u + 1] = (uint8_t)((v >> 8) & 0xff);
                layer.rgba[p * 4u + 2] = (uint8_t)(v & 0xff);
                layer.rgba[p * 4u + 3] = (uint8_t)((v >> 24) & 0xff);
            }
        }
        if (depth && depth->size() == expected) layer.depth_map = *depth;
        candidate.layers.push_back(std::move(layer));
        candidate_source_index.push_back(source_index);
    };
    // SNES (snes_libretro_backend), Genesis/SMS/GG (PicoDrive, both the
    // Genesis path via picodrive_rd_get_layer_rgba/rd_rgba_from_rgb565 and
    // the SMS/GG path via picodrive_sms_lc_copy_layer_rgba/
    // sms_rgba_from_rgb565), and NES (FCEUmm) all write genuine raw
    // per-layer captures using 0xff000000u | (b<<16) | (g<<8) | r, i.e.
    // byte-order R,G,B,A. GB/GBA (mgba_backend.cpp) and PCE (pce_backend.cpp)
    // instead copy the already-composited frame.rgba8888 pixel verbatim into
    // their "layers", which uses the composite convention (B,G,R,A) and
    // needs the other extraction path.
    const bool raw_layer_capture = (kind == qrd::BackendKind::Mame ||
                                     kind == qrd::BackendKind::Snes ||
                                     kind == qrd::BackendKind::Genesis ||
                                     kind == qrd::BackendKind::Sms ||
                                     kind == qrd::BackendKind::Nes);
    for (int li = 0; li < (int)frame.layers.size(); ++li)
        copy_layer(li, frame.layers[li].rgba, &frame.layers[li].depth_map, raw_layer_capture);

    // frame.layers is in raw backend capture order (SNES: BG0,BG1,BG2,BG3,OBJ),
    // which is NOT the order those planes actually sit at in depth -- SNES BG3
    // is the far background and BG0 the near one, so a consumer stepping
    // through this array puts the whole scene back to front. Reorder to the
    // core's own layout, taken from the same GameConfig the real renderer
    // uses, far plane first.
    order_preview_layers_by_config_depth(candidate, candidate_source_index, kind);

    bool has_visible_layer = false;
    for (const auto& layer : candidate.layers) {
        for (size_t p = 3; p < layer.rgba.size(); p += 4) {
            if (layer.rgba[p] != 0) { has_visible_layer = true; break; }
        }
        if (has_visible_layer) break;
    }
    if ((!has_visible_layer || candidate.layers.empty()) && !frame.rgba8888.empty()) {
        candidate.layers.clear();
        candidate_source_index.clear();
        // Neo Geo (and any other future MAME driver synthesizing a
        // z-buffer instead of exporting named layers) has no per-layer
        // capture at all -- frame.layers is always empty for it -- so
        // without this branch every thumbnail falls through to the plain
        // one-flat-layer copy below and never shows depth, even though
        // the same frame's z-buffer is exactly what the real in-game
        // renderer uses to build multiple ZBuffer layers via
        // GameConfig::update_z_splits(). Mirror that here: one
        // RomPreviewLayer per distinct z value found, masked to just the
        // pixels at that depth, so the shelf thumbnail gets genuine
        // depth-separated art instead of a flat composite.
        const size_t expected = static_cast<size_t>(frame.width) * frame.height;
        bool zsplit_applied = false;
        if (kind == qrd::BackendKind::Mame && frame.zbuffer.size() == expected &&
            frame.rgba8888.size() == expected) {
            bool occupied[256] = {false};
            for (uint8_t z : frame.zbuffer) occupied[z] = true;
            std::vector<int> z_values;
            for (int z = 0; z < 256; ++z) if (occupied[z]) z_values.push_back(z);
            // Same cap as GameConfig::update_z_splits's 0..63 scan range
            // plus a sane upper bound on thumbnail layer count.
            if (!z_values.empty() && z_values.size() <= 16) {
                for (int z : z_values) {
                    qrd::RomPreviewLayer layer;
                    layer.width = (int)frame.width;
                    layer.height = (int)frame.height;
                    layer.rgba.assign(expected * 4u, 0u);
                    for (size_t p = 0; p < expected; ++p) {
                        if (frame.zbuffer[p] != (uint8_t)z) continue;
                        const uint32_t v = frame.rgba8888[p];
                        layer.rgba[p * 4u + 0] = (uint8_t)((v >> 16) & 0xff);
                        layer.rgba[p * 4u + 1] = (uint8_t)((v >> 8) & 0xff);
                        layer.rgba[p * 4u + 2] = (uint8_t)(v & 0xff);
                        layer.rgba[p * 4u + 3] = (uint8_t)((v >> 24) & 0xff);
                    }
                    candidate.layers.push_back(std::move(layer));
                }
                zsplit_applied = true;
            }
        }
        if (!zsplit_applied) copy_layer(-1, frame.rgba8888, &frame.depth_map, false);
    }
    if (candidate.layers.empty()) return false;
    out = std::move(candidate);
    return true;
}

std::string lowercase_copy(std::string text) {
    for (auto& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}

std::string basename_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Decides what the post-load hint tooltip should say for this ROM: a missing
// BIOS warning takes priority (it's the more actionable problem), otherwise
// falls back to a "flat 2D, no depth layers" notice when the chosen config
// has nothing to work with. Call after set_current_backend_kind() (so the
// config it inspects is current) and before set_current_rom() (which is what
// actually triggers the tooltip to show).
static void update_rom_hint_for_load(const qrd::EmulatorBackend* backend) {
    std::string warning = backend ? backend->last_load_warning() : std::string();
    if (!warning.empty()) {
        g_openxr_shell.set_rom_hint_override(warning);
    } else if (g_openxr_shell.current_config_is_single_layer()) {
        g_openxr_shell.set_rom_hint_override(
            "This ROM has no per-game depth layers yet.\nShowing flat 2D.");
    } else {
        g_openxr_shell.set_rom_hint_override("");
    }
}

bool path_has_segment(const std::string& path, const char* segment) {
    return lowercase_copy(path).find(segment) != std::string::npos;
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
    if (path_has_segment(rom_path, "/roms/saturn/") || path_has_segment(rom_path, "\\roms\\saturn\\"))
        return qrd::BackendKind::Saturn;
    if (path_has_segment(rom_path, "/roms/psx/") || path_has_segment(rom_path, "\\roms\\psx\\") ||
        path_has_segment(rom_path, "/roms/ps1/") || path_has_segment(rom_path, "\\roms\\ps1\\"))
        return qrd::BackendKind::Psx;
    if (path_has_segment(rom_path, "/roms/mame/") || path_has_segment(rom_path, "\\roms\\mame\\"))
        return qrd::BackendKind::Mame;
    // Unknown folders and archive paths are treated as MAME. The official
    // system roots above are the only way to select a console backend; this
    // lets arbitrary arcade/system folders (for example /roms/playstation/)
    // use MAME without weakening the explicit /roms/snes/... behavior. File
    // extensions are deliberately ignored here: a ROM outside an official
    // system tree belongs to the MAME fallback by policy.
    return qrd::BackendKind::Mame;
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
        path_has_segment(raw_path, "/roms/gb/")     || path_has_segment(raw_path, "\\roms\\gb\\") ||
        path_has_segment(raw_path, "/roms/gbc/")    || path_has_segment(raw_path, "\\roms\\gbc\\") ||
        path_has_segment(raw_path, "/roms/saturn/") || path_has_segment(raw_path, "\\roms\\saturn\\") ||
        path_has_segment(raw_path, "/roms/psx/") || path_has_segment(raw_path, "\\roms\\psx\\") ||
        path_has_segment(raw_path, "/roms/ps1/") || path_has_segment(raw_path, "\\roms\\ps1\\") ||
        path_has_segment(raw_path, "/roms/mame/")   || path_has_segment(raw_path, "\\roms\\mame\\");
    if (raw_has_system_folder) return raw_kind;

    return prepared_kind;
}

bool is_neogeo_mame_preview_path(const std::string& path) {
    if (detect_backend_kind_from_path(path) != qrd::BackendKind::Mame) return false;
    // The dedicated folder is authoritative for Neo Geo ROMs. Do not use the
    // friendly-title database as a fallback here: the full MAME title map now
    // contains every arcade driver, so a title match does not imply Neo Geo.
    if (path_has_segment(path, "/roms/mame/neogeo/") ||
        path_has_segment(path, "\\roms\\mame\\neogeo\\")) return true;
    return false;
}

bool is_saturn_mame_preview_path(const std::string& path) {
    if (detect_backend_kind_from_path(path) != qrd::BackendKind::Mame) return false;
    // Saturn is currently implemented through the MAME backend. Keep the
    // folder authoritative so an arbitrary MAME/arcade set cannot
    // accidentally enter the Saturn thumbnail path.
    return path_has_segment(path, "/roms/saturn/") ||
           path_has_segment(path, "\\roms\\saturn\\");
}

qrd::EmulatorBackend* ensure_backend(qrd::BackendKind wanted) {
    if (!g_backend || !g_backend_kind.has_value() || *g_backend_kind != wanted) {
        g_backend.reset();
        g_backend = qrd::create_backend(wanted);
        g_backend_kind = wanted;
        set_last_status(g_backend
            ? std::string("Backend ready: ") + g_backend->backend_name()
            : (std::string("Backend creation failed for ") + qrd::backend_kind_name(wanted) + "."));
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
static qrd::EmulatorInputState  g_bt_input;      // BT gamepad, OR'd into Touch input each XR frame

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
// Per-core auto-frame-skip, indexed by (int)qrd::BackendKind. Populated from
// VrState by the applier wired below; NES's slot (qrd::BackendKind::Nes)
// simply never gets set (FCEUmm has no frameskip hook), so it stays at its
// default false forever.
static std::atomic<bool> g_auto_frame_skip[10]{};
// Mirrors VrState::psx_gpu_resolution for the load path, which runs before the
// shell has applied settings. Keep the two defaults in step.
static std::atomic<int>  g_psx_gpu_resolution{4};
// Mirrors VrState::psx_texture_filter for the load path, same as above.
static std::atomic<int>  g_psx_texture_filter{0};
static bool auto_frame_skip_for(qrd::BackendKind kind) {
    return g_auto_frame_skip[static_cast<int>(kind)].load(std::memory_order_acquire);
}
static std::atomic<int64_t> g_backend_frame_ns{16'639'267LL};

static constexpr int64_t k_snes_frame_ns    = 16'639'267LL; // 1/60.0988 s (NTSC)
static constexpr int64_t k_genesis_frame_ns = 16'666'667LL; // 60 Hz
static constexpr int64_t k_gba_frame_ns     = 16'743'022LL; // 1/59.7275 s (GBA NTSC)

static int64_t frame_ns_from_hz(double hz, int64_t fallback_ns) {
    if (hz <= 0.0) return fallback_ns;
    const double ns = 1'000'000'000.0 / hz;
    return ns > 0.0 ? static_cast<int64_t>(ns + 0.5) : fallback_ns;
}

static int64_t current_backend_frame_ns() {
    return g_backend_frame_ns.load(std::memory_order_acquire);
}
static void emu_thread_main() {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now();
    static int log_ctr = 0;
    static int perf_log_ctr = 0;

    while (!g_emu_stop.load(std::memory_order_relaxed)) {
        bool frozen   = g_emu_frozen.load(std::memory_order_acquire);
        bool step_one = frozen && g_emu_step_one.exchange(false, std::memory_order_acq_rel);

        if (!frozen || step_one) {
            {
                std::lock_guard<std::mutex> lock(g_backend_mutex);
                const auto active_kind = g_backend_kind.value_or(qrd::BackendKind::Snes);
                auto* backend = ensure_backend(active_kind);
                if (backend) {
                    const int64_t frame_ns = frame_ns_from_hz(backend->frame_rate_hz(), k_snes_frame_ns);
                    g_backend_frame_ns.store(frame_ns, std::memory_order_release);
                    backend->set_auto_frame_skip(auto_frame_skip_for(active_kind));
                    qrd::EmulatorInputState inp;
                    { std::lock_guard<std::mutex> il(g_input_write_mutex); inp = g_pending_input; }

                    std::string err;
                    const auto step_start = Clock::now();
                    if (backend->step_frame(inp, err)) {
                        const auto step_end = Clock::now();
                        const float step_ms = std::chrono::duration<float, std::milli>(step_end - step_start).count();
                        const uint64_t rumble_now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        const auto rumble_events = g_experimental_rumble.evaluate_frame(*backend, rumble_now_ms);
                        const auto& frame = backend->frame_output();
                        if (frame.width > 0 && !frame.rgba8888.empty()) {
                            const auto publish_start = Clock::now();
                            {
                                std::lock_guard<std::mutex> fl(g_frame_mutex);
                                int back = 1 - g_frame_front.load(std::memory_order_relaxed);
                                g_frame_buf[back] = frame;
                                g_frame_front.store(back, std::memory_order_release);
                                g_frame_seq.fetch_add(1, std::memory_order_release);
                                g_has_frame.store(true, std::memory_order_release);
                            }
                            const float publish_ms = std::chrono::duration<float, std::milli>(
                                Clock::now() - publish_start).count();
                            if (log_ctr++ % 600 == 0)
                                LOGI("emu_thread: %ux%u running", frame.width, frame.height);
                            if (g_backend_kind.has_value() &&
                                *g_backend_kind == qrd::BackendKind::Genesis &&
                                (++perf_log_ctr % 120 == 0 || step_ms > 16.7f || publish_ms > 4.0f)) {
                                LOGI("Genesis perf: step=%.2f ms publish_copy=%.2f ms frame=%ux%u layers=%zu",
                                     step_ms, publish_ms, frame.width, frame.height, frame.layers.size());
                            }
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

static void clear_published_emulation_state() {
    {
        std::lock_guard<std::mutex> il(g_input_write_mutex);
        g_pending_input = {};
        g_bt_input = {};
    }
    {
        std::lock_guard<std::mutex> fl(g_frame_mutex);
        g_frame_buf[0] = {};
        g_frame_buf[1] = {};
        g_frame_front.store(0, std::memory_order_release);
        g_frame_seq.fetch_add(1, std::memory_order_release);
        g_has_frame.store(false, std::memory_order_release);
    }
    g_emu_step_one.store(false, std::memory_order_release);
    g_backend_frame_ns.store(k_snes_frame_ns, std::memory_order_release);
}

static void begin_fresh_rom_load() {
    stop_emu_thread();
    clear_published_emulation_state();
}

static qrd::EmulatorBackend* recreate_backend_locked(qrd::BackendKind wanted) {
    g_backend.reset();
    g_backend_kind.reset();
    return ensure_backend(wanted);
}

// XR render thread: feed latest input, return latest completed frame immediately.
bool frame_provider_for_vr(qrd::FrameOutput& out_frame, qrd::EmulatorInputState& input,
                           uint64_t& last_seen_seq) {
    using Clock = std::chrono::steady_clock;
    static int provider_perf_log_ctr = 0;
    {
        std::lock_guard<std::mutex> il(g_input_write_mutex);
        g_pending_input = input;
        g_pending_input.dpad_up    |= g_bt_input.dpad_up;
        g_pending_input.dpad_down  |= g_bt_input.dpad_down;
        g_pending_input.dpad_left  |= g_bt_input.dpad_left;
        g_pending_input.dpad_right |= g_bt_input.dpad_right;
        g_pending_input.button_a   |= g_bt_input.button_a;
        g_pending_input.button_b   |= g_bt_input.button_b;
        g_pending_input.button_x   |= g_bt_input.button_x;
        g_pending_input.button_y   |= g_bt_input.button_y;
        g_pending_input.button_l   |= g_bt_input.button_l;
        g_pending_input.button_r   |= g_bt_input.button_r;
        g_pending_input.button_start  |= g_bt_input.button_start;
        g_pending_input.button_select |= g_bt_input.button_select;
    }
    if (!g_has_frame.load(std::memory_order_acquire)) return false;
    const uint64_t seq = g_frame_seq.load(std::memory_order_acquire);
    if (seq != last_seen_seq) {
        const auto copy_start = Clock::now();
        std::lock_guard<std::mutex> fl(g_frame_mutex);
        const uint64_t locked_seq = g_frame_seq.load(std::memory_order_acquire);
        int front = g_frame_front.load(std::memory_order_acquire);
        out_frame = g_frame_buf[front];
        last_seen_seq = locked_seq;
        const float copy_ms = std::chrono::duration<float, std::milli>(Clock::now() - copy_start).count();
        if (g_backend_kind.has_value() &&
            *g_backend_kind == qrd::BackendKind::Genesis &&
            (++provider_perf_log_ctr % 120 == 0 || copy_ms > 4.0f)) {
            LOGI("Genesis perf: xr_frame_copy=%.2f ms frame=%ux%u layers=%zu",
                 copy_ms, out_frame.width, out_frame.height, out_frame.layers.size());
        }
    }
    return out_frame.width > 0 && !out_frame.rgba8888.empty();
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

static void configure_mgba_frontend_dirs_from_activity(JNIEnv* env, jobject activity) {
    const std::string root_dir = get_settings_directory_from_activity(env, activity);
    if (root_dir.empty()) return;
    const std::string mgba_root = root_dir + "/mgba";
    mkdir(mgba_root.c_str(), 0755);
    const std::string system_dir = mgba_root + "/system";
    const std::string save_dir = mgba_root + "/saves";
    mkdir(system_dir.c_str(), 0755);
    mkdir(save_dir.c_str(), 0755);
    qrd::set_mgba_frontend_directories(system_dir, save_dir);

    // MAME (CPS1/CPS2): reporting root_dir as the libretro "system directory"
    // makes MAME's own rompath auto-search <root_dir>/mame/bios and
    // <root_dir>/mame/roms (mame_libretro's Set_Path_Option(), CORE_NAME =
    // "mame") -- this is the sdcard BIOS folder users drop shared romsets
    // like qsound.zip into. The ROM's own containing folder and the inside
    // of the loaded ROM's own zip are both already covered automatically by
    // MAME itself (g_rom_dir + native zip reading), no extra wiring needed.
    const std::string mame_root = root_dir + "/mame";
    mkdir(mame_root.c_str(), 0755);
    mkdir((mame_root + "/bios").c_str(), 0755);
    mkdir((mame_root + "/roms").c_str(), 0755);
    qrd::set_mame_system_directory(root_dir);

    // PC Engine CD: beetle-pce reads its BIOS/system card image (syscard3.pce
    // and regional variants) straight from whatever directory it's told is
    // the libretro "system directory" -- no search fallback, just string
    // concatenation (MDFN_MakeFName() in third_party/beetle-pce/libretro.cpp).
    // Users drop the matching syscard*.pce file into <root_dir>/pce/bios/.
    const std::string pce_root = root_dir + "/pce";
    mkdir(pce_root.c_str(), 0755);
    const std::string pce_bios_dir = pce_root + "/bios";
    mkdir(pce_bios_dir.c_str(), 0755);
    qrd::set_pce_system_directory(pce_bios_dir);

    // Saturn: Mednafen reads its BIOS (sega_101.bin / mpr-17933.bin) straight
    // from whatever directory it's told is the libretro "system directory" --
    // no search fallback, just string concatenation (ss.cpp's region-detect
    // block). Users drop both files into <root_dir>/saturn/bios/.
    const std::string saturn_root = root_dir + "/saturn";
    mkdir(saturn_root.c_str(), 0755);
    const std::string saturn_bios_dir = saturn_root + "/bios";
    mkdir(saturn_bios_dir.c_str(), 0755);
    qrd::set_saturn_frontend_directory(saturn_bios_dir);

    // PlayStation: SwanStation looks for scph5500/5501/5502.bin (or a
    // compatible OpenBIOS image) in the libretro system directory and writes
    // memory cards alongside the save directory.
    const std::string psx_root = root_dir + "/psx";
    mkdir(psx_root.c_str(), 0755);
    const std::string psx_bios_dir = psx_root + "/bios";
    mkdir(psx_bios_dir.c_str(), 0755);
    qrd::set_psx_system_directory(psx_bios_dir);
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeSetBtInputState(
    JNIEnv*, jobject,
    jboolean up, jboolean down, jboolean left, jboolean right,
    jboolean a, jboolean b, jboolean x, jboolean y,
    jboolean l, jboolean r, jboolean start, jboolean select)
{
    std::lock_guard<std::mutex> il(g_input_write_mutex);
    g_bt_input.dpad_up    = up     == JNI_TRUE;
    g_bt_input.dpad_down  = down   == JNI_TRUE;
    g_bt_input.dpad_left  = left   == JNI_TRUE;
    g_bt_input.dpad_right = right  == JNI_TRUE;
    g_bt_input.button_a   = a      == JNI_TRUE;
    g_bt_input.button_b   = b      == JNI_TRUE;
    g_bt_input.button_x   = x      == JNI_TRUE;
    g_bt_input.button_y   = y      == JNI_TRUE;
    g_bt_input.button_l   = l      == JNI_TRUE;
    g_bt_input.button_r   = r      == JNI_TRUE;
    g_bt_input.button_start  = start  == JNI_TRUE;
    g_bt_input.button_select = select == JNI_TRUE;
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
    configure_mgba_frontend_dirs_from_activity(env, activity);
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
    qrd::initialize_mame_driver_database(g_asset_manager);
    qrd::initialize_rom_title_database(g_asset_manager);
    g_experimental_rumble.set_user_root(g_rumble_root);
    std::string rumble_error;
    if (!g_experimental_rumble.load_catalog(g_asset_manager, rumble_error) && !rumble_error.empty()) {
        LOGI("experimental rumble catalog load failed: %s", rumble_error.c_str());
    }
    g_openxr_shell.set_experimental_rumble_status(g_experimental_rumble.active_status());

    g_openxr_shell.set_frame_provider(frame_provider_for_vr);

    // Wire up emulator freeze controls so the XR thread can pause/resume the emu thread.
    g_openxr_shell.set_emu_freeze_ctrl([](bool freeze) {
        if (freeze) {
            { std::lock_guard<std::mutex> lock(g_backend_mutex);
              if (g_backend) g_backend->on_emu_freeze(); }
            g_emu_frozen.store(true, std::memory_order_release);
        } else {
            g_emu_frozen.store(false, std::memory_order_release);
            g_emu_step_one.store(false, std::memory_order_release);
            { std::lock_guard<std::mutex> lock(g_backend_mutex);
              if (g_backend) g_backend->on_emu_unfreeze(); }
        }
    });
    g_openxr_shell.set_emu_step_one([]() {
        g_emu_step_one.store(true, std::memory_order_release);
    });
    g_openxr_shell.set_gun_mode_ctrl([](bool enabled) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        if (!g_backend) return;
        g_backend->set_gun_mode(enabled);
        // Many gun-peripheral titles only probe the device type at boot/reset,
        // so a runtime toggle needs a soft reset to actually take effect.
        g_backend->soft_reset();
    });

    g_openxr_shell.set_dual_gun_mode_ctrl([](bool enabled) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        if (!g_backend) return;
        // Every backend gets the call; the ones with no second gun to offer
        // ignore it (see backend_supports_dual_gun(), which is what gates the
        // option in the UI in the first place).
        g_backend->set_dual_gun_mode(enabled);
        // Same reason as set_gun_mode: the device in a port is probed at
        // boot, so a game already running will not notice a new gun until it
        // resets.
        g_backend->soft_reset();
    });

    // Wire up vr_state callbacks so settings changes propagate to emulator thread
    g_openxr_shell.set_on_vr_state_changed([](int audio_spatial_mode) {
        g_audio_processor.mode.store(audio_spatial_mode, std::memory_order_relaxed);
    });
    // Per-core auto-frame-skip: updates every core's slot in g_auto_frame_skip
    // (picked up live by the emu thread's per-frame set_auto_frame_skip() call
    // for cores that poll variable changes, e.g. SNES/Genesis's
    // m_variables_dirty). MAME/Saturn/PC Engine/mGBA only read this option
    // while building the machine, so a live change to whichever one is
    // actually running needs a reload to take effect.
    g_openxr_shell.set_auto_frame_skip_applier([](const VrState& vs) {
        struct Entry { qrd::BackendKind kind; bool value; bool load_time_only; };
        const Entry entries[] = {
            {qrd::BackendKind::Snes,    vs.auto_frame_skip_snes,    false},
            {qrd::BackendKind::Genesis, vs.auto_frame_skip_genesis, false},
            {qrd::BackendKind::Sms,     vs.auto_frame_skip_genesis, false},
            {qrd::BackendKind::Mame,    vs.auto_frame_skip_mame,    true},
            {qrd::BackendKind::Saturn,  vs.auto_frame_skip_saturn,  true},
            {qrd::BackendKind::Psx,     false,                      true},
            {qrd::BackendKind::Pce,     vs.auto_frame_skip_pce,     true},
            {qrd::BackendKind::Gba,     vs.auto_frame_skip_gba,     true},
            {qrd::BackendKind::Gb,      vs.auto_frame_skip_gba,     true},
        };
        bool need_reload = false;
        for (const Entry& e : entries) {
            const bool was = g_auto_frame_skip[static_cast<int>(e.kind)].exchange(
                e.value, std::memory_order_acq_rel);
            if (was != e.value && e.load_time_only &&
                g_last_working_backend_kind.has_value() && *g_last_working_backend_kind == e.kind) {
                need_reload = true;
            }
        }
        if (need_reload && !g_last_working_rom_path.empty()) {
            set_last_status("Auto frame skip changed; reloading ROM...");
            g_openxr_shell.request_current_rom_reload();
        }
    });
    g_openxr_shell.set_psx_render_path_ctrl([](int path) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        if (!g_backend || !g_backend_kind.has_value() || *g_backend_kind != qrd::BackendKind::Psx) return;
        g_backend->set_psx_render_path(path);
    });
    g_openxr_shell.set_psx_gpu_resolution_applier([](int scale) {
        if (scale != 1 && scale != 2 && scale != 4) scale = 4;
        g_psx_gpu_resolution.store(scale, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        if (!g_backend || !g_backend_kind.has_value() || *g_backend_kind != qrd::BackendKind::Psx) return;
        if (auto* psx = dynamic_cast<qrd::PsxLibretroBackend*>(g_backend.get()))
            psx->set_gpu_resolution(scale);
    });
    g_openxr_shell.set_psx_texture_filter_applier([](int index) {
        g_psx_texture_filter.store(index, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        if (!g_backend || !g_backend_kind.has_value() || *g_backend_kind != qrd::BackendKind::Psx) return;
        if (auto* psx = dynamic_cast<qrd::PsxLibretroBackend*>(g_backend.get()))
            psx->set_texture_filter(index);
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
    g_openxr_shell.set_mame_occupancy_ctrl([](bool enabled) {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        auto* backend = ensure_backend(g_backend_kind.value_or(qrd::BackendKind::Snes));
        if (backend) backend->set_occupancy_capture_enabled(enabled);
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

    // Give the shell the archive preparer separately from the backend loader.
    // Foreground shelf selections run this part off the XR thread so a large
    // Saturn/ZIP extraction cannot stop headset rendering.
    g_openxr_shell.set_rom_preparer([](const std::string& raw_path) {
        return prepare_rom_path(raw_path);
    });
    g_openxr_shell.set_rom_prepared_path_publisher(
        [](const std::string& raw_path, const std::string& prepared_path) {
            publish_prepared_rom(raw_path, prepared_path);
        });
    g_openxr_shell.set_extracted_rom_cache_clearer([]() {
        clear_extracted_rom_cache();
    });
    g_openxr_shell.set_extracted_rom_cache_stats([]() {
        return extracted_rom_cache_stats();
    });
    g_openxr_shell.set_app_exiter([]() {
        if (!g_vm || !g_activity_global) return;
        JNIEnv* env  = nullptr;
        bool attached = false;
        if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            g_vm->AttachCurrentThread(&env, nullptr);
            attached = true;
        }
        jclass    cls = env->GetObjectClass(g_activity_global);
        jmethodID mid = env->GetMethodID(cls, "exitApp", "()V");
        if (mid) {
            env->CallVoidMethod(g_activity_global, mid);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(cls);
        if (attached) g_vm->DetachCurrentThread();
    });
    g_openxr_shell.set_audio_channel_volume_applier([](const VrState& vs) {
        const bool on = vs.audio_channel_split_enabled;
        for (int i = 0; i < 8; ++i) snes_set_channel_volume(i, on ? vs.snes_voice_volume[i] : 1.0f);
        for (int i = 0; i < 2; ++i) genesis_set_channel_volume(i, on ? vs.genesis_channel_volume[i] : 1.0f);
        for (int i = 0; i < 5; ++i) nes_set_channel_volume(i, on ? vs.nes_channel_volume[i] : 1.0f);
        for (int i = 0; i < 3; ++i) GBAudioSetChannelVolume(i, on ? vs.gba_channel_volume[i] : 1.0f);
        for (int i = 0; i < 6; ++i) pce_psg_set_channel_volume(i, on ? vs.pce_channel_volume[i] : 1.0f);
    });
    g_openxr_shell.set_rom_closer([]() {
        std::lock_guard<std::mutex> transition_lock(g_rom_transition_mutex);
        LOGI("ROM closer: unloading active game for good");
        stop_emu_thread();
        clear_published_emulation_state();
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        g_backend.reset();
        g_backend_kind.reset();
    });

    // Give the shell a ROM loader so it can load ROMs from the browser.
    // Extracts archives via Kotlin, restores the previous ROM if load fails
    // (snes9x clears its state on failed load, which would cause "no ROM loaded").
    g_openxr_shell.set_rom_loader([](const std::string& raw_path, std::string& err) -> bool {
        std::lock_guard<std::mutex> transition_lock(g_rom_transition_mutex);
        LOGI("ROM loader: begin raw=%s", raw_path.c_str());
        begin_fresh_rom_load();
        g_openxr_shell.set_rom_load_stage(raw_path, "LOADING CORE");
        const std::string path = prepare_rom_path(raw_path);
        const auto wanted_kind = resolve_backend_kind(raw_path, path);
        LOGI("ROM loader: prepared=%s backend=%s",
             path.c_str(), qrd::backend_kind_name(wanted_kind));
        // SwanStation latches its renderer choice at boot, so a PSX ROM that
        // loads before the XR renderer has published its GL context is stuck on
        // the software renderer — with no depth — for the life of that load.
        // The startup auto-load (debug_rom.bat's "rom" extra, and the "load
        // last ROM" preference) races exactly that. Wait for the context here:
        // this runs on the JNI thread, while the render thread that captures it
        // is free to keep going.
        if (wanted_kind == qrd::BackendKind::Psx && !qrd::psx_gl_context_host_available()) {
            using namespace std::chrono;
            const auto deadline = steady_clock::now() + seconds(5);
            while (!qrd::psx_gl_context_host_available() && steady_clock::now() < deadline)
                std::this_thread::sleep_for(milliseconds(20));
            LOGI("ROM loader: waited for PSX host GL context, available=%d",
                 (int)qrd::psx_gl_context_host_available());
        }
        const std::string previous_rom_path = g_last_working_rom_path;
        const std::optional<qrd::BackendKind> previous_kind = g_last_working_backend_kind;
        std::string backend_name;
        std::string game_name;
        std::string load_warning;
        bool restored_previous_rom = false;
        bool load_failed = false;
        {
            std::lock_guard<std::mutex> lock(g_backend_mutex);
            LOGI("ROM loader: recreate backend start backend=%s",
                 qrd::backend_kind_name(wanted_kind));
            auto* backend = recreate_backend_locked(wanted_kind);
            LOGI("ROM loader: recreate backend done backend=%s ptr=%p",
                 qrd::backend_kind_name(wanted_kind), backend);
            if (!backend) { err = "Backend unavailable"; return false; }

            // Must happen before load_content(): many gun-peripheral titles latch
            // the port-1 device type at boot/reset and never re-poll it, so setting
            // gun mode after the ROM is already loaded is a no-op for them.
            backend->set_gun_mode(qrd::rom_is_lightgun_capable(wanted_kind, path),
                                  qrd::rom_gun_peripheral(wanted_kind, path));
            // Dual wielding never carries across a ROM load: backends are
            // reused, and the shell's own toggle resets with the new game.
            backend->set_dual_gun_mode(false);
            backend->set_auto_frame_skip(auto_frame_skip_for(wanted_kind));
            if (wanted_kind == qrd::BackendKind::Psx) {
                if (auto* psx = dynamic_cast<qrd::PsxLibretroBackend*>(backend))
                {
                    psx->set_gpu_resolution(g_psx_gpu_resolution.load(std::memory_order_acquire));
                    psx->set_texture_filter(g_psx_texture_filter.load(std::memory_order_acquire));
                    psx->set_psx_render_path(g_openxr_shell.psx_render_path());
                }
            }

            LOGI("ROM loader: load_content start backend=%s path=%s",
                 qrd::backend_kind_name(wanted_kind), path.c_str());
            if (!backend->load_content(path, err)) {
                LOGI("ROM loader: load_content failed backend=%s err=%s",
                     qrd::backend_kind_name(wanted_kind), err.c_str());
                const std::string load_error = err;
                // Restore last working ROM so the display doesn't go blank.
                // This must also handle cross-backend failures, since the failed
                // load recreated the backend and stopped the emu thread.
                if (!previous_rom_path.empty() && previous_kind.has_value()) {
                    std::string restore_err;
                    backend = recreate_backend_locked(*previous_kind);
                    if (backend) {
                        backend->set_gun_mode(qrd::rom_is_lightgun_capable(*previous_kind, previous_rom_path),
                                              qrd::rom_gun_peripheral(*previous_kind, previous_rom_path));
                        backend->set_auto_frame_skip(auto_frame_skip_for(*previous_kind));
                        if (*previous_kind == qrd::BackendKind::Psx) {
                            if (auto* psx = dynamic_cast<qrd::PsxLibretroBackend*>(backend))
                            {
                                psx->set_gpu_resolution(g_psx_gpu_resolution.load(std::memory_order_acquire));
                                psx->set_psx_render_path(g_openxr_shell.psx_render_path());
                            }
                        }
                    }
                    if (backend && backend->load_content(previous_rom_path, restore_err)) {
                        qrd::EmulatorInputState ei;
                        backend->step_frame(ei, restore_err);
                        restored_previous_rom = true;
                    }
                }
                err = load_error;
                load_failed = true;
            }

            if (!load_failed) {
                LOGI("ROM loader: load_content OK backend=%s",
                     qrd::backend_kind_name(wanted_kind));
                qrd::EmulatorInputState empty_input;
                LOGI("ROM loader: prime frame start backend=%s",
                     qrd::backend_kind_name(wanted_kind));
                backend->step_frame(empty_input, err); // prime — ignore failure
                LOGI("ROM loader: prime frame done backend=%s err=%s",
                     qrd::backend_kind_name(wanted_kind), err.c_str());
                const auto& frame = backend->frame_output();
                if (frame.width > 0 && !frame.rgba8888.empty()) {
                }
                g_last_working_rom_path    = path;
                g_last_working_backend_kind = wanted_kind;
                g_last_loaded_rom_filename = basename_from_path(path);
                // (gun mode already set above, before load_content)
                g_last_loaded_rom_prefs_name = basename_from_path(raw_path);
                auto info = backend->get_rom_header_info();
                if (info.has_header && !info.game_name.empty()) {
                    game_name = info.game_name;
                }
                g_last_loaded_game_name = game_name;
                g_experimental_rumble.on_rom_loaded(g_asset_manager, wanted_kind, g_last_loaded_rom_filename, game_name);
                g_openxr_shell.set_experimental_rumble_status(g_experimental_rumble.active_status());
                backend_name = backend->backend_name();
                load_warning = backend->last_load_warning();
            }
        }

        if (load_failed) {
            // Always surface the real error, whether or not there was a
            // previous ROM to fall back to -- silently reverting with no
            // message left the user staring at whatever was on screen with
            // zero indication their new ROM didn't load.
            g_openxr_shell.set_rom_hint_override(
                !err.empty() ? err : std::string("Failed to load this ROM."));
            g_openxr_shell.show_rom_hint();
            if (restored_previous_rom) start_emu_thread();
            return false;
        }

        g_openxr_shell.set_current_backend_kind(wanted_kind, raw_path);
        if (!load_warning.empty()) {
            g_openxr_shell.set_rom_hint_override(load_warning);
        } else if (g_openxr_shell.current_config_is_single_layer()) {
            g_openxr_shell.set_rom_hint_override(
                "This ROM has no per-game depth layers yet.\nShowing flat 2D.");
        } else {
            g_openxr_shell.set_rom_hint_override("");
        }
        g_openxr_shell.set_current_rom(g_last_loaded_rom_filename);
        if (!game_name.empty()) {
            g_openxr_shell.set_current_game_name(game_name);
        }
        LOGI("ROM loader: raw=%s prepared=%s backend=%s",
             raw_path.c_str(), path.c_str(), backend_name.c_str());
        g_openxr_shell.set_rom_load_stage(raw_path, "STARTING GAME");
        start_emu_thread();
        return true;
    });

    g_openxr_shell.set_rom_preview_session(
        [](std::string& error) -> bool {
            std::lock_guard<std::mutex> transition_lock(g_rom_transition_mutex);
            LOGI("ROM preview session: begin, stopping emulation and unloading current backend");
            stop_emu_thread();
            // Stop displaying the last gameplay frame as soon as the browser
            // opens. The backend is gone below, but the XR renderer reads the
            // published double-buffer until it is explicitly invalidated.
            clear_published_emulation_state();
            std::lock_guard<std::mutex> lock(g_backend_mutex);
            g_preview_saved_rom_path = g_last_working_rom_path;
            g_preview_saved_backend_kind = g_last_working_backend_kind;
            g_preview_saved_state.clear();
            g_preview_session_saved = false;
            g_preview_had_active = g_backend && !g_preview_saved_rom_path.empty();
            if (!g_preview_had_active) return true;
            // Some cores (observed with MAME) throw a real C++ exception from
            // save_state() instead of returning false on an unsupported/
            // misconfigured state save, instead of just returning false —
            // this used to only matter when deliberately opening the old ROM
            // browser; the new Library tab reaches this same path far more
            // casually (any hover), so it started crashing the whole app.
            // Treat a thrown exception exactly like a false return: unload
            // without restore rather than letting it escape and abort().
            bool saved_ok = false;
            try {
                saved_ok = g_backend->save_state(g_preview_saved_state, error);
            } catch (const std::exception& ex) {
                error = ex.what();
            } catch (...) {
                error = "native core threw an unrecognized exception";
            }
            if (!saved_ok) {
                LOGE("ROM preview session: save_state failed (%s); unloading without restore", error.c_str());
                g_backend.reset();
                g_backend_kind.reset();
                g_preview_saved_rom_path.clear();
                g_preview_saved_backend_kind.reset();
                g_preview_had_active = false;
                return false;
            }
            // Do not leave the previous game loaded while the shelf is open.
            // Preview jobs normally replace this backend themselves, but a
            // cache hit (or a folder containing only subdirectories) may not
            // start a job at all. Resetting here makes the lifecycle
            // deterministic for Neo Geo and every other backend; the saved
            // ROM/state above is restored when the session ends without a
            // committed ROM launch.
            g_backend.reset();
            g_backend_kind.reset();
            g_preview_session_saved = true;
            LOGI("ROM preview session: current backend unloaded; saved=%d", g_preview_session_saved ? 1 : 0);
            return true;
        },
        [](bool committed) {
            std::lock_guard<std::mutex> transition_lock(g_rom_transition_mutex);
            LOGI("ROM preview session: end committed=%d", committed ? 1 : 0);
            if (committed) {
                g_preview_saved_rom_path.clear();
                g_preview_saved_backend_kind.reset();
                g_preview_saved_state.clear();
                g_preview_session_saved = false;
                g_preview_had_active = false;
                return;
            }
            if (g_preview_session_saved && g_preview_saved_backend_kind.has_value() &&
                !g_preview_saved_rom_path.empty()) {
                std::lock_guard<std::mutex> lock(g_backend_mutex);
                auto* backend = recreate_backend_locked(*g_preview_saved_backend_kind);
                std::string error;
                if (backend) backend->set_auto_frame_skip(auto_frame_skip_for(*g_preview_saved_backend_kind));
                if (backend && backend->load_content(g_preview_saved_rom_path, error)) {
                    if (!g_preview_saved_state.empty()) backend->load_state(
                        g_preview_saved_state.data(), g_preview_saved_state.size(), error);
                    g_last_working_rom_path = g_preview_saved_rom_path;
                    g_last_working_backend_kind = g_preview_saved_backend_kind;
                }
            }
            g_preview_saved_rom_path.clear();
            g_preview_saved_backend_kind.reset();
            g_preview_saved_state.clear();
            g_preview_session_saved = false;
            if (g_preview_had_active) start_emu_thread();
            g_preview_had_active = false;
        });

    // siglongjmp-based thumbnail crash recovery bypasses normal C++
    // destructors. Keep the preview transition locks in a small guard that
    // also registers them with RomPreviewManager, so a faulting core cannot
    // leave either shared mutex locked forever and make the next ROM load
    // appear frozen.
    class RomPreviewCaptureMutexGuard {
    public:
        explicit RomPreviewCaptureMutexGuard(std::mutex& mutex) : m_mutex(&mutex) {
            m_mutex->lock();
            qrd::rom_preview_register_capture_mutex(m_mutex);
        }
        ~RomPreviewCaptureMutexGuard() {
            qrd::rom_preview_unregister_capture_mutex(m_mutex);
            m_mutex->unlock();
        }
        RomPreviewCaptureMutexGuard(const RomPreviewCaptureMutexGuard&) = delete;
        RomPreviewCaptureMutexGuard& operator=(const RomPreviewCaptureMutexGuard&) = delete;
    private:
        std::mutex* m_mutex;
    };

    g_openxr_shell.set_rom_preview_capture(
        [](const std::string& path, qrd::RomPreviewSnapshot& out, std::string& error,
           const std::atomic<bool>& cancel, bool is_live,
           const qrd::RomPreviewPublish& publish,
           const qrd::RomPreviewProgress& progress) -> bool {
            // Decide whether this MAME preview is supported before asking
            // Kotlin to extract a potentially enormous Saturn archive. The
            // previous order made the first Saturn .7z occupy the worker for
            // minutes even though the callback would reject Saturn later.
            const auto raw_kind = detect_backend_kind_from_path(path);
            const bool is_neogeo_preview = is_neogeo_mame_preview_path(path);
            const bool is_saturn_preview = is_saturn_mame_preview_path(path);
            if (raw_kind == qrd::BackendKind::Mame &&
                (!is_neogeo_preview && !is_saturn_preview || is_live)) {
                error = is_live ? "MAME: live preview unsupported"
                                : "MAME: preview unsupported for this system";
                return false;
            }
            // Archives (.zip/.7z) must be extracted to a raw ROM first, same as
            // the normal load path — feeding archive bytes straight into a core
            // corrupts its memory-map setup and crashes (observed as a SIGSEGV
            // inside S9xGetByte when previewing a zipped SNES ROM).
            if (!is_live) progress(1);
            // Live hover-preview captures aren't tracked by the coarse
            // background-job progress() lambda (see the comment above the
            // capture signature), but Kotlin's archive extractor fires the
            // same real per-file nativeRomPreparationProgress events for
            // every prepare_rom_path() call regardless of caller. Marking
            // this path as the preview-extract target lets those real events
            // reach the Library preview sidebar instead of being dropped.
            if (is_live) g_openxr_shell.set_preview_extract_target(path);
            const std::string prepared_path = prepare_rom_path(path);
            if (is_live) g_openxr_shell.set_preview_extract_done(path, prepared_path);
            const auto kind = resolve_backend_kind(path, prepared_path);
            if (!is_live) progress(10);
            // MAME's libretro core re-runs its process boot sequence inside
            // every retro_load_game() call. The callback also shares the
            // process-wide backend mutex with the real game loader, so a
            // slow/hung MAME set could make launching a game wait for the
            // thumbnail job to finish. Keep live MAME hover capture disabled
            // because it would hold this backend for an unbounded duration;
            // background Neo Geo and Saturn thumbnails are supported.
            if (kind == qrd::BackendKind::Mame &&
                (!is_neogeo_preview && !is_saturn_preview || is_live)) {
                error = is_live ? "MAME: live preview unsupported"
                                : "MAME: preview unsupported for this system";
                return false;
            }
            // The preview owns the process-wide backend for the duration of
            // the capture. Keep it out of the way of JNI/browser ROM loads as
            // well as the emulation thread; the backend mutex alone does not
            // cover the gaps between a capture's backend operations.
            RomPreviewCaptureMutexGuard transition_lock(g_rom_transition_mutex);
            RomPreviewCaptureMutexGuard lock(g_backend_mutex);
            auto* backend = recreate_backend_locked(kind);
            if (!backend) return false;
            // Background caching never needs sound (it's stepping through
            // many ROMs unattended), but the live hover preview is a single
            // foreground ROM actually being watched — let it play audio too.
            backend->set_preview_mode(true, /*allow_audio=*/is_live);
            backend->set_auto_frame_skip(auto_frame_skip_for(kind));
            if (!backend->load_content(prepared_path, error)) return false;
            if (!is_live) progress(15);
            qrd::EmulatorInputState input;

            if (is_live) {
                // load_content() already warms each core up internally (with
                // an early-out once a visible frame appears), so most games
                // are already past their very first frame — but that's
                // usually still the publisher/BIOS logo, not gameplay. A
                // short extra warmup keeps a freshly-hovered card from
                // opening on a logo before it starts animating.
                constexpr int kLiveWarmupFrames = 20;
                for (int i = 0; i < kLiveWarmupFrames && !cancel.load(); ++i) {
                    if (!backend->step_frame(input, error)) return false;
                }
                if (cancel.load()) return false;

                // Keeps stepping the core and pushing every rendered frame to
                // the card in real time, instead of freezing on one captured
                // frame — this is what makes the hovered thumbnail actually
                // play the game rather than show a still. It runs until the
                // hover moves elsewhere (cancel becomes true), at which point
                // the worker treats this job as cancelled and moves on;
                // `out`/return value are unused since frames are pushed via
                // `publish`.
                using clock = std::chrono::steady_clock;
                auto next_tick = clock::now();
                // The real core rate, not a rounded 16ms. 16ms is 62.5Hz --
                // ~4% fast, so the core produced audio faster than the output
                // stream drained it and the surplus was dropped, which is a
                // large part of why preview audio sounded worse than the same
                // ROM does once launched.
                constexpr auto kFrameInterval = std::chrono::microseconds(16639); // 1/60.0988s
                // The emulator must step every frame to keep audio continuous,
                // but the card does not need 60 distinct images a second --
                // and building one costs a full per-pixel pass over every
                // layer plus two whole-snapshot copies, all on this thread. At
                // 60Hz that work regularly overran the frame budget and
                // starved the audio callback. Publishing every other frame
                // halves it and leaves audio untouched at full rate.
                constexpr int kPublishEveryNthFrame = 2;
                int frame_index = 0;
                while (!cancel.load()) {
                    if (!backend->step_frame(input, error)) return false;
                    if ((frame_index++ % kPublishEveryNthFrame) == 0) {
                        qrd::RomPreviewSnapshot snapshot;
                        if (snapshot_from_frame(backend->frame_output(), snapshot, kind)) {
                            publish(snapshot);
                        }
                    }
                    next_tick += kFrameInterval;
                    const auto now = clock::now();
                    if (now < next_tick) std::this_thread::sleep_for(next_tick - now);
                    else next_tick = now;
                }
                return true;
            }

            // Background cache job: capture at fixed simulated timestamps
            // (10s/20s/30s/40s/50s of gameplay) instead of scoring candidate
            // frames for "busiest looking" or "most motion" — cheaper (no
            // per-frame scoring math), and spacing the captures seconds apart
            // guarantees they actually look different from each other, which
            // a short scoring window sometimes failed to do (frames a few
            // frames apart looking nearly identical). This is background
            // work that only ever needs to happen once per ROM (results are
            // cached to disk), so taking longer per ROM to get 5 good
            // snapshots instead of 3 is an acceptable one-time cost.
            constexpr int kCoreFps = 60; // close enough to the real ~60.0988Hz for thumbnail timing
            constexpr int kCheckpointCount = 5;
            // Use the same long gameplay schedule for Neo Geo as the other
            // systems, so the shelf cycles through genuinely different parts
            // of the game instead of repeatedly showing only its opening
            // seconds.
            const int kCheckpointFrames[kCheckpointCount] = {
                10 * kCoreFps,
                20 * kCoreFps,
                30 * kCoreFps,
                40 * kCoreFps,
                50 * kCoreFps};
            qrd::RomPreviewSnapshot checkpoint_snapshots[kCheckpointCount];
            bool checkpoint_ok[kCheckpointCount] = {false, false, false, false, false};
            int frames_stepped = 0;
            constexpr int kTotalCaptureFrames = 50 * kCoreFps + 2 * kCheckpointCount;
            int last_progress = 15;
            auto report_capture_progress = [&](int frames) {
                const int percent = 15 + (frames * 85) / kTotalCaptureFrames;
                if (percent != last_progress) {
                    last_progress = percent;
                    progress(percent);
                }
            };
            // Per-layer separation (what makes the VR shelf render actual
            // depth-separated planes instead of a flat picture) is real work
            // the core redoes on *every* frame it renders, not a cheap
            // post-process step — for SNES it happens inside snes9x itself.
            // None of the frames between checkpoints are ever looked at, so
            // disable layer capture while stepping through them and only
            // re-enable it for the single frame at each checkpoint. This is
            // the main reason background caching got much slower once
            // checkpoints moved out to tens of seconds of simulated
            // gameplay.
            constexpr uint32_t kFullLayerMask = 0xFFFFFFFFu; // each backend masks this down to its own bit width
            backend->set_layer_capture_mask(0);
            for (int c = 0; c < kCheckpointCount && !cancel.load(); ++c) {
                bool step_failed = false;
                while (frames_stepped < kCheckpointFrames[c] && !cancel.load()) {
                    if (!backend->step_frame(input, error)) { step_failed = true; break; }
                    ++frames_stepped;
                    report_capture_progress(frames_stepped);
                }
                if (cancel.load()) break;
                if (step_failed) break; // game ended/crashed early; keep whatever we already captured
                backend->set_layer_capture_mask(kFullLayerMask);
                // Toggling the mask right before reading frame_output() can
                // land on a transition frame where the core's internal
                // palette/render state hasn't fully caught up to capturing
                // again (observed as inverted/corrupted colors) — step twice
                // and use the second frame so the renderer has settled.
                if (!backend->step_frame(input, error)) { step_failed = true; }
                else {
                    ++frames_stepped;
                    report_capture_progress(frames_stepped);
                    if (!backend->step_frame(input, error)) { step_failed = true; }
                    else {
                        ++frames_stepped;
                        report_capture_progress(frames_stepped);
                        checkpoint_ok[c] = snapshot_from_frame(backend->frame_output(), checkpoint_snapshots[c], kind);
                    }
                }
                backend->set_layer_capture_mask(0);
                if (step_failed) break;
            }
            bool assigned = false;
            out.extra_frames.clear();
            for (int c = 0; c < kCheckpointCount; ++c) {
                if (!checkpoint_ok[c]) continue;
                if (!assigned) { out = std::move(checkpoint_snapshots[c]); assigned = true; }
                else out.extra_frames.push_back(std::move(checkpoint_snapshots[c].layers));
            }
            progress(100);
            return assigned;
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

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeSubmitRomSearch(
    JNIEnv* env, jobject, jstring text)
{
    const char* raw = text ? env->GetStringUTFChars(text, nullptr) : nullptr;
    const std::string value = raw ? raw : "";
    if (text && raw) env->ReleaseStringUTFChars(text, raw);
    g_openxr_shell.submit_rom_search_text(value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeCancelRomSearch(
    JNIEnv*, jobject)
{
    g_openxr_shell.cancel_rom_search_text();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeGetVrStateSummary(
    JNIEnv* env, jobject)
{
    return make_jstring(env, g_openxr_shell.vr_state_summary());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeLoadRom(
    JNIEnv* env, jobject activity, jstring path, jstring source_name)
{
    std::lock_guard<std::mutex> transition_lock(g_rom_transition_mutex);
    LOGI("nativeLoadRom: begin");
    begin_fresh_rom_load();
    const char* raw = env->GetStringUTFChars(path, nullptr);
    std::string rom_path = raw ? raw : "";
    env->ReleaseStringUTFChars(path, raw);
    const char* raw_source = source_name ? env->GetStringUTFChars(source_name, nullptr) : nullptr;
    const std::string source_hint = raw_source ? raw_source : "";
    std::string prefs_name = source_hint;
    if (prefs_name.find('/') != std::string::npos || prefs_name.find('\\') != std::string::npos)
        prefs_name = basename_from_path(prefs_name);
    if (source_name && raw_source) env->ReleaseStringUTFChars(source_name, raw_source);

    auto wanted_kind = detect_backend_kind_from_path(rom_path);
    // Startup loading passes an extracted cache path, which intentionally no
    // longer contains /roms/nes/ (or another official system root). Use the
    // original source path as a hint in that case, preserving the folder-based
    // backend policy for normal browser loads.
    if (wanted_kind == qrd::BackendKind::Mame && !source_hint.empty()) {
        const auto hinted_kind = detect_backend_kind_from_path(source_hint);
        if (hinted_kind != qrd::BackendKind::Mame) wanted_kind = hinted_kind;
    }
    if (wanted_kind == qrd::BackendKind::Gba || wanted_kind == qrd::BackendKind::Gb)
        configure_mgba_frontend_dirs_from_activity(env, activity);
    LOGI("nativeLoadRom: path=%s source=%s backend=%s",
         rom_path.c_str(), prefs_name.c_str(), qrd::backend_kind_name(wanted_kind));
    // This is the startup path (debug_rom.bat's "rom" extra and the "load last
    // ROM" preference), which races the renderer publishing its GL context.
    // SwanStation latches the renderer at boot, so losing that race leaves PSX
    // on the software renderer, with no depth, until the ROM is loaded again.
    if (wanted_kind == qrd::BackendKind::Psx && !qrd::psx_gl_context_host_available()) {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + seconds(5);
        while (!qrd::psx_gl_context_host_available() && steady_clock::now() < deadline)
            std::this_thread::sleep_for(milliseconds(20));
        LOGI("nativeLoadRom: waited for PSX host GL context, available=%d",
             (int)qrd::psx_gl_context_host_available());
    }
    std::string game_name;
    std::string load_warning;
    {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        LOGI("nativeLoadRom: recreate backend start backend=%s",
             qrd::backend_kind_name(wanted_kind));
        auto* backend = recreate_backend_locked(wanted_kind);
        LOGI("nativeLoadRom: recreate backend done backend=%s ptr=%p",
             qrd::backend_kind_name(wanted_kind), backend);
        if (!backend) return make_jstring(env, "Backend creation failed.");

        // Must happen before load_content(): many gun-peripheral titles latch
        // the port-1 device type at boot/reset and never re-poll it.
        // `adb shell setprop debug.qrd.nogun 1` boots gun titles with a normal
        // pad instead of a light gun. Time Crisis opens on GunCon calibration
        // and draws no 3D until the trigger is pulled, which makes unattended
        // depth testing impossible; this skips straight into the attract demo.
        // Debug only — unset the property for normal play.
        bool gun_capable = qrd::rom_is_lightgun_capable(wanted_kind, rom_path);
        {
            char nogun[PROP_VALUE_MAX] = {0};
            if (__system_property_get("debug.qrd.nogun", nogun) > 0 && nogun[0] == '1') {
                LOGI("nativeLoadRom: debug.qrd.nogun set, forcing pad instead of light gun");
                gun_capable = false;
            }
        }
        backend->set_gun_mode(gun_capable, qrd::rom_gun_peripheral(wanted_kind, rom_path));
        backend->set_dual_gun_mode(false);
        backend->set_auto_frame_skip(auto_frame_skip_for(wanted_kind));
        if (wanted_kind == qrd::BackendKind::Psx) {
            if (auto* psx = dynamic_cast<qrd::PsxLibretroBackend*>(backend))
            {
                psx->set_texture_filter(g_psx_texture_filter.load(std::memory_order_acquire));
                int gpu_res = g_psx_gpu_resolution.load(std::memory_order_acquire);
                // `adb shell setprop debug.qrd.psxres 4` overrides the internal
                // GPU scale for unattended testing, since the setting is
                // otherwise only reachable through the in-headset UI.
                char res_prop[PROP_VALUE_MAX] = {0};
                if (__system_property_get("debug.qrd.psxres", res_prop) > 0) {
                    const int requested = atoi(res_prop);
                    if (requested == 1 || requested == 2 || requested == 4) {
                        LOGI("nativeLoadRom: debug.qrd.psxres=%d overriding GPU scale", requested);
                        gpu_res = requested;
                    }
                }
                psx->set_gpu_resolution(gpu_res);
                psx->set_psx_render_path(g_openxr_shell.psx_render_path());
            }
        }

        std::string error;
        LOGI("nativeLoadRom: load_content start backend=%s path=%s",
             qrd::backend_kind_name(wanted_kind), rom_path.c_str());
        if (!backend->load_content(rom_path, error)) {
            LOGI("nativeLoadRom: load_content failed backend=%s err=%s",
                 qrd::backend_kind_name(wanted_kind), error.c_str());
            return make_jstring(env, "ROM load failed\n\n" + error);
        }
        LOGI("nativeLoadRom: load_content OK backend=%s",
             qrd::backend_kind_name(wanted_kind));

        qrd::EmulatorInputState input;
        LOGI("nativeLoadRom: prime frame start backend=%s",
             qrd::backend_kind_name(wanted_kind));
        backend->step_frame(input, error); // prime — ignore failure
        LOGI("nativeLoadRom: prime frame done backend=%s err=%s",
             qrd::backend_kind_name(wanted_kind), error.c_str());
        const auto& frame = backend->frame_output();
        if (frame.width == 0 || frame.rgba8888.empty()) {
            return make_jstring(env, "ROM load failed\n\nBackend loaded but emitted no video frame.\nCheck logcat for backend errors.");
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
        load_warning = backend->last_load_warning();
    }

    g_openxr_shell.set_current_backend_kind(wanted_kind, rom_path);
    if (!load_warning.empty()) {
        g_openxr_shell.set_rom_hint_override(load_warning);
    } else if (g_openxr_shell.current_config_is_single_layer()) {
        g_openxr_shell.set_rom_hint_override(
            "This ROM has no per-game depth layers yet.\nShowing flat 2D.");
    } else {
        g_openxr_shell.set_rom_hint_override("");
    }
    g_openxr_shell.set_current_rom(g_last_loaded_rom_filename);
    if (!game_name.empty()) {
        g_openxr_shell.set_current_game_name(game_name);
    }
    set_last_status("ROM loaded: " + g_last_loaded_rom_filename);
    start_emu_thread();
    return make_jstring(env, get_last_status_copy());
}

extern "C" JNIEXPORT void JNICALL
Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeRomPreparationProgress(
    JNIEnv* env, jobject, jstring path, jint percent, jint file_index, jint file_total, jstring file_name)
{
    if (!path) return;
    const char* raw = env->GetStringUTFChars(path, nullptr);
    const std::string rom_path = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(path, raw);
    std::string name;
    if (file_name) {
        const char* raw_name = env->GetStringUTFChars(file_name, nullptr);
        if (raw_name) { name = raw_name; env->ReleaseStringUTFChars(file_name, raw_name); }
    }
    g_openxr_shell.set_rom_load_progress(rom_path, (int)percent, (int)file_index, (int)file_total, name);
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
