// AI debug feed: category gating, shape geometry and providers. Pure, header-only.
#include "aver/synapse/AiDebug.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::synapse;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static int g_providerCalls = 0;
static void provider(AiDebugSink& sink, void* user) {
    ++g_providerCalls;
    sink.line(Vec3{0, 0, 0}, Vec3{1, 0, 0}, 0xFF0000FFu, *static_cast<u32*>(user));
}

int main() {
    AVER_INFO("AiDebugTest");

    AiDebugSink s;
    check(!s.wants(kAiDebugSight), "nothing is wanted until enabled");
    s.line(Vec3{}, Vec3{1, 1, 1}, 0xFFFFFFFFu, kAiDebugSight);
    check(s.lines().empty(), "a disabled category records nothing");

    s.setEnabled(kAiDebugSight | kAiDebugPath);
    s.line(Vec3{}, Vec3{1, 1, 1}, 0xFFFFFFFFu, kAiDebugSight);
    s.line(Vec3{}, Vec3{1, 1, 1}, 0xFFFFFFFFu, kAiDebugHearing);
    check(s.lines().size() == 1, "only enabled categories are recorded");

    s.clear();
    s.circle(Vec3{10, 20, 5}, 100.0f, 0xFFFFFFFFu, kAiDebugSight, 32);
    check(s.lines().size() == 32, "a circle with 32 segments is 32 lines");
    bool onRing = true;
    for (const AiDebugLine& l : s.lines()) {
        const Vec3 d = l.a - Vec3{10, 20, 5};
        if (std::fabs(d.size() - 100.0f) > 0.01f || std::fabs(l.a.z - 5.0f) > 1e-4f) onRing = false;
    }
    check(onRing, "every vertex lies on the horizontal ring at the radius");

    s.clear();
    s.sphere(Vec3{}, 50.0f, 0xFFFFFFFFu, kAiDebugSight, 16);
    check(s.lines().size() == 48, "a sphere is three rings");

    s.clear();
    const Vec3 apex{0, 0, 160};
    s.cone(apex, Vec3{1, 0, 0}, 3000.0f, 0.7853982f, 0xFFFFFFFFu, kAiDebugSight, 24);
    check(s.lines().size() == 28, "a cone is a 24-segment rim plus four edges");
    bool atRange = true;
    for (usize i = 24; i < s.lines().size(); ++i)
        if (std::fabs((s.lines()[i].b - apex).size() - 3000.0f) > 0.5f) atRange = false;
    check(atRange, "the four edges end on the rim at exactly the range");
    check(std::fabs((s.lines()[0].a - apex).x - 3000.0f * std::cos(0.7853982f)) < 0.5f, "the rim sits at range * cos(half angle) along the axis");

    s.clear();
    s.cone(apex, Vec3{0, 0, 1}, 100.0f, 0.5f, 0xFFFFFFFFu, kAiDebugSight);
    check(s.lines().size() == 28, "a cone pointing straight up still builds a basis");
    s.clear();
    s.cone(apex, Vec3{0, 0, 0}, 100.0f, 0.5f, 0xFFFFFFFFu, kAiDebugSight);
    check(s.lines().empty(), "a zero direction draws nothing");

    s.clear();
    s.arrow(Vec3{0, 0, 0}, Vec3{100, 0, 0}, 0xFFFFFFFFu, kAiDebugPath);
    check(s.lines().size() == 3, "an arrow is a shaft and two barbs");
    s.clear();
    s.arrow(Vec3{0, 0, 0}, Vec3{0, 0, 0}, 0xFFFFFFFFu, kAiDebugPath);
    check(s.lines().size() == 1, "a zero-length arrow does not produce NaN barbs");

    s.clear();
    s.polyline({Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}}, 0xFFFFFFFFu, kAiDebugPath);
    check(s.lines().size() == 2, "a polyline of three points is two segments");
    s.text(Vec3{0, 0, 200}, "Chase", 0xFFFFFFFFu, kAiDebugBtState);
    check(s.texts().empty(), "text in a disabled category is dropped");
    s.setEnabled(kAiDebugAll);
    s.text(Vec3{0, 0, 200}, "Chase", 0xFFFFFFFFu, kAiDebugBtState);
    check(s.texts().size() == 1 && s.texts()[0].text == "Chase", "text in an enabled category is kept");

    s.clear();
    u32 cat = kAiDebugHearing;
    s.addProvider(&provider, &cat);
    s.runProviders();
    s.runProviders();
    check(g_providerCalls == 2 && s.lines().size() == 2, "a provider runs on every call and feeds the sink");
    s.removeProvider(&provider);
    s.runProviders();
    check(g_providerCalls == 2, "a removed provider no longer runs");

    check(aiRgba(255, 0, 0) == 0xFF0000FFu, "aiRgba packs 0xRRGGBBAA");

    AVER_INFO(g_failures ? "AiDebugTest: {} FAILURES" : "AiDebugTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
