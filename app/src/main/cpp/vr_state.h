#pragma once
#include "game_config.h"
#include <algorithm>
#include <random>
#include <vector>
#include <cmath>

enum class DepthMode : int {
    Off = 0,
    WholeLayer = 1,
    BoundingBox = 2,
    // Per-pixel-run real extrusion: the whole layer is voxelized into thin real-geometry boxes,
    // one per contiguous run of opaque pixels along whichever axis (rows or columns) produces
    // fewer boxes that frame — so every opaque pixel gets an actual solid side, and every
    // transparent pixel is a real hole (no box drawn there), unlike BoundingBox's one-box-per-
    // detected-blob (which only gives each *blob* real sides, not each pixel).
    PixelExtrude = 3,
    // Per-object placement from the emulator's frame-wide z-buffer. Uses the same
    // PixelExtrude object/run geometry, but each object receives its own mapped depth.
    ZBuffer = 4,
    // PixelExtrude geometry with fixed directional diffuse lighting and ambient floor.
    PixelFx = 5,
};

// Values are persisted in settings files — append new modes, never renumber.
// Off = plain texture sample; PixelArt = the original "sharpened bilinear"
// edge-aware reconstruction; Fsr = AMD FSR1's RCAS contrast-adaptive sharpen
// (RCAS only — spatial upscale is already handled by the layer quad's UV
// stretch, so no separate EASU pass/FBO is needed).
enum class UpscaleMode : int {
    Off      = 0,
    PixelArt = 1,
    Fsr      = 2,
};

inline const char* upscale_mode_label(UpscaleMode mode) {
    switch (mode) {
        case UpscaleMode::PixelArt: return "PIXEL ART";
        case UpscaleMode::Fsr:      return "FSR";
        case UpscaleMode::Off:
        default:                    return "OFF";
    }
}

inline UpscaleMode cycle_upscale_mode(UpscaleMode mode, int dir) {
    static const UpscaleMode k_order[3] = {
        UpscaleMode::Off, UpscaleMode::PixelArt, UpscaleMode::Fsr,
    };
    int cur = 0;
    for (int i = 0; i < 3; ++i) {
        if (k_order[i] == mode) { cur = i; break; }
    }
    const int step = (dir == 0) ? 1 : dir;
    return k_order[((cur + step) % 3 + 3) % 3];
}

// Values are persisted in settings files — append new modes, never renumber.
enum class EnvironmentSphereMode : int {
    Off = 0,
    SkyOnly = 1,    // upper hemisphere, coloured from the image's top half
    FullSphere = 2, // full 180° pole to pole, coloured from the whole image
    Ground = 3,     // lower hemisphere, coloured from the image's bottom half
};

enum class AmbilightPlacement : int { Screen = 0, Floor = 1, Ceiling = 2, All = 3 };
inline const char* ambilight_placement_label(AmbilightPlacement p) {
    switch (p) {
        case AmbilightPlacement::Floor: return "FLOOR";
        case AmbilightPlacement::Ceiling: return "CEILING";
        case AmbilightPlacement::All: return "ALL";
        default: return "SCREEN";
    }
}

// UI cycle order (Off → Sky → Ground → Full), independent of the stored enum values.
inline EnvironmentSphereMode cycle_environment_sphere_mode(EnvironmentSphereMode mode, int dir) {
    static const EnvironmentSphereMode k_order[4] = {
        EnvironmentSphereMode::Off,
        EnvironmentSphereMode::SkyOnly,
        EnvironmentSphereMode::Ground,
        EnvironmentSphereMode::FullSphere,
    };
    int cur = 0;
    for (int i = 0; i < 4; ++i) {
        if (k_order[i] == mode) { cur = i; break; }
    }
    const int step = (dir == 0) ? 1 : dir;
    return k_order[((cur + step) % 4 + 4) % 4];
}

inline const char* depth_mode_label(DepthMode mode) {
    switch (mode) {
        case DepthMode::WholeLayer:   return "LAYER";
        case DepthMode::BoundingBox:  return "BBOX";
        case DepthMode::PixelExtrude: return "PIXEL";
        case DepthMode::ZBuffer:      return "ZBUF";
        case DepthMode::PixelFx:      return "PIXEL FX";
        case DepthMode::Off:
        default:                      return "OFF";
    }
}

