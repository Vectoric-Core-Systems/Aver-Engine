// Runtime side: the world draw (onRender), surface resolution, occlusion, AverSR and the path-traced scene view.
// Split out of the 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies verbatim;
// class declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
// game::drawWorld + hook set: the one entity walk both hosts run; onRender's depth prepass below
// calls it too. Included here, not in SandboxApp.hpp, since no member declaration needs these
// types (the decide/warn sinks are captureless lambdas local to onRender) -- so only this TU pays
// for it.
#include "aver/game/GameRender.hpp"
// CpuSpan/CpuLap/CpuNest, formatCpuTiming: CPU-side twin of rhi::GpuTimingReport, used below to
// wrap the cluster-dispatch timing in a CpuNest and print the per-bucket breakdown. Same
// include-here-not-in-header reasoning as GameRender.hpp above -- both headers are
// dependency-minimal by design (CpuTiming.hpp's own top comment: "included from Runtime/, from
// modules/render.voxi/ and from sandbox/").
#include "aver/core/CpuTiming.hpp"
#include "aver/core/CpuTimingFormat.hpp"

namespace aver {

// --no-walk-cache's storage, gating DrawWorldOptions::useMeshLookupCache (GameRender.hpp) at both
// drawWorld call sites below (depth-prepass and colour walk).
//
// TRANSLATION-UNIT-LOCAL, not a SandboxApp member like every other CLI toggle this file reads
// (noEditorChrome_, occlusionCullForceOff_, depthPrepassOverride_, ...): this wiring stage was
// scoped to this file and SandboxMain.cpp alone. `static` gives it internal linkage;
// SandboxMain.cpp reaches it only through setNoWalkCacheArg() below.
//
// DEFAULTS FALSE, matching DrawWorldOptions::useMeshLookupCache defaulting TRUE: negated at each
// call site rather than mirrored here, so this file need not know that header's own default.
static bool g_noWalkCacheArg = false;

// Forward-declared in SandboxMain.cpp; called once from its argv loop after parsing
// --no-walk-cache, at the same point every other app->setXxx(...) CLI setter is called. `on` is
// the flag's own sense (true = cache OFF); negated at each drawWorld call site, next to the field
// it feeds.
void setNoWalkCacheArg(bool on) { g_noWalkCacheArg = on; }

#if AVER_MODULE_SCENE
// THE ENTITY-LEVEL RESOLVE'S "ALREADY DONE THIS WALK" FILTER (colourDecide). A small direct-mapped
// table of material tokens, one per ColourWalk, so it starts empty every frame. A miss just means
// "resolve it", which is what colourDecide did for EVERY entity before this existed, so a slot that a
// colliding token keeps evicting costs speed and never a skipped resolve. TRANSLATION-UNIT-LOCAL for
// the same reason as g_noWalkCacheArg: SandboxApp.hpp is not part of this change.
// SIZED FOR A CITY, NOT A ROOM: a level names hundreds of distinct tokens (NeonDistrict: ~650
// materials), and at 128 direct-mapped slots they evicted one another so often that most entities
// resolved anyway. 4,096 slots with a 4-slot probe keeps "once per token per walk" true at that scale
// for ~20 KB zeroed per frame.
struct EntityTokenMemo {
    static constexpr u32 kBits = 12;
    static constexpr u32 kSlots = 1u << kBits;
    static constexpr u32 kProbe = 4;
    i32 token[kSlots] = {};
    bool seen[kSlots] = {};
    // True the first time `t` is offered this walk (or since it lost its slot); the caller resolves
    // on true.
    bool first(i32 t) {
        u32 slot = (static_cast<u32>(t) * 2654435761u) >> (32u - kBits);
        for (u32 p = 0; p < kProbe; ++p, slot = (slot + 1) & (kSlots - 1)) {
            if (!seen[slot]) { seen[slot] = true; token[slot] = t; return true; }
            if (token[slot] == t) return false;
        }
        token[slot] = t;          // window full: take the slot past it, as the direct map always did
        seen[slot] = true;
        return true;
    }
};

// WHEN THE "[Sandbox] scene-render:" LINE WAS LAST PRINTED, and what it said. Kept apart from
// SandboxApp's lastSceneDrawn_/Culled_/OwnerHidden_ on purpose: those are STATE other code reads
// (startupComplete's settle detector compares lastSceneDrawn_ frame to frame), so they must follow
// every frame; this only decides whether to WRITE the line. Translation-unit-local like
// g_noWalkCacheArg -- one SandboxApp per process, and SandboxApp.hpp is not part of this change.
struct SceneRenderLogState {
    int drawn = -1;
    int culled = -1;
    int ownerHidden = -1;
    std::chrono::steady_clock::time_point at{};
};
static SceneRenderLogState g_sceneRenderLog;
#endif  // AVER_MODULE_SCENE

// Submits the frame: the editor scene, the level world, gizmos, and the overlays.
void SandboxApp::onRender(Engine& e)  {
    handleManip(e);
#if AVER_MODULE_LANDSCAPE
    // Drains an undo/redo that changed terrain heights; deferred to here as the first point after
    // those run that has a device (see GameLandscape::applyHeightRect's own comment).
    landscape_.flushPendingInvalidate(e.device());
#endif
    // Loading screen comes down here, not when applyProject returned: holds until the draw count
    // settles so it covers the tail of the load, not just the head -- startupComplete() ticks the
    // same settle counters, so this is the one place deciding "finished".
    //
    // A borrowed screen (command-line load) is not timed here: Engine::run's warm-up loop already
    // calls startupComplete() once a frame, so a second call would tick the settle counter twice
    // and halve the tail. Follows the engine's own splash instead, drops the wrapper when it closes.
    //
    // Capped at kLoadingScreenFrameCap: the settle rule never fires for a project that draws
    // nothing (empty map, or every mesh failed to load), which would otherwise leave the splash up
    // forever over an editor that is actually running -- matches Engine's own 600-frame warm-up bound.
    constexpr int kLoadingScreenFrameCap = 600;
    if (projectLoading_) {
        if (projectLoading_->borrowed) {
            if (!projectLoading_->borrowed->loadingScreenActive()) projectLoading_.reset();
        } else if (++projectLoadingFrames_ >= kLoadingScreenFrameCap || startupComplete()) {
            projectLoading_.reset();
        }
    }

    applyStartMode();
    e.device()->setWireframe(wireframe_);
    // UNLIT for the whole scene pass, alongside wireframe (shared_prelude.hlsl's `gMaterial.z >
    // 0.5` branch was already live on both backends since the selection outline needed it, at
    // frame constant 22; only the view-mode entry was missing).
    // Cleared at the end of the pass so it cannot leak into editor chrome.
    e.device()->setUnlit(unlit_);
    // Whether a level is OPEN, not whether it has anything in it. Used to read
    // `|| !levelEntities_.empty()`, which hid the built-in Floor/Cube placeholders the instant the
    // first object was added to an empty level (reported as "drag and drop replaces the default
    // stuff" -- nothing was replaced, the placeholders just hid in the same frame). levelEntities_
    // is AVER_MODULE_SCENE-only, so the OR term is simply absent, not always-false, when it's off.
#if AVER_MODULE_SCENE
    hideEditorScene_ = playSessionActive() || !levelPath_.empty();
#else
    hideEditorScene_ = playSessionActive();
#endif
    const bool hideEditorScene = hideEditorScene_;
    for (int i=0;i<(int)objects_.size();++i) {
        MeshObj& o = objects_[i];
        if (!o.visible || hideEditorScene || !showStaticMeshes_) continue;
        Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
        Mat4 w = tr.toMatrix();
        f32 col[4]={o.color[0],o.color[1],o.color[2],1};
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // setDrawBinding is sticky, so it is set unconditionally before every draw.
        if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready())
            e.device()->setDrawBinding(ms.bindingSet(o.material), &ms.constants(o.material),
                                       sizeof(pbr::MaterialConstants));
#endif
        // setDrawBlended is likewise sticky; forced false since this loop only draws opaque
        // editor placeholders (never an authored .ocmat) -- not left to draw ORDER, which a later
        // reorder (a new overlay above this loop) would silently break.
        e.device()->setDrawBlended(false);
        e.device()->drawMesh(o.mesh, &w.m[0][0], col, o.metallic, o.roughness);
        // Floor/Cube placeholders have no .ocmesh behind them, so they get no outline --
        // selectionMeshId_ stays 0 and selectionOutlineLines returns 0 for it.
        if (i == sel_) selectionOutline_ = w, selectionMesh_ = o.mesh, selectionMeshId_ = 0,
                       hasSelection_ = true;
    }

    // ---- the simulated fluid, drawn like every other surface in the level ----
    // Went from an ordinary opaque mesh, to its own FluidScene::transparentPass ("tinted plastic",
    // invisible to shadows/TLAS/GI), to this: a normal TRANSLUCENT draw through
    // VoxiRenderer::submitDraw, sorted/shadowed/fogged/shaded like anything else while skipping the
    // two depth-only passes translucency can't express. Being in the TLAS matters because
    // averVolumeThickness needs a real back face to trace for path length/absorption.
    // NOT GATED ON hideEditorScene: authored level content, stays visible through Play like the
    // landscape below. Material resolution, the fallback look and placement live in GameWater::draw.
#if AVER_MODULE_FLUIDS
    {
        pbr::MaterialSystem* fluidMaterials = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        fluidMaterials = &voxiRenderer_.materials();
#endif
        water_.draw(*e.device(), content_, fluidMaterials);
    }
#endif

#if AVER_MODULE_LANDSCAPE
    // Landscape pass: ring update then one draw through game::GameLandscape, hand-rolled like the
    // objects_ loop above (not an IRenderFeature). Not gated on hideEditorScene: real environment
    // geometry, stays visible through Play like the sky and fog.
    if (landscape_.loaded()) {
        pbr::MaterialSystem* landscapeMaterials = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        landscapeMaterials = &voxiRenderer_.materials();
#endif
        landscape_.updateRingTiles(e.device(), eye_.x, eye_.y, &content_, landscapeMaterials);
        landscape_.draw(*e.device(), eye_, viewProj_, vpH_, &content_, landscapeMaterials);
    }
#endif

#if AVER_MODULE_SCENE
    // Scene-entity pass: draws every live entity carrying a CMeshRenderer.
    {
        scene::World& w = scene::World::instance();
        // One frame of the Play mobility tracker (PlayMobility.hpp), before either walk asks it
        // anything. The possessed pawn's whole tree is movable from its first frame: the viewmodel
        // hangs off its camera and moves whenever the player looks around.
        {
            scene::Entity movableRoot = scene::kInvalidEntity;
#if AVER_MODULE_FRAMEWORK
            if (playSessionActive()) {
                const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pn > 0) movableRoot = static_cast<scene::Entity>(static_cast<u32>(pn));
            }
#endif
            playMobility_.beginFrame(movableRoot);
        }
        // The three drawn/culled/owner-hidden counters belong to game::drawWorld now: it fills a
        // SceneDrawStats the editor reads back (`colourStats` below) rather than incrementing
        // locals itself. 3B's per-frame direct-route tallies (draws delivered, multi-part count for
        // culled entities) moved with them into ColourWalk, the hook context below.
#if AVER_MODULE_TRIFACTOR
        lodStats_ = LodSelectStats{};   // this frame's counters, from zero -- see the struct comment
        lodClusterStats_ = LodClusterStats{};
        lodMeshShaderStats_ = LodMeshShaderStats{};
        ++lodClusterFrame_;
        // Sweeps the per-instance cut cache: an entry unused for 256 frames (entity destroyed,
        // CMeshRenderer removed, or the toggle turned off) would otherwise leak its GPU handle
        // forever -- no destruction hook catches that here. 256-frame grace so a briefly off-screen
        // instance isn't mistaken for gone; checked only every 64 frames.
        if (lodPerClusterEnabled_ && (lodClusterFrame_ % 64 == 0)) {
            for (auto it2 = clusterCutCache_.begin(); it2 != clusterCutCache_.end();) {
                if (lodClusterFrame_ - it2->second.lastUsedFrame > 256) {
                    if (it2->second.handle) e.device()->destroyMesh(it2->second.handle);
                    it2 = clusterCutCache_.erase(it2);
                } else {
                    ++it2;
                }
            }
        }
#if AVER_MODULE_VOXI
        // Re-samples Voxi's current gVoxelTex_/shadowTex_ handles into every cluster mesh's
        // table-0 binding set before any entity draws through it this frame -- one pass over
        // meshClusterGpu_ (bounded by distinct meshes, not instances), keeping tables correct
        // across a live GI-quality/shadow-resolution change that recreates those handles.
        if (lodMeshShaderEnabled_ && lodMeshPipelineReady_) {
            if (rhi::IResourceFactory* giRes = e.device()->resources())
                for (auto& kv : meshClusterGpu_)
                    if (kv.second.bindingSet)
                        voxiRenderer_.bindGiResources(*giRes, kv.second.bindingSet, kClusterGiSrvBase);
        }
#endif
#endif

        // The six frustum planes are no longer derived here: both walks are game::drawWorld now,
        // which builds them from viewProj with the same two dozen adds (GameRender.cpp) -- one
        // spelling instead of two that could drift.
#if AVER_MODULE_VOXI
        // ---- depth prepass phase: the COLOUR WALK'S OWN FUNCTION, run a second time ----
        // ONE ScopedGpuStat, not one per draw: span budget is 64/frame and Electric Dreams submits
        // over a thousand instances -- per-draw markers would also measure wrong, folding colour
        // time into "depth prepass" if interleaved (a span covers everything between its two
        // timestamps in submission order). DrawWorldPass::DepthPrepass emits only setDrawBinding/
        // drawMeshDepthPrepass (never the direct route, never touches a counter), so the span still
        // closes over a depth-only run.
        //
        // A CALL, NOT A LOOP: this used to be a hand-duplicated copy of the colour walk (own entity
        // iteration, own planEntityDraws, own trifactor::chooseLevelCached), and the duplication
        // drifted -- the world-space box was scoped differently in each copy, so LOD selection
        // picked a DIFFERENT LEVEL per walk and fragments behind the wrong depth were silently
        // dropped (fern clumps: 2.81% of pixels differing, falling to 0.04% noise with
        // --no-lod-select). One function called twice cannot disagree with itself.
        //
        // EXCLUDED via decide() below: skinned entities, the GPU cluster mesh-shader path, the CPU
        // per-cluster path (--lod-per-cluster) (none has a depth-only twin) -- LANDSCAPE has its own
        // call site instead. A translucent planned draw is excluded inside the library (glass must
        // never write opaque depth, a fact about the material, not the LOD/skinning system). Also
        // now answered here, on the colour pass's own conditions: the walk this replaced tested
        // destroyPending, visible, mesh, skinned, cluster and frustum -- but never owner-hide and
        // never PlayerStart, so the possessed pawn's body and the PlayerStart marker (both chrome
        // the colour walk skips by identity) each punched an opaque-depth hole where colour then
        // refused to draw. Reachable only with --depth-prepass, which rhi::RHI.hpp and the D3D12
        // device both default OFF, hence unreported for so long.
        //
        // drawWorld refreshes a non-skinned entity's aabb from GameContent::boundsFor in BOTH
        // passes before culling -- decide() runs after that write and cannot undo it
        // (EntityDecision::skip's own contract) -- so this only disagrees with the colour walk for
        // one frame, the first frame an asset's bounds exist (a level load, or a streamed mesh
        // arriving).
        //
        // NO visitOrder: occlusionOrder_ reorders the COLOUR walk for buildPyramid(); this pass has
        // no pyramid and no pass-1/pass-2 boundary, so world order (null) is correct.
        // Not in Wireframe: it draws no scene colour, so there is nothing for depth to save.
        if (e.device()->depthPrepassEnabled() && !wireframe_) {
            if (rhi::IRenderContext* pctx = e.device()->renderContext()) {
                rhi::ScopedGpuStat prepassScope(*pctx, "depth prepass");

                // CAPTURELESS, so it converts to the plain DrawWorldDecideFn function pointer the
                // hook set takes (per-entity path over thousands of entities/frame, not
                // std::function). A lambda inside a member function is a local class of it, so
                // `user` reaches SandboxApp's private members without a SandboxApp.hpp declaration.
                auto prepassDecide = [](aver::game::EntityDecision& d, void* user) {
                    // Posed vertices live in a compute-written buffer, so the only depth this walk
                    // could write is the REST pose's -- not the one colour draws.
                    if (d.skinned) { d.skip = true; return; }
                    {
                        // PlayerStart is chrome in both passes: the colour walk skips it by identity
                        // and draws a billboard icon instead, so prepassing it wrote depth for a
                        // cube that's never drawn -- the same owner-hide hole, by a second route.
                        SandboxApp& ps = *static_cast<SandboxApp*>(user);
                        if (d.entity == ps.playerStart_ && ps.viewportIconsReady_) { d.skip = true; return; }
                    }
#if AVER_MODULE_TRIFACTOR
                    SandboxApp& self = *static_cast<SandboxApp*>(user);
                    // dispatchMeshClusters has no depth-only twin to call.
                    if (self.lodMeshShaderEnabled_ && self.lodMeshPipelineReady_ &&
                        self.meshClusterGpu_.count(d.meshId)) { d.skip = true; return; }
                    // clusterCutCache_ is rebuilt-or-reused once per frame per entity; running that
                    // decision twice would duplicate the rebuild or read a cache the colour walk
                    // hasn't populated yet -- not unsafe, just unneeded for a path nothing in this
                    // session enables.
                    if (self.lodPerClusterEnabled_ && self.meshClusterData_.count(d.meshId)) {
                        d.skip = true; return;
                    }
                    // The cull already answered by the time decide() runs, and a culled entity
                    // emits no depth, so choosing its level would be work for a draw that never happens.
                    if (d.frustumCulled) return;
                    // Guarded on haveWorldBox like the colour walk: an instance with no usable box
                    // must take LOD 0 in BOTH walks. Fed the box the cull already used, not one
                    // re-derived here -- re-deriving it is what made the two copies disagree before.
                    if (self.lodSelectEnabled_ && d.haveWorldBox) {
                        if (const auto lit = self.meshLods_.find(d.meshId);
                            lit != self.meshLods_.end()) {
                            const auto& ladder = lit->second;
                            const Vec3 sphereCenter = (d.worldBoxMin + d.worldBoxMax) * 0.5f;
                            const f32 sphereRadius = dist(d.worldBoxMin, d.worldBoxMax) * 0.5f;
                            trifactor::View pview;
                            pview.eye = self.eye_;
                            pview.viewProj = self.viewProj_;
                            pview.viewportHeightPx = self.vpH_;
                            pview.verticalFovRadians = radians(60.0f);
                            const u32 plevel = trifactor::chooseLevelCached(
                                ladder.errorCm, sphereCenter, sphereRadius,
                                self.lodErrorThresholdPx_, pview);
                            d.chosenMesh = ladder.handles[plevel];
                        }
                    }
#else
                    (void)user;
#endif
                };

                // The editor's own wording, through the editor's own throttles: drawWorld owns the
                // once-per-token bookkeeping (SurfaceWarning), but re-running resolveSurface() here
                // fills the SAME function-local static sets the colour walk's resolveSurface() does,
                // so a token warns exactly once per process, not once per walk. `kind` goes unread --
                // the shared ladder re-derives it from the same inputs, at a cost of one duplicated
                // lookup per token per process (the library's throttle only calls this once, for a
                // token never seen before).
                auto prepassWarn = [](i32 mat, aver::game::SurfaceWarning kind, void* user) {
                    (void)kind;
                    (void)static_cast<SandboxApp*>(user)->resolveSurface(mat);
                };

                aver::game::DrawWorldOptions popt;
                popt.pass = aver::game::DrawWorldPass::DepthPrepass;
                popt.decide = prepassDecide;
                popt.onSurfaceWarn = prepassWarn;
                popt.user = this;
                // Same owner-hide root the colour pass uses -- without it this pass wrote opaque
                // depth for the possessed pawn's own body, which colour then refused to draw: a
                // hole in the shape of the character you're playing. Both passes must agree on
                // "is this entity drawn" or the prepass writes depth for something that never appears.
                popt.ownerHideRoot = firstPersonPawn_;
                popt.mobility = &playMobility_;
                // --no-walk-cache, negated (g_noWalkCacheArg true = cache OFF). MUST MATCH the
                // colour call site's copt.useMeshLookupCache below -- both passes resolve the same
                // mesh ids, so caching one and not the other would measure two different things
                // under one flag.
                popt.useMeshLookupCache = !g_noWalkCacheArg;
                // NO voxiRenderer/onDirectDraw: the depth pass returns at `!route.raster` before
                // either sink is reachable. NO onSkipped either -- that warning is the colour
                // walk's job, once each, into sets this call must not reach first.
                pbr::MaterialSystem* prepassMaterials = nullptr;
#if AVER_MODULE_PBR
                // Same MaterialSystem resolveSurface() binds out of, so each part's descriptor
                // table and constants reach drawMeshDepthPrepass as before.
                prepassMaterials = &voxiRenderer_.materials();
#endif
                // Written by nothing, deliberately: DrawWorldPass::DepthPrepass touches no counter
                // and prints no line, since those three numbers describe a frame and this is half
                // of one. drawn/culled/owner-hidden are still counted by the colour walk below.
                aver::game::SceneDrawStats prepassStats;
                aver::game::drawWorld(*e.device(), viewProj_, content_, prepassStats,
                                      prepassMaterials, skinnedScene_.get(), popt);
            }
        }
#endif // AVER_MODULE_VOXI

