// The 2D canvas (Canvas2D.hpp): sprite gather, the shared sort, the draw-list emit and the stretch transform.
#include "aver/scene/Canvas2D.hpp"

#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace aver::scene2d {

namespace {

constexpr usize kMaxQueue = 1u << 20;

struct Item {
    SortKey key;
    u64 seq = 0;
    // external
    ExternalDrawFn fn = nullptr;
    void* user = nullptr;
    u32 flags = 0;
    // sprite
    bool sprite = false;
    ui::UiAffine xf;            // layer * world, before the canvas transform
    SpriteQuad q;
    u64 tex = 0;
    u32 rgba = 0xFFFFFFFF;
    ui::UiFilter filter = ui::UiFilter::Linear;
    ui::UiBlend blend = ui::UiBlend::Mix;
};

struct State {
    TextureResolveFn resolver = nullptr;
    void* resolverUser = nullptr;
    ui::UiStretchSettings stretch;
    CanvasView view;
    std::vector<Item> items;
    u64 nextSeq = 0;
};

State& st() {
    static State s;
    return s;
}

ui::UiAffine toUi(const Affine2& m) { return {m.a, m.b, m.c, m.d, m.tx, m.ty}; }

bool finite(f32 v) { return std::isfinite(v); }

} // namespace

void setTextureResolver(TextureResolveFn fn, void* user) {
    st().resolver = fn;
    st().resolverUser = user;
}

void setCanvasView(const ui::UiStretchSettings& stretch, const CanvasView& view) {
    st().stretch = stretch;
    st().view = view;
}

void setCanvasViewRect(const CanvasView& view) { st().view = view; }

CanvasTransform canvasTransform() {
    const State& s = st();
    CanvasTransform out;
    if (!(s.view.w > 0.0f) || !(s.view.h > 0.0f)) return out;
    out.stretch = ui::uiComputeStretch(s.stretch, s.view.w, s.view.h);
    out.valid = true;
    const ui::UiStretchResult& r = out.stretch;
    out.toWindow = {r.scaleX, 0.0f, 0.0f, r.scaleY, s.view.x + r.screen.x, s.view.y + r.screen.y};
    return out;
}

bool submitExternal(const SortKey& key, ExternalDrawFn fn, void* user, u32 flags) {
    State& s = st();
    if (!fn || s.items.size() >= kMaxQueue) return false;
    Item it;
    it.key = key;
    it.seq = s.nextSeq++;
    it.fn = fn;
    it.user = user;
    it.flags = flags;
    s.items.push_back(it);
    return true;
}

SpriteQuad spriteQuad(const CSprite2D& sp, i32 texW, i32 texH) {
    SpriteQuad q;
    if (texW <= 0 || texH <= 0) return q;
    f32 rx = 0, ry = 0, rw = static_cast<f32>(texW), rh = static_cast<f32>(texH);
    if (sp.regionW > 0.0f && sp.regionH > 0.0f) {
        rx = sp.regionX; ry = sp.regionY; rw = sp.regionW; rh = sp.regionH;
    }
    if (!finite(rx) || !finite(ry) || !finite(rw) || !finite(rh) || !finite(sp.offsetX) || !finite(sp.offsetY)) return q;
    const i32 hf = std::max<i32>(1, sp.hframes), vf = std::max<i32>(1, sp.vframes);
    const i64 n = static_cast<i64>(hf) * vf;
    const i64 f = n > 1 ? ((sp.frame % n) + n) % n : 0;
    const f32 cw = rw / static_cast<f32>(hf), ch = rh / static_cast<f32>(vf);
    const f32 sx = rx + static_cast<f32>(f % hf) * cw, sy = ry + static_cast<f32>(f / hf) * ch;
    q.w = cw;
    q.h = ch;
    q.x = (sp.flags & kSpriteNotCentered ? 0.0f : -cw * 0.5f) + sp.offsetX;
    q.y = (sp.flags & kSpriteNotCentered ? 0.0f : -ch * 0.5f) + sp.offsetY;
    q.u0 = sx / static_cast<f32>(texW);
    q.v0 = sy / static_cast<f32>(texH);
    q.u1 = (sx + cw) / static_cast<f32>(texW);
    q.v1 = (sy + ch) / static_cast<f32>(texH);
    if (sp.flags & kSpriteFlipH) std::swap(q.u0, q.u1);
    if (sp.flags & kSpriteFlipV) std::swap(q.v0, q.v1);
    q.ok = cw > 0.0f && ch > 0.0f;
    return q;
}

