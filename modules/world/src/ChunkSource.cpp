#include "aver/world/ChunkSource.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>

namespace aver::world {
namespace {

// Rows of the occupancy table: (y, x, z), so one row of a column query is a contiguous run.
bool occBefore(const ChunkCoord& a, const ChunkCoord& b) {
    if (a.y != b.y) return a.y < b.y;
    if (a.x != b.x) return a.x < b.x;
    return a.z < b.z;
}

// The default scan asks has() of every cell; past this many cells it is the wrong tool and says so.
constexpr f64 kMaxScanCells = 4.0e6;

} // namespace

void IChunkSource::forEachOccupied(const ChunkBox& box, const std::function<void(const ChunkCoord&)>& fn) {
    const f64 nx = static_cast<f64>(box.max.x) - static_cast<f64>(box.min.x) + 1.0;
    const f64 ny = static_cast<f64>(box.max.y) - static_cast<f64>(box.min.y) + 1.0;
    const f64 nz = static_cast<f64>(box.max.z) - static_cast<f64>(box.min.z) + 1.0;
    if (nx <= 0.0 || ny <= 0.0 || nz <= 0.0) return;
    if (nx * ny * nz > kMaxScanCells) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            AVER_WARN("[Chunk] forEachOccupied over {:.0f} cells through has(): this source does not know what it holds, so the query "
                      "is refused; give it an occupancy table (RegionChunkSource has one)", nx * ny * nz);
        }
        return;
    }
    for (i32 y = box.min.y;; ++y) {
        for (i32 x = box.min.x;; ++x) {
            for (i32 z = box.min.z;; ++z) {
                const ChunkCoord c{x, y, z};
                if (has(c)) fn(c);
                if (z == box.max.z) break;
            }
            if (x == box.max.x) break;
        }
        if (y == box.max.y) break;
    }
}

bool RegionChunkSource::open(const std::string& indexPath, const std::string& contentDir,
                             std::string* why) {
    open_.clear();
    occ_.clear();
    contentDir_ = contentDir;
    if (!readIndex(indexPath, index_, why)) return false;
    // The index is the authority on chunk size (docs/CHUNKS.md section 6, job 3), and decodeIndex
    // has already refused an invalid one -- so anything above this can take it as given.
    return true;
}

RegionFile* RegionChunkSource::regionFor(const RegionCoord& r, std::string* why) {
    ++tick_;
    for (Open& o : open_)
        if (o.coord == r) { o.lastUsed = tick_; return o.file.get(); }

    // Not listed is NOT an error: most of a world is empty, and answering "no" without touching the
    // disk is the whole reason the index exists.
    const RegionEntry* e = index_.find(r);
    if (!e) return nullptr;

    auto f = std::make_unique<RegionFile>();
    if (!f->open(contentDir_ + "/" + e->relativePath, why)) return nullptr;

    // STALENESS, checked here rather than trusted. A region whose own header disagrees with what the
    // index recorded means the index is out of date; streaming it anyway would serve content the
    // caller believes is something else. Refusing is the honest answer -- see section 6, job 2.
    if (f->header().contentHash != e->contentHash) {
        if (why) *why = e->relativePath + ": content hash disagrees with the index -- the index is stale";
        return nullptr;
    }

    if (open_.size() >= maxOpen_) {
        auto oldest = std::min_element(open_.begin(), open_.end(),
                                       [](const Open& a, const Open& b) { return a.lastUsed < b.lastUsed; });
        open_.erase(oldest);
    }
    open_.push_back(Open{r, std::move(f), tick_});
    return open_.back().file.get();
}

bool RegionChunkSource::has(const ChunkCoord& c) {
    RegionFile* f = regionFor(regionOf(c), nullptr);
    return f && f->hasChunk(localOf(c));
}

