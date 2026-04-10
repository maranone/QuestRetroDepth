#include "rom_browser.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "RomBrowser"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace qrd {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool ci_ends_with(const char* name, const char* ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return false;
    const char* tail = name + nl - el;
    for (size_t i = 0; i < el; ++i)
        if (tolower(tail[i]) != tolower(ext[i])) return false;
    return true;
}

static bool is_supported(const char* name) {
    return ci_ends_with(name, ".smc") || ci_ends_with(name, ".sfc") ||
           ci_ends_with(name, ".fig") || ci_ends_with(name, ".swc") ||
           ci_ends_with(name, ".nes") || ci_ends_with(name, ".md")  ||
           ci_ends_with(name, ".sms") || ci_ends_with(name, ".gg")  ||
           ci_ends_with(name, ".zip") || ci_ends_with(name, ".7z");
}

// ---------------------------------------------------------------------------
// scan / scan_impl
// ---------------------------------------------------------------------------

void RomBrowser::scan(const std::string& dir) {
    if (m_root_dir.empty()) m_root_dir = dir;
    scan_impl(dir);
}

void RomBrowser::scan_impl(const std::string& dir) {
    m_entries.clear();
    m_hovered = 0;
    m_scroll  = 0;
    m_dirty   = true;
    m_current_dir = dir;

    // "Back" entry when not at root
    if (dir != m_root_dir) {
        RomEntry back;
        back.name   = ".. (Back)";
        back.path   = dir;   // placeholder; enter_hovered() will compute parent
        back.is_dir = true;
        m_entries.push_back(std::move(back));
    }

    DIR* d = opendir(dir.c_str());
    if (!d) {
        LOGI("scan_impl: cannot open '%s'", dir.c_str());
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
            std::string full = dir + "/" + ent->d_name;
            if (stat(full.c_str(), &st) == 0) is_dir_entry = S_ISDIR(st.st_mode);
        }

        RomEntry e;
        e.name   = ent->d_name;
        e.path   = dir + "/" + ent->d_name;
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

    LOGI("scan_impl: %d dirs, %d ROMs in '%s'",
         (int)dirs.size(), (int)files.size(), dir.c_str());
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool RomBrowser::set_hover_uv(float /*u*/, float v) {
    if (m_entries.empty()) return false;
    // v = 0 → top of texture. Title bar takes kTitleH/kTexH fraction.
    float content_v0 = (float)kTitleH / (float)kTexH;
    float content_v  = (v - content_v0) / (1.0f - content_v0);
    if (content_v < 0.0f) return false; // hovering title bar

    // Use actual visible count (set by rebuild_texture) to match Kotlin's layout
    // If not yet rendered (0), fall back to fixed row calculation
    int n_in_view = m_visible_count > 0 ? m_visible_count
                                        : std::min((kTexH - kTitleH) / kRowH,
                                                   std::max(1, (int)m_entries.size() - m_scroll));
    int row_in_view = (int)(content_v * n_in_view);
    int abs_row = m_scroll + row_in_view;
    abs_row = std::max(0, std::min(abs_row, (int)m_entries.size() - 1));
    if (abs_row == m_hovered) return false;
    m_hovered = abs_row;
    // No longer mark dirty — highlight is drawn as a separate quad
    return true;
}

void RomBrowser::scroll(int delta) {
    int n_visible = (kTexH - kTitleH) / kRowH;
    m_scroll = std::max(0, std::min(m_scroll + delta,
                                    std::max(0, (int)m_entries.size() - n_visible)));
    m_dirty = true;
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
        // Navigate up one level, clamped to root
        std::string parent = m_current_dir;
        auto slash = parent.rfind('/');
        if (slash != std::string::npos) parent = parent.substr(0, slash);
        // Don't go above root
        if (parent.size() < m_root_dir.size() || parent.empty()) parent = m_root_dir;
        scan_impl(parent);
    } else {
        scan_impl(m_entries[idx].path);
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

    // Determine visible window
    const int n_visible = (kTexH - kTitleH) / kRowH;
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
            jstring js = env->NewStringUTF(m_entries[first + i].name.c_str());
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
