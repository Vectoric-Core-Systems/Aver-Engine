#include "aver/world/LevelFlatten.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/world/LevelTransform.hpp"

#include <unordered_set>

namespace aver::world {

void rotateD(const Quat& q, const f64 v[3], f64 out[3]) {
    const f64 x = q.x, y = q.y, z = q.z, w = q.w;
    // t = 2 * cross(u, v); out = v + w * t + cross(u, t): Quat::rotate, in f64.
    const f64 tx = 2.0 * (y * v[2] - z * v[1]);
    const f64 ty = 2.0 * (z * v[0] - x * v[2]);
    const f64 tz = 2.0 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

Pose64 poseOfPlacement(const fmt::OcWorldPlacement& p) {
    Pose64 o;
    o.pos[0] = p.x; o.pos[1] = p.y; o.pos[2] = p.z;
    o.rot = quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch), static_cast<f32>(p.yaw)});
    o.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
    return o;
}

Pose64 poseOfInstance(const fmt::OcLevelInstance& r) {
    Pose64 o;
    o.pos[0] = r.x; o.pos[1] = r.y; o.pos[2] = r.z;
    o.rot = quatFromEulerDeg(Vec3{static_cast<f32>(r.roll), static_cast<f32>(r.pitch), static_cast<f32>(r.yaw)});
    o.scale = Vec3{static_cast<f32>(r.sx), static_cast<f32>(r.sy), static_cast<f32>(r.sz)};
    return o;
}

Pose64 composePose(const Pose64& p, const Pose64& c) {
    Pose64 o;
    o.scale = Vec3{p.scale.x * c.scale.x, p.scale.y * c.scale.y, p.scale.z * c.scale.z};
    o.rot = p.rot * c.rot;
    const f64 s[3] = {c.pos[0] * static_cast<f64>(p.scale.x), c.pos[1] * static_cast<f64>(p.scale.y),
                      c.pos[2] * static_cast<f64>(p.scale.z)};
    f64 r[3];
    rotateD(p.rot, s, r);
    for (int a = 0; a < 3; ++a) o.pos[a] = p.pos[a] + r[a];
    return o;
}

namespace {

// A level-global record (light, decal, prefab instance) under an instance: its pose in the instance's level frame becomes a
// pose in the world frame. Only the scale slots a record has are touched (a light has none).
void composeRecord(const Pose64& inst, f64& x, f64& y, f64& z, f64& yaw, f64& pitch, f64& roll,
                   f64* sx, f64* sy, f64* sz) {
    Pose64 local;
    local.pos[0] = x; local.pos[1] = y; local.pos[2] = z;
    local.rot = quatFromEulerDeg(Vec3{static_cast<f32>(roll), static_cast<f32>(pitch), static_cast<f32>(yaw)});
    if (sx && sy && sz) local.scale = Vec3{static_cast<f32>(*sx), static_cast<f32>(*sy), static_cast<f32>(*sz)};
    const Pose64 w = composePose(inst, local);
    x = w.pos[0]; y = w.pos[1]; z = w.pos[2];
    const Vec3 e = eulerDegFromQuat(w.rot);
    roll = e.x; pitch = e.y; yaw = e.z;
    if (sx && sy && sz) { *sx = w.scale.x; *sy = w.scale.y; *sz = w.scale.z; }
}

struct Walker {
    LevelAssetLibrary& lib;
    FlatLevel& out;
    std::string why;

    // Appends one level's placements. `frame` is the instance pose they are composed under (null: the level's own, authored as is).
    void addPlacements(const std::vector<fmt::OcWorldPlacement>& ps, const std::vector<u64>* ids, const Pose64* frame,
                       u32 depth, const std::string& idPath) {
        const i32 base = static_cast<i32>(out.placements.size());
        bool warnedParent = false;
        out.placements.reserve(out.placements.size() + ps.size());
        for (usize i = 0; i < ps.size(); ++i) {
            const fmt::OcWorldPlacement& p = ps[i];
            FlatPlacement fp;
            fp.src = &p;
            fp.depth = depth;
            fp.topIndex = frame ? -1 : static_cast<i32>(i);
            const bool validParent = p.parent >= 0 && static_cast<usize>(p.parent) < i;
            if (p.parent >= 0 && !validParent && !warnedParent) {
                warnedParent = true;
                out.warnings.push_back("placement '" + p.asset + "' names a parent that does not precede it; flattened as a root");
            }
            fp.parent = validParent ? base + p.parent : -1;

            const Pose64 local = poseOfPlacement(p);
            if (frame && !validParent) {
                const Pose64 w = composePose(*frame, local);
                for (int a = 0; a < 3; ++a) fp.pos[a] = w.pos[a];
                const Vec3 e = eulerDegFromQuat(w.rot);
                fp.roll = e.x; fp.pitch = e.y; fp.yaw = e.z;
                fp.scale[0] = w.scale.x; fp.scale[1] = w.scale.y; fp.scale[2] = w.scale.z;
                fp.composed = true;
            } else {
                for (int a = 0; a < 3; ++a) fp.pos[a] = local.pos[a];
                fp.roll = static_cast<f32>(p.roll); fp.pitch = static_cast<f32>(p.pitch); fp.yaw = static_cast<f32>(p.yaw);
                fp.scale[0] = local.scale.x; fp.scale[1] = local.scale.y; fp.scale[2] = local.scale.z;
            }

            if (!frame) {
                fp.id = p.placementId ? p.placementId
                                      : fnv1a64(std::string_view("flat|" + p.asset + "|" + std::to_string(i)));
            } else {
                const u64 own = ids && i < ids->size() ? (*ids)[i] : 0;
                fp.id = fnv1a64(std::string_view(idPath + "/" + fmt::ocLevelInstIdText(own ? own : static_cast<u64>(i) + 1)));
            }
            out.placements.push_back(fp);
        }
    }

