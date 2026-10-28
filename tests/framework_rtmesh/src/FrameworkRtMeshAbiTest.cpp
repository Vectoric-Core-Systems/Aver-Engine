// FrameworkRtMeshAbiTest -- G4 R7: the runtime-mesh and MultiMesh C ABI (framework_rtmesh_abi.h) end to end through the Aver.Framework DLL: creation by key and
// the budgets, surfaces and what the built view reads back, the dirty and removed queues the host drains, MultiMesh instance tables baked from a runtime mesh and
// from an .ocmesh in the project's content folder, visible counts, colours, custom data and the vertex budget, and the refusals. The store is process-wide, so each
// section ends with aver_fw_rtmesh_reset. Exits 0 or 1.
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/MeshBuild.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/framework/framework_rtmesh_abi.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }

static void resetAll() {
    aver_fw_rtmesh_reset();
    aver_fw_rtmesh_configure(AVER_FW_RTMESH_DEFAULT_MAX_MESHES, AVER_FW_RTMESH_DEFAULT_MAX_VERTICES);
    uint64_t sink[64];
    while (aver_fw_rtmesh_take_removed(sink, 64) > 0) {}
    int32_t ids[64];
    while (aver_fw_rtmesh_take_dirty(ids, 64) > 0) {}
}

static const float kTri[] = {0, 0, 0, 100, 0, 0, 0, 100, 0};

