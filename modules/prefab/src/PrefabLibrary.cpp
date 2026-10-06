#include "aver/prefab/PrefabLibrary.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <unordered_set>

namespace aver::prefab {
namespace {

constexpr usize kMaxNesting = 8;

fmt::OcSaveComponent* findComp(std::vector<fmt::OcSaveComponent>& cs, const std::string& type) {
    for (auto& c : cs) if (c.type == type) return &c;
    return nullptr;
}

// Entity-kind values in an override name a path relative to the override's own level.
fmt::OcSaveField prefixedValue(const fmt::OcSaveField& v, const std::string& prefix) {
    fmt::OcSaveField f = v;
    if (f.kind == fmt::kOcPrefabKindEntity && f.i >= 0) {
        f.i = 0;
        f.s = fmt::ocPrefabJoinPath(prefix, v.s);
    }
    return f;
}

std::string uidPath(const fmt::OcPrefabData& pd, u32 uid, u32 rootUid) {
    (void)pd;
    return uid == rootUid ? std::string() : std::to_string(uid);
}

enum class Flat { Ok, Missing, Hard };

struct Flattener {
    PrefabLibrary& lib;
    std::vector<std::string> stack;
    std::vector<std::string> warnings;
    std::string hardError;

    Flat run(const std::string& path, std::vector<ResolvedNode>& out) {
        const std::string norm = PrefabLibrary::normalize(path);
        if (std::find(stack.begin(), stack.end(), norm) != stack.end()) {
            hardError = "prefab '" + norm + "' contains itself";
            return Flat::Hard;
        }
        if (stack.size() >= kMaxNesting) {
            hardError = "prefabs nest deeper than " + std::to_string(kMaxNesting) + " levels at '" + norm + "'";
            return Flat::Hard;
        }
        const fmt::OcPrefabData* pd = lib.get(norm);
        if (!pd) return Flat::Missing;
        const fmt::OcPrefabData data = *pd;   // a copy: get() of a nested asset may rehash the cache
        stack.push_back(norm);

        const u32 rootUid = data.rootUid();
        for (const fmt::OcPrefabNode& node : data.nodes) {
            const std::string npath = uidPath(data, node.uid, rootUid);
            const std::string parentPath =
                node.parent == 0 ? std::string() : uidPath(data, node.parent, rootUid);

            if (node.prefab.empty()) {
                ResolvedNode rn;
                rn.path = npath;
                rn.parentPath = parentPath;
                rn.isRoot = node.parent == 0;
                rn.name = node.name;
                rn.className = node.className;
                rn.components = node.components;
                for (auto& c : rn.components)
                    for (auto& f : c.fields) {
                        if (f.kind != fmt::kOcPrefabKindEntity) continue;
                        const i64 uid = f.i;
                        if (uid <= 0) { f.i = -1; f.s.clear(); }
                        else { f.i = 0; f.s = uidPath(data, static_cast<u32>(uid), rootUid); }
                    }
                out.push_back(std::move(rn));
                continue;
            }

            std::vector<ResolvedNode> sub;
            const Flat r = run(node.prefab, sub);
            if (r == Flat::Hard) { stack.pop_back(); return Flat::Hard; }
            if (r == Flat::Missing) {
                warnings.push_back("nested prefab '" + node.prefab + "' could not be loaded");
                ResolvedNode rn;
                rn.path = npath;
                rn.parentPath = parentPath;
                rn.isRoot = node.parent == 0;
                rn.name = node.name;
                rn.className = node.className;
                rn.missingPrefab = node.prefab;
                const usize start = out.size();
                out.push_back(std::move(rn));
                std::vector<ResolvedNode> one(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
                applyOverrides(one, node.overrides, npath, &warnings);
                out.resize(start);
                for (auto& o : one) out.push_back(std::move(o));
                continue;
            }

            const usize start = out.size();
            for (ResolvedNode& s : sub) {
                ResolvedNode rn = std::move(s);
                const bool subRoot = rn.isRoot;
                rn.path = fmt::ocPrefabJoinPath(npath, rn.path);
                if (subRoot) {
                    rn.parentPath = parentPath;
                    rn.isRoot = node.parent == 0;
                    if (!node.name.empty()) rn.name = node.name;
                    if (!node.className.empty()) rn.className = node.className;
                } else {
                    rn.parentPath = fmt::ocPrefabJoinPath(npath, rn.parentPath);
                }
                for (auto& c : rn.components)
                    for (auto& f : c.fields)
                        if (f.kind == fmt::kOcPrefabKindEntity && f.i >= 0)
                            f.s = fmt::ocPrefabJoinPath(npath, f.s);
                out.push_back(std::move(rn));
            }
            // Only this nested block is searched, so an override cannot leak onto a sibling.
            std::vector<ResolvedNode> block(std::make_move_iterator(out.begin() + static_cast<std::ptrdiff_t>(start)),
                                            std::make_move_iterator(out.end()));
            out.resize(start);
            applyOverrides(block, node.overrides, npath, &warnings);
            for (auto& b : block) out.push_back(std::move(b));
        }

        stack.pop_back();
        return Flat::Ok;
    }
};

} // namespace

// ---- Resolved -----------------------------------------------------------------------------------------

const ResolvedNode* Resolved::find(const std::string& path) const {
    for (const ResolvedNode& n : nodes) if (n.path == path) return &n;
    return nullptr;
}

ResolvedNode* Resolved::find(const std::string& path) {
    for (ResolvedNode& n : nodes) if (n.path == path) return &n;
    return nullptr;
}

u32 applyOverrides(std::vector<ResolvedNode>& nodes, const std::vector<fmt::OcPrefabOverride>& overrides,
                   const std::string& prefix, std::vector<std::string>* warnings) {
    u32 missed = 0;
    for (const fmt::OcPrefabOverride& ov : overrides) {
        const std::string target = fmt::ocPrefabJoinPath(prefix, ov.path);
        ResolvedNode* node = nullptr;
        for (ResolvedNode& n : nodes) if (n.path == target) { node = &n; break; }
        if (!node) {
            ++missed;
            if (warnings) warnings->push_back("an override targets node '" + target + "', which does not exist");
            continue;
        }
        switch (ov.op) {
            case fmt::OcOverrideOp::Set: {
                if (ov.component == fmt::kOcPrefabNodeComponent) {
                    if (ov.value.name == "name") node->name = ov.value.s;
                    break;
                }
                fmt::OcSaveComponent* c = findComp(node->components, ov.component);
                if (!c) {
                    node->components.emplace_back();
                    c = &node->components.back();
                    c->type = ov.component;
                }
                const fmt::OcSaveField v = prefixedValue(ov.value, prefix);
                bool placed = false;
                for (auto& f : c->fields)
                    if (f.name == v.name) { f = v; placed = true; break; }
                if (!placed) c->fields.push_back(v);
                break;
            }
            case fmt::OcOverrideOp::AddComponent:
                if (!findComp(node->components, ov.component)) {
                    node->components.emplace_back();
                    node->components.back().type = ov.component;
                }
                break;
            case fmt::OcOverrideOp::RemoveComponent: {
                auto& cs = node->components;
                cs.erase(std::remove_if(cs.begin(), cs.end(),
                                        [&](const fmt::OcSaveComponent& c) { return c.type == ov.component; }),
                         cs.end());
                break;
            }
        }
    }
    return missed;
}

// ---- PrefabLibrary --------------------------------------------------------------------------------------

std::string PrefabLibrary::normalize(std::string_view path) {
    std::string s(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    return s;
}

u64 PrefabLibrary::idOf(std::string_view path) {
    return fnv1a64(std::string_view(normalize(path)));
}

void PrefabLibrary::remember(const std::string& path) {
    const std::string n = normalize(path);
    names_[fnv1a64(std::string_view(n))] = n;
}

const fmt::OcPrefabData* PrefabLibrary::peek(const std::string& path) const {
    const auto it = cache_.find(normalize(path));
    return it == cache_.end() ? nullptr : &it->second;
}

const fmt::OcPrefabData* PrefabLibrary::get(const std::string& path) {
    const std::string n = normalize(path);
    remember(n);
    if (const auto it = cache_.find(n); it != cache_.end()) return &it->second;
    if (!load_) return nullptr;
    fmt::OcPrefabData data;
    std::string why;
    if (!load_(n, data, &why)) return nullptr;
    return &(cache_[n] = std::move(data));
}

void PrefabLibrary::set(const std::string& path, fmt::OcPrefabData data) {
    const std::string n = normalize(path);
    remember(n);
    cache_[n] = std::move(data);
}

bool PrefabLibrary::drop(const std::string& path) {
    return cache_.erase(normalize(path)) != 0;
}

bool PrefabLibrary::save(const std::string& path, std::string* why) {
    const fmt::OcPrefabData* d = peek(path);
    if (!d) { if (why) *why = "prefab '" + path + "' is not loaded"; return false; }
    if (!save_) { if (why) *why = "no saver is installed"; return false; }
    return save_(normalize(path), *d, why);
}

std::string PrefabLibrary::pathOfId(u64 id) const {
    const auto it = names_.find(id);
    return it == names_.end() ? std::string() : it->second;
}

std::vector<std::string> PrefabLibrary::cachedPaths() const {
    std::vector<std::string> out;
    out.reserve(cache_.size());
    for (const auto& kv : cache_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool PrefabLibrary::references(const std::string& path, const std::string& target) {
    const std::string goal = normalize(target);
    std::unordered_set<std::string> seen;
    std::vector<std::string> todo{normalize(path)};
    while (!todo.empty()) {
        const std::string cur = std::move(todo.back());
        todo.pop_back();
        if (!seen.insert(cur).second) continue;
        const fmt::OcPrefabData* pd = get(cur);
        if (!pd) continue;
        for (const fmt::OcPrefabNode& n : pd->nodes) {
            if (n.prefab.empty()) continue;
            const std::string np = normalize(n.prefab);
            if (np == goal) return true;
            todo.push_back(np);
        }
    }
    return false;
}

bool PrefabLibrary::resolve(const std::string& path, Resolved& out, std::string* why) {
    out = Resolved{};
    out.prefab = normalize(path);
    Flattener fl{*this, {}, {}, {}};
    const Flat r = fl.run(path, out.nodes);
    out.warnings = std::move(fl.warnings);
    if (r == Flat::Hard) { out.nodes.clear(); if (why) *why = fl.hardError; return false; }
    if (r == Flat::Missing) { out.nodes.clear(); if (why) *why = "prefab '" + out.prefab + "' could not be loaded"; return false; }
    if (out.nodes.empty()) { if (why) *why = "prefab '" + out.prefab + "' is empty"; return false; }
    return true;
}

} // namespace aver::prefab
