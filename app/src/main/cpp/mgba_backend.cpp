#include "mgba_backend.h"
#include "mgba_layer_capture.h"
#include "audio_processor.h"

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

extern "C" {
void     mgba_retro_set_environment(retro_environment_t cb);
void     mgba_retro_set_video_refresh(retro_video_refresh_t cb);
void     mgba_retro_set_audio_sample(retro_audio_sample_t cb);
void     mgba_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void     mgba_retro_set_input_poll(retro_input_poll_t cb);
void     mgba_retro_set_input_state(retro_input_state_t cb);
unsigned mgba_retro_api_version(void);
void     mgba_retro_set_controller_port_device(unsigned port, unsigned device);
void     mgba_retro_get_system_info(struct retro_system_info* info);
void     mgba_retro_get_system_av_info(struct retro_system_av_info* info);
size_t   mgba_retro_serialize_size(void);
bool     mgba_retro_serialize(void* data, size_t size);
bool     mgba_retro_unserialize(const void* data, size_t size);
void     mgba_retro_cheat_reset(void);
void     mgba_retro_cheat_set(unsigned index, bool enabled, const char* code);
bool     mgba_retro_load_game(const struct retro_game_info* info);
bool     mgba_retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info);
void     mgba_retro_unload_game(void);
unsigned mgba_retro_get_region(void);
void*    mgba_retro_get_memory_data(unsigned type);
size_t   mgba_retro_get_memory_size(unsigned type);
void     mgba_retro_reset(void);
void     mgba_retro_run(void);
void     mgba_retro_init(void);
void     mgba_retro_deinit(void);
// GBA audio channel volume control (0=PSG, 1=Direct Sound A, 2=Direct Sound B)
void     GBAudioSetChannelVolume(int channel, float volume);
}

