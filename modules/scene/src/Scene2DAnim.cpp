// Sprite animation (CSpriteAnim2D, Scene2D.hpp): the clip table, playback, the event queue and the frame step.
#include "aver/scene/Scene2D.hpp"

#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace aver::scene2d {

namespace {

constexpr usize kMaxEvents = 4096;

std::deque<AnimEvent>& events() {
    static std::deque<AnimEvent> q;
    return q;
}

void emit(Entity e, i32 kind, const CSpriteAnim2D& a) {
    auto& q = events();
    if (q.size() >= kMaxEvents) q.pop_front();
    q.push_back({e, kind, a.current, a.frame});
}

CSpriteAnim2D* animOf(scene::World& w, Entity e) {
    const Ids id = ids();
    return id.anim ? w.component<CSpriteAnim2D>(e, id.anim) : nullptr;
}

bool validClip(const CSpriteAnim2D& a) {
    return a.current >= 0 && a.current < a.clipCount && a.clips[a.current].count > 0;
}

f32 clipFps(const AnimClip2D& c) { return std::isfinite(c.fps) && c.fps > 0.0f ? c.fps : 5.0f; }
f32 weightOf(const AnimFrame2D& f) { return std::isfinite(f.weight) && f.weight > 0.0f ? f.weight : 1.0f; }
f32 speedOf(const CSpriteAnim2D& a) { return std::isfinite(a.speedScale) && a.speedScale != 0.0f ? a.speedScale : 1.0f; }

const AnimFrame2D& frameAt(const CSpriteAnim2D& a, const AnimClip2D& c, i32 i) { return a.frames[c.first + std::clamp(i, 0, c.count - 1)]; }

void applyFrame(scene::World& w, Entity e, const CSpriteAnim2D& a) {
    if (!validClip(a)) return;
    const Ids id = ids();
    auto* sp = id.sprite ? w.component<CSprite2D>(e, id.sprite) : nullptr;
    if (!sp) return;
    const AnimFrame2D& f = frameAt(a, a.clips[a.current], a.frame);
    if (f.texture != 0) sp->texture = f.texture;
    sp->regionX = f.regionX; sp->regionY = f.regionY; sp->regionW = f.regionW; sp->regionH = f.regionH;
    sp->frame = f.cell;
}

// Moves the playhead by `remaining` frame-weight units, queuing the events on the way.
void advance(Entity e, CSpriteAnim2D& a, f32 remaining) {
    const AnimClip2D& c = a.clips[a.current];
    const bool loop = !(c.flags & kClipNoLoop);
    const i32 n = c.count;
    a.frame = std::clamp(a.frame, 0, n - 1);
    a.progress = std::clamp(std::isfinite(a.progress) ? a.progress : 0.0f, 0.0f, 1.0f);
    if (loop) {   // whole turns of the clip change nothing visible
        f32 total = 0.0f;
        for (i32 i = 0; i < n; ++i) total += weightOf(frameAt(a, c, i));
        if (std::fabs(remaining) > total * 2.0f) remaining = std::fmod(remaining, total);
    }
    for (int guard = 0; remaining != 0.0f && guard < 4 * n + 8; ++guard) {
        const f32 weight = weightOf(frameAt(a, c, a.frame));
        if (remaining > 0.0f) {
            const f32 left = (1.0f - a.progress) * weight;
            if (remaining < left) { a.progress += remaining / weight; return; }
            remaining -= left;
            a.progress = 0.0f;
            if (a.frame + 1 < n) { ++a.frame; emit(e, kAnimEventFrameChanged, a); continue; }
            if (loop) {
                a.frame = 0;
                emit(e, kAnimEventLooped, a);
                if (n > 1) emit(e, kAnimEventFrameChanged, a);
                continue;
            }
            a.flags &= ~kAnimPlaying;
            emit(e, kAnimEventFinished, a);
            return;
        }
        const f32 used = a.progress * weight;
        if (-remaining < used) { a.progress += remaining / weight; return; }
        remaining += used;
        a.progress = 1.0f;
        if (a.frame > 0) { --a.frame; emit(e, kAnimEventFrameChanged, a); continue; }
        if (loop) {
            a.frame = n - 1;
            emit(e, kAnimEventLooped, a);
            if (n > 1) emit(e, kAnimEventFrameChanged, a);
            continue;
        }
        a.progress = 0.0f;
        a.flags &= ~kAnimPlaying;
        emit(e, kAnimEventFinished, a);
        return;
    }
}

} // namespace

