// THE DECISIONS BEHIND EDITING AN ANIMATION CLIP: how one key moves inside one OcTrack, how a whole
// OcAnimation retimes without its notifies and curves drifting out of sync with the pose, and how a
// skeleton's bones sort themselves into limbs a person can tell apart without reading a name.
//
// A PURE HEADER, DELIBERATELY, following sandbox/src/RevisionControl.hpp line for line -- read that
// file's own top comment for the full case, which this one does not repeat. No ImGui types, no
// AnimEditor state, no device, no globals, no I/O anywhere near it: plain aver::fmt structs in,
// plain values out, over aver/core/Types.hpp and aver/formats/OcAnim.hpp alone. That is what makes
// every decision below a headless unit test in a codebase where almost nothing about the editor can
// be tested at all -- a test that includes only this header and links Aver.Core and Aver.Formats,
// nothing that touches a window, a GPU or a running engine.
//
// THREE THINGS THIS FILE GETS RIGHT THAT ARE EASY TO GET SUBTLY WRONG:
//
//   THE STRIDE. OcTrack::values is key-major: componentsPerKey() floats per key, and that count
//   TRIPLES under CubicSpline because each key then carries an in-tangent, a value and an out-
//   tangent for every enabled channel component, laid out per-channel as
//   [inTan x channelWidth, value x channelWidth, outTan x channelWidth] -- not as three flat blocks
//   spanning every channel. An insert, delete or move that treats a key as `componentsPerKey()`
//   opaque floats is safe; one that writes only the "value" third and leaves the tangents at
//   whatever the vector's resize left there corrupts the track into something OcTrack::valid()
//   rejects the moment anyone tries to save it. Every mutator below goes through
//   trackComponentOffset(), the one place that stride arithmetic is done, so there is exactly one
//   place left to get it wrong.
//
//   RETIMING. A clip's duration, every track's key times, every notify's time AND state-window
//   length, and every curve's key times are all, independently, "a time" -- and a retime that moves
//   some of them and not others is worse than one that moves none, because it looks correct until
//   the footstep sound lands on a frame the foot has already left. CubicSpline tangents are a
//   subtler case of the same bug: they are stored as per-second velocities (see hermiteVelocity's
//   own comment for why), so stretching every TIME by a factor without also shrinking every
//   TANGENT by the same factor leaves the keys in the right place while visibly warping the curve
//   between them.
//
//   THE PALETTE. A rig read by parent index rather than by name, because bone names are whatever
//   the DCC exported and this file has no business trusting them. The chain a bone belongs to is
//   derived from where the skeleton actually forks, not from which bones are direct children of the
//   single root -- see boneChainRootAndDepth's own comment for why the literal reading of
//   "child of root" would paint an entire biped's upper body, both arms included, as one colour.
//
// WHAT THE .cpp SIDE STILL HAS TO DO. Everything with a screen: turn drawTracks()'s "Keys: N" column
// into live numbers by calling trackSampleAt() at the scrub head (or trackReadComponent() per key
// when a row is expanded); give drawTimeline() one lane per track instead of one shared bar, tinted
// by computeBonePalette(); turn a click-and-drag on a key into trackMoveKey(), a double-click on
// empty timeline into trackInsertKey(), Delete into trackDeleteKey(), and a retime toolbar into
// scaleClipDuration()/shiftClipTime()/trimClip(); and call pushUndo() before every one of those
// mutations exactly as flagCheckbox() already does for the flag bits, since this header has no undo
// stack of its own and no dirty()/save() -- it only ever touches the one fmt:: value it was handed.
// The blank-white preview material and the bones that vanish the moment a mesh loads are both real
// bugs the request that led to this file named explicitly, and BOTH are the .cpp side's to fix: one
// is a material-binding question in buildPreview()/AnimSkinFeature, the other is whatever currently
// gates the bone-box overlay off once a mesh preview exists (see this file's own comment on why the
// boxes are still worth keeping). Neither has a "decision" for a pure header to make, so neither is
// here.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace aver::editor {

// ---- KEY EDITING, on one OcTrack -----------------------------------------------------------------

// No key at this index, or no such key resulted from the call. Every mutator below that can fail
// returns this instead of throwing or asserting -- a malformed edit request (an out-of-range index,
// a non-finite time) is routine input from a UI mid-drag, not a programming error.
inline constexpr usize kInvalidKeyIndex = static_cast<usize>(-1);

// Which third of a CubicSpline key a value comes from. The numbering (0/1/2, not an unordered enum)
// is load-bearing: trackComponentOffset multiplies it by a channel's width to find that third's
// offset inside the key, exactly as aver/anim/AnimSampler.cpp's own private readChannel() does for
// the runtime sampler. The two files must agree on this or an edit here and a sample there would
// disagree about which floats mean what.
enum class TrackKeySlot : u8 { InTangent = 0, Value = 1, OutTangent = 2 };

// Floats one channel occupies before any CubicSpline tripling: 4 for a quaternion rotation, 3 for a
// translation or a scale. Undefined for anything other than one of the three kOcChannel* bits --
// exactly the contract aver/anim/AnimSampler.cpp's own channelWidth() keeps, duplicated here rather
// than shared because this file may not reach into Aver.Anim (see this header's own top comment).
inline u32 trackChannelWidth(u8 channel) { return channel == fmt::kOcChannelRotation ? 4u : 3u; }

