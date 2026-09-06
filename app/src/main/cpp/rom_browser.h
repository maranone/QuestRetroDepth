#pragma once
#include <GLES3/gl3.h>
#include <jni.h>
#include <cstdint>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace qrd {

struct RomEntry {
    std::string name;   // source filename used for paths/artwork keys
    std::string display_name; // friendly database label, when one is available
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

    // Store/load the persistent recent-ROM history under the app settings directory.
    void set_recent_store(const std::string& settings_dir);
    void record_recent(const std::string& rom_path);
    void clear_recent();

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
    void                set_hovered_index(int index) {
        if (m_entries.empty()) { m_hovered = 0; return; }
        m_hovered = std::max(0, std::min(index, (int)m_entries.size() - 1));
    }
    const std::string&  hovered_path()   const;
    bool                hovered_is_dir() const;
    int                 count()          const { return (int)m_entries.size(); }
    bool                empty()          const { return m_entries.empty(); }
    // Matches the ROM shelf grid capacity (9 rows x 7 columns) so a full
    // shelf's worth of entries is available at once and page-scroll jumps by
    // exactly one shelf's worth. The flat 2D text-list panel (kTexH/kRowH)
    // this fallback used to derive its row count from is only ever shown
    // when the shelf isn't (rom_preview disabled), so drawing more name rows
    // than fit on that fixed-height bitmap is harmless — they're simply
    // clipped off the bottom.
    int                 visible_rows()   const {
        return kShelfGridCount;
    }

    // Returns count of items currently being rendered (for highlight calculation)
    int                 visible_count()   const { return m_visible_count; }
    std::vector<std::string> visible_rom_paths() const;
    std::vector<RomEntry> visible_entries() const;

    // Full current-directory listing (not just the scrolled-into-view window
    // visible_entries() returns) — for a searchable/filterable list that needs
    // to match against every entry, not just whichever ~63 happen to be
    // scrolled into the shelf/flat-panel's view. Read-only; index into this is
    // a valid argument to set_hovered_index().
    const std::vector<RomEntry>& entries() const { return m_entries; }

    // Resolves the directory enter_hovered() would navigate into, without
    // mutating browser state. Returns empty for a non-directory entry or a
    // virtual node (Recent) that has no real folder to pre-cache.
    std::string peek_hovered_target_dir() const;

    // Lists ROM file paths directly inside `dir` (non-recursive), using the
    // same extension whitelist as scan_impl. Used to pre-queue thumbnail
    // caching for an entire folder before navigating into it.
    std::vector<std::string> list_dir_rom_paths(const std::string& dir) const;

    // Walks `root` and every subdirectory beneath it (no depth limit) and
    // returns every supported ROM file found, for the Library search box —
    // entries()/scan_impl() only ever cover one directory at a time, so a
    // search that should span the whole library needs its own walk. Read-
    // only, doesn't touch m_entries/m_current_dir/scroll/hover state.
    std::vector<RomEntry> scan_recursive(const std::string& root) const;

    // The library's top-level folder — the natural default root for
    // scan_recursive() when searching "everything", not just the current dir.
    const std::string& root_dir() const { return m_root_dir; }

    // Returns the scroll offset (first visible index)
    int                 scroll_offset()    const { return m_scroll; }

    // Returns true if there's more content above/below
    bool                has_more_up()    const { return m_scroll > 0; }
    bool                has_more_down()  const { return m_scroll + m_visible_count < (int)m_entries.size(); }

    // GL texture showing the current panel state.
    GLuint texture() const { return m_tex; }
    bool   dirty()   const { return m_dirty; }
    int    bitmap_generation() const { return m_bitmap_generation; }
    int    bitmap_width() const { return kTexW; }
    int    bitmap_height() const { return kTexH; }
    const std::vector<uint8_t>& bitmap_rgba() const { return m_bitmap_rgba; }

    // Rebuild the panel texture. Must be called from the GL thread.
    // vm/activity are used to call the Kotlin rendering method.
    void rebuild_texture(JavaVM* vm, jobject activity);

    // Free the GL texture (call from GL thread, e.g. on shutdown).
    void destroy_texture();

    static constexpr int kTexW  = 1536;
    static constexpr int kTexH  = 1536;
    static constexpr int kRowH  = 72;  // pixels per row (2×)
    static constexpr int kTitleH = 88; // pixels for title bar (2×)
    static constexpr int kShelfGridCount = 63; // 7 rows x 9 columns

private:
    void scan_impl(const std::string& dir);
    void scan_recent_impl();
    void load_recent();
    void save_recent() const;
    void upload_pixels(const jint* pixels);

    std::vector<RomEntry> m_entries;
    std::vector<std::pair<std::int64_t, std::string>> m_recent;
    std::string m_root_dir;
    std::string m_current_dir;
    std::string m_recent_store_path;
    bool m_recent_mode = false;
    int m_hovered = 0;
    int m_scroll  = 0; // index of first visible row
    int m_visible_count = 0; // number of items rendered in current view

    GLuint m_tex   = 0;
    bool   m_dirty = true;
    int    m_bitmap_generation = 0;
    std::vector<uint8_t> m_bitmap_rgba;
};

} // namespace qrd
