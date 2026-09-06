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
    // MAME OCCUPXY residual: the processor fills this from the final framebuffer
    // wherever the six instrumented draw-order buckets cannot prove coverage.
    MameOccupancyResidual,
};

// Real-geometry render shape for a layer (only takes effect when VrState::real_geometry_boxes
// is on and the layer is in WholeLayer depth mode). Box is the default 5-face extrusion; Floor/
// Ceiling lay the layer flat as a ground/sky plane instead of standing it up; Symmetric mirrors
// the layer as a front+back pair (no side/back faces) for e.g. scroll-shooter side scenery.
enum class LayerGeometryMode : int {
    Box       = 0,
    Floor     = 1,
    Ceiling   = 2,
    Symmetric = 3,
    // Splits a single layer vertically into two regions rendered simultaneously — for
    // backgrounds that bake two surfaces into one plane with no clean per-layer separation to
    // hand to Floor/Ceiling/Box individually (e.g. a beat-em-up's floor+wall, or a wall+ceiling
    // shared in one BG plane). split_pixels is measured from the SplitFloor/SplitCeiling edge
    // respectively — from the bottom for SplitFloor, from the top for SplitCeiling.
    SplitFloor   = 4, // bottom split_pixels rows = Floor, rest = standing Box wall
    SplitCeiling = 5, // top split_pixels rows = Ceiling, rest = standing Box wall
    // Symmetric generalized to N alternating copies spaced around L1 instead of exactly 2 —
    // hall-of-mirrors/tunnel effect. repeat_count controls N (Thick +/- adjusts it in this mode).
    Repeat       = 6,
    // Floor + Ceiling + standing wall from one layer simultaneously (top/bottom split_pixels
    // bands become Ceiling/Floor, the middle remainder stands as a Box wall).
    Room         = 7,
    // Each detected object/pixel-run box gets an independent, frame-stable pseudo-random depth
    // offset (hashed from its own UV rect, so it never re-randomizes) — scatter_range controls
    // the jitter magnitude. Per-object only.
    DepthScatter = 8,
    // Each box's depth offset is instead derived from its own screen-row position (top of screen
    // = farther), giving automatic Mode-7-floor-style perspective. Per-object only.
    AutoYDepth   = 9,
    // Each per-object box rotates to face the HMD instead of using the shared canvas orientation.
    Billboard    = 10,
    // Each per-object box's extrusion depth scales with its own on-screen area — big objects
    // (Mario) get thicker, small ones (a Goomba) get thinner. box_thickness_meters is the ceiling
    // a large object approaches; small objects get proportionally less (floor at 10% of ceiling
    // so nothing fully collapses).
    SizeThickness = 11,
};

struct LayerConfig {
    std::string id;
    float depth_meters      = 1.5f;
    float quad_width_meters = 2.56f;
    // Extra depth-copy offsets (metres toward viewer). Empty = use renderer default.
    std::vector<float> copies;

    // Real-geometry shape for this layer. See LayerGeometryMode.
    LayerGeometryMode geometry_mode = LayerGeometryMode::Box;
    // Box/Symmetric extrusion depth in metres. 0 = auto-derive from copy_span; >0/<0 = explicit
    // override, adjustable per-layer via the layer panel +/- (negative flips extrusion direction
    // toward the viewer). Defaults to an explicit 0.10m rather than auto, per user preference.
    float box_thickness_meters = 0.05f;
    // SplitFloor/SplitCeiling/Room only: how many source pixels belong to the floor/ceiling
    // region (measured from the bottom for SplitFloor, from the top for SplitCeiling; applied to
    // BOTH edges symmetrically for Room); the rest becomes the wall region. 0 = auto (height/5).
    int split_pixels = 0;
    // Repeat only: how many alternating mirrored copies around L1. 0 = auto (3). Clamp [2, 8].
    int repeat_count = 0;
    // DepthScatter only: per-object depth jitter magnitude in metres. 0 = off.
    float scatter_range = 0.5f;
    // AutoYDepth only: per-object depth-from-screen-row range in metres. 0 = off.
    float y_depth_range = 0.5f;

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

