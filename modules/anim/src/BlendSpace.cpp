// Blend space weights, triangulation, sync markers and the player. See BlendSpace.hpp.
#include "aver/anim/BlendSpace.hpp"
#include "aver/anim/AnimSampler.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {

namespace {

constexpr f64 kEps = 1e-9;

f32 axisRange(const BlendAxis& a) {
    const f32 r = a.max - a.min;
    return r > 1e-6f ? r : 1.0f;
}

Vec2 normalise(const BlendSpaceAsset& s, f32 x, f32 y) {
    return Vec2{(x - s.axisX.min) / axisRange(s.axisX),
                s.dims >= 2 ? (y - s.axisY.min) / axisRange(s.axisY) : 0.0f};
}

f64 orient(f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// Positive when d is strictly inside the circumcircle of the CCW triangle abc.
f64 inCircle(const std::array<f64, 2>& a, const std::array<f64, 2>& b, const std::array<f64, 2>& c,
             const std::array<f64, 2>& d) {
    const f64 adx = a[0] - d[0], ady = a[1] - d[1];
    const f64 bdx = b[0] - d[0], bdy = b[1] - d[1];
    const f64 cdx = c[0] - d[0], cdy = c[1] - d[1];
    const f64 ad = adx * adx + ady * ady, bd = bdx * bdx + bdy * bdy, cd = cdx * cdx + cdy * cdy;
    return adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) + ad * (bdx * cdy - bdy * cdx);
}

// 1D lerp along `order` by coordinate `coord(i)`; clamps outside.
template <class F>
void weights1D(const std::vector<u32>& order, F coord, f32 q, std::vector<SampleWeight>& out) {
    if (order.empty()) return;
    if (order.size() == 1 || q <= coord(order.front())) { out.push_back({order.front(), 1.0f}); return; }
    if (q >= coord(order.back())) { out.push_back({order.back(), 1.0f}); return; }
    for (usize k = 0; k + 1 < order.size(); ++k) {
        const f32 a = coord(order[k]), b = coord(order[k + 1]);
        if (q >= a && q <= b) {
            const f32 span = b - a;
            const f32 t = span > 1e-9f ? (q - a) / span : 0.0f;
            if (t < 1.0f) out.push_back({order[k], 1.0f - t});
            if (t > 0.0f) out.push_back({order[k + 1], t});
            return;
        }
    }
    out.push_back({order.back(), 1.0f});
}

// Barycentric of p in (a,b,c); u,v,w sum to 1 when the triangle is not degenerate.
bool bary(Vec2 p, Vec2 a, Vec2 b, Vec2 c, f32& u, f32& v, f32& w) {
    const f64 d = orient(a.x, a.y, b.x, b.y, c.x, c.y);
    if (std::fabs(d) < kEps) return false;
    u = static_cast<f32>(orient(p.x, p.y, b.x, b.y, c.x, c.y) / d);
    v = static_cast<f32>(orient(a.x, a.y, p.x, p.y, c.x, c.y) / d);
    w = 1.0f - u - v;
    return true;
}

} // namespace

bool BlendSpaceAsset::valid(std::string* why) const {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (dims != 1 && dims != 2) return fail("dims must be 1 or 2");
    if (samples.empty()) return fail("a blend space needs at least one sample");
    if (axisX.max <= axisX.min) return fail("axis X range is empty");
    if (dims == 2 && axisY.max <= axisY.min) return fail("axis Y range is empty");
    for (const BlendSample& s : samples) {
        if (s.clip.empty()) return fail("a sample has no clip");
        if (!std::isfinite(s.x) || !std::isfinite(s.y)) return fail("a sample position is not finite");
        if (s.rate <= 0.0f) return fail("a sample rate must be positive");
    }
    return true;
}

