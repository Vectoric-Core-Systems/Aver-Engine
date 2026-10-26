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

AabbD widen(const Aabb& b) {
    return AabbD{Vec3d{b.min.x, b.min.y, b.min.z}, Vec3d{b.max.x, b.max.y, b.max.z}};
}
Aabb narrow(const AabbD& b) {
    return Aabb{Vec3{static_cast<f32>(b.min.x), static_cast<f32>(b.min.y), static_cast<f32>(b.min.z)},
                Vec3{static_cast<f32>(b.max.x), static_cast<f32>(b.max.y), static_cast<f32>(b.max.z)}};
}
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
    if (f64_) { insertCellsD(item); return; }
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
    if (f64_) { eraseCellsD(item); return; }
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
    f64_ = false;
    lastViewersD_.clear();
    items_.resize(bounds.size());
    for (u32 i = 0; i < bounds.size(); ++i) {
        items_[i].b = bounds[i];
        items_[i].bd = widen(bounds[i]);
        items_[i].alive = true;
        insertCells(i);
    }
    dirty_ = true; emitted_ = false;
}

u32 PlacementStreamer::add(const Aabb& b) {
    const u32 i = static_cast<u32>(items_.size());
    items_.emplace_back();
    items_[i].b = b;
    items_[i].bd = widen(b);
    items_[i].alive = true;
    insertCells(i);
    dirty_ = true;
    return i;
}

void PlacementStreamer::setBounds(u32 item, const Aabb& b) {
    if (item >= items_.size() || !items_[item].alive) return;
    eraseCells(item);
    items_[item].b = b;
    items_[item].bd = widen(b);
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

// ---- the f64 path -------------------------------------------------------------------------------------------------
// The same algorithm as the f32 functions above, on the exact f64 box and f64 viewers. Kept apart rather than templated so the
// f32 path stays untouched, which is what "identity at origin zero" asks of it.

cell_t PlacementStreamer::cellOfD(f64 v) const {
    return static_cast<cell_t>(std::floor(v / static_cast<f64>(s_.cellCm)));
}

f64 PlacementStreamer::distXYD(const AabbD& b, const Vec3d& p) {
    const f64 dx = std::max({b.min.x - p.x, 0.0, p.x - b.max.x});
    const f64 dy = std::max({b.min.y - p.y, 0.0, p.y - b.max.y});
    return std::sqrt(dx * dx + dy * dy);
}

f64 PlacementStreamer::nearestD(const AabbD& b, const std::vector<Vec3d>& v) const {
    f64 d = 3.0e38;
    for (const Vec3d& p : v) d = std::min(d, distXYD(b, p));
    return d;
}

void PlacementStreamer::insertCellsD(u32 item) {
    const AabbD& b = items_[item].bd;
    const f64 nx = (b.max.x - b.min.x) / s_.cellCm + 1.0, ny = (b.max.y - b.min.y) / s_.cellCm + 1.0;
    if (!std::isfinite(nx) || !std::isfinite(ny) || nx < 0.0 || ny < 0.0 || nx * ny > kMaxCells) {
        items_[item].wide = true;
        wide_.push_back(item);
        return;
    }
    items_[item].wide = false;
    const cell_t x0 = cellOfD(b.min.x), x1 = cellOfD(b.max.x), y0 = cellOfD(b.min.y), y1 = cellOfD(b.max.y);
    for (cell_t cy = y0; cy <= y1; ++cy)
        for (cell_t cx = x0; cx <= x1; ++cx) cells_[key(cx, cy)].push_back(item);
}

void PlacementStreamer::eraseCellsD(u32 item) {
    if (items_[item].wide) {
        wide_.erase(std::remove(wide_.begin(), wide_.end(), item), wide_.end());
        items_[item].wide = false;
        return;
    }
    const AabbD& b = items_[item].bd;
    const cell_t x0 = cellOfD(b.min.x), x1 = cellOfD(b.max.x), y0 = cellOfD(b.min.y), y1 = cellOfD(b.max.y);
    for (cell_t cy = y0; cy <= y1; ++cy)
        for (cell_t cx = x0; cx <= x1; ++cx) {
            auto it = cells_.find(key(cx, cy));
            if (it == cells_.end()) continue;
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), item), v.end());
            if (v.empty()) cells_.erase(it);
        }
}

