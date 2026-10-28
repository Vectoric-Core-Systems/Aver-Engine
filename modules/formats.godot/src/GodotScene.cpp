// .tscn / .tres interpretation over the generic section parser (GodotSections.cpp).
#include "aver/formats/godot/GodotScene.hpp"

#include <cstdlib>
#include <unordered_map>

namespace aver::fmt::godot {

const GodotVariant* GodotSubResource::prop(std::string_view name) const {
    for (const GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

const GodotVariant* GodotNode::prop(std::string_view key) const {
    for (const GodotProperty& p : props)
        if (p.name == key) return &p.value;
    return nullptr;
}

const GodotVariant* GodotResource::prop(std::string_view name) const {
    for (const GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

const GodotExtResource* GodotDocBase::findExt(std::string_view id) const {
    for (const GodotExtResource& e : extResources)
        if (e.id == id) return &e;
    return nullptr;
}

const GodotSubResource* GodotDocBase::findSub(std::string_view id) const {
    for (const GodotSubResource& s : subResources)
        if (s.id == id) return &s;
    return nullptr;
}

int GodotScene::findNode(std::string_view path) const {
    for (usize i = 0; i < nodes.size(); ++i)
        if (nodes[i].path == path) return static_cast<int>(i);
    return -1;
}

std::vector<int> GodotScene::childrenOf(int index) const {
    std::vector<int> out;
    for (usize i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent == index && static_cast<int>(i) != index) out.push_back(static_cast<int>(i));
    return out;
}

namespace {

// An id attribute is a string in format 3 and an integer in format 2.
std::string idText(const GodotVariant* v) {
    if (!v) return {};
    if (v->type == GodotVType::String) return v->s;
    if (v->type == GodotVType::Int) return std::to_string(v->i);
    return {};
}

std::vector<std::string> stringList(const GodotVariant* v) {
    std::vector<std::string> out;
    if (!v) return out;
    if (v->type == GodotVType::Array) {
        for (const GodotVariant& e : v->items)
            if (e.isStringLike()) out.push_back(e.s);
    } else if (v->type == GodotVType::PackedStringArray) {
        out = v->strs;
    }
    return out;
}

void checkRefs(const GodotDocBase& doc, const GodotVariant& v, GodotDiagnostics& diag) {
    switch (v.type) {
        case GodotVType::ExtResource:
            if (!doc.findExt(v.s)) diag.error(doc.file, v.line, "ExtResource(\"" + v.s + "\") is not declared by an [ext_resource]", "godot.ref");
            return;
        case GodotVType::SubResource:
            if (!doc.findSub(v.s)) diag.error(doc.file, v.line, "SubResource(\"" + v.s + "\") is not declared by a [sub_resource]", "godot.ref");
            return;
        default: break;
    }
    for (const GodotVariant& k : v.keys) checkRefs(doc, k, diag);
    for (const GodotVariant& e : v.items) checkRefs(doc, e, diag);
}

void checkProps(const GodotDocBase& doc, const std::vector<GodotProperty>& props, GodotDiagnostics& diag) {
    for (const GodotProperty& p : props) checkRefs(doc, p.value, diag);
}

bool parseDoc(std::string_view text, std::string_view file, bool wantScene, GodotDocBase& base, GodotScene* scene,
              GodotResource* res, GodotDiagnostics& diag) {
    const usize errorsBefore = diag.errorCount();
    base = GodotDocBase{};
    base.file = std::string(file);

    GodotSectionFile sf;
    parseGodotSections(text, file, sf, diag);

    bool haveHeader = false;
    bool haveResourceSection = false;
    std::unordered_map<std::string, int> pathIndex;  // node path -> index

    for (GodotSection& sec : sf.sections) {
        const std::string& name = sec.tag.name;
        const GodotTag& tag = sec.tag;
        const int line = tag.line;

        if (name.empty()) {
            diag.error(file, sec.props.empty() ? 0 : sec.props[0].line, "properties before the first [section]");
            continue;
        }

        if (name == "gd_scene" || name == "gd_resource") {
            if (haveHeader) {
                diag.error(file, line, "a second [" + name + "] header");
                continue;
            }
            if (&sec != &sf.sections.front())
                diag.error(file, line, "[" + name + "] must be the first section");
            haveHeader = true;
            GodotDocHeader& h = base.header;
            h.kind = name;
            h.line = line;
            h.type = tag.attrString("type");
            h.scriptClass = tag.attrString("script_class");
            h.format = static_cast<int>(tag.attrInt("format", 0));
            h.loadSteps = static_cast<int>(tag.attrInt("load_steps", 0));
            h.uid = tag.attrString("uid");
            if (h.format == 0)
                diag.error(file, line, "the header has no format=N", "godot.format");
            else if (h.format < 2)
                diag.error(file, line, "format=" + std::to_string(h.format) + " is not a Godot 3/4 text format", "godot.format");
            else if (h.format == 2)
                diag.warn(file, line, "format=2 is a Godot 3 file; only Godot 4.1+ is the target, so some properties may not map", "godot.format");
            else if (h.format > 3)
                diag.warn(file, line, "format=" + std::to_string(h.format) + " is newer than the known format 3", "godot.format");
            if (wantScene && name != "gd_scene")
                diag.error(file, line, "this is a [gd_resource], not a scene", "godot.kind");
            if (!wantScene && name != "gd_resource")
                diag.error(file, line, "this is a [gd_scene], not a resource", "godot.kind");
            continue;
        }

        if (!haveHeader) {
            diag.error(file, line, "[" + name + "] before the [gd_scene]/[gd_resource] header");
            haveHeader = true;  // report once
        }

        if (name == "ext_resource") {
            GodotExtResource e;
            e.line = line;
            e.id = idText(tag.attr("id"));
            e.type = tag.attrString("type");
            e.path = tag.attrString("path");
            e.uid = tag.attrString("uid");
            if (e.id.empty()) diag.error(file, line, "[ext_resource] has no id");
            else if (base.findExt(e.id)) diag.error(file, line, "[ext_resource] id \"" + e.id + "\" is declared twice");
            if (e.path.empty() && e.uid.empty()) diag.error(file, line, "[ext_resource] has no path or uid");
            if (!sec.props.empty()) diag.warn(file, sec.props[0].line, "property lines under [ext_resource] are ignored");
            base.extResources.push_back(std::move(e));
        } else if (name == "sub_resource") {
            GodotSubResource s;
            s.line = line;
            s.id = idText(tag.attr("id"));
            s.type = tag.attrString("type");
            if (s.id.empty()) diag.error(file, line, "[sub_resource] has no id");
            else if (base.findSub(s.id)) diag.error(file, line, "[sub_resource] id \"" + s.id + "\" is declared twice");
            s.props = std::move(sec.props);
            base.subResources.push_back(std::move(s));
        } else if (name == "node" && wantScene) {
            GodotNode n;
            n.line = line;
            n.name = tag.attrString("name");
            n.type = tag.attrString("type");
            const GodotVariant* parentAttr = tag.attr("parent");
            n.isRoot = scene->nodes.empty() && !parentAttr;
            if (parentAttr) n.parentPath = parentAttr->isStringLike() ? parentAttr->s : std::string();
            if (const GodotVariant* inst = tag.attr("instance"); inst && inst->type == GodotVType::ExtResource)
                n.instance = inst->s;
            else if (inst)
                diag.error(file, inst->line, "instance= must be ExtResource(\"id\")");
            n.instancePlaceholder = tag.attrString("instance_placeholder");
            n.owner = tag.attrString("owner");
            n.groups = stringList(tag.attr("groups"));
            n.nodePaths = stringList(tag.attr("node_paths"));
            if (const GodotVariant* u = tag.attr("unique_id")) n.uniqueId = u->type == GodotVType::Int ? std::to_string(u->i) : u->s;
            n.index = static_cast<int>(tag.attrInt("index", -1));
            if (const GodotVariant* ix = tag.attr("index"); ix && ix->type == GodotVType::String)
                n.index = std::atoi(ix->s.c_str());  // Godot 3 wrote index="2"
            n.attrs = tag.attrs;
            n.props = std::move(sec.props);

            if (n.name.empty()) diag.error(file, line, "[node] has no name");
            if (!n.isRoot && !parentAttr) diag.error(file, line, "node '" + n.name + "' has no parent and the scene already has a root");
            n.isOverride = !n.isRoot && n.type.empty() && !n.isInstance();

            if (n.isRoot) {
                n.path = ".";
            } else if (n.parentPath == ".") {
                n.path = n.name;
            } else {
                n.path = n.parentPath + "/" + n.name;
            }

            const int myIndex = static_cast<int>(scene->nodes.size());
            if (!n.isRoot) {
                if (n.parentPath == ".") {
                    n.parent = scene->nodes.empty() ? -1 : 0;
                } else {
                    auto it = pathIndex.find(n.parentPath);
                    if (it != pathIndex.end()) {
                        n.parent = it->second;
                    } else {
                        // Walk the longest prefix that is a node of this file: the instance that hosts it.
                        std::string prefix = n.parentPath;
                        int anchor = -1;
                        while (true) {
                            const usize cut = prefix.find_last_of('/');
                            if (cut == std::string::npos) break;
                            prefix.resize(cut);
                            auto pit = pathIndex.find(prefix);
                            if (pit != pathIndex.end()) { anchor = pit->second; break; }
                        }
                        if (anchor < 0 && !scene->nodes.empty()) {
                            // A single-component path: could still be an unseen instance child only if
                            // the root is an inherited scene.
                            if (!scene->nodes[0].instance.empty()) anchor = 0;
                        }
                        if (anchor >= 0) {
                            n.unresolvedParent = true;
                            n.anchor = anchor;
                        } else {
                            diag.error(file, line, "node '" + n.name + "': parent \"" + n.parentPath + "\" is not a node of this scene", "godot.node_parent");
                        }
                    }
                }
            }
            if (!pathIndex.emplace(n.path, myIndex).second)
                diag.error(file, line, "two nodes have the path \"" + n.path + "\"", "godot.node_path");
            if (!n.instance.empty() && !base.findExt(n.instance))
                diag.error(file, line, "instance=ExtResource(\"" + n.instance + "\") is not declared");
            scene->nodes.push_back(std::move(n));
        } else if (name == "connection" && wantScene) {
            GodotConnection c;
            c.line = line;
            c.signal = tag.attrString("signal");
            c.from = tag.attrString("from");
            c.to = tag.attrString("to");
            c.method = tag.attrString("method");
            c.flags = static_cast<int>(tag.attrInt("flags", 0));
            c.unbinds = static_cast<int>(tag.attrInt("unbinds", 0));
            if (const GodotVariant* b = tag.attr("binds"); b && b->type == GodotVType::Array) c.binds = b->items;
            if (c.signal.empty() || c.from.empty() || c.to.empty() || c.method.empty())
                diag.error(file, line, "[connection] needs signal, from, to and method");
            scene->connections.push_back(std::move(c));
        } else if (name == "editable" && wantScene) {
            const std::string p = tag.attrString("path");
            if (p.empty()) diag.error(file, line, "[editable] has no path");
            else scene->editablePaths.push_back(p);
        } else if (name == "resource" && !wantScene) {
            if (haveResourceSection) diag.error(file, line, "a second [resource] section");
            haveResourceSection = true;
            res->props = std::move(sec.props);
        } else {
            diag.warn(file, line, "unknown section [" + name + "] ignored", "godot.unknown_section");
        }
    }

    if (!haveHeader && sf.sections.empty()) diag.error(file, 0, "empty file: no [gd_scene]/[gd_resource] header");

    // Every ExtResource()/SubResource() in a value must point at a declaration.
    for (const GodotSubResource& s : base.subResources) checkProps(base, s.props, diag);
    if (scene) {
        for (const GodotNode& n : scene->nodes) {
            checkProps(base, n.attrs, diag);
            checkProps(base, n.props, diag);
        }
        for (const GodotConnection& c : scene->connections)
            for (const GodotVariant& b : c.binds) checkRefs(base, b, diag);
        if (haveHeader && base.header.kind == "gd_scene" && scene->nodes.empty())
            diag.warn(file, base.header.line, "the scene has no nodes");
    }
    if (res) checkProps(base, res->props, diag);

    return diag.errorCount() == errorsBefore;
}

}  // namespace

bool parseGodotScene(std::string_view text, std::string_view file, GodotScene& out, GodotDiagnostics& diag) {
    out = GodotScene{};
    GodotDocBase base;
    const bool ok = parseDoc(text, file, true, base, &out, nullptr, diag);
    static_cast<GodotDocBase&>(out) = std::move(base);
    return ok;
}

bool parseGodotResource(std::string_view text, std::string_view file, GodotResource& out, GodotDiagnostics& diag) {
    out = GodotResource{};
    GodotDocBase base;
    const bool ok = parseDoc(text, file, false, base, nullptr, &out, diag);
    static_cast<GodotDocBase&>(out) = std::move(base);
    return ok;
}

bool loadGodotScene(const std::string& path, GodotScene& out, GodotDiagnostics& diag, std::string_view displayName) {
    std::string text;
    if (!readGodotTextFile(path, text, diag, displayName)) return false;
    return parseGodotScene(text, displayName.empty() ? std::string_view(path) : displayName, out, diag);
}

bool loadGodotResource(const std::string& path, GodotResource& out, GodotDiagnostics& diag, std::string_view displayName) {
    std::string text;
    if (!readGodotTextFile(path, text, diag, displayName)) return false;
    return parseGodotResource(text, displayName.empty() ? std::string_view(path) : displayName, out, diag);
}

std::string detectGodotTextKind(std::string_view text) {
    GodotVariantReader r(text, {}, nullptr);
    r.skipSpace(true);
    if (r.peek() != '[') return {};
    r.advance();
    const usize s = r.pos();
    while (!r.atEnd() && r.peek() != ']' && r.peek() != ' ' && r.peek() != '\t' && r.peek() != '\n' && r.peek() != '\r') r.advance();
    const std::string name(r.slice(s, r.pos()));
    return (name == "gd_scene" || name == "gd_resource") ? name : std::string();
}

}  // namespace aver::fmt::godot
