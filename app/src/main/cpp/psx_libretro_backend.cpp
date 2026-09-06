#include "psx_libretro_backend.h"
#include "audio_processor.h"
#include "psx_pgxp_capture.h"
#include "psx_gl_context.h"
#include "psx_hw_depth_bridge.h"
#include "psx_gpu_frame.h"
// For the texture-filter option table (VrState::kPsxTextureFilterValues), which
// the menu and the core option string have to agree on.
#include "vr_state.h"

#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <sys/system_properties.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

// SwanStation is compiled into the same shared object as several other
// libretro cores, so CMake prefixes its public entry points with swan_.
extern "C" {
void     swan_retro_set_environment(retro_environment_t cb);
void     swan_retro_set_video_refresh(retro_video_refresh_t cb);
void     swan_retro_set_audio_sample(retro_audio_sample_t cb);
void     swan_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void     swan_retro_set_input_poll(retro_input_poll_t cb);
void     swan_retro_set_input_state(retro_input_state_t cb);
unsigned swan_retro_api_version(void);
void     swan_retro_set_controller_port_device(unsigned port, unsigned device);
void     swan_retro_get_system_info(struct retro_system_info* info);
void     swan_retro_get_system_av_info(struct retro_system_av_info* info);
size_t   swan_retro_serialize_size(void);
bool     swan_retro_serialize(void* data, size_t size);
bool     swan_retro_unserialize(const void* data, size_t size);
void     swan_retro_cheat_reset(void);
void     swan_retro_cheat_set(unsigned index, bool enabled, const char* code);
bool     swan_retro_load_game(const struct retro_game_info* info);
bool     swan_retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info);
void     swan_retro_unload_game(void);
unsigned swan_retro_get_region(void);
void*    swan_retro_get_memory_data(unsigned type);
size_t   swan_retro_get_memory_size(unsigned type);
void     swan_retro_reset(void);
void     swan_retro_run(void);
void     swan_retro_init(void);
void     swan_retro_deinit(void);
}

namespace qrd {

namespace {

constexpr const char* kLogTag = "QuestRetroDepth";
constexpr int kWarmupFrames = 60;
constexpr int kPsxLayerCount = 1;
// Below this much 3D coverage a frame is a menu, an FMV or a 2D game. Those
// get presented flat rather than displaced from a handful of stray polygons.
constexpr std::size_t kMinGeometryCoveragePercent = 8;
// Per-frame adaptation rates for the depth normalisation range, at ~60 Hz.
// These are per-frame fractions, so the settle time is exponential, not the
// time constant: at rate r the range is 1-(1-r)^n converged after n frames.
//   0.02  ~2.5s to 95%  (old default; read as objects drifting then settling)
//   0.1   ~0.5s to 96%
//   0.3   ~0.17s to 97% (current; fast enough not to be noticed)
//   1.0   snaps, at the cost of the depth breathing frame to frame
// Expansion stays near-immediate so new geometry never clamps against a stale
// range — which is why the lag was only ever visible when a scene got
// shallower, never deeper.
constexpr float kDepthRangeExpandRate = 0.50f;
constexpr float kDepthRangeContractRate = 0.3f;

// The range is what depth is normalised against, so while it moves, everything
// already on screen drifts with it — objects ease in depth and then settle.
// Contraction is the visible one: 0.02/frame is roughly 0.8s at 60Hz. Tunable
// with debug.qrd.psxsmooth (1.0 = no smoothing, snap immediately).
float depth_contract_rate() {
    static float rate = kDepthRangeContractRate;
    static int poll = 0;
    if (++poll % 120 == 1) {
        char buf[PROP_VALUE_MAX] = {0};
        rate = kDepthRangeContractRate;
        if (__system_property_get("debug.qrd.psxsmooth", buf) > 0) {
            const float v = (float)atof(buf);
            if (v > 0.0f && v <= 1.0f) rate = v;
        }
    }
    return rate;
}

// RETRO_DEVICE_PS_GUNCON is the libretro GunCon subclass, originally from Beetle PSX.
constexpr unsigned kPsxGuncon = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0);

std::string g_system_dir_storage = ".";
PsxLibretroBackend* g_active_backend = nullptr;
bool g_core_ever_initialized = false;

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
    const int16_t samples[2] = {l, r};
    audio_ring_push(samples, 1);
}

static std::size_t RETRO_CALLCONV frontend_audio_sample_batch(
    const int16_t* data, std::size_t frames) {
    audio_ring_push(data, static_cast<int>(frames));
    return frames;
}

static void RETRO_CALLCONV frontend_input_poll() {}

static int16_t RETRO_CALLCONV frontend_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_active_backend ? g_active_backend->handle_input_state(port, device, index, id) : 0;
}

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

} // namespace

void set_psx_system_directory(const std::string& dir) {
    g_system_dir_storage = dir;
}

PsxLibretroBackend::PsxLibretroBackend() {
    retro_system_info info{};
    swan_retro_get_system_info(&info);
    std::ostringstream name;
    if (info.library_name && info.library_name[0] != '\0') {
        name << info.library_name;
        if (info.library_version && info.library_version[0] != '\0') name << " " << info.library_version;
        name << " (libretro)";
    } else {
        name << "SwanStation (libretro)";
    }
    m_backend_name = name.str();
    m_frame.layers.resize(kPsxLayerCount);
    ensure_frame_size(320, 240);
}

PsxLibretroBackend::~PsxLibretroBackend() {
    reset_core();
}

const char* PsxLibretroBackend::backend_name() const { return m_backend_name.c_str(); }
double PsxLibretroBackend::frame_rate_hz() const { return m_frame_rate_hz; }

