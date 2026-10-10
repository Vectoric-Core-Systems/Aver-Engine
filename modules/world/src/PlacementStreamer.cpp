#include "aver/world/PlacementStreamer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aver::world {

using cell_t = std::int64_t;

namespace {
// An item over this many cells (a whole-level terrain, or unknown bounds at +-1e9 cm) is not gridded:
// it would cost a list entry per cell, and there are few such items to test directly.
constexpr f64 kMaxCells = 4096.0;
}

cell_t PlacementStreamer::cellOf(f32 v) const {
    return static_cast<cell_t>(std::floor(v / s_.cellCm));
}

f32 PlacementStreamer::distXY(const Aabb& b, const Vec3& p) {
    const f32 dx = std::max({b.min.x - p.x, 0.0f, p.x - b.max.x});
    const f32 dy = std::max({b.min.y - p.y, 0.0f, p.y - b.max.y});
    return std::sqrt(dx * dx + dy * dy);
}

f32 PlacementStreamer::nearest(const Aabb& b, const std::vector<Vec3>& v) const {
    f32 d = 3.0e38f;
    for (const Vec3& p : v) d = std::min(d, distXY(b, p));
    return d;
}

void PlacementStreamer::insertCells(u32 item) {
    const Aabb& b = items_[item].b;
    const f64 nx = (f64(b.max.x) - f64(b.min.x)) / s_.cellCm + 1.0, ny = (f64(b.max.y) - f64(b.min.y)) / s_.cellCm + 1.0;
    if (!std::isfinite(nx) || !std::isfinite(ny) || nx < 0.0 || ny < 0.0 || nx * ny > kMaxCells) {
        items_[item].wide = true;
        wide_.push_back(item);
        return;
    }
    items_[item].wide = false;
    const cell_t x0 = cellOf(b.min.x), x1 = cellOf(b.max.x), y0 = cellOf(b.min.y), y1 = cellOf(b.max.y);
    for (cell_t cy = y0; cy <= y1; ++cy)
        for (cell_t cx = x0; cx <= x1; ++cx) cells_[key(cx, cy)].push_back(item);
}

void PlacementStreamer::eraseCells(u32 item) {
    if (items_[item].wide) {
        wide_.erase(std::remove(wide_.begin(), wide_.end(), item), wide_.end());
        items_[item].wide = false;
        return;
    }
    const Aabb& b = items_[item].b;
    const cell_t x0 = cellOf(b.min.x), x1 = cellOf(b.max.x), y0 = cellOf(b.min.y), y1 = cellOf(b.max.y);
    for (cell_t cy = y0; cy <= y1; ++cy)
        for (cell_t cx = x0; cx <= x1; ++cx) {
            auto it = cells_.find(key(cx, cy));
            if (it == cells_.end()) continue;
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), item), v.end());
            if (v.empty()) cells_.erase(it);
        }
}

void PlacementStreamer::build(const std::vector<Aabb>& bounds, const PlacementStreamSettings& s) {
    s_ = s;
    if (s_.cellCm < 1.0f) s_.cellCm = 1.0f;
    if (s_.evictCm <= s_.loadCm) s_.evictCm = s_.loadCm + s_.cellCm;   // hysteresis is not optional
    items_.clear(); cells_.clear(); residents_.clear(); pinned_.clear(); wide_.clear();
    lastViewers_.clear();
    items_.resize(bounds.size());
    for (u32 i = 0; i < bounds.size(); ++i) {
        items_[i].b = bounds[i];
        items_[i].alive = true;
        insertCells(i);
    }
    dirty_ = true; emitted_ = false;
}

u32 PlacementStreamer::add(const Aabb& b) {
    const u32 i = static_cast<u32>(items_.size());
    items_.emplace_back();
    items_[i].b = b;
    items_[i].alive = true;
    insertCells(i);
    dirty_ = true;
    return i;
}

void PlacementStreamer::setBounds(u32 item, const Aabb& b) {
    if (item >= items_.size() || !items_[item].alive) return;
    eraseCells(item);
    items_[item].b = b;
    insertCells(item);
    dirty_ = true;
}

void PlacementStreamer::remove(u32 item) {
    if (item >= items_.size() || !items_[item].alive) return;
    eraseCells(item);
    if (items_[item].resident) markEvicted(item);
    if (items_[item].pinned) pin(item, false);
    items_[item].alive = false;
    dirty_ = true;
}

void PlacementStreamer::pin(u32 item, bool on) {
    if (item >= items_.size() || !items_[item].alive || items_[item].pinned == on) return;
    items_[item].pinned = on;
    if (on) pinned_.push_back(item);
    else pinned_.erase(std::remove(pinned_.begin(), pinned_.end(), item), pinned_.end());
    dirty_ = true;
}