bool RegionChunkSource::load(const ChunkCoord& c, ChunkPayload& out, std::string* why) {
    RegionFile* f = regionFor(regionOf(c), why);
    if (!f) {
        if (why && why->empty()) *why = "no region file covers that chunk";
        return false;
    }
    return f->readChunk(localOf(c), out, why);
}

const RegionChunkSource::Occupancy* RegionChunkSource::occupancyFor(const RegionCoord& r) {
    ++tick_;
    for (Occupancy& o : occ_)
        if (o.coord == r) { o.lastUsed = tick_; return &o; }

    std::string why;
    RegionFile* f = regionFor(r, &why);
    if (!f) {
        if (!why.empty()) AVER_WARN("[Chunk] region ({},{},{}) could not be opened for its occupancy: {}", r.x, r.y, r.z, why);
        return nullptr;
    }
    why.clear();
    const std::vector<ChunkLocal> locals = f->chunks(&why);
    // A damaged group directory is named and skipped by chunks(); what is left is still served.
    if (!why.empty()) AVER_WARN("[Chunk] region ({},{},{}) occupancy is incomplete: {}", r.x, r.y, r.z, why);
    Occupancy o;
    o.coord = r;
    o.lastUsed = tick_;
    o.cells.reserve(locals.size());
    for (const ChunkLocal& l : locals) o.cells.push_back(chunkOf(r, l));
    std::sort(o.cells.begin(), o.cells.end(), occBefore);

    if (occ_.size() >= kMaxOccupancyRegions) {
        auto oldest = std::min_element(occ_.begin(), occ_.end(),
                                       [](const Occupancy& a, const Occupancy& b) { return a.lastUsed < b.lastUsed; });
        occ_.erase(oldest);
    }
    occ_.push_back(std::move(o));
    return &occ_.back();
}

void RegionChunkSource::forEachOccupied(const ChunkBox& box, const std::function<void(const ChunkCoord&)>& fn) {
    if (box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z) return;
    // A copy of the listed regions' coordinates: occupancyFor may reorder nothing here, but opening a region can evict a handle.
    std::vector<RegionCoord> hit;
    for (const RegionEntry& e : index_.regions) {
        bool overlap = true;
        const i64 lo[3] = {static_cast<i64>(kChunksPerRegionAxis) * e.coord.x + kChunkLocalMin,
                           static_cast<i64>(kChunksPerRegionAxis) * e.coord.y + kChunkLocalMin,
                           static_cast<i64>(kChunksPerRegionAxis) * e.coord.z + kChunkLocalMin};
        const i64 bmin[3] = {box.min.x, box.min.y, box.min.z};
        const i64 bmax[3] = {box.max.x, box.max.y, box.max.z};
        for (int a = 0; a < 3; ++a)
            if (bmax[a] < lo[a] || bmin[a] > lo[a] + (kChunksPerRegionAxis - 1)) { overlap = false; break; }
        if (overlap) hit.push_back(e.coord);
    }
    for (const RegionCoord& r : hit) {
        const Occupancy* o = occupancyFor(r);
        if (!o) continue;
        const std::vector<ChunkCoord>& cells = o->cells;
        auto it = std::lower_bound(cells.begin(), cells.end(), ChunkCoord{box.min.x, box.min.y, kChunkAxisMin}, occBefore);
        while (it != cells.end() && it->y <= box.max.y) {
            if (it->x < box.min.x) {   // left of the box on this row: jump to the box's first column
                it = std::lower_bound(it, cells.end(), ChunkCoord{box.min.x, it->y, kChunkAxisMin}, occBefore);
                continue;
            }
            if (it->x > box.max.x) {   // right of it: on to the next row
                if (it->y == kChunkAxisMax) break;
                it = std::lower_bound(it, cells.end(), ChunkCoord{box.min.x, it->y + 1, kChunkAxisMin}, occBefore);
                continue;
            }
            if (it->z >= box.min.z && it->z <= box.max.z) fn(*it);
            ++it;
        }
    }
}

} // namespace aver::world

