// Hand-run test for Aver.Physics: the Aver<->Jolt conversion, then the simulation itself.
// Exit code = failure count. Same shape as tests/scene and tests/formats.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"

#include "../../../modules/physics/src/Convert.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::physics;

static int g_checks = 0, g_failures = 0;

// Counts one assertion and logs `what` when it fails.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// True when two scalars are within `eps`.
static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
// True when two vectors are within `eps` component-wise.
static bool vnear(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
// Formats a vector for a failure message.
static std::string str(const Vec3& v) {
    char b[96]; std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z); return b;
}

// Rotates a vector by a quaternion in Aver's own maths, reusing nothing from the conversion.
static Vec3 averRotate(const Quat& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const f32 s = q.w;
    const Vec3 uxv(u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x);
    const f32 dot = u.x*v.x + u.y*v.y + u.z*v.z;
    const f32 uu  = u.x*u.x + u.y*u.y + u.z*u.z;
    return Vec3(2.0f*dot*u.x + (s*s - uu)*v.x + 2.0f*s*uxv.x,
                2.0f*dot*u.y + (s*s - uu)*v.y + 2.0f*s*uxv.y,
                2.0f*dot*u.z + (s*s - uu)*v.z + 2.0f*s*uxv.z);
}

// Checks the basis directions, the unit scale, the round trip, and that the map flips handedness.
static void testAxisMap() {
    AVER_INFO("-- axis map --");
    check(toJoltUnit(Vec3(0,0,1)) == JPH::Vec3(0,1,0),  "Aver up (+Z) -> Jolt up (+Y)");
    check(toJoltUnit(Vec3(0,1,0)) == JPH::Vec3(1,0,0),  "Aver right (+Y) -> Jolt right (+X)");
    check(toJoltUnit(Vec3(1,0,0)) == JPH::Vec3(0,0,-1), "Aver forward (+X) -> Jolt forward (-Z)");

    check(near(toJolt(Vec3(0,0,100)).GetY(), 1.0f), "100cm up converts to 1m");
    check(vnear(fromJolt(JPH::Vec3(0,1,0)), Vec3(0,0,100)), "1m up converts back to 100cm");

    const Vec3 v(12.0f, -34.0f, 56.0f);
    check(vnear(fromJolt(toJolt(v)), v), "vector round trip: " + str(fromJolt(toJolt(v))));

    const JPH::Vec3 jf = toJoltUnit(Vec3(1,0,0)), jr = toJoltUnit(Vec3(0,1,0));
    check(jf.Cross(jr) == JPH::Vec3(0,-1,0),
          "handedness flips: Jolt fwd x right is DOWN, not up (det = -1)");
}

// Checks convert(rotate(q, v)) == rotate(convert(q), convert(v)) over 64 cases, then a yaw round trip.
static void testRotationMap() {
    AVER_INFO("-- rotation map --");
    const Vec3 axes[] = {Vec3(0,0,1), Vec3(1,0,0), Vec3(0,1,0), Vec3(0.577f,0.577f,0.577f)};
    const f32  angles[] = {0.3f, 1.0f, 2.4f, -0.8f};
    const Vec3 probes[] = {Vec3(1,0,0), Vec3(0,1,0), Vec3(0,0,1), Vec3(3,-4,5)};

    int mismatches = 0;
    for (const Vec3& a : axes) {
        for (f32 ang : angles) {
            const Quat q = Quat::fromAxisAngle(a, ang);
            for (const Vec3& p : probes) {
                const Vec3 want = averRotate(q, p);
                const JPH::Vec3 got = toJolt(q) * toJoltUnit(p);
                if (!vnear(fromJoltUnit(got), want, 2e-3f)) ++mismatches;
            }
        }
    }
    check(mismatches == 0,
          "rotate-then-convert == convert-then-rotate over 64 cases (" +
          std::to_string(mismatches) + " mismatched)");

    const Quat yaw = Quat::fromAxisAngle(Vec3(0,0,1), 1.2f);
    const Quat back = fromJolt(toJolt(yaw));
    check(near(back.x, yaw.x) && near(back.y, yaw.y) && near(back.z, yaw.z),
          "quaternion round trip preserves the axis");
}

