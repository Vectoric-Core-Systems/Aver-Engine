#include "GameContentInternal.hpp"

namespace aver::game {

#if AVER_MODULE_SCENE

namespace content_detail {

// ---- collisionMeshFor's building blocks ------------------------------------------------------------

// Compacts indices down to only the vertices they reference, dropping degenerate triangles.
bool compactTriangles(const f32* srcPositions, u32 srcVertexCount,
                      const u32* srcIndices, usize srcIndexCount,
                      std::vector<f32>& outPositions, std::vector<u32>& outIndices) {
    outPositions.clear();
    outIndices.clear();
    std::unordered_map<u32, u32> remap;
    remap.reserve(srcIndexCount);
    outIndices.reserve(srcIndexCount);
    for (usize k = 0; k + 2 < srcIndexCount; k += 3) {
        const u32 ia = srcIndices[k], ib = srcIndices[k + 1], ic = srcIndices[k + 2];
        if (ia >= srcVertexCount || ib >= srcVertexCount || ic >= srcVertexCount) continue;
        if (ia == ib || ib == ic || ia == ic) continue;
        for (const u32 orig : {ia, ib, ic}) {
            const auto [it, inserted] = remap.try_emplace(orig, static_cast<u32>(outPositions.size() / 3));
            if (inserted) {
                outPositions.push_back(srcPositions[usize(orig) * 3 + 0]);
                outPositions.push_back(srcPositions[usize(orig) * 3 + 1]);
                outPositions.push_back(srcPositions[usize(orig) * 3 + 2]);
            }
            outIndices.push_back(it->second);
        }
    }
    return outIndices.size() >= 3;
}

// Vertex position's exact bit pattern for welding by position, not vertex index.
struct WeldKey {
    u32 xb, yb, zb;
    bool operator==(const WeldKey& o) const { return xb == o.xb && yb == o.yb && zb == o.zb; }
};
struct WeldKeyHash {
    usize operator()(const WeldKey& k) const {
        u64 h = kFnv1a64OffsetBasis;
        h = (h ^ k.xb) * kFnv1a64Prime;
        h = (h ^ k.yb) * kFnv1a64Prime;
        h = (h ^ k.zb) * kFnv1a64Prime;
        return static_cast<usize>(h);
    }
};

// Welds positions/indices (already compacted) by exact position.
// meshopt_simplify preserves topological borders; welding first lets it see the real topology.
void weldByPosition(const std::vector<f32>& positions, const std::vector<u32>& indices,
                    std::vector<f32>& outPositions, std::vector<u32>& outIndices) {
    const usize vertexCount = positions.size() / 3;
    std::unordered_map<WeldKey, u32, WeldKeyHash> weld;
    weld.reserve(vertexCount);
    std::vector<u32> remap(vertexCount);
    for (usize v = 0; v < vertexCount; ++v) {
        // Normalise ±0 to +0 (different IEEE 754 bit patterns).
        f32 x = positions[v * 3 + 0], y = positions[v * 3 + 1], z = positions[v * 3 + 2];
        if (x == 0.0f) x = 0.0f;
        if (y == 0.0f) y = 0.0f;
        if (z == 0.0f) z = 0.0f;
        WeldKey key;
        std::memcpy(&key.xb, &x, sizeof(u32));
        std::memcpy(&key.yb, &y, sizeof(u32));
        std::memcpy(&key.zb, &z, sizeof(u32));
        const auto [it, inserted] = weld.try_emplace(key, static_cast<u32>(outPositions.size() / 3));
        if (inserted) { outPositions.push_back(x); outPositions.push_back(y); outPositions.push_back(z); }
        remap[v] = it->second;
    }
    outIndices.resize(indices.size());
    for (usize k = 0; k < indices.size(); ++k) outIndices[k] = remap[indices[k]];
}

// Simplifies welded positions/indices toward ~2 cm world error, capped at 65536 triangles.
std::vector<u32> simplifyCollisionMesh(const std::vector<f32>& positions, const std::vector<u32>& indices,
                                       const std::string& meshPathForLog, f32& outErrorCm) {
    // ERRORABSOLUTE: error in the SAME units as positions (centimetres).
    constexpr f32 kTargetErrorCm = 2.0f;
    constexpr usize kTriangleCeiling = 65536;
    // LOCKBORDER: preserve genuine open edges and ones from material-slot filtering.
    constexpr unsigned kOptions =
        static_cast<unsigned>(meshopt_SimplifyLockBorder) | static_cast<unsigned>(meshopt_SimplifyErrorAbsolute);

    const usize targetIndexCount = std::min(indices.size(), kTriangleCeiling * 3);
    std::vector<u32> dest(indices.size());
    f32 resultError = 0.0f;
    usize resultCount = meshopt_simplify(dest.data(), indices.data(), indices.size(), positions.data(),
                                         positions.size() / 3, 3 * sizeof(f32), targetIndexCount,
                                         kTargetErrorCm, kOptions, &resultError);

    if (resultCount / 3 > kTriangleCeiling) {
        // 2 cm not enough; ceiling is the hard limit. Error is allowed to grow.
        resultCount = meshopt_simplify(dest.data(), indices.data(), indices.size(), positions.data(),
                                       positions.size() / 3, 3 * sizeof(f32), targetIndexCount,
                                       std::numeric_limits<f32>::max(), kOptions, &resultError);
        AVER_WARN("[Collision] {}: {} triangles would not fit the {}-triangle ceiling within {:.1f} cm "
                  "error; used {:.1f} cm instead", meshPathForLog, indices.size() / 3, kTriangleCeiling,
                  kTargetErrorCm, resultError);
    }

    dest.resize(resultCount);
    outErrorCm = resultError;
    return dest;
}

// ---- the disk cache: <project>/Saved/DerivedDataCache/Collision/<hash>.occol -------------------------
// Derived data (not authored). Whole directory can be deleted for cost of one rebuild per mesh.
// Bump kCollisionCacheVersion when filter rule, simplification target or layout changes.
// 2: the physical-surface palette and the per-slot signature follow the original fields (a version-1 file is a miss).
constexpr u32 kCollisionCacheMagic   = 0x4C4F4341u;
constexpr u32 kCollisionCacheVersion = 2;


std::string collisionCacheDir(const std::string& projectDir) {
    return projectDir.empty() ? std::string() : projectDir + AVER_PATH_SEP "Saved" AVER_PATH_SEP "DerivedDataCache" AVER_PATH_SEP "Collision";
}

// "<hash>.occol", FNV-1a hash of the key (mesh path, size, mtime, version).
std::string collisionCachePathFor(const std::string& projectDir, const std::string& meshPath,
                                  u64 fileSize, i64 mtimeTicks) {
    const std::string dir = collisionCacheDir(projectDir);
    if (dir.empty()) return std::string();
    const std::string key = meshPath + "|" + std::to_string(fileSize) + "|" +
                            std::to_string(mtimeTicks) + "|" + std::to_string(kCollisionCacheVersion);
    const u64 h = fnv1a64(std::string_view(key));
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.occol", static_cast<unsigned long long>(h));
    return dir + AVER_PATH_SEP + name;
}

// The physics shape cache's source key for a collision mesh: what the .occol key covers (path, size, mtime, version) plus what each
// material slot said (collides, surface), because those answers live in .ocmat files the file key does not cover and a slot flip that
// keeps the triangle count would otherwise reuse a stale BVH. world::createMeshShapeFromSource adds its own version and the counts.
u64 shapeSourceKey(const std::string& meshPath, u64 fileSize, i64 mtimeTicks, const GameContent::CollisionMesh& m) {
    std::string k = meshPath + "|" + std::to_string(fileSize) + "|" + std::to_string(mtimeTicks) + "|" +
                    std::to_string(kCollisionCacheVersion);
    for (const GameContent::CollisionMesh::SlotSig& s : m.slotSig)
        k += "|" + s.slot + (s.collides ? "+" : "-") + std::to_string(s.surface);
    return fnv1a64(std::string_view(k));
}
// Reads and validates cache entry against source mesh's current path/size/mtime.
bool readCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                            i64 mtimeTicks, GameContent::CollisionMesh& out, const SlotQuery& slots) {
    std::ifstream in(aver::nativePath(cachePath), std::ios::binary);
    if (!in) return false;
    const auto get = [&in](void* p, usize n) { in.read(static_cast<char*>(p), static_cast<std::streamsize>(n)); return static_cast<bool>(in); };

    u32 magic = 0, version = 0;
    if (!get(&magic, sizeof(magic)) || magic != kCollisionCacheMagic) return false;
    if (!get(&version, sizeof(version)) || version != kCollisionCacheVersion) return false;

    u64 pathLen = 0;
    if (!get(&pathLen, sizeof(pathLen)) || pathLen == 0 || pathLen > (1ull << 20)) return false;
    std::string storedPath(static_cast<usize>(pathLen), '\0');
    if (!get(storedPath.data(), static_cast<usize>(pathLen)) || storedPath != meshPath) return false;

    u64 storedSize = 0;
    i64 storedMtime = 0;
    if (!get(&storedSize, sizeof(storedSize)) || storedSize != fileSize) return false;
    if (!get(&storedMtime, sizeof(storedMtime)) || storedMtime != mtimeTicks) return false;

    u32 vertexCount = 0, indexCount = 0;
    if (!get(&vertexCount, sizeof(vertexCount)) || !get(&indexCount, sizeof(indexCount))) return false;
    // Sanity check: a corrupt count is not a huge allocation.
    constexpr u32 kMaxReasonableVertices = 64u * 1024u * 1024u;
    if (vertexCount == 0 || vertexCount > kMaxReasonableVertices ||
        indexCount < 3 || indexCount > kMaxReasonableVertices * 3u) return false;

    out.positions.resize(usize(vertexCount) * 3);
    if (!get(out.positions.data(), out.positions.size() * sizeof(f32))) return false;
    out.indices.resize(indexCount);
    if (!get(out.indices.data(), out.indices.size() * sizeof(u32))) return false;
    if (!get(&out.lod, sizeof(out.lod)) || !get(&out.errorCm, sizeof(out.errorCm))) return false;

    // The physical-surface palette (empty for a mesh that names none) and one palette index per triangle.
    u32 paletteCount = 0, triCount = 0;
    if (!get(&paletteCount, sizeof(paletteCount)) || paletteCount > 32) return false;
    out.palette.resize(paletteCount);
    if (paletteCount && !get(out.palette.data(), usize(paletteCount) * sizeof(i32))) return false;
    if (!get(&triCount, sizeof(triCount))) return false;
    if ((triCount == 0) != (paletteCount == 0) || (triCount != 0 && triCount != indexCount / 3)) return false;
    out.triPalette.resize(triCount);
    if (triCount && !get(out.triPalette.data(), triCount)) return false;
    for (const u8 index : out.triPalette)
        if (index >= paletteCount) return false;   // corrupt

    // What the build saw of each material slot: the entry is stale if a slot's .ocmat says something else now.
    u32 sigCount = 0;
    if (!get(&sigCount, sizeof(sigCount)) || sigCount > 4096) return false;
    out.slotSig.resize(sigCount);
    for (GameContent::CollisionMesh::SlotSig& sig : out.slotSig) {
        u32 len = 0;
        u8 collides = 1;
        if (!get(&len, sizeof(len)) || len > 4096) return false;
        sig.slot.resize(len);
        if (len && !get(sig.slot.data(), len)) return false;
        if (!get(&collides, sizeof(collides)) || !get(&sig.surface, sizeof(sig.surface))) return false;
        sig.collides = collides != 0;
    }
    for (const GameContent::CollisionMesh::SlotSig& sig : out.slotSig) {
        if (slots.collides && slots.collides(sig.slot) != sig.collides) return false;
        if (slots.surface && slots.surface(sig.slot) != sig.surface) return false;
    }
    return true;
}

// Writes cache entry atomically: record goes to "<cachePath>.tmp" first; successful rename publishes it.
bool writeCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                             i64 mtimeTicks, const GameContent::CollisionMesh& mesh, std::string* why) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(cachePath).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const std::string tmpPath = cachePath + ".tmp";
    {
        std::ofstream out(aver::nativePath(tmpPath), std::ios::binary | std::ios::trunc);
        if (!out) { if (why) *why = "could not open " + tmpPath; return false; }
        const auto put = [&out](const void* p, usize n) { out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); };
        put(&kCollisionCacheMagic, sizeof(kCollisionCacheMagic));
        put(&kCollisionCacheVersion, sizeof(kCollisionCacheVersion));
        const u64 pathLen = meshPath.size();
        put(&pathLen, sizeof(pathLen));
        put(meshPath.data(), meshPath.size());
        put(&fileSize, sizeof(fileSize));
        put(&mtimeTicks, sizeof(mtimeTicks));
        const u32 vertexCount = static_cast<u32>(mesh.positions.size() / 3);
        const u32 indexCount = static_cast<u32>(mesh.indices.size());
        put(&vertexCount, sizeof(vertexCount));
        put(&indexCount, sizeof(indexCount));
        put(mesh.positions.data(), mesh.positions.size() * sizeof(f32));
        put(mesh.indices.data(), mesh.indices.size() * sizeof(u32));
        put(&mesh.lod, sizeof(mesh.lod));
        put(&mesh.errorCm, sizeof(mesh.errorCm));
        const u32 paletteCount = static_cast<u32>(mesh.palette.size());
        const u32 triCount = static_cast<u32>(mesh.triPalette.size());
        put(&paletteCount, sizeof(paletteCount));
        put(mesh.palette.data(), usize(paletteCount) * sizeof(i32));
        put(&triCount, sizeof(triCount));
        put(mesh.triPalette.data(), triCount);
        const u32 sigCount = static_cast<u32>(mesh.slotSig.size());
        put(&sigCount, sizeof(sigCount));
        for (const GameContent::CollisionMesh::SlotSig& sig : mesh.slotSig) {
            const u32 len = static_cast<u32>(sig.slot.size());
            const u8 collides = sig.collides ? 1 : 0;
            put(&len, sizeof(len));
            put(sig.slot.data(), len);
            put(&collides, sizeof(collides));
            put(&sig.surface, sizeof(sig.surface));
        }
        if (!out) { if (why) *why = "write failed for " + tmpPath; return false; }
    }
    std::filesystem::rename(tmpPath, cachePath, ec);
    if (ec) { if (why) *why = "could not publish " + cachePath + ": " + ec.message(); return false; }
    return true;
}

