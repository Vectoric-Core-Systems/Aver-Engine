// .tscn to .ocworld: the node tree (instances flattened, overrides applied), then one class placement per node
// exactly as docs/plans/fabric/G3_RUNTIME.md section 1 says.
#include "FabricInternal.hpp"
#include "FabricMeshGen.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/GodotEnvironment.hpp"
#include "aver/formats/GodotLight.hpp"
#include "aver/formats/PanoramaSky.hpp"
#include "aver/formats/OcPrefab.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

namespace aver::fabric {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Godot energy 1 as an Aver sun: pi engine radiance units (104719.755 lux); Aver.Fabric Light3D.LuxPerEnergy is the same number (G4_RENDER.md R2).
constexpr double kLuxPerEnergy = fmt::godotlight::kLuxPerEnergy;

const std::set<std::string>& supportedClasses() {
    static const std::set<std::string> s = {"Node", "Node3D", "Marker3D", "VisualInstance3D", "GeometryInstance3D", "MeshInstance3D",
                                            "Camera3D", "DirectionalLight3D", "OmniLight3D", "SpotLight3D", "CollisionShape3D",
                                            "StaticBody3D", "AnimatableBody3D", "RigidBody3D", "CharacterBody3D", "Area3D", "Timer",
                                            // F2b added these to Aver.Fabric (G3_RUNTIME 1.4)
                                            "RayCast3D", "ShapeCast3D", "VehicleBody3D", "VehicleWheel3D",
                                            // G4 R5: render-to-texture views
                                            "SubViewport", "SubViewportContainer",
                                            // G4 R7: geometry nodes
                                            "MultiMeshInstance3D", "Sprite3D", "Label3D", "GridMap", "CSGShape3D", "CSGCombiner3D", "CSGBox3D", "CSGSphere3D",
                                            "CSGCylinder3D", "CSGTorus3D", "CSGPolygon3D", "CSGMesh3D",
                                            // G4 R8: environment volumes
                                            "FogVolume", "ReflectionProbe", "LightmapGI", "LightmapProbe", "VoxelGI",
                                            // G6 SH3: a particle emitter whose process material is a particles shader
                                            "GPUParticles3D",
                                            // G5 A1: property-track animation
                                            "AnimationPlayer",
                                            // G5 A2: animation trees, skeletons, paths and helper nodes
                                            "AnimationTree", "Skeleton3D", "BoneAttachment3D", "SkeletonIK3D", "Path3D", "PathFollow3D", "SpringArm3D",
                                            "RemoteTransform3D", "VisibleOnScreenNotifier3D", "VisibleOnScreenEnabler3D",
                                            // G5 S1: audio
                                            "AudioStreamPlayer", "AudioStreamPlayer3D", "AudioListener3D",
                                            // G5 N1: navigation
                                            "NavigationRegion3D", "NavigationLink3D", "NavigationObstacle3D", "NavigationAgent3D",
                                            // G6 NET2: multiplayer replication
                                            "MultiplayerSpawner", "MultiplayerSynchronizer",
                                            // G5 U1: the Control base, the containers and Panel; G5 U2's widgets are in FabricWidgets.cpp
                                            "Control", "Container", "BoxContainer", "HBoxContainer", "VBoxContainer", "GridContainer", "MarginContainer", "CenterContainer",
                                            "PanelContainer", "ScrollContainer", "Panel"};
    return s;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// A packed scene file: text (.tscn) or binary (.scn, read by GodotBinary.hpp).
bool isSceneFile(const std::string& res) { return endsWith(res, ".tscn") || endsWith(res, ".scn"); }

bool isBodyClass(const std::string& t) {
    return t == "StaticBody3D" || t == "AnimatableBody3D" || t == "RigidBody3D" || t == "CharacterBody3D" || t == "Area3D";
}

float srgbToLinearF(double c) {
    c = std::max(0.0, std::min(1.0, c));
    return static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

// One Godot resource found through a property: its class, properties and the document its references resolve against.
struct ResView {
    std::string type;
    const std::vector<gd::GodotProperty>* props = nullptr;
    const gd::GodotDocBase* doc = nullptr;
};

}  // namespace

bool isControlClass(const std::string& t) {
    static const std::set<std::string> s = {
        "Control", "Label", "RichTextLabel", "Button", "CheckBox", "CheckButton", "LinkButton", "OptionButton", "MenuButton",
        "TextureButton", "BaseButton", "Panel", "TextureRect", "ColorRect", "NinePatchRect", "LineEdit", "TextEdit", "CodeEdit",
        "SpinBox", "HSlider", "VSlider", "Slider", "ProgressBar", "TextureProgressBar", "ScrollBar", "HScrollBar", "VScrollBar", "Range",
        "ItemList", "Tree", "TabBar", "HSeparator", "VSeparator", "Separator", "GraphEdit", "GraphNode", "GraphElement", "ReferenceRect",
        "ColorPicker", "ColorPickerButton", "VideoStreamPlayer", "MenuBar", "FileDialog", "Popup", "PopupMenu", "PopupPanel",
        "AcceptDialog", "ConfirmationDialog", "HSplitContainer", "VSplitContainer"};
    return s.count(t) != 0 || endsWith(t, "Container");
}

bool is2DClass(const std::string& t) {
    static const std::set<std::string> s = {"CanvasModulate", "ParallaxBackground", "ParallaxLayer", "TileMap", "TileMapLayer",
                                            "CanvasGroup", "BackBufferCopy", "Skeleton2D", "Bone2D"};
    return endsWith(t, "2D") || s.count(t) != 0;
}

namespace {
std::map<std::string, bool>& extensionClassTable() {
    static std::map<std::string, bool> t;
    return t;
}
}  // namespace

void setExtensionClasses(const std::map<std::string, bool>& classes) { extensionClassTable() = classes; }

bool isExtensionClass(const std::string& t, bool* is3D) {
    const auto it = extensionClassTable().find(t);
    if (it == extensionClassTable().end()) return false;
    if (is3D) *is3D = it->second;
    return true;
}

bool isNode3DClass(const std::string& t) {
    static const std::set<std::string> s = {"Node3D", "Decal", "FogVolume", "VoxelGI", "LightmapGI", "ReflectionProbe", "GridMap",
                                            "LightmapProbe", "OccluderInstance3D", "WorldEnvironment3D"};
    bool ext3D = false;
    if (t == "NavigationAgent3D") return false;   // a plain Node under its mover: no transform
    if (isExtensionClass(t, &ext3D)) return ext3D;
    return endsWith(t, "3D") || s.count(t) != 0;
}

ClassChoice chooseClass(const std::string& t) {
    ClassChoice c;
    if (t.empty()) { c.fabricClass = "Node"; return c; }
    if (t == "Skeleton3D") {
        c.fabricClass = t;
        c.rating = Rating::Approximated;
        c.reason = "the skeleton is a script-level pose model (bone list, rests, poses, overrides, bone tracks); it is not connected to a skinned mesh, so posing it moves no vertices";
        return c;
    }
    if (t == "SkeletonIK3D") {
        c.fabricClass = t;
        c.rating = Rating::Approximated;
        c.reason = "solved with FABRIK on the script-level skeleton, not with Godot's solver";
        return c;
    }
    if (t == "CanvasLayer") {
        c.fabricClass = t;
        c.rating = Rating::Approximated;
        c.reason = "its visible flag hides what is drawn on it; the layer number, offset, rotation and scale are kept but not applied (the GUI draws every Control in tree order, in viewport space)";
        return c;
    }
    if (t == "VisibleOnScreenNotifier3D" || t == "VisibleOnScreenEnabler3D") {
        c.fabricClass = t;
        c.rating = Rating::Approximated;
        c.reason = "tested every frame against the current camera's frustum and the box; the renderer's occlusion culling is not consulted";
        return c;
    }
    if (t == "ScrollContainer") {
        c.fabricClass = t;
        c.rating = Rating::Approximated;
        c.reason = "scrolls through scroll_horizontal / scroll_vertical, the wheel and the scroll bars it draws and lets the player drag; the bars have no arrow buttons and get_h_scroll_bar() / get_v_scroll_bar() "
                   "are detached bars that mirror the position";
        return c;
    }
    if (widgetClassChoice(t, c)) return c;   // G5 U2: the widgets
    if (supportedClasses().count(t)) { c.fabricClass = t; return c; }
    bool ext3D = false;
    if (isExtensionClass(t, &ext3D)) {
        c.fabricClass = ext3D ? "GDExtensionNode3D" : "GDExtensionNode";
        c.rating = Rating::Approximated;
        c.reason = "a class registered by a GDExtension plugin: placed as a " + c.fabricClass + " that runs the native class through the Fabric GDExtension host (not run against a real plugin)";
        c.carrier = true;
        return c;
    }
    c.carrier = true;
    if (t == "Viewport" || t == "Window") {
        c.fabricClass = t == "Window" ? "Window" : "Viewport";
        c.rating = Rating::Approximated;
        c.reason = "a viewport is a placeholder in the runtime (only a SubViewport draws a picture)";
        c.carrier = t != c.fabricClass;
        return c;
    }
    if (t == "VideoStreamPlayer") {
        c.fabricClass = "Control";
        c.rating = Rating::Unsupported;
        c.reason = "the engine has no video decoder (modules/media only encodes H.264); the node is kept as a Control with its properties so the tree and its script survive. A decoder dependency "
                   "(for example Theora/Ogg or a platform media-foundation reader) would be needed; see docs/AUDIO.md";
        c.carrier = true;
        return c;
    }
    if (isControlClass(t)) {
        c.fabricClass = "Control";
        c.rating = Rating::Approximated;
        c.reason = "Aver.Fabric has no " + t + " widget: placed as a Control carrying its layout, theme and input properties, so the tree and its scripts survive (its drawing and behaviour are missing)";
        c.carrier = t != "Control";
        return c;
    }
    if (t == "CanvasItem") {
        c.fabricClass = "CanvasItem";
        c.rating = Rating::Approximated;
        c.reason = "a canvas item is a placeholder in the runtime";
        c.carrier = false;
        return c;
    }
    if (t == "WorldEnvironment") {
        c.fabricClass = "WorldEnvironment";
        c.reason = "its Environment becomes the level's SKY / FOG records and a post volume, and the node holds the Environment and CameraAttributes resources";
        return c;
    }
    if (is2DClass(t)) {
        c.fabricClass = "Node";
        c.rating = Rating::Unsupported;
        c.reason = "2D nodes are out of scope; the node is kept as a plain node so the tree and its script survive";
        return c;
    }
    if (isNode3DClass(t)) {
        c.fabricClass = "Node3D";
        c.rating = Rating::Unsupported;
        c.reason = "not in the Aver.Fabric class table yet (G3_RUNTIME 1.4); placed as a Node3D with its properties kept as metadata";
        return c;
    }
    c.fabricClass = "Node";
    c.rating = Rating::Unsupported;
    c.reason = "no Aver.Fabric class for " + t + " yet; placed as a plain node with its properties kept as metadata";
    return c;
}

FNode* FNode::findChild(const std::string& childName) const {
    for (const auto& c : children)
        if (c->name == childName) return c.get();
    return nullptr;
}

std::string Converter::levelAssetFor(const std::string& sceneRes) const {
    std::string rel = sceneRes.rfind("res://", 0) == 0 ? sceneRes.substr(6) : sceneRes;
    const size_t dot = rel.find_last_of('.');
    if (dot != std::string::npos) rel.erase(dot);
    return "Maps/" + rel + ".ocworld";
}

const gd::GodotScene* Converter::sceneDoc(const std::string& resPath) {
    if (const auto it = sceneDocs_.find(resPath); it != sceneDocs_.end()) return it->second.get();
    auto doc = std::make_unique<gd::GodotScene>();
    gd::GodotDiagnostics d;
    const bool ok = fs_.loadScene(resPath, *doc, d);
    for (const gd::GodotDiag& x : d.items) {
        const Rating r = x.severity == gd::GodotSeverity::Error ? Rating::Unsupported : Rating::Approximated;
        if (x.severity != gd::GodotSeverity::Info) report_.add("scene", resPath, "", "", "", r, x.message, x.line);
    }
    if (!ok) {
        sceneDocs_[resPath] = nullptr;
        return nullptr;
    }
    const gd::GodotScene* p = doc.get();
    sceneDocs_[resPath] = std::move(doc);
    return p;
}

// ---- building the tree ---------------------------------------------------------------------------------------------

namespace {

void upsertProp(FNode& fn, const std::string& key, const Var& value, const gd::GodotDocBase* doc, int line) {
    for (Prop& p : fn.props)
        if (p.key == key) {
            p.value = value;
            p.doc = doc;
            p.line = line;
            return;
        }
    fn.props.push_back(Prop{key, value, doc, line});
}

void addGroups(FNode& fn, const std::vector<std::string>& groups) {
    for (const std::string& g : groups)
        if (std::find(fn.groups.begin(), fn.groups.end(), g) == fn.groups.end()) fn.groups.push_back(g);
}

// A transform property of a Node3D family node. False when the property is not one.
bool applyTransformProp(FNode& fn, const gd::GodotProperty& p) {
    const std::string& n = p.name;
    const Var& v = p.value;
    if (n == "rotation_edit_mode" || n == "rotation_order") return true;
    if (n == "transform" && v.type == VType::Transform3D && v.nums.size() >= 12) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) fn.xf.basis.m[r][c] = v.nums[static_cast<size_t>(r) * 3 + c];
        fn.xf.pos = {v.nums[9], v.nums[10], v.nums[11]};
    } else if (n == "basis" && v.type == VType::Basis && v.nums.size() >= 9) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) fn.xf.basis.m[r][c] = v.nums[static_cast<size_t>(r) * 3 + c];
    } else if (n == "position" && v.type == VType::Vector3) {
        fn.xf.pos = vec3Of(&v, {});
    } else if (n == "rotation" && v.type == VType::Vector3) {
        fn.xf.setRotation(basisFromEuler(vec3Of(&v, {}), fn.rotationOrder));
    } else if (n == "rotation_degrees" && v.type == VType::Vector3) {
        const V3 d = vec3Of(&v, {});
        fn.xf.setRotation(basisFromEuler({d.x * kPi / 180, d.y * kPi / 180, d.z * kPi / 180}, fn.rotationOrder));
    } else if (n == "scale" && v.type == VType::Vector3) {
        fn.xf.setScale(vec3Of(&v, {1, 1, 1}));
    } else if (n == "quaternion" && v.type == VType::Quaternion && v.nums.size() >= 4) {
        fn.xf.setRotation(basisFromQuat({v.nums[0], v.nums[1], v.nums[2], v.nums[3]}));
    } else {
        return false;
    }
    fn.hasXform = true;
    return true;
}

