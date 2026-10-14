// The macOS IWatchBackend: FSEvents, with per-file events. The Win32 one is
// src/win32/Win32DirectoryWatcher.cpp and the Linux one InotifyWatchBackend.cpp; the contract all three
// meet is WatchBackend.hpp's.
//
// WHAT FSEvents DOES DIFFERENTLY, and what this file does about it:
//
//   * FLAGS ARE COALESCED. One event can say a path was created, modified and removed, if all three
//     happened inside the latency window. Which of them is the CURRENT truth is read from the disk:
//     a path that no longer exists was Removed, one that exists and was created was Added, anything
//     else was Modified. DirectoryWatcher's own debounce settles the rest.
//   * A RENAME IS TWO EVENTS, both flagged ItemRenamed, with consecutive event ids and no cookie. The
//     old name is the one that no longer exists. A pair is emitted as RenamedFrom then RenamedTo; a
//     lone renamed path is a move in (exists: Added) or out (gone: Removed) of the tree.
//   * PATHS ARE REAL PATHS. FSEvents reports /private/var/... for a root under /var (a symlink on
//     macOS), so the root is canonicalised first or nothing would ever be relative to it.
//   * IT IS ALWAYS RECURSIVE. A non-recursive watch drops events below the first level.
//   * ITS OWN OVERFLOW IS MustScanSubDirs (or the user/kernel "dropped" flags): a rescan, exactly as
//     ERROR_NOTIFY_ENUM_DIR is on Windows. RootChanged means the watched directory itself went away.
#include "../WatchBackend.hpp"
#include "aver/core/Log.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace aver::detail {
namespace {

constexpr usize kMaxQueued = 8192;   // Win32DirectoryWatcher.cpp's bound
constexpr usize kMaxKnown = 65536;

class FsEventsWatchBackend final : public IWatchBackend {
public:
    ~FsEventsWatchBackend() override { shutdown(); }

    bool died() const override { return died_.load(std::memory_order_acquire); }

    bool init(const std::string& root, bool recursive) {
        std::error_code ec;
        const auto canon = std::filesystem::canonical(root, ec);
        if (ec) {
            AVER_WARN("[Watcher] declined: cannot resolve '{}' ({})", root, ec.message());
            return false;
        }
        root_ = canon.string();
        recursive_ = recursive;

        CFStringRef path = CFStringCreateWithCString(nullptr, root_.c_str(), kCFStringEncodingUTF8);
        CFArrayRef paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1, &kCFTypeArrayCallBacks);
        FSEventStreamContext ctx{0, this, nullptr, nullptr, nullptr};
        stream_ = FSEventStreamCreate(nullptr, &FsEventsWatchBackend::callback, &ctx, paths,
                                      kFSEventStreamEventIdSinceNow, 0.05,
                                      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                          kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagUseCFTypes);
        CFRelease(paths);
        CFRelease(path);
        if (!stream_) {
            AVER_WARN("[Watcher] declined: FSEventStreamCreate failed for '{}'", root_);
            return false;
        }
        queueDispatch_ = dispatch_queue_create("org.vectoric.aver.watcher", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream_, queueDispatch_);
        if (!FSEventStreamStart(stream_)) {
            AVER_WARN("[Watcher] declined: FSEventStreamStart failed for '{}'", root_);
            shutdown();
            return false;
        }
        return true;
    }

    bool drain(std::vector<RawFileEvent>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty()) {
            out.insert(out.end(), std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
            queue_.clear();
        }
        const bool over = overflow_;
        overflow_ = false;
        return over;
    }

private:
    static void callback(ConstFSEventStreamRef, void* info, size_t count, void* eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<FsEventsWatchBackend*>(info);
        auto* paths = static_cast<CFArrayRef>(eventPaths);
        std::vector<std::string> abs(count);
        for (size_t i = 0; i < count; ++i) {
            auto* s = static_cast<CFStringRef>(CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
            char buf[4096];
            if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) abs[i] = buf;
        }
        self->handleBatch(abs, flags, count);
    }

    // Root-relative and '/'-separated, or empty for the root itself and anything outside it.
    std::string relative(const std::string& abs) const {
        if (abs.size() <= root_.size() || abs.compare(0, root_.size(), root_) != 0 || abs[root_.size()] != '/') return {};
        std::string rel = abs.substr(root_.size() + 1);
        if (!recursive_ && rel.find('/') != std::string::npos) return {};
        return rel;
    }

    void handleBatch(const std::vector<std::string>& abs, const FSEventStreamEventFlags flags[], size_t count) {
        if (died()) return;
        std::vector<RawFileEvent> batch;
        const auto now = WatchClock::now();
        for (size_t i = 0; i < count; ++i) {
            const FSEventStreamEventFlags f = flags[i];
            if (f & kFSEventStreamEventFlagRootChanged) { noteExit("the watched root itself was deleted or moved"); return; }
            if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                     kFSEventStreamEventFlagKernelDropped)) {
                flagOverflow("FSEvents dropped events");
                return;
            }
            const std::string rel = relative(abs[i]);
            if (rel.empty()) continue;
            std::error_code ec;
            const bool exists = std::filesystem::exists(std::filesystem::symlink_status(abs[i], ec));

