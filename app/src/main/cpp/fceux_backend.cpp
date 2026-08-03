#include "fceux_backend.h"
#include "fceux_layer_capture.h"
#include "audio_processor.h"

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
void     fceux_retro_set_environment(retro_environment_t cb);
void     fceux_retro_set_video_refresh(retro_video_refresh_t cb);
void     fceux_retro_set_audio_sample(retro_audio_sample_t cb);
void     fceux_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void     fceux_retro_set_input_poll(retro_input_poll_t cb);
void     fceux_retro_set_input_state(retro_input_state_t cb);
unsigned fceux_retro_api_version(void);
void     fceux_retro_set_controller_port_device(unsigned port, unsigned device);
void     fceux_retro_get_system_info(struct retro_system_info* info);
void     fceux_retro_get_system_av_info(struct retro_system_av_info* info);
size_t   fceux_retro_serialize_size(void);
bool     fceux_retro_serialize(void* data, size_t size);
bool     fceux_retro_unserialize(const void* data, size_t size);
void     fceux_retro_cheat_reset(void);
void     fceux_retro_cheat_set(unsigned index, bool enabled, const char* code);
bool     fceux_retro_load_game(const struct retro_game_info* info);
bool     fceux_retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info);
void     fceux_retro_unload_game(void);
unsigned fceux_retro_get_region(void);
void*    fceux_retro_get_memory_data(unsigned type);
size_t   fceux_retro_get_memory_size(unsigned type);
void     fceux_retro_reset(void);
void     fceux_retro_run(void);
void     fceux_retro_init(void);
void     fceux_retro_deinit(void);
}

namespace qrd {

namespace {

constexpr const char* kLogTag       = "QuestRetroDepth";
constexpr const char* kFrontendDir  = ".";
constexpr int         kWarmupFrames = 12;
// NES visible-source layers:
// 0=backdrop, 1=BG far, 2=sprites, 3=BG mid, 4=BG near.
constexpr int kNesLayerCount = 5;

FceuxBackend* g_active_backend = nullptr;

// AAudio ring buffer (same design as other backends)
constexpr int kAudioRingFrames = 8192;
static int16_t g_audio_ring[kAudioRingFrames * 2];
static std::atomic<int> g_ring_write{0};
static std::atomic<int> g_ring_read{0};
static AAudioStream* g_aaudio_stream = nullptr;
static int g_audio_sample_rate = 44100; // FCEUmm default
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

static uint32_t rgba_from_rgb565(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5)  & 0x3F) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static uint32_t rgba_from_0rgb1555(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5)  & 0x1F) * 255 / 31);
    const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static uint32_t rgba_from_xrgb8888(uint32_t pixel) {
    return 0xFF000000u | (pixel & 0x00FFFFFFu);
}

static uint8_t nes_synthetic_bg_source_id_for_score(float luma, unsigned y, unsigned height) {
    const float yf = (height > 1u) ? static_cast<float>(y) / static_cast<float>(height - 1u) : 0.5f;
    const float near_score = yf * 0.65f + (1.0f - luma) * 0.35f;
    if (near_score < 0.50f) return 1u;
    return 3u;
}

