// The FBX importer (FbxImport.hpp) over fixtures written in code as ASCII FBX 7.4 (FbxFixture.hpp): frames and
// units, winding and mirrors, triangulation, welding, instancing, skin and bind pose, sockets, animation,
// materials and textures, cameras, the blend-shape extractor, options text and determinism, plus one run of
// AverAssetC. Every expected number below is derived in the comment beside it. No GPU. Exits 0 or 1.
//
// Where the design's test list differs from what is here: a binary-FBX smoke fixture is not generated (the
// builder writes ASCII only); the "same cube as GLB, bit-exact" comparison is replaced by the analytic engine
// coordinates it would have matched; the Euler 350 -> 10 degree case is replaced by a rotation through more than
// 180 degrees staying in one hemisphere, because FBX interpolates rotation in Euler angles and ufbx bakes the
// long way, which is what the DCC shows.
#include "FbxFixture.hpp"

#include "FbxInternal.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/FbxImport.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;
using fbxfix::Fbx;
using fbxfix::MeshDesc;
using fbxfix::V3;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    check(std::fabs(got - want) <= tol, what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}
static void checkVec(const f32* p, f32 x, f32 y, f32 z, f32 tol, const std::string& what) {
    check(std::fabs(p[0] - x) <= tol && std::fabs(p[1] - y) <= tol && std::fabs(p[2] - z) <= tol,
          what + "  (got " + std::to_string(p[0]) + ", " + std::to_string(p[1]) + ", " + std::to_string(p[2]) + ")");
}
static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static bool anyContains(const std::vector<std::string>& v, const std::string& needle) {
    for (const std::string& s : v) if (contains(s, needle)) return true;
    return false;
}

static bool importText(const Fbx& f, fmt::FbxImportResult& r, const fmt::FbxImportOptions& o = {}, std::string* why = nullptr,
                       const std::string& dir = std::string()) {
    const std::string t = f.toString();
    return fmt::importFbxFromMemory(reinterpret_cast<const u8*>(t.data()), t.size(), dir, r, o, why);
}

// ---- geometry helpers ----

static Vec3 vtx(const fmt::OcMeshData& m, u32 i) { return Vec3{m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]}; }
static Vec3 cross3(const Vec3& a, const Vec3& b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// Geometric normal of triangle t from its index order (left-handed engine, counter-clockwise as indexed).
static Vec3 triCross(const fmt::OcMeshData& m, usize t) {
    const Vec3 a = vtx(m, m.indices[t * 3]), b = vtx(m, m.indices[t * 3 + 1]), c = vtx(m, m.indices[t * 3 + 2]);
    return cross3(b - a, c - a);
}
static f32 totalArea(const fmt::OcMeshData& m) {
    f32 area = 0;
    for (usize t = 0; t < m.indices.size() / 3; ++t) area += 0.5f * triCross(m, t).size();
    return area;
}

static const fmt::GltfPlacement* placementNamed(const fmt::FbxImportResult& r, const std::string& name) {
    for (const fmt::GltfPlacement& p : r.placements) if (p.name == name) return &p;
    return nullptr;
}

// ---- animation helpers ----

static const fmt::OcTrack* findTrack(const fmt::OcAnimation& c, u16 bone, u8 channel) {
    for (const fmt::OcTrack& t : c.tracks) if (t.boneIndex == bone && t.channels == channel) return &t;
    return nullptr;
}
static u32 widthOf(u8 channel) { return channel == fmt::kOcChannelRotation ? 4u : 3u; }
// The track's value at `time` by linear interpolation (components, no slerp: tests compare translations).
static std::vector<f32> sampleLinear(const fmt::OcTrack& t, f32 time) {
    const u32 w = widthOf(t.channels);
    std::vector<f32> out(w, 0.0f);
    if (t.times.empty()) return out;
    if (time <= t.times.front() || t.times.size() == 1) { for (u32 k = 0; k < w; ++k) out[k] = t.values[k]; return out; }
    if (time >= t.times.back()) { for (u32 k = 0; k < w; ++k) out[k] = t.values[(t.times.size() - 1) * w + k]; return out; }
    usize i = 0;
    while (i + 1 < t.times.size() && t.times[i + 1] <= time) ++i;
    const f32 span = t.times[i + 1] - t.times[i];
    const f32 s = span > 0 ? (time - t.times[i]) / span : 0.0f;
    for (u32 k = 0; k < w; ++k) out[k] = t.values[i * w + k] + (t.values[(i + 1) * w + k] - t.values[i * w + k]) * s;
    return out;
}

static Fbx::Curve curve(int target, const char* prop, char axis, std::vector<std::pair<double, double>> keys, V3 base = {}, bool step = false) {
    Fbx::Curve c;
    c.target = target; c.prop = prop; c.axis = axis; c.keys = std::move(keys); c.step = step; c.base = base;
    return c;
}

// Root, Mid (0,1,0 above Root) and Tip (0,1,0 above Mid) bones in a Y-up metre file, a triangle skinned to them:
// vertex 0 -> Root 1.0, vertex 1 -> Mid 0.6 + Tip 0.4, vertex 2 -> nothing.
struct Rig { int root = 0, mid = 0, tip = 0, mesh = 0, geom = 0; };
static Rig buildRig(Fbx& f, int rootParent = 0, V3 midRest = {0, 1, 0}) {
    Rig r;
    r.root = f.bone("Root", rootParent, {0, 0, 0});
    r.mid = f.bone("Mid", r.root, midRest);
    r.tip = f.bone("Tip", r.mid, {0, 1, 0});
    r.mesh = f.mesh("Skin", 0, fbxfix::triangle());
    r.geom = f.geometryOf(r.mesh);
    f.skin(r.geom, {
        {r.root, {0}, {1.0}, {}, fbxfix::matTRS({0, 0, 0})},
        {r.mid,  {1}, {0.6}, {}, fbxfix::matTRS({0, 1, 0})},
        {r.tip,  {1}, {0.4}, {}, fbxfix::matTRS({0, 2, 0})}});
    return r;
}

// ================================================================================================

static void testDetectionAndRefusals() {
    AVER_INFO("=== detection and refusals ===");
    const std::string ascii = Fbx().toString();
    check(fmt::looksLikeFbx(reinterpret_cast<const u8*>(ascii.data()), ascii.size()), "ASCII FBX is recognised");
    std::vector<u8> bin(64, 0);
    const char magic[] = "Kaydara FBX Binary  ";
    std::copy(magic, magic + 21, bin.begin());
    check(fmt::looksLikeFbx(bin.data(), bin.size()), "binary FBX is recognised by its magic");
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    check(!fmt::looksLikeFbx(reinterpret_cast<const u8*>(obj.data()), obj.size()), "an OBJ is not");
    check(fmt::fbxImportAvailable(), "this build links ufbx");

    fmt::FbxImportResult r;
    std::string why;
    check(!fmt::importFbxFromMemory(reinterpret_cast<const u8*>(obj.data()), obj.size(), "", r, {}, &why) && contains(why, "not an FBX"),
          "an OBJ renamed .fbx is refused before ufbx sees it (" + why + ")");
    why.clear();
    check(!fmt::importFbxFromMemory(nullptr, 0, "", r, {}, &why) && !why.empty(), "no bytes: refused with a message");
    why.clear();
    std::vector<u8> junk(4096);
    for (usize i = 0; i < junk.size(); ++i) junk[i] = static_cast<u8>(i * 31 + 7);
    check(!fmt::importFbxFromMemory(junk.data(), junk.size(), "", r, {}, &why) && !why.empty(), "random bytes: refused with a message");

    // A truncated file: no crash, and a refusal carries ufbx's own text.
    Fbx f;
    f.mesh("Tri", 0, fbxfix::triangle());
    const std::string full = f.toString();
    for (const double frac : {0.35, 0.7}) {
        const std::string cut = full.substr(0, static_cast<usize>(full.size() * frac));
        fmt::FbxImportResult tr;
        std::string tw;
        const bool ok = fmt::importFbxFromMemory(reinterpret_cast<const u8*>(cut.data()), cut.size(), "", tr, {}, &tw);
        check(ok || !tw.empty(), "a file cut at " + std::to_string(int(frac * 100)) + "% does not crash and a failure says why (" + tw + ")");
    }
}

static void testFramesAndUnits() {
    AVER_INFO("=== frames and units ===");
    // Y-up metres: file (1,2,3) -> glTF map (x,y,z) -> (-z, x, y) -> (-3, 1, 2) m -> (-300, 100, 200) cm.
    const auto oneVertex = [](Fbx& f) {
        MeshDesc m = fbxfix::triangle();
        m.verts[0] = 1; m.verts[1] = 2; m.verts[2] = 3;
        m.verts[3] = 2; m.verts[4] = 2; m.verts[5] = 3;
        m.verts[6] = 1; m.verts[7] = 3; m.verts[8] = 3;
        f.mesh("T", 0, m);
    };
    {
        Fbx f; oneVertex(f);
        fmt::FbxImportResult r; std::string why;
        check(importText(f, r, {}, &why), "a Y-up metre file imports (" + why + ")");
        check(r.meshes.size() == 1 && r.meshes[0].valid() && r.meshes[0].vertexCount() == 3, "one valid 3-vertex mesh");
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], -300, 100, 200, 0.01f, "Y-up metres (1,2,3) -> engine (-300,100,200)");
    }
    {
        // Z-up centimetres: ufbx turns (x,y,z) into (x,z,-y) metres, so (10,20,30) cm -> (0.1,0.3,-0.2) m -> engine (20,10,30) cm.
        Fbx f; f.zUp().unitScaleFactor(1.0);
        MeshDesc m = fbxfix::triangle();
        m.verts = {10, 20, 30, 11, 20, 30, 10, 21, 30};
        f.mesh("T", 0, m);
        fmt::FbxImportResult r;
        check(importText(f, r), "a Z-up centimetre file imports");
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], 20, 10, 30, 0.01f, "Z-up cm (10,20,30) -> engine (20,10,30)");
    }
    {
        // Inches: UnitScaleFactor 2.54 -> 0.0254 m per unit, so (1,2,3) -> (-3,1,2) * 2.54 cm.
        Fbx f; oneVertex(f); f.unitScaleFactor(2.54);
        fmt::FbxImportResult r;
        check(importText(f, r), "an inch file imports");
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], -7.62f, 2.54f, 5.08f, 0.001f, "UnitScaleFactor 2.54 is honoured");
    }
    {
        // Override: 1 file unit = 0.5 m, so (1,2,3) -> (-3,1,2) * 0.5 m * 100 = (-150, 50, 100).
        Fbx f; oneVertex(f); f.unitScaleFactor(1.0);
        fmt::FbxImportOptions o; o.unitMetersOverride = 0.5f;
        fmt::FbxImportResult r;
        check(importText(f, r, o), "a unit override imports");
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], -150, 50, 100, 0.01f, "unitMetersOverride replaces the file's unit");
    }
    {
        // convertAxes off: the axis map is skipped, (1,2,3) m stays (1,2,3) * 100.
        Fbx f; oneVertex(f);
        fmt::FbxImportOptions o; o.convertAxes = false;
        fmt::FbxImportResult r;
        check(importText(f, r, o), "convertAxes off imports");
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], 100, 200, 300, 0.01f, "convertAxes off: metres times scale, no axis map");
    }
    {
        // scale option: 1 m -> 50 cm.
        Fbx f; oneVertex(f);
        fmt::FbxImportOptions o; o.scale = 50.0f;
        fmt::FbxImportResult r;
        importText(f, r, o);
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], -150, 50, 100, 0.01f, "scale 50 halves the engine size");
    }
    {
        // A node's translation becomes the placement, through the same map: node (1,0,0) m -> engine (0, 100, 0).
        Fbx f;
        f.mesh("Moved", 0, fbxfix::triangle(), {1, 0, 0});
        fmt::FbxImportResult r;
        importText(f, r);
        const fmt::GltfPlacement* p = placementNamed(r, "Moved");
        check(p != nullptr, "the node has a placement");
        if (p) checkVec(&p->position.x, 0, 100, 0, 0.01f, "node translation (1,0,0) m -> placement (0,100,0)");
        if (!r.meshes.empty()) checkVec(&r.meshes[0].positions[0], 0, 0, 0, 0.01f, "and the mesh keeps no translation");
    }
}

