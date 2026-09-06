#include "mame_driver_catalog.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <array>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace qrd {
namespace {

std::unordered_map<std::string, MameLayerProfile> g_generated_profiles;
std::mutex g_generated_profiles_mutex;
bool g_generated_profiles_initialized = false;

bool equals_any(std::string_view value, const auto& names) {
    for (const char* name : names) if (value == name) return true;
    return false;
}

MameDriverClassification captured(MameLayerProfile profile, const char* family) {
    return {profile, family, true};
}

MameDriverClassification fallback(const char* family) {
    return {MameLayerProfile::FullFrame, family, false};
}

const char* family_for_profile(MameLayerProfile profile) {
    switch (profile) {
    case MameLayerProfile::Cps: return "Capcom CPS1/CPS2 (database)";
    case MameLayerProfile::Konami: return "Konami TMNT/Simpsons (database)";
    case MameLayerProfile::Sega16B: return "Sega System 16B (database)";
    case MameLayerProfile::Dec0: return "Data East DEC0 (database)";
    case MameLayerProfile::Gp9001: return "Toaplan GP9001 (database)";
    case MameLayerProfile::NeoGeo: return "SNK Neo Geo (database)";
    case MameLayerProfile::Saturn: return "Sega Saturn (database)";
    case MameLayerProfile::FullFrame: return "full frame";
    }
    return "full frame";
}

bool profile_from_name(std::string_view name, MameLayerProfile& out) {
    if (name == "cps") out = MameLayerProfile::Cps;
    else if (name == "konami") out = MameLayerProfile::Konami;
    else if (name == "sega16b") out = MameLayerProfile::Sega16B;
    else if (name == "dec0") out = MameLayerProfile::Dec0;
    else if (name == "gp9001") out = MameLayerProfile::Gp9001;
    else if (name == "neogeo") out = MameLayerProfile::NeoGeo;
    else if (name == "saturn") out = MameLayerProfile::Saturn;
    else return false;
    return true;
}

} // namespace

