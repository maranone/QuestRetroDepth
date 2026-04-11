#include "snes_libretro_backend.h"
#include "snes9x_layer_capture.h"

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

// Forward declaration — defined in snes9x_zbuffer_accessor.cpp (compiled
// inside snes9x_libretro where snes9x.h and gfx.h are available).
extern "C" const uint8_t* snes9x_get_zbuffer_for_frame(const void* video_data,
                                                        unsigned*   out_stride);
extern "C" const uint8_t* snes9x_get_visible_source_for_frame(const void* video_data,
                                                               unsigned*   out_stride);

namespace qrd {

namespace {

constexpr const char* kLogTag = "QuestRetroDepth";
constexpr const char* kFrontendDir = ".";
constexpr int kWarmupFrames = 30;

SnesLibretroBackend* g_active_backend = nullptr;

// ---------------------------------------------------------------------------
// AAudio ring buffer — lock-free SPSC (producer = libretro thread,
// consumer = AAudio data callback thread).
// ---------------------------------------------------------------------------
constexpr int kAudioRingFrames = 8192; // stereo int16 frames
static int16_t  g_audio_ring[kAudioRingFrames * 2]; // interleaved L/R
static std::atomic<int> g_ring_write{0};
static std::atomic<int> g_ring_read{0};
static AAudioStream* g_aaudio_stream = nullptr;
static int g_audio_sample_rate = 32000;

static void audio_ring_push(const int16_t* samples, int frames) {
    int w = g_ring_write.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        int next = (w + 1) % kAudioRingFrames;
        if (next == g_ring_read.load(std::memory_order_acquire)) break; // full, drop
        g_audio_ring[w * 2 + 0] = samples[i * 2 + 0];
        g_audio_ring[w * 2 + 1] = samples[i * 2 + 1];
        w = next;
    }
    g_ring_write.store(w, std::memory_order_release);
}

static aaudio_data_callback_result_t audio_data_callback(
    AAudioStream*, void* /*userData*/, void* audioData, int32_t numFrames) {
    auto* out = static_cast<int16_t*>(audioData);
    int r = g_ring_read.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; ++i) {
        int w = g_ring_write.load(std::memory_order_acquire);
        if (r == w) { // underrun — silence
            out[i * 2 + 0] = 0;
            out[i * 2 + 1] = 0;
        } else {
            out[i * 2 + 0] = g_audio_ring[r * 2 + 0];
            out[i * 2 + 1] = g_audio_ring[r * 2 + 1];
            r = (r + 1) % kAudioRingFrames;
        }
    }
    g_ring_read.store(r, std::memory_order_release);
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
    if (g_aaudio_stream) AAudioStream_requestStart(g_aaudio_stream);
}

static void close_aaudio_stream() {
    if (g_aaudio_stream) {
        AAudioStream_requestStop(g_aaudio_stream);
        AAudioStream_close(g_aaudio_stream);
        g_aaudio_stream = nullptr;
    }
}

int android_log_priority(retro_log_level level) {
    switch (level) {
    case RETRO_LOG_DEBUG:
        return ANDROID_LOG_DEBUG;
    case RETRO_LOG_INFO:
        return ANDROID_LOG_INFO;
    case RETRO_LOG_WARN:
        return ANDROID_LOG_WARN;
    case RETRO_LOG_ERROR:
        return ANDROID_LOG_ERROR;
    default:
        return ANDROID_LOG_DEFAULT;
    }
}

void RETRO_CALLCONV frontend_log_printf(retro_log_level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(android_log_priority(level), kLogTag, fmt, args);
    va_end(args);
}

bool RETRO_CALLCONV frontend_environment(unsigned cmd, void* data) {
    return g_active_backend ? g_active_backend->handle_environment(cmd, data) : false;
}

void RETRO_CALLCONV frontend_video_refresh(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (g_active_backend) {
        g_active_backend->handle_video_frame(data, width, height, pitch);
    }
}

void RETRO_CALLCONV frontend_audio_sample(int16_t l, int16_t r) {
    int16_t buf[2] = {l, r};
    audio_ring_push(buf, 1);
}

std::size_t RETRO_CALLCONV frontend_audio_sample_batch(const int16_t* data, std::size_t frames) {
    audio_ring_push(data, (int)frames);
    return frames;
}

