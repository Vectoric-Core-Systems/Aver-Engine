// MeshBuildTest -- G4 R7: meshes assembled from script surfaces (ArrayMesh / SurfaceTool / ImmediateMesh arrays) and instance tables baked into one mesh
// (MultiMesh), in aver/formats/MeshBuild.hpp. Streams, material slots, padding, refusal of malformed input, instance transforms (rotation, scale, mirroring),
// colour multiplication, the second UV set from custom data, submesh grouping and the vertex budget. Pure arithmetic: no device. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/MeshBuild.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::fmt;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol; }

// One triangle in the XY plane, facing +Z.
static RuntimeSurface triangle(const char* material) {
    RuntimeSurface s;
    s.pos = {0, 0, 0, 100, 0, 0, 0, 100, 0};
    s.idx = {0, 1, 2};
    s.material = material;
    return s;
}

static void testAssemble() {
    AVER_INFO("=== assembleSurfaces ===");
    RuntimeSurface a = triangle("M_A");
    RuntimeSurface b = triangle("M_B");
    b.col = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    for (usize i = 0; i < b.pos.size(); i += 3) b.pos[i + 2] = 50;
    OcMeshData m;
    std::string why;
    check(assembleSurfaces({a, b}, m, &why), "two surfaces assemble: " + why);
    check(m.vertexCount() == 6 && m.indices.size() == 6, "six vertices, six indices");
    check(m.submeshes.size() == 2 && m.materialSlots.size() == 2 && m.materialSlots[0] == "M_A" && m.materialSlots[1] == "M_B", "one submesh and one material slot per surface");
    check(m.submeshes[0].indexStart == 0 && m.submeshes[0].indexCount == 3 && m.submeshes[1].indexStart == 3 && m.submeshes[1].indexCount == 3, "the submesh ranges are consecutive");
    check(m.indices[3] == 3 && m.indices[4] == 4 && m.indices[5] == 5, "the second surface's indices are offset past the first's vertices");
    check(submeshesPartitionIndices(m), "the submeshes partition the index buffer (the draw path's rule)");
    check(near(m.normals[2], 1.0), "missing normals are computed (a counter-clockwise XY triangle faces +Z)");
    check(m.hasColor() && near(m.colors[0], 1) && near(m.colors[4], 1) && near(m.colors[3], 1) && near(m.colors[3 * 4], 1), "a colour stream only one surface has is padded white on the other");
    check(!m.hasTangent() && !m.hasUv1(), "streams no surface has stay absent");
    check(m.valid(), "the result is a valid OcMeshData");
    check(near(m.boundsMax.x, 100) && near(m.boundsMax.z, 50), "bounds are computed");

    RuntimeSurface c = triangle("");
    c.tan = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    c.uv1 = {0, 0, 1, 0, 0, 1};
    OcMeshData m2;
    check(assembleSurfaces({a, c}, m2, nullptr) && m2.hasTangent() && m2.hasUv1(), "tangent and UV1 streams are kept");
    check(m2.tangents[0] == 0 && m2.tangents[3] == 0 && near(m2.tangents[12], 1) && near(m2.tangents[15], 1), "a tangent stream is padded with 'no tangent' (all zero)");
    check(m2.materialSlots[1] == "M_Default", "an empty material name becomes the default slot name");

    RuntimeSurface strips;
    strips.pos = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1};
    OcMeshData m3;
    check(assembleSurfaces({strips}, m3, nullptr) && m3.indices.size() == 6, "a surface with no index list draws its vertices in order (two triangles)");

    RuntimeSurface bad = triangle("x");
    bad.idx = {0, 1, 7};
    OcMeshData m4;
    check(!assembleSurfaces({bad}, m4, &why) && why.find("index") != std::string::npos, "an index past the vertices is refused");
    RuntimeSurface bad2 = triangle("x");
    bad2.nrm = {0, 0, 1};
    check(!assembleSurfaces({bad2}, m4, &why) && why.find("normal") != std::string::npos, "a normal array of the wrong length is refused");
    RuntimeSurface bad3 = triangle("x");
    bad3.col = {1, 1, 1, 1};
    check(!assembleSurfaces({bad3}, m4, &why), "a colour array of the wrong length is refused");
    RuntimeSurface bad4 = triangle("x");
    bad4.idx = {0, 1};
    check(!assembleSurfaces({bad4}, m4, &why), "an index count that is not a multiple of three is refused");
    check(!assembleSurfaces({}, m4, &why), "no surfaces: refused");
}

