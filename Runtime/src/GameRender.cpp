// The world draw walk: every live entity carrying a visible CMeshRenderer.
//
// Lifted from SandboxApp.cpp:1386-1510. What is NOT here is as deliberate as what is: no selection
// latch, no selection outline, no grid, no gizmo, no skin-scene-test recolour, no objects_
// placeholder pass, and no capture/gate harness. Those are the editor looking at a world; this is a
// game showing one.
//
// What IS here, since SandboxRender.cpp's F4 fix ("unified direct route",
// aver/game/SceneSubmission.hpp): a frustum-culled or DrawWorldOptions::ownerHideRoot-hidden entity
// is not dropped, it is handed straight to DrawWorldOptions::voxiRenderer (when one is attached) so
// shadows, GI voxelisation and the RT TLAS never depend on what the raster camera can see. Only the
// raster drawMesh() call is skipped for it.
//
// A mesh GameContent split into per-material parts (GameContent::partsFor) draws as one PlannedDraw
// per part, on both routes, each part resolving its own material through the same authored > look >
// fallback ladder. Both of those rules are now INCLUDED from aver/game/SceneSubmission.hpp rather
// than hand-kept here: while that header sat in sandbox/src this file carried a copy of PlannedDraw,
// kMaxPlannedDraws, planEntityDraws and the ladder, and the copies had already drifted -- this one's
// ladder branched on `if (authored)` with no liveness test, so a stale material handle painted the
// surface as a white metal mirror instead of falling through to its named look. The header lives in
// this library's own public include directory now (Runtime/include, exposed PUBLICly by
// cmake/AvModule.cmake:15), so the editor (sandbox/src/SandboxRender.cpp's resolveSurface and
// emitEntityDraws) and this walk read the identical decisions.
#include "aver/game/GameRender.hpp"

#if AVER_MODULE_SCENE

#include "aver/game/GameContent.hpp"
#include "aver/game/SceneSubmission.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#if AVER_MODULE_PBR
#  include "aver/pbr/MaterialSystem.hpp"
#endif
#include "aver/render/SkinnedScene.hpp"
#if AVER_MODULE_VOXI
#  include "aver/voxi/VoxiRenderer.hpp"
#endif
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "GameMath.hpp"

#include <cmath>
#include <unordered_set>

