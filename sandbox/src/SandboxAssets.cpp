// Runtime side: the content index and asset resolvers, and mesh / material / particle loading.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_PBR
// Returns where an asset reference points on this machine, or empty when it cannot be resolved.
std::string SandboxApp::resolveAssetPath(const pbr::TextureRef& ref) const {
    if (!ref.path.empty()) {
        const std::string& p = ref.path;
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
        if (absolute) return p;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            const std::string full = content + "\\" + p;
            std::error_code ec;
            if (std::filesystem::exists(full, ec)) return full;
        }
        return p;
    }
    if (ref.id) {
        const auto it = contentIndex_.find(ref.id);
        if (it != contentIndex_.end()) return it->second;
    }
    return {};
}

#endif

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// PIPELINE-BUILD TIME half: giShaderPrelude()/giShaderDefines() are pure functions of the register
// numbers ParticleRenderer::buildPipelines hands in, so this never dereferences `user` -- it exists
// only to keep the signature uniform with particleGiBind below, which does.
 bool SandboxApp::particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                              std::string* outPrelude, std::string* outDefines, void* user) {
    (void)user;
    if (!outPrelude || !outDefines) return false;
    *outPrelude = voxi::giShaderPrelude();
    *outDefines = voxi::giShaderDefines(srvBase, samplerBase, cbRegister);
    return true;
}

// PER-FRAME half. Forwards straight to voxiRenderer_'s own bindGiResources/giFrameConstants --
// see those methods' own comments for why a null volume/shadow texture or a not-yet-ready Voxi
// degrades safely rather than needing a readiness check here.
 void SandboxApp::particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                           const void** outCbData, u32* outCbBytes, void* user) {
    auto* self = static_cast<SandboxApp*>(user);
    if (!self || !outCbData || !outCbBytes) return;
    self->voxiRenderer_.bindGiResources(res, set, srvBase);
    *outCbData = self->voxiRenderer_.giFrameConstants();
    *outCbBytes = self->voxiRenderer_.giFrameConstantBytes();
}

#endif

// Indexes every asset under the project's content root by fnv1a64 of its content-relative path.
void SandboxApp::rebuildContentIndex() {
    contentIndex_.clear();
    const std::string content = project_.contentDir();
    if (content.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(content, ec)) return;
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(it->path(), content, ec).string();
        if (ec || rel.empty()) continue;
        // FROZEN: the id hashes the forward-slash spelling, matching C# Assets.ObjectIdOf.
        for (char& c : rel) if (c == '\\') c = '/';
        contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
    }
    AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);
    // The anim system does its own file discovery through this, and caches by id -- so a
    // re-index has to drop what it cached or a moved asset keeps resolving to the old path.
    // Aver.Anim.Scene is built only under AVER_MODULE_SCENE, so these two calls need their own
    // guard even though the rest of this function has nothing to do with a material or a scene.
#if AVER_MODULE_SCENE
    anim::animSystem().clear();
    anim::animSystem().setResolver(&SandboxApp::resolveAnimAsset, this);
#endif
}

// Maps an asset ObjectId to a path for aver::anim::AnimSystem. A plain function pointer because
// that is what the system takes: asset discovery is the host's business, not the sampler's.
 std::string SandboxApp::resolveAnimAsset(u64 id, void* user) {
    auto* self = static_cast<SandboxApp*>(user);
    if (!self) return {};
    const auto it = self->contentIndex_.find(id);
    return it == self->contentIndex_.end() ? std::string() : it->second;
}

#if AVER_MODULE_SCENE
 rhi::MeshHandle SandboxApp::resolveSceneMesh(u64 id, void* user) {
    auto* self = static_cast<SandboxApp*>(user);
    if (!self) return 0;
    const auto it = self->sceneMeshes_.find(id);
    return it == self->sceneMeshes_.end() ? 0 : it->second;
}

#endif

