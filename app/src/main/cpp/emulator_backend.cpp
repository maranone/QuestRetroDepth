#include "emulator_backend.h"
#include "picodrive_backend.h"
#include "snes_libretro_backend.h"
#include "snes_backend_stub.h"

namespace qrd {

std::unique_ptr<EmulatorBackend> create_backend(BackendKind kind) {
    switch (kind) {
    case BackendKind::Snes:
        return std::make_unique<SnesLibretroBackend>();
    case BackendKind::Genesis:
        return std::make_unique<PicoDriveBackend>();
    default:
        return {};
    }
}

const char* backend_kind_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::Snes:
        return "SNES";
    case BackendKind::Genesis:
        return "Genesis";
    default:
        return "Unknown";
    }
}

} // namespace qrd