        // CPU-bound or GPU-bound? --frame-time reports whole frames from the CPU, and GPU-wait
        // looks identical to CPU work, so this timer (and the streamer's below) answer it directly.
        // Measured on Electric Dreams: 8.2ms walk + 0.7ms streaming inside a 76ms frame -- GPU-bound.
        //
        // EXCLUDES the depth-prepass drawWorld call above (gated on --depth-prepass, default off),
        // which is a second full per-entity walk over the same scene. So with that flag on, this
        // bracket and the CpuTiming breakdown below measure only HALF the CPU walk time -- a number
        // that halves after adding --depth-prepass is not a speedup, it's this comment.
        const auto tWalk0 = std::chrono::steady_clock::now();

        const u32 n = w.count();

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        // ---- occlusion phase 0: collect every entity's world AABB, and split the walk order ----
        // A dedicated pre-walk, not the main loop's own per-entity computation: testBatch() is ONE
        // GPU dispatch over every candidate (a per-entity readback would be 6,370 waitIdle() calls)
        // and needs the WHOLE list, including pass-1 entities the main loop only reaches after
        // they're drawn. Degenerate-box entities are left un-culled (frustum cull's own rule).
        // pass1Count_ entities (occlusionWasVisible true, or never tested) sort first and draw
        // unconditionally; the rest gate on this frame's testBatch() result once the pyramid exists.
        u32 occlusionPass1Count = n;
        bool occlusionPyramidBuilt = false;
        // F7 (occlusion-fix-plan.md, "Link 6" -- the trigger): the scene's own sub-rect of the
        // target depth/pyramid texture, in target pixels, read fresh every frame regardless of
        // whether culling is on -- the trust gate below compares it against last frame's basis
        // (occlusionBasisRect_). False (no viewport to give) zeroes it explicitly, matching
        // ViewportRect's own w<=0/h<=0 "whole target" convention.
        f32 occRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (!e.device()->sceneViewport(occRect)) {
            occRect[0] = occRect[1] = occRect[2] = occRect[3] = 0.0f;
        }
        // ---- MOTION-SAFE TRUST GATE ----
        // testBatch()'s answer is at best one call stale (Occlusion.hpp's TWO-PASS section): the
        // pyramid and readback both describe LAST frame's camera/depth, applied to THIS frame's
        // entities. Harmless on a static camera; under motion it's the false-cull failure mode
        // where an object that entered view since the stale capture reads as hidden and pops out.
        // This measures how far the camera moved/turned since the last basis
        // (occlusionBasisCamPos_/occlusionBasisForward_, not yet overwritten for this frame), used
        // as a per-entity dilation margin below and as a global "distrust this frame" gate when
        // motion is too large to cover.
        //
        // A RETROSPECTIVE PREDICTOR, not a proof of the coming frame: it bounds motion that already
        // happened, as an estimate of what the readback needs to be trusted against when it's
        // actually consumed later this frame. Good under smooth motion; a sudden acceleration
        // between two individually-under-threshold frames could under-cover. Only the global
        // "not trustworthy" fallback below (and readbackLagIsExactlyOneCall()) actually guarantees
        // no false cull -- the per-entity margin doesn't by itself prove safety.
        const f32 occlusionMoveDist = occlusionBasisValid_
            ? (camPos_ - occlusionBasisCamPos_).size() : 1e30f;
        const f32 occlusionRotRad = occlusionBasisValid_
            ? std::acos(std::clamp(dot(camForward(), occlusionBasisForward_), -1.0f, 1.0f))
            : 3.2f;   // > pi: forces "untrustworthy" before the first basis has ever landed
        // 45 degrees: keeps the tan() below from blowing up before the global fallback
        // (kOcclusionTeleportRotRad) takes over.
        constexpr f32 kOcclusionRotClampRad = 0.785f;
        // Caps the rotation term so a distant occluder doesn't dilate unbounded. KNOWN SMALL
        // RESIDUAL: an extremely distant, fast-swept occluder edge beyond this cap could still pop
        // for one frame even when trustworthy -- the global fallback below only protects against
        // overall camera motion, not this distance/cap interaction, so this is not a claim of zero
        // popping in every configuration.
        constexpr f32 kOcclusionMaxRotMarginCm = 2000.0f;
        // Culling runs WHILE the camera moves -- gating on "camera is still" was measured worse
        // than useless (culling then never runs when it would help, and the artefact returns the
        // instant you move). The motion thresholds above no longer VETO culling; they only decide
        // whether the motion was too violent for even the dilated boxes to be trusted. Safety
        // instead comes from the per-entity dilation below: every box grows by the camera's travel
        // since the pyramid was built plus a distance-scaled rotation margin, so an object that
        // could have entered view during the one frame of lag stays inside its own dilated box.
        //
        // The thresholds below survive as an OUTER bound for genuinely discontinuous motion (a
        // teleport, --warp, a level load) where no finite margin is correct at all.
        constexpr f32 kOcclusionTeleportMoveCm = 3000.0f;
        constexpr f32 kOcclusionTeleportRotRad = 1.571f;   // 90 degrees in one frame
        // F7: a dock-layout drag between the pyramid this basis describes and the (stale) readback
        // being consumed is a discontinuity no motion margin covers -- an exact rect mismatch forces
        // the same "everyone visible" fallback a teleport does.
        const bool occlusionRectUnchanged =
            occRect[0] == occlusionBasisRect_[0] && occRect[1] == occlusionBasisRect_[1] &&
            occRect[2] == occlusionBasisRect_[2] && occRect[3] == occlusionBasisRect_[3];
        const bool occlusionTrustworthy = occlusionBasisValid_ &&
            occlusionMoveDist <= kOcclusionTeleportMoveCm &&
            occlusionRotRad <= kOcclusionTeleportRotRad &&
            occlusionRectUnchanged;
        const f32 occlusionRotMarginTan = std::tan(std::min(occlusionRotRad, kOcclusionRotClampRad));
        // F8 (occlusion-fix-plan.md): should the test even run this frame. sceneSuppressed() is
        // true whenever a registered feature (ray-driven Voxi, Path Tracing, GI debug raymarch) is
        // painting the scene itself, where culling saves almost no work (a culled entity still
        // submits for primary rays) -- idles unless occlusion.cullUnderSuppression overrides it.
        // Runs after beginFrame, so this is THIS frame's own election result, not a stale one.
        const bool occlusionRuns = aver::game::occlusionTestShouldRun(
            occlusionCullEnabled_, occluder_ != nullptr, e.device()->sceneSuppressed(),
            occlusionCullUnderSuppression_);
        // Once per transition, not every idle frame -- ray-driven is this project's standing
        // default, so a per-frame line would flood the log. Resets when occlusionRuns next goes
        // true, so idle -> running -> idle logs a second time rather than once per process.
        if (occlusionCullEnabled_ && occluder_ && !occlusionRuns) {
            if (!occlusionIdleLogged_) {
                occlusionIdleLogged_ = true;
                AVER_INFO("[Occlusion] idle: '{}' paints the scene, so culling can save no raster "
                          "work; RENDER.OCCLUSIONCULL is unchanged.",
                          occlusionSuppressingFeatureName());
            }
        } else {
            occlusionIdleLogged_ = false;
        }
        if (occlusionRuns) {
            occlusionBoxes_.clear();
            occlusionBoxEntities_.clear();
            occlusionOrder_.resize(n);
            u32 head = 0, tail = n;   // pass-1 fills from the front, pass-2 from the back
            for (u32 k = 0; k < n; ++k) {
                const scene::Entity e2 = w.at(k);
                (occlusionWasVisible(e2) ? occlusionOrder_[head++] : occlusionOrder_[--tail]) = k;

                if (w.destroyPending(e2)) continue;
                const scene::CMeshRenderer* mr2 =
                    w.component<scene::CMeshRenderer>(e2, scene::kComponentMeshRenderer);
                if (!mr2 || !(mr2->flags & scene::kMeshRendererVisible) || mr2->mesh == 0) continue;
                // Same static-mesh-bounds override the main loop below applies -- must run here
                // too, or this pre-walk's box disagrees with the main loop's the first frame an
                // entity is visited, before either pass has corrected bounds.
                const bool skinned2 = skinnedScene_ && skinnedScene_->drawHandle(e2) != 0;
                if (!skinned2)
                    if (const auto* bit2 = content_.boundsFor(mr2->mesh)) {
                        auto* mw2 = const_cast<scene::CMeshRenderer*>(mr2);
                        mw2->aabbMin[0] = bit2->first.x;  mw2->aabbMin[1] = bit2->first.y;
                        mw2->aabbMin[2] = bit2->first.z;
                        mw2->aabbMax[0] = bit2->second.x; mw2->aabbMax[1] = bit2->second.y;
                        mw2->aabbMax[2] = bit2->second.z;
                    }
                const Vec3 lo2{mr2->aabbMin[0], mr2->aabbMin[1], mr2->aabbMin[2]};
                const Vec3 hi2{mr2->aabbMax[0], mr2->aabbMax[1], mr2->aabbMax[2]};
                if (!(hi2.x > lo2.x && hi2.y > lo2.y && hi2.z > lo2.z)) continue;   // degenerate
                const Mat4& wm2 = w.worldMatrix(e2);
                Vec3 wlo2{1e30f, 1e30f, 1e30f}, whi2{-1e30f, -1e30f, -1e30f};
                for (u32 c = 0; c < 8; ++c) {
                    const Vec3 p{(c & 1) ? hi2.x : lo2.x, (c & 2) ? hi2.y : lo2.y, (c & 4) ? hi2.z : lo2.z};
                    const Vec3 t = xformPoint(wm2, p);
                    wlo2.x = std::fmin(wlo2.x, t.x); whi2.x = std::fmax(whi2.x, t.x);
                    wlo2.y = std::fmin(wlo2.y, t.y); whi2.y = std::fmax(whi2.y, t.y);
                    wlo2.z = std::fmin(wlo2.z, t.z); whi2.z = std::fmax(whi2.z, t.z);
                }
                aver::occlusion::Aabb box;
                if (occlusionTrustworthy) {
                    // Grown by the camera's translation since the last-frame basis this readback
                    // will at best reflect, plus a distance-scaled rotation term (small-angle arc
                    // length ~= distance * tan(angle)) bounding how far that rotation could have
                    // swept this box's occlusion boundary. A LARGER box only makes
                    // conservativelyHidden() (OcclusionMath.hpp) harder to satisfy, never easier --
                    // selectConservativeMip only gets coarser as a box grows, per that header's own
                    // invariant comment -- so this can only remove an existing false cull, never
                    // introduce one -- but it does not bound motion happening between now and when
                    // this readback is actually consumed.
                    const Vec3 boxCentre{(wlo2.x + whi2.x) * 0.5f, (wlo2.y + whi2.y) * 0.5f, (wlo2.z + whi2.z) * 0.5f};
                    const f32 dist = (boxCentre - camPos_).size();
                    // TWO INTERVALS, NOT ONE -- covering only one is why culling still popped
                    // while moving after the margin was first added. occlusionMoveDist measures
                    // motion already elapsed, but the verdict computed here is applied on the NEXT
                    // frame, after the camera moves again by roughly the same amount -- a margin
                    // sized for the elapsed interval alone is short by exactly the interval that
                    // matters. Doubling assumes constant velocity (the next interval mirrors the
                    // last); approximate under acceleration, deliberately, since the alternative is
                    // a velocity estimator with its own lag, and the teleport bound above catches
                    // genuinely unbounded motion. Dilating too far only costs culling, never
                    // correctness -- that's the direction to err in.
                    constexpr f32 kOcclusionMotionLookahead = 2.0f;
                    const f32 margin = kOcclusionMotionLookahead *
                        (occlusionMoveDist +
                         std::min(dist * occlusionRotMarginTan, kOcclusionMaxRotMarginCm));
                    box.min[0] = wlo2.x - margin; box.min[1] = wlo2.y - margin; box.min[2] = wlo2.z - margin;
                    box.max[0] = whi2.x + margin; box.max[1] = whi2.y + margin; box.max[2] = whi2.z + margin;
                } else {
                    // Motion since the last basis (missing basis, or a readback the staleness
                    // detector couldn't vouch for) is past what a margin can safely cover. The
                    // exact box here doesn't matter: occlusionTrustworthy false forces this WHOLE
                    // frame's answer to "visible" regardless of what testBatch() says.
                    box.min[0] = wlo2.x; box.min[1] = wlo2.y; box.min[2] = wlo2.z;
                    box.max[0] = whi2.x; box.max[1] = whi2.y; box.max[2] = whi2.z;
                }
                occlusionBoxes_.push_back(box);
                occlusionBoxEntities_.push_back(e2);
            }
            // `head` is exactly the pass-1 count: head counts up from the front, tail down from
            // the back, so head == tail once every index has landed on one side or the other.
            occlusionPass1Count = head;
            if (rhi::IResourceFactory* occRes = e.device()->resources()) {
                rhi::TextureDesc sceneDesc;
                // sceneDepthTexture() supplies the size itself: sizing the pyramid off anything
                // else risks disagreeing with the actual depth buffer the seed pass reads.
                if (const rhi::TextureHandle depthTex = e.device()->sceneDepthTexture();
                    depthTex && occRes->textureInfo(depthTex, sceneDesc)) {
                    occluder_->ensureSized(*occRes, sceneDesc.width, sceneDesc.height, e.device()->sampleCount());
                    // F7's once-per-change diagnostic: fires when the rect or pyramid actually
                    // changed since it last printed, so a dock-layout drag leaves a trail instead
                    // of a silent change in which entities get culled.
                    if (occRect[0] != occlusionLoggedRect_[0] || occRect[1] != occlusionLoggedRect_[1] ||
                        occRect[2] != occlusionLoggedRect_[2] || occRect[3] != occlusionLoggedRect_[3] ||
                        sceneDesc.width != occlusionLoggedPyramidW_ ||
                        sceneDesc.height != occlusionLoggedPyramidH_) {
                        occlusionLoggedRect_[0] = occRect[0]; occlusionLoggedRect_[1] = occRect[1];
                        occlusionLoggedRect_[2] = occRect[2]; occlusionLoggedRect_[3] = occRect[3];
                        occlusionLoggedPyramidW_ = sceneDesc.width;
                        occlusionLoggedPyramidH_ = sceneDesc.height;
                        AVER_INFO("[Occlusion] testing against scene rect {:.0f},{:.0f} {:.0f}x{:.0f} "
                                  "of a {}x{} pyramid",
                                  occRect[0], occRect[1], occRect[2], occRect[3],
                                  sceneDesc.width, sceneDesc.height);
                    }
                }
            }
        } else {
            // F8: re-entry starts from "never tested = visible" (occlusionWasVisible's own
            // comment), the same safe default a fresh entity gets, so a wall that moved in front of
            // something while culling was idle is discovered fresh on resume, and the next frame
            // doesn't reuse a stale motion basis.
            occlusionVisible_.clear();
            occlusionOrder_.clear();   // empty means "no reordering" -- see the loop below
            occlusionBasisValid_ = false;
        }

        // Runs buildPyramid()+testBatch() ONCE this frame -- either at the pass-1/pass-2 boundary
        // inside the main loop below (the common case), or here as a fallback when pass-2 is empty:
        // occlusionVisible_ still has to be refreshed for every entity or the split never budges
        // from "everyone is pass-1" and this frame's demotions would never be discovered.
        const auto occlusionBuildAndTest = [&]() {
            if (occlusionPyramidBuilt) return;
            occlusionPyramidBuilt = true;
            if (occlusionBoxes_.empty()) return;
            // NO EARLY-OUT HERE: an earlier version skipped this whenever the camera had moved,
            // which froze the basis stash below at the last still pose -- occlusionMoveDist then
            // measured against an ever-older reference, stayed above threshold forever, and
            // culling never switched back on even after the camera stopped ("culling does nothing").
            rhi::IRenderContext* pctx = e.device()->renderContext();
            rhi::IResourceFactory* occRes = e.device()->resources();
            const rhi::TextureHandle depthTex = e.device()->sceneDepthTexture();
            if (!pctx || !occRes || !depthTex) return;
            occluder_->buildPyramid(*pctx, depthTex, &viewProj_.m[0][0]);
            // Stashes the camera basis this call's readback will (at best, one call from now)
            // reflect. camForward() is the same vector that built viewProj_ this frame (the
            // unconditional camForward()/setCamera call earlier in this function, before free-fly
            // or possessed-pawn camera handling diverges), so it's authoritative regardless of
            // camera mode. Deliberately AFTER buildPyramid() (so a bailed-out frame doesn't stash a
            // basis for a pyramid never built) and BEFORE testBatch() (order doesn't matter there,
            // but keeps the stash beside the call it documents).
            occlusionBasisCamPos_ = camPos_;
            occlusionBasisForward_ = camForward();
            // F7: stashed alongside the camera basis, same idiom -- see occlusionBasisRect_'s own
            // member comment.
            occlusionBasisRect_[0] = occRect[0]; occlusionBasisRect_[1] = occRect[1];
            occlusionBasisRect_[2] = occRect[2]; occlusionBasisRect_[3] = occRect[3];
            occlusionBasisValid_ = true;
            // IDENTITY, NOT GEOMETRY: hashed from occlusionBoxEntities_ (which entities, which
            // order), not occlusionBoxes_'s bounds -- every box was just dilated by a margin that
            // changes nearly every frame for the SAME population, so hashing bounds cannot tell
            // "population changed" from "my own safety margin changed" (see hashIdentityKey()'s
            // own comment for the 244-of-255-frames measurement that caught this, and testBatch()'s
            // doc comment in Occlusion.hpp for why the module takes this as a parameter rather than
            // deriving it from the upload bytes itself, as it used to).
            const u64 occlusionIdentityKey = aver::occlusion::hashIdentityKey(
                occlusionBoxEntities_.data(), static_cast<u32>(occlusionBoxEntities_.size()));
            // F7: `occRect` is this frame's own scene sub-rect, never folded into
            // occlusionIdentityKey above, which stays keyed on entity identity alone (0f85785a/
            // e04efdab's own rule, unchanged).
            occluder_->testBatch(*pctx, *occRes, occlusionBoxes_.data(),
                                 static_cast<u32>(occlusionBoxes_.size()), occlusionIdentityKey,
                                 occRect, occlusionResults_);
            u32 c = 0, t = 0;
            occluder_->lastTestCounts(c, t);
            occlusionCulledAccum_ += c;
            occlusionTestedAccum_ += t;
            ++occlusionReportFrames_;
            // Trust the raw per-box answer only when BOTH (a) camera motion stayed under the
            // dilation bound (occlusionTrustworthy) AND (b) the readback landed exactly one call
            // behind, not more (readbackLagIsExactlyOneCall(), OcclusionCuller.cpp's
            // generation-stamp check). Failing (b) means the "one call stale" assumption the whole
            // margin rests on was wrong this frame -- worth knowing about, not just silently
            // correcting for, so it forces the same "everyone visible" fallback as failing (a).
            const bool occlusionReadbackLagExpected = occluder_->readbackLagIsExactlyOneCall();
            const bool occlusionResultsTrusted = occlusionTrustworthy && occlusionReadbackLagExpected;
            if (!occlusionReadbackLagExpected) {
                ++occlusionStaleReadbacks_;
                if (!occlusionStaleWarnedOnce_) {
                    occlusionStaleWarnedOnce_ = true;
                    AVER_WARN("[Occlusion] a readback landed more than one call stale (see "
                              "OcclusionCuller.cpp's generation-stamp check) -- culling is "
                              "disabled for every frame this keeps happening on, not merely made "
                              "more conservative; see the periodic [Occlusion] report line for "
                              "how often");
                }
            }
            for (usize bi = 0; bi < occlusionBoxEntities_.size(); ++bi)
                occlusionVisible_[occlusionBoxEntities_[bi]] =
                    !occlusionResultsTrusted || (occlusionResults_[bi] != 0);
        };
#endif

