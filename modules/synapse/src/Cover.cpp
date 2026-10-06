#include "aver/synapse/Cover.hpp"

#include "aver/core/Math.hpp"
#include "aver/synapse/CrowdOrca.hpp"   // navCellBlocked

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace aver::synapse {
namespace {

i64 bucketKey(i32 bx, i32 by) {
    return (static_cast<i64>(bx) << 32) | static_cast<i64>(static_cast<u32>(by));
}

} // namespace

bool navSegmentBlocked(const fmt::OcNavData& nav, V2 a, V2 b) {
    const f32 cs = nav.cellSizeCm;
    if (cs <= 0.0f) return false;
    const f32 ax = (a.x - nav.originXCm) / cs, ay = (a.y - nav.originYCm) / cs;
    const f32 bx = (b.x - nav.originXCm) / cs, by = (b.y - nav.originYCm) / cs;
    i32 cx = static_cast<i32>(std::floor(ax)), cy = static_cast<i32>(std::floor(ay));
    const i32 sx0 = cx, sy0 = cy;
    const i32 ex = static_cast<i32>(std::floor(bx)), ey = static_cast<i32>(std::floor(by));

    const f32 dx = bx - ax, dy = by - ay;
    const f32 inf = std::numeric_limits<f32>::infinity();
    const i32 stepX = dx > 0.0f ? 1 : -1, stepY = dy > 0.0f ? 1 : -1;
    f32 tMaxX = dx != 0.0f ? (static_cast<f32>(dx > 0.0f ? cx + 1 : cx) - ax) / dx : inf;
    f32 tMaxY = dy != 0.0f ? (static_cast<f32>(dy > 0.0f ? cy + 1 : cy) - ay) / dy : inf;
    const f32 tDx = dx != 0.0f ? std::fabs(1.0f / dx) : inf;
    const f32 tDy = dy != 0.0f ? std::fabs(1.0f / dy) : inf;

    const i32 maxSteps = std::abs(ex - cx) + std::abs(ey - cy) + 2;
    for (i32 s = 0; s <= maxSteps; ++s) {
        const bool endpoint = (cx == sx0 && cy == sy0) || (cx == ex && cy == ey);
        if (!endpoint && navCellBlocked(nav, cx, cy)) return true;
        if (cx == ex && cy == ey) break;
        if (tMaxX < tMaxY) { tMaxX += tDx; cx += stepX; }
        else               { tMaxY += tDy; cy += stepY; }
    }
    return false;
}

// ---- reservations ---------------------------------------------------------------------------------

bool CoverReservations::reserve(u32 coverId, u32 owner, f32 ttlSec) {
    const auto it = byCover_.find(coverId);
    if (it != byCover_.end() && it->second.owner != owner) return false;
    for (auto i = byCover_.begin(); i != byCover_.end();) {
        if (i->second.owner == owner && i->first != coverId) i = byCover_.erase(i);
        else ++i;
    }
    byCover_[coverId] = Entry{owner, ttlSec, ttlSec > 0.0f};
    return true;
}

void CoverReservations::release(u32 coverId, u32 owner) {
    const auto it = byCover_.find(coverId);
    if (it != byCover_.end() && it->second.owner == owner) byCover_.erase(it);
}

void CoverReservations::releaseAll(u32 owner) {
    for (auto i = byCover_.begin(); i != byCover_.end();) {
        if (i->second.owner == owner) i = byCover_.erase(i);
        else ++i;
    }
}

u32 CoverReservations::ownerOf(u32 coverId) const {
    const auto it = byCover_.find(coverId);
    return it == byCover_.end() ? 0 : it->second.owner;
}

bool CoverReservations::reservedByOther(u32 coverId, u32 owner) const {
    const auto it = byCover_.find(coverId);
    return it != byCover_.end() && it->second.owner != owner;
}

u32 CoverReservations::coverOf(u32 owner) const {
    for (const auto& kv : byCover_)
        if (kv.second.owner == owner) return kv.first;
    return 0;
}

void CoverReservations::tick(f32 dt) {
    for (auto i = byCover_.begin(); i != byCover_.end();) {
        if (i->second.timed) {
            i->second.ttlSec -= dt;
            if (i->second.ttlSec <= 0.0f) { i = byCover_.erase(i); continue; }
        }
        ++i;
    }
}

void CoverReservations::owners(std::vector<u32>& out) const {
    out.clear();
    for (const auto& kv : byCover_) out.push_back(kv.second.owner);
}

// ---- cover map ------------------------------------------------------------------------------------

u32 CoverMap::addAuthored(V2 pos, V2 dir, CoverHeight h, f32 arcHalfAngleDeg, u32 owner) {
    CoverPoint c;
    c.id = nextId_++;
    c.pos = pos;
    c.dir = norm2(dir, V2{1.0f, 0.0f});
    c.height = h;
    c.arcHalfAngleDeg = arcHalfAngleDeg;
    c.authored = true;
    c.owner = owner;
    points_.push_back(c);
    return c.id;
}

u32 CoverMap::generateFromNav(const fmt::OcNavData& nav, const CoverGenParams& p) {
    clearGenerated();
    if (!nav.valid() || nav.cellSizeCm <= 0.0f) return 0;

    static const i32 kOx[4] = {1, -1, 0, 0}, kOy[4] = {0, 0, 1, -1};
    static const i32 kDx[4] = {1, -1, 1, -1}, kDy[4] = {1, 1, -1, -1};
    const f32 cs = nav.cellSizeCm;
    const f32 spacing = std::max(p.minSpacingCm, 1.0f);
    std::unordered_map<i64, std::vector<V2>> buckets;
    u32 added = 0;

    for (u32 cy = 0; cy < nav.heightCells && added < p.maxPoints; ++cy) {
        for (u32 cx = 0; cx < nav.widthCells && added < p.maxPoints; ++cx) {
            const fmt::OcNavCell* cell = nav.at(cx, cy);
            if (!cell || (cell->flags & fmt::kOcNavWalkable) == 0) continue;

            const V2 centre{nav.originXCm + (static_cast<f32>(cx) + 0.5f) * cs,
                            nav.originYCm + (static_cast<f32>(cy) + 0.5f) * cs};
            V2 dir{};
            u32 hits = 0;
            for (int k = 0; k < 4; ++k) {
                if (!navCellBlocked(nav, static_cast<i32>(cx) + kOx[k], static_cast<i32>(cy) + kOy[k])) continue;
                const V2 t{static_cast<f32>(kOx[k]), static_cast<f32>(kOy[k])};
                if (p.isWall && !p.isWall(p.user, centre, t)) continue;
                dir += t;
                ++hits;
            }
            if (hits == 0) {
                // Only diagonals blocked: the outside corner of a wall or pillar.
                for (int k = 0; k < 4; ++k) {
                    if (!navCellBlocked(nav, static_cast<i32>(cx) + kDx[k], static_cast<i32>(cy) + kDy[k])) continue;
                    const V2 t = norm2(V2{static_cast<f32>(kDx[k]), static_cast<f32>(kDy[k])});
                    if (p.isWall && !p.isWall(p.user, centre, t)) continue;
                    dir += t;
                    ++hits;
                }
            }
            if (hits == 0) continue;
            const f32 l = len2(dir);
            if (l < 0.5f) continue;   // walls on opposite sides cancel: a corridor has no useful side
            dir = dir * (1.0f / l);

            const i32 bx = static_cast<i32>(std::floor(centre.x / spacing));
            const i32 by = static_cast<i32>(std::floor(centre.y / spacing));
            bool crowded = false;
            for (i32 oy = -1; oy <= 1 && !crowded; ++oy)
                for (i32 ox = -1; ox <= 1 && !crowded; ++ox) {
                    const auto it = buckets.find(bucketKey(bx + ox, by + oy));
                    if (it == buckets.end()) continue;
                    for (const V2& q : it->second)
                        if (distSq2(q, centre) < spacing * spacing) { crowded = true; break; }
                }
            if (crowded) continue;

            buckets[bucketKey(bx, by)].push_back(centre);
            CoverPoint c;
            c.id = nextId_++;
            c.pos = centre;
            c.dir = dir;
            c.height = p.height;
            points_.push_back(c);
            ++added;
        }
    }
    return added;
}

void CoverMap::clearGenerated() {
    points_.erase(std::remove_if(points_.begin(), points_.end(), [](const CoverPoint& c) { return !c.authored; }),
                  points_.end());
}

void CoverMap::clearAuthored() {
    points_.erase(std::remove_if(points_.begin(), points_.end(), [](const CoverPoint& c) { return c.authored; }),
                  points_.end());
}

void CoverMap::clear() { points_.clear(); }

bool CoverMap::setEnabled(u32 id, bool enabled) {
    for (CoverPoint& c : points_)
        if (c.id == id) { c.enabled = enabled; return true; }
    return false;
}

bool CoverMap::remove(u32 id) {
    const auto it = std::find_if(points_.begin(), points_.end(), [id](const CoverPoint& c) { return c.id == id; });
    if (it == points_.end()) return false;
    points_.erase(it);
    return true;
}

const CoverPoint* CoverMap::find(u32 id) const {
    for (const CoverPoint& c : points_)
        if (c.id == id) return &c;
    return nullptr;
}

CoverPoint* CoverMap::findMutable(u32 id) {
    for (CoverPoint& c : points_)
        if (c.id == id) return &c;
    return nullptr;
}

bool CoverMap::protectsFrom(const CoverPoint& c, V2 threat, const fmt::OcNavData* nav, BlockedFn blocked,
                            void* blockedUser) const {
    const V2 to = threat - c.pos;
    const f32 d = len2(to);
    if (d < 1.0f) return false;
    if (dot2(to * (1.0f / d), c.dir) < std::cos(radians(c.arcHalfAngleDeg))) return false;
    if (blocked) return blocked(blockedUser, threat, c.pos);
    if (nav) return navSegmentBlocked(*nav, threat, c.pos);
    return true;   // arc test only
}

bool CoverMap::query(const CoverQuery& q, const CoverReservations* res, CoverResult& out) const {
    const f32 nowThreatDist = dist2(q.from, q.threat);

    struct Cand { f32 straight; const CoverPoint* p; };
    std::vector<Cand> cands;
    for (const CoverPoint& c : points_) {
        if (!c.enabled) continue;
        if (q.requireHigh && c.height != CoverHeight::High) continue;
        const f32 straight = dist2(q.from, c.pos);
        if (straight > q.maxSeekCm) continue;
        const f32 td = dist2(c.pos, q.threat);
        if (td < q.minThreatDistCm || td > q.maxThreatDistCm) continue;
        if (td < nowThreatDist - q.maxCloserCm) continue;
        if (res && res->reservedByOther(c.id, q.seeker)) continue;
        if (!protectsFrom(c, q.threat, q.nav, q.blocked, q.blockedUser)) continue;
        cands.push_back({straight, &c});
    }
    if (cands.empty()) return false;

    // Nearest first, id breaking ties: the path query only runs on the closest few.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.straight != b.straight ? a.straight < b.straight : a.p->id < b.p->id;
    });
    if (!q.pathCost) {
        out = CoverResult{cands[0].p->id, cands[0].p->pos, cands[0].straight};
        return true;
    }

    bool found = false;
    const usize limit = std::min<usize>(cands.size(), std::max<u32>(q.maxPathCalls, 1));
    for (usize i = 0; i < limit; ++i) {
        const f32 cost = q.pathCost(q.pathUser, q.from, cands[i].p->pos);
        if (cost < 0.0f || cost > q.maxSeekCm) continue;
        if (!found || cost < out.cost) {
            out = CoverResult{cands[i].p->id, cands[i].p->pos, cost};
            found = true;
        }
    }
    return found;
}

