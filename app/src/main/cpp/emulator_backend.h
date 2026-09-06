#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "psx_depth_frame.h"

namespace qrd {

enum class BackendKind {
    Snes,
    Genesis,
    Gba,
    Gb,    // Game Boy / Game Boy Color (also via mGBA)
    Nes,   // NES (FCEUmm)
    Pce,     // PC Engine / TurboGrafx-16 (beetle-pce-fast)
    Sms,     // Sega Master System / Game Gear (PicoDrive, but SMS RAM range differs from Genesis)
    Mame,    // Arcade (MAME libretro core) — CPS1/CPS2 to start
    Saturn,  // Sega Saturn (Mednafen/Beetle Saturn)
    Psx,     // Sony PlayStation (SwanStation, the libretro DuckStation fork)
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
    // Sega Saturn pad has two extra face buttons beyond A/B/X/Y; ignored by
    // every other backend.
    bool button_c = false;
    bool button_z = false;
    int16_t mouse_x = 0;
    int16_t mouse_y = 0;
    bool mouse_left_button = false;
    bool mouse_right_button = false;

    // Lightgun aiming (Super Scope / Zapper / Saturn Virtua Gun / MAME gun games).
    // gun_active gates whether the backend should present a lightgun device to the
    // core at all this frame; screen_x/y follow the libretro RETRO_DEVICE_ID_LIGHTGUN_
    // SCREEN_X/Y convention (-0x7fff..0x7fff across the visible game screen).
    bool gun_active = false;
    int16_t gun_screen_x = 0;
    int16_t gun_screen_y = 0;
    bool gun_offscreen = false;
    bool gun_trigger = false;
    bool gun_reload = false;

    // Second lightgun, for dual-wielding: one controller per player. Two-player
    // gun games expect a gun in each controller port, so this is not a trick —
    // it is what the hardware presented, and each port carries its own trigger
    // and Start. Only backends that expose a second port act on it; the fields
    // stay inert (gun2_active false) for single-gun play, so nothing changes
    // for backends that ignore them.
    bool gun2_active = false;
    int16_t gun2_screen_x = 0;
    int16_t gun2_screen_y = 0;
    bool gun2_offscreen = false;
    bool gun2_trigger = false;
    bool gun2_reload = false;
    // Port-1 face buttons. Player two needs its own Start to join and pause,
    // which is the whole point of giving it a real port rather than sharing
    // player one's inputs.
    bool gun2_button_start = false;
    bool gun2_button_select = false;
    bool gun2_button_a = false;
    bool gun2_button_b = false;
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


struct FrameOutput {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba8888;
    // Per-pixel snes9x z-buffer values (0..63).
    // Higher = higher priority / closer to viewer.
    // OBJ ≈ 48, BG high ≈ 46-47, BG low ≈ 35-43, backdrop = 1.
    // Same dimensions as rgba8888; empty when not available.
    std::vector<uint8_t> zbuffer;
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

    // MAME-only generic OCCUPXY status. Kept with the frame so the XR thread
    // can reject stale/invalid capture data atomically.
    bool mame_occupancy_available = false;
    bool mame_occupancy_valid = false;
    bool mame_occupancy_eligible = false;

    // Per-pixel depth for the PSX depth-displaced screen. Other backends leave
    // this empty and continue using their normal output.
    std::shared_ptr<const PsxDepthFrame> psx_depth;
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
    // When true, load_content() must skip opening real audio hardware output.
    // Used by background ROM preview generation, which never needs sound.
    // `allow_audio` overrides that muting — used by the live hover preview,
    // which (unlike background caching) is a single foreground ROM actually
    // being watched, so it's worth letting it actually play sound.
    virtual void set_preview_mode(bool enabled, bool allow_audio = false) {}
    virtual void set_layer_capture_mask(uint32_t mask) = 0;
    // Enables the optional generic MAME draw-path capture. No-op elsewhere.
    virtual void set_occupancy_capture_enabled(bool /*enabled*/) {}
    // Selects the PSX rendering path (see VrState::psx_render_path). No-op
    // on every other backend.
    virtual void set_psx_render_path(int /*path*/) {}
    virtual RomHeaderInfo get_rom_header_info() const = 0;
    // Get z-buffer histogram. Returns nullptr if histogram not available.
    virtual const uint32_t* get_z_histogram() const = 0;
    virtual const uint8_t* system_ram_data() const = 0;
    virtual std::size_t system_ram_size() const = 0;

