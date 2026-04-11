#include "experimental_rumble.h"

#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/stat.h>

#define RUMBLE_LOG_TAG "QuestRetroDepthRumble"
#define RLOGI(...) __android_log_print(ANDROID_LOG_INFO, RUMBLE_LOG_TAG, __VA_ARGS__)

namespace qrd {

namespace {

constexpr float k_runtime_amplitude_scale = 2.0f;
constexpr int k_runtime_duration_scale = 2;

std::vector<std::string> split(const std::string& text, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, delim)) out.push_back(part);
    return out;
}

std::vector<std::string> parse_csv_fields(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
            continue;
        }
        if (c == ',' && !in_quotes) {
            out.push_back(field);
            field.clear();
            continue;
        }
        field.push_back(c);
    }
    out.push_back(field);
    return out;
}

bool ends_with_csv(const std::string& value) {
    return value.size() >= 4 &&
           value.compare(value.size() - 4, 4, ".csv") == 0;
}

int64_t parse_int_auto(const std::string& value, bool& ok_out) {
    std::string trimmed = value;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    if (trimmed.empty()) {
        ok_out = false;
        return 0;
    }
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(trimmed.c_str(), &end, 0);
    ok_out = (errno == 0 && end && *end == '\0');
    return ok_out ? static_cast<int64_t>(parsed) : 0;
}

float effect_scale(RumbleEffect effect, int step_index, int total_steps) {
    if (total_steps <= 1) return 1.0f;
    const float t = static_cast<float>(step_index) / static_cast<float>(total_steps - 1);
    switch (effect) {
        case RumbleEffect::FadeIn: return 0.45f + 0.55f * t;
        case RumbleEffect::FadeOut: return 1.0f - 0.55f * t;
        case RumbleEffect::FadeInOut:
            return step_index == 0 || step_index == total_steps - 1 ? 0.55f : 1.0f;
        case RumbleEffect::Normal:
        default:
            return 1.0f;
    }
}

} // namespace

std::string ExperimentalRumbleManager::trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

bool ExperimentalRumbleManager::read_asset_text(AAssetManager* asset_manager, const std::string& asset_path, std::string& text_out) {
    if (!asset_manager) return false;
    AAsset* asset = AAssetManager_open(asset_manager, asset_path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return false;
    const auto length = AAsset_getLength(asset);
    text_out.resize(static_cast<std::size_t>(length));
    if (length > 0) {
        const int read = AAsset_read(asset, text_out.data(), length);
        if (read != length) {
            AAsset_close(asset);
            return false;
        }
    }
    AAsset_close(asset);
    return true;
}

bool ExperimentalRumbleManager::read_file_text(const std::string& path, std::string& text_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text_out = ss.str();
    return true;
}

BackendKind ExperimentalRumbleManager::backend_kind_from_string(const std::string& value) {
    return value == "genesis" ? BackendKind::Genesis : BackendKind::Snes;
}

std::string ExperimentalRumbleManager::normalize_name(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool in_paren = false;
    bool in_bracket = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '(') { in_paren = true; continue; }
        if (c == ')') { in_paren = false; continue; }
        if (c == '[') { in_bracket = true; continue; }
        if (c == ']') { in_bracket = false; continue; }
        if (in_paren || in_bracket) continue;
        out.push_back(c);
    }
    std::string lowered;
    lowered.reserve(out.size());
    for (char c : out) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    lowered = std::regex_replace(lowered, std::regex("\\.(sfc|smc|fig|bin|md|gen|smd|zip|7z|csv)$"), " ");
    lowered = std::regex_replace(lowered, std::regex("\\b(rev(ision)?|version|ver|proto|beta|demo)\\s*[a-z0-9._-]*\\b"), " ");
    lowered = std::regex_replace(lowered, std::regex("[-_]+"), " ");
    lowered = std::regex_replace(lowered, std::regex("\\b(usa|europe|eur|japan|jpn|es|esp|en|fr|de|it|proto|beta|demo|rev|revision|v[0-9]+(?:\\.[0-9]+)*)\\b"), " ");
    lowered = std::regex_replace(lowered, std::regex("[^a-z0-9]+"), "");
    return lowered;
}

