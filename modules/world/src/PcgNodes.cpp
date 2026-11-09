// The PCG node set. Each node is a plain function (params, inputs, node seed, context) -> outputs; the evaluator in
// PcgGraph.cpp owns scheduling, caching, cancellation and the nodes that need the evaluator itself (Input, Output,
// Subgraph, Loop).
//
// Determinism rules every node follows:
//   * a sampler's points come from a WORLD-anchored lattice and are seeded from the lattice index, so the same point
//     exists with the same seed whatever region is being evaluated;
//   * per-point randomness is hash(point.seed, node seed, stream), never a running counter;
//   * ties in any ordering fall back to (seed, x, y, z, index).
#include "aver/pcg/PcgVolume.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/world/PcgGraph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace aver::world {
namespace {

using In = std::vector<const PcgPoints*>;
constexpr f32 kDeg2Rad = 0.017453292519943295f;
constexpr f32 kRad2Deg = 57.29577951308232f;

const PcgPoints& inAt(const In& in, usize i) {
    static const PcgPoints kEmpty;
    return (i < in.size() && in[i]) ? *in[i] : kEmpty;
}

PcgPoint basePoint(const Vec3& pos, i32 seed) {
    PcgPoint p;
    p.pos = pos;
    p.seed = seed;
    return p;
}

// f32 sin/cos through double, exactly as .NET's float32 `sin` does.
f32 sinD(f32 x) { return static_cast<f32>(std::sin(static_cast<f64>(x))); }
f32 cosD(f32 x) { return static_cast<f32>(std::cos(static_cast<f64>(x))); }
Quat yawQuat(f32 radians) { const f32 h = radians * 0.5f; return Quat{0.0f, 0.0f, sinD(h), cosD(h)}; }

// Centres (i + 0.5) * s inside [lo, hi). False when empty.
bool latticeRange(f64 lo, f64 hi, f64 s, i64& a, i64& b) {
    a = static_cast<i64>(std::ceil(lo / s - 0.5));
    b = static_cast<i64>(std::ceil(hi / s - 0.5)) - 1;
    return b >= a;
}

bool overCap(const PcgNodeCtx& n, f64 count) {
    if (count <= static_cast<f64>(n.ctx.maxPoints)) return false;
    n.error("would produce " + std::to_string(static_cast<u64>(count)) + " points, over the cap of " +
            std::to_string(n.ctx.maxPoints));
    return true;
}

// Tilts +Z onto `normal`, by at most maxDeg (steeper ground tilts only that far).
Quat alignUpTo(const Vec3& normal, f32 maxDeg = 90.0f) {
    const f32 c = std::min(1.0f, std::max(-1.0f, normal.z));
    if (c > 0.99999f) return Quat::identity();
    const Vec3 axis = cross(Vec3{0, 0, 1}, normal);
    if (axis.x * axis.x + axis.y * axis.y + axis.z * axis.z < 1e-12f) return Quat::identity();
    return Quat::fromAxisAngle(axis, std::min(std::acos(c), maxDeg * kDeg2Rad));
}

f32 steepnessDeg(const Vec3& n) { return std::acos(std::min(1.0f, std::max(-1.0f, n.z))) * kRad2Deg; }

PcgBox boxFromParams(const PcgNodeCtx& n, bool& ok) {
    ok = true;
    PcgBox b;
    const std::string vol = n.p.str("volume");
    if (!vol.empty()) {
        const auto it = n.ctx.volumes.find(vol);
        if (it == n.ctx.volumes.end()) { n.error("unknown volume '" + vol + "'"); ok = false; return b; }
        return it->second;
    }
    if (!n.p.has("min") || !n.p.has("max")) { n.error("needs volume=<name> or min= and max="); ok = false; return b; }
    b.min = n.p.vec3("min", {});
    b.max = n.p.vec3("max", {});
    return b;
}

f32 dist2(const Vec3& a, const Vec3& b, bool use3d) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = use3d ? a.z - b.z : 0.0f;
    return dx * dx + dy * dy + dz * dz;
}

// Distance in XY from p to the polyline.
f32 distToPolyline(const Vec3& p, const std::vector<Vec3>& line) {
    f32 best = std::numeric_limits<f32>::max();
    for (usize i = 0; i + 1 < line.size(); ++i) {
        const Vec3 a = line[i], b = line[i + 1];
        const f32 abx = b.x - a.x, aby = b.y - a.y;
        const f32 l2 = abx * abx + aby * aby;
        f32 t = l2 > 1e-12f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / l2 : 0.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        const f32 dx = p.x - (a.x + abx * t), dy = p.y - (a.y + aby * t);
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
    }
    if (line.size() == 1) best = std::sqrt(dist2(p, line[0], false));
    return best;
}

// ---- samplers -----------------------------------------------------------------------------------------------------------

void nCreatePointsGrid(const PcgNodeCtx& n, const In&, PcgNodeOut& out) {
    out.pins.resize(1);
    const f64 s = n.p.num("spacing", 200.0);
    if (s <= 0.0) { n.error("spacing must be > 0"); return; }
    const PcgBox& b = n.ctx.bounds;
    i64 i0, i1, j0, j1;
    if (!latticeRange(b.min.x, b.max.x, s, i0, i1) || !latticeRange(b.min.y, b.max.y, s, j0, j1)) return;
    if (overCap(n, static_cast<f64>(i1 - i0 + 1) * static_cast<f64>(j1 - j0 + 1))) return;
    const f32 z = static_cast<f32>(n.p.num("z", 0.0));
    PcgPoints& o = out.pins[0];
    for (i64 j = j0; j <= j1; ++j)
        for (i64 i = i0; i <= i1; ++i)
            o.push(basePoint(Vec3{static_cast<f32>((static_cast<f64>(i) + 0.5) * s),
                                  static_cast<f32>((static_cast<f64>(j) + 0.5) * s), z},
                             static_cast<i32>(pcgHash3(n.seed, static_cast<i32>(i), static_cast<i32>(j), 0))));
}

