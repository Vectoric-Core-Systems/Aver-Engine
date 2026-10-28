// The sub-viewport registry (aver/core/SubViewport.hpp, G4 R5): creation and the budget, size scaling and the pixel cap, the update modes and
// their scheduling (Once, WhenVisible, WhenParentVisible, Always, Disabled, a pending request), the per-frame render budget and its fairness, the
// clear modes, consumers and the colour encoding they choose, and the texture-handle bookkeeping. Header-only, no device. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/core/SubViewport.hpp"

#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

using Reg = SubViewRegistry;

static SubViewDesc camDesc(ViewUpdate u, u32 w = 256, u32 h = 256) {
    SubViewDesc d;
    d.update = u;
    d.width = w;
    d.height = h;
    d.cameraEntity = 7;
    return d;
}

static std::vector<u32> planIds(Reg& r, u64 frame, std::vector<bool>* clears = nullptr) {
    Reg::Job jobs[kSubViewAbsoluteMax];
    const u32 n = r.plan(frame, jobs, kSubViewAbsoluteMax);
    std::vector<u32> out;
    for (u32 i = 0; i < n; ++i) {
        out.push_back(jobs[i].id);
        if (clears) clears->push_back(jobs[i].clear);
    }
    return out;
}

static void testNumbers() {
    AVER_INFO("=== Godot's numbers ===");
    check(static_cast<int>(ViewUpdate::Disabled) == 0 && static_cast<int>(ViewUpdate::Once) == 1 && static_cast<int>(ViewUpdate::WhenVisible) == 2 &&
              static_cast<int>(ViewUpdate::WhenParentVisible) == 3 && static_cast<int>(ViewUpdate::Always) == 4,
          "update modes are Godot's UPDATE_* values");
    check(static_cast<int>(ViewClear::Always) == 0 && static_cast<int>(ViewClear::Never) == 1 && static_cast<int>(ViewClear::Once) == 2,
          "clear modes are Godot's CLEAR_MODE_* values");
    check(viewUpdateFromInt(9) == ViewUpdate::WhenVisible && viewUpdateFromInt(-1) == ViewUpdate::WhenVisible, "a bad update mode reads as Godot's default");
    check(viewClearFromInt(7) == ViewClear::Always, "a bad clear mode reads as Always");
    SubViewDesc d;
    check(d.update == ViewUpdate::WhenVisible && d.clear == ViewClear::Always && !d.transparent && d.cameraEntity == 0 && d.parentVisible,
          "a default description is Godot's: when visible, clear always, opaque, no camera");
}

static void testCreateAndBudget() {
    AVER_INFO("=== create, keys and the view budget ===");
    Reg r;
    check(r.budget().maxViews == 4 && r.budget().rendersPerFrame == 2, "the default budget is 4 views, 2 renders a frame");
    check(r.create("", SubViewDesc{}) == 0 && r.refused() == 1, "an empty key is refused and counted");
    check(r.create(std::string(kSubViewKeyMax + 1, 'k'), SubViewDesc{}) == 0, "a key longer than the limit is refused");
    const u32 a = r.create("Main/A", SubViewDesc{});
    check(a == 1, "the first view gets id 1");
    check(r.create("Main/A", SubViewDesc{}) == a && r.count() == 1, "the same key returns the same view");
    const u32 b = r.create("Main/B", SubViewDesc{});
    const u32 c = r.create("Main/C", SubViewDesc{});
    const u32 d = r.create("Main/D", SubViewDesc{});
    check(b == 2 && c == 3 && d == 4 && r.count() == 4, "four views fit");
    const u64 before = r.refused();
    check(r.create("Main/E", SubViewDesc{}) == 0 && r.refused() == before + 1 && r.count() == 4, "a fifth is refused: the cap is the cost guard");
    check(r.create("Main/A", SubViewDesc{}) == a, "an existing key is still found when full");
    check(r.destroy(b) && !r.destroy(b) && r.count() == 3, "destroy removes once");
    const u32 e = r.create("Main/E", SubViewDesc{});
    check(e == 5, "ids are never reused: a stale id cannot name a new view");
    check(r.get(b) == nullptr && r.get(e) != nullptr, "a destroyed id resolves to nothing");

    Reg off(SubViewBudget{0, 2, 1u << 20});
    check(off.create("X", SubViewDesc{}) == 0 && off.count() == 0, "a budget of zero views turns the feature off");

    Reg wide;
    SubViewBudget big;
    big.maxViews = 99;
    wide.setBudget(big);
    check(wide.budget().maxViews == kSubViewAbsoluteMax, "the view budget is clamped to the table size");
    big.maxViews = 4;
    big.rendersPerFrame = 0;
    wide.setBudget(big);
    check(wide.budget().rendersPerFrame == 1, "at least one render a frame");
}

