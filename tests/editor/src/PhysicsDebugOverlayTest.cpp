// PhysicsDebugOverlayTest -- the editor side of Show > Physics (sandbox/src/PhysicsDebugOverlay.hpp), against a FAKE feed:
// options to AVER_PHYS_DBG_* flags, what physicsDebugCollect asks of the module (scheme, arm, collect with the eye, radius and
// budget), and what it hands back (the line layout, the label pass-through and colour packing, the truncated flag). No ImGui,
// no window, no physics world: the module's own behaviour is PhysicsDebugTest's job.
#include "PhysicsDebugOverlay.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_debug_abi.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// ---- the fake module --------------------------------------------------------------------------------------------

struct FakeLabel { f32 xyz[3]; f32 rgb[3]; const char* text; };
static struct Fake {
    int schemeCalls = 0, lastScheme = -1;
    int armCalls = 0; u32 lastArm = 0xFFFFFFFFu;
    int collectCalls = 0; u32 lastFlags = 0; f32 lastCam[3] = {}; bool lastCamNull = false; f32 lastRadius = 0; i32 lastBudget = 0;
    i32 lineCount = 0;          // what collect reports
    i32 truncatedAnswer = 0;
    std::vector<f32> lineData;  // 12 floats per line
    std::vector<FakeLabel> labels;
    void reset() { *this = Fake(); }
} g_fake;

static i32 fakeScheme(i32 s) { ++g_fake.schemeCalls; g_fake.lastScheme = s; return 1; }
static i32 fakeArm(u32 f) { ++g_fake.armCalls; g_fake.lastArm = f; return static_cast<i32>(f); }
static i32 fakeCollect(u32 f, const f32* cam, f32 radius, i32 budget) {
    ++g_fake.collectCalls;
    g_fake.lastFlags = f;
    g_fake.lastCamNull = cam == nullptr;
    if (cam) std::memcpy(g_fake.lastCam, cam, sizeof g_fake.lastCam);
    g_fake.lastRadius = radius;
    g_fake.lastBudget = budget;
    return g_fake.lineCount;
}
static i32 fakeTruncated() { return g_fake.truncatedAnswer; }
static i32 fakeLines(f32* out, i32 maxLines) {
    const i32 n = static_cast<i32>(g_fake.lineData.size() / 12) < maxLines ? static_cast<i32>(g_fake.lineData.size() / 12) : maxLines;
    if (n > 0) std::memcpy(out, g_fake.lineData.data(), static_cast<size_t>(n) * 12 * sizeof(f32));
    return n;
}
static i32 fakeLabelCount() { return static_cast<i32>(g_fake.labels.size()); }
static i32 fakeLabelGet(i32 i, f32* xyz, f32* rgb, u8* text, i32 cap) {
    if (i < 0 || i >= static_cast<i32>(g_fake.labels.size())) return 0;
    const FakeLabel& l = g_fake.labels[static_cast<size_t>(i)];
    std::memcpy(xyz, l.xyz, sizeof l.xyz);
    std::memcpy(rgb, l.rgb, sizeof l.rgb);
    std::snprintf(reinterpret_cast<char*>(text), static_cast<size_t>(cap), "%s", l.text);
    return 1;
}
static const PhysicsDebugSource kFake = {&fakeScheme, &fakeArm, &fakeCollect, &fakeTruncated, &fakeLines, &fakeLabelCount, &fakeLabelGet};

// ---- cases ------------------------------------------------------------------------------------------------------

static void testFlags() {
    AVER_INFO("=== options to flags ===");
    check(physicsDebugFlags(PhysicsDebugOptions{}) == 0, "nothing on is flags 0, and any() is false");
    check(!PhysicsDebugOptions{}.any(), "any() is false by default");

    struct One { bool PhysicsDebugOptions::*opt; u32 flag; const char* name; };
    const One ones[] = {
        {&PhysicsDebugOptions::bodies, AVER_PHYS_DBG_BODIES, "bodies"},
        {&PhysicsDebugOptions::bounds, AVER_PHYS_DBG_BOUNDS, "bounds"},
        {&PhysicsDebugOptions::centreOfMass, AVER_PHYS_DBG_COM, "centreOfMass"},
        {&PhysicsDebugOptions::velocity, AVER_PHYS_DBG_VELOCITY, "velocity"},
        {&PhysicsDebugOptions::sleep, AVER_PHYS_DBG_SLEEP, "sleep"},
        {&PhysicsDebugOptions::mass, AVER_PHYS_DBG_MASS, "mass"},
        {&PhysicsDebugOptions::constraints, AVER_PHYS_DBG_CONSTRAINTS, "constraints"},
        {&PhysicsDebugOptions::limits, AVER_PHYS_DBG_LIMITS, "limits"},
        {&PhysicsDebugOptions::contacts, AVER_PHYS_DBG_CONTACTS, "contacts"},
        {&PhysicsDebugOptions::supportFaces, AVER_PHYS_DBG_SUPPORT_FACES, "supportFaces"},
        {&PhysicsDebugOptions::softVertices, AVER_PHYS_DBG_SOFT_VERTICES, "softVertices"},
        {&PhysicsDebugOptions::softEdges, AVER_PHYS_DBG_SOFT_EDGES, "softEdges"},
        {&PhysicsDebugOptions::characters, AVER_PHYS_DBG_CHARACTERS, "characters"},
        {&PhysicsDebugOptions::staticTriangles, AVER_PHYS_DBG_STATIC_TRIANGLES, "staticTriangles"},
        {&PhysicsDebugOptions::linearCast, AVER_PHYS_DBG_LINEAR_CAST, "linearCast"},
        {&PhysicsDebugOptions::labels, AVER_PHYS_DBG_LABELS, "labels"},
    };
    u32 all = 0;
    PhysicsDebugOptions every;
    for (const One& o : ones) {
        PhysicsDebugOptions p;
        p.*(o.opt) = true;
        check(physicsDebugFlags(p) == o.flag, std::string(o.name) + " maps to exactly its flag");
        check(p.any(), std::string(o.name) + " alone makes any() true");
        all |= o.flag;
        every.*(o.opt) = true;
    }
    check(physicsDebugFlags(every) == all, "every option on is the OR of all the flags");
    check((all & AVER_PHYS_DBG_ISLANDS) == 0, "the island scheme is a scheme, not an option flag");
}

