#pragma once

#include <string>

struct AAssetManager;

namespace qrd {

// Loads the bundled MAME and curated Neo Geo shortname -> friendly title
// databases. Safe to call once the Android AssetManager is available; later
// lookups are read-only.
void initialize_rom_title_database(AAssetManager* asset_manager);

// Returns the database title for a ROM filename/stem, or the original value
// when the database has no entry. Paths and archive extensions are accepted.
std::string rom_display_name(const std::string& rom_name);

} // namespace qrd
