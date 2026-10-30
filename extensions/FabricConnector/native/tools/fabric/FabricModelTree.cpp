// The node tree Godot's glTF importer makes for a model file. See FabricModelTree.hpp.
#include "FabricModelTree.hpp"

#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace aver::fabric {

namespace {

using fmt::JsonValue;

// Godot's String::validate_node_name: the characters a node name cannot hold become underscores ("Cube.001" is "Cube_001").
std::string validNodeName(std::string s) {
    for (char& c : s)
        if (c == '.' || c == ':' || c == '@' || c == '/' || c == '"' || c == '%') c = '_';
    return s;
}

// A glTF node's local transform: its matrix, or translation, rotation and scale. `has` tells whether the node states one.
Xform nodeLocal(const JsonValue& n, bool& has) {
    Xform x;
    has = false;
    if (n.has("matrix") && n["matrix"].size() >= 16) {
        const JsonValue& m = n["matrix"];   // column-major
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) x.basis.m[r][c] = m[static_cast<usize>(c * 4 + r)].asDouble();
        x.pos = {m[12].asDouble(), m[13].asDouble(), m[14].asDouble()};
        has = true;
        return x;
    }
    V3 s{1, 1, 1};
    if (n.has("scale") && n["scale"].size() >= 3) { s = {n["scale"][0].asDouble(1), n["scale"][1].asDouble(1), n["scale"][2].asDouble(1)}; has = true; }
    if (n.has("rotation") && n["rotation"].size() >= 4) {
        const JsonValue& q = n["rotation"];
        x.basis = basisFromQuat({q[0].asDouble(), q[1].asDouble(), q[2].asDouble(), q[3].asDouble(1)});
        has = true;
    }
    x.basis = x.basis * M3::diag(s);
    if (n.has("translation") && n["translation"].size() >= 3) {
        x.pos = {n["translation"][0].asDouble(), n["translation"][1].asDouble(), n["translation"][2].asDouble()};
        has = true;
    }
    return x;
}

}  // namespace