// Godot 4 stores an AnimationPlayer's libraries as libraries/<name> properties; the runtime reads one `libraries` dictionary.
void mergeAnimationLibrary(FNode& fn, const gd::GodotProperty& p, const gd::GodotDocBase* doc) {
    const std::string name = p.name.substr(std::strlen("libraries/"));
    Prop* lib = nullptr;
    for (Prop& e : fn.props)
        if (e.key == "libraries") lib = &e;
    if (!lib || lib->doc != doc || lib->value.type != VType::Dictionary) {
        Var d;
        d.type = VType::Dictionary;
        if (lib) { lib->value = std::move(d); lib->doc = doc; lib->line = p.line; }
        else { fn.props.push_back(Prop{"libraries", std::move(d), doc, p.line}); lib = &fn.props.back(); }
    }
    for (size_t i = 0; i < lib->value.keys.size() && i < lib->value.items.size(); ++i)
        if (lib->value.keys[i].asString() == name) { lib->value.items[i] = p.value; return; }
    lib->value.keys.push_back(makeString(name));
    lib->value.items.push_back(p.value);
}

Var xformVar(const Xform& x) {
    Var v;
    v.type = VType::Transform3D;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) v.nums.push_back(x.basis.m[r][c]);
    v.nums.insert(v.nums.end(), {x.pos.x, x.pos.y, x.pos.z});
    return v;
}

bool invertXform(const Xform& x, Xform& out) {
    const M3& a = x.basis;
    const double det = a.determinant();
    if (std::fabs(det) < 1e-12) return false;
    M3 i;
    i.m[0][0] = (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) / det;
    i.m[0][1] = -(a.m[0][1] * a.m[2][2] - a.m[0][2] * a.m[2][1]) / det;
    i.m[0][2] = (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) / det;
    i.m[1][0] = -(a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) / det;
    i.m[1][1] = (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) / det;
    i.m[1][2] = -(a.m[0][0] * a.m[1][2] - a.m[0][2] * a.m[1][0]) / det;
    i.m[2][0] = (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) / det;
    i.m[2][1] = -(a.m[0][0] * a.m[2][1] - a.m[0][1] * a.m[2][0]) / det;
    i.m[2][2] = (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) / det;
    out.basis = i;
    out.pos = (i * x.pos) * -1.0;
    return true;
}

// A Skeleton3D's bones as the bones/N/... properties a scene stores (name, parent, rest, and the pose, which starts at the rest).
void addBoneProps(FNode& f, const ModelTreeNode& tn) {
    for (size_t i = 0; i < tn.bones.size(); ++i) {
        const ModelBone& b = tn.bones[i];
        const std::string k = "bones/" + std::to_string(i) + "/";
        Var rot;
        rot.type = VType::Quaternion;
        const Q4 q = b.rest.quaternion();
        rot.nums = {q.x, q.y, q.z, q.w};
        const V3 s = b.rest.scale();
        f.props.push_back(Prop{k + "name", makeString(b.name), nullptr, 0});
        f.props.push_back(Prop{k + "parent", makeInt(b.parent), nullptr, 0});
        f.props.push_back(Prop{k + "rest", xformVar(b.rest), nullptr, 0});
        f.props.push_back(Prop{k + "enabled", makeBool(true), nullptr, 0});
        f.props.push_back(Prop{k + "position", makeVector3(b.rest.pos.x, b.rest.pos.y, b.rest.pos.z), nullptr, 0});
        f.props.push_back(Prop{k + "rotation", std::move(rot), nullptr, 0});
        f.props.push_back(Prop{k + "scale", makeVector3(s.x, s.y, s.z), nullptr, 0});
    }
}

// The nodes of an instanced model below `root`: the tree Godot's importer makes (`tree`, null when it could not be read), each mesh node showing the
// mesh the importer wrote for it. A mesh is stored with its world rotation and scale baked into the vertices and its position kept apart (the part's
// position), so a mesh node's local transform is the one that puts it at that position under the real transforms of its parents. Meshes the tree has
// no node for (and every mesh when there is no tree) are placed directly below the root.
void buildModelNodes(FNode& root, const ModelTree* tree, const ModelResult& model, const std::string& modelRes) {
    const auto partPosition = [](const ModelPart& part) {
        // The part's position is engine centimetres; the node holds Godot metres.
        Xform x;
        x.pos = {part.y / 100.0, part.z / 100.0, -part.x / 100.0};
        return x;
    };
    std::map<int, const ModelPart*> partOf;
    std::set<int> placed;
    if (tree)
        for (const ModelPart& p : model.parts)
            if (p.node >= 0 && !partOf.count(p.node)) partOf[p.node] = &p;

    std::function<void(int, FNode&, const Xform&, const Xform&, const std::string&)> make;
    make = [&](int idx, FNode& parentF, const Xform& parentActual, const Xform& parentTrue, const std::string& parentPath) {
        const ModelTreeNode& tn = tree->nodes[static_cast<size_t>(idx)];
        const Xform trueWorld = parentTrue.composed(tn.xf);   // where Godot puts the node
        Xform actualWorld = trueWorld;                        // where this node's transform puts it (differs for a mesh: see above)
        const ModelPart* part = nullptr;
        if (tn.type == "MeshInstance3D" && tn.gltfNode >= 0)
            if (const auto it = partOf.find(tn.gltfNode); it != partOf.end()) { part = it->second; placed.insert(tn.gltfNode); }
        if (part) actualWorld = partPosition(*part);
        Xform local = tn.xf;
        Xform inv;
        if (invertXform(parentActual, inv)) local = inv.composed(actualWorld);
        else actualWorld = parentActual.composed(local);

        auto f = std::make_unique<FNode>();
        f->name = tn.name;
        f->type = tn.type;
        f->fromModel = true;
        f->sceneFile = modelRes;
        f->reportFile = modelRes;
        f->reportPath = parentPath.empty() ? tn.name : parentPath + "/" + tn.name;
        f->parent = &parentF;
        f->owner = &root;
        f->xf = local;
        f->hasXform = true;
        if (part) f->props.push_back(Prop{"mesh", makeResource(part->meshAsset), nullptr, 0});
        if (tn.type == "Skeleton3D") addBoneProps(*f, tn);
        if (tn.type == "BoneAttachment3D") f->props.push_back(Prop{"bone_name", makeString(tn.boneName), nullptr, 0});
        FNode& ref = *f;
        parentF.children.push_back(std::move(f));
        for (const int c : tn.children) make(c, ref, actualWorld, trueWorld, ref.reportPath);
    };
    if (tree)
        for (const int c : tree->nodes[0].children) make(c, root, Xform{}, Xform{}, "");

    int k = 0;
    for (const ModelPart& part : model.parts) {
        if (tree && part.node >= 0 && placed.count(part.node)) continue;
        auto child = std::make_unique<FNode>();
        child->name = part.name.empty() ? "Mesh" + std::to_string(k) : part.name;
        child->type = "MeshInstance3D";
        child->fromModel = true;
        child->sceneFile = modelRes;
        child->reportFile = modelRes;
        child->reportPath = child->name;
        child->parent = &root;
        child->owner = &root;
        child->xf = partPosition(part);
        child->hasXform = true;
        child->props.push_back(Prop{"mesh", makeResource(part.meshAsset), nullptr, 0});
        root.children.push_back(std::move(child));
        ++k;
    }
}

}  // namespace

std::unique_ptr<FNode> Converter::buildScene(const std::string& resPath, int depth, const Where& from) {
    const gd::GodotScene* doc = sceneDoc(resPath);
    if (!doc || doc->nodes.empty()) return nullptr;
    if (depth > 24) {
        report_.add("scene", from.file, from.node, "PackedScene", "", Rating::Unsupported, "scenes instance each other more than 24 levels deep (a cycle?)", 0);
        return nullptr;
    }

    std::vector<FNode*> byIndex(doc->nodes.size(), nullptr);
    std::map<std::string, FNode*> byPath;
    std::vector<FNode*> created;   // the nodes this document authored, for ownership
    std::unique_ptr<FNode> root;

    // A path from the scene root, possibly into an instanced sub-scene.
    const auto resolve = [&](const std::string& path) -> FNode* {
        if (path == "." || path.empty()) return byPath.count(".") ? byPath["."] : nullptr;
        std::string head = path;
        std::vector<std::string> tail;
        while (!head.empty() && !byPath.count(head)) {
            const size_t s = head.find_last_of('/');
            tail.push_back(s == std::string::npos ? head : head.substr(s + 1));
            head = s == std::string::npos ? std::string{} : head.substr(0, s);
        }
        FNode* cur = head.empty() ? (byPath.count(".") ? byPath["."] : nullptr) : byPath[head];
        for (auto it = tail.rbegin(); cur && it != tail.rend(); ++it) cur = cur->findChild(*it);
        return cur;
    };

    const auto applyProps = [&](FNode& fn, const std::vector<gd::GodotProperty>& props) {
        for (const gd::GodotProperty& p : props)
            if (p.name == "rotation_order") fn.rotationOrder = static_cast<int>(numOf(&p.value, 2));
        const bool spatial = isNode3DClass(fn.type);
        for (const gd::GodotProperty& p : props) {
            if (p.name == "script") {
                if (p.value.type == VType::ExtResource) {
                    const gd::GodotExtResource* e = doc->findExt(p.value.s);
                    fn.scriptPath = e ? fs_.resolve(*e, &diag_, doc->file) : std::string{};
                } else {
                    fn.scriptPath.clear();
                }
                continue;
            }
            if (spatial && applyTransformProp(fn, p)) continue;
            if (p.name.rfind("libraries/", 0) == 0 && (fn.type == "AnimationPlayer" || fn.type == "AnimationMixer")) { mergeAnimationLibrary(fn, p, doc); continue; }
            if (p.name == "unique_name_in_owner") { fn.unique = boolOf(&p.value, false); continue; }
            upsertProp(fn, p.name, p.value, doc, p.line);
        }
    };

    for (size_t i = 0; i < doc->nodes.size(); ++i) {
        const gd::GodotNode& n = doc->nodes[i];
        const Where w{doc->file, n.path, n.type};

        if (n.isOverride) {
            FNode* target = resolve(n.path);
            if (!target) {
                report_.add("node", doc->file, n.path, "", "", Rating::Unsupported, "an override of a node that the instanced scene does not have", n.line);
                continue;
            }
            applyProps(*target, n.props);
            addGroups(*target, n.groups);
            byIndex[i] = target;
            byPath[n.path] = target;
            continue;
        }

        std::unique_ptr<FNode> made;
        if (n.isInstance()) {
            std::string subRes;
            if (!n.instance.empty()) {
                const gd::GodotExtResource* e = doc->findExt(n.instance);
                if (e) subRes = fs_.resolve(*e, &diag_, doc->file);
            } else {
                subRes = gd::GodotProjectFs::normalizeRes(n.instancePlaceholder);
                report_.add("node", doc->file, n.path, "", "instance_placeholder", Rating::Approximated, "an instance placeholder is instanced at once", n.line);
            }
            const std::string ext = [&] { const size_t d = subRes.find_last_of('.'); return d == std::string::npos ? std::string{} : lower(subRes.substr(d)); }();
            if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj") {
                made = std::make_unique<FNode>();
                made->type = "Node3D";
                made->fromModel = true;
                if (const ModelResult* mr = modelFor(subRes, w)) buildModelNodes(*made, modelTreeFor(subRes, w), *mr, subRes);
                made->instanceOf = subRes;
            } else {
                made = subRes.empty() ? nullptr : buildScene(subRes, depth + 1, w);
                if (!made) {
                    made = std::make_unique<FNode>();
                    made->type = "Node3D";
                    report_.add("node", doc->file, n.path, "", "", Rating::Unsupported, "the instanced scene " + subRes + " could not be read; an empty Node3D stands in", n.line);
                } else {
                    made->instanceOf = subRes;
                    upsertProp(*made, "scene_file_path", makeString(subRes), nullptr, 0);
                    instancedScenes_.insert(subRes);
                }
            }
            made->name = n.name;
            made->owner = nullptr;
            applyProps(*made, n.props);
            addGroups(*made, n.groups);
            // The root of an inherited scene is this scene's own root, not an instance of the base.
            if (n.isRoot && !made->fromModel) upsertProp(*made, "scene_file_path", makeString(resPath), nullptr, 0);
        } else {
            made = std::make_unique<FNode>();
            made->type = n.type;
            made->name = n.name;
            made->sceneFile = doc->file;
            made->line = n.line;
            applyProps(*made, n.props);
            addGroups(*made, n.groups);
        }
        made->reportFile = doc->file;
        made->reportPath = n.path;
        if (made->line == 0) made->line = n.line;
        if (made->sceneFile.empty()) made->sceneFile = doc->file;

        FNode* raw = made.get();
        FNode* parent = nullptr;
        if (n.isRoot) {
            root = std::move(made);
        } else {
            if (n.parent >= 0 && n.parent < static_cast<int>(byIndex.size())) parent = byIndex[static_cast<size_t>(n.parent)];
            else if (n.unresolvedParent) parent = resolve(n.parentPath);
            if (!parent) {
                report_.add("node", doc->file, n.path, n.type, "", Rating::Unsupported, "its parent " + n.parentPath + " was not found; the node is dropped", n.line);
                continue;
            }
            raw->parent = parent;
            size_t at = parent->children.size();
            if (n.index >= 0) at = std::min(at, static_cast<size_t>(n.index));
            parent->children.insert(parent->children.begin() + static_cast<long>(at), std::move(made));
        }
        byIndex[i] = raw;
        byPath[n.path] = raw;
        created.push_back(raw);
    }
    if (!root) return nullptr;

    // Owners: the nodes this scene wrote down belong to its root; a nested instance root belongs to this scene's root too.
    for (FNode* c : created)
        if (c != root.get()) c->owner = root.get();

    // Scene connections: made when the source node is ready, so they ride on the source as `_connections`.
    for (const gd::GodotConnection& c : doc->connections) {
        FNode* source = resolve(c.from);
        FNode* target = resolve(c.to);
        if (!source || !target) {
            report_.add("node", doc->file, c.from, "", "connection:" + c.signal, Rating::Unsupported, "the connection names a node that does not exist (" + c.to + ")", c.line);
            continue;
        }
        source->connections.push_back(Connection{c.signal, target, c.method, c.flags, c.unbinds, c.binds, doc});
    }
    return root;
}

