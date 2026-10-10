// Mesh and material loading: load/release wrappers around aver::game::GameContent, and editor-only
// tables (pick geometry, LOD ladder, depth proxies, cluster data).

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// Build-time half: giShaderPrelude()/giShaderDefines() depend only on register numbers, not user pointer.
 bool SandboxApp::particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                              std::string* outPrelude, std::string* outDefines, void* user) {
    (void)user;
    if (!outPrelude || !outDefines) return false;
    *outPrelude = voxi::giShaderPrelude();
    *outDefines = voxi::giShaderDefines(srvBase, samplerBase, cbRegister);
    return true;
}

// Per-frame half: forwards to voxiRenderer_'s bindGiResources/giFrameConstants.
 void SandboxApp::particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                           const void** outCbData, u32* outCbBytes, void* user) {
    auto* self = static_cast<SandboxApp*>(user);
    if (!self || !outCbData || !outCbBytes) return;
    self->voxiRenderer_.bindGiResources(res, set, srvBase);
    *outCbData = self->voxiRenderer_.giFrameConstants();
    *outCbBytes = self->voxiRenderer_.giFrameConstantBytes();
}

#endif

// Loads every .ocmesh, builds editor tables (pick geometry, LOD ladder, depth proxies, cluster data).
void SandboxApp::loadProjectMeshes(Engine& e) {
#if AVER_MODULE_SCENE
    // setStaticMeshHeapDefault must run before any createMesh call.
    if (e.device()) e.device()->setStaticMeshHeapDefault(meshHeapDefault_);
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    MeshLoadPass pass;
    pass.app = this;
    pass.engine = &e;
    content_.setBuildDepthProxies(false);   // editor builds the LOD ladder and its own depth proxies
    content_.setMeshLoadedHook(&SandboxApp::onMeshLoaded, &pass);
    content_.loadProjectMeshes(*e.device());
    content_.setMeshLoadedHook(nullptr, nullptr);

    // Lazy (streamed) meshes upload later via acquireMesh: run the same registration for them. The pass
    // is static so it outlives this call (app and engine outlive the content).
    static MeshLoadPass s_lazyPass;
    s_lazyPass.app = this;
    s_lazyPass.engine = &e;
    content_.setMeshAcquiredHook(&SandboxApp::onMeshLoaded, &s_lazyPass);
    content_.setMeshReleasedHook([](u64 id, void* user) {
        auto* p = static_cast<MeshLoadPass*>(user);
        if (!p || !p->app || !p->engine) return;
        SandboxApp& app = *p->app;
        app.meshTris_.erase(id);
        app.pickGeometry_.erase(id);
        app.skinnedMeshIds_.erase(id);   // meshPathById_ stays: it is only a path lookup
#if AVER_MODULE_PBR
        if (const auto oit = app.selOutlineLines_.find(id); oit != app.selOutlineLines_.end()) {
            if (oit->second) p->engine->device()->destroyLineMesh(oit->second);
            app.selOutlineLines_.erase(oit);
        }
#endif
#if AVER_MODULE_TRIFACTOR
        if (const auto lit = app.meshLods_.find(id); lit != app.meshLods_.end()) {
            for (const rhi::MeshHandle lh : lit->second.handles) app.depthProxy_.erase(lh);
            app.meshLods_.erase(lit);
        }
        app.meshClusterData_.erase(id);
#endif
    }, &s_lazyPass);

#if AVER_MODULE_TRIFACTOR
    // Run-wide LOD summary: vertex-buffer sharing trade is measured against this.
    if (pass.lodCoarserLevels || pass.lodSharedLevels)
        AVER_INFO("[Mesh] LOD ladders: {} coarser level(s), {} sharing their LOD0 vertex buffer "
                  "({:.1f} MiB of vertex data not duplicated)",
                  pass.lodCoarserLevels, pass.lodSharedLevels,
                  static_cast<f64>(pass.lodSharedVertexBytesSaved) / (1024.0 * 1024.0));
#endif
#if AVER_MODULE_LANDSCAPE
    // Rebuild foliage palette now: this is when the set of placeable meshes changes.
    refreshFoliagePalette();
#endif
#else
    (void)e;
#endif
}

