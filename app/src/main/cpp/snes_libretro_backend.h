#pragma once

#include "emulator_backend.h"
#include "libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

class SnesLibretroBackend final : public EmulatorBackend {
public:
    SnesLibretroBackend();
    ~SnesLibretroBackend() override;

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
    void set_preview_mode(bool enabled, bool allow_audio = false) override {
        m_preview_mode = enabled; m_preview_allow_audio = allow_audio;
    }
    void set_layer_capture_mask(uint32_t mask) override;
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override;
    const uint8_t* system_ram_data() const override;
    std::size_t system_ram_size() const override;

    // Switches port 2 (index 1) between a normal joypad and a lightgun
    // peripheral. `peripheral` selects which one: 0 = Super Scope (Super
    // Scope 6, Yoshi's Safari, Battle Clash, Tin Star, Metal Combat, X-Zone),
    // 1 = Justifier (Lethal Enforcers, T2: The Arcade Game) -- see
    // snes_gun_peripheral_id(). The two use incompatible on-console
    // protocols, so picking the wrong one connects *a* gun but the game
    // never reads a sane position from it.
    void set_gun_mode(bool enabled, int peripheral = 0) override;
    // Second Justifier, daisy-chained off port 1 as snes9x's "Justifier (2P)"
    // device (which the core requires to be assigned to port 2, and then polls
    // through port 2's lightgun inputs). Ignored for the Super Scope: it has
    // no two-player mode on real hardware or in the core.
    void set_dual_gun_mode(bool enabled) override;
    void soft_reset() override;

    // Per-channel audio volume control (0.0..1.0, default 1.0)
    // Channel range: 0-7 for the 8 SNES SPC700 DSP voices
    void set_channel_volume(int channel, float volume);


private:
    bool ensure_core_initialized(std::string& error_out);
    // Pushes m_gun_mode/m_gun_peripheral/m_dual_gun_mode down as port devices.
    void apply_controller_ports();
    void reset_core();
    bool load_file_bytes(const std::string& rom_path, std::string& error_out);
    void ensure_frame_size(unsigned width, unsigned height);
    void update_geometry(const retro_game_geometry& geometry);
    uint32_t joypad_mask() const;
    void write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch);
    void write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch);

    FrameOutput m_frame;
    EmulatorInputState m_input;
    std::vector<uint8_t> m_rom_bytes;
    std::string m_loaded_rom_path;
    std::string m_backend_name;
    bool m_core_initialized = false;
    bool m_game_loaded = false;
    retro_pixel_format m_pixel_format;
    std::uint64_t m_video_frame_count = 0;
    bool m_last_frame_had_visible_pixels = false;
    bool m_auto_frame_skip = false;
    bool m_preview_mode = false;
    bool m_preview_allow_audio = false;
    bool m_variables_dirty = false;
    uint32_t m_layer_capture_mask = 0x1Fu;
    bool m_gun_mode = false;
    int  m_gun_peripheral = 0; // 0=Super Scope, 1=Justifier; see snes_gun_peripheral_id()
    bool m_dual_gun_mode = false;
    mutable uint32_t m_z_histogram[256] = {};
    mutable bool m_histogram_valid = false;
    double m_frame_rate_hz = 60.0988;
};

} // namespace qrd
