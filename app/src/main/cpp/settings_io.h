#pragma once
// Simple flat-file settings persistence for QuestRetroDepth.
// Format: one "key=value\n" per line; unknown keys are silently ignored.
// Booleans: "1" or "0". Floats: decimal. Ints: decimal.
// Layer arrays use keys like "layer_depth_0", "layer_enabled_0", etc.

#include "vr_state.h"
#include "game_config.h"
#include "button_map.h"
#include <algorithm>
#include <string>
#include <vector>
#include "ui_theme.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

// ---- Wipe --------------------------------------------------------------

// Recursively deletes every "*.ini" file under root_dir (settings only —
// save data and ROM files never use the .ini extension). Returns the count
// of files removed.
static inline int settings_wipe_all_ini(const std::string& root_dir) {
    int removed = 0;
    DIR* dir = opendir(root_dir.c_str());
    if (!dir) return removed;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = root_dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            removed += settings_wipe_all_ini(path);
        } else if (name.size() > 4 && name.substr(name.size() - 4) == ".ini") {
            if (std::remove(path.c_str()) == 0) ++removed;
        }
    }
    closedir(dir);
    return removed;
}

// ---- Serialise -------------------------------------------------------

static inline void settings_write(FILE* f, const char* key, float v) {
    fprintf(f, "%s=%.6f\n", key, v);
}
static inline void settings_write(FILE* f, const char* key, bool v) {
    fprintf(f, "%s=%d\n", key, v ? 1 : 0);
}
static inline void settings_write(FILE* f, const char* key, int v) {
    fprintf(f, "%s=%d\n", key, v);
}
// String values must not themselves contain '\n' — callers are expected to use
// a delimiter like ';' internally (e.g. VrState::menu_favorites_csv) since the
// file format is one "key=value" per line.
static inline void settings_write(FILE* f, const char* key, const std::string& v) {
    fprintf(f, "%s=%s\n", key, v.c_str());
}

static inline bool ui_theme_save(const std::string& path, int theme) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "ui_theme=%d\n", std::clamp(theme, 0, qrd::kUiThemeCount - 1));
    fclose(f);
    return true;
}

