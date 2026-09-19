// The world draw walk: every live entity carrying a visible CMeshRenderer.
//
// Lifted from SandboxApp.cpp:1386-1510. What is NOT here is as deliberate as what is: no selection
// latch, no selection outline, no grid, no gizmo, no skin-scene-test recolour, no objects_
// placeholder pass, and no capture/gate harness. Those are the editor looking at a world; this is a
// game showing one.
//
// WHAT THE HOOKS IN DrawWorldOptions ARE FOR, and why they are not that list creeping back in. Two
// hosts run this walk: the shipped game, through GameApp, and the editor, whose own copy is still
// inline in SandboxApp::onRender (sandbox/src/SandboxRender.cpp). The editor's is the behaviour
// reference and carries machinery this library does not have and must not acquire -- a depth
// prepass, hierarchical-Z occlusion culling (Aver.Occlusion is linked into Sandbox alone), Trifactor
// LOD and GPU cluster dispatch, selection outlines, a PlayerStart icon and a path-traced scene view.
// The hooks are the seam that lets the editor's walk BE this walk without any of that arriving here:
// each one is a point where the editor needs to observe or answer something, and every one of them
// defaults to null, meaning "the answer this walk has always given". A default-constructed
// DrawWorldOptions must reproduce the pre-hook command stream exactly, and that is the property this
// file is verified against -- a frame capture of both hosts, not an argument.
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
    // The depth-prepass walk is the same walk emitting depth-only draws -- see DrawWorldPass. Read
    // into a local because it is tested per entity and once per planned draw, and because spelling
    // the enum comparison out at each of those sites reads as if the answer could differ between
    // them.
    const bool depthPass = options.pass == DrawWorldPass::DepthPrepass;
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

    // Resolves one planned draw's material token into what it needs to draw with -- the shared
    // authored > dead-handle > named-look > flat-fallback ladder (resolveSurfaceLook,
    // aver/game/SceneSubmission.hpp), plus the MaterialSystem binding -- so a multi-part entity
    // resolves each part's OWN token through the exact same ladder the single-material path
    // always used for the entity's. Mirrors SandboxApp::resolveSurface
    // (sandbox/src/SandboxRender.cpp) field for field, down to holding the resolved SurfaceLook
    // whole instead of unpacking it, and is called once per planned draw the way that file's own
    // emitEntityDraws() calls resolveSurface() once per draw rather than once per entity.
    //
    // HOISTED OUT OF THE ENTITY LOOP, where it used to be declared: three delivery paths call it
    // now (raster, direct and depth-only) instead of two, and a closure re-created per entity to be
    // called from further down the same iteration reads as if it captured something from it. It
    // captures nothing per-entity -- `content`, `materials` and `options` are all the function's own
    // arguments.
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
            // THE THROTTLE, THOUGH, IS NOT THE HOST'S. It stays on this side of
            // DrawWorldOptions::onSurfaceWarn precisely because "once per material token" is the
            // part two hosts cannot each keep for themselves without the counts drifting -- the
            // set is function-local-static, so it is one set per process however many hosts and
            // however many walks share it, which is what the promise in the sentence means.
            //
            // NOT KNOWN TO FIRE, exactly as on the editor's side -- this is a latent hazard
            // closed on inspection, not a reproduced bug. If this line ever appears in a log,
            // that is new information worth chasing.
            static std::unordered_set<i32> warnedDeadMaterialHandles;
            if (warnedDeadMaterialHandles.insert(m).second) {
                if (options.onSurfaceWarn) {
                    options.onSurfaceWarn(m, SurfaceWarning::DeadMaterialHandle, options.user);
                } else {
                    AVER_WARN("[Game] surface '{}' holds a material handle that no longer resolves "
                              "in pbr::MaterialLibrary; falling through to its built-in look or the "
                              "flat gray fallback. Trusted at face value it would have drawn as a "
                              "bright white mirror",
                              aver_scene_material_name(m));
                }
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
                if (options.onSurfaceWarn) {
                    options.onSurfaceWarn(m, SurfaceWarning::UnresolvedSurface, options.user);
                } else {
                    AVER_WARN("[Game] surface '{}' has no authored .ocmat and no built-in look; "
                              "rendering the flat gray fallback (0.80, 0.80, 0.85) instead",
                              aver_scene_material_name(m));
                }
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

    const u32 n = w.count();
    // THE VISIT ORDER IS THE CALLER'S, and `oi` (the position in it) is what the hooks are told,
    // because the editor's occlusion pass-1/pass-2 boundary is a raw index into THIS sequence and
    // not into the world's -- see DrawWorldOptions::visitOrder. Null is 0..n-1, which is the order
    // this walk has always used, and the only order a game host has ever wanted.
    const u32 visitCount = options.visitOrder ? options.visitOrderCount : n;
    for (u32 oi = 0; oi < visitCount; ++oi) {
        const u32 i = options.visitOrder ? options.visitOrder[oi] : oi;
        // An index the caller's order put out of range would otherwise reach World::at, which
        // indexes its dense array without a bound test. Unreachable on the default path (i == oi
        // < n), one compare on the reordered one.
        if (i >= n) continue;
        const scene::Entity ent = w.at(i);
        // BEFORE THE FIRST FILTER, DELIBERATELY: this fires for every visited index whatever
        // becomes of it, because its caller needs a point in the COMMAND STREAM, not a list of
        // entities -- see DrawWorldVisitFn. Everything below this line can `continue`.
        if (options.onVisit) options.onVisit(oi, ent, options.user);
        if (w.destroyPending(ent)) continue;
        const scene::CMeshRenderer* mr =
            w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
        // NO RENDERER, OR NOTHING ASSIGNED, IS THE ORDINARY CASE and says nothing: most entities in
        // a level carry no mesh at all.
        if (!mr || mr->mesh == 0) continue;
        // SPLIT OUT OF THE COMPOSITE GUARD PURELY SO IT CAN BE REPORTED. The three conditions were
        // one `||` chain -- no renderer, not visible, no mesh -- and they are the same three, now
        // ordered as the editor already orders them (SandboxRender.cpp:772). The set of entities
        // skipped cannot differ: all three are plain `continue`s with nothing between them to have
        // a side effect, so only WHICH of them a reader is told about changes. A mesh is named and
        // the visible bit is clear is the one worth telling: it is NOT the ordinary case, it is the
        // zero-fill trap, and it used to drop the entity with no diagnostic of any kind. See
        // DrawSkipReason.
        if (!(mr->flags & scene::kMeshRendererVisible)) {
            if (options.onSkipped)
                options.onSkipped(ent, mr->mesh, DrawSkipReason::NotVisible, options.user);
            continue;
        }

        const rhi::MeshHandle handle = content.meshFor(mr->mesh);
        if (!handle) {
            if (options.onSkipped)
                options.onSkipped(ent, mr->mesh, DrawSkipReason::MeshNotLoaded, options.user);
            continue;
        }

        // worldMatrix is non-const on World, which is why this takes a non-const World&.
        const Mat4& wm = w.worldMatrix(ent);

        // THE SEAM, and it is one line because the design made it one. A skinned entity's posed
        // vertices live in a DIFFERENT MeshHandle sharing this one's index buffer, so substituting
        // the handle reaches every pass at once. Zero means "not skinned", never "not drawn".
        //
        // ASKED ONCE, HERE, rather than once for the bounds guard and again for the substitution:
        // they were always the same call on the same feature, and a host reading
        // EntityDecision::posedMesh has to be looking at the same answer the bounds decision used or
        // it is deciding about a different frame.
        const rhi::MeshHandle posedMesh = skinning ? skinning->drawHandle(ent) : 0;
        const bool skinned = posedMesh != 0;

        // A STATIC entity gets its bounds from the asset. A SKINNED one already had them written
        // this frame by SkinnedScene from its ACTUAL POSE, so leave those alone -- overwriting with
        // the rest box is exactly the popping the posed-bounds work exists to stop. The guard was
        // written before skinning was wired; it is live now.
        if (!skinned)
        if (const auto* b = content.boundsFor(mr->mesh)) {
            auto* mw = const_cast<scene::CMeshRenderer*>(mr);
            mw->aabbMin[0] = b->first.x;  mw->aabbMin[1] = b->first.y;  mw->aabbMin[2] = b->first.z;
            mw->aabbMax[0] = b->second.x; mw->aabbMax[1] = b->second.y; mw->aabbMax[2] = b->second.z;
        }

        // ---- OWNER HIDE, DECIDED BEFORE THE CULL ----
        // Mirrors SandboxRender.cpp:824-830's ancestor walk against firstPersonPawn_: an entity both
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
        //
        // THE WORLD BOX OUTLIVES THE CULL BLOCK, which it did not have to before there was anyone to
        // hand it to. That scope WAS the whole bug on the editor's side: with the box local to the
        // test, its second walk re-derived a sphere from the LOCAL aabbMin/aabbMax against a
        // world-space eye while the first built it from these corners, the two fed the same LOD
        // function different inputs, and they picked different levels for the same instance --
        // 2.81% of pixels differing, falling to 0.04% (noise) with --no-lod-select
        // (SandboxRender.cpp:234-242). EntityDecision hands the host THIS box for that reason.
        bool haveWorldBox = false;
        bool frustumCulled = false;
        Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
        {
            const Vec3 lo{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            const Vec3 hi{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                for (u32 c = 0; c < 8; ++c) {
                    const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                    const Vec3 t = xformPoint(wm, p);
                    wlo.x = std::fmin(wlo.x, t.x); whi.x = std::fmax(whi.x, t.x);
                    wlo.y = std::fmin(wlo.y, t.y); whi.y = std::fmax(whi.y, t.y);
                    wlo.z = std::fmin(wlo.z, t.z); whi.z = std::fmax(whi.z, t.z);
                }
                haveWorldBox = true;
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

        // THE ENTITY'S OWN FALLBACK MATERIAL: 0 means "ask the mesh", not "no material" -- the same
        // rule the editor applies. Needed unconditionally now (not only inside the PBR branch below):
        // planEntityDraws' single-draw case reads it directly, and it is what a split part's own empty
        // slot falls back to (planEntityDraws' `p.material ? p.material : entityMaterial`).
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        const i32 mat = mr->material ? mr->material : content.meshDefaultMaterial(mr->mesh);
#else
        const i32 mat = mr->material;
#endif

        // ---- THE ONE PLACE A HOST GETS A SAY ----
        // Everything above is what this walk knows on its own; everything below acts on it. The host
        // is asked exactly once, here, with the bounds and both verdicts already computed and before
        // a route is chosen -- see EntityDecision for why it is one in/out struct and not a handful
        // of narrower calls. Every OUT field arrives holding this walk's own answer, so a null
        // `decide` and a `decide` that returns without touching anything are the same walk.
        EntityDecision dec;
        dec.pass = options.pass;
        dec.entity = ent;
        dec.visitIndex = oi;
        dec.meshId = mr->mesh;
        dec.material = mat;
        dec.world = &wm;
        dec.worldBoxMin = wlo;
        dec.worldBoxMax = whi;
        dec.haveWorldBox = haveWorldBox;
        dec.skinned = skinned;
        dec.frustumCulled = frustumCulled;
        dec.ownerHidden = ownerHiddenHere;
        dec.baseMesh = handle;
        dec.posedMesh = posedMesh;
        dec.chosenMesh = posedMesh ? posedMesh : handle;
        if (options.decide) options.decide(dec, options.user);
        if (dec.skip) continue;

        // THE ONE PLACE that decides who delivers this entity -- raster's drawMesh() or Voxi's direct
        // submit(). chooseRoute() (aver/game/SceneSubmission.hpp) is CALLED now rather than spelled
        // out: it used to be written inline here with both of its editor-only inputs pinned false,
        // on the argument that "there is no decision left to share once the inputs are constants",
        // and dec.occlusionCulled and dec.tint are exactly those two inputs ceasing to be constants.
        // With a null `decide` they are still false and this returns the same three values the
        // inline expression did -- {raster = !frustumCulled && !ownerHiddenHere, hiddenFromOwner =
        // ownerHiddenHere, tint = false}.
        //
        // dec.tint FEEDS showCulled, not route.tint directly: the debug view's whole behaviour is
        // that a culled entity takes the RASTER route anyway and is tinted there, and that pairing
        // is chooseRoute's to make. Asking for the tint on an entity chooseRoute would not have
        // tinted therefore changes nothing, which is the right answer and not a silent one.
        const RouteDecision route =
            chooseRoute(frustumCulled, dec.occlusionCulled, ownerHiddenHere, dec.tint);

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
        //
        // AFTER decide(), because dec.chosenMesh is one of its two inputs: planEntityDraws compares
        // the chosen handle against the UNSUBSTITUTED one to know whether this entity's per-material
        // split still applies, and a host that substituted a soft-body or LOD copy has changed that
        // answer (see planEntityDraws' own comment).
        const std::vector<GameContent::MeshPart>* parts = content.partsFor(mr->mesh);
        PlannedDraw pdraws[kMaxPlannedDraws];
        const u32 pdrawCount = planEntityDraws(
            handle, dec.chosenMesh,
            parts ? parts->data() : nullptr,
            parts ? static_cast<u32>(parts->size()) : 0u,
            mat, pdraws, kMaxPlannedDraws);

        if (depthPass) {
            // ---- DEPTH-ONLY DELIVERY ----
            // A non-raster entity writes no depth at all and is not counted: this pass is one half
            // of a frame the colour pass finishes, and both the counters and the "who delivered
            // what" bookkeeping belong to that half. See DrawWorldPass.
            if (!route.raster) continue;
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                // GLASS MUST NOT WRITE DEPTH AT ALL, EVER, and that is a different reason from
                // every other prepass exclusion (skinned, cluster-dispatched): those are excluded
                // because their depth is written some other way, this one because the blended
                // replay runs with depth-WRITE off so a translucent surface never occludes what is
                // behind it. Pre-writing opaque depth for a glass part would leave that depth
                // unconsumed by the colour pass (whose own per-draw prepass gate excludes `blended`
                // too) AND make every opaque object behind the glass depth-test against a surface
                // meant to be see-through, vanishing under it instead of showing through.
                if (dl.look.blended) continue;
                if (dl.matBytes) device.setDrawBinding(dl.matSet, dl.matConstants, dl.matBytes);
                device.drawMeshDepthPrepass(pd.mesh, &wm.m[0][0]);
            }
            continue;
        }

        // THE RASTER ROUTE, MINUS THE ENTITIES SOMEONE ELSE ALREADY SHADED. dec.colourAlreadyDrawn
        // is read only here, on an entity chooseRoute already routed to raster: its lit pixels came
        // from something the host dispatched itself, so drawMesh() would double-draw it -- but
        // drawMesh() is also the ONLY path to IRenderFeature::submitDraw, so skipping it silently
        // drops the entity from the shadow cascades, GI voxelisation and the TLAS. It takes the
        // direct route below instead, which is precisely what that registration is, and it still
        // counts as drawn.
        const bool rasterDraws = route.raster && !dec.colourAlreadyDrawn;
        if (rasterDraws) {
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                // occlusion.showCulled's magenta: (1, 0.15, 1) knocks the green channel down so a
                // false cull is obvious on screen. A BRANCH, not a multiply that silently becomes
                // an identity at 1.0 when the debug view is off -- one bool test per draw is what
                // this debug view is allowed to cost. route.tint is false on every ordinary frame,
                // so `col` is a copy of dl.look.col, value for value.
                f32 col[4] = {dl.look.col[0], dl.look.col[1], dl.look.col[2], dl.look.col[3]};
                if (route.tint) col[1] *= 0.15f;
                if (dl.matBytes) device.setDrawBinding(dl.matSet, dl.matConstants, dl.matBytes);

                // STICKY on the device (RHI.hpp's setDrawBlended comment), so it is set on EVERY draw
                // here, not only when true. Skipping the false case would leave a translucent part's
                // flag set for whatever opaque part or entity this walk visits next -- that next mesh
                // would silently take the blended path too: no ray-traced shadow, no GI bounce, no
                // shadow-cascade write, and drawn through scenePipeline(..., blended=true) instead of
                // the ordinary opaque pipeline, purely because it happened to be drawn after a pane of
                // glass.
                device.setDrawBlended(dl.look.blended);
                // PER DRAW, AND AUTO-CONSUMED BY THE VERY NEXT drawMesh() rather than sticky
                // (RHI.hpp's own comment on setNextDrawPrepassed), which is exactly why the
                // translucency half of the test cannot be hoisted to the entity: one call before a
                // multi-part loop would cover part 0 alone, silently asking the LessEqual/no-write
                // pipeline for parts no depth-prepass walk ever wrote depth for. The entity half is
                // the host's: dec.prepassEligible stays false unless a host says it prepassed this
                // entity, and a game host that runs no prepass walk never says so -- which is why
                // this walk called neither of these two functions before there were hosts that do.
                if (dec.prepassEligible && !dl.look.blended) device.setNextDrawPrepassed(true);
                device.drawMesh(pd.mesh, &wm.m[0][0], col, dl.look.metallic, dl.look.roughness);
            }
        } else if (dec.emitDirectDraws &&
                   (options.voxiRenderer != nullptr || options.onDirectDraw != nullptr)) {
            // THE UNIFIED DIRECT ROUTE: frustum-culled, occlusion-culled or owner-hidden (or shaded
            // already by the host), handed straight to Voxi so shadows, GI voxelisation and the RT
            // TLAS never depend on what the raster camera can see -- mirrors emitEntityDraws'
            // else-branch (SandboxRender.cpp:2108) calling voxiRenderer_.submit() with the SAME
            // mesh/look/translucency this draw would have used on the raster route, plus
            // hiddenFromOwner so a possessed pawn's own body stays out of ray-driven primary
            // visibility (voxi.hlsl's AVER_RT_MASK_OWNER_HIDDEN lane) while still casting a shadow
            // and bouncing light, exactly like the raster walk always did for it. Per planned draw,
            // not per entity, so a culled multi-material entity's parts reach Voxi with their own
            // materials instead of all borrowing the entity's -- the same split the raster route
            // above gets. Both sinks null (no Voxi feature attached and no host listening)
            // reproduces this walk's pre-existing behaviour exactly: the entity is skipped and casts
            // nothing while culled or hidden.
            //
            // THE TWO SINKS ARE INDEPENDENT, deliberately. The editor's path-traced scene view is
            // reached from here and nowhere else -- PtSceneView::submitDraw is otherwise only
            // called through drawMesh(), the very call this route exists to skip, so before it was
            // fed from here the path tracer traced a scene holding only what the camera could see.
            // Gating that sink on a VoxiRenderer having been attached would make one feature's
            // absence silently disable an unrelated one.
            //
            // hiddenFromOwner FOLLOWS THE REAL ROUTE, not this branch: deliver()
            // (aver/game/SceneSubmission.hpp) keys it on route.raster, so an entity that is here
            // only because the host already shaded it carries false, exactly as the raster draw it
            // replaces would have.
            const bool hiddenFromOwner = route.raster ? false : route.hiddenFromOwner;
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                f32 col[4] = {dl.look.col[0], dl.look.col[1], dl.look.col[2], dl.look.col[3]};
                if (route.tint) col[1] *= 0.15f;
#if AVER_MODULE_VOXI
                if (options.voxiRenderer) {
                    options.voxiRenderer->submit(pd.mesh, &wm.m[0][0], col, dl.look.metallic,
                                                 dl.look.roughness, dl.matSet, dl.matConstants,
                                                 dl.matBytes, /*translucent=*/dl.look.blended,
                                                 hiddenFromOwner);
                }
#endif
                if (options.onDirectDraw) {
                    options.onDirectDraw(pd.mesh, &wm.m[0][0], col, dl.look.metallic,
                                         dl.look.roughness, dl.matSet, dl.matConstants, dl.matBytes,
                                         /*translucent=*/dl.look.blended, hiddenFromOwner,
                                         options.user);
                }
            }
        }

        if (route.raster) {
            // ONCE PER DELIVERED ENTITY, after its draws, with the handle they actually used -- the
            // editor latches its selection outline here (SandboxRender.cpp:1459), and an outline
            // drawn from the base handle while the entity rendered a posed or LOD copy would trace
            // the wrong silhouette.
            if (options.onEntityDelivered)
                options.onEntityDelivered(ent, mr->mesh, dec.chosenMesh, wm, /*raster=*/true,
                                          options.user);
            ++drawn;
        } else {
            // Not fired for an entity dec.emitDirectDraws held back: nothing was delivered for it
            // on either route, and a "delivered" hook that fires for a draw that did not happen is
            // the kind of sink a reader later has to disprove.
            if (options.onEntityDelivered && dec.emitDirectDraws)
                options.onEntityDelivered(ent, mr->mesh, dec.chosenMesh, wm, /*raster=*/false,
                                          options.user);
            // Priority matches SandboxRender.cpp:981's counting convention: a frustum-culled-AND-
            // owner-hidden entity counts as culled, never as owner-hidden, even though its direct-route
            // delivery above always carried hiddenFromOwner=true regardless -- a counting convention
            // only, not a correctness question (chosen above, unconditionally). dec.occlusionCulled
            // joins the first bucket for the same reason the editor's does: an occlusion-culled
            // entity is culled, and a counter that ignored it would report the feature as free.
            if (frustumCulled || dec.occlusionCulled) ++culled; else ++ownerHidden;
        }
    }

    // NOT IN THE DEPTH PASS. Those three counters and the sentence below describe a FRAME, and a
    // host running this walk twice over the same entities must not see its entity count doubled or
    // this line fire twice -- see DrawWorldPass::DepthPrepass.
    if (!depthPass) {
        stats.drawn = drawn;
        stats.culled = culled;
        stats.ownerHidden = ownerHidden;
        if (drawn != stats.lastDrawn || culled != stats.lastCulled ||
            ownerHidden != stats.lastOwnerHidden) {
            // SUPPRESSED, NOT REWORDED, for a host with its own sentence: the editor's names
            // "spawned CMeshRenderer entities" and counts occlusion in its "culled"
            // (SandboxRender.cpp:1552), vocabulary a packaged game has no business borrowing. The
            // counters above are what such a host reads instead. See SceneDrawStats.
            if (!options.suppressLog) {
                AVER_INFO("[Game] scene-render: {} drawn, {} frustum-culled, {} owner-hidden",
                          drawn, culled, ownerHidden);
            }
            stats.lastDrawn = drawn;
            stats.lastCulled = culled;
            stats.lastOwnerHidden = ownerHidden;
        }
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