void initialize_mame_driver_database(AAssetManager* asset_manager) {
    if (!asset_manager) return;
    std::lock_guard<std::mutex> lock(g_generated_profiles_mutex);
    if (g_generated_profiles_initialized) return;
    g_generated_profiles_initialized = true;

    AAsset* asset = AAssetManager_open(asset_manager, "game_titles/mame_profiles.txt",
                                       AASSET_MODE_BUFFER);
    if (!asset) {
        __android_log_print(ANDROID_LOG_ERROR, "MameDriverCatalog",
                            "Could not open bundled MAME profile database");
        return;
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

    std::istringstream lines(contents);
    std::string line;
    size_t loaded = 0;
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto first = line.find('|');
        const auto second = first == std::string::npos
            ? std::string::npos : line.find('|', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        std::string name = line.substr(0, first);
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        MameLayerProfile profile;
        if (!profile_from_name(line.substr(second + 1), profile)) continue;
        if (!name.empty()) {
            g_generated_profiles[std::move(name)] = profile;
            ++loaded;
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "MameDriverCatalog",
                        "Loaded %zu generated MAME profile mappings", loaded);
}

MameDriverClassification classify_mame_driver(std::string_view name) {
    std::string normalized(name);
    for (char& c : normalized) c = (char)std::tolower((unsigned char)c);
    {
        std::lock_guard<std::mutex> lock(g_generated_profiles_mutex);
        const auto it = g_generated_profiles.find(normalized);
        if (it != g_generated_profiles.end()) {
            return {it->second, family_for_profile(it->second), true};
        }
    }

    // These lists describe source families that have verified hooks in this
    // checkout. Clone names are explicit where a broad prefix would wrongly
    // classify a different source file (notably tmnt2).
    static constexpr std::array cps = {
        "1941", "1944", "19xxh", "3wonders", "armwar", "avsph",
        "batcir", "captcomm", "captcommu", "cawing", "csclub", "cybots", "ddsomh",
        "ddtodh", "dino", "dimahoo", "dstlk", "ecofghtr", "ffight", "forgottn",
        "ghouls", "gigawing", "hsf2", "knights", "kod", "megaman2", "mercs", "mmancp2u",
        "mpang", "msh", "mshvsf", "mvsc", "nemo", "pang3", "pfghtj", "progear",
        "pzloop2", "sfa3", "sf2ce", "sfz2a", "slammast", "spf2t", "ssf2", "strider",
        "mbombrd", "msword", "mtwins", "punisher",
        "unsquad", "varth", "willow", "wof", "xmvsf",
    };
    static constexpr std::array konami = {
        "cuebrick", "mia", "mia2", "tmht", "tmht2p", "tmht2pa", "tmhta", "tmhtb",
        "tmnt", "tmnta", "tmnt2pj", "tmnt2po", "tmntj", "tmntu", "tmntua", "tmntub",
        "tmntuc", "tmntucbl",
        "simpsons", "simpsons2p", "simpsons2p2", "simpsons2p3", "simpsons2pa",
        "simpsons2pj", "simpsons4pa", "simpsons4pe", "simpsons4pe2",
    };
    static constexpr std::array segas16b = {
        "aceattac", "afighter", "aliensyn", "altbeast", "atomicp", "aurail", "bayroute",
        "bullet", "cencourt", "cotton", "dddoor", "ddux", "defense", "dfjail", "dunkshot",
        "eswat", "exctleag", "fantzn2x", "fantzoneta", "fpoint", "goldnaxe", "hwchamp",
        "lockonph", "mvp", "passsht", "riotcity", "ryukyu", "sdib", "shinobi2", "shinobi3",
        "shinobi4", "shinobi5", "shinobi6", "sjryuko", "snapper", "sonicbom", "suprleag",
        "tetris1", "tetris2", "timescan", "toryumon", "tturf", "ultracin", "wb3", "wfishing",
        "wrestwar", "wwthomas",
    };
    static constexpr std::array dec0 = {
        "baddudes", "drgninja", "hippodrm", "midres", "robocop", "secretag", "slyspy",
        "automat", "birdtry", "bandit",
    };
    static constexpr std::array gp9001 = {
        "batsugun", "dogyuun", "fixeight", "grindstm", "kbash", "snowbro2", "truxton2",
        "vfive", "batrider",
    };

    if (equals_any(name, cps)) return captured(MameLayerProfile::Cps, "Capcom CPS1/CPS2");
    if (equals_any(name, konami)) return captured(MameLayerProfile::Konami, "Konami TMNT/Simpsons");
    if (equals_any(name, segas16b)) return captured(MameLayerProfile::Sega16B, "Sega System 16B");
    if (equals_any(name, dec0)) return captured(MameLayerProfile::Dec0, "Data East DEC0");
    if (equals_any(name, gp9001)) return captured(MameLayerProfile::Gp9001, "Toaplan GP9001");

    static constexpr std::array taito_pc080sn = {
        "opwolf", "opwolfa", "opwolfb", "opwolfj", "opwolfjsc", "opwolfp", "opwolfu",
        "othunder", "othundero", "othunderj", "othunderjsc", "othunderu", "othunderua",
        "undrfire", "undrfirej", "undrfireu",
    };
    if (equals_any(name, taito_pc080sn))
        return captured(MameLayerProfile::Taito, "Taito PC080SN/PC090OJ");

    static constexpr std::array namco_s2 = {
        "bubbletr", "gollygho", "luckywld", "sgunner", "sgunner2",
    };
    if (equals_any(name, namco_s2))
        return captured(MameLayerProfile::Namco, "Namco System 2");

    static constexpr std::array namco_nb1 = { "ptblank" };
    if (equals_any(name, namco_nb1))
        return captured(MameLayerProfile::Namco, "Namco NB-1");

    static constexpr std::array konami_lethal = { "lethalen" };
    if (equals_any(name, konami_lethal))
        return captured(MameLayerProfile::KonamiLethal, "Konami K056832/K053244 (Lethal Enforcers)");

    static constexpr std::array taito_tc0100 = { "spacegun" };
    if (equals_any(name, taito_tc0100))
        return captured(MameLayerProfile::TaitoTc0100, "Taito TC0100SCN (Space Gun)");

    static constexpr std::array taito_tc0480 = {
        "gunbustr", "opwolf3", "opwolf3u", "opwolf3j",
    };
    if (equals_any(name, taito_tc0480))
        return captured(MameLayerProfile::TaitoTc0480, "Taito TC0480SCP");

    static constexpr std::array unico = { "zeropnt", "zeropnt2" };
    if (equals_any(name, unico))
        return captured(MameLayerProfile::Unico, "Unico");

    static constexpr std::array oneshot = { "oneshot" };
    if (equals_any(name, oneshot))
        return captured(MameLayerProfile::Oneshot, "Misc oneshot.cpp");

    static constexpr std::array lordgun = { "lordgun" };
    if (equals_any(name, lordgun))
        return captured(MameLayerProfile::Lordgun, "IGS Lord of Gun");

    static constexpr std::array seta2 = {
        "deerhunt", "trophyh", "turkhunt", "wschamp",
    };
    if (equals_any(name, seta2))
        return captured(MameLayerProfile::Seta2, "Seta X1-020/dx-101");

    static constexpr std::array segaybd = { "rchase" };
    if (equals_any(name, segaybd))
        return captured(MameLayerProfile::Segaybd, "Sega Y-board");

    static constexpr std::array bbusters = { "bbusters", "mechatt" };
    if (equals_any(name, bbusters))
        return captured(MameLayerProfile::Bbusters, "SNK bbusters.cpp");

    static constexpr std::array nycaptor = { "nycaptor" };
    if (equals_any(name, nycaptor))
        return captured(MameLayerProfile::Nycaptor, "Taito nycaptor.cpp");

    static constexpr std::array shared_pending = {
        "tmnt2", "tmnt22pu", "tmnt24pu", "tmnt2a", "tmnt2o", "tmnt2pj",
        "segas16a", "segahang", "segaorun", "segas18", "segas18_astormbl", "segas32",
        "segaxbd", "segaybd", "system16", "taito_f2", "taito_f3", "taito_z", "taito_b",
        "asuka", "darius", "ninjaw", "rastan", "topspeed", "volfied",
        "warriorb", "megasys1", "seta", "downtown", "namcos2", "m72", "m90", "m92", "m107",
        "cave", "kaneko16",
    };
    if (equals_any(name, shared_pending)) {
        if (name.rfind("tmnt2", 0) == 0) return fallback("Konami shared K052/K053 (hook pending)");
        if (name == "segas16a" || name == "segahang" || name == "segaorun" ||
            name == "segas18" || name == "segas18_astormbl" || name == "segas32" ||
            name == "segaxbd" || name == "segaybd" || name == "system16")
            return fallback("Sega shared IC16 (hook pending)");
        if (name == "megasys1") return fallback("Jaleco Mega System 1 (hook pending)");
        if (name == "seta" || name == "downtown") return fallback("Seta X1 (hook pending)");
        if (name == "namcos2") return fallback("Namco System 2 (hook pending)");
        if (name == "m72" || name == "m90" || name == "m92" || name == "m107")
            return fallback("Irem M72/M90/M92 (hook pending)");
        if (name == "cave") return fallback("Cave (hook pending)");
        if (name == "kaneko16") return fallback("Kaneko 16-bit (hook pending)");
        return fallback("Taito shared video (hook pending)");
    }

    return fallback("unclassified MAME driver");
}

const char* mame_layer_profile_name(MameLayerProfile profile) {
    switch (profile) {
    case MameLayerProfile::Cps: return "cps";
    case MameLayerProfile::Konami: return "konami";
    case MameLayerProfile::Sega16B: return "sega16b";
    case MameLayerProfile::Dec0: return "dec0";
    case MameLayerProfile::Gp9001: return "gp9001";
    case MameLayerProfile::NeoGeo: return "neogeo";
    case MameLayerProfile::Saturn: return "saturn";
    case MameLayerProfile::Taito: return "taito";
    case MameLayerProfile::Namco: return "namco";
    case MameLayerProfile::KonamiLethal: return "konami_lethal";
    case MameLayerProfile::TaitoTc0100: return "taito_tc0100";
    case MameLayerProfile::TaitoTc0480: return "taito_tc0480";
    case MameLayerProfile::Unico: return "unico";
    case MameLayerProfile::Oneshot: return "oneshot";
    case MameLayerProfile::Lordgun: return "lordgun";
    case MameLayerProfile::Seta2: return "seta2";
    case MameLayerProfile::Segaybd: return "segaybd";
    case MameLayerProfile::Bbusters: return "bbusters";
    case MameLayerProfile::Nycaptor: return "nycaptor";
    case MameLayerProfile::FullFrame: return "full_frame";
    }
    return "full_frame";
}

} // namespace qrd
