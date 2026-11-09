#include "GameContentInternal.hpp"

namespace aver::game {

#if AVER_MODULE_PBR

std::string GameContent::resolveAssetPath(const pbr::TextureRef& ref) const {
    if (!ref.path.empty()) {
        const std::string& p = ref.path;
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
        if (absolute) return p;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            const std::string full = content + AVER_PATH_SEP + p;
            std::error_code ec;
            if (std::filesystem::exists(full, ec)) return full;
        }
        return p;
    }
    if (ref.id) return pathFor(ref.id);
    return {};
}

pbr::MaterialSystem::ResolvedTexture GameContent::resolveMaterialTextureAsync(const pbr::TextureRef& ref,
                                                                              pbr::TextureSlot slot,
                                                                              pbr::MaterialSystem::TextureAsk ask,
                                                                              void* user) {
    using Ask = pbr::MaterialSystem::TextureAsk;
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_) return {};

    // A relative path is stat-ed to find its file; asked every frame while pending, that is remembered.
    std::string path;
    if (!ref.path.empty() && ref.id == 0) {
        const auto m = self->assetPathMemo_.find(ref.path);
        if (m != self->assetPathMemo_.end()) path = m->second;
    }
    if (path.empty()) {
        path = self->resolveAssetPath(ref);
        std::error_code ec;
        if (!ref.path.empty() && ref.id == 0 && !path.empty() && std::filesystem::exists(path, ec))
            self->assetPathMemo_.emplace(ref.path, path);
    }
    if (path.empty()) {
        AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps its fallback",
                  ref.id, pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }

    const assets::TextureUsage usage = textureUsageFor(slot);
    std::string err;
    pbr::MaterialSystem::ResolvedTexture out;
    switch (assets::requestTexture(path, usage, ask == Ask::Prefetch ? assets::TexturePriority::Prefetch
                                                                     : assets::TexturePriority::Demand, &err)) {
        case assets::TextureState::Pending: out.pending = true; return out;
        case assets::TextureState::Failed:
            AVER_WARN("[Material] {} - slot '{}' keeps its fallback", err, pbr::MaterialLibrary::textureSlotName(slot));
            return {};
        case assets::TextureState::Ready: break;
    }
    if (ask != Ask::Take) { out.pending = true; return out; }   // prepared; waits for upload budget

    assets::TextureUploadInfo info;
    const rhi::TextureHandle h = assets::takeTexture(*self->textureFactory_, path, usage, &err, &info);
    if (!h) {
        AVER_WARN("[Material] {} - slot '{}' keeps its fallback", err, pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }
    AVER_INFO("[Material] {} -> {}x{}, {} mips ({} KB) for slot '{}'", path, info.width, info.height,
              info.mips, info.bytes / 1024, pbr::MaterialLibrary::textureSlotName(slot));
    out.handle = h;
    for (int c = 0; c < 3; ++c) out.averageLinear[c] = info.averageLinear[c];
    out.bytes = info.bytes;
    return out;
}

bool GameContent::asyncTexturesWanted(bool boundedRun) {
    if (boundedRun) return false;
    const char* v = std::getenv("AVER_ASYNC_TEXTURES");
    return !(v && v[0] == '0');
}

usize GameContent::prefetchMaterial(const std::string& name) {
    if (!asyncTextures_ || !textureFactory_ || name.empty()) return 0;
    const pbr::MaterialHandle h = loadSurfaceMaterial(name);
    return h ? queueMaterialTextures(h) : 0;
}

usize GameContent::queueMaterialTextures(pbr::MaterialHandle h) {
    const pbr::MaterialDesc* d = asyncTextures_ && textureFactory_ ? pbr::MaterialLibrary::get().desc(h) : nullptr;
    if (!d || d->terrain.enabled()) return 0;   // a terrain material's atlas is built whole, on demand
    usize queued = 0;
    for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
        const pbr::TextureRef& ref = d->textures[i];
        if (ref.empty() || ref.path.rfind("@rt:", 0) == 0) continue;
        if (i == pbr::kEmbeddedTableSlot && d->embedded.active()) continue;
        const pbr::MaterialSystem::ResolvedTexture r = resolveMaterialTextureAsync(
            ref, static_cast<pbr::TextureSlot>(i), pbr::MaterialSystem::TextureAsk::Prefetch, this);
        if (r.pending) ++queued;
    }
    return queued;
}

