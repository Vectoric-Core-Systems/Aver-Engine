#pragma once
// Property-track animation with animation-player semantics.
//
// The clip is aver::fmt::OcAnimation with its propTracks (the optional PROP chunk of .ocanim). This header is the
// pure, GPU-free half: sampling one track, finding the keys a playhead passed, and PropPlayer, the playback state
// machine (play, queue, crossfade, seek, signals, loop modes, weight-normalised blending). It knows nothing about
// nodes or entities. Each track names a target by path; the host resolves the path and applies the blended values the
// player hands back, and runs the method / audio / sub-player calls the player reports, in order.
#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aver::anim {

using PropValue = fmt::OcPropValue;
using fmt::OcPropKind;
using fmt::OcPropTrackType;

inline constexpr double kPropEps = 1e-5;
inline constexpr double kPropTau = 6.283185307179586476925286766559;

// The easing of a segment: c = 1 linear, c > 1 slow start, 0 < c < 1 slow end, c < 0 in-out, c = 0 a step.
double propEase(double x, double c);

// ---- one track ----

// True for the kinds whose values blend and interpolate numerically.
bool propInterpolable(OcPropKind k);

// The value of a value / transform / blend-shape track at `time`, with the clip's loop mode and length. `ok` is false for
// an empty track or a kind with no value. `backward` only matters for which key's easing curve a segment uses.
struct PropSample { bool ok = false; PropValue value; };
PropSample propSample(const fmt::OcAnimation& clip, const fmt::OcPropTrack& t, double time, bool backward = false);
// A bezier track at `time` (cubic bezier segments, handles clamped into the segment).
double propSampleBezier(const fmt::OcPropTrack& t, double time);

enum class PropFind : u8 { Exact, Nearest };
// Index of the key at `time` (Exact: within tolerance; Nearest: the last key at or before it, or going backward the first at or
// after it), -1 when there is none or the key lies outside the clip.
int propFindKey(const fmt::OcPropTrack& t, double time, PropFind mode, bool backward, double length);

// A piece of playhead movement. forward when b >= a. `incStart` includes a key sitting exactly at `a` (a loop restart).
struct PropSegment { double a = 0, b = 0; bool incStart = false; };
// Keys passed by the movement, in the order the playhead met them: forward (a, b], backward [b, a), an `incStart` segment
// includes `a` too. Appends to `out`.
void propKeysInSegments(const fmt::OcPropTrack& t, const std::vector<PropSegment>& segs, std::vector<int>& out);

// ---- the player ----

enum class PropDiscrete : u8 { Dominant, Recessive, ForceContinuous };

// What the player reports, in the order it happened. The host turns these into signals, calls and property writes.
struct PropEvent {
    enum Type : u8 {
        Started,        // a: animation  (animation_started)
        Finished,       // a: animation  (animation_finished)
        Changed,        // a: old, b: new (animation_changed: a queued animation took over)
        CurrentChanged, // a: new current animation, "" after stop (current_animation_changed)
        Looped,         // a: animation (Aver extension: the playhead wrapped or bounced)
        Method,         // a: method name, path: target, args
        AudioPlay,      // path: target, a: stream, t0: offset into the stream (s), t1: end offset (s)
        AudioStop,      // path: target
        AnimPlay,       // path: target player, a: animation, t0: seconds into it (a seek), t1: 1 when t0 is a seek
        AnimStop,       // path: target player
        Applied,        // the blended values (PropPlayer::outputs) are ready to be written now
        MixerApplied,   // after the write and the signals of this pass (mixer_applied)
    };
    Type type = Started;
    std::string a, b, path;
    std::vector<PropValue> args;
    double t0 = 0, t1 = 0;
};

// One blended value for a track target. `discrete` outputs are single key writes made as the playhead passed the key.
struct PropOutput {
    u32 id = 0;                      // PropPlayer::trackType / trackPath
    PropValue value;
    bool discrete = false;
};

// What the root-motion track (PropPlayer::setRootMotionPath) produced in the last pass, in the clip's (engine) space. The deltas are what
// the blended animations moved the track since the previous pass (position and scale add, rotation composes); the accumulators are the
// blended absolute values at the current playheads. A part with no track of its kind stays at its identity.
struct PropRootMotion {
    bool hasPosition = false, hasRotation = false, hasScale = false;
    double position[3] = {0, 0, 0}, rotation[4] = {0, 0, 0, 1}, scale[3] = {0, 0, 0};
    double positionAccum[3] = {0, 0, 0}, rotationAccum[4] = {0, 0, 0, 1}, scaleAccum[3] = {0, 0, 0};
};

// How one animation instance of a tree pass moved: where its playhead is now, whether it wrapped or bounced (a ping-pong bounce flips the
// direction of play, `flipped` says the number of bounces was odd), and whether the key is known.
struct PropTreeStep { bool ok = false; double next = 0; bool looped = false, flipped = false; };

class PropPlayer {
public:
    using Clip = std::shared_ptr<fmt::OcAnimation>;

    // Library: clips by their full key ("walk" or "lib/walk"). The host decides what a library is.
    void setClip(const std::string& key, Clip clip);
    bool removeClip(const std::string& key);                 // stops playback when it is the current clip
    Clip clip(const std::string& key) const;
    std::vector<std::string> clipKeys() const;               // insertion order

    void setBlendTime(const std::string& from, const std::string& to, double sec);
    // The lookup: from->to, then *->to, then from->*, "" is the wildcard; -1 when none is set.
    double explicitBlendTime(const std::string& from, const std::string& to) const;
    // That, or 0 when none is set (the blend time; play() falls back to default_blend_time itself).
    double blendTime(const std::string& from, const std::string& to) const;
    void setNext(const std::string& from, const std::string& to);
    std::string next(const std::string& from) const;

