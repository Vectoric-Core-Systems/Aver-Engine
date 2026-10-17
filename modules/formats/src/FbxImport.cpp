// FBX import (ufbx): the loader, the scene walk and the meshes. Materials, skeletons and animation are
// FbxMaterials.cpp, FbxSkeleton.cpp and FbxAnimation.cpp. Design: docs/plans/wave9/FBX_IMPORT.md.
#include "aver/formats/FbxImport.hpp"

#include "FbxInternal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

} // namespace

// ---- format sniffing ---------------------------------------------------------------------------

bool looksLikeFbx(const u8* bytes, usize size) {
    static const char kBinary[] = "Kaydara FBX Binary  ";
    if (!bytes) return false;
    if (size >= sizeof(kBinary) && std::memcmp(bytes, kBinary, sizeof(kBinary) - 1) == 0 && bytes[20] == 0) return true;
    // ASCII FBX opens with a ; comment and an FBXHeaderExtension node within the first few lines.
    const usize n = std::min<usize>(size, 4096);
    static const char kAscii[] = "FBXHeaderExtension";
    const usize len = sizeof(kAscii) - 1;
    for (usize i = 0; i + len <= n; ++i)
        if (bytes[i] == 'F' && std::memcmp(bytes + i, kAscii, len) == 0) return true;
    return false;
}

bool fbxImportAvailable() { return AVER_HAVE_UFBX != 0; }

// ---- options as text -----------------------------------------------------------------------------

std::string fbxOptionsToText(const FbxImportOptions& o) {
    char b[64];
    std::string s;
    const auto num = [&](const char* key, double v) {
        std::snprintf(b, sizeof b, "%.9g", v);
        s += key; s += '='; s += b; s += ';';
    };
    const auto flag = [&](const char* key, bool v) { s += key; s += '='; s += v ? '1' : '0'; s += ';'; };
    num("scale", o.scale);
    num("unitMeters", o.unitMetersOverride);
    flag("convertAxes", o.convertAxes);
    flag("generateNormals", o.generateMissingNormals);
    flag("meshes", o.importMeshes);
    flag("skeletons", o.importSkeletons);
    flag("animations", o.importAnimations);
    flag("materials", o.importMaterials);
    flag("blendShapes", o.importBlendShapes);
    flag("linearColours", o.fileColoursAreLinear);
    num("bakeFps", double(o.bakeFps));
    flag("reduceKeys", o.reduceKeys);
    s += "rootMotion=";
    s += o.rootMotion == FbxImportOptions::RootMotion::Off ? "off" : o.rootMotion == FbxImportOptions::RootMotion::All ? "all" : "auto";
    s += ';';
    num("rootMotionMinCm", o.rootMotionMinTravelCm);
    num("maxMemoryMiB", o.maxMemoryMiB);
    s.pop_back();   // the last ';'
    return s;
}

bool fbxOptionsFromText(std::string_view text, FbxImportOptions& out, std::string* why) {
    FbxImportOptions o = out;
    const auto lower = [](std::string_view v) {
        std::string r(v);
        for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    };
    const auto trim = [](std::string_view v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
        return v;
    };
    usize pos = 0;
    while (pos <= text.size()) {
        usize end = text.find_first_of(";\n", pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view item = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (item.empty()) continue;
        const usize eq = item.find('=');
        if (eq == std::string_view::npos) return fail(why, "import option '" + std::string(item) + "' has no '='");
        const std::string key = std::string(trim(item.substr(0, eq)));
        const std::string val = std::string(trim(item.substr(eq + 1)));
        const auto badValue = [&]() { return fail(why, "import option '" + key + "': bad value '" + val + "'"); };
        const auto getNum = [&](double& v) {
            char* endp = nullptr;
            v = std::strtod(val.c_str(), &endp);
            return !val.empty() && endp == val.c_str() + val.size() && std::isfinite(v);
        };
        const auto getFlag = [&](bool& v) {
            const std::string l = lower(val);
            if (l == "1" || l == "true" || l == "on" || l == "yes") { v = true; return true; }
            if (l == "0" || l == "false" || l == "off" || l == "no") { v = false; return true; }
            return false;
        };
        double d = 0;
        if (key == "scale")                { if (!getNum(d) || d <= 0) return badValue(); o.scale = f32(d); }
        else if (key == "unitMeters")      { if (!getNum(d) || d < 0) return badValue(); o.unitMetersOverride = f32(d); }
        else if (key == "convertAxes")     { if (!getFlag(o.convertAxes)) return badValue(); }
        else if (key == "generateNormals") { if (!getFlag(o.generateMissingNormals)) return badValue(); }
        else if (key == "meshes")          { if (!getFlag(o.importMeshes)) return badValue(); }
        else if (key == "skeletons")       { if (!getFlag(o.importSkeletons)) return badValue(); }
        else if (key == "animations")      { if (!getFlag(o.importAnimations)) return badValue(); }
        else if (key == "materials")       { if (!getFlag(o.importMaterials)) return badValue(); }
        else if (key == "blendShapes")     { if (!getFlag(o.importBlendShapes)) return badValue(); }
        else if (key == "linearColours")   { if (!getFlag(o.fileColoursAreLinear)) return badValue(); }
        else if (key == "bakeFps")         { if (!getNum(d) || d < 1 || d > 480) return badValue(); o.bakeFps = static_cast<u32>(d); }
        else if (key == "reduceKeys")      { if (!getFlag(o.reduceKeys)) return badValue(); }
        else if (key == "rootMotion") {
            const std::string l = lower(val);
            if (l == "off") o.rootMotion = FbxImportOptions::RootMotion::Off;
            else if (l == "auto") o.rootMotion = FbxImportOptions::RootMotion::Auto;
            else if (l == "all") o.rootMotion = FbxImportOptions::RootMotion::All;
            else return badValue();
        }
        else if (key == "rootMotionMinCm") { if (!getNum(d) || d < 0) return badValue(); o.rootMotionMinTravelCm = f32(d); }
        else if (key == "maxMemoryMiB")    { if (!getNum(d) || d < 16) return badValue(); o.maxMemoryMiB = f32(d); }
        else return fail(why, "unknown import option '" + key + "'");
    }
    out = o;
    return true;
}

} // namespace aver::fmt

