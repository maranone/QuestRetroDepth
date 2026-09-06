#include "rom_browser.h"
#include "panel_layout.h"
#include "rom_title_db.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <android/log.h>

#define LOG_TAG "RomBrowser"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace qrd {

static constexpr const char* kRecentEntryName = "Recent";
static constexpr const char* kRecentVirtualPath = "__QRD_RECENT__";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool ci_ends_with(const char* name, const char* ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return false;
    const char* tail = name + nl - el;
    for (size_t i = 0; i < el; ++i)
        if (std::tolower((unsigned char)tail[i]) != std::tolower((unsigned char)ext[i])) return false;
    return true;
}

static bool is_supported(const char* name) {
    // SNES
    if (ci_ends_with(name, ".smc") || ci_ends_with(name, ".sfc") ||
        ci_ends_with(name, ".fig") || ci_ends_with(name, ".swc")) return true;
    // Genesis / SMS / Game Gear
    if (ci_ends_with(name, ".md")  || ci_ends_with(name, ".gen") ||
        ci_ends_with(name, ".smd") || ci_ends_with(name, ".sms") ||
        ci_ends_with(name, ".gg"))  return true;
    // NES
    if (ci_ends_with(name, ".nes") || ci_ends_with(name, ".unf") ||
        ci_ends_with(name, ".unif")) return true;
    // GBA / GB / GBC
    if (ci_ends_with(name, ".gba") || ci_ends_with(name, ".gb") ||
        ci_ends_with(name, ".gbc")) return true;
    // PC Engine
    if (ci_ends_with(name, ".pce")) return true;
    // Sega Saturn disc images. A .cue is listed as the launch file and must
    // remain beside its referenced .bin tracks; ISO/CHD are self-contained.
    if (ci_ends_with(name, ".cue") || ci_ends_with(name, ".iso") ||
        ci_ends_with(name, ".chd")) return true;
    // Archives
    if (ci_ends_with(name, ".zip") || ci_ends_with(name, ".7z")) return true;
    return false;
}

// ---------------------------------------------------------------------------
// scan / scan_impl
// ---------------------------------------------------------------------------

void RomBrowser::set_recent_store(const std::string& settings_dir) {
    m_recent_store_path = settings_dir.empty() ? std::string()
                                                : settings_dir + "/recent_roms.txt";
    load_recent();
    m_dirty = true;
}

void RomBrowser::load_recent() {
    m_recent.clear();
    if (m_recent_store_path.empty()) return;
    std::ifstream in(m_recent_store_path);
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::int64_t timestamp = 0;
        try { timestamp = std::stoll(line.substr(0, tab)); }
        catch (...) { continue; }
        const std::string path = line.substr(tab + 1);
        if (path.empty()) continue;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && is_supported(path.c_str()))
            m_recent.emplace_back(timestamp, path);
    }
    std::sort(m_recent.begin(), m_recent.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (m_recent.size() > 32) m_recent.resize(32);
}

void RomBrowser::save_recent() const {
    if (m_recent_store_path.empty()) return;
    std::ofstream out(m_recent_store_path, std::ios::trunc);
    if (!out) return;
    for (const auto& item : m_recent)
        out << item.first << '\t' << item.second << '\n';
}

