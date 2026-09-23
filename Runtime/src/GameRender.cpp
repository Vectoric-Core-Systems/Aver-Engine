// The world draw walk: every live entity carrying a visible CMeshRenderer.
//
// Lifted from the editor's entity loop (formerly SandboxApp.cpp:1386-1510, before the 2026-09-16
// split moved it to sandbox/src/SandboxRender.cpp's onRender, which now calls this function
// instead of carrying a copy). What is NOT here is as deliberate as what is: no selection
// latch, no selection outline, no grid, no gizmo, no skin-scene-test recolour, no objects_
// placeholder pass, and no capture/gate harness. Those are the editor looking at a world; this is a
// game showing one.
//
// WHAT THE HOOKS IN DrawWorldOptions ARE FOR, and why they are not that list creeping back in. Two
// hosts run this walk: the shipped game, through GameApp, and the editor, which calls it TWICE per
// frame from SandboxApp::onRender (sandbox/src/SandboxRender.cpp) -- once for its depth prepass and
// once for colour -- rather than carrying the copy of it that used to sit inline there. The
// editor's is the behaviour reference and carries machinery this library does not have and must
// not acquire -- hierarchical-Z occlusion culling (Aver.Occlusion is linked into Sandbox alone),
// Trifactor LOD and GPU cluster dispatch, selection outlines, a PlayerStart icon and a path-traced
// scene view.
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
// cmake/AvModule.cmake:15), so the editor (sandbox/src/SandboxRender.cpp's resolveSurface, and its
// drawWorld hooks) and this walk read the identical decisions.
#include "aver/game/GameRender.hpp"

#if AVER_MODULE_SCENE

#include "aver/game/GameContent.hpp"
#include "aver/game/SceneSubmission.hpp"
// CpuLap/CpuNest split drawWorld's one "6.8ms in the rest" number into the CpuSpan buckets this
// walk actually spends its time in (WalkEntity, WalkLookup, WalkFrustum, WalkDecide, the three
// WalkEmit* delivery routes, WalkResolveLook, WalkOther) -- see aver/core/CpuTiming.hpp's own
// comment for why this exists and why it is not the GPU side's ScopedGpuStat reused for the CPU.
// Unguarded by any AVER_MODULE_* macro, deliberately: the facility depends on nothing this file's
// own AVER_MODULE_SCENE guard does not already require, and it trims itself out at compile time
// via its own AVER_CPU_TIMING switch rather than needing a second one here.
#include "aver/core/CpuTiming.hpp"
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

