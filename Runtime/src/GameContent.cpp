#include "GameContentInternal.hpp"

namespace aver::game {

void GameContent::adopt(const fmt::ProjectDesc& project) {
    project_ = project;
    contentIndex_.clear();
#if AVER_MODULE_SCENE
    // A project switch is the one place lazy refcounts are dropped (a reload keeps them).
    meshRefs_.clear();
    lazyRel_.clear();
#endif

    const std::string content = project_.contentDir();
    if (content.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(content, ec)) {
        AVER_WARN("[Content] the project's content root does not exist: {}", content);
        return;
    }

    // Use error_code overload to stop on unreadable subdirectory, not throw.
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(it->path(), content, ec).string();
        if (ec || rel.empty()) continue;
        // IDs hash forward-slash spelling, matching C# Assets.ObjectIdOf, so packaging is nearly free.
        for (char& c : rel) if (c == '\\') c = '/';
        contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
    }
    AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);

#if AVER_MODULE_SCENE
    // Animation system caches by id; re-index must clear its cache.
    anim::animSystem().clear();
    anim::animSystem().setResolver(&GameContent::resolveAnimAsset, this);
#endif
}

std::string GameContent::pathFor(u64 id) const {
    const auto it = contentIndex_.find(id);
    return it == contentIndex_.end() ? std::string() : it->second;
}

