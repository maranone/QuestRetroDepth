#include "scummvm_backend.h"
#include "audio_processor.h"

#include <android/log.h>
#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

extern "C" {
    using retrodepth_layer_export_fn_t = void (*)(const char*, uint32_t, uint32_t, const uint8_t*);
}

namespace qrd {

namespace {

constexpr const char* kLogTag = "QuestRetroDepth";
constexpr const char* kFrontendDir = ".";
constexpr const char* kScummVmCoreLibrary = "libscummvm_libretro.so";

ScummVmBackend* g_active_backend = nullptr;

void* g_scummvm_core_handle = nullptr;
void (*p_retro_set_environment)(retro_environment_t) = nullptr;
void (*p_retro_set_video_refresh)(retro_video_refresh_t) = nullptr;
void (*p_retro_set_audio_sample)(retro_audio_sample_t) = nullptr;
void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
void (*p_retro_set_input_poll)(retro_input_poll_t) = nullptr;
void (*p_retro_set_input_state)(retro_input_state_t) = nullptr;
void (*p_retro_init)() = nullptr;
void (*p_retro_deinit)() = nullptr;
void (*p_retro_get_system_info)(retro_system_info*) = nullptr;
void (*p_retro_get_system_av_info)(retro_system_av_info*) = nullptr;
void (*p_retro_set_controller_port_device)(unsigned, unsigned) = nullptr;
bool (*p_retro_load_game)(const retro_game_info*) = nullptr;
void (*p_retro_unload_game)() = nullptr;
void (*p_retro_run)() = nullptr;
void (*p_retro_reset)() = nullptr;
std::size_t (*p_retro_serialize_size)() = nullptr;
bool (*p_retro_serialize)(void*, std::size_t) = nullptr;
bool (*p_retro_unserialize)(const void*, std::size_t) = nullptr;
void* (*p_retro_get_memory_data)(unsigned) = nullptr;
std::size_t (*p_retro_get_memory_size)(unsigned) = nullptr;
void (*p_retrodepth_set_layer_export)(retrodepth_layer_export_fn_t) = nullptr;

template <typename Fn>
bool load_required_symbol(Fn& fn, const char* name, std::string& error_out) {
    fn = reinterpret_cast<Fn>(dlsym(g_scummvm_core_handle, name));
    if (!fn) {
        const char* dl_error = dlerror();
        error_out = std::string("ScummVM: missing required symbol '") + name + "': " +
                    (dl_error ? dl_error : "unknown dlsym error");
        return false;
    }
    return true;
}

const char* scummvm_option_value(const char* key) {
    static const std::unordered_map<std::string, const char*> kOptions = {
        {"scummvm_pointer_device", "mouse"},
        {"scummvm_gamepad_cursor_speed", "1.0"},
        {"scummvm_gamepad_cursor_acceleration_time", "0.2"},
        {"scummvm_analog_response", "linear"},
        {"scummvm_analog_deadzone", "15"},
        {"scummvm_mouse_speed", "1.0"},
        {"scummvm_mouse_fine_control_speed_reduction", "50"},
        {"scummvm_framerate", "60"},
        {"scummvm_samplerate", "44100"},
        {"scummvm_video_hw_acceleration", "disabled"},
        {"scummvm_gui_aspect_ratio", "0"},
        {"scummvm_gui_h_res", "200"},
        {"scummvm_mapper_up", "RETROKE_UP"},
        {"scummvm_mapper_down", "RETROKE_DOWN"},
        {"scummvm_mapper_left", "RETROKE_LEFT"},
        {"scummvm_mapper_right", "RETROKE_RIGHT"},
        {"scummvm_mapper_a", "RETROKE_RETURN"},
        {"scummvm_mapper_b", "RETROKE_ESCAPE"},
        {"scummvm_mapper_x", "RETROKE_F5"},
        {"scummvm_mapper_y", "RETROKE_PERIOD"},
        {"scummvm_mapper_select", "RETROKE_SCUMMVM_VKBD"},
        {"scummvm_mapper_start", "RETROKE_SCUMMVM_GUI"},
        {"scummvm_mapper_l", "RETROKE_INVALID"},
        {"scummvm_mapper_r", "RETROKE_INVALID"},
        {"scummvm_mapper_l2", "RETROKE_INVALID"},
        {"scummvm_mapper_r2", "RETROKE_INVALID"},
        {"scummvm_mapper_l3", "RETROKE_INVALID"},
        {"scummvm_mapper_r3", "RETROKE_INVALID"},
        {"scummvm_mapper_lu", "RETROKE_INVALID"},
        {"scummvm_mapper_ld", "RETROKE_INVALID"},
        {"scummvm_mapper_ll", "RETROKE_INVALID"},
        {"scummvm_mapper_lr", "RETROKE_INVALID"},
        {"scummvm_mapper_ru", "RETROKE_INVALID"},
        {"scummvm_mapper_rd", "RETROKE_INVALID"},
        {"scummvm_mapper_rl", "RETROKE_INVALID"},
        {"scummvm_mapper_rr", "RETROKE_INVALID"},
    };
    const auto it = kOptions.find(key ? key : "");
    return it == kOptions.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// AAudio ring buffer — lock-free SPSC (producer = libretro thread,
// consumer = AAudio data callback thread).
// ---------------------------------------------------------------------------
constexpr int kAudioRingFrames = 8192;
static int16_t  g_scumm_audio_ring[kAudioRingFrames * 2];
static std::atomic<int> g_scumm_ring_write{0};
static std::atomic<int> g_scumm_ring_read{0};
static AAudioStream* g_scumm_aaudio_stream = nullptr;
static int g_scumm_audio_sample_rate = 44100;

static void scumm_audio_ring_push(const int16_t* samples, int frames) {
    int w = g_scumm_ring_write.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        int next = (w + 1) % kAudioRingFrames;
        if (next == g_scumm_ring_read.load(std::memory_order_acquire)) break;
        g_scumm_audio_ring[w * 2 + 0] = samples[i * 2 + 0];
        g_scumm_audio_ring[w * 2 + 1] = samples[i * 2 + 1];
        w = next;
    }
    g_scumm_ring_write.store(w, std::memory_order_release);
}

static aaudio_data_callback_result_t scumm_audio_data_callback(
    AAudioStream*, void*, void* audioData, int32_t numFrames) {
    auto* out = static_cast<int16_t*>(audioData);
    int r = g_scumm_ring_read.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; ++i) {
        int w = g_scumm_ring_write.load(std::memory_order_acquire);
        if (r == w) {
            out[i * 2 + 0] = 0;
            out[i * 2 + 1] = 0;
        } else {
            out[i * 2 + 0] = g_scumm_audio_ring[r * 2 + 0];
            out[i * 2 + 1] = g_scumm_audio_ring[r * 2 + 1];
            r = (r + 1) % kAudioRingFrames;
        }
    }
    g_scumm_ring_read.store(r, std::memory_order_release);
    g_audio_processor.process(out, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void scumm_open_aaudio_stream(int sample_rate) {
    if (g_scumm_aaudio_stream) {
        AAudioStream_close(g_scumm_aaudio_stream);
        g_scumm_aaudio_stream = nullptr;
    }
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, scumm_audio_data_callback, nullptr);
    AAudioStreamBuilder_openStream(builder, &g_scumm_aaudio_stream);
    AAudioStreamBuilder_delete(builder);
    if (g_scumm_aaudio_stream) {
        g_audio_processor.set_sample_rate(sample_rate);
        AAudioStream_requestStart(g_scumm_aaudio_stream);
    }
}

static void scumm_close_aaudio_stream() {
    if (g_scumm_aaudio_stream) {
        AAudioStream_requestStop(g_scumm_aaudio_stream);
        AAudioStream_close(g_scumm_aaudio_stream);
        g_scumm_aaudio_stream = nullptr;
    }
}

// ---------------------------------------------------------------------------
// libretro frontend callbacks
// ---------------------------------------------------------------------------

static int android_log_priority(retro_log_level level) {
    switch (level) {
    case RETRO_LOG_DEBUG: return ANDROID_LOG_DEBUG;
    case RETRO_LOG_INFO:  return ANDROID_LOG_INFO;
    case RETRO_LOG_WARN:  return ANDROID_LOG_WARN;
    case RETRO_LOG_ERROR: return ANDROID_LOG_ERROR;
    default:              return ANDROID_LOG_DEFAULT;
    }
}

static void RETRO_CALLCONV scumm_frontend_log(retro_log_level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(android_log_priority(level), kLogTag, fmt, args);
    va_end(args);
}

static bool RETRO_CALLCONV scumm_frontend_environment(unsigned cmd, void* data) {
    return g_active_backend ? g_active_backend->handle_environment(cmd, data) : false;
}

static void RETRO_CALLCONV scumm_frontend_video_refresh(const void* data, unsigned w, unsigned h, std::size_t pitch) {
    if (g_active_backend) g_active_backend->handle_video_frame(data, w, h, pitch);
}

static void RETRO_CALLCONV scumm_frontend_audio_sample(int16_t l, int16_t r) {
    int16_t buf[2] = {l, r};
    scumm_audio_ring_push(buf, 1);
}

static std::size_t RETRO_CALLCONV scumm_frontend_audio_sample_batch(const int16_t* data, std::size_t frames) {
    scumm_audio_ring_push(data, (int)frames);
    return frames;
}

static void RETRO_CALLCONV scumm_frontend_input_poll() {}

static int16_t RETRO_CALLCONV scumm_frontend_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_active_backend ? g_active_backend->handle_input_state(port, device, index, id) : 0;
}

// ---------------------------------------------------------------------------
// Pixel conversion helpers
// ---------------------------------------------------------------------------

static uint32_t rgba_from_rgb565(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >>  5) & 0x3F) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>( (pixel        & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                         (static_cast<uint32_t>(g) <<  8) | static_cast<uint32_t>(b);
}

static uint32_t rgba_from_xrgb8888(uint32_t pixel) {
    return 0xFF000000u | (pixel & 0x00FFFFFFu);
}

} // namespace