// Where component `component` of `channel` under `slot` sits inside ONE key's stride block (an
// offset in [0, componentsPerKey())). False when the request cannot be answered: `channel` is not
// in the track's mask, `component` is outside that channel's width, or `slot` asks for a tangent on
// a track that is not CubicSpline.
//
// THAT LAST REFUSAL IS STRICTER than AnimSampler's own readChannel(), which silently aliases an
// ignored tangent slot onto the value slot -- safe there because the sampler only ever asks for
// slot 1 outside its own CubicSpline branch. This function is reached from an editor, where a drag
// could plausibly ask for a Linear key's "out tangent" because a handle should not have been drawn
// for it at all; aliasing that write onto the key's actual value would silently move a bone instead
// of failing loudly, so this refuses instead of reinterpreting the request.
inline bool trackComponentOffset(const fmt::OcTrack& track, u8 channel, u32 component,
                                  TrackKeySlot slot, u32& outOffset) {
    if (!(track.channels & channel)) return false;
    const u32 width = trackChannelWidth(channel);
    if (component >= width) return false;
    const bool cubic = track.interp == fmt::OcInterp::CubicSpline;
    if (slot != TrackKeySlot::Value && !cubic) return false;

    const u8 order[3] = {fmt::kOcChannelTranslation, fmt::kOcChannelRotation, fmt::kOcChannelScale};
    const u32 mul = cubic ? 3u : 1u;
    u32 base = 0;
    for (u8 c : order) {
        if (c == channel) {
            outOffset = base + component + (cubic ? static_cast<u32>(slot) * width : 0u);
            return true;
        }
        if (track.channels & c) base += trackChannelWidth(c) * mul;
    }
    return false;   // unreachable: `channel` was in the mask, so it is one of `order`'s three bits
}

// One component of one key, read or written directly -- the primitive an ImGui drag float on a
// single curve handle needs, without the caller having to know the stride math above.
inline bool trackReadComponent(const fmt::OcTrack& track, usize keyIndex, u8 channel, u32 component,
                                TrackKeySlot slot, f32& outValue) {
    if (keyIndex >= track.times.size()) return false;
    u32 offset = 0;
    if (!trackComponentOffset(track, channel, component, slot, offset)) return false;
    const usize idx = keyIndex * static_cast<usize>(track.componentsPerKey()) + offset;
    if (idx >= track.values.size()) return false;   // defensive: a track that was already malformed
    outValue = track.values[idx];
    return true;
}

inline bool trackWriteComponent(fmt::OcTrack& track, usize keyIndex, u8 channel, u32 component,
                                 TrackKeySlot slot, f32 value) {
    if (keyIndex >= track.times.size()) return false;
    if (!std::isfinite(value)) return false;   // one poisoned float here poisons every sample of it
    u32 offset = 0;
    if (!trackComponentOffset(track, channel, component, slot, offset)) return false;
    const usize idx = keyIndex * static_cast<usize>(track.componentsPerKey()) + offset;
    if (idx >= track.values.size()) return false;
    track.values[idx] = value;
    return true;
}

// The last key at or before `time`, and how far from it to the next, exactly as AnimSampler.cpp's
// own private findKey() computes it (same clamp-to-the-nearest-end behaviour outside the track's
// own range). Duplicated for the same reason trackChannelWidth is: this file cannot include
// aver/anim without leaving the Aver.Core/Aver.Formats-only contract the next stage's test depends
// on. Shared by every channel of a track, since `times` is one array for the whole track -- callers
// that need more than one channel's value at the same instant should call this ONCE, not once per
// channel (trackInsertKey below does exactly that).
inline usize findTrackKeySpan(const fmt::OcTrack& track, f32 time, f32& outAlpha, usize& outNext) {
    const usize n = track.times.size();
    outAlpha = 0.0f;
    if (n == 0) { outNext = 0; return 0; }
    if (time <= track.times[0])     { outNext = 0;     return 0; }
    if (time >= track.times[n - 1]) { outNext = n - 1; return n - 1; }
    usize lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (track.times[mid] <= time) lo = mid; else hi = mid;
    }
    outNext = hi;
    const f32 span = track.times[hi] - track.times[lo];
    outAlpha = span > 1e-9f ? (time - track.times[lo]) / span : 0.0f;
    return lo;
}

// glTF's cubic Hermite basis, the same formula aver/anim/AnimSampler.cpp's private hermite() uses:
// p(s) = h00*v0 + h10*dt*b0 + h01*v1 + h11*dt*a1, s in [0,1] across a segment `dt` seconds long.
inline f32 hermiteValue(f32 v0, f32 b0, f32 v1, f32 a1, f32 s, f32 dt) {
    const f32 s2 = s * s, s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * v0
         + (s3 - 2.0f * s2 + s) * dt * b0
         + (-2.0f * s3 + 3.0f * s2) * v1
         + (s3 - s2) * dt * a1;
}

// dp/dtime of the same curve at the same point -- the derivative of hermiteValue's basis, divided
// by `dt` to convert the parametric slope (dp/ds) into the PER-SECOND velocity a stored tangent
// actually is. That division is what makes b0/a1 independent of a segment's length: evaluate this
// at s=0 with any dt and it returns exactly b0, which is the property trackInsertKey's CubicSpline
// case below depends on -- splitting a segment must not have to rewrite the tangents already stored
// at its two ends, only compute one new value at the split point.
inline f32 hermiteVelocity(f32 v0, f32 b0, f32 v1, f32 a1, f32 s, f32 dt) {
    if (dt <= 1e-9f) return 0.0f;   // a zero-length segment has no slope to give
    const f32 s2 = s * s;
    const f32 dh00 = 6.0f * s2 - 6.0f * s;
    const f32 dh10 = 3.0f * s2 - 4.0f * s + 1.0f;
    const f32 dh01 = -6.0f * s2 + 6.0f * s;
    const f32 dh11 = 3.0f * s2 - 2.0f * s;
    return (dh00 * v0 + dh10 * dt * b0 + dh01 * v1 + dh11 * dt * a1) / dt;
}

