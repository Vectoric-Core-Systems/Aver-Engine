// MeshletTest -- the mesh-shader path's geometry (rhi/Meshlets.hpp), with no device.
//
// The buffer is read by ASMain and MSMain (shared_prelude.hlsl, AVER_MS) and nothing checks it on the GPU, so every
// property they rely on is asserted here: the layout, the limits, every triangle exactly once and in order, out-of-range
// triangles dropped, bounds that hold every vertex, and the frustum test (mirrored from ASMain) never culling a meshlet
// that has a vertex inside the side planes.
//
// Links Aver.RHI, Aver.Core and Aver.Platform like WorldShiftTest and nothing else.
#include "aver/core/Log.hpp"
#include "aver/rhi/Meshlets.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::rhi;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {
struct Vtx { f32 p[3]; f32 pad[5]; };   // MeshVertex's stride, position first

f32 asFloat(u32 u) { f32 f; std::memcpy(&f, &u, sizeof f); return f; }

// A w x h grid of quads in the z = 0 plane, in row order, or in tile x tile patches when `tile` is non-zero.
void grid(u32 w, u32 h, std::vector<Vtx>& v, std::vector<u32>& idx, u32 tile = 0) {
    v.clear(); idx.clear();
    for (u32 y = 0; y <= h; ++y)
        for (u32 x = 0; x <= w; ++x) v.push_back({{f32(x), f32(y), 0.0f}, {}});
    const auto quad = [&](u32 x, u32 y) {
        const u32 a = y * (w + 1) + x, b = a + 1, c = a + w + 1, d = c + 1;
        idx.insert(idx.end(), {a, b, c, b, d, c});
    };
    const u32 t = tile ? tile : w;
    for (u32 ty = 0; ty < h; ty += (tile ? tile : 1))
        for (u32 tx = 0; tx < w; tx += t)
            for (u32 y = ty; y < std::min(h, ty + (tile ? tile : 1)); ++y)
                for (u32 x = tx; x < std::min(w, tx + t); ++x) quad(x, y);
}

f32 verticesPerTriangle(const MeshletBuffer& mb, size_t indexCount) {
    u32 verts = 0;
    for (u32 i = 0; i < mb.meshletCount; ++i) verts += mb.words[static_cast<size_t>(i) * kMeshletDescDwords + 2] & 0xFF;
    return f32(verts) / f32(indexCount / 3);
}

struct Decoded { u32 vc, tc; f32 centre[3], radius; std::vector<u32> globals; std::vector<u32> tris; };

// Reads the buffer the way MSMain does, checking every word it touches is in range.
bool decode(const MeshletBuffer& mb, std::vector<Decoded>& out, std::string& why) {
    out.clear();
    const u32 n = mb.meshletCount, size = static_cast<u32>(mb.words.size());
    if (size < n * kMeshletDescDwords) { why = "buffer shorter than its descriptors"; return false; }
    for (u32 i = 0; i < n; ++i) {
        const u32* d = &mb.words[static_cast<size_t>(i) * kMeshletDescDwords];
        Decoded m;
        m.vc = d[2] & 0xFF; m.tc = (d[2] >> 8) & 0xFF;
        if (d[3] != 0 || (d[2] >> 16) != 0) { why = "reserved bits set in meshlet " + std::to_string(i); return false; }
        if (d[0] < n * kMeshletDescDwords || d[0] + m.vc > size || d[1] + m.tc > size) {
            why = "meshlet " + std::to_string(i) + " points outside the buffer"; return false;
        }
        for (int a = 0; a < 3; ++a) m.centre[a] = asFloat(d[4 + a]);
        m.radius = asFloat(d[7]);
        m.globals.assign(mb.words.begin() + d[0], mb.words.begin() + d[0] + m.vc);
        m.tris.assign(mb.words.begin() + d[1], mb.words.begin() + d[1] + m.tc);
        out.push_back(std::move(m));
    }
    return true;
}

// Every valid triangle of `idx`, in order, as the decoded meshlets give them back.
bool roundTrips(const std::vector<Decoded>& ms, const std::vector<u32>& idx, u32 vertexCount, std::string& why) {
    std::vector<u32> want, got;
    for (size_t t = 0; t + 3 <= idx.size(); t += 3)
        if (idx[t] < vertexCount && idx[t + 1] < vertexCount && idx[t + 2] < vertexCount)
            want.insert(want.end(), {idx[t], idx[t + 1], idx[t + 2]});
    for (const Decoded& m : ms)
        for (u32 p : m.tris) {
            const u32 l[3] = {p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF};
            if ((p >> 24) != 0 || l[0] >= m.vc || l[1] >= m.vc || l[2] >= m.vc) { why = "a local index past its meshlet"; return false; }
            for (u32 k : l) got.push_back(m.globals[k]);
        }
    if (got != want) { why = "triangles differ (" + std::to_string(got.size() / 3) + " vs " + std::to_string(want.size() / 3) + ")"; return false; }
    return true;
}