#if AVER_MODULE_SCENE
// Per-mesh hook: builds pick geometry, triangle counts, skinned ids, LOD ladder, depth proxies, cluster data.
void SandboxApp::onMeshLoaded(const game::GameContent::LoadedMesh& m, void* user) {
    auto* pass = static_cast<MeshLoadPass*>(user);
    if (!pass || !pass->app || !pass->engine) return;
    SandboxApp& app = *pass->app;
    Engine& e = *pass->engine;

    if (!m.data) {
        // Built-in (sphere/cube/drone): only pick geometry and triangle count.
        app.meshTris_[m.id] = static_cast<u32>(m.indices.size() / 3);
        app.pickGeometry_[m.id] = buildPickGeometry(m.vertices, m.indices);
        return;
    }

    const u64 id = m.id;
    const std::string& rel = m.relativePath;
    const fmt::OcMeshData& md = *m.data;
    const std::vector<rhi::MeshVertex>& verts = m.vertices;
    const rhi::MeshHandle h = m.handle;

    // Project mesh can override a built-in's id: clear the built-in's cached pick geometry.
    app.pickGeometry_.erase(id);
    // Track path back from id for palette/future asset picker.
    app.meshPathById_[id] = rel;
    app.meshTris_[id] = static_cast<u32>(md.indices.size() / 3);
    if (md.hasSkin()) app.skinnedMeshIds_.insert(id);
    AVER_INFO("[Mesh] '{}' -> {} verts, {} indices, lodCount={}, coarserLods={}, meshlets={}",
              rel, verts.size(), md.indices.size(), md.lodCount(), md.coarserLods.size(),
              md.meshlets.size());

#if AVER_MODULE_TRIFACTOR
        // Build one MeshHandle per LOD level at load time. All levels share verts (same vertex buffer).
        if (md.lodCount() > 1) {
            MeshLodLadder ladder;
            const u32 levels = md.lodCount();
            ladder.handles.reserve(levels);
            ladder.triCounts.reserve(levels);
            ladder.errorCm.reserve(levels);
            ladder.clusters.resize(levels);

            ladder.handles.push_back(h);
            ladder.triCounts.push_back(app.meshTris_[id]);
            ladder.errorCm.push_back(0.0f);
            trifactor::buildLevelClusterViews(md, 0, ladder.clusters[0]);

            bool ok = true;
            for (u32 lvl = 1; lvl < levels && ok; ++lvl) {
                const fmt::OcMeshLod& lod = md.coarserLods[lvl - 1];
                // W11 (--lod-share-vertices): try sharing LOD0's own vertex buffer (`h`) first --
                // 0 back means the device refused (an unsupported backend, or `h`'s vertices are
                // compute-written, e.g. a skin target -- see createMeshSharingVertices' own
                // comment for the full list) and the caller MUST fall back, exactly as if the flag
                // were off. Off by default, so this is a no-op call on the common path.
                rhi::MeshHandle lh = app.lodShareVertices_
                    ? e.device()->createMeshSharingVertices(h, lod.indices.data(), (u32)lod.indices.size())
                    : 0;
                if (lh) {
                    ++pass->lodSharedLevels;
                    pass->lodSharedVertexBytesSaved += static_cast<u64>(verts.size()) * sizeof(rhi::MeshVertex);
                } else {
                    lh = e.device()->createMesh(verts.data(), (u32)verts.size(),
                                                 lod.indices.data(), (u32)lod.indices.size());
                }
                if (!lh) {
                    AVER_WARN("[Mesh] '{}' LOD {} refused by the device; ladder truncated at {} level(s)",
                              rel, lvl, ladder.handles.size());
                    ok = false;
                    break;
                }
                ++pass->lodCoarserLevels;
                ladder.handles.push_back(lh);
                ladder.triCounts.push_back(trifactor::levelTriangleCount(md, lvl));
                ladder.errorCm.push_back(trifactor::levelWorldErrorCm(md, lvl));
                trifactor::buildLevelClusterViews(md, lvl, ladder.clusters[lvl]);
            }
            AVER_INFO("[Mesh] '{}' LOD ladder: {} level(s), {} tris at LOD0 -> {} tris at the coarsest",
                      rel, ladder.handles.size(), ladder.triCounts.front(), ladder.triCounts.back());

            // Depth/voxel passes use coarser LOD (threshold: shadow-map texel ~20cm error).
            {
                constexpr f32 kShadowErrorCm = 20.0f;
                u32 pick = 0;
                for (u32 lvl = 1; lvl < ladder.handles.size(); ++lvl)
                    if (ladder.errorCm[lvl] <= kShadowErrorCm) pick = lvl;
                if (pick > 0) {
                    // Key on every level's handle: --lod-select picks different levels per instance.
                    for (u32 lvl = 0; lvl < ladder.handles.size(); ++lvl)
                        if (ladder.triCounts[lvl] > ladder.triCounts[pick])
                            app.depthProxy_[ladder.handles[lvl]] = ladder.handles[pick];
                    AVER_INFO("[Mesh] '{}' depth proxy: LOD {} ({} tris, {:.1f}x less than LOD 0, "
                              "{:.1f}cm error)",
                              rel, pick, ladder.triCounts[pick],
                              static_cast<f64>(ladder.triCounts.front()) /
                                  static_cast<f64>(ladder.triCounts[pick] ? ladder.triCounts[pick] : 1),
                              ladder.errorCm[pick]);
                }
            }
            app.meshLods_[id] = std::move(ladder);

            // Flat cluster data for per-cluster path. verts is copied (LOD0 MeshHandle already created from it).
            MeshClusterData cd;
            cd.verts = verts;
            trifactor::buildMeshClusterViews(md, cd.clusters, cd.clusterIndices);
            if (!cd.clusters.empty()) {
                // Built once from resident data; never recomputed per frame or instance.
                trifactor::buildMeshClusterLevelBounds(cd.clusters, cd.levelBounds);
                for (const trifactor::MeshClusterView& cv : cd.clusters)
                    cd.maxSphereRadius = std::max(cd.maxSphereRadius, cv.sphereRadius);
                app.meshClusterData_[id] = std::move(cd);
            }

            // GPU cluster buffers for --lod-mesh-shader (built only when flag is on).
            if (app.lodMeshShaderEnabled_) {
                std::vector<trifactor::MeshClusterView> gpuBounds;
                std::vector<trifactor::GpuMeshletDesc> gpuDesc;
                std::vector<u32> gpuVerts, gpuTris;
                trifactor::buildMeshClusterGpuData(md, gpuBounds, gpuDesc, gpuVerts, gpuTris);
                if (!gpuBounds.empty()) {
                    if (rhi::IResourceFactory* res = e.device()->resources()) {
                        MeshClusterGpu gpu;
                        gpu.clusterCount = static_cast<u32>(gpuBounds.size());
                        auto upload = [&](const void* data, u64 bytes, const char* name) -> rhi::BufferHandle {
                            rhi::BufferDesc bd; bd.bytes = bytes; bd.kind = rhi::BufferKind::Upload;
                            bd.debugName = name;
                            const rhi::BufferHandle h = res->createBuffer(bd);
                            if (h) res->writeBuffer(h, data, bytes, 0);
                            return h;
                        };
                        gpu.bounds = upload(gpuBounds.data(), gpuBounds.size() * sizeof(trifactor::MeshClusterView), "lod-mesh-shader bounds");
                        gpu.desc   = upload(gpuDesc.data(),   gpuDesc.size()   * sizeof(trifactor::GpuMeshletDesc),  "lod-mesh-shader desc");
                        gpu.verts  = upload(gpuVerts.data(),  gpuVerts.size()  * sizeof(u32),                        "lod-mesh-shader verts");
                        gpu.tris   = upload(gpuTris.data(),   gpuTris.size()   * sizeof(u32),                        "lod-mesh-shader tris");
                        if (gpu.bounds && gpu.desc && gpu.verts && gpu.tris) {
                            rhi::BindingSetDesc bsd;
                            bsd.srvCount = 4;
                            bsd.srvKinds[0] = bsd.srvKinds[1] = bsd.srvKinds[2] = bsd.srvKinds[3] =
                                rhi::SlotKind::StructuredBuffer;
#if AVER_MODULE_VOXI
                            // Stage 3: merge Voxi GI/shadow slots into table 0 (t4..t12, kClusterGiSrvBase).
                            bsd.srvCount = kClusterGiSrvBase + voxi::kGiSrvCount;   // 4 + 9 = 13
                            bsd.srvKinds[kClusterGiSrvBase + 0] = rhi::SlotKind::Texture3D;           // t4 GI volume
                            bsd.srvKinds[kClusterGiSrvBase + 1] = rhi::SlotKind::Texture2D;           // t5 shadow map
                            bsd.srvKinds[kClusterGiSrvBase + 2] = rhi::SlotKind::AccelerationStructure; // t6 TLAS
                            bsd.srvKinds[kClusterGiSrvBase + 3] = rhi::SlotKind::StructuredBuffer;    // t7 RT verts
                            bsd.srvKinds[kClusterGiSrvBase + 4] = rhi::SlotKind::StructuredBuffer;    // t8 RT indices
                            bsd.srvKinds[kClusterGiSrvBase + 5] = rhi::SlotKind::StructuredBuffer;    // t9 RT instances
                            bsd.srvKinds[kClusterGiSrvBase + 6] = rhi::SlotKind::Texture2D;           // t10 RT shadow hist
                            bsd.srvKinds[kClusterGiSrvBase + 7] = rhi::SlotKind::Texture2D;           // t11 RT refl hist
                            bsd.srvKinds[kClusterGiSrvBase + 8] = rhi::SlotKind::Texture2D;           // t12 GI-only shadow
                            bsd.uavCount = voxi::kGiUavCount;
                            bsd.uavKinds[0] = rhi::SlotKind::Texture3D;   // u0 volume mip 0
                            bsd.uavKinds[1] = rhi::SlotKind::Texture3D;   // u1 injection accumulator
                            bsd.uavKinds[2] = rhi::SlotKind::Texture2D;   // u2 RT shadow hist (write)
                            bsd.uavKinds[3] = rhi::SlotKind::Texture2D;   // u3 RT refl hist (write)
#endif
                            gpu.bindingSet = res->createBindingSet(bsd);
                            if (gpu.bindingSet) {
                                res->setSrvBuffer(gpu.bindingSet, 0, gpu.bounds, sizeof(trifactor::MeshClusterView), (u32)gpuBounds.size());
                                res->setSrvBuffer(gpu.bindingSet, 1, gpu.desc,   sizeof(trifactor::GpuMeshletDesc),  (u32)gpuDesc.size());
                                res->setSrvBuffer(gpu.bindingSet, 2, gpu.verts,  sizeof(u32), (u32)gpuVerts.size());
                                res->setSrvBuffer(gpu.bindingSet, 3, gpu.tris,   sizeof(u32), (u32)gpuTris.size());
                                // Voxi's slots (t4/t5..) are NOT populated here: lifetime mismatch (texture can be resized later).
                                app.meshClusterGpu_[id] = gpu;
                            } else {
                                AVER_WARN("[LOD-MESH-SHADER] '{}' binding set failed; this mesh falls back to the CPU per-cluster path", rel);
                            }
                        } else {
                            AVER_WARN("[LOD-MESH-SHADER] '{}' GPU cluster buffer upload failed; this mesh falls back to the CPU per-cluster path", rel);
                        }
                    }
                }
            }
        }
#endif
}
#endif   // AVER_MODULE_SCENE

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
// Creates the AS+MS+PS pipeline for --lod-mesh-shader. Tried once per run; failed compile/tier-0 falls back to CPU path.
void SandboxApp::ensureLodMeshPipeline(Engine& e) {
    // Reopen latch when a new material graph appears: pipeline compiled earlier won't have an arm for it.
#if AVER_MODULE_PBR
    const u64 graphRev = pbr::materialGraphs().revision();
    if (lodMeshPipelineTried_ && lodMeshPipelineGraphRev_ != graphRev) {
        lodMeshPipelineTried_ = false;
        lodMeshPipelineReady_ = false;
    }
    lodMeshPipelineGraphRev_ = graphRev;
#endif
    if (lodMeshPipelineTried_) return;
    // Test flag before setting latch to avoid burning the one attempt while flag is false.
    if (!lodMeshShaderEnabled_) return;
    lodMeshPipelineTried_ = true;

#if !AVER_MODULE_PBR
    // No PBR module: cluster pixel shader can't evaluate a surface.
    AVER_INFO("[LOD-MESH-SHADER] built without the PBR module, so the GPU per-cluster path has "
              "no materials to shade with; using the CPU paths instead");
    return;
#else
    const rhi::DeviceCaps caps = e.device()->caps();
    if (caps.meshShaderTier == 0 || caps.shaderModel < 65 || !caps.dxcAvailable) {
        AVER_WARN("[LOD-MESH-SHADER] this device (meshShaderTier={}, shaderModel={}, dxc={}) "
                  "cannot run the GPU per-cluster path -- house rule 6's degrade: falling back to "
                  "--lod-per-cluster/--lod-select, whichever else is on",
                  caps.meshShaderTier, caps.shaderModel, caps.dxcAvailable);
        return;
    }
    rhi::IResourceFactory* res = e.device()->resources();
    rhi::IRenderContext* ctx = e.device()->renderContext();
    if (!res || !ctx) {
        AVER_WARN("[LOD-MESH-SHADER] no resource factory / render context on this backend; falling back");
        return;
    }

    lodMeshLayout_ = rhi::PipelineLayout{};
    lodMeshLayout_.srvCount = 4;   // table 0, t0..t3: ClusterBounds, ClusterMeshletDesc, verts, tris
    // Table 1 is the material (t4 without Voxi, t13 with it); srvCount1 supplies material textures.
#if AVER_MODULE_VOXI
    // Stage 3 register map: t0..t3 cluster geometry, t4..t12 Voxi GI (reserves t6..t12 for TLAS/RT),
    // t13..t20 material (table 1). Check against giLayout() (VoxiRenderer.cpp) before deploying.
    lodMeshLayout_.srvCount += voxi::kGiSrvCount;   // t4..t12
    lodMeshLayout_.uavCount  = voxi::kGiUavCount;   // u0..u3, reserved; this PS never writes them
#endif
    lodMeshLayout_.srvCount1 = pbr::kMaterialSrvCount;
    lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].filter        = rhi::Filter::Anisotropic;
    lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].address       = rhi::AddressMode::Wrap;
    lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].maxAnisotropy = 8;
    lodMeshLayout_.samplerCount = kClusterMaterialSamplerSlot + 1;