void RETRO_CALLCONV frontend_input_poll() {}

int16_t RETRO_CALLCONV frontend_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_active_backend ? g_active_backend->handle_input_state(port, device, index, id) : 0;
}

uint32_t rgba_from_rgb565(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint32_t rgba_from_xrgb8888(uint32_t pixel) {
    return 0xFF000000u | (pixel & 0x00FFFFFFu);
}

} // namespace

SnesLibretroBackend::SnesLibretroBackend()
    : m_pixel_format(RETRO_PIXEL_FORMAT_RGB565) {
    retro_system_info info{};
    retro_get_system_info(&info);

    std::ostringstream name;
    name << "Snes9x libretro";
    if (info.library_name && info.library_name[0] != '\0') {
        name.str("");
        name.clear();
        name << info.library_name;
        if (info.library_version && info.library_version[0] != '\0') {
            name << " " << info.library_version;
        }
        name << " (libretro)";
    }

    m_backend_name = name.str();
    m_frame.layers.resize(SNES9X_LAYER_COUNT);
    ensure_frame_size(256, 224);
}

SnesLibretroBackend::~SnesLibretroBackend() {
    reset_core();
}

const char* SnesLibretroBackend::backend_name() const {
    return m_backend_name.c_str();
}

bool SnesLibretroBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();

    if (rom_path.empty()) {
        error_out = "Snes9x: ROM path is empty.";
        return false;
    }

    if (!load_file_bytes(rom_path, error_out)) {
        return false;
    }

    if (!ensure_core_initialized(error_out)) {
        return false;
    }

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = m_rom_bytes.data();
    game_info.size = m_rom_bytes.size();
    game_info.meta = nullptr;

    if (!retro_load_game(&game_info)) {
        error_out = "Snes9x: retro_load_game failed.";
        reset_core();
        return false;
    }

    retro_system_av_info av_info{};
    retro_get_system_av_info(&av_info);
    update_geometry(av_info.geometry);
    g_audio_sample_rate = (av_info.timing.sample_rate > 0)
        ? (int)av_info.timing.sample_rate : 32000;
    open_aaudio_stream(g_audio_sample_rate);

    m_loaded_rom_path = rom_path;
    m_game_loaded = true;

    EmulatorInputState warmup_input{};
    for (int i = 0; i < kWarmupFrames; ++i) {
        m_input = warmup_input;
        retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) {
            break;
        }
    }

    if (m_video_frame_count == 0) {
        error_out = "Snes9x: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }

    __android_log_print(
        ANDROID_LOG_INFO,
        kLogTag,
        "Loaded ROM '%s' -> %ux%u, videoFrames=%llu, visible=%d",
        rom_path.c_str(),
        static_cast<unsigned>(m_frame.width),
        static_cast<unsigned>(m_frame.height),
        static_cast<unsigned long long>(m_video_frame_count),
        m_last_frame_had_visible_pixels ? 1 : 0);

    error_out.clear();
    return true;
}

bool SnesLibretroBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) {
        error_out = "Snes9x: no ROM loaded.";
        return false;
    }

    m_input = input;
    retro_run();
    error_out.clear();
    return true;
}

void SnesLibretroBackend::set_layer_capture_mask(uint32_t mask) {
    m_layer_capture_mask = mask & 0x1Fu;
    snes9x_set_layer_capture_mask(m_layer_capture_mask);
}

const FrameOutput& SnesLibretroBackend::frame_output() const {
    return m_frame;
}

bool SnesLibretroBackend::handle_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto* cb = static_cast<retro_log_callback*>(data);
        cb->log = frontend_log_printf;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        auto** dir = static_cast<const char**>(data);
        *dir = kFrontendDir;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const auto fmt = *static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_RGB565 && fmt != RETRO_PIXEL_FORMAT_XRGB8888) {
            return false;
        }
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
        if (std::strcmp(var->key, "snes9x_auto_frame_skip") == 0) {
            var->value = m_auto_frame_skip ? "enabled" : "disabled";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
        auto* supported = static_cast<bool*>(data);
        *supported = true;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
        auto* result = static_cast<int*>(data);
        // 3 = enable both video and audio (default)
        *result = 3;
        return true;
    }
    default:
        return false;
    }
}

