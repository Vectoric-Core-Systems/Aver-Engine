// The 2D scene C ABI's translation unit (scene2d_abi.h): the canvas over the process-wide Canvas2D.
#include "aver/scene/scene2d_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <type_traits>

using namespace aver;
using namespace aver::scene2d;

static_assert(AVER_SCENE2D_EXTERNAL_IN_CANVAS == kExternalInCanvas, "AVER_SCENE2D_EXTERNAL_IN_CANVAS drift");
static_assert(std::is_same_v<aver_scene2d_draw_fn, ExternalDrawFn>, "the C callback type is the C++ one");
static_assert(AVER_SCENE2D_T_Z_ABSOLUTE == kT2DZAbsolute && AVER_SCENE2D_T_TOP_LEVEL == kT2DTopLevel && AVER_SCENE2D_T_HIDDEN == kT2DHidden, "AVER_SCENE2D_T_* drift");
static_assert(AVER_SCENE2D_S_FLIP_H == kSpriteFlipH && AVER_SCENE2D_S_FLIP_V == kSpriteFlipV && AVER_SCENE2D_S_NOT_CENTERED == kSpriteNotCentered &&
              AVER_SCENE2D_S_HIDDEN == kSpriteHidden && AVER_SCENE2D_S_NEAREST == kSpriteNearest, "AVER_SCENE2D_S_* drift");
static_assert(AVER_SCENE2D_L_HIDDEN == kLayerHidden && AVER_SCENE2D_L_FOLLOW_VIEWPORT == kLayerFollowViewport, "AVER_SCENE2D_L_* drift");
static_assert(AVER_SCENE2D_BLEND_MIX == kBlendMix && AVER_SCENE2D_BLEND_ADD == kBlendAdd && AVER_SCENE2D_BLEND_SUB == kBlendSub &&
              AVER_SCENE2D_BLEND_MUL == kBlendMul, "AVER_SCENE2D_BLEND_* drift");
static_assert(sizeof(AverScene2dTransform) == 32 && sizeof(AverScene2dSprite) == 56 && sizeof(AverScene2dLayer) == 32, "the ABI structs are padding-free");
static_assert(offsetof(C2DTransform, treeOrder) - offsetof(C2DTransform, posX) + sizeof(u32) == sizeof(AverScene2dTransform),
              "AverScene2dTransform mirrors the authored words of C2DTransform");

