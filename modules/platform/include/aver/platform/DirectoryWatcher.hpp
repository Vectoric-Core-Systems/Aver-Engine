#pragma once
#include "aver/core/Types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver {

// What happened to one path, after coalescing. One logical change produces exactly one of these.
enum class FileChange {
    Created,
    Modified,
    Deleted,
    Renamed,
};

// One coalesced change to one path.
struct FileEvent {
    FileChange kind = FileChange::Modified;
    std::string path;      // relative to the watched root, '/'-separated
    std::string oldPath;   // Renamed only; empty otherwise
};

// Watches a directory tree and reports what changed on disk. Never blocks the frame.
// Created is reported for an arrival that may be a replacement: treat Created for a known path
// as Modified.
class DirectoryWatcher {
public:
    DirectoryWatcher();
    ~DirectoryWatcher();
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    // Begins watching `root`. Returns false, having logged why, if it cannot be watched.
    bool start(const std::string& root, bool recursive = true);
    // Stops the watch and drops all accumulated state.
    void stop();

    bool watching() const;
    const std::string& root() const;

    // Appends every settled change to `out` and returns without blocking. Call once a frame from
    // the main thread. Returns true when the caller must rescan the tree itself; `out` is untouched.
    [[nodiscard]] bool poll(std::vector<FileEvent>& out);

    // Sets the debounce window: quiet time before a path is emitted, and the cap on withholding it.
    void setDebounce(u32 settleMs, u32 maxHoldMs);
    u32 settleMs() const;
    u32 maxHoldMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aver