#if AVER_MODULE_PBR
// Turns an .ocmat's GRAPHREF path into the gMaterialGraphId its constants carry. 0 for a
// material with no GRAPHREF, and 0 for one whose graph will not load or compile.
// A broken graph does not take the material down with it: returning 0 falls back to the stock
// .ocmat factors/maps instead of vanishing the object entirely. Logged either way.
u32 SandboxApp::resolveMaterialGraph(const std::string& graphRef) {
    if (graphRef.empty()) return 0;
    const std::string content = project_.contentDir();
    if (content.empty()) return 0;

    // CONTENT-RELATIVE, the same convention COMP mesh= uses in .ocgraph and TEX uses in this
    // very file: a path with the content directory on the front resolves to nothing, silently,
    // which is a mistake worth not repeating here.
    std::string path = content + "\\" + graphRef;
    for (char& c : path) if (c == '/') c = '\\';

    // ALREADY COMPILED? Two materials naming one graph is ordinary -- a stone and a wet stone
    // sharing a pattern -- and asking the registry first means the graph is read and compiled
    // once, and both materials get the same id rather than two arms doing the same arithmetic.
    if (const u32 known = pbr::materialGraphs().idOf(path)) return known;

    fmt::OcGraphData g;
    std::string err;
    if (!fmt::loadOcgraph(path, g, &err)) {
        AVER_ERROR("[MaterialGraph] '{}' could not be read, so the material shades as a stock "
                   "one: {}", path, err);
        return 0;
    }
    return pbr::materialGraphs().add(path, g.name, g);
}

// Returns the material a surface token names, loading it on first use. 0 when the project has none.
pbr::MaterialHandle SandboxApp::materialForSurface(const std::string& name) {
    if (name.empty()) return 0;
    const auto cached = materialAssets_.find(name);
    if (cached != materialAssets_.end()) return cached->second;

    pbr::MaterialHandle h = 0;
    const std::string content = project_.contentDir();
    if (!content.empty()) {
        // Built .ocmat under Binaries wins over a hand-authored one under Content -- see
        // MaterialResolve.hpp for the full three-candidate order, shared with
        // loadProjectMaterials() below so the two never drift apart.
        const std::string path = editor::resolveMaterialPath(project_.binariesDir(), content, name);
        if (!path.empty()) {
            pbr::MaterialDesc d;
            fmt::OcMatExtras extras;
            std::string err;
            if (!fmt::loadOcmat(path, d, &extras, &err)) {
                AVER_WARN("[Material] {}", err);
            } else {
                d.graphId = resolveMaterialGraph(extras.graphRef);
                h = pbr::MaterialLibrary::get().create(d);
                if (h) AVER_INFO("[Material] '{}' loaded from {}{}", d.name, path,
                                 d.graphId ? " (graph " + std::to_string(d.graphId) + ")" : "");
            }
        }
    }
    materialAssets_.emplace(name, h);
    return h;
}

#endif

// Loads every .ocmesh under the project's Content, keyed by fnv1a64 of its forward-slash relative path.
void SandboxApp::loadProjectMeshes(Engine& e) {
#if AVER_MODULE_SCENE
    // W4 (--mesh-heap): the FIRST statement, unconditionally, so every static mesh this call
    // uploads -- built-ins are created earlier, at :2058-2091, and predate this call entirely, so
    // they stay on the Upload heap regardless of this flag -- lands on whichever heap the flag
    // asked for. setStaticMeshHeapDefault only affects createMesh calls made AFTER it, so this
    // has to run before the very first one below, not after an early return might skip it.
    if (e.device()) e.device()->setStaticMeshHeapDefault(meshHeapDefault_);
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
#if AVER_MODULE_TRIFACTOR
    // W11 (--lod-share-vertices): summed across every mesh's LOD ladder built below, printed once
    // as the [Mesh] LOD ladders line at the end of this function -- see that line's own comment.
    u32 lodCoarserLevels = 0;
    u32 lodSharedLevels = 0;
    u64 lodSharedVertexBytesSaved = 0;
#endif
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Mesh) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        fmt::OcMeshData md;
        std::string why;
        if (!fmt::loadOcMesh(full, md, &why)) { AVER_WARN("[Mesh] {}", why); ++failed; continue; }

        std::vector<rhi::MeshVertex> verts(md.vertexCount());
        for (u32 i = 0; i < md.vertexCount(); ++i) {
            rhi::MeshVertex& v = verts[i];
            v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
            v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
            v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
        }
        const rhi::MeshHandle h = e.device()->createMesh(verts.data(), (u32)verts.size(),
                                                        md.indices.data(), (u32)md.indices.size());
        if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); ++failed; continue; }

        const u64 id = fnv1a64(std::string_view(rel));
        sceneMeshes_[id] = h;
        // A project mesh can take a built-in's id (its own Meshes/cube.ocmesh, say), and the
        // built-in's pick triangles, seeded at creation, would then answer for this mesh. Dropped
        // here so pickGeometryFor reads this file instead.
        pickGeometry_.erase(id);
        // The path back from an id: every other map here goes id -> data, so anything wanting to
        // NAME a loaded mesh (the foliage palette, a future asset picker) had no way to. Populated
        // here because this is the one place with both halves at once.
        meshPathById_[id] = rel;
        meshBounds_[id] = {md.boundsMin, md.boundsMax};
        meshTris_[id] = static_cast<u32>(md.indices.size() / 3);
        // SLOT 0 FOR EVERY MESH, split or not. buildMeshParts resolves the slots of a mesh
        // it actually splits; this covers the one it returns early on, which is the common case
        // and was the case nothing read. Recorded even for a multi-slot mesh so the entity's own
        // fallback is its first slot rather than nothing when a split was refused.
        if (!md.materialSlots.empty() && !md.materialSlots[0].empty())
            meshSlot0Material_[id] = aver_scene_material(0, md.materialSlots[0].c_str());
        buildMeshParts(e, id, md, verts, rel);
        if (md.hasSkin()) skinnedMeshIds_.insert(id);
        projectMeshIds_.push_back(id);
        ++loaded;
        AVER_INFO("[Mesh] '{}' -> {} verts, {} indices, lodCount={}, coarserLods={}, meshlets={}",
                  rel, verts.size(), md.indices.size(), md.lodCount(), md.coarserLods.size(),
                  md.meshlets.size());

