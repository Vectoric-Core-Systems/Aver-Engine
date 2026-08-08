// The scene-level skinning check: one rig, one clock, two entities, fifteen pixels.
//
// GUARDED ON AVER_MODULE_SCENE, and it was not. This whole translation unit is compiled
// unconditionally (sandbox/CMakeLists.txt) and used unconditionally from SandboxApp.cpp, yet it
// included aver/scene/World.hpp and used scene:: throughout with no guard anywhere -- so a build
// with SCENE off failed with a C1083 on a header, in a file the editor only ever calls behind a
// runtime check.
//
// The header ALREADY DOCUMENTED the right behaviour: SkinSceneTest.hpp says setup() returning false
// covers "a missing asset, or a build with no scene -- and is reported as unavailable". The class
// was designed to degrade to a runtime no-op; the implementation just never got the split that makes
// that true. So the #else branch below is not a new contract, it is the one the header always
// claimed, finally honoured.
#include "SkinSceneTest.hpp"
#include "aver/core/Log.hpp"

#if AVER_MODULE_SCENE
#include "aver/runtime/Engine.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/Components.hpp"
#include "aver/formats/OcMesh.hpp"

#include <cmath>
#include <vector>

namespace aver::editor {

namespace {

// The three times the clock is set to. The middle one is where the clip's key bends the rig 90
// degrees; the outer two are the same rest time, which is what makes the pose reversible rather
// than merely different.
constexpr f32 kTimes[3] = {0.0f, 0.5f, 0.0f};

// What counts as a changed pixel, matching the draw test so the two report in the same units.
constexpr f32 kMinDelta = 0.05f;

// Frames to settle after writing the clock. The renderer replays LAST frame's draw list into its
// shadow and voxelise passes, so a pose written this frame is not fully on screen yet.
constexpr u32 kSettle = 8;

f32 worst3(const f32 a[4], const f32 b[4]) {
    f32 w = 0.0f;
    for (u32 i = 0; i < 3; ++i) w = std::fmax(w, std::fabs(a[i] - b[i]));
    return w;
}

// Which entity a pixel is showing, decided by hue rather than by where it is. Magenta and cyan sit
// on opposite sides of the green channel, so the test is "is green the smallest or the largest".
enum class Shows { Neither, Subject, Reference };
Shows classify(const f32 c[4]) {
    const f32 lum = (c[0] + c[1] + c[2]) / 3.0f;
    if (lum < 0.06f) return Shows::Neither;                       // too dark to attribute
    if (c[0] > 0.18f && c[2] > 0.18f && c[1] < 0.55f * c[0]) return Shows::Subject;     // magenta
    if (c[1] > 0.18f && c[2] > 0.18f && c[0] < 0.55f * c[2]) return Shows::Reference;   // cyan
    return Shows::Neither;
}

} // namespace

bool SkinSceneTest::setup(Engine& e, const std::string& dir,
                          u64* outMeshId, u64* outSkelId, u64* outClipId, u32* outMesh) {
    std::string d = dir;
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(d + "/Rig.ocmesh", md, &why)) {
        AVER_ERROR("[Skin] scene test: {}/Rig.ocmesh -- {}. Cook it first: "
                   "ConvertTool content/dev/Rig.gltf <dir> Rig", d, why);
        return false;
    }
    if (!md.hasSkin()) {
        AVER_ERROR("[Skin] scene test: Rig.ocmesh carries no skin streams; there is nothing to test");
        return false;
    }

    std::vector<rhi::MeshVertex> verts(md.vertexCount());
    for (u32 i = 0; i < md.vertexCount(); ++i) {
        rhi::MeshVertex& v = verts[i];
        v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
        v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
        v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
    }
    const rhi::MeshHandle h = e.device()->createMesh(verts.data(), static_cast<u32>(verts.size()),
                                                     md.indices.data(), static_cast<u32>(md.indices.size()));
    if (!h) { AVER_ERROR("[Skin] scene test: the device refused the rig mesh"); return false; }

    // Ids the host will register against the three files. Arbitrary and distinct; what matters is
    // that the same id reaches sceneMeshes_, contentIndex_ and the components.
    restMin_ = md.boundsMin;
    restMax_ = md.boundsMax;
    *outMesh = h;
    *outMeshId = 0x5C1'0001ull;
    *outSkelId = 0x5C1'0002ull;
    *outClipId = 0x5C1'0003ull;
    clipId_ = *outClipId;

    // ---- the two entities ----
    scene::World& w = scene::World::instance();