static void testWindingAndMirror() {
    AVER_INFO("=== winding and mirrors ===");
    for (const bool mirrored : {false, true}) {
        Fbx f;
        f.mesh("T", 0, fbxfix::triangle(), {0, 0, 0}, {0, 0, 0}, mirrored ? V3{-1, 1, 1} : V3{1, 1, 1});
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), std::string(mirrored ? "a mirrored" : "a plain") + " triangle imports (" + why + ")");
        if (r.meshes.empty()) continue;
        const fmt::OcMeshData& m = r.meshes[0];
        const Vec3 n = triCross(m, 0);
        const Vec3 stored{m.normals[0], m.normals[1], m.normals[2]};
        // The file's +Z normal is engine -X. The triangle must be wound so its own normal agrees: one reversal
        // for the basis change, and a second one cancelling it under the mirror.
        check(dot(n, stored) > 0.0f, std::string(mirrored ? "mirrored: " : "plain: ") + "the winding agrees with the stored normal");
        checkNear(stored.x, -1.0f, 1e-4f, std::string(mirrored ? "mirrored: " : "plain: ") + "the normal is engine -X");
    }
}

static void testMeshes() {
    AVER_INFO("=== meshes ===");
    {
        Fbx f; f.mesh("Quad", 0, fbxfix::quad());
        fmt::FbxImportResult r;
        importText(f, r);
        check(!r.meshes.empty() && r.meshes[0].indices.size() == 6 && r.meshes[0].vertexCount() == 4, "a quad is two triangles over four vertices");
        if (!r.meshes.empty()) checkNear(totalArea(r.meshes[0]), 10000.0f, 1.0f, "and covers 1 m x 1 m = 10000 cm^2");
    }
    {
        // A concave L: (0,0)(2,0)(2,1)(1,1)(1,2)(0,2), 3 m^2.
        MeshDesc m;
        m.verts = {0, 0, 0, 2, 0, 0, 2, 1, 0, 1, 1, 0, 1, 2, 0, 0, 2, 0};
        m.faces = {{0, 1, 2, 3, 4, 5}};
        Fbx f; f.mesh("L", 0, m);
        fmt::FbxImportResult r;
        importText(f, r);
        check(!r.meshes.empty() && r.meshes[0].indices.size() == 12, "a concave hexagon triangulates to four triangles");
        if (!r.meshes.empty()) checkNear(totalArea(r.meshes[0]), 30000.0f, 5.0f, "with the right area (3 m^2)");
    }
    {
        // Two triangles sharing the edge 0-2. Equal normals weld the shared control points; unequal ones split them.
        const auto build = [](bool hard) {
            MeshDesc m;
            m.verts = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
            m.faces = {{0, 1, 2}, {0, 2, 3}};
            m.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1,
                         0, 0, 1, 0, 0, 1, 0, 0, 1};
            if (hard) for (int k = 9; k < 18; k += 3) { m.normals[k] = 0; m.normals[k + 1] = 1; m.normals[k + 2] = 0; }
            return m;
        };
        for (const bool hard : {false, true}) {
            Fbx f; f.mesh("M", 0, build(hard));
            fmt::FbxImportResult r;
            importText(f, r);
            check(!r.meshes.empty() && r.meshes[0].vertexCount() == (hard ? 6u : 4u),
                  hard ? "a hard edge splits the shared control points (6 vertices)" : "a smooth surface welds (4 vertices)");
        }
    }
    {
        // Two control points at one position keep separate vertices (their skin weights may differ).
        MeshDesc m;
        m.verts = {0, 0, 0, 1, 0, 0, 0, 1, 0,   0, 0, 0, 1, 0, 0, 0, 1, 0};
        m.faces = {{0, 1, 2}, {3, 4, 5}};
        m.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1,  0, 0, 1, 0, 0, 1, 0, 0, 1};
        Fbx f; f.mesh("Dup", 0, m);
        fmt::FbxImportResult r;
        importText(f, r);
        check(!r.meshes.empty() && r.meshes[0].vertexCount() == 6, "coincident control points do not merge (6 vertices)");
    }
    {
        // V flips: (0.25, 0.75) -> (0.25, 0.25).
        MeshDesc m = fbxfix::triangle();
        m.uvs = {0.25, 0.75, 0.5, 0.5, 0.75, 0.25};
        Fbx f; f.mesh("UV", 0, m);
        fmt::FbxImportResult r;
        importText(f, r);
        if (!r.meshes.empty()) {
            const fmt::OcMeshData& m0 = r.meshes[0];
            check(std::fabs(m0.uvs[0] - 0.25f) < 1e-5f && std::fabs(m0.uvs[1] - 0.25f) < 1e-5f &&
                  std::fabs(m0.uvs[4] - 0.75f) < 1e-5f && std::fabs(m0.uvs[5] - 0.75f) < 1e-5f, "V is flipped (v -> 1 - v)");
        }
    }
    {
        // No normals in the file: generated by default, a +Z placeholder when the option is off. No UVs: zeros.
        MeshDesc m = fbxfix::triangle();
        m.normals.clear();
        Fbx f; f.mesh("NoNormals", 0, m);
        fmt::FbxImportResult r;
        importText(f, r);
        if (!r.meshes.empty()) {
            const fmt::OcMeshData& m0 = r.meshes[0];
            bool unit = true;
            for (u32 i = 0; i < m0.vertexCount(); ++i) {
                const f32 len = std::sqrt(m0.normals[i*3]*m0.normals[i*3] + m0.normals[i*3+1]*m0.normals[i*3+1] + m0.normals[i*3+2]*m0.normals[i*3+2]);
                unit = unit && std::fabs(len - 1.0f) < 1e-3f;
            }
            check(unit, "missing normals are generated, unit length");
            check(m0.uvs.size() == m0.vertexCount() * 2 && m0.uvs[0] == 0.0f && m0.uvs[1] == 1.0f, "missing UVs are (0,0) before the V flip");
        }
        fmt::FbxImportOptions o; o.generateMissingNormals = false;
        fmt::FbxImportResult r2;
        importText(f, r2, o);
        if (!r2.meshes.empty()) checkVec(&r2.meshes[0].normals[0], 0, 0, 1, 1e-6f, "with generation off the placeholder is +Z");
    }
    {
        // Two materials -> two submeshes that partition the indices; slots are the material names.
        MeshDesc m;
        m.verts = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,  2, 0, 0, 3, 0, 0, 3, 1, 0, 2, 1, 0};
        m.faces = {{0, 1, 2, 3}, {4, 5, 6, 7}};
        m.normals = std::vector<double>(24 * 1, 0.0);
        for (usize i = 0; i < 8; ++i) m.normals[i * 3 + 2] = 1;
        m.faceMaterial = {0, 1};
        Fbx f;
        const int node = f.mesh("Two", 0, m);
        f.assignMaterial(node, f.material("Red", {1, 0, 0}));
        f.assignMaterial(node, f.material("Blue", {0, 0, 1}));
        fmt::FbxImportResult r;
        importText(f, r);
        check(!r.meshes.empty() && r.meshes[0].submeshes.size() == 2 && fmt::submeshesPartitionIndices(r.meshes[0]),
              "two materials -> two submeshes that partition the indices");
        check(r.materials.size() == 2 && r.materials[0].name == "Red" && r.materials[1].name == "Blue", "materials are named after their elements");
        if (!r.meshes.empty() && r.meshes[0].submeshes.size() == 2) {
            const fmt::OcMeshData& m0 = r.meshes[0];
            check(m0.materialSlots[m0.submeshes[0].materialSlot] == "Red" && m0.materialSlots[m0.submeshes[1].materialSlot] == "Blue",
                  "and the slot of each submesh is its material's name");
        }
    }
    {
        // Duplicate material names get _2; the slot string and the material name agree.
        Fbx f;
        MeshDesc m = fbxfix::quad();
        m.faceMaterial = {0};
        const int node = f.mesh("Dups", 0, m);
        const int a = f.material("Mat", {1, 0, 0});
        const int b = f.material("Mat", {0, 1, 0});
        f.assignMaterial(node, a);
        (void)b;
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.materials.size() == 2 && r.materials[0].name == "Mat" && r.materials[1].name == "Mat_2", "duplicate material names are made unique");
        check(!r.meshes.empty() && !r.meshes[0].materialSlots.empty() && r.meshes[0].materialSlots[0] == r.materials[0].name,
              "and the slot equals ImportedMaterial::name");
    }
    {
        // A hidden node is skipped; an empty mesh is skipped without failing the import.
        Fbx f;
        f.mesh("Shown", 0, fbxfix::triangle());
        f.mesh("Hidden", 0, fbxfix::triangle(5, 0, 0), {}, {}, {1, 1, 1}, /*visible=*/false);
        MeshDesc empty;
        f.mesh("Empty", 0, empty);
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "a file with a hidden node and an empty mesh still imports (" + why + ")");
        check(r.meshes.size() == 1, "only the visible, non-empty mesh is imported");
    }
}