    bool expand(const std::vector<fmt::OcLevelInstance>& recs, const Pose64& frame, u32 depth,
                std::vector<std::string>& stack, std::vector<std::string>& assetStack, const std::string& idPath) {
        for (usize k = 0; k < recs.size(); ++k) {
            const fmt::OcLevelInstance& rec = recs[k];
            if (depth > LevelAssetLibrary::kMaxDepth) {
                why = "level instances nest deeper than " + std::to_string(LevelAssetLibrary::kMaxDepth);
                return false;
            }
            const std::string nk = LevelAssetLibrary::normalise(rec.asset);
            for (usize s = 0; s < stack.size(); ++s) {
                if (stack[s] != nk) continue;
                std::string chain;
                for (usize t = s; t < assetStack.size(); ++t) { chain += assetStack[t]; chain += " -> "; }
                why = "a level instances itself: " + chain + rec.asset;
                return false;
            }

            ResolvedContents rc;
            std::string err;
            if (!lib.resolve(rec, rc, &err)) {
                ++out.unloadedInstances;
                out.warnings.push_back("level instance '" + rec.asset + "' did not load (" + err + "): left empty");
                continue;
            }
            ++out.instances;
            if (depth > out.deepest) out.deepest = depth;

            out.held.push_back(std::make_unique<ResolvedContents>(std::move(rc)));
            const ResolvedContents& r = *out.held.back();
            const Pose64 inst = composePose(frame, poseOfInstance(rec));
            // The instance path: 16 hex digits per record id (an unassigned id falls back to the record's position).
            const std::string path = idPath + "/" + fmt::ocLevelInstIdText(rec.id ? rec.id : static_cast<u64>(k) + 1);

            addPlacements(r.world.placements, &r.placementIds, &inst, depth, path);

            for (const fmt::OcLight& l0 : r.world.lights) {
                fmt::OcLight l = l0;
                composeRecord(inst, l.x, l.y, l.z, l.yaw, l.pitch, l.roll, nullptr, nullptr, nullptr);
                out.lights.push_back(std::move(l));
            }
            for (const fmt::OcDecal& d0 : r.world.decals) {
                fmt::OcDecal d = d0;
                composeRecord(inst, d.x, d.y, d.z, d.yaw, d.pitch, d.roll, &d.sx, &d.sy, &d.sz);
                out.decals.push_back(std::move(d));
            }
            for (const fmt::OcPrefabInstance& p0 : r.world.prefabInstances) {
                fmt::OcPrefabInstance p = p0;
                composeRecord(inst, p.x, p.y, p.z, p.yaw, p.pitch, p.roll, &p.sx, &p.sy, &p.sz);
                out.prefabInstances.push_back(std::move(p));
            }
            if (!r.world.foliageFiles.empty()) {
                out.droppedFoliage += static_cast<u32>(r.world.foliageFiles.size());
                out.warnings.push_back("level instance '" + rec.asset + "' carries " + std::to_string(r.world.foliageFiles.size()) +
                                       " FOLIAGE table(s); an instance table cannot be composed into a flat list, so they are not in the cooked set");
            }

            stack.push_back(nk);
            assetStack.push_back(rec.asset);
            const bool ok = expand(r.world.levelInstances, inst, depth + 1, stack, assetStack, path);
            stack.pop_back();
            assetStack.pop_back();
            if (!ok) return false;
        }
        return true;
    }
};

} // namespace

bool flattenLevelInstances(const fmt::OcWorldData& level, const std::string& levelAsset, LevelAssetLibrary& lib,
                           FlatLevel& out, std::string* why) {
    out = FlatLevel{};
    Walker w{lib, out, {}};
    w.addPlacements(level.placements, nullptr, nullptr, 0, std::string());

    const Pose64 identity;
    std::vector<std::string> stack{LevelAssetLibrary::normalise(levelAsset)};
    std::vector<std::string> assetStack{levelAsset};
    if (!w.expand(level.levelInstances, identity, 1, stack, assetStack, std::string())) {
        if (why) *why = w.why;
        return false;
    }

    // Ids unique across the whole flat list: a duplicate (a copied record keeping its id) is re-derived from its position.
    std::unordered_set<u64> used;
    used.reserve(out.placements.size());
    for (usize i = 0; i < out.placements.size(); ++i) {
        FlatPlacement& fp = out.placements[i];
        u64 id = fp.id;
        u32 salt = 0;
        while (id == 0 || !used.insert(id).second)
            id = fnv1a64(std::string_view(std::to_string(fp.id) + "#" + std::to_string(i) + "#" + std::to_string(++salt)));
        fp.id = id;
    }
    return true;
}

} // namespace aver::world
