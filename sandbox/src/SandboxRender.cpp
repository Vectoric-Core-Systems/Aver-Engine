// Runtime side: the world draw (onRender), surface resolution, occlusion, AverSR and the path-traced scene view.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
// game::drawWorld and its hook set: the ONE entity walk both hosts run, which onRender's depth
// prepass below now calls instead of keeping a second hand-written copy of. Included HERE and not
// in SandboxApp.hpp on purpose -- no member declaration needs the types (the decide/warn sinks are
// captureless lambdas local to onRender), and SandboxApp.hpp is included by every sandbox
// translation unit, so putting it there would rebuild all of them to serve one function.
#include "aver/game/GameRender.hpp"
// CpuSpan/CpuLap/CpuNest and formatCpuTiming: the CPU-side twin of rhi::GpuTimingReport, used below
// to convert the cluster-dispatch steady_clock pair into an exclusive CpuNest span and to print the
// per-bucket breakdown beneath the scene-walk log line. Included HERE rather than in SandboxApp.hpp
// for the same reason GameRender.hpp is above -- nothing declared on SandboxApp needs these types,
// so a member header would rebuild every translation unit that includes it to serve one function.
// Both headers are dependency-minimal by design (CpuTiming.hpp's own top comment: "included from
// Runtime/, from modules/render.voxi/ and from sandbox/") so this costs the rest of the file nothing.
#include "aver/core/CpuTiming.hpp"
#include "aver/core/CpuTimingFormat.hpp"

namespace aver {
// Submits the frame: the editor scene, the level world, gizmos, and the overlays.
void SandboxApp::onRender(Engine& e)  {
    handleManip(e);
#if AVER_MODULE_LANDSCAPE
    // Drains an undo/redo that changed terrain heights. Deferred to here because this is the
    // first point after those run that has a device -- see GameLandscape::applyHeightRect's own comment.
    landscape_.flushPendingInvalidate(e.device());
#endif
    // THE PROJECT LOADING SCREEN COMES DOWN HERE, not when applyProject returned. Same rule as
    // startupComplete: hold until the draw count has stopped changing, so it covers the tail of
    // the load rather than the head. startupComplete() also ticks the settle counters, and asking
    // it here is deliberate -- one implementation of "has this finished", not two that drift.
    //
    // A BORROWED SCREEN IS NOT OURS TO TIME, and the short-circuit is the point: during a
    // command-line load Engine::run's warm-up loop is already calling startupComplete() once a
    // frame, so a second call from here would tick the settle counter TWICE per frame and halve
    // the tail it is supposed to wait through. Follow the engine's own splash instead, and drop
    // the wrapper when that closes so the log sink stops routing into a window that is gone.
    //
    // AND IT IS CAPPED, because the settle rule alone can never fire on a project that draws
    // nothing: an empty start map, or one whose meshes all failed to load, leaves the draw count
    // at zero forever, and a top-most splash that never comes down is worse than one that comes
    // down early -- the editor is running behind it and unreachable. The startup path has this
    // cap already (Engine's 600-frame warm-up bound); the browser path had nothing, so it gets
    // the same number for the same reason.
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
    // UNLIT for the whole scene pass, alongside wireframe. setUnlit has been implemented on
    // both backends, reaching frame constant 22, since the selection outline needed it --
    // the shader branch at shared_prelude.hlsl's `gMaterial.z > 0.5` has been live the whole
    // time. Only the view-mode entry was missing. Cleared again at the end of the pass
    // (beside setWireframe(false)) so it cannot leak into editor chrome.
    e.device()->setUnlit(unlit_);
    // levelEntities_ exists only under AVER_MODULE_SCENE; with it off there is no loaded level to
    // hide the editor placeholders for, so the OR term is simply absent rather than always-false.
    // WHETHER A LEVEL IS OPEN, not whether it has anything in it yet.
    //
    // This read `|| !levelEntities_.empty()`, which made the built-in Floor and Cube disappear the
    // instant the FIRST object was added to an empty level. Reported as "drag and drop replaces the
    // default stuff": nothing was replaced -- the dropped object appeared and the placeholders hid
    // themselves in the same frame, which from the outside is indistinguishable.
    //
    // The placeholders mean "there is no level here". That is a fact about the LEVEL, not about its
    // contents, so an empty level hides them exactly as a full one does -- and adding the first
    // object to a level changes nothing about what else is on screen.
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
        // setDrawBlended is likewise sticky, and this loop draws the EDITOR'S OWN placeholder
        // primitives, never an authored .ocmat, so alphaMode stays Opaque and can never
        // legitimately be translucent. Set unconditionally to false rather than left alone:
        // relying on draw ORDER to keep a STICKY flag correct is exactly what a later reorder (a
        // new overlay above this loop) would silently break.
        e.device()->setDrawBlended(false);
        e.device()->drawMesh(o.mesh, &w.m[0][0], col, o.metallic, o.roughness);
        // The placeholder Floor/Cube have no .ocmesh behind them, so they get no outline --
        // selectionMeshId_ stays 0 and selectionOutlineLines returns 0 for it.
        if (i == sel_) selectionOutline_ = w, selectionMesh_ = o.mesh, selectionMeshId_ = 0,
                       hasSelection_ = true;
    }

    // ---- the simulated fluid, drawn like every other surface in the level ----
    // IT IS BACK HERE, AND THE ROUND TRIP IS THE POINT. This once drew as an ORDINARY OPAQUE mesh;
    // it moved to FluidScene::transparentPass for real water shading, at the cost of the pool
    // becoming invisible to shadow cascades, the TLAS and GI voxelisation -- "tinted plastic"
    // instead of water.
    // The TRANSLUCENT lane changes the bargain: VoxiRenderer::submitDraw takes a blended draw into
    // the TLAS marked non-opaque, so it's sorted, shadowed, fogged and shaded normally while still
    // skipping the two DEPTH-ONLY passes that could never express transmittance. No reason left
    // for water to own its own pipeline, shaders and constant buffer.
    // WHY BEING IN THE TLAS IS LOAD-BEARING: averVolumeThickness traces to the volume's own back
    // face for a real path length; a surface outside the structure has none, so the absorption
    // that makes water read as water needs this.
    // NOT GATED ON hideEditorScene: a fluid volume is authored level content, not an editor
    // placeholder, so it stays visible through Play like the landscape below. Material
    // resolution, the fallback look and per-volume placement now live in GameWater::draw.
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
    // The landscape pass: a ring update then one direct draw call through game::GameLandscape, the
    // same hand-rolled shape as the objects_ loop above -- not an IRenderFeature, and not gated on
    // hideEditorScene: terrain is real environment geometry, staying visible through Play like the
    // sky and fog.
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
        // THE THREE COUNTERS ARE THE WALK'S NOW. game::drawWorld fills a SceneDrawStats and the
        // editor reads it back (`colourStats`, at the call below) to word its own sentence, rather
        // than incrementing three locals from inside a loop it no longer owns. 3B's per-frame
        // direct-route tallies -- how many Draw records this frame's culled entities delivered,
        // and how many of those entities were multi-part -- left with them, into the hook context
        // the walk's sinks read through (ColourWalk, same call site).
#if AVER_MODULE_TRIFACTOR
        lodStats_ = LodSelectStats{};   // this frame's counters, from zero -- see the struct comment
        lodClusterStats_ = LodClusterStats{};
        lodMeshShaderStats_ = LodMeshShaderStats{};
        ++lodClusterFrame_;
        // Sweep the per-instance cut cache: an entry an instance did not touch for a while (its
        // entity destroyed, its CMeshRenderer removed, or lodPerClusterEnabled_ turned off) leaks
        // its GPU handle forever otherwise -- no destruction hook catches that here. 256 frames of
        // grace so a briefly-off-screen instance is never mistaken for a gone one; checked only every 64 frames.
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
        // STAGE 3: re-samples Voxi's CURRENT gVoxelTex_/shadowTex_ handles into every cluster
        // mesh's table-0 binding set BEFORE any entity is drawn through it this frame. ONE PASS
        // OVER meshClusterGpu_, bounded by DISTINCT meshes carrying GPU cluster data, not by
        // instance count -- and keeps the merged tables correct across a LIVE GI-quality or
        // shadow-resolution change, which recreate voxelTex_/shadowTex_ under fresh handles.
        if (lodMeshShaderEnabled_ && lodMeshPipelineReady_) {
            if (rhi::IResourceFactory* giRes = e.device()->resources())
                for (auto& kv : meshClusterGpu_)
                    if (kv.second.bindingSet)
                        voxiRenderer_.bindGiResources(*giRes, kv.second.bindingSet, kClusterGiSrvBase);
        }
#endif
#endif

