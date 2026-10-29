#pragma once
// Generic A* graphs with Godot's semantics (docs/plans/fabric/G5_PARITY.md, N1): an arbitrary point graph (AStar3D; AStar2D is the
// same graph with z = 0) and a rectangular grid (AStarGrid2D) with diagonal modes, heuristics, weights and jump-point search.
// Scripts override the cost and the estimate, so both classes take callables; the defaults are Godot's.
//
// PURE. Ids are 64-bit for the graph and (x, y) cell pairs for the grid.
#include "aver/core/Math.hpp"

#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace aver::synapse {

class AStarGraph {
public:
    using CostFn = std::function<f32(i64 from, i64 to)>;

    void addPoint(i64 id, const Vec3& pos, f32 weightScale = 1.0f);   // an existing id keeps its connections
    bool hasPoint(i64 id) const { return pts_.count(id) != 0; }
    Vec3 pointPosition(i64 id) const;
    f32  pointWeight(i64 id) const;
    void setPointPosition(i64 id, const Vec3& pos);
    void setPointWeight(i64 id, f32 w);
    void removePoint(i64 id);
    void setDisabled(i64 id, bool disabled);
    bool isDisabled(i64 id) const;
    usize pointCount() const { return pts_.size(); }
    std::vector<i64> pointIds() const;
    i64 nextFreeId() const;
    void clear() { pts_.clear(); }

    // A segment. One-way (bidirectional false) adds only id -> to; an existing reverse direction stays.
    bool connect(i64 id, i64 to, bool bidirectional = true);
    void disconnect(i64 id, i64 to, bool bidirectional = true);
    // bidirectional true: a segment exists in either direction; false: id -> to is possible.
    bool areConnected(i64 id, i64 to, bool bidirectional = true) const;
    std::vector<i64> connections(i64 id) const;   // outgoing, ascending

    i64  closestPoint(const Vec3& to, bool includeDisabled = false) const;   // -1 for none; ties: the smallest id
    Vec3 closestPositionInSegment(const Vec3& to) const;

    // Empty when a point is missing or no route exists (unless allowPartial: then the way to the closest point reached).
    std::vector<i64>  idPath(i64 from, i64 to, bool allowPartial = false) const;
    std::vector<Vec3> pointPath(i64 from, i64 to, bool allowPartial = false) const;

    void setComputeCost(CostFn f) { compute_ = std::move(f); }
    void setEstimateCost(CostFn f) { estimate_ = std::move(f); }

private:
    struct Pt {
        Vec3 pos;
        f32 weight = 1.0f;
        bool disabled = false;
        std::set<i64> out;   // outgoing neighbours
    };
    std::map<i64, Pt> pts_;
    CostFn compute_, estimate_;
    bool solve(i64 from, i64 to, bool partial, std::vector<i64>& path) const;
};

class AStarGrid {
public:
    enum Heuristic : i32 { Euclidean = 0, Manhattan = 1, Octile = 2, Chebyshev = 3 };
    enum Diagonal : i32 { Always = 0, Never = 1, AtLeastOneWalkable = 2, OnlyIfNoObstacles = 3 };
    using CostFn = std::function<f32(i32 fx, i32 fy, i32 tx, i32 ty)>;

    struct Rect { i32 x = 0, y = 0, w = 0, h = 0; };

    void setRegion(const Rect& r) { region_ = r; dirty_ = true; }
    Rect region() const { return region_; }
    void setCellSize(f32 x, f32 y) { cell_[0] = x; cell_[1] = y; dirty_ = true; }
    void setOffset(f32 x, f32 y) { off_[0] = x; off_[1] = y; dirty_ = true; }
    void setDiagonalMode(Diagonal d) { diag_ = d; }
    Diagonal diagonalMode() const { return diag_; }
    void setComputeHeuristic(Heuristic h) { hCompute_ = h; }
    void setEstimateHeuristic(Heuristic h) { hEstimate_ = h; }
    Heuristic computeHeuristic() const { return hCompute_; }
    Heuristic estimateHeuristic() const { return hEstimate_; }
    void setJumping(bool on) { jumping_ = on; }
    bool jumping() const { return jumping_; }
    bool isDirty() const { return dirty_; }
    void update();                       // allocates the cells; clears solidity and weights
    bool inBounds(i32 x, i32 y) const { return x >= region_.x && y >= region_.y && x < region_.x + region_.w && y < region_.y + region_.h; }

    void setSolid(i32 x, i32 y, bool solid);
    bool isSolid(i32 x, i32 y) const;
    void fillSolid(const Rect& r, bool solid);
    void setWeight(i32 x, i32 y, f32 w);
    f32  weight(i32 x, i32 y) const;
    void fillWeight(const Rect& r, f32 w);
    void clear();

    std::vector<std::pair<i32, i32>> idPath(i32 fx, i32 fy, i32 tx, i32 ty, bool allowPartial = false) const;
    std::vector<std::pair<f32, f32>> pointPath(i32 fx, i32 fy, i32 tx, i32 ty, bool allowPartial = false) const;
    std::pair<f32, f32> pointPosition(i32 x, i32 y) const { return {off_[0] + static_cast<f32>(x) * cell_[0], off_[1] + static_cast<f32>(y) * cell_[1]}; }

    void setComputeCost(CostFn f) { compute_ = std::move(f); }
    void setEstimateCost(CostFn f) { estimate_ = std::move(f); }

    static f32 heuristic(Heuristic h, i32 dx, i32 dy);

private:
    Rect region_;
    f32 cell_[2] = {1.0f, 1.0f}, off_[2] = {0.0f, 0.0f};
    Diagonal diag_ = Always;
    Heuristic hCompute_ = Euclidean, hEstimate_ = Euclidean;
    bool jumping_ = false, dirty_ = true;
    std::vector<unsigned char> solid_;
    std::vector<f32> weight_;
    CostFn compute_, estimate_;

    usize index(i32 x, i32 y) const { return static_cast<usize>(y - region_.y) * static_cast<usize>(region_.w) + static_cast<usize>(x - region_.x); }
    bool walk(i32 x, i32 y) const { return inBounds(x, y) && !solid_[index(x, y)]; }
    bool canDiag(i32 x, i32 y, i32 dx, i32 dy) const;
    f32 computeCost(i32 fx, i32 fy, i32 tx, i32 ty) const;
    f32 estimateCost(i32 fx, i32 fy, i32 tx, i32 ty) const;
    bool jump(i32 x, i32 y, i32 dx, i32 dy, i32 gx, i32 gy, i32& ox, i32& oy) const;
    bool solve(i32 fx, i32 fy, i32 tx, i32 ty, bool partial, std::vector<std::pair<i32, i32>>& path) const;
};

} // namespace aver::synapse