bool PsxLibretroBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    m_last_load_warning.clear();
    if (rom_path.empty()) { error_out = "PSX: ROM path is empty."; return false; }
    // The shared GL context must exist before retro_load_game(), which is where
    // the core asks for a hardware renderer. If it cannot be created we simply
    // never advertise one and the core stays on its software renderer.
    // Latched here because SwanStation only accepts a renderer choice at boot.
    m_want_hardware = (m_render_path != 2);
    m_hw_context_ready = false;
    if (m_want_hardware && psx_gl_context_host_available()) {
        std::string gl_err;
        if (psx_gl_context_ensure_current(gl_err)) {
            m_hw_context_ready = true;
        } else {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "PSX HW unavailable, using software renderer: %s", gl_err.c_str());
        }
    }
    if (!ensure_core_initialized(error_out)) {
        // The context was bound just above and the warm-up release below is now
        // unreachable; hand it back or the caller's own context stays displaced.
        psx_gl_context_release();
        return false;
    }
    // Software fallback keeps its own CPU depth rasteriser fed.
    psx_pgxp_capture_set_enabled(!m_hw_context_ready);

    bool has_bios = false;
    for (const char* name : {"scph5500.bin", "scph5501.bin", "scph5502.bin",
                             "PSXONPSP660.bin", "ps1_rom.bin", "openbios.bin"}) {
        std::ifstream f(g_system_dir_storage + "/" + name, std::ios::binary);
        if (f.good()) { has_bios = true; break; }
    }
    if (!has_bios) {
        m_last_load_warning = "No PSX BIOS found in\n" + g_system_dir_storage +
                              "\nSwanStation may use its built-in OpenBIOS.";
    }

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    // SwanStation needs the path for CD images and reads the referenced tracks
    // itself. Passing no data also avoids copying multi-hundred-megabyte discs.
    game_info.data = nullptr;
    game_info.size = 0;
    game_info.meta = nullptr;
    if (!swan_retro_load_game(&game_info)) {
        error_out = "PSX: retro_load_game failed (check the CUE/BIN image and BIOS files).";
        reset_core();
        return false;
    }

    retro_system_av_info av_info{};
    swan_retro_get_system_av_info(&av_info);

    // retro_load_game() requests the hardware context but leaves it to the
    // frontend to signal readiness; until context_reset() runs the core has no
    // hardware display and keeps rendering in software.
    if (m_hw_render_valid && m_hw_context_ready) {
        // The system booted with force_software_renderer set (the core had no
        // hardware callback yet), so this av_info reports *unscaled* geometry.
        // The hardware renderer will draw at max * resolution_scale, so size the
        // target for that now — CreateRenderDevice can ask for the framebuffer
        // during context_reset(), before we get a chance to resize.
        // ...but it does not always: depending on when the core last recomputed
        // its geometry, max_width/height can come back already scaled. Scaling
        // again then over-allocates by scale^2 — at 4x that is three
        // 16384x8192 targets, ~1.6GB of requests, before the resize below
        // shrinks them. The render target can never exceed scaled PS1 VRAM, so
        // clamp to that and the estimate is right either way.
        const int scale = effective_gpu_scale();
        constexpr int kPsxVramWidth = 1024;
        constexpr int kPsxVramHeight = 512;
        const int target_w = std::min((int)av_info.geometry.max_width * scale, kPsxVramWidth * scale);
        const int target_h = std::min((int)av_info.geometry.max_height * scale, kPsxVramHeight * scale);
        if (ensure_hw_framebuffer(target_w, target_h) &&
            m_hw_render.context_reset) {
            m_hw_render.context_reset();
            m_hw_active = true;
            // Geometry changes once the hardware renderer takes over; re-query
            // and grow the target if the core wants more than we predicted.
            swan_retro_get_system_av_info(&av_info);
            if (!ensure_hw_framebuffer((int)av_info.geometry.max_width,
                                       (int)av_info.geometry.max_height)) {
                __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                    "PSX HW: could not size render target to %ux%u; staying on software",
                                    av_info.geometry.max_width, av_info.geometry.max_height);
                if (m_hw_render.context_destroy) m_hw_render.context_destroy();
                m_hw_active = false;
                // Falling back this late means the CPU depth path is the only
                // depth source left, and it needs the capture it was denied.
                psx_pgxp_capture_set_enabled(true);
            } else {
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "PSX HW: hardware renderer active, scale=%d target=%ux%u",
                                    scale, av_info.geometry.max_width, av_info.geometry.max_height);
            }
        } else {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "PSX HW: render target setup failed; staying on software");
            m_hw_active = false;
            psx_pgxp_capture_set_enabled(true);
        }
    }
    ensure_frame_size(av_info.geometry.base_width, av_info.geometry.base_height);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 59.826;
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 44100;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "PSX AV: fps=%.4f sample_rate=%.1f geometry=%ux%u",
                        m_frame_rate_hz, av_info.timing.sample_rate,
                        av_info.geometry.base_width, av_info.geometry.base_height);
    if (!m_preview_mode || m_preview_allow_audio) open_aaudio_stream(g_audio_sample_rate);

    m_loaded_rom_path = rom_path;
    m_game_loaded = true;
    // Warm-up below runs the core on this (JNI) thread, so the context must
    // still be held here; it is released once that finishes.

    EmulatorInputState warmup{};
    for (int i = 0; i < kWarmupFrames; ++i) {
        m_input = warmup;
        report_audio_buffer_status();
        psx_pgxp_capture_reset();
        swan_retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) break;
    }
    // Warm-up is done; the emulation thread owns the core from here.
    psx_gl_context_release();
    if (m_video_frame_count == 0) {
        error_out = "PSX: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }
    error_out.clear();
    return true;
}

bool PsxLibretroBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) { error_out = "PSX: no ROM loaded."; return false; }
    m_input = input;
    report_audio_buffer_status();
    psx_pgxp_capture_reset();
    // The hardware renderer issues GL from inside retro_run(), and this runs on
    // the emulation thread while ROM loading runs on the JNI thread. Bind the
    // shared context here and hand it back afterwards so either thread can take
    // it; g_backend_mutex guarantees they never overlap.
    bool bound = false;
    if (m_hw_active) {
        std::string gl_err;
        bound = psx_gl_context_ensure_current(gl_err);
        if (!bound) {
            error_out = gl_err;
            return false;
        }
    }
    swan_retro_run();
    if (bound) psx_gl_context_release();
    error_out.clear();
    return true;
}

const FrameOutput& PsxLibretroBackend::frame_output() const { return m_frame; }

bool PsxLibretroBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "PSX: no ROM loaded."; return false; }
    const std::size_t size = swan_retro_serialize_size();
    if (size == 0) { error_out = "PSX: zero savestate size."; return false; }
    out.assign(size, 0);
    if (!swan_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "PSX: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool PsxLibretroBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "PSX: no ROM loaded."; return false; }
    if (!data || size == 0) { error_out = "PSX: savestate data empty."; return false; }
    if (!swan_retro_unserialize(data, size)) {
        error_out = "PSX: retro_unserialize failed.";
        return false;
    }
    reset_depth_range();
    error_out.clear();
    return true;
}

void PsxLibretroBackend::set_auto_frame_skip(bool enabled) {
    m_auto_frame_skip = enabled;
}

void PsxLibretroBackend::set_gpu_resolution(int scale) {
    switch (scale) {
        case 1: case 2: case 4:
            break;
        default:
            scale = 4;
            break;
    }
    if (m_gpu_resolution == scale) return;
    m_gpu_resolution = scale;
    // SwanStation polls GET_VARIABLE_UPDATE between frames and performs the
    // necessary GPU/geometry rescale there. The frontend and emulation thread
    // are serialized by g_backend_mutex, so this flag is safe to consume here.
    if (m_core_initialized) m_variables_dirty = true;
}

void PsxLibretroBackend::set_texture_filter(int index) {
    const int clamped =
        (index >= 0 && index < VrState::kPsxTextureFilterCount) ? index : 0;
    if (m_texture_filter == clamped) return;
    m_texture_filter = clamped;
    // Same live path as the resolution scale: the core polls
    // GET_VARIABLE_UPDATE between frames and rebuilds what it needs.
    if (m_core_initialized) m_variables_dirty = true;
}

void PsxLibretroBackend::set_psx_render_path(int path) {
    m_render_path = (path >= 0 && path <= 2) ? path : 0;
}

void PsxLibretroBackend::set_layer_capture_mask(uint32_t /*mask*/) {
    // PSX is exposed as a single composited video layer.
}

void PsxLibretroBackend::set_gun_mode(bool enabled, int /*peripheral*/) {
    m_gun_mode = enabled;
    apply_controller_ports();
}

void PsxLibretroBackend::set_dual_gun_mode(bool enabled) {
    if (m_dual_gun_mode == enabled) return;
    m_dual_gun_mode = enabled;
    apply_controller_ports();
}

void PsxLibretroBackend::apply_controller_ports() {
    if (!m_core_initialized) return;
    swan_retro_set_controller_port_device(0, m_gun_mode ? kPsxGuncon : RETRO_DEVICE_JOYPAD);
    // Two-player gun titles look for a gun in port 1. Leaving it empty (device
    // NONE) rather than a pad is deliberate: a pad in port 1 makes some games
    // offer a second player that has no way to aim.
    swan_retro_set_controller_port_device(
        1, (m_gun_mode && m_dual_gun_mode) ? kPsxGuncon : RETRO_DEVICE_NONE);
}

void PsxLibretroBackend::soft_reset() {
    if (m_core_initialized && m_game_loaded) swan_retro_reset();
    reset_depth_range();
}

RomHeaderInfo PsxLibretroBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* PsxLibretroBackend::get_z_histogram() const { return nullptr; }