void RomBrowser::record_recent(const std::string& rom_path) {
    if (rom_path.empty()) return;
    m_recent.erase(std::remove_if(m_recent.begin(), m_recent.end(),
                                  [&](const auto& item) { return item.second == rom_path; }),
                   m_recent.end());
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_recent.emplace_back(now, rom_path);
    std::sort(m_recent.begin(), m_recent.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (m_recent.size() > 32) m_recent.resize(32);
    save_recent();
    m_dirty = true;
}

void RomBrowser::clear_recent() {
    m_recent.clear();
    if (!m_recent_store_path.empty()) std::remove(m_recent_store_path.c_str());
    if (m_recent_mode) scan_impl(m_root_dir);
    m_dirty = true;
}

void RomBrowser::scan(const std::string& dir) {
    if (!dir.empty()) {
        if (m_root_dir.empty()) m_root_dir = dir;
        scan_impl(dir);
        return;
    }
    if (!m_current_dir.empty()) {
        scan_impl(m_current_dir);
        return;
    }
    if (!m_root_dir.empty()) {
        scan_impl(m_root_dir);
        return;
    }
    m_entries.clear();
    m_hovered = 0;
    m_scroll = 0;
    m_dirty = true;
}

void RomBrowser::scan_impl(const std::string& dir) {
    std::string effective_dir = dir;
    if (effective_dir.empty()) {
        if (!m_current_dir.empty()) effective_dir = m_current_dir;
        else effective_dir = m_root_dir;
    }

    m_entries.clear();
    m_hovered = 0;
    m_scroll  = 0;
    m_dirty   = true;
    m_recent_mode = false;
    m_current_dir = effective_dir;
    if (m_root_dir.empty() && !effective_dir.empty()) m_root_dir = effective_dir;

    // "Back" entry when not at root
    if (!effective_dir.empty() && effective_dir != m_root_dir) {
        RomEntry back;
        back.name   = ".. (Back)";
        back.display_name = ".. (Back)";
        back.path   = effective_dir;   // placeholder; enter_hovered() will compute parent
        back.is_dir = true;
        m_entries.push_back(std::move(back));
    }

    if (effective_dir.empty()) {
        LOGI("scan_impl: empty path, nothing to scan");
        return;
    }

    DIR* d = opendir(effective_dir.c_str());
    if (!d) {
        LOGI("scan_impl: cannot open '%s'", effective_dir.c_str());
        return;
    }

    std::vector<RomEntry> dirs, files;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        bool is_dir_entry = (ent->d_type == DT_DIR);
        // DT_UNKNOWN: stat to determine type
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st{};
            std::string full = effective_dir + "/" + ent->d_name;
            if (stat(full.c_str(), &st) == 0) is_dir_entry = S_ISDIR(st.st_mode);
        }

        RomEntry e;
        e.name   = ent->d_name;
        e.display_name = is_dir_entry ? e.name : rom_display_name(e.name);
        e.path   = effective_dir + "/" + ent->d_name;
        e.is_dir = is_dir_entry;

        if (is_dir_entry) {
            dirs.push_back(std::move(e));
        } else if (is_supported(ent->d_name)) {
            files.push_back(std::move(e));
        }
    }
    closedir(d);

    auto by_name = [](const RomEntry& a, const RomEntry& b){ return a.name < b.name; };
    std::sort(dirs.begin(),  dirs.end(),  by_name);
    std::sort(files.begin(), files.end(), by_name);

    for (auto& e : dirs)  m_entries.push_back(std::move(e));
    for (auto& e : files) m_entries.push_back(std::move(e));

    if (effective_dir == m_root_dir) {
        RomEntry recent;
        recent.name = kRecentEntryName;
        recent.display_name = kRecentEntryName;
        recent.path = kRecentVirtualPath;
        recent.is_dir = true;
        m_entries.insert(m_entries.begin(), std::move(recent));
    }

    LOGI("scan_impl: %d dirs, %d ROMs in '%s'",
         (int)dirs.size(), (int)files.size(), effective_dir.c_str());
}