namespace {

// MIXES A MESH ID'S BITS before they choose a slot below, rather than trusting the low bits alone
// (`id & (kMeshLookupCacheSlots - 1)`) the way a plain mask would. CMeshRenderer::mesh is
// fnv1a64(project-relative path) -- verified by reading every writer of sceneMeshes_'s keys in
// GameContent.cpp (registerBuiltins' own `add` lambda, and loadProjectMeshes), there is no second
// scheme anywhere that hands out a mesh id some other way -- so this is already a HASH, not a
// small sequential index or a raw pointer value. That does not make masking its low bits safe: to
// avoid this stage's own named failure ("id & (kSlots-1) can degenerate to one hot slot"), if that
// hash happened to have anything resembling a distribution to it, that is normally judged against
// the theory of the hash function chosen, not against a single project's example inputs. In
// practice this project's own three built-in mesh names ("Meshes/sphere.ocmesh",
// "Meshes/cube.ocmesh", "Meshes/drone.ocmesh") are short and differ in only a handful of trailing
// bytes, which is an input shape that stresses a multiplicative hash's low-order bits more than a
// long, varied one -- and a level's own tree/prop kit names, sharing a common directory prefix, are
// exactly the same shape again. Whether or not that particular worry is founded for THIS specific
// hash, mixing costs two multiplies and three shifts and removes the question entirely, so it is
// paid unconditionally rather than argued away. This is MurmurHash3's 64-bit finalizer (fmix64),
// chosen because it is a well-known, already-reviewed avalanche mix meant for exactly this job --
// re-mixing a hash that is otherwise trusted, never hashing raw bytes itself, which stays
// GameContent's own job (fnv1a64) and is not duplicated here.
constexpr u64 mixMeshId(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// FIXED, POWER OF TWO, so the slot for a mesh id is one mask away from its mixed hash rather than a
// modulo. Verified a power of two by the static_assert just below rather than trusted by eye.
//
// 64, NOT 1 -- a single "last mesh" slot was REJECTED HERE, deliberately, not left unconsidered.
// One slot is the cheaper shape to reach for, and it is 100% effective on the synthetic stress
// scene this stage's own numbers came from (16,000 copies of one cube: nothing ever evicts a
// one-entry cache when there is only ever one mesh id to hold) -- and close to useless on the
// realistic target this stage exists to serve instead: a forest of a HANDFUL of tree species,
// INTERLEAVED in scene::World's own storage order. Nothing sorts entities by mesh -- World::at
// below walks its dense array in spawn/load order, and a level author places a pine next to an oak
// because that is where it looks right, not because a renderer would prefer them grouped -- so two
// different species drawn back to back would evict a one-entry cache on EVERY single iteration: a
// 0% hit rate on precisely the content this programme exists to speed up, arrived at by optimising
// for the benchmark instead of the target. 64 slots buys room for several distinct meshes to
// coexist without evicting each other -- enough for "a handful" of species, prop-kit pieces or
// building modules to all stay resident for the length of one walk -- while the whole array still
// sits in a few cache lines and a full reset (drawWorld's own local, default-constructed fresh on
// every call) costs nothing measurable once a frame.
constexpr u32 kMeshLookupCacheSlots = 64;
static_assert((kMeshLookupCacheSlots & (kMeshLookupCacheSlots - 1)) == 0,
              "kMeshLookupCacheSlots must be a power of two for the '& (kMeshLookupCacheSlots - 1)' "
              "mask below to be equivalent to '% kMeshLookupCacheSlots'");

// ONE SLOT'S ANSWER, for the one mesh id it currently holds, to all four of WalkLookup's named
// probes -- meshFor, boundsFor, meshDefaultMaterial and partsFor (CpuSpan's own comment names all
// four; GameContent.cpp is where each is verified pure: a `find()` plus a return, const, no lazy
// upload, no side effect).
//
// `meshId == 0` MEANS EMPTY, VERIFIED RATHER THAN ASSUMED, so no separate validity bool is needed
// to tell "never touched by this walk" apart from "touched by a mesh id that happens to be 0": every
// entity that reaches ANY of the four call sites below has already survived drawWorld's own
// `if (!mr || mr->mesh == 0) continue;`, ahead of all four in the loop, so mr->mesh -- the only
// value ever hashed into this cache -- is never 0 for a live probe. A default-constructed slot
// therefore reads, correctly, as "empty".
//
// THREE OF THE FOUR FIELDS BELOW ARE RESOLVED LAZILY, NOT TOGETHER, because three of the four
// ORIGINAL probes were themselves gated on something other than "a new mesh id showed up here":
// boundsFor only ever ran `if (!skinned)`, meshDefaultMaterial only ever ran when
// `mr->material == 0`, and either one could, for a GIVEN entity, never be reached at all before an
// earlier `continue` (MeshNotLoaded, decide()'s own `skip`) took that entity out of the loop.
// Resolving either the moment a mesh id first reaches ANY of the four sites -- rather than the
// moment each site's OWN gate first lets an entity through -- would make this cache perform a
// GameContent lookup that entity's own path would never have made, which is exactly the "must
// still only run when" discipline this stage's own brief states outright for meshDefaultMaterial,
// and asks by the same argument for boundsFor's `!skinned` guard. `handle` and `parts` are the two
// exceptions: content.meshFor() and content.partsFor() both run for EVERY entity that reaches
// WalkLookup at all, with no ternary and no `if` gating either one (their own call sites' comments
// say so), so there is no gate a lazy `*Resolved` flag would need to respect for them either --
// they get one exactly the same shape as the other two anyway, purely so all four call sites below
// share one pattern rather than two.
struct MeshLookupCacheSlot {
    u64 meshId = 0;

    bool handleResolved = false;
    rhi::MeshHandle handle = 0;              // content.meshFor(meshId)

    bool boundsResolved = false;
    bool haveBounds = false;                 // boundsFor's own verdict: did it find an entry
    // COPIES of what boundsFor found, NOT the pointer it returned (`&it->second`, into
    // GameContent::meshBounds_). 24 bytes copies for free and can never dangle; see
    // DrawWorldOptions::useMeshLookupCache's own comment for why `parts` below does not get the
    // same treatment and has to reason about the map's own pointer-stability guarantee instead.
    Vec3 boundsMin{};
    Vec3 boundsMax{};

    bool defaultMaterialResolved = false;
    i32 defaultMaterial = 0;                 // content.meshDefaultMaterial(meshId)

    bool partsResolved = false;
    // content.partsFor(meshId)'s OWN pointer, kept as-is rather than copied -- copying the vector it
    // points at would be a heap allocation on every miss, which is exactly what this cache exists to
    // avoid paying, once per distinct mesh, every single frame. Safe on the same grounds
    // DrawWorldOptions::useMeshLookupCache's comment gives in full: std::unordered_map only
    // invalidates a reference or pointer to an element by ERASING it, never by inserting elsewhere
    // or rehashing, and nothing reachable from either installed host's per-entity hooks erases from
    // GameContent::meshParts_ during a walk today.
    const std::vector<GameContent::MeshPart>* parts = nullptr;
};

// Finds this walk's cache slot for `meshId`, evicting whatever DIFFERENT mesh id it held before (if
// any) and resetting every one of its four fields to unresolved -- never handing back a slot that
// still half-remembers the PREVIOUS occupant's bounds or parts pointer under the NEW id. Leaves an
// already-matching slot completely untouched, resolved fields and all, which is the entire point:
// that is the cache HIT this stage exists to create.
//
// CALLED ONCE PER ENTITY, not once per probe -- see drawWorld's own `meshSlot` local, computed at
// the first WalkLookup call site and threaded through the other three by pointer, rather than
// re-derived (and re-risking an eviction) at each one.
MeshLookupCacheSlot& findMeshLookupSlot(MeshLookupCacheSlot* cache, u64 meshId) {
    MeshLookupCacheSlot& slot = cache[static_cast<u32>(mixMeshId(meshId) & (kMeshLookupCacheSlots - 1))];
    if (slot.meshId != meshId) slot = MeshLookupCacheSlot{meshId};
    return slot;
}

} // namespace

void drawWorld(rhi::IDevice& device, const Mat4& viewProj, GameContent& content, SceneDrawStats& stats,
               pbr::MaterialSystem* materials, render::SkinnedScene* skinning,
               const DrawWorldOptions& options) {
    scene::World& w = scene::World::instance();
    // ONE CpuLap FOR THE WHOLE WALK, opened here and never re-constructed, threaded through every
    // phase below via repeated `.to()` calls -- see CpuLap's own comment on why this is the shape
    // the facility is built for (one Lap per measured scope, not one per phase or per entity). It
    // opens on WalkEntity rather than SceneWalk itself: SceneWalk is never measured directly, it is
    // the COMPUTED sum of its children (kCpuSpanParent's own comment on why that sum must be exact),
    // and CpuLap's constructor asserts against being handed it. Opening on WalkEntity here, ahead of
    // the frustum-plane derivation below and before the loop's first entity is even reached, folds
    // that one-time setup into WalkEntity's bucket -- accurate, since it is exactly the kind of
    // "loop overhead" WalkEntity's own comment already names, paid once per drawWorld() call rather
    // than once per entity. `lap` stays alive until this function returns, which is what lets it
    // also cover the logging tail below the loop: nothing between the loop's last transition and
    // that destructor calls `.to()`, so the tail's cost lands in whichever phase was open when the
    // last entity's iteration finished.
    CpuLap lap(CpuSpan::WalkEntity);
    // The depth-prepass walk is the same walk emitting depth-only draws -- see DrawWorldPass. Read
    // into a local because it is tested per entity and once per planned draw, and because spelling
    // the enum comparison out at each of those sites reads as if the answer could differ between
    // them.
    const bool depthPass = options.pass == DrawWorldPass::DepthPrepass;
    int drawn = 0, culled = 0, ownerHidden = 0;

    // WalkLookup's mesh cache for THIS CALL ALONE. See MeshLookupCacheSlot's own comment for the
    // shape and DrawWorldOptions::useMeshLookupCache's for why it can be trusted at all. A LOCAL,
    // never a class member and never heap-allocated -- Aver.Runtime.Game.Core links against no
    // arena for this and none is warranted: the array is a few dozen bytes times
    // kMeshLookupCacheSlots on THIS call's own stack frame, gone the instant drawWorld returns, so
    // the editor's two calls per frame (depth prepass, then colour, SandboxRender.cpp's onRender)
    // each get their own and neither can see the other's. Default-constructed UNCONDITIONALLY, cache
    // on or off, so every call site below can read `options.useMeshLookupCache` with a plain `if`
    // rather than an `#if` -- the one cost the toggle does NOT remove is this array's own
    // construction, kMeshLookupCacheSlots trivial (all-zero) field inits, cheap enough on a local
    // stack array that it was judged not worth an `#if`, or a heap/lazy allocation, just to avoid
    // paying it on the OFF path too.
    MeshLookupCacheSlot meshLookupCache[kMeshLookupCacheSlots]{};
    // Hits and misses across all four WalkLookup call sites this call makes, published onto `stats`
    // below (colour pass only, matching drawn/culled/ownerHidden's own "describes a frame" rule) so
    // a host can print or graph the rate rather than have to infer it. See SceneDrawStats' own
    // comment on why these two are never accumulated ACROSS calls.
    int meshLookupHits = 0, meshLookupMisses = 0;

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
    // whole instead of unpacking it, and called ONCE PER PLANNED DRAW rather than once per entity,
    // which is what the per-material split needs and what the editor's deleted emitEntityDraws did
    // with its own resolveSurface. SandboxApp::resolveSurface survives for the two readers this
    // walk cannot serve -- its warning sentences (SandboxRender.cpp's colourWarn) and the
    // entity-level resolve the GPU cluster path binds through (colourDecide) -- and its own
    // comment at that call site says so.
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
        // WalkResolveLook, for this call's ENTIRE body: a CpuNest because this lambda is entered
        // from all three delivery branches below (the depth-prepass, raster and direct routes, at
        // their own call sites) and must attribute its own cost to the SAME bucket regardless of
        // which one called it, without disturbing whichever WalkEmit* phase is the caller's own.
        // Constructed first, before anything else in the body runs, and destructed by falling out
        // of this lambda's scope when `return dl;` executes below -- one nest brackets the whole
        // call, including the two GameContent lookups it makes itself (authoredFor, lookFor), which
        // are NOT among WalkLookup's four named probes and so are priced here instead, as part of
        // what resolving a look actually costs.
        CpuNest resolveLookNest(CpuSpan::WalkResolveLook);
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
            // that knows, which is where SandboxApp::resolveSurface keeps its own.
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
        // Re-opens WalkEntity for this iteration, closing whichever phase the PREVIOUS iteration
        // left open (one of the WalkEmit* routes, WalkOther, or -- on the very first iteration --
        // the WalkEntity occurrence `lap` opened in its constructor above, which folded in the
        // pre-loop setup). One call per visited index, so WalkEntity's `calls` count reads as "how
        // many entities this walk looked at", not "how many were drawn" -- a skipped entity still
        // opens this phase and, via the next `.to()` or (on the last entity) `lap`'s own destructor,
        // still gets counted.
        lap.to(CpuSpan::WalkEntity);
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
        // one `||` chain -- no renderer, not visible, no mesh -- and they are the same three, in
        // the order the editor's own loop used before that loop became this one. The set of entities
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

        // THIS ENTITY'S CACHE SLOT, resolved ONCE (inside the very first WalkLookup probe below, so
        // the array index/mix/compare that finds or evicts it is itself charged to that bucket) and
        // threaded by pointer through all three of the sites that follow, rather than re-derived at
        // each. Nothing between here and the last of them (content.partsFor, which runs AFTER
        // options.decide has already been given this entity) can move `meshSlot` out from under it:
        // the cache array is local to THIS call's stack frame, invisible to every hook
        // DrawWorldOptions exposes, so the only thing that could ever evict this exact slot is
        // ANOTHER entity's OWN call to findMeshLookupSlot, which cannot happen until the NEXT
        // iteration of this loop begins. Null with the cache switched off, which is what sends every
        // site below down its original, direct-to-GameContent path unconditionally.
        MeshLookupCacheSlot* meshSlot = nullptr;

        // content.meshFor IS THE FIRST OF WalkLookup's FOUR NAMED PROBES (see CpuSpan's own
        // comment). Wrapped around the cache check (or, cache off, the call alone) in an
        // immediately-invoked lambda, so `handle` stays exactly what it was -- a plain value, not a
        // reference into anything the nest's lifetime affects -- and so the probe is timed without
        // moving where in this loop it runs. UNCONDITIONAL FOR EVERY ENTITY in the original code, so
        // there is no per-entity gate for this cache to respect: resolved the first time ANY entity's
        // mesh id lands on this slot, reused by every entity after it that shares that id.
        const rhi::MeshHandle handle = [&] {
            CpuNest lookupNest(CpuSpan::WalkLookup);
            if (options.useMeshLookupCache) {
                meshSlot = &findMeshLookupSlot(meshLookupCache, mr->mesh);
                if (meshSlot->handleResolved) {
                    ++meshLookupHits;
                } else {
                    meshSlot->handle = content.meshFor(mr->mesh);
                    meshSlot->handleResolved = true;
                    ++meshLookupMisses;
                }
                return meshSlot->handle;
            }
            return content.meshFor(mr->mesh);
        }();
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
        //
        // content.boundsFor IS THE SECOND OF WalkLookup's FOUR NAMED PROBES, nested around the CACHE
        // CHECK (or, cache off, the CALL) ONLY, exactly as before, because it sits inside the SAME
        // condition that gates whether it runs at all: pulling it out ahead of this `if` to wrap it
        // more simply would ask for -- or cache -- an answer for a skinned entity, which never
        // happened before and must not start now just because a timer, or a cache, was added. THE
        // GATE IS PRESERVED PER MESH ID TOO, not only per entity: `boundsResolved` stays false on a
        // fresh slot until the FIRST `!skinned` entity sharing that mesh id reaches this line, so a
        // mesh drawn only by skinned entities is never probed for bounds at all, cache on or off,
        // exactly as it never was before this cache existed.
        if (!skinned) {
            bool haveBounds = false;
            Vec3 boundsMin{}, boundsMax{};
            {
                CpuNest lookupNest(CpuSpan::WalkLookup);
                if (options.useMeshLookupCache) {
                    // meshSlot IS NEVER NULL HERE: it was set by the meshFor call above, in this
                    // same iteration, for this same mr->mesh, and nothing between there and here can
                    // have evicted it (see meshSlot's own declaration comment).
                    if (!meshSlot->boundsResolved) {
                        const auto* found = content.boundsFor(mr->mesh);
                        meshSlot->haveBounds = found != nullptr;
                        // A COPY OF WHAT boundsFor FOUND, not the pointer it returned -- see
                        // MeshLookupCacheSlot's own comment on boundsMin/boundsMax for why this
                        // field gets that treatment and `parts` below does not.
                        if (found) { meshSlot->boundsMin = found->first; meshSlot->boundsMax = found->second; }
                        meshSlot->boundsResolved = true;
                        ++meshLookupMisses;
                    } else {
                        ++meshLookupHits;
                    }
                    haveBounds = meshSlot->haveBounds;
                    boundsMin = meshSlot->boundsMin;
                    boundsMax = meshSlot->boundsMax;
                } else if (const auto* b = content.boundsFor(mr->mesh)) {
                    haveBounds = true;
                    boundsMin = b->first;
                    boundsMax = b->second;
                } else {
                    haveBounds = false;
                }
            }
            // THE WRITE-BACK IS PER ENTITY, ALWAYS, cache or no cache, hit or miss -- this is the
            // one thing memoising the LOOKUP must never also memoise or skip: every `!skinned`
            // entity that names this mesh still gets its own CMeshRenderer::aabbMin/aabbMax written
            // here, exactly as often as it did before this cache existed, from a value that is a
            // faithful copy of what boundsFor answered, not a reference to anything that could have
            // changed underneath it.
            if (haveBounds) {
                auto* mw = const_cast<scene::CMeshRenderer*>(mr);
                mw->aabbMin[0] = boundsMin.x; mw->aabbMin[1] = boundsMin.y; mw->aabbMin[2] = boundsMin.z;
                mw->aabbMax[0] = boundsMax.x; mw->aabbMax[1] = boundsMax.y; mw->aabbMax[2] = boundsMax.z;
            }
        }

        // ---- OWNER HIDE, DECIDED BEFORE THE CULL ----
        // THE ONLY ancestor walk now -- the editor's own copy against firstPersonPawn_ went with
        // its loop, and it feeds this one through DrawWorldOptions::ownerHideRoot instead
        // (SandboxRender.cpp sets it from firstPersonPawn_ for both passes). An entity both
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
        // 2.81% of pixels differing, falling to 0.04% (noise) with --no-lod-select (the account
        // sits on SandboxRender.cpp's depth-prepass phase, which is the call that replaced that
        // second walk). EntityDecision hands the host THIS box for that reason.
        //
        // WalkFrustum OPENS HERE, closing out WalkEntity's own occurrence for this iteration: the
        // "loop overhead, w.at, ... the owner-hide walk" work WalkEntity's own comment names is all
        // above this line, and everything from here through the frustum test below is instead spent
        // building the world-space box and testing it against the six planes derived once at the
        // top of this function.
        lap.to(CpuSpan::WalkFrustum);
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
        //
        // WalkDecide OPENS HERE, ahead of this fallback lookup: everything from this point through
        // planEntityDraws() below is the route/material DECISION for this entity, as distinct from
        // the frustum TEST that closed just above it.
        lap.to(CpuSpan::WalkDecide);
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        // content.meshDefaultMaterial IS ONE OF WalkLookup's FOUR NAMED PROBES (see CpuSpan's own
        // comment). A CpuNest, not a `.to()`, so it does not disturb WalkDecide -- the phase this
        // ternary belongs to on both sides of the probe -- and wrapped around the CACHE CHECK (or,
        // cache off, the CALL) alone, in an immediately-invoked lambda, so the ternary's
        // short-circuit is preserved exactly: this probe, and now this cache's own lookup for it,
        // still runs only when `mr->material` is zero, precisely as it did before either existed.
        // THE MESH-ID GATE MIRRORS boundsFor's, above: `defaultMaterialResolved` stays false on a
        // slot until the FIRST zero-material entity sharing that mesh id asks, so a mesh every
        // entity names its own material for is never probed here at all, cache on or off.
        const i32 mat = mr->material ? mr->material
                                      : [&] {
                                            CpuNest lookupNest(CpuSpan::WalkLookup);
                                            if (options.useMeshLookupCache) {
                                                // meshSlot IS NEVER NULL HERE -- see its own
                                                // declaration comment above.
                                                if (!meshSlot->defaultMaterialResolved) {
                                                    meshSlot->defaultMaterial =
                                                        content.meshDefaultMaterial(mr->mesh);
                                                    meshSlot->defaultMaterialResolved = true;
                                                    ++meshLookupMisses;
                                                } else {
                                                    ++meshLookupHits;
                                                }
                                                return meshSlot->defaultMaterial;
                                            }
                                            return content.meshDefaultMaterial(mr->mesh);
                                        }();
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
        // shared planEntityDraws() -- THE ONLY CALL SITE IN EITHER HOST now that the editor's three
        // went with its loop, which is what makes the split one rule rather than a convention two
        // walks keep. An entity with no parts (the common case) plans to exactly the one draw this
        // loop always issued.
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
        // content.partsFor IS THE FOURTH AND LAST OF WalkLookup's NAMED PROBES. This one runs for
        // every entity that reaches here (no ternary, no `if` gating it), so wrapping just the
        // cache check (or, cache off, the call) changes nothing about WHEN it runs, only which
        // bucket its ticks land in -- and, with the cache on, resolved once per mesh id rather than
        // once per entity, same as meshFor above. CACHES THE POINTER partsFor RETURNS, NOT A COPY OF
        // THE VECTOR IT POINTS AT: see MeshLookupCacheSlot's own comment on the `parts` field, and
        // DrawWorldOptions::useMeshLookupCache's, for why that pointer is safe to hold across many
        // entities and why copying the vector instead was rejected (a heap allocation on every
        // distinct mesh, every frame -- exactly what this cache exists to avoid paying).
        const std::vector<GameContent::MeshPart>* parts = [&] {
            CpuNest lookupNest(CpuSpan::WalkLookup);
            if (options.useMeshLookupCache) {
                // meshSlot IS NEVER NULL HERE -- see its own declaration comment above. Still true
                // even though options.decide has already run for this entity by this point (unlike
                // the other three WalkLookup call sites, all of which run before it): decide() has
                // no reach into this walk's own local cache array, which is not exposed through
                // EntityDecision or DrawWorldOptions at all.
                if (!meshSlot->partsResolved) {
                    meshSlot->parts = content.partsFor(mr->mesh);
                    meshSlot->partsResolved = true;
                    ++meshLookupMisses;
                } else {
                    ++meshLookupHits;
                }
                return meshSlot->parts;
            }
            return content.partsFor(mr->mesh);
        }();
        // ---- A SKINNED ENTITY'S SPLIT, RE-CUT OVER ITS POSE ----
        //
        // planEntityDraws applies the per-material split only when the chosen handle IS the one the
        // split was cut from, so a skinned entity -- drawn through its posed copy, a different handle --
        // used to take the single-draw branch: ONE draw of the whole posed mesh under the entity's own
        // material. A character whose hair cards are their own material slot therefore never drew them
        // with the hair material, and no alpha cut-out ever applied to them; KenneyChar in PTTest (two
        // materials) had been drawing flat for the same reason.
        //
        // Only when the walk's OWN skinning answer is still the chosen handle -- a host substituting a
        // soft-body or LOD copy keeps the single draw (neither has posed parts). The posed parts ARE
        // cut from posedMesh, so planning from it makes planEntityDraws' "split applies" test true for
        // exactly them. Any refusal (no split, no skin streams, a reloaded mesh, a backend without
        // createPosedPartMesh) returns null and leaves the single whole-mesh draw exactly as before.
        // Creation cost lands once per skin target, on first sight, in WalkLookup.
        const std::vector<GameContent::MeshPart>* planParts = parts;
        rhi::MeshHandle planFrom = handle;
        if (parts != nullptr && posedMesh != 0 && dec.chosenMesh == posedMesh) {
            CpuNest lookupNest(CpuSpan::WalkLookup);
            if (const auto* posed = content.posedPartsFor(device, mr->mesh, handle, posedMesh)) {
                planParts = posed;
                planFrom = posedMesh;
            }
        }
        PlannedDraw pdraws[kMaxPlannedDraws];
        const u32 pdrawCount = planEntityDraws(
            planFrom, dec.chosenMesh,
            planParts ? planParts->data() : nullptr,
            planParts ? static_cast<u32>(planParts->size()) : 0u,
            mat, pdraws, kMaxPlannedDraws);

        if (depthPass) {
            // WalkEmitDepth: the depth-prepass delivery branch, mutually exclusive with the raster
            // and direct routes below because `depthPass` is fixed for the whole call to drawWorld
            // (set once from options.pass at the top of this function, never touched per entity) --
            // so this `.to()` fires on every iteration of a depth-prepass call and never on a
            // colour-pass one.
            lap.to(CpuSpan::WalkEmitDepth);
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
                // dl.look.col: the SAME base colour the colour pass hands drawMesh (minus a debug tint
                // that only touches .g). PSDepthPrepass's alpha test multiplies by its .a; without it
                // every alpha-masked material clipped every pixel and wrote no depth.
                device.drawMeshDepthPrepass(pd.mesh, &wm.m[0][0], dl.look.col);
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
            // WalkEmitRaster: the raster delivery branch, mutually exclusive with WalkEmitDepth
            // (that branch already `continue`d above when depthPass is true, so this line is only
            // ever reached on a colour-pass call) and with WalkEmitDirect below (an entity takes
            // exactly one of the two on any given colour-pass iteration).
            lap.to(CpuSpan::WalkEmitRaster);
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
                bool prepassedDraw = dec.prepassEligible && !dl.look.blended;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
                // ---- AN ALPHA-MASKED DRAW WRITES ITS OWN DEPTH FIRST WHEN NO PREPASS DID ----
                //
                // The scene colour shader forces early depth ([earlydepthstencil] on PSMainVoxi,
                // 39af0e32 -- load-bearing: without it hidden fragments poison the AO history and
                // shade for nothing). Under forced early depth the depth WRITE happens before the
                // shader runs, so averEvalMaterial's `clip(s.alpha - a.alphaCutoff)` on an
                // alpha-masked material (material_prelude.hlsl) discards the colour and cannot take
                // the depth back: every cut-out texel of a leaf, a grille or a fence wrote opaque
                // depth, and anything drawn behind it afterwards failed the test and never showed
                // through the hole. 39af0e32's own comment claimed PSMainVoxi had no alpha-cutout
                // discard; it only searched voxi.hlsl, and the clip lives in the material prelude.
                // Found by adversarial review.
                //
                // THE FIX REUSES THE PREPASS MACHINERY FOR ONE DRAW: its depth goes through
                // PSDepthPrepass, which does NOT force early depth and clips BEFORE it writes, then
                // its colour goes through the LessEqual/NO-WRITE twin, which leaves early depth
                // nothing to write. Only alpha-masked opaque draws the frame-wide prepass did not
                // already cover pay the second draw. With --depth-prepass on they are already
                // covered and this does not fire.
                //
                // SKINNED AND SOFT-BODY MESHES ARE COVERED TOO: their posed handle is depth-drawn here
                // from the same compute-written buffer its colour draw reads, and the device honours
                // the prepassed flag for that exact handle (see IDevice::drawMeshDepthOnly). Only an
                // alpha-masked material can reach this branch, so opaque characters are untouched.
                //
                // KNOWN RESIDUALS, stated rather than hidden: cluster-dispatched geometry never
                // reaches this loop; and a material GRAPH driving opacity other than through the
                // stock alpha can disagree with PSDepthPrepass at cut-out edges, exactly as it
                // already can under the frame-wide prepass.
                if (!prepassedDraw && !dl.look.blended && dl.matConstants &&
                    (static_cast<const pbr::MaterialConstants*>(dl.matConstants)->flags &
                     pbr::MaterialFlag_AlphaMask) != 0u) {
                    // `col` is the exact array drawMesh gets below: the depth shader's alpha test
                    // reads its .a. Marked prepassed ONLY if depth was actually written -- false in
                    // wireframe, under a scene-suppressing feature, or with no depth-only pipeline,
                    // and each of those must keep its ordinary pipeline.
                    prepassedDraw = device.drawMeshDepthOnly(pd.mesh, &wm.m[0][0], col);
                }
#endif
                if (prepassedDraw) device.setNextDrawPrepassed(true);
                device.drawMesh(pd.mesh, &wm.m[0][0], col, dl.look.metallic, dl.look.roughness);
            }
        } else if (dec.emitDirectDraws &&
                   (options.voxiRenderer != nullptr || options.onDirectDraw != nullptr)) {
            // WalkEmitDirect: the culled/hidden direct route to Voxi, mutually exclusive with
            // WalkEmitRaster above. THIS IS THE BUCKET THE WHOLE STAGE TURNS ON -- a frustum-culled
            // entity still reaches here and still calls resolveDrawLook and VoxiRenderer::submit
            // (below), which is WHY the cost curve this stage exists to explain is linear in entity
            // count rather than in draw count: shadows, GI voxelisation and the RT TLAS must never
            // depend on what the raster camera happens to see, so a culled entity pays nearly the
            // same price as a drawn one here, on purpose, and this bucket is where that price shows.
            lap.to(CpuSpan::WalkEmitDirect);
            // THE UNIFIED DIRECT ROUTE: frustum-culled, occlusion-culled or owner-hidden (or shaded
            // already by the host), handed straight to Voxi so shadows, GI voxelisation and the RT
            // TLAS never depend on what the raster camera can see. This IS that route for both
            // hosts now -- the editor's own emitEntityDraws else-branch, which called
            // voxiRenderer_.submit() from its deleted loop, is where the rule came from -- and it
            // submits with the SAME mesh/look/translucency this draw would have used on the raster
            // route, plus hiddenFromOwner so a possessed pawn's own body stays out of ray-driven
            // primary visibility (voxi.hlsl's AVER_RT_MASK_OWNER_HIDDEN lane) while still casting a shadow
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
            // WHAT EACH DRAW CARRIES IS deliver()'s ANSWER, NOT ONE WRITTEN OUT AGAIN HERE
            // (aver/game/SceneSubmission.hpp). This pair -- the translucency flag and
            // hiddenFromOwner -- used to be spelled inline, which left deliver() stating the rule
            // for a route neither host took it from: the header's whole premise is that a rule
            // whose purpose is to be stated ONCE cannot be stated twice, and an uncalled function
            // plus a hand-written copy is exactly twice. Now that the editor runs this walk too,
            // calling it here makes it the only statement in either host.
            //
            // hiddenFromOwner FOLLOWS THE REAL ROUTE, not this branch: deliver() keys it on
            // route.raster, so an entity that is here only because the host already shaded it
            // carries false, exactly as the raster draw it replaces would have.
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                const VoxiDelivery del = deliver(pd, dl.look, route);
                f32 col[4] = {dl.look.col[0], dl.look.col[1], dl.look.col[2], dl.look.col[3]};
                if (route.tint) col[1] *= 0.15f;
#if AVER_MODULE_VOXI
                if (options.voxiRenderer) {
                    options.voxiRenderer->submit(del.mesh, &wm.m[0][0], col, dl.look.metallic,
                                                 dl.look.roughness, dl.matSet, dl.matConstants,
                                                 dl.matBytes, del.translucent, del.hiddenFromOwner);
                }
#endif
                if (options.onDirectDraw) {
                    options.onDirectDraw(del.mesh, &wm.m[0][0], col, dl.look.metallic,
                                         dl.look.roughness, dl.matSet, dl.matConstants, dl.matBytes,
                                         del.translucent, del.hiddenFromOwner, options.user);
                }
            }
        }

        // WalkOther, for the bookkeeping below: neither a lookup, a decision nor a delivery, just
        // the counters and the onEntityDelivered hook that record what the branches above already
        // decided. Charged here EXPLICITLY and UNCONDITIONALLY -- rather than left to inherit
        // whichever of WalkEmitRaster/WalkEmitDirect happened to run last -- for the case where
        // NEITHER branch runs at all: an entity whose route is not raster (route.raster false, or
        // rasterDraws false because dec.colourAlreadyDrawn is true) and whose direct route has no
        // sink attached (no voxiRenderer, no onDirectDraw) even though dec.emitDirectDraws defaults
        // true. That is not a rare corner case -- it is what happens on every colour-pass call that
        // runs with no Voxi feature attached at all. Without an explicit `.to()` here, such an
        // entity's bookkeeping would silently stay charged to WalkDecide (the last phase actually
        // opened for it), which is exactly the kind of unnamed gap WalkOther exists to rule out --
        // see CpuSpan's own comment on why this bucket must never be a silent remainder.
        lap.to(CpuSpan::WalkOther);
        if (route.raster) {
            // ONCE PER DELIVERED ENTITY, after its draws, with the handle they actually used -- the
            // editor latches its selection outline here (SandboxRender.cpp's colourDelivered), and
            // an outline drawn from the base handle while the entity rendered a posed or LOD copy
            // would trace the wrong silhouette.
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
            // THE COUNTING CONVENTION IS THIS WALK'S, and both hosts' sentences are worded from it:
            // a frustum-culled-AND-owner-hidden entity counts as culled, never as owner-hidden,
            // even though its direct-route delivery above always carried hiddenFromOwner=true
            // regardless -- a counting convention only, not a correctness question (chosen above,
            // unconditionally). dec.occlusionCulled joins the first bucket because an
            // occlusion-culled entity is culled, and a counter that ignored it would report the
            // feature as free; that is why the editor relabelled its line from "frustum-culled" to
            // plain "culled" when it started reading these three numbers.
            if (frustumCulled || dec.occlusionCulled) ++culled; else ++ownerHidden;
        }
    }

    // NOT IN THE DEPTH PASS. Those three counters and the sentence below describe a FRAME, and a
    // host running this walk twice over the same entities must not see its entity count doubled or
    // this line fire twice -- see DrawWorldPass::DepthPrepass. THE MESH-CACHE COUNTERS FOLLOW THE
    // IDENTICAL RULE, for the identical reason: a depth-prepass call resolves the same mesh ids all
    // over again into its OWN local cache and racks up its own hits and misses doing it, and those
    // numbers are deliberately DROPPED here rather than added to the colour pass's own -- adding
    // them would report a rate no single call ever produced, exactly the double-count this whole
    // guard exists to prevent for drawn/culled/ownerHidden.
    if (!depthPass) {
        stats.drawn = drawn;
        stats.culled = culled;
        stats.ownerHidden = ownerHidden;
        stats.meshLookupCacheHits = meshLookupHits;
        stats.meshLookupCacheMisses = meshLookupMisses;
        if (drawn != stats.lastDrawn || culled != stats.lastCulled ||
            ownerHidden != stats.lastOwnerHidden) {
            // SUPPRESSED, NOT REWORDED, for a host with its own sentence: the editor's names
            // "spawned CMeshRenderer entities" and counts occlusion in its "culled" (it writes it
            // from colourStats once the walk returns), vocabulary a packaged game has no business
            // borrowing. The counters above are what such a host reads instead. See SceneDrawStats.
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
