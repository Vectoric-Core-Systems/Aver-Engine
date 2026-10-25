// PhysicsProfile.cpp -- the collector behind aver_phys_profile_* (physics_world_abi.h) and the sink AverProfileHook.cpp calls.
// See PhysicsProfile.hpp for how the pieces fit. Without JPH_EXTERNAL_PROFILE this file is the "no hook" answers only.
#include "PhysicsProfile.hpp"

#include "aver/core/CpuTiming.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/physics/physics_world_abi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace aver::physics::profile {

#ifdef JPH_EXTERNAL_PROFILE

namespace {

constexpr int kMaxDepth = 64;          // deeper scopes are counted and ignored
constexpr int kMaxNodes = 1024;        // per thread
constexpr int kTableSize = 4096;       // power of two, more than twice kMaxNodes
constexpr int kWindowSteps = 60;       // fixed steps per published window
constexpr int kFirstPublishSteps = 5;  // the first table appears early so a short run still shows something

struct Node {
    const char* name = nullptr;
    int32_t parent = -1;
    uint64_t incl = 0, excl = 0;
    uint32_t calls = 0;
};
struct Frame {
    int32_t node = -1;
    uint64_t start = 0, child = 0;
};

// One thread's scope tree. The owning thread takes the flag around every begin and end (uncontended: one exchange); the
// merge takes it to read and clear the counters, so a worker that is still finishing a scope cannot be torn.
struct ThreadAcc {
    std::atomic_flag lock;
    std::atomic<bool> dead{false};
    std::thread::id owner;
    std::vector<Node> nodes;
    std::vector<int32_t> table;      // open addressing over (parent, name pointer): node index or -1
    std::vector<int32_t> rowOf;      // merge side: the global row of each node
    uint32_t rowEpoch = 0;
    Frame stack[kMaxDepth];
    int depth = 0, overflow = 0;
    ThreadAcc() : table(kTableSize, -1) { nodes.reserve(kMaxNodes); }
    void acquire() { while (lock.test_and_set(std::memory_order_acquire)) std::this_thread::yield(); }
    void release() { lock.clear(std::memory_order_release); }
};

std::mutex g_regMutex;
std::vector<ThreadAcc*> g_threads;     // never freed; a thread that exits marks its record dead and the next new thread reuses it

struct TlsHolder {
    ThreadAcc* acc = nullptr;
    ~TlsHolder() {
        if (!acc) return;
        acc->acquire();
        acc->depth = 0;
        acc->overflow = 0;
        acc->release();
        acc->dead.store(true, std::memory_order_release);
    }
};
thread_local TlsHolder t_holder;

ThreadAcc* threadAcc() {
    if (t_holder.acc) return t_holder.acc;
    std::lock_guard<std::mutex> lk(g_regMutex);
    for (ThreadAcc* a : g_threads) {
        bool expect = true;
        if (a->dead.compare_exchange_strong(expect, false)) {
            a->owner = std::this_thread::get_id();
            t_holder.acc = a;
            return a;
        }
    }
    ThreadAcc* a = new ThreadAcc;
    a->owner = std::this_thread::get_id();
    g_threads.push_back(a);
    t_holder.acc = a;
    return a;
}

uint32_t slotOf(const char* name, int32_t parent) {
    const uint64_t x = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(name)) * 0x9E3779B97F4A7C15ull) ^
                       (static_cast<uint64_t>(parent + 1) * 0xC2B2AE3D27D4EB4Full);
    return static_cast<uint32_t>(x >> 40) & (kTableSize - 1);
}

int32_t findNode(ThreadAcc& a, int32_t parent, const char* name) {
    uint32_t i = slotOf(name, parent);
    for (int probe = 0; probe < kTableSize; ++probe, i = (i + 1) & (kTableSize - 1)) {
        const int32_t n = a.table[i];
        if (n < 0) {
            if (static_cast<int>(a.nodes.size()) >= kMaxNodes) return -1;
            Node nd;
            nd.name = name;
            nd.parent = parent;
            a.nodes.push_back(nd);
            a.table[i] = static_cast<int32_t>(a.nodes.size() - 1);
            return a.table[i];
        }
        if (a.nodes[static_cast<size_t>(n)].name == name && a.nodes[static_cast<size_t>(n)].parent == parent) return n;
    }
    return -1;
}

