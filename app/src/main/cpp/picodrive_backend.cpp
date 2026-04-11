#include "picodrive_backend.h"

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
void picodrive_retro_set_environment(retro_environment_t cb);
void picodrive_retro_set_video_refresh(retro_video_refresh_t cb);
void picodrive_retro_set_audio_sample(retro_audio_sample_t cb);
void picodrive_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void picodrive_retro_set_input_poll(retro_input_poll_t cb);
void picodrive_retro_set_input_state(retro_input_state_t cb);
unsigned picodrive_retro_api_version(void);
void picodrive_retro_set_controller_port_device(unsigned port, unsigned device);
void picodrive_retro_get_system_info(struct retro_system_info* info);
void picodrive_retro_get_system_av_info(struct retro_system_av_info* info);
size_t picodrive_retro_serialize_size(void);
bool picodrive_retro_serialize(void* data, size_t size);
bool picodrive_retro_unserialize(const void* data, size_t size);
void picodrive_retro_cheat_reset(void);
void picodrive_retro_cheat_set(unsigned index, bool enabled, const char* code);
bool picodrive_retro_load_game(const struct retro_game_info* info);
bool picodrive_retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info);
void picodrive_retro_unload_game(void);
unsigned picodrive_retro_get_region(void);
void* picodrive_retro_get_memory_data(unsigned type);
size_t picodrive_retro_get_memory_size(unsigned type);
void picodrive_retro_reset(void);
void picodrive_retro_run(void);
void picodrive_retro_init(void);
void picodrive_retro_deinit(void);
void picodrive_rd_set_capture_mask(uint32_t mask);
uint32_t picodrive_rd_get_capture_mask(void);
int picodrive_rd_get_layer_count(void);
const uint32_t* picodrive_rd_get_layer_rgba(int layer, unsigned* out_width, unsigned* out_height);
const uint8_t* picodrive_rd_get_visible_source(unsigned* out_width, unsigned* out_height);
}

namespace qrd {

namespace {

constexpr const char* kLogTag = "QuestRetroDepth";
constexpr const char* kFrontendDir = ".";
constexpr int kWarmupFrames = 12;
constexpr int kGenesisLayerCount = 7;

PicoDriveBackend* g_active_backend = nullptr;

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
    case RETRO_LOG_INFO: return ANDROID_LOG_INFO;
    case RETRO_LOG_WARN: return ANDROID_LOG_WARN;
    case RETRO_LOG_ERROR: return ANDROID_LOG_ERROR;
    default: return ANDROID_LOG_DEFAULT;
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

static int16_t RETRO_CALLCONV frontend_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_active_backend ? g_active_backend->handle_input_state(port, device, index, id) : 0;
}

static uint32_t rgba_from_rgb565(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static uint32_t rgba_from_xrgb8888(uint32_t pixel) {
    return 0xFF000000u | (pixel & 0x00FFFFFFu);
}

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

} // namespace

PicoDriveBackend::PicoDriveBackend()
    : m_pixel_format(RETRO_PIXEL_FORMAT_RGB565) {
    retro_system_info info{};
    picodrive_retro_get_system_info(&info);

    std::ostringstream name;
    name << "PicoDrive libretro";
    if (info.library_name && info.library_name[0] != '\0') {
        name.str("");
        name.clear();
        name << info.library_name;
        if (info.library_version && info.library_version[0] != '\0')
            name << " " << info.library_version;
        name << " (libretro)";
    }
    m_backend_name = name.str();
    m_frame.layers.resize(kGenesisLayerCount);
    ensure_frame_size(320, 240);
}

PicoDriveBackend::~PicoDriveBackend() {
    reset_core();
}

const char* PicoDriveBackend::backend_name() const {
    return m_backend_name.c_str();
}

bool PicoDriveBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    if (rom_path.empty()) {
        error_out = "PicoDrive: ROM path is empty.";
        return false;
    }
    if (!ensure_core_initialized(error_out)) return false;

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = nullptr;
    game_info.size = 0;
    game_info.meta = nullptr;
    if (!picodrive_retro_load_game(&game_info)) {
        error_out = "PicoDrive: retro_load_game failed.";
        reset_core();
        return false;
    }

