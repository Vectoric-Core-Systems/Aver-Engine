// Generating a level's streaming data (docs/LEVEL_STREAMING.md): AverAssetC stream and the editor's
// World Settings > Regenerate Streaming Data both come here.
#include "aver/formats/OcStream.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/world/LevelTransform.hpp"   // header-only: the Euler convention world::instantiate uses

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace aver::fmt {

namespace {

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// The first folder of a content-relative asset; empty for a root file or the shared Meshes folder
// (built-ins and content other levels use stay eager).
std::string lazyDirOf(const std::string& asset) {
    const std::string a = slashes(asset);
    const usize s = a.find('/');
    if (s == std::string::npos || s == 0) return {};
    std::string d = a.substr(0, s);
    std::string lower = d;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == "meshes" ? std::string() : d;
}

Transform localOf(const OcWorldPlacement& p) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
    xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch),
                                               static_cast<f32>(p.yaw)});
    xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
    return xf;
}

std::string streamDir(const std::string& levelStem) { return "Binaries/Streaming/" + levelStem; }

} // namespace

usize assignPlacementIds(OcWorldData& w) {
    std::unordered_set<u64> used;
    for (const OcWorldPlacement& p : w.placements) if (p.placementId) used.insert(p.placementId);
    usize made = 0;
    for (usize i = 0; i < w.placements.size(); ++i) {
        OcWorldPlacement& p = w.placements[i];
        if (p.placementId) continue;
        const std::string key = p.asset + "|" + std::to_string(i) + "|" + std::to_string(p.x) + "," +
                                std::to_string(p.y) + "," + std::to_string(p.z);
        u64 id = fnv1a64(std::string_view(key));
        while (id == 0 || used.count(id)) id = fnv1a64(std::string_view(std::to_string(id) + key));
        used.insert(id);
        p.placementId = id;
        ++made;
    }
    return made;
}

usize applyOcStreamBounds(OcWorldData& w, const OcStreamData& d) {
    std::unordered_map<u64, const OcStreamBounds*> byId;
    byId.reserve(d.bounds.size());
    for (const OcStreamBounds& b : d.bounds) byId.emplace(b.id, &b);
    usize matched = 0;
    for (OcWorldPlacement& p : w.placements) {
        const auto it = p.placementId ? byId.find(p.placementId) : byId.end();
        if (it == byId.end()) continue;
        p.hasBounds = true;
        for (int a = 0; a < 3; ++a) { p.boundsMin[a] = it->second->min[a]; p.boundsMax[a] = it->second->max[a]; }
        ++matched;
    }
    return matched;
}

bool writeOcStreamFor(OcWorldData& w, const std::string& levelStem, const std::string& projectDir,
                      const std::vector<OcStreamFoliage>& foliage, std::string* why) {
    if (w.stream.dataPath.empty()) w.stream.dataPath = streamDir(levelStem) + ".ocstream";
    OcStreamData d;
    d.sourceHash = hashPlacements(w);
    d.foliage = foliage;
    for (const OcWorldPlacement& p : w.placements) {
        if (!p.placementId || !p.hasBounds) continue;
        OcStreamBounds b;
        b.id = p.placementId;
        for (int a = 0; a < 3; ++a) { b.min[a] = p.boundsMin[a]; b.max[a] = p.boundsMax[a]; }
        d.bounds.push_back(b);
    }
    std::set<std::string> lazy(w.stream.lazyDirs.begin(), w.stream.lazyDirs.end());
    for (const OcWorldPlacement& p : w.placements)
        if (const std::string dir = lazyDirOf(p.asset); !dir.empty()) lazy.insert(dir);
    w.stream.lazyDirs.assign(lazy.begin(), lazy.end());
    return saveOcStream((std::filesystem::path(projectDir) / w.stream.dataPath).string(), d, why);
}