// The VALUE (never a tangent) every enabled channel of `track` holds at `time`, in channel order
// translation/rotation/scale, exactly as the runtime would sample it. False, with `outValues`
// cleared, when the track has no keys or an empty channel mask. This is what a live track-table row
// or a "what does this bone look like right now" readout should call -- and it is what
// trackInsertKey below is built to reproduce exactly, which is the whole reason inserting a key
// changes nothing about the pose until it is moved.
inline bool trackSampleAt(const fmt::OcTrack& track, f32 time, std::vector<f32>& outValues) {
    outValues.clear();
    if (track.times.empty() || track.componentsPerKey() == 0) return false;

    f32 alpha = 0.0f;
    usize next = 0;
    const usize k = findTrackKeySpan(track, time, alpha, next);
    const f32 dt = (k == next) ? 0.0f : (track.times[next] - track.times[k]);
    const u32 stride = track.componentsPerKey();

    auto at = [&](usize key, u32 off) -> f32 {
        const usize idx = static_cast<usize>(key) * stride + off;
        return idx < track.values.size() ? track.values[idx] : 0.0f;
    };

    const u8 order[3] = {fmt::kOcChannelTranslation, fmt::kOcChannelRotation, fmt::kOcChannelScale};
    for (u8 channel : order) {
        if (!(track.channels & channel)) continue;
        const u32 width = trackChannelWidth(channel);
        for (u32 c = 0; c < width; ++c) {
            u32 offValue = 0;
            trackComponentOffset(track, channel, c, TrackKeySlot::Value, offValue);
            if (track.interp == fmt::OcInterp::Step || k == next) {
                outValues.push_back(at(k, offValue));
            } else if (track.interp == fmt::OcInterp::Linear) {
                outValues.push_back(at(k, offValue) + (at(next, offValue) - at(k, offValue)) * alpha);
            } else {
                u32 offIn = 0, offOut = 0;
                trackComponentOffset(track, channel, c, TrackKeySlot::InTangent, offIn);
                trackComponentOffset(track, channel, c, TrackKeySlot::OutTangent, offOut);
                outValues.push_back(hermiteValue(at(k, offValue), at(k, offOut),
                                                  at(next, offValue), at(next, offIn), alpha, dt));
            }
        }
    }
    return true;
}