// Reference-compatible random scatter (Aver.Pcg.Pcg.scatter). The point set depends on the region, so it is not
// partition-stable; prefer a grid + noise + jitter inside streamed volumes.
void nCreatePointsRandom(const PcgNodeCtx& n, const In&, PcgNodeOut& out) {
    out.pins.resize(1);
    PcgScatterSpec s;
    s.region = n.ctx.bounds;
    s.count = n.p.integer("count", 100);
    s.seed = n.seed;
    s.randomYaw = n.p.flag("randomYaw", true);
    s.scaleMin = static_cast<f32>(n.p.num("scaleMin", 1.0));
    s.scaleMax = static_cast<f32>(n.p.num("scaleMax", 1.0));
    s.minSpacing = static_cast<f32>(n.p.num("minSpacing", 0.0));
    if (overCap(n, s.count)) return;
    if (n.ctx.partitioned)
        n.warn("count-based scatter depends on the evaluated region, so cell-by-cell generation will not match a whole-volume run");
    out.pins[0] = pcgScatterReference(s);
}

void nSurfaceSampler(const PcgNodeCtx& n, const In&, PcgNodeOut& out) {
    out.pins.resize(1);
    if (!n.ctx.landscape) { n.error("no landscape is available to sample"); return; }
    const f64 s = n.p.num("spacing", 200.0);
    if (s <= 0.0) { n.error("spacing must be > 0"); return; }
    const PcgBox& b = n.ctx.bounds;
    i64 i0, i1, j0, j1;
    if (!latticeRange(b.min.x, b.max.x, s, i0, i1) || !latticeRange(b.min.y, b.max.y, s, j0, j1)) return;
    if (overCap(n, static_cast<f64>(i1 - i0 + 1) * static_cast<f64>(j1 - j0 + 1))) return;
    const bool align = n.p.flag("align", false);
    const f32 zOff = static_cast<f32>(n.p.num("zOffset", 0.0));
    PcgPoints& o = out.pins[0];
    for (i64 j = j0; j <= j1; ++j)
        for (i64 i = i0; i <= i1; ++i) {
            const f64 x = (static_cast<f64>(i) + 0.5) * s, y = (static_cast<f64>(j) + 0.5) * s;
            f32 z = 0.0f;
            if (!n.ctx.landscape->heightAt(x, y, z)) continue;
            Vec3 nrm{0, 0, 1};
            n.ctx.landscape->normalAt(x, y, nrm);
            PcgPoint p = basePoint(Vec3{static_cast<f32>(x), static_cast<f32>(y), z + zOff},
                                   static_cast<i32>(pcgHash3(n.seed, static_cast<i32>(i), static_cast<i32>(j), 0)));
            p.steepness = steepnessDeg(nrm);
            if (align) p.rot = alignUpTo(nrm);
            o.push(p);
        }
}