// One kept run of a mesh's index buffer and the physical surface its material slot names (0 = none).
struct SurfaceRun {
    i32 surface = 0;
    usize begin = 0, end = 0;   // [begin, end) into the mesh's LOD 0 indices
};

// The surface-carrying build (see buildCollisionMesh): triangles grouped by surface, each group welded and simplified on its
// own with its borders locked, the groups joined and welded once more across their seams.
std::unique_ptr<GameContent::CollisionMesh> buildSurfaceCollisionMesh(const fmt::OcMeshData& md, const std::string& path,
                                                                     const std::vector<SurfaceRun>& runs) {
    using CollisionMesh = GameContent::CollisionMesh;
    constexpr usize kMaxPalette = 32;   // Jolt packs a mesh triangle's material index in 5 bits

    // The distinct surfaces in first-appearance order. Past the 31st, a surface joins surface 0 so the palette stays at 32.
    std::vector<i32> distinct;
    for (const SurfaceRun& r : runs)
        if (std::find(distinct.begin(), distinct.end(), r.surface) == distinct.end()) distinct.push_back(r.surface);
    const bool overflow = distinct.size() > kMaxPalette;
    if (overflow)
        AVER_WARN("[Collision] {}: {} distinct physical surfaces, but a mesh holds {}; the rest are treated as no surface",
                  path, distinct.size(), kMaxPalette);
    const auto effective = [&](i32 surface) -> i32 {
        if (!overflow) return surface;
        const auto rank = std::find(distinct.begin(), distinct.end(), surface) - distinct.begin();
        return rank < static_cast<std::ptrdiff_t>(kMaxPalette - 1) ? surface : 0;
    };

    struct Group { i32 surface = 0; std::vector<u32> indices; };
    std::vector<Group> groups;
    for (const SurfaceRun& r : runs) {
        const i32 s = effective(r.surface);
        auto it = std::find_if(groups.begin(), groups.end(), [s](const Group& g) { return g.surface == s; });
        if (it == groups.end()) { groups.push_back(Group{s, {}}); it = groups.end() - 1; }
        it->indices.insert(it->indices.end(), md.indices.begin() + static_cast<std::ptrdiff_t>(r.begin),
                           md.indices.begin() + static_cast<std::ptrdiff_t>(r.end));
    }

    std::vector<f32> allPositions;
    std::vector<u32> allIndices;
    std::vector<u8> triPalette;
    std::vector<i32> palette;
    f32 worstErrorCm = 0.0f;
    for (const Group& g : groups) {
        std::vector<f32> compactPositions;
        std::vector<u32> compactIndices;
        if (!compactTriangles(md.positions.data(), md.vertexCount(), g.indices.data(), g.indices.size(),
                              compactPositions, compactIndices))
            continue;   // every triangle of this surface degenerate or out of range
        std::vector<f32> weldedPositions;
        std::vector<u32> weldedIndices;
        weldByPosition(compactPositions, compactIndices, weldedPositions, weldedIndices);

        f32 errorCm = 0.0f;
        const std::vector<u32> simplified = simplifyCollisionMesh(weldedPositions, weldedIndices, path, errorCm);
        std::vector<f32> groupPositions;
        std::vector<u32> groupIndices;
        if (!compactTriangles(weldedPositions.data(), static_cast<u32>(weldedPositions.size() / 3),
                              simplified.data(), simplified.size(), groupPositions, groupIndices)) {
            groupPositions = std::move(weldedPositions);   // simplification left nothing: keep the group as it was
            groupIndices = std::move(weldedIndices);
            errorCm = 0.0f;
        }

        const u8 paletteIndex = static_cast<u8>(palette.size());
        palette.push_back(g.surface);
        const u32 base = static_cast<u32>(allPositions.size() / 3);
        allPositions.insert(allPositions.end(), groupPositions.begin(), groupPositions.end());
        for (const u32 k : groupIndices) allIndices.push_back(base + k);
        triPalette.insert(triPalette.end(), groupIndices.size() / 3, paletteIndex);
        worstErrorCm = std::max(worstErrorCm, errorCm);
    }
    if (allIndices.size() < 3) return nullptr;

    // The vertices along a seam are separate copies after the per-group passes, at identical positions: one weld joins them.
    auto mesh = std::make_unique<CollisionMesh>();
    weldByPosition(allPositions, allIndices, mesh->positions, mesh->indices);
    mesh->lod = 0;
    mesh->errorCm = worstErrorCm;
    mesh->palette = std::move(palette);
    mesh->triPalette = std::move(triPalette);

    AVER_INFO("[Collision] {}: meshoptimizer, {} surface group(s) ({} tris from {} at LOD 0, {:.1f} cm error)", path,
              mesh->palette.size(), mesh->indices.size() / 3, md.indices.size() / 3, mesh->errorCm);
    return mesh;
}