std::vector<std::string> GameContent::pathsWithExtension(std::string_view ext) const {
    std::vector<std::string> out;
    for (const auto& [id, path] : contentIndex_) {
        if (path.size() < ext.size()) continue;
        // Case-insensitive suffix compare (Windows paths are case-insensitive but string compare is not).
        bool match = true;
        for (usize i = 0; i < ext.size(); ++i) {
            char a = path[path.size() - ext.size() + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) { match = false; break; }
        }
        if (match) out.push_back(path);
    }
    // Sort for reproducible order (unordered_map iteration is not stable).
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<u64, std::string>> GameContent::idsWithExtension(std::string_view ext) const {
    std::vector<std::pair<u64, std::string>> out;
    for (const auto& [id, path] : contentIndex_) {
        if (path.size() < ext.size()) continue;
        bool match = true;
        for (usize i = 0; i < ext.size() && match; ++i) {
            char a = path[path.size() - ext.size() + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            match = a == b;
        }
        if (match) out.emplace_back(id, path);
    }
    std::sort(out.begin(), out.end(), [](const auto& l, const auto& r) { return l.second < r.second; });
    return out;
}

std::string GameContent::resolveAnimAsset(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->pathFor(id) : std::string();
}

#if AVER_MODULE_SCENE

namespace {

// Lowercased, forward-slash, no trailing slash: how lazy folders and relative paths are compared.
std::string lazyKey(std::string s) {
    for (char& c : s) { if (c == '\\') c = '/'; c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// The `lazy=` folders from the STREAM header record of every Content/Maps/*.ocworld. Header lines only:
// stops at the first PLACE/PLACEG/CHILD record.
std::vector<std::string> collectLazyFolders(const std::string& contentDir) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::filesystem::path maps = std::filesystem::path(contentDir) / "Maps";
    if (!std::filesystem::is_directory(maps, ec)) return out;
    for (std::filesystem::directory_iterator it(maps, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".ocworld") continue;
        std::ifstream in(it->path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("PLACE", 0) == 0 || line.rfind("CHILD", 0) == 0) break;
            if (line.rfind("STREAM", 0) != 0) continue;
            usize p = 6;
            while (p < line.size()) {
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
                const usize s = p;
                while (p < line.size() && line[p] != ' ' && line[p] != '\t' && line[p] != '\r') ++p;
                const std::string_view tok(line.data() + s, p - s);
                if (tok.rfind("lazy=", 0) != 0) continue;
                usize q = 5;
                while (q <= tok.size()) {
                    usize e = tok.find(',', q);
                    if (e == std::string_view::npos) e = tok.size();
                    std::string dir = lazyKey(std::string(tok.substr(q, e - q)));
                    if (!dir.empty() && std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(std::move(dir));
                    q = e + 1;
                }
            }
        }
    }
    return out;
}

} // namespace

bool GameContent::loadOneMesh(rhi::IDevice& device, u64 id, const std::string& full, const std::string& rel) {
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(full, md, &why)) { AVER_WARN("[Mesh] {}", why); return false; }
    return uploadMesh(device, id, md, rel);
}

// Thread-free: touches no table and no device, so prefetch workers run it.
void GameContent::prepareMeshCpu(const fmt::OcMeshData& md, const std::string& rel, MeshCpu& out) {
    // Position, normal, uv only. rhi::MeshVertex has nowhere to put joints/weights.
    std::vector<rhi::MeshVertex>& verts = out.verts;
    verts.resize(md.vertexCount());
    for (u32 i = 0; i < md.vertexCount(); ++i) {
        rhi::MeshVertex& v = verts[i];
        v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
        v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
        v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
    }
    // R1: the second vertex stream, for a static mesh that has any of colour, tangent or UV1 (a skinned mesh's vertices are written by compute).
    out.extra.clear();
    if (!md.hasSkin() && md.hasExtra()) {
        out.extra.resize(md.vertexCount());
        const bool c = md.hasColor(), t = md.hasTangent(), u = md.hasUv1();
        for (u32 i = 0; i < md.vertexCount(); ++i)
            out.extra[i] = rhi::packMeshVertexExtra(c ? &md.colors[usize(i)*4] : nullptr, t ? &md.tangents[usize(i)*4] : nullptr,
                                                    u ? &md.uvs1[usize(i)*2] : nullptr);
    }
    // Per-material split: each part an index slice over these vertices (see buildMeshParts).
    out.parts.clear();
    if (md.submeshes.size() <= 1) return;   // common case: nothing to split
    const bool keepBaseIndices = md.hasSkin();
    for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
        if (sm.indexCount == 0) continue;
        const usize end = usize(sm.indexStart) + sm.indexCount;
        if (end > md.indices.size()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' runs past the index buffer; skipped", rel, sm.name);
            continue;
        }
        PartCpu part;
        bool bad = false;
        for (usize k = sm.indexStart; k < end && !bad; ++k) bad = md.indices[k] >= verts.size();
        if (bad) {
            AVER_WARN("[Mesh] '{}' submesh '{}' indexes a vertex it does not have; skipped", rel, sm.name);
            continue;
        }
        part.indices.assign(md.indices.data() + sm.indexStart, md.indices.data() + end);
        part.slot = sm.materialSlot;
        part.name = sm.name;
        // baseIndices[i] is the slice parts[i] was cut from (lockstep).
        if (keepBaseIndices) part.baseIndices = part.indices;
        out.parts.push_back(std::move(part));
    }
}

bool GameContent::uploadMesh(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md, const std::string& rel,
                             MeshCpu* cpu) {
    HitchMarks hm(rel.c_str(), 0.25);
    MeshCpu local;
    if (!cpu) { prepareMeshCpu(md, rel, local); cpu = &local; }
    hm.mark("upload:prepare");
    const std::vector<rhi::MeshVertex>& verts = cpu->verts;
    const bool staged = cpu->staged != nullptr;
    rhi::MeshHandle h = staged ? device.adoptMesh(std::move(cpu->staged)) : 0;
    if (hitchVerbose() && !h && meshStager_) AVER_INFO("[Mesh] {} not staged ({})", rel, staged ? "adopt refused" : "none");
    if (!h) h = device.createMesh(verts.data(), (u32)verts.size(), md.indices.data(), (u32)md.indices.size());
    hm.mark("upload:createMesh");
    if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); return false; }
    // R1: attached before any part or LOD shares this mesh's vertices, so they borrow the stream. Refused = the mesh draws without it.
    if (!cpu->extra.empty() && !device.setMeshExtra(h, cpu->extra.data(), static_cast<u32>(cpu->extra.size())))
        AVER_WARN("[Mesh] '{}': the device kept no vertex colour / tangent / UV1 stream; it draws without them", rel);

    sceneMeshes_[id] = h;
    meshBounds_[id] = {md.boundsMin, md.boundsMax};
    // Mesh's own material slot; see GameContent.hpp::meshDefaultMaterial.
    if (!md.materialSlots.empty() && !md.materialSlots[0].empty()) {
        meshSlot0Material_[id] = aver_scene_material(0, md.materialSlots[0].c_str());
        meshSlot0Name_[id] = md.materialSlots[0];
    }
    // The per-submesh split: a mesh naming several materials draws one part per material.
    buildMeshParts(device, id, md, h, verts, cpu->extra, cpu->parts, rel);
    hm.mark("upload:parts");
    projectMeshIds_.push_back(id);
    if (meshLoaded_) meshLoaded_(LoadedMesh{id, rel, &md, verts, md.indices, h}, meshLoadedUser_);
    if (lazyLoading_ && meshAcquired_)
        meshAcquired_(LoadedMesh{id, rel, &md, verts, md.indices, h, &cpu->lodStaged}, meshAcquiredUser_);
    hm.mark("upload:hooks");

    // Coarse LOD for shadow/GI/voxelise passes (depth-only). 20cm target error (one shadow texel).
