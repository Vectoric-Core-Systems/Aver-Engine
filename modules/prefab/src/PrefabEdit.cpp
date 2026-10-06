// PrefabSystem, the editing half: folding overrides into a prefab, propagation, capturing a subtree as
// a prefab, undo state, and the level records.
#include "aver/prefab/PrefabSystem.hpp"

#include "PrefabFields.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/world/LevelTransform.hpp"   // header-only, Core-only: the .ocworld rotation contract

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace aver::prefab {
using scene::Entity;
using scene::kInvalidEntity;

namespace {

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// An Entity override value as a uid inside `pd`. False when it names something a plain node cannot.
bool pathToUid(const fmt::OcPrefabData& pd, const std::string& path, u32& uid) {
    if (path.empty()) { uid = pd.rootUid(); return true; }
    if (!allDigits(path)) return false;
    uid = static_cast<u32>(std::stoul(path));
    return pd.find(uid) != nullptr;
}

void replaceOrAppend(std::vector<fmt::OcPrefabOverride>& list, const fmt::OcPrefabOverride& ov) {
    // A new Add cancels a Remove of the same component and the other way round.
    const auto contradicts = [&](const fmt::OcPrefabOverride& o) {
        return o.path == ov.path && o.component == ov.component && o.op != fmt::OcOverrideOp::Set &&
               ov.op != fmt::OcOverrideOp::Set && o.op != ov.op;
    };
    list.erase(std::remove_if(list.begin(), list.end(), contradicts), list.end());
    for (auto& o : list)
        if (fmt::ocPrefabSameTarget(o, ov)) { o = ov; return; }
    list.push_back(ov);
}

// Folds one override of an instance of `pd` into `pd` itself.
bool foldOverride(fmt::OcPrefabData& pd, const fmt::OcPrefabOverride& ov, std::string* why) {
    std::string head, rest;
    fmt::ocPrefabSplitPath(ov.path, head, rest);
    u32 uid = 0;
    if (head.empty()) uid = pd.rootUid();
    else if (allDigits(head)) uid = static_cast<u32>(std::stoul(head));
    fmt::OcPrefabNode* node = uid ? pd.find(uid) : nullptr;
    if (!node) return fail(why, "the override names node '" + ov.path + "', which the prefab does not have");

    if (!node->prefab.empty()) {
        // Inside a nested prefab: the nested NODE records it, relative to the nested root.
        fmt::OcPrefabOverride rel = ov;
        rel.path = rest;
        if (rel.value.kind == fmt::kOcPrefabKindEntity && rel.value.i >= 0) {
            const std::string& p = rel.value.s;
            const std::string own = head.empty() ? std::string() : head;
            if (p == own) rel.value.s.clear();
            else if (!own.empty() && p.size() > own.size() + 1 && p.compare(0, own.size(), own) == 0 && p[own.size()] == '/')
                rel.value.s = p.substr(own.size() + 1);
            else return fail(why, "an entity reference that leaves the nested prefab cannot be applied");
        }
        replaceOrAppend(node->overrides, rel);
        return true;
    }

    if (!rest.empty()) return fail(why, "the override's path goes below a node that is not a nested prefab");

    switch (ov.op) {
        case fmt::OcOverrideOp::Set: {
            if (ov.component == fmt::kOcPrefabNodeComponent) {
                if (ov.value.name == "name") node->name = ov.value.s;
                return true;
            }
            fmt::OcSaveField v = ov.value;
            if (v.kind == fmt::kOcPrefabKindEntity) {
                if (v.i < 0) { v.i = -1; v.s.clear(); }
                else {
                    u32 target = 0;
                    if (!pathToUid(pd, v.s, target)) return fail(why, "an entity reference into a nested prefab cannot be applied");
                    v.i = target;
                    v.s.clear();
                }
            }
            fmt::OcSaveComponent* c = nullptr;
            for (auto& cc : node->components) if (cc.type == ov.component) { c = &cc; break; }
            if (!c) {
                node->components.emplace_back();
                c = &node->components.back();
                c->type = ov.component;
            }
            for (auto& f : c->fields) if (f.name == v.name) { f = v; return true; }
            c->fields.push_back(std::move(v));
            return true;
        }
        case fmt::OcOverrideOp::AddComponent: {
            for (auto& cc : node->components) if (cc.type == ov.component) return true;
            node->components.emplace_back();
            node->components.back().type = ov.component;
            return true;
        }
        case fmt::OcOverrideOp::RemoveComponent: {
            auto& cs = node->components;
            cs.erase(std::remove_if(cs.begin(), cs.end(),
                                    [&](const fmt::OcSaveComponent& c) { return c.type == ov.component; }),
                     cs.end());
            return true;
        }
    }
    return false;
}

} // namespace

// ---- apply / propagate ----------------------------------------------------------------------------------

bool PrefabSystem::applyOverride(Entity root, const fmt::OcPrefabOverride& which, std::string* why) {
    if (!isInstanceRoot(root)) return fail(why, "not an instance root");
    const std::string pp = prefabPathOf(root);
    const fmt::OcPrefabData* pd = pp.empty() ? nullptr : lib_.get(pp);
    if (!pd) return fail(why, "the instance's prefab asset is not loaded");
    fmt::OcPrefabData data = *pd;
    if (!foldOverride(data, which, why)) return false;
    return updatePrefab(pp, std::move(data), why);
}

bool PrefabSystem::applyAll(Entity root, std::string* why) {
    std::vector<fmt::OcPrefabOverride> ov;
    if (!computeOverrides(root, ov, why)) return false;
    const std::string pp = prefabPathOf(root);
    const fmt::OcPrefabData* pd = lib_.get(pp);
    if (!pd) return fail(why, "the instance's prefab asset is not loaded");
    fmt::OcPrefabData data = *pd;
    std::string skipped;
    for (const fmt::OcPrefabOverride& o : ov) {
        std::string w;
        if (!foldOverride(data, o, &w)) skipped = w;
    }
    // A fold that cannot be expressed leaves that override on the instance; the rest still apply.
    if (!skipped.empty()) AVER_WARN("[Prefab] apply skipped an override: {}", skipped);
    return updatePrefab(pp, std::move(data), why);
}

bool PrefabSystem::updatePrefab(const std::string& prefabPath, fmt::OcPrefabData data, std::string* why) {
    std::string wr;
    if (!data.valid(&wr)) return fail(why, "refusing an invalid prefab: " + wr);
    const std::string target = PrefabLibrary::normalize(prefabPath);

    struct Affected { Entity root; std::string prefab; std::vector<fmt::OcPrefabOverride> ov; };
    std::vector<Affected> affected;
    for (const Entity r : instanceRoots()) {
        const std::string pp = prefabPathOf(r);
        if (pp.empty() || (pp != target && !lib_.references(pp, target))) continue;
        Resolved old;
        if (!lib_.resolve(pp, old, nullptr)) {
            AVER_WARN("[Prefab] instance of '{}' could not be read against the old asset and is left as it is", pp);
            continue;
        }
        Affected a{r, pp, {}};
        diffAgainst(r, old, a.ov);   // the OLD asset is still in the library
        affected.push_back(std::move(a));
    }

    lib_.set(target, std::move(data));

    for (Affected& a : affected) {
        Resolved base;
        std::string w;
        if (!lib_.resolve(a.prefab, base, &w)) {
            AVER_WARN("[Prefab] '{}' no longer resolves after the edit: {}", a.prefab, w);
            continue;
        }
        realize(a.root, base, a.ov, nullptr, kInvalidEntity, std::string(), 0);
    }
    return true;
}

bool PrefabSystem::syncInstance(Entity root, std::string* why) {
    std::vector<fmt::OcPrefabOverride> ov;
    if (!computeOverrides(root, ov, why)) return false;
    return setOverrides(root, ov, why);
}

// ---- authoring ------------------------------------------------------------------------------------------------

bool PrefabSystem::captureAsPrefab(Entity root, const std::string& name, fmt::OcPrefabData& out, std::string* why) {
    if (!world_.valid(root) || world_.destroyPending(root)) return fail(why, "nothing to capture");
    out = fmt::OcPrefabData{};
    out.name = name;

    struct Item { Entity e; u32 parentUid; u32 uid; bool nested; };
    std::vector<Item> items;
    std::unordered_map<u32, u32> uidOf;
    {
        std::vector<std::pair<Entity, u32>> stack{{root, 0}};
        while (!stack.empty()) {
            const auto [e, parentUid] = stack.back();
            stack.pop_back();
            if (!world_.valid(e) || world_.destroyPending(e)) continue;
            const u32 uid = out.allocUid();
            const bool nested = e != root && isInstanceRoot(e);
            items.push_back({e, parentUid, uid, nested});
            uidOf[static_cast<u32>(e)] = uid;
            for (Entity c = world_.firstChild(e); c != kInvalidEntity; c = world_.nextSibling(c)) {
                // What the nested instance owns is captured through its overrides, not as nodes.
                if (nested) {
                    const auto* lk = world_.component<scene::CPrefabLink>(c, linkType_);
                    if (lk && lk->root == e) continue;
                }
                stack.emplace_back(c, uid);
            }
        }
    }

    const detail::PathLookup lookup = [&](Entity t, std::string& p) {
        const auto it = uidOf.find(static_cast<u32>(t));
        if (it == uidOf.end()) return false;
        p = std::to_string(it->second);
        return true;
    };

    for (const Item& it : items) {
        fmt::OcPrefabNode node;
        node.uid = it.uid;
        node.parent = it.parentUid;
        node.name = world_.name(it.e);

        if (it.nested) {
            const auto* lk = world_.component<scene::CPrefabLink>(it.e, linkType_);
            node.prefab = lib_.pathOfId(lk->prefabId);
            if (node.prefab.empty()) return fail(why, "a nested instance's prefab asset path is unknown");
            if (!computeOverrides(it.e, node.overrides, why)) return false;
        } else {
            for (u32 ci = 0; ci < world_.componentCount(); ++ci) {
                const u32 type = world_.componentAt(ci);
                if (detail::excludedComponent(type, linkType_) || !world_.hasComponent(it.e, type)) continue;
                fmt::OcSaveComponent c;
                if (!detail::readComponent(world_, it.e, type, lookup, c)) continue;
                for (fmt::OcSaveField& f : c.fields)
                    if (f.kind == fmt::kOcPrefabKindEntity && f.i >= 0) {
                        f.i = static_cast<i64>(std::stoul(f.s));   // lookup only ever answers a uid
                        f.s.clear();
                    }
                // The prefab's root sits at the origin; an instance supplies the placement.
                if (it.e == root && type == scene::kComponentLocal)
                    for (fmt::OcSaveField& f : c.fields) {
                        if (f.name == "position" || f.name == "scale")
                            f.f.assign(3, f.name == "scale" ? 1.0f : 0.0f);
                        else if (f.name == "rotation") f.f = {0.0f, 0.0f, 0.0f, 1.0f};
                    }
                node.components.push_back(std::move(c));
            }
        }
        out.nodes.push_back(std::move(node));
    }

    // A nested instance's own placement is an override on its node, whatever it is relative to.
    for (fmt::OcPrefabNode& node : out.nodes) {
        if (node.prefab.empty() || node.parent == 0) continue;
        Entity e = kInvalidEntity;
        for (const Item& it : items) if (it.uid == node.uid) { e = it.e; break; }
        fmt::OcSaveComponent loc;
        if (!detail::readComponent(world_, e, scene::kComponentLocal, nullptr, loc)) continue;
        for (const fmt::OcSaveField& f : loc.fields) {
            fmt::OcPrefabOverride ov;
            ov.path = std::string();
            ov.component = loc.type;
            ov.value = f;
            replaceOrAppend(node.overrides, ov);
        }
    }

    return out.valid(why);
}

Entity PrefabSystem::convertToInstance(Entity root, const std::string& prefabPath, std::string* why) {
    if (!world_.valid(root) || world_.destroyPending(root)) { fail(why, "nothing to convert"); return kInvalidEntity; }
    const Transform xf = world_.localTransform(root);
    const Entity parent = world_.parent(root);
    const std::string name = world_.name(root);
    const Entity inst = instantiate(prefabPath, xf, parent, {}, name, why);
    if (inst == kInvalidEntity) return kInvalidEntity;
    destroyTree(root);
    return inst;
}

// ---- undo ---------------------------------------------------------------------------------------------------------

u32 PrefabSystem::registerPlain(Entity root) {
    const u32 id = nextGroupId_++;
    plainRoots_[id] = root;
    return id;
}

PrefabState PrefabSystem::captureState(const std::vector<std::string>& prefabPaths,
                                       const std::vector<u32>& instanceIds,
                                       const std::vector<u32>& plainGroups) {
    PrefabState s;
    for (const std::string& p : prefabPaths) {
        LibrarySnapshot ls;
        ls.path = PrefabLibrary::normalize(p);
        if (const fmt::OcPrefabData* d = lib_.peek(p)) ls.data = *d;
        else ls.existed = false;
        s.libs.push_back(std::move(ls));
    }
    for (const u32 id : instanceIds) {
        InstanceSnapshot is;
        is.instanceId = id;
        const Entity r = rootOfInstance(id);
        if (r == kInvalidEntity) { is.present = false; s.instances.push_back(std::move(is)); continue; }
        is.prefab = prefabPathOf(r);
        is.name = world_.name(r);
        is.local = world_.localTransform(r);
        is.parent = world_.parent(r);
        computeOverrides(r, is.overrides, nullptr);
        s.instances.push_back(std::move(is));
    }
    for (const u32 gid : plainGroups) {
        PlainSnapshot ps;
        ps.groupId = gid;
        const auto it = plainRoots_.find(gid);
        if (it == plainRoots_.end() || !world_.valid(it->second) || world_.destroyPending(it->second) ||
            !captureAsPrefab(it->second, world_.name(it->second), ps.data, nullptr)) {
            ps.present = false;
        } else {
            ps.local = world_.localTransform(it->second);
            ps.parent = world_.parent(it->second);
        }
        s.plains.push_back(std::move(ps));
    }
    return s;
}

bool PrefabSystem::restoreState(const PrefabState& st, std::string* why) {
    (void)why;
    // Instances the state does not name, but whose prefab it changes, keep their overrides: read them
    // against the assets as they are NOW, before any asset moves.
    std::unordered_set<u32> named;
    for (const InstanceSnapshot& is : st.instances) named.insert(is.instanceId);
    struct Pend { Entity root; std::vector<fmt::OcPrefabOverride> ov; };
    std::vector<Pend> pend;
    if (!st.libs.empty()) {
        for (const Entity r : instanceRoots()) {
            if (named.count(instanceIdOf(r))) continue;
            const std::string pp = prefabPathOf(r);
            if (pp.empty()) continue;
            bool hit = false;
            for (const LibrarySnapshot& l : st.libs)
                if (pp == PrefabLibrary::normalize(l.path) || lib_.references(pp, l.path)) { hit = true; break; }
            if (!hit) continue;
            Pend p{r, {}};
            if (computeOverrides(r, p.ov, nullptr)) pend.push_back(std::move(p));
        }
    }

    for (const LibrarySnapshot& l : st.libs) {
        if (l.existed) lib_.set(l.path, l.data);
        else lib_.drop(l.path);
    }

    for (const PlainSnapshot& ps : st.plains) {
        const auto it = plainRoots_.find(ps.groupId);
        const bool alive = it != plainRoots_.end() && world_.valid(it->second) && !world_.destroyPending(it->second);
        if (!ps.present) {
            if (alive) destroyTree(it->second);
            continue;
        }
        if (alive) continue;
        // Recreated through a throwaway asset, then unlinked: the entities are ordinary again.
        const std::string tmp = "@plain/" + std::to_string(++plainTemp_);
        lib_.set(tmp, ps.data);
        const Entity parent = world_.valid(ps.parent) && !world_.destroyPending(ps.parent) ? ps.parent : kInvalidEntity;
        const Entity inst = instantiate(tmp, ps.local, parent, {}, ps.data.name, nullptr);
        if (inst != kInvalidEntity) {
            unlink(inst);
            plainRoots_[ps.groupId] = inst;
        }
        lib_.drop(tmp);
    }

    for (const InstanceSnapshot& is : st.instances) {
        if (is.present) continue;
        const Entity r = rootOfInstance(is.instanceId);
        if (r != kInvalidEntity) destroyTree(r);
    }
    for (const InstanceSnapshot& is : st.instances) {
        if (!is.present) continue;
        std::string w;
        Resolved base;
        if (!lib_.resolve(is.prefab, base, &w)) {
            AVER_WARN("[Prefab] cannot restore an instance of '{}': {}", is.prefab, w);
            continue;
        }
        const Entity r = rootOfInstance(is.instanceId);
        if (r != kInvalidEntity) {
            realize(r, base, is.overrides, nullptr, kInvalidEntity, std::string(), is.instanceId);
        } else {
            const Entity parent = world_.valid(is.parent) && !world_.destroyPending(is.parent) ? is.parent : kInvalidEntity;
            instantiateWithId(is.prefab, is.local, parent, is.overrides, is.name, is.instanceId, &w);
        }
    }

    for (Pend& p : pend) {
        if (!world_.valid(p.root) || world_.destroyPending(p.root)) continue;
        Resolved base;
        if (!resolveInstance(p.root, base, nullptr)) continue;
        realize(p.root, base, p.ov, nullptr, kInvalidEntity, std::string(), 0);
    }
    return true;
}

// ---- levels ---------------------------------------------------------------------------------------------------------

std::vector<fmt::OcPrefabInstance> PrefabSystem::captureLevelInstances(const std::function<bool(Entity)>& include) {
    std::vector<fmt::OcPrefabInstance> out;
    for (const Entity r : instanceRoots()) {
        if (include && !include(r)) continue;
        fmt::OcPrefabInstance rec;
        rec.prefab = prefabPathOf(r);
        if (rec.prefab.empty()) continue;
        const std::string nm = world_.name(r);
        const fmt::OcPrefabData* pd = lib_.get(rec.prefab);
        const fmt::OcPrefabNode* rootNode = pd ? pd->find(pd->rootUid()) : nullptr;
        if (!rootNode || rootNode->name != nm) rec.name = nm;
        const Transform& xf = world_.localTransform(r);
        rec.x = xf.position.x; rec.y = xf.position.y; rec.z = xf.position.z;
        const Vec3 euler = world::eulerDegFromQuat(xf.rotation);
        rec.roll = euler.x; rec.pitch = euler.y; rec.yaw = euler.z;
        rec.sx = xf.scale.x; rec.sy = xf.scale.y; rec.sz = xf.scale.z;
        computeOverrides(r, rec.overrides, nullptr);
        out.push_back(std::move(rec));
    }
    return out;
}

std::vector<Entity> PrefabSystem::instantiateLevelInstances(const std::vector<fmt::OcPrefabInstance>& records) {
    std::vector<Entity> roots;
    for (const fmt::OcPrefabInstance& rec : records) {
        Transform xf;
        xf.position = Vec3{static_cast<f32>(rec.x), static_cast<f32>(rec.y), static_cast<f32>(rec.z)};
        xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(rec.roll), static_cast<f32>(rec.pitch),
                                                   static_cast<f32>(rec.yaw)});
        xf.scale = Vec3{static_cast<f32>(rec.sx), static_cast<f32>(rec.sy), static_cast<f32>(rec.sz)};
        std::string why;
        const Entity r = instantiate(rec.prefab, xf, kInvalidEntity, rec.overrides, rec.name, &why);
        if (r == kInvalidEntity) AVER_WARN("[Prefab] level instance of '{}' skipped: {}", rec.prefab, why);
        else roots.push_back(r);
    }
    return roots;
}

} // namespace aver::prefab
