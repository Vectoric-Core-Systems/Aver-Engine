#pragma once
// Internal to Aver.Platform. The split exists so that the coalescing policy — which is where all
// the subtlety lives and none of it is OS-specific — is written ONCE, in DirectoryWatcher.cpp,
// and a per-OS backend only has to turn native notifications into raw records.
//
// A backend reports RAW records: exactly what the OS said, in the order it said it, with no
// coalescing, no filtering and no interpretation.

#include "aver/core/Types.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace aver::detail {

using WatchClock = std::chrono::steady_clock;

enum class RawChange {
    Added,
    Removed,
    Modified,
    RenamedFrom,  // the OLD name of a rename; the OS emits it immediately before RenamedTo
    RenamedTo,    // the NEW name; only meaningful paired with the RenamedFrom before it
};

struct RawFileEvent {
    RawChange kind = RawChange::Modified;
    std::string path;              // relative to the watched root, '/'-separated
    WatchClock::time_point at{};   // stamped by the backend when the record was read
};

class IWatchBackend {
public:
    virtual ~IWatchBackend() = default;

    // Appends everything queued since the last call to `out` and returns without blocking.
    // Returns true if the OS dropped records — the caller must then assume the whole tree
    // changed, because Windows gives no detail whatsoever in that case.
    virtual bool drain(std::vector<RawFileEvent>& out) = 0;
};

// Returns nullptr if the directory cannot be watched, having logged why. `root` must exist.
std::unique_ptr<IWatchBackend> createWatchBackend(const std::string& root, bool recursive);

} // namespace aver::detail