#if AVER_MODULE_TRIFACTOR
    if (buildDepthProxies_ && md.lodCount() > 1) {
        constexpr f32 kShadowErrorCm = 20.0f;
        u32 pick = 0;
        for (u32 lvl = 1; lvl < md.lodCount(); ++lvl)
            if (trifactor::levelWorldErrorCm(md, lvl) <= kShadowErrorCm) pick = lvl;

        // Only upload if it's actually cheaper than LOD 0.
        const u32 tris0 = trifactor::levelTriangleCount(md, 0);
        if (pick > 0 && trifactor::levelTriangleCount(md, pick) < tris0) {
            const fmt::OcMeshLod& lod = md.coarserLods[pick - 1];
            // LOD 0's OWN vertex array: a coarser level owns its index buffer but shares the one
            // VTXS block (see OcMeshData::coarserLods), which is why `verts` is correct here.
            rhi::MeshHandle ph = device.createMeshSharingVertices(h, lod.indices.data(), (u32)lod.indices.size());
            if (!ph) ph = device.createMesh(verts.data(), (u32)verts.size(), lod.indices.data(), (u32)lod.indices.size());
            if (ph) {
                depthProxyMap_[h] = ph;
                AVER_INFO("[Mesh] '{}' depth proxy: LOD {} ({} tris, {:.1f}x less than LOD 0, {:.1f}cm error)",
                          rel, pick, trifactor::levelTriangleCount(md, pick),
                          static_cast<f64>(tris0) /
                              static_cast<f64>(trifactor::levelTriangleCount(md, pick)),
                          trifactor::levelWorldErrorCm(md, pick));
            } else {
                AVER_WARN("[Mesh] '{}' depth proxy LOD {} refused by the device; it draws at full detail",
                          rel, pick);
            }
        }
    }
#endif
    return true;
}

void GameContent::loadProjectMeshes(rhi::IDevice& device) {
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    meshStager_ = device.meshStager();
    AVER_INFO("[Mesh] streamed meshes' GPU buffers are made {}",
              meshStager_ ? "on the prefetch workers" : "on the main thread (no stager on this device)");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    const std::vector<std::string> lazyDirs = collectLazyFolders(dir);
    lazyRel_.clear();   // rebuilt by the scan below; refcounts stay

    u32 loaded = 0, failed = 0, lazy = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Mesh) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        const u64 id = fnv1a64(std::string_view(rel));
        if (!lazyDirs.empty()) {
            const std::string key = lazyKey(rel);
            bool isLazy = false;
            for (const std::string& d : lazyDirs)
                if (key.size() > d.size() && key.compare(0, d.size(), d) == 0 && key[d.size()] == '/') { isLazy = true; break; }
            if (isLazy) {
                contentIndex_[id] = full;
                lazyRel_[id] = rel;
                ++lazy;
                continue;
            }
            lazyRel_.erase(id);   // stale entry from an earlier load: this mesh is eager now
        }

        if (loadOneMesh(device, id, full, rel)) ++loaded; else ++failed;
    }
    // A reload keeps refcounts: re-upload lazy meshes still held by resident entities/foliage.
    for (const auto& [id, refs] : meshRefs_) {
        if (refs == 0 || sceneMeshes_.count(id)) continue;
        const auto lit = lazyRel_.find(id);
        const auto pit = contentIndex_.find(id);
        if (lit == lazyRel_.end() || pit == contentIndex_.end() || !loadOneMesh(device, id, pit->second, lit->second)) {
            ++failed;
            AVER_WARN("[Mesh] held lazy mesh {} could not be re-uploaded after reload", id);
        } else ++loaded;
    }
    if (loaded || failed || lazy)
        AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "",
                  lazy ? (", " + std::to_string(lazy) + " indexed for on-demand load") : "");
}