// ---- emitting the tree ---------------------------------------------------------------------------------------------

struct LevelScan {
    bool haveSun = false;
    std::string sunNode;   // report path of the DirectionalLight3D the SUN record came from
    V3 sunToward{0, 1, 0};
    double sunColor[3] = {1, 1, 1};
    double sunEnergy = 1;
    double sunAngular = 0.5;
    bool haveCamera = false, cameraCurrent = false;
    Xform cameraWorld;
    int firstCameraPlacement = -1;   // placement index of the first Camera3D, which becomes current when none says so (Godot's rule)
    std::string firstCameraNode;
    const FNode* environmentNode = nullptr;
    const FNode* sceneRoot = nullptr;
    bool prefabPass = false;
    std::vector<std::pair<std::string, int>> viewports;   // G4 R5: each SubViewport's key and placement index, to write the material consumer counts back
};

namespace {

std::string relPath(const FNode* from, const FNode* to) {
    if (from == to) return ".";
    std::vector<const FNode*> a, b;
    for (const FNode* n = from; n; n = n->parent) a.push_back(n);
    for (const FNode* n = to; n; n = n->parent) b.push_back(n);
    // Trim the common top of both chains (root first).
    size_t common = 0;
    while (common < a.size() && common < b.size() && a[a.size() - 1 - common] == b[b.size() - 1 - common]) ++common;
    std::string out;
    for (size_t i = 0; i + common < a.size(); ++i) out += out.empty() ? ".." : "/..";
    for (size_t i = b.size() - common; i-- > 0;) {
        if (!out.empty()) out += "/";
        out += b[i]->name;
    }
    return out.empty() ? "." : out;
}

// The nearest SubViewport above a node, or null: a camera below one is that view's camera, not the level's.
const FNode* subViewportAbove(const FNode& n) {
    for (const FNode* a = n.parent; a; a = a->parent)
        if (a->type == "SubViewport") return a;
    return nullptr;
}

Var transformVariant(const Xform& x) {
    Var v;
    v.type = VType::Transform3D;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) v.nums.push_back(x.basis.m[r][c]);
    v.nums.insert(v.nums.end(), {x.pos.x, x.pos.y, x.pos.z});
    return v;
}

}  // namespace

std::string viewportKeyOf(const std::string& level, const FNode& n) {
    std::vector<const FNode*> chain;
    for (const FNode* a = &n; a; a = a->parent) chain.push_back(a);
    std::string key = level + ":";
    for (size_t i = chain.size(); i-- > 0;) {
        key += chain[i]->name;
        if (i) key += "/";
    }
    for (char& c : key)   // the key travels inside a .ocmat texture reference and a level Prop: keep it to plain characters
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '/' && c != ':' && c != '-') c = '_';
    return key;
}