BlendTopology buildBlendTopology(const BlendSpaceAsset& space) {
    BlendTopology t;
    const u32 n = static_cast<u32>(space.samples.size());
    t.pts.resize(n);
    t.order.resize(n);
    for (u32 i = 0; i < n; ++i) {
        t.pts[i] = normalise(space, space.samples[i].x, space.samples[i].y);
        t.order[i] = i;
    }
    std::stable_sort(t.order.begin(), t.order.end(), [&](u32 a, u32 b) {
        return space.samples[a].x < space.samples[b].x;
    });
    if (space.dims < 2 || n < 2) return t;

    // Deterministic jitter breaks the cocircular ties of grid layouts, so exactly one diagonal of
    // each square passes the empty-circle test. Real positions are used for the weights.
    std::vector<std::array<f64, 2>> p(n);
    for (u32 i = 0; i < n; ++i) {
        const f64 jx = std::fmod(std::sin(i * 12.9898) * 43758.5453, 1.0);
        const f64 jy = std::fmod(std::sin(i * 78.2330) * 24634.6345, 1.0);
        p[i] = {t.pts[i].x + 1e-7 * jx, t.pts[i].y + 1e-7 * jy};
    }
    std::vector<bool> dup(n, false);
    for (u32 i = 0; i < n; ++i)
        for (u32 j = 0; j < i && !dup[i]; ++j) {
            const f64 dx = t.pts[i].x - t.pts[j].x, dy = t.pts[i].y - t.pts[j].y;
            if (dx * dx + dy * dy < 1e-12) dup[i] = true;
        }

    // Brute-force Delaunay: a blend space has a few dozen points and this runs once per asset.
    for (u32 a = 0; a < n; ++a) {
        if (dup[a]) continue;
        for (u32 b = a + 1; b < n; ++b) {
            if (dup[b]) continue;
            for (u32 c = b + 1; c < n; ++c) {
                if (dup[c]) continue;
                const f64 area = orient(t.pts[a].x, t.pts[a].y, t.pts[b].x, t.pts[b].y, t.pts[c].x, t.pts[c].y);
                if (std::fabs(area) < 1e-9) continue;
                u32 va = a, vb = b, vc = c;
                if (orient(p[va][0], p[va][1], p[vb][0], p[vb][1], p[vc][0], p[vc][1]) < 0) std::swap(vb, vc);
                bool empty = true;
                for (u32 d = 0; d < n && empty; ++d) {
                    if (d == a || d == b || d == c || dup[d]) continue;
                    if (inCircle(p[va], p[vb], p[vc], p[d]) > 1e-14) empty = false;
                }
                if (empty) t.tris.push_back({va, vb, vc});
            }
        }
    }

    if (t.tris.empty()) {
        // All distinct points on one line: weight along it.
        t.collinear = true;
        f64 best = -1.0;
        u32 ba = 0, bb = 0;
        for (u32 i = 0; i < n; ++i)
            for (u32 j = i + 1; j < n; ++j) {
                const f64 dx = t.pts[j].x - t.pts[i].x, dy = t.pts[j].y - t.pts[i].y;
                if (dx * dx + dy * dy > best) { best = dx * dx + dy * dy; ba = i; bb = j; }
            }
        if (best > 0) {
            const f32 len = static_cast<f32>(std::sqrt(best));
            t.dir = Vec2{(t.pts[bb].x - t.pts[ba].x) / len, (t.pts[bb].y - t.pts[ba].y) / len};
        }
        std::stable_sort(t.order.begin(), t.order.end(), [&](u32 x, u32 y) {
            return t.pts[x].x * t.dir.x + t.pts[x].y * t.dir.y < t.pts[y].x * t.dir.x + t.pts[y].y * t.dir.y;
        });
    }
    return t;
}

