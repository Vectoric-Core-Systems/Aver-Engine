// ShapeBuilderTest -- the generic shape handles of physics_shape_builder_abi.h (SHAPES_MATERIALS, phase A).
//
// THE STANDARD IS OBSERVED MOTION and observed geometry, as in ShapeTest: a shape built to twice its size still
// returns a handle and still falls. So almost every check drops something on a floor whose top is exactly z = 0
// and reads where it rests, or reads an AABB, a mass or a ray. The margins are UNVERIFIED starting values -- this
// file was written without being run -- and a failing relation is a prompt to look at the physics before loosening
// the number.
//
//   1. each leaf rests at its half-height; the tapered cylinder pins which end is +Z,
//   2. plane, triangle and empty shapes behave as the header says,
//   3. decorators: rotated-translated, offset centre of mass, scaled; and the refusals,
//   4. compounds: a rotated child, the identity-rotation equivalence with the old box compound, a mutable compound,
//   5. set_shape and set_scale keep the mass the header says they keep,
//   6. introspection, including the inertia frame (the conversion that is UNVERIFIED until this runs),
//   7. mesh options and palettes, hulls, the dynamic mesh body and its documented limit,
//   8. handle lifetime, the old creators against the new builders, a chassis from a shape, and determinism,
//   9. (phase B) what raycast_detail says about compound children, mesh triangles and palettes, and leaf-material friction.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shapes_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_surface_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

static bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;
// A curved body dropped hard on a flat one comes to rest inside Jolt's default penetration slop (2 cm, measured: it
// rests at 24.950 with the slop set to 0.05 cm), so a resting height is checked to slop plus a little.
static const f32 kSlopCm = 2.0f;
static const int32_t kBadHandle = -1, kUnsupported = -5, kInvalidArgument = -6;

// A floor whose TOP SURFACE is exactly z = 0.
static void floorWorld() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
}

static void stepN(int n) {
    for (int i = 0; i < n; ++i) aver_phys_step(kDt);
}

static f32 posZ(int32_t body) {
    float p[3] = {0, 0, 0};
    aver_phys_body_position(body, p);
    return p[2];
}

static int32_t bodyOf(int32_t shape, f32 x, f32 y, f32 z, int32_t motion, f32 mass) {
    const float o[3] = {x, y, z};
    return aver_phys_body_from_shape(shape, o, nullptr, motion, mass, nullptr, 0);
}

// Settles `shape` as a dynamic body dropped from 300 cm and returns its centre-of-mass height.
static f32 restHeight(int32_t shape, f32 mass = 10.0f) {
    const int32_t b = bodyOf(shape, 0, 0, 300.0f, AVER_PHYS_MOTION_DYNAMIC, mass);
    stepN(400);
    const f32 z = posZ(b);
    aver_phys_remove_body(b);   // the next shape drops onto a bare floor
    return z;
}

static f32 aabbSize(int32_t body, int axis) {
    float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    aver_phys_body_aabb(body, mn, mx);
    return mx[axis] - mn[axis];
}

static f32 massOf(int32_t body) {
    float m = 0.0f;
    aver_phys_body_mass(body, &m);
    return m;
}

static bool shapeInfo(int32_t shape, float* f, int32_t* i) {
    return aver_phys_shape_info(shape, f, AVER_PHYS_SI_COUNT, i, AVER_PHYS_SII_COUNT) == 1;
}

// ---- mesh helpers -----------------------------------------------------------------------------------------

struct MeshData {
    std::vector<float> v;
    std::vector<int32_t> idx;
};

// Adds the triangle (a, b, c) wound so that (B - A) x (C - A) points along `outward`: the winding the mesh builder takes
// (a mesh is one-sided to bodies).
static void addTri(MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 outward) {
    if (dot(cross(b - a, c - a), outward) < 0.0f) std::swap(b, c);
    const int32_t base = static_cast<int32_t>(m.v.size() / 3);
    for (Vec3 p : {a, b, c}) { m.v.push_back(p.x); m.v.push_back(p.y); m.v.push_back(p.z); }
    m.idx.push_back(base); m.idx.push_back(base + 1); m.idx.push_back(base + 2);
}

