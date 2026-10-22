// PhysicsDebugTest -- Jolt's debug renderer as a line feed (physics_debug_abi.h), through the ABI only.
//
//   * a box and a sphere drawn with BODIES give lines whose extents are the shapes' own, in ENGINE axes (a box taller than wide
//     proves +Z up and +X forward),
//   * the radius culls: a body 100 m away is absent at 40 m and present at 150 m,
//   * maxLines truncates and sets the flag, and the count stays at the budget,
//   * static triangles: a 20,000-triangle mesh and a heightfield yield only the triangles near the camera, far fewer than the
//     mesh has, none of them far away; BODIES never draws a mesh whole,
//   * CONTACTS (armed) gives the LAST fixed step's lines only: the same count after 1 step and after 4 steps in one call;
//     disarmed, none,
//   * CONSTRAINTS draws a hinge, LABELS carry the body handle,
//   * flags == 0 and no world answer 0, the scheme setter refuses an unknown scheme,
//   * the buffer is 12 floats a line with colours in 0..1.
//
// SKIPS (exit 0, loudly) when the build has no debug renderer (cmake -DAVER_PHYSICS_DEBUG_RENDERER=OFF): the ABI then answers
// available == 0 and does nothing else, which is checked.
//
// Hand-run, exit code = failure count, same shape as PhysicsConfigTest.cpp.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_debug_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

static const float kDt = 1.0f / 60.0f;
static void step(int n) { for (int i = 0; i < n; ++i) aver_phys_step(kDt); }

static void freshWorld() {
    aver_phys_debug_arm(0);
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_init();
}

struct Lines {
    int n = 0;
    std::vector<float> v;   // 12 floats a line
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
};

// Collects and reads the lines back, with their bounding box.
static Lines collect(uint32_t flags, const float* cam, float radiusCm, int maxLines = 200000) {
    Lines L;
    L.n = aver_phys_debug_collect(flags, cam, radiusCm, maxLines);
    if (L.n <= 0) { L.n = 0; return L; }
    L.v.resize(static_cast<size_t>(L.n) * 12);
    const int got = aver_phys_debug_lines(L.v.data(), L.n);
    if (got != L.n) L.n = got;
    for (int a = 0; a < 3; ++a) { L.lo[a] = 1.0e30f; L.hi[a] = -1.0e30f; }
    for (int i = 0; i < L.n; ++i)
        for (int e = 0; e < 2; ++e)
            for (int a = 0; a < 3; ++a) {
                const float x = L.v[static_cast<size_t>(i) * 12 + static_cast<size_t>(e) * 6 + static_cast<size_t>(a)];
                L.lo[a] = std::min(L.lo[a], x);
                L.hi[a] = std::max(L.hi[a], x);
            }
    return L;
}

static bool approx(float a, float b, float tol = 1.0f) { return std::fabs(a - b) <= tol; }

static void testShapesInEngineAxes() {
    AVER_INFO("=== a box and a sphere, in engine axes ===");
    freshWorld();
    aver_phys_add_static_box(100, 200, 300, 20, 30, 50);   // 40 wide (X), 60 deep (Y), 100 tall (Z)
    Lines b = collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f);
    check(b.n > 0, "BODIES draws a box");
    check(approx(b.lo[0], 80) && approx(b.hi[0], 120), "box X extent is 80..120 (half 20 about 100)");
    check(approx(b.lo[1], 170) && approx(b.hi[1], 230), "box Y extent is 170..230 (half 30 about 200)");
    check(approx(b.lo[2], 250) && approx(b.hi[2], 350), "box Z extent is 250..350: taller than wide, so +Z is up");

    freshWorld();
    aver_phys_add_dynamic_sphere(-400, 50, 120, 40, 5.0f);
    Lines s = collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f);
    check(s.n > 0, "BODIES draws a sphere");
    check(approx(s.lo[0], -440) && approx(s.hi[0], -360) && approx(s.lo[1], 10) && approx(s.hi[1], 90) && approx(s.lo[2], 80) && approx(s.hi[2], 160),
          "sphere extents are its centre +- 40 on every axis");

    // Bounds: the world-space box of a body, drawn as 12 edges.
    freshWorld();
    aver_phys_add_static_box(0, 0, 0, 10, 20, 30);
    Lines bb = collect(AVER_PHYS_DBG_BOUNDS, nullptr, 0.0f);
    check(bb.n == 12, "BOUNDS draws the 12 edges of the bounding box");
    check(approx(bb.lo[0], -10) && approx(bb.hi[0], 10) && approx(bb.lo[2], -30) && approx(bb.hi[2], 30), "the bounds are the body's AABB in engine axes");
}