void Converter::emitNode(FNode& fn, int parent, const Xform& parentWorld, fmt::OcWorldData& level, LevelScan& scan) {
    const Where w{fn.reportFile, fn.reportPath, fn.type};
    ClassChoice cc = chooseClass(fn.type);
    std::string cls = cc.fabricClass;
    // G4 R5: what the sub-viewport nodes are in the engine, said once on the node (the properties are rated below).
    if (fn.type == "SubViewport") {
        cc.rating = Rating::Approximated;
        cc.reason = "a render-to-texture view drawn by the cheap raster configuration (one sun, flat material colours, no shadows, global illumination, particles, foliage, terrain or water), "
                    "not the main renderer; the engine budgets how many exist and how many draw per frame (RENDER.SUBVIEWPORTS, RENDER.SUBVIEWRENDERS)";
    } else if (fn.type == "SubViewportContainer") {
        cc.rating = Rating::Approximated;
        cc.reason = "shows its SubViewport child over the window at the container's position and size (anchors and container layout are not computed); the view is "
                    "resized to the container with stretch";
    }
    const FNode* const outerEmitting = emitting_;
    emitting_ = &fn;   // viewportReference() reads the node whose properties are being converted, and the level it is written into
    emittingLevel_ = level.name;
    const FNode* const inSubViewport = subViewportAbove(fn);

    // The node's class, or its script's.
    ScriptInfo* si = nullptr;
    if (!fn.scriptPath.empty()) {
        si = script(fn.scriptPath);
        if (si) {
            cls = si->className;
            si->uses.push_back(fn.reportFile + ":" + fn.reportPath);
            if (!si->exists)
                report_.add("script", fn.reportFile, fn.reportPath, fn.type, "script", Rating::Unsupported, "the script file " + fn.scriptPath + " does not exist", fn.line);
        }
    }

    // CollisionPolygon3D becomes a CollisionShape3D with the polygon extruded (Godot's depth, centred on Z).
    Var polygonShape;
    bool havePolygonShape = false;
    if (fn.type == "CollisionPolygon3D") {
        const Var* poly = nullptr;
        double depth = 1.0;
        for (const Prop& p : fn.props) {
            if (p.key == "polygon") poly = &p.value;
            if (p.key == "depth") depth = numOf(&p.value, 1.0);
        }
        std::vector<std::pair<double, double>> pts;
        if (poly && poly->type == VType::PackedVector2Array)
            for (size_t i = 0; i + 1 < poly->nums.size(); i += 2) pts.emplace_back(poly->nums[i], poly->nums[i + 1]);
        if (pts.size() >= 3) {
            const bool convex = polygonIsConvex(pts);
            Var arr;
            arr.type = VType::PackedVector3Array;
            const auto push = [&](size_t i, double z) { arr.nums.insert(arr.nums.end(), {pts[i].first, pts[i].second, z}); };
            if (convex) {
                for (size_t i = 0; i < pts.size(); ++i) { push(i, -depth / 2); push(i, depth / 2); }
                polygonShape = makeObject("ConvexPolygonShape3D", {{"points", std::move(arr)}});
            } else {
                const std::vector<unsigned> tris = polygonTriangulate(pts);
                for (size_t t = 0; t + 2 < tris.size(); t += 3) {   // front and back caps
                    push(tris[t], depth / 2); push(tris[t + 1], depth / 2); push(tris[t + 2], depth / 2);
                    push(tris[t], -depth / 2); push(tris[t + 2], -depth / 2); push(tris[t + 1], -depth / 2);
                }
                for (size_t i = 0; i < pts.size(); ++i) {   // the walls
                    const size_t j = (i + 1) % pts.size();
                    push(i, -depth / 2); push(j, -depth / 2); push(j, depth / 2);
                    push(i, -depth / 2); push(j, depth / 2); push(i, depth / 2);
                }
                polygonShape = makeObject("ConcavePolygonShape3D", {{"data", std::move(arr)}});
            }
            havePolygonShape = true;
            cls = si ? cls : "CollisionShape3D";
            cc.fabricClass = "CollisionShape3D";
            cc.carrier = false;
            cc.rating = Rating::Approximated;
            cc.reason = std::string("CollisionPolygon3D is extruded into a ") + (convex ? "convex" : "triangle-mesh") + " shape";
        }
    }

    // What this node became, for Fabric/nodes.json: the script converter types $Path, get_node and %Unique from it. Recorded once per scene and path.
    {
        const FNode* top = &fn;
        while (top->parent) top = top->parent;
        std::string nodePath = ".";
        for (const FNode* a = &fn; a->parent; a = a->parent) nodePath = nodePath == "." ? a->name : a->name + "/" + nodePath;
        if (!top->reportFile.empty() && sceneNodePaths_[top->reportFile].insert(nodePath).second) {
            SceneNodeRecord rec;
            rec.path = nodePath;
            rec.baseClass = cc.fabricClass;
            rec.cls = cls;
            rec.script = fn.scriptPath;
            rec.owner = fn.owner ? relPath(top, fn.owner) : "";
            rec.unique = fn.unique;
            if (si)
                for (const Prop& pr : fn.props)
                    if (pr.doc && pr.value.type == VType::ExtResource)
                        if (const gd::GodotExtResource* e = pr.doc->findExt(pr.value.s)) {
                            const std::string res = fs_.resolve(*e, nullptr, pr.doc->file);
                            if (isSceneFile(res)) rec.packedScenes.emplace_back(pr.key, res);
                        }
            sceneNodes_[top->reportFile].push_back(std::move(rec));
        }
    }

    // ---- the transform ----
    fmt::OcWorldPlacement p;
    p.asset = "none";
    p.className = cls;
    p.name = fn.name;
    p.parent = parent;
    const bool spatial = isNode3DClass(fn.type) || fn.type == "CollisionPolygon3D";
    Xform local = spatial ? fn.xf : Xform{};
    V3 s = local.scale();
    {
        // Float noise in a saved basis (a unit scale stored as 0.99999994) is not authored scale.
        const auto snap = [](double v) { return std::fabs(v - 1.0) < 1e-5 ? 1.0 : (std::fabs(v + 1.0) < 1e-5 ? -1.0 : v); };
        s = {snap(s.x), snap(s.y), snap(s.z)};
        const auto clean = [](double v) { return v == 0.0 ? 0.0 : v; };   // no negative zero in the file
        const Vec3 pos = fmt::axis::position(static_cast<f32>(local.pos.x), static_cast<f32>(local.pos.y), static_cast<f32>(local.pos.z));
        const Q4 q = local.quaternion();
        const Quat eq = fmt::axis::rotation(static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z), static_cast<f32>(q.w));
        const Vec3 e = world::eulerDegFromQuat(eq);
        const Vec3 sc = fmt::axis::scale(static_cast<f32>(s.x), static_cast<f32>(s.y), static_cast<f32>(s.z));
        p.x = clean(pos.x); p.y = clean(pos.y); p.z = clean(pos.z);
        p.roll = clean(e.x); p.pitch = clean(e.y); p.yaw = clean(e.z);
        p.sx = clean(sc.x); p.sy = clean(sc.y); p.sz = clean(sc.z);
    }
    const Xform worldXf = parentWorld.composed(local);

    // ---- the properties, converted ----
    std::vector<std::pair<std::string, Var>> props;
    const auto findProp = [&](const std::string& k) -> Var* {
        for (auto& kv : props) if (kv.first == k) return &kv.second;
        return nullptr;
    };
    const auto setProp = [&](const std::string& k, Var v, bool onlyIfAbsent) {
        if (Var* f = findProp(k)) { if (!onlyIfAbsent) *f = std::move(v); }
        else props.emplace_back(k, std::move(v));
    };
    const auto eraseProp = [&](const std::string& k) {
        props.erase(std::remove_if(props.begin(), props.end(), [&](const auto& kv) { return kv.first == k; }), props.end());
    };

    props.emplace_back("_name", makeString(fn.name));
    if (fn.owner && fn.owner != &fn) props.emplace_back("_owner", makeString(relPath(&fn, fn.owner)));
    if (fn.unique) props.emplace_back("_unique", makeInt(1));
    if (cc.carrier && !si) props.emplace_back("_class", makeString(fn.type));
    else if (cc.carrier && si) props.emplace_back("_class", makeString(fn.type));
    if (!si && cls.rfind("GDExtensionNode", 0) == 0) props.emplace_back("extension_class", makeString(fn.type));
    if (fn.persistent) props.emplace_back("_persistent", makeInt(1));

    if (fn.type == "AnimationPlayer") noteAnimationPlayer(fn);
    if (fn.type == "AudioStreamPlayer" || fn.type == "AudioStreamPlayer3D" || fn.type == "AudioListener3D") noteAudioNode(fn);
    noteAnimTreeNode(fn);
    noteNavigationNode(fn);
    if (fn.type == "MultiplayerSpawner" || fn.type == "MultiplayerSynchronizer") noteMultiplayerNode(fn);   // G6 NET2
    if (isControlClass(fn.type)) noteUiNode(fn);
    for (const Prop& pr : fn.props) {
        if (fn.type == "CollisionPolygon3D" && havePolygonShape && (pr.key == "polygon" || pr.key == "depth" || pr.key == "margin")) continue;
        if (inSubViewport && fn.type == "Camera3D" && pr.key == "current") {
            // A camera below a SubViewport is that view's camera and must never take the window's view: the node is told under a private name that only a
            // SubViewport honours (Aver.Fabric Camera3D.ViewportCurrent).
            if (boolOf(&pr.value, false)) props.emplace_back("_viewport_current", makeBool(true));
            continue;
        }
        const Where pw{fn.reportFile, fn.reportPath, fn.type};
        if (fn.type == "GridMap" && pr.key == "mesh_library") { props.emplace_back(pr.key, makeNil()); continue; }   // geometryNode reads the library itself
        if (fn.type == "MultiplayerSpawner" && pr.key == "_spawnable_scenes") { props.emplace_back(pr.key, spawnableScenesValue(pr.value, fn, pr)); continue; }   // G6 NET2
        Var cv = pr.doc ? convertValue(pr.value, *pr.doc, pw, pr.key) : pr.value;
        if (cv.isNil() && !pr.value.isNil()) continue;   // a resource that could not be converted; already reported
        // parameters/... (AnimationTree) and bones/... (Skeleton3D) are members the runtime classes read by their scene names.
        const bool dynamicKey = (fn.type == "AnimationTree" && pr.key.rfind("parameters/", 0) == 0) || (fn.type == "Skeleton3D" && pr.key.rfind("bones/", 0) == 0) ||
                                (isControlClass(fn.type) && pr.key.rfind("theme_override_", 0) == 0) || isWidgetDynamicKey(fn.type, pr.key);
        if (pr.key.find('/') != std::string::npos && pr.key.rfind("surface_material_override/", 0) != 0 && !dynamicKey) {
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, Rating::Approximated,
                        "a path-style property is not a member of the runtime class; it is kept as node metadata", pr.line);
        }
        props.emplace_back(pr.key, std::move(cv));
    }
    if (havePolygonShape) props.emplace_back("shape", polygonShape);

    // G5 N1: a navigation mesh that was never baked in the editor is baked by the region when it is ready (an Aver.Fabric extension; Godot leaves it empty).
    if (fn.type == "NavigationRegion3D")
        if (Var* nm = findProp("navigation_mesh"); nm && nm->type == VType::Object && nm->s == "NavigationMesh") {
            bool baked = false;
            for (size_t i = 0; i < nm->keys.size(); ++i)
                if (nm->keys[i].asString() == "polygons" && nm->items[i].count() > 0) baked = true;
            if (!baked) props.emplace_back("bake_on_ready", makeBool(true));
        }

    // G4 R7: baked meshes and materials, and the child nodes, of MultiMeshInstance3D, Label3D, Sprite3D, the CSG family and GridMap.
    std::vector<std::unique_ptr<FNode>> synthetic;
    geometryNode(fn, props, synthetic, cc);

    // MeshInstance3D: surface overrides. The runtime holds one material per mesh instance.
    if (fn.type == "MeshInstance3D") {
        std::vector<std::pair<std::string, Var>> overrides;
        for (auto it = props.begin(); it != props.end();) {
            if (it->first.rfind("surface_material_override/", 0) == 0) { overrides.push_back(*it); it = props.erase(it); }
            else ++it;
        }
        for (auto& [k, v] : overrides) {
            if (k == "surface_material_override/0" && !findProp("material_override")) {
                props.emplace_back("material_override", v);
                report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Approximated,
                            "written as material_override: a mesh instance holds one material in the runtime", 0);
            } else {
                report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Unsupported,
                            "only the first surface's material override is carried", 0);
            }
        }
    }

    // Godot's defaults, made explicit so Aver's own defaults never decide a body's behaviour.
    if (isBodyClass(fn.type)) {
        setProp("collision_layer", makeInt(1), true);
        setProp("collision_mask", makeInt(1), true);
        const Where bw{fn.reportFile, fn.reportPath, fn.type};
        if (Var* pm = findProp("physics_material_override"); pm && pm->type == VType::Dictionary)
            report_.add("property", bw.file, bw.node, fn.type, "physics_material_override", Rating::Approximated,
                        "friction, bounce, rough and absorbent are written as a dictionary; the body node does not read them yet", 0);
    }
    if (fn.type == "RigidBody3D") {
        setProp("mass", makeFloat(1.0), true);
        setProp("gravity_scale", makeFloat(1.0), true);
        const struct { const char* value; const char* mode; const char* project; } damps[] = {
            {"linear_damp", "linear_damp_mode", "physics/3d/default_linear_damp"},
            {"angular_damp", "angular_damp_mode", "physics/3d/default_angular_damp"}};
        for (const auto& d : damps) {
            const int mode = findProp(d.mode) ? static_cast<int>(numOf(findProp(d.mode), 0)) : 0;
            const double own = numOf(findProp(d.value), 0.0);
            const double eff = mode == 1 ? own : own + setting(d.project, 0.1);
            setProp(d.value, makeFloat(eff), false);
            eraseProp(d.mode);
        }
        report_.add("property", fn.reportFile, fn.reportPath, fn.type, "linear_damp", Rating::Converted,
                    "written as the effective damping: the project default plus the body's own, as Godot's Combine mode does", 0);
        for (const char* axis : {"axis_lock_linear_x", "axis_lock_linear_y", "axis_lock_linear_z", "axis_lock_angular_x", "axis_lock_angular_y", "axis_lock_angular_z"})
            if (findProp(axis))
                report_.add("property", fn.reportFile, fn.reportPath, fn.type, axis, Rating::Approximated, "axis locks are kept as properties; the body node does not apply them yet", 0);
    }

    // ---- lights: what each stored property becomes (docs/plans/fabric/G4_RENDER.md R2) ----
    // Godot stores only non-default values, so every key found here was authored. The node class (Aver.Fabric Light3D) pushes them to the
    // engine; the raster local-light path reads Godot's curves, the ray-traced light path and the sun read less, which the reasons say.
    if (!scan.prefabPass && (fn.type == "DirectionalLight3D" || fn.type == "OmniLight3D" || fn.type == "SpotLight3D")) {
        const bool sun = fn.type == "DirectionalLight3D";
        const auto note = [&](const Prop& pr, Rating r, const char* why) {
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, r, why, pr.line);
        };
        for (const Prop& pr : fn.props) {
            const std::string& k = pr.key;
            if (k == "light_energy") note(pr, Rating::Converted, "energy times pi engine radiance units (104719.755 lux for the sun, candela at 1 m for an omni or spot): a white surface under energy E returns E, as in Godot");
            else if (k == "light_color") note(pr, Rating::Converted, "sRGB to linear");
            else if (k == "shadow_enabled") note(pr, Rating::Converted, sun ? "the sun is shadowed by Aver's own cascades or ray-traced shadows" : "a shadow map from the raster local-light path (a cube for an omni); the ray-traced light path traces a shadow ray instead");
            else if (k == "omni_range" || k == "spot_range") note(pr, Rating::Converted, "metres to centimetres; nothing reaches past it");
            else if (k == "omni_attenuation" || k == "spot_attenuation") note(pr, Rating::Converted, "Godot's falloff (linear window times distance^-exponent) in the raster local-light path; the ray-traced path keeps Aver's inverse square");
            else if (k == "spot_angle") note(pr, Rating::Converted, "the cone's half-angle");
            else if (k == "spot_angle_attenuation") note(pr, Rating::Converted, "Godot's cone-edge curve in the raster local-light path; the ray-traced path has a smooth edge from 80 percent of the angle");
            else if (k == "light_specular") note(pr, Rating::Approximated, "scales the specular term of the raster local lights (twice the value, as Godot does); the ray-traced path and the sun ignore it");
            else if (k == "light_size") note(pr, Rating::Approximated, "becomes the emitter radius: the shadow filter width and the highlight size, not a physically soft penumbra");
            else if (k == "light_negative") note(pr, Rating::Approximated, "subtracts light in the raster local-light path only; the pixel is clamped at zero");
            else if (k == "light_indirect_energy") note(pr, Rating::Approximated, "local lights add no indirect light; the sun's indirect light is Aver's global illumination, not scaled per light");
            else if (k == "shadow_bias" || k == "shadow_normal_bias") note(pr, Rating::Approximated, "Godot's units become centimetres of push toward the light and shadow texels of push along the normal; tune by eye");
            else if (k == "shadow_blur") note(pr, Rating::Approximated, "not read; the filter width follows light_size");
            else if (k == "distance_fade_enabled" || k == "distance_fade_begin" || k == "distance_fade_shadow" || k == "distance_fade_length")
                note(pr, Rating::Converted, "the raster local-light path fades the light and its shadow with distance from the camera; the ray-traced path does not");
            else if (k == "omni_shadow_mode") note(pr, Rating::Approximated, "an omni light always uses a cube shadow map");
            else if (k == "light_angular_distance") note(pr, sun ? Rating::Converted : Rating::Approximated, sun ? "written to the SUN record" : "not used by an omni or spot light");
            else if (k == "directional_shadow_mode" || k == "directional_shadow_max_distance" || k == "directional_shadow_split_1" || k == "directional_shadow_split_2" ||
                     k == "directional_shadow_split_3" || k == "directional_shadow_blend_splits" || k == "directional_shadow_fade_start" || k == "directional_shadow_pancake_size")
                note(pr, Rating::Approximated, "the sun's shadow cascades are fitted by Aver, not by these values");
            else if (k == "light_cull_mask") note(pr, Rating::Approximated, sun ? "the sun lights every layer: the engine's sun has no per-instance mask" : "which instances the light affects, in the raster local-light path; the ray-traced light path ignores it");
            else if (k == "light_projector") note(pr, Rating::Unsupported, "projector textures are not converted; Aver lights take a cookie texture (CLight.cookie) instead");
            else if (k == "light_bake_mode") note(pr, Rating::Approximated, "every light is dynamic");
            else if (k == "light_volumetric_fog_energy") note(pr, Rating::Unsupported, "volumetric fog takes the sun and the fog settings, not a per-light multiplier");
            else if (k == "shadow_caster_mask") note(pr, Rating::Approximated, sun ? "the sun's shadows come from every layer" : "which instances cast this light's shadow, in the raster local-light path; the ray-traced path ignores it");
            else if (k == "shadow_reverse_cull_face" || k == "shadow_transmittance_bias" || k == "shadow_opacity" || k == "sky_mode")
                note(pr, Rating::Unsupported, "no equivalent in the engine's shadow or sun model");
        }
        if (sun && scan.haveSun && !scan.sunNode.empty() && scan.sunNode != fn.reportPath)
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "only the first DirectionalLight3D ('" + scan.sunNode + "') is the sun (the level's SUN record, which the engine draws); this one is placed and its values are kept but it lights nothing", fn.line);
    }

    // ---- cameras, render layers and visibility ranges (docs/plans/fabric/G4_RENDER.md R4) ----
    // The Aver.Fabric node classes push these to the engine; here only what each stored key becomes is said.
    if (!scan.prefabPass && (fn.type == "Camera3D" || fn.type == "MeshInstance3D" || fn.type == "GeometryInstance3D" || fn.type == "VisualInstance3D" ||
                             fn.type == "DirectionalLight3D" || fn.type == "OmniLight3D" || fn.type == "SpotLight3D")) {
        const auto note = [&](const Prop& pr, Rating r, const char* why) {
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, r, why, pr.line);
        };
        for (const Prop& pr : fn.props) {
            const std::string& k = pr.key;
            if (fn.type == "Camera3D") {
                if (k == "projection") {
                    const int mode = static_cast<int>(numOf(&pr.value, 0));
                    if (mode == 0) note(pr, Rating::Converted, "perspective");
                    else note(pr, Rating::Approximated, mode == 1
                        ? "orthographic is drawn by the rasteriser only: the ray-driven primary pass is skipped for such a view, and sky, fog and ray-traced reflections still assume a perspective view ray"
                        : "an off-centre frustum: the projection is exact; sky and fog rays use it as a perspective view, the denoisers' view basis assumes a symmetric frustum");
                } else if (k == "keep_aspect") note(pr, Rating::Converted, "fov or size describes the kept dimension, as in Godot");
                else if (k == "size") note(pr, Rating::Converted, "metres to centimetres; the width or height per keep_aspect (an orthographic view, or a frustum at its near plane)");
                else if (k == "frustum_offset") note(pr, Rating::Converted, "shifts the frustum window at the near plane (metres to centimetres)");
                else if (k == "h_offset" || k == "v_offset") note(pr, Rating::Converted, "the camera slides along its own right and up axes");
                else if (k == "cull_mask") note(pr, Rating::Converted, "layers the camera draws; an instance outside it is hidden from the camera but still casts shadows and stays in global illumination and reflections, as in Godot");
                else if (k == "current") note(pr, Rating::Converted, inSubViewport ? "this camera is the camera of the SubViewport above it, never the window's"
                                                                                  : "the camera node that is current drives the view");
                else if (k == "fov" || k == "near" || k == "far") note(pr, Rating::Converted, k == "fov" ? "degrees; vertical, or horizontal with keep_aspect width" : "metres to centimetres");
                else if (k == "doppler_tracking") note(pr, Rating::Unsupported, "audio doppler is not driven by a camera");
            } else if (k == "layers") {
                note(pr, Rating::Converted, fn.type == "MeshInstance3D" || fn.type == "GeometryInstance3D"
                     ? "the instance's render layers (20 bits): tested against a camera's cull_mask and, in the raster local-light path, a light's light_cull_mask and shadow_caster_mask"
                     : "kept; only a mesh instance's layers are used");
            } else if (k == "visibility_range_begin" || k == "visibility_range_end" || k == "visibility_range_begin_margin" || k == "visibility_range_end_margin") {
                note(pr, Rating::Converted, "metres to centimetres; the instance is skipped outside [begin, end] by distance from the camera to its origin, a margin being a hysteresis");
            } else if (k == "visibility_range_fade_mode") {
                if (numOf(&pr.value, 0) == 0) note(pr, Rating::Converted, "hysteresis, as Godot's disabled mode");
                else note(pr, Rating::Approximated, "no dithered fade: the instance appears and disappears with a hard cut at its range");
            } else if (k == "visibility_parent") {
                note(pr, Rating::Unsupported, "fading a visibility dependency in or out is not modelled; the dependency keeps its own range");
            }
        }
    }

    // ---- fog volumes, reflection probes and baked / probe GI nodes (docs/plans/fabric/G4_RENDER.md R8) ----
    // The Aver.Fabric classes keep every property; FogVolume and ReflectionProbe publish theirs to the engine (framework_envfx_abi.h). Said here: what each key becomes.
    if (!scan.prefabPass && (fn.type == "FogVolume" || fn.type == "ReflectionProbe" || fn.type == "LightmapGI" || fn.type == "LightmapProbe" || fn.type == "VoxelGI")) {
        const auto note = [&](const Prop& pr, Rating r, const char* why) {
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, r, why, pr.line);
        };
        if (fn.type == "FogVolume")
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Converted,
                        "a shaped region of extra fog inside Voxi's volumetric fog (the 16 nearest the camera are drawn); like Godot's it shows only while volumetric fog is on, which the project's RENDER.VOLFOG 1 does when the start scene's Environment enables volumetric_fog", fn.line);
        else if (fn.type == "ReflectionProbe")
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "recorded by the engine (box, blend, intensity, box projection, interior, ambient) and queryable from scripts; the renderers do not capture or sample a probe cubemap yet, so surfaces keep Aver's own reflections: ray-traced when ray tracing is on, else the voxel cone trace and the sky", fn.line);
        else if (fn.type == "LightmapGI")
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "Aver has no lightmap baker and its renderers sample no baked lightmap (OcLightmap is storage only), and Godot's .lmbake is a binary resource this converter does not read; the node is kept and its baked light is replaced by Aver's real-time global illumination (RENDER.GI)", fn.line);
        else if (fn.type == "VoxelGI")
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "Aver's voxel / ray-traced global illumination is always real time and follows the camera (RENDER.GI, RENDER.VOXELRES, RENDER.GIDISTANCE); this node's box and baked data are kept and not used", fn.line);
        else
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Approximated,
                        "a LightmapProbe samples a baked lightmap for dynamic objects; with no lightmap the node only marks a position and is kept", fn.line);
        for (const Prop& pr : fn.props) {
            const std::string& k = pr.key;
            if (fn.type == "FogVolume") {
                if (k == "size") note(pr, Rating::Converted, "metres to centimetres, along the node's own axes (the node's scale scales it); ignored for the World shape");
                else if (k == "shape") note(pr, Rating::Converted, "ellipsoid, cone, cylinder, box and world, Godot's numbers; the cone's tip and the cylinder's axis are the node's up axis (the cone's orientation follows Godot's code from memory and is unverified)");
                else if (k == "material") note(pr, Rating::Converted, "a FogMaterial keeps density, albedo, emission, height_falloff and edge_fade; a ShaderMaterial over a fog shader (G6 SH3) replaces them with the shader's DENSITY, ALBEDO and EMISSION");
            } else if (fn.type == "ReflectionProbe") {
                if (k == "size" || k == "origin_offset") note(pr, Rating::Converted, "the influence box and the capture point (metres to centimetres, the node's axes); used for the blend and for box-projected queries");
                else if (k == "box_projection" || k == "blend_distance" || k == "max_distance" || k == "interior" || k == "intensity")
                    note(pr, Rating::Approximated, "recorded and used by the engine's probe queries; nothing samples a capture, so it changes no pixel yet");
                else if (k == "update_mode") note(pr, Rating::Approximated, "recorded; once and always alike have nothing to update, as no capture is taken");
                else if (k == "ambient_mode" || k == "ambient_color" || k == "ambient_color_energy")
                    note(pr, Rating::Approximated, "recorded; the ambient light stays the sky's and the voxel GI's, with no per-probe override");
                else if (k == "cull_mask" || k == "reflection_mask") note(pr, Rating::Approximated, "recorded; reflection_mask limits which receivers a probe selection serves");
                else if (k == "enable_shadows" || k == "mesh_lod_threshold" || k == "resolution")
                    note(pr, Rating::Unsupported, "a capture setting: no capture is taken");
            } else if (fn.type == "LightmapGI") {
                if (k == "light_data") note(pr, Rating::Unsupported, "the baked .lmbake is a binary resource and Aver samples no lightmap: rebake is not possible in Aver, the light is real time");
                else if (k == "environment_mode" || k == "environment_custom_sky" || k == "environment_custom_color" || k == "environment_custom_energy")
                    note(pr, Rating::Approximated, "the bake's sky light; Aver's GI takes the level's sky");
                else if (k == "directional" || k == "interior" || k == "use_texture_for_bounces")
                    note(pr, Rating::Approximated, "a bake option; Aver's GI is real time and has the equivalent built in");
                else if (k == "quality" || k == "bounces" || k == "bounce_indirect_energy" || k == "supersampling" || k == "supersampling_factor" || k == "texel_scale" ||
                         k == "max_texture_size" || k == "use_denoiser" || k == "denoiser_strength" || k == "denoiser_range" || k == "bias" || k == "generate_probes_subdiv")
                    note(pr, Rating::Unsupported, "a bake setting: Aver has no lightmap baker");
                else if (k == "camera_attributes") note(pr, Rating::Unsupported, "the bake's camera attributes");
            } else if (fn.type == "VoxelGI") {
                if (k == "data") note(pr, Rating::Unsupported, "the baked VoxelGIData is binary and not read; Aver's voxel GI is real time");
                else if (k == "size" || k == "subdiv") note(pr, Rating::Approximated, "Aver's GI volume is the project's RENDER.VOXELRES / RENDER.GIDISTANCE around the camera, not this box");
                else if (k == "camera_attributes") note(pr, Rating::Unsupported, "the bake's camera attributes");
            }
        }
    }

    // ---- sub-viewports (docs/plans/fabric/G4_RENDER.md R5) ----
    // The key names the view for the engine and for the materials that show it ('@rt:viewport.<key>'); the Aver.Fabric node pushes the rest.
    if (fn.type == "SubViewport") props.emplace_back("_viewport_key", makeString(viewportKeyOf(level.name, fn)));
    if (!scan.prefabPass && (fn.type == "SubViewport" || fn.type == "SubViewportContainer")) {
        const auto note = [&](const Prop& pr, Rating r, const char* why) {
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, r, why, pr.line);
        };
        for (const Prop& pr : fn.props) {
            const std::string& k = pr.key;
            if (fn.type == "SubViewport") {
                if (k == "size") note(pr, Rating::Converted, "the size in pixels; the engine draws it at this size times scaling_3d_scale, fitted to the project's pixel budget (RENDER.SUBVIEWPIXELS)");
                else if (k == "render_target_update_mode") note(pr, Rating::Converted, "disabled, once, when visible, when parent visible and always, with Godot's numbers; 'when visible' counts the materials the converter found that show the view, a SubViewportContainer on screen and a script's UI");
                else if (k == "render_target_clear_mode") note(pr, Rating::Converted, "always, never or once");
                else if (k == "transparent_bg") note(pr, Rating::Converted, "the target clears to alpha 0");
                else if (k == "scaling_3d_scale") note(pr, Rating::Converted, "multiplies the size the view is drawn at (0.25 to 8)");
                else if (k == "scaling_3d_mode" || k == "fsr_sharpness" || k == "texture_mipmap_bias" || k == "anisotropic_filtering_level") note(pr, Rating::Approximated, "any scaling mode is a plain scale; FSR and filtering options are not applied to a view");
                else if (k == "own_world_3d") note(pr, Rating::Approximated, "the engine has one world; the view always shows it");
                else if (k == "world_3d" || k == "world_2d") note(pr, Rating::Unsupported, "a separate World3D is not supported; the view shows the level's world");
                else if (k == "msaa_3d" || k == "msaa_2d" || k == "screen_space_aa" || k == "use_taa" || k == "use_debanding" || k == "use_occlusion_culling" || k == "mesh_lod_threshold" || k == "positional_shadow_atlas_size" ||
                         k == "positional_shadow_atlas_quad_0" || k == "positional_shadow_atlas_quad_1" || k == "positional_shadow_atlas_quad_2" || k == "positional_shadow_atlas_quad_3" || k == "use_hdr_2d" || k == "vrs_mode" || k == "vrs_update_mode")
                    note(pr, Rating::Approximated, "kept; a view is drawn at one sample with no anti-aliasing, shadow atlas or occlusion culling of its own");
                else if (k == "disable_3d") note(pr, Rating::Approximated, "a view with 3D disabled is given no camera, so nothing is drawn into it");
                else if (k == "debug_draw") note(pr, Rating::Unsupported, "viewport debug draw modes are not available in a view");
                else if (k == "size_2d_override" || k == "size_2d_override_stretch" || k == "canvas_cull_mask" || k == "canvas_item_default_texture_filter" || k == "canvas_item_default_texture_repeat" || k == "snap_2d_transforms_to_pixel" ||
                         k == "snap_2d_vertices_to_pixel" || k == "disable_2d" || k == "gui_embed_subwindows" || k == "gui_disable_input" || k == "gui_snap_controls_to_pixels" || k == "handle_input_locally" || k == "physics_object_picking" ||
                         k == "physics_object_picking_sort" || k == "physics_object_picking_first_only" || k == "audio_listener_enable_2d" || k == "audio_listener_enable_3d" || k == "sdf_oversize" || k == "sdf_scale")
                    note(pr, Rating::Unsupported, "2D, GUI, picking, audio listener and distance-field options of a viewport have no equivalent in a view");
            } else {
                if (k == "stretch") note(pr, Rating::Converted, "the view is resized to the container's size");
                else if (k == "stretch_shrink") note(pr, Rating::Converted, "the view is drawn at the container's size divided by this; the picture fills the container");
                else if (k == "mouse_target") note(pr, Rating::Unsupported, "input is not forwarded into the view");
            }
        }
    }

    // ---- connections ----
    if (!fn.connections.empty()) {
        Var arr;
        arr.type = VType::Array;
        for (const Connection& c : fn.connections) {
            std::vector<std::pair<std::string, Var>> d;
            d.emplace_back("signal", makeString(c.signal));
            d.emplace_back("to", makeString(relPath(&fn, c.to)));
            d.emplace_back("method", makeString(c.method));
            d.emplace_back("flags", makeInt(c.flags));
            Var binds;
            binds.type = VType::Array;
            binds.items = c.binds;
            d.emplace_back("binds", std::move(binds));
            d.emplace_back("unbinds", makeInt(c.unbinds));
            arr.items.push_back(makeDict(std::move(d)));
        }
        props.emplace_back("_connections", std::move(arr));
    }

    if (spatial && local.sheared()) {
        props.emplace_back("transform", transformVariant(local));
        report_.add("node", fn.reportFile, fn.reportPath, fn.type, "transform", Rating::Approximated,
                    "the basis is sheared; the exact transform is written as a property and the placement line carries the nearest rotation and scale", fn.line);
    }

    // ---- the record ----
    fmt::OcPlacedComponent comp;
    comp.type = "Props";
    for (auto& [k, v] : props) comp.kv.emplace_back(k, gd::godotVariantToText(v));
    p.components.push_back(std::move(comp));
    if (fn.type == "GPUParticles3D") emitGpuParticles(fn, props, p, scan.prefabPass);   // G6 SH3
    if (!fn.groups.empty()) {
        fmt::OcPlacedComponent g;
        g.type = "Groups";
        std::string names;
        for (const std::string& n : fn.groups) names += (names.empty() ? "" : ",") + n;
        g.kv.emplace_back("names", names);
        p.components.push_back(std::move(g));
    }

    // ---- report ----
    std::string reason = cc.reason;
    if (si) reason += (reason.empty() ? "" : "; ") + std::string("script ") + fn.scriptPath + " is class " + si->className + " (the script body is a later package)";
    if (!scan.prefabPass) report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", cc.rating, reason, fn.line);

    // ---- what the level needs from this node ----
    if (!scan.prefabPass) {
        if (fn.type == "DirectionalLight3D" && !scan.haveSun) {
            scan.haveSun = true;
            scan.sunNode = fn.reportPath;
            const V3 forward = worldXf.basis.column(2) * -1.0;   // a Godot light shines along its -Z
            scan.sunToward = (forward * -1.0).normalized();
            const Var* c = nullptr;
            const Var* e = nullptr;
            const Var* ang = nullptr;
            for (const Prop& pr : fn.props) {
                if (pr.key == "light_color") c = &pr.value;
                if (pr.key == "light_energy") e = &pr.value;
                if (pr.key == "light_angular_distance") ang = &pr.value;
            }
            if (c && c->type == VType::Color && c->nums.size() >= 3)
                for (int k = 0; k < 3; ++k) scan.sunColor[k] = srgbToLinearF(c->nums[k]);
            scan.sunEnergy = numOf(e, 1.0);
            scan.sunAngular = numOf(ang, 0.5);
        }
        if (fn.type == "Camera3D" && !inSubViewport) {   // a camera below a SubViewport is that view's, not the level's
            bool current = false;
            for (const Prop& pr : fn.props) if (pr.key == "current") current = boolOf(&pr.value, false);
            if (scan.firstCameraPlacement < 0) {
                scan.firstCameraPlacement = static_cast<int>(level.placements.size());
                scan.firstCameraNode = fn.reportPath;
            }
            if (!scan.haveCamera || (current && !scan.cameraCurrent)) {
                scan.haveCamera = true;
                scan.cameraCurrent = current;
                scan.cameraWorld = worldXf;
            }
        }
        if (fn.type == "WorldEnvironment" && !scan.environmentNode) scan.environmentNode = &fn;
    }

    const int index = static_cast<int>(level.placements.size());
    if (fn.type == "SubViewport" && !scan.prefabPass) scan.viewports.emplace_back(viewportKeyOf(level.name, fn), index);
    level.placements.push_back(std::move(p));
    emitting_ = outerEmitting;   // this node's properties are converted; its children set their own
    for (const auto& c : fn.children) {
        if (isCsgClass(fn.type) && isCsgClass(c->type)) {   // a CSG shape below another is part of its root's baked mesh
            if (!scan.prefabPass) report_.add("node", c->reportFile, c->reportPath, c->type, "", Rating::Converted, "folded into the baked mesh of the CSG root above it; it is not a node of its own", c->line);
            continue;
        }
        emitNode(*c, index, worldXf, level, scan);
    }
    for (const auto& sn : synthetic) emitNode(*sn, index, worldXf, level, scan);
}

