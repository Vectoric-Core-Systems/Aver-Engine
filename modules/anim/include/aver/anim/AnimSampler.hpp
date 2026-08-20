// Sampling an .ocanim clip into a pose, and a small player that owns a clip's clock.
#pragma once

#include "aver/anim/Pose.hpp"

namespace aver::anim {

// Wraps a time into the clip for a looping clip, clamps it for a one-shot. A zero-length clip
// always returns 0.
f32 clipTime(const fmt::OcAnimation& clip, f32 seconds);

// Samples `clip` at `seconds` into `inOut`.
//
// Bones with no track in this clip are LEFT ALONE, which is why the caller seeds `inOut` with
// restPose: a clip that animates an arm should not collapse the legs to the identity. `seconds` is
// taken as given -- run it through clipTime first if you want looping.
//
// PER-KEY VALUE LAYOUT, which the format header did not pin down and nothing had ever read: channel
// -major, in the order translation(3), rotation(4), scale(3), skipping absent channels. Under
// CubicSpline each present channel's block is its in-tangent, value and out-tangent back to back,
// per FORMAT_SPECS.md 9.2 ("each key = Time + inTangent + value + outTangent").
void sampleAnimation(const fmt::OcAnimation& clip, f32 seconds, Pose& inOut);

// How a clock step is travelling through a clip. Everything notifiesCrossed needs that it cannot
// work out from the two times alone -- a step from 1.9 to 0.1 in a 2 s clip is a forward wrap, a
// backward scrub, or a whole loop plus a bit, and those fire different notifies.
struct ClipStep {
    f32  prev = 0.0f;          // where the clock was, already wrapped into [0, duration]
    f32  now = 0.0f;           // where it is, likewise wrapped
    bool forward = true;       // the SIGN OF THE STEP, not of `now - prev`
    bool sweptWholeClip = false;   // one step covered >= the clip: everything is crossed, once
    // THE FIRST STEP OF A CLIP INCLUDES ITS OWN START. Every other step uses a half-open interval
    // -- (prev, now] going forward -- because prev was already fired last step and firing it again
    // is a double-fire. But the first step has no last step, so a half-open rule would make a notify
    // AT TIME ZERO the one notify that never fires on play, which is exactly where an author puts
    // "the swing starts". Loop wraps do not need this flag: their second interval already includes 0.
    bool inclusiveStart = false;
};

// Appends the INDEX of every notify in `clip` that `step` crossed, in the clip's own file order.
//
// Indices rather than names or pointers, so a caller can decide for itself whether to copy the name,
// and so this stays usable if notifies ever grow a payload. Appends rather than assigns, so one
// scratch vector can serve a whole tick's worth of entities.
//
// A notify outside [0, duration] -- which the format permits, since nothing on write clamps it -- is
// clamped here rather than dropped. An author who drags a marker past the end of a clip and then
// shortens the clip should get the event at the end, not silence.
void notifiesCrossed(const fmt::OcAnimation& clip, const ClipStep& step, std::vector<u32>& outIndices);

// Plays one clip against a skeleton, and crossfades when the clip changes.
//
// It holds POINTERS to clips it does not own; whatever owns the assets must outlive the player.
class AnimPlayer {
public:
    // Starts `clip`, crossfading from whatever is playing over `fadeSeconds`. Passing the clip that
    // is already playing does nothing, so calling this every frame from gameplay is safe.
    void play(const fmt::OcAnimation* clip, f32 fadeSeconds = 0.2f);
    // Cuts to `clip` with no fade and no memory of what came before.
    void reset(const fmt::OcAnimation* clip);
    // Advances the clock, and the crossfade with it.
    void advance(f32 dt);
    // Writes the current pose. Seeds from the skeleton's rest pose, so untracked bones hold rest.
    void evaluate(const fmt::OcSkeleton& skel, Pose& out) const;

    void  setSpeed(f32 s) { speed_ = s; }
    f32   speed() const { return speed_; }
    f32   time() const { return time_; }
    bool  fading() const { return fade_ < 1.0f && prev_ != nullptr; }
    const fmt::OcAnimation* clip() const { return clip_; }

private:
    const fmt::OcAnimation* clip_ = nullptr;
    const fmt::OcAnimation* prev_ = nullptr;
    f32 time_ = 0.0f;
    f32 prevTime_ = 0.0f;     // frozen at the swap: the outgoing clip stops advancing
    f32 fade_ = 1.0f;         // 0 at the swap, 1 when the incoming clip owns the pose
    f32 fadeRate_ = 0.0f;     // per second; 0 means an instant cut
    f32 speed_ = 1.0f;
};

} // namespace aver::anim
