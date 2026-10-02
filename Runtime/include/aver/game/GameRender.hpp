// The world draw walk.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
// rhi::MeshHandle and rhi::BindingSetHandle appear BY VALUE in the hook signatures below, and an
// alias cannot be forward-declared the way the rhi::IDevice reference in drawWorld's own signature
// is (RHIResources.hpp:13 spells both as plain u32). This is a leaf header over
// aver/core/Types.hpp, and this library already exposes the much larger aver/rhi/RHI.hpp through
// GameContent.hpp, so it costs nothing new.
//
// UNGUARDED, AND THAT IS A FACT ABOUT THE LINK GRAPH, not an oversight: Aver.RHI is an
// unconditional DEP of Aver.Runtime.Game.Core and reaches every host again PUBLICly through
// Aver.Runtime (Runtime/CMakeLists.txt), so there is no module configuration in which this header
// compiles and that one is absent. The scene include below is the opposite case.
#include "aver/rhi/RHIResources.hpp"
// GUARDED, because Aver.Scene is an OPTIONAL link -- Runtime/CMakeLists.txt reaches it through
// `if(TARGET Aver.Scene)`, and its include directory arrives with the target or not at all. With
// AVER_MODULE_SCENE=0 this line was a C1083 that killed every translation unit reaching this
// header, GameApp.cpp among them, which is why two whole matrix configurations died here before
// anything of their own was ever compiled.
//
// Nothing ABOVE the `#if AVER_MODULE_SCENE` region below names a scene type -- SceneDrawStats is
// plain ints (three pairs plus, since Stage 2 of the CPU-profiler work, one more unpaired one for
// the WalkLookup mesh cache below), and both hosts read it in every configuration -- so the guard
// costs that region nothing, and with the module present the include still lands here, ahead of
// everything else and in the same order it always did.
#if AVER_MODULE_SCENE
#  include "aver/scene/Entity.hpp"
#endif

namespace aver::rhi { class IDevice; }
namespace aver::pbr { class MaterialSystem; }
namespace aver::render { class SkinnedScene; }
namespace aver::voxi { class VoxiRenderer; }

namespace aver::game {

class GameContent;
class PlayMobility;

// Draw counters.
//
// The `last*` trio is kept across frames so the log line fires on CHANGE rather than every frame.
// It starts at -1 so the first frame always reports, including a first frame that drew nothing --
// "0 drawn" is the single most useful line when a world fails to appear, and a counter starting at
// 0 would swallow it.
//
// The unprefixed trio is THIS FRAME's own answer, and it exists because the two hosts that run this
// walk print DIFFERENTLY WORDED lines from the same three numbers: the shipped game's "[Game]
// scene-render: N drawn, N frustum-culled, N owner-hidden" (GameRender.cpp) against the editor's
// "[Sandbox] scene-render: N spawned CMeshRenderer entities drawn, N culled, N owner-hidden"
// (SandboxRender.cpp, where it is written from colourStats after the walk returns, and which also
// says "entity" in the singular). Neither sentence is a
// candidate for unification -- the editor's names a concept ("spawned CMeshRenderer entities") that
// a packaged game has no vocabulary for, and its "culled" deliberately covers occlusion as well as
// the frustum -- so a host that wants to keep its own wording sets DrawWorldOptions::suppressLog and
// reads these three instead of parsing a line it did not want written.
//
// A SUPPRESSING HOST KEEPS ITS OWN PREVIOUS-FRAME COPY. `last*` is still maintained under
// suppressLog, but it is updated before the caller gets control back, so it cannot answer "did this
// change since last frame" for anyone but drawWorld itself -- which is exactly why SandboxApp
// already holds lastSceneDrawn_/lastSceneCulled_/lastSceneOwnerHidden_ of its own.
//
// The depth-prepass pass writes NONE of these. See DrawWorldPass::DepthPrepass.
struct SceneDrawStats {
    int drawn = 0;
    int culled = 0;
    int ownerHidden = 0;

    int lastDrawn = -1;
    int lastCulled = -1;
    int lastOwnerHidden = -1;