bool setClip(CSpriteAnim2D& a, i32 index, const char* name, f32 fps, bool loop, const AnimFrame2D* frames, i32 count) {
    if (index < 0 || index > a.clipCount || index >= kMaxAnimClips || !name || !*name || count < 0 || (count > 0 && !frames)) return false;
    if (std::strlen(name) >= static_cast<usize>(kClipNameBytes) || !std::isfinite(fps) || fps < 0.0f) return false;
    for (i32 i = 0; i < a.clipCount; ++i)
        if (i != index && std::strcmp(a.clips[i].name, name) == 0) return false;
    // Repack: every clip's frames in clip order, the new ones in place of the old.
    i32 total = count;
    for (i32 i = 0; i < a.clipCount; ++i)
        if (i != index) total += a.clips[i].count;
    if (total > kMaxAnimFrames) return false;

    AnimClip2D oldClips[kMaxAnimClips];
    std::memcpy(oldClips, a.clips, sizeof oldClips);
    static thread_local AnimFrame2D oldFrames[kMaxAnimFrames];
    std::memcpy(oldFrames, a.frames, sizeof oldFrames);

    const i32 clipCount = std::max(a.clipCount, index + 1);
    i32 at = 0;
    for (i32 i = 0; i < clipCount; ++i) {
        AnimClip2D& c = a.clips[i];
        if (i == index) {
            std::memset(&c, 0, sizeof c);
            std::memcpy(c.name, name, std::strlen(name));
            c.fps = fps;
            c.flags = loop ? 0u : kClipNoLoop;
            c.first = at;
            c.count = count;
            if (count > 0) std::memcpy(&a.frames[at], frames, sizeof(AnimFrame2D) * static_cast<usize>(count));
            at += count;
        } else {
            c = oldClips[i];
            const i32 n = c.count;
            if (n > 0) std::memcpy(&a.frames[at], &oldFrames[oldClips[i].first], sizeof(AnimFrame2D) * static_cast<usize>(n));
            c.first = at;
            at += n;
        }
    }
    if (at < kMaxAnimFrames) std::memset(&a.frames[at], 0, sizeof(AnimFrame2D) * static_cast<usize>(kMaxAnimFrames - at));
    a.clipCount = clipCount;
    a.frameCount = at;
    if (a.current == index && a.clips[index].count > 0) a.frame = std::clamp(a.frame, 0, a.clips[index].count - 1);
    return true;
}

i32 findClip(const CSpriteAnim2D& a, const char* name) {
    if (!name) return -1;
    for (i32 i = 0; i < a.clipCount; ++i)
        if (std::strcmp(a.clips[i].name, name) == 0) return i;
    return -1;
}

bool playClip(scene::World& w, Entity e, const char* name, bool restart) {
    CSpriteAnim2D* a = animOf(w, e);
    if (!a) return false;
    i32 idx = a->current;
    if (name && *name) idx = findClip(*a, name);
    if (idx < 0 || idx >= a->clipCount || a->clips[idx].count <= 0) return false;
    const AnimClip2D& c = a->clips[idx];
    const bool backwards = speedOf(*a) < 0.0f;
    const bool atEnd = !(c.flags & kClipNoLoop) ? false : (backwards ? a->frame == 0 : a->frame == c.count - 1);
    const bool changed = idx != a->current;
    if (changed || restart || (!(a->flags & kAnimPlaying) && atEnd)) {
        a->current = idx;
        a->frame = backwards ? c.count - 1 : 0;
        a->progress = 0.0f;
    }
    a->flags |= kAnimPlaying | kAnimStarted;
    if (changed) emit(e, kAnimEventAnimationChanged, *a);
    if (changed || restart) emit(e, kAnimEventFrameChanged, *a);
    applyFrame(w, e, *a);
    return true;
}

void stopClip(scene::World& w, Entity e) {
    CSpriteAnim2D* a = animOf(w, e);
    if (!a) return;
    a->flags = (a->flags & ~kAnimPlaying) | kAnimStarted;
    if (a->frame != 0) { a->frame = 0; emit(e, kAnimEventFrameChanged, *a); }
    a->progress = 0.0f;
    applyFrame(w, e, *a);
}

void pauseClip(scene::World& w, Entity e) {
    if (CSpriteAnim2D* a = animOf(w, e)) a->flags = (a->flags & ~kAnimPlaying) | kAnimStarted;
}

void setClipFrame(scene::World& w, Entity e, i32 frame, f32 progress) {
    CSpriteAnim2D* a = animOf(w, e);
    if (!a || !validClip(*a)) return;
    const i32 f = std::clamp(frame, 0, a->clips[a->current].count - 1);
    const bool changed = f != a->frame;
    a->frame = f;
    a->progress = std::clamp(std::isfinite(progress) ? progress : 0.0f, 0.0f, 1.0f);
    if (changed) emit(e, kAnimEventFrameChanged, *a);
    applyFrame(w, e, *a);
}

u32 drainAnimEvents(AnimEvent* out, u32 max) {
    auto& q = events();
    u32 n = 0;
    while (n < max && !q.empty() && out) {
        out[n++] = q.front();
        q.pop_front();
    }
    return n;
}

void clearAnimEvents() { events().clear(); }

void stepAnimations(scene::World& w, f32 dt) {
    const Ids id = ids();
    if (!id.anim) return;
    scene::ComponentPool* pool = w.pool(id.anim);
    if (!pool) return;
    const f32 step = std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    for (usize i = 0; i < pool->size(); ++i) {
        const Entity e = pool->entityAt(i);
        auto& a = *static_cast<CSpriteAnim2D*>(pool->dataAt(i));
        if (!(a.flags & kAnimStarted)) {
            a.flags |= kAnimStarted;
            if (a.autoplay > 0 && a.autoplay <= a.clipCount && a.clips[a.autoplay - 1].count > 0) {
                a.current = a.autoplay - 1;
                a.frame = speedOf(a) < 0.0f ? a.clips[a.current].count - 1 : 0;
                a.progress = 0.0f;
                a.flags |= kAnimPlaying;
            }
        }
        if (step > 0.0f && (a.flags & kAnimPlaying) && validClip(a))
            advance(e, a, step * clipFps(a.clips[a.current]) * speedOf(a));
        applyFrame(w, e, a);
    }
}

} // namespace aver::scene2d