// Checks convert(transform(M, p)) == transform(convert(M), convert(p)) over a spread of transforms.
//
// THE ONE PROPERTY A CALLER CAN NEVER SEE, and the reason this test links Jolt at all. The matrix
// conversion exists to hand Aver's animation palette to Jolt's soft-body skinning, and its two
// hazards are silent: Aver's Mat4 is ROW-VECTOR while Jolt's Mat44 is column-vector, and the basis
// change is a handedness flip. Get either wrong and a skinned soft body still simulates, still
// collides, and skins to the wrong place -- with nothing anywhere reporting an error.
//
// Falsified by dropping the negation on toJolt(Mat4)'s third column: rotations still round-trip and
// this check fails on every case with a rotation in it.
static void testMatrixMap() {
    AVER_INFO("-- matrix map --");
    const Vec3 axes[]   = {Vec3(0,0,1), Vec3(1,0,0), Vec3(0,1,0), Vec3(0.577f,0.577f,0.577f)};
    const f32  angles[] = {0.0f, 0.4f, 1.6f, -1.1f};
    const Vec3 trans[]  = {Vec3(0,0,0), Vec3(30,-12,7)};
    const Vec3 probes[] = {Vec3(1,0,0), Vec3(0,1,0), Vec3(0,0,1), Vec3(11,-3,25)};

    int mismatches = 0, cases = 0;
    for (const Vec3& a : axes)
        for (f32 ang : angles)
            for (const Vec3& t : trans) {
                // Row-vector composition: rotate, THEN translate, which is what Mat4 means by R*T.
                const Mat4 m = Mat4::fromQuat(Quat::fromAxisAngle(a, ang)) * Mat4::translation(t);
                const JPH::Mat44 jm = toJolt(m);
                for (const Vec3& p : probes) {
                    // Transform in engine space, then convert the RESULT as a position.
                    const Vec3 avr(p.x*m.m[0][0] + p.y*m.m[1][0] + p.z*m.m[2][0] + m.m[3][0],
                                   p.x*m.m[0][1] + p.y*m.m[1][1] + p.z*m.m[2][1] + m.m[3][1],
                                   p.x*m.m[0][2] + p.y*m.m[1][2] + p.z*m.m[2][2] + m.m[3][2]);
                    const JPH::Vec3 want = toJolt(avr);
                    // Convert first, then transform in Jolt space. These agree only if every column
                    // of toJolt(Mat4) is right.
                    const JPH::Vec3 got = jm * toJolt(p);
                    ++cases;
                    if (!vnear(fromJolt(got), fromJolt(want), 2e-3f)) ++mismatches;
                }
            }
    check(mismatches == 0,
          "transform-then-convert == convert-then-transform over " + std::to_string(cases) +
          " cases (" + std::to_string(mismatches) + " mismatched)");

    // Identity has to survive exactly, or every unposed joint is subtly wrong.
    const JPH::Mat44 id = toJolt(Mat4::identity());
    check(id == JPH::Mat44::sIdentity(), "the identity converts to the identity");

    // Translation-only, checked by hand against the axis contract: +X forward in Aver is -Z in Jolt.
    const JPH::Mat44 tm = toJolt(Mat4::translation(Vec3(100, 0, 0)));
    check(near(tm.GetTranslation().GetZ(), -1.0f),
          "100cm of Aver forward becomes 1m of Jolt -Z");
}