        // PROSE, NOT CODE: the constant this introduces lives in the decide() sink below, beside
        // its only test, so no longer behind #if AVER_MODULE_VOXI -- which would hide this account
        // from the build that cannot make the decision.
        //
        // A CASTER THE CAMERA CANNOT SEE STILL CASTS A SHADOW. The frustum/occlusion/owner-hide
        // verdicts below used to skip an entity via a bare `continue` past drawMesh() -- but
        // drawMesh() is the ONLY thing that reaches the render features (submitDraw() is how
        // VoxiRenderer learns an entity exists), so a culled entity vanished from shadow cascades,
        // GI voxelisation and the RT TLAS the instant it left the view ("shadows are screen-space",
        // even though no technique here is). F4 closes this by submitting the entity to the
        // features WITHOUT drawing it, through the same per-draw emitter the visible route uses
        // (game::drawWorld) -- Voxi applies its own per-cascade cull in LIGHT space instead. The
        // old submitShadowOnly lambda is gone: it read the wrong material and dropped multi-part splits.
        //
        // BOUNDED BY ANGULAR SIZE: submitting every culled entity measured +64ms/frame in
        // ElectricDreams (5,884 of 6,617 culled, mostly scatter plants; RT TLAS rebuild went from
        // 9.1ms to 64.2ms at 759->6,571 instances). The floor is Voxi's own per-cascade argument --
        // an object too small to fill a shadow texel can't cast a visible shadow -- applied earlier
        // so it stops the work, not just the draw. Negative radius means no bounds, so the entity
        // submits regardless (frustum cull's "must not vanish" rule). Scale: a crate at 5m subtends
        // ~0.2 rad and is kept; an ankle-height plant at 100m subtends ~0.003 rad and is not. The
        // threshold constant itself lives in the decide() sink below, beside its only test.
        //
        // THIS FLOOR IS ASYMMETRIC, and that asymmetry is the bug: it exists on the direct route
        // only, where an entity inside the frustum reaches drawMesh() with no size test, and only
        // gets dropped here once it rotates outside. So an object near ~1.1 degrees leaves the GI
        // draw list entirely on that frame, changing giDrawsKey and rejecting the rebuild gate --
        // panning past a field of small props re-bakes the whole volume once per prop.
        //
        // GATED ON WHETHER ANYONE BUT THE SHADOW CARES: Voxi's submission also feeds voxelisation
        // and the TLAS, which have no business being decided by a caster-size heuristic (a prop too
        // small to shadow still occludes a ray and bounces light) -- skipped whenever Voxi is
        // attached. Applied once per entity, not per part, at the direct-route branch below.

        // ONE GPU SPAN AROUND THE WHOLE OPAQUE WALK -- the raster path's counterpart to "Voxi
        // ray-driven primary": without it, raster-vs-ray-driven ratios compared a named ray span
        // against `scene draw`'s leftover exclusive time, not the same category of number. One
        // bracket, not one per draw, same reason as the depth prepass above (span cap is 64/frame).
        // The occlusion culler's "HZB build"/"HZB test" spans nest INSIDE this one deliberately, so
        // this span's exclusive time is the shading itself. `optional`, not the prepass's `if
        // (ctx)` shape, since renderContext() legitimately returns nullptr (Null backend) and this
        // loop must still run. Closed explicitly after the loop, not at end of scope.
        std::optional<rhi::ScopedGpuStat> rasterScope;
        if (rhi::IRenderContext* rctx = e.device()->renderContext())
            rasterScope.emplace(*rctx, "raster scene draws");

        // ---- the colour walk: THE SAME game::drawWorld, this time in DrawWorldPass::Colour ----
        // This used to be the hand-written original both of this frame's walks were copied from
        // (entity iteration, visible/mesh guards, asset-bounds write-back, frustum cull, owner-hide
        // walk, material resolution, planEntityDraws split, raster/direct routing) -- all of that
        // is in Runtime/src/GameRender.cpp now. A second spelling here would just be two walks that
        // agree until they don't: the prepass copy's world-box scope had already drifted enough to
        // pick a different LOD level for the same instance (2.81% of pixels, see the prepass's own comment).
        //
        // NOTHING EDITOR-ONLY MOVED, AND NONE OF IT MAY: Aver.Occlusion links into Sandbox alone and
        // this library has no path tracer, so hierarchical-Z, Trifactor LOD, GPU cluster dispatch,
        // the selection outline, the PlayerStart icon and the path-traced scene view all stay here,
        // reaching the walk through DrawWorldOptions' hooks -- each a point where the editor answers
        // a question the library can't ask itself, defaulting to what the walk already decided.
        //
        // visitOrder EXISTS FOR THE OCCLUSION ORDER: occlusionOrder_ puts everything seen last
        // frame first so buildPyramid() has depth to build from, and the pass-1/pass-2 boundary is a
        // raw index into that sequence -- fired from onVisit because it's a statement about the
        // command stream, not something that can be hoisted earlier.

        // The frame state the hooks need, as a local struct rather than SandboxApp members: the
        // sinks must be CAPTURELESS lambdas (plain function pointers, not std::function, for a
        // per-entity path over thousands of entities/frame), so everything they read arrives
        // through the one `user` pointer. All of it is this frame's alone -- the counters start at
        // zero and the occlusion split is recomputed every frame -- so it belongs on the stack, not
        // on the app, where a stale value could outlive the walk that wrote it. A lambda inside a
        // member function is a local class of it, so `self` reaches SandboxApp's private members
        // without a SandboxApp.hpp declaration or rebuilding every TU that includes it.
        struct ColourWalk {
            SandboxApp* self = nullptr;
            Engine* engine = nullptr;
            // The occlusion pass-1/pass-2 split, read by onVisit alone. Left false/0 with the
            // module compiled out, where onVisit is not installed either.
            bool occlusionRuns = false;
            u32 pass1Count = 0;
            // occlusionBuildAndTest is a by-reference closure on this frame's stack; a function
            // pointer can't carry one, so it's reached via a captureless trampoline (filled in
            // below, beside the lambda it points at).
            void (*buildPyramidNow)(void*) = nullptr;
            void* buildPyramidUser = nullptr;
            // 3B: how many Draw records the direct route delivered for this frame's culled
            // entities, and how many were multi-part -- evidence a culled tree still yields N draws
            // instead of collapsing to slot 0's. Counted through the hooks: onDirectDraw tallies
            // this entity's draws, onEntityDelivered banks them once the walk delivers the entity.
            u32 culledDraws = 0;
            u32 culledMultiPart = 0;
            u32 entityDirectDraws = 0;
            bool entityCulled = false;
            // CPU time inside dispatchMeshClusters, for the scene-walk report below. Sourced from
            // decide()'s own CpuSpan::ClusterDispatch CpuNest (not a second manual accumulator that
            // could disagree with it), filled once per throttled print -- see the scene-walk log block
            // below -- so it stays at its constructed 0.0 on any frame that log block does not run.
            f64 dispatchMs = 0.0;
            // Entity-level material tokens colourDecide has already resolved THIS walk; see
            // EntityTokenMemo and the resolve in colourDecide.
            EntityTokenMemo entityTokens;
            // Entities colourSkipped saw for the first time this walk that carry a level placement
            // record (authored hidden, or hidden in the editor) and so are skipped by design; told
            // once, as a single line, after the walk -- see colourSkipped.
            u32 authoredHiddenNew = 0;
            // The first few of them, named in that line: a script that zero-fills the renderer of an
            // entity that happens to carry a record lands in this count too, and an id is what lets
            // that case still be found.
            u64 authoredHiddenIds[3] = {};
        } walk;
        walk.self = this;
        walk.engine = &e;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        walk.occlusionRuns = occlusionRuns;
        walk.pass1Count = occlusionPass1Count;
        using OcclusionBuildFn = decltype(occlusionBuildAndTest);
        walk.buildPyramidUser = const_cast<void*>(static_cast<const void*>(&occlusionBuildAndTest));
        walk.buildPyramidNow = [](void* p) { (*static_cast<const OcclusionBuildFn*>(p))(); };

        // THE ONE ORDERING REQUIREMENT IN THE HOOK SET, and why DrawWorldVisitFn returns nothing:
        // buildPyramid() reads depth pass one's draws just wrote, so it must be recorded at the
        // pass-1/pass-2 boundary in the command stream -- a pre-pass computing the same verdicts
        // could still be wrong, since the pyramid would be built against a different set of draws.
        auto colourVisit = [](u32 visitIndex, scene::Entity ent, void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            (void)ent;
            if (c.occlusionRuns && visitIndex == c.pass1Count && c.buildPyramidNow)
                c.buildPyramidNow(c.buildPyramidUser);
        };
#endif

