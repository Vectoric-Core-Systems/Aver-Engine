// Music crossfading and the occlusion ray probe.
#include "aver/audio/Music.hpp"

#include <algorithm>
#include <cmath>

namespace aver::audio {

VoiceHandle MusicCrossfader::play(std::unique_ptr<StreamSource> source, const Options& o) {
    if (!mixer_ || !source) return 0;

    PlayDesc d;
    d.volume = o.volume;
    d.looping = o.looping;
    d.bus = o.bus;
    d.fadeInSeconds = o.fadeSeconds;
    d.fadeCurve = o.curve;
    const VoiceHandle next = mixer_->playStream(std::move(source), d, o.stream);
    if (next == 0) return 0;    // keep the old track rather than going silent on a failed start

    if (current_ != 0) mixer_->fadeVoice(current_, 0.0f, o.fadeSeconds, o.curve, true);
    current_ = next;
    return next;
}

void MusicCrossfader::stop(f32 fadeSeconds, FadeCurve curve) {
    if (!mixer_ || current_ == 0) return;
    mixer_->fadeVoice(current_, 0.0f, fadeSeconds, curve, true);
    current_ = 0;
}

VoiceHandle MusicCrossfader::current() const {
    return mixer_ && current_ != 0 && mixer_->playing(current_) ? current_ : 0;
}

// ---------------------------------------------------------------- occlusion

namespace {

// A frame around the line of sight `d` (unit): `right` and `up` span the plane the probe ring lies in.
void lineFrame(const f32 d[3], f32 right[3], f32 up[3]) {
    const f32 refUp[3] = {std::fabs(d[2]) > 0.99f ? 1.0f : 0.0f, 0.0f, std::fabs(d[2]) > 0.99f ? 0.0f : 1.0f};
    right[0] = d[1] * refUp[2] - d[2] * refUp[1];
    right[1] = d[2] * refUp[0] - d[0] * refUp[2];
    right[2] = d[0] * refUp[1] - d[1] * refUp[0];
    const f32 rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (int k = 0; k < 3; ++k) right[k] /= rl;
    up[0] = right[1] * d[2] - right[2] * d[1];
    up[1] = right[2] * d[0] - right[0] * d[2];
    up[2] = right[0] * d[1] - right[1] * d[0];
}

} // namespace

f32 measureOcclusion(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3],
                     const OcclusionProbe& probe) {
    if (!fn) return 0.0f;
    f32 d[3] = {source[0] - listener[0], source[1] - listener[1], source[2] - listener[2]};
    const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0e-3f) return 0.0f;
    for (f32& c : d) c /= len;

    f32 right[3], up[3];
    lineFrame(d, right, up);

    u32 n = probe.rayCount < 1 ? 1 : (probe.rayCount > 16 ? 16 : probe.rayCount);
    f32 sum = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        f32 to[3] = {source[0], source[1], source[2]};
        if (i > 0) {
            const f32 a = 6.28318530718f * static_cast<f32>(i - 1) / static_cast<f32>(n - 1);
            const f32 ca = std::cos(a) * probe.probeRadiusCm, sa = std::sin(a) * probe.probeRadiusCm;
            for (int k = 0; k < 3; ++k) to[k] += right[k] * ca + up[k] * sa;
        }
        f32 b = fn(user, listener, to);
        sum += b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
    }
    return sum / static_cast<f32>(n);
}

f32 smoothOcclusion(f32 current, f32 measured, f32 dt, f32 riseSec, f32 fallSec) {
    const f32 tau = measured > current ? riseSec : fallSec;
    if (dt <= 0.0f) return current;
    if (tau <= 1.0e-6f) return measured;
    return current + (measured - current) * (1.0f - std::exp(-dt / tau));
}

// ---------------------------------------------------------------- diffraction

f32 diffractionOcclusion(f32 directLengthCm, f32 pathLengthCm, f32 bendRadians, f32 maxOcclusion) {
    const f32 pathGain = pathLengthCm > 1.0e-3f ? std::fmin(1.0f, std::fmax(0.0f, directLengthCm / pathLengthCm)) : 1.0f;
    const f32 bendGain = 0.5f * (1.0f + std::cos(bendRadians));
    const f32 occ = maxOcclusion * (1.0f - pathGain * bendGain);
    return std::fmin(maxOcclusion, std::fmax(0.0f, occ));
}

namespace {

f32 dist3(const f32 a[3], const f32 b[3]) {
    const f32 x = b[0] - a[0], y = b[1] - a[1], z = b[2] - a[2];
    return std::sqrt(x * x + y * y + z * z);
}

// One waypoint line: a direction around the line of sight and a position along it, with the bracket the offset search has narrowed to.
struct DiffLine {
    f32 along = 0.5f;         // fraction of the way from the listener to the source
    f32 dir[3] = {};          // unit offset direction, perpendicular to the line of sight
    f32 lo = 0.0f, hi = 0.0f; // lo: largest offset known blocked (0 = the line of sight); hi: smallest known clear
    bool clear = false;
    f32 len = 0.0f;          // path length at hi
};

} // namespace