void nSplineSampler(const PcgNodeCtx& n, const In&, PcgNodeOut& out) {
    out.pins.resize(1);
    std::vector<Vec3> line = n.p.polyline("pts");
    const std::string name = n.p.str("name");
    if (line.empty() && !name.empty()) {
        const auto it = n.ctx.splines.find(name);
        if (it == n.ctx.splines.end()) { n.error("unknown spline '" + name + "'"); return; }
        line = it->second;
    }
    const f32 spacing = static_cast<f32>(n.p.num("spacing", 200.0));
    const f32 jitter = static_cast<f32>(n.p.num("jitter", 0.0));
    if (line.size() < 2 || spacing <= 0.0f) { if (line.size() < 2) n.warn("needs a spline of at least two points"); return; }
    PcgPoints& o = out.pins[0];
    f32 carry = 0.0f;
    i32 index = 0;
    for (usize s = 0; s + 1 < line.size(); ++s) {
        const Vec3 a = line[s], b = line[s + 1];
        const Vec3 seg = b - a;
        const f32 segLen = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
        if (segLen <= 1e-4f) continue;
        const f32 dirX = seg.x / segLen, dirY = seg.y / segLen;
        f32 t = carry;
        while (t < segLen) {
            const f32 u = t / segLen;
            const Vec3 base{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
            const u32 h = pcgHash2(n.seed, index);
            const f32 j = jitter > 0.0f ? (pcgFloat01(h) - 0.5f) * 2.0f * jitter : 0.0f;
            PcgPoint p = basePoint(Vec3{base.x - dirY * j, base.y + dirX * j, base.z}, static_cast<i32>(h));
            p.rot = yawQuat(static_cast<f32>(std::atan2(static_cast<f64>(dirY), static_cast<f64>(dirX))));
            if (n.ctx.bounds.containsXY(p.pos)) o.push(p);   // the whole spline is walked so the index never depends on the region
            ++index;
            t += spacing;
        }
        carry = t - segLen;
    }
}

void nVolumeSampler(const PcgNodeCtx& n, const In&, PcgNodeOut& out) {
    out.pins.resize(1);
    bool ok;
    const PcgBox box = boxFromParams(n, ok);
    if (!ok) return;
    const f64 s = n.p.num("spacing", 200.0);
    if (s <= 0.0) { n.error("spacing must be > 0"); return; }
    const PcgBox& c = n.ctx.bounds;
    i64 i0, i1, j0, j1, k0, k1;
    if (!latticeRange(std::max(box.min.x, c.min.x), std::min(box.max.x, c.max.x), s, i0, i1) ||
        !latticeRange(std::max(box.min.y, c.min.y), std::min(box.max.y, c.max.y), s, j0, j1))
        return;
    // Z is closed on both ends, so the upper test is on the last centre rather than a half-open bound.
    k0 = static_cast<i64>(std::ceil(box.min.z / s - 0.5));
    k1 = static_cast<i64>(std::floor(box.max.z / s - 0.5));
    if (k1 < k0) return;
    if (overCap(n, static_cast<f64>(i1 - i0 + 1) * static_cast<f64>(j1 - j0 + 1) * static_cast<f64>(k1 - k0 + 1))) return;
    PcgPoints& o = out.pins[0];
    for (i64 k = k0; k <= k1; ++k)
        for (i64 j = j0; j <= j1; ++j)
            for (i64 i = i0; i <= i1; ++i)
                o.push(basePoint(Vec3{static_cast<f32>((static_cast<f64>(i) + 0.5) * s),
                                      static_cast<f32>((static_cast<f64>(j) + 0.5) * s),
                                      static_cast<f32>((static_cast<f64>(k) + 0.5) * s)},
                                 static_cast<i32>(pcgHash3(n.seed, static_cast<i32>(i), static_cast<i32>(j), static_cast<i32>(k)))));
}

// ---- filters ------------------------------------------------------------------------------------------------------------

// Splits `in` into {inside, outside} by a per-point predicate; both outputs keep the input order and schema.
template <class Pred>
void split(const PcgPoints& in, PcgNodeOut& out, Pred&& inside) {
    out.pins.assign(2, in.emptyLike());
    for (usize i = 0; i < in.size(); ++i) out.pins[inside(i) ? 0 : 1].appendFrom(in, i);
}

void nBoundsFilter(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    bool ok;
    const PcgBox box = boxFromParams(n, ok);
    const PcgPoints& P = inAt(in, 0);
    if (!ok) { split(P, out, [](usize) { return false; }); return; }
    split(P, out, [&](usize i) { return box.contains(P.pos[i]); });
}

// The hash-lattice fBm the noise nodes share: `seed` params add to the node seed, `cell` is the lattice cell in cm.
pcg::InfiniteSpec noiseSpecFor(const PcgNodeCtx& n, f32 defaultCellCm) {
    pcg::InfiniteSpec spec{};
    spec.seed = n.seed + n.p.integer("seed", 0);
    spec.layerCount = 1;
    spec.layers[0].frequency = 1.0f;
    spec.layers[0].amplitude = 1.0f;
    spec.layers[0].octaves = std::max(1, n.p.integer("octaves", 3));
    spec.layers[0].lacunarity = 2.0f;
    spec.layers[0].gain = 0.5f;
    spec.cellSizeCm = static_cast<f32>(n.p.num("cell", defaultCellCm));
    if (spec.cellSizeCm <= 0.0f) spec.cellSizeCm = defaultCellCm;
    return spec;
}

void nDensityNoise(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    PcgPoints P = inAt(in, 0);
    const pcg::InfiniteSpec spec = noiseSpecFor(n, 1600.0f);
    const std::string mode = n.p.str("mode", "set");
    const f32 scale = static_cast<f32>(n.p.num("scale", 1.0)), bias = static_cast<f32>(n.p.num("bias", 0.0));
    const bool use3d = n.p.flag("use3d", false);
    for (usize i = 0; i < P.size(); ++i) {
        const f32 v = pcg::sampleInfiniteAbs(spec, P.pos[i].x, P.pos[i].y, use3d ? P.pos[i].z : 0.0) * scale + bias;
        if (mode == "mul") P.density[i] *= v;
        else if (mode == "add") P.density[i] += v;
        else if (mode == "max") P.density[i] = std::max(P.density[i], v);
        else P.density[i] = v;
    }
    out.pins.push_back(std::move(P));
}

// Splits by the noise value at the point's XY: inside = [min, max]. The patch mask of a biome rule.
void nNoiseMask(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const pcg::InfiniteSpec spec = noiseSpecFor(n, 4000.0f);
    const f32 lo = static_cast<f32>(n.p.num("min", 0.5)), hi = static_cast<f32>(n.p.num("max", 1.0e30));
    split(P, out, [&](usize i) {
        const f32 v = pcg::sampleInfiniteAbs(spec, P.pos[i].x, P.pos[i].y, 0.0);
        return v >= lo && v <= hi;
    });
}

void nDensityFilter(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const f32 lo = static_cast<f32>(n.p.num("min", 0.5)), hi = static_cast<f32>(n.p.num("max", 1.0e30));
    split(P, out, [&](usize i) { return P.density[i] >= lo && P.density[i] <= hi; });
}

void nDensityRemap(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    PcgPoints P = inAt(in, 0);
    const f32 a = static_cast<f32>(n.p.num("inMin", 0.0)), b = static_cast<f32>(n.p.num("inMax", 1.0));
    const f32 c = static_cast<f32>(n.p.num("outMin", 0.0)), d = static_cast<f32>(n.p.num("outMax", 1.0));
    const bool clamp = n.p.flag("clamp", true);
    for (usize i = 0; i < P.size(); ++i) {
        f32 t = (b != a) ? (P.density[i] - a) / (b - a) : 0.0f;
        if (clamp) t = std::min(1.0f, std::max(0.0f, t));
        P.density[i] = c + (d - c) * t;
    }
    out.pins.push_back(std::move(P));
}

bool compareOp(std::string_view op, f32 a, f32 b) {
    if (op == "<") return a < b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == ">=") return a >= b;
    if (op == "!=") return a != b;
    return a == b;
}

void nAttributeFilter(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const std::string attr = n.p.str("attr", "density"), op = n.p.str("op", ">=");
    const f32 v = static_cast<f32>(n.p.num("value", 0.5));
    if (!P.empty() && !P.hasScalar(attr)) n.warn("no attribute '" + attr + "' on the input points");
    split(P, out, [&](usize i) { return compareOp(op, P.scalar(attr, i), v); });
}

void nAttributeMath(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    PcgPoints P = inAt(in, 0);
    const std::string dst = n.p.str("dst", "density"), a = n.p.str("a", "density"), b = n.p.str("b"), op = n.p.str("op", "mul");
    const f32 k = static_cast<f32>(n.p.num("value", 1.0));
    if (!P.empty() && !P.hasScalar(a)) n.warn("no attribute '" + a + "' on the input points");
    for (usize i = 0; i < P.size(); ++i) {
        const f32 x = P.scalar(a, i), y = b.empty() ? k : P.scalar(b, i);
        f32 r;
        if (op == "add") r = x + y;
        else if (op == "sub") r = x - y;
        else if (op == "div") r = y != 0.0f ? x / y : 0.0f;
        else if (op == "min") r = std::min(x, y);
        else if (op == "max") r = std::max(x, y);
        else r = x * y;
        P.setScalar(dst, i, r);
    }
    out.pins.push_back(std::move(P));
}

