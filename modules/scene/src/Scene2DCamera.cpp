// 2D camera, parallax and the frame step (CCamera2D, CParallax2D, tick in Scene2D.hpp).
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>

namespace aver::scene2d {

namespace {

Entity g_lastCurrent = scene::kInvalidEntity;

f32 fin(f32 v, f32 def = 0.0f) { return std::isfinite(v) ? v : def; }
f32 zoomOf(f32 z) { return std::isfinite(z) && z > 0.0f ? z : 1.0f; }

CCamera2D* camOf(scene::World& w, Entity e) {
    const Ids id = ids();
    return id.camera && e != scene::kInvalidEntity ? w.component<CCamera2D>(e, id.camera) : nullptr;
}

// Where the camera wants its centre: its transform plus the offset (plus half a view when the transform is the top-left corner).
void target(scene::World& w, Entity e, const CCamera2D& c, f32 cw, f32 ch, f32 out[2]) {
    const Affine2 m = worldAffine(w, e);
    f32 x = m.tx + fin(c.offsetX), y = m.ty + fin(c.offsetY);
    if (c.flags & kCamAnchorTopLeft) {
        x += cw * 0.5f / zoomOf(c.zoomX);
        y += ch * 0.5f / zoomOf(c.zoomY);
    }
    out[0] = x;
    out[1] = y;
}

} // namespace

Entity currentCamera(scene::World& w) {
    const Ids id = ids();
    scene::ComponentPool* pool = id.camera ? w.pool(id.camera) : nullptr;
    if (!pool) return scene::kInvalidEntity;
    Entity first = scene::kInvalidEntity;
    for (usize i = 0; i < pool->size(); ++i) {
        const auto& c = *static_cast<const CCamera2D*>(pool->dataAt(i));
        if (c.flags & kCamDisabled) continue;
        if (c.flags & kCamCurrent) return pool->entityAt(i);
        if (first == scene::kInvalidEntity) first = pool->entityAt(i);
    }
    return first;
}

bool makeCameraCurrent(scene::World& w, Entity e) {
    CCamera2D* target = camOf(w, e);
    if (!target) return false;
    const Ids id = ids();
    scene::ComponentPool* pool = w.pool(id.camera);
    for (usize i = 0; pool && i < pool->size(); ++i) static_cast<CCamera2D*>(pool->dataAt(i))->flags &= ~kCamCurrent;
    target->flags = (target->flags | kCamCurrent) & ~kCamDisabled;
    target->centerValid = 0;   // the view jumps to the new camera, easing from nowhere
    return true;
}

void cameraCenter(scene::World& w, Entity e, f32 cw, f32 ch, f32 out[2]) {
    out[0] = out[1] = 0.0f;
    const CCamera2D* c = camOf(w, e);
    if (!c) return;
    updateTransforms(w);
    if (c->centerValid) { out[0] = fin(c->centerX); out[1] = fin(c->centerY); }
    else target(w, e, *c, cw, ch, out);
    const f32 hw = cw * 0.5f / zoomOf(c->zoomX), hh = ch * 0.5f / zoomOf(c->zoomY);
    if ((c->flags & kCamLimitRight) && out[0] + hw > c->limitRight) out[0] = c->limitRight - hw;
    if ((c->flags & kCamLimitBottom) && out[1] + hh > c->limitBottom) out[1] = c->limitBottom - hh;
    if ((c->flags & kCamLimitLeft) && out[0] - hw < c->limitLeft) out[0] = c->limitLeft + hw;
    if ((c->flags & kCamLimitTop) && out[1] - hh < c->limitTop) out[1] = c->limitTop + hh;
}

Affine2 cameraView(scene::World& w, f32 cw, f32 ch) {
    const Entity e = currentCamera(w);
    const CCamera2D* c = camOf(w, e);
    if (!c || !(cw > 0.0f) || !(ch > 0.0f)) return {};
    updateTransforms(w);
    f32 ctr[2];
    cameraCenter(w, e, cw, ch, ctr);
    const f32 zx = zoomOf(c->zoomX), zy = zoomOf(c->zoomY);
    Affine2 rot;
    if (c->flags & kCamFollowRotation) {
        const Affine2 m = worldAffine(w, e);
        const f32 r = std::atan2(m.b, m.a), cs = std::cos(r), sn = std::sin(r);
        rot = {cs, -sn, sn, cs, 0, 0};   // the inverse of the camera's turn
    }
    const Affine2 scale{zx, 0, 0, zy, 0, 0};
    const Affine2 toCentre{1, 0, 0, 1, cw * 0.5f, ch * 0.5f};
    const Affine2 fromCentre{1, 0, 0, 1, -ctr[0], -ctr[1]};
    return toCentre * rot * scale * fromCentre;
}

ParallaxShift parallaxShift(const CParallax2D& p, f32 camX, f32 camY) {
    ParallaxShift out;
    out.followsCamera = !(p.flags & kParallaxNoFollow);
    f32 cx = out.followsCamera ? fin(camX) : 0.0f, cy = out.followsCamera ? fin(camY) : 0.0f;
    if (p.flags & kParallaxLimitBegin) { cx = std::max(cx, p.limitBeginX); cy = std::max(cy, p.limitBeginY); }
    if (p.flags & kParallaxLimitEnd) { cx = std::min(cx, p.limitEndX); cy = std::min(cy, p.limitEndY); }
    f32 dx = cx * fin(p.pinX) + fin(p.offsetX) + fin(p.timeX);
    f32 dy = cy * fin(p.pinY) + fin(p.offsetY) + fin(p.timeY);
    const i32 times = std::clamp(p.repeatTimes > 0 ? p.repeatTimes : 1, 1, 8);
    const auto wrap = [](f32 d, f32 cam, f32 rep) {   // keep the layer within one tile of the view, however far the camera went
        const f32 s = d - cam;
        return s - rep * std::floor(s / rep) + cam;
    };
    if (std::isfinite(p.repeatX) && p.repeatX > 0.0f) { dx = wrap(dx, cx, p.repeatX); out.repeatX = p.repeatX; out.timesX = times; }
    if (std::isfinite(p.repeatY) && p.repeatY > 0.0f) { dy = wrap(dy, cy, p.repeatY); out.repeatY = p.repeatY; out.timesY = times; }
    out.dx = dx;
    out.dy = dy;
    return out;
}

namespace {

void stepParallax(scene::World& w, f32 dt) {
    const Ids id = ids();
    scene::ComponentPool* pool = id.parallax ? w.pool(id.parallax) : nullptr;
    if (!pool || !(dt > 0.0f)) return;
    for (usize i = 0; i < pool->size(); ++i) {
        auto& p = *static_cast<CParallax2D*>(pool->dataAt(i));
        p.timeX = fin(p.timeX) + fin(p.autoX) * dt;
        p.timeY = fin(p.timeY) + fin(p.autoY) * dt;
        // The time only matters modulo the tile, and a float that grows for hours loses its fraction.
        if (p.repeatX > 0.0f) p.timeX = std::fmod(p.timeX, p.repeatX);
        if (p.repeatY > 0.0f) p.timeY = std::fmod(p.timeY, p.repeatY);
    }
}

void stepCamera(scene::World& w, f32 dt) {
    const Entity e = currentCamera(w);
    if (e != g_lastCurrent) {
        if (CCamera2D* old = camOf(w, g_lastCurrent)) old->centerValid = 0;
        if (CCamera2D* now = camOf(w, e)) now->centerValid = 0;
        g_lastCurrent = e;
    }
    CCamera2D* c = camOf(w, e);
    if (!c) return;
    const CanvasTransform ct = canvasTransform();
    const f32 cw = ct.valid ? ct.stretch.contentSize.x : 0.0f, ch = ct.valid ? ct.stretch.contentSize.y : 0.0f;
    f32 t[2];
    target(w, e, *c, cw, ch, t);
    if (!c->centerValid) {
        c->centerX = t[0];
        c->centerY = t[1];
        c->centerValid = 1;
        return;
    }
    const f32 hw = cw * 0.5f / zoomOf(c->zoomX), hh = ch * 0.5f / zoomOf(c->zoomY);
    f32 goalX = t[0], goalY = t[1];
    if (c->flags & kCamDragH) {
        const f32 l = std::clamp(fin(c->dragLeft), 0.0f, 1.0f) * hw, r = std::clamp(fin(c->dragRight), 0.0f, 1.0f) * hw;
        goalX = t[0] < c->centerX - l ? t[0] + l : (t[0] > c->centerX + r ? t[0] - r : c->centerX);
    }
    if (c->flags & kCamDragV) {
        const f32 u = std::clamp(fin(c->dragTop), 0.0f, 1.0f) * hh, d = std::clamp(fin(c->dragBottom), 0.0f, 1.0f) * hh;
        goalY = t[1] < c->centerY - u ? t[1] + u : (t[1] > c->centerY + d ? t[1] - d : c->centerY);
    }
    if ((c->flags & kCamSmoothing) && dt > 0.0f) {
        const f32 speed = c->smoothSpeed > 0.0f && std::isfinite(c->smoothSpeed) ? c->smoothSpeed : 5.0f;
        const f32 k = std::min(1.0f, speed * dt);
        c->centerX += (goalX - c->centerX) * k;
        c->centerY += (goalY - c->centerY) * k;
    } else {
        c->centerX = goalX;
        c->centerY = goalY;
    }
}

} // namespace

void tick(scene::World& w, f32 dt) {
    const f32 step = std::isfinite(dt) && dt > 0.0f ? std::min(dt, 1.0f) : 0.0f;
    updateTransforms(w);
    stepAnimations(w, step);
    stepParallax(w, step);
    stepCamera(w, step);
}

} // namespace aver::scene2d