    // Dynamic z-split layers that update_z_splits() should never tear down
    // for being briefly absent from the histogram, even after its normal
    // absence-tolerance window -- for z-values known ahead of time to be
    // fixed/structural (e.g. Neo Geo's backdrop and fix/HUD text layers,
    // which always exist even on a frame where the fix layer happens to be
    // blank). Seeded once at config-creation time, matched and kept in place
    // by z-value for the lifetime of the config; never auto-removed.
    bool z_layer_permanent = false;

    // ZBuffer extraction only: also treat near-black source pixels as
    // transparent instead of opaque, for a layer known to be monochrome
    // content on a black background (e.g. Neo Geo's fix/HUD text layer).
    // Needed because a z-band layer's opaque region is bounded per-pixel,
    // not per-quad -- at the edge between an opaque text pixel and a
    // transparent (rgba all-zero) neighbour, bilinear texture filtering
    // blends toward black-with-partial-alpha rather than white-fading-to-
    // nothing, which reads on device as a black fringe/bar around the text
    // rather than a clean edge. Keying black out here removes the source of
    // that blend instead of trying to fix it after the fact.
    bool zbuffer_key_black = false;

    // GameConfig::rank_spread_layers only: marks this layer as one of the N
    // interchangeable "plane slots" whose z_min/z_max update_z_splits()
    // rewrites every window -- instead of the usual exact-match-by-constant
    // behaviour. See rank_spread_layers below for what this replaces and why.
    bool z_rank_spread_slot = false;
};

struct GameConfig {
    std::string game;
    int virtual_width  = 256;
    int virtual_height = 224;
    float quad_y_meters = 1.6f;   // vertical centre in stage space (floor = 0)
    std::vector<LayerConfig> layers;
    bool dynamic_layers = false;   // enable dynamic z-split (auto-adjust from histogram)
    // Parallel to `layers`, 1:1 by index -- how many consecutive
    // update_z_splits() calls a dynamic layer's z-value has been missing
    // from the histogram. Lets a z-band layer survive a few empty windows
    // (HUD blink, momentary occlusion, etc.) instead of being torn down and
    // rebuilt every time, which read as the whole layer set flickering.
    std::vector<int> z_absent_windows;

