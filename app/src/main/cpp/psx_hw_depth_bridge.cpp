#include "psx_hw_depth_bridge.h"

namespace qrd {
namespace {

// Emulation-thread only: published during retro_run() and drained in the video
// callback, which the core invokes from inside the same retro_run().
PsxHwDepthInfo g_info;

} // namespace

void psx_hw_depth_publish(const PsxHwDepthInfo& info) { g_info = info; }

PsxHwDepthInfo psx_hw_depth_take() {
    PsxHwDepthInfo out = g_info;
    g_info.valid = false;
    return out;
}

} // namespace qrd
