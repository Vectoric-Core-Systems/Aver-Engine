#include "aver/core/TraceCapture.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

namespace aver::trace {

namespace detail { std::atomic<bool> g_capturing{false}; }

namespace {

struct State {
    std::mutex m;                       // guards everything below except the ring slots themselves
    std::vector<Event> ring;
    std::atomic<u64> head{0};           // total events recorded (slot = head % ring.size())
    u64 originUs = 0;
    u32 framesWanted = 0;
    u32 framesDone = 0;
    bool armed = false;
    bool running = false;
    u64 frameStartUs = 0;
    std::string path;
    std::string lastPath;
    std::map<u32, std::string> trackNames;
    std::deque<std::string> names;      // interned strings; deque keeps pointers stable
    std::map<std::string, const char*> nameIndex;
};

State& st() { static State s; return s; }

u32 threadTrack() {
    static std::atomic<u32> next{1};
    thread_local u32 id = next.fetch_add(1);
    return id;
}

void appendEscaped(std::string& out, const char* s) {
    for (; s && *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += static_cast<char>(c);
        }
    }
}

// Caller holds s.m. Returns whether a capture was armed or running.
bool finishLocked(State& s) {
    detail::g_capturing.store(false, std::memory_order_relaxed);
    const bool wasActive = s.armed || s.running;
    s.armed = false;
    s.running = false;
    return wasActive;
}

}  // namespace

u64 detail::nowUs() {
    using C = std::chrono::steady_clock;
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(C::now().time_since_epoch()).count());
}

void detail::record(const char* name, const char* cat, u64 absStartUs, u64 absEndUs) {
    addEvent(name, cat, threadTrack(), absStartUs, absEndUs);
}

void addEvent(const char* name, const char* cat, u32 track, u64 absStartUs, u64 absEndUs) {
    if (!capturing()) return;
    State& s = st();
    Event e;
    e.name = name; e.cat = cat; e.track = track;
    e.tsUs = absStartUs >= s.originUs ? absStartUs - s.originUs : 0;
    e.durUs = absEndUs > absStartUs ? absEndUs - absStartUs : 0;
    e.frame = s.framesDone;
    const u64 slot = s.head.fetch_add(1, std::memory_order_relaxed);
    s.ring[slot % s.ring.size()] = e;
}

bool begin(u32 frames, const std::string& path, u32 ringCapacity) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.armed || s.running) return false;
    s.framesWanted = std::clamp<u32>(frames, 1u, 100000u);
    s.path = path;
    s.ring.assign(std::max<u32>(ringCapacity, 256u), Event{});
    s.head.store(0);
    s.framesDone = 0;
    s.armed = true;
    return true;
}

bool armed() { State& s = st(); std::lock_guard<std::mutex> lk(s.m); return s.armed; }
u32 framesRecorded() { State& s = st(); std::lock_guard<std::mutex> lk(s.m); return s.framesDone; }
std::string lastWrittenPath() { State& s = st(); std::lock_guard<std::mutex> lk(s.m); return s.lastPath; }

void beginFrame() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.armed) {
        s.armed = false;
        s.running = true;
        s.originUs = detail::nowUs();
        detail::g_capturing.store(true, std::memory_order_relaxed);
    }
    if (s.running) s.frameStartUs = detail::nowUs();
}

void endFrame() {
    State& s = st();
    std::string path;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (!s.running) return;
        const u64 now = detail::nowUs();
        Event e;
        e.name = "Frame"; e.cat = "frame"; e.track = kTrackFrames;
        e.tsUs = s.frameStartUs >= s.originUs ? s.frameStartUs - s.originUs : 0;
        e.durUs = now - s.frameStartUs;
        e.frame = s.framesDone;
        s.ring[s.head.fetch_add(1) % s.ring.size()] = e;
        if (++s.framesDone < s.framesWanted) return;
        finishLocked(s);
        path = s.path;
    }
    std::string err;
    if (!writeChromeJson(path, &err)) AVER_ERROR("[Trace] could not write '{}': {}", path, err);
    else AVER_INFO("[Trace] wrote {} frames to {}", framesRecorded(), path);
}

