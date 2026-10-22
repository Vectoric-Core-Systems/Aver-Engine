// PhysicsBenchTool -- not a test. Measurements the physics design asks for, run by hand on a real machine.
//
//   PhysicsBenchTool shapes [--grid N]... [--heightfield N]... [--mesh file.ocmesh]... [--content dir [--top K]] [--repeat R]
//
// SHAPES is the gate of phase E (docs/plans/wave11/WORLD_STATE_DEBUG.md, 2.7): does restoring a mesh or heightfield shape from its
// binary blob cost a fifth or less of building it, and is the blob under three times the triangles it came from? For each subject
// it prints the BVH build time, the save time, the restore time (best of R, checksum included), the blob size, and the verdict.
// Wire the cache into the loaders (PHYSICS.SHAPECACHE) only if the real levels say GO; the numbers go in section 8.4 of the design.
//
//   * --grid N        a synthetic N x N vertex bumpy grid (2*(N-1)^2 triangles). Default: 128, 256, 512.
//   * --heightfield N a synthetic N x N heightfield. Default: 1025. Use 4096 for the 4k field of the gate.
//   * --mesh F        one .ocmesh, taken whole (every triangle, every material slot): an UPPER bound on what the collision builder
//                     feeds Jolt, which filters non-colliding slots and simplifies first.
//   * --content DIR   the K (default 8) largest .ocmesh files under DIR, as --mesh. NewSponza, NeonDistrict, CyberCity2099.
//
// What it does not time: reading the blob from disk and the AVR1 container check (modules/world PhysicsShapeCache); it times the
// physics module's half. The disk half is a sequential read of the blob size printed here.
//
// Not a *Test, so ctest does not pick it up. Exit code 0 unless it could not run.
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_state_abi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Row {
    std::string label;
    size_t triangles = 0;     // or samples, for a heightfield
    double buildMs = 0, saveMs = 0, restoreMs = 0;
    size_t blobBytes = 0, rawBytes = 0;
    bool ok = false;
};

static float bump(float x, float y) { return 25.0f * std::sin(x * 0.045f) * std::cos(y * 0.03f) + 6.0f * std::sin(x * 0.31f + y * 0.17f); }

static Row measure(const std::string& label, size_t count, size_t rawBytes, int repeat, int32_t (*build)(const void*), const void* ctx) {
    Row r;
    r.label = label;
    r.triangles = count;
    r.rawBytes = rawBytes;
    const Clock::time_point t0 = Clock::now();
    const int32_t shape = build(ctx);
    r.buildMs = msSince(t0);
    if (!shape) { AVER_WARN("{}: the shape did not build", label); return r; }

    const uint64_t key = 0xBE5C0DE5ull;
    const Clock::time_point t1 = Clock::now();
    const int32_t need = aver_phys_shape_save(shape, key, nullptr, 0);
    std::vector<uint8_t> blob(need > 0 ? static_cast<size_t>(need) : 0u);
    const int32_t wrote = need > 0 ? aver_phys_shape_save(shape, key, blob.data(), need) : 0;
    r.saveMs = msSince(t1);
    if (wrote != need || need <= 0) { AVER_WARN("{}: the shape cannot be saved", label); aver_phys_release_mesh_shape(shape); return r; }
    r.blobBytes = blob.size();

    r.restoreMs = 1.0e30;
    for (int i = 0; i < repeat; ++i) {
        const Clock::time_point t2 = Clock::now();
        const int32_t back = aver_phys_shape_restore(blob.data(), need, key);
        const double ms = msSince(t2);
        if (!back) { AVER_WARN("{}: the blob did not restore", label); aver_phys_release_mesh_shape(shape); return r; }
        r.restoreMs = std::min(r.restoreMs, ms);
        aver_phys_release_mesh_shape(back);
    }
    aver_phys_release_mesh_shape(shape);
    r.ok = true;
    return r;
}

struct MeshCtx { const float* v; int32_t vc; const int32_t* idx; int32_t ic; };
static int32_t buildMesh(const void* p) {
    const MeshCtx& m = *static_cast<const MeshCtx*>(p);
    return aver_phys_create_mesh_shape(m.v, m.vc, m.idx, m.ic);
}
struct FieldCtx { const float* s; int32_t n; };
static int32_t buildField(const void* p) {
    const FieldCtx& f = *static_cast<const FieldCtx*>(p);
    return aver_phys_create_heightfield_shape(f.s, f.n, 25.0f);
}

