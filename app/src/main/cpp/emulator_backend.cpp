#include "emulator_backend.h"
#include "psx_libretro_backend.h"
#include "fceux_backend.h"
#include "mame_backend.h"
#include "mgba_backend.h"
#include "pce_backend.h"
#include "picodrive_backend.h"
#include "saturn_libretro_backend.h"
#include "snes_libretro_backend.h"
#include "snes_backend_stub.h"

#include <algorithm>
#include <cctype>

namespace qrd {

namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool contains(const std::string& haystack_lower, const char* needle) {
    return haystack_lower.find(needle) != std::string::npos;
}

} // namespace

bool rom_is_snes_justifier_title(const std::string& rom_stem) {
    const std::string name = to_lower(rom_stem);
    return contains(name, "lethal enforcers") ||
           contains(name, "t2: the arcade game") || contains(name, "t2 the arcade game") ||
           contains(name, "terminator 2: the arcade game") ||
           contains(name, "terminator 2 the arcade game");
}

bool rom_is_lightgun_capable(BackendKind kind, const std::string& rom_stem) {
    const std::string name = to_lower(rom_stem);
    switch (kind) {
    case BackendKind::Snes:
        // Super Scope titles, plus Konami Justifier titles (a different SNES
        // lightgun peripheral -- see rom_is_snes_justifier_title() and
        // snes_gun_peripheral_id()).
        return contains(name, "super scope") || contains(name, "superscope") ||
               contains(name, "yoshi's safari") || contains(name, "yoshis safari") ||
               contains(name, "battle clash") ||
               contains(name, "tin star") ||
               contains(name, "metal combat") ||
               contains(name, "x-zone") || contains(name, "x zone") ||
               rom_is_snes_justifier_title(rom_stem);
    case BackendKind::Nes:
        // Zapper titles.
        return contains(name, "duck hunt") ||
               contains(name, "hogan's alley") || contains(name, "hogans alley") ||
               contains(name, "wild gunman") ||
               contains(name, "barker bill") ||
               contains(name, "to the earth") ||
               contains(name, "mechanized attack") ||
               contains(name, "freedom force") ||
               contains(name, "gumshoe");
    case BackendKind::Mame:
        // Saturn Virtua Gun/Stunner and MAME arcade gun games.
        return contains(name, "opwolf") || contains(name, "operation wolf") ||
               contains(name, "othunder") || contains(name, "operation thunderbolt") ||
               contains(name, "undrfire") || contains(name, "under fire") ||
               contains(name, "nycaptor") || contains(name, "n.y. captor") ||
               contains(name, "timecris") || contains(name, "time crisis") ||
               contains(name, "virtua cop") ||
               contains(name, "vcop") ||
               contains(name, "house of the dead") || contains(name, "hotd") ||
               contains(name, "die hard arcade") ||
               contains(name, "lethal enforcers") ||
               contains(name, "crypt killer") ||
               contains(name, "point blank") ||
               contains(name, "maximum force");
    case BackendKind::Saturn:
        // Saturn Virtua Gun/Stunner titles.
        return contains(name, "virtua cop") ||
               contains(name, "vcop") ||
               contains(name, "house of the dead") || contains(name, "hotd") ||
               contains(name, "die hard arcade") ||
               contains(name, "crypt killer");
    case BackendKind::Psx:
        // PlayStation Guncon/Justifier titles. The Controls panel still has a
        // manual override for abbreviated or otherwise unusual dump names.
        return contains(name, "time crisis") ||
               contains(name, "point blank") ||
               contains(name, "elemental gearbolt") ||
               contains(name, "area 51") ||
               contains(name, "project horned owl") ||
               contains(name, "ghoul panic") ||
               contains(name, "die hard trilogy") ||
               contains(name, "judge dredd") ||
               contains(name, "revolution x");
    default:
        return false;
    }
}

bool backend_supports_dual_gun(BackendKind kind, const std::string& rom_stem) {
    // A second gun is only worth offering where the core has a real second gun
    // device. Everything here is additionally gated on the ROM being a gun
    // title at all (rom_is_lightgun_capable, or the manual override that
    // stands in for it).
    switch (kind) {
    case BackendKind::Psx:      // second GunCon in port 1
    case BackendKind::Saturn:   // second Virtua Gun in port 1
    case BackendKind::Mame:     // MAME polls a lightgun per player port
        return true;
    case BackendKind::Snes:
        // Only the Justifier daisy-chains a second gun (snes9x's
        // "Justifier (2P)" device). The Super Scope is single-player.
        return rom_is_snes_justifier_title(rom_stem);
    default:
        return false;           // NES Zapper and friends: one gun only
    }
}

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
    case BackendKind::Mame:
        return std::make_unique<MameBackend>();
    case BackendKind::Saturn:
        return std::make_unique<SaturnLibretroBackend>();
    case BackendKind::Psx:
        return std::make_unique<PsxLibretroBackend>();
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
    case BackendKind::Mame:    return "MAME";
    case BackendKind::Saturn:  return "Saturn";
    case BackendKind::Psx:     return "PlayStation";
    default:                   return "Unknown";
    }
}

} // namespace qrd
