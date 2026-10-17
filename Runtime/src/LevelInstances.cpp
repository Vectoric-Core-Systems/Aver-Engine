#include "aver/game/LevelInstances.hpp"

#if AVER_MODULE_SCENE

#include "aver/core/Hash.hpp"
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/formats/OcPack.hpp"
#include "aver/formats/OcStream.hpp"
#include "aver/game/LevelComponents.hpp"
#include "aver/game/LevelDecals.hpp"
#include "aver/game/LevelLights.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/LevelTransform.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <unordered_set>

namespace aver::game {

namespace {

// Per-tick budgets, the LevelStreaming constants: kNearLoadMs when the nearest missing unit is within a quarter of the
// load distance, kFarLoadMs past three quarters; evictions kEvictMs. A unit realises over as many ticks as it needs.
constexpr f64 kNearLoadMs = 12.0, kFarLoadMs = 3.0, kEvictMs = 2.0;
// Cells within loadCm * this have their meshes read ahead (LevelFoliage::kPrefetchFactor).
constexpr f32 kPrefetchFactor = 1.3f;

// world::instantiate's composition: component-wise scale, rotation, rotated scaled offset.
Transform compose(const Transform& p, const Transform& c) {
    Transform o;
    o.scale = Vec3{p.scale.x * c.scale.x, p.scale.y * c.scale.y, p.scale.z * c.scale.z};
    o.rotation = p.rotation * c.rotation;
    const Vec3 s{c.position.x * p.scale.x, c.position.y * p.scale.y, c.position.z * p.scale.z};
    o.position = p.position + p.rotation.rotate(s);
    return o;
}

AABB transformBox(const AABB& b, const Transform& xf) {
    AABB out;
    for (int k = 0; k < 8; ++k) {
        const Vec3 c{(k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z};
        const Vec3 s{c.x * xf.scale.x, c.y * xf.scale.y, c.z * xf.scale.z};
        const Vec3 w = xf.position + xf.rotation.rotate(s);
        if (k == 0) out.min = out.max = w;
        else out.expand(w);
    }
    return out;
}

void unionBox(AABB& acc, bool& any, const AABB& b) {
    if (!any) { acc = b; any = true; return; }
    acc.expand(b.min);
    acc.expand(b.max);
}

// The id CLevelInstanceLink::assetId stores: fnv1a64 of the authored forward-slash path.
u64 assetHashOf(const std::string& asset) {
    std::string p = asset;
    for (char& c : p) if (c == '\\') c = '/';
    return fnv1a64(std::string_view(p));
}

std::string stemOf(const std::string& asset) {
    std::string p = asset;
    for (char& c : p) if (c == '\\') c = '/';
    const usize slash = p.find_last_of('/');
    if (slash != std::string::npos) p.erase(0, slash + 1);
    const usize dot = p.find_last_of('.');
    if (dot != std::string::npos && dot > 0) p.erase(dot);
    return p;
}

bool nearVec(const Vec3& a, const Vec3& b) {
    const auto close = [](f32 x, f32 y) { return std::fabs(x - y) <= 1e-3f + 1e-6f * std::fabs(x); };
    return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
}

bool nearRot(const Quat& a, const Quat& b) {
    const f32 d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return d > 0.999999f;
}

fmt::OcPrefabOverride makeOverride(const std::string& path, const char* field, u32 kind) {
    fmt::OcPrefabOverride o;
    o.op = fmt::OcOverrideOp::Set;
    o.path = path;
    o.component = fmt::kOcLevelInstPlacementComponent;
    o.value.name = field;
    o.value.kind = kind;
    return o;
}
fmt::OcPrefabOverride vec3Override(const std::string& path, const char* field, f32 x, f32 y, f32 z) {
    fmt::OcPrefabOverride o = makeOverride(path, field, fmt::kOcPrefabKindVec3);
    o.value.f = {x, y, z};
    return o;
}
fmt::OcPrefabOverride boolOverride(const std::string& path, const char* field, bool v) {
    fmt::OcPrefabOverride o = makeOverride(path, field, fmt::kOcPrefabKindBool);
    o.value.i = v ? 1 : 0;
    return o;
}
fmt::OcPrefabOverride stringOverride(const std::string& path, const char* field, const std::string& v) {
    fmt::OcPrefabOverride o = makeOverride(path, field, fmt::kOcPrefabKindString);
    o.value.s = v;
    return o;
}

// Keeps the placements `keep` accepts, renumbering `parent`.
template <class Keep>
void filterPlacements(world::ResolvedContents& rc, Keep keep) {
    const usize n = rc.world.placements.size();
    std::vector<i32> remap(n, -1);
    std::vector<fmt::OcWorldPlacement> kept;
    std::vector<u64> ids;
    kept.reserve(n);
    ids.reserve(n);
    for (usize i = 0; i < n; ++i) {
        if (!keep(i)) continue;
        remap[i] = static_cast<i32>(kept.size());
        fmt::OcWorldPlacement p = std::move(rc.world.placements[i]);
        p.parent = p.parent >= 0 ? remap[static_cast<usize>(p.parent)] : -1;
        ids.push_back(rc.placementIds[i]);
        kept.push_back(std::move(p));
    }
    rc.world.placements = std::move(kept);
    rc.placementIds = std::move(ids);
}

} // namespace

// ============================================================================================================
// State
// ============================================================================================================

struct LevelInstances::Impl {
    struct Member {
        scene::Entity entity = scene::kInvalidEntity;
        u64 placementId = 0;
        i32 body = -1;
        bool collide = true;
    };
    struct PendingClass {
        fmt::OcWorldPlacement p;
        Transform world;
    };
    struct Pending {
        world::ResolvedContents rc;
        std::vector<std::string> stack;
        std::vector<usize> subtreeEnd;      // per root placement: one past its last descendant
        std::unordered_set<u64> seenMesh;
        usize next = 0;                     // placements before this one are instantiated
        usize acquired = 0;                 // and before this one have their meshes
    };
    struct Inst {
        u32 id = 0;
        fmt::OcLevelInstance rec;
        const world::LevelAsset* asset = nullptr;
        scene::Entity root = scene::kInvalidEntity;
        u32 containerId = 0;                // 0 = placed in the host level (or under a script-chosen parent)
        u32 depth = 0;
        bool loaded = false, packedActive = false, editing = false, runtimeOnly = false, pinned = false, manual = false;
        std::string packWhy;
        Transform local, world;             // local to the container root (or parent entity); composed world
        AABB bounds;                        // world, when hasBounds
        bool hasBounds = false;
        i32 streamItem = -1;
        std::vector<Member> members;
        std::vector<i32> bodies;
        std::vector<i32> packedBodies;
        std::vector<scene::Entity> extras;          // lights, decals, prefab roots
        std::vector<scene::Entity> classEntities;
        std::vector<PendingClass> pendingClasses;
        std::vector<u64> meshes;
        std::vector<u32> childIds;
        std::vector<fmt::OcPrefabOverride> savedOverrides;   // set aside while editing
        std::vector<world::AnimatedBody> animated;
        std::shared_ptr<const world::LoadedLevelPack> pack;  // the pack this instance (or its packed ancestor) shows
        std::vector<u64> idPrefix;                           // nested-instance ids from the packed top-level down
        bool foliageSet = false, packedSet = false;
        u32 packedRows = 0;
        u32 entityCount = 0;
        std::vector<u64> needMeshes;
        bool needKnown = false;
        std::unique_ptr<Pending> pending;   // a realise spread over ticks (realiseBegin .. realiseFinish)
    };
    struct PackEntry {
        std::shared_ptr<const world::LoadedLevelPack> lp;
        int state = 0;   // 0 usable, 1 none, 2 stale
        std::string why;
    };

    explicit Impl(LevelInstances& s) : self(s) {}

    LevelInstances& self;
    std::map<u32, Inst> insts;
    std::unordered_map<scene::Entity, u32> byRoot;
    std::unordered_map<u64, u32> nestedIds;     // (container id, record id) -> instance id, stable across reloads
    u32 nextId = 1;
    u32 linkType = 0;
    Hooks hooks;
    PackedSink sink;
    world::InstantiateOptions opt;
    bool levelStreams = false;
    world::PlacementStreamSettings streamSettings;
    world::PlacementStreamer streamer;
    bool streamerBuilt = false;
    std::vector<u32> itemInst;                  // streamer item -> instance id (0 = none)
    std::unordered_map<u64, i32> meshShapes;
    std::vector<u64> orphanMeshes;              // acquired for entities a break left behind
    std::vector<world::AnimatedBody> animated;
    bool packedOk = false;
    std::vector<u32> packedQueue;               // instances to re-realise after setPackedRenderOk flipped, one per tick
    u32 loadingId = 0;                          // the streamed unit being realised over ticks
    std::unordered_map<std::string, PackEntry> packs;
    bool begun = false;

