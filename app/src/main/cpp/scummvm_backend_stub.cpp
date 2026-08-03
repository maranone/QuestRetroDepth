// Compiled only when third_party/scummvm_libretro is not present.
// Satisfies the ScummVmBackend symbol so the rest of the project links.
#include "scummvm_backend.h"

namespace qrd {

ScummVmBackend::ScummVmBackend()  = default;
ScummVmBackend::~ScummVmBackend() = default;

const char* ScummVmBackend::backend_name() const { return "ScummVM (not built)"; }
double      ScummVmBackend::frame_rate_hz()  const { return 60.0; }

bool ScummVmBackend::load_content(const std::string&, std::string& err) {
    err = "ScummVM backend not built: add third_party/scummvm_libretro submodule.";
    return false;
}
bool ScummVmBackend::step_frame(const EmulatorInputState&, std::string& err) {
    err = "ScummVM backend not built.";
    return false;
}
const FrameOutput& ScummVmBackend::frame_output() const { return m_frame; }

bool ScummVmBackend::save_state(std::vector<uint8_t>&, std::string& err) {
    err = "ScummVM backend not built.";
    return false;
}
bool ScummVmBackend::load_state(const void*, std::size_t, std::string& err) {
    err = "ScummVM backend not built.";
    return false;
}

void      ScummVmBackend::set_auto_frame_skip(bool) {}
void      ScummVmBackend::set_layer_capture_mask(uint32_t) {}
RomHeaderInfo ScummVmBackend::get_rom_header_info() const { return {}; }
const uint32_t* ScummVmBackend::get_z_histogram()   const { return nullptr; }
const uint8_t*  ScummVmBackend::system_ram_data()   const { return nullptr; }
std::size_t     ScummVmBackend::system_ram_size()    const { return 0; }

bool    ScummVmBackend::handle_environment(unsigned, void*)                       { return false; }
void    ScummVmBackend::handle_video_frame(const void*, unsigned, unsigned, std::size_t) {}
int16_t ScummVmBackend::handle_input_state(unsigned, unsigned, unsigned, unsigned) const { return 0; }

} // namespace qrd
