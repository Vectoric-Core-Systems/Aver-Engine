#pragma once
// PlayProfile -- the numbers the profiler panel needs to see a Play session: a smoothed frame time
// with the worst frame of the last two seconds or so, and one smoothed CPU cost per timed phase of
// onUpdate.
//
// WHY IT EXISTS. Play-in-Editor was reported to drop the frame rate a lot, and nothing the editor
// could show said where. The GPU table is a since-boot average, so a pass that only exists in Play
// is divided by every edit frame before it; the status bar showed one raw frame; and none of the CPU
// work Play adds to onUpdate (the gameplay tick and physics step, vehicles, object animation, the
// kinematic body drive, skinning, graph ticks) had a timer. This is the CPU half of that fix, and
// the pure part of it, so the arithmetic is checkable without a window (tests/editor/src/
// PlayProfileTest.cpp).
//
// NOT CpuLap/CpuNest (aver/core/CpuTiming.hpp). Those exist for the ~100,000 boundaries a frame the
// scene walk crosses: a TSC read, a fixed span enum shared with the walk, strict LIFO scopes. These
// phases are a handful of sequential blocks of onUpdate, each opened once a frame, so steady_clock
// (about 20 ns a read) costs well under a microsecond a frame in total. And the phases are a
// begin()/end() PAIR rather than a scoped object on purpose: a scoped object at function scope would
// time everything after it, and wrapping a statement in a brace block of its own would add a nesting
// level to SandboxApp's onUpdate, which already sits close to MSVC's block-nesting limit (C1061).
//
// NO ALLOCATION, EVER: fixed arrays, filled by index. begin()/end() are two array stores and a
// subtraction; endFrame() is one pass over kPhases entries.
//
// A phase can be opened more than once in a frame (the vehicles run before AND after the physics
// step) and the pieces add up. A begin() with no matching end() -- an early return between the two --
// is simply replaced by the next begin() of that phase, and an end() with no begin() is ignored, so a
// bracket that goes wrong loses one sample and never poisons the next.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aver::editor {

// The timed phases. Add one by appending before Count and giving it a label below -- nothing else
// indexes by position. Each is a block that onUpdate runs in Play; the ones that also run outside
// Play (animation, skinning, the flush) take a cheap path there and are folded in only while a Play
// session is running (see endFrame), so the table reads as "what Play costs".
enum class PlayPhase : u8 {
    Gameplay,     // game::tickGameplayGroups: PrePhysics group, the Jolt step, Physics + PostPhysics groups
    Vehicles,     // VehicleSystem::prePhysics and postPhysics
    ObjectAnim,   // AnimSystem::tick (object clips, skeletal animators, control rigs)
    DriveBodies,  // driveAnimatedBodies: kinematic bodies follow their animated placements
    Skinned,      // SkinnedScene::update
    GraphTicks,   // ScriptHost::tickGraphClassInstances: class-placed graphs' OnTick
    WorldFlush,   // World::flush: deferred destroys + world-matrix recomposition, O(entities)
    Count
};

inline constexpr usize kPlayPhaseCount = static_cast<usize>(PlayPhase::Count);

inline constexpr const char* kPlayPhaseLabel[kPlayPhaseCount] = {
    "Gameplay tick + physics step",
    "Vehicles",
    "Animation tick",
    "Drive animated bodies",
    "Skinned meshes",
    "Graph ticks",
    "World flush",
};

class PlayProfile {
public:
    // The window "worst" is taken over, in frames. About two seconds at 60 fps.
    static constexpr u32 kWindow = 120;
    // Time constant of every moving average here, in seconds. Frame-time based, not a fixed 1/N per
    // frame, so a 30 fps session and a 144 fps one smooth over the same stretch of wall time and a
    // long hitch pulls the average by the hitch's own weight.
    static constexpr f64 kSmoothSec = 0.5;

    static u64 clockNs() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // ---- phase timers, called from onUpdate ----------------------------------------------------
    // The time argument defaults to the clock; a test passes its own.
    void begin(PlayPhase p, u64 nowNs = clockNs()) {
        const usize i = static_cast<usize>(p);
        start_[i] = nowNs;
        open_[i] = true;
    }
    void end(PlayPhase p, u64 nowNs = clockNs()) {
        const usize i = static_cast<usize>(p);
        if (!open_[i]) return;
        open_[i] = false;
        if (nowNs > start_[i]) acc_[i] += nowNs - start_[i];
    }
    // Fixed physics steps the frame's tick ran (tickGameplayGroups' return). More than one a frame
    // is the catch-up the physics world does when a frame runs long, which feeds back into the next
    // frame's length -- the one cost in Play that grows as the frame slows.
    void addPhysicsSteps(i32 n) { if (n > 0) steps_ += static_cast<u32>(n); }

    // Whether a phase (or any phase) measured time since the last endFrame(). Read BEFORE endFrame,
    // which empties the accumulators: it lets the caller tell a frame whose Play work ran from one
    // that only looks like Play (a paused session, or the frame the Play button wiped the
    // accumulators in) and keep those out of the averages.
    bool ranThisFrame(PlayPhase p) const { return acc_[static_cast<usize>(p)] > 0; }
    bool anyRanThisFrame() const {
        for (usize i = 0; i < kPlayPhaseCount; ++i)
            if (acc_[i] > 0) return true;
        return false;
    }

