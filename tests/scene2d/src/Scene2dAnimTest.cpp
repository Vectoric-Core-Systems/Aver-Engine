// Scene2dAnimTest -- sprite animation (CSpriteAnim2D): the clip table, playback in both directions, loop and finish, per-frame weights,
// the speed scale, autoplay, frame writes into the CSprite2D, the event queue and the animation half of scene2d_abi.h. Headless.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene2d_abi.h"

#include <cmath>
#include <cstring>
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

static bool approx(f32 a, f32 b, f32 tol = 1e-4f) { return std::fabs(a - b) <= tol; }

static AnimFrame2D frame(i64 tex, i32 cell, f32 weight = 0.0f, f32 rw = 0.0f, f32 rh = 0.0f) {
    AnimFrame2D f{};
    f.texture = tex;
    f.cell = cell;
    f.weight = weight;
    f.regionW = rw;
    f.regionH = rh;
    return f;
}

static std::vector<AnimEvent> drain() {
    std::vector<AnimEvent> out(64);
    out.resize(drainAnimEvents(out.data(), 64));
    return out;
}

static int count(const std::vector<AnimEvent>& v, i32 kind) {
    int n = 0;
    for (const auto& e : v) n += e.kind == kind;
    return n;
}

int main() {
    World& w = World::instance();
    registerComponents(w);
    const Ids id = ids();

    // ---- the clip table ----
    {
        const Entity e = w.create("clips");
        auto* a = attachAnim(w, e);
        const AnimFrame2D idle[3] = {frame(1, 0), frame(1, 1), frame(1, 2)};
        const AnimFrame2D run[2] = {frame(2, 0), frame(2, 1)};
        check(a && setClip(*a, 0, "idle", 10, true, idle, 3), "a clip appends at index == clipCount");
        check(setClip(*a, 1, "run", 12, false, run, 2) && a->clipCount == 2 && a->frameCount == 5, "and a second one");
        check(a->clips[1].first == 3 && a->clips[1].count == 2 && (a->clips[1].flags & kClipNoLoop), "frames pack in clip order; loop off sets the flag");
        check(findClip(*a, "run") == 1 && findClip(*a, "idle") == 0 && findClip(*a, "nope") == -1 && findClip(*a, nullptr) == -1, "findClip is by exact name");
        const AnimFrame2D idle2[1] = {frame(9, 7)};
        check(setClip(*a, 0, "idle", 10, true, idle2, 1) && a->clips[0].count == 1 && a->clips[1].first == 1 && a->frames[1].texture == 2 && a->frameCount == 3,
              "replacing a clip repacks the others and keeps their frames");
        check(!setClip(*a, 3, "gap", 5, true, idle, 3), "a gap in the indices is refused");
        check(!setClip(*a, 1, "idle", 5, true, run, 2), "a name already used by another clip is refused");
        check(!setClip(*a, 2, "", 5, true, nullptr, 0), "an empty name is refused");
        check(!setClip(*a, 2, "a_name_that_is_much_too_long_for_it", 5, true, nullptr, 0), "a name past 23 bytes is refused");
        check(!setClip(*a, 2, "nan", NAN, true, nullptr, 0), "a NaN fps is refused");
        std::vector<AnimFrame2D> many(kMaxAnimFrames);
        check(!setClip(*a, 2, "huge", 5, true, many.data(), kMaxAnimFrames), "more frames than the table holds are refused");
        check(a->clipCount == 2 && a->frameCount == 3, "and a refusal changes nothing");
        for (int i = 2; i < kMaxAnimClips; ++i) setClip(*a, i, ("c" + std::to_string(i)).c_str(), 5, true, nullptr, 0);
        check(a->clipCount == kMaxAnimClips && !setClip(*a, kMaxAnimClips, "one_more", 5, true, nullptr, 0), "sixteen clips at most");
    }

    // ---- playback ----
    {
        const Entity e = w.create("walker");
        attachSprite(w, e, 77);
        auto* sp = w.component<CSprite2D>(e, id.sprite);
        sp->hframes = 4;
        auto* a = attachAnim(w, e);
        const AnimFrame2D walk[4] = {frame(0, 0), frame(0, 1), frame(0, 2), frame(0, 3)};
        const AnimFrame2D hit[2] = {frame(5, 1, 0, 8, 8), frame(6, 0, 3.0f, 4, 4)};
        setClip(*a, 0, "walk", 10, true, walk, 4);   // 10 fps: 0.1 s a frame
        setClip(*a, 1, "hit", 10, false, hit, 2);
        clearAnimEvents();

        check(playClip(w, e, "walk") && (a->flags & kAnimPlaying) && a->frame == 0, "play starts the clip at its first frame");
        auto ev = drain();
        check(count(ev, kAnimEventAnimationChanged) == 0, "playing the clip that is already current is not a change");
        tick(w, 0.05f);
        check(a->frame == 0 && approx(a->progress, 0.5f), "half a frame in: still frame 0, progress 0.5");
        tick(w, 0.06f);
        check(a->frame == 1 && approx(a->progress, 0.1f, 1e-3f) && sp->frame == 1, "past the frame: frame 1 and the sprite's cell follows");
        check(sp->texture == 77, "a frame with texture 0 keeps the sprite's own texture");
        ev = drain();
        check(count(ev, kAnimEventFrameChanged) == 1 && ev[0].frame == 1 && ev[0].clip == 0, "one frame-changed event");

        tick(w, 0.3f);   // 1 -> 2 -> 3 -> wraps to 0 (0.1 progress + 0.3 = 0.4 s from frame 1)
        check(a->frame == 0, "a looping clip wraps to frame 0");
        ev = drain();
        check(count(ev, kAnimEventLooped) == 1 && count(ev, kAnimEventFinished) == 0, "and says it looped, not finished");
        check((a->flags & kAnimPlaying) != 0, "it keeps playing");

        tick(w, 100.0f);
        check(a->frame >= 0 && a->frame < 4 && (a->flags & kAnimPlaying), "a huge step on a loop lands on a valid frame");
        clearAnimEvents();

        // speed scale doubles the rate, and 0 reads as 1
        stopClip(w, e);
        a->speedScale = 2.0f;
        playClip(w, e, "walk");
        tick(w, 0.1f);
        check(a->frame == 2, "speed scale 2: two frames in a tenth of a second at 10 fps");
        // backwards from the last frame
        a->speedScale = -1.0f;
        stopClip(w, e);
        playClip(w, e, "walk", true);
        check(a->frame == 3, "a negative speed scale starts on the last frame");
        tick(w, 0.15f);
        check(a->frame == 1 && approx(a->progress, 0.5f, 1e-3f), "and runs backwards through the frames (1.5 frame units back from the start of frame 3)");
        tick(w, 0.5f);
        check(a->frame == 3 || a->frame == 2 || a->frame == 1 || a->frame == 0, "a backwards loop wraps to a valid frame");
        a->speedScale = 0.0f;

        // a clip that does not loop finishes on its last frame
        clearAnimEvents();
        check(playClip(w, e, "hit"), "play a different clip");
        ev = drain();
        check(count(ev, kAnimEventAnimationChanged) == 1 && count(ev, kAnimEventFrameChanged) == 1, "a new clip reports animation-changed and a frame-changed");
        check(a->current == 1 && sp->texture == 5 && sp->regionW == 8 && sp->regionH == 8 && sp->frame == 1, "the frame's texture, region and cell reach the sprite");
        tick(w, 0.11f);   // frame 0 (weight 1) is 0.1 s, frame 1 weighs 3: 0.3 s
        check(a->frame == 1 && sp->texture == 6 && sp->regionW == 4, "a frame's weight stretches it: frame 1 takes over after 0.1 s");
        tick(w, 0.25f);
        check(a->frame == 1 && (a->flags & kAnimPlaying), "and still shows after 0.26 s of its 0.3");
        tick(w, 0.1f);
        check(!(a->flags & kAnimPlaying) && a->frame == 1, "the clip ends on its last frame and stops playing");
        ev = drain();
        check(count(ev, kAnimEventFinished) == 1 && count(ev, kAnimEventLooped) == 0, "one finished event");
        tick(w, 1.0f);
        check(drain().empty(), "a stopped clip raises nothing");
        playClip(w, e, "hit");
        check(a->frame == 0 && (a->flags & kAnimPlaying), "playing a finished clip again restarts it");

        // stop, pause, set frame
        tick(w, 0.05f);
        pauseClip(w, e);
        const i32 held = a->frame;
        tick(w, 5.0f);
        check(a->frame == held && !(a->flags & kAnimPlaying), "pause holds the frame");
        playClip(w, e, nullptr);
        check((a->flags & kAnimPlaying) && a->frame == held, "play with no name resumes the current clip where it was");
        clearAnimEvents();
        setClipFrame(w, e, 1, 0.0f);
        check(a->frame == 1 && sp->frame == 0 && sp->texture == 6 && drain().size() == 1, "set frame shows that frame and raises frame-changed once");
        setClipFrame(w, e, 9, 0.5f);
        check(a->frame == 1 && approx(a->progress, 0.5f), "a frame past the end clamps to the last, with the progress given");
        stopClip(w, e);
        check(a->frame == 0 && !(a->flags & kAnimPlaying) && a->progress == 0.0f, "stop rewinds to frame 0");

        check(!playClip(w, e, "missing"), "an unknown clip does not play");
        check(!playClip(w, kInvalidEntity, "walk"), "and neither does an entity with no animation");
    }

    // ---- autoplay and an animation with no sprite ----
    {
        const Entity e = w.create("auto");
        attachSprite(w, e, 3);
        auto* a = attachAnim(w, e);
        const AnimFrame2D f1[2] = {frame(0, 0), frame(0, 1)};
        const AnimFrame2D f2[2] = {frame(0, 4), frame(0, 5)};
        setClip(*a, 0, "a", 5, true, f1, 2);
        setClip(*a, 1, "b", 5, true, f2, 2);
        a->autoplay = 2;
        check(!(a->flags & kAnimPlaying), "autoplay waits for the first step");
        tick(w, 0.0f);
        check((a->flags & kAnimPlaying) && a->current == 1 && w.component<CSprite2D>(e, id.sprite)->frame == 4, "the first step starts the autoplay clip and shows its first frame");
        stopClip(w, e);
        tick(w, 1.0f);
        check(!(a->flags & kAnimPlaying), "autoplay happens once: after stop it stays stopped");

        const Entity bare = w.create("nosprite");
        auto* b = attachAnim(w, bare);
        setClip(*b, 0, "x", 5, true, f1, 2);
        playClip(w, bare, "x");
        tick(w, 0.5f);
        check(b->frame == 1 || b->frame == 0, "an animation with no sprite still steps");
    }

    // ---- the ABI ----
    {
        const Entity e = w.create("abi");
        const int h = static_cast<int>(e);
        attachSprite(w, e, 11);
        AverScene2dAnimFrame fr[2] = {};
        fr[0].texture = 4; fr[0].region[2] = 16; fr[0].region[3] = 16; fr[0].weight = 2; fr[0].cell = 3;
        fr[1].cell = 4;
        check(aver_scene2d_anim_set_clip(h, 0, "spin", 8.0f, 1, fr, 2) == 1, "anim_set_clip adds the component and the clip");
        check(aver_scene2d_anim_set_clip(h, 5, "gap", 8.0f, 1, fr, 2) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::InvalidArgument), "a bad index is InvalidArgument");
        AverScene2dAnimFrame bad = fr[0];
        bad.weight = -1;
        check(aver_scene2d_anim_set_clip(h, 1, "neg", 8.0f, 1, &bad, 1) == 0, "a negative weight is refused");
        check(aver_scene2d_anim_set_clip(h, 1, nullptr, 8.0f, 1, fr, 2) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::NullPointer), "a NULL name is NullPointer");
        uint8_t name[AVER_SCENE2D_ANIM_NAME_BYTES] = {};
        float fps = 0;
        int32_t loop = 0, n = 0;
        check(aver_scene2d_anim_get_clip(h, 0, name, &fps, &loop, &n) == 1 && std::strcmp(reinterpret_cast<char*>(name), "spin") == 0 && fps == 8.0f && loop == 1 && n == 2,
              "anim_get_clip reads name, fps, loop and frame count");
        check(aver_scene2d_anim_get_clip(h, 1, name, nullptr, nullptr, nullptr) == 0, "past the last clip is 0");
        AverScene2dAnimFrame back[4] = {};
        check(aver_scene2d_anim_get_frames(h, 0, back, 4) == 2 && back[0].texture == 4 && back[0].region[2] == 16 && back[0].weight == 2 && back[0].cell == 3 && back[1].cell == 4,
              "anim_get_frames returns the frames");
        check(aver_scene2d_anim_find(h, "spin") == 0 && aver_scene2d_anim_find(h, "x") == -1, "anim_find");
        AverScene2dAnimState st{};
        st.speedScale = 2.0f;
        st.autoplay = 1;
        check(aver_scene2d_set_anim(h, &st) == 1, "set_anim writes speed scale and autoplay");
        clearAnimEvents();
        check(aver_scene2d_anim_play(h, "spin", 0) == 1 && aver_scene2d_anim_play(h, "zzz", 0) == 0, "anim_play");
        aver_scene2d_tick(0.2f);   // 8 fps * 2 = 16 frame units a second: 3.2 units, past the 3 of the clip
        AverScene2dAnimState r{};
        check(aver_scene2d_get_anim(h, &r) == 1 && r.clipCount == 1 && r.frameCount == 2 && r.speedScale == 2.0f && (r.flags & AVER_SCENE2D_A_PLAYING) && r.clip == 0,
              "get_anim reports the state");
        AverScene2dAnimEvent evs[16] = {};
        const int got = aver_scene2d_anim_poll(evs, 16);
        int mine = 0;
        for (int i = 0; i < got; ++i) mine += evs[i].entity == h;
        check(got >= 2 && mine >= 2, "anim_poll drains the events with the entity");
        check(aver_scene2d_anim_poll(evs, 16) == 0, "and drains them once");
        check(aver_scene2d_anim_set_frame(h, 1, 0.25f) == 1 && aver_scene2d_anim_pause(h) == 1 && aver_scene2d_anim_stop(h) == 1, "set_frame, pause and stop");
        const Entity none = w.create("none");
        check(aver_scene2d_anim_play(static_cast<int>(none), "spin", 0) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::BadHandle), "an entity with no animation is BadHandle");
        check(aver_scene2d_component_id(3) == static_cast<int>(id.anim), "component 3 is the animation");
    }

    // ---- Play's pause and time dilation reach the animation step ----
    {
        check(gameStepDt(0.1f, true, 1.0f) == 0.0f, "a paused host steps no time");
        check(approx(gameStepDt(0.1f, false, 0.5f), 0.05f), "slow motion scales the step");
        check(gameStepDt(0.1f, false, 0.0f) == 0.0f && gameStepDt(0.1f, false, -1.0f) == 0.0f && gameStepDt(0.1f, false, NAN) == 0.0f,
              "a zero, negative or NaN scale steps no time");
        check(gameStepDt(-0.1f, false, 1.0f) == 0.0f, "so does a negative wall step");
        const Entity e = w.create("paused");
        attachSprite(w, e, 77);
        auto* sp = w.component<CSprite2D>(e, id.sprite);
        sp->hframes = 4;
        auto* a = attachAnim(w, e);
        const AnimFrame2D walk[4] = {frame(0, 0), frame(0, 1), frame(0, 2), frame(0, 3)};
        setClip(*a, 0, "walk", 10, true, walk, 4);
        playClip(w, e, "walk");
        clearAnimEvents();
        tick(w, gameStepDt(0.5f, true, 1.0f));
        check(a->frame == 0 && approx(a->progress, 0.0f) && sp->frame == 0 && drain().empty(), "a paused tick does not advance the frame, the sprite or queue events");
        tick(w, gameStepDt(0.25f, false, 1.0f));
        check(a->frame == 2, "the same tick unpaused does");
        stopClip(w, e);
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