void RomBrowser::scan_recent_impl() {
    m_entries.clear();
    m_hovered = 0;
    m_scroll = 0;
    m_dirty = true;
    m_recent_mode = true;
    for (const auto& item : m_recent) {
        struct stat st{};
        if (stat(item.second.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        RomEntry e;
        const auto slash = item.second.find_last_of("/\\");
        e.name = slash == std::string::npos ? item.second : item.second.substr(slash + 1);
        e.display_name = rom_display_name(e.name);
        e.path = item.second;
        e.is_dir = false;
        m_entries.push_back(std::move(e));
    }
    RomEntry back;
    back.name = ".. (Back)";
    back.display_name = ".. (Back)";
    back.path = m_root_dir;
    back.is_dir = true;
    m_entries.insert(m_entries.begin(), std::move(back));
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool RomBrowser::set_hover_uv(float /*u*/, float v) {
    if (m_entries.empty()) return false;
    int n_in_view = m_visible_count > 0 ? m_visible_count
                                        : std::min((kTexH - kTitleH) / kRowH,
                                                   std::max(1, (int)m_entries.size() - m_scroll));
    PanelLayout layout = make_browser_layout(n_in_view, m_scroll);
    const PanelLayoutItem* item = layout.hit(0.5f, v);
    if (!item) return false;
    int abs_row = item->id;
    abs_row = std::max(0, std::min(abs_row, (int)m_entries.size() - 1));
    if (abs_row == m_hovered) return false;
    m_hovered = abs_row;
    // No longer mark dirty — highlight is drawn as a separate quad
    return true;
}

std::vector<std::string> RomBrowser::visible_rom_paths() const {
    std::vector<std::string> paths;
    const int visible = m_visible_count > 0 ? m_visible_count : visible_rows();
    const int first = std::max(0, m_scroll);
    const int last = std::min((int)m_entries.size(), first + visible);
    for (int i = first; i < last; ++i) {
        if (!m_entries[i].is_dir && !m_entries[i].path.empty()) paths.push_back(m_entries[i].path);
    }
    return paths;
}

std::vector<RomEntry> RomBrowser::visible_entries() const {
    std::vector<RomEntry> entries;
    const int visible = m_visible_count > 0 ? m_visible_count : visible_rows();
    const int first = std::max(0, m_scroll);
    const int last = std::min((int)m_entries.size(), first + visible);
    for (int i = first; i < last; ++i) entries.push_back(m_entries[i]);
    return entries;
}

std::string RomBrowser::peek_hovered_target_dir() const {
    if (m_entries.empty()) return {};
    int idx = std::max(0, std::min(m_hovered, (int)m_entries.size() - 1));
    if (!m_entries[idx].is_dir) return {};
    if (m_entries[idx].path == kRecentVirtualPath) return {};
    if (m_entries[idx].name == ".. (Back)") {
        std::string parent = m_current_dir;
        auto slash = parent.rfind('/');
        if (slash != std::string::npos) parent = parent.substr(0, slash);
        else parent.clear();
        if (parent.size() < m_root_dir.size() || parent.empty()) parent = m_root_dir;
        return parent;
    }
    std::string next = m_entries[idx].path;
    if (next.empty()) {
        next = m_current_dir;
        if (!next.empty() && next.back() != '/') next += '/';
        next += m_entries[idx].name;
    } else if (next.front() != '/' && !m_current_dir.empty()) {
        std::string abs = m_current_dir;
        if (abs.back() != '/') abs += '/';
        next = abs + next;
    }
    return next;
}

std::vector<std::string> RomBrowser::list_dir_rom_paths(const std::string& dir) const {
    std::vector<std::string> paths;
    if (dir.empty()) return paths;
    DIR* d = opendir(dir.c_str());
    if (!d) return paths;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dir + "/" + ent->d_name;
        bool is_dir_entry = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st{};
            if (stat(full.c_str(), &st) == 0) is_dir_entry = S_ISDIR(st.st_mode);
        }
        if (!is_dir_entry && is_supported(ent->d_name)) paths.push_back(std::move(full));
    }
    closedir(d);
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<RomEntry> RomBrowser::scan_recursive(const std::string& root) const {
    std::vector<RomEntry> out;
    if (root.empty()) return out;
    std::vector<std::string> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        std::string dir = std::move(stack.back());
        stack.pop_back();
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string full = dir + "/" + ent->d_name;
            bool is_dir_entry = (ent->d_type == DT_DIR);
            if (ent->d_type == DT_UNKNOWN) {
                struct stat st{};
                if (stat(full.c_str(), &st) == 0) is_dir_entry = S_ISDIR(st.st_mode);
            }
            if (is_dir_entry) {
                stack.push_back(full);
            } else if (is_supported(ent->d_name)) {
                RomEntry e;
                e.name = ent->d_name;
                e.display_name = rom_display_name(e.name);
                e.path = full;
                e.is_dir = false;
                out.push_back(std::move(e));
            }
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const RomEntry& a, const RomEntry& b) { return a.name < b.name; });
    return out;
}

void RomBrowser::scroll(int delta) {
    int n_visible = visible_rows();
    m_scroll = std::max(0, std::min(m_scroll + delta,
                                    std::max(0, (int)m_entries.size() - n_visible)));
    m_dirty = true;
}

void RomBrowser::scroll_page(int pages) {
    if (pages == 0) return;
    scroll(pages * visible_rows());
}

const std::string& RomBrowser::hovered_path() const {
    static const std::string empty;
    if (m_entries.empty()) return empty;
    int idx = std::max(0, std::min(m_hovered, (int)m_entries.size()-1));
    if (m_entries[idx].is_dir) return empty; // caller must use enter_hovered() instead
    return m_entries[idx].path;
}

bool RomBrowser::hovered_is_dir() const {
    if (m_entries.empty()) return false;
    int idx = std::max(0, std::min(m_hovered, (int)m_entries.size()-1));
    return m_entries[idx].is_dir;
}

bool RomBrowser::enter_hovered() {
    if (m_entries.empty()) return false;
    int idx = std::max(0, std::min(m_hovered, (int)m_entries.size()-1));
    if (!m_entries[idx].is_dir) return false;

    if (m_entries[idx].name == ".. (Back)") {
        if (m_recent_mode) {
            scan_impl(m_root_dir);
            return true;
        }
        // Navigate up one level, clamped to root
        std::string parent = m_current_dir;
        auto slash = parent.rfind('/');
        if (slash != std::string::npos) parent = parent.substr(0, slash);
        else parent.clear();
        // Don't go above root
        if (parent.size() < m_root_dir.size() || parent.empty()) parent = m_root_dir;
        scan_impl(parent);
    } else if (m_entries[idx].path == kRecentVirtualPath) {
        scan_recent_impl();
    } else {
        std::string next = m_entries[idx].path;
        if (next.empty()) {
            next = m_current_dir;
            if (!next.empty() && next.back() != '/') next += '/';
            next += m_entries[idx].name;
        } else if (next.front() != '/' && !m_current_dir.empty()) {
            std::string abs = m_current_dir;
            if (abs.back() != '/') abs += '/';
            next = abs + next;
        }
        scan_impl(next);
    }
    return true;
}