void nAttributeSet(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    PcgPoints P = inAt(in, 0);
    const std::string name = n.p.str("name");
    if (name.empty()) { n.error("needs name="); out.pins.push_back(std::move(P)); return; }
    const bool random = n.p.has("min") && n.p.has("max");
    const f32 lo = static_cast<f32>(n.p.num("min", 0.0)), hi = static_cast<f32>(n.p.num("max", 0.0));
    const f32 v = static_cast<f32>(n.p.num("value", 0.0));
    for (usize i = 0; i < P.size(); ++i) {
        const f32 r = random ? lo + (hi - lo) * pcgFloat01(pcgHash2(P.seed[i] ^ n.seed, 0x5E7)) : v;
        P.setScalar(name, i, r);
    }
    out.pins.push_back(std::move(P));
}

void nTransformPoints(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    PcgPoints P = inAt(in, 0);
    const Vec3 pMin = n.p.vec3("posMin", {}), pMax = n.p.vec3("posMax", {});
    const Vec3 rMin = n.p.vec3("rotMin", {}), rMax = n.p.vec3("rotMax", {});   // degrees about X, Y, Z
    const Vec3 sMin = n.p.vec3("scaleMin", {1, 1, 1}), sMax = n.p.vec3("scaleMax", {1, 1, 1});
    const bool uniform = n.p.flag("uniformScale", true);
    for (usize i = 0; i < P.size(); ++i) {
        const i32 base = static_cast<i32>(pcgHash2(P.seed[i], n.seed));
        const auto u = [&](i32 k) { return pcgFloat01(pcgHash2(base, k)); };
        const auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
        P.pos[i] += Vec3{lerp(pMin.x, pMax.x, u(0)), lerp(pMin.y, pMax.y, u(1)), lerp(pMin.z, pMax.z, u(2))};
        const f32 rx = lerp(rMin.x, rMax.x, u(3)) * kDeg2Rad, ry = lerp(rMin.y, rMax.y, u(4)) * kDeg2Rad,
                  rz = lerp(rMin.z, rMax.z, u(5)) * kDeg2Rad;
        if (rx != 0.0f || ry != 0.0f || rz != 0.0f)
            P.rot[i] = (P.rot[i] * Quat::fromAxisAngle({0, 0, 1}, rz) * Quat::fromAxisAngle({0, 1, 0}, ry) *
                        Quat::fromAxisAngle({1, 0, 0}, rx)).normalized();
        if (uniform) {
            const f32 s = lerp(sMin.x, sMax.x, u(6));
            P.scale[i] = Vec3{P.scale[i].x * s, P.scale[i].y * s, P.scale[i].z * s};
        } else {
            P.scale[i] = Vec3{P.scale[i].x * lerp(sMin.x, sMax.x, u(6)), P.scale[i].y * lerp(sMin.y, sMax.y, u(7)),
                              P.scale[i].z * lerp(sMin.z, sMax.z, u(8))};
        }
    }
    out.pins.push_back(std::move(P));
}

void nSelfPrune(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const usize N = P.size();
    const f32 radius = static_cast<f32>(n.p.num("radius", 0.0));
    const std::string prio = n.p.str("priority", "density");
    const bool greedy = n.p.str("mode", "greedy") != "local";
    const bool use3d = n.p.flag("dims3", false);

    std::vector<f32> r(N, 0.0f);
    f32 maxThr = radius;
    if (radius <= 0.0f) {
        for (usize i = 0; i < N; ++i) {
            r[i] = std::max(P.extent[i].x, P.extent[i].y) * std::max(P.scale[i].x, P.scale[i].y);
            maxThr = std::max(maxThr, 2.0f * r[i]);
        }
    }
    out.pins.resize(1);
    if (maxThr <= 0.0f) { out.pins[0] = P; return; }
    const auto thr = [&](usize a, usize b) { return radius > 0.0f ? radius : r[a] + r[b]; };

    std::vector<u32> order(N);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const f32 pa = P.scalar(prio, a), pb = P.scalar(prio, b);
        if (pa != pb) return pa > pb;
        if (P.seed[a] != P.seed[b]) return P.seed[a] < P.seed[b];
        if (P.pos[a].x != P.pos[b].x) return P.pos[a].x < P.pos[b].x;
        if (P.pos[a].y != P.pos[b].y) return P.pos[a].y < P.pos[b].y;
        if (P.pos[a].z != P.pos[b].z) return P.pos[a].z < P.pos[b].z;
        return a < b;
    });
    std::vector<u32> rank(N);
    for (usize k = 0; k < N; ++k) rank[order[k]] = static_cast<u32>(k);

    std::vector<char> keep(N, 1);
    PcgSpatialGrid grid(maxThr);
    if (greedy) {
        for (u32 idx : order) {
            bool blocked = false;
            grid.forEachNear(P.pos[idx], maxThr, [&](u32 j) {
                if (!blocked && dist2(P.pos[idx], P.pos[j], use3d) < thr(idx, j) * thr(idx, j)) blocked = true;
            });
            if (blocked) keep[idx] = 0; else grid.insert(P.pos[idx], idx);
        }
    } else {
        grid.build(P.pos);
        for (u32 idx = 0; idx < N; ++idx) {
            bool blocked = false;
            grid.forEachNear(P.pos[idx], maxThr, [&](u32 j) {
                if (!blocked && j != idx && rank[j] < rank[idx] &&
                    dist2(P.pos[idx], P.pos[j], use3d) < thr(idx, j) * thr(idx, j))
                    blocked = true;
            });
            if (blocked) keep[idx] = 0;
        }
    }
    out.pins[0] = P.emptyLike();
    for (usize i = 0; i < N; ++i) if (keep[i]) out.pins[0].appendFrom(P, i);
}

// ---- set operations -----------------------------------------------------------------------------------------------------

bool anyNear(const PcgSpatialGrid& g, const PcgPoints& B, const Vec3& p, f32 radius, bool use3d) {
    bool hit = false;
    g.forEachNear(p, radius, [&](u32 j) { if (!hit && dist2(p, B.pos[j], use3d) <= radius * radius) hit = true; });
    return hit;
}

void nDifference(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& A = inAt(in, 0);
    const PcgPoints& B = inAt(in, 1);
    const f32 r = static_cast<f32>(n.p.num("radius", 1.0));
    const bool use3d = n.p.flag("dims3", false);
    PcgSpatialGrid g(std::max(r, 1.0f));
    g.build(B.pos);
    out.pins.assign(1, A.emptyLike());
    for (usize i = 0; i < A.size(); ++i) if (!anyNear(g, B, A.pos[i], r, use3d)) out.pins[0].appendFrom(A, i);
}

