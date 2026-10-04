// `.ocindex` -- which regions a level has, without opening any of them.
//
// FOUR JOBS, each of which justifies the file existing separately from the regions themselves:
//
//   1. EXISTENCE WITHOUT I/O. "Is there anything at region (12,-3,0)?" answered from one resident
//      structure. With regions 16 km across a streamer asks this constantly and must not pay an
//      open() to be told no.
//   2. STALENESS. Each entry carries the region's own contentHash. A mismatch means the index is
//      stale -- refuse to stream and say so, rather than streaming a lie.
//   3. THE CHUNK SIZE LIVES HERE, ONCE. This is the file that makes "one chunk size per level"
//      enforceable rather than conventional.
//   4. GENERATOR SKEW. A world that generates as you move has regions produced by SOME generator;
//      `generatorVersion` makes "these regions were made by an older one" detectable instead of
//      silently mixing two worlds together.
//
// WHOLE-FILE, unlike a region. It is small, always rewritten entirely, and must be rebuildable from
// scratch by scanning the region files -- so a corrupt index is recoverable rather than fatal.
//
// SORTED LEXICOGRAPHICALLY BY (x, y, z), not by Morton code as docs/CHUNKS.md first proposed. Morton
// buys spatial locality in the FILE, which matters when entries are paged; this index is fully
// resident, so it buys nothing here -- and a Morton key over three signed 32-bit axes is 96 bits,
// which is real complexity for no gain. Lexicographic is just as binary-searchable and just as
// deterministic, which are the two properties actually needed.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/world/ChunkCoord.hpp"

#include <string>
#include <vector>

namespace aver::world {

inline constexpr u32 kIndexMagic   = 0x58444943u;   // 'CIDX' little-endian
inline constexpr u32 kIndexVersion = 1;

struct RegionEntry {
    RegionCoord coord;
    u64 contentHash = 0;      // must equal the region header's own
    u32 chunkCount = 0;
    std::string relativePath;
};

struct RegionIndex {
    u64 levelId = 0;
    i32 chunkSizeCm = kDefaultChunkSizeCm;   // THE authority for this level
    u64 worldSeed = 0;
    u32 generatorVersion = 0;
    // Sorted by (x, y, z). Kept sorted by add()/sort(), which is what lets find() binary-search.
    std::vector<RegionEntry> regions;
    RegionCoord minRegion, maxRegion;        // meaningless when regions is empty

    // Adds or replaces the entry for `e.coord`, keeping the order. Returns true if it replaced one.
    bool add(const RegionEntry& e);
    // The entry for `c`, or nullptr.
    const RegionEntry* find(const RegionCoord& c) const;
    // Recomputes the bounds from the entries. Called by add(); exposed for a hand-built index.
    void recomputeBounds();
};

// Serialises to bytes. Deterministic: fixed order, no padding, no timestamps.
std::vector<u8> encodeIndex(const RegionIndex& ix);
bool decodeIndex(const u8* data, usize size, RegionIndex& out, std::string* why = nullptr);

bool writeIndex(const std::string& path, const RegionIndex& ix, std::string* why = nullptr);
bool readIndex(const std::string& path, RegionIndex& out, std::string* why = nullptr);

} // namespace aver::world