static void testNormals() {
    AVER_INFO("=== smooth normals ===");
    // A flat quad of two triangles: every normal is +Z; a roof of two triangles sharing an edge: the shared vertices average.
    const std::vector<f32> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    const std::vector<u32> idx = {0, 1, 2, 1, 3, 2};
    std::vector<f32> n;
    computeSmoothNormals(pos, idx, n);
    check(n.size() == 12 && near(n[2], 1) && near(n[5], 1) && near(n[8], 1) && near(n[11], 1), "a flat quad has +Z normals at every vertex");
    const std::vector<f32> roof = {0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, -1, 1, 1, -1};
    const std::vector<u32> ridge = {0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4};
    computeSmoothNormals(roof, ridge, n);
    double len = std::sqrt(double(n[6]) * n[6] + double(n[7]) * n[7] + double(n[8]) * n[8]);
    check(near(len, 1.0), "normals come out unit length");
    computeSmoothNormals({0, 0, 0}, {}, n);
    check(n.size() == 3 && near(n[2], 1), "a vertex no triangle uses gets +Z, never NaN");
}

static OcMeshData makeQuadMesh() {
    // Two submeshes (two materials), 4 vertices: a unit square in the XY plane at the origin, in centimetres.
    RuntimeSurface a, b;
    a.pos = {0, 0, 0, 10, 0, 0, 10, 10, 0};
    a.idx = {0, 1, 2};
    a.material = "M_One";
    b.pos = {0, 0, 0, 10, 10, 0, 0, 10, 0};
    b.idx = {0, 1, 2};
    b.material = "M_Two";
    OcMeshData m;
    assembleSurfaces({a, b}, m, nullptr);
    return m;
}

