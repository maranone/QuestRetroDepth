#include "pce_backend.h"
#include "pce_layer_capture.h"
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
void     pce_retro_set_environment(retro_environment_t cb);
void     pce_retro_set_video_refresh(retro_video_refresh_t cb);
void     pce_retro_set_audio_sample(retro_audio_sample_t cb);
void     pce_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void     pce_retro_set_input_poll(retro_input_poll_t cb);
void     pce_retro_set_input_state(retro_input_state_t cb);
unsigned pce_retro_api_version(void);
void     pce_retro_set_controller_port_device(unsigned port, unsigned device);
void     pce_retro_get_system_info(struct retro_system_info* info);
void     pce_retro_get_system_av_info(struct retro_system_av_info* info);
size_t   pce_retro_serialize_size(void);
bool     pce_retro_serialize(void* data, size_t size);
bool     pce_retro_unserialize(const void* data, size_t size);
void     pce_retro_cheat_reset(void);
void     pce_retro_cheat_set(unsigned index, bool enabled, const char* code);
bool     pce_retro_load_game(const struct retro_game_info* info);
bool     pce_retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info);
void     pce_retro_unload_game(void);
unsigned pce_retro_get_region(void);
void*    pce_retro_get_memory_data(unsigned type);
size_t   pce_retro_get_memory_size(unsigned type);
void     pce_retro_reset(void);
void     pce_retro_run(void);
void     pce_retro_init(void);
void     pce_retro_deinit(void);
void     pce_psg_set_channel_volume(int channel, float volume);
}

namespace qrd {

namespace {

constexpr const char* kLogTag       = "QuestRetroDepth";
constexpr int         kWarmupFrames = 12;

std::string g_system_dir_storage = ".";
// PC Engine: 2 capture layers — 0=BG plane, 1=sprites (backdrop transparent in both)
constexpr int kPceLayerCount = 2;

PceBackend* g_active_backend = nullptr;
// See the equivalent comment in snes_libretro_backend.cpp: the core's global
// state is not safe to repeatedly retro_deinit()/retro_init() between ROM
// loads, so only ever initialize it once per process.
bool g_core_ever_initialized = false;

// AAudio ring buffer
constexpr int kAudioRingFrames = 8192;
static int16_t g_audio_ring[kAudioRingFrames * 2];
static std::atomic<int> g_ring_write{0};
static std::atomic<int> g_ring_read{0};
static AAudioStream* g_aaudio_stream = nullptr;
static int g_audio_sample_rate = 44100; // beetle-pce default
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

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base  = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot   = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

} // namespace

void set_pce_system_directory(const std::string& dir) {
    g_system_dir_storage = dir;
}

// ---------------------------------------------------------------------------
// PceBackend
// ---------------------------------------------------------------------------

PceBackend::PceBackend() {
    retro_system_info info{};
    pce_retro_get_system_info(&info);

    std::ostringstream name;
    if (info.library_name && info.library_name[0] != '\0') {
        name << info.library_name;
        if (info.library_version && info.library_version[0] != '\0')
            name << " " << info.library_version;
        name << " (libretro)";
    } else {
        name << "beetle-pce-fast (libretro)";
    }
    m_backend_name = name.str();

    // PC Engine: backdrop, bg_plane, sprites — visible-source extraction
    m_frame.layers.resize(kPceLayerCount);
    ensure_frame_size(512, 243);
}

PceBackend::~PceBackend() {
    reset_core();
}

const char* PceBackend::backend_name() const { return m_backend_name.c_str(); }

double PceBackend::frame_rate_hz() const { return m_frame_rate_hz; }

bool PceBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    m_last_load_warning.clear();
    if (rom_path.empty()) { error_out = "PCE: ROM path is empty."; return false; }
    if (!ensure_core_initialized(error_out)) return false;

