// FabricBinaryTest -- `AverAssetC fabric` on a Godot project whose meshes, collision shapes and GridMap scene are BINARY resources (.res / .scn).
//
// The project is built by the test in a temp folder (GodotBinaryFixtures.hpp writes the RSRC bytes; none is a copy of a real file): a binary ArrayMesh
// quad, a binary BoxShape3D, a text MeshLibrary naming both, a binary GridMap scene with three cells, and a text main scene that instances the grid
// and also uses the binary mesh directly. The converter is run as a process and what it wrote is read back: every binary file reported as read,
// the GridMap's cells placed as an instanced item with collision shapes, the mesh baked, and a level for the binary scene. Exits 0 or 1.
#include "../../formats.godot/src/GodotBinaryFixtures.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotMesh.hpp"
#include "aver/platform/FileSystem.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;
using namespace gbfix;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        AVER_INFO("  ok    {}", what);
        return;
    }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

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

static void writeFile(const std::string& path, const Bytes& b) { writeFileBytes(path, b.data(), b.size()); }
static void writeText(const std::string& path, const std::string& s) { writeFileBytes(path, s.data(), s.size()); }

// One JSON line of the report that holds every needle.
static bool reported(const std::string& jsonl, std::initializer_list<const char*> needles) {
    usize at = 0;
    while (at < jsonl.size()) {
        usize end = jsonl.find('\n', at);
        if (end == std::string::npos) end = jsonl.size();
        const std::string line = jsonl.substr(at, end - at);
        bool all = true;
        for (const char* n : needles)
            if (line.find(n) == std::string::npos) { all = false; break; }
        if (all) return true;
        at = end + 1;
    }
    return false;
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

// A quad in the XZ plane as an ArrayMesh surface (format: vertex, normal, uv, index; version 1), the way Godot 4 stores one.
static Bytes quadMesh() {
    FileDef d;
    d.cls = "ArrayMesh";
    d.uid = 0x220b6198bd8231fdULL;
    FileDef::Res m;
    m.path = "res://quad.res";
    m.type = "ArrayMesh";
    d.res.push_back(m);
    Enc vd, ad, id;
    const f32 pos[12] = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
    for (f32 f : pos) vd.f32v(f);
    for (int i = 0; i < 4; ++i) { vd.u16v(32768); vd.u16v(32768); }   // an octahedral normal that decodes to +Z (the decoder is checked in GodotMeshTest)
    const f32 uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (f32 f : uv) ad.f32v(f);
    for (int i : {0, 1, 2, 0, 2, 3}) id.u16v(u16(i));
    const i64 format = 1 | 2 | 16 | 4096 | (1ll << 35);   // vertex, normal, uv, index, format version 1
    const Fmt& f = d.f;
    const Bytes surface = f.dict({{f.text("aabb"), f.reals(kAabb, {-1, 0, -1, 2, 0, 2})},
                                  {f.text("attribute_data"), f.packedBytes(ad.b)},
                                  {f.text("format"), f.int64(format)},
                                  {f.text("index_count"), f.integer(6)},
                                  {f.text("index_data"), f.packedBytes(id.b)},
                                  {f.text("name"), f.text("quad_surface")},
                                  {f.text("primitive"), f.integer(3)},
                                  {f.text("uv_scale"), f.reals(kVector4, {0, 0, 0, 0})},
                                  {f.text("vertex_count"), f.integer(4)},
                                  {f.text("vertex_data"), f.packedBytes(vd.b)}});
    d.prop(d.res.back(), "resource_name", f.text("quad"));
    d.prop(d.res.back(), "_surfaces", f.array({surface}));
    d.prop(d.res.back(), "blend_shape_mode", f.integer(0));   // stored by Godot because 0 is not the default (relative); meaningless with no blend shapes
    return d.build();
}

static Bytes boxShape() {
    FileDef d;
    d.cls = "BoxShape3D";
    FileDef::Res m;
    m.path = "res://box.res";
    m.type = "BoxShape3D";
    d.res.push_back(m);
    d.prop(d.res.back(), "size", d.f.reals(kVector3, {2, 2, 2}));
    return d.build();
}

// A scene whose root is a GridMap with a MeshLibrary and three cells of item 0.
static Bytes gridScene() {
    FileDef d;
    d.uid = 0x6f607bd40182c27fULL;
    d.ext.push_back({"MeshLibrary", "res://tiles.tres", 0});
    SceneDef s;
    s.names = {"Grid", "GridMap", "mesh_library", "cell_size", "data"};
    const Fmt& f = d.f;
    std::vector<i32> cells;
    for (const auto& c : {std::array<int, 3>{0, 0, 0}, std::array<int, 3>{1, 0, 0}, std::array<int, 3>{0, 0, 1}}) {
        fmt::godot::GodotGridCell cell;
        cell.x = c[0]; cell.y = c[1]; cell.z = c[2]; cell.item = 0;
        i32 packed[3];
        fmt::godot::encodeGridCell(cell, packed);
        cells.insert(cells.end(), packed, packed + 3);
    }
    s.variants = {f.extRef(0), f.reals(kVector3, {2, 2, 2}), f.dict({{f.text("cells"), f.packedI32(cells)}})};
    s.nodes = {-1, -1, 1, 0, -1, 3, 2, 0, 3, 1, 4, 2, 0};
    s.nodeCount = 1;
    s.nodeIds = {7};
    addMainScene(d, s);
    return d.build();
}

int main() {
    AVER_INFO("FabricBinaryTest");
    namespace fs = std::filesystem;
    const fs::path exe = AVER_ASSETC_EXE;
    std::error_code ec;
    check(fs::exists(exe, ec), "AverAssetC was built: " + exe.string());
    if (g_failures) return exitCode(ExitCode::Failed);

    const fs::path root = fs::temp_directory_path() / "aver-fabric-binary-test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "game", ec);
    const std::string game = (root / "game").generic_string();
    writeText(game + "/project.godot", "config_version=5\n\n[application]\n\nconfig/name=\"BinGame\"\nrun/main_scene=\"res://main.tscn\"\n");
    writeFile(game + "/quad.res", quadMesh());
    writeFile(game + "/box.res", boxShape());
    writeFile(game + "/grid.scn", gridScene());
    writeText(game + "/tiles.tres",
              "[gd_resource type=\"MeshLibrary\" format=3]\n\n"
              "[ext_resource type=\"ArrayMesh\" path=\"res://quad.res\" id=\"1_q\"]\n"
              "[ext_resource type=\"Shape3D\" path=\"res://box.res\" id=\"2_b\"]\n\n"
              "[resource]\n"
              "item/0/name = \"Quad\"\n"
              "item/0/mesh = ExtResource(\"1_q\")\n"
              "item/0/mesh_transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)\n"
              "item/0/shapes = [ExtResource(\"2_b\"), Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)]\n"
              "item/0/navigation_mesh_transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)\n"
              "item/0/navigation_layers = 1\n");
    writeText(game + "/main.tscn",
              "[gd_scene load_steps=3 format=3]\n\n"
              "[ext_resource type=\"PackedScene\" path=\"res://grid.scn\" id=\"1_g\"]\n"
              "[ext_resource type=\"ArrayMesh\" path=\"res://quad.res\" id=\"2_q\"]\n\n"
              "[node name=\"Main\" type=\"Node3D\"]\n\n"
              "[node name=\"Grid\" parent=\".\" instance=ExtResource(\"1_g\")]\n\n"
              "[node name=\"Floor\" type=\"MeshInstance3D\" parent=\".\"]\n"
              "mesh = ExtResource(\"2_q\")\n");

    const std::string out = (root / "out").generic_string();
    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + game + "\" \"" + out + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    check(log.find("\"levels\":2") != std::string::npos, "two levels: the text scene and the binary one");
    if (code != 0 || g_failures) AVER_INFO("{}", log);

    const std::string jsonl = slurp(out + "/FabricReport.jsonl");
    check(!jsonl.empty(), "a report was written");
    for (const char* f : {"res://quad.res", "res://box.res", "res://grid.scn"})
        check(reported(jsonl, {"\"kind\":\"file\"", (std::string("\"file\":\"") + f + "\"").c_str(), "\"rating\":\"converted\"", "RSRC"}), std::string(f) + " is reported as a binary file that was read");
    check(!reported(jsonl, {"\"rating\":\"unsupported\"", "resave"}), "nothing is asked to be resaved as text");
    check(reported(jsonl, {"\"kind\":\"scene\"", "res://grid.scn", "\"rating\":\"converted\""}), "the binary scene is converted as a level of its own");
    check(reported(jsonl, {"\"kind\":\"resource\"", "ArrayMesh", "decoded 1 surface(s), 4 vertices, 2 triangles"}), "the binary ArrayMesh was decoded: one surface, 4 vertices, 2 triangles");
    check(reported(jsonl, {"\"type\":\"GridMap\"", "3 cell(s) placed as 1 instanced item node(s) and 3 static collision shape(s)"}),
          "the GridMap read from the binary scene placed its 3 cells as one instanced item with 3 collision shapes");
    check(!reported(jsonl, {"blend shapes are not decoded"}), "the stored blend_shape_mode of a mesh with no blend shapes is not reported as unsupported");
    check(!reported(jsonl, {"navigation meshes of library items"}), "the always-stored navigation transform and layers of a library item are not reported when it has no navigation mesh");

    const std::string content = out + "/BinGame/Content";
    fmt::OcWorldData w;
    std::string why;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &why), "the main level loads: " + why);
    check(find(w, {"Main", "Grid", "Item0_Quad"}) >= 0, "the grid's item node is in the level under the instanced scene");
    const int body = find(w, {"Main", "Grid", "_GridCollision"});
    int shapes = 0;
    if (body >= 0)
        for (const fmt::OcWorldPlacement& p : w.placements)
            if (p.parent == body) ++shapes;
    check(body >= 0 && shapes == 3, "and its static body holds 3 shapes (found " + std::to_string(shapes) + ")");
    check(find(w, {"Main", "Floor"}) >= 0, "the MeshInstance3D that uses the binary mesh directly is placed");
    fmt::OcWorldData g;
    check(fmt::loadOcworld(content + "/Maps/grid.ocworld", g, &why), "the binary scene's own level loads: " + why);
    check(find(g, {"Grid"}) >= 0 && find(g, {"Grid", "Item0_Quad"}) >= 0, "and has the GridMap root with its item node");
    fs::directory_iterator it(content + "/Meshes/Generated", ec);
    int meshes = 0;
    for (; !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".ocmesh") ++meshes;
    check(meshes == 1, "one baked mesh in Meshes/Generated (the quad, shared by the library and the floor): " + std::to_string(meshes));

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) fs::remove_all(root, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