static void testBake() {
    AVER_INFO("=== bakeInstances ===");
    const OcMeshData src = makeQuadMesh();
    MeshInstanceSet set;
    // 0: identity at the origin; 1: translated (100, 0, 0); 2: scaled 2x and rotated 90 degrees about Z, row-vector form: x' = -y, y' = x.
    set.transforms = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
                      1, 0, 0, 0, 1, 0, 0, 0, 1, 100, 0, 0,
                      0, 2, 0, -2, 0, 0, 0, 0, 2, 0, 0, 200};
    OcMeshData out;
    u32 used = 0;
    std::string why;
    check(bakeInstances(src, set, 3, 0, out, &used, &why) && used == 3, "three instances bake: " + why);
    check(out.vertexCount() == src.vertexCount() * 3 && out.indices.size() == src.indices.size() * 3, "the vertices and triangles are the source's times three");
    check(near(out.positions[(1 * 6 + 1) * 3], 110) && near(out.positions[(1 * 6 + 1) * 3 + 1], 0), "instance 1 is moved 100 cm along X");
    const usize o2 = 2 * 6 + 1;   // source vertex 1 = (10, 0, 0); rotated + scaled: p * M = (10*0 + 0*-2, 10*2, 0) + (0, 0, 200)
    check(near(out.positions[o2 * 3], 0) && near(out.positions[o2 * 3 + 1], 20) && near(out.positions[o2 * 3 + 2], 200), "instance 2 is rotated, scaled and lifted (row-vector form)");
    check(near(out.normals[o2 * 3 + 2], 1.0) && near(std::fabs(out.normals[o2 * 3]) + std::fabs(out.normals[o2 * 3 + 1]), 0.0), "a uniform scale and a turn about Z keep the +Z normal");
    check(out.submeshes.size() == 2 && out.materialSlots.size() == 2, "the source's two submeshes survive");
    check(out.submeshes[0].indexCount == 9 && out.submeshes[1].indexCount == 9 && out.submeshes[1].indexStart == 9, "each submesh groups its triangles from every instance");
    check(submeshesPartitionIndices(out), "and the submeshes partition the index buffer");
    check(out.indices[3] == 6 && out.indices[6] == 12, "instance 1's triangles index its own vertex copy");
    check(out.valid() && !out.hasColor(), "no colour stream when neither the mesh nor the table has colours");

    // colours
    set.colors = {1, 0, 0, 1, 0, 1, 0, 1, 0.5f, 0.5f, 0.5f, 0.5f};
    OcMeshData withCol;
    check(bakeInstances(src, set, 3, 0, withCol, &used, &why) && withCol.hasColor(), "per-instance colours make a colour stream");
    check(near(withCol.colors[0], 1) && near(withCol.colors[1], 0) && near(withCol.colors[(6) * 4 + 1], 1) && near(withCol.colors[(12) * 4], 0.5), "each instance's vertices carry its colour");
    OcMeshData tinted = src;
    tinted.colors.assign(tinted.vertexCount() * 4, 0.5f);
    OcMeshData both;
    check(bakeInstances(tinted, set, 3, 0, both, &used, &why) && near(both.colors[0], 0.5) && near(both.colors[6 * 4 + 1], 0.5) && near(both.colors[6 * 4], 0.0), "the mesh's own vertex colour is multiplied by the instance colour");

    // custom data -> UV1
    set.custom = {0.25f, 0.75f, 9, 9, 1, 2, 9, 9, 3, 4, 9, 9};
    OcMeshData withCustom;
    check(bakeInstances(src, set, 3, 0, withCustom, &used, &why) && withCustom.hasUv1() && near(withCustom.uvs1[0], 0.25) && near(withCustom.uvs1[1], 0.75) && near(withCustom.uvs1[6 * 2], 1) && near(withCustom.uvs1[12 * 2 + 1], 4),
          "custom data x,y become the second UV set, per instance");

    // visible count and budget
    OcMeshData one;
    check(bakeInstances(src, set, 1, 0, one, &used, &why) && used == 1 && one.vertexCount() == 6, "visible = 1 draws only the first instance");
    OcMeshData capped;
    check(bakeInstances(src, set, 3, 13, capped, &used, &why) && used == 2 && capped.vertexCount() == 12, "a vertex budget of 13 lets two 6-vertex instances through");
    check(!bakeInstances(src, set, 3, 4, capped, &used, &why) && used == 0, "a budget below one instance draws nothing");
    check(!bakeInstances(src, set, 0, 0, capped, &used, &why), "no visible instance: nothing to draw");
    MeshInstanceSet none;
    check(!bakeInstances(src, none, 5, 0, capped, &used, &why), "an empty table: nothing to draw");

    // mirroring flips the winding and keeps the normal on the outside
    MeshInstanceSet mirror;
    mirror.transforms = {-1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    OcMeshData mir;
    check(bakeInstances(src, mirror, 1, 0, mir, &used, &why), "a mirrored instance bakes");
    // Source triangle (0,0,0) (10,0,0) (10,10,0) counter-clockwise from +Z; mirrored in X it would be clockwise, so two indices swap.
    check(mir.indices[0] == 0 && mir.indices[1] == 2 && mir.indices[2] == 1, "its triangles are re-wound");
    check(near(mir.normals[2], 1.0), "and its +Z normal still faces +Z");
    // a non-uniform scale: a 45 degree slope's normal tilts toward the squashed axis
    RuntimeSurface slope;
    slope.pos = {0, 0, 0, 10, 0, 10, 0, 10, 0};
    slope.idx = {0, 1, 2};
    slope.nrm = {-0.70710678f, 0, 0.70710678f, -0.70710678f, 0, 0.70710678f, -0.70710678f, 0, 0.70710678f};
    OcMeshData sm;
    assembleSurfaces({slope}, sm, nullptr);
    MeshInstanceSet squash;
    squash.transforms = {1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0, 0, 0};   // z halved
    OcMeshData sq;
    check(bakeInstances(sm, squash, 1, 0, sq, &used, &why), "a squashed instance bakes");
    // inverse transpose of diag(1,1,0.5) scales the normal's z by 2: (-0.707, 0, 1.414) normalised
    check(near(sq.normals[0], -0.4472, 1e-3) && near(sq.normals[2], 0.8944, 1e-3), "a non-uniform scale tilts the normal by the inverse transpose");

    // tangents follow the instance and flip handedness under a mirror
    OcMeshData tan = src;
    tan.tangents.assign(tan.vertexCount() * 4, 0.0f);
    for (u32 v = 0; v < tan.vertexCount(); ++v) { tan.tangents[usize(v) * 4] = 1; tan.tangents[usize(v) * 4 + 3] = 1; }
    OcMeshData tm;
    check(bakeInstances(tan, mirror, 1, 0, tm, &used, &why) && tm.hasTangent() && near(tm.tangents[0], -1) && near(tm.tangents[3], -1), "a mirrored instance's tangent turns with it and its handedness flips");
    OcMeshData ti;
    check(bakeInstances(tan, set, 3, 0, ti, &used, &why) && near(ti.tangents[2 * 6 * 4 + 1], 1) && near(ti.tangents[2 * 6 * 4 + 3], 1), "a turned instance's tangent turns (X to Y)");
}

int main() {
    testAssemble();
    testNormals();
    testBake();
    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
