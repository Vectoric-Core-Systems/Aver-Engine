// The fixed-rate worker: the same step() as the synchronous path, on its own thread, over a private
// copy of the cage. The game thread only ever reads snapshots; it never touches the worker cage.
//
// ONE mutex guards every cross-thread field and is held only for O(1) swaps and small copies; the
// snapshot arrays are filled outside it.
#pragma once

#include "aver/softbody/Cage.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace aver::softbody {

struct Snapshot {
    std::vector<Vec3>  positions;    // per particle (including duplicates made by tears)
    std::vector<u8>    triDead;
    std::vector<SplitEvent> splits;  // tears since the previous snapshot you took
    u64  serial     = 0;
    u32  stepCount  = 0;             // steps the worker has run since start()
    u32  brokenBeams = 0;
    bool settled    = false;         // the worker has gone idle
};

struct AsyncOptions {
    f32  hz = 64.0f;          // clamped to 15..240; the fixed step is 1/hz
    // false: step as fast as possible (tests, bakes). The step count and results do not change.
    bool realTime = true;
};

class AsyncSolver {
public:
    AsyncSolver() = default;
    ~AsyncSolver() { stop(); }
    AsyncSolver(const AsyncSolver&) = delete;
    AsyncSolver& operator=(const AsyncSolver&) = delete;

    // Copies `seed` (which must be build()-ed) and starts the thread. Commands queued before this
    // are kept and run first. False when already running or the seed is not built.
    bool start(const Cage& seed, const StepConfig& cfg, const AsyncOptions& opt = {});
    // Joins the thread. Safe to call twice.
    void stop();
    bool running() const { return running_.load(); }

    // Commands run at the top of the next step, in order, on the worker. Thread-safe.
    void setConfig(const StepConfig& cfg);
    void enqueueImpact(const Impact& im, const CrushParams& crush = {});
    void enqueueBreak(u32 beam);
    void enqueueMove(u32 particle, const Vec3& pos);   // a kinematic (pinned) particle
    void enqueueRepair();

    // O(1) swap of the latest snapshot into `out`. False when nothing is new since the last take.
    bool tryGetSnapshot(Snapshot& out);
    // Blocks until the worker has run every queued command and gone idle, or the timeout passes.
    bool waitSettled(u32 timeoutMs);

private:
    struct Command {
        enum class Kind : u8 { Impact, Break, Move, Repair };
        Kind kind = Kind::Impact;
        Impact impact;
        CrushParams crush;
        u32 index = 0;
        Vec3 pos;
    };

    void run();
    void push(const Command& c);
    void publish(bool settled);

    std::mutex mu_;
    std::condition_variable wake_;       // worker waits here (commands, stop, realtime pacing)
    std::condition_variable settledCv_;  // waitSettled waits here
    std::vector<Command> commands_;
    StepConfig cfg_;
    AsyncOptions opt_;
    bool stop_ = false;
    u64  enqueued_ = 0, consumed_ = 0;
    bool publishedSettled_ = false;
    Snapshot latest_;
    std::vector<SplitEvent> pendingSplits_;
    u64  taken_ = 0;

    Cage cage_;                          // worker-owned while running
    u32  stepCount_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace aver::softbody
