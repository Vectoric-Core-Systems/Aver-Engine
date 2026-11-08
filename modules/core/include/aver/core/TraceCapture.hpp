#pragma once
// Timeline capture: CPU scopes (and optional GPU spans) recorded per frame into a ring buffer and
// written as Chrome trace JSON, which chrome://tracing and Perfetto open directly.
//
// Idle cost is one relaxed atomic load per AVER_TRACE_SCOPE. A capture is armed with begin(N, path);
// recording starts at the next beginFrame() and the file is written when the Nth endFrame() lands.
// If more events arrive than the ring holds, the oldest are overwritten and the file notes it.
//
// Event names must be string literals (stored by pointer). Dynamic labels go through internName().
// Scopes may be opened from any thread; the writer runs on the thread that ends the last frame, so a
// scope still open on another thread at that moment may be dropped.
#include "aver/core/Types.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace aver::trace {

struct Event {
    const char* name = nullptr;
    const char* cat = nullptr;
    u32 track = 0;     // Chrome "tid"
    u64 tsUs = 0;      // microseconds since the capture began
    u64 durUs = 0;
    u32 frame = 0;     // frame index within the capture
};

inline constexpr u32 kTrackFrames = 0;     // one box per frame
inline constexpr u32 kTrackGpu = 1000;     // GPU spans, see addGpuReport

namespace detail {
extern std::atomic<bool> g_capturing;
u64 nowUs();                                        // absolute, monotonic
void record(const char* name, const char* cat, u64 absStartUs, u64 absEndUs);
}

inline bool capturing() { return detail::g_capturing.load(std::memory_order_relaxed); }

// Arms a capture of `frames` frames (clamped to 1..100000) written to `path` when it completes.
// Returns false if one is already armed or running.
bool begin(u32 frames, const std::string& path, u32 ringCapacity = 1u << 17);
// Ends a running or armed capture now, writing what was recorded. Returns false if nothing was active.
bool stop();
bool armed();                                       // begin() called, first frame not yet started
u32 framesRecorded();
// Path of the last file written, empty if none. Set once the write has finished.
std::string lastWrittenPath();

// Called once per frame by the main loop. Both are cheap no-ops when nothing is armed.
void beginFrame();
void endFrame();

// Names the calling thread's track in the output (the first call per thread wins).
void setThreadName(const char* name);
const char* internName(const std::string& s);       // stable pointer, lives for the process

// A complete event on an explicit track (absolute microseconds from detail::nowUs()).
void addEvent(const char* name, const char* cat, u32 track, u64 absStartUs, u64 absEndUs);

struct GpuSpan {
    std::string label;
    double ms = 0;
    u32 parent = 0xFFFFFFFFu;   // index into the same list, earlier than this span; 0xFFFFFFFF = top level
};
// The device reports per-pass times averaged over recent frames, not timestamps. They are laid out
// back to back from `absFrameStartUs` (children inside their parent) so the track reads as a
// per-frame budget, not as real GPU start times.
void addGpuReport(const std::vector<GpuSpan>& spans, u64 absFrameStartUs);

// Serialises what is recorded so far. Valid JSON at any point, including with nothing recorded.
std::string toChromeJson();
bool writeChromeJson(const std::string& path, std::string* err = nullptr);

class Scope {
public:
    explicit Scope(const char* name, const char* cat = "cpu") {
        if (capturing()) { name_ = name; cat_ = cat; t0_ = detail::nowUs(); }
    }
    ~Scope() { if (name_) detail::record(name_, cat_, t0_, detail::nowUs()); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    const char* name_ = nullptr;
    const char* cat_ = nullptr;
    u64 t0_ = 0;
};

}  // namespace aver::trace

#define AVER_TRACE_CAT2(a, b) a##b
#define AVER_TRACE_CAT(a, b) AVER_TRACE_CAT2(a, b)
#define AVER_TRACE_SCOPE(name) ::aver::trace::Scope AVER_TRACE_CAT(averTraceScope_, __LINE__)(name)
