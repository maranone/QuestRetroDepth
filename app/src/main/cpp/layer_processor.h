#pragma once
#include "game_config.h"
#include "emulator_backend.h"
#include <vector>
#include <cstdint>

struct LayerFrame {
    std::string id;
    float depth_meters      = 1.5f;
    float quad_width_meters = 2.56f;
    std::vector<float> copies;   // depth offsets toward viewer (metres)
    int width  = 0;
    int height = 0;
    // RGBA bytes (R=byte0, G=byte1, B=byte2, A=byte3).
    // Alpha=0 means fully transparent (color-keyed out).
    std::vector<uint8_t> rgba;
    bool contrib_ambilight = true; // whether this layer feeds the ambilight effect
    // True only when at least one opaque pixel was written (ZBuffer layers may be
    // allocated but contain zero opaque pixels; skip rendering those).
    bool has_pixels = true;
    // True when the layer contains both transparent and opaque pixels, so wedge
    // scaling thickens a cutout silhouette without bulging full-frame layers.
    bool wedge_eligible = false;
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
                                    const qrd::FrameOutput* frame = nullptr);
    void process_into(std::vector<LayerFrame>& out,
                      const uint32_t* src, int src_w, int src_h,
                      const uint8_t* zbuf = nullptr,
                      const qrd::FrameOutput* frame = nullptr);

private:
    const GameConfig& m_config;

    void prepare_frame(LayerFrame& f, const LayerConfig& lc, int w, int h, bool clear_pixels);
    void fill_full_frame    (LayerFrame& f, const LayerConfig& lc, const uint32_t* src, int w, int h);
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
};