pbr::MaterialSystem::ResolvedTexture GameContent::resolveMaterialTexture(const pbr::TextureRef& ref,
                                                                         pbr::TextureSlot slot, void* user) {
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_) return {};

    const std::string path = self->resolveAssetPath(ref);
    if (path.empty()) {
        AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps its fallback",
                  ref.id, pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }

    const assets::TextureUsage usage = textureUsageFor(slot);

    std::string err;
    assets::TextureUploadInfo info;
    const rhi::TextureHandle h = assets::uploadTexture(*self->textureFactory_, path, usage, &err, &info);
    if (!h) {
        AVER_WARN("[Material] {} - slot '{}' keeps its fallback", err,
                  pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }
    AVER_INFO("[Material] {} -> {}x{}, {} mips ({} KB) for slot '{}'", path, info.width, info.height,
              info.mips, info.bytes / 1024, pbr::MaterialLibrary::textureSlotName(slot));
    // Mean in handle: needed for sampling fallbacks.
    pbr::MaterialSystem::ResolvedTexture out;
    out.handle = h;
    for (int c = 0; c < 3; ++c) out.averageLinear[c] = info.averageLinear[c];
    // Bytes: used for level-change eviction log.
    out.bytes = info.bytes;
    return out;
}

// ---- wave3 terrain layers ----
bool GameContent::resolveTerrainAtlas(const pbr::MaterialDesc& d, pbr::TerrainTextures& out, void* user) {
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_ || !d.terrain.enabled()) return false;
    const u32 count = static_cast<u32>(std::min<usize>(d.terrain.layers.size(), pbr::kMaxTerrainLayers));

    // Up to 4 columns: 8 layers are 4 x 2, fewer shrink the atlas to what they need.
    assets::AtlasSpec spec;
    spec.cols = std::min<u32>(count, 4u);
    spec.rows = (count + spec.cols - 1) / spec.cols;
    spec.tileTexels = d.terrain.tileTexels;
    spec.padTexels = d.terrain.padTexels;
    assets::AtlasSpec albedo = spec, normal = spec, orm = spec;
    albedo.usage = assets::TextureUsage::Colour;
    normal.usage = assets::TextureUsage::NormalMap;
    orm.usage = assets::TextureUsage::Data;
    for (u32 i = 0; i < count; ++i) {
        const pbr::TerrainLayerDesc& l = d.terrain.layers[i];
        assets::AtlasTile a, n, o;
        a.colorPath = self->resolveAssetPath(l.baseColor);
        a.alphaPath = self->resolveAssetPath(l.height);
        a.alphaFromLuma = a.alphaPath.empty();            // no height map: the albedo's luminance stands in
        n.colorPath = self->resolveAssetPath(l.normal);
        n.fill[0] = 0.5f; n.fill[1] = 0.5f; n.fill[2] = 1.0f; n.fill[3] = 1.0f;   // a flat tangent-space normal
        o.colorPath = self->resolveAssetPath(l.metalRough);
        o.fill[0] = 1.0f; o.fill[1] = 1.0f; o.fill[2] = 0.0f; o.fill[3] = 1.0f;   // full AO, rough, non-metal
        albedo.tiles.push_back(std::move(a));
        normal.tiles.push_back(std::move(n));
        orm.tiles.push_back(std::move(o));
    }

    assets::AtlasImage ia, in, io;
    std::string err;
    if (!assets::buildAtlasImage(albedo, ia, &err) || !assets::buildAtlasImage(normal, in, &err) ||
        !assets::buildAtlasImage(orm, io, &err)) {
        AVER_WARN("[Terrain] material '{}': the atlas could not be built: {}", d.name, err);
        return false;
    }
    const usize bytes = [&] { usize b = 0; for (const auto* im : {&ia, &in, &io}) for (const auto& l : im->levels) b += l.size(); return b; }();
    if (bytes > (160u << 20))
        AVER_WARN("[Terrain] material '{}': the atlases take {} MiB of GPU memory (tile {}); use 512 texel tiles to save memory",
                  d.name, bytes >> 20, d.terrain.tileTexels);

    out = pbr::TerrainTextures{};
    out.albedo = assets::uploadAtlas(*self->textureFactory_, ia, true, &err);
    out.normal = assets::uploadAtlas(*self->textureFactory_, in, false, &err);
    out.orm = assets::uploadAtlas(*self->textureFactory_, io, false, &err);
    if (!out.albedo || !out.normal || !out.orm) {
        AVER_WARN("[Terrain] material '{}': the atlas could not be uploaded: {}", d.name, err);
        for (rhi::TextureHandle t : {out.albedo, out.normal, out.orm}) if (t) self->textureFactory_->destroyTexture(t);
        out = pbr::TerrainTextures{};
        return false;
    }
    const u32 S = spec.tileTexels + 2 * spec.padTexels;
    out.cols = spec.cols; out.rows = spec.rows; out.stride = S; out.tile = spec.tileTexels; out.pad = spec.padTexels;
    out.atlasW = ia.width; out.atlasH = ia.height;
    out.bytes = bytes;
    for (u32 i = 0; i < count; ++i) {
        out.avg[i][0] = ia.tileAverage[i][0]; out.avg[i][1] = ia.tileAverage[i][1]; out.avg[i][2] = ia.tileAverage[i][2];
        out.avg[i][3] = io.tileAverage[i][1];   // ORM: G = roughness
        out.avg[i][4] = io.tileAverage[i][2];   // B = metallic
    }
    AVER_INFO("[Terrain] material '{}': {} layer(s) packed into {}x{} atlases ({} MiB)", d.name, count, ia.width, ia.height, bytes >> 20);
    return true;
}
// ---- end wave3 terrain layers ----

