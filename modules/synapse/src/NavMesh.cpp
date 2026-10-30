#include "aver/synapse/NavMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <unordered_map>

namespace aver::synapse {

// ---------------------------------------------------------------------------------------------------------------- geometry

Vec3 navClosestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const f32 l2 = dot(ab, ab);
    if (l2 <= 1e-12f) return a;
    f32 t = dot(p - a, ab) / l2;
    t = std::clamp(t, 0.0f, 1.0f);
    return a + ab * t;
}

Vec3 navClosestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const f32 d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const f32 den = d1 - d3;
        return a + ab * (std::fabs(den) < 1e-12f ? 0.0f : d1 / den);
    }
    const Vec3 cp = p - c;
    const f32 d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const f32 den = d2 - d6;
        return a + ac * (std::fabs(den) < 1e-12f ? 0.0f : d2 / den);
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const f32 den = (d4 - d3) + (d5 - d6);
        return b + (c - b) * (std::fabs(den) < 1e-12f ? 0.0f : (d4 - d3) / den);
    }
    const f32 sum = va + vb + vc;
    if (std::fabs(sum) < 1e-20f) return navClosestOnSegment(p, a, b);   // degenerate triangle
    const f32 denom = 1.0f / sum;
    return a + ab * (vb * denom) + ac * (vc * denom);
}

