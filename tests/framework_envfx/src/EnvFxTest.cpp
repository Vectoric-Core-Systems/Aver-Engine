// Fog volumes and reflection probes (G4 R8): the pure maths in aver/core/FogVolume.hpp and aver/core/ReflectionProbe.hpp. Fog: the affine inverse, the
// per-shape signed distance, the edge fade and height falloff, negative density, packing and the HLSL text that mirrors it. Probes: oriented-box
// influence and blending, box projection against hand-computed intersections, probe selection (smallest first, mask, max distance), the octahedral
// mapping and the cube-face mapping round trips. Header-only, no device. Exits 0 or 1.
#include "aver/core/FogVolume.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/ReflectionProbe.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 tol = 1e-4f) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

// A box volume of half size (hx, hy, hz) centred at c, rotated by yaw degrees about Z and scaled by s.
static FogVolumeDesc volume(FogVolumeShape shape, f32 hx, f32 hy, f32 hz, f32 cx, f32 cy, f32 cz, f32 yawDeg = 0, f32 s = 1) {
    FogVolumeDesc d;
    d.shape = shape;
    d.halfExtentCm[0] = hx; d.halfExtentCm[1] = hy; d.halfExtentCm[2] = hz;
    const f32 r = yawDeg * 3.14159265f / 180.0f, c = std::cos(r), sn = std::sin(r);
    // Row-vector basis: local X goes to (c, s, 0), local Y to (-s, c, 0).
    const f32 w[12] = {c * s, sn * s, 0, -sn * s, c * s, 0, 0, 0, s, cx, cy, cz};
    for (int i = 0; i < 12; ++i) d.world[i] = w[i];
    return d;
}

static f32 densityAt(const FogVolumeDesc& d, f32 x, f32 y, f32 z) {
    f32 pk[kFogVolumeVecs][4];
    if (!fogv::pack(d, pk)) return -12345.0f;
    const f32 p[3] = {x, y, z};
    return fogv::density(pk, p);
}