    // ---- small helpers ----
    scene::CLevelInstanceLink* link(scene::Entity e) {
        return linkType && self.world_ ? self.world_->component<scene::CLevelInstanceLink>(e, linkType) : nullptr;
    }
    void addLink(scene::Entity e, const scene::CLevelInstanceLink& l) {
        if (auto* c = static_cast<scene::CLevelInstanceLink*>(self.world_->addComponent(e, linkType))) *c = l;
    }
    Inst* get(u32 id) {
        const auto it = insts.find(id);
        return it == insts.end() ? nullptr : &it->second;
    }
    const Inst* get(u32 id) const {
        const auto it = insts.find(id);
        return it == insts.end() ? nullptr : &it->second;
    }
    Inst* byRootEntity(scene::Entity root) {
        const auto it = byRoot.find(root);
        return it == byRoot.end() ? nullptr : get(it->second);
    }
    const Inst* byRootEntity(scene::Entity root) const {
        const auto it = byRoot.find(root);
        return it == byRoot.end() ? nullptr : get(it->second);
    }
    static u64 nestedKey(u32 container, u64 recId) { return (static_cast<u64>(container) << 40) ^ recId ^ (static_cast<u64>(container) * 0x9E3779B97F4A7C15ull); }
    static u64 ownerPacked(u32 id) { return (static_cast<u64>(id) << 1) | 0u; }
    static u64 ownerFoliage(u32 id) { return (static_cast<u64>(id) << 1) | 1u; }

    std::vector<std::string> stackFor(const Inst& in) const {
        std::vector<std::string> st;
        for (const Inst* c = in.containerId ? get(in.containerId) : nullptr; c; c = c->containerId ? get(c->containerId) : nullptr)
            st.insert(st.begin(), world::LevelAssetLibrary::normalise(c->rec.asset));
        return st;
    }

    void rebuildAnimated() {
        animated.clear();
        for (const auto& [id, in] : insts) animated.insert(animated.end(), in.animated.begin(), in.animated.end());
    }

    // The pin state the streamer should hold for `in`.
    void applyPin(Inst& in) {
        if (in.streamItem >= 0) streamer.pin(static_cast<u32>(in.streamItem), in.pinned || in.editing || (in.manual && in.loaded));
    }

    bool wantsStream(const Inst& in) const {
        if (in.containerId != 0 || in.runtimeOnly || !in.hasBounds) return false;
        return in.rec.stream == 1 || (in.rec.stream == -1 && levelStreams);
    }

    // ---- creation ----
    u32 createInstance(const fmt::OcLevelInstance& rec, scene::Entity parentEnt, u32 containerId, u32 depth,
                       bool runtimeOnly, u32 forcedId) {
        const u32 id = forcedId ? forcedId : nextId++;
        if (id >= nextId) nextId = id + 1;
        Inst& in = insts[id];
        in = Inst{};
        in.id = id;
        in.rec = rec;
        in.containerId = containerId;
        in.depth = depth;
        in.runtimeOnly = runtimeOnly;
        in.local = world::levelInstanceTransform(rec);
        Transform parentWorld;
        scene::Entity rootParent = scene::kInvalidEntity;
        if (containerId) {
            const Inst* c = get(containerId);
            parentWorld = c->world;
            rootParent = c->root;
        } else if (parentEnt != scene::kInvalidEntity && self.world_->valid(parentEnt)) {
            parentWorld = transformFromMatrix(self.world_->worldMatrix(parentEnt));
            rootParent = parentEnt;
        }
        in.world = compose(parentWorld, in.local);
        in.root = self.world_->create(rec.name.empty() ? stemOf(rec.asset) : rec.name, rootParent, in.local);
        if (in.root == scene::kInvalidEntity) { insts.erase(id); return 0; }
        byRoot[in.root] = id;
        addLink(in.root, scene::CLevelInstanceLink{assetHashOf(rec.asset), 0, in.root, id, depth, 0});
        if (containerId) {
            if (Inst* c = get(containerId)) c->childIds.push_back(id);
        } else {
            AABB local;
            if (self.lib_ && self.lib_->instanceBounds(rec, local)) {
                in.bounds = transformBox(local, parentWorld);
                in.hasBounds = true;
            }
        }
        if (hooks.created) hooks.created(in.root, -1, nullptr);
        return id;
    }

    void registerStream(Inst& in) {
        if (!wantsStream(in) || in.streamItem >= 0) return;
        if (!streamerBuilt) { streamer.build({}, streamSettings); streamerBuilt = true; }
        in.streamItem = static_cast<i32>(streamer.add(in.bounds));
        const usize item = static_cast<usize>(in.streamItem);
        if (item >= itemInst.size()) itemInst.resize(item + 1, 0);
        itemInst[item] = in.id;
        applyPin(in);
    }

    // ---- packs ----
    std::shared_ptr<const world::LoadedLevelPack> packFor(Inst& in) {
        in.packWhy.clear();
        if (!in.rec.packed) return nullptr;
        if (!packedOk || !sink.set) { in.packWhy = "unpacked: renderer"; return nullptr; }
        if (in.editing) { in.packWhy = "unpacked: editing"; return nullptr; }
        if (!in.rec.overrides.empty()) { in.packWhy = "unpacked: overrides"; return nullptr; }
        if (self.port_.projectDir.empty() || !self.lib_) { in.packWhy = "no pack"; return nullptr; }
        const std::string key = world::LevelAssetLibrary::normalise(in.rec.asset);
        auto it = packs.find(key);
        if (it == packs.end()) {
            PackEntry e;
            auto lp = std::make_shared<world::LoadedLevelPack>();
            bool stale = false;
            std::string why;
            if (!world::loadLevelPack(self.port_.projectDir, in.rec.asset, self.lib_->closureHash(in.rec.asset), *lp, &stale, &why)) {
                e.state = 1;
                e.why = why;
                AVER_INFO("[LevelInstance] '{}' is marked packed but has no usable pack ({}); realising unpacked", in.rec.asset, why);
            } else if (stale) {
                e.state = 2;
                AVER_INFO("[LevelInstance] the pack of '{}' is older than the level; realising unpacked (bake it again)", in.rec.asset);
            } else {
                e.lp = std::move(lp);
            }
            it = packs.emplace(key, std::move(e)).first;
        }
        if (it->second.state == 1) { in.packWhy = "no pack"; return nullptr; }
        if (it->second.state == 2) { in.packWhy = "pack stale"; return nullptr; }
        return it->second.lp;
    }

    // ---- realise ----
    // Whole, in one call: level open, edits, nested and pinned instances.
    bool realise(Inst& in, std::vector<std::string> stack) {
        if (!realiseBegin(in, std::move(stack))) return false;
        realiseStep(in, -1.0);
        realiseFinish(in);
        return true;
    }

    // Resolves the child and its pack. False (nothing made) for a cycle, too deep, or a child that does not load.
    bool realiseBegin(Inst& in, std::vector<std::string> stack) {
        in.pending.reset();
        const std::string key = world::LevelAssetLibrary::normalise(in.rec.asset);
        if (std::find(stack.begin(), stack.end(), key) != stack.end()) {
            std::string chain;
            for (const std::string& s : stack) { chain += s; chain += " -> "; }
            AVER_ERROR("[LevelInstance] cycle {}{}: instance skipped", chain, in.rec.asset);
            return false;
        }
        if (in.depth > world::LevelAssetLibrary::kMaxDepth) {
            AVER_ERROR("[LevelInstance] '{}' nests deeper than {} levels: instance skipped", in.rec.asset,
                       world::LevelAssetLibrary::kMaxDepth);
            return false;
        }
        auto pend = std::make_unique<Pending>();
        world::ResolvedContents& rc = pend->rc;
        std::string why;
        if (!self.lib_ || !self.lib_->resolve(in.rec, rc, &why)) {
            AVER_WARN("[LevelInstance] '{}' did not load ({}): instance left empty", in.rec.asset, why);
            return false;
        }
        in.asset = self.lib_->get(in.rec.asset);

        // PACKED: a top-level instance decides for itself, a nested one inherits its container's pack.
        in.packedActive = false;
        if (in.containerId == 0) {
            in.pack = packFor(in);
            in.idPrefix.clear();
        }
        if (in.pack) {
            const world::LoadedLevelPack& lp = *in.pack;
            filterPlacements(rc, [&](usize i) {
                std::vector<u64> path = in.idPrefix;
                path.push_back(rc.placementIds[i]);
                return lp.pack.keeps(fmt::ocLevelInstJoinIds(path));
            });
            if (in.containerId == 0) in.packedActive = true;
        }

        // A placement's subtree follows it (parents precede children), so slices cut only between whole subtrees.
        const std::vector<fmt::OcWorldPlacement>& ps = rc.world.placements;
        std::vector<usize> rootOf(ps.size());
        pend->subtreeEnd.assign(ps.size(), 0);
        for (usize i = 0; i < ps.size(); ++i) {
            const i32 par = ps[i].parent;
            rootOf[i] = par >= 0 && static_cast<usize>(par) < i ? rootOf[static_cast<usize>(par)] : i;
            pend->subtreeEnd[rootOf[i]] = i + 1;
        }
        pend->seenMesh.insert(in.meshes.begin(), in.meshes.end());
        pend->stack = std::move(stack);
        in.animated.clear();
        in.pending = std::move(pend);
        return true;
    }