const uint8_t* PsxLibretroBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(swan_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t PsxLibretroBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return swan_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

bool PsxLibretroBackend::handle_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        if (!data) return false;
        static_cast<retro_log_callback*>(data)->log = frontend_log_printf;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        if (!data) return false;
        *static_cast<const char**>(data) = g_system_dir_storage.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        // The software build is XRGB8888. Accept the request and normalize it
        // in handle_video_frame below.
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        auto* callback = static_cast<const retro_audio_buffer_status_callback*>(data);
        g_audio_buffer_status_callback = callback ? callback->callback : nullptr;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (!data) return false;
        *static_cast<bool*>(data) = m_variables_dirty;
        m_variables_dirty = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        if (var && var->key && std::strcmp(var->key, "swanstation_CPU_ExecutionMode") == 0) {
            var->value = m_auto_frame_skip ? "execute" : "disabled";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_Renderer") == 0) {
            // The hardware renderer is what carries the PGXP depth buffer; the
            // software renderer has no depth target at all.
            var->value = m_hw_context_ready ? "OpenGL" : "Software";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_ResolutionScale") == 0) {
            switch (effective_gpu_scale()) {
                case 2:  var->value = "2"; break;
                case 4:  var->value = "4"; break;
                case 1:
                default: var->value = "1"; break;
            }
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_TextureFilter") == 0) {
            const int idx = (m_texture_filter >= 0 &&
                             m_texture_filter < VrState::kPsxTextureFilterCount)
                                ? m_texture_filter : 0;
            var->value = VrState::kPsxTextureFilterValues[idx];
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPEnable") == 0) {
            var->value = "true";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPCulling") == 0) {
            var->value = "true";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPTextureCorrection") == 0) {
            var->value = "true";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPColorCorrection") == 0) {
            var->value = "false";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPVertexCache") == 0) {
            var->value = "false";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPCPU") == 0) {
            var->value = "false";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPPreserveProjFP") == 0) {
            var->value = "false";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPTolerance") == 0) {
            var->value = "-1.0";
            return true;
        }
        if (var && var->key && std::strcmp(var->key, "swanstation_GPU_PGXPDepthBuffer") == 0) {
            // The entire point of the hardware path: gpu_hw writes PGXP W into
            // a real depth target, which QRD resolves instead of rasterising.
            var->value = m_hw_context_ready ? "true" : "false";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        // Ask for GLES3 first. The core tries desktop GL before ES otherwise,
        // and every desktop attempt would be refused below anyway.
        if (data) *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES3;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto* cb = static_cast<retro_hw_render_callback*>(data);
        if (!cb) return false;
        // Our shared context is GLES3; refuse anything else so the core falls
        // through its version list to one we can actually satisfy.
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
            cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) return false;
        if (!m_hw_context_ready) return false;
        cb->get_current_framebuffer = &PsxLibretroBackend::hw_get_current_framebuffer;
        cb->get_proc_address = reinterpret_cast<retro_hw_get_proc_address_t>(&eglGetProcAddress);
        m_hw_render = *cb;
        m_hw_render_valid = true;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "PSX HW: accepted GLES3 hardware render context (type=%u)",
                            (unsigned)cb->context_type);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        if (data) *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        if (!data) return false;
        *static_cast<int*>(data) = 3;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (data) {
            const auto& geometry = *static_cast<const retro_game_geometry*>(data);
            ensure_frame_size(geometry.base_width, geometry.base_height);
        }
        return true;
    default:
        return false;
    }
}

void PsxLibretroBackend::handle_video_frame(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        // Hardware path: the finished image is in our FBO, and the core's depth
        // target is published alongside it.
        if (!m_hw_active) return;
        ++m_video_frame_count;
        process_hw_frame(width, height);
        return;
    }
    if (!data) return;
    ++m_video_frame_count;
    ensure_frame_size(width, height);
    write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
    // SwanStation's software renderer owns the colour image; QRD only adds the
    // per-pixel depth that displaces it into stereo. Always on: PSX has no flat
    // presentation mode.
    m_frame.psx_depth = build_depth_frame(width, height);
    m_frame.layers[0].rgba = m_frame.rgba8888;
    m_frame.layers[0].depth_map.clear();
}

