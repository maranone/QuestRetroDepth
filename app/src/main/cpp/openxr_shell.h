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
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "button_map.h"
#include "panel_layout.h"
#include "emulator_backend.h"
#include "lightgun_calibration.h"
#include "experimental_rumble.h"
#include "game_config.h"
#include "gles_renderer.h"
#include "layer_processor.h"
#include "rom_browser.h"
#include "rom_preview.h"
#include "vr_state.h"
#include "ui_theme.h"

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

// One row of the Credits panel, parsed at runtime from assets/credits.txt so
// the list can be edited without touching code — just rebuild the APK.
// A section header (from a "[Section]" line) has is_header = true and an
// empty url; a normal entry with an empty url renders but isn't clickable.
struct CreditRow {
    std::string name;
    std::string detail;
    std::string url;
    bool is_header = false;
};

// One line of the Help tab, parsed at runtime from assets/help.txt (same
// edit-the-asset-and-rebuild deal as CreditRow above). `section` is the
// "[Section]" the line was under and must match a group name listed for the
// "Help" tab in kUnifiedMenuTabs, otherwise the line is simply never drawn.
struct HelpRow {
    enum class Kind { Para, Bullet, Tip, Dim, Spacer };
    std::string section;
    std::string text;
    Kind kind = Kind::Para;
};

struct SaveStateSlotInfo {
    bool occupied = false;
    std::string label;
    std::uint64_t timestamp_epoch_seconds = 0;
};

class OpenXrShell {
public:
    OpenXrShell() = default;
    ~OpenXrShell();

    bool start(JavaVM* vm, JNIEnv* env, jobject activity, bool open_menu_on_startup,
               int autosave_interval_seconds, bool load_last_save_enabled,
               std::string& status_out);
    void stop(JNIEnv* env);
    std::string status() const;
    void set_frame_provider(FrameProvider provider);
    void request_open_main_menu();
    void request_open_homebrew();

