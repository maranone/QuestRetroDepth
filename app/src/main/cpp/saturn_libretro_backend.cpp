#include "saturn_libretro_backend.h"
#include "saturn_layer_capture.h"
#include "audio_processor.h"

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <sstream>

extern "C" {
void     saturn_retro_set_environment(retro_environment_t cb);
void     saturn_retro_set_video_refresh(retro_video_refresh_t cb);
void     saturn_retro_set_audio_sample(retro_audio_sample_t cb);
void     saturn_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void     saturn_retro_set_input_poll(retro_input_poll_t cb);
void     saturn_retro_set_input_state(retro_input_state_t cb);
unsigned saturn_retro_api_version(void);
void     saturn_retro_set_controller_port_device(unsigned port, unsigned device);
void     saturn_retro_get_system_info(struct retro_system_info* info);
void     saturn_retro_get_system_av_info(struct retro_system_av_info* info);
size_t   saturn_retro_serialize_size(void);
bool     saturn_retro_serialize(void* data, size_t size);
bool     saturn_retro_unserialize(const void* data, size_t size);
void     saturn_retro_cheat_reset(void);
void     saturn_retro_cheat_set(unsigned index, bool enabled, const char* code);
bool     saturn_retro_load_game(const struct retro_game_info* info);
bool     saturn_retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info);
void     saturn_retro_unload_game(void);
unsigned saturn_retro_get_region(void);
void*    saturn_retro_get_memory_data(unsigned type);
size_t   saturn_retro_get_memory_size(unsigned type);
void     saturn_retro_reset(void);
void     saturn_retro_run(void);
void     saturn_retro_init(void);
void     saturn_retro_deinit(void);
}

