// Scene2dStretchTest -- the engine-owned canvas transform against uiComputeStretch (the rule Fabric's Control path reaches
// through aver_ui_stretch_compute), over every mode, aspect and scale mode on a spread of window shapes; and the canvas
// half of scene2d_abi.h (set/get the canvas, the texture resolver, external submit, flush and frame). No GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene2d_abi.h"
#include "aver/ui/ui_style_abi.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;
using namespace aver::scene2d;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static bool sameStretch(const ui::UiStretchResult& a, const ui::UiStretchResult& b) {
    return a.screen.x == b.screen.x && a.screen.y == b.screen.y && a.screen.w == b.screen.w && a.screen.h == b.screen.h &&
           a.contentSize.x == b.contentSize.x && a.contentSize.y == b.contentSize.y && a.renderSize.x == b.renderSize.x &&
           a.renderSize.y == b.renderSize.y && a.scaleX == b.scaleX && a.scaleY == b.scaleY && a.letterboxed == b.letterboxed;
}

// ---- C ABI fixtures ----
static bool g_resolverCalled = false;
static int32_t resolveC(int64_t id, uint64_t* tex, int32_t* wh, void*) {
    g_resolverCalled = true;
    if (id != 7) return 0;
    *tex = 777;
    wh[0] = 40;
    wh[1] = 20;
    return 1;
}
struct Mark { float x; uint64_t tag; };
static void drawMark(void* list, void* user) {
    const Mark* m = static_cast<const Mark*>(user);
    static_cast<ui::UiDrawList*>(list)->addTexturedRect(m->x, 0, 4, 4, m->tag, 0, 0, 1, 1, 0xFFFFFFFF);
}