    // WalkLookup's mesh-cache hit rate for the most recent COLOUR-pass call (see
    // DrawWorldOptions::useMeshLookupCache and GameRender.cpp's own comment on the cache itself).
    // NOT ACCUMULATED ACROSS FRAMES OR PASSES -- each is overwritten every colour-pass call, the
    // same "describes a FRAME, not a running total" convention drawn/culled/ownerHidden already
    // follow just above, and for the identical reason: the editor runs this walk twice a frame, and
    // a counter that kept adding the depth-prepass call's own numbers on top would report a rate
    // that no single call ever produced. Both read 0 with the cache switched off (there is no cache
    // to have hit or missed), which is a fact worth printing rather than a state worth hiding --
    // see this stage's own brief on why a cache that cannot report its own hit rate cannot be
    // judged, and why "off" must say so plainly rather than leave the field looking like a real,
    // if unlucky, 0%.
    int meshLookupCacheHits = 0;
    int meshLookupCacheMisses = 0;
};

#if AVER_MODULE_SCENE

// Which of the two walks over the same entities this call is performing.
//
// BOTH OF THE EDITOR'S WALKS ARE THIS FUNCTION NOW. It used to run a SECOND, EARLIER walk for its
// depth prepass, hand-duplicated from its colour walk -- its own entity iteration, its own
// planEntityDraws, its own LOD choice -- and that duplication had already cost a real bug: the
// world-space box was scoped differently in the two copies, so LOD selection fed the same function
// different inputs and could pick a DIFFERENT LEVEL per walk. The prepass wrote depth for one mesh
// while colour drew another, and every fragment behind the wrong depth was silently dropped:
// 2.81% of pixels differing, falling to 0.04% (noise) with --no-lod-select. That walk is deleted.
// SandboxRender.cpp's depth-prepass phase calls drawWorld a second time with this pass instead --
// its own comment there carries the full account -- and one function called twice cannot drift
// from itself, which is what makes the divergence structurally impossible rather than merely
// fixed.
//
// Colour is what drawWorld has always done and is the default.
//
// DepthPrepass emits device.drawMeshDepthPrepass() instead of device.drawMesh(), skips any planned
// draw whose resolved material is translucent (glass must never write opaque depth -- the
// exclusion is GameRender.cpp's own depth-only delivery, which drops a planned draw whose resolved
// look is blended, and it lives there rather than in either host because it is a fact about the
// MATERIAL), never takes the direct route, never fires onEntityDelivered/onDirectDraw, and
// TOUCHES NO COUNTER IN SceneDrawStats. The counters and
// the log line belong to the frame, not to the pass: a host calling this function twice per frame
// must not see its entity count doubled or its log line fire twice.
//
// SKIPPING IS THE HOST'S JOB IN THIS PASS, which is why EntityDecision carries `pass`: the editor
// excludes skinned entities (posed vertices are compute-written), the GPU cluster mesh-shader path
// (no depth-only twin) and the CPU per-cluster path from its prepass, and every one of those
// exclusions is about machinery this library does not have. It answers them from decide().
enum class DrawWorldPass : u8 { Colour, DepthPrepass };

// Which once-per-material warning the walk is throttling. THE TEXT IS THE HOST'S, the throttle is
// the library's: the editor's wording names editor-only directories ("no .ocmat under
// Binaries/Materials or Content/Materials", SandboxRender.cpp's resolveSurface) and speaks about a
// library the material editor writes through, neither of which a packaged game has any business
// claiming -- the same split GameTick.hpp states outright (":19") and GameCamera.hpp uses for its
// aspect (":11"). What must NOT be duplicated is the "once per material token, ever" bookkeeping,
// because two hosts keeping two sets is how the counts drift.
enum class SurfaceWarning : u8 {
    // An authored handle content.authoredFor still returns that pbr::MaterialLibrary no longer
    // resolves. Taken at face value it would have drawn as a bright white mirror.
    DeadMaterialHandle,
    // Neither an authored .ocmat nor a built-in SurfaceLook claimed this surface: it is about to
    // draw the flat 0.80/0.80/0.85 gray fallback. Only ever reported for a NON-ZERO token -- token
    // 0 is "this entity named no material", which every untextured placeholder legitimately is.
    UnresolvedSurface,
};

// Why the walk dropped an entity before it could reach either delivery route.
//
// These exist because each of these two checks cost a day of bisection in the editor before it
// said anything at all -- SandboxRender.cpp's colourSkipped still writes those two sentences, now
// from this reason code rather than from a copy of the tests. Losing those diagnostics to a
// library that drops entities in silence would be worse than the duplication they replace, so the
// walk reports them and the host writes the sentence.
enum class DrawSkipReason : u8 {
    // A mesh is named and kMeshRendererVisible is clear -- the zero-fill trap. World::addComponent
    // hands back zeroed storage and the visible bit is positive-sense, so a renderer attached
    // directly is attached, correct, and invisible.
    NotVisible,
    // The mesh id resolves to no handle in GameContent. Worth reporting per ID rather than per
    // entity: many entities can name the same missing mesh, and it is the id that identifies the
    // fault, not whichever entity reached it first.
    MeshNotLoaded,
};

// ONE in/out struct per entity, handed to DrawWorldOptions::decide after the walk has computed this
// entity's world-space bounds and its frustum/owner-hide verdict and before it commits to a route.
//
// THE BOX IS AN INPUT, NOT SOMETHING THE HOST RECOMPUTES. That is the whole reason this is a struct
// rather than a handful of narrower callbacks: SandboxRender.cpp's depth-prepass phase records what
// happened when two walks derived "the same" bounds separately -- one fed LOD selection a sphere
// built from LOCAL aabbMin/aabbMax against a world-space eye, the other from the world-space corners,
// and the two picked different LOD levels for the same instance. A host that needs the box for LOD,
// for an occlusion query or for a caster-size floor must be handed the one the cull already used.
//
// `haveWorldBox` false means the entity's bounds were degenerate and no world box was computed --
// the walk draws it rather than culling it ("must not vanish"), and a host must guard its own box
// readers the same way or it reintroduces the divergence above by a different route.
struct EntityDecision {
    // ---------------------------------------------------------------- in
    DrawWorldPass pass = DrawWorldPass::Colour;
    scene::Entity entity = scene::kInvalidEntity;
    // Position in the VISIT order, not the world order: this is `oi`, the loop counter, so it still
    // means something when DrawWorldOptions::visitOrder has reordered the walk.
    u32 visitIndex = 0;
    u64 meshId = 0;              // CMeshRenderer::mesh, the content-hash asset id
    i32 material = 0;            // the entity's own fallback token, 0 already resolved through
                                 // GameContent::meshDefaultMaterial
    const Mat4* world = nullptr; // World::worldMatrix(entity); never null, valid for this call only
    Vec3 worldBoxMin{};          // world-space extent of the entity's box, meaningless unless
    Vec3 worldBoxMax{};          // haveWorldBox
    bool haveWorldBox = false;
    bool skinned = false;        // SkinnedScene claimed this entity (its bounds are its POSED ones)
    bool frustumCulled = false;
    bool ownerHidden = false;
    rhi::MeshHandle baseMesh = 0;   // GameContent::meshFor(meshId), the unsubstituted geometry the
                                    // per-material split was cut from
    rhi::MeshHandle posedMesh = 0;  // SkinnedScene's substituted handle, or 0