namespace qrd {

namespace {

constexpr const char* kLogTag       = "QuestRetroDepth";
constexpr const char* kFrontendDir  = ".";
constexpr int         kWarmupFrames = 12;
// GB/GBC: 3 capture layers — 0=BG, 1=Window, 2=OBJ
constexpr int kGbLayerCount = 3;
// GBA: 5 capture layers — BG0-BG3, OBJ; backdrop (source_id 5) goes transparent
constexpr int kGbaLayerCount = 5;

MgbaBackend* g_active_backend = nullptr;
// See the equivalent comment in snes_libretro_backend.cpp: the core's global
// state is not safe to repeatedly retro_deinit()/retro_init() between ROM
// loads, so only ever initialize it once per process.
bool g_core_ever_initialized = false;
std::string g_mgba_system_dir = kFrontendDir;
std::string g_mgba_save_dir   = kFrontendDir;

// AAudio ring buffer (same design as PicoDrive backend)
constexpr int kAudioRingFrames = 8192;
static int16_t g_audio_ring[kAudioRingFrames * 2];
static std::atomic<int> g_ring_write{0};
static std::atomic<int> g_ring_read{0};
static AAudioStream* g_aaudio_stream = nullptr;
static int g_audio_sample_rate = 32768; // GBA default (core's native rate)
static retro_audio_buffer_status_callback_t g_audio_buffer_status_callback = nullptr;

// GBA's native audio rate (32768 Hz) doesn't evenly divide the Quest's fixed
// hardware mix rate (48000 Hz) — a 256:375 ratio. Rather than let AAudio's
// own resampler handle that mismatch, we resample to 48000 Hz ourselves.
//
// A plain linear interpolator has no stopband attenuation, so high-frequency
// content aliases back down into the audible range and sounds harsh/grainy.
// Instead we use a polyphase windowed-sinc (Kaiser window) resampler: the
// input:output ratio is reduced to lowest terms (L:M) and a bank of L
// bandlimited FIR filter phases is precomputed once per stream-open, cutting
// at min(input, output) Nyquist so neither aliasing nor imaging occurs.
constexpr int kOutputSampleRate = 48000;
static int    g_resample_input_rate = 32768;

constexpr int kResamplerTapsPerPhase = 16;   // taps either side of center, per phase
constexpr int kResamplerFilterLen    = 2 * kResamplerTapsPerPhase; // taps per phase

static int    g_resampler_L = 1;   // upsample factor (reduced ratio numerator, output side)
static int    g_resampler_M = 1;   // downsample factor (reduced ratio denominator, input side)
static std::vector<float> g_resampler_taps; // [phase][tap], phase in [0, L), tap in [0, kResamplerFilterLen)

// Persistent buffer of not-yet-fully-consumed input frames (interleaved L/R).
// New input is appended each call; frames the filter window has fully passed
// are dropped from the front. g_resampler_frame indexes into this buffer.
static std::vector<int16_t> g_resampler_buf;
static int64_t g_resampler_frame = kResamplerTapsPerPhase - 1; // integer input-frame index of the current output sample
static int     g_resampler_phase = 0;                          // fractional part, in [0, L)

static double sinc(double x) {
    if (std::fabs(x) < 1e-9) return 1.0;
    const double px = M_PI * x;
    return std::sin(px) / px;
}

// Modified Bessel function I0, needed for the Kaiser window.
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double halfx = x * 0.5;
    for (int k = 1; k <= 24; ++k) {
        term *= (halfx / k);
        sum += term * term;
    }
    return sum;
}

static void build_resampler_filter(int input_rate, int output_rate) {
    const int g = std::gcd(input_rate, output_rate);
    g_resampler_L = output_rate / g; // upsample
    g_resampler_M = input_rate  / g; // downsample

    // Cutoff below the lower of the two Nyquist rates (in the upsampled
    // L*input_rate domain), scaled slightly under 1.0 for a clean transition band.
    const double cutoff = 0.5 * std::min(1.0 / g_resampler_L, 1.0 / g_resampler_M) * 0.98;

    constexpr double kBeta = 8.0; // Kaiser window shape (higher = more stopband attenuation)
    const double i0_beta = bessel_i0(kBeta);

    g_resampler_taps.assign(static_cast<size_t>(g_resampler_L) * kResamplerFilterLen, 0.0f);
    for (int phase = 0; phase < g_resampler_L; ++phase) {
        double sum = 0.0;
        std::vector<double> tmp(kResamplerFilterLen);
        for (int t = 0; t < kResamplerFilterLen; ++t) {
            // Tap position relative to the (fractional) output sample, in input-frame units.
            const double center_offset = static_cast<double>(phase) / g_resampler_L;
            const double n = (t - kResamplerTapsPerPhase + 1) - center_offset;
            const double windowed_n = n / kResamplerTapsPerPhase;
            double w = 0.0;
            if (windowed_n > -1.0 && windowed_n < 1.0) {
                w = bessel_i0(kBeta * std::sqrt(std::max(0.0, 1.0 - windowed_n * windowed_n))) / i0_beta;
            }
            const double h = 2.0 * cutoff * sinc(2.0 * cutoff * n) * w;
            tmp[t] = h;
            sum += h;
        }
        // Normalize so each phase's taps sum to 1 (unity DC gain).
        if (sum != 0.0) {
            for (int t = 0; t < kResamplerFilterLen; ++t) tmp[t] /= sum;
        }
        for (int t = 0; t < kResamplerFilterLen; ++t) {
            g_resampler_taps[static_cast<size_t>(phase) * kResamplerFilterLen + t] = static_cast<float>(tmp[t]);
        }
    }
}

static void reset_resampler_state() {
    // Prime the buffer with kResamplerTapsPerPhase-1 frames of silence so the
    // first output sample's filter window (which looks kResamplerTapsPerPhase-1
    // frames back) has valid data instead of reading out of bounds.
    g_resampler_buf.assign(static_cast<size_t>(kResamplerTapsPerPhase - 1) * 2, 0);
    g_resampler_frame = kResamplerTapsPerPhase - 1;
    g_resampler_phase = 0;
}

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

static void resample_and_push(const int16_t* in, int in_frames) {
    if (in_frames <= 0) return;
    // g_resampler_taps is only populated by build_resampler_filter(), called
    // from open_aaudio_stream() — which preview-mode captures skip (no real
    // audio output needed for a thumbnail). Indexing into it before that has
    // run crashed with a null-pointer dereference the moment a GB game's
    // audio callback fired during preview capture.
    if (g_resampler_taps.empty()) return;

    // Append this call's input to the persistent buffer.
    const size_t old_frames = g_resampler_buf.size() / 2;
    g_resampler_buf.resize((old_frames + static_cast<size_t>(in_frames)) * 2);
    std::memcpy(&g_resampler_buf[old_frames * 2], in, static_cast<size_t>(in_frames) * 2 * sizeof(int16_t));

    const int64_t buf_frames = static_cast<int64_t>(g_resampler_buf.size() / 2);
    const int64_t L = g_resampler_L;
    const int64_t M = g_resampler_M;
    int16_t out[2];

    // Filter window for output sample at g_resampler_frame/g_resampler_phase
    // spans buffer indices [g_resampler_frame - (taps-1), g_resampler_frame + taps].
    while (g_resampler_frame + kResamplerTapsPerPhase < buf_frames) {
        const float* taps = &g_resampler_taps[static_cast<size_t>(g_resampler_phase) * kResamplerFilterLen];
        float acc_l = 0.0f, acc_r = 0.0f;
        const int64_t base = g_resampler_frame - (kResamplerTapsPerPhase - 1);
        for (int t = 0; t < kResamplerFilterLen; ++t) {
            const size_t idx = static_cast<size_t>(base + t);
            acc_l += taps[t] * static_cast<float>(g_resampler_buf[idx * 2 + 0]);
            acc_r += taps[t] * static_cast<float>(g_resampler_buf[idx * 2 + 1]);
        }
        out[0] = static_cast<int16_t>(std::clamp(acc_l, -32768.0f, 32767.0f));
        out[1] = static_cast<int16_t>(std::clamp(acc_r, -32768.0f, 32767.0f));
        audio_ring_push(out, 1);

        g_resampler_phase += static_cast<int>(M);
        g_resampler_frame += g_resampler_phase / L;
        g_resampler_phase %= L;
    }

    // Drop fully-consumed frames from the front, keeping enough lookback
    // (kResamplerTapsPerPhase - 1) for the next output sample's window.
    const int64_t drop = g_resampler_frame - (kResamplerTapsPerPhase - 1);
    if (drop > 0) {
        g_resampler_buf.erase(g_resampler_buf.begin(), g_resampler_buf.begin() + static_cast<size_t>(drop) * 2);
        g_resampler_frame -= drop;
    }
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

static void open_aaudio_stream(int core_sample_rate) {
    if (g_aaudio_stream) {
        AAudioStream_close(g_aaudio_stream);
        g_aaudio_stream = nullptr;
    }
    g_resample_input_rate = core_sample_rate > 0 ? core_sample_rate : 32768;
    build_resampler_filter(g_resample_input_rate, kOutputSampleRate);
    reset_resampler_state();

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, kOutputSampleRate);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, audio_data_callback, nullptr);
    AAudioStreamBuilder_openStream(builder, &g_aaudio_stream);
    AAudioStreamBuilder_delete(builder);
    if (g_aaudio_stream) {
        g_audio_processor.set_sample_rate(kOutputSampleRate);
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
    resample_and_push(buf, 1);
}

static std::size_t RETRO_CALLCONV frontend_audio_sample_batch(const int16_t* data, std::size_t frames) {
    resample_and_push(data, static_cast<int>(frames));
    return frames;
}

static void RETRO_CALLCONV frontend_input_poll() {}

static int16_t RETRO_CALLCONV frontend_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_active_backend ? g_active_backend->handle_input_state(port, device, index, id) : 0;
}