    // Meshes (with their materials and textures), then entities, a slice of whole subtrees at a time, until `budgetMs`
    // (< 0: all) is spent; at least one mesh or slice per call. True when every placement is instantiated.
    bool realiseStep(Inst& in, f64 budgetMs) {
        Pending& pd = *in.pending;
        const std::vector<fmt::OcWorldPlacement>& ps = pd.rc.world.placements;
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const u32 flags = in.editing ? scene::kLinkEditing : 0u;
        const u64 assetId = assetHashOf(in.rec.asset);
        constexpr usize kSlice = 16;
        bool worked = false;
        const auto spent = [&] {
            return budgetMs >= 0.0 && worked &&
                   std::chrono::duration<f64, std::milli>(Clock::now() - start).count() >= budgetMs;
        };
        // The materials acquires made, one at a time: false when the budget ran out with some waiting.
        const auto buildMaterials = [&] {
            while (self.port_.buildMaterial) {
                if (spent()) return false;
                if (!self.port_.buildMaterial()) return true;
                worked = true;
            }
            return true;
        };
        while (pd.next < ps.size()) {
            const usize a = pd.next;
            usize b = std::min(ps.size(), a + kSlice);
            for (usize i = a; i < b; ++i) b = std::max(b, pd.subtreeEnd[i]);   // 0 for a non-root: no effect

            // MESHES before the entities that show them; the prefetched physics shape with each.
            for (; pd.acquired < b; ++pd.acquired) {
                const fmt::OcWorldPlacement& p = ps[pd.acquired];
                if (!p.className.empty() || pd.seenMesh.count(p.objectId)) continue;
                if (!buildMaterials() || spent()) return false;
                pd.seenMesh.insert(p.objectId);
                if (self.port_.acquire && self.port_.acquire(p.objectId)) in.meshes.push_back(p.objectId);
                if (self.port_.takeShape && !meshShapes.count(p.objectId))
                    if (const i32 shape = self.port_.takeShape(p.objectId)) meshShapes.emplace(p.objectId, shape);
                worked = true;
            }
            if (!buildMaterials() || spent()) return false;

            // ENTITIES: this slice of the child's placements, attached under the root.
            fmt::OcWorldData slice;
            slice.placements.assign(ps.begin() + static_cast<isize>(a), ps.begin() + static_cast<isize>(b));
            for (fmt::OcWorldPlacement& p : slice.placements)
                if (p.parent >= 0) p.parent -= static_cast<i32>(a);
            world::InstantiateOptions o = opt;
            o.progress = nullptr;
            o.meshShapes = &meshShapes;
            const world::InstantiateOptions::Attach attach{in.root, in.world};
            o.attach = &attach;
            const world::LevelInstance li = world::instantiate(slice, o);
            for (usize k = 0; k < li.entities.size(); ++k) {
                const usize pi = a + li.placementIndex[k];
                const u64 pid = pd.rc.placementIds[pi];
                const i32 body = k < li.entityBody.size() ? li.entityBody[k] : -1;
                addLink(li.entities[k], scene::CLevelInstanceLink{assetId, pid, in.root, in.id, in.depth, flags});
                in.members.push_back(Member{li.entities[k], pid, body, ps[pi].collide});
                if (body > 0) in.bodies.push_back(body);
                if (hooks.created) hooks.created(li.entities[k], body, &ps[pi]);
            }
            in.animated.insert(in.animated.end(), li.animatedBodies.begin(), li.animatedBodies.end());
            // COMPONENT records on the members, after the host's created hook. Also what keeps them through an editor
            // Commit, which captures the members' live components back into the child level.
            for (usize k = 0; k < li.entities.size(); ++k) {
                const usize pi = a + li.placementIndex[k];
                if (!ps[pi].components.empty())
                    applyPlacedComponents(scene::World::instance(), li.entities[k], ps[pi], static_cast<i64>(pi));
            }
            pd.next = b;
            worked = true;
        }
        return true;
    }

    // Classes, lights, decals, prefabs, foliage, pack and nested instances; the unit is loaded after this.
    void realiseFinish(Inst& in) {
        const std::unique_ptr<Pending> pend = std::move(in.pending);
        world::ResolvedContents& rc = pend->rc;
        const std::vector<std::string>& stack = pend->stack;
        const std::string key = world::LevelAssetLibrary::normalise(in.rec.asset);
        const u32 flags = in.editing ? scene::kLinkEditing : 0u;
        const u64 assetId = assetHashOf(in.rec.asset);

        // CLASS placements wait for scripting (spawnPendingClasses); their world pose is composed here.
        {
            std::vector<Transform> rel(rc.world.placements.size());
            for (usize i = 0; i < rc.world.placements.size(); ++i) {
                const fmt::OcWorldPlacement& p = rc.world.placements[i];
                const Transform xf = world::levelPlacementTransform(p);
                rel[i] = (p.parent >= 0 && static_cast<usize>(p.parent) < i) ? compose(rel[static_cast<usize>(p.parent)], xf) : xf;
                if (!p.className.empty()) in.pendingClasses.push_back(PendingClass{p, compose(in.world, rel[i])});
            }
        }

        // LIGHTS and DECALS: bare entities at the record's pose, then hung under the root keeping that local pose.
        {
            std::vector<scene::Entity> made = spawnLevelLights(*self.world_, rc.world.lights);
            const std::vector<scene::Entity> decals = spawnLevelDecals(*self.world_, rc.world.decals);
            made.insert(made.end(), decals.begin(), decals.end());
            for (const scene::Entity e : made) {
                self.world_->setParent(e, in.root, false);
                addLink(e, scene::CLevelInstanceLink{assetId, 0, in.root, in.id, in.depth, flags});
                in.extras.push_back(e);
                if (hooks.created) hooks.created(e, -1, nullptr);
            }
        }

        // PREFAB instances through the host's prefab system.
        if (hooks.spawnPrefab) {
            for (const fmt::OcPrefabInstance& pr : rc.world.prefabInstances) {
                Transform xf;
                xf.position = Vec3{static_cast<f32>(pr.x), static_cast<f32>(pr.y), static_cast<f32>(pr.z)};
                xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(pr.roll), static_cast<f32>(pr.pitch), static_cast<f32>(pr.yaw)});
                xf.scale = Vec3{static_cast<f32>(pr.sx), static_cast<f32>(pr.sy), static_cast<f32>(pr.sz)};
                const scene::Entity e = hooks.spawnPrefab(pr, xf, in.root);
                if (e == scene::kInvalidEntity) continue;
                addLink(e, scene::CLevelInstanceLink{assetId, 0, in.root, in.id, in.depth, flags});
                in.extras.push_back(e);
            }
        }

        // The child's own FOLIAGE tables, through the instance matrix.
        if (sink.set && !rc.world.foliageFiles.empty() && !self.port_.contentDir.empty()) {
            fmt::OcInstanceData merged;
            const Mat4 m = in.world.toMatrix();
            for (const std::string& path : rc.world.foliageFiles) {
                fmt::OcInstanceData d;
                std::string ferr;
                if (!fmt::loadOcInstances((std::filesystem::path(self.port_.contentDir) / path).string(), d, &ferr)) {
                    AVER_WARN("[LevelInstance] '{}' foliage '{}' could not be read: {}", in.rec.asset, path, ferr);
                    continue;
                }
                const u32 base = static_cast<u32>(merged.transforms.size() / 12);
                const usize at = merged.transforms.size();
                merged.transforms.resize(at + d.transforms.size());
                fmt::transformOcInstanceRows(d.transforms.data(), d.transforms.size() / 12, m, merged.transforms.data() + at);
                for (fmt::OcInstanceGroup g : d.groups) { g.first += base; merged.groups.push_back(std::move(g)); }
            }
            if (!merged.groups.empty()) {
                sink.set(ownerFoliage(in.id), std::move(merged), std::vector<std::string>(0));
                in.foliageSet = true;
            }
        }

        if (in.packedActive) applyPack(in);

        // NESTED instances, one level down.
        std::vector<std::string> nestedStack = stack;
        nestedStack.push_back(key);
        for (const fmt::OcLevelInstance& nr : rc.world.levelInstances) {
            const u64 nk = nestedKey(in.id, nr.id);
            const auto known = nestedIds.find(nk);
            const u32 cid = createInstance(nr, scene::kInvalidEntity, in.id, in.depth + 1, in.runtimeOnly,
                                           known == nestedIds.end() ? 0 : known->second);
            if (!cid) continue;
            nestedIds[nk] = cid;
            Inst& child = insts[cid];
            child.pack = in.pack;
            child.idPrefix = in.idPrefix;
            child.idPrefix.push_back(nr.id);
            realise(child, nestedStack);
            child.loaded = true;   // an instance whose own realise failed is still "there", empty
            if (onResidencyFn()) onResidencyFn()(child.root, true, child.entityCount);
        }

        // Bodies went in a slice at a time; one rebalance for a big unit, as a whole-level instantiate does.
#if AVER_MODULE_PHYSICS
        if (in.bodies.size() >= 256) aver_phys_optimize_broadphase();
#endif
        while (self.port_.buildMaterial && self.port_.buildMaterial()) {}   // any the pack's acquires queued
        in.entityCount = 1 + static_cast<u32>(in.members.size() + in.extras.size() + in.classEntities.size());
        in.loaded = true;
        if (!animated.empty() || !in.animated.empty()) rebuildAnimated();
        if (self.classesReady_) spawnClasses(in);
        applyPin(in);
        if (onResidencyFn() && in.containerId == 0) onResidencyFn()(in.root, true, in.entityCount);
    }

    const std::function<void(scene::Entity, bool, u32)>& onResidencyFn() const { return self.onResidency; }