static void testSizes() {
    AVER_INFO("=== sizes: scale, clamp, pixel cap ===");
    SubViewDesc d;
    d.width = 640; d.height = 360; d.scale3d = 0.5f;
    u32 w = 0, h = 0;
    Reg::scaledSize(d, w, h);
    check(w == 320 && h == 180, "scaling_3d_scale multiplies the size");
    d.width = 2; d.height = 100000; d.scale3d = 1.0f;
    Reg::scaledSize(d, w, h);
    check(w == kSubViewMinSide && h == kSubViewMaxSide, "sides are clamped to 16..4096");
    d.scale3d = -3.0f;
    d.width = 100; d.height = 100;
    Reg::scaledSize(d, w, h);
    check(w == 100 && h == 100, "a bad scale reads as 1");

    SubViewBudget b;
    b.maxTotalPixels = 1000u * 1000u;
    Reg r(b);
    SubViewDesc big;
    big.width = 1920; big.height = 1080;
    const u32 x = r.create("A", big);
    const u32 y = r.create("B", big);
    check(r.get(x)->effW * r.get(x)->effH <= 1000u * 1000u / 2 + 4000, "two 1080p views are shrunk to share the pixel cap");
    check(r.totalPixels() <= b.maxTotalPixels, "the summed pixels fit the cap");
    const f64 aspect = static_cast<f64>(r.get(x)->effW) / r.get(x)->effH;
    check(aspect > 1.7 && aspect < 1.85, "shrinking keeps the aspect ratio");
    check(r.get(y)->effW == r.get(x)->effW, "equal views shrink equally");
    r.destroy(y);
    check(r.get(x)->effW > 1000, "a lone view is shrunk less once the other is gone");
    check(r.get(x)->effW * static_cast<u64>(r.get(x)->effH) <= b.maxTotalPixels, "and still fits the cap");

    Reg tiny(SubViewBudget{4, 2, 64 * 64});
    SubViewDesc s;
    s.width = 16; s.height = 16;
    for (int i = 0; i < 4; ++i) tiny.create("T" + std::to_string(i), s);
    check(tiny.get(1)->effW >= kSubViewMinSide, "no view goes below the minimum side");
}

