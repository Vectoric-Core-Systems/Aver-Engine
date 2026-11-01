// Background texture preparation: a worker pool that runs the CPU half of an upload (cache read, or
// decode + mips + BC7 encode) so the render thread only ever copies a finished chain to the GPU.
#include "aver/assets/TextureUpload.hpp"

#include "TextureUploadDetail.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace aver::assets {

namespace {

enum class JobState : u8 { Queued, Running, Ready };

struct Job {
    std::string path;
    TextureUsage usage = TextureUsage::Colour;
    JobState state = JobState::Queued;
    TexturePriority priority = TexturePriority::Prefetch;
    u64 order = 0;               // arrival order: FIFO within a priority
    bool cancelled = false;      // set while Running; the result is discarded when it lands
    bool failed = false;
    std::string err;
    detail::PreparedTexture prepared;
    u64 bytes = 0;
};

struct Loader {
    std::mutex m;
    std::condition_variable cv;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    bool stop = false;   // never set: see loader()
    bool started = false;
    u32 workerCount = ~0u;       // ~0: choose from the machine
    u64 nextOrder = 0;
    u64 readyBytes = 0;
    u32 queued = 0, running = 0, ready = 0;
    std::atomic<u64> requested{0}, prepared{0}, cacheHits{0}, compressed{0}, failed{0}, uploaded{0};

    // A prefetch backlog may hold this much prepared data before workers stop starting new prefetch jobs; a
    // Demand job always runs.
    static constexpr u64 kPrefetchReadyCap = 768ull << 20;

    static std::string keyOf(const std::string& path, TextureUsage usage) {
        std::string k = path;
#ifdef _WIN32
        for (char& c : k) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
        k += '|';
        k += static_cast<char>('0' + static_cast<u32>(usage));
        return k;
    }

    // The queued job to start next: Demand before Prefetch, then arrival order. Caller holds m.
    std::shared_ptr<Job> pickLocked(bool respectCap) {
        std::shared_ptr<Job> best;
        for (auto& [k, j] : jobs) {
            if (j->state != JobState::Queued) continue;
            if (respectCap && j->priority == TexturePriority::Prefetch && readyBytes >= kPrefetchReadyCap) continue;
            if (!best || j->priority < best->priority || (j->priority == best->priority && j->order < best->order))
                best = j;
        }
        return best;
    }

    // Runs one job's CPU work; the lock is not held.
    void execute(const std::shared_ptr<Job>& j) {
        detail::PreparedTexture p;
        std::string err;
        const bool ok = detail::prepareTexture(j->path, j->usage, p, &err);
        std::lock_guard<std::mutex> l(m);
        --running;
        if (j->cancelled) {                       // a project or level change while it ran
            for (auto it = jobs.begin(); it != jobs.end(); ++it)
                if (it->second == j) { jobs.erase(it); break; }
            cv.notify_all();
            return;
        }
        ++prepared;
        if (ok) {
            if (p.fromCache) ++cacheHits; else if (p.bc7) ++compressed;
            j->bytes = p.info.bytes;
            j->prepared = std::move(p);
            readyBytes += j->bytes;
        } else {
            ++failed;
            j->failed = true;
            j->err = std::move(err);
        }
        j->state = JobState::Ready;
        ++ready;
        cv.notify_all();
    }

    void run() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // the render thread keeps its core
#endif
        for (;;) {
            std::shared_ptr<Job> j;
            {
                std::unique_lock<std::mutex> l(m);
                for (;;) {
                    if (stop) return;
                    j = pickLocked(true);
                    if (j) break;
                    cv.wait(l);
                }
                j->state = JobState::Running;
                --queued;
                ++running;
            }
            execute(j);
        }
    }

    void startLocked() {
        if (started) return;
        started = true;
        u32 n = workerCount;
        if (n == ~0u) {
            const u32 hw = std::max(2u, std::thread::hardware_concurrency());
            n = std::clamp(hw - 2u, 1u, 4u);
        }
        for (u32 i = 0; i < n; ++i) std::thread([this] { run(); }).detach();
    }
};

// Never destroyed: a worker mid-encode at exit must not find the statics it uses (the cache directory, the held
// map) gone, and joining it would hold the process open for the length of a BC7 encode. The OS ends the thread; the
// cache writes are atomic, so a file half-written at that moment is never seen.
Loader& loader() {
    static Loader& l = *new Loader();
    return l;
}

} // namespace

