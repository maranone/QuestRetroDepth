#include "rom_preview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <zlib.h>
#include <android/log.h>

#define LOG_TAG "RomPreview"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace qrd {
namespace {

// --- Crash barrier around m_capture() -----------------------------------
//
// m_capture() runs a real emulator core's load/step path on unvetted,
// possibly-foreign ROM data (wrong system routed by folder/extension
// guessing, a corrupt file, a MAME set the wrong driver was pointed at,
// ...). A core can SIGSEGV on garbage input -- that's a native fault, not a
// C++ exception, so no try/catch can stop it, and left alone it takes the
// whole app down (this is exactly what happened: a Neo Geo romset routed
// into PicoDrive's Z80 core segfaulted in Cz80_Exec, killing the process
// from a background thumbnail job).
//
// The fix is a sigsetjmp/siglongjmp barrier: install handlers for the
// fault signals a broken core can raise, sigsetjmp() immediately before
// each m_capture() call, and have the handler siglongjmp() back out to
// there instead of letting the crash proceed. The worker thread then just
// treats that job as a failed capture (same as any other m_capture()
// failure) and moves on to the next one.
//
// Real caveat, not swept under the rug: a signal handler can only stop the
// crash from taking down the *process* -- it cannot undo memory corruption
// the faulting core already caused (a scribbled heap, a torn malloc arena).
// The recovered worker thread keeps running on whatever state survived.
// This is still the right trade for what this call site actually is
// (passive background thumbnailing of arbitrary files, not the live
// gameplay path), and is the same approach real-world
// frontends/thumbnailers use for exactly this scenario: one bad file marks
// itself failed and gets skipped forever after, instead of crash-looping
// the whole app on every launch.
thread_local sigjmp_buf g_capture_jmp_buf;
thread_local volatile sig_atomic_t g_capture_guarded = 0;
thread_local std::mutex* g_capture_mutexes[4] = {};

void release_capture_mutexes_after_fault() {
    // Unlock in reverse acquisition order. This runs after siglongjmp has
    // returned to run_capture_guarded(), so it is no longer inside the signal
    // handler itself. The pointers are deliberately TLS: only the worker
    // thread can own these preview-transition locks.
    for (int i = 3; i >= 0; --i) {
        if (g_capture_mutexes[i]) {
            g_capture_mutexes[i]->unlock();
            g_capture_mutexes[i] = nullptr;
        }
    }
}

void capture_crash_handler(int sig) {
    if (g_capture_guarded) {
        siglongjmp(g_capture_jmp_buf, sig);
    }
    // Not inside a guarded capture call (e.g. crash on another thread) --
    // restore the default handler and re-raise so it's still a real crash
    // with a normal tombstone, not silently swallowed.
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_capture_crash_handler() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    struct sigaction sa{};
    sa.sa_handler = capture_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: capture_crash_handler never returns normally anyway
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) {
        sigaction(sig, &sa, nullptr);
    }
}

// Runs fn() with the crash barrier armed. Returns false (and never calls
// fn()'s side effects again) if fn() raised one of the guarded signals.
template <typename Fn>
bool run_capture_guarded(Fn&& fn) {
    install_capture_crash_handler();
    g_capture_guarded = 1;
    const int fault = sigsetjmp(g_capture_jmp_buf, 1);
    if (fault != 0) {
        g_capture_guarded = 0;
        release_capture_mutexes_after_fault();
        LOGE("worker_loop: capture crashed with signal %d, recovered", fault);
        return false;
    }
    fn();
    g_capture_guarded = 0;
    return true;
}

constexpr uint32_t kMagic = 0x33505251u; // QRP3
constexpr uint32_t kVersion = 5; // v5 invalidates MAME thumbnails made with the old R/B ordering
constexpr int kMaxDimension = 192;

template <typename T>
bool write_value(std::ofstream& f, const T& value) {
    f.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return !!f;
}

template <typename T>
bool read_value(std::ifstream& f, T& value) {
    f.read(reinterpret_cast<char*>(&value), sizeof(T));
    return !!f;
}