    // The SUBJECT: mesh, rig, and a PAUSED animator. Paused because the clock is data on the
    // component, so the test writes a->time directly and the pose becomes a pure function of a
    // number it chose rather than of how long a frame took.
    const scene::Entity a = w.create("SkinSubject");
    {
        auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(a, scene::kComponentMeshRenderer));
        mr->mesh = *outMeshId;
        mr->flags = scene::kMeshRendererVisible;
        auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(a, scene::kComponentSkeletalMesh));
        sm->skeleton = *outSkelId;
        auto* an = static_cast<scene::CAnimator*>(w.addComponent(a, scene::kComponentAnimator));
        an->clip = *outClipId;
        an->flags = scene::kAnimatorPaused;
        an->time = 0.0f;
    }

    // The REFERENCE: the same mesh with NO CSkeletalMesh, so it goes down the ordinary static path
    // and can never be posed. It is the control that separates "the subject moved" from "the whole
    // frame moved", which no amount of looking at the subject alone can do.
    const scene::Entity c = w.create("SkinReference");
    {
        auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(c, scene::kComponentMeshRenderer));
        mr->mesh = *outMeshId;
        mr->flags = scene::kMeshRendererVisible;
    }

    // Side by side across the camera's view. The default editor camera sits at (700,700,450) looking
    // at the origin, so this axis is roughly screen-horizontal -- but nothing here depends on that,
    // because the probes are assigned by colour.
    // SCALED UP HARD, and the number is not arbitrary. The rig is a 16 cm tube: at its authored
    // size a probe grid lands on almost none of it, and the first version of this test classified
    // exactly ONE probe onto each entity -- a pass that was a coin flip, and became a total miss
    // the moment the grid moved. Two fat pillars either side of the origin cannot be missed by a
    // grid this dense, which is what makes the result a measurement rather than a lucky sample.
    Transform ta{}; ta.position = Vec3{-140.0f, 140.0f, -60.0f}; ta.scale = Vec3{14.0f, 14.0f, 2.6f};
    Transform tc{}; tc.position = Vec3{ 140.0f,-140.0f, -60.0f}; tc.scale = Vec3{14.0f, 14.0f, 2.6f};
    w.setLocalTransform(a, ta);
    w.setLocalTransform(c, tc);
    w.flush();

    // A THIRD entity, parked a kilometre behind the camera. It exists so the frustum culler has
    // something it must reject: without it "0 culled" is indistinguishable from a culler that never
    // rejects anything, and a culler that never rejects anything passes every other check here.
    const scene::Entity off = w.create("SkinOffscreen");
    {
        auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(off, scene::kComponentMeshRenderer));
        mr->mesh = *outMeshId;
        mr->flags = scene::kMeshRendererVisible;
    }
    Transform to{}; to.position = Vec3{0.0f, 0.0f, -100000.0f}; to.scale = Vec3{1,1,1};
    w.setLocalTransform(off, to);
    offscreen_ = off;

    subject_ = a;
    reference_ = c;

    // A grid over the middle of the viewport. More probes than needed, because which of them land
    // on which entity is the projection's business and the assertions are over the classified sets.
    u32 p = 0;
    const f32 us[7] = {0.28f, 0.34f, 0.40f, 0.50f, 0.60f, 0.66f, 0.72f};
    const f32 vs[5] = {0.38f, 0.44f, 0.50f, 0.56f, 0.62f};
    for (f32 v : vs) for (f32 u : us) { probes_[p].u = u; probes_[p].v = v; ++p; }

    AVER_INFO("[Skin] scene test armed: subject entity {} (rig {} verts, paused animator), "
              "reference entity {} (same mesh, no skeleton)", a, md.vertexCount(), c);
    return true;
}