inline bool is_pixel_geometry_mode(DepthMode mode) {
    return mode == DepthMode::PixelExtrude || mode == DepthMode::PixelFx;
}

inline const char* environment_sphere_mode_label(EnvironmentSphereMode mode) {
    switch (mode) {
        case EnvironmentSphereMode::SkyOnly:   return "SKY";
        case EnvironmentSphereMode::Ground:    return "GROUND";
        case EnvironmentSphereMode::FullSphere:return "FULL";
        case EnvironmentSphereMode::Off:
        default:                               return "OFF";
    }
}

// -----------------------------------------------------------------------
// Complete VR visual state — every knob that can be randomised or tweaked
// -----------------------------------------------------------------------
struct VrState {
    // Color grading
    float gamma      = 1.5f;
    float contrast   = 1.05f;
    float saturation = 1.00f;
    float brightness = 1.00f;

    // Geometry effects
    float roundness    = 0.0f;   // always 0 — disabled
    float screen_curve = 0.0f;   // always 0 — disabled

    // Subtle screen tilt (radians)
    float tilt_x = 0.0f;
    float tilt_y = 0.0f;

    // Feature flags
    bool immersive_beta_enabled = false; // master flag for beta immersive presentation work
    // Always render through the tessellated-strip "immersive" render path
    // (see gles_renderer.cpp's m_curve_vao + immersive shader program), even
    // when screen_curve is 0 -- ported from the rplaceRT sibling project,
    // where switching to that path fixed a real head-tracking judder/nausea
    // issue versus the plain quad path. Re-enabled after determining the
    // earlier regression was actually caused by the auto-pause-on-menu
    // behavior (see the disabled block in openxr_shell.cpp's render loop),
    // not by this. Default on since it's a strict quality improvement with
    // no known downside.
    bool permacurve = true;
    bool layers_3d         = false; // depth-write on copies (volumetric)
    bool solid_stack       = false; // disabled — always off
    DepthMode depth_mode   = DepthMode::PixelExtrude; // off / whole-layer wedge / bbox-per-object wedge / per-pixel voxel extrusion / pixel fx (lit)
    // When DepthMode::BoundingBox is active: build a real 5-face box (back + 4 sides) per
    // detected object instead of a stack of camera-facing card copies. The stack only reads
    // correctly from roughly in front of it; a real box has an actual side to look at from an
    // angle, which matters once the player can walk around instead of just facing forward.
    // Default on — strict quality improvement, no known downside. Not yet in the settings UI.
    bool real_geometry_boxes = true;
    // When on, a box's side faces follow the sprite's actual per-row/per-column silhouette
    // outline (see LayerFrame::edge_lr/edge_tb) instead of one fixed edge column repeated —
    // e.g. Mario's side reads as his real shape (head/shoulders/legs), not a mostly-transparent
    // sliver. Off by default: extra per-layer textures + a per-frame CPU scan, opt-in.
    bool silhouette_sides = false;
    // Master switch for ROM previews (background thumbnail caching and the
    // Library list's hover-dwell live preview + its world-space depth
    // diorama). Off means the browser is plain text rows and the preview
    // worker never runs.
    bool rom_preview_enabled = true;
    // For natively-portrait arcade boards (e.g. 1941) that would otherwise
    // render squished into the normal landscape quad -- rotates the sampled
    // texture and swaps the quad's aspect ratio to match. 0=off, 1=90,
    // 2=180, 3=270 degrees. See uRotateMode in gles_renderer.cpp's
    // kLayerVS/kImmersiveLayerVS.
    int rotate_screen = 0;
    // Cocktail-cabinet presentation: lowers and tilts the canvas so the
    // player looks down at it like a table screen, instead of the normal
    // front-facing vertical panel. Applied/restored via saved canvas_y/
    // canvas_el snapshots -- see OpenXrShell's table-mode toggle handling.
    // Flat-surface presentation: 0 = Off (normal upright canvas),
    // 1 = Table (cocktail cabinet, face up, look down),
    // 2 = Ceiling (face down, look up). Table and Ceiling share all of
    // their placement code and differ only by which way the surface faces,
    // which way the layer stack grows, and how high it sits.
    // Replaces the old bool table_mode; settings_load still accepts that
    // key so existing .ini files keep working.
    int surface_mode = 0;
    // Menu background music (Kotlin-side MediaPlayer over assets/bgm/*.ogg,
    // see QuestVrActivity.kt's bgmEnable()/bgmDisable()). Persisted so the
    // preference survives a restart; OpenXrShell::run() applies it once via
    // call_activity_void("bgmDisable") right after settings load if false.
    bool bgm_enabled       = true;
    // Menu music volume (0..1), independent of the emulator/ROM audio volume
    // (see EmulatorBackend's own volume, driven separately). Applied live via
    // OpenXrShell::call_activity_float("bgmSetVolume", ...).
    float bgm_volume       = 0.5f;
    // Per-core ROM audio channel splitting (Audio > Channels). Master gate:
    // when off, every backend is forced to full volume on every channel
    // regardless of the slider values below, so output is bit-for-bit what
    // it always was — the sliders exist purely as pre-set values for the
    // next time this is turned on. Each backend applies its own channels'
    // gains inside its native sound-chip mixer (see e.g.
    // snes_set_channel_volume/genesis_set_channel_volume/etc. in the
    // matching *_backend.h) — OpenXrShell pushes these values down whenever
    // they change and once on ROM load.
    bool audio_channel_split_enabled = false;
    float snes_voice_volume[8]      = {1,1,1,1,1,1,1,1};       // SPC700 DSP voices 1-8
    float genesis_channel_volume[2] = {1,1};                   // 0=FM (music), 1=PSG (square/noise) — also SMS/GG
    float nes_channel_volume[5]     = {1,1,1,1,1};             // 0=Pulse1,1=Pulse2,2=Triangle,3=Noise,4=DMC
    float gba_channel_volume[3]     = {1,1,1};                 // 0=PSG group,1=Direct Sound A,2=Direct Sound B
    float pce_channel_volume[6]     = {1,1,1,1,1,1};           // HuC6280 PSG voices 1-6
    UpscaleMode upscale_mode = UpscaleMode::Off; // off / pixel-art reconstruction / FSR (RCAS) sharpen
    bool shadows           = false; // repurposed as Meta Quest passthrough
    bool ambilight         = true;
    AmbilightPlacement ambilight_placement = AmbilightPlacement::Screen;
    // Content shown on the passive left/right side panels, cycled via the "Side Panels" settings
    // row: 0=Help (static instructions, the original content), 1=Settings (quick controls +
    // presets), 2=Perf Overlay (FPS/CPU/RAM/GPU, moved here from a left-controller-attached
    // panel), 3=Background Color (color/gradient picker for the VR backdrop).
    int side_panel_mode = 0;
    // VR lightgun overlay model: 0=downloaded CC0 pistol, 1=restored low-poly
    // pistol, 2=scope rifle. Cycled via the "Gun Model" settings row.
    // See GlesRenderer::draw_gun_model().
    int gun_model = 0;
    // When true, aiming the lightgun onto the screen holds the reload/hide
    // input; aiming off any edge (past a small dead-zone margin) releases it.
    // The field name and persisted key retain the old offscreen name for
    // compatibility. Off by default (per-game .ini opt-in, e.g. for Time
    // Crisis's screen-held hide/reload gesture) since it changes normal
    // button behavior.
    bool gun_offscreen_reload_enabled = false;
    // Which input the screen gesture drives while gun_offscreen_reload_enabled
    // is on. 0 = the standard libretro lightgun reload signal (RETRO_DEVICE_ID_
    // LIGHTGUN_RELOAD, what most Zapper/Super Scope/Virtua Gun games expect).
    // 1-10 = a regular joypad button instead (A/B/X/Y/L/R/Start/Select/C/Z, in
    // that order) for games whose "reload" is just a normal button rather than
    // the lightgun API (e.g. some MAME lightgun titles). See kGunOffscreenReloadButtonNames.
    int gun_offscreen_reload_button = 0;
    static constexpr int kGunOffscreenReloadButtonCount = 11;
    static constexpr const char* kGunOffscreenReloadButtonNames[kGunOffscreenReloadButtonCount] = {
        "Lightgun Reload", "A", "B", "X", "Y", "L", "R", "Start", "Select", "C", "Z"
    };
    // Haptic envelope played on a lightgun trigger pull: 0=off, 1=sharp recoil,
    // 2=machine-gun burst, 3=revolver swell/full kick/decay.
    int gun_vibration_mode = 0;
    // Player two's gun, used when Two Guns is on. Separate from player one's so
    // the two hands can hold different weapons with different kick -- there is
    // no reason a second player should be forced into the same pistol.
    int gun2_model = 0;
    int gun2_vibration_mode = 0;
    // Experimental > Motion Controls > D-Pad Headset: head tilt drives the
    // d-pad. Tilt left/right (ear toward shoulder) holds left/right, pitch
    // up/down holds up/down, and the two combine into diagonals. Off by
    // default like everything under the Experimental tab.
    bool  dpad_headset_enabled   = false;
    // Tilt past this many radians on an axis to latch that direction. Low
    // values react to head shake; high values demand an uncomfortable craning.
    // Released again at 60% of this (see kDpadHeadsetRelease) so a direction
    // held right at the line does not chatter on and off.
    float dpad_headset_threshold = 0.30f; // ~17 degrees
    static constexpr float kDpadHeadsetThresholdMin = 0.10f; // ~6 deg
    static constexpr float kDpadHeadsetThresholdMax = 0.70f; // ~40 deg
    static constexpr float kDpadHeadsetRelease      = 0.60f; // fraction of threshold

