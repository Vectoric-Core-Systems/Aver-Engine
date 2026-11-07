// 2D components and the 2D transform pass (Scene2D.hpp).
#include "aver/scene/Scene2D.hpp"

#include "aver/core/Assert.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace aver::scene2d {

namespace {

Ids g_ids;

constexpr int kMaxParentDepth = 512;   // a deeper chain is cut: the rest is treated as a root

f32 finiteOr(f32 v, f32 def) { return std::isfinite(v) ? v : def; }

i32 clampZ(i64 z) { return static_cast<i32>(std::clamp<i64>(z, kZMin, kZMax)); }

constexpr u16 off(usize o) { return static_cast<u16>(o); }

// Recomposes `e`'s affine if its authored words, its parent, or its parent's composition changed. Parents first.
// `count` is the number recomposed.
void refresh(scene::World& w, Entity e, u32 type, int depth, u32& count) {
    auto* t = w.component<C2DTransform>(e, type);
    if (!t) return;
    const Entity p = (t->flags & kT2DTopLevel) || depth >= kMaxParentDepth ? scene::kInvalidEntity : w.parent(e);
    auto* pt = p != scene::kInvalidEntity ? w.component<C2DTransform>(p, type) : nullptr;
    if (pt) refresh(w, p, type, depth + 1, count);

    const u32 pid = pt ? p : 0u;
    const u32 prev = pt ? pt->worldRev : 0u;
    const bool dirty = t->worldRev == 0 || std::memcmp(&t->posX, t->seen, 8 * sizeof(u32)) != 0 ||
                       t->seen[8] != pid || t->parentRevSeen != prev;
    if (!dirty) return;

    const f32 sx = effScale(finiteOr(t->scaleX, 1.0f)), sy = effScale(finiteOr(t->scaleY, 1.0f));
    const f32 r = finiteOr(t->rotation, 0.0f);
    const f32 cs = std::cos(r), sn = std::sin(r);
    Affine2 world{cs * sx, sn * sx, -sn * sy, cs * sy, finiteOr(t->posX, 0.0f), finiteOr(t->posY, 0.0f)};
    i32 z = clampZ(t->zIndex);
    if (pt) {
        world = Affine2{pt->a, pt->b, pt->c, pt->d, pt->tx, pt->ty} * world;
        if (!(t->flags & kT2DZAbsolute)) z = clampZ(static_cast<i64>(pt->worldZ) + z);
    }
    t->a = world.a; t->b = world.b; t->c = world.c; t->d = world.d; t->tx = world.tx; t->ty = world.ty;
    t->worldZ = z;
    std::memcpy(t->seen, &t->posX, 8 * sizeof(u32));
    t->seen[8] = pid;
    t->parentRevSeen = prev;
    if (++t->worldRev == 0) t->worldRev = 1;
    ++count;
}

} // namespace

