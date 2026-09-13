// THE RULE THIS HEADER ENFORCES: per entity, ONE function produces the list of draws the renderer
// receives, and that list does not depend on any cull verdict. A cull decides only WHO delivers the
// list -- drawMesh() (which broadcasts it to every IRenderFeature) or a direct voxiRenderer_.submit()
// -- never WHAT is in it. Culling may skip rasterisation, LOD/cluster selection and outline
// bookkeeping; it must never change a mesh, a material, or a translucency flag.
//
// THE BUG THIS FIXES. Before this header existed, a culled or owner-hidden entity reached Voxi
// through a second, hand-written route (SandboxApp.cpp's submitShadowOnly) that read a different
// material (mesh-slot-0's, not each part's own), dropped multi-part splits entirely, and diverged on
// translucency and on the owner-hidden flag. In PTTest, 110 of 142 placed meshes have more than one
// submesh, so a false cull -- itself caused by a separate viewport-mapping bug this plan's F7 fixes --
// routinely reshaded whole entities as their slot-0 material: lamps and door frames turned into rough
// metal, glass panes went opaque or vanished, emissive bulbs lost their glow. Feeding the ray-traced
// GI volume and REBLUR's denoiser history a flip between "N parts, N materials" and "1 draw, slot 0's
// material" every time a visibility verdict changed is what produced the white panels this whole
// investigation started from (see the plan's Link 5 for the denoiser mechanism; UNCONFIRMED which
// exact step crosses the radiance ceiling, but F1-F4 remove every candidate at once by construction).
//
// A PURE HEADER, DELIBERATELY, for PtRenderConflict.hpp's exact reason (see that file's own top
// comment, which this one follows line for line): no ImGui types, no SandboxApp state, no pbr::,
// no rhi::, no AVER_WARN/AVER_INFO, no globals -- plain values in, plain values out. That is what
// makes every decision below a headless unit test (tests/editor/src/SceneSubmissionTest.cpp) in a
// codebase where almost nothing about the editor's rendering walk can otherwise be tested at all.
// Every ImGui call, every pbr::MaterialLibrary/pbr::isTranslucent lookup, every AVER_WARN, and every
// rhi:: handle resolution stays at the call site in SandboxApp.cpp; only the DECISIONS move here.
//
// SurfaceInputs exists so this header never has to know what pbr::MaterialDesc or pbr::isTranslucent
// even are: the caller (SandboxApp.cpp's own resolver, still in that file) does the two lookups
// (surfaceMaterials_.find, MaterialLibrary::desc) and hands the three booleans and the looked-up
// SurfaceLook fields across as plain data.
#pragma once
#include "aver/core/Types.hpp"