// Inserts a key at `time`, sampling the track's CURRENT value there for every enabled channel, so
// the pose at that instant (and, for CubicSpline, everywhere else too -- see below) is unchanged
// until the author moves the new key. Returns the new key's index, or kInvalidKeyIndex when the
// track's channel mask is empty or `time` is not finite. Ties (a key already at `time`, which
// OcTrack::valid() allows as a deliberate "jump" -- see OcCurve's own comment on the same case) land
// AFTER the existing one, via std::upper_bound; trackMoveKey below uses the same tie-break so the
// two operations agree on where an ambiguous time sorts.
//
// STEP AND LINEAR are exact by construction: a Step track's value at `time` is whatever the
// preceding key already holds, so inserting it changes nothing at any sampled instant; a Linear
// track's new key lies exactly on the straight segment it splits, so the two shorter segments trace
// the same line the one longer one did.
//
// CUBICSPLINE NEEDS A THIRD VALUE PER COMPONENT, not one -- see this file's top comment on the
// stride trap -- and getting the pose-preservation guarantee for free needs more than copying the
// sampled value into the middle third and zero into the other two. A cubic Hermite segment is, in
// real time, a single cubic polynomial; splitting it at `time` and re-expressing each half as its
// own Hermite segment is EXACT (the two halves concatenate back to the identical curve) precisely
// when the new key's in- and out-tangent both equal hermiteVelocity() at the split point, and the
// two ORIGINAL keys either side keep the tangents they already had -- hermiteVelocity's own comment
// is where that "no rewrite needed at the ends" property comes from. So: the two straddling keys
// are left alone, and the new key's value and both tangents come from hermiteValue/hermiteVelocity
// at the same (k, next, alpha, dt) span shared across every channel.
//
// THE ONE CASE THAT DOES NOT GET THIS GUARANTEE is inserting a key BEFORE a track's first key or
// AFTER its last: that is not splitting an existing segment, it is extending the track into time it
// previously did not cover, which was flat-clamped to the boundary key's own value with no slope at
// all. The new key's tangents are set to zero there -- an eased entry into the new coverage rather
// than inheriting a neighbour's possibly large tangent -- but the newly-covered span between the two
// keys will generally NOT be flat any more, because it is now a real interpolated segment using the
// boundary key's own (pre-existing, untouched) tangent. That is the unavoidable cost of asking for
// coverage that did not exist before, not a bug in this function.
inline usize trackInsertKey(fmt::OcTrack& track, f32 time) {
    const u32 stride = track.componentsPerKey();
    if (stride == 0 || !std::isfinite(time)) return kInvalidKeyIndex;
    const bool cubic = track.interp == fmt::OcInterp::CubicSpline;

    bool haveSpan = false;
    f32 alpha = 0.0f, dt = 0.0f;
    usize k = 0, next = 0;
    if (!track.times.empty()) {
        k = findTrackKeySpan(track, time, alpha, next);
        dt = (k == next) ? 0.0f : (track.times[next] - track.times[k]);
        haveSpan = true;
    }

    const u8 order[3] = {fmt::kOcChannelTranslation, fmt::kOcChannelRotation, fmt::kOcChannelScale};
    std::vector<f32> block;
    block.reserve(stride);
    for (u8 channel : order) {
        if (!(track.channels & channel)) continue;
        const u32 width = trackChannelWidth(channel);
        // Value and (when cubic) velocity for every component of THIS channel, gathered before
        // appending: the on-disk layout is per-channel [in x width, value x width, out x width], so
        // the three thirds cannot be written component-by-component -- see this file's top comment.
        std::vector<f32> val(width, 0.0f), vel(width, 0.0f);
        if (haveSpan) {
            for (u32 c = 0; c < width; ++c) {
                u32 offValue = 0;
                trackComponentOffset(track, channel, c, TrackKeySlot::Value, offValue);
                const usize strideSz = static_cast<usize>(stride);
                auto at = [&](usize key, u32 off) -> f32 {
                    const usize idx = static_cast<usize>(key) * strideSz + off;
                    return idx < track.values.size() ? track.values[idx] : 0.0f;
                };
                if (!cubic) {
                    const f32 v0 = at(k, offValue);
                    val[c] = (track.interp == fmt::OcInterp::Step || k == next)
                                 ? v0 : v0 + (at(next, offValue) - v0) * alpha;
                } else {
                    u32 offIn = 0, offOut = 0;
                    trackComponentOffset(track, channel, c, TrackKeySlot::InTangent, offIn);
                    trackComponentOffset(track, channel, c, TrackKeySlot::OutTangent, offOut);
                    const f32 v0 = at(k, offValue), b0 = at(k, offOut);
                    const f32 v1 = at(next, offValue), a1 = at(next, offIn);
                    if (k == next) {
                        val[c] = v0;
                        vel[c] = 0.0f;   // outside the track's own range: flat, not extrapolated
                    } else {
                        val[c] = hermiteValue(v0, b0, v1, a1, alpha, dt);
                        vel[c] = hermiteVelocity(v0, b0, v1, a1, alpha, dt);
                    }
                }
            }
        }
        if (cubic) {
            block.insert(block.end(), vel.begin(), vel.end());   // in-tangent
            block.insert(block.end(), val.begin(), val.end());   // value
            block.insert(block.end(), vel.begin(), vel.end());   // out-tangent: same slope both sides
        } else {
            block.insert(block.end(), val.begin(), val.end());
        }
    }

    const usize insertAt = static_cast<usize>(
        std::upper_bound(track.times.begin(), track.times.end(), time) - track.times.begin());
    track.times.insert(track.times.begin() + static_cast<isize>(insertAt), time);
    track.values.insert(track.values.begin() + static_cast<isize>(insertAt) * static_cast<isize>(stride),
                         block.begin(), block.end());
    return insertAt;
}

// Removes key `keyIndex` -- its time and its whole stride block -- and nothing else. Can shrink a
// track to zero keys; OcTrack::valid() requires at least one, so a caller that lets a track reach
// zero keys owns the decision of what happens to a now-invalid track (drop it from the clip, refuse
// the delete before calling this, or something else). This function does not reach into
// OcAnimation::tracks to make that call itself -- it only ever touches the one track it was handed.
inline bool trackDeleteKey(fmt::OcTrack& track, usize keyIndex) {
    if (keyIndex >= track.times.size()) return false;
    const u32 stride = track.componentsPerKey();
    track.times.erase(track.times.begin() + static_cast<isize>(keyIndex));
    if (stride > 0) {
        const isize begin = static_cast<isize>(keyIndex) * static_cast<isize>(stride);
        const isize wanted = begin + static_cast<isize>(stride);
        const isize end = std::min<isize>(wanted, static_cast<isize>(track.values.size()));
        if (begin >= 0 && static_cast<usize>(begin) <= track.values.size() && end > begin)
            track.values.erase(track.values.begin() + begin, track.values.begin() + end);
    }
    return true;
}

