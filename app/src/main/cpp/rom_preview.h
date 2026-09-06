#pragma once

#include "emulator_backend.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qrd {

struct RomPreviewLayer {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> depth_map;
};

struct RomPreviewSnapshot {
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    std::vector<RomPreviewLayer> layers; // primary/first-shown frame
    // Additional sampled frames (background cache jobs only) for idle
    // cycling on the shelf, so an unhovered card isn't stuck on one frame
    // forever. Empty for live previews and any snapshot from an older
    // cache format.
    std::vector<std::vector<RomPreviewLayer>> extra_frames;
};

// Called off the XR thread. The callback must create/load a temporary backend,
// advance it to a representative frame, and return the captured per-layer data.
// `is_live` distinguishes a hover-triggered live preview (should stay fast, for
// responsiveness) from a background cache job (can afford a longer warmup to
// get past a game's boot/publisher-logo screen into actual gameplay, since it
// runs fully async while the user keeps browsing).
//
// `publish` lets a live capture push a running stream of frames while it
// keeps stepping the emulator instead of returning once — used so a hovered
// card shows actual looping gameplay rather than a single freeze-frame. It is
// ignored by background (non-live) captures, which just return one snapshot
// via `out` as before.
using RomPreviewPublish = std::function<void(const RomPreviewSnapshot&)>;
// Reports background-capture progress as an integer percentage. It is kept
// separate from `publish` because background jobs do not publish frames until
// the complete multi-checkpoint capture is ready.
using RomPreviewProgress = std::function<void(int percent)>;
using RomPreviewCapture = std::function<bool(const std::string& path,
                                             RomPreviewSnapshot& out,
                                             std::string& error_out,
                                             const std::atomic<bool>& cancel,
                                             bool is_live,
                                             const RomPreviewPublish& publish,
                                             const RomPreviewProgress& progress)>;

// A guarded preview capture may recover from a native core fault with
// siglongjmp, which bypasses normal C++ destructors. Preview code that holds
// shared mutexes while calling a core must register them here so the worker
// can release them before it continues to the next job.
void rom_preview_register_capture_mutex(std::mutex* mutex);
void rom_preview_unregister_capture_mutex(std::mutex* mutex);

class RomPreviewManager {
public:
    RomPreviewManager() = default;
    ~RomPreviewManager();

    void configure(const std::string& settings_dir, RomPreviewCapture capture);
    void set_enabled(bool enabled);
    bool enabled() const;

    // Starts low-priority generation for the supplied visible ROMs. Existing
    // packages are loaded immediately; only missing/stale packages are queued.
    void set_visible(const std::vector<std::string>& paths);
    void request_live(const std::string& path);
    void clear_live();

    // Drops any not-yet-started background cache jobs AND forcibly
    // interrupts whichever job is currently being captured (even one that's
    // become immune to hover-preemption — see kMaxPreemptions in
    // worker_loop()), instead of leaving it to run to completion. Call this
    // when switching folders: the previous folder's in-flight ROM would
    // otherwise keep the worker busy on stale content — for up to the full
    // multi-checkpoint capture window per the currently-hovered case — before
    // the new folder's jobs ever get a turn, which reads as the thumbnailer
    // being "stuck" on the folder you already left.
    void clear_pending_background_jobs();

    bool get_cached(const std::string& path, RomPreviewSnapshot& out) const;
    bool get_live(RomPreviewSnapshot& out) const;
    // Monotonic counter bumped once per published live frame. Lets the render
    // thread tell "the emulator produced a new frame" from "I'm redrawing the
    // same one", so it can skip re-uploading identical layer textures — the
    // live loop publishes at ~60Hz while the shelf redraws at display rate.
    // 0 means no live frame has been published.
    uint64_t live_serial() const;
    std::string live_path() const;
    bool get_progress(const std::string& path, int& percent) const;

    // True once `path` has either finished caching successfully or the
    // worker has given up on it permanently (load/capture failure). Lets a
    // caller wait for "all done" without hanging forever on a ROM whose
    // preview will never succeed.
    bool is_settled(const std::string& path) const;

    // Cancels work and joins the worker. Safe to call when leaving the browser.
    void stop();
    void clear_cache();

    // Development aid: drop every cached preview (memory + on-disk) without
    // touching the worker thread, so the next folder visit regenerates every
    // thumbnail from scratch — useful while iterating on the capture logic,
    // where a stale .qrp from an earlier build would otherwise hide whether
    // a fix actually changed anything. Unlike clear_cache(), safe to call
    // while a preview session is active.
    void invalidate_all();

private:
    struct Job { std::string path; bool live = false; uint64_t generation = 0; int preemptions = 0; };
    struct CacheEntry { RomPreviewSnapshot snapshot; uint64_t fingerprint = 0; };

    uint64_t fingerprint_for(const std::string& path) const;
    bool load_package(const std::string& path, uint64_t fingerprint, RomPreviewSnapshot& out) const;
    bool save_package(const std::string& path, uint64_t fingerprint, const RomPreviewSnapshot& snapshot) const;
    bool read_layers(std::ifstream& f, std::vector<RomPreviewLayer>& out, const std::string& path) const;
    bool write_layers(std::ofstream& f, const std::vector<RomPreviewLayer>& layers, const std::string& path) const;
    void worker_loop();
    void enqueue_locked(const std::string& path, bool live);

    mutable std::mutex m_mutex;
    std::string m_cache_dir;
    RomPreviewCapture m_capture;
    std::unordered_map<std::string, CacheEntry> m_cache;
    std::unordered_set<std::string> m_failed;
    std::vector<Job> m_jobs;
    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_cancel_job{false};
    // Interrupts a job even if it's become immune to ordinary hover-based
    // cancellation (see kMaxPreemptions in worker_loop()) — set by
    // clear_pending_background_jobs() on a folder switch, which needs to
    // move on regardless of that immunity.
    std::atomic<bool> m_force_cancel{false};
    bool m_enabled = true;
    uint64_t m_generation = 0;
    std::string m_live_path;
    RomPreviewSnapshot m_live_snapshot;
    bool m_live_ready = false;
    uint64_t m_live_serial = 0;
    std::string m_progress_path;
    int m_progress_percent = -1;
};

} // namespace qrd
