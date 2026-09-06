#pragma once
#include "game_config.h"
#include "emulator_backend.h"
#include <vector>
#include <cstdint>

struct ObjectBoundingBox {
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    float depth_meters = -1.0f;
};

struct LayerFrame {
    std::string id;
    float depth_meters      = 1.5f;
    float quad_width_meters = 2.56f;
    std::vector<float> copies;   // depth offsets toward viewer (metres)
    LayerGeometryMode geometry_mode = LayerGeometryMode::Box;
    float box_thickness_meters = 0.0f; // 0 = auto-derive from copy_span
    int split_pixels = 0; // SplitFloor/SplitCeiling/Room only; 0 = auto (height/5)
    int repeat_count = 0; // Repeat only; 0 = auto (3), clamp [2,8]
    float scatter_range = 0.5f; // DepthScatter only, metres
    float y_depth_range = 0.5f; // AutoYDepth only, metres
    int width  = 0;
    int height = 0;
    // RGBA bytes (R=byte0, G=byte1, B=byte2, A=byte3).
    // Alpha=0 means fully transparent (color-keyed out).
    std::vector<uint8_t> rgba;
    // When perspective compensation is active, this is depth_i/ref_depth (>=1.0).
    // The renderer uses it to zoom into the texture centre so content fills the
    // correct visual angle without transparent bars at the edges.
    float persp_comp_scale = 1.0f;
    bool contrib_ambilight = true; // whether this layer feeds the ambilight effect
    // Side/back face color override for real-geometry (PixelExtrude/PixelFx) boxes:
    // 0=Ori (sample the real texture, default) 1=Black 2=White 3=Red 4=Green 5=Blue.
    // The front face always shows the real sprite/tile regardless of this value.
    int side_color_mode = 0;
    // Y-depth map for sprite layers (non-empty when sprite_y_depth is enabled and backend supplied it).
    // One uint8_t per pixel: 0=top of screen (far), 255=bottom (close).
    std::vector<uint8_t> depth_map;
    // True only when at least one opaque pixel was written (ZBuffer layers may be
    // allocated but contain zero opaque pixels; skip rendering those).
    bool has_pixels = true;
    bool is_ui_bar = false; // ScummVM: render detached below game screen at front depth
    // True when the layer contains both transparent and opaque pixels, so wedge
    // scaling thickens a cutout silhouette without bulging full-frame layers.
    bool wedge_eligible = false;
    // True when connected-component bounding boxes were extracted successfully.
    bool bbox_eligible = false;
    bool zbuffer_depth_valid = false;
    std::vector<ObjectBoundingBox> object_boxes;
    // Single tight rectangle enclosing every non-transparent pixel in the whole layer — a
    // plain min/max scan, NOT per-object clustering like object_boxes (no flood-fill/labeling,
    // so no room for the same class of bug: nothing to merge or mis-split). Used by the
    // WholeLayer real-geometry box/floor/ceiling/symmetric/split path so a small sprite on an
    // otherwise-empty full-frame layer gets a footprint that hugs its actual silhouette instead
    // of the padded frame — so side faces sample real edge pixels instead of empty transparency.
    bool content_bounds_valid = false;
    ObjectBoundingBox content_bounds;
    // Per-row / per-column silhouette edge profile, used when VrState::silhouette_sides is on
    // so a real-geometry box's side faces follow the sprite's actual outline at each height
    // instead of one fixed edge column repeated — e.g. Mario's side reads as his real silhouette
    // (head/shoulders/legs), not a mostly-transparent sliver. Still just a min/max scan per row
    // and per column, no clustering — same category as content_bounds, not object detection.
    // edge_lr: one {left_u, right_u} pair per row (size = height*2), normalized 0..1 over width.
    // edge_tb: one {top_v, bottom_v} pair per column (size = width*2), normalized 0..1 over height.
    // Empty when not wedge_eligible or when a row/column has no opaque pixel at all (gaps are
    // filled from the nearest valid neighbour, same approach used for the environment sphere's
    // band gap-filling).
    std::vector<float> edge_lr;
    std::vector<float> edge_tb;
    std::uint64_t content_revision = 0;
};

// Slices a single RGBA frame into per-layer RGBA frames according to GameConfig.
// Input pixel format: uint32_t per pixel, stored as [B,G,R,A] bytes on little-endian
// ARM (i.e. the snes9x backend format).  The processor converts to [R,G,B,A] output.
class LayerProcessor {
public:
    explicit LayerProcessor(const GameConfig& config);

