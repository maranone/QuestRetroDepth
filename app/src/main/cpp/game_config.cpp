#include "game_config.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr float k_genesis_default_near_depth = 2.920264f;
constexpr float k_genesis_default_far_depth = 5.343064f;
constexpr float k_genesis_default_quad_width = 2.56f;
constexpr int k_genesis_default_copy_count = 28;
constexpr float k_genesis_default_copy_step = 0.003f;

static void apply_uniform_width_and_copies(LayerConfig& lc, float width, int copy_count, float copy_step) {
    lc.quad_width_meters = width;
    lc.copies.resize(copy_count);
    for (int i = 0; i < copy_count; ++i) {
        lc.copies[i] = (float)(i + 1) * copy_step;
    }
}

static void apply_even_default_depth_envelope(std::vector<LayerConfig>& layers,
                                              float near_depth,
                                              float far_depth,
                                              float width,
                                              int copy_count,
                                              float copy_step) {
    const int n = (int)layers.size();
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
        const float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        layers[i].depth_meters = far_depth + (near_depth - far_depth) * t;
        apply_uniform_width_and_copies(layers[i], width, copy_count, copy_step);
    }
}

} // namespace

// Find z-values that have non-empty pixels.
static std::vector<int> find_occupied_z(const uint32_t histogram[256]) {
    std::vector<int> occupied;
    for (int z = 0; z < 64; ++z) {
        if (histogram[z] > 0) {
            occupied.push_back(z);
        }
    }
    return occupied;
}

int GameConfig::update_z_splits(const uint32_t histogram[256]) {
    if (!dynamic_layers) {
        for (auto& lc : layers) {
            if (!lc.dynamic_z_split) continue;
            lc.z_min = 0;
            lc.z_max = 63;
        }
        return 0;
    }

    auto occupied = find_occupied_z(histogram);
    if (occupied.empty()) return 0;

    size_t n = occupied.size();

    if (layers.size() < n) {
        for (size_t i = layers.size(); i < n; ++i) {
            LayerConfig lc;
            lc.id = "layer_" + std::to_string(i);
            lc.extraction_type = ExtractionType::ZBuffer;
            lc.dynamic_z_split = true;
            layers.push_back(lc);
        }
    } else if (layers.size() > n) {
        layers.resize(n);
    }

    for (size_t i = 0; i < layers.size(); ++i) {
        if (i < occupied.size()) {
            layers[i].z_min = static_cast<uint8_t>(occupied[i]);
            layers[i].z_max = static_cast<uint8_t>(occupied[i]);
            layers[i].depth_meters = 2.0f - (occupied[i] / 63.0f) * 1.2f;
        }
    }

    
    return static_cast<int>(layers.size());
}

void even_spread_layer_depths(std::vector<LayerConfig>& layers) {
    const int n = (int)layers.size();
    if (n < 2) return;
    float mn = layers[0].depth_meters, mx = layers[0].depth_meters;
    for (const auto& lc : layers) {
        mn = std::min(mn, lc.depth_meters);
        mx = std::max(mx, lc.depth_meters);
    }
    if (mx - mn < 0.1f) mx = mn + 0.5f * (n - 1);
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return layers[a].depth_meters > layers[b].depth_meters;
    });
    for (int i = 0; i < n; ++i) {
        float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        float d = mx + t * (mn - mx);
        layers[idx[i]].depth_meters = d < 0.05f ? 0.05f : d;
    }
}

GameConfig GameConfig::make_flat() {
    GameConfig cfg;
    cfg.game           = "flat";
    cfg.virtual_width  = 256;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;
    LayerConfig lc;
    lc.id               = "frame";
    lc.depth_meters     = 1.5f;
    lc.quad_width_meters = 2.56f;
    lc.extraction_type  = ExtractionType::FullFrame;
    cfg.layers.push_back(std::move(lc));
    return cfg;
}

