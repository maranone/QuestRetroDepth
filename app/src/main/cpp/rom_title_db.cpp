#include "rom_title_db.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#define LOG_TAG "RomTitleDb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace qrd {
namespace {

std::unordered_map<std::string, std::string> g_titles;
std::mutex g_mutex;
bool g_initialized = false;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string database_key(std::string value) {
    const auto slash = value.find_last_of("/\\");
    if (slash != std::string::npos) value = value.substr(slash + 1);
    const auto dot = value.find_last_of('.');
    if (dot != std::string::npos) value.resize(dot);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return trim(std::move(value));
}

} // namespace

void initialize_rom_title_database(AAssetManager* asset_manager) {
    if (!asset_manager) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;
    g_initialized = true;

    // Load the broad MAME database first, then the curated Neo Geo file so
    // existing Neo Geo titles remain the preferred display names.
    constexpr const char* kDatabaseAssets[] = {
        "game_titles/mame.txt",
        "game_titles/neogeo.txt",
    };
    size_t loaded_records = 0;
    for (const char* asset_name : kDatabaseAssets) {
        AAsset* asset = AAssetManager_open(asset_manager, asset_name,
                                           AASSET_MODE_BUFFER);
        if (!asset) {
            LOGE("Could not open bundled title database: %s", asset_name);
            continue;
        }

        const off_t length = AAsset_getLength(asset);
        std::string contents;
        if (length > 0) {
            contents.resize(static_cast<size_t>(length));
            const int read = AAsset_read(asset, contents.data(), contents.size());
            if (read < 0) contents.clear();
            else contents.resize(static_cast<size_t>(read));
        }
        AAsset_close(asset);
        if (contents.empty()) {
            LOGE("Bundled title database is empty: %s", asset_name);
            continue;
        }

        std::istringstream lines(contents);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(std::move(line));
            if (line.empty() || line[0] == '#') continue;
            const auto separator = line.find('|');
            if (separator == std::string::npos) continue;
            const std::string key = database_key(line.substr(0, separator));
            const std::string title = trim(line.substr(separator + 1));
            if (!key.empty() && !title.empty()) {
                g_titles[key] = title;
                ++loaded_records;
            }
        }
    }
    LOGI("Loaded %zu bundled title records (%zu unique)", loaded_records,
         g_titles.size());
}

std::string rom_display_name(const std::string& rom_name) {
    const std::string key = database_key(rom_name);
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_titles.find(key);
    return it == g_titles.end() ? rom_name : it->second;
}

} // namespace qrd