        // THE SIX FRUSTUM PLANES ARE NOT DERIVED HERE ANY MORE. Both of this frame's walks are
        // game::drawWorld, which builds them from the viewProj it is handed, in the same
        // row-vector convention and with the same two dozen adds (GameRender.cpp). The copy that
        // stood here had exactly one reader, the entity loop's own cull, and outliving that reader
        // is how a second spelling of a rule gets left behind to drift -- which is the failure
        // this whole slice exists to remove, not to relocate.
#if AVER_MODULE_VOXI
        // ---- depth prepass phase: the COLOUR WALK'S OWN FUNCTION, run a second time ----
        // ONE ScopedGpuStat, not one per draw: the GPU stat tree budgets 64 open spans/frame and
        // Electric Dreams submits over a thousand instances -- per-draw markers would blow that
        // AND measure wrong, since a span's time is everything between its two timestamps IN
        // SUBMISSION ORDER, folding colour time into "depth prepass" if interleaved. The bracket
        // survives the move because game::drawWorld in DrawWorldPass::DepthPrepass is CONTIGUOUS:
        // it emits nothing but setDrawBinding and drawMeshDepthPrepass, never takes the direct
        // route and never touches a counter (GameRender.hpp's DrawWorldPass), so the span still
        // closes over a depth-only run and the number still means what it meant.
        //
        // WHY THIS IS A CALL AND NOT A LOOP. What stood here was a hand-duplicated copy of the
        // colour walk below -- its own entity iteration, its own planEntityDraws, its own
        // trifactor::chooseLevelCached -- and that duplication had already cost a real bug: the
        // world-space box was scoped differently in the two copies, so LOD selection fed the same
        // function different inputs and could pick a DIFFERENT LEVEL per walk. The prepass wrote
        // depth for one mesh while colour drew another and every fragment behind the wrong depth
        // was silently dropped -- fern clumps rendering visibly sparser, 2.81% of pixels differing,
        // falling to 0.04% (noise) with --no-lod-select. A comment asking two copies to agree only
        // records that they should; one function called twice cannot disagree with itself.
        //
        // EXCLUDED, now ANSWERED rather than restated: a SKINNED entity (posed vertex buffer is
        // compute-written), the GPU CLUSTER MESH-SHADER PATH (no depth-only twin) and the CPU
        // per-cluster path (--lod-per-cluster) are all machinery this library does not have, so
        // they come back through DrawWorldOptions::decide below. The LANDSCAPE is excluded by not
        // being in this walk at all -- it has its own call site. A TRANSLUCENT planned draw is
        // excluded INSIDE the library now (GameRender.cpp's depth-only delivery drops a draw whose
        // resolved look is blended), and that is where it belongs: glass must never write opaque
        // depth, which is a fact about the material, not about which LOD or skinning system claims
        // the geometry.
        //
        // TWO DEFECTS THIS PASS CARRIED FROM THE DAY IT WAS WRITTEN, both now closed, and both
        // found only because unifying the two walks forced someone to state what each pass skips.
        // The walk this replaced tested destroyPending, visible, mesh, skinned, cluster and
        // frustum -- and never owner-hide, and never the PlayerStart. So a possessed first-person
        // pawn's body wrote opaque prepass depth that the colour pass then refused to draw, and
        // every fragment behind it failed the depth test: a hole through the world in the shape of
        // the character you are playing. The PlayerStart marker, chrome the colour walk skips by
        // identity, punched the same hole by a second route. Both are answered below, on the same
        // conditions the colour pass uses. Reachable only with --depth-prepass, which rhi::RHI.hpp
        // and the D3D12 device both default OFF, which is why neither was ever reported.
        //
        // ONE THING THIS CALL DOES THAT THE DELETED WALK DID NOT, stated rather than discovered
        // later: drawWorld refreshes a non-skinned entity's CMeshRenderer aabbMin/aabbMax from
        // GameContent::boundsFor before it culls, and does it in BOTH passes -- decide() runs after
        // that write and cannot undo it (EntityDecision::skip's own contract). On every settled
        // frame this rewrites the identical values the colour walk wrote last frame, so nothing
        // moves. It differs only on the FIRST frame an asset's bounds exist -- the frame a level
        // loads, or a streamed mesh arrives -- where the prepass used to cull against last frame's
        // (or zeroed, hence degenerate, hence "draw it") box while the colour walk culled against
        // the fresh one. That is the two walks disagreeing for one frame, which is the failure this
        // commit exists to remove, so it is closed in the direction of agreement, not preserved.
        //
        // NO visitOrder EITHER, which is deliberate and not an omission: occlusionOrder_ reorders
        // the COLOUR walk so that everything seen last frame draws before buildPyramid(), and this
        // pass has no pyramid to build and no pass-1/pass-2 boundary to sit at. World order is what
        // the deleted walk used and what drawWorld does with a null order.
        if (e.device()->depthPrepassEnabled()) {
            if (rhi::IRenderContext* pctx = e.device()->renderContext()) {
                rhi::ScopedGpuStat prepassScope(*pctx, "depth prepass");

                // CAPTURELESS, so it converts to the plain DrawWorldDecideFn function pointer the
                // hook set takes (GameRender.hpp states why the sinks are function pointers and not
                // std::function: this is a per-entity path over thousands of entities a frame). A
                // lambda written inside a member function is a local class OF that member function
                // and so reaches SandboxApp's private members through `user`, which is what lets
                // this commit stay inside one .cpp instead of adding a declaration to SandboxApp.hpp
                // and rebuilding every translation unit that includes it.
                auto prepassDecide = [](aver::game::EntityDecision& d, void* user) {
                    // Its posed vertices live in a compute-written buffer, so the only depth this
                    // walk could write for it is the REST pose's -- not the one colour draws.
                    if (d.skinned) { d.skip = true; return; }
                    {
                        // THE PLAYER START IS CHROME IN BOTH PASSES. The colour walk skips it by
                        // identity and draws a billboard icon instead, so prepassing it wrote depth
                        // for a cube that is never drawn -- the same hole the owner-hide root above
                        // closes, by a second route. Skipped here for the identical reason and on
                        // the identical condition.
                        SandboxApp& ps = *static_cast<SandboxApp*>(user);
                        if (d.entity == ps.playerStart_ && ps.viewportIconsReady_) { d.skip = true; return; }
                    }
#if AVER_MODULE_TRIFACTOR
                    SandboxApp& self = *static_cast<SandboxApp*>(user);
                    // dispatchMeshClusters has no depth-only twin to call.
                    if (self.lodMeshShaderEnabled_ && self.lodMeshPipelineReady_ &&
                        self.meshClusterGpu_.count(d.meshId)) { d.skip = true; return; }
                    // clusterCutCache_ is rebuilt-or-reused once per frame per entity, and running
                    // that decision twice would either duplicate the rebuild or read a cache the
                    // colour walk has not populated yet -- not unsafe, just unneeded for a path
                    // nothing in this session enables.
                    if (self.lodPerClusterEnabled_ && self.meshClusterData_.count(d.meshId)) {
                        d.skip = true; return;
                    }
                    // THE CULL HAS ALREADY ANSWERED by the time decide() runs, and a culled entity
                    // emits no depth at all, so choosing its level would be work for a draw that
                    // never happens. The deleted walk got this ordering from a `continue` on the
                    // cull before it reached the ladder; here it is one line, said once.
                    if (d.frustumCulled) return;
                    // Discrete per-level LOD, guarded on haveWorldBox exactly the way the colour
                    // walk guards its own -- an instance without a usable box must take LOD 0 in
                    // BOTH walks or it is the same divergence by another route. Fed THE BOX THE
                    // CULL ALREADY USED (EntityDecision's, per its own contract) rather than one
                    // re-derived here, which is the specific mistake that made the two copies
                    // choose different levels.
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

                // THE EDITOR'S SENTENCE, THROUGH THE EDITOR'S OWN THROTTLES. drawWorld owns the
                // "once per material token, ever" bookkeeping (GameRender.hpp's SurfaceWarning)
                // because two hosts keeping two sets is how the counts drift -- but the wording is
                // the host's, and the library's default says "[Game]" and names no editor
                // directory. Re-running resolveSurface() is what keeps the line byte-identical to
                // the one this walk printed before the move: it reaches whichever of
                // warnDeadMaterialHandle and the missing-.ocmat warning its own ladder decides on,
                // filling the SAME function-local static sets the colour walk's resolveSurface()
                // fills, so a token first seen here still warns exactly once for the process rather
                // than once per walk. `kind` therefore goes unread -- the shared ladder re-derives
                // it from the same inputs -- and the duplicated resolve costs one lookup per token
                // per process, because the library's throttle only calls this for a token it has
                // never seen.
                auto prepassWarn = [](i32 mat, aver::game::SurfaceWarning kind, void* user) {
                    (void)kind;
                    (void)static_cast<SandboxApp*>(user)->resolveSurface(mat);
                };

                aver::game::DrawWorldOptions popt;
                popt.pass = aver::game::DrawWorldPass::DepthPrepass;
                popt.decide = prepassDecide;
                popt.onSurfaceWarn = prepassWarn;
                popt.user = this;
                // THE SAME OWNER-HIDE ROOT THE COLOUR PASS USES. Without it this pass wrote opaque
                // depth for the possessed pawn's own body, which the colour pass then refused to
                // draw -- so every fragment behind the body failed the depth test and the world
                // showed a hole in exactly the shape of the character you are playing. The two
                // passes have to answer "is this entity drawn" the same way or the prepass is
                // writing depth for something that never appears, which is the whole failure mode
                // a depth prepass has.
                popt.ownerHideRoot = firstPersonPawn_;
                // NO voxiRenderer AND NO onDirectDraw: the depth pass returns at `!route.raster`
                // before either sink can be reached, so attaching them would advertise a delivery
                // that cannot happen. NO onSkipped either -- this walk has never said a word about
                // an invisible-bit or missing-mesh entity, and the colour walk below still says
                // both, once each, into sets this call must not reach first.
                pbr::MaterialSystem* prepassMaterials = nullptr;
#if AVER_MODULE_PBR
                // The same MaterialSystem resolveSurface() binds out of, so each part's descriptor
                // table and constants reach drawMeshDepthPrepass exactly as they did here before.
                prepassMaterials = &voxiRenderer_.materials();
#endif
                // WRITTEN BY NOTHING, and that is the contract rather than an oversight:
                // DrawWorldPass::DepthPrepass touches no counter and prints no line, because those
                // three numbers describe a FRAME and this is one half of one. It exists only
                // because drawWorld takes the reference unconditionally; the editor's own
                // drawn/culled/owner-hidden are still counted by the colour walk below.
                aver::game::SceneDrawStats prepassStats;
                aver::game::drawWorld(*e.device(), viewProj_, content_, prepassStats,
                                      prepassMaterials, skinnedScene_.get(), popt);
            }
        }
#endif // AVER_MODULE_VOXI

        // IS THE FRAME CPU-BOUND OR GPU-BOUND? Establishing that took a dozen capture runs,
        // because --frame-time reports WHOLE frames from the CPU and time spent waiting for the
        // GPU looks exactly like CPU work. Two timers answer it directly: this one, and the
        // streamer's below. On Electric Dreams: 8.2ms walk + 0.7ms streaming inside a 76ms frame -- GPU-bound, so nothing here can matter.
        //
        // THIS BRACKET EXCLUDES THE DEPTH-PREPASS drawWorld CALL a few lines above (:372-373),
        // which is a SECOND full per-entity walk over the identical scene -- gated on
        // device()->depthPrepassEnabled(), false by default and turned on only by --depth-prepass.
        // So the number this bracket produces, and everything the CpuTiming breakdown below derives
        // from it, is THE WHOLE WALK today and only HALF of the CPU time actually spent walking the
        // scene the moment that flag is passed -- the other half runs, uninstrumented by this
        // bracket, inside the depth-prepass call above. A reader who sees this number drop by
        // roughly half after adding --depth-prepass has not found a speedup; they have found this
        // comment.
        const auto tWalk0 = std::chrono::steady_clock::now();

        const u32 n = w.count();

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        // ---- occlusion phase 0: collect every entity's world AABB, and split the walk order ----
        // A DEDICATED PRE-WALK, not the main loop's own per-entity computation: occluder_->
        // testBatch() is ONE GPU dispatch over every candidate (a synchronous readback per entity
        // would be 6,370 waitIdle() calls) and needs the WHOLE list, including pass-1 entities the
        // main loop only reaches after they're drawn. Degenerate-box entities are left un-culled,
        // same as the frustum cull's "must not vanish" rule.
        // pass1Count_ entities (occlusionWasVisible true, or never tested) sort FIRST and draw
        // UNCONDITIONALLY; the rest gate on this frame's testBatch() result once the pyramid from
        // the first group exists.
        u32 occlusionPass1Count = n;
        bool occlusionPyramidBuilt = false;
        // F7 (occlusion-fix-plan.md, "Link 6" -- the trigger): THE SCENE'S OWN SUB-RECT of the
        // target depth/pyramid texture, in target pixels, read fresh every frame regardless of
        // whether culling is even on -- the trust gate just below needs to compare it against
        // LAST frame's basis (occlusionBasisRect_) before anything else here runs, and the
        // box-collection loop and testBatch() call further down both need THIS frame's own copy.
        // False (RHI.hpp's sceneViewport doc: "False when the backend has no viewport to give")
        // zeroes it explicitly rather than trusting an untouched caller buffer, which lands on
        // exactly the same "whole target" convention ViewportRect's own w<=0/h<=0 rule already
        // uses (OcclusionMath.hpp) -- a backend that cannot answer this degrades to today's
        // assume-the-whole-target behaviour, never to a nonsense rect.
        f32 occRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (!e.device()->sceneViewport(occRect)) {
            occRect[0] = occRect[1] = occRect[2] = occRect[3] = 0.0f;
        }
        // ---- MOTION-SAFE TRUST GATE ----
        // testBatch()'s answer this frame is (at best) exactly one call stale -- see
        // Occlusion.hpp's corrected "TWO-PASS" section for why: this RHI has no primitive that
        // flushes and resumes THIS frame's own still-recording command list, only
        // IResourceFactory::waitIdle(), which only drains work already submitted. So the pyramid
        // buildPyramid() is about to build and the bytes testBatch() is about to hand back both
        // describe LAST frame's camera and LAST frame's depth, applied to THIS frame's entities.
        // On a static camera that is harmless -- last frame's answer IS this frame's answer,
        // which is exactly why the user's own report says a still camera is fine. Under motion it
        // is the precise false-cull failure mode Occlusion.hpp's TWO-PASS section names: an
        // object that entered view since the stale data was captured reads as hidden and pops
        // out. This computes how far the camera moved and turned since the LAST basis
        // occlusionBuildAndTest() stashed (occlusionBasisCamPos_/occlusionBasisForward_, not yet
        // overwritten for THIS frame -- that happens later, inside occlusionBuildAndTest, right
        // after buildPyramid() runs), and uses it two ways below: as a per-entity dilation margin
        // (the box-collection loop just past this comment) and as a global "distrust this whole
        // frame" gate when the motion is too large for a margin to safely cover (RESULT
        // APPLICATION inside occlusionBuildAndTest).
        //
        // THIS IS A RETROSPECTIVE PREDICTOR, NOT A PROOF OF THE COMING FRAME. It bounds motion
        // that ALREADY HAPPENED, and is used as an estimate of what the readback -- which will
        // not actually be consumed until later in THIS frame's own draw walk -- needs to be
        // trusted against. Under smooth motion that estimate is good; under a sudden
        // acceleration between two frames that were each individually under the trust threshold,
        // a margin sized off the slower window could under-cover the faster one. The only thing
        // that actually GUARANTEES no false cull is the global "not trustworthy" fallback below
        // (and, separately, IOcclusionCuller::readbackLagIsExactlyOneCall() catching a readback that
        // was not even the one-call-stale this whole calculation assumes) -- the per-entity
        // margin makes ordinary motion still cull something, it does not by itself prove safety.
        const f32 occlusionMoveDist = occlusionBasisValid_
            ? (camPos_ - occlusionBasisCamPos_).size() : 1e30f;
        const f32 occlusionRotRad = occlusionBasisValid_
            ? std::acos(std::clamp(dot(camForward(), occlusionBasisForward_), -1.0f, 1.0f))
            : 3.2f;   // > pi: forces "untrustworthy" before the first basis has ever landed
        // 45 degrees -- keeps the tan() below from blowing up before the global fallback (which
        // fires well before rotRad could reach this anyway, at kOcclusionTeleportRotRad) takes over.
        constexpr f32 kOcclusionRotClampRad = 0.785f;
        // Absolute cap on the rotation term's contribution, so a very distant occluder does not
        // dilate its box without bound. A KNOWN, SMALL RESIDUAL: an extremely distant,
        // extremely fast-swept occluder edge beyond this cap could in principle still pop for
        // one frame even when occlusionTrustworthy is true -- the global fallback only protects
        // against overall camera motion, not this specific distance/cap interaction. Flagged, not
        // hidden: this is not a claim of zero popping in every conceivable configuration.
        constexpr f32 kOcclusionMaxRotMarginCm = 2000.0f;
        // CULLING RUNS WHILE THE CAMERA MOVES, which is the whole point of having it -- the
        // motion thresholds above no longer VETO culling, they only decide whether the motion
        // was violent enough that even the dilated boxes cannot be trusted. Gating on "the
        // camera is still" was measured worse than useless: culling then never runs when it
        // would save anything, and the artefact it suppresses comes back the instant you move.
        //
        // WHAT KEEPS IT SAFE INSTEAD is the per-entity dilation below: every box grows by the
        // distance the camera has travelled since the pyramid was built, plus a rotation margin
        // scaled by its own distance. An object that could have entered view during the one
        // frame of lag is therefore inside its own dilated box and is not culled. That is the
        // mechanism designed for exactly this lag; the veto was a blunt substitute for it.
        //
        // The thresholds survive as an OUTER bound for genuinely discontinuous motion -- a
        // teleport, a --warp, a level load -- where no finite margin is correct because the
        // camera did not travel between the two positions at all.
        constexpr f32 kOcclusionTeleportMoveCm = 3000.0f;
        constexpr f32 kOcclusionTeleportRotRad = 1.571f;   // 90 degrees in one frame
        // F7: a dock-layout drag between the pyramid this basis describes and the (one-call-stale)
        // readback actually being consumed is a discontinuity no motion margin was ever meant to
        // cover -- occlusionBasisRect_ is the SAME scene sub-rect that produced the CURRENT basis,
        // so an exact mismatch here forces the same safe "everyone visible" fallback a teleport
        // does, rather than silently applying last basis's verdicts to a different mapping.
        const bool occlusionRectUnchanged =
            occRect[0] == occlusionBasisRect_[0] && occRect[1] == occlusionBasisRect_[1] &&
            occRect[2] == occlusionBasisRect_[2] && occRect[3] == occlusionBasisRect_[3];
        const bool occlusionTrustworthy = occlusionBasisValid_ &&
            occlusionMoveDist <= kOcclusionTeleportMoveCm &&
            occlusionRotRad <= kOcclusionTeleportRotRad &&
            occlusionRectUnchanged;
        const f32 occlusionRotMarginTan = std::tan(std::min(occlusionRotRad, kOcclusionRotClampRad));
        // F8 (occlusion-fix-plan.md): should the test even run THIS frame. `occluder_ != nullptr`
        // is haveOccluder; e.device()->sceneSuppressed() is true whenever a registered feature
        // (ray-driven Voxi, Path Tracing, the GI debug raymarch) has claimed the frame and is
        // painting the scene itself, in which case culling saves almost no work -- every culled
        // entity still has to be submitted for primary rays (see occlusionTestShouldRun's own
        // comment, aver/game/SceneSubmission.hpp, for the full accounting) -- so it idles unless
        // occlusion.cullUnderSuppression overrides it back on. THE WALK RUNS IN onRender AFTER
        // beginFrame (Engine.cpp's own onUpdate/beginFrame/onRender order), so sceneSuppressed()
        // here is THIS frame's own election result, not a stale one from before the ray-driven
        // pass ran.
        const bool occlusionRuns = aver::game::occlusionTestShouldRun(
            occlusionCullEnabled_, occluder_ != nullptr, e.device()->sceneSuppressed(),
            occlusionCullUnderSuppression_);
        // ONCE PER TRANSITION, not every idle frame -- ray-driven primary visibility is this
        // project's own standing default, so a per-frame line here would flood the log for nearly
        // the entire session. occlusionIdleLogged_ resets the moment occlusionRuns is next true, so
        // idle -> running -> idle again logs a second time rather than only ever once per process.
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
                // The SAME static-mesh-bounds override the main loop below applies -- MUST run
                // here too, or this pre-walk's box disagrees with the main loop's for frustum
                // culling the very first frame an entity is visited, before either pass has corrected bounds.
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
                    // Grown by the camera's own translation since the (last-frame) basis this
                    // readback will at best reflect, plus a distance-scaled rotation term
                    // (small-angle arc length ~= distance * tan(angle)) bounding how far a
                    // rotation of that size could have swept this box's occlusion boundary. A
                    // LARGER box can only make conservativelyHidden() (OcclusionMath.hpp) HARDER
                    // to satisfy, never easier -- selectConservativeMip only gets coarser as a
                    // box grows, per that header's own invariant comment -- so this can only
                    // REMOVE an existing false cull, never introduce a new one. See this loop's
                    // own trust-gate comment above for what it does NOT guarantee (bounding
                    // motion that happens between now and when this readback is actually
                    // consumed, later in this same frame's draw walk).
                    const Vec3 boxCentre{(wlo2.x + whi2.x) * 0.5f, (wlo2.y + whi2.y) * 0.5f, (wlo2.z + whi2.z) * 0.5f};
                    const f32 dist = (boxCentre - camPos_).size();
                    // TWO INTERVALS, NOT ONE -- and covering only one is why culling still
                    // popped while moving after the margin was first added. occlusionMoveDist
                    // measures motion that has ALREADY happened, from the basis to now. But this
                    // answer is not consumed now: testBatch()'s readback is one call stale, so
                    // the verdict computed here is applied on the NEXT frame, after the camera
                    // has moved again by roughly the same amount. A margin sized for the elapsed
                    // interval alone is short by exactly the interval that matters.
                    //
                    // The last interval is the predictor for the next one: a camera moving at
                    // constant velocity travels the same distance again, so doubling covers both
                    // exactly. Under acceleration it is approximate -- deliberately, because the
                    // alternative is a velocity estimator with its own lag -- and the teleport
                    // bound above catches the case where no finite margin is correct at all.
                    //
                    // Dilating too far only costs culling, never correctness: a LARGER box makes
                    // conservativelyHidden() harder to satisfy, never easier (OcclusionMath.hpp's
                    // own invariant), so an over-wide margin draws something that was genuinely
                    // hidden. That is the direction to err in.
                    constexpr f32 kOcclusionMotionLookahead = 2.0f;
                    const f32 margin = kOcclusionMotionLookahead *
                        (occlusionMoveDist +
                         std::min(dist * occlusionRotMarginTan, kOcclusionMaxRotMarginCm));
                    box.min[0] = wlo2.x - margin; box.min[1] = wlo2.y - margin; box.min[2] = wlo2.z - margin;
                    box.max[0] = whi2.x + margin; box.max[1] = whi2.y + margin; box.max[2] = whi2.z + margin;
                } else {
                    // Motion since the last basis (or the lack of a basis at all, or a readback
                    // the staleness detector could not vouch for -- see RESULT APPLICATION in
                    // occlusionBuildAndTest) is past what a margin can safely cover. The exact
                    // box submitted here does not matter: occlusionTrustworthy being false forces
                    // this WHOLE frame's answer to "visible" regardless of what testBatch() says.
                    box.min[0] = wlo2.x; box.min[1] = wlo2.y; box.min[2] = wlo2.z;
                    box.max[0] = whi2.x; box.max[1] = whi2.y; box.max[2] = whi2.z;
                }
                occlusionBoxes_.push_back(box);
                occlusionBoxEntities_.push_back(e2);
            }
            // `head` is now exactly the pass-1 count: every index landed on one side or the
            // other, head counting up from the front and tail down from the back, so head == tail
            // once the loop above has placed all n.
            occlusionPass1Count = head;
            if (rhi::IResourceFactory* occRes = e.device()->resources()) {
                rhi::TextureDesc sceneDesc;
                // sceneDepthTexture() itself supplies the size (via textureInfo below): asking
                // occluder_ to size its pyramid off anything else risks it disagreeing with the
                // ACTUAL depth buffer the seed pass is about to read.
                if (const rhi::TextureHandle depthTex = e.device()->sceneDepthTexture();
                    depthTex && occRes->textureInfo(depthTex, sceneDesc)) {
                    occluder_->ensureSized(*occRes, sceneDesc.width, sceneDesc.height, e.device()->sampleCount());
                    // F7's once-per-change diagnostic: fires exactly when the rect or the pyramid
                    // it sits inside actually changed since the last time this printed, so a dock
                    // layout drag (or a backend that starts/stops answering sceneViewport()) leaves
                    // a trail instead of only a silent change in which entities get culled.
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
            // F8: re-entry starts from "never tested = visible" -- the same safe default a freshly
            // spawned entity already gets (occlusionWasVisible's own comment) -- so a wall that
            // moved in front of something while culling was idle is discovered fresh on resume
            // rather than trusted from a verdict computed before the idle period began, and the
            // NEXT resumed frame does not silently reuse a motion basis from before the gap.
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
            // NO EARLY-OUT HERE, AND THE ONE THAT BRIEFLY STOOD HERE WAS A DEADLOCK. It skipped
            // the work whenever the camera had moved -- but the basis stash below is what tells
            // the NEXT frame how far the camera has travelled, so skipping it froze the basis at
            // the last still pose. occlusionMoveDist then measured against an ever-older
            // reference, stayed above the threshold forever, and culling never switched back on
            // even once the camera stopped. The symptom was "culling appears to do nothing",
            // which is exactly what it had been reduced to.
            rhi::IRenderContext* pctx = e.device()->renderContext();
            rhi::IResourceFactory* occRes = e.device()->resources();
            const rhi::TextureHandle depthTex = e.device()->sceneDepthTexture();
            if (!pctx || !occRes || !depthTex) return;
            occluder_->buildPyramid(*pctx, depthTex, &viewProj_.m[0][0]);
            // Stash the camera basis THIS call's readback will (at best, one call from now)
            // reflect -- see the trust-gate comment above the box-collection loop. camForward()
            // is the SAME vector that built viewProj_ this frame (the unconditional
            // camForward()/setCamera call earlier in this function, before either free-fly or
            // possessed-pawn camera handling has a chance to diverge), so it is authoritative
            // regardless of camera mode. Deliberately AFTER buildPyramid() (so a frame that
            // bails out above, before buildPyramid ever runs, does not stash a basis for a
            // pyramid that was never actually built) and BEFORE testBatch() (order does not
            // matter for testBatch itself, but keeping the stash immediately beside the call it
            // documents is the point).
            occlusionBasisCamPos_ = camPos_;
            occlusionBasisForward_ = camForward();
            // F7: stashed alongside the camera basis, same idiom, same reasoning -- see
            // occlusionBasisRect_'s own member comment.
            occlusionBasisRect_[0] = occRect[0]; occlusionBasisRect_[1] = occRect[1];
            occlusionBasisRect_[2] = occRect[2]; occlusionBasisRect_[3] = occRect[3];
            occlusionBasisValid_ = true;
            // IDENTITY, NOT GEOMETRY -- hashed from occlusionBoxEntities_ (which entities, in
            // which order), not from occlusionBoxes_'s bounds. THE BOUNDS ARE THE WRONG THING TO
            // HASH HERE: every box in occlusionBoxes_ was just dilated by occlusionMoveDist/
            // occlusionRotMarginTan above, both of which are a different float on nearly every
            // frame the camera is in motion, for the SAME population of entities. An identity
            // check keyed on those bytes (as this used to be) cannot tell "the population
            // changed" apart from "my own safety margin changed" -- see hashIdentityKey()'s own
            // comment (OcclusionMath.hpp) for the 244-of-255-frames measurement that caught it,
            // and testBatch()'s doc comment (Occlusion.hpp) for why the module now takes this as
            // a parameter instead of deriving it from the upload bytes itself.
            const u64 occlusionIdentityKey = aver::occlusion::hashIdentityKey(
                occlusionBoxEntities_.data(), static_cast<u32>(occlusionBoxEntities_.size()));
            // F7: `occRect` is THIS frame's own scene sub-rect (never folded into
            // occlusionIdentityKey above, which stays keyed on entity identity alone -- 0f85785a/
            // e04efdab's own rule, unchanged).
            occluder_->testBatch(*pctx, *occRes, occlusionBoxes_.data(),
                                 static_cast<u32>(occlusionBoxes_.size()), occlusionIdentityKey,
                                 occRect, occlusionResults_);
            u32 c = 0, t = 0;
            occluder_->lastTestCounts(c, t);
            occlusionCulledAccum_ += c;
            occlusionTestedAccum_ += t;
            ++occlusionReportFrames_;
            // Trust the raw per-box answer only when BOTH (a) camera motion since the stale
            // basis stayed under the dilation math's own bound (occlusionTrustworthy, computed
            // once above the box-collection loop) AND (b) this call's readback actually landed
            // exactly one call behind, not more (IOcclusionCuller::readbackLagIsExactlyOneCall() --
            // OcclusionCuller.cpp's generation-stamp check). Failing (b) means the "one call
            // stale" assumption the whole margin calculation rests on was WRONG this frame -- a
            // silent wrong answer that would otherwise just look like slightly more aggressive
            // culling -- so it forces the same safe "everyone visible" fallback as failing (a),
            // and it is worth knowing about rather than only silently correcting for.
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

        // PROSE, NOT CODE, since the constant it introduces moved down to the decide() sink beside
        // its only test -- and therefore no longer behind #if AVER_MODULE_VOXI, which would only
        // hide the account of a decision from the build that cannot make it.
        //
        // A CASTER THE CAMERA CANNOT SEE STILL CASTS A SHADOW. The frustum/occlusion/owner-hide
        // verdicts below used to skip DRAWING an entity via a bare `continue` past drawMesh() --
        // but drawMesh() is the ONLY thing that reaches the render features (submitDraw() is how
        // VoxiRenderer learns an entity exists), so a culled entity was absent from shadow
        // cascades, GI voxelisation and the RT TLAS: its shadow vanished the instant it left the
        // view. THAT IS THE "SHADOWS ARE SCREEN-SPACE" SYMPTOM, fair even though no shadowing
        // technique here is screen-space -- what FED the world-space cascades and RayQuery was the
        // camera frustum. F4 (occlusion-fix-plan.md) closes this by submitting the entity to the
        // features WITHOUT drawing it -- Voxi applies its own per-cascade cull in LIGHT space, the
        // cull a shadow caster should have gotten all along -- through the SAME per-draw emitter
        // the visible route uses, which is game::drawWorld's own now. The
        // lambda that used to live here (submitShadowOnly) is gone: it read a different material
        // (mesh-slot-0's, not each part's own), dropped multi-part splits entirely, and had DRIFTED
        // on the translucency test from the visible path's copy -- see
        // aver/game/SceneSubmission.hpp's own top comment for the full history this closes.
        //
        // BOUNDED BY ANGULAR SIZE -- the difference between free and unaffordable: submitting EVERY
        // culled entity was measured at +64ms/frame in ElectricDreams (5,884 of 6,617 entities
        // culled, mostly scatter plants), since the RT TLAS is a full rebuild every frame and
        // 759->6,571 instances took it from 9.1ms to 64.2ms. The floor is Voxi's own per-cascade
        // argument -- an object too small to fill a shadow texel cannot cast a visible shadow --
        // applied earlier, where it can stop the work, not just the draw. A NEGATIVE radius means
        // the caller had no bounds, so the entity submits regardless, matching the frustum cull's
        // own "must not vanish" rule. For scale: a crate at 5 m subtends ~0.2 rad and is kept; an
        // ankle-height plant at 100 m subtends ~0.003 rad and is not. The constant itself now sits
        // in the decide() sink below, beside the one branch that reads it: the direct route is
        // answered from there, and a threshold a few hundred lines from its only test is a
        // threshold nobody reads against its own prose.
        // THIS FLOOR IS ASYMMETRIC, AND THAT ASYMMETRY IS THE BUG. It exists only on the direct
        // route: an entity INSIDE the frustum reaches the renderer through drawMesh() with no size
        // test at all, and the moment it rotates outside it may be dropped here instead. So an
        // object sitting near ~1.1 degrees does not merely lose its shadow when it leaves the view
        // -- it leaves the GI draw list entirely, changing what giDrawsKey hashes, and rejecting
        // the rebuild gate on the frame it crosses. Panning past a field of small props re-bakes
        // the whole volume once per prop.
        //
        // GATED ON WHETHER ANYONE BUT THE SHADOW CARES. Voxi's submission feeds voxelisation and
        // the ray-traced TLAS as well as the cascades, and neither of those has any business being
        // decided by a caster-size heuristic -- a prop too small to cast a resolvable shadow still
        // occludes a ray and still bounces light. When Voxi is attached the floor is skipped and
        // the draw goes through; the heuristic survives for the case it was written for, which is a
        // build with no Voxi where this really is only feeding cascades. Applied ONCE PER ENTITY
        // (not per part) at the direct-route branch below, exactly where it applied before.

        // ONE GPU SPAN AROUND THE WHOLE OPAQUE WALK -- the raster path's counterpart to "Voxi
        // ray-driven primary". Until this existed there was no GPU-timed marker for raster pixel
        // shading, so raster-versus-ray-driven ratios compared a named ray span against `scene
        // draw`'s leftover EXCLUSIVE time -- not the same category of number.
        // ONE BRACKET, NOT ONE PER DRAW, same reason as the depth prepass above: per-draw markers
        // would fold neighbouring work together, and are unavailable anyway since the span cap is
        // 64 and overflow drops silently.
        // The occlusion culler's "HZB build"/"HZB test" spans nest INSIDE this one deliberately:
        // this span's EXCLUSIVE time is then the shading itself.
        // An optional rather than the depth prepass's `if (ctx) {...}` shape, since
        // IDevice::renderContext() legitimately returns nullptr (Null backend) and this loop must
        // still run. Closed explicitly after the loop, not at end of scope.
        std::optional<rhi::ScopedGpuStat> rasterScope;
        if (rhi::IRenderContext* rctx = e.device()->renderContext())
            rasterScope.emplace(*rctx, "raster scene draws");

        // ---- the colour walk: THE SAME game::drawWorld, this time in DrawWorldPass::Colour ----
        // What stood here was the hand-written original BOTH of this frame's walks were copied
        // from -- the entity iteration, the visible/mesh guards, the asset-bounds write-back, the
        // frustum cull, the owner-hide ancestor walk, material resolution, the per-part
        // planEntityDraws split and the raster/direct routing. Every one of those is in
        // Runtime/src/GameRender.cpp now, and the depth prepass at the top of this function has
        // already been calling it for a commit. A second spelling of the same rules here could
        // only ever mean two walks that agree until they do not, which is not hypothetical: the
        // prepass copy's world-box scope had already drifted far enough to pick a DIFFERENT LOD
        // level for the same instance, 2.81% of pixels (DrawWorldPass' own comment).
        //
        // NOTHING EDITOR-ONLY MOVED, AND NONE OF IT MAY. Aver.Occlusion links into Sandbox alone
        // and this library has no path tracer, so the hierarchical-Z machinery, Trifactor LOD and
        // the GPU cluster dispatch, the selection outline, the PlayerStart icon and the
        // path-traced scene view all stay in this file. They reach the walk through
        // DrawWorldOptions' hooks instead -- each hook a point where the editor answers a question
        // the library cannot ask on its own, and every one of them defaulting to the answer the
        // walk already gave (GameRender.hpp's own contract for the hook set).
        //
        // THE OCCLUSION ORDER IS WHY visitOrder EXISTS. occlusionOrder_ puts everything seen last
        // frame first so buildPyramid() has depth to build from; the pass-1/pass-2 boundary is a
        // raw index into THAT sequence, which is also why the boundary is fired from onVisit and
        // cannot be hoisted anywhere earlier -- it is a statement about the command stream.

        // THE FRAME STATE THE HOOKS NEED, and the reason it is a local struct rather than members
        // on SandboxApp. The sinks have to be CAPTURELESS lambdas to convert to the plain function
        // pointers DrawWorldOptions takes (GameRender.hpp states why it is function pointers and
        // not std::function: this is a per-entity path over thousands of entities a frame), so
        // everything they read arrives through the one `user` pointer. All of it is this frame's
        // and nothing else's -- the counters start at zero every frame, the occlusion split is
        // recomputed every frame -- so it belongs on this frame's stack, where its lifetime is
        // visible, and not on the app, where a value left behind would outlive the walk that wrote
        // it. A lambda written inside a member function is a local class OF that member function,
        // so `self` reaches SandboxApp's private members exactly as the code around it does; that
        // is also what keeps this commit inside one .cpp instead of adding declarations to
        // SandboxApp.hpp and rebuilding every translation unit that includes it.
        struct ColourWalk {
            SandboxApp* self = nullptr;
            Engine* engine = nullptr;
            // The occlusion pass-1/pass-2 split, read by onVisit alone. Left false/0 with the
            // module compiled out, where onVisit is not installed either.
            bool occlusionRuns = false;
            u32 pass1Count = 0;
            // occlusionBuildAndTest is a by-reference closure sitting on this frame's stack and a
            // function pointer cannot carry one, so it is reached by its own address through a
            // captureless trampoline -- filled in just below, beside the lambda it points at.
            void (*buildPyramidNow)(void*) = nullptr;
            void* buildPyramidUser = nullptr;
            // 3B's periodic-report extension, formerly two plain locals above this walk: how many
            // Draw records the direct route actually delivered for THIS frame's culled entities,
            // and how many of those entities were multi-part -- the concrete evidence that a culled
            // tree still yields N draws instead of collapsing to slot 0's single one. Counted
            // through the hooks now because the emission itself is the library's: onDirectDraw
            // tallies this entity's draws, onEntityDelivered banks them once the walk says the
            // entity was delivered on the direct route at all.
            u32 culledDraws = 0;
            u32 culledMultiPart = 0;
            u32 entityDirectDraws = 0;
            bool entityCulled = false;
            // CPU time inside dispatchMeshClusters, for the scene-walk report below. NO LONGER
            // ACCUMULATED HERE BY HAND: decide() times the dispatch itself now, through a
            // CpuSpan::ClusterDispatch CpuNest (see that call site's own comment on why a second,
            // independent steady_clock pair beside the CpuTiming facility would double-report the
            // identical microseconds). This field is instead filled, once per throttled print, from
            // the facility's own published ClusterDispatch bucket -- see the scene-walk log block
            // below -- so it stays at its constructed 0.0 on any frame that log block does not run.
            f64 dispatchMs = 0.0;
        } walk;
        walk.self = this;
        walk.engine = &e;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        walk.occlusionRuns = occlusionRuns;
        walk.pass1Count = occlusionPass1Count;
        using OcclusionBuildFn = decltype(occlusionBuildAndTest);
        walk.buildPyramidUser = const_cast<void*>(static_cast<const void*>(&occlusionBuildAndTest));
        walk.buildPyramidNow = [](void* p) { (*static_cast<const OcclusionBuildFn*>(p))(); };

        // THE ONE ORDERING REQUIREMENT IN THE WHOLE HOOK SET, and the only reason
        // DrawWorldVisitFn returns nothing: buildPyramid() reads the depth that pass one's draws
        // have just written, so it has to be RECORDED at the pass-1/pass-2 boundary IN THE COMMAND
        // STREAM. A pre-pass over the same entities could compute the same verdicts and still be
        // wrong, because the pyramid would be built against a different set of draws.
        auto colourVisit = [](u32 visitIndex, scene::Entity ent, void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            (void)ent;
            if (c.occlusionRuns && visitIndex == c.pass1Count && c.buildPyramidNow)
                c.buildPyramidNow(c.buildPyramidUser);
        };
#endif

        // THE EDITOR'S EVERY ANSWER, IN ONE CALL. decide() runs after the walk has computed this
        // entity's world box and both of its own verdicts and before it commits to a route, which
        // is exactly the point the deleted loop made all of these decisions at -- see
        // EntityDecision for why it is one in/out struct and not a handful of narrower callbacks
        // (the box is an INPUT: re-deriving it here is the precise mistake that made the prepass
        // and colour copies choose different LOD levels).
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
            // Skipping it by identity drops it from the opaque pass, shadow cascade, GI and RT
            // acceleration structure all at once -- right for a marker: it should not cast a
            // shadow, bounce light, or appear in a reflection.
            // NOT BY CLEARING kMeshRendererVisible: ray picking asks the IDENTICAL question, so
            // unsetting the flag would also remove the ability to click the marker. Already
            // special-cased by identity in loadLevel/unloadLevel/addPlayerStart.
            // The icon itself is queued further down, sharing the selection outline's
            // is-anything-playing test.
            //
            // IT IS LATE NOW, AND THAT IS A REAL CHANGE, stated rather than discovered later
            // (EntityDecision::skip's own contract says decide() cannot un-do what ran before it).
            // The deleted loop skipped the marker BEFORE the asset-bounds write-back and before
            // content_.meshFor, so this walk now refreshes the marker's CMeshRenderer aabb from
            // GameContent::boundsFor and will warn once through onSkipped if its mesh id ever
            // fails to resolve. Both were already true of the occlusion pre-walk above, which
            // carries no PlayerStart exception at all, and of the depth-prepass call.
            if (d.entity == self.playerStart_ && self.viewportIconsReady_) { d.skip = true; return; }

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            // PASS-2 ONLY: `visitIndex < pass1Count` entities drew unconditionally before
            // occlusionBuildAndTest() ran. Everything below has ALREADY been tested against a
            // pyramid buildPyramid() built from every pass-1 draw -- but CORRECTED: that does NOT
            // make occlusionWasVisible(ent) "this frame's own fresh answer". testBatch()'s
            // readback is, at best, one call stale (Occlusion.hpp's corrected "TWO-PASS" section)
            // -- what actually landed in occlusionVisible_[ent] this frame, inside
            // occlusionBuildAndTest's RESULT APPLICATION, is the raw (motion-dilated) test result
            // ONLY when this frame's camera motion since the last basis AND the readback's own
            // staleness (IOcclusionCuller::readbackLagIsExactlyOneCall()) both stayed inside what
            // the trust gate above the box-collection loop is willing to trust; otherwise it is
            // forced to "visible" regardless of what the pyramid says. A box excluded from
            // occlusionBoxes_ (degenerate) was never tested and defaults to visible, same as
            // EntityDecision::haveWorldBox being false does here. `visitIndex` is the position in
            // occlusionOrder_, which is the sequence the boundary was cut in -- DrawWorldOptions
            // hands the hooks that index for exactly this reason.
            d.occlusionCulled = c.occlusionRuns && d.visitIndex >= c.pass1Count && d.haveWorldBox &&
                                !self.occlusionWasVisible(d.entity);
#endif
            // The by-hand false-cull finder (section 3B): an otherwise-culled, non-owner-hidden
            // entity draws through the RASTER route anyway, tinted. chooseRoute owns that pairing,
            // so this is fed to it rather than applied here.
            // GUARDED, unlike occlusionCullEnabled_ which is a manifest key: the tint answers "what
            // did the culler decide to hide", so with no culler compiled in there is nothing for it
            // to show. d.occlusionCulled is false throughout that build for the same reason, so the
            // route chooseRoute picks is identical either way.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            d.tint = self.occlusionShowCulled_;
#endif

            // ASKED HERE TOO, NOT RESTATED. drawWorld calls the identical chooseRoute() on the
            // identical four inputs a few lines after this returns (GameRender.cpp), so the two
            // cannot disagree about a pure function. The editor needs the answer BEFORE it hands
            // control back, because everything the deleted loop did from this point on sat on one
            // side or the other of this very `if (!route.raster)`: the angular-size caster floor
            // below it, Trifactor LOD and the cluster dispatch above it.
            const aver::game::RouteDecision route = aver::game::chooseRoute(
                d.frustumCulled, d.occlusionCulled, d.ownerHidden, d.tint);

            if (!route.raster) {
                // THE UNIFIED DIRECT ROUTE: frustum-culled, occlusion-culled or owner-hidden.
                // Still submitted to Voxi so shadows, GI voxelisation and the RT TLAS never depend
                // on what the camera itself can see -- see this walk's own "A CASTER THE CAMERA
                // CANNOT SEE STILL CASTS A SHADOW" comment above. The submission itself is the
                // library's now; what is left here is the two answers it cannot give.
                //
                // KNOWN RESIDUAL, STATED RATHER THAN FIXED (occlusion-fix-plan.md F4): this route
                // never runs LOD/cluster selection, so a posed (skinned/soft-body) entity aside,
                // it always plans from the BASE mesh and its full part split. A VISIBLE entity
                // whose LOD or cluster cut substitutes a different, unsplit handle therefore
                // delivers ONE draw of that handle while the SAME entity, culled, delivers its
                // base parts -- only live with LOD selection or the CLI cluster paths (LODSELECT
                // is 0 in PTTest, so dormant here).
                c.entityCulled = d.frustumCulled || d.occlusionCulled;
#if AVER_MODULE_VOXI
                // THE SOFT-BODY SEAM: posedHandle() asks skinning first and then a soft-body scene
                // (nothing forbids CSoftBody on an already-skinned mesh, so asking skinning first
                // makes the collision deterministic). EntityDecision::chosenMesh arrives holding
                // skinning's answer alone, because the library has no soft-body scene to ask --
                // GameRender.hpp names this seam outright -- so filling it in is the host's job.
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
                // THE ANGULAR-SIZE FLOOR -- see its own comment above this walk for the full
                // "GATED ON WHETHER ANYONE BUT THE SHADOW CARES" reasoning. Applied ONCE PER
                // ENTITY, matching its old home exactly.
                //
                // emitDirectDraws, NOT skip, AND THE DIFFERENCE IS MEASURED. `skip` counts the
                // entity nowhere; this drops its Voxi submission while it still counts as culled,
                // which is what the deleted branch did (its `if (angularFloorOk)` wrapped only the
                // plan-and-emit, never the `++culled` after it). Getting this the other way round
                // is the 9.1 -> 64.2 ms/frame that motivated the floor in the first place.
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

            // THE ENTITY-LEVEL RESOLVE, AND IT IS NOT THE DRAWS'. Each planned draw's own token is
            // resolved by the library, once per draw, through the same authored > look > fallback
            // ladder this function calls (aver/game/SceneSubmission.hpp's resolveSurfaceLook). This
            // one survives for two readers the walk cannot serve. The GPU cluster path below binds
            // col/metallic/roughness/matSet/matConstants through the CONTEXT rather than the
            // device -- setDrawBinding records on the DEVICE, which only forwards it from inside
            // drawMesh(), the very call that path skips -- and gates itself on `blended`. And
            // resolveSurface()'s own once-per-token warning sets are still filled from here for the
            // ENTITY's token, which a multi-part mesh whose parts all name their own materials
            // resolves nowhere else.
            const SandboxApp::ResolvedSurface rsEntity = self.resolveSurface(d.material);

            // THE SEAM, and it is one line because the design made it one: a posed entity's
            // vertices live in a DIFFERENT MeshHandle sharing this one's index buffer, so
            // substituting the handle reaches every pass at once. Zero means "not posed", never
            // "not drawn".
            rhi::MeshHandle mesh = d.baseMesh;
            // Set true only by the GPU per-cluster path below (AVER_MODULE_TRIFACTOR only) when it
            // actually dispatches this instance's geometry -- declared unconditionally, like `mesh`
            // above, so EntityDecision::colourAlreadyDrawn can be answered regardless of the module.
            bool clusterDispatched = false;
#if AVER_MODULE_TRIFACTOR
            f32 col[4] = {rsEntity.look.col[0], rsEntity.look.col[1], rsEntity.look.col[2],
                          rsEntity.look.col[3]};
            f32 metallic = rsEntity.look.metallic, roughness = rsEntity.look.roughness;
            const bool blended = rsEntity.look.blended;
            // The scene test paints its two entities so a probe can tell which it is looking
            // at. Only ever active behind --skin-scene-test. IT ONLY EVER REACHED THE CLUSTER
            // DISPATCH: the per-draw emitter re-derived `col` from each draw's own material and
            // never saw this override, and the library's resolveDrawLook does the same, so moving
            // it next to its one reader changes nothing and stops it reading like a scene-wide
            // recolour that quietly is not one.
            if (self.skinScene_) {
                if (d.entity == static_cast<scene::Entity>(self.skinScene_->subjectEntity()))
                    aver::editor::SkinSceneTest::subjectColor(col);
                else if (d.entity == static_cast<scene::Entity>(self.skinScene_->referenceEntity()))
                    aver::editor::SkinSceneTest::referenceColor(col);
            }

            // Virtualized-geometry LOD selection (trifactor::ClusterAdapt/ClusterSelect), per-
            // LEVEL not per-cluster. Skipped for a skinned entity (its posed handle always wins)
            // and a mesh with no LOD ladder. NO CACHE: chooseLevelCached is a handful of float ops
            // touching no GPU resource, cheap enough to run fresh EVERY frame.
            // GPU per-cluster path (--lod-mesh-shader) wins over everything below when the mesh
            // has GPU cluster buffers AND the pipeline came up on this device: it dispatches the
            // geometry itself, so `mesh` is never substituted and drawMesh() is skipped
            // entirely. Falls through to the CPU per-cluster/per-level paths whenever it doesn't
            // apply (see ensureLodMeshPipeline's degrade comment).
            // ALSO EXCLUDED: a BLENDED instance, for the same reason -- it skips drawMesh() and
            // calls dispatchMeshClusters() directly, but drawMesh() is EXACTLY where the fixed
            // contract's opaque/blended split lives (sort, wait for the sky, depth-write off). A
            // cluster-dispatched glass pane would skip all of that and reach
            // voxiRenderer_.submit() unconditionally, straight back into the shadow/GI/TLAS
            // exclusion `blended` exists to enforce. Excluding it leaves a blended instance free to
            // still take the CPU per-cluster or discrete-LOD path (neither skips drawMesh()), or
            // the unmodified mesh -- every road ends at the ordinary drawMesh() call, the only
            // place that draws glass correctly.
            if (self.lodMeshShaderEnabled_ && self.lodMeshPipelineReady_ && !d.skinned &&
                d.haveWorldBox && !blended) {
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

                        // ClusterFrameCB (b4): budget CLAMPED above zero here, on the CPU, before
                        // upload -- ASMain does not re-clamp -- and the six frustum planes copied
                        // verbatim from the SAME Frustum::fromViewProj the CPU reference calls.
                        SandboxApp::ClusterFrameCB frameCb;
                        frameCb.budgetPx = std::max(self.lodErrorThresholdPx_, trifactor::kMinClusterBudgetPx);
                        frameCb.projScale = trifactor::projScale(view);
                        frameCb.worldScale = worldScale;
                        const trifactor::Frustum frustum = trifactor::Frustum::fromViewProj(view.viewProj);
                        static_assert(sizeof(frameCb.planes) == sizeof(frustum.plane),
                                      "ClusterFrameCB::planes must match trifactor::Frustum::plane byte for byte");
                        std::memcpy(frameCb.planes, frustum.plane, sizeof(frameCb.planes));

                        // PerObject (b1): the SAME 32-dword layout drawMesh() itself packs
                        // (world, base colour, metallic/roughness, then the frozen shading-model
                        // tail), so PSClusterMain's plainShadeSurface reads real, live values.
                        f32 consts[rhi::kObjectConstantDwords];
                        std::memcpy(consts, &wm.m[0][0], 16 * sizeof(f32));
                        std::memcpy(consts + 16, col, 4 * sizeof(f32));
                        consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
                        // THE THIRD WRITER OF gShadingModel, and the one a search for
                        // writeShadingConstants does not find: this path builds the PerObject
                        // block by hand rather than calling it. It hardcoded STANDARD, so a
                        // mesh drawn through the GPU cluster path ignored Unlit while every
                        // other path honoured it -- the same mode giving two answers depending
                        // on which pipeline happened to draw the geometry.
                        const u32 shadingModel = self.unlit_ ? 1u : 0u;   // AVER_MODEL_UNLIT / STANDARD
                        std::memcpy(consts + 24, &shadingModel, sizeof(shadingModel));
                        consts[25] = 0.04f; consts[26] = 1.0f; consts[27] = 0.0f;
                        consts[28] = consts[29] = consts[30] = consts[31] = 0.0f;

                        // CpuSpan::ClusterDispatch, AS A CpuNest AND NOT A SECOND steady_clock PAIR:
                        // the plain std::chrono pair this replaces (tDis0 in, c.dispatchMs += out)
                        // measured exactly the interval below, and that interval sits INSIDE the
                        // decide() callback -- which is to say inside whatever CpuSpan GameRender.cpp
                        // has open while it calls decide() (CpuSpan::WalkDecide's own comment: "the
                        // EntityDecision fill, options.decide(), chooseRoute, planEntityDraws"). Left
                        // as a bare steady_clock pair, the same microseconds would land in BOTH
                        // c.dispatchMs (read by the scene-walk log line below) AND WalkDecide's own
                        // bucket (read by the CpuTiming breakdown printed under it) -- two reports of
                        // the identical time under two different names, with nothing to say which one
                        // to believe when they inevitably drift apart under a different entity mix. A
                        // CpuNest instead SUSPENDS whatever span is currently open (WalkDecide) for
                        // exactly this scope, so its own time is excluded from WalkDecide's bucket and
                        // resumes it, restarting WalkDecide's clock, the instant this scope ends --
                        // see CpuNest's own comment in CpuTiming.hpp for why this is the one primitive
                        // in that header built for a region reachable from more than one enclosing
                        // phase. walk.dispatchMs (the log line's own number) is populated FROM this
                        // same facility further down, once per throttled print, rather than from a
                        // second manual accumulator that could disagree with it -- see that call site.
                        {
                            CpuNest clusterDispatchNest(CpuSpan::ClusterDispatch);
                            ctx->setPipeline(self.lodMeshPipeline_);
                            ctx->setBindingSet(gpu.bindingSet, 0);
                            // THE MATERIAL, ON THE CONTEXT -- why the foliage on this path drew black.
                            // setDrawBinding above records the material on the DEVICE, which only
                            // forwards it to the context from inside drawMesh(), never called here. So
                            // dispatchMeshClusters' table-1 binding was whatever an EARLIER draw left
                            // sticky -- Voxi's fallback set, whose metal-rough map is white -- so
                            // metallic came out 1, kdAlbedo came out 0, and the diffuse lobe vanished:
                            // black, even though every value measured correct for a DIFFERENT material.
                            if (rsEntity.matSet)
                                ctx->setDrawBinding(rsEntity.matSet, rsEntity.matConstants, rsEntity.matBytes);
                            ctx->setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
                            ctx->setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frameCb, sizeof(frameCb));
#if AVER_MODULE_VOXI
                            // b3: VoxiFrame, the SAME bytes Voxi's own scenePass binds at b4 for the
                            // ordinary path (see kClusterGiFrameRegister's comment on why this path
                            // can't reuse b4). Bound every draw rather than once per mesh -- a root CBV
                            // pointer set is cheap -- keeping shadowFactor()/coneTracedIndirect() valid
                            // even before Voxi finishes init() (giFrameConstants() returns an all-zero block, degrading the same way Voxi's own checks do).
                            ctx->setConstantBuffer(kClusterGiFrameRegister, self.voxiRenderer_.giFrameConstants(),
                                                   self.voxiRenderer_.giFrameConstantBytes());
#endif
                            ctx->dispatchMeshClusters(mesh, gpu.clusterCount);
                        }
                        clusterDispatched = true;

                        // DEFECT 2's FIX, in full: this instance's LIT pixels already came from
                        // dispatchMeshClusters above, so drawMesh() is skipped for it -- and
                        // IRenderFeature::submitDraw is called from EXACTLY ONE place in the
                        // engine, D3D12Device::drawMesh. Skipping drawMesh() therefore also skips
                        // submitDraw(), the ONLY way geometry reaches VoxiRenderer::draws_ -- so
                        // this instance never appeared in shadowPass, giShadowPass or
                        // voxelizePass. A tree that casts no shadow is not a cheaper tree, it's wrong.
                        //
                        // ANSWERED, NOT HAND-WRITTEN, NOW: EntityDecision::colourAlreadyDrawn is
                        // exactly this case, and drawWorld sends such an entity down the direct
                        // route it would otherwise have taken only when culled -- per planned
                        // draw, with hiddenFromOwner false (deliver() keys that on route.raster,
                        // which is true here), which is the same {false,false,false} route the
                        // deleted call passed by hand.

                        ++self.lodMeshShaderStats_.instancesTested;
                        self.lodMeshShaderStats_.clustersDispatched += gpu.clusterCount;

                        // Informational counters (LodMeshShaderStats): the SAME CPU reference
                        // over the SAME clusters and budget the GPU dispatch just used, sampled
                        // every 64 frames.
                        // GATED ON lodClusterStatsEnabled_ (--lod-cluster-stats), OFF BY DEFAULT --
                        // even with THE INSTANCE-LEVEL SHORTCUT below, the handful of instances the
                        // shortcut cannot prove (mixed-level, closest/largest-DAG trees) are
                        // expensive enough alone that this block cost ~1.1s on the sampled frame --
                        // and since 64 divides --frames 128 evenly, that frame is GUARANTEED to be
                        // this task's own benchmark's last, not a coincidence. A once-per-second
                        // stall for a log line no render pass reads is unacceptable.
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
            // entirely unchanged, still reachable and reproducing pre-existing behaviour when --lod-per-cluster is off.
            if (!clusterDispatched && self.lodPerClusterEnabled_ && !d.skinned && d.haveWorldBox) {
                if (const auto cit = self.meshClusterData_.find(d.meshId); cit != self.meshClusterData_.end()) {
                    const auto& cd = cit->second;
                    trifactor::View view;
                    view.eye = self.eye_;
                    view.viewProj = self.viewProj_;
                    view.viewportHeightPx = self.vpH_;
                    view.verticalFovRadians = radians(60.0f);
                    const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                    // THE INSTANCE-LEVEL SHORTCUT (ClusterAdapt.hpp; TrifactorTest proves it
                    // against the real scan): if provablySingleLevelCut can PROVE the real
                    // O(all-DAG-clusters) scan would select exactly the whole level it names, draw
                    // that level's already-resident ladder handle directly, skipping the
                    // copy/transform/scan. Falls through to the real scan when the proof doesn't
                    // hold.
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
                    // Mesh-local -> world, once per instance per frame, for EVERY cluster of the
                    // mesh's whole DAG at once: the local cut test needs every cluster's world-space
                    // sphere against this frame's camera. Same uniform-scale approximation the per-level telemetry above uses.
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

                    // Sort for a cheap, order-independent "did the cut change since last frame"
                    // comparison. The cut is expected to be STABLE frame to frame (the camera moves
                    // continuously, not by a full LOD jump), so this is a cache hit most frames once settled.
                    std::vector<u32> selectedIds = cr.drawnIds;
                    std::sort(selectedIds.begin(), selectedIds.end());

                    auto& cache = self.clusterCutCache_[d.entity];
                    cache.lastUsedFrame = self.lodClusterFrame_;
                    if (selectedIds != cache.selectedIds || cache.handle == 0) {
                        // REBUILD: concatenate every selected cluster's precomputed expanded
                        // GLOBAL index list into ONE fresh index buffer, against the SAME shared
                        // vertex array every level of this mesh uses -- one draw call for the whole
                        // mixed-LOD cut, avoiding ExecuteIndirect entirely. Measured, not guessed: real wall time and real upload byte counts.
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
                            // A DEPTH PROXY FOR THE CUT, POINTING AT THE STABLE SOURCE MESH.
                            //
                            // The cut handle is born fresh every time the selected cluster set
                            // changes, and under a rotating camera that is nearly every frame --
                            // cluster selection is frustum- and cone-culled per view, so the
                            // comment above ("expected to be STABLE frame to frame ... a cache hit
                            // most frames once settled") holds for a still camera and fails for a
                            // moving one. Without an entry here depthProxyLookup returns 0, Voxi's
                            // submit() falls back to the cut handle, and the four depth-only
                            // consumers -- the shadow cascades, the GI shadow map, voxelisation and
                            // the TLAS -- all follow a handle that is destroyed and recreated
                            // continuously. MEASURED: that is what churned the GI rebuild gate's
                            // draw key (the delta log named consecutive handles arriving and
                            // leaving), and it is where "createBlas for destroyed mesh" comes from.
                            //
                            // The SOURCE mesh, not the cut, is the right answer for all four: none
                            // of them wants a view-dependent cluster subset. A depth-only pass wants
                            // cheap, complete, stable geometry, and voxelisation in particular
                            // resolves the world into 512^3 cells where a cluster-level cut is far
                            // below one voxel. The cut still draws the colour pass, unchanged.
                            //
                            // FIXED (a real defect, found chasing a /W4 C4244 on this line, not a
                            // cosmetic one): this read `mr->mesh`, the mesh's u64 CONTENT-HASH asset
                            // id (the key meshClusterData_/meshLods_ are looked up by, a few lines
                            // above) -- not a handle at all. depthProxy_ maps rhi::MeshHandle ->
                            // rhi::MeshHandle (u32), so that u64 was truncated into whatever 32 bits
                            // happened to survive, which depthProxyLookup would then have handed to
                            // the four depth-only consumers this comment already names as a mesh
                            // handle. `mesh` -- still the pre-cut, SOURCE handle at this point,
                            // exactly what the paragraph above asks for -- is the value that belongs
                            // here.
                            self.depthProxy_[newHandle] = mesh;
                        }
                        // newHandle == 0 (empty cut this frame, or the device refused): keep
                        // whatever handle the cache already had (fail-safe), or fall through to the
                        // LOD-0 whole-mesh handle `mesh` already holds for the very first frame.
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
                                                                      // encloses the box exactly, same conservative shape ClusterSelect's own
                                                                      // sphere tests assume.
                    trifactor::View view;
                    view.eye = self.eye_;
                    view.viewProj = self.viewProj_;
                    view.viewportHeightPx = self.vpH_;
                    view.verticalFovRadians = radians(60.0f);   // matches the literal at this
                                                                 // frame's own proj build, above
                    const u32 level = trifactor::chooseLevelCached(
                        ladder.errorCm, sphereCenter, sphereRadius, self.lodErrorThresholdPx_, view);
                    mesh = ladder.handles[level];

                    ++self.lodStats_.instancesTested;
                    if (level > 0) ++self.lodStats_.levelCollapsed;
                    self.lodStats_.trianglesBeforeLod0 += ladder.triCounts.front();
                    self.lodStats_.trianglesAfterLevel += ladder.triCounts[level];

                    // Informational cluster-cull telemetry for the CHOSEN level only -- real,
                    // tested, but not subtracted from trianglesAfterLevel: this slice draws the
                    // whole chosen level.
                    // ladder.clusters[level] holds MESH-LOCAL bounds; view.viewProj/frustum are
                    // WORLD space, so a working copy is transformed by this instance's world
                    // matrix first. Radius/axis use a UNIFORM-scale approximation -- fine for an
                    // INFORMATIONAL counter that never reaches the draw call.
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
            // WHICH FEATURE OWNS THIS ENTITY'S VERTICES -- posedHandle(), the SAME helper the
            // direct route above already called, so the seam agrees with itself instead of two
            // near-identical hand-written copies: skinning checked first (nothing forbids
            // CSoftBody on an already-skinned mesh, so asking skinning first makes the collision
            // deterministic), soft body only filling in where it declined.
            //
            // NOT FOR A CLUSTER-DISPATCHED ENTITY, which is not a new exception but the old one
            // written down. The hand-written submission this replaces was planned from
            // sceneMeshHandle and said so in as many words ("`mesh` still equals `sceneMeshHandle`
            // here -- no LOD/skin substitution has run yet at this point in the walk"), because it
            // ran ABOVE this line. chosenMesh is what planEntityDraws plans from, so handing it a
            // substituted handle here would collapse a multi-part cluster mesh's per-part split to
            // one draw the moment a soft body claimed it -- planEntityDraws drops the split for
            // any handle the split was not cut from.
            if (!clusterDispatched)
                if (const rhi::MeshHandle substituted = self.posedHandle(d.entity)) mesh = substituted;
            d.chosenMesh = mesh;
            // The GPU per-cluster path already dispatched this instance's geometry, so the raster
            // drawMesh() must not run for it -- but drawMesh() is also the only path to
            // IRenderFeature::submitDraw, so the entity takes the direct route instead and still
            // counts as drawn. See EntityDecision::colourAlreadyDrawn.
            d.colourAlreadyDrawn = clusterDispatched;

#if AVER_MODULE_VOXI
            // F6/F4: the ENTITY-level half of depth-prepass eligibility. BLENDED IS NOT CHECKED
            // HERE: it is a PER-DRAW question, answered inside drawWorld from each planned draw's
            // own resolved look, because a mesh with an opaque trunk and a translucent leaf part
            // gets it right per part where one entity-level flag could not -- and because
            // setNextDrawPrepassed is AUTO-CONSUMED by the very next drawMesh() rather than sticky
            // (RHI.hpp), so one call before a multi-part loop would cover part 0 alone. The three
            // tests below restate the prepass call's own prepassDecide exclusions -- skinned, GPU
            // cluster dispatch, CPU per-cluster -- and still have to: an entity that walk skipped
            // wrote no depth, so asking the LessEqual/no-write pipeline for it here would test
            // against whatever depth was already there.
            {
                bool eligible =
                    c.engine->device()->depthPrepassEnabled() && !clusterDispatched && !d.skinned;
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

        // ONCE PER ENTITY THE WALK ACTUALLY DELIVERED, on either route. `raster` is what separates
        // the two claims, and the editor needs the second one: an entity delivered on the DIRECT
        // route was culled or owner-hidden, so it reached Voxi's shadow/GI/TLAS submission and
        // nothing else. Latching the selection outline off "delivered" alone would start outlining
        // off-screen objects.
        auto colourDelivered = [](scene::Entity ent, u64 meshId, rhi::MeshHandle chosenMesh,
                                  const Mat4& world, bool raster, void* user) {
            ColourWalk& c = *static_cast<ColourWalk*>(user);
            SandboxApp& self = *c.self;
            if (!raster) {
                // 3B's per-frame evidence that a culled multi-part entity still yields N draws
                // instead of collapsing to slot 0's single one. Banked HERE, not in onDirectDraw,
                // because "was this entity culled or merely owner-hidden" is a per-ENTITY question
                // and the draws carry no answer to it. Never reached for an entity the
                // angular-size floor held back (drawWorld does not fire this for one), which is the
                // same accounting the deleted branch's `if (angularFloorOk)` wrapper produced.
                if (c.entityCulled) {
                    c.culledDraws += c.entityDirectDraws;
                    if (c.entityDirectDraws > 1) ++c.culledMultiPart;
                }
                return;
            }
            // EVERY SELECTED ENTITY, NOT ONLY THE ANCHOR. This kept one Mat4 and one mesh id,
            // so a multi-selection was highlighted in the Outliner tree and invisible in the 3D
            // view -- which is where the objects are. Selecting five props and dragging them
            // showed an outline on one of the five.
            if (self.sel_ == SandboxApp::kSelScene &&
                (ent == self.selEntity_ || self.multiIsSelected(ent))) {
                self.selectionOutline_ = world;
                self.selectionMesh_ = chosenMesh;
                self.selectionMeshId_ = meshId;
                self.hasSelection_ = true;
                // The anchor stays in the scalars above (other code reads them); the rest
                // accumulate here. Cleared with hasSelection_ at the draw site, so a stale
                // entry cannot outlive the frame that produced it.
                self.selectionOutlines_.push_back({world, meshId});
            }
        };

#if AVER_MODULE_VOXI
        // ONCE PER DRAW ON THE DIRECT ROUTE, immediately after the Voxi submit it made. THE PATH
        // TRACER NEEDS THE SAME OFF-SCREEN GEOMETRY AND THIS IS ITS ONLY WAY IN: PtSceneView::
        // submitDraw is otherwise reached only through drawMesh(), which this route exists to skip,
        // so before it was fed from here the path-traced view traced a scene holding only what the
        // camera could see (and no cluster-dispatched instance at all): no roof overhead, no wall
        // behind the camera. Measured on PTTest NewSponza: Voxi's TLAS held 400 instances while the
        // path tracer "re-armed on 154", with 73 entities frustum-culled.
        //
        // A SEPARATE SINK FROM options.voxiRenderer, deliberately, and this is gotcha 3: the two
        // are independent in drawWorld, so the path tracer still gets fed on a frame where no Voxi
        // feature is attached. Gating one feature's delivery on another's presence is how the path
        // tracer went blind in the first place.
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

        // THE EDITOR'S SENTENCE, THROUGH THE EDITOR'S OWN THROTTLES -- the same arrangement the
        // depth-prepass call above already uses, and for the same reason. drawWorld owns the "once
        // per material token, ever" bookkeeping (GameRender.hpp's SurfaceWarning) because two hosts
        // keeping two sets is how the counts drift; the wording is the host's, and the library's
        // default says "[Game]" and names no editor directory. Re-running resolveSurface() is what
        // keeps the line byte-identical to the one this walk printed before the move: it reaches
        // whichever of warnDeadMaterialHandle and the missing-.ocmat warning its own ladder decides
        // on, filling the SAME function-local static sets, so `kind` goes unread and the duplicated
        // resolve costs one lookup per token per process.
        auto colourWarn = [](i32 mat, aver::game::SurfaceWarning kind, void* user) {
            (void)kind;
            (void)static_cast<ColourWalk*>(user)->self->resolveSurface(mat);
        };

        // THE TWO DIAGNOSTICS THIS WALK WOULD OTHERWISE LOSE. Each of these checks cost a day of
        // bisection before it said anything at all, so the library reports the drop and the editor
        // writes the sentence -- see DrawSkipReason. Not throttled on the library's side because
        // the two reasons want different keys, which is exactly the split these two sets already
        // keep.
        auto colourSkipped = [](scene::Entity ent, u64 meshId, aver::game::DrawSkipReason reason,
                                void* user) {
            SandboxApp& self = *static_cast<ColourWalk*>(user)->self;
            if (reason == aver::game::DrawSkipReason::NotVisible) {
                // A MESH IS NAMED AND THE VISIBLE BIT IS CLEAR, which is NOT ordinary -- it is the
                // zero-fill trap. World::addComponent hands back zeroed storage and
                // kMeshRendererVisible is positive-sense, so a renderer attached directly is
                // attached, correct, and invisible; GraphComponentTree.cs:100-103 documents the
                // same trap on the C# side and dodges it by going through Entity.SetVisible.
                // ONCE PER ENTITY, because this is a per-entity per-frame walk and an unthrottled
                // warning floods the log.
                if (self.undrawnInvisible_.insert(static_cast<u64>(ent)).second)
                    AVER_WARN("[Sandbox] entity {} names mesh id {} but its kMeshRendererVisible "
                              "bit is clear, so the scene walk skips it and it draws nothing. A "
                              "component attached directly arrives zero-filled -- attach through "
                              "Entity.SetVisible (EnsureMeshRenderer), which seeds the bit.",
                              static_cast<u64>(ent), meshId);
                return;
            }
            // AN ID THAT RESOLVES TO NOTHING. Said ONCE PER ID rather than per entity: many
            // entities can name the same missing mesh, and it is the id that identifies the
            // fault, not the entity that happened to reach it first.
            //
            // THE ID IS PRINTED RAW AND THAT IS NOT LAZINESS. meshPathById_ is populated by
            // loadProjectMeshes only for meshes that LOADED, so it is empty for exactly the
            // ids that land here -- there is nothing to translate with. fnv1a64 of the
            // authored path is what to grep the .ocgraph/.ocmap for.
            if (self.undrawnMissingMesh_.insert(meshId).second)
                AVER_WARN("[Sandbox] mesh id {} (named by entity {}) is not in content_'s meshes, "
                          "so every entity naming it draws nothing. Built-in primitives are "
                          "seeded at startup and .ocmesh files are registered by "
                          "loadProjectMeshes from the project's content root -- an id that is "
                          "missing was never loaded under the string that was authored.",
                          meshId, static_cast<u64>(ent));
        };

        aver::game::DrawWorldOptions copt;
        // The possessed first-person pawn, so the ancestor walk this used to run by hand runs
        // inside drawWorld instead. A COMP tree can nest, so the pawn's body may be several hops
        // below it; World::setParent already refuses a cycle, so that walk terminates.
        copt.ownerHideRoot = firstPersonPawn_;
        // The editor's "[Sandbox] scene-render: N spawned CMeshRenderer entities drawn, ..." line
        // below is worded differently from the library's on purpose -- it names a concept ("spawned
        // CMeshRenderer entities") a packaged game has no vocabulary for and its "culled"
        // deliberately covers occlusion as well as the frustum. SceneDrawStats' own comment states
        // why the two sentences cannot be unified; this is the switch it describes.
        copt.suppressLog = true;
        copt.decide = colourDecide;
        copt.onEntityDelivered = colourDelivered;
        copt.onSurfaceWarn = colourWarn;
        copt.onSkipped = colourSkipped;
        copt.user = &walk;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        copt.onVisit = colourVisit;
        // EMPTY MEANS "NO REORDERING", exactly as the deleted loop's `occlusionOrder_.empty() ? oi
        // : occlusionOrder_[oi]` meant it -- occlusionOrder_ is resized to n when culling runs and
        // cleared when it idles, so a null order here is world order, which is what that ternary
        // fell back to.
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
        // BOTH MODULES, not just PBR: the system lives on voxiRenderer_, which is itself declared
        // only under AVER_MODULE_VOXI (SandboxApp.hpp:3760). It is the same MaterialSystem
        // resolveSurface() binds out of, so each part's descriptor table and constants reach
        // drawMesh() exactly as they did when this file emitted the draw itself.
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        colourMaterials = &voxiRenderer_.materials();
#endif
        // THIS FRAME'S OWN, not a member: SceneDrawStats' `last*` trio is maintained even under
        // suppressLog, but it is updated before the caller gets control back, so it cannot answer
        // "did this change since last frame" for anyone but drawWorld itself -- which is exactly
        // why SandboxApp keeps lastSceneDrawn_/lastSceneCulled_/lastSceneOwnerHidden_ of its own
        // and compares against those below (SceneDrawStats' own comment says so outright).
        aver::game::SceneDrawStats colourStats;
        aver::game::drawWorld(*e.device(), viewProj_, content_, colourStats, colourMaterials,
                              skinnedScene_.get(), copt);
        // Closes "raster scene draws" HERE, at the end of the draw walk, rather than letting it
        // run to the end of the enclosing block -- the occlusion reporting and pass-2 fallback
        // below are not scene shading and do not belong in the number.
        rasterScope.reset();
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        // The pass-2-empty fallback: occlusionVisible_ still needs a fresh answer for every
        // entity even when there was nothing left to gate, or a wall walked in front of a
        // previously "visible" entity would never be discovered and it would stay drawn forever.
        if (occlusionRuns) occlusionBuildAndTest();
        // Reported on the SAME "power-of-two frame count" cadence D3D12Device's own GPU-timing
        // report uses, so the culled-count line and the "HZB build"/"HZB test" spans it names land
        // at a frame this app was already printing at, not a second unrelated rhythm.
        // F8: occlusionRuns, not occlusionCullEnabled_ && occluder_ -- occlusionReportFrames_ only
        // INCREMENTS when occlusionRuns is true (the pass-2-empty fallback just above, and the
        // pass-1/pass-2-boundary call inside the walk, both already switched), so gating the PRINT
        // on the wider occlusionCullEnabled_ && occluder_ condition would keep re-satisfying this
        // power-of-two check every single frame culling sits idle (F8's whole point) once the
        // count freezes on a 2^n-1 value, instead of printing once and stopping like it does today.
        if (occlusionRuns && occlusionReportFrames_ &&
            (occlusionReportFrames_ & (occlusionReportFrames_ + 1)) == 0) {
            const f64 pct = occlusionTestedAccum_ ? 100.0 * static_cast<f64>(occlusionCulledAccum_) /
                                                    static_cast<f64>(occlusionTestedAccum_) : 0.0;
            AVER_INFO("[Occlusion] {} of {} tested entities culled ({:.1f}%) over {} frame(s)",
                      occlusionCulledAccum_, occlusionTestedAccum_, pct, occlusionReportFrames_);
            // 3B: THIS FRAME's own snapshot (unlike the lifetime accumulators just above) -- the
            // concrete evidence F1-F4 actually deliver what they promise: a culled multi-part
            // entity still yields N draws here, never one collapsed to slot 0's material.
            AVER_INFO("[Occlusion] this frame: {} entities culled (frustum or occlusion) delivered "
                      "as {} draws ({} multi-part)",
                      colourStats.culled, walk.culledDraws, walk.culledMultiPart);
            // Folded into the same cadence rather than its own: a staleness-detector trip is
            // rare enough that a separate periodic line would mostly print zero, and this way it
            // rides the report a reader is already watching.
            if (occlusionStaleReadbacks_)
                AVER_INFO("[Occlusion] {} of those {} frame(s) had a readback more than one call "
                          "stale (see the one-time warning above) -- culling was fully disabled "
                          "on those frames, not merely more conservative",
                          occlusionStaleReadbacks_, occlusionReportFrames_);
            // SPLITS occlusionStaleReadbacks_ BY WHICH of the two independent checks failed --
            // IOcclusionCuller::boxIdentityChurnCount() isolates the IDENTITY dimension (the
            // caller's identityKey disagreed even though the GPU generation stamp matched),
            // separate from the TIMING dimension (the generation stamp itself missed by more
            // than one call). Occlusion.hpp's own comment on boxIdentityChurnCount() names
            // exactly this gap ("a caller wanting to separate... needs a number to diff against,
            // not just an instantaneous bool"), and no caller read it before this. THIS SPLIT IS
            // WHAT FOUND THE BUG hashIdentityKey()'s own comment (OcclusionMath.hpp) documents:
            // before that fix, identityChurn tracked occlusionStaleReadbacks_ exactly 1:1 under
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
                // THE FACILITY'S OWN SNAPSHOT, taken ONCE for this whole throttled print rather than
                // once per line below -- collectCpuTiming() only reads the last WINDOW the facility
                // already froze (CpuTimingReport's own comment), so a second call a few lines down
                // would not see anything newer, only cost a second pass over kCpuSpanCount nodes for
                // nothing. Reading it here, before the log line that names cluster-dispatch time,
                // is also what lets walk.dispatchMs (below) come from this same snapshot instead of
                // a manual accumulator that could disagree with it.
                const CpuTimingReport cpuReport = collectCpuTiming();
                // walk.dispatchMs SOURCED FROM THE FACILITY -- see the CpuNest conversion at the
                // dispatch call site for why a second, independent steady_clock pair was removed
                // rather than kept alongside it. `nodes` is either empty (nothing published yet, or
                // this build/process cannot report at all -- CpuTimingReport::supported's own
                // comment) or sized exactly kCpuSpanCount (collectCpuTiming's own contract), so the
                // emptiness check alone is enough to index ClusterDispatch safely. Left at its
                // constructed 0.0, not stale, when there is nothing to read yet.
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
                // THE PER-BUCKET TREE, PRINTED DIRECTLY BENEATH THAT LINE ON PURPOSE: the two are
                // meant to be cross-checked against each other, not read in isolation. walkMs above
                // is THIS SINGLE FRAME's own steady_clock bracket; the tree below averages over
                // CpuTiming.hpp's kCpuTimingWindowOccurrences occurrences (a window, stated in the
                // tree's own header line), so the two are not expected to match to the decimal --
                // only to stay in the same neighbourhood, which is itself the check: a tree total
                // wildly different from the single-frame number above says the workload just changed
                // shape, not that either instrument is lying. This is also the split the whole stage
                // exists for -- see this bracket's own comment at tWalk0 and CpuTiming.hpp's top-of-
                // file comment for the sweep (250/1000/4000/16000 entities, ~0.45us/entity) that
                // showed the single "6.8ms in the rest" number could not say which of several
                // candidate fixes it would pay for. formatCpuTiming already emits its own "not
                // supported" / "no window yet" sentences when cpuReport has nothing to show, so no
                // extra guard is needed here for either case.
                formatCpuTiming(cpuReport,
                                 [](const std::string& line) { AVER_INFO("[Sandbox] {}", line); });
#if AVER_MODULE_VOXI
                // M2(c): the CPU cost of Voxi's acceleration-structure per-draw loop on its last
                // rebuild (VoxiRenderer::lastAccelBuildCpuMs, C-2), printed at the SAME widening
                // cadence as the scene-walk line directly above so the two land in the log
                // together as one picture of where a frame's CPU time goes. Reads 0.0 until the
                // first GI rebuild has actually reached the per-draw loop -- see that method's own
                // comment (VoxiRenderer.hpp) for why "no build yet" and "a build that measured
                // zero" cannot be told apart from this number alone.
                AVER_INFO("[Sandbox] Voxi acceleration-structure draw loop {:.2f} ms CPU (last build)",
                          voxiRenderer_.lastAccelBuildCpuMs());
#endif
            }
            ++sceneWalkReports_;
        }
        // READ BACK OUT OF SceneDrawStats, not counted here: DrawWorldOptions::suppressLog above
        // stops the library writing its own "[Game] scene-render:" line precisely so the editor can
        // write this one from the same three numbers. The comparison is against SandboxApp's own
        // previous-frame copies, not colourStats' `last*` trio, because drawWorld updates that trio
        // before returning and so cannot answer "did this change since last frame" for anyone but
        // itself (SceneDrawStats' own comment).
        if (colourStats.drawn != lastSceneDrawn_ || colourStats.culled != lastSceneCulled_ ||
            colourStats.ownerHidden != lastSceneOwnerHidden_) {
            // F4 (occlusion-fix-plan.md): `culled` now counts an occlusion-culled entity too, not
            // only a frustum-culled one -- previously the occlusion branch incremented no counter
            // at all, so occlusion's own contribution was invisible here. Relabelled from
            // "frustum-culled" to plain "culled" to match; see chooseRoute()'s own priority
            // ordering for which counter an entity that is BOTH culled and owner-hidden lands in.
            AVER_INFO("[Sandbox] scene-render: {} spawned CMeshRenderer entit{} drawn, {} culled, {} owner-hidden",
                      colourStats.drawn, colourStats.drawn == 1 ? "y" : "ies", colourStats.culled,
                      colourStats.ownerHidden);
            lastSceneDrawn_ = colourStats.drawn;
            lastSceneCulled_ = colourStats.culled;
            lastSceneOwnerHidden_ = colourStats.ownerHidden;
        }
#if AVER_MODULE_TRIFACTOR
        // Greppable per the brief's requirement: `[LOD-SELECT]`, only when something in the tuple
        // changed. trianglesBeforeLod0 vs trianglesAfterLevel actually predicts frame time; the
        // cluster-cull counters are real telemetry against the CHOSEN level's meshlets, informational only (ClusterAdapt.hpp).
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
        // `[LOD-CLUSTER]`, greppable. `distinctLevelsMax` is the task brief's proof of the
        // feature: if it never exceeds 1, every instance's cut collapsed to one level and this is
        // discrete LOD with extra steps. `rebuilds`/`hits`/`rebuildMs` are the real CPU-assembly
        // cost. `instancesShortcut` is THE INSTANCE-LEVEL SHORTCUT's count -- NOT included in
        // `instancesTested`/`clustersTested`, so their sum is this frame's true instance total.
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
    // IT USED TO BE A drawMesh AND THAT IS WHY THERE WAS NO OUTLINE. drawMesh is gated on
    // sceneSuppressed(), and VoxiRenderer::suppressesScene() is true whenever ray-driven primary
    // visibility is on -- this engine's STANDING DEFAULT. So the outline was issued into nothing
    // for the mode everyone actually uses. Measured, rather than reasoned: a bounded capture with
    // the interactive gate lifted and an entity selected produced exactly ONE orange-ish pixel
    // anywhere in the viewport, and it was a leaf vein. The device log states the same thing --
    // "2 render features claim the whole scene ... The rasteriser draws NOTHING while this holds."
    //
    // drawLines is gated on suppressesWholeFrame(), which VoxiRenderer deliberately keeps FALSE
    // for ray-driven so that chrome survives, and which D3D12Device::drawLines explains: "Gizmos
    // and wireframes belong in a ray-driven viewport as much as in a rastered one, and they
    // depth-test against the real depth the ray pass writes." The grid, gizmo, nav mesh and
    // collider overlay were already on that path; the outline was the one piece of chrome that
    // was not.
    //
    // STILL INTERACTIVE-ONLY (maxFrames_ == 0), and now for a different reason than before. The
    // old reason -- that drawMesh hands every draw to every feature's submitDraw before honouring
    // suppressesScene, so the path tracer accumulated an oversized solid copy of the selection --
    // does not apply to drawLines, which no feature captures. The reason it stays is the render
    // gates: they run bounded, they do NOT pass --no-editor-chrome, and the editor starts with
    // sel_ = 1, the placeholder Cube. Dropping this gate would put an orange outline in every gate
    // image and move all twenty of them.
    //
    // NOT SCALED. The old shell was inflated 1.02-1.12x to escape z-fighting with the surface it
    // copied; lines depth-test against the ray pass's own depth and sit exactly on the geometry,
    // so growing them would only lift the outline off the object it is meant to trace.
    //
    // anyPlayActive() IS DECLARED WITH startPlay/stopPlay INSIDE AVER_MODULE_FRAMEWORK
    // (SandboxApp.hpp), so a framework-less build has no notion of a play-in-editor session at
    // all and can never be mid-play. Resolved once here, rather than called again at the player
    // start marker's own gate below, so the two conditions that comment already promises stay in
    // lockstep instead of drifting if only one call site got a guard.
#if AVER_MODULE_FRAMEWORK
    const bool anyPlaying = anyPlayActive();
#else
    const bool anyPlaying = false;
#endif
    if (hasSelection_ && maxFrames_ == 0 && !anyPlaying && !noEditorChrome_) {
        // One drawLines per selected entity. selectionOutlineLines caches per MESH id, so N
        // copies of the same asset share one line buffer and this costs N draws, not N buffers.
        for (const auto& [xf, meshId] : selectionOutlines_)
            if (const rhi::LineHandle lh = selectionOutlineLines(e, meshId))
                e.device()->drawLines(lh, &xf.m[0][0]);
    }
    hasSelection_ = false;
    selectionOutlines_.clear();

    // ---- THE PLAYER START'S MARKER ----
    // Queued every frame rather than kept as scene state, matching how the rest of the editor's
    // viewport chrome already works.
    // HIDDEN BY THE SAME anyPlaying THE OUTLINE ABOVE COMPUTES, deliberately, so the two can't
    // drift apart.
    // NOT GATED ON maxFrames_, unlike the outline: the outline responds to a click, meaningless in
    // a bounded run; the marker is part of what the level LOOKS like.
#if AVER_MODULE_SCENE
    if (viewportIconsReady_ && !noEditorChrome_ && !anyPlaying &&
        playerStart_ != scene::kInvalidEntity) {
        const scene::World& psw = scene::World::instance();
        if (psw.valid(playerStart_)) {
            // Raised by its own half-height so the pin's TIP lands on the marker's origin
            // rather than its middle -- the quad is centred on the position it is given, and
            // the artwork points down (see scripts/make-editor-icons.py).
            const Vec3 at = psw.localTransform(playerStart_).position;
            viewportIcons_.addIcon(Vec3{at.x, at.y, at.z + kPlayerStartIconHalfSize},
                                   kPlayerStartIconHalfSize, playerStartIcon_);
        }
    }
#endif

    e.device()->setWireframe(false);
    // Cleared with wireframe, and for the same reason: both are sticky device state, so leaving
    // unlit on here would flatten the grid, the gizmo and every other piece of chrome drawn
    // after the scene.
    e.device()->setUnlit(false);
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
    //
    // There was no collider overlay, no toggle and no wireframe anywhere in this editor, so
    // "does the collision match the art" was answerable only by dropping something on it and
    // watching what happened. The drawLines infrastructure was already here for the navmesh.
    //
    // REBUILT EVERY FRAME, deliberately, unlike the navmesh's cached overlay: a dynamic body
    // moves, so a cached mesh would draw last frame's boxes. It costs one line mesh per frame
    // while the toggle is on and nothing at all while it is off.
    if (showColliders_ && !noEditorChrome_) rebuildColliderOverlay(e);
    if (showColliders_ && !noEditorChrome_ && colliderMesh_) {
        const Mat4 cm = Mat4::identity();   // world space already, same as the navmesh
        e.device()->drawLines(colliderMesh_, &cm.m[0][0]);
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
    // lastSceneCulled_ is declared only under AVER_MODULE_SCENE (it counts what the scene-render
    // pass just culled), but skinScene_->tick() is called unguarded -- SkinSceneTest degrades to a
    // no-op with SCENE off, so 0 is the value lastSceneCulled_ would hold anyway.
    if (skinScene_ && !skinScene_->finished())
#if AVER_MODULE_SCENE
        skinScene_->tick(e, vpX_, vpY_, vpW_, vpH_, static_cast<u32>(lastSceneCulled_ < 0 ? 0 : lastSceneCulled_));
#else
        skinScene_->tick(e, vpX_, vpY_, vpW_, vpH_, 0u);
#endif
    captureCheck(e);
    // AFTER captureCheck, not before: both touch the device's single capture slot, and this
    // ordering is what lets serviceViewportScreenshot's own request skip only the one frame
    // captureCheck requests on (see its comment) rather than also the frame after -- by the time
    // this call could take the slot, captureCheck's read of ITS OWN request for this tick has
    // already happened and capDone_ has latched it out of the slot for the rest of the run.
    serviceViewportScreenshot(e);
    lumaSweepCheck(e);
    resizeCheck(e);
    gpuTimingCheck(e);
    rayProbeCheck(e);
#if AVER_MODULE_SYNAPSE
    navBakeCheck(e);
#endif
    // ROLLED HERE, AT THE END OF THE FRAME, and the position is the whole contract: InputState's
    // own header says newFrame() must come BEFORE pumpEvents(), since pressed()/released() report
    // edges since the last newFrame(). Rolling it at the top of onUpdate would discard the frame's
    // own edges -- "the game ignores single taps". Same placement, same reasoning, as GameApp.
    input_.newFrame();
}

// VIEWPORT SCREENSHOT (File > Take Screenshot, F9). See the member block above requestViewportScreenshot's
// own declaration in SandboxApp.hpp for the state machine's shape. request...() only sets the latch --
// the actual requestCapture() call has to happen from inside onRender, where vpX_/vpY_/vpW_/vpH_ are
// current and the device is guaranteed to exist, neither of which a keybind handler or a menu click
// running earlier in the frame can assume.
void SandboxApp::requestViewportScreenshot() {
    // A second press while one is already in flight is a no-op, not a queued second shot: the
    // device has one capture slot, and racing two requests through this state machine would have
    // the later one's requestCapture() silently cancel the earlier one's pending read.
    if (viewportShotState_ != 0) return;
    viewportShotState_ = 1;
}

// Runs once a frame, right beside captureCheck(e) in onRender -- see the comment at that call site
// for why the ordering there (this AFTER captureCheck) is what makes the single frame-number check
// below sufficient.
void SandboxApp::serviceViewportScreenshot(Engine& e) {
    if (viewportShotState_ == 0) return;

    if (viewportShotState_ == 1) {
        // --luma-sweep/--firefly-metric request a fresh capture every tick for the WHOLE run (see
        // captureCheck's own comment) -- there is no frame where the slot is safely ours to take.
        if (lumaSweep_ || fireflyMetric_) return;
        // The one frame captureCheck itself calls requestCapture() on, in a bounded run: taking the
        // slot first would have captureCheck's read land OUR pixel under the probe coordinates it
        // remembers asking for, reporting a wrong pixel as the probe's own. Leave the latch set --
        // requestViewportScreenshot() already refused to re-arm it -- and try again next frame.
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

    // state 2: awaiting the image. ONE FRAME OF LAG IS INHERENT to requestCapture() -- same as
    // captureCheck's own comment on the same API -- it is serviced inside present(), so a request
    // made while handling frame f is not ready to read back until the NEXT tick.
    if (e.time().frame <= viewportShotFrame_) return;
    ++viewportShotTries_;

    std::vector<u8> img; u32 iw = 0, ih = 0;
    if (e.device()->getFrameImage(img, iw, ih) && iw && ih) {
        // Crop to the 3D viewport alone, not the whole backbuffer: vpX_/vpY_/vpW_/vpH_ are already
        // backbuffer pixels (the same convention setViewportRect documents), so this is a straight
        // rect against the image -- but CLAMPED, not trusted, because the viewport can resize or
        // collapse to nothing between the request going out and the image landing (a panel drag, a
        // window resize), and an out-of-range rect must fall back to the whole frame rather than
        // read out of bounds.
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

        // <project>/Saved/Screenshots -- the same `Saved/` convention autosavePathFor's own comment
        // documents for recoverable, non-authored state. No project open falls back to the
        // executable's own directory, the same fallback CrashReport.cpp uses for its Saved/Crashes
        // when nothing else names a place to put it.
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

        // THE LOG LINE IS THE ANSWER WITHOUT ImGui, THE TOAST IS THE ANSWER WITH IT. notifyOutcome
        // pushes onto editor::notifications(), which only the notification overlay ever drains, so
        // it is declared and defined behind AVER_WITH_IMGUI along with the rest of the content
        // browser -- and the caller is guarded instead, exactly as notifyOutcome's own comment says
        // of --import's deferred handshake. A screenshot is still taken and still written in a
        // -DAVER_ENABLE_UI=OFF build; only the toast about it has nowhere to appear.
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

    // GIVE UP rather than wait forever: a device loss or a backend that never completes the
    // readback must not leave the latch stuck at "pending" for the rest of the session, which would
    // silently swallow every later F9 press -- requestViewportScreenshot() refuses to re-arm while
    // viewportShotState_ != 0.
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
    // The test's second experiment holds the pose still and toggles ray tracing instead, because
    // the sun cascade is rasterised from the same posed vertices and so moves with the pose
    // whether or not the acceleration structure was rebuilt -- measured, not assumed.
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
    //
    // Straight onto the SAME draw list the bars above went into, through the C++ API rather than
    // the C ABI: aver_ui_text takes a font, and a font is a C++ type. The ABI seam for text is
    // the next slice; what this proves is that the geometry, the atlas and the renderer's
    // texture path all line up, which is the part that had never been exercised.
    if (uiFont_.valid()) {
        // const_cast because aver_ui_draw_list() hands back a const void* -- the ABI's read-only
        // view of the list it owns. Appending to it is exactly what every aver_ui_rect call
        // above already does through the C side; this reaches the same object by the C++ type.
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
        // SAY SO, ONCE PER NAME -- moved verbatim from the entity loop's own copy (formerly
        // 5904-5919), same static set, now the only place this warning can fire from.
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
// F4: shared by the direct route and the raster substitution below (formerly 6316-6321) --
// skinning checked first, matching "SKINNING WINS WHERE BOTH CLAIM AN ENTITY" (nothing forbids
// CSoftBody on an already-skinned mesh, so asking skinning first makes the collision deterministic).
rhi::MeshHandle SandboxApp::posedHandle(scene::Entity ent) {
    rhi::MeshHandle h = 0;
    if (skinnedScene_) h = skinnedScene_->drawHandle(ent);
#if AVER_MODULE_RENDER_SOFTBODY
    if (!h && softBodyScene_) h = softBodyScene_->drawHandle(ent);
#endif
    return h;
}

#endif

// F8's idle log names WHICH feature is painting the scene. INTEGRATOR FIX: the plan's own text
// named this as VoxiRenderer::debugViewActive()/rayDrivenActive() individually (checked in
// suppressesScene()'s own priority, VoxiRenderer.cpp:2955 -- debugViewActive() before
// rayDrivenActive()), but both are PRIVATE to VoxiRenderer (VoxiRenderer.hpp:1611,1615) and
// SandboxApp is not a friend -- a genuine compile error (C2248), not a contradiction worth
// stopping the item over, since this function only ever feeds a human-readable log string, never
// a decision. Fixed by reading the one PUBLIC signal that already answers "is Voxi the cause"
// (suppressesScene(), VoxiRenderer.hpp:253) instead of asking which of its two sub-reasons fired --
// this loses the debug-raymarch-vs-ray-driven distinction in the log line's wording only; the F8
// gate itself (occlusionTestShouldRun, fed by e.device()->sceneSuppressed()) never called either
// private method and is unaffected. Widening VoxiRenderer's access instead was rejected: the
// plan's own review checklist (section 6, item 18) requires "No file under modules/render.voxi
// has changed", and no lane owned that file.
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

// --no-occlusion-cull: the CLI has no way to say "off" that BEATS a level's own manifest --
// project_.occlusionCull >= 0 (applyProjectVoxiSettings) writes occlusionCullEnabled_ during
// project/level load, which runs AFTER every CLI setter fires at construction, so a bare
// setOcclusionCullOverride(false) here would be silently overwritten the instant the level
// opens -- exactly the repro project's own case, whose manifest records OCCLUSIONCULL 1.
// Applied instead every frame in onUpdate (occlusionCullForceOff_ below), the same runtime
// point the Project Settings "Occlusion culling" checkbox writes this member from -- i.e. this
// reproduces headlessly the identical live toggle the user already did by hand for the A/B this
// flag exists to automate, not a new code path.
void SandboxApp::setOcclusionCullForceOff() { occlusionCullForceOff_ = true; }

// --occlusion-waitidle / --no-occlusion-waitidle: forces (or releases) OcclusionCuller::
// testBatch()'s res.waitIdle() at runtime, no rebuild -- see IOcclusionCuller::
// setDebugForceWaitIdle's own comment (Occlusion.hpp) for exactly what it does and why it now
// defaults ON (a real, unresolved GI/lighting dependency on the wait, not merely the buffer race
// the rotation fix already covers -- see OcclusionCuller.cpp's FOLLOW-UP comment above its
// kInFlight member). Only SEEDS occlusionDebugForceWaitIdleArg_ here, at construction, before
// occluder_ exists -- onInit copies it into editor::consoleOcclusionForceWaitIdleSlot() once
// occluder_ is created, and onUpdate reasserts that slot onto occluder_ every frame, the same
// "CLI sets the seed, a per-frame reassert makes it live" shape --no-occlusion-cull already uses
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

void SandboxApp::setPtBounces(int n) { ptBouncesOverride_ = n; }

void SandboxApp::setLayeredBsdf(int n) { layeredBsdfOverride_ = n; }

void SandboxApp::setCoat(f32 w, f32 r, f32 f0) { coatWeight_=w; coatRough_=r; coatF0_=f0; }

void SandboxApp::setGiUpdateInterval(int n) { giUpdateIntervalOverride_ = n; }

// --gi-mode N. Unlike --rd-ablate this is NOT a shader define -- both estimators are compiled
// into every scene pipeline and PSMainVoxi/PSRayDriven choose between them per pixel by
// Settings::giMode at draw time -- so, unlike --rd-ablate, it does not have to be handed over
// before init() below; it only has to reach setSettings() before the frame that reads it.
void SandboxApp::setGiMode(int n) { giModeOverride_ = n; }

// --restir-visibility none|reconstructed|half|full: same "does not have to be handed over before
// init(), only before the frame that reads it" reasoning as setGiMode just above -- both estimator
// and visibility mode are read per pixel at draw time, not baked into a pipeline at load.
void SandboxApp::setRestirVisibility(int n) { restirVisibilityOverride_ = n; }

void SandboxApp::setDenoiser(int n) { denoiserOverride_ = n; }

void SandboxApp::setReblurAccum(int n) { reblurAccumOverride_ = n; }

void SandboxApp::setRenderScale(f32 s) { renderScaleOverride_ = s; }

#if AVER_MODULE_SR
// --aversr LEVEL. Records that the CLI chose it, so loadEditorPreferences leaves it alone --
// the same "a flag exists so a human at the keyboard can override recorded state" rule the
// render-settings override block spells out at length.
// averSrCliLevel_ pins the level for updateAverSrAuto's resolveAverSrLevel call every frame, so a
// project opened (or reloaded) after startup can never re-derive AverSR's level out from under an
// explicit --aversr LEVEL the way it legitimately can for Auto -- "the command line wins" has to
// keep winning past the first frame, not just at the moment this setter runs.
void SandboxApp::setAverSrQuality(aver::sr::Quality q) {
    averSrQuality_ = q; averSrFromCli_ = true; averSrCliLevel_ = static_cast<int>(q);
}

// --aversr auto: EXPLICIT CLI Auto (plan section 3.3 A) -- distinct from never passing --aversr at
// all. Sets averSrFromCli_ so loadEditorPreferences still leaves the stored choice alone (the
// command line still wins), but leaves averSrQuality_/averSrCliLevel_ untouched (-1) so
// updateAverSrAuto's resolveAverSrLevel call has no pinned CLI level to short-circuit with -- the
// project manifest and the ladder's own per-rung default get to decide the level every frame,
// exactly as if no --aversr flag had been given, except that a stored user preference on disk is
// never consulted for the length of this run.
void SandboxApp::setAverSrCliAuto() { averSrFromCli_ = true; averSrCliAuto_ = true; }

// Constructs SpatialUpscaler against `dev`'s resource factory if it is not already built.
// Idempotent -- cheap to call every time the quality combo changes, not just once. See
// logAverSrActive()'s comment for what constructing it does and does not buy today.
void SandboxApp::ensureAverSrUpscaler(rhi::IDevice* dev) {
    if (!dev) return;
    if (!averSrUpscaler_) {
        if (rhi::IResourceFactory* res = dev->resources())
            averSrUpscaler_ = std::make_unique<aver::sr::SpatialUpscaler>(*res);
    }
    // HANDED TO THE DEVICE, the step that was missing: constructed and correct, but nothing ever
    // called execute() because IDevice had no slot for it. It does now.
    // NULL WHEN Off is the whole of how the bit-identical invariant is kept: the backend branches
    // on upscaler() != nullptr. Non-owning on the device's side -- the device is told nullptr
    // before this object goes away (clearAverSrUpscaler).
    applyUpscalerSlot(dev);
}

// Detaches before destruction, so the device can never hold a dangling upscaler.
void SandboxApp::clearAverSrUpscaler(rhi::IDevice* dev) {
    if (dev) dev->setUpscaler(nullptr);
}

void SandboxApp::setEdgeAaOverride(bool on) { edgeAaEnabled_ = on; }

// Picks whichever of --edge-aa / --aversr should actually be bound to the device's ONE upscaler
// slot -- only one can run at a time (edgeAaEnabled_'s comment). Every call site that used to hand
// the device an upscaler directly now goes through this, so the two can never race to overwrite each other.
void SandboxApp::applyUpscalerSlot(rhi::IDevice* dev) {
    if (!dev) return;
    if (edgeAaEnabled_ && edgeAaUpscaler_) { dev->setUpscaler(edgeAaUpscaler_.get()); return; }
    dev->setUpscaler(averSrQuality_ == aver::sr::Quality::Off ? nullptr : averSrUpscaler_.get());
}

// Constructs FxaaResolve against `dev`'s resource factory if not already built, then hands it to
// the device through applyUpscalerSlot() -- the same idempotent shape as ensureAverSrUpscaler, for
// the same rhi::IUpscaler seam with a different algorithm. Off (edgeAaEnabled_ never set) never calls this.
void SandboxApp::ensureEdgeAaUpscaler(rhi::IDevice* dev) {
    if (!dev) return;
    if (!edgeAaUpscaler_) {
        if (rhi::IResourceFactory* res = dev->resources())
            edgeAaUpscaler_ = std::make_unique<aver::sr::FxaaResolve>(*res);
    }
    applyUpscalerSlot(dev);
    if (edgeAaUpscaler_)
        AVER_INFO("[AverSR] '{}' handed to the device (--edge-aa)", edgeAaUpscaler_->name());
    else
        AVER_WARN("[AverSR] --edge-aa requested but FxaaResolve could not be constructed "
                  "(no resource factory)");
}

// Logs the [AverSR] brand-tag line plus the one honest caveat: rhi::IDevice has no
// setUpscaler()/upscaler() hook yet, so nothing on the present path calls
// SpatialUpscaler::execute(). --render-scale is real; SpatialUpscaler is constructed, correct, and
// reachable, but its resample pass is not yet what produces the pixels on screen -- still the
// backend's own bilinear render-scale resize. Closing the gap needs a backend to read
// device->upscaler() from its own composite/present step (D3D12Device.cpp).
void SandboxApp::logAverSrActive(rhi::IDevice* dev) {
    if (!dev) return;
    AVER_INFO("[AverSR] {}: render scale {:.2f}{}", aver::sr::qualityName(averSrQuality_),
              dev->renderScale(),
              averSrUpscaler_ ? "" : " (SpatialUpscaler not constructed -- no resource factory)");
    // The old warning here said SpatialUpscaler was "constructed but not yet reachable from
    // the present path -- rhi::IDevice has no upscaler hook". That hook exists now and the
    // backend logs when it actually runs, so this would have been a lie the moment it fired.
    if (averSrUpscaler_ && averSrQuality_ != aver::sr::Quality::Off)
        AVER_INFO("[AverSR] {} handed to the device; the backend reports when it upscales",
                  averSrUpscaler_->name());
}

// Applies one AverSR quality level from the render-settings combo: the docs/AVERSR.md
// render-scale table through the SAME rhi::IDevice::setRenderScale the slider next to it already
// edits. Off resets the scale to native and drops any constructed upscaler -- bit-identical to never having touched the combo.
void SandboxApp::applyAverSrQuality(rhi::IDevice* dev, aver::sr::Quality q) {
    // NAMED, because a render scale of 0.67 turned up at startup that nothing on the command
    // line, project manifest or editor.ini had asked for, and this is the only code that can
    // produce that number. If this line prints, this function is the source; if it doesn't and
    // the scale still moves, the search goes elsewhere. Fires only on an explicit quality change.
    AVER_INFO("[AverSR] applyAverSrQuality({}) -> render scale {:.4f}",
              static_cast<int>(q), aver::sr::renderScaleFor(q));
    averSrQuality_ = q;
    if (!dev) return;
    if (q == aver::sr::Quality::Off) {
        // DETACH BEFORE DESTROY. The device holds a RAW pointer to this upscaler; resetting first
        // left D3D12Device::upscaler_ dangling and the next frame's composite crashed on freed
        // memory -- turning AverSR ON then OFF crashed the editor.
        // THE GUARD ALREADY EXISTED AND HAD NO CALLERS: clearAverSrUpscaler's own comment says
        // "Detaches before destruction, so the device can never hold a dangling upscaler", and the
        // teardown path above describes the same bug ("--edge-aa's first --frames run crashed
        // (SIGSEGV) AT PROCESS EXIT") but fixes it INLINE rather than calling the helper. This, the
        // only case a user can reach from the UI, was left open.
        // applyUpscalerSlot rather than setUpscaler(nullptr): the slot resolves to edge-AA if that
        // is on; clearing it outright would silently switch --edge-aa off as a side effect.
        applyUpscalerSlot(dev);
        averSrUpscaler_.reset();
        dev->setRenderScale(1.0f);
        return;
    }
    dev->setRenderScale(aver::sr::renderScaleFor(q));
    ensureAverSrUpscaler(dev);
    logAverSrActive(dev);
}

// ---- AverSR's RESOLUTION CHAIN, WHICH NEEDS THE LADDER AND THEREFORE VOXI -------------------
// Everything above this point is AverSR on its own: a render scale, an upscaler, and the CLI flag
// that pins one. Everything below resolves WHICH level to apply through voxi::resolveAverSrLevel /
// voxi::autoAverSrLevel against the Overall rung -- render.voxi's ladder, named in these functions'
// own signatures (voxi::AverSrSource, voxi::Settings, voxi::DeviceInfo). SR and VOXI are
// independent options and PBR=OFF forces VOXI=OFF, so an SR-on/VOXI-off tree is real and reached
// these definitions with no aver/voxi header in sight. Their one caller already asks for both
// (SandboxApp.cpp's onUpdate, `#if AVER_MODULE_VOXI` around `#if AVER_MODULE_SR`).
#if AVER_MODULE_VOXI
// Human text for the "(source)" half of every AverSR surface (the mandatory startup log, the
// Display combo's "Auto (<level> from <source>)" preview, and the Project Settings upscaling
// line) -- one place so the three descriptions can never drift apart. ForcedOff does not say WHY
// here (the --edge-aa upscaler-slot conflict and a tripped crash cookie both read as ForcedOff
// through this enum alone); a caller that needs to tell those apart checks
// edgeAaEnabled_/averSrCookieTripped_ itself before falling back to this text.
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

// The rung whose own ladder default autoAverSrLevel (Scalability.hpp) just resolved through --
// computed again here, deliberately, only for display text: autoAverSrLevel already did the real
// arithmetic, this only names which rung its answer came from, for the Project Settings upscaling
// line's "Auto from <rung>" / "(differs from the <rung> preset's default...)" text (3.3 A: "When
// overallFromSettings reads Custom, name the tier Auto used" -- the higher of the GI/RT tiers,
// autoAverSrLevel's own Custom branch, mirrored here rather than shared, since that function
// returns the LEVEL, not the rung's name).
const char* SandboxApp::averSrAutoRungName(const voxi::Settings& s, const voxi::DeviceInfo& d) const {
    const voxi::OverallQuality rung = voxi::overallFromSettings(s, d);
    if (rung != voxi::OverallQuality::Custom)
        return voxi::Renderer::qualityName(static_cast<voxi::Quality>(static_cast<u32>(rung)));
    const u32 giTier = static_cast<u32>(s.globalIllumination);
    const u32 rtTier = static_cast<u32>(s.rayTracing);
    return voxi::Renderer::qualityName(static_cast<voxi::Quality>(giTier > rtTier ? giTier : rtTier));
}

// optimisation-wave-2, U2 (3.3 A): resolves AverSR's level fresh every frame from CLI > the user's
// own Display choice > the project manifest > the Overall rung's own ladder default
// (Scalability.hpp's resolveAverSrLevel), and applies it only on an actual change. Called from
// onUpdate, OUTSIDE beginFrame/endFrame -- see the call site's own comment: applyAverSrQuality ends
// in setRenderScale, and setRenderScale mid-frame (between beginFrame and endFrame) is exactly the
// device-loss class aver-render-scale-device-loss documents.
void SandboxApp::updateAverSrAuto(Engine& e) {
    if (!voxiAttached_) return;
#if AVER_WITH_IMGUI
    // NOT BEFORE THE PREFERENCES HAVE LOADED. onUpdate runs BEFORE buildUI, and buildUI is where
    // loadEditorPreferences first runs (prefsLoaded_). So on frame 1 this used to resolve a level from
    // the member defaults (Auto), arm display.renderScalePending in the in-memory prefs store and
    // apply it -- and loadEditorPreferences, later in that SAME frame, read the cookie this function
    // had just armed as "the last launch did not survive applying Auto", latched
    // averSrCookieTripped_ and forced AverSR Off. Every launch, whatever the Display choice or the
    // project's RENDER.AVERSR asked for: the render scale sat at 1.0 no matter what was picked. A
    // headless run never builds the UI or loads preferences, so there is nothing to wait for there.
    if (!prefsLoaded_ && !headless_) return;
#endif
    rhi::IDevice* dev = e.device();
    voxi::Renderer& vxr = voxi::Renderer::get();

    // A PLAIN --render-scale, WITH NO --aversr, ALREADY OWNS THE SCALE OUTRIGHT: onInit applies it
    // directly (the same "--render-scale wins" precedent loadEditorPreferences' own guard uses),
    // and resolving/applying a level here would silently walk it back the instant Auto (or a
    // Display/manifest pick) disagreed with it. --aversr ITSELF (LEVEL or auto) is NOT caught by
    // this: averSrFromCli_ routes through cliLevel below instead -- onInit's own "--aversr sets
    // renderScaleOverride_ too, as a side effect, when it is not already pinned" mutation means
    // renderScaleOverride_ alone cannot tell the two apart, so averSrFromCli_ is the second half of
    // the same test the load path's own guard already needs.
    const bool explicitRenderScaleOnly = renderScaleOverride_ != 1.0f && !averSrFromCli_;

    if (!explicitRenderScaleOnly && averSrChoice_ != editor::AverSrChoice::Manual) {
        const int cliLevel  = (averSrFromCli_ && !averSrCliAuto_) ? averSrCliLevel_ : -1;
        const int userLevel = editor::userLevelFor(averSrChoice_);
        const u32 autoLevel = voxi::autoAverSrLevel(vxr.settings(), vxr.deviceInfo());
        voxi::AverSrDecision decision =
            voxi::resolveAverSrLevel(cliLevel, userLevel, averSrProjectDefault_, autoLevel);

        // --edge-aa and AverSR share the one upscaler slot, and applyUpscalerSlot lets edge-AA win
        // (:7976-7980 region) -- only overrides an AUTO resolution: an explicit CLI/user/manifest
        // pin is still a deliberate ask this flag should not silently swallow.
        if (edgeAaEnabled_ && decision.source == voxi::AverSrSource::Auto) {
            decision = voxi::AverSrDecision{0u, voxi::AverSrSource::ForcedOff};
            if (!edgeAaAverSrWarnLogged_) {
                AVER_WARN("[AverSR] --edge-aa occupies the upscaler slot; AverSR Auto is off for "
                          "this session");
                edgeAaAverSrWarnLogged_ = true;
            }
        }
        // A LEVEL THAT JUST TOOK THE DEVICE DOWN IS NEVER SILENTLY RE-ATTEMPTED -- the load path's
        // own cookie check (loadEditorPreferences) already forced Off and latched this for a level
        // that did not survive ITS OWN launch; kept forced for the rest of this session, the same
        // way the load-time latch is never cleared except by a fresh process.
        if (averSrCookieTripped_) decision = voxi::AverSrDecision{0u, voxi::AverSrSource::ForcedOff};

        averSrSource_ = decision.source;
        const aver::sr::Quality q = static_cast<aver::sr::Quality>(decision.level);
        if (q != averSrQuality_) {
            // ARMED BEFORE THE FIRST NON-OFF APPLICATION THIS SESSION (3.3 A): a level Auto
            // resolves to mid-session can lose the device exactly the way a stored one can at load
            // -- same cookie, extended to cover it. The existing 30-frame clear (onUpdate, beside
            // the shader watcher poll) then applies unchanged.
            if (q != aver::sr::Quality::Off && !averSrArmedNonOffOnce_) {
                editor::setPrefBool("display.renderScalePending", true);
                editor::flushEditorPrefs();
                renderScaleCookieArmed_ = true;
                averSrArmedNonOffOnce_ = true;
            }
            applyAverSrQuality(dev, q);
        }
    } else if (!explicitRenderScaleOnly) {
        // Manual: the Render Scale slider already owns the render scale directly
        // (prefsDevice_->setRenderScale) -- nothing here to resolve or apply. Resolving through
        // userLevelFor's -1 sentinel and applying an unrelated named level would fight the user's
        // own drag every single frame, so this branch only reports the choice, never touches the
        // device.
        averSrSource_ = voxi::AverSrSource::User;
    }
    // else: explicitRenderScaleOnly -- averSrSource_/averSrQuality_ left exactly as they are
    // (Off, untouched by anything AverSR-side); the startup log below still fires and reports that
    // honestly, since a plain --render-scale run is still a non-native capture worth the same warning.

    // THE MANDATORY STARTUP LOG (C2-10), fired once per process, on every run including --frames --
    // the only warning an ad-hoc --frames capture that forgot --aversr off gets that it is not
    // measuring native resolution. Scene size is recomputed from the present size and the live
    // scale (D3D12Device::computeSceneSize's own round-to-nearest formula) rather than read off a
    // backend-private field: no rhi::IDevice accessor for it exists, and this line only needs to
    // report it, not derive anything from it. PRESENT SIZE COMES FROM e.window(), NOT dev -- integrator
    // fix: rhi::IDevice has no width()/height() of its own (only ISwapchain does); e.window() is the
    // same accessor GameApp::onInit's own copy of this line already uses.
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

// Reconciles ptSceneView_ (the ACTUAL registration) with ptSceneViewWantEnabled_ (what --pt-scene
// or the settings combo most recently asked for). Idempotent, so free to call every frame.
// CALLED FROM ONUPDATE() ONLY, never from buildUI()/onRender(): onUpdate() runs BEFORE
// device_->beginFrame(), the one point nothing is mid-recording. suppressesScene() is read LIVE
// once per drawMesh() call all through onRender, so mutating features_ mid-loop would let one
// frame's draws disagree about whether the scene is suppressed. Deferring to the NEXT onUpdate()
// sidesteps it.
// addRenderFeature() calls onRenderTargetsChanged() immediately against the device's CURRENT scene
// targets, so a feature turned on mid-session builds against THIS session's swapchain for free.
// removeRenderFeature() is a plain vector erase with no waitIdle; the object's own destroy calls
// retire behind the graphics queue's fence, so releasing GPU objects a frame or two still in
// flight might be reading is safe.
// THE ONE GAP NOT CLOSED: PathTracer leaks every TLAS it builds (no destroyTlas in the RHI), so
// toggling this repeatedly leaks one TLAS per re-arm -- small, bounded, and a pre-existing RHI gap,
// but real.
void SandboxApp::syncPtSceneView(rhi::IDevice* dev) {
    if (!dev) return;

    // ---- NOTHING TRACES FOR A VIEWER THAT CANNOT SEE IT ----
    // The path-traced view and ray-driven primary visibility paint the same pixels, and only one
    // wins the device's suppressesScene() election -- verifiably: with --rt-render-mode 0 the path
    // tracer DOES paint (54% different, 7.8ms); in mode 1 the frame is what ray-driven drew.
    // But PtSceneView::prePass accumulates regardless of who wins, since prePass runs BEFORE the
    // election. Measured on PTTest: 8 spp for 200 frames, ~9ms each, converging an image that's
    // thrown away. The honest place to stop it is here, before it exists at all.
    // AN EXPLICIT --pt-scene IS NOT SILENTLY IGNORED: it is told what happened, because "asked for
    // the path-traced view, got the ray-driven one with no message" is a class of silence this
    // file has been bitten by before.
    // A3: willSuppressSceneThisFrame(), NOT suppressesScene(). This function runs from
    // onUpdate(), before device_->beginFrame() -- see this function's own header comment above.
    // suppressesScene() reads rtActive_, which buildAccelerationStructures() (called from
    // prePass(), inside THIS frame's beginFrame(), AFTER beginScene() has already swapped
    // drawsPrev_/draws_) has not recomputed for this frame yet -- so suppressesScene() here would
    // answer LAST frame's question. willSuppressSceneThisFrame() predicts what prePass is about
    // to make true instead; see its own comment in VoxiRenderer.hpp for the swap it accounts for.
#if AVER_MODULE_VOXI
    const bool rayDrivenPaints = voxiRenderer_.willSuppressSceneThisFrame();
#else
    // THE ELECTION HAS ONE FEWER CANDIDATE. voxiRenderer_ is declared `#if AVER_MODULE_VOXI`, and
    // this function is deliberately not -- the flag and the PT view's registration must keep
    // working with the module off (see ptSceneViewWantEnabled_'s own comment). Nothing else claims
    // primary visibility, so the answer is a constant here rather than a call.
    const bool rayDrivenPaints = false;
#endif
    if (rayDrivenPaints && ptSceneViewWantEnabled_) {
        // A1: named for the Path Tracing page's Quality-combo tag (see PtRenderConflict.hpp's
        // choosePtViewTag and this function's caller in buildUI()). Set every frame this branch
        // fires, same as the log-once flag below is CHECKED every frame -- so it stays true for as
        // long as the suppression does, not just on the first frame it started.
        ptSceneViewSuppressedByRayDriven_ = true;
        if (!ptSceneViewYieldLogged_) {
            ptSceneViewYieldLogged_ = true;
            if (ptSceneViewFromCli_)
                AVER_WARN("[PT] --pt-scene was given, but ray-driven primary visibility is "
                          "painting the scene and wins the election -- the path-traced image "
                          "would never be shown, so it is not being traced. Add "
                          "--rt-render-mode 0 to actually see it.");
            else
                AVER_INFO("[PT] the path-traced view is not being traced: ray-driven primary "
                          "visibility is painting the scene, so its image would never be "
                          "shown. --rt-render-mode 0 hands the frame back to it.");
        }
        ptSceneViewWantEnabled_ = false;
    } else if (!rayDrivenPaints) {
        ptSceneViewYieldLogged_ = false;   // re-arm the message if the mode changes back
#if AVER_MODULE_VOXI
        // N8 FIX, PART 2: THE PT VIEW ACTUALLY COMES BACK -- the promise the INFO/WARN messages
        // above already made ("--rt-render-mode 0 hands the frame back to it") but that this
        // branch never kept before this fix: it only ever re-armed the log/suppression flags, so
        // ptSceneViewWantEnabled_ stayed false forever once ray-driven had suppressed it, even
        // after ray-driven itself stopped painting. Restored here, ONLY if this yield was actually
        // the reason the want flag went false (ptSceneViewSuppressedByRayDriven_, read BEFORE the
        // line below clears it) and the request it suppressed is still live: the Path Tracing tier
        // is still above Off, or --pt-scene explicitly asked to keep the view regardless of tier.
        // Guarded on AVER_MODULE_VOXI, like occlusionSuppressingFeatureName()'s own Voxi read a
        // little above in this file -- this function must still build with the module off, and
        // voxi::Renderer::get() (a type this read needs) does not exist in that build at all.
        if (ptSceneViewSuppressedByRayDriven_ &&
            (voxi::Renderer::get().settings().pathTracing != voxi::Quality::Off || ptSceneViewFromCli_))
            ptSceneViewWantEnabled_ = true;
#endif
        ptSceneViewSuppressedByRayDriven_ = false;   // A1: same re-arm trigger as the log message
    }

    if (ptSceneViewWantEnabled_ == (ptSceneView_ != nullptr)) return;

    if (ptSceneViewWantEnabled_) {
        ptSceneView_ = std::make_unique<aver::pt::PtSceneView>();
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // THE HOST RESOLVES THE MATERIAL, because the host is the only thing that knows it bound
        // one -- why Aver.Render.PathTracer can link Aver.RHI and Aver.Core alone.
        // ownsBindingSet() is the IDENTITY test, not a shape test: a block merely sizeof
        // (MaterialConstants) is not a material. Size is checked only as corroboration.
        // AND THE FALLBACK SET MEANS "NOT AUTHORED": fallbackSet_/fallbackConstants_' baseColorFactor
        // is {1,1,1,1}, so reading it would render every non-authored surface white -- those draws
        // fall through to the per-draw baseColor instead.
        ptSceneView_->setAlbedoResolver(
            [this](aver::rhi::BindingSetHandle set, const void* constants, aver::u32 bytes,
                   pt::PtSceneView::ResolvedMaterial& out) -> bool {
                aver::f32* outAlbedo = out.albedo;
                if (!set || !constants || bytes != sizeof(pbr::MaterialConstants)) return false;
                pbr::MaterialSystem& ms = voxiRenderer_.materials();
                // THIS EARLY RETURN IS WHY THE VIEW RE-ARMS TWICE ON A STATIC SCENE -- looks like
                // a tracer bug, is not: until the material system is ready this resolves nothing,
                // so PtSurface uses the ordinary base colour; when ready() flips, every surface's
                // reported albedo CHANGES, moving the drawsKey() hash and re-arming the accumulator.
                // Measured on FirstPerson: two re-arms in the first frames of a static scene,
                // costing a few frames out of ~204. Used to cost more -- every re-arm also leaked a
                // TLAS, which PathTracer::addScene no longer does.
                // NOT FIXED HERE: false currently means both not-ready-yet and not-ours/
                // un-authored, and distinguishing them is a change to AlbedoResolver's signature.
                if (!ms.ready()) return false;
                if (set == ms.fallbackBindingSet()) return false;   // un-authored: keep the look's colour
                if (!ms.ownsBindingSet(set)) return false;          // not one of ours at all
                const auto* mc = static_cast<const pbr::MaterialConstants*>(constants);
                // THE TRACER SAMPLES TEXTURES NOW, so the answer depends on whether this
                // material has one, and the two branches mean DIFFERENT THINGS by outAlbedo.
                //
                // WITH a texture: hand back the FACTOR and the handle. baseColorFactor is already
                // linear (packMaterial decoded it), and the tracer multiplies factor x texel --
                // the same composition voxi.hlsl's textured ray hit makes. Returning
                // averageBaseColor here instead would be the bug this whole seam is shaped to
                // prevent: that value is factor x texture MEAN, so the texture would be applied
                // twice, and the only symptom is a uniformly too-dark scene with nothing logged.
                //
                // WITHOUT one: the previous behaviour, unchanged and still necessary. A modern
                // material puts its look in a TEXTURE and leaves the factor a plain white
                // multiplier -- every one of the forty materials in the demo project declares
                // `baseColorFactor 1 1 1 1` -- so a tracer that could not sample and read the
                // factor alone painted every surface pure white, roughly three times too bright
                // and completely flat. The mean is what that fallback is for, and it is still
                // what a device with no bindless support gets.
                if (const auto* tex = ms.textures(set)) {
                    // textures() reports EFFECTIVE handles, so an unmapped slot is the 1x1
                    // identity fallback (white / flat normal / (0,255,255,255) metal-rough)
                    // rather than 0 -- sampling it is a multiply by one, which is correct, and
                    // costs one fetch on a material that authored no such map.
                    const aver::rhi::TextureHandle base =
                        (*tex)[static_cast<aver::usize>(pbr::TextureSlot::BaseColor)];
                    if (base) {
                        out.baseColorTex  = base;
                        out.metalRoughTex = (*tex)[static_cast<aver::usize>(pbr::TextureSlot::MetalRough)];
                        out.normalTex     = (*tex)[static_cast<aver::usize>(pbr::TextureSlot::Normal)];
                        // FACTORS, not finished values -- the tracer multiplies each by its map.
                        // baseColorFactor is already linear (packMaterial decoded it); roughness
                        // and metallic are linear scalars and need no decode.
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
            // Don't retry every frame. This does NOT reach back into voxi::Settings::pathTracing
            // -- syncPtSceneView() has no Voxi dependency (must keep working with AVER_MODULE_VOXI
            // off) -- so the settings-page combo can be left stale; see its own BeginDisabled for how the UI stays honest anyway.
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
// The exact inverse of averFogFactor's k<=1e-8 branch: that function computes
// opacity(d) = 1 - exp(-density*d), so this solves the SAME expression backwards for density given
// a target opacity. Not an approximation: OcWorld.hpp has no fields for fogFalloff/fogStart at
// all, so k and start are always 0 for anything a level can author.
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
