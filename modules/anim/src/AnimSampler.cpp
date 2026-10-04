// Clip sampling: key lookup, the three interpolations, and the player's clock.
#include "aver/anim/AnimSampler.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {

namespace {

// Width in floats of one channel's value.
u32 channelWidth(u8 channel) { return channel == fmt::kOcChannelRotation ? 4u : 3u; }

// Float offset of `channel`'s block within one key, and false when the track lacks it. Channels sit
// in the fixed order translation, rotation, scale; under CubicSpline each present channel occupies
// three times its width, because its in-tangent, value and out-tangent are stored together.
bool channelOffset(const fmt::OcTrack& t, u8 channel, u32& outOffset) {
    if (!(t.channels & channel)) return false;
    const u32 mul = t.interp == fmt::OcInterp::CubicSpline ? 3u : 1u;
    u32 off = 0;
    const u8 order[3] = {fmt::kOcChannelTranslation, fmt::kOcChannelRotation, fmt::kOcChannelScale};
    for (u8 c : order) {
        if (c == channel) { outOffset = off; return true; }
        if (t.channels & c) off += channelWidth(c) * mul;
    }
    return false;
}

// The last key at or before `seconds`, and how far from it to the next. Returns the key count when
// there is nothing to sample.
usize findKey(const fmt::OcTrack& t, f32 seconds, f32& outAlpha, usize& outNext) {
    const usize n = t.times.size();
    outAlpha = 0.0f;
    if (n == 0) { outNext = 0; return 0; }
    if (seconds <= t.times[0])     { outNext = 0;     return 0; }
    if (seconds >= t.times[n - 1]) { outNext = n - 1; return n - 1; }

    usize lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (t.times[mid] <= seconds) lo = mid; else hi = mid;
    }
    outNext = hi;
    const f32 span = t.times[hi] - t.times[lo];
    outAlpha = span > 1e-9f ? (seconds - t.times[lo]) / span : 0.0f;
    return lo;
}

// Reads `count` floats of one channel out of key `k`. `slot` picks the in-tangent (0), the value (1)
// or the out-tangent (2) under CubicSpline, and is ignored otherwise.
void readChannel(const fmt::OcTrack& t, usize k, u32 offset, u32 count, int slot, f32* out) {
    const u32 stride = t.componentsPerKey();
    u32 base = static_cast<u32>(k) * stride + offset;
    if (t.interp == fmt::OcInterp::CubicSpline) base += static_cast<u32>(slot) * count;
    for (u32 i = 0; i < count; ++i) {
        const usize idx = static_cast<usize>(base) + i;
        out[i] = idx < t.values.size() ? t.values[idx] : 0.0f;
    }
}

// Spherical interpolation between two rotation keys (glTF's LINEAR for rotations), along the
// shorter arc: q and -q are the same rotation, so a key pair stored in opposite hemispheres would
// otherwise swing the long way round. Constant angular speed between keys, unlike a lerp of the
// components, which speeds up and slows down across each key interval. Nearly equal keys fall back
// to a normalised lerp, where slerp's sin(theta) divide loses precision.
void slerpQuat(const f32 a[4], const f32 b[4], f32 s, f32 out[4]) {
    f32 bb[4] = {b[0], b[1], b[2], b[3]};
    f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) { d = -d; for (f32& v : bb) v = -v; }
    f32 wa = 1.0f - s, wb = s;
    if (d < 0.9995f) {
        const f32 theta = std::acos(std::min(d, 1.0f));
        const f32 sinT  = std::sin(theta);
        wa = std::sin((1.0f - s) * theta) / sinT;
        wb = std::sin(s * theta) / sinT;
    }
    f32 len2 = 0.0f;
    for (int i = 0; i < 4; ++i) { out[i] = a[i] * wa + bb[i] * wb; len2 += out[i] * out[i]; }
    const f32 inv = len2 > 0.0f ? 1.0f / std::sqrt(len2) : 1.0f;
    for (int i = 0; i < 4; ++i) out[i] *= inv;
}