namespace {

// Screen-space depth raster. invw holds 1/w (larger = nearer); 0 means the
// pixel was never covered by 3D geometry.
struct DepthRaster {
    int w = 0;
    int h = 0;
    std::vector<float> invw;
    std::vector<uint8_t> pinned;  // screen-plane elements (HUD, text, sprites)
};

// Half-space triangle fill. 1/w interpolates linearly in screen space, so a
// plain barycentric lerp is exactly correct here — no perspective division
// needed. Nearest value wins, which gives correct occlusion for free.
void raster_triangle(DepthRaster& r, const float x[3], const float y[3], const float iw[3]) {
    const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (std::fabs(area) < 1e-6f) return;
    const float inv_area = 1.0f / area;

    int min_x = (int)std::floor(std::min({x[0], x[1], x[2]}));
    int max_x = (int)std::ceil (std::max({x[0], x[1], x[2]}));
    int min_y = (int)std::floor(std::min({y[0], y[1], y[2]}));
    int max_y = (int)std::ceil (std::max({y[0], y[1], y[2]}));
    min_x = std::max(min_x, 0); min_y = std::max(min_y, 0);
    max_x = std::min(max_x, r.w - 1); max_y = std::min(max_y, r.h - 1);
    if (min_x > max_x || min_y > max_y) return;

    for (int py = min_y; py <= max_y; ++py) {
        const float fy = (float)py + 0.5f;
        for (int px = min_x; px <= max_x; ++px) {
            const float fx = (float)px + 0.5f;
            const float w0 = ((x[1] - x[0]) * (fy - y[0]) - (fx - x[0]) * (y[1] - y[0])) * inv_area;
            const float w1 = ((fx - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (fy - y[0])) * inv_area;
            if (w0 < 0.0f || w1 < 0.0f || (w0 + w1) > 1.0f) continue;
            const float b2 = w0, b1 = w1, b0 = 1.0f - w0 - w1;
            const float value = iw[0] * b0 + iw[1] * b1 + iw[2] * b2;
            float& slot = r.invw[(std::size_t)py * r.w + px];
            if (value > slot) slot = value;
        }
    }
}

} // namespace

std::shared_ptr<const PsxDepthFrame> PsxLibretroBackend::build_depth_frame(
    unsigned width, unsigned height) {
    std::vector<PsxPgxpCapturePrimitive> captured;
    PsxPgxpCaptureDisplay display{};
    psx_pgxp_capture_take(captured, display);
    if (width == 0 || height == 0) return nullptr;

    // A frame with no usable depth still returns a frame, flagged flat. Falling
    // back to null here would hand the frame to the layered renderer instead,
    // and a game crossing the coverage threshold would visibly pop between two
    // renderers with different colour handling every few frames.
    auto flat = [&]() {
        auto f = std::make_shared<PsxDepthFrame>();
        f->width = width;
        f->height = height;
        f->has_geometry = false;
        return f;
    };
    // Without a scanout rectangle the VRAM draw coordinates cannot be mapped
    // onto the video frame, and guessing puts the depth map out of register
    // with the picture. Better to present flat than mis-registered.
    if (captured.empty() || !display.valid) return flat();

    DepthRaster raster;
    raster.w = (int)width;
    raster.h = (int)height;
    raster.invw.assign((std::size_t)width * height, 0.0f);
    raster.pinned.assign((std::size_t)width * height, 0);

    // Draw coordinates are VRAM coordinates; shift them into display space.
    const float origin_x = (float)display.vram_left;
    const float origin_y = (float)display.vram_top;

    for (const PsxPgxpCapturePrimitive& prim : captured) {
        // Semi-transparent primitives are effects layered over solid geometry.
        // Writing them into depth punches holes in the surfaces beneath.
        if (prim.semi_transparent) continue;

        if (prim.kind == PsxPgxpCaptureKind::Sprite) {
            // 2D screen elements: pin to the screen plane so HUD and text stay
            // readable and flat rather than riding the geometry behind them.
            const float sx0 = prim.vertices[0].x - origin_x;
            const float sy0 = prim.vertices[0].y - origin_y;
            const float sx1 = prim.vertices[3].x - origin_x;
            const float sy1 = prim.vertices[3].y - origin_y;
            const int x0 = std::max(0, (int)std::floor(std::min(sx0, sx1)));
            const int x1 = std::min(raster.w - 1, (int)std::ceil(std::max(sx0, sx1)));
            const int y0 = std::max(0, (int)std::floor(std::min(sy0, sy1)));
            const int y1 = std::min(raster.h - 1, (int)std::ceil(std::max(sy0, sy1)));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    raster.pinned[(std::size_t)y * raster.w + x] = 1;
            continue;
        }

        // Lines and any primitive PGXP could not resolve carry no usable depth.
        if (prim.kind == PsxPgxpCaptureKind::Line) continue;
        if (prim.vertex_count < 3) continue;
        bool all_valid = true;
        for (unsigned i = 0; i < prim.vertex_count; ++i)
            if (!prim.vertices[i].valid_w || !(prim.vertices[i].w > 0.0f) ||
                !std::isfinite(prim.vertices[i].w)) { all_valid = false; break; }
        if (!all_valid) continue;

        float vx[4], vy[4], viw[4];
        bool sane = true;
        for (unsigned i = 0; i < prim.vertex_count; ++i) {
            vx[i] = prim.vertices[i].x - origin_x;
            vy[i] = prim.vertices[i].y - origin_y;
            viw[i] = 1.0f / prim.vertices[i].w;
            if (!std::isfinite(vx[i]) || !std::isfinite(vy[i]) || !std::isfinite(viw[i]))
                { sane = false; break; }
        }
        if (!sane) continue;

        {
            const float tx[3] = {vx[0], vx[1], vx[2]};
            const float ty[3] = {vy[0], vy[1], vy[2]};
            const float tw[3] = {viw[0], viw[1], viw[2]};
            raster_triangle(raster, tx, ty, tw);
        }
        if (prim.vertex_count == 4) {
            // PSX quads are two triangles sharing the v1/v2 edge.
            const float tx[3] = {vx[1], vx[2], vx[3]};
            const float ty[3] = {vy[1], vy[2], vy[3]};
            const float tw[3] = {viw[1], viw[2], viw[3]};
            raster_triangle(raster, tx, ty, tw);
        }
    }

    // Robust range from the covered pixels. Percentiles rather than min/max:
    // a single near-clipped polygon would otherwise flatten the whole frame.
    std::vector<float> covered;
    covered.reserve(raster.invw.size() / 4);
    for (float v : raster.invw)
        if (v > 0.0f) covered.push_back(v);

    const std::size_t total = (std::size_t)width * height;
    if (covered.size() * 100 < total * kMinGeometryCoveragePercent) return flat();

    const std::size_t lo_index = covered.size() * 2 / 100;
    const std::size_t hi_index = covered.size() - 1 - (covered.size() * 2 / 100);
    std::nth_element(covered.begin(), covered.begin() + lo_index, covered.end());
    const float raw_lo = covered[lo_index];
    std::nth_element(covered.begin(), covered.begin() + hi_index, covered.end());
    const float raw_hi = covered[hi_index];
    if (!(raw_hi > raw_lo)) return flat();

    // Temporal smoothing of the normalisation range. Taking the raw percentiles
    // straight from each frame makes the entire scene's depth rescale as the
    // camera moves, so the picture visibly pulses in and out. Expand quickly so
    // newly-visible near/far geometry is not clamped flat against a stale range;
    // contract slowly, which is what actually removes the breathing.
    if (!m_depth_range_valid) {
        m_depth_range_lo = raw_lo;
        m_depth_range_hi = raw_hi;
        m_depth_range_valid = true;
    } else if (raw_lo > m_depth_range_hi || raw_hi < m_depth_range_lo) {
        // Ranges do not overlap at all: a scene cut (menu to gameplay, FMV, a
        // warp). Easing across that would leave the depth wrong for about a
        // second, so snap instead.
        m_depth_range_lo = raw_lo;
        m_depth_range_hi = raw_hi;
    } else {
        m_depth_range_lo += (raw_lo - m_depth_range_lo) *
            (raw_lo < m_depth_range_lo ? kDepthRangeExpandRate : depth_contract_rate());
        m_depth_range_hi += (raw_hi - m_depth_range_hi) *
            (raw_hi > m_depth_range_hi ? kDepthRangeExpandRate : depth_contract_rate());
    }

    const float lo = m_depth_range_lo;
    const float hi = m_depth_range_hi;
    if (!(hi > lo)) return flat();
    const float inv_span = 1.0f / (hi - lo);

    auto frame = std::make_shared<PsxDepthFrame>();
    frame->width = width;
    frame->height = height;
    frame->has_geometry = true;
    frame->depth.assign(total, 255);
    for (std::size_t i = 0; i < total; ++i) {
        if (raster.pinned[i]) { frame->depth[i] = 0; continue; }
        const float v = raster.invw[i];
        if (v <= 0.0f) continue;  // uncovered: background, sits furthest back
        const float n = std::clamp((v - lo) * inv_span, 0.0f, 1.0f);
        frame->depth[i] = static_cast<uint8_t>((1.0f - n) * 255.0f + 0.5f);
    }
    return frame;
}

void PsxLibretroBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto w = std::max(1u, width);
    const auto h = std::max(1u, height);
    const int wanted_layers = kPsxLayerCount;
    if (m_frame.width == w && m_frame.height == h &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(w) * h &&
        (int)m_frame.layers.size() == wanted_layers) return;
    m_frame.width = w;
    m_frame.height = h;
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    m_frame.rgba8888.assign(npix, 0xFF000000u);
    m_frame.zbuffer.clear();
    m_frame.psx_depth.reset();
    m_frame.depth_map.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(wanted_layers);
    for (auto& layer : m_frame.layers) {
        layer.rgba.clear();
        layer.depth_map.clear();
    }
}

void PsxLibretroBackend::write_xrgb8888_frame(
    const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const uint32_t pixel = row[x];
            dst[x] = 0xFF000000u | (pixel & 0x00FFFFFFu);
            has_visible = has_visible || ((pixel & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible;
}

int16_t PsxLibretroBackend::handle_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (index != 0) return 0;
    // Port 1 is the second player's gun when dual-gun mode is on. It answers
    // only for the lightgun device — a pad in port 1 is not something QRD
    // drives — and reports nothing unless that gun is actually active, so a
    // two-player game does not see a phantom second player.
    if (port == 1) {
        if (device != RETRO_DEVICE_LIGHTGUN || !m_dual_gun_mode || !m_input.gun2_active) return 0;
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return m_input.gun2_screen_x;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return m_input.gun2_screen_y;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return m_input.gun2_offscreen ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return m_input.gun2_trigger ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD: return m_input.gun2_reload ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return m_input.gun2_button_a ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return m_input.gun2_button_b ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_START: return m_input.gun2_button_start ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SELECT: return m_input.gun2_button_select ? 1 : 0;
        default: return 0;
        }
    }
    if (port != 0) return 0;
    if (device == RETRO_DEVICE_LIGHTGUN) {
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return m_input.gun_active ? m_input.gun_screen_x : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return m_input.gun_active ? m_input.gun_screen_y : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return (m_input.gun_active && m_input.gun_offscreen) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return (m_input.gun_active && m_input.gun_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD: return (m_input.gun_active && m_input.gun_reload) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return m_input.button_a ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return m_input.button_b ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_START: return m_input.button_start ? 1 : 0;
        default: return 0;
        }
    }
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     return m_input.dpad_up ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return m_input.dpad_down ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return m_input.dpad_left ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return m_input.dpad_right ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return m_input.button_b ? 1 : 0; // Cross
    case RETRO_DEVICE_ID_JOYPAD_A:      return m_input.button_a ? 1 : 0; // Circle
    case RETRO_DEVICE_ID_JOYPAD_Y:      return m_input.button_y ? 1 : 0; // Triangle
    case RETRO_DEVICE_ID_JOYPAD_X:      return m_input.button_x ? 1 : 0; // Square
    case RETRO_DEVICE_ID_JOYPAD_L:      return m_input.button_l ? 1 : 0; // L1
    case RETRO_DEVICE_ID_JOYPAD_R:      return m_input.button_r ? 1 : 0; // R1
    case RETRO_DEVICE_ID_JOYPAD_L2:     return m_input.button_c ? 1 : 0; // L2
    case RETRO_DEVICE_ID_JOYPAD_R2:     return m_input.button_z ? 1 : 0; // R2
    case RETRO_DEVICE_ID_JOYPAD_START:  return m_input.button_start ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return m_input.button_select ? 1 : 0;
    default: return 0;
    }
}

