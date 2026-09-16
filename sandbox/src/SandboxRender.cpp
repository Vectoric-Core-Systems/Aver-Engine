// Runtime side: the world draw (onRender), surface resolution, occlusion, AverSR and the path-traced scene view.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
// Submits the frame: the editor scene, the level world, gizmos, and the overlays.
void SandboxApp::onRender(Engine& e)  {
    handleManip(e);
#if AVER_MODULE_LANDSCAPE
    // Drains an undo/redo that changed terrain heights. Deferred to here because this is the
    // first point after those run that has a device -- see applyLandscapeRect's own comment.
    flushLandscapeInvalidate(e);
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
// AVER_FLUIDS_SIMULATED, not AVER_MODULE_FLUIDS: fluidScene_ only exists under the narrower define
// (fluids AND physics), so the wider guard would build against an undeclared member. The registration
// and the update() drain carry this same guard for the same reason.
#if AVER_FLUIDS_SIMULATED
    {
        // NOT GATED ON hideEditorScene: that gate is what made this block draw nothing the first
        // time -- copying the objects_ loop's guard hid the water exactly when it mattered. A
        // fluid volume is authored level content, not an editor placeholder; it stays visible
        // through Play like the landscape below.
        // Every live volume, drawn in handle order and NOT sorted here -- the device captures
        // blended draws and replays them sorted back-to-front itself (D3D12Device::endFrame).
        const auto drawOneFluid = [&](fluids::FluidHandle h) {
            const rhi::MeshHandle fm = fluidScene_.drawHandle(h);
            if (!fm) return;

            // THE SURFACE MATERIAL, resolved through the SAME two steps every other surface in a
            // level uses (aver_scene_material interns the name, surfaceMaterials_ maps it to a
            // live handle). Resolved at DRAW time rather than latched at spawn, because a volume
            // can be spawned before its project's materials finish loading -- a handle captured too early would be a permanent zero.
            u32 authored = 0;
#if AVER_MODULE_PBR
            if (const auto nm = fluidSurfaceMaterial_.find(h);
                nm != fluidSurfaceMaterial_.end() && !nm->second.empty()) {
                const i32 mid = aver_scene_material(0, nm->second.c_str());
                if (const auto it = surfaceMaterials_.find(mid); it != surfaceMaterials_.end())
                    authored = it->second;
                else {
                    // ONCE PER NAME, matching the scene loop's own warning for the same mistake: a
                    // WATER record naming a material nobody authored is otherwise silent, and this
                    // exact silence -- PTTest naming M_Concrete with no .ocmat -- cost a multi-day investigation.
                    static std::unordered_set<std::string> s_warned;
                    if (s_warned.insert(nm->second).second)
                        AVER_WARN("[Water] volume {} names surface material '{}' but no .ocmat by "
                                  "that name was loaded; drawing the fallback look", h, nm->second);
                }
            }
#endif
            // Neutralised to 1.0 where a material carries the value, as the scene loop does.
            // THE FALLBACK LOOK, for a WATER record naming no .ocmat. ALPHA is load-bearing:
            // averBuildSurface computes `s.alpha = gBaseColor.a * a.opacity`, so at 1.0 this
            // would put an opaque lid over the pool -- the "tinted plastic" outcome above warns
            // against. Roughness 0.10 (smooth), metallic 0 (dielectric).
            f32 col[4]   = {0.35f, 0.55f, 0.62f, 0.35f};
            f32 metallic = 0.0f, roughness = 0.10f;
            bool blended = true;
#if AVER_MODULE_PBR
            if (authored) {
                col[0] = col[1] = col[2] = 1.0f;
                metallic = roughness = 1.0f;
                if (const pbr::MaterialDesc* d = pbr::MaterialLibrary::get().desc(authored))
                    blended = pbr::isTranslucent(*d);
            }
#endif
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
            // setDrawBinding is STICKY, so it is stated before every draw rather than set once.
            if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready())
                e.device()->setDrawBinding(ms.bindingSet(authored), &ms.constants(authored),
                                           sizeof(pbr::MaterialConstants));
#endif
            // BLENDED EVEN WITH NO AUTHORED MATERIAL: `blended` starts true, and only an authored
            // OPAQUE material turns it off -- water with no .ocmat must composite over the pool
            // floor, not draw as a solid lid. An author wanting opaque water can say so in the .ocmat.
            e.device()->setDrawBlended(blended);

            // A REAL TRANSLATION, NOT IDENTITY: the buffer is mesh-LOCAL about the volume's
            // centre, so this matrix places it AND gives the blended flush something true to sort
            // by -- D3D12Device orders blended draws back-to-front on world[12..14], so an
            // identity matrix claims to sit at the world ORIGIN and can composite on the wrong side of a glass pane it is plainly in front of.
            f32 origin[3] = {0.0f, 0.0f, 0.0f};
            fluidScene_.volumeOrigin(h, origin);
            Mat4 fw = Mat4::identity();
            fw.m[3][0] = origin[0];
            fw.m[3][1] = origin[1];
            fw.m[3][2] = origin[2];
            e.device()->drawMesh(fm, &fw.m[0][0], col, metallic, roughness);
        };

        if (fluidHandle_) drawOneFluid(fluidHandle_);
        for (const fluids::FluidHandle h : fluidGraphHandles_) drawOneFluid(h);
    }
#endif

#if AVER_MODULE_LANDSCAPE
    // The landscape pass: one direct select()+draw() call, the same hand-rolled shape as the
    // objects_ loop above -- not an IRenderFeature, and not gated on hideEditorScene: terrain is
    // real environment geometry, staying visible through Play like the sky and fog.
    if (landscapeLoaded_) updateLandscapeRingTiles(e.device(), eye_.x, eye_.y);

    if (landscapeLoaded_ && landscapeRenderer_) {
        // Bind the level's terrain material the first frame the material system is ready.
        // LEVEL LOAD CANNOT BE TRUSTED TO BE LATE ENOUGH: MaterialSystem::ready() also needs its
        // GPU side up with no guaranteed order versus the landscape load, so a per-frame bool test is cheaper than reasoning about that ordering and self-heals.
        if (!landscapeMaterial_.empty() &&
            (!landscapeRenderer_->hasSurfaceBinding() || !landscapeUvTilingResolved_))
            applyLandscapeSurfaceToAll(e.device());

        landscape::SelectParams lp;
        lp.cameraCm[0] = eye_.x; lp.cameraCm[1] = eye_.y; lp.cameraCm[2] = eye_.z;
        // Same fovY and projScale formula trifactor::projScale uses, over THIS frame's actual
        // viewport height rather than SelectParams's 540.0f default -- a mismatched scale reads as
        // terrain refining at the wrong distance, not a crash, which would go unnoticed.
        lp.projScale = vpH_ / (2.0f * std::tan(radians(60.0f) * 0.5f));
        lp.useFrustum = true;
        f32 vpm[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) vpm[r * 4 + c] = viewProj_.m[r][c];
        lp.frustum = landscape::Frustum::fromViewProj(vpm);
        // A SHARE of the renderer's one shared draw budget, not the whole thing -- see the member
        // block's own comment on kLandscapeMaxDrawsPerTile (docs/LANDSCAPE_EDITOR.md blocker 9).
        lp.maxDraws = kLandscapeMaxDrawsPerTile;

        landscape::SelectResult lsel;
        landscapeTree_.select(lp, lsel);

        // Sections carry their own world position in every sample (OcLandData::worldAt already
        // folds originCm in -- see ChunkMesh.cpp), so the transform LandscapeRenderer::draw()
        // applies on top is identity, not a placement matrix.
        static const f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        landscapeRenderer_->draw(*e.device(), landscapeData_, landscapeTree_, lsel, kIdentity,
                                 landscapeUvTilingCm_);

        // The ring: every procedural tile resident around the camera, drawn through the SAME
        // select()+draw() pair, sharing the SAME per-tile budget -- that sharing keeps total draws
        // across every resident section within the renderer's one real ceiling.
        for (auto& kv : landscapeRingTiles_) {
            LandscapeRingTile& tile = kv.second;
            if (!tile.renderer) continue;
            landscape::SelectResult rsel;
            tile.tree.select(lp, rsel);
            tile.renderer->draw(*e.device(), tile.data, tile.tree, rsel, kIdentity,
                                landscapeUvTilingCm_);
        }
    }
#endif