Ids registerComponents(scene::World& world) {
    if (g_ids.transform != 0 && world.componentId("C2DTransform") == g_ids.transform &&
        world.componentId("CSprite2D") == g_ids.sprite && world.componentId("C2DLayer") == g_ids.layer)
        return g_ids;
    using scene::FieldKind;
    Ids out;
    {
        auto b = world.registerComponent<C2DTransform>("C2DTransform");
        b.field("posX", FieldKind::F32, off(offsetof(C2DTransform, posX)))
            .field("posY", FieldKind::F32, off(offsetof(C2DTransform, posY)))
            .field("rotation", FieldKind::F32, off(offsetof(C2DTransform, rotation)))
            .field("scaleX", FieldKind::F32, off(offsetof(C2DTransform, scaleX)))
            .field("scaleY", FieldKind::F32, off(offsetof(C2DTransform, scaleY)))
            .field("zIndex", FieldKind::I32, off(offsetof(C2DTransform, zIndex)))
            .field("flags", FieldKind::I32, off(offsetof(C2DTransform, flags)))
            .field("treeOrder", FieldKind::I32, off(offsetof(C2DTransform, treeOrder)));
        static const char* const kSeen[9] = {"seen0", "seen1", "seen2", "seen3", "seen4", "seen5", "seen6", "seen7", "seen8"};
        for (usize i = 0; i < 9; ++i)
            b.field(kSeen[i], FieldKind::I32, off(offsetof(C2DTransform, seen) + i * 4), 0, true);
        b.field("a", FieldKind::F32, off(offsetof(C2DTransform, a)), 0, true)
            .field("b", FieldKind::F32, off(offsetof(C2DTransform, b)), 0, true)
            .field("c", FieldKind::F32, off(offsetof(C2DTransform, c)), 0, true)
            .field("d", FieldKind::F32, off(offsetof(C2DTransform, d)), 0, true)
            .field("tx", FieldKind::F32, off(offsetof(C2DTransform, tx)), 0, true)
            .field("ty", FieldKind::F32, off(offsetof(C2DTransform, ty)), 0, true)
            .field("worldZ", FieldKind::I32, off(offsetof(C2DTransform, worldZ)), 0, true)
            .field("worldRev", FieldKind::I32, off(offsetof(C2DTransform, worldRev)), 0, true)
            .field("parentRevSeen", FieldKind::I32, off(offsetof(C2DTransform, parentRevSeen)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(C2DTransform)), "C2DTransform");
        out.transform = b.typeId();
    }
    {
        auto b = world.registerComponent<CSprite2D>("CSprite2D");
        b.field("texture", FieldKind::I64, off(offsetof(CSprite2D, texture)))
            .field("regionX", FieldKind::F32, off(offsetof(CSprite2D, regionX)))
            .field("regionY", FieldKind::F32, off(offsetof(CSprite2D, regionY)))
            .field("regionW", FieldKind::F32, off(offsetof(CSprite2D, regionW)))
            .field("regionH", FieldKind::F32, off(offsetof(CSprite2D, regionH)))
            .field("offsetX", FieldKind::F32, off(offsetof(CSprite2D, offsetX)))
            .field("offsetY", FieldKind::F32, off(offsetof(CSprite2D, offsetY)))
            .field("modulate", FieldKind::I32, off(offsetof(CSprite2D, modulate)))
            .field("flags", FieldKind::I32, off(offsetof(CSprite2D, flags)))
            .field("hframes", FieldKind::I32, off(offsetof(CSprite2D, hframes)))
            .field("vframes", FieldKind::I32, off(offsetof(CSprite2D, vframes)))
            .field("frame", FieldKind::I32, off(offsetof(CSprite2D, frame)))
            .field("blend", FieldKind::I32, off(offsetof(CSprite2D, blend)));
        AVER_ASSERTM(b.verify(sizeof(CSprite2D)), "CSprite2D");
        out.sprite = b.typeId();
    }
    {
        auto b = world.registerComponent<C2DLayer>("C2DLayer");
        b.field("index", FieldKind::I32, off(offsetof(C2DLayer, index)))
            .field("flags", FieldKind::I32, off(offsetof(C2DLayer, flags)))
            .field("offsetX", FieldKind::F32, off(offsetof(C2DLayer, offsetX)))
            .field("offsetY", FieldKind::F32, off(offsetof(C2DLayer, offsetY)))
            .field("rotation", FieldKind::F32, off(offsetof(C2DLayer, rotation)))
            .field("scaleX", FieldKind::F32, off(offsetof(C2DLayer, scaleX)))
            .field("scaleY", FieldKind::F32, off(offsetof(C2DLayer, scaleY)))
            .field("reserved", FieldKind::I32, off(offsetof(C2DLayer, reserved)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(C2DLayer)), "C2DLayer");
        out.layer = b.typeId();
    }
    g_ids = out;
    return g_ids;
}

Ids ids() { return g_ids; }

C2DTransform* attachTransform(scene::World& w, Entity e) {
    return g_ids.transform ? static_cast<C2DTransform*>(w.addComponent(e, g_ids.transform)) : nullptr;
}

CSprite2D* attachSprite(scene::World& w, Entity e, i64 texture) {
    auto* s = g_ids.sprite ? static_cast<CSprite2D*>(w.addComponent(e, g_ids.sprite)) : nullptr;
    if (s) s->texture = texture;
    return s;
}

C2DLayer* attachLayer(scene::World& w, Entity e, i32 index) {
    auto* l = g_ids.layer ? static_cast<C2DLayer*>(w.addComponent(e, g_ids.layer)) : nullptr;
    if (l) l->index = index;
    return l;
}

u32 updateTransforms(scene::World& w) {
    if (!g_ids.transform) return 0;
    scene::ComponentPool* pool = w.pool(g_ids.transform);
    if (!pool) return 0;
    u32 count = 0;
    // refresh() only writes component bytes, so the dense slots stay put while it recurses.
    for (usize i = 0; i < pool->size(); ++i) refresh(w, pool->entityAt(i), g_ids.transform, 0, count);
    return count;
}

Affine2 worldAffine(scene::World& w, Entity e, i32* worldZ) {
    const auto* t = g_ids.transform ? w.component<C2DTransform>(e, g_ids.transform) : nullptr;
    if (worldZ) *worldZ = t ? t->worldZ : 0;
    if (!t || t->worldRev == 0) return {};
    return {t->a, t->b, t->c, t->d, t->tx, t->ty};
}

Affine2 layerAffine(const C2DLayer& l) {
    const f32 sx = effScale(finiteOr(l.scaleX, 1.0f)), sy = effScale(finiteOr(l.scaleY, 1.0f));
    const f32 r = finiteOr(l.rotation, 0.0f);
    const f32 cs = std::cos(r), sn = std::sin(r);
    return {cs * sx, sn * sx, -sn * sy, cs * sy, finiteOr(l.offsetX, 0.0f), finiteOr(l.offsetY, 0.0f)};
}

} // namespace aver::scene2d