// Moves key `keyIndex` to `newTime`, RE-SORTING the track rather than clamping the move at its
// neighbours. Both are defensible: a clamp keeps every key's index stable, but it means a drag that
// asks for a time past the next key silently stops short of it with no way for this pure function
// to explain why, which reads as the editor ignoring the mouse. Re-sorting instead means every drag
// lands exactly where it was aimed, matches how Maya's Graph Editor and Unreal's Sequencer both
// treat a key dragged past its neighbour (the keys simply change order), and needs no second code
// path: it is trackDeleteKey's stride-block extraction followed by trackInsertKey's sorted
// placement, reusing both rather than clamping being a third way to move a key.
//
// WHAT A TEST CAN CHECK: the returned index is NOT `keyIndex` once the move crosses a neighbour;
// track.times stays ascending; and the stride block now sitting at the returned index is byte-for-
// byte the one that used to sit at `keyIndex` -- i.e. the key's data moved with it rather than being
// resampled or dropped. Returns kInvalidKeyIndex, leaving the track untouched, when `keyIndex` is
// out of range, `newTime` is not finite, or the track is malformed (values shorter than its own
// stride implies).
inline usize trackMoveKey(fmt::OcTrack& track, usize keyIndex, f32 newTime) {
    if (keyIndex >= track.times.size() || !std::isfinite(newTime)) return kInvalidKeyIndex;
    const u32 stride = track.componentsPerKey();
    const isize oldBegin = static_cast<isize>(keyIndex) * static_cast<isize>(stride);
    if (stride == 0 || oldBegin < 0 ||
        static_cast<usize>(oldBegin) + static_cast<usize>(stride) > track.values.size())
        return kInvalidKeyIndex;

    const isize oldEnd = oldBegin + static_cast<isize>(stride);
    std::vector<f32> block(track.values.begin() + oldBegin, track.values.begin() + oldEnd);
    track.values.erase(track.values.begin() + oldBegin, track.values.begin() + oldEnd);
    track.times.erase(track.times.begin() + static_cast<isize>(keyIndex));

    // SAME TIE-BREAK AS trackInsertKey: a moved key landing exactly on an existing time sorts after
    // it, so the two operations never disagree about where an ambiguous time belongs.
    const usize insertAt = static_cast<usize>(
        std::upper_bound(track.times.begin(), track.times.end(), newTime) - track.times.begin());
    track.times.insert(track.times.begin() + static_cast<isize>(insertAt), newTime);
    track.values.insert(track.values.begin() + static_cast<isize>(insertAt) * static_cast<isize>(stride),
                         block.begin(), block.end());
    return insertAt;
}

// ---- CLIP OPERATIONS, on a whole OcAnimation -----------------------------------------------------

// The latest instant anything in `clip` actually reaches: the last key of every track, every
// notify's own end (time + its state-window length, when it has one), and the last key of every
// curve. Used by shiftClipTime as a safety net -- see its own comment for why OcAnimation::valid()
// does not already guarantee `clip.duration` is the largest of these.
inline f32 clipMaxTimeReached(const fmt::OcAnimation& clip) {
    f32 m = 0.0f;
    for (const fmt::OcTrack& t : clip.tracks)
        if (!t.times.empty()) m = std::max(m, t.times.back());
    for (usize i = 0; i < clip.notifies.size(); ++i) {
        f32 end = clip.notifies[i].time;
        if (i < clip.notifyDurations.size()) end += clip.notifyDurations[i];
        m = std::max(m, end);
    }
    for (const fmt::OcCurve& c : clip.curves)
        if (!c.times.empty()) m = std::max(m, c.times.back());
    return m;
}

// Scales the WHOLE clip's timeline by `factor` (> 1 slows it down, < 1 speeds it up): duration,
// every track's key times, every notify's time AND state-window length (a duration is a span of
// time exactly like a track's dt, so it scales too, unlike under shiftClipTime), and every curve's
// key times. False and unchanged when `factor` is not finite and strictly positive -- zero or
// negative would collapse every key onto one instant or reverse their order, which is not what
// "scale" means for a timeline this function still has to leave ascending.
//
// CUBICSPLINE TANGENTS ARE RESCALED BY 1/factor, not by `factor`: they are stored as per-second
// velocities (hermiteVelocity's own comment says why), so stretching every TIME by `factor` without
// touching a stored slope leaves the curve's VALUE at each key exactly where it was but changes how
// fast it travels between them, visibly warping the shape. Only the in/out tangent thirds of a key
// move here -- the value third does not -- which is why this walks per-channel blocks by stride
// rather than the flat array, exactly as trackInsertKey's cubic case does.
//
// BAKEDUNIFORM'S sampleRate (keys per second) is the reciprocal of a time span, so it moves the
// OPPOSITE way duration does: the same key count spread over `factor` times the time is `factor`
// times fewer keys per second. Rounded (not truncated) so a factor near 1.0 -- an accidental no-op
// retime -- does not quietly nudge the stored rate down by one, and clamped to [1, 65535] because
// OcAnimation::valid() treats a zero sampleRate on BakedUniform storage as invalid, and a retimed
// clip that validated before this call must still validate after it.
inline bool scaleClipDuration(fmt::OcAnimation& clip, f32 factor) {
    if (!std::isfinite(factor) || !(factor > 0.0f)) return false;

    clip.duration *= factor;

    for (fmt::OcTrack& t : clip.tracks) {
        for (f32& time : t.times) time *= factor;
        if (t.interp != fmt::OcInterp::CubicSpline) continue;
        const u32 stride = t.componentsPerKey();
        if (stride == 0) continue;
        // PER CHANNEL, not as one flat [all-in][all-value][all-out] key -- a multi-channel track
        // (translation AND rotation, say) lays each channel's own [in,value,out] triple out before
        // the next channel's starts (see this file's top comment on the stride trap), so a flat
        // width-sized slice off the front of the key would grab channel A's in-tangent together
        // with the START of channel A's own VALUE third once a second channel pushes the true
        // in-tangent boundary past `width`. trackComponentOffset is the one place that already gets
        // this right, so it is used here instead of re-deriving the layout a second time.
        const u8 order[3] = {fmt::kOcChannelTranslation, fmt::kOcChannelRotation, fmt::kOcChannelScale};
        for (usize k = 0; k < t.times.size(); ++k) {
            const usize base = k * stride;
            for (u8 channel : order) {
                if (!(t.channels & channel)) continue;
                const u32 width = trackChannelWidth(channel);
                for (u32 c = 0; c < width; ++c) {
                    u32 offIn = 0, offOut = 0;
                    trackComponentOffset(t, channel, c, TrackKeySlot::InTangent, offIn);
                    trackComponentOffset(t, channel, c, TrackKeySlot::OutTangent, offOut);
                    if (base + offIn < t.values.size())  t.values[base + offIn]  /= factor;
                    if (base + offOut < t.values.size()) t.values[base + offOut] /= factor;
                }
            }
        }
    }

    for (fmt::OcNotify& n : clip.notifies) n.time *= factor;
    for (f32& d : clip.notifyDurations) d *= factor;

    for (fmt::OcCurve& c : clip.curves) {
        for (f32& time : c.times) time *= factor;
        if (c.interp == fmt::OcInterp::CubicSpline &&
            c.inTangents.size() == c.times.size() && c.outTangents.size() == c.times.size()) {
            for (f32& v : c.inTangents)  v /= factor;
            for (f32& v : c.outTangents) v /= factor;
        }
    }

    if (clip.storage == fmt::OcAnimStorage::BakedUniform && clip.sampleRate != 0) {
        const f32 rescaled = static_cast<f32>(clip.sampleRate) / factor;
        const f32 clamped = std::max(1.0f, std::min(65535.0f, std::round(rescaled)));
        clip.sampleRate = static_cast<u16>(clamped);
    }
    return true;
}

