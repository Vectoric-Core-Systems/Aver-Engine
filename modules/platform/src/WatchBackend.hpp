#pragma once
// Internal to Aver.Platform: the per-OS directory-watch backend interface and its raw record type.

#include "aver/core/Types.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace aver::detail {

using WatchClock = std::chrono::steady_clock;

// What the OS said happened, uncoalesced and uninterpreted.
enum class RawChange {
    Added,
    Removed,
    Modified,
    RenamedFrom,  // the OLD name; the OS emits it immediately before RenamedTo
    RenamedTo,    // the NEW name; only meaningful paired with the RenamedFrom before it
};

// One raw notification record from the OS.
struct RawFileEvent {
    RawChange kind = RawChange::Modified;
    std::string path;              // relative to the watched root, '/'-separated
    WatchClock::time_point at{};   // stamped when the record was read
};

// A per-OS source of raw directory-change records.
class IWatchBackend {
public:
    virtual ~IWatchBackend() = default;

    // Appends everything queued since the last call to `out` without blocking. Returns true if the
    // OS dropped records, in which case the whole tree must be assumed changed.
    virtual bool drain(std::vector<RawFileEvent>& out) = 0;
};

// Creates a started backend for `root`, or nullptr if it cannot be watched, having logged why.
std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool recursive);

} // namespace aver::detail
