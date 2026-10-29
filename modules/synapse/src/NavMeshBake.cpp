#include "aver/synapse/NavMeshBake.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace aver::synapse {
namespace {

constexpr i32 kMaxH = 0x3fffffff;

struct Span {
    i32 smin = 0, smax = 0;
    f32 zTop = 0.0f;       // exact top of the walkable surface (or of the geometry, when the span is not walkable)
    bool walk = false;
};

Span mergeSpans(const Span& a, const Span& b, i32 thr) {
    Span m;
    m.smin = std::min(a.smin, b.smin);
    m.smax = std::max(a.smax, b.smax);
    if (a.smax > b.smax + thr) { m.walk = a.walk; m.zTop = a.zTop; }
    else if (b.smax > a.smax + thr) { m.walk = b.walk; m.zTop = b.zTop; }
    else {
        m.walk = a.walk || b.walk;
        if (a.walk && b.walk) m.zTop = std::max(a.zTop, b.zTop);
        else if (a.walk) m.zTop = a.zTop;
        else if (b.walk) m.zTop = b.zTop;
        else m.zTop = std::max(a.zTop, b.zTop);
    }
    return m;
}

void addSpan(std::vector<Span>& col, Span ns, i32 thr) {
    std::vector<Span> out;
    out.reserve(col.size() + 1);
    bool placed = false;
    for (const Span& s : col) {
        if (s.smax < ns.smin) { out.push_back(s); continue; }
        if (s.smin > ns.smax) {
            if (!placed) { out.push_back(ns); placed = true; }
            out.push_back(s);
            continue;
        }
        ns = mergeSpans(s, ns, thr);   // overlap: grow the new span
    }
    if (!placed) out.push_back(ns);
    col.swap(out);
}

// Clips a convex polygon to an axis-aligned slab; poly has at most 8 points.
int clipAxis(const Vec3* in, int n, Vec3* out, int axis, f32 limit, bool keepGreater) {
    int m = 0;
    auto get = [&](const Vec3& v) { return axis == 0 ? v.x : v.y; };
    for (int i = 0; i < n; ++i) {
        const Vec3& a = in[i];
        const Vec3& b = in[(i + 1) % n];
        const f32 da = (get(a) - limit) * (keepGreater ? 1.0f : -1.0f);
        const f32 db = (get(b) - limit) * (keepGreater ? 1.0f : -1.0f);
        if (da >= 0) out[m++] = a;
        if ((da >= 0) != (db >= 0)) {
            const f32 t = da / (da - db);
            out[m++] = a + (b - a) * t;
        }
    }
    return m;
}

bool pointInPoly(const std::vector<Vec3>& poly, f32 x, f32 y) {
    bool in = false;
    for (usize i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec3& a = poly[i];
        const Vec3& b = poly[j];
        if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) in = !in;
    }
    return in;
}

struct Cs {   // a walkable span, flattened
    i32 x = 0, y = 0;
    i32 bot = 0, ceil = 0;
    f32 z = 0.0f;
    i32 nb[4] = {-1, -1, -1, -1};   // +x, +y, -x, -y
    bool alive = true;
    i32 region = 0;
    i32 dist = 0;
    bool visited = false;
};

constexpr int kDx[4] = {1, 0, -1, 0};
constexpr int kDy[4] = {0, 1, 0, -1};

}  // namespace