    retro_system_av_info av_info{};
    picodrive_retro_get_system_av_info(&av_info);
    update_geometry(av_info.geometry);
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 44100;
    open_aaudio_stream(g_audio_sample_rate);

    m_loaded_rom_path = rom_path;
    m_game_loaded = true;

    EmulatorInputState warmup_input{};
    for (int i = 0; i < kWarmupFrames; ++i) {
        m_input = warmup_input;
        report_audio_buffer_status();
        picodrive_retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) break;
    }

    if (m_video_frame_count == 0) {
        error_out = "PicoDrive: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool PicoDriveBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) {
        error_out = "PicoDrive: no ROM loaded.";
        return false;
    }
    m_input = input;
    report_audio_buffer_status();
    picodrive_retro_run();
    error_out.clear();
    return true;
}

const FrameOutput& PicoDriveBackend::frame_output() const {
    return m_frame;
}

bool PicoDriveBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) {
        error_out = "PicoDrive: no ROM loaded.";
        return false;
    }

    const std::size_t size = picodrive_retro_serialize_size();
    if (size == 0) {
        error_out = "PicoDrive: core reported zero savestate size.";
        return false;
    }

    out.assign(size, 0);
    if (!picodrive_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "PicoDrive: retro_serialize failed.";
        return false;
    }

    error_out.clear();
    return true;
}

bool PicoDriveBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) {
        error_out = "PicoDrive: no ROM loaded.";
        return false;
    }
    if (!data || size == 0) {
        error_out = "PicoDrive: savestate data is empty.";
        return false;
    }
    if (!picodrive_retro_unserialize(data, size)) {
        error_out = "PicoDrive: retro_unserialize failed.";
        return false;
    }

    error_out.clear();
    return true;
}

bool PicoDriveBackend::handle_environment(unsigned cmd, void* data) {
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
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
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
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        auto* callback = static_cast<const retro_audio_buffer_status_callback*>(data);
        g_audio_buffer_status_callback = callback ? callback->callback : nullptr;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
        return false;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!data) return false;
        const auto fmt = *static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_RGB565 && fmt != RETRO_PIXEL_FORMAT_XRGB8888) return false;
        m_pixel_format = fmt;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (!data) return false;
        update_geometry(*static_cast<const retro_game_geometry*>(data));
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        if (!data) return false;
        auto* updated = static_cast<bool*>(data);
        *updated = m_variables_dirty;
        m_variables_dirty = false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        if (!data) return false;
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        if (std::strcmp(var->key, "picodrive_input1") == 0) {
            var->value = "6 button pad";
            return true;
        }
        if (std::strcmp(var->key, "picodrive_input2") == 0) {
            var->value = "None";
            return true;
        }
        if (std::strcmp(var->key, "picodrive_frameskip") == 0) {
            var->value = m_auto_frame_skip ? "auto" : "disabled";
            return true;
        }
        if (std::strcmp(var->key, "picodrive_frameskip_threshold") == 0) {
            var->value = "33";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
        if (data) {
            auto* supported = static_cast<bool*>(data);
            *supported = true;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
        if (!data) return false;
        auto* result = static_cast<int*>(data);
        *result = 3;
        return true;
    }
    default:
        return false;
    }
}

void PicoDriveBackend::handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);
    if (m_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
        write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
    else
        write_rgb565_frame(static_cast<const uint16_t*>(data), width, height, pitch);

    unsigned owner_w = 0;
    unsigned owner_h = 0;
    const uint8_t* owner = picodrive_rd_get_visible_source(&owner_w, &owner_h);
    if (owner && owner_w == width && owner_h == height &&
        m_frame.visible_source_id.size() == static_cast<std::size_t>(width) * height) {
        std::memcpy(m_frame.visible_source_id.data(), owner, m_frame.visible_source_id.size());
    }

    update_layer_captures(width, height);
}