// ---- ASMain's frustum test, mirrored: row vectors, clip = p * M, planes are sums of M's columns.
using M4 = f32[4][4];
void mul(const M4 a, const M4 b, M4 o) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k) s += a[r][k] * b[k][c];
            o[r][c] = s;
        }
}
void planes(const M4 mvp, f32 out[4][4]) {
    for (int i = 0; i < 4; ++i) {
        const int axis = i / 2;
        const f32 sign = (i % 2) ? -1.0f : 1.0f;
        for (int r = 0; r < 4; ++r) out[i][r] = mvp[r][3] + sign * mvp[r][axis];
    }
}
bool inFrustum(const f32 pl[4][4], const f32 c[3], f32 r) {
    for (int i = 0; i < 4; ++i) {
        const f32 len = std::sqrt(pl[i][0] * pl[i][0] + pl[i][1] * pl[i][1] + pl[i][2] * pl[i][2]);
        if (len > 1e-20f && pl[i][0] * c[0] + pl[i][1] * c[1] + pl[i][2] * c[2] + pl[i][3] < -r * len) return false;
    }
    return true;
}
} // namespace

static void testGridLayout() {
    AVER_INFO("grid: layout, limits and round trip");
    std::vector<Vtx> v; std::vector<u32> idx;
    grid(100, 60, v, idx);
    const u32 vc = static_cast<u32>(v.size());
    const MeshletBuffer mb = buildMeshlets(idx.data(), static_cast<u32>(idx.size()), vc, v[0].p, sizeof(Vtx));
    std::vector<Decoded> ms; std::string why;
    check(mb.meshletCount > 0 && decode(mb, ms, why), "the buffer decodes " + why);
    bool limits = true;
    for (const Decoded& m : ms)
        limits = limits && m.vc >= 1 && m.vc <= kMeshletMaxVertices && m.tc >= 1 && m.tc <= kMeshletMaxTriangles;
    check(limits, "every meshlet holds 1.." + std::to_string(kMeshletMaxVertices) + " vertices and 1.." +
                      std::to_string(kMeshletMaxTriangles) + " triangles");
    check(roundTrips(ms, idx, vc, why), "every triangle comes back once, in order " + why);
    // Three per triangle was the old path's cost. Index order is kept, so a long row strip reuses only along the row
    // and compact patches reuse in both directions.
    const f32 rowOrder = verticesPerTriangle(mb, idx.size());
    check(rowOrder < 1.1f, "vertices shaded per triangle, row-order grid: " + std::to_string(rowOrder) + " (< 1.1, was 3)");
    std::vector<Vtx> tv; std::vector<u32> tidx;
    grid(96, 96, tv, tidx, 6);
    const MeshletBuffer tiled = buildMeshlets(tidx.data(), static_cast<u32>(tidx.size()), static_cast<u32>(tv.size()), tv[0].p, sizeof(Vtx));
    const f32 patches = verticesPerTriangle(tiled, tidx.size());
    check(patches < 0.75f, "vertices shaded per triangle, 6x6-patch grid: " + std::to_string(patches) + " (< 0.75; greedy fill splits patches)");
    bool contained = true;
    for (const Decoded& m : ms)
        for (u32 g : m.globals) {
            const f32 dx = v[g].p[0] - m.centre[0], dy = v[g].p[1] - m.centre[1], dz = v[g].p[2] - m.centre[2];
            contained = contained && std::sqrt(dx * dx + dy * dy + dz * dz) <= m.radius;
        }
    check(contained, "every meshlet's sphere holds its vertices");
}

static void testEdges() {
    AVER_INFO("edges: bad indices, no positions, degenerate input");
    std::vector<Vtx> v; std::vector<u32> idx;
    grid(4, 4, v, idx);
    const u32 vc = static_cast<u32>(v.size());
    std::vector<u32> bad = idx;
    bad[4] = vc + 7;   // triangle 1 names a vertex past the buffer
    MeshletBuffer mb = buildMeshlets(bad.data(), static_cast<u32>(bad.size()), vc, v[0].p, sizeof(Vtx));
    std::vector<Decoded> ms; std::string why;
    check(decode(mb, ms, why) && roundTrips(ms, bad, vc, why), "a triangle naming a vertex past the buffer is dropped " + why);

    mb = buildMeshlets(idx.data(), static_cast<u32>(idx.size()), vc, nullptr, 0);
    bool unbounded = decode(mb, ms, why) && !ms.empty();
    for (const Decoded& m : ms) unbounded = unbounded && m.radius < 0.0f;
    check(unbounded, "without positions every meshlet is unbounded (never culled)");

    check(buildMeshlets(idx.data(), 2, vc, nullptr, 0).meshletCount == 0, "fewer than three indices: no meshlets");
    check(buildMeshlets(nullptr, 6, vc, nullptr, 0).meshletCount == 0, "no indices: no meshlets");
    const u32 allBad[3] = {vc, vc + 1, vc + 2};
    check(buildMeshlets(allBad, 3, vc, nullptr, 0).meshletCount == 0, "only invalid triangles: no meshlets");
    const u32 degenerate[6] = {0, 0, 0, 1, 1, 2};
    mb = buildMeshlets(degenerate, 6, vc, v[0].p, sizeof(Vtx));
    check(decode(mb, ms, why) && ms.size() == 1 && ms[0].vc == 3 && ms[0].tc == 2 && roundTrips(ms, {0, 0, 0, 1, 1, 2}, vc, why),
          "repeated vertices in a triangle are stored once " + why);
}

