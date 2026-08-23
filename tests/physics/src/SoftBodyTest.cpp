// Soft bodies through the plain-C ABI: a cloth that hangs, a body that lands on a floor rather than
// through it, and a skinned body that follows an animation palette.
//
// LINKS NO JOLT, deliberately, unlike PhysicsTest beside it. That test is the ONE place allowed to
// speak both sides (see tests/physics/CMakeLists.txt), because the conversion property it checks is
// invisible from outside. Everything here is visible from outside -- it is what a caller of the ABI
// can see -- so it is checked the way a caller would see it, and this file is the evidence that the
// rule about Jolt staying private to the module still holds.
#include "aver/physics/physics_abi.h"

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

    // 0.6 * 2 g hz (nx+1)(ny+1) in Jolt's metres -- water::fluidPressureFor's own formula, restated
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

int main() {
    AVER_INFO("SoftBodyTest");
    testClothHangs();
    testSoftBodyLandsOnFloor();
    testSkinnedFollowsItsJoints();
    testSkinnedCanLeaveItsSkin();
    testHandleContract();
    testPressureHoldsAShellUp();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