    // ---------------------------------------------------------------- out
    // Each field arrives holding THE WALK'S OWN ANSWER, so a decide() that returns without touching
    // anything is exactly a decide() that was never installed.

    // Drop this entity entirely: no draw on either route, and no counter incremented. The editor's
    // PlayerStart marker is this case -- it draws as an icon instead, and skipping it by identity
    // keeps it out of the opaque pass, the shadow cascade, GI and the RT acceleration structure at
    // once (SandboxRender.cpp's colourDecide, which skips it by identity).
    //
    // IT IS LATE. decide() runs AFTER the walk has written this entity's asset bounds back into its
    // CMeshRenderer and computed the world box, so `skip` cannot un-do that write. A host whose
    // skip must precede it needs a check of its own ahead of the call, not this flag.
    bool skip = false;
    // The host's own occlusion verdict, joined with frustumCulled by chooseRoute. Hierarchical-Z
    // occlusion culling is editor-only machinery (Aver.Occlusion is linked into Sandbox alone), so
    // this library has no way to answer it and no business trying.
    bool occlusionCulled = false;
    // Ask for the occlusion.showCulled debug tint: the culled entity draws through the RASTER route
    // anyway, with its green channel knocked down to 0.15 so a false cull reads as obvious magenta.
    // Fed straight to chooseRoute's `showCulled`, so it is honoured exactly where chooseRoute
    // honours it -- a culled, NON-owner-hidden entity -- and changes nothing anywhere else. A mesh
    // hidden from its own owner is not a "culled" entity in the sense this debug view is for.
    bool tint = false;
    // Which geometry this entity actually draws. Pre-filled with posedMesh when skinning claimed
    // the entity and baseMesh otherwise -- the substitution this walk has always made.
    //
    // THE SOFT-BODY SEAM IS HERE. The editor's SandboxApp::posedHandle asks skinning
    // first and then a soft-body scene, deterministically in that order because nothing forbids
    // CSoftBody on an already-skinned mesh; this library has no soft-body scene to ask. A host with
    // one fills in where posedMesh came back 0. planEntityDraws then decides on its own whether the
    // per-material split still applies, by comparing this against baseMesh.
    rhi::MeshHandle chosenMesh = 0;
    // This entity's lit pixels have ALREADY been produced by something the host dispatched itself,
    // so device.drawMesh() must not run for it -- but its shadow/GI/TLAS submission still must.
    // The editor's GPU cluster mesh-shader path is this case: dispatchMeshClusters() draws the
    // geometry, and skipping drawMesh() also skips IRenderFeature::submitDraw, which is the ONLY
    // way geometry reaches Voxi (SandboxRender.cpp's colourDecide answers this flag from its own
    // dispatchMeshClusters call, and the walk then takes the direct route for exactly that
    // reason). The entity still counts as drawn.
    bool colourAlreadyDrawn = false;
    // Emit this entity's DIRECT-route draws at all. False delivers nothing to the VoxiRenderer and
    // fires no onDirectDraw, while the entity is still counted as culled or owner-hidden exactly as
    // it would have been -- which is what separates it from `skip`, whose entity is counted nowhere.
    //
    // IT EXISTS FOR THE ANGULAR-SIZE FLOOR and, so far, nothing else. Submitting EVERY culled entity
    // to Voxi was measured at +64ms/frame in ElectricDreams -- 5,884 of 6,617 entities culled, mostly
    // scatter plants, taking a full TLAS rebuild from 759 to 6,571 instances and 9.1ms to 64.2ms --
    // so the editor drops a caster too small to fill a shadow texel before the WORK, not merely
    // before the draw (kMinCasterAngle, applied in SandboxRender.cpp's colourDecide). That floor is a
    // heuristic about SHADOWS, applied per entity, and asymmetric by construction: an entity inside
    // the frustum reaches the renderer with no size test at all. A library with no cascade of its
    // own to protect has no business making that trade, so it is the host's answer, and the default
    // is the honest one, which is to submit.
    bool emitDirectDraws = true;
    // The ENTITY-level half of depth-prepass eligibility, for the COLOUR pass: true means a
    // DepthPrepass call already wrote this entity's depth, so each of its opaque draws may ask for
    // the LessEqual/no-write pipeline. Per-draw translucency is still the walk's own answer -- a
    // mesh with an opaque trunk and a translucent leaf part gets it right per part, which a single
    // entity-level flag could not (SandboxRender.cpp's colourDecide sets only the entity half, and
    // says so, for the same reason). Left false here means "nobody prepassed this", which is what
    // a host with no prepass walk should say and what this library has always assumed.
    bool prepassEligible = false;
};

// FUNCTION POINTERS, NOT std::function, throughout: this is a per-entity path over thousands of
// entities per frame, and the idiom is already this library's -- GameContent::setMeshLoadedHook(fn,
// void*) (GameContent.hpp:154) and GameApp::setAverSrInstaller (GameApp.hpp:192) both take one.
// GameLevel::LoadHooks uses std::function because it fires a handful of times per LEVEL LOAD; that
// is the distinction, not a disagreement.

// Fired at the TOP of each iteration, before any component lookup and before any filtering -- ahead
// of the destroy-pending test, the visible-bit test and the mesh lookup, so it fires for EVERY
// visited index whatever becomes of it.
//
// IT EXISTS FOR ONE ORDERING REQUIREMENT AND CANNOT BE HOISTED. The editor's occlusion walk splits
// into two passes at a raw index into the unfiltered visit order, and must record buildPyramid() at
// exactly that point IN THE COMMAND STREAM -- it builds the pyramid from the depth that pass 1's
// draws have just written (SandboxRender.cpp's colourVisit, which fires occlusionBuildAndTest at
// exactly that boundary). Moving that call into a pre-pass, or firing it
// after this entity's filtering, would build the pyramid against the wrong set of draws. It is a
// statement about WHEN, not about data, which is why it returns nothing.
using DrawWorldVisitFn = void (*)(u32 visitIndex, scene::Entity entity, void* user);

// See EntityDecision.
using DrawWorldDecideFn = void (*)(EntityDecision& decision, void* user);

// Once per entity the COLOUR pass actually delivered, on either route, after its draws are emitted.
// The editor captures its selection-outline transform and mesh here, for every selected entity
// rather than only the anchor (SandboxRender.cpp's colourDelivered). `chosenMesh` is the
// substituted handle the draws actually used, not the base one -- an outline traced from the base
// handle while the entity rendered a posed or LOD copy would draw the wrong silhouette.
//
// `raster` SAYS WHICH ROUTE, because "delivered" and "on screen" are not the same claim and the
// editor's use needs the second one: an entity delivered on the direct route was culled or
// owner-hidden, so it reached Voxi's shadow/GI/TLAS submission and nothing else. Never fired for an
// entity the walk skipped, nor for one whose direct draws EntityDecision::emitDirectDraws
// suppressed, nor at all in the depth-prepass pass.
using DrawWorldEntityDeliveredFn = void (*)(scene::Entity entity, u64 meshId,
                                            rhi::MeshHandle chosenMesh, const Mat4& world,
                                            bool raster, void* user);

// Once per draw on the DIRECT route, immediately after the Voxi submit that draw made (or in its
// place, when no VoxiRenderer is attached). Arguments mirror VoxiRenderer::submit's own order.
//
// The editor forwards these to its path-traced scene view, whose submitDraw is otherwise reached
// only through drawMesh() -- the very call the direct route exists to skip -- so without this the
// path tracer traced a scene holding only what the camera could see: no roof overhead, no wall
// behind it (SandboxRender.cpp's colourDirect carries the measurement). THIS LIBRARY LINKS NO PATH
// TRACER AND NEVER WILL, which is the whole reason this is a sink rather than a second submit call.
//
// `col` is the FINAL colour, tint already applied. It, `world` and `matConstants` all point at
// storage that lives only for the duration of this call.
using DrawWorldDirectDrawFn = void (*)(rhi::MeshHandle mesh, const f32 world[16], const f32 col[4],
                                       f32 metallic, f32 roughness, rhi::BindingSetHandle matSet,
                                       const void* matConstants, u32 matBytes, bool translucent,
                                       bool hiddenFromOwner, void* user);

// Once per material token per process, for each warning kind -- the library owns that throttle, the
// host owns the sentence. See SurfaceWarning. Null leaves the walk printing its own "[Game]" lines,
// which is what it has always done.
using DrawWorldSurfaceWarnFn = void (*)(i32 material, SurfaceWarning kind, void* user);

// Every entity the walk dropped before either route, with the reason. NOT throttled here: the two
// reasons want different keys (per entity for NotVisible, per mesh id for MeshNotLoaded) and only
// the host knows which of its own sets it is filling. Null drops them in silence, exactly as this
// walk always has.
using DrawWorldSkippedFn = void (*)(scene::Entity entity, u64 meshId, DrawSkipReason reason,
                                    void* user);

// Widens drawWorld's cull/owner-hide behaviour, and now its whole per-entity route, without widening
// its required arguments -- EVERY field here defaults to the behaviour that already exists, so a
// default-constructed DrawWorldOptions reproduces the walk this struct was added to change not one
// bit. That property is what the hook set is verified against: with every pointer null, the walk
// below must emit the identical command stream it emitted before the hooks existed.
struct DrawWorldOptions {
    // The possessed first-person pawn. Entities flagged kMeshRendererHiddenFromOwner under it (ancestor walk) are not drawn
    // in the raster pass. scene::kInvalidEntity = no owner-hide.
    scene::Entity ownerHideRoot = scene::kInvalidEntity;

