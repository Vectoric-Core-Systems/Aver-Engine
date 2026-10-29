#include "aver/synapse/NavGraph.hpp"
#include "aver/synapse/NavMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace aver::synapse {
namespace {

constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kSqrt2 = 1.41421356237f;

struct HeapItem {
    f32 f, g;
    i64 key;
    u32 stamp;
};
struct HeapWorse {
    bool operator()(const HeapItem& a, const HeapItem& b) const {
        if (a.f != b.f) return a.f > b.f;
        if (a.g != b.g) return a.g < b.g;   // among equal f the one further along first
        return a.key > b.key;
    }
};

}  // namespace

// ------------------------------------------------------------------------------------------------------------ AStarGraph

void AStarGraph::addPoint(i64 id, const Vec3& pos, f32 weightScale) {
    if (id < 0) return;   // Godot refuses negative ids
    Pt& p = pts_[id];
    p.pos = pos;
    p.weight = weightScale < 0 ? 1.0f : weightScale;
}
Vec3 AStarGraph::pointPosition(i64 id) const { auto it = pts_.find(id); return it == pts_.end() ? Vec3{} : it->second.pos; }
f32 AStarGraph::pointWeight(i64 id) const { auto it = pts_.find(id); return it == pts_.end() ? 0.0f : it->second.weight; }
void AStarGraph::setPointPosition(i64 id, const Vec3& pos) { auto it = pts_.find(id); if (it != pts_.end()) it->second.pos = pos; }
void AStarGraph::setPointWeight(i64 id, f32 w) { auto it = pts_.find(id); if (it != pts_.end() && w >= 0) it->second.weight = w; }
void AStarGraph::removePoint(i64 id) {
    if (!pts_.erase(id)) return;
    for (auto& [k, p] : pts_) p.out.erase(id);
}
void AStarGraph::setDisabled(i64 id, bool d) { auto it = pts_.find(id); if (it != pts_.end()) it->second.disabled = d; }
bool AStarGraph::isDisabled(i64 id) const { auto it = pts_.find(id); return it != pts_.end() && it->second.disabled; }
std::vector<i64> AStarGraph::pointIds() const {
    std::vector<i64> v;
    for (const auto& [k, p] : pts_) v.push_back(k);
    return v;
}
i64 AStarGraph::nextFreeId() const {
    i64 id = 0;
    for (const auto& [k, p] : pts_) { if (k != id) break; ++id; }   // the smallest unused non-negative id
    return id;
}

bool AStarGraph::connect(i64 id, i64 to, bool bidirectional) {
    if (id == to) return false;
    auto a = pts_.find(id), b = pts_.find(to);
    if (a == pts_.end() || b == pts_.end()) return false;
    a->second.out.insert(to);
    if (bidirectional) b->second.out.insert(id);
    return true;
}
void AStarGraph::disconnect(i64 id, i64 to, bool bidirectional) {
    auto a = pts_.find(id), b = pts_.find(to);
    if (a == pts_.end() || b == pts_.end()) return;
    a->second.out.erase(to);
    if (bidirectional) b->second.out.erase(id);
}
bool AStarGraph::areConnected(i64 id, i64 to, bool bidirectional) const {
    auto a = pts_.find(id), b = pts_.find(to);
    if (a == pts_.end() || b == pts_.end()) return false;
    if (a->second.out.count(to)) return true;
    return bidirectional && b->second.out.count(id) != 0;
}
std::vector<i64> AStarGraph::connections(i64 id) const {
    auto it = pts_.find(id);
    if (it == pts_.end()) return {};
    return std::vector<i64>(it->second.out.begin(), it->second.out.end());
}