// Conventional RGB565 (Red high: bits 15-11, Green bits 10-5, Blue bits 4-0) —
// the format libretro's RETRO_PIXEL_FORMAT_RGB565 guarantees. Used only for
// pixels coming through the retro_video_refresh callback (write_rgb565_frame).
static uint32_t rgba_from_rgb565(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5)  & 0x3F) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// mGBA's internal mColor with COLOR_16_BIT + COLOR_5_6_5 (see mgba-util/image.h:44-51)
// packs Red in bits 0-4, Green in bits 5-10, Blue in bits 11-15 — despite the
// "RGB565" name, that's the reverse of libretro's conventional RGB565 (Red high).
// Used for the raw mColor values captured directly from mGBA's renderer
// (mgba_lc_bg_pixel/mgba_lc_obj_pixel), which never pass through libretro's
// pixel-format conversion.
static uint32_t rgba_from_mgba_color(uint16_t pixel) {
    const uint8_t r = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5)  & 0x3F) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base  = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot   = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

static void ensure_dir_exists(const std::string& path) {
    if (!path.empty()) mkdir(path.c_str(), 0755);
}

static const char* mgba_option_value(const char* key) {
    static const std::unordered_map<std::string, const char*> kOptions = {
        {"mgba_gb_model", "Autodetect"},
        {"mgba_use_bios", "ON"},
        {"mgba_skip_bios", "OFF"},
        {"mgba_gb_colors", "Grayscale"},
        {"mgba_gb_colors_preset", "0"},
        {"mgba_sgb_borders", "ON"},
        {"mgba_audio_low_pass_filter", "disabled"},
        {"mgba_audio_low_pass_range", "60"},
        {"mgba_allow_opposing_directions", "no"},
        {"mgba_solar_sensor_level", "0"},
        {"mgba_force_gbp", "OFF"},
        // "Remove Known" (idle-loop skip, GBA-only — GB/GBC has no equivalent) patches out
        // detected CPU spin-loops for speed, but that alters exactly the cycle timing some
        // GBA games' audio-mixing routines depend on — a known source of GBA-specific audio
        // crackle/distortion that GB/GBC can't exhibit since idle-skip never applies there.
        {"mgba_idle_optimization", "Don't Remove"},
        {"mgba_frameskip", "0"},
        {"mgba_frameskip_threshold", "33"},
        {"mgba_frameskip_interval", "0"},
    };
    if (!key) return nullptr;
    const auto it = kOptions.find(key);
    return it == kOptions.end() ? nullptr : it->second;
}

} // namespace