#if AVER_HAVE_UFBX

namespace aver::fmt::fbx {
namespace {

constexpr f32 kDeg = 57.29577951308232f;

// The 3x3 cofactor matrix of the basis, scaled to a largest entry of +-1 with the determinant's sign:
// det * inverse-transpose, normalised. A normal takes THIS, not the basis the positions take; the sign
// keeps a mirrored normal pointing out of the mirrored surface (GltfImport.cpp importPrimitive).
Basis3 normalMatrix(const Basis3& b) {
    const f32* ra = &b.k[0]; const f32* rb = &b.k[3]; const f32* rc = &b.k[6];
    Basis3 n;
    n.k[0] = rb[1]*rc[2] - rb[2]*rc[1]; n.k[1] = rb[2]*rc[0] - rb[0]*rc[2]; n.k[2] = rb[0]*rc[1] - rb[1]*rc[0];
    n.k[3] = rc[1]*ra[2] - rc[2]*ra[1]; n.k[4] = rc[2]*ra[0] - rc[0]*ra[2]; n.k[5] = rc[0]*ra[1] - rc[1]*ra[0];
    n.k[6] = ra[1]*rb[2] - ra[2]*rb[1]; n.k[7] = ra[2]*rb[0] - ra[0]*rb[2]; n.k[8] = ra[0]*rb[1] - ra[1]*rb[0];
    f32 big = 0.0f;
    for (int i = 0; i < 9; ++i) big = std::fmax(big, std::fabs(n.k[i]));
    const f32 s = (det3(b) < 0.0f ? -1.0f : 1.0f) / (big > 0.0f ? big : 1.0f);
    for (int i = 0; i < 9; ++i) n.k[i] *= s;
    return n;
}

// Row-vector transform: x*row0 + y*row1 + z*row2.
inline void applyRows(const Basis3& b, f64 x, f64 y, f64 z, f64 out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = x * b.k[k] + y * b.k[3 + k] + z * b.k[6 + k];
}

bool sameBasis(const Basis3& a, const Basis3& b) {
    // Relative to the basis' own magnitude: a model under a 0.001 unit root has entries near 1e-3.
    f32 mag = 0.0f;
    for (int k = 0; k < 9; ++k) mag = std::fmax(mag, std::fmax(std::fabs(a.k[k]), std::fabs(b.k[k])));
    for (int k = 0; k < 9; ++k) if (std::fabs(a.k[k] - b.k[k]) > 1e-5f * mag) return false;
    return true;
}

// One corner of the triangulated mesh. ufbx_generate_indices compares these with memcmp, so the layout
// is padding-free and zeroed.
struct FbxVert {
    double p[3];
    double n[3];
    double uv[2];
    uint32_t cp;
    uint32_t pad;
};
static_assert(sizeof(FbxVert) == 72, "FbxVert must be padding-free");

// The slot index for a material name, appending it when new.
u32 slotFor(OcMeshData& m, const std::string& name) {
    for (usize i = 0; i < m.materialSlots.size(); ++i) if (m.materialSlots[i] == name) return static_cast<u32>(i);
    m.materialSlots.push_back(name);
    return static_cast<u32>(m.materialSlots.size() - 1);
}

struct MeshIn {
    const ufbx_node* node = nullptr;
    const ufbx_mesh* mesh = nullptr;
    ufbx_matrix g{};                // geometry to world (the bind pose for a skinned mesh)
    bool withTranslation = false;   // skinned meshes keep the translation; static ones hand it to a placement
};

// Builds one engine mesh from `in`. cpOut[v] is vertex v's control point (for skin and blend shapes).
bool buildMeshCopy(Ctx& c, const MeshIn& in, OcMeshData& m, std::vector<u32>& cpOut, std::string* why) {
    const ufbx_mesh* mesh = in.mesh;
    const Basis3 basis = basisOf(in.g);
    const f32 det = det3(basis);
    const Basis3 nrmXf = normalMatrix(basis);
    const bool reverseWinding = c.basis.convertAxes != (det < 0.0f);

    const bool hasNrm = mesh->vertex_normal.exists && mesh->vertex_normal.indices.count >= mesh->num_indices;
    const bool hasUv  = mesh->vertex_uv.exists && mesh->vertex_uv.indices.count >= mesh->num_indices;
    if (!mesh->vertex_position.exists) return fail(why, "FBX: a mesh has no vertex positions");

    // ---- corners, one run per material part ----
    struct Run { usize materialPart; usize begin, count; };
    std::vector<FbxVert> verts;
    verts.reserve(mesh->num_triangles * 3);
    std::vector<Run> runs;
    std::vector<uint32_t> tri(std::max<usize>(mesh->max_face_triangles * 3, 3));

    const auto emitFace = [&](usize fi) {
        if (fi >= mesh->faces.count) return;
        const ufbx_face face = mesh->faces.data[fi];
        if (face.num_indices < 3) return;
        if (fi < mesh->face_hole.count && mesh->face_hole.data[fi]) return;
        const uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
        for (uint32_t t = 0; t < nt * 3; ++t) {
            const uint32_t ix = tri[t];
            FbxVert v;
            std::memset(&v, 0, sizeof v);
            const ufbx_vec3 p = mesh->vertex_position.values.data[mesh->vertex_position.indices.data[ix]];
            v.p[0] = p.x; v.p[1] = p.y; v.p[2] = p.z;
            if (hasNrm) {
                const ufbx_vec3 n = mesh->vertex_normal.values.data[mesh->vertex_normal.indices.data[ix]];
                v.n[0] = n.x; v.n[1] = n.y; v.n[2] = n.z;
            }
            if (hasUv) {
                const ufbx_vec2 uv = mesh->vertex_uv.values.data[mesh->vertex_uv.indices.data[ix]];
                v.uv[0] = uv.x; v.uv[1] = uv.y;
            }
            v.cp = mesh->vertex_indices.data[ix];
            verts.push_back(v);
        }
    };
    if (mesh->material_parts.count > 0) {
        for (usize p = 0; p < mesh->material_parts.count; ++p) {
            const ufbx_mesh_part& part = mesh->material_parts.data[p];
            const usize begin = verts.size();
            for (usize k = 0; k < part.face_indices.count; ++k) emitFace(part.face_indices.data[k]);
            if (verts.size() > begin) runs.push_back({p, begin, verts.size() - begin});
        }
    } else {
        for (usize fi = 0; fi < mesh->faces.count; ++fi) emitFace(fi);
        if (!verts.empty()) runs.push_back({0, 0, verts.size()});
    }
    if (verts.empty()) return fail(why, "FBX: a mesh has no triangles");

    // ---- unique vertices: position + normal + uv + control point ----
    // The control point is in the key so two control points at one position keep their own skin weights
    // and blend offsets; a hard edge stays split and a smooth surface welds.
    std::vector<uint32_t> idx(verts.size());
    ufbx_vertex_stream stream = {verts.data(), verts.size(), sizeof(FbxVert)};
    ufbx_error err = {};
    const usize unique = ufbx_generate_indices(&stream, 1, idx.data(), idx.size(), nullptr, &err);
    if (err.type != UFBX_ERROR_NONE || unique == 0) {
        char msg[256];
        ufbx_format_error(msg, sizeof msg, &err);
        return fail(why, std::string("FBX: could not index a mesh: ") + msg);
    }

    m = OcMeshData{};
    m.positions.resize(unique * 3);
    m.normals.resize(unique * 3);
    m.uvs.resize(unique * 2);
    cpOut.assign(unique, 0);
    const ufbx_vec3 tr = in.g.cols[3];
    for (usize u = 0; u < unique; ++u) {
        const FbxVert& v = verts[u];
        f64 o[3];
        applyRows(basis, v.p[0], v.p[1], v.p[2], o);
        if (in.withTranslation) { o[0] += tr.x; o[1] += tr.y; o[2] += tr.z; }
        const Vec3 p = c.basis.toEngine(o[0], o[1], o[2], false);
        m.positions[u * 3 + 0] = p.x; m.positions[u * 3 + 1] = p.y; m.positions[u * 3 + 2] = p.z;

        if (hasNrm) {
            f64 nn[3];
            applyRows(nrmXf, v.n[0], v.n[1], v.n[2], nn);
            Vec3 e = c.basis.toEngine(nn[0], nn[1], nn[2], true);
            const f32 len = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
            if (len > 1e-12f) { e.x /= len; e.y /= len; e.z /= len; } else { e = Vec3{0, 0, 1}; }
            m.normals[u * 3 + 0] = e.x; m.normals[u * 3 + 1] = e.y; m.normals[u * 3 + 2] = e.z;
        } else {
            m.normals[u * 3 + 0] = 0.0f; m.normals[u * 3 + 1] = 0.0f; m.normals[u * 3 + 2] = 1.0f;   // replaced below if asked
        }
        m.uvs[u * 2 + 0] = f32(v.uv[0]);
        m.uvs[u * 2 + 1] = f32(1.0 - v.uv[1]);      // FBX's origin is bottom-left, like OBJ and USD
        cpOut[u] = v.cp;
    }

    m.indices.resize(idx.size());
    for (usize t = 0; t + 2 < idx.size(); t += 3) {
        const u32 a = idx[t], b = idx[t + 1], cc = idx[t + 2];
        if (reverseWinding) { m.indices[t] = a; m.indices[t + 1] = cc; m.indices[t + 2] = b; }
        else                { m.indices[t] = a; m.indices[t + 1] = b;  m.indices[t + 2] = cc; }
    }

    // Flat normals where the source had none (ufbx smooth ones were asked for first), after the winding fix.
    if (!hasNrm && c.opt->generateMissingNormals) {
        for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
            const u32 ia = m.indices[i], ib = m.indices[i + 1], ic = m.indices[i + 2];
            const f32* A = &m.positions[usize(ia) * 3];
            const f32* B = &m.positions[usize(ib) * 3];
            const f32* C = &m.positions[usize(ic) * 3];
            const f32 e1[3] = {B[0]-A[0], B[1]-A[1], B[2]-A[2]};
            const f32 e2[3] = {C[0]-A[0], C[1]-A[1], C[2]-A[2]};
            f32 n[3] = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
            const f32 len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-12f) { n[0] /= len; n[1] /= len; n[2] /= len; } else { n[0] = 0; n[1] = 0; n[2] = 1; }
            for (const u32 vi : {ia, ib, ic}) {
                m.normals[usize(vi) * 3 + 0] = n[0]; m.normals[usize(vi) * 3 + 1] = n[1]; m.normals[usize(vi) * 3 + 2] = n[2];
            }
        }
    }