// Slides every time-bearing field in `clip` by `deltaSeconds`: track keys, notify times, curve
// keys. NOT notifyDurations -- a duration is a SPAN, not a POSITION, and sliding every timestamp
// later or earlier does not change how long a hit window stays open once it opens, which is exactly
// why scaleClipDuration (a change of SPEED, not of position) scales it and this does not.
//
// `clip.duration` is widened (or, for a negative shift, still kept at least as large as the content
// actually reaches) rather than shifted by the same delta and left at that: AnimSampler.cpp's
// clipTime() clamps playback to [0, duration], so if duration did not grow along with a positive
// shift, whatever moved past the OLD boundary would become unreachable -- silently truncating the
// very content this call just moved there. `duration + delta` is the answer for the ordinary case;
// clipMaxTimeReached() is the safety net for the case OcAnimation::valid() does not actually forbid
// -- a track whose last key already sat past `duration` before this call.
inline bool shiftClipTime(fmt::OcAnimation& clip, f32 deltaSeconds) {
    if (!std::isfinite(deltaSeconds)) return false;

    for (fmt::OcTrack& t : clip.tracks)
        for (f32& time : t.times) time += deltaSeconds;
    for (fmt::OcNotify& n : clip.notifies) n.time += deltaSeconds;
    for (fmt::OcCurve& c : clip.curves)
        for (f32& time : c.times) time += deltaSeconds;

    clip.duration = std::max(0.0f, std::max(clip.duration + deltaSeconds, clipMaxTimeReached(clip)));
    return true;
}

// Trims `clip` to [startTime, endTime] (clamped into the clip's own current range; false and
// unchanged if that leaves nothing, i.e. end <= start after clamping), then re-anchors everything
// so the kept span starts at zero.
//
// TRACKS GET A BOUNDARY KEY INSERTED AT EACH CUT before anything is deleted, via trackInsertKey --
// which samples the CURRENT curve, so this step alone changes nothing -- so that cutting away
// everything outside [start,end] does not also throw away the exact pose the surviving span had at
// its own edges. Without it, a bone whose nearest surviving key sits a quarter-second inside the
// new range would visibly jump the moment the trimmed clip starts playing.
//
// NOTIFIES OUTSIDE THE RANGE ARE DROPPED, and one that survives has its state-window length clamped
// so it cannot reach past the new, shorter duration -- unlike scaleClipDuration, which relies on
// the retimed duration and the retimed window growing together, a trim can genuinely shorten the
// window itself when the cut lands inside it.
//
// CURVES ARE TRIMMED BUT NOT BOUNDARY-KEYED THE WAY TRACKS ARE: a dropped curve edge degrades to
// aver/anim/AnimSampler.cpp's own sampleCurve() clamping to its nearest surviving key -- a flat
// hold, blunt but harmless. Doing the same to a TRACK would corrupt a POSE (a bone snapping to
// whichever key happens to survive the cut), which is the whole reason tracks get the more careful
// treatment above and curves do not need it.
inline bool trimClip(fmt::OcAnimation& clip, f32 startTime, f32 endTime) {
    if (!std::isfinite(startTime) || !std::isfinite(endTime)) return false;
    const f32 start = std::max(0.0f, std::min(startTime, clip.duration));
    const f32 end   = std::max(0.0f, std::min(endTime, clip.duration));
    if (!(end > start)) return false;

    constexpr f32 kEps = 1e-6f;
    auto hasKeyNear = [&](const std::vector<f32>& times, f32 t) {
        for (f32 tm : times) if (std::fabs(tm - t) <= kEps) return true;
        return false;
    };

    for (fmt::OcTrack& t : clip.tracks) {
        if (t.times.empty()) continue;
        if (!hasKeyNear(t.times, start)) trackInsertKey(t, start);
        if (!hasKeyNear(t.times, end))   trackInsertKey(t, end);
        // Sweep from the back: erasing a key does not disturb the indices of the ones before it.
        for (isize i = static_cast<isize>(t.times.size()) - 1; i >= 0; --i) {
            const f32 tm = t.times[static_cast<usize>(i)];
            if (tm < start - kEps || tm > end + kEps) trackDeleteKey(t, static_cast<usize>(i));
        }
    }

    std::vector<fmt::OcNotify> keptNotifies;
    std::vector<f32> keptDurations;
    const bool haveDurations = clip.notifyDurations.size() == clip.notifies.size();
    for (usize i = 0; i < clip.notifies.size(); ++i) {
        const fmt::OcNotify& n = clip.notifies[i];
        if (n.time < start - kEps || n.time > end + kEps) continue;
        fmt::OcNotify kept = n;
        kept.time = std::min(std::max(kept.time, start), end);
        if (haveDurations) keptDurations.push_back(std::min(clip.notifyDurations[i], end - kept.time));
        keptNotifies.push_back(kept);
    }
    clip.notifies = std::move(keptNotifies);
    clip.notifyDurations = std::move(keptDurations);

    for (fmt::OcCurve& c : clip.curves) {
        const bool hasTangents =
            c.inTangents.size() == c.times.size() && c.outTangents.size() == c.times.size();
        std::vector<f32> kt, kv, kin, kout;
        for (usize i = 0; i < c.times.size(); ++i) {
            if (c.times[i] < start - kEps || c.times[i] > end + kEps) continue;
            kt.push_back(std::min(std::max(c.times[i], start), end));
            kv.push_back(c.values[i]);
            if (hasTangents) { kin.push_back(c.inTangents[i]); kout.push_back(c.outTangents[i]); }
        }
        c.times = std::move(kt);
        c.values = std::move(kv);
        c.inTangents = std::move(kin);
        c.outTangents = std::move(kout);
    }

    // Re-anchor to zero, reusing shiftClipTime's own field list rather than a second copy of it --
    // see this file's top comment on why one time-bearing field moving on its own is the whole bug
    // class this file exists to avoid. Its safety-net duration is then overwritten with the exact
    // trimmed length: shiftClipTime cannot know the cut was deliberate rather than a track that
    // already ran past the old duration, so it always keeps at least as much as it started with.
    (void)shiftClipTime(clip, -start);
    clip.duration = end - start;
    return true;
}