i64 AStarGraph::closestPoint(const Vec3& to, bool includeDisabled) const {
    i64 best = -1;
    f32 bd = 1e20f;
    for (const auto& [k, p] : pts_) {
        if (p.disabled && !includeDisabled) continue;
        const f32 d = distSquared(p.pos, to);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

Vec3 AStarGraph::closestPositionInSegment(const Vec3& to) const {
    Vec3 best;
    f32 bd = 1e20f;
    for (const auto& [k, p] : pts_) {
        if (p.disabled) continue;
        for (i64 n : p.out) {
            const Pt& q = pts_.at(n);
            if (q.disabled) continue;
            const Vec3 c = navClosestOnSegment(to, p.pos, q.pos);
            const f32 d = distSquared(c, to);
            if (d < bd) { bd = d; best = c; }
        }
    }
    return best;
}

bool AStarGraph::solve(i64 from, i64 to, bool partial, std::vector<i64>& path) const {
    path.clear();
    auto s = pts_.find(from), e = pts_.find(to);
    if (s == pts_.end() || e == pts_.end()) return false;
    if (s->second.disabled) return false;
    if (from == to) { path.push_back(from); return true; }
    if (e->second.disabled && !partial) return false;

    auto estimate = [&](i64 a, i64 b) { return estimate_ ? estimate_(a, b) : dist(pts_.at(a).pos, pts_.at(b).pos); };
    auto compute = [&](i64 a, i64 b) { return compute_ ? compute_(a, b) : dist(pts_.at(a).pos, pts_.at(b).pos); };

    struct State { f32 g = kInf, h = 0; i64 prev = -1; bool closed = false; u32 stamp = 0; };
    std::unordered_map<i64, State> st;
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapWorse> open;
    State& s0 = st[from];
    s0.g = 0; s0.h = estimate(from, to);
    open.push({s0.h, 0, from, 0});
    i64 closest = -1;
    bool found = false;
    while (!open.empty()) {
        const HeapItem top = open.top();
        open.pop();
        State& cur = st[top.key];
        if (cur.closed || top.stamp != cur.stamp) continue;
        if (closest < 0 || st[closest].h > cur.h || (st[closest].h == cur.h && st[closest].g > cur.g)) closest = top.key;
        if (top.key == to) { found = true; break; }
        cur.closed = true;
        const Pt& cp = pts_.at(top.key);
        for (i64 n : cp.out) {
            const Pt& np = pts_.at(n);
            if (np.disabled) continue;
            auto it = st.find(n);
            if (it != st.end() && it->second.closed) continue;
            const f32 g = cur.g + compute(top.key, n) * np.weight;
            State& ns = st[n];
            if (ns.g != kInf && !(g < ns.g)) continue;
            ns.g = g;
            ns.h = estimate(n, to);
            ns.prev = top.key;
            ns.stamp++;
            open.push({g + ns.h, g, n, ns.stamp});
        }
    }
    i64 end = found ? to : (partial ? closest : -1);
    if (end < 0) return false;
    std::vector<i64> rev;
    for (i64 k = end; k >= 0; k = st[k].prev) {
        rev.push_back(k);
        if (k == from) break;
    }
    std::reverse(rev.begin(), rev.end());
    path = std::move(rev);
    return found || partial;
}

std::vector<i64> AStarGraph::idPath(i64 from, i64 to, bool allowPartial) const {
    std::vector<i64> p;
    solve(from, to, allowPartial, p);
    return p;
}

std::vector<Vec3> AStarGraph::pointPath(i64 from, i64 to, bool allowPartial) const {
    std::vector<Vec3> out;
    for (i64 id : idPath(from, to, allowPartial)) out.push_back(pts_.at(id).pos);
    return out;
}

// ------------------------------------------------------------------------------------------------------------ AStarGrid

f32 AStarGrid::heuristic(Heuristic h, i32 dx, i32 dy) {
    const f32 ax = static_cast<f32>(std::abs(dx)), ay = static_cast<f32>(std::abs(dy));
    switch (h) {
    case Manhattan: return ax + ay;
    case Octile: return std::max(ax, ay) + (kSqrt2 - 1.0f) * std::min(ax, ay);
    case Chebyshev: return std::max(ax, ay);
    case Euclidean:
    default: return std::sqrt(ax * ax + ay * ay);
    }
}

void AStarGrid::update() {
    const usize n = static_cast<usize>(std::max(region_.w, 0)) * static_cast<usize>(std::max(region_.h, 0));
    solid_.assign(n, 0);
    weight_.assign(n, 1.0f);
    dirty_ = false;
}
void AStarGrid::setSolid(i32 x, i32 y, bool s) { if (!dirty_ && inBounds(x, y)) solid_[index(x, y)] = s ? 1 : 0; }
bool AStarGrid::isSolid(i32 x, i32 y) const { return !dirty_ && inBounds(x, y) && solid_[index(x, y)]; }
void AStarGrid::fillSolid(const Rect& r, bool s) {
    if (dirty_) return;
    for (i32 y = std::max(r.y, region_.y); y < std::min(r.y + r.h, region_.y + region_.h); ++y)
        for (i32 x = std::max(r.x, region_.x); x < std::min(r.x + r.w, region_.x + region_.w); ++x) solid_[index(x, y)] = s ? 1 : 0;
}
void AStarGrid::setWeight(i32 x, i32 y, f32 w) { if (!dirty_ && inBounds(x, y)) weight_[index(x, y)] = w; }
f32 AStarGrid::weight(i32 x, i32 y) const { return (!dirty_ && inBounds(x, y)) ? weight_[index(x, y)] : 0.0f; }
void AStarGrid::fillWeight(const Rect& r, f32 w) {
    if (dirty_) return;
    for (i32 y = std::max(r.y, region_.y); y < std::min(r.y + r.h, region_.y + region_.h); ++y)
        for (i32 x = std::max(r.x, region_.x); x < std::min(r.x + r.w, region_.x + region_.w); ++x) weight_[index(x, y)] = w;
}
void AStarGrid::clear() {
    region_ = {};
    solid_.clear();
    weight_.clear();
    dirty_ = false;
}

bool AStarGrid::canDiag(i32 x, i32 y, i32 dx, i32 dy) const {
    switch (diag_) {
    case Always: return true;
    case Never: return false;
    case AtLeastOneWalkable: return walk(x + dx, y) || walk(x, y + dy);
    case OnlyIfNoObstacles: return walk(x + dx, y) && walk(x, y + dy);
    }
    return true;
}

f32 AStarGrid::computeCost(i32 fx, i32 fy, i32 tx, i32 ty) const {
    return compute_ ? compute_(fx, fy, tx, ty) : heuristic(hCompute_, tx - fx, ty - fy);
}
f32 AStarGrid::estimateCost(i32 fx, i32 fy, i32 tx, i32 ty) const {
    return estimate_ ? estimate_(fx, fy, tx, ty) : heuristic(hEstimate_, tx - fx, ty - fy);
}

bool AStarGrid::jump(i32 x, i32 y, i32 dx, i32 dy, i32 gx, i32 gy, i32& ox, i32& oy) const {
    for (;;) {
        const i32 nx = x + dx, ny = y + dy;
        if (dx != 0 && dy != 0) {
            if (!walk(nx, ny) || !canDiag(x, y, dx, dy)) return false;
        } else if (!walk(nx, ny)) {
            return false;
        }
        x = nx; y = ny;
        if (x == gx && y == gy) { ox = x; oy = y; return true; }
        i32 tx, ty;
        if (dx != 0 && dy != 0) {
            // forced neighbours of a diagonal move, then the straight scans from here
            if ((!walk(x - dx, y) && walk(x - dx, y + dy)) || (!walk(x, y - dy) && walk(x + dx, y - dy))) { ox = x; oy = y; return true; }
            if (jump(x, y, dx, 0, gx, gy, tx, ty) || jump(x, y, 0, dy, gx, gy, tx, ty)) { ox = x; oy = y; return true; }
        } else if (dx != 0) {
            // Diagonals allowed: a blocked side cell with a free cell beyond it forces a turn here. Orthogonal-only: the turn is where the side opens.
            for (int s = -1; s <= 1; s += 2) {
                bool forced;
                if (diag_ == Never) forced = !walk(x - dx, y + s) && walk(x, y + s);
                else if (diag_ == OnlyIfNoObstacles) forced = !walk(x - dx, y + s) && walk(x, y + s) && walk(x + dx, y + s);
                else forced = !walk(x, y + s) && walk(x + dx, y + s);
                if (forced) { ox = x; oy = y; return true; }
            }
        } else {
            if (diag_ == Never) {
                // vertical movement is the "diagonal" of a four-connected grid: a horizontal scan that finds something makes this a jump point
                if (jump(x, y, 1, 0, gx, gy, tx, ty) || jump(x, y, -1, 0, gx, gy, tx, ty)) { ox = x; oy = y; return true; }
            }
            for (int s = -1; s <= 1; s += 2) {
                bool forced;
                if (diag_ == Never) forced = !walk(x + s, y - dy) && walk(x + s, y);
                else if (diag_ == OnlyIfNoObstacles) forced = !walk(x + s, y - dy) && walk(x + s, y) && walk(x + s, y + dy);
                else forced = !walk(x + s, y) && walk(x + s, y + dy);
                if (forced) { ox = x; oy = y; return true; }
            }
        }
    }
}

bool AStarGrid::solve(i32 fx, i32 fy, i32 tx, i32 ty, bool partial, std::vector<std::pair<i32, i32>>& path) const {
    path.clear();
    if (dirty_ || !inBounds(fx, fy) || !inBounds(tx, ty)) return false;
    if (!walk(fx, fy)) return false;
    if (fx == tx && fy == ty) { path.push_back({fx, fy}); return true; }
    if (!walk(tx, ty) && !partial) return false;

    struct State { f32 g = kInf, h = 0; i32 px = 0, py = 0; bool hasPrev = false, closed = false; u32 stamp = 0; };
    const usize n = static_cast<usize>(region_.w) * static_cast<usize>(region_.h);
    std::vector<State> st(n);
    auto keyOf = [&](i32 x, i32 y) { return static_cast<i64>(index(x, y)); };
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapWorse> open;
    State& s0 = st[index(fx, fy)];
    s0.g = 0; s0.h = estimateCost(fx, fy, tx, ty);
    open.push({s0.h, 0, keyOf(fx, fy), 0});
    i64 closest = -1;
    bool found = false;
    const i32 dirsAll[8][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1}};

    while (!open.empty()) {
        const HeapItem top = open.top();
        open.pop();
        State& cur = st[static_cast<usize>(top.key)];
        if (cur.closed || top.stamp != cur.stamp) continue;
        const i32 cx = region_.x + static_cast<i32>(top.key % region_.w), cy = region_.y + static_cast<i32>(top.key / region_.w);
        if (closest < 0 || st[static_cast<usize>(closest)].h > cur.h || (st[static_cast<usize>(closest)].h == cur.h && st[static_cast<usize>(closest)].g > cur.g)) closest = top.key;
        if (cx == tx && cy == ty) { found = true; break; }
        cur.closed = true;

        auto relax = [&](i32 nx, i32 ny, f32 stepCost) {
            State& ns = st[index(nx, ny)];
            if (ns.closed) return;
            const f32 g = cur.g + stepCost;
            if (ns.g != kInf && !(g < ns.g)) return;
            ns.g = g; ns.h = estimateCost(nx, ny, tx, ty);
            ns.px = cx; ns.py = cy; ns.hasPrev = true;
            ns.stamp++;
            open.push({g + ns.h, g, keyOf(nx, ny), ns.stamp});
        };

        if (!jumping_) {
            for (int d = 0; d < 8; ++d) {
                const i32 dx = dirsAll[d][0], dy = dirsAll[d][1];
                const i32 nx = cx + dx, ny = cy + dy;
                if (!walk(nx, ny)) continue;
                if (dx != 0 && dy != 0 && !canDiag(cx, cy, dx, dy)) continue;
                relax(nx, ny, computeCost(cx, cy, nx, ny) * weight_[index(nx, ny)]);
            }
        } else {
            const i32 bdx = cur.hasPrev ? (cx > cur.px) - (cx < cur.px) : 0;
            const i32 bdy = cur.hasPrev ? (cy > cur.py) - (cy < cur.py) : 0;
            for (int d = 0; d < 8; ++d) {
                const i32 dx = dirsAll[d][0], dy = dirsAll[d][1];
                if (dx != 0 && dy != 0 && diag_ == Never) continue;
                if (cur.hasPrev && dx == -bdx && dy == -bdy) continue;   // back where we came from
                i32 jx, jy;
                if (!jump(cx, cy, dx, dy, tx, ty, jx, jy)) continue;
                relax(jx, jy, computeCost(cx, cy, jx, jy));   // the weight scale does not apply to a jump
            }
        }
    }
    i64 endKey = found ? keyOf(tx, ty) : (partial ? closest : -1);
    if (endKey < 0) return false;
    std::vector<std::pair<i32, i32>> rev;
    i32 x = region_.x + static_cast<i32>(endKey % region_.w), y = region_.y + static_cast<i32>(endKey / region_.w);
    for (;;) {
        rev.push_back({x, y});
        const State& s = st[index(x, y)];
        if (!s.hasPrev) break;
        const i32 px = s.px, py = s.py;
        x = px; y = py;
    }
    std::reverse(rev.begin(), rev.end());
    path = std::move(rev);
    return found || partial;
}

std::vector<std::pair<i32, i32>> AStarGrid::idPath(i32 fx, i32 fy, i32 tx, i32 ty, bool allowPartial) const {
    std::vector<std::pair<i32, i32>> p;
    solve(fx, fy, tx, ty, allowPartial, p);
    return p;
}

std::vector<std::pair<f32, f32>> AStarGrid::pointPath(i32 fx, i32 fy, i32 tx, i32 ty, bool allowPartial) const {
    std::vector<std::pair<f32, f32>> out;
    for (const auto& c : idPath(fx, fy, tx, ty, allowPartial)) out.push_back(pointPosition(c.first, c.second));
    return out;
}

} // namespace aver::synapse