int ExperimentalRumbleManager::event_priority(const std::string& event) {
    if (event == "game_over") return 100;
    if (event == "death") return 90;
    if (event == "life_lost") return 80;
    if (event == "damage_taken") return 70;
    if (event == "level_complete") return 60;
    if (event == "life_gained") return 50;
    if (event == "powerup_gained") return 40;
    if (event == "enemy_defeated") return 30;
    if (event == "pickup") return 20;
    if (event == "score") return 10;
    return 0;
}

int ExperimentalRumbleManager::event_cooldown_frames(const std::string& event) {
    if (event == "pickup") return 2;
    if (event == "score") return 3;
    if (event == "powerup_gained") return 4;
    if (event == "enemy_defeated") return 3;
    if (event == "damage_taken") return 6;
    if (event == "life_lost" || event == "life_gained") return 10;
    if (event == "death") return 20;
    if (event == "level_complete" || event == "game_over") return 30;
    return 4;
}

int ExperimentalRumbleManager::cooldown_ms_to_frames(int cooldown_ms) {
    if (cooldown_ms <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(cooldown_ms) / 16.6667)));
}

int64_t ExperimentalRumbleManager::decode_value(const uint8_t* ptr, int size, const std::string& value_type, const std::string& byte_order) {
    if (!ptr || size <= 0) return 0;
    std::vector<uint8_t> bytes(ptr, ptr + size);
    if (byte_order == "little") {
        std::reverse(bytes.begin(), bytes.end());
    }

    if (value_type == "bcd") {
        int64_t value = 0;
        for (uint8_t b : bytes) value = value * 100 + ((b >> 4) & 0xF) * 10 + (b & 0xF);
        return value;
    }
    if (value_type == "nibble") {
        int64_t value = 0;
        for (uint8_t b : bytes) value = value * 100 + ((b >> 4) & 0xF) * 10 + (b & 0xF);
        return value;
    }

    uint64_t raw = 0;
    for (uint8_t b : bytes) raw = (raw << 8) | b;
    if (value_type == "int") {
        const int bits = size * 8;
        const uint64_t sign = 1ULL << (bits - 1);
        if (raw & sign) {
            const uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
            return -static_cast<int64_t>((~raw + 1) & mask);
        }
    }
    return static_cast<int64_t>(raw);
}

bool ExperimentalRumbleManager::condition_matches(const Trigger& trigger, int64_t prev, int64_t curr) {
    const std::string& condition = trigger.condition;
    if (condition == "prev > curr" || condition == "prev_gt_curr") return prev > curr;
    if (condition == "prev < curr" || condition == "prev_lt_curr") return prev < curr;
    if (condition == "prev != curr" || condition == "changed") return prev != curr;
    if (condition == "prev > 0 && curr == 0" || condition == "became_zero") return prev > 0 && curr == 0;
    if (condition == "prev == 0 && curr != 0" || condition == "became_nonzero") return prev == 0 && curr != 0;
    if (condition == "prev == 0 && curr > 0") return prev == 0 && curr > 0;
    if (condition == "equals") return trigger.has_compare_value && curr == trigger.compare_value;
    if (condition == "not_equals") return trigger.has_compare_value && curr != trigger.compare_value;
    return false;
}

bool ExperimentalRumbleManager::load_catalog(AAssetManager* asset_manager, std::string& error_out) {
    std::string text;
    if (!read_asset_text(asset_manager, "rumble/catalog.tsv", text)) {
        error_out = "Missing rumble/catalog.tsv asset.";
        RLOGI("catalog missing: rumble/catalog.tsv");
        return false;
    }
    m_catalog.clear();
    std::stringstream ss(text);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (first) { first = false; continue; }
        auto cols = split(line, '\t');
        if (cols.size() < 5) continue;
        CatalogEntry entry;
        entry.backend_kind = backend_kind_from_string(cols[0]);
        entry.game_key = cols[1];
        entry.asset_path = "rumble/" + cols[2];
        entry.aliases = split(cols[3], '|');
        entry.trigger_count = std::atoi(cols[4].c_str());
        m_catalog.push_back(std::move(entry));
    }
    m_catalog_loaded = true;
    error_out.clear();
    RLOGI("catalog loaded: %zu entries", m_catalog.size());
    return true;
}

