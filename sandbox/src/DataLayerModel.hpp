#pragma once
// The Data Layers panel's model (WAVE2_UE W10 step 4): the level's DATALAYER list, which entities belong to which layer, and the editor-only
// visible / loaded toggles. Header-only and ImGui-free so every rule is unit tested. An entity is named by an opaque 64-bit key (the caller
// uses its stable edit id), so an undone delete finds its layers again.
//
// Every mutator returns an Edit: the layer list before and after, plus the membership rows it changed. apply(edit, undo) replays it either
// way, so the editor's undo stack stores nothing but Edits. A refused or no-op call returns an empty Edit and changes nothing.
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aver::editor::dl {

using Key = u64;
using Names = std::vector<std::string>;

struct MemberChange {
    Key key = 0;
    Names before, after;
};

struct Edit {
    bool layersChanged = false;
    std::vector<fmt::OcDataLayer> layersBefore, layersAfter;
    std::vector<MemberChange> members;
    bool empty() const { return !layersChanged && members.empty(); }
};

// Editor-only, never saved: a hidden layer's entities are not drawn, an unloaded one is also not pickable (the editor hides it and
// leaves it out of selection; the entities themselves stay in the scene so the level still saves whole).
struct EditorFlags {
    bool visible = true;
    bool loaded = true;
};

class Model {
public:
    // ---- document ----
    void clear() { layers_.clear(); members_.clear(); flags_.clear(); }
    // From the level's DATALAYER records and each placement's `layers=` (keys of placements with none are simply absent).
    void load(const std::vector<fmt::OcDataLayer>& defs) { layers_ = defs; flags_.clear(); members_.clear(); }
    void setMembers(Key key, Names names) {
        if (names.empty()) members_.erase(key);
        else members_[key] = std::move(names);
    }
    const std::vector<fmt::OcDataLayer>& layers() const { return layers_; }
    const Names& layersOf(Key key) const {
        static const Names none;
        const auto it = members_.find(key);
        return it == members_.end() ? none : it->second;
    }
    bool hasLayer(const std::string& name) const { return indexOf(name) >= 0; }
    const fmt::OcDataLayer* find(const std::string& name) const {
        const int i = indexOf(name);
        return i < 0 ? nullptr : &layers_[static_cast<usize>(i)];
    }
    std::vector<Key> membersOf(const std::string& name) const {
        std::vector<Key> out;
        for (const auto& kv : members_)
            if (std::find(kv.second.begin(), kv.second.end(), name) != kv.second.end()) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }
    usize memberCount(const std::string& name) const { return membersOf(name).size(); }
    // The names of every layer under `name` (children, grandchildren...), not including it.
    Names descendants(const std::string& name) const {
        Names out;
        for (usize i = 0; i < out.size() + 1; ++i) {
            const std::string at = i == 0 ? name : out[i - 1];   // a copy: push_back below can reallocate `out`
            for (const fmt::OcDataLayer& l : layers_)
                if (l.parent == at && std::find(out.begin(), out.end(), l.name) == out.end()) out.push_back(l.name);
        }
        return out;
    }

    // The tree the panel lists: roots in file order, each followed by its children (depth-first). A layer whose parent is missing counts as a root.
    struct Row { std::string name; int depth = 0; };
    std::vector<Row> rows() const {
        std::vector<Row> out;
        std::unordered_set<std::string> done;
        const auto walk = [&](auto&& self, const std::string& name, int depth) -> void {
            if (!done.insert(name).second) return;
            out.push_back({name, depth});
            for (const fmt::OcDataLayer& l : layers_) if (l.parent == name) self(self, l.name, depth + 1);
        };
        for (const fmt::OcDataLayer& l : layers_)
            if (l.parent.empty() || indexOf(l.parent) < 0) walk(walk, l.name, 0);
        return out;
    }

    // ---- layer edits ----
    // True when `name` can name a new layer: not empty, no leading/trailing space, not already used.
    bool nameFree(const std::string& name) const {
        if (name.empty() || name.front() == ' ' || name.back() == ' ') return false;
        return indexOf(name) < 0;
    }
    // A unique "Layer", "Layer 2"... for the New button.
    std::string freshName(const std::string& base = "Layer") const {
        if (nameFree(base)) return base;
        for (int i = 2;; ++i) {
            std::string n = base + " " + std::to_string(i);
            if (nameFree(n)) return n;
        }
    }
    Edit create(const std::string& name, const std::string& parent = {}) {
        if (!nameFree(name)) return {};
        if (!parent.empty() && indexOf(parent) < 0) return {};
        Edit e = begin();
        fmt::OcDataLayer l;
        l.name = name;
        l.parent = parent;
        if (const fmt::OcDataLayer* p = find(parent)) l.initial = std::min(l.initial, p->initial);
        layers_.push_back(l);
        return finish(std::move(e));
    }
    // Renames and carries the change to every child's parent, every membership row and the editor flags.
    Edit rename(const std::string& from, const std::string& to) {
        const int at = indexOf(from);
        if (at < 0 || from == to || !nameFree(to)) return {};
        Edit e = begin();
        layers_[static_cast<usize>(at)].name = to;
        for (fmt::OcDataLayer& l : layers_) if (l.parent == from) l.parent = to;
        for (auto& kv : members_) {
            Names after = kv.second;
            bool hit = false;
            for (std::string& n : after) if (n == from) { n = to; hit = true; }
            if (hit) { e.members.push_back({kv.first, kv.second, after}); }
        }
        for (const MemberChange& m : e.members) members_[m.key] = m.after;
        if (const auto f = flags_.find(from); f != flags_.end()) { const EditorFlags fl = f->second; flags_.erase(from); flags_[to] = fl; }
        return finish(std::move(e));
    }
    // Removes the layer. Its children move up to its parent; entities lose the name.
    Edit remove(const std::string& name) {
        const int at = indexOf(name);
        if (at < 0) return {};
        Edit e = begin();
        const std::string up = layers_[static_cast<usize>(at)].parent;
        layers_.erase(layers_.begin() + at);
        for (fmt::OcDataLayer& l : layers_) if (l.parent == name) l.parent = up;
        for (auto& kv : members_) {
            Names after = kv.second;
            after.erase(std::remove(after.begin(), after.end(), name), after.end());
            if (after.size() != kv.second.size()) e.members.push_back({kv.first, kv.second, after});
        }
        for (const MemberChange& m : e.members) setMembers(m.key, m.after);
        flags_.erase(name);
        return finish(std::move(e));
    }
    Edit setInitial(const std::string& name, fmt::OcDataLayerState s) {
        const int at = indexOf(name);
        if (at < 0 || layers_[static_cast<usize>(at)].initial == s) return {};
        Edit e = begin();
        layers_[static_cast<usize>(at)].initial = s;
        return finish(std::move(e));
    }
    Edit setRuntime(const std::string& name, bool runtime) {
        const int at = indexOf(name);
        if (at < 0 || layers_[static_cast<usize>(at)].runtime == runtime) return {};
        Edit e = begin();
        layers_[static_cast<usize>(at)].runtime = runtime;
        return finish(std::move(e));
    }
    // Empty `parent` makes it a root. Refused for a missing layer, itself, or one of its own descendants.
    Edit setParent(const std::string& name, const std::string& parent) {
        const int at = indexOf(name);
        if (at < 0 || layers_[static_cast<usize>(at)].parent == parent) return {};
        if (!parent.empty()) {
            if (indexOf(parent) < 0 || parent == name) return {};
            const Names under = descendants(name);
            if (std::find(under.begin(), under.end(), parent) != under.end()) return {};
        }
        Edit e = begin();
        layers_[static_cast<usize>(at)].parent = parent;
        return finish(std::move(e));
    }

    // ---- membership ----
    Edit assign(const std::vector<Key>& keys, const std::string& layer) {
        if (indexOf(layer) < 0) return {};
        Edit e;
        for (const Key k : keys) {
            Names before = layersOf(k);
            if (std::find(before.begin(), before.end(), layer) != before.end()) continue;
            Names after = before;
            after.push_back(layer);
            e.members.push_back({k, std::move(before), std::move(after)});
        }
        for (const MemberChange& m : e.members) setMembers(m.key, m.after);
        return e;
    }
    Edit unassign(const std::vector<Key>& keys, const std::string& layer) {
        Edit e;
        for (const Key k : keys) {
            Names before = layersOf(k);
            const auto it = std::find(before.begin(), before.end(), layer);
            if (it == before.end()) continue;
            Names after = before;
            after.erase(after.begin() + (it - before.begin()));
            e.members.push_back({k, std::move(before), std::move(after)});
        }
        for (const MemberChange& m : e.members) setMembers(m.key, m.after);
        return e;
    }
    // Entities that are gone: forget them without an Edit (an undone delete re-adds them through its own undo).
    void forget(Key key) { members_.erase(key); }

    // ---- undo ----
    void apply(const Edit& e, bool undoing) {
        if (e.layersChanged) layers_ = undoing ? e.layersBefore : e.layersAfter;
        for (const MemberChange& m : e.members) setMembers(m.key, undoing ? m.before : m.after);
        // A name that no longer exists loses its flags.
        for (auto it = flags_.begin(); it != flags_.end();) it = indexOf(it->first) < 0 ? flags_.erase(it) : std::next(it);
    }

    // ---- editor visibility / load (not saved, not undone) ----
    EditorFlags flagsOf(const std::string& name) const {
        const auto it = flags_.find(name);
        return it == flags_.end() ? EditorFlags{} : it->second;
    }
    void setEditorVisible(const std::string& name, bool v) { if (indexOf(name) >= 0) flags_[name].visible = v; }
    void setEditorLoaded(const std::string& name, bool v) { if (indexOf(name) >= 0) flags_[name].loaded = v; }
    // A layer shows only when it and every ancestor is visible and loaded.
    bool layerShown(const std::string& name) const {
        std::unordered_set<std::string> seen;
        for (std::string at = name; !at.empty() && seen.insert(at).second;) {
            const EditorFlags f = flagsOf(at);
            if (!f.visible || !f.loaded) return false;
            const fmt::OcDataLayer* l = find(at);
            if (!l) break;
            at = l->parent;
        }
        return true;
    }
    bool layerLoaded(const std::string& name) const {
        std::unordered_set<std::string> seen;
        for (std::string at = name; !at.empty() && seen.insert(at).second;) {
            if (!flagsOf(at).loaded) return false;
            const fmt::OcDataLayer* l = find(at);
            if (!l) break;
            at = l->parent;
        }
        return true;
    }
    // An entity is hidden when any layer it belongs to is hidden; unpickable when any is unloaded.
    bool entityHidden(Key key) const {
        for (const std::string& n : layersOf(key)) if (!layerShown(n)) return true;
        return false;
    }
    bool entityLoaded(Key key) const {
        for (const std::string& n : layersOf(key)) if (!layerLoaded(n)) return false;
        return true;
    }
    // Entities in any layer, for the caller to re-evaluate visibility after a flag change.
    std::vector<Key> allMembers() const {
        std::vector<Key> out;
        out.reserve(members_.size());
        for (const auto& kv : members_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    int indexOf(const std::string& name) const {
        for (usize i = 0; i < layers_.size(); ++i) if (layers_[i].name == name) return static_cast<int>(i);
        return -1;
    }
    Edit begin() const {
        Edit e;
        e.layersBefore = layers_;
        return e;
    }
    Edit finish(Edit e) const {
        e.layersAfter = layers_;
        e.layersChanged = e.layersBefore != e.layersAfter;
        return e;
    }

    std::vector<fmt::OcDataLayer> layers_;
    std::unordered_map<Key, Names> members_;
    std::unordered_map<std::string, EditorFlags> flags_;
};

}  // namespace aver::editor::dl