    // When set, update_z_splits() stops matching z-values to layers by exact
    // constant and instead treats every layer with z_rank_spread_slot=true as
    // an interchangeable bucket: whatever distinct z-values are occupied this
    // window (excluding any claimed by a non-spread permanent layer, e.g.
    // backdrop/fix) are ranked and evenly divided across those bucket slots,
    // in slot array order (so slot 0 always gets the farthest-ranked values,
    // the last slot the nearest). This is what lets e.g. a Neo Geo scene with
    // only 3 distinct z-values still fill 3 of 5 sprite planes with real
    // content instead of 3 fixed-constant layers lighting up and the other 2
    // permanently sitting empty because their pinned constant never appears.
    bool rank_spread_layers = false;

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
    // CPS1/CPS2 (MAME): background/scroll3/scroll2/scroll1/sprites hardware
    // layers, layer_index 0-4 matching mame_backend.cpp's fixed name order.
    static GameConfig make_default_mame_cps();
    // Konami K052109/K051960 boards (MAME): Cuebrick, M.I.A., TMNT, TMNT2
    // (tmnt.cpp) and The Simpsons (simpsons.cpp) -- scroll2/scroll1/scroll0/
    // sprites, layer_index matching mame_backend.cpp's shared name table.
    static GameConfig make_default_mame_konami();
    // Sega System 16B/16A (segas16b.cpp/segas16a.cpp, shared segaic16vid
    // device): background/foreground/text/sprites.
    static GameConfig make_default_mame_segas16b();
    // Data East dec0.cpp (Bad Dudes, Heavy Barrel, RoboCop, Sly Spy,
    // Birdie Try, Bandit): background/midground/foreground/sprites.
    static GameConfig make_default_mame_dec0();
    // Toaplan GP9001 VDP (gp9001.cpp, shared by ~10 toaplan2-era driver
    // files -- Truxton II, Batsugun, Dogyuun, Fixeight, V-Five, Snow Bros 2,
    // Kbash, Battle Garegga, etc): layer0/layer1/layer2/sprites.
    static GameConfig make_default_mame_gp9001();
    // Neo Geo (snk/neogeo.cpp -- Metal Slug, KOF, Fatal Fury, Samurai Shodown,
    // ...). Unlike every other MAME config here this one has NO fixed layer
    // list: the hardware has no separable planes, so neogeo_v.cpp synthesizes a
    // per-pixel z-buffer instead and dynamic_layers lets update_z_splits()
    // derive the layers from it at runtime.
    static GameConfig make_default_mame_neogeo();
    static GameConfig make_default_mame_neogeo_capped_capture();
    static GameConfig make_default_mame_neogeo_zbuffer_fallback();
    // Sega Saturn (MAME): fixed VDP2 source planes plus the combined VDP1
    // sprite/polygon pass. The driver exports eight named capture slots.
    static GameConfig make_default_mame_saturn();
    static GameConfig make_default_saturn();
    static GameConfig make_default_psx();
    // Taito PC080SN/PC090OJ (opwolf/othunder/undrfire): two tilemap planes
    // (bg/fg) plus one sprite plane.
    static GameConfig make_default_mame_taito();
    // Namco System 2 (bubbletr/gollygho/luckywld/sgunner/sgunner2): coarse
    // background-composite + sprites split.
    static GameConfig make_default_mame_namco();
    // Konami K056832/K053244 (lethal.cpp -- Lethal Enforcers): 3 tilemap
    // sublayers + sprites + a forced-topmost text tilemap.
    static GameConfig make_default_mame_konami_lethal();
    // Taito TC0100SCN (taito_z_v.cpp screen_update_spacegun -- Space Gun):
    // 2 bg tilemap layers + text layer + sprites.
    static GameConfig make_default_mame_taito_tc0100();
    // Taito TC0480SCP (gunbustr.cpp, slapshot.cpp -- opwolf3): 4 dynamic-
    // order bg layers + fixed text layer + sprites.
    static GameConfig make_default_mame_taito_tc0480();
    // Unico (unico.cpp -- zeropnt/zeropnt2): 3 tilemaps + sprites.
    static GameConfig make_default_mame_unico();
    // Misc oneshot.cpp: bg/mid tilemaps, sprites, fg tilemap.
    static GameConfig make_default_mame_oneshot();
    // IGS lordgun_v.cpp: 4 scrolling tilemaps + sprites.
    static GameConfig make_default_mame_lordgun();
    // Seta X1-020/dx-101 device (deerhunt/trophyh/turkhunt/wschamp):
    // sprite-only hardware, single layer.
    static GameConfig make_default_mame_seta2();
    // Sega Y-board (segaybd.cpp -- rchase): simplified 2-layer split.
    static GameConfig make_default_mame_segaybd();
    // SNK bbusters.cpp (bbusters/mechatt): 2 playfield tilemaps + fix
    // tilemap + 2 sprite-chip composites.
    static GameConfig make_default_mame_bbusters();
    // Taito nycaptor.cpp: simplified 2 background composites + sprites.
    static GameConfig make_default_mame_nycaptor();
    // Safe fallback for drivers without a verified independent layer hook.
    static GameConfig make_default_mame_full_frame();
    // Manual generic MAME fallback: six draw-order buckets plus a residual layer.
    static GameConfig make_default_mame_occupxy();

    // Update dynamic z-splits based on z-buffer histogram analysis.
    // Finds natural z-value clusters and adjusts z_min/z_max for dynamic layers.
    // Returns number of active layers (0 if no change).
    int update_z_splits(const uint32_t histogram[256]);
};

// Redistribute depths uniformly [far..near] while preserving ordering.
void even_spread_layer_depths(std::vector<LayerConfig>& layers);
