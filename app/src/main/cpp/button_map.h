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
    SNES_BUTTON_COUNT = 12
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

inline const char* button_name_for_backend(BackendKind kind, int b) {
    return kind == BackendKind::Genesis ? genesis_button_name(b) : snes_button_name(b);
}

inline const char* button_map_title_for_backend(BackendKind kind) {
    return kind == BackendKind::Genesis ? "Genesis Controller Map" : "SNES Controller Map";
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
    QI_COUNT       = 17
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

inline ButtonMap default_button_map_for_backend(BackendKind kind) {
    return kind == BackendKind::Genesis ? default_genesis_button_map() : default_button_map();
}

} // namespace qrd