static void testInstancing() {
    AVER_INFO("=== instancing ===");
    {
        Fbx f;
        const int a = f.mesh("A", 0, fbxfix::triangle());
        f.instance("B", 0, a, {1, 0, 0});
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.meshes.size() == 1 && r.placements.size() == 2, "two nodes sharing a geometry with one basis: one mesh, two placements");
        const fmt::GltfPlacement* pb = placementNamed(r, "B");
        check(pb && pb->meshIndex == 0, "both place mesh 0");
        if (pb) checkVec(&pb->position.x, 0, 100, 0, 0.01f, "B's translation is its placement");
    }
    {
        Fbx f;
        const int a = f.mesh("A", 0, fbxfix::triangle());
        f.instance("C", 0, a, {0, 0, 0}, {0, 90, 0});
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.meshes.size() == 2 && r.placements.size() == 2, "a rotated instance gets its own baked copy");
        bool unique = r.meshNames.size() == 2 && r.meshNames[0] != r.meshNames[1];
        check(unique, "and the copy has its own name");
    }
    {
        Fbx f;
        MeshDesc m = fbxfix::triangle();
        m.faceMaterial = {0};
        const int a = f.mesh("A", 0, m);
        const int d = f.instance("D", 0, a);
        f.assignMaterial(a, f.material("One", {1, 0, 0}));
        f.assignMaterial(d, f.material("Two", {0, 1, 0}));
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.meshes.size() == 2, "a node-level material override makes a separate copy");
    }
}

