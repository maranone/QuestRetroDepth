#pragma once
#include <GLES3/gl3.h>
#include <jni.h>
#include <string>
#include <vector>

namespace qrd {

struct RomEntry {
    std::string name;   // display filename
    std::string path;   // absolute path
    bool        is_dir; // true = subdirectory (or ".." back entry)
};

class RomBrowser {
public:
    RomBrowser() = default;
    ~RomBrowser() { destroy_texture(); }

    // Scan a directory for supported ROM/archive files and subdirectories.
    // Call with the root dir on first open; subsequent navigation updates current dir.
    void scan(const std::string& dir);

    // If the hovered entry is a directory (or ".."), navigate into it and return true.
    // If it is a ROM file, return false (caller should load it).
    bool enter_hovered();

    // Update the hovered row from panel UV coordinates [0,1].
    // u = horizontal, v = vertical (0 = top). Returns true if hover changed.
    bool set_hover_uv(float u, float v);

    // Scroll visible window by +/- rows.
    void scroll(int delta);
    void scroll_page(int pages);

    int                 hovered_index()  const { return m_hovered; }
    const std::string&  hovered_path()   const;
    bool                hovered_is_dir() const;
    int                 count()          const { return (int)m_entries.size(); }
    bool                empty()          const { return m_entries.empty(); }
    int                 visible_rows()   const {
        return (kTexH - kTitleH) / kRowH;
    }

    // Returns count of items currently being rendered (for highlight calculation)
    int                 visible_count()   const { return m_visible_count; }

    // Returns the scroll offset (first visible index)
    int                 scroll_offset()    const { return m_scroll; }

    // Returns true if there's more content above/below
    bool                has_more_up()    const { return m_scroll > 0; }
    bool                has_more_down()  const { return m_scroll + m_visible_count < (int)m_entries.size(); }

    // GL texture showing the current panel state.
    GLuint texture() const { return m_tex; }
    bool   dirty()   const { return m_dirty; }

    // Rebuild the panel texture. Must be called from the GL thread.
    // vm/activity are used to call the Kotlin rendering method.
    void rebuild_texture(JavaVM* vm, jobject activity);

    // Free the GL texture (call from GL thread, e.g. on shutdown).
    void destroy_texture();

    static constexpr int kTexW  = 1536;
    static constexpr int kTexH  = 1536;
    static constexpr int kRowH  = 72;  // pixels per row (2×)
    static constexpr int kTitleH = 88; // pixels for title bar (2×)

private:
    void scan_impl(const std::string& dir);
    void upload_pixels(const jint* pixels);

    std::vector<RomEntry> m_entries;
    std::string m_root_dir;
    std::string m_current_dir;
    int m_hovered = 0;
    int m_scroll  = 0; // index of first visible row
    int m_visible_count = 0; // number of items rendered in current view

    GLuint m_tex   = 0;
    bool   m_dirty = true;
};

} // namespace qrd