void nIntersect(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& A = inAt(in, 0);
    const PcgPoints& B = inAt(in, 1);
    const f32 r = static_cast<f32>(n.p.num("radius", 1.0));
    const bool use3d = n.p.flag("dims3", false);
    PcgSpatialGrid g(std::max(r, 1.0f));
    g.build(B.pos);
    out.pins.assign(1, A.emptyLike());
    for (usize i = 0; i < A.size(); ++i) if (anyNear(g, B, A.pos[i], r, use3d)) out.pins[0].appendFrom(A, i);
}

void nUnion(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& A = inAt(in, 0);
    const PcgPoints& B = inAt(in, 1);
    const f32 dedup = static_cast<f32>(n.p.num("dedup", 0.0));   // >0: drop B points this close to an A point
    out.pins.assign(1, A);
    if (dedup > 0.0f) {
        PcgSpatialGrid g(dedup);
        g.build(A.pos);
        for (usize i = 0; i < B.size(); ++i) if (!anyNear(g, A, B.pos[i], dedup, true)) out.pins[0].appendFrom(B, i);
    } else {
        out.pins[0].appendAll(B);
    }
}

// ---- landscape-driven ---------------------------------------------------------------------------------------------------

void nProjectToSurface(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    out.pins.assign(1, P.emptyLike());
    if (!n.ctx.landscape) { n.error("no landscape is available to project onto"); return; }
    const bool align = n.p.flag("align", false), drop = n.p.flag("drop", true);
    const f32 zOff = static_cast<f32>(n.p.num("zOffset", 0.0)), alignMax = static_cast<f32>(n.p.num("alignMax", 90.0));
    for (usize i = 0; i < P.size(); ++i) {
        f32 z = 0.0f;
        if (!n.ctx.landscape->heightAt(P.pos[i].x, P.pos[i].y, z)) {
            if (!drop) out.pins[0].appendFrom(P, i);
            continue;
        }
        out.pins[0].appendFrom(P, i);
        const usize k = out.pins[0].size() - 1;
        PcgPoints& o = out.pins[0];
        o.pos[k].z = z + zOff;
        Vec3 nrm{0, 0, 1};
        n.ctx.landscape->normalAt(P.pos[i].x, P.pos[i].y, nrm);
        o.steepness[k] = steepnessDeg(nrm);
        if (align) o.rot[k] = alignUpTo(nrm, alignMax) * P.rot[i];
    }
}

void nSlopeFilter(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const f32 lo = static_cast<f32>(n.p.num("min", 0.0)), hi = static_cast<f32>(n.p.num("max", 30.0));
    const bool fromLand = n.p.str("source", "point") == "landscape";
    if (fromLand && !n.ctx.landscape) { n.error("source=landscape but no landscape is available"); split(P, out, [](usize) { return false; }); return; }
    split(P, out, [&](usize i) {
        f32 s = P.steepness[i];
        if (fromLand) {
            Vec3 nrm{0, 0, 1};
            if (!n.ctx.landscape->normalAt(P.pos[i].x, P.pos[i].y, nrm)) return false;
            s = steepnessDeg(nrm);
        }
        return s >= lo && s <= hi;
    });
}

void nHeightFilter(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const f32 lo = static_cast<f32>(n.p.num("min", -1.0e30)), hi = static_cast<f32>(n.p.num("max", 1.0e30));
    split(P, out, [&](usize i) { return P.pos[i].z >= lo && P.pos[i].z <= hi; });
}

void nLayerWeightFilter(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    if (!n.ctx.landscape) { n.error("no landscape is available"); split(P, out, [](usize) { return false; }); return; }
    const std::string layer = n.p.str("layer");
    const f32 lo = static_cast<f32>(n.p.num("min", 0.5)), hi = static_cast<f32>(n.p.num("max", 1.0));
    split(P, out, [&](usize i) {
        const f32 w = n.ctx.landscape->layerWeight(layer, P.pos[i].x, P.pos[i].y);
        return w >= lo && w <= hi;
    });
}

void nDistanceToSpline(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const std::string name = n.p.str("name");
    const auto it = n.ctx.splines.find(name);
    if (it == n.ctx.splines.end()) { n.error("unknown spline '" + name + "'"); split(P, out, [](usize) { return false; }); return; }
    const f32 lo = static_cast<f32>(n.p.num("min", 0.0)), hi = static_cast<f32>(n.p.num("max", 1.0e30));
    split(P, out, [&](usize i) { const f32 d = distToPolyline(P.pos[i], it->second); return d >= lo && d <= hi; });
}

void nDistanceToActor(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const std::string tag = n.p.str("tag");
    const auto it = n.ctx.actors.find(tag);
    const f32 lo = static_cast<f32>(n.p.num("min", 0.0)), hi = static_cast<f32>(n.p.num("max", 1.0e30));
    if (it == n.ctx.actors.end()) {
        n.warn("no actors tagged '" + tag + "'");   // nothing to be near: every point is at infinity
        split(P, out, [&](usize) { return hi >= 1.0e29f; });
        return;
    }
    split(P, out, [&](usize i) {
        f32 best = std::numeric_limits<f32>::max();
        for (const Vec3& a : it->second) best = std::min(best, dist2(P.pos[i], a, false));
        const f32 d = std::sqrt(best);
        return d >= lo && d <= hi;
    });
}

// ---- biome rules --------------------------------------------------------------------------------------------------------

f32 ramp01(f32 v, f32 lo, f32 width) {
    if (width <= 0.0f) return v >= lo ? 1.0f : 0.0f;
    return std::min(1.0f, std::max(0.0f, (v - lo) / width));
}

