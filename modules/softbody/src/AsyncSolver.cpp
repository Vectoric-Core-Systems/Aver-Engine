#include "aver/softbody/AsyncSolver.hpp"

#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace aver::softbody {

namespace {
constexpr int kMaxStepsPerWake = 4;   // spiral-of-death clamp
}

bool AsyncSolver::start(const Cage& seed, const StepConfig& cfg, const AsyncOptions& opt) {
    if (running_.load() || !seed.built) return false;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        opt_ = opt;
        opt_.hz = std::clamp(opt_.hz, 15.0f, 240.0f);
        cfg_ = cfg;
        cfg_.dt = 1.0f / opt_.hz;
        stop_ = false;
        publishedSettled_ = false;
        latest_ = Snapshot{};
        pendingSplits_.clear();
        taken_ = 0;
    }
    cage_ = seed;
    cage_.splitLog.clear();
    stepCount_ = 0;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void AsyncSolver::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void AsyncSolver::push(const Command& c) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        commands_.push_back(c);
        ++enqueued_;
    }
    wake_.notify_all();
}

void AsyncSolver::setConfig(const StepConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
        cfg_.dt = 1.0f / std::clamp(opt_.hz, 15.0f, 240.0f);
    }
    wake_.notify_all();
}

void AsyncSolver::enqueueImpact(const Impact& im, const CrushParams& crush) {
    Command c;
    c.kind = Command::Kind::Impact;
    c.impact = im;
    c.crush = crush;
    push(c);
}

void AsyncSolver::enqueueBreak(u32 beam) {
    Command c;
    c.kind = Command::Kind::Break;
    c.index = beam;
    push(c);
}

void AsyncSolver::enqueueMove(u32 particle, const Vec3& pos) {
    Command c;
    c.kind = Command::Kind::Move;
    c.index = particle;
    c.pos = pos;
    push(c);
}

void AsyncSolver::enqueueRepair() {
    Command c;
    c.kind = Command::Kind::Repair;
    push(c);
}

bool AsyncSolver::tryGetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (latest_.serial == taken_) return false;
    out.positions.swap(latest_.positions);
    out.triDead.swap(latest_.triDead);
    out.splits = std::move(pendingSplits_);
    pendingSplits_.clear();
    out.serial = latest_.serial;
    out.stepCount = latest_.stepCount;
    out.brokenBeams = latest_.brokenBeams;
    out.settled = latest_.settled;
    taken_ = latest_.serial;
    return true;
}

bool AsyncSolver::waitSettled(u32 timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    return settledCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                               [this] { return publishedSettled_ && consumed_ == enqueued_; });
}

// Fills the scratch outside the lock, then swaps it in.
void AsyncSolver::publish(bool settled) {
    Snapshot scratch;
    scratch.positions.resize(cage_.particles.size());
    for (usize i = 0; i < cage_.particles.size(); ++i) scratch.positions[i] = cage_.particles[i].pos;
    scratch.triDead = cage_.triDead;
    scratch.stepCount = stepCount_;
    scratch.brokenBeams = brokenBeamCount(cage_);
    scratch.settled = settled;
    {
        std::lock_guard<std::mutex> lk(mu_);
        scratch.serial = latest_.serial + 1;
        latest_ = std::move(scratch);
        for (SplitEvent& ev : cage_.splitLog) pendingSplits_.push_back(std::move(ev));
        publishedSettled_ = settled;
    }
    cage_.splitLog.clear();
    if (settled) settledCv_.notify_all();
}

void AsyncSolver::run() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // never starve game/render
#endif
    using Clock = std::chrono::steady_clock;
    bool settled = false;
    double accumulator = 0.0;
    Clock::time_point last = Clock::now();

    for (;;) {
        std::vector<Command> cmds;
        StepConfig cfg;
        AsyncOptions opt;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (settled && commands_.empty() && !stop_) {
                wake_.wait(lk, [this] { return stop_ || !commands_.empty(); });
                last = Clock::now();
                accumulator = 0.0;
            }
            if (stop_) break;
            cmds.swap(commands_);
            consumed_ = enqueued_;
            if (!cmds.empty()) publishedSettled_ = false;
            cfg = cfg_;
            opt = opt_;
        }
        if (!cmds.empty()) settled = false;

        for (const Command& c : cmds) {
            switch (c.kind) {
            case Command::Kind::Impact: applyImpact(cage_, c.impact, c.crush); break;
            case Command::Kind::Break:  breakBeam(cage_, c.index); break;
            case Command::Kind::Move:
                if (c.index < cage_.particles.size()) {
                    cage_.particles[c.index].pos = c.pos;
                    cage_.particles[c.index].prev = c.pos;
                }
                break;
            case Command::Kind::Repair: repair(cage_); break;
            }
        }

        int steps = 1;
        if (opt.realTime) {
            const Clock::time_point now = Clock::now();
            accumulator += std::chrono::duration<double>(now - last).count();
            last = now;
            const double dt = cfg.dt;
            steps = std::min(static_cast<int>(accumulator / dt), kMaxStepsPerWake);
            accumulator -= steps * dt;
            accumulator = std::min(accumulator, dt * kMaxStepsPerWake);
            if (steps == 0) {
                std::unique_lock<std::mutex> lk(mu_);
                wake_.wait_for(lk, std::chrono::duration<double>(dt - accumulator),
                               [this] { return stop_ || !commands_.empty(); });
                continue;
            }
        }

        for (int s = 0; s < steps; ++s) {
            const StepResult r = step(cage_, cfg);
            ++stepCount_;
            if (r.settled) { settled = true; break; }
        }
        publish(settled);
    }
    running_.store(false);
}

} // namespace aver::softbody
