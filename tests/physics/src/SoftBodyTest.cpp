// Soft bodies through the plain-C ABI: a cloth that hangs, a body that lands on a floor rather than
// through it, and a skinned body that follows an animation palette.
//
// LINKS NO JOLT, deliberately, unlike PhysicsTest beside it. That test is the ONE place allowed to
// speak both sides (see tests/physics/CMakeLists.txt), because the conversion property it checks is
// invisible from outside. Everything here is visible from outside -- it is what a caller of the ABI
// can see -- so it is checked the way a caller would see it, and this file is the evidence that the
// rule about Jolt staying private to the module still holds.
#include "aver/physics/physics_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }

// An n x n grid of vertices in the engine's XY plane at height z, triangulated. Engine axes: +X
// forward, +Y right, +Z up, so this is a horizontal sheet.
struct Grid {
    std::vector<float> verts;
    std::vector<int32_t> indices;
    int32_t n = 0;
    int32_t count() const { return n * n; }
    int32_t at(int32_t ix, int32_t iy) const { return iy * n + ix; }
};

static Grid makeGrid(int32_t n, float spacingCm, float z) {
    Grid g;
    g.n = n;
    const float half = (n - 1) * spacingCm * 0.5f;
    for (int32_t iy = 0; iy < n; ++iy)
        for (int32_t ix = 0; ix < n; ++ix) {
            g.verts.push_back(ix * spacingCm - half);   // x
            g.verts.push_back(iy * spacingCm - half);   // y
            g.verts.push_back(z);                        // z
        }
    for (int32_t iy = 0; iy + 1 < n; ++iy)
        for (int32_t ix = 0; ix + 1 < n; ++ix) {
            const int32_t a = g.at(ix, iy), b = g.at(ix + 1, iy);
            const int32_t c = g.at(ix + 1, iy + 1), d = g.at(ix, iy + 1);
            g.indices.insert(g.indices.end(), {a, b, c});
            g.indices.insert(g.indices.end(), {a, c, d});
        }
    return g;
}

// A closed, subdivided box shell wound to the ENGINE's convention -- (C-A)x(B-A) is the outward
// normal. Each quad is emitted in an arbitrary order and then corrected against the outward direction
// from the box centre, so this helper states the convention it wants instead of hard-coding six faces
// of hand-checked index triples that a reader has to verify one at a time.
struct Shell {
    std::vector<float> verts;
    std::vector<int32_t> indices;
    int32_t count() const { return static_cast<int32_t>(verts.size() / 3); }
};