#if AVER_MODULE_TRIFACTOR
        // The Cook wrote coarser LOD levels for this mesh: build one whole-level MeshHandle per
        // level, ONCE, here at load time -- never per frame. Every level shares `verts` (the SAME
        // vertex buffer), so this only duplicates INDEX data on the GPU, never vertices, sidestepping
        // the per-frame-index-upload failure mode entirely: selection only CHOOSES among resident handles.
        if (md.lodCount() > 1) {
            MeshLodLadder ladder;
            const u32 levels = md.lodCount();
            ladder.handles.reserve(levels);
            ladder.triCounts.reserve(levels);
            ladder.errorCm.reserve(levels);
            ladder.clusters.resize(levels);

            ladder.handles.push_back(h);
            ladder.triCounts.push_back(meshTris_[id]);
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
                rhi::MeshHandle lh = lodShareVertices_
                    ? e.device()->createMeshSharingVertices(h, lod.indices.data(), (u32)lod.indices.size())
                    : 0;
                if (lh) {
                    ++lodSharedLevels;
                    lodSharedVertexBytesSaved += static_cast<u64>(verts.size()) * sizeof(rhi::MeshVertex);
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
                ++lodCoarserLevels;
                ladder.handles.push_back(lh);
                ladder.triCounts.push_back(trifactor::levelTriangleCount(md, lvl));
                ladder.errorCm.push_back(trifactor::levelWorldErrorCm(md, lvl));
                trifactor::buildLevelClusterViews(md, lvl, ladder.clusters[lvl]);
            }
            AVER_INFO("[Mesh] '{}' LOD ladder: {} level(s), {} tris at LOD0 -> {} tris at the coarsest",
                      rel, ladder.handles.size(), ladder.triCounts.front(), ladder.triCounts.back());

            // The handle the shadow/voxel passes draw instead: those passes are depth-only, so a
            // coarser level's dropped detail never shows. The lit pass is unaffected.
            // Chosen on world error, not a triangle ratio: a ratio picked badly at both ends
            // (fir_sapling at 393k triangles, 1.1x off LOD0, near-zero saving, while reducing a
            // 116-triangle moss). Threshold is a shadow-map texel (a cascade covers its slice with
            // 2048 texels), below which error can't change the shadow.
            {
                constexpr f32 kShadowErrorCm = 20.0f;
                u32 pick = 0;
                for (u32 lvl = 1; lvl < ladder.handles.size(); ++lvl)
                    if (ladder.errorCm[lvl] <= kShadowErrorCm) pick = lvl;
                if (pick > 0) {
                    // Keyed on every level's handle, not just LOD 0's: when --lod-select is on the
                    // lit pass submits whichever level it chose, and that handle must resolve too
                    // or the proxy silently stops applying to exactly the instances furthest away.
                    for (u32 lvl = 0; lvl < ladder.handles.size(); ++lvl)
                        if (ladder.triCounts[lvl] > ladder.triCounts[pick])
                            depthProxy_[ladder.handles[lvl]] = ladder.handles[pick];
                    AVER_INFO("[Mesh] '{}' depth proxy: LOD {} ({} tris, {:.1f}x less than LOD 0, "
                              "{:.1f}cm error)",
                              rel, pick, ladder.triCounts[pick],
                              static_cast<f64>(ladder.triCounts.front()) /
                                  static_cast<f64>(ladder.triCounts[pick] ? ladder.triCounts[pick] : 1),
                              ladder.errorCm[pick]);
                }
            }
            meshLods_[id] = std::move(ladder);

            // Flat, all-levels-at-once cluster data for the per-cluster path. `verts` is copied
            // (not moved) since the LOD-0 MeshHandle `h` was already created from it -- this copy
            // is what a cache rebuild re-uploads later.
            MeshClusterData cd;
            cd.verts = verts;
            trifactor::buildMeshClusterViews(md, cd.clusters, cd.clusterIndices);
            if (!cd.clusters.empty()) {
                // Once per mesh, from data already resident -- see MeshClusterData::levelBounds'
                // own comment. Never recomputed per frame or per instance.
                trifactor::buildMeshClusterLevelBounds(cd.clusters, cd.levelBounds);
                for (const trifactor::MeshClusterView& cv : cd.clusters)
                    cd.maxSphereRadius = std::max(cd.maxSphereRadius, cv.sphereRadius);
                meshClusterData_[id] = std::move(cd);
            }

            // GPU cluster buffers for --lod-mesh-shader, built only when the flag is on, so a run
            // that never asks for it never pays for the extra upload. Uses buildMeshClusterGpuData,
            // which keeps each meshlet's local vertex/triangle block intact -- what the
            // amplification+mesh shader pair reads -- instead of pre-expanding to global indices.
            if (lodMeshShaderEnabled_) {
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
                            // STAGE 3: this SAME set is table 0, so it also carries Voxi's merged
                            // GI/shadow slots at kClusterGiSrvBase.. (see ensureLodMeshPipeline's
                            // register-map comment). Declared with EXACTLY giLayout()'s kinds, just
                            // based four registers higher; the content is written later by the
                            // per-frame sync, not at mesh-load time, so a live GI/shadow change stays current.
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
                                // Voxi's slots (t4/t5, kClusterGiSrvBase..) are NOT populated here
                                // -- binding them once at upload time is the wrong lifetime for a
                                // texture that can be resized or recreated at any later frame.
                                meshClusterGpu_[id] = gpu;
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
    if (loaded || failed)
        AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
#if AVER_MODULE_TRIFACTOR
    // W11: once per loadProjectMeshes, not per mesh -- the existing lodCount= line on each mesh's
    // own [Mesh] '...' -> ... line above (unchanged, byte-identical) is what a per-mesh check
    // sums; this is the run-wide total the flag's own trade (vertex-buffer sharing) is measured
    // against. Printed even when both counts are 0 (--lod-share-vertices off, or no mesh in this
    // project has a coarser LOD at all), so its absence in a log is never ambiguous with "the
    // line was never reached".
    if (lodCoarserLevels || lodSharedLevels)
        AVER_INFO("[Mesh] LOD ladders: {} coarser level(s), {} sharing their LOD0 vertex buffer "
                  "({:.1f} MiB of vertex data not duplicated)",
                  lodCoarserLevels, lodSharedLevels,
                  static_cast<f64>(lodSharedVertexBytesSaved) / (1024.0 * 1024.0));
#endif
#if AVER_MODULE_LANDSCAPE
        // Rebuilt here rather than lazily on entering Foliage mode: this is the moment the set of
        // placeable meshes actually changes, and editorModeAvailable() asks whether the palette
        // is empty every frame the mode dropdown is open.
        refreshFoliagePalette();
#endif
#else
    (void)e;
#endif
}

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
// Creates the AS+MS+PS pipeline --lod-mesh-shader draws through. Tried EXACTLY ONCE per run
// (lodMeshPipelineTried_ latches immediately): a failed compile or tier-0 device means the CPU
// per-cluster path runs instead, logged once, never retried.
void SandboxApp::ensureLodMeshPipeline(Engine& e) {
    // A material graph appearing since this pipeline was built reopens the latch: PSClusterMain
    // calls averEvalMaterial like PSMainVoxi does, so a pipeline compiled earlier has a switch
    // with no arm for it -- a cluster-drawn mesh would silently shade stock while the same
    // material drawn the other way shows its graph. Reopening rather than a second entry point:
    // everything below already destroys/rebuilds what it finds.
    // Guarded: this function compiles under AVER_MODULE_PBR=OFF too, so naming pbr:: unguarded
    // broke that build outright.
#if AVER_MODULE_PBR
    const u64 graphRev = pbr::materialGraphs().revision();
    if (lodMeshPipelineTried_ && lodMeshPipelineGraphRev_ != graphRev) {
        lodMeshPipelineTried_ = false;
        lodMeshPipelineReady_ = false;
    }
    lodMeshPipelineGraphRev_ = graphRev;
#endif
    if (lodMeshPipelineTried_) return;
    // THE FLAG IS TESTED BEFORE THE LATCH IS SET, the opposite of what it used to do: latching
    // first meant a single call made while the flag was still false burned the one attempt this
    // function will ever make, with "we already tried" answering every later call. The ordering
    // fix that now sets the flag early makes a future reordering merely late instead of fatal.
    if (!lodMeshShaderEnabled_) return;
    lodMeshPipelineTried_ = true;

#if !AVER_MODULE_PBR
    // NO MATERIALS, NO CLUSTER PATH: its pixel shader evaluates a pbr:: surface, and a build
    // without the material system has nothing for it to evaluate. Declining here is the honest
    // degrade: shading from the per-object colour instead is exactly the black-foliage bug this
    // change exists to fix, so it would reintroduce it on exactly the configurations nobody looks at.
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
    // TABLE 1 IS THE MATERIAL, and adding it stops this path drawing black: the pixel shader now
    // evaluates a real surface, so srvCount1 (the RHI's second table) needs material textures
    // bound (t4 without Voxi, t13 with it).
    // NO THIRD TABLE NEEDED -- STAGE 3'S WHOLE POINT: rhi::kBindingTableCount is 2 and STAYS 2
    // (D3D12Device.cpp's nullFill: a third table would also cost Vulkan a descriptor set it isn't
    // guaranteed to have). Voxi's resources MERGE into table 0 alongside the cluster buffers;
    // table 1 stays the material's alone.
#if AVER_MODULE_VOXI
    // STAGE 3'S REGISTER MAP, established BEFORE any of it is code (an off-by-one here is a silent
    // mis-sample, not a compile error) -- checked against giLayout() (VoxiRenderer.cpp) and
    // VoxiGiShaders.hpp before anything below was written:
    //   t0..t3   cluster geometry (above, unchanged) -- ClusterBounds/ClusterMeshletDesc/verts/tris
    //   t4       Voxi's GI volume  (kClusterGiSrvBase+0, Texture3D)  -- giShaderPrelude() reads it
    //   t5       Voxi's shadow map (kClusterGiSrvBase+1, Texture2D)  -- giShaderPrelude() reads it
    //   t6..t12  Voxi's TLAS / RT geometry table / RT history / GI-only shadow map -- RESERVED so
    //            this table-0 union has EXACTLY giLayout()'s shape (kGiSrvCount = 9), but never
    //            declared by giShaderPrelude(): this pipeline's pixel shader runs Voxi's non-ray-
    //            traced fallback only and reads none of them. See VoxiGiShaders.hpp's own comment
    //            on why over-provisioning here is deliberate, not an oversight.
    //   t13..t20 material (table 1, srvCount1 = pbr::kMaterialSrvCount, now based at t13)
    //   u0..u3   Voxi's volume-mip / accumulator / RT-history UAVs -- RESERVED, same reason as
    //            t6..t12: this pixel shader never writes any of them.
    //   s0       material sampler (kClusterMaterialSamplerSlot, unchanged)
    //   s1       Voxi's volume sampler (kClusterGiSamplerBase+0, linear-clamp)
    //   s2       Voxi's shadow sampler (kClusterGiSamplerBase+1, comparison-linear-clamp)
    //   b1       PerObject (unchanged)
    //   b3       VoxiFrame (kClusterGiFrameRegister) -- read by the pixel shader only
    //   b4       ClusterFrameCB (kFeatureFrameConstantRegister, unchanged) -- read by AS/MS only;
    //            see kClusterGiFrameRegister's own comment on why VoxiFrame could not share it
    lodMeshLayout_.srvCount += voxi::kGiSrvCount;   // t4..t12
    lodMeshLayout_.uavCount  = voxi::kGiUavCount;   // u0..u3, reserved; this PS never writes them
#endif
    lodMeshLayout_.srvCount1 = pbr::kMaterialSrvCount;
    lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].filter        = rhi::Filter::Anisotropic;
    lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].address       = rhi::AddressMode::Wrap;
    lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].maxAnisotropy = 8;
    lodMeshLayout_.samplerCount = kClusterMaterialSamplerSlot + 1;
