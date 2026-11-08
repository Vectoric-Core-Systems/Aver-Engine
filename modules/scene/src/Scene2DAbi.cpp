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
static_assert(AVER_SCENE2D_A_PLAYING == kAnimPlaying && AVER_SCENE2D_ANIM_MAX_CLIPS == kMaxAnimClips && AVER_SCENE2D_ANIM_MAX_FRAMES == kMaxAnimFrames &&
              AVER_SCENE2D_ANIM_NAME_BYTES == kClipNameBytes, "AVER_SCENE2D_A_* / ANIM_* drift");
static_assert(AVER_SCENE2D_ANIM_EVENT_FRAME_CHANGED == kAnimEventFrameChanged && AVER_SCENE2D_ANIM_EVENT_FINISHED == kAnimEventFinished &&
              AVER_SCENE2D_ANIM_EVENT_LOOPED == kAnimEventLooped && AVER_SCENE2D_ANIM_EVENT_ANIMATION_CHANGED == kAnimEventAnimationChanged, "AVER_SCENE2D_ANIM_EVENT_* drift");
static_assert(AVER_SCENE2D_C_DISABLED == kCamDisabled && AVER_SCENE2D_C_CURRENT == kCamCurrent && AVER_SCENE2D_C_FOLLOW_ROTATION == kCamFollowRotation &&
              AVER_SCENE2D_C_SMOOTHING == kCamSmoothing && AVER_SCENE2D_C_DRAG_H == kCamDragH && AVER_SCENE2D_C_DRAG_V == kCamDragV &&
              AVER_SCENE2D_C_ANCHOR_TOP_LEFT == kCamAnchorTopLeft && AVER_SCENE2D_C_LIMIT_LEFT == kCamLimitLeft && AVER_SCENE2D_C_LIMIT_TOP == kCamLimitTop &&
              AVER_SCENE2D_C_LIMIT_RIGHT == kCamLimitRight && AVER_SCENE2D_C_LIMIT_BOTTOM == kCamLimitBottom, "AVER_SCENE2D_C_* drift");
static_assert(AVER_SCENE2D_P_NO_FOLLOW == kParallaxNoFollow && AVER_SCENE2D_P_LIMIT_BEGIN == kParallaxLimitBegin && AVER_SCENE2D_P_LIMIT_END == kParallaxLimitEnd,
              "AVER_SCENE2D_P_* drift");
static_assert(sizeof(AverScene2dAnimFrame) == sizeof(AnimFrame2D) && sizeof(AverScene2dAnimEvent) == sizeof(AnimEvent), "frame and event structs mirror the C++ ones");
static_assert(sizeof(AverScene2dAnimState) == 32 && sizeof(AverScene2dCamera) == 56 && sizeof(AverScene2dParallax) == 56, "the ABI structs are padding-free");
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

const CSpriteAnim2D* animOfEntity(int32_t entity) {
    if (!live(entity)) return nullptr;
    const CSpriteAnim2D* a = ids().anim ? scene::World::instance().component<CSpriteAnim2D>(static_cast<Entity>(entity), ids().anim) : nullptr;
    if (!a) setAbiError(AbiError::BadHandle);
    return a;
}

}   // namespace

