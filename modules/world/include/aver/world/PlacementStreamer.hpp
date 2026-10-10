// Distance residency for authored placements. Pure logic: no scene, GPU or physics dependency.
// See docs/LEVEL_STREAMING.md section 2.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <unordered_map>
#include <vector>

namespace aver::world {

using Aabb = aver::AABB;

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
    u32 add(const Aabb& b);
    void setBounds(u32 item, const Aabb& b);
    void remove(u32 item);
    void pin(u32 item, bool on);

    // Appends to toLoad (pinned first, then nearest first, <= loadBudget) and toEvict (furthest first).
    void update(const std::vector<Vec3>& viewers, std::vector<u32>& toLoad, std::vector<u32>& toEvict);
    void markLoaded(u32 item);
    void markEvicted(u32 item);
    bool resident(u32 item) const;
    usize residentCount() const { return residents_.size(); }

private:
    struct Item {
        Aabb b;
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

    PlacementStreamSettings s_;
    std::vector<Item> items_;
    std::unordered_map<u64, std::vector<u32>> cells_;
    std::vector<u32> residents_;
    std::vector<u32> pinned_;
    std::vector<u32> wide_;
    u32 stampCounter_ = 0;

    // Idle early-out state.
    std::vector<Vec3> lastViewers_;
    bool dirty_ = true;
    bool emitted_ = false;
};

} // namespace aver::world