void sinkBegin(const char* name, uint32_t, uint8_t*) {
    ThreadAcc& a = *threadAcc();
    a.acquire();
    if (a.depth >= kMaxDepth) {
        ++a.overflow;
        a.release();
        return;
    }
    const int32_t parent = a.depth > 0 ? a.stack[a.depth - 1].node : -1;
    Frame& f = a.stack[a.depth++];
    f.node = findNode(a, parent, name);
    f.child = 0;
    f.start = ::aver::detail::cpuReadTicks();
    a.release();
}

void sinkEnd(uint8_t*) {
    const uint64_t now = ::aver::detail::cpuReadTicks();   // before the flag: waiting for it must not be billed to the scope
    ThreadAcc& a = *threadAcc();
    a.acquire();
    if (a.overflow > 0) {
        --a.overflow;
    } else if (a.depth > 0) {
        const Frame f = a.stack[--a.depth];
        const uint64_t ticks = now >= f.start ? now - f.start : 0;
        if (f.node >= 0) {
            Node& n = a.nodes[static_cast<size_t>(f.node)];
            n.incl += ticks;
            n.excl += ticks - (f.child < ticks ? f.child : ticks);
            ++n.calls;
        }
        if (a.depth > 0) a.stack[a.depth - 1].child += ticks;
    }
    a.release();
}

const Sink kSink{&sinkBegin, &sinkEnd};

// ---- the merged table ------------------------------------------------------------------------------------------

struct GRow {
    std::string name;
    int32_t parent = -1;
    bool synthetic = false;          // "Worker jobs": no time of its own, the sum of its children
    uint64_t callIncl = 0;           // this call's, over threads (scratch)
    uint64_t winIncl = 0, winExcl = 0, winCalls = 0;
    double maxMs = 0.0;
};

std::atomic<bool> g_enabled{false};
std::vector<GRow> g_rows;            // main thread only
uint32_t g_epoch = 1;
int g_winSteps = 0;
int32_t g_workersRow = -1;
std::thread::id g_mainThread;
std::mutex g_pubMutex;
std::vector<Row> g_published;

double ticksToMs(uint64_t t) {
    const double ns = ::aver::detail::cpuNsPerTick > 0.0 ? ::aver::detail::cpuNsPerTick : 1.0;
    return static_cast<double>(t) * ns * 1.0e-6;
}

int32_t findOrAddRow(int32_t parent, const char* name, bool synthetic = false) {
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].parent == parent && g_rows[i].name == name) return static_cast<int32_t>(i);
    GRow r;
    r.name = name;
    r.parent = parent;
    r.synthetic = synthetic;
    g_rows.push_back(std::move(r));
    return static_cast<int32_t>(g_rows.size() - 1);
}

int32_t resolveRow(ThreadAcc& a, int32_t idx, bool isMain) {
    int32_t& cached = a.rowOf[static_cast<size_t>(idx)];
    if (cached >= 0) return cached;
    const Node& n = a.nodes[static_cast<size_t>(idx)];
    int32_t parentRow = -1;
    if (n.parent >= 0) {
        parentRow = resolveRow(a, n.parent, isMain);
    } else if (!isMain) {
        // A worker thread's top-level scopes are Jolt jobs; they hang under one synthetic row instead of cluttering the root.
        if (g_workersRow < 0 || g_workersRow >= static_cast<int32_t>(g_rows.size())) g_workersRow = findOrAddRow(-1, "Worker jobs", true);
        parentRow = g_workersRow;
    }
    cached = findOrAddRow(parentRow, n.name);
    return cached;
}

void clearThreadCounters() {
    for (ThreadAcc* a : g_threads) {
        a->acquire();
        for (Node& n : a->nodes) { n.incl = 0; n.excl = 0; n.calls = 0; }
        a->release();
    }
}