static float luma_from_rgba(uint32_t rgba) {
    const float r = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((rgba >>  8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>( rgba        & 0xFFu) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base  = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot   = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

} // namespace

// ---------------------------------------------------------------------------
// FceuxBackend
// ---------------------------------------------------------------------------

FceuxBackend::FceuxBackend() {
    m_backend_name = "FCEUmm (libretro)";

    // NES: backdrop, BG, sprites — visible-source extraction, no raw layer captures needed
    m_frame.layers.resize(kNesLayerCount);
    ensure_frame_size(256, 240);
}

FceuxBackend::~FceuxBackend() {
    reset_core();
}

const char* FceuxBackend::backend_name() const { return m_backend_name.c_str(); }

double FceuxBackend::frame_rate_hz() const { return m_frame_rate_hz; }

bool FceuxBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm load: begin path=%s", rom_path.c_str());
    if (rom_path.empty()) { error_out = "FCEUmm: ROM path is empty."; return false; }
    if (!ensure_core_initialized(error_out)) return false;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm load: core initialized");
    retro_system_info system_info{};
    fceux_retro_get_system_info(&system_info);
    if (system_info.library_name && system_info.library_name[0] != '\0') {
        std::ostringstream name;
        name << system_info.library_name;
        if (system_info.library_version && system_info.library_version[0] != '\0')
            name << " " << system_info.library_version;
        name << " (libretro)";
        m_backend_name = name.str();
    }

    {
        std::ifstream f(rom_path, std::ios::binary | std::ios::ate);
        if (!f) { error_out = "FCEUmm: unable to open ROM file."; return false; }
        const std::streamsize sz = f.tellg();
        if (sz <= 0) { error_out = "FCEUmm: ROM file is empty."; return false; }
        m_rom_bytes.assign(static_cast<std::size_t>(sz), 0);
        f.seekg(0, std::ios::beg);
        if (!f.read(reinterpret_cast<char*>(m_rom_bytes.data()), sz)) {
            error_out = "FCEUmm: failed to read ROM file.";
            m_rom_bytes.clear();
            return false;
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm load: read %zu bytes",
                        m_rom_bytes.size());

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = m_rom_bytes.data();
    game_info.size = m_rom_bytes.size();
    game_info.meta = nullptr;
    prepare_game_info_ext(rom_path);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm load: calling retro_load_game");
    if (!fceux_retro_load_game(&game_info)) {
        error_out = "FCEUmm: retro_load_game failed.";
        reset_core();
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm load: retro_load_game OK");

    retro_system_av_info av_info{};
    fceux_retro_get_system_av_info(&av_info);
    ensure_frame_size(av_info.geometry.base_width, av_info.geometry.base_height);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 60.0988;
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 44100;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "FCEUmm AV: fps=%.4f sample_rate=%.1f geometry=%ux%u",
                        m_frame_rate_hz,
                        av_info.timing.sample_rate,
                        av_info.geometry.base_width,
                        av_info.geometry.base_height);
    open_aaudio_stream(g_audio_sample_rate);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm load: audio opened");

    m_loaded_rom_path = rom_path;
    m_game_loaded = true;

    EmulatorInputState warmup{};
    for (int i = 0; i < kWarmupFrames; ++i) {
        m_input = warmup;
        report_audio_buffer_status();
        fceux_retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) break;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "FCEUmm load: warmup frames=%llu visible=%d",
                        static_cast<unsigned long long>(m_video_frame_count),
                        m_last_frame_had_visible_pixels ? 1 : 0);

    if (m_video_frame_count == 0) {
        error_out = "FCEUmm: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool FceuxBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) { error_out = "FCEUmm: no ROM loaded."; return false; }
    m_input = input;
    report_audio_buffer_status();
    fceux_retro_run();
    error_out.clear();
    return true;
}

const FrameOutput& FceuxBackend::frame_output() const { return m_frame; }

bool FceuxBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "FCEUmm: no ROM loaded."; return false; }
    const std::size_t size = fceux_retro_serialize_size();
    if (size == 0) { error_out = "FCEUmm: zero savestate size."; return false; }
    out.assign(size, 0);
    if (!fceux_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "FCEUmm: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool FceuxBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "FCEUmm: no ROM loaded."; return false; }
    if (!data || size == 0) { error_out = "FCEUmm: savestate data empty."; return false; }
    if (!fceux_retro_unserialize(data, size)) {
        error_out = "FCEUmm: retro_unserialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

void FceuxBackend::set_auto_frame_skip(bool /*enabled*/) {}
void FceuxBackend::set_layer_capture_mask(uint32_t mask) {
    m_layer_capture_mask = mask;
    fceux_lc_set_capture_mask(mask & 0x1Fu);
}

RomHeaderInfo FceuxBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* FceuxBackend::get_z_histogram() const { return nullptr; }

const uint8_t* FceuxBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(fceux_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t FceuxBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return fceux_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

bool FceuxBackend::handle_environment(unsigned cmd, void* data) {
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
        *dir = kFrontendDir;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!data) return false;
        const auto fmt = *static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 &&
            fmt != RETRO_PIXEL_FORMAT_RGB565 &&
            fmt != RETRO_PIXEL_FORMAT_XRGB8888) {
            return false;
        }
        m_pixel_format = fmt;
        return true;
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
        return false;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
        if (data) *static_cast<bool*>(data) = true;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
        if (!data) return false;
        *static_cast<int*>(data) = 3; // enable both audio and video
        return true;
    }
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT: {
        return false;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (data) {
            const auto& geom = *static_cast<const retro_game_geometry*>(data);
            ensure_frame_size(geom.base_width, geom.base_height);
        }
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        if (data) *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        if (data) *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        if (data) *static_cast<unsigned*>(data) = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        return false;
    case RETRO_ENVIRONMENT_SET_VARIABLE:
        return false;
    default:
        return false;
    }
}

void FceuxBackend::handle_video_frame(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);
    switch (m_pixel_format) {
    case RETRO_PIXEL_FORMAT_RGB565:
        write_rgb565_frame(static_cast<const uint16_t*>(data), width, height, pitch);
        break;
    case RETRO_PIXEL_FORMAT_XRGB8888:
        write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
        break;
    case RETRO_PIXEL_FORMAT_0RGB1555:
    default:
        write_0rgb1555_frame(static_cast<const uint16_t*>(data), width, height, pitch);
        break;
    }

    // Copy the per-pixel visible-source IDs captured by the PPU hook.
    // The capture buffer is always 256x240. FCEUmm may report a smaller rendered
    // frame (e.g. 240x224) when overscan cropping is active, so copy the
    // crop-centred sub-region row by row when the sizes differ.
    unsigned nes_w = 0, nes_h = 0;
    const uint8_t* vs = fceux_lc_get_visible_source(&nes_w, &nes_h);
    if (vs && nes_w >= width && nes_h >= height &&
        m_frame.visible_source_id.size() == static_cast<std::size_t>(width) * height) {
        const unsigned h_off = (nes_w - width) / 2;
        const unsigned v_off = (nes_h - height) / 2;
        if (h_off == 0 && v_off == 0) {
            std::memcpy(m_frame.visible_source_id.data(), vs,
                        static_cast<std::size_t>(width) * height);
        } else {
            for (unsigned row = 0; row < height; ++row) {
                std::memcpy(m_frame.visible_source_id.data() + row * width,
                            vs + (row + v_off) * nes_w + h_off,
                            width);
            }
        }
    } else if (m_frame.visible_source_id.size() == static_cast<std::size_t>(width) * height) {
        std::fill(m_frame.visible_source_id.begin(), m_frame.visible_source_id.end(), 0xFFu);
    }

    // Split final RGBA frame into per-layer buffers using visible-source IDs.
    // NES only has one hardware BG plane, so visible BG pixels are split into
    // generic far/mid/near buckets to avoid rendering the entire BG as one flat
    // depth sheet. Sprites stay on their real hardware layer.
    const std::size_t npix = static_cast<std::size_t>(width) * height;
    m_frame.layers.resize(kNesLayerCount);
    auto& vsid = m_frame.visible_source_id;
    const auto& src  = m_frame.rgba8888;
    if (vsid.size() == npix && src.size() == npix && (m_layer_capture_mask & 0x1Fu)) {
        for (int li = 0; li < kNesLayerCount; ++li) {
            if (m_frame.layers[li].rgba.size() != npix)
                m_frame.layers[li].rgba.resize(npix);
            std::fill(m_frame.layers[li].rgba.begin(), m_frame.layers[li].rgba.end(), 0u);
        }
        m_frame.layers[2].depth_map.clear();

        constexpr unsigned kTileCell = 8;
        const unsigned tile_cols = (width + kTileCell - 1u) / kTileCell;
        const unsigned tile_rows = (height + kTileCell - 1u) / kTileCell;
        std::vector<uint8_t> bg_tile_source(static_cast<std::size_t>(tile_cols) * tile_rows, 1u);
        for (unsigned ty = 0; ty < tile_rows; ++ty) {
            for (unsigned tx = 0; tx < tile_cols; ++tx) {
                float luma_sum = 0.0f;
                unsigned bg_count = 0;
                const unsigned x0 = tx * kTileCell;
                const unsigned y0 = ty * kTileCell;
                const unsigned x1 = std::min(width, x0 + kTileCell);
                const unsigned y1 = std::min(height, y0 + kTileCell);
                for (unsigned y = y0; y < y1; ++y) {
                    for (unsigned x = x0; x < x1; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * width + x;
                        if (vsid[i] != 1u) continue;
                        luma_sum += luma_from_rgba(src[i]);
                        bg_count++;
                    }
                }
                if (bg_count > 0) {
                    const float avg_luma = luma_sum / static_cast<float>(bg_count);
                    const unsigned cy = std::min(height - 1u, y0 + kTileCell / 2u);
                    bg_tile_source[static_cast<std::size_t>(ty) * tile_cols + tx] =
                        nes_synthetic_bg_source_id_for_score(avg_luma, cy, height);
                }
            }
        }

        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * width + x;
                uint8_t sid = vsid[i];
                if (sid == 1u) {
                    const unsigned tx = std::min(tile_cols - 1u, x / kTileCell);
                    const unsigned ty = std::min(tile_rows - 1u, y / kTileCell);
                    sid = bg_tile_source[static_cast<std::size_t>(ty) * tile_cols + tx];
                    vsid[i] = sid;
                }
                if (sid < kNesLayerCount) {
                    m_frame.layers[sid].rgba[i] = src[i];
                }
            }
        }
        static int s_synthetic_log_frame = 0;
        if ((s_synthetic_log_frame++ % 120) == 0) {
            int cnt[kNesLayerCount] = {0, 0, 0, 0, 0};
            int other = 0;
            for (std::size_t i = 0; i < npix; ++i) {
                const uint8_t sid = vsid[i];
                if (sid < kNesLayerCount) cnt[sid]++;
                else other++;
            }
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                "NES synthetic layers: backdrop=%d bg_far=%d sprites=%d bg_mid=%d bg_near=%d other=%d mask=0x%X",
                cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], other, m_layer_capture_mask);
        }
    } else {
        static int s_synthetic_skip_log_frame = 0;
        if ((s_synthetic_skip_log_frame++ % 120) == 0) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "NES synthetic layers skipped: vsid=%zu rgba=%zu npix=%zu mask=0x%X",
                vsid.size(), src.size(), npix, m_layer_capture_mask);
        }
        for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    }
}