static void testSkin() {
    AVER_INFO("=== skin and skeleton ===");
    {
        Fbx f; buildRig(f);
        fmt::FbxImportResult r; std::string why;
        check(importText(f, r, {}, &why), "a skinned mesh imports (" + why + ")");
        check(r.skeletons.size() == 1 && r.skeletons[0].bones.size() == 3 && r.skeletons[0].valid(), "one valid skeleton of three bones");
        if (r.skeletons.empty()) return;
        const fmt::OcSkeleton& sk = r.skeletons[0];
        check(sk.bones[0].name == "Root" && sk.bones[1].name == "Mid" && sk.bones[2].name == "Tip" &&
              sk.bones[0].parent == fmt::kOcBoneNoParent && sk.bones[1].parent == 0 && sk.bones[2].parent == 1, "bone names and hierarchy");
        // Mid is 1 m up its parent in the file: glTF (0,1,0) -> engine (0,0,100).
        checkVec(&sk.bones[1].translation.x, 0, 0, 100, 0.01f, "a bone's local rest goes through the engine map");
        // The invariant: inverseBind * model == identity for every bone, to 1e-4.
        std::vector<Mat4> model(sk.bones.size());
        bool identity = true;
        for (usize i = 0; i < sk.bones.size(); ++i) {
            Transform x; x.position = sk.bones[i].translation; x.rotation = sk.bones[i].rotation; x.scale = sk.bones[i].scale;
            const Mat4 local = x.toMatrix();
            model[i] = sk.bones[i].parent >= 0 ? local * model[usize(sk.bones[i].parent)] : local;
            Mat4 inv;
            for (int rr = 0; rr < 4; ++rr) for (int cc = 0; cc < 4; ++cc) inv.m[rr][cc] = sk.bones[i].inverseBind[rr * 4 + cc];
            const Mat4 prod = inv * model[i];
            for (int rr = 0; rr < 4; ++rr)
                for (int cc = 0; cc < 4; ++cc)
                    if (std::fabs(prod.m[rr][cc] - (rr == cc ? 1.0f : 0.0f)) > 1e-3f) identity = false;
        }
        check(identity, "inverseBind * model rest == identity for every bone");
        check(r.meshes.size() == 1 && r.meshes[0].hasSkin() && r.meshSkinIndex.size() == 1 && r.meshSkinIndex[0] == 0, "the mesh is skinned to skeleton 0");
        if (r.meshes.empty() || !r.meshes[0].hasSkin()) return;
        const fmt::OcMeshData& m = r.meshes[0];
        check(m.joints[0] == 0 && std::fabs(m.weights[0] - 1.0f) < 1e-5f, "vertex 0: Root, weight 1");
        check(m.joints[4] == 1 && m.joints[5] == 2 && std::fabs(m.weights[4] - 0.6f) < 1e-4f && std::fabs(m.weights[5] - 0.4f) < 1e-4f,
              "vertex 1: Mid 0.6 and Tip 0.4");
        check(m.joints[8] == 0 && std::fabs(m.weights[8] - 1.0f) < 1e-5f && m.weights[9] == 0.0f, "vertex 2 had no weight: it binds to bone 0");
        check(r.placements.size() == 1 && r.placements[0].position.size() < 1e-3f, "a skinned mesh keeps its translation in the geometry, not a placement");
    }
    {
        // The bind pose wins over the scene's frame-0 pose: Mid sits at (0,1.5,0) in the scene but binds at (0,1,0).
        Fbx f; buildRig(f, 0, {0, 1.5, 0});
        fmt::FbxImportResult r;
        importText(f, r);
        if (!r.skeletons.empty()) checkVec(&r.skeletons[0].bones[1].translation.x, 0, 0, 100, 0.01f, "the skeleton follows the bind pose, not frame 0");
    }
    {
        // Five influences: the top four, renormalised; the truncation is counted and reported.
        Fbx f;
        const int root = f.bone("Root", 0), b1 = f.bone("B1", root), b2 = f.bone("B2", root), b3 = f.bone("B3", root), b4 = f.bone("B4", root);
        const int node = f.mesh("Five", 0, fbxfix::triangle());
        std::vector<fbxfix::ClusterDesc> cl;
        const int bones[5] = {root, b1, b2, b3, b4};
        const double w[5] = {0.40, 0.25, 0.15, 0.12, 0.08};
        for (int k = 0; k < 5; ++k) cl.push_back({bones[k], {0}, {w[k]}, {}, {}});
        f.skin(f.geometryOf(node), cl);
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.stats.truncatedInfluenceVertices == 1, "the vertex with five influences is counted");
        checkNear(r.stats.maxDroppedWeight, 0.08f, 1e-4f, "and the largest dropped weight is recorded");
        if (!r.meshes.empty() && r.meshes[0].hasSkin()) {
            const fmt::OcMeshData& m = r.meshes[0];
            f32 sum = 0; for (int k = 0; k < 4; ++k) sum += m.weights[k];
            checkNear(sum, 1.0f, 1e-4f, "the kept four sum to one");
            checkNear(m.weights[0], 0.40f / 0.92f, 1e-3f, "the largest is renormalised (0.40 / 0.92)");
            check(m.joints[0] == 0 && m.joints[1] == 1 && m.joints[2] == 2 && m.joints[3] == 3, "in weight order, B4 dropped");
        }
    }
    {
        // A non-bone node between two bones joins the chain; sockets follow the glTF four-rule port.
        Fbx f;
        const int root = f.bone("Root", 0);
        const int spacer = f.node("Spacer", root, {0, 0.5, 0});
        const int mid = f.bone("Mid", spacer, {0, 0.5, 0});
        const int muzzle1 = f.node("Muzzle", mid, {0.1, 0, 0});
        const int muzzle2 = f.node("Muzzle", mid, {0.2, 0, 0});
        const int handMesh = f.mesh("Held", mid, fbxfix::triangle());
        const int node = f.mesh("Skin", 0, fbxfix::triangle());
        (void)muzzle1; (void)muzzle2; (void)handMesh;
        f.skin(f.geometryOf(node), {{root, {0}, {1.0}, {}, {}}, {mid, {1, 2}, {1.0, 1.0}, {}, fbxfix::matTRS({0, 1, 0})}});
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "a rig with a spacer and locators imports (" + why + ")");
        if (r.skeletons.empty()) return;
        const fmt::OcSkeleton& sk = r.skeletons[0];
        bool spacerIsBone = false;
        for (const fmt::OcBone& b : sk.bones) if (b.name == "Spacer") spacerIsBone = true;
        check(sk.bones.size() == 3 && spacerIsBone, "the helper node between two bones is a chain bone");
        check(sk.sockets.size() == 2 && sk.sockets[0].name == "Muzzle" && sk.sockets[1].name == "Muzzle2", "locators under a bone are sockets, the duplicate renamed");
        check(sk.sockets[0].bone == 2, "bound to the bone they hang from");
        bool heldIsSocket = false;
        for (const fmt::OcSocket& s : sk.sockets) if (s.name == "Held") heldIsSocket = true;
        check(!heldIsSocket, "a mesh child of a bone is not a socket");
        check(r.stats.sockets == 2, "Stats counts the sockets");
    }
    {
        // Two disjoint rigs -> two skeletons, and meshSkinIndex tells their meshes apart.
        Fbx f;
        const int r1 = f.bone("RootA", 0), r2 = f.bone("RootB", 0);
        const int m1 = f.mesh("MeshA", 0, fbxfix::triangle());
        const int m2 = f.mesh("MeshB", 0, fbxfix::triangle(5, 0, 0));
        f.skin(f.geometryOf(m1), {{r1, {0, 1, 2}, {1, 1, 1}, {}, {}}});
        f.skin(f.geometryOf(m2), {{r2, {0, 1, 2}, {1, 1, 1}, {}, {}}});
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.skeletons.size() == 2 && r.meshes.size() == 2 && r.meshSkinIndex.size() == 2 && r.meshSkinIndex[0] != r.meshSkinIndex[1],
              "two rigs: two skeletons and distinct meshSkinIndex");
    }
    {
        // More than 256 bones is reported, not enforced.
        Fbx f;
        int parent = 0, first = 0;
        for (int i = 0; i < 257; ++i) { parent = f.bone("B" + std::to_string(i), parent); if (i == 0) first = parent; }
        const int node = f.mesh("Long", 0, fbxfix::triangle());
        f.skin(f.geometryOf(node), {{first, {0, 1, 2}, {1, 1, 1}, {}, {}}});
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "a 257-bone chain imports (" + why + ")");
        check(!r.skeletons.empty() && r.skeletons[0].bones.size() == 257, "all 257 bones are kept");
        check(anyContains(r.unsupported, "257"), "and the count over 256 is reported");
    }
    {
        // An armature node carrying a rotation above the topmost bone folds into the rest pose.
        Fbx f;
        const int arm = f.node("Armature", 0, {}, {90, 0, 0});
        const int root = f.bone("Root", arm);
        const int node = f.mesh("Skin", 0, fbxfix::triangle());
        f.skin(f.geometryOf(node), {{root, {0, 1, 2}, {1, 1, 1}, {}, fbxfix::matTRS({0, 0, 0}, {90, 0, 0})}});
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "a rig under a rotated armature imports (" + why + ")");
        if (!r.skeletons.empty()) {
            const fmt::OcBone& b = r.skeletons[0].bones[0];
            check(std::fabs(b.rotation.w) < 0.999f, "the armature's quarter turn is in the root bone's rest (it is its model-space transform)");
        }
    }
    {
        // A file with bones and no skinned mesh -- an animation-only export -- still yields its rig.
        Fbx f;
        const int root = f.bone("Root", 0);
        f.bone("Child", root, {0, 1, 0});
        f.node("Anchor", 0);
        fmt::FbxImportResult r; std::string why;
        check(importText(f, r, {}, &why), "bones with no mesh import (" + why + ")");
        check(r.skeletons.size() == 1 && r.skeletons[0].bones.size() == 2, "as a skeleton");
    }
}

