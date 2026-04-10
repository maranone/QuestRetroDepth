#pragma once

// jni.h must come first — openxr_platform.h references jobject
#include <jni.h>

#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "button_map.h"
#include "emulator_backend.h"
#include "game_config.h"
#include "gles_renderer.h"
#include "layer_processor.h"
#include "rom_browser.h"
#include "vr_state.h"

namespace qrd {

// Called from the XR render thread to publish controller state and fetch the
// latest emulator frame. The sequence is caller-owned; the provider only copies
// FrameOutput when a newer frame is available.
using FrameProvider = std::function<bool(FrameOutput&, EmulatorInputState&, uint64_t&)>;

enum class LayerFilterMode {
    ShowAll = 0,
    Z       = 1,
    Per     = 2,
    Hybrid  = 3,
};

class OpenXrShell {
public:
    OpenXrShell() = default;
    ~OpenXrShell();

    bool start(JavaVM* vm, JNIEnv* env, jobject activity, std::string& status_out);
    void stop(JNIEnv* env);
    std::string status() const;
    void set_frame_provider(FrameProvider provider);

    // Thread-safe: schedule a randomise on the next XR frame
    void randomize();
    // Called by ROM loader to update the active game name (used for per-game settings)
    void set_current_rom(const std::string& rom_filename); // e.g. "Chrono Trigger.sfc"
    void load_preset(int idx);
    void save_preset(int idx);
    std::string vr_state_summary() const;

    // Share-code: encode current state → 8-char string; decode & apply a code.
    std::string get_state_code() const;
    // Returns true on success; false if the code is malformed.
    bool apply_state_code(const std::string& code);

    // Set callback invoked on the XR thread when the user picks a ROM.
    using RomLoader = std::function<bool(const std::string&, std::string&)>;
    void set_rom_loader(RomLoader loader) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_loader = std::move(loader);
    }

    // Emulator freeze control: called from XR thread to pause/resume the emu thread,
    // and to step exactly one frame (so layers refresh when settings change while frozen).
    using EmuFreezeCtrl = std::function<void(bool freeze)>; // true=freeze, false=unfreeze
    using EmuStepOne    = std::function<void()>;            // step one frame then re-freeze
    void set_emu_freeze_ctrl(EmuFreezeCtrl fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_emu_freeze_ctrl = std::move(fn);
    }
    void set_emu_step_one(EmuStepOne fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_emu_step_one = std::move(fn);
    }
    using LayerCaptureMaskCtrl = std::function<void(uint32_t mask)>;
    void set_layer_capture_mask_ctrl(LayerCaptureMaskCtrl fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_layer_capture_mask_ctrl = std::move(fn);
    }
    using VrStateChanged = std::function<void(bool auto_frame_skip)>;
    void set_on_vr_state_changed(VrStateChanged fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_on_vr_state_changed = std::move(fn);
    }

    void set_current_backend_kind(BackendKind kind);
    void set_current_game_name(const std::string& name);

private:
    void set_status(const std::string& s);

    // ---------- init chain ----------
    bool initialize_loader();
    bool create_instance();
    bool create_system();
    bool create_graphics_context();
    bool create_session();
    bool create_reference_space();
    bool create_swapchains();
    void destroy_swapchains();
    bool init_actions();
    bool init_renderer();
    bool init_passthrough();
    void sync_passthrough_state();
    bool passthrough_active() const;

    // ---------- run loop ----------
    void run();
    void poll_events(bool& exit);
    void poll_actions();
    void render_frame(XrTime predicted_time);
    void apply_pending_vr_changes();
    void recenter_to_hmd();                    // snap canvas to current HMD gaze direction
    void open_rom_menu();                      // scan ROMs + place main menu panel in front of HMD
    void rebuild_main_menu_texture();          // call Kotlin → upload GL texture for main menu
    void rebuild_layer_panel_texture();        // call Kotlin → upload GL texture
    void rebuild_settings_panel_texture();     // call Kotlin → upload GL texture
    void rebuild_code_panel_texture();         // call Kotlin → upload GL texture
    void rebuild_ctrlmap_panel_texture();      // call Kotlin → upload GL texture
    std::string get_settings_dir();            // call Kotlin → returns settings directory path
    void save_settings(bool game_scope);       // save current state to disk
    void load_settings(bool game_scope);       // load state from disk (if file exists)
    void reset_settings();                     // reset to hardcoded defaults
    void apply_layer_filter_mode(LayerFilterMode mode, bool restore_saved_state);
    void sync_layer_capture_mask();
    // right=true → right controller, false → left; amplitude 0-1, duration_ms
    void fire_haptic(bool right, float amplitude = 0.4f, int duration_ms = 40);
    void shutdown();

    // ---------- Android / JNI ----------
    JavaVM*  m_vm              = nullptr;
    jobject  m_activity_global = nullptr;

    std::thread        m_thread;
    std::atomic<bool>  m_stop_requested{false};
    std::atomic<bool>  m_running{false};

