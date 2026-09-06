#include "mame_backend.h"
#include "mame_layer_capture.h"
#include "audio_processor.h"
#include "neogeo_palette_debug.h"

#include "libretro.h"

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <dlfcn.h>

// MAME's libretro entry points are exported by mame_libretro_android.so
// (linked as an IMPORTED SHARED CMake target, see CMakeLists.txt), unlike
// every other core here, which is compiled directly into this .so with its
// retro_* symbols renamed at compile time (-Dretro_load_game=xxx_retro_load_game
// etc. in CMakeLists.txt) specifically so multiple cores sharing one binary
// don't collide. SNES9X is the one exception -- it's the original/base
// backend and was never given that treatment, so it still exports plain,
// globally-visible symbols named exactly "retro_load_game" etc. straight out
// of this .so itself.
//
// That is fatal for a plain `extern "C" bool retro_load_game(...)` + direct
// call here: the linker resolves an unprefixed symbol reference to a
// definition local to the *same* shared object before it ever considers a
// dependency's export, so every call below would silently bind to SNES9X's
// implementation instead of MAME's separate .so -- confirmed via `llvm-nm -D`
// on the built .so, which shows retro_load_game as a real defined (T) symbol
// here, not an unresolved import. (This explains a lot of confusing MAME
// behavior seen on-device before this fix -- e.g. "geometry=256x224" showing
// up for every single MAME load, which is SNES's native resolution, not any
// arcade board's.)
//
// Fix: resolve every entry point explicitly via dlsym() against MAME's own
// already-loaded module handle (RTLD_NOLOAD since it's already a DT_NEEDED
// dependency of this .so) instead of relying on the linker's default name
// resolution.
extern "C" {
typedef void     (*retro_set_environment_t)(retro_environment_t);
typedef void     (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void     (*retro_set_audio_sample_fn_t)(retro_audio_sample_t);
typedef void     (*retro_set_audio_sample_batch_fn_t)(retro_audio_sample_batch_t);
typedef void     (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void     (*retro_set_input_state_t)(retro_input_state_t);
typedef unsigned (*retro_api_version_t)(void);
typedef void     (*retro_set_controller_port_device_t)(unsigned, unsigned);
typedef void     (*retro_get_system_info_t)(struct retro_system_info*);
typedef void     (*retro_get_system_av_info_t)(struct retro_system_av_info*);
typedef size_t   (*retro_serialize_size_t)(void);
typedef bool     (*retro_serialize_t)(void*, size_t);
typedef bool     (*retro_unserialize_t)(const void*, size_t);
typedef bool     (*retro_load_game_t)(const struct retro_game_info*);
typedef void     (*retro_unload_game_t)(void);
typedef void*    (*retro_get_memory_data_t)(unsigned);
typedef size_t   (*retro_get_memory_size_t)(unsigned);
typedef void     (*retro_reset_t)(void);
typedef void     (*retro_run_t)(void);
typedef void     (*retro_init_t)(void);
typedef void     (*retro_deinit_t)(void);
typedef void     (*mame_occupancy_set_enabled_t)(int);
typedef int      (*mame_occupancy_available_t)(void);
typedef int      (*mame_occupancy_valid_t)(void);
typedef int      (*mame_occupancy_bucket_count_t)(void);
typedef const uint32_t* (*mame_occupancy_bucket_pixels_t)(int, uint32_t*, uint32_t*);
typedef int      (*mame_game_orientation_t)(void);
}

namespace {
struct MameEntryPoints {
    retro_set_environment_t retro_set_environment = nullptr;
    retro_set_video_refresh_t retro_set_video_refresh = nullptr;
    retro_set_audio_sample_fn_t retro_set_audio_sample = nullptr;
    retro_set_audio_sample_batch_fn_t retro_set_audio_sample_batch = nullptr;
    retro_set_input_poll_t retro_set_input_poll = nullptr;
    retro_set_input_state_t retro_set_input_state = nullptr;
    retro_set_controller_port_device_t retro_set_controller_port_device = nullptr;
    retro_get_system_av_info_t retro_get_system_av_info = nullptr;
    retro_serialize_size_t retro_serialize_size = nullptr;
    retro_serialize_t retro_serialize = nullptr;
    retro_unserialize_t retro_unserialize = nullptr;
    retro_load_game_t retro_load_game = nullptr;
    retro_unload_game_t retro_unload_game = nullptr;
    retro_get_memory_data_t retro_get_memory_data = nullptr;
    retro_get_memory_size_t retro_get_memory_size = nullptr;
    retro_run_t retro_run = nullptr;
    retro_init_t retro_init = nullptr;
    mame_occupancy_set_enabled_t mame_occupancy_set_enabled = nullptr;
    mame_occupancy_available_t mame_occupancy_available = nullptr;
    mame_occupancy_valid_t mame_occupancy_valid = nullptr;
    mame_occupancy_bucket_count_t mame_occupancy_bucket_count = nullptr;
    mame_occupancy_bucket_pixels_t mame_occupancy_bucket_pixels = nullptr;
    mame_game_orientation_t mame_game_orientation = nullptr;
    bool ok = false;
};

MameEntryPoints& mame_entry_points() {
    static MameEntryPoints ep = [] {
        MameEntryPoints e;
        // Already a DT_NEEDED dependency of this .so (linked at build time
        // via the IMPORTED SHARED CMake target) -- RTLD_NOLOAD just returns
        // its existing handle rather than loading a second copy.
        void* handle = dlopen("libmame_libretro.so", RTLD_NOLOAD | RTLD_NOW);
        if (!handle) {
            __android_log_print(ANDROID_LOG_ERROR, "QuestRetroDepth",
                "MAME dlopen(RTLD_NOLOAD) failed: %s", dlerror());
            return e;
        }
#define QRD_MAME_DLSYM(field, name) \
        e.field = reinterpret_cast<decltype(e.field)>(dlsym(handle, name)); \
        if (!e.field) __android_log_print(ANDROID_LOG_ERROR, "QuestRetroDepth", \
            "MAME dlsym('%s') failed: %s", name, dlerror());
        QRD_MAME_DLSYM(retro_set_environment, "retro_set_environment")
        QRD_MAME_DLSYM(retro_set_video_refresh, "retro_set_video_refresh")
        QRD_MAME_DLSYM(retro_set_audio_sample, "retro_set_audio_sample")
        QRD_MAME_DLSYM(retro_set_audio_sample_batch, "retro_set_audio_sample_batch")
        QRD_MAME_DLSYM(retro_set_input_poll, "retro_set_input_poll")
        QRD_MAME_DLSYM(retro_set_input_state, "retro_set_input_state")
        QRD_MAME_DLSYM(retro_set_controller_port_device, "retro_set_controller_port_device")
        QRD_MAME_DLSYM(retro_get_system_av_info, "retro_get_system_av_info")
        QRD_MAME_DLSYM(retro_serialize_size, "retro_serialize_size")
        QRD_MAME_DLSYM(retro_serialize, "retro_serialize")
        QRD_MAME_DLSYM(retro_unserialize, "retro_unserialize")
        QRD_MAME_DLSYM(retro_load_game, "retro_load_game")
        QRD_MAME_DLSYM(retro_unload_game, "retro_unload_game")
        QRD_MAME_DLSYM(retro_get_memory_data, "retro_get_memory_data")
        QRD_MAME_DLSYM(retro_get_memory_size, "retro_get_memory_size")
        QRD_MAME_DLSYM(retro_run, "retro_run")
        QRD_MAME_DLSYM(retro_init, "retro_init")
        // Optional so an older prebuilt core retains all existing behavior and
        // simply has no generic OCCUPXY stream.
        e.mame_occupancy_set_enabled = reinterpret_cast<mame_occupancy_set_enabled_t>(dlsym(handle, "mame_occupancy_set_enabled"));
        e.mame_occupancy_available = reinterpret_cast<mame_occupancy_available_t>(dlsym(handle, "mame_occupancy_available"));
        e.mame_occupancy_valid = reinterpret_cast<mame_occupancy_valid_t>(dlsym(handle, "mame_occupancy_valid"));
        e.mame_occupancy_bucket_count = reinterpret_cast<mame_occupancy_bucket_count_t>(dlsym(handle, "mame_occupancy_bucket_count"));
        e.mame_occupancy_bucket_pixels = reinterpret_cast<mame_occupancy_bucket_pixels_t>(dlsym(handle, "mame_occupancy_bucket_pixels"));
        e.mame_game_orientation = reinterpret_cast<mame_game_orientation_t>(dlsym(handle, "mame_game_orientation"));
#undef QRD_MAME_DLSYM
        e.ok = e.retro_set_environment && e.retro_set_video_refresh && e.retro_set_audio_sample &&
               e.retro_set_audio_sample_batch && e.retro_set_input_poll && e.retro_set_input_state &&
               e.retro_set_controller_port_device && e.retro_get_system_av_info &&
               e.retro_serialize_size && e.retro_serialize && e.retro_unserialize &&
               e.retro_load_game && e.retro_unload_game && e.retro_get_memory_data &&
               e.retro_get_memory_size && e.retro_run && e.retro_init;
        return e;
    }();
    return ep;
}
} // namespace

namespace qrd {

namespace {

constexpr const char* kLogTag       = "QuestRetroDepth";
constexpr int         kWarmupFrames = 30; // MAME/CPS boards take longer to reach a visible frame than consoles

// Fixed name -> slot mapping shared by every MAME driver's layer export.
// CPS1/CPS2 write background/scroll3/scroll2/scroll1/sprites (slots 0-4,
// see third_party/mame_libretro/src/mame/capcom/{cps1_v,cps2}.cpp);
// Konami's tmnt.cpp/simpsons.cpp write scroll2/scroll1/scroll0/sprites
// (slots 2,3,5,4 -- scroll2/scroll1/sprites reuse the same slots CPS uses,
// scroll0 gets its own since CPS has no equivalent). Must match
// GameConfig::make_default_mame_cps()/make_default_mame_konami()/
// make_default_mame_segas16b()/make_default_mame_dec0()/
// make_default_mame_gp9001()'s layer_index values. Only one driver is ever
// active per process, so the unused slots for whichever variant isn't
// loaded just stay empty.
// Neo Geo (neogeo_v.cpp) exports "neogeo_base" (the ordinary flat composite --
// backdrop + every sprite + fix layer, unchanged) plus RD_DRAW_CAP=30
// true-independent-capture layers named "neogeo_draw0".."neogeo_draw29", one
// per distinct sprite the renderer visits first each frame (real draw order,
// no z-range bucketing) -- see rd_claim_capture_slot()'s comment in
// neogeo_spr.h for why (no tilemap planes to redraw separately the way
// CPS1/Konami/etc do, so this capped per-object capture substitutes).
constexpr int kNeogeoDrawCap = 30;
constexpr int kNeogeoLayerCount = 2 + kNeogeoDrawCap; // neogeo_base + neogeo_fix + draw0..29
// Keep the fallback outside the existing Neo Geo slot range so saved layer
// indices and the established family configurations remain compatible.
constexpr int kMameFullFrameLayerIndex = 44;
constexpr int kSaturnLayerBase = kMameFullFrameLayerIndex + 1;
constexpr int kSaturnLayerCount = 8;
// Taito PC080SN/PC090OJ family (opwolf.cpp, othunder.cpp, undrfire.cpp, ...):
// two tilemap planes (bg/fg) plus one sprite plane.
constexpr int kTaitoLayerBase = kSaturnLayerBase + kSaturnLayerCount;
constexpr int kTaitoLayerCount = 3;
// Namco System 2 (namcos2_v.cpp): coarse background-composite + sprites
// split (bubbletr/gollygho/luckywld/sgunner/sgunner2). Also reused as-is by
// namconb1.cpp (ptblank) -- same 2-layer namco_bg/namco_sprites scheme, own
// local export helper, no dedicated slots needed.
constexpr int kNamcoLayerBase = kTaitoLayerBase + kTaitoLayerCount;
constexpr int kNamcoLayerCount = 2;
// Konami K056832/K053244 (lethal.cpp): 3 tilemap sublayers + sprites + a
// forced-topmost text tilemap.
constexpr int kKonamiLethalLayerBase = kNamcoLayerBase + kNamcoLayerCount;
constexpr int kKonamiLethalLayerCount = 5;
// Taito TC0100SCN (taito_z_v.cpp screen_update_spacegun): 2 bg tilemap
// layers + text layer + sprites.
constexpr int kTaitoTc0100LayerBase = kKonamiLethalLayerBase + kKonamiLethalLayerCount;
constexpr int kTaitoTc0100LayerCount = 4;
// Taito TC0480SCP (gunbustr.cpp, slapshot.cpp/slapshot_v.cpp): 4 dynamic-
// order bg layers + fixed text layer + sprites.
constexpr int kTaitoTc0480LayerBase = kTaitoTc0100LayerBase + kTaitoTc0100LayerCount;
constexpr int kTaitoTc0480LayerCount = 6;
// Unico (unico.cpp): 3 tilemaps + sprites.
constexpr int kUnicoLayerBase = kTaitoTc0480LayerBase + kTaitoTc0480LayerCount;
constexpr int kUnicoLayerCount = 4;
// Misc oneshot.cpp: bg/mid tilemaps, sprites, fg tilemap (fg topmost).
constexpr int kOneshotLayerBase = kUnicoLayerBase + kUnicoLayerCount;
constexpr int kOneshotLayerCount = 4;
// IGS lordgun_v.cpp: 4 scrolling tilemaps + sprites, piggybacked off the
// driver's own already-isolated m_bitmaps[0..4].
constexpr int kLordgunLayerBase = kOneshotLayerBase + kOneshotLayerCount;
constexpr int kLordgunLayerCount = 5;
// Seta X1-020/dx-101 (devices/video/x1_020_dx_101.cpp): sprite-only
// hardware (deerhunt/trophyh/turkhunt/wschamp), single layer.
constexpr int kSeta2LayerBase = kLordgunLayerBase + kLordgunLayerCount;
constexpr int kSeta2LayerCount = 1;
// Sega Y-board (segaybd.cpp, rchase): simplified 2-layer split -- combined
// rotated-tilemap+y-sprites scene, plus the separable b-board sprite plane.
constexpr int kSegaybdLayerBase = kSeta2LayerBase + kSeta2LayerCount;
constexpr int kSegaybdLayerCount = 2;
// SNK bbusters.cpp (bbusters/mechatt): 2 playfield tilemaps + fix tilemap +
// 2 sprite-chip composites (palette-bank priority split simplified away).
constexpr int kBbustersLayerBase = kSegaybdLayerBase + kSegaybdLayerCount;
constexpr int kBbustersLayerCount = 5;
// Taito nycaptor.cpp: simplified 2 background-layer composites + sprites
// (runtime spot()&3 interleave and 4-priority-band structure collapsed).
constexpr int kNycaptorLayerBase = kBbustersLayerBase + kBbustersLayerCount;
constexpr int kNycaptorLayerCount = 3;
constexpr int kMameOccupancyBase = kNycaptorLayerBase + kNycaptorLayerCount;
constexpr int kMameOccupancyBucketCount = 6;
constexpr int kMameOccupancyResidualSlot = kMameOccupancyBase + kMameOccupancyBucketCount;
constexpr int kMameLayerCount = kMameOccupancyResidualSlot + 1;
const char* const kMameLayerNames[kMameLayerCount] = {
    "background", "scroll3", "scroll2", "scroll1", "sprites", "scroll0",
    // Sega System 16B (segas16b_v.cpp)
    "foreground", "text",
    // Data East dec0.cpp
    "midground",
    // Toaplan GP9001 (gp9001.cpp, shared by ~10 toaplan2-era driver files)
    "layer0", "layer1", "layer2",
    // Neo Geo (see comment above)
    "neogeo_base",
    "neogeo_fix",
    "neogeo_draw0",  "neogeo_draw1",  "neogeo_draw2",  "neogeo_draw3",
    "neogeo_draw4",  "neogeo_draw5",  "neogeo_draw6",  "neogeo_draw7",
    "neogeo_draw8",  "neogeo_draw9",  "neogeo_draw10", "neogeo_draw11",
    "neogeo_draw12", "neogeo_draw13", "neogeo_draw14", "neogeo_draw15",
    "neogeo_draw16", "neogeo_draw17", "neogeo_draw18", "neogeo_draw19",
    "neogeo_draw20", "neogeo_draw21", "neogeo_draw22", "neogeo_draw23",
    "neogeo_draw24", "neogeo_draw25", "neogeo_draw26", "neogeo_draw27",
    "neogeo_draw28", "neogeo_draw29",
    "full_frame",
    // Sega Saturn VDP2 sources plus the combined VDP1 sprite/polygon pass.
    "saturn_back", "saturn_nbg0", "saturn_nbg1", "saturn_nbg2",
    "saturn_nbg3", "saturn_rbg0", "saturn_rbg1", "saturn_vdp1",
    // Taito PC080SN/PC090OJ family (see opwolf.cpp's RetroDepth export block).
    "taito_bg", "taito_fg", "taito_sprites",
    // Namco System 2 family (see namcos2_v.cpp's RetroDepth export block).
    // Also reused by namco/namconb1.cpp (ptblank) via its own local helper.
    "namco_bg", "namco_sprites",
    // Konami K056832/K053244 (lethal.cpp).
    "lethal_bg3", "lethal_bg2", "lethal_bg1", "lethal_sprites", "lethal_text",
    // Taito TC0100SCN (taito_z_v.cpp screen_update_spacegun).
    "spacegun_bg0", "spacegun_bg1", "spacegun_text", "spacegun_sprites",
    // Taito TC0480SCP (gunbustr.cpp, slapshot.cpp/slapshot_v.cpp).
    "tc0480_bg0", "tc0480_bg1", "tc0480_bg2", "tc0480_bg3",
    "tc0480_text", "tc0480_sprites",
    // Unico (unico.cpp).
    "unico_bg", "unico_mid", "unico_fg", "unico_sprites",
    // Misc oneshot.cpp.
    "oneshot_bg", "oneshot_mid", "oneshot_sprites", "oneshot_fg",
    // IGS lordgun_v.cpp.
    "lordgun_tile0", "lordgun_tile1", "lordgun_tile2", "lordgun_tile3",
    "lordgun_sprites",
    // Seta X1-020/dx-101 (deerhunt/trophyh/turkhunt/wschamp).
    "seta2_sprites",
    // Sega Y-board (segaybd.cpp, rchase).
    "segaybd_scene", "segaybd_bsprites",
    // SNK bbusters.cpp (bbusters/mechatt).
    "bbusters_pf1", "bbusters_pf0", "bbusters_sprites1",
    "bbusters_sprites0", "bbusters_fix",
    // Taito nycaptor.cpp.
    "nycaptor_bg0", "nycaptor_bg1", "nycaptor_sprites",
    // Generic shared-draw fallback (slots 58..64).
    "mame_occupxy_0", "mame_occupxy_1", "mame_occupxy_2",
    "mame_occupxy_3", "mame_occupxy_4", "mame_occupxy_5",
    "mame_occupxy_residual",
};

MameBackend* g_active_backend = nullptr;
bool g_core_ever_initialized = false;

constexpr int kAudioRingFrames = 8192;
static int16_t g_audio_ring[kAudioRingFrames * 2];
static std::atomic<int> g_ring_write{0};
static std::atomic<int> g_ring_read{0};
static AAudioStream* g_aaudio_stream = nullptr;
static int g_audio_sample_rate = 44100; // CPS2 QSound default; overwritten from av_info
static retro_audio_buffer_status_callback_t g_audio_buffer_status_callback = nullptr;
static std::string g_system_dir_storage; // backing storage for RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY

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

// Most recent ERROR-level line MAME's own log_cb reported. MAME already knows
// exactly which required file it couldn't find (BIOS/CHD/ROM set) and says so
// in its own log text -- rather than re-deriving that here, just capture it
// and surface it verbatim as the load warning.
std::string g_last_mame_error_log;

static void RETRO_CALLCONV frontend_log_printf(retro_log_level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(android_log_priority(level), kLogTag, fmt, args);
    va_end(args);

    if (level == RETRO_LOG_ERROR) {
        va_list args2;
        va_start(args2, fmt);
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, args2);
        va_end(args2);
        g_last_mame_error_log = buf;
    }
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

static std::string rom_name_from_path(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto base  = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot   = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

} // namespace

void set_mame_system_directory(const std::string& dir) {
    g_system_dir_storage = dir;
}

const std::string& mame_system_directory() {
    return g_system_dir_storage;
}

// ---------------------------------------------------------------------------
// MameBackend
// ---------------------------------------------------------------------------

MameBackend::MameBackend() {
    m_backend_name = "MAME (libretro)";
    m_frame.layers.resize(kMameLayerCount);
    ensure_frame_size(384, 224);
}

MameBackend::~MameBackend() {
    reset_core();
}

const char* MameBackend::backend_name() const { return m_backend_name.c_str(); }

double MameBackend::frame_rate_hz() const { return m_frame_rate_hz; }

bool MameBackend::load_content(const std::string& rom_path, std::string& error_out) {
    reset_core();
    m_last_load_warning.clear();
    g_last_mame_error_log.clear();
    if (rom_path.empty()) { error_out = "MAME: ROM path is empty."; return false; }
    if (!ensure_core_initialized(error_out)) return false;

    // MAME's libretro core wants the containing directory + zip filename as
    // `path` (it does its own zip/rompath resolution internally, including
    // BIOS lookups against RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) rather
    // than pre-read file bytes the way console cores expect.
    retro_game_info game_info{};
    game_info.path = rom_path.c_str();
    game_info.data = nullptr;
    game_info.size = 0;
    game_info.meta = nullptr;
    if (!mame_entry_points().ok) {
        error_out = "MAME: failed to resolve libmame_libretro.so entry points.";
        m_last_load_warning = error_out;
        return false;
    }
    if (!mame_entry_points().retro_load_game(&game_info)) {
        // MAME's own log already names the exact missing file (BIOS/CHD/ROM
        // set); surface that verbatim rather than a generic message.
        error_out = g_last_mame_error_log.empty()
            ? "MAME: retro_load_game failed (ROM/BIOS missing or unsupported set?)."
            : ("MAME: " + g_last_mame_error_log);
        m_last_load_warning = error_out;
        reset_core();
        return false;
    }

    // MAME applies the driver's cabinet orientation to the final libretro
    // framebuffer. RetroDepth driver captures are made before that transform,
    // so remember the same flags and normalize named planes below.
    m_mame_orientation = mame_entry_points().mame_game_orientation
        ? static_cast<uint32_t>(mame_entry_points().mame_game_orientation()) : 0u;

    neogeo_palette_debug_maybe_arm(rom_path);

    retro_system_av_info av_info{};
    mame_entry_points().retro_get_system_av_info(&av_info);
    ensure_frame_size(av_info.geometry.base_width, av_info.geometry.base_height);
    m_frame_rate_hz = (av_info.timing.fps > 0.0) ? av_info.timing.fps : 59.63;
    g_audio_sample_rate = (av_info.timing.sample_rate > 0.0)
        ? static_cast<int>(av_info.timing.sample_rate) : 44100;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "MAME AV: fps=%.4f sample_rate=%.1f geometry=%ux%u",
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
        mame_entry_points().retro_run();
        if (m_video_frame_count > 0) break;
    }

    if (m_video_frame_count == 0) {
        error_out = g_last_mame_error_log.empty()
            ? "MAME: ROM loaded but emitted no video frames."
            : ("MAME: " + g_last_mame_error_log);
        m_last_load_warning = error_out;
        reset_core();
        return false;
    }

    error_out.clear();
    return true;
}

bool MameBackend::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (!m_game_loaded) { error_out = "MAME: no ROM loaded."; return false; }
    m_input = input;
    report_audio_buffer_status();
    mame_entry_points().retro_run();
    error_out.clear();
    return true;
}

const FrameOutput& MameBackend::frame_output() const { return m_frame; }

bool MameBackend::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (!m_game_loaded) { error_out = "MAME: no ROM loaded."; return false; }
    const std::size_t size = mame_entry_points().retro_serialize_size();
    if (size == 0) { error_out = "MAME: zero savestate size."; return false; }
    out.assign(size, 0);
    if (!mame_entry_points().retro_serialize(out.data(), out.size())) {
        out.clear();
        error_out = "MAME: retro_serialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

bool MameBackend::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (!m_game_loaded) { error_out = "MAME: no ROM loaded."; return false; }
    if (!data || size == 0) { error_out = "MAME: savestate data empty."; return false; }
    if (!mame_entry_points().retro_unserialize(data, size)) {
        error_out = "MAME: retro_unserialize failed.";
        return false;
    }
    error_out.clear();
    return true;
}

void MameBackend::set_auto_frame_skip(bool enabled) {
    m_auto_frame_skip = enabled;
}
void MameBackend::set_layer_capture_mask(uint32_t /*mask*/) {
    // Not used -- MAME layer visibility is per-name via mame_layer_capture.h,
    // not a bitmask like the console cores.
}

void MameBackend::set_occupancy_capture_enabled(bool enabled) {
    auto& ep = mame_entry_points();
    if (ep.mame_occupancy_set_enabled)
        ep.mame_occupancy_set_enabled(enabled ? 1 : 0);
    if (!enabled) {
        m_frame.mame_occupancy_valid = false;
        for (int i = 0; i < kMameOccupancyBucketCount; ++i)
            m_frame.layers[kMameOccupancyBase + i].rgba.clear();
    }
}

RomHeaderInfo MameBackend::get_rom_header_info() const {
    RomHeaderInfo info;
    if (m_loaded_rom_path.empty()) return info;
    info.game_name = rom_name_from_path(m_loaded_rom_path);
    info.has_header = !info.game_name.empty();
    return info;
}

const uint32_t* MameBackend::get_z_histogram() const { return nullptr; }

const uint8_t* MameBackend::system_ram_data() const {
    if (!m_game_loaded) return nullptr;
    return static_cast<const uint8_t*>(mame_entry_points().retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

std::size_t MameBackend::system_ram_size() const {
    if (!m_game_loaded) return 0;
    return mame_entry_points().retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
}

bool MameBackend::handle_environment(unsigned cmd, void* data) {
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
        *dir = m_system_dir.empty() ? "." : m_system_dir.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!data) return false;
        // MAME's libretro core requests XRGB8888 -- accept it (our video
        // refresh handler below assumes 32bpp either way).
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
        // MAME's libretro OSD glue (retro_init.cpp's execute_game()) branches
        // its whole argv-construction path on the "mame_media_type" core
        // variable: unset (our previous unconditional `return false` here)
        // leaves its `mediaType` global blank instead of "-rom", which routes
        // into the *software-list* arg path instead of the plain
        // single-driver path -- adding 3 stray positional/unadorned tokens
        // (system name, blank media type, full ROM path) that MAME's own
        // option parser then rejects with "unknown option: -<UNADORNED2>".
        // Standard libretro frontends supply this from MAME's declared
        // core-options defaults; we don't implement full core-options
        // negotiation, so just hardcode the one value that keeps every
        // driver on the simple path (arcade board *and* console-with-BIOS
        // systems like Neo Geo, matched here purely by filename/rompath, not
        // software lists).
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        if (std::strcmp(var->key, "mame_media_type") == 0) {
            var->value = "rom";
            return true;
        }
        // QuestRetroDepth owns this setting rather than exposing MAME's full
        // core-option UI. It is queried by the patched MAME libretro glue
        // before retro_load_game() builds the machine.
        if (std::strcmp(var->key, "mame_autoframeskip") == 0) {
            var->value = m_auto_frame_skip ? "enabled" : "disabled";
            return true;
        }
        // Enables MAME's built-in RETRO_DEVICE_LIGHTGUN polling (retro_osd_interface::
        // process_lightgun_state in libretro-internal/input_retro.cpp) -- without this
        // the core never queries lightgun screen coordinates at all, regardless of what
        // handle_input_state() answers. Applies to any loaded driver with a MAME gun
        // ioport (Saturn's Virtua Gun/Stunner games, Model 2/segas32 arcade gun games,
        // etc); drivers without one simply ignore it.
        if (std::strcmp(var->key, "mame_lightgun_mode") == 0) {
            var->value = "lightgun";
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

void MameBackend::handle_video_frame(
    const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;

    ++m_video_frame_count;
    ensure_frame_size(width, height);

    // MAME's libretro core outputs XRGB8888 (0xXXRRGGBB, top byte ignored).
    // FrameOutput stores the same logical channel positions as ARGB8888; the
    // later LayerProcessor/shelf conversion unpacks those positions to the
    // GL_RGBA byte order.  Keep the source word intact here.  A red/blue swap
    // at this point makes the Metal Slug image visibly blue-tinted.
    const auto* src32 = static_cast<const uint32_t*>(data);
    const std::size_t stride_pixels = pitch / sizeof(uint32_t);
    for (unsigned y = 0; y < height; ++y) {
        const uint32_t* row = src32 + (std::size_t)y * stride_pixels;
        uint32_t* dst = m_frame.rgba8888.data() + (std::size_t)y * width;
        for (unsigned x = 0; x < width; ++x) {
            dst[x] = 0xFF000000u | (row[x] & 0x00FFFFFFu);
        }
    }

    pull_named_layers();
}

bool MameBackend::ensure_core_initialized(std::string& error_out) {
    g_active_backend = this;

    // BIOS search covers all 3 requested locations, with only one of them
    // needing anything from us:
    //   1. sdcard bios/ folder -- reported here via
    //      RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY; MAME's own
    //      Set_Path_Option() (third_party/mame_libretro/src/osd/libretro/
    //      libretro-internal/retro_init.cpp) appends "/mame/bios" and
    //      "/mame/roms" to whatever directory we report, so
    //      set_mame_system_directory() (called from
    //      configure_mgba_frontend_dirs_from_activity() in
    //      questretrodepth_main.cpp, despite the mgba-specific name) points
    //      this at the same root all the other cores' per-system folders
    //      live under.
    //   2. The loaded ROM's own directory -- MAME derives this itself from
    //      the game path (g_rom_dir) and always puts it first in rompath.
    //      Nothing to do here.
    //   3. Inside the loaded ROM's own zip -- native MAME zip reading; a
    //      "merged" romset with its BIOS packed alongside the game just
    //      works. Nothing to do here either.
    m_system_dir = g_system_dir_storage;

    auto& ep = mame_entry_points();
    if (!ep.ok) {
        error_out = "MAME: failed to resolve libmame_libretro.so entry points.";
        return false;
    }
    ep.retro_set_environment(frontend_environment);
    ep.retro_set_video_refresh(frontend_video_refresh);
    ep.retro_set_audio_sample(frontend_audio_sample);
    ep.retro_set_audio_sample_batch(frontend_audio_sample_batch);
    ep.retro_set_input_poll(frontend_input_poll);
    ep.retro_set_input_state(frontend_input_state);
    if (!g_core_ever_initialized) {
        ep.retro_init();
        g_core_ever_initialized = true;
    }
    ep.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    m_core_initialized = true;
    error_out.clear();
    return true;
}

void MameBackend::reset_core() {
    close_aaudio_stream();
    if (mame_entry_points().mame_occupancy_set_enabled)
        mame_entry_points().mame_occupancy_set_enabled(0);
    if (m_game_loaded && mame_entry_points().ok) {
        mame_entry_points().retro_unload_game();
    }
    m_core_initialized = false;
    m_game_loaded      = false;
    m_loaded_rom_path.clear();
    m_video_frame_count = 0;
    m_last_mame_frame_id = 0xFFFFFFFFu;
    m_mame_orientation = 0u;
    m_mame_one_layer_streak = 0;
    g_audio_buffer_status_callback = nullptr;
    m_frame.zbuffer.clear();
    m_frame.visible_source_id.clear();
    m_frame.mame_occupancy_available = false;
    m_frame.mame_occupancy_valid = false;
    m_frame.mame_occupancy_eligible = false;
    m_frame.layers.resize(kMameLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
    if (g_active_backend == this) g_active_backend = nullptr;
}

void MameBackend::ensure_frame_size(unsigned width, unsigned height) {
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
    m_frame.mame_occupancy_available = false;
    m_frame.mame_occupancy_valid = false;
    m_frame.mame_occupancy_eligible = false;
    m_frame.layers.resize(kMameLayerCount);
    for (auto& layer : m_frame.layers) { layer.rgba.clear(); layer.depth_map.clear(); }
}

void MameBackend::pull_named_layers() {
    // mame_frame_id()/mame_layer_count() only ever advance for drivers whose
    // family ships a mame_retrodepth_hook.h (capcom/konami/sega/dataeast/
    // toaplan/snk) -- for every other family (taito/namco/seta/gaelco/igs/
    // williams/atari/unico/...) mame_frame_id() is stuck at its initial
    // value forever, since nothing ever calls through to bump it. Gating the
    // whole function -- including the full-frame fallback below -- on that
    // staleness check meant those drivers ran this exactly once (on frame 0,
    // typically still black during boot) and then froze forever while
    // m_frame.rgba8888 kept updating live in the background: a real, opaque
    // black screen that "has_pixels" reported as normal since alpha stayed
    // 0xFF. Only skip the (potentially expensive) named-layer walk when the
    // hook signals a genuinely new committed frame; the full-frame fallback
    // has no such redundancy concern and must run unconditionally every call.
    const uint32_t frame_id = mame_frame_id();
    const bool have_new_hook_frame = (frame_id != m_last_mame_frame_id);
    m_last_mame_frame_id = frame_id;

    bool accepted_named_layer = false;
    int usable_named_layer_count = 0;
    const int count = have_new_hook_frame ? mame_layer_count() : 0;
    if (have_new_hook_frame) {
        for (int li = 0; li < kMameLayerCount; ++li) {
            m_frame.layers[li].rgba.clear();
            m_frame.layers[li].depth_map.clear();
        }
    } else {
        // No fresh hook frame this call -- a named layer was accepted on a
        // prior call iff it still holds pixel data now (untouched below).
        for (int li = 0; li < kMameLayerCount; ++li) {
            if (li < kMameOccupancyBase && li != kMameFullFrameLayerIndex &&
                !m_frame.layers[li].rgba.empty()) {
                accepted_named_layer = true;
                ++usable_named_layer_count;
            }
        }
    }
    for (int i = 0; i < count; ++i) {
        const char* name = mame_layer_name(i);
        int dst_index = -1;
        for (int li = 0; li < kMameLayerCount; ++li) {
            if (std::strcmp(name, kMameLayerNames[li]) == 0) { dst_index = li; break; }
        }
        if (dst_index < 0) continue; // unknown layer name -- ignore

        uint32_t lw = 0, lh = 0;
        const uint32_t* pixels = mame_layer_pixels(i, &lw, &lh);
        if (!pixels || lw != m_frame.width || lh != m_frame.height) continue;

        accepted_named_layer = true;
        ++usable_named_layer_count;

        auto& out = m_frame.layers[dst_index].rgba;
        out.resize((std::size_t)lw * lh);
        // mame_retrodepth_hook.h writes logical ARGB8888 (0xAARRGGBB, alpha
        // 0/1 sentinel for transparent per the desktop patch convention).
        // LayerCapture is consumed by LayerProcessor via memcpy as RGBA bytes,
        // so store the equivalent packed little-endian word 0xAABBGGRR here.
        // The composed FrameOutput remains logical 0xAARRGGBB and is converted
        // separately by LayerProcessor::to_rgba(). Keeping these two contracts
        // distinct fixes the live-only red/blue swap while preserving the
        // thumbnail conversion.
        // ORIENTATION_SWAP_XY (0x4, e.g. ROT90/ROT270 -- 1941 and other
        // natively-vertical CPS boards) pairs with FLIP_X/FLIP_Y in MAME's
        // raw driver rotation flags, but a width/height swap never happens
        // here (lw/lh stay the driver's native un-rotated dims -- see the
        // lw/lh == m_frame.width/height guard above). Applying just the
        // flip half of a SWAP_XY orientation actively corrupts the capture
        // (X-mirrors an image that still needs a 90-degree turn, not a
        // mirror) instead of leaving it as the untouched, correctly-oriented
        // source the app's own Rotate Screen setting expects to rotate. Only
        // apply flip_x/flip_y for pure-flip orientations (ROT0/ROT180).
        const bool swap_xy = (m_mame_orientation & 0x4u) != 0; // ORIENTATION_SWAP_XY
        const bool flip_x = !swap_xy && (m_mame_orientation & 0x1u) != 0; // ORIENTATION_FLIP_X
        const bool flip_y = !swap_xy && (m_mame_orientation & 0x2u) != 0; // ORIENTATION_FLIP_Y
        for (uint32_t y = 0; y < lh; ++y) {
            for (uint32_t x = 0; x < lw; ++x) {
                const uint32_t source_x = flip_x ? (lw - 1u - x) : x;
                const uint32_t source_y = flip_y ? (lh - 1u - y) : y;
                const std::size_t source = static_cast<std::size_t>(source_y) * lw + source_x;
                const std::size_t dest = static_cast<std::size_t>(y) * lw + x;
                const uint32_t argb = pixels[source];
                const uint32_t a = (argb >> 24) & 0xFFu;
                if (a <= 1u) { out[dest] = 0u; continue; } // desktop's transparent sentinel (0x00000001)
                const uint32_t r = (argb >> 16) & 0xFFu;
                const uint32_t g = (argb >> 8)  & 0xFFu;
                const uint32_t b = argb & 0xFFu;
                out[dest] = (a << 24) | (b << 16) | (g << 8) | r;
            }
        }
    }

    // Manual OCCUPXY is offered only after a sustained observation that the
    // active MAME title exposes zero or one usable named layer. Native Neo Geo
    // remains on its own driver-specific path and is never treated as generic.
    if (usable_named_layer_count <= 1) {
        m_mame_one_layer_streak = std::min(m_mame_one_layer_streak + 1, 30);
    } else {
        m_mame_one_layer_streak = 0;
    }
    m_frame.mame_occupancy_eligible = m_mame_one_layer_streak >= 30;

    pull_synthesized_zbuffer();
    pull_occupancy_layers();
    // The complete image is a fallback for drivers without a named export.
    // Do not also publish it when independent planes are available: rendering
    // both produces an opaque duplicate of the scene (and makes any orientation
    // mismatch look like an extra mirrored layer).
    if (!accepted_named_layer &&
        m_frame.rgba8888.size() == static_cast<std::size_t>(m_frame.width) * m_frame.height) {
        // rgba8888 packs each pixel as 0xAARRGGBB (memory order B,G,R,A -- see
        // handle_video_frame()). The PerLayerCapture consumer expects memory
        // order R,G,B,A instead (the same reorder pull_named_layers applies
        // above for hooked layers), so this needs the same swap, not a raw copy.
        auto& out = m_frame.layers[kMameFullFrameLayerIndex].rgba;
        out.resize(m_frame.rgba8888.size());
        for (std::size_t p = 0; p < out.size(); ++p) {
            const uint32_t argb = m_frame.rgba8888[p];
            const uint32_t a = (argb >> 24) & 0xFFu;
            const uint32_t r = (argb >> 16) & 0xFFu;
            const uint32_t g = (argb >> 8)  & 0xFFu;
            const uint32_t b = argb & 0xFFu;
            out[p] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
    neogeo_palette_debug_tick(m_frame);
}

void MameBackend::pull_occupancy_layers() {
    auto& ep = mame_entry_points();
    const bool api = ep.mame_occupancy_available &&
                     ep.mame_occupancy_valid &&
                     ep.mame_occupancy_bucket_count &&
                     ep.mame_occupancy_bucket_pixels;
    m_frame.mame_occupancy_available = api && ep.mame_occupancy_available() != 0;
    m_frame.mame_occupancy_valid = false;
    if (!m_frame.mame_occupancy_available || !ep.mame_occupancy_valid()) {
        for (int i = 0; i < kMameOccupancyBucketCount; ++i)
            m_frame.layers[kMameOccupancyBase + i].rgba.clear();
        return;
    }

    const int count = std::min(kMameOccupancyBucketCount, ep.mame_occupancy_bucket_count());
    bool all_valid = count == kMameOccupancyBucketCount;
    for (int i = 0; i < kMameOccupancyBucketCount; ++i) {
        auto& out = m_frame.layers[kMameOccupancyBase + i].rgba;
        out.clear();
        if (i >= count) continue;
        uint32_t w = 0, h = 0;
        const uint32_t* pixels = ep.mame_occupancy_bucket_pixels(i, &w, &h);
        if (!pixels || w != m_frame.width || h != m_frame.height) {
            all_valid = false;
            continue;
        }
        const std::size_t npix = (std::size_t)w * h;
        out.resize(npix);
        for (std::size_t p = 0; p < npix; ++p) {
            const uint32_t argb = pixels[p];
            const uint32_t a = (argb >> 24) & 0xFFu;
            out[p] = a <= 1u ? 0u : (a << 24) |
                   ((argb & 0xFFu) << 16) |
                   (argb & 0x0000FF00u) |
                   ((argb >> 16) & 0xFFu);
        }
    }
    m_frame.mame_occupancy_valid = all_valid;
}

void MameBackend::pull_synthesized_zbuffer() {
    // Neo Geo has no separable hardware layers, so neogeo_v.cpp fabricates a
    // per-pixel depth channel instead of exporting named layers. Feeding it
    // through FrameOutput::zbuffer puts it on the exact path snes9x's real
    // z-buffer already uses: GameConfig::update_z_splits() derives one layer
    // per distinct depth value it finds, so the layer count and the split
    // points come from the data rather than being hardcoded here.
    uint32_t zw = 0, zh = 0;
    const uint8_t* z = mame_zbuffer(&zw, &zh);
    static bool s_logged_once = false;
    if (!z || zw != m_frame.width || zh != m_frame.height) {
        if (!s_logged_once) {
            s_logged_once = true;
            __android_log_print(ANDROID_LOG_INFO, "QuestRetroDepth",
                "zbuffer dim mismatch: z=%p zw=%u zh=%u frame=%ux%u",
                (const void*)z, zw, zh, m_frame.width, m_frame.height);
        }
        m_frame.zbuffer.clear();
        return;
    }
    if (!s_logged_once) {
        s_logged_once = true;
        __android_log_print(ANDROID_LOG_INFO, "QuestRetroDepth",
            "zbuffer OK: %ux%u", zw, zh);
    }
    m_frame.zbuffer.assign(z, z + (std::size_t)zw * zh);
}

int16_t MameBackend::handle_input_state(
    unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port == 0 && device == RETRO_DEVICE_LIGHTGUN) {
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:    return m_input.gun_active ? m_input.gun_screen_x : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:    return m_input.gun_active ? m_input.gun_screen_y : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return (m_input.gun_active && m_input.gun_offscreen) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:     return (m_input.gun_active && m_input.gun_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:      return (m_input.gun_active && m_input.gun_reload) ? 1 : 0;
        default: return 0;
        }
    }
    // Port 1 is player two's gun while dual-wielding. MAME polls every player
    // port's lightgun each frame regardless, so this must report offscreen
    // rather than a silent zero when there is no second gun -- a zero would
    // read as player two aiming dead centre.
    if (port == 1 && device == RETRO_DEVICE_LIGHTGUN) {
        const bool live = m_dual_gun_mode && m_input.gun2_active;
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:     return live ? m_input.gun2_screen_x : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:     return live ? m_input.gun2_screen_y : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return (!live || m_input.gun2_offscreen) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:      return (live && m_input.gun2_trigger) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:       return (live && m_input.gun2_reload) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_A:        return (live && m_input.gun2_button_a) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_B:        return (live && m_input.gun2_button_b) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_START:        return (live && m_input.gun2_button_start) ? 1 : 0;
        case RETRO_DEVICE_ID_LIGHTGUN_SELECT:       return (live && m_input.gun2_button_select) ? 1 : 0;
        default: return 0;
        }
    }
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