    // Settings.
    double speedScale = 1.0;
    double defaultBlendTime = 0.0;
    bool deterministic = false;       // no weight normalisation across animations
    bool active = true;
    PropDiscrete discrete = PropDiscrete::Recessive;

    // Transport. Return false on an unknown animation. play(): empty name = the assigned one (a resume).
    bool play(const std::string& name = std::string(), double customBlend = -1.0, double customSpeed = 1.0, bool fromEnd = false);
    void stop(bool keepState = false);
    void pause();
    void queue(const std::string& name);
    void clearQueue() { queue_.clear(); }
    const std::vector<std::string>& queued() const { return queue_; }
    void seek(double time, bool update = false, bool updateOnly = false);
    void advance(double delta);                              // one mixer pass
    void setAssigned(const std::string& name);

    // State.
    bool isPlaying() const { return playing_ && cur_.clip != nullptr; }
    bool hasCurrent() const { return cur_.clip != nullptr; }
    const std::string& assigned() const { return assigned_; }
    std::string current() const { return isPlaying() ? assigned_ : std::string(); }
    double position() const { return cur_.pos; }
    double length() const { return cur_.clip ? double(cur_.clip->duration) : 0.0; }
    double playingSpeed() const { return isPlaying() ? speedScale * cur_.speed : 0.0; }
    double customSpeed() const { return cur_.speed; }
    usize blendCount() const { return blends_.size(); }
    double blendLeft(usize i) const { return i < blends_.size() ? blends_[i].left : 0.0; }

    // Results of the passes since the host last cleared them.
    std::vector<PropEvent>& events() { return events_; }
    const std::vector<PropOutput>& outputs() const { return outputs_; }
    void clearResults() { events_.clear(); }
    usize trackCount() const { return reg_.size(); }
    OcPropTrackType trackType(u32 id) const { return reg_[id].type; }
    const std::string& trackPath(u32 id) const { return reg_[id].path; }

    // Capture mode: the paths of the next play()'s target clip whose track is in Capture mode; the host reads each target's
    // current value and passes them to capture(), which fades that snapshot out over `duration` while the new clip comes in.
    std::vector<std::pair<OcPropTrackType, std::string>> captureTracks(const std::string& name) const;
    void capture(const std::vector<std::pair<std::pair<OcPropTrackType, std::string>, PropValue>>& values, double duration, double transition, double ease);

    // ---- tree mode (AnimationTree, G5 A2) ----
    // A graph evaluated by the host (the state machine, the blend nodes) hands this player the animations it wants mixed this pass, then
    // calls treeMix(): the same weight-normalised blend, discrete / method / audio / sub-player handling and event order as a player pass.
    // `trackWeights` (optional, one per track of the clip, else all 1) scales the instance per track: how a filter keeps a blend node
    // off some tracks. `prev` is the playhead before the pass and `delta` the signed movement; the returned step has the new playhead
    // (wrapped by the clip's loop mode) and the segments the pass crossed are kept for the method / audio keys. A seeked instance does
    // not move the root-motion track.
    PropTreeStep treeAdd(const std::string& key, double prev, double delta, double weight, const std::vector<double>& trackWeights, bool seeked, bool external, bool includeStart = false);
    void treeMix(bool updateOnly);

    // The node path of the root-motion track ("" off). Its position / rotation / scale tracks are not written to outputs(); rootMotion()
    // carries what they did instead.
    void setRootMotionPath(const std::string& path) { rootPath_ = path; rootMotion_ = PropRootMotion{}; }
    const std::string& rootMotionPath() const { return rootPath_; }
    const PropRootMotion& rootMotion() const { return rootMotion_; }

private:
    struct Playing { Clip clip; double pos = 0; double speed = 1; std::string name; };
    struct Blend { Playing data; double left = 0, time = 1; bool snapshot = false; };   // snapshot: a capture(), which a play() keeps
    struct Inst {
        Clip clip; std::string name; double weight = 0, time = 0, delta = 0;
        bool seeked = false, external = false, backward = false;
        std::vector<PropSegment> segs;
        std::vector<double> tw;          // per-track weight factors (tree mode filters); empty = all 1
        double trackWeight(usize i) const { return i < tw.size() ? tw[i] : 1.0; }
    };
    struct TrackReg { OcPropTrackType type; std::string path; bool hasInit = false; PropValue init; };

    void process(double delta, bool updateOnly);
    void playbackData(Playing& p, double delta, double weight, bool seeked, bool internalSeeked, bool started, bool isCurrent, std::vector<Inst>& out);
    void mix(std::vector<Inst>& insts, bool updateOnly, bool externalSeekCurrent);
    void rootMotionStep(const Inst& in, const fmt::OcAnimation& c, const fmt::OcPropTrack& t, double blend);
    u32 intern(OcPropTrackType type, const std::string& path);
    double currentBlendAmount() const;
    void emitAudioStops();
    void push(PropEvent::Type t, std::string a = {}, std::string b = {});

    std::vector<std::pair<std::string, Clip>> clips_;
    std::map<std::pair<std::string, std::string>, double> blendTimes_;
    std::map<std::string, std::string> next_;
    std::vector<std::string> queue_;
    Playing cur_;
    std::string assigned_;
    std::vector<Blend> blends_;
    bool playing_ = false, started_ = false, seeked_ = false, internalSeeked_ = false;
    bool endReached_ = false, endNotify_ = false;
    std::vector<TrackReg> reg_;
    std::map<std::pair<u8, std::string>, u32> regIndex_;
    std::vector<PropEvent> events_;
    std::vector<PropOutput> outputs_;
    std::vector<Inst> treeInsts_;
    std::string rootPath_;
    PropRootMotion rootMotion_;
};

} // namespace aver::anim