#if AVER_MODULE_VOXI
    // Voxi's two samplers, at kClusterGiSamplerBase (same filter/address/compare as giSamplers()).
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 0].filter  = rhi::Filter::Linear;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 0].address = rhi::AddressMode::Clamp;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].filter  = rhi::Filter::ComparisonLinear;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].address = rhi::AddressMode::Clamp;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].compare = rhi::CompareOp::LessEqual;
    lodMeshLayout_.samplerCount = kClusterGiSamplerBase + 2;
#endif
    // b1 (PerObject): 32-dword root-constants; b4 (kFeatureFrameConstantRegister): AS/MS only.
    lodMeshLayout_.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    lodMeshLayout_.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;
#if AVER_MODULE_VOXI
    // b3 (VoxiFrame): pixel-shader-only root CBV (cannot share b4; see kClusterGiFrameRegister comment).
    lodMeshLayout_.constantDwords[kClusterGiFrameRegister] = 0;
#endif

    // AS/MS from shared prelude (no material state); pixel shader below does.
    const std::string defs = std::string("AVER_MS_CLUSTER=1;") + rhi::meshGeometryDefines(lodMeshLayout_);
    static const std::string kEmptySource;

    rhi::ShaderDesc asd;
    asd.prelude = rhi::sharedShaderPrelude();
    asd.source = kEmptySource.c_str();
    asd.entry = "ASMain";
    asd.stage = rhi::ShaderStage::Amplification;
    asd.minShaderModel = 65;
    asd.defines = defs.c_str();
    lodMeshAsShader_ = res->createShader(asd);

    rhi::ShaderDesc msd = asd;
    msd.entry = "MSClusterMain";
    msd.stage = rhi::ShaderStage::Mesh;
    lodMeshMsShader_ = res->createShader(msd);

    // Pixel shader: shared prelude + material prelude + optional Voxi GI prelude + source.
    const std::string& kMaterialGraphHlsl = pbr::materialGraphs().hlsl();
    std::string kClusterPsPrelude = rhi::sharedShaderPrelude();
    if (!kMaterialGraphHlsl.empty()) kClusterPsPrelude += "\n#define AVER_MATERIAL_GRAPH 1\n";
    kClusterPsPrelude += pbr::materialShaderPrelude();
    kClusterPsPrelude += kMaterialGraphHlsl;
