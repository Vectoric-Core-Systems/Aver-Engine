// ColliderShapesTest -- an .ocshape tree built into physics shapes, and the CollisionShape component (docs/PHYSICS_SHAPES.md).
//
//   1. the chair example builds into a static compound of six leaves with the volume the leaves add up to, rests on a floor, and
//      leaves exactly one live handle (every intermediate handle is released),
//   2. surfaces: the file's default reaches every leaf, a leaf's own `surface` wins, a mesh leaf's slot surfaces win over both,
//   3. `hull mesh` and `mesh` leaves read geometry through the injected MeshSource, and refuse an unknown path,
//   4. decorators and a mutable compound arrive as the shape types the file names; a refusal by the physics module leaves nothing allocated,
//   5. the component: apply and capture round-trip with golden keys, defaults omitted, `#hex` for a surface the registry does not
//      know, malformed keys refused before anything is attached, a re-apply keeping the runtime OUT fields, a CRigidBody attached.
//
// The margins are tolerances: a failing relation is a prompt to look at the physics before loosening the number. A real scene::World and a real physics world; no device, no window.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/formats/OcShape.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/ColliderShapes.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_surface_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
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
static const f32 kDt = 1.0f / 60.0f;

static const char* kChair = R"(OCSHAPE 1
NAME Chair
MASS 12
SURFACE Wood
NODE seat  box 40 40 4
NODE back  box 40 3 25
NODE leg   cylinder 3 45 radius 0.5
NODE top   compound static
CHILD top seat at 0 0 45
CHILD top back at -37 0 75 rot 0 0 0 1
CHILD top leg  at 32 32 22
CHILD top leg  at 32 -32 22
CHILD top leg  at -32 32 22
CHILD top leg  at -32 -32 22
ROOT top
)";

static fmt::OcShapeData parse(const char* text) {
    fmt::OcShapeData d;
    std::string err;
    if (!fmt::parseOcShape(text, d, &err)) AVER_ERROR("   (test shape did not parse: {})", err);
    return d;
}

struct Info {
    float f[AVER_PHYS_SI_COUNT] = {};
    int32_t i[AVER_PHYS_SII_COUNT] = {};
    bool ok = false;
};

static Info infoOf(int32_t shape) {
    Info r;
    r.ok = aver_phys_shape_info(shape, r.f, AVER_PHYS_SI_COUNT, r.i, AVER_PHYS_SII_COUNT) != 0;
    return r;
}

static f32 massOf(int32_t body) {
    float m = 0.0f;
    aver_phys_body_mass(body, &m);
    return m;
}

static void floorWorld() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);   // its top is z = 0
}

static void stepN(int n) {
    for (int i = 0; i < n; ++i) aver_phys_step(kDt);
}

// A static body from a shape at the origin, so a ray can read what the shape is made of.
static int32_t staticBody(int32_t shape, f32 x = 0, f32 y = 0, f32 z = 0) {
    const float o[3] = {x, y, z};
    return aver_phys_body_from_shape(shape, o, nullptr, AVER_PHYS_MOTION_STATIC, 0.0f, nullptr, 0);
}

// The surface id a ray straight down at (x, y) from z = 400 reports, or -1 for a miss.
static int32_t surfaceBelow(f32 x, f32 y) {
    float f[AVER_PHYS_HITF_COUNT] = {};
    int32_t i[AVER_PHYS_HITI_COUNT] = {};
    const int32_t body = aver_phys_raycast_detail(x, y, 400.0f, 0, 0, -1.0f, 1000.0f, 0xFFFFFFFFu, 0, 0, f, AVER_PHYS_HITF_COUNT, i, AVER_PHYS_HITI_COUNT);
    return body ? i[AVER_PHYS_HITI_SURFACE] : -1;
}