bool GameContent::acquireMesh(rhi::IDevice& device, u64 id) {
    const auto lit = lazyRel_.find(id);
    if (lit == lazyRel_.end()) return sceneMeshes_.count(id) != 0;   // eager: permanently loaded
    u32& refs = meshRefs_[id];
    if (refs == 0 && sceneMeshes_.count(id) == 0) {
        const auto pit = contentIndex_.find(id);
        lazyLoading_ = true;
        bool ok = false;
        if (pit != contentIndex_.end()) {
            fmt::OcMeshData md;
            MeshCpu cpu;
            std::unique_ptr<CollisionMesh> collision;
            i32 shape = 0;
            std::string why;
            const int got = takePrefetch(id, md, cpu, collision, shape, why);
            if (got < 0) {
                if (hitchVerbose())
                    AVER_INFO("[Mesh] {} read on the main thread ({}, {})", lit->second,
                              got == -2 ? "its prefetch was still reading" : "never prefetched",
                              acquireTag ? acquireTag : "other");
                ok = loadOneMesh(device, id, pit->second, lit->second);
            } else if (got == 0) {
                AVER_WARN("[Mesh] {}", why);
            } else {
                ok = uploadMesh(device, id, md, lit->second, &cpu);
                if (ok && collision && !collisionMeshCache_.count(id)) collisionMeshCache_[id] = std::move(collision);
                if (ok && shape) { readyShapes_[id] = shape; shape = 0; }
            }
#  if AVER_MODULE_PHYSICS
            if (shape) aver_phys_release_mesh_shape(shape);
#  endif
        }
        lazyLoading_ = false;
        if (!ok) {
            meshRefs_.erase(id);
            return false;
        }
    }
    ++refs;
    return true;
}

void GameContent::releaseMesh(rhi::IDevice& device, u64 id) {
    (void)device;
    if (!lazyRel_.count(id)) return;
    const auto rit = meshRefs_.find(id);
    if (rit == meshRefs_.end() || rit->second == 0) return;
    if (--rit->second > 0) return;
    meshRefs_.erase(rit);
    unloadToPending(id);
}

bool GameContent::meshLoaded(u64 id) const { return sceneMeshes_.count(id) != 0; }
bool GameContent::uploadBuiltMesh(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md, const std::string& rel) {
    if (sceneMeshes_.count(id)) unloadToPending(id);
    lazyLoading_ = true;
    const bool ok = uploadMesh(device, id, md, rel);
    lazyLoading_ = false;
    return ok;
}

void GameContent::releaseBuiltMesh(u64 id) {
    if (sceneMeshes_.count(id)) unloadToPending(id);
}

void GameContent::unloadToPending(u64 id) {
    PendingMesh p;
    p.releasedAt = lastFlushFrame_;
    for (auto it = posedParts_.begin(); it != posedParts_.end();) {
        if (it->second.meshId == id) {
            for (const MeshPart& q : it->second.parts) if (q.mesh) p.others.push_back(q.mesh);
            it = posedParts_.erase(it);
        } else ++it;
    }
    meshPartBaseIndices_.erase(id);
    if (const auto pit = meshParts_.find(id); pit != meshParts_.end()) {
        for (const MeshPart& q : pit->second) if (q.mesh) p.others.push_back(q.mesh);
        meshParts_.erase(pit);
    }
    if (const auto sit = sceneMeshes_.find(id); sit != sceneMeshes_.end()) {
        if (const auto dit = depthProxyMap_.find(sit->second); dit != depthProxyMap_.end()) {
            if (dit->second) p.others.push_back(dit->second);
            depthProxyMap_.erase(dit);
        }
        p.base = sit->second;
        sceneMeshes_.erase(sit);
    }
    meshBounds_.erase(id);
    meshSlot0Material_.erase(id);
    meshSlot0Name_.erase(id);
    collisionMeshCache_.erase(id);
#  if AVER_MODULE_PBR
    if (const auto mn = meshMaterialNames_.find(id); mn != meshMaterialNames_.end()) {
        for (const std::string& name : mn->second)
            if (const auto u = materialUse_.find(name); u != materialUse_.end() && u->second.meshes && !--u->second.meshes)
                u->second.idleSince = streamTick_;
        meshMaterialNames_.erase(mn);
    }
#  endif
    if (const auto rs = readyShapes_.find(id); rs != readyShapes_.end()) {
#  if AVER_MODULE_PHYSICS
        aver_phys_release_mesh_shape(rs->second);
#  endif
        readyShapes_.erase(rs);
    }
    projectMeshIds_.erase(std::remove(projectMeshIds_.begin(), projectMeshIds_.end(), id), projectMeshIds_.end());
    pendingMeshes_.push_back(std::move(p));
    if (meshReleased_) meshReleased_(id, meshReleasedUser_);
}

