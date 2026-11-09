#include "aver/world/LevelAssetLibrary.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/formats/OcStream.hpp"
#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

namespace aver::world {

namespace {

bool sameKey(const std::string& a, const std::string& b) {
    return LevelAssetLibrary::normalise(a) == LevelAssetLibrary::normalise(b);
}

u64 hexMix(u64 a, u64 b) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return fnv1a64(std::string_view(buf));
}

// The AABB of `b` taken through `xf` (eight corners).
AABB transformBox(const AABB& b, const Transform& xf) {
    AABB out;
    bool first = true;
    for (int k = 0; k < 8; ++k) {
        const Vec3 c{(k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z};
        const Vec3 s{c.x * xf.scale.x, c.y * xf.scale.y, c.z * xf.scale.z};
        const Vec3 w = xf.position + xf.rotation.rotate(s);
        if (first) { out.min = out.max = w; first = false; }
        else out.expand(w);
    }
    return out;
}

void unionInto(AABB& acc, bool& any, const AABB& b) {
    if (!any) { acc = b; any = true; return; }
    acc.expand(b.min);
    acc.expand(b.max);
}

bool readVec3(const fmt::OcSaveField& f, f64 out[3]) {
    if (f.kind != fmt::kOcPrefabKindVec3 || f.f.size() < 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = f.f[static_cast<usize>(i)];
    return true;
}

} // namespace

Transform levelPlacementTransform(const fmt::OcWorldPlacement& p) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
    xf.rotation = quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch), static_cast<f32>(p.yaw)});
    xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
    return xf;
}

Transform levelInstanceTransform(const fmt::OcLevelInstance& r) {
    Transform xf;
    xf.position = Vec3{static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.z)};
    xf.rotation = quatFromEulerDeg(Vec3{static_cast<f32>(r.roll), static_cast<f32>(r.pitch), static_cast<f32>(r.yaw)});
    xf.scale = Vec3{static_cast<f32>(r.sx), static_cast<f32>(r.sy), static_cast<f32>(r.sz)};
    return xf;
}