bool PsxLibretroBackend::ensure_core_initialized(std::string& error_out) {
    g_active_backend = this;
    // Native mode presents the completed SwanStation framebuffer. Keep the
    // unfinished PGXP replay capture disabled until it reproduces the full
    // GPU/VRAM command stream without dropping geometry.
    psx_pgxp_capture_set_enabled(false);
    psx_pgxp_capture_reset();
    swan_retro_set_environment(frontend_environment);
    swan_retro_set_video_refresh(frontend_video_refresh);
    swan_retro_set_audio_sample(frontend_audio_sample);
    swan_retro_set_audio_sample_batch(frontend_audio_sample_batch);
    swan_retro_set_input_poll(frontend_input_poll);
    swan_retro_set_input_state(frontend_input_state);
    if (!g_core_ever_initialized) {
        swan_retro_init();
        g_core_ever_initialized = true;
    }
    m_core_initialized = true;
    apply_controller_ports();
    error_out.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Hardware renderer path
// ---------------------------------------------------------------------------

namespace {

// Resolves the core's VRAM-space depth texture down to the display rectangle.
// The values need no reconstruction: PGXP normalises W to 0..1 in
// GetPreciseVertex(), and gpu_hw's vertex shader writes it straight to pos_z,
// so sampling the depth buffer returns far-ness directly.
const char* kDepthResolveVS = R"GLSL(#version 300 es
out vec2 vUV;
void main() {
    // Fullscreen triangle from gl_VertexID; no vertex buffer needed.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* kDepthResolveFS = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D uDepth;
uniform vec4 uRect;      // xy = display origin, zw = display size (texels)
uniform vec2 uTexSize;
uniform vec2 uRange;     // percentile lo/hi of this scene's depth
uniform int uNormalize;  // 0 = raw passthrough (for range estimation)
// Depth for pixels the core drew but PGXP could not resolve — HUD, text, 2D
// sprites, billboarded effects. Set to the renderer's pivot so they land on the
// screen plane, where screen-space elements belong.
uniform float uFlatValue;
// Depth for pixels nothing was ever drawn into. Distinct from the above: the
// background belongs at the far plane, not on the screen plane with the HUD.
uniform float uBackgroundValue;
in vec2 vUV;
out vec4 fragColor;
void main() {
    // vUV.y = 0 is the top of the emulator image. The core's VRAM texture is
    // stored bottom-up, so flip when mapping into the display sub-rectangle.
    float px = uRect.x + vUV.x * uRect.z;
    float py = uRect.y + (1.0 - vUV.y) * uRect.w;
    vec2 uv = vec2(px, uTexSize.y - py) / uTexSize;
    float d = texture(uDepth, uv).r;
    float o;
    if (uNormalize != 0) {
        // 1.0 is the cleared value, left by HUD, text and any batch PGXP could
        // not resolve. Those are screen-space elements, so pin them to the
        // screen plane rather than letting them fall to the far plane.
        // Three cases, separated by the sentinel gpu_hw writes for !valid_w:
        //   >= 0.99999  nothing drawn here at all -> background
        //   >= 0.9998   drawn, but PGXP could not resolve it -> screen plane
        //   otherwise   a real PGXP depth -> normalise it
        o = (d >= 0.99999)  ? uBackgroundValue
          : (d >= 0.9998)   ? uFlatValue
                            : clamp((d - uRange.x) / max(uRange.y - uRange.x, 1e-6), 0.0, 1.0);
    } else {
        o = d;
    }
    fragColor = vec4(o, 0.0, 0.0, 1.0);
}
)GLSL";

// Depth range is estimated from a heavily downsampled copy. Percentiles over a
// few thousand samples are as good as over a million for this, and it keeps the
// readback small enough to be free.
constexpr int kDepthRangeSampleW = 64;
constexpr int kDepthRangeSampleH = 48;

GLuint compile_gl_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "PSX depth resolve shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool make_color_target(GLuint& fbo, GLuint& tex, int width, int height) {
    if (!fbo) glGenFramebuffers(1, &fbo);
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return status == GL_FRAMEBUFFER_COMPLETE;
}

} // namespace

int PsxLibretroBackend::effective_gpu_scale() const {
    const int requested = (m_gpu_resolution == 1 || m_gpu_resolution == 2 || m_gpu_resolution == 4)
                              ? m_gpu_resolution : 1;
    if (!m_hw_context_ready) return requested;
    return std::min(requested, kMaxHardwareGpuScale);
}

uintptr_t PsxLibretroBackend::hw_get_current_framebuffer() {
    if (!g_active_backend) return 0;
    PsxLibretroBackend* self = g_active_backend;
    // Rotate only once per emulated frame. The core is free to ask for the
    // framebuffer more than once, and switching targets mid-frame would split
    // one image across two slots.
    if (!self->m_hw_slot_pending) {
        const int slot = psx_gpu_frame_acquire_slot();
        if (slot >= 0) self->m_hw_current_slot = slot;
        self->m_hw_slot_pending = true;
    }
    return self->m_hw_slots[self->m_hw_current_slot].fbo;
}

bool PsxLibretroBackend::ensure_hw_framebuffer(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (m_hw_fbo_w == width && m_hw_fbo_h == height && m_hw_slots[0].fbo) return true;

    for (auto& slot : m_hw_slots) {
        if (!make_color_target(slot.fbo, slot.color, width, height)) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "PSX HW: slot framebuffer incomplete at %dx%d", width, height);
            return false;
        }
        // gpu_hw composites into its own targets and only blits the finished
        // image here, but the core still asks for depth/stencil in its context
        // request, so honour it rather than hand back an incomplete FBO.
        if (m_hw_render.depth || m_hw_render.stencil) {
            if (!slot.depth_rb) glGenRenderbuffers(1, &slot.depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, slot.depth_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, slot.depth_rb);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) return false;
        }
    }
    m_hw_fbo_w = width;
    m_hw_fbo_h = height;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "PSX HW: %d render slots at %dx%d", kPsxGpuFrameSlots, width, height);
    return true;
}