static void testAnimation() {
    AVER_INFO("=== animation ===");
    {
        // Mid slides along file X from 0 to 1 m over one second. Local (x,1,0) -> engine (0, 100x, 100).
        Fbx f; const Rig rig = buildRig(f);
        f.animStack("Take 001", 1.0, {curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}, {0, 1, 0})});
        fmt::FbxImportResult r; std::string why;
        check(importText(f, r, {}, &why), "an animated rig imports (" + why + ")");
        check(r.animations.size() == 1 && r.animationNames.size() == 1 && r.animationNames[0] == "Take 001", "one clip named after the stack");
        check(r.animationSkeleton.size() == 1 && r.animationSkeleton[0] == 0, "and bound to skeleton 0");
        if (r.animations.empty()) return;
        const fmt::OcAnimation& c = r.animations[0];
        check(c.valid() && std::fabs(c.duration - 1.0f) < 1e-3f, "valid, one second long");
        const fmt::OcTrack* t = findTrack(c, 1, fmt::kOcChannelTranslation);
        check(t != nullptr, "Mid has a translation track");
        if (t) {
            const std::vector<f32> a = sampleLinear(*t, 0.0f), b = sampleLinear(*t, 1.0f), mid = sampleLinear(*t, 0.5f);
            checkVec(a.data(), 0, 0, 100, 0.01f, "t = 0: the rest value");
            checkVec(b.data(), 0, 100, 100, 0.01f, "t = 1: file X = 1 m is engine Y = 100");
            checkVec(mid.data(), 0, 50, 100, 0.5f, "halfway between");
        }
        check(findTrack(c, 0, fmt::kOcChannelTranslation) == nullptr && findTrack(c, 0, fmt::kOcChannelRotation) == nullptr,
              "Root, which the stack does not touch, emits nothing");
        check(!(c.flags & fmt::kOcAnimRootMotion), "the root did not travel: not flagged (Auto)");
    }
    {
        // A channel that is constant and equal to rest emits nothing.
        Fbx f; const Rig rig = buildRig(f);
        f.animStack("Hold", 1.0, {curve(rig.root, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 0.0}}, {0, 0, 0}),
                                  curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}, {0, 1, 0})});
        fmt::FbxImportResult r;
        importText(f, r);
        check(!r.animations.empty() && findTrack(r.animations[0], 0, fmt::kOcChannelTranslation) == nullptr,
              "a constant channel equal to its rest value emits no track");
    }
    {
        // A stepped curve stays a hold: 0 until t = 1, then 1 m.
        Fbx f; const Rig rig = buildRig(f);
        f.animStack("Step", 1.0, {curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}, {0, 1, 0}, /*step=*/true)});
        fmt::FbxImportResult r;
        importText(f, r);
        const fmt::OcTrack* t = r.animations.empty() ? nullptr : findTrack(r.animations[0], 1, fmt::kOcChannelTranslation);
        check(t != nullptr, "the stepped curve has a track");
        if (t) {
            checkVec(sampleLinear(*t, 0.5f).data(), 0, 0, 100, 0.5f, "it holds the first value halfway");
            checkVec(sampleLinear(*t, 1.0f).data(), 0, 100, 100, 0.5f, "and reaches the second at the end");
        }
    }
    {
        // A rotation through more than half a turn: successive quaternions stay in one hemisphere.
        Fbx f; const Rig rig = buildRig(f);
        f.animStack("Spin", 1.0, {curve(rig.mid, "Lcl Rotation", 'Z', {{0.0, 0.0}, {1.0, 270.0}}, {0, 0, 0})});
        fmt::FbxImportResult r;
        importText(f, r);
        const fmt::OcTrack* t = r.animations.empty() ? nullptr : findTrack(r.animations[0], 1, fmt::kOcChannelRotation);
        check(t != nullptr && t->times.size() > 2, "a 270 degree turn is resampled into several keys");
        if (t) {
            bool shortArc = true;
            for (usize k = 1; k < t->times.size(); ++k) {
                f32 d = 0;
                for (int c = 0; c < 4; ++c) d += t->values[k * 4 + c] * t->values[(k - 1) * 4 + c];
                if (d < 0) shortArc = false;
            }
            check(shortArc, "no key flips hemisphere against its neighbour");
        }
    }
    {
        // A topmost bone under a rotated armature: its keys are folded through the armature's world.
        // Local (0,y,0) under Rx(90) is world (0,0,y); file (0,0,1) m -> engine (-100, 0, 0).
        Fbx f;
        const int arm = f.node("Armature", 0, {}, {90, 0, 0});
        const int root = f.bone("Root", arm);
        const int node = f.mesh("Skin", 0, fbxfix::triangle());
        f.skin(f.geometryOf(node), {{root, {0, 1, 2}, {1, 1, 1}, {}, fbxfix::matTRS({0, 0, 0}, {90, 0, 0})}});
        f.animStack("Fold", 1.0, {curve(root, "Lcl Translation", 'Y', {{0.0, 0.0}, {1.0, 1.0}}, {0, 0, 0})});
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "an animated rig under a rotated armature imports (" + why + ")");
        const fmt::OcTrack* t = r.animations.empty() ? nullptr : findTrack(r.animations[0], 0, fmt::kOcChannelTranslation);
        check(t != nullptr, "the root has a translation track");
        if (t) {
            checkVec(sampleLinear(*t, 0.0f).data(), 0, 0, 0, 0.01f, "folded: t = 0");
            checkVec(sampleLinear(*t, 1.0f).data(), -100, 0, 0, 0.05f, "folded: the armature's rotation is in the key");
        }
    }
    {
        // A stack with no duration is skipped; two stacks make two clips.
        Fbx f; const Rig rig = buildRig(f);
        f.animStack("Empty", 0.0, {});
        f.animStack("A", 1.0, {curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}, {0, 1, 0})});
        f.animStack("B", 2.0, {curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {2.0, 1.0}}, {0, 1, 0})});
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.animations.size() == 2 && r.animationNames.size() == 2, "the empty stack is skipped, two clips remain");
        bool a = false, b = false;
        for (const std::string& n : r.animationNames) { a = a || n == "A"; b = b || n == "B"; }
        check(a && b, "named A and B");
    }
    {
        // Root motion: the root bone travels 200 cm in the plane (file Z = 2 m -> engine X = -200), or 2 cm.
        const auto build = [](double meters) {
            Fbx f; const Rig rig = buildRig(f);
            f.animStack("Walk", 1.0, {curve(rig.root, "Lcl Translation", 'Z', {{0.0, 0.0}, {1.0, meters}}, {0, 0, 0})});
            return f;
        };
        for (const auto mode : {fmt::FbxImportOptions::RootMotion::Off, fmt::FbxImportOptions::RootMotion::Auto, fmt::FbxImportOptions::RootMotion::All}) {
            fmt::FbxImportOptions o; o.rootMotion = mode;
            fmt::FbxImportResult farRes, nearRes;
            importText(build(2.0), farRes, o);
            importText(build(0.02), nearRes, o);
            const bool farFlag = !farRes.animations.empty() && (farRes.animations[0].flags & fmt::kOcAnimRootMotion);
            const bool nearFlag = !nearRes.animations.empty() && (nearRes.animations[0].flags & fmt::kOcAnimRootMotion);
            if (mode == fmt::FbxImportOptions::RootMotion::Off)
                check(!farFlag && !nearFlag, "Off never sets the flag");
            else if (mode == fmt::FbxImportOptions::RootMotion::Auto)
                check(farFlag && !nearFlag, "Auto flags 200 cm of travel and not 2 cm");
            else
                check(farFlag && nearFlag, "All flags every clip with a root track");
        }
    }
    {
        // Two skeletons animated by one stack: two clips with distinct animationSkeleton.
        Fbx f;
        const int r1 = f.bone("RootA", 0), r2 = f.bone("RootB", 0);
        const int m1 = f.mesh("MeshA", 0, fbxfix::triangle());
        const int m2 = f.mesh("MeshB", 0, fbxfix::triangle(5, 0, 0));
        f.skin(f.geometryOf(m1), {{r1, {0, 1, 2}, {1, 1, 1}, {}, {}}});
        f.skin(f.geometryOf(m2), {{r2, {0, 1, 2}, {1, 1, 1}, {}, {}}});
        f.animStack("Take 001", 1.0, {curve(r1, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}), curve(r2, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}})});
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.animations.size() == 2 && r.animationSkeleton.size() == 2 && r.animationSkeleton[0] != r.animationSkeleton[1],
              "a stack animating two skeletons yields two clips with distinct animationSkeleton");
        check(r.animationNames.size() == 2 && r.animationNames[0] != r.animationNames[1], "with distinct names");
    }
    {
        // An object clip: a mesh node turning 90 degrees about file Y. A(t) = W(t) * Q0^-1 = the rotation itself.
        // glTF quaternion (0, sin45, 0, cos45) -> engine {z, -x, -y, w} = (0, 0, -0.7071, 0.7071).
        Fbx f;
        const int spin = f.mesh("Spinner", 0, fbxfix::triangle());
        f.animStack("Take 001", 1.0, {curve(spin, "Lcl Rotation", 'Y', {{0.0, 0.0}, {1.0, 90.0}}, {0, 0, 0})});
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "a spinning mesh imports (" + why + ")");
        check(r.objectAnimations.size() == 1 && r.objectAnimationNames.size() == 1 && r.objectAnimationPlacement.size() == 1,
              "one object clip");
        if (r.objectAnimations.empty()) return;
        const fmt::OcAnimation& c = r.objectAnimations[0];
        check(c.valid() && (c.flags & fmt::kOcAnimObject) && (c.flags & fmt::kOcAnimLoop) && c.tracks.size() == 1 && c.tracks[0].boneIndex == 0,
              "flagged object and loop, one track on bone 0");
        check(r.objectAnimationNames[0] == "Spinner" && r.objectAnimationPlacement[0] == 0, "named after the node, attached to its placement");
        const fmt::OcTrack& t = c.tracks[0];
        check((t.channels & fmt::kOcChannelRotation) != 0, "it holds a rotation");
        usize stride = 0;
        if (t.channels & fmt::kOcChannelTranslation) stride += 3;
        const usize rotAt = stride;
        stride += 4;
        if (t.channels & fmt::kOcChannelScale) stride += 3;
        const usize last = t.times.size() - 1;
        const f32* q = &t.values[last * stride + rotAt];
        const f32 d = std::fabs(q[2] * -0.70710678f + q[3] * 0.70710678f);
        check(std::fabs(q[0]) < 1e-3f && std::fabs(q[1]) < 1e-3f && d > 0.9999f, "the last key is a 90 degree turn, in engine axes");
    }
}