static void testAffine() {
    AVER_INFO("=== the world -> unit transform ===");
    const FogVolumeDesc d = volume(FogVolumeShape::Box, 100, 200, 50, 1000, 2000, 300, 90, 2.0f);
    f32 rows[3][4];
    check(fogv::invertAffine(d.world, rows), "a rotated, scaled matrix inverts");
    // The local origin lands on the translation: the inverse of the translation is the origin.
    const f32 o[3] = {rows[0][0] * 1000 + rows[0][1] * 2000 + rows[0][2] * 300 + rows[0][3], rows[1][0] * 1000 + rows[1][1] * 2000 + rows[1][2] * 300 + rows[1][3],
                      rows[2][0] * 1000 + rows[2][1] * 2000 + rows[2][2] * 300 + rows[2][3]};
    check(near(o[0], 0, 1e-3f) && near(o[1], 0, 1e-3f) && near(o[2], 0, 1e-3f), "the translation maps to the local origin");
    // Local X (100 cm * scale 2) rotated 90 degrees about Z points along world +Y: the point centre + (0, 200, 0) is local (100, 0, 0).
    const f32 px = 1000, py = 2000 + 200, pz = 300;
    const f32 lx = rows[0][0] * px + rows[0][1] * py + rows[0][2] * pz + rows[0][3];
    const f32 ly = rows[1][0] * px + rows[1][1] * py + rows[1][2] * pz + rows[1][3];
    check(near(lx, 100, 1e-3f) && near(ly, 0, 1e-3f), "a 90 degree yaw and a scale of 2 carry the local X axis onto world Y");
    const f32 sing[12] = {1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    f32 r2[3][4];
    check(!fogv::invertAffine(sing, r2), "a singular matrix is refused");
}

static void testShapes() {
    AVER_INFO("=== signed distances ===");
    const f32 o[3] = {0, 0, 0}, edge[3] = {1, 0, 0}, out[3] = {2, 0, 0}, corner[3] = {0.5f, 0.5f, 0.5f};
    check(near(fogv::sdf(FogVolumeShape::Box, o), -1) && near(fogv::sdf(FogVolumeShape::Box, edge), 0) && near(fogv::sdf(FogVolumeShape::Box, out), 1),
          "box: -1 at the centre, 0 on a wall, 1 a half size outside");
    check(near(fogv::sdf(FogVolumeShape::Box, corner), -0.5f), "box: the nearest wall decides");
    check(near(fogv::sdf(FogVolumeShape::Ellipsoid, corner), std::sqrt(0.75f) - 1.0f), "ellipsoid: radial");
    const f32 cyl[3] = {0.6f, 0.6f, 0.0f};
    check(near(fogv::sdf(FogVolumeShape::Cylinder, cyl), std::sqrt(0.72f) - 1.0f), "cylinder: radial in X/Y");
    const f32 capped[3] = {0, 0, 1.5f};
    check(near(fogv::sdf(FogVolumeShape::Cylinder, capped), 0.5f), "cylinder: the Z caps");
    // Cone: tip at +Z, radius 1 at the base (-Z).
    const f32 base[3] = {0.9f, 0, -0.9f}, tip[3] = {0.4f, 0, 0.9f}, mid[3] = {0.4f, 0, 0};
    check(fogv::sdf(FogVolumeShape::Cone, base) < 0.0f, "cone: inside near the base rim");
    check(fogv::sdf(FogVolumeShape::Cone, tip) > 0.0f, "cone: outside beside the tip");
    check(near(fogv::sdf(FogVolumeShape::Cone, mid), 0.4f - 0.5f), "cone: half way up the radius is one half");
    check(fogv::sdf(FogVolumeShape::World, out) < 0.0f && fogv::sdf(FogVolumeShape::World, o) < 0.0f, "world: inside everywhere");
}

static void testDensity() {
    AVER_INFO("=== density: edge fade, height falloff, carving ===");
    FogVolumeDesc d = volume(FogVolumeShape::Box, 200, 200, 200, 0, 0, 0);
    d.densityPerM = 2.0f;
    d.edgeFade = 0.1f;
    check(near(densityAt(d, 0, 0, 0), 0.02f), "density 2 per metre is 0.02 per cm at the centre");
    check(near(densityAt(d, 300, 0, 0), 0.0f), "outside the box there is none");
    // 5% of the half size inside the wall is half of the 10% fade band.
    check(near(densityAt(d, 190, 0, 0), 0.01f, 1e-3f), "half way through the edge fade: half density");
    check(near(densityAt(d, 199.9f, 0, 0), 0.0f, 0.1f), "on the wall: next to nothing");
    d.edgeFade = 1.0f;
    check(near(densityAt(d, 100, 0, 0), 0.01f, 1e-3f), "edge fade 1 ramps over the whole half size");
    d.edgeFade = 0.1f;
    d.densityPerM = -1.0f;
    check(densityAt(d, 0, 0, 0) < 0.0f, "a negative density carves");
    d.densityPerM = 1.0f;
    d.heightFalloff = 1.0f;   // per metre
    // The volume's lowest point is z = -200. At z = -200 + 100 cm (1 m up) the density is e^-1 of the base.
    check(near(densityAt(d, 0, 0, -100), 0.01f * std::exp(-1.0f), 1e-3f), "height falloff: e^-1 one metre above the bottom");
    check(near(densityAt(d, 0, 0, -190), 0.01f * 0.5f * std::exp(-0.1f), 1e-3f), "ten centimetres above the bottom: e^-0.1, and half the edge fade");
    FogVolumeDesc w = volume(FogVolumeShape::World, 1, 1, 1, 0, 0, 0);
    w.densityPerM = 0.5f;
    check(near(densityAt(w, 123456, -9876, 4321), 0.005f), "a world volume is the same everywhere");
}

static void testPack() {
    AVER_INFO("=== packing ===");
    check(sizeof(FogVolumeBlock) == 16 + 16 * 6 * 16 && kFogVolumeMax == 16 && kFogVolumeVecs == 6, "the block is 16 B of header and 16 x 6 float4");
    FogVolumeDesc v[20];
    for (int i = 0; i < 20; ++i) { v[i] = volume(FogVolumeShape::Box, 100, 100, 100, f32(i) * 10, 0, 0); v[i].albedo[0] = 0.5f; v[i].emission[1] = 3.0f; }
    FogVolumeBlock b;
    check(fogv::packAll(v, 20, b) == 16 && b.header[0] == 16.0f, "more than 16 volumes: the first 16 are packed");
    check(near(b.vec[0][4][0], 0.5f) && near(b.vec[0][5][1], 3.0f), "albedo and emission land in their float4s");
    check(near(b.vec[0][5][3], 100.0f), "the smallest half size rides in emission.w");
    check(near(b.vec[0][4][3], -100.0f), "the lowest Z rides in albedo.w");
    FogVolumeDesc bad = volume(FogVolumeShape::Box, 1, 1, 1, 0, 0, 0);
    bad.world[0] = bad.world[1] = bad.world[2] = 0;
    FogVolumeDesc mix[3] = {v[0], bad, v[1]};
    check(fogv::packAll(mix, 3, b) == 2, "a volume with a singular matrix is skipped");
    FogVolumeDesc nanDensity = v[0]; nanDensity.densityPerM = NAN;
    f32 pk[kFogVolumeVecs][4];
    check(!fogv::pack(nanDensity, pk), "a NaN density is refused");
    // A rotated, scaled volume: the shader's unit coordinates at the corner are +-1.
    FogVolumeDesc r = volume(FogVolumeShape::Box, 100, 50, 25, 10, 20, 30, 30, 2.0f);
    fogv::pack(r, pk);
    // Local (100, 50, 25) in cm, through the row-vector basis (scaled 2, yawed 30): world = local * B + t.
    const f32 c = std::cos(0.5235988f), s = std::sin(0.5235988f);
    const f32 wx = (100 * c - 50 * s) * 2 + 10, wy = (100 * s + 50 * c) * 2 + 20, wz = 25 * 2 + 30;
    const f32 u0 = pk[0][0] * wx + pk[0][1] * wy + pk[0][2] * wz + pk[0][3];
    const f32 u1 = pk[1][0] * wx + pk[1][1] * wy + pk[1][2] * wz + pk[1][3];
    const f32 u2 = pk[2][0] * wx + pk[2][1] * wy + pk[2][2] * wz + pk[2][3];
    check(near(u0, 1, 1e-3f) && near(u1, 1, 1e-3f) && near(u2, 1, 1e-3f), "the packed rows send a corner of the rotated, scaled box to (1, 1, 1)");
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void testShaderText() {
    AVER_INFO("=== the HLSL mirrors the constants ===");
    const std::string hlsl = readFile(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_froxel.hlsl");
    check(!hlsl.empty(), "voxi_froxel.hlsl is readable");
    check(hlsl.find("#define AVER_FV_MAX 16") != std::string::npos, "AVER_FV_MAX mirrors kFogVolumeMax");
    check(hlsl.find("#define AVER_FV_VECS 6") != std::string::npos, "AVER_FV_VECS mirrors kFogVolumeVecs");
    check(hlsl.find("AVER_FOG_VOLUMES") != std::string::npos, "the fog-volume code is behind AVER_FOG_VOLUMES");
}

// ---- probes ----

static ReflectionProbeDesc probeAt(f32 hx, f32 hy, f32 hz, f32 cx, f32 cy, f32 cz, f32 yawDeg = 0) {
    ReflectionProbeDesc p;
    p.halfExtentCm[0] = hx; p.halfExtentCm[1] = hy; p.halfExtentCm[2] = hz;
    const f32 r = yawDeg * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
    const f32 w[12] = {c, s, 0, -s, c, 0, 0, 0, 1, cx, cy, cz};
    for (int i = 0; i < 12; ++i) p.world[i] = w[i];
    return p;
}

static void testInfluence() {
    AVER_INFO("=== probe influence ===");
    ReflectionProbeDesc p = probeAt(500, 300, 200, 0, 0, 200);
    p.blendDistanceCm = 100;
    const probe::Frame f = probe::frameOf(p);
    check(f.ok, "the frame inverts");
    check(near(probe::influence(p, f, {0, 0, 200}), 1.0f), "the middle is full influence");
    check(probe::influence(p, f, {600, 0, 200}) == 0.0f, "outside the box is none");
    check(near(probe::influence(p, f, {450, 0, 200}), 0.5f), "50 cm inside a wall with a 100 cm blend is one half");
    check(near(probe::influence(p, f, {0, 0, 10}), 0.0f + 10.0f / 100.0f), "the floor 10 cm below counts as a wall too");
    p.blendDistanceCm = 0;
    check(probe::influence(p, probe::frameOf(p), {499, 0, 200}) == 1.0f, "no blend distance: full to the wall");
    ReflectionProbeDesc rot = probeAt(500, 100, 200, 0, 0, 0, 90);   // long axis now along world Y
    rot.blendDistanceCm = 0;
    const probe::Frame fr = probe::frameOf(rot);
    check(probe::influence(rot, fr, {0, 400, 0}) == 1.0f && probe::influence(rot, fr, {400, 0, 0}) == 0.0f, "a rotated box covers the rotated volume");
}

static void testBoxProject() {
    AVER_INFO("=== box projection ===");
    ReflectionProbeDesc p = probeAt(500, 500, 250, 0, 0, 250);
    p.boxProjection = true;
    const probe::Frame f = probe::frameOf(p);
    // Standing at x = 400 and looking along +X, the +X wall is 100 cm away; the capture at the centre sees that wall straight along +X.
    Vec3 d = probe::boxProject(p, f, {400, 0, 250}, {1, 0, 0});
    check(near(d.x, 1, 1e-4f) && near(d.y, 0, 1e-4f) && near(d.z, 0, 1e-4f), "straight at a wall: unchanged");
    // From x = 400 looking along +Y the +Y wall is hit at (400, 500): from the capture point that is the direction (400, 500, 0).
    d = probe::boxProject(p, f, {400, 0, 250}, {0, 1, 0});
    const f32 n = std::sqrt(400.0f * 400.0f + 500.0f * 500.0f);
    check(near(d.x, 400 / n, 1e-4f) && near(d.y, 500 / n, 1e-4f) && near(d.z, 0, 1e-4f), "sideways: pulled toward the wall hit point");
    // Down from the middle height of the room: the floor at z = 0 is 250 below, the hit is under the point itself.
    d = probe::boxProject(p, f, {300, -200, 250}, {0, 0, -1});
    const f32 n2 = std::sqrt(300.0f * 300.0f + 200.0f * 200.0f + 250.0f * 250.0f);
    check(near(d.x, 300 / n2, 1e-4f) && near(d.y, -200 / n2, 1e-4f) && near(d.z, -250 / n2, 1e-4f), "down: the floor point seen from the centre");
    // A capture offset moves the reference point.
    p.originOffsetCm[0] = 100;
    d = probe::boxProject(p, probe::frameOf(p), {400, 0, 250}, {0, 1, 0});
    const f32 n3 = std::sqrt(300.0f * 300.0f + 500.0f * 500.0f);
    check(near(d.x, 300 / n3, 1e-4f) && near(d.y, 500 / n3, 1e-4f), "an origin offset of +100 shifts the capture point");
    p.originOffsetCm[0] = 0;
    p.boxProjection = false;
    d = probe::boxProject(p, probe::frameOf(p), {400, 0, 250}, {0, 1, 0});
    check(near(d.x, 0) && near(d.y, 1), "without box projection the direction is untouched");
    // A rotated box projects in its own axes: yaw 90 swaps the roles of X and Y.
    ReflectionProbeDesc q = probeAt(500, 200, 250, 0, 0, 250, 90);
    q.boxProjection = true;
    const probe::Frame fq = probe::frameOf(q);
    d = probe::boxProject(q, fq, {0, 100, 250}, {1, 0, 0});   // +X world is the box's short side now: the wall is 200 away
    const f32 n4 = std::sqrt(200.0f * 200.0f + 100.0f * 100.0f);
    check(near(d.x, 200 / n4, 1e-3f) && near(d.y, 100 / n4, 1e-3f), "a yawed box is projected in its own axes");
    // A point outside the box is not corrected.
    p.boxProjection = true;
    d = probe::boxProject(p, probe::frameOf(p), {900, 0, 250}, {0, 1, 0});
    check(near(d.x, 0) && near(d.y, 1), "outside the box: no correction");
}

static void testSelect() {
    AVER_INFO("=== choosing probes ===");
    ReflectionProbeDesc big = probeAt(2000, 2000, 500, 0, 0, 500);
    ReflectionProbeDesc small = probeAt(300, 300, 200, 0, 0, 200);
    big.blendDistanceCm = small.blendDistanceCm = 0;
    ReflectionProbeDesc ps[2] = {big, small};
    probe::Selection s = probe::select(ps, 2, {0, 0, 200}, {0, 0, 0});
    check(s.count == 1 && s.pick[0].index == 1 && near(s.rest, 0.0f, 1e-3f), "inside both with no blend: the smaller box takes everything");
    ps[1].blendDistanceCm = 200;
    s = probe::select(ps, 2, {250, 0, 200}, {0, 0, 0});   // 50 cm from the small probe's wall: influence 0.25
    check(s.count == 2 && s.pick[0].index == 1 && near(s.pick[0].weight, 0.25f) && s.pick[1].index == 0 && near(s.pick[1].weight, 0.75f) && near(s.rest, 0.0f, 1e-3f),
          "at the edge of the small probe the big one fills in the rest");
    s = probe::select(ps, 2, {1500, 0, 200}, {0, 0, 0});
    check(s.count == 1 && s.pick[0].index == 0, "outside the small probe only the big one lights");
    s = probe::select(ps, 2, {5000, 0, 200}, {0, 0, 0});
    check(s.count == 0 && near(s.rest, 1.0f), "outside every probe the sky keeps it all");
    ps[0].intensity = 0.5f;
    s = probe::select(ps, 2, {1500, 0, 200}, {0, 0, 0});
    check(near(s.pick[0].weight, 0.5f), "intensity scales the probe's weight");
    ps[0].intensity = 1.0f;
    ps[0].reflectionMask = 2u;
    s = probe::select(ps, 2, {1500, 0, 200}, {0, 0, 0}, 1u);
    check(s.count == 0, "a receiver on a layer the probe does not serve is skipped");
    ps[0].reflectionMask = 0xFFFFFFFFu;
    ps[0].maxDistanceCm = 1000;
    s = probe::select(ps, 2, {1500, 0, 200}, {0, 0, 5000});
    check(s.count == 0, "past max_distance from the camera the probe is off");
    s = probe::select(ps, 2, {1500, 0, 200}, {0, 0, 500});
    check(s.count == 1, "within max_distance it is on");
    ReflectionProbeDesc many[6];
    for (auto& m : many) { m = probeAt(1000, 1000, 1000, 0, 0, 0); m.blendDistanceCm = 400; }
    for (int i = 0; i < 6; ++i) { many[i].halfExtentCm[0] = 1000.0f + i; }
    s = probe::select(many, 6, {800, 0, 0}, {0, 0, 0});
    check(s.count <= kProbeMaxActive, "no more than four probes blend");
    f32 sum = s.rest;
    for (u32 i = 0; i < s.count; ++i) sum += s.pick[i].weight;
    check(near(sum, 1.0f, 1e-3f), "the weights and the rest add to one");
}

static void testOct() {
    AVER_INFO("=== octahedral and cube-face maps ===");
    const Vec3 dirs[] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {1, 1, 1}, {-1, 2, -3}, {0.3f, -0.8f, 0.2f}, {-4, -1, 0.001f}};
    bool all = true;
    for (Vec3 d : dirs) {
        d = d.getSafeNormal();
        f32 u, v;
        probe::octEncode(d, u, v);
        const Vec3 b = probe::octDecode(u, v);
        if (u < 0 || u > 1 || v < 0 || v > 1 || (b - d).size() > 1e-4f) all = false;
    }
    check(all, "oct encode / decode round-trips ten directions inside the unit square");
    f32 u, v;
    probe::octEncode({0, 0, 1}, u, v);
    check(near(u, 0.5f) && near(v, 0.5f), "+Z is the centre of the square");
    probe::octEncode({0, 0, -1}, u, v);
    check((near(u, 0) || near(u, 1)) && (near(v, 0) || near(v, 1)), "-Z is a corner");
    bool faces = true;
    for (Vec3 d : dirs) {
        d = d.getSafeNormal();
        f32 s, t;
        const u32 face = probe::cubeFace(d, s, t);
        const Vec3 b = probe::cubeDirection(face, s, t);
        if (face > 5 || s < 0 || s > 1 || t < 0 || t > 1 || (b - d).size() > 1e-4f) faces = false;
    }
    check(faces, "cube face / direction round-trips");
    f32 s, t;
    check(probe::cubeFace({1, 0, 0}, s, t) == 0 && probe::cubeFace({-1, 0, 0}, s, t) == 1 && probe::cubeFace({0, 1, 0}, s, t) == 2 &&
              probe::cubeFace({0, -1, 0}, s, t) == 3 && probe::cubeFace({0, 0, 1}, s, t) == 4 && probe::cubeFace({0, 0, -1}, s, t) == 5,
          "the face order is +X -X +Y -Y +Z -Z");
    check(near(probe::roughnessToMip(0.5f, 9), 4.0f) && near(probe::roughnessToMip(2.0f, 9), 8.0f) && probe::roughnessToMip(0.5f, 1) == 0.0f, "roughness picks a mip linearly");
}

int main() {
    testAffine();
    testShapes();
    testDensity();
    testPack();
    testShaderText();
    testInfluence();
    testBoxProject();
    testSelect();
    testOct();
    if (g_failures) {
        AVER_ERROR("EnvFxTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("EnvFxTest: ok");
    return 0;
}