    // Thread-safe: schedule a randomise on the next XR frame
    void randomize();
    // Called by ROM loader to update the active game name (used for per-game settings)
    void set_current_rom(const std::string& rom_filename); // e.g. "Chrono Trigger.sfc"
    // Shows the "Left grip + L-stick / Left grip + R-stick / Left grip + trigger / Left
    // thumbstick click" tips panel in front of the player for a few seconds. Called once per
    // successful ROM load.
    void show_rom_hint();
    // Sets a one-shot message that the next show_rom_hint() displays instead
    // of the fixed Tips list -- pass empty to clear (normal Tips resume).
    // Call before set_current_rom() so the same load's hint reflects it.
    void set_rom_hint_override(const std::string& text) { m_rom_hint_override_text = text; }
    // Foreground ROM preparation status. These are called from the extraction
    // worker/JNI thread and consumed by the XR thread when rebuilding the
    // loading panel.
    void set_rom_load_stage(const std::string& path, const std::string& stage);
    // file_index/file_total/file_name describe a multi-file disc image
    // extraction (Saturn cue+bin, Dreamcast gdi+tracks): each completed file
    // gets its own line in the displayed message instead of the percent
    // silently looping back to 0 with no indication anything besides the
    // current file exists. Single-file archives pass (1, 1, entryName).
    void set_rom_load_progress(const std::string& path, int percent,
                                int file_index, int file_total, const std::string& file_name);
    bool rom_load_in_progress() const { return m_rom_load_in_progress.load(std::memory_order_acquire); }
    // Marks which raw path (if any) is currently being extracted purely for a
    // hover-triggered live preview (RomPreviewManager's "is_live" capture) —
    // separate from the real foreground m_rom_load_path/m_rom_load_in_progress
    // gate above, since a hovered preview never sets that. Kotlin's archive
    // extractor calls the same nativeRomPreparationProgress hook regardless
    // of why prepare_rom_path() was invoked; set_rom_load_progress()/
    // set_rom_load_stage() forward matching updates here too so the preview
    // sidebar can show real "EXTRACTING ROM N%"/file-count text instead of a
    // generic "Loading preview..." placeholder while a zipped ROM unpacks.
    void set_preview_extract_target(const std::string& path);
    void set_preview_extract_done(const std::string& path, const std::string& extracted_path);
    std::string preview_extract_status(const std::string& path) const;
    // True when the game config currently in use has no real per-game layer
    // set (dynamic-layer games like Neo Geo don't count -- they start with
    // one seed layer but grow more once depth data is available).
    bool current_config_is_single_layer() const {
        return !m_config.dynamic_layers && m_config.layers.size() <= 1;
    }
    void load_preset(int idx);
    void save_preset(int idx);
    void submit_quick_preset_name(int kind, int slot, const std::string& name);
    void cancel_quick_preset_name(int kind, int slot);
    // ROM search dialog round-trip (Library > Browse & Launch's search box —
    // see draw_library_rom_list()): request_rom_search_dialog() calls into
    // Kotlin's showRomSearchDialog() (real AlertDialog+EditText, so Quest's
    // system keyboard actually appears — ImGui's own text fields have nothing
    // behind them since input here is laser+trigger, not a physical keyboard);
    // submit/cancel are the JNI callbacks once the player closes that dialog.
    // Credits — real data (m_credit_entries, parsed from
    // assets/credits.txt by the existing load_credits_entries()/
    // open_credits_link()), not a re-typed copy of the old panel's text.
    // Experimental > PSX -- renderer path, GPU resolution, texture filtering.
    void draw_psx_group();
    void draw_credits_group();
    // Help > <group> — draws the assets/help.txt lines whose [Section] equals
    // `section`. Content is data, not code: adding a topic means adding a
    // section to help.txt plus a group name under the "Help" tab.
    void draw_help_group(const char* section);
    // Reads assets/help.txt (via Kotlin) and parses it into m_help_entries.
    // Called lazily the first time the Help tab is drawn.
    void load_help_entries();
    // Library search — real in-VR virtual keyboard (laser+trigger clickable),
    // replacing the invisible-in-VR AlertDialog. Writes directly into the
    // caller's filter buffer so the ROM list filters live, key by key.
    void draw_rom_search_keyboard(char* filter, size_t filter_size);
    void request_rom_search_dialog(const std::string& current_text);
    void submit_rom_search_text(const std::string& text);
    void cancel_rom_search_text();
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
    // Re-run the current ROM through the normal asynchronous loader. Used for
    // settings that MAME latches while constructing a machine.
    void request_current_rom_reload();
    using RomPreparer = std::function<std::string(const std::string&)>;
    void set_rom_preparer(RomPreparer preparer) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_preparer = std::move(preparer);
    }
    using RomPreparedPathPublisher = std::function<void(const std::string&, const std::string&)>;
    void set_rom_prepared_path_publisher(RomPreparedPathPublisher publisher) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_prepared_path_publisher = std::move(publisher);
    }
    using RomPreviewCapture = qrd::RomPreviewCapture;
    void set_rom_preview_capture(RomPreviewCapture capture) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_preview_capture = std::move(capture);
    }
    using RomPreviewSessionBegin = std::function<bool(std::string&)>;
    using RomPreviewSessionEnd = std::function<void(bool committed)>;
    void set_rom_preview_session(RomPreviewSessionBegin begin, RomPreviewSessionEnd end) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_preview_begin = std::move(begin);
        m_rom_preview_end = std::move(end);
    }
    // Deletes every cached extracted-archive directory (Wipe Settings). Also
    // the recovery path for an archive that only partially extracted (app
    // killed mid-extraction, or the multi-file-disc sibling-track bug).
    using ExtractedRomCacheClearer = std::function<void()>;
    void set_extracted_rom_cache_clearer(ExtractedRomCacheClearer fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_extracted_rom_cache_clearer = std::move(fn);
    }
    // Returns {file_count, total_bytes} currently held in the extracted-archive
    // cache, for the Danger Zone's live count/size display.
    using ExtractedRomCacheStats = std::function<std::pair<int, long long>()>;
    void set_extracted_rom_cache_stats(ExtractedRomCacheStats fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_extracted_rom_cache_stats = std::move(fn);
    }
    // Quits the app (System > Exit).
    using AppExiter = std::function<void()>;
    void set_app_exiter(AppExiter fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_app_exiter = std::move(fn);
    }
    // Pushes VrState's per-core audio channel volumes into every backend's
    // native mixer (Audio > Channels). Wired in questretrodepth_main.cpp,
    // which is the one file that already links every concrete backend.
    using AudioChannelVolumeApplier = std::function<void(const VrState&)>;
    void set_audio_channel_volume_applier(AudioChannelVolumeApplier fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_audio_channel_volume_applier = std::move(fn);
    }
    // Unloads the currently-playing game for good (no restore) — used by the
    // Library tab's "Close ROM" gate so browsing never runs a live preview
    // concurrently with an already-active game (that combination crashed).
    using RomCloser = std::function<void()>;
    void set_rom_closer(RomCloser fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_rom_closer = std::move(fn);
    }
    // Closes the active game, optionally saving to `save_slot` first (-1 = no save).
    void close_current_rom(int save_slot);

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
    // Runtime lightgun mode control: called from XR thread when the player
    // holds the left thumbstick to force-enable/disable gun mode on a ROM the
    // title-matching heuristic didn't recognize.
    using EmuSetGunMode = std::function<void(bool enabled)>;
    using EmuSetDualGunMode = std::function<void(bool enabled)>;
    void set_dual_gun_mode_ctrl(EmuSetDualGunMode fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_dual_gun_mode_ctrl = std::move(fn);
    }
    void set_gun_mode_ctrl(EmuSetGunMode fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_gun_mode_ctrl = std::move(fn);
    }
    using LayerCaptureMaskCtrl = std::function<void(uint32_t mask)>;
    void set_layer_capture_mask_ctrl(LayerCaptureMaskCtrl fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_layer_capture_mask_ctrl = std::move(fn);
    }
    using MameOccupancyCtrl = std::function<void(bool enabled)>;
    void set_mame_occupancy_ctrl(MameOccupancyCtrl fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_mame_occupancy_ctrl = std::move(fn);
    }
    using SaveStateCapture = std::function<bool(std::vector<uint8_t>&, std::string&)>;
    using SaveStateApply = std::function<bool(const void*, std::size_t, std::string&)>;
    void set_save_state_capture(SaveStateCapture fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_save_state_capture = std::move(fn);
    }
    void set_save_state_apply(SaveStateApply fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_save_state_apply = std::move(fn);
    }
    using VrStateChanged = std::function<void(int audio_spatial_mode)>;
    void set_on_vr_state_changed(VrStateChanged fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_on_vr_state_changed = std::move(fn);
    }
    // Pushes VrState's per-core auto-frame-skip toggles down to whichever
    // backend is (or will be) active — same shape as
    // AudioChannelVolumeApplier, and wired the same way in
    // questretrodepth_main.cpp (the one file that already links every
    // concrete backend and knows the active/wanted BackendKind at each call
    // site).
    using AutoFrameSkipApplier = std::function<void(const VrState&)>;
    void set_auto_frame_skip_applier(AutoFrameSkipApplier fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_auto_frame_skip_applier = std::move(fn);
    }
    using PsxGpuResolutionApplier = std::function<void(int scale)>;
    using PsxRenderPathCtrl = std::function<void(int path)>;
    void set_psx_render_path_ctrl(PsxRenderPathCtrl fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_psx_render_path_ctrl = std::move(fn);
    }
    int psx_render_path() const { return m_vr_state.psx_render_path; }
    void apply_psx_render_path();
    // Snaps VrState::vr_resolution_scale and schedules the eye swapchains to be
    // rebuilt at the new size. The rebuild itself is deferred to between frames
    // (m_pending_swapchain_rebuild): the menu is drawn from inside a frame that
    // already holds acquired swapchain images, so tearing them down there would
    // pull the ground out from under the frame being rendered.
    void apply_vr_resolution_scale();
    void set_psx_gpu_resolution_applier(PsxGpuResolutionApplier fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_psx_gpu_resolution_applier = std::move(fn);
    }
    // SwanStation texture filtering, by VrState::psx_texture_filter index.
    using PsxTextureFilterApplier = std::function<void(int index)>;
    void set_psx_texture_filter_applier(PsxTextureFilterApplier fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_psx_texture_filter_applier = std::move(fn);
    }
    int psx_texture_filter() const { return m_vr_state.psx_texture_filter; }
    void apply_psx_texture_filter();
    using ExperimentalRumbleChanged = std::function<void(bool enabled)>;
    void set_on_experimental_rumble_changed(ExperimentalRumbleChanged fn) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_on_experimental_rumble_changed = std::move(fn);
    }

    // rom_name_hint: the ROM about to be loaded's filename or original path.
    // Pass this when calling before set_current_rom() has run -- classifiers
    // like is_neogeo_mame_rom_name() key off the current ROM name, and
    // set_current_rom() is normally called *after* this, so without the hint
    // this would always classify against the *previous* ROM (or empty on the
    // first load), never the one actually being loaded. Falls back to
    // m_current_rom_name when empty, for callers that aren't mid-ROM-switch.
    void set_current_backend_kind(BackendKind kind, const std::string& rom_name_hint = std::string());
    void set_current_game_name(const std::string& name);
    void enqueue_haptic(bool right, float amplitude, int duration_ms);
    void enqueue_haptic(const QueuedHapticEvent& event);
    void set_experimental_rumble_status(const std::string& status);

    struct QuickSettingsPreset {
        std::string name;
        float canvas_x = 0.0f;
        float canvas_y = 0.0f;
        float canvas_az = 0.0f;
        float canvas_el = 0.0f;
        float canvas_scale = 1.0f;
        float near_depth = 1.0f;
        float far_depth = 1.8f;
        float quad_width = 2.56f;
        int copy_count = 1;
        bool immersive_beta_enabled = false;
        UpscaleMode upscale_mode = UpscaleMode::Off;
        bool ambilight = true;
        bool passthrough = false;
        DepthMode depth_mode = DepthMode::Off;
        bool layers_3d = false;
        float gamma = 1.5f;
        float contrast = 0.85f;
        float saturation = 1.00f;
        float brightness = 1.00f;
        bool perspective_comp = true;
        EnvironmentSphereMode environment_sphere_mode = EnvironmentSphereMode::Off;
    };

    struct QuickLayerPreset {
        std::string name;
        std::vector<std::string> ordered_ids;
        std::vector<bool> enabled;
        std::vector<bool> ambilight;
        std::vector<float> depths;
    };

    struct EnvironmentSphereSample {
        bool valid = false;
        std::array<std::array<float, 4>, 12> bands{};
    };

    enum class BlackoutRevealPhase {
        Normal = 0,
        BlackoutLatched,
        RevealAnimating,
        RevealCooldown,
    };

    enum class RomTransitionPhase {
        None = 0,
        FadeOut,
        FadeIn,
    };

    void homebrew_data_ready()        { m_hw_loading = false;     m_hw_dirty = true; }
    void homebrew_download_complete() { m_hw_downloading = false; m_hw_dirty = true; }
    void set_homebrew_feed(int idx)   { m_hw_feed = idx < 0 ? 0 : idx; m_hw_loading = true; m_hw_dirty = true; m_hw_view = 0; m_hw_hovered = -1; m_hw_scroll = 0; }