GameConfig GameConfig::make_default_snes() {
    GameConfig cfg;
    cfg.game           = "snes";
    cfg.virtual_width  = 256;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;
    cfg.dynamic_layers = false;

    // Fixed semantic z-bands covering all snes9x priority values across all BG modes.
    // D=32 (main screen). Sprites: DrawOBJS(D+4), Z2 = 36 + priority*4.
    // BG z = D + Zh/Zl from DO_BG macro:
    //   Mode 0: BG3 lo=34 hi=38, BG2 lo=35 hi=39, BG1 lo=42 hi=46, BG0 lo=43 hi=47
    //   Mode 1: BG2 lo=35 hi=39 (or 49 w/ BG3Priority), BG1 lo=42 hi=46, BG0 lo=43 hi=47
    //   Mode 2-6: BG1 lo=35 hi=39, BG0 lo=39/43 hi=47 (varies)
    // Bands use ranges so no z-value falls through a gap.
    // Layers with no pixels this frame are skipped by the renderer automatically.
    static const struct { const char* id; float depth; uint8_t z_min; uint8_t z_max; } k[] = {
        { "backdrop",   2.00f,  1,  1  },  // solid colour fill (z=1)
        { "bg_far_lo",  1.85f, 34, 35  },  // BG3 lo (Mode 0, z=34) + BG2 lo (Mode 1, z=35)
        { "sprite_p0",  1.75f, 36, 37  },  // sprite priority 0 (z=36)
        { "bg_far_hi",  1.65f, 38, 39  },  // BG3 hi (Mode 0, z=38) + BG2 hi (Mode 1, z=39)
        { "sprite_p1",  1.55f, 40, 41  },  // sprite priority 1 (z=40)
        { "bg1_lo",     1.45f, 42, 42  },  // BG1 low-priority (z=42)
        { "bg0_lo",     1.35f, 43, 43  },  // BG0 low-priority (z=43)
        { "sprite_p2",  1.25f, 44, 45  },  // sprite priority 2 (z=44)
        { "bg1_hi",     1.15f, 46, 46  },  // BG1 high-priority (z=46)
        { "bg0_hi",     1.05f, 47, 47  },  // BG0 high-priority (z=47)
        { "sprite_p3",  0.90f, 48, 63  },  // sprite priority 3 (z=48) + BG3Priority (z=49) + any overflow
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id                = s.id;
        lc.depth_meters      = s.depth;
        lc.quad_width_meters = 2.56f;
        lc.extraction_type   = ExtractionType::ZBuffer;
        lc.z_min             = s.z_min;
        lc.z_max             = s.z_max;
        cfg.layers.push_back(std::move(lc));
    }

    static const struct { const char* id; float depth; int layer_index; } k_capture[] = {
        { "pc_bg4", 2.00f, 3 }, // internal BG3 = SNES BG4
        { "pc_bg3", 1.84f, 2 }, // internal BG2 = SNES BG3
        { "pc_bg2", 1.61f, 1 }, // internal BG1 = SNES BG2
        { "pc_bg1", 1.29f, 0 }, // internal BG0 = SNES BG1
        { "pc_obj", 0.98f, 4 }, // sprites
    };
    for (const auto& s : k_capture) {
        LayerConfig lc;
        lc.id                = s.id;
        lc.depth_meters      = s.depth;
        lc.quad_width_meters = 2.56f;
        lc.extraction_type   = ExtractionType::PerLayerCapture;
        lc.layer_index       = s.layer_index;
        lc.default_enabled   = false;
        lc.default_ambilight = false;
        cfg.layers.push_back(std::move(lc));
    }

    apply_even_default_depth_envelope(cfg.layers,
                                      k_genesis_default_near_depth,
                                      k_genesis_default_far_depth,
                                      k_genesis_default_quad_width,
                                      k_genesis_default_copy_count,
                                      k_genesis_default_copy_step);

    return cfg;
}