void set_mgba_frontend_directories(std::string system_dir, std::string save_dir) {
    if (system_dir.empty()) system_dir = kFrontendDir;
    if (save_dir.empty()) save_dir = system_dir;
    ensure_dir_exists(system_dir);
    ensure_dir_exists(save_dir);
    g_mgba_system_dir = std::move(system_dir);
    g_mgba_save_dir = std::move(save_dir);
}

// ---------------------------------------------------------------------------
// MgbaBackend
// ---------------------------------------------------------------------------

MgbaBackend::MgbaBackend() {
    retro_system_info info{};
    mgba_retro_get_system_info(&info);

    std::ostringstream name;
    if (info.library_name && info.library_name[0] != '\0') {
        name << info.library_name;
        if (info.library_version && info.library_version[0] != '\0')
            name << " " << info.library_version;
        name << " (libretro)";
    } else {
        name << "mGBA (libretro)";
    }
    m_backend_name = name.str();

    // GBA: BG0, BG1, BG2, BG3, OBJ — visible-source extraction, no raw layer captures needed
    m_frame.layers.resize(kGbaLayerCount);
    ensure_frame_size(240, 160);
}

MgbaBackend::~MgbaBackend() {
    reset_core();
}

const char* MgbaBackend::backend_name() const { return m_backend_name.c_str(); }

double MgbaBackend::frame_rate_hz() const { return m_frame_rate_hz; }

bool MgbaBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    if (rom_path.empty()) { error_out = "mGBA: ROM path is empty."; return false; }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "mGBA load_content: start path='%s'", rom_path.c_str());
    if (!ensure_core_initialized(error_out)) return false;

    {
        std::ifstream f(rom_path, std::ios::binary | std::ios::ate);
        if (!f) { error_out = "mGBA: unable to open ROM file."; return false; }
        const std::streamsize sz = f.tellg();
        if (sz <= 0) { error_out = "mGBA: ROM file is empty."; return false; }
        m_rom_bytes.assign(static_cast<std::size_t>(sz), 0);
        f.seekg(0, std::ios::beg);
        if (!f.read(reinterpret_cast<char*>(m_rom_bytes.data()), sz)) {
            error_out = "mGBA: failed to read ROM file.";
            m_rom_bytes.clear();
            return false;
        }
    }

    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = m_rom_bytes.data();
    game_info.size = m_rom_bytes.size();
    game_info.meta = nullptr;
    if (!mgba_retro_load_game(&game_info)) {
        error_out = "mGBA: retro_load_game failed.";
        reset_core();
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "mGBA load_content: retro_load_game OK");

    retro_system_av_info av_info{};
    mgba_retro_get_system_av_info(&av_info);
    ensure_frame_size(av_info.geometry.base_width, av_info.geometry.base_height);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 59.7275;
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 32768;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "mGBA AV: fps=%.4f sample_rate=%.1f geometry=%ux%u",
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
        mgba_lc_clear();
        mgba_retro_run();
        if (m_video_frame_count > 0 && m_last_frame_had_visible_pixels) break;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "mGBA load_content: warmup done video_frame_count=%llu visible=%d",
                        static_cast<unsigned long long>(m_video_frame_count),
                        m_last_frame_had_visible_pixels ? 1 : 0);

    if (m_video_frame_count == 0) {
        error_out = "mGBA: ROM loaded but emitted no video frames.";
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool MgbaBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) { error_out = "mGBA: no ROM loaded."; return false; }
    m_input = input;
    report_audio_buffer_status();
    mgba_lc_clear();
    mgba_retro_run();
    error_out.clear();
    return true;
}