private:
    void set_status(const std::string& s);
    bool start_common(JavaVM* vm, JNIEnv* env, jobject activity, bool open_menu_on_startup,
                      int autosave_interval_seconds, bool load_last_save_enabled,
                      std::string& status_out);

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
    void load_controller_render_models(); // XR_FB_render_model (optional) -- see openxr_shell.cpp
    bool m_controller_models_loaded = false; // one-shot guard, see poll_events()
    void sync_passthrough_state();
    bool passthrough_active() const;

    // ---------- run loop ----------
    void run();
    void poll_events(bool& exit);
    void poll_actions();
    void render_frame(XrTime predicted_time);
    void update_live_layer_canvas_interaction(const VrState& render_state);
    void append_live_layer_canvas_guides(OverlayInfo& overlay, const VrState& render_state);
    // Unified-menu ImGui rail/tab framework (see the Panel UI Migration plan).
    // Placeholder tabs only for now — real settings move in tab-by-tab.
    void draw_unified_menu();
    void draw_depth_arrangement_widget(); // Layers > Stack — see m_layer_slot_fraction above
    void draw_layer_control_rows();       // Layers > Stack — per-layer Visibility/Ambilight/Side Color, favoritable
    void draw_config_files_group();       // System > Config Files — real save_settings()/load_settings() buttons
    void draw_theme_row();                // Interface > Theme — real m_ui_theme (ui_theme.h), shared with the old Kotlin panels
    void draw_camera_position_group();    // Layers > Camera Position — real m_canvas_x/m_canvas_y/m_canvas_scale
    void draw_library_rom_list();         // Library > Browse & Launch — real searchable list over m_rom_browser
    void draw_rom_preview_sidebar();      // right-hand sidebar — bottom half of the diorama live preview from Library
    void draw_controls_group();           // Controls > Controller Map — real m_button_map remap grid
    void draw_experimental_notice();      // Experimental > standing warning banner
    void draw_motion_controls_group();    // Experimental > Motion Controls
    void draw_motion_binding(int qi, const char* lamp_label);
    void draw_lightgun_group();           // Controls > Lightgun — five-point calibration
    void draw_save_state_group();         // Save States > Slots — real save/load slot grid + autosave/autoload
    void draw_presets_group();            // System > Presets — real m_presets (load_preset/save_preset)
    void draw_danger_zone_group();        // System > Danger Zone — real file counts/sizes + confirmed wipe
    void draw_exit_group();               // System > Exit — quit the app
    void draw_audio_channels_group();     // Audio > Channels — per-core ROM audio channel volumes
    void draw_frame_skip_group();         // Visuals > Frame Skip — per-core auto-frame-skip toggles
    // Pushes m_vr_state's per-core auto-frame-skip toggles down to the
    // active backend. Called whenever a checkbox changes, and once after a
    // ROM loads/at startup (a freshly created backend starts disabled).
    void apply_auto_frame_skip();
    // Pushes the selected PSX internal GPU resolution to the active backend.

    void apply_psx_gpu_resolution();
    // Pushes m_vr_state's per-core channel volumes down into each backend's
    // native mixer. Called whenever a slider/the master toggle changes, and
    // once after a ROM loads (a freshly created backend starts at its own
    // defaults). When audio_channel_split_enabled is false, every channel is
    // forced to 1.0 regardless of the stored slider values.
    void apply_audio_channel_volumes();
    void apply_pending_vr_changes();
    void recenter_to_hmd();                    // snap canvas to current HMD gaze direction
    void open_rom_menu();                      // scan ROMs + place main menu panel in front of HMD
    void poll_rom_load_completion();
    void start_async_rom_preparation(const std::string& path);
    float rom_transition_alpha();
    void open_homebrew_panel(bool fetch_feed);
    void open_credits_panel();
    // Reads assets/credits.txt (via Kotlin) and parses it into m_credit_entries.
    void load_credits_entries();
    void rebuild_credits_panel_texture();
    // Opens the url for credit entry `entry_index` (an index into
    // m_credit_entries, not a visible row) in the system/Meta browser via the
    // Kotlin side (ACTION_VIEW), same mechanism as the homebrew panel's "open
    // website" button. No-op for entries without a url.
    void open_credits_link(int entry_index);
    // Calls a no-arg void method on the Kotlin activity (bgmEnterMenu/bgmExitMenu/
    // bgmDuck/bgmUnduck), attaching the current thread to the JVM if needed.
    void call_activity_void(const char* method);
    void call_activity_float(const char* method, float value);
    void enter_manual_edit_mode();             // enter controller-driven canvas edit mode
    void apply_quick_settings_preset(int idx);
    bool apply_quick_layer_preset(int idx, std::string& status_out);
    void request_quick_settings_preset_save(int idx);
    void request_quick_layer_preset_save(int idx);
    void reset_quick_settings_presets();
    void reset_quick_layer_presets();
    void refresh_quick_layer_presets();
    void rebuild_main_menu_texture();          // call Kotlin → upload GL texture for main menu
    void rebuild_quick_edit_panel_texture();   // call Kotlin → upload GL texture for quick preset panel
    void rebuild_side_bar_texture();           // call Kotlin → upload GL texture for the always-visible Side Panels bar
    void rebuild_bg_color_panel_texture();     // call Kotlin → upload GL texture for the Background Color preset grid
    void rebuild_themes_panel_texture();       // call Kotlin → upload GL texture for the UI theme picker
    void rebuild_layer_panel_texture();        // call Kotlin → upload GL texture
    void rebuild_settings_panel_texture();     // call Kotlin → upload GL texture
    void rebuild_save_state_panel_texture();   // call Kotlin → upload GL texture
    void rebuild_code_panel_texture();         // call Kotlin → upload GL texture
    void rebuild_ctrlmap_panel_texture();      // call Kotlin → upload GL texture
    void rebuild_help_panel_texture();         // call Kotlin → upload GL texture
    void rebuild_rom_hint_texture();           // same JNI path, fixed 2-line tips content
    void refresh_rom_preview_jobs();
    // Enters the currently-hovered folder immediately (no blocking) and
    // queues background caching for every ROM directly inside it. Live
    // hover-preview is available right away; a live request preempts the
    // in-flight cache job, and an uncached card falls back to its cached
    // (or placeholder) art until the first live frame arrives.
    void enter_folder_and_queue_caching();
    // Draws the Library hover-dwell preview's per-layer quads in world space
    // beside the ImGui menu panel — ImGui renders one flat texture, so this
    // is the only place the preview can actually show depth.
    void build_library_preview_diorama(OverlayInfo& overlay, const XrPosef& menu_pose,
                                       float menu_w, float menu_h, float alpha);
    bool begin_rom_preview_session();
    void end_rom_preview_session(bool committed);
    // Hover-dwell live preview for the flat Library > Browse & Launch list
    // (draw_library_rom_list()/draw_rom_preview_sidebar()) — reuses the same
    // RomPreviewManager/session/BGM-duck machinery as the 3D Shelf's
    // laser-hover path, just triggered by a 0.5s ImGui hover-dwell instead.
    void start_library_live_preview(const std::string& path);
    void update_library_live_preview();
    void stop_library_live_preview();
    void rebuild_homebrew_panel_texture();     // call Kotlin → upload GL texture
    void rebuild_dashboard_left_panel_texture(); // call Kotlin → upload GL texture for manual dashboard left panel
    std::string get_settings_dir();            // call Kotlin → returns settings directory path
    void save_settings(bool game_scope);       // save current state to disk
    void load_settings(bool game_scope);       // load state from disk (if file exists)
    void reset_settings();                     // reset to hardcoded defaults
    void wipe_all_settings();                  // delete every saved .ini on disk + reset to defaults
    void apply_layer_filter_mode(LayerFilterMode mode, bool restore_saved_state);
    void set_mame_composition_mode(int mode);
    void sync_layer_capture_mask();
    void refresh_save_state_slots();
    bool save_state_to_slot(int slot, std::string& error_out);
    bool load_state_from_slot(int slot, std::string& error_out);
    bool save_state_to_path(const std::string& path, std::string& error_out);
    bool load_state_from_path(const std::string& path, std::string& error_out);
    bool try_load_latest_state(std::string& loaded_name_out, std::string& error_out, bool& found_any);
    void persist_save_automation_settings();
    // Reads back the "load_last_rom" key from the same save_automation.ini
    // persist_save_automation_settings() writes -- m_load_last_rom_enabled
    // isn't passed in via native_start_vr() like m_load_last_save_enabled is
    // (Kotlin's onResume() reads the file directly for its own startup
    // decision, before native even starts), so this is the only way native
    // picks up the persisted value for the Save States tab's checkbox.
    void load_last_rom_setting();
    void maybe_run_autosave();
    void reset_emulation_cache_for_rom_change();
    void flush_pending_haptics();
    // right=true → right controller, false → left; amplitude 0-1, duration_ms
    void fire_haptic(bool right, float amplitude = 0.4f, int duration_ms = 40);
    void fire_lightgun_vibration(bool right, int mode);
    bool m_gun2_trigger_prev = false;
    void shutdown();
    void mark_visual_state_dirty();
    void reset_panel_pose_defaults();

    // ---------- Android / JNI ----------
    JavaVM*  m_vm              = nullptr;
    jobject  m_activity_global = nullptr;

    std::thread        m_thread;
    std::thread        m_rom_load_thread;
    std::atomic<bool>  m_stop_requested{false};
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_rom_load_in_progress{false};
    std::atomic<bool>  m_rom_load_completion_pending{false};
    std::atomic<bool>  m_rom_load_release_required{false};

    mutable std::mutex m_mutex;
    std::string        m_status = "VR shell idle.";
    std::string        m_rom_load_path;
    std::string        m_rom_load_message;
    // Per-file extraction history for the current load (see set_rom_load_progress).
    std::vector<std::string> m_rom_load_extract_history;
    std::string        m_rom_load_prepared_path;
    std::string        m_rom_load_error;
    bool               m_rom_load_ok = false;
    std::atomic<bool>  m_rom_load_panel_dirty{false};
    // Live-preview archive-extraction progress (see set_preview_extract_target).
    std::string        m_preview_extract_path;
    std::string        m_preview_extract_message;
    FrameProvider      m_frame_provider;
    VrStateChanged     m_on_vr_state_changed;
    AutoFrameSkipApplier m_auto_frame_skip_applier;
    PsxRenderPathCtrl m_psx_render_path_ctrl;
    PsxGpuResolutionApplier m_psx_gpu_resolution_applier;
    PsxTextureFilterApplier m_psx_texture_filter_applier;
    ExperimentalRumbleChanged m_on_experimental_rumble_changed;
    float              m_active_refresh_rate = 0.0f;
    float              m_last_render_ms      = 0.0f;
    float              m_avg_render_ms       = 0.0f;
    float              m_max_render_ms       = 0.0f;
    uint64_t           m_render_sample_count = 0;

    // ---------- Perf overlay (VrState::side_panel_mode == 2) ----------
    // Frame timestamps for instant + rolling average FPS. Old entries outside the longest
    // one-minute window are dropped each frame.
    std::deque<double> m_perf_frame_times;
    float              m_perf_fps_instant  = 0.0f;
    float              m_perf_fps_avg10s   = 0.0f;
    float              m_perf_fps_avg30s   = 0.0f;
    float              m_perf_fps_avg60s   = 0.0f;
    // CPU%/RAM sampled periodically (reading /proc is not free) rather than every frame.
    XrTime             m_perf_last_sample_time = 0;
    long               m_perf_last_cpu_jiffies = -1; // utime+stime from /proc/self/stat
    double             m_perf_last_sample_wall = 0.0;
    float              m_perf_cpu_percent  = 0.0f;
    float              m_perf_ram_mb       = 0.0f;
    int                m_perf_layer_count  = 0;
    int                m_perf_object_count = 0;
    std::uint64_t      m_perf_estimated_pixels = 0;
    // Updates m_perf_fps_instant/avg5s every frame, and CPU%/RAM on a slower cadence. Cheap
    // enough to call unconditionally but only actually samples /proc when the perf overlay side
    // panel is active (caller gates that).
    void update_perf_stats();

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
    int     m_credits_link_armed_index = -1;   // first tap on a credits link arms it; -1 = none armed
    XrTime  m_credits_link_arm_time    = 0;
    bool    m_wipe_settings_armed     = false; // first tap on "Wipe Settings" arms it
    XrTime  m_wipe_settings_arm_time  = 0;     // arming expires after a few seconds
    XrTime  m_wipe_settings_done_until = 0;    // label shows "Done" until this time, then reverts

    // ---------- ROM browser panel ----------
    RomBrowser  m_rom_browser;
    qrd::RomPreviewManager m_rom_preview;
    bool        m_menu_open  = false;
    bool        m_bgm_menu_open_prev = false;
    // Background-music edge-detect state: previous-frame values so bgmEnterMenu/
    // bgmExitMenu/bgmDuck/bgmUnduck fire exactly once per real transition, no
    // matter which of the many call sites changed m_menu_open or the live-preview
    // hover state this frame.
    bool        m_bgm_should_play_prev = false;
    bool        m_bgm_live_active      = false;
    bool        m_bgm_live_active_prev = false;
    std::string m_settings_dir;       // cached path to settings directory
    std::string m_current_rom_name;   // filename stem of currently loaded ROM (for per-game settings)
    std::string m_current_rom_path;   // original path used to reload the current ROM
    std::string m_current_mame_path_hint; // original path used for authoritative MAME subfolder profiles
    std::string m_current_game_name;  // from ROM header (0xFFC0+) for version fallback
    BackendKind m_current_backend_kind = BackendKind::Snes;
    // Session-only MAME composition selector: 0=Flat, 1=OCCUPXY.
    int  m_mame_composition_mode = 0;
    bool m_mame_occupancy_eligible = false;
    bool m_mame_occupancy_available = false;
    bool m_mame_occupancy_valid = false;
    bool        m_experimental_rumble_enabled = true;
    std::string m_experimental_rumble_status = "OFF";

    // Lightgun support: m_gun_capable is set once per ROM load (set_current_backend_kind)
    // by matching m_current_rom_name against known gun-peripheral titles. m_gun_hand is
    // player-controlled: 0=off, 1=right controller, 2=left controller (for left-handed
    // play) -- cycled by a left-thumbstick-click while a gun-capable ROM is loaded, and
    // reset to 1 (right) whenever a new gun-capable ROM loads. When non-zero, the chosen
    // controller's aim ray (after m_gun_recenter_quat) is raycast against the game screen
    // each gameplay frame and fed to the backend as RETRO_DEVICE_LIGHTGUN screen
    // coordinates, and a blocky gun model is rendered attached to that controller.
    // m_gun_manual_override lets the player force lightgun mode on for ROMs the
    // title-matching in rom_is_lightgun_capable() didn't recognize (renamed/hacked
    // ROMs, or scope games not in the list). Toggled by holding the left thumbstick
    // click for kGunOverrideHoldMs; effective gun mode is m_gun_capable, which gets
    // OR'd with this override and re-synced to the backend via m_gun_mode_ctrl.
    bool           m_gun_manual_override = false;
    // Dual wielding: a second lightgun on whatever port the current system
    // keeps one on, with the other controller aiming it.
    bool           m_dual_gun_enabled = false;
    // Whether the loaded backend/ROM has a second gun to plug in at all --
    // backend_supports_dual_gun() at load time. False hides the option rather
    // than offering a toggle that could not do anything.
    bool           m_dual_gun_supported = false;
    // Set by apply_vr_resolution_scale(), consumed by the frame loop.
    bool           m_pending_swapchain_rebuild = false;
    // Non-zero while "Calibrate Gun 2" has borrowed m_gun_hand; restored when
    // the five-point flow finishes.
    int            m_gun_calibration_restore_hand = 0;
    bool           m_gun2_render_show = false;
    XrPosef        m_gun2_render_pose = {{0,0,0,1},{0,0,0}};
    // Player two's own shot animation. Player one's envelope lives in
    // m_gun_recoil/m_gun_animation_start and is driven by its own trigger, so
    // sharing it would have made each gun kick on the other player's shots.
    XrTime         m_gun2_animation_start = 0;
    float          m_gun2_recoil = 0.0f;
    bool           m_gun_capable_auto = false; // result of rom_is_lightgun_capable() at load time
    bool           m_gun_capable = false;       // effective: m_gun_capable_auto || m_gun_manual_override
    int            m_gun_hand = 0;
    XrQuaternionf  m_gun_recenter_quat = {0.0f, 0.0f, 0.0f, 1.0f};
    // Gun model render state, updated each gameplay frame alongside m_input_state.gun_*
    // and consumed by the render pass (draw_gun_model): world pose of the aiming
    // controller with m_gun_recenter_quat applied, or show=false when gun_hand is off.
    bool           m_gun_render_show = false;
    XrPosef        m_gun_render_pose = {{0,0,0,1},{0,0,-1}};
    bool           m_gun_trigger_prev = false;
    float          m_gun_recoil = 0.0f;
    float          m_gun_tilt = 0.0f;
    XrTime         m_gun_animation_start = 0;
    int            m_gun_animation_mode = 1;
    // Low-poly pistol muzzle tint, re-rolled per shot (per burst in machinegun
    // mode). m_gun_muzzle_burst tracks which burst has already been coloured.
    float          m_gun_muzzle_color[3] = {0.55f, 0.56f, 0.58f};
    int            m_gun_muzzle_burst = -1;
    // Scope rifle muzzle heat, 0..1, on the same timeline as m_gun_recoil.
    float          m_gun_muzzle_heat = 0.0f;

    // D-Pad Headset latch state (Experimental > Motion Controls). Held across
    // frames so the hysteresis band has something to compare against.
    bool           m_dpad_headset_left  = false;
    bool           m_dpad_headset_right = false;
    bool           m_dpad_headset_up    = false;
    bool           m_dpad_headset_down  = false;

    // Air Wheel latch + calibration state (Experimental > Motion Controls).
    // Default-constructing the whole struct is how every motion is released at
    // once when the feature is disabled or tracking drops out.
    struct AirWheelState {
        bool steer_left = false, steer_right = false;
        bool accel = false, brake = false;
        bool gear_up = false, gear_down = false;
        bool handbrake = false, bike = false;
        // Low-passed chest anchor that accelerate/brake are measured from.
        XrVector3f anchor{0,0,0};
        bool  anchor_valid  = false;
        // Neutral "hands on the wheel" pose, captured once then used as the
        // zero point for every distance-based motion.
        bool  neutral_valid = false;
        float neutral_reach = 0.0f;
        float neutral_height = 0.0f;
        // Previous-frame sample, for the brake's inward-speed test.
        bool  last_time_valid = false;
        float last_reach = 0.0f;
        float last_time = 0.0f;
        float brake_hold_until = 0.0f;
    };
    AirWheelState  m_air_wheel;
    // Air Jump / Crouch latch + calibration state.
    struct AirJumpState {
        bool  jump = false, crouch = false;
        float jump_hold_until = 0.0f, crouch_hold_until = 0.0f;
        bool  neutral_valid = false;
        float neutral_height = 0.0f;
        bool  last_time_valid = false;
        float last_y = 0.0f, last_time = 0.0f;
    };
    AirJumpState   m_air_jump;
    void           update_air_jump();
    bool           locate_hand_poses(XrPosef& left, XrPosef& right);

    // ---- Air Fighter -------------------------------------------------
    // Recognised moves are not held latches like every other motion: they play
    // back as a SEQUENCE of directional steps. The queue is driven off frame
    // time rather than emulator frames, with each step held long enough
    // (kFightStepSeconds) for the 60Hz emulator to sample it reliably.
    //
    // Bit positions match the QI_FIGHT_* order, so a step is just a mask.
    enum FightBit : unsigned {
        FB_UP = 1u << 0, FB_DOWN = 1u << 1, FB_LEFT = 1u << 2,
        FB_RIGHT = 1u << 3, FB_PUNCH = 1u << 4, FB_KICK = 1u << 5,
        FB_PUNCH_HARD = 1u << 6, FB_KICK_HARD = 1u << 7,
    };
    struct FightMacro {
        static constexpr int kMaxSteps = 8;
        unsigned steps[kMaxSteps] = {};
        int   step_count = 0;
        int   cur = -1;          // -1 = idle
        float step_ends_at = 0.0f;
    };
    struct AirFighterState {
        FightMacro macro;
        // Previous-frame hand samples, for the velocity tests.
        bool  last_valid = false;
        float last_time = 0.0f;
        XrVector3f last_l{0,0,0}, last_r{0,0,0};
        // Charge move: which side is being held (-1 left, +1 right, 0 none),
        // by which hand, and since when.
        int   charge_dir = 0;
        int   charge_hand = -1;   // 0 = left, 1 = right
        float charge_since = 0.0f;
        bool  charge_ready = false;
        // Debounce so one big arm swing cannot fire twice.
        float rearm_at = 0.0f;
        const char* last_move = "";
        // A normal attack in flight. Light vs heavy is only distinguishable
        // AFTER the strike -- by whether the arm snapped back or stayed out --
        // so the strike is recorded here and resolved when the window expires.
        bool  pending = false;
        bool  pending_kick = false;
        int   pending_hand = -1;
        float pending_until = 0.0f;
        float pending_reach = 0.0f; // forward extension at the moment of impact
    };
    AirFighterState m_air_fighter;
    void            update_air_fighter();
    void            queue_fight_macro(const unsigned* steps, int count, const char* name);
    void           update_dpad_headset();
    void           update_air_wheel();
    void           recenter_air_wheel();
    void           roll_gun_muzzle_color();

    // Five-point lightgun calibration. Profiles are keyed by hand, backend,
    // output aspect, canvas placement/zoom, immersive tilt/curve, and world
    // locomotion so a calibration cannot silently move when the layout changes.
    std::vector<LightgunCalibrationProfile> m_gun_calibration_profiles;
    LightgunCalibrationProfile m_gun_calibration_active_profile{};
    bool m_gun_calibration_loaded = false;
    bool m_gun_calibration_profile_active = false;
    bool m_gun_calibration_active = false;
    bool m_gun_calibration_wait_release = false;
    bool m_gun_calibration_release_required = false;
    int  m_gun_calibration_target = -1;
    int  m_gun_calibration_sample_frames = 0;
    LightgunUv m_gun_calibration_sample_sum{};
    std::array<LightgunUv, LightgunCalibrationProfile::kPointCount> m_gun_calibration_captured{};
    LightgunCalibrationProfile m_gun_calibration_capture_context{};
    bool m_gun_calibration_trigger_prev = false;
    LightgunSurface m_gun_calibration_surface{};
    bool m_gun_calibration_surface_valid = false;

    void load_lightgun_calibration();
    void save_lightgun_calibration();
    void clear_lightgun_calibration();
    void begin_lightgun_calibration();
    void finish_lightgun_calibration();

    bool        m_menu_prev  = false;
    bool        m_rtrig_prev = false;
    std::string m_rom_dir;
    XrTime      m_last_rom_fire = 0;  // throttle for ROM browser rebuilds

    // Panel poses (set in open_rom_menu, index matches k_panel_* constants)
    static constexpr int k_panel_main_menu  = 0;
    static constexpr int k_panel_layers     = 1;
    static constexpr int k_panel_browser    = 2;
    static constexpr int k_panel_settings   = 3;
    static constexpr int k_panel_save_state = 4;
    static constexpr int k_panel_code       = 5;
    static constexpr int k_panel_ctrlmap    = 6;
    static constexpr int k_panel_quick_edit = 7;
    static constexpr int k_panel_homebrew  = 8;
    static constexpr int k_panel_manual_dashboard = 9;
    static constexpr int k_panel_side_bar   = 10; // always-visible Side Panels mode-select bar
    static constexpr int k_panel_bg_color   = 11; // Background Color preset grid
    static constexpr int k_panel_themes     = 12; // UI theme picker on lateral wings
    static constexpr int k_panel_credits    = 13; // credits / open-source projects list
    XrPosef m_main_menu_pose       = {{0,0,0,1},{0,0,-1}};
    XrPosef m_quick_panel_pose     = {{0,0,0,1},{0,0,-1}};
    XrPosef m_panel_pose           = {{0,0,0,1},{0,0,-1}}; // browser (centre)
    XrPosef m_layer_panel_pose     = {{0,0,0,1},{0,0,-1}};
    XrPosef m_settings_panel_pose  = {{0,0,0,1},{0,0,-1}};
    XrPosef m_save_state_panel_pose = {{0,0,0,1},{0,0,-1}};
    XrPosef m_code_panel_pose      = {{0,0,0,1},{0,0,-1}};
    XrPosef m_ctrlmap_panel_pose   = {{0,0,0,1},{0,0,-1}};
    bool    m_ctrlmap_mode         = false; // true = showing ctrlmap panel only

    XrPosef m_homebrew_panel_pose  = {{0,0,0,1},{0,0,-1}};

    // ---------- Credits panel ----------
    XrPosef m_credits_panel_pose = {{0,0,0,1},{0,0,-1}};
    GLuint  m_credits_tex        = 0;
    bool    m_credits_dirty      = true;
    int     m_credits_hovered    = -1;
    int     m_credits_scroll     = 0;
    int     m_credits_window_first   = 0; // index into m_credit_entries of the first visible row
    int     m_credits_window_visible = 0; // how many entries are currently visible (excludes Back row)
    std::vector<CreditRow> m_credit_entries; // parsed from assets/credits.txt
    std::vector<HelpRow>   m_help_entries;   // parsed from assets/help.txt
    bool                   m_help_loaded = false; // help.txt read attempted (may be empty)

    // ---------- Manual editor dashboard (wing-style placement) ----------
    XrPosef m_dashboard_left_pose  = {{0,0,0,1},{0,0,-1}};  // left wing
    XrPosef m_dashboard_right_pose = {{0,0,0,1},{0,0,-1}};  // right wing (layer panel on right)
    // Gameplay-time interactive side panels (Settings mode of the Side Panels cycle): left wing
    // is the real Settings panel, right wing is the quick-edit panel — recomputed every frame
    // (like the dashboard wings above) so the laser hit-test always matches what's rendered.
    XrPosef m_side_settings_left_pose  = {{0,0,0,1},{0,0,-1}};
    XrPosef m_side_settings_right_pose = {{0,0,0,1},{0,0,-1}};
    // Background Color mode: same texture (the preset grid) shown at both wing positions —
    // recomputed every frame like the poses above so the raycast and render pass agree.
    XrPosef m_bg_color_left_pose  = {{0,0,0,1},{0,0,-1}};
    XrPosef m_bg_color_right_pose = {{0,0,0,1},{0,0,-1}};
    XrPosef m_themes_left_pose    = {{0,0,0,1},{0,0,-1}};
    XrPosef m_themes_right_pose   = {{0,0,0,1},{0,0,-1}};
    GLuint  m_dashboard_left_panel_tex     = 0;
    bool    m_dashboard_left_panel_dirty   = true;
    int     m_dashboard_left_panel_hovered = -1; // row under laser
    XrTime  m_last_dashboard_fire = 0;
    XrTime  m_dashboard_hold_start = 0; // when the CURRENT held button started, for adaptive repeat
    int     m_dashboard_hold_row   = -1; // row the hold started on; a change resets the ramp
    bool    m_dashboard_hold_is_plus = false; // which button (Minus vs Plus) the hold started on
    PanelLayout m_dashboard_left_panel_layout;
    // Non-persisted in-memory duplication spacing (copy_step) for dashboard UI
    float   m_dashboard_duplication_spacing = 0.003f;

    // Main menu / standalone panel tracking
    // 0 = main menu, 1 = browser, 2 = layers, 3 = settings, 4 = save states, 5 = code, 6 = ctrlmap, 7 = quick edit, 8 = homebrew, 9 = manual dashboard
    int     m_active_sub_panel     = 0;

    // Multi-panel laser state (menu mode — right controller)
    bool        m_laser_hit    = false;
    XrVector3f  m_laser_origin = {0,0,0};
    XrVector3f  m_laser_end    = {0,0,0};
    int         m_laser_panel  = -1;  // which panel was hit (k_panel_*)
    float       m_laser_hit_u  = 0.0f;
    float       m_laser_hit_v  = 0.0f;
    PanelLayoutItem m_laser_hit_item{};
    bool        m_laser_hit_has_item = false;
    PanelLayout m_main_menu_layout;
    PanelLayout m_quick_panel_layout;
    PanelLayout m_layer_panel_layout;
    PanelLayout m_settings_panel_layout;
    PanelLayout m_save_state_panel_layout;
    PanelLayout m_code_panel_layout;
    PanelLayout m_ctrlmap_panel_layout;
    PanelLayout m_homebrew_panel_layout;
    PanelLayout m_themes_panel_layout;
    PanelLayout m_credits_panel_layout;

    // ---------- Homebrew panel ----------
    GLuint m_hw_tex         = 0;
    int    m_hw_view        = 0;   // 0 = list, 1 = detail
    int    m_hw_hovered     = -1;
    int    m_hw_scroll      = 0;
    int    m_hw_selected    = -1;
    int    m_hw_feed        = 0;
    bool   m_hw_dirty       = true;
    bool   m_hw_loading     = false;
    bool   m_hw_downloading = false;
    XrTime m_last_hw_fire   = 0;
    EnvironmentSphereSample m_environment_sphere_sample;
    EnvironmentSphereMode m_environment_sphere_sample_mode = EnvironmentSphereMode::Off;
    BlackoutRevealPhase m_blackout_reveal_phase = BlackoutRevealPhase::Normal;
    int m_blackout_candidate_frames = 0;
    int m_blackout_visible_frames = 0;
    XrTime m_blackout_reveal_start_time = 0;
    std::vector<std::string> m_blackout_reveal_layer_ids;
    RomTransitionPhase m_rom_transition_phase = RomTransitionPhase::None;
    std::uint64_t m_rom_transition_start_ms = 0;

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
    RomPreparer m_rom_preparer;
    RomPreparedPathPublisher m_rom_prepared_path_publisher;
    RomPreviewCapture m_rom_preview_capture;
    RomPreviewSessionBegin m_rom_preview_begin;
    RomPreviewSessionEnd m_rom_preview_end;
    ExtractedRomCacheClearer m_extracted_rom_cache_clearer;
    ExtractedRomCacheStats m_extracted_rom_cache_stats;
    AppExiter m_app_exiter;
    RomCloser m_rom_closer;
    AudioChannelVolumeApplier m_audio_channel_volume_applier;
    std::atomic<bool> m_rom_preview_session_active{false};


    // ---------- Side Panels mode-select bar (always visible, bottom-of-view, follows the
    // player) ---------- Detached from the Settings menu's row 18 buttons — this is the same
    // Persistent OFF/HELP/SETTINGS/PERF/BG COLOR/THEMES selector reachable without opening any menu,
    // dim (25%) until the laser hovers it, full opacity + clickable while hovered.
    GLuint  m_side_bar_tex       = 0;
    bool    m_side_bar_dirty     = true;
    XrTime  m_last_side_bar_fire = 0;
    XrPosef m_side_bar_left_pose  = {{0,0,0,1},{0,0,-1}};
    XrPosef m_side_bar_right_pose = {{0,0,0,1},{0,0,-1}};
    PanelLayout m_side_bar_layout;
    int     m_side_bar_hovered_id = -1; // -1 = not hovered
    bool    m_side_bar_hit_is_left = true; // which of the two duplicated copies is currently hit

    // ---------- Background Color side panel (8 solid + 8 gradient presets) ----------
    GLuint  m_bg_color_panel_tex       = 0;
    bool    m_bg_color_panel_dirty     = true;
    XrTime  m_last_bg_color_fire       = 0;
    PanelLayout m_bg_color_panel_layout;
    int     m_bg_color_last_hover_id   = -2; // -2 = uninitialized, forces first-frame dirty
    bool    m_bg_color_hit_is_left     = true; // which of the two duplicated wing copies is currently hit
    bool    m_side_bar_was_hovered = false; // last frame's hover state, to know when to redirty

    // UI-only theme preference. This deliberately never enters VrState, presets, or share codes.
    UiThemeId m_ui_theme = UiThemeId::PremiumRetroTech;
    GLuint    m_themes_panel_tex = 0;
    bool      m_themes_panel_dirty = true;
    int       m_themes_panel_hovered = -1;
    bool      m_themes_hit_is_left = true;
    XrTime    m_last_themes_fire = 0;

    // ---------- Quick preset panel ----------
    GLuint  m_quick_panel_tex       = 0;
    bool    m_quick_panel_dirty     = true;
    XrTime  m_last_quick_panel_fire = 0;
    std::vector<QuickSettingsPreset> m_quick_settings_presets;
    std::vector<QuickLayerPreset> m_quick_layer_presets;
    bool    m_settings_return_to_quick = false;
    bool    m_quick_preset_dialog_open = false;
    int     m_pending_quick_preset_kind = -1;
    int     m_pending_quick_preset_slot = -1;

    // ---------- Layer order panel ----------
    // Layer names/order/enabled are game-session state (not saved in presets)
    std::vector<std::string> m_layer_names;
    std::vector<int>         m_layer_order;   // m_layer_order[display_row] = original_idx
    // Depth Arrangement (new unified menu, Layers > Stack): one fraction
    // (0=nearest..1=farthest, RELATIVE to whatever the live per-frame
    // near/far depth envelope is -- not an absolute canvas-metres depth; see
    // apply_slot_fraction_layer_depths in presentation_shared.h) per SLOT —
    // indexed by display_row (rank in m_layer_order), not by original_idx.
    // This is deliberately the opposite convention from m_layer_enabled/
    // m_layer_ambilight/m_layer_side_color below, which travel with content
    // (original_idx) — a slot's depth position must stay put across
    // reorders, per the settled slot-vs-content design (see the Panel UI
    // Migration plan).
    std::vector<float> m_layer_slot_fraction;
    float m_canvas_depth_meters_ui = 2.0f;
    bool  m_thickness_overlap_ui = false;
    std::vector<bool>        m_layer_enabled;   // indexed by original_idx
    std::vector<bool>        m_layer_ambilight; // indexed by original_idx
    // Per-layer side/back face color override for real-geometry (PixelExtrude/PixelFx) boxes.
    // 0=Ori (sample the real texture, current behaviour) 1=Black 2=White 3=Red 4=Green 5=Blue.
    // Indexed by original_idx, same convention as m_layer_ambilight.
    std::vector<int>         m_layer_side_color;
    int     m_layer_auto_dup_percent = 75; // -1 = OFF, otherwise percentage target for farthest layer
    LayerFilterMode m_layer_filter_mode = LayerFilterMode::Hybrid;
    GLuint  m_layer_panel_tex     = 0;
    bool    m_layer_panel_dirty   = true;
    // TEMP TESTING FLAG: when true, any layer with zero opaque pixels this
    // frame (LayerFrame::has_pixels == false) is skipped entirely -- not
    // rendered AND not listed in the manual layers panel -- instead of
    // showing up as a normal, just-currently-empty entry. Flip to true to
    // test; default false since this changes what "exists" in the panel,
    // not just what's visible.
    bool    m_debug_hide_empty_layers = false; // TEMP: was left enabled by default, which hid the manual layers panel entirely on anything slow to produce a frame (e.g. Saturn); see comment above
    int     m_layer_panel_hovered = -1; // display row under laser
    int     m_layer_panel_grabbed = -1; // display row being dragged (-1 = none)
    int     m_layer_depth_selected = -1; // display row in depth-edit mode (-1 = none)
    // Live layer canvas interaction. Slots are the current near-to-far display
    // positions in m_layer_order; selection is performed directly against the
    // real rendered layer planes, not against a proxy UI panel.
    int     m_live_layer_grabbed_slot = -1;
    // Each horizontal stick gesture moves the selected layer once. A new move
    // requires the stick to pass back through neutral, preventing a held stick
    // from repeatedly reordering the stack every frame.
    int     m_live_layer_lstick_move_dir = 0;
    int     m_live_layer_rstick_move_dir = 0;
    int     m_live_layer_hovered_slot = -1;
    int     m_live_layer_flash_slot = -1;
    XrTime  m_live_layer_flash_until = 0;
    XrVector3f m_live_layer_laser_origin = {0,0,0};
    XrVector3f m_live_layer_laser_end = {0,0,0};
    bool    m_live_layer_laser_hit = false;
    XrTime  m_last_layer_fire     = 0;
    // Adaptive hold-repeat state for the per-layer DistMinus/DistPlus/ThickMinus/ThickPlus
    // buttons (see adaptive_repeat_interval_ns).
    XrTime  m_last_layer_adj_fire  = 0;
    XrTime  m_layer_adj_hold_start = 0;
    int     m_layer_adj_hold_row   = -1; // display row the hold started on
    PanelRole m_layer_adj_hold_role = PanelRole::None; // which of the 4 buttons the hold started on
    bool    m_emu_frozen_display  = false; // frozen state for play/pause button display
    bool    m_auto_pause_ui_open_prev = false; // edge-detect for menu-open auto-pause
    struct LayerModeSnapshot {
        bool valid = false;
        LayerFilterMode mode = LayerFilterMode::Hybrid;
        GameConfig config;
        std::vector<int> order;
        std::vector<bool> enabled;
        std::vector<bool> ambilight;
        std::vector<int> side_color;
    } m_saved_layer_mode_state;

    // ---------- Settings panel ----------
    GLuint  m_settings_panel_tex     = 0;
    bool    m_settings_panel_dirty   = true;
    int     m_settings_panel_hovered = -1;
    int     m_settings_panel_area    = 0; // 0=none 1=minus 2=plus
    int     m_settings_panel_hovered_id = -1; // hovered item's id (used by row 18's 5 side-panel buttons)
    XrTime  m_last_settings_fire     = 0;
    std::vector<QueuedHapticEvent> m_pending_haptics;

    // ---------- Save-state panel ----------
    GLuint  m_save_state_panel_tex     = 0;
    bool    m_save_state_panel_dirty   = true;
    int     m_save_state_panel_hovered = -1; // 0-7 interactive cell/row
    std::vector<SaveStateSlotInfo> m_save_state_slots;
    int     m_autosave_interval_seconds = 30;
    bool    m_load_last_save_enabled = true;  // when a ROM opens, auto-load its most recent save-state slot
    bool    m_load_last_rom_enabled = true;   // when the APK launches, auto-boot the last-played ROM (read by Kotlin's onResume(), not native)
    std::uint64_t m_last_autosave_time_ms = 0;
    std::atomic<bool> m_autosave_in_progress{false};

    // ---------- Code-input panel (floats above ROM browser) ----------
    GLuint      m_code_panel_tex     = 0;
    bool        m_code_panel_dirty   = true;
    int         m_code_panel_hovered = -1; // index into k_code_keys (-1 = none)
    XrTime      m_last_code_fire     = 0;
    std::string m_code_input_buf;          // chars typed so far (≤ 20)
    bool        m_code_panel_quick_name_mode = false;
    // ---------- Controller map panel ----------
    GLuint  m_ctrlmap_panel_tex     = 0;
    bool    m_ctrlmap_panel_dirty   = true;
    int     m_ctrlmap_panel_hovered = -1; // row under laser (-1=none)
    int     m_ctrlmap_selected_row  = -1; // row being remapped (-1=none)
    ButtonMap m_button_map;               // current mapping (SNES→QuestInput)

    // ---------- Passive side help panels ----------
    GLuint      m_help_panel_tex   = 0;
    bool        m_help_panel_dirty = true;
    XrTime      m_last_help_fire   = 0;
    std::string m_help_panel_key;

    // ---------- ROM-load tips panel: shown centred in front of the player for a
    // few seconds each time a ROM finishes loading, reminding of the two least
    // discoverable controls (world-move grip, quick menu click). Reuses the same
    // renderHelpPanelBitmap() JNI path as the side help wings, just with fixed
    // content and its own texture/timer so it doesn't fight the real help panel. ----------
    GLuint       m_rom_hint_tex        = 0;
    bool         m_rom_hint_dirty      = false;
    XrTime       m_last_rom_hint_fire  = 0;
    std::uint64_t m_rom_hint_hide_at_ms = 0; // 0 = not showing
    // When non-empty, the next show_rom_hint() displays this single message
    // instead of the fixed Tips list -- used for "no per-game depth layers,
    // showing flat 2D" and "missing BIOS file" notices. Cleared once shown so
    // a later ROM with nothing to report goes back to the normal Tips text.
    std::string  m_rom_hint_override_text;
    std::string  m_rom_hint_rendered_text; // text the cached texture currently shows

    // ---------- Perf overlay panel: FPS/CPU/RAM/GPU, shown on the left/right side wing panels
    // while VrState::side_panel_mode == 2. Reuses the same renderHelpPanelBitmap() JNI path,
    // refreshed on a timer since (unlike the other help-style panels) its content changes every
    // frame. ----------
    GLuint       m_perf_overlay_tex       = 0;
    bool         m_perf_overlay_dirty     = true;
    XrTime       m_last_perf_overlay_fire = 0;
    void rebuild_perf_overlay_texture();

    // Emulator freeze control callbacks (set by questretrodepth_main.cpp)
    EmuFreezeCtrl m_emu_freeze_ctrl;
    EmuStepOne    m_emu_step_one;
    EmuSetGunMode m_gun_mode_ctrl;
    EmuSetDualGunMode m_dual_gun_mode_ctrl;
    LayerCaptureMaskCtrl m_layer_capture_mask_ctrl;
    MameOccupancyCtrl m_mame_occupancy_ctrl;
    SaveStateCapture m_save_state_capture;
    SaveStateApply   m_save_state_apply;

    // Pending changes requested from JNI thread
    std::atomic<bool> m_randomize_pending{false};
    std::atomic<int>  m_preset_load_pending{-1};
    std::atomic<int>  m_preset_save_pending{-1};
    // ROM search dialog result — written by submit_rom_search_text() (JNI
    // callback thread) under m_mutex, consumed once by draw_library_rom_list()
    // on the XR thread via the pending flag (same pattern as m_rom_load_ok/
    // m_rom_load_error above, not a new pattern).
    std::atomic<bool> m_rom_search_result_pending{false};
    std::string        m_rom_search_result;

    // Library > Browse & Launch's hover state — text-only metadata (real
    // name/system/size, cheap stat() calls), deliberately NOT wired to
    // RomPreviewManager: the flat searchable list's row metadata must never
    // spin up the thumbnailer. The live preview is driven separately, by the
    // hover-dwell path (start_library_live_preview()).
    std::string m_library_preview_path;   // empty = nothing hovered
    std::string m_library_preview_name;
    std::string m_library_preview_system; // best-effort, from the ROM's parent folder — see draw_library_rom_list()
    std::string m_library_preview_size_str;
    // Layers actually uploaded for the world-space depth diorama this frame
    // (<= GlesRenderer::k_max_library_preview_layers), and the source frame's
    // aspect so the floating card is not stretched.
    int   m_library_preview_diorama_layers = 0;
    float m_library_preview_aspect = 4.0f / 3.0f;
    // The preview slot's rect inside the menu, normalised against the panel
    // texture, recorded in draw_rom_preview_sidebar(). The layer quads stand
    // out of exactly this rectangle toward the headset.
    bool  m_library_preview_rect_valid = false;
    float m_library_preview_rect_u0 = 0.0f, m_library_preview_rect_v0 = 0.0f;
    float m_library_preview_rect_u1 = 0.0f, m_library_preview_rect_v1 = 0.0f;
    // Hover-dwell live preview (see start_library_live_preview() /
    // update_library_live_preview() / stop_library_live_preview()). Unlike
    // the four fields above (instant, text-only), this group only activates
    // once the SAME row has been continuously hovered for 0.5s.
    float m_library_hover_dwell_start = -1.0f;  // ImGui::GetTime() when current row's hover began; -1 = inactive
    std::string m_library_hover_dwell_path;     // path the dwell timer is tracking
    bool m_library_live_preview_active = false;    // 0.5s dwell fired, live job running for m_library_preview_path
    bool m_library_preview_session_active = false; // whether THIS list opened the RomPreviewManager session
    std::string m_library_preview_uncompressed_size_str; // best-effort; may stay empty
    bool m_library_preview_is_archive = false;     // .zip/.7z filename check, set on hover
    float m_library_preview_reveal_t = 0.0f;       // 0..1 text/image reveal-fade progress
    bool m_library_preview_has_frame = false;      // at least one live snapshot uploaded to the texture
    int m_library_preview_layer_count = 0;
    // Shown/hidden by draw_library_rom_list()'s Search button and
    // draw_rom_search_keyboard()'s Close key; also dismissed automatically
    // when the laser hovers a ROM row, or when leaving the Library tab.
    bool m_rom_search_keyboard_open = false;
    std::atomic<int>  m_quick_settings_save_request_pending{-1};
    std::atomic<int>  m_quick_layers_save_request_pending{-1};
    std::atomic<int>  m_quick_settings_reset_pending{0};
    std::atomic<int>  m_quick_layers_reset_pending{0};
    std::atomic<int>  m_quick_named_save_kind_pending{-1};
    std::atomic<int>  m_quick_named_save_slot_pending{-1};
    std::string       m_quick_named_save_name; // guarded by m_mutex
    // Share-code apply: store the code string guarded by m_mutex, then set flag
    std::atomic<bool> m_apply_code_pending{false};
    std::string       m_pending_code;          // guarded by m_mutex
    // Settings I/O actions: 0=none, 1=save_game, 2=save_global, 3=load_game, 4=load_global, 5=reset, 6=calibrate_gun
    std::atomic<int>  m_settings_action_pending{0};
    // Flag: load global settings on next XR frame (set at startup)
    std::atomic<bool> m_load_global_pending{false};
    // Flag: open main menu on first frame after session starts
    std::atomic<bool> m_open_menu_on_startup{false};
    // Flag: load game settings for the newly set ROM (set by set_current_rom)
    std::atomic<bool> m_load_game_pending{false};
    std::atomic<bool> m_autoload_latest_save_pending{false};
    std::atomic<bool> m_request_open_menu{false};
    std::atomic<bool> m_request_open_homebrew{false};

    // Controller-driven input (written by XR thread, consumed by frame provider)
    std::mutex         m_input_mutex;
    EmulatorInputState m_input_state;

    // Raw per-hand physical controller state (not the remapped emulator
    // buttons in m_input_state) -- cached each poll_actions() only while
    // VrState::show_controller_models is on, and consumed when building the
    // frame's OverlayInfo in render_frame(). Index 0 = left, 1 = right.
    bool  m_ctrl_pose_valid[2]  = {false, false};
    XrPosef m_ctrl_pose[2]      = {{{0,0,0,1},{0,0,0}}, {{0,0,0,1},{0,0,0}}};
    bool  m_ctrl_btn_a[2]       = {false, false}; // X(left)/A(right)
    bool  m_ctrl_btn_b[2]       = {false, false}; // Y(left)/B(right)
    float m_ctrl_trigger[2]     = {0.0f, 0.0f};
    float m_ctrl_grip[2]        = {0.0f, 0.0f};
    float m_ctrl_stick_x[2]     = {0.0f, 0.0f};
    float m_ctrl_stick_y[2]     = {0.0f, 0.0f};
    bool  m_ctrl_stick_click[2] = {false, false};

    // Cached emulator frame/layers. Updated only when the emulator publishes a
    // new frame, so XR refreshes can reuse already processed Genesis layers.
    FrameOutput             m_cached_frame_out;
    uint64_t                m_cached_frame_seq = 0;
    // Counts published emulator frames while a panel UI is up, so layer
    // extraction can run at a reduced stride behind the panels instead of
    // being skipped entirely (see the process_layers block in render_frame).
    // Reset as soon as no panel is in use.
    int                     m_menu_layer_update_counter = 0;
    // True while the full menu, a standalone quick/manual panel, or a
    // laser-hovered side panel owns the laser (set from panel_ui_owns_laser
    // during input handling). Drives the throttle above.
    bool                    m_panel_ui_active = false;
    std::vector<LayerFrame> m_cached_layer_frames;
    std::vector<LayerFrame*> m_render_layer_refs;
    // Parallel to m_render_layer_refs: the depth-arrangement slot (m_layer_order
    // index) each pushed ref came from, for apply_slot_fraction_layer_depths.
    std::vector<int>         m_render_layer_slot;

    // Set each frame by render_frame()'s unified-menu panel block (on-demand
    // panel + the two automatic gameplay side panels) whenever that hand's
    // laser is currently intersecting ANY of them; read one frame later by
    // poll_actions() to suppress that hand's trigger from also firing as real
    // emulator/game input (SNES button etc.) while it's being used to click a
    // menu. One-frame-stale (poll_actions runs before render_frame each
    // frame) but that's an imperceptible ~11-16ms lag for a held/edge input.
    bool m_right_hand_menu_hover = false;
    bool m_left_hand_menu_hover  = false;

    // Refresh rate (0 = use headset default; otherwise Hz e.g. 72, 90, 120)
    float  m_desired_refresh_rate = 0.0f;
    std::atomic<bool> m_apply_refresh_pending{false};

    // Rate-limiting (nanoseconds wall-clock via XrTime)
    XrTime m_last_depth_fire  = 0;
    XrTime m_last_browser_row_scroll_fire = 0;
    XrTime m_last_browser_page_scroll_fire = 0;
    XrTime m_last_width_fire  = 0;
    XrTime m_last_copy_fire   = 0;
    bool   m_lstick_click_prev = false;
    bool   m_rstick_click_prev = false;
    XrTime m_rclick_press_time = 0;   // XrTime when right stick click began (0 = not held)
    bool   m_rclick_passthrough_fired = false; // true once the hold-threshold passthrough toggle has fired for the current press

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
    float m_canvas_scale = 1.0f; // screen size multiplier at the same depth
    // World locomotion, outside edit mode: hold left grip alone for everything, except when
    // the lightgun is assigned to the left hand (then left grip passes through to the game).
    // Left stick left/right turns (m_canvas_az) / up/down scales the whole environment
    // (m_world_scale,
    // depth + width together around the viewer origin); right stick pans the screen
    // (m_canvas_x / m_canvas_y); triggers throttle forward/backward facing the headset.
    float m_world_scale = 1.0f;
    float m_world_forward_offset = 0.0f; // metres, +ve = moved deeper into the scene (unused by current locomotion scheme, kept for save-file compat)
    XrTime m_last_locomotion_time = 0;   // 0 = not currently active (skip dt on activation frame)
    bool m_locomotion_l_was_active = false;
    // Free-roam camera/layer-pick mode. Toggled by a plain left-thumbstick
    // click (see poll_actions()) -- left grip is no longer the trigger, so
    // it's always available as a real, user-mappable gameplay button.
    bool m_locomotion_active = false;
    // Layer-deck bookshelf mode: DISABLED. The left-thumbstick click cycle is
    // now a plain gameplay <-> free-roam toggle and never enters the deck, so
    // m_layer_deck_active stays false for the whole session. Everything the
    // deck needs (yaw, picking, guides, spread control) is still compiled and
    // wired up -- flip this constant to true to put it back in the cycle.
    static constexpr bool k_layer_deck_enabled = false;
    bool m_layer_deck_active = false;
    // How far the bookshelf opens: a 0..1 fraction of the max per-slot yaw
    // (see presentation::layer_deck_yaw) -- 1.0 turns the deepest layer nearly
    // edge-on. Live left/right-stick-adjustable while m_layer_deck_active so
    // the user can dial it to a comfortable viewing angle instead of a fixed
    // one. Layers never move; only their facing changes.
    float m_layer_deck_spread = 0.35f;
    bool m_layer_grab_held = false;      // right trigger, sampled for live layer selection
    bool m_layer_grab_pressed = false;   // rising edge for selecting a layer
    // Throttle: while left grip is held, right trigger pushes the player forward along the
    // direction the right controller is pointing (visualized with a laser so the thrust
    // direction is visible), left trigger pushes backward.
    XrTime m_last_throttle_time  = 0;
    bool   m_throttle_was_active = false;
    // Thrust-direction laser: right controller's aim ray while left grip is held, drawn so the
    // player can see which way the throttle will push. Set each frame the grip is held, read by
    // the overlay-building code later in the same frame.
    bool       m_thrust_laser_active = false;
    XrVector3f m_thrust_laser_origin = {0,0,0};
    XrVector3f m_thrust_laser_dir    = {0,0,-1};
    // Predicted display time for the current frame (used to locate controllers)
    XrTime m_frame_predicted_time = 0;
    // Auto-recenter once on first valid HMD pose
    bool   m_initial_recenter_done = false;

    // ---------- OpenXR / EGL / GL resources (opaque impl) ----------
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace qrd
