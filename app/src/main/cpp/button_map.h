#pragma once
// Per-game or global controller button mapping for QuestRetroDepth.
// Maps each libretro joypad slot to a Quest controller physical input.

#include <string>
#include <array>
#include <cstring>

#include "emulator_backend.h"

namespace qrd {

// Libretro joypad slots (SNES names kept for compatibility with existing code).
enum SnesButton : int {
    SNES_B      = 0,
    SNES_A      = 1,
    SNES_Y      = 2,
    SNES_X      = 3,
    SNES_L      = 4,
    SNES_R      = 5,
    SNES_START  = 6,
    SNES_SELECT = 7,
    SNES_UP     = 8,
    SNES_DOWN   = 9,
    SNES_LEFT   = 10,
    SNES_RIGHT  = 11,
    // Sega Saturn's pad has two extra face buttons beyond A/B/X/Y (C and Z);
    // every other backend leaves these two slots at QI_NONE.
    SATURN_C    = 12,
    SATURN_Z    = 13,
    SNES_BUTTON_COUNT = 14
};

inline const char* snes_button_name(int b) {
    switch (b) {
        case SNES_B:      return "B";
        case SNES_A:      return "A";
        case SNES_Y:      return "Y";
        case SNES_X:      return "X";
        case SNES_L:      return "L";
        case SNES_R:      return "R";
        case SNES_START:  return "Start";
        case SNES_SELECT: return "Select";
        case SNES_UP:     return "Up";
        case SNES_DOWN:   return "Down";
        case SNES_LEFT:   return "Left";
        case SNES_RIGHT:  return "Right";
        default:          return "?";
    }
}

inline const char* genesis_button_name(int b) {
    switch (b) {
        case SNES_B:      return "B";
        case SNES_A:      return "C";
        case SNES_Y:      return "A";
        case SNES_X:      return "Y";
        case SNES_L:      return "X";
        case SNES_R:      return "Z";
        case SNES_START:  return "Start";
        case SNES_SELECT: return "Mode";
        case SNES_UP:     return "Up";
        case SNES_DOWN:   return "Down";
        case SNES_LEFT:   return "Left";
        case SNES_RIGHT:  return "Right";
        default:          return "?";
    }
}

inline const char* gba_button_name(int b) {
    switch (b) {
        case SNES_B:      return "B";
        case SNES_A:      return "A";
        case SNES_Y:      return "Unused";
        case SNES_X:      return "Unused";
        case SNES_L:      return "L";
        case SNES_R:      return "R";
        case SNES_START:  return "Start";
        case SNES_SELECT: return "Select";
        case SNES_UP:     return "Up";
        case SNES_DOWN:   return "Down";
        case SNES_LEFT:   return "Left";
        case SNES_RIGHT:  return "Right";
        default:          return "?";
    }
}

inline const char* gb_button_name(int b) {
    switch (b) {
        case SNES_B:      return "B";
        case SNES_A:      return "A";
        case SNES_Y:      return "Unused";
        case SNES_X:      return "Unused";
        case SNES_L:      return "Unused";
        case SNES_R:      return "Unused";
        case SNES_START:  return "Start";
        case SNES_SELECT: return "Select";
        case SNES_UP:     return "Up";
        case SNES_DOWN:   return "Down";
        case SNES_LEFT:   return "Left";
        case SNES_RIGHT:  return "Right";
        default:          return "?";
    }
}

inline const char* saturn_button_name(int b) {
    // Slot -> EmulatorInputState field is fixed across all backends (see
    // openxr_shell.cpp's normal-mode input block); SaturnLibretroBackend
    // reads each field as the Saturn button of the same letter directly
    // (button_b == Saturn B, button_a == Saturn A, etc. -- see its
    // handle_input_state()), so these labels match the slot names as-is.
    switch (b) {
        case SNES_B:      return "B";
        case SNES_A:      return "A";
        case SNES_Y:      return "Y";
        case SNES_X:      return "X";
        case SNES_L:      return "L";
        case SNES_R:      return "R";
        case SNES_START:  return "Start";
        case SNES_SELECT: return "Unused";
        case SATURN_C:    return "C";
        case SATURN_Z:    return "Z";
        case SNES_UP:     return "Up";
        case SNES_DOWN:   return "Down";
        case SNES_LEFT:   return "Left";
        case SNES_RIGHT:  return "Right";
        default:          return "?";
    }
}

inline const char* psx_button_name(int b) {
    switch (b) {
        case SNES_B:      return "Cross";
        case SNES_A:      return "Circle";
        case SNES_Y:      return "Triangle";
        case SNES_X:      return "Square";
        case SNES_L:      return "L1";
        case SNES_R:      return "R1";
        case SNES_START:  return "Start";
        case SNES_SELECT: return "Select";
        case SATURN_C:    return "L2";
        case SATURN_Z:    return "R2";
        case SNES_UP:     return "Up";
        case SNES_DOWN:   return "Down";
        case SNES_LEFT:   return "Left";
        case SNES_RIGHT:  return "Right";
        default:          return "?";
    }
}

inline const char* button_name_for_backend(BackendKind kind, int b) {
    if (kind == BackendKind::Genesis) return genesis_button_name(b);
    if (kind == BackendKind::Gba)     return gba_button_name(b);
    if (kind == BackendKind::Gb)      return gb_button_name(b);
    if (kind == BackendKind::Saturn)  return saturn_button_name(b);
    if (kind == BackendKind::Psx)     return psx_button_name(b);
    return snes_button_name(b);
}

inline const char* button_map_title_for_backend(BackendKind kind) {
    if (kind == BackendKind::Genesis) return "Genesis Controller Map";
    if (kind == BackendKind::Gba)     return "GBA Controller Map";
    if (kind == BackendKind::Gb)      return "GB/GBC Controller Map";
    if (kind == BackendKind::Saturn)  return "Saturn Controller Map";
    if (kind == BackendKind::Psx)     return "PlayStation Controller Map";
    return "SNES Controller Map";
}

// Quest physical inputs that can be bound to a libretro joypad slot
enum QuestInput : int {
    QI_NONE        = 0,
    QI_A           = 1,   // right controller A
    QI_B           = 2,   // right controller B
    QI_X           = 3,   // left controller X
    QI_Y           = 4,   // left controller Y
    QI_RTRIG       = 5,   // right trigger
    QI_LTRIG       = 6,   // left trigger
    QI_RGRIP       = 7,   // right grip
    QI_LGRIP       = 8,   // left grip
    QI_RSTICK_UP   = 9,
    QI_RSTICK_DOWN = 10,
    QI_RSTICK_LEFT = 11,
    QI_RSTICK_RIGHT= 12,
    QI_LSTICK_UP   = 13,
    QI_LSTICK_DOWN = 14,
    QI_LSTICK_LEFT = 15,
    QI_LSTICK_RIGHT= 16,
    // Head tilt (Experimental > Motion Controls > D-Pad Headset). Latched from
    // HMD roll/pitch in poll_actions(); assignable like any other input, so a
    // remap of these follows the same path as remapping a stick direction.
    // Appended after QI_LSTICK_RIGHT so existing saved btn_map_* indices in
    // settings .ini files keep pointing at the same inputs.
    QI_HEAD_UP     = 17,
    QI_HEAD_DOWN   = 18,
    QI_HEAD_LEFT   = 19,
    QI_HEAD_RIGHT  = 20,
    // Air Wheel (Experimental > Motion Controls). Each motion is an
    // independently enabled gesture that latches like a button, so the
    // Controller Map decides what it means on this console -- steering might
    // be the d-pad on one game and the analogue stick on another.
    QI_WHEEL_LEFT      = 21,
    QI_WHEEL_RIGHT     = 22,
    QI_WHEEL_ACCEL     = 23,
    QI_WHEEL_BRAKE     = 24,
    QI_WHEEL_GEAR_UP   = 25,
    QI_WHEEL_HANDBRAKE = 26,
    QI_WHEEL_BIKE      = 27,
    // Appended rather than slotted next to GEAR_UP so the indices above keep
    // their meaning in already-saved btn_map_* entries.
    QI_WHEEL_GEAR_DOWN = 28,
    // Air Jump / Crouch: a sudden rise or drop of the headset.
    QI_AIR_JUMP        = 29,
    QI_AIR_CROUCH      = 30,
    // Air Fighter (Experimental > Motion Controls). These are the COMPONENTS a
    // recognised move plays back as a timed sequence -- a quarter-circle is
    // Down, then Down+Left, then Left, then Punch. Bound like every other
    // motion input, so the sequence lands on whatever this console calls those
    // directions and buttons.
    QI_FIGHT_UP        = 31,
    QI_FIGHT_DOWN      = 32,
    QI_FIGHT_LEFT      = 33,
    QI_FIGHT_RIGHT     = 34,
    QI_FIGHT_PUNCH     = 35,
    QI_FIGHT_KICK      = 36,
    // Normal attacks come in two strengths: a quick jab that snaps back, and a
    // heavy one where the arm is left extended. PUNCH/KICK above are the light
    // versions (also what the special moves press); these are the heavy ones.
    QI_FIGHT_PUNCH_HARD = 37,
    QI_FIGHT_KICK_HARD  = 38,
    QI_COUNT       = 39
};

inline const char* qi_name(int qi) {
    switch (qi) {
        case QI_NONE:         return "---";
        case QI_A:            return "A";
        case QI_B:            return "B";
        case QI_X:            return "X";
        case QI_Y:            return "Y";
        case QI_RTRIG:        return "R.Trig";
        case QI_LTRIG:        return "L.Trig";
        case QI_RGRIP:        return "R.Grip";
        case QI_LGRIP:        return "L.Grip";
        case QI_RSTICK_UP:    return "RS Up";
        case QI_RSTICK_DOWN:  return "RS Down";
        case QI_RSTICK_LEFT:  return "RS Left";
        case QI_RSTICK_RIGHT: return "RS Right";
        case QI_LSTICK_UP:    return "LS Up";
        case QI_LSTICK_DOWN:  return "LS Down";
        case QI_LSTICK_LEFT:  return "LS Left";
        case QI_LSTICK_RIGHT: return "LS Right";
        case QI_HEAD_UP:      return "Head Up";
        case QI_HEAD_DOWN:    return "Head Down";
        case QI_HEAD_LEFT:    return "Head Left";
        case QI_HEAD_RIGHT:   return "Head Right";
        case QI_WHEEL_LEFT:      return "Wheel L";
        case QI_WHEEL_RIGHT:     return "Wheel R";
        case QI_WHEEL_ACCEL:     return "Accel";
        case QI_WHEEL_BRAKE:     return "Brake";
        case QI_WHEEL_GEAR_UP:   return "Gear Up";
        case QI_WHEEL_GEAR_DOWN: return "Gear Dn";
        case QI_AIR_JUMP:        return "Air Jump";
        case QI_AIR_CROUCH:      return "Air Crouch";
        case QI_FIGHT_UP:        return "Fight Up";
        case QI_FIGHT_DOWN:      return "Fight Down";
        case QI_FIGHT_LEFT:      return "Fight Left";
        case QI_FIGHT_RIGHT:     return "Fight Right";
        case QI_FIGHT_PUNCH:     return "Fight Punch";
        case QI_FIGHT_KICK:      return "Fight Kick";
        case QI_FIGHT_PUNCH_HARD:return "Punch Hard";
        case QI_FIGHT_KICK_HARD: return "Kick Hard";
        case QI_WHEEL_HANDBRAKE: return "Handbrake";
        case QI_WHEEL_BIKE:      return "Bike Accel";
        default:              return "?";
    }
}

using ButtonMap = std::array<int, SNES_BUTTON_COUNT>;

// Default SNES mapping: user-customized layout
inline std::array<int, SNES_BUTTON_COUNT> default_button_map() {
    std::array<int, SNES_BUTTON_COUNT> m{};
    m[SNES_B]      = QI_A;
    m[SNES_A]      = QI_RGRIP;
    m[SNES_Y]      = QI_B;
    m[SNES_X]      = QI_LGRIP;
    m[SNES_L]      = QI_LTRIG;
    m[SNES_R]      = QI_RTRIG;
    m[SNES_START]  = QI_X;
    m[SNES_SELECT] = QI_Y;
    m[SNES_UP]     = QI_LSTICK_UP;
    m[SNES_DOWN]   = QI_LSTICK_DOWN;
    m[SNES_LEFT]   = QI_LSTICK_LEFT;
    m[SNES_RIGHT]  = QI_LSTICK_RIGHT;
    return m;
}

inline ButtonMap default_genesis_button_map() {
    ButtonMap m{};
    m[SNES_Y]      = QI_A;          // Genesis A
    m[SNES_B]      = QI_B;          // Genesis B
    m[SNES_A]      = QI_RGRIP;      // Genesis C
    m[SNES_L]      = QI_LTRIG;      // Genesis X
    m[SNES_X]      = QI_LGRIP;      // Genesis Y
    m[SNES_R]      = QI_RTRIG;      // Genesis Z
    m[SNES_START]  = QI_X;
    m[SNES_SELECT] = QI_Y;          // Genesis Mode
    m[SNES_UP]     = QI_LSTICK_UP;
    m[SNES_DOWN]   = QI_LSTICK_DOWN;
    m[SNES_LEFT]   = QI_LSTICK_LEFT;
    m[SNES_RIGHT]  = QI_LSTICK_RIGHT;
    return m;
}

inline ButtonMap default_gba_button_map() {
    ButtonMap m{};
    m[SNES_B]      = QI_B;
    m[SNES_A]      = QI_A;
    m[SNES_Y]      = QI_NONE;
    m[SNES_X]      = QI_NONE;
    m[SNES_L]      = QI_LTRIG;
    m[SNES_R]      = QI_RTRIG;
    m[SNES_START]  = QI_X;
    m[SNES_SELECT] = QI_Y;
    m[SNES_UP]     = QI_LSTICK_UP;
    m[SNES_DOWN]   = QI_LSTICK_DOWN;
    m[SNES_LEFT]   = QI_LSTICK_LEFT;
    m[SNES_RIGHT]  = QI_LSTICK_RIGHT;
    return m;
}

inline ButtonMap default_gb_button_map() {
    ButtonMap m{};
    m[SNES_B]      = QI_B;
    m[SNES_A]      = QI_A;
    m[SNES_Y]      = QI_NONE;
    m[SNES_X]      = QI_NONE;
    m[SNES_L]      = QI_NONE;
    m[SNES_R]      = QI_NONE;
    m[SNES_START]  = QI_X;
    m[SNES_SELECT] = QI_Y;
    m[SNES_UP]     = QI_LSTICK_UP;
    m[SNES_DOWN]   = QI_LSTICK_DOWN;
    m[SNES_LEFT]   = QI_LSTICK_LEFT;
    m[SNES_RIGHT]  = QI_LSTICK_RIGHT;
    return m;
}

inline ButtonMap default_saturn_button_map() {
    // Saturn needs 9 mappable functions (A/B/C/X/Y/Z/L/R/Start) but the Quest
    // controllers only offer 8 non-stick physical inputs (A/B/X/Y/L+R
    // trigger/L+R grip); Z is left unmapped by default as the least commonly
    // used Saturn button -- remap it in the Controller Map panel if a game
    // needs it. Start goes on the X physical button (left grip was unreliable
    // -- easy to squeeze past the world-locomotion system's activation delay
    // without meaning to); Saturn X moves to left grip in its place.
    ButtonMap m{};
    m[SNES_A]      = QI_A;      // Saturn A
    m[SNES_B]      = QI_B;      // Saturn B
    m[SATURN_C]    = QI_RTRIG;  // Saturn C
    m[SNES_X]      = QI_LGRIP;  // Saturn X
    m[SNES_Y]      = QI_Y;      // Saturn Y
    m[SATURN_Z]    = QI_NONE;   // Saturn Z (unmapped by default)
    m[SNES_L]      = QI_LTRIG;  // Saturn L
    m[SNES_R]      = QI_RGRIP;  // Saturn R
    m[SNES_START]  = QI_X;
    m[SNES_SELECT] = QI_NONE;   // Saturn has no Select
    m[SNES_UP]     = QI_LSTICK_UP;
    m[SNES_DOWN]   = QI_LSTICK_DOWN;
    m[SNES_LEFT]   = QI_LSTICK_LEFT;
    m[SNES_RIGHT]  = QI_LSTICK_RIGHT;
    return m;
}

inline ButtonMap default_psx_button_map() {
    ButtonMap m{};
    m[SNES_B]      = QI_B;      // Cross
    m[SNES_A]      = QI_A;      // Circle
    m[SNES_Y]      = QI_Y;      // Triangle
    m[SNES_X]      = QI_X;      // Square
    m[SNES_L]      = QI_LGRIP;  // L1
    m[SNES_R]      = QI_RGRIP;  // R1
    m[SNES_START]  = QI_X;
    m[SNES_SELECT] = QI_NONE;
    m[SATURN_C]    = QI_LTRIG;  // L2
    m[SATURN_Z]    = QI_RTRIG;  // R2
    m[SNES_UP]     = QI_LSTICK_UP;
    m[SNES_DOWN]   = QI_LSTICK_DOWN;
    m[SNES_LEFT]   = QI_LSTICK_LEFT;
    m[SNES_RIGHT]  = QI_LSTICK_RIGHT;
    return m;
}

inline ButtonMap default_button_map_for_backend(BackendKind kind) {
    if (kind == BackendKind::Genesis) return default_genesis_button_map();
    if (kind == BackendKind::Gba)     return default_gba_button_map();
    if (kind == BackendKind::Gb)      return default_gb_button_map();
    if (kind == BackendKind::Saturn)  return default_saturn_button_map();
    if (kind == BackendKind::Psx)     return default_psx_button_map();
    return default_button_map();
}

} // namespace qrd