bool bakeOcStream(OcWorldData& w, const std::string& levelStem, const std::string& contentDir,
                  const std::string& projectDir, const OcStreamBakeOptions& opt, OcStreamBakeReport& report,
                  std::string* why) {
    report = OcStreamBakeReport{};
    report.placements = w.placements.size();
    report.newIds = assignPlacementIds(w);

    // Local bounds per asset, once each: the host's (loaded meshes) first, else the file.
    struct LocalBounds { bool ok = false; Vec3 lo{}, hi{}; };
    std::unordered_map<std::string, LocalBounds> cache;
    const auto localBounds = [&](const std::string& asset) -> const LocalBounds& {
        if (const auto it = cache.find(asset); it != cache.end()) return it->second;
        LocalBounds b;
        if (opt.meshBounds && opt.meshBounds(asset, b.lo, b.hi)) b.ok = true;
        else {
            OcMeshData md;
            if (loadOcMesh((std::filesystem::path(contentDir) / asset).string(), md)) {
                b.ok = true;
                b.lo = md.boundsMin;
                b.hi = md.boundsMax;
            }
        }
        return cache.emplace(asset, b).first->second;
    };

    // World transforms, parents composed first as world::instantiate composes them.
    const usize n = w.placements.size();
    std::vector<Transform> worldXf(n);
    for (usize i = 0; i < n; ++i) {
        const OcWorldPlacement& p = w.placements[i];
        Transform xf = localOf(p);
        if (p.parent >= 0 && static_cast<usize>(p.parent) < i) {   // parents precede children
            const Transform& pw = worldXf[static_cast<usize>(p.parent)];
            Transform o;
            o.scale = Vec3{pw.scale.x * xf.scale.x, pw.scale.y * xf.scale.y, pw.scale.z * xf.scale.z};
            o.rotation = pw.rotation * xf.rotation;
            o.position = pw.position + pw.rotation.rotate(Vec3{xf.position.x * pw.scale.x, xf.position.y * pw.scale.y,
                                                               xf.position.z * pw.scale.z});
            xf = o;
        }
        worldXf[i] = xf;
    }
    for (usize i = 0; i < n; ++i) {
        OcWorldPlacement& p = w.placements[i];
        p.hasBounds = false;
        if (!p.className.empty() || p.asset.empty()) continue;
        const LocalBounds& lb = localBounds(p.asset);
        if (!lb.ok) continue;
        const Transform& t = worldXf[i];
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner{(c & 1) ? lb.hi.x : lb.lo.x, (c & 2) ? lb.hi.y : lb.lo.y, (c & 4) ? lb.hi.z : lb.lo.z};
            const Vec3 q = t.position + t.rotation.rotate(Vec3{corner.x * t.scale.x, corner.y * t.scale.y,
                                                               corner.z * t.scale.z});
            lo = Vec3{std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = Vec3{std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        p.hasBounds = true;
        p.boundsMin[0] = lo.x; p.boundsMin[1] = lo.y; p.boundsMin[2] = lo.z;
        p.boundsMax[0] = hi.x; p.boundsMax[1] = hi.y; p.boundsMax[2] = hi.z;
        ++report.withBounds;
    }

    // Foliage: a cell-sorted copy of each table beside the .ocstream; the Content original is untouched.
    std::vector<OcStreamFoliage> foliage;
    std::set<std::string> lazy(w.stream.lazyDirs.begin(), w.stream.lazyDirs.end());
    for (const std::string& rel : w.foliageFiles) {
        OcInstanceData in;
        std::string err;
        if (!loadOcInstances((std::filesystem::path(contentDir) / rel).string(), in, &err)) {
            if (why) *why = rel + ": " + err;
            return false;
        }
        std::vector<f32> radius(in.groups.size(), 0.0f);
        for (usize g = 0; g < in.groups.size(); ++g) {
            if (const std::string d = lazyDirOf(in.groups[g].asset); !d.empty()) lazy.insert(d);
            const LocalBounds& lb = localBounds(in.groups[g].asset);
            if (!lb.ok) continue;
            const f32 ex = std::max(std::fabs(lb.lo.x), std::fabs(lb.hi.x));
            const f32 ey = std::max(std::fabs(lb.lo.y), std::fabs(lb.hi.y));
            const f32 ez = std::max(std::fabs(lb.lo.z), std::fabs(lb.hi.z));
            radius[g] = std::sqrt(ex * ex + ey * ey + ez * ez);
        }
        if (!cellOcInstances(in, opt.foliageCellCm, radius, &err)) { if (why) *why = rel + ": " + err; return false; }
        const std::string cells = streamDir(levelStem) + "/" + std::filesystem::path(rel).filename().string();
        const std::filesystem::path out = std::filesystem::path(projectDir) / cells;
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        if (!saveOcInstances(out.string(), in, &err)) { if (why) *why = cells + ": " + err; return false; }
        foliage.push_back(OcStreamFoliage{slashes(rel), cells});
        ++report.foliageTables;
        report.foliageInstances += in.transforms.size() / 12;
        report.foliageCells += in.cells.size();
    }
    w.stream.lazyDirs.assign(lazy.begin(), lazy.end());
    w.stream.dataPath = streamDir(levelStem) + ".ocstream";
    return writeOcStreamFor(w, levelStem, projectDir, foliage, why);
}

} // namespace aver::fmt
