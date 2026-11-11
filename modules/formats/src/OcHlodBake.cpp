#include "aver/formats/OcHlodBake.hpp"

#include "aver/core/Hash.hpp"
#include "aver/world/LevelTransform.hpp"   // header-only: the Euler convention world::instantiate uses
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <unordered_map>

namespace aver::fmt {

namespace {

struct Pose {
    f64 pos[3] = {0, 0, 0};
    Quat rot = Quat::identity();
    Vec3 scale{1, 1, 1};
};

void rotateD(const Quat& q, const f64 v[3], f64 out[3]) {
    const f64 qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    const f64 tx = 2.0 * (qy * v[2] - qz * v[1]), ty = 2.0 * (qz * v[0] - qx * v[2]), tz = 2.0 * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

Pose poseOf(const OcWorldPlacement& p) {
    Pose o;
    o.pos[0] = p.x; o.pos[1] = p.y; o.pos[2] = p.z;
    o.rot = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch), static_cast<f32>(p.yaw)});
    o.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
    return o;
}

Pose compose(const Pose& parent, const Pose& child) {
    Pose o;
    o.scale = Vec3{parent.scale.x * child.scale.x, parent.scale.y * child.scale.y, parent.scale.z * child.scale.z};
    o.rot = parent.rot * child.rot;
    const f64 c[3] = {child.pos[0] * parent.scale.x, child.pos[1] * parent.scale.y, child.pos[2] * parent.scale.z};
    f64 r[3];
    rotateD(parent.rot, c, r);
    for (int a = 0; a < 3; ++a) o.pos[a] = parent.pos[a] + r[a];
    return o;
}

void matrixOf(const Pose& t, f64 m[12]) { ocMergeMatrix(t.pos, t.rot, t.scale, m); }

std::string slashes(std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; }

const OcDataLayer* findLayer(const std::vector<OcDataLayer>& defs, const std::string& name) {
    for (const OcDataLayer& d : defs) if (d.name == name) return &d;
    return nullptr;
}

struct CachedMesh {
    OcMeshData mesh;
    bool ok = false;
    u64 hash = 0;
    u32 triangles = 0;
};

} // namespace

u64 hashHlodMeshContent(const OcMeshData& m) {
    u64 h = fnv1a64(m.positions.data(), m.positions.size() * sizeof(f32));
    h = h * 1099511628211ull ^ fnv1a64(m.indices.data(), m.indices.size() * sizeof(u32));
    if (!m.normals.empty()) h = h * 1099511628211ull ^ fnv1a64(m.normals.data(), m.normals.size() * sizeof(f32));
    if (!m.uvs.empty()) h = h * 1099511628211ull ^ fnv1a64(m.uvs.data(), m.uvs.size() * sizeof(f32));
    for (const OcMeshSubmesh& s : m.submeshes) {
        const std::string t = std::to_string(s.indexStart) + ":" + std::to_string(s.indexCount) + ":" + std::to_string(s.materialSlot);
        h = h * 1099511628211ull ^ fnv1a64(std::string_view(t));
    }
    for (const std::string& n : m.materialSlots) h = h * 1099511628211ull ^ fnv1a64(std::string_view(n));
    return h;
}

u32 hlodTargetTriangles(u32 sourceTriangles, f32 cellCm, f32 hlodCm, const OcHlodOptions& opt) {
    f64 target = static_cast<f64>(sourceTriangles);
    if (cellCm > 0.0f && hlodCm > 0.0f) {
        const f64 half = std::tan(static_cast<f64>(opt.fovYDeg) * 0.5 * 3.14159265358979323846 / 180.0);
        const f64 px = (static_cast<f64>(cellCm) / static_cast<f64>(hlodCm)) * (static_cast<f64>(opt.viewportPx) * 0.5 / (half > 1e-6 ? half : 1.0));
        target = std::min(target, px * px / 32.0);
    }
    const u32 t = static_cast<u32>(std::min<f64>(std::max(target, 0.0), 4294967000.0));
    return std::min(std::max(t, opt.minTriangles), std::max(opt.maxTriangles, opt.minTriangles));
}

