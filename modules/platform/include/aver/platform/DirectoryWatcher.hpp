#pragma once
#include "aver/core/Types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver {

// Watches a directory tree and reports what changed on disk, so the editor can notice a file that
// an IDE or a text editor wrote behind its back without being restarted.
//
// The interface is deliberately platform-neutral — no HANDLE, no OVERLAPPED, no wchar_t — because
// the Win32 backend (ReadDirectoryChangesW) is one implementation of it and not the shape of it.
//
// House rules this obeys, and why:
//   * It never blocks the frame. The OS notification API is inherently blocking, so the backend
//     owns a worker thread and `poll()` only drains what has already arrived.
//   * It DECLINES cleanly. `start()` on a missing or unwatchable directory logs once and returns
//     false; the caller carries on with no watch, exactly as it does with no project loaded.

// What happened to one path, after coalescing. One logical change produces exactly one of these.
enum class FileChange {
    Created,   // the path exists now and did not appear to before
    Modified,  // contents or metadata changed
    Deleted,   // the path is gone
    Renamed,   // `path` is the new name; `oldPath` is what it was called
};

struct FileEvent {
    FileChange kind = FileChange::Modified;

    // Relative to the watched root, with '/' separators. The OS hands these back relative to the
    // root already; they are normalised to '/' so a path is a stable map key regardless of which
    // separator the producer used, and Win32 accepts either when the caller joins it back onto
    // the root. Never absolute — the caller knows its own root.
    std::string path;

    // Renamed only; empty for every other kind.
    std::string oldPath;
};

// ---------------------------------------------------------------------------------------------
// A note on Created vs Modified, because it cannot be made exact and pretending otherwise is worse
// ---------------------------------------------------------------------------------------------
// A watcher has no memory of what was on disk before it started, and Windows reports an atomic
// "safe save" (write temp -> replace target -> rename) as a rename onto the target with no removal
// record. Coalescing that burst yields "this path exists now and the last thing that happened to
// it was an arrival", which is genuinely ambiguous between a new file and a replaced one.
//
// This class resolves the ambiguity towards `Created`, and a consumer that keys on path should
// treat `Created` for a path it already knows about as `Modified`. That direction is chosen
// because the opposite mistake — reporting `Modified` for a file the caller has never seen — makes
// a new file invisible, and an invisible new file is the failure this whole watcher exists to fix.
// ---------------------------------------------------------------------------------------------

class DirectoryWatcher {
public:
    DirectoryWatcher();
    ~DirectoryWatcher();
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    // Begins watching `root`. Returns false — having logged the reason once — if the directory
    // does not exist or the OS refuses the watch. A second start() stops the first.
    bool start(const std::string& root, bool recursive = true);
    void stop();

    bool watching() const;
    const std::string& root() const;

    // Drains every change that has SETTLED (see the debounce note below) into `out`, appending,
    // and returns immediately. Call once a frame from the main thread.
    //
    // Returns TRUE when the caller must RESCAN THE TREE ITSELF: the OS dropped notification
    // records because more changed than its buffer could hold, and Windows then tells us only
    // that something happened, never what. That is not a condition to log and swallow — the
    // watcher's whole contract is broken for that interval, so it is surfaced as a return value
    // the caller cannot accidentally ignore. Nothing is appended to `out` on such a call: the
    // half-accumulated state describes a world that may no longer exist, so it is dropped rather
    // than emitted alongside an instruction to rescan.
    [[nodiscard]] bool poll(std::vector<FileEvent>& out);

    // Debounce window, in milliseconds. `settleMs` is how long a path must go quiet before its
    // coalesced event is emitted; `maxHoldMs` caps how long a path that never goes quiet (a log
    // file being appended to) can be withheld. Both may be set before or after start().
    void setDebounce(u32 settleMs, u32 maxHoldMs);
    u32 settleMs() const;
    u32 maxHoldMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------------------------
// Why debouncing is not optional
// ---------------------------------------------------------------------------------------------
// One save from a real editor is a BURST, not an event. Visual Studio and VS Code both write to a
// temporary file, remove or replace the original, and rename the temporary into place; on top of
// that, watching both last-write time and size reports the same write twice. A watcher that
// forwards raw records reports three or four changes for one save, and the file may not even exist
// at the instant the first record arrives — so a consumer that reads the file on notification
// reads a half-written or absent one.
//
// Events are therefore accumulated per path and emitted once the path has been quiet for
// `settleMs`. The default is 150 ms: measured bursts from Notepad, Visual Studio and VS Code on
// this machine span under 20 ms end to end, so 150 ms clears them by an order of magnitude while
// staying well inside the ~250 ms at which a UI update stops feeling immediate. It is a settle
// timer rather than a fixed window from the first record, so a burst that keeps arriving keeps
// deferring — which is what `maxHoldMs` (default 1000 ms) exists to bound.
// ---------------------------------------------------------------------------------------------

} // namespace aver