#if AVER_MODULE_VOXI
    kClusterPsPrelude += voxi::giShaderPrelude();
#endif
    // Material textures based at THIS layout's srvCount (t13 with Voxi, t4 without).
    std::string psDefs =
#if AVER_MODULE_VOXI
        pbr::materialShaderDefines(lodMeshLayout_.srvCount, kClusterMaterialSamplerSlot,
                                   voxi::Renderer::get().settings().layeredBsdf != voxi::Quality::Off) +
#else
        pbr::materialShaderDefines(lodMeshLayout_.srvCount, kClusterMaterialSamplerSlot,
                                   /*layeredBsdf=*/false) +
#endif
        ";AVER_CLUSTER_PS_DEBUG=0";
#if AVER_MODULE_VOXI
    // AVER_CLUSTER_VOXI=1 switches to real shadowFactor()/coneTracedIndirect() calls.
    psDefs += ";AVER_CLUSTER_VOXI=1;" +
              voxi::giShaderDefines(kClusterGiSrvBase, kClusterGiSamplerBase, kClusterGiFrameRegister);
#endif
    static const std::string kClusterPsSource = rhi::shaderFile("cluster_material.hlsl");

    rhi::ShaderDesc psd;
    psd.prelude = kClusterPsPrelude.c_str();
    psd.source  = kClusterPsSource.c_str();
    psd.entry = "PSClusterMain";
    psd.stage = rhi::ShaderStage::Pixel;
    psd.defines = psDefs.c_str();
    // SM 6.5 like siblings (no SM6-specific syntax; untested with lower model).
    psd.minShaderModel = 65;
    lodMeshPsShader_ = res->createShader(psd);

    if (!lodMeshAsShader_ || !lodMeshMsShader_ || !lodMeshPsShader_) {
        AVER_WARN("[LOD-MESH-SHADER] AS/MS/PS compile failed; falling back to --lod-per-cluster/--lod-select");
        return;
    }

    rhi::GraphicsPipelineDesc pd;
    pd.as = lodMeshAsShader_;
    pd.ms = lodMeshMsShader_;
    pd.ps = lodMeshPsShader_;
    pd.layout = lodMeshLayout_;
    pd.cull = rhi::CullMode::None;
    pd.depth = {true, true, rhi::CompareOp::Less};
    pd.renderTargetCount = 1;
    pd.renderTargets[0] = e.device()->backbufferFormat();
    pd.depthFormat = e.device()->depthFormat();
    pd.sampleCount = e.device()->sampleCount();
    lodMeshPipeline_ = res->createGraphicsPipeline(pd);
    if (!lodMeshPipeline_) {
        AVER_WARN("[LOD-MESH-SHADER] pipeline creation failed; falling back to --lod-per-cluster/--lod-select");
        return;
    }
    lodMeshPipelineReady_ = true;
    AVER_INFO("[LOD-MESH-SHADER] GPU per-cluster pipeline ready (meshShaderTier={})", caps.meshShaderTier);
