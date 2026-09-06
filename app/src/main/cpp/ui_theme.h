#pragma once

#include <algorithm>

namespace qrd {

enum class UiThemeId : int {
    Classic = 0,
    PremiumRetroTech = 1,
    Glass = 2,
    Arcade = 3,
    Forest = 4,
    Sunset = 5,
    Mono = 6,
    Royal = 7,
};

constexpr int kUiThemeCount = 8;

inline UiThemeId clamp_ui_theme(int value) {
    return static_cast<UiThemeId>(std::clamp(value, 0, kUiThemeCount - 1));
}

inline const char* ui_theme_name(UiThemeId theme) {
    switch (theme) {
    case UiThemeId::PremiumRetroTech: return "PREMIUM";
    case UiThemeId::Glass: return "GLASS";
    case UiThemeId::Arcade: return "ARCADE";
    case UiThemeId::Forest: return "FOREST";
    case UiThemeId::Sunset: return "SUNSET";
    case UiThemeId::Mono: return "MONO";
    case UiThemeId::Royal: return "ROYAL";
    case UiThemeId::Classic:
    default: return "CLASSIC";
    }
}

} // namespace qrd