bool PsxLibretroBackend::init_depth_resolve() {
    if (m_resolve_program) return true;
    GLuint vs = compile_gl_shader(GL_VERTEX_SHADER, kDepthResolveVS);
    if (!vs) return false;
    GLuint fs = compile_gl_shader(GL_FRAGMENT_SHADER, kDepthResolveFS);
    if (!fs) { glDeleteShader(vs); return false; }
    m_resolve_program = glCreateProgram();
    glAttachShader(m_resolve_program, vs);
    glAttachShader(m_resolve_program, fs);
    glLinkProgram(m_resolve_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(m_resolve_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(m_resolve_program);
        m_resolve_program = 0;
        return false;
    }
    m_resolve_u_depth = glGetUniformLocation(m_resolve_program, "uDepth");
    m_resolve_u_rect = glGetUniformLocation(m_resolve_program, "uRect");
    m_resolve_u_tex_size = glGetUniformLocation(m_resolve_program, "uTexSize");
    m_resolve_u_range = glGetUniformLocation(m_resolve_program, "uRange");
    m_resolve_u_normalize = glGetUniformLocation(m_resolve_program, "uNormalize");
    m_resolve_u_flat_value = glGetUniformLocation(m_resolve_program, "uFlatValue");
    m_resolve_u_background_value = glGetUniformLocation(m_resolve_program, "uBackgroundValue");
    glGenVertexArrays(1, &m_resolve_vao);
    return true;
}

void PsxLibretroBackend::run_depth_resolve(const PsxHwDepthInfo& info, GLuint target_fbo,
                                           int target_w, int target_h, bool normalize) {
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, target_w, target_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(m_resolve_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, info.depth_texture);
    // The core leaves compare mode set for its own sampling; a depth texture
    // read through a plain sampler2D must have it off or the fetch returns a
    // comparison result instead of the stored value.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(m_resolve_u_depth, 0);
    glUniform4f(m_resolve_u_rect, (float)info.display_x, (float)info.display_y,
                (float)info.display_width, (float)info.display_height);
    glUniform2f(m_resolve_u_tex_size, (float)info.texture_width, (float)info.texture_height);
    glUniform2f(m_resolve_u_range, m_depth_range_lo, m_depth_range_hi);
    glUniform1i(m_resolve_u_normalize, normalize ? 1 : 0);
    {
        // Must match the renderer's pivot: unresolved pixels are written at
        // this value so that (far_ness - pivot) puts them on the screen plane.
        static float flat_value = kPsxDepthPivotDefault;
        static float background_value = 1.0f;
        static int flat_poll = 0;
        if (++flat_poll % 120 == 1) {
            char buf[PROP_VALUE_MAX] = {0};
            flat_value = kPsxDepthPivotDefault;
            if (__system_property_get("debug.qrd.psxpivot", buf) > 0) {
                const float v = (float)atof(buf);
                if (v >= 0.0f && v <= 1.0f) flat_value = v;
            }
            buf[0] = '\0';
            background_value = 1.0f;
            if (__system_property_get("debug.qrd.psxbg", buf) > 0) {
                const float v = (float)atof(buf);
                if (v >= 0.0f && v <= 1.0f) background_value = v;
            }
        }
        glUniform1f(m_resolve_u_flat_value, flat_value);
        glUniform1f(m_resolve_u_background_value, background_value);
    }
    glBindVertexArray(m_resolve_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool PsxLibretroBackend::ensure_readback_resources(int native_w, int native_h) {
    if (native_w <= 0 || native_h <= 0) return false;
    if (m_small_w != native_w || m_small_h != native_h) {
        if (!make_color_target(m_small_fbo, m_small_tex, native_w, native_h)) return false;
        m_small_w = native_w;
        m_small_h = native_h;
        m_pbo_primed = false;
    }
    if (!m_range_fbo &&
        !make_color_target(m_range_fbo, m_range_tex, kDepthRangeSampleW, kDepthRangeSampleH))
        return false;

    const std::size_t color_bytes = (std::size_t)native_w * native_h * 4;
    const std::size_t range_bytes = (std::size_t)kDepthRangeSampleW * kDepthRangeSampleH * 4;
    if (!m_pbo_color[0]) glGenBuffers(2, m_pbo_color);
    if (!m_pbo_range[0]) glGenBuffers(2, m_pbo_range);
    if (!m_pbo_depth[0]) glGenBuffers(2, m_pbo_depth);
    if (m_pbo_depth_bytes != color_bytes) {
        for (GLuint pbo : m_pbo_depth) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)color_bytes, nullptr, GL_STREAM_READ);
        }
        m_pbo_depth_bytes = color_bytes;
        m_pbo_primed = false;
    }
    if (m_pbo_color_bytes != color_bytes) {
        for (GLuint pbo : m_pbo_color) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)color_bytes, nullptr, GL_STREAM_READ);
        }
        m_pbo_color_bytes = color_bytes;
        m_pbo_primed = false;
    }
    if (m_pbo_range_bytes != range_bytes) {
        for (GLuint pbo : m_pbo_range) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)range_bytes, nullptr, GL_STREAM_READ);
        }
        m_pbo_range_bytes = range_bytes;
        m_pbo_primed = false;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

void PsxLibretroBackend::consume_color_readback(const uint8_t* pixels) {
    // Bottom-up (the core requests bottom_left_origin) and BGRA-order relative
    // to QRD's packed 0xAABBGGRR words.
    bool has_visible = false;
    for (int y = 0; y < m_small_h; ++y) {
        const uint8_t* src = pixels + (std::size_t)(m_small_h - 1 - y) * m_small_w * 4;
        uint32_t* dst = m_frame.rgba8888.data() + (std::size_t)y * m_small_w;
        for (int x = 0; x < m_small_w; ++x) {
            const uint8_t r = src[x * 4 + 0], g = src[x * 4 + 1], b = src[x * 4 + 2];
            dst[x] = 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
            if (!has_visible && (r | g | b) != 0) has_visible = true;
        }
    }
    m_last_frame_had_visible_pixels = has_visible;
}

bool PsxLibretroBackend::consume_range_readback(const uint8_t* pixels) {
    std::vector<uint8_t> covered;
    covered.reserve(kDepthRangeSampleW * kDepthRangeSampleH);
    uint8_t seen_min = 255, seen_max = 0;
    for (int i = 0; i < kDepthRangeSampleW * kDepthRangeSampleH; ++i) {
        const uint8_t v = pixels[i * 4];
        if (v < seen_min) seen_min = v;
        if (v > seen_max) seen_max = v;
        if (v < 255) covered.push_back(v);  // 255 = cleared, i.e. no geometry
    }
    const std::size_t total = kDepthRangeSampleW * kDepthRangeSampleH;
    {
        // Distinguishes "the resolve sampled nothing" (all 255) from "it read
        // real values but the coverage gate rejected them".
        static int range_dbg = 0;
        if (++range_dbg % 120 == 1) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                "PSX RANGE: covered=%zu/%zu min=%u max=%u",
                covered.size(), total, (unsigned)seen_min, (unsigned)seen_max);
        }
    }
    if (covered.size() * 100 < total * kMinGeometryCoveragePercent) return false;

    const std::size_t lo_index = covered.size() * 2 / 100;
    const std::size_t hi_index = covered.size() - 1 - (covered.size() * 2 / 100);
    std::nth_element(covered.begin(), covered.begin() + lo_index, covered.end());
    const float raw_lo = (float)covered[lo_index] / 255.0f;
    std::nth_element(covered.begin(), covered.begin() + hi_index, covered.end());
    const float raw_hi = (float)covered[hi_index] / 255.0f;
    if (!(raw_hi > raw_lo)) return false;

    if (!m_depth_range_valid) {
        m_depth_range_lo = raw_lo;
        m_depth_range_hi = raw_hi;
        m_depth_range_valid = true;
    } else if (raw_lo > m_depth_range_hi || raw_hi < m_depth_range_lo) {
        m_depth_range_lo = raw_lo;
        m_depth_range_hi = raw_hi;
    } else {
        m_depth_range_lo += (raw_lo - m_depth_range_lo) *
            (raw_lo < m_depth_range_lo ? kDepthRangeExpandRate : depth_contract_rate());
        m_depth_range_hi += (raw_hi - m_depth_range_hi) *
            (raw_hi > m_depth_range_hi ? kDepthRangeExpandRate : depth_contract_rate());
    }
    return m_depth_range_hi > m_depth_range_lo;
}

void PsxLibretroBackend::consume_depth_readback(const uint8_t* pixels, int w, int h) {
    m_depth_bytes.resize((std::size_t)w * h);
    // Bottom-up, like every other readback here.
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = pixels + (std::size_t)(h - 1 - y) * w * 4;
        uint8_t* dst = m_depth_bytes.data() + (std::size_t)y * w;
        for (int x = 0; x < w; ++x) dst[x] = src[x * 4];
    }
}