uint64_t fnv1a(const void* data, size_t size, uint64_t hash = 1469598103934665603ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

RomPreviewLayer downsample(const RomPreviewLayer& src) {
    RomPreviewLayer dst;
    if (src.width <= 0 || src.height <= 0 || src.rgba.empty()) return dst;
    const float scale = std::min(1.0f, (float)kMaxDimension / (float)std::max(src.width, src.height));
    dst.width = std::max(1, (int)std::lround(src.width * scale));
    dst.height = std::max(1, (int)std::lround(src.height * scale));
    dst.rgba.resize((size_t)dst.width * dst.height * 4u);
    if (!src.depth_map.empty()) dst.depth_map.resize((size_t)dst.width * dst.height);
    for (int y = 0; y < dst.height; ++y) {
        const int sy = std::min(src.height - 1, (int)((y + 0.5f) / scale));
        for (int x = 0; x < dst.width; ++x) {
            const int sx = std::min(src.width - 1, (int)((x + 0.5f) / scale));
            const size_t si = ((size_t)sy * src.width + sx);
            const size_t di = ((size_t)y * dst.width + x);
            if (si * 4u + 4u <= src.rgba.size())
                std::memcpy(dst.rgba.data() + di * 4u, src.rgba.data() + si * 4u, 4u);
            if (!dst.depth_map.empty() && si < src.depth_map.size()) dst.depth_map[di] = src.depth_map[si];
        }
    }
    return dst;
}

bool compress_block(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    uLongf bound = compressBound((uLong)input.size());
    output.resize((size_t)bound);
    const int rc = compress2(output.data(), &bound, input.data(), (uLong)input.size(), Z_BEST_SPEED);
    if (rc != Z_OK) return false;
    output.resize((size_t)bound);
    return true;
}

bool decompress_block(const std::vector<uint8_t>& input, size_t expected, std::vector<uint8_t>& output) {
    output.resize(expected);
    uLongf actual = (uLongf)expected;
    const int rc = uncompress(output.data(), &actual, input.data(), (uLong)input.size());
    if (rc != Z_OK || actual != expected) { output.clear(); return false; }
    return true;
}

} // namespace

void rom_preview_register_capture_mutex(std::mutex* mutex) {
    if (!mutex) return;
    for (std::mutex* held : g_capture_mutexes)
        if (held == mutex) return;
    for (std::mutex*& held : g_capture_mutexes) {
        if (!held) {
            held = mutex;
            return;
        }
    }
    LOGE("capture mutex registry full");
}

void rom_preview_unregister_capture_mutex(std::mutex* mutex) {
    if (!mutex) return;
    for (std::mutex*& held : g_capture_mutexes) {
        if (held == mutex) {
            held = nullptr;
            return;
        }
    }
}

RomPreviewManager::~RomPreviewManager() { stop(); }

void RomPreviewManager::configure(const std::string& settings_dir, RomPreviewCapture capture) {
    stop();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache_dir = settings_dir + "/rom_preview_cache";
    m_capture = std::move(capture);
    m_enabled = true;
    m_stop.store(false);
    m_worker = std::thread(&RomPreviewManager::worker_loop, this);
    LOGI("configure: cache_dir='%s' capture=%s", m_cache_dir.c_str(), m_capture ? "set" : "null");
}

void RomPreviewManager::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
    if (!enabled) { m_jobs.clear(); m_cancel_job.store(true); m_live_ready = false; m_live_path.clear(); }
}

bool RomPreviewManager::enabled() const { std::lock_guard<std::mutex> lock(m_mutex); return m_enabled; }

uint64_t RomPreviewManager::fingerprint_for(const std::string& path) const {
    struct stat st{};
    uint64_t hash = fnv1a(path.data(), path.size());
    if (stat(path.c_str(), &st) == 0) {
        hash = fnv1a(&st.st_size, sizeof(st.st_size), hash);
        hash = fnv1a(&st.st_mtime, sizeof(st.st_mtime), hash);
    }
    return hash;
}