// glTF's cubic Hermite: p(s) = h00*v0 + h10*dt*b0 + h01*v1 + h11*dt*a1, with b0 the out-tangent of
// the first key and a1 the in-tangent of the second.
f32 hermite(f32 v0, f32 b0, f32 v1, f32 a1, f32 s, f32 dt) {
    const f32 s2 = s * s, s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * v0
         + (s3 - 2.0f * s2 + s) * dt * b0
         + (-2.0f * s3 + 3.0f * s2) * v1
         + (s3 - s2) * dt * a1;
}

// Samples one channel of one track into `out`, or leaves it untouched and returns false.
bool sampleChannel(const fmt::OcTrack& t, u8 channel, f32 seconds, f32* out, u32 count) {
    u32 offset = 0;
    if (!channelOffset(t, channel, offset)) return false;
    if (t.times.empty()) return false;

    f32 alpha = 0.0f;
    usize next = 0;
    const usize k = findKey(t, seconds, alpha, next);

    if (t.interp == fmt::OcInterp::Step || k == next) {
        readChannel(t, k, offset, count, 1, out);
        return true;
    }
    if (t.interp == fmt::OcInterp::Linear) {
        f32 a[4] = {}, b[4] = {};
        readChannel(t, k, offset, count, 1, a);
        readChannel(t, next, offset, count, 1, b);
        if (channel == fmt::kOcChannelRotation) {
            slerpQuat(a, b, alpha, out);
            return true;
        }
        for (u32 i = 0; i < count; ++i) out[i] = a[i] + (b[i] - a[i]) * alpha;
        return true;
    }
    // CubicSpline
    f32 v0[4] = {}, b0[4] = {}, v1[4] = {}, a1[4] = {};
    readChannel(t, k,    offset, count, 1, v0);
    readChannel(t, k,    offset, count, 2, b0);
    readChannel(t, next, offset, count, 1, v1);
    readChannel(t, next, offset, count, 0, a1);
    const f32 dt = t.times[next] - t.times[k];
    for (u32 i = 0; i < count; ++i) out[i] = hermite(v0[i], b0[i], v1[i], a1[i], alpha, dt);
    return true;
}

} // namespace

f32 clipTime(const fmt::OcAnimation& clip, f32 seconds) {
    if (!(clip.duration > 0.0f)) return 0.0f;
    if (clip.flags & fmt::kOcAnimLoop) {
        f32 t = std::fmod(seconds, clip.duration);
        if (t < 0.0f) t += clip.duration;   // fmod keeps the sign of the numerator
        return t;
    }
    return seconds < 0.0f ? 0.0f : (seconds > clip.duration ? clip.duration : seconds);
}

void sampleAnimation(const fmt::OcAnimation& clip, f32 seconds, Pose& inOut) {
    for (const fmt::OcTrack& t : clip.tracks) {
        const usize bone = t.boneIndex;
        if (bone >= inOut.local.size()) continue;   // a track for a bone this skeleton does not have
        Transform& x = inOut.local[bone];

        f32 v[4] = {};
        if (sampleChannel(t, fmt::kOcChannelTranslation, seconds, v, 3))
            x.position = Vec3{v[0], v[1], v[2]};
        if (sampleChannel(t, fmt::kOcChannelRotation, seconds, v, 4))
            x.rotation = Quat{v[0], v[1], v[2], v[3]}.normalized();
        if (sampleChannel(t, fmt::kOcChannelScale, seconds, v, 3))
            x.scale = Vec3{v[0], v[1], v[2]};
    }
}

f32 sampleCurve(const fmt::OcCurve& c, f32 seconds, f32 fallback) {
    const usize n = c.times.size();
    // A curve with no keys has no value to give, and 0 would be a lie that looks like data. The
    // caller supplies what "no value" means to it.
    if (n == 0 || c.values.size() != n) return fallback;
    if (seconds <= c.times[0]) return c.values[0];
    if (seconds >= c.times[n - 1]) return c.values[n - 1];

    // Binary search for the last key at or before `seconds` -- the same shape findKey uses for a
    // bone track, because a curve is the same problem with a width of one.
    usize lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (c.times[mid] <= seconds) lo = mid; else hi = mid;
    }
    if (c.interp == fmt::OcInterp::Step) return c.values[lo];

    const f32 span = c.times[hi] - c.times[lo];
    // TWO KEYS AT THE SAME TIME are legal and mean a jump. Dividing by that zero span would give
    // an infinity; holding the earlier value makes the jump land exactly at the later key.
    if (span <= 1e-9f) return c.values[lo];
    const f32 a = (seconds - c.times[lo]) / span;

    // CUBICSPLINE, WITH REAL TANGENTS -- reuses the same hermite() helper the bone-track sampler
    // above already uses (and AnimTest already checks), rather than a second derivation. Only taken
    // when the curve's own tangent arrays line up 1:1 with its keys; every curve with no CTAN chunk
    // -- every curve saved before tonight -- falls through to the LINEAR return below instead, which
    // is what keeps an existing clip sampling EXACTLY as it did before this feature existed.
    if (c.interp == fmt::OcInterp::CubicSpline && c.inTangents.size() == n && c.outTangents.size() == n)
        return hermite(c.values[lo], c.outTangents[lo], c.values[hi], c.inTangents[hi], a, span);

    return c.values[lo] + (c.values[hi] - c.values[lo]) * a;
}