    // Where a frustum-culled or owner-hidden entity's draw goes INSTEAD of the raster pass -- the
    // "unified direct route" of aver/game/SceneSubmission.hpp, which this walk now owns outright
    // (it was the editor's own emitEntityDraws else-branch calling voxiRenderer_.submit() until
    // that walk was deleted, and both hosts reach it through this field now):
    // device.drawMesh() is not an option here, because drawMesh both
    // broadcasts to every registered IRenderFeature AND rasterises to the backbuffer, and the whole
    // point of this route is the first half without the second (an off-screen caster must reach
    // Voxi's shadow/GI/TLAS submission without appearing on screen; an owner-hidden mesh is usually
    // sitting squarely IN the frustum, so drawMesh() would draw it regardless of culling). Null (a
    // game with no Voxi feature, or a caller that has not attached one yet) reproduces today's
    // behaviour exactly: a culled or owner-hidden entity is skipped and casts no shadow while it is.
    voxi::VoxiRenderer* voxiRenderer = nullptr;

    // Which entities move during a play session (see PlayMobility.hpp). A movable entity's draws
    // reach Voxi flagged `movable`, which keeps them out of the GI bake. Null, or an inactive
    // tracker, flags nothing -- the editor outside Play.
    PlayMobility* mobility = nullptr;

