#pragma once
// AVR1 — the unified binary container every binary asset format sits on (FORMAT_SPECS.md §3):
// a 64-byte header, a directory of 40-byte chunk entries, and payloads found by chunk id.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// Packs four characters into the little-endian u32 that appears on disk.
constexpr u32 avrFourCC(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0]))
         | (static_cast<u32>(static_cast<u8>(s[1])) << 8)
         | (static_cast<u32>(static_cast<u8>(s[2])) << 16)
         | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

inline constexpr u32 kAvr1Magic = avrFourCC("AVR1");

// Subtypes from §3.1.
inline constexpr u32 kAvrSubtypeMesh  = avrFourCC("MESH");
inline constexpr u32 kAvrSubtypeTex   = avrFourCC("TEX ");
inline constexpr u32 kAvrSubtypeSkel  = avrFourCC("SKEL");
inline constexpr u32 kAvrSubtypeAnim  = avrFourCC("ANIM");
inline constexpr u32 kAvrSubtypeMatl  = avrFourCC("MATL");
inline constexpr u32 kAvrSubtypePref  = avrFourCC("PREF");
inline constexpr u32 kAvrSubtypeWrld  = avrFourCC("WRLD");
inline constexpr u32 kAvrSubtypePak   = avrFourCC("PAK ");

inline constexpr u16 kAvrContainerVersion = 1;

// Chunk flags (§3.2).
inline constexpr u8 kAvrChunkRequired      = 0x1;
inline constexpr u8 kAvrChunkGpuUploadable = 0x2;
inline constexpr u8 kAvrChunkText          = 0x4;

// File flags (§3.1).
inline constexpr u32 kAvrFlagCooked        = 0x1;
inline constexpr u32 kAvrFlagCompressed    = 0x2;
inline constexpr u32 kAvrFlagHasSourceHash = 0x4;

// One directory entry and its payload.
struct AvrChunk {
    u32              id = 0;
    u16              version = 1;
    u8               compression = 0;      // 0 none; 1 zstd and 2 lz4 are read-only
    u8               flags = 0;
    std::vector<u8>  data;                 // always the UNCOMPRESSED payload, both directions
};

// A whole container in memory.
struct Avr1File {
    u32 subtype = 0;
    u16 contentVersion = 1;
    u16 minReaderVersion = kAvrContainerVersion;
    u8  alignLog2 = 4;                     // base alignment is 1 << alignLog2 bytes
    u32 flags = 0;
    u8  guid[16] = {};
    std::vector<AvrChunk> chunks;

    // The chunk with this id, or null.
    const AvrChunk* find(u32 chunkId) const;
    // Appends a chunk and returns it.
    AvrChunk&       add(u32 chunkId, std::vector<u8> payload, u8 chunkFlags = 0, u16 chunkVersion = 1);
};

// Reads a container. Returns false with `why` set on bad magic, a header CRC mismatch, a truncated
// file, a chunk whose payload leaves the file, a chunk hash mismatch, or a MinReaderVersion above
// ours.
//
// IT DOES NOT CHECK kAvrChunkRequired, and this comment used to claim it did. Nothing in the tree
// reads that flag: writers set it (OcMesh, OcAnim, OcAudio, GiCache and the rest all mark their base
// chunks Required) and no reader has ever looked. The claim was here long enough to be believed.
//
// It is also not a check parseAvr1 COULD make. "Unknown" is not a property of the container -- it is
// a property of the reader, which is the only party that knows which chunk ids it understands.
// parseAvr1 sees four bytes and a flag byte; it cannot tell a chunk this build has never heard of
// from one the caller is about to look up by id. Enforcement has to live in each format reader,
// where the known-id set exists: after find()ing what it wants, walk `chunks` once and refuse any
// entry carrying kAvrChunkRequired that it did not claim.
//
// Until one does, kAvrChunkRequired is descriptive rather than load-bearing: a future writer adding a
// Required chunk to mean "an old reader MUST refuse this file" will not get that behaviour, and an
// old build will load the file with stale semantics instead. That is a real gap, deliberately left
// stated rather than papered over with a helper no reader calls -- an unused enforcement API is the
// same defect as an unenforced flag, one layer up.
bool loadAvr1(const std::string& path, Avr1File& out, std::string* why = nullptr);
bool parseAvr1(const u8* bytes, usize size, Avr1File& out, std::string* why = nullptr);

// Writes a container. Payloads go in directory order, aligned to the base alignment, or to 256 bytes
// when GpuUploadable (§3.2).
bool saveAvr1(const std::string& path, const Avr1File& in, std::string* why = nullptr);
bool writeAvr1(const Avr1File& in, std::vector<u8>& out, std::string* why = nullptr);

inline constexpr u32 kAvrStringNull = 0xFFFFFFFFu;

// The §3.3 string table: u16-length-prefixed UTF-8 entries, offset 0 reserved for the empty string.
class AvrStringTable {
public:
    // Starts with the reserved empty entry at offset 0.
    AvrStringTable();
    // Interns a string and returns its StringRef. Identical strings share one entry.
    u32              add(std::string_view s);
    // The string at a StringRef, or "" for 0 or anything out of range.
    std::string_view get(u32 ref) const;
    const std::vector<u8>& bytes() const { return blob_; }
    void             setBytes(std::vector<u8> b) { blob_ = std::move(b); }
private:
    std::vector<u8> blob_;
};

// The container's chunk hash. AVR1 v1 uses xxHash64, not the xxHash3-64 §3.2 originally named.
u64 avrHash64(const void* data, usize size);
// The container's header checksum.
u32 avrCrc32c(const void* data, usize size);

} // namespace aver::fmt