    // Experimental > Motion Controls > Air Wheel. Hold both controllers as if
    // gripping a wheel; each motion below is enabled independently and latches
    // into its own QuestInput (QI_WHEEL_*), so what it actually does is decided
    // by the Controller Map for the current console.
    bool  air_wheel_enabled = false;
    // Steering: the line between the hands rolling. Right hand higher = left.
    bool  air_wheel_steer_enabled   = true;
    float air_wheel_steer_threshold = 0.25f;  // radians, ~14 deg of wheel roll
    // Accelerate / brake: hands pushed away from / pulled toward the chest.
    bool  air_wheel_accel_enabled   = false;
    bool  air_wheel_brake_enabled   = false;
    float air_wheel_push_threshold  = 0.10f;  // metres from the neutral reach
    // Brake is a SPEED gesture, not a position one: yank the wheel in quickly
    // and you brake, ease it in slowly and you simply stop accelerating (i.e.
    // coast). Metres/second of inward travel needed to trigger it.
    float air_wheel_brake_speed     = 0.60f;
    // Once triggered, hold the brake at least this long -- a fast pull is over
    // in a few frames, far too short for a game to register as a button press.
    static constexpr float kAirWheelBrakeMinHold = 0.22f; // seconds
    // Let the neutral pose drift toward wherever the hands actually rest, so
    // the wheel settles at a comfortable position instead of the one captured
    // on the first frame. Frozen while any motion is latched or the hands are
    // moving, otherwise a held input would drag the neutral out to meet it and
    // silently release itself.
    bool  air_wheel_adaptive_neutral = true;

