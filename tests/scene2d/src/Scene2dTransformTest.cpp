// Scene2dTransformTest -- the 2D components as dynamically registered entity components, and the 2D transform pass:
// parent chains, rotation, scale, z (relative and absolute), top-level, revisions (what recomposes and what does not).
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>

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

static bool approx(f32 a, f32 b, f32 tol = 1e-4f) { return std::fabs(a - b) <= tol; }

static C2DTransform* T(World& w, Entity e) { return w.component<C2DTransform>(e, ids().transform); }

static void point(const Affine2& m, f32 x, f32 y, f32& ox, f32& oy) {
    ox = m.a * x + m.c * y + m.tx;
    oy = m.b * x + m.d * y + m.ty;
}

int main() {
    World& w = World::instance();
    const u32 builtins = w.componentCount();

    const Ids id = registerComponents(w);
    check(id.transform && id.sprite && id.layer, "the three components register");
    check(id.transform > 16 && id.sprite > 16 && id.layer > 16, "they take dynamic ids above the built-ins");
    check(w.componentCount() == builtins + 3, "and add exactly three components");
    const Ids again = registerComponents(w);
    check(again.transform == id.transform && again.sprite == id.sprite && again.layer == id.layer && w.componentCount() == builtins + 3,
          "registering again is idempotent");
    check(w.componentVerified(id.transform) && w.componentVerified(id.sprite) && w.componentVerified(id.layer), "the field tables verify");
    check(w.componentSize(id.transform) == sizeof(C2DTransform) && w.componentSize(id.sprite) == sizeof(CSprite2D) &&
              w.componentSize(id.layer) == sizeof(C2DLayer),
          "strides match the structs");
    check(w.fieldId("CSprite2D.texture") != 0 && w.fieldId("C2DTransform.zIndex") != 0 && w.fieldId("C2DLayer.index") != 0,
          "fields are addressable by name for the generic scene ABI");

    // Zero means unset: a fresh component is valid as it is.
    const Entity a = w.create("a");
    attachTransform(w, a);
    C2DTransform* ta = T(w, a);   // re-fetched after every attach: the pool may move
    check(ta && ta->scaleX == 0.0f && ta->worldRev == 0, "a fresh C2DTransform is all zero");
    check(updateTransforms(w) == 1, "the first pass composes it");
    {
        i32 z = 99;
        const Affine2 m = worldAffine(w, a, &z);
        check(approx(m.a, 1) && approx(m.d, 1) && approx(m.b, 0) && approx(m.c, 0) && approx(m.tx, 0) && approx(m.ty, 0) && z == 0,
              "zero scale reads as 1: the identity");
    }
    check(updateTransforms(w) == 0, "an untouched world recomposes nothing");

    // Position, scale, rotation: p' = T * R * S * p.
    ta->posX = 10; ta->posY = 20; ta->scaleX = 2; ta->scaleY = 3;
    check(updateTransforms(w) == 1, "writing the authored words recomposes");
    {
        f32 x, y;
        point(worldAffine(w, a), 1, 1, x, y);
        check(approx(x, 12) && approx(y, 23), "position and scale apply");
    }
    ta->rotation = 1.57079632679f;   // a quarter turn, clockwise on screen (y down)
    updateTransforms(w);
    {
        f32 x, y;
        point(worldAffine(w, a), 1, 0, x, y);
        check(approx(x, 10, 1e-3f) && approx(y, 22, 1e-3f), "rotation turns +x onto +y after scale (1,0 -> 2 along +y)");
        point(worldAffine(w, a), 0, 1, x, y);
        check(approx(x, 7, 1e-3f) && approx(y, 20, 1e-3f), "and +y onto -x, scaled by 3");
    }

    // Parent chain.
    const Entity b = w.create("b", a, Transform{});
    const Entity c = w.create("c", b, Transform{});
    attachTransform(w, b);
    attachTransform(w, c);
    ta = T(w, a);
    C2DTransform* tb = T(w, b);
    C2DTransform* tc = T(w, c);
    tb->posX = 5;
    tc->posY = 7;
    check(updateTransforms(w) == 2, "attaching two children recomposes those two");
    {
        // b's origin: (5,0) -> scale (10,0) -> rotate (0,10) -> translate (10,30). c adds (0,7): scale (0,21) -> rotate (-21,0) -> (-11,30).
        f32 x, y, bx, by;
        point(worldAffine(w, b), 0, 0, bx, by);
        point(worldAffine(w, c), 0, 0, x, y);
        check(approx(bx, 10, 1e-3f) && approx(by, 30, 1e-3f), "a child lands where its parent's transform puts its local position");
        check(approx(x, -11, 1e-3f) && approx(y, 30, 1e-3f), "a grandchild composes the whole chain");
    }

    // Revisions: only what depends on a change recomposes.
    const u32 revB = tb->worldRev, revC = tc->worldRev;
    ta->posX = 11;
    check(updateTransforms(w) == 3, "moving the root recomposes it and everything below");
    check(tb->worldRev != revB && tc->worldRev != revC, "and bumps their revisions");
    tc->posY = 8;
    const u32 revB2 = tb->worldRev;
    check(updateTransforms(w) == 1 && tb->worldRev == revB2, "moving a leaf recomposes only the leaf");

    // A plain entity between 2D entities resets the chain.
    const Entity plain = w.create("plain", a, Transform{});
    const Entity d = w.create("d", plain, Transform{});
    attachTransform(w, d);
    ta = T(w, a); tb = T(w, b); tc = T(w, c);
    C2DTransform* td = T(w, d);
    td->posX = 3;
    updateTransforms(w);
    {
        f32 x, y;
        point(worldAffine(w, d), 0, 0, x, y);
        check(approx(x, 3) && approx(y, 0), "a parent that is not a C2DTransform contributes nothing");
    }

    // Reparenting recomposes with the new parent.
    const u32 beforeMove = td->worldRev;
    w.setParent(d, b);
    check(updateTransforms(w) >= 1 && td->worldRev != beforeMove, "reparenting recomposes");
    {
        f32 x, y, dx, dy;
        point(worldAffine(w, b), 3, 0, x, y);
        point(worldAffine(w, d), 0, 0, dx, dy);
        check(approx(dx, x, 1e-3f) && approx(dy, y, 1e-3f), "under its new parent the child sits at parent * local");
    }

    // Top level: keeps its place in the tree but not its parent's transform.
    td->flags |= kT2DTopLevel;
    updateTransforms(w);
    {
        f32 x, y;
        point(worldAffine(w, d), 0, 0, x, y);
        check(approx(x, 3) && approx(y, 0), "top-level ignores the parent's transform");
    }

    // z: relative by default, absolute by flag, clamped.
    ta->zIndex = 5;
    tb->zIndex = 2;
    td->flags &= ~kT2DTopLevel;
    td->zIndex = 1;
    tc->zIndex = 10;
    tc->flags |= kT2DZAbsolute;
    updateTransforms(w);
    {
        i32 zb = 0, zc = 0, zd = 0;
        worldAffine(w, b, &zb);
        worldAffine(w, c, &zc);
        worldAffine(w, d, &zd);
        check(zb == 7, "z is relative to the parent by default (5 + 2)");
        check(zc == 10, "kT2DZAbsolute makes it the final z");
        check(zd == 8, "a re-parented child follows its new parent's z (7 + 1)");
    }
    ta->zIndex = 100000;
    updateTransforms(w);
    {
        i32 z = 0;
        worldAffine(w, a, &z);
        check(z == kZMax, "z clamps to the canvas range");
    }

    // Non-finite input does not poison the chain.
    ta->posX = std::nanf("");
    updateTransforms(w);
    {
        const Affine2 m = worldAffine(w, b);
        check(std::isfinite(m.tx) && std::isfinite(m.ty), "a NaN position composes as 0");
    }
    ta->posX = 0;

    // Layers: their own transform, defaults applied.
    const Entity l = w.create("layer");
    C2DLayer* lay = attachLayer(w, l, 3);
    check(lay && lay->index == 3 && lay->scaleX == 0.0f, "attachLayer sets the index only");
    lay->offsetX = 100; lay->scaleX = 2;
    {
        const Affine2 m = layerAffine(*lay);
        check(approx(m.a, 2) && approx(m.d, 1) && approx(m.tx, 100) && approx(m.ty, 0), "a layer affine is translate * rotate * scale with scale 0 = 1");
    }

    // Sprite defaults.
    const Entity s = w.create("sprite");
    CSprite2D* sp = attachSprite(w, s, 1234);
    check(sp && sp->texture == 1234 && sp->modulate == 0 && sp->hframes == 0 && sp->flags == 0, "attachSprite sets the texture only");

    // Destroying a parent takes the subtree with it and the pass copes.
    w.destroy(a);
    w.flush();
    check(!w.valid(c), "the subtree went with its parent");
    check(updateTransforms(w) == 0, "and the pass settles");

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