        // decide() runs after the walk computes this entity's world box and both verdicts, before
        // it commits to a route -- one in/out struct (EntityDecision), not narrower callbacks,
        // because the box is an INPUT: re-deriving it here is what made the prepass and colour
        // copies choose different LOD levels before.
        auto colourDecide = [](aver::game::EntityDecision& d, void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            SandboxApp& self = *c.self;
            // Reset HERE rather than in onVisit: onVisit is only installed when the occlusion
            // module is in, and these two belong to the direct-route accounting, which is Voxi's.
            // Nothing can fire onDirectDraw or onEntityDelivered for this entity before this line
            // runs, so this is the last point that is still early enough.
            c.entityDirectDraws = 0;
            c.entityCulled = false;

            // ---- THE PLAYER START IS CHROME, AND IT DRAWS AS AN ICON INSTEAD ----
            // Skipping it by identity drops it from opaque, shadow, GI and RT all at once -- right
            // for a marker that should never cast a shadow, bounce light or appear in a reflection.
            // NOT by clearing kMeshRendererVisible: ray picking asks the identical question, so
            // that would also block clicking the marker. The icon is queued further down, sharing
            // the selection outline's is-anything-playing test.
            //
            // IT IS LATE NOW, a real change: this walk refreshes the marker's aabb from
            // GameContent::boundsFor and warns via onSkipped if its mesh id ever fails to resolve
            // (decide() runs after both, and EntityDecision::skip can't undo what already ran) --
            // already true of the occlusion pre-walk and the depth-prepass call too.
            if (d.entity == self.playerStart_ && self.viewportIconsReady_) { d.skip = true; return; }

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            // PASS-2 ONLY: pass-1 entities (visitIndex < pass1Count) drew unconditionally before
            // occlusionBuildAndTest() ran. occlusionWasVisible(ent) is NOT a fresh answer for this
            // frame -- testBatch()'s readback is at best one call stale, so occlusionVisible_[ent]
            // only holds the raw motion-dilated result when this frame's camera motion AND the
            // readback's own staleness both stayed inside the trust gate above; otherwise it's
            // forced to "visible" regardless of what the pyramid says. A degenerate box (excluded
            // from occlusionBoxes_) was never tested and defaults to visible, same as haveWorldBox
            // false here. `visitIndex` is the position in occlusionOrder_, the sequence the pass
            // boundary was cut in -- DrawWorldOptions hands the hooks that index for exactly this
            // reason.
            d.occlusionCulled = c.occlusionRuns && d.visitIndex >= c.pass1Count && d.haveWorldBox &&
                                !self.occlusionWasVisible(d.entity);
#endif
            // 3B's by-hand false-cull finder: an otherwise-culled, non-owner-hidden entity draws
            // through the RASTER route anyway, tinted -- chooseRoute owns that pairing, so this is
            // fed to it rather than applied here. Guarded (unlike occlusionCullEnabled_, a manifest
            // key) because with no culler compiled in, d.occlusionCulled is always false, so the
            // route is identical either way regardless of this tint.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            d.tint = self.occlusionShowCulled_;
#endif

            // Asked here too, not restated: drawWorld calls the identical chooseRoute() on the
            // identical inputs a few lines after this returns, so the two can't disagree about a
            // pure function. The editor needs the answer now because everything below sits on one
            // side of this `if (!route.raster)`: the angular-size floor, Trifactor LOD, cluster dispatch.
            const aver::game::RouteDecision route = aver::game::chooseRoute(
                d.frustumCulled, d.occlusionCulled, d.ownerHidden, d.tint);

            if (!route.raster) {
                // THE UNIFIED DIRECT ROUTE: frustum-culled, occlusion-culled or owner-hidden.
                // Still submitted to Voxi so shadows/GI/TLAS never depend on what the camera can
                // see (see "A CASTER THE CAMERA CANNOT SEE STILL CASTS A SHADOW" above); the
                // submission is the library's, what's left here is the two answers it can't give.
                //
                // KNOWN RESIDUAL, STATED RATHER THAN FIXED (F4): this route never runs LOD/cluster
                // selection (skinned/soft-body aside), so it always plans the base mesh's full part
                // split -- a visible entity whose LOD/cluster cut substitutes a different handle
                // delivers ONE draw while the same entity, culled, delivers its base parts. Only
                // live with LOD selection or the CLI cluster paths (LODSELECT is 0 in PTTest, so
                // dormant here).
                c.entityCulled = d.frustumCulled || d.occlusionCulled;
#if AVER_MODULE_VOXI
                // THE SOFT-BODY SEAM: posedHandle() asks skinning first, then soft-body (skinning
                // first makes CSoftBody-on-a-skinned-mesh deterministic). chosenMesh arrives holding
                // only skinning's answer -- the library has no soft-body scene to ask -- so filling
                // it in is the host's job.
                const rhi::MeshHandle posed = self.posedHandle(d.entity);
                d.chosenMesh = posed ? posed : d.baseMesh;

                const Vec3 wlo = d.worldBoxMin, whi = d.worldBoxMax;
                const Vec3 cullCentre = d.haveWorldBox
                    ? Vec3{(wlo.x + whi.x) * 0.5f, (wlo.y + whi.y) * 0.5f, (wlo.z + whi.z) * 0.5f}
                    : Vec3{0.0f, 0.0f, 0.0f};
                const f32 cullRadius = d.haveWorldBox
                    ? 0.5f * std::sqrt((whi.x - wlo.x) * (whi.x - wlo.x) +
                                        (whi.y - wlo.y) * (whi.y - wlo.y) +
                                        (whi.z - wlo.z) * (whi.z - wlo.z))
                    : -1.0f;
                // The angular-size floor -- see "GATED ON WHETHER ANYONE BUT THE SHADOW CARES"
                // above this walk. Applied once per entity, matching its old home.
                //
                // emitDirectDraws, NOT skip: `skip` counts the entity nowhere; this drops its Voxi
                // submission while it still counts as culled -- matching the deleted branch, whose
                // `if (angularFloorOk)` wrapped only the plan-and-emit, never the `++culled` after
                // it. Getting this the other way round is the 9.1 -> 64.2 ms/frame that motivated
                // the floor in the first place.
                constexpr f32 kMinCasterAngle = 0.02f;   // radians (~1.1 degrees)
                if (!self.voxiAttached_ && cullRadius >= 0.0f) {
                    const f32 dx = cullCentre.x - self.camPos_.x, dy = cullCentre.y - self.camPos_.y,
                              dz = cullCentre.z - self.camPos_.z;
                    const f32 dsq = dx * dx + dy * dy + dz * dz;
                    if (dsq > cullRadius * cullRadius) {
                        const f32 dd = std::sqrt(dsq);
                        if (2.0f * cullRadius / dd < kMinCasterAngle) d.emitDirectDraws = false;
                    }
                }
#endif
                return;
            }

            const Mat4& wm = *d.world;
            const Vec3 wlo = d.worldBoxMin, whi = d.worldBoxMax;

            // THE ENTITY-LEVEL RESOLVE, NOT THE DRAWS': each planned draw's own token is resolved
            // by the library separately (resolveSurfaceLook). This survives for two readers the
            // walk can't serve: the GPU cluster path below binds col/metallic/roughness/matSet/
            // matConstants through the CONTEXT (setDrawBinding only forwards from inside drawMesh(),
            // which that path skips), and resolveSurface()'s once-per-token warning sets still need
            // filling for the ENTITY's token, which a multi-part mesh resolves nowhere else.
            //
            // THE RESULT IS READ BY THE GPU CLUSTER PATH ALONE -- col/metallic/roughness/blended
            // below and rsEntity.matSet/matConstants/matBytes inside its dispatch, all behind
            // lodMeshShaderEnabled_ -- so only with that path on does every entity need its own
            // answer. Without it the only reason left is the warning/lazy-entry side effects, and
            // those are per TOKEN: resolveSurface's warning sets are function-local statics, and
            // the MaterialSystem entry it builds on first ask stays built. So the resolve runs once
            // per token per walk instead of once per rasterised entity (roughly seven hash lookups
            // each), a token first met this walk still gets exactly the call it always got, and an
            // unread `rsEntity` stays the default look.
#if AVER_MODULE_TRIFACTOR
            const bool entityLookRead = self.lodMeshShaderEnabled_;
#else
            const bool entityLookRead = false;
#endif
            SandboxApp::ResolvedSurface rsEntity;
            if (entityLookRead || c.entityTokens.first(d.material))
                rsEntity = self.resolveSurface(d.material);

            // A posed entity's vertices live in a different MeshHandle sharing this one's index
            // buffer, so substituting the handle reaches every pass at once. Zero = "not posed",
            // never "not drawn".
            rhi::MeshHandle mesh = d.baseMesh;
            // Set true only by the GPU per-cluster path below when it actually dispatches this
            // instance's geometry -- declared unconditionally so colourAlreadyDrawn can be answered
            // regardless of the module.
            bool clusterDispatched = false;
#if AVER_MODULE_TRIFACTOR
            f32 col[4] = {rsEntity.look.col[0], rsEntity.look.col[1], rsEntity.look.col[2],
                          rsEntity.look.col[3]};
            f32 metallic = rsEntity.look.metallic, roughness = rsEntity.look.roughness;
            const bool blended = rsEntity.look.blended;
            // Paints the scene test's two entities so a probe can tell which it's looking at, only
            // behind --skin-scene-test. Only ever reached the cluster dispatch (the per-draw
            // emitter and resolveDrawLook re-derive `col` and never see this override), so this is
            // not the scene-wide recolour it might look like.
            if (self.skinScene_) {
                if (d.entity == static_cast<scene::Entity>(self.skinScene_->subjectEntity()))
                    aver::editor::SkinSceneTest::subjectColor(col);
                else if (d.entity == static_cast<scene::Entity>(self.skinScene_->referenceEntity()))
                    aver::editor::SkinSceneTest::referenceColor(col);
            }

            // Virtualized-geometry LOD selection (trifactor::ClusterAdapt/ClusterSelect), per-level
            // not per-cluster. Skipped for a skinned entity and a mesh with no LOD ladder. NO
            // CACHE: chooseLevelCached is cheap enough to run fresh every frame.
            // GPU per-cluster path (--lod-mesh-shader) wins over everything below when the mesh has
            // GPU cluster buffers and the pipeline is up: it dispatches the geometry itself, so
            // `mesh` is never substituted and drawMesh() is skipped entirely; falls through to the
            // CPU paths otherwise (see ensureLodMeshPipeline's degrade comment).
            // ALSO EXCLUDES a BLENDED instance: drawMesh() is where the opaque/blended contract
            // lives (sort, wait for sky, depth-write off), and a cluster-dispatched glass pane
            // would skip all of that straight into voxiRenderer_.submit() unconditionally --
            // breaking the shadow/GI/TLAS exclusion `blended` exists to enforce. A blended instance
            // still reaches the ordinary drawMesh() call via the CPU per-cluster/discrete-LOD
            // paths, or the unmodified mesh -- the only place that draws glass correctly.
            // NOR IN WIREFRAME: this path shades straight into the scene target, and the wireframe
            // view draws only what reaches drawMesh (IDevice::setWireframe).
            if (self.lodMeshShaderEnabled_ && self.lodMeshPipelineReady_ && !d.skinned &&
                d.haveWorldBox && !blended && !self.wireframe_) {
                if (const auto git = self.meshClusterGpu_.find(d.meshId); git != self.meshClusterGpu_.end()) {
                    const auto& gpu = git->second;
                    if (rhi::IRenderContext* ctx = c.engine->device()->renderContext();
                        ctx && gpu.clusterCount) {
                        trifactor::View view;
                        view.eye = self.eye_;
                        view.viewProj = self.viewProj_;
                        view.viewportHeightPx = self.vpH_;
                        view.verticalFovRadians = radians(60.0f);
                        const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                        // ClusterFrameCB (b4): budget clamped above zero here on the CPU before
                        // upload (ASMain does not re-clamp); the six frustum planes are copied
                        // verbatim from the same Frustum::fromViewProj the CPU reference calls.
                        SandboxApp::ClusterFrameCB frameCb;
                        frameCb.budgetPx = std::max(self.lodErrorThresholdPx_, trifactor::kMinClusterBudgetPx);
                        frameCb.projScale = trifactor::projScale(view);
                        frameCb.worldScale = worldScale;
                        const trifactor::Frustum frustum = trifactor::Frustum::fromViewProj(view.viewProj);
                        static_assert(sizeof(frameCb.planes) == sizeof(frustum.plane),
                                      "ClusterFrameCB::planes must match trifactor::Frustum::plane byte for byte");
                        std::memcpy(frameCb.planes, frustum.plane, sizeof(frameCb.planes));

                        // PerObject (b1): the same 32-dword layout drawMesh() itself packs (world,
                        // base colour, metallic/roughness, frozen shading-model tail), so
                        // PSClusterMain's plainShadeSurface reads real, live values.
                        f32 consts[rhi::kObjectConstantDwords];
                        std::memcpy(consts, &wm.m[0][0], 16 * sizeof(f32));
                        std::memcpy(consts + 16, col, 4 * sizeof(f32));
                        consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
                        // A THIRD WRITER OF gShadingModel that writeShadingConstants search misses:
                        // this path builds PerObject by hand. Used to hardcode STANDARD, so a mesh
                        // drawn through the GPU cluster path ignored Unlit while every other path honoured it.
                        const u32 shadingModel = self.unlit_ ? 1u : 0u;   // AVER_MODEL_UNLIT / STANDARD
                        std::memcpy(consts + 24, &shadingModel, sizeof(shadingModel));
                        consts[25] = 0.04f; consts[26] = 1.0f; consts[27] = 0.0f;
                        consts[28] = consts[29] = consts[30] = consts[31] = 0.0f;

                        // CpuSpan::ClusterDispatch, as a CpuNest and not a second steady_clock pair:
                        // this interval sits INSIDE decide(), inside whatever CpuSpan GameRender.cpp
                        // has open there (CpuSpan::WalkDecide: "the EntityDecision fill,
                        // options.decide(), chooseRoute, planEntityDraws"). A bare steady_clock pair
                        // would land the same microseconds in both c.dispatchMs (the log line below)
                        // and WalkDecide's own bucket -- two reports of the same time under different
                        // names that could drift apart. A CpuNest instead SUSPENDS WalkDecide for
                        // this scope and resumes it when the scope ends -- see CpuNest's own comment
                        // in CpuTiming.hpp for why it's the one primitive there built for a region
                        // reachable from more than one enclosing phase. walk.dispatchMs is populated
                        // FROM this same facility further down, once per throttled print, not from a
                        // second manual accumulator that could disagree with it -- see that call site.
                        {
                            CpuNest clusterDispatchNest(CpuSpan::ClusterDispatch);
                            ctx->setPipeline(self.lodMeshPipeline_);
                            ctx->setBindingSet(gpu.bindingSet, 0);
                            // THE MATERIAL, ON THE CONTEXT -- why foliage on this path drew black.
                            // setDrawBinding above records the material on the DEVICE, which only
                            // forwards it to the context from inside drawMesh(), never called here,
                            // so table-1 binding was whatever an earlier draw left sticky (Voxi's
                            // fallback set, white metal-rough map -> metallic 1, kdAlbedo 0, no
                            // diffuse lobe) -- even though every value measured correct, just for a
                            // different material.
                            if (rsEntity.matSet)
                                ctx->setDrawBinding(rsEntity.matSet, rsEntity.matConstants, rsEntity.matBytes);
                            ctx->setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
                            ctx->setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frameCb, sizeof(frameCb));
#if AVER_MODULE_VOXI
                            // b3: VoxiFrame, the same bytes Voxi's scenePass binds at b4 for the
                            // ordinary path (this path can't reuse b4). Bound every draw, not once
                            // per mesh -- a root CBV pointer set is cheap -- keeping
                            // shadowFactor()/coneTracedIndirect() valid even before Voxi's init()
                            // (giFrameConstants() then returns an all-zero block).
                            ctx->setConstantBuffer(kClusterGiFrameRegister, self.voxiRenderer_.giFrameConstants(),
                                                   self.voxiRenderer_.giFrameConstantBytes());
#endif
                            ctx->dispatchMeshClusters(mesh, gpu.clusterCount);
                        }
                        clusterDispatched = true;

                        // DEFECT 2's FIX: this instance's lit pixels already came from
                        // dispatchMeshClusters above, so drawMesh() is skipped -- and since
                        // IRenderFeature::submitDraw is only called from D3D12Device::drawMesh,
                        // skipping it also skips submitDraw(), the ONLY way geometry reaches
                        // VoxiRenderer::draws_. Without colourAlreadyDrawn this instance never
                        // appeared in shadowPass, giShadowPass or voxelizePass: a tree that casts no
                        // shadow, not a cheaper one. Now answered via EntityDecision::colourAlreadyDrawn,
                        // which sends the entity down the same direct route culling would, with
                        // hiddenFromOwner false (deliver() keys that on route.raster, true here).

                        ++self.lodMeshShaderStats_.instancesTested;
                        self.lodMeshShaderStats_.clustersDispatched += gpu.clusterCount;

                        // Informational counters: the SAME CPU reference over the same clusters and
                        // budget the GPU dispatch just used, sampled every 64 frames.
                        // GATED ON lodClusterStatsEnabled_ (--lod-cluster-stats), off by default --
                        // even with the instance-level shortcut below, the instances it can't prove
                        // (mixed-level, closest/largest-DAG trees) cost ~1.1s alone on the sampled
                        // frame -- and since 64 divides --frames 128 evenly, that frame is
                        // guaranteed to be this task's own benchmark's last, not a coincidence. A
                        // once-per-second stall for a log line no render pass reads is unacceptable.
                        if (self.lodClusterStatsEnabled_ && self.lodClusterFrame_ % 64 == 0) {
                            if (const auto cit2 = self.meshClusterData_.find(d.meshId);
                                cit2 != self.meshClusterData_.end()) {
                                const auto& mcd = cit2->second;
                                bool sampledShortcut = false;
                                if (const auto lit2 = self.meshLods_.find(d.meshId); lit2 != self.meshLods_.end()) {
                                    const auto& ladder2 = lit2->second;
                                    const Vec3 sphereCenter2 = (wlo + whi) * 0.5f;
                                    const f32 sphereRadius2 = dist(wlo, whi) * 0.5f;
                                    const u32 candidateLevel2 = trifactor::chooseLevelCached(
                                        ladder2.errorCm, sphereCenter2, sphereRadius2,
                                        self.lodErrorThresholdPx_, view);
                                    if (candidateLevel2 < mcd.levelBounds.size() &&
                                        trifactor::provablySingleLevelCut(
                                            mcd.levelBounds, candidateLevel2, sphereCenter2, sphereRadius2,
                                            mcd.maxSphereRadius * worldScale, self.lodErrorThresholdPx_, view)) {
                                        self.lodMeshShaderStats_.survivors += mcd.levelBounds[candidateLevel2].count;
                                        self.lodMeshShaderStats_.trianglesDrawn +=
                                            mcd.levelBounds[candidateLevel2].triangleCount;
                                        self.lodMeshShaderStats_.maxDistinctLevelsSeen =
                                            std::max(self.lodMeshShaderStats_.maxDistinctLevelsSeen, 1u);
                                        ++self.lodMeshShaderStats_.instancesShortcut;
                                        sampledShortcut = true;
                                    }
                                }
                                if (!sampledShortcut) {
                                    std::vector<trifactor::MeshClusterView> worldClusters = mcd.clusters;
                                    for (trifactor::MeshClusterView& cv : worldClusters) {
                                        cv.sphereCenter = xformPoint(wm, cv.sphereCenter);
                                        cv.sphereRadius *= worldScale;
                                        cv.coneApex = xformPoint(wm, cv.coneApex);
                                        cv.coneAxis = xformVec(wm, cv.coneAxis).getSafeNormal();
                                    }
                                    const trifactor::ClusterCutResult cr = trifactor::selectClusterCut(
                                        worldClusters, self.lodErrorThresholdPx_, view, true);
                                    self.lodMeshShaderStats_.survivors += cr.stats.drawn;
                                    self.lodMeshShaderStats_.trianglesDrawn += cr.stats.trianglesAfter;
                                    if (cr.stats.distinctLevels > 1) ++self.lodMeshShaderStats_.instancesMixedLevels;
                                    self.lodMeshShaderStats_.maxDistinctLevelsSeen =
                                        std::max(self.lodMeshShaderStats_.maxDistinctLevelsSeen, cr.stats.distinctLevels);
                                }
                            }
                        }
                    }
                }
            }
            // PER-CLUSTER path wins over per-LEVEL when both are enabled: this is what actually
            // mixes LOD levels within one instance's draw; the per-level `else if` below is
            // unchanged and still reachable when --lod-per-cluster is off.
            if (!clusterDispatched && self.lodPerClusterEnabled_ && !d.skinned && d.haveWorldBox) {
                if (const auto cit = self.meshClusterData_.find(d.meshId); cit != self.meshClusterData_.end()) {
                    const auto& cd = cit->second;
                    trifactor::View view;
                    view.eye = self.eye_;
                    view.viewProj = self.viewProj_;
                    view.viewportHeightPx = self.vpH_;
                    view.verticalFovRadians = radians(60.0f);
                    const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                    // THE INSTANCE-LEVEL SHORTCUT (ClusterAdapt.hpp; TrifactorTest proves it against
                    // the real scan): if provablySingleLevelCut can prove the real O(all-DAG-
                    // clusters) scan would select exactly the whole level it names, draw that
                    // level's ladder handle directly, skipping the copy/transform/scan. Falls
                    // through to the real scan when the proof doesn't hold.
                    bool tookShortcut = false;
                    if (const auto lit = self.meshLods_.find(d.meshId); lit != self.meshLods_.end()) {
                        const auto& ladder = lit->second;
                        const Vec3 sphereCenter = (wlo + whi) * 0.5f;
                        const f32 sphereRadius = dist(wlo, whi) * 0.5f;
                        const u32 candidateLevel = trifactor::chooseLevelCached(
                            ladder.errorCm, sphereCenter, sphereRadius, self.lodErrorThresholdPx_, view);
                        if (candidateLevel < ladder.handles.size() &&
                            trifactor::provablySingleLevelCut(
                                cd.levelBounds, candidateLevel, sphereCenter, sphereRadius,
                                cd.maxSphereRadius * worldScale, self.lodErrorThresholdPx_, view)) {
                            mesh = ladder.handles[candidateLevel];
                            tookShortcut = true;
                            ++self.lodClusterStats_.instancesShortcut;
                        }
                    }

                    if (!tookShortcut) {
                    // Mesh-local -> world, once per instance per frame, for every cluster of the
                    // mesh's whole DAG: the cut test needs each cluster's world-space sphere
                    // against this frame's camera (same uniform-scale approximation as above).
                    std::vector<trifactor::MeshClusterView> worldClusters = cd.clusters;
                    for (trifactor::MeshClusterView& cv : worldClusters) {
                        cv.sphereCenter = xformPoint(wm, cv.sphereCenter);
                        cv.sphereRadius *= worldScale;
                        cv.coneApex = xformPoint(wm, cv.coneApex);
                        cv.coneAxis = xformVec(wm, cv.coneAxis).getSafeNormal();
                    }

                    const trifactor::ClusterCutResult cr = trifactor::selectClusterCut(
                        worldClusters, self.lodErrorThresholdPx_, view, true /* useFrustum */);

                    ++self.lodClusterStats_.instancesTested;
                    self.lodClusterStats_.clustersTested += cr.stats.tested;
                    self.lodClusterStats_.frustumCulled += cr.stats.frustumCulled;
                    self.lodClusterStats_.coneCulled += cr.stats.coneCulled;
                    self.lodClusterStats_.lodRejected += cr.stats.lodRejected;
                    self.lodClusterStats_.clustersDrawn += cr.stats.drawn;
                    self.lodClusterStats_.trianglesDrawn += cr.stats.trianglesAfter;
                    if (const auto tIt = self.meshTris_.find(d.meshId); tIt != self.meshTris_.end())
                        self.lodClusterStats_.trianglesBeforeLod0 += tIt->second;
                    if (cr.stats.distinctLevels > 1) ++self.lodClusterStats_.instancesMixedLevels;
                    self.lodClusterStats_.maxDistinctLevelsSeen =
                        std::max(self.lodClusterStats_.maxDistinctLevelsSeen, cr.stats.distinctLevels);

                    // Sort for a cheap, order-independent "did the cut change" comparison. The cut
                    // is expected STABLE frame to frame (camera moves continuously, not by a full
                    // LOD jump), so this is a cache hit most frames once settled.
                    std::vector<u32> selectedIds = cr.drawnIds;
                    std::sort(selectedIds.begin(), selectedIds.end());

                    auto& cache = self.clusterCutCache_[d.entity];
                    cache.lastUsedFrame = self.lodClusterFrame_;
                    if (selectedIds != cache.selectedIds || cache.handle == 0) {
                        // REBUILD: concatenate every selected cluster's precomputed expanded global
                        // index list into ONE fresh index buffer against the shared vertex array
                        // every level of this mesh uses -- one draw call for the whole mixed-LOD
                        // cut, avoiding ExecuteIndirect entirely. Measured, not guessed: real wall
                        // time and real upload byte counts.
                        std::vector<u32> assembled;
                        u64 idxCount = 0;
                        for (u32 cidx : selectedIds)
                            if (cidx < cd.clusterIndices.size()) idxCount += cd.clusterIndices[cidx].size();
                        assembled.reserve(idxCount);
                        for (u32 cidx : selectedIds)
                            if (cidx < cd.clusterIndices.size())
                                assembled.insert(assembled.end(), cd.clusterIndices[cidx].begin(),
                                                  cd.clusterIndices[cidx].end());

                        const auto t0 = std::chrono::steady_clock::now();
                        const rhi::MeshHandle newHandle = assembled.empty() ? 0 :
                            c.engine->device()->createMesh(cd.verts.data(), (u32)cd.verts.size(),
                                                    assembled.data(), (u32)assembled.size());
                        const auto t1 = std::chrono::steady_clock::now();
                        self.lodClusterStats_.rebuildMs +=
                            std::chrono::duration<f64, std::milli>(t1 - t0).count();
                        self.lodClusterStats_.rebuildIndices += assembled.size();
                        ++self.lodClusterStats_.rebuilds;

                        if (newHandle) {
                            if (cache.handle) {
                                self.depthProxy_.erase(cache.handle);
                                c.engine->device()->destroyMesh(cache.handle);
                            }
                            cache.handle = newHandle;
                            cache.selectedIds = std::move(selectedIds);
                            ++cache.rebuildCount;
                            // A DEPTH PROXY FOR THE CUT, pointing at the stable SOURCE mesh. The cut
                            // handle is born fresh under a rotating camera nearly every frame (cluster
                            // selection is frustum- and cone-culled per view, so the "stable frame to
                            // frame" comment above holds only for a still camera); without an entry
                            // here, depthProxyLookup returns 0, Voxi's submit() falls back to the cut
                            // handle, and the shadow cascades/GI shadow map/voxelisation/TLAS all
                            // follow a handle destroyed and recreated continuously -- MEASURED to churn
                            // the GI rebuild gate's draw key (the delta log named consecutive handles
                            // arriving and leaving) -- "createBlas for destroyed mesh" is where that
                            // comes from. The source mesh is the right answer for all four: none wants
                            // a view-dependent cluster subset, and voxelisation's 512^3 cells are far
                            // coarser than a cluster-level cut anyway. The cut still draws colour.
                            //
                            // FIXED (found chasing a /W4 C4244, not cosmetic): this used to read
                            // `mr->mesh`, the mesh's u64 CONTENT-HASH id (the key meshClusterData_/
                            // meshLods_ are looked up by, above) -- not a handle at all, truncated
                            // into whatever 32 bits of depthProxy_'s u32 survived. `mesh`, the
                            // pre-cut SOURCE handle, is the value that belongs here.
                            self.depthProxy_[newHandle] = mesh;
                        }
                        // newHandle == 0 (empty cut, or device refused): keep the cache's existing
                        // handle (fail-safe), or fall through to the LOD-0 handle `mesh` already
                        // holds on the very first frame.
                    } else {
                        ++self.lodClusterStats_.cacheHits;
                    }
                    if (cache.handle) mesh = cache.handle;
                    }   // !tookShortcut
                }
            } else if (!clusterDispatched && self.lodSelectEnabled_ && !d.skinned && d.haveWorldBox) {
                if (const auto lit = self.meshLods_.find(d.meshId); lit != self.meshLods_.end()) {
                    const auto& ladder = lit->second;
                    const Vec3 sphereCenter = (wlo + whi) * 0.5f;
                    const f32 sphereRadius = dist(wlo, whi) * 0.5f;   // half the box diagonal:
                                                                      // encloses the box exactly (ClusterSelect's own convention).
                    trifactor::View view;
                    view.eye = self.eye_;
                    view.viewProj = self.viewProj_;
                    view.viewportHeightPx = self.vpH_;
                    view.verticalFovRadians = radians(60.0f);   // matches this frame's proj build, above
                    const u32 level = trifactor::chooseLevelCached(
                        ladder.errorCm, sphereCenter, sphereRadius, self.lodErrorThresholdPx_, view);
                    mesh = ladder.handles[level];

                    ++self.lodStats_.instancesTested;
                    if (level > 0) ++self.lodStats_.levelCollapsed;
                    self.lodStats_.trianglesBeforeLod0 += ladder.triCounts.front();
                    self.lodStats_.trianglesAfterLevel += ladder.triCounts[level];

                    // Informational cluster-cull telemetry for the CHOSEN level only -- real and
                    // tested, but not subtracted from trianglesAfterLevel (this slice draws the
                    // whole chosen level). ladder.clusters[level] holds mesh-local bounds; a working
                    // copy is transformed by this instance's world matrix into world space, using a
                    // uniform-scale approximation that's fine for a counter never reaching the draw call.
                    if (self.lodClusterStatsEnabled_ &&
                        level < ladder.clusters.size() && !ladder.clusters[level].empty()) {
                        const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();
                        std::vector<trifactor::ClusterView> worldClusters = ladder.clusters[level];
                        for (trifactor::ClusterView& cv : worldClusters) {
                            cv.sphereCenter = xformPoint(wm, cv.sphereCenter);
                            cv.sphereRadius *= worldScale;
                            cv.coneApex = xformPoint(wm, cv.coneApex);
                            cv.coneAxis = xformVec(wm, cv.coneAxis).getSafeNormal();
                        }
                        const trifactor::SelectionResult sr = trifactor::selectVisibleClustersWithStats(
                            worldClusters, 1e30f /* no LOD collapse: already chosen */, view,
                            true /* useFrustum */);
                        self.lodStats_.clustersTested += sr.stats.tested;
                        self.lodStats_.frustumCulled += sr.stats.frustumCulled;
                        self.lodStats_.coneCulled += sr.stats.coneCulled;
                    }
                }
            }
#else
            // Only the GPU cluster path reads the entity-level look, the world matrix and the world
            // box, and that path is Trifactor's. The resolve itself still runs above, because its
            // once-per-token warning sets are not optional -- so say these go unread under /W4
            // (CMakeLists.txt:202) rather than let it report them as locals nobody wanted.
            (void)rsEntity;
            (void)wm;
            (void)wlo; (void)whi;
#endif
            // WHICH FEATURE OWNS THIS ENTITY'S VERTICES -- posedHandle(), the same helper the
            // direct route above calls, so the seam agrees with itself instead of two near-
            // identical copies (skinning checked first -- nothing forbids CSoftBody on an
            // already-skinned mesh, so checking skinning first makes the collision deterministic --
            // soft body filling in where it declined).
            //
            // NOT FOR A CLUSTER-DISPATCHED ENTITY (the old exception, now written down: the
            // hand-written code this replaced ran above this line, planned from sceneMeshHandle
            // since no LOD/skin substitution had run yet): chosenMesh is what planEntityDraws plans
            // from, so a substituted handle here would collapse a multi-part cluster mesh's per-part
            // split to one draw the moment a soft body claimed it -- planEntityDraws drops the
            // split for any handle it wasn't cut from.
            if (!clusterDispatched)
                if (const rhi::MeshHandle substituted = self.posedHandle(d.entity)) mesh = substituted;
            d.chosenMesh = mesh;
            // The GPU per-cluster path already dispatched this instance's geometry, so the raster
            // drawMesh() must not run for it -- but drawMesh() is also the only path to
            // IRenderFeature::submitDraw, so the entity takes the direct route instead and still
            // counts as drawn. See EntityDecision::colourAlreadyDrawn.
            d.colourAlreadyDrawn = clusterDispatched;

#if AVER_MODULE_VOXI
            // F6/F4: the entity-level half of depth-prepass eligibility. BLENDED IS NOT CHECKED
            // HERE -- it's a per-draw question answered inside drawWorld, since a mesh with an
            // opaque trunk and translucent leaf part needs per-part accuracy one entity-level flag
            // can't give (setNextDrawPrepassed is auto-consumed by the next drawMesh(), not sticky --
            // RHI.hpp -- so one call before a multi-part loop would cover part 0 alone).
            // The three tests below restate prepassDecide's own exclusions (skinned, GPU cluster,
            // CPU per-cluster): an entity that walk skipped wrote no depth, so testing against the
            // LessEqual/no-write pipeline here would test against whatever depth was already there.
            {
                bool eligible = c.engine->device()->depthPrepassEnabled() && !self.wireframe_ &&
                                !clusterDispatched && !d.skinned;
#if AVER_MODULE_TRIFACTOR
                if (eligible && self.lodMeshShaderEnabled_ && self.lodMeshPipelineReady_ &&
                    self.meshClusterGpu_.count(d.meshId)) eligible = false;
                if (eligible && self.lodPerClusterEnabled_ && self.meshClusterData_.count(d.meshId))
                    eligible = false;
#endif
                d.prepassEligible = eligible;
            }
#endif
        };

        // ONCE PER ENTITY THE WALK ACTUALLY DELIVERED, on either route. `raster` separates the two
        // claims: an entity delivered on the DIRECT route was culled or owner-hidden, so it only
        // reached Voxi's shadow/GI/TLAS submission -- latching the selection outline off
        // "delivered" alone would outline off-screen objects.
        auto colourDelivered = [](scene::Entity ent, u64 meshId, rhi::MeshHandle chosenMesh,
                                  const Mat4& world, bool raster, void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            SandboxApp& self = *c.self;
            if (!raster) {
                // 3B: per-frame evidence a culled multi-part entity still yields N draws instead of
                // collapsing to slot 0's. Banked here, not in onDirectDraw, since "culled or merely
                // owner-hidden" is a per-ENTITY question the draws carry no answer to. Never
                // reached for an entity the angular-size floor held back (drawWorld does not fire
                // this for one -- the same accounting the deleted `if (angularFloorOk)` branch produced).
                if (c.entityCulled) {
                    c.culledDraws += c.entityDirectDraws;
                    if (c.entityDirectDraws > 1) ++c.culledMultiPart;
                }
                return;
            }
            // EVERY SELECTED ENTITY, NOT ONLY THE ANCHOR: used to keep one Mat4/mesh id, so a
            // multi-selection was highlighted in the Outliner but only one of five dragged props
            // showed an outline in the 3D view.
            if (self.sel_ == SandboxApp::kSelScene &&
                (ent == self.selEntity_ || self.multiIsSelected(ent))) {
                self.selectionOutline_ = world;
                self.selectionMesh_ = chosenMesh;
                self.selectionMeshId_ = meshId;
                self.hasSelection_ = true;
                // Anchor stays in the scalars above (other code reads them); the rest accumulate
                // here, cleared with hasSelection_ at the draw site so nothing outlives its frame.
                self.selectionOutlines_.push_back({world, meshId});
            }
        };