    // Which walk this is. See DrawWorldPass.
    DrawWorldPass pass = DrawWorldPass::Colour;

    // The order to visit entities in: an array of indices into scene::World's own order, and its
    // length. Null means 0..World::count()-1, which is the order this walk has always used.
    //
    // The editor's occlusion culler REORDERS its walk so that everything it saw last frame draws
    // first and everything else draws after the pyramid is built (SandboxRender.cpp's
    // occlusionOrder_), and the pass-1/pass-2 boundary it fires buildPyramid at is an index into
    // THIS order. An order the walk re-derived, or filtered, would put that boundary somewhere else.
    // An out-of-range index is skipped rather than handed to World::at.
    const u32* visitOrder = nullptr;
    u32 visitOrderCount = 0;

    // Do not print the "[Game] scene-render:" line at all. SceneDrawStats' counts are still
    // written, so a host that words its own sentence gets the numbers without the library also
    // writing one it did not want -- see SceneDrawStats' own comment for why the two sentences
    // cannot be unified.
    bool suppressLog = false;

    // Collapse WalkLookup's four per-entity GameContent probes (meshFor, boundsFor,
    // meshDefaultMaterial, partsFor -- CpuSpan's own comment names all four) to at most one
    // resolution of each PER DISTINCT MESH ID this call visits, through a fixed-size, open-addressed
    // table that is reused from call to call but logically EMPTY at the start of each one -- nothing
    // one call learned is visible to the next, which is what "local to this one call" always
    // meant (GameRender.cpp's own comment on kMeshLookupCacheSlots and WalkCacheLease has the shape
    // and the reasoning, including why a single "last mesh" slot was rejected on purpose rather
    // than by omission, and what a re-entrant call gets). A call that cannot claim the table (a
    // hook that re-entered drawWorld, or a second thread) runs as if this were false and reports
    // 0 hits and 0 misses.
    //
    // ON BY DEFAULT, AND THAT IS THE POINT OF STAGE 2. All four probes are a `find()` plus a
    // return -- const, no lazy upload, no side effect of any kind (verified by reading
    // GameContent.cpp: meshFor, boundsFor, partsFor, meshDefaultMaterial) -- so memoising them
    // changes no draw, no branch and no order; it is pixel-neutral BY CONSTRUCTION, not merely
    // probably safe, and an optimisation with that property should be what a caller gets without
    // having to ask for it.
    //
    // THE ESCAPE HATCH IS FOR THE ONE THING THAT COULD MAKE IT UNSAFE, not for doubt about whether
    // it helps. boundsFor and partsFor return pointers/values sourced from GameContent's own
    // unordered_maps, and this cache can hold onto a mesh id's answer across many entities and,
    // therefore, across many calls to `decide` -- a HOST callback that runs mid-loop and, in
    // general, can run arbitrary editor code. std::unordered_map's own guarantee is narrower than
    // it looks: inserting into it, and any rehash that causes, invalidates ITERATORS but never
    // invalidates a reference or pointer to an element already in the table -- only ERASING that
    // element does. Nothing reachable from either host's `decide` (or any other hook this struct
    // exposes) erases from GameContent's mesh tables today -- SandboxRender.cpp's colourDecide and
    // prepassDecide were read end to end to confirm it, and every other installed hook
    // (colourVisit, colourDelivered, colourDirect, colourWarn/prepassWarn, colourSkipped) was
    // searched for a call into GameContent's own mutators (registerMesh, loadProjectMeshes,
    // releaseProjectMeshes, adopt) and a level (re)load trigger, and found to make none -- so the
    // cache is safe against every caller that exists. A FUTURE host whose `decide`, `onVisit` or
    // other hook starts tearing down or reloading meshes THIS SAME WALK IS STILL READING would be
    // the exception, and this switch
    // is how it opts back out without a recompile: OFF sends every WalkLookup call site straight to
    // its original, direct-to-GameContent probe, with none of the caching machinery touched, which
    // is what this file did before this field existed. A compile-time switch was rejected instead
    // of this one for the same reason the toggle exists at all: a `-D` reconfigure of a shared
    // build tree is measured in this repo to move rendering by 16% of pixels on ITS OWN
    // (see aver-reconfigure-pollutes-build in the engine's own notes), which would make an A/B
    // comparison of this cache untrustworthy no matter how carefully the two builds were compared.
    // A runtime bool lets both paths be measured in the SAME binary.
    bool useMeshLookupCache = true;