void PlacementStreamer::markLoaded(u32 item) {
    if (item >= items_.size() || !items_[item].alive || items_[item].resident) return;
    items_[item].resident = true;
    items_[item].resPos = static_cast<u32>(residents_.size());
    residents_.push_back(item);
    dirty_ = true;
}

void PlacementStreamer::markEvicted(u32 item) {
    if (item >= items_.size() || !items_[item].resident) return;
    const u32 pos = items_[item].resPos;
    const u32 last = residents_.back();
    residents_[pos] = last;
    items_[last].resPos = pos;
    residents_.pop_back();
    items_[item].resident = false;
    dirty_ = true;
}

bool PlacementStreamer::resident(u32 item) const {
    return item < items_.size() && items_[item].resident;
}

void PlacementStreamer::update(const std::vector<Vec3>& viewers, std::vector<u32>& toLoad,
                               std::vector<u32>& toEvict) {
    // Idle: nothing changed, the last call asked for nothing, and no viewer left its cell or moved a
    // quarter cell (distance tests are exact, so small drift is re-checked only occasionally).
    if (!dirty_ && !emitted_ && viewers.size() == lastViewers_.size()) {
        bool same = true;
        const f32 q = s_.cellCm * 0.25f;
        for (usize i = 0; i < viewers.size() && same; ++i) {
            const Vec3& a = viewers[i];
            const Vec3& b = lastViewers_[i];
            same = cellOf(a.x) == cellOf(b.x) && cellOf(a.y) == cellOf(b.y) &&
                   std::fabs(a.x - b.x) < q && std::fabs(a.y - b.y) < q;
        }
        if (same) return;
    }
    lastViewers_ = viewers;
    dirty_ = false;
    emitted_ = false;

    // Pinned and not resident: loaded first, ignoring distance and budget.
    const usize loadStart = toLoad.size();
    for (u32 i : pinned_)
        if (!items_[i].resident) toLoad.push_back(i);

    if (!viewers.empty()) {
        struct Cand { f32 d; u32 item; };
        std::vector<Cand> cand;
        if (++stampCounter_ == 0) {
            for (Item& it : items_) it.stamp = 0;
            stampCounter_ = 1;
        }
        for (const Vec3& v : viewers) {
            const cell_t x0 = cellOf(v.x - s_.loadCm), x1 = cellOf(v.x + s_.loadCm);
            const cell_t y0 = cellOf(v.y - s_.loadCm), y1 = cellOf(v.y + s_.loadCm);
            for (cell_t cy = y0; cy <= y1; ++cy)
                for (cell_t cx = x0; cx <= x1; ++cx) {
                    auto it = cells_.find(key(cx, cy));
                    if (it == cells_.end()) continue;
                    for (u32 id : it->second) {
                        Item& m = items_[id];
                        if (m.stamp == stampCounter_) continue;
                        m.stamp = stampCounter_;
                        if (m.resident || m.pinned) continue;
                        const f32 d = nearest(m.b, viewers);
                        if (d <= s_.loadCm) cand.push_back({d, id});
                    }
                }
        }
        for (u32 id : wide_) {
            const Item& m = items_[id];
            if (m.resident || m.pinned) continue;
            const f32 d = nearest(m.b, viewers);
            if (d <= s_.loadCm) cand.push_back({d, id});
        }
        std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
            return a.d != b.d ? a.d < b.d : a.item < b.item;
        });
        const usize cap = s_.loadBudget ? s_.loadBudget : cand.size();
        for (usize i = 0; i < cand.size() && i < cap; ++i) toLoad.push_back(cand[i].item);

        // A resident outside every viewer's range is far by construction, so only the resident
        // list is scanned for eviction.
        std::vector<Cand> ev;
        for (u32 id : residents_) {
            const Item& m = items_[id];
            if (m.pinned) continue;
            const f32 d = nearest(m.b, viewers);
            if (d > s_.evictCm) ev.push_back({d, id});
        }
        std::sort(ev.begin(), ev.end(), [](const Cand& a, const Cand& b) {
            return a.d != b.d ? a.d > b.d : a.item < b.item;
        });
        const usize ecap = s_.evictBudget ? s_.evictBudget : ev.size();
        for (usize i = 0; i < ev.size() && i < ecap; ++i) toEvict.push_back(ev[i].item);
        emitted_ = !ev.empty();
    }
    emitted_ = emitted_ || toLoad.size() > loadStart;
}

} // namespace aver::world