void publish() {
    const size_t n = g_rows.size();
    std::vector<uint8_t> active(n, 0);
    for (size_t i = 0; i < n; ++i) if (g_rows[i].winCalls > 0) active[i] = 1;
    // A synthetic row is shown when any child is, and carries the children's sum.
    std::vector<uint64_t> synthIncl(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (!active[i]) continue;
        for (int32_t p = g_rows[i].parent; p >= 0 && g_rows[static_cast<size_t>(p)].synthetic; p = g_rows[static_cast<size_t>(p)].parent) {
            active[static_cast<size_t>(p)] = 1;
            if (g_rows[i].parent == p) synthIncl[static_cast<size_t>(p)] += g_rows[i].winIncl;
        }
    }
    const double steps = g_winSteps > 0 ? static_cast<double>(g_winSteps) : 1.0;

    std::vector<Row> out;
    out.reserve(n);
    // Depth first, parents before children, siblings in the order they were first seen.
    std::vector<int32_t> pubIndex(n, -1);
    struct Item { int32_t row; int32_t parentPub; int32_t depth; };
    std::vector<Item> stack;
    for (size_t i = n; i-- > 0;)
        if (active[i] && g_rows[i].parent < 0) stack.push_back({static_cast<int32_t>(i), -1, 0});
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        const GRow& g = g_rows[static_cast<size_t>(it.row)];
        Row r;
        r.name = g.name;
        const uint64_t incl = g.synthetic ? synthIncl[static_cast<size_t>(it.row)] : g.winIncl;
        r.inclusiveMs = ticksToMs(incl) / steps;
        r.exclusiveMs = g.synthetic ? 0.0 : ticksToMs(g.winExcl) / steps;
        r.maxMs = g.maxMs;
        r.callsPerStep = static_cast<double>(g.winCalls) / steps;
        r.parent = it.parentPub;
        r.depth = it.depth;
        pubIndex[static_cast<size_t>(it.row)] = static_cast<int32_t>(out.size());
        out.push_back(std::move(r));
        const int32_t me = pubIndex[static_cast<size_t>(it.row)];
        for (size_t c = n; c-- > 0;)
            if (active[c] && g_rows[c].parent == it.row) stack.push_back({static_cast<int32_t>(c), me, it.depth + 1});
    }
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        g_published = std::move(out);
    }
    for (GRow& g : g_rows) { g.winIncl = 0; g.winExcl = 0; g.winCalls = 0; g.maxMs = 0.0; }
    g_winSteps = 0;
}

} // namespace

bool compiled() { return true; }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void reset() {
    std::lock_guard<std::mutex> reg(g_regMutex);
    g_rows.clear();
    g_workersRow = -1;
    g_winSteps = 0;
    ++g_epoch;
    clearThreadCounters();
    std::lock_guard<std::mutex> lk(g_pubMutex);
    g_published.clear();
}

bool setEnabled(bool on) {
    if (on) {
        if (enabled()) return true;
        ::aver::detail::cpuEnsureCalibrated();
        g_mainThread = std::this_thread::get_id();
        reset();
        g_sink.store(&kSink, std::memory_order_release);
        g_enabled.store(true, std::memory_order_relaxed);
    } else {
        g_sink.store(nullptr, std::memory_order_release);
        g_enabled.store(false, std::memory_order_relaxed);
    }
    return true;
}