static void testScattered() {
    AVER_INFO("scattered: random index order still meets the limits");
    std::mt19937 rng(7);
    std::vector<Vtx> v(5000);
    for (Vtx& x : v) for (f32& c : x.p) c = std::uniform_real_distribution<f32>(-100.0f, 100.0f)(rng);
    std::vector<u32> idx(3 * 9000);
    for (u32& i : idx) i = std::uniform_int_distribution<u32>(0, 4999)(rng);
    const MeshletBuffer mb = buildMeshlets(idx.data(), static_cast<u32>(idx.size()), 5000, v[0].p, sizeof(Vtx));
    std::vector<Decoded> ms; std::string why;
    bool limits = decode(mb, ms, why);
    for (const Decoded& m : ms) limits = limits && m.vc <= kMeshletMaxVertices && m.tc <= kMeshletMaxTriangles;
    check(limits, "worst-case vertex reuse: every meshlet within the limits " + why);
    check(roundTrips(ms, idx, 5000, why), "worst-case vertex reuse: every triangle once, in order " + why);
}

static void testFrustum() {
    AVER_INFO("frustum: a culled meshlet has every vertex outside one side plane");
    std::vector<Vtx> v; std::vector<u32> idx;
    grid(64, 64, v, idx);
    for (Vtx& x : v) x.p[2] = 3.0f * std::sin(0.3f * x.p[0]) * std::cos(0.2f * x.p[1]);
    const MeshletBuffer mb = buildMeshlets(idx.data(), static_cast<u32>(idx.size()), static_cast<u32>(v.size()), v[0].p, sizeof(Vtx));
    std::vector<Decoded> ms; std::string why;
    decode(mb, ms, why);
    std::mt19937 rng(11);
    std::uniform_real_distribution<f32> u(-1.0f, 1.0f);
    u32 culled = 0, kept = 0, wrong = 0;
    for (int trial = 0; trial < 200; ++trial) {
        // A world with rotation, non-uniform scale, shear and a mirror: the planes are taken through it, never a scale estimate.
        M4 world = {{u(rng) * 2, u(rng), u(rng), 0}, {u(rng), u(rng) * 2, u(rng), 0}, {u(rng), u(rng), u(rng) * 2, 0},
                    {u(rng) * 50, u(rng) * 50, u(rng) * 50, 1}};
        if (trial % 3 == 0) for (int c = 0; c < 3; ++c) world[0][c] = -world[0][c];
        // A perspective projection (row vectors, w = view z) looking down +z from a random offset.
        const f32 f = 1.0f + 2.0f * (u(rng) + 1.0f), aspect = 1.0f + 0.5f * (u(rng) + 1.0f);
        M4 proj = {{f / aspect, 0, 0, 0}, {0, f, 0, 0}, {0, 0, 1.0f, 1.0f}, {u(rng) * 20, u(rng) * 20, 0.1f, 60.0f + 40.0f * u(rng)}};
        M4 mvp; mul(world, proj, mvp);
        f32 pl[4][4]; planes(mvp, pl);
        for (const Decoded& m : ms) {
            if (inFrustum(pl, m.centre, m.radius)) { ++kept; continue; }
            ++culled;
            bool someOutside = false;
            for (int i = 0; i < 4 && !someOutside; ++i) {
                bool all = true;
                for (u32 g : m.globals) {
                    const f32* p = v[g].p;
                    all = all && pl[i][0] * p[0] + pl[i][1] * p[1] + pl[i][2] * p[2] + pl[i][3] < 0.0f;
                }
                someOutside = all;
            }
            wrong += !someOutside;
        }
    }
    check(culled > 0 && kept > 0, "the trials cull some meshlets and keep others (" + std::to_string(culled) + " culled, " +
                                      std::to_string(kept) + " kept)");
    check(wrong == 0, "no culled meshlet has a vertex inside every side plane (" + std::to_string(wrong) + " wrong)");
}

int main() {
    AVER_INFO("MeshletTest -- the mesh-shader path's meshlet buffer");
    testGridLayout();
    testEdges();
    testScattered();
    testFrustum();
    if (g_failures == 0) {
        AVER_INFO("MeshletTest: {} checks, all passed", g_checks);
        return 0;
    }
    AVER_ERROR("MeshletTest: {} of {} checks FAILED", g_failures, g_checks);
    return 1;
}