GameConfig GameConfig::make_default_genesis() {
    GameConfig cfg;
    cfg.game           = "genesis";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; float width; int layer_index; } k[] = {
        { "background",   5.343064f, 2.56f, 0 },
        { "plane_b_low",  2.920264f, 2.56f, 1 },
        { "plane_b_high", 3.324065f, 2.56f, 2 },
        { "plane_a_low",  3.727862f, 2.56f, 3 },
        { "plane_a_high", 4.131662f, 2.56f, 4 },
        { "sprites_low",  4.535461f, 2.56f, 5 },
        { "sprites_high", 4.939260f, 2.56f, 6 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.quad_width_meters = s.width;
        lc.extraction_type  = ExtractionType::VisibleSourceHybrid;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_nes() {
    // NES: 3 hardware planes (backdrop, BG tiles, sprites).
    // Per-pixel source IDs captured by fceux_layer_capture from FCEUmm PPU:
    //   0 = backdrop, 1 = background, 2 = sprites.
    GameConfig cfg;
    cfg.game           = "nes";
    cfg.virtual_width  = 256;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "backdrop",    k_genesis_default_far_depth,  0 },
        { "background",  3.5f,                          1 },
        { "sprites",     k_genesis_default_near_depth,  2 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::VisibleSourceFinal;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_sms() {
    // SMS: 3 hardware planes (backdrop, BG tile layer, sprites).
    // Per-pixel source IDs captured by picodrive_sms_layer_capture from Mode 4 renderer:
    //   0 = backdrop, 1 = bg_plane, 2 = sprites.
    // GG (160x144) dimension mismatch means it falls back to FullFrame — deferred.
    GameConfig cfg;
    cfg.game           = "sms";
    cfg.virtual_width  = 256;
    cfg.virtual_height = 192;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "backdrop",  k_genesis_default_far_depth,  0 },
        { "bg_plane",  3.5f,                          1 },
        { "sprites",   k_genesis_default_near_depth,  2 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.quad_width_meters = 2.56f;
        lc.extraction_type  = ExtractionType::VisibleSourceFinal;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_gba() {
    // GBA: 240x160, 5 hardware layers — BG0-3 and OBJ.
    // mGBA renders via VisibleSourceFinal (visible_source_id: 0=BG0,1=BG1,2=BG2,3=BG3,4=OBJ).
    // BG0 is typically the HUD/text (front), BG3 the sky/background (back).
    GameConfig cfg;
    cfg.game           = "gba";
    cfg.virtual_width  = 240;
    cfg.virtual_height = 160;
    cfg.quad_y_meters  = 1.6f;

    // far → near: BG3(sky) … BG0(HUD) … OBJ(sprites)
    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "bg3",  k_genesis_default_far_depth,                                          3 },
        { "bg2",  k_genesis_default_far_depth * 0.75f + k_genesis_default_near_depth * 0.25f, 2 },
        { "bg1",  k_genesis_default_far_depth * 0.50f + k_genesis_default_near_depth * 0.50f, 1 },
        { "bg0",  k_genesis_default_far_depth * 0.25f + k_genesis_default_near_depth * 0.75f, 0 },
        { "obj",  k_genesis_default_near_depth,                                          4 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::VisibleSourceFinal;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_gb() {
    // GB/GBC: 160x144, 3 planes (BG, Window overlay, OBJ sprites).
    // mGBA visible_source_id: 0=BG, 1=window (maps to BG1), 4=OBJ.
    // Window (BG1) is the HUD overlay — sits in front of BG, behind sprites.
    GameConfig cfg;
    cfg.game           = "gb";
    cfg.virtual_width  = 160;
    cfg.virtual_height = 144;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "bg",     k_genesis_default_far_depth,  0 },  // BG tilemap
        { "window", 3.5f,                          1 },  // Window overlay (HUD)
        { "obj",    k_genesis_default_near_depth,  4 },  // Sprites
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::VisibleSourceFinal;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_pce() {
    // PC Engine: 512x243 internal VDC surface, 3 planes (backdrop, bg_plane, sprites).
    // beetle-pce visible_source_id: 0=backdrop, 1=bg_plane, 2=sprites.
    GameConfig cfg;
    cfg.game           = "pce";
    cfg.virtual_width  = 256;  // typical PCE game width; VDC surface is 512 wide
    cfg.virtual_height = 243;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "backdrop",  k_genesis_default_far_depth,  0 },
        { "bg_plane",  3.5f,                          1 },
        { "sprites",   k_genesis_default_near_depth,  2 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::VisibleSourceFinal;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}
