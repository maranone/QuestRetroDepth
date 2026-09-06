#pragma once

#include <string_view>

struct AAssetManager;

namespace qrd {

// Capture-capability classification. A shared MAME video device can still
// require a different screen-composition adapter, so this is intentionally
// separate from the hardware name.
enum class MameLayerProfile {
    Cps,
    Konami,
    Sega16B,
    Dec0,
    Gp9001,
    NeoGeo,
    Saturn,
    Taito,
    Namco,
    KonamiLethal,
    TaitoTc0100,
    TaitoTc0480,
    Unico,
    Oneshot,
    Lordgun,
    Seta2,
    Segaybd,
    Bbusters,
    Nycaptor,
    FullFrame,
};

struct MameDriverClassification {
    MameLayerProfile profile = MameLayerProfile::FullFrame;
    const char* family = "unclassified";
    bool true_capture_available = false;
};

MameDriverClassification classify_mame_driver(std::string_view shortname);
const char* mame_layer_profile_name(MameLayerProfile profile);

// Loads the generated shortname -> MAME source/profile map from APK assets.
// Static verified-name matching remains as a fallback for compatibility.
void initialize_mame_driver_database(AAssetManager* asset_manager);

} // namespace qrd