bool FceuxBackend::ensure_core_initialized(std::string& error_out) {
    if (m_core_initialized) return true;

    g_active_backend = this;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm init: set callbacks start");
    fceux_retro_set_environment(frontend_environment);
    fceux_retro_set_video_refresh(frontend_video_refresh);
    fceux_retro_set_audio_sample(frontend_audio_sample);
    fceux_retro_set_audio_sample_batch(frontend_audio_sample_batch);
    fceux_retro_set_input_poll(frontend_input_poll);
    fceux_retro_set_input_state(frontend_input_state);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm init: retro_init start");
    fceux_retro_init();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "FCEUmm init: retro_init done");

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void FceuxBackend::reset_core() {
    close_aaudio_stream();
    if (m_core_initialized) {
        if (m_game_loaded) fceux_retro_unload_game();
        fceux_retro_deinit();
    }
    m_core_initialized = false;
    m_game_loaded      = false;
    m_loaded_rom_path.clear();
    m_content_dir.clear();
    m_content_name.clear();
    m_content_ext.clear();
    m_game_info_ext = {};
    m_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kNesLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    if (g_active_backend == this) g_active_backend = nullptr;
}

void FceuxBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto w = std::max(1u, width);
    const auto h = std::max(1u, height);
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    if (m_frame.width == w && m_frame.height == h &&
        m_frame.rgba8888.size() == npix &&
        m_frame.visible_source_id.size() == npix &&
        m_frame.layers.size() == kNesLayerCount) return;

    m_frame.width  = w;
    m_frame.height = h;
    if (m_frame.rgba8888.size() != npix) {
        m_frame.rgba8888.assign(npix, 0xFF000000u);
    }
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.assign(npix, 0xFFu);
    m_frame.layers.resize(kNesLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
}