DiffractionPath findDiffractionPath(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3], const DiffractionProbe& probe) {
    DiffractionPath out;
    out.occlusion = 1.0f;
    const f32 dx = source[0] - listener[0], dy = source[1] - listener[1], dz = source[2] - listener[2];
    const f32 direct = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.directLengthCm = direct;
    if (!fn || direct < 1.0e-3f) return out;
    const f32 d[3] = {dx / direct, dy / direct, dz / direct};
    f32 right[3], up[3];
    lineFrame(d, right, up);

    const u32 dirs = probe.directions < 2 ? 2u : (probe.directions > 16 ? 16u : probe.directions);
    const f32 along[3] = {0.25f, 0.5f, 0.75f};
    DiffLine lines[16 * 3];
    u32 count = 0;
    for (u32 a = 0; a < dirs; ++a) {
        const f32 ang = 6.28318530718f * static_cast<f32>(a) / static_cast<f32>(dirs);
        const f32 c = std::cos(ang), s = std::sin(ang);
        for (f32 t : along) {
            DiffLine& l = lines[count++];
            l.along = t;
            for (int k = 0; k < 3; ++k) l.dir[k] = right[k] * c + up[k] * s;
        }
    }

    u32 rays = 0;
    auto at = [&](const DiffLine& l, f32 r, f32 p[3]) {
        for (int k = 0; k < 3; ++k) p[k] = listener[k] + d[k] * (l.along * direct) + l.dir[k] * r;
    };
    // A path is clear when both legs are; the second leg is not cast when the first is blocked. False when the budget is spent.
    auto legsClear = [&](const f32 p[3], bool& clear) {
        if (rays >= probe.maxRays) return false;
        ++rays;
        if (fn(user, listener, p) >= 0.5f) { clear = false; return true; }
        if (rays >= probe.maxRays) return false;
        ++rays;
        clear = fn(user, p, source) < 0.5f;
        return true;
    };

    const f32 first = probe.firstOffsetCm > 1.0f ? probe.firstOffsetCm : 1.0f;
    bool budget = true, any = false;
    for (f32 r = first; budget && !any; r *= 2.0f) {
        for (u32 i = 0; i < count && budget; ++i) {
            DiffLine& l = lines[i];
            f32 p[3];
            at(l, r, p);
            bool clear = false;
            if (!legsClear(p, clear)) { budget = false; break; }
            if (clear) { l.clear = true; l.hi = r; any = true; }
            else l.lo = r;
        }
        if (r >= probe.maxOffsetCm) break;
    }
    if (!any) { out.raysUsed = rays; return out; }

    auto length = [&](const DiffLine& l, f32 r) {
        f32 p[3];
        at(l, r, p);
        return dist3(listener, p) + dist3(p, source);
    };
    for (u32 i = 0; i < count; ++i) if (lines[i].clear) lines[i].len = length(lines[i], lines[i].hi);

    // Tighten the shortest few to the edge: bisect between the blocked offset below and the clear one.
    u32 order[16 * 3];
    u32 nClear = 0;
    for (u32 i = 0; i < count; ++i) if (lines[i].clear) order[nClear++] = i;
    std::sort(order, order + nClear, [&](u32 a, u32 b) { return lines[a].len < lines[b].len; });
    const u32 refine = nClear < 4 ? nClear : 4;
    for (u32 n = 0; n < refine; ++n) {
        DiffLine& l = lines[order[n]];
        for (int it = 0; it < 3; ++it) {
            const f32 mid = 0.5f * (l.lo + l.hi);
            f32 p[3];
            at(l, mid, p);
            bool clear = false;
            if (!legsClear(p, clear)) break;
            if (clear) l.hi = mid; else l.lo = mid;
        }
        l.len = length(l, l.hi);
    }

    const DiffLine* best = nullptr;
    for (u32 n = 0; n < nClear; ++n) {
        const DiffLine& l = lines[order[n]];
        if (!best || l.len < best->len) best = &l;
    }
    f32 p[3];
    at(*best, best->hi, p);
    const f32 a = dist3(listener, p), b = dist3(p, source);
    f32 cosBend = 1.0f;
    if (a > 1.0e-3f && b > 1.0e-3f)
        cosBend = ((p[0] - listener[0]) * (source[0] - p[0]) + (p[1] - listener[1]) * (source[1] - p[1]) + (p[2] - listener[2]) * (source[2] - p[2])) / (a * b);
    cosBend = std::fmin(1.0f, std::fmax(-1.0f, cosBend));

    out.found = true;
    out.pathLengthCm = a + b;
    out.detourCm = std::fmax(0.0f, out.pathLengthCm - direct);
    out.bendRadians = std::acos(cosBend);
    out.occlusion = diffractionOcclusion(direct, out.pathLengthCm, out.bendRadians, probe.maxOcclusion);
    out.pathGain = out.pathLengthCm > 1.0e-3f ? std::fmin(1.0f, direct / out.pathLengthCm) : 1.0f;
    out.bendGain = 0.5f * (1.0f + cosBend);
    for (int k = 0; k < 3; ++k) out.waypoint[k] = p[k];
    out.raysUsed = rays;
    return out;
}

f32 applyDiffraction(f32 directOcclusion, const DiffractionPath& path) {
    return path.found && path.occlusion < directOcclusion ? path.occlusion : directOcclusion;
}

f32 measureOcclusionDiffracted(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3], const OcclusionProbe& directProbe,
                               const DiffractionProbe& probe, DiffractionPath* path) {
    const f32 direct = measureOcclusion(fn, user, listener, source, directProbe);
    DiffractionPath found;
    if (direct > probe.trigger) found = findDiffractionPath(fn, user, listener, source, probe);
    if (path) *path = found;
    return applyDiffraction(direct, found);
}

} // namespace aver::audio
