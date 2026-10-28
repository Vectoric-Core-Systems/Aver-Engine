// .tscn to .ocworld: the node tree (instances flattened, overrides applied), then one class placement per node
// exactly as docs/plans/fabric/G3_RUNTIME.md section 1 says.
#include "FabricInternal.hpp"
#include "FabricMeshGen.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/OcPrefab.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace aver::fabric {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kLuxPerEnergy = 100000.0f;   // Aver.Fabric Light3D.LuxPerEnergy

const std::set<std::string>& supportedClasses() {
    static const std::set<std::string> s = {"Node", "Node3D", "Marker3D", "VisualInstance3D", "GeometryInstance3D", "MeshInstance3D",
                                            "Camera3D", "DirectionalLight3D", "OmniLight3D", "SpotLight3D", "CollisionShape3D",
                                            "StaticBody3D", "AnimatableBody3D", "RigidBody3D", "CharacterBody3D", "Area3D", "Timer",
                                            // F2b added these to Aver.Fabric (G3_RUNTIME 1.4)
                                            "RayCast3D", "ShapeCast3D", "VehicleBody3D", "VehicleWheel3D"};
    return s;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

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
    static const std::set<std::string> s = {"CanvasLayer", "CanvasModulate", "ParallaxBackground", "ParallaxLayer", "TileMap", "TileMapLayer",
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
    if (isExtensionClass(t, &ext3D)) return ext3D;
    return endsWith(t, "3D") || s.count(t) != 0;
}

ClassChoice chooseClass(const std::string& t) {
    ClassChoice c;
    if (t.empty()) { c.fabricClass = "Node"; return c; }
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
    if (t == "Viewport" || t == "SubViewport" || t == "Window") {
        c.fabricClass = t == "Window" ? "Window" : "Viewport";
        c.rating = Rating::Approximated;
        c.reason = "a viewport is a placeholder in the runtime (render-to-texture is a later rendering package)";
        c.carrier = t != c.fabricClass;
        return c;
    }
    if (isControlClass(t)) {
        c.fabricClass = "Control";
        c.rating = Rating::Approximated;
        c.reason = "UI controls are placeholders holding layout values until the UI package fills them";
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
        c.fabricClass = "Node";
        c.reason = "its Environment becomes the level's SUN / SKY / FOG records; the node is kept as a plain node";
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
                const ModelResult* mr = modelFor(subRes, w);
                if (mr) {
                    int k = 0;
                    for (const ModelPart& part : mr->parts) {
                        auto child = std::make_unique<FNode>();
                        child->name = part.name.empty() ? "Mesh" + std::to_string(k) : part.name;
                        child->type = "MeshInstance3D";
                        child->fromModel = true;
                        child->sceneFile = subRes;
                        child->reportFile = subRes;
                        child->reportPath = child->name;
                        child->parent = made.get();
                        child->owner = made.get();
                        // The part's position is engine centimetres; the node holds Godot metres.
                        child->xf.pos = {part.y / 100.0, part.z / 100.0, -part.x / 100.0};
                        child->hasXform = true;
                        child->props.push_back(Prop{"mesh", makeResource(part.meshAsset), nullptr, 0});
                        made->children.push_back(std::move(child));
                        ++k;
                    }
                }
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
    V3 sunToward{0, 1, 0};
    double sunColor[3] = {1, 1, 1};
    double sunEnergy = 1;
    double sunAngular = 0.5;
    bool haveCamera = false, cameraCurrent = false;
    Xform cameraWorld;
    const FNode* environmentNode = nullptr;
    const FNode* sceneRoot = nullptr;
    bool prefabPass = false;
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

Var transformVariant(const Xform& x) {
    Var v;
    v.type = VType::Transform3D;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) v.nums.push_back(x.basis.m[r][c]);
    v.nums.insert(v.nums.end(), {x.pos.x, x.pos.y, x.pos.z});
    return v;
}

}  // namespace

void Converter::emitNode(FNode& fn, int parent, const Xform& parentWorld, fmt::OcWorldData& level, LevelScan& scan) {
    const Where w{fn.reportFile, fn.reportPath, fn.type};
    ClassChoice cc = chooseClass(fn.type);
    std::string cls = cc.fabricClass;

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

    for (const Prop& pr : fn.props) {
        if (fn.type == "CollisionPolygon3D" && havePolygonShape && (pr.key == "polygon" || pr.key == "depth" || pr.key == "margin")) continue;
        if (fn.type == "WorldEnvironment" && pr.key == "environment") continue;   // becomes level records
        if (fn.type == "WorldEnvironment" && pr.key == "camera_attributes") continue;
        const Where pw{fn.reportFile, fn.reportPath, fn.type};
        Var cv = pr.doc ? convertValue(pr.value, *pr.doc, pw, pr.key) : pr.value;
        if (cv.isNil() && !pr.value.isNil()) continue;   // a resource that could not be converted; already reported
        if (pr.key.find('/') != std::string::npos && pr.key.rfind("surface_material_override/", 0) != 0) {
            report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, Rating::Approximated,
                        "a path-style property is not a member of the runtime class; it is kept as node metadata", pr.line);
        }
        props.emplace_back(pr.key, std::move(cv));
    }
    if (havePolygonShape) props.emplace_back("shape", polygonShape);

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
        if (fn.type == "Camera3D") {
            bool current = false;
            for (const Prop& pr : fn.props) if (pr.key == "current") current = boolOf(&pr.value, false);
            if (!scan.haveCamera || (current && !scan.cameraCurrent)) {
                scan.haveCamera = true;
                scan.cameraCurrent = current;
                scan.cameraWorld = worldXf;
            }
        }
        if (fn.type == "WorldEnvironment" && !scan.environmentNode) scan.environmentNode = &fn;
    }

    const int index = static_cast<int>(level.placements.size());
    level.placements.push_back(std::move(p));
    for (const auto& c : fn.children) emitNode(*c, index, worldXf, level, scan);
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

    const int bg = static_cast<int>(numOf(rprop(*e, "background_mode"), 0));
    static const double clearGrey[3] = {0.3, 0.3, 0.3};
    if (bg == 0 || bg == 1) {
        // A flat colour: both ends of the authored dome.
        double col[3];
        colorOf(bg == 1 ? rprop(*e, "background_color") : nullptr, col, clearGrey);
        env.hasSky = true;
        env.skyPhysical = false;
        for (int i = 0; i < 3; ++i) env.zenith[i] = env.horizon[i] = env.ground[i] = col[i];
        note("background_mode", Rating::Approximated, "a flat background colour is written as an authored sky dome of one colour");
    } else if (bg == 2) {
        env.hasSky = true;
        env.skyPhysical = true;
        if (const Var* skyRef = rprop(*e, "sky")) {
            if (const auto sky = resolveRes(c, *skyRef, *e->doc)) {
                if (const Var* matRef = rprop(*sky, "sky_material")) {
                    if (const auto mat = resolveRes(c, *matRef, *sky->doc)) {
                        if (mat->type == "ProceduralSkyMaterial") {
                            static const double top[3] = {0.385, 0.454, 0.55}, hor[3] = {0.646, 0.656, 0.67}, gnd[3] = {0.2, 0.169, 0.133};
                            colorOf(rprop(*mat, "sky_top_color"), env.zenith, top);
                            colorOf(rprop(*mat, "sky_horizon_color"), env.horizon, hor);
                            colorOf(rprop(*mat, "ground_horizon_color"), env.ground, gnd);
                            env.skyPhysical = false;
                            note("sky_material", Rating::Approximated, "ProceduralSkyMaterial becomes the authored two-colour dome");
                        } else if (mat->type == "PhysicalSkyMaterial") {
                            note("sky_material", Rating::Approximated, "PhysicalSkyMaterial becomes Aver's physical sky with its own defaults");
                        } else if (mat->type == "PanoramaSkyMaterial") {
                            note("sky_material", Rating::Unsupported, "an HDRI panorama sky needs the rendering package (G4); the physical sky is used");
                        } else {
                            note("sky_material", Rating::Unsupported, mat->type + " is not converted; the physical sky is used");
                        }
                    }
                }
            }
        }
    } else {
        note("background_mode", Rating::Unsupported, "this background mode (canvas, keep, camera feed) has no Aver equivalent");
    }

    const int ambient = static_cast<int>(numOf(rprop(*e, "ambient_light_source"), 0));
    const double energy = numOf(rprop(*e, "ambient_light_energy"), 1.0);
    env.skyLight = ambient == 1 ? 0.0 : energy;
    if (ambient == 2) note("ambient_light_color", Rating::Approximated, "a flat ambient colour is not representable; the sky light is scaled by its energy instead");
    else if (ambient != 0 || energy != 1.0) note("ambient_light_energy", Rating::Approximated, "becomes the sky light multiplier");

    if (boolOf(rprop(*e, "fog_enabled"), false)) {
        env.hasFog = true;
        static const double fogDefault[3] = {0.518, 0.553, 0.608};
        colorOf(rprop(*e, "fog_light_color"), env.fogColor, fogDefault);
        // Godot's density is per metre; the level's is per centimetre.
        env.fogDensityPerCm = numOf(rprop(*e, "fog_density"), 0.01) / 100.0;
        note("fog_density", Rating::Approximated, "per-metre density divided by 100; Godot's fog falloff and sky affect are not modelled");
    }
    env.tonemapMode = static_cast<int>(numOf(rprop(*e, "tonemap_mode"), 0));
    env.tonemapExposure = numOf(rprop(*e, "tonemap_exposure"), 1.0);
    if (env.tonemapMode != 0)
        note("tonemap_mode", Rating::Approximated, "mode " + std::to_string(env.tonemapMode) + " is recorded in Fabric/project.json; Aver's tonemapper is chosen by the project's own setting (G4)");
    env.glow = boolOf(rprop(*e, "glow_enabled"), false);
    env.glowIntensity = numOf(rprop(*e, "glow_intensity"), 0.8);
    if (env.glow) note("glow_enabled", Rating::Approximated, "glow becomes the project's bloom intensity");
    for (const gd::GodotProperty& p : *e->props) {
        static const std::set<std::string> known = {"background_mode", "background_color", "sky", "ambient_light_source", "ambient_light_color",
                                                    "ambient_light_energy", "fog_enabled", "fog_light_color", "fog_density", "tonemap_mode",
                                                    "tonemap_exposure", "glow_enabled", "glow_intensity", "resource_local_to_scene",
                                                    "resource_name", "script", "resource_path"};
        if (!known.count(p.name))
            c.report_.add("property", file, node, "Environment", p.name, Rating::Unsupported, "no level environment record for this setting", p.line);
    }
    return env;
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
            if (ext == ".tscn") {
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
            if (!node->scriptPath.empty() || ext == ".tscn")
                report_.add("setting", "res://project.godot", a.name, "Autoload", "", Rating::Converted, "a persistent top-level node, first in the start level", a.line);
            autoloads.push_back(std::move(node));
        }
    }

    Xform identity;
    for (auto& a : autoloads) emitNode(*a, -1, identity, level, scan);
    emitNode(root, -1, identity, level, scan);

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
            } else if (env.skyLight != 1.0) {
                level.hasSky = true;
                level.skyLight = env.skyLight;
            }
            if (env.hasFog) {
                level.hasFog = true;
                level.fogDensity = env.fogDensityPerCm;
                for (int i = 0; i < 3; ++i) level.fogColor[i] = env.fogColor[i];
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
        if (ext != ".tscn") continue;
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
        if (res.size() > 5 && lower(res.substr(res.size() - 5)) == ".tscn") scenes.push_back(res);

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
            ok = false;
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