#if AVER_MODULE_SCENE
    // Scene-entity pass: draws every live entity carrying a CMeshRenderer.
    {
        scene::World& w = scene::World::instance();
        int drawn = 0, culled = 0, ownerHidden = 0;
        // 3B's periodic-report extension: how many Draw records the direct route actually
        // delivered for THIS frame's culled entities, and how many of those entities were
        // multi-part -- the concrete evidence that a culled tree still yields N draws instead of
        // collapsing to slot 0's single one.
        u32 culledDraws = 0, culledMultiPart = 0;
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

        // The six frustum planes, from the camera's viewProj. ENGINE convention: row-vector, so a
        // clip coordinate dots with a COLUMN. Derived per frame rather than cached: two dozen adds, and a stale frustum culls things on screen.
        f32 pl[6][4];
        {
            const Mat4& m = viewProj_;
            for (int i = 0; i < 4; ++i) {
                pl[0][i] = m.m[i][3] + m.m[i][0];   // left
                pl[1][i] = m.m[i][3] - m.m[i][0];   // right
                pl[2][i] = m.m[i][3] + m.m[i][1];   // bottom
                pl[3][i] = m.m[i][3] - m.m[i][1];   // top
                pl[4][i] = m.m[i][2];               // near
                pl[5][i] = m.m[i][3] - m.m[i][2];   // far
            }
        }
#if AVER_MODULE_VOXI
        // ---- depth prepass phase: a SEPARATE, EARLIER walk over the SAME entities ----
        // ONE ScopedGpuStat, not one per draw: the GPU stat tree budgets 64 open spans/frame and
        // Electric Dreams submits over a thousand instances -- per-draw markers would blow that
        // AND measure wrong, since a span's time is everything between its two timestamps IN
        // SUBMISSION ORDER, folding colour time into "depth prepass" if interleaved. One bracket
        // around a CONTIGUOUS depth-only run is what makes it a real number -- also why the walk
        // runs twice instead of emitting prepass draws inline in the colour loop.
        // EXCLUDED: a SKINNED entity (posed vertex buffer is compute-written); the LANDSCAPE (its
        // own call site); the GPU CLUSTER MESH-SHADER PATH (no depth-only twin). The
        // CPU-per-cluster path (--lod-per-cluster) is excluded too: its cache (clusterCutCache_) is
        // rebuilt-or-reused once per frame per entity, and running that decision twice would
        // duplicate the rebuild or read a cache the colour walk hasn't populated -- not unsafe,
        // just unneeded for a path this task never enables.
        // ALSO EXCLUDED (with blended draws): a TRANSLUCENT entity (.ocmat alphaMode BLEND),
        // checked per-instance in this walk's own material-resolution block (search "TRANSLUCENT:
        // EXCLUDED"), since it depends on the resolved material, not which LOD/skinning system
        // claims the geometry.
        if (e.device()->depthPrepassEnabled()) {
            if (rhi::IRenderContext* pctx = e.device()->renderContext()) {
                rhi::ScopedGpuStat prepassScope(*pctx, "depth prepass");
                const u32 pn = w.count();
                for (u32 pi = 0; pi < pn; ++pi) {
                    const scene::Entity pent = w.at(pi);
                    if (w.destroyPending(pent)) continue;
                    const scene::CMeshRenderer* pmr =
                        w.component<scene::CMeshRenderer>(pent, scene::kComponentMeshRenderer);
                    if (!pmr || !(pmr->flags & scene::kMeshRendererVisible) || pmr->mesh == 0) continue;
                    const auto pit = sceneMeshes_.find(pmr->mesh);
                    if (pit == sceneMeshes_.end()) continue;
                    // Skinned: excluded (posed, compute-written buffer -- see the block comment).
                    if (skinnedScene_ && skinnedScene_->drawHandle(pent) != 0) continue;
#if AVER_MODULE_TRIFACTOR
                    // GPU cluster mesh-shader path: excluded (see the block comment).
                    if (lodMeshShaderEnabled_ && lodMeshPipelineReady_ && meshClusterGpu_.count(pmr->mesh)) continue;
                    // CPU per-cluster path: excluded (see the block comment).
                    if (lodPerClusterEnabled_ && meshClusterData_.count(pmr->mesh)) continue;
#endif
                    const Mat4& pwm = w.worldMatrix(pent);
                    // The SAME box-cull test the colour walk below runs, mirrored -- must never
                    // disagree about visibility, or a pixel this walk skips depth for could be
                    // drawn by the colour walk's prepassed (LessEqual/no-write) pipeline reading
                    // whatever depth was already there.
                    // DECLARED OUT HERE, NOT INSIDE THE CULL BLOCK -- that scope was the whole bug:
                    // the world-space box went out of scope, so LOD selection re-derived a sphere
                    // from pmr->aabbMin/aabbMax (LOCAL bounds) against the world-space eye, while
                    // the colour walk did it correctly from wlo/whi -- the two walks fed the SAME
                    // function different inputs and could pick DIFFERENT LOD levels, so the
                    // prepass wrote depth for one mesh while colour drew another and every
                    // fragment behind the wrong depth was silently dropped. Measured as fern
                    // clumps rendering visibly sparser -- 2.81% of pixels differing, falling to
                    // 0.04% (noise) with --no-lod-select.
                    bool poutside = false;
                    bool pHaveWorldBox = false;
                    Vec3 plo{1e30f, 1e30f, 1e30f}, phi{-1e30f, -1e30f, -1e30f};
                    {
                        const Vec3 lo{pmr->aabbMin[0], pmr->aabbMin[1], pmr->aabbMin[2]};
                        const Vec3 hi{pmr->aabbMax[0], pmr->aabbMax[1], pmr->aabbMax[2]};
                        if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                            pHaveWorldBox = true;
                            for (u32 c = 0; c < 8; ++c) {
                                const Vec3 cp{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                                const Vec3 t = xformPoint(pwm, cp);
                                plo.x = std::fmin(plo.x, t.x); phi.x = std::fmax(phi.x, t.x);
                                plo.y = std::fmin(plo.y, t.y); phi.y = std::fmax(phi.y, t.y);
                                plo.z = std::fmin(plo.z, t.z); phi.z = std::fmax(phi.z, t.z);
                            }
                            for (u32 fi = 0; fi < 6 && !poutside; ++fi) {
                                const f32 d = pl[fi][0] * (pl[fi][0] > 0 ? phi.x : plo.x)
                                            + pl[fi][1] * (pl[fi][1] > 0 ? phi.y : plo.y)
                                            + pl[fi][2] * (pl[fi][2] > 0 ? phi.z : plo.z)
                                            + pl[fi][3];
                                if (d < 0.0f) poutside = true;
                            }
                        }
                    }
                    if (poutside) continue;

                    rhi::MeshHandle pmesh = pit->second;
#if AVER_MODULE_TRIFACTOR
                    // Discrete per-level LOD: replicated safely (pure function). Same ladder
                    // lookup and chooseLevelCached call the colour walk's own branch makes below.
                    // GUARDED THE SAME WAY THE COLOUR WALK GUARDS ITS OWN, deliberately, or an
                    // instance without a usable box takes the ladder here and LOD 0 there -- the same divergence by a different route.
                    if (lodSelectEnabled_ && pHaveWorldBox) {
                        if (const auto plit = meshLods_.find(pmr->mesh); plit != meshLods_.end()) {
                            const MeshLodLadder& ladder = plit->second;
                            const Vec3 sphereCenter = (plo + phi) * 0.5f;
                            const f32 sphereRadius = dist(plo, phi) * 0.5f;   // world space, as
                                                                              // chooseLevelCached
                                                                              // requires
                            trifactor::View pview;
                            pview.eye = eye_;
                            pview.viewProj = viewProj_;
                            pview.viewportHeightPx = vpH_;
                            pview.verticalFovRadians = radians(60.0f);
                            const u32 plevel = trifactor::chooseLevelCached(
                                ladder.errorCm, sphereCenter, sphereRadius, lodErrorThresholdPx_, pview);
                            pmesh = ladder.handles[plevel];
                        }
                    }
#endif
                    // F6 (occlusion-fix-plan.md): this walk used to read the RAW `pmr->material`
                    // with no meshDefaultMaterial fallback and no per-part split -- a THIRD copy of
                    // the material-resolution rule, independently wrong in a way neither the entity
                    // loop nor (the now-deleted) submitShadowOnly was: a multi-material mesh always
                    // prepassed as whatever pmr->material happened to be (usually 0, "every plant"),
                    // never any individual part's own translucency. planEntityDraws() is the SAME
                    // split the colour walk uses below, so a mesh this walk excludes here (every
                    // part translucent) is exactly the mesh the colour walk's own `blended` gate
                    // (prepassEligibleBase's per-draw check) would also have excluded.
                    const i32 pmat = pmr->material ? pmr->material : meshDefaultMaterial(pmr->mesh);
                    const auto ppit = meshParts_.find(pmr->mesh);
                    aver::editor::PlannedDraw pdraws[kMaxPlannedDraws];
                    const u32 pdrawCount = aver::editor::planEntityDraws(
                        pit->second, pmesh,
                        ppit != meshParts_.end() ? ppit->second.data() : nullptr,
                        ppit != meshParts_.end() ? static_cast<u32>(ppit->second.size()) : 0u,
                        pmat, pdraws, kMaxPlannedDraws);
                    for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                        const aver::editor::PlannedDraw& pd = pdraws[pdi];
                        if (!pd.mesh) continue;
                        const ResolvedSurface prs = resolveSurface(pd.material);
                        // TRANSLUCENT: EXCLUDED, per part now, joining skinned/GPU-cluster/
                        // CPU-per-cluster -- but for a DIFFERENT reason: those three are excluded
                        // because depth is written some OTHER way; glass must not write depth AT
                        // ALL, EVER (the blended replay in endFrame runs with depth-WRITE off so a
                        // translucent surface never occludes what's behind it). Pre-writing opaque
                        // depth for a glass part here would leave that depth unconsumed by the
                        // colour loop's per-draw prepass gate (which also excludes `blended`) AND
                        // make every opaque object BEHIND the glass depth-test against a surface
                        // meant to be see-through, vanishing under it instead of showing through.
                        if (prs.look.blended) continue;
                        if (prs.matBytes)
                            e.device()->setDrawBinding(prs.matSet, prs.matConstants, prs.matBytes);
                        e.device()->drawMeshDepthPrepass(pd.mesh, &pwm.m[0][0]);
                    }
                }
            }
        }
