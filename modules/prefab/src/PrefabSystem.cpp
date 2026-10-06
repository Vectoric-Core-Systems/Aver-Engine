// PrefabSystem: queries, spawning, the diff that yields overrides, and realize(), the one routine
// that builds or updates an instance in place. Authoring, apply, undo state and levels are in
// PrefabEdit.cpp.
#include "aver/prefab/PrefabSystem.hpp"

#include "PrefabFields.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <unordered_set>

namespace aver::prefab {
using scene::Entity;
using scene::kInvalidEntity;

namespace {

u64 pathHash(const std::string& p) { return fnv1a64(std::string_view(p)); }

const fmt::OcSaveComponent* findComp(const std::vector<fmt::OcSaveComponent>& cs, const std::string& type) {
    for (const auto& c : cs) if (c.type == type) return &c;
    return nullptr;
}

const fmt::OcSaveField* findField(const fmt::OcSaveComponent& c, const std::string& name) {
    for (const auto& f : c.fields) if (f.name == name) return &f;
    return nullptr;
}

// What an absent CLocal field means: the identity transform, not zero (a zero scale is a collapse).
fmt::OcSaveField defaultFor(const scene::FieldDesc& d, const char* component) {
    fmt::OcSaveField z = detail::zeroField(d);
    if (std::string_view(component) == "CLocal") {
        if (z.name == "rotation" && z.f.size() == 4) z.f[3] = 1.0f;
        if (z.name == "scale" && z.f.size() == 3) z.f[0] = z.f[1] = z.f[2] = 1.0f;
    }
    return z;
}

} // namespace

struct PrefabSystem::Live {
    std::unordered_map<u64, Entity> byHash;   // path hash -> entity
    std::unordered_map<u32, u64> hashOf;      // entity -> path hash
    std::vector<Entity> order;                // parents first
};

PrefabSystem::PrefabSystem(scene::World& world, PrefabLibrary& library) : world_(world), lib_(library) {
    linkType_ = scene::registerPrefabLink(world_);
    if (!linkType_) AVER_ERROR("[Prefab] could not register CPrefabLink; prefab instances are unavailable");
    // Continue past ids already in the world (a restored save, a second system over the same world).
    if (scene::ComponentPool* p = linkType_ ? world_.pool(linkType_) : nullptr)
        for (usize i = 0; i < p->size(); ++i) {
            const auto* lk = static_cast<const scene::CPrefabLink*>(p->dataAt(i));
            nextInstanceId_ = std::max(nextInstanceId_, lk->instanceId + 1);
        }
}

// ---- queries -----------------------------------------------------------------------------------------

bool PrefabSystem::isLinked(Entity e) const { return linkType_ && world_.hasComponent(e, linkType_); }

Entity PrefabSystem::instanceRoot(Entity e) const {
    if (!linkType_) return kInvalidEntity;
    const auto* lk = world_.component<scene::CPrefabLink>(e, linkType_);
    return lk && world_.valid(lk->root) ? lk->root : kInvalidEntity;
}

bool PrefabSystem::isInstanceRoot(Entity e) const {
    const auto* lk = linkType_ ? world_.component<scene::CPrefabLink>(e, linkType_) : nullptr;
    return lk && lk->root == e;
}

u32 PrefabSystem::instanceIdOf(Entity e) const {
    const auto* lk = linkType_ ? world_.component<scene::CPrefabLink>(e, linkType_) : nullptr;
    return lk ? lk->instanceId : 0;
}

std::vector<Entity> PrefabSystem::instanceRoots() const {
    std::vector<Entity> out;
    scene::ComponentPool* p = linkType_ ? world_.pool(linkType_) : nullptr;
    if (!p) return out;
    for (usize i = 0; i < p->size(); ++i) {
        const Entity e = p->entityAt(i);
        const auto* lk = static_cast<const scene::CPrefabLink*>(p->dataAt(i));
        if (lk->root == e && world_.valid(e) && !world_.destroyPending(e)) out.push_back(e);
    }
    return out;
}

Entity PrefabSystem::rootOfInstance(u32 instanceId) const {
    for (const Entity r : instanceRoots())
        if (instanceIdOf(r) == instanceId) return r;
    return kInvalidEntity;
}

std::string PrefabSystem::prefabPathOf(Entity e) const {
    const Entity root = instanceRoot(e);
    if (root == kInvalidEntity) return {};
    const auto* lk = world_.component<scene::CPrefabLink>(root, linkType_);
    return lk ? lib_.pathOfId(lk->prefabId) : std::string();
}

bool PrefabSystem::resolveInstance(Entity root, Resolved& out, std::string* why) {
    const std::string pp = prefabPathOf(root);
    if (pp.empty()) {
        if (why) *why = "the instance's prefab asset path is unknown";
        return false;
    }
    return lib_.resolve(pp, out, why);
}

bool PrefabSystem::nodePathOf(Entity e, std::string& path) {
    const auto* lk = linkType_ ? world_.component<scene::CPrefabLink>(e, linkType_) : nullptr;
    if (!lk) return false;
    Resolved base;
    if (!resolveInstance(lk->root, base, nullptr)) return false;
    for (const ResolvedNode& n : base.nodes)
        if (pathHash(n.path) == lk->pathHash) { path = n.path; return true; }
    return false;
}

std::vector<Entity> PrefabSystem::instancesOf(const std::string& prefabPath, bool includeNested) {
    const std::string want = PrefabLibrary::normalize(prefabPath);
    std::vector<Entity> out;
    for (const Entity r : instanceRoots()) {
        const std::string pp = prefabPathOf(r);
        if (pp.empty()) continue;
        if (pp == want || (includeNested && lib_.references(pp, want))) out.push_back(r);
    }
    return out;
}

PrefabSystem::Live PrefabSystem::liveOf(Entity root) const {
    Live l;
    if (!world_.valid(root)) return l;
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        if (!world_.valid(e) || world_.destroyPending(e)) continue;
        const auto* lk = world_.component<scene::CPrefabLink>(e, linkType_);
        if (lk && lk->root == root) {
            l.byHash[lk->pathHash] = e;
            l.hashOf[static_cast<u32>(e)] = lk->pathHash;
            l.order.push_back(e);
        }
        for (Entity c = world_.firstChild(e); c != kInvalidEntity; c = world_.nextSibling(c)) stack.push_back(c);
    }
    return l;
}