// ---- the level's environment ---------------------------------------------------------------------------------------------

namespace {

std::optional<ResView> resolveRes(Converter& c, const Var& v, const gd::GodotDocBase& doc) {
    if (v.type == VType::SubResource) {
        if (const gd::GodotSubResource* sr = doc.findSub(v.s)) return ResView{sr->type, &sr->props, &doc};
    } else if (v.type == VType::ExtResource) {
        if (const gd::GodotExtResource* e = doc.findExt(v.s)) {
            const std::string res = c.fs_.resolve(*e, &c.diag_, doc.file);
            if (const gd::GodotResource* r = c.resourceDoc(res)) return ResView{r->header.type, &r->props, r};
        }
    }
    return std::nullopt;
}

const Var* rprop(const ResView& r, const char* name) {
    for (const gd::GodotProperty& p : *r.props)
        if (p.name == name) return &p.value;
    return nullptr;
}

void colorOf(const Var* v, double out[3], const double def[3]) {
    for (int i = 0; i < 3; ++i) out[i] = (v && v->type == VType::Color && v->nums.size() >= 3) ? v->nums[static_cast<size_t>(i)] : def[i];
}

}  // namespace

static EnvInfo readEnvironment(Converter& c, const FNode& wenv) {
    EnvInfo env;
    const std::string file = wenv.reportFile, node = wenv.reportPath;
    const Prop* envProp = nullptr;
    for (const Prop& p : wenv.props) if (p.key == "environment") envProp = &p;
    if (!envProp || !envProp->doc) return env;
    const auto e = resolveRes(c, envProp->value, *envProp->doc);
    if (!e) return env;
    env.found = true;
    env.source = file + ":" + node;
    auto note = [&](const char* prop, Rating r, const std::string& why) { c.report_.add("property", file, node, "Environment", prop, r, why, 0); };

    namespace ge = fmt::godotenv;
    const int bg = static_cast<int>(numOf(rprop(*e, "background_mode"), 0));
    const double bgEnergy = numOf(rprop(*e, "background_energy_multiplier"), 1.0);
    double skyLightMul = 1.0;   // sky energies that cannot scale a colour (physical sky) scale the sky light instead
    static const double clearGrey[3] = {0.3, 0.3, 0.3};
    if (bg == 0 || bg == 1) {
        // A flat colour: both ends of the authored dome.
        double col[3];
        colorOf(bg == 1 ? rprop(*e, "background_color") : nullptr, col, clearGrey);
        env.hasSky = true;
        env.skyPhysical = false;
        for (int i = 0; i < 3; ++i) env.zenith[i] = env.horizon[i] = env.ground[i] = ge::scaleAuthoredColour(col[i], bgEnergy);
        note("background_mode", Rating::Approximated, "a flat background colour is written as an authored sky dome of one colour");
    } else if (bg == 2) {
        env.hasSky = true;
        env.skyPhysical = true;
        skyLightMul = bgEnergy;
        if (const Var* skyRef = rprop(*e, "sky")) {
            if (const auto sky = resolveRes(c, *skyRef, *e->doc)) {
                if (const Var* matRef = rprop(*sky, "sky_material")) {
                    if (const auto mat = resolveRes(c, *matRef, *sky->doc)) {
                        if (mat->type == "ProceduralSkyMaterial") {
                            static const double top[3] = {0.385, 0.454, 0.55}, hor[3] = {0.646, 0.656, 0.67}, gnd[3] = {0.2, 0.169, 0.133};
                            colorOf(rprop(*mat, "sky_top_color"), env.zenith, top);
                            colorOf(rprop(*mat, "sky_horizon_color"), env.horizon, hor);
                            colorOf(rprop(*mat, "ground_horizon_color"), env.ground, gnd);
                            const double skyE = numOf(rprop(*mat, "sky_energy_multiplier"), 1.0) * bgEnergy;
                            const double gndE = numOf(rprop(*mat, "ground_energy_multiplier"), 1.0) * bgEnergy;
                            for (int i = 0; i < 3; ++i) {
                                env.zenith[i] = ge::scaleAuthoredColour(env.zenith[i], skyE);
                                env.horizon[i] = ge::scaleAuthoredColour(env.horizon[i], skyE);
                                env.ground[i] = std::min(ge::scaleAuthoredColour(env.ground[i], gndE), 1.0);
                            }
                            env.skyPhysical = false;
                            skyLightMul = 1.0;
                            note("sky_material", Rating::Approximated, "ProceduralSkyMaterial becomes the authored two-colour dome (the colours scaled by the energy multipliers; the sky curve, sun disc and cover textures are not modelled)");
                        } else if (mat->type == "PhysicalSkyMaterial") {
                            const double mie = numOf(rprop(*mat, "mie_coefficient"), 0.005), turb = numOf(rprop(*mat, "turbidity"), 10.0);
                            env.hasSkyMieScale = true;
                            env.skyMieScale = std::max(mie / 0.005, 0.0) * std::max(turb / 10.0, 0.0);
                            colorOf(rprop(*mat, "ground_color"), env.ground, env.ground);
                            skyLightMul = numOf(rprop(*mat, "energy_multiplier"), 1.0) * bgEnergy;
                            note("sky_material", Rating::Approximated, "PhysicalSkyMaterial becomes Aver's physical sky: the haze (mie_coefficient x turbidity) scales its Mie scattering, ground_color is the ground albedo, energy_multiplier scales the sky light; the Rayleigh terms, sun disc scale and night sky are not mapped");
                        } else if (mat->type == "PanoramaSkyMaterial") {
                            bool fitted = false;
                            if (const Var* pano = rprop(*mat, "panorama"); pano && pano->type == VType::ExtResource) {
                                if (const gd::GodotExtResource* ext = mat->doc->findExt(pano->s)) {
                                    const std::string res = c.fs_.resolve(*ext, &c.diag_, mat->doc->file);
                                    const std::string disk = res.empty() ? std::string() : c.fs_.toDiskPath(res);
                                    fmt::PanoramaImage img;
                                    std::string why;
                                    fmt::SkyDomeFit fit;
                                    if (!disk.empty() && fmt::loadPanorama(disk, img, &why) && fmt::fitSkyDome(img, numOf(rprop(*mat, "energy_multiplier"), 1.0) * bgEnergy, fit)) {
                                        for (int i = 0; i < 3; ++i) {
                                            env.zenith[i] = fit.zenith[i]; env.horizon[i] = fit.horizon[i]; env.ground[i] = fit.groundAlbedo[i];
                                        }
                                        env.hasDomeExponent = true;
                                        env.domeExponent = fit.domeExponent;
                                        env.skyPhysical = false;
                                        skyLightMul = 1.0;
                                        fitted = true;
                                        note("sky_material", Rating::Approximated, "PanoramaSkyMaterial: the panorama is fitted onto the authored dome (zenith and horizon colour, blend exponent, ground albedo by elevation) and its energy multiplier scales the colours; the renderers do not sample the HDRI, so its detail, sun and rotation are not shown");
                                    } else {
                                        note("sky_material", Rating::Approximated, "the panorama could not be read (" + (why.empty() ? std::string("file missing") : why) + "); the physical sky is used");
                                    }
                                }
                            }
                            if (!fitted && !rprop(*mat, "panorama")) note("sky_material", Rating::Approximated, "a PanoramaSkyMaterial with no panorama; the physical sky is used");
                        } else if (mat->type == "ShaderMaterial") {
                            note("sky_material", Rating::Approximated, "a ShaderMaterial over a sky shader is converted to Shaders/<name>.sky.ocgraph and made the sky at run time by Aver.Fabric (docs/plans/fabric/G6_SHADERS.md SH3); "
                                                                       "the level's own sky record stays the physical sky, which still feeds anything the shader does not replace");
                        } else {
                            note("sky_material", Rating::Unsupported, mat->type + " is not converted; the physical sky is used");
                        }
                    }
                }
            }
        }
        if (rprop(*e, "sky_rotation") || rprop(*e, "sky_custom_fov"))
            note("sky_rotation", Rating::Approximated, "the sky dome depends on elevation only, so its rotation and custom FOV change nothing");
    } else {
        note("background_mode", Rating::Unsupported, "this background mode (canvas, keep, camera feed) has no Aver equivalent");
    }

    const int ambient = static_cast<int>(numOf(rprop(*e, "ambient_light_source"), 0));
    const double energy = numOf(rprop(*e, "ambient_light_energy"), 1.0);
    env.skyLight = (ambient == 1 ? 0.0 : energy) * skyLightMul;
    if (ambient == 2) note("ambient_light_color", Rating::Approximated, "a flat ambient colour is not representable; the sky light is scaled by its energy instead");
    else if (ambient != 0 || energy != 1.0) note("ambient_light_energy", Rating::Approximated, "becomes the sky light multiplier");

    // ---- fog: exponential density, height fog, depth fog (4.3 fog_mode 1) and volumetric fog ----
    const bool fogOn = boolOf(rprop(*e, "fog_enabled"), false);
    const bool volFog = boolOf(rprop(*e, "volumetric_fog_enabled"), false);
    if (fogOn || volFog) {
        env.hasFog = true;
        static const double fogDefault[3] = {0.518, 0.553, 0.608};
        if (fogOn) {
            colorOf(rprop(*e, "fog_light_color"), env.fogColor, fogDefault);
            // Godot's density is per metre; the level's is per centimetre.
            env.fogDensityPerCm = ge::fogDensityPerCm(numOf(rprop(*e, "fog_density"), 0.01));
            if (static_cast<int>(numOf(rprop(*e, "fog_mode"), 0)) == 1) {
                // Depth fog ramps from fog_depth_begin to fog_depth_end metres: a density that reaches ~95 percent at the end.
                const double begin = numOf(rprop(*e, "fog_depth_begin"), 10.0), end = std::max(numOf(rprop(*e, "fog_depth_end"), 100.0), begin + 0.1);
                env.fogStartCm = begin * 100.0;
                env.fogDensityPerCm = 3.0 / ((end - begin) * 100.0);
                note("fog_mode", Rating::Approximated, "depth fog becomes exponential fog that starts at fog_depth_begin and reaches about 95 percent at fog_depth_end; fog_depth_curve is not modelled");
            } else {
                note("fog_density", Rating::Approximated, "per-metre density divided by 100; fog_sky_affect and fog_aerial_perspective are not modelled");
            }
            if (const Var* hd = rprop(*e, "fog_height_density"); hd && numOf(hd, 0.0) != 0.0) {
                const ge::HeightFog hf = ge::heightFog(numOf(rprop(*e, "fog_height"), 0.0), numOf(hd, 0.0));
                // The height is Godot's Y (up) in metres; Aver's Z is up and a centimetre.
                env.fogFalloff = hf.falloffPerCm;
                env.fogHeightCm = hf.heightCm;
                note("fog_height", Rating::Approximated, "height fog becomes Aver's height falloff from fog_height (the density thins going up at |fog_height_density| per metre); Godot adds it to the base fog");
            }
            if (numOf(rprop(*e, "fog_light_energy"), 1.0) != 1.0)
                note("fog_light_energy", Rating::Approximated, "fog light energy is not modelled; the fog tints the in-scattered sky");
        } else {
            static const double volDefault[3] = {1.0, 1.0, 1.0};
            colorOf(rprop(*e, "volumetric_fog_albedo"), env.fogColor, volDefault);
            env.fogDensityPerCm = ge::fogDensityPerCm(numOf(rprop(*e, "volumetric_fog_density"), 0.05));
        }
        if (volFog) {
            env.volumetricFog = true;
            note("volumetric_fog_enabled", Rating::Approximated, "volumetric fog becomes the level's distance fog at volumetric_fog_density (per metre / 100) and switches Voxi's light-shaft fog on (RENDER.VOLFOG 1: the sun scattered through that fog, shadowed by the scene) where FogVolume nodes draw; Godot's froxel resolution, volumetric_fog_gi_inject, detail_spread, ambient_inject, sky_affect and temporal reprojection settings are not modelled");
        }
    }

    // ---- tonemap, exposure, adjustments, glow ----
    env.tonemapMode = static_cast<int>(numOf(rprop(*e, "tonemap_mode"), 0));
    env.tonemapExposure = numOf(rprop(*e, "tonemap_exposure"), 1.0);
    env.tonemapWhite = numOf(rprop(*e, "tonemap_white"), 1.0);
    if (env.tonemapMode == 3)
        note("tonemap_mode", Rating::Converted, "Godot's ACES (mode 6 of Aver's tonemappers, with tonemap_white)");
    else
        note("tonemap_mode", Rating::Converted, "Godot's " + std::string(env.tonemapMode == 0 ? "linear" : env.tonemapMode == 1 ? "Reinhard" : env.tonemapMode == 2 ? "filmic" : "AgX") +
                                                " tonemapper (Aver tonemap " + std::to_string(ge::tonemapMode(env.tonemapMode)) + "), with tonemap_exposure and tonemap_white");
    env.adjust = boolOf(rprop(*e, "adjustment_enabled"), false);
    if (env.adjust) {
        env.adjBrightness = numOf(rprop(*e, "adjustment_brightness"), 1.0);
        env.adjContrast = numOf(rprop(*e, "adjustment_contrast"), 1.0);
        env.adjSaturation = numOf(rprop(*e, "adjustment_saturation"), 1.0);
        note("adjustment_enabled", Rating::Converted, "brightness, contrast and saturation run in display space after the tonemap (RENDER.ADJUST*)");
        if (rprop(*e, "adjustment_color_correction"))
            note("adjustment_color_correction", Rating::Unsupported, "a colour-correction texture is not converted; grade with a post volume's .cube LUT");
    }
    env.glow = boolOf(rprop(*e, "glow_enabled"), false);
    env.glowIntensity = numOf(rprop(*e, "glow_intensity"), 0.8);
    if (env.glow) {
        note("glow_enabled", Rating::Approximated, "glow becomes the bloom intensity (glow_intensity) and, when stated, the bloom threshold (glow_hdr_threshold); levels, strength, blend mode and bloom are not modelled");
        if (rprop(*e, "glow_hdr_threshold")) env.glowThreshold = numOf(rprop(*e, "glow_hdr_threshold"), 1.0);
    }

    // ---- camera attributes: exposure multiplier, auto exposure, depth of field ----
    for (const Prop& p : wenv.props) {
        if (p.key != "camera_attributes" || !p.doc) continue;
        const auto ca = resolveRes(c, p.value, *p.doc);
        if (!ca) continue;
        const bool practical = ca->type == "CameraAttributesPractical", physical = ca->type == "CameraAttributesPhysical";
        if (!practical && !physical) { note("camera_attributes", Rating::Unsupported, ca->type + " is not converted"); continue; }
        if (practical) {
            env.cameraExposure = numOf(rprop(*ca, "exposure_multiplier"), 1.0);
            env.autoExposure = boolOf(rprop(*ca, "auto_exposure_enabled"), false);
            const bool farOn = boolOf(rprop(*ca, "dof_blur_far_enabled"), false), nearOn = boolOf(rprop(*ca, "dof_blur_near_enabled"), false);
            const ge::Dof d = ge::dofFromPractical(nearOn, farOn, numOf(rprop(*ca, "dof_blur_near_distance"), 2.0), numOf(rprop(*ca, "dof_blur_far_distance"), 10.0),
                                                   numOf(rprop(*ca, "dof_blur_amount"), 0.1));
            env.dof = d.on; env.dofFocusCm = d.focusCm; env.dofFStop = d.fStop; env.dofFocalMm = d.focalMm;
            c.report_.add("property", file, node, ca->type, "exposure_multiplier", Rating::Converted, "multiplies the tonemap exposure (RENDER.EXPOSURE)", 0);
            c.report_.add("property", file, node, ca->type, "auto_exposure_enabled", env.autoExposure ? Rating::Approximated : Rating::Converted,
                          env.autoExposure ? "Aver's eye adaptation (RENDER.AUTOEXPOSURE 1) with its own speed and key; auto_exposure_scale and speed are not mapped"
                                           : "Godot's exposure is fixed, so the project turns Aver's eye adaptation off (RENDER.AUTOEXPOSURE 0)", 0);
            if (d.on) c.report_.add("property", file, node, ca->type, "dof_blur_far_enabled", Rating::Approximated,
                                    "Aver's lens is physical: a lens focused at the far blur distance with an aperture from dof_blur_amount; Godot's distance bands and transitions are not modelled", 0);
        } else {
            const ge::Dof d = ge::dofFromPhysical(numOf(rprop(*ca, "frustum_focus_distance"), 10.0), numOf(rprop(*ca, "frustum_focal_length"), 35.0),
                                                  numOf(rprop(*ca, "exposure_aperture"), 16.0));
            env.dof = d.on; env.dofFocusCm = d.focusCm; env.dofFStop = d.fStop; env.dofFocalMm = d.focalMm;
            c.report_.add("property", file, node, ca->type, "frustum_focus_distance", Rating::Converted, "focus distance, focal length and aperture drive Aver's physical depth of field", 0);
            c.report_.add("property", file, node, ca->type, "exposure_aperture", Rating::Approximated,
                          "the physical exposure (aperture, shutter, ISO) is not converted to an exposure multiplier; the project keeps eye adaptation off and exposure at tonemap_exposure", 0);
        }
    }

    for (const gd::GodotProperty& p : *e->props) {
        static const std::set<std::string> known = {"background_mode", "background_color", "background_energy_multiplier", "sky", "sky_rotation",
                                                    "sky_custom_fov", "ambient_light_source", "ambient_light_color", "ambient_light_energy",
                                                    "fog_enabled", "fog_light_color", "fog_density", "fog_mode", "fog_depth_begin", "fog_depth_end",
                                                    "fog_depth_curve", "fog_height", "fog_height_density", "fog_light_energy", "fog_sky_affect",
                                                    "fog_aerial_perspective", "volumetric_fog_enabled", "volumetric_fog_density", "volumetric_fog_albedo",
                                                    "tonemap_mode", "tonemap_exposure", "tonemap_white", "glow_enabled", "glow_intensity",
                                                    "glow_hdr_threshold", "adjustment_enabled", "adjustment_brightness", "adjustment_contrast",
                                                    "adjustment_saturation", "adjustment_color_correction", "resource_local_to_scene",
                                                    "resource_name", "script", "resource_path"};
        if (known.count(p.name)) continue;
        // G4 R8: Godot's screen-space and SDF effects have no pass of their own in Aver; its GI is the same job done another way, so they are rated for what
        // covers them (RENDER.GI and ray tracing are the project's choice, not forced here).
        const char* gi = nullptr;
        if (p.name.rfind("ssao_", 0) == 0)
            gi = "no screen-space pass: ambient occlusion comes from Aver's global illumination (ray-traced sky occlusion with ray tracing; the voxel cone trace's occlusion with RENDER.GI on). Radius, intensity, power and detail are not mapped, and with GI off and no ray tracing there is no occlusion";
        else if (p.name.rfind("ssr_", 0) == 0)
            gi = "no screen-space reflections: reflections come from ray tracing when it is on, else from the voxel cone trace (RENDER.GI on) and the sky. Steps, fade and depth tolerance are not mapped, and off-volume reflections show the sky";
        else if (p.name.rfind("ssil_", 0) == 0)
            gi = "no screen-space indirect light: Aver's global illumination (ray-traced or voxel cone) lights from the scene, not from the screen";
        else if (p.name.rfind("sdfgi_", 0) == 0)
            gi = "no signed-distance-field GI: Aver's voxel / ray-traced global illumination (RENDER.GI, RENDER.VOXELRES, RENDER.GIDISTANCE) does the job around the camera; cascades, bounce feedback and energy are not mapped";
        c.report_.add("property", file, node, "Environment", p.name, gi ? Rating::Approximated : Rating::Unsupported,
                      gi ? gi : "no level environment record for this setting", p.line);
    }
    return env;
}