bool buildModelTree(const std::string& path, ModelTree& out, std::string& why) {
    out = ModelTree{};
    std::vector<u8> bytes;
    if (!readFileBytes(path, bytes)) { why = "could not read " + path; return false; }
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (bytes.size() >= 20 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        const auto le32 = [&](size_t at) {
            return static_cast<u32>(bytes[at]) | static_cast<u32>(bytes[at + 1]) << 8 | static_cast<u32>(bytes[at + 2]) << 16 | static_cast<u32>(bytes[at + 3]) << 24;
        };
        const u32 len = le32(12);
        if (le32(16) != 0x4E4F534Au || static_cast<size_t>(len) + 20 > bytes.size()) { why = "the GLB has no JSON chunk"; return false; }
        text = text.substr(20, len);
    }
    JsonValue doc;
    if (!fmt::parseJson(text, doc, &why)) return false;
    if (!doc.isObject()) { why = "not a glTF document"; return false; }

    const JsonValue& jn = doc["nodes"];
    const int N = static_cast<int>(jn.size());
    const auto at = [](int i) { return static_cast<size_t>(i); };

    // ---- the node graph ----
    std::vector<int> par(at(N), -1);
    std::vector<std::vector<int>> kids(at(N));
    for (int i = 0; i < N; ++i) {
        const JsonValue& ch = jn[at(i)]["children"];
        for (usize k = 0; k < ch.size(); ++k) {
            const int c = static_cast<int>(ch[k].asInt(-1));
            if (c < 0 || c >= N || c == i || par[at(c)] >= 0) continue;
            par[at(c)] = i;
            kids[at(i)].push_back(c);
        }
    }
    std::vector<int> roots;
    const int sceneIdx = static_cast<int>(doc["scene"].asInt(0));
    const JsonValue& scenes = doc["scenes"];
    if (sceneIdx >= 0 && static_cast<usize>(sceneIdx) < scenes.size()) {
        const JsonValue& rn = scenes[static_cast<usize>(sceneIdx)]["nodes"];
        for (usize k = 0; k < rn.size(); ++k) {
            const int c = static_cast<int>(rn[k].asInt(-1));
            if (c >= 0 && c < N) roots.push_back(c);
        }
    } else {
        for (int i = 0; i < N; ++i)
            if (par[at(i)] < 0) roots.push_back(i);
    }

    // ---- names: unique across the file, in node order (a repeat gets a number after it) ----
    std::set<std::string> used;
    const auto uniqueName = [&](const std::string& raw) {
        const std::string base = validNodeName(raw);
        std::string cand = base;
        int n = 1;
        while (used.count(cand)) cand = base + std::to_string(++n);
        used.insert(cand);
        return cand;
    };
    std::vector<std::string> nm(at(N));
    for (int i = 0; i < N; ++i) {
        std::string raw(jn[at(i)]["name"].asString());
        if (raw.empty()) raw = jn[at(i)].has("mesh") ? "MeshInstance3D" : "Node3D";
        nm[at(i)] = uniqueName(raw);
    }

    // ---- skeletons: skins that share a joint are one skeleton; its bones are the joints and the nodes between them ----
    const JsonValue& skins = doc["skins"];
    const int S = static_cast<int>(skins.size());
    std::vector<int> uf(at(S));
    std::iota(uf.begin(), uf.end(), 0);
    const std::function<int(int)> find = [&](int a) { return uf[at(a)] == a ? a : uf[at(a)] = find(uf[at(a)]); };
    std::vector<int> jointSkin(at(N), -1);
    for (int s = 0; s < S; ++s) {
        const JsonValue& js = skins[at(s)]["joints"];
        for (usize k = 0; k < js.size(); ++k) {
            const int j = static_cast<int>(js[k].asInt(-1));
            if (j < 0 || j >= N) continue;
            if (jointSkin[at(j)] < 0) jointSkin[at(j)] = s;
            else uf[at(find(s))] = find(jointSkin[at(j)]);
        }
    }
    std::map<int, std::vector<int>> groupJoints;   // skeleton (its skin group) -> joint nodes
    for (int s = 0; s < S; ++s) {
        const JsonValue& js = skins[at(s)]["joints"];
        std::vector<int>& list = groupJoints[find(s)];
        for (usize k = 0; k < js.size(); ++k) {
            const int j = static_cast<int>(js[k].asInt(-1));
            if (j >= 0 && j < N && std::find(list.begin(), list.end(), j) == list.end()) list.push_back(j);
        }
    }
    const auto ancestorOrSelf = [&](int a, int b) {
        for (int n = b, guard = 0; n >= 0 && guard <= N; n = par[at(n)], ++guard)
            if (n == a) return true;
        return false;
    };
    std::vector<int> boneGroup(at(N), -1);
    std::map<int, int> groupParent;   // skeleton -> the node its bones hang from (-1: the scene root)
    for (const auto& [g, js] : groupJoints) {
        if (js.empty()) continue;
        int lca = js[0];
        for (const int j : js)
            while (lca >= 0 && !ancestorOrSelf(lca, j)) lca = par[at(lca)];
        const int p = lca >= 0 && std::find(js.begin(), js.end(), lca) != js.end() ? par[at(lca)] : lca;
        groupParent[g] = p;
        for (const int j : js)
            for (int n = j, guard = 0; n >= 0 && n != p && guard <= N; n = par[at(n)], ++guard) {
                if (boneGroup[at(n)] >= 0 && boneGroup[at(n)] != g) break;
                boneGroup[at(n)] = g;
            }
    }

    // ---- the tree ----
    ModelTree& T = out;
    T.nodes.emplace_back();
    T.nodes[0].type = "Node3D";
    const auto add = [&](int parent, const std::string& name, const std::string& type) {
        ModelTreeNode n;
        n.name = name;
        n.type = type;
        n.parent = parent;
        T.nodes.push_back(std::move(n));
        const int idx = static_cast<int>(T.nodes.size()) - 1;
        T.nodes[at(parent)].children.push_back(idx);
        return idx;
    };
    // Godot's add_child(legible_unique_name): the class name, then the class name with 2, 3, ...
    const auto siblingName = [&](int parent, const std::string& base) {
        std::string cand = base;
        for (int n = 2;; ++n) {
            bool taken = false;
            for (const int c : T.nodes[at(parent)].children) taken = taken || T.nodes[at(c)].name == cand;
            if (!taken) return cand;
            cand = base + std::to_string(n);
        }
    };

    std::vector<int> treeOf(at(N), -1), boneIndex(at(N), -1);
    std::vector<char> seen(at(N), 0);
    std::map<int, int> skelNode;                    // skeleton -> its Skeleton3D tree node
    std::map<int, std::vector<Xform>> boneWorld;    // skeleton -> each bone's rest pose in the skeleton's space
    bool foreignNode = false;

    std::function<void(int, int, int)> gen;
    gen = [&](int n, int sceneParent, int depth) {
        if (depth > 4096 || seen[at(n)]) return;
        seen[at(n)] = 1;
        if (boneGroup[at(n)] >= 0) {
            // A joint is a bone, not a node. The skeleton node appears where its first bone is met.
            const int g = boneGroup[at(n)];
            if (!skelNode.count(g)) {
                const int sk = add(sceneParent, uniqueName("Skeleton3D"), "Skeleton3D");
                skelNode[g] = sk;
                std::vector<ModelBone> bones;
                std::vector<Xform> world;
                std::function<void(int, int)> walk;
                walk = [&](int b, int pb) {
                    if (boneGroup[at(b)] != g || boneIndex[at(b)] >= 0 || bones.size() >= 4096) return;   // a hostile file's chain must not run the stack out
                    bool has = false;
                    ModelBone mb;
                    mb.name = nm[at(b)];
                    mb.parent = pb;
                    mb.rest = nodeLocal(jn[at(b)], has);
                    const int idx = static_cast<int>(bones.size());
                    boneIndex[at(b)] = idx;
                    world.push_back(pb >= 0 ? world[at(pb)].composed(mb.rest) : mb.rest);
                    bones.push_back(std::move(mb));
                    for (const int c : kids[at(b)]) walk(c, idx);
                };
                const int p = groupParent[g];
                if (p >= 0) for (const int c : kids[at(p)]) walk(c, -1);
                else for (const int r : roots) walk(r, -1);
                T.nodes[at(sk)].bones = std::move(bones);
                boneWorld[g] = std::move(world);
            }
            const int sk = skelNode[g];
            for (const int c : kids[at(n)]) {
                if (boneGroup[at(c)] == g) {
                    gen(c, sceneParent, depth + 1);
                } else {   // a node hung on a bone is held by a BoneAttachment3D below the skeleton
                    const int att = add(sk, siblingName(sk, "BoneAttachment3D"), "BoneAttachment3D");
                    T.nodes[at(att)].boneName = nm[at(n)];
                    const std::vector<Xform>& bw = boneWorld[g];
                    if (boneIndex[at(n)] >= 0 && at(boneIndex[at(n)]) < bw.size()) T.nodes[at(att)].xf = bw[at(boneIndex[at(n)])];
                    gen(c, att, depth + 1);
                }
            }
            return;
        }
        const JsonValue& jnode = jn[at(n)];
        const int t = add(sceneParent, nm[at(n)], jnode.has("mesh") ? "MeshInstance3D" : "Node3D");
        bool has = false;
        T.nodes[at(t)].xf = nodeLocal(jnode, has);
        T.nodes[at(t)].gltfNode = n;
        treeOf[at(n)] = t;
        if (jnode.has("camera") || (jnode.has("extensions") && jnode["extensions"].has("KHR_lights_punctual"))) foreignNode = true;
        for (const int c : kids[at(n)]) gen(c, t, depth + 1);
    };

    // The file's one root can be the scene root itself (Godot writes GODOT_single_root); anything else gets a root named for the file.
    bool single = false;
    const JsonValue& ext = doc["extensionsUsed"];
    for (usize k = 0; k < ext.size(); ++k) single = single || ext[k].asString() == "GODOT_single_root";
    if (single && roots.size() == 1) {
        const int r = roots[0];
        bool has = false;
        nodeLocal(jn[at(r)], has);
        single = !has && !jn[at(r)].has("mesh") && boneGroup[at(r)] < 0;
    } else {
        single = false;
    }
    T.singleRoot = single;
    if (single) {
        const int r = roots[0];
        seen[at(r)] = 1;
        T.nodes[0].gltfNode = r;
        for (const int c : kids[at(r)]) gen(c, 0, 0);
    } else {
        for (const int r : roots) gen(r, 0, 0);
    }

    // A skinned mesh is moved below its Skeleton3D, after the bone attachments, with its own transform dropped.
    for (int n = 0; n < N; ++n) {
        if (treeOf[at(n)] < 0 || !jn[at(n)].has("mesh") || !jn[at(n)].has("skin")) continue;
        const int s = static_cast<int>(jn[at(n)]["skin"].asInt(-1));
        if (s < 0 || s >= S) continue;
        const auto it = skelNode.find(find(s));
        if (it == skelNode.end()) continue;
        const int t = treeOf[at(n)], sk = it->second;
        bool below = false;   // the skeleton must not sit below the mesh being moved
        for (int a = sk, guard = 0; a >= 0 && guard <= static_cast<int>(T.nodes.size()); a = T.nodes[at(a)].parent, ++guard) below = below || a == t;
        if (below) continue;
        std::vector<int>& old = T.nodes[at(T.nodes[at(t)].parent)].children;
        old.erase(std::remove(old.begin(), old.end(), t), old.end());
        T.nodes[at(sk)].children.push_back(t);
        T.nodes[at(t)].parent = sk;
        T.nodes[at(t)].skinned = true;
        T.nodes[at(t)].xf = Xform{};
    }

    if (doc["animations"].size() > 0) add(0, siblingName(0, "AnimationPlayer"), "AnimationPlayer");

    if (foreignNode) T.notes.push_back("a glTF camera or light node is kept as a plain Node3D; Godot would make a Camera3D or a light node");
    if (!groupJoints.empty())
        T.notes.push_back("a skeleton's bones are written as Skeleton3D bone properties (name, parent, rest pose); no skinned vertex follows them (see the Skeleton3D note)");
    return true;
}

}  // namespace aver::fabric