    // ---- once a frame, from the status bar (the one place that runs every UI frame) -------------
    // `dtSec` is the frame's wall time. `playing` says whether this frame's phase times belong in
    // the table: outside Play the accumulators are still emptied, so a Play that starts later never
    // inherits an edit frame's animation tick, but the smoothed values are left where they were.
    void endFrame(f64 dtSec, bool playing) {
        u64 acc[kPlayPhaseCount];
        for (usize i = 0; i < kPlayPhaseCount; ++i) { acc[i] = acc_[i]; acc_[i] = 0; open_[i] = false; }
        const u32 steps = steps_;
        steps_ = 0;
        if (!(dtSec > 0.0)) return;

        const f64 alpha = 1.0 - std::exp(-dtSec / kSmoothSec);
        const f64 dtMs = dtSec * 1000.0;
        lastFrameMs_ = dtMs;
        frameEmaMs_ = frameCount_ == 0 ? dtMs : frameEmaMs_ + alpha * (dtMs - frameEmaMs_);
        frameRing_[framePos_] = static_cast<f32>(dtMs);
        framePos_ = (framePos_ + 1) % kWindow;
        if (frameCount_ < kWindow) ++frameCount_;

        if (!playing) return;
        for (usize i = 0; i < kPlayPhaseCount; ++i) {
            const f64 ms = static_cast<f64>(acc[i]) * 1e-6;
            phaseEmaMs_[i] = phaseSeeded_ ? phaseEmaMs_[i] + alpha * (ms - phaseEmaMs_[i]) : ms;
            phaseRing_[phasePos_][i] = static_cast<f32>(ms);
        }
        const f64 stepsF = static_cast<f64>(steps);
        stepsEma_ = phaseSeeded_ ? stepsEma_ + alpha * (stepsF - stepsEma_) : stepsF;
        stepsRing_[phasePos_] = static_cast<u8>(std::min<u32>(steps, 255u));
        phaseSeeded_ = true;
        phasePos_ = (phasePos_ + 1) % kWindow;
        if (phaseCount_ < kWindow) ++phaseCount_;
        ++playFrames_;
    }

    // ---- frame time ----------------------------------------------------------------------------
    f64 frameEmaMs() const { return frameEmaMs_; }
    f64 lastFrameMs() const { return lastFrameMs_; }
    // Frames the worst-of window currently holds (kWindow once it has filled).
    u32 windowFrames() const { return frameCount_; }
    f64 worstFrameMs() const {
        f32 worst = 0.0f;
        for (u32 i = 0; i < frameCount_; ++i) worst = std::max(worst, frameRing_[i]);
        return worst;
    }

    // ---- phases (only frames endFrame was told were Play) --------------------------------------
    // Frames folded since the last resetPhases(); 0 means "no Play has run yet".
    u64 playFrames() const { return playFrames_; }
    f64 phaseEmaMs(PlayPhase p) const { return phaseEmaMs_[static_cast<usize>(p)]; }
    f64 phaseWorstMs(PlayPhase p) const {
        f32 worst = 0.0f;
        for (u32 f = 0; f < phaseCount_; ++f) worst = std::max(worst, phaseRing_[f][static_cast<usize>(p)]);
        return worst;
    }
    f64 stepsEma() const { return stepsEma_; }
    u32 stepsWorst() const {
        u32 worst = 0;
        for (u32 f = 0; f < phaseCount_; ++f) worst = std::max<u32>(worst, stepsRing_[f]);
        return worst;
    }

    // ---- resets --------------------------------------------------------------------------------
    // The phase table: Play start, and the panel's Reset button.
    void resetPhases() {
        for (usize i = 0; i < kPlayPhaseCount; ++i) { acc_[i] = 0; open_[i] = false; phaseEmaMs_[i] = 0.0; }
        for (u32 f = 0; f < kWindow; ++f)
            for (usize i = 0; i < kPlayPhaseCount; ++i) phaseRing_[f][i] = 0.0f;
        for (u32 f = 0; f < kWindow; ++f) stepsRing_[f] = 0;
        steps_ = 0;
        stepsEma_ = 0.0;
        phaseSeeded_ = false;
        phasePos_ = 0;
        phaseCount_ = 0;
        playFrames_ = 0;
    }
    // The frame-time average and the worst-of window: the panel's Reset button.
    void resetFrames() {
        frameEmaMs_ = 0.0;
        lastFrameMs_ = 0.0;
        for (u32 f = 0; f < kWindow; ++f) frameRing_[f] = 0.0f;
        framePos_ = 0;
        frameCount_ = 0;
    }

private:
    // Timers: this frame's accumulated nanoseconds per phase, and the open bracket's start.
    u64  acc_[kPlayPhaseCount] = {};
    u64  start_[kPlayPhaseCount] = {};
    bool open_[kPlayPhaseCount] = {};
    u32  steps_ = 0;

    f64  frameEmaMs_ = 0.0;
    f64  lastFrameMs_ = 0.0;
    f32  frameRing_[kWindow] = {};
    u32  framePos_ = 0, frameCount_ = 0;

    f64  phaseEmaMs_[kPlayPhaseCount] = {};
    f32  phaseRing_[kWindow][kPlayPhaseCount] = {};
    u8   stepsRing_[kWindow] = {};
    f64  stepsEma_ = 0.0;
    bool phaseSeeded_ = false;
    u32  phasePos_ = 0, phaseCount_ = 0;
    u64  playFrames_ = 0;
};

} // namespace aver::editor
