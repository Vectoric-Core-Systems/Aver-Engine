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

#if AVER_MODULE_SCENE
#  include "aver/world/RegionFile.hpp"
#  include "aver/world/RegionIndex.hpp"

#  include <memory>
#  include <string>
#  include <vector>

namespace aver::world {

class IChunkSource {
public:
    virtual ~IChunkSource() = default;
    // Cheap existence. Must NOT read a payload -- the residency set asks this far more often than
    // it loads, and about chunks that mostly do not exist.
    virtual bool has(const ChunkCoord& c) = 0;
    // Fills `out`, including its coord and chunkSizeCm. False with `why` set on absence or damage.
    virtual bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) = 0;
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

    RegionIndex index_;
    std::string contentDir_;
    // LRU over open handles. A camera near a region corner touches up to 8, so the default is 8 --
    // enough that the common case never evicts, small enough that a long flight does not accumulate
    // file handles for regions it has left behind.
    std::vector<Open> open_;
    usize maxOpen_ = 8;
    u64 tick_ = 0;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
