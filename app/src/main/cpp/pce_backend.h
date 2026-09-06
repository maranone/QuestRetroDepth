#pragma once

#include "emulator_backend.h"
#include "platform/libretro/libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

// Sets the directory beetle-pce reports through RETRO_ENVIRONMENT_GET_SYSTEM_
// DIRECTORY, which is where it looks for the PCE-CD BIOS/system card image
// (syscard3.pce and friends -- see MDFN_MakeFName() in
// third_party/beetle-pce/libretro.cpp) via straight string concatenation, no
// search fallback. Must be called before load_content() loads a CD game.
void set_pce_system_directory(const std::string& dir);

class PceBackend final : public EmulatorBackend {
public:
    PceBackend();
    ~PceBackend() override;

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
    void set_pce_channel_volume(int channel, float volume);
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
    void reset_core();
    void ensure_frame_size(unsigned width, unsigned height);
    void write_rgb565_frame(const uint16_t* pixels, unsigned width, unsigned height, std::size_t pitch);

    FrameOutput           m_frame;
    EmulatorInputState    m_input;
    std::vector<uint8_t>  m_rom_bytes;
    std::string           m_loaded_rom_path;
    std::string           m_backend_name;
    std::string           m_last_load_warning;
    bool                  m_core_initialized = false;
    bool                  m_game_loaded      = false;
    bool                  m_preview_mode     = false;
    bool                  m_preview_allow_audio = false;
    std::uint64_t         m_video_frame_count = 0;
    bool                  m_last_frame_had_visible_pixels = false;
    uint32_t              m_layer_capture_mask = 0x3u;
    double                m_frame_rate_hz = 59.82;
    // Queried by the core's own check_variables() ("pce_fast_frameskip") --
    // before retro_load_game() builds the machine, same load-time-only
    // pattern as the other cores' auto-frame-skip options. This core's
    // "auto" mode is genuinely adaptive (driven off audio-buffer occupancy
    // via RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, already wired
    // in handle_environment()), unlike mGBA's fixed skip-count option.
    bool                  m_auto_frame_skip = false;
};

} // namespace qrd