namespace {

struct TextureBridge {
    aver_scene2d_texture_fn fn = nullptr;
    void* user = nullptr;
};
TextureBridge g_tex;

bool resolveThunk(i64 assetId, TextureInfo* out, void*) {
    uint64_t t = 0;
    int32_t wh[2] = {0, 0};
    if (!g_tex.fn || !g_tex.fn(assetId, &t, wh, g_tex.user) || t == 0 || wh[0] <= 0 || wh[1] <= 0) return false;
    out->ui = t;
    out->w = wh[0];
    out->h = wh[1];
    return true;
}

bool finiteAll(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// A live entity, or sets BadHandle.
bool live(int32_t entity) {
    const bool ok = entity > 0 && scene::World::instance().valid(static_cast<Entity>(entity));
    if (!ok) setAbiError(AbiError::BadHandle);
    return ok;
}

}   // namespace

extern "C" {

int32_t aver_scene2d_abi_version(void) { return AVER_SCENE2D_ABI_VERSION; }

int32_t aver_scene2d_register(void) {
    const Ids id = registerComponents(scene::World::instance());
    const bool ok = id.transform && id.sprite && id.layer;
    setAbiError(ok ? AbiError::Ok : AbiError::AllocationFailed);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_component_id(int32_t which) {
    const Ids id = ids();
    return static_cast<int32_t>(which == 0 ? id.transform : which == 1 ? id.sprite : which == 2 ? id.layer : 0);
}

int32_t aver_scene2d_struct_size(int32_t which) {
    return which == 0 ? static_cast<int32_t>(sizeof(AverScene2dTransform)) : which == 1 ? static_cast<int32_t>(sizeof(AverScene2dSprite))
         : which == 2 ? static_cast<int32_t>(sizeof(AverScene2dLayer)) : 0;
}

int32_t aver_scene2d_set_transform(int32_t entity, const AverScene2dTransform* d) {
    if (!d) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    if (!finiteAll(d->position, 2) || !std::isfinite(d->rotation) || !finiteAll(d->scale, 2) || d->zIndex < kZMin || d->zIndex > kZMax) {
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    scene::World& w = scene::World::instance();
    if (!ids().transform) registerComponents(w);
    C2DTransform* t = attachTransform(w, static_cast<Entity>(entity));
    if (!t) { setAbiError(AbiError::AllocationFailed); return 0; }
    t->posX = d->position[0]; t->posY = d->position[1];
    t->rotation = d->rotation;
    t->scaleX = d->scale[0]; t->scaleY = d->scale[1];
    t->zIndex = d->zIndex;
    t->flags = d->flags & (kT2DZAbsolute | kT2DTopLevel | kT2DHidden);
    t->treeOrder = d->treeOrder;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_get_transform(int32_t entity, AverScene2dTransform* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    const C2DTransform* t = ids().transform ? scene::World::instance().component<C2DTransform>(static_cast<Entity>(entity), ids().transform) : nullptr;
    if (!t) { setAbiError(AbiError::BadHandle); return 0; }
    out->position[0] = t->posX; out->position[1] = t->posY;
    out->rotation = t->rotation;
    out->scale[0] = t->scaleX; out->scale[1] = t->scaleY;
    out->zIndex = t->zIndex;
    out->flags = t->flags;
    out->treeOrder = t->treeOrder;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_set_sprite(int32_t entity, const AverScene2dSprite* d) {
    if (!d) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    if (!finiteAll(d->region, 4) || !finiteAll(d->offset, 2) || d->blend < kBlendMix || d->blend > kBlendMul) {
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    scene::World& w = scene::World::instance();
    if (!ids().sprite) registerComponents(w);
    CSprite2D* s = attachSprite(w, static_cast<Entity>(entity), d->texture);
    if (!s) { setAbiError(AbiError::AllocationFailed); return 0; }
    s->regionX = d->region[0]; s->regionY = d->region[1]; s->regionW = d->region[2]; s->regionH = d->region[3];
    s->offsetX = d->offset[0]; s->offsetY = d->offset[1];
    s->modulate = d->modulate;
    s->flags = d->flags & (kSpriteFlipH | kSpriteFlipV | kSpriteNotCentered | kSpriteHidden | kSpriteNearest);
    s->hframes = d->hframes; s->vframes = d->vframes; s->frame = d->frame;
    s->blend = d->blend;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_get_sprite(int32_t entity, AverScene2dSprite* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    const CSprite2D* s = ids().sprite ? scene::World::instance().component<CSprite2D>(static_cast<Entity>(entity), ids().sprite) : nullptr;
    if (!s) { setAbiError(AbiError::BadHandle); return 0; }
    out->texture = s->texture;
    out->region[0] = s->regionX; out->region[1] = s->regionY; out->region[2] = s->regionW; out->region[3] = s->regionH;
    out->offset[0] = s->offsetX; out->offset[1] = s->offsetY;
    out->modulate = s->modulate;
    out->flags = s->flags;
    out->hframes = s->hframes; out->vframes = s->vframes; out->frame = s->frame;
    out->blend = s->blend;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_set_layer(int32_t entity, const AverScene2dLayer* d) {
    if (!d) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    if (!finiteAll(d->offset, 2) || !std::isfinite(d->rotation) || !finiteAll(d->scale, 2)) {
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    scene::World& w = scene::World::instance();
    if (!ids().layer) registerComponents(w);
    C2DLayer* l = attachLayer(w, static_cast<Entity>(entity), d->index);
    if (!l) { setAbiError(AbiError::AllocationFailed); return 0; }
    l->flags = d->flags & (kLayerHidden | kLayerFollowViewport);
    l->offsetX = d->offset[0]; l->offsetY = d->offset[1];
    l->rotation = d->rotation;
    l->scaleX = d->scale[0]; l->scaleY = d->scale[1];
    l->reserved = 0;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_get_layer(int32_t entity, AverScene2dLayer* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    const C2DLayer* l = ids().layer ? scene::World::instance().component<C2DLayer>(static_cast<Entity>(entity), ids().layer) : nullptr;
    if (!l) { setAbiError(AbiError::BadHandle); return 0; }
    out->index = l->index;
    out->flags = l->flags;
    out->offset[0] = l->offsetX; out->offset[1] = l->offsetY;
    out->rotation = l->rotation;
    out->scale[0] = l->scaleX; out->scale[1] = l->scaleY;
    out->reserved = 0;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_remove(int32_t entity, int32_t which) {
    if (!live(entity)) return 0;
    const u32 type = static_cast<u32>(aver_scene2d_component_id(which));
    if (type == 0) { setAbiError(AbiError::InvalidArgument); return 0; }
    const bool ok = scene::World::instance().removeComponent(static_cast<Entity>(entity), type);
    setAbiError(ok ? AbiError::Ok : AbiError::BadHandle);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_update_transforms(void) { return static_cast<int32_t>(updateTransforms(scene::World::instance())); }

int32_t aver_scene2d_world_affine(int32_t entity, float* out6, int32_t* outZ) {
    if (!out6) { setAbiError(AbiError::NullPointer); return 0; }
    const float identity[6] = {1, 0, 0, 1, 0, 0};
    std::memcpy(out6, identity, sizeof identity);
    if (outZ) *outZ = 0;
    if (!live(entity)) return 0;
    scene::World& w = scene::World::instance();
    const C2DTransform* t = ids().transform ? w.component<C2DTransform>(static_cast<Entity>(entity), ids().transform) : nullptr;
    if (!t) { setAbiError(AbiError::BadHandle); return 0; }
    updateTransforms(w);
    i32 z = 0;
    const Affine2 m = worldAffine(w, static_cast<Entity>(entity), &z);
    const float v[6] = {m.a, m.b, m.c, m.d, m.tx, m.ty};
    std::memcpy(out6, v, sizeof v);
    if (outZ) *outZ = z;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_sprite_bounds(int32_t entity, float* out4) {
    if (!out4) { setAbiError(AbiError::NullPointer); return 0; }
    std::memset(out4, 0, 4 * sizeof(float));
    if (!live(entity)) return 0;
    float r[4];
    if (!spriteBounds(scene::World::instance(), static_cast<Entity>(entity), r)) { setAbiError(AbiError::Unsupported); return 0; }
    std::memcpy(out4, r, sizeof r);
    setAbiError(AbiError::Ok);
    return 1;
}

void aver_scene2d_set_canvas(const float* in6, float viewX, float viewY, float viewW, float viewH) {
    ui::UiStretchSettings s;
    if (in6) {
        s.mode = static_cast<ui::UiStretchMode>(static_cast<int>(in6[0]));
        s.aspect = static_cast<ui::UiStretchAspect>(static_cast<int>(in6[1]));
        s.scaleMode = static_cast<ui::UiStretchScaleMode>(static_cast<int>(in6[2]));
        s.baseW = in6[3]; s.baseH = in6[4]; s.scale = in6[5];
    }
    const bool finite = std::isfinite(viewX) && std::isfinite(viewY) && std::isfinite(viewW) && std::isfinite(viewH);
    setCanvasView(s, finite ? CanvasView{viewX, viewY, viewW, viewH} : CanvasView{});
    setAbiError(finite ? AbiError::Ok : AbiError::InvalidArgument);
}

void aver_scene2d_set_canvas_view(float viewX, float viewY, float viewW, float viewH) {
    const bool finite = std::isfinite(viewX) && std::isfinite(viewY) && std::isfinite(viewW) && std::isfinite(viewH);
    setCanvasViewRect(finite ? CanvasView{viewX, viewY, viewW, viewH} : CanvasView{});
    setAbiError(finite ? AbiError::Ok : AbiError::InvalidArgument);
}

int32_t aver_scene2d_canvas_transform(float* out6, float* out11) {
    const CanvasTransform c = canvasTransform();
    if (out6) {
        const float v[6] = {c.toWindow.a, c.toWindow.b, c.toWindow.c, c.toWindow.d, c.toWindow.tx, c.toWindow.ty};
        std::memcpy(out6, v, sizeof v);
    }
    if (out11) {
        const ui::UiStretchResult& r = c.stretch;
        const float v[11] = {r.screen.x, r.screen.y, r.screen.w, r.screen.h, r.contentSize.x, r.contentSize.y, r.renderSize.x, r.renderSize.y,
                             r.scaleX, r.scaleY, r.letterboxed ? 1.0f : 0.0f};
        std::memcpy(out11, v, sizeof v);
    }
    return c.valid ? 1 : 0;
}

void aver_scene2d_set_texture_resolver(aver_scene2d_texture_fn fn, void* user) {
    g_tex = {fn, user};
    setTextureResolver(fn ? &resolveThunk : nullptr, nullptr);
}

int32_t aver_scene2d_submit_external(int32_t layer, int32_t z, int64_t order, aver_scene2d_draw_fn fn, void* user, uint32_t flags) {
    if (!fn) { setAbiError(AbiError::NullPointer); return 0; }
    const bool ok = submitExternal(SortKey{layer, z, static_cast<u64>(order)}, fn, user, flags);
    setAbiError(ok ? AbiError::Ok : AbiError::AllocationFailed);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_pending(void) { return static_cast<int32_t>(pendingCount()); }

void aver_scene2d_flush(void* drawList) {
    if (drawList) flush(*static_cast<ui::UiDrawList*>(drawList));
    else dropQueue();
}

int32_t aver_scene2d_frame(void* drawList) {
    if (!drawList) { setAbiError(AbiError::NullPointer); return 0; }
    const u32 queued = submitSprites(scene::World::instance());
    flush(*static_cast<ui::UiDrawList*>(drawList));
    setAbiError(AbiError::Ok);
    return static_cast<int32_t>(queued);
}

}   // extern "C"
