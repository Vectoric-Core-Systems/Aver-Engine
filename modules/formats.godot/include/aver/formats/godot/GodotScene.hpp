#pragma once
// The scene (.tscn) and resource (.tres) document model. Contract: docs/plans/fabric/G3_FORMATS.md.
//
// A scene is: ext resources (other files), sub resources (embedded objects with properties), a node
// list in file order (parents before children, parent paths already resolved to indices),
// signal connections and `[editable]` paths. Property values are raw Variants; nothing here knows what
// a Node3D or a StandardMaterial3D is, and no unit or axis conversion happens (Godot metres, +Y up).
#include "aver/formats/godot/GodotSections.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::godot {

// [ext_resource type="PackedScene" uid="uid://..." path="res://x.tscn" id="1_abc"]
struct GodotExtResource {
    std::string id;    // "1_abc" (format 3) or "1" (format 2)
    std::string type;
    std::string path;  // res:// path as written
    std::string uid;   // "uid://..." or empty
    int line = 0;
};

// [sub_resource type="BoxMesh" id="BoxMesh_x"] + its property lines.
struct GodotSubResource {
    std::string id;
    std::string type;
    std::vector<GodotProperty> props;
    int line = 0;

    const GodotVariant* prop(std::string_view name) const;
};

// [node name="X" type="T" parent="." instance=ExtResource("id") groups=[...] ...] + property lines.
struct GodotNode {
    std::string name;
    std::string type;         // empty for an instance or an override of a node inside an instance
    bool isRoot = false;      // the first node: it has no parent attribute
    std::string parentPath;   // the parent attribute as written ("." = the root); empty for the root
    std::string path;         // scene-relative path: "." for the root, else "A/B" (parent path + name)
    int parent = -1;          // index in GodotScene::nodes of the parent; -1 for the root or when the
                              // parent is a node INSIDE an instanced scene (see unresolvedParent)
    bool unresolvedParent = false;  // parentPath names a node that lives inside an instanced sub-scene
    int anchor = -1;          // when unresolvedParent: the nearest node of this file on that path (the
                              // instance node that hosts it), or -1
    bool isOverride = false;  // no type and no instance on a non-root node: it edits a node that comes
                              // from an instanced scene (or an inherited base)
    std::string instance;     // ext resource id of instance=ExtResource("id"); empty if not an instance
    std::string instancePlaceholder;  // instance_placeholder="res://..."
    std::string owner;        // owner attribute as written (usually ".")
    std::vector<std::string> groups;
    std::vector<std::string> nodePaths;  // node_paths=PackedStringArray(...): properties holding NodePaths
    std::string uniqueId;     // unique_id attribute (Godot 4.5+), as text
    int index = -1;           // index attribute (sibling order override), -1 if absent
    std::vector<GodotProperty> attrs;  // every header attribute as written
    std::vector<GodotProperty> props;  // the node's property lines
    int line = 0;

    const GodotVariant* prop(std::string_view name) const;
    bool isInstance() const { return !instance.empty() || !instancePlaceholder.empty(); }
};

// [connection signal="s" from="a" to="b" method="m" flags=N binds=[...] unbinds=N]
struct GodotConnection {
    std::string signal;
    std::string from;  // node paths relative to the scene root, "." = root
    std::string to;
    std::string method;
    int flags = 0;     // Godot's CONNECT_* flag bits
    int unbinds = 0;
    std::vector<GodotVariant> binds;
    int line = 0;
};

// The [gd_scene ...] / [gd_resource ...] header.
struct GodotDocHeader {
    std::string kind;          // "gd_scene" or "gd_resource"
    std::string type;          // gd_resource type="..." (the main resource's class); empty for scenes
    std::string scriptClass;   // gd_resource script_class="..."
    int format = 0;            // 3 for Godot 4; 2 for Godot 3 (accepted, with a warning)
    int loadSteps = 0;
    std::string uid;           // uid="uid://..." or empty
    int line = 0;
};

struct GodotDocBase {
    std::string file;  // as given to the parser (a res:// path when loaded through GodotProjectFs)
    GodotDocHeader header;
    std::vector<GodotExtResource> extResources;
    std::vector<GodotSubResource> subResources;

    const GodotExtResource* findExt(std::string_view id) const;
    const GodotSubResource* findSub(std::string_view id) const;
};

struct GodotScene : GodotDocBase {
    std::vector<GodotNode> nodes;  // file order; nodes[0] is the root when present
    std::vector<GodotConnection> connections;
    std::vector<std::string> editablePaths;  // [editable path="Inst"]: instance children opened for editing

    // Index of the node with this scene-relative path ("." = root), or -1.
    int findNode(std::string_view path) const;
    // Children of node `index` in this file, in file order (the `index` attribute is not applied).
    std::vector<int> childrenOf(int index) const;
    // The root node's instance ext-resource id when the scene inherits from another scene, else empty.
    std::string inheritsFrom() const { return nodes.empty() ? std::string() : nodes[0].instance; }
};

struct GodotResource : GodotDocBase {
    std::vector<GodotProperty> props;  // the [resource] section: the main resource's properties
    const GodotVariant* prop(std::string_view name) const;
};

// Parses a .tscn. Format 3 is read; format 2 (Godot 3) reads with a warning; a newer format warns.
// True when this call added no errors; `out` holds whatever was readable either way.
bool parseGodotScene(std::string_view text, std::string_view file, GodotScene& out, GodotDiagnostics& diag);
bool parseGodotResource(std::string_view text, std::string_view file, GodotResource& out, GodotDiagnostics& diag);

// Reads the file from disk (binary .scn/.res are reported as "resave as text").
bool loadGodotScene(const std::string& path, GodotScene& out, GodotDiagnostics& diag, std::string_view displayName = {});
bool loadGodotResource(const std::string& path, GodotResource& out, GodotDiagnostics& diag,
                       std::string_view displayName = {});

// "gd_scene" / "gd_resource" from the first section of a text file; empty if it is neither.
std::string detectGodotTextKind(std::string_view text);

}  // namespace aver::fmt::godot
