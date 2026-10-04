// DirectoryWatcher, platform-neutral half: coalescing, rename pairing, and the frame-side drain.

#include "aver/platform/DirectoryWatcher.hpp"

#include "WatchBackend.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>

namespace aver {
namespace {

using detail::RawChange;
using detail::RawFileEvent;
using detail::WatchClock;

constexpr u32 kDefaultSettleMs = 150;
constexpr u32 kDefaultMaxHoldMs = 1000;

// Past this many distinct pending paths, poll() asks for a rescan instead of reporting each one.
constexpr usize kMaxPending = 512;

// One path's accumulated state during the debounce window.
struct Pending {
    std::string path;
    bool created = false;
    bool deleted = false;
    bool modified = false;
    bool renamedTo = false;
    bool fromTemporary = false;   // the rename's source was created inside this same window
    std::string oldPath;
    WatchClock::time_point first{};
    WatchClock::time_point last{};
};

} // namespace

// The watcher's state: backend, pending table, debounce settings and the held rename source.
struct DirectoryWatcher::Impl {
    std::string root;
    std::unique_ptr<detail::IWatchBackend> backend;
    std::vector<RawFileEvent> raw;
    std::vector<Pending> pending;
    u32 settleMs = kDefaultSettleMs;
    u32 maxHoldMs = kDefaultMaxHoldMs;

    bool haveRenameFrom = false;
    std::string renameFrom;
    WatchClock::time_point renameFromAt{};

    // Returns the pending entry for `path`, creating it if new, and stamps its last-seen time.
    Pending& entryFor(const std::string& path, WatchClock::time_point at) {
        for (Pending& p : pending) {
            if (p.path == path) { p.last = at; return p; }
        }
        pending.push_back(Pending{});
        Pending& p = pending.back();
        p.path = path;
        p.first = at;
        p.last = at;
        return p;
    }

    // Removes `path` from the pending table, reporting whether it was created inside this window.
    void dropEntry(const std::string& path, bool& wasCreatedHere) {
        wasCreatedHere = false;
        for (usize i = 0; i < pending.size(); ++i) {
            if (pending[i].path != path) continue;
            wasCreatedHere = pending[i].created;
            pending.erase(pending.begin() + (isize)i);
            return;
        }
    }

    // Turns a held RenamedFrom whose partner never arrived into a deletion.
    void settleOrphanRename() {
        if (!haveRenameFrom) return;
        haveRenameFrom = false;
        entryFor(renameFrom, renameFromAt).deleted = true;
    }

    // Folds one raw record into the pending table.
    void fold(const RawFileEvent& e) {
        if (e.kind == RawChange::RenamedTo && haveRenameFrom) {
            haveRenameFrom = false;
            bool sourceWasCreatedInWindow = false;
            dropEntry(renameFrom, sourceWasCreatedInWindow);
            Pending& p = entryFor(e.path, e.at);
            p.renamedTo = true;
            p.oldPath = renameFrom;
            p.fromTemporary = sourceWasCreatedInWindow;
            return;
        }

        settleOrphanRename();

        switch (e.kind) {
            case RawChange::RenamedFrom:
                haveRenameFrom = true;
                renameFrom = e.path;
                renameFromAt = e.at;
                return;
            case RawChange::Added:      entryFor(e.path, e.at).created = true; return;
            case RawChange::Removed:    entryFor(e.path, e.at).deleted = true; return;
            case RawChange::Modified:   entryFor(e.path, e.at).modified = true; return;
            case RawChange::RenamedTo:
                entryFor(e.path, e.at).created = true;
                return;
        }
    }