    // Which PHYSICAL input each motion gesture stands in for. A motion does not
    // press a console button directly -- it acts as though you moved the stick
    // or pressed the button named here, and the ordinary Controller Map then
    // decides what that means on this console. So "Wheel L = LS Left" plus the
    // usual "SNES Left = LS Left" closes the loop, and a game that remaps LS
    // Left to something else carries the wheel along with it automatically.
    //
    // Indexed by (QuestInput - kMotionBindFirst); values are QuestInput ids,
    // 0 (QI_NONE) meaning the gesture drives nothing. Kept as plain ints so
    // vr_state.h does not have to include button_map.h.
    // Exclusive Motion Input: when a gesture stands in for a physical input,
    // suppress that physical input so only the gesture drives it. With wheel
    // steering bound to LS Left/Right this means the stick no longer turns the
    // car -- only the wheel does. Applies to every enabled motion across the
    // whole Motion Controls group; inputs no motion claims are untouched.
    bool motion_exclusive = false;

    // Experimental > Motion Controls > Air Jump / Crouch. A sudden rise of the
    // headset holds "jump", a sudden drop holds "crouch"; both keep holding
    // while your head stays away from the neutral height, so a held crouch is
    // just staying down.
    bool  air_jump_enabled     = false;
    float air_jump_speed       = 0.90f; // m/s of head travel needed to trigger
    float air_jump_hold_margin = 0.07f; // metres from neutral that still counts as held
    static constexpr float kAirJumpMinHold = 0.12f; // seconds, so a quick bob registers