// ---------------------------------------------------------------------------
// ScummVmBackend — per-actor layer export callback
// ---------------------------------------------------------------------------

void ScummVmBackend::s_layer_export_cb(const char* id, uint32_t w, uint32_t h, const uint8_t* bgra) {
    if (!g_active_backend || !id) return;
    const size_t npix = (size_t)w * h;
    CapturedLayer cl;
    cl.id = id; cl.w = w; cl.h = h;
    cl.rgba.resize(npix * 4);
    for (size_t i = 0; i < npix; ++i) {
        cl.rgba[i*4+0] = bgra[i*4+2]; // R  (BGRA→RGBA: swap B and R)
        cl.rgba[i*4+1] = bgra[i*4+1]; // G
        cl.rgba[i*4+2] = bgra[i*4+0]; // B
        cl.rgba[i*4+3] = bgra[i*4+3]; // A
    }
    g_active_backend->m_captured_layers.push_back(std::move(cl));
}

void ScummVmBackend::recompute_depths() {
    const int N = (int)m_layer_order.size();
    if (N == 0) return;
    const float far_bound  = 2.5f + 0.5f * m_depth_spread;
    const float near_bound = 2.5f - 0.5f * m_depth_spread;

    // Count non-ui layers so the spread is computed over game scene layers only.
    // "ui" is pinned in front of everything else below.
    int non_ui_count = 0;
    for (const auto& id : m_layer_order)
        if (id != "ui") ++non_ui_count;

    int non_ui_idx = 0;
    for (int i = 0; i < N; ++i) {
        auto& nl = m_frame.native_layers[i];
        if (m_layer_order[i] == "ui") {
            nl.depth_meters = near_bound - 0.15f; // in front of all scene layers
            nl.is_ui_bar    = true;
        } else {
            const float t = (non_ui_count > 1)
                ? (float)non_ui_idx / (float)(non_ui_count - 1) : 0.0f;
            nl.depth_meters = far_bound + t * (near_bound - far_bound);
            nl.is_ui_bar    = false;
            ++non_ui_idx;
        }
    }
}