bool navBake(const std::vector<Vec3>& verts, const std::vector<u32>& tris, const std::vector<NavBakeObstacle>& obstacles,
             const NavBakeParams& p, NavBakeResult& out, std::string* why, u64 maxCells) {
    out = NavBakeResult{};
    auto fail = [&](const char* msg) { if (why) *why = msg; return false; };
    if (p.cellSizeCm <= 0.0f || p.cellHeightCm <= 0.0f) return fail("cell size and cell height must be positive");
    if (tris.size() < 3 || tris.size() % 3 != 0) return fail("no triangles");
    for (u32 i : tris) if (i >= verts.size()) return fail("a triangle index is out of range");

    // Bounds.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (u32 i : tris) {
        const Vec3& v = verts[i];
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    if (p.useBounds) {
        lo = {std::max(lo.x, p.boundsMin.x), std::max(lo.y, p.boundsMin.y), std::max(lo.z, p.boundsMin.z)};
        hi = {std::min(hi.x, p.boundsMax.x), std::min(hi.y, p.boundsMax.y), std::min(hi.z, p.boundsMax.z)};
        if (lo.x >= hi.x || lo.y >= hi.y) return fail("the bounds hold no geometry");
    }
    const f32 cs = p.cellSizeCm, ch = p.cellHeightCm;
    const f32 border = std::max(p.borderSizeCm, 0.0f);
    const f32 ox = lo.x - border, oy = lo.y - border, baseZ = lo.z;
    const i32 W = static_cast<i32>(std::ceil((hi.x - lo.x + 2 * border) / cs)) + 1;
    const i32 H = static_cast<i32>(std::ceil((hi.y - lo.y + 2 * border) / cs)) + 1;
    if (W <= 0 || H <= 0 || static_cast<u64>(W) * static_cast<u64>(H) > maxCells) return fail("the grid is too large");

    const i32 climb = static_cast<i32>(std::floor(p.agentMaxClimbCm / ch));
    const i32 heightCells = static_cast<i32>(std::ceil(p.agentHeightCm / ch));
    const i32 radiusCells = static_cast<i32>(std::ceil(p.agentRadiusCm / cs));
    const f32 cosSlope = std::cos(p.agentMaxSlopeDeg * kDegToRad);

    std::vector<std::vector<Span>> cols(static_cast<usize>(W) * H);
    out.stats.columns = static_cast<u32>(cols.size());

    // ---- rasterise
    for (usize t = 0; t + 2 < tris.size(); t += 3) {
        const Vec3 &a = verts[tris[t]], &b = verts[tris[t + 1]], &c = verts[tris[t + 2]];
        const Vec3 n = cross(b - a, c - a);
        const f32 nl = n.size();
        if (nl < 1e-9f) continue;
        const bool walk = n.z / nl >= cosSlope;
        const bool verticalTri = std::fabs(n.z) < 1e-6f * nl;   // zero area in XY: a wall, kept on every cell it touches
        const f32 minx = std::min({a.x, b.x, c.x}), maxx = std::max({a.x, b.x, c.x});
        const f32 miny = std::min({a.y, b.y, c.y}), maxy = std::max({a.y, b.y, c.y});
        i32 x0 = static_cast<i32>(std::floor((minx - ox) / cs)), x1 = static_cast<i32>(std::floor((maxx - ox) / cs));
        i32 y0 = static_cast<i32>(std::floor((miny - oy) / cs)), y1 = static_cast<i32>(std::floor((maxy - oy) / cs));
        x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, W - 1); y1 = std::min(y1, H - 1);
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                Vec3 b0[9], b1[9];
                b0[0] = a; b0[1] = b; b0[2] = c;
                int cnt = 3;
                cnt = clipAxis(b0, cnt, b1, 0, ox + x * cs, true);
                if (cnt < 3) continue;
                cnt = clipAxis(b1, cnt, b0, 0, ox + (x + 1) * cs, false);
                if (cnt < 3) continue;
                cnt = clipAxis(b0, cnt, b1, 1, oy + y * cs, true);
                if (cnt < 3) continue;
                cnt = clipAxis(b1, cnt, b0, 1, oy + (y + 1) * cs, false);
                if (cnt < 3) continue;
                f32 zmin = 1e30f, zmax = -1e30f, area2 = 0.0f;
                for (int i = 0; i < cnt; ++i) {
                    zmin = std::min(zmin, b0[i].z); zmax = std::max(zmax, b0[i].z);
                    const Vec3& pa = b0[i];
                    const Vec3& pb = b0[(i + 1) % cnt];
                    area2 += pa.x * pb.y - pb.x * pa.y;
                }
                // A sliver along a cell edge (a triangle edge exactly on the grid line) must not claim the neighbouring column.
                if (!verticalTri && std::fabs(area2) < 1e-4f * cs * cs) continue;
                zmin = std::max(zmin, lo.z);
                if (p.useBounds && zmin > hi.z) continue;
                zmax = std::max(zmax, zmin);
                Span s;
                s.smin = static_cast<i32>(std::floor((zmin - baseZ) / ch));
                s.smax = std::max(s.smin, static_cast<i32>(std::ceil((zmax - baseZ) / ch)));
                s.walk = walk;
                s.zTop = zmax;
                addSpan(cols[static_cast<usize>(y) * W + x], s, climb);
            }
        }
    }
    for (const auto& c : cols) out.stats.spans += static_cast<u32>(c.size());

    // ---- filters
    if (p.filterLowHangingObstacles) {
        for (auto& col : cols) {
            bool prevWalk = false;
            i32 prevMax = 0;
            f32 prevZ = 0;
            for (Span& s : col) {
                if (!s.walk && prevWalk && std::abs(s.smax - prevMax) <= climb) { s.walk = true; }
                prevWalk = s.walk; prevMax = s.smax; prevZ = s.zTop;
            }
            (void)prevZ;
        }
    }
    if (p.filterLedgeSpans) {
        std::vector<std::vector<bool>> kill(cols.size());
        for (i32 y = 0; y < H; ++y) {
            for (i32 x = 0; x < W; ++x) {
                const auto& col = cols[static_cast<usize>(y) * W + x];
                kill[static_cast<usize>(y) * W + x].assign(col.size(), false);
                for (usize si = 0; si < col.size(); ++si) {
                    const Span& s = col[si];
                    if (!s.walk) continue;
                    const i32 bot = s.smax;
                    const i32 top = si + 1 < col.size() ? col[si + 1].smin : kMaxH;
                    i32 minh = kMaxH, asmin = s.smax, asmax = s.smax;
                    for (int d = 0; d < 4; ++d) {
                        const i32 nx = x + kDx[d], ny = y + kDy[d];
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) { minh = std::min(minh, -climb - bot); continue; }
                        const auto& ncol = cols[static_cast<usize>(ny) * W + nx];
                        i32 nbot = -climb;
                        i32 ntop = ncol.empty() ? kMaxH : ncol[0].smin;
                        if (std::min(top, ntop) - std::max(bot, nbot) > heightCells) minh = std::min(minh, nbot - bot);
                        for (usize k = 0; k < ncol.size(); ++k) {
                            nbot = ncol[k].smax;
                            ntop = k + 1 < ncol.size() ? ncol[k + 1].smin : kMaxH;
                            if (std::min(top, ntop) - std::max(bot, nbot) > heightCells) {
                                minh = std::min(minh, nbot - bot);
                                if (std::abs(nbot - bot) <= climb) { asmin = std::min(asmin, nbot); asmax = std::max(asmax, nbot); }
                            }
                        }
                    }
                    if (minh < -climb || asmax - asmin > climb) kill[static_cast<usize>(y) * W + x][si] = true;
                }
            }
        }
        for (usize ci = 0; ci < cols.size(); ++ci)
            for (usize si = 0; si < cols[ci].size(); ++si)
                if (kill[ci][si]) cols[ci][si].walk = false;
    }
    if (p.filterWalkableLowHeightSpans) {
        for (auto& col : cols)
            for (usize si = 0; si < col.size(); ++si) {
                const i32 top = si + 1 < col.size() ? col[si + 1].smin : kMaxH;
                if (col[si].walk && top - col[si].smax < heightCells) col[si].walk = false;
            }
    }

    // ---- compact walkable spans
    std::vector<Cs> S;
    std::vector<i32> colStart(cols.size() + 1, 0);
    for (usize ci = 0; ci < cols.size(); ++ci) {
        colStart[ci] = static_cast<i32>(S.size());
        const auto& col = cols[ci];
        for (usize si = 0; si < col.size(); ++si) {
            if (!col[si].walk) continue;
            Cs c;
            c.x = static_cast<i32>(ci % static_cast<usize>(W));
            c.y = static_cast<i32>(ci / static_cast<usize>(W));
            c.bot = col[si].smax;
            c.ceil = si + 1 < col.size() ? col[si + 1].smin : kMaxH;
            c.z = col[si].zTop;
            S.push_back(c);
        }
    }
    colStart[cols.size()] = static_cast<i32>(S.size());
    out.stats.walkableSpans = static_cast<u32>(S.size());
    if (S.empty()) return true;

    // Obstacles carve before the connections are made.
    for (Cs& c : S) {
        const f32 cx = ox + (c.x + 0.5f) * cs, cy = oy + (c.y + 0.5f) * cs;
        for (const NavBakeObstacle& o : obstacles) {
            if (c.z < o.zMin) continue;
            if (o.height > 0.0f && c.z > o.zMin + o.height) continue;
            bool hit;
            if (!o.verts.empty()) hit = pointInPoly(o.verts, cx, cy);
            else hit = (cx - o.position.x) * (cx - o.position.x) + (cy - o.position.y) * (cy - o.position.y) <= o.radius * o.radius;
            if (hit) { c.alive = false; break; }
        }
    }

    // Connections: a step of at most the climb with room for the agent in the gap.
    for (i32 i = 0; i < static_cast<i32>(S.size()); ++i) {
        Cs& c = S[static_cast<usize>(i)];
        if (!c.alive) continue;
        for (int d = 0; d < 4; ++d) {
            const i32 nx = c.x + kDx[d], ny = c.y + kDy[d];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            const usize ncol = static_cast<usize>(ny) * W + nx;
            i32 best = -1, bestDz = kMaxH;
            for (i32 j = colStart[ncol]; j < colStart[ncol + 1]; ++j) {
                const Cs& n = S[static_cast<usize>(j)];
                if (!n.alive) continue;
                const i32 gap = std::min(c.ceil, n.ceil) - std::max(c.bot, n.bot);
                const i32 dz = std::abs(n.bot - c.bot);
                if (gap >= heightCells && dz <= climb && dz < bestDz) { best = j; bestDz = dz; }
            }
            c.nb[d] = best;
        }
    }
    auto live = [&](i32 idx) { return idx >= 0 && S[static_cast<usize>(idx)].alive; };

    // ---- erode by the agent radius (chamfer distance to the nearest edge or wall, as Recast does)
    if (radiusCells > 0) {
        for (Cs& c : S) {
            bool boundary = !c.alive;
            if (!boundary) for (int d = 0; d < 4; ++d) if (!live(c.nb[d])) { boundary = true; break; }
            c.dist = boundary ? 0 : 255;
        }
        auto relax = [&](Cs& c, i32 from, i32 w) { if (live(from)) c.dist = std::min(c.dist, S[static_cast<usize>(from)].dist + w); };
        for (i32 i = 0; i < static_cast<i32>(S.size()); ++i) {   // forward: -x, -x-y, -y, +x-y
            Cs& c = S[static_cast<usize>(i)];
            if (!c.alive) continue;
            if (live(c.nb[2])) {
                relax(c, c.nb[2], 2);
                const Cs& a = S[static_cast<usize>(c.nb[2])];
                if (live(a.nb[3])) relax(c, a.nb[3], 3);
            }
            if (live(c.nb[3])) {
                relax(c, c.nb[3], 2);
                const Cs& a = S[static_cast<usize>(c.nb[3])];
                if (live(a.nb[0])) relax(c, a.nb[0], 3);
            }
        }
        for (i32 i = static_cast<i32>(S.size()) - 1; i >= 0; --i) {   // backward: +x, +x+y, +y, -x+y
            Cs& c = S[static_cast<usize>(i)];
            if (!c.alive) continue;
            if (live(c.nb[0])) {
                relax(c, c.nb[0], 2);
                const Cs& a = S[static_cast<usize>(c.nb[0])];
                if (live(a.nb[1])) relax(c, a.nb[1], 3);
            }
            if (live(c.nb[1])) {
                relax(c, c.nb[1], 2);
                const Cs& a = S[static_cast<usize>(c.nb[1])];
                if (live(a.nb[2])) relax(c, a.nb[2], 3);
            }
        }
        for (Cs& c : S) if (c.alive && c.dist < radiusCells * 2) c.alive = false;
    }

    // ---- floors: connected components, small ones removed
    const i32 minArea = static_cast<i32>(std::max(p.regionMinSize, 0.0f) * std::max(p.regionMinSize, 0.0f));
    i32 regionCount = 0;
    std::vector<i32> stack;
    for (i32 i = 0; i < static_cast<i32>(S.size()); ++i) {
        Cs& c = S[static_cast<usize>(i)];
        if (!c.alive || c.region != 0) continue;
        const i32 id = ++regionCount;
        std::vector<i32> members;
        stack.assign(1, i);
        c.region = id;
        while (!stack.empty()) {
            const i32 cur = stack.back();
            stack.pop_back();
            members.push_back(cur);
            for (int d = 0; d < 4; ++d) {
                const i32 n = S[static_cast<usize>(cur)].nb[d];
                if (!live(n) || S[static_cast<usize>(n)].region != 0) continue;
                S[static_cast<usize>(n)].region = id;
                stack.push_back(n);
            }
        }
        if (static_cast<i32>(members.size()) < minArea) {
            for (i32 m : members) S[static_cast<usize>(m)].alive = false;
            ++out.stats.regionsRemoved;
        }
    }
    out.stats.regions = static_cast<u32>(regionCount) - out.stats.regionsRemoved;

    // ---- cover each floor with rectangles that fit a plane
    const f32 tol = std::max(p.detailSampleMaxErrorCm, 0.0f);
    const i32 maxEdgeCells = p.edgeMaxLengthCm > 0.0f ? std::max(1, static_cast<i32>(std::floor(p.edgeMaxLengthCm / cs))) : kMaxH;
    std::unordered_map<u64, u32> vmap;
    auto cornerZ = [&](i32 s, int cx, int cy) {
        const Cs& c = S[static_cast<usize>(s)];
        const int dirX = cx ? 0 : 2, dirY = cy ? 1 : 3;
        f32 sum = c.z;
        int n = 1;
        const i32 nx = live(c.nb[dirX]) ? c.nb[dirX] : -1;
        const i32 ny = live(c.nb[dirY]) ? c.nb[dirY] : -1;
        if (nx >= 0) { sum += S[static_cast<usize>(nx)].z; ++n; }
        if (ny >= 0) { sum += S[static_cast<usize>(ny)].z; ++n; }
        i32 diag = -1;
        if (nx >= 0 && live(S[static_cast<usize>(nx)].nb[dirY])) diag = S[static_cast<usize>(nx)].nb[dirY];
        else if (ny >= 0 && live(S[static_cast<usize>(ny)].nb[dirX])) diag = S[static_cast<usize>(ny)].nb[dirX];
        if (diag >= 0) { sum += S[static_cast<usize>(diag)].z; ++n; }
        return sum / static_cast<f32>(n);
    };
    auto vertex = [&](i32 ix, i32 iy, f32 z) {
        const u64 key = (static_cast<u64>(static_cast<u32>(ix)) << 40) ^ (static_cast<u64>(static_cast<u32>(iy)) << 20) ^ static_cast<u64>(static_cast<u32>(static_cast<i32>(std::lround(z * 4.0f)) + 0x400000));
        auto it = vmap.find(key);
        if (it != vmap.end()) return it->second;
        const u32 idx = static_cast<u32>(out.verts.size());
        out.verts.push_back({ox + static_cast<f32>(ix) * cs, oy + static_cast<f32>(iy) * cs, z});
        vmap.emplace(key, idx);
        return idx;
    };

    for (i32 i = 0; i < static_cast<i32>(S.size()); ++i) {
        Cs& s0 = S[static_cast<usize>(i)];
        if (!s0.alive || s0.visited) continue;
        std::vector<i32> row{i};
        bool gxSet = false, gySet = false;
        f32 gx = 0, gy = 0;
        const f32 z0 = s0.z;
        while (static_cast<i32>(row.size()) < maxEdgeCells) {
            const i32 n = S[static_cast<usize>(row.back())].nb[0];
            if (!live(n) || S[static_cast<usize>(n)].visited) break;
            const f32 zn = S[static_cast<usize>(n)].z;
            if (!gxSet) { gx = zn - z0; gxSet = true; }
            else if (std::fabs(zn - (z0 + gx * static_cast<f32>(row.size()))) > tol) break;
            row.push_back(n);
        }
        std::vector<i32> rect = row;
        i32 height = 1;
        const i32 width = static_cast<i32>(row.size());
        std::vector<i32> last = row;
        while (height < maxEdgeCells) {
            std::vector<i32> next;
            bool ok = true;
            for (i32 k = 0; k < width && ok; ++k) {
                const i32 n = S[static_cast<usize>(last[static_cast<usize>(k)])].nb[1];
                if (!live(n) || S[static_cast<usize>(n)].visited) { ok = false; break; }
                if (k > 0 && S[static_cast<usize>(next.back())].nb[0] != n) { ok = false; break; }
                next.push_back(n);
            }
            if (!ok) break;
            f32 gyTry = gy;
            bool gyNew = false;
            if (!gySet) { gyTry = S[static_cast<usize>(next[0])].z - z0; gyNew = true; }
            for (i32 k = 0; k < width && ok; ++k) {
                const f32 expect = z0 + gx * static_cast<f32>(k) + gyTry * static_cast<f32>(height);
                if (std::fabs(S[static_cast<usize>(next[static_cast<usize>(k)])].z - expect) > tol) ok = false;
            }
            if (!ok) break;
            if (gyNew) { gy = gyTry; gySet = true; }
            for (i32 n : next) rect.push_back(n);
            last = next;
            ++height;
        }
        for (i32 n : rect) S[static_cast<usize>(n)].visited = true;
        const i32 x0 = s0.x, y0 = s0.y, x1 = s0.x + width, y1 = s0.y + height;
        const i32 firstRowFirst = rect.front();
        const i32 firstRowLast = rect[static_cast<usize>(width - 1)];
        const i32 lastRowFirst = rect[static_cast<usize>((height - 1) * width)];
        const i32 lastRowLast = rect.back();
        const u32 v0 = vertex(x0, y0, cornerZ(firstRowFirst, 0, 0));
        const u32 v1 = vertex(x1, y0, cornerZ(firstRowLast, 1, 0));
        const u32 v2 = vertex(x1, y1, cornerZ(lastRowLast, 1, 1));
        const u32 v3 = vertex(x0, y1, cornerZ(lastRowFirst, 0, 1));
        if (p.verticesPerPolygon >= 4) out.polys.push_back({v0, v1, v2, v3});
        else { out.polys.push_back({v0, v1, v2}); out.polys.push_back({v0, v2, v3}); }
        ++out.stats.rectangles;
    }
    return true;
}

}  // namespace aver::synapse