#endif // AVER_MODULE_VOXI

        // IS THE FRAME CPU-BOUND OR GPU-BOUND? Establishing that took a dozen capture runs,
        // because --frame-time reports WHOLE frames from the CPU and time spent waiting for the
        // GPU looks exactly like CPU work. Two timers answer it directly: this one, and the
        // streamer's below. On Electric Dreams: 8.2ms walk + 0.7ms streaming inside a 76ms frame -- GPU-bound, so nothing here can matter.
        const auto tWalk0 = std::chrono::steady_clock::now();
        f64 dispatchMs = 0.0;

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
        // comment, SceneSubmission.hpp, for the full accounting) -- so it idles unless
        // occlusion.cullUnderSuppression overrides it back on. THE WALK RUNS IN onRender AFTER
        // beginFrame (Engine.cpp's own onUpdate/beginFrame/onRender order), so sceneSuppressed()
        // here is THIS frame's own election result, not a stale one from before the ray-driven
        // pass ran.
        const bool occlusionRuns = aver::editor::occlusionTestShouldRun(
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
                    if (const auto bit2 = meshBounds_.find(mr2->mesh); bit2 != meshBounds_.end()) {
                        auto* mw2 = const_cast<scene::CMeshRenderer*>(mr2);
                        mw2->aabbMin[0] = bit2->second.first.x;  mw2->aabbMin[1] = bit2->second.first.y;
                        mw2->aabbMin[2] = bit2->second.first.z;
                        mw2->aabbMax[0] = bit2->second.second.x; mw2->aabbMax[1] = bit2->second.second.y;
                        mw2->aabbMax[2] = bit2->second.second.z;
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

#if AVER_MODULE_VOXI
        // A CASTER THE CAMERA CANNOT SEE STILL CASTS A SHADOW. The frustum/occlusion/owner-hide
        // verdicts below used to skip DRAWING an entity via a bare `continue` past drawMesh() --
        // but drawMesh() is the ONLY thing that reaches the render features (submitDraw() is how
        // VoxiRenderer learns an entity exists), so a culled entity was absent from shadow
        // cascades, GI voxelisation and the RT TLAS: its shadow vanished the instant it left the
        // view. THAT IS THE "SHADOWS ARE SCREEN-SPACE" SYMPTOM, fair even though no shadowing
        // technique here is screen-space -- what FED the world-space cascades and RayQuery was the
        // camera frustum. F4 (occlusion-fix-plan.md) closes this by submitting the entity to the
        // features WITHOUT drawing it -- Voxi applies its own per-cascade cull in LIGHT space, the
        // cull a shadow caster should have gotten all along -- via the SAME emitEntityDraws() the
        // visible route uses, from the unified direct-route branch further down this walk. The
        // lambda that used to live here (submitShadowOnly) is gone: it read a different material
        // (mesh-slot-0's, not each part's own), dropped multi-part splits entirely, and had DRIFTED
        // on the translucency test from the visible path's copy -- see SceneSubmission.hpp's own
        // top comment for the full history this closes.
        //
        // BOUNDED BY ANGULAR SIZE -- the difference between free and unaffordable: submitting EVERY
        // culled entity was measured at +64ms/frame in ElectricDreams (5,884 of 6,617 entities
        // culled, mostly scatter plants), since the RT TLAS is a full rebuild every frame and
        // 759->6,571 instances took it from 9.1ms to 64.2ms. The floor is Voxi's own per-cascade
        // argument -- an object too small to fill a shadow texel cannot cast a visible shadow --
        // applied earlier, where it can stop the work, not just the draw. A NEGATIVE radius means
        // the caller had no bounds, so the entity submits regardless, matching the frustum cull's
        // own "must not vanish" rule. For scale: a crate at 5 m subtends ~0.2 rad and is kept; an
        // ankle-height plant at 100 m subtends ~0.003 rad and is not.
        constexpr f32 kMinCasterAngle = 0.02f;   // radians (~1.1 degrees)
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
#endif

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

        for (u32 oi = 0; oi < n; ++oi) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            const u32 i = occlusionOrder_.empty() ? oi : occlusionOrder_[oi];
            if (occlusionRuns && oi == occlusionPass1Count) occlusionBuildAndTest();
#else
            const u32 i = oi;
#endif
            const scene::Entity ent = w.at(i);
            if (w.destroyPending(ent)) continue;
            const scene::CMeshRenderer* mr =
                w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            // NO RENDERER, OR NOTHING ASSIGNED, IS THE ORDINARY CASE and says nothing: most
            // entities in a level carry no mesh at all.
            if (!mr || mr->mesh == 0) continue;
            // A MESH IS NAMED AND THE VISIBLE BIT IS CLEAR, which is NOT ordinary -- it is the
            // zero-fill trap. World::addComponent hands back zeroed storage and
            // kMeshRendererVisible is positive-sense, so a renderer attached directly is
            // attached, correct, and invisible; GraphComponentTree.cs:100-103 documents the same
            // trap on the C# side and dodges it by going through Entity.SetVisible.
            //
            // SPLIT OUT OF THE COMPOSITE GUARD PURELY TO SAY SO. This loop used to drop the
            // entity here with no diagnostic of any kind, which cost a day of bisection: an
            // entity that cannot draw said nothing at all. ONCE PER ENTITY, because this is a
            // per-entity per-frame walk and an unthrottled warning floods the log.
            if (!(mr->flags & scene::kMeshRendererVisible)) {
                if (undrawnInvisible_.insert(static_cast<u64>(ent)).second)
                    AVER_WARN("[Sandbox] entity {} names mesh id {} but its kMeshRendererVisible "
                              "bit is clear, so the scene walk skips it and it draws nothing. A "
                              "component attached directly arrives zero-filled -- attach through "
                              "Entity.SetVisible (EnsureMeshRenderer), which seeds the bit.",
                              static_cast<u64>(ent), mr->mesh);
                continue;
            }

            // ---- THE PLAYER START IS CHROME, AND IT DRAWS AS AN ICON INSTEAD ----
            // Skipping it HERE, by identity, drops it from the opaque pass, shadow cascade, GI and
            // RT acceleration structure all at once -- right for a marker: it should not cast a
            // shadow, bounce light, or appear in a reflection.
            // NOT BY CLEARING kMeshRendererVisible: ray picking asks the IDENTICAL question, so
            // unsetting the flag would also remove the ability to click the marker. Already
            // special-cased by identity in loadLevel/unloadLevel/addPlayerStart.
            // The icon itself is queued further down, sharing the selection outline's
            // is-anything-playing test.
            if (ent == playerStart_ && viewportIconsReady_) continue;

            const auto it = sceneMeshes_.find(mr->mesh);
            if (it == sceneMeshes_.end()) {
                // AN ID THAT RESOLVES TO NOTHING. Said ONCE PER ID rather than per entity: many
                // entities can name the same missing mesh, and it is the id that identifies the
                // fault, not the entity that happened to reach it first.
                //
                // THE ID IS PRINTED RAW AND THAT IS NOT LAZINESS. meshPathById_ is populated by
                // loadProjectMeshes only for meshes that LOADED, so it is empty for exactly the
                // ids that land here -- there is nothing to translate with. fnv1a64 of the
                // authored path is what to grep the .ocgraph/.ocmap for.
                if (undrawnMissingMesh_.insert(mr->mesh).second)
                    AVER_WARN("[Sandbox] mesh id {} (named by entity {}) is not in sceneMeshes_, "
                              "so every entity naming it draws nothing. Built-in primitives are "
                              "seeded at startup and .ocmesh files are registered by "
                              "loadProjectMeshes from the project's content root -- an id that is "
                              "missing was never loaded under the string that was authored.",
                              mr->mesh, static_cast<u64>(ent));
                continue;
            }
            const Mat4& wm = w.worldMatrix(ent);

            // ---- OWNER HIDE, DECIDED BEFORE THE CULLS ----
            // Needed by chooseRoute() below regardless of which cull (if any) also applies: an
            // entity both frustum-culled and owner-hidden must still carry hiddenFromOwner=true on
            // its one (direct-route) delivery, or it would be primary-visible again once back on
            // screen -- the 0d3bcf1 regression chooseRoute's own contract pins a test against.
            // COSTS ESSENTIALLY NOTHING to hoist: the walk runs only for an entity that both
            // carries the flag and has a first-person viewer to hide from -- in practice one
            // entity, the possessed pawn's own body.
            // ANCESTOR WALK, NOT A DIRECT-PARENT COMPARE: a COMP tree can nest, so "this mesh's
            // owner" may be several hops above `ent`. World::setParent already refuses a cycle, so
            // this walk is guaranteed to reach kInvalidEntity and stop.
            bool ownerHiddenHere = false;
            if ((mr->flags & scene::kMeshRendererHiddenFromOwner) &&
                firstPersonPawn_ != scene::kInvalidEntity) {
                for (scene::Entity anc = ent; w.valid(anc); anc = w.parent(anc)) {
                    if (anc == firstPersonPawn_) { ownerHiddenHere = true; break; }
                }
            }

            // A STATIC entity gets its bounds from the asset. A skinned one already had them
            // written this frame by SkinnedScene from its ACTUAL POSE, so leave those alone --
            // overwriting with the rest box is exactly the popping this exists to stop.
            const bool skinned = skinnedScene_ && skinnedScene_->drawHandle(ent) != 0;
            if (!skinned)
                if (const auto bit = meshBounds_.find(mr->mesh); bit != meshBounds_.end()) {
                    auto* mw = const_cast<scene::CMeshRenderer*>(mr);
                    mw->aabbMin[0] = bit->second.first.x;  mw->aabbMin[1] = bit->second.first.y;
                    mw->aabbMin[2] = bit->second.first.z;
                    mw->aabbMax[0] = bit->second.second.x; mw->aabbMax[1] = bit->second.second.y;
                    mw->aabbMax[2] = bit->second.second.z;
                }

            // Frustum cull on the world-space extent of the entity's own box. A DEGENERATE box
            // is drawn rather than culled: an entity whose bounds were never filled in must not
            // vanish -- being conservative costs a draw call where being wrong costs a character.
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
                        // The corner FURTHEST along the plane normal. If even that one is behind,
                        // every corner is, and only then is the box definitely out.
                        const f32 d = pl[pi][0] * (pl[pi][0] > 0 ? whi.x : wlo.x)
                                    + pl[pi][1] * (pl[pi][1] > 0 ? whi.y : wlo.y)
                                    + pl[pi][2] * (pl[pi][2] > 0 ? whi.z : wlo.z)
                                    + pl[pi][3];
                        if (d < 0.0f) outside = true;
                    }
                    // F4 (occlusion-fix-plan.md): STORE the verdict rather than acting on it here.
                    // chooseRoute() (SceneSubmission.hpp), a few lines down once the occlusion
                    // verdict is known too, is the ONE place that decides what happens next -- a
                    // frustum-culled entity and an occlusion-culled one are now handled by the
                    // exact same code from there on. This used to `continue` right here, through
                    // the deleted submitShadowOnly lambda.
                    frustumCulled = outside;
                }
            }
            bool occlusionCulled = false;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            // PASS-2 ONLY: `oi < occlusionPass1Count` entities drew unconditionally before
            // occlusionBuildAndTest() ran. Everything below has ALREADY been tested against a
            // pyramid buildPyramid() built from every pass-1 draw -- but CORRECTED: that does NOT
            // make occlusionWasVisible(ent) "this frame's own fresh answer". testBatch()'s
            // readback is, at best, one call stale (Occlusion.hpp's corrected "TWO-PASS" section)
            // -- what actually landed in occlusionVisible_[ent] this frame, inside
            // occlusionBuildAndTest's RESULT APPLICATION, is the raw (motion-dilated) test result
            // ONLY when this frame's camera motion since the last basis AND the readback's own
            // staleness (IOcclusionCuller::readbackLagIsExactlyOneCall()) both stayed inside what the
            // trust gate above the box-collection loop is willing to trust; otherwise it is
            // forced to "visible" regardless of what the pyramid says. A box excluded from
            // occlusionBoxes_ (degenerate) was never tested and defaults to visible, same as
            // haveWorldBox==false for frustum culling above. F8: occlusionRuns (computed once,
            // above the box-collection pre-walk) replaces occlusionCullEnabled_ && occluder_ here.
            occlusionCulled = occlusionRuns && oi >= occlusionPass1Count && haveWorldBox &&
                !occlusionWasVisible(ent);