void PlacementStreamer::build(const std::vector<AabbD>& bounds, const PlacementStreamSettings& s) {
    s_ = s;
    if (s_.cellCm < 1.0f) s_.cellCm = 1.0f;
    if (s_.evictCm <= s_.loadCm) s_.evictCm = s_.loadCm + s_.cellCm;   // hysteresis is not optional
    items_.clear(); cells_.clear(); residents_.clear(); pinned_.clear(); wide_.clear();
    lastViewers_.clear();
    lastViewersD_.clear();
    f64_ = true;
    items_.resize(bounds.size());
    for (u32 i = 0; i < bounds.size(); ++i) {
        items_[i].bd = bounds[i];
        items_[i].b = narrow(bounds[i]);
        items_[i].alive = true;
        insertCells(i);
    }
    dirty_ = true; emitted_ = false;
}

u32 PlacementStreamer::add(const AabbD& b) {
    const u32 i = static_cast<u32>(items_.size());
    items_.emplace_back();
    items_[i].bd = b;
    items_[i].b = narrow(b);
    items_[i].alive = true;
    insertCells(i);
    dirty_ = true;
    return i;
}

void PlacementStreamer::setBounds(u32 item, const AabbD& b) {
    if (item >= items_.size() || !items_[item].alive) return;
    eraseCells(item);
    items_[item].bd = b;
    items_[item].b = narrow(b);
    insertCells(item);
    dirty_ = true;
}

void PlacementStreamer::update(const std::vector<Vec3d>& viewers, std::vector<u32>& toLoad,
                               std::vector<u32>& toEvict) {
    // Idle: as the f32 update, the quarter-cell drift test in f64.
    if (!dirty_ && !emitted_ && viewers.size() == lastViewersD_.size()) {
        bool same = true;
        const f64 q = static_cast<f64>(s_.cellCm) * 0.25;
        for (usize i = 0; i < viewers.size() && same; ++i) {
            const Vec3d& a = viewers[i];
            const Vec3d& b = lastViewersD_[i];
            same = cellOfD(a.x) == cellOfD(b.x) && cellOfD(a.y) == cellOfD(b.y) &&
                   std::fabs(a.x - b.x) < q && std::fabs(a.y - b.y) < q;
        }
        if (same) return;
    }
    lastViewersD_ = viewers;
    dirty_ = false;
    emitted_ = false;

    const usize loadStart = toLoad.size();
    for (u32 i : pinned_)
        if (!items_[i].resident) toLoad.push_back(i);

    if (!viewers.empty()) {
        struct Cand { f64 d; u32 item; };
        std::vector<Cand> cand;
        if (++stampCounter_ == 0) {
            for (Item& it : items_) it.stamp = 0;
            stampCounter_ = 1;
        }
        const f64 loadCm = static_cast<f64>(s_.loadCm), evictCm = static_cast<f64>(s_.evictCm);
        for (const Vec3d& v : viewers) {
            const cell_t x0 = cellOfD(v.x - loadCm), x1 = cellOfD(v.x + loadCm);
            const cell_t y0 = cellOfD(v.y - loadCm), y1 = cellOfD(v.y + loadCm);
            for (cell_t cy = y0; cy <= y1; ++cy)
                for (cell_t cx = x0; cx <= x1; ++cx) {
                    auto it = cells_.find(key(cx, cy));
                    if (it == cells_.end()) continue;
                    for (u32 id : it->second) {
                        Item& m = items_[id];
                        if (m.stamp == stampCounter_) continue;
                        m.stamp = stampCounter_;
                        if (m.resident || m.pinned) continue;
                        const f64 d = nearestD(m.bd, viewers);
                        if (d <= loadCm) cand.push_back({d, id});
                    }
                }
        }
        for (u32 id : wide_) {
            const Item& m = items_[id];
            if (m.resident || m.pinned) continue;
            const f64 d = nearestD(m.bd, viewers);
            if (d <= loadCm) cand.push_back({d, id});
        }
        std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
            return a.d != b.d ? a.d < b.d : a.item < b.item;
        });
        const usize cap = s_.loadBudget ? s_.loadBudget : cand.size();
        for (usize i = 0; i < cand.size() && i < cap; ++i) toLoad.push_back(cand[i].item);

        std::vector<Cand> ev;
        for (u32 id : residents_) {
            const Item& m = items_[id];
            if (m.pinned) continue;
            const f64 d = nearestD(m.bd, viewers);
            if (d > evictCm) ev.push_back({d, id});
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