// .ocmat GRAPHREF path to gMaterialGraphId. Returns 0 for materials with no graph or load failure.
// Broken graph does not disable the material: falls back to stock factors/maps.
u32 GameContent::resolveMaterialGraph(const std::string& graphRef,
                                      const std::vector<std::pair<std::string, std::string>>& params) const {
    if (graphRef.empty()) return 0;
    const std::string content = project_.contentDir();
    if (content.empty()) return 0;

    // Content-relative path (matches COMP mesh= and TEX conventions).
    std::string path = content + AVER_PATH_SEP + graphRef;
    toNativeSeparators(path);

    // Compile once: multiple materials can share a graph. A parameter set is part of the identity.
    const std::string key = params.empty() ? path : path + "#" + fmt::materialParameterKey(params);
    if (const u32 known = pbr::materialGraphs().idOf(key)) return known;

    fmt::OcGraphData g;
    std::string err;
    if (!fmt::loadOcgraph(path, g, &err)) {
        AVER_ERROR("[MaterialGraph] '{}' could not be read, so the material shades as a stock "
                   "one: {}", path, err);
        return 0;
    }
    const fmt::MaterialGraphReader readFn = [&content](const std::string& rel, fmt::OcGraphData& out, std::string* e) {
        std::string p = content + AVER_PATH_SEP + rel;
        toNativeSeparators(p);
        return fmt::loadOcgraph(p, out, e);
    };
    if (!fmt::inlineMaterialFunctions(g, readFn, &err)) {
        AVER_ERROR("[MaterialGraph] '{}': {}", path, err);
        return 0;
    }
    std::vector<std::string> unknown;
    if (!fmt::applyMaterialGraphParameters(g, params, &unknown, &err)) {
        AVER_ERROR("[MaterialGraph] '{}': {}", path, err);
        return 0;
    }
    for (const std::string& u : unknown)
        AVER_WARN("[MaterialGraph] '{}' has no parameter '{}' -- the override is ignored", path, u);
    return pbr::materialGraphs().add(key, g.name, g);
}

