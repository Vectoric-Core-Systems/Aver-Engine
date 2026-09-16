// The world draw walk: every live entity carrying a visible CMeshRenderer.
//
// Lifted from SandboxApp.cpp:1386-1510. What is NOT here is as deliberate as what is: no selection
// latch, no selection outline, no grid, no gizmo, no skin-scene-test recolour, no objects_
// placeholder pass, and no capture/gate harness. Those are the editor looking at a world; this is a
// game showing one.
//
// What IS here, since SandboxRender.cpp's F4 fix ("unified direct route", SceneSubmission.hpp): a
// frustum-culled or DrawWorldOptions::ownerHideRoot-hidden entity is not dropped, it is handed
// straight to DrawWorldOptions::voxiRenderer (when one is attached) so shadows, GI voxelisation and
// the RT TLAS never depend on what the raster camera can see. Only the raster drawMesh() call is
// skipped for it.
#include "aver/game/GameRender.hpp"

#if AVER_MODULE_SCENE

#include "aver/game/GameContent.hpp"
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
        // submit() -- mirroring SceneSubmission.hpp's chooseRoute() (ported rather than included: this
        // library must not include a sandbox/ header). No occlusion culling exists in this walk (that
        // machinery is editor-only, see this file's own header comment), so chooseRoute's occlusionCulled
        // input is always false here.
        const bool raster = !frustumCulled && !ownerHiddenHere;

        // Colour: the named-surface look, or a neutral default. The authored-material half
        // (surfaceMaterials_ and MaterialSystem) is C8's; until then a level's M_* surfaces already
        // render with the colours the editor gives them.
        f32 col[4] = {0.80f, 0.80f, 0.85f, 1.0f};
        f32 metallic = 0.0f, roughness = 0.5f;

        u32 authored = 0;
        // Whether this entity's resolved material takes the blended draw path near the end of this
        // loop. Only an AUTHORED .ocmat can ever set this true: a built-in SurfaceLook (three floats
        // plus metal/rough -- see GameContent.cpp's registerBuiltins) has no BLEND record at all, and
        // neither does the flat-gray fallback, so the `else`/`else if` below never touches it. Read
        // straight from MaterialLibrary rather than trusting a value cached anywhere on GameContent,
        // because the library is the one place an .ocmat's alphaMode can change after load (the
        // material editor writes through it), and a cached copy would survive a hot-reload the mesh
        // itself did not.
        bool blended = false;
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        // THE SAME FALLBACK THE EDITOR APPLIES: 0 means "ask the mesh", not "no material".
        // Resolved into a local rather than inline so every later read in this loop -- the
        // blended test, the binding set, the per-draw colour -- sees the one value, the way
        // the editor's own `mat` local does.
        const i32 mat = mr->material ? mr->material : content.meshDefaultMaterial(mr->mesh);
        authored = content.authoredFor(mat);
        if (authored) {
            // The packaged game's copy of the same rule. BOTH ROOTS OR NEITHER -- a predicate the
            // editor honours and the game does not is this repo's most-repeated defect shape.
            if (const pbr::MaterialDesc* d = pbr::MaterialLibrary::get().desc(authored))
                blended = pbr::isTranslucent(*d);
        }
#endif
        if (authored) {
            // An AUTHORED material supplies its own colour and its own metal/rough through the
            // binding set below, so the per-draw values are neutralised to 1 rather than left as
            // the fallback. Multiplying an authored albedo by 0.8 grey is the classic way to get a
            // world that looks correct but uniformly dingy.
            col[0] = col[1] = col[2] = 1.0f;
            metallic = roughness = 1.0f;
        } else if (const GameContent::SurfaceLook* look = content.lookFor(mat)) {
            col[0] = look->col[0]; col[1] = look->col[1]; col[2] = look->col[2];
            metallic = look->metallic; roughness = look->roughness;
        } else if (mat != 0) {
            // Neither an authored .ocmat nor a built-in SurfaceLook claimed this entity's named
            // surface -- it is about to draw the flat 0.80/0.80/0.85 gray fallback above with no
            // record anywhere that anything went wrong. This is EXACTLY the failure a prior
            // investigation traced to a parity gap between this table and the editor's: three names
            // (M_Foliage, M_Bark, M_Rock) were registered in sandbox/src/SandboxApp.cpp but not here,
            // so a level authored in the editor rendered its intended colour there and silently fell
            // through to gray the moment the packaged game ran it. That specific gap is closed in
            // GameContent.cpp's registerBuiltins now, but nothing stops the next one -- a look added
            // to the editor's table and never mirrored into this one reproduces the identical silent
            // failure. This warning is the backstop for that.
            //
            // ONE-SHOT, and function-local rather than a member on GameContent (which this file does
            // not own): drawWorld runs every frame for every visible entity, so without a latch a
            // level with a single unresolved surface used by fifty placements would log fifty lines
            // EVERY FRAME rather than once, ever. Keyed on the interned token so the check is one
            // hash-set lookup, not a string compare; reported by NAME via aver_scene_material_name
            // because the token's value is process-startup-order dependent (scene_abi.h's own
            // comment on that function) and means nothing to a person reading the log.
            static std::unordered_set<i32> warnedUnresolvedMaterials;
            if (warnedUnresolvedMaterials.insert(mat).second) {
                AVER_WARN("[Game] surface '{}' has no authored .ocmat and no built-in look; "
                          "rendering the flat gray fallback (0.80, 0.80, 0.85) instead",
                          aver_scene_material_name(mat));
            }
        }

        // Resolved once per entity, regardless of which route delivers it: the raster route feeds
        // these to device.setDrawBinding's STICKY state below, the direct route feeds the SAME values
        // straight to voxiRenderer->submit()'s drawBinding/drawConstants/drawConstantBytes -- one
        // resolve, like SceneSubmission.hpp's deliver() reading a single ResolvedSurface for both.
        rhi::BindingSetHandle matSet = 0;
        const void* matConstants = nullptr;
        u32 matBytes = 0;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // Guarded on ready(): binding a descriptor table the material system has not built is not a
        // wrong colour on this renderer, it is a GPU hang. This project has already lost a session
        // to an unbound root CBV that presented as "slow geometry shaders".
        if (materials && materials->ready()) {
            matSet = materials->bindingSet(authored);
            matConstants = &materials->constants(authored);
            matBytes = sizeof(pbr::MaterialConstants);
        }