namespace aver::game {

void drawWorld(rhi::IDevice& device, const Mat4& viewProj, GameContent& content, SceneDrawStats& stats,
               pbr::MaterialSystem* materials, render::SkinnedScene* skinning,
               const DrawWorldOptions& options) {
    scene::World& w = scene::World::instance();
    int drawn = 0, culled = 0, ownerHidden = 0;

    // The six frustum planes, from the camera's viewProj. ENGINE convention: row-vector, so a clip
    // coordinate is a dot with a COLUMN, and each plane is a sum or difference of two columns.
    // Derived per frame rather than cached: it is two dozen adds, and a stale frustum culls things
    // that are on screen. Left UNNORMALISED -- only the sign of the distance is ever read.
    f32 pl[6][4];
    {
        const Mat4& m = viewProj;
        for (int i = 0; i < 4; ++i) {
            pl[0][i] = m.m[i][3] + m.m[i][0];   // left
            pl[1][i] = m.m[i][3] - m.m[i][0];   // right
            pl[2][i] = m.m[i][3] + m.m[i][1];   // bottom
            pl[3][i] = m.m[i][3] - m.m[i][1];   // top
            pl[4][i] = m.m[i][2];               // near   ([0,1] depth, so no m[i][3] term)
            pl[5][i] = m.m[i][3] - m.m[i][2];   // far
        }
    }

    const u32 n = w.count();
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity ent = w.at(i);
        if (w.destroyPending(ent)) continue;
        const scene::CMeshRenderer* mr =
            w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
        if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;

        const rhi::MeshHandle handle = content.meshFor(mr->mesh);
        if (!handle) continue;

        // worldMatrix is non-const on World, which is why this takes a non-const World&.
        const Mat4& wm = w.worldMatrix(ent);

        // A STATIC entity gets its bounds from the asset. A SKINNED one already had them written
        // this frame by SkinnedScene from its ACTUAL POSE, so leave those alone -- overwriting with
        // the rest box is exactly the popping the posed-bounds work exists to stop. The guard was
        // written before skinning was wired; it is live now.
        const bool skinned = skinning && skinning->drawHandle(ent) != 0;
        if (!skinned)
        if (const auto* b = content.boundsFor(mr->mesh)) {
            auto* mw = const_cast<scene::CMeshRenderer*>(mr);
            mw->aabbMin[0] = b->first.x;  mw->aabbMin[1] = b->first.y;  mw->aabbMin[2] = b->first.z;
            mw->aabbMax[0] = b->second.x; mw->aabbMax[1] = b->second.y; mw->aabbMax[2] = b->second.z;
        }

        // ---- OWNER HIDE, DECIDED BEFORE THE CULL ----
        // Mirrors SandboxRender.cpp:920-937's ancestor walk against firstPersonPawn_: an entity both
        // frustum-culled and owner-hidden must still carry hiddenFromOwner=true on its direct-route
        // delivery below, or it would be primary-visible again in ray-driven mode the moment it comes
        // back on screen (the 0d3bcf1 regression SceneSubmission.hpp's chooseRoute was written to
        // pin a test against). ANCESTOR WALK, NOT A DIRECT-PARENT COMPARE: a COMP tree can nest, so
        // "this mesh's owner" may be several hops above `ent`. World::setParent already refuses a
        // cycle, so this walk is guaranteed to reach kInvalidEntity and stop.
        bool ownerHiddenHere = false;
        if ((mr->flags & scene::kMeshRendererHiddenFromOwner) &&
            options.ownerHideRoot != scene::kInvalidEntity) {
            for (scene::Entity anc = ent; w.valid(anc); anc = w.parent(anc)) {
                if (anc == options.ownerHideRoot) { ownerHiddenHere = true; break; }
            }
        }

        // Frustum cull on the world-space extent of the entity's own box. A DEGENERATE box is DRAWN
        // rather than culled: an entity whose bounds were never filled in must not vanish, and being
        // conservative costs a draw call where being wrong costs a character.
        //
        // THE VERDICT IS STORED, NOT ACTED ON HERE. Frustum-culled and owner-hidden used to `continue`
        // straight past every draw call below, which means past the ONLY thing that reaches Voxi's
        // submitDraw() too -- an off-screen shadow caster's shadow vanished the instant it left the
        // frustum, and an owner-hidden mesh's shadow never existed at all. SandboxRender.cpp's F4 fix
        // (SceneSubmission.hpp, "unified direct route") routes a culled/hidden entity to Voxi directly
        // instead of dropping it; see the route decision a few lines down.
        bool frustumCulled = false;
        {
            const Vec3 lo{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            const Vec3 hi{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
                for (u32 c = 0; c < 8; ++c) {
                    const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                    const Vec3 t = xformPoint(wm, p);
                    wlo.x = std::fmin(wlo.x, t.x); whi.x = std::fmax(whi.x, t.x);
                    wlo.y = std::fmin(wlo.y, t.y); whi.y = std::fmax(whi.y, t.y);
                    wlo.z = std::fmin(wlo.z, t.z); whi.z = std::fmax(whi.z, t.z);
                }
                bool outside = false;
                for (u32 pi = 0; pi < 6 && !outside; ++pi) {
                    // The corner FURTHEST along the plane normal. If even that one is behind, every
                    // corner is, and only then is the box definitely out.
                    const f32 d = pl[pi][0] * (pl[pi][0] > 0 ? whi.x : wlo.x)
                                + pl[pi][1] * (pl[pi][1] > 0 ? whi.y : wlo.y)
                                + pl[pi][2] * (pl[pi][2] > 0 ? whi.z : wlo.z)
                                + pl[pi][3];
                    if (d < 0.0f) outside = true;
                }
                frustumCulled = outside;
            }
        }
        // THE ONE PLACE that decides who delivers this entity -- raster's drawMesh() or Voxi's direct
        // submit(). This is chooseRoute()'s own raster expression (aver/game/SceneSubmission.hpp)
        // with both of its editor-only inputs pinned false: no occlusion culling exists in this walk
        // (that machinery is editor-only, see this file's own header comment) and there is no
        // occlusion.showCulled debug view to tint anything, so chooseRoute would return
        // {raster = !frustumCulled && !ownerHiddenHere, hiddenFromOwner = ownerHiddenHere, tint =
        // false} -- three values this walk already has as two locals. Left spelled out rather than
        // called, unlike planEntityDraws and resolveSurfaceLook below, because there is no decision
        // left to share once the inputs are constants.
        const bool raster = !frustumCulled && !ownerHiddenHere;

        // THE SEAM, and it is one line because the design made it one. A skinned entity's posed
        // vertices live in a DIFFERENT MeshHandle sharing this one's index buffer, so substituting
        // the handle reaches every pass at once. Zero means "not skinned", never "not drawn". Computed
        // here, ahead of the draw plan below, because planEntityDraws needs to compare it against the
        // UNSUBSTITUTED `handle` to know whether this entity's own per-material split still applies --
        // a posed copy is different geometry from the one the split was cut from (see planEntityDraws'
        // own comment above).
        rhi::MeshHandle drawHandle = handle;
        if (skinning) {
            if (const rhi::MeshHandle sk = skinning->drawHandle(ent)) drawHandle = sk;
        }

        // THE ENTITY'S OWN FALLBACK MATERIAL: 0 means "ask the mesh", not "no material" -- the same
        // rule the editor applies. Needed unconditionally now (not only inside the PBR branch below):
        // planEntityDraws' single-draw case reads it directly, and it is what a split part's own empty
        // slot falls back to (planEntityDraws' `p.material ? p.material : entityMaterial`).
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        const i32 mat = mr->material ? mr->material : content.meshDefaultMaterial(mr->mesh);
#else
        const i32 mat = mr->material;
#endif

        // ONE MESH THAT NAMES SEVERAL MATERIALS DRAWS AS SEVERAL MESHES, ONE PER SLOT -- the split
        // GameContent::loadProjectMeshes built (mirroring SandboxApp::buildMeshParts), planned by the
        // shared planEntityDraws(), called with the same `parts ? parts->data() : nullptr, count`
        // shape SandboxRender.cpp's own three call sites use. An entity with no parts (the common
        // case) plans to exactly the one draw this loop always issued.
        //
        // GameContent::MeshPart satisfies planEntityDraws' `Part` template parameter without the
        // header having to name it: it reads exactly the two fields MeshPart declares
        // (rhi::MeshHandle mesh; i32 material;), and rhi::MeshHandle is a plain alias for u32
        // (modules/rhi/include/aver/rhi/RHIResources.hpp:13), so PlannedDraw::mesh being spelled u32
        // there costs nothing here -- it is the identical type, not a laundered one.
        const std::vector<GameContent::MeshPart>* parts = content.partsFor(mr->mesh);
        PlannedDraw pdraws[kMaxPlannedDraws];
        const u32 pdrawCount = planEntityDraws(
            handle, drawHandle,
            parts ? parts->data() : nullptr,
            parts ? static_cast<u32>(parts->size()) : 0u,
            mat, pdraws, kMaxPlannedDraws);

        // Resolves one planned draw's material token into what it needs to draw with -- the shared
        // authored > dead-handle > named-look > flat-fallback ladder (resolveSurfaceLook,
        // aver/game/SceneSubmission.hpp), plus the MaterialSystem binding -- so a multi-part entity
        // resolves each part's OWN token through the exact same ladder the single-material path
        // always used for the entity's. Mirrors SandboxApp::resolveSurface
        // (sandbox/src/SandboxRender.cpp) field for field, down to holding the resolved SurfaceLook
        // whole instead of unpacking it, and is called once per planned draw the way that file's own
        // emitEntityDraws() calls resolveSurface() once per draw rather than once per entity.
        //
        // CALLING resolveSurfaceLook IS A BUG FIX HERE, not a deduplication. This lambda used to ask
        // MaterialLibrary for the desc only to read alphaMode off it, then take the authored branch
        // on `if (authored)` alone -- no liveness test at all. A token whose handle content.authoredFor
        // still returns but which MaterialLibrary::desc() no longer resolves therefore drew as the
        // multiplicative identity, col 1/1/1/1 with metallic and roughness both 1: a pure white
        // mirror, which reads as confident lighting rather than as missing content and so is among
        // the worst appearances a content error can take (SandboxAssets.cpp's warnDeadMaterialHandle
        // makes the same argument for the editor). resolveSurfaceLook demands authored AND
        // authoredLive and otherwise falls THROUGH to the named look or the flat fallback, so that
        // stale handle now draws something honest and says so once.
        auto resolveDrawLook = [&](i32 m) {
            struct DrawLook {
                // The ladder's verdict, unedited. `look.blended` can only ever be true for a LIVE
                // authored .ocmat: a built-in SurfaceLook (three floats plus metal/rough -- see
                // GameContent.cpp's registerBuiltins) has no BLEND record at all, and neither does
                // the flat-gray fallback, so no other rung can set it.
                SurfaceLook look;
                rhi::BindingSetHandle matSet = 0;
                const void* matConstants = nullptr;
                u32 matBytes = 0;
            } dl;

            SurfaceInputs in;
            u32 authored = 0;
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
            authored = content.authoredFor(m);
            in.authored = authored != 0;
            // THE HOST'S HALF, not a second copy of the rule: SceneSubmission.hpp deliberately
            // knows nothing of pbr::MaterialDesc or pbr::isTranslucent (it is a pure header so its
            // decisions can be unit-tested with no device and no RHI), so each host performs these
            // two lookups itself and hands the answers across as the plain booleans SurfaceInputs
            // declares. What is shared is what they MEAN, which is the part that drifted. Read
            // straight from MaterialLibrary rather than trusting a value cached anywhere on
            // GameContent, because the library is the one place an .ocmat's alphaMode can change
            // after load (the material editor writes through it), and a cached copy would survive
            // a hot-reload the mesh itself did not. The SAME desc pointer answers both questions the
            // header asks -- whether the handle is still live, and whether it is translucent -- so
            // the liveness test the old ladder was missing costs no extra lookup, it was already
            // being performed and thrown away.
            const pbr::MaterialDesc* d =
                authored ? pbr::MaterialLibrary::get().desc(authored) : nullptr;
            in.authoredLive = d != nullptr;
            in.translucent = d != nullptr && pbr::isTranslucent(*d);
#endif
            // Asked UNCONDITIONALLY, not only when nothing is authored, because a dead authored
            // handle falls through to exactly this rung -- the whole point of the fix. One extra
            // hash lookup per authored draw, which is what SandboxApp::resolveSurface already pays.
            if (const GameContent::SurfaceLook* builtin = content.lookFor(m)) {
                in.haveLook = true;
                in.lookCol[0] = builtin->col[0];
                in.lookCol[1] = builtin->col[1];
                in.lookCol[2] = builtin->col[2];
                in.lookMetallic = builtin->metallic;
                in.lookRoughness = builtin->roughness;
            }
            dl.look = resolveSurfaceLook(in);

            if (dl.look.warnDeadHandle) {
                // EACH HOST WORDS ITS OWN SENTENCE from the shared flag -- the same split
                // GameTick.hpp already states outright ("Both pass their own host tag", :19) and
                // GameCamera.hpp uses for its aspect (:11). The editor's warnDeadMaterialHandle
                // (sandbox/src/SandboxAssets.cpp:602) says "[Editor]" and speaks about a library the
                // material editor writes through, neither of which a packaged game has any business
                // claiming. Only the DECISION is shared; the message is not, and unifying the two
                // texts would mean one of them lying about where to go and look.
                //
                // NOT KNOWN TO FIRE, exactly as on the editor's side -- this is a latent hazard
                // closed on inspection, not a reproduced bug. If this line ever appears in a log,
                // that is new information worth chasing.
                static std::unordered_set<i32> warnedDeadMaterialHandles;
                if (warnedDeadMaterialHandles.insert(m).second) {
                    AVER_WARN("[Game] surface '{}' holds a material handle that no longer resolves "
                              "in pbr::MaterialLibrary; falling through to its built-in look or the "
                              "flat gray fallback. Trusted at face value it would have drawn as a "
                              "bright white mirror",
                              aver_scene_material_name(m));
                }
            }
            if (dl.look.usedFallback && m != 0) {
                // Neither an authored .ocmat nor a built-in SurfaceLook claimed this surface -- it is
                // about to draw the flat 0.80/0.80/0.85 gray fallback with no record anywhere that
                // anything went wrong. This is EXACTLY the failure a prior investigation traced to
                // a parity gap between this table and the editor's: three names (M_Foliage, M_Bark,
                // M_Rock) were registered in sandbox/src/SandboxApp.cpp but not here, so a level
                // authored in the editor rendered its intended colour there and silently fell through
                // to gray the moment the packaged game ran it. That specific gap is closed in
                // GameContent.cpp's registerBuiltins now, but nothing stops the next one -- a look
                // added to the editor's table and never mirrored into this one reproduces the identical
                // silent failure. This warning is the backstop for that.
                //
                // STILL GATED ON A NON-ZERO TOKEN: material 0 is "this entity named no material",
                // which every untextured placeholder in a scene legitimately is, and warning about
                // it once per launch would be noise that trains a reader to ignore the line that
                // matters. resolveSurfaceLook sets usedFallback for token 0 too -- it has no way to
                // tell "unnamed" from "named and missing" -- so the gate belongs here, at the caller
                // that knows, which is where SandboxRender.cpp:2028 keeps its own.
                //
                // ONE-SHOT, and function-local (a lambda's local static is exactly as permanent as a
                // plain function's -- initialised once, ever, not once per call). Keyed on the interned
                // token so the check is one hash-set lookup, not a string compare; reported by NAME via
                // aver_scene_material_name because the token's value is process-startup-order dependent
                // (scene_abi.h's own comment on that function) and means nothing to a person reading
                // the log.
                static std::unordered_set<i32> warnedUnresolvedMaterials;
                if (warnedUnresolvedMaterials.insert(m).second) {
                    AVER_WARN("[Game] surface '{}' has no authored .ocmat and no built-in look; "
                              "rendering the flat gray fallback (0.80, 0.80, 0.85) instead",
                              aver_scene_material_name(m));
                }
            }
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
            // Guarded on ready(): binding a descriptor table the material system has not built is not a
            // wrong colour on this renderer, it is a GPU hang. This project has already lost a session
            // to an unbound root CBV that presented as "slow geometry shaders".
            //
            // THE RAW AUTHORED HANDLE, BOUND EVEN WHEN resolveSurfaceLook JUST REJECTED IT AS DEAD.
            // That looks inconsistent and is deliberate parity: SandboxApp::resolveSurface sets its
            // ResolvedSurface::authored from content_.authoredFor with no liveness test and hands
            // THAT to MaterialSystem::bindingSet/constants, so binding the same handle here is what
            // keeps the editor the reference for what a stale handle looks like. MaterialSystem is
            // also a different table from MaterialLibrary -- a handle the library has dropped may
            // still have a built descriptor set here -- and either way the colour the dead handle
            // now draws with comes from dl.look, which never trusted it.
            if (materials && materials->ready()) {
                dl.matSet = materials->bindingSet(authored);
                dl.matConstants = &materials->constants(authored);
                dl.matBytes = sizeof(pbr::MaterialConstants);
            }
#else
            (void)materials;
            // With PBR compiled out `authored` is assigned zero above and read nowhere at all, and
            // the binding block that is its only other reader is gone with this branch -- so say so
            // rather than let /W4 (CMakeLists.txt:202) report it as a local nobody wanted.
            (void)authored;
#endif
            return dl;
        };

        if (raster) {
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                if (dl.matBytes) device.setDrawBinding(dl.matSet, dl.matConstants, dl.matBytes);

                // STICKY on the device (RHI.hpp's setDrawBlended comment), so it is set on EVERY draw
                // here, not only when true. Skipping the false case would leave a translucent part's
                // flag set for whatever opaque part or entity this walk visits next -- that next mesh
                // would silently take the blended path too: no ray-traced shadow, no GI bounce, no
                // shadow-cascade write, and drawn through scenePipeline(..., blended=true) instead of
                // the ordinary opaque pipeline, purely because it happened to be drawn after a pane of
                // glass.
                //
                // NO DEPTH-PREPASS GUARD NEEDED HERE, and that is a fact about this file rather than
                // about translucency: drawWorld never calls setNextDrawPrepassed or
                // drawMeshDepthPrepass at all -- this is the packaged-game walk, not SandboxApp.cpp's
                // editor loop with its `prepassEligible` exclusion list (SandboxApp.cpp, search "depth
                // prepass phase"). setDrawBlended's own contract already guarantees a blended draw is
                // "never depth-prepassed" regardless, because the device captures it at the very top of
                // drawMesh, before the same submitDraw loop a prepass would also have to be skipped
                // ahead of. If a depth-prepass walk is ever added to this file, it must exclude exactly
                // the draws `blended` is true for here, the same way SandboxApp.cpp's prepassEligible
                // excludes skinned and GPU-cluster-dispatched ones -- stated here so that addition does
                // not have to rediscover it.
                device.setDrawBlended(dl.look.blended);
                device.drawMesh(pd.mesh, &wm.m[0][0], dl.look.col, dl.look.metallic,
                                dl.look.roughness);
            }
            ++drawn;
        } else {
#if AVER_MODULE_VOXI
            // THE UNIFIED DIRECT ROUTE: frustum-culled or owner-hidden, handed straight to Voxi so
            // shadows, GI voxelisation and the RT TLAS never depend on what the raster camera can see
            // -- mirrors emitEntityDraws' else-branch (SandboxRender.cpp:2195-2226) calling
            // voxiRenderer_.submit() with the SAME mesh/look/translucency this draw would have used on
            // the raster route, plus ownerHiddenHere so a possessed pawn's own body stays out of
            // ray-driven primary visibility (voxi.hlsl's AVER_RT_MASK_OWNER_HIDDEN lane) while still
            // casting a shadow and bouncing light, exactly like the raster walk always did for it. Now
            // per planned draw, not per entity, so a culled multi-material entity's parts reach Voxi
            // with their own materials instead of all borrowing the entity's -- the same split the
            // raster route above now gets. options.voxiRenderer null (no Voxi feature attached)
            // reproduces this walk's pre-existing behaviour exactly: the entity is skipped and casts
            // nothing while culled or hidden.
            if (options.voxiRenderer) {
                for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                    const PlannedDraw& pd = pdraws[pdi];
                    if (!pd.mesh) continue;
                    const auto dl = resolveDrawLook(pd.material);
                    options.voxiRenderer->submit(pd.mesh, &wm.m[0][0], dl.look.col, dl.look.metallic,
                                                 dl.look.roughness, dl.matSet, dl.matConstants,
                                                 dl.matBytes, /*translucent=*/dl.look.blended,
                                                 ownerHiddenHere);
                }
            }
#endif
            // Priority matches SandboxRender.cpp:1086's counting convention: a frustum-culled-AND-
            // owner-hidden entity counts as culled, never as owner-hidden, even though its direct-route
            // delivery above always carried hiddenFromOwner=true regardless -- a counting convention
            // only, not a correctness question (chosen above, unconditionally).
            if (frustumCulled) ++culled; else ++ownerHidden;
        }
    }

    if (drawn != stats.lastDrawn || culled != stats.lastCulled || ownerHidden != stats.lastOwnerHidden) {
        AVER_INFO("[Game] scene-render: {} drawn, {} frustum-culled, {} owner-hidden",
                  drawn, culled, ownerHidden);
        stats.lastDrawn = drawn;
        stats.lastCulled = culled;
        stats.lastOwnerHidden = ownerHidden;
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