#if AVER_MODULE_VOXI
        // ONCE PER DRAW ON THE DIRECT ROUTE, right after the Voxi submit. This is PtSceneView's
        // only way to see off-screen geometry: submitDraw is otherwise reached only through
        // drawMesh(), which this route skips, so before this the path-traced view saw only what
        // the camera could see, and no cluster-dispatched instance at all -- no roof overhead, no
        // wall behind the camera (measured on PTTest NewSponza: Voxi's TLAS held 400 instances,
        // the path tracer "re-armed on 154", with 73 entities frustum-culled).
        //
        // A separate sink from options.voxiRenderer, deliberately (gotcha 3): the two are
        // independent in drawWorld, so the path tracer still gets fed with no Voxi feature
        // attached -- gating one on the other's presence is how the path tracer went blind before.
        //
        // Owner-hidden draws stay out: PtSceneView has no owner-hidden mask lane, so it would paint
        // the owner's own body over the camera, and the raster route never hands it those either.
        auto colourDirect = [](rhi::MeshHandle mesh, const f32 world[16], const f32 col[4],
                               f32 metallic, f32 roughness, rhi::BindingSetHandle matSet,
                               const void* matConstants, u32 matBytes, bool translucent,
                               bool hiddenFromOwner, void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            ++c.entityDirectDraws;
            if (c.self->ptSceneView_ && !hiddenFromOwner)
                c.self->ptSceneView_->submitDraw(mesh, world, col, metallic, roughness, matSet,
                                                 matConstants, matBytes, translucent);
        };
#endif

        // Same throttled-warning arrangement as the depth-prepass call's colourWarn twin above:
        // drawWorld owns the once-per-material-token bookkeeping (GameRender.hpp's SurfaceWarning)
        // so two hosts keeping separate sets can't drift; re-running resolveSurface() keeps this
        // line byte-identical to before the move, costing one extra lookup per token; `kind` goes
        // unread since it fills the same static sets.
        auto colourWarn = [](i32 mat, aver::game::SurfaceWarning kind, void* user) {
            (void)kind;
            (void)static_cast<ColourWalk*>(user)->self->resolveSurface(mat);
        };

        // Two diagnostics this walk would otherwise lose (each cost a day of bisection before it
        // said anything): the library reports the drop, the editor writes the sentence (see
        // DrawSkipReason). Not throttled on the library's side, since the two reasons need
        // separate keys.
        auto colourSkipped = [](scene::Entity ent, u64 meshId, aver::game::DrawSkipReason reason,
                                void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            SandboxApp& self = *c.self;
            if (reason == aver::game::DrawSkipReason::NotVisible) {
                // A mesh is named and the visible bit is clear -- the zero-fill trap:
                // World::addComponent hands back zeroed storage and kMeshRendererVisible is
                // positive-sense, so a renderer attached directly is attached, correct, and
                // invisible (GraphComponentTree.cs:100-103 dodges the same trap via SetVisible).
                // Once per entity: unthrottled would flood the log.
                //
                // NOT EVERY CLEAR BIT IS THE TRAP. A level placement that asked to be hidden
                // (OcWorldPlacement::visible false -- a collision volume, a proxy) never had the bit
                // seeded on purpose (LevelInstance.cpp), and an editor H-hide clears it too. Both kinds
                // of entity have an entityLabels_ record -- level load, paste, duplicate, spawn and MCP
                // placement all write one (SandboxLevelLoad.cpp, SandboxSelection.cpp,
                // SandboxViewport.cpp, SandboxMcp.cpp) -- while an entity a script built with
                // World::addComponent, the entity this warning is FOR, never gets one. The level that
                // motivated this had 8,765 authored-hidden colliders, and warning about each (one
                // flushed line apiece, on the first frame) buried every real fault in the log.
                //
                // Decided on the first sighting only, behind the same once-per-entity set as before, so
                // the per-frame cost for a hidden entity is what it was: one set lookup. An entity that
                // has a record is counted, not warned about, and reported in one line after the walk.
                if (self.undrawnInvisible_.insert(static_cast<u64>(ent)).second) {
                    if (self.entityLabels_.find(static_cast<u32>(ent)) != self.entityLabels_.end()) {
                        if (c.authoredHiddenNew < 3) c.authoredHiddenIds[c.authoredHiddenNew] = static_cast<u64>(ent);
                        ++c.authoredHiddenNew;
                    } else
                        AVER_WARN("[Sandbox] entity {} names mesh id {} but its kMeshRendererVisible "
                                  "bit is clear, so the scene walk skips it and it draws nothing. A "
                                  "component attached directly arrives zero-filled -- attach through "
                                  "Entity.SetVisible (EnsureMeshRenderer), which seeds the bit.",
                                  static_cast<u64>(ent), meshId);
                }
                return;
            }
            // An id that resolves to nothing, said once per id (not per entity): the id identifies
            // the fault, not whichever entity reached it first. Printed raw, not translated:
            // meshPathById_ is empty for exactly these ids (only populated by loadProjectMeshes for
            // meshes that loaded) -- fnv1a64 of the authored path is what to grep the .ocgraph/.ocmap for.
            if (self.undrawnMissingMesh_.insert(meshId).second)
                AVER_WARN("[Sandbox] mesh id {} (named by entity {}) is not in content_'s meshes, "
                          "so every entity naming it draws nothing. Built-in primitives are "
                          "seeded at startup and .ocmesh files are registered by "
                          "loadProjectMeshes from the project's content root -- an id that is "
                          "missing was never loaded under the string that was authored.",
                          meshId, static_cast<u64>(ent));
        };

        aver::game::DrawWorldOptions copt;
        // The possessed first-person pawn; a COMP tree can nest, so the pawn's body may be several
        // hops below it. World::setParent already refuses a cycle, so this walk terminates.
        copt.ownerHideRoot = firstPersonPawn_;
        copt.mobility = &playMobility_;
        // Worded differently from the library's line on purpose: "spawned CMeshRenderer entities"
        // names a concept a packaged game has no vocabulary for, and "culled" deliberately covers
        // occlusion as well as the frustum (see SceneDrawStats' comment for why the two can't be
        // unified).
        copt.suppressLog = true;
        copt.decide = colourDecide;
        copt.onEntityDelivered = colourDelivered;
        copt.onSurfaceWarn = colourWarn;
        copt.onSkipped = colourSkipped;
        copt.user = &walk;
        // --no-walk-cache, negated -- MUST MATCH the depth-prepass call site's popt.useMeshLookupCache:
        // both walks resolve the same mesh ids, so caching one and not the other would make
        // WalkLookup's hit-rate line below describe only half the frame's probes.
        copt.useMeshLookupCache = !g_noWalkCacheArg;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        copt.onVisit = colourVisit;
        // Empty means "no reordering": occlusionOrder_ is resized to n when culling runs and
        // cleared when idle, so a null order here is world order.
        if (!occlusionOrder_.empty()) {
            copt.visitOrder = occlusionOrder_.data();
            copt.visitOrderCount = static_cast<u32>(occlusionOrder_.size());
        }
#endif
#if AVER_MODULE_VOXI
        copt.voxiRenderer = &voxiRenderer_;
        copt.onDirectDraw = colourDirect;
#endif
        pbr::MaterialSystem* colourMaterials = nullptr;
        // Both modules, not just PBR: the system lives on voxiRenderer_ (declared only under
        // AVER_MODULE_VOXI, SandboxApp.hpp:3760). Same MaterialSystem resolveSurface() binds out
        // of, so each part's descriptor table and constants reach drawMesh() exactly as before this
        // moved into the library.
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        colourMaterials = &voxiRenderer_.materials();
#endif
        // THIS FRAME'S OWN, not a member: SceneDrawStats' `last*` trio is maintained even under
        // suppressLog, but it updates before the caller gets control back, so it can't answer "did
        // this change since last frame" for anyone but drawWorld -- hence SandboxApp keeps its own
        // lastSceneDrawn_/Culled_/OwnerHidden_ below.
        aver::game::SceneDrawStats colourStats;
        aver::game::drawWorld(*e.device(), viewProj_, content_, colourStats, colourMaterials,
                              skinnedScene_.get(), copt);
        // The one line that stands in for colourSkipped's old per-entity warning on entities that
        // carry a level placement record: how many were newly seen this walk, so a level that
        // authors thousands of hidden colliders says so once (on its first frame, and again only if
        // more entities go hidden later) instead of once per entity.
        if (walk.authoredHiddenNew)
            AVER_INFO("[Sandbox] {} entit{} with a level placement record newly seen with "
                      "kMeshRendererVisible clear (first: {} {} {}) -- authored-hidden placements "
                      "(collision volumes, proxies) or hidden in the editor; the scene walk skips them by "
                      "design. The zero-fill warning stays for an entity that has no placement record.",
                      walk.authoredHiddenNew, walk.authoredHiddenNew == 1 ? "y" : "ies",
                      walk.authoredHiddenIds[0], walk.authoredHiddenIds[1], walk.authoredHiddenIds[2]);
        // Closes "raster scene draws" here, at the end of the draw walk: the occlusion reporting
        // and pass-2 fallback below are not scene shading and don't belong in the number.
        rasterScope.reset();
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        // Pass-2-empty fallback: occlusionVisible_ still needs a fresh answer for every entity even
        // with nothing left to gate, or a wall that walked in front of a "visible" entity would
        // never be discovered and stay drawn forever.
        if (occlusionRuns) occlusionBuildAndTest();
        // Same power-of-two frame-count cadence D3D12Device's GPU-timing report uses, so this lands
        // alongside the "HZB build"/"HZB test" spans it prints, not a second unrelated rhythm.
        // F8: gated on occlusionRuns, not occlusionCullEnabled_ && occluder_ -- occlusionReportFrames_
        // only increments when occlusionRuns is true (the pass-2-empty fallback above and the
        // pass-1/pass-2-boundary call in the walk), so the wider condition would keep re-satisfying
        // this power-of-two check every frame culling sits idle, once the count freezes on a 2^n-1
        // value, instead of printing once and stopping.
        if (occlusionRuns && occlusionReportFrames_ &&
            (occlusionReportFrames_ & (occlusionReportFrames_ + 1)) == 0) {
            const f64 pct = occlusionTestedAccum_ ? 100.0 * static_cast<f64>(occlusionCulledAccum_) /
                                                    static_cast<f64>(occlusionTestedAccum_) : 0.0;
            AVER_INFO("[Occlusion] {} of {} tested entities culled ({:.1f}%) over {} frame(s)",
                      occlusionCulledAccum_, occlusionTestedAccum_, pct, occlusionReportFrames_);
            // 3B: THIS FRAME's own snapshot (unlike the lifetime accumulators above) -- concrete
            // evidence a culled multi-part entity still yields N draws, never collapsed to slot 0's.
            AVER_INFO("[Occlusion] this frame: {} entities culled (frustum or occlusion) delivered "
                      "as {} draws ({} multi-part)",
                      colourStats.culled, walk.culledDraws, walk.culledMultiPart);
            // Folded into the same cadence: a staleness-detector trip is rare enough that a separate
            // periodic line would mostly print zero, riding a report a reader already watches.
            if (occlusionStaleReadbacks_)
                AVER_INFO("[Occlusion] {} of those {} frame(s) had a readback more than one call "
                          "stale (see the one-time warning above) -- culling was fully disabled "
                          "on those frames, not merely more conservative",
                          occlusionStaleReadbacks_, occlusionReportFrames_);
            // Splits occlusionStaleReadbacks_ by which of two independent checks failed:
            // IOcclusionCuller::boxIdentityChurnCount() isolates the IDENTITY dimension (caller's
            // identityKey disagreed though the GPU generation stamp matched) from the TIMING
            // dimension (stamp itself missed by more than one call) -- a gap Occlusion.hpp's own
            // comment names ("needs a number to diff against, not just an instantaneous bool"),
            // unread by any caller before this. THIS SPLIT FOUND THE BUG hashIdentityKey()'s
            // own comment (OcclusionMath.hpp) documents: before that fix, identityChurn tracked
            // occlusionStaleReadbacks_ 1:1 under
            // --cam-wobble (244 of 255 frames, both counters) -- every "stale" warning while the
            // camera moved was the identity check misreading the caller's own motion-dilation
            // margin as population churn, not a GPU timing race. Left in place as a standing
            // diagnostic: a future regression that reopens that gap (or a genuine new one) shows
            // up here as the same 1:1 tracking, rather than as an unexplained stale-readback count.
            if (const u64 identityChurn = occluder_->boxIdentityChurnCount())
                AVER_INFO("[Occlusion] {} of those {} stale readback(s) were the IDENTITY check "
                          "(caller's box population/order actually changed), not a GPU timing "
                          "race -- see hashIdentityKey()'s comment (OcclusionMath.hpp) if this "
                          "number is tracking staleReadbacks almost 1:1 under camera motion",
                          identityChurn, occlusionStaleReadbacks_);
        }
#endif
        {
            const f64 walkMs = std::chrono::duration<f64, std::milli>(
                std::chrono::steady_clock::now() - tWalk0).count();
            if ((sceneWalkReports_ & (sceneWalkReports_ + 1)) == 0) {
                // Taken once for this whole throttled print, not once per line below:
                // collectCpuTiming() only reads the last window the facility already froze, so a
                // second call would see nothing newer, only cost another pass over kCpuSpanCount
                // nodes. Also lets walk.dispatchMs come from this snapshot, not a second accumulator.
                const CpuTimingReport cpuReport = collectCpuTiming();
                // walk.dispatchMs is sourced from the facility (see the CpuNest conversion at the
                // dispatch call site for why a second, independent steady_clock pair was removed
                // rather than kept alongside it). `nodes` is either empty (nothing published yet, or
                // this build can't report at all) or sized exactly kCpuSpanCount (collectCpuTiming's
                // own contract), so the emptiness check alone is enough to index ClusterDispatch
                // safely. Left at its constructed 0.0 when there's nothing to read.
                if (!cpuReport.nodes.empty()) {
                    walk.dispatchMs = cpuReport.nodes[static_cast<usize>(CpuSpan::ClusterDispatch)].ms;
                }
                AVER_INFO("[Sandbox] scene walk {:.1f}ms -- {:.1f}ms in cluster dispatch across {} "
                          "drawn ({:.1f}us each), {:.1f}ms in the rest over {} entities",
                          walkMs, walk.dispatchMs, colourStats.drawn,
                          colourStats.drawn
                              ? walk.dispatchMs * 1000.0 / static_cast<f64>(colourStats.drawn)
                              : 0.0,
                          walkMs - walk.dispatchMs, n);
                // Printed directly beneath the line above so the two are cross-checked, not read
                // in isolation: walkMs is this SINGLE frame's bracket, the tree below averages over
                // kCpuTimingWindowOccurrences, so they're expected to stay in the same neighbourhood,
                // not match to the decimal -- a wildly different tree total says the workload
                // changed shape, not that either instrument lies. This is the split the whole stage
                // exists for -- see tWalk0's own comment, and CpuTiming.hpp's top-of-file sweep
                // (250/1000/4000/16000 entities, ~0.45us/entity), which showed a single "6.8ms in
                // the rest" number couldn't say which of several candidate fixes it would pay for.
                // formatCpuTiming emits its own "not supported"/"no window yet" sentences when
                // there's nothing to show.
                formatCpuTiming(cpuReport,
                                 [](const std::string& line) { AVER_INFO("[Sandbox] {}", line); });
                // WalkLookup's hit rate, printed beneath the tree it explains (see
                // DrawWorldOptions::useMeshLookupCache on Stage 2's whole point). colourStats, not an
                // accumulator: meshLookupCacheHits/Misses are the colour pass's most recent call,
                // overwritten every frame like drawn/culled/ownerHidden.
                //
                // "off" is its own sentence, not "0 hits, 0 misses (0.0%)": g_noWalkCacheArg, not
                // hits+misses==0, decides which -- an emptied scene (n==0, nothing to resolve) reads
                // hits+misses==0 with the cache still ON, and a 0% there would look like a real (if
                // unlucky) measurement rather than "nothing to cache" (SceneDrawStats' own comment
                // names this exact trap).
                if (g_noWalkCacheArg) {
                    AVER_INFO("[Sandbox] WalkLookup mesh cache: off (--no-walk-cache)");
                } else {
                    const int lookupTotal = colourStats.meshLookupCacheHits +
                                             colourStats.meshLookupCacheMisses;
                    const f64 hitPct = lookupTotal
                        ? 100.0 * static_cast<f64>(colourStats.meshLookupCacheHits) /
                              static_cast<f64>(lookupTotal)
                        : 0.0;
                    AVER_INFO("[Sandbox] WalkLookup mesh cache: {} hits, {} misses ({:.1f}% hit rate)",
                              colourStats.meshLookupCacheHits, colourStats.meshLookupCacheMisses,
                              hitPct);
                }
#if AVER_MODULE_VOXI
                // M2(c): CPU cost of Voxi's acceleration-structure per-draw loop on its last rebuild
                // (VoxiRenderer::lastAccelBuildCpuMs, C-2), printed at the same cadence as the
                // scene-walk line above. Reads 0.0 until the first GI rebuild reaches the per-draw
                // loop -- see that method's own comment (VoxiRenderer.hpp): "no build yet" and "a
                // build that measured zero" can't be told apart from this number alone.
                AVER_INFO("[Sandbox] Voxi acceleration-structure draw loop {:.2f} ms CPU (last build)",
                          voxiRenderer_.lastAccelBuildCpuMs());
#endif
            }
            ++sceneWalkReports_;
        }
        // Read back out of SceneDrawStats, not counted here: suppressLog above stops the library
        // writing its own "[Game] scene-render:" line so the editor can write this one instead.
        // Compared against the last line PRINTED (g_sceneRenderLog), not colourStats' `last*` trio,
        // since drawWorld updates that trio before returning.
        //
        // THE LINE IS THROTTLED, THE STATE IS NOT. lastSceneDrawn_ is read every frame by
        // startupComplete()'s settle detector, so it (and its two siblings) follow the counts on
        // every frame exactly as before, whether or not a line is written. What is throttled is the
        // WRITE: over a big level a moving camera changes these counts nearly every frame, and each
        // AVER_* line takes the log mutex, flushes and feeds the Output Log. The line is compared
        // against what the LAST PRINTED line said (not against last frame), so a change that is held
        // back is still owed: once the camera settles, the next line is at most a second away and says
        // the final numbers. The first line always prints, and so does every change in a bounded run
        // (--frames N, the gates and captures), where frames are not one-to-one with wall time and the
        // log is what a script reads.
        const auto sceneNow = std::chrono::steady_clock::now();
        if ((colourStats.drawn != g_sceneRenderLog.drawn || colourStats.culled != g_sceneRenderLog.culled ||
             colourStats.ownerHidden != g_sceneRenderLog.ownerHidden) &&
            (g_sceneRenderLog.drawn < 0 || maxFrames_ != 0 ||
             sceneNow - g_sceneRenderLog.at >= std::chrono::seconds(1))) {
            // F4 (occlusion-fix-plan.md): `culled` now counts an occlusion-culled entity too, not
            // only frustum-culled -- previously the occlusion branch incremented no counter at all,
            // so its contribution was invisible here. Relabelled from "frustum-culled" to plain
            // "culled" to match (see chooseRoute()'s priority ordering for which counter an entity
            // BOTH culled and owner-hidden lands in).
            AVER_INFO("[Sandbox] scene-render: {} spawned CMeshRenderer entit{} drawn, {} culled, {} owner-hidden",
                      colourStats.drawn, colourStats.drawn == 1 ? "y" : "ies", colourStats.culled,
                      colourStats.ownerHidden);
            g_sceneRenderLog.drawn = colourStats.drawn;
            g_sceneRenderLog.culled = colourStats.culled;
            g_sceneRenderLog.ownerHidden = colourStats.ownerHidden;
            g_sceneRenderLog.at = sceneNow;
        }
        lastSceneDrawn_ = colourStats.drawn;
        lastSceneCulled_ = colourStats.culled;
        lastSceneOwnerHidden_ = colourStats.ownerHidden;
