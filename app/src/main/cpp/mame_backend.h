#pragma once

#include "emulator_backend.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrd {

// Sets the libretro "system directory" MAME reports its BIOS/rompath search
// from (MAME's own Set_Path_Option() then auto-searches
// <dir>/mame/bios and <dir>/mame/roms -- see mame_backend.cpp's comment on
// ensure_core_initialized() for the full 3-location BIOS search story).
// Call once at app/activity startup, mirroring set_mgba_frontend_directories().
void set_mame_system_directory(const std::string& dir);


// The same directory set_mame_system_directory() was last called with (i.e.
// questretrodepth's per-app root dir on the sdcard). Used by
// neogeo_palette_debug.cpp to place its output alongside mame/bios and
// mame/roms. Empty if never set.
const std::string& mame_system_directory();

// Wraps the prebuilt mame_libretro_android.so (third_party/mame_libretro,
// built via its own Makefile.libretro -- see the build notes in the plan
// this was implemented from) through the standard, unprefixed libretro API
// (retro_init/retro_run/retro_load_game/...). Unlike the other backends in
// this project, MAME isn't compiled as a static lib into questretrodepth's
// own .so -- it's linked against as a separate prebuilt shared library
// (CMakeLists.txt IMPORTED SHARED target), so there's no symbol-prefixing
// concern the way pce_retro_*/mgba_retro_* etc need.
//
// Per-layer pixel data comes from mame_layer_capture.h's mame_layer_*
// accessors (exported by the verified driver/device hooks in the MAME source)
// and is matched by name into a fixed index order. Drivers without a verified
// source hook are copied into the final "full_frame" slot by this backend so
// they remain visible without being mislabeled as CPS layers.
class MameBackend final : public EmulatorBackend {
public:
    MameBackend();
    ~MameBackend() override;

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
    void set_occupancy_capture_enabled(bool enabled) override;
    // MAME polls a lightgun for every player port (process_lightgun_state in
    // the core's input_retro.cpp), so player two's gun needs no port setup at
    // all -- this only decides whether port 1 answers with a live gun.
    void set_dual_gun_mode(bool enabled) override { m_dual_gun_mode = enabled; }
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
    void pull_named_layers();
    void pull_synthesized_zbuffer();
    void pull_occupancy_layers();

    FrameOutput           m_frame;
    EmulatorInputState    m_input;
    std::vector<uint8_t>  m_rom_bytes;
    std::string           m_loaded_rom_path;
    std::string           m_backend_name;
    std::string           m_last_load_warning;
    std::string           m_system_dir;
    bool                  m_core_initialized = false;
    bool                  m_game_loaded      = false;
    bool                  m_dual_gun_mode    = false;
    // Latched before retro_load_game(); MAME's video manager reads the
    // autoframeskip option while constructing the machine.
    bool                  m_auto_frame_skip = false;
    bool                  m_preview_mode     = false;
    bool                  m_preview_allow_audio = false;
    std::uint64_t         m_video_frame_count = 0;
    uint32_t              m_last_mame_frame_id = 0xFFFFFFFFu;
    uint32_t              m_mame_orientation = 0;
    int                   m_mame_one_layer_streak = 0;
    double                m_frame_rate_hz = 59.63; // CPS1/CPS2 default; overwritten from av_info
};

} // namespace qrd
