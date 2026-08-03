#pragma once
// --skin-scene-test <dir>: proves a SCENE ENTITY carrying a CSkeletalMesh draws posed.
//
// --skin-draw-test proves the draw path can rasterise a skinned buffer, using a box it built
// itself. This asks the question one level up and with a real asset: an entity spawned into the
// world, its rig read off disk, its clip sampled by AnimSystem, its per-entity skin target found by
// SkinnedScene, and its handle substituted at the draw. Every link in that chain is one the box
// test bypasses entirely.
//
// THE INSTRUMENT IS A RELATION, as it is throughout this area: the same pixels while exactly one
// thing changes -- here the animator's CLOCK, written directly because the clip is paused and the
// clock is data on the component. So the pose is a pure function of a number this test chose, not
// of frame timing or GPU speed. No baseline, no gate line, nothing to re-record.
//
// PROBES ARE CLASSIFIED BY COLOUR, NOT BY POSITION. The subject and the static reference are given
// distinct colours and a grid is swept; a probe is assigned to whichever of them it is showing.
// That is what lets the test assert things about "the reference" without knowing where the
// projection puts it -- and it means a change of camera, DPI or window size cannot silently move a
// probe onto the wrong object, which is the trap this repo has been caught by before.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <string>

namespace aver { class Engine; }

namespace aver::editor {

// The subject, the reference, and the schedule that separates them.
class SkinSceneTest {
public:
    // Loads the rig from `dir`, registers it, and spawns the entities. False means the check cannot
    // run -- a missing asset, or a build with no scene -- and is reported as unavailable.
    // `outMesh` is the uploaded handle: the HOST must put it in the same table its draw pass looks
    // in, or the entities exist and nothing draws them. That was the first way this went wrong.
    bool setup(Engine& e, const std::string& dir,
               u64* outMeshId, u64* outSkelId, u64* outClipId, u32* outMesh);

    // The rig's rest extent, so the host can register it the way it registers a project mesh's.
    void restBounds(Vec3& lo, Vec3& hi) const { lo = restMin_; hi = restMax_; }

    // Advances the schedule and samples the next probe. Once per frame, from the app.
    // `culledThisFrame` is what the host's scene pass just rejected on the frustum. Passed in
    // rather than inferred, because "nothing was culled" and "the culler does not work" look
    // identical from outside and only one of them is acceptable.
    void tick(Engine& e, f32 vpX, f32 vpY, f32 vpW, f32 vpH, u32 culledThisFrame);

    bool finished() const { return done_; }

    // The two entities, so the host can colour them. The scene pass otherwise draws every entity
    // the same default grey, which would leave every probe unclassifiable -- and a probe that
    // cannot be attributed is a probe that proves nothing.
    u64 subjectEntity() const { return subject_; }
    u64 referenceEntity() const { return reference_; }

    // Chosen far apart in hue so a probe can be assigned to one or the other with no ambiguity,
    // and both far from the sky and the ground.
    static void subjectColor(f32 out[3]) { out[0] = 0.90f; out[1] = 0.06f; out[2] = 0.90f; }
    static void referenceColor(f32 out[3]) { out[0] = 0.06f; out[1] = 0.85f; out[2] = 0.90f; }

    // True only when report() reached its PASS. Every failure path returns before setting it, so
    // "not passed" covers a real failure and a run that never got far enough to judge.
    //
    // READ BY SandboxApp::exitCode. Before that existed this flag was set and never looked at, so
    // --skin-scene-test printed FAIL to the log and exited 0, and any script driving it saw success.
    bool passed() const { return ok_; }

private:
    void report();

    // A DENSE grid, because a thin one is a coin flip. The first version swept 15 points and
    // classified exactly one onto each entity: enough to pass, and nowhere near enough to keep
    // passing when the window size or the DPI changes. Sampling costs two frames a probe and
    // nothing else.
    enum : u32 { kPhases = 3, kProbes = 35 };

    struct Probe {
        f32  u = 0.0f, v = 0.0f;
        f32  c[kPhases][4] = {};
        bool have[kPhases] = {};
    };

    Probe probes_[kProbes];
    u64   subject_ = 0;      // the animated entity, as a scene::Entity widened
    u64   reference_ = 0;    // the same mesh, no CSkeletalMesh, never posed
    u64   clipId_ = 0;
    Vec3  restMin_{0, 0, 0}, restMax_{0, 0, 0};
    u64   offscreen_ = 0;        // a third entity, parked far outside the frustum
    u32   maxCulled_ = 0;        // the most the host culled in any one frame of the run
    // The subject's published bounds, sampled once per phase. The pose is what should move them; a
    // box that never changes is a character culled and picked against its bind pose.
    Vec3  boundsLo_[kPhases], boundsHi_[kPhases];
    bool  haveBounds_[kPhases] = {};
    u32   step_ = 0;
    u32   phase_ = 0;
    bool  done_ = false;
    bool  ok_ = false;
};

} // namespace aver::editor
