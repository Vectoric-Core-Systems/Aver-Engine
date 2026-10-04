// LightmapUnwrapTest -- the dependency-free chart-by-normal lightmap unwrapper.
//
// THE PACKER IS THE POINT. A chart-by-normal grouping that is wrong in some subtle way still LOOKS
// like a lightmap unwrap; two charts that silently overlap in the atlas do not show up until an
// author bakes a level and sees one object's lighting smeared across another's. So the central check
// here is not "did it run" but "do the charts it produced actually not collide" -- see the
// occupancy grid below, which is the one assertion that would fail if the packer degenerated into
// stacking every chart at the origin.
//
// THAT GRID IS ONE CELL PER TEXEL, AND IT WAS NOT ALWAYS. It used a fixed 64x64 grid, which over a
// 256-texel atlas is 4 texels per cell against a 2-texel gutter -- so two charts the packer had
// separated correctly still shared a boundary cell, and the test reported an overlap that did not
// exist. It failed for two days against a packer that was right. A grid coarser than the separation
// it checks for cannot answer the question it asks, and a test that fails on correct code teaches
// people to ignore it.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/LightmapUnwrap.hpp"

#include <cmath>
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

namespace {

// A single quad in the Z=const plane (normal +Z), corners CCW from (0,0). Deliberately non-square
// (200 x 100 cm) so a packer that silently swapped width and height would still be visible in the
// UV span check rather than accidentally passing by symmetry.
struct Mesh {
    std::vector<f32> positions;
    std::vector<f32> normals;
    std::vector<u32> indices;
};

Mesh makeQuad(f32 w, f32 h, f32 z) {
    Mesh m;
    const f32 p[4][3] = {{0, 0, z}, {w, 0, z}, {w, h, z}, {0, h, z}};
    for (auto& v : p) { m.positions.insert(m.positions.end(), {v[0], v[1], v[2]}); m.normals.insert(m.normals.end(), {0, 0, 1}); }
    for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(i);
    return m;
}

// Two quads on the SAME plane (both normal +Z, both Z=0) but nowhere near each other and sharing no
// vertex index -- the case a chart-by-normal-ALONE unwrapper gets wrong by merging them into one
// chart just because their normals agree, and connected-components is what has to keep them apart.
Mesh makeTwoDisconnectedCoplanarQuads() {
    Mesh a = makeQuad(100, 100, 0);
    Mesh b = makeQuad(100, 100, 0);
    // Slide b far from a so their footprints cannot be confused for one shape.
    for (usize i = 0; i < b.positions.size(); i += 3) b.positions[i + 0] += 500.0f;

    Mesh m = a;
    const u32 base = static_cast<u32>(a.positions.size() / 3);
    m.positions.insert(m.positions.end(), b.positions.begin(), b.positions.end());
    m.normals.insert(m.normals.end(), b.normals.begin(), b.normals.end());
    for (u32 i : b.indices) m.indices.push_back(base + i);
    return m;
}

// A unit-ish cube, 24 vertices (4 per face, no vertex shared across faces -- the same shape
// tests/formats/src/MeshTest.cpp's makeCube() uses, and for the identical reason: it is what a real
// hard-surface export looks like, one flat-shaded quad per face rather than one smoothed corner
// vertex serving three faces). Face order matches LightmapUnwrap's own axis numbering (+X,-X,+Y,-Y,
// +Z,-Z) so a test failure can name which face's chart went missing.
struct CubeMesh : Mesh {
    // vertex index range [faceFirstVertex[f], faceFirstVertex[f]+4) belongs to face f.
    u32 faceFirstVertex[6] = {};
};

CubeMesh makeCube(f32 half) {
    CubeMesh m;
    const f32 n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const f32 p[6][4][3] = {
        {{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1},{ 1,-1, 1}},   // +X
        {{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},{-1,-1,-1}},   // -X
        {{-1, 1,-1},{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1}},   // +Y
        {{-1,-1, 1},{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1}},   // -Y
        {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}},   // +Z
        {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},   // -Z
    };
    for (int f = 0; f < 6; ++f) {
        m.faceFirstVertex[f] = static_cast<u32>(m.positions.size() / 3);
        const u32 base = m.faceFirstVertex[f];
        for (int v = 0; v < 4; ++v) {
            m.positions.insert(m.positions.end(), {p[f][v][0] * half, p[f][v][1] * half, p[f][v][2] * half});
            m.normals.insert(m.normals.end(), {n[f][0], n[f][1], n[f][2]});
        }
        for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    return m;
}

// UV bounding box over a run of TRIANGLES, read through the result's own remapped index buffer.
//
// NOT A RANGE OF INPUT VERTICES, which is what this used to take. The unwrapper rebuilds the vertex
// list -- one copy of a vertex per chart that touches it -- so `uv` is addressed by OUTPUT index and
// an input vertex number means nothing here any more. Going through r.indices is also the only
// formulation that stays correct once a vertex IS shared between charts: it asks "what did this
// triangle's corners get", which is the question, rather than "what did vertex 7 get", which stops
// having one answer.
struct Box { f32 minU, minV, maxU, maxV; };

Box uvBoxOfTriangles(const fmt::UnwrapResult& r, u32 firstTri, u32 triCount) {
    Box b{1.0f, 1.0f, 0.0f, 0.0f};
    for (u32 t = firstTri; t < firstTri + triCount; ++t) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = r.indices[t * 3 + k];
            const f32 u = r.uv[v * 2 + 0], vv = r.uv[v * 2 + 1];
            b.minU = std::min(b.minU, u); b.maxU = std::max(b.maxU, u);
            b.minV = std::min(b.minV, vv); b.maxV = std::max(b.maxV, vv);
        }
    }
    return b;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("LightmapUnwrap");

    AVER_INFO("=== a single quad unwraps to one chart, sensibly sized ===");
    {
        const Mesh q = makeQuad(200, 100, 0);
        fmt::UnwrapResult r;
        std::string err;
        const bool ok = fmt::unwrapForLightmap(q.positions.data(), q.normals.data(), 4,
                                                q.indices.data(), static_cast<u32>(q.indices.size()),
                                                256, r, &err);
        check(ok, "the quad unwraps: " + err);
        check(r.chartCount == 1, "exactly one chart, got " + std::to_string(r.chartCount));
        check(r.atlasWidth == 256 && r.atlasHeight == 256, "the atlas is exactly the requested edge");

        const Box b = uvBoxOfTriangles(r, 0, 2);
        const f32 spanU = b.maxU - b.minU, spanV = b.maxV - b.minV;
        // "Sensible" means the packer did not shrink a lone, uncontested chart down to a speck --
        // with nothing else competing for atlas space it should occupy a large fraction of it.
        check(spanU > 0.3f && spanV > 0.3f,
              "its UV footprint covers a real fraction of the atlas (u=" + std::to_string(spanU) +
                  ", v=" + std::to_string(spanV) + ")");
    }

    AVER_INFO("=== a cube produces at least one chart per face direction ===");
    fmt::UnwrapResult cubeResult;
    const CubeMesh cube = makeCube(50.0f);
    {
        std::string err;
        const bool ok = fmt::unwrapForLightmap(cube.positions.data(), cube.normals.data(),
                                                static_cast<u32>(cube.positions.size() / 3),
                                                cube.indices.data(), static_cast<u32>(cube.indices.size()),
                                                512, cubeResult, &err);
        check(ok, "the cube unwraps: " + err);
        check(cubeResult.chartCount >= 6,
              "at least 6 charts, one per face direction -- got " + std::to_string(cubeResult.chartCount) +
                  " (a collapse into fewer means opposite faces were merged)");
    }

    AVER_INFO("=== every returned UV is inside [0,1] ===");
    {
        bool allInRange = true;
        for (f32 c : cubeResult.uv) if (c < 0.0f || c > 1.0f) allInRange = false;
        check(allInRange, "all " + std::to_string(cubeResult.uv.size()) + " cube UV components are in [0,1]");
    }

    AVER_INFO("=== no two charts overlap in the atlas ===");
    {
        // Rasterise each of the cube's 6 known face groups (see makeCube) into an occupancy grid
        // over the atlas and fail the instant two DIFFERENT faces claim the same cell. This is the
        // assertion that actually exercises the shelf packer: chart-by-normal alone guarantees 6
        // SEPARATE charts, but says nothing about whether the packer then placed them on top of
        // each other.
        //
        // ONE CELL PER TEXEL, and that is not fussiness -- it is the difference between this test
        // measuring the packer and measuring itself. It used to use a fixed 64x64 grid, which over
        // a 256-texel atlas is 4 texels per cell while the packer's gutter is 2. Two charts placed
        // correctly, with the full gutter between them, still landed in one shared boundary cell and
        // the test reported an overlap that was not there. A grid coarser than the separation it is
        // checking for cannot answer the question it is asking.
        const u32 gw = cubeResult.atlasWidth, gh = cubeResult.atlasHeight;
        std::vector<int> owner(static_cast<usize>(gw) * gh, -1);
        bool overlap = false;
        std::string firstClash;
        for (int f = 0; f < 6; ++f) {
            const Box b = uvBoxOfTriangles(cubeResult, static_cast<u32>(f) * 2, 2);
            const u32 cx0 = static_cast<u32>(b.minU * gw), cx1 = std::min(gw - 1, static_cast<u32>(b.maxU * gw));
            const u32 cy0 = static_cast<u32>(b.minV * gh), cy1 = std::min(gh - 1, static_cast<u32>(b.maxV * gh));
            for (u32 cy = cy0; cy <= cy1; ++cy) {
                for (u32 cx = cx0; cx <= cx1; ++cx) {
                    int& cell = owner[cy * gw + cx];
                    if (cell != -1 && cell != f) {
                        if (!overlap)
                            firstClash = " (texel " + std::to_string(cx) + "," + std::to_string(cy) +
                                         " claimed by face " + std::to_string(cell) + " and face " +
                                         std::to_string(f) + ")";
                        overlap = true;
                    }
                    cell = f;
                }
            }
        }
        check(!overlap, "no atlas texel is claimed by two different cube faces" + firstClash);
    }

    AVER_INFO("=== two coplanar but disconnected quads produce two charts ===");
    {
        const Mesh two = makeTwoDisconnectedCoplanarQuads();
        fmt::UnwrapResult r;
        std::string err;
        const bool ok = fmt::unwrapForLightmap(two.positions.data(), two.normals.data(),
                                                static_cast<u32>(two.positions.size() / 3),
                                                two.indices.data(), static_cast<u32>(two.indices.size()),
                                                256, r, &err);
        check(ok, "the two-quad mesh unwraps: " + err);
        check(r.chartCount == 2,
              "two charts, not one collapsed by sharing a normal -- got " + std::to_string(r.chartCount));
    }

    AVER_INFO("=== malformed input is refused, not crashed on ===");
    {
        fmt::UnwrapResult r;
        std::string err;
        const u32 badIndex[3] = {0, 1, 99};
        const f32 pos[9] = {0,0,0, 1,0,0, 0,1,0};
        check(!fmt::unwrapForLightmap(pos, pos, 3, badIndex, 3, 64, r, &err),
              "an index past vertexCount is refused");
        check(!err.empty(), "  ...and says why: " + err);

        const u32 okIndex[3] = {0, 1, 2};
        check(!fmt::unwrapForLightmap(pos, pos, 3, okIndex, 3, 3, r, &err),
              "an atlas edge too small for its own gutter is refused");
    }

    AVER_INFO("==================================================");
    AVER_INFO("RESULT: {}", g_failures == 0 ? "PASS" : "FAIL");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