void GameContent::releaseMeshHandle(rhi::MeshHandle h) {
    if (!h) return;
    // Into the entry the released hook's own mesh just made, whose `others` go before its base.
    if (pendingMeshes_.empty() || pendingMeshes_.back().releasedAt != lastFlushFrame_) {
        PendingMesh p;
        p.releasedAt = lastFlushFrame_;
        pendingMeshes_.push_back(std::move(p));
    }
    pendingMeshes_.back().others.push_back(h);
}

void GameContent::flushMeshReleases(rhi::IDevice& device, u64 frameIndex) {
    lastFlushFrame_ = frameIndex;
    usize w = 0;
    for (usize i = 0; i < pendingMeshes_.size(); ++i) {
        PendingMesh& p = pendingMeshes_[i];
        if (frameIndex < p.releasedAt + 3) {
            if (w != i) pendingMeshes_[w] = std::move(p);
            ++w;
            continue;
        }
        // destroyMesh refuses while shared buffers are still referenced: keep those and retry.
        usize kept = 0;
        for (const rhi::MeshHandle m : p.others)
            if (!device.destroyMesh(m)) p.others[kept++] = m;
        p.others.resize(kept);
        if (p.base && device.destroyMesh(p.base)) p.base = 0;
        if (p.others.empty() && !p.base) continue;
        if (!p.warned) {
            p.warned = true;
            AVER_WARN("[Mesh] a released mesh could not be destroyed yet (shared buffers still referenced); retrying");
        }
        if (w != i) pendingMeshes_[w] = std::move(p);
        ++w;
    }
    pendingMeshes_.resize(w);
}

