// HairGenerate.cpp -- the procedural groom. See HairGenerate.hpp for the construction.
#include "aver/hair/HairGenerate.hpp"

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace aver::hair {
namespace {

bool fail(std::string* why, std::string m) {
    if (why) *why = std::move(m);
    return false;
}

// PCG32 (O'Neill, published): a 64-bit LCG state with an xorshift-rotate output permutation.
struct Pcg32 {
    u64 state = 0;
    u64 inc = 1;
    explicit Pcg32(u64 seed, u64 sequence = 0x5eed) {
        inc = (sequence << 1u) | 1u;
        next();
        state += seed;
        next();
    }
    u32 next() {
        const u64 old = state;
        state = old * 6364136223846793005ULL + inc;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }
    // [0, 1)
    f32 uniform() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }
    u32 below(u32 n) { return n == 0 ? 0 : static_cast<u32>((static_cast<u64>(next()) * n) >> 32); }
};

f32 smooth(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Vec3 at(const std::vector<f32>& p, u32 i) { return Vec3(p[static_cast<usize>(i) * 3], p[static_cast<usize>(i) * 3 + 1], p[static_cast<usize>(i) * 3 + 2]); }

f32 maskAt(const ScalpMesh& s, u32 v) { return s.mask.empty() ? 1.0f : std::clamp(s.mask[v], 0.0f, 1.0f); }

struct Root {
    Vec3 p;
    Vec3 n;
};

// A uniform grid over the roots found so far, cell = the spacing, so a neighbour test looks at 27 cells.
struct RootGrid {
    f32 cell = 0.0f;
    std::unordered_map<u64, std::vector<u32>> cells;

    static u64 key(i32 x, i32 y, i32 z) {
        return (static_cast<u64>(static_cast<u32>(x) & 0x1fffffu) << 42) | (static_cast<u64>(static_cast<u32>(y) & 0x1fffffu) << 21) |
               static_cast<u64>(static_cast<u32>(z) & 0x1fffffu);
    }
    void cellOf(const Vec3& p, i32& x, i32& y, i32& z) const {
        x = static_cast<i32>(std::floor(p.x / cell));
        y = static_cast<i32>(std::floor(p.y / cell));
        z = static_cast<i32>(std::floor(p.z / cell));
    }
    void rebuild(const std::vector<Root>& roots, f32 newCell) {
        cell = newCell;
        cells.clear();
        if (cell <= 0.0f) return;
        for (u32 i = 0; i < roots.size(); ++i) add(roots[i].p, i);
    }
    void add(const Vec3& p, u32 index) {
        if (cell <= 0.0f) return;
        i32 x, y, z;
        cellOf(p, x, y, z);
        cells[key(x, y, z)].push_back(index);
    }
    bool tooClose(const std::vector<Root>& roots, const Vec3& p) const {
        if (cell <= 0.0f) return false;
        i32 x, y, z;
        cellOf(p, x, y, z);
        const f32 r2 = cell * cell;
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dy = -1; dy <= 1; ++dy)
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const auto it = cells.find(key(x + dx, y + dy, z + dz));
                    if (it == cells.end()) continue;
                    for (u32 other : it->second)
                        if (distSquared(roots[other].p, p) < r2) return true;
                }
        return false;
    }
};

// The mask-weighted triangle table shared by area and sampling.
struct TriTable {
    std::vector<f64> cumulative;
    std::vector<f32> maxMask;
    f64 total = 0.0;
};

TriTable buildTable(const ScalpMesh& s) {
    TriTable t;
    const usize tc = s.triangleCount();
    t.cumulative.resize(tc);
    t.maxMask.resize(tc);
    for (usize i = 0; i < tc; ++i) {
        const u32 ia = s.triangles[i * 3], ib = s.triangles[i * 3 + 1], ic = s.triangles[i * 3 + 2];
        const Vec3 a = at(s.positions, ia), b = at(s.positions, ib), c = at(s.positions, ic);
        const f32 area = 0.5f * cross(b - a, c - a).size();
        const f32 ma = maskAt(s, ia), mb = maskAt(s, ib), mc = maskAt(s, ic);
        t.maxMask[i] = std::max({ma, mb, mc});
        t.total += static_cast<f64>(area) * static_cast<f64>((ma + mb + mc) / 3.0f);
        t.cumulative[i] = t.total;
    }
    return t;
}

bool scalpUsable(const ScalpMesh& s) {
    if (s.positions.size() % 3 != 0 || s.triangles.size() % 3 != 0 || s.triangles.empty()) return false;
    if (!s.mask.empty() && s.mask.size() != s.vertexCount()) return false;
    for (f32 v : s.positions)
        if (!std::isfinite(v)) return false;
    for (u32 i : s.triangles)
        if (i >= s.vertexCount()) return false;
    for (f32 m : s.mask)
        if (!std::isfinite(m)) return false;
    return true;
}