    // Turns one path's accumulated flags into at most one event. Returns false when there is none.
    bool resolve(const Pending& p, FileEvent& out) const {
        const std::string full = root + "/" + p.path;
        const bool exists = fileExists(full);

        if (!exists) {
            if (p.created || p.fromTemporary) return false;
            out.kind = FileChange::Deleted;
            out.path = p.path;
            out.oldPath.clear();
            return true;
        }

        if (p.renamedTo && !p.fromTemporary && !p.deleted) {
            out.kind = FileChange::Renamed;
            out.path = p.path;
            out.oldPath = p.oldPath;
            return true;
        }

        if (p.deleted) {
            out.kind = FileChange::Modified;
            out.path = p.path;
            out.oldPath.clear();
            return true;
        }

        if (p.created || p.renamedTo) {
            out.kind = FileChange::Created;
            out.path = p.path;
            out.oldPath.clear();
            return true;
        }

        out.kind = FileChange::Modified;
        out.path = p.path;
        out.oldPath.clear();
        return true;
    }
};

DirectoryWatcher::DirectoryWatcher() : impl_(std::make_unique<Impl>()) {}
DirectoryWatcher::~DirectoryWatcher() { stop(); }

// Begins watching `root`. Returns false, having logged why, if it cannot be watched.
bool DirectoryWatcher::start(const std::string& root, bool recursive) {
    stop();

    if (root.empty() || !directoryExists(root)) {
        AVER_WARN("[Watcher] declined: '{}' is not an existing directory", root);
        return false;
    }

    impl_->backend = detail::createWatchBackend(root, recursive);
    if (!impl_->backend) return false;

    impl_->root = root;
    AVER_INFO("[Watcher] watching '{}'{}", root, recursive ? " (recursive)" : "");
    return true;
}

// Stops the watch and clears all accumulated state.
void DirectoryWatcher::stop() {
    if (impl_->backend) {
        impl_->backend.reset();
        AVER_INFO("[Watcher] stopped watching '{}'", impl_->root);
    }
    impl_->root.clear();
    impl_->raw.clear();
    impl_->pending.clear();
    impl_->haveRenameFrom = false;
}

// A watcher whose worker has died is NOT watching, whatever the backend pointer says. Callers use
// this to decide whether hot reload is live, and answering "yes" for a dead watch is how a stale
// editor session convinces someone their file did not save.
bool DirectoryWatcher::watching() const {
    return impl_->backend != nullptr && !impl_->backend->died();
}
const std::string& DirectoryWatcher::root() const { return impl_->root; }

// Sets the settle and maximum-hold windows, in milliseconds.
void DirectoryWatcher::setDebounce(u32 settleMs, u32 maxHoldMs) {
    impl_->settleMs = settleMs;
    impl_->maxHoldMs = std::max(maxHoldMs, settleMs);
}

u32 DirectoryWatcher::settleMs() const { return impl_->settleMs; }
u32 DirectoryWatcher::maxHoldMs() const { return impl_->maxHoldMs; }

// Appends every settled change to `out`. Returns true when the caller must rescan the tree itself.
bool DirectoryWatcher::poll(std::vector<FileEvent>& out) {
    Impl& m = *impl_;
    if (!m.backend) return false;

    m.raw.clear();
    bool rescan = m.backend->drain(m.raw);

    if (rescan) {
        m.pending.clear();
        m.haveRenameFrom = false;
        m.raw.clear();
        return true;
    }

    bool bulk = false;
    for (const RawFileEvent& e : m.raw) {
        if (m.pending.size() >= kMaxPending) { bulk = true; break; }
        m.fold(e);
    }

    if (bulk) {
        m.pending.clear();
        m.haveRenameFrom = false;
        AVER_WARN("[Watcher] more than {} paths changed at once in '{}'; asking for a rescan",
                  (u32)kMaxPending, m.root);
        return true;
    }

    const auto now = WatchClock::now();
    const auto settle = std::chrono::milliseconds(m.settleMs);
    const auto hold = std::chrono::milliseconds(m.maxHoldMs);

    if (m.haveRenameFrom && now - m.renameFromAt >= settle) m.settleOrphanRename();

    usize keep = 0;
    for (usize i = 0; i < m.pending.size(); ++i) {
        const Pending& p = m.pending[i];
        const bool quiet = (now - p.last) >= settle;
        const bool held = (now - p.first) >= hold;
        if (!quiet && !held) {
            if (keep != i) m.pending[keep] = std::move(m.pending[i]);
            ++keep;
            continue;
        }
        FileEvent ev;
        if (m.resolve(p, ev)) out.push_back(std::move(ev));
    }
    m.pending.resize(keep);

    return false;
}

} // namespace aver