    // ---- submeshes: one per non-empty material part ----
    for (const Run& r : runs) {
        const ufbx_material* mat = nullptr;
        if (in.node->materials.count > r.materialPart) mat = in.node->materials.data[r.materialPart];
        else if (mesh->materials.count > r.materialPart) mat = mesh->materials.data[r.materialPart];
        OcMeshSubmesh sm;
        sm.name = "prim" + std::to_string(m.submeshes.size());
        sm.materialSlot = slotFor(m, slotNameOf(c, mat));
        sm.indexStart = static_cast<u32>(r.begin);
        sm.indexCount = static_cast<u32>(r.count);
        sm.baseVertex = 0;
        sm.vertexCount = m.vertexCount();
        m.submeshes.push_back(std::move(sm));
    }
    if (m.materialSlots.empty()) m.materialSlots.push_back("M_Default");
    m.computeBounds();
    return true;
}

#if defined(AVER_OCMESH_HAS_MORPH)
// The one adapter into the morph-target feature (docs/plans/wave9/MORPH_TARGETS.md). The names follow
// FBX_IMPORT.md 2.7; if that feature picks others, this is the only function to change.
void attachBlendShapes(OcMeshData& m, const std::vector<FbxBlendShape>& shapes) {
    for (const FbxBlendShape& s : shapes) {
        OcMorphTarget t;
        t.name = s.name;
        t.vertices = s.vertices;
        t.positionDeltas = s.positionDeltas;
        t.normalDeltas = s.normalDeltas;
        t.defaultWeight = s.defaultWeight;
        m.morphTargets.push_back(std::move(t));
    }
}
#endif