bool ExperimentalRumbleManager::load_profile(AAssetManager* asset_manager, const CatalogEntry& entry, Profile& profile_out) {
    std::string text;
    if (!read_asset_text(asset_manager, entry.asset_path, text)) {
        RLOGI("profile asset missing: %s", entry.asset_path.c_str());
        return false;
    }
    profile_out = {};
    profile_out.backend_kind = entry.backend_kind;
    profile_out.aliases = entry.aliases;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("game_name=", 0) == 0) {
            profile_out.game_name = line.substr(10);
            continue;
        }
        if (line.rfind("game_key=", 0) == 0) {
            profile_out.game_key = line.substr(9);
            continue;
        }
        if (line.rfind("trigger\t", 0) != 0) continue;
        auto cols = split(line, '\t');
        if (cols.size() < 11) continue;
        Trigger trigger;
        trigger.id = cols[1];
        trigger.event = cols[2];
        const uint32_t absolute_address = static_cast<uint32_t>(std::strtoul(cols[3].c_str(), nullptr, 16));
        trigger.offset = (entry.backend_kind == BackendKind::Genesis)
            ? static_cast<std::size_t>(absolute_address - 0xFF0000u)
            : static_cast<std::size_t>(((absolute_address >> 16) - 0x7Eu) * 0x10000u + (absolute_address & 0xFFFFu));
        trigger.size = std::max(1, std::atoi(cols[4].c_str()));
        trigger.value_type = cols[5];
        trigger.byte_order = cols[6];
        trigger.condition = cols[7];
        trigger.priority = event_priority(trigger.event);
        trigger.cooldown_frames = event_cooldown_frames(trigger.event);
        for (const std::string& part : split(cols[10], ';')) {
            if (part.empty()) continue;
            auto h = split(part, ':');
            if (h.size() != 3) continue;
            HapticSpec spec;
            spec.right = h[0] == "right";
            spec.amplitude = std::strtof(h[1].c_str(), nullptr);
            spec.duration_ms = std::atoi(h[2].c_str());
            spec.start_ms = 0;
            spec.effect = RumbleEffect::Normal;
            trigger.haptics.push_back(spec);
        }
        if (!trigger.haptics.empty()) profile_out.triggers.push_back(std::move(trigger));
    }
    RLOGI("profile loaded: key=%s game=%s triggers=%zu asset=%s",
          entry.game_key.c_str(), profile_out.game_name.c_str(), profile_out.triggers.size(), entry.asset_path.c_str());
    return true;
}