namespace qrd {

namespace {

constexpr const char* kLogTag       = "QuestRetroDepth";
constexpr int         kWarmupFrames = 30; // Saturn BIOS boot takes longer than console-cart cores

std::string g_system_dir_storage = ".";
constexpr int kSaturnLayerCount = SATURN_LAYER_COUNT;

SaturnLibretroBackend* g_active_backend = nullptr;
bool g_core_ever_initialized = false;

// AAudio ring buffer
constexpr int kAudioRingFrames = 8192;
static int16_t g_audio_ring[kAudioRingFrames * 2];
static std::atomic<int> g_ring_write{0};
static std::atomic<int> g_ring_read{0};
static AAudioStream* g_aaudio_stream = nullptr;
static int g_audio_sample_rate = 44100;
static retro_audio_buffer_status_callback_t g_audio_buffer_status_callback = nullptr;

static void audio_ring_push(const int16_t* samples, int frames) {
    int w = g_ring_write.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        const int next = (w + 1) % kAudioRingFrames;
        if (next == g_ring_read.load(std::memory_order_acquire)) break;
        g_audio_ring[w * 2 + 0] = samples[i * 2 + 0];
        g_audio_ring[w * 2 + 1] = samples[i * 2 + 1];
        w = next;
    }
    g_ring_write.store(w, std::memory_order_release);
}

static unsigned audio_ring_occupancy_percent() {
    const int w = g_ring_write.load(std::memory_order_acquire);
    const int r = g_ring_read.load(std::memory_order_acquire);
    const int queued = (w >= r) ? (w - r) : (kAudioRingFrames - r + w);
    return static_cast<unsigned>(std::clamp((queued * 100) / (kAudioRingFrames - 1), 0, 100));
}

static void report_audio_buffer_status() {
    if (!g_audio_buffer_status_callback) return;
    const unsigned occupancy = audio_ring_occupancy_percent();
    g_audio_buffer_status_callback(true, occupancy, occupancy < 20);
}

static aaudio_data_callback_result_t audio_data_callback(
    AAudioStream*, void*, void* audioData, int32_t numFrames) {
    auto* out = static_cast<int16_t*>(audioData);
    int r = g_ring_read.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; ++i) {
        const int w = g_ring_write.load(std::memory_order_acquire);
        if (r == w) {
            out[i * 2 + 0] = 0;
            out[i * 2 + 1] = 0;
        } else {
            out[i * 2 + 0] = g_audio_ring[r * 2 + 0];
            out[i * 2 + 1] = g_audio_ring[r * 2 + 1];
            r = (r + 1) % kAudioRingFrames;
        }
    }
    g_ring_read.store(r, std::memory_order_release);
    g_audio_processor.process(out, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void open_aaudio_stream(int sample_rate) {
    if (g_aaudio_stream) {
        AAudioStream_close(g_aaudio_stream);
        g_aaudio_stream = nullptr;
    }
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, audio_data_callback, nullptr);
    AAudioStreamBuilder_openStream(builder, &g_aaudio_stream);
    AAudioStreamBuilder_delete(builder);
    if (g_aaudio_stream) {
        g_audio_processor.set_sample_rate(sample_rate);
        AAudioStream_requestStart(g_aaudio_stream);
    }
}

static void close_aaudio_stream() {
    g_ring_write.store(0, std::memory_order_release);
    g_ring_read.store(0, std::memory_order_release);
    if (g_aaudio_stream) {
        AAudioStream_requestStop(g_aaudio_stream);
        AAudioStream_close(g_aaudio_stream);
        g_aaudio_stream = nullptr;
    }
}

static int android_log_priority(retro_log_level level) {
    switch (level) {
    case RETRO_LOG_DEBUG: return ANDROID_LOG_DEBUG;
    case RETRO_LOG_INFO:  return ANDROID_LOG_INFO;
    case RETRO_LOG_WARN:  return ANDROID_LOG_WARN;
    case RETRO_LOG_ERROR: return ANDROID_LOG_ERROR;
    default:              return ANDROID_LOG_DEFAULT;
    }
}

static void RETRO_CALLCONV frontend_log_printf(retro_log_level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(android_log_priority(level), kLogTag, fmt, args);
    va_end(args);
}

static bool RETRO_CALLCONV frontend_environment(unsigned cmd, void* data) {
    return g_active_backend ? g_active_backend->handle_environment(cmd, data) : false;
}

static void RETRO_CALLCONV frontend_video_refresh(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (g_active_backend) g_active_backend->handle_video_frame(data, width, height, pitch);
}

static void RETRO_CALLCONV frontend_audio_sample(int16_t l, int16_t r) {
    int16_t buf[2] = { l, r };
    audio_ring_push(buf, 1);
}

static std::size_t RETRO_CALLCONV frontend_audio_sample_batch(const int16_t* data, std::size_t frames) {
    audio_ring_push(data, static_cast<int>(frames));
    return frames;
}

static void RETRO_CALLCONV frontend_input_poll() {}

static int16_t RETRO_CALLCONV frontend_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_active_backend ? g_active_backend->handle_input_state(port, device, index, id) : 0;
}

static uint32_t rgba_from_xrgb8888(uint32_t pixel) {
    return 0xFF000000u | (pixel & 0x00FFFFFFu);
}

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base  = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot   = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

} // namespace

void set_saturn_frontend_directory(const std::string& dir) {
    g_system_dir_storage = dir;
}

// ---------------------------------------------------------------------------
// SaturnLibretroBackend
// ---------------------------------------------------------------------------

SaturnLibretroBackend::SaturnLibretroBackend() {
    retro_system_info info{};
    saturn_retro_get_system_info(&info);

    std::ostringstream name;
    if (info.library_name && info.library_name[0] != '\0') {
        name << info.library_name;
        if (info.library_version && info.library_version[0] != '\0')
            name << " " << info.library_version;
        name << " (libretro)";
    } else {
        name << "Yaba Sanshiro 2 (libretro)";
    }
    m_backend_name = name.str();

    m_frame.layers.resize(kSaturnLayerCount);
    ensure_frame_size(352, 240);
}

SaturnLibretroBackend::~SaturnLibretroBackend() {
    reset_core();
}

const char* SaturnLibretroBackend::backend_name() const { return m_backend_name.c_str(); }

double SaturnLibretroBackend::frame_rate_hz() const { return m_frame_rate_hz; }

