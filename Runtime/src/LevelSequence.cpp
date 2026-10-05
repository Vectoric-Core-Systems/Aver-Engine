// Level sequence sampling and playback (LevelSequence.hpp).
#include "aver/game/LevelSequence.hpp"

#include "aver/world/LevelTransform.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#if AVER_MODULE_SCENE
#include "aver/scene/World.hpp"
#endif

namespace aver::game {

namespace {

using fmt::OcSeqInterp;
using fmt::OcSeqKey;
using fmt::OcSeqTrack;
using fmt::OcSeqTrackKind;

// The segment t falls in: keys k1 -> k2, with k0 / k3 the Catmull-Rom neighbours (clamped).
struct Seg {
    usize k0 = 0, k1 = 0, k2 = 0, k3 = 0;
    f64 u = 0;
    OcSeqInterp interp = OcSeqInterp::Smooth;
};

Seg findSeg(const std::vector<OcSeqKey>& keys, f64 t) {
    Seg s;
    const usize n = keys.size();
    if (n == 1 || t <= keys.front().t) return s;   // hold the first key (all indices 0)
    if (t >= keys.back().t) {
        s.k0 = s.k1 = s.k2 = s.k3 = n - 1;
        return s;
    }
    usize i = 0;
    while (i + 2 < n && keys[i + 1].t <= t) ++i;
    s.k1 = i;
    s.k2 = i + 1;
    s.k0 = i > 0 ? i - 1 : i;
    s.k3 = i + 2 < n ? i + 2 : i + 1;
    const f64 dt = keys[s.k2].t - keys[s.k1].t;
    s.u = dt > 1e-12 ? std::clamp((t - keys[s.k1].t) / dt, 0.0, 1.0) : 0.0;
    s.interp = keys[s.k1].interp;
    return s;
}

f64 catmullRom(f64 p0, f64 p1, f64 p2, f64 p3, f64 u) {
    const f64 u2 = u * u, u3 = u2 * u;
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * u + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * u2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * u3);
}

f64 wrap180(f64 d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

// One scalar channel across the segment. angle: degrees, unwrapped so the short way round is taken.
f64 sampleChannel(const std::vector<OcSeqKey>& keys, const Seg& s, int c, bool angle) {
    f64 p0 = keys[s.k0].v[c], p1 = keys[s.k1].v[c], p2 = keys[s.k2].v[c], p3 = keys[s.k3].v[c];
    if (angle) {
        p0 = p1 + wrap180(p0 - p1);
        p2 = p1 + wrap180(p2 - p1);
        p3 = p2 + wrap180(p3 - p2);
    }
    switch (s.interp) {
        case OcSeqInterp::Step:   return p1;
        case OcSeqInterp::Linear: return p1 + (p2 - p1) * s.u;
        case OcSeqInterp::Smooth: return catmullRom(p0, p1, p2, p3, s.u);
    }
    return p1;
}

Quat keyRotation(const OcSeqKey& k) {
    return world::quatFromEulerDeg(Vec3{static_cast<f32>(k.v[5]), static_cast<f32>(k.v[4]),
                                        static_cast<f32>(k.v[3])});
}

f32 f(f64 v) { return static_cast<f32>(v); }

} // namespace

f64 seqWrapTime(const fmt::OcSequence& seq, f64 t) {
    if (seq.loop) {
        if (seq.length <= 0) return 0;
        f64 r = std::fmod(t, seq.length);
        if (r < 0) r += seq.length;
        return r;
    }
    return std::clamp(t, 0.0, std::max(seq.length, 0.0));
}

bool sampleSeqTransform(const OcSeqTrack& track, f64 t, Transform& out) {
    if (track.kind != OcSeqTrackKind::Transform || track.keys.empty()) return false;
    const Seg s = findSeg(track.keys, t);
    out.position = Vec3{f(sampleChannel(track.keys, s, 0, false)), f(sampleChannel(track.keys, s, 1, false)),
                        f(sampleChannel(track.keys, s, 2, false))};
    out.scale    = Vec3{f(sampleChannel(track.keys, s, 6, false)), f(sampleChannel(track.keys, s, 7, false)),
                        f(sampleChannel(track.keys, s, 8, false))};
    const Quat q1 = keyRotation(track.keys[s.k1]);
    if (s.interp == OcSeqInterp::Step || s.k1 == s.k2) {
        out.rotation = q1;
    } else {
        const f32 u = f(s.u);
        const f32 w = s.interp == OcSeqInterp::Smooth ? u * u * (3.0f - 2.0f * u) : u;
        out.rotation = Quat::slerp(q1, keyRotation(track.keys[s.k2]), w).normalized();   // slerp takes the short arc
    }
    return true;
}

bool sampleSeqCamera(const OcSeqTrack& track, f64 t, SeqCameraPose& out) {
    if (track.kind != OcSeqTrackKind::Camera || track.keys.empty()) return false;
    const Seg s = findSeg(track.keys, t);
    out.position = Vec3{f(sampleChannel(track.keys, s, 0, false)), f(sampleChannel(track.keys, s, 1, false)),
                        f(sampleChannel(track.keys, s, 2, false))};
    constexpr f64 kDeg2Rad = 3.14159265358979323846 / 180.0;
    out.yaw   = f(sampleChannel(track.keys, s, 3, true) * kDeg2Rad);
    out.pitch = f(sampleChannel(track.keys, s, 4, false) * kDeg2Rad);
    return true;
}

bool sampleSeqMaterial(const OcSeqTrack& track, f64 t, f32 outScale[3]) {
    if (track.kind != OcSeqTrackKind::Material || track.keys.empty()) return false;
    const Seg s = findSeg(track.keys, t);
    const f64 intensity = std::max(0.0, sampleChannel(track.keys, s, 3, false));
    for (int c = 0; c < 3; ++c)
        outScale[c] = f(std::max(0.0, sampleChannel(track.keys, s, c, false)) * intensity);
    return true;
}

void seqKeyFromTransform(const Transform& xf, fmt::OcSeqKey& key) {
    const Vec3 e = world::eulerDegFromQuat(xf.rotation);   // (roll, pitch, yaw)
    key.v[0] = xf.position.x; key.v[1] = xf.position.y; key.v[2] = xf.position.z;
    key.v[3] = e.z; key.v[4] = e.y; key.v[5] = e.x;
    key.v[6] = xf.scale.x; key.v[7] = xf.scale.y; key.v[8] = xf.scale.z;
}

void seqKeyFromCamera(const SeqCameraPose& pose, fmt::OcSeqKey& key) {
    constexpr f64 kRad2Deg = 180.0 / 3.14159265358979323846;
    key.v[0] = pose.position.x; key.v[1] = pose.position.y; key.v[2] = pose.position.z;
    key.v[3] = pose.yaw * kRad2Deg;
    key.v[4] = pose.pitch * kRad2Deg;
}

#if AVER_MODULE_SCENE

scene::Entity SequencePlayer::entityOf(i32 placement) const {
    if (placement < 0 || static_cast<usize>(placement) >= placementEntities_.size()) return scene::kInvalidEntity;
    return placementEntities_[static_cast<usize>(placement)];
}

namespace {
std::vector<scene::Entity> entitiesOf(const SequencePlayer& p, bool material) {
    std::vector<scene::Entity> out;
    std::unordered_set<scene::Entity> seen;
    for (const OcSeqTrack& tr : p.sequence().tracks) {
        const bool wanted = tr.kind == OcSeqTrackKind::Transform || (material && tr.kind == OcSeqTrackKind::Material);
        if (!wanted) continue;
        const scene::Entity e = p.entityOf(tr.target);
        if (e != scene::kInvalidEntity && seen.insert(e).second) out.push_back(e);
    }
    return out;
}
} // namespace

std::vector<scene::Entity> SequencePlayer::boundEntities() const { return entitiesOf(*this, true); }
std::vector<scene::Entity> SequencePlayer::transformEntities() const { return entitiesOf(*this, false); }

void SequencePlayer::advance(f64 dt) {
    if (!playing_) return;
    time_ += dt;
    if (seq_.loop) {
        time_ = seqWrapTime(seq_, time_);
    } else if (time_ >= seq_.length) {
        time_ = std::max(seq_.length, 0.0);
        playing_ = false;
    }
}

void SequencePlayer::evaluate(scene::World& world) {
    emissive_.clear();
    hasCamera_ = false;
    const f64 t = seqWrapTime(seq_, time_);
    for (const OcSeqTrack& tr : seq_.tracks) {
        switch (tr.kind) {
            case OcSeqTrackKind::Transform: {
                const scene::Entity e = entityOf(tr.target);
                Transform xf;
                if (e != scene::kInvalidEntity && world.valid(e) && sampleSeqTransform(tr, t, xf))
                    world.setLocalTransform(e, xf);
                break;
            }
            case OcSeqTrackKind::Camera: {
                SeqCameraPose pose;
                if (!hasCamera_ && sampleSeqCamera(tr, t, pose)) { camera_ = pose; hasCamera_ = true; }
                break;
            }
            case OcSeqTrackKind::Material: {
                const scene::Entity e = entityOf(tr.target);
                f32 rgb[3];
                if (e != scene::kInvalidEntity && sampleSeqMaterial(tr, t, rgb))
                    emissive_[e] = {rgb[0], rgb[1], rgb[2]};
                break;
            }
        }
    }
}

bool SequencePlayer::camera(SeqCameraPose& out) const {
    if (!hasCamera_) return false;
    out = camera_;
    return true;
}

bool SequencePlayer::emissiveScale(scene::Entity e, f32 out[3]) const {
    if (emissive_.empty()) return false;
    const auto it = emissive_.find(e);
    if (it == emissive_.end()) return false;
    out[0] = it->second[0]; out[1] = it->second[1]; out[2] = it->second[2];
    return true;
}

#endif

} // namespace aver::game