std::vector<Entity> PrefabSystem::instanceEntities(Entity root) const { return liveOf(root).order; }

Entity PrefabSystem::entityAtPath(Entity root, const std::string& path) const {
    const Live l = liveOf(root);
    const auto it = l.byHash.find(pathHash(path));
    return it == l.byHash.end() ? kInvalidEntity : it->second;
}

// ---- the diff ------------------------------------------------------------------------------------------

bool PrefabSystem::diffAgainst(Entity root, const Resolved& base, std::vector<fmt::OcPrefabOverride>& out) {
    out.clear();
    const Live live = liveOf(root);

    std::unordered_map<u64, std::string> pathByHash;
    for (const ResolvedNode& n : base.nodes) pathByHash[pathHash(n.path)] = n.path;
    const detail::PathLookup lookup = [&](Entity t, std::string& path) {
        const auto h = live.hashOf.find(static_cast<u32>(t));
        if (h == live.hashOf.end()) return false;
        const auto n = pathByHash.find(h->second);
        if (n == pathByHash.end()) return false;
        path = n->second;
        return true;
    };

    for (const ResolvedNode& node : base.nodes) {
        const auto it = live.byHash.find(pathHash(node.path));
        if (it == live.byHash.end()) continue;   // a deleted linked entity is not an override (v1)
        const Entity e = it->second;

        if (!node.isRoot && !node.name.empty() && node.name != world_.name(e)) {
            fmt::OcPrefabOverride ov;
            ov.path = node.path;
            ov.component = fmt::kOcPrefabNodeComponent;
            ov.value.name = "name";
            ov.value.kind = fmt::kOcPrefabKindString;
            ov.value.s = world_.name(e);
            out.push_back(std::move(ov));
        }

        for (u32 ci = 0; ci < world_.componentCount(); ++ci) {
            const u32 type = world_.componentAt(ci);
            if (detail::excludedComponent(type, linkType_) || !world_.hasComponent(e, type)) continue;
            if (node.isRoot && type == scene::kComponentLocal) continue;   // the instance's own placement
            fmt::OcSaveComponent liveC;
            if (!detail::readComponent(world_, e, type, lookup, liveC)) continue;
            const char* cname = world_.componentName(type);
            const fmt::OcSaveComponent* baseC = findComp(node.components, liveC.type);

            if (!baseC && type != scene::kComponentLocal) {
                fmt::OcPrefabOverride ov;
                ov.op = fmt::OcOverrideOp::AddComponent;
                ov.path = node.path;
                ov.component = liveC.type;
                out.push_back(std::move(ov));
            }
            for (const fmt::OcSaveField& lf : liveC.fields) {
                const fmt::OcSaveField* bf = baseC ? findField(*baseC, lf.name) : nullptr;
                bool differs;
                if (bf) {
                    differs = !fmt::ocPrefabFieldEqual(*bf, lf);
                } else {
                    const scene::FieldDesc* d = world_.field(world_.fieldId(std::string(cname) + "." + lf.name));
                    differs = !d || !fmt::ocPrefabFieldEqual(defaultFor(*d, cname), lf);
                }
                if (!differs) continue;
                fmt::OcPrefabOverride ov;
                ov.op = fmt::OcOverrideOp::Set;
                ov.path = node.path;
                ov.component = liveC.type;
                ov.value = lf;
                out.push_back(std::move(ov));
            }
        }

        for (const fmt::OcSaveComponent& bc : node.components) {
            const u32 type = world_.componentId(bc.type);
            if (type == 0 || type == scene::kComponentLocal || detail::excludedComponent(type, linkType_)) continue;
            if (world_.hasComponent(e, type)) continue;
            fmt::OcPrefabOverride ov;
            ov.op = fmt::OcOverrideOp::RemoveComponent;
            ov.path = node.path;
            ov.component = bc.type;
            out.push_back(std::move(ov));
        }
    }
    return true;
}