std::vector<PostKey> postKeysOf(const EnvInfo& env) {
    namespace ge = fmt::godotenv;
    std::vector<PostKey> k;
    k.push_back({"tonemap", static_cast<double>(ge::tonemapMode(env.tonemapMode))});
    k.push_back({"tonemapwhite", env.tonemapWhite});
    k.push_back({"exposure", ge::exposure(env.tonemapExposure, env.cameraExposure)});
    k.push_back({"autoexposure", env.autoExposure ? 1.0 : 0.0});
    k.push_back({"bloom", env.glow ? env.glowIntensity : 0.0});
    if (env.glow && env.glowThreshold >= 0.0) k.push_back({"bloomthreshold", env.glowThreshold});
    k.push_back({"adjbrightness", env.adjust ? env.adjBrightness : 1.0});
    k.push_back({"adjcontrast", env.adjust ? env.adjContrast : 1.0});
    k.push_back({"adjsaturation", env.adjust ? env.adjSaturation : 1.0});
    k.push_back({"dof", env.dof ? 1.0 : 0.0});
    if (env.dof) {
        k.push_back({"doffocus", env.dofFocusCm});
        k.push_back({"dofstop", env.dofFStop});
        k.push_back({"dofmm", env.dofFocalMm});
    }
    return k;
}