void SkinSceneTest::tick(Engine& e, f32 vpX, f32 vpY, f32 vpW, f32 vpH, u32 culledThisFrame) {
    if (done_) return;
    if (culledThisFrame > maxCulled_) maxCulled_ = culledThisFrame;

    scene::World& w = scene::World::instance();
    const scene::Entity subj = static_cast<scene::Entity>(subject_);

    // Written EVERY frame, not once per phase: the animator is paused so nothing else touches the
    // clock, and rewriting it makes the phase's pose independent of how many frames it has been up.
    if (auto* an = w.component<scene::CAnimator>(subj, scene::kComponentAnimator))
        an->time = kTimes[phase_];

    if (step_ < kSettle) { ++step_; return; }

    const u32 t = step_ - kSettle;
    const u32 slot = t / 2;
    if (slot >= kProbes) {
        // The subject's PUBLISHED bounds for this phase, read off the component the culler and the
        // picker read. Sampled here rather than in setup because they are rewritten every frame.
        if (const auto* mr = w.component<scene::CMeshRenderer>(subj, scene::kComponentMeshRenderer)) {
            boundsLo_[phase_] = Vec3{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            boundsHi_[phase_] = Vec3{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            haveBounds_[phase_] = true;
        }
        if (phase_ + 1 < kPhases) { ++phase_; step_ = 0; return; }
        report();
        done_ = true;
        return;
    }

    Probe& p = probes_[slot];
    if ((t % 2) == 0) {
        e.device()->requestCapture(static_cast<u32>(vpX + vpW * p.u), static_cast<u32>(vpY + vpH * p.v));
    } else {
        f32 c[4];
        if (e.device()->getCapture(c)) {
            for (u32 i = 0; i < 4; ++i) p.c[phase_][i] = c[i];
            p.have[phase_] = true;
        }
    }
    ++step_;
}

void SkinSceneTest::report() {
    u32 onSubject = 0, onReference = 0;
    u32 subjectMoved = 0, referenceMoved = 0, subjectReturned = 0;
    f32 bestSubject = 0.0f, worstReference = 0.0f, worstReturn = 0.0f;

    for (u32 i = 0; i < kProbes; ++i) {
        const Probe& p = probes_[i];
        if (!p.have[0] || !p.have[1] || !p.have[2]) continue;

        // Classified in the REST phase, which both outer phases share, so a probe keeps one
        // identity for the whole run rather than changing allegiance when the subject moves.
        const Shows what = classify(p.c[0]);
        const f32 moved = worst3(p.c[0], p.c[1]);
        const f32 returned = worst3(p.c[0], p.c[2]);

        if (what == Shows::Subject) {
            ++onSubject;
            bestSubject = std::fmax(bestSubject, moved);
            if (moved > kMinDelta) ++subjectMoved;
            worstReturn = std::fmax(worstReturn, returned);
            if (returned <= kMinDelta) ++subjectReturned;
        } else if (what == Shows::Reference) {
            ++onReference;
            worstReference = std::fmax(worstReference, moved);
            if (moved > kMinDelta) ++referenceMoved;
        }
    }

    AVER_INFO("[Skin] scene test: {} probes on the subject, {} on the reference", onSubject, onReference);
    // Reported because "0 on each" has two very different causes -- a schedule that ran out of
    // frames, and a grid that missed -- and the count tells them apart at a glance.
    u32 complete = 0;
    for (u32 i = 0; i < kProbes; ++i) if (probes_[i].have[0] && probes_[i].have[1] && probes_[i].have[2]) ++complete;
    AVER_INFO("[Skin] scene test: {} of {} probes read back in all three phases", complete, u32(kProbes));

    // ---- DRAWN. A skinned entity that fails to resolve must fall back to its rest pose, never to
    //      nothing, so its absence is a failure and not merely an absence of evidence. ----
    if (onSubject == 0) {
        AVER_ERROR("[Skin] scene test FAIL (drawn): no probe ever showed the skinned entity. Either "
                   "it was not drawn at all -- a substituted handle of zero being read as 'draw "
                   "nothing' -- or it is not where the camera is looking");
        return;
    }
    if (onReference == 0) {
        AVER_ERROR("[Skin] scene test INCONCLUSIVE: no probe showed the static reference, so there "
                   "is no control and 'the subject moved' cannot be told apart from 'the frame moved'");
        return;
    }

    // ---- CONTROL first: if the whole frame moved, the headline assertion is meaningless ----
    if (referenceMoved > 0) {
        AVER_ERROR("[Skin] scene test INCONCLUSIVE: {} of {} probes on the STATIC reference changed "
                   "(worst {:.4f}) when only the animator's clock was written. Something global "
                   "moved -- exposure, a temporal filter, a GI reconvergence -- so a change on the "
                   "subject would prove nothing",
                   referenceMoved, onReference, worstReference);
        return;
    }

    // ---- POSED: the headline ----
    AVER_INFO("[Skin] scene test: subject changed on {} of {} probes (largest {:.4f}); reference "
              "held still on all {} (largest {:.4f})",
              subjectMoved, onSubject, bestSubject, onReference, worstReference);

    if (subjectMoved == 0) {
        AVER_ERROR("[Skin] scene test FAIL (posed): the skinned entity did not move when its clip "
                   "time went from {} to {}, while the static reference beside it held still. The "
                   "rest vertices are identical in both frames, so this is a character drawn in "
                   "bind pose -- the handle was never substituted, or nothing posed it",
                   kTimes[0], kTimes[1]);
        return;
    }

    // ---- REVERSIBLE: the pose is a function of time, not an accumulation ----
    if (subjectReturned != onSubject) {
        AVER_ERROR("[Skin] scene test FAIL (reversible): the subject moved, but returning the clock "
                   "to {} did not return {} of {} probes to what they were (worst {:.4f}). A pose "
                   "that does not come back means a dispatch reading its own output, or the rest "
                   "stream being overwritten by the posed one",
                   kTimes[2], onSubject - subjectReturned, onSubject, worstReturn);
        return;
    }

    // ---- CULLING: the frustum rejected the entity that is a kilometre away ----
    if (maxCulled_ == 0) {
        AVER_ERROR("[Skin] scene test FAIL (culling): an entity parked 1 km behind the camera was "
                   "never frustum-culled in any frame. Bounds that follow the pose are worth nothing "
                   "if nothing rejects anything -- the culler is a no-op");
        return;
    }
    AVER_INFO("[Skin] scene test CULL PASS: the off-screen entity was rejected (up to {} in a frame), "
              "so the frustum test is live and the bounds below are actually consumed", maxCulled_);

    // ---- BOUNDS: the box the culler and the picker read must follow the pose ----
    if (haveBounds_[0] && haveBounds_[1] && haveBounds_[2]) {
        const auto span = [](const Vec3& lo, const Vec3& hi) {
            return std::fmax(hi.x - lo.x, std::fmax(hi.y - lo.y, hi.z - lo.z));
        };
        const f32 restSpan = span(restMin_, restMax_);
        AVER_INFO("[Skin] scene test: bounds  rest ({:.0f},{:.0f},{:.0f})..({:.0f},{:.0f},{:.0f})  "
                  "posed ({:.0f},{:.0f},{:.0f})..({:.0f},{:.0f},{:.0f})",
                  boundsLo_[0].x, boundsLo_[0].y, boundsLo_[0].z,
                  boundsHi_[0].x, boundsHi_[0].y, boundsHi_[0].z,
                  boundsLo_[1].x, boundsLo_[1].y, boundsLo_[1].z,
                  boundsHi_[1].x, boundsHi_[1].y, boundsHi_[1].z);

        // It must CONTAIN the rest extent in every phase -- the bound is conservative by
        // construction and a box that has shrunk inside the mesh would cull a visible character.
        bool containsRest = true;
        for (u32 ph = 0; ph < kPhases; ++ph)
            if (boundsLo_[ph].z > restMin_.z + 1e-2f || boundsHi_[ph].z < restMax_.z - 1e-2f)
                containsRest = false;
        if (!containsRest) {
            AVER_ERROR("[Skin] scene test FAIL (bounds): the published box does not contain the rest "
                       "extent, so a visible character can be culled or missed by a click");
            return;
        }

        // And it must MOVE. A box that is identical at t=0 and t=0.5 is the bind-pose box, which is
        // the whole defect: the character bends out of it and pops at the screen edge.
        f32 moved = 0.0f;
        for (u32 i = 0; i < 3; ++i) {
            const f32* a0 = &boundsLo_[0].x; const f32* a1 = &boundsLo_[1].x;
            const f32* b0 = &boundsHi_[0].x; const f32* b1 = &boundsHi_[1].x;
            moved = std::fmax(moved, std::fabs(a0[i] - a1[i]));
            moved = std::fmax(moved, std::fabs(b0[i] - b1[i]));
        }
        if (moved < 1.0f) {
            AVER_ERROR("[Skin] scene test FAIL (bounds): the box moved {:.3f} cm when the pose "
                       "changed, on a rig spanning {:.0f} cm. That is the bind-pose box, so a bent "
                       "limb is culled and picked against geometry that is not there", moved, restSpan);
            return;
        }
        AVER_INFO("[Skin] scene test BOUNDS PASS: the published box contains the rest extent and "
                  "moved {:.1f} cm with the pose, so culling and picking follow the character", moved);
    } else {
        AVER_WARN("[Skin] scene test: the subject's bounds were not sampled; the culling half did "
                  "NOT run and the result below covers only what is drawn");
    }

    ok_ = true;
    AVER_INFO("[Skin] scene test PASS: an entity with a CSkeletalMesh drew POSED, a static entity "
              "beside it did not move, and returning the clock returned the pose. The whole chain "
              "held: .ocmesh skin streams, .ocskel, .ocanim, AnimSystem, the per-entity skin target, "
              "and the substituted draw handle.");
}

#else   // !AVER_MODULE_SCENE

// The documented degraded form: available() stays false, and nothing else does anything.
bool SkinSceneTest::setup(Engine&, const std::string&, u64*, u64*, u64*, u32*) {
    AVER_INFO("[Skin] scene test unavailable: this build has no scene module");
    return false;
}
void SkinSceneTest::tick(Engine&, f32, f32, f32, f32, u32) {}
void SkinSceneTest::report() {
    AVER_INFO("[Skin] scene test was not run: this build has no scene module");
}

#endif  // AVER_MODULE_SCENE

} // namespace aver::editor