    // The packed rows through the instance matrix, and a static body per colliding row.
    void applyPack(Inst& in) {
        const world::LoadedLevelPack& lp = *in.pack;
        const usize rows = lp.table.transforms.size() / 12;
        fmt::OcInstanceData d;
        d.groups = lp.table.groups;
        d.transforms.resize(rows * 12);
        const Mat4 m = in.world.toMatrix();
        fmt::transformOcInstanceRows(lp.table.transforms.data(), rows, m, d.transforms.data());
        in.packedRows = static_cast<u32>(rows);
#if AVER_MODULE_PHYSICS
        if (opt.createBodies && aver_phys_ready()) {
            for (usize g = 0; g < lp.table.groups.size() && g < lp.pack.groups.size(); ++g) {
                if (!lp.pack.groups[g].collide) continue;
                const fmt::OcInstanceGroup& grp = lp.table.groups[g];
                const u64 meshId = fnv1a64(std::string_view(grp.asset));
                if (std::find(in.meshes.begin(), in.meshes.end(), meshId) == in.meshes.end() &&
                    self.port_.acquire && self.port_.acquire(meshId))
                    in.meshes.push_back(meshId);
                for (u32 r = 0; r < grp.count; ++r) {
                    const f32* row = d.transforms.data() + (static_cast<usize>(grp.first) + r) * 12;
                    Mat4 rm;
                    for (int a = 0; a < 4; ++a)
                        for (int c = 0; c < 4; ++c) rm.m[a][c] = c < 3 ? row[a * 3 + c] : (a == 3 ? 1.0f : 0.0f);
                    const i32 body = world::addStaticPlacementBody(opt, meshShapes, meshId, transformFromMatrix(rm));
                    if (body > 0) in.packedBodies.push_back(body);
                }
            }
        }
#endif
        std::vector<std::string> mats;
        mats.reserve(lp.pack.groups.size());
        for (const fmt::OcPackGroup& g : lp.pack.groups) mats.push_back(g.material);
        sink.set(ownerPacked(in.id), std::move(d), std::move(mats));
        in.packedSet = true;
    }

    void spawnClasses(Inst& in) {
        if (in.pendingClasses.empty()) return;
        std::vector<PendingClass> pending;
        pending.swap(in.pendingClasses);
        const u64 assetId = assetHashOf(in.rec.asset);
        for (PendingClass& pc : pending) {
            if (!hooks.spawnClass) { in.pendingClasses.push_back(std::move(pc)); continue; }
            const scene::Entity e = hooks.spawnClass(pc.p, pc.world, in.root);
            if (e == scene::kInvalidEntity) continue;
            // Under the root keeping its world pose, so moving the instance carries the actor.
            self.world_->setParent(e, in.root, true);
            addLink(e, scene::CLevelInstanceLink{assetId, pc.p.placementId, in.root, in.id, in.depth, in.editing ? scene::kLinkEditing : 0u});
            in.classEntities.push_back(e);
            in.members.push_back(Member{e, pc.p.placementId, -1, true});
            if (hooks.created) hooks.created(e, -1, &pc.p);
            ++in.entityCount;
        }
    }

    // ---- unrealise ----
    void collectDescendants(scene::Entity e, std::vector<scene::Entity>& out) const {
        for (scene::Entity c = self.world_->firstChild(e); c != scene::kInvalidEntity; c = self.world_->nextSibling(c)) {
            out.push_back(c);
            collectDescendants(c, out);
        }
    }

    // Everything under the root goes; the root stays. Nested instances are destroyed outright (they come back
    // with their ids when the container realises again).
    void unrealise(Inst& in) {
        in.pending.reset();
        if (loadingId == in.id) loadingId = 0;
        const std::vector<u32> kids = in.childIds;
        for (const u32 cid : kids) destroyInstance(cid);
        in.childIds.clear();
        if (!in.loaded && in.members.empty() && in.meshes.empty() && in.extras.empty()) { in.pendingClasses.clear(); return; }

        HitchMarks hm("instance unrealise", 0.25);
        std::vector<scene::Entity> all;
        collectDescendants(in.root, all);
        if (hooks.destroying)
            for (const scene::Entity e : all)
                if (self.world_->valid(e) && !self.world_->destroyPending(e)) hooks.destroying(e);
        hm.mark("hooks");
        for (const scene::Entity e : in.classEntities) {
            if (!self.world_->valid(e)) continue;
            if (hooks.destroyClass) hooks.destroyClass(e);
            else self.world_->destroy(e);
        }
#if AVER_MODULE_PHYSICS
        for (const i32 b : in.bodies) if (b > 0) aver_phys_remove_body(b);
        for (const i32 b : in.packedBodies) if (b > 0) aver_phys_remove_body(b);
#endif
        hm.mark("bodies");
        if (sink.clear) {
            if (in.packedSet) sink.clear(ownerPacked(in.id));
            if (in.foliageSet) sink.clear(ownerFoliage(in.id));
        }
        for (scene::Entity c = self.world_->firstChild(in.root); c != scene::kInvalidEntity;) {
            const scene::Entity next = self.world_->nextSibling(c);
            if (self.world_->valid(c) && !self.world_->destroyPending(c)) self.world_->destroy(c);
            c = next;
        }
        hm.mark("destroy");
        if (self.port_.release) for (const u64 m : in.meshes) self.port_.release(m);
        // A mesh no unit holds any more takes its shape with it (its bodies are gone; a reload brings a new one).
        if (self.port_.loaded)
            for (const u64 m : in.meshes)
                if (const auto s = meshShapes.find(m); s != meshShapes.end() && !self.port_.loaded(m)) {
#if AVER_MODULE_PHYSICS
                    if (s->second) aver_phys_release_mesh_shape(s->second);
#endif
                    meshShapes.erase(s);
                }
        hm.mark("release");
        const u32 count = in.entityCount;
        in.members.clear();
        in.bodies.clear();
        in.packedBodies.clear();
        in.extras.clear();
        in.classEntities.clear();
        in.pendingClasses.clear();
        in.meshes.clear();
        in.animated.clear();
        in.packedSet = in.foliageSet = in.packedActive = false;
        in.packedRows = 0;
        in.entityCount = 0;
        const bool was = in.loaded;
        in.loaded = false;
        rebuildAnimated();
        if (was && onResidencyFn() && in.containerId == 0) onResidencyFn()(in.root, false, count);
    }

    // unrealise + realise, the root and its streamer item staying put.
    void rebuild(Inst& in) {
        const bool was = in.loaded;
        unrealise(in);
        if (was) {
            realise(in, stackFor(in));
            in.loaded = true;
        }
    }

    // Removes an instance entirely: contents, root, table entries.
    void destroyInstance(u32 id) {
        Inst* in = get(id);
        if (!in) return;
        unrealise(*in);
        const scene::Entity root = in->root;
        if (hooks.destroying && self.world_->valid(root)) hooks.destroying(root);
        if (in->streamItem >= 0) {
            streamer.remove(static_cast<u32>(in->streamItem));
            if (static_cast<usize>(in->streamItem) < itemInst.size()) itemInst[static_cast<usize>(in->streamItem)] = 0;
        }
        if (in->containerId) {
            if (Inst* c = get(in->containerId)) c->childIds.erase(std::remove(c->childIds.begin(), c->childIds.end(), id), c->childIds.end());
        }
        byRoot.erase(root);
        if (self.world_->valid(root)) self.world_->destroy(root);
        insts.erase(id);
        rebuildAnimated();
    }