// What the build saw of each (distinct, named) material slot, for the disk cache's check against the .ocmat files.
void fillSlotSig(GameContent::CollisionMesh& mesh, const fmt::OcMeshData& md, const SlotQuery& slots) {
    for (const std::string& name : md.materialSlots) {
        if (name.empty()) continue;
        const bool seen = std::any_of(mesh.slotSig.begin(), mesh.slotSig.end(),
                                      [&](const GameContent::CollisionMesh::SlotSig& s) { return s.slot == name; });
        if (seen) continue;
        GameContent::CollisionMesh::SlotSig sig;
        sig.slot = name;
        sig.collides = !slots.collides || slots.collides(name);
        sig.surface = slots.surface ? slots.surface(name) : 0;
        mesh.slotSig.push_back(std::move(sig));
    }
}

// Collision geometry for a parsed mesh: slots `collides` rejects are dropped, then the cheaper of a
// meshoptimizer simplification and a Trifactor LOD is kept. Touches no GameContent state (prefetch
// workers run it).
//
// A mesh whose kept slots NAME PHYSICAL SURFACES takes buildSurfaceCollisionMesh instead (groups per surface, Trifactor not
// used: its coarser levels know nothing of material slots). A mesh that names none is built exactly as it was before
// surfaces existed.
std::unique_ptr<GameContent::CollisionMesh> buildCollisionMesh(const fmt::OcMeshData& md, const std::string& path,
                                                              const SlotQuery& slots) {
    using CollisionMesh = GameContent::CollisionMesh;
    const u32 vertexCount = md.vertexCount();
    if (vertexCount == 0 || md.indices.size() < 3) return nullptr;

    // Drop cut-out/translucent material slots from LOD 0 submeshes.
    bool anyDropped = false, anySurface = false;
    std::vector<u32> keptIndices;
    std::vector<SurfaceRun> runs;
    if (md.submeshes.empty()) {
        keptIndices = md.indices;   // no submesh table: keep all
    } else {
        keptIndices.reserve(md.indices.size());
        u32 droppedSlots = 0;
        for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
            if (sm.indexCount == 0) continue;
            const usize end = usize(sm.indexStart) + sm.indexCount;
            if (end > md.indices.size()) continue;   // malformed range
            const std::string slotName =
                sm.materialSlot < md.materialSlots.size() ? md.materialSlots[sm.materialSlot] : std::string();
            if (slots.collides && !slots.collides(slotName)) { anyDropped = true; ++droppedSlots; continue; }
            const i32 surface = slots.surface ? slots.surface(slotName) : 0;
            if (surface != 0) anySurface = true;
            runs.push_back(SurfaceRun{surface, usize(sm.indexStart), end});
            keptIndices.insert(keptIndices.end(),
                               md.indices.begin() + static_cast<std::ptrdiff_t>(sm.indexStart),
                               md.indices.begin() + static_cast<std::ptrdiff_t>(end));
        }
        if (anyDropped)
            AVER_INFO("[Collision] {}: {} of {} material slot(s) are alpha-masked or translucent; "
                      "excluded from collision", path, droppedSlots, md.submeshes.size());
    }
    if (keptIndices.empty()) {
        // Logged once per mesh id (cache prevents reruns).
        AVER_WARN("[Collision] {}: every material slot is alpha-masked or translucent; no collision "
                  "geometry", path);
        return nullptr;
    }

    if (anySurface) {
        auto surfaced = buildSurfaceCollisionMesh(md, path, runs);
        if (surfaced) fillSlotSig(*surfaced, md, slots);
        return surfaced;
    }

    // Compact: keep only referenced vertices, remove degenerate triangles.
    std::vector<f32> filteredPositions;
    std::vector<u32> filteredIndices;
    if (!compactTriangles(md.positions.data(), vertexCount, keptIndices.data(), keptIndices.size(),
                          filteredPositions, filteredIndices))
        return nullptr;   // all kept triangles degenerate or out of range

    // Weld by position, simplify with meshoptimizer toward ~2 cm.
    std::vector<f32> weldedPositions;
    std::vector<u32> weldedIndices;
    weldByPosition(filteredPositions, filteredIndices, weldedPositions, weldedIndices);

    f32 meshoptErrorCm = 0.0f;
    const std::vector<u32> simplified =
        simplifyCollisionMesh(weldedPositions, weldedIndices, path, meshoptErrorCm);
    std::vector<f32> meshoptPositions;
    std::vector<u32> meshoptIndices;
    const bool meshoptOk = compactTriangles(weldedPositions.data(),
                                            static_cast<u32>(weldedPositions.size() / 3),
                                            simplified.data(), simplified.size(),
                                            meshoptPositions, meshoptIndices);

    // Trifactor coarsest LOD only when nothing was filtered (no submesh table to filter through).
    u32 trifactorLod = 0;
    f32 trifactorErrorCm = 0.0f;
    std::vector<f32> trifactorPositions;
    std::vector<u32> trifactorIndices;
    bool haveTrifactor = false;