f32 navSegmentSegment(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3* c1, Vec3* c2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const f32 a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    f32 s = 0, t = 0;
    if (a <= 1e-12f && e <= 1e-12f) {
        s = t = 0;
    } else if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const f32 c = dot(d1, r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const f32 b = dot(d1, d2);
            const f32 denom = a * e - b * b;
            s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    const Vec3 x1 = p1 + d1 * s, x2 = p2 + d2 * t;
    if (c1) *c1 = x1;
    if (c2) *c2 = x2;
    return distSquared(x1, x2);
}

f32 navSignedAreaXY(const std::vector<Vec3>& poly) {
    f64 a = 0;
    for (usize i = 0; i < poly.size(); ++i) {
        const Vec3& p = poly[i];
        const Vec3& q = poly[(i + 1) % poly.size()];
        a += static_cast<f64>(p.x) * q.y - static_cast<f64>(q.x) * p.y;
    }
    return static_cast<f32>(a * 0.5);
}

static void rdp(const std::vector<Vec3>& pts, usize lo, usize hi, f32 eps, std::vector<bool>& keep) {
    if (hi <= lo + 1) return;
    f32 worst = -1.0f;
    usize at = lo;
    for (usize i = lo + 1; i < hi; ++i) {
        const f32 d = dist(pts[i], navClosestOnSegment(pts[i], pts[lo], pts[hi]));
        if (d > worst) { worst = d; at = i; }
    }
    if (worst > eps) {
        keep[at] = true;
        rdp(pts, lo, at, eps, keep);
        rdp(pts, at, hi, eps, keep);
    }
}

std::vector<Vec3> navSimplify(const std::vector<Vec3>& pts, f32 epsilon, std::vector<u32>* kept) {
    std::vector<Vec3> out;
    if (kept) kept->clear();
    if (pts.size() < 3 || epsilon <= 0.0f) {
        out = pts;
        if (kept) for (u32 i = 0; i < pts.size(); ++i) kept->push_back(i);
        return out;
    }
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    rdp(pts, 0, pts.size() - 1, epsilon, keep);
    for (usize i = 0; i < pts.size(); ++i) {
        if (!keep[i]) continue;
        out.push_back(pts[i]);
        if (kept) kept->push_back(static_cast<u32>(i));
    }
    return out;
}

namespace {

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

f32 triArea2(const Vec3& a, const Vec3& b, const Vec3& c) {   // positive: c is left of a->b
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool sameXY(const Vec3& a, const Vec3& b) { return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- objects

u32 NavServer::createMap() {
    const u32 id = nextId_++;
    maps_[id].id = id;
    return id;
}

bool NavServer::destroyMap(u32 id) {
    if (!maps_.erase(id)) return false;
    for (auto& [k, r] : regions_) if (r.map == id) r.map = 0;
    for (auto& [k, l] : links_) if (l.map == id) l.map = 0;
    for (auto& [k, o] : obstacles_) if (o.map == id) o.map = 0;
    for (auto& [k, a] : agents_) if (a.map == id) a.map = 0;
    return true;
}

bool NavServer::hasMap(u32 id) const { return maps_.count(id) != 0; }
void NavServer::setMapActive(u32 id, bool on) { auto it = maps_.find(id); if (it != maps_.end()) it->second.active = on; }
bool NavServer::mapActive(u32 id) const { auto it = maps_.find(id); return it != maps_.end() && it->second.active; }
NavMapParams* NavServer::mapParams(u32 id) { auto it = maps_.find(id); return it == maps_.end() ? nullptr : &it->second.params; }
const NavMapParams* NavServer::mapParams(u32 id) const { auto it = maps_.find(id); return it == maps_.end() ? nullptr : &it->second.params; }
u32 NavServer::mapIteration(u32 id) const { auto it = maps_.find(id); return it == maps_.end() ? 0 : it->second.iteration; }

std::vector<u32> NavServer::maps() const {
    std::vector<u32> v;
    for (const auto& [k, m] : maps_) v.push_back(k);
    return v;
}

void NavServer::markDirty(u32 map) { auto it = maps_.find(map); if (it != maps_.end()) it->second.dirty = true; }

u32 NavServer::createRegion(u32 map) {
    const u32 id = nextId_++;
    NavRegionData& r = regions_[id];
    r.id = id; r.map = hasMap(map) ? map : 0;
    markDirty(r.map);
    return id;
}
u32 NavServer::createLink(u32 map) {
    const u32 id = nextId_++;
    NavLinkData& l = links_[id];
    l.id = id; l.map = hasMap(map) ? map : 0;
    markDirty(l.map);
    return id;
}
u32 NavServer::createObstacle(u32 map) {
    const u32 id = nextId_++;
    NavObstacleData& o = obstacles_[id];
    o.id = id; o.map = hasMap(map) ? map : 0;
    return id;
}
u32 NavServer::createAgent(u32 map) {
    const u32 id = nextId_++;
    NavAgentData& a = agents_[id];
    a.id = id; a.map = hasMap(map) ? map : 0;
    return id;
}

bool NavServer::destroy(u32 id) {
    if (auto it = regions_.find(id); it != regions_.end()) { markDirty(it->second.map); regions_.erase(it); return true; }
    if (auto it = links_.find(id); it != links_.end()) { markDirty(it->second.map); links_.erase(it); return true; }
    return obstacles_.erase(id) != 0 || agents_.erase(id) != 0;
}

NavRegionData* NavServer::region(u32 id) { auto it = regions_.find(id); return it == regions_.end() ? nullptr : &it->second; }
NavLinkData* NavServer::link(u32 id) { auto it = links_.find(id); return it == links_.end() ? nullptr : &it->second; }
NavObstacleData* NavServer::obstacle(u32 id) { auto it = obstacles_.find(id); return it == obstacles_.end() ? nullptr : &it->second; }
NavAgentData* NavServer::agent(u32 id) { auto it = agents_.find(id); return it == agents_.end() ? nullptr : &it->second; }
const NavRegionData* NavServer::region(u32 id) const { auto it = regions_.find(id); return it == regions_.end() ? nullptr : &it->second; }
const NavLinkData* NavServer::link(u32 id) const { auto it = links_.find(id); return it == links_.end() ? nullptr : &it->second; }

std::vector<u32> NavServer::regionsOf(u32 map) const {
    std::vector<u32> v;
    for (const auto& [k, r] : regions_) if (r.map == map) v.push_back(k);
    return v;
}
std::vector<u32> NavServer::linksOf(u32 map) const {
    std::vector<u32> v;
    for (const auto& [k, r] : links_) if (r.map == map) v.push_back(k);
    return v;
}
std::vector<u32> NavServer::agentsOf(u32 map) const {
    std::vector<u32> v;
    for (const auto& [k, r] : agents_) if (r.map == map) v.push_back(k);
    return v;
}
std::vector<u32> NavServer::obstaclesOf(u32 map) const {
    std::vector<u32> v;
    for (const auto& [k, r] : obstacles_) if (r.map == map) v.push_back(k);
    return v;
}

void NavServer::setRegionMap(u32 region, u32 map) {
    if (NavRegionData* r = this->region(region)) { markDirty(r->map); r->map = hasMap(map) ? map : 0; markDirty(r->map); }
}
void NavServer::setLinkMap(u32 link, u32 map) {
    if (NavLinkData* l = this->link(link)) { markDirty(l->map); l->map = hasMap(map) ? map : 0; markDirty(l->map); }
}
void NavServer::setObstacleMap(u32 obstacle, u32 map) {
    if (NavObstacleData* o = this->obstacle(obstacle)) o->map = hasMap(map) ? map : 0;
}
void NavServer::setAgentMap(u32 agent, u32 map) {
    if (NavAgentData* a = this->agent(agent)) a->map = hasMap(map) ? map : 0;
}

bool NavServer::setRegionMesh(u32 region, std::vector<Vec3> verts, std::vector<std::vector<u32>> polys) {
    NavRegionData* r = this->region(region);
    if (!r) return false;
    for (const auto& p : polys)
        for (u32 i : p)
            if (i >= verts.size()) return false;
    r->verts = std::move(verts);
    r->polys = std::move(polys);
    markDirty(r->map);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- rebuild

namespace {

struct EdgeRef {
    u32 poly;
    Vec3 a, b;
};

struct CellKey {
    i32 x, y;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y; }
};
struct CellHash {
    usize operator()(const CellKey& k) const { return (static_cast<usize>(static_cast<u32>(k.x)) * 73856093u) ^ (static_cast<usize>(static_cast<u32>(k.y)) * 19349663u); }
};

}  // namespace

bool NavServer::update(u32 map) {
    auto it = maps_.find(map);
    if (it == maps_.end() || !it->second.dirty) return false;
    rebuild(it->second);
    return true;
}

void NavServer::rebuild(Map& m) {
    m.dirty = false;
    ++m.iteration;
    m.polys.clear();
    m.linkAttached.clear();
    m.minTravel = 1.0f;
    bool anyCost = false;

    for (const auto& [rid, r] : regions_) {
        if (r.map != m.id || !r.enabled) continue;
        for (usize pi = 0; pi < r.polys.size(); ++pi) {
            const auto& idx = r.polys[pi];
            if (idx.size() < 3) continue;
            Poly p;
            p.region = rid;
            p.local = static_cast<u32>(pi);
            for (u32 i : idx) p.pts.push_back(r.verts[i]);
            const f32 area = navSignedAreaXY(p.pts);
            if (std::fabs(area) < 1e-3f) continue;   // vertical or degenerate: nothing to stand on
            if (area < 0) std::reverse(p.pts.begin(), p.pts.end());
            p.lo = p.hi = p.pts[0];
            Vec3 sum;
            for (const Vec3& v : p.pts) {
                sum += v;
                p.lo = {std::min(p.lo.x, v.x), std::min(p.lo.y, v.y), std::min(p.lo.z, v.z)};
                p.hi = {std::max(p.hi.x, v.x), std::max(p.hi.y, v.y), std::max(p.hi.z, v.z)};
            }
            p.centre = sum / static_cast<f32>(p.pts.size());
            p.layers = r.layers;
            p.enterCost = r.enterCost;
            p.travelCost = r.travelCost;
            if (!anyCost || r.travelCost < m.minTravel) m.minTravel = r.travelCost;
            anyCost = true;
            m.polys.push_back(std::move(p));
        }
    }
    if (m.minTravel < 0.0f) m.minTravel = 0.0f;

    const f32 margin = std::max(m.params.edgeConnectionMarginCm, 0.01f);

    if (m.params.useEdgeConnections && m.polys.size() > 1) {
        std::vector<EdgeRef> edges;
        for (u32 pi = 0; pi < m.polys.size(); ++pi) {
            const auto& pts = m.polys[pi].pts;
            for (usize k = 0; k < pts.size(); ++k) edges.push_back({pi, pts[k], pts[(k + 1) % pts.size()]});
        }
        const f32 cell = std::max(200.0f, margin * 4.0f);
        std::unordered_map<CellKey, std::vector<u32>, CellHash> grid;
        for (u32 ei = 0; ei < edges.size(); ++ei) {
            const EdgeRef& e = edges[ei];
            const i32 x0 = static_cast<i32>(std::floor((std::min(e.a.x, e.b.x) - margin) / cell));
            const i32 x1 = static_cast<i32>(std::floor((std::max(e.a.x, e.b.x) + margin) / cell));
            const i32 y0 = static_cast<i32>(std::floor((std::min(e.a.y, e.b.y) - margin) / cell));
            const i32 y1 = static_cast<i32>(std::floor((std::max(e.a.y, e.b.y) + margin) / cell));
            for (i32 y = y0; y <= y1; ++y)
                for (i32 x = x0; x <= x1; ++x) grid[{x, y}].push_back(ei);
        }
        std::set<std::pair<u32, u32>> done;
        for (auto& [key, list] : grid) {
            for (usize i = 0; i < list.size(); ++i) {
                for (usize j = i + 1; j < list.size(); ++j) {
                    u32 ea = list[i], eb = list[j];
                    if (ea > eb) std::swap(ea, eb);
                    const EdgeRef& e = edges[ea];
                    const EdgeRef& f = edges[eb];
                    if (e.poly == f.poly) continue;
                    if (m.polys[e.poly].region != m.polys[f.poly].region &&
                        (!regions_.at(m.polys[e.poly].region).useEdgeConnections || !regions_.at(m.polys[f.poly].region).useEdgeConnections)) continue;
                    if (!done.insert({ea, eb}).second) continue;
                    const Vec3 d1 = e.b - e.a, d2 = f.b - f.a;
                    const f32 l1 = d1.size(), l2 = d2.size();
                    if (l1 < 1e-3f || l2 < 1e-3f) continue;
                    const Vec3 u = d1 / l1;
                    if (dot(u, d2 / l2) > -0.5f) continue;               // not facing each other
                    const f32 ta = dot(f.a - e.a, u), tb = dot(f.b - e.a, u);   // along e; tb < ta for opposite edges
                    const f32 lo = std::max(0.0f, std::min(ta, tb)), hi = std::min(l1, std::max(ta, tb));
                    if (hi - lo < 2.0f) continue;                        // touching at a corner is not a doorway
                    auto onF = [&](f32 t) {                              // the point of f that projects to t on e
                        const f32 s = (ta - tb) == 0 ? 0.0f : (ta - t) / (ta - tb);
                        return f.a + (f.b - f.a) * s;
                    };
                    const Vec3 e0 = e.a + u * lo, e1 = e.a + u * hi;
                    const Vec3 f0 = onF(lo), f1 = onF(hi);
                    if (dist(e0, f0) > margin || dist(e1, f1) > margin) continue;
                    const Vec3 p0 = (e0 + f0) * 0.5f, p1 = (e1 + f1) * 0.5f;   // p0 toward e.a, p1 toward e.b
                    Conn c1;                                              // e.poly -> f.poly: right = a side, left = b side
                    c1.to = f.poly; c1.a = p0; c1.b = p1;
                    Conn c2;                                              // f's direction is reversed
                    c2.to = e.poly; c2.a = p1; c2.b = p0;
                    m.polys[e.poly].conns.push_back(c1);
                    m.polys[f.poly].conns.push_back(c2);
                }
            }
        }
    }

    // Links attach to the nearest polygon within the connection radius.
    const f32 linkR = std::max(m.params.linkConnectionRadiusCm, 0.0f);
    auto nearestPoly = [&](const Vec3& p, f32& outD) -> i32 {
        i32 best = -1;
        f32 bd = kInf;
        for (u32 i = 0; i < m.polys.size(); ++i) {
            const Poly& poly = m.polys[i];
            // AABB reject
            const f32 dx = std::max({poly.lo.x - p.x, 0.0f, p.x - poly.hi.x});
            const f32 dy = std::max({poly.lo.y - p.y, 0.0f, p.y - poly.hi.y});
            const f32 dz = std::max({poly.lo.z - p.z, 0.0f, p.z - poly.hi.z});
            if (dx * dx + dy * dy + dz * dz >= bd * bd) continue;
            for (usize k = 1; k + 1 < poly.pts.size(); ++k) {
                const f32 d = dist(p, navClosestOnTriangle(p, poly.pts[0], poly.pts[k], poly.pts[k + 1]));
                if (d < bd) { bd = d; best = static_cast<i32>(i); }
            }
        }
        outD = bd;
        return best;
    };
    for (const auto& [lid, l] : links_) {
        if (l.map != m.id || !l.enabled) continue;
        f32 d0 = 0, d1 = 0;
        const i32 s = nearestPoly(l.start, d0), e = nearestPoly(l.end, d1);
        u32 attached = 0;
        if (s >= 0 && d0 <= linkR) ++attached;
        if (e >= 0 && d1 <= linkR) ++attached;
        m.linkAttached[lid] = attached;
        if (attached < 2 || s == e) continue;
        Conn fwd;
        fwd.to = static_cast<u32>(e); fwd.link = lid; fwd.linkStart = l.start; fwd.linkEnd = l.end;
        m.polys[static_cast<u32>(s)].conns.push_back(fwd);
        if (l.bidirectional) {
            Conn back;
            back.to = static_cast<u32>(s); back.link = lid; back.linkStart = l.end; back.linkEnd = l.start;
            m.polys[static_cast<u32>(e)].conns.push_back(back);
        }
        if (l.travelCost >= 0.0f && l.travelCost < m.minTravel) m.minTravel = l.travelCost;
    }
}

u32 NavServer::polygonCount(u32 map) {
    update(map);
    auto it = maps_.find(map);
    return it == maps_.end() ? 0u : static_cast<u32>(it->second.polys.size());
}

u32 NavServer::linkConnectedPolys(u32 link) {
    const NavLinkData* l = this->link(link);
    if (!l) return 0;
    update(l->map);
    auto it = maps_.find(l->map);
    if (it == maps_.end()) return 0;
    auto f = it->second.linkAttached.find(link);
    return f == it->second.linkAttached.end() ? 0u : f->second;
}

std::vector<std::pair<Vec3, Vec3>> NavServer::regionConnections(u32 regionId) {
    std::vector<std::pair<Vec3, Vec3>> out;
    const NavRegionData* r = region(regionId);
    if (!r) return out;
    update(r->map);
    auto it = maps_.find(r->map);
    if (it == maps_.end()) return out;
    for (const Poly& p : it->second.polys) {
        if (p.region != regionId) continue;
        for (const Conn& c : p.conns) {
            if (c.link) continue;
            if (it->second.polys[c.to].region == regionId) continue;
            out.emplace_back(c.a, c.b);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- queries

namespace {

struct PolyHit {
    Vec3 point;
    f32 dist = kInf;
};

}  // namespace

bool NavServer::eligible(const Poly& p, const NavPathQuery& q) const {
    if ((p.layers & q.layers) == 0) return false;
    if (!q.includedRegions.empty() && std::find(q.includedRegions.begin(), q.includedRegions.end(), p.region) == q.includedRegions.end()) return false;
    if (!q.excludedRegions.empty() && std::find(q.excludedRegions.begin(), q.excludedRegions.end(), p.region) != q.excludedRegions.end()) return false;
    return true;
}

namespace {

Vec3 closestOnPoly(const std::vector<Vec3>& pts, const Vec3& p) {
    Vec3 best = pts[0];
    f32 bd = kInf;
    for (usize k = 1; k + 1 < pts.size(); ++k) {
        const Vec3 c = navClosestOnTriangle(p, pts[0], pts[k], pts[k + 1]);
        const f32 d = distSquared(c, p);
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

struct Corner {
    Vec3 p;
    u32 poly;
};

// The simple stupid funnel over portals (left, right as seen walking forward), in the XY plane; heights ride along.
void funnel(const Vec3& start, const Vec3& end, u32 startPoly, const std::vector<Vec3>& left, const std::vector<Vec3>& right,
            const std::vector<u32>& polyOf, u32 endPoly, std::vector<Corner>& out) {
    std::vector<Vec3> L, R;
    std::vector<u32> P;
    L.push_back(start); R.push_back(start); P.push_back(startPoly);
    for (usize i = 0; i < left.size(); ++i) { L.push_back(left[i]); R.push_back(right[i]); P.push_back(polyOf[i]); }
    L.push_back(end); R.push_back(end); P.push_back(endPoly);

    Vec3 apex = start, pl = L[0], pr = R[0];
    usize apexIdx = 0, li = 0, ri = 0;
    out.push_back({start, startPoly});
    const usize n = L.size();
    usize guard = 0;
    for (usize i = 1; i < n; ++i) {
        if (++guard > n * n + 16) break;
        const Vec3 nl = L[i], nr = R[i];
        // tighten the right side
        if (triArea2(apex, pr, nr) >= 0.0f) {
            if (sameXY(apex, pr) || triArea2(apex, pl, nr) < 0.0f) {
                pr = nr; ri = i;
            } else {
                out.push_back({pl, P[li]});
                apex = pl; apexIdx = li;
                pl = pr = apex; li = ri = apexIdx;
                i = apexIdx;
                continue;
            }
        }
        // tighten the left side
        if (triArea2(apex, pl, nl) <= 0.0f) {
            if (sameXY(apex, pl) || triArea2(apex, pr, nl) > 0.0f) {
                pl = nl; li = i;
            } else {
                out.push_back({pr, P[ri]});
                apex = pr; apexIdx = ri;
                pl = pr = apex; li = ri = apexIdx;
                i = apexIdx;
                continue;
            }
        }
    }
    out.push_back({end, endPoly});
}

}  // namespace

NavPathResult NavServer::queryPath(const NavPathQuery& q) {
    NavPathResult res;
    auto mit = maps_.find(q.map);
    if (mit == maps_.end()) return res;
    Map& m = mit->second;
    if (m.dirty) rebuild(m);
    if (m.polys.empty()) return res;

    auto snap = [&](const Vec3& p, u32& outPoly, Vec3& outPt) {
        f32 bd = kInf;
        outPoly = ~0u;
        for (u32 i = 0; i < m.polys.size(); ++i) {
            const Poly& poly = m.polys[i];
            if (!eligible(poly, q)) continue;
            const f32 dx = std::max({poly.lo.x - p.x, 0.0f, p.x - poly.hi.x});
            const f32 dy = std::max({poly.lo.y - p.y, 0.0f, p.y - poly.hi.y});
            const f32 dz = std::max({poly.lo.z - p.z, 0.0f, p.z - poly.hi.z});
            if (dx * dx + dy * dy + dz * dz > bd) continue;
            const Vec3 c = closestOnPoly(poly.pts, p);
            const f32 d = distSquared(c, p);
            if (d < bd) { bd = d; outPoly = i; outPt = c; }
        }
    };
    u32 sPoly = ~0u, ePoly = ~0u;
    Vec3 sPt, ePt;
    snap(q.start, sPoly, sPt);
    snap(q.target, ePoly, ePt);
    if (sPoly == ~0u || ePoly == ~0u) return res;

    struct Node {
        f32 g = kInf;
        Vec3 entry;
        i32 parent = -1;
        i32 via = -1;       // index in the parent's conns
        bool closed = false;
    };
    std::vector<Node> nodes(m.polys.size());
    using Item = std::pair<f32, u32>;   // (f, poly)
    auto cmp = [](const Item& a, const Item& b) { return a.first > b.first || (a.first == b.first && a.second > b.second); };
    std::priority_queue<Item, std::vector<Item>, decltype(cmp)> open(cmp);
    nodes[sPoly].g = 0;
    nodes[sPoly].entry = sPt;
    open.push({dist(sPt, ePt) * m.minTravel, sPoly});

    u32 reached = ~0u;
    u32 closestNode = sPoly;
    f32 closestD = kInf, closestG = kInf;
    Vec3 closestPt = sPt;
    while (!open.empty()) {
        const u32 cur = open.top().second;
        open.pop();
        if (nodes[cur].closed) continue;
        nodes[cur].closed = true;
        ++res.expansions;
        const Poly& cp = m.polys[cur];
        // track the best partial end
        {
            const Vec3 c = closestOnPoly(cp.pts, q.target);
            const f32 d = dist(c, q.target);
            if (d < closestD - 1e-3f || (std::fabs(d - closestD) <= 1e-3f && nodes[cur].g < closestG)) {
                closestD = d; closestG = nodes[cur].g; closestNode = cur; closestPt = c;
            }
        }
        if (cur == ePoly) { reached = cur; break; }
        for (u32 ci = 0; ci < cp.conns.size(); ++ci) {
            const Conn& c = cp.conns[ci];
            const Poly& to = m.polys[c.to];
            if (nodes[c.to].closed || !eligible(to, q)) continue;
            Vec3 newEntry;
            f32 cost;
            if (c.link) {
                const NavLinkData& L = links_.at(c.link);
                if ((L.layers & q.layers) == 0) continue;
                newEntry = c.linkEnd;
                cost = dist(nodes[cur].entry, c.linkStart) * cp.travelCost + L.enterCost + dist(c.linkStart, c.linkEnd) * L.travelCost;
            } else {
                newEntry = navClosestOnSegment(nodes[cur].entry, c.a, c.b);
                cost = dist(nodes[cur].entry, newEntry) * cp.travelCost;
            }
            if (to.region != cp.region) cost += to.enterCost;
            const f32 g = nodes[cur].g + cost;
            if (g < nodes[c.to].g) {
                nodes[c.to].g = g;
                nodes[c.to].entry = newEntry;
                nodes[c.to].parent = static_cast<i32>(cur);
                nodes[c.to].via = static_cast<i32>(ci);
                open.push({g + dist(newEntry, ePt) * m.minTravel, c.to});
            }
        }
    }

    u32 last = reached;
    Vec3 endPoint = ePt;
    if (last == ~0u) {
        last = closestNode;
        endPoint = closestPt;
    }
    res.reachedTarget = reached != ~0u;
    res.endGapCm = dist(endPoint, q.target);

    // The corridor: polys from start to `last`, with the connection used to leave each.
    std::vector<u32> chain;
    std::vector<i32> leave;   // conn index leaving chain[i] (the last has -1)
    {
        std::vector<u32> rev;
        for (i32 n = static_cast<i32>(last); n >= 0; n = nodes[n].parent) rev.push_back(static_cast<u32>(n));
        std::reverse(rev.begin(), rev.end());
        chain = rev;
        for (usize i = 0; i + 1 < chain.size(); ++i) leave.push_back(nodes[chain[i + 1]].via);
        leave.push_back(-1);
    }

    auto regionOf = [&](u32 poly) { return m.polys[poly].region; };
    auto addPoint = [&](const Vec3& p, i32 type, u32 rid) {
        res.points.push_back(p);
        res.types.push_back(type);
        res.rids.push_back(rid);
        u64 owner = 0;
        if (type == kNavPointLink) { if (auto* l = link(rid)) owner = l->owner; }
        else if (auto* r = region(rid)) owner = r->owner;
        res.owners.push_back(owner);
    };

    if (q.postprocess == kNavPostNone) {
        addPoint(sPt, kNavPointRegion, regionOf(chain[0]));
        for (usize i = 0; i + 1 < chain.size(); ++i) {
            const Conn& c = m.polys[chain[i]].conns[static_cast<usize>(leave[i])];
            if (c.link) {
                addPoint(c.linkStart, kNavPointLink, c.link);
                addPoint(c.linkEnd, kNavPointLink, c.link);
            } else {
                addPoint(nodes[chain[i + 1]].entry, kNavPointRegion, regionOf(chain[i]));
            }
        }
        addPoint(endPoint, kNavPointRegion, regionOf(chain.back()));
    } else if (q.postprocess == kNavPostEdgeCentered) {
        addPoint(sPt, kNavPointRegion, regionOf(chain[0]));
        for (usize i = 0; i + 1 < chain.size(); ++i) {
            const Conn& c = m.polys[chain[i]].conns[static_cast<usize>(leave[i])];
            if (c.link) {
                addPoint(c.linkStart, kNavPointLink, c.link);
                addPoint(c.linkEnd, kNavPointLink, c.link);
            } else {
                addPoint((c.a + c.b) * 0.5f, kNavPointRegion, regionOf(chain[i]));
            }
        }
        addPoint(endPoint, kNavPointRegion, regionOf(chain.back()));
    } else {
        // Corridor funnel, one stretch at a time between link crossings.
        Vec3 segStart = sPt;
        u32 segPoly = chain[0];
        std::vector<Vec3> left, right;
        std::vector<u32> polyOf;
        bool first = true;
        auto flush = [&](const Vec3& segEnd, u32 endPolyIdx) {
            std::vector<Corner> corners;
            funnel(segStart, segEnd, segPoly, left, right, polyOf, endPolyIdx, corners);
            corners.pop_back();   // the stretch ends at a link; the link's own point follows
            for (usize k = first ? 0 : 1; k < corners.size(); ++k) {
                // a corner repeated at the same place adds nothing
                if (!res.points.empty() && distSquared(res.points.back(), corners[k].p) < 1e-6f) continue;
                addPoint(corners[k].p, kNavPointRegion, regionOf(corners[k].poly));
            }
            first = false;
            left.clear(); right.clear(); polyOf.clear();
        };
        for (usize i = 0; i + 1 < chain.size(); ++i) {
            const Conn& c = m.polys[chain[i]].conns[static_cast<usize>(leave[i])];
            if (c.link) {
                flush(c.linkStart, chain[i]);
                addPoint(c.linkStart, kNavPointLink, c.link);
                addPoint(c.linkEnd, kNavPointLink, c.link);
                segStart = c.linkEnd;
                segPoly = chain[i + 1];
                first = false;
            } else {
                left.push_back(c.b);
                right.push_back(c.a);
                polyOf.push_back(chain[i]);
            }
        }
        // the final stretch (its first corner repeats segStart, already in the list unless this is the very first)
        {
            std::vector<Corner> corners;
            funnel(segStart, endPoint, segPoly, left, right, polyOf, chain.back(), corners);
            const bool prior = !res.points.empty();
            for (usize k = prior ? 1 : 0; k < corners.size(); ++k) {
                if (!res.points.empty() && distSquared(res.points.back(), corners[k].p) < 1e-6f) continue;
                addPoint(corners[k].p, kNavPointRegion, regionOf(corners[k].poly));
            }
        }
        if (res.points.empty()) addPoint(sPt, kNavPointRegion, regionOf(chain[0]));
    }

    if (q.simplify && q.simplifyEpsilonCm > 0.0f && res.points.size() > 2) {
        // simplify between link points, which are always kept
        std::vector<u32> keepIdx;
        std::vector<usize> cuts{0};
        for (usize i = 1; i + 1 < res.points.size(); ++i) if (res.types[i] == kNavPointLink) cuts.push_back(i);
        cuts.push_back(res.points.size() - 1);
        for (usize ci = 0; ci + 1 < cuts.size(); ++ci) {
            std::vector<Vec3> chunk(res.points.begin() + static_cast<isize>(cuts[ci]), res.points.begin() + static_cast<isize>(cuts[ci + 1]) + 1);
            std::vector<u32> kept;
            navSimplify(chunk, q.simplifyEpsilonCm, &kept);
            for (u32 k : kept) {
                const u32 gi = static_cast<u32>(cuts[ci]) + k;
                if (keepIdx.empty() || keepIdx.back() != gi) keepIdx.push_back(gi);
            }
        }
        NavPathResult s;
        s.reachedTarget = res.reachedTarget;
        s.expansions = res.expansions;
        for (u32 gi : keepIdx) {
            s.points.push_back(res.points[gi]);
            s.types.push_back(res.types[gi]);
            s.rids.push_back(res.rids[gi]);
            s.owners.push_back(res.owners[gi]);
        }
        res = std::move(s);
    }
    res.lengthCm = 0;
    for (usize i = 1; i < res.points.size(); ++i) res.lengthCm += dist(res.points[i - 1], res.points[i]);
    return res;
}

NavClosest NavServer::closestPoint(u32 map, const Vec3& p, u32 layers) {
    NavClosest out;
    auto mit = maps_.find(map);
    if (mit == maps_.end()) return out;
    Map& m = mit->second;
    if (m.dirty) rebuild(m);
    f32 bd = kInf;
    for (const Poly& poly : m.polys) {
        if ((poly.layers & layers) == 0) continue;
        for (usize k = 1; k + 1 < poly.pts.size(); ++k) {
            const Vec3 c = navClosestOnTriangle(p, poly.pts[0], poly.pts[k], poly.pts[k + 1]);
            const f32 d = distSquared(c, p);
            if (d < bd) {
                bd = d;
                out.point = c;
                Vec3 n = cross(poly.pts[k] - poly.pts[0], poly.pts[k + 1] - poly.pts[0]).getSafeNormal();
                if (n.z < 0) n = -n;
                out.normal = n;
                out.region = poly.region;
                out.found = true;
            }
        }
    }
    if (out.found) if (auto* r = region(out.region)) out.owner = r->owner;
    return out;
}

NavClosest NavServer::closestPointOnRegion(u32 regionId, const Vec3& p) {
    NavClosest out;
    const NavRegionData* r = region(regionId);
    if (!r) return out;
    update(r->map);
    auto mit = maps_.find(r->map);
    if (mit == maps_.end()) return out;
    f32 bd = kInf;
    for (const Poly& poly : mit->second.polys) {
        if (poly.region != regionId) continue;
        for (usize k = 1; k + 1 < poly.pts.size(); ++k) {
            const Vec3 c = navClosestOnTriangle(p, poly.pts[0], poly.pts[k], poly.pts[k + 1]);
            const f32 d = distSquared(c, p);
            if (d < bd) {
                bd = d; out.point = c; out.found = true; out.region = regionId; out.owner = r->owner;
                Vec3 n = cross(poly.pts[k] - poly.pts[0], poly.pts[k + 1] - poly.pts[0]).getSafeNormal();
                out.normal = n.z < 0 ? -n : n;
            }
        }
    }
    return out;
}

Vec3 NavServer::closestPointToSegment(u32 map, const Vec3& a, const Vec3& b, Vec3* onSegment) {
    Vec3 bestPoly = a, bestSeg = a;
    auto mit = maps_.find(map);
    if (mit == maps_.end()) return bestPoly;
    Map& m = mit->second;
    if (m.dirty) rebuild(m);
    f32 bd = kInf;
    for (const Poly& poly : m.polys) {
        for (usize k = 1; k + 1 < poly.pts.size(); ++k) {
            const Vec3 &t0 = poly.pts[0], &t1 = poly.pts[k], &t2 = poly.pts[k + 1];
            auto consider = [&](const Vec3& onTri, const Vec3& onSeg) {
                const f32 d = distSquared(onTri, onSeg);
                if (d < bd) { bd = d; bestPoly = onTri; bestSeg = onSeg; }
            };
            consider(navClosestOnTriangle(a, t0, t1, t2), a);
            consider(navClosestOnTriangle(b, t0, t1, t2), b);
            const Vec3 tri[3] = {t0, t1, t2};
            for (int e = 0; e < 3; ++e) {
                Vec3 c1, c2;
                navSegmentSegment(a, b, tri[e], tri[(e + 1) % 3], &c1, &c2);
                consider(c2, c1);
            }
            // the segment through the triangle
            const Vec3 n = cross(t1 - t0, t2 - t0);
            const f32 da = dot(n, a - t0), db = dot(n, b - t0);
            if (da * db < 0) {
                const Vec3 x = a + (b - a) * (da / (da - db));
                const Vec3 c = navClosestOnTriangle(x, t0, t1, t2);
                if (distSquared(c, x) < 1e-6f) consider(c, x);
            }
        }
    }
    if (onSegment) *onSegment = bestSeg;
    return bestPoly;
}

Vec3 NavServer::randomPoint(u32 map, u32 layers, bool uniformly, f32 r1, f32 r2, f32 r3, bool* found) {
    if (found) *found = false;
    auto mit = maps_.find(map);
    if (mit == maps_.end()) return {};
    Map& m = mit->second;
    if (m.dirty) rebuild(m);
    std::vector<u32> ok;
    for (u32 i = 0; i < m.polys.size(); ++i) if (m.polys[i].layers & layers) ok.push_back(i);
    if (ok.empty()) return {};
    // triangle table over the eligible polygons
    struct Tri { u32 poly; usize k; f32 area; };
    std::vector<Tri> tris;
    f32 total = 0;
    for (u32 pi : ok) {
        const Poly& p = m.polys[pi];
        for (usize k = 1; k + 1 < p.pts.size(); ++k) {
            const f32 a = cross(p.pts[k] - p.pts[0], p.pts[k + 1] - p.pts[0]).size() * 0.5f;
            tris.push_back({pi, k, a});
            total += a;
        }
    }
    if (tris.empty()) return {};
    usize pick = 0;
    if (uniformly) {
        f32 x = std::clamp(r1, 0.0f, 0.99999994f) * total, acc = 0;
        for (usize i = 0; i < tris.size(); ++i) { acc += tris[i].area; if (x < acc) { pick = i; break; } pick = i; }
    } else {
        // one polygon, then one of its triangles: the non-uniform mode weights every polygon the same
        const u32 pi = ok[std::min<usize>(static_cast<usize>(std::clamp(r1, 0.0f, 0.99999994f) * ok.size()), ok.size() - 1)];
        std::vector<usize> mine;
        for (usize i = 0; i < tris.size(); ++i) if (tris[i].poly == pi) mine.push_back(i);
        pick = mine.empty() ? 0 : mine[std::min<usize>(static_cast<usize>(std::clamp(r2, 0.0f, 0.99999994f) * mine.size()), mine.size() - 1)];
    }
    const Poly& p = m.polys[tris[pick].poly];
    const Vec3 &a = p.pts[0], &b = p.pts[tris[pick].k], &c = p.pts[tris[pick].k + 1];
    f32 u = std::clamp(r2, 0.0f, 1.0f), v = std::clamp(r3, 0.0f, 1.0f);
    if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
    if (found) *found = true;
    return a + (b - a) * u + (c - a) * v;
}

}  // namespace aver::synapse