pbr::MaterialHandle GameContent::materialForSurface(const std::string& name) {
    if (name.empty()) return 0;
    materialUse_[name].kept = true;
    return loadSurfaceMaterial(name);
}

pbr::MaterialHandle GameContent::loadSurfaceMaterial(const std::string& name) {
    if (name.empty()) return 0;
    const auto cached = materialAssets_.find(name);
    if (cached != materialAssets_.end()) return cached->second;

    pbr::MaterialHandle h = 0;
    const std::string content = project_.contentDir();
    if (!content.empty()) {
        // Built .ocmat (Binaries) wins over hand-authored (Content).
        const std::string candidates[3] = {
            project_.binariesDir() + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
            content + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
            content + AVER_PATH_SEP + name,
        };
        for (const std::string& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) continue;
            pbr::MaterialDesc d;
            fmt::OcMatExtras extras;
            std::string err;
            // Parse failure breaks: corrupt built material must not fall through to stale hand-authored.
            std::vector<std::pair<std::string, std::string>> instanceParams;
            if (!fmt::loadOcmatAny(path, content, d, &extras, &instanceParams, &err)) { AVER_WARN("[Material] {}", err); break; }
            d.graphId = resolveMaterialGraph(extras.graphRef, instanceParams);
            h = pbr::MaterialLibrary::get().create(d);
            if (h) {
                AVER_INFO("[Material] '{}' loaded from {}{}", d.name, path,
                          d.graphId ? " (graph " + std::to_string(d.graphId) + ")" : "");
                queueMaterialTextures(h);   // the workers start on its textures now, before anything draws it
            }
            break;
        }
    }
    // Cache 0 (no retries) to avoid stat calls for missing surfaces.
    materialAssets_.emplace(name, h);
    MaterialUse& u = materialUse_[name];
    if (!u.meshes) u.idleSince = streamTick_;
    return h;
}

void GameContent::releaseIdleMaterials() {
    constexpr u64 kIdleTicks = 600;   // ~10 s: a cell flickering at the evict edge does not reload them
    usize released = 0;
    for (auto it = materialUse_.begin(); it != materialUse_.end();) {
        const MaterialUse& u = it->second;
        if (u.kept || u.meshes || u.idleSince + kIdleTicks > streamTick_) { ++it; continue; }
        if (const auto a = materialAssets_.find(it->first); a != materialAssets_.end()) {
            if (const pbr::MaterialHandle h = a->second) {
#  if AVER_MODULE_SCENE
                for (auto s = surfaceMaterials_.begin(); s != surfaceMaterials_.end();)
                    s = s->second == h ? surfaceMaterials_.erase(s) : std::next(s);
#  endif
                pbr::MaterialLibrary::get().destroy(h);   // MaterialSystem frees its textures at its next update
                ++released;
            }
            materialAssets_.erase(a);
        }
        it = materialUse_.erase(it);
    }
    if (!released) return;
    assets::forgetUploadedTextures();   // their textures may be read ahead again
    AVER_INFO("[Material] {} streamed material(s) no loaded mesh uses released", released);
}

bool GameContent::buildNextMaterial() {
    if (materialBinds_.empty()) return false;
    const MaterialBind b = std::move(materialBinds_.front());
    materialBinds_.pop_front();
    if (const pbr::MaterialHandle h = loadSurfaceMaterial(b.name)) {
        if (materialSystem_) materialSystem_->bindingSet(h);   // builds it unless already built
        bindSurfaceMaterial(b.token, h);
    }
    return true;
}

void GameContent::releaseProjectMaterials(bool clearGraphRegistry) {
    materialBinds_.clear();
    assets::dropPrefetchedTextures();
    assets::cancelTextureRequests();
    assetPathMemo_.clear();
    // Destroy materials (library owns them, map only names).
    for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
    materialAssets_.clear();
    materialUse_.clear();
    unresolvedSurfaces_.clear();
    meshMaterialNames_.clear();
    // Material graphs cached process-wide by path; clear to prevent id collisions on project reload.
    if (clearGraphRegistry) pbr::materialGraphs().clear();
#if AVER_MODULE_SCENE
    surfaceMaterials_.clear();
#endif
}

