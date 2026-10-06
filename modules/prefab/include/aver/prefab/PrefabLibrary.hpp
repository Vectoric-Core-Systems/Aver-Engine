#pragma once
// The prefab assets a process has loaded, and the flattening of one prefab (nested prefabs and
// overrides applied) into the plain node list an instance is built from.
//
// No scene in here: this half is pure data, so the flatten rules are testable without a World.
#include "aver/core/Types.hpp"
#include "aver/formats/OcPrefab.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aver::prefab {

// One node after nesting and overrides are folded in. Paths are the OcPrefab.hpp node paths.
struct ResolvedNode {
    std::string path;          // "" is the root
    std::string parentPath;    // meaningless for the root
    bool isRoot = false;
    std::string name;
    std::string className;
    // Entity-kind fields are resolved: `i == -1` is none, otherwise `s` is the target node's path.
    std::vector<fmt::OcSaveComponent> components;
    // Non-empty when this node stands in for a nested prefab that could not be loaded.
    std::string missingPrefab;
};

struct Resolved {
    std::string prefab;                 // normalised asset path
    std::vector<ResolvedNode> nodes;    // parents first; nodes[0] is the root
    std::vector<std::string> warnings;  // missing nested assets, overrides aimed at nothing

    const ResolvedNode* find(const std::string& path) const;
    ResolvedNode* find(const std::string& path);
};

// Folds `overrides` into `nodes`. Their paths are relative to `prefix` (the path of the node whose
// nested prefab they belong to; "" for an instance's own list). Returns how many hit no node.
u32 applyOverrides(std::vector<ResolvedNode>& nodes, const std::vector<fmt::OcPrefabOverride>& overrides,
                   const std::string& prefix, std::vector<std::string>* warnings = nullptr);

class PrefabLibrary {
public:
    // Reads / writes one asset by its content-relative path. Unset: only set() prefabs exist.
    using LoadFn = std::function<bool(const std::string& path, fmt::OcPrefabData& out, std::string* why)>;
    using SaveFn = std::function<bool(const std::string& path, const fmt::OcPrefabData& data, std::string* why)>;

    // Backslashes to slashes and a leading "./" dropped, so one asset has one spelling.
    static std::string normalize(std::string_view path);
    // fnv1a64 of the normalised path: what a CPrefabLink's prefabId holds.
    static u64 idOf(std::string_view path);

    void setLoader(LoadFn fn) { load_ = std::move(fn); }
    void setSaver(SaveFn fn)  { save_ = std::move(fn); }

    // The cached asset, loading it on first use. nullptr when it does not exist.
    const fmt::OcPrefabData* get(const std::string& path);
    // The cached asset only; never loads.
    const fmt::OcPrefabData* peek(const std::string& path) const;
    // Replaces the cached asset. Does NOT touch instances -- PrefabSystem::updatePrefab does.
    void set(const std::string& path, fmt::OcPrefabData data);
    // Forgets the cached asset so the next get() reloads it.
    bool drop(const std::string& path);
    // Writes the cached asset through the saver.
    bool save(const std::string& path, std::string* why = nullptr);
    // Records that `path` exists as a name, so an id read from a CPrefabLink can be turned back into
    // a path even before the asset loads.
    void remember(const std::string& path);

    // The path an id was last seen under, "" when never seen.
    std::string pathOfId(u64 id) const;
    std::vector<std::string> cachedPaths() const;

    // True when `path` contains `target` as a nested prefab anywhere below it (never itself).
    bool references(const std::string& path, const std::string& target);

    // Flattens `path` into `out`. False on a cycle, a depth over 8, or when `path` itself is missing;
    // a missing NESTED asset is a warning and a placeholder node, so one broken reference does not
    // take every instance of the outer prefab down.
    bool resolve(const std::string& path, Resolved& out, std::string* why = nullptr);

private:
    std::unordered_map<std::string, fmt::OcPrefabData> cache_;
    std::unordered_map<u64, std::string> names_;
    LoadFn load_;
    SaveFn save_;
};

} // namespace aver::prefab