// Drops a sphere onto a static floor, rays down at it, and lands a character: gravity, resting
// contact, raycast and the character controller.
static void testSimulation() {
    AVER_INFO("-- simulation --");
    check(aver_phys_init() == 1, "world starts");
    check(aver_phys_ready() == 1, "world reports ready");

    const int32_t floor = aver_phys_add_static_box(0, 0, -10.0f, 500.0f, 500.0f, 10.0f);
    check(floor != 0, "static floor created");
    check(aver_phys_body_count() == 1, "one body in the world");

    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 300.0f, 25.0f, 10.0f);
    check(ball != 0, "dynamic sphere created");

    float p[3] = {0,0,0};
    aver_phys_body_position(ball, p);
    const f32 startZ = p[2];

    int steps = 0;
    for (int i = 0; i < 240; ++i) steps += aver_phys_step(1.0f / 60.0f);
    check(steps > 200, "stepping ran " + std::to_string(steps) + " fixed steps");

    aver_phys_body_position(ball, p);
    check(p[2] < startZ - 100.0f, "sphere fell under gravity (z " + std::to_string(startZ) +
                                  " -> " + std::to_string(p[2]) + ")");
    check(p[2] > 0.0f && p[2] < 60.0f,
          "sphere came to rest ON the floor, not through it (z=" + std::to_string(p[2]) + ")");

    check(near(p[0], 0.0f, 5.0f) && near(p[1], 0.0f, 5.0f),
          "it fell straight down, not sideways (x=" + std::to_string(p[0]) +
          " y=" + std::to_string(p[1]) + ")");

    float hit[3] = {0,0,0}, nrm[3] = {0,0,0};
    int32_t hitEntity = -1;
    const int32_t rayHit = aver_phys_raycast(200.0f, 200.0f, 200.0f, 0, 0, -1, 400.0f, hit, nrm, &hitEntity);
    check(rayHit == floor, "downward ray hit the floor body");
    check(near(hit[2], 0.0f, 2.0f), "hit point is at the floor surface (z=" + std::to_string(hit[2]) + ")");
    check(nrm[2] > 0.9f, "surface normal points UP in engine axes (z=" + std::to_string(nrm[2]) + ")");
    check(hitEntity == 0, "the floor was never stamped, so the hit reports entity 0 (unmapped, not a miss)");

    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, 0, 200.0f, 250.0f);
    check(ch != 0, "character created");
    for (int i = 0; i < 180; ++i) aver_phys_step(1.0f / 60.0f);
    float cp[3] = {0,0,0};
    aver_phys_character_position(ch, cp);
    check(cp[2] > -10.0f && cp[2] < 120.0f,
          "character settled on the floor (z=" + std::to_string(cp[2]) + ")");
    check(aver_phys_character_grounded(ch) == 1, "character reports grounded");

    aver_phys_shutdown();
    check(aver_phys_ready() == 0, "world stops");
}

// Drops a body through a sensor onto a floor: sensor enter/exit, solid contacts, overlap sphere and
// shape cast.
static void testEventsAndQueries() {
    AVER_INFO("-- sensors, events, queries --");
    check(aver_phys_init() == 1, "world starts");

    const int32_t floor = aver_phys_add_static_box(0, 0, -10.0f, 5000.0f, 5000.0f, 10.0f);
    check(floor != 0, "floor created");

    const int32_t gate = aver_phys_add_sensor_box(0, 0, 200.0f, 100.0f, 100.0f, 40.0f);
    check(gate != 0, "sensor created");

    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 400.0f, 20.0f, 5.0f);
    check(ball != 0, "falling body created");

    bool sawEnter = false, sawExit = false;
    int32_t contactsSeen = 0;
    for (int i = 0; i < 240; ++i) {
        aver_phys_step(1.0f / 60.0f);
        for (int32_t k = 0, n = aver_phys_overlap_count(); k < n; ++k) {
            int32_t s = 0, b = 0, entered = 0;
            if (!aver_phys_overlap_get(k, &s, &b, &entered)) continue;
            if (s == gate && b == ball) { if (entered) sawEnter = true; else sawExit = true; }
        }
        contactsSeen += aver_phys_contact_count();
    }
    check(sawEnter, "sensor reported the body ENTERING");
    check(sawExit,  "sensor reported the body LEAVING");
    check(contactsSeen > 0, "solid contact reported when it hit the floor");

    float p[3] = {0,0,0};
    aver_phys_body_position(ball, p);
    check(p[2] > 0.0f && p[2] < 60.0f,
          "body passed THROUGH the sensor and landed (z=" + std::to_string(p[2]) + ")");

    int32_t found[16] = {0};
    const int32_t n = aver_phys_overlap_sphere(p[0], p[1], p[2], 80.0f, found, 16);
    bool foundBall = false;
    for (int32_t i = 0; i < n; ++i) if (found[i] == ball) foundBall = true;
    check(n > 0, "overlap sphere found " + std::to_string(n) + " body(ies)");
    check(foundBall, "overlap sphere found the resting body");

    float hp[3] = {0,0,0}, hn[3] = {0,0,0};
    const int32_t sweptHit = aver_phys_sphere_cast(0, 0, 500.0f, 0, 0, -1.0f, 1000.0f, 30.0f, hp, hn);
    check(sweptHit != 0, "sphere cast hit something on the way down");
    check(hn[2] > 0.5f, "sphere cast normal points UP (z=" + std::to_string(hn[2]) + ")");

    aver_phys_shutdown();
}