    // ---- overrides ----
    // The instance's overrides as the diff of its live members against `baselineRec` resolved with NO overrides.
    void diffInstance(Inst& in, const fmt::OcLevelInstance& baselineRec, const std::vector<u64>& prefix,
                      std::vector<fmt::OcPrefabOverride>& out) {
        fmt::OcLevelInstance base = baselineRec;
        base.overrides.clear();
        world::ResolvedContents rc;
        if (!self.lib_ || !self.lib_->resolve(base, rc)) return;
        const auto pathOf = [&](u64 id) {
            std::vector<u64> p = prefix;
            p.push_back(id);
            return fmt::ocLevelInstJoinIds(p);
        };
        std::unordered_map<u64, const Member*> byPid;
        for (const Member& m : in.members) byPid.emplace(m.placementId, &m);

        for (usize i = 0; i < rc.world.placements.size(); ++i) {
            const fmt::OcWorldPlacement& p = rc.world.placements[i];
            const u64 pid = rc.placementIds[i];
            if (!p.className.empty()) continue;   // actors are the class system's
            const auto it = byPid.find(pid);
            const bool alive = it != byPid.end() && self.world_->valid(it->second->entity) &&
                               !self.world_->destroyPending(it->second->entity);
            const std::string path = pathOf(pid);
            if (!alive) {
                // A removed subtree is one override, on its top: a child of a vanished parent adds nothing.
                bool parentVanished = false;
                if (p.parent >= 0) {
                    const auto pit = byPid.find(rc.placementIds[static_cast<usize>(p.parent)]);
                    parentVanished = pit == byPid.end() || !self.world_->valid(pit->second->entity) ||
                                     self.world_->destroyPending(pit->second->entity);
                }
                if (!parentVanished) out.push_back(boolOverride(path, fmt::kLiFieldRemoved, true));
                continue;
            }
            const Member& m = *it->second;
            const Transform& live = self.world_->localTransform(m.entity);
            const Transform want = world::levelPlacementTransform(p);
            if (!nearVec(live.position, want.position))
                out.push_back(vec3Override(path, fmt::kLiFieldPosition, live.position.x, live.position.y, live.position.z));
            if (!nearRot(live.rotation, want.rotation)) {
                const Vec3 e = world::eulerDegFromQuat(live.rotation);   // roll, pitch, yaw
                out.push_back(vec3Override(path, fmt::kLiFieldRotation, e.z, e.y, e.x));
            }
            if (!nearVec(live.scale, want.scale))
                out.push_back(vec3Override(path, fmt::kLiFieldScale, live.scale.x, live.scale.y, live.scale.z));
            if (const auto* mr = self.world_->component<scene::CMeshRenderer>(m.entity, scene::kComponentMeshRenderer)) {
                const i32 wantTok = p.material.empty() ? 0 : aver_scene_material(0, p.material.c_str());
                if (mr->material != wantTok) {
                    const char* nm = mr->material ? aver_scene_material_name(mr->material) : "";
                    out.push_back(stringOverride(path, fmt::kLiFieldMaterial, nm ? nm : ""));
                }
                if (((mr->flags & scene::kMeshRendererVisible) != 0) != p.visible)
                    out.push_back(boolOverride(path, fmt::kLiFieldVisible, (mr->flags & scene::kMeshRendererVisible) != 0));
            }
            if (m.collide != p.collide) out.push_back(boolOverride(path, fmt::kLiFieldCollide, m.collide));
        }

        // Nested instances: their own transform, their existence, and what is inside them.
        for (const fmt::OcLevelInstance& nr : rc.world.levelInstances) {
            Inst* child = nullptr;
            for (const u32 cid : in.childIds) {
                Inst* c = get(cid);
                if (c && c->rec.id == nr.id) { child = c; break; }
            }
            const std::string path = pathOf(nr.id);
            if (!child || !self.world_->valid(child->root) || self.world_->destroyPending(child->root)) {
                out.push_back(boolOverride(path, fmt::kLiFieldRemoved, true));
                continue;
            }
            const Transform& live = self.world_->localTransform(child->root);
            const Transform want = world::levelInstanceTransform(nr);
            if (!nearVec(live.position, want.position))
                out.push_back(vec3Override(path, fmt::kLiFieldPosition, live.position.x, live.position.y, live.position.z));
            if (!nearRot(live.rotation, want.rotation)) {
                const Vec3 e = world::eulerDegFromQuat(live.rotation);
                out.push_back(vec3Override(path, fmt::kLiFieldRotation, e.z, e.y, e.x));
            }
            if (!nearVec(live.scale, want.scale))
                out.push_back(vec3Override(path, fmt::kLiFieldScale, live.scale.x, live.scale.y, live.scale.z));
            std::vector<u64> childPrefix = prefix;
            childPrefix.push_back(nr.id);
            diffInstance(*child, nr, childPrefix, out);
        }
    }

    bool computeOverridesOf(Inst& in, std::vector<fmt::OcPrefabOverride>& out) {
        out.clear();
        if (in.editing) { out = in.savedOverrides; return true; }
        if (!in.loaded || in.packedActive) { out = in.rec.overrides; return true; }
        diffInstance(in, in.rec, {}, out);
        // The label lives in the host's tables, not on the entity: name overrides ride through.
        for (const fmt::OcPrefabOverride& ov : in.rec.overrides)
            if (ov.value.name == fmt::kLiFieldName) out.push_back(ov);
        return true;
    }

    void syncOverrides(Inst& in) {
        if (in.containerId != 0 || in.editing || !in.loaded || in.packedActive) return;
        std::vector<fmt::OcPrefabOverride> ov;
        computeOverridesOf(in, ov);
        in.rec.overrides = std::move(ov);
    }

    // ---- records ----
    fmt::OcLevelInstance recordOfInst(Inst& in) {
        fmt::OcLevelInstance r = in.rec;
        if (self.world_->valid(in.root)) {
            const Transform& t = self.world_->localTransform(in.root);
            r.x = t.position.x; r.y = t.position.y; r.z = t.position.z;
            const Vec3 e = world::eulerDegFromQuat(t.rotation);
            r.roll = e.x; r.pitch = e.y; r.yaw = e.z;
            r.sx = t.scale.x; r.sy = t.scale.y; r.sz = t.scale.z;
        }
        std::vector<fmt::OcPrefabOverride> ov;
        computeOverridesOf(in, ov);
        r.overrides = std::move(ov);
        return r;
    }

    // ---- streaming helpers ----
    void collectNeeded(const fmt::OcLevelInstance& rec, std::unordered_set<u64>& set, u32 depth) {
        if (depth > world::LevelAssetLibrary::kMaxDepth || !self.lib_) return;
        world::ResolvedContents rc;
        if (!self.lib_->resolve(rec, rc)) return;
        for (const fmt::OcWorldPlacement& p : rc.world.placements) if (p.className.empty()) set.insert(p.objectId);
        for (const fmt::OcLevelInstance& n : rc.world.levelInstances) collectNeeded(n, set, depth + 1);
    }
    const std::vector<u64>& needed(Inst& in) {
        if (!in.needKnown) {
            std::unordered_set<u64> set;
            collectNeeded(in.rec, set, 0);
            in.needMeshes.assign(set.begin(), set.end());
            std::sort(in.needMeshes.begin(), in.needMeshes.end());
            in.needKnown = true;
        }
        return in.needMeshes;
    }

    // A part-realised unit resolved its content (overrides, pack, editing) when it began: dropped before any of that
    // changes, so the next tick starts it afresh.
    void cancelPending(Inst& in) { if (in.pending) unrealise(in); }

    void loadUnit(Inst& in) {
        if (in.pending) {
            realiseStep(in, -1.0);
            realiseFinish(in);
            if (loadingId == in.id) loadingId = 0;
        } else {
            realise(in, stackFor(in));
        }
        in.loaded = true;
        if (in.streamItem >= 0) streamer.markLoaded(static_cast<u32>(in.streamItem));
        applyPin(in);
    }
    void evictUnit(Inst& in) {
        syncOverrides(in);
        unrealise(in);
        if (in.streamItem >= 0) streamer.markEvicted(static_cast<u32>(in.streamItem));
    }
};

// ============================================================================================================
// Public surface
// ============================================================================================================

LevelInstances::LevelInstances() : d_(new Impl(*this)) {}
LevelInstances::~LevelInstances() = default;

void LevelInstances::init(world::LevelAssetLibrary& lib, scene::World& w) {
    lib_ = &lib;
    world_ = &w;
    d_->linkType = scene::registerLevelInstanceLink(w);
}

void LevelInstances::begin(const fmt::OcWorldData& level, const ContentPort& port, const world::InstantiateOptions& opt,
                           const world::PlacementStreamSettings* levelStream, Hooks hooks, PackedSink sink) {
    if (!lib_ || !world_) return;
    end();
    Impl& d = *d_;
    port_ = port;
    d.opt = opt;
    d.opt.progress = nullptr;
    d.opt.attach = nullptr;
    d.hooks = std::move(hooks);
    d.sink = std::move(sink);
    d.levelStreams = levelStream != nullptr;
    d.streamSettings = levelStream ? *levelStream : world::PlacementStreamSettings{};
    d.begun = true;

    u32 index = 0;
    for (fmt::OcLevelInstance rec : level.levelInstances) {
        if (!rec.id) rec.id = fnv1a64(std::string_view(rec.asset + "#li" + std::to_string(index)));
        ++index;
        const u32 id = d.createInstance(rec, scene::kInvalidEntity, 0, 0, false, 0);
        if (!id) continue;
        Impl::Inst& in = d.insts[id];
        d.registerStream(in);
        if (in.streamItem >= 0) continue;   // realised by tick() when a viewer comes near
        d.loadUnit(in);
    }
    if (!d.insts.empty())
        AVER_INFO("[LevelInstance] {} instance record(s); {} streamed as units", level.levelInstances.size(), d.itemInst.size());
}

void LevelInstances::end() {
    Impl& d = *d_;
    if (!world_) return;
    std::vector<u32> top;
    for (const auto& [id, in] : d.insts) if (in.containerId == 0) top.push_back(id);
    for (const u32 id : top) d.destroyInstance(id);
    d.insts.clear();
    d.byRoot.clear();
    d.nestedIds.clear();
    if (port_.release) for (const u64 m : d.orphanMeshes) port_.release(m);
    d.orphanMeshes.clear();
#if AVER_MODULE_PHYSICS
    for (const auto& [mesh, shape] : d.meshShapes) { (void)mesh; if (shape) aver_phys_release_mesh_shape(shape); }
#endif
    d.meshShapes.clear();
    d.packs.clear();
    d.packedQueue.clear();
    d.animated.clear();
    d.streamer = world::PlacementStreamer{};
    d.streamerBuilt = false;
    d.itemInst.clear();
    d.begun = false;
    d.nextId = 1;
}