extern "C" {

int32_t aver_scene2d_abi_version(void) { return AVER_SCENE2D_ABI_VERSION; }

int32_t aver_scene2d_register(void) {
    const Ids id = registerComponents(scene::World::instance());
    const bool ok = id.transform && id.sprite && id.layer && id.anim && id.camera && id.parallax;
    setAbiError(ok ? AbiError::Ok : AbiError::AllocationFailed);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_component_id(int32_t which) {
    const Ids id = ids();
    const u32 all[6] = {id.transform, id.sprite, id.layer, id.anim, id.camera, id.parallax};
    return which >= 0 && which < 6 ? static_cast<int32_t>(all[which]) : 0;
}

int32_t aver_scene2d_struct_size(int32_t which) {
    return which == 0 ? static_cast<int32_t>(sizeof(AverScene2dTransform)) : which == 1 ? static_cast<int32_t>(sizeof(AverScene2dSprite))
         : which == 2 ? static_cast<int32_t>(sizeof(AverScene2dLayer)) : which == 3 ? static_cast<int32_t>(sizeof(AverScene2dAnimState))
         : which == 4 ? static_cast<int32_t>(sizeof(AverScene2dCamera)) : which == 5 ? static_cast<int32_t>(sizeof(AverScene2dParallax))
         : which == 6 ? static_cast<int32_t>(sizeof(AverScene2dAnimFrame)) : which == 7 ? static_cast<int32_t>(sizeof(AverScene2dAnimEvent)) : 0;
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

int32_t aver_scene2d_set_camera(int32_t entity, const AverScene2dCamera* d) {
    if (!d) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    if (!finiteAll(d->zoom, 2) || !finiteAll(d->offset, 2) || !finiteAll(d->limit, 4) || !std::isfinite(d->smoothSpeed) || !finiteAll(d->drag, 4)) {
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    scene::World& w = scene::World::instance();
    if (!ids().camera) registerComponents(w);
    CCamera2D* c = attachCamera(w, static_cast<Entity>(entity));
    if (!c) { setAbiError(AbiError::AllocationFailed); return 0; }
    const u32 mask = kCamDisabled | kCamCurrent | kCamFollowRotation | kCamSmoothing | kCamDragH | kCamDragV | kCamAnchorTopLeft | kCamLimitLeft |
                     kCamLimitTop | kCamLimitRight | kCamLimitBottom;
    c->zoomX = d->zoom[0]; c->zoomY = d->zoom[1];
    c->offsetX = d->offset[0]; c->offsetY = d->offset[1];
    c->limitLeft = d->limit[0]; c->limitTop = d->limit[1]; c->limitRight = d->limit[2]; c->limitBottom = d->limit[3];
    c->smoothSpeed = d->smoothSpeed;
    c->dragLeft = d->drag[0]; c->dragTop = d->drag[1]; c->dragRight = d->drag[2]; c->dragBottom = d->drag[3];
    c->flags = d->flags & mask;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_get_camera(int32_t entity, AverScene2dCamera* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    const CCamera2D* c = ids().camera ? scene::World::instance().component<CCamera2D>(static_cast<Entity>(entity), ids().camera) : nullptr;
    if (!c) { setAbiError(AbiError::BadHandle); return 0; }
    out->zoom[0] = c->zoomX; out->zoom[1] = c->zoomY;
    out->offset[0] = c->offsetX; out->offset[1] = c->offsetY;
    out->limit[0] = c->limitLeft; out->limit[1] = c->limitTop; out->limit[2] = c->limitRight; out->limit[3] = c->limitBottom;
    out->smoothSpeed = c->smoothSpeed;
    out->drag[0] = c->dragLeft; out->drag[1] = c->dragTop; out->drag[2] = c->dragRight; out->drag[3] = c->dragBottom;
    out->flags = c->flags;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_set_parallax(int32_t entity, const AverScene2dParallax* d) {
    if (!d) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    if (!finiteAll(d->scrollScale, 2) || !finiteAll(d->scrollOffset, 2) || !finiteAll(d->repeatSize, 2) || d->repeatSize[0] < 0 || d->repeatSize[1] < 0 ||
        d->repeatTimes < 0 || !finiteAll(d->autoscroll, 2) || !finiteAll(d->limitBegin, 2) || !finiteAll(d->limitEnd, 2)) {
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    scene::World& w = scene::World::instance();
    if (!ids().parallax) registerComponents(w);
    CParallax2D* p = attachParallax(w, static_cast<Entity>(entity));
    if (!p) { setAbiError(AbiError::AllocationFailed); return 0; }
    p->pinX = 1.0f - d->scrollScale[0]; p->pinY = 1.0f - d->scrollScale[1];
    p->offsetX = d->scrollOffset[0]; p->offsetY = d->scrollOffset[1];
    p->repeatX = d->repeatSize[0]; p->repeatY = d->repeatSize[1];
    p->repeatTimes = d->repeatTimes;
    p->autoX = d->autoscroll[0]; p->autoY = d->autoscroll[1];
    p->limitBeginX = d->limitBegin[0]; p->limitBeginY = d->limitBegin[1]; p->limitEndX = d->limitEnd[0]; p->limitEndY = d->limitEnd[1];
    p->flags = d->flags & (kParallaxNoFollow | kParallaxLimitBegin | kParallaxLimitEnd);
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_get_parallax(int32_t entity, AverScene2dParallax* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    const CParallax2D* p = ids().parallax ? scene::World::instance().component<CParallax2D>(static_cast<Entity>(entity), ids().parallax) : nullptr;
    if (!p) { setAbiError(AbiError::BadHandle); return 0; }
    out->scrollScale[0] = 1.0f - p->pinX; out->scrollScale[1] = 1.0f - p->pinY;
    out->scrollOffset[0] = p->offsetX; out->scrollOffset[1] = p->offsetY;
    out->repeatSize[0] = p->repeatX; out->repeatSize[1] = p->repeatY;
    out->repeatTimes = p->repeatTimes;
    out->autoscroll[0] = p->autoX; out->autoscroll[1] = p->autoY;
    out->limitBegin[0] = p->limitBeginX; out->limitBegin[1] = p->limitBeginY; out->limitEnd[0] = p->limitEndX; out->limitEnd[1] = p->limitEndY;
    out->flags = p->flags;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_camera_make_current(int32_t entity) {
    if (!live(entity)) return 0;
    const bool ok = makeCameraCurrent(scene::World::instance(), static_cast<Entity>(entity));
    setAbiError(ok ? AbiError::Ok : AbiError::BadHandle);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_camera_current(void) {
    const Entity e = currentCamera(scene::World::instance());
    return e == scene::kInvalidEntity ? 0 : static_cast<int32_t>(e);
}

int32_t aver_scene2d_camera_center(int32_t entity, float* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    out[0] = out[1] = 0.0f;
    if (!live(entity)) return 0;
    scene::World& w = scene::World::instance();
    if (!ids().camera || !w.component<CCamera2D>(static_cast<Entity>(entity), ids().camera)) { setAbiError(AbiError::BadHandle); return 0; }
    updateTransforms(w);
    const CanvasTransform ct = canvasTransform();
    cameraCenter(w, static_cast<Entity>(entity), ct.valid ? ct.stretch.contentSize.x : 0.0f, ct.valid ? ct.stretch.contentSize.y : 0.0f, out);
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_camera_view(float* out6) {
    if (!out6) { setAbiError(AbiError::NullPointer); return 0; }
    scene::World& w = scene::World::instance();
    const CanvasTransform ct = canvasTransform();
    const Affine2 m = ct.valid ? cameraView(w, ct.stretch.contentSize.x, ct.stretch.contentSize.y) : Affine2{};
    const float v[6] = {m.a, m.b, m.c, m.d, m.tx, m.ty};
    std::memcpy(out6, v, sizeof v);
    setAbiError(AbiError::Ok);
    return ct.valid && currentCamera(w) != scene::kInvalidEntity ? 1 : 0;
}

int32_t aver_scene2d_set_anim(int32_t entity, const AverScene2dAnimState* d) {
    if (!d) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    if (!std::isfinite(d->speedScale) || d->autoplay < 0 || d->autoplay > kMaxAnimClips) { setAbiError(AbiError::InvalidArgument); return 0; }
    scene::World& w = scene::World::instance();
    if (!ids().anim) registerComponents(w);
    CSpriteAnim2D* a = attachAnim(w, static_cast<Entity>(entity));
    if (!a) { setAbiError(AbiError::AllocationFailed); return 0; }
    a->speedScale = d->speedScale;
    a->autoplay = d->autoplay;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_get_anim(int32_t entity, AverScene2dAnimState* out) {
    if (!out) { setAbiError(AbiError::NullPointer); return 0; }
    const CSpriteAnim2D* a = animOfEntity(entity);
    if (!a) return 0;
    out->speedScale = a->speedScale;
    out->flags = a->flags & kAnimPlaying;
    out->clip = a->clipCount > 0 ? a->current : -1;
    out->autoplay = a->autoplay;
    out->frame = a->frame;
    out->progress = a->progress;
    out->clipCount = a->clipCount;
    out->frameCount = a->frameCount;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_anim_set_clip(int32_t entity, int32_t index, const char* name, float fps, int32_t loop, const AverScene2dAnimFrame* frames, int32_t count) {
    if (!name || (count > 0 && !frames)) { setAbiError(AbiError::NullPointer); return 0; }
    if (!live(entity)) return 0;
    for (int32_t i = 0; i < count; ++i) {
        const AverScene2dAnimFrame& f = frames[i];
        if (!finiteAll(f.region, 4) || !std::isfinite(f.weight) || f.weight < 0 || f.region[2] < 0 || f.region[3] < 0) { setAbiError(AbiError::InvalidArgument); return 0; }
    }
    scene::World& w = scene::World::instance();
    if (!ids().anim) registerComponents(w);
    CSpriteAnim2D* a = attachAnim(w, static_cast<Entity>(entity));
    if (!a) { setAbiError(AbiError::AllocationFailed); return 0; }
    const bool ok = setClip(*a, index, name, fps, loop != 0, reinterpret_cast<const AnimFrame2D*>(frames), count);
    setAbiError(ok ? AbiError::Ok : AbiError::InvalidArgument);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_anim_get_clip(int32_t entity, int32_t index, uint8_t* outName, float* outFps, int32_t* outLoop, int32_t* outFrameCount) {
    const CSpriteAnim2D* a = animOfEntity(entity);
    if (!a) return 0;
    if (index < 0 || index >= a->clipCount) { setAbiError(AbiError::InvalidArgument); return 0; }
    const AnimClip2D& c = a->clips[index];
    if (outName) std::memcpy(outName, c.name, kClipNameBytes);
    if (outFps) *outFps = c.fps;
    if (outLoop) *outLoop = (c.flags & kClipNoLoop) ? 0 : 1;
    if (outFrameCount) *outFrameCount = c.count;
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_scene2d_anim_get_frames(int32_t entity, int32_t index, AverScene2dAnimFrame* outFrames, int32_t max) {
    const CSpriteAnim2D* a = animOfEntity(entity);
    if (!a) return -1;
    if (index < 0 || index >= a->clipCount) { setAbiError(AbiError::InvalidArgument); return -1; }
    const AnimClip2D& c = a->clips[index];
    const int32_t n = outFrames && max > 0 ? (c.count < max ? c.count : max) : 0;
    if (n > 0) std::memcpy(outFrames, &a->frames[c.first], sizeof(AnimFrame2D) * static_cast<size_t>(n));
    setAbiError(AbiError::Ok);
    return n;
}

int32_t aver_scene2d_anim_find(int32_t entity, const char* name) {
    const CSpriteAnim2D* a = animOfEntity(entity);
    return a ? findClip(*a, name) : -1;
}

int32_t aver_scene2d_anim_play(int32_t entity, const char* name, int32_t restart) {
    if (!animOfEntity(entity)) return 0;
    const bool ok = playClip(scene::World::instance(), static_cast<Entity>(entity), name, restart != 0);
    setAbiError(ok ? AbiError::Ok : AbiError::InvalidArgument);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_anim_stop(int32_t entity) {
    if (!animOfEntity(entity)) return 0;
    stopClip(scene::World::instance(), static_cast<Entity>(entity));
    return 1;
}

int32_t aver_scene2d_anim_pause(int32_t entity) {
    if (!animOfEntity(entity)) return 0;
    pauseClip(scene::World::instance(), static_cast<Entity>(entity));
    return 1;
}

int32_t aver_scene2d_anim_set_frame(int32_t entity, int32_t frame, float progress) {
    if (!animOfEntity(entity)) return 0;
    setClipFrame(scene::World::instance(), static_cast<Entity>(entity), frame, progress);
    return 1;
}

int32_t aver_scene2d_anim_poll(AverScene2dAnimEvent* outEvents, int32_t max) {
    if (!outEvents || max <= 0) return 0;
    return static_cast<int32_t>(drainAnimEvents(reinterpret_cast<AnimEvent*>(outEvents), static_cast<u32>(max)));
}

void aver_scene2d_tick(float dt) { tick(scene::World::instance(), dt); }

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
