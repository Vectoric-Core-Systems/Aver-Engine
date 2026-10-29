// G4 R7: geometry the converter bakes or maps: ArrayMesh surfaces, MultiMesh tables, CSG trees, Sprite3D quads and GridMap cells. Contract: docs/plans/fabric/G4_RENDER.md R7.
#include "FabricInternal.hpp"
#include "FabricMeshGen.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/Csg.hpp"
#include "aver/formats/MeshBuild.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/godot/GodotMesh.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>

namespace aver::fabric {

namespace csg = aver::fmt::csg;

bool isCsgClass(const std::string& t) {
    return t == "CSGShape3D" || t == "CSGPrimitive3D" || t == "CSGCombiner3D" || t == "CSGBox3D" || t == "CSGSphere3D" || t == "CSGCylinder3D" || t == "CSGTorus3D" ||
           t == "CSGPolygon3D" || t == "CSGMesh3D";
}

namespace {

constexpr double kPi = 3.14159265358979323846;

struct ResRef {
    std::string type;
    const std::vector<gd::GodotProperty>* props = nullptr;
    const gd::GodotDocBase* doc = nullptr;
};

std::optional<ResRef> resolveRes(Converter& c, const Var& v, const gd::GodotDocBase& doc) {
    if (v.type == VType::SubResource) {
        if (const gd::GodotSubResource* sr = doc.findSub(v.s)) return ResRef{sr->type, &sr->props, &doc};
    } else if (v.type == VType::ExtResource) {
        if (const gd::GodotExtResource* e = doc.findExt(v.s)) {
            const std::string res = c.fs_.resolve(*e, &c.diag_, doc.file);
            if (res.empty()) return std::nullopt;
            if (const gd::GodotResource* r = c.resourceDoc(res)) return ResRef{r->header.type, &r->props, r};
        }
    }
    return std::nullopt;
}

const Var* findP(const std::vector<gd::GodotProperty>& props, const char* name) {
    for (const gd::GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

const Prop* nodeProp(const FNode& n, const char* key) {
    for (const Prop& p : n.props)
        if (p.key == key) return &p;
    return nullptr;
}
double nodeNum(const FNode& n, const char* key, double def) { const Prop* p = nodeProp(n, key); return p ? numOf(&p->value, def) : def; }
bool nodeBool(const FNode& n, const char* key, bool def) { const Prop* p = nodeProp(n, key); return p ? boolOf(&p->value, def) : def; }

Xform xformOfVar(const Var* v) {
    Xform x;
    if (v && v->type == VType::Transform3D && v->nums.size() >= 12) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) x.basis.m[r][c] = v->nums[static_cast<size_t>(r * 3 + c)];
        x.pos = {v->nums[9], v->nums[10], v->nums[11]};
    }
    return x;
}

Var transform3DVar(const Xform& x) {
    Var v;
    v.type = VType::Transform3D;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) v.nums.push_back(x.basis.m[r][c]);
    v.nums.insert(v.nums.end(), {x.pos.x, x.pos.y, x.pos.z});
    return v;
}

void xformRow12(const Xform& x, double out[12]) {   // the 3x4 column-vector matrix csg::transform takes
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = x.basis.m[r][c];
        out[r * 4 + 3] = r == 0 ? x.pos.x : (r == 1 ? x.pos.y : x.pos.z);
    }
}

Var packedFloats(const std::vector<double>& f) {
    Var v;
    v.type = VType::PackedFloat32Array;
    v.nums = f;
    return v;
}

Var packedVec3(const std::vector<double>& f) {
    Var v;
    v.type = VType::PackedVector3Array;
    v.nums = f;
    return v;
}

Var makeColor(double r, double g, double b, double a) {
    Var v;
    v.type = VType::Color;
    v.nums = {r, g, b, a};
    return v;
}

Var makeArray(std::vector<Var> items) {
    Var v;
    v.type = VType::Array;
    v.items = std::move(items);
    return v;
}

std::unique_ptr<FNode> syntheticNode(FNode& parent, const std::string& name, const std::string& type) {
    auto n = std::make_unique<FNode>();
    n->name = name;
    n->type = type;
    n->sceneFile = parent.sceneFile;
    n->parent = &parent;
    n->owner = nullptr;
    n->reportFile = parent.reportFile;
    n->reportPath = parent.reportPath.empty() ? name : parent.reportPath + "/" + name;
    n->line = parent.line;
    return n;
}

void setProp(FNode& n, const std::string& key, Var v) {
    Prop p;
    p.key = key;
    p.value = std::move(v);
    n.props.push_back(std::move(p));
}

bool saveMesh(Converter& c, const fmt::OcMeshData& m, const std::string& asset, const Where& w, const char* type) {
    createDirectories(c.contentDir_ + "/Meshes/Generated");
    std::string err;
    if (!fmt::saveOcMesh(c.contentDir_ + "/" + asset, m, &err)) {
        c.report_.add("resource", w.file, w.node, type, "", Rating::Unsupported, "could not write " + asset + ": " + err, 0);
        return false;
    }
    return true;
}