    // The per-entity sinks. See each type for what it exists to serve.
    DrawWorldVisitFn onVisit = nullptr;
    DrawWorldDecideFn decide = nullptr;
    DrawWorldEntityDeliveredFn onEntityDelivered = nullptr;
    DrawWorldDirectDrawFn onDirectDraw = nullptr;
    DrawWorldSurfaceWarnFn onSurfaceWarn = nullptr;
    DrawWorldSkippedFn onSkipped = nullptr;

    // ONE user pointer for all six, the same shape GameContent::setMeshLoadedHook uses. Every sink
    // here belongs to the one host driving the walk; a per-hook pointer would only invite two.
    void* user = nullptr;
};

// Draws every live entity carrying a visible CMeshRenderer whose mesh resolves.
//
// Takes the device and the content rather than the app: the walk needs a mesh table and somewhere
// to send triangles, and nothing else. Takes viewProj by value-ref because the frustum is derived
// from it per frame.
// `materials` may be null: a game with no Voxi feature has no material system, and every
// surface then draws with its named-surface look or the neutral fallback.
// `skinning` may be null: init compiles HLSL at runtime and can fail on a machine where the build
// was green, and a null feature means skinned entities draw at their REST POSE rather than not at
// all. A character that fails to skin must still appear.
void drawWorld(rhi::IDevice& device, const Mat4& viewProj, GameContent& content, SceneDrawStats& stats,
               pbr::MaterialSystem* materials, render::SkinnedScene* skinning,
               const DrawWorldOptions& options = {});
#endif

} // namespace aver::game
