// DirectoryWatcher — the platform-neutral half: coalescing, rename pairing, and the frame-side
// drain. Nothing in this file knows which OS it is running on; see src/win32/ for the backend.

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

// A burst that touches more distinct paths than this is not a burst, it is a bulk operation
// (a branch checkout, an unzip). Reporting it path by path is both slow — this table is a linear
// scan, deliberately, because a real burst holds a handful of entries — and useless to a caller
// that will end up rebuilding its whole view anyway. Past this, say "rescan" instead.
//
// The bound is small on purpose. It was measured at 2048 first: creating 20,000 files under a
// 60 Hz poll never reached it, and instead spent 21 ms inside a single poll — a dropped frame,
// caused by the quadratic scan and by one existence check per path. 512 converts that same storm
// into one rescan request in well under a millisecond, which is both cheaper and more useful.
constexpr usize kMaxPending = 512;

// One path's accumulated state during the debounce window. Deliberately flags rather than a
// "latest action", because the whole point is that the actions are not independent: Removed
// followed by RenamedTo is one save, not a delete and an arrival.
struct Pending {
    std::string path;
    bool created = false;
    bool deleted = false;
    bool modified = false;
    bool renamedTo = false;
    // True when the rename's source was itself created inside this same window — i.e. it was a
    // temporary file, and this is an editor's atomic save rather than a user renaming something.
    bool fromTemporary = false;
    std::string oldPath;
    WatchClock::time_point first{};
    WatchClock::time_point last{};
};

} // namespace

struct DirectoryWatcher::Impl {
    std::string root;
    std::unique_ptr<detail::IWatchBackend> backend;
    std::vector<RawFileEvent> raw;
    std::vector<Pending> pending;
    u32 settleMs = kDefaultSettleMs;
    u32 maxHoldMs = kDefaultMaxHoldMs;

    // A RenamedFrom is meaningless alone and the OS emits its RenamedTo next, so it is held here
    // until the pair completes. It survives across drains because a pair can straddle two reads.
    bool haveRenameFrom = false;
    std::string renameFrom;
    WatchClock::time_point renameFromAt{};

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

    void dropEntry(const std::string& path, bool& wasCreatedHere) {
        wasCreatedHere = false;
        for (usize i = 0; i < pending.size(); ++i) {
            if (pending[i].path != path) continue;
            wasCreatedHere = pending[i].created;
            pending.erase(pending.begin() + (isize)i);
            return;
        }
    }

    // A RenamedFrom whose partner never arrived is just a disappearance.
    void settleOrphanRename() {
        if (!haveRenameFrom) return;
        haveRenameFrom = false;
        entryFor(renameFrom, renameFromAt).deleted = true;
    }

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

        // Anything else means the held RenamedFrom is not going to be paired.
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
                // No partner — treat it as an arrival, which is what it looks like from here.
                entryFor(e.path, e.at).created = true;
                return;
        }
    }

    // Turns one path's accumulated flags into at most one event. Returns false when the net effect
    // is nothing the caller ever needs to hear about.
    bool resolve(const Pending& p, FileEvent& out) const {
        const std::string full = root + "/" + p.path;
        const bool exists = fileExists(full);

        if (!exists) {
            // A file that appeared and vanished inside one window never existed as far as the
            // caller is concerned — this is the editor's temporary file, and reporting a Deleted
            // for a path nobody was ever told about is pure noise.
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

        // Removed-then-back is a safe save: the file the caller knows about changed.
        if (p.deleted) {
            out.kind = FileChange::Modified;
            out.path = p.path;
            out.oldPath.clear();
            return true;
        }

        // Arrived and stayed — either directly, or via a temporary renamed into place. See the
        // Created-vs-Modified note in the header for why this resolves towards Created.
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

bool DirectoryWatcher::start(const std::string& root, bool recursive) {
    stop();

    if (root.empty() || !directoryExists(root)) {
        AVER_WARN("[Watcher] declined: '{}' is not an existing directory", root);
        return false;
    }

    impl_->backend = detail::createWatchBackend(root, recursive);
    if (!impl_->backend) return false;  // the backend has already said why

    impl_->root = root;
    AVER_INFO("[Watcher] watching '{}'{}", root, recursive ? " (recursive)" : "");
    return true;
}

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

bool DirectoryWatcher::watching() const { return impl_->backend != nullptr; }
const std::string& DirectoryWatcher::root() const { return impl_->root; }

void DirectoryWatcher::setDebounce(u32 settleMs, u32 maxHoldMs) {
    impl_->settleMs = settleMs;
    impl_->maxHoldMs = std::max(maxHoldMs, settleMs);
}

u32 DirectoryWatcher::settleMs() const { return impl_->settleMs; }
u32 DirectoryWatcher::maxHoldMs() const { return impl_->maxHoldMs; }

bool DirectoryWatcher::poll(std::vector<FileEvent>& out) {
    Impl& m = *impl_;
    if (!m.backend) return false;

    m.raw.clear();
    bool rescan = m.backend->drain(m.raw);

    if (rescan) {
        // No detail survives an overflow, so anything half-accumulated describes a world that may
        // no longer exist. Drop it rather than emitting events the caller would have to unpick.
        m.pending.clear();
        m.haveRenameFrom = false;
        m.raw.clear();
        return true;
    }

    // The cap is tested INSIDE the fold, not after it. Folding the whole batch first and then
    // noticing it was too big is the expensive way to reach the same answer — the table is a
    // linear scan, so a 5000-record batch spends milliseconds building a table it is about to
    // throw away. Measured: 28 ms in one poll, i.e. a dropped frame, which is precisely the thing
    // this class promises not to do.
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

    // An unpaired RenamedFrom that has gone quiet is a deletion, not a pending pair.
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
