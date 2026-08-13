// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Aver.Occlusion — a hierarchical-Z occlusion culler, and NOTHING else. It takes a depth buffer and
// a batch of world-space AABBs and answers, per box, "did every draw so far already cover this box's
// screen footprint with something closer?" It has no idea what a box belongs to (an entity, a chunk,
// a cluster — this module's own signature never spells any of those words), which is what makes it
// usable from more than one caller: SandboxApp.cpp's per-entity scene walk is the first, and a future
// landscape-tile or cluster culler is free to be the second without this module changing at all.
//
// WHY ITS OWN MODULE. The design review that asked for this was explicit that occlusion must NOT
// live in three obvious-looking places:
//   - the RHI (modules/rhi), which must never learn about features — an HZB pyramid is a rendering
//     TECHNIQUE, not a hardware primitive, and the RHI's job stops at giving a technique the pieces
//     it needs (a texture it can create, a compute pipeline it can dispatch, a barrier it can record).
//   - modules/render.voxi, which would wire occlusion to global illumination for no reason beyond
//     "that's where the other GPU passes are" — occlusion has nothing to do with light transport, and
//     a project that ships without Voxi (AVER_MODULE_VOXI=OFF, which this engine treats as a build
//     that must still assemble) must still be able to cull.
//   - modules/trifactor, which would couple occlusion to virtualized geometry specifically, when the
//     first thing this module culls is neither virtualized nor geometry-shaped from its point of view
//     — it is 6,370 plain CMeshRenderer entities, and the SECOND thing it culls (a landscape tile, a
//     cluster, whatever comes next) must not need this module rewritten to accept it.
//
// THE TWO PIECES THE HEADER-LEVEL COMMENT ABOVE PROMISES:
//   - "a pass that builds the pyramid": buildPyramid() below.
//   - "something that takes bounds and a view and answers can this be seen": testBatch() below. The
//     view is consumed AT buildPyramid() time (the pyramid IS a function of one particular view); by
//     the time testBatch() runs, "the view" is simply "whichever pyramid buildPyramid() built last".
//
// ---------------------------------------------------------------------------------------------------
// HOW THE PYRAMID IS BUILT (see OcclusionCuller.cpp for the HLSL these three passes actually run):
//
// Mip 0 is SEEDED from the live scene depth buffer, not copied from it. Two things make this more
// than a reformat:
//   (a) MSAA. The scene depth buffer defaults to 4 samples on both backends (D3D12Device's
//       kDefaultSampleCount), while the only precedent for a depth-as-SRV trick in this tree (Voxi's
//       own shadow map, VoxiRenderer.cpp createShadowResources) is forced single-sampled and always
//       has been — nothing in this engine has ever read a MULTISAMPLED depth buffer from a shader
//       before this. The seed pass declares a REAL Texture2DMS in HLSL (CSSeed in OcclusionCuller.cpp)
//       and reduces its N samples with max(), the same conservative direction the rest of the pyramid
//       uses (see below) — collapsing to ANY single sample, or to their average, could report a pixel
//       as closer than its farthest-covered sample and manufacture a false cull at a foliage edge,
//       which is exactly the content this scene is made of.
//   (b) the resource has to become TYPELESS to be read at all. D3D12Device's depth buffer used to be
//       created directly as DXGI_FORMAT_D32_FLOAT, which can only ever be VIEWED as D32_FLOAT — no
//       SRV of any format is legal on it. It is now created R32_TYPELESS (D3D12Device.cpp's
//       kDepthResourceFormat), with an explicit DSV desc pinning the depth VIEW back to D32_FLOAT —
///      the identical "one resource, two views" trick Voxi's shadow map already uses, just extended
//       to a multisampled resource for the first time. See D3D12Device::createDepthBuffer's own
//       comment for the mechanics.
//
// Every mip AFTER 0 is the CONSERVATIVE reduction of its four parents: max(), meaning FURTHEST from
// the camera under this engine's depth convention (near = 0, far = 1 — see D3D12Device's depth-clear
// value of 1.0f). This is not an arbitrary choice of "how to downsample": a coarse cell's stored value
// has to be at least as far as the CLOSEST occluder anywhere under its footprint, or a box that is
// hidden everywhere but one corner of that cell would read as hidden everywhere. max() of the four
// children's own (already-conservative) values is exactly that bound, applied recursively — see
// OcclusionMath.hpp's conservativelyHidden for where the bound is finally spent, and
// tests/occlusion/src/OcclusionMathTest.cpp for a synthetic pyramid where getting this backwards
// (min() instead of max()) is caught by a box popping invisible that a screenshot diff would call a
// bug, not a tradeoff.
//
// ---------------------------------------------------------------------------------------------------
// TWO-PASS, AND WHY BOTH PASSES SHARE ONE FRAME. Single-pass occlusion — cull THIS frame against a
// pyramid built from LAST frame's depth — produces FALSE CULLS the instant the camera moves fast
// enough that something enters view between the two frames: nothing in last frame's depth buffer
// could possibly have drawn it, so the pyramid reports it hidden regardless of where it actually is
// now. That failure mode is an object popping OUT of existence, which is worse than the milliseconds
// two-pass spends to avoid it. The caller (SandboxApp.cpp) is the one that actually runs both passes,
// because it is the one holding the entity list and the "was this entity drawn last frame" bit this
// module deliberately does not store (see the header comment above on what this module does not
// know) — but the shape is:
//   1. draw the entities that were visible last frame, with THIS frame's camera and transforms —
//      this is ordinary rendering, no different from a build with occlusion off, and it is what
//      seeds the depth buffer buildPyramid() reads.
//   2. buildPyramid() — this file's own pass, from the depth that step 1 just wrote.
//   3. testBatch() over EVERY entity (not only the ones step 1 skipped) — see its own comment for why
//      re-testing the ones already drawn is not wasted work, it is how an entity that has become
//      occluded since last frame gets DEMOTED for next frame rather than staying in the "always draw"
//      set forever.
//   4. draw whichever of step 3's PREVIOUSLY-HIDDEN entities it says might now be visible.
//
// ---------------------------------------------------------------------------------------------------
// THE ONE COST THIS DESIGN DOES NOT HIDE: testBatch() has to hand the caller a CPU-visible answer
// before step 4 can decide what to draw, in the SAME frame the pyramid that answer depends on was
// built. This engine's RHI (modules/rhi) exposes exactly one GPU/CPU synchronisation primitive a
// feature module can reach for — IResourceFactory::waitIdle(), a full stop — and testBatch() uses it.
// That is a REAL, MEASURED stall (see the design doc / commit message this lands with for the number),
// not a free lunch: a production system would replace it with either GPU-side predication (skip a
// draw call itself based on a GPU-written flag, never surfacing the answer to the CPU at all) or an
// indirect/GPU-driven draw path, and this codebase currently has neither — every draw call is issued
// by name from a CPU walk (SandboxApp.cpp), and adding predication or indirect draws to that walk is
// a change to the CORE draw path this task's own scope does not include. Reporting the stall honestly,
// including a run where it makes the frame worse, is the more useful result than hiding it.
#pragma once
#include "aver/occlusion/OcclusionMath.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <vector>

