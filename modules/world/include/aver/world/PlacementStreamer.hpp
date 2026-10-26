// Distance residency for authored placements. Pure logic: no scene, GPU or physics dependency.
// See docs/LEVEL_STREAMING.md section 2.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/world/WorldPos.hpp"

#include <initializer_list>
#include <unordered_map>
#include <vector>

namespace aver::world {

using Aabb = aver::AABB;

// The f64 ABSOLUTE-centimetre box the large-world path (LARGE_WORLD 2.4.1) keeps for a placement: an authored record is f64
// text, and an f32 box quantises it past ~10 km (1 cm at 100 km). Viewers on that path are f64 absolute too (the scene
// position + the floating origin, summed by the caller).
struct AabbD { Vec3d min, max; };

struct PlacementStreamSettings {
    f32 cellCm = 6400.0f;
    f32 loadCm = 25000.0f;
    f32 evictCm = 30000.0f;
    u32 loadBudget = 128;   // a batch past ~256 bodies rebuilds the whole physics broadphase
    u32 evictBudget = 2048;   // 0 = unlimited
};

class PlacementStreamer {
public:
    void build(const std::vector<Aabb>& bounds, const PlacementStreamSettings& s);
    // `build({}, s)` (LevelInstances.cpp) would be ambiguous between the f32 and f64 vectors; an empty list is an identity
    // conversion to this one, so it wins and the call stays what it was.
    void build(std::initializer_list<Aabb> bounds, const PlacementStreamSettings& s) { build(std::vector<Aabb>(bounds), s); }
    u32 add(const Aabb& b);
    void setBounds(u32 item, const Aabb& b);

    // ---- the f64 path (additive; the f32 API above computes exactly what it always did) ----
    // Building from f64 boxes puts the streamer in f64 mode: the grid, the distance tests and the idle early-out all run in
    // f64 against the exact box. In f32 mode (the default, and after an f32 build) these overloads narrow what they are given,
    // which is the old precision. add/setBounds keep both forms of the box, so either API can follow either build.
    void build(const std::vector<AabbD>& bounds, const PlacementStreamSettings& s);
    u32 add(const AabbD& b);
    void setBounds(u32 item, const AabbD& b);
    void update(const std::vector<Vec3d>& viewers, std::vector<u32>& toLoad, std::vector<u32>& toEvict);
    f64 distance(u32 item, const std::vector<Vec3d>& viewers) const { return nearestD(items_[item].bd, viewers); }
    bool f64Mode() const { return f64_; }

    void remove(u32 item);
    void pin(u32 item, bool on);

    // Appends to toLoad (pinned first, then nearest first, <= loadBudget) and toEvict (furthest first).
    void update(const std::vector<Vec3>& viewers, std::vector<u32>& toLoad, std::vector<u32>& toEvict);
    void markLoaded(u32 item);
    void markEvicted(u32 item);
    bool resident(u32 item) const;
    usize residentCount() const { return residents_.size(); }
    bool pinned(u32 item) const { return item < items_.size() && items_[item].pinned; }
    // Horizontal distance from the nearest viewer to the item's bounds (0 inside).
    f32 distance(u32 item, const std::vector<Vec3>& viewers) const { return nearest(items_[item].b, viewers); }
    const PlacementStreamSettings& settings() const { return s_; }
    // Load / unload distances changed in place (the cell grid stays); the next update applies them.
    void setDistances(f32 loadCm, f32 evictCm) {
        s_.loadCm = loadCm;
        s_.evictCm = evictCm > loadCm ? evictCm : loadCm + s_.cellCm;
        dirty_ = true;
    }

private:
    struct Item {
        Aabb b;
        AabbD bd;   // the same box in f64; equal to b widened unless the box came from the f64 API
        bool alive = false, resident = false, pinned = false;
        bool wide = false;   // spans more than kMaxCells cells (or unknown): checked directly, not gridded
        u32 resPos = 0;   // index in residents_ while resident
        u32 stamp = 0;    // dedup mark while visiting cells
    };

    std::int64_t cellOf(f32 v) const;
    static u64 key(std::int64_t cx, std::int64_t cy) { return (u64(std::uint32_t(cx)) << 32) | u64(std::uint32_t(cy)); }
    void insertCells(u32 item);
    void eraseCells(u32 item);
    static f32 distXY(const Aabb& b, const Vec3& p);
    f32 nearest(const Aabb& b, const std::vector<Vec3>& v) const;
    std::int64_t cellOfD(f64 v) const;
    static f64 distXYD(const AabbD& b, const Vec3d& p);
    f64 nearestD(const AabbD& b, const std::vector<Vec3d>& v) const;
    void insertCellsD(u32 item);
    void eraseCellsD(u32 item);

    PlacementStreamSettings s_;
    std::vector<Item> items_;
    std::unordered_map<u64, std::vector<u32>> cells_;
    std::vector<u32> residents_;
    std::vector<u32> pinned_;
    std::vector<u32> wide_;
    u32 stampCounter_ = 0;

    bool f64_ = false;

    // Idle early-out state.
    std::vector<Vec3> lastViewers_;
    std::vector<Vec3d> lastViewersD_;
    bool dirty_ = true;
    bool emitted_ = false;
};

} // namespace aver::world