static Shell makeBoxShell(float hx, float hy, float hz, int32_t nx, int32_t ny, int32_t nz) {
    Shell s;
    std::vector<std::vector<std::vector<int32_t>>> id(
        nx + 1, std::vector<std::vector<int32_t>>(ny + 1, std::vector<int32_t>(nz + 1, -1)));
    for (int32_t i = 0; i <= nx; ++i)
        for (int32_t j = 0; j <= ny; ++j)
            for (int32_t k = 0; k <= nz; ++k) {
                const bool surface = i == 0 || i == nx || j == 0 || j == ny || k == 0 || k == nz;
                if (!surface) continue;   // interior lattice points are not part of a shell
                id[i][j][k] = s.count();
                s.verts.push_back(-hx + i * (2 * hx / nx));
                s.verts.push_back(-hy + j * (2 * hy / ny));
                s.verts.push_back(-hz + k * (2 * hz / nz));
            }

    // Emits one quad as two triangles, each flipped if it faces inward. `outward` is any vector
    // pointing away from the box centre at that quad; the box is centred on the local origin, so the
    // first vertex's own position serves.
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        const int32_t tri[2][3] = {{a, b, c}, {a, c, d}};
        for (const auto& t : tri) {
            const float* A = &s.verts[static_cast<size_t>(t[0]) * 3];
            const float* B = &s.verts[static_cast<size_t>(t[1]) * 3];
            const float* C = &s.verts[static_cast<size_t>(t[2]) * 3];
            const float u[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
            const float v[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
            const float nrm[3] = {u[1] * v[2] - u[2] * v[1],
                                  u[2] * v[0] - u[0] * v[2],
                                  u[0] * v[1] - u[1] * v[0]};
            const bool out = nrm[0] * A[0] + nrm[1] * A[1] + nrm[2] * A[2] > 0.0f;
            s.indices.push_back(t[0]);
            s.indices.push_back(out ? t[1] : t[2]);
            s.indices.push_back(out ? t[2] : t[1]);
        }
    };
    for (int32_t i = 0; i < nx; ++i)
        for (int32_t j = 0; j < ny; ++j) {
            quad(id[i][j][0],  id[i+1][j][0],  id[i+1][j+1][0],  id[i][j+1][0]);    // -Z
            quad(id[i][j][nz], id[i+1][j][nz], id[i+1][j+1][nz], id[i][j+1][nz]);   // +Z
        }
    for (int32_t i = 0; i < nx; ++i)
        for (int32_t k = 0; k < nz; ++k) {
            quad(id[i][0][k],  id[i+1][0][k],  id[i+1][0][k+1],  id[i][0][k+1]);    // -Y
            quad(id[i][ny][k], id[i+1][ny][k], id[i+1][ny][k+1], id[i][ny][k+1]);   // +Y
        }
    for (int32_t j = 0; j < ny; ++j)
        for (int32_t k = 0; k < nz; ++k) {
            quad(id[0][j][k],  id[0][j+1][k],  id[0][j+1][k+1],  id[0][j][k+1]);    // -X
            quad(id[nx][j][k], id[nx][j+1][k], id[nx][j+1][k+1], id[nx][j][k+1]);   // +X
        }
    return s;
}

static void step(float seconds) {
    const float fixed = aver_phys_fixed_step();
    for (float t = 0.0f; t < seconds; t += fixed) aver_phys_step(fixed);
}

// ---- a cloth hangs from its pinned corners --------------------------------------------------------
static void testClothHangs() {
    AVER_INFO("-- a cloth hangs from what pins it --");
    check(aver_phys_init() == 1, "physics started");

    const Grid g = makeGrid(6, 10.0f, 200.0f);
    // INVERSE mass, so 0 is infinitely heavy and therefore PINNED. The whole top edge is held.
    std::vector<float> invMass(static_cast<size_t>(g.count()), 1.0f);
    for (int32_t ix = 0; ix < g.n; ++ix) invMass[static_cast<size_t>(g.at(ix, 0))] = 0.0f;

    const int32_t body = aver_phys_softbody_create(g.verts.data(), g.count(),
                                                   g.indices.data(), static_cast<int32_t>(g.indices.size()),
                                                   invMass.data(), 0, 0, 0, 0.0f, 0.0f);
    check(body != 0, "the cloth was created");
    check(aver_phys_softbody_vertex_count(body) == g.count(),
          "it kept every one of its " + std::to_string(g.count()) + " particles");

    std::vector<float> before(static_cast<size_t>(g.count()) * 3, 0.0f);
    check(aver_phys_softbody_vertices(body, before.data(), g.count()) == g.count(),
          "its particles read back");

    step(1.0f);

    std::vector<float> after(static_cast<size_t>(g.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, after.data(), g.count());

    // The pinned edge has not moved, and the free edge has fallen. Both halves matter: a cloth that
    // falls entirely has ignored its pins, and one that does not move at all is not simulating.
    float pinnedDrop = 0.0f, freeDrop = 0.0f;
    for (int32_t ix = 0; ix < g.n; ++ix) {
        const int32_t p = g.at(ix, 0), f = g.at(ix, g.n - 1);
        pinnedDrop += before[static_cast<size_t>(p) * 3 + 2] - after[static_cast<size_t>(p) * 3 + 2];
        freeDrop   += before[static_cast<size_t>(f) * 3 + 2] - after[static_cast<size_t>(f) * 3 + 2];
    }
    pinnedDrop /= static_cast<float>(g.n);
    freeDrop   /= static_cast<float>(g.n);
    check(near(pinnedDrop, 0.0f, 0.5f), "the pinned edge stayed put (" + std::to_string(pinnedDrop) + " cm)");
    check(freeDrop > 10.0f, "the free edge fell (" + std::to_string(freeDrop) + " cm)");

    aver_phys_shutdown();
}

// ---- it collides with the world -------------------------------------------------------------------
static void testSoftBodyLandsOnFloor() {
    AVER_INFO("-- it lands on the floor rather than through it --");
    check(aver_phys_init() == 1, "physics started");

    // THE WHOLE REASON THIS IS JOLT AND NOT A CAGE SOLVER OF OUR OWN. A hand-written solver gets
    // no world collision for free; this check is what that decision bought, so it is the one that
    // matters most in this file.
    const int32_t floor = aver_phys_add_static_box(0, 0, 0, 500, 500, 10);
    check(floor != 0, "a static floor exists, its top at z = 10");

    const Grid g = makeGrid(5, 12.0f, 150.0f);
    const int32_t body = aver_phys_softbody_create(g.verts.data(), g.count(),
                                                   g.indices.data(), static_cast<int32_t>(g.indices.size()),
                                                   nullptr, 0, 0, 0, 0.0f, 0.0f);
    check(body != 0, "an unpinned cloth was created above it");

    step(3.0f);

    std::vector<float> pos(static_cast<size_t>(g.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, pos.data(), g.count());
    float lowest = 1e9f, highest = -1e9f;
    for (int32_t i = 0; i < g.count(); ++i) {
        const float z = pos[static_cast<size_t>(i) * 3 + 2];
        lowest = std::min(lowest, z);
        highest = std::max(highest, z);
    }
    check(lowest > -5.0f, "nothing fell through the floor (lowest z = " + std::to_string(lowest) + ")");
    check(highest < 140.0f, "and it really did fall (highest z = " + std::to_string(highest) + ")");

    aver_phys_shutdown();
}

// ---- skinning -------------------------------------------------------------------------------------

// A row-major engine Mat4 as 16 floats: identity, then a translation in the last row.
static void writeTranslation(float* m, float x, float y, float z) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = x; m[13] = y; m[14] = z;
}

static void testSkinnedFollowsItsJoints() {
    AVER_INFO("-- a skinned soft body follows its animation palette --");
    check(aver_phys_init() == 1, "physics started");

    const Grid g = makeGrid(5, 10.0f, 100.0f);
    const int32_t verts = g.count();
    // One joint, every vertex fully weighted to it: the simplest case that still exercises the
    // whole path -- palette conversion, the identity inverse binds, and SkinVertices itself.
    std::vector<int32_t> joints(static_cast<size_t>(verts), 0);
    std::vector<float>   weights(static_cast<size_t>(verts), 1.0f);

    // MAX DISTANCE ZERO means "you may not leave your skinned position", so the soft body must
    // reproduce ORDINARY SKINNING exactly. That is the strongest statement available here: if the
    // matrix conversion or the inverse-bind trick were wrong, this is where it shows, because the
    // answer is one the test can compute for itself.
    const int32_t body = aver_phys_softbody_create_skinned(
        g.verts.data(), verts, g.indices.data(), static_cast<int32_t>(g.indices.size()),
        nullptr, joints.data(), weights.data(), 1, 1,
        /*maxDistanceCm*/ 0.0f, /*backStopDistanceCm*/ -1.0f, 0, 0, 0, 0.0f);
    check(body != 0, "the skinned body was created");
    check(aver_phys_softbody_vertex_count(body) == verts, "with every particle");

    float palette[16];
    writeTranslation(palette, 0, 0, 0);
    check(aver_phys_softbody_skin(body, palette, 1, /*hardSkin*/ 1) == 1,
          "an identity palette skins it");

    std::vector<float> pos(static_cast<size_t>(verts) * 3, 0.0f);
    aver_phys_softbody_vertices(body, pos.data(), verts);
    float worst = 0.0f;
    for (int32_t i = 0; i < verts; ++i)
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(pos[static_cast<size_t>(i) * 3 + c] -
                                             g.verts[static_cast<size_t>(i) * 3 + c]));
    check(worst < 0.5f,
          "an identity palette leaves every particle on its bind pose (worst " +
          std::to_string(worst) + " cm off)");

    // Now MOVE the joint. Every vertex is weighted entirely to it, so every vertex must move by
    // exactly the same offset -- in ENGINE axes, which is what catches a basis error that an
    // identity palette cannot.
    writeTranslation(palette, 40.0f, -25.0f, 15.0f);
    check(aver_phys_softbody_skin(body, palette, 1, 1) == 1, "a translated palette skins it");
    aver_phys_softbody_vertices(body, pos.data(), verts);

    worst = 0.0f;
    for (int32_t i = 0; i < verts; ++i) {
        const float dx = pos[static_cast<size_t>(i) * 3 + 0] - g.verts[static_cast<size_t>(i) * 3 + 0];
        const float dy = pos[static_cast<size_t>(i) * 3 + 1] - g.verts[static_cast<size_t>(i) * 3 + 1];
        const float dz = pos[static_cast<size_t>(i) * 3 + 2] - g.verts[static_cast<size_t>(i) * 3 + 2];
        worst = std::max(worst, std::abs(dx - 40.0f));
        worst = std::max(worst, std::abs(dy + 25.0f));
        worst = std::max(worst, std::abs(dz - 15.0f));
    }
    check(worst < 0.5f,
          "every particle moved by the joint's own offset, on the engine's axes (worst " +
          std::to_string(worst) + " cm off)");

    // And it stays there under gravity, because maxDistance is 0. A soft body that sags here has
    // lost its skinned constraints somewhere between creation and the step.
    step(0.5f);
    aver_phys_softbody_skin(body, palette, 1, 0);
    aver_phys_softbody_vertices(body, pos.data(), verts);
    float sag = 0.0f;
    for (int32_t i = 0; i < verts; ++i)
        sag = std::max(sag, (g.verts[static_cast<size_t>(i) * 3 + 2] + 15.0f) -
                            pos[static_cast<size_t>(i) * 3 + 2]);
    check(sag < 2.0f, "at maxDistance 0 it does not sag under gravity (" + std::to_string(sag) + " cm)");

    aver_phys_shutdown();
}

static void testSkinnedCanLeaveItsSkin() {
    AVER_INFO("-- and at a larger max distance it is allowed to sag --");
    check(aver_phys_init() == 1, "physics started");

    const Grid g = makeGrid(5, 10.0f, 100.0f);
    const int32_t verts = g.count();
    std::vector<int32_t> joints(static_cast<size_t>(verts), 0);
    std::vector<float>   weights(static_cast<size_t>(verts), 1.0f);

    // THE DIAL THE WHOLE FEATURE TURNS ON. Same body, same palette, one number changed -- and it
    // goes from "this is just skinning" to "this drapes". Checking both ends is what proves the
    // parameter reaches the solver rather than being accepted and dropped.
    const int32_t body = aver_phys_softbody_create_skinned(
        g.verts.data(), verts, g.indices.data(), static_cast<int32_t>(g.indices.size()),
        nullptr, joints.data(), weights.data(), 1, 1,
        /*maxDistanceCm*/ 30.0f, -1.0f, 0, 0, 0, 0.0f);
    check(body != 0, "a body with 30 cm of slack was created");

    float palette[16];
    writeTranslation(palette, 0, 0, 0);
    aver_phys_softbody_skin(body, palette, 1, 1);

    for (int i = 0; i < 60; ++i) { aver_phys_softbody_skin(body, palette, 1, 0); aver_phys_step(aver_phys_fixed_step()); }

    std::vector<float> pos(static_cast<size_t>(verts) * 3, 0.0f);
    aver_phys_softbody_vertices(body, pos.data(), verts);
    float sag = 0.0f;
    for (int32_t i = 0; i < verts; ++i)
        sag = std::max(sag, g.verts[static_cast<size_t>(i) * 3 + 2] - pos[static_cast<size_t>(i) * 3 + 2]);
    check(sag > 1.0f, "it sagged (" + std::to_string(sag) + " cm)");
    check(sag < 35.0f, "but no further than its slack allowed (" + std::to_string(sag) + " cm)");

    aver_phys_shutdown();
}

// ---- the handle contract ---------------------------------------------------------------------------
static void testHandleContract() {
    AVER_INFO("-- a soft body is a body, and bad input is refused --");
    check(aver_phys_init() == 1, "physics started");

    const Grid g = makeGrid(4, 10.0f, 50.0f);
    const int32_t soft = aver_phys_softbody_create(g.verts.data(), g.count(),
                                                   g.indices.data(), static_cast<int32_t>(g.indices.size()),
                                                   nullptr, 0, 0, 0, 0.0f, 0.0f);
    const int32_t rigid = aver_phys_add_dynamic_sphere(0, 0, 300, 10, 1);
    check(soft != 0 && rigid != 0, "a soft body and a rigid body both exist");
    check(soft != rigid, "drawn from one handle counter, so they never collide");

    // Every body-level call has to work on it unchanged -- that is what "a soft body is a body"
    // buys, and it is only true if the handle went into the same table.
    check(aver_phys_set_entity(soft, 4242) == 1, "aver_phys_set_entity accepts it");
    float xyz[3];
    check(aver_phys_body_position(soft, xyz) == 1, "aver_phys_body_position reads it");

    check(aver_phys_softbody_vertex_count(rigid) == 0, "vertex_count on a rigid body is 0");
    check(aver_phys_softbody_vertices(rigid, xyz, 1) == 0, "so is reading its vertices");
    check(aver_phys_softbody_vertex_count(999999) == 0, "and on a dead handle");

    float palette[16];
    writeTranslation(palette, 0, 0, 0);
    check(aver_phys_softbody_skin(soft, palette, 1, 1) == 0,
          "skinning an UNSKINNED soft body is refused rather than asserting inside Jolt");
    check(aver_phys_softbody_skin(rigid, palette, 1, 1) == 0, "and skinning a rigid body is refused");

    // aver_phys_softbody_apply_impulse -- the same bad-input discipline as every call above it, plus
    // the two parameters unique to this one: a non-positive radius (an empty or inverted query sphere
    // has nothing to nudge) and the null-pointer pair a caller could hand in by mistake.
    const float zero3[3] = {0.0f, 0.0f, 0.0f};
    check(aver_phys_softbody_apply_impulse(rigid, zero3, 100.0f, zero3, 1.0f) == 0,
          "apply_impulse on a rigid body is refused, same as skin/vertices above");
    check(aver_phys_softbody_apply_impulse(999999, zero3, 100.0f, zero3, 1.0f) == 0,
          "and on a dead handle");
    check(aver_phys_softbody_apply_impulse(soft, zero3, 0.0f, zero3, 1.0f) == 0,
          "a zero radius catches nothing rather than being treated as unbounded");
    check(aver_phys_softbody_apply_impulse(soft, zero3, -50.0f, zero3, 1.0f) == 0,
          "neither does a negative one");
    check(aver_phys_softbody_apply_impulse(soft, nullptr, 100.0f, zero3, 1.0f) == 0,
          "a null centre pointer is refused rather than dereferenced");
    check(aver_phys_softbody_apply_impulse(soft, zero3, 100.0f, nullptr, 1.0f) == 0,
          "so is a null velocity pointer");
    // The grid's own vertices sit at z = 50 (see makeGrid's call above), all within 100 cm of the
    // origin horizontally for at least the centre one -- a real, positive count, not just "not zero"
    // by coincidence of the failure checks above returning zero too.
    check(aver_phys_softbody_apply_impulse(soft, zero3, 1000.0f, zero3, 1.0f) > 0,
          "a valid call against a real soft body actually finds and nudges vertices");

    check(aver_phys_softbody_create(nullptr, 4, g.indices.data(), 6, nullptr, 0,0,0, 0,0) == 0,
          "null vertices are refused");
    check(aver_phys_softbody_create(g.verts.data(), g.count(), g.indices.data(), 0, nullptr, 0,0,0, 0,0) == 0,
          "a mesh with no triangles is refused");

    // Degenerate triangles would trip an assert inside Jolt's AddFace; they must be dropped, and a
    // mesh that is ENTIRELY degenerate must then be refused rather than creating an empty body.
    const int32_t degenerate[] = {0, 0, 0, 1, 1, 1};
    check(aver_phys_softbody_create(g.verts.data(), g.count(), degenerate, 6, nullptr, 0,0,0, 0,0) == 0,
          "a mesh of only degenerate triangles is refused");

    check(aver_phys_remove_body(soft) == 1, "and it is removed by the ordinary body call");
    check(aver_phys_softbody_vertex_count(soft) == 0, "after which its handle is dead");

    aver_phys_shutdown();
}

// ---- pressure actually reaches the solver ---------------------------------------------------------
//
// THE REGRESSION THIS EXISTS FOR. Jolt's ApplyPressure opens with `if (six_volume > 0.0f)`, so a shell
// whose faces arrive wound inward gets no pressure AT ALL -- silently, with no warning and no error,
// and looking exactly like a pressure coefficient that is merely too small. That is what shipped:
// the conversion into Jolt's axes already mirrors the mesh, and an index swap on top of it put every
// face back inside out. It went unnoticed because the only caller at the time passed pressure 0.
//
// Checked as a DIFFERENCE between two identical shells, one pressurised and one not, so it cannot
// pass by accident: whatever the absolute numbers, the pressurised one must hold up and the other
// must not.
static void testPressureHoldsAShellUp() {
    AVER_INFO("-- a pressurised shell holds its shape, an unpressurised one does not --");
    check(aver_phys_init() == 1, "physics started");

    check(aver_phys_add_static_box(0, 0, -10, 500, 500, 10) != 0, "a static floor exists, top at z = 0");

    // THE PROPORTIONS THAT ACTUALLY FAILED: the FirstPerson template's pool, 6 m x 4 m x 1.2 m at
    // 8 x 8 x 4. A small stiff cube survives three seconds unpressurised and proves nothing.
    const float hx = 300.0f, hy = 200.0f, hz = 60.0f;
    const int32_t nx = 8, ny = 8, nz = 4;
    const Shell s = makeBoxShell(hx, hy, hz, nx, ny, nz);
    check(s.count() > 0 && !s.indices.empty(), "a closed box shell was built");

    // 0.6 * 2 g hz (nx+1)(ny+1) in Jolt's metres -- fluids::fluidPressureFor's own formula, restated
    // here because this test links the ABI and nothing else (see this file's opening comment). Kept
    // in step with that function by the two bounds below, which fail if either drifts.
    const float pressure = 0.6f * 1.0e-4f * 2.0f * 980.0f * hz * (nx + 1) * (ny + 1);

    auto heightAfter = [&](float p) {
        const int32_t body = aver_phys_softbody_create(
            s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
            nullptr, 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, p);
        if (body == 0) return -1.0f;
        step(4.0f);
        std::vector<float> pos(static_cast<size_t>(s.count()) * 3, 0.0f);
        aver_phys_softbody_vertices(body, pos.data(), s.count());
        float lo = 1e9f, hi = -1e9f;
        for (int32_t i = 0; i < s.count(); ++i) {
            const float z = pos[static_cast<size_t>(i) * 3 + 2];
            lo = std::min(lo, z);
            hi = std::max(hi, z);
        }
        aver_phys_remove_body(body);
        return hi - lo;
    };

    const float limp = heightAfter(0.0f);
    const float firm = heightAfter(pressure);
    check(limp >= 0.0f && firm >= 0.0f, "both shells were created");
    check(limp < 0.92f * 2 * hz, "without pressure it sags under its own weight (height " +
                                 std::to_string(limp) + " of " + std::to_string(2 * hz) + ")");
    check(firm > limp + 0.1f * hz, "with pressure it holds up better (height " + std::to_string(firm) +
                                   " against " + std::to_string(limp) + ")");
    // BOTH ENDS, because both are real failures that shipped. Too little pressure -- or, as happened,
    // pressure the solver silently discards because the faces arrived inside out -- and the shell
    // puddles. Too much and it balloons out of its basin; the first version of this coefficient was
    // 10,000x too large and swallowed the camera.
    check(firm > 0.90f * 2 * hz, "keeping nearly all its depth (" + std::to_string(firm) + ")");
    check(firm < 1.15f * 2 * hz, "and not gaining any (" + std::to_string(firm) + ")");

    aver_phys_shutdown();
}

// ---- does the pool react when the player walks through it? ----------------------------------------
//
// THE QUESTION THIS FILE ANSWERS EMPIRICALLY, in two tests. Short version: NO -- not through the
// passive path, not today. Long version below, and in the two tests' own comments.
//
// aver_phys_character_create does not hand Jolt a rigid body this module drives directly -- it builds a
// JPH::CharacterVirtual, which keeps its OWN position (advanced by ExtendedUpdate, called once per fixed
// step from PhysicsWorld's loop, BEFORE PhysicsSystem::Update runs) and TELEPORTS a companion kinematic
// JPH::Body to match every step via UpdateInnerBodyTransform -> BodyInterface::SetPositionAndRotation.
// That inner body is what a soft body's own solver would have to see for the pool to react.
//
// Two SEPARATE Jolt mechanisms are in play here, and the measurements below show they are NOT
// symmetric:
//
//   (1) THE CHARACTER'S OWN COLLISION AGAINST THE POOL. CharacterVirtual::MoveShape (called from
//       ExtendedUpdate) does an ordinary narrow-phase shape query, and SoftBodyShape.cpp's sRegister()
//       DOES wire up a general Convex-vs-SoftBody dispatch (sCollideConvexVsSoftBody /
//       sCastConvexVsSoftBody) built directly from the pool's LIVE, currently-deformed vertex
//       positions -- not a stale snapshot. This path works: testCharacterEmbeddedInPoolDoesNotMoveIt
//       below drops a character dead centre in the pool and watches Jolt eject it by tens of
//       centimetres, exactly as if the pool were solid ground.
//
//   (2) THE POOL'S OWN VERTICES REACTING TO THE CHARACTER. This is the ENTIRELY SEPARATE path a soft
//       body uses for ITS OWN update: SoftBodyMotionProperties::DetermineCollidingShapes broadphase-
//       queries for nearby rigid bodies and calls THEIR shape's CollideSoftBodyVertices to push the
//       pool's own particles out. Both tests below measure this side and find NOTHING: a pool vertex
//       sitting deep inside the character's capsule for two full seconds ends up displaced by LESS than
//       the pool's own natural settling jiggle over the same span with no character present at all --
//       and a character actually driven across the pool with aver_phys_character_set_velocity (the
//       exact call AverCharacter.Drive makes every tick) produces a result INDISTINGUISHABLE from a
//       character that never moved.
//
// WHAT THIS RULES OUT, with a measurement rather than a guess for each:
//   - wrong object layer: ruled out by reading -- Layers::MOVING vs MOVING is unconditionally permitted
//     by both ObjectLayerPairFilter and ObjectVsBroadPhaseFilter in PhysicsWorld.cpp, on both sides.
//   - a capsule too small or too shallow to reach the pool: ruled out by MEASUREMENT --
//     testCharacterEmbeddedInPoolDoesNotMoveIt places a 40 cm-radius capsule dead centre in the settled
//     pool and confirms (as a precondition, not an assumption) that a real pool vertex starts well
//     inside it.
//   - a kinematic inner body never getting a velocity Jolt can read: this explains why the DYNAMIC-only
//     branch at SoftBodyMotionProperties.cpp:216 never fires, but NOT the missing reaction, because the
//     positional correction that follows it (`v.mPosition += contact_normal * projected_distance`,
//     same file ~line 733) fires unconditionally for ANY colliding shape, no velocity required -- and
//     the embedded-character test still finds nothing, which this alone cannot explain.
//   - a contact listener silently rejecting the contact: there isn't one. PhysicsWorld.cpp only ever
//     installs a JPH::ContactListener (for ordinary rigid contacts); it never calls
//     PhysicsSystem::SetSoftBodyContactListener, so SoftBodyUpdateContext::mContactListener is null and
//     DetermineCollidingShapes' `if (mContext.mContactListener == nullptr)` branch -- the fully
//     permissive one -- is the one that always runs.
//
// What is left, and is NOT ruled out by anything measurable through this ABI: DetermineCollidingShapes'
// own broadphase query (SoftBodyMotionProperties.cpp:253, `GetBroadPhaseQuery().CollideAABox(...)`)
// appears to never return the character's inner BodyID as a hit, for a reason this test cannot see from
// outside Jolt -- everything the ABI can inspect (layers, filters, listeners, geometry, sleep state:
// CharacterVirtual explicitly sets `mAllowSleeping = false` on the inner body) comes back clean. That is
// the honest boundary of what a black-box ABI test can diagnose; the next step would be instrumenting
// Jolt itself, which is out of scope here.

// The maximal, timing-free version of the question: drop a character CENTRED on a settled pool, deep
// enough that a real vertex starts inside its capsule, and hold it there -- no sweep, no "did it arrive
// in time", nothing left to explain away.
static void testCharacterEmbeddedInPoolDoesNotMoveIt() {
    AVER_INFO("-- a character embedded dead centre in the pool: does the pool feel it? --");
    check(aver_phys_init() == 1, "physics started");
    check(aver_phys_add_static_box(0, 0, -10, 800, 800, 10) != 0, "a static floor exists, top at z = 0");

    // Same pool proportions as testPressureHoldsAShellUp, for the same reason given there.
    const float hx = 300.0f, hy = 200.0f, hz = 60.0f;
    const int32_t nx = 8, ny = 8, nz = 4;
    const Shell s = makeBoxShell(hx, hy, hz, nx, ny, nz);
    const float pressure = 0.6f * 1.0e-4f * 2.0f * 980.0f * hz * (nx + 1) * (ny + 1);
    const int32_t body = aver_phys_softbody_create(
        s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
        nullptr, 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, pressure);
    check(body != 0, "the pool was created");
    step(3.0f);   // settle before anything else exists.

    std::vector<float> before(static_cast<size_t>(s.count()) * 3, 0.0f);
    check(aver_phys_softbody_vertices(body, before.data(), s.count()) == s.count(), "settled positions read back");

    // Whichever settled vertex sits closest to the pool's own centre -- found, not assumed, so this
    // test cannot go quietly vacuous if the shell's own tessellation ever changes.
    const float cx = 0.0f, cy = 0.0f, cz = hz + 5.0f;
    int32_t nearestIdx = -1; float nearestDist = 1e9f;
    for (int32_t i = 0; i < s.count(); ++i) {
        const float dx = before[static_cast<size_t>(i) * 3 + 0] - cx;
        const float dy = before[static_cast<size_t>(i) * 3 + 1] - cy;
        const float dz = before[static_cast<size_t>(i) * 3 + 2] - cz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < nearestDist) { nearestDist = d; nearestIdx = i; }
    }

    // 40 cm radius, 100 cm tall, centred on the pool -- big enough relative to the pool's own 300 cm
    // half-width that missing every vertex would mean the shell itself is broken, not this test.
    const int32_t ch = aver_phys_character_create(40.0f, 100.0f, cx, cy, cz);
    check(ch != 0, "the character was created, embedded, at the pool's centre");
    check(nearestDist < 40.0f,
          "and confirmed embedded for real -- the nearest pool vertex started " +
          std::to_string(nearestDist) + " cm from the capsule's centre, inside its 40 cm radius");

    step(2.0f);   // held there, motionless, for two full seconds.

    std::vector<float> after(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, after.data(), s.count());
    const float vBx = before[static_cast<size_t>(nearestIdx) * 3 + 0];
    const float vBy = before[static_cast<size_t>(nearestIdx) * 3 + 1];
    const float vBz = before[static_cast<size_t>(nearestIdx) * 3 + 2];
    const float vAx = after[static_cast<size_t>(nearestIdx) * 3 + 0];
    const float vAy = after[static_cast<size_t>(nearestIdx) * 3 + 1];
    const float vAz = after[static_cast<size_t>(nearestIdx) * 3 + 2];
    const float vertexMoved = std::sqrt((vAx - vBx) * (vAx - vBx) + (vAy - vBy) * (vAy - vBy) +
                                        (vAz - vBz) * (vAz - vBz));

    float chAfter[3] = {0, 0, 0};
    aver_phys_character_position(ch, chAfter);
    const float characterDrift = std::sqrt((chAfter[0] - cx) * (chAfter[0] - cx) +
                                           (chAfter[1] - cy) * (chAfter[1] - cy) +
                                           (chAfter[2] - cz) * (chAfter[2] - cz));
    aver_phys_shutdown();

    // PAIRED CONTROL: identical pool, identical settle, identical hold -- no character. What the SAME
    // vertex does entirely on its own, so "did the character move it" has a real number to beat.
    check(aver_phys_init() == 1, "physics started (control)");
    check(aver_phys_add_static_box(0, 0, -10, 800, 800, 10) != 0, "floor (control)");
    const int32_t bodyCtrl = aver_phys_softbody_create(
        s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
        nullptr, 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, pressure);
    step(3.0f);
    std::vector<float> beforeCtrl(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(bodyCtrl, beforeCtrl.data(), s.count());
    step(2.0f);
    std::vector<float> afterCtrl(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(bodyCtrl, afterCtrl.data(), s.count());
    const float cBx = beforeCtrl[static_cast<size_t>(nearestIdx) * 3 + 0];
    const float cBy = beforeCtrl[static_cast<size_t>(nearestIdx) * 3 + 1];
    const float cBz = beforeCtrl[static_cast<size_t>(nearestIdx) * 3 + 2];
    const float cAx = afterCtrl[static_cast<size_t>(nearestIdx) * 3 + 0];
    const float cAy = afterCtrl[static_cast<size_t>(nearestIdx) * 3 + 1];
    const float cAz = afterCtrl[static_cast<size_t>(nearestIdx) * 3 + 2];
    const float controlVertexMoved = std::sqrt((cAx - cBx) * (cAx - cBx) + (cAy - cBy) * (cAy - cBy) +
                                               (cAz - cBz) * (cAz - cBz));
    aver_phys_shutdown();

    AVER_INFO("  embedded pool vertex moved {} cm in 2s; the SAME vertex with no character moved {} cm on its own",
              vertexMoved, controlVertexMoved);
    AVER_INFO("  the character itself drifted {} cm from where it was dropped (proves real geometric overlap)",
              characterDrift);

    // ASYMMETRY, MEASURED. The character reacts to the pool -- Jolt's general shape-cast collision
    // (mechanism (1) in this section's opening comment) ejects it from the overlap by many times its
    // own capsule radius. But the pool's own vertex, sitting the whole time inside that same capsule,
    // moves NO MORE than it would have moved on its own with no character there at all: this is
    // TODAY'S REAL BEHAVIOUR, not a guess, and this assertion is what would fail the day it changes.
    check(characterDrift > 10.0f,
          "the character was visibly ejected by the overlap, so the two shapes genuinely touched (" +
          std::to_string(characterDrift) + " cm)");
    check(vertexMoved < controlVertexMoved + 5.0f,
          "AS OF TODAY: the pool vertex embedded inside the character moved no more than the pool's own "
          "natural jiggle explains (" + std::to_string(vertexMoved) + " cm vs " +
          std::to_string(controlVertexMoved) + " cm unperturbed) -- the soft body's own collision solver "
          "is not reacting to the character at all. If this ever fails, the passive path started "
          "working: flip this assertion to require vertexMoved to clear the capsule's own radius, the "
          "way testPressureHoldsAShellUp pins its own two ends.");
}

// The realistic version: the actual gameplay call (aver_phys_character_set_velocity, exactly what
// AverCharacter.Drive calls every tick) driving a character across the pool, control-vs-treatment
// against a character that exists but never moves -- same discipline as testPressureHoldsAShellUp.
static void runSweepPass(bool moveThrough, float* outMaxDispCm, float* outMeanDispCm, float* outFinalX) {
    check(aver_phys_init() == 1, "physics started");
    check(aver_phys_add_static_box(0, 0, -10, 800, 800, 10) != 0, "a static floor exists, top at z = 0");

    const float hx = 300.0f, hy = 200.0f, hz = 60.0f;
    const int32_t nx = 8, ny = 8, nz = 4;
    const Shell s = makeBoxShell(hx, hy, hz, nx, ny, nz);
    const float pressure = 0.6f * 1.0e-4f * 2.0f * 980.0f * hz * (nx + 1) * (ny + 1);
    const int32_t body = aver_phys_softbody_create(
        s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
        nullptr, 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, pressure);
    check(body != 0, "the pool was created");

    step(3.0f);   // settle under gravity + pressure BEFORE the character exists, so everything the
                  // after-minus-before diff below finds can only be the character's own wake, not
                  // leftover settling motion the pool would have had anyway.

    std::vector<float> before(static_cast<size_t>(s.count()) * 3, 0.0f);
    check(aver_phys_softbody_vertices(body, before.data(), s.count()) == s.count(),
          "settled positions read back");

    // Standing height: capsule centre 90 cm up puts a 180 cm-tall, 25 cm-radius capsule's feet exactly
    // on the floor and its head 90 cm above that -- comfortably taller than the pool is deep at any
    // point along its settle, so "did the capsule reach deep enough" is not a variable this run has to
    // account for (and testCharacterEmbeddedInPoolDoesNotMoveIt above measures that question directly
    // anyway). Started 150 cm outside the pool's -X wall, floor sized generously so a full-speed
    // crossing never runs the character off the edge.
    const int32_t ch = aver_phys_character_create(25.0f, 180.0f, -(hx + 150.0f), 0.0f, 90.0f);
    check(ch != 0, "the character was created");
    if (moveThrough)
        check(aver_phys_character_set_velocity(ch, 250.0f, 0.0f, 0.0f) == 1,
              "given a walking velocity aimed at the pool -- aver_phys_character_set_velocity, exactly "
              "what AverCharacter.Drive calls every tick");
    // else: left at its default (0,0,0) -- THE CONTROL. Same character, same broadphase-visible inner
    // body sitting in the same spot, just never told to move.

    step(4.0f);   // at 250 cm/s (a jog) this crosses the pool's 600 cm width with 150 cm of clearance to
                  // enter and leave on, so by the end it is a clean pass all the way through and out.

    float chPos[3] = {0, 0, 0};
    aver_phys_character_position(ch, chPos);
    if (outFinalX) *outFinalX = chPos[0];

    std::vector<float> after(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, after.data(), s.count());

    float maxDisp = 0.0f, sumDisp = 0.0f;
    for (int32_t i = 0; i < s.count(); ++i) {
        const float dx = after[static_cast<size_t>(i) * 3 + 0] - before[static_cast<size_t>(i) * 3 + 0];
        const float dy = after[static_cast<size_t>(i) * 3 + 1] - before[static_cast<size_t>(i) * 3 + 1];
        const float dz = after[static_cast<size_t>(i) * 3 + 2] - before[static_cast<size_t>(i) * 3 + 2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        maxDisp = std::max(maxDisp, d);
        sumDisp += d;
    }
    *outMaxDispCm = maxDisp;
    *outMeanDispCm = sumDisp / static_cast<float>(s.count());

    aver_phys_shutdown();
}

static void testCharacterSweepDoesNotVisiblyMoveTheSoftBody() {
    AVER_INFO("-- the player's own capsule, driven the way gameplay drives it: does the pool feel it? --");

    float ctrlMax = 0.0f, ctrlMean = 0.0f, sweepMax = 0.0f, sweepMean = 0.0f;
    float ctrlFinalX = 0.0f, sweepFinalX = 0.0f;
    runSweepPass(false, &ctrlMax, &ctrlMean, &ctrlFinalX);
    runSweepPass(true,  &sweepMax, &sweepMean, &sweepFinalX);
    AVER_INFO("  control (character present, stationary): max {} cm, mean {} cm, final x {} cm",
              ctrlMax, ctrlMean, ctrlFinalX);
    AVER_INFO("  swept through (aver_phys_character_set_velocity): max {} cm, mean {} cm, final x {} cm",
              sweepMax, sweepMean, sweepFinalX);

    // First, prove the test itself is not vacuous: the swept-through character really did walk across,
    // the control character really did stay put. Both use the identical real ABI call the game uses.
    check(ctrlMax >= 0.0f && sweepMax >= 0.0f, "both passes produced a reading");
    check(std::abs(ctrlFinalX - (-(300.0f + 150.0f))) < 5.0f,
          "the control character stayed exactly where it started (" + std::to_string(ctrlFinalX) + " cm)");
    check(sweepFinalX > 300.0f,
          "the swept-through character genuinely crossed the pool and came out the far side (" +
          std::to_string(sweepFinalX) + " cm)");

    // THE ACTUAL FINDING, AS A REGRESSION GUARD. Given the asymmetry measured directly in
    // testCharacterEmbeddedInPoolDoesNotMoveIt above, a real walking pass produces NO reaction
    // distinguishable from the stationary control -- this is TODAY'S measured behaviour, asserted so a
    // change in either direction gets caught: a further regression (the character-side collision itself
    // breaking) is not what this checks, but a pool that starts reacting IS what flips this assertion,
    // which is the signal that this test (and FluidVolume's docs) need updating.
    check(sweepMax < ctrlMax * 3.0f + 10.0f,
          "AS OF TODAY: a character walked all the way through the pool moves it no more than a "
          "motionless one sitting beside it (" + std::to_string(sweepMax) + " cm vs " +
          std::to_string(ctrlMax) + " cm) -- the passive gameplay path does not make this pool react.");

    // Stated plainly against the pool's own scale (hz = 60 cm, FluidVolume.hpp's half-depth, the
    // dimension a camera watching this pool would actually see move): today's numbers are a rounding
    // error against it, not a visible ripple.
    const float hz = 60.0f;
    check(sweepMax < 0.5f * hz,
          "and in absolute terms it stays well under the pool's own 60 cm half-depth (" +
          std::to_string(sweepMax) + " cm) -- not the visible reaction a player walking through a pool "
          "should produce");

    // No aver_phys_shutdown() here -- each of the two passes above already started and stopped its own
    // world (runSweepPass is symmetric: one aver_phys_init, one aver_phys_shutdown), so nothing is left
    // running for this function to close.
}

// ---- the fallback: aver_phys_softbody_apply_impulse actually moves the pool -----------------------
//
// THE OTHER HALF OF THE STORY ABOVE. The two tests just above measured that the PASSIVE path -- the
// pool discovering the character on its own, through Jolt's ordinary soft-body collision update --
// produces no reaction. This is the ACTIVE fallback SandboxApp.cpp's onUpdate now calls instead,
// right where fluidScene_.update() runs: a direct, composition-root nudge to the vertices' own
// velocity, the one thing SoftBodyVertex.h documents as the sanctioned external lever ("you should
// only modify the inverse mass and/or velocity of a vertex to control the soft body... Modifying the
// position can lead to missed collisions"). Same discipline as every test above: control vs
// treatment, real numbers, not an assertion that "something changed".
static void testApplyImpulseMovesNearbyVertices() {
    AVER_INFO("-- aver_phys_softbody_apply_impulse: the active fallback, does it actually move the pool? --");
    check(aver_phys_init() == 1, "physics started");
    check(aver_phys_add_static_box(0, 0, -10, 800, 800, 10) != 0, "a static floor exists, top at z = 0");

    // Same pool proportions as every other pool test in this file, for the same reason given in
    // testPressureHoldsAShellUp: real numbers on a shape this file has already characterised, not a
    // fresh shell whose settling behaviour is unknown.
    const float hx = 300.0f, hy = 200.0f, hz = 60.0f;
    const int32_t nx = 8, ny = 8, nz = 4;
    const Shell s = makeBoxShell(hx, hy, hz, nx, ny, nz);
    const float pressure = 0.6f * 1.0e-4f * 2.0f * 980.0f * hz * (nx + 1) * (ny + 1);
    const int32_t body = aver_phys_softbody_create(
        s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
        nullptr, 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, pressure);
    check(body != 0, "the pool was created");
    step(3.0f);   // settle first, same reason as every other pool test in this file.

    std::vector<float> before(static_cast<size_t>(s.count()) * 3, 0.0f);
    check(aver_phys_softbody_vertices(body, before.data(), s.count()) == s.count(),
          "settled positions read back");

    // A point near one corner of the pool, mid-depth, and a radius generous enough to catch a real
    // cluster of this tessellation's vertices while clearly missing others -- CONFIRMED below, not
    // assumed, the same way testCharacterEmbeddedInPoolDoesNotMoveIt confirms its own overlap rather
    // than trusting the geometry by inspection.
    const float cx = -hx * 0.6f, cy = -hy * 0.6f, cz = hz + 5.0f;
    const float centre[3] = {cx, cy, cz};
    const float radiusCm = 150.0f;
    // Sideways, not straight down: the query point sits only ~5 cm above the floor's top face
    // (hz + 5 - hz = 5), so a downward shove would be measuring "does the floor stop it", not "does
    // the impulse move it". A push along the pool's long axis has 300+ cm of clear travel before it
    // would reach any other boundary.
    const float velCmPerS[3] = {300.0f, 0.0f, 0.0f};

    auto sqDist = [&](const std::vector<float>& v, int32_t i) {
        const float dx = v[static_cast<size_t>(i) * 3 + 0] - cx;
        const float dy = v[static_cast<size_t>(i) * 3 + 1] - cy;
        const float dz = v[static_cast<size_t>(i) * 3 + 2] - cz;
        return dx * dx + dy * dy + dz * dz;
    };
    // FAR, not merely "outside the radius" -- reported as DATA below, not asserted as a bound.
    // Measured first: at this radius/velocity, the far side of the SAME closed, pressurised shell
    // moved 12.4 cm against the touched cluster's 16.9 cm -- most of the way there, not a rounding
    // error. That is Jolt's own ApplyPressure being a BULK term, not a nearest-neighbour one: it is
    // computed from the shell's total enclosed volume every step, so any local push that changes that
    // volume changes the pressure force on every face at once, edge-adjacency or not. A "the far side
    // barely moves" assertion would be asserting a locality property this shell does not actually
    // have, for the same reason testPressureHoldsAShellUp needs pressure in the first place -- so this
    // stays a logged number, and the real regression guard below is the one comparison that IS true
    // regardless: the same vertices with the call against the same vertices without it.
    const float farThresholdCm = 350.0f;
    int32_t insideBefore = 0, outsideBefore = 0, farBefore = 0;
    for (int32_t i = 0; i < s.count(); ++i) {
        const float d2 = sqDist(before, i);
        if (d2 < radiusCm * radiusCm) ++insideBefore; else ++outsideBefore;
        if (d2 > farThresholdCm * farThresholdCm) ++farBefore;
    }
    check(insideBefore >= 3, "the query sphere catches a real cluster of vertices at this tessellation (" +
                             std::to_string(insideBefore) + ")");
    check(outsideBefore >= 3, "and clearly leaves others outside it (" + std::to_string(outsideBefore) + " )");
    check(farBefore >= 3, "and a real cluster sits far enough away to judge localisation against (" +
                          std::to_string(farBefore) + ")");

    const int32_t nudged = aver_phys_softbody_apply_impulse(body, centre, radiusCm, velCmPerS, 1.0f);
    check(nudged == insideBefore,
          "it reports nudging exactly the vertices the sphere actually contains (" +
          std::to_string(nudged) + " vs " + std::to_string(insideBefore) + ")");

    step(0.5f);   // short: this measures an immediate velocity kick, not a long settle -- long enough
                  // for a genuine push to separate itself from the pool's own idle jiggle, short
                  // enough that the constraint network has not yet pulled everything back into shape.

    std::vector<float> after(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, after.data(), s.count());

    auto movedCm = [&](int32_t i) {
        const float dx = after[static_cast<size_t>(i) * 3 + 0] - before[static_cast<size_t>(i) * 3 + 0];
        const float dy = after[static_cast<size_t>(i) * 3 + 1] - before[static_cast<size_t>(i) * 3 + 1];
        const float dz = after[static_cast<size_t>(i) * 3 + 2] - before[static_cast<size_t>(i) * 3 + 2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    float insideSum = 0.0f, farSum = 0.0f;
    for (int32_t i = 0; i < s.count(); ++i) {
        const float d2 = sqDist(before, i);
        if (d2 < radiusCm * radiusCm) insideSum += movedCm(i);
        if (d2 > farThresholdCm * farThresholdCm) farSum += movedCm(i);
    }
    const float insideMean = insideSum / static_cast<float>(insideBefore);
    const float farMean = farSum / static_cast<float>(farBefore);
    aver_phys_shutdown();

    // PAIRED CONTROL: identical pool, identical settle, NO call to apply_impulse. What the SAME
    // vertices do on their own over the same half-second -- same discipline as
    // testCharacterEmbeddedInPoolDoesNotMoveIt's paired control above.
    check(aver_phys_init() == 1, "physics started (control)");
    check(aver_phys_add_static_box(0, 0, -10, 800, 800, 10) != 0, "floor (control)");
    const int32_t bodyCtrl = aver_phys_softbody_create(
        s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
        nullptr, 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, pressure);
    step(3.0f);
    std::vector<float> beforeCtrl(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(bodyCtrl, beforeCtrl.data(), s.count());
    step(0.5f);
    std::vector<float> afterCtrl(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(bodyCtrl, afterCtrl.data(), s.count());
    float ctrlInsideSum = 0.0f;
    for (int32_t i = 0; i < s.count(); ++i) {
        if (sqDist(beforeCtrl, i) >= radiusCm * radiusCm) continue;
        const float dx = afterCtrl[static_cast<size_t>(i) * 3 + 0] - beforeCtrl[static_cast<size_t>(i) * 3 + 0];
        const float dy = afterCtrl[static_cast<size_t>(i) * 3 + 1] - beforeCtrl[static_cast<size_t>(i) * 3 + 1];
        const float dz = afterCtrl[static_cast<size_t>(i) * 3 + 2] - beforeCtrl[static_cast<size_t>(i) * 3 + 2];
        ctrlInsideSum += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const float ctrlInsideMean = ctrlInsideSum / static_cast<float>(insideBefore);
    aver_phys_shutdown();

    AVER_INFO("  treated pool: nudged vertices moved {} cm on average, the far side of the SAME pool "
              "moved {} cm; the SAME nudged vertices with no call at all moved {} cm",
              insideMean, farMean, ctrlInsideMean);

    // THE ACTUAL FINDING, AS A REGRESSION GUARD: the touched vertices moved measurably more than the
    // SAME vertices got with no call at all -- proof the CALL did this, not the pool's own settling or
    // its own pressure-driven jiggle (testCharacterEmbeddedInPoolDoesNotMoveIt's control measured
    // exactly that source of noise already: ~2.5 cm over a much longer 2 s). Unlike the passive path
    // this file spends its first half proving does nothing, this is a real, attributable reaction.
    check(insideMean > ctrlInsideMean + 5.0f,
          "the nudged vertices moved measurably more than the SAME vertices with no call at all (" +
          std::to_string(insideMean) + " cm vs " + std::to_string(ctrlInsideMean) + " cm)");
    // farMean (logged above) is intentionally not asserted against -- see farThresholdCm's own comment
    // for why "the far side barely moves" is not a property this pressurised shell actually has.
}

// ---- density gives a soft body REAL mass, and real mass changes how it sags ------------------------
//
// THE CLAIM UNDER TEST, restated from fluids::fluidParticleMassKg's own header comment
// (modules/fluids/include/aver/fluids/FluidVolume.hpp): mass = density * enclosedVolume /
// particleCount is REAL physics, not a fit, because Jolt's soft-body solver honours mass directly
// through invMass (PhysicsWorld.cpp's buildSoftShared: `v.mInvMass = invMasses ? invMasses[i] :
// 1.0f`). This restates that exact arithmetic here rather than linking Aver.Fluids to call it --
// the same "this test links the ABI and nothing else" discipline testPressureHoldsAShellUp already
// applies to fluidPressureFor's own formula (see this file's own opening comment) -- and checks it
// the way a caller outside the fluids module would see it: pin one wall of a closed box shell, let
// gravity and a soft (non-zero-compliance) edge constraint pull the rest of it down, and measure how
// far the far wall sags.
//
// WHY A PINNED WALL AND NOT A FREE FALL. Under gravity ALONE, in a vacuum, every mass falls at the
// same acceleration -- Newton's second law makes mass cancel out of a(t), so a free-falling shell's
// TRAJECTORY carries no information about how heavy it is, and a test built on one would prove
// nothing regardless of what invMasses said. What DOES depend on mass, for a FIXED compliance (XPBD
// compliance is an inverse stiffness: a constraint's steady-state stretch under a steady load scales
// with load / stiffness, same as an ordinary spring's x = F/k), is how far a constrained shell sags
// before its own edges' stretch balances gravity's pull on that extra mass -- exactly the "denser
// fluid has more inertia and sags harder" physics this field exists to make checkable.
static float pinnedShellSagCm(float densityKgM3) {
    check(aver_phys_init() == 1, "physics started");

    // A smaller box than the pool tests above -- this measures a per-particle MASS effect, not a
    // pressure/topology one, so there is no reason to pay for the pool's own 258-particle mesh.
    const float hx = 150.0f, hy = 100.0f, hz = 50.0f;
    const int32_t nx = 6, ny = 4, nz = 3;
    const Shell s = makeBoxShell(hx, hy, hz, nx, ny, nz);

    // mass = density * enclosedVolume / particleCount, restated from fluids::fluidParticleMassKg --
    // enclosedVolumeM3 is the box's own 8 hx hy hz, converted out of the CENTIMETRES makeBoxShell's
    // own arguments are in: 1 cm = 0.01 m, so 1 cm^3 = (0.01 m)^3 = 1e-6 m^3 -- the exact arithmetic
    // check fluids::kFluidCmCubedToM3's own comment names, and the one place this whole feature is a
    // factor of a million away from being silently wrong in either direction.
    const float enclosedVolumeM3 = 8.0f * hx * hy * hz * 1.0e-6f;
    const float massPerParticleKg = densityKgM3 * enclosedVolumeM3 / static_cast<float>(s.count());
    const float invMassFree = 1.0f / massPerParticleKg;

    // Pin the -X wall -- every vertex this shell's own construction put at x == -hx -- and give
    // everything else the SAME real, density-derived inverse mass (uniform per particle, matching
    // fluidParticleMassKg's own "one particle, one share" division). No partial pin: the whole wall,
    // so the "far" wall measured below is unambiguously the one doing all the sagging.
    std::vector<float> invMass(static_cast<size_t>(s.count()), invMassFree);
    int32_t pinnedCount = 0;
    for (int32_t i = 0; i < s.count(); ++i) {
        if (s.verts[static_cast<size_t>(i) * 3 + 0] <= -hx + 1.0f) {
            invMass[static_cast<size_t>(i)] = 0.0f;
            ++pinnedCount;
        }
    }
    check(pinnedCount > 0,
          "the -X wall of the shell was pinned (" + std::to_string(pinnedCount) + " particles)");

    // NO PRESSURE -- this isolates mass and compliance, which is not the fluid-pressure balance
    // testPressureHoldsAShellUp already covers. Compliance is the fluids module's own real
    // production value (kHeavyLiquidCompliance, FluidVolume.hpp) -- not a softened one invented for
    // this test. Damping is NOT the production default, and MEASURABLY so: a first version of this
    // test at kDefaultFluidDamping (0.1) actually FAILED, in the WRONG direction (3000 kg/m^3
    // sagging 124.9 cm against 100 kg/m^3's 269.3 cm, at 3 s) -- not because mass stopped mattering,
    // but because a pinned shell over a spring-like edge is an OSCILLATOR, and its natural frequency
    // sqrt(k/m) falls as mass rises, so a fixed 3 s window catches different masses at different,
    // incomparable PHASES of their own swing rather than settled. Heavier damping (1.0, i.e. a 1 s
    // velocity-decay time constant against Jolt's own dv/dt = -damping * v) forces every mass to
    // settle toward its OWN equilibrium stretch well inside the 6 s this test now runs, which is the
    // regime testPressureHoldsAShellUp's own pool tests already settle in too (that one at 4 s).
    // This is what "measured, not guessed" means for a test constant, the same discipline
    // kFluidPressureHeadroom's own sweep comment states for a production one.
    const int32_t body = aver_phys_softbody_create(
        s.verts.data(), s.count(), s.indices.data(), static_cast<int32_t>(s.indices.size()),
        invMass.data(), 0.0f, 0.0f, 0.0f, /*compliance*/ 1.0e-4f, /*pressure*/ 0.0f,
        /*damping*/ 1.0f);
    if (body == 0) { aver_phys_shutdown(); return -1.0f; }

    std::vector<float> before(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, before.data(), s.count());

    step(6.0f);

    std::vector<float> after(static_cast<size_t>(s.count()) * 3, 0.0f);
    aver_phys_softbody_vertices(body, after.data(), s.count());

    // The +X wall -- as far from the pin as this shell reaches -- averaged over every particle on
    // it, the same "over the whole face, not a single vertex" discipline testClothHangs' own
    // pinnedDrop/freeDrop already use above.
    float farSag = 0.0f;
    int32_t farCount = 0;
    for (int32_t i = 0; i < s.count(); ++i) {
        if (s.verts[static_cast<size_t>(i) * 3 + 0] >= hx - 1.0f) {
            farSag += before[static_cast<size_t>(i) * 3 + 2] - after[static_cast<size_t>(i) * 3 + 2];
            ++farCount;
        }
    }
    aver_phys_shutdown();
    check(farCount > 0, "the +X wall (farthest from the pin) has particles to measure sag from");
    return farCount > 0 ? farSag / static_cast<float>(farCount) : -1.0f;
}

static void testDensityChangesSagUnderGravity() {
    AVER_INFO("-- real density gives real mass, and real mass changes how a pinned shell sags --");

    // TWO RUNS AT THE SAME DENSITY FIRST -- the noise floor any REAL difference below has to clear,
    // the identical control-before-treatment discipline testCharacterEmbeddedInPoolDoesNotMoveIt and
    // testApplyImpulseMovesNearbyVertices already use above. aver_phys_step integrates a fixed,
    // deterministic timestep with no randomness anywhere in this path, so two identical runs are
    // expected to reproduce the same result up to ordinary float accumulation, not merely "close".
    const float controlA = pinnedShellSagCm(500.0f);
    const float controlB = pinnedShellSagCm(500.0f);
    check(controlA >= 0.0f && controlB >= 0.0f, "both control runs produced a reading");
    const float noiseFloor = std::abs(controlA - controlB);
    AVER_INFO("  same density (500 kg/m^3) twice: {} cm vs {} cm sag (noise floor {} cm)",
              controlA, controlB, noiseFloor);
    check(noiseFloor < 0.5f,
          "two runs at the identical density sag by the identical amount, within floating-point "
          "noise (" + std::to_string(noiseFloor) + " cm)");

    // NOW THE TREATMENT: light vs. heavy, same shell, same pin, same compliance, same everything
    // else -- the ONLY thing that differs is densityKgM3.
    const float light = pinnedShellSagCm(100.0f);
    const float heavy = pinnedShellSagCm(3000.0f);
    check(light >= 0.0f && heavy >= 0.0f, "both treatment runs produced a reading");
    AVER_INFO("  100 kg/m^3 sagged {} cm; 3000 kg/m^3 sagged {} cm", light, heavy);

    // THE DIRECTION PHYSICS PREDICTS: heavier particles pulling on the same, fixed-compliance edges
    // stretch them further -- x = F/k with F = m g and k fixed by compliance -- so the denser shell
    // must sag MORE, and by a margin that clears the noise floor measured above, not by a hair.
    check(heavy > light + std::max(noiseFloor * 5.0f, 1.0f),
          "the denser shell (3000 kg/m^3) sagged further than the lighter one (100 kg/m^3): " +
          std::to_string(heavy) + " cm vs " + std::to_string(light) + " cm, clear of the " +
          std::to_string(noiseFloor) + " cm noise floor");
}

// ---- an impulse wakes a body that has gone to sleep --------------------------------------------
//
// THE REGRESSION THIS EXISTS FOR, and it is the one that matters most for gameplay.
// aver_phys_softbody_apply_impulse writes vertex velocities directly. That is a poke at memory, not
// a physics event -- Jolt never sees it. So a body the solver has already put to sleep stays asleep
// with its new velocities sitting unread, while the call cheerfully returns a positive nudged count
// telling the caller it worked.
//
// WHY IT HID FOR SO LONG. Every fluid used to have unit particle mass and light damping, which
// together kept a settled pool jittering just above Jolt's sleep threshold, so the bug could not
// fire. Real density-scaled mass plus the damping the thicker presets map to settles a volume
// genuinely still -- it sleeps a few seconds after spawn, and from then on a player could walk
// through it and nothing would move. Every existing test disturbed a body that had never had time
// to fall asleep, so none of them could see it.
static void testImpulseWakesASleepingBody() {
    AVER_INFO("-- an impulse wakes a body the solver has put to sleep --");
    check(aver_phys_init() == 1, "physics started");
    check(aver_phys_add_static_box(0, 0, -10, 20000, 20000, 10) != 0, "a large static floor exists");

    const float hx = 300.0f, hy = 200.0f, hz = 60.0f;
    const int32_t nx = 8, ny = 8, nz = 4;
    const Shell sh = makeBoxShell(hx, hy, hz, nx, ny, nz);

    // HEAVY AND HEAVILY DAMPED, which is exactly the regime the thicker presets produce: real
    // particle mass rather than the historic 1, and damping high enough that the shell stops moving
    // rather than idling. Both are needed -- either alone leaves enough residual motion to keep the
    // body awake, which is why the old defaults never tripped this.
    std::vector<float> invMasses(static_cast<size_t>(sh.count()), 1.0f / 40.0f);
    const float pressure = 0.6f * 1.0e-4f * 2.0f * 980.0f * hz * (nx + 1) * (ny + 1) * 40.0f;
    const int32_t body = aver_phys_softbody_create(
        sh.verts.data(), sh.count(), sh.indices.data(), static_cast<int32_t>(sh.indices.size()),
        invMasses.data(), 0.0f, 0.0f, hz + 5.0f, 1.0e-4f, pressure, 3.0f, 5);
    check(body != 0, "a heavy, heavily damped shell was created");

    auto maxSpeedOver = [&](float seconds) {
        std::vector<float> a(static_cast<size_t>(sh.count()) * 3, 0.0f), b = a;
        aver_phys_softbody_vertices(body, a.data(), sh.count());
        const float fixed = aver_phys_fixed_step();
        float peak = 0.0f;
        for (float t = 0.0f; t < seconds; t += fixed) {
            aver_phys_step(fixed);
            aver_phys_softbody_vertices(body, b.data(), sh.count());
            for (int32_t i = 0; i < sh.count(); ++i) {
                float d = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float e = b[static_cast<size_t>(i) * 3 + usize(k)] - a[static_cast<size_t>(i) * 3 + usize(k)];
                    d += e * e;
                }
                peak = std::sqrt(d) > peak ? std::sqrt(d) : peak;
            }
            a = b;
        }
        return peak;
    };

    // Long enough for the solver to settle it AND for its sleep timer to expire.
    step(10.0f);
    const float quiet = maxSpeedOver(0.5f);
    check(quiet < 0.05f, "it has genuinely gone still (peak step motion " + std::to_string(quiet) + " cm)");

    const float centre[3]   = {0.0f, 0.0f, hz + 5.0f};
    const float velocity[3] = {400.0f, 0.0f, 0.0f};
    const int32_t nudged = aver_phys_softbody_apply_impulse(body, centre, 2000.0f, velocity, 1.0f);
    check(nudged > 0, "the impulse reports touching " + std::to_string(nudged) + " particles");

    // THE ASSERTION THE BUG FAILED. Before the fix this was 0.000 -- the velocities were written and
    // the body, being asleep, never integrated them. A positive nudged count above proves the write
    // happened, so a still-motionless body here isolates the wake, not the reach.
    const float moved = maxSpeedOver(0.5f);
    check(moved > 10.0f * (quiet + 1e-4f),
          "and it MOVES afterwards (" + std::to_string(moved) + " cm against a quiet " +
          std::to_string(quiet) + " cm)");

    aver_phys_shutdown();
}

int main() {
    AVER_INFO("SoftBodyTest");
    testClothHangs();
    testSoftBodyLandsOnFloor();
    testSkinnedFollowsItsJoints();
    testSkinnedCanLeaveItsSkin();
    testHandleContract();
    testPressureHoldsAShellUp();
    testImpulseWakesASleepingBody();
    testCharacterEmbeddedInPoolDoesNotMoveIt();
    testCharacterSweepDoesNotVisiblyMoveTheSoftBody();
    testApplyImpulseMovesNearbyVertices();
    testDensityChangesSagUnderGravity();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