// A closed box mesh, half extent h, 12 triangles.
static MeshData boxMesh(f32 h) {
    MeshData m;
    const Vec3 c[8] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}};
    const int face[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    const Vec3 nrm[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (int f = 0; f < 6; ++f) {
        addTri(m, c[face[f][0]], c[face[f][1]], c[face[f][2]], nrm[f]);
        addTri(m, c[face[f][0]], c[face[f][2]], c[face[f][3]], nrm[f]);
    }
    return m;
}

// A flat quad at height z facing up, half size h: two triangles.
static MeshData quadMesh(f32 h, f32 z) {
    MeshData m;
    const Vec3 up(0, 0, 1);
    addTri(m, Vec3(-h, -h, z), Vec3(h, -h, z), Vec3(h, h, z), up);
    addTri(m, Vec3(-h, -h, z), Vec3(h, h, z), Vec3(-h, h, z), up);
    return m;
}

static int32_t meshShape(const MeshData& m, const uint8_t* triPalette = nullptr, const int32_t* palette = nullptr,
                         int32_t paletteCount = 0, const int32_t* triUser = nullptr,
                         const float* p = nullptr, int32_t n = 0) {
    return aver_phys_shape_mesh(m.v.data(), static_cast<int32_t>(m.v.size() / 3), m.idx.data(),
                                static_cast<int32_t>(m.idx.size()), triPalette, palette, paletteCount, triUser, p, n);
}

// ---------------------------------------------------------------------------------------------------------

static void testLeavesRest() {
    AVER_INFO("=== each leaf rests at its half-height ===");
    floorWorld();

    const int32_t box = aver_phys_shape_box(20, 20, 20, 0, nullptr, 0);
    check(box != 0, "a box shape is created");
    const f32 zBox = restHeight(box);
    check(near(zBox, 20.0f, 1.5f), "a 40 cm box rests with its centre at 20 cm, got " + f2s(zBox));

    const int32_t sphere = aver_phys_shape_sphere(25, 0, nullptr, 0);
    const f32 zSphere = restHeight(sphere);
    check(near(zSphere, 25.0f, kSlopCm + 0.5f), "a 25 cm sphere rests at 25 cm, got " + f2s(zSphere));

    const int32_t capsule = aver_phys_shape_capsule(20, 120, 0, nullptr, 0);
    const f32 zCap = restHeight(capsule);
    check(near(zCap, 60.0f, kSlopCm + 0.5f), "a 120 cm total-height capsule rests at 60 cm, got " + f2s(zCap));

    const int32_t cyl = aver_phys_shape_cylinder(30, 80, 0, nullptr, 0);
    const f32 zCyl = restHeight(cyl);
    check(near(zCyl, 40.0f, kSlopCm + 0.5f), "an 80 cm cylinder rests at 40 cm, got " + f2s(zCyl));

    // THE LARGE RADIUS ON THE FLOOR AND THE SMALL ONE UP pins topRadius at +Z: stood the other way it would fall
    // over and lie on its side, and its AABB would be 80 cm tall instead of 100.
    const int32_t tcyl = aver_phys_shape_tapered_cylinder(10, 40, 100, 0, nullptr, 0);
    check(tcyl != 0, "a tapered cylinder is created");
    const int32_t tb = bodyOf(tcyl, 0, 0, 300.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    stepN(400);
    check(near(aabbSize(tb, 2), 100.0f, 3.0f), "it stands upright: 100 cm tall, got " + f2s(aabbSize(tb, 2)));
    check(near(posZ(tb), 32.0f, 4.0f), "its centre of mass is near the wide end, ~32 cm up, got " + f2s(posZ(tb)));

    check(aver_phys_shape_tapered_capsule(10, 30, 140, 0, nullptr, 0) != 0, "a tapered capsule is created");
    check(aver_phys_shape_capsule(40, 50, 0, nullptr, 0) == 0 && aver_phys_last_error() == kInvalidArgument,
          "a capsule too short for its radius is refused with InvalidArgument");
    check(aver_phys_shape_box(0, 20, 20, 0, nullptr, 0) == 0, "a box with a zero half extent is refused");
    float tooMany[7] = {};
    check(aver_phys_shape_box(20, 20, 20, 0, tooMany, 7) == 0, "a parameter block longer than the header defines is refused");

    aver_phys_shutdown();
}

static void testPlaneTriangleEmpty() {
    AVER_INFO("=== plane, triangle, empty ===");
    aver_phys_init();

    // A static plane at z = 40 holds a sphere at 40 + r.
    const float up[3] = {0, 0, 1};
    const int32_t plane = aver_phys_shape_plane(up, 40.0f, 5000.0f, 0);
    check(plane != 0, "a plane shape is created");
    check(bodyOf(plane, 0, 0, 0, AVER_PHYS_MOTION_STATIC, 0) != 0, "a static body from it is created");
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 200.0f, 10.0f, 5.0f);
    stepN(300);
    check(near(posZ(ball), 50.0f, 2.5f), "a ball dropped on a plane at z = 40 rests at 50, got " + f2s(posZ(ball)));
    check(bodyOf(plane, 0, 0, 0, AVER_PHYS_MOTION_DYNAMIC, 10.0f) == 0 && aver_phys_last_error() == kUnsupported,
          "a dynamic body from a plane is refused with Unsupported");

    // A triangle is a thin convex: it holds a sphere. Large and flat at z = 500 so it does not meet the plane.
    const float tri[9] = {-200, -200, 500, 200, -200, 500, 0, 200, 500};   // faces up, like a mesh wound the same way
    const int32_t t = aver_phys_shape_triangle(tri, 0, nullptr, 0);
    check(t != 0, "a triangle shape is created");
    bodyOf(t, 0, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t ball2 = aver_phys_add_dynamic_sphere(0, -50.0f, 700.0f, 10.0f, 5.0f);
    stepN(300);
    check(near(posZ(ball2), 510.0f, kSlopCm + 1.0f), "a ball on the triangle rests 10 cm above it, got " + f2s(posZ(ball2)));
    float collinear[9] = {0, 0, 0, 10, 0, 0, 20, 0, 0};
    check(aver_phys_shape_triangle(collinear, 0, nullptr, 0) == 0, "a collinear triangle is refused");

    // An empty shape has no volume and collides with nothing: a dynamic body of it falls through the plane.
    const float com[3] = {0, 0, 0};
    const int32_t empty = aver_phys_shape_empty(com);
    const int32_t eb = bodyOf(empty, 300.0f, 0, 100.0f, AVER_PHYS_MOTION_DYNAMIC, 0);
    stepN(200);
    check(eb != 0 && posZ(eb) < -100.0f, "a dynamic empty body falls through everything, z = " + f2s(posZ(eb)));
    float f[AVER_PHYS_SI_COUNT];
    int32_t iv[AVER_PHYS_SII_COUNT];
    check(shapeInfo(empty, f, iv) && iv[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_EMPTY, "its info says EMPTY");

    aver_phys_shutdown();
}

static void testTiltedPlaneSlides() {
    AVER_INFO("=== a plane tilted 45 degrees slides a box ===");
    aver_phys_init();
    // n . x = 0 with n = (0, s, s): the surface falls toward +Y, the solid side is below it.
    const f32 s = 0.70710678f;
    const float n[3] = {0, s, s};
    const int32_t plane = aver_phys_shape_plane(n, 0.0f, 5000.0f, 0);
    bodyOf(plane, 0, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 40.0f, 10, 10, 10, 5.0f);
    stepN(90);
    float v[3] = {0, 0, 0};
    aver_phys_body_velocity(box, v);
    check(v[1] > 50.0f, "the box slides down the slope toward +Y, vy = " + f2s(v[1]));
    aver_phys_shutdown();
}

static void testDecorators() {
    AVER_INFO("=== rotated-translated, offset centre of mass, scaled ===");
    floorWorld();

    // A capsule turned 90 degrees about +Y lies on its side and rests at its radius.
    const int32_t cap = aver_phys_shape_capsule(20, 120, 0, nullptr, 0);
    const Quat q = Quat::fromAxisAngle(Vec3{0, 1, 0}, radians(90.0f));
    const float qv[4] = {q.x, q.y, q.z, q.w};
    const int32_t rot = aver_phys_shape_rotated_translated(cap, nullptr, qv);
    check(rot != 0, "a rotated-translated shape is created");
    check(near(restHeight(rot), 20.0f, 2.0f), "a capsule turned onto its side rests at its radius");

    aver_phys_shutdown();
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);

    // The centre of mass moves by the offset; the origin does not.
    const int32_t box = aver_phys_shape_box(20, 20, 20, 0, nullptr, 0);
    const float off[3] = {0, 0, 10};
    const int32_t shifted = aver_phys_shape_offset_com(box, off);
    const int32_t b = bodyOf(shifted, 0, 0, 100.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    float origin[3] = {0, 0, 0};
    check(aver_phys_body_origin(b, origin) == 1 && near(origin[2], 100.0f, 0.5f), "the body origin stays at 100, got " + f2s(origin[2]));
    check(near(posZ(b), 110.0f, 0.5f), "the centre of mass is 10 above it, got " + f2s(posZ(b)));

    // Scale (2, 1, 1) doubles the AABB along X only.
    const int32_t small = aver_phys_shape_box(10, 10, 10, 0, nullptr, 0);
    const float sc[3] = {2, 1, 1};
    const int32_t scaled = aver_phys_shape_scaled(small, sc);
    const int32_t sb = bodyOf(scaled, 500.0f, 0, 100.0f, AVER_PHYS_MOTION_STATIC, 0);
    check(sb != 0 && near(aabbSize(sb, 0), 40.0f, 1.0f) && near(aabbSize(sb, 1), 20.0f, 1.0f),
          "a (2,1,1) scale gives a 40 x 20 footprint, got " + f2s(aabbSize(sb, 0)) + " x " + f2s(aabbSize(sb, 1)));
    const float zero[3] = {0, 1, 1};
    check(aver_phys_shape_scaled(small, zero) == 0 && aver_phys_last_error() == kInvalidArgument, "a zero scale is refused");
    check(aver_phys_shape_scaled(99999, sc) == 0 && aver_phys_last_error() == kBadHandle, "an unknown inner handle is refused");

    aver_phys_shutdown();
}

static void testStaticCompound() {
    AVER_INFO("=== a static compound keeps child rotation, and equals the old box compound ===");
    aver_phys_init();

    // Child A (half 50,10,10) turned 90 degrees about +Z so it runs along Y; child B far away so Jolt keeps a compound.
    const int32_t a = aver_phys_shape_box(50, 10, 10, 0, nullptr, 0);
    const int32_t bsmall = aver_phys_shape_box(5, 5, 5, 0, nullptr, 0);
    const Quat q = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(90.0f));
    const int32_t kids[2] = {a, bsmall};
    const float pos[6] = {0, 0, 0, 200, 0, 0};
    const float rot[8] = {q.x, q.y, q.z, q.w, 0, 0, 0, 1};
    const int32_t user[2] = {7, 9};
    const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids, pos, rot, user, 2);
    check(comp != 0, "a static compound is created");
    const int32_t body = bodyOf(comp, 0, 0, 100.0f, AVER_PHYS_MOTION_STATIC, 0);
    float mn[3], mx[3];
    aver_phys_body_aabb(body, mn, mx);
    check(near(mn[1], -50.0f, 1.5f) && near(mx[1], 50.0f, 1.5f), "the rotated child spans y in [-50, 50], got " + f2s(mn[1]) + " .. " + f2s(mx[1]));
    check(near(mx[0], 205.0f, 1.5f), "the second child reaches x = 205, got " + f2s(mx[0]));
    float f[AVER_PHYS_SI_COUNT];
    int32_t iv[AVER_PHYS_SII_COUNT];
    check(shapeInfo(comp, f, iv) && iv[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_STATIC_COMPOUND &&
          iv[AVER_PHYS_SII_CHILDREN] == 2 && iv[AVER_PHYS_SII_LEAVES] == 2,
          "its info says a static compound of 2 children and 2 leaves");
    check(aver_phys_body_compound_count(body) == 2, "the body reports 2 children");
    aver_phys_shutdown();

    // IDENTITY ROTATION EQUALS THE OLD CREATOR: three boxes, dropped, same pose after 300 steps. Each in a fresh world at
    // the same spot: side by side (20 m apart) they land on different floats and settle 0.2 cm apart, which says nothing
    // about the shape.
    const float offs[9] = {0, 0, 80, -50, 0, 20, 50, 0, 20};
    const float halves[9] = {60, 30, 10, 10, 10, 20, 10, 10, 20};
    float p1[3], p2[3];
    floorWorld();
    const int32_t legacy = aver_phys_add_dynamic_compound_boxes(0, 0, 200.0f, offs, halves, 3, 12.0f);
    stepN(300);
    aver_phys_body_position(legacy, p1);
    aver_phys_shutdown();
    floorWorld();
    int32_t boxes[3];
    for (int i = 0; i < 3; ++i) boxes[i] = aver_phys_shape_box(halves[i*3], halves[i*3+1], halves[i*3+2], 0, nullptr, 0);
    const int32_t fresh = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, boxes, offs, nullptr, nullptr, 3);
    const int32_t nb = bodyOf(fresh, 0, 0, 200.0f, AVER_PHYS_MOTION_DYNAMIC, 12.0f);
    stepN(300);
    aver_phys_body_position(nb, p2);
    check(near(p1[2], p2[2], 1.0e-3f), "the builder compound rests where the old creator's does: " + f2s(p1[2]) + " vs " + f2s(p2[2]));
    aver_phys_shutdown();
}

static void testMutableCompound() {
    AVER_INFO("=== a mutable compound gains and loses children ===");
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);

    const int32_t box = aver_phys_shape_box(20, 20, 20, 0, nullptr, 0);
    const int32_t kids[1] = {box};
    const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_MUTABLE, kids, nullptr, nullptr, nullptr, 1);
    check(comp != 0, "a mutable compound of one child is created");
    const int32_t body = bodyOf(comp, 0, 0, 500.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    check(body != 0, "a body is made from it");
    check(bodyOf(comp, 0, 0, 900.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f) == 0 && aver_phys_last_error() == kInvalidArgument,
          "a second body from the same mutable compound is refused");
    check(aver_phys_body_compound_count(body) == 1, "one child to start");

    float point[3], normal[3];
    int32_t entity = 0;
    check(aver_phys_raycast(100, 0, 800, 0, 0, -1, 1000, point, normal, &entity) == 0, "a ray down x = 100 misses before the add");

    const float at[3] = {100, 0, 0};
    const int32_t idx = aver_phys_body_compound_add(body, box, at, nullptr, 5);
    check(idx == 1, "the new child has index 1, got " + std::to_string(idx));
    check(aver_phys_body_compound_count(body) == 2, "the body reports 2 children");
    check(near(aabbSize(body, 0), 140.0f, 2.0f), "the AABB grows to 140 along X, got " + f2s(aabbSize(body, 0)));
    check(near(massOf(body), 10.0f, 0.02f), "the mass is kept at 10 kg, got " + f2s(massOf(body)));
    check(aver_phys_raycast(100, 0, 800, 0, 0, -1, 1000, point, normal, &entity) == body, "a ray down x = 100 now hits the new child");
    check(aver_phys_raycast(0, 0, 800, 0, 0, -1, 1000, point, normal, &entity) == body, "and the first child is still where it was");
    float origin[3];
    aver_phys_body_origin(body, origin);
    check(near(origin[0], 0.0f, 0.01f) && near(origin[2], 500.0f, 0.01f), "the body origin did not move");

    check(aver_phys_body_compound_set_child(body, 1, nullptr, nullptr) == 1, "set_child with NULL position and rotation keeps the child");
    check(aver_phys_body_compound_remove(body, 1) == 1, "the child is removed");
    check(aver_phys_body_compound_count(body) == 1, "one child again");
    check(aver_phys_raycast(100, 0, 800, 0, 0, -1, 1000, point, normal, &entity) == 0, "the ray misses again");
    check(aver_phys_body_compound_remove(body, 0) == 0, "the last child cannot be removed");
    check(aver_phys_body_compound_add(body, 99999, at, nullptr, 0) == -1, "an unknown child handle gives -1");

    const int32_t plain = aver_phys_add_dynamic_sphere(1000, 0, 500, 10, 1);
    check(aver_phys_body_compound_count(plain) == -1, "a sphere body is not a compound: -1");
    check(aver_phys_body_compound_add(plain, box, at, nullptr, 0) == -1, "and takes no children");

    aver_phys_shutdown();
}