// Drops a convex hull onto a triangle mesh and builds a heightfield: the mesh case catches a winding
// that was not corrected for the det -1 axis map.
static void testShapes() {
    AVER_INFO("-- convex hull, mesh, heightfield --");
    check(aver_phys_init() == 1, "world starts");

    const float mv[12] = {
        -2000.0f, -2000.0f, 0.0f,
         2000.0f, -2000.0f, 0.0f,
         2000.0f,  2000.0f, 0.0f,
        -2000.0f,  2000.0f, 0.0f,
    };
    const int32_t mi[6] = {0, 1, 2, 0, 2, 3};
    const int32_t meshFloor = aver_phys_add_mesh(mv, 4, mi, 6, 0, 0, 0);
    check(meshFloor != 0, "triangle-mesh floor created");

    const float hp[24] = {
        -20,-20,-20,  20,-20,-20,  20, 20,-20, -20, 20,-20,
        -20,-20, 20,  20,-20, 20,  20, 20, 20, -20, 20, 20,
    };
    const int32_t hull = aver_phys_add_convex_hull(hp, 8, 0, 0, 400.0f, /*dynamic*/1, 8.0f);
    check(hull != 0, "convex hull created");

    for (int i = 0; i < 300; ++i) aver_phys_step(1.0f / 60.0f);

    float p[3] = {0,0,0};
    aver_phys_body_position(hull, p);
    check(p[2] > 0.0f && p[2] < 80.0f,
          "hull landed ON the triangle mesh, not through it (z=" + std::to_string(p[2]) + ")");

    float hf[64];
    for (int i = 0; i < 64; ++i) hf[i] = 0.0f;
    const int32_t field = aver_phys_add_heightfield(hf, 8, 500.0f, -2000.0f, -2000.0f, 0.0f);
    check(field != 0, "heightfield created");

    aver_phys_shutdown();
}