void notifiesCrossed(const fmt::OcAnimation& clip, const ClipStep& step, std::vector<u32>& outIndices) {
    if (clip.notifies.empty()) return;
    const f32 dur = clip.duration;

    for (usize i = 0; i < clip.notifies.size(); ++i) {
        const f32 t = dur > 0.0f ? std::min(std::max(clip.notifies[i].time, 0.0f), dur)
                                 : 0.0f;
        bool hit;
        if (step.sweptWholeClip) {
            // ONE STEP, ONE FIRING, however many times round the clip it went. A frame that lost a
            // second to a breakpoint or a level load should not deliver forty footsteps at once --
            // and the alternative (firing per lap) is not more correct, it is just louder about a
            // frame that already went wrong.
            hit = true;
        } else if (step.forward) {
            hit = (step.now >= step.prev)
                    ? (step.inclusiveStart ? (t >= step.prev && t <= step.now)
                                           : (t >  step.prev && t <= step.now))
                    // WRAPPED: the tail of the clip and then its head, and the head half INCLUDES 0
                    // so a start-of-clip notify fires on every loop rather than only on the first.
                    : ((t > step.prev && t <= dur) || (t >= 0.0f && t <= step.now));
        } else {
            hit = (step.now <= step.prev)
                    ? (step.inclusiveStart ? (t <= step.prev && t >= step.now)
                                           : (t <  step.prev && t >= step.now))
                    // Wrapped the other way: off the front of the clip and back onto its end.
                    : ((t < step.prev && t >= 0.0f) || (t >= step.now && t <= dur));
        }
        if (hit) outIndices.push_back(static_cast<u32>(i));
    }
}

void AnimPlayer::play(const fmt::OcAnimation* clip, f32 fadeSeconds) {
    if (clip == clip_) return;
    if (clip_ && fadeSeconds > 0.0f) {
        prev_ = clip_;
        prevTime_ = time_;
        fade_ = 0.0f;
        fadeRate_ = 1.0f / fadeSeconds;
    } else {
        prev_ = nullptr;
        fade_ = 1.0f;
        fadeRate_ = 0.0f;
    }
    clip_ = clip;
    time_ = 0.0f;
}

void AnimPlayer::reset(const fmt::OcAnimation* clip) {
    clip_ = clip;
    prev_ = nullptr;
    time_ = prevTime_ = 0.0f;
    fade_ = 1.0f;
    fadeRate_ = 0.0f;
}

void AnimPlayer::advance(f32 dt) {
    time_ += dt * speed_;
    if (prev_ && fade_ < 1.0f) {
        fade_ += dt * fadeRate_;
        if (fade_ >= 1.0f) { fade_ = 1.0f; prev_ = nullptr; }
    }
}

void AnimPlayer::evaluate(const fmt::OcSkeleton& skel, Pose& out) const {
    restPose(skel, out);
    if (!clip_) return;

    sampleAnimation(*clip_, clipTime(*clip_, time_), out);
    if (!prev_ || fade_ >= 1.0f) return;

    // The outgoing clip is sampled at the time it was frozen at, so a crossfade blends toward the
    // incoming clip rather than running two clocks that drift apart.
    Pose from;
    restPose(skel, from);
    sampleAnimation(*prev_, clipTime(*prev_, prevTime_), from);
    Pose blended;
    blendPose(from, out, fade_, blended);
    out = blended;
}

} // namespace aver::anim