// An .ocmesh written by this converter, back in Godot space (metres, +Y up, counter-clockwise) as CSG polygons; one material index per submesh.
csg::Mesh ocMeshToCsg(const fmt::OcMeshData& m, std::vector<std::string>& slots) {
    csg::Mesh out;
    const auto slotIndex = [&](const std::string& name) {
        for (size_t i = 0; i < slots.size(); ++i) if (slots[i] == name) return static_cast<int>(i);
        slots.push_back(name);
        return static_cast<int>(slots.size() - 1);
    };
    const auto vtx = [&](u32 i) {
        csg::Vertex v;
        // The inverse of fmt::axis: engine (x, y, z) cm = (-zg, xg, yg) * 100.
        const double ex = m.positions[usize(i) * 3], ey = m.positions[usize(i) * 3 + 1], ez = m.positions[usize(i) * 3 + 2];
        v.p[0] = ey / 100.0; v.p[1] = ez / 100.0; v.p[2] = -ex / 100.0;
        const double nx = m.normals[usize(i) * 3], ny = m.normals[usize(i) * 3 + 1], nz = m.normals[usize(i) * 3 + 2];
        v.n[0] = ny; v.n[1] = nz; v.n[2] = -nx;
        v.uv[0] = m.uvs[usize(i) * 2]; v.uv[1] = m.uvs[usize(i) * 2 + 1];
        return v;
    };
    std::vector<fmt::OcMeshSubmesh> subs = m.submeshes;
    if (subs.empty()) { fmt::OcMeshSubmesh s; s.indexCount = static_cast<u32>(m.indices.size()); subs.push_back(s); }
    for (const fmt::OcMeshSubmesh& sm : subs) {
        const std::string name = sm.materialSlot < m.materialSlots.size() ? m.materialSlots[sm.materialSlot] : std::string("M_Default");
        const int mat = slotIndex(name);
        for (u32 i = sm.indexStart; i + 2 < sm.indexStart + sm.indexCount && i + 2 < m.indices.size(); i += 3) {
            csg::Polygon p;
            p.material = mat;
            // The axis map reverses winding when it writes a mesh; undo that here.
            p.v = {vtx(m.indices[i]), vtx(m.indices[i + 2]), vtx(m.indices[i + 1])};
            out.polys.push_back(std::move(p));
        }
    }
    return out;
}

struct CsgCtx {
    Converter& c;
    std::vector<std::string> slots;
    Where where;
    bool pathUnsupported = false;
    int shapes = 0;
};

int slotOfMaterial(CsgCtx& ctx, const FNode& n) {
    std::string name = "M_Default";
    if (const Prop* p = nodeProp(n, "material"); p && p->doc) {
        const Var cv = ctx.c.convertValue(p->value, *p->doc, ctx.where, "material");
        if (cv.type == VType::Resource) name = stemOf(cv.s);
    }
    for (size_t i = 0; i < ctx.slots.size(); ++i) if (ctx.slots[i] == name) return static_cast<int>(i);
    ctx.slots.push_back(name);
    return static_cast<int>(ctx.slots.size() - 1);
}

csg::Mesh evalCsg(const FNode& n, CsgCtx& ctx) {
    csg::Mesh own;
    const std::string& t = n.type;
    if (t == "CSGCombiner3D" || t == "CSGShape3D") {
        // no geometry of its own
    } else if (t == "CSGMesh3D") {
        const Prop* mp = nodeProp(n, "mesh");
        if (mp && mp->doc) {
            const Var cv = ctx.c.convertValue(mp->value, *mp->doc, ctx.where, "mesh");
            fmt::OcMeshData md;
            std::string why;
            if (cv.type == VType::Resource && fmt::loadOcMesh(ctx.c.contentDir_ + "/" + cv.s, md, &why)) own = ocMeshToCsg(md, ctx.slots);
            else ctx.c.report_.add("property", n.reportFile, n.reportPath, t, "mesh", Rating::Unsupported, "the mesh could not be read back into the CSG tree" + (why.empty() ? std::string() : ": " + why), 0);
        }
    } else {
        const int mat = slotOfMaterial(ctx, n);
        const bool smooth = nodeBool(n, "smooth_faces", true);
        if (t == "CSGBox3D") {
            const Prop* sp = nodeProp(n, "size");
            const V3 s = vec3Of(sp ? &sp->value : nullptr, {1, 1, 1});
            const double size[3] = {s.x, s.y, s.z};
            own = csg::makeBox(size, mat);
        } else if (t == "CSGSphere3D") {
            own = csg::makeSphere(nodeNum(n, "radius", 0.5), static_cast<int>(nodeNum(n, "radial_segments", 12)), static_cast<int>(nodeNum(n, "rings", 6)), smooth, mat);
        } else if (t == "CSGCylinder3D") {
            own = csg::makeCylinder(nodeNum(n, "radius", 0.5), nodeNum(n, "height", 2.0), static_cast<int>(nodeNum(n, "sides", 8)), nodeBool(n, "cone", false), smooth, mat);
        } else if (t == "CSGTorus3D") {
            own = csg::makeTorus(nodeNum(n, "inner_radius", 0.5), nodeNum(n, "outer_radius", 1.0), static_cast<int>(nodeNum(n, "sides", 8)), static_cast<int>(nodeNum(n, "ring_sides", 6)), smooth, mat);
        } else if (t == "CSGPolygon3D") {
            const Prop* pp = nodeProp(n, "polygon");
            std::vector<double> xy;
            if (pp && pp->value.type == VType::PackedVector2Array) xy = pp->value.nums;
            else xy = {0, 0, 0, 1, 1, 1, 1, 0};   // Godot's default polygon: a unit square
            const int mode = static_cast<int>(nodeNum(n, "mode", 0));
            if (mode == 0) own = csg::extrudePolygon(xy, nodeNum(n, "depth", 1.0), mat);
            else if (mode == 1) own = csg::spinPolygon(xy, nodeNum(n, "spin_degrees", 360.0), static_cast<int>(nodeNum(n, "spin_sides", 5)), mat);
            else ctx.pathUnsupported = true;
        }
    }
    if (nodeBool(n, "invert_faces", false)) csg::invertFaces(own);
    csg::Mesh result = std::move(own);
    for (const auto& child : n.children) {
        if (!isCsgClass(child->type)) continue;
        csg::Mesh cm = evalCsg(*child, ctx);
        if (cm.empty() && static_cast<int>(nodeNum(*child, "operation", 0)) != 0) continue;
        double m34[12];
        xformRow12(child->hasXform ? child->xf : Xform{}, m34);
        csg::transform(cm, m34);
        result = csg::combine(result, cm, static_cast<csg::Op>(std::min(2, std::max(0, static_cast<int>(nodeNum(*child, "operation", 0))))));
    }
    return result;
}