// A unit-ish cube mesh: 8 vertices, 12 triangles, half extent 10 cm.
// `bothSides` appends every triangle reversed, so a ray hits whichever way the physics module winds the faces.
static game::ColliderMesh cubeMesh(bool bothSides = false) {
    game::ColliderMesh m;
    const float s = 10.0f;
    for (int i = 0; i < 8; ++i) {
        m.vertices.push_back((i & 1) ? s : -s);
        m.vertices.push_back((i & 2) ? s : -s);
        m.vertices.push_back((i & 4) ? s : -s);
    }
    const int32_t quads[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto& q : quads) {
        m.indices.insert(m.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    }
    if (bothSides) {
        const size_t n = m.indices.size();
        for (size_t t = 0; t < n; t += 3) m.indices.insert(m.indices.end(), {m.indices[t], m.indices[t + 2], m.indices[t + 1]});
    }
    return m;
}

static void testChair() {
    AVER_INFO("=== the chair ===");
    floorWorld();
    const fmt::OcShapeData d = parse(kChair);
    const int32_t before = aver_phys_shape_count();
    int32_t shape = 0;
    std::string why;
    check(game::buildColliderShape(d, game::ColliderMeshSource{}, shape, why) && shape != 0, "the chair builds: " + why);
    check(aver_phys_shape_count() == before + 1, "exactly one handle is left: the root's (every intermediate handle was released)");

    const Info in = infoOf(shape);
    check(in.ok && in.i[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_STATIC_COMPOUND, "it is a static compound");
    check(in.i[AVER_PHYS_SII_CHILDREN] == 6 && in.i[AVER_PHYS_SII_LEAVES] == 6, "six children, six leaves");
    // seat 80x80x8 + back 80x6x50 + four legs of pi * 3^2 * 45
    const f32 volume = 80.0f * 80.0f * 8.0f + 80.0f * 6.0f * 50.0f + 4.0f * 3.14159265f * 9.0f * 45.0f;
    check(near(in.f[AVER_PHYS_SI_VOLUME_CM3], volume, volume * 0.015f), "the volume is the leaves' sum (" + std::to_string(in.f[AVER_PHYS_SI_VOLUME_CM3]) + " of " + std::to_string(volume) + ")");

    // As a dynamic body with the file's mass it falls and rests on its legs.
    const float o[3] = {0, 0, 200.0f};
    const int32_t body = aver_phys_body_from_shape(shape, o, nullptr, AVER_PHYS_MOTION_DYNAMIC, d.massKg, nullptr, 0);
    check(body != 0, "a dynamic body is made from it");
    check(near(massOf(body), 12.0f, 0.12f), "with the file's 12 kg");
    stepN(400);
    float lo[3] = {}, hi[3] = {};
    aver_phys_body_aabb(body, lo, hi);
    check(near(lo[2], 0.0f, 1.5f) && hi[2] > 70.0f, "it rests on its legs: the bounds' bottom is at the floor and the back is above 70 cm");
    aver_phys_remove_body(body);
    check(aver_phys_shape_release(shape) != 0 && aver_phys_shape_count() == before, "releasing the root returns the table to where it started");
    aver_phys_shutdown();
}

static void testSurfaces() {
    AVER_INFO("=== surfaces ===");
    aver_phys_init();
    const i32 wood = aver::surfaceIdFromName("Wood");
    const i32 ice = aver::surfaceIdFromName("Ice");
    const i32 rubber = aver::surfaceIdFromName("Rubber");

    int32_t shape = 0;
    std::string why;
    check(game::buildColliderShape(parse(kChair), game::ColliderMeshSource{}, shape, why), "chair builds: " + why);
    const int32_t chair = staticBody(shape);
    aver_phys_shape_release(shape);
    check(surfaceBelow(0, 0) == wood, "the file's default surface reaches the seat");
    check(surfaceBelow(-37, 0) == wood, "and the back");
    aver_phys_remove_body(chair);

    const fmt::OcShapeData own = parse("OCSHAPE 1\nSURFACE Wood\nNODE a box 20 20 5 surface Ice\nNODE b box 20 20 5\nNODE c compound static\n"
                                       "CHILD c a at -30 0 0\nCHILD c b at 30 0 0\nROOT c\n");
    check(game::buildColliderShape(own, game::ColliderMeshSource{}, shape, why), "a shape with one leaf naming Ice builds: " + why);
    const int32_t two = staticBody(shape, 0, 0, -5.0f);
    aver_phys_shape_release(shape);
    check(surfaceBelow(-30, 0) == ice, "a leaf's own surface wins over the default");
    check(surfaceBelow(30, 0) == wood, "the other leaf takes the default");
    aver_phys_remove_body(two);

    // A mesh leaf: slot 0 names Rubber, slot 1 names nothing and takes the file's default.
    game::ColliderMeshSource src;
    src.mesh = [&](const std::string& path, game::ColliderMesh& out) {
        if (path != "Meshes/cube.ocmesh") return false;
        out = cubeMesh(true);
        out.palette = {rubber, 0};
        out.triPalette.assign(out.indices.size() / 3, 0);
        for (size_t t = 0; t < out.triPalette.size(); ++t) out.triPalette[t] = (t % 12) >= 6 ? 1 : 0;   // quads 3 to 5 of each side are slot 1
        return true;
    };
    const fmt::OcShapeData mesh = parse("OCSHAPE 1\nSURFACE Wood\nNODE m mesh Meshes/cube.ocmesh\nROOT m\n");
    check(game::buildColliderShape(mesh, src, shape, why), "a mesh leaf builds through the seam: " + why);
    const Info in = infoOf(shape);
    check(in.i[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_MESH && in.i[AVER_PHYS_SII_TRIANGLES] >= 12, "it is a mesh of at least the cube's 12 triangles");
    const int32_t body = staticBody(shape, 0, 0, 0);
    aver_phys_shape_release(shape);
    // The cube's top face (z = +10) is the quad {4, 6, 7, 5} = triangles 2 and 3: slot 0, Rubber. (Quads 3 to 5, triangles 6 to 11, are
    // slot 1: the +Y, -X and +X faces, whose zero takes the file's default, Wood.)
    check(surfaceBelow(0, 0) == rubber, "the slot surface wins on the top face");
    aver_phys_remove_body(body);
    aver_phys_shutdown();
}

static void testMeshSeam() {
    AVER_INFO("=== hull mesh and mesh leaves ===");
    aver_phys_init();
    game::ColliderMeshSource src;
    src.mesh = [](const std::string& path, game::ColliderMesh& out) {
        if (path != "Meshes/cube.ocmesh") return false;
        out = cubeMesh();
        return true;
    };
    int32_t shape = 0;
    std::string why;
    const int32_t before = aver_phys_shape_count();
    check(game::buildColliderShape(parse("OCSHAPE 1\nNODE h hull mesh Meshes/cube.ocmesh\nROOT h\n"), src, shape, why), "hull mesh builds: " + why);
    check(infoOf(shape).i[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_CONVEX_HULL, "it is a convex hull");
    check(near(infoOf(shape).f[AVER_PHYS_SI_VOLUME_CM3], 8000.0f, 800.0f), "of the cube's volume");
    aver_phys_shape_release(shape);

    check(!game::buildColliderShape(parse("OCSHAPE 1\nNODE h hull mesh Meshes/ghost.ocmesh\nROOT h\n"), src, shape, why) && shape == 0, "an unknown mesh path is refused");
    check(why.find("ghost.ocmesh") != std::string::npos && why.find("not available") != std::string::npos, "saying which: " + why);
    check(!game::buildColliderShape(parse("OCSHAPE 1\nNODE h hull mesh Meshes/cube.ocmesh\nROOT h\n"), game::ColliderMeshSource{}, shape, why),
          "and a leaf is refused when the host gave no mesh source at all");
    check(aver_phys_shape_count() == before, "no refusal leaves a handle behind");
    aver_phys_shutdown();
}

static void testDecoratorsAndRefusals() {
    AVER_INFO("=== decorators, a mutable compound, a refusal ===");
    aver_phys_init();
    const int32_t before = aver_phys_shape_count();
    int32_t shape = 0;
    std::string why;
    const game::ColliderMeshSource none;

    const auto typeOf = [&](const char* text) {
        int32_t s = 0;
        std::string w;
        if (!game::buildColliderShape(parse(text), none, s, w)) { AVER_ERROR("   (build failed: {})", w); return -1; }
        const int32_t t = infoOf(s).i[AVER_PHYS_SII_TYPE];
        aver_phys_shape_release(s);
        return t;
    };
    check(typeOf("OCSHAPE 1\nNODE b box 5 5 5\nNODE r rt b at 10 0 0 rot 0 0 0.7071068 0.7071068\nROOT r\n") == AVER_PHYS_SHAPE_TYPE_ROTATED_TRANSLATED, "rt builds a rotated-translated shape");
    check(typeOf("OCSHAPE 1\nNODE b box 5 5 5\nNODE o com b at 0 0 -5\nROOT o\n") == AVER_PHYS_SHAPE_TYPE_OFFSET_COM, "com builds an offset-centre-of-mass shape");
    check(typeOf("OCSHAPE 1\nNODE b sphere 5\nNODE s scale b 2 1 1\nROOT s\n") != -1, "scale builds (a non-uniform sphere is baked by Jolt, so its type is Jolt's choice)");
    check(typeOf("OCSHAPE 1\nNODE a tcylinder 6 3 20\nROOT a\n") == AVER_PHYS_SHAPE_TYPE_TAPERED_CYLINDER, "tcylinder builds a tapered cylinder");
    check(typeOf("OCSHAPE 1\nNODE a plane 0 0 1 0 1000\nROOT a\n") == AVER_PHYS_SHAPE_TYPE_PLANE, "plane builds a plane");
    check(typeOf("OCSHAPE 1\nNODE a empty 0 0 5\nROOT a\n") == AVER_PHYS_SHAPE_TYPE_EMPTY, "empty builds an empty shape");
    check(typeOf("OCSHAPE 1\nNODE a triangle 0 0 0 20 0 0 0 20 0\nROOT a\n") == AVER_PHYS_SHAPE_TYPE_TRIANGLE, "triangle builds a convex triangle");
    check(typeOf("OCSHAPE 1\nNODE a hullpoints 0 0 0 10 0 0 0 10 0 0 0 10\nROOT a\n") == AVER_PHYS_SHAPE_TYPE_CONVEX_HULL, "hullpoints builds a convex hull");

    // A mutable compound: the body keeps the child count the file gave it, and a child can be added at run time.
    const char* grow = "OCSHAPE 1\nMASS 5\nNODE a box 10 10 10\nNODE b box 5 5 5\nNODE m compound mutable\nCHILD m a\nCHILD m b at 30 0 0\nROOT m\n";
    check(game::buildColliderShape(parse(grow), none, shape, why), "a mutable compound builds: " + why);
    check(infoOf(shape).i[AVER_PHYS_SII_TYPE] == AVER_PHYS_SHAPE_TYPE_MUTABLE_COMPOUND, "its type is mutable compound");
    const float o[3] = {0, 0, 100.0f};
    const int32_t body = aver_phys_body_from_shape(shape, o, nullptr, AVER_PHYS_MOTION_DYNAMIC, 5.0f, nullptr, 0);
    check(body != 0 && aver_phys_body_compound_count(body) == 2, "the body reports two children");
    aver_phys_shape_release(shape);

    // The physics module refuses four coplanar points as a hull; the file validates (four points), the build does not -- and the
    // handles made for the nodes before it are released.
    const int32_t mid = aver_phys_shape_count();
    const bool built = game::buildColliderShape(parse("OCSHAPE 1\nNODE ok box 5 5 5\nNODE flat hullpoints 0 0 0 10 0 0 0 10 0 10 10 0\n"
                                                      "NODE c compound static\nCHILD c ok\nCHILD c flat\nROOT c\n"), none, shape, why);
    check(!built && shape == 0, "a hull of four coplanar points is refused by the physics module");
    check(why.find("flat") != std::string::npos, "naming the node: " + why);
    check(aver_phys_shape_count() == mid, "and nothing built for the other nodes is left allocated");

    aver_phys_remove_body(body);
    check(aver_phys_shape_count() <= before + 1, "(the mutable compound's handle was released above)");
    aver_phys_shutdown();
}

static fmt::OcPlacedComponent rec(std::initializer_list<std::pair<const char*, const char*>> kv) {
    fmt::OcPlacedComponent r;
    r.type = "CollisionShape";
    for (const auto& p : kv) r.kv.emplace_back(p.first, p.second);
    return r;
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

static std::string show(const Pairs& p) {
    std::string s;
    for (const auto& kv : p) s += kv.first + "=" + kv.second + " ";
    return s;
}

static void testComponent() {
    AVER_INFO("=== the component ===");
    scene::World& w = scene::World::instance();
    const u32 type = game::registerCollisionShapeComponent(w);
    check(type != 0 && game::collisionShapeType(w) == type, "CCollisionShape is registered");
    check(game::registerCollisionShapeComponent(w) == type, "registering again returns the same id");
    check(w.componentSize(type) == 32 && w.componentVerified(type), "32 bytes, and the field table covers them");

    const std::function<std::string(u64)> noPaths = [](u64) { return std::string{}; };
    const std::function<std::string(u64)> paths = [](u64 id) { return id == fnv1a64(std::string_view("Physics/Chair.ocshape")) ? std::string("Physics/Chair.ocshape") : std::string{}; };

    aver_phys_init();
    aver_phys_surface_reset();
    aver_phys_surface_register("Wood", 0.5f, -1.0f, 0);

    const scene::Entity e = w.create("Chair", scene::kInvalidEntity, Transform{});
    std::string why;
    check(!game::captureCollisionShape(w, e, paths).has_value(), "an entity without the component captures nothing");
    check(game::applyCollisionShape(w, e, rec({{"asset", "Physics/Chair.ocshape"}, {"surface", "Wood"}}), why), "apply: " + why);
    const auto* c = w.component<game::CCollisionShape>(e, type);
    check(c && c->asset == fnv1a64(std::string_view("Physics/Chair.ocshape")) && c->surface == aver::surfaceIdFromName("Wood") && c->flags == 0,
          "the asset id is the path's fnv1a64 and the surface is the name's hash");
    check(w.hasComponent(e, scene::kComponentRigidBody), "a CRigidBody was attached (no level file carries one)");

    auto got = game::captureCollisionShape(w, e, paths);
    const Pairs want = {{"asset", "Physics/Chair.ocshape"}, {"surface", "Wood"}};
    check(got && got->type == "CollisionShape" && got->kv == want, "capture writes asset and surface, nothing else" + (got ? "  got [" + show(got->kv) + "]" : std::string(" got nothing")));

    // noscale, and a re-apply is an update that leaves the runtime OUT fields alone.
    auto* mc = w.component<game::CCollisionShape>(e, type);
    mc->outShape = 42;
    mc->outMassKg = 12.0f;
    check(game::applyCollisionShape(w, e, rec({{"asset", "Physics/Chair.ocshape"}, {"surface", "Wood"}, {"noscale", "1"}}), why), "apply again with noscale=1: " + why);
    mc = w.component<game::CCollisionShape>(e, type);
    check(mc->flags == game::kCollisionShapeIgnoreScale && mc->outShape == 42 && near(mc->outMassKg, 12.0f, 1e-6f), "the flag is set, the OUT fields are untouched");
    got = game::captureCollisionShape(w, e, paths);
    const Pairs want2 = {{"asset", "Physics/Chair.ocshape"}, {"surface", "Wood"}, {"noscale", "1"}};
    check(got && got->kv == want2, "capture adds noscale=1");
    check(game::applyCollisionShape(w, e, rec({{"asset", "Physics/Chair.ocshape"}}), why) && w.component<game::CCollisionShape>(e, type)->flags == 0 &&
          w.component<game::CCollisionShape>(e, type)->surface == 0, "a record without the keys resets them (the file is the truth)");

    // motion: the motion type of the CRigidBody the placement gets (no level file carries one).
    const auto motionOf = [&](scene::Entity ent) { return w.component<scene::CRigidBody>(ent, scene::kComponentRigidBody)->motionType; };
    check(motionOf(e) == 0, "a body attached by apply is static");
    check(game::applyCollisionShape(w, e, rec({{"asset", "Physics/Chair.ocshape"}, {"motion", "Dynamic"}}), why) && motionOf(e) == 2, "motion=Dynamic (any case) makes the body dynamic: " + why);
    got = game::captureCollisionShape(w, e, paths);
    const Pairs wantDyn = {{"asset", "Physics/Chair.ocshape"}, {"motion", "dynamic"}};
    check(got && got->kv == wantDyn, "capture writes motion=dynamic after the asset" + (got ? "  got [" + show(got->kv) + "]" : std::string()));
    check(game::applyCollisionShape(w, e, rec({{"asset", "Physics/Chair.ocshape"}}), why) && motionOf(e) == 0, "a record without motion is static (the canonical default; the record is the whole authored state)");
    check(game::applyCollisionShape(w, e, rec({{"motion", "kinematic"}}), why) && motionOf(e) == 1, "motion=kinematic");
    got = game::captureCollisionShape(w, e, paths);
    check(got && got->kv.size() == 1 && got->kv[0].first == "motion" && got->kv[0].second == "kinematic", "and captures as such");
    check(!game::applyCollisionShape(w, e, rec({{"motion", "flying"}}), why) && why.find("motion") != std::string::npos && motionOf(e) == 1, "an unknown motion is refused and changes nothing");
    check(game::applyCollisionShape(w, e, rec({{"motion", "static"}}), why) && motionOf(e) == 0, "motion=static puts it back");

    // An id the path table does not know, and a surface the registry does not know, survive as hex.
    mc = w.component<game::CCollisionShape>(e, type);
    mc->asset = 0x0123456789abcdefull;
    mc->surface = aver::surfaceIdFromName("Mystery");
    got = game::captureCollisionShape(w, e, noPaths);
    const Pairs wantHex = {{"asset#", "0123456789abcdef"}, {"surface", "#" + [] { char b[16]; std::snprintf(b, sizeof b, "%08x", static_cast<unsigned>(aver::surfaceIdFromName("Mystery"))); return std::string(b); }()}};
    check(got && got->kv == wantHex, "an unknown asset id is asset#<16 hex> and an unregistered surface is #<8 hex>" + (got ? "  got [" + show(got->kv) + "]" : std::string()));
    const scene::Entity e2 = w.create("Copy", scene::kInvalidEntity, Transform{});
    check(got && game::applyCollisionShape(w, e2, *got, why), "and that record applies: " + why);
    const auto* c2 = w.component<game::CCollisionShape>(e2, type);
    check(c2 && c2->asset == 0x0123456789abcdefull && c2->surface == aver::surfaceIdFromName("Mystery"), "to the same asset id and surface id");

    // Malformed keys are refused before anything is attached.
    const scene::Entity e3 = w.create("Bad", scene::kInvalidEntity, Transform{});
    check(!game::applyCollisionShape(w, e3, rec({{"asset#", "zz"}}), why) && !w.hasComponent(e3, type) && !w.hasComponent(e3, scene::kComponentRigidBody),
          "asset#=zz is refused and attaches nothing");
    check(!game::applyCollisionShape(w, e3, rec({{"surface", "Bad-Name"}}), why) && why.find("surface") != std::string::npos && !w.hasComponent(e3, type), "a bad surface name is refused");
    check(!game::applyCollisionShape(w, e3, rec({{"surface", "#xyz"}}), why) && !w.hasComponent(e3, type), "a malformed #hex surface is refused");
    check(!game::applyCollisionShape(w, scene::kInvalidEntity, rec({}), why), "a dead entity is refused");

    // A record with nothing in it is a valid empty component.
    check(game::applyCollisionShape(w, e3, rec({}), why) && w.hasComponent(e3, type), "a record with no keys attaches an empty component");
    got = game::captureCollisionShape(w, e3, paths);
    check(got && got->kv.empty(), "which captures as no keys");

    // Removing the component takes back the zero-size Rigid Body it attached, and leaves a sized one alone.
    check(game::removeCollisionShape(w, e3) && !w.hasComponent(e3, type) && !w.hasComponent(e3, scene::kComponentRigidBody),
          "removing the component takes the unusable zero-size Rigid Body it attached");
    if (auto* sized = w.component<scene::CRigidBody>(e2, scene::kComponentRigidBody)) sized->dims[0] = sized->dims[1] = sized->dims[2] = 5.0f;
    check(game::removeCollisionShape(w, e2) && !w.hasComponent(e2, type) && w.hasComponent(e2, scene::kComponentRigidBody),
          "a Rigid Body that has a size stays when the shape component goes");
    check(!game::removeCollisionShape(w, e2), "removing again reports nothing to remove");
    aver_phys_surface_reset();
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("ColliderShapes: .ocshape -> shapes, and the CollisionShape component");
    testChair();
    testSurfaces();
    testMeshSeam();
    testDecoratorsAndRefusals();
    testComponent();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
