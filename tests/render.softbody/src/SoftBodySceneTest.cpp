// softBodyPackVertices: the pure half of the soft-body draw path, and the only half a headless test
// can reach -- see tests/render.softbody/CMakeLists.txt for why the class itself is out of scope
// here. Shape copied from tests/physics/src/SoftBodyTest.cpp deliberately, for the same reason that
// file gives: one house style for "prints one line per assertion and returns the failure count".
#include "aver/render/SoftBodyScene.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::render;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool nearf(f32 a, f32 b, f32 eps = 1e-3f) { return std::abs(a - b) <= eps; }

// ---- the world-to-local space conversion ------------------------------------------------------------
//
// THE TRAP THIS WHOLE TEST EXISTS TO CATCH: a worldToLocal built from ONLY a translation, or ONLY a
// rotation, cannot tell a row-vector transform from a column-vector one -- for a pure translation
// both conventions add the same offset, and for a pure rotation about a point through the origin
// both conventions agree wherever the point being transformed sits on the rotation's own axis. Only
// a matrix carrying BOTH, applied to a point off every axis of the rotation, produces genuinely
// different answers under the two conventions. This builds exactly that matrix and computes the
// expected answer with its OWN loop -- written independently of softBodyPackVertices, implementing
// only the formula the header documents -- rather than by calling the function twice or reusing
// Mat4::operator*.
static void testWorldToLocalIsRowVector() {
    AVER_INFO("-- worldToLocal is applied as a row-vector transform --");

    // 90 degrees about +Z (engine axes: +X forward, +Y right, +Z up), then a translation, combined
    // as R * T so that under row-vector convention a point is rotated FIRST and translated SECOND.
    // (1.57079633f is pi/2 in radians, spelled as a literal rather than M_PI, which MSVC only
    // defines behind _USE_MATH_DEFINES.)
    const Quat q = Quat::fromAxisAngle(Vec3{0, 0, 1}, 1.57079633f);
    const Mat4 R = Mat4::fromQuat(q);
    const Mat4 T = Mat4::translation(Vec3{10.0f, -5.0f, 20.0f});
    const Mat4 worldToLocal = R * T;

    // A point off the rotation axis in all of x and y, so rotation actually moves it, and with a
    // nonzero z so a bug that swapped which axis carries the translation would still be caught.
    const f32 world[3] = {3.0f, 7.0f, 11.0f};

    // The independent reference: local[j] = sum_i(world[i] * M.m[i][j]) + M.m[3][j]. This is the
    // formula from SoftBodyScene.hpp, transcribed fresh rather than shared with the production code.
    f32 expected[3] = {0, 0, 0};
    for (int j = 0; j < 3; ++j) {
        f32 v = worldToLocal.m[3][j];
        for (int i = 0; i < 3; ++i) v += world[i] * worldToLocal.m[i][j];
        expected[j] = v;
    }

    rhi::MeshVertex out[1]{};
    const u32 tri[3] = {0, 0, 0};   // one degenerate "triangle" so the normals pass has something to
                                     // walk without needing a real shape; irrelevant to this check.
    softBodyPackVertices(world, 1, worldToLocal, tri, 3, nullptr, out);

    check(nearf(out[0].px, expected[0]) && nearf(out[0].py, expected[1]) && nearf(out[0].pz, expected[2]),
          "the packed position matches the row-vector formula (" +
          std::to_string(out[0].px) + "," + std::to_string(out[0].py) + "," + std::to_string(out[0].pz) +
          " vs expected " + std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," +
          std::to_string(expected[2]) + ")");

    // The column-vector answer this same matrix and point would give, so a failure of the check
    // above is legible: if it lands HERE instead, the bug is exactly the transpose swap the header
    // warns about.
    f32 wrong[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        f32 v = worldToLocal.m[i][3];
        for (int j = 0; j < 3; ++j) v += worldToLocal.m[i][j] * world[j];
        wrong[i] = v;
    }
    check(!(nearf(out[0].px, wrong[0]) && nearf(out[0].py, wrong[1]) && nearf(out[0].pz, wrong[2])),
          "and it is not the column-vector answer for the same matrix and point");
}

