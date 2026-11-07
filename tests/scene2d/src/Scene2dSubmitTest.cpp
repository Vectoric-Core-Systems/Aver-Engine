// Scene2dSubmitTest -- the ordered sprite submit into the UI draw list and the shared-key submit for external items:
// quads (flip, region UVs, frames, modulate, clips, sampler and blend), the sort by (canvas layer, z, tree order), and
// a sprite under, between and over external draws. Against a real UiDrawList; no window, no GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

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

static bool approx(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

// Asset ids the fake host knows -> draw-list textures and sizes. Anything else does not resolve.
static bool resolve(i64 id, TextureInfo* out, void*) {
    switch (id) {
        case 1: *out = {101, 64, 64}; return true;
        case 2: *out = {102, 128, 64}; return true;
        case 3: *out = {103, 128, 64}; return true;
        case 4: *out = {104, 32, 32}; return true;
        case 5: *out = {105, 32, 32}; return true;
        default: return false;
    }
}

struct Box { f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; };

// The bounding box and the vertex nearest the top-left corner of one draw command.
static Box boxOf(const ui::UiDrawList& l, const ui::UiDrawCmd& c, ui::UiVertex* topLeft = nullptr) {
    Box b;
    for (u32 i = 0; i < c.indexCount; ++i) {
        const ui::UiVertex& v = l.vertices()[l.indices()[c.indexOffset + i]];
        b.x0 = std::min(b.x0, v.x); b.y0 = std::min(b.y0, v.y);
        b.x1 = std::max(b.x1, v.x); b.y1 = std::max(b.y1, v.y);
    }
    if (topLeft) {
        f32 best = 1e18f;
        for (u32 i = 0; i < c.indexCount; ++i) {
            const ui::UiVertex& v = l.vertices()[l.indices()[c.indexOffset + i]];
            const f32 d = (v.x - b.x0) * (v.x - b.x0) + (v.y - b.y0) * (v.y - b.y0);
            if (d < best) { best = d; *topLeft = v; }
        }
    }
    return b;
}

static std::vector<u64> order(const ui::UiDrawList& l) {
    std::vector<u64> t;
    for (const auto& c : l.commands(ui::UiLayer::Content)) t.push_back(c.texture);
    return t;
}

static C2DTransform* T(World& w, Entity e) { return w.component<C2DTransform>(e, ids().transform); }
static CSprite2D* S(World& w, Entity e) { return w.component<CSprite2D>(e, ids().sprite); }

static Entity sprite(World& w, const char* name, i64 tex, f32 x, f32 y, i32 z = 0, Entity parent = kInvalidEntity) {
    const Entity e = parent == kInvalidEntity ? w.create(name) : w.create(name, parent, Transform{});
    attachTransform(w, e);
    attachSprite(w, e, tex);
    C2DTransform* t = T(w, e);
    t->posX = x; t->posY = y; t->zIndex = z;
    return e;
}

static void clearAll(World& w) {
    reset();
    setTextureResolver(&resolve, nullptr);
    for (u32 i = w.count(); i-- > 0;) w.destroy(w.at(i));
    w.flush();
}

// An external item that draws one 10x10 rect with texture `tag` at (x, y).
struct Ext { f32 x, y; u64 tag; };
static void drawExt(void* list, void* user) {
    auto* l = static_cast<ui::UiDrawList*>(list);
    const Ext* e = static_cast<const Ext*>(user);
    l->addTexturedRect(e->x, e->y, 10, 10, e->tag, 0, 0, 1, 1, 0xFFFFFFFF);
}

int main() {
    World& w = World::instance();
    registerComponents(w);

    // ---- quads ----
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 1, 100, 50);
        (void)a;
        ui::UiDrawList l;
        check(submitSprites(w) == 1, "a sprite with a resolvable texture queues");
        flush(l);
        check(pendingCount() == 0, "flush empties the queue");
        check(l.commands(ui::UiLayer::Content).size() == 1, "one command in the Content band");
        const auto& c = l.commands(ui::UiLayer::Content)[0];
        check(c.texture == 101, "the command carries the resolved draw-list texture");
        const Box b = boxOf(l, c);
        check(approx(b.x0, 68) && approx(b.y0, 18) && approx(b.x1, 132) && approx(b.y1, 82), "centered on its position, one pixel per texel (64x64)");
        ui::UiVertex tl;
        boxOf(l, c, &tl);
        check(approx(tl.u, 0) && approx(tl.v, 0), "the top-left corner samples (0,0)");
        check(tl.rgba == 0xFFFFFFFFu, "a zero modulate is opaque white");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 1, 0, 0);
        S(w, a)->flags = kSpriteFlipH;
        ui::UiDrawList l;
        frame(w, l);
        ui::UiVertex tl;
        boxOf(l, l.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(tl.u, 1) && approx(tl.v, 0), "flip H swaps the U range");
        S(w, a)->flags = kSpriteFlipV;
        ui::UiDrawList l2;
        frame(w, l2);
        boxOf(l2, l2.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(tl.u, 0) && approx(tl.v, 1), "flip V swaps the V range");
        S(w, a)->flags = kSpriteFlipH | kSpriteFlipV;
        ui::UiDrawList l3;
        frame(w, l3);
        boxOf(l3, l3.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(tl.u, 1) && approx(tl.v, 1), "both flips swap both");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 1, 0, 0);
        CSprite2D* s = S(w, a);
        s->regionX = 16; s->regionY = 0; s->regionW = 32; s->regionH = 32;
        ui::UiDrawList l;
        frame(w, l);
        ui::UiVertex tl;
        const Box b = boxOf(l, l.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(b.x1 - b.x0, 32) && approx(b.y1 - b.y0, 32), "a region sets the quad's size");
        check(approx(tl.u, 0.25f) && approx(tl.v, 0), "and its UV origin");
        ui::UiVertex br;
        {
            const auto& c = l.commands(ui::UiLayer::Content)[0];
            for (u32 i = 0; i < c.indexCount; ++i) {
                const ui::UiVertex& v = l.vertices()[l.indices()[c.indexOffset + i]];
                if (approx(v.x, b.x1) && approx(v.y, b.y1)) br = v;
            }
        }
        check(approx(br.u, 0.75f) && approx(br.v, 0.5f), "the opposite corner samples (0.75, 0.5)");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 2, 0, 0);   // 128x64
        CSprite2D* s = S(w, a);
        s->hframes = 4; s->vframes = 2; s->frame = 5;   // column 1, row 1
        ui::UiDrawList l;
        frame(w, l);
        ui::UiVertex tl;
        const Box b = boxOf(l, l.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(b.x1 - b.x0, 32) && approx(b.y1 - b.y0, 32), "a frame is one cell of the grid");
        check(approx(tl.u, 0.25f) && approx(tl.v, 0.5f), "frame 5 of a 4x2 grid sits at column 1, row 1");
        S(w, a)->frame = 8;
        ui::UiDrawList l2;
        frame(w, l2);
        boxOf(l2, l2.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(tl.u, 0) && approx(tl.v, 0), "a frame past the grid wraps to 0");
        S(w, a)->frame = -1;
        ui::UiDrawList l3;
        frame(w, l3);
        boxOf(l3, l3.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(tl.u, 0.75f) && approx(tl.v, 0.5f), "and a negative frame wraps from the end");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 1, 100, 100);
        CSprite2D* s = S(w, a);
        s->modulate = 0x80FFFFFFu;
        s->flags = kSpriteNotCentered;
        s->offsetX = 10; s->offsetY = -5;
        ui::UiDrawList l;
        frame(w, l);
        ui::UiVertex tl;
        const Box b = boxOf(l, l.commands(ui::UiLayer::Content)[0], &tl);
        check(approx(b.x0, 110) && approx(b.y0, 95), "not centered: the offset places the top-left corner");
        check((tl.rgba >> 24) == 0x80, "the modulate alpha reaches the vertices");
        check((tl.rgba & 0xFF) >= 0x7F && (tl.rgba & 0xFF) <= 0x81, "and premultiplies the colour");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 2, 10, 20);   // 128x64
        T(w, a)->rotation = 1.57079632679f;
        ui::UiDrawList l;
        frame(w, l);
        const Box b = boxOf(l, l.commands(ui::UiLayer::Content)[0]);
        check(approx(b.x1 - b.x0, 64) && approx(b.y1 - b.y0, 128) && approx((b.x0 + b.x1) * 0.5f, 10) && approx((b.y0 + b.y1) * 0.5f, 20),
              "a quarter turn swaps the extents about the position");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 1, 0, 0);
        S(w, a)->flags = kSpriteNearest;
        S(w, a)->blend = kBlendAdd;
        ui::UiDrawList l;
        l.setLayer(ui::UiLayer::Overlay);
        l.setBlend(ui::UiBlend::Mul);
        frame(w, l);
        const auto& c = l.commands(ui::UiLayer::Content)[0];
        check(c.filter == ui::UiFilter::Nearest && c.blend == ui::UiBlend::Add, "sampler and blend ride on the command");
        check(l.layer() == ui::UiLayer::Overlay && l.blend() == ui::UiBlend::Mul, "and the draw list's own state is restored");
    }
    {   // merge: same texture in a row is one command
        clearAll(w);
        sprite(w, "a", 1, 0, 0); sprite(w, "b", 1, 80, 0); sprite(w, "c", 1, 160, 0);
        ui::UiDrawList l;
        frame(w, l);
        check(l.commands(ui::UiLayer::Content).size() == 1 && l.commands(ui::UiLayer::Content)[0].indexCount == 18, "three sprites of one texture batch into one command");
    }

    // ---- what is not drawn ----
    {
        clearAll(w);
        const Entity hid = sprite(w, "hid", 1, 0, 0);
        S(w, hid)->flags = kSpriteHidden;
        const Entity clear = sprite(w, "clear", 1, 0, 0);
        S(w, clear)->modulate = 0x00FFFFFFu;
        const Entity none = sprite(w, "none", 0, 0, 0);
        (void)none;
        sprite(w, "unknown", 99, 0, 0);
        const Entity parent = sprite(w, "parent", 4, 0, 0);
        sprite(w, "child", 4, 0, 0, 0, parent);
        T(w, parent)->flags = kT2DHidden;
        const Entity lay = w.create("hiddenLayer");
        attachLayer(w, lay, 2)->flags = kLayerHidden;
        sprite(w, "inLayer", 1, 0, 0, 0, lay);
        check(submitSprites(w) == 0, "hidden, fully transparent, textureless, unresolved, under a hidden parent or layer: nothing queues");
        reset();
        setTextureResolver(nullptr, nullptr);
        sprite(w, "visible", 1, 0, 0);
        check(submitSprites(w) == 0, "with no texture resolver installed nothing queues");
        setTextureResolver(&resolve, nullptr);
        ui::UiDrawList l;
        flush(l);
        check(l.empty(), "an empty flush draws nothing");
    }

    // ---- the clip and the canvas ----
    {
        clearAll(w);
        sprite(w, "a", 1, 50, 50);
        ui::UiStretchSettings st;
        st.mode = ui::UiStretchMode::CanvasItems;
        st.aspect = ui::UiStretchAspect::Keep;
        st.baseW = 100; st.baseH = 100;
        setCanvasView(st, CanvasView{10, 20, 400, 200});   // a 100x100 picture in 400x200: bars left and right, scale 2
        ui::UiDrawList l;
        frame(w, l);
        const auto& c = l.commands(ui::UiLayer::Content)[0];
        const Box b = boxOf(l, c);
        // screen = {100,0,200,200} in the view; origin (10,20). Sprite at content (50,50) is 64 wide: (18..82) * 2 + (110, 20).
        check(approx(b.x0, 18 * 2 + 110) && approx(b.x1, 82 * 2 + 110) && approx(b.y0, 18 * 2 + 20), "sprites draw under the stretch transform");
        check(c.clip.left == 110 && c.clip.top == 20 && c.clip.right == 310 && c.clip.bottom == 220, "and are clipped to the stretched picture, not the bars");
        check(l.clip().left < 0, "the clip is popped afterwards");
        setCanvasView({}, CanvasView{});
        ui::UiDrawList l2;
        frame(w, l2);
        const auto& c2 = l2.commands(ui::UiLayer::Content)[0];
        check(c2.clip.left < 0, "with no view set there is no clip");
        check(approx(boxOf(l2, c2).x0, 18), "and the transform is the identity");
    }

    // ---- sort order ----
    {
        clearAll(w);
        sprite(w, "z2", 1, 0, 0, 2);   // tex 101
        sprite(w, "z1", 2, 0, 0, 1);   // 102
        sprite(w, "z1b", 3, 0, 0, 1);  // 103, created later: drawn after z1
        sprite(w, "z-3", 4, 0, 0, -3); // 104
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{104, 102, 103, 101}), "sorted by z, then tree order (creation order among siblings)");
    }
    {
        clearAll(w);
        const Entity top = w.create("top");
        attachLayer(w, top, 5);
        const Entity hud = w.create("hud");
        attachLayer(w, hud, -1);
        sprite(w, "world", 1, 0, 0, 100);              // layer 0 (no layer ancestor), high z
        sprite(w, "overlay", 2, 0, 0, -50, top);       // layer 5, low z
        sprite(w, "below", 3, 0, 0, 0, hud);           // layer -1
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{103, 101, 102}), "the canvas layer outranks z: layer -1, then 0, then 5");
    }
    {
        clearAll(w);
        const Entity p = sprite(w, "p", 1, 0, 0, 0);
        const Entity c1 = sprite(w, "c1", 2, 0, 0, 0, p);
        sprite(w, "c2", 3, 0, 0, 0, p);
        sprite(w, "g", 4, 0, 0, 0, c1);
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{101, 102, 104, 103}), "equal layer and z: tree order, a parent before its children, depth first");
        // reparent c1's subtree after c2
        w.setParent(c1, kInvalidEntity);
        w.setParent(c1, p);
        ui::UiDrawList l2;
        frame(w, l2);
        check((order(l2) == std::vector<u64>{101, 103, 102, 104}), "re-parenting moves a subtree to the end of its new siblings");
    }
    {
        clearAll(w);
        const Entity a = sprite(w, "a", 1, 0, 0);
        const Entity b = sprite(w, "b", 2, 0, 0);
        T(w, a)->treeOrder = 20;
        T(w, b)->treeOrder = 10;
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{102, 101}), "an explicit tree order overrides the hierarchy's");
    }
    {   // z is relative to the parent, so a child's key is its world z
        clearAll(w);
        const Entity p = sprite(w, "p", 1, 0, 0, 5);
        sprite(w, "c", 2, 0, 0, -10, p);     // world z -5
        sprite(w, "other", 3, 0, 0, 0);      // z 0
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{102, 103, 101}), "z accumulates down the 2D chain");
    }
    {   // a layer's transform applies to its sprites
        clearAll(w);
        const Entity lay = w.create("layer");
        attachLayer(w, lay, 1);
        C2DLayer* lc = w.component<C2DLayer>(lay, ids().layer);
        lc->offsetX = 100; lc->scaleX = 2; lc->scaleY = 2;
        sprite(w, "s", 4, 10, 10, 0, lay);   // 32x32 at (10,10) in the layer -> (120,20) centre, 64 wide
        ui::UiDrawList l;
        frame(w, l);
        const Box b = boxOf(l, l.commands(ui::UiLayer::Content)[0]);
        check(approx(b.x0, 88) && approx(b.x1, 152) && approx(b.y0, -12) && approx(b.y1, 52), "a layer's offset and scale carry its sprites");
    }
    {   // the transform pass runs inside the submit
        clearAll(w);
        const Entity p = sprite(w, "p", 4, 100, 0);
        sprite(w, "c", 5, 10, 0, 0, p);
        ui::UiDrawList l;
        frame(w, l);
        check(approx(boxOf(l, l.commands(ui::UiLayer::Content)[1]).x0, 110 - 16), "a child follows its parent without a separate pass call");
    }

    // ---- shared-key submit for external items ----
    {
        clearAll(w);
        const Entity a = sprite(w, "bg", 1, 0, 0, 0);       // 101
        T(w, a)->treeOrder = 1;
        const Entity c = sprite(w, "over", 2, 0, 0, 0);     // 102
        T(w, c)->treeOrder = 3;
        Ext button{5, 5, 900};
        check(submitExternal(SortKey{0, 0, 2}, &drawExt, &button), "an external item queues");
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{101, 900, 102}), "a sprite under, a control between and a sprite over, by tree order");
    }
    {
        clearAll(w);
        sprite(w, "s", 1, 0, 0, 0);    // layer 0 z 0     101
        sprite(w, "t", 2, 0, 0, 3);    // layer 0 z 3     102
        Ext e1{0, 0, 901}, e2{0, 0, 902}, e3{0, 0, 903}, e4{0, 0, 904};
        submitExternal(SortKey{0, 1, 0}, &drawExt, &e1);    // between the two sprites by z
        submitExternal(SortKey{1, -9, 0}, &drawExt, &e2);   // a higher canvas layer: after everything
        submitExternal(SortKey{-1, 99, 0}, &drawExt, &e3);  // a lower layer: before everything
        submitExternal(SortKey{0, 1, 0}, &drawExt, &e4);    // same key as e1: submission order breaks the tie
        ui::UiDrawList l;
        frame(w, l);
        check((order(l) == std::vector<u64>{903, 101, 901, 904, 102, 902}), "external items interleave by layer and z, and ties keep submission order");
    }
    {
        clearAll(w);
        ui::UiStretchSettings st;
        st.mode = ui::UiStretchMode::CanvasItems;
        st.aspect = ui::UiStretchAspect::Keep;
        st.baseW = 100; st.baseH = 100;
        setCanvasView(st, CanvasView{0, 0, 200, 200});   // scale 2
        Ext inCanvas{10, 10, 910}, raw{10, 10, 911};
        submitExternal(SortKey{0, 0, 1}, &drawExt, &inCanvas, kExternalInCanvas);
        submitExternal(SortKey{0, 0, 2}, &drawExt, &raw);
        ui::UiDrawList l;
        flush(l);
        const auto& cmds = l.commands(ui::UiLayer::Content);
        check(cmds.size() == 2, "two external draws, two commands");
        const Box b1 = boxOf(l, cmds[0]), b2 = boxOf(l, cmds[1]);
        check(approx(b1.x0, 20) && approx(b1.x1, 40), "kExternalInCanvas draws in content units under the stretch");
        check(approx(b2.x0, 10) && approx(b2.x1, 20), "otherwise it draws in window pixels");
        check(!submitExternal(SortKey{}, nullptr, nullptr), "a null callback is refused");
    }
    {   // an item queued from a callback waits for the next flush
        clearAll(w);
        struct Re { u64 tag; bool done; };
        static Re re{920, false};
        static const SortKey again{0, 0, 9};
        static Ext inner{0, 0, 921};
        static const auto reenter = [](void* list, void*) {
            auto* l = static_cast<ui::UiDrawList*>(list);
            l->addTexturedRect(0, 0, 10, 10, re.tag, 0, 0, 1, 1, 0xFFFFFFFF);
            submitExternal(again, &drawExt, &inner);
        };
        submitExternal(SortKey{0, 0, 1}, reenter, nullptr);
        ui::UiDrawList l;
        flush(l);
        check((order(l) == std::vector<u64>{920}) && pendingCount() == 1, "an item submitted while flushing is held for the next flush");
        ui::UiDrawList l2;
        flush(l2);
        check((order(l2) == std::vector<u64>{921}), "and drawn there");
    }

    clearAll(w);
    reset();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
