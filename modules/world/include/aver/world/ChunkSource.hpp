// Where a chunk's contents come from.
//
// AN INTERFACE, so the residency machinery can be tested with no files at all. That is not a
// convenience: streaming logic and file I/O fail in completely different ways, and a bug in "which
// chunks should be resident" is much easier to find when nothing on disk is involved. Slice 8's
// generate-as-you-go becomes another implementation of this and nothing above it changes.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayload.hpp"

#include "aver/world/RegionFile.hpp"
#include "aver/world/RegionIndex.hpp"

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Not scene-gated: a source hands out the POD half of ChunkPayload.hpp, so the cook and its tests need no Aver.Scene.
namespace aver::world {

// An inclusive box of chunk coordinates. An axis left at kChunkAxisMin..kChunkAxisMax is unbounded (the column scan uses it for Z).
inline constexpr i32 kChunkAxisMin = std::numeric_limits<i32>::min();
inline constexpr i32 kChunkAxisMax = std::numeric_limits<i32>::max();
struct ChunkBox {
    ChunkCoord min{kChunkAxisMin, kChunkAxisMin, kChunkAxisMin};
    ChunkCoord max{kChunkAxisMax, kChunkAxisMax, kChunkAxisMax};
};

class IChunkSource {
public:
    virtual ~IChunkSource() = default;
    // Cheap existence. Must NOT read a payload -- the residency set asks this far more often than
    // it loads, and about chunks that mostly do not exist.
    virtual bool has(const ChunkCoord& c) = 0;
    // Fills `out`, including its coord and chunkSizeCm. False with `why` set on absence or damage.
    virtual bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) = 0;

    // Calls `fn` for every chunk of `box` that has() is true for. THE COLUMN-OCCUPANCY QUERY: with it the streamer finds a tower
    // whose floors span chunk layers -1..31 without scanning a Z slab. The default asks has() of every cell, which is right for a
    // test and wrong for a world (a box over a few million cells is refused with a warning); a source that knows what it holds
    // overrides it. `fn` must not call back into the source. Order is ascending (y, x, z) per region the source holds.
    virtual void forEachOccupied(const ChunkBox& box, const std::function<void(const ChunkCoord&)>& fn);
};

// A source backed by an `.ocindex` and the `.avrgn` files it names.
class RegionChunkSource final : public IChunkSource {
public:
    // `contentDir` is where the index's relative paths resolve against.
    bool open(const std::string& indexPath, const std::string& contentDir, std::string* why = nullptr);

    const RegionIndex& index() const { return index_; }
    i32 chunkSizeCm() const { return index_.chunkSizeCm; }

    bool has(const ChunkCoord& c) override;
    bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) override;

    // The region sources hold their own occupancy: each region's chunk list is read once (RegionFile::chunks) and kept sorted by
    // (y, x, z), so a column query is a binary search per row instead of a probe per cell. At most kMaxOccupancyRegions regions
    // are kept (least recently used out first).
    void forEachOccupied(const ChunkBox& box, const std::function<void(const ChunkCoord&)>& fn) override;
    usize occupancyTables() const { return occ_.size(); }
    static constexpr usize kMaxOccupancyRegions = 16;

    // How many region files are held open. Bounded by `maxOpenRegions`.
    usize openRegions() const { return open_.size(); }
    void setMaxOpenRegions(usize n) { maxOpen_ = n < 1 ? 1 : n; }

private:
    struct Open {
        RegionCoord coord;
        std::unique_ptr<RegionFile> file;
        u64 lastUsed = 0;
    };
    // Opens (or finds) the region for `c`. Null when the index does not list it.
    RegionFile* regionFor(const RegionCoord& r, std::string* why);

    struct Occupancy {
        RegionCoord coord;
        std::vector<ChunkCoord> cells;   // sorted by (y, x, z)
        u64 lastUsed = 0;
    };
    // The occupancy table of `r` (built on first use), or null when the region is not listed or cannot be read.
    const Occupancy* occupancyFor(const RegionCoord& r);

    RegionIndex index_;
    std::string contentDir_;
    // LRU over open handles. A camera near a region corner touches up to 8, so the default is 8 --
    // enough that the common case never evicts, small enough that a long flight does not accumulate
    // file handles for regions it has left behind.
    std::vector<Open> open_;
    usize maxOpen_ = 8;
    u64 tick_ = 0;
    std::vector<Occupancy> occ_;
};

} // namespace aver::world