static void testSetShapeAndScale() {
    AVER_INFO("=== set_shape and set_scale ===");
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);

    const int32_t sphere = aver_phys_shape_sphere(20, 0, nullptr, 0);
    const int32_t box = aver_phys_shape_box(20, 20, 20, 0, nullptr, 0);
    int32_t b = bodyOf(sphere, 0, 0, 500.0f, AVER_PHYS_MOTION_DYNAMIC, 5.0f);
    check(near(massOf(b), 5.0f, 0.01f), "a 5 kg sphere body");
    check(aver_phys_body_set_shape(b, box, 1) == 1, "its shape is swapped for a box, keeping the mass");
    check(near(massOf(b), 5.0f, 0.01f) && near(posZ(b), 500.0f, 0.01f), "the mass is 5 kg and the body did not move");
    check(aver_phys_body_set_shape(b, sphere, 0) == 1, "back to a sphere, taking the shape's own mass");
    // 4/3 pi (0.2 m)^3 * 1000 kg/m^3 = 33.51 kg
    check(near(massOf(b), 33.51f, 0.8f), "the mass is the sphere's volume times density, got " + f2s(massOf(b)));

    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, 2000.0f, 0, 100.0f);
    check(aver_phys_body_set_shape(ch, box, 1) == 0, "a character cannot take set_shape");
    const float verts[12] = {0, 0, 0, 50, 0, 0, 0, 50, 0, 0, 0, 50};
    const int32_t tris[12] = {0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2};
    const int32_t soft = aver_phys_softbody_create(verts, 4, tris, 12, nullptr, 3000.0f, 0, 500.0f, 0.0f, 0.0f, 0.1f, 5);
    if (soft != 0) check(aver_phys_body_set_shape(soft, box, 1) == 0, "nor can a soft body");

    // set_scale is ABSOLUTE and mass follows volume.
    int32_t c = bodyOf(box, 0, 3000.0f, 500.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    const float two[3] = {2, 2, 2};
    check(aver_phys_body_set_scale(c, two, 0) == 1, "a box body takes scale 2");
    check(near(aabbSize(c, 0), 80.0f, 2.0f), "its AABB is 80 wide, got " + f2s(aabbSize(c, 0)));
    check(near(massOf(c), 80.0f, 1.0f), "mass scales by 8 to 80 kg, got " + f2s(massOf(c)));
    aver_phys_body_set_scale(c, two, 0);
    check(near(aabbSize(c, 0), 80.0f, 2.0f) && near(massOf(c), 80.0f, 1.0f), "scale 2 again changes nothing: it is absolute");
    float s[3] = {0, 0, 0};
    check(aver_phys_body_scale(c, s) == 1 && near(s[0], 2.0f, 0.01f), "body_scale reads 2");
    const float three[3] = {3, 3, 3};
    aver_phys_body_set_scale(c, three, 1);
    check(near(aabbSize(c, 0), 120.0f, 3.0f) && near(massOf(c), 80.0f, 1.0f), "scale 3 with keepMass is 120 wide at the same 80 kg");
    const float none[3] = {0, 1, 1};
    check(aver_phys_body_set_scale(c, none, 0) == 0, "a zero scale component is refused");

    // A body made by aver_phys_add_mesh_shape_body (already scaled) is re-scaled absolutely.
    const MeshData m = boxMesh(20.0f);
    const int32_t ms = aver_phys_create_mesh_shape(m.v.data(), static_cast<int32_t>(m.v.size() / 3), m.idx.data(), static_cast<int32_t>(m.idx.size()));
    const int32_t mb = aver_phys_add_mesh_shape_body(ms, 0, 4000.0f, 500.0f, 0, 0, 0, 1, 2, 2, 2);
    check(near(aabbSize(mb, 0), 80.0f, 2.0f), "a placement scaled 2 is 80 wide");
    const float one[3] = {1, 1, 1};
    aver_phys_body_set_scale(mb, one, 0);
    check(near(aabbSize(mb, 0), 40.0f, 2.0f), "set_scale(1) makes it 40 wide, got " + f2s(aabbSize(mb, 0)));

    aver_phys_shutdown();
}

