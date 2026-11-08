// Scene2dCameraTest -- the 2D camera (CCamera2D) and Parallax2D: the view transform (position, offset, zoom, rotation, anchor), limits,
// drag margins, smoothing, which layers follow, the current camera, parallax scale / offset / autoscroll / tiling / no-follow / limits,
// and the camera half of scene2d_abi.h. Against a real UiDrawList; no window, no GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene2d_abi.h"

#include <algorithm>
#include <cmath>
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

static bool approx(f32 a, f32 b, f32 tol = 0.05f) { return std::fabs(a - b) <= tol; }

static bool resolve(i64 id, TextureInfo* out, void*) {
    if (id == 1) { *out = {101, 64, 64}; return true; }
    return false;
}

struct Box { f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; f32 cx() const { return (x0 + x1) * 0.5f; } f32 cy() const { return (y0 + y1) * 0.5f; } f32 w() const { return x1 - x0; } };

// One box per drawn quad, in draw order.
static std::vector<Box> quads(const ui::UiDrawList& l) {
    std::vector<Box> out;
    for (const auto& c : l.commands(ui::UiLayer::Content)) {
        for (u32 i = 0; i + 5 < c.indexCount; i += 6) {
            Box b;
            for (u32 k = 0; k < 6; ++k) {
                const ui::UiVertex& v = l.vertices()[l.indices()[c.indexOffset + i + k]];
                b.x0 = std::min(b.x0, v.x); b.y0 = std::min(b.y0, v.y); b.x1 = std::max(b.x1, v.x); b.y1 = std::max(b.y1, v.y);
            }
            out.push_back(b);
        }
    }
    return out;
}

static std::vector<Box> draw(World& w) {
    ui::UiDrawList l;
    tick(w, 0.0f);   // settles the transforms and snaps a camera that has no easing
    submitSprites(w);
    flush(l);
    return quads(l);
}

static Ids g;

static void clearAll(World& w) {
    reset();
    setTextureResolver(&resolve, nullptr);
    setCanvasView(ui::UiStretchSettings{}, CanvasView{0, 0, 800, 600});
    for (u32 i = w.count(); i-- > 0;) w.destroy(w.at(i));
    w.flush();
    clearAnimEvents();
}

static Entity sprite(World& w, f32 x, f32 y, Entity parent = kInvalidEntity) {
    const Entity e = parent == kInvalidEntity ? w.create("s") : w.create("s", parent, Transform{});
    attachTransform(w, e)->posX = x;
    w.component<C2DTransform>(e, g.transform)->posY = y;
    attachSprite(w, e, 1);
    return e;
}

static Entity camera(World& w, f32 x, f32 y, u32 flags = 0) {
    const Entity e = w.create("cam");
    C2DTransform* t = attachTransform(w, e);
    t->posX = x; t->posY = y;
    attachCamera(w, e)->flags = flags;
    return e;
}

static C2DTransform* T(World& w, Entity e) { return w.component<C2DTransform>(e, g.transform); }
static CCamera2D* C(World& w, Entity e) { return w.component<CCamera2D>(e, g.camera); }

static f32 centerX(World& w, Entity cam) { f32 c[2]; cameraCenter(w, cam, 800, 600, c); return c[0]; }
static f32 centerY(World& w, Entity cam) { f32 c[2]; cameraCenter(w, cam, 800, 600, c); return c[1]; }