u32 submitSprites(scene::World& w) {
    const Ids id = ids();
    if (!id.sprite) return 0;
    updateTransforms(w);
    State& s = st();
    if (!s.resolver) return 0;
    scene::ComponentPool* pool = w.pool(id.sprite);
    if (!pool) return 0;

    struct Pending {
        Item item;
        std::vector<u64> path;   // order stamps, root first
        bool derived = true;
    };
    std::vector<Pending> pend;
    for (usize i = 0; i < pool->size(); ++i) {
        const Entity e = pool->entityAt(i);
        const auto& sp = *static_cast<const CSprite2D*>(pool->dataAt(i));
        if ((sp.flags & kSpriteHidden) || sp.texture == 0) continue;
        const u32 rgba = sp.modulate ? sp.modulate : 0xFFFFFFFFu;
        if ((rgba >> 24) == 0) continue;

        bool hidden = false;
        const C2DLayer* layer = nullptr;
        std::vector<u64> path;
        for (Entity a = e; a != scene::kInvalidEntity; a = w.parent(a)) {
            if (id.transform) {
                const auto* t = w.component<C2DTransform>(a, id.transform);
                if (t && (t->flags & kT2DHidden)) { hidden = true; break; }
            }
            if (!layer && id.layer) layer = w.component<C2DLayer>(a, id.layer);
            path.push_back(w.orderStamp(a));
        }
        if (hidden || (layer && (layer->flags & kLayerHidden))) continue;

        TextureInfo ti;
        if (!s.resolver(sp.texture, &ti, s.resolverUser) || ti.w <= 0 || ti.h <= 0) continue;
        const SpriteQuad q = spriteQuad(sp, ti.w, ti.h);
        if (!q.ok) continue;

        Pending p;
        p.item.sprite = true;
        p.item.q = q;
        p.item.tex = ti.ui;
        p.item.rgba = rgba;
        p.item.filter = (sp.flags & kSpriteNearest) ? ui::UiFilter::Nearest : ui::UiFilter::Linear;
        p.item.blend = static_cast<ui::UiBlend>(std::clamp<i32>(sp.blend, kBlendMix, kBlendMul));
        i32 z = 0;
        Affine2 world = worldAffine(w, e, &z);
        if (layer) world = layerAffine(*layer) * world;
        p.item.xf = toUi(world);
        p.item.key.layer = layer ? layer->index : 0;
        p.item.key.z = z;
        if (id.transform) {
            if (const auto* t = w.component<C2DTransform>(e, id.transform); t && t->treeOrder) {
                p.item.key.order = t->treeOrder;
                p.derived = false;
            }
        }
        std::reverse(path.begin(), path.end());
        p.path = std::move(path);
        pend.push_back(std::move(p));
    }

    // Tree order: an ancestor before its descendants, siblings by their order stamp. Ranks count from 1.
    std::vector<usize> idx(pend.size());
    std::iota(idx.begin(), idx.end(), usize{0});
    std::sort(idx.begin(), idx.end(), [&](usize a, usize b) { return pend[a].path < pend[b].path; });
    u64 rank = 0;
    for (const usize k : idx)
        if (pend[k].derived) pend[k].item.key.order = ++rank;

    u32 queued = 0;
    for (Pending& p : pend) {
        if (s.items.size() >= kMaxQueue) break;
        p.item.seq = s.nextSeq++;
        s.items.push_back(p.item);
        ++queued;
    }
    return queued;
}

