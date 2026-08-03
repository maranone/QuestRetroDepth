#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qrd {

enum class BackendKind {
    Snes,
    Genesis,
    Gba,
    Gb,    // Game Boy / Game Boy Color (also via mGBA)
    Nes,   // NES (FCEUmm)
    Pce,     // PC Engine / TurboGrafx-16 (beetle-pce-fast)
    Sms,     // Sega Master System / Game Gear (PicoDrive, but SMS RAM range differs from Genesis)
    ScummVm, // Classic point-and-click adventures via ScummVM libretro
};

struct EmulatorInputState {
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;
    bool button_a = false;
    bool button_b = false;
    bool button_x = false;
    bool button_y = false;
    bool button_l = false;
    bool button_r = false;
    bool button_start = false;
    bool button_select = false;
    // Mouse / pointer input (used by ScummVM for point-and-click).
    // Absolute game-pixel coordinates; backend converts to per-frame deltas internally.
    int16_t mouse_x = 0;
    int16_t mouse_y = 0;
    bool mouse_left_button = false;
    bool mouse_right_button = false;
};

// Per-layer pixel frame captured during backend rendering.
// rgba: one uint32 (RGBA8888) per pixel, alpha=255 for opaque, alpha=0 for transparent.
// width/height match FrameOutput::width/height.
// depth_map: per-pixel Y-depth hint for sprite layers; 0=top of screen (far), 255=bottom (close).
//            Empty for BG layers or when sprite_y_depth is not populated.
struct LayerCapture {
    std::vector<uint32_t> rgba;      // opaque where a tile was drawn; transparent otherwise
    std::vector<uint8_t>  depth_map; // Y-depth hint for sprite layers; empty when unused
};

// Lightweight per-layer output for backends that build their own layer stack.
// ScummVM backend populates FrameOutput::native_layers with these; the render side
// converts them to LayerFrame objects, bypassing LayerProcessor.
// This lives here (not in layer_processor.h) to avoid a circular include.
struct NativeLayerFrame {
    std::string id;
    float depth_meters      = 2.5f;
    float quad_width_meters = 2.56f;
    int   width             = 0;
    int   height            = 0;
    std::vector<uint8_t> rgba; // RGBA bytes (R=byte0, G=byte1, B=byte2, A=byte3)
    bool is_ui_bar = false; // ScummVM: render detached below game screen at front depth
};

struct FrameOutput {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba8888;
    // Per-pixel snes9x z-buffer values (0..63).
    // Higher = higher priority / closer to viewer.
    // OBJ ≈ 48, BG high ≈ 46-47, BG low ≈ 35-43, backdrop = 1.
    // Same dimensions as rgba8888; empty when not available.
    std::vector<uint8_t> zbuffer;
    // Composite-frame Y-depth map for FullFrame extraction path (e.g. ScummVM).
    // 0 = top of screen (far), 255 = bottom (near). Same dims as rgba8888; empty when unused.
    std::vector<uint8_t> depth_map;
    // Per-layer captures for the active backend.
    // SNES uses 5 captures: BG0-BG3, OBJ.
    // Genesis uses 7 captures: background, plane_b_low/high, plane_a_low/high, sprites_low/high.
    // Each non-transparent pixel carries the raw layer color. Empty when not available.
    std::vector<LayerCapture> layers;
    // Main-screen ownership of the final visible pixel.
    // SNES: 0-3 = BG0-BG3, 4 = OBJ, 5 = backdrop.
    // Genesis: 0 = background, 1/2 = plane B low/high,
    // 3/4 = plane A+window low/high, 5/6 = sprites low/high.
    // 255 = none / unavailable.
    std::vector<uint8_t> visible_source_id;
    // ScummVM per-actor native layer frames. Non-empty when ScummVmBackend builds its
    // own layer stack (bypasses LayerProcessor on the render side).
    std::vector<NativeLayerFrame> native_layers;
};

struct RomHeaderInfo {
    std::string game_name;
    bool has_header = false;
};

class EmulatorBackend {
public:
    virtual ~EmulatorBackend() = default;

    virtual const char* backend_name() const = 0;
    virtual double frame_rate_hz() const = 0;
    virtual bool load_content(const std::string& rom_path, std::string& error_out) = 0;
    virtual bool step_frame(const EmulatorInputState& input, std::string& error_out) = 0;
    virtual const FrameOutput& frame_output() const = 0;
    virtual bool save_state(std::vector<uint8_t>& out, std::string& error_out) = 0;
    virtual bool load_state(const void* data, std::size_t size, std::string& error_out) = 0;
    virtual void set_auto_frame_skip(bool enabled) = 0;
    virtual void set_layer_capture_mask(uint32_t mask) = 0;
    virtual RomHeaderInfo get_rom_header_info() const = 0;
    // Get z-buffer histogram. Returns nullptr if histogram not available.
    virtual const uint32_t* get_z_histogram() const = 0;
    virtual const uint8_t* system_ram_data() const = 0;
    virtual std::size_t system_ram_size() const = 0;

    virtual void on_emu_freeze()   {}
    virtual void on_emu_unfreeze() {}
};

std::unique_ptr<EmulatorBackend> create_backend(BackendKind kind);
const char* backend_kind_name(BackendKind kind);

} // namespace qrd