#if AVER_MODULE_VOXI
    // Voxi's own two samplers, at kClusterGiSamplerBase -- the SAME filter/address/compare
    // values giSamplers() (VoxiRenderer.cpp) gives its own s0/s1, just moved up because s0 here
    // is already the material's.
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 0].filter  = rhi::Filter::Linear;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 0].address = rhi::AddressMode::Clamp;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].filter  = rhi::Filter::ComparisonLinear;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].address = rhi::AddressMode::Clamp;
    lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].compare = rhi::CompareOp::LessEqual;
    lodMeshLayout_.samplerCount = kClusterGiSamplerBase + 2;   // 3: material(0), volume(1), shadow(2)
#endif
    // b1 (PerObject): the SAME 32-dword root-constants shape drawMesh() itself uses, so the
    // cluster shaders read real gWorld/gBaseColor/gMaterial. b4 (kFeatureFrameConstantRegister): a
    // root CBV, read by AS/MS (ClusterFrameCB) only; the pixel shader below never touches it.
    lodMeshLayout_.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    lodMeshLayout_.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;
#if AVER_MODULE_VOXI
    // b3: VoxiFrame, a root CBV the pixel shader alone reads -- see kClusterGiFrameRegister's
    // own comment on why this cannot share ClusterFrameCB's b4.
    lodMeshLayout_.constantDwords[kClusterGiFrameRegister] = 0;
