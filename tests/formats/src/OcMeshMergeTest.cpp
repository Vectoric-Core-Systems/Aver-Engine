// OcMeshMergeTest -- fmt::mergeStaticMeshes (transform baking, material grouping, welding, drops, determinism) and, where
// Trifactor is built, trifactor::simplifyToBudget (budget respected, material slots kept, no NaN). CPU only.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMeshMerge.hpp"
#if AVER_MESHMERGE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// A unit cube [0,1]^3, 24 vertices (per-face normals), 12 outward triangles, one material.
static fmt::OcMeshData cube(const std::string& mat) {
    fmt::OcMeshData m;
    struct Face { f32 n[3]; f32 u[3]; f32 v[3]; };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    for (const Face& f : faces) {
        const u32 base = static_cast<u32>(m.positions.size() / 3);
        const f32 o[3] = {f.n[0] > 0 ? 1.0f : 0.0f, f.n[1] > 0 ? 1.0f : 0.0f, f.n[2] > 0 ? 1.0f : 0.0f};
        for (int c = 0; c < 4; ++c) {
            const f32 a = (c == 1 || c == 2) ? 1.0f : 0.0f, b = (c >= 2) ? 1.0f : 0.0f;
            for (int k = 0; k < 3; ++k) {
                const f32 base0 = (f.n[k] != 0) ? o[k] : 0.0f;
                m.positions.push_back(base0 + a * f.u[k] + b * f.v[k]);
                m.normals.push_back(f.n[k]);
            }
            m.uvs.push_back(a); m.uvs.push_back(b);
        }
        // u x v == n for every face above, so (0,1,2) (0,2,3) is counter-clockwise seen from outside
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    m.materialSlots = {mat};
    fmt::OcMeshSubmesh sm; sm.name = mat; sm.materialSlot = 0; sm.indexStart = 0; sm.indexCount = static_cast<u32>(m.indices.size());
    sm.vertexCount = m.vertexCount();
    m.submeshes = {sm};
    m.computeBounds();
    return m;
}

static f64 signedVolume(const fmt::OcMeshData& m) {
    f64 v = 0;
    for (usize t = 0; t + 2 < m.indices.size(); t += 3) {
        const f32* a = &m.positions[m.indices[t] * 3];
        const f32* b = &m.positions[m.indices[t + 1] * 3];
        const f32* c = &m.positions[m.indices[t + 2] * 3];
        v += (f64(a[0]) * (f64(b[1]) * c[2] - f64(b[2]) * c[1]) - f64(a[1]) * (f64(b[0]) * c[2] - f64(b[2]) * c[0]) +
              f64(a[2]) * (f64(b[0]) * c[1] - f64(b[1]) * c[0])) / 6.0;
    }
    return v;
}

static fmt::OcMergeItem at(const fmt::OcMeshData* mesh, f64 x, f64 y, f64 z, f64 sx = 1, f64 sy = 1, f64 sz = 1, const std::string& mat = {}) {
    fmt::OcMergeItem it;
    it.mesh = mesh;
    it.m[0] = sx; it.m[5] = sy; it.m[10] = sz;
    it.m[3] = x; it.m[7] = y; it.m[11] = z;
    it.material = mat;
    return it;
}

static bool finite(const fmt::OcMeshData& m) {
    for (const f32 v : m.positions) if (!std::isfinite(v)) return false;
    for (const f32 v : m.normals) if (!std::isfinite(v)) return false;
    return true;
}

int main() {
    AVER_INFO("=== OcMeshMergeTest ===");
    const fmt::OcMeshData stone = cube("stone"), wood = cube("wood");
    check(signedVolume(stone) > 0.99 && signedVolume(stone) < 1.01, "fixture: the cube has volume 1 and outward winding");

    {   // conservation
        const std::vector<fmt::OcMergeItem> items = {at(&stone, 0, 0, 0), at(&stone, 10, 0, 0, 2, 3, 4), at(&stone, 0, 10, 0, -1, 1, 1)};
        fmt::OcMeshData out; fmt::OcMergeReport rep; std::string why;
        check(fmt::mergeStaticMeshes(items, {}, out, &rep, &why), "merge succeeds: " + why);
        check(rep.merged == 3 && rep.trianglesIn == 36 && rep.trianglesOut == 36 && out.indices.size() == 108, "triangle count conserved (3 cubes, 36)");
        const f64 vol = signedVolume(out);
        check(std::fabs(vol - (1.0 + 24.0 + 1.0)) < 1e-3, "volume conserved incl. a 2x3x4 scale and a mirror (got " + std::to_string(vol) + ")");
        check(finite(out) && out.vertexCount() > 0 && out.normals.size() == out.positions.size() && out.uvs.size() == out.vertexCount() * 2, "streams consistent, finite");
        check(out.boundsMax.x >= 12.0f - 1e-4f && out.boundsMin.x <= -1.0f + 1e-4f, "bounds computed from the baked vertices");
        // normals of the mirrored cube still point outward
        bool outward = true;
        for (usize t = 0; t + 2 < out.indices.size(); t += 3) {
            const f32* p = &out.positions[out.indices[t] * 3];
            const f32* n = &out.normals[out.indices[t] * 3];
            const f32* q = &out.positions[out.indices[t + 1] * 3];
            const f32* r = &out.positions[out.indices[t + 2] * 3];
            const f64 ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2], fx = r[0] - p[0], fy = r[1] - p[1], fz = r[2] - p[2];
            const f64 cx = ey * fz - ez * fy, cy = ez * fx - ex * fz, cz = ex * fy - ey * fx;
            if (cx * n[0] + cy * n[1] + cz * n[2] <= 0) outward = false;
        }
        check(outward, "every vertex normal agrees with its triangle winding (mirror handled)");
        fmt::OcMeshData again;
        fmt::mergeStaticMeshes(items, {}, again);
        check(again.positions == out.positions && again.indices == out.indices && again.normals == out.normals && again.uvs == out.uvs, "deterministic: same input, same output");
    }
    {   // materials
        const std::vector<fmt::OcMergeItem> items = {at(&stone, 0, 0, 0), at(&wood, 5, 0, 0), at(&stone, 10, 0, 0), at(&stone, 15, 0, 0, 1, 1, 1, "glass")};
        fmt::OcMeshData out;
        fmt::mergeStaticMeshes(items, {}, out);
        check(out.submeshes.size() == 3 && out.materialSlots == std::vector<std::string>({"stone", "wood", "glass"}), "grouped by material name in first-seen order; override applied");
        check(out.submeshes[0].indexCount == 72 && out.submeshes[1].indexCount == 36 && out.submeshes[2].indexCount == 36, "two stone cubes share one submesh");
        check(fmt::submeshesPartitionIndices(out), "submeshes partition the index buffer");
        check(out.submeshes[1].indexStart == 72 && out.submeshes[2].indexStart == 108, "submesh ranges are contiguous");
    }
    {   // weld + origin
        const std::vector<fmt::OcMergeItem> items = {at(&stone, 0, 0, 0), at(&stone, 1, 0, 0)};
        fmt::OcMeshData out; fmt::OcMergeReport rep;
        fmt::mergeStaticMeshes(items, {}, out, &rep);
        check(rep.verticesBeforeWeld == 48 && rep.verticesOut == 48, "cubes with per-face normals share no welded vertex across different normals");
        const std::vector<fmt::OcMergeItem> twins = {at(&stone, 0, 0, 0), at(&stone, 0, 0, 0)};
        fmt::mergeStaticMeshes(twins, {}, out, &rep);
        check(rep.verticesOut == 24 && rep.trianglesOut == 24, "coincident copies weld to one set of vertices (the triangles stay)");
        fmt::OcMergeOptions o; o.origin[0] = 1000000.0; o.origin[1] = 5.0; o.origin[2] = 0;
        std::vector<fmt::OcMergeItem> far = {at(&stone, 1000000.0, 5.0, 0)};
        fmt::mergeStaticMeshes(far, o, out);
        check(out.boundsMin.x == 0.0f && out.boundsMax.x == 1.0f && out.boundsMin.y == 0.0f, "origin keeps far-away geometry exact in f32");
    }
    {   // drops
        fmt::OcMeshData tiny = cube("stone");
        const std::vector<fmt::OcMergeItem> items = {at(&stone, 0, 0, 0, 100, 100, 100), at(&tiny, 10, 10, 10, 0.1, 0.1, 0.1), at(&tiny, 500, 0, 0, 1, 1, 1)};
        fmt::OcMeshData out; fmt::OcMergeReport rep;
        fmt::OcMergeOptions o; o.dropSmallerCm = 1.0f;
        fmt::mergeStaticMeshes(items, o, out, &rep);
        check(rep.droppedSmall == 1 && rep.merged == 2, "dropSmallerCm removes the tiny instance");
        o = {}; o.dropEnclosed = true;
        fmt::mergeStaticMeshes(items, o, out, &rep);
        check(rep.droppedEnclosed == 1, "dropEnclosed removes the instance inside the big one");
        check(rep.merged == 2, "the free-standing cube survives");
        std::vector<fmt::OcMergeItem> bad = {fmt::OcMergeItem{}};
        check(!fmt::mergeStaticMeshes(bad, {}, out, &rep) && rep.skipped == 1, "an item with no mesh is skipped, and nothing to merge reports false");
    }
#if AVER_MESHMERGE_TRIFACTOR
    {   // budget
        fmt::OcMeshData grid;
        const int N = 40;
        for (int y = 0; y <= N; ++y)
            for (int x = 0; x <= N; ++x) {
                grid.positions.insert(grid.positions.end(), {f32(x), f32(y), 0.2f * std::sin(0.4f * x) * std::cos(0.3f * y)});
                grid.normals.insert(grid.normals.end(), {0, 0, 1});
                grid.uvs.insert(grid.uvs.end(), {f32(x) / N, f32(y) / N});
            }
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const u32 a = y * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
                grid.indices.insert(grid.indices.end(), {a, b, d, a, d, c});
            }
        grid.materialSlots = {"moss"};
        fmt::OcMeshSubmesh sm; sm.name = "moss"; sm.indexCount = static_cast<u32>(grid.indices.size()); sm.vertexCount = grid.vertexCount();
        grid.submeshes = {sm};
        grid.computeBounds();
        // two placements of the grid, two materials: 6400 triangles
        std::vector<fmt::OcMergeItem> items = {at(&grid, 0, 0, 0), at(&grid, 100, 0, 0, 1, 1, 1, "rock")};
        fmt::OcMeshData merged;
        fmt::mergeStaticMeshes(items, {}, merged);
        const u32 before = static_cast<u32>(merged.indices.size() / 3);
        u32 got = 0; std::string why;
        const bool ok = trifactor::simplifyToBudget(merged, 1200, &got, &why);
        check(ok && got <= 1320 && got == merged.indices.size() / 3, "budget respected: " + std::to_string(before) + " -> " + std::to_string(got) + " (budget 1200) " + why);
        check(merged.materialSlots == std::vector<std::string>({"moss", "rock"}) && merged.submeshes.size() == 2 && fmt::submeshesPartitionIndices(merged),
              "material slots and ranges preserved");
        check(merged.submeshes[0].indexCount > 0 && merged.submeshes[1].indexCount > 0 && finite(merged), "both materials keep triangles; no NaN");
        u32 under = 0;
        fmt::OcMeshData small = cube("stone");
        check(trifactor::simplifyToBudget(small, 100, &under) && under == 12, "a mesh already within budget is untouched");
    }
#endif

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