// ---- normals on a shape whose correct normal is known by construction --------------------------------
static void testNormalsOnFlatAndRotatedQuad() {
    AVER_INFO("-- normals on a flat quad, and a rotated one --");

    // A unit quad in the XY plane, wound so cross(B-A, C-A) points +Z -- the same winding
    // softBodyPackVertices itself uses, so this is the shape's own geometry proving the normal,
    // not an assumption about the implementation's convention.
    const f32 flat[4 * 3] = {
        0, 0, 0,   // A
        1, 0, 0,   // B
        1, 1, 0,   // C
        0, 1, 0,   // D
    };
    const u32 idx[6] = {0, 1, 2,  0, 2, 3};
    rhi::MeshVertex out[4]{};
    softBodyPackVertices(flat, 4, Mat4::identity(), idx, 6, nullptr, out);
    for (u32 i = 0; i < 4; ++i)
        check(nearf(out[i].nx, 0, 0.01f) && nearf(out[i].ny, 0, 0.01f) && nearf(out[i].nz, 1, 0.01f),
              "flat quad vertex " + std::to_string(i) + " got the plane normal (0,0,1), got (" +
              std::to_string(out[i].nx) + "," + std::to_string(out[i].ny) + "," +
              std::to_string(out[i].nz) + ")");

    // The SAME quad, rotated 90 degrees about X: (x,y,z) -> (x,-z,y). Its plane normal must rotate
    // the same way, from (0,0,1) to (0,-1,0) -- computed here by hand, not by asking the library to
    // rotate a vector and then asking it again whether the two answers agree with each other.
    const f32 rotated[4 * 3] = {
        0, 0, 0,   // A'
        1, 0, 0,   // B'
        1, 0, 1,   // C'
        0, 0, 1,   // D'
    };
    rhi::MeshVertex out2[4]{};
    softBodyPackVertices(rotated, 4, Mat4::identity(), idx, 6, nullptr, out2);
    for (u32 i = 0; i < 4; ++i)
        check(nearf(out2[i].nx, 0, 0.01f) && nearf(out2[i].ny, -1, 0.01f) && nearf(out2[i].nz, 0, 0.01f),
              "rotated quad vertex " + std::to_string(i) + " followed the rotation to (0,-1,0), got (" +
              std::to_string(out2[i].nx) + "," + std::to_string(out2[i].ny) + "," +
              std::to_string(out2[i].nz) + ")");
}

// ---- a degenerate triangle contributes nothing, never a NaN ------------------------------------------
static void testDegenerateTriangleNoNaN() {
    AVER_INFO("-- a degenerate triangle produces no NaN --");

    // Three coincident positions: every edge is the zero vector, so the face normal is the zero
    // vector regardless of which pair of edges the implementation happens to cross.
    const f32 verts[3 * 3] = {
        5, 5, 5,
        5, 5, 5,
        5, 5, 5,
    };
    const u32 idx[3] = {0, 1, 2};
    rhi::MeshVertex out[3]{};
    softBodyPackVertices(verts, 3, Mat4::identity(), idx, 3, nullptr, out);
    for (u32 i = 0; i < 3; ++i) {
        check(!std::isnan(out[i].nx) && !std::isnan(out[i].ny) && !std::isnan(out[i].nz),
              "vertex " + std::to_string(i) + " of the degenerate triangle has no NaN in its normal");
        check(nearf(out[i].nx, 0) && nearf(out[i].ny, 0) && nearf(out[i].nz, 0),
              "and it contributed nothing rather than some spurious direction");
    }
}

// ---- a shared vertex gets the BLENDED normal, not the last one written -------------------------------
static void testSharedVertexGetsBlendedNormal() {
    AVER_INFO("-- a vertex shared by two triangles is blended, not overwritten --");

    // V0 is shared. Triangle A = (V0,V1,V2) has raw (unnormalised) face normal (0,0,1); triangle B
    // = (V0,V3,V1) has raw face normal (0,1,0) -- reusing V1 rather than adding a fifth vertex, so
    // the only thing that differs between the triangles is which face they belong to, not which
    // vertices exist. Their SUM is (0,1,1), which normalises to (0, 1/root2, 1/root2): a direction
    // equal to neither triangle's own normal, so it cannot be produced by an implementation that
    // just overwrites V0's normal with whichever triangle it processes last (or first).
    const f32 verts[4 * 3] = {
        0, 0, 0,   // V0
        1, 0, 0,   // V1
        0, 1, 0,   // V2
        0, 0, 1,   // V3
    };
    const u32 idx[6] = {0, 1, 2,   0, 3, 1};
    rhi::MeshVertex out[4]{};
    softBodyPackVertices(verts, 4, Mat4::identity(), idx, 6, nullptr, out);

    const f32 expect = 1.0f / std::sqrt(2.0f);
    check(nearf(out[0].nx, 0, 0.01f) && nearf(out[0].ny, expect, 0.01f) && nearf(out[0].nz, expect, 0.01f),
          "the shared vertex got the averaged normal (0, 0.707, 0.707), got (" +
          std::to_string(out[0].nx) + "," + std::to_string(out[0].ny) + "," + std::to_string(out[0].nz) + ")");
    check(!(nearf(out[0].ny, 0, 0.05f) && nearf(out[0].nz, 1, 0.05f)),
          "and it is not just triangle A's own normal (0,0,1)");
    check(!(nearf(out[0].ny, 1, 0.05f) && nearf(out[0].nz, 0, 0.05f)),
          "nor just triangle B's own normal (0,1,0)");
}