    virtual void on_emu_freeze()   {}
    virtual void on_emu_unfreeze() {}

    // Switches the backend's lightgun-capable port between a normal joypad
    // and its lightgun peripheral (SNES Super Scope, NES Zapper). No-op on
    // backends with no such peripheral, and on MAME (whose lightgun support
    // is always-on via a core option and self-gates per driver ioport).
    // `peripheral` is backend-specific: SNES uses snes_gun_peripheral_id() (0=Super
    // Scope, 1=Justifier); ignored by backends with only one gun peripheral type.
    virtual void set_gun_mode(bool enabled, int peripheral = 0) {}

    // Plugs a second lightgun in, so a two-player gun title sees a real player
    // two with its own aim and trigger (QRD's dual-wield mode). Where that
    // second gun lives is per-system: PSX puts a second GunCon in port 1, SNES
    // daisy-chains a second Justifier off port 1 (Super Scope has no
    // two-player mode at all), Saturn takes a second Virtua Gun in port 1, and
    // MAME just polls a lightgun per player port. Backends with no second gun
    // to offer ignore this — see backend_supports_dual_gun(), which is what
    // the UI asks before offering the option.
    virtual void set_dual_gun_mode(bool /*enabled*/) {}

    // Soft-resets the running game (equivalent to a console reset button), without
    // reloading ROM data or clearing loaded-backend state. Many gun-peripheral
    // titles only probe port 1's device type at boot/reset and never re-poll it
    // afterward, so toggling set_gun_mode() at runtime (e.g. the player's manual
    // lightgun override) needs a follow-up soft_reset() to actually take effect.
    // No-op on backends that don't support it.
    virtual void soft_reset() {}

    // Non-fatal, human-readable warning from the most recent load_content()
    // call -- e.g. a missing BIOS/firmware file the core needed. Empty when
    // there's nothing to report. Backends that have no such concept (most of
    // them) just use the default. Checked once right after a ROM load to
    // decide what the post-load hint tooltip should say.
    virtual std::string last_load_warning() const { return {}; }
};

std::unique_ptr<EmulatorBackend> create_backend(BackendKind kind);
const char* backend_kind_name(BackendKind kind);

// True if rom_stem (any case, extension optional) is a known lightgun-peripheral
// title for the given backend: SNES Super Scope games, NES Zapper games, and
// MAME/Saturn Virtua Gun & Stunner gun games. Used to decide whether to switch
// the loaded backend into gun mode (set_gun_mode()) and whether the VR shell
// should raycast the right controller against the game screen for aiming.
bool rom_is_lightgun_capable(BackendKind kind, const std::string& rom_stem);

// True if a second lightgun can actually be plugged in for this backend/ROM —
// i.e. the core has a real second gun device the game will read, not just a
// second controller QRD could aim. Systems answer differently: PSX, Saturn and
// MAME can host two guns for any gun title; SNES only for Justifier games
// (Lethal Enforcers, T2 — the Super Scope has no two-player mode); the NES
// Zapper and everything else, not at all. Gates the "Two Guns" option so it is
// never offered where it would do nothing. See EmulatorBackend::set_dual_gun_mode().
bool backend_supports_dual_gun(BackendKind kind, const std::string& rom_stem);

// True if rom_stem is a SNES title that uses the Konami Justifier peripheral
// (Lethal Enforcers, T2: The Arcade Game) instead of the Super Scope. The two
// are different lightgun peripherals with incompatible on-console protocols --
// connecting the wrong one to snes9x makes the game see *a* gun but never
// read a sane position from it. See snes_gun_peripheral_id().
bool rom_is_snes_justifier_title(const std::string& rom_stem);

// SNES lightgun peripheral subclass ids for SnesLibretroBackend::set_gun_mode().
inline constexpr int kSnesGunSuperScope = 0; // default, most titles
inline constexpr int kSnesGunJustifier  = 1;

inline int snes_gun_peripheral_id(const std::string& rom_stem) {
    return rom_is_snes_justifier_title(rom_stem) ? kSnesGunJustifier : kSnesGunSuperScope;
}

// Convenience for call sites that call set_gun_mode() for any backend kind:
// the peripheral id for SNES, 0 (ignored) for everything else.
inline int rom_gun_peripheral(BackendKind kind, const std::string& rom_stem) {
    return kind == BackendKind::Snes ? snes_gun_peripheral_id(rom_stem) : 0;
}

} // namespace qrd