    // PCE-CD needs a system card BIOS beetle-pce reads straight from the
    // reported system directory (see set_pce_system_directory()) -- check for
    // it up front so a missing file gets a clear message instead of the core
    // just failing to load or rendering a black screen with no explanation.
    {
        const auto dot = rom_path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : rom_path.substr(dot + 1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == "cue" || ext == "ccd" || ext == "chd" || ext == "iso") {
            static const char* const kCandidates[] = {
                "syscard3.pce", "syscard3u.pce", "syscard2.pce", "syscard2u.pce",
                "syscard1.pce", "gexpress.pce",
            };
            bool found = false;
            for (const char* name : kCandidates) {
                std::ifstream f(g_system_dir_storage + "/" + name, std::ios::binary);
                if (f.good()) { found = true; break; }
            }
            if (!found) {
                m_last_load_warning = "Missing PCE-CD BIOS (syscard3.pce) in\n" +
                                       g_system_dir_storage;
            }
        }
    }

    // Existence/readability check only -- the bytes are deliberately NOT kept.
    // beetle-pce reports need_fullpath = true (libretro.cpp's
    // retro_get_system_info), which per the libretro contract means the core
    // opens the file itself from `path` and the frontend must pass data =
    // nullptr. Handing it a buffer as well made every CD load read the entire
    // image into RAM for nothing -- hundreds of megabytes for a .chd, on a
    // Quest. Saturn and PSX (the other disc backends here) already do this
    // correctly; see saturn_libretro_backend.cpp and psx_libretro_backend.cpp.
    {
        std::ifstream f(rom_path, std::ios::binary | std::ios::ate);
        if (!f) { error_out = "PCE: unable to open ROM file."; return false; }
        if (f.tellg() <= 0) { error_out = "PCE: ROM file is empty."; return false; }
    }
    m_rom_bytes.clear();
    m_rom_bytes.shrink_to_fit();

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = nullptr;
    game_info.size = 0;
    game_info.meta = nullptr;
    if (!pce_retro_load_game(&game_info)) {
        error_out = "PCE: retro_load_game failed.";
        reset_core();
        return false;
    }

    retro_system_av_info av_info{};
    pce_retro_get_system_av_info(&av_info);
    ensure_frame_size(av_info.geometry.base_width, av_info.geometry.base_height);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 59.82;
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 44100;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "PCE AV: fps=%.4f sample_rate=%.1f geometry=%ux%u",
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
        pce_lc_frame_begin();
        pce_retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) break;
    }

    if (m_video_frame_count == 0) {
        error_out = "PCE: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool PceBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) { error_out = "PCE: no ROM loaded."; return false; }
    m_input = input;
    report_audio_buffer_status();
    pce_lc_frame_begin();
    pce_retro_run();
    error_out.clear();
    return true;
}

const FrameOutput& PceBackend::frame_output() const { return m_frame; }