const FrameOutput& MgbaBackend::frame_output() const { return m_frame; }

bool MgbaBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "mGBA: no ROM loaded."; return false; }
    const std::size_t size = mgba_retro_serialize_size();
    if (size == 0) { error_out = "mGBA: zero savestate size."; return false; }
    out.assign(size, 0);
    if (!mgba_retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "mGBA: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool MgbaBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "mGBA: no ROM loaded."; return false; }
    if (!data || size == 0) { error_out = "mGBA: savestate data empty."; return false; }
    if (!mgba_retro_unserialize(data, size)) {
        error_out = "mGBA: retro_unserialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

void MgbaBackend::set_auto_frame_skip(bool enabled) {
    m_auto_frame_skip = enabled;
}
void MgbaBackend::set_layer_capture_mask(uint32_t mask) {
    m_layer_capture_mask = mask;
}

RomHeaderInfo MgbaBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* MgbaBackend::get_z_histogram() const { return nullptr; }

const uint8_t* MgbaBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(mgba_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t MgbaBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return mgba_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

bool MgbaBackend::handle_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        if (!data) return false;
        auto* cb = static_cast<retro_log_callback*>(data);
        cb->log = frontend_log_printf;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (!data) return false;
        *static_cast<const char**>(data) = g_mgba_system_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        if (!data) return false;
        auto** dir = static_cast<const char**>(data);
        *dir = g_mgba_save_dir.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        // GBA content can change its own audio sample rate mid-game (Direct Sound is
        // DMA/timer-driven and games can reprogram the timer), which mGBA reports via
        // this callback (see _audioRateChanged in libretro.c) — GB/GBC instead uses a
        // fixed-rate stream (_postAudioBuffer) that never fires this. We weren't handling
        // it at all, so the resampler kept using whatever rate was current at load time
        // forever after: once a GBA game reprogrammed its rate, every sample afterward was
        // resampled against the wrong ratio, heard as a constant crackle/distortion for
        // the rest of that game (every GBA game does this, which matches the report).
        if (!data) return false;
        const auto* info = static_cast<const retro_system_av_info*>(data);
        const int new_rate = (info->timing.sample_rate > 0.0)
            ? static_cast<int>(info->timing.sample_rate) : g_audio_sample_rate;
        if (new_rate != g_audio_sample_rate) {
            g_audio_sample_rate = new_rate;
            if (!m_preview_mode || m_preview_allow_audio) open_aaudio_stream(g_audio_sample_rate);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!data) return false;
        const auto fmt = *static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_RGB565 && fmt != RETRO_PIXEL_FORMAT_XRGB8888) return false;
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
        if (!data) return false;
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        // QuestRetroDepth owns this one instead of the static kOptions table
        // below (mirrors MameBackend's "mame_autoframeskip"/
        // SaturnLibretroBackend's "yabasanshiro_frameskip"). Unlike those
        // cores' true adaptive auto-skip, mGBA only exposes a fixed
        // skip-N-frames-after-each-render count -- "2" (~20fps) is the
        // closest one-toggle equivalent; there's no real "auto" mode here.
        if (std::strcmp(var->key, "mgba_frameskip") == 0) {
            var->value = m_auto_frame_skip ? "2" : "0";
            return true;
        }
        var->value = mgba_option_value(var->key);
        return var->value != nullptr;
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
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
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
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
    case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        return false;
    // Silently accept anything we don't need to handle
    default:
        return false;
    }
}

