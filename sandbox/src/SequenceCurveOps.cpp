// The sequencer's curve editing (SequenceCurveOps.hpp).
#include "SequenceCurveOps.hpp"

#include "aver/game/LevelSequence.hpp"

#include <algorithm>
#include <cmath>

namespace aver::editor {

using fmt::OcSeqInterp;
using fmt::OcSeqKey;
using fmt::OcSeqTrack;

namespace {

// Fills `tan` with the slopes the key is using now (own or automatic) for every channel, so editing one
// channel leaves the others where they were.
void materialise(OcSeqTrack& tr, usize key) {
    OcSeqKey& k = tr.keys[key];
    const u32 n = seqTangentChannels(tr);
    if (k.tan.size() >= 2u * n) return;
    std::vector<f64> t(2u * n, 0.0);
    for (u32 c = 0; c < n; ++c) game::seqEffectiveTangent(tr.keys, key, c, t[2u * c], t[2u * c + 1u]);
    k.tan = std::move(t);
}

usize insertSorted(std::vector<OcSeqKey>& keys, OcSeqKey k) {
    const auto it = std::upper_bound(keys.begin(), keys.end(), k.t, [](f64 t, const OcSeqKey& a) { return t < a.t; });
    // Two statements, not one: in `keys.insert(...) - keys.begin()` the operands are unsequenced, and a
    // compiler that reads begin() first subtracts a pre-reallocation iterator from a post-insert one.
    const auto at = keys.insert(it, std::move(k));
    return static_cast<usize>(at - keys.begin());
}

} // namespace

u32 seqTangentChannels(const OcSeqTrack& track) {
    return track.kind == fmt::OcSeqTrackKind::Property ? 4u : game::seqChannelCount(track, 4);
}

void seqSetKeyInterp(OcSeqTrack& track, usize key, OcSeqInterp interp) {
    if (key < track.keys.size()) track.keys[key].interp = interp;
}

SeqTangentMode seqKeyTangentMode(const OcSeqKey& key, u32 channel) {
    if (key.tan.size() <= 2u * channel + 1u) return SeqTangentMode::Auto;
    const f64 a = key.tan[2u * channel], l = key.tan[2u * channel + 1u];
    if (a != l) return SeqTangentMode::Broken;
    return l == 0.0 ? SeqTangentMode::Flat : SeqTangentMode::User;
}

void seqSetTangentMode(OcSeqTrack& tr, usize key, SeqTangentMode mode) {
    if (key >= tr.keys.size()) return;
    OcSeqKey& k = tr.keys[key];
    const u32 n = seqTangentChannels(tr);
    switch (mode) {
        case SeqTangentMode::Auto:
            k.tan.clear();
            return;
        case SeqTangentMode::Flat:
            k.tan.assign(2u * n, 0.0);
            return;
        case SeqTangentMode::Linear: {
            const bool hasPrev = key > 0, hasNext = key + 1 < tr.keys.size();
            std::vector<f64> t(2u * n, 0.0);
            for (u32 c = 0; c < n; ++c) {
                f64 in = 0.0, out = 0.0;
                if (hasPrev) {
                    const f64 dt = k.t - tr.keys[key - 1].t;
                    if (dt > 1e-12) in = (k.v[c] - tr.keys[key - 1].v[c]) / dt;
                }
                if (hasNext) {
                    const f64 dt = tr.keys[key + 1].t - k.t;
                    if (dt > 1e-12) out = (tr.keys[key + 1].v[c] - k.v[c]) / dt;
                }
                if (!hasPrev) in = out;
                if (!hasNext) out = in;
                t[2u * c] = in;
                t[2u * c + 1u] = out;
            }
            k.tan = std::move(t);
            return;
        }
        case SeqTangentMode::User:
            materialise(tr, key);
            for (u32 c = 0; c < n; ++c) k.tan[2u * c] = k.tan[2u * c + 1u];
            return;
        case SeqTangentMode::Broken:
            materialise(tr, key);
            return;
    }
}

void seqSetTangent(OcSeqTrack& tr, usize key, u32 channel, f64 arrive, f64 leave, bool broken) {
    if (key >= tr.keys.size() || channel >= seqTangentChannels(tr)) return;
    materialise(tr, key);
    OcSeqKey& k = tr.keys[key];
    k.tan[2u * channel] = arrive;
    k.tan[2u * channel + 1u] = broken ? leave : arrive;
}

usize seqMoveKey(OcSeqTrack& tr, usize key, f64 t, u32 channel, f64 value, f64 sequenceLength) {
    if (key >= tr.keys.size()) return key;
    OcSeqKey k = std::move(tr.keys[key]);
    tr.keys.erase(tr.keys.begin() + static_cast<std::ptrdiff_t>(key));
    k.t = sequenceLength > 0.0 ? std::clamp(t, 0.0, sequenceLength) : std::max(t, 0.0);
    if (channel < 9) k.v[channel] = value;
    return insertSorted(tr.keys, std::move(k));
}

usize seqInsertKeyAt(OcSeqTrack& tr, f64 t, u32 propertyArity) {
    const u32 n = game::seqChannelCount(tr, propertyArity);
    OcSeqKey nk;
    nk.t = t;
    for (u32 c = 0; c < n; ++c) nk.v[c] = game::sampleSeqChannel(tr, c, t);
    // The segment the new key splits.
    usize left = tr.keys.size();   // index of the last key at or before t
    for (usize i = 0; i < tr.keys.size() && tr.keys[i].t <= t; ++i) left = i;
    if (left == tr.keys.size()) {   // before every key
        nk.interp = tr.keys.empty() ? OcSeqInterp::Smooth : tr.keys.front().interp;
        return insertSorted(tr.keys, std::move(nk));
    }
    nk.interp = tr.keys[left].interp;
    if (left + 1 < tr.keys.size() && tr.keys[left].interp == OcSeqInterp::Cubic) {
        // A Hermite segment splits into two with the same curve: pin the neighbours' slopes (they would
        // otherwise be recomputed against the new key) and give the new key the curve's own slope.
        materialise(tr, left);
        materialise(tr, left + 1);
        const OcSeqKey& a = tr.keys[left];
        const OcSeqKey& b = tr.keys[left + 1];
        const f64 dt = b.t - a.t;
        if (dt > 1e-12) {
            const f64 u = std::clamp((t - a.t) / dt, 0.0, 1.0), u2 = u * u;
            const u32 tc = seqTangentChannels(tr);
            nk.tan.assign(2u * tc, 0.0);
            for (u32 c = 0; c < tc && c < 9; ++c) {
                const f64 m1 = a.tan[2u * c + 1u], m2 = b.tan[2u * c];
                const f64 slope = ((6.0 * u2 - 6.0 * u) * a.v[c] + (3.0 * u2 - 4.0 * u + 1.0) * dt * m1 +
                                   (-6.0 * u2 + 6.0 * u) * b.v[c] + (3.0 * u2 - 2.0 * u) * dt * m2) / dt;
                nk.tan[2u * c] = nk.tan[2u * c + 1u] = slope;
            }
        }
    }
    return insertSorted(tr.keys, std::move(nk));
}

void seqCurveBounds(const OcSeqTrack& tr, u32 mask, u32 propertyArity, f64& t0, f64& t1, f64& v0, f64& v1) {
    t0 = 0.0; t1 = 1.0; v0 = 0.0; v1 = 1.0;
    if (tr.keys.empty()) return;
    const u32 n = game::seqChannelCount(tr, propertyArity);
    t0 = tr.keys.front().t;
    t1 = tr.keys.back().t;
    bool any = false;
    for (const OcSeqKey& k : tr.keys)
        for (u32 c = 0; c < n && c < 32; ++c) {
            if (!(mask & (1u << c))) continue;
            if (!any) { v0 = v1 = k.v[c]; any = true; }
            v0 = std::min(v0, k.v[c]);
            v1 = std::max(v1, k.v[c]);
        }
    if (!any) { v0 = 0.0; v1 = 1.0; }
    if (t1 - t0 < 1e-6) t1 = t0 + 1.0;
    if (v1 - v0 < 1e-9) { v0 -= 0.5; v1 += 0.5; }
}

} // namespace aver::editor
