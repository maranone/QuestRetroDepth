#pragma once

#include "emulator_backend.h"
#include "platform/libretro/libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

class FceuxBackend final : public EmulatorBackend {
public:
    FceuxBackend();
    ~FceuxBackend() override;

    const char* backend_name() const override;
    bool load_content(const std::string& rom_path, std::string& error_out) override;
    bool step_frame(const EmulatorInputState& input, std::string& error_out) override;
    const FrameOutput& frame_output() const override;
    bool save_state(std::vector<uint8_t>& out, std::string& error_out) override;
    bool load_state(const void* data, std::size_t size, std::string& error_out) override;
    void set_auto_frame_skip(bool enabled) override;
    void set_layer_capture_mask(uint32_t mask) override;
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override;
    const uint8_t* system_ram_data() const override;
    std::size_t system_ram_size() const override;

    bool handle_environment(unsigned cmd, void* data);
    void handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch);
    int16_t handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const;

private:
    bool ensure_core_initialized(std::string& error_out);
    void reset_core();
    void ensure_frame_size(unsigned width, unsigned height);
    void write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch);

    FrameOutput           m_frame;
    EmulatorInputState    m_input;
    std::vector<uint8_t>  m_rom_bytes;
    std::string           m_loaded_rom_path;
    std::string           m_backend_name;
    bool                  m_core_initialized = false;
    bool                  m_game_loaded      = false;
    std::uint64_t         m_video_frame_count = 0;
    bool                  m_last_frame_had_visible_pixels = false;
};

} // namespace qrd