#endif

    // AS and MS come from the shared prelude alone: they touch no material state. The PIXEL
    // shader does not -- see below.
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

    // THE PIXEL SHADER IS COMPOSED DIFFERENTLY FROM ITS AS/MS SIBLINGS: shared prelude + MATERIAL
    // prelude (+ Voxi's GI/shadow prelude when AVER_MODULE_VOXI is compiled in) + its own source.
    // Its predecessor lived inside the shared prelude and could not call averEvalMaterial at all.
    // AVER_MS_CLUSTER is deliberately NOT defined for it: needs only VSOut, which is unguarded.
    // NOT `static`: a static would be computed once and then wrong the moment a project's material
    // graphs registered, and this function re-enters exactly then (see the latch above).
    // AVER_MATERIAL_GRAPH goes in the TEXT, between the two preludes: as a -D it would have to
    // reach every shader compiled against this text, and the one missed would carry both the
    // stock and generated averEvalMaterial.
    const std::string& kMaterialGraphHlsl = pbr::materialGraphs().hlsl();
    std::string kClusterPsPrelude = rhi::sharedShaderPrelude();
    if (!kMaterialGraphHlsl.empty()) kClusterPsPrelude += "\n#define AVER_MATERIAL_GRAPH 1\n";
    kClusterPsPrelude += pbr::materialShaderPrelude();
    kClusterPsPrelude += kMaterialGraphHlsl;