static void testUpdateModes() {
    AVER_INFO("=== update modes ===");
    {
        Reg r;
        const u32 id = r.create("always", camDesc(ViewUpdate::Always));
        check(planIds(r, 1) == std::vector<u32>{id} && planIds(r, 2) == std::vector<u32>{id}, "Always draws every frame");
        check(r.get(id)->renderCount == 2 && r.get(id)->lastRenderFrame == 2, "and the render count and frame are stamped");
    }
    {
        Reg r;
        const u32 id = r.create("off", camDesc(ViewUpdate::Disabled));
        check(planIds(r, 1).empty(), "Disabled never draws");
        r.get(id)->pendingRender = true;
        check(planIds(r, 2) == std::vector<u32>{id} && !r.get(id)->pendingRender, "a requested render draws once even when Disabled");
        check(planIds(r, 3).empty(), "and the request is consumed");
    }
    {
        Reg r;
        const u32 id = r.create("once", camDesc(ViewUpdate::Once));
        check(planIds(r, 1) == std::vector<u32>{id}, "Once draws on the first frame");
        check(r.get(id)->desc.update == ViewUpdate::Disabled, "and then reads as Disabled, as Godot's UPDATE_ONCE does");
        check(planIds(r, 2).empty() && planIds(r, 3).empty(), "it does not draw again");
        r.get(id)->desc.update = ViewUpdate::Once;
        r.get(id)->pendingRender = true;
        check(planIds(r, 4) == std::vector<u32>{id}, "setting Once again (which also requests a render) draws once more");
    }
    {
        Reg r;
        SubViewDesc d = camDesc(ViewUpdate::Once);
        d.cameraEntity = 0;
        const u32 id = r.create("nocam", d);
        check(planIds(r, 1).empty() && r.get(id)->desc.update == ViewUpdate::Once, "a view with no camera is never due, and a Once view keeps its one render for later");
        r.get(id)->desc.cameraEntity = 9;
        check(planIds(r, 2) == std::vector<u32>{id}, "it draws as soon as it has a camera");
    }
    {
        Reg r;
        const u32 id = r.create("vis", camDesc(ViewUpdate::WhenVisible));
        check(planIds(r, 1).empty(), "WhenVisible draws nothing while nothing shows it");
        check(r.consumer(id, 0, +1), "a material starts using the texture");
        check(planIds(r, 2) == std::vector<u32>{id}, "now it draws");
        check(r.consumer(id, 0, -1) && r.consumer(id, 0, -1) && r.get(id)->materialConsumers == 0, "consumers never go below zero");
        check(planIds(r, 3).empty(), "and stops when the last consumer goes");
        check(!r.consumer(id, 5, +1) && !r.consumer(99, 0, +1), "a bad kind or id is refused");
        r.get(id)->onScreen = true;
        check(planIds(r, 4) == std::vector<u32>{id}, "a view on screen counts as visible");
    }
    {
        Reg r;
        SubViewDesc d = camDesc(ViewUpdate::WhenParentVisible);
        const u32 id = r.create("par", d);
        check(planIds(r, 1) == std::vector<u32>{id}, "WhenParentVisible draws while the parent is visible");
        r.get(id)->desc.parentVisible = false;
        check(planIds(r, 2).empty(), "and not once it is hidden");
    }
}

static void testBudgetAndFairness() {
    AVER_INFO("=== the per-frame render budget ===");
    Reg r;
    std::vector<u32> ids;
    for (int i = 0; i < 4; ++i) ids.push_back(r.create("v" + std::to_string(i), camDesc(ViewUpdate::Always)));
    // Frame 1: every view has never been drawn; two go, by id.
    check(planIds(r, 1) == (std::vector<u32>{ids[0], ids[1]}), "frame 1 draws two views, lowest ids first");
    // Frame 2: the never-drawn ones go before the stale ones.
    check(planIds(r, 2) == (std::vector<u32>{ids[2], ids[3]}), "frame 2 draws the two that never had a picture");
    // Frame 3: all drawn once; the least recently drawn go (frame 1's pair).
    check(planIds(r, 3) == (std::vector<u32>{ids[0], ids[1]}), "afterwards the least recently drawn go first (round robin)");
    check(planIds(r, 4) == (std::vector<u32>{ids[2], ids[3]}), "and the other pair next");
    check(r.deferred() >= 6, "postponed renders are counted");
    Reg::Job one[1];
    check(r.plan(5, one, 1) == 1, "the caller's array also caps the plan");
    check(r.plan(6, one, 0) == 0, "a zero-sized array plans nothing");

    // A Once view and a requested render are waiting for their first picture: they go before stale always-views.
    Reg q;
    const u32 a1 = q.create("a1", camDesc(ViewUpdate::Always));
    const u32 a2 = q.create("a2", camDesc(ViewUpdate::Always));
    planIds(q, 1);   // both drawn
    const u32 o = q.create("once", camDesc(ViewUpdate::Once));
    const std::vector<u32> p = planIds(q, 2);
    check(p.size() == 2 && p[0] == o, "a view waiting for its first picture goes first");
    (void)a1; (void)a2;
}