fmt::OcMeshData soupToOcMesh(const csg::Soup& s, const std::vector<std::string>& slots, bool tangents) {
    fmt::OcMeshData m;
    const size_t n = s.pos.size() / 3;
    m.positions.resize(n * 3);
    m.normals.resize(n * 3);
    m.uvs = s.uv;
    for (size_t i = 0; i < n; ++i) {
        const Vec3 p = fmt::axis::position(s.pos[i * 3], s.pos[i * 3 + 1], s.pos[i * 3 + 2]);
        const Vec3 nn = fmt::axis::direction(s.nrm[i * 3], s.nrm[i * 3 + 1], s.nrm[i * 3 + 2]);
        m.positions[i * 3] = p.x; m.positions[i * 3 + 1] = p.y; m.positions[i * 3 + 2] = p.z;
        m.normals[i * 3] = nn.x; m.normals[i * 3 + 1] = nn.y; m.normals[i * 3 + 2] = nn.z;
    }
    for (size_t r = 0; r < s.materials.size(); ++r) {
        fmt::OcMeshSubmesh sm;
        const int mat = s.materials[r];
        const std::string name = mat >= 0 && static_cast<size_t>(mat) < slots.size() ? slots[static_cast<size_t>(mat)] : std::string("M_Default");
        u32 slot = 0;
        for (; slot < m.materialSlots.size(); ++slot) if (m.materialSlots[slot] == name) break;
        if (slot == m.materialSlots.size()) m.materialSlots.push_back(name);
        sm.name = name;
        sm.materialSlot = slot;
        sm.indexStart = static_cast<u32>(m.indices.size());
        for (u32 i = 0; i + 2 < s.rangeCount[r]; i += 3) {   // the axis map reverses every triangle
            const u32 a = s.idx[s.rangeStart[r] + i], b = s.idx[s.rangeStart[r] + i + 1], c = s.idx[s.rangeStart[r] + i + 2];
            m.indices.insert(m.indices.end(), {a, c, b});
        }
        sm.indexCount = static_cast<u32>(m.indices.size()) - sm.indexStart;
        sm.vertexCount = static_cast<u32>(n);
        m.submeshes.push_back(std::move(sm));
    }
    m.computeBounds();
    if (tangents) fmt::generateTangents(m);
    return m;
}

// PNG width and height from the IHDR chunk; false for any other file.
bool pngSize(const std::string& disk, int& w, int& h) {
    std::vector<u8> b;
    if (disk.empty() || !readFileBytes(disk, b) || b.size() < 24) return false;
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) if (b[static_cast<size_t>(i)] != sig[i]) return false;
    w = static_cast<int>((u32(b[16]) << 24) | (u32(b[17]) << 16) | (u32(b[18]) << 8) | u32(b[19]));
    h = static_cast<int>((u32(b[20]) << 24) | (u32(b[21]) << 16) | (u32(b[22]) << 8) | u32(b[23]));
    return w > 0 && h > 0;
}

}  // namespace

// ---- ArrayMesh ------------------------------------------------------------------------------------------------------------

std::string Converter::arrayMeshAsset(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w) {
    const Var* surfaces = findP(props, "_surfaces");
    if (!surfaces || surfaces->type != VType::Array || surfaces->items.empty()) {
        report_.add("resource", doc.file, w.node, "ArrayMesh", "", Rating::Unsupported, "an ArrayMesh with no stored surfaces (one built by a script is made at run time)", 0);
        return {};
    }
    const std::string key = "ArrayMesh|" + doc.file + "|" + fnvHex(gd::godotVariantToText(*surfaces));
    if (const auto it = meshAssets_.find(key); it != meshAssets_.end()) return it->second;
    meshAssets_[key] = {};

    std::vector<fmt::RuntimeSurface> out;
    std::vector<std::string> allNotes;
    int skipped = 0;
    size_t totalTris = 0;
    for (size_t si = 0; si < surfaces->items.size(); ++si) {
        const Var& sd = surfaces->items[si];
        gd::GodotSurface gs;
        std::string why;
        std::vector<std::string> notes;
        if (!gd::decodeArrayMeshSurface(sd, gs, &why, &notes)) {
            report_.add("resource", doc.file, w.node, "ArrayMesh", "", Rating::Unsupported, "surface " + std::to_string(si) + " is not decoded: " + why, 0);
            ++skipped;
            continue;
        }
        for (const std::string& n : notes) allNotes.push_back("surface " + std::to_string(si) + ": " + n);
        if (gs.boneCount > 0) allNotes.push_back("surface " + std::to_string(si) + ": bone indices and weights are dropped (the mesh is drawn static)");

        fmt::RuntimeSurface rs;
        const size_t n = gs.vertexCount;
        rs.pos.resize(n * 3);
        for (size_t i = 0; i < n; ++i) {
            const Vec3 p = fmt::axis::position(gs.pos[i * 3], gs.pos[i * 3 + 1], gs.pos[i * 3 + 2]);
            rs.pos[i * 3] = p.x; rs.pos[i * 3 + 1] = p.y; rs.pos[i * 3 + 2] = p.z;
        }
        if (!gs.nrm.empty()) {
            rs.nrm.resize(n * 3);
            for (size_t i = 0; i < n; ++i) {
                const Vec3 d = fmt::axis::direction(gs.nrm[i * 3], gs.nrm[i * 3 + 1], gs.nrm[i * 3 + 2]);
                rs.nrm[i * 3] = d.x; rs.nrm[i * 3 + 1] = d.y; rs.nrm[i * 3 + 2] = d.z;
            }
        }
        rs.uv = gs.uv;
        rs.col = gs.col;
        rs.uv1 = gs.uv2;
        if (!gs.tan.empty() && !gs.nrm.empty()) {
            rs.tan.resize(n * 4);
            for (size_t i = 0; i < n; ++i) {
                // As the glTF importer: the bitangent cross(N, T) * w points at decreasing v, so the stored direction of increasing v is its negation.
                const f32* nl = &gs.nrm[i * 3];
                const f32* tv = &gs.tan[i * 4];
                const f32 bl[3] = {(nl[1] * tv[2] - nl[2] * tv[1]) * tv[3], (nl[2] * tv[0] - nl[0] * tv[2]) * tv[3], (nl[0] * tv[1] - nl[1] * tv[0]) * tv[3]};
                const Vec3 te = fmt::axis::direction(tv[0], tv[1], tv[2]);
                const Vec3 dv = fmt::axis::direction(-bl[0], -bl[1], -bl[2]);
                const f32 tgt[3] = {te.x, te.y, te.z}, dir[3] = {dv.x, dv.y, dv.z};
                fmt::packTangent(&rs.nrm[i * 3], tgt, dir, &rs.tan[i * 4]);
            }
        }
        // Godot's front faces are clockwise and the axis map has determinant -1: the stored order is already the engine's (the primitive generators write
        // counter-clockwise triangles and toOcMesh reverses them to the same result).
        rs.idx = gs.indices;
        totalTris += rs.idx.size() / 3;
        rs.material = "M_Default";
        if (sd.type == VType::Dictionary)
            if (const Var* mv = sd.find("material"); mv && !mv->isNil()) {
                const Var cv = convertValue(*mv, doc, w, "material");
                if (cv.type == VType::Resource) rs.material = stemOf(cv.s);
            }
        out.push_back(std::move(rs));
    }
    if (out.empty()) return {};
    fmt::OcMeshData mesh;
    std::string why;
    if (!fmt::assembleSurfaces(out, mesh, &why)) {
        report_.add("resource", doc.file, w.node, "ArrayMesh", "", Rating::Unsupported, "the decoded surfaces do not make a mesh: " + why, 0);
        return {};
    }
    if (!mesh.hasTangent()) fmt::generateTangents(mesh);
    const std::string asset = "Meshes/Generated/ArrayMesh_" + fnvHex(key) + ".ocmesh";
    if (!saveMesh(*this, mesh, asset, w, "ArrayMesh")) return {};
    std::string say = "decoded " + std::to_string(out.size()) + " surface(s), " + std::to_string(mesh.vertexCount()) + " vertices, " + std::to_string(totalTris) + " triangles into " + asset;
    report_.add("resource", doc.file, w.node, "ArrayMesh", "", Rating::Converted, say, 0);
    for (const std::string& n : allNotes) report_.add("resource", doc.file, w.node, "ArrayMesh", "", Rating::Approximated, n, 0);
    if (skipped) report_.add("resource", doc.file, w.node, "ArrayMesh", "", Rating::Approximated, std::to_string(skipped) + " surface(s) were left out", 0);
    const Var* shapeNames = findP(props, "_blend_shape_names");
    const bool hasShapes = shapeNames && shapeNames->count() > 0;   // the mode (a binary .res always stores it) means nothing without shapes
    for (const gd::GodotProperty& p : props) {
        if (p.name == "_surfaces" || p.name == "resource_name" || p.name == "resource_local_to_scene" || p.name == "resource_path" || p.name == "script") continue;
        if (p.name == "blend_shape_mode" || p.name == "_blend_shape_names") {
            if (hasShapes) report_.add("property", doc.file, w.node, "ArrayMesh", p.name, Rating::Unsupported, "blend shapes are not decoded", p.line);
        }
        else if (p.name == "custom_aabb" || p.name == "shadow_mesh" || p.name == "lightmap_size_hint")
            report_.add("property", doc.file, w.node, "ArrayMesh", p.name, Rating::Approximated, "not used: the bounds follow the vertices and a shadow proxy is not a separate mesh", p.line);
    }
    meshAssets_[key] = asset;
    return asset;
}