#if AVER_MODULE_VOXI
    kClusterPsPrelude += voxi::giShaderPrelude();
#endif
    // Based at THIS layout's own srvCount, so material textures land in table 1 wherever Stage
    // 3's merge put it (t13 with Voxi, t4 without). AVER_CLUSTER_PS_DEBUG=0 is the real shader;
    // 1..7 isolate one input each when this path renders wrong (see ClusterMaterialShader.hpp).
    std::string psDefs =
        // The cluster path assembles its OWN defines string, separately from VoxiRenderer's --
        // why materialShaderDefines takes this as a required argument. Read from live settings
        // rather than latched: this string is rebuilt per compile, and Voxi's own latch decides shader contents.
        pbr::materialShaderDefines(lodMeshLayout_.srvCount, kClusterMaterialSamplerSlot,
                                   voxi::Renderer::get().settings().layeredBsdf != voxi::Quality::Off) +
        ";AVER_CLUSTER_PS_DEBUG=0";
#if AVER_MODULE_VOXI
    // AVER_CLUSTER_VOXI=1 is what switches PSClusterMain from the neutral sun.visibility=1.0 /
    // zero-indirect stand-in Stage 2 left it with to the real shadowFactor()/coneTracedIndirect()
    // calls -- see ClusterMaterialShader.hpp's own #if AVER_CLUSTER_VOXI ladder.
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
    // SM 6.5 like its siblings: it has no SM6-only syntax of its own any more (that was from
    // sharing a source string with ASMain's DispatchMesh), but a pixel shader paired with a LOWER
    // model AS/MS pipeline has never been tried here, and a release is not the place to find out.
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
#endif   // AVER_MODULE_PBR: the no-materials build returned above
}

#endif