bool Converter::emitLevel(const std::string& sceneRes, FNode& root, fmt::OcWorldData& level, bool isStart) {
    level.name = stemOf(sceneRes);
    LevelScan scan;
    scan.sceneRoot = &root;

    std::vector<std::unique_ptr<FNode>> autoloads;
    if (isStart) {
        for (const gd::GodotAutoload& a : project_.autoloads) {
            const std::string res = gd::GodotProjectFs::isUid(a.path) ? fs_.resolve(a.path, "") : gd::GodotProjectFs::normalizeRes(a.path);
            const Where w{"res://project.godot", a.name, ""};
            if (!a.singleton) {
                report_.add("setting", "res://project.godot", a.name, "Autoload", "", Rating::Approximated,
                            "an autoload without the singleton flag is not added to the tree in Godot either; nothing is placed", a.line);
                continue;
            }
            std::string ext;
            if (const size_t d = res.find_last_of('.'); d != std::string::npos) ext = lower(res.substr(d));
            std::unique_ptr<FNode> node;
            if (ext == ".tscn" || ext == ".scn") {
                node = buildScene(res, 0, w);
                if (node) { node->instanceOf = res; instancedScenes_.insert(res); }
            } else if (ext == ".gd" || ext == ".cs") {
                node = std::make_unique<FNode>();
                if (ScriptInfo* si = script(res)) {
                    si->autoload = true;
                    node->type = si->extends.empty() ? "Node" : si->extends;
                }
                node->scriptPath = res;
                node->sceneFile = "res://project.godot";
                node->reportFile = "res://project.godot";
                node->reportPath = a.name;
                node->line = a.line;
            }
            if (!node) {
                report_.add("setting", "res://project.godot", a.name, "Autoload", "", Rating::Unsupported, "the autoload " + a.path + " could not be converted", a.line);
                continue;
            }
            node->name = a.name;
            node->persistent = true;
            node->owner = nullptr;
            if (!node->scriptPath.empty() || ext == ".tscn" || ext == ".scn")
                report_.add("setting", "res://project.godot", a.name, "Autoload", "", Rating::Converted, "a persistent top-level node, first in the start level", a.line);
            autoloads.push_back(std::move(node));
        }
    }

    Xform identity;
    viewportUsers_.clear();
    for (auto& a : autoloads) emitNode(*a, -1, identity, level, scan);
    emitNode(root, -1, identity, level, scan);

    // ---- sub-viewports (G4 R5): how many materials show each view, now that every material has been converted ----
    // A SubViewport is usually placed before the mesh whose material names it, so the count is known only here. The node adds that many
    // consumers; a 'when visible' view is drawn only while it has one (or a container shows it, or a script's UI).
    for (const auto& [key, idx] : scan.viewports) {
        const auto used = viewportUsers_.find(key);
        if (used == viewportUsers_.end() || used->second <= 0) continue;
        for (fmt::OcPlacedComponent& comp : level.placements[static_cast<size_t>(idx)].components)
            if (comp.type == "Props") { comp.kv.emplace_back("_material_consumers", std::to_string(used->second)); break; }
    }

    // ---- level records from what the scan found ----
    if (scan.environmentNode) {
        const EnvInfo env = readEnvironment(*this, *scan.environmentNode);
        if (env.found) {
            if (env.hasSky) {
                level.hasSky = true;
                level.skyPhysical = env.skyPhysical;
                for (int i = 0; i < 3; ++i) {
                    level.skyZenith[i] = env.zenith[i];
                    level.skyHorizon[i] = env.horizon[i];
                    level.skyGroundAlbedo[i] = env.ground[i];
                }
                level.skyLight = env.skyLight;
                if (env.hasDomeExponent) level.skyDomeExponent = env.domeExponent;
                if (env.hasSkyMieScale && env.skyPhysical) {
                    // Aver's calibrated clear-sky Mie values (rhi::AtmosphereProfile) scaled by the haze.
                    level.skyMieScatter = 2.8e-3 * env.skyMieScale;
                    level.skyMieExtinction = 3.1e-3 * env.skyMieScale;
                }
            } else if (env.skyLight != 1.0) {
                level.hasSky = true;
                level.skyLight = env.skyLight;
            }
            if (env.hasFog) {
                level.hasFog = true;
                level.fogDensity = env.fogDensityPerCm;
                for (int i = 0; i < 3; ++i) level.fogColor[i] = env.fogColor[i];
                level.fogFalloff = env.fogFalloff;
                level.fogHeight = env.fogHeightCm;
                level.fogStart = env.fogStartCm;
            }
            // The post settings of this WorldEnvironment as one level-wide post volume, so a level that differs from the project's
            // start level still switches its tonemapper, exposure and adjustments when it loads.
            {
                fmt::OcPostVolume pv;
                pv.name = "WorldEnvironment";
                pv.infinite = true;
                for (const PostKey& k : postKeysOf(env)) pv.set(k.key, &k.v, 1);
                level.postVolumes.push_back(std::move(pv));
            }
            if (!projectEnv_.found || isStart) projectEnv_ = env;
        }
    }
    if (scan.haveSun) {
        const Vec3 d = fmt::axis::direction(static_cast<f32>(scan.sunToward.x), static_cast<f32>(scan.sunToward.y), static_cast<f32>(scan.sunToward.z));
        level.hasSun = true;
        level.sunDir[0] = d.x; level.sunDir[1] = d.y; level.sunDir[2] = d.z;
        for (int i = 0; i < 3; ++i) level.sunColor[i] = scan.sunColor[i];
        level.sunLux = scan.sunEnergy * kLuxPerEnergy;
        level.sunAngularDeg = scan.sunAngular;
    }
    // Godot makes the first camera current when none is marked (G4 R4): the node class would otherwise leave the player's own view in place.
    if (scan.haveCamera && !scan.cameraCurrent && scan.firstCameraPlacement >= 0 &&
        scan.firstCameraPlacement < static_cast<int>(level.placements.size())) {
        for (fmt::OcPlacedComponent& comp : level.placements[static_cast<size_t>(scan.firstCameraPlacement)].components) {
            if (comp.type != "Props") continue;
            comp.kv.emplace_back("current", "true");
            report_.add("property", sceneRes, scan.firstCameraNode, "Camera3D", "current", Rating::Converted,
                        "no camera in this scene is marked current: the first one is made current, as Godot does", 0);
            break;
        }
    }
    if (scan.haveCamera) {
        const Vec3 pos = fmt::axis::position(static_cast<f32>(scan.cameraWorld.pos.x), static_cast<f32>(scan.cameraWorld.pos.y),
                                             static_cast<f32>(scan.cameraWorld.pos.z));
        const V3 fwd = scan.cameraWorld.basis.column(2) * -1.0;
        const Vec3 f = fmt::axis::direction(static_cast<f32>(fwd.x), static_cast<f32>(fwd.y), static_cast<f32>(fwd.z));
        level.hasCamera = true;
        const auto clean = [](double v) { return v == 0.0 ? 0.0 : v; };
        level.camX = clean(pos.x); level.camY = clean(pos.y); level.camZ = clean(pos.z);
        level.camYaw = clean(std::atan2(f.y, f.x) * 180.0 / kPi);
        level.camPitch = clean(std::asin(std::max(-1.0f, std::min(1.0f, f.z / std::max(1e-6f, std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z))))) * 180.0 / kPi);
        level.camSpeed = 0.0;
    }
    return true;
}