#endif   // AVER_MODULE_PBR
}

#endif

// Drops project meshes from id tables. Built-in primitives survive.
void SandboxApp::releaseProjectMeshes(Engine& e) {
    (void)e;
    // Mesh/bounds/slot-0-material/part tables are content_'s; content_.releaseProjectMeshes drops them.
#if AVER_MODULE_SCENE
    for (const u64 id : content_.projectMeshIds()) {
        meshTris_.erase(id);
        pickGeometry_.erase(id);
#if AVER_MODULE_PBR
        // selOutlineLines_ exists only under PBR; line mesh cached here is dropped.
        if (const auto oit = selOutlineLines_.find(id); oit != selOutlineLines_.end()) {
            if (oit->second) e.device()->destroyLineMesh(oit->second);
            selOutlineLines_.erase(oit);
        }
#endif
#if AVER_MODULE_TRIFACTOR
        meshLods_.erase(id);
        meshClusterData_.erase(id);
#endif
    }
#if AVER_MODULE_TRIFACTOR
    // Destroy per-instance CPU-assembled cut handles explicitly (GPU buffers would leak).
    for (auto& [ent, cache] : clusterCutCache_)
        if (cache.handle) e.device()->destroyMesh(cache.handle);
    clusterCutCache_.clear();
#endif
    // destroyBaseHandles=false: editor's mesh reload never destroyed base handles.
    content_.releaseProjectMeshes(*e.device(), /*destroyBaseHandles=*/false);
#endif
}