// ---------------------------------------------------------------------------
// GL texture
// ---------------------------------------------------------------------------

void RomBrowser::destroy_texture() {
    if (m_tex) { glDeleteTextures(1, &m_tex); m_tex = 0; }
}

void RomBrowser::upload_pixels(const jint* pixels) {
    // Android Bitmap format is ARGB_8888 (packed int, big-endian ARGB).
    // OpenGL expects RGBA byte order.
    std::vector<uint8_t> rgba(kTexW * kTexH * 4);
    for (int i = 0; i < kTexW * kTexH; ++i) {
        uint32_t argb = (uint32_t)pixels[i];
        rgba[i*4+0] = (argb >> 16) & 0xFF; // R
        rgba[i*4+1] = (argb >>  8) & 0xFF; // G
        rgba[i*4+2] = (argb >>  0) & 0xFF; // B
        rgba[i*4+3] = (argb >> 24) & 0xFF; // A
    }

    if (!m_tex) {
        glGenTextures(1, &m_tex);
    }
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTexW, kTexH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_bitmap_rgba = rgba;
    ++m_bitmap_generation;
    m_dirty = false;
}

// ---------------------------------------------------------------------------
// rebuild_texture — calls Kotlin renderRomPanelBitmap via JNI
// ---------------------------------------------------------------------------

void RomBrowser::rebuild_texture(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    int rc = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        else { LOGE("rebuild_texture: AttachCurrentThread failed"); return; }
    } else if (rc != JNI_OK || !env) {
        LOGE("rebuild_texture: GetEnv failed");
        return;
    }

    // Determine visible window. Matches the ROM shelf grid capacity
    // (visible_rows()) rather than the flat text bitmap's own row count so a
    // full shelf's worth of entries is available; extra rows beyond what the
    // bitmap can show are simply clipped (harmless — this bitmap is only
    // used as the flat-panel fallback when the shelf isn't drawn).
    const int n_visible = visible_rows();
    const int first = m_scroll;
    const int last  = std::min(first + n_visible, (int)m_entries.size());
    const int count = last - first;

    m_visible_count = count; // Store for hover detection

    // Build java String[] of visible names and boolean[] isDir flags
    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray names = env->NewObjectArray(count, str_cls, nullptr);
    jbooleanArray isDir = env->NewBooleanArray(count);
    {
        std::vector<jboolean> dirFlags(count);
        for (int i = 0; i < count; ++i) {
            jstring js = env->NewStringUTF(m_entries[first + i].display_name.c_str());
            env->SetObjectArrayElement(names, i, js);
            env->DeleteLocalRef(js);
            dirFlags[i] = m_entries[first + i].is_dir ? JNI_TRUE : JNI_FALSE;
        }
        env->SetBooleanArrayRegion(isDir, 0, count, dirFlags.data());
    }

    int hovered_in_view = m_hovered - m_scroll;
    bool has_more_up   = (m_scroll > 0);
    bool has_more_down = (last < (int)m_entries.size());

    // Call: renderRomPanelBitmap(String[] names, boolean[] isDir, int hovered,
    //                            int width, int height,
    //                            boolean hasMoreUp, boolean hasMoreDown): int[]
    jclass  cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "renderRomPanelBitmap",
                                      "([Ljava/lang/String;[ZIIIZZ)[I");
    if (!mid) {
        LOGE("rebuild_texture: renderRomPanelBitmap not found");
        env->DeleteLocalRef(names);
        env->DeleteLocalRef(isDir);
        if (attached) vm->DetachCurrentThread();
        return;
    }

    jintArray result = (jintArray)env->CallObjectMethod(
        activity, mid, names, isDir,
        (jint)hovered_in_view, (jint)kTexW, (jint)kTexH,
        (jboolean)has_more_up, (jboolean)has_more_down);
    env->DeleteLocalRef(names);
    env->DeleteLocalRef(isDir);

    if (result) {
        jsize len = env->GetArrayLength(result);
        if (len >= kTexW * kTexH) {
            jint* pixels = env->GetIntArrayElements(result, nullptr);
            if (pixels) {
                upload_pixels(pixels);
                env->ReleaseIntArrayElements(result, pixels, JNI_ABORT);
            }
        }
        env->DeleteLocalRef(result);
    }

    if (attached) vm->DetachCurrentThread();
}

} // namespace qrd