bool PrefabSystem::computeOverrides(Entity root, std::vector<fmt::OcPrefabOverride>& out, std::string* why) {
    out.clear();
    if (!isInstanceRoot(root)) { if (why) *why = "not an instance root"; return false; }
    Resolved base;
    if (!resolveInstance(root, base, why)) return false;
    return diffAgainst(root, base, out);
}

// ---- realize ---------------------------------------------------------------------------------------------

Entity PrefabSystem::realize(Entity existingRoot, const Resolved& base,
                             const std::vector<fmt::OcPrefabOverride>& overrides, const Transform* rootLocal,
                             Entity parent, const std::string& rootName, u32 instanceId) {
    std::vector<ResolvedNode> nodes = base.nodes;
    std::vector<std::string> warnings = base.warnings;
    applyOverrides(nodes, overrides, std::string(), &warnings);
    for (const std::string& w : warnings) AVER_WARN("[Prefab] {}: {}", base.prefab, w);

    const bool existing = world_.valid(existingRoot);
    if (existing) {
        if (const auto* lk = world_.component<scene::CPrefabLink>(existingRoot, linkType_)) instanceId = lk->instanceId;
    }
    const Live live = existing ? liveOf(existingRoot) : Live{};
    Entity root = existing ? existingRoot : kInvalidEntity;
    const u64 prefabId = PrefabLibrary::idOf(base.prefab);

    std::unordered_map<std::string, Entity> byPath;
    std::vector<Entity> created;
    std::vector<std::pair<Entity, std::string>> classes;
    std::unordered_set<u32> changedSet;
    struct Pending { Entity e; u32 fid; const fmt::OcSaveField* f; };
    std::vector<Pending> pending;
    std::unordered_set<u64> wanted;

    for (const ResolvedNode& node : nodes) {
        const u64 h = pathHash(node.path);
        wanted.insert(h);
        Entity e = kInvalidEntity;
        if (node.isRoot && existing) e = existingRoot;
        else if (const auto it = live.byHash.find(h); it != live.byHash.end()) e = it->second;

        if (e == kInvalidEntity) {
            Entity parentEnt = kInvalidEntity;
            if (node.isRoot) {
                parentEnt = parent;
            } else {
                const auto pit = byPath.find(node.parentPath);
                if (pit == byPath.end()) {
                    AVER_WARN("[Prefab] {}: node '{}' has no parent '{}' -- skipped", base.prefab, node.path, node.parentPath);
                    continue;
                }
                parentEnt = pit->second;
            }
            const std::string nm = node.isRoot && !rootName.empty() ? rootName
                                 : node.name.empty() ? std::string("Entity") : node.name;
            const Transform xf = node.isRoot && rootLocal ? *rootLocal : Transform{};
            e = world_.create(nm, parentEnt, xf);
            if (e == kInvalidEntity) { AVER_WARN("[Prefab] the world refused a new entity for '{}'", nm); continue; }
            if (node.isRoot) root = e;
            auto* lk = static_cast<scene::CPrefabLink*>(world_.addComponent(e, linkType_));
            lk->prefabId = prefabId;
            lk->pathHash = h;
            lk->root = node.isRoot ? e : root;
            lk->instanceId = instanceId;
            created.push_back(e);
            if (!node.className.empty()) classes.emplace_back(e, node.className);
        } else if (!node.isRoot) {
            const auto pit = byPath.find(node.parentPath);
            if (pit != byPath.end() && world_.parent(e) != pit->second && world_.setParent(e, pit->second))
                changedSet.insert(static_cast<u32>(e));
            if (!node.name.empty() && node.name != world_.name(e)) {
                world_.setName(e, node.name);
                changedSet.insert(static_cast<u32>(e));
            }
        }
        byPath[node.path] = e;

        for (const fmt::OcSaveComponent& comp : node.components) {
            const u32 type = world_.componentId(comp.type);
            if (type == 0) { AVER_WARN("[Prefab] {}: component '{}' is not registered in this build", base.prefab, comp.type); continue; }
            if (detail::excludedComponent(type, linkType_)) continue;
            if (node.isRoot && type == scene::kComponentLocal) continue;
            if (!world_.hasComponent(e, type)) {
                if (!world_.addComponent(e, type)) continue;
                changedSet.insert(static_cast<u32>(e));
            }
            for (const fmt::OcSaveField& f : comp.fields) {
                const u32 fid = world_.fieldId(comp.type + "." + f.name);
                if (fid == 0) continue;
                if (f.kind == fmt::kOcPrefabKindEntity) { pending.push_back({e, fid, &f}); continue; }
                if (detail::writeField(world_, e, fid, f, {}) == detail::Write::Changed)
                    changedSet.insert(static_cast<u32>(e));
            }
        }
    }

    // Entity references can name a node later in the list, so they wait until every entity exists.
    const detail::EntityLookup lookup = [&](const std::string& p) {
        const auto it = byPath.find(p);
        return it == byPath.end() ? kInvalidEntity : it->second;
    };
    for (const Pending& p : pending)
        if (detail::writeField(world_, p.e, p.fid, *p.f, lookup) == detail::Write::Changed)
            changedSet.insert(static_cast<u32>(p.e));

    // A component the prefab (with overrides) no longer has. CLocal is never removed.
    for (const ResolvedNode& node : nodes) {
        const auto it = byPath.find(node.path);
        if (it == byPath.end()) continue;
        const Entity e = it->second;
        for (u32 ci = 0; ci < world_.componentCount(); ++ci) {
            const u32 type = world_.componentAt(ci);
            if (detail::excludedComponent(type, linkType_) || type == scene::kComponentLocal) continue;
            if (!world_.hasComponent(e, type) || findComp(node.components, world_.componentName(type))) continue;
            world_.removeComponent(e, type);
            changedSet.insert(static_cast<u32>(e));
        }
    }

    // Nodes the prefab dropped. Whatever the user parented under them that the prefab never knew about
    // moves to the instance root first, so it survives.
    for (const Entity de : live.order) {
        const auto h = live.hashOf.find(static_cast<u32>(de));
        if (h == live.hashOf.end() || wanted.count(h->second)) continue;
        if (!world_.valid(de) || world_.destroyPending(de)) continue;
        std::vector<Entity> kids;
        for (Entity c = world_.firstChild(de); c != kInvalidEntity; c = world_.nextSibling(c)) kids.push_back(c);
        for (const Entity c : kids) {
            const auto* lk = world_.component<scene::CPrefabLink>(c, linkType_);
            if (lk && lk->root == root) continue;   // a linked node has its own entry in live.order
            world_.setParent(c, root, true);
        }
        if (hooks_.destroying) hooks_.destroying(de);
        world_.destroy(de);
    }

    if (hooks_.classBound)
        for (const auto& c : classes) hooks_.classBound(c.first, c.second);
    if (hooks_.created) for (const Entity e : created) hooks_.created(e);
    if (hooks_.changed) {
        const std::unordered_set<u32> fresh(created.begin(), created.end());
        for (const u32 e : changedSet)
            if (!fresh.count(e) && world_.valid(static_cast<Entity>(e))) hooks_.changed(static_cast<Entity>(e));
    }
    return root;
}

