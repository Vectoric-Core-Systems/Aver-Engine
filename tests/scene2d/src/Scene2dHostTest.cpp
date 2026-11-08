// Scene2dHostTest -- GameScene2D.hpp, the helper both hosts (the editor's Play and the packaged game) call: an asset id becomes a UI
// image through the host's image table, the viewport rectangle reaches the canvas each frame, sprites and queued external draws leave
// as one sorted list, and a host that is not drawing the game UI drops the queue. A fake image host and a real UiDrawList; no device.
#include "aver/core/Log.hpp"
#include "aver/game/GameScene2D.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"
#include "aver/ui/ui_abi.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

// The fake image table: two pictures by content path.
struct Pic { uint64_t tex; int w, h; };
static std::map<std::string, Pic>& table() {
    static std::map<std::string, Pic> t = {{"Art/bg.png", {501, 200, 100}}, {"Art/dot.png", {502, 16, 16}}};
    return t;
}
static int g_useCalls = 0;
static uint64_t fakeUse(const char* path) {
    ++g_useCalls;
    const auto it = table().find(path ? path : "");
    return it == table().end() ? 0 : it->second.tex;
}
static uint64_t fakeAcquire(const char* path) { return fakeUse(path); }
static void fakeRelease(uint64_t) {}
static int32_t fakeSize(uint64_t id, int32_t* wh) {
    for (const auto& kv : table())
        if (kv.second.tex == id) { wh[0] = kv.second.w; wh[1] = kv.second.h; return 1; }
    return 0;
}

static const i64 kBg = 1001, kDot = 1002, kMissing = 1003, kUnindexed = 1004;
static std::string pathOf(u64 id) {
    switch (static_cast<i64>(id)) {
        case kBg: return "Art/bg.png";
        case kDot: return "Art/dot.png";
        case kMissing: return "Art/gone.png";   // indexed, but the image table cannot load it
        default: return {};                     // kUnindexed: the project does not hold it
    }
}

static Entity sprite(World& w, const char* name, i64 tex, f32 x, f32 y, i32 z = 0) {
    const Entity e = w.create(name);
    scene2d::attachTransform(w, e);
    scene2d::attachSprite(w, e, tex);
    auto* t = w.component<scene2d::C2DTransform>(e, scene2d::ids().transform);
    t->posX = x; t->posY = y; t->zIndex = z;
    return e;
}

static void clearWorld(World& w) {
    for (u32 i = w.count(); i-- > 0;) w.destroy(w.at(i));
    w.flush();
}

struct Ext { f32 x; u64 tag; };
static void drawExt(void* list, void* user) {
    auto* l = static_cast<ui::UiDrawList*>(list);
    const Ext* e = static_cast<const Ext*>(user);
    l->addTexturedRect(e->x, 0, 10, 10, e->tag, 0, 0, 1, 1, 0xFFFFFFFF);
}

static std::vector<u64> textures(const ui::UiDrawList& l) {
    std::vector<u64> t;
    for (const auto& c : l.commands(ui::UiLayer::Content)) t.push_back(c.texture);
    return t;
}