static void testRadius() {
    AVER_INFO("=== the radius culls ===");
    freshWorld();
    aver_phys_add_static_box(10000, 0, 0, 20, 20, 20);   // 100 m away
    const float cam[3] = {0, 0, 0};
    check(collect(AVER_PHYS_DBG_BODIES, cam, 4000.0f).n == 0, "a body 100 m away is absent at a 40 m radius");
    check(collect(AVER_PHYS_DBG_BODIES, cam, 15000.0f).n > 0, "and present at 150 m");
    check(collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f).n > 0, "no camera means no culling");
    check(collect(AVER_PHYS_DBG_BOUNDS, cam, 4000.0f).n == 0, "bounds are culled too");
}

static void testBudget() {
    AVER_INFO("=== the line budget ===");
    freshWorld();
    for (int i = 0; i < 20; ++i) aver_phys_add_static_box(static_cast<float>(i) * 100.0f, 0, 0, 20, 20, 20);
    const int n = aver_phys_debug_collect(AVER_PHYS_DBG_BODIES | AVER_PHYS_DBG_BOUNDS, nullptr, 0.0f, 10);
    check(n == 10, "the count is clamped to maxLines");
    check(aver_phys_debug_truncated() == 1, "and the truncated flag is set");
    const int all = aver_phys_debug_collect(AVER_PHYS_DBG_BODIES | AVER_PHYS_DBG_BOUNDS, nullptr, 0.0f, 100000);
    check(all > 10 && aver_phys_debug_truncated() == 0, "with room, nothing is truncated");
}

static void testStaticTriangles() {
    AVER_INFO("=== static triangles: only what is near the camera ===");
    constexpr int N = 100;   // 100 x 100 cells of 100 cm: 20,000 triangles, 10,000 cm square
    freshWorld();
    std::vector<float> verts;
    for (int j = 0; j <= N; ++j)
        for (int i = 0; i <= N; ++i) {
            verts.push_back(static_cast<float>(i - N / 2) * 100.0f);
            verts.push_back(static_cast<float>(j - N / 2) * 100.0f);
            verts.push_back(0.0f);
        }
    std::vector<int32_t> idx;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const int a = j * (N + 1) + i, b = a + 1, c = a + N + 1, d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    check(aver_phys_add_mesh(verts.data(), static_cast<int32_t>(verts.size() / 3), idx.data(), static_cast<int32_t>(idx.size()), 0, 0, 0) != 0,
          "the mesh builds");

    const float cam[3] = {0, 0, 50};
    check(collect(AVER_PHYS_DBG_BODIES, cam, 100000.0f).n == 0, "BODIES never draws a mesh whole");
    check(collect(AVER_PHYS_DBG_STATIC_TRIANGLES, nullptr, 0.0f).n == 0, "static triangles need a camera and a radius");

    const Lines near10 = collect(AVER_PHYS_DBG_STATIC_TRIANGLES, cam, 1000.0f);
    const Lines near30 = collect(AVER_PHYS_DBG_STATIC_TRIANGLES, cam, 3000.0f);
    const int totalEdges = 3 * N * N + 2 * N;   // unique edges of the whole grid
    check(near10.n > 0, "triangles inside a 10 m radius are drawn");
    check(near10.n * 10 < totalEdges, "far fewer lines than the mesh has edges");
    check(near30.n > near10.n, "a larger radius draws more");
    float farthest = 0;
    for (int i = 0; i < near10.n; ++i)
        for (int e = 0; e < 2; ++e) {
            const float* p = &near10.v[static_cast<size_t>(i) * 12 + static_cast<size_t>(e) * 6];
            farthest = std::max(farthest, std::sqrt(p[0] * p[0] + p[1] * p[1] + (p[2] - 50) * (p[2] - 50)));
        }
    check(farthest < 1000.0f + 150.0f, "no line reaches beyond the radius plus one cell");
    check(near10.n == collect(AVER_PHYS_DBG_STATIC_TRIANGLES, cam, 1000.0f).n, "the same camera gives the same count");

    freshWorld();
    constexpr int H = 129;
    std::vector<float> hf(static_cast<size_t>(H) * H, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < H; ++x) hf[static_cast<size_t>(y) * H + x] = 20.0f * std::sin(static_cast<float>(x) * 0.3f);
    const int32_t field = aver_phys_add_heightfield(hf.data(), H, 100.0f, -6400.0f, -6400.0f, 0.0f);
    check(field != 0, "the heightfield builds");
    // The camera goes to the field's own centre, so the test does not depend on which corner the body origin is.
    float mn[3] = {}, mx[3] = {};
    aver_phys_body_aabb(field, mn, mx);
    const float hcam[3] = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
    const Lines hn = collect(AVER_PHYS_DBG_STATIC_TRIANGLES, hcam, 1000.0f);
    check(hn.n > 0 && hn.n * 10 < 3 * 128 * 128, "a heightfield yields only the triangles near the camera");
    check(collect(AVER_PHYS_DBG_BODIES, hcam, 100000.0f).n == 0, "BODIES never draws a heightfield whole");
}