const char* axisName(ufbx_coordinate_axis a) {
    switch (a) {
        case UFBX_COORDINATE_AXIS_POSITIVE_X: return "+X"; case UFBX_COORDINATE_AXIS_NEGATIVE_X: return "-X";
        case UFBX_COORDINATE_AXIS_POSITIVE_Y: return "+Y"; case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return "-Y";
        case UFBX_COORDINATE_AXIS_POSITIVE_Z: return "+Z"; case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return "-Z";
        default: return "unknown";
    }
}

struct SceneGuard {
    ufbx_scene* s = nullptr;
    ~SceneGuard() { if (s) ufbx_free_scene(s); }
};

// The part of the import that needs the loaded scene.
void importScene(Ctx& c) {
    const ufbx_scene* scene = c.scene;
    const FbxImportOptions& o = *c.opt;
    FbxImportResult& r = *c.out;
    const usize nn = scene->nodes.count;

    c.placementOfNode.assign(nn, -1);
    c.bakedBasisOfNode.assign(nn, Basis3{});
    c.skinnedMeshNode.assign(nn, 0);

    // ---- the report's opening lines ----
    {
        char b[256];
        std::snprintf(b, sizeof b, "FBX %u%s, exporter %s; file axes right %s up %s front %s, %g m per unit",
                      scene->metadata.version, scene->metadata.ascii ? " (ascii)" : "",
                      scene->metadata.creator.length ? str(scene->metadata.creator).c_str() : "unknown",
                      axisName(scene->settings.axes.right), axisName(scene->settings.axes.up), axisName(scene->settings.axes.front),
                      double(scene->settings.original_unit_meters));
        c.line(b);
        c.line(o.convertAxes ? "axes: converted to the engine frame (the glTF map)" : "axes: left as authored (convertAxes off)");
        c.line(o.unitMetersOverride > 0 ? "unit: overridden to " + std::to_string(o.unitMetersOverride) + " m per file unit"
                                        : "unit: the file's own, converted to metres");
        for (usize i = 0; i < scene->metadata.warnings.count; ++i) {
            const ufbx_warning& w = scene->metadata.warnings.data[i];
            c.line("ufbx: " + str(w.description) + (w.count > 1 ? " (x" + std::to_string(w.count) + ")" : std::string()));
        }
    }

    // ---- which mesh nodes are imported, and which of them are skinned ----
    std::vector<const ufbx_node*> meshNodes, skinnedNodes;
    std::unordered_set<const ufbx_node*> skinnedSet;
    u32 hidden = 0;
    for (usize i = 0; i < nn; ++i) {
        const ufbx_node* n = scene->nodes.data[i];
        if (n->is_root || !n->mesh) continue;
        if (!n->visible) { ++hidden; continue; }
        const ufbx_mesh* mesh = n->mesh;
        if (mesh->num_triangles == 0) { c.line("mesh '" + str(n->name) + "' has no triangles and was skipped"); continue; }
        meshNodes.push_back(n);
        if (o.importSkeletons && mesh->skin_deformers.count > 0 && mesh->skin_deformers.data[0]->clusters.count > 0)
            { skinnedNodes.push_back(n); skinnedSet.insert(n); }
        if (mesh->vertex_color.exists) c.note("vertex colour");
        if (mesh->uv_sets.count > 1) c.note("a second UV set (only the first is carried)");
        if (mesh->subdivision_render_levels > 0 && mesh->subdivision_display_mode != UFBX_SUBDIVISION_DISPLAY_DISABLED)
            c.note("subdivision surfaces (the control cage was imported, unsubdivided)");
        if (mesh->cache_deformers.count > 0) c.note("geometry caches");
    }
    if (hidden) c.line(std::to_string(hidden) + " hidden mesh node(s) skipped");
    if (scene->nurbs_surfaces.count > 0 || scene->nurbs_curves.count > 0) c.note("NURBS curves and surfaces");
    if (scene->lights.count > 0) {
        c.note("lights (counted, not imported)");
        c.line(std::to_string(scene->lights.count) + " light(s) in the file, not imported");
    }

    if (o.importMaterials || o.importMeshes) importMaterials(c);
    if (o.importSkeletons) buildSkeletons(c, skinnedNodes);
    else c.boneOfNode.assign(nn, BoneRef{});

    // ---- meshes ----
    if (o.importMeshes) {
        struct Built { Basis3 basis; std::vector<u32> mats; bool skinned; i32 index; };
        std::unordered_map<u32, std::vector<Built>> built;          // by mesh element typed_id
        std::unordered_map<u32, usize> nextSuffix;
        std::unordered_set<std::string> usedNames;

        const auto uniqueName = [&](const std::string& base) -> std::string {
            if (base.empty()) return std::string();
            std::string cand = base;
            for (int k = 2; !usedNames.insert(lowerOf(cand)).second; ++k) cand = base + "_" + std::to_string(k);
            return cand;
        };

        for (const ufbx_node* n : meshNodes) {
            const ufbx_mesh* mesh = n->mesh;
            const bool wantSkin = skinnedSet.count(n) != 0;
            const ufbx_skin_deformer* skin = wantSkin ? mesh->skin_deformers.data[0] : nullptr;
            ufbx_matrix g = n->geometry_to_world;
            bool skinned = false;
            if (skin) {
                ufbx_matrix bind;
                if (skinBindMatrix(c, n, skin, bind) && skeletonOfSkin(c, skin) >= 0) { g = bind; skinned = true; }
                else c.line("skin of '" + str(n->name) + "' has no usable bone: imported as a static mesh");
            }

            // The material set this node uses, so a node-level override makes its own copy.
            std::vector<u32> mats;
            const ufbx_material_list& ml = n->materials.count > 0 ? n->materials : mesh->materials;
            for (usize k = 0; k < ml.count; ++k) mats.push_back(ml.data[k] ? ml.data[k]->typed_id : UFBX_NO_INDEX);

            const Basis3 key = basisOf(g);
            i32 target = -1;
            for (const Built& b : built[mesh->typed_id])
                if (b.skinned == skinned && b.mats == mats && sameBasis(b.basis, key)) { target = b.index; break; }
            if (target < 0) {
                OcMeshData md;
                std::vector<u32> cp;
                std::string why;
                MeshIn in;
                in.node = n; in.mesh = mesh; in.g = g; in.withTranslation = skinned;
                if (!buildMeshCopy(c, in, md, cp, &why)) {
                    c.line("mesh '" + str(n->name) + "' skipped: " + why);
                    continue;
                }
                if (skinned) writeSkinStreams(c, skin, cp, md);

                std::string name;
                auto& list = built[mesh->typed_id];
                if (list.empty()) {
                    name = uniqueName(!str(n->name).empty() ? str(n->name) : str(mesh->name));
                } else {
                    const std::string base = !str(mesh->name).empty() ? str(mesh->name) : str(n->name);
                    usize& suffix = nextSuffix[mesh->typed_id];
                    if (suffix < 2) suffix = 2;
                    name = base.empty() ? std::string() : base + "_" + std::to_string(suffix++);
                    if (!name.empty() && !usedNames.insert(lowerOf(name)).second) name = uniqueName(name);
                }
                target = static_cast<i32>(r.meshes.size());
                r.meshes.push_back(std::move(md));
                r.meshNames.push_back(name);
                r.meshSkinIndex.push_back(skinned ? skeletonOfSkin(c, skin) : -1);
                r.blendShapes.emplace_back();
                list.push_back(Built{key, mats, skinned, target});

                if (o.importBlendShapes && mesh->blend_deformers.count > 0) {
                    u32 channels = 0;
                    for (usize d = 0; d < mesh->blend_deformers.count; ++d) channels += static_cast<u32>(mesh->blend_deformers.data[d]->channels.count);
                    if (channels > 0) {
#if defined(AVER_OCMESH_HAS_MORPH)
                        u32 inBetweens = 0;
                        r.blendShapes.back() = extractFbxBlendShapes(c.basis, mesh, cp, key, &inBetweens);
                        attachBlendShapes(r.meshes.back(), r.blendShapes.back());
                        if (inBetweens) c.note("in-between blend shapes (only each channel's full shape is carried)");
#else
                        c.note("blend shapes (the morph-target feature has not landed; no mesh data was written for them)");
                        c.line("mesh '" + str(n->name) + "': " + std::to_string(channels) + " blend channel(s) not imported");
#endif
                    }
                }
            }

            // Rigid attachment of an unskinned mesh to a bone stays a static placement, and says so.
            if (!skinned) {
                for (const ufbx_node* p = n->parent; p && !p->is_root; p = p->parent) {
                    if (p->typed_id < c.boneOfNode.size() && c.boneOfNode[p->typed_id].skeleton >= 0) {
                        c.line("mesh '" + str(n->name) + "' is rigidly attached to bone '" + str(p->name) +
                               "': imported as a static placement");
                        break;
                    }
                }
            }

            GltfPlacement pl;
            pl.meshIndex = target;
            pl.position = skinned ? Vec3{0, 0, 0} : c.basis.pos(n->geometry_to_world.cols[3]);
            pl.name = str(n->name);
            if (pl.name.empty()) pl.name = r.meshNames[usize(target)];
            // What the copy was baked with: the first node's basis when nodes share one copy.
            Basis3 baked = key;
            for (const Built& b : built[mesh->typed_id]) if (b.index == target) { baked = b.basis; break; }
            c.placementOfNode[n->typed_id] = static_cast<i32>(r.placements.size());
            c.bakedBasisOfNode[n->typed_id] = baked;
            c.skinnedMeshNode[n->typed_id] = skinned ? 1 : 0;
            r.placements.push_back(std::move(pl));
        }
        if (r.meshes.empty() && !meshNodes.empty()) c.line("no mesh could be built");
    }

    // ---- cameras ----
    for (usize i = 0; i < nn; ++i) {
        const ufbx_node* n = scene->nodes.data[i];
        if (n->is_root || !n->camera || !n->visible) continue;
        ImportedCamera ic;
        ic.name = str(n->name);
        ic.position = c.basis.pos(n->node_to_world.cols[3]);
        const Vec3 fe = c.basis.dir(ufbx_vec3_normalize(n->node_to_world.cols[0]));   // FBX cameras look down +X
        const f32 len = std::sqrt(fe.x * fe.x + fe.y * fe.y + fe.z * fe.z);
        if (len > 1e-12f) {
            ic.yawDeg = std::atan2(fe.y, fe.x) * kDeg;
            ic.pitchDeg = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fe.z / len))) * kDeg;
        }
        const f64 fov = n->camera->field_of_view_deg.y > 0.0 ? n->camera->field_of_view_deg.y : n->camera->field_of_view_deg.x;
        ic.fovDeg = fov > 0.0 ? f32(fov) : 60.0f;
        r.cameras.push_back(std::move(ic));
    }

    if (o.importAnimations) importAnimations(c);

    r.stats.nodes = static_cast<u32>(nn);
    r.stats.meshes = static_cast<u32>(r.meshes.size());
    r.stats.clips = static_cast<u32>(r.animations.size() + r.objectAnimations.size());
    if (r.stats.truncatedInfluenceVertices)
        c.line(std::to_string(r.stats.truncatedInfluenceVertices) + " vertex(es) had more than " +
               std::to_string(kOcMeshInfluences) + " bone influences; the top " + std::to_string(kOcMeshInfluences) +
               " were kept (largest dropped weight " + std::to_string(r.stats.maxDroppedWeight) + ")");
}

} // namespace