// A mask value m in [0, 1] per point -- a paint layer weight, a noise value, nearness to a spline, depth inside a volume,
// each ramped over `edge` -- keeps the point when a seeded per-point hash u < m. In the ramp the survivors thin out at
// random, so the border of two biomes (one `invert=1`) interleaves instead of drawing a line. edge=0 is a hard cut.
// outputs: inside (kept), outside. The kept points carry the value as attribute "mask".
void nEdgeFalloff(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const std::string source = n.p.str("source", "layer");
    const f32 edge = static_cast<f32>(n.p.num("edge", 0.0)), mn = static_cast<f32>(n.p.num("min", 0.5));
    const bool invert = n.p.flag("invert", false);
    pcg::InfiniteSpec spec{};
    const std::vector<Vec3>* line = nullptr;
    PcgBox box;
    bool ok = true;
    if (source == "layer") {
        if (!n.ctx.landscape) { n.error("source=layer but no landscape is available"); ok = false; }
    } else if (source == "noise") {
        spec = noiseSpecFor(n, 4000.0f);
    } else if (source == "spline") {
        const auto it = n.ctx.splines.find(n.p.str("name"));
        if (it == n.ctx.splines.end()) { n.error("unknown spline '" + n.p.str("name") + "'"); ok = false; } else line = &it->second;
    } else if (source == "volume") {
        box = boxFromParams(n, ok);
    } else {
        n.error("source must be layer, noise, spline or volume");
        ok = false;
    }
    out.pins.assign(2, P.emptyLike());
    if (!ok) { out.pins[1] = P; return; }
    const std::string layer = n.p.str("name");
    const f32 half = static_cast<f32>(n.p.num("half", 500.0));
    for (usize i = 0; i < P.size(); ++i) {
        const Vec3& q = P.pos[i];
        f32 m = 0.0f;
        if (source == "layer") m = ramp01(n.ctx.landscape->layerWeight(layer, q.x, q.y), mn, edge);
        else if (source == "noise") m = ramp01(pcg::sampleInfiniteAbs(spec, q.x, q.y, 0.0), mn, edge);
        else if (source == "spline") m = ramp01(half + edge - distToPolyline(q, *line), 0.0f, edge);
        else m = ramp01(std::min({q.x - box.min.x, box.max.x - q.x, q.y - box.min.y, box.max.y - q.y}), 0.0f, edge);
        if (invert) m = 1.0f - m;
        const f32 u = pcgFloat01(pcgHash2(P.seed[i] ^ n.seed, 0xED6E));
        if (u < m) {
            out.pins[0].appendFrom(P, i);
            out.pins[0].setScalar("mask", out.pins[0].size() - 1, m);
        } else {
            out.pins[1].appendFrom(P, i);
        }
    }
}

// Each input point is a clump PARENT: it gets min..max children inside `radius` (XY), placed from the parent's seed so the
// children exist wherever the parent does. `spacing` > 0 keeps siblings that far apart (a few seeded retries, then the
// child is dropped). `chance` thins the parents. keepParent=1 keeps the parent as one of the clump.
void nClump(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    out.pins.assign(1, P.emptyLike());
    const f32 radius = static_cast<f32>(n.p.num("radius", 0.0));
    const i32 cmin = std::max(0, n.p.integer("min", 3)), cmax = std::max(cmin, n.p.integer("max", 6));
    const f32 chance = static_cast<f32>(n.p.num("chance", 1.0)), spacing = static_cast<f32>(n.p.num("spacing", 0.0));
    const bool keepParent = n.p.flag("keepParent", false);
    if (radius <= 0.0f) { n.error("needs radius > 0"); out.pins[0] = P; return; }
    if (overCap(n, static_cast<f64>(P.size()) * (cmax + 1))) return;
    PcgPoints& o = out.pins[0];
    std::vector<Vec3> sib;
    for (usize i = 0; i < P.size(); ++i) {
        const i32 base = P.seed[i] ^ n.seed;
        if (chance < 1.0f && pcgFloat01(pcgHash2(base, 0xC1)) >= chance) continue;
        if (keepParent) o.appendFrom(P, i);
        const i32 count = cmin + static_cast<i32>(pcgHash2(base, 0xC2) % static_cast<u32>(cmax - cmin + 1));
        sib.clear();
        for (i32 k = 0; k < count; ++k) {
            const u32 childSeed = pcgHash2(base, 100 + k);
            Vec3 off{};
            bool placed = false;
            for (i32 attempt = 0; attempt < (spacing > 0.0f ? 8 : 1) && !placed; ++attempt) {
                const i32 a = static_cast<i32>(childSeed) + attempt * 7919;
                const f32 r = radius * std::sqrt(pcgFloat01(pcgHash2(a, 1)));   // sqrt: uniform over the disc
                const f32 th = pcgFloat01(pcgHash2(a, 2)) * 6.28318530718f;
                off = Vec3{r * cosD(th), r * sinD(th), 0.0f};
                placed = true;
                if (spacing > 0.0f)
                    for (const Vec3& s : sib)
                        if (dist2(s, off, false) < spacing * spacing) { placed = false; break; }
            }
            if (!placed) continue;
            sib.push_back(off);
            o.appendFrom(P, i);
            const usize c = o.size() - 1;
            o.pos[c] = P.pos[i] + off;
            o.seed[c] = static_cast<i32>(childSeed);
        }
    }
}