// ---- MultiMesh ------------------------------------------------------------------------------------------------------------

Var Converter::multiMeshObject(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w) {
    const int format = static_cast<int>(numOf(findP(props, "transform_format"), 0));
    if (format != 1) {
        report_.add("resource", doc.file, w.node, "MultiMesh", "transform_format", Rating::Unsupported, "a 2D MultiMesh (Transform2D instances) has no counterpart; only 3D tables are converted", 0);
        return makeNil();
    }
    const bool colors = boolOf(findP(props, "use_colors"), false), custom = boolOf(findP(props, "use_custom_data"), false);
    const int count = static_cast<int>(numOf(findP(props, "instance_count"), 0));
    const size_t stride = 12 + (colors ? 4 : 0) + (custom ? 4 : 0);
    std::vector<std::pair<std::string, Var>> o;
    if (const Var* m = findP(props, "mesh")) {
        const Var cv = convertValue(*m, doc, w, "mesh");
        if (!cv.isNil()) o.emplace_back("mesh", cv);
    }
    o.emplace_back("transform_format", makeInt(1));
    o.emplace_back("use_colors", makeBool(colors));
    o.emplace_back("use_custom_data", makeBool(custom));
    o.emplace_back("instance_count", makeInt(count));
    o.emplace_back("visible_instance_count", makeInt(static_cast<long long>(numOf(findP(props, "visible_instance_count"), -1))));
    const Var* buf = findP(props, "buffer");
    if (buf && buf->type == VType::PackedFloat32Array) {
        std::vector<double> nums = buf->nums;
        if (nums.size() != static_cast<size_t>(count) * stride) {
            report_.add("resource", doc.file, w.node, "MultiMesh", "buffer", Rating::Approximated,
                        "the buffer holds " + std::to_string(nums.size()) + " floats; " + std::to_string(count) + " instances of " + std::to_string(stride) + " floats need " + std::to_string(static_cast<size_t>(count) * stride) + "; it is cut or padded to fit", 0);
            nums.resize(static_cast<size_t>(count) * stride, 0.0);
        }
        o.emplace_back("buffer", packedFloats(nums));
    } else if (count > 0) {
        report_.add("resource", doc.file, w.node, "MultiMesh", "buffer", Rating::Approximated, "no instance data is stored; the instances are identity transforms until a script sets them", 0);
    }
    for (const gd::GodotProperty& p : props) {
        if (p.name == "custom_aabb") report_.add("property", doc.file, w.node, "MultiMesh", p.name, Rating::Approximated, "not used: the baked mesh is bounded by its own vertices", p.line);
        else if (p.name == "physics_interpolation_quality") report_.add("property", doc.file, w.node, "MultiMesh", p.name, Rating::Unsupported, "physics interpolation is not modelled", p.line);
    }
    report_.add("resource", doc.file, w.node, "MultiMesh", "", Rating::Converted,
                "instance transforms" + std::string(colors ? ", colours" : "") + std::string(custom ? " and custom data" : "") +
                    " are drawn as one mesh the engine bakes from the table (a vertex colour multiplies the albedo, so the material needs vertex_color_use_as_albedo, as in Godot)", 0);
    return makeObject("MultiMesh", std::move(o));
}