bool CoverMap::claim(const CoverQuery& q, CoverReservations& res, f32 ttlSec, CoverResult& out) const {
    if (!query(q, &res, out)) return false;
    return res.reserve(out.id, q.seeker, ttlSec);
}

// ---- squads ---------------------------------------------------------------------------------------

void squadAssignRoles(const SquadMemberIn* m, u32 n, V2 target, SquadRole* out) {
    std::vector<u32> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const f32 da = distSq2(m[a].pos, target), db = distSq2(m[b].pos, target);
        return da != db ? da < db : m[a].id < m[b].id;
    });
    for (u32 r = 0; r < n; ++r) {
        out[order[r]] = r == 0 ? SquadRole::Anchor
                      : r == 1 ? SquadRole::FlankLeft
                      : r == 2 ? SquadRole::FlankRight
                               : SquadRole::Support;
    }
}

V2 squadFlankPosition(V2 centre, V2 target, SquadRole side, f32 radiusCm, f32 angleDeg) {
    const V2 b = norm2(centre - target, V2{1.0f, 0.0f});
    const f32 sign = side == SquadRole::FlankRight ? 1.0f : (side == SquadRole::FlankLeft ? -1.0f : 0.0f);
    const f32 a = sign * radians(angleDeg);
    const f32 c = std::cos(a), s = std::sin(a);
    return target + V2{b.x * c - b.y * s, b.x * s + b.y * c} * radiusCm;
}

V2 squadSpacingPush(V2 self, const V2* others, u32 n, f32 spacingCm) {
    V2 push{};
    for (u32 i = 0; i < n; ++i) {
        const V2 away = self - others[i];
        const f32 d = len2(away);
        if (d >= spacingCm) continue;
        const V2 dir = d > 1e-4f ? away * (1.0f / d)
                                 : V2{std::cos(static_cast<f32>(i)), std::sin(static_cast<f32>(i))};
        push += dir * (spacingCm - d);
    }
    return push;
}

V2 squadSupportPosition(V2 anchor, V2 target, f32 behindCm, u32 slot, f32 spacingCm) {
    const V2 back = norm2(anchor - target, V2{-1.0f, 0.0f});
    const V2 side = perp2(back);
    const i32 k = slot == 0 ? 0 : ((slot & 1u) ? static_cast<i32>((slot + 1) / 2) : -static_cast<i32>(slot / 2));
    return anchor + back * behindCm + side * (spacingCm * static_cast<f32>(k));
}

} // namespace aver::synapse