#endif
            // F4: THE ONE PLACE that decides who delivers this entity -- see SceneSubmission.hpp's
            // own top comment for the rule (culling may change WHO delivers the draws, never WHAT
            // is in them). showCulled sends an otherwise-culled, non-owner-hidden entity through
            // the raster route too, tinted, for the by-hand false-cull finder (section 3B).
            const aver::editor::RouteDecision route = aver::editor::chooseRoute(
                frustumCulled, occlusionCulled, ownerHiddenHere, occlusionShowCulled_);

            if (!route.raster) {
#if AVER_MODULE_VOXI
                // THE UNIFIED DIRECT ROUTE: frustum-culled, occlusion-culled or owner-hidden.
                // Still submitted to Voxi so shadows, GI voxelisation and the RT TLAS never depend
                // on what the camera itself can see -- see this walk's own "A CASTER THE CAMERA
                // CANNOT SEE STILL CASTS A SHADOW" comment above for the full history, and
                // SceneSubmission.hpp's deliver() for the exact translucent/hiddenFromOwner
                // formula emitEntityDraws() applies below (raster: hiddenFromOwner always false;
                // direct: route.hiddenFromOwner, which chooseRoute already set to ownerHiddenHere
                // on EVERY route, including frustum-culled-AND-owner-hidden -- the 0d3bcf1 fix).
                // KNOWN RESIDUAL, STATED RATHER THAN FIXED (occlusion-fix-plan.md F4): this route
                // never runs LOD/cluster selection, so a posed (skinned/soft-body) entity aside,
                // it always plans from the BASE mesh and its full part split. A VISIBLE entity
                // whose LOD or cluster cut substitutes a different, unsplit handle therefore
                // delivers ONE draw of that handle while the SAME entity, culled, delivers its
                // base parts -- only live with LOD selection or the CLI cluster paths (LODSELECT
                // is 0 in PTTest, so dormant here). Closing it means Voxi taking source geometry
                // independently of the raster LOD choice (the TLAS is keyed on d.mesh,
                // VoxiRenderer.cpp:1136) -- out of scope for this fix; the SceneSubmission.hpp
                // pure-function tests pin the rule as-is rather than hiding it.
                const rhi::MeshHandle chosenMesh = posedHandle(ent);
                const rhi::MeshHandle baseMeshForCull = chosenMesh ? chosenMesh : it->second;
                const Vec3 cullCentre = haveWorldBox
                    ? Vec3{(wlo.x + whi.x) * 0.5f, (wlo.y + whi.y) * 0.5f, (wlo.z + whi.z) * 0.5f}
                    : Vec3{0.0f, 0.0f, 0.0f};
                const f32 cullRadius = haveWorldBox
                    ? 0.5f * std::sqrt((whi.x - wlo.x) * (whi.x - wlo.x) +
                                        (whi.y - wlo.y) * (whi.y - wlo.y) +
                                        (whi.z - wlo.z) * (whi.z - wlo.z))
                    : -1.0f;
                // THE ANGULAR-SIZE FLOOR -- see its own comment above (moved here from the deleted
                // submitShadowOnly lambda) for the full "GATED ON WHETHER ANYONE BUT THE SHADOW
                // CARES" reasoning. Applied ONCE PER ENTITY, matching its old home exactly.
                bool angularFloorOk = true;
                if (!voxiAttached_ && cullRadius >= 0.0f) {
                    const f32 dx = cullCentre.x - camPos_.x, dy = cullCentre.y - camPos_.y,
                              dz = cullCentre.z - camPos_.z;
                    const f32 dsq = dx * dx + dy * dy + dz * dz;
                    if (dsq > cullRadius * cullRadius) {
                        const f32 d = std::sqrt(dsq);
                        if (2.0f * cullRadius / d < kMinCasterAngle) angularFloorOk = false;
                    }
                }
                if (angularFloorOk) {
                    // THE SAME FALLBACK THE VISIBLE PATH RESOLVES a few dozen lines down
                    // (`mr->material ? ... : meshDefaultMaterial`) -- passing the raw handle here
                    // meant a mesh whose material is 0 ("every plant", per meshDefaultMaterial's
                    // own comment) baked a flat grey into GI on exactly the frames it was
                    // off-screen, and its authored look on the frames it was not, flipping a value
                    // inside giDrawsKey every time it crossed the frustum edge.
                    const i32 directMat = mr->material ? mr->material : meshDefaultMaterial(mr->mesh);
                    const auto directParts = meshParts_.find(mr->mesh);
                    aver::editor::PlannedDraw pdraws[kMaxPlannedDraws];
                    const u32 pdrawCount = aver::editor::planEntityDraws(
                        it->second, baseMeshForCull,
                        directParts != meshParts_.end() ? directParts->second.data() : nullptr,
                        directParts != meshParts_.end()
                            ? static_cast<u32>(directParts->second.size()) : 0u,
                        directMat, pdraws, kMaxPlannedDraws);
                    emitEntityDraws(e, pdraws, pdrawCount, wm, route, /*prepassEligibleBase=*/false);
                    if (frustumCulled || occlusionCulled) {
                        culledDraws += pdrawCount;
                        if (pdrawCount > 1) ++culledMultiPart;
                    }
                }
#endif
                // Priority matches the OLD sequential-continue shape exactly (frustum/occlusion
                // checked before owner-hide used to mean a frustum-culled-AND-owner-hidden entity
                // was counted as culled, never as owner-hidden, even though route.hiddenFromOwner
                // -- via ownerHiddenHere above -- was always true regardless): this is a counting
                // convention only, not a correctness question.
                if (frustumCulled || occlusionCulled) ++culled; else ++ownerHidden;
                continue;
            }
            // THE ONE READ everything downstream keys off: surfaceMaterials_, surfaceLooks_,
            // the PBR binding set -- and, because VoxiRenderer::submitDraw stores whatever
            // matSet/matConstants it is handed verbatim, the TLAS instance's materialIndex, GI
            // voxelisation and the ray-driven alpha-mask cutout too. Resolving here is what makes
            // one fallback reach the renderer that is actually on screen.
            const i32 mat = mr->material ? mr->material : meshDefaultMaterial(mr->mesh);
            // F2: ONE call replaces the resolution this loop used to run by hand (surfaceMaterials_.
            // find, MaterialLibrary::desc, isTranslucent, surfaceLooks_.find, the MaterialSystem
            // lookup, the dead-handle warning, the once-per-name missing-material warning) -- see
            // resolveSurface's own comment. Kept in LOCALS, not just read off `rsEntity` at the
            // final dispatch, because the GPU-cluster path below reads col/metallic/roughness/
            // matSet/matConstants through the CONTEXT rather than the device, `blended` gates
            // whether that path may run at all, and the skin-scene-test override just below
            // overwrites `col` directly. `matConstantBytes` staying 0 (module compiled out, or the
            // system not ready) is what tells emitEntityDraws() below not to touch the device's
            // sticky draw binding at all -- see its own comment on why that must be a branch, not
            // an unconditional call with zeros.
            const ResolvedSurface rsEntity = resolveSurface(mat);
            f32 col[4] = {rsEntity.look.col[0], rsEntity.look.col[1], rsEntity.look.col[2],
                          rsEntity.look.col[3]};
            f32 metallic = rsEntity.look.metallic, roughness = rsEntity.look.roughness;
            const bool blended = rsEntity.look.blended;
            const rhi::BindingSetHandle matSet = rsEntity.matSet;
            const void* matConstants = rsEntity.matConstants;
            const u32 matConstantBytes = rsEntity.matBytes;
            // The scene test paints its two entities so a probe can tell which it is looking
            // at. Only ever active behind --skin-scene-test.
            if (skinScene_) {
                if (ent == static_cast<scene::Entity>(skinScene_->subjectEntity()))
                    aver::editor::SkinSceneTest::subjectColor(col);
                else if (ent == static_cast<scene::Entity>(skinScene_->referenceEntity()))
                    aver::editor::SkinSceneTest::referenceColor(col);
            }

            // THE SEAM, and it is one line because the design made it one: a skinned entity's
            // posed vertices live in a DIFFERENT MeshHandle sharing this one's index buffer, so
            // substituting the handle reaches every pass at once. Zero means "not skinned", never "not drawn".
            rhi::MeshHandle mesh = it->second;
            // Set true only by the GPU per-cluster path below (AVER_MODULE_TRIFACTOR only) when it
            // actually dispatches this instance's geometry -- declared unconditionally, like `mesh`
            // above, so the plain drawMesh() call at the end can check it regardless of the module.
            bool clusterDispatched = false;