std::string LevelAssetLibrary::normalise(const std::string& path) {
    std::string s = path;
    for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void LevelAssetLibrary::setLoader(Loader l) {
    loader_ = std::move(l);
}

LevelAssetLibrary::Loader LevelAssetLibrary::diskLoader(std::string contentDir) {
    return [dir = std::move(contentDir)](const std::string& path, fmt::OcWorldData& out, std::string* why) {
        return fmt::loadOcworld((std::filesystem::path(dir) / path).string(), out, why);
    };
}

bool LevelAssetLibrary::fill(Entry& e, const std::string& path, fmt::OcWorldData data) {
    if (!e.asset) e.asset = std::make_unique<LevelAsset>();
    LevelAsset& a = *e.asset;
    if (a.key.empty()) a.key = path;

    // Ids a child file lacks are derived from its path and file order, so a never-saved child still instantiates.
    // Both kinds share one id space: an override path names either a placement or a nested instance.
    a.idsFromFile = true;
    std::unordered_set<u64> used;
    for (const fmt::OcWorldPlacement& p : data.placements) if (p.placementId) used.insert(p.placementId);
    for (const fmt::OcLevelInstance& r : data.levelInstances) if (r.id) used.insert(r.id);
    const auto derive = [&](const std::string& tag) {
        u64 id = fnv1a64(std::string_view(tag));
        while (id == 0 || used.count(id)) id = fnv1a64(std::string_view(std::to_string(id) + tag));
        used.insert(id);
        return id;
    };
    for (usize i = 0; i < data.placements.size(); ++i)
        if (!data.placements[i].placementId) {
            data.placements[i].placementId = derive(path + "#" + std::to_string(i));
            a.idsFromFile = false;
        }
    for (usize i = 0; i < data.levelInstances.size(); ++i)
        if (!data.levelInstances[i].id) {
            data.levelInstances[i].id = derive(path + "#inst" + std::to_string(i));
            a.idsFromFile = false;
        }

    a.childAssets.clear();
    for (const fmt::OcLevelInstance& r : data.levelInstances) {
        bool seen = false;
        for (const std::string& c : a.childAssets) if (sameKey(c, r.asset)) { seen = true; break; }
        if (!seen) a.childAssets.push_back(r.asset);
    }

    // What an instance of this level depends on: the records section 2.3 takes, nothing the host level owns.
    fmt::OcWorldData h;
    h.placements = data.placements;
    h.lights = data.lights;
    h.decals = data.decals;
    h.prefabInstances = data.prefabInstances;
    h.foliageFiles = data.foliageFiles;
    h.foliageLayers = data.foliageLayers;
    h.levelInstances = data.levelInstances;
    a.contentHash = fnv1a64(std::string_view(fmt::writeOcworld(h)));

    a.data = std::move(data);
    e.valid = true;
    return true;
}

const LevelAsset* LevelAssetLibrary::get(const std::string& path, std::string* why) {
    const std::string nk = normalise(path);
    if (const auto it = cache_.find(nk); it != cache_.end() && it->second.valid) return it->second.asset.get();
    if (!loader_) {
        if (why) *why = "no level loader is installed";
        return nullptr;
    }
    fmt::OcWorldData d;
    std::string err;
    ++loads_;
    if (!loader_(path, d, &err)) {
        if (why) *why = err.empty() ? "could not load " + path : err;
        return nullptr;
    }
    Entry& e = cache_[nk];
    fill(e, path, std::move(d));
    return e.asset.get();
}

void LevelAssetLibrary::put(const std::string& path, fmt::OcWorldData data) {
    Entry& e = cache_[normalise(path)];
    fill(e, path, std::move(data));
}

void LevelAssetLibrary::invalidate(const std::string& path) {
    if (const auto it = cache_.find(normalise(path)); it != cache_.end()) it->second.valid = false;
}

bool LevelAssetLibrary::ensureIds(const std::string& path, const std::string& absoluteFile, std::string* why) {
    fmt::OcWorldData raw;
    std::string err;
    if (!fmt::loadOcworld(absoluteFile, raw, &err)) {
        if (why) *why = err.empty() ? "could not read " + absoluteFile : err;
        return false;
    }
    bool missing = false;
    for (const fmt::OcWorldPlacement& p : raw.placements) if (!p.placementId) { missing = true; break; }
    if (!missing)
        for (const fmt::OcLevelInstance& r : raw.levelInstances) if (!r.id) { missing = true; break; }
    if (!missing) return true;
    if (guard_ && !guard_(path, why)) return false;

    fmt::assignPlacementIds(raw);
    std::unordered_set<u64> used;
    for (const fmt::OcWorldPlacement& p : raw.placements) used.insert(p.placementId);
    for (const fmt::OcLevelInstance& r : raw.levelInstances) if (r.id) used.insert(r.id);
    for (usize i = 0; i < raw.levelInstances.size(); ++i) {
        fmt::OcLevelInstance& r = raw.levelInstances[i];
        if (r.id) continue;
        const std::string tag = r.asset + "|inst|" + std::to_string(i);
        u64 id = fnv1a64(std::string_view(tag));
        while (id == 0 || used.count(id)) id = fnv1a64(std::string_view(std::to_string(id) + tag));
        used.insert(id);
        r.id = id;
    }
    if (!fmt::saveOcworld(absoluteFile, raw, &err)) {
        if (why) *why = err;
        return false;
    }
    invalidate(path);
    return true;
}

bool LevelAssetLibrary::visit(const std::string& key, u32 depth, std::vector<std::string>& stack,
                              std::vector<std::string>& order, std::vector<std::string>& done, std::string* chain) {
    if (depth > kMaxDepth) {
        if (chain) *chain = "depth > " + std::to_string(kMaxDepth);
        return false;
    }
    const std::string nk = normalise(key);
    for (usize i = 0; i < stack.size(); ++i) {
        if (normalise(stack[i]) != nk) continue;
        if (chain) {
            std::string c;
            for (usize k = i; k < stack.size(); ++k) { c += stack[k]; c += " -> "; }
            *chain = c + key;
        }
        return false;
    }
    if (std::find(done.begin(), done.end(), nk) != done.end()) return true;
    bool listed = false;
    for (const std::string& o : order) if (normalise(o) == nk) { listed = true; break; }
    if (!listed) order.push_back(key);
    const LevelAsset* a = get(key);
    if (!a) { done.push_back(nk); return true; }   // an unloadable child is a leaf; resolve reports it
    stack.push_back(key);
    for (const fmt::OcLevelInstance& r : a->data.levelInstances)
        if (!visit(r.asset, depth + 1, stack, order, done, chain)) { stack.pop_back(); return false; }
    stack.pop_back();
    done.push_back(nk);
    return true;
}

bool LevelAssetLibrary::closure(const std::string& root, std::vector<std::string>& order, std::string* chain) {
    order.clear();
    std::vector<std::string> stack, done;
    return visit(root, 0, stack, order, done, chain);
}

bool LevelAssetLibrary::wouldCycle(const std::string& container, const std::string& child, std::string* chain) {
    if (sameKey(container, child)) {
        if (chain) *chain = container + " -> " + container;
        return true;
    }
    std::vector<std::string> order;
    std::string ignored;
    closure(child, order, &ignored);   // a cycle already inside `child` does not hide `container` from `order`
    for (const std::string& o : order)
        if (sameKey(o, container)) {
            if (chain) *chain = container + " -> " + child + " -> " + container;
            return true;
        }
    return false;
}

u64 LevelAssetLibrary::closureHash(const std::string& root) {
    std::vector<std::string> order;
    closure(root, order, nullptr);
    if (order.empty()) order.push_back(root);
    u64 h = 0;
    for (const std::string& k : order) {
        const LevelAsset* a = get(k);
        h = hexMix(h, a ? a->contentHash : 0);
    }
    return h;
}

std::string LevelAssetLibrary::keyForHash(u64 hash) const {
    for (const auto& [k, e] : cache_) {
        (void)k;
        if (!e.asset) continue;
        std::string p = e.asset->key;
        for (char& c : p) if (c == '\\') c = '/';
        if (fnv1a64(std::string_view(p)) == hash) return e.asset->key;
    }
    return {};
}

std::vector<std::string> LevelAssetLibrary::usersOf(const std::string& path) const {
    std::vector<std::string> out;
    for (const auto& [k, e] : cache_) {
        (void)k;
        if (!e.valid) continue;
        for (const std::string& c : e.asset->childAssets)
            if (sameKey(c, path)) { out.push_back(e.asset->key); break; }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool LevelAssetLibrary::resolve(const fmt::OcLevelInstance& rec, ResolvedContents& out, std::string* why) {
    out = ResolvedContents{};
    const LevelAsset* a = get(rec.asset, why);
    if (!a) return false;

    // Only the records an instance takes (section 2.3); everything else is the host level's.
    fmt::OcWorldData& w = out.world;
    w.placements = a->data.placements;
    w.lights = a->data.lights;
    w.decals = a->data.decals;
    w.prefabInstances = a->data.prefabInstances;
    w.foliageFiles = a->data.foliageFiles;
    w.foliageLayers = a->data.foliageLayers;
    w.levelInstances = a->data.levelInstances;

    std::vector<char> removed(w.placements.size(), 0);
    std::vector<char> instRemoved(w.levelInstances.size(), 0);
    std::unordered_map<u64, usize> byId;
    byId.reserve(w.placements.size());
    for (usize i = 0; i < w.placements.size(); ++i) byId.emplace(w.placements[i].placementId, i);
    std::unordered_map<u64, usize> instById;
    for (usize i = 0; i < w.levelInstances.size(); ++i) instById.emplace(w.levelInstances[i].id, i);

    for (const fmt::OcPrefabOverride& ov : rec.overrides) {
        if (ov.op != fmt::OcOverrideOp::Set) { ++out.unmatched; continue; }
        std::vector<u64> ids;
        if (!fmt::ocLevelInstSplitIds(ov.path, ids)) { ++out.unmatched; continue; }
        if (ids.size() > 1) {
            // Inside a nested instance: move it onto that record with the head stripped, so nesting needs no special case later.
            const auto it = instById.find(ids[0]);
            if (it == instById.end()) { ++out.unmatched; continue; }
            fmt::OcPrefabOverride moved = ov;
            ids.erase(ids.begin());
            moved.path = fmt::ocLevelInstJoinIds(ids);
            w.levelInstances[it->second].overrides.push_back(std::move(moved));
            ++out.applied;
            continue;
        }
        const std::string& f = ov.value.name;
        f64 v[3];
        if (const auto it = byId.find(ids[0]); it != byId.end()) {
            fmt::OcWorldPlacement& p = w.placements[it->second];
            bool ok = true;
            if (f == fmt::kLiFieldPosition && readVec3(ov.value, v)) { p.x = v[0]; p.y = v[1]; p.z = v[2]; }
            else if (f == fmt::kLiFieldRotation && readVec3(ov.value, v)) { p.yaw = v[0]; p.pitch = v[1]; p.roll = v[2]; }
            else if (f == fmt::kLiFieldScale && readVec3(ov.value, v)) { p.sx = v[0]; p.sy = v[1]; p.sz = v[2]; }
            else if (f == fmt::kLiFieldMaterial && ov.value.kind == fmt::kOcPrefabKindString) p.material = ov.value.s;
            else if (f == fmt::kLiFieldVisible && ov.value.kind == fmt::kOcPrefabKindBool) p.visible = ov.value.i != 0;
            else if (f == fmt::kLiFieldCollide && ov.value.kind == fmt::kOcPrefabKindBool) p.collide = ov.value.i != 0;
            else if (f == fmt::kLiFieldRemoved && ov.value.kind == fmt::kOcPrefabKindBool) removed[it->second] = ov.value.i != 0;
            else if (f == fmt::kLiFieldName && ov.value.kind == fmt::kOcPrefabKindString) p.name = ov.value.s;
            else ok = false;
            ok ? ++out.applied : ++out.unmatched;
        } else if (const auto jt = instById.find(ids[0]); jt != instById.end()) {
            // A nested instance's own record: its transform and whether it exists.
            fmt::OcLevelInstance& r = w.levelInstances[jt->second];
            bool ok = true;
            if (f == fmt::kLiFieldPosition && readVec3(ov.value, v)) { r.x = v[0]; r.y = v[1]; r.z = v[2]; }
            else if (f == fmt::kLiFieldRotation && readVec3(ov.value, v)) { r.yaw = v[0]; r.pitch = v[1]; r.roll = v[2]; }
            else if (f == fmt::kLiFieldScale && readVec3(ov.value, v)) { r.sx = v[0]; r.sy = v[1]; r.sz = v[2]; }
            else if (f == fmt::kLiFieldRemoved && ov.value.kind == fmt::kOcPrefabKindBool) instRemoved[jt->second] = ov.value.i != 0;
            else if (f == fmt::kLiFieldName && ov.value.kind == fmt::kOcPrefabKindString) r.name = ov.value.s;
            else ok = false;
            ok ? ++out.applied : ++out.unmatched;
        } else {
            ++out.unmatched;
        }
    }

    // Removal takes the whole subtree; parents precede children, so one forward pass is final.
    const usize n = w.placements.size();
    for (usize i = 0; i < n; ++i) {
        const i32 par = w.placements[i].parent;
        if (par >= 0 && static_cast<usize>(par) < i && removed[static_cast<usize>(par)]) removed[i] = 1;
    }
    std::vector<i32> remap(n, -1);
    std::vector<fmt::OcWorldPlacement> kept;
    kept.reserve(n);
    out.placementIds.reserve(n);
    for (usize i = 0; i < n; ++i) {
        if (removed[i]) continue;
        remap[i] = static_cast<i32>(kept.size());
        fmt::OcWorldPlacement p = std::move(w.placements[i]);
        p.parent = p.parent >= 0 ? remap[static_cast<usize>(p.parent)] : -1;
        out.placementIds.push_back(p.placementId);
        kept.push_back(std::move(p));
    }
    w.placements = std::move(kept);

    if (std::any_of(instRemoved.begin(), instRemoved.end(), [](char c) { return c != 0; })) {
        std::vector<fmt::OcLevelInstance> keptInst;
        for (usize i = 0; i < w.levelInstances.size(); ++i)
            if (!instRemoved[i]) keptInst.push_back(std::move(w.levelInstances[i]));
        w.levelInstances = std::move(keptInst);
    }
    if (out.unmatched)
        AVER_WARN("[LevelInstance] {}: {} override(s) matched nothing in '{}' and were ignored", rec.asset, out.unmatched, a->key);
    return true;
}

bool LevelAssetLibrary::boundsRec(const std::string& path, u32 depth, AABB& out) {
    if (depth > kMaxDepth) return false;
    const LevelAsset* a = get(path);
    if (!a) return false;
    if (a->data.hasExtent) {
        out.min = Vec3{static_cast<f32>(a->data.extentMin[0]), static_cast<f32>(a->data.extentMin[1]),
                       static_cast<f32>(a->data.extentMin[2])};
        out.max = Vec3{static_cast<f32>(a->data.extentMax[0]), static_cast<f32>(a->data.extentMax[1]),
                       static_cast<f32>(a->data.extentMax[2])};
        return true;
    }
    // Without an EXTENT a child is as big as the instances it holds; a level of bare placements stays unknown.
    AABB acc;
    bool any = false;
    for (const fmt::OcLevelInstance& r : a->data.levelInstances) {
        AABB local;
        if (!boundsRec(r.asset, depth + 1, local)) return false;   // one unknown nested bound makes the union unknown
        unionInto(acc, any, transformBox(local, levelInstanceTransform(r)));
    }
    if (!any || !a->data.placements.empty()) return false;
    out = acc;
    return true;
}

bool LevelAssetLibrary::instanceBounds(const fmt::OcLevelInstance& rec, AABB& out) {
    AABB local;
    if (!boundsRec(rec.asset, 1, local)) return false;
    out = transformBox(local, levelInstanceTransform(rec));
    return true;
}

} // namespace aver::world