ScummVmBackend::ScummVmBackend() {
    std::ostringstream name;
    name << "ScummVM libretro";
    std::string error;
    if (load_core_symbols(error)) {
        retro_system_info info{};
        p_retro_get_system_info(&info);
        if (info.library_name && info.library_name[0] != '\0') {
            name.str(""); name.clear();
            name << info.library_name;
            if (info.library_version && info.library_version[0] != '\0')
                name << " " << info.library_version;
            name << " (libretro)";
        }
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "%s", error.c_str());
        name << " (core missing)";
    }
    m_backend_name = name.str();
    ensure_frame_size(320, 200);
}

ScummVmBackend::~ScummVmBackend() {
    reset_core();
}

const char* ScummVmBackend::backend_name() const {
    return m_backend_name.c_str();
}

double ScummVmBackend::frame_rate_hz() const {
    return m_frame_rate_hz;
}

bool ScummVmBackend::load_core_symbols(std::string& error_out) {
    if (g_scummvm_core_handle && p_retro_load_game) {
        error_out.clear();
        return true;
    }

    g_scummvm_core_handle = dlopen(kScummVmCoreLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!g_scummvm_core_handle) {
        error_out = std::string("ScummVM core library not packaged: ") +
                    (dlerror() ? dlerror() : kScummVmCoreLibrary);
        return false;
    }

    const bool ok =
        load_required_symbol(p_retro_set_environment, "retro_set_environment", error_out) &&
        load_required_symbol(p_retro_set_video_refresh, "retro_set_video_refresh", error_out) &&
        load_required_symbol(p_retro_set_audio_sample, "retro_set_audio_sample", error_out) &&
        load_required_symbol(p_retro_set_audio_sample_batch, "retro_set_audio_sample_batch", error_out) &&
        load_required_symbol(p_retro_set_input_poll, "retro_set_input_poll", error_out) &&
        load_required_symbol(p_retro_set_input_state, "retro_set_input_state", error_out) &&
        load_required_symbol(p_retro_init, "retro_init", error_out) &&
        load_required_symbol(p_retro_deinit, "retro_deinit", error_out) &&
        load_required_symbol(p_retro_get_system_info, "retro_get_system_info", error_out) &&
        load_required_symbol(p_retro_get_system_av_info, "retro_get_system_av_info", error_out) &&
        load_required_symbol(p_retro_set_controller_port_device, "retro_set_controller_port_device", error_out) &&
        load_required_symbol(p_retro_load_game, "retro_load_game", error_out) &&
        load_required_symbol(p_retro_unload_game, "retro_unload_game", error_out) &&
        load_required_symbol(p_retro_run, "retro_run", error_out) &&
        load_required_symbol(p_retro_reset, "retro_reset", error_out) &&
        load_required_symbol(p_retro_serialize_size, "retro_serialize_size", error_out) &&
        load_required_symbol(p_retro_serialize, "retro_serialize", error_out) &&
        load_required_symbol(p_retro_unserialize, "retro_unserialize", error_out) &&
        load_required_symbol(p_retro_get_memory_data, "retro_get_memory_data", error_out) &&
        load_required_symbol(p_retro_get_memory_size, "retro_get_memory_size", error_out);
    if (!ok) return false;

    p_retrodepth_set_layer_export =
        reinterpret_cast<void (*)(retrodepth_layer_export_fn_t)>(
            dlsym(g_scummvm_core_handle, "retrodepth_set_layer_export"));
    if (!p_retrodepth_set_layer_export) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
            "ScummVM core loaded without retrodepth_set_layer_export; composite fallback only.");
    }

    error_out.clear();
    return true;
}