#if AVER_MODULE_TRIFACTOR
            // Virtualized-geometry LOD selection (trifactor::ClusterAdapt/ClusterSelect), per-
            // LEVEL not per-cluster. Skipped for a skinned entity (its posed handle always wins)
            // and a mesh with no LOD ladder. NO CACHE: chooseLevelCached is a handful of float ops
            // touching no GPU resource, cheap enough to run fresh EVERY frame.
            // GPU per-cluster path (--lod-mesh-shader) wins over everything below when the mesh
            // has GPU cluster buffers AND the pipeline came up on this device: it dispatches the
            // geometry itself, so `mesh` is never substituted and drawMesh() at the end is skipped
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
            if (lodMeshShaderEnabled_ && lodMeshPipelineReady_ && !skinned && haveWorldBox && !blended) {
                if (const auto git = meshClusterGpu_.find(mr->mesh); git != meshClusterGpu_.end()) {
                    const MeshClusterGpu& gpu = git->second;
                    if (rhi::IRenderContext* ctx = e.device()->renderContext(); ctx && gpu.clusterCount) {
                        trifactor::View view;
                        view.eye = eye_;
                        view.viewProj = viewProj_;
                        view.viewportHeightPx = vpH_;
                        view.verticalFovRadians = radians(60.0f);
                        const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                        // ClusterFrameCB (b4): budget CLAMPED above zero here, on the CPU, before
                        // upload -- ASMain does not re-clamp -- and the six frustum planes copied
                        // verbatim from the SAME Frustum::fromViewProj the CPU reference calls.
                        ClusterFrameCB frameCb;
                        frameCb.budgetPx = std::max(lodErrorThresholdPx_, trifactor::kMinClusterBudgetPx);
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
                        const u32 shadingModel = unlit_ ? 1u : 0u;   // AVER_MODEL_UNLIT / STANDARD
                        std::memcpy(consts + 24, &shadingModel, sizeof(shadingModel));
                        consts[25] = 0.04f; consts[26] = 1.0f; consts[27] = 0.0f;
                        consts[28] = consts[29] = consts[30] = consts[31] = 0.0f;

                        const auto tDis0 = std::chrono::steady_clock::now();
                        ctx->setPipeline(lodMeshPipeline_);
                        ctx->setBindingSet(gpu.bindingSet, 0);
                        // THE MATERIAL, ON THE CONTEXT -- why the foliage on this path drew black.
                        // setDrawBinding above records the material on the DEVICE, which only
                        // forwards it to the context from inside drawMesh(), never called here. So
                        // dispatchMeshClusters' table-1 binding was whatever an EARLIER draw left
                        // sticky -- Voxi's fallback set, whose metal-rough map is white -- so
                        // metallic came out 1, kdAlbedo came out 0, and the diffuse lobe vanished:
                        // black, even though every value measured correct for a DIFFERENT material.
                        if (matSet) ctx->setDrawBinding(matSet, matConstants, matConstantBytes);
                        ctx->setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
                        ctx->setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frameCb, sizeof(frameCb));
#if AVER_MODULE_VOXI
                        // b3: VoxiFrame, the SAME bytes Voxi's own scenePass binds at b4 for the
                        // ordinary path (see kClusterGiFrameRegister's comment on why this path
                        // can't reuse b4). Bound every draw rather than once per mesh -- a root CBV
                        // pointer set is cheap -- keeping shadowFactor()/coneTracedIndirect() valid
                        // even before Voxi finishes init() (giFrameConstants() returns an all-zero block, degrading the same way Voxi's own checks do).
                        ctx->setConstantBuffer(kClusterGiFrameRegister, voxiRenderer_.giFrameConstants(),
                                               voxiRenderer_.giFrameConstantBytes());
#endif
                        ctx->dispatchMeshClusters(mesh, gpu.clusterCount);
                        dispatchMs += std::chrono::duration<f64, std::milli>(
                            std::chrono::steady_clock::now() - tDis0).count();
                        clusterDispatched = true;

#if AVER_MODULE_VOXI
                        // DEFECT 2's FIX, in full: this instance's LIT pixels already came from
                        // dispatchMeshClusters above, so drawMesh() is skipped for it -- and
                        // IRenderFeature::submitDraw is called from EXACTLY ONE place in the
                        // engine, D3D12Device::drawMesh. Skipping drawMesh() therefore also skips
                        // submitDraw(), the ONLY way geometry reaches VoxiRenderer::draws_ -- so
                        // this instance never appeared in shadowPass, giShadowPass or
                        // voxelizePass. A tree that casts no shadow is not a cheaper tree, it's wrong.
                        //
                        // F4 (occlusion-fix-plan.md): goes through planEntityDraws()/
                        // emitEntityDraws() now, same as every other route, instead of a single
                        // hand-written submit() naming only `mesh` -- that used to submit a
                        // multi-material mesh as ONE draw carrying the ENTITY's material, silently
                        // losing the per-part split the raster and direct-cull routes both already
                        // had. `mesh` still equals `it->second` here (no LOD/skin substitution has
                        // run yet at this point in the walk), so this is exactly F4's "goes through
                        // planEntityDraws with chosenMesh = it->second" case. submit() still
                        // resolves d.depthMesh through the SAME depthProxyFn_ every ordinary
                        // instance goes through, so depth-only passes draw the cheap proxy with no
                        // new resolution logic.
                        {
                            const auto clusterParts = meshParts_.find(mr->mesh);
                            aver::editor::PlannedDraw cdraws[kMaxPlannedDraws];
                            const u32 cdrawCount = aver::editor::planEntityDraws(
                                it->second, mesh,
                                clusterParts != meshParts_.end() ? clusterParts->second.data() : nullptr,
                                clusterParts != meshParts_.end()
                                    ? static_cast<u32>(clusterParts->second.size()) : 0u,
                                mat, cdraws, kMaxPlannedDraws);
                            // A DIRECT-ONLY delivery: colour already came from dispatchMeshClusters
                            // just above, so this call exists purely to register the shadow/GI/TLAS
                            // submission drawMesh() would otherwise have made. Never owner-hidden or
                            // tinted -- this whole block only ever runs for a route.raster entity
                            // (the culled branch above already `continue`d before reaching here).
                            const aver::editor::RouteDecision clusterRoute{false, false, false};
                            emitEntityDraws(e, cdraws, cdrawCount, wm, clusterRoute,
                                            /*prepassEligibleBase=*/false);
                        }
#endif

                        ++lodMeshShaderStats_.instancesTested;
                        lodMeshShaderStats_.clustersDispatched += gpu.clusterCount;

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
                        if (lodClusterStatsEnabled_ && lodClusterFrame_ % 64 == 0) {
                            if (const auto cit2 = meshClusterData_.find(mr->mesh); cit2 != meshClusterData_.end()) {
                                const MeshClusterData& mcd = cit2->second;
                                bool sampledShortcut = false;
                                if (const auto lit2 = meshLods_.find(mr->mesh); lit2 != meshLods_.end()) {
                                    const MeshLodLadder& ladder2 = lit2->second;
                                    const Vec3 sphereCenter2 = (wlo + whi) * 0.5f;
                                    const f32 sphereRadius2 = dist(wlo, whi) * 0.5f;
                                    const u32 candidateLevel2 = trifactor::chooseLevelCached(
                                        ladder2.errorCm, sphereCenter2, sphereRadius2,
                                        lodErrorThresholdPx_, view);
                                    if (candidateLevel2 < mcd.levelBounds.size() &&
                                        trifactor::provablySingleLevelCut(
                                            mcd.levelBounds, candidateLevel2, sphereCenter2, sphereRadius2,
                                            mcd.maxSphereRadius * worldScale, lodErrorThresholdPx_, view)) {
                                        lodMeshShaderStats_.survivors += mcd.levelBounds[candidateLevel2].count;
                                        lodMeshShaderStats_.trianglesDrawn +=
                                            mcd.levelBounds[candidateLevel2].triangleCount;
                                        lodMeshShaderStats_.maxDistinctLevelsSeen =
                                            std::max(lodMeshShaderStats_.maxDistinctLevelsSeen, 1u);
                                        ++lodMeshShaderStats_.instancesShortcut;
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
                                        worldClusters, lodErrorThresholdPx_, view, true);
                                    lodMeshShaderStats_.survivors += cr.stats.drawn;
                                    lodMeshShaderStats_.trianglesDrawn += cr.stats.trianglesAfter;
                                    if (cr.stats.distinctLevels > 1) ++lodMeshShaderStats_.instancesMixedLevels;
                                    lodMeshShaderStats_.maxDistinctLevelsSeen =
                                        std::max(lodMeshShaderStats_.maxDistinctLevelsSeen, cr.stats.distinctLevels);
                                }
                            }
                        }
                    }
                }
            }
            // PER-CLUSTER path wins over per-LEVEL when both are enabled: this is what actually
            // mixes LOD levels within one instance's draw; the per-level `else if` below is
            // entirely unchanged, still reachable and reproducing pre-existing behaviour when --lod-per-cluster is off.
            if (!clusterDispatched && lodPerClusterEnabled_ && !skinned && haveWorldBox) {
                if (const auto cit = meshClusterData_.find(mr->mesh); cit != meshClusterData_.end()) {
                    const MeshClusterData& cd = cit->second;
                    trifactor::View view;
                    view.eye = eye_;
                    view.viewProj = viewProj_;
                    view.viewportHeightPx = vpH_;
                    view.verticalFovRadians = radians(60.0f);
                    const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                    // THE INSTANCE-LEVEL SHORTCUT (ClusterAdapt.hpp; TrifactorTest proves it
                    // against the real scan): if provablySingleLevelCut can PROVE the real
                    // O(all-DAG-clusters) scan would select exactly the whole level it names, draw
                    // that level's already-resident ladder handle directly, skipping the
                    // copy/transform/scan. Falls through to the real scan when the proof doesn't
                    // hold.
                    bool tookShortcut = false;
                    if (const auto lit = meshLods_.find(mr->mesh); lit != meshLods_.end()) {
                        const MeshLodLadder& ladder = lit->second;
                        const Vec3 sphereCenter = (wlo + whi) * 0.5f;
                        const f32 sphereRadius = dist(wlo, whi) * 0.5f;
                        const u32 candidateLevel = trifactor::chooseLevelCached(
                            ladder.errorCm, sphereCenter, sphereRadius, lodErrorThresholdPx_, view);
                        if (candidateLevel < ladder.handles.size() &&
                            trifactor::provablySingleLevelCut(
                                cd.levelBounds, candidateLevel, sphereCenter, sphereRadius,
                                cd.maxSphereRadius * worldScale, lodErrorThresholdPx_, view)) {
                            mesh = ladder.handles[candidateLevel];
                            tookShortcut = true;
                            ++lodClusterStats_.instancesShortcut;
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
                        worldClusters, lodErrorThresholdPx_, view, true /* useFrustum */);

                    ++lodClusterStats_.instancesTested;
                    lodClusterStats_.clustersTested += cr.stats.tested;
                    lodClusterStats_.frustumCulled += cr.stats.frustumCulled;
                    lodClusterStats_.coneCulled += cr.stats.coneCulled;
                    lodClusterStats_.lodRejected += cr.stats.lodRejected;
                    lodClusterStats_.clustersDrawn += cr.stats.drawn;
                    lodClusterStats_.trianglesDrawn += cr.stats.trianglesAfter;
                    if (const auto tIt = meshTris_.find(mr->mesh); tIt != meshTris_.end())
                        lodClusterStats_.trianglesBeforeLod0 += tIt->second;
                    if (cr.stats.distinctLevels > 1) ++lodClusterStats_.instancesMixedLevels;
                    lodClusterStats_.maxDistinctLevelsSeen =
                        std::max(lodClusterStats_.maxDistinctLevelsSeen, cr.stats.distinctLevels);

                    // Sort for a cheap, order-independent "did the cut change since last frame"
                    // comparison. The cut is expected to be STABLE frame to frame (the camera moves
                    // continuously, not by a full LOD jump), so this is a cache hit most frames once settled.
                    std::vector<u32> selectedIds = cr.drawnIds;
                    std::sort(selectedIds.begin(), selectedIds.end());

                    ClusterCutCache& cache = clusterCutCache_[ent];
                    cache.lastUsedFrame = lodClusterFrame_;
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
                            e.device()->createMesh(cd.verts.data(), (u32)cd.verts.size(),
                                                    assembled.data(), (u32)assembled.size());
                        const auto t1 = std::chrono::steady_clock::now();
                        lodClusterStats_.rebuildMs +=
                            std::chrono::duration<f64, std::milli>(t1 - t0).count();
                        lodClusterStats_.rebuildIndices += assembled.size();
                        ++lodClusterStats_.rebuilds;

                        if (newHandle) {
                            if (cache.handle) { depthProxy_.erase(cache.handle); e.device()->destroyMesh(cache.handle); }
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
                            depthProxy_[newHandle] = mesh;
                        }
                        // newHandle == 0 (empty cut this frame, or the device refused): keep
                        // whatever handle the cache already had (fail-safe), or fall through to the
                        // LOD-0 whole-mesh handle `mesh` already holds for the very first frame.
                    } else {
                        ++lodClusterStats_.cacheHits;
                    }
                    if (cache.handle) mesh = cache.handle;
                    }   // !tookShortcut
                }
            } else if (!clusterDispatched && lodSelectEnabled_ && !skinned && haveWorldBox) {
                if (const auto lit = meshLods_.find(mr->mesh); lit != meshLods_.end()) {
                    const MeshLodLadder& ladder = lit->second;
                    const Vec3 sphereCenter = (wlo + whi) * 0.5f;
                    const f32 sphereRadius = dist(wlo, whi) * 0.5f;   // half the box diagonal:
                                                                      // encloses the box exactly, same conservative shape ClusterSelect's own
                                                                      // sphere tests assume.
                    trifactor::View view;
                    view.eye = eye_;
                    view.viewProj = viewProj_;
                    view.viewportHeightPx = vpH_;
                    view.verticalFovRadians = radians(60.0f);   // matches the literal at this
                                                                 // frame's own proj build, above
                    const u32 level = trifactor::chooseLevelCached(
                        ladder.errorCm, sphereCenter, sphereRadius, lodErrorThresholdPx_, view);
                    mesh = ladder.handles[level];

                    ++lodStats_.instancesTested;
                    if (level > 0) ++lodStats_.levelCollapsed;
                    lodStats_.trianglesBeforeLod0 += ladder.triCounts.front();
                    lodStats_.trianglesAfterLevel += ladder.triCounts[level];

                    // Informational cluster-cull telemetry for the CHOSEN level only -- real,
                    // tested, but not subtracted from trianglesAfterLevel: this slice draws the
                    // whole chosen level.
                    // ladder.clusters[level] holds MESH-LOCAL bounds; view.viewProj/frustum are
                    // WORLD space, so a working copy is transformed by this instance's world
                    // matrix first. Radius/axis use a UNIFORM-scale approximation -- fine for an
                    // INFORMATIONAL counter that never reaches the draw call.
                    if (lodClusterStatsEnabled_ &&
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
                        lodStats_.clustersTested += sr.stats.tested;
                        lodStats_.frustumCulled += sr.stats.frustumCulled;
                        lodStats_.coneCulled += sr.stats.coneCulled;
                    }
                }
            }