void setTextureWorkerCount(u32 count) {
    Loader& l = loader();
    std::lock_guard<std::mutex> lock(l.m);
    if (!l.started) l.workerCount = count;
}

TextureState requestTexture(const std::string& path, TextureUsage usage, TexturePriority priority, std::string* err) {
    if (path.empty()) {
        if (err) *err = "empty texture path";
        return TextureState::Failed;
    }
    Loader& l = loader();
    const std::string key = Loader::keyOf(path, usage);
    std::lock_guard<std::mutex> lock(l.m);
    const auto it = l.jobs.find(key);
    if (it == l.jobs.end()) {
        auto j = std::make_shared<Job>();
        j->path = path;
        j->usage = usage;
        j->priority = priority;
        j->order = ++l.nextOrder;
        l.jobs.emplace(key, std::move(j));
        ++l.queued;
        ++l.requested;
        l.startLocked();
        l.cv.notify_one();
        return TextureState::Pending;
    }
    Job& j = *it->second;
    j.cancelled = false;   // wanted again while a cancelled job was still running
    if (j.state == JobState::Ready) {
        if (!j.failed) return TextureState::Ready;
        if (err) *err = j.err;
        --l.ready;
        l.jobs.erase(it);   // reported once; the next request tries again
        return TextureState::Failed;
    }
    if (j.state == JobState::Queued && priority < j.priority) {
        j.priority = priority;
        j.order = ++l.nextOrder;
        l.cv.notify_one();
    }
    return TextureState::Pending;
}

rhi::TextureHandle takeTexture(rhi::IResourceFactory& res, const std::string& path, TextureUsage usage,
                               std::string* err, TextureUploadInfo* info) {
    Loader& l = loader();
    std::shared_ptr<Job> j;
    {
        std::lock_guard<std::mutex> lock(l.m);
        const auto it = l.jobs.find(Loader::keyOf(path, usage));
        if (it == l.jobs.end() || it->second->state != JobState::Ready || it->second->failed) {
            if (err) *err = "texture is not prepared: " + path;
            return 0;
        }
        j = it->second;
        l.jobs.erase(it);
        --l.ready;
        l.readyBytes -= j->bytes;
        l.cv.notify_all();   // room under the prefetch cap
    }
    const rhi::TextureHandle h = detail::uploadPrepared(res, path, j->prepared, err, info);
    if (!h && j->prepared.bc7) {
        // A BC7 chain the RHI refuses: the plain RGBA8 path, prepared here (rare, and the reason is logged).
        detail::PreparedTexture plain;
        if (detail::prepareTexture(path, usage, plain, err, false)) {
            const rhi::TextureHandle h2 = detail::uploadPrepared(res, path, plain, err, info);
            if (h2) ++l.uploaded;
            return h2;
        }
        return 0;
    }
    if (h) ++l.uploaded;
    return h;
}

void cancelTextureRequests() {
    Loader& l = loader();
    std::lock_guard<std::mutex> lock(l.m);
    for (auto it = l.jobs.begin(); it != l.jobs.end();) {
        Job& j = *it->second;
        if (j.state == JobState::Running) { j.cancelled = true; ++it; continue; }
        if (j.state == JobState::Queued) --l.queued; else --l.ready;
        it = l.jobs.erase(it);
    }
    l.readyBytes = 0;
    l.cv.notify_all();
}

TextureLoadStats textureLoadStats() {
    Loader& l = loader();
    TextureLoadStats s;
    s.requested = l.requested; s.prepared = l.prepared; s.cacheHits = l.cacheHits;
    s.compressed = l.compressed; s.failed = l.failed; s.uploaded = l.uploaded;
    std::lock_guard<std::mutex> lock(l.m);
    s.queued = l.queued; s.running = l.running; s.ready = l.ready; s.readyBytes = l.readyBytes;
    return s;
}

u32 runTextureJobsHere(u32 maxJobs) {
    Loader& l = loader();
    u32 ran = 0;
    while (ran < maxJobs) {
        std::shared_ptr<Job> j;
        {
            std::lock_guard<std::mutex> lock(l.m);
            j = l.pickLocked(false);
            if (!j) break;
            j->state = JobState::Running;
            --l.queued;
            ++l.running;
        }
        l.execute(j);
        ++ran;
    }
    return ran;
}

} // namespace aver::assets
