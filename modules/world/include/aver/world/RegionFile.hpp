// The `.avrgn` region archive: one file per 1024³ chunks, seekable, checksummed, crash-detectable.
//
// NOT AN AVR1 CONTAINER, and this is a decision rather than an oversight. AVR1 is an excellent
// IMMUTABLE asset container and a bad mutable one, in two ways that are structural:
//   * it pins its directory at offset 64 with payloads immediately after (Avr1.cpp:162-175), so
//     growing the directory by one entry displaces every payload in the file;
//   * parseAvr1 COPIES and xxHash64-verifies every payload on open (Avr1.cpp:280-282), so reading
//     one 16 m chunk out of a 16 km region costs a full read and hash of the whole archive.
// Both are fine for a mesh and disqualifying for an archive you seek into and later mutate.
//
// LAYOUT (4 KiB sectors throughout):
//
//   sector 0    header, copy A          } identical fields, independent CRCs, MONOTONIC `serial`.
//   sector 1    header, copy B          } The reader takes the higher serial that passes its CRC.
//   sector 2..  group table             sparse, sorted by groupIndex: {u32 group, u32 dirSector}
//               group directories       dense 16³ = 4096 slots x 12 bytes = 48 KiB = 12 sectors
//                                       each: {u32 firstSector, u32 byteLength, u32 crc32c}
//               free list               {u32 firstSector, u32 sectorCount}, empty in a cooked file
//               payloads                sector-aligned
//
// WHY TWO LEVELS. 1024³ is 1 073 741 824 chunk slots. A dense table is 8 GiB and a flat sparse
// directory for a fully-explored surface region is ~128 MiB -- neither can be held. A sparse table of
// 64³ groups (~128 KiB resident for a real region) over dense 16³ directories (48 KiB, read on
// demand) is Anvil's trick applied one level down, and the 6+4 bit split it needs is exactly the one
// ChunkCoord.hpp already proves is lossless.
//
// WHY TWO HEADERS. Anvil has one and is famous for corrupting on power loss. Writing the payload
// first, then the directory entry, then a header with `serial + 1` means a crash at any point leaves
// the previous serial intact and loses at most the one chunk in flight. Slice 5 only cooks, so
// nothing mutates yet -- but the field has to be in the format from the start, because adding it
// later means every file already written has no second copy to fall back to.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayload.hpp"

#if AVER_MODULE_SCENE
#  include <string>
#  include <vector>

namespace aver::world {

inline constexpr u32 kRegionMagic       = 0x4E475241u;   // 'ARGN' little-endian
// VERSION 2 moved the group table and the free list from fixed positions into header fields.
//
// v1 pinned the group table at sector 2 with the directories immediately after -- which is exactly
// the mistake that disqualified AVR1 for this job, made again one level up: adding a chunk to a
// group that does not exist yet needs a NEW directory and a LONGER group table, and a table that
// cannot move displaces every directory behind it. It went unnoticed while the format was cook-only,
// because a cook lays the whole file out at once and nothing ever grows. Runtime writes are what
// made it matter.
//
// No v1 file exists outside a test's temp directory, so the bump simply refuses them rather than
// carrying a compatibility path for content nobody has.
inline constexpr u32 kRegionVersion     = 2;
inline constexpr u32 kRegionSectorBytes = 4096;
inline constexpr u32 kGroupDirEntryBytes = 12;
inline constexpr u32 kGroupDirBytes     = kChunksPerGroup * kGroupDirEntryBytes;   // 48 KiB
inline constexpr u32 kGroupDirSectors   = kGroupDirBytes / kRegionSectorBytes;     // 12, exactly

struct RegionHeader {
    u32 version = kRegionVersion;
    u32 chunkSizeCm = kDefaultChunkSizeCm;
    u32 sectorBytes = kRegionSectorBytes;
    RegionCoord coord;
    u64 levelId = 0;
    u64 worldSeed = 0;
    u32 generatorVersion = 0;
    u32 groupCount = 0;
    u32 chunkCount = 0;
    u32 sectorCount = 0;
    u64 contentHash = 0;   // over every payload, in stored order; the index cross-checks this
    u64 serial = 0;
    // Where the group table lives, so it can be relocated when it grows. v2.
    u32 groupTableSector = 0;
    // The persisted free list: {u32 firstSector, u32 sectorCount} pairs. Zero count means none.
    u32 freeListSector = 0;
    u32 freeListCount = 0;
};

// What the cooker needs beyond the chunks themselves.
struct RegionWriteDesc {
    RegionCoord coord;
    i32 chunkSizeCm = kDefaultChunkSizeCm;
    u64 levelId = 0;
    u64 worldSeed = 0;
    u32 generatorVersion = 0;
    u64 serial = 1;
};

// Writes a whole region. `chunks` may be in any order -- it is SORTED here, by (group, slot), which
// is what makes two cooks of the same content byte-identical however the caller's map iterated.
bool writeRegion(const std::string& path, const RegionWriteDesc& desc,
                 std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks, std::string* why = nullptr);

// A region opened for reading. Holds the header and the group table; group directories and payloads
// are read on demand, which is the entire point of the format.
class RegionFile {
public:
    // Reads both headers, takes the higher serial that passes its CRC, then the group table.
    bool open(const std::string& path, std::string* why = nullptr);
    void close();
    bool isOpen() const { return file_.isOpen(); }