bool SaturnLibretroBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    m_last_load_warning.clear();
    if (rom_path.empty()) { error_out = "Saturn: ROM path is empty."; return false; }
    if (!ensure_core_initialized(error_out)) return false;

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = nullptr;
    game_info.size = 0;
    game_info.meta = nullptr;
    if (!saturn_retro_load_game(&game_info)) {
        error_out = "Saturn: retro_load_game failed (check BIOS files in " +
                    g_system_dir_storage + ").";
        reset_core();
        return false;
    }

    retro_system_av_info av_info{};
    saturn_retro_get_system_av_info(&av_info);
    ensure_frame_size(av_info.geometry.base_width, av_info.geometry.base_height);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 60.0;
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 44100;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "Saturn AV: fps=%.4f sample_rate=%.1f geometry=%ux%u",
                        m_frame_rate_hz,
                        av_info.timing.sample_rate,
                        av_info.geometry.base_width,
                        av_info.geometry.base_height);
    if (!m_preview_mode || m_preview_allow_audio) open_aaudio_stream(g_audio_sample_rate);

    m_loaded_rom_path = rom_path;
    m_game_loaded = true;

    EmulatorInputState warmup{};
    for (int i = 0; i < kWarmupFrames; ++i) {
        m_input = warmup;
        report_audio_buffer_status();
        saturn_lc_frame_begin();
        saturn_retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) break;
    }

    if (m_video_frame_count == 0) {
        error_out = "Saturn: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool SaturnLibretroBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) { error_out = "Saturn: no ROM loaded."; return false; }
    m_input = input;
    report_audio_buffer_status();
    saturn_lc_frame_begin();
    saturn_retro_run();
    error_out.clear();
    return true;
}

const FrameOutput& SaturnLibretroBackend::frame_output() const { return m_frame; }

bool SaturnLibretroBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "Saturn: no ROM loaded."; return false; }
    const std::size_t size = saturn_retro_serialize_size();
    if (size == 0) { error_out = "Saturn: zero savestate size."; return false; }
    out.assign(size, 0);
    if (!saturn_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "Saturn: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool SaturnLibretroBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "Saturn: no ROM loaded."; return false; }
    if (!data || size == 0) { error_out = "Saturn: savestate data empty."; return false; }
    if (!saturn_retro_unserialize(data, size)) {
        error_out = "Saturn: retro_unserialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

void SaturnLibretroBackend::set_auto_frame_skip(bool enabled) {
    m_auto_frame_skip = enabled;
}
void SaturnLibretroBackend::set_layer_capture_mask(uint32_t mask) {
    m_layer_capture_mask = mask;
    saturn_lc_set_layer_capture_mask(mask);
}

void SaturnLibretroBackend::set_gun_mode(bool enabled, int /*peripheral*/) {
    m_gun_mode = enabled;
    apply_controller_ports();
}

void SaturnLibretroBackend::set_dual_gun_mode(bool enabled) {
    if (m_dual_gun_mode == enabled) return;
    m_dual_gun_mode = enabled;
    apply_controller_ports();
}

void SaturnLibretroBackend::apply_controller_ports() {
    if (!m_core_initialized) return;
    saturn_retro_set_controller_port_device(0, m_gun_mode ? RETRO_DEVICE_LIGHTGUN : RETRO_DEVICE_JOYPAD);
    // Port 1 is player two: a second gun when dual-wielding, otherwise a plain
    // pad, which is what it has always been.
    saturn_retro_set_controller_port_device(
        1, (m_gun_mode && m_dual_gun_mode) ? RETRO_DEVICE_LIGHTGUN : RETRO_DEVICE_JOYPAD);
}

RomHeaderInfo SaturnLibretroBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* SaturnLibretroBackend::get_z_histogram() const { return nullptr; }

const uint8_t* SaturnLibretroBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(saturn_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t SaturnLibretroBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return saturn_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

bool SaturnLibretroBackend::handle_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        if (!data) return false;
        auto* cb = static_cast<retro_log_callback*>(data);
        cb->log = frontend_log_printf;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        if (!data) return false;
        auto** dir = static_cast<const char**>(data);
        *dir = g_system_dir_storage.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!data) return false;
        // Core requires XRGB8888; reject anything else.
        return *static_cast<const retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    }
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        auto* callback = static_cast<const retro_audio_buffer_status_callback*>(data);
        g_audio_buffer_status_callback = callback ? callback->callback : nullptr;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        if (!data) return false;
        *static_cast<bool*>(data) = false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        // The core builds and registers its ARM64 SH2 dynamic recompiler
        // (DYNAREC_DEVMIYAX) but only actually selects it if this environment
        // query answers "dynarec" for its "yabasanshiro_sh2coretype" core
        // option -- previously always returning false left it silently
        // defaulting to the plain interpreter, so the dynarec this core was
        // vendored for was never engaged.
        auto* var = static_cast<retro_variable*>(data);
        if (var && var->key && std::strcmp(var->key, "yabasanshiro_sh2coretype") == 0) {
            var->value = "dynarec";
            return true;
        }
        // QuestRetroDepth owns this setting rather than exposing the core's
        // full core-option UI, same as MameBackend's "mame_autoframeskip" --
        // queried by check_variables() before retro_load_game() builds the
        // machine (see rumble/stable_retro/cores/saturn/src/libretro/libretro.c).
        if (var && var->key && std::strcmp(var->key, "yabasanshiro_frameskip") == 0) {
            var->value = m_auto_frame_skip ? "enabled" : "disabled";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
        if (data) *static_cast<bool*>(data) = true;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
        if (!data) return false;
        *static_cast<int*>(data) = 3;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        if (data) {
            const auto& geom = *static_cast<const retro_game_geometry*>(data);
            ensure_frame_size(geom.base_width, geom.base_height);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return false;
    default:
        return false;
    }
}

void SaturnLibretroBackend::handle_video_frame(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);
    write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
    capture_layers(width, height);
}