// ---- lifecycle ------------------------------------------------------------------------------------------

Entity PrefabSystem::instantiateWithId(const std::string& prefabPath, const Transform& local, Entity parent,
                                       const std::vector<fmt::OcPrefabOverride>& overrides,
                                       const std::string& name, u32 instanceId, std::string* why) {
    if (!linkType_) { if (why) *why = "CPrefabLink is not registered"; return kInvalidEntity; }
    Resolved base;
    if (!lib_.resolve(prefabPath, base, why)) return kInvalidEntity;
    nextInstanceId_ = std::max(nextInstanceId_, instanceId + 1);
    const Entity root = realize(kInvalidEntity, base, overrides, &local, parent, name, instanceId);
    if (root == kInvalidEntity && why) *why = "the world refused the instance's root entity";
    return root;
}

Entity PrefabSystem::instantiate(const std::string& prefabPath, const Transform& local, Entity parent,
                                 const std::vector<fmt::OcPrefabOverride>& overrides, const std::string& name,
                                 std::string* why) {
    return instantiateWithId(prefabPath, local, parent, overrides, name, nextInstanceId_, why);
}

void PrefabSystem::destroyTree(Entity e) {
    if (!world_.valid(e) || world_.destroyPending(e)) return;
    if (hooks_.destroying) {
        std::vector<Entity> stack{e};
        while (!stack.empty()) {
            const Entity c = stack.back();
            stack.pop_back();
            hooks_.destroying(c);
            for (Entity k = world_.firstChild(c); k != kInvalidEntity; k = world_.nextSibling(k)) stack.push_back(k);
        }
    }
    world_.destroy(e);
}