#endif
            // WHICH FEATURE OWNS THIS ENTITY'S VERTICES -- posedHandle() (F4), the SAME helper the
            // direct route above already called, so the seam agrees with itself instead of two
            // near-identical hand-written copies: skinning checked first (nothing forbids
            // CSoftBody on an already-skinned mesh, so asking skinning first makes the collision
            // deterministic), soft body only filling in where it declined.
            if (const rhi::MeshHandle substituted = posedHandle(ent)) mesh = substituted;
            // F6/F4: the ENTITY-level half of depth-prepass eligibility. BLENDED IS NO LONGER
            // CHECKED HERE, unlike the shape this replaces: it is now a PER-DRAW question,
            // decided inside emitEntityDraws() from each draw's own resolveSurface() result,
            // because F6 made the depth-prepass walk itself split into parts and skip translucent
            // ones individually -- a single entity-level check here would be wrong for a mesh with
            // both an opaque trunk and a translucent leaf part. Still mirrors the depth-prepass
            // walk's OTHER exclusions exactly -- skinned, GPU cluster dispatch, CPU per-cluster.
            bool prepassEligibleBase = false;
#if AVER_MODULE_VOXI
            {
                prepassEligibleBase = e.device()->depthPrepassEnabled() && !clusterDispatched && !skinned;
#if AVER_MODULE_TRIFACTOR
                if (prepassEligibleBase && lodMeshShaderEnabled_ && lodMeshPipelineReady_ &&
                    meshClusterGpu_.count(mr->mesh)) prepassEligibleBase = false;
                if (prepassEligibleBase && lodPerClusterEnabled_ && meshClusterData_.count(mr->mesh))
                    prepassEligibleBase = false;
#endif
            }
