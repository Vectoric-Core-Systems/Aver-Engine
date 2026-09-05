#include "aver/formats/LightmapUnwrap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <unordered_map>

namespace aver::fmt {
namespace {

// At least 2 texels between charts and at the atlas edge (LightmapUnwrap.hpp point 4: bilinear
// sampling and mip generation both read past a chart's own border). Kept as one named constant
// rather than a magic 2 sprinkled through the packer, in case a future caller needs to widen it for
// a coarser mip chain than this v1 was checked against (which is to say: not checked against any
// mip chain at all, since this module never touches a GPU).
constexpr u32 kGutterTexels = 2;

// Shelf packing gives up this many times, halving its scale estimate each time, before reporting
// failure. Each retry is cheap (it is just arithmetic over however many charts there are), and this
// many halvings shrinks content to roughly 1/16000th of its first guess -- past that, the atlas is
// genuinely too small for the gutter overhead alone, not for a bad initial guess.
constexpr int kMaxPackAttempts = 14;

struct Vec3f { f32 x = 0, y = 0, z = 0; };
Vec3f sub(const Vec3f& a, const Vec3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3f add(const Vec3f& a, const Vec3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f32 length(const Vec3f& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

// Which of the 6 signed axis buckets a normal falls into: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z. Ties (a
// component exactly equal in magnitude to another) favour the earlier axis in that list, which only
// ever matters on perfectly diagonal input (a 45-degree corner normal) -- there is no "more correct"
// bucket for those, only an arbitrary but deterministic one.
u32 dominantAxis(const Vec3f& n) {
    const f32 ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay && ax >= az) return n.x >= 0.0f ? 0u : 1u;
    if (ay >= az) return n.y >= 0.0f ? 2u : 3u;
    return n.z >= 0.0f ? 4u : 5u;
}

// Flattens a 3D point onto its chart's dominant axis plane. Cyclic X->(y,z), Y->(z,x), Z->(x,y) --
// the same coordinate pair for the positive and negative bucket of a given axis; see the header for
// why no mirror-correction is applied for the negative half.
void project(const Vec3f& p, u32 axis, f32& u, f32& v) {
    switch (axis >> 1) {
        case 0: u = p.y; v = p.z; break;   // X faces
        case 1: u = p.z; v = p.x; break;   // Y faces
        default: u = p.x; v = p.y; break;  // Z faces
    }
}

// Union-find over triangle indices, unioned within one edge-key map so a triangle only merges with
// another that shares BOTH an edge and an axis bucket (see buildCharts).
struct DisjointSet {
    std::vector<u32> parent;
    explicit DisjointSet(usize n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    u32 find(u32 x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
    void unite(u32 a, u32 b) {
        a = find(a); b = find(b);
        if (a != b) parent[a] = b;
    }
};

struct Chart {
    u32 axis = 0;
    std::vector<u32> verts;   // distinct INPUT vertex indices this chart touches, in first-seen order
    std::vector<u32> tris;    // triangle indices this chart owns, for remapping the index buffer
    f32 minU = 0, minV = 0, maxU = 0, maxV = 0;
};

// One placed chart's texel-space content rectangle (excluding its own gutter halo).
struct Placement {
    u32 chartIndex = 0;
    u32 x = 0, y = 0;
    u32 w = 0, h = 0;
};

// Packs `sizes` (world-space w,h per chart, matching `charts` by index) into a square atlas of edge
// `atlasEdge` at world-to-texel `scale`, leaving kGutterTexels between charts and at the border.
// Returns false the instant a chart would not fit -- the caller retries at a smaller scale rather
// than this function doing its own backtracking, which keeps the shelf logic itself dead simple.
bool tryShelfPack(const std::vector<std::pair<f32, f32>>& sizes, f32 scale, u32 atlasEdge,
                   std::vector<Placement>& out) {
    out.clear();
    if (atlasEdge <= 2 * kGutterTexels) return false;
    const u32 usableEdge = atlasEdge - 2 * kGutterTexels;

    std::vector<u32> order(sizes.size());
    std::iota(order.begin(), order.end(), 0u);
    // Tallest first: the classic shelf heuristic. A shelf's height is set by its first (tallest)
    // occupant, so placing the tall ones first means later, shorter charts fill in beside them
    // instead of every chart forcing its own new shelf.
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (sizes[a].second != sizes[b].second) return sizes[a].second > sizes[b].second;
        return sizes[a].first > sizes[b].first;
    });

    u32 cursorX = 0, cursorY = 0, shelfH = 0;
    for (u32 idx : order) {
        const u32 w = std::max(1u, static_cast<u32>(std::ceil(sizes[idx].first * scale)));
        const u32 h = std::max(1u, static_cast<u32>(std::ceil(sizes[idx].second * scale)));

        if (cursorX != 0 && cursorX + w > usableEdge) {
            // Doesn't fit on this shelf -- start a new one below it.
            cursorX = 0;
            cursorY += shelfH + kGutterTexels;
            shelfH = 0;
        }
        if (w > usableEdge || cursorY + h > usableEdge) return false;   // never fits at this scale

        out.push_back({idx, kGutterTexels + cursorX, kGutterTexels + cursorY, w, h});
        cursorX += w + kGutterTexels;
        shelfH = std::max(shelfH, h);
    }
    return true;
}

} // namespace

bool unwrapForLightmap(const f32* positions, const f32* normals, u32 vertexCount,
                        const u32* indices, u32 indexCount, u32 targetAtlasEdge,
                        UnwrapResult& out, std::string* err) {
    out = UnwrapResult{};
    const auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };

    if (vertexCount == 0 || positions == nullptr) return fail("no vertices");
    if (indexCount % 3 != 0) return fail("indexCount is not a multiple of 3");
    if (indexCount > 0 && indices == nullptr) return fail("null index buffer with a non-zero indexCount");
    if (targetAtlasEdge <= 2 * kGutterTexels)
        return fail("targetAtlasEdge is too small to hold even the gutter");
    for (u32 i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount) return fail("an index references a vertex past vertexCount");

    out.atlasWidth = targetAtlasEdge;
    out.atlasHeight = targetAtlasEdge;
    // Sized later, once the charts are known: the output vertex list is REBUILT with a copy per
    // (chart, vertex) pair, so its length is not vertexCount. Cleared here so the zero-triangle
    // early-out below returns something well-formed rather than whatever the caller passed in.
    out.uv.clear();
    out.vertexSource.clear();
    out.indices.clear();

    const u32 triCount = indexCount / 3;
    if (triCount == 0) return true;   // legal: a mesh with no faces unwraps to nothing

    const auto posAt = [&](u32 v) { return Vec3f{positions[v * 3 + 0], positions[v * 3 + 1], positions[v * 3 + 2]}; };

    // Per-triangle dominant axis. Preferring the AVERAGED VERTEX NORMAL over a geometric
    // cross-product normal: a caller unwrapping a mesh already has normals it trusts (they came from
    // the same source the render path shades with), and using them keeps a chart boundary aligned
    // with wherever the mesh's own shading already puts a hard edge, rather than requiring positions
    // to also carry a consistent winding order this module would otherwise depend on. When the three
    // vertex normals cancel out (a degenerate sliver, or a caller that passed all-zero normals) this
    // falls back to the geometric normal, so a triangle is never left unclassified.
    std::vector<u32> triAxis(triCount);
    std::vector<Vec3f> triNormal(triCount);
    for (u32 t = 0; t < triCount; ++t) {
        const u32 i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
        Vec3f n{0, 0, 0};
        if (normals != nullptr) {
            n.x = normals[i0 * 3 + 0] + normals[i1 * 3 + 0] + normals[i2 * 3 + 0];
            n.y = normals[i0 * 3 + 1] + normals[i1 * 3 + 1] + normals[i2 * 3 + 1];
            n.z = normals[i0 * 3 + 2] + normals[i1 * 3 + 2] + normals[i2 * 3 + 2];
        }
        if (length(n) < 1e-8f) {
            const Vec3f p0 = posAt(i0), p1 = posAt(i1), p2 = posAt(i2);
            n = cross(sub(p1, p0), sub(p2, p0));
        }
        const f32 len = length(n);
        if (len > 1e-12f) { n.x /= len; n.y /= len; n.z /= len; }
        triNormal[t] = n;
        triAxis[t] = dominantAxis(n);
    }

    // CHART = one connected component of triangles that share BOTH an axis bucket and an edge
    // (an edge here means two triangles referencing the same pair of vertex INDICES, not merely two
    // vertices at the same position -- see the header's note on this being an under-merge relative
    // to a position-welding unwrapper like xatlas). This is what keeps two triangles that face the
    // same direction but belong to unrelated, disconnected surfaces -- two opposite walls of a room,
    // both facing +X -- from ever landing in one chart: nothing about their normals differs, but
    // they share no edge, so the edge map below never unions them.
    DisjointSet dsu(triCount);
    {
        // Keyed by (axis, low vertex, high vertex) so an edge only unions triangles already known to
        // share a bucket -- one map instead of 6, since the axis rides along in the key itself. A
        // struct key with real equality rather than a bit-packed u64: vertex indices are full u32s,
        // and packing (axis, lo, hi) into 64 bits with room to spare for every field would need to
        // steal bits from one of them, which is exactly the kind of silent, index-count-dependent
        // collision this engine's own GiCache hash gets away with only because a wrong ANSWER there
        // just costs a rebuild -- a wrong answer here silently welds two unrelated charts together.
        struct EdgeKey {
            u32 axis, lo, hi;
            bool operator==(const EdgeKey& o) const { return axis == o.axis && lo == o.lo && hi == o.hi; }
        };
        struct EdgeKeyHash {
            usize operator()(const EdgeKey& k) const {
                u64 h = 1469598103934665603ull;
                h = (h ^ k.axis) * 1099511628211ull;
                h = (h ^ k.lo) * 1099511628211ull;
                h = (h ^ k.hi) * 1099511628211ull;
                return static_cast<usize>(h);
            }
        };
        std::unordered_map<EdgeKey, u32, EdgeKeyHash> edgeOwner;
        edgeOwner.reserve(static_cast<usize>(triCount) * 3);
        for (u32 t = 0; t < triCount; ++t) {
            const u32 v[3] = {indices[t * 3 + 0], indices[t * 3 + 1], indices[t * 3 + 2]};
            for (int e = 0; e < 3; ++e) {
                const u32 a = v[e], b = v[(e + 1) % 3];
                const EdgeKey key{triAxis[t], std::min(a, b), std::max(a, b)};
                auto it = edgeOwner.find(key);
                if (it == edgeOwner.end()) edgeOwner.emplace(key, t);
                else dsu.unite(it->second, t);
            }
        }
    }

    // Gather triangles by DSU root into charts, preserving first-seen order for determinism (the
    // packer sorts by size anyway, but a stable input order keeps two runs over identical input
    // byte-identical, which is worth having for anything a test diffs).
    std::unordered_map<u32, u32> rootToChart;
    std::vector<Chart> charts;
    for (u32 t = 0; t < triCount; ++t) {
        const u32 root = dsu.find(t);
        auto it = rootToChart.find(root);
        u32 chartIdx;
        if (it == rootToChart.end()) {
            chartIdx = static_cast<u32>(charts.size());
            rootToChart.emplace(root, chartIdx);
            charts.push_back(Chart{});
            charts.back().axis = triAxis[t];
        } else {
            chartIdx = it->second;
        }
        Chart& c = charts[chartIdx];
        c.tris.push_back(t);
        for (int k = 0; k < 3; ++k) {
            const u32 vi = indices[t * 3 + k];
            if (std::find(c.verts.begin(), c.verts.end(), vi) == c.verts.end()) c.verts.push_back(vi);
        }
    }
    out.chartCount = static_cast<u32>(charts.size());

    // Project each chart flat and take its 2D bounds.
    std::vector<std::pair<f32, f32>> worldSize(charts.size());
    for (usize ci = 0; ci < charts.size(); ++ci) {
        Chart& c = charts[ci];
        bool first = true;
        for (u32 vi : c.verts) {
            f32 u, v;
            project(posAt(vi), c.axis, u, v);
            if (first) { c.minU = c.maxU = u; c.minV = c.maxV = v; first = false; }
            else {
                c.minU = std::min(c.minU, u); c.maxU = std::max(c.maxU, u);
                c.minV = std::min(c.minV, v); c.maxV = std::max(c.maxV, v);
            }
        }
        // A pancake-flat chart (all verts collinear in the projection, e.g. a degenerate sliver
        // triangle) would pack as a 0-texel-wide rectangle. Floor it to a hairline width/height so
        // it still reserves a real cell rather than aliasing on top of whatever lands at its exact
        // coordinate.
        const f32 w = std::max(c.maxU - c.minU, 1e-4f);
        const f32 h = std::max(c.maxV - c.minV, 1e-4f);
        worldSize[ci] = {w, h};
    }

    // Initial scale guess: fill half the USABLE atlas area (post-gutter) with chart content. Half,
    // not all of it, because the shelf packer is not going to perfectly tile these rectangles --
    // every shelf's leftover width past its last chart is wasted, and every chart's own bounding-box
    // padding over its true silhouette (see the header: no rotation, no polygon fit) is wasted too.
    // Measured against nothing more scientific than "the retry loop below corrects an over-optimistic
    // guess in a couple of halvings, and an over-cautious one just leaves the atlas emptier than it
    // had to be" -- this is a starting point, not a tuned constant.
    f64 totalArea = 0.0;
    for (const auto& s : worldSize) totalArea += static_cast<f64>(s.first) * static_cast<f64>(s.second);
    const f32 usableEdge = static_cast<f32>(targetAtlasEdge - 2 * kGutterTexels);
    f32 scale = 1.0f;
    if (totalArea > 1e-12) {
        scale = static_cast<f32>(std::sqrt(0.5 * static_cast<f64>(usableEdge) * usableEdge / totalArea));
    }

    std::vector<Placement> placements;
    bool packed = false;
    for (int attempt = 0; attempt < kMaxPackAttempts; ++attempt) {
        if (tryShelfPack(worldSize, scale, targetAtlasEdge, placements)) { packed = true; break; }
        scale *= 0.7f;
    }
    if (!packed)
        return fail("could not pack every chart into targetAtlasEdge even after repeated shrinking");

    // Stamp UVs onto a REBUILT vertex list, one copy of each vertex per chart that touches it.
    //
    // The previous version wrote into the caller's own vertex indexing and let the last chart to
    // touch a shared vertex win. That is not a seam in the cosmetic sense -- the losing chart's
    // triangles end up with a corner whose UV points into an unrelated rectangle of the atlas, so
    // they sample another surface's lighting outright. Its own overlap test failed on a cube for
    // exactly this reason, which is what a test that checks the packing rather than the return code
    // is for.
    //
    // One pass per chart: allocate this chart's copies, stamp their UVs, and record the input vertex
    // each came from. Nothing is deduplicated across charts on purpose -- two charts sharing a
    // vertex is precisely the case that needs two copies.
    const f32 invW = 1.0f / static_cast<f32>(out.atlasWidth);
    const f32 invH = 1.0f / static_cast<f32>(out.atlasHeight);
    out.uv.clear();
    out.vertexSource.clear();
    out.indices.assign(static_cast<usize>(indexCount), 0u);

    // Per chart, input vertex -> its copy's index in the output. Cleared for each chart rather than
    // shared, so a vertex in two charts genuinely gets two entries.
    std::unordered_map<u32, u32> copyOf;
    for (const Placement& pl : placements) {
        const Chart& c = charts[pl.chartIndex];
        const f32 spanU = std::max(c.maxU - c.minU, 1e-4f);
        const f32 spanV = std::max(c.maxV - c.minV, 1e-4f);
        copyOf.clear();
        for (u32 vi : c.verts) {
            f32 u, v;
            project(posAt(vi), c.axis, u, v);
            const f32 tx = static_cast<f32>(pl.x) + (u - c.minU) / spanU * static_cast<f32>(pl.w);
            const f32 ty = static_cast<f32>(pl.y) + (v - c.minV) / spanV * static_cast<f32>(pl.h);
            const u32 dst = static_cast<u32>(out.vertexSource.size());
            copyOf.emplace(vi, dst);
            out.vertexSource.push_back(vi);
            out.uv.push_back(std::clamp(tx * invW, 0.0f, 1.0f));
            out.uv.push_back(std::clamp(ty * invH, 0.0f, 1.0f));
        }
        // Every triangle in this chart now points at this chart's own copies, so no triangle can
        // reference a vertex another chart placed.
        for (u32 t : c.tris)
            for (int k = 0; k < 3; ++k)
                out.indices[t * 3 + k] = copyOf.find(indices[t * 3 + k])->second;
    }

    return true;
}

} // namespace aver::fmt