static inline bool ui_theme_load(const std::string& path, int& theme_out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[128];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        int value = 0;
        if (sscanf(line, "ui_theme=%d", &value) == 1) {
            theme_out = std::clamp(value, 0, qrd::kUiThemeCount - 1);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

// Save all VrState fields + GameConfig layer depths/widths/copies
// + per-layer enabled/ambilight arrays + button map + optional refresh_rate (0 = headset default).
static inline bool settings_save(
    const std::string& path,
    const VrState& vs,
    const GameConfig& cfg,
    const std::vector<int>& layer_order,
    const std::vector<bool>& layer_enabled,
    const std::vector<bool>& layer_ambilight,
    const std::vector<int>& layer_side_color,
    int layer_filter_mode = -1,
    int layer_auto_dup_percent = -1,
    float refresh_rate = 0.0f,
    bool experimental_rumble_enabled = true,
    const qrd::ButtonMap* btn_map = nullptr,
    qrd::BackendKind btn_map_backend = qrd::BackendKind::Snes
    )
{
    qrd::ButtonMap snes_map = qrd::default_button_map_for_backend(qrd::BackendKind::Snes);
    qrd::ButtonMap genesis_map = qrd::default_button_map_for_backend(qrd::BackendKind::Genesis);
    qrd::ButtonMap gba_map = qrd::default_button_map_for_backend(qrd::BackendKind::Gba);
    qrd::ButtonMap gb_map = qrd::default_button_map_for_backend(qrd::BackendKind::Gb);
    if (FILE* existing = fopen(path.c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof(line), existing)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char* key = line;
            const int value = atoi(eq + 1);
            int bi = -1;
            if (sscanf(key, "btn_map_snes_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT)
                snes_map[bi] = value;
            else if (sscanf(key, "btn_map_genesis_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT)
                genesis_map[bi] = value;
            else if (sscanf(key, "btn_map_gba_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT)
                gba_map[bi] = value;
            else if (sscanf(key, "btn_map_gb_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT)
                gb_map[bi] = value;
            else if (sscanf(key, "btn_map_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT)
                snes_map[bi] = value;
        }
        fclose(existing);
    }
    if (btn_map) {
        if (btn_map_backend == qrd::BackendKind::Genesis)
            genesis_map = *btn_map;
        else if (btn_map_backend == qrd::BackendKind::Gba)
            gba_map = *btn_map;
        else if (btn_map_backend == qrd::BackendKind::Gb)
            gb_map = *btn_map;
        else
            snes_map = *btn_map;
    }

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;

    // Device settings
    settings_write(f, "refresh_rate", refresh_rate);
    settings_write(f, "experimental_rumble_enabled", experimental_rumble_enabled);

    // VrState scalars
    settings_write(f, "gamma",        vs.gamma);
    settings_write(f, "contrast",     vs.contrast);
    settings_write(f, "saturation",   vs.saturation);
    settings_write(f, "brightness",   vs.brightness);
    settings_write(f, "screen_curve", vs.screen_curve);
    settings_write(f, "tilt_x",       vs.tilt_x);
    settings_write(f, "tilt_y",       vs.tilt_y);
    settings_write(f, "immersive_beta_enabled", vs.immersive_beta_enabled);
    settings_write(f, "layers_3d",       vs.layers_3d);
    settings_write(f, "depth_mode",      (int)vs.depth_mode);
    settings_write(f, "upscale",         (int)vs.upscale_mode);
    settings_write(f, "passthrough",     vs.shadows);
    settings_write(f, "ambilight",       vs.ambilight);
    settings_write(f, "ambilight_placement", (int)vs.ambilight_placement);
    settings_write(f, "side_panel_mode", vs.side_panel_mode);
    settings_write(f, "gun_model",       vs.gun_model);
    settings_write(f, "gun_offscreen_reload_enabled", vs.gun_offscreen_reload_enabled);
    settings_write(f, "gun_offscreen_reload_button", vs.gun_offscreen_reload_button);
    settings_write(f, "gun_vibration_mode", vs.gun_vibration_mode);
    settings_write(f, "gun2_model", vs.gun2_model);
    settings_write(f, "gun2_vibration_mode", vs.gun2_vibration_mode);
    settings_write(f, "dpad_headset_enabled",   vs.dpad_headset_enabled);
    settings_write(f, "dpad_headset_threshold", vs.dpad_headset_threshold);
    settings_write(f, "air_wheel_enabled",           vs.air_wheel_enabled);
    settings_write(f, "air_wheel_steer_enabled",     vs.air_wheel_steer_enabled);
    settings_write(f, "air_wheel_steer_threshold",   vs.air_wheel_steer_threshold);
    settings_write(f, "air_wheel_accel_enabled",     vs.air_wheel_accel_enabled);
    settings_write(f, "air_wheel_brake_enabled",     vs.air_wheel_brake_enabled);
    settings_write(f, "air_wheel_push_threshold",    vs.air_wheel_push_threshold);
    settings_write(f, "air_wheel_brake_speed",       vs.air_wheel_brake_speed);
    settings_write(f, "air_wheel_adaptive_neutral",  vs.air_wheel_adaptive_neutral);
    settings_write(f, "motion_exclusive",           vs.motion_exclusive);
    settings_write(f, "air_jump_enabled",     vs.air_jump_enabled);
    settings_write(f, "air_jump_speed",       vs.air_jump_speed);
    settings_write(f, "air_jump_hold_margin", vs.air_jump_hold_margin);
    settings_write(f, "air_fighter_enabled",     vs.air_fighter_enabled);
    settings_write(f, "fight_qc_enabled",        vs.fight_qc_enabled);
    settings_write(f, "fight_qc_speed",          vs.fight_qc_speed);
    settings_write(f, "fight_dp_enabled",        vs.fight_dp_enabled);
    settings_write(f, "fight_dp_speed",          vs.fight_dp_speed);
    settings_write(f, "fight_charge_across_enabled", vs.fight_charge_across_enabled);
    settings_write(f, "fight_charge_up_enabled",     vs.fight_charge_up_enabled);
    settings_write(f, "fight_charge_distance",   vs.fight_charge_distance);
    settings_write(f, "fight_charge_seconds",    vs.fight_charge_seconds);
    settings_write(f, "fight_charge_speed",      vs.fight_charge_speed);
    settings_write(f, "fight_qck_enabled",       vs.fight_qck_enabled);
    settings_write(f, "fight_qck_speed",         vs.fight_qck_speed);
    settings_write(f, "fight_qck_purity",        vs.fight_qck_purity);
    settings_write(f, "fight_punch_enabled",     vs.fight_punch_enabled);
    settings_write(f, "fight_kick_enabled",      vs.fight_kick_enabled);
    settings_write(f, "fight_heavy_enabled",     vs.fight_heavy_enabled);
    settings_write(f, "fight_punch_speed",       vs.fight_punch_speed);
    settings_write(f, "fight_hold_seconds",      vs.fight_hold_seconds);
    settings_write(f, "fight_kick_ratio",        vs.fight_kick_ratio);
    for (int i = 0; i < VrState::kMotionBindCount; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "motion_bind_%d", i);
        settings_write(f, key, vs.motion_bind[i]);
    }
    settings_write(f, "air_wheel_gear_enabled",      vs.air_wheel_gear_enabled);
    settings_write(f, "air_wheel_gear_threshold",    vs.air_wheel_gear_threshold);
    settings_write(f, "air_wheel_handbrake_enabled", vs.air_wheel_handbrake_enabled);
    settings_write(f, "air_wheel_handbrake_threshold", vs.air_wheel_handbrake_threshold);
    settings_write(f, "air_wheel_bike_enabled",      vs.air_wheel_bike_enabled);
    settings_write(f, "air_wheel_bike_threshold",    vs.air_wheel_bike_threshold);
    settings_write(f, "bg_preset_index", vs.bg_preset_index);
    settings_write(f, "real_geometry_boxes", vs.real_geometry_boxes);
    settings_write(f, "silhouette_sides", vs.silhouette_sides);
    settings_write(f, "rom_preview_enabled", vs.rom_preview_enabled);
    settings_write(f, "bgm_enabled", vs.bgm_enabled);
    settings_write(f, "bgm_volume", vs.bgm_volume);
    settings_write(f, "audio_channel_split_enabled", vs.audio_channel_split_enabled);
    for (int i = 0; i < 8; ++i)
        settings_write(f, ("snes_voice_volume_" + std::to_string(i)).c_str(), vs.snes_voice_volume[i]);
    for (int i = 0; i < 2; ++i)
        settings_write(f, ("genesis_channel_volume_" + std::to_string(i)).c_str(), vs.genesis_channel_volume[i]);
    for (int i = 0; i < 5; ++i)
        settings_write(f, ("nes_channel_volume_" + std::to_string(i)).c_str(), vs.nes_channel_volume[i]);
    for (int i = 0; i < 3; ++i)
        settings_write(f, ("gba_channel_volume_" + std::to_string(i)).c_str(), vs.gba_channel_volume[i]);
    for (int i = 0; i < 6; ++i)
        settings_write(f, ("pce_channel_volume_" + std::to_string(i)).c_str(), vs.pce_channel_volume[i]);
    settings_write(f, "rotate_screen", vs.rotate_screen);
    settings_write(f, "surface_mode", vs.surface_mode);
    settings_write(f, "environment_sphere_mode", (int)vs.environment_sphere_mode);
    settings_write(f, "perspective_comp",    vs.perspective_comp);
    settings_write(f, "parallax_ratio",      vs.parallax_ratio);
    settings_write(f, "show_controller_models", vs.show_controller_models);
    settings_write(f, "auto_frame_skip_snes",    vs.auto_frame_skip_snes);
    settings_write(f, "auto_frame_skip_genesis", vs.auto_frame_skip_genesis);
    settings_write(f, "auto_frame_skip_mame",    vs.auto_frame_skip_mame);
    settings_write(f, "auto_frame_skip_saturn",  vs.auto_frame_skip_saturn);
    settings_write(f, "auto_frame_skip_pce",     vs.auto_frame_skip_pce);
    settings_write(f, "auto_frame_skip_gba",     vs.auto_frame_skip_gba);
    settings_write(f, "emu_resolution_scale", vs.emu_resolution_scale);
    settings_write(f, "psx_render_path", vs.psx_render_path);
    settings_write(f, "psx_gpu_resolution",  vs.psx_gpu_resolution);
    settings_write(f, "psx_texture_filter",  vs.psx_texture_filter);
    settings_write(f, "vr_resolution_scale",  vs.vr_resolution_scale);
    settings_write(f, "sprite_y_depth",        vs.sprite_y_depth);
    settings_write(f, "sprite_y_depth_spread", vs.sprite_y_depth_spread);
    settings_write(f, "audio_spatial_mode",    vs.audio_spatial_mode);
    settings_write(f, "audio_screen_lock",     vs.audio_screen_lock);
    settings_write(f, "layer_filter_mode", layer_filter_mode);
    settings_write(f, "layer_auto_dup_percent", layer_auto_dup_percent);
    settings_write(f, "menu_position_mode",     vs.menu_position_mode);
    settings_write(f, "menu_transparency_mode", vs.menu_transparency_mode);
    if (!vs.menu_favorites_csv.empty()) settings_write(f, "menu_favorites_csv", vs.menu_favorites_csv);
    // Depth Arrangement widget state — per-game (this file's own layer count is
    // what layer_slot_fractions_csv is meaningful against), unlike the menu_*
    // keys above which are only ever loaded back in the !game_scope path.
    settings_write(f, "canvas_depth_meters_ui", vs.canvas_depth_meters_ui);
    settings_write(f, "thickness_overlap_ui",   vs.thickness_overlap_ui);
    if (!vs.layer_slot_fractions_csv.empty())
        settings_write(f, "layer_slot_fractions_csv", vs.layer_slot_fractions_csv);

    // Layer geometry (from GameConfig, reflecting current depths/widths)
    int nlayers = (int)cfg.layers.size();
    settings_write(f, "num_layers", nlayers);
    for (int i = 0; i < nlayers; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "layer_depth_%d", i);
        settings_write(f, key, cfg.layers[i].depth_meters);
        snprintf(key, sizeof(key), "layer_width_%d", i);
        settings_write(f, key, cfg.layers[i].quad_width_meters);
        // copies
        snprintf(key, sizeof(key), "layer_copies_count_%d", i);
        settings_write(f, key, (int)cfg.layers[i].copies.size());
        for (int c = 0; c < (int)cfg.layers[i].copies.size(); ++c) {
            snprintf(key, sizeof(key), "layer_copy_%d_%d", i, c);
            settings_write(f, key, cfg.layers[i].copies[c]);
        }
        snprintf(key, sizeof(key), "layer_geom_mode_%d", i);
        settings_write(f, key, (int)cfg.layers[i].geometry_mode);
        snprintf(key, sizeof(key), "layer_thickness_%d", i);
        settings_write(f, key, cfg.layers[i].box_thickness_meters);
        snprintf(key, sizeof(key), "layer_split_px_%d", i);
        settings_write(f, key, cfg.layers[i].split_pixels);
        snprintf(key, sizeof(key), "layer_repeat_count_%d", i);
        settings_write(f, key, cfg.layers[i].repeat_count);
        snprintf(key, sizeof(key), "layer_scatter_range_%d", i);
        settings_write(f, key, cfg.layers[i].scatter_range);
        snprintf(key, sizeof(key), "layer_y_depth_range_%d", i);
        settings_write(f, key, cfg.layers[i].y_depth_range);
        snprintf(key, sizeof(key), "layer_order_%d", i);
        settings_write(f, key, (i < (int)layer_order.size()) ? layer_order[i] : i);
        snprintf(key, sizeof(key), "layer_enabled_%d", i);
        settings_write(f, key, (i < (int)layer_enabled.size()) ? (bool)layer_enabled[i] : true);
        snprintf(key, sizeof(key), "layer_ambilight_%d", i);
        settings_write(f, key, (i < (int)layer_ambilight.size()) ? (bool)layer_ambilight[i] : true);
        snprintf(key, sizeof(key), "layer_side_color_%d", i);
        settings_write(f, key, (i < (int)layer_side_color.size()) ? layer_side_color[i] : 0);
    }

    // Button mapping
    if (btn_map) {
        for (int i = 0; i < qrd::SNES_BUTTON_COUNT; ++i) {
            char key[64];
            snprintf(key, sizeof(key), "btn_map_snes_%d", i);
            settings_write(f, key, snes_map[i]);
            snprintf(key, sizeof(key), "btn_map_genesis_%d", i);
            settings_write(f, key, genesis_map[i]);
            snprintf(key, sizeof(key), "btn_map_gba_%d", i);
            settings_write(f, key, gba_map[i]);
            snprintf(key, sizeof(key), "btn_map_gb_%d", i);
            settings_write(f, key, gb_map[i]);
        }
    }

    fclose(f);
    return true;
}

// ---- Deserialise -------------------------------------------------------

static inline bool settings_load(
    const std::string& path,
    VrState& vs,
    GameConfig& cfg,
    std::vector<int>& layer_order,
    std::vector<bool>& layer_enabled,
    std::vector<bool>& layer_ambilight,
    std::vector<int>& layer_side_color,
    int* layer_filter_mode = nullptr,
    int* layer_auto_dup_percent = nullptr,
    float* refresh_rate = nullptr,
    bool* experimental_rumble_enabled = nullptr,
    qrd::ButtonMap* btn_map = nullptr,
    qrd::BackendKind btn_map_backend = qrd::BackendKind::Snes
    )
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    // We parse into a flat map first, then apply
    // Use simple line-by-line parsing; key and value separated by '='
    char line[256];
    int num_layers = -1;

    // First pass: read num_layers so we can size vectors correctly
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if (strcmp(key, "num_layers") == 0) {
            num_layers = atoi(val);
            break;
        }
    }

    if (num_layers > 0 && num_layers <= 16) {
        // Pre-size cfg.layers if needed (preserve existing layer configs; only update values)
        // Don't resize cfg.layers — just update the ones that exist
        layer_order.resize(num_layers);
        layer_enabled.resize(num_layers, true);
        layer_ambilight.resize(num_layers, true);
        layer_side_color.resize(num_layers, 6); // 6 = Darker, the shipped default
        for (int i = 0; i < num_layers; ++i) layer_order[i] = i;
    }

    // Second pass: rewind and read everything
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        auto readf = [&](float& dst) { dst = (float)atof(val); };
        auto readb = [&](bool& dst)  { dst = atoi(val) != 0; };
        auto readi = [&](int& dst)    { dst = atoi(val); };

        if      (strcmp(key,"refresh_rate")  == 0) { if (refresh_rate) *refresh_rate = (float)atof(val); }
        else if (strcmp(key,"experimental_rumble_enabled") == 0) { if (experimental_rumble_enabled) *experimental_rumble_enabled = atoi(val) != 0; }
        else if (strcmp(key,"gamma")        == 0) readf(vs.gamma);
        else if (strcmp(key,"contrast")     == 0) readf(vs.contrast);
        else if (strcmp(key,"saturation")   == 0) readf(vs.saturation);
        else if (strcmp(key,"brightness")   == 0) readf(vs.brightness);
        else if (strcmp(key,"roundness")    == 0) readf(vs.roundness);
        else if (strcmp(key,"screen_curve") == 0) readf(vs.screen_curve);
        else if (strcmp(key,"tilt_x")       == 0) readf(vs.tilt_x);
        else if (strcmp(key,"tilt_y")       == 0) readf(vs.tilt_y);
        else if (strcmp(key,"immersive_beta_enabled") == 0) readb(vs.immersive_beta_enabled);
        else if (strcmp(key,"layers_3d")       == 0) readb(vs.layers_3d);
        else if (strcmp(key,"solid_stack")     == 0) readb(vs.solid_stack);
        else if (strcmp(key,"depth_mode")      == 0) vs.depth_mode = (DepthMode)std::clamp(atoi(val), 0, 5);
        else if (strcmp(key,"depthmap")        == 0) vs.depth_mode = atoi(val) != 0 ? DepthMode::WholeLayer : DepthMode::Off;
        else if (strcmp(key,"upscale")         == 0) vs.upscale_mode = (UpscaleMode)std::clamp(atoi(val), 0, 2);
        else if (strcmp(key,"passthrough")     == 0) readb(vs.shadows);
        else if (strcmp(key,"ambilight")       == 0) readb(vs.ambilight);
        else if (strcmp(key,"ambilight_placement") == 0)
            vs.ambilight_placement = (AmbilightPlacement)std::clamp(atoi(val), 0, 3);
        else if (strcmp(key,"side_panel_mode") == 0) vs.side_panel_mode = std::clamp(atoi(val), 0, 5);
        else if (strcmp(key,"gun_model") == 0) vs.gun_model = std::clamp(atoi(val), 0, 2);
        else if (strcmp(key,"gun_offscreen_reload_enabled") == 0) readb(vs.gun_offscreen_reload_enabled);
        else if (strcmp(key,"gun_offscreen_reload_button") == 0)
            vs.gun_offscreen_reload_button = std::clamp(atoi(val), 0, VrState::kGunOffscreenReloadButtonCount - 1);
        else if (strcmp(key,"gun_vibration_mode") == 0)
            vs.gun_vibration_mode = std::clamp(atoi(val), 0, VrState::kGunVibrationModeCount - 1);
        else if (strcmp(key,"gun2_model") == 0) vs.gun2_model = std::clamp(atoi(val), 0, 2);
        else if (strcmp(key,"gun2_vibration_mode") == 0)
            vs.gun2_vibration_mode = std::clamp(atoi(val), 0, VrState::kGunVibrationModeCount - 1);
        else if (strcmp(key,"dpad_headset_enabled") == 0) readb(vs.dpad_headset_enabled);
        else if (strcmp(key,"air_wheel_enabled") == 0)           readb(vs.air_wheel_enabled);
        else if (strcmp(key,"air_wheel_steer_enabled") == 0)     readb(vs.air_wheel_steer_enabled);
        else if (strcmp(key,"air_wheel_steer_threshold") == 0)
            vs.air_wheel_steer_threshold = std::clamp((float)atof(val), 0.08f, 0.60f);
        else if (strcmp(key,"air_wheel_accel_enabled") == 0)     readb(vs.air_wheel_accel_enabled);
        else if (strcmp(key,"air_wheel_brake_enabled") == 0)     readb(vs.air_wheel_brake_enabled);
        else if (strcmp(key,"air_wheel_push_threshold") == 0)
            vs.air_wheel_push_threshold = std::clamp((float)atof(val), 0.04f, 0.30f);
        else if (strcmp(key,"air_wheel_brake_speed") == 0)
            vs.air_wheel_brake_speed = std::clamp((float)atof(val), 0.20f, 2.00f);
        else if (strcmp(key,"air_wheel_adaptive_neutral") == 0)  readb(vs.air_wheel_adaptive_neutral);
        else if (strcmp(key,"motion_exclusive") == 0)            readb(vs.motion_exclusive);
        else if (strcmp(key,"air_jump_enabled") == 0)            readb(vs.air_jump_enabled);
        else if (strcmp(key,"air_jump_speed") == 0)
            vs.air_jump_speed = std::clamp((float)atof(val), 0.30f, 2.50f);
        else if (strcmp(key,"air_jump_hold_margin") == 0)
            vs.air_jump_hold_margin = std::clamp((float)atof(val), 0.02f, 0.30f);
        else if (strcmp(key,"air_fighter_enabled") == 0)  readb(vs.air_fighter_enabled);
        else if (strcmp(key,"fight_qc_enabled") == 0)     readb(vs.fight_qc_enabled);
        else if (strcmp(key,"fight_qc_speed") == 0)
            vs.fight_qc_speed = std::clamp((float)atof(val), 0.40f, 3.00f);
        else if (strcmp(key,"fight_dp_enabled") == 0)     readb(vs.fight_dp_enabled);
        else if (strcmp(key,"fight_dp_speed") == 0)
            vs.fight_dp_speed = std::clamp((float)atof(val), 0.40f, 3.00f);
        else if (strcmp(key,"fight_charge_across_enabled") == 0) readb(vs.fight_charge_across_enabled);
        else if (strcmp(key,"fight_charge_up_enabled") == 0)     readb(vs.fight_charge_up_enabled);
        else if (strcmp(key,"fight_charge_distance") == 0)
            vs.fight_charge_distance = std::clamp((float)atof(val), 0.15f, 0.60f);
        else if (strcmp(key,"fight_charge_seconds") == 0)
            vs.fight_charge_seconds = std::clamp((float)atof(val), 0.50f, 4.00f);
        else if (strcmp(key,"fight_charge_speed") == 0)
            vs.fight_charge_speed = std::clamp((float)atof(val), 0.40f, 3.00f);
        else if (strcmp(key,"fight_qck_enabled") == 0)    readb(vs.fight_qck_enabled);
        else if (strcmp(key,"fight_qck_speed") == 0)
            vs.fight_qck_speed = std::clamp((float)atof(val), 0.40f, 3.00f);
        else if (strcmp(key,"fight_qck_purity") == 0)
            vs.fight_qck_purity = std::clamp((float)atof(val), 1.20f, 5.00f);
        else if (strcmp(key,"fight_punch_enabled") == 0)  readb(vs.fight_punch_enabled);
        else if (strcmp(key,"fight_kick_enabled") == 0)   readb(vs.fight_kick_enabled);
        else if (strcmp(key,"fight_heavy_enabled") == 0)  readb(vs.fight_heavy_enabled);
        else if (strcmp(key,"fight_punch_speed") == 0)
            vs.fight_punch_speed = std::clamp((float)atof(val), 0.40f, 3.00f);
        else if (strcmp(key,"fight_hold_seconds") == 0)
            vs.fight_hold_seconds = std::clamp((float)atof(val), 0.06f, 0.40f);
        else if (strcmp(key,"fight_kick_ratio") == 0)
            vs.fight_kick_ratio = std::clamp((float)atof(val), 0.20f, 1.50f);
        else if (strncmp(key,"motion_bind_", 12) == 0) {
            const int i = atoi(key + 12);
            if (i >= 0 && i < VrState::kMotionBindCount)
                vs.motion_bind[i] = std::clamp(atoi(val), 0, VrState::kMotionBindFirst - 1);
        }
        else if (strcmp(key,"air_wheel_gear_enabled") == 0)      readb(vs.air_wheel_gear_enabled);
        else if (strcmp(key,"air_wheel_gear_threshold") == 0)
            vs.air_wheel_gear_threshold = std::clamp((float)atof(val), 0.20f, 0.90f);
        else if (strcmp(key,"air_wheel_handbrake_enabled") == 0) readb(vs.air_wheel_handbrake_enabled);
        else if (strcmp(key,"air_wheel_handbrake_threshold") == 0)
            vs.air_wheel_handbrake_threshold = std::clamp((float)atof(val), 0.05f, 0.40f);
        else if (strcmp(key,"air_wheel_bike_enabled") == 0)      readb(vs.air_wheel_bike_enabled);
        else if (strcmp(key,"air_wheel_bike_threshold") == 0)
            vs.air_wheel_bike_threshold = std::clamp((float)atof(val), 0.10f, 0.80f);
        else if (strcmp(key,"dpad_headset_threshold") == 0)
            vs.dpad_headset_threshold = std::clamp((float)atof(val),
                VrState::kDpadHeadsetThresholdMin, VrState::kDpadHeadsetThresholdMax);
        else if (strcmp(key,"bg_preset_index") == 0) vs.bg_preset_index = std::clamp(atoi(val), -1, 15);
        else if (strcmp(key,"real_geometry_boxes") == 0) readb(vs.real_geometry_boxes);
        else if (strcmp(key,"silhouette_sides") == 0) readb(vs.silhouette_sides);
        else if (strcmp(key,"rom_preview_enabled") == 0) readb(vs.rom_preview_enabled);
        else if (strcmp(key,"bgm_enabled") == 0) readb(vs.bgm_enabled);
        else if (strcmp(key,"bgm_volume") == 0) vs.bgm_volume = std::clamp((float)atof(val), 0.0f, 1.0f);
        else if (strcmp(key,"audio_channel_split_enabled") == 0) readb(vs.audio_channel_split_enabled);
        else if (strncmp(key,"snes_voice_volume_", 19) == 0) {
            const int i = atoi(key + 19);
            if (i >= 0 && i < 8) vs.snes_voice_volume[i] = std::clamp((float)atof(val), 0.0f, 1.0f);
        }
        else if (strncmp(key,"genesis_channel_volume_", 24) == 0) {
            const int i = atoi(key + 24);
            if (i >= 0 && i < 2) vs.genesis_channel_volume[i] = std::clamp((float)atof(val), 0.0f, 1.0f);
        }
        else if (strncmp(key,"nes_channel_volume_", 20) == 0) {
            const int i = atoi(key + 20);
            if (i >= 0 && i < 5) vs.nes_channel_volume[i] = std::clamp((float)atof(val), 0.0f, 1.0f);
        }
        else if (strncmp(key,"gba_channel_volume_", 20) == 0) {
            const int i = atoi(key + 20);
            if (i >= 0 && i < 3) vs.gba_channel_volume[i] = std::clamp((float)atof(val), 0.0f, 1.0f);
        }
        else if (strncmp(key,"pce_channel_volume_", 20) == 0) {
            const int i = atoi(key + 20);
            if (i >= 0 && i < 6) vs.pce_channel_volume[i] = std::clamp((float)atof(val), 0.0f, 1.0f);
        }
        else if (strcmp(key,"rotate_screen") == 0) vs.rotate_screen = std::clamp(atoi(val), 0, 3);
        else if (strcmp(key,"surface_mode") == 0) vs.surface_mode = std::clamp(atoi(val), 0, 2);
        // Legacy key from when this was a bool: "on" meant Table.
        else if (strcmp(key,"table_mode") == 0) {
            bool legacy = false; readb(legacy);
            if (legacy) vs.surface_mode = 1;
        }
        else if (strcmp(key,"show_controller_models") == 0) readb(vs.show_controller_models);
        else if (strcmp(key,"environment_sphere_mode") == 0) {
            vs.environment_sphere_mode = (EnvironmentSphereMode)std::clamp(atoi(val), 0, 3);
        }
        else if (strcmp(key,"sky_dome")        == 0) {
            bool legacy = false;
            readb(legacy);
            vs.environment_sphere_mode = legacy ? EnvironmentSphereMode::SkyOnly : EnvironmentSphereMode::Off;
        }
        else if (strcmp(key,"perspective_comp") == 0) readb(vs.perspective_comp);
        else if (strcmp(key,"parallax_ratio")   == 0) readf(vs.parallax_ratio);
        else if (strcmp(key,"auto_frame_skip_snes") == 0) readb(vs.auto_frame_skip_snes);
        else if (strcmp(key,"auto_frame_skip_genesis") == 0) readb(vs.auto_frame_skip_genesis);
        else if (strcmp(key,"auto_frame_skip_mame") == 0) readb(vs.auto_frame_skip_mame);
        else if (strcmp(key,"auto_frame_skip_saturn") == 0) readb(vs.auto_frame_skip_saturn);
        else if (strcmp(key,"auto_frame_skip_pce") == 0) readb(vs.auto_frame_skip_pce);
        else if (strcmp(key,"auto_frame_skip_gba") == 0) readb(vs.auto_frame_skip_gba);
        else if (strcmp(key,"emu_resolution_scale") == 0) readi(vs.emu_resolution_scale);
        else if (strcmp(key,"psx_render_path") == 0) readi(vs.psx_render_path);
        else if (strcmp(key,"psx_gpu_resolution") == 0) {
            const int requested = atoi(val);
            vs.psx_gpu_resolution = requested == 1 || requested == 2 || requested == 4
                ? requested : 4;
        }
        else if (strcmp(key,"psx_texture_filter") == 0) {
            const int requested = atoi(val);
            vs.psx_texture_filter =
                (requested >= 0 && requested < VrState::kPsxTextureFilterCount) ? requested : 0;
        }
        else if (strcmp(key,"vr_resolution_scale")  == 0) readf(vs.vr_resolution_scale);
        else if (strcmp(key,"sprite_y_depth")        == 0) readb(vs.sprite_y_depth);
        else if (strcmp(key,"sprite_y_depth_spread") == 0) readf(vs.sprite_y_depth_spread);
        else if (strcmp(key,"audio_spatial_mode")    == 0) vs.audio_spatial_mode = std::clamp(atoi(val), 0, 3);
        else if (strcmp(key,"audio_screen_lock")     == 0) readb(vs.audio_screen_lock);
        else if (strcmp(key,"layer_filter_mode") == 0) {
            if (layer_filter_mode) *layer_filter_mode = atoi(val);
        }
        else if (strcmp(key,"layer_auto_dup_percent") == 0) {
            if (layer_auto_dup_percent) *layer_auto_dup_percent = atoi(val);
        }
        else if (strcmp(key,"menu_position_mode")     == 0) vs.menu_position_mode     = std::clamp(atoi(val), 0, 4);
        else if (strcmp(key,"menu_transparency_mode") == 0) vs.menu_transparency_mode = std::clamp(atoi(val), 0, 3);
        else if (strcmp(key,"menu_favorites_csv")     == 0) {
            vs.menu_favorites_csv = val;
            // fgets() leaves the trailing newline (and possibly \r) in val —
            // every other field here is numeric and atoi/atof silently stop at
            // it, but a raw string copy needs an explicit trim.
            while (!vs.menu_favorites_csv.empty() &&
                   (vs.menu_favorites_csv.back() == '\n' || vs.menu_favorites_csv.back() == '\r')) {
                vs.menu_favorites_csv.pop_back();
            }
        }
        else if (strcmp(key,"canvas_depth_meters_ui") == 0) readf(vs.canvas_depth_meters_ui);
        else if (strcmp(key,"thickness_overlap_ui")   == 0) readb(vs.thickness_overlap_ui);
        else if (strcmp(key,"layer_slot_fractions_csv") == 0) {
            vs.layer_slot_fractions_csv = val;
            while (!vs.layer_slot_fractions_csv.empty() &&
                   (vs.layer_slot_fractions_csv.back() == '\n' || vs.layer_slot_fractions_csv.back() == '\r')) {
                vs.layer_slot_fractions_csv.pop_back();
            }
        }
        else {
            // Per-layer keys: parse index from suffix
            int idx = -1, cidx = -1;
            if (sscanf(key, "layer_depth_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].depth_meters = (float)atof(val);
            else if (sscanf(key, "layer_width_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].quad_width_meters = (float)atof(val);
            else if (sscanf(key, "layer_geom_mode_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].geometry_mode = (LayerGeometryMode)std::clamp(atoi(val), 0, 11);
            else if (sscanf(key, "layer_thickness_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].box_thickness_meters = (float)atof(val);
            else if (sscanf(key, "layer_split_px_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].split_pixels = std::clamp(atoi(val), 0, 512);
            else if (sscanf(key, "layer_repeat_count_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].repeat_count = std::clamp(atoi(val), 0, 8);
            else if (sscanf(key, "layer_scatter_range_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].scatter_range = std::clamp((float)atof(val), 0.0f, 5.0f);
            else if (sscanf(key, "layer_y_depth_range_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].y_depth_range = std::clamp((float)atof(val), 0.0f, 5.0f);
            else if (sscanf(key, "layer_copies_count_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size()) {
                int cnt = atoi(val);
                if (cnt >= 0 && cnt <= 100) cfg.layers[idx].copies.resize(cnt, 0.0f);
            }
            else if (sscanf(key, "layer_copy_%d_%d", &idx, &cidx) == 2
                     && idx >= 0 && idx < (int)cfg.layers.size()
                     && cidx >= 0 && cidx < (int)cfg.layers[idx].copies.size())
                cfg.layers[idx].copies[cidx] = (float)atof(val);
            else if (sscanf(key, "layer_order_%d", &idx) == 1 && idx >= 0 && idx < (int)layer_order.size())
                layer_order[idx] = atoi(val);
            else if (sscanf(key, "layer_enabled_%d", &idx) == 1 && idx >= 0 && idx < (int)layer_enabled.size())
                layer_enabled[idx] = atoi(val) != 0;
            else if (sscanf(key, "layer_ambilight_%d", &idx) == 1 && idx >= 0 && idx < (int)layer_ambilight.size())
                layer_ambilight[idx] = atoi(val) != 0;
            else if (sscanf(key, "layer_side_color_%d", &idx) == 1 && idx >= 0 && idx < (int)layer_side_color.size())
                layer_side_color[idx] = std::clamp(atoi(val), 0, 6); // 6 = Darker
            else if (btn_map) {
                int bi = -1;
                if (btn_map_backend == qrd::BackendKind::Genesis &&
                    sscanf(key, "btn_map_genesis_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT) {
                    (*btn_map)[bi] = atoi(val);
                } else if (btn_map_backend == qrd::BackendKind::Gba &&
                    sscanf(key, "btn_map_gba_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT) {
                    (*btn_map)[bi] = atoi(val);
                } else if (btn_map_backend == qrd::BackendKind::Gb &&
                    sscanf(key, "btn_map_gb_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT) {
                    (*btn_map)[bi] = atoi(val);
                } else if (btn_map_backend == qrd::BackendKind::Snes &&
                    sscanf(key, "btn_map_snes_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT) {
                    (*btn_map)[bi] = atoi(val);
                } else if (btn_map_backend == qrd::BackendKind::Snes &&
                    sscanf(key, "btn_map_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT) {
                    (*btn_map)[bi] = atoi(val);
                }
            }
        }
    }

    fclose(f);

    // Clear any all-zero copies vectors — saved with a zero copy_step (e.g. from a
    // discarded experiment) and would collapse all depth instances to the same plane.
    // Empty copies triggers the k_max_copies fallback in the renderer.
    for (auto& layer : cfg.layers) {
        if (!layer.copies.empty() && layer.copies.back() <= 0.0f)
            layer.copies.clear();
    }

    return true;
}