namespace aver::occlusion {

// One box, as the caller already has it: world-space min/max corners, row-major cm, +Z up (this
// engine's convention throughout — see MeshVertex's own note in RHI.hpp).
struct Aabb {
    f32 min[3];
    f32 max[3];
};

// The narrow interface this module promises: build the pyramid, then answer visibility for a batch
// of boxes against it. Knows nothing about what a box belongs to — see this header's top comment.
class IOcclusionCuller {
public:
    virtual ~IOcclusionCuller() = default;

    // (Re)sizes the pyramid for the scene's current render target, a no-op when nothing changed.
    // Call this every frame before buildPyramid(); it is cheap when sceneWidth/sceneHeight/
    // sampleCount already match what the pyramid was built for. Returns false when the pyramid could
    // not be (re)built — a compute-pipeline compile failure, most likely — in which case buildPyramid
    // and testBatch both become no-ops (buildPyramid logs once; testBatch marks every box "visible",
    // the always-safe answer) until a later call succeeds.
    virtual bool ensureSized(rhi::IResourceFactory& res, u32 sceneWidth, u32 sceneHeight,
                             u32 sampleCount) = 0;

    // Builds the pyramid from `sceneDepth` (IDevice::sceneDepthTexture()) exactly as it stands right
    // now — see this header's top comment for why that has to be AFTER the "visible last frame" set
    // has been drawn and BEFORE anything else is drawn this frame. Transitions `sceneDepth` to a
    // shader-readable state and back to DepthWrite before returning, so the caller's own depth writes
    // can resume immediately afterwards. Opens its own ScopedGpuStat("HZB build") span.
    virtual void buildPyramid(rhi::IRenderContext& ctx, rhi::TextureHandle sceneDepth,
                              const f32 viewProj[16]) = 0;

    // Tests every box in `boxes` against the pyramid buildPyramid() most recently built, and fills
    // outVisible with one byte per box (non-zero = "the pyramid could not prove this is hidden; draw
    // it" — see OcclusionMath.hpp's conservativelyHidden for what that promise actually rests on).
    // Forces the answer to be available before returning (see this header's own top comment on why
    // that is a real, deliberate synchronisation cost and not an oversight). Opens its own
    // ScopedGpuStat("HZB test") span, SEPARATE from "HZB build" — the two cost different things and a
    // caller measuring "does this pay for itself" needs to see them apart.
    virtual void testBatch(rhi::IRenderContext& ctx, rhi::IResourceFactory& res,
                           const Aabb* boxes, u32 count, std::vector<u8>& outVisible) = 0;

    // Boxes testBatch() said "hidden" for, and the total it was asked about, across the MOST RECENT
    // testBatch() call only (not accumulated) — a caller wanting a running total (SandboxApp.cpp's
    // periodic log line) accumulates these itself, once per frame, the same way every other counter
    // in that file already does.
    virtual void lastTestCounts(u32& culled, u32& tested) const = 0;
};

// Owns nothing the caller does not already own (the resource factory, the depth texture) except its
// OWN pyramid texture, compute pipelines and the small readback buffer testBatch() uses. Destroy with
// destroyOcclusionCuller before the IResourceFactory that created its resources goes away.
IOcclusionCuller* createOcclusionCuller(rhi::IResourceFactory& res);
void destroyOcclusionCuller(rhi::IResourceFactory& res, IOcclusionCuller* culler);

} // namespace aver::occlusion