bool RomPreviewManager::read_layers(std::ifstream& f, std::vector<RomPreviewLayer>& out, const std::string& path) const {
    uint32_t layer_count = 0;
    if (!read_value(f, layer_count)) return false;
    out.clear(); out.reserve(layer_count);
    for (uint32_t i = 0; i < layer_count; ++i) {
        RomPreviewLayer layer;
        uint32_t w = 0, h = 0, rgba_size = 0, depth_size = 0, raw_rgba = 0, raw_depth = 0;
        if (!read_value(f, w) || !read_value(f, h) || !read_value(f, raw_rgba) || !read_value(f, raw_depth) || !read_value(f, rgba_size) || !read_value(f, depth_size)) {
            LOGE("load_package: truncated layer header %u for '%s'", i, path.c_str());
            return false;
        }
        std::vector<uint8_t> rgba(rgba_size), depth(depth_size);
        if ((rgba_size && !f.read(reinterpret_cast<char*>(rgba.data()), rgba_size)) || (depth_size && !f.read(reinterpret_cast<char*>(depth.data()), depth_size))) {
            LOGE("load_package: truncated layer data %u for '%s'", i, path.c_str());
            return false;
        }
        layer.width = (int)w; layer.height = (int)h;
        if (!decompress_block(rgba, raw_rgba, layer.rgba) || !decompress_block(depth, raw_depth, layer.depth_map)) {
            LOGE("load_package: decompress failed for layer %u of '%s'", i, path.c_str());
            return false;
        }
        out.push_back(std::move(layer));
    }
    return true;
}

bool RomPreviewManager::write_layers(std::ofstream& f, const std::vector<RomPreviewLayer>& layers, const std::string& path) const {
    const uint32_t count = (uint32_t)layers.size();
    if (!write_value(f, count)) return false;
    for (const auto& layer : layers) {
        std::vector<uint8_t> rgba_z, depth_z;
        if (!compress_block(layer.rgba, rgba_z) || !compress_block(layer.depth_map, depth_z)) {
            LOGE("save_package: compress_block failed for '%s'", path.c_str());
            return false;
        }
        const uint32_t w = (uint32_t)layer.width, h = (uint32_t)layer.height;
        const uint32_t raw_rgba = (uint32_t)layer.rgba.size(), raw_depth = (uint32_t)layer.depth_map.size();
        const uint32_t rgba_size = (uint32_t)rgba_z.size(), depth_size = (uint32_t)depth_z.size();
        if (!write_value(f, w) || !write_value(f, h) || !write_value(f, raw_rgba) || !write_value(f, raw_depth) ||
            !write_value(f, rgba_size) || !write_value(f, depth_size)) return false;
        if ((rgba_size && !f.write(reinterpret_cast<const char*>(rgba_z.data()), rgba_size)) ||
            (depth_size && !f.write(reinterpret_cast<const char*>(depth_z.data()), depth_size))) return false;
    }
    return true;
}

bool RomPreviewManager::load_package(const std::string& path, uint64_t fingerprint, RomPreviewSnapshot& out) const {
    const std::string file = m_cache_dir + "/" + std::to_string(fingerprint) + ".qrp";
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0, version = 0;
    if (!read_value(f, magic) || !read_value(f, version) || magic != kMagic || version != kVersion) {
        LOGE("load_package: bad header for '%s' (magic/version mismatch)", path.c_str());
        return false;
    }
    uint64_t stored_fp = 0;
    if (!read_value(f, stored_fp) || stored_fp != fingerprint || !read_value(f, out.source_width) || !read_value(f, out.source_height)) {
        LOGE("load_package: bad fingerprint/dimensions for '%s'", path.c_str());
        return false;
    }
    if (!read_layers(f, out.layers, path)) return false;
    uint32_t extra_count = 0;
    if (!read_value(f, extra_count)) return false;
    out.extra_frames.clear(); out.extra_frames.reserve(extra_count);
    for (uint32_t i = 0; i < extra_count; ++i) {
        std::vector<RomPreviewLayer> frame;
        if (!read_layers(f, frame, path)) return false;
        out.extra_frames.push_back(std::move(frame));
    }
    return true;
}

bool RomPreviewManager::save_package(const std::string& path, uint64_t fingerprint, const RomPreviewSnapshot& snapshot) const {
    std::error_code ec;
    std::filesystem::create_directories(m_cache_dir, ec);
    if (ec) {
        LOGE("save_package: create_directories('%s') failed: %s", m_cache_dir.c_str(), ec.message().c_str());
        return false;
    }
    const std::string tmp = m_cache_dir + "/" + std::to_string(fingerprint) + ".tmp";
    const std::string file = m_cache_dir + "/" + std::to_string(fingerprint) + ".qrp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
        LOGE("save_package: failed to open '%s' for write (errno=%d)", tmp.c_str(), errno);
        return false;
    }
    if (!write_value(f, kMagic) || !write_value(f, kVersion) || !write_value(f, fingerprint) ||
        !write_value(f, snapshot.source_width) || !write_value(f, snapshot.source_height)) {
        LOGE("save_package: failed writing header for '%s'", path.c_str());
        return false;
    }
    if (!write_layers(f, snapshot.layers, path)) return false;
    const uint32_t extra_count = (uint32_t)snapshot.extra_frames.size();
    if (!write_value(f, extra_count)) return false;
    for (const auto& frame : snapshot.extra_frames) {
        if (!write_layers(f, frame, path)) return false;
    }
    f.close();
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        LOGE("save_package: rename to '%s' failed: %s", file.c_str(), ec.message().c_str());
        return false;
    }
    LOGI("save_package: wrote '%s' (%zu layers, %u extra frames) for '%s'",
         file.c_str(), snapshot.layers.size(), extra_count, path.c_str());
    return true;
}