// Drops the project's meshes from the id table. The built-in primitives survive.
void SandboxApp::releaseProjectMeshes(Engine& e) {
    (void)e;   // only read under AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR, below
    // sceneMeshes_ is scene-only; with the module off loadProjectMeshes() never populated it (see
    // its own #if AVER_MODULE_SCENE above), so there is nothing here to erase from it either.
#if AVER_MODULE_SCENE
    for (const u64 id : projectMeshIds_) {
        // The per-material split parts own REAL GPU meshes of their own -- destroyed explicitly
        // for clusterCutCache_'s reason immediately below, and not merely erased from the map.
        if (const auto pit = meshParts_.find(id); pit != meshParts_.end()) {
            for (const MeshPart& p : pit->second)
                if (p.mesh) e.device()->destroyMesh(p.mesh);
            meshParts_.erase(pit);
        }
        sceneMeshes_.erase(id); meshTris_.erase(id); meshSlot0Material_.erase(id);
        pickGeometry_.erase(id);   // whatever pickGeometryFor cached (loaded or empty) for this id
        if (const auto oit = selOutlineLines_.find(id); oit != selOutlineLines_.end()) {
            if (oit->second) e.device()->destroyLineMesh(oit->second);
            selOutlineLines_.erase(oit);
        }
#if AVER_MODULE_TRIFACTOR
        meshLods_.erase(id);
        meshClusterData_.erase(id);
#endif
    }
#if AVER_MODULE_TRIFACTOR
    // Every per-instance CPU-assembled cut handle is about to be invalid (its source mesh data
    // is gone) -- destroy each one explicitly rather than leaking GPU index/vertex buffers.
    for (auto& [ent, cache] : clusterCutCache_)
        if (cache.handle) e.device()->destroyMesh(cache.handle);
    clusterCutCache_.clear();
#endif
#endif
    projectMeshIds_.clear();
}

#if AVER_MODULE_PBR
void SandboxApp::loadProjectMaterials() {
#if AVER_MODULE_SCENE
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    const std::string contentMatDir = dir + "\\Materials";
    const std::string binMatDir = project_.binariesDir() + "\\Materials";

    // MaterialResolve.hpp, so this enumeration and materialForSurface()'s own resolution can
    // never name the two directories differently or disagree on what ".ocmat" means.
    const std::vector<std::string> stems = editor::projectMaterialStems(project_.binariesDir(), dir);

    u32 loaded = 0;
    for (const std::string& stem : stems) {
        const pbr::MaterialHandle h = materialForSurface(stem);
        if (!h) continue;
        surfaceMaterials_[aver_scene_material(0, stem.c_str())] = h;
        ++loaded;
    }
    if (loaded)
        AVER_INFO("[Material] {} project material(s) loaded from {} and {}", loaded, binMatDir,
                  contentMatDir);
#endif
}

// Destroys every material the project owns. The texture cache behind them survives.
void SandboxApp::releaseProjectMaterials() {
    for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
    materialAssets_.clear();
    surfaceMaterials_.clear();
}

#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
void SandboxApp::loadProjectParticleEffects() {
    particles::particleEffects().clear();
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Particle) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        particles::ParticleEffect fx;
        std::string err;
        if (!fmt::loadOcparticle(full, fx, nullptr, &err)) {
            AVER_WARN("[Particles] {}", err);
            ++failed;
            continue;
        }
        particles::particleEffects().set(fnv1a64(std::string_view(rel)), fx);
        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Particles] {} project effect(s) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}

#endif