void LevelInstances::tick(const std::vector<Vec3>& viewers) {
    Impl& d = *d_;
    if (!d.begun) return;

    // A flip of the packed-render predicate re-realises the packed instances, one per tick.
    while (!d.packedQueue.empty()) {
        const u32 id = d.packedQueue.back();
        d.packedQueue.pop_back();
        Impl::Inst* in = d.get(id);
        if (!in || !in->loaded) continue;
        d.rebuild(*in);
        break;
    }

    if (d.itemInst.empty()) return;
    std::vector<u32> toLoad, toEvict;
    d.streamer.update(viewers, toLoad, toEvict);
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point t) { return std::chrono::duration<f64, std::milli>(Clock::now() - t).count(); };

    const auto evictStart = Clock::now();
    u32 evicted = 0;
    for (const u32 item : toEvict) {
        if (evicted > 0 && msSince(evictStart) >= kEvictMs) break;
        if (item >= d.itemInst.size()) continue;
        Impl::Inst* in = d.get(d.itemInst[item]);
        if (!in || !in->loaded || in->pinned || in->editing || in->manual) continue;
        d.evictUnit(*in);
        ++evicted;
    }

    std::vector<u32> now, ready;
    f32 nearestMissing = FLT_MAX;
    const f32 loadCm = d.streamer.settings().loadCm;
    // Every request first, then readiness: a request may trim finished reads, never one just checked.
    if (port_.prefetch) {
        for (u32 item = 0; item < d.itemInst.size(); ++item) {
            Impl::Inst* in = d.itemInst[item] ? d.get(d.itemInst[item]) : nullptr;
            if (!in || in->loaded || in->manual) continue;
            if (d.streamer.distance(item, viewers) > loadCm * kPrefetchFactor && !d.streamer.pinned(item)) continue;
            for (const u64 m : d.needed(*in)) port_.prefetch(m);
        }
    }
    for (const u32 item : toLoad) {
        if (item >= d.itemInst.size()) continue;
        Impl::Inst* in = d.get(d.itemInst[item]);
        if (!in || in->loaded || in->manual) continue;
        if (d.streamer.pinned(item)) { now.push_back(item); continue; }
        const f32 dist = d.streamer.distance(item, viewers);
        bool allReady = true;
        if (port_.ready)
            for (const u64 m : d.needed(*in)) if (!port_.ready(m)) { allReady = false; break; }
        nearestMissing = std::min(nearestMissing, dist);
        if (allReady) ready.push_back(item);
    }
    for (const u32 item : now) {
        Impl::Inst* in = d.get(d.itemInst[item]);
        if (in && !in->loaded) d.loadUnit(*in);
    }

    // The unit being realised: dropped once the viewer has left it behind, else continued first.
    Impl::Inst* cur = d.loadingId ? d.get(d.loadingId) : nullptr;
    if (cur && (!cur->pending || cur->loaded)) { d.loadingId = 0; cur = nullptr; }
    if (cur && cur->streamItem >= 0) {
        const u32 item = static_cast<u32>(cur->streamItem);
        const f32 dist = d.streamer.distance(item, viewers);
        if (dist > d.streamer.settings().evictCm && !d.streamer.pinned(item)) {
            d.unrealise(*cur);
            cur = nullptr;
        } else {
            nearestMissing = std::min(nearestMissing, dist);
        }
    }
    if (!cur && ready.empty()) return;
    const f64 t = std::clamp((static_cast<f64>(nearestMissing) / static_cast<f64>(loadCm) - 0.25) / 0.5, 0.0, 1.0);
    const f64 budgetMs = kNearLoadMs + (kFarLoadMs - kNearLoadMs) * t;
    const auto start = Clock::now();
    usize nextReady = 0;
    for (;;) {
        const f64 left = budgetMs - msSince(start);
        if (left <= 0.0) break;
        if (!cur) {
            while (nextReady < ready.size() && !cur) {
                Impl::Inst* in = d.get(d.itemInst[ready[nextReady++]]);
                if (!in || in->loaded || in->pending) continue;
                if (d.realiseBegin(*in, d.stackFor(*in))) { cur = in; d.loadingId = in->id; }
                else {   // nothing to realise (logged): loaded, empty, as a whole realise leaves it
                    in->loaded = true;
                    if (in->streamItem >= 0) d.streamer.markLoaded(static_cast<u32>(in->streamItem));
                }
            }
            if (!cur) break;
        }
        if (!d.realiseStep(*cur, left)) break;
        d.loadUnit(*cur);   // finishes it: classes, lights, nested, pins, the streamer told
        cur = nullptr;
    }
}

void LevelInstances::spawnPendingClasses() {
    classesReady_ = true;
    for (auto& [id, in] : d_->insts)
        if (in.loaded) d_->spawnClasses(in);
}

void LevelInstances::rescan() {
    Impl& d = *d_;
    if (!world_ || !d.linkType || !lib_) return;
    // The table is rebuilt from the links alone; whatever a save did not carry (bodies, sink rows) is not restored.
    d.insts.clear();
    d.byRoot.clear();
    scene::ComponentPool* pool = world_->pool(d.linkType);
    if (!pool) return;
    u32 maxId = 0;
    struct Row { scene::Entity e; scene::CLevelInstanceLink l; };
    std::vector<Row> rows;
    for (usize i = 0; i < pool->size(); ++i) {
        const auto* l = static_cast<const scene::CLevelInstanceLink*>(pool->dataAt(i));
        rows.push_back(Row{pool->entityAt(i), *l});
        maxId = std::max(maxId, l->instanceId);
    }
    for (const Row& r : rows) {
        if (r.l.root != r.e) continue;
        Impl::Inst& in = d.insts[r.l.instanceId];
        in.id = r.l.instanceId;
        in.root = r.e;
        in.depth = r.l.depth;
        in.rec.asset = lib_->keyForHash(r.l.assetId);
        const Transform& t = world_->localTransform(r.e);
        in.rec.x = t.position.x; in.rec.y = t.position.y; in.rec.z = t.position.z;
        const Vec3 e = world::eulerDegFromQuat(t.rotation);
        in.rec.roll = e.x; in.rec.pitch = e.y; in.rec.yaw = e.z;
        in.rec.sx = t.scale.x; in.rec.sy = t.scale.y; in.rec.sz = t.scale.z;
        in.local = t;
        in.world = transformFromMatrix(world_->worldMatrix(r.e));
        in.asset = in.rec.asset.empty() ? nullptr : lib_->get(in.rec.asset);
        in.loaded = true;
        in.editing = (r.l.flags & scene::kLinkEditing) != 0;
        d.byRoot[r.e] = in.id;
    }
    for (const Row& r : rows) {
        Impl::Inst* in = d.get(r.l.instanceId);
        if (!in) continue;
        if (r.l.root != r.e) in->members.push_back(Impl::Member{r.e, r.l.placementId, -1, true});
        else if (in->depth > 0) {
            // A nested root hangs under its container's root.
            const scene::Entity parent = world_->parent(r.e);
            if (const scene::CLevelInstanceLink* pl = d.link(parent)) {
                in->containerId = pl->instanceId;
                if (Impl::Inst* c = d.get(pl->instanceId)) c->childIds.push_back(in->id);
            }
        }
    }
    d.nextId = std::max(d.nextId, maxId + 1);
    d.begun = true;
}

// ---- run time ----

scene::Entity LevelInstances::spawn(const fmt::OcLevelInstance& rec, scene::Entity parent, bool runtimeOnly) {
    Impl& d = *d_;
    if (!lib_ || !world_) return scene::kInvalidEntity;
    d.begun = true;
    const u32 id = d.createInstance(rec, parent, 0, 0, runtimeOnly, 0);
    if (!id) return scene::kInvalidEntity;
    Impl::Inst& in = d.insts[id];
    d.registerStream(in);
    if (in.streamItem < 0) d.loadUnit(in);
    return in.root;
}

bool LevelInstances::destroy(scene::Entity root) {
    Impl::Inst* in = d_->byRootEntity(root);
    if (!in) return false;
    d_->destroyInstance(in->id);
    return true;
}

scene::Entity LevelInstances::rootOf(scene::Entity e) const {
    Impl& d = *d_;
    if (!world_) return scene::kInvalidEntity;
    for (u32 guard = 0; guard < 256 && e != scene::kInvalidEntity && world_->valid(e); ++guard) {
        if (const scene::CLevelInstanceLink* l = d.link(e)) return l->root;
        e = world_->parent(e);
    }
    return scene::kInvalidEntity;
}

bool LevelInstances::isMember(scene::Entity e) const { return rootOf(e) != scene::kInvalidEntity; }

scene::Entity LevelInstances::topRootOf(scene::Entity e) const {
    scene::Entity r = rootOf(e);
    for (u32 guard = 0; guard < 16 && r != scene::kInvalidEntity; ++guard) {
        const Impl::Inst* in = d_->byRootEntity(r);
        if (!in || in->containerId == 0) break;
        const Impl::Inst* c = d_->get(in->containerId);
        if (!c) break;
        r = c->root;
    }
    return r;
}