bool PceBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "PCE: no ROM loaded."; return false; }
    const std::size_t size = pce_retro_serialize_size();
    if (size == 0) { error_out = "PCE: zero savestate size."; return false; }
    out.assign(size, 0);
    if (!pce_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "PCE: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool PceBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "PCE: no ROM loaded."; return false; }
    if (!data || size == 0) { error_out = "PCE: savestate data empty."; return false; }
    if (!pce_retro_unserialize(data, size)) {
        error_out = "PCE: retro_unserialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

void PceBackend::set_auto_frame_skip(bool enabled) {
    m_auto_frame_skip = enabled;
}
void PceBackend::set_layer_capture_mask(uint32_t mask) {
    m_layer_capture_mask = mask;
}

void PceBackend::set_pce_channel_volume(int channel, float volume) {
    pce_psg_set_channel_volume(channel, volume);
}

RomHeaderInfo PceBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* PceBackend::get_z_histogram() const { return nullptr; }

const uint8_t* PceBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(pce_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t PceBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return pce_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

bool PceBackend::handle_environment(unsigned cmd, void* data) {
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
        // beetle-pce uses RGB565
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
        auto* var = static_cast<retro_variable*>(data);
        // QuestRetroDepth owns this setting rather than exposing the core's
        // full core-option UI, same as MameBackend's "mame_autoframeskip" /
        // SaturnLibretroBackend's "yabasanshiro_frameskip". "auto" enables
        // the core's real adaptive frameskip (see check_variables() in
        // third_party/beetle-pce/libretro.cpp); anything else (including no
        // match) leaves it at its default-disabled 0.
        if (var && var->key && std::strcmp(var->key, "pce_fast_frameskip") == 0) {
            var->value = m_auto_frame_skip ? "auto" : "disabled";
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
        *static_cast<int*>(data) = 3; // enable both audio and video
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        if (data) {
            const auto& geom = *static_cast<const retro_game_geometry*>(data);
            ensure_frame_size(geom.base_width, geom.base_height);
        }
        return true;
    }
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

void PceBackend::handle_video_frame(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);
    write_rgb565_frame(static_cast<const uint16_t*>(data), width, height, pitch);

    // Copy the per-pixel visible-source IDs captured by the VDC hook
    unsigned vs_w = 0, vs_h = 0;
    const uint8_t* vs = pce_lc_get_visible_source(&vs_w, &vs_h);
    const std::size_t npix = static_cast<std::size_t>(width) * height;
    if (vs && m_frame.visible_source_id.size() == npix) {
        if (vs_w == width && vs_h == height && height == PCE_LC_H) {
            std::memcpy(m_frame.visible_source_id.data(), vs, npix);
        } else {
            if (!pce_lc_copy_visible_source(m_frame.visible_source_id.data(), width, height)) {
                std::fill(m_frame.visible_source_id.begin(), m_frame.visible_source_id.end(), 0xFFu);
            }
        }
    } else if (m_frame.visible_source_id.size() == npix) {
        std::fill(m_frame.visible_source_id.begin(), m_frame.visible_source_id.end(), 0xFFu);
    }

    // Split final RGBA frame into per-layer buffers using visible-source IDs.
    // Layer 0 = BG plane (sid==1), Layer 1 = sprites (sid==2); backdrop transparent.
    m_frame.layers.resize(kPceLayerCount);
    const auto& vsid = m_frame.visible_source_id;
    const auto& src  = m_frame.rgba8888;
    if (vsid.size() == npix && src.size() == npix && (m_layer_capture_mask & 0x3u)) {
        for (int li = 0; li < kPceLayerCount; ++li) {
            if (m_frame.layers[li].rgba.size() != npix)
                m_frame.layers[li].rgba.resize(npix);
        }
        // Sprite layer (li=1, sid==2) gets Y-depth map
        auto& dmap = m_frame.layers[1].depth_map;
        if (dmap.size() != npix) dmap.resize(npix);
        for (unsigned y = 0; y < height; ++y) {
            const uint8_t depth_y = (height > 1u)
                ? static_cast<uint8_t>(y * 255u / (height - 1u)) : 128u;
            for (unsigned x = 0; x < width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * width + x;
                const uint8_t sid = vsid[i];
                m_frame.layers[0].rgba[i] = (sid == 1u) ? src[i] : 0u;
                m_frame.layers[1].rgba[i] = (sid == 2u) ? src[i] : 0u;
                dmap[i] = (sid == 2u) ? depth_y : 0u;
            }
        }
    } else {
        for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    }
}

bool PceBackend::ensure_core_initialized(std::string& error_out) {
    g_active_backend = this;
    pce_retro_set_environment(frontend_environment);
    pce_retro_set_video_refresh(frontend_video_refresh);
    pce_retro_set_audio_sample(frontend_audio_sample);
    pce_retro_set_audio_sample_batch(frontend_audio_sample_batch);
    pce_retro_set_input_poll(frontend_input_poll);
    pce_retro_set_input_state(frontend_input_state);
    if (!g_core_ever_initialized) {
        pce_retro_init();
        g_core_ever_initialized = true;
    }
    pce_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void PceBackend::reset_core() {
    close_aaudio_stream();
    if (m_game_loaded) {
        pce_retro_unload_game();
    }
    m_core_initialized = false;
    m_game_loaded      = false;
    m_loaded_rom_path.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kPceLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    if (g_active_backend == this) g_active_backend = nullptr;
}

void PceBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto w = std::max(1u, width);
    const auto h = std::max(1u, height);
    if (m_frame.width == w && m_frame.height == h &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(w) * h) return;

    m_frame.width  = w;
    m_frame.height = h;
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    m_frame.rgba8888.assign(npix, 0xFF000000u);
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.assign(npix, 0xFFu);
    m_frame.layers.resize(kPceLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
}

void PceBackend::write_rgb565_frame(
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

int16_t PceBackend::handle_input_state(
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