bool spriteBounds(scene::World& w, Entity e, f32 out[4]) {
    const Ids id = ids();
    const auto* sp = id.sprite ? w.component<CSprite2D>(e, id.sprite) : nullptr;
    if (!sp) return false;
    TextureInfo ti;
    const bool hasRegion = sp->regionW > 0.0f && sp->regionH > 0.0f;
    const State& s = st();
    const bool resolved = s.resolver && s.resolver(sp->texture, &ti, s.resolverUser) && ti.w > 0 && ti.h > 0;
    if (!resolved) {
        if (!hasRegion) return false;
        ti.w = ti.h = 1;   // the region sets the size; the image's own is not needed for a rectangle
    }
    const SpriteQuad q = spriteQuad(*sp, ti.w, ti.h);
    if (!q.ok) return false;
    updateTransforms(w);
    const Affine2 m = worldAffine(w, e);
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; ++i) {
        const f32 px = q.x + ((i & 1) ? q.w : 0.0f), py = q.y + ((i & 2) ? q.h : 0.0f);
        const f32 wx = m.a * px + m.c * py + m.tx, wy = m.b * px + m.d * py + m.ty;
        x0 = std::min(x0, wx); y0 = std::min(y0, wy); x1 = std::max(x1, wx); y1 = std::max(y1, wy);
    }
    out[0] = x0; out[1] = y0; out[2] = x1 - x0; out[3] = y1 - y0;
    return true;
}

u32 pendingCount() { return static_cast<u32>(st().items.size()); }

void dropQueue() {
    st().items.clear();
    st().nextSeq = 0;
}

void flush(ui::UiDrawList& list) {
    State& s = st();
    std::vector<Item> items;
    items.swap(s.items);   // a callback that submits again queues for the NEXT flush
    s.nextSeq = 0;
    if (items.empty()) return;
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.key.layer != b.key.layer) return a.key.layer < b.key.layer;
        if (a.key.z != b.key.z) return a.key.z < b.key.z;
        if (a.key.order != b.key.order) return a.key.order < b.key.order;
        return a.seq < b.seq;
    });

    const CanvasTransform canvas = canvasTransform();
    const ui::UiLayer layerWas = list.layer();
    const ui::UiFilter filterWas = list.filter();
    const ui::UiAddress addressWas = list.address();
    const ui::UiBlend blendWas = list.blend();
    bool clipped = false;
    if (canvas.valid) {   // the letterbox bars stay empty
        const f32 vx = st().view.x + canvas.stretch.screen.x, vy = st().view.y + canvas.stretch.screen.y;
        list.pushClip({static_cast<i32>(std::floor(vx)), static_cast<i32>(std::floor(vy)),
                       static_cast<i32>(std::ceil(vx + canvas.stretch.screen.w)), static_cast<i32>(std::ceil(vy + canvas.stretch.screen.h))});
        clipped = true;
    }
    list.setLayer(ui::UiLayer::Content);

    for (const Item& it : items) {
        if (it.sprite) {
            list.setSampler(it.filter, ui::UiAddress::Clamp);
            list.setBlend(it.blend);
            list.pushTransform(canvas.toWindow * it.xf);
            list.addTexturedRect(it.q.x, it.q.y, it.q.w, it.q.h, it.tex, it.q.u0, it.q.v0, it.q.u1, it.q.v1, it.rgba);
            list.popTransform();
        } else {
            list.setLayer(ui::UiLayer::Content);
            list.setSampler(ui::UiFilter::Linear, ui::UiAddress::Clamp);
            list.setBlend(ui::UiBlend::Mix);
            const bool inCanvas = (it.flags & kExternalInCanvas) != 0;
            if (inCanvas) list.pushTransform(canvas.toWindow);
            it.fn(&list, it.user);
            if (inCanvas) list.popTransform();
        }
    }

    if (clipped) list.popClip();
    list.setLayer(layerWas);
    list.setSampler(filterWas, addressWas);
    list.setBlend(blendWas);
}

void frame(scene::World& world, ui::UiDrawList& list) {
    submitSprites(world);
    flush(list);
}

void reset() {
    State& s = st();
    s.items.clear();
    s.nextSeq = 0;
    s.resolver = nullptr;
    s.resolverUser = nullptr;
    s.stretch = {};
    s.view = {};
}

} // namespace aver::scene2d