bool SaturnLibretroBackend::ensure_core_initialized(std::string& error_out) {
    g_active_backend = this;
    saturn_retro_set_environment(frontend_environment);
    saturn_retro_set_video_refresh(frontend_video_refresh);
    saturn_retro_set_audio_sample(frontend_audio_sample);
    saturn_retro_set_audio_sample_batch(frontend_audio_sample_batch);
    saturn_retro_set_input_poll(frontend_input_poll);
    saturn_retro_set_input_state(frontend_input_state);
    if (!g_core_ever_initialized) {
        saturn_retro_init();
        g_core_ever_initialized = true;
    }
    m_core_initialized = true;
    apply_controller_ports();
    error_out.clear();
    return true;
}

void SaturnLibretroBackend::soft_reset() {
    if (m_core_initialized && m_game_loaded) {
        g_active_backend = this;
        saturn_retro_reset();
    }
}

void SaturnLibretroBackend::reset_core() {
    close_aaudio_stream();
    if (m_game_loaded) {
        saturn_retro_unload_game();
    }
    m_core_initialized = false;
    m_game_loaded      = false;
    m_loaded_rom_path.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kSaturnLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    if (g_active_backend == this) g_active_backend = nullptr;
}

void SaturnLibretroBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto w = std::max(1u, width);
    const auto h = std::max(1u, height);
    if (m_frame.width == w && m_frame.height == h &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(w) * h) return;

    m_frame.width  = w;
    m_frame.height = h;
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    m_frame.rgba8888.assign(npix, 0xFF000000u);
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kSaturnLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
}