#else
        (void)materials;
#endif

        // THE SEAM, and it is one line because the design made it one. A skinned entity's posed
        // vertices live in a DIFFERENT MeshHandle sharing this one's index buffer, so substituting
        // the handle reaches every pass at once. Zero means "not skinned", never "not drawn".
        rhi::MeshHandle drawHandle = handle;
        if (skinning) {
            if (const rhi::MeshHandle sk = skinning->drawHandle(ent)) drawHandle = sk;
        }

        if (raster) {
            if (matBytes) device.setDrawBinding(matSet, matConstants, matBytes);

            // STICKY on the device (RHI.hpp's setDrawBlended comment), so it is set on EVERY draw
            // here, not only when true. Skipping the false case would leave a translucent entity's
            // flag set for whatever opaque entity this walk visits next -- that next mesh would
            // silently take the blended path too: no ray-traced shadow, no GI bounce, no shadow-
            // cascade write, and drawn through scenePipeline(..., blended=true) instead of the
            // ordinary opaque pipeline, purely because it happened to be drawn after a pane of glass.
            //
            // NO DEPTH-PREPASS GUARD NEEDED HERE, and that is a fact about this file rather than about
            // translucency: drawWorld never calls setNextDrawPrepassed or drawMeshDepthPrepass at all
            // -- this is the packaged-game walk, not SandboxApp.cpp's editor loop with its
            // `prepassEligible` exclusion list (SandboxApp.cpp, search "depth prepass phase").
            // setDrawBlended's own contract already guarantees a blended draw is "never depth-
            // prepassed" regardless, because the device captures it at the very top of drawMesh,
            // before the same submitDraw loop a prepass would also have to be skipped ahead of. If a
            // depth-prepass walk is ever added to this file, it must exclude exactly the entities
            // `blended` is true for here, the same way SandboxApp.cpp's prepassEligible excludes
            // skinned and GPU-cluster-dispatched ones -- stated here so that addition does not have to
            // rediscover it.
            device.setDrawBlended(blended);
            device.drawMesh(drawHandle, &wm.m[0][0], col, metallic, roughness);
            ++drawn;
        } else {
#if AVER_MODULE_VOXI
            // THE UNIFIED DIRECT ROUTE: frustum-culled or owner-hidden, handed straight to Voxi so
            // shadows, GI voxelisation and the RT TLAS never depend on what the raster camera can see
            // -- mirrors emitEntityDraws' else-branch (SandboxRender.cpp:2195-2226) calling
            // voxiRenderer_.submit() with the SAME mesh/look/translucency this entity would have drawn
            // with on the raster route, plus ownerHiddenHere so a possessed pawn's own body stays out
            // of ray-driven primary visibility (voxi.hlsl's AVER_RT_MASK_OWNER_HIDDEN lane) while still
            // casting a shadow and bouncing light, exactly like the raster walk always did for it.
            // options.voxiRenderer null (no Voxi feature attached) reproduces this walk's pre-existing
            // behaviour exactly: the entity is skipped and casts nothing while culled or hidden.
            if (options.voxiRenderer) {
                options.voxiRenderer->submit(drawHandle, &wm.m[0][0], col, metallic, roughness,
                                             matSet, matConstants, matBytes,
                                             /*translucent=*/blended, ownerHiddenHere);
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