int main() {
    World& w = World::instance();
    g = registerComponents(w);
    check(g.camera != 0 && g.parallax != 0 && g.anim != 0, "the six components register");

    // ---- no camera, and the basic view ----
    {
        clearAll(w);
        sprite(w, 100, 50);
        auto q = draw(w);
        check(q.size() == 1 && approx(q[0].cx(), 100) && approx(q[0].cy(), 50) && approx(q[0].w(), 64), "no camera: the sprite is at its world position");
        check(currentCamera(w) == kInvalidEntity, "and there is no current camera");

        const Entity cam = camera(w, 400, 300);
        q = draw(w);
        check(approx(q[0].cx(), 100) && approx(q[0].cy(), 50), "a camera centred on the middle of an 800x600 view changes nothing");
        T(w, cam)->posX = 500;
        q = draw(w);
        check(approx(q[0].cx(), 0) && approx(q[0].cy(), 50), "moving the camera 100 right moves the sprite 100 left");
        check(currentCamera(w) == cam, "the only camera is current");

        T(w, cam)->posX = 400;
        C(w, cam)->zoomX = C(w, cam)->zoomY = 2.0f;
        q = draw(w);
        check(approx(q[0].cx(), -200) && approx(q[0].cy(), -200) && approx(q[0].w(), 128), "zoom 2 about the view centre: position 100 -> -200, size doubled");
        C(w, cam)->zoomX = C(w, cam)->zoomY = 0.0f;
        C(w, cam)->offsetX = 50;
        q = draw(w);
        check(approx(q[0].cx(), 50), "offset 50 moves the view centre right: the sprite goes 50 left");
        C(w, cam)->offsetX = 0;

        C(w, cam)->flags = kCamAnchorTopLeft;
        T(w, cam)->posX = 0; T(w, cam)->posY = 0;
        q = draw(w);
        check(approx(q[0].cx(), 100) && approx(q[0].cy(), 50), "top-left anchor at (0,0): the view's corner is the origin");
        T(w, cam)->posX = 100;
        q = draw(w);
        check(approx(q[0].cx(), 0), "and at x=100 the sprite at 100 sits on the left edge");
        C(w, cam)->flags = 0;

        T(w, cam)->posX = 400; T(w, cam)->posY = 300; T(w, cam)->rotation = 1.5707964f;
        q = draw(w);
        check(approx(q[0].cx(), 100) && approx(q[0].cy(), 50), "a rotated camera ignores its rotation by default");
        C(w, cam)->flags = kCamFollowRotation;
        q = draw(w);
        // world offset from the centre (-300, -250) turned by -90 degrees on screen: (x, y) -> (y, -x) = (-250, 300), around (400, 300).
        check(approx(q[0].cx(), 150) && approx(q[0].cy(), 600), "with follow rotation the view turns the other way: offset (-300,-250) lands at (150, 600)");
    }

    // ---- limits ----
    {
        clearAll(w);
        sprite(w, 100, 50);
        const Entity cam = camera(w, 0, 0, kCamLimitLeft | kCamLimitTop);
        C(w, cam)->limitLeft = 0; C(w, cam)->limitTop = 0;
        auto q = draw(w);
        check(approx(q[0].cx(), 100) && approx(q[0].cy(), 50), "left/top limit 0: a camera at the origin cannot show past it, the view starts at the corner");
        check(approx(centerX(w, cam), 400) && approx(centerY(w, cam), 300), "the centre is pushed to half a view in");
        T(w, cam)->posX = 5000; T(w, cam)->posY = 5000;
        C(w, cam)->flags = kCamLimitRight | kCamLimitBottom;
        C(w, cam)->limitRight = 1000; C(w, cam)->limitBottom = 800;
        tick(w, 0.0f);
        check(approx(centerX(w, cam), 600) && approx(centerY(w, cam), 500), "right/bottom limits stop the view's far edge at the limit");
        C(w, cam)->zoomX = C(w, cam)->zoomY = 2.0f;
        check(approx(centerX(w, cam), 800) && approx(centerY(w, cam), 650), "and with zoom 2 the view is half as wide, so the centre gets closer");
        C(w, cam)->flags = kCamLimitLeft | kCamLimitRight;
        C(w, cam)->limitLeft = 0; C(w, cam)->limitRight = 100;
        C(w, cam)->zoomX = C(w, cam)->zoomY = 1.0f;
        check(approx(centerX(w, cam), 400), "limits closer together than the view: the left limit wins");
    }

    // ---- drag margins and smoothing ----
    {
        clearAll(w);
        const Entity cam = camera(w, 0, 0, kCamDragH);
        C(w, cam)->dragLeft = C(w, cam)->dragRight = 0.5f;   // half the half view (400) = 200 either side
        tick(w, 0.0f);
        check(approx(centerX(w, cam), 0), "the first step snaps the centre to the camera");
        T(w, cam)->posX = 150;
        tick(w, 0.016f);
        check(approx(centerX(w, cam), 0), "inside the drag margin the centre stays");
        T(w, cam)->posX = 300;
        tick(w, 0.016f);
        check(approx(centerX(w, cam), 100), "past the margin the centre trails the target by exactly the margin (300 - 200)");
        T(w, cam)->posX = -50;
        tick(w, 0.016f);
        check(approx(centerX(w, cam), 100), "back inside again: no movement");
        T(w, cam)->posX = -400;
        tick(w, 0.016f);
        check(approx(centerX(w, cam), -200), "and the left margin trails the other way (-400 + 200)");

        clearAll(w);
        const Entity sm = camera(w, 0, 0, kCamSmoothing);
        C(w, sm)->smoothSpeed = 5.0f;
        tick(w, 0.0f);
        T(w, sm)->posX = 100;
        tick(w, 0.1f);
        check(approx(centerX(w, sm), 50), "smoothing speed 5 over 0.1 s covers half the distance");
        tick(w, 0.1f);
        check(approx(centerX(w, sm), 75), "and half of what is left on the next step");
        tick(w, 10.0f);
        check(approx(centerX(w, sm), 100), "a long step arrives and does not overshoot");
        T(w, sm)->posX = 300;
        tick(w, 0.0f);
        check(approx(centerX(w, sm), 300), "a zero step with smoothing on snaps");
    }

    // ---- layers and the current camera ----
    {
        clearAll(w);
        const Entity cam = camera(w, 500, 300);
        const Entity ui = w.create("ui");
        attachLayer(w, ui, 1);
        attachTransform(w, ui);
        sprite(w, 100, 50, ui);
        const Entity world = w.create("world");
        attachLayer(w, world, 0)->flags = kLayerFollowViewport;
        attachTransform(w, world);
        sprite(w, 100, 50, world);
        sprite(w, 100, 50);
        auto q = draw(w);
        check(q.size() == 3, "three sprites");
        std::sort(q.begin(), q.end(), [](const Box& a, const Box& b) { return a.cx() < b.cx(); });
        check(approx(q[0].cx(), 0) && approx(q[1].cx(), 0) && approx(q[2].cx(), 100),
              "the default canvas and a follow-viewport layer move with the camera; a plain layer stays on screen");
        (void)cam;

        clearAll(w);
        sprite(w, 100, 50);
        const Entity a = camera(w, 400, 300), b = camera(w, 500, 300);
        check(currentCamera(w) == a, "with several cameras and none flagged the first is current");
        check(makeCameraCurrent(w, b) && currentCamera(w) == b, "make current picks another");
        check((C(w, a)->flags & kCamCurrent) == 0 && (C(w, b)->flags & kCamCurrent) != 0, "and clears the flag on the old one");
        const auto q2 = draw(w);
        check(approx(q2[0].cx(), 0), "the sprite follows that camera");
        C(w, b)->flags |= kCamDisabled;
        check(currentCamera(w) == a, "a disabled camera is never current");
        C(w, a)->flags |= kCamDisabled;
        check(currentCamera(w) == kInvalidEntity, "no enabled camera: none");
        check(!makeCameraCurrent(w, w.create("plain")), "make current needs a camera");
    }

    // ---- parallax ----
    {
        clearAll(w);
        const Entity cam = camera(w, 500, 300);
        const Entity par = w.create("par");
        attachTransform(w, par);
        CParallax2D* p = attachParallax(w, par);
        p->pinX = 0.5f;   // scroll scale 0.5
        sprite(w, 100, 50, par);
        auto q = draw(w);
        check(q.size() == 1 && approx(q[0].cx(), 250), "scroll scale 0.5: the layer moves half as far as the camera (100 + 250 - 100 = 250 on screen)");
        p->pinX = 1.0f;
        T(w, cam)->posX = 400;
        q = draw(w);
        check(approx(q[0].cx(), 500), "scroll scale 0: pinned to the screen (world 100 + camera 400 = 500)");
        T(w, cam)->posX = 600;
        q = draw(w);
        check(approx(q[0].cx(), 500), "and it does not move as the camera does");
        p->pinX = 0.0f;
        p->offsetX = 30;
        q = draw(w);
        check(approx(q[0].cx(), 100 - 600 + 400 + 30), "scroll offset adds to the displacement");
        p->offsetX = 0;

        T(w, cam)->posX = 400;
        p->autoX = 50;
        tick(w, 1.0f);
        q = draw(w);
        check(approx(q[0].cx(), 150), "autoscroll 50 units a second moves the layer 50 after a second");
        p->autoX = 0; p->timeX = 0;

        p->flags = kParallaxNoFollow;
        T(w, cam)->posX = 900;
        q = draw(w);
        check(approx(q[0].cx(), 100) && approx(q[0].cy(), 50), "not following the camera: the layer is in screen space, camera moves do not show");
        p->flags = 0;

        p->pinX = 1.0f;
        p->flags = kParallaxLimitEnd;
        p->limitEndX = 300; p->limitEndY = 1e9f;
        T(w, cam)->posX = 600;
        q = draw(w);
        check(approx(q[0].cx(), 100 + 300 - 600 + 400), "a limit end clamps the camera position the scroll uses (300) while the view still follows (600)");
        p->flags = 0; p->pinX = 0;

        // tiling
        p->repeatX = 200;
        T(w, cam)->posX = 400;
        q = draw(w);
        std::sort(q.begin(), q.end(), [](const Box& a, const Box& b) { return a.cx() < b.cx(); });
        check(q.size() == 3 && approx(q[0].cx(), 300) && approx(q[1].cx(), 500) && approx(q[2].cx(), 700),
              "repeat size 200, one copy each side: three tiles 200 apart, on the lattice of the authored position (100 + 200 k)");
        T(w, cam)->posX = 4400;   // a long way off: still a tile either side of the view
        q = draw(w);
        std::sort(q.begin(), q.end(), [](const Box& a, const Box& b) { return a.cx() < b.cx(); });
        check(q.size() == 3 && q[1].cx() > 100 && q[1].cx() < 700, "however far the camera went the tiles stay around the view");
        p->repeatTimes = 3;
        q = draw(w);
        check(q.size() == 7, "repeat times 3: seven tiles");
        p->repeatY = 100;
        q = draw(w);
        check(q.size() == 49, "tiled on both axes: seven by seven");
        p->repeatX = 0; p->repeatY = 0;
        q = draw(w);
        check(q.size() == 1, "no repeat size: one copy");
    }

    // ---- a parallax of two sprites keeps their order ----
    {
        clearAll(w);
        camera(w, 400, 300);
        const Entity par = w.create("par");
        attachTransform(w, par);
        CParallax2D* p = attachParallax(w, par);
        p->repeatX = 100;
        const Entity a = sprite(w, 0, 0, par);
        const Entity b = sprite(w, 0, 0, par);
        T(w, a)->zIndex = 0; T(w, b)->zIndex = 0;
        ui::UiDrawList l;
        submitSprites(w);
        check(pendingCount() == 6, "two sprites in a three-tile parallax queue six items");
        flush(l);
        check(quads(l).size() == 6, "and draw six quads");
    }

    // ---- the ABI ----
    {
        clearAll(w);
        const Entity e = w.create("cam");
        const int h = static_cast<int>(e);
        AverScene2dTransform tr{};
        tr.position[0] = 400; tr.position[1] = 300;
        aver_scene2d_set_transform(h, &tr);
        AverScene2dCamera c{};
        c.zoom[0] = 2; c.zoom[1] = 2; c.offset[0] = 5; c.offset[1] = 6; c.limit[0] = 1; c.limit[1] = 2; c.limit[2] = 3; c.limit[3] = 4;
        c.smoothSpeed = 7; c.drag[0] = 0.1f; c.drag[1] = 0.2f; c.drag[2] = 0.3f; c.drag[3] = 0.4f;
        c.flags = AVER_SCENE2D_C_SMOOTHING | AVER_SCENE2D_C_LIMIT_LEFT | 0x8000;
        check(aver_scene2d_set_camera(h, &c) == 1, "set_camera adds the component");
        AverScene2dCamera r{};
        check(aver_scene2d_get_camera(h, &r) == 1 && r.zoom[0] == 2 && r.offset[1] == 6 && r.limit[3] == 4 && r.smoothSpeed == 7 && r.drag[2] == 0.3f &&
                  r.flags == (AVER_SCENE2D_C_SMOOTHING | AVER_SCENE2D_C_LIMIT_LEFT),
              "every camera field round-trips and unknown flag bits are dropped");
        c.zoom[0] = NAN;
        check(aver_scene2d_set_camera(h, &c) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::InvalidArgument), "a NaN zoom is InvalidArgument");

        AverScene2dParallax p{};
        p.scrollScale[0] = 0.25f; p.scrollScale[1] = 1; p.scrollOffset[0] = 3; p.repeatSize[0] = 64; p.repeatTimes = 2; p.autoscroll[1] = -4;
        p.limitBegin[0] = -9; p.limitEnd[1] = 9; p.flags = AVER_SCENE2D_P_LIMIT_BEGIN | 0x40;
        check(aver_scene2d_set_parallax(h, &p) == 1, "set_parallax");
        AverScene2dParallax pr{};
        check(aver_scene2d_get_parallax(h, &pr) == 1 && pr.scrollScale[0] == 0.25f && pr.scrollScale[1] == 1 && pr.scrollOffset[0] == 3 && pr.repeatSize[0] == 64 &&
                  pr.repeatTimes == 2 && pr.autoscroll[1] == -4 && pr.limitBegin[0] == -9 && pr.limitEnd[1] == 9 && pr.flags == AVER_SCENE2D_P_LIMIT_BEGIN,
              "the scroll scale round-trips as given (stored as 1 - scale)");
        const auto* pc = w.component<CParallax2D>(e, g.parallax);
        check(pc && pc->pinX == 0.75f && pc->pinY == 0.0f, "and sits in the component as the share of the camera it cancels");
        p.repeatSize[0] = -1;
        check(aver_scene2d_set_parallax(h, &p) == 0, "a negative repeat size is refused");

        check(aver_scene2d_camera_make_current(h) == 1 && aver_scene2d_camera_current() == h, "make_current / current");
        float ctr[2] = {};
        aver_scene2d_set_canvas(nullptr, 0, 0, 800, 600);
        c.zoom[0] = 1; c.zoom[1] = 1; c.flags = 0; c.offset[0] = c.offset[1] = 0;
        aver_scene2d_set_camera(h, &c);
        check(aver_scene2d_camera_center(h, ctr) == 1 && approx(ctr[0], 400) && approx(ctr[1], 300), "camera_center reads the centre");
        float v[6] = {};
        check(aver_scene2d_camera_view(v) == 1 && approx(v[0], 1) && approx(v[3], 1) && approx(v[4], 0) && approx(v[5], 0), "camera_view for a centred camera is the identity");
        T(w, e)->posX = 450;
        check(aver_scene2d_camera_view(v) == 1 && approx(v[4], -50), "and shifts as the camera moves");
        check(aver_scene2d_camera_center(0x7FFFFF, ctr) == 0, "a stale entity has no centre");
        check(aver_scene2d_component_id(4) == static_cast<int>(g.camera) && aver_scene2d_component_id(5) == static_cast<int>(g.parallax), "component ids 4 and 5");
        check(aver_scene2d_struct_size(3) == 32 && aver_scene2d_struct_size(4) == 56 && aver_scene2d_struct_size(5) == 56 && aver_scene2d_struct_size(6) == 32 &&
                  aver_scene2d_struct_size(7) == 16 && aver_scene2d_struct_size(8) == 0,
              "the DLL reports the new struct sizes");
    }

    reset();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
