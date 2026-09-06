#pragma once

#include "emulator_backend.h"
#include "platform/libretro/libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

void set_mgba_frontend_directories(std::string system_dir, std::string save_dir);

class MgbaBackend final : public EmulatorBackend {
public:
    MgbaBackend();
    ~MgbaBackend() override;

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
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override;
    const uint8_t* system_ram_data() const override;
    std::size_t system_ram_size() const override;

    bool handle_environment(unsigned cmd, void* data);
    void handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch);
    int16_t handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    void set_channel_volume(int channel, float volume) const;

private:
    bool ensure_core_initialized(std::string& error_out);
    void reset_core();
    void ensure_frame_size(unsigned width, unsigned height);
    void write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch);
    void write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height, std::size_t pitch);

    FrameOutput           m_frame;
    EmulatorInputState    m_input;
    std::vector<uint8_t>  m_rom_bytes;
    std::string           m_loaded_rom_path;
    std::string           m_backend_name;
    bool                  m_core_initialized = false;
    bool                  m_game_loaded      = false;
    bool                  m_preview_mode     = false;
    bool                  m_preview_allow_audio = false;
    std::uint64_t         m_video_frame_count = 0;
    bool                  m_last_frame_had_visible_pixels = false;
    uint32_t              m_layer_capture_mask = 0x1Fu;
    retro_pixel_format    m_pixel_format = RETRO_PIXEL_FORMAT_RGB565;
    double                m_frame_rate_hz = 59.7275;
    // Queried by the core's own check_variables() ("mgba_frameskip") before
    // retro_load_game() builds the machine -- see the note by that GET_VARIABLE
    // branch in mgba_backend.cpp for why this maps to a fixed skip count, not
    // a real adaptive/auto mode.
    bool                  m_auto_frame_skip = false;
};

} // namespace qrd