// ---- normals are RECOMPUTED from this call's positions, never carried from a rest value --------------
//
// softBodyPackVertices takes no source-normal parameter at all, so the only way a "copied from rest"
// bug could exist is an implementation that caches its first answer and returns it again regardless
// of what positions arrive on a later call. This exercises exactly that: the same topology, called
// once at rest and once deformed, checked at a vertex whose OWN position never moves -- so any change
// in ITS normal can only be explained by the deformation of its NEIGHBOUR having actually been read.
static void testNormalsAreRecomputedNotCached() {
    AVER_INFO("-- normals are recomputed from THIS call's positions, not a cached rest value --");

    const f32 rest[4 * 3] = {
        0, 0, 0,   // A
        1, 0, 0,   // B  -- never moves
        1, 1, 0,   // C
        0, 1, 0,   // D
    };
    const u32 idx[6] = {0, 1, 2,  0, 2, 3};
    rhi::MeshVertex restOut[4]{};
    softBodyPackVertices(rest, 4, Mat4::identity(), idx, 6, nullptr, restOut);
    check(nearf(restOut[1].nx, 0, 0.01f) && nearf(restOut[1].ny, 0, 0.01f) && nearf(restOut[1].nz, 1, 0.01f),
          "at rest, B's normal is the flat plane normal (0,0,1)");

    // Deform C upward. B's own position is untouched.
    const f32 deformed[4 * 3] = {
        0, 0, 0,   // A
        1, 0, 0,   // B  -- same three floats as above
        1, 1, 2,   // C  -- pushed up
        0, 1, 0,   // D
    };
    rhi::MeshVertex deformedOut[4]{};
    softBodyPackVertices(deformed, 4, Mat4::identity(), idx, 6, nullptr, deformedOut);

    // Expected by hand: triangle (0,1,2) now has raw face normal cross(B-A, C-A)
    // = cross((1,0,0), (1,1,2)) = (0*2-0*1, 0*1-1*2, 1*1-0*1) = (0,-2,1), so B -- touched by only
    // that one triangle -- normalises to (0,-2,1)/sqrt(5).
    const f32 mag = std::sqrt(5.0f);
    check(nearf(deformedOut[1].nx, 0, 0.01f) && nearf(deformedOut[1].ny, -2.0f / mag, 0.01f) &&
          nearf(deformedOut[1].nz, 1.0f / mag, 0.01f),
          "after deforming its NEIGHBOUR, B's recomputed normal follows (expected (0," +
          std::to_string(-2.0f / mag) + "," + std::to_string(1.0f / mag) + "), got (" +
          std::to_string(deformedOut[1].nx) + "," + std::to_string(deformedOut[1].ny) + "," +
          std::to_string(deformedOut[1].nz) + "))");
    check(!(nearf(deformedOut[1].ny, 0, 0.05f) && nearf(deformedOut[1].nz, 1, 0.05f)),
          "and B's normal is no longer the stale rest value (0,0,1) -- it was not left cached there");
}

// ---- uv passthrough, and the null fallback ------------------------------------------------------------
static void testUvPassthroughAndNullFallback() {
    AVER_INFO("-- uvs are copied from the source, or zeroed when there is no source --");

    const f32 verts[2 * 3] = {0, 0, 0,  1, 1, 1};
    const u32 idx[3] = {0, 1, 1};   // degenerate on purpose; this test only cares about u,v
    const f32 uvs[2 * 2] = {0.25f, 0.75f,  0.9f, 0.1f};

    rhi::MeshVertex withUv[2]{};
    softBodyPackVertices(verts, 2, Mat4::identity(), idx, 3, uvs, withUv);
    check(nearf(withUv[0].u, 0.25f) && nearf(withUv[0].v, 0.75f), "vertex 0's uv was copied from the source");
    check(nearf(withUv[1].u, 0.9f) && nearf(withUv[1].v, 0.1f), "vertex 1's uv was copied from the source");

    rhi::MeshVertex noUv[2]{};
    softBodyPackVertices(verts, 2, Mat4::identity(), idx, 3, nullptr, noUv);
    check(nearf(noUv[0].u, 0.0f) && nearf(noUv[0].v, 0.0f), "with no source, vertex 0's uv is zero rather than uninitialised");
    check(nearf(noUv[1].u, 0.0f) && nearf(noUv[1].v, 0.0f), "and so is vertex 1's");
}

