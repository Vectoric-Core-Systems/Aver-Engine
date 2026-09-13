// SceneSubmissionTest -- the white-panel fix's F1 decisions, all of SceneSubmission.hpp:
// planEntityDraws' part-split rule, resolveSurfaceLook's authored/look/fallback chain, chooseRoute's
// raster/direct/showCulled-tint decision, deliver()'s route-independent draw content, and F8's
// occlusionTestShouldRun gate.
//
// Header-only and dependency-free, for PtRenderConflictTest.cpp's exact reason (see that file's own
// top comment, and SceneSubmission.hpp's): no ImGui, no SandboxApp, no pbr::, no rhi::, no
// AVER_WARN, no voxi::Renderer -- every function under test takes plain bools/u32/i32/f32 in and
// returns plain data out, so the rule that a cull verdict must never change WHAT an entity delivers
// (only WHO delivers it) is reachable without a window, a device, a project file, or a scene. This is
// COMPILED, not run, by this lane's own build -- see the task's own verification rule.
#include "SceneSubmission.hpp"

#include <cstdio>

using namespace aver;
using namespace aver::editor;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("[INFO ]   ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[ERROR]   FAIL  %s\n", what);
    }
}

// Mirrors SandboxApp::MeshPart's own two fields (rhi::MeshHandle mesh; i32 material;) without
// pulling in rhi:: -- planEntityDraws is a template over `Part` for exactly this reason, so a
// stand-in with the same two members satisfies it.
struct TestPart {
    u32 mesh;
    i32 material;
};

} // namespace

