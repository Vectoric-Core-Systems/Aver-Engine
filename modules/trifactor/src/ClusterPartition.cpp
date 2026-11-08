#include "aver/trifactor/ClusterPartition.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

namespace aver::trifactor {

namespace {

struct Edge { u32 a, b; u32 w; };

// Weighted cluster adjacency in CSR form.
struct Graph {
    std::vector<u32> start;   // n + 1
    std::vector<u32> nbr;
    std::vector<u32> wgt;
};

// A vertex shared by more clusters than this (a fan hub) adds no useful locality signal and would
// cost quadratic edges, so it is skipped.
constexpr usize kMaxSharers = 32;

Graph buildGraph(const u32* idx, const u32* counts, usize n, usize vertexCount) {
    std::vector<std::pair<u32, u32>> vc;   // (vertex, cluster)
    usize off = 0;
    for (usize c = 0; c < n; ++c) {
        for (u32 i = 0; i < counts[c]; ++i)
            if (idx[off + i] < vertexCount) vc.emplace_back(idx[off + i], static_cast<u32>(c));
        off += counts[c];
    }
    std::sort(vc.begin(), vc.end());
    vc.erase(std::unique(vc.begin(), vc.end()), vc.end());

    std::vector<Edge> edges;
    for (usize i = 0; i < vc.size();) {
        usize j = i;
        while (j < vc.size() && vc[j].first == vc[i].first) ++j;
        if (j - i > 1 && j - i <= kMaxSharers)
            for (usize x = i; x < j; ++x)
                for (usize y = x + 1; y < j; ++y) edges.push_back({vc[x].second, vc[y].second, 1});
        i = j;
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) {
        return l.a != r.a ? l.a < r.a : l.b < r.b;
    });
    std::vector<Edge> merged;
    for (const Edge& e : edges) {
        if (!merged.empty() && merged.back().a == e.a && merged.back().b == e.b) merged.back().w += e.w;
        else merged.push_back(e);
    }

    Graph g;
    g.start.assign(n + 1, 0);
    for (const Edge& e : merged) { ++g.start[e.a + 1]; ++g.start[e.b + 1]; }
    for (usize i = 0; i < n; ++i) g.start[i + 1] += g.start[i];
    g.nbr.resize(g.start[n]);
    g.wgt.resize(g.start[n]);
    std::vector<u32> fill(g.start.begin(), g.start.end() - 1);
    for (const Edge& e : merged) {
        g.nbr[fill[e.a]] = e.b; g.wgt[fill[e.a]++] = e.w;
        g.nbr[fill[e.b]] = e.a; g.wgt[fill[e.b]++] = e.w;
    }
    return g;
}

struct Partitioner {
    const Graph& g;
    const std::vector<Vec3>& centroid;
    std::vector<u32>& out;
    u32 nextGroup = 0;

    std::vector<u32> stamp;    // stamp[v] == curStamp  <=>  v is in the set being split
    u32 curStamp = 0;
    std::vector<u8> side;      // 0 = left, 1 = right (valid inside the set)
    std::vector<i64> gain;
    std::vector<u8> locked;

    Partitioner(const Graph& gr, const std::vector<Vec3>& c, std::vector<u32>& o)
        : g(gr), centroid(c), out(o), stamp(c.size(), 0), side(c.size(), 0), gain(c.size(), 0), locked(c.size(), 0) {}

    void emit(const std::vector<u32>& nodes) {
        for (u32 v : nodes) out[v] = nextGroup;
        ++nextGroup;
    }

    i64 computeGain(u32 v) const {
        i64 ext = 0, in = 0;
        for (u32 e = g.start[v]; e < g.start[v + 1]; ++e) {
            const u32 u = g.nbr[e];
            if (stamp[u] != curStamp) continue;
            (side[u] == side[v] ? in : ext) += g.wgt[e];
        }
        return ext - in;
    }

    // One FM pass. Returns true when it reduced the cut.
    bool refinePass(const std::vector<u32>& nodes, u32 lo, u32 hi, u32& leftSize) {
        using Entry = std::pair<i64, u32>;
        std::priority_queue<Entry> heap[2];
        for (u32 v : nodes) {
            locked[v] = 0;
            gain[v] = computeGain(v);
            heap[side[v]].emplace(gain[v], v);
        }
        std::vector<u32> moved;
        i64 cum = 0, best = 0;
        usize bestLen = 0;
        u32 size = leftSize, bestSize = leftSize;
        usize sinceBest = 0;
        const usize patience = std::max<usize>(32, nodes.size() / 8);

        for (;;) {
            // The side whose move keeps the left size inside [lo, hi].
            const bool canLeft = size > lo;     // move a left node to the right
            const bool canRight = size < hi;    // move a right node to the left
            u32 pick = ~0u;
            int pickSide = -1;
            for (int s = 0; s < 2; ++s) {
                if ((s == 0 && !canLeft) || (s == 1 && !canRight)) continue;
                while (!heap[s].empty()) {
                    const auto [gn, v] = heap[s].top();
                    if (locked[v] || side[v] != s || gain[v] != gn) { heap[s].pop(); continue; }
                    if (pick == ~0u || gn > gain[pick]) { pick = v; pickSide = s; }
                    break;
                }
            }
            if (pick == ~0u) break;
            heap[pickSide].pop();

            cum += gain[pick];
            locked[pick] = 1;
            side[pick] ^= 1;
            size = pickSide == 0 ? size - 1 : size + 1;
            moved.push_back(pick);
            for (u32 e = g.start[pick]; e < g.start[pick + 1]; ++e) {
                const u32 u = g.nbr[e];
                if (stamp[u] != curStamp || locked[u]) continue;
                gain[u] = computeGain(u);
                heap[side[u]].emplace(gain[u], u);
            }
            if (cum > best) { best = cum; bestLen = moved.size(); bestSize = size; sinceBest = 0; }
            else if (++sinceBest > patience) break;
        }
        for (usize i = moved.size(); i-- > bestLen;) side[moved[i]] ^= 1;
        leftSize = bestSize;
        return best > 0;
    }