static Row gridRow(int n, int repeat) {
    std::vector<float> v;
    std::vector<int32_t> idx;
    v.reserve(static_cast<size_t>(n) * n * 3);
    for (int iy = 0; iy < n; ++iy)
        for (int ix = 0; ix < n; ++ix) {
            const float x = static_cast<float>(ix) * 15.0f, y = static_cast<float>(iy) * 15.0f;
            v.insert(v.end(), {x, y, bump(x, y)});
        }
    for (int iy = 0; iy + 1 < n; ++iy)
        for (int ix = 0; ix + 1 < n; ++ix) {
            const int32_t a = iy * n + ix, b = a + 1, c = a + n + 1, d = a + n;
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    const MeshCtx ctx{v.data(), static_cast<int32_t>(v.size() / 3), idx.data(), static_cast<int32_t>(idx.size())};
    return measure("grid " + std::to_string(n) + "x" + std::to_string(n), idx.size() / 3, v.size() * 4 + idx.size() * 4, repeat, &buildMesh, &ctx);
}

static Row fieldRow(int n, int repeat) {
    std::vector<float> s(static_cast<size_t>(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) s[static_cast<size_t>(y) * n + x] = bump(static_cast<float>(x) * 25.0f, static_cast<float>(y) * 25.0f);
    const FieldCtx ctx{s.data(), n};
    return measure("heightfield " + std::to_string(n) + "^2", static_cast<size_t>(n) * n, s.size() * 4, repeat, &buildField, &ctx);
}

static Row meshFileRow(const std::string& path, int repeat) {
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path, md, &why)) { AVER_WARN("{}: {}", path, why); Row r; r.label = path; return r; }
    std::vector<int32_t> idx(md.indices.begin(), md.indices.end());
    const MeshCtx ctx{md.positions.data(), static_cast<int32_t>(md.positions.size() / 3), idx.data(), static_cast<int32_t>(idx.size())};
    return measure(fs::path(path).filename().string(), idx.size() / 3, md.positions.size() * 4 + idx.size() * 4, repeat, &buildMesh, &ctx);
}

static bool go(const Row& r) { return r.ok && r.restoreMs * 5.0 < r.buildMs && r.blobBytes < r.rawBytes * 3; }

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "shapes") != 0) {
        std::printf("usage: PhysicsBenchTool shapes [--grid N]... [--heightfield N]... [--mesh f.ocmesh]... [--content dir [--top K]] [--repeat R]\n");
        return 2;
    }
    std::vector<int> grids, fields;
    std::vector<std::string> meshes;
    std::string content;
    int top = 8, repeat = 3;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--grid" && more) grids.push_back(std::atoi(argv[++i]));
        else if (a == "--heightfield" && more) fields.push_back(std::atoi(argv[++i]));
        else if (a == "--mesh" && more) meshes.push_back(argv[++i]);
        else if (a == "--content" && more) content = argv[++i];
        else if (a == "--top" && more) top = std::max(1, std::atoi(argv[++i]));
        else if (a == "--repeat" && more) repeat = std::max(1, std::atoi(argv[++i]));
        else { std::printf("unknown argument %s\n", a.c_str()); return 2; }
    }
    if (grids.empty() && fields.empty() && meshes.empty() && content.empty()) { grids = {128, 256, 512}; fields = {1025}; }

    if (!content.empty()) {
        std::vector<std::pair<uintmax_t, std::string>> found;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(content, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".ocmesh") found.emplace_back(it->file_size(ec), it->path().string());
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < found.size() && i < static_cast<size_t>(top); ++i) meshes.push_back(found[i].second);
        std::printf("%zu .ocmesh files under %s, taking the %zu largest\n", found.size(), content.c_str(), std::min(found.size(), static_cast<size_t>(top)));
    }

    if (!aver_phys_init()) { std::printf("the physics world did not start\n"); return 3; }
    std::vector<Row> rows;
    for (const int n : grids) if (n >= 2) rows.push_back(gridRow(n, repeat));
    for (const int n : fields) if (n >= 2) rows.push_back(fieldRow(n, repeat));
    for (const std::string& m : meshes) rows.push_back(meshFileRow(m, repeat));

    std::printf("\n%-34s %10s %10s %9s %11s %10s %9s %9s  %s\n", "subject", "tris/samp", "build ms", "save ms", "restore ms", "blob MB", "raw MB", "restore/build", "gate");
    int goes = 0, measured = 0;
    for (const Row& r : rows) {
        if (!r.ok) { std::printf("%-34s  (not measured)\n", r.label.c_str()); continue; }
        ++measured;
        goes += go(r) ? 1 : 0;
        std::printf("%-34s %10zu %10.1f %9.1f %11.2f %10.2f %9.2f %9.1f%%  %s\n", r.label.c_str(), r.triangles, r.buildMs, r.saveMs, r.restoreMs,
                    static_cast<double>(r.blobBytes) / 1048576.0, static_cast<double>(r.rawBytes) / 1048576.0,
                    r.buildMs > 0 ? 100.0 * r.restoreMs / r.buildMs : 0.0, go(r) ? "GO" : "no");
    }
    std::printf("\ngate (restore under a fifth of build AND blob under 3x the raw triangles): %d of %d subjects GO\n", goes, measured);
    std::printf("record these numbers in docs/plans/wave11/WORLD_STATE_DEBUG.md section 8.4; wire the cache (PHYSICS.SHAPECACHE) only if the real levels say GO\n");
    aver_phys_shutdown();
    return 0;
}