#if AVER_MODULE_TRIFACTOR
    if (!anyDropped) {
        constexpr f32 kCollisionMaxErrorCm = 2.0f;
        for (u32 lvl = 1; lvl < md.lodCount(); ++lvl) {
            const f32 err = trifactor::levelWorldErrorCm(md, lvl);
            if (err <= kCollisionMaxErrorCm) { trifactorLod = lvl; trifactorErrorCm = err; }
        }
        const std::vector<u32>& lodSrc =
            trifactorLod == 0 ? md.indices : md.coarserLods[trifactorLod - 1].indices;
        haveTrifactor = compactTriangles(md.positions.data(), vertexCount, lodSrc.data(), lodSrc.size(),
                                         trifactorPositions, trifactorIndices);
    }
#endif

    // Fewer triangles wins; a tie keeps Trifactor (no further cost).
    auto mesh = std::make_unique<CollisionMesh>();
    const bool useTrifactor =
        haveTrifactor && (!meshoptOk || trifactorIndices.size() <= meshoptIndices.size());
    const char* wonBy = "meshoptimizer";
    if (useTrifactor) {
        mesh->positions = std::move(trifactorPositions);
        mesh->indices = std::move(trifactorIndices);
        mesh->lod = trifactorLod;
        mesh->errorCm = trifactorErrorCm;
        wonBy = "Trifactor LOD";
    } else if (meshoptOk) {
        mesh->positions = std::move(meshoptPositions);
        mesh->indices = std::move(meshoptIndices);
        mesh->lod = 0;
        mesh->errorCm = meshoptErrorCm;
    } else {
        // Fallback: neither simplifier produced output; use filtered set before simplification.
        mesh->positions = std::move(filteredPositions);
        mesh->indices = std::move(filteredIndices);
        mesh->lod = 0;
        mesh->errorCm = 0.0f;
        wonBy = "unsimplified";
    }
    if (mesh->indices.size() < 3) return nullptr;

    fillSlotSig(*mesh, md, slots);
    AVER_INFO("[Collision] {}: {} ({} tris from {} at LOD 0, {:.1f} cm error)", path, wonBy,
              mesh->indices.size() / 3, md.indices.size() / 3, mesh->errorCm);
    return mesh;
}