scene::Entity LevelInstances::find(scene::Entity root, std::string_view idPath) const {
    Impl& d = *d_;
    const Impl::Inst* in = d.byRootEntity(root);
    std::vector<u64> ids;
    if (!in) return scene::kInvalidEntity;
    if (idPath.empty()) return root;
    if (!fmt::ocLevelInstSplitIds(idPath, ids)) return scene::kInvalidEntity;
    for (usize k = 0; k < ids.size(); ++k) {
        const bool last = k + 1 == ids.size();
        const Impl::Inst* next = nullptr;
        for (const u32 cid : in->childIds) {
            const Impl::Inst* c = d.get(cid);
            if (c && c->rec.id == ids[k]) { next = c; break; }
        }
        if (next) {
            if (last) return next->root;
            in = next;
            continue;
        }
        if (!last) return scene::kInvalidEntity;
        for (const Impl::Member& m : in->members)
            if (m.placementId == ids[k] && world_->valid(m.entity)) return m.entity;
        return scene::kInvalidEntity;
    }
    return scene::kInvalidEntity;
}

bool LevelInstances::setLoaded(scene::Entity root, bool load) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || in->containerId != 0) return false;
    in->manual = true;
    if (load) {
        if (!in->loaded) d.loadUnit(*in);
        d.applyPin(*in);
    } else {
        if (in->loaded) d.evictUnit(*in);
        else if (in->pending) d.unrealise(*in);   // part-realised: nothing to sync back
        d.applyPin(*in);
    }
    return true;
}

bool LevelInstances::loaded(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in && in->loaded;
}

bool LevelInstances::reload(scene::Entity root) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in) return false;
    d.syncOverrides(*in);
    d.rebuild(*in);
    return true;
}

bool LevelInstances::reloadAsset(const std::string& path) {
    Impl& d = *d_;
    if (!lib_) return false;
    d.packs.clear();
    const std::string key = world::LevelAssetLibrary::normalise(path);
    std::vector<u32> hit;
    for (const auto& [id, in] : d.insts) {
        bool match = world::LevelAssetLibrary::normalise(in.rec.asset) == key;
        // A packed top-level instance whose closure contains the asset has a stale pack now.
        if (!match && in.containerId == 0 && in.packedActive) {
            std::vector<std::string> order;
            lib_->closure(in.rec.asset, order, nullptr);
            for (const std::string& o : order) if (world::LevelAssetLibrary::normalise(o) == key) { match = true; break; }
        }
        if (match) hit.push_back(id);
    }
    bool any = false;
    for (const u32 id : hit) {
        Impl::Inst* in = d.get(id);   // a container's rebuild may already have replaced a nested one
        if (!in) continue;
        in->needKnown = false;
        d.rebuild(*in);
        any = true;
    }
    return any;
}

bool LevelInstances::setPacked(scene::Entity root, bool packed) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || in->containerId != 0) return false;
    d.cancelPending(*in);
    in->rec.packed = packed;
    if (in->loaded) {
        d.syncOverrides(*in);
        d.rebuild(*in);
    }
    return in->packedActive == packed;
}

bool LevelInstances::bounds(scene::Entity root, AABB& out) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    if (!in || !in->hasBounds) return false;
    out = in->bounds;
    return true;
}

u32 LevelInstances::count() const { return static_cast<u32>(d_->insts.size()); }

scene::Entity LevelInstances::at(u32 i) const {
    if (i >= d_->insts.size()) return scene::kInvalidEntity;
    auto it = d_->insts.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(i));
    return it->second.root;
}

// ---- editor ----

bool LevelInstances::beginEdit(scene::Entity root) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || in->containerId != 0 || in->editing) return false;
    for (const auto& [id, o] : d.insts) if (o.editing) return false;   // one at a time
    d.cancelPending(*in);
    if (in->loaded && !in->packedActive) d.syncOverrides(*in);
    in->savedOverrides = in->rec.overrides;
    in->rec.overrides.clear();
    in->editing = true;
    if (in->loaded) d.rebuild(*in);
    else d.loadUnit(*in);
    d.applyPin(*in);
    return true;
}

bool LevelInstances::discardEdit(scene::Entity root) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || !in->editing) return false;
    in->editing = false;
    in->rec.overrides = in->savedOverrides;
    in->savedOverrides.clear();
    d.rebuild(*in);
    d.applyPin(*in);
    return true;
}

bool LevelInstances::editing(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in && in->editing;
}

bool LevelInstances::anyDirtyEdit() const {
    for (const auto& [id, in] : d_->insts) if (in.editing) return true;
    return false;
}

bool LevelInstances::commitEdit(scene::Entity root, std::string* why) {
    Impl& d = *d_;
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || !in->editing) return fail("that instance is not being edited");
    if (!d.hooks.captureEntity) return fail("the host cannot capture entities");
    if (!lib_) return fail("no level library");
    const world::LevelAsset* asset = lib_->get(in->rec.asset);
    if (!asset) return fail("the child level is not loaded");
    if (port_.contentDir.empty()) return fail("no content directory to write the level to");

    fmt::OcWorldData out = asset->data;   // header records and everything an instance ignores ride through untouched
    out.placements.clear();
    out.lights.clear();
    out.decals.clear();
    out.prefabInstances.clear();
    out.levelInstances.clear();

    // Hierarchy order: the host appends records; this fills the parent index and keeps ids from the links.
    std::function<void(scene::Entity, i32)> visit = [&](scene::Entity e, i32 parentIdx) {
        if (world_->destroyPending(e)) return;   // members of an earlier rebuild stay linked until the next flush
        if (const scene::CLevelInstanceLink* l = d.link(e); l && l->root == e && l->instanceId != in->id) return;   // a nested instance: captured below
        const usize before = out.placements.size();
        const bool took = d.hooks.captureEntity(e, out, parentIdx);
        i32 myIdx = parentIdx;
        if (took && out.placements.size() > before) {
            myIdx = static_cast<i32>(before);
            fmt::OcWorldPlacement& p = out.placements[before];
            p.parent = parentIdx;
            if (const scene::CLevelInstanceLink* l = d.link(e); l && l->instanceId == in->id) p.placementId = l->placementId;
        }
        for (scene::Entity c = world_->firstChild(e); c != scene::kInvalidEntity; c = world_->nextSibling(c)) visit(c, myIdx);
    };
    for (scene::Entity c = world_->firstChild(in->root); c != scene::kInvalidEntity; c = world_->nextSibling(c)) visit(c, -1);
    fmt::assignPlacementIds(out);
    for (const u32 cid : in->childIds) {
        Impl::Inst* c = d.get(cid);
        if (!c) continue;
        fmt::OcLevelInstance r = d.recordOfInst(*c);
        out.levelInstances.push_back(std::move(r));
    }
    if (d.hooks.capturePrefabs) out.prefabInstances = d.hooks.capturePrefabs(in->root);

    // EXTENT: the mesh boxes of the captured placements through their composed poses, plus the nested instances' bounds.
    {
        AABB acc;
        bool any = false;
        std::vector<Transform> wx(out.placements.size());
        for (usize i = 0; i < out.placements.size(); ++i) {
            const fmt::OcWorldPlacement& p = out.placements[i];
            const Transform xf = world::levelPlacementTransform(p);
            wx[i] = (p.parent >= 0 && static_cast<usize>(p.parent) < i) ? compose(wx[static_cast<usize>(p.parent)], xf) : xf;
            Vec3 lo, hi;
            if (d.opt.localBoundsFor && d.opt.localBoundsFor(p.objectId, lo, hi)) {
                AABB b;
                b.min = lo;
                b.max = hi;
                unionBox(acc, any, transformBox(b, wx[i]));
            } else {
                unionBox(acc, any, AABB{wx[i].position, wx[i].position});
            }
        }
        for (const fmt::OcLevelInstance& r : out.levelInstances) {
            AABB b;
            if (lib_->instanceBounds(r, b)) unionBox(acc, any, b);
        }
        if (any) {
            out.hasExtent = true;
            out.extentMin[0] = acc.min.x; out.extentMin[1] = acc.min.y; out.extentMin[2] = acc.min.z;
            out.extentMax[0] = acc.max.x; out.extentMax[1] = acc.max.y; out.extentMax[2] = acc.max.z;
        }
    }

    std::string err;
    const std::string file = (std::filesystem::path(port_.contentDir) / in->rec.asset).string();
    if (!fmt::saveOcworld(file, out, &err)) return fail(err);

    // Every other instance of the asset folds its live edits into its record before the library changes under it.
    const std::string key = world::LevelAssetLibrary::normalise(in->rec.asset);
    for (auto& [id, o] : d.insts)
        if (o.id != in->id && o.loaded && world::LevelAssetLibrary::normalise(o.rec.asset) == key) d.syncOverrides(o);
    const std::string assetPath = in->rec.asset;
    lib_->put(assetPath, std::move(out));
    in->editing = false;
    in->rec.overrides = in->savedOverrides;
    in->savedOverrides.clear();
    d.applyPin(*in);
    reloadAsset(assetPath);
    return true;
}

bool LevelInstances::computeOverrides(scene::Entity root, std::vector<fmt::OcPrefabOverride>& out) const {
    Impl::Inst* in = d_->byRootEntity(root);
    if (!in) return false;
    return d_->computeOverridesOf(*in, out);
}

bool LevelInstances::setOverrides(scene::Entity root, const std::vector<fmt::OcPrefabOverride>& ov) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in) return false;
    if (in->editing) { in->savedOverrides = ov; return true; }
    d.cancelPending(*in);
    in->rec.overrides = ov;
    if (in->loaded) d.rebuild(*in);
    return true;
}