static void testBasics() {
    AVER_INFO("=== create, surfaces, view ===");
    resetAll();
    check(aver_fw_rtmesh_abi_version() == 1 && AVER_FW_RTMESH_ABI_VERSION == 1, "the runtime-mesh ABI is version 1");
    check(aver_fw_rtmesh_create(nullptr) == 0 && aver_fw_rtmesh_create("") == 0, "a null or empty key is refused");
    const std::string longKey(121, 'k');
    check(aver_fw_rtmesh_create(longKey.c_str()) == 0, "a key over 120 bytes is refused");
    const int32_t a = aver_fw_rtmesh_create("Main/Blob");
    check(a > 0 && aver_fw_rtmesh_count() == 1, "a mesh is made");
    check(aver_fw_rtmesh_create("Main/Blob") == a && aver_fw_rtmesh_find("Main/Blob") == a && aver_fw_rtmesh_find("Nope") == 0 && aver_fw_rtmesh_find(nullptr) == 0,
          "the same key returns the same mesh; find answers by key");
    check(aver_fw_rtmesh_object_id(a) == fnv1a64(std::string("@rtmesh/Main/Blob")) && aver_fw_rtmesh_object_id(999) == 0, "the object id is fnv1a64 of the path '@rtmesh/<key>'");
    char key[64] = {};
    check(aver_fw_rtmesh_key(a, key, sizeof key) == 9 && std::string(key) == "Main/Blob" && aver_fw_rtmesh_key(999, key, sizeof key) == 0, "the key reads back by id");

    AverFwRtMeshView v{};
    check(aver_fw_rtmesh_view(a, &v) == 0, "a mesh with no surface has nothing to view");
    const uint32_t idx[] = {0, 1, 2};
    check(aver_fw_rtmesh_add_surface(a, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 3, idx, "M_Rock") == 0, "the first surface is index 0");
    int32_t info[4] = {};
    check(aver_fw_rtmesh_info(a, info) == 1 && info[0] == 1 && info[1] == 3 && info[2] == 1, "info: one surface, 3 vertices, 1 triangle");
    check(aver_fw_rtmesh_view(a, &v) == 1 && v.vertexCount == 3 && v.indexCount == 3 && v.submeshCount == 1, "the view reads back the built mesh");
    check(near(v.pos[3], 100) && near(v.nrm[2], 1.0) && v.col == nullptr && v.uv1 == nullptr && v.tan == nullptr, "positions are kept, normals computed, absent streams are null");
    check(std::string(v.subMaterial[0]) == "M_Rock" && v.subStart[0] == 0 && v.subCount[0] == 3, "the material name travels with the submesh");
    check(v.objectId == aver_fw_rtmesh_object_id(a), "the view carries the object id");

    const float col[] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    const float tan[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float uv1[] = {0, 0, 1, 0, 0, 1};
    check(aver_fw_rtmesh_add_surface(a, 3, kTri, nullptr, nullptr, col, uv1, tan, 0, nullptr, nullptr) == 1, "a second surface with colour, tangent and UV1 and no index list");
    check(aver_fw_rtmesh_surface_count(a) == 2 && aver_fw_rtmesh_info(a, info) == 1 && info[0] == 2 && info[1] == 6 && info[2] == 2, "two surfaces, six vertices");
    check(aver_fw_rtmesh_view(a, &v) == 1 && v.col && v.uv1 && v.tan && v.submeshCount == 2 && std::string(v.subMaterial[1]) == "M_Default", "the streams appear, padded for the first surface");
    check(near(v.col[0], 1) && near(v.col[3 * 4 + 1], 0) && near(v.col[3 * 4], 1), "the colour stream: white for surface 0, the given colours for surface 1");

    // refusals
    check(aver_fw_rtmesh_add_surface(999, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == -1, "an unknown mesh is refused");
    check(aver_fw_rtmesh_add_surface(a, 0, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == -1, "no vertices is refused");
    check(aver_fw_rtmesh_add_surface(a, 3, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == -1, "no positions is refused");
    const uint32_t bad[] = {0, 1, 3};
    check(aver_fw_rtmesh_add_surface(a, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 3, bad, nullptr) == -1, "an index past the vertices is refused");
    check(aver_fw_rtmesh_add_surface(a, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 2, idx, nullptr) == -1, "an index count that is not a multiple of three is refused");
    check(aver_fw_rtmesh_add_surface(a, 4, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == -1, "four unindexed vertices (not whole triangles) are refused");
    check(aver_fw_rtmesh_surface_count(a) == 2, "refusals change nothing");

    check(aver_fw_rtmesh_clear(a) == 1 && aver_fw_rtmesh_surface_count(a) == 0 && aver_fw_rtmesh_view(a, &v) == 0, "clear removes the surfaces; the mesh stays");
    check(aver_fw_rtmesh_destroy(a) == 1 && aver_fw_rtmesh_destroy(a) == 0 && aver_fw_rtmesh_find("Main/Blob") == 0 && aver_fw_rtmesh_count() == 0, "destroy removes once");
    resetAll();
}

static void testBudgets() {
    AVER_INFO("=== budgets ===");
    resetAll();
    aver_fw_rtmesh_configure(3, 10);
    check(aver_fw_rtmesh_create("a") > 0 && aver_fw_rtmesh_create("b") > 0 && aver_fw_rtmesh_create("c") > 0, "three meshes fit");
    check(aver_fw_rtmesh_create("d") == 0 && aver_fw_rtmesh_create("a") > 0, "a fourth is refused; an existing key still answers");
    const int32_t a = aver_fw_rtmesh_find("a");
    const float big[3 * 11] = {};
    check(aver_fw_rtmesh_add_surface(a, 11, big, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == -1, "a surface over the vertex budget is refused");
    check(aver_fw_rtmesh_add_surface(a, 9, big, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == 0, "9 vertices fit");
    check(aver_fw_rtmesh_add_surface(a, 3, big, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr) == -1, "the budget counts the surfaces already there");
    resetAll();
}

static void testQueues() {
    AVER_INFO("=== dirty and removed queues ===");
    resetAll();
    const int32_t a = aver_fw_rtmesh_create("one"), b = aver_fw_rtmesh_create("two");
    int32_t ids[8];
    check(aver_fw_rtmesh_take_dirty(ids, 8) == 0, "new meshes with no surface are not dirty");
    aver_fw_rtmesh_add_surface(a, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr);
    check(aver_fw_rtmesh_take_dirty(ids, 8) == 1 && ids[0] == a, "a surface makes its mesh dirty");
    check(aver_fw_rtmesh_take_dirty(ids, 8) == 0, "taking clears it");
    aver_fw_rtmesh_add_surface(a, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr);
    aver_fw_rtmesh_add_surface(b, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr);
    check(aver_fw_rtmesh_take_dirty(ids, 1) == 1 && aver_fw_rtmesh_take_dirty(ids + 1, 7) == 1, "max limits one take; the rest wait");
    aver_fw_rtmesh_clear(a);
    check(aver_fw_rtmesh_take_dirty(ids, 8) == 1 && ids[0] == a, "clearing marks it dirty (the host then finds nothing to view and unloads)");
    check(aver_fw_rtmesh_take_dirty(nullptr, 8) == 0 && aver_fw_rtmesh_take_dirty(ids, 0) == 0, "bad arguments take nothing");

    uint64_t gone[8];
    check(aver_fw_rtmesh_take_removed(gone, 8) == 0, "nothing removed yet");
    aver_fw_rtmesh_destroy(a);
    check(aver_fw_rtmesh_take_removed(gone, 8) == 1 && gone[0] == fnv1a64(std::string("@rtmesh/one")), "a destroyed mesh is reported by object id");
    check(aver_fw_rtmesh_take_removed(gone, 8) == 0, "once");
    aver_fw_rtmesh_reset();
    check(aver_fw_rtmesh_count() == 0 && aver_fw_rtmesh_take_removed(gone, 8) == 1 && gone[0] == fnv1a64(std::string("@rtmesh/two")), "reset forgets everything and reports the removals");
    resetAll();
}

static void testMultiMesh() {
    AVER_INFO("=== MultiMesh from a runtime mesh ===");
    resetAll();
    const int32_t src = aver_fw_rtmesh_create("src/tri");
    aver_fw_rtmesh_add_surface(src, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, "M_Leaf");
    check(aver_fw_multimesh_create(nullptr) == 0, "a null key is refused");
    const int32_t mm = aver_fw_multimesh_create("Grass");
    check(mm > 0 && aver_fw_multimesh_create("Grass") == mm, "a MultiMesh is made, once per key");
    const int32_t target = aver_fw_multimesh_mesh(mm);
    check(target > 0 && aver_fw_rtmesh_object_id(target) == fnv1a64(std::string("@rtmesh/multimesh/Grass")), "it bakes into the runtime mesh 'multimesh/<key>'");
    check(aver_fw_multimesh_destroy(aver_fw_multimesh_create("Other")) == 1 && aver_fw_rtmesh_find("multimesh/Other") == 0, "destroying a MultiMesh removes its mesh");
    check(aver_fw_rtmesh_destroy(target) == 0, "its mesh cannot be destroyed by hand");

    check(aver_fw_multimesh_set_source(mm, "@rtmesh/src/tri") == 1 && aver_fw_multimesh_set_count(mm, 3) == 1, "source and count");
    const float t1[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 500, 0, 0};
    const float t2[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 600, 0};
    check(aver_fw_multimesh_set_transform(mm, 1, t1) == 1 && aver_fw_multimesh_set_transform(mm, 2, t2) == 1, "transforms");
    check(aver_fw_multimesh_set_transform(mm, 3, t1) == 0 && aver_fw_multimesh_set_transform(mm, -1, t1) == 0 && aver_fw_multimesh_set_transform(mm, 0, nullptr) == 0, "an index out of range or no data is refused");
    const float nan[12] = {NAN, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    check(aver_fw_multimesh_set_transform(mm, 0, nan) == 0, "a non-finite transform is refused");
    int32_t info[4] = {};
    check(aver_fw_multimesh_info(mm, info) == 1 && info[0] == 3 && info[1] == 3 && info[2] == 0, "info: 3 instances, 3 visible, none drawn before the first bake");
    check(aver_fw_rtmesh_pump() == 1, "pump bakes the one changed MultiMesh");
    check(aver_fw_rtmesh_pump() == 0, "and nothing the second time");
    check(aver_fw_multimesh_info(mm, info) == 1 && info[2] == 3, "three instances drawn");
    int32_t dirty[8];
    check(aver_fw_rtmesh_take_dirty(dirty, 8) >= 1, "the baked mesh is dirty for the host");
    AverFwRtMeshView v{};
    check(aver_fw_rtmesh_view(target, &v) == 1 && v.vertexCount == 9 && v.indexCount == 9, "the baked mesh has the triangle three times");
    check(near(v.pos[3 * 3 + 0], 500) && near(v.pos[6 * 3 + 1], 600) && near(v.pos[4 * 3], 600), "instance positions: moved by the table");
    check(std::string(v.subMaterial[0]) == "M_Leaf", "the source's material name carries over");

    check(aver_fw_multimesh_set_visible(mm, 1) == 1, "visible = 1");
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(target, &v) == 1 && v.vertexCount == 3, "only the first instance is drawn");
    check(aver_fw_multimesh_set_visible(mm, -1) == 1, "visible = -1 is all");
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(target, &v) == 1 && v.vertexCount == 9, "all three again");

    // colours and custom data
    check(aver_fw_multimesh_set_flags(mm, 1, 1) == 1, "colours and custom data on");
    aver_fw_multimesh_set_color(mm, 2, 0.25f, 0.5f, 0.75f, 1.0f);
    aver_fw_multimesh_set_custom(mm, 1, 7, 8, 9, 10);
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(target, &v) == 1 && v.col && near(v.col[6 * 4], 0.25) && near(v.col[6 * 4 + 2], 0.75) && near(v.col[0], 1.0), "instance colours reach the vertices; the default is white");
    check(v.uv1 && near(v.uv1[3 * 2], 7) && near(v.uv1[3 * 2 + 1], 8) && near(v.uv1[0], 0), "custom data x,y reach UV1");
    check(aver_fw_multimesh_set_color(mm, 3, 1, 1, 1, 1) == 0 && aver_fw_multimesh_set_custom(mm, -1, 0, 0, 0, 0) == 0, "an index out of range is refused");

    // the source changing re-bakes
    aver_fw_rtmesh_add_surface(src, 3, kTri, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, "M_Leaf2");
    check(aver_fw_rtmesh_pump() >= 1 && aver_fw_rtmesh_view(target, &v) == 1 && v.vertexCount == 18, "changing the source mesh bakes the instances again");

    // bulk buffer
    const float buf[24] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 20, 0, 0};
    const float cols[8] = {1, 0, 0, 1, 0, 0, 1, 1};
    check(aver_fw_multimesh_set_buffer(mm, 2, buf, cols, nullptr) == 1, "set_buffer resizes and fills");
    aver_fw_rtmesh_pump();
    check(aver_fw_multimesh_info(mm, info) == 1 && info[0] == 2 && info[2] == 2, "two instances now");
    check(aver_fw_rtmesh_view(target, &v) == 1 && v.vertexCount == 12 && near(v.col[6 * 4 + 2], 1.0), "and the bulk colours landed");
    check(aver_fw_multimesh_set_buffer(mm, 2, nullptr, nullptr, nullptr) == 0 && aver_fw_multimesh_set_buffer(mm, -1, buf, nullptr, nullptr) == 0, "a bulk set with no data is refused");
    check(aver_fw_multimesh_set_count(mm, -1) == 0 && aver_fw_multimesh_set_count(mm, (1 << 20) + 1) == 0, "an instance count out of range is refused");

    // the vertex budget
    aver_fw_rtmesh_configure(0, 12);
    aver_fw_multimesh_set_count(mm, 100);
    aver_fw_rtmesh_pump();
    check(aver_fw_multimesh_info(mm, info) == 1 && info[0] == 100 && info[2] == 2, "a 12-vertex budget draws two of the 100 six-vertex instances");
    resetAll();
}

static void testMultiMeshFromFile() {
    AVER_INFO("=== MultiMesh from an .ocmesh in the content folder ===");
    resetAll();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aver_rtmesh_abi_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Meshes", ec);
    fmt::RuntimeSurface s;
    s.pos = {0, 0, 0, 10, 0, 0, 0, 10, 0, 10, 10, 0};
    s.idx = {0, 1, 2, 1, 3, 2};
    s.uv = {0, 0, 1, 0, 0, 1, 1, 1};
    s.material = "M_Stone";
    fmt::OcMeshData md;
    std::string why;
    check(fmt::assembleSurfaces({s}, md, &why), "the test mesh assembles");
    check(fmt::saveOcMesh((root / "Meshes" / "Quad.ocmesh").string(), md, &why), "and is written: " + why);
    const std::string content = root.string();
    aver_fw_set_project(content.c_str(), content.c_str(), content.c_str(), 0);

    const int32_t mm = aver_fw_multimesh_create("Stones");
    aver_fw_multimesh_set_source(mm, "Meshes/Quad.ocmesh");
    const float buf[24] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 50, 0, 0};
    aver_fw_multimesh_set_buffer(mm, 2, buf, nullptr, nullptr);
    check(aver_fw_rtmesh_pump() == 1, "the MultiMesh bakes");
    AverFwRtMeshView v{};
    check(aver_fw_rtmesh_view(aver_fw_multimesh_mesh(mm), &v) == 1 && v.vertexCount == 8 && v.indexCount == 12, "two copies of the file's quad");
    check(std::string(v.subMaterial[0]) == "M_Stone" && near(v.pos[4 * 3], 50), "its material and the second copy's position");

    aver_fw_multimesh_set_source(mm, "Meshes/Missing.ocmesh");
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(aver_fw_multimesh_mesh(mm), &v) == 0, "a source that cannot be read draws nothing (and the mesh is empty for the host)");
    aver_fw_multimesh_set_source(mm, "../Outside.ocmesh");
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(aver_fw_multimesh_mesh(mm), &v) == 0, "a path that climbs out of the content folder is refused");
    aver_fw_multimesh_set_source(mm, "C:/x.ocmesh");
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(aver_fw_multimesh_mesh(mm), &v) == 0, "so is a drive path");
    aver_fw_multimesh_set_source(mm, "@rtmesh/not/there");
    aver_fw_rtmesh_pump();
    check(aver_fw_rtmesh_view(aver_fw_multimesh_mesh(mm), &v) == 0, "and a runtime mesh that does not exist");
    std::filesystem::remove_all(root, ec);
    resetAll();
}

int main() {
    testBasics();
    testBudgets();
    testQueues();
    testMultiMesh();
    testMultiMeshFromFile();
    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