bool ExperimentalRumbleManager::load_user_profile(BackendKind backend_kind,
                                                  const std::string& path,
                                                  Profile& profile_out,
                                                  std::string& error_out) {
    std::string text;
    if (!read_file_text(path, text)) {
        error_out = "unable to read file";
        return false;
    }

    profile_out = {};
    profile_out.backend_kind = backend_kind;
    std::stringstream ss(text);
    std::string line;
    std::vector<std::string> header_cols;
    std::vector<std::string> match_names;
    std::string game_name;
    int line_no = 0;
    bool have_header = false;

    auto fail = [&](const std::string& reason) {
        error_out = "line " + std::to_string(line_no) + ": " + reason;
        return false;
    };

    std::vector<Trigger> parsed_triggers;
    std::vector<std::string> normalized_header_cols;
    while (std::getline(ss, line)) {
        ++line_no;
        line = trim(line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            std::string meta = trim(line.substr(1));
            if (meta.rfind("game_name=", 0) == 0) {
                game_name = trim(meta.substr(10));
            } else if (meta.rfind("match_names=", 0) == 0) {
                auto parts = split(meta.substr(12), '|');
                for (auto& part : parts) {
                    part = normalize_name(trim(part));
                    if (!part.empty()) match_names.push_back(part);
                }
            }
            continue;
        }

        if (!have_header) {
            header_cols = parse_csv_fields(line);
            normalized_header_cols.clear();
            for (auto& col : header_cols) normalized_header_cols.push_back(normalize_name(trim(col)));
            have_header = true;
            continue;
        }

        auto row = parse_csv_fields(line);
        if (row.size() < header_cols.size()) row.resize(header_cols.size());

        auto get = [&](const char* name) -> std::string {
            const std::string target = normalize_name(name);
            for (std::size_t i = 0; i < normalized_header_cols.size(); ++i) {
                if (normalized_header_cols[i] == target) return trim(i < row.size() ? row[i] : "");
            }
            return "";
        };

        static const char* k_required[] = {
            "event_id", "address", "size", "value_type", "condition", "controller",
            "amplitude", "effect", "start_ms", "duration_ms", "cooldown_ms", "priority"
        };
        for (const char* required : k_required) {
            if (get(required).empty()) return fail(std::string("missing required column or value: ") + required);
        }

        Trigger trigger;
        trigger.id = get("event_id");
        trigger.event = trigger.id;

        bool ok = false;
        const int64_t address = parse_int_auto(get("address"), ok);
        if (!ok) return fail("invalid address");
        if (backend_kind == BackendKind::Genesis) {
            if (address < 0xFF0000 || address > 0xFFFFFF) return fail("genesis address must be within 0xFF0000-0xFFFFFF");
            trigger.offset = static_cast<std::size_t>(address - 0xFF0000);
        } else {
            const uint32_t bank = (static_cast<uint32_t>(address) >> 16) & 0xFFu;
            if (bank != 0x7E && bank != 0x7F) return fail("snes address must be in WRAM bank 0x7E/0x7F");
            trigger.offset = static_cast<std::size_t>((bank - 0x7E) * 0x10000u + (static_cast<uint32_t>(address) & 0xFFFFu));
        }

        trigger.size = static_cast<int>(parse_int_auto(get("size"), ok));
        if (!ok || (trigger.size != 1 && trigger.size != 2 && trigger.size != 4)) return fail("size must be 1, 2, or 4");

        std::string value_type = normalize_name(get("value_type"));
        if (value_type == "uint8" || value_type == "uint16" || value_type == "uint32" || value_type == "uint") value_type = "uint";
        if (value_type == "int8" || value_type == "int16" || value_type == "int32" || value_type == "int") value_type = "int";
        if (value_type != "uint" && value_type != "int" && value_type != "bcd" && value_type != "nibble") return fail("unsupported value_type");
        trigger.value_type = value_type;
        trigger.byte_order = backend_kind == BackendKind::Genesis ? "big" : "big";

        std::string condition = normalize_name(get("condition"));
        if (condition == "prevgtcurr") condition = "prev_gt_curr";
        else if (condition == "prevltcurr") condition = "prev_lt_curr";
        else if (condition == "becamezero") condition = "became_zero";
        else if (condition == "becamenonzero") condition = "became_nonzero";
        else if (condition == "notequals") condition = "not_equals";
        if (condition != "prev_gt_curr" && condition != "prev_lt_curr" && condition != "changed" &&
            condition != "became_zero" && condition != "became_nonzero" &&
            condition != "equals" && condition != "not_equals") {
            return fail("unsupported condition");
        }
        trigger.condition = condition;
        if (condition == "equals" || condition == "not_equals") {
            trigger.compare_value = parse_int_auto(get("compare_value"), ok);
            if (!ok) return fail("compare_value required for equals/not_equals");
            trigger.has_compare_value = true;
        }

        const std::string controller = normalize_name(get("controller"));
        if (controller != "left" && controller != "right" && controller != "both") return fail("controller must be left, right, or both");

        char* amp_end = nullptr;
        const float amplitude = std::strtof(get("amplitude").c_str(), &amp_end);
        if (!(amp_end && *amp_end == '\0') || amplitude < 0.0f || amplitude > 1.0f) return fail("amplitude must be 0.0 to 1.0");

        std::string effect_name = normalize_name(get("effect"));
        RumbleEffect effect = RumbleEffect::Normal;
        if (effect_name == "fadein") effect = RumbleEffect::FadeIn;
        else if (effect_name == "fadeout") effect = RumbleEffect::FadeOut;
        else if (effect_name == "fadeinout") effect = RumbleEffect::FadeInOut;
        else if (effect_name != "normal") return fail("unsupported effect");

        const int start_ms = static_cast<int>(parse_int_auto(get("start_ms"), ok));
        if (!ok || start_ms < 0) return fail("start_ms must be >= 0");
        const int duration_ms = static_cast<int>(parse_int_auto(get("duration_ms"), ok));
        if (!ok || duration_ms <= 0) return fail("duration_ms must be > 0");
        const int cooldown_ms = static_cast<int>(parse_int_auto(get("cooldown_ms"), ok));
        if (!ok || cooldown_ms < 0) return fail("cooldown_ms must be >= 0");
        const int priority = static_cast<int>(parse_int_auto(get("priority"), ok));
        if (!ok) return fail("priority must be an integer");

        trigger.priority = priority;
        trigger.cooldown_frames = cooldown_ms_to_frames(cooldown_ms);

        HapticSpec spec;
        spec.amplitude = amplitude;
        spec.duration_ms = duration_ms;
        spec.start_ms = start_ms;
        spec.effect = effect;
        if (controller == "left") {
            spec.right = false;
            trigger.haptics.push_back(spec);
        } else if (controller == "right") {
            spec.right = true;
            trigger.haptics.push_back(spec);
        } else {
            spec.right = false;
            trigger.haptics.push_back(spec);
            spec.right = true;
            trigger.haptics.push_back(spec);
        }
        parsed_triggers.push_back(std::move(trigger));
    }

    if (!have_header) {
        error_out = "missing CSV header";
        return false;
    }

    if (match_names.empty()) {
        const auto slash = path.find_last_of("/\\");
        const std::string filename = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const auto dot = filename.rfind('.');
        const std::string stem = (dot == std::string::npos) ? filename : filename.substr(0, dot);
        const std::string fallback_alias = normalize_name(stem);
        if (!fallback_alias.empty()) match_names.push_back(fallback_alias);
    }

    profile_out.game_name = game_name.empty() ? path : game_name;
    profile_out.game_key = game_name.empty() ? path : game_name;
    profile_out.aliases = match_names;
    profile_out.triggers = std::move(parsed_triggers);
    return true;
}