int main() {
    // ---- parity with uiComputeStretch over the whole rule space ----
    struct Win { f32 w, h; };
    const Win windows[] = {{1920, 1080}, {1280, 720}, {1000, 1000}, {800, 1200}, {2560, 1080}, {640, 360}, {321, 199}, {3840, 2160}};
    const f32 bases[][2] = {{1280, 720}, {320, 180}, {1024, 768}, {0, 0}};
    const f32 scales[] = {1.0f, 1.5f, 2.0f};
    int combos = 0, bad = 0, badMap = 0;
    for (int m = 0; m <= 2; ++m)
        for (int asp = 0; asp <= 4; ++asp)
            for (int sm = 0; sm <= 1; ++sm)
                for (const auto& base : bases)
                    for (f32 sc : scales)
                        for (const Win& win : windows) {
                            ui::UiStretchSettings s;
                            s.mode = static_cast<ui::UiStretchMode>(m);
                            s.aspect = static_cast<ui::UiStretchAspect>(asp);
                            s.scaleMode = static_cast<ui::UiStretchScaleMode>(sm);
                            s.baseW = base[0]; s.baseH = base[1]; s.scale = sc;
                            const CanvasView view{12.0f, 34.0f, win.w, win.h};
                            setCanvasView(s, view);
                            const CanvasTransform c = canvasTransform();
                            const ui::UiStretchResult want = ui::uiComputeStretch(s, win.w, win.h);
                            ++combos;
                            if (!c.valid || !sameStretch(c.stretch, want)) { ++bad; continue; }
                            // The affine maps content to window exactly as the stretch's own helper does, shifted by the view's origin.
                            for (const ui::UiVec2 p : {ui::UiVec2{0, 0}, ui::UiVec2{37.5f, 11.25f}, want.contentSize}) {
                                const ui::UiVec2 ref = ui::uiStretchContentToWindow(want, p.x, p.y);
                                f32 x, y;
                                c.toWindow.apply(p.x, p.y, x, y);
                                if (!approx(x, ref.x + view.x, 1e-2f) || !approx(y, ref.y + view.y, 1e-2f)) ++badMap;
                                const ui::UiVec2 back = ui::uiStretchWindowToContent(want, x - view.x, y - view.y);
                                if (!approx(back.x, p.x, 1e-2f) || !approx(back.y, p.y, 1e-2f)) ++badMap;
                            }
                        }
    check(bad == 0, std::to_string(combos) + " settings x window shapes: the canvas stretch equals uiComputeStretch field for field");
    check(badMap == 0, "and its affine agrees with uiStretchContentToWindow / WindowToContent (view origin added)");

    // ---- known layouts ----
    {
        ui::UiStretchSettings s;
        s.mode = ui::UiStretchMode::CanvasItems;
        s.aspect = ui::UiStretchAspect::Keep;
        s.baseW = 1280; s.baseH = 720;
        setCanvasView(s, CanvasView{0, 0, 1000, 1000});
        CanvasTransform c = canvasTransform();
        check(c.stretch.letterboxed && approx(c.stretch.scaleX, 1000.0f / 1280.0f), "1280x720 kept in a square window is letterboxed at 0.78125");
        check(approx(c.toWindow.ty, (1000 - 562) / 2.0f, 1.0f) && approx(c.toWindow.tx, 0), "centred vertically: bars above and below");
        s.aspect = ui::UiStretchAspect::Expand;
        setCanvasView(s, CanvasView{0, 0, 1000, 1000});
        c = canvasTransform();
        check(!c.stretch.letterboxed && approx(c.toWindow.tx, 0) && approx(c.toWindow.ty, 0) && c.stretch.contentSize.y > 720.0f,
              "expand fills the window and grows the content instead of adding bars");
        setCanvasView({}, CanvasView{5, 6, 640, 360});
        c = canvasTransform();
        check(c.valid && c.toWindow.a == 1 && c.toWindow.d == 1 && c.toWindow.tx == 5 && c.toWindow.ty == 6,
              "no stretch: content units are pixels, placed at the view's origin");
        setCanvasView({}, CanvasView{});
        c = canvasTransform();
        check(!c.valid && c.toWindow.identity(), "a view with no size leaves the identity");
    }

    // ---- the C ABI: canvas ----
    {
        check(aver_scene2d_abi_version() == AVER_SCENE2D_ABI_VERSION, "the ABI version is what the header says");
        const float in6[6] = {1, 1, 0, 1280, 720, 1};
        aver_scene2d_set_canvas(in6, 10, 20, 1000, 1000);
        float out6[6] = {}, out11[11] = {}, ref11[11] = {};
        check(aver_scene2d_canvas_transform(out6, out11) == 1, "canvas_transform reports a valid view");
        aver_ui_stretch_compute(in6, 1000, 1000, ref11);
        check(std::memcmp(out11, ref11, sizeof out11) == 0, "its stretch block is byte-identical to aver_ui_stretch_compute (the Fabric Control path)");
        ui::UiStretchSettings s;
        s.mode = ui::UiStretchMode::CanvasItems; s.aspect = ui::UiStretchAspect::Keep; s.baseW = 1280; s.baseH = 720;
        const CanvasTransform c = canvasTransform();
        check(out6[0] == c.toWindow.a && out6[3] == c.toWindow.d && out6[4] == c.toWindow.tx && out6[5] == c.toWindow.ty && out6[1] == 0 && out6[2] == 0,
              "and the affine is the C++ one: {a, b, c, d, tx, ty}");
        check(approx(out6[4], 10 + ref11[0]) && approx(out6[5], 20 + ref11[1]), "origin = view + the stretch's screen offset");
        aver_scene2d_set_canvas(nullptr, 0, 0, 0, 0);
        check(aver_scene2d_canvas_transform(out6, nullptr) == 0 && out6[0] == 1 && out6[4] == 0, "a zero-size view clears it; out11 may be NULL");
        const float nan = std::nanf("");
        aver_scene2d_set_canvas(in6, nan, 0, 100, 100);
        check(aver_scene_last_error() < 0 && aver_scene2d_canvas_transform(out6, nullptr) == 0, "a non-finite view is refused");
    }

    // ---- the C ABI: resolver, external items, flush and frame ----
    {
        reset();
        World& w = World::instance();
        registerComponents(w);
        aver_scene2d_set_texture_resolver(&resolveC, nullptr);
        const Entity e = w.create("sprite");
        attachTransform(w, e);
        attachSprite(w, e, 7);
        w.component<C2DTransform>(e, ids().transform)->posX = 50;

        Mark before{0, 11}, after{0, 12};
        check(aver_scene2d_submit_external(-1, 0, 0, &drawMark, &before, 0) == 1, "submit_external queues");
        check(aver_scene2d_submit_external(1, 0, 0, &drawMark, &after, 0) == 1, "a second one queues");
        check(aver_scene2d_submit_external(0, 0, 0, nullptr, nullptr, 0) == 0 && aver_scene_last_error() < 0, "a NULL callback is refused with a reason");
        check(aver_scene2d_pending() == 2, "pending counts the queue");

        ui::UiDrawList l;
        check(aver_scene2d_frame(&l) == 1 && g_resolverCalled, "frame queues the scene's sprite through the C resolver");
        std::vector<u64> tex;
        for (const auto& c : l.commands(ui::UiLayer::Content)) tex.push_back(c.texture);
        check((tex == std::vector<u64>{11, 777, 12}), "and one sorted list comes out: layer -1 item, the sprite, layer 1 item");
        check(aver_scene2d_pending() == 0, "frame empties the queue");

        aver_scene2d_submit_external(0, 0, 0, &drawMark, &before, 0);
        aver_scene2d_flush(nullptr);
        check(aver_scene2d_pending() == 0, "flush(NULL) drops the queue without drawing");
        check(aver_scene2d_frame(nullptr) == 0 && aver_scene_last_error() < 0, "frame needs a draw list");

        aver_scene2d_set_texture_resolver(nullptr, nullptr);
        ui::UiDrawList none;
        check(aver_scene2d_frame(&none) == 0 && none.empty(), "with the resolver cleared no sprite is drawn");

        w.destroy(e);
        w.flush();
    }

    reset();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