bool LevelInstances::revertOverride(scene::Entity root, const fmt::OcPrefabOverride& which) {
    std::vector<fmt::OcPrefabOverride> ov;
    if (!computeOverrides(root, ov)) return false;
    ov.erase(std::remove_if(ov.begin(), ov.end(), [&](const fmt::OcPrefabOverride& o) { return fmt::ocPrefabSameTarget(o, which); }), ov.end());
    return setOverrides(root, ov);
}

bool LevelInstances::revertAll(scene::Entity root) { return setOverrides(root, {}); }

std::vector<fmt::OcLevelInstance> LevelInstances::captureRecords(const std::function<bool(scene::Entity)>& include) const {
    Impl& d = *d_;
    std::vector<fmt::OcLevelInstance> out;
    for (auto& [id, in] : d.insts) {
        if (in.containerId != 0 || in.runtimeOnly) continue;
        if (include && !include(in.root)) continue;
        out.push_back(d.recordOfInst(in));
    }
    return out;
}

scene::Entity LevelInstances::createFromRecord(const fmt::OcLevelInstance& rec, scene::Entity parent, std::string* why) {
    Impl& d = *d_;
    if (!lib_ || !world_) { if (why) *why = "level instances are not initialised"; return scene::kInvalidEntity; }
    std::string w;
    if (!lib_->get(rec.asset, &w)) { if (why) *why = w; return scene::kInvalidEntity; }
    d.begun = true;
    fmt::OcLevelInstance r = rec;
    if (!r.id) r.id = fnv1a64(std::string_view(r.asset + "#new" + std::to_string(d.nextId)));
    const u32 id = d.createInstance(r, parent, 0, 0, false, 0);
    if (!id) { if (why) *why = "the world refused the instance root"; return scene::kInvalidEntity; }
    Impl::Inst& in = d.insts[id];
    d.registerStream(in);
    if (in.streamItem < 0) d.loadUnit(in);
    return in.root;
}

bool LevelInstances::breakInstance(scene::Entity root) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || !in->loaded) return false;
    if (in->packedActive) {   // packed rows have no entities to keep: unpack first
        in->rec.packed = false;
        d.rebuild(*in);
    }
    const scene::Entity newParent = world_->parent(root);
    // Nested instances break first, so their entities are plain by the time the container's are reparented.
    const std::vector<u32> kids = in->childIds;
    for (const u32 cid : kids) if (Impl::Inst* c = d.get(cid)) breakInstance(c->root);
    // Direct children of the root go up one level, keeping world pose; links come off everything under it.
    std::vector<scene::Entity> all;
    d.collectDescendants(root, all);
    for (const scene::Entity e : all) world_->removeComponent(e, d.linkType);
    for (scene::Entity c = world_->firstChild(root); c != scene::kInvalidEntity;) {
        const scene::Entity next = world_->nextSibling(c);
        world_->setParent(c, newParent, true);
        c = next;
    }
    // What the instance held (meshes, shapes) stays held for the entities that remain until end().
    d.orphanMeshes.insert(d.orphanMeshes.end(), in->meshes.begin(), in->meshes.end());
    in->meshes.clear();
    in->members.clear();
    in->bodies.clear();
    in->extras.clear();
    in->classEntities.clear();
    in->animated.clear();
    in->loaded = false;
    d.rebuildAnimated();
    d.destroyInstance(in->id);
    return true;
}

LevelInstances::LevelInstanceState LevelInstances::captureState(const std::vector<u32>& instanceIds) {
    Impl& d = *d_;
    LevelInstanceState st;
    for (const u32 id : instanceIds) {
        LevelInstanceState::Item item;
        item.instanceId = id;
        Impl::Inst* in = d.get(id);
        if (!in) { item.present = false; st.items.push_back(std::move(item)); continue; }
        item.rec = d.recordOfInst(*in);
        item.parent = world_->valid(in->root) ? world_->parent(in->root) : scene::kInvalidEntity;
        st.items.push_back(std::move(item));
    }
    return st;
}

bool LevelInstances::restoreState(const LevelInstanceState& st) {
    Impl& d = *d_;
    bool ok = true;
    for (const LevelInstanceState::Item& item : st.items) {
        Impl::Inst* in = d.get(item.instanceId);
        if (!item.present) {
            if (in) d.destroyInstance(item.instanceId);
            continue;
        }
        if (in) {
            d.cancelPending(*in);
            in->rec = item.rec;
            in->local = world::levelInstanceTransform(item.rec);
            world_->setLocalTransform(in->root, in->local);
            if (in->loaded) d.rebuild(*in);
            continue;
        }
        d.begun = true;
        const u32 id = d.createInstance(item.rec, item.parent, 0, 0, false, item.instanceId);
        if (!id) { ok = false; continue; }
        Impl::Inst& n = d.insts[id];
        d.registerStream(n);
        if (n.streamItem < 0) d.loadUnit(n);
    }
    return ok;
}

void LevelInstances::pin(scene::Entity root, bool on) {
    Impl::Inst* in = d_->byRootEntity(root);
    if (!in) return;
    in->pinned = on;
    d_->applyPin(*in);
}

const std::vector<world::AnimatedBody>& LevelInstances::animatedBodies() const { return d_->animated; }

bool LevelInstances::extentBounds(Vec3& lo, Vec3& hi) const {
    bool any = false;
    AABB acc;
    for (const auto& [id, in] : d_->insts)
        if (in.containerId == 0 && in.hasBounds) unionBox(acc, any, in.bounds);
    if (!any) return false;
    lo = acc.min;
    hi = acc.max;
    return true;
}

void LevelInstances::setPackedRenderOk(bool ok) {
    Impl& d = *d_;
    if (d.packedOk == ok) return;
    d.packedOk = ok;
    d.packs.clear();
    d.packedQueue.clear();
    for (auto& [id, in] : d.insts) {
        if (in.containerId == 0 && in.rec.packed) d.cancelPending(in);
        if (in.containerId == 0 && in.rec.packed && in.loaded) d.packedQueue.push_back(id);
    }
    std::reverse(d.packedQueue.begin(), d.packedQueue.end());   // popped from the back, so the oldest first
}

LevelInstances::Stats LevelInstances::stats() const {
    Stats s{0, 0, 0, 0, 0};
    for (const auto& [id, in] : d_->insts) {
        ++s.instances;
        if (in.loaded) ++s.loaded;
        s.entities += in.entityCount;
        s.bodies += static_cast<u32>(in.bodies.size() + in.packedBodies.size());
        s.packedRows += in.packedRows;
    }
    return s;
}

// ---- accessors ----

u32 LevelInstances::instanceIdOf(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in ? in->id : 0;
}

scene::Entity LevelInstances::rootOfId(u32 instanceId) const {
    const Impl::Inst* in = d_->get(instanceId);
    return in ? in->root : scene::kInvalidEntity;
}

const fmt::OcLevelInstance* LevelInstances::recordOf(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in ? &in->rec : nullptr;
}

bool LevelInstances::setRecordName(scene::Entity root, const std::string& name) {
    Impl::Inst* in = d_->byRootEntity(root);
    if (!in) return false;
    in->rec.name = name;
    if (world_->valid(root)) world_->setName(root, name.empty() ? stemOf(in->rec.asset) : name);
    return true;
}

bool LevelInstances::setRecordStream(scene::Entity root, i32 stream) {
    Impl& d = *d_;
    Impl::Inst* in = d.byRootEntity(root);
    if (!in || in->containerId != 0) return false;
    in->rec.stream = stream < 0 ? -1 : (stream ? 1 : 0);
    // Stream membership changes apply at the next load; a resident unit stays resident until then.
    if (!d.wantsStream(*in) && in->streamItem >= 0) {
        d.streamer.remove(static_cast<u32>(in->streamItem));
        d.itemInst[static_cast<usize>(in->streamItem)] = 0;
        in->streamItem = -1;
        if (!in->loaded) d.loadUnit(*in);
    } else if (d.wantsStream(*in) && in->streamItem < 0) {
        d.registerStream(*in);
        if (in->loaded && in->streamItem >= 0) d.streamer.markLoaded(static_cast<u32>(in->streamItem));
    }
    return true;
}

u32 LevelInstances::assetCount(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    if (!in) return 0;
    const world::LevelAsset* a = lib_ ? lib_->get(in->rec.asset) : nullptr;
    return a ? static_cast<u32>(a->data.placements.size()) : 0;
}

std::string LevelInstances::assetOf(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in ? in->rec.asset : std::string();
}

u64 LevelInstances::assetIdOf(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in ? assetHashOf(in->rec.asset) : 0;
}

std::string LevelInstances::packState(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    if (!in || !in->rec.packed) return {};
    if (in->packedActive) return "packed";
    return in->packWhy.empty() ? std::string("unpacked") : in->packWhy;
}

bool LevelInstances::isRuntimeOnly(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in && in->runtimeOnly;
}

bool LevelInstances::isNested(scene::Entity root) const {
    const Impl::Inst* in = d_->byRootEntity(root);
    return in && in->containerId != 0;
}

void LevelInstances::setMemberCollide(scene::Entity member, bool collide) {
    Impl& d = *d_;
    const scene::CLevelInstanceLink* l = d.link(member);
    if (!l) return;
    Impl::Inst* in = d.get(l->instanceId);
    if (!in) return;
    for (Impl::Member& m : in->members) if (m.entity == member) { m.collide = collide; return; }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