bool ExperimentalRumbleManager::find_matching_user_profile(BackendKind backend_kind,
                                                           const std::string& normalized_rom,
                                                           const std::string& normalized_header,
                                                           Profile& profile_out,
                                                           std::string& error_out) {
    error_out.clear();
    if (m_user_root.empty()) return false;
    const std::string system_dir = m_user_root + "/" + (backend_kind == BackendKind::Genesis ? "genesis" : "snes");
    DIR* dir = opendir(system_dir.c_str());
    if (!dir) return false;

    bool saw_broken_match = false;
    std::string broken_error;
    dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || !ends_with_csv(name)) continue;
        Profile candidate;
        std::string path = system_dir + "/" + name;
        std::string parse_error;
        if (!load_user_profile(backend_kind, path, candidate, parse_error)) {
            const std::string fallback_alias = normalize_name(name);
            if ((!normalized_header.empty() && fallback_alias == normalized_header) ||
                (!normalized_rom.empty() && fallback_alias == normalized_rom)) {
                saw_broken_match = true;
                broken_error = path + ": " + parse_error;
                RLOGI("user csv broken: %s", broken_error.c_str());
            }
            continue;
        }
        bool matches = false;
        for (const std::string& alias : candidate.aliases) {
            if ((!normalized_header.empty() && alias == normalized_header) ||
                (!normalized_rom.empty() && alias == normalized_rom)) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;
        closedir(dir);
        profile_out = std::move(candidate);
        RLOGI("user csv matched: %s aliases=%zu triggers=%zu",
              path.c_str(), profile_out.aliases.size(), profile_out.triggers.size());
        return true;
    }
    closedir(dir);
    if (saw_broken_match) error_out = broken_error;
    return false;
}