static void testClearModes() {
    AVER_INFO("=== clear modes ===");
    for (ViewClear mode : {ViewClear::Always, ViewClear::Never, ViewClear::Once}) {
        Reg r;
        SubViewDesc d = camDesc(ViewUpdate::Always);
        d.clear = mode;
        r.create("c", d);
        std::vector<bool> c1, c2;
        planIds(r, 1, &c1);
        planIds(r, 2, &c2);
        check(c1.size() == 1 && c1[0], "the first picture always clears, whatever the mode");
        check(c2.size() == 1 && c2[0] == (mode == ViewClear::Always), "later pictures clear only for the Always mode");
    }
}

static void testEncodingAndTexture() {
    AVER_INFO("=== colour encoding and the texture handle ===");
    Reg r;
    const u32 id = r.create("t", camDesc(ViewUpdate::Always));
    Reg::Entry* e = r.get(id);
    check(!Reg::linearOutput(*e), "a view nothing reads stores display values");
    r.consumer(id, 0, +1);
    check(Reg::linearOutput(*e), "a view only a material reads stores linear values");
    r.consumer(id, 1, +1);
    check(!Reg::linearOutput(*e), "one the UI also shows stores display values");
    r.consumer(id, 1, -1);
    e->onScreen = true;
    check(!Reg::linearOutput(*e), "so does one drawn over the window");

    check(e->texture == 0 && e->generation == 0, "no texture before the host draws");
    check(r.setTexture(id, 0x55, 256, 256) && e->generation == 1 && e->texture == 0x55, "the host's texture is recorded and bumps the generation");
    check(r.setTexture(id, 0x55, 256, 256) && e->generation == 1, "the same texture does not");
    check(r.setTexture(id, 0x56, 256, 256) && e->generation == 2, "a rebuilt target does");
    check(r.setTexture(id, 0x56, 512, 256) && e->generation == 3, "and so does a new size");
    check(!r.setTexture(999, 1, 1, 1), "an unknown id is refused");

    check(Reg::runtimeTextureName("Main/Screen") == "viewport.Main/Screen", "the runtime texture name");
    check(Reg::runtimeTexturePath("Main/Screen") == "@rt:viewport.Main/Screen", "the texture reference a converted material carries");
}

static void testUpdateKeepsScheduling() {
    AVER_INFO("=== update keeps the scheduling state ===");
    Reg r;
    const u32 id = r.create("u", camDesc(ViewUpdate::Always));
    planIds(r, 1);
    r.consumer(id, 0, +1);
    r.setTexture(id, 9, 64, 64);
    SubViewDesc d = r.get(id)->desc;
    d.width = 128;
    check(r.update(id, d) && r.get(id)->renderCount == 1 && r.get(id)->materialConsumers == 1 && r.get(id)->texture == 9, "a new description keeps counts, consumers and texture");
    check(r.get(id)->effW == 128, "and the effective size follows it");
    check(!r.update(77, d), "an unknown id is refused");
    SubViewDesc bad;
    bad.scale3d = std::nanf("");
    bad.clearColor[2] = std::nanf("");
    r.update(id, bad);
    check(r.get(id)->desc.scale3d == 1.0f && r.get(id)->desc.clearColor[2] == 0.0f, "non-finite values are replaced");
    r.clear();
    check(r.count() == 0 && r.create("again", SubViewDesc{}) > id, "clear() ends a session without reusing ids");
}

int main() {
    AVER_INFO("SubViewportTest");
    testNumbers();
    testCreateAndBudget();
    testSizes();
    testUpdateModes();
    testBudgetAndFairness();
    testClearModes();
    testEncodingAndTexture();
    testUpdateKeepsScheduling();
    if (g_failures) {
        AVER_ERROR("SubViewportTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("SubViewportTest: ok");
    return 0;
}
