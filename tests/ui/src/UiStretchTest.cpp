// Headless test for the stretch / letterbox arithmetic (G5 U1): Godot's display/window/stretch rules for every mode and aspect.
#include "aver/core/Log.hpp"
#include "aver/ui/UiStretch.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static UiStretchSettings cfg(UiStretchMode m, UiStretchAspect a, f32 bw, f32 bh, f32 scale = 1.0f, UiStretchScaleMode sm = UiStretchScaleMode::Fractional) {
    UiStretchSettings s;
    s.mode = m; s.aspect = a; s.baseW = bw; s.baseH = bh; s.scale = scale; s.scaleMode = sm;
    return s;
}

int main() {
    using M = UiStretchMode;
    using A = UiStretchAspect;

    AVER_INFO("=== disabled ===");
    {
        const auto r = uiComputeStretch(cfg(M::Disabled, A::Keep, 1280, 720), 1920, 1080);
        check(near(r.contentSize.x, 1920) && near(r.contentSize.y, 1080) && near(r.scaleX, 1) && !r.letterboxed, "the content is the window");
        check(near(r.screen.w, 1920) && near(r.screen.x, 0), "the whole window");
        const auto s2 = uiComputeStretch(cfg(M::Disabled, A::Keep, 1280, 720, 2.0f), 1920, 1080);
        check(near(s2.contentSize.x, 960) && near(s2.contentSize.y, 540) && near(s2.scaleX, 2), "the scale factor shrinks the content and enlarges its pixels");
        const auto nb = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 0, 0), 800, 600);
        check(near(nb.contentSize.x, 800) && near(nb.scaleX, 1), "no base size behaves as disabled");
    }

    AVER_INFO("=== canvas_items ===");
    {
        auto r = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 1280, 720), 1920, 1080);
        check(near(r.contentSize.x, 1280) && near(r.contentSize.y, 720) && near(r.scaleX, 1.5f) && near(r.scaleY, 1.5f) && !r.letterboxed, "same aspect: scaled, no bars");
        r = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 1280, 720), 1920, 1200);
        check(r.letterboxed && near(r.screen.x, 0) && near(r.screen.y, 60) && near(r.screen.w, 1920) && near(r.screen.h, 1080), "taller window, keep: bars above and below");
        check(near(r.contentSize.x, 1280) && near(r.scaleX, 1.5f), "the content keeps its size");
        r = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 1280, 720), 2560, 1080);
        check(r.letterboxed && near(r.screen.x, 320) && near(r.screen.y, 0) && near(r.screen.w, 1920), "wider window, keep: bars at the sides");
        r = uiComputeStretch(cfg(M::CanvasItems, A::Expand, 1280, 720), 2560, 1080);
        check(!r.letterboxed && near(r.contentSize.x, 1706) && near(r.contentSize.y, 720) && near(r.screen.w, 2560), "wider window, expand: the visible area grows in width");
        r = uiComputeStretch(cfg(M::CanvasItems, A::Expand, 1280, 720), 1920, 1200);
        check(!r.letterboxed && near(r.contentSize.x, 1280) && near(r.contentSize.y, 800), "taller window, expand: grows in height");
        r = uiComputeStretch(cfg(M::CanvasItems, A::KeepHeight, 1280, 720), 2560, 1080);
        check(!r.letterboxed && near(r.contentSize.y, 720) && near(r.contentSize.x, 1706), "keep_height, wider window: height kept, width grows");
        r = uiComputeStretch(cfg(M::CanvasItems, A::KeepHeight, 1280, 720), 1920, 1200);
        check(r.letterboxed && near(r.screen.y, 60), "keep_height, taller window: bars");
        r = uiComputeStretch(cfg(M::CanvasItems, A::KeepWidth, 1280, 720), 1920, 1200);
        check(!r.letterboxed && near(r.contentSize.x, 1280) && near(r.contentSize.y, 800), "keep_width, taller window: width kept, height grows");
        r = uiComputeStretch(cfg(M::CanvasItems, A::KeepWidth, 1280, 720), 2560, 1080);
        check(r.letterboxed && near(r.screen.x, 320), "keep_width, wider window: bars");
        r = uiComputeStretch(cfg(M::CanvasItems, A::Ignore, 1280, 720), 1920, 1200);
        check(!r.letterboxed && near(r.scaleX, 1.5f) && near(r.scaleY, 1200.0f / 720.0f) && near(r.contentSize.y, 720), "ignore: each axis stretches on its own");
        r = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 1280, 720, 2.0f), 1920, 1080);
        check(near(r.contentSize.x, 640) && near(r.contentSize.y, 360) && near(r.scaleX, 3), "the scale factor divides the content size");
    }

    AVER_INFO("=== viewport ===");
    {
        auto r = uiComputeStretch(cfg(M::Viewport, A::Keep, 320, 180), 1280, 720);
        check(near(r.contentSize.x, 320) && near(r.renderSize.x, 320) && near(r.renderSize.y, 180) && near(r.scaleX, 4), "rendered at the base size, scaled up 4x");
        r = uiComputeStretch(cfg(M::Viewport, A::Keep, 320, 180), 1280, 800);
        check(r.letterboxed && near(r.screen.y, 40) && near(r.screen.h, 720), "bars when the shape differs");
        r = uiComputeStretch(cfg(M::Viewport, A::Expand, 320, 180), 1280, 800);
        check(near(r.contentSize.x, 320) && near(r.contentSize.y, 200) && !r.letterboxed, "expand grows the rendered size");
        r = uiComputeStretch(cfg(M::Viewport, A::Keep, 320, 180, 2.0f), 1280, 720);
        check(near(r.contentSize.x, 160) && near(r.contentSize.y, 90) && near(r.scaleX, 8), "the scale factor halves the rendered size");
    }

    AVER_INFO("=== integer scaling ===");
    {
        const auto r = uiComputeStretch(cfg(M::Viewport, A::Keep, 320, 180, 1.0f, UiStretchScaleMode::Integer), 1000, 600);
        check(near(r.screen.w, 960) && near(r.screen.h, 540) && near(r.scaleX, 3), "a whole multiple of the base size");
        check(near(r.screen.x, 20) && near(r.screen.y, 30) && r.letterboxed, "centred with bars");
        const auto small = uiComputeStretch(cfg(M::Viewport, A::Keep, 320, 180, 1.0f, UiStretchScaleMode::Integer), 200, 100);
        check(small.screen.w > 0 && small.screen.h > 0, "a window smaller than the base still gets a picture");
    }

    AVER_INFO("=== pointer mapping ===");
    {
        const auto r = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 1280, 720), 2560, 1080);
        const UiVec2 c = uiStretchWindowToContent(r, 320 + 960, 540);
        check(near(c.x, 640) && near(c.y, 360), "the centre of the window is the centre of the content");
        const UiVec2 corner = uiStretchWindowToContent(r, 320, 0);
        check(near(corner.x, 0) && near(corner.y, 0), "the picture's corner is the content's origin");
        const UiVec2 bar = uiStretchWindowToContent(r, 10, 10);
        check(bar.x < 0, "a point in the bar is outside the content");
        const UiVec2 back = uiStretchContentToWindow(r, c.x, c.y);
        check(near(back.x, 1280) && near(back.y, 540), "and back");
        const auto zero = uiComputeStretch(cfg(M::CanvasItems, A::Keep, 1280, 720), 0, 0);
        check(zero.contentSize.x == 0 && zero.screen.w == 0, "a zero window gives a zero result");
    }

    if (g_failures == 0) AVER_INFO("UiStretchTest: all checks passed");
    else AVER_ERROR("UiStretchTest: {} check(s) failed", g_failures);
    return g_failures == 0 ? 0 : 1;
}
