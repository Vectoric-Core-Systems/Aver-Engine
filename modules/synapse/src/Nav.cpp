#include "aver/synapse/Nav.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace aver::synapse {
namespace {

bool walkable(const fmt::OcNavData& nav, u32 x, u32 y) {
    const fmt::OcNavCell* c = nav.at(x, y);
    return c && (c->flags & fmt::kOcNavWalkable) != 0;
}

u32 idx(const fmt::OcNavData& nav, u32 x, u32 y) { return y * nav.widthCells + x; }

// The eight neighbours, orthogonals first. Order matters only for tie-breaking, and putting the
// orthogonals first makes an A* prefer a straight step to a diagonal one of equal cost, which is
// what a path across open ground should look like.
constexpr i32 kDx[8] = { 1, -1,  0,  0,  1,  1, -1, -1 };
constexpr i32 kDy[8] = { 0,  0,  1, -1,  1, -1,  1, -1 };

constexpr f32 kSqrt2 = 1.41421356f;

} // namespace

bool worldToCell(const fmt::OcNavData& nav, f32 xCm, f32 yCm, u32& outX, u32& outY) {
    if (nav.cellSizeCm <= 0.0f) return false;
    const f32 fx = (xCm - nav.originXCm) / nav.cellSizeCm;
    const f32 fy = (yCm - nav.originYCm) / nav.cellSizeCm;
    // FLOOR, not truncate, and not a cast. A cast rounds toward zero, so every position with a
    // negative grid coordinate would land one cell too high and the whole grid would be off by one
    // on two of its four sides. ChunkCoord.hpp:106 records the same trap for chunk coordinates.
    const f32 ffx = std::floor(fx), ffy = std::floor(fy);
    if (ffx < 0.0f || ffy < 0.0f) return false;
    const f32 wx = static_cast<f32>(nav.widthCells), wy = static_cast<f32>(nav.heightCells);
    if (ffx >= wx || ffy >= wy) return false;
    outX = static_cast<u32>(ffx);
    outY = static_cast<u32>(ffy);
    return true;
}

Vec3 cellToWorld(const fmt::OcNavData& nav, u32 x, u32 y) {
    const fmt::OcNavCell* c = nav.at(x, y);
    return Vec3{nav.originXCm + (static_cast<f32>(x) + 0.5f) * nav.cellSizeCm,
                nav.originYCm + (static_cast<f32>(y) + 0.5f) * nav.cellSizeCm,
                c ? c->floorZCm : 0.0f};
}

bool nearestWalkable(const fmt::OcNavData& nav, f32 xCm, f32 yCm, u32 maxRingCells,
                     u32& outX, u32& outY) {
    u32 cx = 0, cy = 0;
    if (!worldToCell(nav, xCm, yCm, cx, cy)) {
        // OUTSIDE THE GRID ENTIRELY. Clamping to the nearest edge cell and searching from there
        // would silently path an agent that is far off the map as though it were standing on the
        // rim. Refusing is the honest answer, and the caller gets OffMesh.
        return false;
    }
    if (walkable(nav, cx, cy)) { outX = cx; outY = cy; return true; }

    for (u32 r = 1; r <= maxRingCells; ++r) {
        const i32 ri = static_cast<i32>(r);
        for (i32 dy = -ri; dy <= ri; ++dy) {
            for (i32 dx = -ri; dx <= ri; ++dx) {
                // The RING only: everything closer was covered by a previous, smaller r, so this
                // finds the nearest rather than merely a near one.
                if (std::abs(dx) != ri && std::abs(dy) != ri) continue;
                const i32 nx = static_cast<i32>(cx) + dx, ny = static_cast<i32>(cy) + dy;
                if (nx < 0 || ny < 0) continue;
                const u32 ux = static_cast<u32>(nx), uy = static_cast<u32>(ny);
                if (walkable(nav, ux, uy)) { outX = ux; outY = uy; return true; }
            }
        }
    }
    return false;
}

