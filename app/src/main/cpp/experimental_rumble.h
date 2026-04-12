#pragma once

#include <android/asset_manager.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "emulator_backend.h"

namespace qrd {

enum class RumbleWavePattern {
    Single,
    Both,
};

enum class RumbleEffect {
    Normal,
    FadeIn,
    FadeOut,
    FadeInOut,
};

struct QueuedHapticEvent {
    bool right = false;
    float amplitude = 0.0f;
    int duration_ms = 0;
    int delay_ms = 0;
    uint64_t due_time_ms = 0;
    RumbleWavePattern pattern = RumbleWavePattern::Single;
    RumbleEffect effect = RumbleEffect::Normal;
    int start_ms = 0;
};

class ExperimentalRumbleManager {
public:
    bool load_catalog(AAssetManager* asset_manager, std::string& error_out);
    void set_user_root(const std::string& user_root);
    void set_enabled(bool enabled);
    bool enabled() const { return m_enabled; }

    void reset_runtime();
    void on_rom_loaded(AAssetManager* asset_manager,
                       BackendKind backend_kind,
                       const std::string& rom_filename,
                       const std::string& game_name);
    std::string active_status() const;

    std::vector<QueuedHapticEvent> evaluate_frame(const EmulatorBackend& backend);

private:
    struct HapticSpec {
        bool right = false;
        float amplitude = 0.0f;
        int duration_ms = 0;
        int start_ms = 0;
        RumbleEffect effect = RumbleEffect::Normal;
    };

    struct Trigger {
        std::string id;
        std::string event;
        std::string condition;
        std::size_t offset = 0;
        int size = 1;
        std::string value_type;
        std::string byte_order;
        int priority = 0;
        int cooldown_frames = 0;
        uint64_t last_fire_frame = 0;
        bool has_prev = false;
        int64_t prev_value = 0;
        bool has_compare_value = false;
        int64_t compare_value = 0;
        std::vector<HapticSpec> haptics;
    };

    struct Profile {
        BackendKind backend_kind = BackendKind::Snes;
        std::string game_name;
        std::string game_key;
        std::vector<std::string> aliases;
        std::vector<Trigger> triggers;
    };

    struct CatalogEntry {
        BackendKind backend_kind = BackendKind::Snes;
        std::string game_key;
        std::string asset_path;
        std::vector<std::string> aliases;
        int trigger_count = 0;
    };

    static std::string normalize_name(const std::string& value);
    static std::string trim(std::string text);
    static bool read_asset_text(AAssetManager* asset_manager, const std::string& asset_path, std::string& text_out);
    static bool read_file_text(const std::string& path, std::string& text_out);
    static BackendKind backend_kind_from_string(const std::string& value);
    static int event_priority(const std::string& event);
    static int event_cooldown_frames(const std::string& event);
    static int cooldown_ms_to_frames(int cooldown_ms);
    static int64_t decode_value(const uint8_t* ptr, int size, const std::string& value_type, const std::string& byte_order);
    static bool condition_matches(const Trigger& trigger, int64_t prev, int64_t curr);

    bool load_profile(AAssetManager* asset_manager, const CatalogEntry& entry, Profile& profile_out);
    bool load_user_profile(BackendKind backend_kind,
                           const std::string& path,
                           Profile& profile_out,
                           std::string& error_out);
    bool find_matching_user_profile(BackendKind backend_kind,
                                    const std::string& normalized_rom,
                                    const std::string& normalized_header,
                                    Profile& profile_out,
                                    std::string& error_out);

    bool m_enabled = true;
    uint64_t m_frame_counter = 0;
    std::vector<CatalogEntry> m_catalog;
    bool m_catalog_loaded = false;
    bool m_has_active_profile = false;
    std::string m_user_root;
    std::string m_active_status = "ON";
    Profile m_active_profile;
};

} // namespace qrd