static void testContacts() {
    AVER_INFO("=== contacts: the last fixed step only ===");
    freshWorld();
    aver_phys_setting_set(AVER_PHYS_WS_ALLOW_SLEEPING, 0);   // a sleeping box makes no contacts to draw
    aver_phys_add_static_box(0, 0, -50, 3000, 3000, 50);
    aver_phys_add_dynamic_box(0, 0, 10.5f, 10, 10, 10, 10.0f);
    step(30);

    check(collect(AVER_PHYS_DBG_CONTACTS, nullptr, 0.0f).n == 0, "disarmed, contacts draw nothing");
    check(aver_phys_debug_arm(AVER_PHYS_DBG_CONTACTS) == static_cast<int32_t>(AVER_PHYS_DBG_CONTACTS), "arm answers the mask it armed");
    step(1);
    const Lines one = collect(AVER_PHYS_DBG_CONTACTS, nullptr, 0.0f);
    step(4);
    const Lines four = collect(AVER_PHYS_DBG_CONTACTS, nullptr, 0.0f);
    check(one.n > 0, "a resting box has contact lines from the last step");
    check(one.n == four.n, "the count is the same after 1 step and after 4 steps in one call: the last step's, not a smear");

    check(aver_phys_debug_arm(0) == 0, "arm(0) disarms");
    step(2);
    check(collect(AVER_PHYS_DBG_CONTACTS, nullptr, 0.0f).n == 0, "disarmed again, nothing");
}

static void testConstraintsAndLabels() {
    AVER_INFO("=== constraints and labels ===");
    freshWorld();
    const int32_t a = aver_phys_add_static_box(0, 0, 100, 10, 10, 10);
    const int32_t b = aver_phys_add_dynamic_box(40, 0, 100, 10, 10, 10, 5.0f);
    const float pt[3] = {20, 0, 100}, axis[3] = {0, 1, 0}, normal[3] = {1, 0, 0};
    check(aver_phys_joint_hinge(a, b, pt, axis, normal, -0.5f, 0.5f) != 0, "the hinge builds");
    check(collect(AVER_PHYS_DBG_CONSTRAINTS, nullptr, 0.0f).n > 0, "CONSTRAINTS draws the hinge");
    check(collect(AVER_PHYS_DBG_LIMITS, nullptr, 0.0f).n > 0, "LIMITS draws its limit arc");
    check(collect(AVER_PHYS_DBG_BOUNDS, nullptr, 0.0f).n > 0 && aver_phys_debug_label_count() == 0, "labels are empty without LABELS");

    collect(AVER_PHYS_DBG_LABELS, nullptr, 0.0f);
    const int count = aver_phys_debug_label_count();
    check(count >= 1, "LABELS gives a label for the moving body");
    bool found = false;
    const std::string want = "#" + std::to_string(b);
    for (int i = 0; i < count; ++i) {
        float xyz[3] = {}, rgb[3] = {};
        uint8_t text[96] = {};
        if (aver_phys_debug_label_get(i, xyz, rgb, text, static_cast<int32_t>(sizeof text)) &&
            std::string(reinterpret_cast<const char*>(text)).find(want) != std::string::npos)
            found = approx(xyz[0], 40, 5.0f) && approx(xyz[1], 0, 5.0f) && xyz[2] > 100.0f;
    }
    check(found, "the label carries the body handle and sits above the body, in engine cm");
    uint8_t t2[8];
    check(aver_phys_debug_label_get(count, nullptr, nullptr, t2, 8) == 0, "a bad label index is refused");
}

