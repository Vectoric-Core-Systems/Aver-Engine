// FabricGeometryTest -- G4 R7 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/GeomGame, then the files it wrote read back with the engine's own
// loaders. A stored ArrayMesh decoded from packed bytes, a MultiMesh table, the trail meshes, CSG trees baked into meshes (volumes and bounds), Sprite3D's quad and
// material, Label3D, GridMap cells (item instances, orientation, collision shapes) and the report. Runs the tool as a process, like FabricConvertTest.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotVariant.hpp"
#include "aver/platform/FileSystem.hpp"
#if defined(AVER_FABRIC_TEST_OCMAT)
#include "aver/formats/OcMat.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;
namespace gd = aver::fmt::godot;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

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

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

static int find(const fmt::OcWorldData& w, std::initializer_list<const char*> path) {
    int parent = -1;
    for (const char* name : path) {
        int found = -1;
        for (usize i = 0; i < w.placements.size(); ++i)
            if (w.placements[i].parent == parent && w.placements[i].name == name) { found = static_cast<int>(i); break; }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

static bool propVar(const fmt::OcWorldData& w, int idx, const char* key, gd::GodotVariant& out) {
    if (idx < 0) return false;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find(key)) {
                gd::GodotDiagnostics d;
                return gd::parseGodotVariant(*v, out, &d, key);
            }
    return false;
}

static std::string resourceProp(const fmt::OcWorldData& w, int idx, const char* key) {
    gd::GodotVariant v;
    return propVar(w, idx, key, v) && v.type == gd::GodotVType::Resource ? v.s : std::string();
}

static bool hasProp(const fmt::OcWorldData& w, int idx, const char* key) {
    gd::GodotVariant v;
    return propVar(w, idx, key, v);
}

// The signed volume of an engine-space triangle mesh (cm^3), absolute.
static double volumeCm3(const fmt::OcMeshData& m) {
    double v = 0;
    for (usize t = 0; t + 2 < m.indices.size(); t += 3) {
        const f32* a = &m.positions[usize(m.indices[t]) * 3];
        const f32* b = &m.positions[usize(m.indices[t + 1]) * 3];
        const f32* c = &m.positions[usize(m.indices[t + 2]) * 3];
        v += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) - double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) + double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6.0;
    }
    return std::fabs(v);
}

static const gd::GodotVariant* objField(const gd::GodotVariant& o, const char* key) {
    for (usize i = 0; i < o.keys.size(); ++i)
        if (o.keys[i].s == key) return &o.items[i];
    return nullptr;
}