// ---- per-node geometry ----------------------------------------------------------------------------------------------------

void Converter::geometryNode(FNode& fn, std::vector<std::pair<std::string, Var>>& props, std::vector<std::unique_ptr<FNode>>& synthetic, ClassChoice& cc) {
    const std::string& t = fn.type;
    const Where w{fn.reportFile, fn.reportPath, fn.type};
    const auto rate = [&](const std::string& key, Rating r, const std::string& why) { report_.add("property", fn.reportFile, fn.reportPath, t, key, r, why, 0); };
    const auto hasKey = [&](const char* k) { return nodeProp(fn, k) != nullptr; };

    if (t == "MultiMeshInstance3D") {
        if (hasKey("multimesh")) rate("multimesh", Rating::Converted, "the instance table is baked into one mesh by the engine (a draw per MultiMesh, shadows and global illumination as an ordinary mesh)");
        return;
    }

    if (t == "Label3D") {
        cc.rating = Rating::Approximated;
        cc.reason = "placed as a Label3D with Godot's properties; the engine has no world-space text renderer, so the text is NOT drawn (use a Control label on screen)";
        for (const Prop& p : fn.props) {
            if (p.key == "text") rate(p.key, Rating::Unsupported, "kept as the label's text; nothing is drawn for it in the world");
            else if (p.key == "pixel_size" || p.key == "billboard" || p.key == "modulate" || p.key == "outline_modulate" || p.key == "double_sided" || p.key == "alpha_cut" || p.key == "font" || p.key == "font_size" ||
                     p.key == "outline_size" || p.key == "horizontal_alignment" || p.key == "vertical_alignment" || p.key == "width" || p.key == "autowrap_mode" || p.key == "shaded" || p.key == "fixed_size" ||
                     p.key == "no_depth_test" || p.key == "render_priority" || p.key == "outline_render_priority" || p.key == "line_spacing" || p.key == "uppercase" || p.key == "texture_filter")
                rate(p.key, Rating::Unsupported, "kept on the node; it only matters when the text is drawn");
        }
        return;
    }

    if (t == "Sprite3D") {
        const Prop* texProp = nodeProp(fn, "texture");
        if (!texProp || !texProp->doc || texProp->value.type != VType::ExtResource) {
            cc.rating = Rating::Approximated;
            cc.reason = "a Sprite3D with no texture file draws nothing";
            return;
        }
        const gd::GodotDocBase& doc = *texProp->doc;
        const gd::GodotExtResource* e = doc.findExt(texProp->value.s);
        const std::string res = e ? fs_.resolve(*e, &diag_, doc.file) : std::string{};
        int tw = 64, th = 64;
        const bool sized = !res.empty() && pngSize(fs_.toDiskPath(res), tw, th);
        if (!sized) rate("texture", Rating::Approximated, "the texture's size could not be read (only PNG headers are); a 64 x 64 quad stands in, so pixel_size does not give Godot's size");
        const double ps = nodeNum(fn, "pixel_size", 0.01);
        const int hf = std::max(1, static_cast<int>(nodeNum(fn, "hframes", 1))), vf = std::max(1, static_cast<int>(nodeNum(fn, "vframes", 1)));
        double bx = 0, by = 0, bw = tw, bh = th;
        if (nodeBool(fn, "region_enabled", false))
            if (const Prop* rp = nodeProp(fn, "region_rect"); rp && rp->value.type == VType::Rect2 && rp->value.nums.size() >= 4) {
                bx = rp->value.nums[0]; by = rp->value.nums[1]; bw = rp->value.nums[2]; bh = rp->value.nums[3];
            }
        int frame = static_cast<int>(nodeNum(fn, "frame", 0));
        if (const Prop* fc = nodeProp(fn, "frame_coords"); fc && fc->value.nums.size() >= 2) frame = static_cast<int>(fc->value.nums[1]) * hf + static_cast<int>(fc->value.nums[0]);
        frame = std::max(0, std::min(hf * vf - 1, frame));
        const double fw = bw / hf, fh = bh / vf;
        const double sx = bx + (frame % hf) * fw, sy = by + (frame / hf) * fh;
        double u0 = sx / tw, u1 = (sx + fw) / tw, v0 = sy / th, v1 = (sy + fh) / th;
        if (nodeBool(fn, "flip_h", false)) std::swap(u0, u1);
        if (nodeBool(fn, "flip_v", false)) std::swap(v0, v1);
        double ox = 0, oy = 0;
        if (const Prop* op = nodeProp(fn, "offset"); op && op->value.nums.size() >= 2) { ox = op->value.nums[0]; oy = op->value.nums[1]; }
        const bool centered = nodeBool(fn, "centered", true);
        const double x0 = (centered ? ox - fw / 2 : ox) * ps, x1 = x0 + fw * ps;
        const double y1 = -(centered ? oy - fh / 2 : oy) * ps, y0 = y1 - fh * ps;   // y1 is the top edge
        SoupMesh soup;
        const auto vert = [&](double x, double y, double u, double v) {
            double px = x, py = y, pz = 0, nx = 0, ny = 0, nz = 1;
            const int axis = static_cast<int>(nodeNum(fn, "axis", 2));
            if (axis == 0) { pz = -x; px = 0; nx = 1; nz = 0; }                   // faces +X
            else if (axis == 1) { pz = -y; py = 0; nz = 0; ny = 1; }             // faces +Y
            soup.pos.insert(soup.pos.end(), {float(px), float(py), float(pz)});
            soup.nrm.insert(soup.nrm.end(), {float(nx), float(ny), float(nz)});
            soup.uv.insert(soup.uv.end(), {float(u), float(v)});
        };
        vert(x0, y0, u0, v1); vert(x1, y0, u1, v1); vert(x1, y1, u1, v0); vert(x0, y1, u0, v0);
        soup.idx = {0, 1, 2, 0, 2, 3};
        if (nodeNum(fn, "axis", 2) != 2) rate("axis", Rating::Approximated, "the quad is turned to face +X or +Y; Godot's exact orientation of those axes is not modelled");

        // The material: Godot's SpriteBase3D properties as a StandardMaterial3D.
        std::vector<gd::GodotProperty> mp;
        const auto addMat = [&](const char* name, const Var& v) { gd::GodotProperty p; p.name = name; p.value = v; mp.push_back(std::move(p)); };
        addMat("albedo_texture", texProp->value);
        if (const Prop* mod = nodeProp(fn, "modulate"); mod && mod->value.type == VType::Color) addMat("albedo_color", mod->value);
        const bool transparent = nodeBool(fn, "transparent", true);
        const int alphaCut = static_cast<int>(nodeNum(fn, "alpha_cut", 0));
        int transparency = 0;
        if (transparent) transparency = alphaCut == 1 ? 2 : (alphaCut == 3 ? 3 : (alphaCut == 2 ? 4 : 1));
        addMat("transparency", makeInt(transparency));
        if (const Prop* th2 = nodeProp(fn, "alpha_scissor_threshold")) addMat("alpha_scissor_threshold", th2->value);
        if (const Prop* hs = nodeProp(fn, "alpha_hash_scale")) addMat("alpha_hash_scale", hs->value);
        addMat("shading_mode", makeInt(nodeBool(fn, "shaded", false) ? 1 : 0));
        addMat("cull_mode", makeInt(nodeBool(fn, "double_sided", true) ? 2 : 0));
        if (const int bb = static_cast<int>(nodeNum(fn, "billboard", 0)); bb != 0) addMat("billboard_mode", makeInt(bb));
        if (nodeBool(fn, "no_depth_test", false)) addMat("no_depth_test", makeBool(true));
        if (nodeBool(fn, "fixed_size", false)) addMat("fixed_size", makeBool(true));
        if (const Prop* rp = nodeProp(fn, "render_priority")) addMat("render_priority", rp->value);
        const std::string stem = stemOf(res);
        std::string canon = "Sprite3D|" + doc.file + "|" + texProp->value.s;
        for (const gd::GodotProperty& p : mp) canon += "|" + p.name + "=" + gd::godotVariantToText(p.value);
        const std::string matAsset = materialAsset(canon, "StandardMaterial3D", mp, doc, stem.empty() ? "Sprite" : stem + "_Sprite", w);
        const std::string slot = matAsset.empty() ? "M_Default" : stemOf(matAsset);
        fmt::OcMeshData mesh = toOcMesh(soup, slot);
        std::string qcanon = "SpriteQuad|" + std::to_string(x0) + "|" + std::to_string(x1) + "|" + std::to_string(y0) + "|" + std::to_string(y1) + "|" + std::to_string(u0) + "|" + std::to_string(u1) +
                             "|" + std::to_string(v0) + "|" + std::to_string(v1) + "|" + std::to_string(static_cast<int>(nodeNum(fn, "axis", 2))) + "|" + slot;
        const std::string asset = "Meshes/Generated/SpriteQuad_" + fnvHex(qcanon) + ".ocmesh";
        if (saveMesh(*this, mesh, asset, w, "Sprite3D")) {
            props.emplace_back("mesh", makeResource(asset));
            if (!matAsset.empty()) props.emplace_back("material_override", makeResource(matAsset));
        }
        for (const Prop& p : fn.props) {
            const std::string& k = p.key;
            if (k == "texture" || k == "pixel_size" || k == "centered" || k == "offset" || k == "flip_h" || k == "flip_v" || k == "hframes" || k == "vframes" || k == "frame" || k == "frame_coords" ||
                k == "region_enabled" || k == "region_rect" || k == "modulate" || k == "transparent" || k == "shaded" || k == "double_sided" || k == "no_depth_test" || k == "render_priority" || k == "fixed_size")
                rate(k, Rating::Converted, "baked into the quad's geometry and UVs, or into the sprite's material");
            else if (k == "billboard") rate(k, static_cast<int>(numOf(&p.value, 0)) == 3 ? Rating::Approximated : Rating::Converted, "the material's billboard mode (enabled, or Y-billboard)");
            else if (k == "alpha_cut") rate(k, alphaCut == 1 || alphaCut == 0 ? Rating::Converted : Rating::Approximated, alphaCut == 1 ? "alpha scissor" : (alphaCut == 0 ? "alpha blend" : "hash and opaque pre-pass are the material's approximations"));
            else if (k == "alpha_scissor_threshold" || k == "alpha_hash_scale") rate(k, Rating::Converted, "the material's cut-off");
            else if (k == "texture_filter" || k == "alpha_antialiasing_mode" || k == "alpha_antialiasing_edge") rate(k, Rating::Approximated, "the engine's own texture filtering and antialiasing apply");
            else if (k == "axis") {}
            else if (k == "sorting_offset" || k == "use_texture_repeat") rate(k, Rating::Unsupported, "not modelled");
        }
        return;
    }

    if (isCsgClass(t) && !(fn.parent && isCsgClass(fn.parent->type))) {
        CsgCtx ctx{*this, {}, w, false, 0};
        csg::Mesh result = evalCsg(fn, ctx);
        if (ctx.pathUnsupported) rate("mode", Rating::Unsupported, "a CSGPolygon3D extruded along a path is not baked; its depth and spin modes are");
        const double snap = nodeNum(fn, "snap", 0.001);
        const csg::Soup soup = csg::toSoup(result, snap);
        if (soup.idx.empty()) {
            cc.rating = Rating::Unsupported;
            cc.reason = "the CSG tree produced no geometry";
            return;
        }
        const fmt::OcMeshData mesh = soupToOcMesh(soup, ctx.slots, nodeBool(fn, "calculate_tangents", true));
        const std::string asset = "Meshes/Generated/CSG_" + fnvHex(fn.reportFile + ":" + fn.reportPath) + ".ocmesh";
        if (!saveMesh(*this, mesh, asset, w, t.c_str())) return;
        props.erase(std::remove_if(props.begin(), props.end(), [](const auto& kv) { return kv.first == "mesh"; }), props.end());
        props.emplace_back("mesh", makeResource(asset));
        cc.rating = Rating::Approximated;
        cc.reason = "the tree's boolean operations are baked at convert time into a static mesh (" + std::to_string(soup.pos.size() / 3) + " vertices); a property changed at run time does not rebuild it";
        for (const Prop& p : fn.props) {
            const std::string& k = p.key;
            if (k == "operation") rate(k, Rating::Converted, "union, intersection and subtraction against the parent's result, in child order");
            else if (k == "snap") rate(k, Rating::Converted, "vertices are snapped to the grid");
            else if (k == "calculate_tangents") rate(k, Rating::Converted, "tangents are generated from the UVs");
            else if (k == "invert_faces") rate(k, Rating::Converted, "the faces are turned inside out before they are combined");
            else if (k == "use_collision" || k == "collision_layer" || k == "collision_mask" || k == "collision_priority") {}
            else if (k == "smooth_faces" || k == "radius" || k == "radial_segments" || k == "rings" || k == "height" || k == "sides" || k == "cone" || k == "size" || k == "inner_radius" || k == "outer_radius" ||
                     k == "ring_sides" || k == "polygon" || k == "depth" || k == "spin_degrees" || k == "spin_sides" || k == "mode" || k == "material" || k == "mesh")
                rate(k, Rating::Converted, "baked into the mesh");
            else if (k == "path_node" || k == "path_interval_type" || k == "path_interval" || k == "path_simplify_angle" || k == "path_rotation" || k == "path_local" || k == "path_continuous_u" || k == "path_u_distance" || k == "path_joined")
                rate(k, Rating::Unsupported, "path extrusion is not baked");
            else if (k == "smooth_faces") {}
        }
        if (nodeBool(fn, "use_collision", false)) {
            // Godot makes a body inside the CSG node; here it is a static body child with the triangles as a concave shape.
            std::vector<double> tri;
            for (size_t i = 0; i < soup.idx.size(); ++i) {
                const uint32_t v = soup.idx[i];
                tri.insert(tri.end(), {soup.pos[v * 3], soup.pos[v * 3 + 1], soup.pos[v * 3 + 2]});
            }
            auto body = syntheticNode(fn, "_CSGCollision", "StaticBody3D");
            setProp(*body, "collision_layer", makeInt(static_cast<long long>(nodeNum(fn, "collision_layer", 1))));
            setProp(*body, "collision_mask", makeInt(static_cast<long long>(nodeNum(fn, "collision_mask", 1))));
            auto shape = syntheticNode(*body, "Shape", "CollisionShape3D");
            setProp(*shape, "shape", makeObject("ConcavePolygonShape3D", {{"data", packedVec3(tri)}}));
            body->children.push_back(std::move(shape));
            synthetic.push_back(std::move(body));
            rate("use_collision", Rating::Approximated, "collision is a static body child (_CSGCollision) holding the baked triangles as a concave shape; layer and mask are kept");
        }
        return;
    }

    if (t == "GridMap") {
        const Prop* libProp = nodeProp(fn, "mesh_library");
        const Prop* dataProp = nodeProp(fn, "data");
        std::vector<gd::GodotGridCell> cells;
        std::string why;
        if (dataProp && !gd::decodeGridCells(dataProp->value, cells, &why)) rate("data", Rating::Unsupported, "the cell table is not read: " + why);
        // The library's items: id -> mesh asset, mesh transform and collision shapes.
        struct Item { std::string name, meshAsset; Xform meshT; std::vector<std::pair<Var, Xform>> shapes; bool has = false; };
        std::map<int, Item> items;
        std::vector<Var> libItems;
        if (libProp && libProp->doc) {
            const auto lib = resolveRes(*this, libProp->value, *libProp->doc);
            if (lib && lib->type == "MeshLibrary") {
                for (const gd::GodotProperty& p : *lib->props) {
                    if (p.name.rfind("item/", 0) != 0) continue;
                    const size_t slash = p.name.find('/', 5);
                    if (slash == std::string::npos) continue;
                    const int id = std::atoi(p.name.substr(5, slash - 5).c_str());
                    const std::string field = p.name.substr(slash + 1);
                    Item& it = items[id];
                    it.has = true;
                    if (field == "name") it.name = stringOf(&p.value);
                    else if (field == "mesh") {
                        const Var cv = convertValue(p.value, *lib->doc, w, "mesh_library");
                        if (cv.type == VType::Resource) it.meshAsset = cv.s;
                    } else if (field == "mesh_transform") it.meshT = xformOfVar(&p.value);
                    else if (field == "shapes" && p.value.type == VType::Array) {
                        for (size_t k = 0; k + 1 < p.value.items.size(); k += 2) {
                            const Var sv = convertValue(p.value.items[k], *lib->doc, w, "mesh_library");
                            if (sv.type == VType::Object) it.shapes.emplace_back(sv, xformOfVar(&p.value.items[k + 1]));
                        }
                    } else if (field == "navigation_mesh" || field == "navigation_mesh_transform" || field == "navigation_layers") {
                        // The transform and layers are always stored; they only matter when the item has a navigation mesh.
                        const std::string meshKey = "item/" + p.name.substr(5, slash - 5) + "/navigation_mesh";
                        bool hasNavMesh = false;
                        for (const gd::GodotProperty& q : *lib->props)
                            if (q.name == meshKey && !q.value.isNil()) hasNavMesh = true;
                        if (hasNavMesh)
                            report_.add("property", libProp->doc->file, w.node, "MeshLibrary", p.name, Rating::Unsupported, "navigation meshes of library items are not converted", p.line);
                    }
                }
                for (const auto& [id, it] : items)
                    libItems.push_back(makeDict({{"id", makeInt(id)}, {"name", makeString(it.name)}, {"mesh", it.meshAsset.empty() ? makeNil() : makeResource(it.meshAsset)}}));
                // The runtime MeshLibrary: names and meshes for scripts that ask.
                for (auto& kv : props) if (kv.first == "mesh_library") kv.second = makeObject("MeshLibrary", {{"items", makeArray(libItems)}});
                rate("mesh_library", Rating::Converted, std::to_string(items.size()) + " item(s) read; each used item's meshes become instances and its shapes become static collision");
            } else {
                rate("mesh_library", Rating::Unsupported, "the MeshLibrary resource could not be read");
            }
        } else if (!cells.empty()) {
            rate("mesh_library", Rating::Unsupported, "the grid has cells but no MeshLibrary: nothing to place");
        }
        props.erase(std::remove_if(props.begin(), props.end(), [](const auto& kv) { return kv.first == "mesh_library" && kv.second.isNil(); }), props.end());
        const Prop* csProp = nodeProp(fn, "cell_size");
        const V3 cellSize = vec3Of(csProp ? &csProp->value : nullptr, {2, 2, 2});
        const double cellScale = nodeNum(fn, "cell_scale", 1.0);
        const V3 off{cellSize.x * 0.5 * (nodeBool(fn, "cell_center_x", true) ? 1 : 0), cellSize.y * 0.5 * (nodeBool(fn, "cell_center_y", true) ? 1 : 0),
                     cellSize.z * 0.5 * (nodeBool(fn, "cell_center_z", true) ? 1 : 0)};
        std::map<int, std::vector<double>> buffers;
        std::map<int, int> counts;
        constexpr int kMaxShapes = 4096;
        int shapeCount = 0, shapesDropped = 0, noMesh = 0, unknownItem = 0;
        auto body = syntheticNode(fn, "_GridCollision", "StaticBody3D");
        setProp(*body, "collision_layer", makeInt(static_cast<long long>(nodeNum(fn, "collision_layer", 1))));
        setProp(*body, "collision_mask", makeInt(static_cast<long long>(nodeNum(fn, "collision_mask", 1))));
        for (const gd::GodotGridCell& c : cells) {
            const auto itf = items.find(c.item);
            if (itf == items.end()) { ++unknownItem; continue; }
            const Item& it = itf->second;
            double b[9];
            gd::gdGridBasis(c.orientation, b);
            Xform cell;
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) cell.basis.m[r][k] = b[r * 3 + k] * cellScale;
            cell.pos = {c.x * cellSize.x + off.x, c.y * cellSize.y + off.y, c.z * cellSize.z + off.z};
            if (!it.meshAsset.empty()) {
                const Xform x = cell.composed(it.meshT);
                std::vector<double>& buf = buffers[c.item];
                for (int r = 0; r < 3; ++r) {
                    for (int k = 0; k < 3; ++k) buf.push_back(x.basis.m[r][k]);
                    buf.push_back(r == 0 ? x.pos.x : (r == 1 ? x.pos.y : x.pos.z));
                }
                ++counts[c.item];
            } else {
                ++noMesh;
            }
            for (const auto& [shape, local] : it.shapes) {
                if (shapeCount >= kMaxShapes) { ++shapesDropped; continue; }
                auto cs = syntheticNode(*body, "Shape" + std::to_string(shapeCount), "CollisionShape3D");
                cs->xf = cell.composed(local);
                cs->hasXform = true;
                setProp(*cs, "shape", shape);
                body->children.push_back(std::move(cs));
                ++shapeCount;
            }
        }
        for (auto& [id, buf] : buffers) {
            const Item& it = items[id];
            auto node = syntheticNode(fn, "Item" + std::to_string(id) + "_" + safeName(it.name, "item"), "MultiMeshInstance3D");
            setProp(*node, "multimesh", makeObject("MultiMesh", {{"mesh", makeResource(it.meshAsset)}, {"transform_format", makeInt(1)}, {"use_colors", makeBool(false)},
                                                                  {"use_custom_data", makeBool(false)}, {"instance_count", makeInt(counts[id])}, {"visible_instance_count", makeInt(-1)},
                                                                  {"buffer", packedFloats(buf)}}));
            synthetic.push_back(std::move(node));
        }
        if (shapeCount > 0) synthetic.push_back(std::move(body));
        if (shapesDropped) report_.add("node", fn.reportFile, fn.reportPath, t, "", Rating::Approximated, std::to_string(shapesDropped) + " collision shape(s) beyond the first " + std::to_string(kMaxShapes) + " were left out", 0);
        if (noMesh) report_.add("node", fn.reportFile, fn.reportPath, t, "", Rating::Approximated, std::to_string(noMesh) + " cell(s) use a library item with no mesh and draw nothing", 0);
        if (unknownItem) report_.add("node", fn.reportFile, fn.reportPath, t, "", Rating::Unsupported, std::to_string(unknownItem) + " cell(s) name an item the library does not have", 0);
        cc.rating = Rating::Approximated;
        cc.reason = std::to_string(cells.size()) + " cell(s) placed as " + std::to_string(buffers.size()) + " instanced item node(s)" + (shapeCount ? " and " + std::to_string(shapeCount) + " static collision shape(s)" : std::string()) +
                    "; the grid is built once at convert time, so set_cell_item at run time changes the stored cells but not what is drawn";
        for (const Prop& p : fn.props) {
            const std::string& k = p.key;
            if (k == "cell_size" || k == "cell_center_x" || k == "cell_center_y" || k == "cell_center_z" || k == "cell_scale" || k == "data") rate(k, Rating::Converted, "applied to the cell transforms");
            else if (k == "collision_layer" || k == "collision_mask") rate(k, Rating::Converted, "set on the static body made for the items' shapes");
            else if (k == "cell_octant_size") rate(k, Rating::Approximated, "the grid is not split into octants");
            else if (k == "collision_priority" || k == "physics_material") rate(k, Rating::Approximated, "kept; the baked body does not read it");
            else if (k == "bake_navigation") rate(k, Rating::Unsupported, "navigation meshes are not baked");
        }
        return;
    }
}

}  // namespace aver::fabric