void RomPreviewManager::enqueue_locked(const std::string& path, bool live) {
    if (!m_enabled || path.empty() || !m_capture) return;
    for (const auto& queued : m_jobs) {
        if (queued.path == path && queued.live == live) return;
    }
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if ((live && it->live) || (!live && it->path == path)) it = m_jobs.erase(it); else ++it;
    }
    // Live (hover) jobs jump the queue since they need to start immediately.
    // Background cache jobs are appended so they process in the same order
    // they were enqueued in (set_visible() enqueues them in folder-display
    // order), instead of being reversed.
    if (live) m_jobs.insert(m_jobs.begin(), Job{path, live, ++m_generation});
    else m_jobs.push_back(Job{path, live, ++m_generation});
    // A live preview must replace the current background capture. Cache jobs
    // are cancelled only when a live request actually needs to take priority;
    // clearing hover must not cancel the cache worker.
    //
    // m_force_cancel as well as m_cancel_job: with kMaxPreemptions == 0 every
    // background job is "protected" from its very first attempt and watches
    // only m_force_cancel, so setting m_cancel_job alone made this whole
    // jump-the-queue path dead code -- a live request sat behind the in-flight
    // job's entire multi-checkpoint window (50 simulated seconds of gameplay)
    // before the worker even looked at it. Starvation is not a risk here: the
    // folder gate (m_live_preview_gated) already reserves an uncontested
    // window for background caching, and a preempted job re-queues and retries.
    if (live) { m_cancel_job.store(true); m_force_cancel.store(true); }
}

void RomPreviewManager::set_visible(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_enabled) return;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        const uint64_t fp = fingerprint_for(path);
        auto cached = m_cache.find(path);
        if (cached == m_cache.end() || cached->second.fingerprint != fp) {
            RomPreviewSnapshot snapshot;
            if (load_package(path, fp, snapshot)) m_cache[path] = CacheEntry{std::move(snapshot), fp};
            else enqueue_locked(path, false);
        }
    }
}

void RomPreviewManager::request_live(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_enabled || path.empty()) return;
    if (m_live_path == path) return;
    m_live_path = path; m_live_ready = false; m_live_snapshot = {};
    enqueue_locked(path, true);
}

void RomPreviewManager::clear_pending_background_jobs() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Drops queued-but-not-yet-started jobs...
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (!it->live) it = m_jobs.erase(it); else ++it;
    }
    // ...and interrupts whatever the worker is capturing right now, even if
    // it's already immune to ordinary hover-preemption. A folder switch
    // means we no longer care about that ROM's progress; leaving it to run
    // to completion would block the new folder's jobs for its entire
    // capture window, which reads as the thumbnailer being stuck.
    m_cancel_job.store(true);
    m_force_cancel.store(true);
}

void RomPreviewManager::clear_live() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_live_path.clear();
    m_live_ready = false;
    m_live_snapshot = {};
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->live) it = m_jobs.erase(it); else ++it;
    }
    // A live job's capture loop runs `while (!cancel.load())` indefinitely (it's what
    // makes the hovered card keep playing), holding the shared backend the whole time.
    // Without this, hovering a ROM and then loading that SAME ROM (path unchanged, so
    // request_live() never fires to set this) left that loop running forever, and the
    // real load's own backend lock acquisition deadlocked waiting for it — seen as the
    // app "hanging" on load right after a hover. A live job is never cancel-protected
    // (see worker_loop's protected_from_cancel), so this always reaches it.
    m_cancel_job.store(true);
}