void ExperimentalRumbleManager::set_user_root(const std::string& user_root) {
    m_user_root = user_root;
}

void ExperimentalRumbleManager::set_enabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!m_enabled) m_active_status = "OFF";
    RLOGI("toggle: experimental rumble %s", m_enabled ? "ON" : "OFF");
    reset_runtime();
}

void ExperimentalRumbleManager::reset_runtime() {
    m_frame_counter = 0;
    for (auto& trigger : m_active_profile.triggers) {
        trigger.has_prev = false;
        trigger.prev_value = 0;
        trigger.last_fire_frame = 0;
    }
}

void ExperimentalRumbleManager::on_rom_loaded(AAssetManager* asset_manager,
                                              BackendKind backend_kind,
                                              const std::string& rom_filename,
                                              const std::string& game_name) {
    reset_runtime();
    m_has_active_profile = false;
    m_active_profile = {};
    m_active_profile.backend_kind = backend_kind;
    if (!m_enabled || !m_catalog_loaded) {
        m_active_status = "OFF";
        RLOGI("rom load skipped: enabled=%d catalog_loaded=%d rom=%s header=%s",
              (int)m_enabled, (int)m_catalog_loaded, rom_filename.c_str(), game_name.c_str());
        return;
    }

    const std::string normalized_header = normalize_name(game_name);
    const std::string normalized_rom = normalize_name(rom_filename);
    RLOGI("match start: system=%s rom=%s header=%s normalized_rom=%s normalized_header=%s",
          backend_kind == BackendKind::Genesis ? "genesis" : "snes",
          rom_filename.c_str(), game_name.c_str(), normalized_rom.c_str(), normalized_header.c_str());

    Profile user_profile;
    std::string user_error;
    if (find_matching_user_profile(backend_kind, normalized_rom, normalized_header, user_profile, user_error)) {
        m_active_profile = std::move(user_profile);
        m_has_active_profile = !m_active_profile.triggers.empty();
        m_active_status = m_has_active_profile ? "USER OK" : "NO MATCH";
        return;
    }

    const CatalogEntry* matched = nullptr;
    for (const auto& entry : m_catalog) {
        if (entry.backend_kind != backend_kind) continue;
        if (!normalized_header.empty() &&
            std::find(entry.aliases.begin(), entry.aliases.end(), normalized_header) != entry.aliases.end()) {
            matched = &entry;
            break;
        }
    }
    if (!matched) {
        for (const auto& entry : m_catalog) {
            if (entry.backend_kind != backend_kind) continue;
            if (!normalized_rom.empty() &&
                std::find(entry.aliases.begin(), entry.aliases.end(), normalized_rom) != entry.aliases.end()) {
                matched = &entry;
                break;
            }
        }
    }

    if (!matched) {
        m_active_status = user_error.empty() ? "NO MATCH" : "USER BROKEN";
        if (!user_error.empty()) RLOGI("user fallback unavailable: %s", user_error.c_str());
        RLOGI("no profile: rom=%s header=%s normalized_rom=%s normalized_header=%s",
              rom_filename.c_str(), game_name.c_str(), normalized_rom.c_str(), normalized_header.c_str());
        return;
    }

    Profile bundled_profile;
    if (!load_profile(asset_manager, *matched, bundled_profile)) {
        m_active_status = user_error.empty() ? "NO MATCH" : "USER BROKEN";
        RLOGI("profile load failed: key=%s asset=%s", matched->game_key.c_str(), matched->asset_path.c_str());
        return;
    }

    m_active_profile = std::move(bundled_profile);
    m_has_active_profile = !m_active_profile.triggers.empty();
    m_active_status = user_error.empty() ? "BUNDLED" : "USER BROKEN -> BUNDLED";
    RLOGI("profile matched: key=%s game=%s triggers=%zu status=%s",
          matched->game_key.c_str(),
          m_active_profile.game_name.c_str(),
          m_active_profile.triggers.size(),
          m_active_status.c_str());
}