// Differential test that a 9x9 heightfield keeps its last row: two fields differing only there must
// give different ray hits. Asserts nothing about the axis mapping.
void testHeightfieldKeepsEverySample() {
    AVER_INFO("-- heightfield sample retention --");

    const int32_t N = 9;                  // deliberately NOT a multiple of 8
    const float   spacing = 200.0f;       // cm
    const float   raised = 400.0f;        // cm

    auto sweep = [&]() {
        std::vector<float> zs;
        for (int32_t iy = -(N + 1); iy <= N + 1; ++iy) {
            for (int32_t ix = -(N + 1); ix <= N + 1; ++ix) {
                const float x = static_cast<float>(ix) * spacing;
                const float y = static_cast<float>(iy) * spacing;
                float p[3] = {0,0,0}, nrm[3] = {0,0,0};
                const int32_t hit = aver_phys_raycast(x, y, 5000.0f, 0.0f, 0.0f, -1.0f,
                                                      20000.0f, p, nrm, nullptr);
                zs.push_back(hit ? p[2] : -99999.0f);
            }
        }
        return zs;
    };

    std::vector<float> flatHits, raisedHits;

    std::vector<float> flat(static_cast<size_t>(N) * N, 0.0f);
    check(aver_phys_init() == 1, "physics up for the flat field");
    check(aver_phys_add_heightfield(flat.data(), N, spacing, 0.0f, 0.0f, 0.0f) != 0,
          "9x9 heightfield created (not a multiple of 8)");
    flatHits = sweep();
    aver_phys_shutdown();

    std::vector<float> bumped = flat;
    for (int32_t x = 0; x < N; ++x)
        bumped[static_cast<size_t>(N - 1) * N + x] = raised;

    check(aver_phys_init() == 1, "physics up for the bumped field");
    check(aver_phys_add_heightfield(bumped.data(), N, spacing, 0.0f, 0.0f, 0.0f) != 0,
          "9x9 heightfield with a raised last row created");
    raisedHits = sweep();
    aver_phys_shutdown();

    check(flatHits.size() == raisedHits.size(), "both sweeps cast the same number of rays");

    int differing = 0, raisedSeen = 0;
    for (size_t i = 0; i < flatHits.size() && i < raisedHits.size(); ++i) {
        if (std::fabs(flatHits[i] - raisedHits[i]) > 1.0f) ++differing;
        if (raisedHits[i] > raised * 0.5f) ++raisedSeen;
    }
    check(differing > 0,
          "the last row of a 9x9 heightfield is not discarded (" + std::to_string(differing) +
          " ray(s) differ from the flat field)");
    check(raisedSeen > 0,
          "a ray actually landed on the raised last row (" + std::to_string(raisedSeen) + " hit(s))");
}

// Stamps bodies with aver_phys_set_entity and checks Raycast reports them back correctly: a stamped
// hit, an unmapped hit told apart from a genuine miss, a dead handle refusing to be stamped, and a
// removed body's handle never coming back to resolve to someone else's entity.
static void testEntityAssociation() {
    AVER_INFO("-- entity association, unmapped vs miss, and handle reuse --");
    check(aver_phys_init() == 1, "world starts");

    // ---- a stamped body: raycast reports the entity it was stamped with --------------------------
    const int32_t crate = aver_phys_add_static_box(0, 0, 0, 50, 50, 50);
    check(crate != 0, "crate created");
    check(aver_phys_set_entity(crate, 4242) == 1, "stamping a live body succeeds");

    float p[3] = {0,0,0}, n[3] = {0,0,0};
    int32_t entity = -1;
    int32_t got = aver_phys_raycast(200, 0, 0, -1, 0, 0, 400, p, n, &entity);
    check(got == crate, "ray hit the stamped crate");
    check(entity == 4242, "raycast reports the stamped entity (got " + std::to_string(entity) + ")");

    // ---- unmapped vs miss: a real hit with no owner must not read as "nothing happened" ----------
    const int32_t bare = aver_phys_add_static_box(0, 1000, 0, 50, 50, 50);   // never stamped
    check(bare != 0, "unmapped body created");
    entity = -1;
    got = aver_phys_raycast(200, 1000, 0, -1, 0, 0, 400, p, n, &entity);
    check(got == bare, "ray hit the unmapped body -- a REAL hit, not a miss");
    check(entity == 0, "unmapped body reports entity 0 (\"hit something no entity owns\")");

    entity = -777;   // sentinel: proves outEntity is left untouched, not zeroed, on a genuine miss --
                      // otherwise a caller could not tell "hit, unmapped" from "missed" by the output
                      // alone and would have to trust the return value anyway, defeating the point.
    got = aver_phys_raycast(9000, 9000, 9000, 0, 0, -1, 10.0f, p, n, &entity);
    check(got == 0, "a ray into empty space is a genuine miss");
    check(entity == -777, "outEntity is untouched on a miss, unlike the unmapped case above (entity=" +
                          std::to_string(entity) + ")");

    // ---- a dead handle refuses to be stamped, rather than silently doing nothing -----------------
    check(aver_phys_set_entity(999999, 1) == 0, "stamping a handle that was never issued fails");

    // ---- handle reuse: nextHandle only ever increments (PhysicsWorld.cpp), so a removed body's
    // handle is never reissued, and cannot be coerced into resolving to someone else's entity.
    check(aver_phys_remove_body(crate) == 1, "crate removed");
    check(aver_phys_set_entity(crate, 9999) == 0,
          "the now-dead handle can no longer be stamped -- it does not silently succeed");

    const int32_t second = aver_phys_add_static_box(0, 0, 0, 50, 50, 50);   // the SAME spot as `crate`
    check(second != 0, "a second body created at the same spot");
    check(second != crate, "its engine handle is NEW, not the removed one (old=" +
                           std::to_string(crate) + " new=" + std::to_string(second) + ")");
    check(aver_phys_set_entity(second, 7777) == 1, "the new body can be stamped");

    entity = -1;
    got = aver_phys_raycast(200, 0, 0, -1, 0, 0, 400, p, n, &entity);
    check(got == second, "ray now hits the SECOND body at that spot");
    check(entity == 7777, "and reports the SECOND body's entity, not the removed body's stale 4242 -- "
                          "resolving to none would be fine, resolving to someone else's entity would "
                          "not (entity=" + std::to_string(entity) + ")");

    aver_phys_shutdown();
}