// Overlap resolution across types: the point with the higher `priority` attribute wins, then the larger radius, then the
// seed. Two points conflict when closer than (ra + rb) * overlap, where r = the `radius` attribute * the larger XY scale.
// A point with radius 0 never blocks and is only blocked by a real radius. `maxRadius` bounds the radius for the margin.
void nCompete(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const usize N = P.size();
    const std::string radAttr = n.p.str("radiusAttr", "radius"), prioAttr = n.p.str("priorityAttr", "priority");
    const f32 overlap = static_cast<f32>(n.p.num("overlap", 1.0)), maxDeclared = static_cast<f32>(n.p.num("maxRadius", 0.0));
    const bool greedy = n.p.str("mode", "greedy") != "local", use3d = n.p.flag("dims3", false);
    std::vector<f32> r(N, 0.0f), pr(N, 0.0f);
    f32 maxR = 0.0f;
    for (usize i = 0; i < N; ++i) {
        r[i] = P.scalar(radAttr, i) * std::max(P.scale[i].x, P.scale[i].y);
        pr[i] = P.scalar(prioAttr, i);
        maxR = std::max(maxR, r[i]);
    }
    out.pins.assign(1, P.emptyLike());
    if (maxR <= 0.0f) { out.pins[0] = P; return; }
    if (maxR > maxDeclared * 1.0001f + 1e-3f)
        n.warn("a radius of " + std::to_string(maxR) + " cm exceeds maxRadius=" + std::to_string(maxDeclared) +
               "; cell seams are only exact up to the declared maximum");
    const f32 maxThr = 2.0f * maxR * overlap;
    std::vector<u32> order(N);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (pr[a] != pr[b]) return pr[a] > pr[b];
        if (r[a] != r[b]) return r[a] > r[b];
        if (P.seed[a] != P.seed[b]) return P.seed[a] < P.seed[b];
        if (P.pos[a].x != P.pos[b].x) return P.pos[a].x < P.pos[b].x;
        if (P.pos[a].y != P.pos[b].y) return P.pos[a].y < P.pos[b].y;
        return a < b;
    });
    std::vector<u32> rank(N);
    for (usize k = 0; k < N; ++k) rank[order[k]] = static_cast<u32>(k);
    std::vector<char> keep(N, 1);
    PcgSpatialGrid grid(maxThr);
    const auto blockedBy = [&](u32 a, u32 b) {
        const f32 t = (r[a] + r[b]) * overlap;
        return t > 0.0f && dist2(P.pos[a], P.pos[b], use3d) < t * t;
    };
    if (greedy) {
        for (u32 idx : order) {
            bool blocked = false;
            grid.forEachNear(P.pos[idx], maxThr, [&](u32 j) { if (!blocked && blockedBy(idx, j)) blocked = true; });
            if (blocked) keep[idx] = 0; else grid.insert(P.pos[idx], idx);
        }
    } else {
        grid.build(P.pos);
        for (u32 idx = 0; idx < N; ++idx) {
            bool blocked = false;
            grid.forEachNear(P.pos[idx], maxThr, [&](u32 j) { if (!blocked && j != idx && rank[j] < rank[idx] && blockedBy(idx, j)) blocked = true; });
            if (blocked) keep[idx] = 0;
        }
    }
    for (usize i = 0; i < N; ++i) if (keep[i]) out.pins[0].appendFrom(P, i);
}

// ---- spawners -----------------------------------------------------------------------------------------------------------

void nWeightedMeshSpawner(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    PcgPoints P = inAt(in, 0);
    struct Entry { std::string mesh; f32 weight; };
    std::vector<Entry> entries;
    const std::string spec = n.p.str("meshes");
    for (usize b = 0; b < spec.size();) {
        usize e = spec.find(',', b);
        if (e == std::string::npos) e = spec.size();
        const std::string item = spec.substr(b, e - b);
        b = e + 1;
        if (item.empty()) continue;
        const usize colon = item.rfind(':');
        f32 w = 1.0f;
        std::string path = item;
        if (colon != std::string::npos && colon + 1 < item.size() &&
            item.find_first_not_of("0123456789.", colon + 1) == std::string::npos) {
            w = static_cast<f32>(std::atof(item.c_str() + colon + 1));
            path = item.substr(0, colon);
        }
        entries.push_back({path, w});
    }
    if (entries.empty()) n.error("needs meshes=path:weight,path:weight");
    f64 total = 0.0;
    for (const Entry& e : entries) if (e.weight > 0.0f) total += e.weight;
    if (total <= 0.0) { if (!entries.empty()) n.error("every mesh weight is <= 0"); out.pins.push_back(std::move(P)); return; }
    const std::string material = n.p.str("material");
    std::vector<PcgInstanceGroup> groups(entries.size());
    const f32 cullStart = static_cast<f32>(n.p.num("cullStart", 0.0)), cullEnd = static_cast<f32>(n.p.num("cullEnd", 0.0));
    const f32 lodBias = static_cast<f32>(n.p.num("lodBias", 1.0));
    const i32 collision = n.p.integer("collision", 0);   // 0 none, 1 trunk, 2 full
    for (usize k = 0; k < entries.size(); ++k) {
        groups[k].mesh = entries[k].mesh;
        groups[k].material = material;
        groups[k].cullStartCm = cullStart;
        groups[k].cullEndCm = cullEnd;
        groups[k].lodScaleBias = lodBias > 0.0f ? lodBias : 1.0f;
        groups[k].flags = collision == 1 ? fmt::kOcInstanceFlagCollisionTrunk : collision == 2 ? fmt::kOcInstanceFlagCollisionFull : 0u;
    }
    auto& meshIdx = P.iAttr["mesh"];
    meshIdx.assign(P.size(), 0);
    for (usize i = 0; i < P.size(); ++i) {
        const f64 target = static_cast<f64>(pcgHash2(P.seed[i], 0x5EED)) / 4294967296.0 * total;
        f64 cum = 0.0;
        usize pick = entries.size();
        for (usize k = 0; k < entries.size(); ++k) {
            if (entries[k].weight <= 0.0f) continue;
            cum += entries[k].weight;
            pick = k;
            if (target < cum) break;
        }
        meshIdx[i] = static_cast<i32>(pick);
        Transform xf;
        xf.position = P.pos[i]; xf.rotation = P.rot[i]; xf.scale = P.scale[i];
        groups[pick].items.push_back({xf, P.seed[i]});
    }
    for (PcgInstanceGroup& g : groups) if (!g.items.empty()) out.groups.push_back(std::move(g));
    out.pins.push_back(std::move(P));
}

void nActorSpawner(const PcgNodeCtx& n, const In& in, PcgNodeOut& out) {
    const PcgPoints& P = inAt(in, 0);
    const std::string cls = n.p.str("class");
    if (cls.empty()) n.error("needs class=<actor class>");
    for (usize i = 0; i < P.size(); ++i) {
        Transform xf;
        xf.position = P.pos[i]; xf.rotation = P.rot[i]; xf.scale = P.scale[i];
        out.actors.push_back({cls, xf, P.seed[i]});
    }
    out.pins.push_back(P);
}

// ---- margins ------------------------------------------------------------------------------------------------------------