bool ScummVmBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();

    if (!load_core_symbols(error_out)) return false;

    if (rom_path.empty()) {
        error_out = "ScummVM: path is empty.";
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: start path='%s'", rom_path.c_str());

    // Read the .scummvm shortcut file to extract game ID for logging.
    // The libretro ScummVM core reads the file itself; we just pass the path.
    {
        std::ifstream f(rom_path);
        if (f) {
            std::getline(f, m_game_id);
            // strip trailing whitespace
            while (!m_game_id.empty() && (m_game_id.back() == '\r' || m_game_id.back() == '\n' || m_game_id.back() == ' '))
                m_game_id.pop_back();
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: game_id='%s'", m_game_id.c_str());

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: calling ensure_core_initialized");
    if (!ensure_core_initialized(error_out)) return false;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: ensure_core_initialized done");

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = nullptr;
    game_info.size = 0;
    game_info.meta = nullptr;

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: calling retro_load_game");
    const bool load_ok = p_retro_load_game(&game_info);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: retro_load_game returned %d", (int)load_ok);
    if (!load_ok) {
        error_out = "ScummVM: retro_load_game failed for '" + rom_path + "'.";
        reset_core();
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: calling get_system_av_info");
    retro_system_av_info av_info{};
    p_retro_get_system_av_info(&av_info);
    update_geometry(av_info.geometry);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 60.0;
    g_scumm_audio_sample_rate = (av_info.timing.sample_rate > 0)
        ? (int)av_info.timing.sample_rate : 44100;

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM AV: fps=%.4f sample_rate=%.1f geometry=%ux%u game_id='%s'",
        m_frame_rate_hz, av_info.timing.sample_rate,
        av_info.geometry.base_width, av_info.geometry.base_height,
        m_game_id.c_str());

    scumm_open_aaudio_stream(g_scumm_audio_sample_rate);

    m_game_loaded = true;

    // Run warmup frames so we have a valid first frame for the renderer.
    // ScummVM can take many retro_run() calls before emitting the first video frame
    // (engine startup, theme loading, game init), so allow up to 120 iterations.
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: starting warmup (max 120 frames)");
    EmulatorInputState warmup{};
    for (int i = 0; i < 120 && m_video_frame_count == 0; ++i) {
        __android_log_print(ANDROID_LOG_DEBUG, kLogTag,
            "ScummVM load_content: warmup frame %d", i);
        m_input = warmup;
        p_retro_run();
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "ScummVM load_content: warmup done video_frame_count=%llu",
        static_cast<unsigned long long>(m_video_frame_count));

    if (m_video_frame_count == 0) {
        error_out = "ScummVM: game loaded but emitted no video frames after 120 warmup frames.";
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool ScummVmBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) {
        error_out = "ScummVM: no game loaded.";
        return false;
    }
    m_input = input;
    m_captured_layers.clear();
    ++m_step_count;

    p_retro_run();

    // Dynamic layer tracking: mirrors ScummVmSource::poll() on the desktop.
    constexpr uint64_t kStaleFrames = 4;
    bool topology_changed = false;

    for (auto& cl : m_captured_layers) {
        auto it = std::find(m_layer_order.begin(), m_layer_order.end(), cl.id);
        if (it != m_layer_order.end()) {
            const size_t idx = (size_t)(it - m_layer_order.begin());
            m_layer_last_seen[idx] = m_step_count;
            m_frame.native_layers[idx].width = (int)cl.w;
            m_frame.native_layers[idx].height = (int)cl.h;
            m_frame.native_layers[idx].rgba = std::move(cl.rgba);
        } else {
            m_layer_order.push_back(cl.id);
            m_layer_last_seen.push_back(m_step_count);
            NativeLayerFrame f;
            f.id = cl.id;
            f.width  = (int)cl.w;
            f.height = (int)cl.h;
            f.quad_width_meters = 2.56f;
            f.depth_meters = 2.5f;
            f.rgba = std::move(cl.rgba);
            m_frame.native_layers.push_back(std::move(f));
            topology_changed = true;
        }
    }

    // Evict stale actor layers (named layers like "background", "text", "ui" are never evicted).
    for (int i = (int)m_layer_order.size() - 1; i >= 0; --i) {
        const std::string& lid = m_layer_order[(size_t)i];
        if (lid.rfind("actor:", 0) != 0) continue;
        if (m_step_count - m_layer_last_seen[(size_t)i] <= kStaleFrames) continue;
        m_layer_order.erase(m_layer_order.begin() + i);
        m_layer_last_seen.erase(m_layer_last_seen.begin() + i);
        m_frame.native_layers.erase(m_frame.native_layers.begin() + i);
        topology_changed = true;
    }

    if (topology_changed) recompute_depths();

    error_out.clear();
    return true;
}

const FrameOutput& ScummVmBackend::frame_output() const {
    return m_frame;
}

bool ScummVmBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "ScummVM: no game loaded."; return false; }
    const std::size_t size = p_retro_serialize_size();
    if (size == 0) { error_out = "ScummVM: core reports zero savestate size."; return false; }
    out.assign(size, 0);
    if (!p_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "ScummVM: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool ScummVmBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "ScummVM: no game loaded."; return false; }
    if (!data || size == 0) { error_out = "ScummVM: savestate data is empty."; return false; }
    if (!p_retro_unserialize(data, size)) {
        error_out = "ScummVM: retro_unserialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool ScummVmBackend::handle_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto* cb = static_cast<retro_log_callback*>(data);
        cb->log = scumm_frontend_log;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY: {
        auto** dir = static_cast<const char**>(data);
        *dir = kFrontendDir;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const auto fmt = *static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_RGB565 && fmt != RETRO_PIXEL_FORMAT_XRGB8888)
            return false;
        m_pixel_format = fmt;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        update_geometry(*static_cast<const retro_game_geometry*>(data));
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        auto* updated = static_cast<bool*>(data);
        *updated = m_variables_dirty;
        m_variables_dirty = false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        if (!data) return false;
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        var->value = scummvm_option_value(var->key);
        return var->value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
        if (!data) return false;
        auto* supported = static_cast<bool*>(data);
        *supported = false; // ScummVM uses pointer/mouse, not bitmask joypad
        return true;
    }
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE: {
        if (!data) return false;
        auto* refresh_rate = static_cast<double*>(data);
        *refresh_rate = 60.0;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
        auto* result = static_cast<int*>(data);
        *result = 3; // enable both video and audio
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    default:
        return false;
    }
}

void ScummVmBackend::handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);

    if (m_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
        write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
    else
        write_rgb565_frame(static_cast<const uint16_t*>(data), width, height, pitch);

    generate_y_zbuffer(width, height);
}

