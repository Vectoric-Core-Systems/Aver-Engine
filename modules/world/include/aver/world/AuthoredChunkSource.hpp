// A cooked authored level as a chunk source (LARGE_WORLD 2.4.6): the region files and the index under
// <project>/Binaries/Chunks/<level>/, and nothing else. No generator, no override layer: a chunk the cook did not write is empty.
//
// Scene-free, like the rest of the source types, so a tool can open a cooked set and read it.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/world/ChunkSource.hpp"
#include "aver/world/RegionIndex.hpp"

#include <string>
#include <vector>

namespace aver::world {

// Names inside a cook directory.
inline constexpr const char* kCookIndexName = "world.ocindex";
inline constexpr const char* kCookLevelName = "level.ocworld";

// The streamer's fixed padding for a cooked level: a chunk-owned placement reaches at most this many chunks beyond its owner
// chunk (the cook calls a root WIDE past that), so loading owner chunks this far beyond the load radius loads every placement
// that reaches into it. Fixed rather than stored: the cook clamps its wide threshold to it.
inline constexpr i32 kAuthoredPadChunks = 2;

class AuthoredChunkSource final : public IChunkSource {
public:
    // Opens `<cookDir>/world.ocindex`. False with `why` when it is missing, damaged, or not a cooked set (an overrides index).
    bool open(const std::string& cookDir, std::string* why = nullptr);
    void close();
    bool isOpen() const { return open_; }

    const std::string& dir() const { return dir_; }
    // <cookDir>/level.ocworld: the slim level (level-global records) the cook wrote beside the regions.
    std::string levelPath() const { return dir_ + "/" + kCookLevelName; }
    const RegionIndex& index() const { return src_.index(); }
    i32 chunkSizeCm() const { return src_.chunkSizeCm(); }
    u64 sourceHash() const { return src_.index().sourceHash; }
    u64 levelId() const { return src_.index().levelId; }
    // The oversize items the streamer keeps their owner chunks resident for (RegionIndex::wide).
    const std::vector<WideItem>& wide() const { return src_.index().wide; }

    bool has(const ChunkCoord& c) override { return open_ && src_.has(c); }
    bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) override { return open_ ? src_.load(c, out, why) : false; }
    void forEachOccupied(const ChunkBox& box, const std::function<void(const ChunkCoord&)>& fn) override {
        if (open_) src_.forEachOccupied(box, fn);
    }

    // Region handles and occupancy tables held (tests).
    usize openRegions() const { return src_.openRegions(); }
    usize occupancyTables() const { return src_.occupancyTables(); }

private:
    RegionChunkSource src_;
    std::string dir_;
    bool open_ = false;
};

} // namespace aver::world