// Writes the bytes of a file and returns its path.
static std::filesystem::path writeFile(const std::filesystem::path& p, const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p;
}

static void testMaterials() {
    AVER_INFO("=== materials and textures ===");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "aver_fbx_material_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    {
        // Phong: roughness from the exponent, sqrt(2/(Ns+2)); metallic 0; the diffuse colour.
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        f.assignMaterial(node, f.material("Phong", {0.2, 0.4, 0.6}, "phong", 20.0));
        fmt::FbxImportResult r;
        importText(f, r);
        check(r.materials.size() == 1, "one material");
        if (!r.materials.empty()) {
            const fmt::ImportedMaterial& m = r.materials[0];
            checkNear(m.roughnessFactor, std::sqrt(2.0f / 22.0f), 1e-3f, "Phong exponent 20 -> roughness sqrt(2/22)");
            checkNear(m.metallicFactor, 0.0f, 1e-6f, "metallic 0, not glTF's 1");
            checkNear(m.baseColorFactor[0], 0.2f, 1e-3f, "diffuse red");
            checkNear(m.baseColorFactor[2], 0.6f, 1e-3f, "diffuse blue");
            check(m.alphaMode == "OPAQUE" && !m.baseColorFactorLinear, "opaque, factors sRGB by default");
        }
        fmt::FbxImportOptions o; o.fileColoursAreLinear = true;
        fmt::FbxImportResult rl;
        importText(f, rl, o);
        check(!rl.materials.empty() && rl.materials[0].baseColorFactorLinear, "fileColoursAreLinear is carried");
    }
    {
        // Lambert: no specular exponent -> fully rough, still metallic 0. TransparencyFactor 0.4 -> opacity 0.6 -> BLEND.
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        f.assignMaterial(node, f.material("Glass", {1, 1, 1}, "lambert", -1, 0.4));
        fmt::FbxImportResult r;
        importText(f, r);
        if (!r.materials.empty()) {
            const fmt::ImportedMaterial& m = r.materials[0];
            checkNear(m.roughnessFactor, 1.0f, 1e-6f, "Lambert: roughness 1");
            check(m.alphaMode == "BLEND", "opacity below 1 is BLEND");
            checkNear(m.baseColorFactor[3], 0.6f, 1e-3f, "with alpha 0.6");
        }
    }
    {
        // An external texture next to the model, via the absolute path the file recorded; the bytes are verbatim.
        const std::vector<unsigned char> bytes = {'P', 'N', 'G', 1, 2, 3, 4};
        const fs::path tex = writeFile(root / "Tex Diff.png", bytes);
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        const int mat = f.material("Textured", {0.8, 0.8, 0.8});
        f.assignMaterial(node, mat);
        f.texture(mat, "DiffuseColor", "Diff", tex.string(), "Tex Diff.png");
        fmt::FbxImportResult r;
        importText(f, r, {}, nullptr, root.string());
        check(r.images.size() == 1 && r.images[0].ok && r.images[0].bytes == bytes, "the external file's bytes are taken verbatim");
        check(!r.images.empty() && r.images[0].suggestedName == "Tex_Diff" && r.images[0].ext == ".png", "the stem is sanitised, the extension kept");
        check(!r.materials.empty() && !r.materials[0].baseColorTex.empty() && r.materials[0].baseColorTex.imageIndex == 0, "bound to the base colour");
        checkNear(r.materials.empty() ? 0.0f : r.materials[0].baseColorFactor[0], 1.0f, 1e-3f, "a Phong diffuse colour under a texture is not a tint");
    }
    {
        // The <name>.fbm folder Autodesk tools extract to. Needs a path import so the model's stem is known.
        const fs::path dir = root / "fbm";
        const std::vector<unsigned char> bytes = {'J', 'P', 'G', 9, 8, 7};
        writeFile(dir / "hero.fbm" / "skin.jpg", bytes);
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        const int mat = f.material("Skin", {1, 1, 1});
        f.assignMaterial(node, mat);
        f.texture(mat, "DiffuseColor", "Skin", "C:/nowhere/skin.jpg", "skin.jpg");
        const std::string t = f.toString();
        writeFile(dir / "hero.fbx", std::vector<unsigned char>(t.begin(), t.end()));
        fmt::FbxImportResult r;
        std::string why;
        check(fmt::importFbx((dir / "hero.fbx").string(), r, {}, &why), "importFbx from a path (" + why + ")");
        check(r.images.size() == 1 && r.images[0].bytes == bytes, "the <name>.fbm folder is searched");
    }
    {
        // A missing texture leaves the slot unset and is reported by name; a path that climbs out is refused.
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        const int mat = f.material("Missing", {1, 1, 1});
        f.assignMaterial(node, mat);
        f.texture(mat, "DiffuseColor", "Gone", "C:/nowhere/gone.png", "gone.png");
        const int mat2 = f.material("Escape", {1, 1, 1});
        f.assignMaterial(node, mat2);
        f.texture(mat2, "DiffuseColor", "Esc", "", "../../../../etc/secret.png");
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why, (root / "deep").string()), "a model whose textures are missing still imports (" + why + ")");
        check(r.images.empty(), "no image entry for a file that is not there");
        bool unset = r.materials.size() == 2;
        for (const fmt::ImportedMaterial& m : r.materials) unset = unset && m.baseColorTex.empty();
        check(unset, "the slots stay unset: no dangling reference");
        check(anyContains(r.unsupported, "gone.png"), "the missing file is named");
        check(anyContains(r.unsupported, "refused"), "and the escaping path is refused");
    }
    {
        // Embedded bytes are taken verbatim, with the stem from the recorded file name.
        const std::vector<unsigned char> bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        const int mat = f.material("Emb", {1, 1, 1});
        f.assignMaterial(node, mat);
        f.texture(mat, "DiffuseColor", "Emb", "C:/art/My Embedded.png", "My Embedded.png", bytes);
        fmt::FbxImportResult r;
        std::string why;
        check(importText(f, r, {}, &why), "an embedded texture imports (" + why + ")");
        check(r.images.size() == 1 && r.images[0].bytes == bytes, "embedded bytes verbatim");
        check(!r.images.empty() && r.images[0].suggestedName == "My_Embedded" && r.images[0].ext == ".png", "named from the recorded file name");
    }
    {
        // A normal map connected as NormalMap.
        const std::vector<unsigned char> bytes = {'N', 'R', 'M', 1};
        const fs::path tex = writeFile(root / "nrm.png", bytes);
        Fbx f;
        const int node = f.mesh("M", 0, fbxfix::quad());
        const int mat = f.material("Normal", {1, 1, 1});
        f.assignMaterial(node, mat);
        f.texture(mat, "NormalMap", "Nrm", tex.string(), "nrm.png");
        fmt::FbxImportResult r;
        importText(f, r, {}, nullptr, root.string());
        check(!r.materials.empty() && !r.materials[0].normalTex.empty(), "NormalMap fills the normal slot");
    }
    fs::remove_all(root, ec);
}