void PsxLibretroBackend::process_hw_frame(unsigned width, unsigned height) {
    const int scale = std::max(1, effective_gpu_scale());
    const int native_w = std::max(1, (int)width / scale);
    const int native_h = std::max(1, (int)height / scale);
    const bool zero_copy = (m_render_path == 0);

    // The slots are sized at load, but the resolution setting can change while
    // a game is running: SwanStation rescales between frames, and the core then
    // renders larger than the target we handed it. Grow the slots so the next
    // frame has somewhere valid to land, and drop any published frame first —
    // make_color_target reallocates storage behind texture ids the renderer may
    // still be holding.
    if ((int)width > m_hw_fbo_w || (int)height > m_hw_fbo_h) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "PSX HW: core grew to %ux%u, resizing slots from %dx%d",
                            width, height, m_hw_fbo_w, m_hw_fbo_h);
        psx_gpu_frame_reset();
        for (auto& entry : m_depth_entries) entry.valid = false;
        ensure_hw_framebuffer(std::max((int)width, m_hw_fbo_w),
                              std::max((int)height, m_hw_fbo_h));
        // This frame was already drawn into the undersized target; skip it
        // rather than publish a clipped image.
        return;
    }

    HwSlot& slot = m_hw_slots[m_hw_current_slot];
    const PsxHwDepthInfo info = psx_hw_depth_take();
    const bool want_depth = info.valid && info.pgxp_depth && init_depth_resolve();

    if (!ensure_readback_resources(native_w, native_h)) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "PSX HW: readback resources unavailable");
        return;
    }
    ensure_frame_size((unsigned)native_w, (unsigned)native_h);

    // --- depth ---------------------------------------------------------------
    // The depth buffer holds the region the core just *drew*, not the one it is
    // scanning out: games double-buffer, and ClearDepthBuffer() wipes the whole
    // texture when drawing moves to the other buffer. Resolving the displayed
    // rectangle therefore reads nothing but the clear value. Resolve the drawn
    // rectangle instead and hold it until that buffer is the one on screen.
    // The drawing area is the whole back buffer and can be taller than the
    // window actually scanned out of it (Time Crisis draws 240 rows and
    // displays 224). Resolve the *display* window's shape anchored to the drawn
    // buffer's origin: that is the region this buffer will scan out next flip,
    // so the depth lines up pixel-for-pixel with the colour it ships with.
    // The scanned-out window is inset within its buffer: Time Crisis uses
    // 240-row buffers showing 224 rows, so the display sits 8 rows down from
    // the buffer origin. Anchoring the resolve at the buffer origin rather than
    // at that inset shifts the depth against the colour by the overscan.
    //
    // Buffers are not always stacked vertically. Crypt Killer places them side
    // by side (drawing at x=1280 while displaying x=0), so the same inset has
    // to be derived per axis — assuming a shared column silently disqualified
    // every horizontally-buffered game and left it with no depth at all.
    const auto axis_inset = [](int display_start, int draw_start, int draw_extent,
                               int display_extent) -> int {
        if (draw_extent <= 0) return 0;
        const int inset = ((display_start - draw_start) % draw_extent + draw_extent) % draw_extent;
        return (inset + display_extent <= draw_extent) ? inset : 0;
    };
    const int display_inset_x =
        axis_inset(info.display_x, info.draw_x, info.draw_width, info.display_width);
    const int display_inset_y =
        axis_inset(info.display_y, info.draw_y, info.draw_height, info.display_height);

    PsxHwDepthInfo draw_info = info;
    draw_info.display_x = info.draw_x + display_inset_x;
    draw_info.display_y = info.draw_y + display_inset_y;
    draw_info.display_width = info.display_width;
    draw_info.display_height = info.display_height;

    // Only require both rectangles to be real. The display window is not always
    // contained in the drawing area — Point Blank clips a few pixels in, giving
    // a 1148x892 drawing area for a 1280x896 window — and demanding containment
    // rejected the whole frame over a 4-pixel difference. Overrunning is
    // harmless: the excess reads as cleared, which is exactly the background
    // case, and axis_inset() already falls back to the buffer origin when the
    // window does not fit.
    const bool draw_rect_valid =
        info.draw_width > 0 && info.draw_height > 0 &&
        info.display_width > 0 && info.display_height > 0;

    const DepthEntry* match = nullptr;
    if (want_depth && draw_rect_valid) {
        // Raw pass first, at sample resolution, to estimate this frame's range.
        run_depth_resolve(draw_info, m_range_fbo, kDepthRangeSampleW, kDepthRangeSampleH, false);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_range_fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo_range[m_pbo_index]);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, kDepthRangeSampleW, kDepthRangeSampleH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        if (m_pbo_primed) {
            const int prev = 1 - m_pbo_index;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo_range[prev]);
            const auto* mapped = static_cast<const uint8_t*>(
                glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)m_pbo_range_bytes, GL_MAP_READ_BIT));
            if (mapped) {
                m_hw_range_usable = consume_range_readback(mapped);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
        }
        // Normalised pass at full resolution, using the range from the previous
        // frame. One frame of lag is nothing against a range that is already
        // smoothed over roughly a second.
        //
        // Gated on having *a* valid range, not on this frame's estimate
        // succeeding. The estimator rejects any frame whose PGXP coverage dips
        // below the threshold, and gating the resolve on it froze the depth
        // image while the colour kept moving — the displacement then described
        // a picture that was several frames old.
        if (m_depth_range_valid) {
            // Zero-copy keeps depth at the drawn region's full render
            // resolution since the renderer samples it directly. Readback
            // resolves at native resolution to match the CPU colour frame it
            // will be paired with.
            const int depth_w = zero_copy ? draw_info.display_width : native_w;
            const int depth_h = zero_copy ? draw_info.display_height : native_h;
            DepthEntry& entry = m_depth_entries[m_depth_entry_index];
            if (entry.w != depth_w || entry.h != depth_h) {
                if (make_color_target(entry.fbo, entry.tex, depth_w, depth_h)) {
                    entry.w = depth_w;
                    entry.h = depth_h;
                } else {
                    entry.valid = false;
                }
            }
            if (entry.tex && entry.w == depth_w) {
                run_depth_resolve(draw_info, entry.fbo, depth_w, depth_h, true);
                entry.rect_x = draw_info.display_x;
                entry.rect_y = draw_info.display_y;
                entry.rect_w = draw_info.display_width;
                entry.rect_h = draw_info.display_height;
                entry.valid = true;
                m_depth_entry_index = (m_depth_entry_index + 1) % kPsxGpuFrameSlots;
            }
        }
    }

    // Prefer the entry whose rectangle is the one being scanned out: that is
    // the same image, so colour and depth agree exactly. Double-buffered games
    // hit this a flip after the buffer was drawn.
    for (const DepthEntry& e : m_depth_entries) {
        if (e.valid && e.tex &&
            e.rect_x == info.display_x && e.rect_y == info.display_y &&
            e.rect_w == info.display_width && e.rect_h == info.display_height) {
            match = &e;
            break;
        }
    }
    // Deliberately no fallback to "the newest resolve". Time Crisis alternates
    // buffers properly (drawing y=240 while displaying y=8), so the newest
    // resolve is the *other* buffer — a different picture. Displacing the image
    // by depth that belongs to another frame is worse than leaving it flat: the
    // geometry lands where nothing is, and the stereo stops agreeing with what
    // you see. A game whose displayed buffer is never drawn into simply gets no
    // depth.
    bool has_depth = (match != nullptr);

    if (has_depth && !zero_copy) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, match->fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo_depth[m_pbo_index]);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, match->w, match->h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (m_pbo_primed) {
            const int prev = 1 - m_pbo_index;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo_depth[prev]);
            const auto* mapped = static_cast<const uint8_t*>(glMapBufferRange(
                GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)m_pbo_depth_bytes, GL_MAP_READ_BIT));
            if (mapped) {
                consume_depth_readback(mapped, match->w, match->h);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    {
        // Depth has several independent gates and no way to tell from the
        // picture which one failed: a flat screen looks the same whether the
        // core published nothing, the range was rejected, or the resolve ran
        // and produced zeroes.
        // Log on transitions, not every Nth frame: a periodic sample lands on
        // the same parity every time and cannot show whether the buffers swap.
        static int last_disp_y = -1, last_draw_y = -1, last_has = -1;
        const bool changed = (info.display_y != last_disp_y || info.draw_y != last_draw_y ||
                              (int)has_depth != last_has);
        last_disp_y = info.display_y;
        last_draw_y = info.draw_y;
        last_has = (int)has_depth;
        static int depth_dbg = 0;
        ++depth_dbg;
        if (changed) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                "PSX DEPTH: path=%d zc=%d valid=%d pgxp=%d want=%d range_usable=%d has_depth=%d "
                "disp=%d,%d %dx%d draw=%d,%d %dx%d tex=%dx%d range=%.4f..%.4f depth_tex=%u",
                m_render_path, (int)zero_copy,
                (int)info.valid, (int)info.pgxp_depth, (int)want_depth,
                (int)m_hw_range_usable, (int)has_depth,
                info.display_x, info.display_y, info.display_width, info.display_height,
                info.draw_x, info.draw_y, info.draw_width, info.draw_height,
                info.texture_width, info.texture_height,
                m_depth_range_lo, m_depth_range_hi, match ? match->tex : 0u);
        }
    }

    // --- colour for the CPU pipeline -----------------------------------------
    // Native resolution regardless of internal scale: ambilight, previews and
    // the frame publish check all still work, and their cost stops scaling with
    // the renderer's resolution.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, slot.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_small_fbo);
    glBlitFramebuffer(0, 0, (GLint)width, (GLint)height, 0, 0, native_w, native_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_small_fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo_color[m_pbo_index]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, native_w, native_h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    if (m_pbo_primed) {
        const int prev = 1 - m_pbo_index;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo_color[prev]);
        const auto* mapped = static_cast<const uint8_t*>(
            glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)m_pbo_color_bytes, GL_MAP_READ_BIT));
        if (mapped) {
            consume_color_readback(mapped);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    m_pbo_index = 1 - m_pbo_index;
    m_pbo_primed = true;

    // --- publish -------------------------------------------------------------
    PsxGpuFrame published{};
    published.color_texture = slot.color;
    published.depth_texture = has_depth ? match->tex : 0;
    // Size of the image actually written into the slot.
    //
    // This must be the size video_refresh reports, NOT the depth bridge's
    // display rect. The depth rect is kept in unscaled PSX pixels (256x224 for
    // Time Crisis) while the colour slot is allocated at the internal
    // resolution scale (1024x896 at 4x), so using it made the shader sample a
    // 0.25 x 0.25 corner and stretch that quarter over the whole screen.
    //
    // Known gap: in 24-bit colour mode (FMV, Point Blank's attract) the core
    // renders the display unscaled -- gpu_hw_opengl.cpp uses
    // `display_area_color_depth_24 ? 1 : m_resolution_scale` -- so it writes a
    // small corner while video_refresh still reports the scaled size. There is
    // no 24-bit flag on PsxHwDepthInfo to detect that here, so those sequences
    // will sample too large an area. Fixing it properly means plumbing the
    // core's 24-bit flag through the bridge; gameplay is the common case and
    // was broken at every scale above 1x.
    published.width  = (int)width;
    published.height = (int)height;
    // The image fills the slot on this path, so there is no origin to skip.
    published.x = 0;
    published.y = 0;
    published.tex_width = m_hw_fbo_w;
    published.tex_height = m_hw_fbo_h;
    published.has_depth = has_depth;
    published.slot = m_hw_current_slot;
    m_hw_slot_pending = false;
    if (zero_copy) {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // Without this the fence may sit unflushed in this context's command
        // buffer while the renderer blocks waiting for it.
        glFlush();
        psx_gpu_frame_publish(published, fence);
    } else {
        // Readback path: nothing is shared, so make sure the renderer is not
        // still holding a stale texture from a previous zero-copy frame.
        psx_gpu_frame_reset();
    }

    // The renderer reads pixels from the shared textures; psx_depth now carries
    // only the metadata that says PSX is active and whether depth is usable.
    auto meta = std::make_shared<PsxDepthFrame>();
    meta->width = (uint32_t)native_w;
    meta->height = (uint32_t)native_h;
    meta->has_geometry = has_depth;
    if (!zero_copy && has_depth &&
        m_depth_bytes.size() == (std::size_t)native_w * native_h) {
        meta->depth = m_depth_bytes;
    } else if (!zero_copy) {
        meta->has_geometry = false;
    }
    m_frame.psx_depth = meta;
    m_frame.layers[0].rgba = m_frame.rgba8888;
    m_frame.layers[0].depth_map.clear();
}

