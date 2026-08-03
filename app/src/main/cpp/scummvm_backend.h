#pragma once

#include "emulator_backend.h"
#include "libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

class ScummVmBackend final : public EmulatorBackend {
public:
    ScummVmBackend();
    ~ScummVmBackend() override;

    const char* backend_name() const override;
    double frame_rate_hz() const override;
    bool load_content(const std::string& rom_path, std::string& error_out) override;
    bool step_frame(const EmulatorInputState& input, std::string& error_out) override;
    const FrameOutput& frame_output() const override;
    bool save_state(std::vector<uint8_t>& out, std::string& error_out) override;
    bool load_state(const void* data, std::size_t size, std::string& error_out) override;

    bool handle_environment(unsigned cmd, void* data);
    void handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch);
    int16_t handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    void set_auto_frame_skip(bool enabled) override;
    void set_layer_capture_mask(uint32_t mask) override;
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override;
    const uint8_t* system_ram_data() const override;
    std::size_t system_ram_size() const override;

private:
    bool ensure_core_initialized(std::string& error_out);
    bool load_core_symbols(std::string& error_out);
    void reset_core();
    void ensure_frame_size(unsigned width, unsigned height);
    void update_geometry(const retro_game_geometry& geometry);
    void write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch);
    void write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch);
    void generate_y_zbuffer(unsigned width, unsigned height);
    void recompute_depths();

    // Per-actor layer capture — filled by s_layer_export_cb during retro_run, consumed by step_frame.
    struct CapturedLayer {
        std::string id;
        std::vector<uint8_t> rgba; // RGBA bytes (already BGRA→RGBA converted)
        uint32_t w = 0, h = 0;
    };
    static void s_layer_export_cb(const char* id, uint32_t w, uint32_t h, const uint8_t* bgra);

    std::vector<CapturedLayer> m_captured_layers;
    std::vector<std::string>   m_layer_order;     // discovery order; [0]=furthest (background)
    std::vector<uint64_t>      m_layer_last_seen;  // step_count when each layer was last exported
    uint64_t m_step_count   = 0;
    float    m_depth_spread = 1.0f;

    FrameOutput m_frame;
    EmulatorInputState m_input;
    std::string m_game_id;
    std::string m_backend_name;
    bool m_core_initialized = false;
    bool m_game_loaded = false;
    retro_pixel_format m_pixel_format = RETRO_PIXEL_FORMAT_RGB565;
    uint64_t m_video_frame_count = 0;
    bool m_last_frame_had_visible_pixels = false;
    bool m_variables_dirty = false;
    uint32_t m_layer_capture_mask = 0;
    double m_frame_rate_hz = 60.0;
    // Mouse delta tracking: ScummVM libretro expects relative deltas each frame.
    mutable int16_t m_last_mouse_x = 0;
    mutable int16_t m_last_mouse_y = 0;
};

} // namespace qrd