static void testCamerasAndLights() {
    AVER_INFO("=== cameras and lights ===");
    Fbx f;
    f.mesh("Tri", 0, fbxfix::triangle());
    f.camera("Cam", 0, {1, 2, 3});
    f.light("Lamp", 0);
    fmt::FbxImportResult r;
    std::string why;
    check(importText(f, r, {}, &why), "a file with a camera and a light imports (" + why + ")");
    check(r.cameras.size() == 1, "one camera");
    if (!r.cameras.empty()) {
        const fmt::ImportedCamera& c = r.cameras[0];
        check(c.name == "Cam", "named");
        checkVec(&c.position.x, -300, 100, 200, 0.01f, "position through the engine map");
        // An FBX camera looks down its node's +X; file +X is engine +Y, so yaw 90 and level.
        checkNear(c.yawDeg, 90.0f, 0.01f, "yaw: file +X is engine +Y");
        checkNear(c.pitchDeg, 0.0f, 0.01f, "and level");
        check(c.fovDeg > 1.0f && c.fovDeg < 179.0f, "a sane field of view");
    }
    check(anyContains(r.unsupported, "lights"), "lights are reported, not imported");
}

static void testBlendShapes() {
    AVER_INFO("=== blend shapes ===");
    Fbx f;
    const int node = f.mesh("Face", 0, fbxfix::triangle());
    const int geom = f.geometryOf(node);
    fbxfix::ShapeDesc half; half.name = "SmileHalf"; half.fullWeight = 50; half.indexes = {0}; half.verts = {0.5, 0, 0};
    fbxfix::ShapeDesc full; full.name = "Smile"; full.fullWeight = 100; full.indexes = {0, 1}; full.verts = {1, 0, 0, 0, 0, 0};
    full.normals = {0, 1, 0, 0, 0, 0};
    const int channel = f.blendChannel(geom, "Smile", {half, full});
    f.animStack("Take 001", 1.0, {curve(channel, "DeformPercent", 'X', {{0.0, 0.0}, {1.0, 100.0}})});

    // The importer today reports them; the extractor is the seam the morph feature uses, tested directly.
    fmt::FbxImportResult r;
    std::string why;
    check(importText(f, r, {}, &why), "a mesh with blend shapes imports (" + why + ")");
    check(anyContains(r.unsupported, "blend shapes"), "and the shapes are reported until the morph feature lands");
    check(r.blendShapes.size() == r.meshes.size(), "blendShapes is parallel to meshes");

    const std::string text = f.toString();
    ufbx_load_opts lo = {};
    lo.target_axes = ufbx_axes_right_handed_y_up;
    lo.target_unit_meters = 1;
    ufbx_error err = {};
    ufbx_scene* scene = ufbx_load_memory(text.data(), text.size(), &lo, &err);
    check(scene != nullptr, "ufbx loads the blend-shape fixture");
    if (!scene) return;
    const ufbx_mesh* mesh = scene->meshes.count ? scene->meshes.data[0] : nullptr;
    check(mesh && mesh->blend_deformers.count == 1, "with one blend deformer");
    if (mesh && mesh->blend_deformers.count == 1) {
        fmt::fbx::Basis basis; basis.convertAxes = true; basis.scale = 100.0f;
        fmt::fbx::Basis3 identity;
        // Output vertex 0 and 1 are both control point 0 (a split), 2 is control point 1, 3 is control point 2.
        const std::vector<u32> cp = {0, 0, 1, 2};
        u32 inBetween = 0;
        const std::vector<fmt::FbxBlendShape> shapes = fmt::fbx::extractFbxBlendShapes(basis, mesh, cp, identity, &inBetween);
        check(shapes.size() == 1 && shapes[0].name == "Smile", "one channel, named");
        check(inBetween == 1, "the in-between shape is dropped and counted");
        if (shapes.size() == 1) {
            const fmt::FbxBlendShape& s = shapes[0];
            // control point 0 moved by file +X 1 m -> engine +Y 100 cm, on both vertices it split into; point 1 did not move.
            check(s.vertices.size() == 2 && s.vertices[0] == 0 && s.vertices[1] == 1, "the delta applies to every vertex the control point split into");
            if (s.positionDeltas.size() == 6) {
                checkVec(&s.positionDeltas[0], 0, 100, 0, 0.01f, "a +X delta in a Y-up file lands on engine +Y, scaled by 100");
            }
            // normal delta (0,1,0) -> direction (-0,0,1): unscaled
            if (s.normalDeltas.size() == 6) checkVec(&s.normalDeltas[0], 0, 0, 1, 1e-4f, "the normal delta takes the direction map, no scale");
        }
    }
    ufbx_free_scene(scene);
}