            if (f & kFSEventStreamEventFlagItemRenamed) {
                // The next event a rename partner? Old name gone, new name present.
                if (i + 1 < count && (flags[i + 1] & kFSEventStreamEventFlagItemRenamed)) {
                    const std::string nextRel = relative(abs[i + 1]);
                    const bool nextExists = std::filesystem::exists(std::filesystem::symlink_status(abs[i + 1], ec));
                    if (!nextRel.empty() && !exists && nextExists) {
                        batch.push_back(RawFileEvent{RawChange::RenamedFrom, rel, now});
                        batch.push_back(RawFileEvent{RawChange::RenamedTo, nextRel, now});
                        known_.erase(rel);
                        remember(nextRel);
                        ++i;
                        continue;
                    }
                }
                batch.push_back(RawFileEvent{exists ? RawChange::Added : RawChange::Removed, rel, now});
                if (exists) remember(rel); else known_.erase(rel);
                continue;
            }
            if (!exists) {
                known_.erase(rel);
                if (f & kFSEventStreamEventFlagItemRemoved) batch.push_back(RawFileEvent{RawChange::Removed, rel, now});
            } else if (f & kFSEventStreamEventFlagItemCreated) {
                // ItemCreated stays set on later events for the same path, so only a path not seen before
                // (or one removed and made again) is an Added; a known one was rewritten.
                if (known_.count(rel) == 0 || (f & kFSEventStreamEventFlagItemRemoved)) {
                    remember(rel);
                    batch.push_back(RawFileEvent{RawChange::Added, rel, now});
                    if (f & kFSEventStreamEventFlagItemModified) batch.push_back(RawFileEvent{RawChange::Modified, rel, now});
                } else if (!(f & kFSEventStreamEventFlagItemIsDir)) {
                    batch.push_back(RawFileEvent{RawChange::Modified, rel, now});
                }
            } else if (f & (kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemInodeMetaMod)) {
                if (!(f & kFSEventStreamEventFlagItemIsDir)) batch.push_back(RawFileEvent{RawChange::Modified, rel, now});
            }
        }
        if (batch.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (overflow_) return;
            if (queue_.size() + batch.size() <= kMaxQueued) {
                queue_.insert(queue_.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
            } else {
                queue_.clear();
                if (!overflow_) AVER_WARN("[Watcher] lost track of '{}' (the frame side stopped draining); a rescan is required", root_);
                overflow_ = true;
            }
        }
        if (const int kill = killAfterBatches(); kill > 0 && ++batches_ >= kill) {
            AVER_WARN("[Watcher] AVER_WATCHER_KILL_AFTER={}: stopping the stream as a failure would", kill);
            noteExit("a simulated stream failure");
        }
    }

    // Dispatch queue only. Bounded: past the cap it is dropped, and a rewrite then reads as Created,
    // which the header allows.
    void remember(const std::string& rel) {
        if (known_.size() >= kMaxKnown) known_.clear();
        known_.insert(rel);
    }

    void flagOverflow(const char* why) {
        known_.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        if (!overflow_) AVER_WARN("[Watcher] lost track of '{}' ({}); a rescan is required", root_, why);
        overflow_ = true;
    }

    void noteExit(const char* why) {
        if (died_.exchange(true, std::memory_order_acq_rel)) return;
        AVER_WARN("[Watcher] '{}' stopped watching: {}. Hot reload is over for this session unless the "
                  "caller re-arms.", root_, why);
    }

    static int killAfterBatches() {
        const char* v = std::getenv("AVER_WATCHER_KILL_AFTER");
        return v && *v ? std::atoi(v) : 0;
    }

    void shutdown() {
        if (stream_) {
            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
        if (queueDispatch_) {
            // Every callback already queued has run once this returns, so none can touch `this` later.
            dispatch_sync_f(queueDispatch_, nullptr, [](void*) {});
            dispatch_release(queueDispatch_);
            queueDispatch_ = nullptr;
        }
    }

    std::string root_;
    bool recursive_ = true;
    FSEventStreamRef stream_ = nullptr;
    dispatch_queue_t queueDispatch_ = nullptr;
    int batches_ = 0;   // dispatch queue only
    std::unordered_set<std::string> known_;   // paths already reported as Added; dispatch queue only
    std::mutex mutex_;
    std::vector<RawFileEvent> queue_;
    bool overflow_ = false;
    std::atomic<bool> died_{false};
};

} // namespace

std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool recursive) {
    auto backend = std::make_unique<FsEventsWatchBackend>();
    if (!backend->init(root, recursive)) return nullptr;
    return backend;
}

} // namespace aver::detail