// Splits a mesh that names more than one material into one MeshHandle per slot. Ported from
// SandboxApp::buildMeshParts (sandbox/src/SandboxAssets.cpp) -- see that function's own comment for
// why splitting at load time, rather than drawing per-submesh RANGES, is what makes this tractable at
// all (the ray path's BLAS carries one materialIndex per instance, so a range draw would still shade
// flat in the renderer that is actually on screen).
//
// SHARING THE PARENT'S VERTICES: each part owns only its index slice (createMeshSharingVertices), so a
// multi-material mesh holds its vertices once (compacted copies doubled them, in VRAM since meshes moved to
// the Default heap). Parts take the parent's bounds. A device that refuses the share gets the whole vertex
// array per part, which is correct, just larger.
void GameContent::buildMeshParts(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md, rhi::MeshHandle whole,
                                  const std::vector<rhi::MeshVertex>& verts, const std::vector<rhi::MeshVertexExtra>& extra,
                                  std::vector<PartCpu>& cpuParts, const std::string& rel) {
    if (cpuParts.empty()) return;
    std::vector<MeshPart> parts;
    parts.reserve(cpuParts.size());
    const bool keepBaseIndices = md.hasSkin();
    std::vector<std::vector<u32>> baseIndices;
    for (PartCpu& pc : cpuParts) {
        MeshPart part;
        part.mesh = pc.staged ? device.adoptMeshSharingVertices(whole, std::move(pc.staged)) : 0;
        if (!part.mesh)
            part.mesh = device.createMeshSharingVertices(whole, pc.indices.data(), static_cast<u32>(pc.indices.size()));
        if (!part.mesh) {
            part.mesh = device.createMesh(verts.data(), static_cast<u32>(verts.size()),
                                          pc.indices.data(), static_cast<u32>(pc.indices.size()));
            // The device refused the share, so this part owns a whole vertex copy and needs its own second stream.
            if (part.mesh && !extra.empty()) device.setMeshExtra(part.mesh, extra.data(), static_cast<u32>(extra.size()));
        }
        if (!part.mesh) {
            AVER_WARN("[Mesh] the device refused submesh '{}' of '{}'", pc.name, rel);
            continue;
        }
        part.slot = pc.slot;
        // Slot names the material, resolving through the same path as an authored material.
        if (pc.slot < md.materialSlots.size()) {
            const std::string& slot = md.materialSlots[pc.slot];
            if (!slot.empty()) part.material = aver_scene_material(0, slot.c_str());
        }
        parts.push_back(part);
        if (keepBaseIndices) baseIndices.push_back(std::move(pc.baseIndices));
    }

    // One surviving part is not a split; falling through costs one less draw call.
    if (parts.size() <= 1) {
        for (const MeshPart& p : parts) if (p.mesh) device.destroyMesh(p.mesh);
        return;
    }
    if (!lazyLoading_)
        AVER_INFO("[Mesh] '{}' names {} materials; split into {} part(s) so each draws its own",
                  rel, md.materialSlots.size(), parts.size());
    meshParts_[id] = std::move(parts);
    if (keepBaseIndices) meshPartBaseIndices_[id] = std::move(baseIndices);
}

const std::vector<GameContent::MeshPart>* GameContent::partsFor(u64 id) const {
    const auto it = meshParts_.find(id);
    return it == meshParts_.end() ? nullptr : &it->second;
}

const std::vector<GameContent::MeshPart>* GameContent::posedPartsFor(rhi::IDevice& device, u64 id,
        rhi::MeshHandle baseMesh, rhi::MeshHandle posedMesh) {
    if (!posedMesh || !baseMesh || posedMesh == baseMesh) return nullptr;
    if (const auto it = posedParts_.find(posedMesh); it != posedParts_.end())
        return (it->second.meshId == id && !it->second.parts.empty()) ? &it->second.parts : nullptr;
    PosedParts& entry = posedParts_[posedMesh];   // cache the refusal so it is not retried per frame
    entry.meshId = id;
    const auto pit = meshParts_.find(id);
    const auto bit = meshPartBaseIndices_.find(id);
    // Refusal (cached per posed handle): multi-material character draws whole under one material.
    if (pit == meshParts_.end() || bit == meshPartBaseIndices_.end() ||
        bit->second.size() != pit->second.size()) {
        if (pit != meshParts_.end())   // no parts at all is a single-material mesh: nothing to say
            AVER_WARN("[Mesh] posed split skipped for mesh {} (posed handle {}): its per-slot index "
                      "slices were not kept; it draws as one mesh", id, posedMesh);
        return nullptr;
    }
    // Skin target shares source's index buffer verbatim, so equal index buffers and vertex counts
    // mean it was cut from baseMesh. After mesh reload they differ; entity keeps single draw.
    rhi::BufferHandle baseIb = 0, posedIb = 0;
    u32 baseVc = 0, posedVc = 0;
    if (!device.meshGeometry(baseMesh, nullptr, &baseIb, &baseVc, nullptr) ||
        !device.meshGeometry(posedMesh, nullptr, &posedIb, &posedVc, nullptr) ||
        baseIb == 0 || baseIb != posedIb || baseVc != posedVc) {
        AVER_WARN("[Mesh] posed split skipped for mesh {}: posed handle {} was not cut from this "
                  "upload (base {}; reloaded since it was skinned?); it draws as one mesh",
                  id, posedMesh, baseMesh);
        return nullptr;
    }
    std::vector<MeshPart> out;
    out.reserve(pit->second.size());
    for (usize i = 0; i < pit->second.size(); ++i) {
        MeshPart pp;
        pp.material = pit->second[i].material;
        pp.slot = pit->second[i].slot;
        const std::vector<u32>& idx = bit->second[i];
        if (pit->second[i].mesh && !idx.empty()) {
            pp.mesh = device.createPosedPartMesh(posedMesh, idx.data(), static_cast<u32>(idx.size()));
            // All or nothing: half-split character silently loses failed parts; whole-mesh fallback draws all.
            if (!pp.mesh) {
                for (const MeshPart& q : out) if (q.mesh) device.destroyMesh(q.mesh);
                AVER_WARN("[Mesh] posed split refused for mesh {} (posed handle {}); it draws as one mesh",
                          id, posedMesh);
                return nullptr;
            }
        }
        out.push_back(pp);
    }
    entry.parts = std::move(out);
    AVER_INFO("[Mesh] mesh {}: {} posed part(s) over posed handle {}", id, entry.parts.size(), posedMesh);
    return &entry.parts;
}