    mutable std::mutex m_mutex;
    std::string        m_status = "VR shell idle.";
    FrameProvider      m_frame_provider;
    VrStateChanged     m_on_vr_state_changed;
    float              m_active_refresh_rate = 0.0f;
    float              m_last_render_ms      = 0.0f;
    float              m_avg_render_ms       = 0.0f;
    float              m_max_render_ms       = 0.0f;
    uint64_t           m_render_sample_count = 0;

    // ---------- VR state (accessed only from XR thread after init) ----------
    GameConfig           m_config;
    VrState              m_vr_state;
    std::vector<VrState> m_presets;
    std::mt19937         m_rng{std::random_device{}()};

    // ---------- Main menu panel ----------
    GLuint  m_main_menu_tex       = 0;
    bool    m_main_menu_dirty     = true;
    int     m_main_menu_hovered   = -1; // row under laser (-1=none)
    XrTime  m_last_main_menu_fire = 0;

    // ---------- ROM browser panel ----------
    RomBrowser  m_rom_browser;
    bool        m_menu_open  = false;
    std::string m_settings_dir;       // cached path to settings directory
    std::string m_current_rom_name;   // filename stem of currently loaded ROM (for per-game settings)
    std::string m_current_game_name;  // from ROM header (0xFFC0+) for version fallback
    BackendKind m_current_backend_kind = BackendKind::Snes;

    bool        m_menu_prev  = false;
    bool        m_rtrig_prev = false;
    std::string m_rom_dir;
    XrTime      m_last_rom_fire = 0;  // throttle for ROM browser rebuilds

    // Panel poses (set in open_rom_menu, index matches k_panel_* constants)
    static constexpr int k_panel_main_menu  = 0;
    static constexpr int k_panel_layers     = 1;
    static constexpr int k_panel_browser    = 2;
    static constexpr int k_panel_settings   = 3;
    static constexpr int k_panel_code       = 4;
    static constexpr int k_panel_ctrlmap    = 5;
    XrPosef m_main_menu_pose       = {{0,0,0,1},{0,0,-1}};
    XrPosef m_panel_pose           = {{0,0,0,1},{0,0,-1}}; // browser (centre)
    XrPosef m_layer_panel_pose     = {{0,0,0,1},{0,0,-1}};
    XrPosef m_settings_panel_pose  = {{0,0,0,1},{0,0,-1}};
    XrPosef m_code_panel_pose      = {{0,0,0,1},{0,0,-1}};
    XrPosef m_ctrlmap_panel_pose   = {{0,0,0,1},{0,0,-1}};
    bool    m_ctrlmap_mode         = false; // true = showing ctrlmap panel only

    // Main menu sub-panel tracking: which secondary panel is currently open
    // 0 = main menu showing, 1 = browser, 2 = layers, 3 = settings, 4 = code, 5 = ctrlmap
    int     m_active_sub_panel     = 0;

    // Multi-panel laser state (menu mode — right controller)
    bool        m_laser_hit    = false;
    XrVector3f  m_laser_origin = {0,0,0};
    XrVector3f  m_laser_end    = {0,0,0};
    int         m_laser_panel  = -1;  // which panel was hit (k_panel_*)
    float       m_laser_hit_u  = 0.0f;
    float       m_laser_hit_v  = 0.0f;

    // Edit-mode laser state
    XrVector3f  m_edit_laser_l_origin = {0,0,0};
    XrVector3f  m_edit_laser_l_end    = {0,0,0};
    XrVector3f  m_edit_laser_r_origin = {0,0,0};
    XrVector3f  m_edit_laser_r_end    = {0,0,0};
    // Left laser: reference aim direction at entry (for translation)
    XrVector3f  m_edit_laim_ref_dir   = {0,0,-1};
    bool        m_edit_laim_ref_valid  = false;
    // Right laser: reference aim direction at entry (for sphere delta)
    float       m_edit_raim_ref_az   = 0.0f;
    float       m_edit_raim_ref_el   = 0.0f;
    bool        m_edit_raim_ref_valid = false;

    // ROM loader callback (set by main.cpp)
    RomLoader m_rom_loader;

    // ---------- Layer order panel ----------
    // Layer names/order/enabled are game-session state (not saved in presets)
    std::vector<std::string> m_layer_names;
    std::vector<int>         m_layer_order;   // m_layer_order[display_row] = original_idx
    std::vector<bool>        m_layer_enabled;   // indexed by original_idx
    std::vector<bool>        m_layer_ambilight; // indexed by original_idx
    int     m_layer_auto_dup_percent = 25; // -1 = OFF, otherwise percentage target for farthest layer
    LayerFilterMode m_layer_filter_mode = LayerFilterMode::Hybrid;
    GLuint  m_layer_panel_tex     = 0;
    bool    m_layer_panel_dirty   = true;
    int     m_layer_panel_hovered = -1; // display row under laser
    int     m_layer_panel_grabbed = -1; // display row being dragged (-1 = none)
    XrTime  m_last_layer_fire     = 0;
    bool    m_emu_frozen_display  = false; // frozen state for play/pause button display
    struct LayerModeSnapshot {
        bool valid = false;
        LayerFilterMode mode = LayerFilterMode::Hybrid;
        GameConfig config;
        std::vector<int> order;
        std::vector<bool> enabled;
        std::vector<bool> ambilight;
    } m_saved_layer_mode_state;