int main() {
    std::printf("[INFO ] === scene submission rules (F1) ===\n");

    // ---- planEntityDraws: the single copy of SandboxApp.cpp's 6360-6365 plus 8297-8299 ----
    {
        TestPart parts[3] = {{101, 11}, {102, 12}, {103, 13}};
        PlannedDraw out[8];
        const u32 n = planEntityDraws(/*baseMesh=*/50, /*chosenMesh=*/50, parts, 3,
                                       /*entityMaterial=*/99, out, 8);
        check(n == 3, "3 parts, chosenMesh==base: 3 draws written");
        check(out[0].mesh == 101 && out[0].material == 11, "part 0 keeps its own material");
        check(out[1].mesh == 102 && out[1].material == 12, "part 1 keeps its own material");
        check(out[2].mesh == 103 && out[2].material == 13, "part 2 keeps its own material");
    }
    {
        TestPart parts[1] = {{101, 0}};
        PlannedDraw out[4];
        const u32 n = planEntityDraws(50, 50, parts, 1, 99, out, 4);
        check(n == 1 && out[0].material == 99, "a part with material 0 gets entityMaterial");
    }
    {
        TestPart parts[3] = {{101, 11}, {0, 12}, {103, 13}};
        PlannedDraw out[8];
        const u32 n = planEntityDraws(50, 50, parts, 3, 99, out, 8);
        check(n == 2, "a part with mesh 0 is skipped: 2 draws written, not 3");
        check(out[0].mesh == 101 && out[1].mesh == 103, "the surviving parts keep their order");
    }
    {
        PlannedDraw out[4];
        const u32 n = planEntityDraws<TestPart>(50, 50, nullptr, 0, 99, out, 4);
        check(n == 1 && out[0].mesh == 50 && out[0].material == 99,
              "no parts: one draw, the base mesh, the entity's own material");
    }
    {
        // A posed skin/soft-body copy or a LOD level: chosenMesh != baseMesh even though parts exist.
        // The split was cut from the UNsubstituted geometry, so it does not apply here -- one draw of
        // the substituted handle, the entity's own material (SandboxApp.cpp:6353-6359's own rule).
        TestPart parts[3] = {{101, 11}, {102, 12}, {103, 13}};
        PlannedDraw out[8];
        const u32 n = planEntityDraws(/*baseMesh=*/50, /*chosenMesh=*/777, parts, 3, 99, out, 8);
        check(n == 1 && out[0].mesh == 777 && out[0].material == 99,
              "posed/LOD substitution with parts present: one draw of the substituted mesh");
    }
    {
        TestPart parts[5] = {{101, 11}, {102, 12}, {103, 13}, {104, 14}, {105, 15}};
        PlannedDraw out[2];
        const u32 n = planEntityDraws(50, 50, parts, 5, 99, out, 2);
        check(n == 2, "capacity truncation: never more than outCapacity draws are written");
        check(out[0].mesh == 101 && out[1].mesh == 102, "...the first outCapacity parts, in order");
    }

    // ---- resolveSurfaceLook: the single copy of 5584-5591, 5897-5903 and 8313-8324 ----
    {
        SurfaceInputs in;
        in.authored = true;
        in.authoredLive = true;
        in.translucent = false;
        const SurfaceLook look = resolveSurfaceLook(in);
        check(look.col[0] == 1.0f && look.col[1] == 1.0f && look.col[2] == 1.0f && look.col[3] == 1.0f,
              "live authored, opaque: identity colour (1,1,1,1)");
        check(look.metallic == 1.0f && look.roughness == 1.0f, "live authored: identity metal/rough");
        check(!look.blended, "live authored, opaque material: blended false");
        check(!look.warnDeadHandle && !look.usedFallback, "live authored: no warn, no fallback flag");
    }
    {
        SurfaceInputs in;
        in.authored = true;
        in.authoredLive = true;
        in.translucent = true;
        check(resolveSurfaceLook(in).blended,
              "live authored, translucent material: blended follows isTranslucent");
    }
    {
        SurfaceInputs in;
        in.authored = true;
        in.authoredLive = false;
        in.haveLook = true;
        in.lookCol[0] = 0.1f; in.lookCol[1] = 0.2f; in.lookCol[2] = 0.3f;
        in.lookMetallic = 0.4f; in.lookRoughness = 0.6f;
        const SurfaceLook look = resolveSurfaceLook(in);
        check(look.warnDeadHandle, "dead authored handle: warn flag set");
        check(look.col[0] == 0.1f && look.col[1] == 0.2f && look.col[2] == 0.3f,
              "dead authored handle, look present: falls through to the look's colour");
        check(look.metallic == 0.4f && look.roughness == 0.6f, "...and the look's metal/rough");
        check(!look.usedFallback, "a look was found: the flat-fallback flag stays false");
        check(!look.blended, "a SurfaceLook has no alphaMode of its own: blended stays false");
    }
    {
        SurfaceInputs in;
        in.authored = true;
        in.authoredLive = false;
        in.haveLook = false;
        const SurfaceLook look = resolveSurfaceLook(in);
        check(look.warnDeadHandle, "dead authored handle, no look: warn flag still set");
        check(look.usedFallback, "...and it falls all the way through to the flat fallback");
        check(look.col[0] == 0.80f && look.col[1] == 0.80f && look.col[2] == 0.85f && look.col[3] == 1.0f,
              "the flat fallback colour (0.80, 0.80, 0.85, 1)");
        check(look.metallic == 0.0f && look.roughness == 0.5f, "the flat fallback metal/rough (0, 0.5)");
    }
    {
        SurfaceInputs in;
        in.authored = false;
        in.haveLook = true;
        in.lookCol[0] = 0.5f; in.lookCol[1] = 0.5f; in.lookCol[2] = 0.5f;
        in.lookMetallic = 0.2f; in.lookRoughness = 0.9f;
        const SurfaceLook look = resolveSurfaceLook(in);
        check(!look.warnDeadHandle, "unauthored surface: never warns about a dead handle");
        check(look.col[0] == 0.5f && look.metallic == 0.2f && look.roughness == 0.9f,
              "unauthored, look present: the look's own values");
        check(!look.blended, "a built-in look is never translucent");
    }
    {
        // Translucent is never true unless authored AND live -- covering the remaining combinations.
        SurfaceInputs in;
        in.authored = false;
        in.translucent = true;   // a real caller never sets this without authoredLive, but the
                                  // function must not trust it regardless of who is calling
        check(!resolveSurfaceLook(in).blended, "translucent input ignored when not authored");

        in.authored = true;
        in.authoredLive = false;
        in.haveLook = false;
        check(!resolveSurfaceLook(in).blended, "translucent input ignored when authored but dead");
    }

    // ---- chooseRoute ----
    {
        const RouteDecision r = chooseRoute(false, false, false, false);
        check(r.raster && !r.hiddenFromOwner && !r.tint, "nothing culled, not hidden: plain raster");
    }
    {
        const RouteDecision r = chooseRoute(true, false, false, false);
        check(!r.raster && !r.tint, "frustum-culled, showCulled off: direct route, no tint");
    }
    {
        const RouteDecision r = chooseRoute(false, true, false, false);
        check(!r.raster && !r.tint, "occlusion-culled, showCulled off: direct route, no tint");
    }
    {
        const RouteDecision r = chooseRoute(false, false, true, false);
        check(!r.raster && r.hiddenFromOwner, "owner-hidden alone: direct route, hiddenFromOwner true");
    }
    {
        const RouteDecision r = chooseRoute(true, false, false, true);
        check(r.raster && r.tint, "frustum-culled + showCulled: raster route, tinted");
    }
    {
        const RouteDecision r = chooseRoute(false, true, false, true);
        check(r.raster && r.tint, "occlusion-culled + showCulled: raster route, tinted");
    }
    {
        // showCulled must never override an owner-hide: the camera being inside its own character's
        // body is not the kind of "culled" this debug view exists to surface (0d3bcf1).
        const RouteDecision r = chooseRoute(true, false, true, true);
        check(!r.raster && !r.tint && r.hiddenFromOwner,
              "frustum-culled + owner-hidden + showCulled: still hidden, never tinted");
    }
    {
        const RouteDecision r = chooseRoute(false, false, false, true);
        check(r.raster && !r.tint, "showCulled on but nothing culled: plain raster, no tint");
    }

    // ---- occlusionTestShouldRun (F8) ----
    {
        check(occlusionTestShouldRun(true, true, true, true),
              "enabled, have occluder, suppressed, cullUnderSuppression: runs");
        check(!occlusionTestShouldRun(true, true, true, false),
              "enabled, have occluder, suppressed, no override: F8 idles it");
        check(occlusionTestShouldRun(true, true, false, false),
              "enabled, have occluder, not suppressed: runs regardless of the override");
        check(!occlusionTestShouldRun(false, true, true, true),
              "manifest disabled + force: force never enables culling the manifest turned off");
        check(!occlusionTestShouldRun(true, false, false, false),
              "no IOcclusionCuller object exists: never runs");
    }

    // ---- the parity principle -----------------------------------------------------------------
    // For every combination of frustum-culled, occlusion-culled and owner-hidden, with showCulled
    // false, with and without parts, and with and without a translucent part: the draws deliver()
    // would produce down the raster route and down the direct route carry the same mesh, material
    // and translucent flag for a non-owner-hidden entity -- proving F1's own rule, that a cull
    // verdict changes who delivers a draw and never what is in it. An owner-hidden entity gives
    // hiddenFromOwner true on EVERY draw regardless of route, and frustum-culled plus owner-hidden
    // still gives hiddenFromOwner true -- the 0d3bcf1 regression this whole plan traces back to.
    {
        const bool bools[2] = {false, true};
        for (bool frustumCulled : bools)
        for (bool occlusionCulled : bools)
        for (bool ownerHidden : bools)
        for (bool withParts : bools)
        for (bool translucentPart : bools) {
            const RouteDecision route =
                chooseRoute(frustumCulled, occlusionCulled, ownerHidden, /*showCulled=*/false);

            TestPart parts[2] = {{201, 21}, {202, 22}};
            PlannedDraw planned[4];
            u32 n = 0;
            if (withParts) {
                n = planEntityDraws(50, 50, parts, 2, 99, planned, 4);
            } else {
                n = planEntityDraws<TestPart>(50, 50, nullptr, 0, 99, planned, 4);
            }

            SurfaceLook look;
            look.blended = translucentPart;

            // The SAME planned draws and the SAME look, delivered as if by BOTH routes -- exactly
            // the invariant deliver() exists to guarantee: only `route` may move hiddenFromOwner,
            // never mesh, material or translucent.
            RouteDecision asRaster = route; asRaster.raster = true;
            RouteDecision asDirect = route; asDirect.raster = false;

            bool ok = true;
            for (u32 i = 0; i < n; ++i) {
                const VoxiDelivery viaRaster = deliver(planned[i], look, asRaster);
                const VoxiDelivery viaDirect = deliver(planned[i], look, asDirect);
                if (viaRaster.mesh != viaDirect.mesh || viaRaster.material != viaDirect.material ||
                    viaRaster.translucent != viaDirect.translucent) {
                    ok = false;
                }
                if (!ownerHidden) {
                    // The equality claim is scoped to non-owner-hidden entities: a raster draw never
                    // carries hiddenFromOwner (chooseRoute never sets raster true while ownerHidden
                    // is true in the first place, so `false` there is a statement of fact), and here
                    // the direct route's own hiddenFromOwner is false too.
                    if (viaRaster.hiddenFromOwner || viaDirect.hiddenFromOwner) ok = false;
                } else if (!viaDirect.hiddenFromOwner) {
                    ok = false;
                }
            }
            check(ok, "raster/direct parity holds for this (frustum, occlusion, ownerHidden, parts, "
                      "translucent) combination");

            // The REAL route this combination gets from chooseRoute, delivered for real.
            for (u32 i = 0; i < n; ++i) {
                const VoxiDelivery d = deliver(planned[i], look, route);
                if (ownerHidden) {
                    check(d.hiddenFromOwner,
                          "owner-hidden entity: hiddenFromOwner true on every delivered draw");
                } else {
                    check(!d.hiddenFromOwner,
                          "non-owner-hidden entity: hiddenFromOwner false on every delivered draw");
                }
            }
        }
    }
    {
        // The 0d3bcf1 case, named explicitly rather than left to only be covered by the sweep above:
        // frustum-culled AND owner-hidden must still hide the entity from its own owner.
        const RouteDecision route = chooseRoute(/*frustumCulled=*/true, false, /*ownerHidden=*/true,
                                                 false);
        const PlannedDraw draw{123, 45};
        const SurfaceLook look;
        check(deliver(draw, look, route).hiddenFromOwner,
              "0d3bcf1 regression case: frustum-culled + owner-hidden still hides from its own owner");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