void GameContent::registerMesh(u64 id, rhi::MeshHandle handle, const std::pair<Vec3, Vec3>& bounds) {
    sceneMeshes_[id] = handle;
    meshBounds_[id] = bounds;
}

void GameContent::releaseProjectMeshes(rhi::IDevice& device, bool destroyBaseHandles) {
    dropPrefetches();
    // Posed parts hold a vertex share on a skin target (which would refuse its own destruction).
    for (auto& kv : posedParts_)
        for (const MeshPart& p : kv.second.parts)
            if (p.mesh) device.destroyMesh(p.mesh);
    posedParts_.clear();
    for (const u64 id : projectMeshIds_) {
        meshPartBaseIndices_.erase(id);
        // Split parts are this class's uploads; nothing else keys on them.
        if (const auto pit = meshParts_.find(id); pit != meshParts_.end()) {
            for (const MeshPart& p : pit->second)
                if (p.mesh) device.destroyMesh(p.mesh);
            meshParts_.erase(pit);
        }
        if (const auto sit = sceneMeshes_.find(id); sit != sceneMeshes_.end()) {
            // Depth proxy is keyed on the base handle.
            if (const auto dit = depthProxyMap_.find(sit->second); dit != depthProxyMap_.end()) {
                if (dit->second) device.destroyMesh(dit->second);
                depthProxyMap_.erase(dit);
            }
            if (destroyBaseHandles && sit->second) device.destroyMesh(sit->second);
            sceneMeshes_.erase(sit);
        }
        meshBounds_.erase(id);
        meshSlot0Material_.erase(id);
        meshSlot0Name_.erase(id);
        collisionMeshCache_.erase(id);
    }
    projectMeshIds_.clear();
    // Lazy meshes already released but still waiting out their frames: nothing draws them, free now.
    for (const PendingMesh& p : pendingMeshes_) {
        for (const rhi::MeshHandle m : p.others) device.destroyMesh(m);
        if (destroyBaseHandles && p.base) device.destroyMesh(p.base);
    }
    pendingMeshes_.clear();
}

const std::pair<Vec3, Vec3>* GameContent::boundsFor(u64 id) const {
    const auto it = meshBounds_.find(id);
    return it == meshBounds_.end() ? nullptr : &it->second;
}

rhi::MeshHandle GameContent::resolveSceneMesh(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->meshFor(id) : 0;
}

const GameContent::SurfaceLook* GameContent::lookFor(i32 material) const {
    const auto it = surfaceLooks_.find(material);
    return it == surfaceLooks_.end() ? nullptr : &it->second;
}

#if AVER_MODULE_SCENE
i32 GameContent::meshDefaultMaterial(u64 meshId) const {
    const auto it = meshSlot0Material_.find(meshId);
    return it == meshSlot0Material_.end() ? 0 : it->second;
}

const std::string& GameContent::meshSlot0Name(u64 meshId) const {
    static const std::string kNone;
    const auto it = meshSlot0Name_.find(meshId);
    return it == meshSlot0Name_.end() ? kNone : it->second;
}
#endif

#endif // AVER_MODULE_SCENE

// Mesh lookup in content index; empty table means no scene state.
rhi::MeshHandle GameContent::meshFor(u64 id) const {
    const auto it = sceneMeshes_.find(id);
    return it == sceneMeshes_.end() ? 0 : it->second;
}

} // namespace aver::game