#  if AVER_MODULE_PBR
// collisionSlotCollides without the material library: the same file lookup materialForSurface makes,
// parsed directly (thread-free).
bool slotCollidesFromFiles(const std::string& binariesDir, const std::string& contentDir, const std::string& name) {
    if (name.empty() || contentDir.empty()) return true;
    const std::string candidates[3] = {
        binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP + name,
    };
    for (const std::string& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        pbr::MaterialDesc d;
        fmt::OcMatExtras extras;
        std::string err;
        if (!fmt::loadOcmatAny(path, contentDir, d, &extras, nullptr, &err)) return true;
        return d.alphaMode != pbr::AlphaMode::Mask && d.alphaMode != pbr::AlphaMode::Blend;
    }
    return true;
}

// The physical surface a material slot carries: its .ocmat PHYSSURFACE hashed to the id the physics registry and a raycast
// use, or 0 for a slot with no material file or no such record. The same file lookup as slotCollidesFromFiles (thread-free).
i32 slotSurfaceFromFiles(const std::string& binariesDir, const std::string& contentDir, const std::string& name) {
    if (name.empty() || contentDir.empty()) return 0;
    const std::string candidates[3] = {
        binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP + name,
    };
    for (const std::string& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        pbr::MaterialDesc d;
        fmt::OcMatExtras extras;
        std::string err;
        if (!fmt::loadOcmatAny(path, contentDir, d, &extras, nullptr, &err)) return 0;
        return aver::surfaceIdFromName(extras.physSurface);
    }
    return 0;
}