void FceuxBackend::prepare_game_info_ext(const std::string& rom_path) {
    m_loaded_rom_path = rom_path;

    const auto slash = rom_path.find_last_of("/\\");
    const std::string base = (slash == std::string::npos) ? rom_path : rom_path.substr(slash + 1);
    m_content_dir = (slash == std::string::npos) ? "." : rom_path.substr(0, slash);

    const auto dot = base.find_last_of('.');
    if (dot == std::string::npos) {
        m_content_name = base;
        m_content_ext.clear();
    } else {
        m_content_name = base.substr(0, dot);
        m_content_ext = lower_ascii(base.substr(dot + 1));
    }

    m_game_info_ext = {};
    m_game_info_ext.full_path = m_loaded_rom_path.c_str();
    m_game_info_ext.archive_path = nullptr;
    m_game_info_ext.archive_file = nullptr;
    m_game_info_ext.dir = m_content_dir.c_str();
    m_game_info_ext.name = m_content_name.c_str();
    m_game_info_ext.ext = m_content_ext.c_str();
    m_game_info_ext.meta = nullptr;
    m_game_info_ext.data = m_rom_bytes.data();
    m_game_info_ext.size = m_rom_bytes.size();
    m_game_info_ext.file_in_archive = false;
    m_game_info_ext.persistent_data = true;
}