void SnesLibretroBackend::handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) {
        return;
    }

    ++m_video_frame_count;
    ensure_frame_size(width, height);
    if (m_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
    } else {
        write_rgb565_frame(static_cast<const uint16_t*>(data), width, height, pitch);
    }

    // Copy the snes9x per-pixel z-buffer (depth/priority values 0-63).
    if (!m_frame.zbuffer.empty()) {
        unsigned zstride = 0;
        const uint8_t* zsrc = snes9x_get_zbuffer_for_frame(data, &zstride);
        if (zsrc && zstride > 0) {
            uint8_t* dst = m_frame.zbuffer.data();
            const unsigned copy_w = std::min(width, zstride);
            for (unsigned y = 0; y < height; ++y) {
                std::memcpy(dst + y * width, zsrc + y * zstride, copy_w);
            }

            unsigned vstride = 0;
            const uint8_t* vsrc = snes9x_get_visible_source_for_frame(data, &vstride);
            if (vsrc && vstride > 0 && !m_frame.visible_source_id.empty()) {
                uint8_t* vdst = m_frame.visible_source_id.data();
                const unsigned vcopy_w = std::min(width, vstride);
                for (unsigned y = 0; y < height; ++y) {
                    std::memcpy(vdst + y * width, vsrc + y * vstride, vcopy_w);
                }
            }

            // Copy per-layer captures (RGB565 + mask) → RGBA8888.
            for (int li = 0; li < SNES9X_LAYER_COUNT; ++li) {
                if ((m_layer_capture_mask & (1u << li)) == 0) continue;
                unsigned lstride = 0;
                const uint16_t* lpix  = snes9x_get_layer_pixels(li, &lstride);
                const uint8_t*  lmask = snes9x_get_layer_mask(li, nullptr);
                auto& out = m_frame.layers[li].rgba;
                if (lpix && lmask && lstride > 0 && !out.empty()) {
                    for (unsigned y = 0; y < height; ++y) {
                        for (unsigned x = 0; x < width; ++x) {
                            const uint16_t px   = lpix [y * lstride + x];
                            const uint8_t  opaq = lmask[y * lstride + x];
                            const uint8_t r = static_cast<uint8_t>(((px >> 11) & 0x1F) * 255 / 31);
                            const uint8_t g = static_cast<uint8_t>(((px >>  5) & 0x3F) * 255 / 63);
                            const uint8_t b = static_cast<uint8_t>( (px        & 0x1F) * 255 / 31);
                            const uint8_t a = opaq ? 255u : 0u;
                            out[y * width + x] = (static_cast<uint32_t>(a) << 24) |
                                                 (static_cast<uint32_t>(b) << 16) |
                                                 (static_cast<uint32_t>(g) <<  8) |
                                                  static_cast<uint32_t>(r);
                        }
                    }
                }
            }

            // Log z-value histogram every 120 frames so we can verify
            // which priority bands the game actually populates.
            if (m_video_frame_count % 120 == 0) {
                memset(m_z_histogram, 0, sizeof(m_z_histogram));
                const uint8_t* p = m_frame.zbuffer.data();
                const size_t npix = static_cast<size_t>(width) * height;
                for (size_t i = 0; i < npix; ++i) m_z_histogram[p[i]]++;
                m_histogram_valid = true;
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                    "ZBuf histogram (frame %llu, %ux%u):",
                    static_cast<unsigned long long>(m_video_frame_count), width, height);
                for (int z = 0; z < 256; ++z) {
                    if (m_z_histogram[z] > 0)
                        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "  z=%3d : %u px", z, m_z_histogram[z]);
                }
            }
        }
    }
}

bool SnesLibretroBackend::ensure_core_initialized(std::string& error_out) {
    if (m_core_initialized) {
        return true;
    }

    g_active_backend = this;
    snes9x_set_layer_capture_mask(m_layer_capture_mask);
    retro_set_environment(frontend_environment);
    retro_set_video_refresh(frontend_video_refresh);
    retro_set_audio_sample(frontend_audio_sample);
    retro_set_audio_sample_batch(frontend_audio_sample_batch);
    retro_set_input_poll(frontend_input_poll);
    retro_set_input_state(frontend_input_state);

    retro_init();
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void SnesLibretroBackend::reset_core() {
    close_aaudio_stream();
    if (m_core_initialized) {
        retro_unload_game();
        retro_deinit();
    }

    m_core_initialized = false;
    m_game_loaded = false;
    m_loaded_rom_path.clear();
    m_rom_bytes.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    m_variables_dirty = false;
    if (g_active_backend == this) {
        g_active_backend = nullptr;
    }
}

bool SnesLibretroBackend::load_file_bytes(const std::string& rom_path, std::string& error_out) {
    std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
    if (!file) {
        error_out = "Snes9x: unable to open ROM file.";
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        error_out = "Snes9x: ROM file is empty.";
        return false;
    }

    m_rom_bytes.assign(static_cast<std::size_t>(size), 0);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(m_rom_bytes.data()), size)) {
        error_out = "Snes9x: failed to read ROM file.";
        m_rom_bytes.clear();
        return false;
    }

    return true;
}

void SnesLibretroBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto safe_width = std::max(1u, width);
    const auto safe_height = std::max(1u, height);
    if (m_frame.width == safe_width && m_frame.height == safe_height &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(safe_width) * safe_height) {
        return;
    }

    m_frame.width = safe_width;
    m_frame.height = safe_height;
    const std::size_t npix = static_cast<std::size_t>(safe_width) * safe_height;
    m_frame.rgba8888.assign(npix, 0xFF000000u);
    m_frame.zbuffer.assign(npix, 0u);
    m_frame.visible_source_id.assign(npix, 0xFFu);
    m_frame.layers.resize(SNES9X_LAYER_COUNT);
    for (auto& lc : m_frame.layers)
        lc.rgba.assign(npix, 0u);
}

void SnesLibretroBackend::update_geometry(const retro_game_geometry& geometry) {
    ensure_frame_size(geometry.base_width, geometry.base_height);
}

int16_t SnesLibretroBackend::handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port != 0 || index != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        return static_cast<int16_t>(joypad_mask());
    }

    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:
        return m_input.button_b ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_Y:
        return m_input.button_y ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT:
        return m_input.button_select ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START:
        return m_input.button_start ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_UP:
        return m_input.dpad_up ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:
        return m_input.dpad_down ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:
        return m_input.dpad_left ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:
        return m_input.dpad_right ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_A:
        return m_input.button_a ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_X:
        return m_input.button_x ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_L:
        return m_input.button_l ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_R:
        return m_input.button_r ? 1 : 0;
    default:
        return 0;
    }
}

uint32_t SnesLibretroBackend::joypad_mask() const {
    uint32_t mask = 0;
    if (m_input.button_b) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_B);
    if (m_input.button_y) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_Y);
    if (m_input.button_select) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (m_input.button_start) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_START);
    if (m_input.dpad_up) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_UP);
    if (m_input.dpad_down) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (m_input.dpad_left) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (m_input.dpad_right) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (m_input.button_a) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_A);
    if (m_input.button_x) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_X);
    if (m_input.button_l) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_L);
    if (m_input.button_r) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_R);
    return mask;
}

const uint32_t* SnesLibretroBackend::get_z_histogram() const {
    return m_histogram_valid ? m_z_histogram : nullptr;
}

void SnesLibretroBackend::write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible_pixels = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const auto rgba = rgba_from_rgb565(row[x]);
            dst[x] = rgba;
            has_visible_pixels = has_visible_pixels || ((rgba & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible_pixels;
}

void SnesLibretroBackend::write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible_pixels = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const auto rgba = rgba_from_xrgb8888(row[x]);
            dst[x] = rgba;
            has_visible_pixels = has_visible_pixels || ((rgba & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible_pixels;
}

void SnesLibretroBackend::set_auto_frame_skip(bool enabled) {
    if (m_auto_frame_skip == enabled) return;
    m_auto_frame_skip = enabled;
    m_variables_dirty = true;
}

RomHeaderInfo SnesLibretroBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_rom_bytes.empty()) return info;
    if (m_rom_bytes.size() < 0xFFC0 + 21) return info;

    const char* header = reinterpret_cast<const char*>(m_rom_bytes.data() + 0xFFC0);
    if (header[0] == '\0') return info;

    info.has_header = true;
    info.game_name.assign(header, 21);
    while (!info.game_name.empty() && (info.game_name.back() == ' ' || info.game_name.back() == '\0'))
        info.game_name.pop_back();
    return info;
}

const uint8_t* SnesLibretroBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t SnesLibretroBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

} // namespace qrd