bool RomPreviewManager::get_cached(const std::string& path, RomPreviewSnapshot& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(path); if (it == m_cache.end()) return false;
    if (it->second.fingerprint != fingerprint_for(path)) return false;
    out = it->second.snapshot; return true;
}

bool RomPreviewManager::get_live(RomPreviewSnapshot& out) const { std::lock_guard<std::mutex> lock(m_mutex); if (!m_live_ready) return false; out = m_live_snapshot; return true; }
uint64_t RomPreviewManager::live_serial() const { std::lock_guard<std::mutex> lock(m_mutex); return m_live_serial; }
std::string RomPreviewManager::live_path() const { std::lock_guard<std::mutex> lock(m_mutex); return m_live_path; }
bool RomPreviewManager::get_progress(const std::string& path, int& percent) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (path.empty() || path != m_progress_path || m_progress_percent < 0) return false;
    percent = m_progress_percent;
    return true;
}

bool RomPreviewManager::is_settled(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(path);
    if (it != m_cache.end() && it->second.fingerprint == fingerprint_for(path)) return true;
    return m_failed.count(path) != 0;
}

void RomPreviewManager::worker_loop() {
    LOGI("worker_loop: started");
    // Once a background cache job has been preempted this many times by
    // incoming live-preview requests, it stops being cancellable and is
    // allowed to run to completion — otherwise a steady stream of hovers
    // (faster than a single capture takes) can starve it forever, since
    // every attempt gets cancelled within its first frame. 0 means a
    // background job is immune to hover-preemption from the very first
    // attempt (still interruptible by a folder switch, via m_force_cancel)
    // -- live-hover preview and background caching share ONE worker thread
    // and ONE global emulator backend (see g_backend/recreate_backend_locked
    // in questretrodepth_main.cpp), so they can never truly run in parallel
    // anyway; letting hover "jump the queue" only bought snappier hover
    // response back when captures were cheap/instant. Now that a capture is
    // real per-frame emulation work (slow for some cores, e.g. MAME), a
    // steady stream of hovers while scrolling a folder could otherwise
    // cancel every single background job before it ever reached a
    // checkpoint, so background caching visibly never made progress while
    // browsing.
    constexpr int kMaxPreemptions = 0;
    for (;;) {
        Job job;
        { std::lock_guard<std::mutex> lock(m_mutex); if (m_stop.load()) { LOGI("worker_loop: stopping"); return; } if (!m_jobs.empty()) { job = m_jobs.front(); m_jobs.erase(m_jobs.begin()); m_cancel_job.store(false); m_force_cancel.store(false); m_progress_path = job.live ? std::string() : job.path; m_progress_percent = job.live ? -1 : 0; } }
        if (job.path.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
        const bool protected_from_cancel = !job.live && job.preemptions >= kMaxPreemptions;
        LOGI("worker_loop: capturing '%s' live=%d preemptions=%d protected=%d",
             job.path.c_str(), job.live ? 1 : 0, job.preemptions, protected_from_cancel ? 1 : 0);
        RomPreviewSnapshot snapshot; std::string error;
        const std::string job_path = job.path;
        const bool job_live = job.live;
        auto publish = [this, job_path, job_live](const RomPreviewSnapshot& snap) {
            if (!job_live) return;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_live_path != job_path) return; // hover moved on; drop stale frame
            m_live_snapshot = snap;
            m_live_ready = true;
            ++m_live_serial;
        };
        auto progress = [this, job_path, job_live](int percent) {
            if (job_live) return;
            percent = std::max(0, std::min(100, percent));
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_progress_path == job_path) m_progress_percent = percent;
        };
        // A job immune to ordinary hover-preemption (see kMaxPreemptions
        // above) still must be interruptible when the user switches
        // folders — otherwise it keeps running on stale content and blocks
        // the new folder's jobs for its entire capture window. Protected
        // jobs watch m_force_cancel instead of m_cancel_job; ordinary jobs
        // watch m_cancel_job (which clear_pending_background_jobs() also
        // sets, so either path is interrupted by a folder switch).
        bool ok = false;
        if (m_capture) {
            const bool survived = run_capture_guarded([&] {
                ok = m_capture(job.path, snapshot, error,
                                protected_from_cancel ? m_force_cancel : m_cancel_job,
                                job.live, publish, progress);
            });
            if (!survived) {
                ok = false;
                error = "capture crashed (bad/foreign ROM data) -- recovered, skipping this file";
            }
        }
        const bool cancelled = protected_from_cancel ? m_force_cancel.load() : m_cancel_job.load();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_progress_path == job_path) {
                m_progress_path.clear();
                m_progress_percent = -1;
            }
        }
        if (cancelled) {
            LOGI("worker_loop: cancelled '%s'", job.path.c_str());
            // A live-preview request preempts the in-flight background cache
            // job so hovering feels instant, but the preempted ROM's card
            // must not be abandoned — otherwise browsing while hovering
            // silently starves the rest of the folder's thumbnails (only new
            // set_visible() calls would ever re-queue it). Re-queue it at the
            // back so it resumes once there's no more urgent work.
            if (!job.live) {
                std::lock_guard<std::mutex> lock(m_mutex);
                bool already_queued = false;
                for (const auto& q : m_jobs) {
                    if (!q.live && q.path == job.path) { already_queued = true; break; }
                }
                if (!already_queued && m_enabled) {
                    job.preemptions += 1;
                    m_jobs.push_back(job);
                }
            }
            continue;
        }
        if (!ok) {
            LOGE("worker_loop: capture failed for '%s': %s", job.path.c_str(), error.c_str());
            if (!job.live) { std::lock_guard<std::mutex> lock(m_mutex); m_failed.insert(job.path); }
            continue;
        }
        const uint64_t fp = fingerprint_for(job.path);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (job.live) {
            if (m_live_path == job.path) { m_live_snapshot = std::move(snapshot); m_live_ready = true; ++m_live_serial; LOGI("worker_loop: live preview ready for '%s'", job.path.c_str()); }
        } else {
            m_failed.erase(job.path);
            for (auto& layer : snapshot.layers) layer = downsample(layer);
            for (auto& frame : snapshot.extra_frames)
                for (auto& layer : frame) layer = downsample(layer);
            m_cache[job.path] = CacheEntry{snapshot, fp};
            if (save_package(job.path, fp, snapshot)) {
                LOGI("worker_loop: cached preview for '%s' (%zu layers)", job.path.c_str(), snapshot.layers.size());
            }
        }
    }
}