    // Experimental > Motion Controls > Air Fighter. Recognised arm motions play
    // back as timed directional sequences. Which SIDE the move comes out is
    // decided by where the motion finishes, not by reading the game's memory:
    // finish a quarter-circle on the left of the screen and it plays
    // Down, Down+Left, Left, Punch; finish on the right and it mirrors. The
    // player already knows which way they are facing, so nothing has to.
    bool  air_fighter_enabled = false;
    // Quarter-Circle + Punch: both hands thrust forward together.
    bool  fight_qc_enabled    = true;
    float fight_qc_speed      = 1.20f; // m/s of forward hand travel
    // Dragon Punch: one hand swings up. Left hand aims left, right hand right.
    bool  fight_dp_enabled    = true;
    float fight_dp_speed      = 1.40f; // m/s of upward hand travel
    // Charge move (Sonic Boom style): hold a hand out to one side to charge
    // that direction for real, then sweep across to fire the opposite way.
    // The two charge releases are separate moves that happen to share one held
    // position; the charge itself is held whenever either is on.
    bool  fight_charge_across_enabled = false;
    bool  fight_charge_up_enabled     = false;
    float fight_charge_distance = 0.30f; // metres to the side that starts a charge
    float fight_charge_seconds  = 2.00f; // how long it must be held, matching the game
    float fight_charge_speed    = 1.20f; // m/s of the release sweep
    // How long each step of a played-back sequence is held. Roughly three
    // emulator frames at 60Hz -- long enough for the game to sample it, short
    // enough that the whole motion lands inside its input window.
    static constexpr float kFightStepSeconds = 0.05f;
    // Quarter-Circle + Kick: thrust one hand SIDEWAYS rather than forward.
    // A sideways strike is cleanly separable from every other gesture here --
    // forward is a punch or a fireball, up is a Dragon Punch -- so it needs no
    // awkward rolling motion to distinguish it.
    bool  fight_qck_enabled   = true;
    float fight_qck_speed     = 1.10f; // m/s of sideways hand travel
    // How much the sideways component must DOMINATE the forward and vertical
    // ones. Without this a boxing hook, or any punch thrown slightly off
    // centre, carries enough lateral travel to fire this by accident. At 2.5
    // the strike has to be within roughly 22 degrees of straight out to the
    // side, so ordinary punching never triggers it.
    float fight_qck_purity    = 2.50f;

    // Normal attacks: thrust one hand. Straight ahead is a punch, angled down
    // is a kick. Snap it back for the light version, leave the arm extended
    // for the heavy one. Upward is deliberately NOT a normal -- that axis is
    // already Dragon Punch and the charged Up+Kick, and a three-way split on
    // one axis misfires constantly.
    bool  fight_punch_enabled   = false; // forward thrust
    bool  fight_kick_enabled    = false; // downward-angled thrust
    // Heavy variants. Turning this OFF removes the decide window entirely, so
    // light attacks fire the instant you strike with no latency at all -- worth
    // it for games with a single attack button, or if the delay bothers you.
    bool  fight_heavy_enabled   = true;
    float fight_punch_speed     = 1.10f; // m/s of hand travel to register a strike
    // How long to wait before deciding light vs heavy. This is unavoidable
    // latency on the light attack -- the difference only exists in what the
    // hand does AFTER the strike, so there is nothing to measure until then.
    float fight_hold_seconds    = 0.15f;
    // How steeply the thrust must angle down before it counts as a kick,
    // as a fraction of the forward speed.
    float fight_kick_ratio      = 0.60f;