// THE GAP EVERY OTHER NORMAL TEST IN THIS FILE LEAVES OPEN, and it took a reviewer to see it: the
// five tests above all pass worldToLocal = identity, so the transformed positions are numerically
// identical to the raw world input. An implementation that accumulated face normals from
// `worldPositionsXyz` (pre-transform, WORLD space) instead of from the already-converted local
// positions would produce byte-identical output in every one of them, and pass.
//
// It would also be wrong for every rotated soft body in a real scene -- normals left in world space
// while positions are in mesh-local means the entity's own transform rotates them a second time at
// draw. This is the test that separates those two implementations: a quad whose normal is known by
// construction, packed through a worldToLocal that ROTATES, so the correct answer is the rotated
// normal and the incorrect one is the unrotated normal.
static void testNormalsAreComputedInLocalSpaceNotWorld() {
    // A quad lying flat in world Z: its world-space normal is +Z by construction.
    const f32 world[] = {
        0.0f,   0.0f, 100.0f,
        50.0f,  0.0f, 100.0f,
        50.0f, 50.0f, 100.0f,
        0.0f,  50.0f, 100.0f,
    };
    const u32 idx[] = {0, 1, 2, 0, 2, 3};

    // worldToLocal is a 90-degree rotation about +X. It takes world +Z onto local -Y (row-vector:
    // v' = v * M), so a correct implementation -- which converts positions FIRST and derives the
    // normal from those local positions -- must report a normal along Y, never along Z.
    const Quat rot = Quat::fromAxisAngle(Vec3(1, 0, 0), 1.57079632679f);
    const Mat4 worldToLocal = Mat4::fromQuat(rot);

    rhi::MeshVertex out[4]{};
    render::softBodyPackVertices(world, 4, worldToLocal, idx, 6, nullptr, out);

    // Derive the expected normal the same way the test derives a position: transform the world
    // normal by the SAME matrix, independently of the function under test.
    const f32 wn[3] = {0.0f, 0.0f, 1.0f};
    const f32 ex = wn[0]*worldToLocal.m[0][0] + wn[1]*worldToLocal.m[1][0] + wn[2]*worldToLocal.m[2][0];
    const f32 ey = wn[0]*worldToLocal.m[0][1] + wn[1]*worldToLocal.m[1][1] + wn[2]*worldToLocal.m[2][1];
    const f32 ez = wn[0]*worldToLocal.m[0][2] + wn[1]*worldToLocal.m[1][2] + wn[2]*worldToLocal.m[2][2];

    bool allMatch = true;
    for (int v = 0; v < 4; ++v)
        if (!nearf(out[v].nx, ex) || !nearf(out[v].ny, ey) || !nearf(out[v].nz, ez)) allMatch = false;
    check(allMatch,
          "normals are derived from LOCAL positions, not world ones (got " +
          std::to_string(out[0].nx) + ", " + std::to_string(out[0].ny) + ", " +
          std::to_string(out[0].nz) + "; expected " + std::to_string(ex) + ", " +
          std::to_string(ey) + ", " + std::to_string(ez) + ")");

    // And the discriminator stated outright: the WRONG implementation would report +Z here, so if
    // the two ever coincide this test has quietly stopped proving anything.
    check(!nearf(ez, 1.0f),
          "the rotation really does move the normal off +Z, so this test can distinguish the two");
}

// The one branch in the pure function with no covering assertion: a triangle naming a vertex that
// does not exist. It must be skipped rather than read out of bounds -- a corrupt or truncated index
// buffer is exactly the kind of asset that reaches a renderer.
static void testOutOfRangeIndicesAreSkipped() {
    const f32 world[] = {
        0.0f, 0.0f, 0.0f,
        10.0f, 0.0f, 0.0f,
        10.0f, 10.0f, 0.0f,
    };
    // The second triangle names vertex 7, which does not exist in a 3-vertex mesh.
    const u32 idx[] = {0, 1, 2, 0, 1, 7};

    rhi::MeshVertex out[3]{};
    render::softBodyPackVertices(world, 3, Mat4::identity(), idx, 6, nullptr, out);

    // It must not have crashed, and the surviving triangle's normal must still be the clean +Z of
    // the first one -- proof the bad triangle contributed nothing rather than contributing garbage.
    bool finite = true;
    for (int v = 0; v < 3; ++v)
        if (!std::isfinite(out[v].nx) || !std::isfinite(out[v].ny) || !std::isfinite(out[v].nz))
            finite = false;
    check(finite, "an out-of-range triangle leaves every normal finite");
    check(nearf(std::abs(out[0].nz), 1.0f),
          "and the valid triangle still produced its own normal (" + std::to_string(out[0].nz) + ")");
}

int main() {
    AVER_INFO("SoftBodySceneTest");
    testWorldToLocalIsRowVector();
    testNormalsOnFlatAndRotatedQuad();
    testDegenerateTriangleNoNaN();
    testSharedVertexGetsBlendedNormal();
    testNormalsAreRecomputedNotCached();
    testUvPassthroughAndNullFallback();
    testNormalsAreComputedInLocalSpaceNotWorld();
    testOutOfRangeIndicesAreSkipped();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