std::string ExperimentalRumbleManager::active_status() const {
    return m_active_status;
}

std::vector<QueuedHapticEvent> ExperimentalRumbleManager::evaluate_frame(const EmulatorBackend& backend) {
    std::vector<QueuedHapticEvent> out;
    if (!m_enabled || !m_has_active_profile) return out;
    const uint8_t* ram = backend.system_ram_data();
    const std::size_t ram_size = backend.system_ram_size();
    if (!ram || ram_size == 0) return out;
    ++m_frame_counter;

    struct FiredTrigger {
        int priority = 0;
        std::size_t offset = 0;
        const Trigger* trigger = nullptr;
    };
    std::vector<FiredTrigger> fired;
    fired.reserve(4);

    for (auto& trigger : m_active_profile.triggers) {
        if (trigger.offset + static_cast<std::size_t>(trigger.size) > ram_size) continue;
        const int64_t curr = decode_value(ram + trigger.offset, trigger.size, trigger.value_type, trigger.byte_order);
        if (!trigger.has_prev) {
            trigger.has_prev = true;
            trigger.prev_value = curr;
            continue;
        }
        const bool matched = condition_matches(trigger, trigger.prev_value, curr);
        trigger.prev_value = curr;
        if (!matched) continue;
        if (m_frame_counter - trigger.last_fire_frame < static_cast<uint64_t>(trigger.cooldown_frames)) continue;
        trigger.last_fire_frame = m_frame_counter;
        fired.push_back({trigger.priority, trigger.offset, &trigger});
    }

    std::sort(fired.begin(), fired.end(), [](const FiredTrigger& a, const FiredTrigger& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.offset < b.offset;
    });

    std::vector<std::size_t> used_offsets;
    for (const auto& item : fired) {
        if (out.size() >= 2) break;
        if (std::find(used_offsets.begin(), used_offsets.end(), item.offset) != used_offsets.end()) continue;
        used_offsets.push_back(item.offset);
        if (item.trigger->haptics.empty()) continue;

        float peak_amplitude = 0.0f;
        int peak_duration_ms = 0;
        int start_ms = 0;
        bool primary_right = item.trigger->haptics.front().right;
        RumbleEffect effect = item.trigger->haptics.front().effect;
        for (const auto& spec : item.trigger->haptics) {
            peak_amplitude = std::max(peak_amplitude, std::min(1.0f, spec.amplitude * k_runtime_amplitude_scale));
            peak_duration_ms = std::max(peak_duration_ms, spec.duration_ms * k_runtime_duration_scale);
            start_ms = std::max(start_ms, spec.start_ms);
        }
        const RumbleWavePattern pattern =
            item.trigger->haptics.size() >= 2 ? RumbleWavePattern::Both : RumbleWavePattern::Single;
        QueuedHapticEvent event;
        event.right = primary_right;
        event.amplitude = peak_amplitude;
        event.duration_ms = peak_duration_ms;
        event.pattern = pattern;
        event.effect = effect;
        event.start_ms = start_ms;
        out.push_back(event);
        RLOGI("triggered: game=%s id=%s event=%s pattern=%s primary=%s amp=%.2f dur=%dms start=%dms offset=0x%zX frame=%llu",
              m_active_profile.game_name.c_str(),
              item.trigger->id.c_str(),
              item.trigger->event.c_str(),
              pattern == RumbleWavePattern::Both ? "both-wave" : "stereo-wave",
              primary_right ? "right" : "left",
              peak_amplitude,
              peak_duration_ms,
              start_ms,
              item.offset,
              (unsigned long long)m_frame_counter);
    }
    return out;
}

} // namespace qrd
