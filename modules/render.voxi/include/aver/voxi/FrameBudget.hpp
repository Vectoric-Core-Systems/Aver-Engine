// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

// THE FRAME-BUDGET CONTROLLER, SHARED BY BOTH HOSTS. Lifted out of SandboxApp::frameBudgetTick
// (sandbox/src/SandboxProject.cpp) and its frameBudget* members (sandbox/src/SandboxApp.hpp) into a
// header-only pure function over FrameBudgetState and a Settings&, mirroring ProjectRenderApply.hpp's
// own house style: no AVER_WARN/INFO, no voxi::Renderer::get(), no device -- so the rung ladder itself
// is testable headless, the same reason that header gives for staying pure. The editor's
// SandboxApp::frameBudgetTick is still a separate copy of this logic.
//
// BYTE-IDENTICAL TO THE EDITOR'S OWN COPY: same 30-frame warm-up, same 250ms hitch threshold, same
// 0.9/0.1 EMA, same over/under-budget band (budget / budget*0.85), same 60-frame climb-back and the
// same five-rung interval/cone-cap table. A caller that wants the editor's own log line reads it off
// this call's return value plus the FrameBudgetState/Settings fields it just touched -- see
// frameBudgetTick's own comment below for the exact shape.
namespace aver::voxi {

inline constexpr i32 kFrameBudgetRungs = 5;

// One controller's worth of state, owned by the caller (SandboxApp keeps it as member fields today;
// a GameApp keeps one FrameBudgetState alongside its other per-run state). Default-constructed is
// "off" -- budgetMs 0 means frameBudgetTick never touches its Settings& argument.
struct FrameBudgetState {
    f32 budgetMs = 0.0f;   // RENDER.FRAMEBUDGETMS, or <= 0 to disable the whole controller
    i32 rung = 0;          // 0 = full quality, the authored settings untouched
    f32 avgMs = 0.0f;      // EMA of the frame time, so one long frame cannot move the rung
    u32 frames = 0;        // frames seen since budgetMs was last set; the first few are loading
    i32 under = 0;         // consecutive comfortable frames, for the slow climb back to rung 0
    u32 appliedInterval = 0;
    u32 appliedCones = 0;
};

// The rung ladder itself: same values as SandboxProject.cpp's own file-local kInterval/kConeCap.
// 0 in kFrameBudgetConeCap means "leave giCones as authored" -- the two lower rungs throttle only
// the GI update interval.
inline constexpr u32 kFrameBudgetInterval[kFrameBudgetRungs] = {1, 2, 4, 4, 8};
inline constexpr u32 kFrameBudgetConeCap[kFrameBudgetRungs]  = {0, 0, 0, 8, 5};

// Advances `state` by one frame of `dt` and, once it decides a rung above 0 is warranted, raises
// `vs.giUpdateInterval` and caps `vs.giCones` to that rung's values. `vs` should be a COPY of the
// live settings the caller is about to push, never the singleton itself -- SandboxApp.cpp's own
// comment on this is the reason: the controller must never write back into the authored settings, or
// one throttled frame would become the new baseline and quality could only ever ratchet down.
//
// Returns true the one frame the applied GI update interval or cone cap actually changed -- the
// caller's cue to log, with `state.budgetMs`, `state.avgMs`, `state.rung`, `vs.giUpdateInterval` and
// `vs.giCones` as its arguments, the exact line SandboxProject.cpp's own frameBudgetTick prints
// ("[Sandbox] frame budget {:.1f}ms: {:.1f}ms average -> rung {} (GI every {} frame(s), {} cone(s))",
// substituting whatever prefix the caller's own log lines use). Returns false on every other frame,
// INCLUDING the frame the rung climbs back down to 0 -- the editor's own early-out below never
// touches `vs` or the applied* fields on that transition either, so there is nothing new to report:
// `vs` already holds the authored values once the rung is 0, because it was read fresh from the live
// settings this same frame.
inline bool frameBudgetTick(FrameBudgetState& state, f32 dt, Settings& vs) {
    // OFF, OR NOT YET WARM. See FrameBudgetState::budgetMs's own comment for what "off" means; a
    // caller that only wants the controller active outside a bounded capture decides that BEFORE
    // calling this (skip the call entirely), rather than this header knowing what a capture is.
    if (state.budgetMs <= 0.0f) { state.rung = 0; return false; }
    const f32 ms = dt * 1000.0f;

    // WARM-UP, AND IT IS NOT OPTIONAL. Opening a level costs seconds in one frame -- mesh uploads,
    // shader work, the first voxelisation. Folding that into the average made the controller read
    // over a second on a scene running fine, and an EMA this smooth needs about a hundred frames to
    // forget a sample that size: it ratcheted straight to the bottom rung off a number that described
    // loading, then sat there. Ignore the opening frames outright.
    if (++state.frames < 30) return false;

    // AND REJECT HITCHES AFTERWARDS, for the same reason in miniature. A single 250ms+ frame is a
    // compile, a stream-in or an alt-tab; none of them is a signal about steady-state quality, and
    // reacting to one drops quality for the second or so it takes the average to recover. Dropped
    // rather than clamped, because a clamped hitch is still a vote for "too slow".
    constexpr f32 kHitchMs = 250.0f;
    if (ms > kHitchMs) return false;

    // Seeded on the first frame that survives both filters rather than climbing from zero, which
    // would otherwise read as "comfortably under budget" for the first dozen frames of every launch.
    state.avgMs = state.avgMs <= 0.0f ? ms : (state.avgMs * 0.9f + ms * 0.1f);

    // The band, not the line. Dropping at exactly the budget and climbing at exactly the budget
    // guarantees oscillation; 15% of headroom is the gap between the two decisions.
    const f32 over  = state.budgetMs;
    const f32 under = state.budgetMs * 0.85f;
    if (state.avgMs > over && state.rung < kFrameBudgetRungs - 1) {
        ++state.rung;
        state.under = 0;
    } else if (state.avgMs < under && state.rung > 0) {
        // Sixty comfortable frames -- about a second -- before giving quality back. Deliberately far
        // slower than the drop.
        if (++state.under >= 60) { --state.rung; state.under = 0; }
    } else {
        state.under = 0;
    }

    // The ladder. Rung 0 leaves the authored settings completely alone, so a project inside its
    // budget renders exactly what it asked for -- and, on the frame the rung climbs back down to it,
    // returns before touching `vs` or the applied* fields at all (see this function's own comment).
    if (state.rung == 0) return false;
    const u32 wantInterval = kFrameBudgetInterval[state.rung];
    if (wantInterval > vs.giUpdateInterval) vs.giUpdateInterval = wantInterval;
    if (kFrameBudgetConeCap[state.rung] && vs.giCones > kFrameBudgetConeCap[state.rung])
        vs.giCones = kFrameBudgetConeCap[state.rung];

    if (vs.giUpdateInterval != state.appliedInterval || vs.giCones != state.appliedCones) {
        state.appliedInterval = vs.giUpdateInterval;
        state.appliedCones = vs.giCones;
        return true;
    }
    return false;
}

} // namespace aver::voxi
