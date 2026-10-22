// Rope planning and the tube around it.
#include "aver/softbody/Rope.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Vec3 anyPerpendicular(const Vec3& t) {
    const Vec3 ref = std::fabs(t.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    return cross(ref, t).getSafeNormal();
}

// The parabola from a to b with `sag` cm of drop along `perp` at the middle.
Vec3 curveAt(const Vec3& a, const Vec3& b, const Vec3& perp, f32 sag, f32 t) { return a + (b - a) * t + perp * (sag * 4.0f * t * (1.0f - t)); }

f32 curveLength(const Vec3& a, const Vec3& b, const Vec3& perp, f32 sag, u32 samples) {
    f32 len = 0.0f;
    Vec3 prev = a;
    for (u32 i = 1; i <= samples; ++i) {
        const Vec3 p = curveAt(a, b, perp, sag, static_cast<f32>(i) / static_cast<f32>(samples));
        len += (p - prev).size();
        prev = p;
    }
    return len;
}

}  // namespace

f32 polylineLength(const Vec3* nodes, u32 count) {
    f32 len = 0.0f;
    for (u32 i = 1; i < count; ++i) len += (nodes[i] - nodes[i - 1]).size();
    return len;
}

u32 nearestNode(const Vec3* nodes, u32 count, const Vec3& p) {
    u32 best = 0;
    f32 bestSq = 1e30f;
    for (u32 i = 0; i < count; ++i) {
        const f32 d = distSquared(nodes[i], p);
        if (d < bestSq) { bestSq = d; best = i; }
    }
    return best;
}

bool planRope(const Vec3& a, const Vec3& b, const RopeSpec& s, RopeBuild& out) {
    out = RopeBuild{};
    const auto fail = [&out](const char* why) {
        out.error = why;
        return false;
    };
    if (!finite3(a) || !finite3(b)) return fail("rope: an end is not finite");
    if (!(s.segmentCm > 0.0f) || !std::isfinite(s.segmentCm)) return fail("rope: segmentCm must be > 0");
    if (!(s.massPerMeterKg > 0.0f) || !std::isfinite(s.massPerMeterKg)) return fail("rope: massPerMeterKg must be > 0");
    const f32 chord = dist(a, b);
    f32 length = s.lengthCm > 0.0f ? s.lengthCm : chord * std::max(s.slack, 1.0f);
    length = std::max(length, chord);   // never shorter than the distance between the ends
    if (!(length > 1e-3f) || !std::isfinite(length)) return fail("rope: the length is zero");
    const u32 segs = std::max(2u, static_cast<u32>(std::ceil(length / s.segmentCm - 1e-4f)));
    if (segs + 1 > std::max(s.maxNodes, 3u)) return fail("rope: the rope needs more nodes than the cap (over the limit); raise segmentCm");

    out.nodes = segs + 1;
    out.rest.resize(out.nodes);

    if (chord < 1e-3f) {
        // Both ends on one point: a straight drop from it (a rope hanging from a hook with its end free).
        for (u32 i = 0; i < out.nodes; ++i)
            out.rest[i] = a + Vec3{0, 0, -length * static_cast<f32>(i) / static_cast<f32>(segs)};
    } else {
        const Vec3 c = (b - a) / chord;
        Vec3 perp = Vec3{0, 0, -1} - c * dot(Vec3{0, 0, -1}, c);
        perp = perp.sizeSquared() < 1e-6f ? anyPerpendicular(c) : perp.getSafeNormal();
        // The sag whose arc length is `length`, by bisection (the arc grows with the sag).
        f32 sag = 0.0f;
        if (length - chord > 1e-4f * length) {
            f32 lo = 0.0f, hi = length;
            for (int it = 0; it < 48; ++it) {
                const f32 mid = 0.5f * (lo + hi);
                if (curveLength(a, b, perp, mid, 96) < length) lo = mid; else hi = mid;
            }
            sag = 0.5f * (lo + hi);
        }
        // Resample at even arc length.
        constexpr u32 kSamples = 512;
        std::vector<f32> cum(kSamples + 1, 0.0f);
        std::vector<Vec3> pts(kSamples + 1);
        pts[0] = a;
        for (u32 i = 1; i <= kSamples; ++i) {
            pts[i] = curveAt(a, b, perp, sag, static_cast<f32>(i) / static_cast<f32>(kSamples));
            cum[i] = cum[i - 1] + (pts[i] - pts[i - 1]).size();
        }
        const f32 total = cum[kSamples];
        u32 m = 1;
        for (u32 i = 0; i < out.nodes; ++i) {
            const f32 target = total * static_cast<f32>(i) / static_cast<f32>(segs);
            while (m < kSamples && cum[m] < target) ++m;
            const f32 span = cum[m] - cum[m - 1];
            const f32 t = span > 1e-9f ? std::clamp((target - cum[m - 1]) / span, 0.0f, 1.0f) : 0.0f;
            out.rest[i] = lerp(pts[m - 1], pts[m], t);
        }
        out.rest[0] = a;
        out.rest[segs] = b;
    }

    out.restLengthCm = polylineLength(out.rest.data(), out.nodes);
    out.segmentCm = out.restLengthCm / static_cast<f32>(segs);
    out.nodeMassKg = s.massPerMeterKg * (length / static_cast<f32>(segs)) * 0.01f;
    out.invMass.assign(out.nodes, out.nodeMassKg > 1e-9f ? 1.0f / out.nodeMassKg : 1.0f);
    return true;
}