    // ---------- Settings panel ----------
    GLuint  m_settings_panel_tex     = 0;
    bool    m_settings_panel_dirty   = true;
    int     m_settings_panel_hovered = -1;
    int     m_settings_panel_area    = 0; // 0=none 1=minus 2=plus
    XrTime  m_last_settings_fire     = 0;

    // ---------- Code-input panel (floats above ROM browser) ----------
    GLuint      m_code_panel_tex     = 0;
    bool        m_code_panel_dirty   = true;
    int         m_code_panel_hovered = -1; // index into k_code_keys (-1 = none)
    XrTime      m_last_code_fire     = 0;
    std::string m_code_input_buf;          // chars typed so far (≤ 20)

    // ---------- Controller map panel ----------
    GLuint  m_ctrlmap_panel_tex     = 0;
    bool    m_ctrlmap_panel_dirty   = true;
    int     m_ctrlmap_panel_hovered = -1; // row under laser (-1=none)
    int     m_ctrlmap_selected_row  = -1; // row being remapped (-1=none)
    ButtonMap m_button_map;               // current mapping (SNES→QuestInput)

    // Emulator freeze control callbacks (set by questretrodepth_main.cpp)
    EmuFreezeCtrl m_emu_freeze_ctrl;
    EmuStepOne    m_emu_step_one;
    LayerCaptureMaskCtrl m_layer_capture_mask_ctrl;

    // Pending changes requested from JNI thread
    std::atomic<bool> m_randomize_pending{false};
    std::atomic<int>  m_preset_load_pending{-1};
    std::atomic<int>  m_preset_save_pending{-1};
    // Share-code apply: store the code string guarded by m_mutex, then set flag
    std::atomic<bool> m_apply_code_pending{false};
    std::string       m_pending_code;          // guarded by m_mutex
    // Settings I/O actions: 0=none, 1=save_game, 2=save_global, 3=load_game, 4=load_global, 5=reset
    std::atomic<int>  m_settings_action_pending{0};
    // Flag: load global settings on next XR frame (set at startup)
    std::atomic<bool> m_load_global_pending{false};
    // Flag: open main menu on first frame after session starts
    std::atomic<bool> m_open_menu_on_startup{false};
    // Flag: load game settings for the newly set ROM (set by set_current_rom)
    std::atomic<bool> m_load_game_pending{false};

    // Controller-driven input (written by XR thread, consumed by frame provider)
    std::mutex         m_input_mutex;
    EmulatorInputState m_input_state;

    // Cached emulator frame/layers. Updated only when the emulator publishes a
    // new frame, so XR refreshes can reuse already processed Genesis layers.
    FrameOutput             m_cached_frame_out;
    uint64_t                m_cached_frame_seq = 0;
    std::vector<LayerFrame> m_cached_layer_frames;
    std::vector<LayerFrame*> m_render_layer_refs;

    // Refresh rate (0 = use headset default; otherwise Hz e.g. 72, 90, 120)
    float  m_desired_refresh_rate = 0.0f;
    std::atomic<bool> m_apply_refresh_pending{false};

    // Rate-limiting (nanoseconds wall-clock via XrTime)
    XrTime m_last_depth_fire  = 0;
    XrTime m_last_width_fire  = 0;
    XrTime m_last_copy_fire   = 0;
    bool   m_lstick_click_prev = false;
    bool   m_rstick_click_prev = false;

    // Edit mode — toggled by left thumbstick click
    bool  m_edit_mode = false;
    // Canvas state at the moment edit was entered
    float m_edit_canvas_x = 0.0f;
    float m_edit_canvas_y = 0.0f;
    float m_edit_canvas_az = 0.0f; // azimuth (radians, right = positive)
    float m_edit_canvas_el = 0.0f; // elevation (radians, up = positive)
    // Live canvas placement (persists across edit sessions)
    float m_canvas_x  = 0.0f; // horizontal offset from centre (metres)
    float m_canvas_y  = 0.0f; // vertical offset from floor level (metres)
    float m_canvas_az = 0.0f; // horizontal arc angle (radians)
    float m_canvas_el = 0.0f; // vertical arc angle (radians)
    // Predicted display time for the current frame (used to locate controllers)
    XrTime m_frame_predicted_time = 0;
    // Auto-recenter once on first valid HMD pose
    bool   m_initial_recenter_done = false;

    // ---------- OpenXR / EGL / GL resources (opaque impl) ----------
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace qrd