static void testRefusalsAndLayout() {
    AVER_INFO("=== refusals and the buffer layout ===");
    freshWorld();
    aver_phys_add_dynamic_box(0, 0, 100, 20, 20, 20, 5.0f);
    check(aver_phys_debug_collect(0, nullptr, 0.0f, 1000) == 0, "flags == 0 answers 0");
    check(aver_phys_debug_collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f, 0) == 0, "a zero budget answers 0");
    for (int s = 0; s < AVER_PHYS_DBG_SCHEME_COUNT; ++s) check(aver_phys_debug_set_color_scheme(s) == 1, "scheme " + std::to_string(s) + " is accepted");
    check(aver_phys_debug_set_color_scheme(AVER_PHYS_DBG_SCHEME_COUNT) == 0 && aver_phys_debug_set_color_scheme(-1) == 0, "an unknown scheme is refused");
    for (int s = 0; s < AVER_PHYS_DBG_SCHEME_COUNT; ++s) {
        aver_phys_debug_set_color_scheme(s);
        check(collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f).n > 0, "scheme " + std::to_string(s) + " draws the body");
    }
    aver_phys_debug_set_color_scheme(AVER_PHYS_DBG_SCHEME_MOTION);

    const Lines L = collect(AVER_PHYS_DBG_BODIES | AVER_PHYS_DBG_BOUNDS | AVER_PHYS_DBG_COM, nullptr, 0.0f);
    bool colours = L.n > 0, finite = true;
    for (float f : L.v) finite = finite && std::isfinite(f);
    for (int i = 0; i < L.n; ++i)
        for (int e = 0; e < 2; ++e)
            for (int c = 3; c < 6; ++c) {
                const float x = L.v[static_cast<size_t>(i) * 12 + static_cast<size_t>(e) * 6 + static_cast<size_t>(c)];
                if (x < 0.0f || x > 1.0f) colours = false;
            }
    check(L.v.size() == static_cast<size_t>(L.n) * 12, "12 floats a line");
    check(colours && finite, "every colour is within 0..1 and every coordinate is finite");
    check(aver_phys_debug_lines(nullptr, 10) == 0 && aver_phys_debug_lines(const_cast<float*>(L.v.data()), 0) == 0, "lines with no buffer or no room copies nothing");

    aver_phys_shutdown();
    check(aver_phys_debug_collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f, 1000) == 0, "no world answers 0");
    check(aver_phys_debug_arm(AVER_PHYS_DBG_CONTACTS) == 0, "arm needs a world");
    check(aver_phys_debug_label_count() == 0 && aver_phys_debug_lines(const_cast<float*>(L.v.data()), 10) == 0, "and nothing is left to read");
}

int main() {
    check(aver_phys_debug_abi_version() == AVER_PHYS_DEBUG_ABI_VERSION, "the ABI version answers");
    const bool flag = (aver_phys_build_flags() & AVER_PHYS_BUILD_DEBUG_RENDERER) != 0;
    check((aver_phys_debug_available() != 0) == flag, "available agrees with the build flags");
    if (!aver_phys_debug_available()) {
        AVER_WARN("=== SKIPPED: this build has no Jolt debug renderer ===");
        aver_phys_init();
        check(aver_phys_debug_collect(AVER_PHYS_DBG_BODIES, nullptr, 0.0f, 1000) == 0, "collect answers 0 without a renderer");
        check(aver_phys_debug_arm(AVER_PHYS_DBG_CONTACTS) == 0, "arm answers 0 without a renderer");
        aver_phys_shutdown();
        return g_failures;
    }
    testShapesInEngineAxes();
    testRadius();
    testBudget();
    testStaticTriangles();
    testContacts();
    testConstraintsAndLabels();
    testRefusalsAndLayout();
    aver_phys_shutdown();
    AVER_INFO("=== PhysicsDebugTest: {} checks, {} failures ===", g_checks, g_failures);
    return g_failures;
}
