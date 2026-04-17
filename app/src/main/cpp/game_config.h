#pragma once
#include <string>
#include <vector>
#include <array>
#include <cstdint>

enum class ExtractionType {
    FullFrame,
    Region,
    ColorKey,
    ColorKeyInverted,
    // Use the snes9x per-pixel z-buffer to isolate a priority band.
    // Pixels whose z-value is in [z_min, z_max] are opaque; others transparent.
    ZBuffer,
    // Use the backend-provided per-layer capture buffer (filled during the render pass).
    // layer_index selects which backend layer to use.
    // Each pixel is opaque where a non-transparent tile was drawn, alpha=0 elsewhere.
    // Unlike ZBuffer slicing this does NOT create holes — each layer captures all
    // of its own tiles regardless of whether a closer layer also covers that pixel.
    PerLayerCapture,
    // Use final composited frame colour, masked by the main-screen visible source id.
    // layer_index selects which source won the final pixel: 0-3=BG0-BG3, 4=OBJ, 5=backdrop.
    VisibleSourceFinal,
    // Start with a raw layer capture, then replace the pixels that actually win the
    // final composite with the final frame colour for that source id.
    VisibleSourceHybrid,
};

struct LayerConfig {
    std::string id;
    float depth_meters      = 1.5f;
    float quad_width_meters = 2.56f;
    // Extra depth-copy offsets (metres toward viewer). Empty = use renderer default.
    std::vector<float> copies;

    ExtractionType extraction_type = ExtractionType::FullFrame;

    // Region extraction: [x, y, width, height] in source pixels
    std::array<int, 4> rect = {0, 0, 0, 0};

    // ColorKey / ColorKeyInverted: color in RGB order, tolerance per channel
    std::array<uint8_t, 3> color     = {0, 0, 0};
    int                    tolerance = 8;

    // Initial runtime flags for new layer state when no saved preference exists.
    bool default_enabled    = true;
    bool default_ambilight  = true;

    // ZBuffer: inclusive z-value range [z_min, z_max] (snes9x scale 0–63).
    // D=32 for main screen; sprites ≈36, BG low ≈35–43, BG high ≈43–47, backdrop ≈1.
    uint8_t z_min = 0;
    uint8_t z_max = 255;

    // PerLayerCapture / VisibleSourceFinal source selection.
    // SNES VisibleSourceFinal: 0-3 = BG0-BG3, 4 = OBJ, 5 = backdrop.
    // Genesis PerLayerCapture: 0 = background, 1/2 = plane B low/high,
    // 3/4 = plane A+window low/high, 5/6 = sprites low/high.
    int layer_index = 0;

    // Dynamic z-split: if true, z_min/z_max are auto-updated from histogram analysis.
    // This allows layers to adapt to different scenes automatically.
    bool dynamic_z_split = false;
};

struct GameConfig {
    std::string game;
    int virtual_width  = 256;
    int virtual_height = 224;
    float quad_y_meters = 1.6f;   // vertical centre in stage space (floor = 0)
    std::vector<LayerConfig> layers;
    bool dynamic_layers = false;   // enable dynamic z-split (auto-adjust from histogram)

    // Single full-frame layer — baseline when no game config is found.
    static GameConfig make_flat();

    // Per-system defaults matching the retrodepth PC defaults.
    static GameConfig make_default_snes();
    static GameConfig make_default_genesis();
    static GameConfig make_default_nes();
    static GameConfig make_default_sms();
    static GameConfig make_default_gba();
    static GameConfig make_default_gb();
    static GameConfig make_default_pce();

    // Update dynamic z-splits based on z-buffer histogram analysis.
    // Finds natural z-value clusters and adjusts z_min/z_max for dynamic layers.
    // Returns number of active layers (0 if no change).
    int update_z_splits(const uint32_t histogram[256]);
};

// Redistribute depths uniformly [far..near] while preserving ordering.
void even_spread_layer_depths(std::vector<LayerConfig>& layers);