// Any unit vector perpendicular to v.
Vec3 perpendicular(const Vec3& v) {
    const Vec3 axis = std::fabs(v.x) < 0.6f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    return cross(v, axis).getSafeNormal();
}

} // namespace

f32 scalpArea(const ScalpMesh& scalp) {
    if (!scalpUsable(scalp)) return 0.0f;
    return static_cast<f32>(buildTable(scalp).total);
}

bool generateHair(const fmt::OcHairGenerate& g, const ScalpMesh& scalp, fmt::OcHairStrands& out, GenerateStats* stats, std::string* why) {
    if (stats) *stats = GenerateStats{};
    if (g.strands == 0) return fail(why, "hair generate: strands is 0");
    if (g.segments < 2 || g.segments + 1 > fmt::kOcHairMaxStrandVertices) return fail(why, "hair generate: segments must be 2 to 254");
    if (!(g.lengthCm[0] > 0.0f) || g.lengthCm[1] < g.lengthCm[0]) return fail(why, "hair generate: lengthCm must be 0 < min <= max");
    if (!scalpUsable(scalp)) return fail(why, "hair generate: the scalp mesh is empty or malformed");
    const TriTable table = buildTable(scalp);
    if (!(table.total > 0.0)) return fail(why, "hair generate: the scalp has no area under the mask");

    const u32 count = g.strands;
    Pcg32 rng(g.seed);

    // The centre of the scalp orients triangle normals outward (a head's roots grow away from its middle); a flat scalp keeps its winding.
    Vec3 centre(0, 0, 0);
    for (u32 v = 0; v < scalp.vertexCount(); ++v) centre += at(scalp.positions, v);
    centre = centre * (1.0f / static_cast<f32>(scalp.vertexCount()));
    f32 extent = 0.0f;
    for (u32 v = 0; v < scalp.vertexCount(); ++v) extent = std::max(extent, dist(at(scalp.positions, v), centre));

    // ---- roots, by dart throwing ----
    std::vector<Root> roots;
    roots.reserve(count);
    RootGrid grid;
    f32 spacing = 0.6f * std::sqrt(static_cast<f32>(table.total) / static_cast<f32>(count));
    u32 rounds = 0;
    const u64 attemptCap = static_cast<u64>(count) * 400u + 4000u;
    u64 attempts = 0;
    grid.rebuild(roots, spacing);
    while (roots.size() < count && attempts < attemptCap) {
        const u64 roundCap = std::max<u64>(static_cast<u64>(count) * 30u, 1000u);
        for (u64 a = 0; a < roundCap && roots.size() < count && attempts < attemptCap; ++a, ++attempts) {
            const f64 pick = static_cast<f64>(rng.uniform()) * table.total;
            const usize tri = static_cast<usize>(std::lower_bound(table.cumulative.begin(), table.cumulative.end(), pick) - table.cumulative.begin());
            const usize t = std::min(tri, table.cumulative.size() - 1);
            const u32 ia = scalp.triangles[t * 3], ib = scalp.triangles[t * 3 + 1], ic = scalp.triangles[t * 3 + 2];
            const f32 r1 = std::sqrt(rng.uniform()), r2 = rng.uniform();
            const f32 b0 = 1.0f - r1, b1 = r1 * (1.0f - r2), b2 = r1 * r2;
            const f32 accept = rng.uniform();
            if (!scalp.mask.empty()) {
                const f32 m = b0 * maskAt(scalp, ia) + b1 * maskAt(scalp, ib) + b2 * maskAt(scalp, ic);
                if (accept * std::max(table.maxMask[t], 1.0e-6f) >= m) continue;
            }
            const Vec3 pa = at(scalp.positions, ia), pb = at(scalp.positions, ib), pc = at(scalp.positions, ic);
            const Vec3 p = pa * b0 + pb * b1 + pc * b2;
            if (grid.tooClose(roots, p)) continue;
            Vec3 n = cross(pb - pa, pc - pa).getSafeNormal();
            if (n.sizeSquared() == 0.0f) continue;                    // a degenerate triangle grows nothing
            const f32 side = dot(n, p - centre);
            if (std::fabs(side) > 1.0e-3f * std::max(extent, 1.0f) && side < 0.0f) n = -n;
            roots.push_back({p, n});
            grid.add(p, static_cast<u32>(roots.size() - 1));
        }
        if (roots.size() < count) {
            ++rounds;
            spacing = rounds > 24 ? 0.0f : spacing * 0.85f;
            grid.rebuild(roots, spacing);
        }
    }
    if (roots.size() < count) return fail(why, "hair generate: could only place " + std::to_string(roots.size()) + " of " + std::to_string(count) +
                                                " roots (the mask leaves too little scalp)");

    // ---- guides ----
    const u32 guideCount = std::min(g.clumps, count);
    std::vector<u32> order(count);
    for (u32 i = 0; i < count; ++i) order[i] = i;
    for (u32 i = count; i > 1; --i) std::swap(order[i - 1], order[rng.below(i)]);
    std::vector<i32> guideOf(count, -1);                              // root -> index of its guide root, -1 = none
    std::vector<u32> guides(order.begin(), order.begin() + guideCount);
    for (u32 gi : guides) guideOf[gi] = static_cast<i32>(gi);
    if (guideCount > 0 && g.clumpRadiusCm > 0.0f) {
        const f32 r2 = g.clumpRadiusCm * g.clumpRadiusCm;
        for (u32 i = 0; i < count; ++i) {
            if (guideOf[i] >= 0) continue;
            f32 best = r2;
            for (u32 gi : guides) {
                const f32 d2 = distSquared(roots[i].p, roots[gi].p);
                if (d2 < best) { best = d2; guideOf[i] = static_cast<i32>(gi); }
            }
        }
    }

    // ---- shapes ----
    const u32 points = g.segments + 1;
    const Vec3 styleDir = Vec3(g.style[0], g.style[1], g.style[2]).getSafeNormal();
    const Vec3 partDir = Vec3(g.partDirection[0], g.partDirection[1], g.partDirection[2]).getSafeNormal();
    std::vector<std::vector<Vec3>> own(count);
    for (u32 i = 0; i < count; ++i) {
        const f32 length = g.lengthCm[0] + (g.lengthCm[1] - g.lengthCm[0]) * rng.uniform();
        const f32 phase = rng.uniform() * kTwoPi;
        const f32 step = length / static_cast<f32>(g.segments);
        const Root& r = roots[i];
        const Vec3 side = cross(r.n, partDir).getSafeNormal();
        const f32 partPush = (g.partWidthCm > 0.0f && side.sizeSquared() > 0.0f) ? std::clamp(dot(r.p - centre, side) / g.partWidthCm, -1.0f, 1.0f) : 0.0f;

        std::vector<Vec3> base(points);
        std::vector<Vec3> dirs(points);
        Vec3 p = r.p;
        for (u32 k = 0; k < points; ++k) {
            const f32 f = static_cast<f32>(k) / static_cast<f32>(g.segments);
            const f32 w = g.styleWeight * smooth(f);
            Vec3 d = (r.n * (1.0f - w) + styleDir * w).getSafeNormal();
            if (d.sizeSquared() == 0.0f) d = r.n;
            if (partPush != 0.0f) {
                d = (d + side * (partPush * 0.35f * (1.0f - f) * (1.0f - f))).getSafeNormal();
            }
            base[k] = p;
            dirs[k] = d;
            p = p + d * step;
        }
        // Curl: a helix around the local axis, zero at the root.
        if (g.curlCm > 0.0f) {
            for (u32 k = 1; k < points; ++k) {
                const f32 f = static_cast<f32>(k) / static_cast<f32>(g.segments);
                const Vec3 u = perpendicular(dirs[k]);
                const Vec3 v = cross(dirs[k], u);
                const f32 theta = phase + kTwoPi * g.curlFrequency * (static_cast<f32>(k) * step / 10.0f);
                base[k] = base[k] + (u * std::cos(theta) + v * std::sin(theta)) * (g.curlCm * smooth(f / 0.2f));
            }
        }
        own[i] = std::move(base);
    }

    // ---- clump pull, toward the guide's finished curve ----
    std::vector<std::vector<Vec3>> shape = own;
    u32 followers = 0;
    for (u32 i = 0; i < count; ++i) {
        const i32 gi = guideOf[i];
        if (gi < 0 || static_cast<u32>(gi) == i) continue;
        ++followers;
        const Vec3 offset = roots[i].p - roots[static_cast<u32>(gi)].p;
        for (u32 k = 1; k < points; ++k) {
            const f32 f = static_cast<f32>(k) / static_cast<f32>(g.segments);
            const f32 m = g.clumpTightness * f;
            const Vec3 target = own[static_cast<u32>(gi)][k] + offset * (1.0f - f);
            shape[i][k] = own[i][k] * (1.0f - m) + target * m;
        }
    }

    // ---- out ----
    out = fmt::OcHairStrands{};
    out.points.reserve(static_cast<usize>(count) * points * 3);
    out.start.reserve(static_cast<usize>(count) + 1);
    out.material.assign(count, static_cast<u8>(std::min<u32>(g.material, fmt::kOcHairMaxMaterials)));
    for (u32 i = 0; i < count; ++i) {
        out.start.push_back(static_cast<u32>(out.points.size() / 3));
        for (u32 k = 0; k < points; ++k) {
            const Vec3& q = shape[i][k];
            out.points.push_back(q.x);
            out.points.push_back(q.y);
            out.points.push_back(q.z);
        }
    }
    out.start.push_back(static_cast<u32>(out.points.size() / 3));

    if (stats) {
        stats->strands = count;
        stats->guides = guideCount;
        stats->followers = followers;
        stats->scalpAreaCm2 = static_cast<f32>(table.total);
        stats->spacingCm = spacing;
        stats->relaxRounds = rounds;
    }
    return true;
}

} // namespace aver::hair
