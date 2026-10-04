#pragma once
// .cache — a derived-data cache entry holding one baked GI radiance volume.
//
// WHAT THIS IS FOR. Voxi's global illumination is a 3D radiance volume rebuilt by rasterising the
// scene into it (voxelizePass), resolving the atomic accumulator (CSResolve) and filtering the mip
// chain (CSMip). That is genuinely dynamic and stays dynamic: an in-memory gate
// (VoxiRenderer::giSnapshotUnchanged) already skips the rebuild on any frame whose inputs are
// unchanged, and measures ~93% of ticks avoided in a static scene. What the gate CANNOT do is
// remember anything across a run, so every level load pays a full revoxelisation before the first
// lit frame — and so does every return to a level the author has already looked at.
//
// This is that memory. The resolved volume is written beside the project under
// DerivedDataCache/GI, keyed by exactly the inputs the in-memory gate already keys on, and read
// back at load. A hit skips voxelisation entirely; a miss rebuilds and writes a fresh entry.
//
// DERIVED DATA, WHICH IS THE OTHER HALF OF THE CONTRACT. Nothing here is authored and nothing here
// is precious: the whole directory can be deleted at any time and the only cost is one rebuild.
// That is why it lives in its own folder rather than under Content, and why a stale or unreadable
// entry is a miss rather than an error.
//
// AVR1 container, subtype "GIVL". Engine space: cm, X fwd / Y right / Z up.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// The inputs that decide whether a cached volume is still the right answer. Every field here is
// something voxelizePass actually reads, and a difference in any of them means the baked radiance
// would come out different.
//
// WHY A HASH AND NOT THE INPUTS THEMSELVES for the draws and the sky: the draw list is thousands of
// transforms and the sky is a struct that grows, and neither is worth storing to compare. The hash
// is the one VoxiRenderer already computes for its in-memory gate, so a cache hit and a gate hit
// mean the same thing by construction rather than by two implementations agreeing.
struct GiCacheKey {
    u64 drawsKey = 0;       // VoxiRenderer::giDrawsKey() -- mesh handles, transforms, material bits
    u64 skyKey   = 0;       // the SkyAtmosphere bytes, EXCLUDING the cloud clock (see giSnapshotUnchanged)
    f32 centre[3] = {0, 0, 0};
    f32 extent   = 0.0f;    // volume half-edge, cm
    u32 resolution = 0;     // voxels per edge; the volume is resolution^3
    u32 mipCount = 0;

    bool operator==(const GiCacheKey& o) const;
    bool operator!=(const GiCacheKey& o) const { return !(*this == o); }
};

// A whole cache entry: the key it was baked for, and the volume itself.
struct GiCacheEntry {
    GiCacheKey key{};
    // Mip 0..mipCount-1 concatenated, tightly packed, RGBA16F (8 bytes per voxel). Mip m is
    // (resolution >> m)^3 voxels, so the total is fully determined by the key -- which is what
    // loadGiCache checks the payload length against rather than trusting a stored size.
    std::vector<u8> voxels;
};

// Bytes one mip occupies, and the whole chain. Pure arithmetic on the key; no I/O.
u64 giCacheMipBytes(const GiCacheKey& k, u32 mip);
u64 giCacheTotalBytes(const GiCacheKey& k);
// Byte offset of a mip within GiCacheEntry::voxels.
u64 giCacheMipOffset(const GiCacheKey& k, u32 mip);

// The file name an entry with this key takes, WITHOUT a directory: "<16 hex digits>.cache".
//
// NAMED BY THE KEY, so two different bakes of one level coexist rather than overwrite each other --
// an author toggling between two sun angles gets a hit on both instead of thrashing one file. The
// directory is swept by age, not by uniqueness; see giCacheSweep.
std::string giCacheFileName(const GiCacheKey& k);

// "<projectDir>\DerivedDataCache\GI". Empty when projectDir is empty.
std::string giCacheDir(const std::string& projectDir);

// Reads an entry. Returns false with `why` set on a missing file, bad container, wrong subtype, a
// key that does not parse, or a payload whose length disagrees with its key.
bool loadGiCache(const std::string& path, GiCacheEntry& out, std::string* why = nullptr);
// Writes an entry, creating the directory chain if needed.
bool saveGiCache(const std::string& path, const GiCacheEntry& in, std::string* why = nullptr);

// Deletes the oldest entries in `dir` until at most `keep` .cache files remain. Returns how many
// were removed.
//
// BOUNDED, BECAUSE NOTHING ELSE BOUNDS IT. Every distinct bake writes a new file and nothing ever
// overwrites one, so an author who nudges the sun a hundred times leaves a hundred volumes on disk
// at ~18 MiB each. Age-ordered rather than LRU: last-write time is what the filesystem already
// tracks, and the difference between the two only matters for entries that are being hit, which are
// exactly the ones being rewritten.
// Keeps the `keep` newest .cache files, and additionally stops at `maxBytes` if that is reached
// first. maxBytes == 0 means "no byte limit", which is the behaviour this had before it took the
// argument at all.
//
// A COUNT ALONE IS THE WRONG UNIT and that is why the byte cap exists. An entry is one GI volume,
// and a volume is 18 MB at 128^3 but 256 MB at the largest size the writer will accept
// (kMaxCachedGiEntryBytes) -- so "keep 8" means 144 MB in one project and 2 GB in another, with
// nothing in the code saying which. Raising the count to hold a real working set at 128^3 would
// have silently authorised multiple gigabytes at the top end.
u32 giCacheSweep(const std::string& dir, u32 keep, u64 maxBytes = 0);

}  // namespace aver::fmt