    const RegionHeader& header() const { return header_; }
    // Which of the two header copies was accepted, for a caller that wants to report a torn write.
    u32 acceptedHeaderCopy() const { return acceptedCopy_; }

    // True if this region stores that chunk. Costs one cached lookup plus one ranged read of the
    // group's directory the first time that group is touched.
    bool hasChunk(const ChunkLocal& l, std::string* why = nullptr);

    // Reads and decodes one chunk. False if absent, truncated, or CRC-mismatched -- and `why` NAMES
    // the chunk, because "a region is corrupt" is not something anyone can act on.
    bool readChunk(const ChunkLocal& l, ChunkPayload& out, std::string* why = nullptr);

    // Every chunk this region stores, sorted the way the file stores them.
    std::vector<ChunkLocal> chunks(std::string* why = nullptr);

private:
    // The 4096-entry directory of one group, as read from disk.
    struct GroupDir {
        u32 groupIndex = 0;
        std::vector<u8> raw;   // kGroupDirBytes
    };
    const GroupDir* groupDir(u32 groupIndex, std::string* why);

    mutable aver::File file_;
    RegionHeader header_;
    u32 acceptedCopy_ = 0;
    // groupIndex -> the sector its directory starts at. Sorted, so a lookup is a binary search.
    std::vector<std::pair<u32, u32>> groupTable_;
    // Directories read so far. A region a camera is standing in touches a handful of groups, so a
    // plain vector searched linearly beats a map here.
    std::vector<GroupDir> cache_;
};

// A region open for MUTATION: add, replace and remove chunks without rewriting the file.
//
// WHAT IS AND IS NOT PROMISED, stated precisely, because "crash-safe" claimed loosely is how a
// format ends up like Anvil -- which has one header and a reputation for corrupting on power loss.
//
//   A payload is NEVER written over live data. It goes to free sectors or past the end, and is
//   synced before anything points at it. So a crash mid-payload loses only sectors nothing refers
//   to -- leaked, not corrupt, and reclaimable by rebuilding the free list from the directories.
//
//   The directory entry is then patched -- 12 bytes inside one sector -- and synced. This is the one
//   write that is not atomic in general. It does not need to be: the entry carries the payload's
//   CRC, so a torn entry produces a chunk that FAILS ITS CHECKSUM and is refused by name. One chunk
//   is lost; the region stays usable. That is what the per-chunk CRC is for.
//
//   The header is written LAST, to the copy that is not currently live, with serial + 1. So the two
//   copies are never both mid-write, and a reader takes the higher serial that passes its own CRC.
//
//   What that does NOT give you is a transaction. A crash after the directory patch and before the
//   header leaves the new chunk visible under a header whose sectorCount, chunkCount and contentHash
//   are one step stale. The chunk data is correct; the index's staleness check will fire. Making
//   that atomic needs the directory itself to be double-buffered, which is a real cost for a case
//   that degrades safely, so it is named here rather than pretended away.
class RegionWriter {
public:
    // Opens an existing region for mutation, or creates an empty one at `desc` when absent.
    bool open(const std::string& path, const RegionWriteDesc& desc, std::string* why = nullptr);
    void close();
    bool isOpen() const { return file_.isOpen(); }

    const RegionHeader& header() const { return header_; }

    // Adds or replaces one chunk. Durable when it returns true.
    bool writeChunk(const ChunkLocal& l, const ChunkPayload& p, std::string* why = nullptr);
    // Forgets a chunk and returns its sectors to the free list. False if it was not there.
    bool removeChunk(const ChunkLocal& l, std::string* why = nullptr);

    // Rewrites the file in the canonical layout a fresh cook would produce, dropping every hole.
    //
    // COMPACTION IS THE FORMAT'S ONLY DETERMINISM GUARANTEE ONCE IT MUTATES. Two cooks of the same
    // content are byte-identical; two files that reached the same content by DIFFERENT sequences of
    // writes are not, because sector allocation depends on history. Compaction is what maps them
    // back onto the one canonical form -- which is why the test compares a compacted file against a
    // fresh cook rather than against another mutated one.
    bool compact(std::string* why = nullptr);

    usize freeExtents() const { return free_.size(); }
    u32 freeSectors() const;

private:
    struct Extent { u32 first = 0; u32 count = 0; };
    struct Group { u32 index = 0; u32 dirSector = 0; std::vector<u8> raw; bool dirty = false; };

    Group* group(u32 groupIndex);
    Group* groupOrCreate(u32 groupIndex, std::string* why);
    u32  allocate(u32 sectors, std::string* why);
    void release(u32 first, u32 count);
    bool flushMetadata(std::string* why);
    bool readAllChunks(std::vector<std::pair<ChunkLocal, ChunkPayload>>& out, std::string* why);

    aver::File file_;
    RegionHeader header_;
    RegionWriteDesc desc_;
    std::string path_;
    std::vector<Group> groups_;      // sorted by index
    std::vector<Extent> free_;       // sorted by first, coalesced
    bool groupTableDirty_ = false;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