// ---- THE BONE PALETTE, from an OcSkeleton ----------------------------------------------------------

// Plain sRGB-space components in [0,1], ready to hand to whatever colour type the .cpp side's UI
// library actually wants (an ImVec4, an IM_COL32) with no further conversion.
struct BoneColor {
    f32 r = 0.0f, g = 0.0f, b = 0.0f;
};

// Standard HSL -> RGB. Pulled out on its own, rather than folded into boneColorForChain below, so a
// test can pin a few known conversions (0,1,0.5 -> pure red, and so on) independently of anything
// about bones or chains.
inline BoneColor hslToBoneColor(f32 hueDegrees, f32 saturation01, f32 lightness01) {
    const f32 s = std::clamp(saturation01, 0.0f, 1.0f);
    const f32 l = std::clamp(lightness01, 0.0f, 1.0f);
    f32 h = std::fmod(hueDegrees, 360.0f);
    if (h < 0.0f) h += 360.0f;

    const f32 c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const f32 hp = h / 60.0f;
    const f32 x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    f32 r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if      (hp < 1.0f) { r1 = c;  g1 = x;  b1 = 0; }
    else if (hp < 2.0f) { r1 = x;  g1 = c;  b1 = 0; }
    else if (hp < 3.0f) { r1 = 0;  g1 = c;  b1 = x; }
    else if (hp < 4.0f) { r1 = 0;  g1 = x;  b1 = c; }
    else if (hp < 5.0f) { r1 = x;  g1 = 0;  b1 = c; }
    else                { r1 = c;  g1 = 0;  b1 = x; }
    const f32 m = l - c * 0.5f;
    return BoneColor{r1 + m, g1 + m, b1 + m};
}

// One entry per bone: how many OTHER bones name it as their parent. Parent links point only up, so
// this is the one thing boneChainRootAndDepth cannot answer from a single OcBone in isolation --
// "does this bone fork" needs the whole skeleton looked at once, which is why this is built once
// and shared rather than recomputed per bone inside the walk below.
inline void boneChildCounts(const fmt::OcSkeleton& skeleton, std::vector<u32>& outCounts) {
    outCounts.assign(skeleton.bones.size(), 0u);
    for (usize i = 0; i < skeleton.bones.size(); ++i) {
        const i32 parent = skeleton.bones[i].parent;
        if (parent != fmt::kOcBoneNoParent && static_cast<usize>(parent) < outCounts.size())
            ++outCounts[static_cast<usize>(parent)];
    }
}