f32 marginNone(const PcgParams&) { return 0.0f; }
f32 marginPrune(const PcgParams& p) {
    const f32 r = static_cast<f32>(p.num("radius", 0.0));
    if (r <= 0.0f) return static_cast<f32>(p.num("margin", 0.0));
    return p.str("mode", "greedy") == "local" ? r : 3.0f * r;   // greedy chains: three radii covers all but pathological runs
}
f32 marginClump(const PcgParams& p) { return static_cast<f32>(p.num("radius", 0.0)); }
f32 marginCompete(const PcgParams& p) {
    const f32 thr = 2.0f * static_cast<f32>(p.num("maxRadius", 0.0)) * static_cast<f32>(p.num("overlap", 1.0));
    return p.str("mode", "greedy") == "local" ? thr : 3.0f * thr;
}
f32 marginRadius(const PcgParams& p) { return static_cast<f32>(p.num("radius", 1.0)); }
f32 marginJitter(const PcgParams& p) {
    const Vec3 a = p.vec3("posMin", {}), b = p.vec3("posMax", {});
    return std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(b.x), std::fabs(b.y)});
}

} // namespace

// ---- registry -----------------------------------------------------------------------------------------------------------

const std::vector<PcgNodeDef>& pcgNodeDefs() {
    static const std::vector<PcgNodeDef> defs = {
        {"CreatePointsGrid", {}, {"out"}, nCreatePointsGrid, false, marginNone, "World-anchored XY grid of points at cell centres."},
        {"CreatePointsRandom", {}, {"out"}, nCreatePointsRandom, false, marginNone, "Count-based uniform scatter, bit-identical to Aver.Pcg.Pcg.scatter. Not partition-stable."},
        {"SurfaceSampler", {}, {"out"}, nSurfaceSampler, false, marginNone, "Grid points dropped onto the landscape; sets steepness, optionally aligns to the normal."},
        {"SplineSampler", {}, {"out"}, nSplineSampler, false, marginNone, "Points along a named or inline spline, yaw along the tangent."},
        {"VolumeSampler", {}, {"out"}, nVolumeSampler, false, marginNone, "World-anchored 3D lattice inside a box or named volume."},
        {"BoundsFilter", {"in"}, {"inside", "outside"}, nBoundsFilter, false, marginNone, "Splits points by a box or named volume."},
        {"DensityNoise", {"in"}, {"out"}, nDensityNoise, false, marginNone, "Sets, multiplies or adds hash-lattice noise into density."},
        {"DensityFilter", {"in"}, {"inside", "outside"}, nDensityFilter, false, marginNone, "Splits by a density range."},
        {"DensityRemap", {"in"}, {"out"}, nDensityRemap, false, marginNone, "Linearly remaps density."},
        {"AttributeFilter", {"in"}, {"inside", "outside"}, nAttributeFilter, false, marginNone, "Splits by comparing an attribute to a value."},
        {"AttributeMath", {"in"}, {"out"}, nAttributeMath, false, marginNone, "dst = a op (b or value)."},
        {"AttributeSet", {"in"}, {"out"}, nAttributeSet, false, marginNone, "Sets an attribute to a constant or a per-point seeded random range."},
        {"TransformPoints", {"in"}, {"out"}, nTransformPoints, false, marginJitter, "Seeded jitter, rotation and scale ranges."},
        {"SelfPrune", {"in"}, {"out"}, nSelfPrune, false, marginPrune, "Priority-based radius pruning (greedy or local)."},
        {"Difference", {"a", "b"}, {"out"}, nDifference, false, marginRadius, "A without points near B."},
        {"Union", {"a", "b"}, {"out"}, nUnion, false, marginRadius, "A then B, optionally dropping near-duplicates."},
        {"Intersect", {"a", "b"}, {"out"}, nIntersect, false, marginRadius, "A points near a B point."},
        {"ProjectToSurface", {"in"}, {"out"}, nProjectToSurface, false, marginNone, "Snaps points to the landscape height."},
        {"SlopeFilter", {"in"}, {"inside", "outside"}, nSlopeFilter, false, marginNone, "Splits by surface steepness in degrees."},
        {"HeightFilter", {"in"}, {"inside", "outside"}, nHeightFilter, false, marginNone, "Splits by world Z."},
        {"LayerWeightFilter", {"in"}, {"inside", "outside"}, nLayerWeightFilter, false, marginNone, "Splits by a landscape paint layer weight."},
        {"DistanceToSplineFilter", {"in"}, {"inside", "outside"}, nDistanceToSpline, false, marginNone, "Splits by XY distance to a named spline."},
        {"DistanceToActorFilter", {"in"}, {"inside", "outside"}, nDistanceToActor, false, marginNone, "Splits by XY distance to the nearest tagged actor."},
        {"NoiseMask", {"in"}, {"inside", "outside"}, nNoiseMask, false, marginNone, "Splits by the noise value at the point (cell=, octaves=, seed=, min=, max=)."},
        {"EdgeFalloff", {"in"}, {"inside", "outside"}, nEdgeFalloff, false, marginNone, "Biome mask from a layer, noise, spline or volume, ramped over edge=; a seeded dither thins points across the ramp."},
        {"Clump", {"in"}, {"out"}, nClump, false, marginClump, "Each point becomes the parent of min..max children inside radius (optional sibling spacing)."},
        {"Compete", {"in"}, {"out"}, nCompete, false, marginCompete, "Overlap resolution across types by priority, then radius attribute (maxRadius= bounds the margin)."},
        {"WeightedMeshSpawner", {"in"}, {"out"}, nWeightedMeshSpawner, true, marginNone, "Picks a mesh per point by weight and emits instances."},
        {"ActorSpawner", {"in"}, {"out"}, nActorSpawner, true, marginNone, "Emits one actor spawn record per point."},
        {"Input", {}, {"out"}, nullptr, false, marginNone, "Subgraph entry: the points the caller passed in."},
        {"Output", {"in"}, {}, nullptr, false, marginNone, "Subgraph exit: the points handed back to the caller."},
        {"Subgraph", {"in"}, {"out"}, nullptr, false, marginNone, "Runs graph=<name> once with the input points."},
        {"Loop", {"in"}, {"out"}, nullptr, false, marginNone, "Runs graph=<name> once per input point and merges the outputs."},
    };
    return defs;
}

} // namespace aver::world