// ---- blend shapes (the seam the morph-target feature plugs into) --------------------------------

std::vector<FbxBlendShape> extractFbxBlendShapes(const Basis& basis, const ufbx_mesh* mesh,
                                                 const std::vector<u32>& vertexCp, const Basis3& linear,
                                                 u32* inBetweens) {
    std::vector<FbxBlendShape> shapes;
    // control point -> the output vertices it split into (a hard edge makes several)
    std::vector<std::pair<u32, u32>> byCp;
    byCp.reserve(vertexCp.size());
    for (usize v = 0; v < vertexCp.size(); ++v) byCp.push_back({vertexCp[v], static_cast<u32>(v)});
    std::sort(byCp.begin(), byCp.end());
    const Basis3 nrm = normalMatrix(linear);

    for (usize d = 0; d < mesh->blend_deformers.count; ++d) {
        const ufbx_blend_deformer* bd = mesh->blend_deformers.data[d];
        for (usize ci = 0; ci < bd->channels.count; ++ci) {
            const ufbx_blend_channel* ch = bd->channels.data[ci];
            if (ch->keyframes.count == 0) continue;
            const ufbx_blend_keyframe* full = &ch->keyframes.data[0];
            for (usize k = 1; k < ch->keyframes.count; ++k)
                if (ch->keyframes.data[k].target_weight > full->target_weight) full = &ch->keyframes.data[k];
            for (usize k = 0; k < ch->keyframes.count; ++k) {
                const f64 w = ch->keyframes.data[k].target_weight;
                if (inBetweens && w > 1e-9 && w < full->target_weight - 1e-9) ++*inBetweens;
            }
            const ufbx_blend_shape* shape = full->shape;
            if (!shape) continue;

            FbxBlendShape s;
            s.name = str(ch->name);
            s.defaultWeight = f32(std::min(1.0, std::max(0.0, double(ch->weight))));
            for (usize i = 0; i < shape->num_offsets; ++i) {
                const u32 cp = shape->offset_vertices.data[i];
                const ufbx_vec3 po = shape->position_offsets.data[i];
                f64 lp[3], ln[3] = {0, 0, 0};
                applyRows(linear, po.x, po.y, po.z, lp);
                const Vec3 dp = basis.toEngine(lp[0], lp[1], lp[2], false);
                Vec3 dn{0, 0, 0};
                const bool haveN = i < shape->normal_offsets.count;
                if (haveN) {
                    const ufbx_vec3 no = shape->normal_offsets.data[i];
                    applyRows(nrm, no.x, no.y, no.z, ln);
                    dn = basis.toEngine(ln[0], ln[1], ln[2], true);
                }
                if (dp.size() < 1e-6f && dn.size() < 1e-6f) continue;       // a zero delta is not stored
                const auto lo = std::lower_bound(byCp.begin(), byCp.end(), std::make_pair(cp, u32(0)));
                for (auto it = lo; it != byCp.end() && it->first == cp; ++it) {
                    s.vertices.push_back(it->second);
                    s.positionDeltas.insert(s.positionDeltas.end(), {dp.x, dp.y, dp.z});
                    if (haveN) s.normalDeltas.insert(s.normalDeltas.end(), {dn.x, dn.y, dn.z});
                }
            }
            if (!s.vertices.empty()) shapes.push_back(std::move(s));
        }
    }
    return shapes;
}

} // namespace aver::fmt::fbx