u32 buildRegions(fmt::OcNavData& nav) {
    for (fmt::OcNavCell& c : nav.cells) c.regionId = 0;
    u32 next = 0;
    std::vector<u32> stack;

    for (u32 y = 0; y < nav.heightCells; ++y) {
        for (u32 x = 0; x < nav.widthCells; ++x) {
            if (!walkable(nav, x, y)) continue;
            if (nav.cells[idx(nav, x, y)].regionId != 0) continue;

            // SATURATES rather than wrapping. A wrapped id would merge two unconnected regions into
            // one and make the O(1) reachability check answer "reachable" for a pair that is not --
            // exactly the failure it exists to prevent. Past the ceiling every further region shares
            // 65535, which makes the check merely useless for those cells rather than WRONG for
            // them: it will say "maybe" and let A* decide, which is the safe direction to fail in.
            u16 id;
            if (next < 65535u) { ++next; id = static_cast<u16>(next); }
            else                { id = 65535u; }

            stack.clear();
            stack.push_back(idx(nav, x, y));
            nav.cells[idx(nav, x, y)].regionId = id;
            while (!stack.empty()) {
                const u32 at = stack.back(); stack.pop_back();
                const u32 ax = at % nav.widthCells, ay = at / nav.widthCells;
                for (int k = 0; k < 8; ++k) {
                    const i32 nx = static_cast<i32>(ax) + kDx[k], ny = static_cast<i32>(ay) + kDy[k];
                    if (nx < 0 || ny < 0) continue;
                    const u32 ux = static_cast<u32>(nx), uy = static_cast<u32>(ny);
                    if (ux >= nav.widthCells || uy >= nav.heightCells) continue;
                    if (!walkable(nav, ux, uy)) continue;
                    if (nav.cells[idx(nav, ux, uy)].regionId != 0) continue;
                    // THE FLOOD USES THE SAME STEP RULE THE SEARCH WILL. If it did not, two cells
                    // could share a region with a cliff between them, and the precheck would promise
                    // a path A* cannot find -- turning an O(1) refusal into a guaranteed 8000-node
                    // dead end. The region id is only meaningful if it means "actually connected".
                    const f32 dz = std::fabs(nav.cells[idx(nav, ux, uy)].floorZCm -
                                             nav.cells[at].floorZCm);
                    if (dz > nav.maxStepCm) continue;
                    if (k >= 4) {
                        // A diagonal needs both of its shared orthogonals, so the flood does not
                        // leak through the corner where two walls meet.
                        if (!walkable(nav, ux, ay) || !walkable(nav, ax, uy)) continue;
                    }
                    nav.cells[idx(nav, ux, uy)].regionId = id;
                    stack.push_back(idx(nav, ux, uy));
                }
            }
        }
    }
    return next;
}

bool canStep(const fmt::OcNavData& nav, u32 ax, u32 ay, u32 bx, u32 by) {
    if (!walkable(nav, ax, ay) || !walkable(nav, bx, by)) return false;
    const i32 dx = static_cast<i32>(bx) - static_cast<i32>(ax);
    const i32 dy = static_cast<i32>(by) - static_cast<i32>(ay);
    if (std::abs(dx) > 1 || std::abs(dy) > 1) return false;   // not adjacent
    if (dx == 0 && dy == 0) return true;

    const fmt::OcNavCell* a = nav.at(ax, ay);
    const fmt::OcNavCell* b = nav.at(bx, by);
    if (std::fabs(a->floorZCm - b->floorZCm) > nav.maxStepCm) return false;
    if (dx != 0 && dy != 0) {
        // No corner cutting: both cells sharing the diagonal must be walkable too.
        if (!walkable(nav, bx, ay) || !walkable(nav, ax, by)) return false;
    }
    return true;
}

bool lineOfSight(const fmt::OcNavData& nav, u32 ax, u32 ay, u32 bx, u32 by) {
    // A SUPERCOVER walk: every cell the segment touches, not just the ones a thin Bresenham line
    // would name. A thin line can slip diagonally between two blocked cells and report clear, which
    // is how a string-pull produces a shortcut through a wall corner.
    i32 x = static_cast<i32>(ax), y = static_cast<i32>(ay);
    const i32 tx = static_cast<i32>(bx), ty = static_cast<i32>(by);
    i32 dx = std::abs(tx - x), dy = std::abs(ty - y);
    const i32 sx = tx > x ? 1 : -1, sy = ty > y ? 1 : -1;
    i32 err = dx - dy;

    if (!walkable(nav, ax, ay)) return false;
    while (x != tx || y != ty) {
        const i32 e2 = 2 * err;
        i32 nx = x, ny = y;
        if (e2 > -dy) { err -= dy; nx = x + sx; }
        if (e2 <  dx) { err += dx; ny = y + sy; }
        if (nx != x && ny != y) {
            // A diagonal move: require the two orthogonal cells it passes between, so the line
            // cannot squeeze through a corner.
            if (!canStep(nav, static_cast<u32>(x), static_cast<u32>(y),
                         static_cast<u32>(nx), static_cast<u32>(ny))) return false;
        } else {
            if (!canStep(nav, static_cast<u32>(x), static_cast<u32>(y),
                         static_cast<u32>(nx), static_cast<u32>(ny))) return false;
        }
        x = nx; y = ny;
    }
    return true;
}

