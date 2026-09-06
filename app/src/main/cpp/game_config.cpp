#include "game_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr float k_genesis_default_near_depth = 2.920264f;
constexpr float k_genesis_default_far_depth = 5.343064f;
constexpr float k_genesis_default_quad_width = 2.56f;
constexpr int k_genesis_default_copy_count = 28;
constexpr float k_genesis_default_copy_step = 0.003f;
constexpr float k_snes_default_near_depth = 1.18f;
constexpr float k_snes_default_far_depth = 2.25f;
constexpr float k_snes_default_quad_width = 2.56f;
constexpr int k_snes_default_copy_count = 16;
constexpr float k_snes_default_copy_step = 0.002f;
constexpr float k_nes_default_quad_width = 2.15f;
constexpr int k_nes_default_copy_count = 4;
constexpr float k_nes_default_copy_step = 0.001f;

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
    for (int z = 0; z < 256; ++z) {
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

    if (rank_spread_layers) {
        // Slot layers, in array order (far to near) -- these get their
        // z_min/z_max rewritten every window based on rank, not matched by
        // constant.
        std::vector<int> slot_indices;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (layers[i].z_rank_spread_slot) slot_indices.push_back((int)i);
        }

        // A value already claimed by a fixed (non-spread) single-value layer
        // -- e.g. backdrop/fix -- has its own dedicated layer and doesn't
        // compete for a slot.
        std::vector<int> pool;
        for (int z : occupied) {
            bool claimed = false;
            for (const auto& lc : layers) {
                if (!lc.z_rank_spread_slot && lc.z_min == (uint8_t)z && lc.z_max == (uint8_t)z) {
                    claimed = true;
                    break;
                }
            }
            if (!claimed) pool.push_back(z);
        }

        const int slot_count = (int)slot_indices.size();
        const int n = (int)pool.size();
        std::vector<uint8_t> slot_min(slot_count, 255), slot_max(slot_count, 0);
        std::vector<bool> slot_has(slot_count, false);

        if (slot_count > 0 && n > 0) {
            for (int r = 0; r < n; ++r) {
                int slot = (r * slot_count) / n;
                if (slot >= slot_count) slot = slot_count - 1;
                const uint8_t z = (uint8_t)pool[r];
                if (!slot_has[slot] || z < slot_min[slot]) slot_min[slot] = z;
                if (!slot_has[slot] || z > slot_max[slot]) slot_max[slot] = z;
                slot_has[slot] = true;
            }
        }

        for (int s = 0; s < slot_count; ++s) {
            LayerConfig& lc = layers[slot_indices[s]];
            if (slot_has[s]) {
                lc.z_min = slot_min[s];
                lc.z_max = slot_max[s];
            } else {
                // No occupied value ranked into this slot this window --
                // match nothing rather than keep showing a stale range from
                // a previous window's different bucket (z_min > z_max is
                // treated as an empty/unmatchable range by the extractor).
                lc.z_min = 255;
                lc.z_max = 0;
            }
        }
        return static_cast<int>(layers.size());
    }

    if (occupied.empty()) return static_cast<int>(layers.size());

    // A z-band layer's depth is purely a function of its own z-value (see
    // below), so it never needs to move once created -- the only two things
    // that should ever happen are a genuinely new z-value appearing (append)
    // or a z-value that's been gone for a while going away (erase). Rewriting
    // every entry by array position every call (the previous approach) tore
    // down and rebuilt the whole layer set on every single window, which is
    // what showed up on-device as the layer set (and anything rendered from
    // it) flickering even when the underlying content was perfectly stable.
    z_absent_windows.resize(layers.size(), 0);

    std::vector<bool> matched(layers.size(), false);
    for (int z : occupied) {
        bool found = false;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (layers[i].dynamic_z_split && layers[i].z_min == (uint8_t)z) {
                matched[i] = true;
                z_absent_windows[i] = 0;
                found = true;
                break;
            }
        }
        if (!found) {
            LayerConfig lc;
            // Neo Geo's backdrop (z=2) and fix/HUD (z=52) used to be
            // separately hand-seeded, hand-labeled layers with their own
            // special-case logic (see make_default_mame_neogeo()). They're
            // now discovered here like any other z-value -- keep their
            // recognizable ids for debugging, but nothing in the code
            // branches on these specific id strings anymore except the
            // zbuffer_key_black line just below.
            lc.id = (game == "mame_neogeo" && z == 2)   ? "neogeo_backdrop"
                  : (game == "mame_neogeo" && z == 255) ? "neogeo_fix"
                  : ("layer_z" + std::to_string(z));
            lc.extraction_type = ExtractionType::ZBuffer;
            lc.dynamic_z_split = true;
            // Once a z-value has appeared, keep its layer forever rather than
            // tearing it down after a few empty windows -- for Neo Geo (see
            // make_default_mame_neogeo()) the z space is a small, fixed,
            // deterministic function of palette bank, not free-floating
            // per-frame noise, so a given palette WILL come back, and an
            // empty layer already renders nothing on its own (extraction
            // just has zero matching pixels this frame). Destroying and
            // recreating it every time its palette leaves and returns is
            // pure churn for no benefit.
            lc.z_layer_permanent = true;
            lc.z_min = static_cast<uint8_t>(z);
            lc.z_max = static_cast<uint8_t>(z);
            lc.depth_meters = 2.0f - (z / 255.0f) * 1.2f;
            // Fix layer content is monochrome text on black -- key black out
            // so it doesn't render as an opaque black box/fringe (see
            // zbuffer_key_black's comment in game_config.h). This used to
            // only be set on the hand-seeded fix layer; now it's set here so
            // it still applies once fix is discovered organically.
            lc.zbuffer_key_black = (game == "mame_neogeo" && z == 255); // RD_Z_FIX in neogeo_spr.h
            // Backdrop used to default off -- it was one of the two layers
            // implicated in the duplication/ghosting bug (the other being
            // fix), and disabling it via the manual layers panel confirmed
            // the issue involved it. Root cause has since been fixed at its
            // source (see the uniform_z_layer check in
            // assign_zbuffer_object_depths(), layer_processor.cpp), so
            // backdrop is back to rendering normally by default.
            // Uniform width and copy-stack for every auto-created layer --
            // leaving this unset stayed at the struct default of NO copies,
            // which the renderer falls back on with a different copy
            // count/span (k_max_copies=20 @ k_default_copy_step), making
            // auto-created layers visibly a different size/thickness than
            // ones seeded some other way.
            apply_uniform_width_and_copies(lc,
                                           k_genesis_default_quad_width,
                                           k_genesis_default_copy_count,
                                           k_genesis_default_copy_step);
            layers.push_back(lc);
            z_absent_windows.push_back(0);
            matched.push_back(true);
        }
    }

    // Tolerate a z-value being briefly absent (a few windows, ~0.5s at the
    // caller's ~0.08s cadence) before actually tearing its layer down --
    // covers a blinking HUD/fix layer or a momentarily-occluded plane
    // without destroying and recreating the layer for it. In practice this
    // path is now moot for any layer created above (they're all permanent),
    // but stays in place for other configs/future dynamic layers that don't
    // want that guarantee.
    constexpr int kAbsentWindowsBeforeRemoval = 6;
    for (size_t i = 0; i < layers.size();) {
        if (layers[i].dynamic_z_split && !layers[i].z_layer_permanent && !matched[i]) {
            if (++z_absent_windows[i] > kAbsentWindowsBeforeRemoval) {
                layers.erase(layers.begin() + (ptrdiff_t)i);
                z_absent_windows.erase(z_absent_windows.begin() + (ptrdiff_t)i);
                matched.erase(matched.begin() + (ptrdiff_t)i);
                continue;
            }
        }
        ++i;
    }

    // DISABLED (testing a duplication/ghosting bug where the fix layer,
    // forced to always be the single closest layer regardless of how much
    // of the screen it actually covers that frame -- e.g. a text-heavy
    // dialogue/story screen drawn via the fix layer -- ends up showing a
    // large chunk of content right in front of the camera while the actual
    // game scene sits far behind it, looking like "the same scene twice at
    // very different sizes"). RD_Z_FIX=52 is already at/near the top of the
    // 0-63 range on its own, so the plain z->depth_meters formula below
    // should naturally place it near the front most of the time without
    // this override forcibly undercutting every other layer.
    //
    // {
    //     int fix_idx = -1;
    //     float min_other_depth = 0.0f;
    //     bool have_other = false;
    //     for (size_t i = 0; i < layers.size(); ++i) {
    //         if (layers[i].id == "neogeo_fix") {
    //             fix_idx = (int)i;
    //             continue;
    //         }
    //         if (!have_other || layers[i].depth_meters < min_other_depth) {
    //             min_other_depth = layers[i].depth_meters;
    //             have_other = true;
    //         }
    //     }
    //     if (fix_idx >= 0 && have_other) {
    //         constexpr float kFixMargin = 0.05f; // metres closer than the next-nearest layer
    //         layers[(size_t)fix_idx].depth_meters = min_other_depth - kFixMargin;
    //     }
    // }

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
        // Superseded by the PerLayerCapture set below (true independent
        // per-plane capture from snes9x's real draw hooks) which does not
        // suffer the "occlusion hole" bug this shared z-buffer slicing has
        // whenever one BG/sprite plane draws over another. Kept disabled
        // for quick A/B comparison.
        lc.default_enabled   = false;
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
        cfg.layers.push_back(std::move(lc));
    }

    apply_even_default_depth_envelope(cfg.layers,
                                      k_snes_default_near_depth,
                                      k_snes_default_far_depth,
                                      k_snes_default_quad_width,
                                      k_snes_default_copy_count,
                                      k_snes_default_copy_step);

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
    // NES: one hardware BG plane plus sprites. The backend classifies true
    // backdrop pixels and splits visible BG pixels into generic far/mid/near
    // buckets so the headset renderer has useful depth planes.
    // Per-pixel source IDs captured by fceux_layer_capture from FCEUmm PPU:
    //   0 = BG (+backdrop fill), 1 = sprites -- true independent captures
    //   from fceux_layer_capture.cpp's two-pass design (BG snapshotted
    //   before CopySprites() runs), so an occluded BG pixel behind a sprite
    //   survives instead of being lost the way the old VisibleSourceFinal
    //   (post-composite slicing) lost it.
    GameConfig cfg;
    cfg.game           = "nes";
    cfg.virtual_width  = 256;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct {
        const char* id;
        float depth;
        int layer_index;
        bool enabled;
    } k[] = {
        { "bg_plane", 1.98f, 0, true },
        { "sprites",  1.66f, 1, true },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        lc.default_enabled  = s.enabled;
        apply_uniform_width_and_copies(lc,
                                       k_nes_default_quad_width,
                                       k_nes_default_copy_count,
                                       k_nes_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_sms() {
    // SMS: 2 true independent captures (BG tile layer incl. backdrop fill,
    // sprites), from picodrive_sms_layer_capture's two-pass design:
    // capture_bg_line() snapshots BG pixels BEFORE DrawSpritesM4() runs, so
    // a sprite drawing over a BG tile never destroys the BG capture -- no
    // "occlusion hole" the way the old VisibleSourceFinal (post-composite
    // slicing) had. layer_index 0 = BG (+backdrop fill), 1 = sprites.
    GameConfig cfg;
    cfg.game           = "sms";
    cfg.virtual_width  = 256;
    cfg.virtual_height = 192;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "bg_plane",  3.5f,                          0 },
        { "sprites",   k_genesis_default_near_depth,  1 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.quad_width_meters = 2.56f;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
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
    // All 5 layers are direct-write PerLayerCapture (mgba_layer_capture.cpp),
    // matching SNES's architecture — mGBA hooks the actual pixel-draw sites
    // instead of reconstructing layer identity from the composited frame.
    GameConfig cfg;
    cfg.game           = "gba";
    cfg.virtual_width  = 240;
    cfg.virtual_height = 160;
    cfg.quad_y_meters  = 1.6f;

    // far → near: BG3 … BG2 … BG1 … OBJ … BG0
    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "bg3",  k_genesis_default_far_depth,                                          3 },
        { "bg2",  k_genesis_default_far_depth * 0.75f + k_genesis_default_near_depth * 0.25f, 2 },
        { "bg1",  k_genesis_default_far_depth * 0.50f + k_genesis_default_near_depth * 0.50f, 1 },
        { "obj",  k_genesis_default_far_depth * 0.25f + k_genesis_default_near_depth * 0.75f, 4 },
        { "bg0",  k_genesis_default_near_depth,                                          0 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
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

GameConfig GameConfig::make_default_mame_cps() {
    // CPS1/CPS2: 384x224, 5 hardware layers, far to near.
    // layer_index must match mame_backend.cpp's fixed name-order mapping
    // (0=background, 1=scroll3, 2=scroll2, 3=scroll1, 4=sprites).
    GameConfig cfg;
    cfg.game           = "mame_cps";
    cfg.virtual_width  = 384;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "background", k_genesis_default_far_depth,                                          0 },
        { "scroll3",    k_genesis_default_far_depth * 0.75f + k_genesis_default_near_depth * 0.25f, 1 },
        { "scroll2",    k_genesis_default_far_depth * 0.50f + k_genesis_default_near_depth * 0.50f, 2 },
        { "scroll1",    k_genesis_default_far_depth * 0.25f + k_genesis_default_near_depth * 0.75f, 3 },
        { "sprites",    k_genesis_default_near_depth,                                          4 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_konami() {
    // Konami K052109/K051960: 4 hardware layers, far to near.
    // layer_index must match mame_backend.cpp's shared name table --
    // scroll2/scroll1/sprites share slots with the CPS config; scroll0 gets
    // its own slot (index 5) since CPS has no equivalent layer.
    GameConfig cfg;
    cfg.game           = "mame_konami";
    cfg.virtual_width  = 288;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "scroll2", k_genesis_default_far_depth,                                          2 },
        { "scroll1", k_genesis_default_far_depth * 0.66f + k_genesis_default_near_depth * 0.34f, 3 },
        { "scroll0", k_genesis_default_far_depth * 0.33f + k_genesis_default_near_depth * 0.66f, 5 },
        { "sprites", k_genesis_default_near_depth,                                          4 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_segas16b() {
    // Sega System 16B/16A: background/foreground/text tilemaps + sprites,
    // far to near. layer_index matches mame_backend.cpp's shared name table
    // (0=background, 4=sprites, 6=foreground, 7=text).
    GameConfig cfg;
    cfg.game           = "mame_segas16b";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "background", k_genesis_default_far_depth,                                          0 },
        { "foreground", k_genesis_default_far_depth * 0.5f + k_genesis_default_near_depth * 0.5f, 6 },
        { "sprites",    k_genesis_default_far_depth * 0.25f + k_genesis_default_near_depth * 0.75f, 4 },
        { "text",       k_genesis_default_near_depth,                                          7 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_dec0() {
    // Data East dec0.cpp: background/midground/foreground tilegen slots +
    // sprites, far to near. layer_index matches mame_backend.cpp's shared
    // name table (0=background, 4=sprites, 6=foreground, 8=midground).
    GameConfig cfg;
    cfg.game           = "mame_dec0";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "background", k_genesis_default_far_depth,                                          0 },
        { "midground",  k_genesis_default_far_depth * 0.66f + k_genesis_default_near_depth * 0.34f, 8 },
        { "sprites",    k_genesis_default_far_depth * 0.33f + k_genesis_default_near_depth * 0.66f, 4 },
        { "foreground", k_genesis_default_near_depth,                                          6 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_gp9001() {
    // Toaplan GP9001 VDP: 3 independent tilemap layers + sprites, far to
    // near. layer_index matches mame_backend.cpp's shared name table
    // (4=sprites, 9=layer0, 10=layer1, 11=layer2).
    GameConfig cfg;
    cfg.game           = "mame_gp9001";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "layer0",  k_genesis_default_far_depth,                                          9 },
        { "layer1",  k_genesis_default_far_depth * 0.66f + k_genesis_default_near_depth * 0.34f, 10 },
        { "layer2",  k_genesis_default_far_depth * 0.33f + k_genesis_default_near_depth * 0.66f, 11 },
        { "sprites", k_genesis_default_near_depth,                                          4 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_neogeo() {
    // Neo Geo: 320x224, no hardware layers at all.
    //
    // TODO(occlusion-hole fix, in progress): an earlier attempt exported 32
    // z-range "buckets" (PerLayerCapture, layer_index 12+b) and produced a
    // black screen in-headset on-device (audio kept playing, FPS stayed at
    // 66-74/72, and mame_layer_pixels/has_pixels all confirmed real opaque
    // data flowing through) -- root cause not yet found, reverted.
    //
    // Currently trying a smaller, cheaper experiment instead: cap true
    // independent capture to the first RD_DRAW_CAP=30 distinct sprites the
    // renderer actually visits each frame, in real draw order (no z-range
    // bucketing at all -- see rd_claim_capture_slot()'s comment in
    // neogeo_spr.h). Everything else (backdrop, fix layer, sprites beyond
    // the cap) stays in "neogeo_base", the ordinary flat composite exported
    // alongside, so nothing goes missing -- only the first 30 sprites get
    // real depth separation. If this renders correctly it confirms the
    // concept works and the black screen above was something about the
    // bucket/z-range design specifically (or scale); if it ALSO goes black,
    // that points at something more structural in how these layers get
    // consumed downstream, independent of bucket count.
    return make_default_mame_neogeo_capped_capture();
}

GameConfig GameConfig::make_default_mame_neogeo_capped_capture() {
    GameConfig cfg;
    cfg.game           = "mame_neogeo";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    constexpr int kDrawCap = 30; // must match neosprite_base_device::RD_DRAW_CAP

    // NOTE: the depth values apply_even_default_depth_envelope assigns below
    // are NOT what actually places these layers in 3D -- openxr_shell.cpp's
    // render loop unconditionally re-derives depth for m_config.game ==
    // "mame_neogeo" via apply_accordion_layer_depths(), which walks the
    // RENDER-ORDER list (default_layer_order_for_config's neogeo branch,
    // which sorts by z_min ascending) and gives array-index-0 the NEAREST
    // depth, increasing with each step. So LOWEST z_min = NEAREST.
    //
    // z_min for "neogeo_drawN" is NOT static -- openxr_shell.cpp updates it
    // every frame (see the "mame_neogeo" block near its update_z_splits()
    // call) from neogeo_v.cpp's live per-layer z_order (mame_layer_z_order()),
    // which is the REAL palette bank (0..255) that claimed that slot this
    // frame -- so the 30 draws sort by actual palette number (0 = furthest,
    // 255 = nearest), respecting the real numeric gaps between palette
    // banks in use, not just "first encountered" claim order. "neogeo_fix"
    // (the HUD/"insert coin" layer) is the one explicit exception: it's not
    // a sprite and carries no such palette number, so it's pinned to the
    // lowest possible z_min (0) here, once, and never updated -- always the
    // single nearest layer, full stop. "neogeo_base" (the flat catch-all
    // for everything not separately captured) is pinned to the highest
    // z_min (255) -- always farthest.
    {
        LayerConfig lc;
        lc.id              = "neogeo_base";
        lc.extraction_type = ExtractionType::PerLayerCapture;
        lc.layer_index     = 12; // must match kMameLayerNames' "neogeo_base" position
        lc.z_min           = 255; // always sorts last -> farthest
        cfg.layers.push_back(std::move(lc));
    }
    {
        LayerConfig lc;
        lc.id              = "neogeo_fix";
        lc.extraction_type = ExtractionType::PerLayerCapture;
        lc.layer_index     = 13; // must match kMameLayerNames' "neogeo_fix" position
        lc.z_min           = 0; // always sorts first -> nearest (explicit exception)
        cfg.layers.push_back(std::move(lc));
    }
    char id_buf[16];
    for (int b = 0; b < kDrawCap; ++b) {
        std::snprintf(id_buf, sizeof(id_buf), "neogeo_draw%d", b);
        LayerConfig lc;
        lc.id               = id_buf;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        // layer_index must match kMameLayerNames' position for "neogeo_drawN"
        // in mame_backend.cpp (12 fixed names + "neogeo_base" + "neogeo_fix"
        // precede these).
        lc.layer_index      = 14 + b;
        // Placeholder until the first per-frame update in openxr_shell.cpp
        // (see the big comment above) overwrites this with the real
        // palette-derived value; kept strictly between neogeo_fix's 0 and
        // neogeo_base's 255 so ordering never collides even before that
        // first update lands.
        lc.z_min            = 128;
        cfg.layers.push_back(std::move(lc));
    }
    apply_even_default_depth_envelope(cfg.layers,
                                      k_snes_default_near_depth,
                                      k_snes_default_far_depth,
                                      k_snes_default_quad_width,
                                      k_snes_default_copy_count,
                                      k_snes_default_copy_step);
    return cfg;
}

GameConfig GameConfig::make_default_mame_neogeo_zbuffer_fallback() {
    // Neo Geo: 320x224, no hardware layers at all.
    //
    // Previous, known-working z-buffer/dynamic-layers config -- kept as a
    // named fallback (rather than deleted) so it can be swapped back in
    // instantly if the capped-capture experiment above doesn't pan out.
    //
    // Every other config in this file enumerates fixed layers because the
    // hardware has fixed planes to enumerate. Neo Geo does not -- its display
    // is a backdrop fill, one sprite pass, and the fix layer (neogeo_v.cpp), so
    // depth cannot be read off the hardware and is synthesized per-pixel on the
    // MAME side instead, into a small fixed set of z-values: RD_Z_BACKDROP=2,
    // 10 sprite planes (rd_sprite_z[] in neogeo_spr.cpp = 5,10,...,50), and
    // RD_Z_FIX=52. All 12 are seeded here as permanent layers up front instead
    // of being discovered/grown by update_z_splits() from histogram occupancy
    // -- letting that function auto-add/remove layers as sprite planes come
    // and go frame to frame collapsed the visible layer count down to
    // whichever 3-4 happened to be occupied at any instant, which read on
    // device as the whole layer set flickering/regrouping. Since the complete
    // z-value set is a fixed constant of the MAME-side synthesis (not derived
    // from any one frame), seeding it directly and marking every layer
    // z_layer_permanent sidesteps that: update_z_splits() still matches
    // occupied z-values against these by z_min, but never tears any of them
    // down for being briefly empty (a sprite plane with nothing on it this
    // frame, or the fix layer between HUD blinks).
    GameConfig cfg;
    cfg.game           = "mame_neogeo";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;
    cfg.dynamic_layers = true;

    // Nothing is pre-seeded/pinned anymore -- not even backdrop (z=2) and fix
    // (z=52), which used to be hardcoded as special always-present layers
    // with their own id and a forced-closest override for fix. Both showed
    // up wrong (an oversized "duplicate scene" ghost) once actually tested
    // on-device: the "always closest" override forced fix's depth without
    // any regard for how much of the screen it actually covered that frame,
    // and pinning both as separate hand-labeled layers meant they never went
    // through the same generic discovery path as every other z-value. So
    // now backdrop and fix are just two more z-values (2 and 52) that
    // update_z_splits() discovers and creates layers for organically, via
    // the exact same generic append path used for every sprite plane
    // (RD_Z_SPRITE_MIN=3..RD_Z_SPRITE_MAX=51 in neogeo_spr.h) -- same
    // formula-derived depth, same permanence rule, no special-casing left.
    // cfg.layers starts empty; the first update_z_splits() call (driven by
    // the real per-pixel z-buffer histogram) creates every layer, backdrop
    // and fix included.
    return cfg;
}

GameConfig GameConfig::make_default_mame_taito() {
    // Taito PC080SN/PC090OJ (opwolf.cpp/othunder.cpp/undrfire.cpp): two
    // tilemap planes (bg/fg) + one sprite plane, backed by the complete MAME
    // display. The full-frame backing layer sits between the foreground and
    // background in the near-to-far manual order. layer_index matches
    // mame_backend.cpp's slots (44=full_frame, 53=taito_bg,
    // 54=taito_fg, 55=taito_sprites).
    GameConfig cfg;
    cfg.game           = "mame_taito";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "full_frame",   k_genesis_default_far_depth * 0.75f + k_genesis_default_near_depth * 0.25f, 44 },
        { "taito_bg",      k_genesis_default_far_depth,  53 },
        { "taito_fg",      k_genesis_default_far_depth * 0.5f + k_genesis_default_near_depth * 0.5f, 54 },
        { "taito_sprites", k_genesis_default_near_depth, 55 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        // The complete framebuffer is still available as the fourth manual
        // layer, but it is an opaque composite. Showing it together with the
        // independent planes creates a second scene/ghost, so leave it off by
        // default when a Taito driver already supplies the three source planes.
        if (lc.id == "full_frame")
            lc.default_enabled = false;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_namco() {
    // Namco System 2 (namcos2_v.cpp): coarse 2-layer split -- everything
    // except sprites redrawn into one background-composite plane, sprites
    // into a second, far to near. layer_index matches mame_backend.cpp's
    // kNamcoLayerBase slots (56=namco_bg, 57=namco_sprites).
    GameConfig cfg;
    cfg.game           = "mame_namco";
    cfg.virtual_width  = 288;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "namco_bg",      k_genesis_default_far_depth,  56 },
        { "namco_sprites", k_genesis_default_near_depth, 57 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_konami_lethal() {
    // lethal.cpp (Lethal Enforcers): K056832 tilemap sublayers 3/2/1 (far to
    // near), K053244 sprites, then the forced-topmost K056832 layer 0 (text/
    // "A" plane). layer_index matches mame_backend.cpp's kKonamiLethalLayerBase
    // slots (58-62).
    GameConfig cfg;
    cfg.game           = "mame_konami_lethal";
    cfg.virtual_width  = 288;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "lethal_bg3",      k_genesis_default_far_depth,  58 },
        { "lethal_bg2",      k_genesis_default_far_depth * 0.66f + k_genesis_default_near_depth * 0.33f, 59 },
        { "lethal_bg1",      k_genesis_default_far_depth * 0.33f + k_genesis_default_near_depth * 0.66f, 60 },
        { "lethal_sprites",  k_genesis_default_near_depth, 61 },
        { "lethal_text",     k_genesis_default_near_depth * 1.1f, 62 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_taito_tc0100() {
    // taito_z_v.cpp screen_update_spacegun (Space Gun): 2 dynamic-order bg
    // tilemap layers, fixed text layer, sprites. layer_index matches
    // mame_backend.cpp's kTaitoTc0100LayerBase slots (63-66).
    GameConfig cfg;
    cfg.game           = "mame_taito_tc0100";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "spacegun_bg0",      k_genesis_default_far_depth,  63 },
        { "spacegun_bg1",      k_genesis_default_far_depth * 0.5f + k_genesis_default_near_depth * 0.5f, 64 },
        { "spacegun_sprites",  k_genesis_default_near_depth, 65 },
        { "spacegun_text",     k_genesis_default_near_depth * 1.1f, 66 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_taito_tc0480() {
    // gunbustr.cpp / slapshot.cpp (opwolf3): 4 dynamic-order TC0480SCP bg
    // layers, fixed text layer, sprites. layer_index matches
    // mame_backend.cpp's kTaitoTc0480LayerBase slots (67-72).
    GameConfig cfg;
    cfg.game           = "mame_taito_tc0480";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "tc0480_bg0",      k_genesis_default_far_depth,  67 },
        { "tc0480_bg1",      k_genesis_default_far_depth * 0.75f + k_genesis_default_near_depth * 0.25f, 68 },
        { "tc0480_bg2",      k_genesis_default_far_depth * 0.5f  + k_genesis_default_near_depth * 0.5f,  69 },
        { "tc0480_bg3",      k_genesis_default_far_depth * 0.25f + k_genesis_default_near_depth * 0.75f, 70 },
        { "tc0480_sprites",  k_genesis_default_near_depth, 71 },
        { "tc0480_text",     k_genesis_default_near_depth * 1.1f, 72 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_unico() {
    // unico.cpp (zeropnt/zeropnt2): bg/mid/fg tilemaps + sprites.
    // layer_index matches mame_backend.cpp's kUnicoLayerBase slots (73-76).
    GameConfig cfg;
    cfg.game           = "mame_unico";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "unico_bg",       k_genesis_default_far_depth,  73 },
        { "unico_mid",      k_genesis_default_far_depth * 0.66f + k_genesis_default_near_depth * 0.33f, 74 },
        { "unico_fg",       k_genesis_default_far_depth * 0.33f + k_genesis_default_near_depth * 0.66f, 75 },
        { "unico_sprites",  k_genesis_default_near_depth, 76 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_oneshot() {
    // misc/oneshot.cpp: bg/mid tilemaps, sprites, fg tilemap (fg topmost).
    // layer_index matches mame_backend.cpp's kOneshotLayerBase slots (77-80).
    GameConfig cfg;
    cfg.game           = "mame_oneshot";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "oneshot_bg",       k_genesis_default_far_depth,  77 },
        { "oneshot_mid",      k_genesis_default_far_depth * 0.5f + k_genesis_default_near_depth * 0.5f, 78 },
        { "oneshot_sprites",  k_genesis_default_near_depth, 79 },
        { "oneshot_fg",       k_genesis_default_near_depth * 1.1f, 80 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_lordgun() {
    // igs/lordgun_v.cpp: 4 scrolling tilemaps (already isolated by the
    // driver into m_bitmaps[0..3]) + sprites (m_bitmaps[4]). layer_index
    // matches mame_backend.cpp's kLordgunLayerBase slots (81-85).
    GameConfig cfg;
    cfg.game           = "mame_lordgun";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "lordgun_tile0",    k_genesis_default_far_depth,  81 },
        { "lordgun_tile1",    k_genesis_default_far_depth * 0.75f + k_genesis_default_near_depth * 0.25f, 82 },
        { "lordgun_tile2",    k_genesis_default_far_depth * 0.5f  + k_genesis_default_near_depth * 0.5f,  83 },
        { "lordgun_tile3",    k_genesis_default_far_depth * 0.25f + k_genesis_default_near_depth * 0.75f, 84 },
        { "lordgun_sprites",  k_genesis_default_near_depth, 85 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_seta2() {
    // devices/video/x1_020_dx_101.cpp (deerhunt/trophyh/turkhunt/wschamp):
    // sprite-only hardware, single layer. layer_index matches
    // mame_backend.cpp's kSeta2LayerBase slot (86).
    GameConfig cfg;
    cfg.game           = "mame_seta2";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    LayerConfig lc;
    lc.id               = "seta2_sprites";
    lc.depth_meters     = k_genesis_default_near_depth;
    lc.extraction_type  = ExtractionType::PerLayerCapture;
    lc.layer_index      = 86;
    apply_uniform_width_and_copies(lc,
                                   k_genesis_default_quad_width,
                                   k_genesis_default_copy_count,
                                   k_genesis_default_copy_step);
    cfg.layers.push_back(std::move(lc));
    return cfg;
}

GameConfig GameConfig::make_default_mame_segaybd() {
    // segaybd.cpp (rchase): simplified 2-layer split -- combined rotated-
    // tilemap+y-sprites scene, plus the separable b-board sprite plane.
    // layer_index matches mame_backend.cpp's kSegaybdLayerBase slots (87-88).
    GameConfig cfg;
    cfg.game           = "mame_segaybd";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "segaybd_scene",     k_genesis_default_far_depth,  87 },
        { "segaybd_bsprites",  k_genesis_default_near_depth, 88 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_bbusters() {
    // snk/bbusters.cpp (bbusters/mechatt): 2 playfield tilemaps + fix
    // tilemap + 2 sprite-chip composites (palette-bank priority split
    // simplified away). layer_index matches mame_backend.cpp's
    // kBbustersLayerBase slots (89-93).
    GameConfig cfg;
    cfg.game           = "mame_bbusters";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "bbusters_pf1",       k_genesis_default_far_depth,  89 },
        { "bbusters_pf0",       k_genesis_default_far_depth * 0.6f + k_genesis_default_near_depth * 0.4f, 90 },
        { "bbusters_sprites1",  k_genesis_default_far_depth * 0.3f + k_genesis_default_near_depth * 0.7f, 91 },
        { "bbusters_sprites0",  k_genesis_default_near_depth, 92 },
        { "bbusters_fix",       k_genesis_default_near_depth * 1.1f, 93 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_nycaptor() {
    // taito/nycaptor.cpp: simplified 2 background composites + sprites
    // (runtime spot()&3 interleave and 4-priority-band structure
    // collapsed). layer_index matches mame_backend.cpp's
    // kNycaptorLayerBase slots (94-96).
    GameConfig cfg;
    cfg.game           = "mame_nycaptor";
    cfg.virtual_width  = 288;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "nycaptor_bg0",      k_genesis_default_far_depth,  94 },
        { "nycaptor_bg1",      k_genesis_default_far_depth * 0.5f + k_genesis_default_near_depth * 0.5f, 95 },
        { "nycaptor_sprites",  k_genesis_default_near_depth, 96 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_mame_full_frame() {
    GameConfig cfg;
    cfg.game           = "mame_full_frame";
    cfg.virtual_width  = 384;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    LayerConfig layer;
    layer.id                = "full_frame";
    layer.depth_meters      = k_genesis_default_near_depth;
    layer.quad_width_meters = k_genesis_default_quad_width;
    layer.extraction_type   = ExtractionType::PerLayerCapture;
    layer.layer_index       = 44; // final slot in mame_backend.cpp
    layer.default_enabled   = true;
    layer.default_ambilight = true;
    cfg.layers.push_back(std::move(layer));
    return cfg;
}

GameConfig GameConfig::make_default_mame_occupxy() {
    // Generic MAME fallback: six far-to-near draw-occupancy buckets plus a
    // near residual layer. Slots are append-only after the existing MAME
    // table (0..57); do not renumber them.
    GameConfig cfg;
    cfg.game           = "mame_occupxy";
    cfg.virtual_width  = 384;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    constexpr int kBucketCount = 6;
    constexpr int kFirstSlot = 58;
    for (int i = 0; i < kBucketCount; ++i) {
        LayerConfig lc;
        char id[32];
        std::snprintf(id, sizeof(id), "mame_occupxy_%d", i);
        lc.id = id;
        const float t = (float)i / (float)(kBucketCount - 1);
        lc.depth_meters = k_genesis_default_far_depth +
                          (k_genesis_default_near_depth - k_genesis_default_far_depth) * t;
        lc.extraction_type = ExtractionType::PerLayerCapture;
        lc.layer_index = kFirstSlot + i;
        apply_uniform_width_and_copies(lc, k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }

    LayerConfig residual;
    residual.id = "mame_occupxy_residual";
    residual.depth_meters = k_genesis_default_near_depth - 0.08f;
    residual.extraction_type = ExtractionType::MameOccupancyResidual;
    residual.layer_index = kFirstSlot + kBucketCount;
    apply_uniform_width_and_copies(residual, k_genesis_default_quad_width,
                                   k_genesis_default_copy_count,
                                   k_genesis_default_copy_step);
    cfg.layers.push_back(std::move(residual));
    return cfg;
}

GameConfig GameConfig::make_default_mame_saturn() {
    // Saturn source slots, listed far-to-near. VDP2 priority/window mixing is
    // still retained by MAME's normal composite; these captures provide the
    // stable source planes for QRD's fixed eight-layer presentation.
    GameConfig cfg;
    cfg.game           = "mame_saturn";
    cfg.virtual_width  = 320;
    cfg.virtual_height = 224;
    cfg.quad_y_meters  = 1.6f;

    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "saturn_back",  k_genesis_default_far_depth, 45 },
        { "saturn_rbg1",  3.1f,                         51 },
        { "saturn_nbg3",  3.4f,                         49 },
        { "saturn_nbg2",  3.7f,                         48 },
        { "saturn_nbg1",  4.0f,                         47 },
        { "saturn_nbg0",  4.3f,                         46 },
        { "saturn_rbg0",  4.6f,                         50 },
        { "saturn_vdp1",  k_genesis_default_near_depth, 52 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_saturn() {
    // Real Mednafen/Beetle Saturn VDP2/VDP1 per-plane captures (see
    // saturn_layer_capture.h). layer_index matches SATURN_LAYER_* exactly:
    // NBG0-3 = 0-3, RBG0 = 4, RBG1 = 5, VDP1 sprites = 6.
    //
    // Depth order confirmed in-headset with real gameplay (Virtua Cop, post
    // boot-fix): the previous ordering had this backwards -- VDP1 sprites
    // (enemies, reticle, HUD) are the primary foreground gameplay elements
    // and must be nearest, not farthest. NBG0-3/RBG0/RBG1 are VDP2
    // background/raster planes and sit progressively farther behind. NBG0
    // and RBG1 share the same hardware slot (a game uses one or the other,
    // never both), so they get the same depth. Real games can and do
    // reprogram VDP2 priority at runtime -- it's a per-game register
    // setting, not a hardware constant -- so this remains a starting point
    // to tune further per-title from in-headset observation, not a
    // universally "correct" order.
    GameConfig cfg;
    cfg.game           = "saturn";
    cfg.virtual_width  = 352;
    cfg.virtual_height = 240;
    cfg.quad_y_meters  = 1.6f;

    // Authored far -> near (index 0 = farthest): the generic layer-order
    // fallback (default_layer_order_for_config in presentation_shared.h)
    // reverses config.layers' array order to build near -> far display
    // order for the manual layers panel, same convention every other
    // backend's array already follows. Getting this backwards put VDP1
    // (meant to be nearest) last in the panel instead of first.
    static const struct { const char* id; float depth; int layer_index; } k[] = {
        { "saturn_rbg0",  k_genesis_default_far_depth,  4 },
        { "saturn_nbg3",  4.3f,                         3 },
        { "saturn_nbg2",  4.0f,                         2 },
        { "saturn_nbg1",  3.7f,                         1 },
        { "saturn_rbg1",  3.4f,                         5 },
        { "saturn_nbg0",  3.4f,                         0 },
        { "saturn_vdp1",  3.1f,                         6 },
    };
    for (const auto& s : k) {
        LayerConfig lc;
        lc.id               = s.id;
        lc.depth_meters     = s.depth;
        lc.extraction_type  = ExtractionType::PerLayerCapture;
        lc.layer_index      = s.layer_index;
        apply_uniform_width_and_copies(lc,
                                       k_genesis_default_quad_width,
                                       k_genesis_default_copy_count,
                                       k_genesis_default_copy_step);
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_psx() {
    // The default PSX profile intentionally exposes only the final software
    // framebuffer. The opt-in source-layer profile below is independent of
    // this conservative flat presentation.
    GameConfig cfg;
    cfg.game          = "psx";
    cfg.virtual_width = 320;
    cfg.virtual_height = 240;
    cfg.quad_y_meters = 1.6f;

    LayerConfig layer;
    layer.id = "psx_frame";
    layer.depth_meters = 1.5f;
    layer.quad_width_meters = 2.56f;
    layer.extraction_type = ExtractionType::FullFrame;
    cfg.layers.push_back(std::move(layer));
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