// Creates one actor's material and pins the actor's own metallic/roughness to the identity 1.
void SandboxApp::makeMaterialFor(MeshObj& o) {
#if AVER_MODULE_PBR
    pbr::MaterialDesc d;
    d.name            = o.name;
    d.metallicFactor  = o.metallic;
    d.roughnessFactor = o.roughness;
    // --coat only. Zero otherwise, which leaves MaterialFlag_Coat clear in packMaterial and the
    // coat term unreachable even in a build that compiled it -- so this line cannot change a
    // default run.
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

// ONCE PER NAME, beside the missing-material warning this mirrors (see the "SAY SO, ONCE PER NAME"
// block in the visible-draw path). A surface whose pbr::MaterialLibrary handle no longer resolves
// used to be drawn with the multiplicative identity baked in as its FINAL appearance -- white,
// metallic 1, roughness 1 -- i.e. a bright mirror, which reads as confident lighting rather than as
// missing content and so is among the worst appearances a content error can take. It now falls
// through to the named look or the flat fallback instead, and says why.
//
// NOT KNOWN TO FIRE. An investigation of a white-under-motion artefact could not trigger this path
// and found a different cause, so this is a latent hazard closed on inspection rather than a
// reproduced bug -- if this line ever appears in a log, that is new information worth chasing.
void SandboxApp::warnDeadMaterialHandle(i32 mat) {
    static std::unordered_set<i32> s_warnedDeadHandle;
    if (!s_warnedDeadHandle.insert(mat).second) return;
    AVER_WARN("[Editor] surface '{}' holds a material handle that no longer resolves in "
              "pbr::MaterialLibrary; drawing its named look instead of the white/metal=1 "
              "placeholder. A stale handle here would otherwise render as a bright mirror.",
              aver_scene_material_name(mat));
}

#if AVER_MODULE_LANDSCAPE
// THE MESH'S OWN MATERIAL, for an entity that never named one.
//
// WHY THIS EXISTS. .ocmesh carries a materialSlots table and every importer writes it, but until
// now the ONLY code that read it was buildMeshParts -- whose first line is
// `if (md.submeshes.size() <= 1) return;`. So a multi-material mesh got its slots resolved and a
// SINGLE-material one got nothing: CMeshRenderer.material stayed 0, surfaceMaterials_ found
// nothing to map, and the entity drew with the flat grey fallback. Importing 53 foliage meshes
// made that obvious -- 37 of them name exactly one material, which is every plant.
//
// 0 MEANS "ASK THE MESH", NOT "NO MATERIAL". That is the same rule drawMeshParts already applies
// one level down (`p.material ? p.material : entityMat`), and it is why this is a fallback rather
// than something written into CMeshRenderer at placement: the mesh already declares its material,
// and copying that name into every placement would be a second copy free to drift from it. A
// non-zero CMeshRenderer.material stays exactly what it has always been -- an override.
i32 SandboxApp::meshDefaultMaterial(u64 meshId) const {
    const auto it = meshSlot0Material_.find(meshId);
    return it == meshSlot0Material_.end() ? 0 : it->second;
}

// Splits a mesh that names more than one material into one MeshHandle per slot.
//
// COMPACTED PER PART, not sharing the parent's vertex array. createMesh COPIES what it is given,
// so handing all seven parts of a palm the whole vertex buffer would upload that buffer seven
// times. The remap also gives each part honest bounds, which is what the culler and the GI
// volume want anyway.
void SandboxApp::buildMeshParts(Engine& e, u64 id, const fmt::OcMeshData& md,
                    const std::vector<rhi::MeshVertex>& verts, const std::string& rel) {
    if (md.submeshes.size() <= 1) return;   // the common case: nothing to split

    std::vector<MeshPart> parts;
    parts.reserve(md.submeshes.size());
    std::unordered_map<u32, u32> remap;
    std::vector<rhi::MeshVertex> pv;
    std::vector<u32> pi;

    for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
        if (sm.indexCount == 0) continue;
        const usize end = usize(sm.indexStart) + sm.indexCount;
        if (end > md.indices.size()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' runs past the index buffer; skipped", rel, sm.name);
            continue;
        }
        remap.clear(); pv.clear(); pi.clear();
        pi.reserve(sm.indexCount);
        bool bad = false;
        for (usize k = sm.indexStart; k < end; ++k) {
            const u32 vi = md.indices[k];
            if (vi >= verts.size()) { bad = true; break; }
            const auto [it2, inserted] = remap.try_emplace(vi, static_cast<u32>(pv.size()));
            if (inserted) pv.push_back(verts[vi]);
            pi.push_back(it2->second);
        }
        if (bad || pv.empty()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' indexes a vertex it does not have; skipped", rel, sm.name);
            continue;
        }

        MeshPart part;
        part.mesh = e.device()->createMesh(pv.data(), static_cast<u32>(pv.size()),
                                           pi.data(), static_cast<u32>(pi.size()));
        if (!part.mesh) {
            AVER_WARN("[Mesh] the device refused submesh '{}' of '{}'", sm.name, rel);
            continue;
        }
        // THE SLOT NAMES THE MATERIAL, which is the whole point of the format's slot table --
        // and the cook writes those names as the .ocmat stems it produced, so a name resolves
        // through exactly the path an authored material does.
        if (sm.materialSlot < md.materialSlots.size()) {
            const std::string& slot = md.materialSlots[sm.materialSlot];
            if (!slot.empty()) part.material = aver_scene_material(0, slot.c_str());
        }
        parts.push_back(part);
    }

    // ONE SURVIVING PART IS NOT A SPLIT. Falling through to the ordinary single-mesh path costs
    // a draw call less and keeps the entity's own material override meaningful.
    if (parts.size() <= 1) {
        for (const MeshPart& p : parts) e.device()->destroyMesh(p.mesh);
        return;
    }
    AVER_INFO("[Mesh] '{}' names {} materials; split into {} part(s) so each draws its own",
              rel, md.materialSlots.size(), parts.size());
    meshParts_[id] = std::move(parts);
}

#endif

} // namespace aver