PathResult findPath(const fmt::OcNavData& nav, const PathRequest& req) {
    PathResult out;
    if (!nav.valid() || nav.cells.empty()) { out.status = PathStatus::Invalid; return out; }

    u32 sx = 0, sy = 0, gx = 0, gy = 0;
    if (!nearestWalkable(nav, req.startXCm, req.startYCm, req.snapRingCells, sx, sy) ||
        !nearestWalkable(nav, req.goalXCm,  req.goalYCm,  req.snapRingCells, gx, gy)) {
        out.status = PathStatus::OffMesh;
        return out;
    }

    // THE O(1) REFUSAL. Two cells in different regions have no path, and finding that out by
    // exhausting an 8000-node search is the single most expensive thing this function can be asked
    // to do. It is also the common case: an agent asked to reach somewhere across a locked door will
    // ask again every think tick, forever.
    const u16 sr = nav.cells[idx(nav, sx, sy)].regionId;
    const u16 gr = nav.cells[idx(nav, gx, gy)].regionId;
    if (sr != gr) { out.status = PathStatus::Unreachable; return out; }

    const u32 n = static_cast<u32>(nav.cells.size());
    const f32 kInf = 3.4e38f;
    std::vector<f32> g(n, kInf);
    std::vector<i32> from(n, -1);
    std::vector<u8>  closed(n, 0);

    auto octile = [&](u32 x, u32 y) {
        const f32 dx = std::fabs(static_cast<f32>(x) - static_cast<f32>(gx));
        const f32 dy = std::fabs(static_cast<f32>(y) - static_cast<f32>(gy));
        return (dx + dy) + (kSqrt2 - 2.0f) * std::min(dx, dy);
    };

    struct Node { f32 f; u32 i; };
    struct Worse { bool operator()(const Node& a, const Node& b) const { return a.f > b.f; } };
    std::priority_queue<Node, std::vector<Node>, Worse> open;

    const u32 startI = idx(nav, sx, sy), goalI = idx(nav, gx, gy);
    g[startI] = 0.0f;
    open.push({octile(sx, sy), startI});

    // The best node seen, by heuristic, so a budget-exhausted search returns something useful rather
    // than nothing. An agent that gets most of the way there and re-asks next tick behaves far
    // better than one that stands still.
    u32 best = startI;
    f32 bestH = octile(sx, sy);

    while (!open.empty()) {
        const Node cur = open.top(); open.pop();
        if (closed[cur.i]) continue;
        closed[cur.i] = 1;
        ++out.expansions;

        if (cur.i == goalI) { best = goalI; bestH = 0.0f; break; }
        if (out.expansions >= req.maxExpansions) break;

        const u32 cx = cur.i % nav.widthCells, cy = cur.i / nav.widthCells;
        const f32 h = octile(cx, cy);
        if (h < bestH) { bestH = h; best = cur.i; }

        for (int k = 0; k < 8; ++k) {
            const i32 nx = static_cast<i32>(cx) + kDx[k], ny = static_cast<i32>(cy) + kDy[k];
            if (nx < 0 || ny < 0) continue;
            const u32 ux = static_cast<u32>(nx), uy = static_cast<u32>(ny);
            if (ux >= nav.widthCells || uy >= nav.heightCells) continue;
            if (!canStep(nav, cx, cy, ux, uy)) continue;
            const u32 ni = idx(nav, ux, uy);
            if (closed[ni]) continue;
            const f32 step = (k < 4) ? 1.0f : kSqrt2;
            const f32 ng = g[cur.i] + step;
            if (ng < g[ni]) {
                g[ni] = ng;
                from[ni] = static_cast<i32>(cur.i);
                open.push({ng + octile(ux, uy), ni});
            }
        }
    }

    const bool reached = closed[goalI] != 0;
    u32 end = reached ? goalI : best;
    out.status = reached ? PathStatus::Found : PathStatus::Partial;
    // A budget-exhausted search that never left the start has nothing useful to say. Reporting a
    // one-point "path" would have the caller walk to where it already is, every tick.
    if (!reached && end == startI) { out.status = PathStatus::Partial; }

    // Walk the parents back, then reverse.
    std::vector<u32> cells;
    for (i32 at = static_cast<i32>(end); at >= 0; at = from[static_cast<u32>(at)])
        cells.push_back(static_cast<u32>(at));
    std::reverse(cells.begin(), cells.end());

    // STRING-PULL. Greedily extend from the current anchor to the furthest cell still in clear line
    // of sight, so a path across open ground becomes two points rather than forty. It is done
    // against the GRID, not against physics: the grid already encodes what a raycast would answer,
    // and a bake that is wrong about a wall is wrong for the pathfinder too -- consulting physics
    // here would make the two disagree.
    out.points.clear();
    if (!cells.empty()) {
        usize anchor = 0;
        out.points.push_back(cellToWorld(nav, cells[0] % nav.widthCells, cells[0] / nav.widthCells));
        while (anchor + 1 < cells.size()) {
            usize furthest = anchor + 1;
            for (usize j = cells.size() - 1; j > anchor; --j) {
                if (lineOfSight(nav, cells[anchor] % nav.widthCells, cells[anchor] / nav.widthCells,
                                cells[j] % nav.widthCells, cells[j] / nav.widthCells)) {
                    furthest = j;
                    break;
                }
            }
            out.points.push_back(cellToWorld(nav, cells[furthest] % nav.widthCells,
                                             cells[furthest] / nav.widthCells));
            anchor = furthest;
        }
    }
    return out;
}

} // namespace aver::synapse
