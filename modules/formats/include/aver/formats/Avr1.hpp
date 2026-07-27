#pragma once
// AVR1 — the unified binary container (FORMAT_SPECS.md §3).
//
// Every binary asset format in this engine is meant to sit on this: .ocmesh, .octex, .ocskel,
// .ocanim, .ocprefab and .ocpak are all "an AVR1 file with a different Subtype and a different set
// of chunks". Until now none of them existed -- a grep for AVR1 across the tree returned nothing,
// and the spec had been "implementation-ready" and unimplemented for its whole life. This is the
// foundation the mesh pipeline stands on, so it is deliberately the smallest thing that can carry
// the whole family rather than something shaped around meshes.
//
// WHAT A CONTAINER IS HERE: a 64-byte header, a directory of 40-byte chunk entries, and payloads.
// The directory is authoritative -- chunks may sit in any order, and a reader finds them by id. That
// is what buys forward compatibility: a v2 writer can add a chunk a v1 reader has never heard of,
// and the v1 reader skips it unless the writer marked it Required.
//
// TWO DELIBERATE DEVIATIONS FROM THE SPEC AS WRITTEN, both recorded here rather than left for
// someone to discover by diffing:
//
//   1. The chunk hash is NOT xxHash3-64. That algorithm is not vendored, this tree has no network
//      access to fetch it, and adding a dependency is not a decision a format reader should make on
//      its own. AVR1 v1 uses xxHash64 -- the same author's earlier function, small enough to
//      implement correctly in one screen, and a real 64-bit hash rather than something invented
//      here. The field is the same size and in the same place; only the algorithm differs, and
//      FORMAT_SPECS.md §3.2 has been corrected to say so. A file written by this code and read by a
//      future xxHash3 reader would report corruption, which is exactly why the spec must not keep
//      claiming an algorithm nothing implements.
//   2. Compression is written as 0 (none) only. The field and the enum are honoured on READ, so a
//      later writer can add zstd without a format break, but no compressor is vendored either.
//
// Neither deviation costs the format anything a caller can observe: sizes and offsets are unchanged.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// FourCC as it appears on disk: little-endian, so 'AVR1' is 0x31525641 and the bytes read A V R 1.
constexpr u32 avrFourCC(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0]))
         | (static_cast<u32>(static_cast<u8>(s[1])) << 8)
         | (static_cast<u32>(static_cast<u8>(s[2])) << 16)
         | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

inline constexpr u32 kAvr1Magic = avrFourCC("AVR1");

// Subtypes from §3.1. Only MESH is written today; the rest are named so a reader can report
// "this is a .octex and you asked me to load a mesh" instead of "bad file".
inline constexpr u32 kAvrSubtypeMesh  = avrFourCC("MESH");
inline constexpr u32 kAvrSubtypeTex   = avrFourCC("TEX ");
inline constexpr u32 kAvrSubtypeSkel  = avrFourCC("SKEL");
inline constexpr u32 kAvrSubtypeAnim  = avrFourCC("ANIM");
inline constexpr u32 kAvrSubtypeMatl  = avrFourCC("MATL");
inline constexpr u32 kAvrSubtypePref  = avrFourCC("PREF");
inline constexpr u32 kAvrSubtypeWrld  = avrFourCC("WRLD");
inline constexpr u32 kAvrSubtypePak   = avrFourCC("PAK ");

inline constexpr u16 kAvrContainerVersion = 1;

// Chunk flags (§3.2). Required is the one that matters: it is how a writer says "if you do not
// understand this chunk you must not pretend to have loaded the asset".
inline constexpr u8 kAvrChunkRequired      = 0x1;
inline constexpr u8 kAvrChunkGpuUploadable = 0x2;
inline constexpr u8 kAvrChunkText          = 0x4;

// File flags (§3.1).
inline constexpr u32 kAvrFlagCooked        = 0x1;
inline constexpr u32 kAvrFlagCompressed    = 0x2;
inline constexpr u32 kAvrFlagHasSourceHash = 0x4;

struct AvrChunk {
    u32              id = 0;
    u16              version = 1;
    u8               compression = 0;      // 0 none; 1 zstd and 2 lz4 are read-only, see the header note
    u8               flags = 0;
    std::vector<u8>  data;                 // always the UNCOMPRESSED payload, both directions
};

// A whole container in memory. Small assets are read entirely; this format is not a streaming one
// and the spec does not ask it to be.
struct Avr1File {
    u32 subtype = 0;
    u16 contentVersion = 1;
    u16 minReaderVersion = kAvrContainerVersion;
    u8  alignLog2 = 4;                     // 16-byte base alignment, the spec's default
    u32 flags = 0;
    u8  guid[16] = {};
    std::vector<AvrChunk> chunks;

    const AvrChunk* find(u32 chunkId) const;
    AvrChunk&       add(u32 chunkId, std::vector<u8> payload, u8 chunkFlags = 0, u16 chunkVersion = 1);
};

// Read a container. Returns false and fills `why` on anything the spec says to refuse: bad magic, a
// header CRC mismatch, a truncated file, a MinReaderVersion above ours, or an unknown chunk marked
// Required. `why` is always set on failure and never on success.
bool loadAvr1(const std::string& path, Avr1File& out, std::string* why = nullptr);
bool parseAvr1(const u8* bytes, usize size, Avr1File& out, std::string* why = nullptr);

// Write a container. Chunk payloads are laid out in directory order, each aligned to the base
// alignment (or 256 bytes when GpuUploadable, per §3.2).
bool saveAvr1(const std::string& path, const Avr1File& in, std::string* why = nullptr);
bool writeAvr1(const Avr1File& in, std::vector<u8>& out, std::string* why = nullptr);

// ---- the string table (§3.3) ----
// A blob of u16-length-prefixed UTF-8 entries. Offset 0 is reserved to hold the empty string, so a
// StringRef of 0 is legal and means "", and 0xFFFFFFFF means null.
inline constexpr u32 kAvrStringNull = 0xFFFFFFFFu;

class AvrStringTable {
public:
    AvrStringTable();                       // starts with the reserved empty entry at offset 0
    u32              add(std::string_view s);   // returns the StringRef; identical strings share one entry
    std::string_view get(u32 ref) const;        // "" for 0 or for anything out of range
    const std::vector<u8>& bytes() const { return blob_; }
    void             setBytes(std::vector<u8> b) { blob_ = std::move(b); }
private:
    std::vector<u8> blob_;
};

// Exposed because the mesh reader checks payloads itself, and because a test that cannot compute the
// same hash cannot prove the writer wrote the right one.
u64 avrHash64(const void* data, usize size);
u32 avrCrc32c(const void* data, usize size);

} // namespace aver::fmt