// FIRES A RAY AT A LIVE CHARACTER and reports what actually comes back. aver_phys_character_create
// hands out a DIFFERENT handle family from a body, and Jolt's CharacterVirtual is documented as
// invisible to every broadphase query (NarrowPhaseQuery::CastRay among them) unless given an inner
// body -- this is the proof that the fix in aver_phys_character_create closes that gap, not an
// argument that it should.
static void testCharacterIsRaycastVisible() {
    AVER_INFO("-- a ray fired at a live character --");
    check(aver_phys_init() == 1, "world starts");

    // No floor and no step: the character's position is exactly the one it was created at (Jolt syncs
    // the inner body's transform in the CharacterVirtual constructor itself), so the ray target is
    // known rather than inferred from a settled simulation.
    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, 500.0f, 0.0f, 100.0f);
    check(ch != 0, "character created");
    check(aver_phys_set_entity(ch, 8181) == 1, "character stamped with an entity");

    // Horizontal, through the torso: the capsule is centred at z=100 with an 180cm total height, so
    // it spans roughly z=[10,190], and a ray at z=100 threading straight through x must catch it.
    float p[3] = {0,0,0}, n[3] = {0,0,0};
    int32_t entity = -1;
    int32_t got = aver_phys_raycast(0.0f, 0.0f, 100.0f, 1.0f, 0.0f, 0.0f, 1000.0f, p, n, &entity);
    check(got == ch, "the ray's returned handle IS the character's own handle (got " +
                     std::to_string(got) + ", character is " + std::to_string(ch) + ")");
    check(entity == 8181, "the ray identifies WHAT it hit, not just THAT it hit something (entity=" +
                          std::to_string(entity) + ")");

    // Control: the identical ray shape, aimed well clear of the character, must still miss -- proving
    // the hit above is the character's geometry and not some accident of an always-hit query.
    entity = -55;
    got = aver_phys_raycast(0.0f, 5000.0f, 100.0f, 1.0f, 0.0f, 0.0f, 1000.0f, p, n, &entity);
    check(got == 0, "the same ray, aimed clear of the character, misses (control)");
    check(entity == -55, "outEntity untouched on that miss too");

    aver_phys_shutdown();
}

// Runs every suite and returns the failure count.
int main() {
    testAxisMap();
    testRotationMap();
    testMatrixMap();
    testSimulation();
    testEventsAndQueries();
    testShapes();
    testHeightfieldKeepsEverySample();
    testEntityAssociation();
    testCharacterIsRaycastVisible();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