#endif
            // The GPU per-cluster path already dispatched this instance's geometry -- drawing
            // again here would double-draw. setDrawBinding/setDrawBlended/setNextDrawPrepassed are
            // now all set PER DRAW, inside emitEntityDraws() below (F3) -- not here beforehand --
            // since a multi-part mesh's parts can each carry a different material/blend/prepass
            // answer, the same reason drawMeshParts used to override this loop's own
            // setDrawBlended(blended) call per part.
            if (!clusterDispatched) {
                // F4: THE ONE PLACE 6360-6365/8297-8299's old duplication used to live -- see
                // planEntityDraws' own comment (SceneSubmission.hpp) for the exact rule (a mesh
                // that names several materials draws as several meshes, one per slot; a
                // substituted handle -- LOD or posed -- keeps today's single draw and the entity's
                // own material, since the split was cut from the UNSUBSTITUTED geometry).
                const auto pit = meshParts_.find(mr->mesh);
                aver::editor::PlannedDraw pdraws[kMaxPlannedDraws];
                const u32 pdrawCount = aver::editor::planEntityDraws(
                    it->second, mesh,
                    pit != meshParts_.end() ? pit->second.data() : nullptr,
                    pit != meshParts_.end() ? static_cast<u32>(pit->second.size()) : 0u,
                    mat, pdraws, kMaxPlannedDraws);
                emitEntityDraws(e, pdraws, pdrawCount, wm, route, prepassEligibleBase);
            }
            // EVERY SELECTED ENTITY, NOT ONLY THE ANCHOR. This kept one Mat4 and one mesh id,
            // so a multi-selection was highlighted in the Outliner tree and invisible in the 3D
            // view -- which is where the objects are. Selecting five props and dragging them
            // showed an outline on one of the five.
            if (sel_ == kSelScene && (ent == selEntity_ || multiIsSelected(ent))) {
                selectionOutline_ = wm; selectionMesh_ = mesh; selectionMeshId_ = mr->mesh;
                hasSelection_ = true;
                // The anchor stays in the scalars above (other code reads them); the rest
                // accumulate here. Cleared with hasSelection_ at the draw site, so a stale
                // entry cannot outlive the frame that produced it.
                selectionOutlines_.push_back({wm, mr->mesh});
            }
            ++drawn;
        }
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
                      culled, culledDraws, culledMultiPart);
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
                AVER_INFO("[Sandbox] scene walk {:.1f}ms -- {:.1f}ms in cluster dispatch across {} "
                          "drawn ({:.1f}us each), {:.1f}ms in the rest over {} entities",
                          walkMs, dispatchMs, drawn,
                          drawn ? dispatchMs * 1000.0 / static_cast<f64>(drawn) : 0.0,
                          walkMs - dispatchMs, n);
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
        if (drawn != lastSceneDrawn_ || culled != lastSceneCulled_ || ownerHidden != lastSceneOwnerHidden_) {
            // F4 (occlusion-fix-plan.md): `culled` now counts an occlusion-culled entity too, not
            // only a frustum-culled one -- previously the occlusion branch incremented no counter
            // at all, so occlusion's own contribution was invisible here. Relabelled from
            // "frustum-culled" to plain "culled" to match; see chooseRoute()'s own priority
            // ordering for which counter an entity that is BOTH culled and owner-hidden lands in.
            AVER_INFO("[Sandbox] scene-render: {} spawned CMeshRenderer entit{} drawn, {} culled, {} owner-hidden",
                      drawn, drawn == 1 ? "y" : "ies", culled, ownerHidden);
            lastSceneDrawn_ = drawn;
            lastSceneCulled_ = culled;
            lastSceneOwnerHidden_ = ownerHidden;
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
    if (hasSelection_ && maxFrames_ == 0 && !anyPlayActive() && !noEditorChrome_) {
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
    // HIDDEN BY THE SAME anyPlayActive() THE OUTLINE ABOVE USES, deliberately, so the two can't
    // drift apart.
    // NOT GATED ON maxFrames_, unlike the outline: the outline responds to a click, meaningless in
    // a bounded run; the marker is part of what the level LOOKS like.
#if AVER_MODULE_SCENE
    if (viewportIconsReady_ && !noEditorChrome_ && !anyPlayActive() &&
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
    aver::editor::SurfaceInputs in;
#if AVER_MODULE_PBR
    if (const auto it = surfaceMaterials_.find(mat); it != surfaceMaterials_.end()) rs.authored = it->second;
    in.authored = rs.authored != 0;
    const pbr::MaterialDesc* d = rs.authored ? pbr::MaterialLibrary::get().desc(rs.authored) : nullptr;
    in.authoredLive = d != nullptr;
    in.translucent = d != nullptr && pbr::isTranslucent(*d);
#endif
    if (const auto lookIt = surfaceLooks_.find(mat); lookIt != surfaceLooks_.end()) {
        in.haveLook = true;
        in.lookCol[0] = lookIt->second.col[0];
        in.lookCol[1] = lookIt->second.col[1];
        in.lookCol[2] = lookIt->second.col[2];
        in.lookMetallic = lookIt->second.metallic;
        in.lookRoughness = lookIt->second.roughness;
    }
    rs.look = aver::editor::resolveSurfaceLook(in);
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
                      aver_scene_material_name(mat));
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

// F3: THE ONE EMITTER. For every planned draw, resolveSurface() once and hand the result to
// whichever route actually delivers it -- raster's drawMesh() or Voxi's direct submit() -- so
// both produce the SAME Draw record for the same material (translucent = look.blended on both;
// hiddenFromOwner = false on raster, route.hiddenFromOwner on direct -- see SceneSubmission.hpp's
// deliver(), which this hand-writes rather than calls, to keep col/metallic/roughness and the
// translucent/hiddenFromOwner decision reading from the exact same ResolvedSurface in one place).
//
// prepassEligibleBase is the ENTITY-level half of the depth-prepass eligibility test (formerly
// 6323-6341's shape, minus the blended check, which F6 made a PER-PART question): with the depth
// prepass walk now writing one drawMeshDepthPrepass per part and skipping translucent ones
// individually, a single setNextDrawPrepassed(true) call before a multi-part loop would only
// cover draw 0 -- the flag is AUTO-CONSUMED by the very next drawMesh(), not sticky (RHI.hpp's own
// comment on setNextDrawPrepassed) -- silently asking the LessEqual/no-write pipeline for parts
// the prepass walk never wrote depth for. Deciding it per draw, right here, is what keeps the two
// walks in lockstep at the new per-part granularity. `(void)` up front because a non-VOXI build
// never reads it below (the whole prepass feature is VOXI-only) and an unreferenced-parameter
// warning on a build that never fires it would be a strange place for /W4 to complain.
void SandboxApp::emitEntityDraws(Engine& e, const aver::editor::PlannedDraw* draws, u32 n, const Mat4& wm,
                    const aver::editor::RouteDecision& route, bool prepassEligibleBase) {
    (void)prepassEligibleBase;
    for (u32 i = 0; i < n; ++i) {
        const aver::editor::PlannedDraw& d = draws[i];
        if (!d.mesh) continue;
        const ResolvedSurface rs = resolveSurface(d.material);
        f32 col[4] = {rs.look.col[0], rs.look.col[1], rs.look.col[2], rs.look.col[3]};
        // occlusion.showCulled: (1, 0.15, 1) knocks the green channel down so a false cull reads
        // as an obvious magenta tint. A BRANCH, not a multiply that silently becomes identity at
        // 1.0 when the debug view is off -- see EditorConsole.hpp's own doc comment on the cost
        // this is supposed to be ("one bool test per culled entity").
        if (route.tint) col[1] *= 0.15f;
        if (route.raster) {
            // Only when a live MaterialSystem actually resolved something -- calling this with
            // rs.matBytes == 0 (module compiled out, or the system not ready yet) would STOMP
            // whatever binding a previous draw left sticky (RHI.hpp's own "sticky until changed"
            // contract on setDrawBinding), which is not what "nothing to bind" ever meant before.
            if (rs.matBytes) e.device()->setDrawBinding(rs.matSet, rs.matConstants, rs.matBytes);
            e.device()->setDrawBlended(rs.look.blended);
#if AVER_MODULE_VOXI
            if (prepassEligibleBase && !rs.look.blended) e.device()->setNextDrawPrepassed(true);
#endif
            e.device()->drawMesh(d.mesh, &wm.m[0][0], col, rs.look.metallic, rs.look.roughness);
        }
#if AVER_MODULE_VOXI
        else {
            // THE UNIFIED DIRECT ROUTE: frustum-culled, occlusion-culled or owner-hidden, still
            // handed to Voxi so shadows/GI/the RT TLAS never depend on what the camera itself can
            // see -- see the call site's own comment for the "shadows are screen-space" history
            // this closes.
            //
            // F5: translucent = rs.look.blended, the SAME value the raster branch just above feeds
            // setDrawBlended() -- glass leaving the frustum now joins the translucent lane exactly
            // like visible glass, matching VoxiRenderer::submitDraw's own opaque/blended split
            // (VoxiRenderer.cpp:861-880, read-only citation, not edited by this lane) and the "10b"
            // contract that glass DOES cast an attenuated shadow now (VoxiRenderer.cpp:4603-4615,
            // read-only citation) -- the stale comment this replaces (formerly 5556-5566, deleted
            // with submitShadowOnly) claimed the opposite, a contract that no longer existed. The
            // exclusion from voxelisation/cascades/the GI shadow map is unchanged: submit() still
            // reads this same `translucent` flag to route into the non-opaque TLAS lane.
            voxiRenderer_.submit(d.mesh, &wm.m[0][0], col, rs.look.metallic, rs.look.roughness,
                                 rs.matSet, rs.matConstants, rs.matBytes,
                                 /*translucent=*/rs.look.blended, route.hiddenFromOwner);
            // THE PATH TRACER NEEDS THE SAME OFF-SCREEN GEOMETRY, AND THIS IS ITS ONLY WAY IN.
            // PtSceneView::submitDraw is otherwise reached only through drawMesh(), which this
            // branch exists to skip -- so the path-traced view traced a scene holding only what
            // the camera could see (and no cluster-dispatched instance at all): no roof overhead,
            // no wall behind the camera. Measured on PTTest NewSponza: Voxi's TLAS held 400
            // instances while the path tracer "re-armed on 154", with 73 entities frustum-culled.
            // Owner-hidden draws stay out: PtSceneView has no owner-hidden mask lane, so it would
            // paint the owner's own body over the camera, and the raster route never hands it
            // those either.
            if (ptSceneView_ && !route.hiddenFromOwner)
                ptSceneView_->submitDraw(d.mesh, &wm.m[0][0], col, rs.look.metallic, rs.look.roughness,
                                         rs.matSet, rs.matConstants, rs.matBytes, rs.look.blended);
        }
#endif
    }
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

#endif

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
    const bool rayDrivenPaints = voxiRenderer_.willSuppressSceneThisFrame();
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

#if AVER_MODULE_SCENE
#if AVER_MODULE_SCENE
 void SandboxApp::accumulateStreamStats(world::StreamStats& into, const world::StreamStats& add) {
    into.residentChunks   += add.residentChunks;
    into.residentEntities += add.residentEntities;
    into.loadedThisUpdate += add.loadedThisUpdate;
    into.evictedThisUpdate += add.evictedThisUpdate;
    into.entitiesIn       += add.entitiesIn;
    into.entitiesOut      += add.entitiesOut;
    into.pendingLoads     += add.pendingLoads;
    into.failedLoads      += add.failedLoads;
    into.totalLoads       += add.totalLoads;
}

#endif
#endif

 rhi::MeshHandle SandboxApp::depthProxyLookup(rhi::MeshHandle mesh, void* user) {
    const auto& m = static_cast<const SandboxApp*>(user)->depthProxy_;
    const auto it = m.find(mesh);
    return it == m.end() ? 0 : it->second;
}

} // namespace aver
