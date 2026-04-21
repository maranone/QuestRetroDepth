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
#include <cstdio>
#include <cstring>

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

// Save all VrState fields + GameConfig layer depths/widths/copies
// + per-layer enabled/ambilight arrays + button map + optional refresh_rate (0 = headset default).
static inline bool settings_save(
    const std::string& path,
    const VrState& vs,
    const GameConfig& cfg,
    const std::vector<int>& layer_order,
    const std::vector<bool>& layer_enabled,
    const std::vector<bool>& layer_ambilight,
    int layer_filter_mode = -1,
    int layer_auto_dup_percent = -1,
    float refresh_rate = 0.0f,
    bool experimental_rumble_enabled = true,
    const qrd::ButtonMap* btn_map = nullptr,
    qrd::BackendKind btn_map_backend = qrd::BackendKind::Snes)
{
    qrd::ButtonMap snes_map = qrd::default_button_map_for_backend(qrd::BackendKind::Snes);
    qrd::ButtonMap genesis_map = qrd::default_button_map_for_backend(qrd::BackendKind::Genesis);
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
            else if (sscanf(key, "btn_map_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT)
                snes_map[bi] = value;
        }
        fclose(existing);
    }
    if (btn_map) {
        if (btn_map_backend == qrd::BackendKind::Genesis)
            genesis_map = *btn_map;
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
    settings_write(f, "upscale",         vs.upscale);
    settings_write(f, "passthrough",     vs.shadows);
    settings_write(f, "ambilight",       vs.ambilight);
    settings_write(f, "environment_sphere_mode", (int)vs.environment_sphere_mode);
    settings_write(f, "perspective_comp",    vs.perspective_comp);
    settings_write(f, "parallax_ratio",      vs.parallax_ratio);
    settings_write(f, "auto_frame_skip",    vs.auto_frame_skip);
    settings_write(f, "emu_resolution_scale", vs.emu_resolution_scale);
    settings_write(f, "vr_resolution_scale",  vs.vr_resolution_scale);
    settings_write(f, "layer_filter_mode", layer_filter_mode);
    settings_write(f, "layer_auto_dup_percent", layer_auto_dup_percent);

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
        snprintf(key, sizeof(key), "layer_order_%d", i);
        settings_write(f, key, (i < (int)layer_order.size()) ? layer_order[i] : i);
        snprintf(key, sizeof(key), "layer_enabled_%d", i);
        settings_write(f, key, (i < (int)layer_enabled.size()) ? (bool)layer_enabled[i] : true);
        snprintf(key, sizeof(key), "layer_ambilight_%d", i);
        settings_write(f, key, (i < (int)layer_ambilight.size()) ? (bool)layer_ambilight[i] : true);
    }

    // Button mapping
    if (btn_map) {
        for (int i = 0; i < qrd::SNES_BUTTON_COUNT; ++i) {
            char key[64];
            snprintf(key, sizeof(key), "btn_map_snes_%d", i);
            settings_write(f, key, snes_map[i]);
            snprintf(key, sizeof(key), "btn_map_genesis_%d", i);
            settings_write(f, key, genesis_map[i]);
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
    int* layer_filter_mode = nullptr,
    int* layer_auto_dup_percent = nullptr,
    float* refresh_rate = nullptr,
    bool* experimental_rumble_enabled = nullptr,
    qrd::ButtonMap* btn_map = nullptr,
    qrd::BackendKind btn_map_backend = qrd::BackendKind::Snes)
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
        else if (strcmp(key,"depth_mode")      == 0) vs.depth_mode = (DepthMode)std::clamp(atoi(val), 0, 2);
        else if (strcmp(key,"depthmap")        == 0) vs.depth_mode = atoi(val) != 0 ? DepthMode::WholeLayer : DepthMode::Off;
        else if (strcmp(key,"upscale")         == 0) readb(vs.upscale);
        else if (strcmp(key,"passthrough")     == 0) readb(vs.shadows);
        else if (strcmp(key,"ambilight")       == 0) readb(vs.ambilight);
        else if (strcmp(key,"environment_sphere_mode") == 0) {
            vs.environment_sphere_mode = (EnvironmentSphereMode)std::clamp(atoi(val), 0, 2);
        }
        else if (strcmp(key,"sky_dome")        == 0) {
            bool legacy = false;
            readb(legacy);
            vs.environment_sphere_mode = legacy ? EnvironmentSphereMode::SkyOnly : EnvironmentSphereMode::Off;
        }
        else if (strcmp(key,"perspective_comp") == 0) readb(vs.perspective_comp);
        else if (strcmp(key,"parallax_ratio")   == 0) readf(vs.parallax_ratio);
        else if (strcmp(key,"auto_frame_skip") == 0) readb(vs.auto_frame_skip);
        else if (strcmp(key,"emu_resolution_scale") == 0) readi(vs.emu_resolution_scale);
        else if (strcmp(key,"vr_resolution_scale")  == 0) readf(vs.vr_resolution_scale);
        else if (strcmp(key,"layer_filter_mode") == 0) {
            if (layer_filter_mode) *layer_filter_mode = atoi(val);
        }
        else if (strcmp(key,"layer_auto_dup_percent") == 0) {
            if (layer_auto_dup_percent) *layer_auto_dup_percent = atoi(val);
        }
        else {
            // Per-layer keys: parse index from suffix
            int idx = -1, cidx = -1;
            if (sscanf(key, "layer_depth_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].depth_meters = (float)atof(val);
            else if (sscanf(key, "layer_width_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size())
                cfg.layers[idx].quad_width_meters = (float)atof(val);
            else if (sscanf(key, "layer_copies_count_%d", &idx) == 1 && idx >= 0 && idx < (int)cfg.layers.size()) {
                int cnt = atoi(val);
                if (cnt >= 0 && cnt <= 32) cfg.layers[idx].copies.resize(cnt, 0.0f);
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
            else if (btn_map) {
                int bi = -1;
                if (btn_map_backend == qrd::BackendKind::Genesis &&
                    sscanf(key, "btn_map_genesis_%d", &bi) == 1 && bi >= 0 && bi < qrd::SNES_BUTTON_COUNT) {
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
    return true;
}