// The texture files a surface material names, as resolveMaterialTexture would load them: paths under
// the content dir (content-index ids and terrain layers are left to the main thread).
std::vector<std::pair<std::string, assets::TextureUsage>> slotTexturesFromFiles(
        const std::string& binariesDir, const std::string& contentDir, const std::string& name) {
    std::vector<std::pair<std::string, assets::TextureUsage>> out;
    if (name.empty() || contentDir.empty()) return out;
    const std::string candidates[3] = {
        binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP + name,
    };
    for (const std::string& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        pbr::MaterialDesc d;
        fmt::OcMatExtras extras;
        std::string err;
        if (!fmt::loadOcmatAny(path, contentDir, d, &extras, nullptr, &err) || d.terrain.enabled()) return out;
        for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
            const std::string& p = d.textures[i].path;
            if (p.empty() || p.rfind("@rt:", 0) == 0) continue;
            const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
            std::string full = absolute ? p : contentDir + AVER_PATH_SEP + p;
            if (!absolute && !std::filesystem::exists(full, ec)) continue;
            out.emplace_back(std::move(full), textureUsageFor(static_cast<pbr::TextureSlot>(i)));
        }
        return out;
    }
    return out;
}
#  endif

} // namespace content_detail

#if AVER_MODULE_PBR
// Material slot collision filter.
bool GameContent::collisionSlotCollides(const std::string& slotName) {
    if (slotName.empty()) return true;
    // A loaded material's desc, else its file: asking would load (and keep) a material nothing draws.
    const auto it = materialAssets_.find(slotName);
    if (it == materialAssets_.end()) return slotCollidesFromFiles(project_.binariesDir(), project_.contentDir(), slotName);
    const pbr::MaterialDesc* d = it->second ? pbr::MaterialLibrary::get().desc(it->second) : nullptr;
    if (!d) return true;   // no authored .ocmat: cannot resolve
    return d->alphaMode != pbr::AlphaMode::Mask && d->alphaMode != pbr::AlphaMode::Blend;
}