void tubeVertices(const Vec3* nodes, u32 n, const Quat* rodRotation, f32 radiusCm, u32 sides, Vec3* outPos) {
    if (n == 0 || sides < 3) return;
    const u32 ring = sides + 1;
    Vec3 prevT{1, 0, 0}, prevY{0, 1, 0};
    bool haveY = false;
    for (u32 i = 0; i < n; ++i) {
        // The tangent: central difference, one-sided at the ends, the previous one over a coincident pair.
        const u32 i0 = i > 0 ? i - 1 : 0, i1 = i + 1 < n ? i + 1 : n - 1;
        Vec3 t = (nodes[i1] - nodes[i0]).getSafeNormal();
        if (t.sizeSquared() < 0.5f) t = prevT;

        Vec3 y{0, 0, 0};
        if (rodRotation && n >= 2) {
            // The twist: the average of the two rods meeting at this node, made perpendicular to the tangent.
            const Quat qa = rodRotation[i > 0 ? i - 1 : 0], qb = rodRotation[i + 1 < n ? i : n - 2];
            const Quat q = Quat::slerp(qa, qb, 0.5f);
            const Vec3 raw = q.rotate(Vec3{0, 1, 0});
            y = (raw - t * dot(raw, t)).getSafeNormal();
        }
        if (y.sizeSquared() < 0.5f) {
            // Parallel transport from the previous ring (or any perpendicular for the first).
            if (haveY) y = (prevY - t * dot(prevY, t)).getSafeNormal();
            if (y.sizeSquared() < 0.5f) y = anyPerpendicular(t);
        }
        const Vec3 z = cross(t, y);
        for (u32 k = 0; k <= sides; ++k) {
            const f32 phi = kTwoPi * static_cast<f32>(k % sides) / static_cast<f32>(sides);
            outPos[i * ring + k] = nodes[i] + (y * std::cos(phi) + z * std::sin(phi)) * radiusCm;
        }
        prevT = t;
        prevY = y;
        haveY = true;
    }
}

void tubeUvs(const Vec3* nodes, u32 n, u32 sides, f32 uvCmPerTile, f32* outUv) {
    if (n == 0 || sides < 3) return;
    const u32 ring = sides + 1;
    const f32 inv = 1.0f / std::max(uvCmPerTile, 1e-3f);
    f32 arc = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        if (i > 0) arc += (nodes[i] - nodes[i - 1]).size();
        for (u32 k = 0; k <= sides; ++k) {
            outUv[(i * ring + k) * 2] = arc * inv;
            outUv[(i * ring + k) * 2 + 1] = static_cast<f32>(k) / static_cast<f32>(sides);
        }
    }
}

void tubeIndices(u32 n, u32 sides, std::vector<u32>& out) {
    out.clear();
    if (n < 2 || sides < 3) return;
    const u32 ring = sides + 1;
    out.reserve(static_cast<usize>(n - 1) * sides * 6);
    for (u32 i = 0; i + 1 < n; ++i)
        for (u32 k = 0; k < sides; ++k) {
            const u32 a = i * ring + k, b = a + 1, c = (i + 1) * ring + k, d = c + 1;
            out.insert(out.end(), {a, b, c, b, d, c});
        }
}

}  // namespace aver::softbody