#if AVER_MODULE_PBR
void SandboxApp::loadProjectMaterials() {
#if AVER_MODULE_SCENE
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    const std::string contentMatDir = dir + "\\Materials";
    const std::string binMatDir = project_.binariesDir() + "\\Materials";

    // Must agree with content_.materialForSurface()'s resolution (MaterialResolve.hpp).
    const std::vector<std::string> stems = editor::projectMaterialStems(project_.binariesDir(), dir);

    u32 loaded = 0;
    for (const std::string& stem : stems) {
        const pbr::MaterialHandle h = content_.materialForSurface(stem);
        if (!h) continue;
        content_.bindSurfaceMaterial(aver_scene_material(0, stem.c_str()), h);
        ++loaded;
    }
    if (loaded)
        AVER_INFO("[Material] {} project material(s) loaded from {} and {}", loaded, binMatDir,
                  contentMatDir);
#endif
}

// Destroys every project material. Texture cache survives.
void SandboxApp::releaseProjectMaterials() {
    // clearGraphRegistry=false: editor never clears pbr::materialGraphs().
    content_.releaseProjectMaterials(/*clearGraphRegistry=*/false);
}

// See header comment (SandboxApp.hpp) for the full A/B contract.
bool SandboxApp::levelScopedMaterialsEnabled() {
    const char* v = std::getenv("AVER_LEVEL_SCOPED_MATERIALS");
    return !v || v[0] != '0';
}