// Walks up from `boneIndex` to the nearest ancestor (itself included) that STARTS a chain, writing
// that ancestor's own index to `outRoot` and how many steps the walk took to `outDepth` (0 when
// `boneIndex` is already a chain's start).
//
// A CHAIN STARTS at the skeleton's own root, or at any bone whose PARENT FORKS (has more than one
// child) -- not only at a direct child of the single skeleton root, which is the literal reading of
// "child of root" and the wrong one for what this is FOR. A standard biped has one root (the hips)
// with perhaps three direct children: spine, left thigh, right thigh. The spine bone alone then
// carries the chest, both shoulders, both entire arms and the neck for many more bones before
// anything forks again. Stopping only at direct children of the skeleton root would put that whole
// upper body -- both arms included -- into one "spine" chain, which is exactly the "one grey mass"
// a bone palette exists to avoid. Stopping at every fork instead gives the left arm its own chain
// the instant the shoulder bone splits off from the chest, which is what letting an author tell
// left from right without reading a name actually requires.
inline void boneChainRootAndDepth(const fmt::OcSkeleton& skeleton, u32 boneIndex,
                                   const std::vector<u32>& childCounts, u32& outRoot, u32& outDepth) {
    outRoot = boneIndex;
    outDepth = 0;
    if (boneIndex >= skeleton.bones.size()) return;   // defensive: answers as its own, singleton chain

    u32 cur = boneIndex;
    u32 steps = 0;
    // Bounded by bone count, not left to run until it finds a root: OcSkeleton::valid() rejects a
    // cyclic parent chain, but this function has to answer (rather than hang) even before a
    // skeleton has been validated.
    for (usize guard = 0; guard < skeleton.bones.size(); ++guard) {
        const i32 parent = skeleton.bones[cur].parent;
        if (parent == fmt::kOcBoneNoParent) break;                 // cur is itself a root: its own chain
        const u32 up = static_cast<u32>(parent);
        if (up >= skeleton.bones.size()) break;                    // defensive: a corrupt parent index
        const bool parentIsSkeletonRoot = skeleton.bones[up].parent == fmt::kOcBoneNoParent;
        const bool parentForks = childCounts[up] > 1;
        if (parentIsSkeletonRoot || parentForks) break;             // cur starts its own chain here
        cur = up;
        ++steps;
    }
    outRoot = cur;
    outDepth = steps;
}

inline u32 boneChainRoot(const fmt::OcSkeleton& skeleton, u32 boneIndex) {
    std::vector<u32> counts;
    boneChildCounts(skeleton, counts);
    u32 root = boneIndex, depth = 0;
    boneChainRootAndDepth(skeleton, boneIndex, counts, root, depth);
    return root;
}

inline u32 boneChainDepth(const fmt::OcSkeleton& skeleton, u32 boneIndex) {
    std::vector<u32> counts;
    boneChildCounts(skeleton, counts);
    u32 root = boneIndex, depth = 0;
    boneChainRootAndDepth(skeleton, boneIndex, counts, root, depth);
    return depth;
}

// One turn of the golden angle's FRACTIONAL part -- only where a hue lands on the 360-degree wheel
// matters, not how many full turns it took. Multiplying any two DISTINCT integers by this and
// reducing mod 1 puts their hues far apart, not approximately: this is the same low-discrepancy
// property behind sunflower-seed phyllotaxis and behind every "N maximally distinct colours"
// generator that does not know N in advance. It holds for any two distinct integers, adjacent or
// not, which is why boneColorForChain below needs no lookup table of "hues already used" and no
// pass over the skeleton to colour one bone -- unlike boneChainRootAndDepth, it does not even need
// childCounts.
inline constexpr f32 kBoneHueGoldenTurns = 0.6180339887498949f;

inline constexpr f32 kBoneSaturation  = 0.62f;
inline constexpr f32 kBoneLightLow    = 0.36f;
inline constexpr f32 kBoneLightHigh   = 0.64f;
// Depth-steps from low lightness back to low again is twice this: lightness CYCLES with depth
// (a triangle wave) rather than ramping monotonically, because a ramp eventually reaches white or
// black on a long chain -- a finger bone twelve joints into a hand would read as the same washed-
// out near-white as the one eleven joints in. A triangle wave keeps every step visibly different
// from its immediate neighbour no matter how long the chain runs.
inline constexpr u32 kBoneLightPeriod = 4;

inline BoneColor boneColorForChain(u32 chainRootBoneIndex, u32 depthInChain) {
    const f32 hue = std::fmod(static_cast<f32>(chainRootBoneIndex) * kBoneHueGoldenTurns, 1.0f) * 360.0f;

    const u32 phase = depthInChain % (2 * kBoneLightPeriod);
    const u32 folded = phase <= kBoneLightPeriod ? phase : (2 * kBoneLightPeriod - phase);
    const f32 frac = static_cast<f32>(folded) / static_cast<f32>(kBoneLightPeriod);
    const f32 lightness = kBoneLightLow + (kBoneLightHigh - kBoneLightLow) * frac;

    return hslToBoneColor(hue, kBoneSaturation, lightness);
}

// One bone's colour. O(bone count) PER CALL (it rebuilds childCounts every time), which is fine for
// a tooltip or a single selected-bone highlight but wasteful in a loop over every bone in the rig --
// use computeBonePalette for that instead, which builds childCounts once and shares this same
// chain/depth walk and colour formula so the two paths can never disagree about a bone's colour.
inline BoneColor boneColorFor(const fmt::OcSkeleton& skeleton, u32 boneIndex) {
    std::vector<u32> counts;
    boneChildCounts(skeleton, counts);
    u32 root = boneIndex, depth = 0;
    boneChainRootAndDepth(skeleton, boneIndex, counts, root, depth);
    return boneColorForChain(root, depth);
}

// One colour per bone, in bone order -- the call the .cpp side should actually make, once per
// skeleton load (or whenever the rig changes), caching the result rather than calling boneColorFor
// per bone per frame.
inline std::vector<BoneColor> computeBonePalette(const fmt::OcSkeleton& skeleton) {
    std::vector<u32> counts;
    boneChildCounts(skeleton, counts);
    std::vector<BoneColor> out(skeleton.bones.size());
    for (usize i = 0; i < skeleton.bones.size(); ++i) {
        u32 root = static_cast<u32>(i), depth = 0;
        boneChainRootAndDepth(skeleton, static_cast<u32>(i), counts, root, depth);
        out[i] = boneColorForChain(root, depth);
    }
    return out;
}

} // namespace aver::editor