static void testIntrospection() {
    AVER_INFO("=== shape info ===");
    aver_phys_init();

    const int32_t box = aver_phys_shape_box(20, 30, 40, 0, nullptr, 0);   // 40 x 60 x 80 cm
    float f[AVER_PHYS_SI_COUNT];
    int32_t iv[AVER_PHYS_SII_COUNT];
    check(shapeInfo(box, f, iv), "info of a box");
    check(near(f[AVER_PHYS_SI_VOLUME_CM3], 192000.0f, 200.0f), "volume 192000 cm^3, got " + f2s(f[AVER_PHYS_SI_VOLUME_CM3]));
    check(near(f[AVER_PHYS_SI_MASS_KG], 192.0f, 0.5f), "mass 192 kg at density 1000, got " + f2s(f[AVER_PHYS_SI_MASS_KG]));
    check(near(f[AVER_PHYS_SI_BOUNDS_MIN_X], -20.0f, 0.5f) && near(f[AVER_PHYS_SI_BOUNDS_MAX_Z], 40.0f, 0.5f), "bounds are the half extents");
    check(iv[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_BOX && iv[AVER_PHYS_SII_LEAVES] == 1 && iv[AVER_PHYS_SII_HAS_MASS] == 1 &&
          iv[AVER_PHYS_SII_MUST_BE_STATIC] == 0, "type BOX, one leaf, has a mass, not static-only");

    // THE INERTIA FRAME: rebuild the engine-axis tensor from the principal moments and the frame quaternion. For a
    // solid box of mass m and full sizes (a, b, c) along x, y, z: Ixx = m/12 (b^2 + c^2) and so on.
    const Quat q(f[AVER_PHYS_SI_INERTIA_FRAME_X], f[AVER_PHYS_SI_INERTIA_FRAME_Y], f[AVER_PHYS_SI_INERTIA_FRAME_Z], f[AVER_PHYS_SI_INERTIA_FRAME_W]);
    const Vec3 axis[3] = {q.rotate(Vec3(1, 0, 0)), q.rotate(Vec3(0, 1, 0)), q.rotate(Vec3(0, 0, 1))};
    const f32 d[3] = {f[AVER_PHYS_SI_INERTIA_PRINCIPAL_X], f[AVER_PHYS_SI_INERTIA_PRINCIPAL_Y], f[AVER_PHYS_SI_INERTIA_PRINCIPAL_Z]};
    f32 t[3][3] = {};
    for (int k = 0; k < 3; ++k) {
        const f32 c[3] = {axis[k].x, axis[k].y, axis[k].z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t[i][j] += d[k] * c[i] * c[j];
    }
    const f32 m = 192.0f;
    const f32 ixx = m / 12.0f * (60.0f * 60.0f + 80.0f * 80.0f);
    const f32 iyy = m / 12.0f * (40.0f * 40.0f + 80.0f * 80.0f);
    const f32 izz = m / 12.0f * (40.0f * 40.0f + 60.0f * 60.0f);
    check(near(t[0][0], ixx, ixx * 0.02f) && near(t[1][1], iyy, iyy * 0.02f) && near(t[2][2], izz, izz * 0.02f),
          "the reconstructed tensor diagonal is (" + f2s(ixx) + ", " + f2s(iyy) + ", " + f2s(izz) + "), got (" +
          f2s(t[0][0]) + ", " + f2s(t[1][1]) + ", " + f2s(t[2][2]) + ")");
    check(near(t[0][1], 0.0f, ixx * 0.02f) && near(t[0][2], 0.0f, ixx * 0.02f) && near(t[1][2], 0.0f, ixx * 0.02f),
          "and its off-diagonal terms are zero");

    const MeshData mesh = boxMesh(20.0f);
    const int32_t ms = meshShape(mesh);
    check(shapeInfo(ms, f, iv) && iv[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_MESH && iv[AVER_PHYS_SII_TRIANGLES] == 12 &&
          iv[AVER_PHYS_SII_HAS_MASS] == 0 && iv[AVER_PHYS_SII_MUST_BE_STATIC] == 1,
          "a mesh: type MESH, 12 triangles, no mass, static only");

    check(aver_phys_shape_info(99999, f, AVER_PHYS_SI_COUNT, iv, AVER_PHYS_SII_COUNT) == 0 && aver_phys_last_error() == kBadHandle,
          "an unknown handle gives BadHandle");
    check(aver_phys_shape_info(box, nullptr, 1, iv, AVER_PHYS_SII_COUNT) == 0, "a count with no float pointer is refused");

    aver_phys_shutdown();
}

static void testMeshOptions() {
    AVER_INFO("=== mesh palette, user data, options ===");
    aver_phys_init();

    const MeshData quad = quadMesh(200.0f, 0.0f);
    const uint8_t triPal[2] = {0, 1};
    const int32_t palette[2] = {111, 222};
    const int32_t user[2] = {40, 41};
    const int32_t shape = meshShape(quad, triPal, palette, 2, user);
    check(shape != 0, "a mesh with a two-entry palette and per-triangle user data builds");
    const int32_t body = bodyOf(shape, 0, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    float xyz[9 * 4];
    int32_t surf[4] = {-1, -1, -1, -1};
    const float lo[3] = {-300, -300, -10}, hi[3] = {300, 300, 10};
    const int32_t n = aver_phys_body_triangles(body, lo, hi, xyz, surf, 4);
    check(n == 2, "the body reports 2 triangles, got " + std::to_string(n));
    const bool has111 = (surf[0] == 111 || surf[1] == 111), has222 = (surf[0] == 222 || surf[1] == 222);
    check(has111 && has222, "and one triangle carries each palette surface");
    check(near(xyz[2], 0.0f, 0.5f), "triangle vertices are in engine cm: z = 0");

    int32_t big[33];
    uint8_t bigIdx[2] = {0, 32};
    for (int i = 0; i < 33; ++i) big[i] = 1000 + i;
    check(meshShape(quad, bigIdx, big, 33) == 0 && aver_phys_last_error() == kInvalidArgument, "a palette of 33 is refused");
    check(meshShape(quad, nullptr, nullptr, 0, nullptr, nullptr, 7) == 0, "a parameter block of 7 entries is refused");

    // EDGE_ANGLE_DEG -1 (every edge active) and the build-speed quality both build; rays agree with the default build.
    float allEdges[4] = {-1, -1, -1, -1};
    allEdges[AVER_PHYS_SP_EDGE_ANGLE_DEG] = -1.0f;
    check(meshShape(quad, nullptr, nullptr, 0, nullptr, allEdges, 4) != 0, "every edge active builds");
    float fast[6] = {-1, -1, -1, 5, 1, 8};
    const int32_t fastShape = meshShape(quad, nullptr, nullptr, 0, nullptr, fast, 6);
    check(fastShape != 0, "favour-build-speed builds");
    const int32_t plainShape = meshShape(quad);
    const int32_t b1 = bodyOf(fastShape, 1000.0f, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t b2 = bodyOf(plainShape, 3000.0f, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    float p1[3], p2[3], nrm[3];
    int32_t e;
    check(aver_phys_raycast(1010, 10, 300, 0, 0, -1, 1000, p1, nrm, &e) == b1 && aver_phys_raycast(3010, 10, 300, 0, 0, -1, 1000, p2, nrm, &e) == b2 &&
          near(p1[2], p2[2], 1.0e-3f), "a ray lands at the same height on both builds");

    aver_phys_shutdown();
}

static void testHull() {
    AVER_INFO("=== hulls ===");
    floorWorld();

    std::vector<float> cube;
    for (int i = 0; i < 8; ++i) {
        cube.push_back((i & 1) ? 15.0f : -15.0f);
        cube.push_back((i & 2) ? 15.0f : -15.0f);
        cube.push_back((i & 4) ? 15.0f : -15.0f);
    }
    float r0[1] = {0.0f}, r5[1] = {5.0f};
    const int32_t h0 = aver_phys_shape_convex_hull(cube.data(), 8, 0, r0, 1);
    const int32_t h5 = aver_phys_shape_convex_hull(cube.data(), 8, 0, r5, 1);
    check(h0 != 0 && h5 != 0, "a 30 cm cube hull builds with convex radius 0 and 5");
    const f32 z0 = restHeight(h0, 5.0f), z5 = restHeight(h5, 5.0f);
    check(near(z0, 15.0f, kSlopCm + 0.5f) && near(z5, 15.0f, kSlopCm + 0.5f), "both rest at 15 cm: " + f2s(z0) + " and " + f2s(z5));

    std::vector<float> cloud;
    for (int i = 0; i < 300; ++i) {
        const f32 a = static_cast<f32>(i) * 0.6180339f * 6.2831853f, b = std::acos(1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / 300.0f);
        cloud.push_back(50.0f * std::sin(b) * std::cos(a));
        cloud.push_back(50.0f * std::sin(b) * std::sin(a));
        cloud.push_back(50.0f * std::cos(b));
    }
    check(aver_phys_shape_convex_hull(cloud.data(), 300, 0, nullptr, 0) != 0, "a 300-point cloud builds");
    check(aver_phys_shape_convex_hull(cube.data(), 3, 0, nullptr, 0) == 0, "fewer than 4 points is refused");

    aver_phys_shutdown();
}

static void testDynamicMesh() {
    AVER_INFO("=== a dynamic mesh body, and its documented limit ===");
    floorWorld();
    const MeshData cubeMesh = boxMesh(20.0f);
    const int32_t shape = meshShape(cubeMesh);

    check(bodyOf(shape, 0, 0, 200.0f, AVER_PHYS_MOTION_DYNAMIC, 0) == 0 && aver_phys_last_error() == kUnsupported,
          "a mesh with no mass cannot be dynamic");
    const int32_t body = bodyOf(shape, 0, 0, 200.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    check(body != 0, "a mesh with a mass can");
    check(near(massOf(body), 10.0f, 0.05f), "its mass is 10 kg");
    stepN(400);
    check(near(posZ(body), 20.0f, 3.0f), "it falls onto a convex floor and rests with its centre at 20 cm, got " + f2s(posZ(body)));

    // An off-centre impulse spins it: the inertia is a box's, not zero.
    aver_phys_set_gravity(0, 0, 0);
    aver_phys_body_set_velocity(body, 0, 0, 0);
    aver_phys_body_add_impulse_at(body, 0, 50.0f, 0, 0, 0, 40.0f);
    float w[3] = {0, 0, 0};
    aver_phys_body_angular_velocity(body, w);
    check(std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) > 0.01f, "an off-centre impulse gives it an angular velocity");
    aver_phys_shutdown();

    // THE LIMIT: no mesh-mesh collision. A dynamic mesh dropped on a static mesh floor falls through (pinned so a
    // Jolt change is noticed).
    aver_phys_init();
    const MeshData floorMesh = quadMesh(2000.0f, 0.0f);
    bodyOf(meshShape(floorMesh), 0, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t falling = bodyOf(shape, 0, 0, 200.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    stepN(300);
    check(posZ(falling) < -50.0f, "it falls through a mesh floor, z = " + f2s(posZ(falling)));

    // set_motion_type(dynamic) on a mass-less mesh body is still refused.
    const int32_t staticMesh = aver_phys_add_mesh(cubeMesh.v.data(), static_cast<int32_t>(cubeMesh.v.size() / 3), cubeMesh.idx.data(),
                                                  static_cast<int32_t>(cubeMesh.idx.size()), 5000.0f, 0, 0);
    check(aver_phys_body_set_motion_type(staticMesh, AVER_PHYS_MOTION_DYNAMIC) == 0, "set_motion_type(dynamic) on a mass-less mesh is refused");
    aver_phys_shutdown();
}

static void testLifetime() {
    AVER_INFO("=== handle lifetime ===");
    floorWorld();
    const int32_t before = aver_phys_shape_count();
    for (int i = 0; i < 20; ++i) {
        const int32_t s = aver_phys_shape_sphere(10, 0, nullptr, 0);
        aver_phys_shape_release(s);
    }
    check(aver_phys_shape_count() == before, "a build-and-release loop returns the live count to " + std::to_string(before));

    const int32_t s = aver_phys_shape_box(20, 20, 20, 0, nullptr, 0);
    const int32_t body = bodyOf(s, 0, 0, 300.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    check(aver_phys_shape_release(s) == 1, "the shape is released while a body uses it");
    stepN(400);
    check(near(posZ(body), 20.0f, 1.5f), "the body still stands on its shape");
    check(aver_phys_shape_release(s) == 0 && aver_phys_last_error() == kBadHandle, "releasing twice is a BadHandle");
    check(bodyOf(s, 0, 0, 300.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f) == 0 && aver_phys_last_error() == kBadHandle, "a released handle makes no body");

    const int32_t t = aver_phys_shape_box(5, 5, 5, 0, nullptr, 0);
    check(aver_phys_release_mesh_shape(t) == 1, "the old release call releases a new-style handle");

    // The shape of a body, as a new handle.
    const int32_t of = aver_phys_shape_of_body(body);
    float f[AVER_PHYS_SI_COUNT];
    int32_t iv[AVER_PHYS_SII_COUNT];
    check(of != 0 && shapeInfo(of, f, iv) && iv[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_BOX, "shape_of_body gives a box handle");
    aver_phys_shape_release(of);
    aver_phys_shutdown();
}

// Builds every old-creator shape both ways and returns the settled heights, old then new, per shape.
static void testOldCreatorsMatch() {
    AVER_INFO("=== the old creators against the new builders ===");
    floorWorld();

    struct Pair { int32_t oldBody, newBody; const char* name; };
    std::vector<Pair> pairs;
    pairs.push_back({aver_phys_add_dynamic_capsule(0, 0, 250, 20, 120, 8), bodyOf(aver_phys_shape_capsule(20, 120, 0, nullptr, 0), 2000, 0, 250, AVER_PHYS_MOTION_DYNAMIC, 8), "capsule"});
    pairs.push_back({aver_phys_add_dynamic_cylinder(0, 600, 250, 30, 80, 8), bodyOf(aver_phys_shape_cylinder(30, 80, 0, nullptr, 0), 2000, 600, 250, AVER_PHYS_MOTION_DYNAMIC, 8), "cylinder"});
    pairs.push_back({aver_phys_add_dynamic_tapered_capsule(0, 1200, 250, 15, 25, 140, 8), bodyOf(aver_phys_shape_tapered_capsule(15, 25, 140, 0, nullptr, 0), 2000, 1200, 250, AVER_PHYS_MOTION_DYNAMIC, 8), "tapered capsule"});
    stepN(300);
    for (const Pair& p : pairs) {
        float a[3], b[3];
        aver_phys_body_position(p.oldBody, a);
        aver_phys_body_position(p.newBody, b);
        check(near(a[2], b[2], 1.0e-3f), std::string(p.name) + ": old and new settle at the same height (" + f2s(a[2]) + " vs " + f2s(b[2]) + ")");
    }
    aver_phys_shutdown();
}

static f32 settledChassisHeight(bool fromShape) {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 20000.0f, 20000.0f, 50.0f);
    const f32 radius = 33.0f, susp = 30.0f, hz = 2.0f;
    const f32 sag = 981.0f / ((2.0f * kPi * hz) * (2.0f * kPi * hz));
    const f32 attachZ = radius + susp - sag;
    const int32_t v = aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1);
    for (int i = 0; i < 4; ++i)
        aver_phys_vehicle_add_wheel(v, i < 2 ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, radius, 22, 5, susp, hz, 0.5f,
                                    i < 2 ? 35.0f : 0.0f, 1500, 2500, i >= 2 ? 1 : 0);
    const float boxes[12] = {0, 0, 60, 200, 90, 30, 0, 0, 110, 100, 60, 15};
    if (!fromShape) {
        aver_phys_vehicle_set_chassis_boxes(v, boxes, 2);
    } else {
        const int32_t a = aver_phys_shape_box(boxes[3], boxes[4], boxes[5], 0, nullptr, 0);
        const int32_t b = aver_phys_shape_box(boxes[9], boxes[10], boxes[11], 0, nullptr, 0);
        const int32_t kids[2] = {a, b};
        const float pos[6] = {boxes[0], boxes[1], boxes[2], boxes[6], boxes[7], boxes[8]};
        const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids, pos, nullptr, nullptr, 2);
        if (aver_phys_vehicle_set_chassis_shape(v, comp) != 1) return -1000.0f;
    }
    aver_phys_vehicle_finish(v, 60.0f);
    stepN(300);
    float p[3] = {0, 0, 0}, q[4];
    aver_phys_vehicle_pose(v, p, q);
    const f32 z = p[2];
    // After _finish the chassis cannot be swapped.
    const int32_t one = aver_phys_shape_box(10, 10, 10, 0, nullptr, 0);
    if (fromShape && aver_phys_vehicle_set_chassis_shape(v, one) != 0) return -2000.0f;
    aver_phys_shutdown();
    return z;
}

static void testVehicleChassis() {
    AVER_INFO("=== a vehicle chassis from a shape ===");
    const f32 zBoxes = settledChassisHeight(false);
    const f32 zShape = settledChassisHeight(true);
    // The origin settles about 1.4 cm under the floor (Jolt's spring sag runs a little over the hand-worked one; VehicleTest
    // allows 2.5 cm, and the old box chassis gives the same figure).
    check(near(zBoxes, 0.0f, 2.5f) && near(zShape, 0.0f, 2.5f), "both cars settled on the floor (" + f2s(zBoxes) + ", " + f2s(zShape) + "; -1000 = chassis call failed, -2000 = swap after finish allowed)");
    check(near(zBoxes, zShape, 0.5f), "the shape chassis settles at the box chassis' height: " + f2s(zBoxes) + " vs " + f2s(zShape));
}

static f32 determinismRun(std::vector<f32>& out) {
    floorWorld();
    const int32_t box = aver_phys_shape_box(20, 15, 10, 0, nullptr, 0);
    const int32_t cap = aver_phys_shape_capsule(15, 100, 0, nullptr, 0);
    const int32_t kids[2] = {box, cap};
    const float pos[6] = {0, 0, 0, 40, 0, 0};
    const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids, pos, nullptr, nullptr, 2);
    const int32_t b = bodyOf(comp, 0, 0, 300.0f, AVER_PHYS_MOTION_DYNAMIC, 9.0f);
    aver_phys_body_add_impulse_at(b, 30, 10, 0, 5, 5, 320);
    stepN(300);
    float p[3], q[4];
    aver_phys_body_position(b, p);
    aver_phys_body_rotation(b, q);
    out.assign({p[0], p[1], p[2], q[0], q[1], q[2], q[3]});
    aver_phys_shutdown();
    return p[2];
}

static void testDeterminism() {
    AVER_INFO("=== two fresh worlds replay bit for bit ===");
    std::vector<f32> a, b;
    determinismRun(a);
    determinismRun(b);
    check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0, "the same script gives the same bits");
}

static int32_t detailRay(float x, float y, float fromZ, float* f, int32_t* iv) {
    return aver_phys_raycast_detail(x, y, fromZ, 0, 0, -1, 1000.0f, 0xFFFFFFFFu, 0, 0, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT);
}

static void testRaycastDetail() {
    AVER_INFO("=== raycast_detail: compound child, mesh triangle user data, palette ===");
    aver_phys_init();

    // A compound's children report their index and user data.
    const int32_t a = aver_phys_shape_box(50, 50, 20, 0, nullptr, 0);
    const int32_t b = aver_phys_shape_box(50, 50, 20, 0, nullptr, 0);
    const int32_t kids[2] = {a, b};
    const float pos[6] = {-100, 0, 0, 100, 0, 0};
    const int32_t user[2] = {7, 9};
    const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids, pos, nullptr, user, 2);
    const int32_t body = bodyOf(comp, 0, 0, 100.0f, AVER_PHYS_MOTION_STATIC, 0);
    float f[AVER_PHYS_HITF_COUNT];
    int32_t iv[AVER_PHYS_HITI_COUNT];
    check(detailRay(-100, 0, 400, f, iv) == body && iv[AVER_PHYS_HITI_CHILD] == 0 && iv[AVER_PHYS_HITI_CHILD_USER_DATA] == 7,
          "a ray through the first child reports child 0 and its user data 7");
    check(detailRay(100, 0, 400, f, iv) == body && iv[AVER_PHYS_HITI_CHILD] == 1 && iv[AVER_PHYS_HITI_CHILD_USER_DATA] == 9,
          "and through the second child 1 and 9");
    check(near(f[AVER_PHYS_HITF_POINT_Z], 120.0f, 0.5f) && near(f[AVER_PHYS_HITF_DISTANCE_CM], 280.0f, 0.5f), "at the child's top, 280 cm along the ray");

    // A mesh built with per-triangle user data and a palette: the triangle a ray lands in names both.
    const MeshData quad = quadMesh(200.0f, 0.0f);
    const uint8_t triPal[2] = {0, 1};
    const int32_t palette[2] = {111, 222};
    const int32_t triUser[2] = {40, 41};
    const int32_t withData = meshShape(quad, triPal, palette, 2, triUser);
    const int32_t mb = bodyOf(withData, 1000.0f, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    // quadMesh's first triangle covers x > y, the second y > x (in the mesh's own frame).
    check(detailRay(1100, -100, 100, f, iv) == mb && iv[AVER_PHYS_HITI_TRIANGLE] == 40 && iv[AVER_PHYS_HITI_SURFACE] == 111,
          "a ray in the first triangle reports user data 40 and surface 111, got " + std::to_string(iv[AVER_PHYS_HITI_TRIANGLE]) + " / " + std::to_string(iv[AVER_PHYS_HITI_SURFACE]));
    check(detailRay(900, 100, 100, f, iv) == mb && iv[AVER_PHYS_HITI_TRIANGLE] == 41 && iv[AVER_PHYS_HITI_SURFACE] == 222,
          "and in the second 41 and 222");
    check(f[AVER_PHYS_HITF_FRICTION] == -1.0f && iv[AVER_PHYS_HITI_CHILD] == -1 && iv[AVER_PHYS_HITI_CELL_X] == -1,
          "an unregistered surface defers (-1); no child, no cell");

    // Without per-triangle user data the field reads -1, and without a palette the surface is 0.
    const int32_t plain = meshShape(quad);
    const int32_t pb = bodyOf(plain, 3000.0f, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
    check(detailRay(3100, -100, 100, f, iv) == pb && iv[AVER_PHYS_HITI_TRIANGLE] == -1 && iv[AVER_PHYS_HITI_SURFACE] == 0,
          "a mesh built with neither reports triangle -1 and surface 0");
    aver_phys_shutdown();
}

static void testLeafFriction() {
    AVER_INFO("=== a leaf material replaces the body friction in a contact ===");
    floorWorld();
    const int32_t ice = aver_phys_surface_register("SBIce", 0.02f, 0.0f, 0);
    const int32_t rubber = aver_phys_surface_register("SBRubber", 1.0f, 0.0f, 0);
    int32_t boxes[2];
    for (int k = 0; k < 2; ++k) {
        const int32_t shape = aver_phys_shape_box(20, 20, 20, k == 0 ? ice : rubber, nullptr, 0);
        boxes[k] = bodyOf(shape, -3000.0f, k == 0 ? -1000.0f : 1000.0f, 20.5f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    }
    stepN(30);
    for (int32_t b : boxes) aver_phys_body_set_velocity(b, 500.0f, 0, 0);
    stepN(600);
    float pi[3], pr[3];
    aver_phys_body_position(boxes[0], pi);
    aver_phys_body_position(boxes[1], pr);
    const f32 travelIce = pi[0] + 3000.0f, travelRubber = pr[0] + 3000.0f;
    check(travelIce > 4.0f * travelRubber && travelRubber > 20.0f,
          "the Ice-leaf box slides >= 4x as far as the Rubber-leaf box: " + f2s(travelIce) + " vs " + f2s(travelRubber));
    float bodyFriction = 0.0f;
    aver_phys_body_friction(boxes[0], &bodyFriction);
    check(near(bodyFriction, 0.2f, 1.0e-4f), "while the body's own friction stays 0.2");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("generic shape handles: builders, bodies, compounds, scale, info");

    testLeavesRest();
    testPlaneTriangleEmpty();
    testTiltedPlaneSlides();
    testDecorators();
    testStaticCompound();
    testMutableCompound();
    testSetShapeAndScale();
    testIntrospection();
    testMeshOptions();
    testHull();
    testDynamicMesh();
    testLifetime();
    testOldCreatorsMatch();
    testVehicleChassis();
    testDeterminism();
    testRaycastDetail();
    testLeafFriction();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