bool hlodLayersAllowed(const std::vector<std::string>& layerNames, const std::vector<OcDataLayer>& defs) {
    for (const std::string& n : layerNames) {
        const OcDataLayer* d = findLayer(defs, n);
        for (usize hops = 0; d && hops <= defs.size(); ++hops) {
            if (d->runtime || d->initial != OcDataLayerState::Activated) return false;
            d = d->parent.empty() ? nullptr : findLayer(defs, d->parent);
        }
    }
    return true;
}

bool buildHlodProxy(const std::vector<OcMergeItem>& items, f64 originX, f64 originY, f32 cellCm, f32 hlodCm, const OcHlodOptions& opt,
                    OcHlodCellBuild& out, std::string* why) {
    OcMergeOptions mo;
    mo.origin[0] = originX; mo.origin[1] = originY; mo.origin[2] = 0.0;
    mo.dropSmallerCm = opt.dropSmallerCm;
    mo.dropEnclosed = opt.dropEnclosed;
    OcMergeReport mr;
    if (!mergeStaticMeshes(items, mo, out.mesh, &mr, why)) return false;
    out.trianglesIn = mr.trianglesOut;
    if (opt.maxErrorCm > 0.0f && opt.simplifyError) {
        OcMeshData reduced = out.mesh;
        if (opt.simplifyError(reduced, opt.maxErrorCm) && !reduced.indices.empty()) out.mesh = std::move(reduced);
        out.mesh.computeBounds();
        return true;
    }
    const u32 budget = hlodTargetTriangles(mr.trianglesOut, cellCm, hlodCm, opt);
    if (opt.simplify && mr.trianglesOut > budget) {
        OcMeshData reduced = out.mesh;
        if (opt.simplify(reduced, budget) && !reduced.indices.empty()) out.mesh = std::move(reduced);
    }
    out.mesh.computeBounds();
    return true;
}

void planHlodCells(const OcWorldData& w, f32 cellCm, OcHlodPlan& out) {
    out = OcHlodPlan{};
    const usize n = w.placements.size();
    out.matrices.assign(n * 12, 0.0);
    if (!(cellCm >= 1.0f)) return;
    std::vector<Pose> poses(n);
    for (usize i = 0; i < n; ++i) {
        const OcWorldPlacement& p = w.placements[i];
        Pose xf = poseOf(p);
        if (p.parent >= 0 && static_cast<usize>(p.parent) < i) xf = compose(poses[static_cast<usize>(p.parent)], xf);
        poses[i] = xf;
    }
    std::map<std::pair<i32, i32>, std::vector<u32>> byCell;
    for (usize i = 0; i < n; ++i) {
        const OcWorldPlacement& p = w.placements[i];
        if (p.asset.empty() || !p.className.empty() || !p.vehiclePreset.empty() || !p.animClip.empty() || !p.visible) continue;
        if (!hlodLayersAllowed(p.layers, w.dataLayers)) continue;
        const f64 wide = static_cast<f64>(cellCm) * static_cast<f64>(kOcHlodWideCells);
        if (p.hasBounds && (static_cast<f64>(p.boundsMax[0]) - p.boundsMin[0] > wide || static_cast<f64>(p.boundsMax[1]) - p.boundsMin[1] > wide))
            continue;
        matrixOf(poses[i], &out.matrices[i * 12]);
        f64 cx = poses[i].pos[0], cy = poses[i].pos[1];
        if (p.hasBounds) { cx = (static_cast<f64>(p.boundsMin[0]) + p.boundsMax[0]) * 0.5; cy = (static_cast<f64>(p.boundsMin[1]) + p.boundsMax[1]) * 0.5; }
        byCell[{static_cast<i32>(std::floor(cy / cellCm)), static_cast<i32>(std::floor(cx / cellCm))}].push_back(static_cast<u32>(i));
    }
    out.cells.reserve(byCell.size());
    for (auto& [key, list] : byCell) {
        OcHlodPlan::Cell c;
        c.cy = key.first;
        c.cx = key.second;
        c.placements = std::move(list);
        out.cells.push_back(std::move(c));
    }
}