i32 GameContent::collisionSlotSurface(const std::string& slotName) {
    // The file, always: the loaded material desc carries no PHYSSURFACE (it is not a rendering property).
    return slotSurfaceFromFiles(project_.binariesDir(), project_.contentDir(), slotName);
}
#else
// Every slot collides (no PBR module), and none names a surface.
bool GameContent::collisionSlotCollides(const std::string&) { return true; }
i32 GameContent::collisionSlotSurface(const std::string&) { return 0; }
#endif

// Filters and simplifies collision geometry from meshes with multiple per-material submeshes.
const GameContent::CollisionMesh* GameContent::collisionMeshFor(u64 id) {
    if (const auto it = collisionMeshCache_.find(id); it != collisionMeshCache_.end())
        return it->second.get();

    // Cache null entries so failed lookups are O(1) on retry.
    std::unique_ptr<CollisionMesh>& slot = collisionMeshCache_[id];

    const std::string path = pathFor(id);
    if (path.empty()) return nullptr;   // built-in or unrecognized id

    // Disk cache: check source file size and mtime.
    std::error_code statEc;
    const u64 srcSize = static_cast<u64>(std::filesystem::file_size(path, statEc));
    const i64 srcMtime = statEc ? 0 : static_cast<i64>(
        std::filesystem::last_write_time(path, statEc).time_since_epoch().count());
    const bool canCache = !statEc && !project_.dir.empty();
    const std::string cachePath = canCache ? collisionCachePathFor(project_.dir, path, srcSize, srcMtime)
                                           : std::string();
    SlotQuery slots;
    slots.collides = [this](const std::string& slotName) { return collisionSlotCollides(slotName); };
    slots.surface = [this](const std::string& slotName) { return collisionSlotSurface(slotName); };
    if (!cachePath.empty()) {
        auto cached = std::make_unique<CollisionMesh>();
        if (readCollisionCacheFile(cachePath, path, srcSize, srcMtime, *cached, slots)) {
            ++collisionCacheHits_;
            AVER_INFO("[Collision] {}: {} tris from the disk cache", path, cached->indices.size() / 3);
            cached->shapeSource = shapeSourceKey(path, srcSize, srcMtime, *cached);
            slot = std::move(cached);
            return slot.get();
        }
    }

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path, md, &why)) {
        AVER_WARN("[Collision] {}", why);
        return nullptr;
    }
    auto mesh = buildCollisionMesh(md, path, slots);
    if (!mesh) return nullptr;

    if (!cachePath.empty()) {
        std::string saveWhy;
        if (!writeCollisionCacheFile(cachePath, path, srcSize, srcMtime, *mesh, &saveWhy))
            AVER_WARN("[Collision] {}: could not write the disk cache: {}", path, saveWhy);
    }

    if (canCache) mesh->shapeSource = shapeSourceKey(path, srcSize, srcMtime, *mesh);
    slot = std::move(mesh);
    return slot.get();
}

#endif // AVER_MODULE_SCENE

} // namespace aver::game