bool stop() {
    State& s = st();
    std::string path;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (!finishLocked(s)) return false;
        path = s.path;
    }
    std::string err;
    if (!writeChromeJson(path, &err)) AVER_ERROR("[Trace] could not write '{}': {}", path, err);
    else AVER_INFO("[Trace] wrote {} frames to {}", framesRecorded(), path);
    return true;
}

void setThreadName(const char* name) {
    State& s = st();
    const u32 id = threadTrack();
    std::lock_guard<std::mutex> lk(s.m);
    s.trackNames.emplace(id, name);
}

const char* internName(const std::string& str) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.nameIndex.find(str);
    if (it != s.nameIndex.end()) return it->second;
    s.names.push_back(str);
    return s.nameIndex.emplace(str, s.names.back().c_str()).first->second;
}

void addGpuReport(const std::vector<GpuSpan>& spans, u64 absFrameStartUs) {
    if (!capturing() || spans.empty()) return;
    {
        State& s = st();
        std::lock_guard<std::mutex> lk(s.m);
        s.trackNames[kTrackGpu] = "GPU (frame-averaged passes, laid out in sequence)";
    }
    constexpr u32 kNone = 0xFFFFFFFFu;
    const u32 n = static_cast<u32>(spans.size());
    std::vector<std::vector<u32>> kids(n);
    std::vector<u32> top;
    for (u32 i = 0; i < n; ++i) {
        if (spans[i].parent != kNone && spans[i].parent < n && spans[i].parent != i) kids[spans[i].parent].push_back(i);
        else top.push_back(i);
    }
    std::vector<bool> done(n, false);
    auto place = [&](auto&& self, u32 i, double startMs) -> void {
        if (done[i]) return;
        done[i] = true;
        const double ms = std::max(0.0, spans[i].ms);
        const u64 a = absFrameStartUs + static_cast<u64>(std::max(0.0, startMs) * 1000.0);
        addEvent(internName(spans[i].label), "gpu", kTrackGpu, a, a + static_cast<u64>(ms * 1000.0));
        double c = startMs;
        for (u32 k : kids[i]) { self(self, k, c); c += std::max(0.0, spans[k].ms); }
    };
    double cursor = 0.0;
    for (u32 i : top) { place(place, i, cursor); cursor += std::max(0.0, spans[i].ms); }
}

std::string toChromeJson() {
    State& s = st();
    std::vector<Event> evs;
    std::map<u32, std::string> tracks;
    bool wrapped = false;
    {
        std::lock_guard<std::mutex> lk(s.m);
        const u64 total = s.head.load();
        const u64 cap = s.ring.size();
        wrapped = total > cap;
        const u64 count = std::min(total, cap);
        const u64 first = total - count;
        evs.reserve(static_cast<size_t>(count));
        for (u64 i = first; i < total; ++i) evs.push_back(s.ring[static_cast<size_t>(i % cap)]);
        tracks = s.trackNames;
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) { return a.tsUs < b.tsUs; });

    std::string o = "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"ringWrapped\":";
    o += wrapped ? "true" : "false";
    o += "},\"traceEvents\":[";
    bool firstItem = true;
    auto sep = [&] { if (!firstItem) o += ","; firstItem = false; };
    tracks.emplace(kTrackFrames, "Frames");
    for (const auto& [tid, name] : tracks) {
        sep();
        o += "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" + std::to_string(tid) + ",\"args\":{\"name\":\"";
        appendEscaped(o, name.c_str());
        o += "\"}}";
    }
    for (const Event& e : evs) {
        sep();
        o += "{\"name\":\"";
        appendEscaped(o, e.name);
        o += "\",\"cat\":\"";
        appendEscaped(o, e.cat ? e.cat : "cpu");
        o += "\",\"ph\":\"X\",\"pid\":1,\"tid\":" + std::to_string(e.track) + ",\"ts\":" + std::to_string(e.tsUs) +
             ",\"dur\":" + std::to_string(e.durUs) + ",\"args\":{\"frame\":" + std::to_string(e.frame) + "}}";
    }
    o += "]}";
    return o;
}

bool writeChromeJson(const std::string& path, std::string* err) {
    const std::string json = toChromeJson();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot open for writing"; return false; }
    const bool ok = std::fwrite(json.data(), 1, json.size(), f) == json.size();
    std::fclose(f);
    if (!ok) { if (err) *err = "short write"; return false; }
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    s.lastPath = path;
    return true;
}

}  // namespace aver::trace