    static constexpr int kMotionBindFirst = 17; // QI_HEAD_UP
    static constexpr int kMotionBindCount = 22; // .. QI_FIGHT_KICK_HARD (38)
    // Defaults: head tilt and wheel steering both stand in for the left stick,
    // which is what the d-pad maps to on every backend out of the box.
    int motion_bind[kMotionBindCount] = {
        13, 14, 15, 16,  // Head Up/Down/Left/Right -> LS Up/Down/Left/Right
        15, 16,          // Wheel L/R               -> LS Left/Right
        0, 0,            // Accel, Brake            -> unassigned
        0,               // Gear Up
        0,               // Handbrake
        0,               // Bike Accel
        0,               // Gear Down
        0, 0,            // Air Jump, Air Crouch
        // Fight components default to the left stick plus A/B, which is what
        // the d-pad and the two main attack buttons map to out of the box.
        13, 14, 15, 16,  // Fight Up/Down/Left/Right -> LS Up/Down/Left/Right
        1, 2,            // Fight Punch -> A, Fight Kick -> B
        3, 4,            // Punch Hard -> X, Kick Hard -> Y
    };
    // Gear: right controller pointed at the floor shifts up, at the ceiling
    // shifts down. Threshold is the Y of the controller's forward axis, so
    // 0.5 is roughly 30 degrees off horizontal and 1.0 is straight down.
    bool  air_wheel_gear_enabled    = false;
    float air_wheel_gear_threshold  = 0.50f;
    // Handbrake: both hands lifted, like pulling a lever up.
    bool  air_wheel_handbrake_enabled  = false;
    float air_wheel_handbrake_threshold = 0.15f; // metres of lift
    // Bike accelerate: twisting the grips down, like a motorcycle throttle.
    bool  air_wheel_bike_enabled    = false;
    float air_wheel_bike_threshold  = 0.35f;  // radians of twist
    static constexpr float kAirWheelRelease = 0.60f; // fraction, same idea as above
    static constexpr int kGunVibrationModeCount = 4;
    static constexpr const char* kGunVibrationModeNames[kGunVibrationModeCount] = {
        "Off", "Recoil", "Machinegun", "Revolver"
    };
    // Real Quest controller models (XR_FB_render_model), rendered in VR with
    // live button/trigger/stick animation -- mainly for tutorial recordings
    // where viewers need to see what's being pressed. See
    // OpenXrShell::load_controller_render_models() and
    // GlesRenderer::draw_controller_model().
    bool show_controller_models = true;

    // Background Color side-panel selection: -1 = none chosen (falls back to whatever the
    // Environment Sphere setting already produces, if any); 0-7 = solid color presets; 8-15 =
    // gradient (floor/bottom to sky/top) presets. See kBgSolidPresets/kBgGradientPresets in
    // openxr_shell.cpp for the actual colors.
    int bg_preset_index = -1;
    EnvironmentSphereMode environment_sphere_mode = EnvironmentSphereMode::Off;

    // Perspective compensation: scale each layer's quad width so all layers subtend
    // the same visual angle as the nearest layer.
    bool perspective_comp = true;

    // Optional PSX source-layer capture from the GPU command classes. It is
    // off by default and does not extract 3D geometry.
    // Opt-in native SwanStation PGXP scene replay. When unavailable for a
    // frame, the normal PSX framebuffer/layer path remains active.

    // Parallax peek: head rotation shifts deeper layers proportionally.
    // 0=off; steps are k_parallax_steps in openxr_shell.cpp (0, 0.005, 0.05,
    // 0.1, 0.25, 0.5, 1.0 -- farthest-layer multiplier). Defaults to the first
    // non-zero step: enough depth response to feel present on every system
    // without the deeper layers visibly sliding under head motion.
    float parallax_ratio = 0.005f;

    // Sprite Y-depth: per-pixel Z displacement on sprite/OBJ layers
    bool  sprite_y_depth        = false;
    float sprite_y_depth_spread = 0.5f; // total Z range in metres (close−far)

    // Spatial audio mode: 0=off  1=wide  2=spatial EQ  3=spatial EQ + haptics
    int  audio_spatial_mode  = 2;     // 0=off  1=wide  2=spatial EQ  3=spatial EQ + haptics
    bool audio_screen_lock   = false; // anchor stereo field to screen world position