void RomPreviewManager::invalidate_all() {
    std::error_code ec;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
    m_failed.clear();
    std::filesystem::remove_all(m_cache_dir, ec);
    LOGI("invalidate_all: cleared in-memory cache and removed '%s' (ec=%s)",
         m_cache_dir.c_str(), ec ? ec.message().c_str() : "ok");
}

void RomPreviewManager::stop() {
    // Must also set m_force_cancel: worker_loop() marks a background job
    // "protected from cancel" (immune to plain m_cancel_job) once it's been
    // preempted by hover previews kMaxPreemptions times, so it can run to
    // completion instead of starving forever under a steady stream of
    // hovers. That job then ONLY watches m_force_cancel. Without setting it
    // here too, stop() (called synchronously on the render thread right
    // before loading a newly-selected ROM) could join() on a protected job
    // that keeps stepping/holding g_backend_mutex for its full multi-
    // checkpoint capture window -- observed as the app appearing to hang
    // instead of launching the ROM.
    m_stop.store(true); m_cancel_job.store(true); m_force_cancel.store(true);
    if (m_worker.joinable()) m_worker.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.clear();
    // Stopping a worker must also invalidate the last live snapshot. Otherwise
    // the next shelf session can treat the previous game's frame as the live
    // card even though its backend and worker are already gone.
    m_live_path.clear();
    m_live_snapshot = {};
    m_live_ready = false;
    m_progress_path.clear();
    m_progress_percent = -1;
}

void RomPreviewManager::clear_cache() {
    // Must NOT call stop() here: that permanently joins the worker thread
    // (configure() only ever starts it once, at app init), so background
    // caching would never run again for the rest of the session. Cancel
    // whatever job is in flight and drop the queue instead, same as
    // invalidate_all(), so the worker is free to immediately start
    // recaching everything from scratch.
    std::error_code ec;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.clear();
    m_cancel_job.store(true);
    m_cache.clear();
    m_failed.clear();
    m_live_snapshot = {};
    m_live_ready = false;
    std::filesystem::remove_all(m_cache_dir, ec);
    LOGI("clear_cache: cleared in-memory cache and removed '%s' (ec=%s)",
         m_cache_dir.c_str(), ec ? ec.message().c_str() : "ok");
}

} // namespace qrd
