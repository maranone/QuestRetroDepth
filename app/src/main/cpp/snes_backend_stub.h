#pragma once

#include "emulator_backend.h"

namespace qrd {

class SnesBackendStub final : public EmulatorBackend {
public:
    SnesBackendStub();

    const char* backend_name() const override;
    bool load_content(const std::string& rom_path, std::string& error_out) override;
    bool step_frame(const EmulatorInputState& input, std::string& error_out) override;
    const FrameOutput& frame_output() const override;
    void set_auto_frame_skip(bool enabled) override {}
    void set_layer_capture_mask(uint32_t mask) override {}
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override { return nullptr; }
    const uint8_t* system_ram_data() const override { return nullptr; }
    std::size_t system_ram_size() const override { return 0; }
private:
    void rebuild_placeholder_frame();

    FrameOutput m_frame;
    std::string m_loaded_rom_path;
    uint32_t m_frame_counter = 0;
};

} // namespace qrd
