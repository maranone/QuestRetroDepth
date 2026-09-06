#pragma once

#include "emulator_backend.h"
#include "platform/libretro/libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

// Sets the directory the Saturn core reports through RETRO_ENVIRONMENT_GET_
// SYSTEM_DIRECTORY, which is where mednafen/ss/ss.cpp looks for the BIOS
// files sega_101.bin (Japan) / mpr-17933.bin (US/EU) via straight string
// concatenation -- see ss.cpp's region-detection block. Must be called
// before load_content() loads a disc.
void set_saturn_frontend_directory(const std::string& dir);

class SaturnLibretroBackend final : public EmulatorBackend {
public:
    SaturnLibretroBackend();
    ~SaturnLibretroBackend() override;

    const char* backend_name() const override;
    double frame_rate_hz() const override;
    bool load_content(const std::string& rom_path, std::string& error_out) override;
    bool step_frame(const EmulatorInputState& input, std::string& error_out) override;
    const FrameOutput& frame_output() const override;
    bool save_state(std::vector<uint8_t>& out, std::string& error_out) override;
    bool load_state(const void* data, std::size_t size, std::string& error_out) override;
    void set_auto_frame_skip(bool enabled) override;
    void set_preview_mode(bool enabled, bool allow_audio = false) override {
        m_preview_mode = enabled; m_preview_allow_audio = allow_audio;
    }
    void set_layer_capture_mask(uint32_t mask) override;
    void set_gun_mode(bool enabled, int peripheral = 0) override;
    // Second Virtua Gun in port 1 (the core builds one peripheral per libretro
    // port, so player two's gun is simply port 1's lightgun).
    void set_dual_gun_mode(bool enabled) override;
    void soft_reset() override;
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override;
    const uint8_t* system_ram_data() const override;
    std::size_t system_ram_size() const override;
    std::string last_load_warning() const override { return m_last_load_warning; }

    bool handle_environment(unsigned cmd, void* data);
    void handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch);
    int16_t handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const;

private:
    bool ensure_core_initialized(std::string& error_out);
    // Pushes m_gun_mode/m_dual_gun_mode down as port devices.
    void apply_controller_ports();
    void reset_core();
    void ensure_frame_size(unsigned width, unsigned height);
    void write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch);
    void capture_layers(unsigned width, unsigned height);

    FrameOutput           m_frame;
    EmulatorInputState    m_input;
    std::string           m_loaded_rom_path;
    std::string           m_backend_name;
    std::string           m_last_load_warning;
    bool                  m_core_initialized = false;
    bool                  m_game_loaded      = false;
    bool                  m_preview_mode     = false;
    bool                  m_preview_allow_audio = false;
    std::uint64_t         m_video_frame_count = 0;
    bool                  m_last_frame_had_visible_pixels = false;
    uint32_t              m_layer_capture_mask = 0x7Fu; // 7 layers
    double                m_frame_rate_hz = 60.0;
    bool                  m_gun_mode = false;
    bool                  m_dual_gun_mode = false;
    // Queried by the core's own check_variables() (yabasanshiro_frameskip)
    // before retro_load_game() builds the machine -- same load-time-only
    // pattern as MameBackend::m_auto_frame_skip/"mame_autoframeskip".
    bool                  m_auto_frame_skip = false;
};

} // namespace qrd
