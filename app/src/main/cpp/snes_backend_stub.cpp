#include "snes_backend_stub.h"

#include <algorithm>

namespace qrd {

SnesBackendStub::SnesBackendStub() {
    rebuild_placeholder_frame();
}

const char* SnesBackendStub::backend_name() const {
    return "SNES stub backend";
}

bool SnesBackendStub::load_content(const std::string& rom_path, std::string& error_out) {
    if (rom_path.empty()) {
        error_out = "SNES stub: ROM path is empty.";
        return false;
    }

    m_loaded_rom_path = rom_path;
    m_frame_counter = 0;
    rebuild_placeholder_frame();
    error_out.clear();
    return true;
}

bool SnesBackendStub::step_frame(const EmulatorInputState& input, std::string& error_out) {
    if (m_loaded_rom_path.empty()) {
        error_out = "SNES stub: no ROM has been loaded.";
        return false;
    }

    ++m_frame_counter;
    rebuild_placeholder_frame();

    // Placeholder "reactive" tint so the shell can be wired before the real core lands.
    if (!m_frame.rgba8888.empty()) {
        uint32_t accent = 0xFF2B7A78u;
        if (input.button_a) accent = 0xFFEF476Fu;
        else if (input.button_b) accent = 0xFFFFD166u;
        else if (input.button_x) accent = 0xFF06D6A0u;
        else if (input.button_y) accent = 0xFF118AB2u;
        std::fill(m_frame.rgba8888.begin(), m_frame.rgba8888.begin() + std::min<size_t>(m_frame.rgba8888.size(), 160u), accent);
    }

    error_out.clear();
    return true;
}

const FrameOutput& SnesBackendStub::frame_output() const {
    return m_frame;
}

bool SnesBackendStub::save_state(std::vector<uint8_t>& out, std::string& error_out) {
    if (m_loaded_rom_path.empty()) {
        error_out = "SNES stub: no ROM has been loaded.";
        return false;
    }
    out.assign(16, 0);
    error_out.clear();
    return true;
}

bool SnesBackendStub::load_state(const void* data, std::size_t size, std::string& error_out) {
    if (m_loaded_rom_path.empty()) {
        error_out = "SNES stub: no ROM has been loaded.";
        return false;
    }
    if (!data || size == 0) {
        error_out = "SNES stub: savestate data is empty.";
        return false;
    }
    error_out.clear();
    return true;
}

RomHeaderInfo SnesBackendStub::get_rom_header_info() const {
    RomHeaderInfo info;
    return info;
}

void SnesBackendStub::rebuild_placeholder_frame() {
    m_frame.width = 256;
    m_frame.height = 224;
    m_frame.rgba8888.assign(static_cast<size_t>(m_frame.width) * m_frame.height, 0xFF101820u);

    for (uint32_t y = 0; y < m_frame.height; ++y) {
        for (uint32_t x = 0; x < m_frame.width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(m_frame.width - 1);
            const float fy = static_cast<float>(y) / static_cast<float>(m_frame.height - 1);

            uint8_t r = static_cast<uint8_t>(24 + 28 * fx);
            uint8_t g = static_cast<uint8_t>(36 + 92 * fy);
            uint8_t b = static_cast<uint8_t>(58 + 132 * fx);

            if (y > 144) {
                r = static_cast<uint8_t>(30 + 24 * fx);
                g = static_cast<uint8_t>(90 + 65 * fy);
                b = static_cast<uint8_t>(20 + 30 * fx);
            }

            if (((x / 16) + (y / 16) + (m_frame_counter / 20)) % 7 == 0) {
                r = static_cast<uint8_t>(std::min<int>(255, r + 26));
                g = static_cast<uint8_t>(std::min<int>(255, g + 18));
            }

            m_frame.rgba8888[static_cast<size_t>(y) * m_frame.width + x] =
                0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                (static_cast<uint32_t>(g) << 8) | b;
        }
    }
}

} // namespace qrd