bool bakeHlod(OcWorldData& w, const std::string& levelStem, const std::string& contentDir, const std::string& projectDir,
              const OcHlodOptions& opt, OcHlodReport& report, std::string* why) {
    report = OcHlodReport{};
    if (w.stream.dataPath.empty()) {
        if (why) *why = "the level has no streaming data (run the stream bake first)";
        return false;
    }
    const std::string dataFile = (std::filesystem::path(projectDir) / w.stream.dataPath).string();
    OcStreamData sd;
    if (!loadOcStream(dataFile, sd, why)) return false;

    const f32 cellCm = opt.cellCm > 0.0f ? opt.cellCm : w.stream.cellCm;
    const f32 hlodCm = opt.hlodCm > 0.0f ? opt.hlodCm : (w.stream.hlodCm > 0.0f ? w.stream.hlodCm : w.stream.loadCm * 3.0f);
    if (!(cellCm >= 1.0f)) { if (why) *why = "cell size must be at least 1 cm"; return false; }

    // World poses, parents first (they precede children), composed in f64.
    const usize n = w.placements.size();
    std::vector<Pose> poses(n);
    for (usize i = 0; i < n; ++i) {
        const OcWorldPlacement& p = w.placements[i];
        Pose xf = poseOf(p);
        if (p.parent >= 0 && static_cast<usize>(p.parent) < i) xf = compose(poses[static_cast<usize>(p.parent)], xf);
        poses[i] = xf;
    }

    std::unordered_map<std::string, CachedMesh> cache;
    const auto meshOf = [&](const std::string& asset) -> const CachedMesh& {
        if (const auto it = cache.find(asset); it != cache.end()) return it->second;
        CachedMesh c;
        if (opt.meshLoader) c.ok = opt.meshLoader(asset, c.mesh);
        else c.ok = loadOcMesh((std::filesystem::path(contentDir) / asset).string(), c.mesh);
        if (c.ok) { c.hash = hashHlodMeshContent(c.mesh); c.triangles = static_cast<u32>(c.mesh.indices.size() / 3); }
        return cache.emplace(asset, std::move(c)).first->second;
    };

    // Eligible placements by cell (bounds centre, as the streamer homes them), in placement order.
    std::map<std::pair<i32, i32>, std::vector<usize>> byCell;
    for (usize i = 0; i < n; ++i) {
        const OcWorldPlacement& p = w.placements[i];
        if (p.asset.empty() || !p.className.empty() || !p.vehiclePreset.empty() || !p.animClip.empty() || !p.visible) { ++report.placementsExcluded; continue; }
        if (!hlodLayersAllowed(p.layers, w.dataLayers)) { ++report.placementsExcluded; ++report.excludedByLayer; continue; }
        if (!meshOf(p.asset).ok) {
            ++report.placementsExcluded;
            if (report.warnings.size() < 16) report.warnings.push_back("no mesh for '" + p.asset + "'");
            continue;
        }
        f64 cx = poses[i].pos[0], cy = poses[i].pos[1];
        if (p.hasBounds) { cx = (static_cast<f64>(p.boundsMin[0]) + p.boundsMax[0]) * 0.5; cy = (static_cast<f64>(p.boundsMin[1]) + p.boundsMax[1]) * 0.5; }
        byCell[{static_cast<i32>(std::floor(cx / cellCm)), static_cast<i32>(std::floor(cy / cellCm))}].push_back(i);
    }
    report.cells = static_cast<u32>(byCell.size());

    const auto proxyPath = [&](i32 cx, i32 cy) {
        return slashes("Binaries/Streaming/" + levelStem + "/hlod/c_" + std::to_string(cx) + "_" + std::to_string(cy) + ".ocmesh");
    };
    const std::string params = "v" + std::to_string(kOcHlodBakeVersion) + "|" + std::to_string(cellCm) + "|" + std::to_string(hlodCm) + "|" +
                               std::to_string(opt.minTriangles) + "|" + std::to_string(opt.maxTriangles) + "|" + std::to_string(opt.viewportPx) + "|" +
                               std::to_string(opt.fovYDeg) + "|" + std::to_string(opt.dropSmallerCm) + "|" + (opt.dropEnclosed ? "e" : "-") + "|" +
                               (opt.simplify ? "s" : "-") + "|";

    std::vector<OcStreamHlod> next;   // level 1 entries after this run
    u32 done = 0;
    for (const auto& [cell, list] : byCell) {
        if (opt.cancel && opt.cancel()) { report.cancelled = true; break; }
        std::string key = params;
        std::vector<OcMergeItem> items;
        items.reserve(list.size());
        char buf[160];
        for (const usize i : list) {
            const OcWorldPlacement& p = w.placements[i];
            const CachedMesh& cm = meshOf(p.asset);
            OcMergeItem it;
            it.mesh = &cm.mesh;
            matrixOf(poses[i], it.m);
            it.material = p.material;
            std::snprintf(buf, sizeof buf, "%016" PRIx64 "|%016" PRIx64 "|", static_cast<uint64_t>(p.placementId), static_cast<uint64_t>(cm.hash));
            key += buf; key += p.asset; key += '|'; key += p.material; key += '|';
            for (int k = 0; k < 12; ++k) { std::snprintf(buf, sizeof buf, "%.3f,", it.m[k]); key += buf; }
            key += ';';
            items.push_back(std::move(it));
        }
        const u64 hash = fnv1a64(std::string_view(key));
        const std::string rel = proxyPath(cell.first, cell.second);
        const std::string abs = (std::filesystem::path(projectDir) / rel).string();

        OcStreamHlod entry{cell.first, cell.second, 1, rel, hash};
        const auto old = std::find_if(sd.hlod.begin(), sd.hlod.end(), [&](const OcStreamHlod& h) { return h.level == 1 && h.cx == cell.first && h.cy == cell.second; });
        if (!opt.force && old != sd.hlod.end() && old->sourceHash == hash && std::filesystem::exists(abs)) {
            ++report.fresh;
            next.push_back(*old);
        } else {
            OcHlodCellBuild b;
            std::string bwhy;
            createParentDirectories(abs);
            if (buildHlodProxy(items, static_cast<f64>(cell.first) * cellCm, static_cast<f64>(cell.second) * cellCm, cellCm, hlodCm, opt, b, &bwhy) &&
                saveOcMesh(abs, b.mesh, &bwhy)) {
                ++report.built;
                report.trianglesIn += b.trianglesIn;
                report.trianglesOut += static_cast<u32>(b.mesh.indices.size() / 3);
                report.placementsMerged += static_cast<u32>(list.size());
                next.push_back(entry);
            } else {
                ++report.failed;
                if (report.warnings.size() < 16) report.warnings.push_back("cell " + std::to_string(cell.first) + "," + std::to_string(cell.second) + ": " + bwhy);
                if (old != sd.hlod.end()) next.push_back(*old);   // keep the previous proxy rather than lose the cell
            }
        }
        if (opt.progress) opt.progress(++done, report.cells);
    }

    if (!report.cancelled) {
        // Proxies of cells that no longer have eligible content go away with their files.
        for (const OcStreamHlod& h : sd.hlod) {
            if (h.level != 1) continue;
            const bool kept = std::any_of(next.begin(), next.end(), [&](const OcStreamHlod& k) { return k.cx == h.cx && k.cy == h.cy; });
            if (kept) continue;
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(projectDir) / h.proxy, ec);
            ++report.removed;
        }
    } else {
        for (const OcStreamHlod& h : sd.hlod)   // a cancelled run changes only what it finished
            if (h.level == 1 && std::none_of(next.begin(), next.end(), [&](const OcStreamHlod& k) { return k.cx == h.cx && k.cy == h.cy; })) next.push_back(h);
    }
    std::vector<OcStreamHlod> merged;
    for (const OcStreamHlod& h : sd.hlod) if (h.level != 1) merged.push_back(h);   // higher levels are another bake's
    merged.insert(merged.end(), next.begin(), next.end());
    std::sort(merged.begin(), merged.end(), [](const OcStreamHlod& a, const OcStreamHlod& b) {
        return a.level != b.level ? a.level < b.level : a.cy != b.cy ? a.cy < b.cy : a.cx < b.cx;
    });
    sd.hlod = std::move(merged);
    if (!saveOcStream(dataFile, sd, why)) return false;
    if (w.stream.hlodCm <= 0.0f) w.stream.hlodCm = hlodCm;
    return true;
}

} // namespace aver::fmt