#if AVER_MODULE_TRIFACTOR
        // Greppable `[LOD-SELECT]`, only when the tuple changed. trianglesBeforeLod0 vs
        // trianglesAfterLevel predicts frame time; the cluster-cull counters are real telemetry
        // against the chosen level's meshlets, informational only (ClusterAdapt.hpp).
        if (lodSelectEnabled_ &&
            (lodStats_.instancesTested != lastLoggedLodStats_.instancesTested ||
             lodStats_.levelCollapsed != lastLoggedLodStats_.levelCollapsed ||
             lodStats_.trianglesAfterLevel != lastLoggedLodStats_.trianglesAfterLevel ||
             lodStats_.frustumCulled != lastLoggedLodStats_.frustumCulled ||
             lodStats_.coneCulled != lastLoggedLodStats_.coneCulled)) {
            AVER_INFO("[LOD-SELECT] instances={} levelCollapsed={} clustersTested={} "
                      "frustumCulled(info)={} coneCulled(info)={} trisBefore={} trisAfter={}",
                      lodStats_.instancesTested, lodStats_.levelCollapsed, lodStats_.clustersTested,
                      lodStats_.frustumCulled, lodStats_.coneCulled,
                      lodStats_.trianglesBeforeLod0, lodStats_.trianglesAfterLevel);
            lastLoggedLodStats_ = lodStats_;
        }
        // `[LOD-CLUSTER]`, greppable. `distinctLevelsMax`: if it never exceeds 1, every instance's
        // cut collapsed to one level (discrete LOD with extra steps). `rebuilds`/`hits`/`rebuildMs`
        // are the real CPU-assembly cost. `instancesShortcut` (the instance-level shortcut's count)
        // is NOT included in `instancesTested`/`clustersTested` -- sum both for the true total.
        if (lodPerClusterEnabled_ &&
            (lodClusterStats_.instancesTested != lastLoggedLodClusterStats_.instancesTested ||
             lodClusterStats_.clustersDrawn != lastLoggedLodClusterStats_.clustersDrawn ||
             lodClusterStats_.trianglesDrawn != lastLoggedLodClusterStats_.trianglesDrawn ||
             lodClusterStats_.instancesMixedLevels != lastLoggedLodClusterStats_.instancesMixedLevels ||
             lodClusterStats_.instancesShortcut != lastLoggedLodClusterStats_.instancesShortcut ||
             lodClusterStats_.rebuilds != lastLoggedLodClusterStats_.rebuilds)) {
            AVER_INFO("[LOD-CLUSTER] instances={} instancesShortcut={} clustersTested={} "
                      "frustumCulled={} coneCulled={} lodRejected={} clustersDrawn={} trisBefore={} "
                      "trisDrawn={} instancesMixedLevels={} distinctLevelsMax={} rebuilds={} hits={} "
                      "rebuildIdx={} rebuildMs={:.3f}",
                      lodClusterStats_.instancesTested, lodClusterStats_.instancesShortcut,
                      lodClusterStats_.clustersTested,
                      lodClusterStats_.frustumCulled, lodClusterStats_.coneCulled,
                      lodClusterStats_.lodRejected, lodClusterStats_.clustersDrawn,
                      lodClusterStats_.trianglesBeforeLod0, lodClusterStats_.trianglesDrawn,
                      lodClusterStats_.instancesMixedLevels, lodClusterStats_.maxDistinctLevelsSeen,
                      lodClusterStats_.rebuilds, lodClusterStats_.cacheHits,
                      lodClusterStats_.rebuildIndices, lodClusterStats_.rebuildMs);
            lastLoggedLodClusterStats_ = lodClusterStats_;
        }
        // `[LOD-MESH-SHADER]`, greppable, same log-on-change discipline. `clustersDispatched` is
        // real, the exact clusterCount every dispatchMeshClusters call used; `survivors`/
        // `trianglesDrawn`/`distinctLevelsMax` are CPU-mirrored telemetry sampled every 64 frames.
        if (lodMeshShaderEnabled_ &&
            (lodMeshShaderStats_.instancesTested != lastLoggedLodMeshShaderStats_.instancesTested ||
             lodMeshShaderStats_.clustersDispatched != lastLoggedLodMeshShaderStats_.clustersDispatched ||
             lodMeshShaderStats_.survivors != lastLoggedLodMeshShaderStats_.survivors ||
             lodMeshShaderStats_.instancesMixedLevels != lastLoggedLodMeshShaderStats_.instancesMixedLevels ||
             lodMeshShaderStats_.instancesShortcut != lastLoggedLodMeshShaderStats_.instancesShortcut)) {
            AVER_INFO("[LOD-MESH-SHADER] pipelineReady={} instances={} clustersDispatched={} "
                      "survivors(sampled)={} trisDrawn(sampled)={} instancesMixedLevels(sampled)={} "
                      "distinctLevelsMax(sampled)={} shortcut(sampled)={}",
                      lodMeshPipelineReady_, lodMeshShaderStats_.instancesTested,
                      lodMeshShaderStats_.clustersDispatched, lodMeshShaderStats_.survivors,
                      lodMeshShaderStats_.trianglesDrawn, lodMeshShaderStats_.instancesMixedLevels,
                      lodMeshShaderStats_.maxDistinctLevelsSeen, lodMeshShaderStats_.instancesShortcut);
            lastLoggedLodMeshShaderStats_ = lodMeshShaderStats_;
        }
#endif
    }
#endif
    // Selection outline: the selected mesh's boundary and crease edges, as LINES.
    //
    // USED TO BE A drawMesh, which is why there was no outline: drawMesh is gated on
    // sceneSuppressed(), true whenever ray-driven primary visibility is on (this engine's standing
    // default), so the outline was issued into nothing for the mode everyone uses. Measured: a
    // bounded capture with the gate lifted and an entity selected produced exactly ONE orange-ish
    // pixel in the whole viewport (a leaf vein); the device log said the same thing -- "2 render
    // features claim the whole scene ... The rasteriser draws NOTHING while this holds."
    //
    // drawLines is gated on suppressesWholeFrame() instead, which VoxiRenderer deliberately keeps
    // FALSE for ray-driven so chrome survives (grid, gizmo, nav mesh, collider overlay were already
    // on that path; the outline was the one piece that wasn't) -- D3D12Device::drawLines explains
    // why: "Gizmos and wireframes belong in a ray-driven viewport as much as in a rastered one, and
    // they depth-test against the real depth the ray pass writes."
    //
    // STILL INTERACTIVE-ONLY (maxFrames_ == 0), now for a different reason: the old reason (drawMesh
    // hands every draw to every feature before honouring suppressesScene, so the path tracer
    // accumulated an oversized solid copy) doesn't apply to drawLines. Stays because the render
    // gates run bounded, don't pass --no-editor-chrome, and start with sel_ = 1 (the placeholder
    // Cube) -- dropping this would put an orange outline in every gate image and move all twenty of
    // them.
    //
    // NOT SCALED: the old shell was inflated 1.02-1.12x to escape z-fighting; lines depth-test
    // against the ray pass's real depth and sit exactly on the geometry, so growing them would only
    // lift the outline off the object.
    //
    // anyPlayActive() needs AVER_MODULE_FRAMEWORK (declared with startPlay/stopPlay in
    // SandboxApp.hpp), so a framework-less build can never be mid-play. Resolved once here rather
    // than again at the marker's own gate below, so the two stay in lockstep.
#if AVER_MODULE_FRAMEWORK
    const bool anyPlaying = anyPlayActive();
#else
    const bool anyPlaying = false;
#endif
    // Ejected shows the outline again (UE shows selection in Simulate): anyPlaying itself must stay
    // the union every OTHER play-chrome gate (including the Player Start marker below) relies on.
    if (hasSelection_ && maxFrames_ == 0 && (!anyPlaying || playEjected()) && !noEditorChrome_) {
        // Unreal's selection outline width, scaled by DPI like the gizmo handles below.
        e.device()->setLineWidth(2.0f * dpi_);
        // One drawLines per selected entity. selectionOutlineLines caches per MESH id, so N
        // copies of the same asset share one line buffer and this costs N draws, not N buffers.
        for (const auto& [xf, meshId] : selectionOutlines_)
            if (const rhi::LineHandle lh = selectionOutlineLines(e, meshId))
                e.device()->drawLines(lh, &xf.m[0][0]);
        e.device()->setLineWidth(1.0f);   // sticky: back to hairline before the marker/grid below
    }
    hasSelection_ = false;
    selectionOutlines_.clear();

    // ---- THE PLAYER START'S MARKER: a wire capsule, a facing arrow and a billboard sprite, like
    // Unreal's APlayerStart ----
    // Queued every frame rather than kept as scene state, matching the rest of the editor's chrome.
    // Hidden by the same anyPlaying the outline above computes, so the two can't drift apart.
    // NOT gated on maxFrames_, unlike the outline: the outline responds to a click (meaningless in
    // a bounded run); the marker is part of what the level looks like.
    //
    // viewportIconsReady_ gates only the SPRITE below (it needs the icon renderer and its texture);
    // the capsule/arrow are ordinary line meshes and draw whenever the marker itself should be
    // visible, the same as the gizmo or the grid.
#if AVER_MODULE_SCENE
    if (!noEditorChrome_ && !anyPlaying && playerStart_ != scene::kInvalidEntity) {
        const scene::World& psw = scene::World::instance();
        if (psw.valid(playerStart_)) {
            const Transform& psXf = psw.localTransform(playerStart_);
            // No scale: psXf.scale is the marker's PICK CUBE size (makePlayerStart), unrelated to
            // the capsule/arrow's own real-world centimetre dimensions.
            const Mat4 psWorld = Mat4::fromQuat(psXf.rotation) * Mat4::translation(psXf.position);
            // Unreal highlights the selected actor's own shape rather than a separate outline on top
            // of it; the Player Start's mesh draw is skipped by identity (the colour walk above), so
            // selectionOutlines_ never carries it -- checked directly here instead, the same
            // selection test colourDelivered uses for every other entity.
            const bool psSelected = sel_ == kSelScene &&
                (playerStart_ == selEntity_ || multiIsSelected(playerStart_));
            e.device()->setLineDepth(true);
            e.device()->setLineWidth(1.5f * dpi_);
            e.device()->drawLines(psSelected ? playerStartCapsuleSel_ : playerStartCapsule_, &psWorld.m[0][0]);
            e.device()->drawLines(playerStartArrow_, &psWorld.m[0][0]);
            e.device()->setLineWidth(1.0f);

            if (viewportIconsReady_) {
                // Billboard at the capsule's own centre height -- no longer a pin with its tip on
                // the origin (see kPlayerStartIconHalfSize's own comment).
                viewportIcons_.addIcon(Vec3{psXf.position.x, psXf.position.y,
                                            psXf.position.z + kPlayerStartCapsuleHalfHeight},
                                       kPlayerStartIconHalfSize, playerStartIcon_);
            }
        }
    }
#endif

    e.device()->setWireframe(false);
    // Cleared with wireframe, and for the same reason: both are sticky device state, so leaving
    // unlit on here would flatten the grid, the gizmo and every other piece of chrome drawn
    // after the scene.
    e.device()->setUnlit(false);
    // Explicit, not relying on the selection outline/marker above resetting it themselves: the grid,
    // nav mesh and collider overlay below must stay hairline-thin regardless of what drew before them.
    e.device()->setLineWidth(1.0f);
    if (showGrid_ && !noEditorChrome_) {
        const Mat4 g = Mat4::identity();
        e.device()->drawLines(gridMesh_, &g.m[0][0]);
    }
#if AVER_MODULE_SYNAPSE
    if (showNav_ && navMesh_ && !noEditorChrome_) {
        // Already in world space -- buildNavOverlay emits absolute cell corners, because a
        // grid has an origin of its own and folding it into a matrix would mean two places
        // could disagree about where the navmesh is.
        const Mat4 n = Mat4::identity();
        e.device()->drawLines(navMesh_, &n.m[0][0]);
    }
#endif
#if AVER_MODULE_PHYSICS
    // ---- COLLIDERS, which could not be seen at all until now --------------------------------
    // No collider overlay, toggle or wireframe existed before; the drawLines infrastructure was
    // already here for the navmesh. TWO MESHES: the static bodies, rebuilt only when the bodies
    // change, and the few that move, remade each frame a play session runs and some box moved.
    // rebuildColliderOverlay keeps the last of each while its stamp holds. See it for the stamp and
    // the fallback to a full per-frame walk when untracked static bodies exist.
    if (showColliders_ && !noEditorChrome_) rebuildColliderOverlay(e);
    if (showColliders_ && !noEditorChrome_ && (colliderMesh_ || colliderMoverMesh_)) {
        const Mat4 cm = Mat4::identity();   // world space already, same as the navmesh
        if (colliderMesh_) e.device()->drawLines(colliderMesh_, &cm.m[0][0]);
        if (colliderMoverMesh_) e.device()->drawLines(colliderMoverMesh_, &cm.m[0][0]);
    }
#endif
    // GIZMO AND SCULPT CURSOR are chrome too -- and the gizmo is the loudest of the lot, since
    // it draws on top of geometry by design.
    if (!noEditorChrome_) {
        drawGizmo(e);
#if AVER_MODULE_LANDSCAPE
        drawSculptCursor(e);
#endif
    }
    buildUI(e);
#if AVER_WITH_IMGUI
    uiReg_.endFrame();
#endif
    submitGameUi(e);
    // Before captureCheck, because both use the device's single capture slot and the draw test
    // finishes inside the first dozen frames while the ordinary probe fires near the last.
    skinDrawCheck(e);
    if (refl_ && !refl_->finished()) {
#if AVER_MODULE_VOXI
        // The schedule owns both switches, because the experiment IS the pair of them: the same
        // world change has to be run past the ray tracer and past the cone tracer.
        voxi::Settings vs = voxi::Renderer::get().settings();
        const auto want = refl_->wantRayTracing() ? voxi::Quality::High : voxi::Quality::Off;
        if (vs.rayTracing != want) {
            vs.rayTracing = want;
            voxi::Renderer::get().setSettings(vs);
            voxiRenderer_.setSettings(vs);
        }
#endif
        if (reflBeaconIndex_ >= 0 && reflBeaconIndex_ < (int)objects_.size())
            objects_[reflBeaconIndex_].visible = refl_->beaconVisible();
        refl_->tick(e, vpX_, vpY_, vpW_, vpH_);
    }
    // lastSceneCulled_ only exists under AVER_MODULE_SCENE; skinScene_->tick() is called unguarded
    // since SkinSceneTest is a no-op with SCENE off anyway (0, same as lastSceneCulled_ would read).
    if (skinScene_ && !skinScene_->finished())
#if AVER_MODULE_SCENE
        skinScene_->tick(e, vpX_, vpY_, vpW_, vpH_, static_cast<u32>(lastSceneCulled_ < 0 ? 0 : lastSceneCulled_));
#else
        skinScene_->tick(e, vpX_, vpY_, vpW_, vpH_, 0u);
#endif
    captureCheck(e);
    // AFTER captureCheck, not before: both touch the device's single capture slot -- this ordering
    // lets serviceViewportScreenshot's own request skip only the one frame captureCheck requests on,
    // not also the frame after. By the time this call could take the slot, captureCheck's own
    // request for this tick has already run and capDone_ has latched it out of the slot for the
    // rest of the run.
    serviceViewportScreenshot(e);
    lumaSweepCheck(e);
    resizeCheck(e);
    gpuTimingCheck(e);
    rayProbeCheck(e);
#if AVER_MODULE_SYNAPSE
    navBakeCheck(e);
#endif
    // Rolled here, at end of frame: InputState's newFrame() must come BEFORE pumpEvents(), since
    // pressed()/released() report edges since the last newFrame(). Rolling it at the top of
    // onUpdate would discard the frame's own edges ("the game ignores single taps"). Same
    // placement as GameApp.
    input_.newFrame();
}

// VIEWPORT SCREENSHOT (File > Take Screenshot, F9). See requestViewportScreenshot's declaration in
// SandboxApp.hpp for the state machine's shape. request...() only sets the latch -- the actual
// requestCapture() call happens from onRender, where vpX_/vpY_/vpW_/vpH_ are current and the
// device is guaranteed to exist -- neither of which a keybind handler or a menu click running
// earlier in the frame can assume.
void SandboxApp::requestViewportScreenshot() {
    // A second press while one is in flight is a no-op, not a queued shot: the device has one
    // capture slot, and racing two requests would have the later one silently cancel the earlier
    // one's pending read.
    if (viewportShotState_ != 0) return;
    viewportShotState_ = 1;
}