int main() {
    AVER_INFO("FabricGeometryTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/GeomGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-geometry-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    // The scratch copy of the project, with a real PNG header (64 x 32) where the fixture keeps a stand-in: the converter reads a sprite's size from IHDR.
    const std::string in = (outRoot / "in").string();
    std::filesystem::copy(dataDir, in, std::filesystem::copy_options::recursive, ec);
    check(!ec, "the fixture is copied to a scratch folder");
    {
        const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 64, 0, 0, 0, 32, 8, 6, 0, 0, 0, 0, 0, 0, 0};
        std::ofstream f(in + "/textures/sprite.png", std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(png), sizeof png);
    }
    const std::string out1 = (outRoot / "one").string();
    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + in + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string proj = out1 + "/GeomGame";
    const std::string content = proj + "/Content";
    std::string err;
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);

    // ---- ArrayMesh --------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- ArrayMesh: Godot's packed surface decoded --");
    {
        const int idx = find(w, {"Main", "Array"});
        const std::string asset = resourceProp(w, idx, "mesh");
        fmt::OcMeshData m;
        check(!asset.empty() && fmt::loadOcMesh(content + "/" + asset, m, &err), "the ArrayMesh became an .ocmesh: " + asset + " " + err);
        check(m.vertexCount() == 4 && m.indices.size() == 6 && m.valid(), "four vertices, two triangles");
        if (m.vertexCount() == 4) {
            // Godot (x, y, z) metres -> engine (-z, x, y) * 100 cm: vertex 1 (1, 0, 0) -> (0, 100, 0), vertex 3 (0, 1, 0) -> (0, 0, 100).
            check(near(m.positions[3], 0) && near(m.positions[4], 100) && near(m.positions[5], 0), "vertex 1 went through the axis map and metres to centimetres");
            check(near(m.positions[9], 0) && near(m.positions[10], 0) && near(m.positions[11], 100), "vertex 3 too");
            check(near(m.uvs[2], 1) && near(m.uvs[3], 0) && near(m.uvs[7], 1), "UVs are carried as stored");
            check(near(m.normals[0], -1) && near(m.normals[1], 0) && near(m.normals[2], 0), "the octahedral normal (+Z) is engine -X");
            check(m.indices[0] == 0 && m.indices[1] == 2 && m.indices[2] == 1 && m.indices[3] == 0 && m.indices[4] == 3 && m.indices[5] == 2,
                  "the index order is kept (Godot's clockwise front faces are the engine's order after the axis map)");
            // The same orientation as a generated BoxMesh face: cross(b - a, c - a) against the stored normal has one sign for both.
            const auto faceSign = [](const fmt::OcMeshData& mesh, usize tri) {
                const f32* a = &mesh.positions[usize(mesh.indices[tri * 3]) * 3];
                const f32* b = &mesh.positions[usize(mesh.indices[tri * 3 + 1]) * 3];
                const f32* c = &mesh.positions[usize(mesh.indices[tri * 3 + 2]) * 3];
                const f32* n = &mesh.normals[usize(mesh.indices[tri * 3]) * 3];
                const double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]}, e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
                return (e1[1] * e2[2] - e1[2] * e2[1]) * n[0] + (e1[2] * e2[0] - e1[0] * e2[2]) * n[1] + (e1[0] * e2[1] - e1[1] * e2[0]) * n[2];
            };
            fmt::OcMeshData boxRef;
            gd::GodotVariant mmv;
            const gd::GodotVariant* bm = propVar(w, find(w, {"Main", "Many"}), "multimesh", mmv) ? objField(mmv, "mesh") : nullptr;
            check(bm && fmt::loadOcMesh(content + "/" + bm->s, boxRef, &err), "a generated BoxMesh is the reference for the winding");
            check(boxRef.vertexCount() > 0 && faceSign(m, 0) * faceSign(boxRef, 0) > 0, "the decoded triangle faces the way a generated primitive's does");
            check(m.hasTangent(), "the authored tangent is kept");
            if (m.hasTangent()) check(near(m.tangents[0], 0) && near(m.tangents[1], 1) && near(m.tangents[2], 0) && near(m.tangents[3], 1), "tangent +X is engine +Y, handedness +1 (the glTF rule)");
        }
        check(!m.materialSlots.empty() && m.materialSlots[0].find("red") != std::string::npos, "the surface's material became the mesh's slot: " + (m.materialSlots.empty() ? std::string() : m.materialSlots[0]));
    }

    // ---- MultiMesh --------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- MultiMeshInstance3D --");
    {
        const int idx = find(w, {"Main", "Many"});
        gd::GodotVariant mm;
        check(propVar(w, idx, "multimesh", mm) && mm.type == gd::GodotVType::Object && mm.s == "MultiMesh", "the node carries Object(MultiMesh, ...)");
        const gd::GodotVariant* buf = objField(mm, "buffer");
        const gd::GodotVariant* mesh = objField(mm, "mesh");
        check(buf && buf->type == gd::GodotVType::PackedFloat32Array && buf->nums.size() == 48, "the buffer holds 3 instances of 16 floats (12 transform + 4 colour)");
        check(buf && buf->nums.size() == 48 && near(buf->nums[16 + 3], 2) && near(buf->nums[32 + 3], 4) && near(buf->nums[16 + 13], 1) && near(buf->nums[16 + 12], 0), "instance origins and colours are kept in Godot's layout");
        fmt::OcMeshData box;
        check(mesh && mesh->type == gd::GodotVType::Resource && fmt::loadOcMesh(content + "/" + mesh->s, box, &err) && box.vertexCount() == 24, "the instanced BoxMesh became a mesh asset");
        const gd::GodotVariant* colors = objField(mm, "use_colors");
        const gd::GodotVariant* count = objField(mm, "instance_count");
        check(colors && colors->b && count && count->asInt() == 3, "use_colors and instance_count are kept");
    }

    // ---- trail meshes and TextMesh --------------------------------------------------------------------------------------------------------
    AVER_INFO("-- TubeTrailMesh, RibbonTrailMesh, TextMesh --");
    {
        fmt::OcMeshData tube, ribbon;
        check(fmt::loadOcMesh(content + "/" + resourceProp(w, find(w, {"Main", "Tube"}), "mesh"), tube, &err), "the tube trail mesh loads: " + err);
        check(tube.valid() && near(tube.boundsMax.z - tube.boundsMin.z, 200.0, 0.5) && near(tube.boundsMax.x, 25.0, 0.5), "a static tube: 4 sections of 0.5 m = 2 m tall (engine Z), radius 25 cm");
        check(fmt::loadOcMesh(content + "/" + resourceProp(w, find(w, {"Main", "Ribbon"}), "mesh"), ribbon, &err) && ribbon.valid(), "the ribbon trail mesh loads: " + err);
        check(ribbon.valid() && near(ribbon.boundsMax.z - ribbon.boundsMin.z, 200.0, 0.5), "a static ribbon: 2 sections of 1 m");
        check(!hasProp(w, find(w, {"Main", "Text"}), "mesh"), "a TextMesh gives no mesh (reported unsupported)");
    }

    // ---- CSG ------------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- CSG trees baked --");
    {
        const int csgIdx = find(w, {"Main", "Csg"});
        fmt::OcMeshData m;
        check(fmt::loadOcMesh(content + "/" + resourceProp(w, csgIdx, "mesh"), m, &err) && m.valid(), "the CSGCombiner3D root has a baked mesh: " + err);
        check(near(volumeCm3(m), 7.0e6, 1.0e4), "box 2 m minus an overlapping 2 m box (1 m^3 shared): 7 m^3");
        check(near(m.boundsMin.x, -100, 0.5) && near(m.boundsMax.x, 100, 0.5) && near(m.boundsMin.z, -100, 0.5) && near(m.boundsMax.z, 100, 0.5), "bounds are the base box's (+-1 m)");
        check(find(w, {"Main", "Csg", "Base"}) < 0 && find(w, {"Main", "Csg", "Hole"}) < 0, "the CSG children are folded into the root, not placed");
        check(!m.materialSlots.empty(), "and the mesh has material slots");
        const int bodyIdx = find(w, {"Main", "Csg", "_CSGCollision"});
        check(bodyIdx >= 0 && w.placements[static_cast<usize>(bodyIdx)].className == "StaticBody3D", "use_collision adds a static body child");
        gd::GodotVariant layer;
        check(propVar(w, bodyIdx, "collision_layer", layer) && layer.asInt() == 4, "carrying the CSG node's collision layer");
        gd::GodotVariant shape;
        const int shapeIdx = find(w, {"Main", "Csg", "_CSGCollision", "Shape"});
        check(propVar(w, shapeIdx, "shape", shape) && shape.type == gd::GodotVType::Object && shape.s == "ConcavePolygonShape3D", "whose shape is the baked triangles as a concave polygon");
        const gd::GodotVariant* data = objField(shape, "data");
        check(data && data->type == gd::GodotVType::PackedVector3Array && data->nums.size() % 9 == 0 && data->nums.size() > 100, "a PackedVector3Array of whole triangles");

        fmt::OcMeshData cut;
        const int cutIdx = find(w, {"Main", "Cut"});
        check(fmt::loadOcMesh(content + "/" + resourceProp(w, cutIdx, "mesh"), cut, &err) && cut.valid(), "a sphere with an intersecting slab child is a CSG root too: " + err);
        const double expect = 3.14159265358979323846 * (1.0 - 1.0 / 12.0) * 1.0e6;   // the slab |z| < 0.5 of a unit sphere
        check(near(volumeCm3(cut), expect, expect * 0.06), "sphere intersect slab: pi (r^2 h - h^3 / 12) m^3 within the sphere's facets");
        check(near(cut.boundsMax.x, 50, 0.5) && near(cut.boundsMin.x, -50, 0.5), "the slab cut the sphere to +-0.5 m along Godot Z (engine X)");
        check(w.placements[static_cast<usize>(cutIdx)].y == 500, "and the node keeps its own placement (x = 5 m is engine Y 500 cm)");
    }

    // ---- Sprite3D / Label3D -------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- Sprite3D and Label3D --");
    {
        const int idx = find(w, {"Main", "Sprite"});
        check(idx >= 0 && w.placements[static_cast<usize>(idx)].className == "Sprite3D", "the Sprite3D is placed as Aver.Fabric's Sprite3D");
        fmt::OcMeshData q;
        check(fmt::loadOcMesh(content + "/" + resourceProp(w, idx, "mesh"), q, &err) && q.vertexCount() == 4 && q.indices.size() == 6, "its quad loads: " + err);
        // 64 x 32 texture, 2 horizontal frames: a 32 x 32 px frame at 0.5 m per pixel = 16 m, centred: +-8 m = +-800 cm.
        check(near(q.boundsMax.y, 800, 1) && near(q.boundsMin.y, -800, 1) && near(q.boundsMax.z, 800, 1) && near(q.boundsMin.z, -800, 1), "the quad is the frame's size times pixel_size, centred");
        double umin = 9, umax = -9;
        for (usize i = 0; i + 1 < q.uvs.size(); i += 2) { umin = std::min(umin, double(q.uvs[i])); umax = std::max(umax, double(q.uvs[i])); }
        check(near(umin, 0.5) && near(umax, 1.0), "frame 1 of 2 is the right half of the texture (u 0.5 to 1)");
        const std::string mat = resourceProp(w, idx, "material_override");
        check(!mat.empty() && std::filesystem::exists(content + "/" + mat), "the sprite has a baked material: " + mat);
#if defined(AVER_FABRIC_TEST_OCMAT)
        if (!mat.empty()) {
            pbr::MaterialDesc md;
            fmt::OcMatExtras ex;
            check(fmt::loadOcmat(content + "/" + mat, md, &ex, &err), "the material loads: " + err);
            const std::string text = slurp(content + "/" + mat);
            check(text.find("billboard=y") != std::string::npos, "billboard = Y is the material's billboard mode");
        }
#endif
        const int lab = find(w, {"Main", "Label"});
        check(lab >= 0 && w.placements[static_cast<usize>(lab)].className == "Label3D", "the Label3D is placed as Aver.Fabric's Label3D");
        gd::GodotVariant txt;
        check(propVar(w, lab, "text", txt) && txt.s == "Hello", "with its text");
    }

    // ---- GridMap ----------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- GridMap and MeshLibrary --");
    {
        const int grid = find(w, {"Main", "Grid"});
        check(grid >= 0 && w.placements[static_cast<usize>(grid)].className == "GridMap", "the GridMap is placed as a GridMap");
        gd::GodotVariant lib;
        check(propVar(w, grid, "mesh_library", lib) && lib.type == gd::GodotVType::Object && lib.s == "MeshLibrary", "its library is an Object(MeshLibrary, ...)");
        gd::GodotVariant item0, item1;
        const int i0 = find(w, {"Main", "Grid", "Item0_Block"}), i1 = find(w, {"Main", "Grid", "Item1_Pillar"});
        check(propVar(w, i0, "multimesh", item0) && propVar(w, i1, "multimesh", item1), "one instanced node per used item");
        const gd::GodotVariant* b0 = objField(item0, "buffer");
        const gd::GodotVariant* b1 = objField(item1, "buffer");
        check(b0 && b0->nums.size() == 36 && objField(item0, "instance_count")->asInt() == 3, "the block item has 3 instances");
        if (b0 && b0->nums.size() == 36) {
            // cell_size 2 and centring: cell (x, y, z) sits at (2x + 1, 2y + 1, 2z + 1). Orientation 10 is a half turn about Y: diag(-1, 1, -1).
            const double a[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
            const double b[12] = {-1, 0, 0, 3, 0, 1, 0, 1, 0, 0, -1, 1};
            const double c[12] = {1, 0, 0, -1, 0, 1, 0, 1, 0, 0, 1, 1};
            bool ok = true;
            for (int k = 0; k < 12; ++k) ok = ok && near(b0->nums[static_cast<usize>(k)], a[k]) && near(b0->nums[static_cast<usize>(12 + k)], b[k]) && near(b0->nums[static_cast<usize>(24 + k)], c[k]);
            check(ok, "cell (0,0,0), the half-turned cell (1,0,0) and cell (-1,0,0) have the right transforms in the table");
        }
        check(b1 && b1->nums.size() == 12 && near(b1->nums[3], 1) && near(b1->nums[7], 3.5) && near(b1->nums[11], 5), "the pillar at cell (0,1,2) sits at (1, 3, 5) plus its item's mesh_transform (0, 0.5, 0)");
        const int body = find(w, {"Main", "Grid", "_GridCollision"});
        gd::GodotVariant layer;
        check(body >= 0 && propVar(w, body, "collision_layer", layer) && layer.asInt() == 2, "the items' shapes sit in a static body with the GridMap's layer");
        check(find(w, {"Main", "Grid", "_GridCollision", "Shape0"}) >= 0 && find(w, {"Main", "Grid", "_GridCollision", "Shape2"}) >= 0 && find(w, {"Main", "Grid", "_GridCollision", "Shape3"}) < 0,
              "one collision shape per block cell (3), none for the pillar item without shapes");
        const int s0 = find(w, {"Main", "Grid", "_GridCollision", "Shape0"});
        const int s1 = find(w, {"Main", "Grid", "_GridCollision", "Shape1"});
        check(s0 >= 0 && near(w.placements[static_cast<usize>(s0)].x, -100) && near(w.placements[static_cast<usize>(s0)].y, 100) && near(w.placements[static_cast<usize>(s0)].z, 100),
              "shape 0 is at Godot (1, 1, 1) = engine (-100, 100, 100)");
        check(s1 >= 0 && near(w.placements[static_cast<usize>(s1)].y, 300), "shape 1 is at Godot x = 3 (engine Y 300)");
    }

    // ---- the report -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("\"type\":\"ArrayMesh\"", "\"rating\":\"converted\"", "decoded 1 surface"), "the ArrayMesh is rated converted");
        check(!has("\"type\":\"ArrayMesh\"", "\"rating\":\"unsupported\"", ""), "and no longer listed as unsupported (it used to say 're-export as .glb')");
        check(has("\"type\":\"MultiMesh\"", "\"rating\":\"converted\"", "bakes from the table"), "the MultiMesh is rated converted");
        check(has("\"type\":\"TubeTrailMesh\"", "\"rating\":\"approximated\"", "static straight tube"), "TubeTrailMesh is approximated");
        check(has("\"type\":\"RibbonTrailMesh\"", "\"rating\":\"approximated\"", "ribbon"), "RibbonTrailMesh is approximated");
        check(has("\"type\":\"TextMesh\"", "\"rating\":\"unsupported\"", "glyph"), "TextMesh is unsupported with the reason");
        check(has("\"node\":\"Csg\"", "\"rating\":\"approximated\"", "baked at convert time"), "the CSG root is approximated (baked)");
        check(has("\"node\":\"Csg/Base\"", "\"rating\":\"converted\"", "folded"), "CSG children are reported as folded");
        check(has("\"node\":\"Csg\"", "\"property\":\"use_collision\"", "_CSGCollision"), "use_collision is explained");
        check(has("\"node\":\"Label\"", "\"property\":\"text\"", "\"rating\":\"unsupported\""), "Label3D text is unsupported (not drawn)");
        check(has("\"node\":\"Grid\"", "\"rating\":\"approximated\"", "converted once") || has("\"node\":\"Grid\"", "\"rating\":\"approximated\"", "built once at convert time"), "the GridMap node is approximated (static)");
        check(has("\"node\":\"Grid\"", "\"property\":\"mesh_library\"", "\"rating\":\"converted\""), "mesh_library is converted");
        check(has("\"node\":\"Sprite\"", "\"property\":\"billboard\"", "\"rating\":\"converted\""), "Sprite3D billboard is converted");
    }

    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