bool PicoDriveBackend::ensure_core_initialized(std::string& error_out) {
    if (m_core_initialized) return true;

    g_active_backend = this;
    picodrive_retro_set_environment(frontend_environment);
    picodrive_retro_set_video_refresh(frontend_video_refresh);
    picodrive_retro_set_audio_sample(frontend_audio_sample);
    picodrive_retro_set_audio_sample_batch(frontend_audio_sample_batch);
    picodrive_retro_set_input_poll(frontend_input_poll);
    picodrive_retro_set_input_state(frontend_input_state);
    picodrive_rd_set_capture_mask(m_layer_capture_mask);
    picodrive_retro_init();
    picodrive_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void PicoDriveBackend::reset_core() {
    close_aaudio_stream();
    if (m_core_initialized) {
        picodrive_retro_unload_game();
        picodrive_retro_deinit();
    }

    m_core_initialized = false;
    m_game_loaded = false;
    m_loaded_rom_path.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    m_variables_dirty = false;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kGenesisLayerCount);
    for (auto& layer : m_frame.layers) layer.rgba.clear();
    if (g_active_backend == this) g_active_backend = nullptr;
}

void PicoDriveBackend::ensure_frame_size(unsigned width, unsigned height) {
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
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.assign(npix, 0xFFu);
    m_frame.layers.resize(kGenesisLayerCount);
    for (auto& layer : m_frame.layers) layer.rgba.clear();
}

void PicoDriveBackend::update_geometry(const retro_game_geometry& geometry) {
    ensure_frame_size(geometry.base_width, geometry.base_height);
}

int16_t PicoDriveBackend::handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port != 0 || index != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(joypad_mask());

    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP: return m_input.dpad_up ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return m_input.dpad_down ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT: return m_input.dpad_left ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return m_input.dpad_right ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_A: return m_input.button_a ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B: return m_input.button_b ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_X: return m_input.button_x ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_Y: return m_input.button_y ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_L: return m_input.button_l ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_R: return m_input.button_r ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START: return m_input.button_start ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return m_input.button_select ? 1 : 0;
    default: return 0;
    }
}

uint32_t PicoDriveBackend::joypad_mask() const {
    uint32_t mask = 0;
    if (m_input.dpad_up) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_UP);
    if (m_input.dpad_down) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (m_input.dpad_left) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (m_input.dpad_right) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (m_input.button_a) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_A);
    if (m_input.button_b) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_B);
    if (m_input.button_x) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_X);
    if (m_input.button_y) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_Y);
    if (m_input.button_l) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_L);
    if (m_input.button_r) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_R);
    if (m_input.button_start) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_START);
    if (m_input.button_select) mask |= (1u << RETRO_DEVICE_ID_JOYPAD_SELECT);
    return mask;
}

void PicoDriveBackend::write_rgb565_frame(
    const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
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

void PicoDriveBackend::write_xrgb8888_frame(
    const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
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

void PicoDriveBackend::set_auto_frame_skip(bool enabled) {
    if (m_auto_frame_skip == enabled) return;
    m_auto_frame_skip = enabled;
    m_variables_dirty = true;
}

void PicoDriveBackend::set_layer_capture_mask(uint32_t mask) {
    m_layer_capture_mask = mask & 0x7Fu;
    picodrive_rd_set_capture_mask(m_layer_capture_mask);
}

RomHeaderInfo PicoDriveBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* PicoDriveBackend::get_z_histogram() const {
    return nullptr;
}

const uint8_t* PicoDriveBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(picodrive_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t PicoDriveBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return picodrive_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

void PicoDriveBackend::update_layer_captures(unsigned width, unsigned height) {
    m_frame.layers.resize(kGenesisLayerCount);
    for (auto& layer : m_frame.layers) {
        if (!layer.rgba.empty()) layer.rgba.clear();
    }

    for (int li = 0; li < kGenesisLayerCount; ++li) {
        if ((m_layer_capture_mask & (1u << li)) == 0) continue;
        unsigned cap_w = 0;
        unsigned cap_h = 0;
        const uint32_t* src = picodrive_rd_get_layer_rgba(li, &cap_w, &cap_h);
        if (!src || cap_w != width || cap_h != height) continue;
        const std::size_t npix = static_cast<std::size_t>(width) * height;
        auto& dst = m_frame.layers[li].rgba;
        if (dst.size() != npix) dst.resize(npix);
        std::memcpy(dst.data(), src, npix * sizeof(uint32_t));
    }
}

} // namespace qrd