    // Performance settings
    // Per-core "let the emulator skip frames under load" toggles (Visuals >
    // Frame Skip). One field per backend that actually has a real (or
    // best-effort, see MgbaBackend) frameskip hook — NES/FCEUmm has no such
    // core option at all (see FceuxBackend::set_auto_frame_skip), so there's
    // deliberately no toggle for it. Genesis/SMS/GG share one field (both
    // run through PicoDriveBackend); GB/GBA share one field (both run
    // through MgbaBackend).
    bool  auto_frame_skip_snes    = false;
    bool  auto_frame_skip_genesis = false;
    bool  auto_frame_skip_mame    = false;
    bool  auto_frame_skip_saturn  = false;
    bool  auto_frame_skip_pce     = false;
    bool  auto_frame_skip_gba     = false;
    int   emu_resolution_scale = 1;     // emulator internal render scale (1-4)
    // Which PSX rendering path to use. These produce the same picture by
    // different routes, and exist as a switch because each trades reliability
    // against cost differently:
    //   0 Zero-Copy - hardware renderer, textures shared straight to the XR
    //     thread. Cheapest, most moving parts.
    //   1 Readback  - hardware renderer, frame and depth copied back to the CPU.
    //     Simpler handoff, costs bandwidth.
    //   2 Software  - no hardware renderer; depth rasterised on the CPU.
    //     Slowest and least sharp, but the fewest ways to fail.
    // Zero-Copy and Readback switch live. Software changes the core's renderer,
    // which SwanStation only accepts at boot, so it applies on the next ROM load.
    int psx_render_path = 0;
    // SwanStation internal GPU resolution (1, 2 or 4). 4x measured on Quest 2:
    // 72/72 fps, no stale frames, with the hardware renderer doing the work.
    // Native 1x makes the depth map too coarse to read.
    int   psx_gpu_resolution = 4;
    // SwanStation texture filtering (swanstation_GPU_TextureFilter) -- the
    // core's internal upscaler for magnified textures, which is a different
    // thing from the resolution scale above: the scale sharpens polygon edges
    // and rasterisation, this smooths the chunky texels stretched across them.
    // Hardware renderer only, and its effect grows with the resolution scale.
    // Index into kPsxTextureFilterValues/Names below.
    int   psx_texture_filter = 0;
    static constexpr int kPsxTextureFilterCount = 5;
    // Core option values, in menu order.
    static constexpr const char* kPsxTextureFilterValues[kPsxTextureFilterCount] = {
        "Nearest", "Bilinear", "BilinearBinAlpha", "JINC2", "xBR"};
    static constexpr const char* kPsxTextureFilterNames[kPsxTextureFilterCount] = {
        "Nearest", "Bilinear", "Bilinear NB", "JINC2", "xBR"};
    float vr_resolution_scale  = 1.0f;  // OpenXR eye swapchain scale vs recommended (0.25-4.0)

    // Unified menu (Interface > Placement): 0=Follow Left Hand 1=Follow Right Hand
    // 2=Follow Headset 3=Left Side 4=Right Side.
    int menu_position_mode     = 2;
    // Unified menu (Interface > Placement): 0=Automatic (laser-proximity ramp)
    // 1=25% 2=50% 3=100%.
    int menu_transparency_mode = 0;
    // Unified menu favorites: ★-starred row keys (e.g. "Visuals|Color Grading|Gamma"
    // or "LAYER::game::layer|Visibility"), joined with ';' since the .ini format is
    // one "key=value" per line. Global preference only — see save_settings/
    // load_settings, which only sync this in the !game_scope path.
    std::string menu_favorites_csv;

    // Depth Arrangement widget (Layers > Stack): m_layer_slot_fraction, joined
    // with ';' (one 0-1 fraction per depth slot, in slot order — count is
    // fixed per emulator core, so this only round-trips correctly for the same
    // core/layer-count it was saved with; a mismatched count is discarded and
    // the widget re-seeds its default even spacing instead). Per-game, not
    // global — see save_settings/load_settings' game_scope path.
    std::string layer_slot_fractions_csv;
    // Depth Arrangement's global "Canvas Depth" slider (metres) and "Thickness
    // Overlap" toggle — per-game, same as layer_slot_fractions_csv above.
    float canvas_depth_meters_ui = 2.0f;
    bool  thickness_overlap_ui   = false;

    // Per-layer overrides (populated by the randomiser, indexed to match GameConfig::layers)
    struct LayerOverride {
        float depth_meters      = 0.0f; // 0 = use config default
        float quad_width_meters = 0.0f;
        std::vector<float> copies;
    };
    std::vector<LayerOverride> layer_overrides;

    // -----------------------------------------------------------------------
    // Apply layerOverrides back onto a GameConfig (if they are set)
    // -----------------------------------------------------------------------
    void apply_to_config(GameConfig& cfg) const {
        for (int i = 0; i < (int)cfg.layers.size() && i < (int)layer_overrides.size(); ++i) {
            const auto& ov = layer_overrides[i];
            if (ov.depth_meters > 0.0f)
                cfg.layers[i].depth_meters = ov.depth_meters;
            if (ov.quad_width_meters > 0.0f)
                cfg.layers[i].quad_width_meters = ov.quad_width_meters;
            if (!ov.copies.empty())
                cfg.layers[i].copies = ov.copies;
        }
    }