void FceuxBackend::write_rgb565_frame(
    const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
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

void FceuxBackend::write_0rgb1555_frame(
    const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const auto rgba = rgba_from_0rgb1555(row[x]);
            dst[x] = rgba;
            has_visible = has_visible || ((rgba & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible;
}

void FceuxBackend::write_xrgb8888_frame(
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

int16_t FceuxBackend::handle_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port != 0 || index != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        uint32_t mask = 0;
        if (m_input.dpad_up)     mask |= (1u << RETRO_DEVICE_ID_JOYPAD_UP);
        if (m_input.dpad_down)   mask |= (1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
        if (m_input.dpad_left)   mask |= (1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
        if (m_input.dpad_right)  mask |= (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
        if (m_input.button_a)    mask |= (1u << RETRO_DEVICE_ID_JOYPAD_A);
        if (m_input.button_b)    mask |= (1u << RETRO_DEVICE_ID_JOYPAD_B);
        if (m_input.button_x)    mask |= (1u << RETRO_DEVICE_ID_JOYPAD_X);
        if (m_input.button_y)    mask |= (1u << RETRO_DEVICE_ID_JOYPAD_Y);
        if (m_input.button_l)    mask |= (1u << RETRO_DEVICE_ID_JOYPAD_L);
        if (m_input.button_r)    mask |= (1u << RETRO_DEVICE_ID_JOYPAD_R);
        if (m_input.button_start)  mask |= (1u << RETRO_DEVICE_ID_JOYPAD_START);
        if (m_input.button_select) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_SELECT);
        return static_cast<int16_t>(mask);
    }
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     return m_input.dpad_up     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return m_input.dpad_down   ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return m_input.dpad_left   ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return m_input.dpad_right  ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_A:      return m_input.button_a    ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return m_input.button_b    ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_X:      return m_input.button_x    ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_Y:      return m_input.button_y    ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_L:      return m_input.button_l    ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_R:      return m_input.button_r    ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START:  return m_input.button_start  ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return m_input.button_select ? 1 : 0;
    default: return 0;
    }
}

} // namespace qrd