// Runs once a frame, right beside captureCheck(e) in onRender -- AFTER it, which is what makes the
// single frame-number check below sufficient.
void SandboxApp::serviceViewportScreenshot(Engine& e) {
    if (viewportShotState_ == 0) return;

    if (viewportShotState_ == 1) {
        // --luma-sweep/--firefly-metric request a fresh capture every tick for the WHOLE run (see
        // captureCheck's own comment) -- there is no frame where the slot is safely ours to take.
        if (lumaSweep_ || fireflyMetric_) return;
        // The one frame captureCheck itself calls requestCapture() on, in a bounded run: taking
        // the slot first would have captureCheck's read land OUR pixel under its own probe
        // coordinates, reporting a wrong pixel as the probe's own. Leave the latch set
        // (requestViewportScreenshot() already refused to re-arm it) and try again next frame.
        const u64 f = e.time().frame;
        const u64 sf = maxFrames_ > 8 ? maxFrames_ - 3 : 4;
        if (maxFrames_ != 0 && f == sf) return;

        const u32 cx = static_cast<u32>(vpX_ + vpW_ * 0.5f);
        const u32 cy = static_cast<u32>(vpY_ + vpH_ * 0.5f);
        e.device()->requestCapture(cx, cy);
        viewportShotFrame_ = f;
        viewportShotTries_ = 0;
        viewportShotState_ = 2;
        return;
    }

    // state 2: awaiting the image. One frame of lag is inherent to requestCapture() -- serviced
    // inside present(), so a request made while handling frame f isn't ready until the next tick.
    if (e.time().frame <= viewportShotFrame_) return;
    ++viewportShotTries_;

    std::vector<u8> img; u32 iw = 0, ih = 0;
    if (e.device()->getFrameImage(img, iw, ih) && iw && ih) {
        // Crop to the 3D viewport alone: vpX_/vpY_/vpW_/vpH_ are already backbuffer pixels, so
        // this is a straight rect against the image -- but CLAMPED, not trusted, since the
        // viewport can resize or collapse between the request and the image landing, and an
        // out-of-range rect must fall back to the whole frame rather than read out of bounds.
        auto clampToRange = [](f32 v, u32 hi) -> u32 {
            if (v <= 0.0f) return 0;
            if (v >= (f32)hi) return hi;
            return (u32)v;
        };
        const u32 cx = clampToRange(vpX_, iw);
        const u32 cy = clampToRange(vpY_, ih);
        const u32 cw = clampToRange(vpW_, iw - cx);
        const u32 ch = clampToRange(vpH_, ih - cy);
        const bool crop = cw > 0 && ch > 0;
        const u32 outW = crop ? cw : iw;
        const u32 outH = crop ? ch : ih;

        std::vector<u8> cropped;
        const u8* pixels = img.data();
        if (crop) {
            cropped.resize((size_t)outW * outH * 4);
            for (u32 row = 0; row < outH; ++row)
                std::memcpy(cropped.data() + (size_t)row * outW * 4,
                            img.data() + ((size_t)(cy + row) * iw + cx) * 4,
                            (size_t)outW * 4);
            pixels = cropped.data();
        }

        // <project>/Saved/Screenshots -- same `Saved/` convention autosavePathFor documents for
        // recoverable, non-authored state. No project open falls back to the executable's
        // directory (same fallback CrashReport.cpp uses for Saved/Crashes).
        const std::string base = !project_.dir.empty() ? project_.dir : executableDir();
        const std::string dir = base + "\\Saved\\Screenshots";
        std::error_code mkec;
        std::filesystem::create_directories(dir, mkec);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char ts[32];
        std::snprintf(ts, sizeof ts, "%04u%02u%02u_%02u%02u%02u",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::string path = dir + "\\Screenshot_" + ts + ".png";

        // The log line is the answer without ImGui, the toast is the answer with it: notifyOutcome
        // pushes onto editor::notifications(), drained only by the notification overlay, so it
        // is declared behind AVER_WITH_IMGUI and the caller is guarded instead. A screenshot is
        // still taken and written in a -DAVER_ENABLE_UI=OFF build; only the toast has nowhere to appear.
        if (stbi_write_png(path.c_str(), (int)outW, (int)outH, 4, pixels, (int)outW * 4)) {
            AVER_INFO("[Sandbox] screenshot: {} ({}x{})", path, outW, outH);
#if AVER_WITH_IMGUI
            notifyOutcome(editor::NotifySeverity::Success, "Screenshot saved", path);
#endif
        } else {
            AVER_WARN("[Sandbox] screenshot: could not write {}", path);
#if AVER_WITH_IMGUI
            notifyOutcome(editor::NotifySeverity::Warning, "Screenshot failed",
                          "Could not write " + path, true);
#endif
        }
        viewportShotState_ = 0;
        return;
    }

    // GIVE UP rather than wait forever: a device loss or a stuck readback must not leave the latch
    // pending forever, which would silently swallow every later F9 press -- requestViewportScreenshot()
    // refuses to re-arm while viewportShotState_ != 0.
    if (viewportShotTries_ >= 30) {
        AVER_WARN("[Sandbox] screenshot: no frame image after {} frames, giving up", viewportShotTries_);
#if AVER_WITH_IMGUI
        notifyOutcome(editor::NotifySeverity::Warning, "Screenshot failed",
                      "The captured frame never arrived.", true);
#endif
        viewportShotState_ = 0;
    }
}

// Drives --skin-draw-test, handing it the LIVE viewport rect so its probes can be expressed as
// fractions of it -- fractions rather than pixels since the rect depends on DPI and which panels
// are open, and a probe landing on editor chrome would read chrome and report it as shading.
void SandboxApp::skinDrawCheck(Engine& e) {
    if (!skinDraw_ || skinDraw_->finished()) return;
#if AVER_MODULE_VOXI
    // The test's second experiment holds the pose still and toggles ray tracing instead: the sun
    // cascade rasterises from the same posed vertices and moves with the pose regardless of
    // whether the acceleration structure was rebuilt -- measured, not assumed.
    const bool want = skinDraw_->wantRayTracing();
    voxi::Settings s = voxi::Renderer::get().settings();
    const auto q = want ? voxi::Quality::High : voxi::Quality::Off;
    if (s.rayTracing != q) {
        s.rayTracing = q;
        voxi::Renderer::get().setSettings(s);
        voxiRenderer_.setSettings(s);
    }
#endif
    skinDraw_->tick(*e.device(), vpX_, vpY_, vpW_, vpH_);
}

// Hands the ABI's retained UI draw list to the game UI render feature, once per frame.
void SandboxApp::submitGameUi(Engine& e) {
    (void)e;
    if (!gameUi_) return;

    if (showUiDemo_) drawUiDemo();

    const auto* dl = static_cast<const aver::ui::UiDrawList*>(aver_ui_draw_list());
    if (!dl) return;
    gameUi_->submit(*dl);
}

// Draws the placeholder HUD demo through the UI C ABI: bars, crosshair, clipped list, tooltip.
void SandboxApp::drawUiDemo() {
    float vp[4] = {};
    aver_ui_viewport(vp);
    const f32 ox = vp[0], oy = vp[1], sw = vp[2], sh = vp[3];
    if (sw < 80.0f || sh < 60.0f) return;

    aver_ui_push_clip(static_cast<i32>(ox), static_cast<i32>(oy),
                      static_cast<i32>(ox + sw), static_cast<i32>(oy + sh));

    // Colours are 0xAABBGGRR, premultiplied.
    constexpr u32 kPanel   = 0xB0201814;   // near-black, 69% alpha
    constexpr u32 kFrame   = 0xFF3A3226;
    constexpr u32 kHealth  = 0xFF2E4CE8;   // red, in BGR order
    constexpr u32 kStamina = 0xFF3FC8E8;   // amber
    constexpr u32 kInk     = 0xFFE8E4DC;

    aver_ui_set_layer(AVER_UI_LAYER_CONTENT);
    const f32 barW = 260.0f, barH = 14.0f;
    const f32 barX = ox + 32.0f, barY = oy + sh - 240.0f;
    aver_ui_rect(barX - 3, barY - 3, barW + 6, barH * 2 + 12, kPanel);
    aver_ui_rect(barX, barY, barW, barH, kFrame);
    aver_ui_rect(barX + 1, barY + 1, (barW - 2) * uiDemoHealth_, barH - 2, kHealth);
    aver_ui_rect(barX, barY + barH + 6, barW, barH, kFrame);
    aver_ui_rect(barX + 1, barY + barH + 7, (barW - 2) * uiDemoStamina_, barH - 2, kStamina);

    // ---- TEXT, which this HUD could not draw until the font landed --------------------------
    // Straight onto the SAME draw list the bars above went into, through the C++ API since
    // aver_ui_text takes a font (a C++ type). Proves the geometry, atlas and texture path all
    // line up -- never exercised before; the C ABI seam for text is the next slice.
    if (uiFont_.valid()) {
        // const_cast because aver_ui_draw_list() hands back a const void*, the ABI's read-only
        // view -- appending to it is what every aver_ui_rect call above already does via the C
        // side; this reaches the same object by the C++ type.
        if (auto* dl = const_cast<aver::ui::UiDrawList*>(
                static_cast<const aver::ui::UiDrawList*>(aver_ui_draw_list()))) {
            constexpr u32 kLabel = 0xFFD8D2C8;
            char buf[64];
            std::snprintf(buf, sizeof buf, "HEALTH  %d%%", static_cast<int>(uiDemoHealth_ * 100.0f + 0.5f));
            dl->addText(barX, barY - 6.0f, buf, uiFont_, kLabel);
            std::snprintf(buf, sizeof buf, "STAMINA %d%%", static_cast<int>(uiDemoStamina_ * 100.0f + 0.5f));
            dl->addText(barX, barY + barH * 2.0f + 18.0f, buf, uiFont_, kLabel);
            // RIGHT-ALIGNED, which is what uiTextWidth is for: measuring before drawing is the
            // only way to place anything that is not left-aligned.
            const char* title = "Aver.UI can draw text";
            dl->addText(ox + sw - 12.0f - ui::uiTextWidth(uiFont_, title), oy + 24.0f,
                        title, uiFont_, kLabel);
        }
    }

    const f32 cx = ox + sw * 0.5f, cy = oy + sh * 0.5f;
    aver_ui_rect(cx - 11, cy - 1, 7, 2, kInk);
    aver_ui_rect(cx + 4,  cy - 1, 7, 2, kInk);
    aver_ui_rect(cx - 1, cy - 11, 2, 7, kInk);
    aver_ui_rect(cx - 1, cy + 4,  2, 7, kInk);

    const f32 pw = 220.0f, ph = 132.0f;
    const f32 px = ox + sw - pw - 32.0f, py = oy + 240.0f;
    aver_ui_set_layer(AVER_UI_LAYER_OVERLAY);
    aver_ui_rect(px, py, pw, ph, kPanel);
    aver_ui_push_clip(static_cast<i32>(px) + 8, static_cast<i32>(py) + 8,
                      static_cast<i32>(px + pw) - 8, static_cast<i32>(py + ph) - 8);
    for (int i = 0; i < 12; ++i) {
        const f32 ry = py + 12.0f + static_cast<f32>(i) * 18.0f - uiDemoScroll_;
        aver_ui_rect(px + 12, ry, pw - 24, 12, (i & 1) ? 0x60E8E4DC : 0x30E8E4DC);
    }
    aver_ui_pop_clip();

    aver_ui_set_layer(AVER_UI_LAYER_TOOLTIP);
    aver_ui_rect(px - 40, py + ph - 24, 96, 20, 0xE0202020);
    aver_ui_rect(px - 38, py + ph - 22, 92, 16, 0xFF6AC46A);

    aver_ui_pop_clip();
}

SandboxApp::ResolvedSurface SandboxApp::resolveSurface(i32 mat) {
    ResolvedSurface rs;
    aver::game::SurfaceInputs in;
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    if (const pbr::MaterialHandle authoredHandle = content_.authoredFor(mat)) rs.authored = authoredHandle;
    in.authored = rs.authored != 0;
    const pbr::MaterialDesc* d = rs.authored ? pbr::MaterialLibrary::get().desc(rs.authored) : nullptr;
    in.authoredLive = d != nullptr;
    in.translucent = d != nullptr && pbr::isTranslucent(*d);
#endif
#if AVER_MODULE_SCENE
    if (const auto* look = content_.lookFor(mat)) {
        in.haveLook = true;
        in.lookCol[0] = look->col[0];
        in.lookCol[1] = look->col[1];
        in.lookCol[2] = look->col[2];
        in.lookMetallic = look->metallic;
        in.lookRoughness = look->roughness;
    }
#endif
    rs.look = aver::game::resolveSurfaceLook(in);
    if (rs.look.warnDeadHandle) warnDeadMaterialHandle(mat);
    if (rs.look.usedFallback && mat != 0) {
        // Once per name; moved from the entity loop's own copy, same static set -- the only place
        // this warning fires from now.
        static std::unordered_set<i32> s_warnedMissingMaterial;
        if (s_warnedMissingMaterial.insert(mat).second)
            AVER_WARN("[Editor] surface '{}' has no .ocmat under Binaries/Materials or "
                      "Content/Materials and no built-in look; drawing the flat fallback "
                      "(0.80, 0.80, 0.85). Either author the material or use one of the "
                      "built-in names.",
                      editor::surfaceDisplayName(mat));
    }
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
    if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready()) {
        rs.matSet = ms.bindingSet(rs.authored);
        rs.matConstants = &ms.constants(rs.authored);
        rs.matBytes = sizeof(pbr::MaterialConstants);
    }
#endif
    return rs;
}

#if AVER_MODULE_SCENE
// F4: shared by the direct route and the raster substitution below. Skinning checked first
// (nothing forbids CSoftBody on an already-skinned mesh, so this order makes it deterministic).
rhi::MeshHandle SandboxApp::posedHandle(scene::Entity ent) {
    rhi::MeshHandle h = 0;
    if (skinnedScene_) h = skinnedScene_->drawHandle(ent);
#if AVER_MODULE_RENDER_SOFTBODY
    if (!h && softBodyScene_) h = softBodyScene_->drawHandle(ent);
#endif
    return h;
}

#endif

// F8's idle log names WHICH feature is painting the scene. debugViewActive()/rayDrivenActive()
// are PRIVATE to VoxiRenderer and SandboxApp is not a friend (C2248) -- not worth stopping the
// item over, since this function only ever feeds a human-readable log string, never a decision --
// so this reads the one PUBLIC signal that already answers "is Voxi the cause" (suppressesScene())
// instead -- loses the debug-raymarch-vs-ray-driven distinction in the log wording only; the F8
// gate itself (occlusionTestShouldRun, fed by e.device()->sceneSuppressed()) never called either
// private method and is unaffected. Widening VoxiRenderer's access instead was rejected: the
// plan's review checklist requires no file under modules/render.voxi to have changed.
const char* SandboxApp::occlusionSuppressingFeatureName() const {
#if AVER_MODULE_VOXI
    if (voxiRenderer_.suppressesScene()) return "ray-driven Voxi";
#endif
    if (ptSceneView_) return "Path Tracing";
    return "a registered feature";
}

void SandboxApp::setMsaaOverride(int n) { msaaOverride_ = n; }

void SandboxApp::setDepthPrepassOverride(bool on) { depthPrepassOverride_ = on; }

void SandboxApp::setGBufferOverride(bool on) { gbufferOverride_ = on; }

// --gbuffer-debug: see gbufferDebugView_'s own member comment for what this actually drives.
void SandboxApp::setGBufferDebugView(GBufferDebugFeature::Mode m) { gbufferDebugView_ = m; }

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
// --occlusion-cull: see occlusionCullEnabled_'s own member comment. Unset (default) never
// reorders the entity walk or calls occluder_ at all, reproducing today's frame exactly. Guarded
// the same as the member it assigns: with the module OFF, that member doesn't exist either.
void SandboxApp::setOcclusionCullOverride(bool on) { occlusionCullEnabled_ = on; }

// --no-occlusion-cull: the CLI has no way to say "off" that beats a level's own manifest --
// applyProjectVoxiSettings writes occlusionCullEnabled_ during load, AFTER CLI setters run at
// construction, so a bare setOcclusionCullOverride(false) would be silently overwritten the
// instant the level opens -- exactly the repro project's own case, whose manifest records
// OCCLUSIONCULL 1. Applied instead every frame in onUpdate (occlusionCullForceOff_
// below), the same point the Project Settings checkbox writes this member from -- i.e. this
// reproduces headlessly the same live toggle done by hand, not a new code path.
void SandboxApp::setOcclusionCullForceOff() { occlusionCullForceOff_ = true; }

// --occlusion-waitidle / --no-occlusion-waitidle: forces (or releases) OcclusionCuller::
// testBatch()'s res.waitIdle() at runtime, no rebuild -- see IOcclusionCuller::setDebugForceWaitIdle's
// own comment (Occlusion.hpp) for exactly what it does. Defaults ON: a real, unresolved GI/
// lighting dependency on the wait, not merely the buffer race the rotation fix already covers (see
// OcclusionCuller.cpp's FOLLOW-UP comment above kInFlight). Only SEEDS occlusionDebugForceWaitIdleArg_
// here, before occluder_ exists -- onInit copies it into editor::consoleOcclusionForceWaitIdleSlot()
// once occluder_ is created, and onUpdate reasserts that slot every frame, the same seed-then-reassert
// shape as occlusionCullForceOff_.
// for occlusionCullForceOff_ above.
void SandboxApp::setOcclusionDebugForceWaitIdle(bool on) { occlusionDebugForceWaitIdleArg_ = on; }

#endif

void SandboxApp::setGiOverride(int q, bool dbg) { giOverride_ = q; giDebugView_ = dbg; }

void SandboxApp::setGiForceOff(bool off) { giForceOff_ = off; }

// --no-gi-cone: the A/B measurement toggle (VoxiRenderer::setConeTraceEnabled). DELIBERATELY NOT
// --no-gi (which also stops the volume being built): this flag alone turns off the per-pixel
// cone-trace READ, so two otherwise-identical runs differ by exactly the trace's own GPU cost.
void SandboxApp::setGiConeTraceOff(bool off) { giConeTraceOff_ = off; }

void SandboxApp::setRtOverride(int q) { rtOverride_ = q; }

void SandboxApp::setPtOverride(int q) { ptOverride_ = q; }

void SandboxApp::setRtForceOff(bool off) { rtForceOff_ = off; }

void SandboxApp::setRtRays(int n) { rtRaysOverride_ = n; }

void SandboxApp::setGiSkyOcclusionRays(int n) { giSkyOccRaysOverride_ = n; }

void SandboxApp::setGiSkyOcclusionTile(int n) { giSkyOccTileOverride_ = n; }

void SandboxApp::setSkyLight(f32 v) { skyLightOverride_ = v; }

void SandboxApp::setGiIntensity(f32 v) { giIntensityOverride_ = v; }

void SandboxApp::setRtPixelsPerRay(int n) { rtPixelsPerRayOverride_ = n; }

void SandboxApp::setRtShadowDenoise(int n) { rtShadowDenoiseOverride_ = n; }

void SandboxApp::setRtRenderMode(int n) { rtRenderModeOverride_ = n; }

void SandboxApp::setRdStages(int n) { rdStagesOverride_ = n; }

void SandboxApp::setPtBounces(int n) { ptBouncesOverride_ = n; }

void SandboxApp::setLayeredBsdf(int n) { layeredBsdfOverride_ = n; }

void SandboxApp::setCoat(f32 w, f32 r, f32 f0) { coatWeight_=w; coatRough_=r; coatF0_=f0; }

void SandboxApp::setGiUpdateInterval(int n) { giUpdateIntervalOverride_ = n; }

// --gi-mode N. Unlike --rd-ablate this is NOT a shader define -- both estimators compile into
// every scene pipeline and PSMainVoxi/PSRayDriven choose per pixel via Settings::giMode at draw
// time, so this only has to reach setSettings() before the frame that reads it, not before init().
void SandboxApp::setGiMode(int n) { giModeOverride_ = n; }

// --restir-visibility none|reconstructed|half|full: same reasoning as setGiMode above -- read
// per pixel at draw time, not baked into a pipeline at load.
void SandboxApp::setRestirVisibility(int n) { restirVisibilityOverride_ = n; }

void SandboxApp::setDenoiser(int n) { denoiserOverride_ = n; }
void SandboxApp::setNeuralDenoise(int n) { neuralDenoiseOverride_ = n; }

void SandboxApp::setRenderScale(f32 s) { renderScaleOverride_ = s; }

#if AVER_MODULE_SR
// --aversr LEVEL. Records that the CLI chose it, so loadEditorPreferences leaves it alone -- the
// same "a flag exists so a human at the keyboard can override recorded state" rule the
// render-settings override block spells out at length.
// averSrCliLevel_ pins the level for updateAverSrAuto's resolveAverSrLevel call every frame, so a
// project reopened after startup can never re-derive the level out from under an explicit LEVEL the
// way it legitimately can for Auto -- "the command line wins" keeps winning past the first frame,
// not just when this setter runs.
void SandboxApp::setAverSrQuality(aver::sr::Quality q) {
    averSrQuality_ = q; averSrFromCli_ = true; averSrCliLevel_ = static_cast<int>(q);
}

// --aversr auto: EXPLICIT CLI Auto, distinct from never passing --aversr at all. Sets
// averSrFromCli_ so loadEditorPreferences still leaves the stored choice alone (the command line
// still wins), but leaves averSrQuality_/averSrCliLevel_ untouched (-1) so resolveAverSrLevel has
// no pinned CLI level to short-circuit with -- the project manifest and ladder default decide the
// level every frame, as if no --aversr flag had been given, except a stored user preference is
// never consulted this run.
void SandboxApp::setAverSrCliAuto() { averSrFromCli_ = true; averSrCliAuto_ = true; }

// Constructs SpatialUpscaler against `dev`'s resource factory if not already built. Idempotent --
// cheap to call every time the quality combo changes, not just once. See logAverSrActive()'s
// comment for what constructing it does and does not buy today.
void SandboxApp::ensureAverSrUpscaler(rhi::IDevice* dev) {
    if (!dev) return;
    if (!averSrUpscaler_) {
        if (rhi::IResourceFactory* res = dev->resources())
            averSrUpscaler_ = std::make_unique<aver::sr::FsrUpscaler>(*res);
    }
    applyUpscalerSlot(dev);
}

// Detaches before destruction, so the device can never hold a dangling upscaler.
void SandboxApp::clearAverSrUpscaler(rhi::IDevice* dev) {
    if (dev) dev->setUpscaler(nullptr);
}

void SandboxApp::setEdgeAaOverride(bool on) { edgeAaEnabled_ = on; }

// The device's one upscaler slot: FSR whenever the scene is rendered below native (an AverSR level
// OR a manual render scale, which used to fall back to a plain bilinear stretch) or edge AA is
// asked for; nothing otherwise, which keeps native-resolution frames bit-identical. Called every
// frame from onUpdate (outside beginFrame/endFrame), so a manual scale change takes effect at once.
// DETACH BEFORE DESTROY: the device holds a raw pointer, so it is told nullptr first.
void SandboxApp::applyUpscalerSlot(rhi::IDevice* dev) {
    if (!dev) return;
    rhi::IResourceFactory* res = dev->resources();
    // Temporal AA takes the slot whenever it is on, at any scale (it is the AA as well as the upscale).
    if (temporalAaEnabled_ && !taaUpscaler_ && res) taaUpscaler_ = std::make_unique<aver::sr::TemporalUpscaler>(*res);
    const bool wantFsr = !temporalAaEnabled_ &&
        (averSrQuality_ != aver::sr::Quality::Off || dev->renderScale() < 0.999f || edgeAaEnabled_);
    if (wantFsr && !averSrUpscaler_ && res) averSrUpscaler_ = std::make_unique<aver::sr::FsrUpscaler>(*res);
    rhi::IUpscaler* slot = temporalAaEnabled_ ? static_cast<rhi::IUpscaler*>(taaUpscaler_.get())
                         : wantFsr ? averSrUpscaler_.get() : nullptr;
    if (neuraaDebugView_ && !neuraa_ && res) neuraa_ = std::make_unique<aver::sr::NeuRaa>(*res);
    if (neuraaDebugView_ && neuraa_) {
        neuraa_->setInner(slot);
        neuraa_->setDebugView(true);
        slot = neuraa_.get();
    }
    if (averSrUpscaler_) {
        averSrUpscaler_->setEdgeAa(edgeAaEnabled_);
        averSrUpscaler_->setSharpness(fsrSharpness_);
    }
    if (taaUpscaler_) {
        taaUpscaler_->setEdgeAa(edgeAaEnabled_);
        taaUpscaler_->setSharpness(fsrSharpness_);
    }
    if (dev->upscaler() != slot) dev->setUpscaler(slot);
    if (!wantFsr && averSrUpscaler_) averSrUpscaler_.reset();
    if (!temporalAaEnabled_ && taaUpscaler_) taaUpscaler_.reset();
    if (!neuraaDebugView_ && neuraa_) neuraa_.reset();
}

// Logs the [AverSR] brand-tag line: current render scale, and whether an upscaler was
// constructed. The backend itself now logs when it actually runs the upscaler on the present path
// (device->upscaler(), D3D12Device.cpp), so this function no longer needs to carry that caveat.
void SandboxApp::logAverSrActive(rhi::IDevice* dev) {
    if (!dev) return;
    AVER_INFO("[AverSR] {}: render scale {:.2f}{}", aver::sr::qualityName(averSrQuality_),
              dev->renderScale(),
              averSrUpscaler_ ? "" : " (FsrUpscaler not constructed -- no resource factory)");
    if (averSrUpscaler_ && averSrQuality_ != aver::sr::Quality::Off)
        AVER_INFO("[AverSR] {} handed to the device; the backend reports when it upscales",
                  averSrUpscaler_->name());
}

