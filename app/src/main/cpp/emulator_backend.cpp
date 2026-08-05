#include "emulator_backend.h"
#include "fceux_backend.h"
#include "mgba_backend.h"
#include "pce_backend.h"
#include "picodrive_backend.h"
#include "snes_libretro_backend.h"
#include "snes_backend_stub.h"

namespace qrd {

std::unique_ptr<EmulatorBackend> create_backend(BackendKind kind) {
    switch (kind) {
    case BackendKind::Snes:
        return std::make_unique<SnesLibretroBackend>();
    case BackendKind::Genesis:
    case BackendKind::Sms:
        return std::make_unique<PicoDriveBackend>();
    case BackendKind::Gba:
    case BackendKind::Gb:
        return std::make_unique<MgbaBackend>();
    case BackendKind::Nes:
        return std::make_unique<FceuxBackend>();
    case BackendKind::Pce:
        return std::make_unique<PceBackend>();
    default:
        return {};
    }
}

const char* backend_kind_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::Snes:    return "SNES";
    case BackendKind::Genesis: return "Genesis";
    case BackendKind::Gba:     return "GBA";
    case BackendKind::Gb:      return "GB/GBC";
    case BackendKind::Nes:     return "NES";
    case BackendKind::Pce:     return "PCE";
    case BackendKind::Sms:     return "SMS/GG";
    default:                   return "Unknown";
    }
}

} // namespace qrd