    // -----------------------------------------------------------------------
    // Randomise — exact probability weights ported from retrodepth PC
    // -----------------------------------------------------------------------
    void randomize(GameConfig& cfg, std::mt19937& rng) {
        auto randf = [&](float lo, float hi) {
            return std::uniform_real_distribution<float>(lo, hi)(rng);
        };
        auto randi = [&](int lo, int hi) {
            return std::uniform_int_distribution<int>(lo, hi)(rng);
        };
        auto randb = [&](float p) {
            return std::bernoulli_distribution(p)(rng);
        };

        gamma      = randf(0.95f, 1.35f);
        contrast   = randf(0.82f, 1.20f);
        saturation = randf(0.60f, 1.15f);
        brightness = 1.0f;

        layers_3d       = randb(0.68f);
        solid_stack     = false;
        depth_mode      = randb(0.18f) ? DepthMode::WholeLayer : DepthMode::Off;
        upscale_mode    = randb(0.35f) ? UpscaleMode::PixelArt : UpscaleMode::Off;
        shadows         = false;
        ambilight       = randb(0.62f);
        side_panel_mode = 0;

        roundness    = 0.0f;
        screen_curve = 0.0f;
        tilt_x       = randf(-0.075f, 0.075f);
        tilt_y       = randf(-0.11f,  0.11f);

        perspective_comp      = true;
        parallax_ratio        = randb(0.40f) ? randf(0.0f, 1.5f) : 0.0f;
        sprite_y_depth        = randb(0.30f);
        sprite_y_depth_spread = sprite_y_depth ? randf(0.25f, 0.85f) : 0.5f;

        const int n = (int)cfg.layers.size();
        if (n == 0) return;

        const float far_depth   = randf(1.55f, 4.20f);
        const float spread      = randf(0.24f, 0.88f);
        const float near_depth  = std::max(0.80f, far_depth - spread);
        const float app_scale   = randf(0.68f, 1.18f);
        const float base_width  = std::clamp(far_depth * app_scale, 1.35f, 3.30f);
        const float width_slope = randf(-0.10f, 0.14f);
        const int   copy_count  = randi(6, 18);
        const float copy_step   = randf(0.0100f, 0.0200f);

        layer_overrides.resize(n);
        for (int i = 0; i < n; ++i) {
            float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
            auto& ov = layer_overrides[i];
            ov.depth_meters      = far_depth + t * (near_depth - far_depth);
            ov.quad_width_meters = std::clamp(
                base_width + (t - 0.5f) * width_slope * 2.0f, 1.40f, 4.00f);
            float spacing = copy_step * randf(0.90f, 1.12f);
            ov.copies.resize(copy_count);
            for (int c = 0; c < copy_count; ++c)
                ov.copies[c] = (float)(c + 1) * spacing;
        }

        apply_to_config(cfg);
    }
};

// -----------------------------------------------------------------------
// Five named presets (ported from retrodepth PC build_default_vr_presets)
// -----------------------------------------------------------------------
inline std::vector<VrState> make_default_vr_presets() {
    std::vector<VrState> presets(5);

    // Preset 0: Balanced
    presets[0].gamma = 1.15f; presets[0].contrast = 1.05f; presets[0].saturation = 0.80f;

    // Preset 1: Compressed depth, wider layers
    presets[1].gamma = 1.10f; presets[1].contrast = 1.05f; presets[1].saturation = 0.75f;

    // Preset 2: Exaggerated depth
    presets[2].gamma = 1.20f; presets[2].contrast = 1.05f; presets[2].saturation = 0.85f;

    // Preset 3: 3-D layers + solid extrusion
    presets[3].gamma = 1.18f; presets[3].contrast = 1.05f; presets[3].saturation = 0.82f;
    presets[3].layers_3d = true;

    // Preset 4: Crisp upscale + ambilight
    presets[4].gamma = 1.05f; presets[4].contrast = 1.05f; presets[4].saturation = 1.00f;
    presets[4].upscale_mode = UpscaleMode::PixelArt; presets[4].ambilight = true;

    return presets;
}