// Release all materials except those in keep set.
usize GameContent::releaseMaterialsExcept(const std::unordered_set<std::string>& keep) {
    assets::dropPrefetchedTextures();   // what it releases may be needed again
    assets::cancelTextureRequests();    // a kept material still waiting asks again at its next update
    unresolvedSurfaces_.clear();
    usize released = 0;
    for (auto it = materialAssets_.begin(); it != materialAssets_.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        if (it->second) { pbr::MaterialLibrary::get().destroy(it->second); ++released; }
        materialUse_.erase(it->first);
        it = materialAssets_.erase(it);
    }
#if AVER_MODULE_SCENE
    // Clean dead handles from surface map.
    if (released) {
        for (auto it = surfaceMaterials_.begin(); it != surfaceMaterials_.end();) {
            if (it->second && !pbr::MaterialLibrary::get().valid(it->second)) it = surfaceMaterials_.erase(it);
            else ++it;
        }
    }
#endif
    return released;
}

#endif // AVER_MODULE_PBR

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
void GameContent::queueMeshMaterials(u64 meshId) {
    if (meshMaterialNames_.count(meshId)) return;
    std::vector<std::string>& names = meshMaterialNames_[meshId];
    const auto queue = [&](i32 token, const std::string& name) {
        queueSurfaceMaterial(token, name);
        if (std::find(names.begin(), names.end(), name) != names.end()) return;
        names.push_back(name);
        ++materialUse_[name].meshes;
    };
    if (const i32 slot0Token = meshDefaultMaterial(meshId)) {
        const std::string& name = meshSlot0Name(meshId);
        if (!name.empty()) queue(slot0Token, name);
    }
    if (const std::vector<MeshPart>* parts = partsFor(meshId)) {
        for (const MeshPart& part : *parts) {
            if (!part.material) continue;
            const char* partName = aver_scene_material_name(part.material);
            if (partName && *partName) queue(part.material, partName);
        }
    }
}

usize GameContent::prefetchMeshMaterials(const std::string& meshPath) {
    if (!asyncTextures_ || !textureFactory_ || meshPath.empty()) return 0;
    std::string rel = meshPath;
    for (char& c : rel) if (c == '\\') c = '/';
    const u64 meshId = fnv1a64(std::string_view(rel));
    usize queued = 0;
    const auto one = [&](const std::string& name) {
        if (!name.empty()) queued += prefetchMaterial(name);
    };
    one(meshSlot0Name(meshId));
    if (const std::vector<MeshPart>* parts = partsFor(meshId))
        for (const MeshPart& part : *parts) {
            const char* partName = part.material ? aver_scene_material_name(part.material) : nullptr;
            if (partName && *partName) one(partName);
        }
    // A mesh not uploaded yet names its slots only in its file: the mesh workers read it and queue their textures.
    if (lazyRel_.count(meshId) && !sceneMeshes_.count(meshId)) prefetchMesh(meshId, 0.0f);
    return queued;
}

pbr::MaterialHandle GameContent::authoredFor(i32 token) const {
    const auto it = surfaceMaterials_.find(token);
    return it == surfaceMaterials_.end() ? 0 : it->second;
}

pbr::MaterialHandle GameContent::authoredOrResolve(i32 token) {
    if (const pbr::MaterialHandle bound = authoredFor(token)) return bound;
    if (!token || unresolvedSurfaces_.count(token)) return 0;
    const char* name = aver_scene_material_name(token);
    const pbr::MaterialHandle h = name && *name ? materialForSurface(name) : 0;
    if (h) bindSurfaceMaterial(token, h);
    else unresolvedSurfaces_.insert(token);
    return h;
}
#endif

} // namespace aver::game