void mergeCall(int fixedSteps) {
    if (!enabled() || fixedSteps <= 0) return;
    g_mainThread = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> reg(g_regMutex);
        for (ThreadAcc* a : g_threads) {
            a->acquire();
            if (a->rowEpoch != g_epoch) {
                a->rowOf.assign(a->nodes.size(), -1);
                a->rowEpoch = g_epoch;
            } else if (a->rowOf.size() < a->nodes.size()) {
                a->rowOf.resize(a->nodes.size(), -1);
            }
            const bool isMain = a->owner == g_mainThread;
            for (size_t i = 0; i < a->nodes.size(); ++i) {
                Node& nd = a->nodes[i];
                if (nd.calls == 0) continue;
                GRow& g = g_rows[static_cast<size_t>(resolveRow(*a, static_cast<int32_t>(i), isMain))];
                g.callIncl += nd.incl;
                g.winIncl += nd.incl;
                g.winExcl += nd.excl;
                g.winCalls += nd.calls;
                nd.incl = 0;
                nd.excl = 0;
                nd.calls = 0;
            }
            a->release();
        }
    }
    const double steps = static_cast<double>(fixedSteps);
    for (GRow& g : g_rows) {
        if (g.callIncl > 0) {
            const double ms = ticksToMs(g.callIncl) / steps;
            if (ms > g.maxMs) g.maxMs = ms;
            g.callIncl = 0;
        }
    }
    g_winSteps += fixedSteps;
    bool firstTable;
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        firstTable = g_published.empty();
    }
    if (g_winSteps >= kWindowSteps || (firstTable && g_winSteps >= kFirstPublishSteps)) publish();
}

int rowCount() {
    std::lock_guard<std::mutex> lk(g_pubMutex);
    return static_cast<int>(g_published.size());
}

bool getRow(int index, Row& out) {
    std::lock_guard<std::mutex> lk(g_pubMutex);
    if (index < 0 || index >= static_cast<int>(g_published.size())) return false;
    out = g_published[static_cast<size_t>(index)];
    return true;
}

std::string topSummary() {
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        rows = g_published;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.exclusiveMs > b.exclusiveMs; });
    std::string s;
    for (size_t i = 0; i < rows.size() && i < 3; ++i) {
        if (rows[i].exclusiveMs <= 0.0) break;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%s %.2f ms", s.empty() ? "" : " | ", rows[i].name.c_str(), rows[i].exclusiveMs);
        s += buf;
    }
    return s;
}

#else // no hook in this build

bool compiled() { return false; }
bool enabled() { return false; }
bool setEnabled(bool) { return false; }
void reset() {}
void mergeCall(int) {}
int rowCount() { return 0; }
bool getRow(int, Row&) { return false; }
std::string topSummary() { return {}; }

#endif

} // namespace aver::physics::profile

extern "C" {

int32_t aver_phys_profile_enable(int32_t on) {
    using namespace aver::physics;
    if (!profile::compiled()) {
        aver::setAbiError(aver::AbiError::Unsupported);
        return 0;
    }
    profile::setEnabled(on != 0);
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_profile_is_enabled(void) { return aver::physics::profile::enabled() ? 1 : 0; }

int32_t aver_phys_profile_count(void) { return aver::physics::profile::rowCount(); }

int32_t aver_phys_profile_get(int32_t index, uint8_t* outNameUtf8, int32_t nameCap, double* outValues, int32_t* outParentDepth) {
    aver::physics::profile::Row r;
    if (!aver::physics::profile::getRow(index, r)) {
        aver::setAbiError(aver::AbiError::OutOfRange);
        return 0;
    }
    if (outNameUtf8 && nameCap > 0) {
        const size_t n = std::min(r.name.size(), static_cast<size_t>(nameCap - 1));
        std::memcpy(outNameUtf8, r.name.data(), n);
        outNameUtf8[n] = 0;
    }
    if (outValues) {
        outValues[0] = r.inclusiveMs;
        outValues[1] = r.exclusiveMs;
        outValues[2] = r.maxMs;
        outValues[3] = r.callsPerStep;
    }
    if (outParentDepth) {
        outParentDepth[0] = r.parent;
        outParentDepth[1] = r.depth;
    }
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_profile_reset(void) {
    aver::physics::profile::reset();
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_profile_summary(uint8_t* outUtf8, int32_t cap) {
    const std::string s = aver::physics::profile::topSummary();
    const int32_t need = static_cast<int32_t>(s.size()) + 1;
    if (outUtf8 && cap > 0) {
        const int32_t n = need <= cap ? need : cap;
        std::memcpy(outUtf8, s.c_str(), static_cast<size_t>(n));
        outUtf8[n - 1] = 0;
    }
    return need;
}

} // extern "C"