int main() {
    World& w = World::instance();
    scene2d::registerComponents(w);
    aver_ui_set_image_host(&fakeAcquire, &fakeRelease, &fakeSize, &fakeUse);

    check(game::scene2dRelativePath("C:/p/Content/Art/a.png", "C:/p/Content") == "Art/a.png", "an absolute path under Content becomes content-relative");
    check(game::scene2dRelativePath("", "C:/p/Content").empty(), "an empty path stays empty");

    game::scene2dBind(&pathOf);
    scene2d::setCanvasView({}, {});

    // ---- an asset id reaches the draw list as its UI image, at the viewport's offset ----
    {
        clearWorld(w);
        sprite(w, "bg", kBg, 100, 50);   // 200 x 100 image, centered
        ui::UiDrawList l;
        const float vp[4] = {40, 30, 1280, 720};
        check(game::scene2dFrame(w, l, vp) == 1, "the sprite queues");
        check(textures(l) == std::vector<u64>{501}, "its asset id became the image table's texture");
        const auto& c = l.commands(ui::UiLayer::Content)[0];
        f32 x0 = 1e9f, y0 = 1e9f;
        for (u32 i = 0; i < c.indexCount; ++i) {
            const ui::UiVertex& v = l.vertices()[l.indices()[c.indexOffset + i]];
            x0 = std::min(x0, v.x); y0 = std::min(y0, v.y);
        }
        check(approx(x0, 40) && approx(y0, 30), "centered on its position and moved by the viewport's origin");
        const auto ct = scene2d::canvasTransform();
        check(ct.valid && approx(ct.toWindow.tx, 40) && approx(ct.toWindow.ty, 30), "the canvas has the real viewport");
    }

    // ---- the viewport moves each frame and the stretch rules stay ----
    {
        ui::UiStretchSettings s;
        s.mode = ui::UiStretchMode::CanvasItems;
        s.aspect = ui::UiStretchAspect::Keep;
        s.baseW = 640; s.baseH = 360;
        scene2d::setCanvasView(s, {0, 0, 640, 360});   // what a script that owns the rules does
        ui::UiDrawList l;
        const float vp[4] = {0, 0, 1280, 720};
        game::scene2dFrame(w, l, vp);
        auto ct = scene2d::canvasTransform();
        check(approx(ct.toWindow.a, 2) && approx(ct.toWindow.d, 2), "the script's stretch rules scale to the new viewport (1280x720 over 640x360)");
        ui::UiDrawList l2;
        const float wide[4] = {0, 0, 2560, 720};
        game::scene2dFrame(w, l2, wide);
        ct = scene2d::canvasTransform();
        check(approx(ct.toWindow.a, 2) && approx(ct.toWindow.tx, 640), "a wider window letterboxes: the view origin plus the stretch offset");
        scene2d::setCanvasView({}, {});
    }

    // ---- sprites and external items leave as one list, in the shared order ----
    {
        clearWorld(w);
        sprite(w, "bg", kBg, 0, 0, 0);
        sprite(w, "dot", kDot, 0, 0, 5);
        Ext under{0, 900}, between{0, 901}, over{0, 902};
        scene2d::submitExternal({0, -1, 0}, &drawExt, &under);
        scene2d::submitExternal({0, 2, 0}, &drawExt, &between);
        scene2d::submitExternal({0, 9, 0}, &drawExt, &over);
        ui::UiDrawList l;
        const float vp[4] = {0, 0, 800, 600};
        game::scene2dFrame(w, l, vp);
        check(textures(l) == std::vector<u64>({900, 501, 901, 502, 902}), "an external draw sits under, between and over the sprites by z");
        check(scene2d::pendingCount() == 0, "the frame emptied the queue");
    }

    // ---- sprites that cannot resolve are skipped, and the lookup is cached ----
    {
        clearWorld(w);
        sprite(w, "gone", kMissing, 0, 0);
        sprite(w, "unindexed", kUnindexed, 0, 0);
        sprite(w, "dot", kDot, 0, 0);
        ui::UiDrawList l;
        const float vp[4] = {0, 0, 800, 600};
        check(game::scene2dFrame(w, l, vp) == 1 && textures(l) == std::vector<u64>{502}, "an image the table cannot load, and an id the project does not hold, draw nothing");
        g_useCalls = 0;
        ui::UiDrawList l2;
        game::scene2dFrame(w, l2, vp);
        check(g_useCalls == 2, "the image table is asked once per indexed path per frame; an unindexed id costs no call");
    }

    // ---- the frame steps time: an animation advances by dt, and the camera moves the sprites ----
    {
        clearWorld(w);
        const scene::Entity e = sprite(w, "anim", kBg, 0, 0);
        scene2d::CSpriteAnim2D* an = scene2d::attachAnim(w, e);
        scene2d::AnimFrame2D fr[2] = {};
        fr[1].cell = 3;
        scene2d::setClip(*an, 0, "a", 10, true, fr, 2);
        scene2d::playClip(w, e, "a");
        ui::UiDrawList l;
        const float vp[4] = {0, 0, 800, 600};
        game::scene2dFrame(w, l, vp, 0.15f);
        check(an->frame == 1 && w.component<scene2d::CSprite2D>(e, scene2d::ids().sprite)->frame == 3, "scene2dFrame(dt) advances the animation");
        game::scene2dFrame(w, l, vp);
        check(an->frame == 1, "and a zero dt advances nothing");
        const scene::Entity cam = w.create("cam");
        scene2d::attachTransform(w, cam)->posX = 500;
        scene2d::attachCamera(w, cam);
        scene2d::setCanvasView({}, {});
        ui::UiDrawList l2;
        game::scene2dFrame(w, l2, vp, 0.016f);
        const auto& c = l2.commands(ui::UiLayer::Content)[0];
        f32 x0 = 1e9f;
        for (u32 i = 0; i < c.indexCount; ++i) x0 = std::min(x0, l2.vertices()[l2.indices()[c.indexOffset + i]].x);
        check(approx(x0, -200), "a camera 100 right of the middle moves the 200 wide sprite 100 left of where it would be");
    }

    // ---- a host that is not drawing the game UI drops the queue; unbinding stops sprites ----
    {
        Ext e{0, 900};
        scene2d::submitExternal({0, 0, 0}, &drawExt, &e);
        game::scene2dDrop();
        check(scene2d::pendingCount() == 0, "scene2dDrop empties the queue");
        game::scene2dUnbind();
        ui::UiDrawList l;
        const float vp[4] = {0, 0, 800, 600};
        check(game::scene2dFrame(w, l, vp) == 0 && l.commands(ui::UiLayer::Content).empty(), "after unbinding no sprite draws");
    }

    AVER_INFO("Scene2dHostTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