    void split(std::vector<u32> nodes, u32 k) {
        const u32 n = static_cast<u32>(nodes.size());
        if (k <= 1 || n <= kMaxGroupSizeBisect) { emit(nodes); return; }

        const u32 k1 = k / 2, k2 = k - k1;
        const u32 lo = std::max(k1 * kMinGroupSize, n - std::min(n, k2 * kMaxGroupSizeBisect));
        const u32 hi = std::min(k1 * kMaxGroupSizeBisect, n - std::min(n, k2 * kMinGroupSize));
        u32 target = static_cast<u32>((u64(n) * k1 + k / 2) / k);
        target = std::max(lo, std::min(hi, target));

        Vec3 mn = centroid[nodes[0]], mx = mn;
        for (u32 v : nodes) {
            const Vec3& c = centroid[v];
            mn = {std::min(mn.x, c.x), std::min(mn.y, c.y), std::min(mn.z, c.z)};
            mx = {std::max(mx.x, c.x), std::max(mx.y, c.y), std::max(mx.z, c.z)};
        }
        const f32 ex = mx.x - mn.x, ey = mx.y - mn.y, ez = mx.z - mn.z;
        const int axis = (ex >= ey && ex >= ez) ? 0 : (ey >= ez ? 1 : 2);
        auto coord = [&](u32 v) { return axis == 0 ? centroid[v].x : axis == 1 ? centroid[v].y : centroid[v].z; };
        std::sort(nodes.begin(), nodes.end(), [&](u32 a, u32 b) {
            const f32 ca = coord(a), cb = coord(b);
            return ca != cb ? ca < cb : a < b;
        });

        ++curStamp;
        for (u32 i = 0; i < n; ++i) { stamp[nodes[i]] = curStamp; side[nodes[i]] = i < target ? 0 : 1; }
        u32 leftSize = target;
        for (int pass = 0; pass < 4; ++pass)
            if (!refinePass(nodes, lo, hi, leftSize)) break;

        std::vector<u32> left, right;
        left.reserve(leftSize);
        right.reserve(n - leftSize);
        for (u32 v : nodes) (side[v] == 0 ? left : right).push_back(v);
        nodes.clear();
        nodes.shrink_to_fit();
        split(std::move(left), k1);
        split(std::move(right), k2);
    }
};

} // namespace

usize partitionClustersBisect(u32* partition, const u32* idx, const u32* counts, usize n, const f32* positions,
                              usize vertexCount, usize strideBytes, usize target) {
    if (n == 0) return 0;
    const usize stride = strideBytes / sizeof(f32);
    std::vector<Vec3> centroid(n, Vec3{0, 0, 0});
    usize off = 0;
    for (usize c = 0; c < n; ++c) {
        f64 sx = 0, sy = 0, sz = 0;
        u32 m = 0;
        for (u32 i = 0; i < counts[c]; ++i) {
            const u32 v = idx[off + i];
            if (v >= vertexCount) continue;
            sx += positions[usize(v) * stride + 0];
            sy += positions[usize(v) * stride + 1];
            sz += positions[usize(v) * stride + 2];
            ++m;
        }
        off += counts[c];
        if (m) centroid[c] = Vec3{static_cast<f32>(sx / m), static_cast<f32>(sy / m), static_cast<f32>(sz / m)};
    }

    const Graph g = buildGraph(idx, counts, n, vertexCount);
    std::vector<u32> out(n, 0);
    Partitioner p(g, centroid, out);

    const usize tgt = std::max<usize>(kMinGroupSize, std::min<usize>(target, kMaxGroupSizeBisect));
    const usize kLo = (n + kMaxGroupSizeBisect - 1) / kMaxGroupSizeBisect;
    const usize kHi = std::max<usize>(kLo, n / kMinGroupSize);
    const usize k = std::max(kLo, std::min(kHi, (n + tgt / 2) / tgt));

    std::vector<u32> all(n);
    std::iota(all.begin(), all.end(), 0u);
    p.split(std::move(all), static_cast<u32>(k));
    for (usize i = 0; i < n; ++i) partition[i] = out[i];
    return p.nextGroup;
}

u64 partitionCutWeight(const u32* partition, const u32* idx, const u32* counts, usize n, usize vertexCount) {
    const Graph g = buildGraph(idx, counts, n, vertexCount);
    u64 cut = 0;
    for (usize v = 0; v < n; ++v)
        for (u32 e = g.start[v]; e < g.start[v + 1]; ++e)
            if (g.nbr[e] > v && partition[g.nbr[e]] != partition[v]) cut += g.wgt[e];
    return cut;
}

} // namespace aver::trifactor