bool PrefabSystem::destroyInstance(Entity root) {
    if (!isInstanceRoot(root)) return false;
    destroyTree(root);
    return true;
}

bool PrefabSystem::unlink(Entity root) {
    if (!isInstanceRoot(root)) return false;
    for (const Entity e : liveOf(root).order) world_.removeComponent(e, linkType_);
    return true;
}

// ---- overrides --------------------------------------------------------------------------------------------

bool PrefabSystem::setOverrides(Entity root, const std::vector<fmt::OcPrefabOverride>& overrides, std::string* why) {
    if (!isInstanceRoot(root)) { if (why) *why = "not an instance root"; return false; }
    Resolved base;
    if (!resolveInstance(root, base, why)) return false;
    return realize(root, base, overrides, nullptr, kInvalidEntity, std::string(), 0) != kInvalidEntity;
}

bool PrefabSystem::revertAll(Entity root) { return setOverrides(root, {}); }

bool PrefabSystem::revertOverride(Entity root, const fmt::OcPrefabOverride& which) {
    std::vector<fmt::OcPrefabOverride> ov;
    if (!computeOverrides(root, ov)) return false;
    ov.erase(std::remove_if(ov.begin(), ov.end(), [&](const fmt::OcPrefabOverride& o) {
                 if (fmt::ocPrefabSameTarget(o, which)) return true;
                 // Reverting an added component takes its field values with it.
                 return which.op == fmt::OcOverrideOp::AddComponent && o.path == which.path &&
                        o.component == which.component;
             }), ov.end());
    return setOverrides(root, ov);
}

bool PrefabSystem::revertNode(Entity e) {
    const Entity root = instanceRoot(e);
    std::string path;
    if (root == kInvalidEntity || !nodePathOf(e, path)) return false;
    std::vector<fmt::OcPrefabOverride> ov;
    if (!computeOverrides(root, ov)) return false;
    ov.erase(std::remove_if(ov.begin(), ov.end(), [&](const fmt::OcPrefabOverride& o) { return o.path == path; }),
             ov.end());
    return setOverrides(root, ov);
}

} // namespace aver::prefab