bool ScummVmBackend::ensure_core_initialized(std::string& error_out) {
    if (m_core_initialized) return true;

    g_active_backend = this;
    if (!load_core_symbols(error_out)) return false;
    p_retro_set_environment(scumm_frontend_environment);
    p_retro_set_video_refresh(scumm_frontend_video_refresh);
    p_retro_set_audio_sample(scumm_frontend_audio_sample);
    p_retro_set_audio_sample_batch(scumm_frontend_audio_sample_batch);
    p_retro_set_input_poll(scumm_frontend_input_poll);
    p_retro_set_input_state(scumm_frontend_input_state);

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "ScummVM: calling retro_init");
    p_retro_init();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "ScummVM: retro_init done");

    if (p_retrodepth_set_layer_export)
        p_retrodepth_set_layer_export(&ScummVmBackend::s_layer_export_cb);
    // Register as pointer/mouse controller on port 0
    p_retro_set_controller_port_device(0, RETRO_DEVICE_MOUSE);

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void ScummVmBackend::reset_core() {
    scumm_close_aaudio_stream();
    if (m_core_initialized) {
        if (p_retro_unload_game) p_retro_unload_game();
        if (p_retro_deinit) p_retro_deinit();
    }
    m_core_initialized = false;
    m_game_loaded = false;
    m_game_id.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    m_variables_dirty = false;
    m_last_mouse_x = 0;
    m_last_mouse_y = 0;
    m_captured_layers.clear();
    m_layer_order.clear();
    m_layer_last_seen.clear();
    m_step_count = 0;
    m_frame.native_layers.clear();
    if (g_active_backend == this) g_active_backend = nullptr;
}

void ScummVmBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto w = std::max(1u, width);
    const auto h = std::max(1u, height);
    if (m_frame.width == w && m_frame.height == h &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(w) * h)
        return;

    m_frame.width = w;
    m_frame.height = h;
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    m_frame.rgba8888.assign(npix, 0xFF000000u);
    m_frame.zbuffer.assign(npix, 0u);
    m_frame.depth_map.assign(npix, 0u);
    // No hardware layers — single composite frame; layers array stays empty.
    m_frame.layers.clear();
    m_frame.visible_source_id.clear();
}

void ScummVmBackend::update_geometry(const retro_game_geometry& geometry) {
    ensure_frame_size(geometry.base_width, geometry.base_height);
}

void ScummVmBackend::write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const auto rgba = rgba_from_rgb565(row[x]);
            dst[x] = rgba;
            has_visible = has_visible || ((rgba & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible;
}

void ScummVmBackend::write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const auto rgba = rgba_from_xrgb8888(row[x]);
            dst[x] = rgba;
            has_visible = has_visible || ((rgba & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible;
}

void ScummVmBackend::generate_y_zbuffer(unsigned width, unsigned height) {
    // Y → depth: top dead zone (sky) = far, active zone = power curve, bottom dead zone (floor) = near.
    // Matches painter's-algorithm ordering used by classic SCUMM adventure engines.
    const std::size_t npix = static_cast<std::size_t>(width) * height;
    if (m_frame.zbuffer.size() != npix) return;

    constexpr float kTopDead    = 0.20f; // top 20% → z=0 (far; sky/ceiling)
    constexpr float kBottomDead = 0.10f; // bottom 10% → z=255 (near; floor)
    constexpr float kGamma      = 1.5f;  // power curve: concentrates depth variation mid-screen

    for (unsigned y = 0; y < height; ++y) {
        const float fy = (height > 1u) ? static_cast<float>(y) / static_cast<float>(height - 1u) : 0.0f;
        uint8_t z;
        if (fy <= kTopDead) {
            z = 0;
        } else if (fy >= (1.0f - kBottomDead)) {
            z = 255;
        } else {
            const float t = (fy - kTopDead) / (1.0f - kBottomDead - kTopDead);
            z = static_cast<uint8_t>(std::pow(t, kGamma) * 255.0f + 0.5f);
        }
        uint8_t* row_zbuf  = m_frame.zbuffer.data()   + static_cast<std::size_t>(y) * width;
        uint8_t* row_depth = m_frame.depth_map.data() + static_cast<std::size_t>(y) * width;
        std::fill(row_zbuf,  row_zbuf  + width, z);
        std::fill(row_depth, row_depth + width, z);
    }
}

int16_t ScummVmBackend::handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port != 0) return 0;

    if (device == RETRO_DEVICE_MOUSE) {
        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X: {
            // Return delta since last query, then reset.
            const int16_t dx = m_input.mouse_x - m_last_mouse_x;
            m_last_mouse_x = m_input.mouse_x;
            return dx;
        }
        case RETRO_DEVICE_ID_MOUSE_Y: {
            const int16_t dy = m_input.mouse_y - m_last_mouse_y;
            m_last_mouse_y = m_input.mouse_y;
            return dy;
        }
        case RETRO_DEVICE_ID_MOUSE_LEFT:
            return m_input.mouse_left_button ? 1 : 0;
        case RETRO_DEVICE_ID_MOUSE_RIGHT:
            return m_input.mouse_right_button ? 1 : 0;
        default:
            return 0;
        }
    }

    if (device == RETRO_DEVICE_POINTER && index == 0) {
        // Some ScummVM versions also query the pointer device.
        // Map to the same absolute coords scaled to libretro pointer range [-0x7fff, 0x7fff].
        const int fw = (int)m_frame.width;
        const int fh = (int)m_frame.height;
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X:
            return (fw > 0)
                ? static_cast<int16_t>(((int)m_input.mouse_x * 0xFFFE / fw) - 0x7FFF)
                : 0;
        case RETRO_DEVICE_ID_POINTER_Y:
            return (fh > 0)
                ? static_cast<int16_t>(((int)m_input.mouse_y * 0xFFFE / fh) - 0x7FFF)
                : 0;
        case RETRO_DEVICE_ID_POINTER_PRESSED:
            return m_input.mouse_left_button ? 1 : 0;
        default:
            return 0;
        }
    }

    return 0;
}

void ScummVmBackend::set_auto_frame_skip(bool /*enabled*/) {}
void ScummVmBackend::set_layer_capture_mask(uint32_t mask) { m_layer_capture_mask = mask; }

RomHeaderInfo ScummVmBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (!m_game_id.empty()) {
        info.has_header = true;
        info.game_name  = m_game_id;
    }
    return info;
}

const uint32_t* ScummVmBackend::get_z_histogram() const { return nullptr; }
const uint8_t*  ScummVmBackend::system_ram_data()   const { return nullptr; }
std::size_t     ScummVmBackend::system_ram_size()    const { return 0; }

} // namespace qrd