// ---- prefabs: the instanced scenes as .ocprefab assets --------------------------------------------------------------------

bool Converter::writePrefabs() {
    bool ok = true;
    for (const std::string& res : instancedScenes_) {
        std::string ext;
        if (const size_t d = res.find_last_of('.'); d != std::string::npos) ext = lower(res.substr(d));
        if (ext != ".tscn" && ext != ".scn") continue;
        const Where w{res, "", ""};
        std::unique_ptr<FNode> root = buildScene(res, 0, w);
        if (!root) continue;
        fmt::OcWorldData scratch;
        LevelScan scan;
        scan.prefabPass = true;
        Xform identity;
        emitNode(*root, -1, identity, scratch, scan);

        fmt::OcPrefabData prefab;
        prefab.name = stemOf(res);
        for (size_t i = 0; i < scratch.placements.size(); ++i) {
            const fmt::OcWorldPlacement& pl = scratch.placements[i];
            fmt::OcPrefabNode node;
            node.uid = static_cast<u32>(i + 1);
            node.parent = pl.parent < 0 ? 0u : static_cast<u32>(pl.parent + 1);
            node.name = pl.name;
            node.className = pl.className;

            fmt::OcSaveComponent local;
            local.type = "CLocal";
            fmt::OcSaveField pos, rot, scl;
            pos.name = "position"; pos.kind = fmt::kOcPrefabKindVec3;
            pos.f = {static_cast<f32>(pl.x), static_cast<f32>(pl.y), static_cast<f32>(pl.z)};
            const Quat q = world::quatFromEulerDeg(Vec3{static_cast<f32>(pl.roll), static_cast<f32>(pl.pitch), static_cast<f32>(pl.yaw)});
            rot.name = "rotation"; rot.kind = fmt::kOcPrefabKindQuat; rot.f = {q.x, q.y, q.z, q.w};
            scl.name = "scale"; scl.kind = fmt::kOcPrefabKindVec3;
            scl.f = {static_cast<f32>(pl.sx), static_cast<f32>(pl.sy), static_cast<f32>(pl.sz)};
            local.fields = {pos, rot, scl};
            node.components.push_back(std::move(local));

            // The mesh a MeshInstance3D shows, by the id a placement hashes its asset path to.
            for (const fmt::OcPlacedComponent& c : pl.components) {
                if (c.type != "Props") continue;
                for (const auto& [k, v] : c.kv) {
                    if (k != "mesh") continue;
                    const size_t a = v.find('"'), b = v.rfind('"');
                    if (a == std::string::npos || b <= a) continue;
                    fmt::OcSaveComponent mr;
                    mr.type = "CMeshRenderer";
                    fmt::OcSaveField mesh;
                    mesh.name = "mesh";
                    mesh.kind = fmt::kOcPrefabKindI64;
                    mesh.i = static_cast<i64>(fnv1a64(std::string_view(v).substr(a + 1, b - a - 1)));
                    mr.fields.push_back(std::move(mesh));
                    node.components.push_back(std::move(mr));
                }
            }
            prefab.nodes.push_back(std::move(node));
        }
        prefab.nextUid = static_cast<u32>(prefab.nodes.size() + 1);
        std::string why;
        if (!prefab.valid(&why)) {
            report_.add("scene", res, "", "PackedScene", "", Rating::Unsupported, "the prefab is not valid: " + why, 0);
            ok = false;
            continue;
        }
        std::string rel = res.substr(6);
        rel.erase(rel.find_last_of('.'));
        const std::string asset = "Prefabs/" + rel + ".ocprefab";
        if (!fmt::saveOcPrefab(contentDir_ + "/" + asset, prefab, &why)) {
            report_.add("scene", res, "", "PackedScene", "", Rating::Unsupported, "could not write " + asset + ": " + why, 0);
            ok = false;
            continue;
        }
        prefabs_.push_back(asset);
        report_.add("scene", res, "", "PackedScene", "", Rating::Approximated,
                    "also written as " + asset + " (names, classes, transforms, meshes); the level holds the flattened placements, which carry the properties", 0);
    }
    return ok;
}

// ---- every scene -----------------------------------------------------------------------------------------------------------

bool Converter::convertScenes() {
    bool ok = true;
    std::vector<std::string> scenes;
    for (const std::string& res : fs_.files())
        if ((res.size() > 5 && lower(res.substr(res.size() - 5)) == ".tscn") || (res.size() > 4 && lower(res.substr(res.size() - 4)) == ".scn")) scenes.push_back(res);

    // The start scene first, so its environment is the project's.
    std::string start;
    if (!project_.mainScene.empty())
        start = gd::GodotProjectFs::isUid(project_.mainScene) ? fs_.resolve(project_.mainScene, "") : gd::GodotProjectFs::normalizeRes(project_.mainScene);
    startSceneRes_ = start;
    std::stable_sort(scenes.begin(), scenes.end(), [&](const std::string& a, const std::string& b) { return a == start && b != start; });

    for (const std::string& res : scenes) {
        const Where w{res, "", ""};
        std::unique_ptr<FNode> root = buildScene(res, 0, w);
        if (!root) {
            report_.add("scene", res, "", "PackedScene", "", Rating::Unsupported, "the scene could not be read", 0);
            if (!endsWith(res, ".scn")) ok = false;   // a binary scene that cannot be read is reported, as before; it does not fail the run
            continue;
        }
        fmt::OcWorldData level;
        emitLevel(res, *root, level, res == start);
        const std::string asset = levelAssetFor(res);
        std::string why;
        if (!fmt::saveOcworld(contentDir_ + "/" + asset, level, &why)) {
            report_.add("scene", res, "", "PackedScene", "", Rating::Unsupported, "could not write " + asset + ": " + why, 0);
            ok = false;
            continue;
        }
        // A written level must read back.
        fmt::OcWorldData back;
        if (!fmt::loadOcworld(contentDir_ + "/" + asset, back, &why) || back.placements.size() != level.placements.size()) {
            report_.add("scene", res, "", "PackedScene", "", Rating::Unsupported, "the written level did not read back: " + why, 0);
            ok = false;
            continue;
        }
        sceneLevels_.push_back(asset);
        report_.add("scene", res, "", "PackedScene", "", Rating::Converted,
                    "written as " + asset + " with " + std::to_string(level.placements.size()) + " placement(s)", 0);
        if (res == start) startLevelAsset_ = asset;
    }
    if (!start.empty() && startLevelAsset_.empty())
        report_.add("setting", "res://project.godot", "", "", "application/run/main_scene", Rating::Unsupported, "the main scene " + start + " was not converted", 0);
    writePrefabs();
    return ok;
}

}  // namespace aver::fabric
