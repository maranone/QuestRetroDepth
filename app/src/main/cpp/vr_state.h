#pragma once
#include "game_config.h"
#include <algorithm>
#include <random>
#include <vector>
#include <cmath>

// -----------------------------------------------------------------------
// Complete VR visual state — every knob that can be randomised or tweaked
// -----------------------------------------------------------------------
struct VrState {
    // Color grading
    float gamma      = 1.15f;
    float contrast   = 0.90f;
    float saturation = 0.80f;
    float brightness = 1.00f;

    // Geometry effects
    float roundness    = 0.0f;   // always 0 — disabled
    float screen_curve = 0.0f;   // always 0 — disabled

    // Subtle screen tilt (radians)
    float tilt_x = 0.0f;
    float tilt_y = 0.0f;

    // Feature flags
    bool immersive_beta_enabled = false; // master flag for beta immersive presentation work
    bool layers_3d         = false; // depth-write on copies (volumetric)
    bool solid_stack       = false; // disabled — always off
    bool depthmap          = false; // silhouette wedge scaling across the copy stack
    bool depthmap_mirror   = false;
    bool upscale           = false; // sharpened bilinear
    bool shadows           = false; // repurposed as Meta Quest passthrough
    bool ambilight         = true;

    // Performance settings
    bool  auto_frame_skip      = false; // let emulator decide to skip frames
    int   emu_resolution_scale = 1;     // emulator internal render scale (1-4)
    float vr_resolution_scale  = 1.0f;  // OpenXR eye swapchain scale vs recommended (0.25-4.0)

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
        depthmap        = randb(0.18f);
        depthmap_mirror = false;
        upscale         = randb(0.35f);
        shadows         = false;
        ambilight       = randb(0.62f);

        roundness    = 0.0f;
        screen_curve = 0.0f;
        tilt_x       = randf(-0.075f, 0.075f);
        tilt_y       = randf(-0.11f,  0.11f);

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
    presets[0].gamma = 1.15f; presets[0].contrast = 0.90f; presets[0].saturation = 0.80f;

    // Preset 1: Compressed depth, wider layers
    presets[1].gamma = 1.10f; presets[1].contrast = 0.88f; presets[1].saturation = 0.75f;

    // Preset 2: Exaggerated depth
    presets[2].gamma = 1.20f; presets[2].contrast = 0.95f; presets[2].saturation = 0.85f;

    // Preset 3: 3-D layers + solid extrusion
    presets[3].gamma = 1.18f; presets[3].contrast = 1.00f; presets[3].saturation = 0.82f;
    presets[3].layers_3d = true;

    // Preset 4: Crisp upscale + ambilight
    presets[4].gamma = 1.05f; presets[4].contrast = 1.10f; presets[4].saturation = 1.00f;
    presets[4].upscale = true; presets[4].ambilight = true;

    return presets;
}
