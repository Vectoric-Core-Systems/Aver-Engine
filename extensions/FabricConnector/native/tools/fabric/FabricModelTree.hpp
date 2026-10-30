#pragma once
// The node tree Godot's glTF importer makes for a model file (names, classes, transforms, skeletons), so a .tscn that instances the model with
// editable children can name nodes inside it ("Player/Skeleton/Skeleton3D/Robot") and hang children on them. Behaviour as Godot 4 shows it in
// scenes; docs/plans/fabric/G3_SCENES.md.
#include "FabricMath.hpp"

#include <string>
#include <vector>

namespace aver::fabric {

struct ModelBone {
    std::string name;
    int parent = -1;   // index into the skeleton's bones, -1 for a root bone
    Xform rest;        // local rest pose, metres
};

struct ModelTreeNode {
    std::string name;
    std::string type;                 // Node3D, MeshInstance3D, Skeleton3D, BoneAttachment3D, AnimationPlayer
    int parent = -1;
    std::vector<int> children;        // in Godot's child order
    int gltfNode = -1;                // the glTF node this stands for; -1 for the nodes the importer adds
    Xform xf;                         // local transform in Godot's metres; identity for a node with no transform
    bool skinned = false;             // a mesh node that is moved below its Skeleton3D (the node's own transform is dropped)
    std::vector<ModelBone> bones;     // Skeleton3D
    std::string boneName;             // BoneAttachment3D
};

struct ModelTree {
    std::vector<ModelTreeNode> nodes;   // [0] is the root (its name is the instancing node's); creation order, not child order
    std::vector<std::string> notes;     // what the tree could not keep, for the report
    bool singleRoot = false;
};

// Reads a .glb or .gltf file's JSON (the buffers are not touched). False with `why` when the file is not glTF 2.
bool buildModelTree(const std::string& path, ModelTree& out, std::string& why);

}  // namespace aver::fabric