void MgbaBackend::handle_video_frame(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);
    if (m_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        write_xrgb8888_frame(static_cast<const uint32_t*>(data), width, height, pitch);
    } else {
        write_rgb565_frame(static_cast<const uint16_t*>(data), width, height, pitch);
    }

    // Copy the per-pixel visible-source IDs captured by the renderer hook.
    // GBA and GB/GBC use distinct capture paths inside mGBA.
    const uint8_t* vs = nullptr;
    if (width == MGBA_GB_LC_W && height == MGBA_GB_LC_H) {
        vs = mgba_gb_lc_get_visible_source();
    } else {
        vs = mgba_lc_get_visible_source();
    }
    if (vs && m_frame.visible_source_id.size() == static_cast<std::size_t>(width) * height) {
        std::memcpy(m_frame.visible_source_id.data(), vs,
                    static_cast<std::size_t>(width) * height);
    } else if (m_frame.visible_source_id.size() == static_cast<std::size_t>(width) * height) {
        std::fill(m_frame.visible_source_id.begin(), m_frame.visible_source_id.end(), 0xFFu);
    }

    // Split final RGBA frame into per-layer buffers using visible-source IDs.
    const std::size_t npix = static_cast<std::size_t>(width) * height;
    const auto& vsid = m_frame.visible_source_id;
    const auto& src  = m_frame.rgba8888;
    const bool is_gb = (width == MGBA_GB_LC_W && height == MGBA_GB_LC_H);
    const int lcount = is_gb ? kGbLayerCount : kGbaLayerCount;
    m_frame.layers.resize(lcount);

    if (vsid.size() == npix && src.size() == npix && m_layer_capture_mask) {
        for (int li = 0; li < lcount; ++li) {
            if (m_frame.layers[li].rgba.size() != npix)
                m_frame.layers[li].rgba.resize(npix);
        }
        if (is_gb) {
            // GB/GBC: source IDs 0=BG, 1=Window, 4=OBJ → layers 0,1,2; OBJ (li=2) gets depth_map
            auto& dmap = m_frame.layers[2].depth_map;
            if (dmap.size() != npix) dmap.resize(npix);
            for (unsigned y = 0; y < height; ++y) {
                const uint8_t depth_y = (height > 1u)
                    ? static_cast<uint8_t>(y * 255u / (height - 1u)) : 128u;
                for (unsigned x = 0; x < width; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * width + x;
                    const uint8_t sid = vsid[i];
                    const uint32_t px = src[i];
                    m_frame.layers[0].rgba[i] = (sid == 0u) ? px : 0u;
                    m_frame.layers[1].rgba[i] = (sid == 1u) ? px : 0u;
                    m_frame.layers[2].rgba[i] = (sid == 4u) ? px : 0u;
                    dmap[i] = (sid == 4u) ? depth_y : 0u;
                }
            }
        } else {
            // GBA BG0-3: direct-write per-pixel capture, uniformly across every BG mode
            // (0/1/2 tile+affine via MGBA_QRD_BG_CAP, 3/4/5 bitmap via MGBA_QRD_BG_CAP_RAW —
            // see software-bg.c). Every draw path captures at the exact point mGBA decides
            // the pixel is opaque and about to composite, so the mask is ground truth —
            // no need to infer layer identity from the composited frame's flag bits.
            std::size_t bg_mask_count[4] = {0, 0, 0, 0};
            for (int li = 0; li < 4; ++li) {
                auto& layer = m_frame.layers[li];
                if (layer.rgba.size() != npix) layer.rgba.resize(npix);
                const uint16_t* lpix  = mgba_lc_get_bg_pixels(li);
                const uint8_t*  lmask = mgba_lc_get_bg_mask(li);
                for (std::size_t i = 0; i < npix; ++i) {
                    if (lmask && lmask[i]) {
                        layer.rgba[i] = rgba_from_mgba_color(lpix[i]);
                        ++bg_mask_count[li];
                    } else {
                        layer.rgba[i] = 0u;
                    }
                }
            }
            // GBA OBJ: direct-write capture, same architecture as BG0-3 — sprite
            // pixels are captured where mGBA actually draws them (software-obj.c's
            // PostprocessSprite), not inferred from packed flag bits in the
            // composited framebuffer. See mgba_lc_obj_pixel().
            auto& obj  = m_frame.layers[4];
            auto& dmap = obj.depth_map;
            if (obj.rgba.size() != npix) obj.rgba.resize(npix);
            if (dmap.size() != npix) dmap.resize(npix);
            const uint16_t* opix = mgba_lc_get_obj_pixels();
            const uint8_t*  omask = mgba_lc_get_obj_mask();
            std::size_t obj_pixel_count = 0;
            for (unsigned y = 0; y < height; ++y) {
                const uint8_t depth_y = (height > 1u)
                    ? static_cast<uint8_t>(y * 255u / (height - 1u)) : 128u;
                for (unsigned x = 0; x < width; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * width + x;
                    const bool is_obj = omask && omask[i];
                    obj.rgba[i] = is_obj ? rgba_from_mgba_color(opix[i]) : 0u;
                    dmap[i]     = is_obj ? depth_y : 0u;
                    if (is_obj) ++obj_pixel_count;
                }
            }

            // Diagnostic: log GBA layer capture stats roughly once a second so we can
            // see frame size and per-BG/OBJ pixel counts — useful for tracking down
            // layer/content mismatches.
            static std::uint64_t s_gba_diag_frame = 0;
            if ((++s_gba_diag_frame % 60u) == 0u) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                    "GBA layer capture: %ux%u bg0=%zu bg1=%zu bg2=%zu bg3=%zu obj=%zu",
                    width, height,
                    bg_mask_count[0], bg_mask_count[1], bg_mask_count[2], bg_mask_count[3],
                    obj_pixel_count);
            }
        }
    } else {
        for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    }
}