namespace aver::editor {

// What the caller found out about ONE surface token before asking what it should look like.
struct SurfaceInputs {
    bool authored = false;      // surfaceMaterials_ has a handle for this token
    bool authoredLive = false;  // MaterialLibrary::desc(handle) is non-null; == authored when PBR is
                                 // compiled out (there is no library to ask, so a handle is trusted
                                 // at face value, matching SandboxApp.cpp:5881's own comment)
    bool translucent = false;   // pbr::isTranslucent(*desc) -- live handles only, meaningless otherwise
    bool haveLook = false;      // surfaceLooks_.find(token) succeeded
    f32 lookCol[3] = {0.0f, 0.0f, 0.0f};
    f32 lookMetallic = 0.0f;
    f32 lookRoughness = 0.0f;
};

// The resolved appearance for one surface, and how it got there. `blended`/`warnDeadHandle`/
// `usedFallback` default false and `col`/`metallic`/`roughness` default to the flat fallback look
// itself, so a default-constructed SurfaceLook is already the correct "nothing matched" answer --
// resolveSurfaceLook only has to OVERWRITE fields a branch actually changes, the same aggregate-init
// idiom PtRenderConflict.hpp's PtRtConflict uses for the same reason.
struct SurfaceLook {
    f32 col[4] = {0.80f, 0.80f, 0.85f, 1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    bool blended = false;
    bool warnDeadHandle = false;
    bool usedFallback = false;
};

// THE SINGLE COPY of the rule duplicated three times before this header existed -- the entity loop
// (SandboxApp.cpp:5897-5903), the off-screen-caster lambda submitShadowOnly (5584-5591), and
// drawMeshParts (8313-8324) all ran this exact if/else chain by hand, and the third copy is missing
// the liveness check the other two have (a dead handle there baked in the bright-white-mirror
// identity as the entity's FINAL look -- see 5889-5896's own comment on why that is one of the worst
// possible failure appearances). Order matters and is preserved exactly:
//   1. live authored material: the multiplicative identity (1,1,1,1)/1/1, so the material's own
//      texture/factor pair supplies everything; blended is the material's own alphaMode read.
//   2. authored but the handle no longer resolves: flag it (the caller warns once per name with
//      warnDeadMaterialHandle), then FALL THROUGH to the same look-up an unauthored surface gets --
//      a dead handle must not be trusted, but it also must not go unlit.
//   3. a named built-in SurfaceLook: its colour/metallic/roughness (SurfaceLook has no alphaMode of
//      its own, so blended stays false here -- see SandboxApp.cpp:5877's own note).
//   4. neither: the flat fallback {0.80, 0.80, 0.85, 1.0}/0/0.5, flagged so the caller can warn once.
inline SurfaceLook resolveSurfaceLook(const SurfaceInputs& in) {
    SurfaceLook look;
    if (in.authored && in.authoredLive) {
        look.col[0] = look.col[1] = look.col[2] = look.col[3] = 1.0f;
        look.metallic = 1.0f;
        look.roughness = 1.0f;
        look.blended = in.translucent;
        return look;
    }
    if (in.authored && !in.authoredLive) {
        look.warnDeadHandle = true;
        // fall through -- a dead handle is treated exactly like an unauthored surface below.
    }
    if (in.haveLook) {
        look.col[0] = in.lookCol[0];
        look.col[1] = in.lookCol[1];
        look.col[2] = in.lookCol[2];
        look.metallic = in.lookMetallic;
        look.roughness = in.lookRoughness;
        return look;
    }
    look.usedFallback = true;
    return look;
}

// One draw Voxi (or the raster device) will actually receive: which mesh, and which material token.
struct PlannedDraw {
    u32 mesh = 0;
    i32 material = 0;
};

// THE SINGLE COPY of 6360-6365 plus 8297-8299: the "a mesh that names several materials draws as
// several meshes, one per slot" rule, and its "a substituted handle keeps today's single draw and the
// entity's own material" exception (6353-6359's own comment -- a LOD level or a posed skin/soft-body
// copy is DIFFERENT geometry from the one meshParts_ was split from, so the split does not apply to
// it). `Part` is a template parameter rather than SandboxApp::MeshPart by name so this header never
// has to declare or forward-declare that type -- it only ever reads two fields off it, exactly the
// contract SandboxApp's own MeshPart already satisfies (rhi::MeshHandle mesh; i32 material;), and
// rhi::MeshHandle is a plain u32 (RHIResources.hpp), so writing it into PlannedDraw::mesh needs no
// rhi:: include either.
//
// Returns the number of entries written into `out` (capped at outCapacity, never exceeded).
template <class Part>
inline u32 planEntityDraws(u32 baseMesh, u32 chosenMesh, const Part* parts, u32 partCount,
                            i32 entityMaterial, PlannedDraw* out, u32 outCapacity) {
    if (parts != nullptr && partCount > 0 && chosenMesh == baseMesh) {
        u32 n = 0;
        for (u32 i = 0; i < partCount && n < outCapacity; ++i) {
            const Part& p = parts[i];
            if (!p.mesh) continue;   // a part whose slot named nothing carries no geometry of its own
            out[n].mesh = static_cast<u32>(p.mesh);
            out[n].material = p.material ? p.material : entityMaterial;
            ++n;
        }
        return n;
    }
    // No split applies: either this entity has no parts, or chosenMesh is a substitution (LOD/posed
    // skin/soft-body) that the split was never cut from. One draw, the entity's own material.
    if (chosenMesh != 0 && outCapacity > 0) {
        out[0].mesh = chosenMesh;
        out[0].material = entityMaterial;
        return 1;
    }
    return 0;
}

// Which of the two delivery routes an entity takes, and whether it is hidden from its own owner.
struct RouteDecision {
    bool raster = false;
    bool hiddenFromOwner = false;
    bool tint = false;
};

// hiddenFromOwner is decided ONCE, before either cull, and carried on EVERY route -- the 0d3bcf1 fix
// ("hidden=owner never reached the renderer that actually draws the image"): under ray-driven primary
// visibility the TLAS *is* what the camera sees, so an owner-hidden mesh must stay out of it exactly
// as it stays out of the raster walk, on every path this function can send it down, frustum-culled or
// not (frustum-culled AND owner-hidden must still give hiddenFromOwner true -- that combination is
// the regression 0d3bcf1 itself fixed and is pinned by a test here).
//
// raster is the ordinary "nothing is hiding or culling this entity" case, PLUS occlusion.showCulled's
// debug case: showCulled sends an otherwise-culled, non-owner-hidden entity through the raster route
// too (so it draws instead of being skipped) with tint set, so the caller can multiply its colour by
// (1, 0.15, 1) instead of drawing it unmodified. showCulled never overrides an owner-hide: a mesh
// hidden from its own owner (the camera being inside it) is not a "culled" entity in the sense this
// debug view is for, and must stay invisible regardless.
inline RouteDecision chooseRoute(bool frustumCulled, bool occlusionCulled, bool ownerHidden,
                                  bool showCulled) {
    const bool culled = frustumCulled || occlusionCulled;
    RouteDecision r;
    r.hiddenFromOwner = ownerHidden;
    r.raster = !culled && !ownerHidden;
    r.tint = false;
    if (showCulled && culled && !ownerHidden) {
        r.raster = true;
        r.tint = true;
    }
    return r;
}

// What actually reaches Voxi (or the raster device) for one planned draw, once its look and its
// route are both known.
struct VoxiDelivery {
    u32 mesh = 0;
    i32 material = 0;
    bool translucent = false;
    bool hiddenFromOwner = false;
};

// MIRRORS VoxiRenderer::submitDraw AT VoxiRenderer.cpp:861-880 EXACTLY -- deliberately not edited by
// this lane, only cited: submitDraw's `blended` parameter is read straight off IRenderFeature's own
// contract for a raster draw, and everything downstream of it (submit()'s translucent lane, the TLAS
// non-opaque flag, the exclusion from the cascade/GI shadow map/voxelisation) is driven by that one
// bool. deliver() reproduces the SAME translucent value for the direct route, so a culled pane of
// glass and a visible one agree about being translucent -- which submitShadowOnly's old, independent
// re-implementation of this test (5556-5566, now deleted by lane A) did not always do.
//
// hiddenFromOwner: the raster route never carries it (a raster draw that reached drawMesh() was never
// owner-hidden in the first place -- chooseRoute's own raster expression already excludes that case,
// so `false` here is a statement of fact, not a default silently accepted). The direct route passes
// route.hiddenFromOwner through unchanged, which is what lets voxiRenderer_.submit's own
// AVER_RT_MASK_OWNER_HIDDEN lane (voxi.hlsl:215-218) keep the mesh out of primary visibility while
// still letting it cast a shadow and contribute GI, exactly like the raster walk always did.
inline VoxiDelivery deliver(const PlannedDraw& draw, const SurfaceLook& look,
                             const RouteDecision& route) {
    VoxiDelivery d;
    d.mesh = draw.mesh;
    d.material = draw.material;
    d.translucent = look.blended;
    if (route.raster) {
        d.hiddenFromOwner = false;
    } else {
        d.hiddenFromOwner = route.hiddenFromOwner;
    }
    return d;
}

// F8's gate: should the occlusion test even run this frame. `cullEnabled` is the manifest/CLI
// RENDER.OCCLUSIONCULL setting (occlusionCullEnabled_ -- never written by this function or by its
// caller); `haveOccluder` is whether an IOcclusionCuller actually exists; `sceneSuppressed` is
// e.device()->sceneSuppressed() -- true whenever a render feature (ray-driven Voxi, Path Tracing) has
// claimed the frame and is painting the scene itself, in which case culling can save almost no work
// (every culled entity still has to be submitted for primary rays -- see the plan's F8 for the full
// accounting) so it goes idle; `runUnderSuppression` is the override
// (consoleOcclusionCullUnderSuppressionSlot, EditorConsole.hpp) that keeps it running anyway, the
// only way to exercise F1-F4's route parity in ray-driven mode once the idle is in effect. The
// override can only ever ADD a run, never remove one the manifest already turned off: with
// cullEnabled false, this returns false no matter what runUnderSuppression says.
inline bool occlusionTestShouldRun(bool cullEnabled, bool haveOccluder, bool sceneSuppressed,
                                    bool runUnderSuppression) {
    return cullEnabled && haveOccluder && (!sceneSuppressed || runUnderSuppression);
}

} // namespace aver::editor