static void testOptionsText() {
    AVER_INFO("=== options text ===");
    fmt::FbxImportOptions o;
    o.scale = 2.5f; o.unitMetersOverride = 0.01f; o.convertAxes = false; o.generateMissingNormals = false;
    o.importMeshes = false; o.importSkeletons = false; o.importAnimations = false; o.importMaterials = false; o.importBlendShapes = false;
    o.fileColoursAreLinear = true; o.bakeFps = 60; o.reduceKeys = false;
    o.rootMotion = fmt::FbxImportOptions::RootMotion::All; o.rootMotionMinTravelCm = 3.5f; o.maxMemoryMiB = 512.0f;
    const std::string text = fmt::fbxOptionsToText(o);
    fmt::FbxImportOptions back;
    std::string why;
    check(fmt::fbxOptionsFromText(text, back, &why), "the options text parses back (" + why + ")");
    check(back.scale == o.scale && back.unitMetersOverride == o.unitMetersOverride && back.convertAxes == o.convertAxes &&
          back.generateMissingNormals == o.generateMissingNormals && back.importMeshes == o.importMeshes && back.importSkeletons == o.importSkeletons &&
          back.importAnimations == o.importAnimations && back.importMaterials == o.importMaterials && back.importBlendShapes == o.importBlendShapes &&
          back.fileColoursAreLinear == o.fileColoursAreLinear && back.bakeFps == o.bakeFps && back.reduceKeys == o.reduceKeys &&
          back.rootMotion == o.rootMotion && back.rootMotionMinTravelCm == o.rootMotionMinTravelCm && back.maxMemoryMiB == o.maxMemoryMiB,
          "every field round trips: " + text);
    check(fmt::fbxOptionsToText(fmt::FbxImportOptions{}) == "scale=100;unitMeters=0;convertAxes=1;generateNormals=1;meshes=1;skeletons=1;"
          "animations=1;materials=1;blendShapes=1;linearColours=0;bakeFps=30;reduceKeys=1;rootMotion=auto;rootMotionMinCm=10;maxMemoryMiB=2048",
          "the default text is stable");
    fmt::FbxImportOptions partial;
    check(fmt::fbxOptionsFromText("rootMotion=off;scale=10", partial, &why) && partial.rootMotion == fmt::FbxImportOptions::RootMotion::Off &&
          partial.scale == 10.0f && partial.bakeFps == 30, "a partial text changes only the keys it names");
    fmt::FbxImportOptions bad;
    why.clear();
    check(!fmt::fbxOptionsFromText("scale=100;bogus=1", bad, &why) && contains(why, "bogus"), "an unknown key is an error naming it (" + why + ")");
    why.clear();
    check(!fmt::fbxOptionsFromText("scale=abc", bad, &why) && contains(why, "scale"), "a bad value names its key (" + why + ")");
    check(!fmt::fbxOptionsFromText("rootMotion=sideways", bad, &why), "a bad root motion mode is refused");
}

static void testDeterminism() {
    AVER_INFO("=== determinism ===");
    Fbx f; const Rig rig = buildRig(f);
    f.animStack("Take 001", 1.0, {curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}, {0, 1, 0})});
    fmt::FbxImportResult a, b;
    importText(f, a);
    importText(f, b);
    bool same = a.meshes.size() == b.meshes.size() && a.animations.size() == b.animations.size() && !a.meshes.empty() && !a.animations.empty();
    for (usize i = 0; same && i < a.meshes.size(); ++i) {
        std::vector<u8> x, y;
        same = fmt::writeOcMesh(a.meshes[i], x, nullptr) && fmt::writeOcMesh(b.meshes[i], y, nullptr) && x == y;
    }
    for (usize i = 0; same && i < a.animations.size(); ++i) {
        std::vector<u8> x, y;
        same = fmt::writeOcAnim(a.animations[i], x, nullptr) && fmt::writeOcAnim(b.animations[i], y, nullptr) && x == y;
    }
    check(same, "importing the same bytes twice gives identical .ocmesh and .ocanim bytes");
    check(a.report == b.report, "and an identical report");
    for (const fmt::OcAnimation& c : a.animations) check(c.valid(), "every produced clip is valid");
}

// Runs a command, capturing stdout and stderr; the exit code, or -1.
static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
#if defined(_WIN32)
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = cmd + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static void testAssetC() {
    AVER_INFO("=== AverAssetC convert fixture.fbx ===");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exe(AVER_ASSETC_EXE);
    check(fs::exists(exe, ec), "AverAssetC was built: " + exe.string());
    if (!fs::exists(exe, ec)) return;
    const fs::path dir = fs::temp_directory_path(ec) / "aver_fbx_assetc_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "out", ec);

    Fbx f; const Rig rig = buildRig(f);
    f.animStack("Take 001", 1.0, {curve(rig.mid, "Lcl Translation", 'X', {{0.0, 0.0}, {1.0, 1.0}}, {0, 1, 0})});
    const std::string text = f.toString();
    const fs::path input = writeFile(dir / "fixture.fbx", std::vector<unsigned char>(text.begin(), text.end()));

    std::string out;
    const int code = runCapture("\"" + exe.string() + "\" convert \"" + input.string() + "\" --out-dir \"" + (dir / "out").string() + "\"", out);
    check(code == 0, "it exits 0 (got " + std::to_string(code) + ")");
    bool mesh = false, skel = false, anim = false, summary = false;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() != '{') continue;
        const bool verified = contains(line, "\"verified\":true");
        if (contains(line, "\"kind\":\"mesh\"") && verified) mesh = true;
        if (contains(line, "\"kind\":\"skeleton\"") && verified) skel = true;
        if (contains(line, "\"kind\":\"animation\"") && verified) anim = true;
        if (contains(line, "\"summary\":true")) summary = true;
    }
    check(mesh, "a verified mesh artifact line");
    check(skel, "a verified skeleton artifact line");
    check(anim, "a verified animation artifact line");
    check(summary, "and the summary line closes the run");
    fs::remove_all(dir, ec);
}

int main() {
    AVER_INFO("FbxImportTest");
    testDetectionAndRefusals();
    testFramesAndUnits();
    testWindingAndMirror();
    testMeshes();
    testInstancing();
    testSkin();
    testAnimation();
    testMaterials();
    testCamerasAndLights();
    testBlendShapes();
    testOptionsText();
    testDeterminism();
    testAssetC();
    AVER_INFO(g_failures == 0 ? "FbxImportTest: PASS" : "FbxImportTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