void SaturnLibretroBackend::write_xrgb8888_frame(
    const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
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

void SaturnLibretroBackend::capture_layers(unsigned width, unsigned height) {
    m_frame.layers.resize(kSaturnLayerCount);
    if (!m_layer_capture_mask) {
        for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
        return;
    }
    const std::size_t npix = static_cast<std::size_t>(width) * height;
    const unsigned cap_w = std::min<unsigned>(width, SATURN_LC_MAX_W);
    const unsigned cap_h = std::min<unsigned>(height, SATURN_LC_MAX_H);
    for (int li = 0; li < kSaturnLayerCount; ++li) {
        if (!((m_layer_capture_mask >> li) & 1u)) {
            m_frame.layers[li].rgba.clear();
            m_frame.layers[li].depth_map.clear();
            continue;
        }
        const uint32_t* src = saturn_lc_get_layer_pixels(li);
        auto& dst = m_frame.layers[li].rgba;
        dst.assign(npix, 0u);
        if (!src) continue;
        // VDP2 lets each plane independently run in normal or hi-res
        // horizontal mode, so a layer's real captured width can differ from
        // the overall frame width (e.g. 704 vs 352) -- nearest-neighbor
        // resample each row into the canvas width instead of truncating,
        // so a hi-res plane doesn't just show its left half.
        const int src_w = saturn_lc_get_layer_width(li);
        if (src_w > 0 && static_cast<unsigned>(src_w) != width) {
            for (unsigned y = 0; y < cap_h; ++y) {
                const uint32_t* src_row = src + static_cast<std::size_t>(y) * SATURN_LC_MAX_W;
                uint32_t* dst_row = dst.data() + static_cast<std::size_t>(y) * width;
                for (unsigned x = 0; x < width; ++x) {
                    const unsigned sx = (x * static_cast<unsigned>(src_w)) / width;
                    dst_row[x] = src_row[sx < static_cast<unsigned>(src_w) ? sx : static_cast<unsigned>(src_w) - 1];
                }
            }
        } else {
            for (unsigned y = 0; y < cap_h; ++y) {
                const uint32_t* src_row = src + static_cast<std::size_t>(y) * SATURN_LC_MAX_W;
                uint32_t* dst_row = dst.data() + static_cast<std::size_t>(y) * width;
                std::memcpy(dst_row, src_row, static_cast<std::size_t>(cap_w) * sizeof(uint32_t));
            }
        }
        m_frame.layers[li].depth_map.clear();
    }
}

int16_t SaturnLibretroBackend::handle_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port == 0 && device == RETRO_DEVICE_LIGHTGUN) {
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return m_input.gun_active ? m_input.gun_screen_x : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return m_input.gun_active ? m_input.gun_screen_y : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return (m_input.gun_active && m_input.gun_offscreen) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return (m_input.gun_active && m_input.gun_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD: return (m_input.gun_active && m_input.gun_offscreen && m_input.gun_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_START: return m_input.button_start ? 1 : 0;
        default: return 0;
        }
    }
    // Port 1 is player two's gun while dual-wielding. It answers only for the
    // lightgun device and only while that gun is live, and otherwise reports
    // offscreen so the game sees no phantom second player.
    if (port == 1 && device == RETRO_DEVICE_LIGHTGUN) {
        const bool live = m_dual_gun_mode && m_input.gun2_active;
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return live ? m_input.gun2_screen_x : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return live ? m_input.gun2_screen_y : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return (!live || m_input.gun2_offscreen) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return (live && m_input.gun2_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
            return (live && m_input.gun2_offscreen && m_input.gun2_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_START: return (live && m_input.gun2_button_start) ? 1 : 0;
        default: return 0;
        }
    }
    if (port != 0 || index != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    // Saturn pad button remap for lr-yabasanshiro's src/libretro/libretro.c
    // input descriptor table (differs from Beetle Saturn's mapping in the
    // C/Z trigger assignment): libretro JOYPAD id -> Saturn button. We answer
    // each libretro id with the QRD input field that maps to it.
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     return m_input.dpad_up     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return m_input.dpad_down   ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return m_input.dpad_left   ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return m_input.dpad_right  ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return m_input.button_a    ? 1 : 0; // JOYPAD_B -> Saturn A
    case RETRO_DEVICE_ID_JOYPAD_A:      return m_input.button_b    ? 1 : 0; // JOYPAD_A -> Saturn B
    case RETRO_DEVICE_ID_JOYPAD_L:      return m_input.button_c    ? 1 : 0; // JOYPAD_L -> Saturn C
    case RETRO_DEVICE_ID_JOYPAD_Y:      return m_input.button_x    ? 1 : 0; // JOYPAD_Y -> Saturn X
    case RETRO_DEVICE_ID_JOYPAD_X:      return m_input.button_y    ? 1 : 0; // JOYPAD_X -> Saturn Y
    case RETRO_DEVICE_ID_JOYPAD_R:      return m_input.button_z    ? 1 : 0; // JOYPAD_R -> Saturn Z
    case RETRO_DEVICE_ID_JOYPAD_L2:     return m_input.button_l    ? 1 : 0; // JOYPAD_L2 -> Saturn L
    case RETRO_DEVICE_ID_JOYPAD_R2:     return m_input.button_r    ? 1 : 0; // JOYPAD_R2 -> Saturn R
    case RETRO_DEVICE_ID_JOYPAD_START:  return m_input.button_start ? 1 : 0;
    default: return 0;
    }
}

} // namespace qrd