// Applies one AverSR quality level from the render-settings combo: the docs/AVERSR.md
// render-scale table through the SAME rhi::IDevice::setRenderScale the slider next to it already
// edits. Off resets the scale to native and drops the upscaler -- bit-identical to never touching the combo.
void SandboxApp::applyAverSrQuality(rhi::IDevice* dev, aver::sr::Quality q) {
    // NAMED: a startup render scale of 0.67 once appeared with nothing in the CLI/manifest/
    // editor.ini asking for it, and this is the only code that can produce that number -- if this
    // fires, this is the source; if it doesn't and the scale still moves, look elsewhere. Fires
    // only on an explicit quality change.
    AVER_INFO("[AverSR] applyAverSrQuality({}) -> render scale {:.4f}",
              static_cast<int>(q), aver::sr::renderScaleFor(q));
    averSrQuality_ = q;
    if (!dev) return;
    if (q == aver::sr::Quality::Off) {
        // DETACH BEFORE DESTROY: the device holds a raw pointer to the upscaler; reset() before
        // detaching left D3D12Device::upscaler_ dangling and crashed the next frame's composite
        // (AverSR on->off crashed the editor; --edge-aa's first --frames run hit the same bug as a
        // SIGSEGV at process exit, fixed inline there). clearAverSrUpscaler() already guards this
        // ("detaches before destruction, so the device can never hold a dangling upscaler") but was
        // never called from here -- the only UI-reachable case, left open until now.
        // Scale first, then the slot: it keeps FSR (and so edge AA) when that is still wanted and
        // otherwise detaches before dropping it.
        dev->setRenderScale(1.0f);
        applyUpscalerSlot(dev);
        return;
    }
    dev->setRenderScale(aver::sr::renderScaleFor(q));
    ensureAverSrUpscaler(dev);
    logAverSrActive(dev);
}

// ---- AverSR's RESOLUTION CHAIN (needs the ladder, therefore Voxi) --------------------------
// Above: AverSR alone (scale, upscaler, CLI pin). Below: which LEVEL to apply, via
// voxi::resolveAverSrLevel/autoAverSrLevel against the Overall rung (render.voxi's ladder, named in
// these signatures: voxi::AverSrSource, voxi::Settings, voxi::DeviceInfo). SR and Voxi are
// independent (PBR=OFF forces VOXI=OFF too, so an SR-on/VOXI-off build is real, no voxi header in
// sight here); one caller gates both (SandboxApp.cpp onUpdate, #if AVER_MODULE_VOXI around #if AVER_MODULE_SR).
#if AVER_MODULE_VOXI
// Human text for the "(source)" half of every AverSR surface (startup log, the Display combo's
// "Auto (<level> from <source>)" preview, Project Settings line) -- one place so the three can't
// drift apart. ForcedOff means a tripped crash cookie (averSrCookieTripped_).
const char* SandboxApp::averSrSourceText(voxi::AverSrSource source) const {
    switch (source) {
        case voxi::AverSrSource::Auto:      return "Auto";
        case voxi::AverSrSource::Manifest:  return "project default";
        case voxi::AverSrSource::User:      return "your Display preference";
        case voxi::AverSrSource::Cli:       return "--aversr";
        case voxi::AverSrSource::ForcedOff: return "forced Off";
    }
    return "?";
}

// (3.3 A) Names the rung autoAverSrLevel (Scalability.hpp) resolved through, for display text only
// ("Auto from <rung>" / "(differs from the <rung> preset's default...)" wording on the Project
// Settings line) -- autoAverSrLevel returns the LEVEL, not the name; mirrors its own Custom branch
// (higher of GI/RT tiers) rather than sharing it.
const char* SandboxApp::averSrAutoRungName(const voxi::Settings& s, const voxi::DeviceInfo& d) const {
    const voxi::OverallQuality rung = voxi::overallFromSettings(s, d);
    if (rung != voxi::OverallQuality::Custom)
        return voxi::Renderer::qualityName(static_cast<voxi::Quality>(static_cast<u32>(rung)));
    const u32 giTier = static_cast<u32>(s.globalIllumination);
    const u32 rtTier = static_cast<u32>(s.rayTracing);
    return voxi::Renderer::qualityName(static_cast<voxi::Quality>(giTier > rtTier ? giTier : rtTier));
}

// optimisation-wave-2, U2 (3.3 A): resolves AverSR's level every frame from CLI > Display choice >
// project manifest > the Overall rung's ladder default (resolveAverSrLevel), applying only on
// change. Called from
// onUpdate, OUTSIDE beginFrame/endFrame: applyAverSrQuality ends in setRenderScale, and doing that
// mid-frame is the device-loss class aver-render-scale-device-loss documents.
void SandboxApp::updateAverSrAuto(Engine& e) {
    if (!voxiAttached_) return;
#if AVER_WITH_IMGUI
    // NOT BEFORE PREFERENCES LOAD: onUpdate runs before buildUI's loadEditorPreferences. On frame 1,
    // resolving Auto here armed display.renderScalePending, read back by loadEditorPreferences as
    // "the last launch did not survive applying Auto" and forced AverSR Off every launch regardless
    // of Display or the project's RENDER.AVERSR choice; headless never builds UI or loads prefs, so nothing to wait for.
    if (!prefsLoaded_ && !headless_) return;
#endif
    rhi::IDevice* dev = e.device();
    voxi::Renderer& vxr = voxi::Renderer::get();

    // A plain --render-scale (no --aversr) already owns the scale outright (onInit applies it
    // directly, the same "--render-scale wins" precedent loadEditorPreferences' guard uses);
    // resolving a level here would walk it back. --aversr itself (LEVEL or auto) is NOT caught:
    // onInit also sets renderScaleOverride_ as a side effect when it is not already pinned, so
    // averSrFromCli_ is needed as the other half of the check.
    const bool explicitRenderScaleOnly = renderScaleOverride_ != 1.0f && !averSrFromCli_;

    if (!explicitRenderScaleOnly && averSrChoice_ != editor::AverSrChoice::Manual) {
        const int cliLevel  = (averSrFromCli_ && !averSrCliAuto_) ? averSrCliLevel_ : -1;
        const int userLevel = editor::userLevelFor(averSrChoice_);
        const u32 autoLevel = voxi::autoAverSrLevel(vxr.settings(), vxr.deviceInfo());
        voxi::AverSrDecision decision =
            voxi::resolveAverSrLevel(cliLevel, userLevel, averSrProjectDefault_, autoLevel);

        // A level that just took the device down is never silently re-attempted: loadEditorPreferences'
        // own cookie check already forced Off and latched it for a level that didn't survive launch;
        // stays forced for the session, same as that load-time latch (cleared only by a fresh process).
        if (averSrCookieTripped_) decision = voxi::AverSrDecision{0u, voxi::AverSrSource::ForcedOff};

        averSrSource_ = decision.source;
        const aver::sr::Quality q = static_cast<aver::sr::Quality>(decision.level);
        if (q != averSrQuality_) {
            // (3.3 A) Armed before the first non-Off application this session: a level Auto
            // resolves mid-session can lose the device same as one loaded at startup -- same
            // cookie, extended to cover it; the existing 30-frame clear (onUpdate, beside the
            // shader watcher poll) applies unchanged.
            if (q != aver::sr::Quality::Off && !averSrArmedNonOffOnce_) {
                editor::setPrefBool("display.renderScalePending", true);
                editor::flushEditorPrefs();
                renderScaleCookieArmed_ = true;
                averSrArmedNonOffOnce_ = true;
            }
            applyAverSrQuality(dev, q);
        }
    } else if (!explicitRenderScaleOnly) {
        // Manual: the Render Scale slider owns the scale directly (prefsDevice_->setRenderScale).
        // Resolving through userLevelFor's -1 sentinel would fight the user's drag every frame, so
        // this branch only reports the choice and never touches the device.
        averSrSource_ = voxi::AverSrSource::User;
    }
    // else (explicitRenderScaleOnly): averSrSource_/averSrQuality_ untouched (Off); the startup
    // log below still fires -- a plain --render-scale run is still a non-native capture worth the warning.

    // MANDATORY STARTUP LOG (C2-10), fired once per process on every run including --frames -- the
    // only warning an ad-hoc capture that forgot --aversr off gets that it isn't native resolution.
    // Scene size is recomputed (D3D12Device::computeSceneSize's round-to-nearest; no IDevice
    // accessor reads it back, this line only reports it). Present size is e.window(), not dev
    // (integrator fix) -- IDevice has no width()/height() (only ISwapchain does); same accessor
    // GameApp::onInit uses.
    if (!averSrStartupLogged_ && dev) {
        const f32 scale = dev->renderScale();
        const u32 pw = e.window() ? e.window()->width()  : 0u;
        const u32 ph = e.window() ? e.window()->height() : 0u;
        const u32 sw = pw ? static_cast<u32>(std::lround(static_cast<f32>(pw) * scale)) : 0u;
        const u32 sh = ph ? static_cast<u32>(std::lround(static_cast<f32>(ph) * scale)) : 0u;
        if (averSrChoice_ == editor::AverSrChoice::Manual)
            AVER_INFO("[AverSR] Manual ({:.2f}) ({}): scene {}x{} -> present {}x{}; pass --aversr "
                      "off for native captures", scale, averSrSourceText(averSrSource_), sw, sh, pw, ph);
        else
            AVER_INFO("[AverSR] {} ({}): scene {}x{} -> present {}x{}; pass --aversr off for "
                      "native captures", aver::sr::qualityName(averSrQuality_),
                      averSrSourceText(averSrSource_), sw, sh, pw, ph);
        averSrStartupLogged_ = true;
    }
}
#endif  // AVER_MODULE_VOXI

#endif  // AVER_MODULE_SR

// Reconciles ptSceneView_ (actual registration) with ptSceneViewWantEnabled_ (what --pt-scene or
// the settings combo last asked for). Idempotent; safe every frame. Called from onUpdate() ONLY,
// never buildUI()/onRender(): that runs before beginFrame(), the one point nothing is mid-recording,
// and suppressesScene() is read live per drawMesh() through onRender -- mutating features_ mid-loop
// would let one frame's draws disagree, so this defers to the next onUpdate(). addRenderFeature()
// calls onRenderTargetsChanged() immediately against the device's CURRENT scene targets;
// removeRenderFeature() erases with no waitIdle (destroy calls retire behind the queue's fence, so
// releasing in-flight GPU objects is safe).
// OPEN GAP: PathTracer leaks every TLAS it builds (no destroyTlas in the RHI) -- one leak per
// re-arm; small, bounded, pre-existing.
bool SandboxApp::ptTakesViewport() const {
#if AVER_MODULE_VOXI
    if (ptSceneViewUnavailable_ || wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off)
        return false;
    if (debugView_ != voxi::VoxiRenderer::ViewDebug::None && voxiRenderer_.rayDrivenAvailable())
        return false;
    return ptSceneViewFromCli_;
#else
    return false;
#endif
}

void SandboxApp::syncPtSceneView(rhi::IDevice* dev) {
    if (!dev) return;

    // ---- NOTHING TRACES FOR A VIEWER THAT CAN'T SEE IT ----
    // Path-traced view and ray-driven primary visibility paint the same pixels; only one wins
    // suppressesScene() (measured: mode 0 -> tracer paints, 54% different, 7.8ms; mode 1 ->
    // ray-driven's frame). prePass accumulates regardless since it runs BEFORE the election
    // (PTTest: 8spp x 200 frames, ~9ms, thrown away) -- stopped here, and --pt-scene is told why.
    // A3: willSuppressSceneThisFrame(), not suppressesScene() -- rtActive_ isn't recomputed for
    // this frame until beginFrame() runs buildAccelerationStructures, so suppressesScene() would
    // answer LAST frame's question; this predicts prePass's answer instead (VoxiRenderer.hpp).
#if AVER_MODULE_VOXI
    // ptTakesViewport(): onUpdate forces raster primary this frame, so ray-driven will not paint
    // (its last-frame mode may still read 1 on the frame Path Tracing is switched on).
    const bool rayDrivenPaints = voxiRenderer_.willSuppressSceneThisFrame() && !ptTakesViewport();
#else
    // One fewer candidate: voxiRenderer_ is `#if AVER_MODULE_VOXI`-only but this function isn't --
    // the flag and PT view registration must keep working with the module off, so this is a constant.
    const bool rayDrivenPaints = false;
#endif
    if (rayDrivenPaints && ptSceneViewWantEnabled_) {
        // Set every frame this branch fires, so it stays true for as long as suppression does.
        ptSceneViewSuppressedByRayDriven_ = true;
        if (!ptSceneViewYieldLogged_) {
            ptSceneViewYieldLogged_ = true;
            AVER_INFO("[PT] the path-traced view is paused while a ray-hit debug view is "
                      "drawing; it resumes when the view goes back to Lit.");
        }
        ptSceneViewWantEnabled_ = false;
    } else if (!rayDrivenPaints) {
        ptSceneViewYieldLogged_ = false;   // re-arm the message if the mode changes back
        // The view comes back only if THIS yield caused the loss and --pt-scene still asks for it.
        if (ptSceneViewSuppressedByRayDriven_ && ptSceneViewFromCli_) ptSceneViewWantEnabled_ = true;
        ptSceneViewSuppressedByRayDriven_ = false;   // A1: same re-arm trigger as the log message
    }

    // ---- A RASTER-ONLY VIEW MODE HAS THE FRAME ----
    // Wireframe/G-buffer views force rtRenderMode 0 (onUpdate's auto-switch), but mode 0 also hands
    // the frame to THIS view, which wins suppressesScene() over raster -- without this block the
    // tracer, not wireframe, would fill the viewport. After the ray-driven block on purpose (its
    // release restores the want the same frame this withdraws it); released the same way -- returns
    // only if the view mode caused the loss and the request is still live, else the hold passes to ray-driven.
    const bool rasterViewMode = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
    if (rasterViewMode) {
        if (ptSceneViewWantEnabled_) ptSceneViewSuppressedByViewMode_ = true;
        ptSceneViewWantEnabled_ = false;
    } else if (ptSceneViewSuppressedByViewMode_) {
        ptSceneViewSuppressedByViewMode_ = false;
        if (rayDrivenPaints) {
            ptSceneViewSuppressedByRayDriven_ = true;
        } else {
            if (ptSceneViewFromCli_) ptSceneViewWantEnabled_ = true;
        }
    }

    if (ptSceneViewWantEnabled_ == (ptSceneView_ != nullptr)) return;

    if (ptSceneViewWantEnabled_) {
        ptSceneView_ = std::make_unique<aver::pt::PtSceneView>();
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // The HOST resolves the material -- only the host knows it bound one, which is why
        // Aver.Render.PathTracer can link just Aver.RHI and Aver.Core. ownsBindingSet() is the
        // IDENTITY test (a block merely sizeof(MaterialConstants) is only corroboration, not proof).
        // The fallback set means "not authored": fallbackSet_/fallbackConstants_' baseColorFactor
        // is {1,1,1,1}, so reading it would paint every non-authored surface white -- those draws
        // fall through to per-draw baseColor instead.
        ptSceneView_->setAlbedoResolver(
            [this](aver::rhi::BindingSetHandle set, const void* constants, aver::u32 bytes,
                   pt::PtSceneView::ResolvedMaterial& out) -> bool {
                aver::f32* outAlbedo = out.albedo;
                if (!set || !constants || bytes != sizeof(pbr::MaterialConstants)) return false;
                pbr::MaterialSystem& ms = voxiRenderer_.materials();
                // This early return is why the view re-arms twice on a static scene -- not a tracer
                // bug: until ready() flips, PtSurface uses the base colour; then every surface's
                // albedo changes at once, moving drawsKey()'s hash. Measured on FirstPerson: two
                // re-arms in the first frames, costing a few frames out of ~204 (used to cost more --
                // each also leaked a TLAS; PathTracer::addScene no longer does). NOT FIXED HERE:
                // false means both not-ready-yet and not-ours; telling them apart needs an AlbedoResolver signature change.
                if (!ms.ready()) return false;
                if (set == ms.fallbackBindingSet()) return false;   // un-authored: keep the look's colour
                if (!ms.ownsBindingSet(set)) return false;          // not one of ours at all
                const auto* mc = static_cast<const pbr::MaterialConstants*>(constants);
                // Emissive before the texture branches below, since all three return paths carry it:
                // the factor is already linear radiance and the path tracer takes no emissive map.
                out.emissive[0] = mc->emissiveFactor[0];
                out.emissive[1] = mc->emissiveFactor[1];
                out.emissive[2] = mc->emissiveFactor[2];
                // The tracer samples textures now, so outAlbedo means something DIFFERENT per branch.
                // WITH a texture: return the FACTOR (already linear) plus the handle -- multiplies
                // factor x texel, matching voxi.hlsl's textured hit. averageBaseColor here instead
                // would double-apply the texture (factor x texture MEAN), silently too dark.
                // WITHOUT one: unchanged, still necessary -- all forty demo materials declare
                // `baseColorFactor 1 1 1 1` and put their look in a texture, so factor alone painted
                // every surface pure white, ~3x too bright and flat. The mean is what
                // averageBaseColor's fallback is for, and still what a no-bindless device gets.
                if (const auto* tex = ms.textures(set)) {
                    // textures() reports EFFECTIVE handles: an unmapped slot is the 1x1 identity
                    // fallback (white / flat normal / (0,255,255,255) metal-rough), not 0 -- a
                    // correct multiply-by-one, at the cost of one fetch on an unauthored map.
                    const aver::rhi::TextureHandle base =
                        (*tex)[static_cast<aver::usize>(pbr::TextureSlot::BaseColor)];
                    if (base) {
                        out.baseColorTex  = base;
                        out.metalRoughTex = (*tex)[static_cast<aver::usize>(pbr::TextureSlot::MetalRough)];
                        out.normalTex     = (*tex)[static_cast<aver::usize>(pbr::TextureSlot::Normal)];
                        // Factors, not finished values -- multiplied by their maps. baseColorFactor
                        // is already linear (packMaterial decoded it); roughness/metallic need no decode.
                        outAlbedo[0] = mc->baseColorFactor[0];
                        outAlbedo[1] = mc->baseColorFactor[1];
                        outAlbedo[2] = mc->baseColorFactor[2];
                        out.roughness   = mc->roughnessFactor;
                        out.metallic    = mc->metallicFactor;
                        out.normalScale = mc->normalScale;
                        return true;
                    }
                }
                if (ms.averageBaseColor(set, outAlbedo)) return true;
                outAlbedo[0] = mc->baseColorFactor[0];
                outAlbedo[1] = mc->baseColorFactor[1];
                outAlbedo[2] = mc->baseColorFactor[2];
                return true;
            });
#endif
        if (ptSceneView_->init(*dev)) {
            dev->addRenderFeature(ptSceneView_.get());
            ptSceneViewUnavailable_ = false;
            AVER_INFO("[PT] scene view enabled");
        } else {
            AVER_ERROR("[PT] scene view unavailable on this device");
            ptSceneView_.reset();
            ptSceneViewUnavailable_ = true;
            // Don't retry every frame. No reach into voxi::Settings::pathTracing here --
            // syncPtSceneView() must keep working with AVER_MODULE_VOXI off -- so the settings-page
            // combo can go stale; its own BeginDisabled keeps the UI honest anyway.
        }
    } else {
        dev->removeRenderFeature(ptSceneView_.get());
        ptSceneView_.reset();
        AVER_INFO("[PT] scene view disabled; raster scene restored");
    }
}

void SandboxApp::setFrameTimeReport(bool on) { frameTimeReport_ = on; }

// --rd-ablate: forwarded to voxiRenderer_ BEFORE init(), because it becomes a shader define and
// the pipelines are compiled once there. Setting it later would be silently inert.
void SandboxApp::setRayDrivenAblation(int m) { rdAblate_ = m; }

// --rt-denoise-motion: forwarded to Voxi where --rd-ablate is, and for the same reason --
// both are measurement dials that have to be set before the renderer composes a frame.
void SandboxApp::setRtDenoiseMotionTaper(f32 v) { rtDenoiseMotionTaper_ = v; }

// --refraction / --refraction-strength / --refraction-fade. -1 in any of them means "not given".
void SandboxApp::setRefractionOverrides(int mode, f32 strength, f32 fade) {
    refractionOverride_ = mode; refractionStrengthOverride_ = strength; refractionFadeOverride_ = fade;
}

void SandboxApp::setMsOverride(bool on) { msOverride_ = on; }

void SandboxApp::setProbe(u32 x, u32 y) { probeX_ = x; probeY_ = y; }

void SandboxApp::setProbeRel(f32 u, f32 v) { probeU_ = u; probeV_ = v; }

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
bool SandboxApp::occlusionWasVisible(scene::Entity e) const {
    const auto it = occlusionVisible_.find(e);
    return it == occlusionVisible_.end() || it->second;
}

#endif

#if AVER_MODULE_SCENE
// Exact inverse of averFogFactor's k<=1e-8 branch (opacity(d) = 1 - exp(-density*d)): solves that
// same expression backwards for density given a target opacity. Not an approximation -- OcWorld.hpp
// has no fogFalloff/fogStart fields at all, so k and start are always 0 for anything a level can author.
 f32 SandboxApp::fogDensityForOpacityAt(f32 distanceCm, f32 targetOpacity) {
    if (distanceCm <= 1.0f) return 0.0f;
    const f32 t = targetOpacity < 0.01f ? 0.01f : (targetOpacity > 0.999f ? 0.999f : targetOpacity);
    return -std::log(1.0f - t) / distanceCm;
}

#endif

 rhi::MeshHandle SandboxApp::depthProxyLookup(rhi::MeshHandle mesh, void* user) {
    const auto& m = static_cast<const SandboxApp*>(user)->depthProxy_;
    const auto it = m.find(mesh);
    return it == m.end() ? 0 : it->second;
}

} // namespace aver