void PsxLibretroBackend::destroy_hw_resources() {
    if (!psx_gl_context_host_available()) return;
    std::string err;
    if (!psx_gl_context_ensure_current(err)) return;
    psx_gpu_frame_reset();
    for (auto& slot : m_hw_slots) {
        if (slot.depth_tex) { glDeleteTextures(1, &slot.depth_tex); slot.depth_tex = 0; }
        if (slot.depth_fbo) { glDeleteFramebuffers(1, &slot.depth_fbo); slot.depth_fbo = 0; }
        if (slot.depth_rb) { glDeleteRenderbuffers(1, &slot.depth_rb); slot.depth_rb = 0; }
        if (slot.color) { glDeleteTextures(1, &slot.color); slot.color = 0; }
        if (slot.fbo) { glDeleteFramebuffers(1, &slot.fbo); slot.fbo = 0; }
        slot.depth_w = slot.depth_h = 0;
    }
    if (m_pbo_color[0]) { glDeleteBuffers(2, m_pbo_color); m_pbo_color[0] = m_pbo_color[1] = 0; }
    if (m_pbo_range[0]) { glDeleteBuffers(2, m_pbo_range); m_pbo_range[0] = m_pbo_range[1] = 0; }
    if (m_pbo_depth[0]) { glDeleteBuffers(2, m_pbo_depth); m_pbo_depth[0] = m_pbo_depth[1] = 0; }
    if (m_range_tex) { glDeleteTextures(1, &m_range_tex); m_range_tex = 0; }
    if (m_range_fbo) { glDeleteFramebuffers(1, &m_range_fbo); m_range_fbo = 0; }
    for (auto& entry : m_depth_entries) {
        if (entry.tex) { glDeleteTextures(1, &entry.tex); entry.tex = 0; }
        if (entry.fbo) { glDeleteFramebuffers(1, &entry.fbo); entry.fbo = 0; }
        entry = DepthEntry{};
    }
    m_depth_entry_index = 0;
    if (m_small_tex) { glDeleteTextures(1, &m_small_tex); m_small_tex = 0; }
    if (m_small_fbo) { glDeleteFramebuffers(1, &m_small_fbo); m_small_fbo = 0; }
    if (m_resolve_vao) { glDeleteVertexArrays(1, &m_resolve_vao); m_resolve_vao = 0; }
    if (m_resolve_program) { glDeleteProgram(m_resolve_program); m_resolve_program = 0; }
    m_hw_fbo_w = m_hw_fbo_h = 0;
    m_small_w = m_small_h = 0;
    m_pbo_color_bytes = m_pbo_range_bytes = m_pbo_depth_bytes = 0;
    m_pbo_primed = false;
    m_hw_range_usable = false;
    psx_gl_context_release();
}

void PsxLibretroBackend::reset_core() {
    close_aaudio_stream();
    psx_pgxp_capture_reset();
    reset_depth_range();
    if (m_hw_active) {
        std::string gl_err;
        const bool bound = psx_gl_context_ensure_current(gl_err);
        if (m_hw_render.context_destroy) m_hw_render.context_destroy();
        if (bound) psx_gl_context_release();
    }
    m_hw_active = false;
    m_hw_render_valid = false;
    destroy_hw_resources();
    if (m_game_loaded) swan_retro_unload_game();
    m_core_initialized = false;
    m_game_loaded = false;
    m_loaded_rom_path.clear();
    m_rom_bytes.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.depth_map.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kPsxLayerCount);
    for (auto& layer : m_frame.layers) {
        layer.rgba.clear();
        layer.depth_map.clear();
    }
    if (g_active_backend == this) g_active_backend = nullptr;
}

} // namespace qrd
