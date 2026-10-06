#include "aver/synapse/CrowdOrca.hpp"

#include "aver/formats/OcNav.hpp"

#include <algorithm>
#include <cmath>

namespace aver::synapse {
namespace {

constexpr f32 kEps = 1e-5f;

// Best point on line `lineNo` that satisfies every earlier line, inside the speed disc.
bool lp1(const std::vector<OrcaLine>& lines, usize lineNo, f32 radius, V2 opt, bool dirOpt, V2& result) {
    const OrcaLine& L = lines[lineNo];
    const f32 dotp = dot2(L.point, L.direction);
    const f32 disc = dotp * dotp + radius * radius - lenSq2(L.point);
    if (disc < 0.0f) return false;   // the line misses the speed disc entirely
    const f32 root = std::sqrt(disc);
    f32 tLeft = -dotp - root, tRight = -dotp + root;

    for (usize i = 0; i < lineNo; ++i) {
        const f32 denom = det2(L.direction, lines[i].direction);
        const f32 numer = det2(lines[i].direction, L.point - lines[i].point);
        if (std::fabs(denom) <= kEps) {
            if (numer < 0.0f) return false;   // parallel and on the wrong side
            continue;
        }
        const f32 t = numer / denom;
        if (denom >= 0.0f) tRight = std::min(tRight, t);
        else               tLeft  = std::max(tLeft, t);
        if (tLeft > tRight) return false;
    }

    if (dirOpt) {
        result = L.point + (dot2(opt, L.direction) > 0.0f ? tRight : tLeft) * L.direction;
    } else {
        const f32 t = std::clamp(dot2(L.direction, opt - L.point), tLeft, tRight);
        result = L.point + t * L.direction;
    }
    return true;
}

// Index of the first line it could not satisfy, or lines.size() on success.
usize lp2(const std::vector<OrcaLine>& lines, f32 radius, V2 opt, bool dirOpt, V2& result) {
    if (dirOpt)                              result = opt * radius;
    else if (lenSq2(opt) > radius * radius)  result = norm2(opt) * radius;
    else                                     result = opt;

    for (usize i = 0; i < lines.size(); ++i) {
        if (det2(lines[i].direction, lines[i].point - result) > 0.0f) {
            const V2 keep = result;
            if (!lp1(lines, i, radius, opt, dirOpt, result)) {
                result = keep;
                return i;
            }
        }
    }
    return lines.size();
}

// Infeasible case: minimise the largest violation of the soft lines, hard lines kept as-is.
void lp3(const std::vector<OrcaLine>& lines, usize hardCount, usize begin, f32 radius, V2& result,
         std::vector<OrcaLine>& proj) {
    f32 worst = 0.0f;
    for (usize i = begin; i < lines.size(); ++i) {
        if (det2(lines[i].direction, lines[i].point - result) <= worst) continue;

        proj.assign(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(hardCount));
        for (usize j = hardCount; j < i; ++j) {
            OrcaLine l;
            const f32 d = det2(lines[i].direction, lines[j].direction);
            if (std::fabs(d) <= kEps) {
                if (dot2(lines[i].direction, lines[j].direction) > 0.0f) continue;   // same way
                l.point = (lines[i].point + lines[j].point) * 0.5f;
            } else {
                l.point = lines[i].point +
                          (det2(lines[j].direction, lines[i].point - lines[j].point) / d) * lines[i].direction;
            }
            l.direction = norm2(lines[j].direction - lines[i].direction);
            proj.push_back(l);
        }
        const V2 keep = result;
        const V2 optDir{-lines[i].direction.y, lines[i].direction.x};
        if (lp2(proj, radius, optDir, true, result) < proj.size()) result = keep;
        worst = det2(lines[i].direction, lines[i].point - result);
    }
}

} // namespace

OrcaLine orcaAgentLine(V2 posA, V2 velA, f32 rA, V2 posB, V2 velB, f32 rB, f32 share,
                       f32 timeHorizon, f32 dt) {
    const V2 relPos = posB - posA;
    const V2 relVel = velA - velB;
    const f32 distSq = lenSq2(relPos);
    const f32 rr = rA + rB;
    const f32 rrSq = rr * rr;

    OrcaLine line;
    V2 u;
    if (distSq > rrSq) {
        const f32 invT = 1.0f / timeHorizon;
        const V2 w = relVel - relPos * invT;   // from the cut-off circle's centre to the relative velocity
        const f32 wLenSq = lenSq2(w);
        const f32 dot1 = dot2(w, relPos);

        if (dot1 < 0.0f && dot1 * dot1 > rrSq * wLenSq) {
            // Closest boundary point is on the circular cut-off.
            const f32 wLen = std::sqrt(wLenSq);
            const V2 unitW = w * (1.0f / wLen);
            line.direction = {unitW.y, -unitW.x};
            u = unitW * (rr * invT - wLen);
        } else {
            // Closest boundary point is on one of the two tangent legs.
            const f32 leg = std::sqrt(distSq - rrSq);
            if (det2(relPos, w) > 0.0f) {
                line.direction = V2{relPos.x * leg - relPos.y * rr, relPos.x * rr + relPos.y * leg} * (1.0f / distSq);
            } else {
                line.direction = -(V2{relPos.x * leg + relPos.y * rr, -relPos.x * rr + relPos.y * leg} * (1.0f / distSq));
            }
            u = line.direction * dot2(relVel, line.direction) - relVel;
        }
    } else {
        // Already overlapping: resolve within one step.
        const f32 invDt = 1.0f / dt;
        const V2 w = relVel - relPos * invDt;
        const f32 wLen = len2(w);
        const V2 unitW = wLen > 1e-6f ? w * (1.0f / wLen) : V2{1.0f, 0.0f};
        line.direction = {unitW.y, -unitW.x};
        u = unitW * (rr * invDt - wLen);
    }
    line.point = velA + u * share;
    return line;
}

OrcaLine orcaWallLine(V2 posA, f32 rA, V2 closest, V2 fallbackNormal, f32 horizon) {
    const V2 away = posA - closest;
    const f32 d = len2(away);
    const V2 n = d > 1e-5f ? away * (1.0f / d) : fallbackNormal;
    // Allowed speed INTO the wall; negative once inside the radius, which forces the agent out.
    const f32 c = -(d - rA) / horizon;
    OrcaLine line;
    line.point = n * c;
    line.direction = {n.y, -n.x};
    return line;
}

V2 orcaSolve(const std::vector<OrcaLine>& lines, u32 hardCount, V2 preferred, f32 maxSpeed,
             OrcaScratch& scratch) {
    V2 result;
    const usize fail = lp2(lines, maxSpeed, preferred, false, result);
    if (fail < lines.size()) lp3(lines, hardCount, fail, maxSpeed, result, scratch.proj);
    return result;
}

V2 closestPointOnCell(V2 p, f32 minX, f32 minY, f32 size) {
    return {std::clamp(p.x, minX, minX + size), std::clamp(p.y, minY, minY + size)};
}

bool navCellBlocked(const fmt::OcNavData& nav, i32 cx, i32 cy) {
    if (cx < 0 || cy < 0) return true;
    const fmt::OcNavCell* c = nav.at(static_cast<u32>(cx), static_cast<u32>(cy));
    return !c || (c->flags & fmt::kOcNavWalkable) == 0;
}

void orcaGatherWalls(const fmt::OcNavData& nav, V2 pos, f32 radius, f32 reach, f32 horizon,
                     u32 maxLines, std::vector<OrcaLine>& out) {
    if (nav.cellSizeCm <= 0.0f || nav.widthCells == 0 || nav.heightCells == 0) return;
    const f32 cs = nav.cellSizeCm;
    const f32 range = radius + reach;

    const i32 x0 = static_cast<i32>(std::floor((pos.x - range - nav.originXCm) / cs));
    const i32 x1 = static_cast<i32>(std::floor((pos.x + range - nav.originXCm) / cs));
    const i32 y0 = static_cast<i32>(std::floor((pos.y - range - nav.originYCm) / cs));
    const i32 y1 = static_cast<i32>(std::floor((pos.y + range - nav.originYCm) / cs));

    struct Cand { f32 d; i32 cx, cy; V2 p; V2 centre; };
    thread_local std::vector<Cand> cands;
    cands.clear();

    for (i32 cy = y0; cy <= y1; ++cy) {
        for (i32 cx = x0; cx <= x1; ++cx) {
            if (!navCellBlocked(nav, cx, cy)) continue;
            // Interior wall cells are hidden behind the boundary ones.
            if (navCellBlocked(nav, cx - 1, cy) && navCellBlocked(nav, cx + 1, cy) &&
                navCellBlocked(nav, cx, cy - 1) && navCellBlocked(nav, cx, cy + 1)) continue;
            const f32 minX = nav.originXCm + static_cast<f32>(cx) * cs;
            const f32 minY = nav.originYCm + static_cast<f32>(cy) * cs;
            const V2 p = closestPointOnCell(pos, minX, minY, cs);
            const f32 d = dist2(pos, p);
            if (d > range) continue;
            cands.push_back({d, cx, cy, p, V2{minX + cs * 0.5f, minY + cs * 0.5f}});
        }
    }
    if (cands.empty()) return;

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.d != b.d) return a.d < b.d;
        if (a.cy != b.cy) return a.cy < b.cy;
        return a.cx < b.cx;
    });
    const usize n = std::min<usize>(cands.size(), maxLines);
    for (usize i = 0; i < n; ++i)
        out.push_back(orcaWallLine(pos, radius, cands[i].p,
                                   norm2(pos - cands[i].centre, V2{1.0f, 0.0f}), horizon));
}

} // namespace aver::synapse