    // src: one uint32_t per pixel in [B,G,R,A] byte order, src_w × src_h pixels.
    // zbuf: optional snes9x z-buffer (one uint8 per pixel, same dimensions).
    //       Required for ExtractionType::ZBuffer layers; may be nullptr otherwise.
    // frame: full FrameOutput from the backend; used for PerLayerCapture layers.
    // Returns one LayerFrame per layer in the config.
    std::vector<LayerFrame> process(const uint32_t* src, int src_w, int src_h,
                                    const uint8_t* zbuf = nullptr,
                                    const qrd::FrameOutput* frame = nullptr,
                                    bool build_object_boxes = false,
                                    bool build_extrude_runs = false,
                                    bool use_zbuffer_depths = false);
    void process_into(std::vector<LayerFrame>& out,
                      const uint32_t* src, int src_w, int src_h,
                      const uint8_t* zbuf = nullptr,
                      const qrd::FrameOutput* frame = nullptr,
                      bool build_object_boxes = false,
                      bool build_extrude_runs = false,
                      bool use_zbuffer_depths = false);

private:
    const GameConfig& m_config;

    void prepare_frame(LayerFrame& f, const LayerConfig& lc, int w, int h, bool clear_pixels);
    void fill_full_frame    (LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h,
                             const uint8_t* depth_map_src = nullptr, std::size_t depth_map_npix = 0);
    void fill_region        (LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h);
    void fill_color_key     (LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h, bool invert);
    void fill_per_layer_capture(LayerFrame& f, const LayerConfig& lc,
                                const qrd::FrameOutput* frame,
                                int w, int h);
    void fill_visible_source_final(LayerFrame& f, const LayerConfig& lc,
                                   const uint32_t* src,
                                   const qrd::FrameOutput* frame,
                                   int w, int h);
    void fill_visible_source_hybrid(LayerFrame& f, const LayerConfig& lc,
                                    const uint32_t* src,
                                    const qrd::FrameOutput* frame,
                                    int w, int h);
    void fill_mame_occupancy_residual(LayerFrame& f, const LayerConfig& lc,
                                      const uint32_t* src,
                                      const qrd::FrameOutput* frame,
                                      int w, int h);

    LayerFrame extract_full_frame    (const LayerConfig& lc, const uint32_t* src, int w, int h);
    LayerFrame extract_region        (const LayerConfig& lc, const uint32_t* src, int w, int h);
    LayerFrame extract_color_key     (const LayerConfig& lc, const uint32_t* src, int w, int h, bool invert);
    // Single-pass: fill all ZBuffer layers at once (one loop over all pixels).
    void extract_all_zbuffer_layers(std::vector<LayerFrame>& frames,
                                    const std::vector<int>& zbuf_indices,
                                    const uint32_t* src,
                                    const uint8_t* zbuf, int w, int h);

    // Copy a per-layer capture buffer from FrameOutput into a LayerFrame.
    // Falls back to FullFrame if the capture is not available.
    LayerFrame extract_per_layer_capture(const LayerConfig& lc,
                                         const qrd::FrameOutput* frame,
                                         int w, int h);
    LayerFrame extract_visible_source_final(const LayerConfig& lc,
                                            const uint32_t* src,
                                            const qrd::FrameOutput* frame,
                                            int w, int h);
    LayerFrame extract_visible_source_hybrid(const LayerConfig& lc,
                                             const uint32_t* src,
                                             const qrd::FrameOutput* frame,
                                             int w, int h);

    // Convert one source pixel to RGBA bytes
    static void to_rgba(uint32_t src_pixel, uint8_t* out_rgba);
    // Returns true if src_pixel colour matches the LayerConfig key within tolerance
    static bool color_match(uint32_t src_pixel, const LayerConfig& lc);
    static void finalize_frame(LayerFrame& frame);
    static void compute_object_boxes(LayerFrame& frame);
    // Voxelizes the layer into one thin real-geometry box per contiguous run of opaque pixels,
    // along whichever axis (rows or columns) produces fewer total boxes for this frame. See
    // DepthMode::PixelExtrude.
    static void compute_extrude_runs(LayerFrame& frame);
    void assign_zbuffer_object_depths(std::vector<LayerFrame>& frames,
                                      const uint8_t* zbuf, int w, int h);
    bool can_use_genesis_hybrid_fast_path(const qrd::FrameOutput* frame, int w, int h) const;
    void process_genesis_hybrid_fast(std::vector<LayerFrame>& result,
                                     const uint32_t* src,
                                     const qrd::FrameOutput* frame,
                                     int w, int h);
};