void computeBlendWeights(const BlendSpaceAsset& space, const BlendTopology& topo, f32 x, f32 y,
                         std::vector<SampleWeight>& out) {
    out.clear();
    if (space.samples.empty()) return;

    if (space.dims < 2) {
        weights1D(topo.order, [&](u32 i) { return space.samples[i].x; }, x, out);
    } else if (topo.collinear) {
        const Vec2 q = normalise(space, x, y);
        weights1D(topo.order,
                  [&](u32 i) { return topo.pts[i].x * topo.dir.x + topo.pts[i].y * topo.dir.y; },
                  q.x * topo.dir.x + q.y * topo.dir.y, out);
    } else if (topo.tris.empty()) {
        out.push_back({0, 1.0f});
    } else {
        const Vec2 q = normalise(space, x, y);
        bool found = false;
        for (const auto& tri : topo.tris) {
            f32 u, v, w;
            if (!bary(q, topo.pts[tri[0]], topo.pts[tri[1]], topo.pts[tri[2]], u, v, w)) continue;
            constexpr f32 tol = -1e-5f;
            if (u < tol || v < tol || w < tol) continue;
            out.push_back({tri[0], std::max(u, 0.0f)});
            out.push_back({tri[1], std::max(v, 0.0f)});
            out.push_back({tri[2], std::max(w, 0.0f)});
            found = true;
            break;
        }
        if (!found) {
            // Outside the hull: nearest point on any triangle edge (the hull edge is one of them).
            f64 bestD = 1e300;
            u32 ea = 0, eb = 0;
            f32 et = 0;
            for (const auto& tri : topo.tris)
                for (int k = 0; k < 3; ++k) {
                    const u32 ia = tri[k], ib = tri[(k + 1) % 3];
                    const Vec2 a = topo.pts[ia], b = topo.pts[ib];
                    const f64 ex = b.x - a.x, ey = b.y - a.y;
                    const f64 l2 = ex * ex + ey * ey;
                    f64 s = l2 > kEps ? ((q.x - a.x) * ex + (q.y - a.y) * ey) / l2 : 0.0;
                    s = std::clamp(s, 0.0, 1.0);
                    const f64 dx = a.x + s * ex - q.x, dy = a.y + s * ey - q.y;
                    const f64 d = dx * dx + dy * dy;
                    if (d < bestD) { bestD = d; ea = ia; eb = ib; et = static_cast<f32>(s); }
                }
            out.push_back({ea, 1.0f - et});
            out.push_back({eb, et});
        }
    }

    // Drop dust and make the sum exactly one.
    f32 sum = 0.0f;
    for (const SampleWeight& w : out) sum += w.weight;
    if (sum <= 0.0f) { out.assign(1, SampleWeight{out.empty() ? 0u : out.front().index, 1.0f}); return; }
    usize k = 0;
    for (usize i = 0; i < out.size(); ++i) {
        const f32 w = out[i].weight / sum;
        if (w < 1e-6f) continue;
        out[k++] = {out[i].index, w};
    }
    out.resize(k);
    f32 s2 = 0.0f;
    for (const SampleWeight& w : out) s2 += w.weight;
    for (SampleWeight& w : out) w.weight /= s2;
}

bool makeSyncCycle(const std::vector<SyncMarker>& markers, f32 duration,
                   const std::vector<std::string>& names, SyncCycle& out) {
    out.duration = duration;
    out.times.clear();
    if (markers.empty() || names.size() != markers.size() || duration <= 0.0f) return false;
    std::vector<SyncMarker> m = markers;
    std::stable_sort(m.begin(), m.end(), [](const SyncMarker& a, const SyncMarker& b) { return a.time < b.time; });
    usize r = m.size();
    for (usize i = 0; i < m.size(); ++i)
        if (m[i].name == names[0]) { r = i; break; }
    if (r == m.size()) return false;
    std::vector<f32> times(m.size());
    for (usize i = 0; i < m.size(); ++i) {
        const SyncMarker& s = m[(r + i) % m.size()];
        if (s.name != names[i]) return false;
        times[i] = s.time;
    }
    out.times = std::move(times);
    return true;
}

f32 syncPhaseToTime(const SyncCycle& c, f32 phase) {
    phase -= std::floor(phase);
    if (c.duration <= 0.0f) return 0.0f;
    const usize n = c.times.size();
    if (n == 0) return phase * c.duration;
    const f32 scaled = phase * static_cast<f32>(n);
    const usize k = std::min(static_cast<usize>(scaled), n - 1);
    const f32 frac = scaled - static_cast<f32>(k);
    const f32 a = c.times[k];
    f32 len = c.times[(k + 1) % n] - a;
    if (len <= 0.0f) len += c.duration;
    f32 t = a + frac * len;
    t = std::fmod(t, c.duration);
    return t < 0.0f ? t + c.duration : t;
}

f32 syncTimeToPhase(const SyncCycle& c, f32 time) {
    if (c.duration <= 0.0f) return 0.0f;
    time = std::fmod(time, c.duration);
    if (time < 0.0f) time += c.duration;
    const usize n = c.times.size();
    if (n == 0) return time / c.duration;
    for (usize k = 0; k < n; ++k) {
        f32 len = c.times[(k + 1) % n] - c.times[k];
        if (len <= 0.0f) len += c.duration;
        f32 off = time - c.times[k];
        if (off < 0.0f) off += c.duration;
        if (off < len || k + 1 == n) return (static_cast<f32>(k) + std::min(off / len, 1.0f)) / static_cast<f32>(n);
    }
    return 0.0f;
}

