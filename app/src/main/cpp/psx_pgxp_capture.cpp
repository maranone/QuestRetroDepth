#include "psx_pgxp_capture.h"

#include <utility>

namespace qrd {
namespace {

bool g_capture_enabled = false;
std::vector<PsxPgxpCapturePrimitive> g_capture_primitives;
PsxPgxpCaptureDisplay g_capture_display;

} // namespace

void psx_pgxp_capture_set_enabled(bool enabled) {
    g_capture_enabled = enabled;
    if (!enabled) {
        g_capture_primitives.clear();
        g_capture_display = PsxPgxpCaptureDisplay{};
    }
}

bool psx_pgxp_capture_enabled() {
    return g_capture_enabled;
}

void psx_pgxp_capture_reset() {
    if (g_capture_enabled) g_capture_primitives.clear();
}

void psx_pgxp_capture_emit(const PsxPgxpCapturePrimitive& primitive) {
    if (g_capture_enabled) g_capture_primitives.push_back(primitive);
}

void psx_pgxp_capture_set_display(uint16_t vram_left, uint16_t vram_top,
                                  uint16_t width, uint16_t height) {
    if (!g_capture_enabled) return;
    g_capture_display.vram_left = vram_left;
    g_capture_display.vram_top = vram_top;
    g_capture_display.width = width;
    g_capture_display.height = height;
    g_capture_display.valid = (width > 0 && height > 0);
}

void psx_pgxp_capture_take(std::vector<PsxPgxpCapturePrimitive>& out,
                           PsxPgxpCaptureDisplay& display_out) {
    out = std::move(g_capture_primitives);
    g_capture_primitives.clear();
    display_out = g_capture_display;
}

} // namespace qrd