#endif // AVER_HAVE_UFBX

namespace aver::fmt {
namespace {

bool importImpl(const u8* bytes, usize size, const std::string& baseDir, const std::string& stem,
                FbxImportResult& out, const FbxImportOptions& o, std::string* why) {
    out = FbxImportResult{};
    if (!bytes || size < 8) return fail(why, "FBX: file is too small");
    if (!looksLikeFbx(bytes, size))
        return fail(why, "FBX: not an FBX file (no Kaydara binary header and no FBXHeaderExtension)");
#if !AVER_HAVE_UFBX
    (void)baseDir; (void)stem; (void)o;
    return fail(why, "FBX: built without ufbx");
#else
    ufbx_load_opts lo = {};
    lo.file_format = UFBX_FILE_FORMAT_FBX;
    if (o.convertAxes) lo.target_axes = ufbx_axes_right_handed_y_up;
    else lo.target_axes = ufbx_coordinate_axes{UFBX_COORDINATE_AXIS_UNKNOWN, UFBX_COORDINATE_AXIS_UNKNOWN, UFBX_COORDINATE_AXIS_UNKNOWN};
    lo.target_unit_meters = o.unitMetersOverride > 0.0f ? 0.0 : 1.0;      // 0 = no unit conversion: the override is applied below
    lo.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    lo.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;                  // only acts on a left-handed file
    lo.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    lo.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    lo.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_ROTATION_PIVOT;
    lo.generate_missing_normals = o.generateMissingNormals;
    lo.normalize_normals = true;
    lo.clean_skin_weights = true;
    lo.use_blender_pbr_material = true;                                   // recovers roughness/metalness from Blender's Phong
    lo.load_external_files = false;                                       // WE read textures; ufbx must not open arbitrary paths
    lo.ignore_embedded = false;
    lo.ignore_animation = !o.importAnimations;
    lo.evaluate_skinning = false;
    lo.skip_skin_vertices = !o.importSkeletons;
    const size_t limit = static_cast<size_t>(std::max(16.0f, o.maxMemoryMiB)) << 20;
    lo.temp_allocator.memory_limit = limit;
    lo.result_allocator.memory_limit = limit;
    lo.node_depth_limit = 512;

    ufbx_error err = {};
    fbx::SceneGuard guard;
    guard.s = ufbx_load_memory(bytes, size, &lo, &err);
    if (!guard.s) {
        char msg[512];
        ufbx_format_error(msg, sizeof msg, &err);
        return fail(why, std::string("FBX: ") + msg);
    }

    fbx::Ctx c;
    c.scene = guard.s;
    c.opt = &o;
    c.out = &out;
    c.baseDir = baseDir;
    c.sourceStem = stem;
    c.basis.convertAxes = o.convertAxes;
    c.basis.scale = o.scale * (o.unitMetersOverride > 0.0f ? o.unitMetersOverride : 1.0f);
    fbx::importScene(c);

    if (out.meshes.empty() && out.skeletons.empty() && out.animations.empty() && out.objectAnimations.empty() &&
        out.cameras.empty())
        return fail(why, "FBX: the file contains nothing this importer carries (no visible mesh, skeleton or clip)");
    return true;
#endif
}

} // namespace

bool importFbxFromMemory(const u8* bytes, usize size, const std::string& baseDir, FbxImportResult& out,
                         const FbxImportOptions& opt, std::string* why) {
    return importImpl(bytes, size, baseDir, std::string(), out, opt, why);
}

bool importFbx(const std::string& path, FbxImportResult& out, const FbxImportOptions& opt, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "FBX: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "FBX: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, "FBX: short read on " + path);

    const usize slash = path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
    const usize dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return importImpl(bytes.data(), bytes.size(), dir, stem, out, opt, why);
}

} // namespace aver::fmt