bool MgbaBackend::ensure_core_initialized(std::string& error_out) {
    g_active_backend = this;
    mgba_retro_set_environment(frontend_environment);
    mgba_retro_set_video_refresh(frontend_video_refresh);
    mgba_retro_set_audio_sample(frontend_audio_sample);
    mgba_retro_set_audio_sample_batch(frontend_audio_sample_batch);
    mgba_retro_set_input_poll(frontend_input_poll);
    mgba_retro_set_input_state(frontend_input_state);
    if (!g_core_ever_initialized) {
        mgba_retro_init();
        g_core_ever_initialized = true;
    }
    mgba_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void MgbaBackend::reset_core() {
    close_aaudio_stream();
    if (m_game_loaded) {
        mgba_retro_unload_game();
    }
    m_core_initialized = false;
    m_game_loaded      = false;
    m_loaded_rom_path.clear();
    m_video_frame_count = 0;
    m_last_frame_had_visible_pixels = false;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.layers.resize(kGbaLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    if (g_active_backend == this) g_active_backend = nullptr;
}

void MgbaBackend::ensure_frame_size(unsigned width, unsigned height) {
    const auto w = std::max(1u, width);
    const auto h = std::max(1u, height);
    if (m_frame.width == w && m_frame.height == h &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(w) * h &&
        m_frame.visible_source_id.size() == static_cast<std::size_t>(w) * h) return;

    m_frame.width  = w;
    m_frame.height = h;
    const std::size_t npix = static_cast<std::size_t>(w) * h;
    m_frame.rgba8888.assign(npix, 0xFF000000u);
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.assign(npix, 0xFFu);
    m_frame.layers.resize(kGbaLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
}

void MgbaBackend::write_rgb565_frame(
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

void MgbaBackend::write_xrgb8888_frame(
    const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch) {
    bool has_visible = false;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(pixels) + y * pitch);
        auto* dst = m_frame.rgba8888.data() + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const uint32_t xrgb = row[x];
            const uint32_t rgba = 0xFF000000u |
                ((xrgb >> 16) & 0x000000FFu) << 16 |
                ((xrgb >> 8)  & 0x000000FFu) << 8  |
                (xrgb & 0x000000FFu);
            dst[x] = rgba;
            has_visible = has_visible || ((rgba & 0x00FFFFFFu) != 0);
        }
    }
    m_last_frame_had_visible_pixels = has_visible;
}

int16_t MgbaBackend::handle_input_state(
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

void MgbaBackend::set_channel_volume(int channel, float volume) const {
    GBAudioSetChannelVolume(channel, volume);
}

} // namespace qrd