#endif

// Creates actor's material and pins metallic/roughness to identity 1.
void SandboxApp::makeMaterialFor(MeshObj& o) {
#if AVER_MODULE_PBR
    pbr::MaterialDesc d;
    d.name            = o.name;
    d.metallicFactor  = o.metallic;
    d.roughnessFactor = o.roughness;
    // --coat only; zero otherwise leaves MaterialFlag_Coat clear in packMaterial.
    d.coatWeight      = coatWeight_;
    d.coatRoughness   = coatRough_;
    d.coatF0          = coatF0_;
    o.material = pbr::MaterialLibrary::get().create(d);
    if (!o.material) { AVER_WARN("[Sandbox] no material for '{}'; it will draw with the fallback", o.name); return; }
    o.metallic = o.roughness = 1.0f;
#else
    (void)o;
#endif
}

// Once per name: warn when a material handle no longer resolves in pbr::MaterialLibrary.
void SandboxApp::warnDeadMaterialHandle(i32 mat) {
    static std::unordered_set<i32> s_warnedDeadHandle;
    if (!s_warnedDeadHandle.insert(mat).second) return;
    AVER_WARN("[Editor] surface '{}' holds a material handle that no longer resolves in "
              "pbr::MaterialLibrary; drawing its named look instead of the white/metal=1 "
              "placeholder. A stale handle here would otherwise render as a bright mirror.",
              editor::surfaceDisplayName(mat));
}

} // namespace aver