static void testCollect() {
    AVER_INFO("=== physicsDebugCollect against a fake feed ===");
    PhysicsDebugOptions o;
    o.bodies = true;
    o.contacts = true;
    o.colourScheme = 3;
    o.radiusCm = 2500.0f;
    o.maxLines = 4321;

    g_fake.reset();
    g_fake.lineCount = 3;
    for (int i = 0; i < 36; ++i) g_fake.lineData.push_back(static_cast<f32>(i) * 0.5f);
    g_fake.truncatedAnswer = 1;
    g_fake.labels.push_back({{10, 20, 30}, {1.0f, 0.5f, 0.0f}, "#7  12 kg  340 cm/s"});
    g_fake.labels.push_back({{-1, -2, -3}, {0.0f, 0.0f, 1.0f}, "second"});

    PhysicsDebugFrame frame;
    frame.lines.resize(99);   // stale content must go
    physicsDebugCollect(o, Vec3{100, 200, 300}, frame, kFake);

    check(g_fake.lastScheme == 3, "the colour scheme is passed on");
    check(g_fake.lastArm == (AVER_PHYS_DBG_BODIES | AVER_PHYS_DBG_CONTACTS), "arm is called with the full flag set");
    check(g_fake.collectCalls == 1 && g_fake.lastFlags == (AVER_PHYS_DBG_BODIES | AVER_PHYS_DBG_CONTACTS), "collect gets the flags");
    check(!g_fake.lastCamNull && g_fake.lastCam[0] == 100 && g_fake.lastCam[1] == 200 && g_fake.lastCam[2] == 300, "collect gets the eye");
    check(g_fake.lastRadius == 2500.0f && g_fake.lastBudget == 4321, "collect gets the radius and the line budget");

    check(frame.lines.size() == 6, "three lines make six vertices (the stale content is gone)");
    bool layout = frame.lines.size() == 6;
    for (size_t v = 0; layout && v < 6; ++v) {
        const f32* want = &g_fake.lineData[v * 6];
        layout = frame.lines[v].px == want[0] && frame.lines[v].py == want[1] && frame.lines[v].pz == want[2] &&
                 frame.lines[v].r == want[3] && frame.lines[v].g == want[4] && frame.lines[v].b == want[5];
    }
    check(layout, "a vertex is (x y z r g b), straight from the feed");
    check(frame.truncated, "the truncated flag is passed through");

    check(frame.labels.size() == 2, "both labels arrive");
    if (frame.labels.size() == 2) {
        check(frame.labels[0].text == "#7  12 kg  340 cm/s", "label text passes through");
        check(frame.labels[0].world.x == 10 && frame.labels[0].world.y == 20 && frame.labels[0].world.z == 30, "label position passes through");
        check(frame.labels[0].rgba == 0xFFFF8000u, "label colour packs to 0xAARRGGBB (1.0, 0.5, 0.0 is FF8000)");
        check(frame.labels[1].rgba == 0xFF0000FFu, "blue packs to 0000FF");
    }

    // A feed that reports lines but copies fewer: the frame follows what was copied.
    g_fake.reset();
    g_fake.lineCount = 5;
    for (int i = 0; i < 24; ++i) g_fake.lineData.push_back(1.0f);   // only two lines of data
    physicsDebugCollect(o, Vec3{0, 0, 0}, frame, kFake);
    check(frame.lines.size() == 4, "the frame holds the lines the feed actually copied");
    check(!frame.truncated && frame.labels.empty(), "and its flags and labels are refreshed");

    // A budget of zero or less still asks for one line: the module treats 0 as 'draw nothing'.
    g_fake.reset();
    o.maxLines = 0;
    physicsDebugCollect(o, Vec3{0, 0, 0}, frame, kFake);
    check(g_fake.lastBudget == 1, "a zero budget is clamped to one");
}

static void testOff() {
    AVER_INFO("=== with nothing on ===");
    g_fake.reset();
    PhysicsDebugOptions off;
    PhysicsDebugFrame frame;
    frame.lines.resize(10);
    frame.labels.push_back({});
    frame.truncated = true;
    physicsDebugCollect(off, Vec3{1, 2, 3}, frame, kFake);
    check(g_fake.collectCalls == 0, "collect is not called");
    check(g_fake.armCalls == 1 && g_fake.lastArm == 0, "arm(0) is called, so a previously armed capture is switched off");
    check(g_fake.schemeCalls == 0, "the scheme is not set");
    check(frame.lines.empty() && frame.labels.empty() && !frame.truncated, "the frame is cleared");
}

int main() {
    testFlags();
    testCollect();
    testOff();
    AVER_INFO("=== PhysicsDebugOverlayTest: {} checks, {} failures ===", g_checks, g_failures);
    return g_failures;
}
