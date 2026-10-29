// Multiplayer nodes (docs/plans/fabric/G6_NETWORK.md, NET2): MultiplayerSpawner, MultiplayerSynchronizer and the SceneReplicationConfig sub-resource.
//
// A replication config has no engine file format: Aver.Fabric builds it from `Object(SceneReplicationConfig, "properties": [{"path": NodePath(".:position"),
// "spawn": true, "replication_mode": 1}, ...])`. The property paths stay Godot paths (relative to the synchronizer's root_path, the property named after the colon,
// in Godot's names); the runtime reads and writes them with Object.Get / Set. The spawner's spawnable scene list is written as the converted level paths.
#include "FabricInternal.hpp"

#include <algorithm>
#include <map>

namespace aver::fabric {

namespace {

struct RepProp {
    int index = 0;
    std::string path;
    int line = 0;
    bool spawn = true;
    int mode = 1;          // 0 never, 1 always, 2 on change
    bool hasPath = false;
    bool hasMode = false;
    bool legacySync = false, legacyWatch = false, hasLegacy = false;
};

bool ignorable(const std::string& n) {
    return n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script";
}

bool skipKey(const std::string& k) {
    return k == "_name" || k == "script" || k == "unique_name_in_owner" || k.rfind("metadata/", 0) == 0 || k == "process_mode" || k == "process_priority" ||
           k == "process_physics_priority" || k == "editor_description" || k == "scene_file_path";
}

// Walks a relative node path through the converted scene tree; null when a step leaves the tree or the node is not in the scene file.
const FNode* walk(const FNode* from, const std::string& path) {
    const FNode* cur = from;
    size_t i = 0;
    while (cur && i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string seg = path.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { cur = cur->parent; continue; }
        cur = cur->findChild(seg);
    }
    return cur;
}

}  // namespace

bool isReplicationResourceType(const std::string& t) { return t == "SceneReplicationConfig"; }

Var Converter::replicationConfigObject(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line) {
    std::map<int, RepProp> byIndex;
    int unknown = 0;
    for (const gd::GodotProperty& p : props) {
        if (ignorable(p.name)) continue;
        // properties/<n>/path | spawn | replication_mode (4.2+) | sync | watch (4.0, 4.1)
        if (p.name.rfind("properties/", 0) != 0) {
            ++unknown;
            report_.add("property", doc.file, w.node, "SceneReplicationConfig", p.name, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
            continue;
        }
        const size_t slash = p.name.find('/', 11);
        if (slash == std::string::npos) { ++unknown; continue; }
        const int idx = std::atoi(p.name.substr(11, slash - 11).c_str());
        const std::string field = p.name.substr(slash + 1);
        RepProp& r = byIndex[idx];
        r.index = idx;
        if (r.line == 0) r.line = p.line;
        if (field == "path") { r.path = p.value.asString(); r.hasPath = true; }
        else if (field == "spawn") r.spawn = p.value.asBool(true);
        else if (field == "replication_mode") { r.mode = static_cast<int>(p.value.asInt(1)); r.hasMode = true; }
        else if (field == "sync") { r.legacySync = p.value.asBool(false); r.hasLegacy = true; }
        else if (field == "watch") { r.legacyWatch = p.value.asBool(false); r.hasLegacy = true; }
        else {
            ++unknown;
            report_.add("property", doc.file, w.node, "SceneReplicationConfig", p.name, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
        }
    }

    Var list;
    list.type = VType::Array;
    int spawnCount = 0, always = 0, onChange = 0, dropped = 0;
    for (auto& [idx, r] : byIndex) {
        if (!r.hasMode && r.hasLegacy) r.mode = r.legacyWatch ? 2 : (r.legacySync ? 1 : 0);
        const size_t colon = r.path.find(':');
        if (!r.hasPath || r.path.empty() || colon == std::string::npos || colon + 1 >= r.path.size()) {
            ++dropped;
            report_.add("property", doc.file, w.node, "SceneReplicationConfig", "properties/" + std::to_string(idx) + "/path", Rating::Unsupported,
                        "the path '" + r.path + "' names no property (a replicated path is node:property, '.:position' for the synchronizer's root); the entry is dropped", r.line);
            continue;
        }
        if (r.mode < 0 || r.mode > 2) {
            ++dropped;
            report_.add("property", doc.file, w.node, "SceneReplicationConfig", "properties/" + std::to_string(idx) + "/replication_mode", Rating::Unsupported,
                        "replication mode " + std::to_string(r.mode) + " does not exist (0 never, 1 always, 2 on change); the entry is dropped", r.line);
            continue;
        }
        if (r.path[0] == '/')
            report_.add("property", doc.file, w.node, "SceneReplicationConfig", "properties/" + std::to_string(idx) + "/path", Rating::Approximated,
                        "an absolute path: Godot resolves replicated paths from the synchronizer's root_path, so this finds nothing at run time", r.line);
        if (r.spawn) ++spawnCount;
        if (r.mode == 1) ++always;
        if (r.mode == 2) ++onChange;
        Var np;
        np.type = VType::NodePath;
        np.s = r.path;
        list.items.push_back(makeDict({{"path", np}, {"spawn", makeBool(r.spawn)}, {"replication_mode", makeInt(r.mode)}}));
    }
    report_.add("resource", doc.file, w.node, "SceneReplicationConfig", "", (dropped || unknown) ? Rating::Approximated : Rating::Converted,
                std::to_string(list.items.size()) + " replicated propert" + (list.items.size() == 1 ? "y" : "ies") + ": " + std::to_string(spawnCount) + " sent with the spawn, " +
                    std::to_string(always) + " synced every interval, " + std::to_string(onChange) + " synced on change; read at run time by Aver.Fabric's MultiplayerSynchronizer" +
                    (dropped ? "; " + std::to_string(dropped) + " entr" + (dropped == 1 ? "y" : "ies") + " dropped" : std::string()),
                line);
    return makeObject("SceneReplicationConfig", {{"properties", std::move(list)}});
}

Var Converter::spawnableScenesValue(const Var& v, const FNode& fn, const Prop& pr) {
    Var out;
    out.type = VType::PackedStringArray;
    std::vector<std::string> paths = v.strs;
    for (const Var& it : v.items) paths.push_back(it.asString());   // an untyped array of strings
    int missing = 0;
    for (const std::string& res : paths) {
        const bool scene = res.size() > 5 && (res.compare(res.size() - 5, 5, ".tscn") == 0 || res.compare(res.size() - 4, 4, ".scn") == 0);
        if (!scene || !fs_.exists(res)) {
            ++missing;
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, Rating::Unsupported,
                        "the spawnable scene " + res + (scene ? " does not exist in the project" : " is not a scene file") + "; it is dropped from the list (the list position is what travels on the wire)", pr.line);
            out.strs.push_back("");   // keep the positions
            continue;
        }
        out.strs.push_back(levelAssetFor(res));
        report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, Rating::Converted,
                    "spawnable scene " + res + " is listed as the converted level " + levelAssetFor(res) + "; an instance of it added under spawn_path is replicated", pr.line);
    }
    return out;
}

void Converter::noteMultiplayerNode(const FNode& fn) {
    const bool spawner = fn.type == "MultiplayerSpawner";
    const std::vector<const char*> known = spawner ? std::vector<const char*>{"spawn_path", "spawn_limit", "_spawnable_scenes"}
                                                   : std::vector<const char*>{"root_path", "replication_config", "replication_interval", "delta_interval", "public_visibility", "visibility_update_mode"};
    for (const Prop& pr : fn.props) {
        const std::string& k = pr.key;
        if (skipKey(k)) continue;
        bool done = false;
        for (const char* kn : known)
            if (k == kn) {
                // _spawnable_scenes and replication_config are rated where their values convert
                if (k != "_spawnable_scenes" && k != "replication_config")
                    report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Converted, "Aver.Fabric " + fn.type + " reads it", pr.line);
                done = true;
                break;
            }
        if (!done) report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Unsupported, "Aver.Fabric " + fn.type + " has no such property", pr.line);
    }
    if (spawner) {
        const Prop* path = nullptr;
        for (const Prop& pr : fn.props) if (pr.key == "spawn_path") path = &pr;
        std::string target = path ? path->value.asString() : "";
        if (!path || target.empty())
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "no spawn_path: the spawner replicates nothing until a script sets one", 0);
        else if (target[0] != '/' && !walk(&fn, target))
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "spawn_path '" + target + "' names no node of this scene file (a script may create it before the spawner needs it)", path->line);
        else
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Converted,
                        "scenes of the spawnable list added under spawn_path by the spawner's authority (the server unless set_multiplayer_authority says otherwise) appear on every peer, with the synchronizers' spawn state, and are freed with it; spawn() needs a spawn_function set by a script", 0);
    } else {
        report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Converted,
                    "copies the properties of replication_config from the node's multiplayer authority to the other peers (SceneMultiplayer, G6 NET2); a script must give the tree a MultiplayerPeer", 0);
        const Prop* rc = nullptr;
        for (const Prop& pr : fn.props) if (pr.key == "replication_config") rc = &pr;
        if (!rc || rc->value.isNil())
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated, "no replication_config: the synchronizer replicates nothing until a script sets one", 0);
    }
}

}  // namespace aver::fabric