void BlendSpacePlayer::bind(const BlendSpaceAsset* space, std::vector<const fmt::OcAnimation*> clips) {
    std::vector<f32> d(clips.size(), 1.0f);
    for (usize i = 0; i < clips.size(); ++i)
        if (clips[i] && clips[i]->duration > 0.0f) d[i] = clips[i]->duration;
    bindDurations(space, std::move(d));
    clips_ = std::move(clips);
}

void BlendSpacePlayer::bindDurations(const BlendSpaceAsset* space, std::vector<f32> durations) {
    space_ = space;
    durations_ = std::move(durations);
    clips_.assign(durations_.size(), nullptr);
    topo_ = space ? buildBlendTopology(*space) : BlendTopology{};
    phase_ = 0.0f;
    elapsed_ = 0.0f;
    sync_.assign(durations_.size(), SyncCycle{});
    refresh();
}

void BlendSpacePlayer::setPhase(f32 p) {
    phase_ = p - std::floor(p);
    refresh();
}

void BlendSpacePlayer::refresh() {
    weights_.clear();
    if (!space_ || space_->samples.empty()) return;
    computeBlendWeights(*space_, topo_, x_, y_, weights_);
    const usize n = std::min(space_->samples.size(), durations_.size());

    f32 dur = 0.0f;
    for (const SampleWeight& w : weights_) {
        if (w.index >= n) continue;
        const f32 d = durations_[w.index] > 0.0f ? durations_[w.index] : 1.0f;
        dur += w.weight * d / std::max(space_->samples[w.index].rate, 1e-3f);
    }
    blendedDuration_ = dur > 1e-4f ? dur : 1.0f;

    // The leader (heaviest sample with markers) names the shared cycle; every other active sample is
    // rotated onto it, or falls back to linear phase when its markers do not match.
    std::vector<std::string> names;
    if (space_->syncMarkers) {
        f32 best = -1.0f;
        for (const SampleWeight& w : weights_) {
            if (w.index >= n || space_->samples[w.index].markers.empty() || w.weight <= best) continue;
            best = w.weight;
            std::vector<SyncMarker> m = space_->samples[w.index].markers;
            std::stable_sort(m.begin(), m.end(), [](const SyncMarker& a, const SyncMarker& b) { return a.time < b.time; });
            names.clear();
            for (const SyncMarker& s : m) names.push_back(s.name);
        }
    }
    for (const SampleWeight& w : weights_) {
        if (w.index >= n) continue;
        SyncCycle& c = sync_[w.index];
        const f32 d = durations_[w.index] > 0.0f ? durations_[w.index] : 1.0f;
        if (names.empty() || !makeSyncCycle(space_->samples[w.index].markers, d, names, c)) {
            c.duration = d;
            c.times.clear();
        }
    }
}

void BlendSpacePlayer::advance(f32 dt) {
    if (!space_) return;
    auto follow = [&](f32& v, f32 target, f32 smoothing) {
        v = smoothing > 0.0f ? v + (target - v) * (1.0f - std::exp(-dt / smoothing)) : target;
    };
    follow(x_, tx_, space_->axisX.smoothing);
    follow(y_, ty_, space_->axisY.smoothing);
    refresh();
    const f32 step = dt / blendedDuration_;
    phase_ += step;
    elapsed_ += step;
    phase_ -= std::floor(phase_);
}

f32 BlendSpacePlayer::sampleTime(u32 i) const {
    if (i >= sync_.size()) return 0.0f;
    return syncPhaseToTime(sync_[i], phase_);
}

void BlendSpacePlayer::evaluate(const fmt::OcSkeleton& skel, Pose& out) const {
    restPose(skel, out);
    f32 acc = 0.0f;
    Pose tmp, mix;
    for (const SampleWeight& w : weights_) {
        if (w.index >= clips_.size()) continue;
        restPose(skel, tmp);
        if (clips_[w.index]) sampleAnimation(*clips_[w.index], sampleTime(w.index), tmp);
        if (acc <= 0.0f) { out = tmp; acc = w.weight; continue; }
        blendPose(out, tmp, w.weight / (acc + w.weight), mix);
        out = mix;
        acc += w.weight;
    }
}

} // namespace aver::anim
