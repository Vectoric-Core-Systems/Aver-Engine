#pragma once
// The world coordinate hierarchy: region -> chunk -> local float.
//
// WHAT THIS IS FOR. Every position in this engine is an absolute f32 in centimetres, and f32 holds
// sub-millimetre precision out to 2^20 cm = 10.486 km and no further. Past that a coordinate is
// quantised: at 100 km the grid is 1 cm, at the planet's own radius it is 64 cm. The text level
// format is worse still -- OcWorld.cpp's `%.6g` moves a 12.3 km coordinate by 3 cm on every save,
// before f32 is involved at all (docs/CHUNKS.md B8).
//
// Splitting a position into an integer chunk index plus a small float offset removes both problems
// at once: the integer part is exact at any distance, and the float part is at most one chunk wide,
// where f32's grid is ~1.2 um. That is the whole idea, and everything below is bookkeeping around it.
//
// THIS HEADER IS DELIBERATELY PURE. No I/O, no scene, no allocation, everything constexpr where it
// can be. It is a value type like Vec3, which is why it is NOT behind AVER_MODULE_STREAM: if it were
// optional, every caller would need an #if to ask which chunk something is in.
//
// See docs/CHUNKS.md sections 4.1-4.3 for the derivation, including why 512 and why 16 m.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <cmath>

namespace aver::world {

// ---- the fixed geometry ------------------------------------------------------------------------
//
// A region spans 1024 chunks per axis, indexed -512..+511. Those are not arbitrary:
//
//   * 1024 values is EXACTLY 10 bits signed, so three axes pack into 30 bits with 2 to spare, and
//     the split into a 6-bit group and a 4-bit slot is exact rather than approximate (kGroup*/kSlot*
//     below). The region file's directory is built on that split -- 1024^3 is 1.07 billion slots,
//     which is far too many for one flat table, so it becomes a sparse 64^3 group table over dense
//     16^3 directories.
//   * Indexing from -512 rather than 0 centres region 0 ON the world origin. An existing flat level
//     sitting at (0,0,0) is then in the middle of its region rather than on a seam between four.
inline constexpr i32 kChunksPerRegionAxis = 1024;
inline constexpr i32 kChunkLocalMin       = -512;
inline constexpr i32 kChunkLocalMax       =  511;
inline constexpr i32 kChunkLocalBits      =  10;   // exactly, for kChunksPerRegionAxis values

// The two-level directory split: the top 6 bits of a local chunk coordinate choose a group, the
// bottom 4 choose a slot inside it. 64^3 groups of 16^3 chunks.
inline constexpr i32 kGroupsPerRegionAxis = 64;
inline constexpr i32 kChunksPerGroupAxis  = 16;
inline constexpr i32 kGroupBits           = 6;
inline constexpr i32 kSlotBits            = 4;
inline constexpr u32 kGroupsPerRegion     = 64u * 64u * 64u;    // 262 144
inline constexpr u32 kChunksPerGroup      = 16u * 16u * 16u;    // 4 096

// ---- the precision budget ----------------------------------------------------------------------

// The largest |coordinate|, in centimetres, at which f32 still resolves better than a millimetre.
//
// DERIVED, NOT PICKED. ulp(x) = 2^(floor(log2 x) - 23), so ulp <= 0.1 cm needs 2^e <= 0.1 * 2^23 =
// 838860.8, hence e <= 19 and |x| < 2^20. At exactly 2^20 the grid is 0.125 cm.
inline constexpr i32 kSubMillimetreCm = 1 << 20;   // 1 048 576 cm = 10.48576 km

// The default chunk size: 16 m. The largest round size satisfying chunkSizeValid below -- 512 * 1600
// = 819 200 cm, where f32's grid is 0.625 mm. OcWorld.hpp's OcPcgVolume::cellSizeCm defaults to the
// same 1600 for the same reason, so a level's PCG cells and its chunks line up by default.
inline constexpr i32 kDefaultChunkSizeCm = 1600;

// True if a region built on this chunk size stays inside the sub-millimetre budget.
//
// A region reaches 512 chunks from its centre, so the requirement is 512 * size < 2^20, i.e.
// size <= 2047. 20.48 m is the first size that fails; it is not catastrophic (the grid becomes
// 1.25 mm) but it is past the point where "the hierarchy makes precision a non-issue" is true, and
// something has to say where that point is.
constexpr bool chunkSizeValid(i32 chunkSizeCm) {
    // STRICTLY less than 2^20, and computed in i64. Integer-dividing the other way round accepts
    // 2048 -- 2^20 / 2048 is exactly 512 -- which is the one size that must be rejected, since
    // 512 * 2048 lands exactly ON the cliff where the grid becomes 1.25 mm.
    return chunkSizeCm > 0
        && static_cast<i64>(kChunkLocalMax + 1) * chunkSizeCm < static_cast<i64>(kSubMillimetreCm);
}

// The f32 grid spacing at |x| centimetres -- what one least-significant bit is worth there.
// Returns 0 for x == 0. Not constexpr: std::frexp is not.
inline f32 f32GridAtCm(f32 x) {
    if (x == 0.0f) return 0.0f;
    int e = 0;
    std::frexp(std::fabs(x), &e);      // |x| = m * 2^e, m in [0.5, 1), so |x| is in [2^(e-1), 2^e)
    return std::ldexp(1.0f, e - 24);   // ulp = 2^((e-1) - 23), f32 having 23 fraction bits
}

// True if an absolute world position still resolves to better than a millimetre in f32.
//
// THIS IS THE CHECK docs/CHUNKS.md SAYS TO BUILD INSTEAD OF THE MACHINERY. Origin rebasing is slice
// 10; until then a level simply has to stay inside the budget, and the only thing that makes that
// enforceable rather than hopeful is something that measures it.
inline bool withinSubMillimetre(const Vec3& worldCm) {
    const f32 lim = static_cast<f32>(kSubMillimetreCm);
    return std::fabs(worldCm.x) < lim && std::fabs(worldCm.y) < lim && std::fabs(worldCm.z) < lim;
}

// ---- integer division that floors ----------------------------------------------------------------

// floorDiv/floorMod for a POSITIVE divisor. C++ truncates toward zero, which is wrong here: -1 / 1600
// is 0, so a position one centimetre below the origin would land in chunk 0 alongside +1, and chunk 0
// would be twice as wide as every other chunk.
//
// THE TREE HAS ALREADY BEEN BITTEN BY EXACTLY THIS. PcgVolume.cpp:50-52 truncates where :78-80
// floors, and PcgShaders.hpp:64-66 admits the two disagree "the moment a volume is centred on the
// origin". Every negative-coordinate case in ChunkCoordTest exists because of that precedent.
constexpr i32 floorDiv(i32 a, i32 b) {
    const i32 q = a / b;
    return (a % b != 0 && (a < 0)) ? q - 1 : q;
}
constexpr i32 floorMod(i32 a, i32 b) {
    const i32 m = a % b;
    return m < 0 ? m + b : m;
}

// ---- the coordinates themselves ------------------------------------------------------------------

// A chunk's GLOBAL index: unbounded, exact, and the identity a chunk is keyed by everywhere. The
// region/local pair below is a VIEW of this, not a separate truth -- keeping one authority is what
// stops the two from drifting the way the level loaders did.
struct ChunkCoord {
    i32 x = 0, y = 0, z = 0;

    friend constexpr bool operator==(const ChunkCoord& a, const ChunkCoord& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(const ChunkCoord& a, const ChunkCoord& b) { return !(a == b); }
};

// Which region file a chunk lives in.
struct RegionCoord {
    i32 x = 0, y = 0, z = 0;

    friend constexpr bool operator==(const RegionCoord& a, const RegionCoord& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(const RegionCoord& a, const RegionCoord& b) { return !(a == b); }
};

// A chunk's index WITHIN its region, on [-512, +511] per axis. i16 because 10 bits is all it needs
// and the region directory stores it packed anyway.
struct ChunkLocal {
    i16 x = 0, y = 0, z = 0;

    friend constexpr bool operator==(const ChunkLocal& a, const ChunkLocal& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(const ChunkLocal& a, const ChunkLocal& b) { return !(a == b); }
};

// A world position, split. `local` is on [0, chunkSizeCm) per axis -- deliberately UNSIGNED, so the
// truncate-vs-floor question is answered once inside floorDiv and never again at a call site.
struct SplitPos {
    ChunkCoord chunk;
    Vec3       local;
};

// ---- region <-> chunk ------------------------------------------------------------------------------

// The region containing a global chunk index.
//
// The +512 bias is what centres region 0 on the origin: region r covers gc in
// [1024r - 512, 1024r + 511].
constexpr i32 regionOfAxis(i32 globalChunk) {
    return floorDiv(globalChunk + (kChunkLocalMax + 1), kChunksPerRegionAxis);
}
constexpr i16 localOfAxis(i32 globalChunk) {
    return static_cast<i16>(floorMod(globalChunk + (kChunkLocalMax + 1), kChunksPerRegionAxis)
                            + kChunkLocalMin);
}
// The inverse: the global chunk index a (region, local) pair names.
constexpr i32 globalChunkOfAxis(i32 region, i32 local) {
    return region * kChunksPerRegionAxis + local;
}

constexpr RegionCoord regionOf(const ChunkCoord& c) {
    return RegionCoord{regionOfAxis(c.x), regionOfAxis(c.y), regionOfAxis(c.z)};
}
constexpr ChunkLocal localOf(const ChunkCoord& c) {
    return ChunkLocal{localOfAxis(c.x), localOfAxis(c.y), localOfAxis(c.z)};
}
constexpr ChunkCoord chunkOf(const RegionCoord& r, const ChunkLocal& l) {
    return ChunkCoord{globalChunkOfAxis(r.x, l.x), globalChunkOfAxis(r.y, l.y),
                      globalChunkOfAxis(r.z, l.z)};
}

// ---- packing ---------------------------------------------------------------------------------------

// A local chunk coordinate as 30 bits: 10 per axis, biased to unsigned, x in the low bits.
//
// EXACT, not approximate: [-512, 511] is 1024 values, which is 2^10 with nothing left over. The two
// spare bits of the u32 are left alone rather than used, because the region directory's own flags
// belong in the directory and not smuggled into a coordinate.
constexpr u32 packLocal(const ChunkLocal& l) {
    const u32 ux = static_cast<u32>(l.x - kChunkLocalMin);
    const u32 uy = static_cast<u32>(l.y - kChunkLocalMin);
    const u32 uz = static_cast<u32>(l.z - kChunkLocalMin);
    return ux | (uy << kChunkLocalBits) | (uz << (2 * kChunkLocalBits));
}
constexpr ChunkLocal unpackLocal(u32 packed) {
    constexpr u32 mask = (1u << kChunkLocalBits) - 1u;
    return ChunkLocal{
        static_cast<i16>(static_cast<i32>(packed & mask) + kChunkLocalMin),
        static_cast<i16>(static_cast<i32>((packed >> kChunkLocalBits) & mask) + kChunkLocalMin),
        static_cast<i16>(static_cast<i32>((packed >> (2 * kChunkLocalBits)) & mask) + kChunkLocalMin)};
}

// The two-level directory split. groupIndex picks one of 64^3 groups; slotIndex picks one of 16^3
// chunks inside it. Together they are the same 30 bits packLocal produces, cut 6/4 per axis --
// which is what makes "read the group's 32 KiB dense table, then one payload" possible at all.
constexpr u32 groupIndexOf(const ChunkLocal& l) {
    const u32 gx = static_cast<u32>(l.x - kChunkLocalMin) >> kSlotBits;
    const u32 gy = static_cast<u32>(l.y - kChunkLocalMin) >> kSlotBits;
    const u32 gz = static_cast<u32>(l.z - kChunkLocalMin) >> kSlotBits;
    return gx + gy * static_cast<u32>(kGroupsPerRegionAxis)
              + gz * static_cast<u32>(kGroupsPerRegionAxis * kGroupsPerRegionAxis);
}
constexpr u32 slotIndexOf(const ChunkLocal& l) {
    constexpr u32 m = (1u << kSlotBits) - 1u;
    const u32 sx = static_cast<u32>(l.x - kChunkLocalMin) & m;
    const u32 sy = static_cast<u32>(l.y - kChunkLocalMin) & m;
    const u32 sz = static_cast<u32>(l.z - kChunkLocalMin) & m;
    return sx + sy * static_cast<u32>(kChunksPerGroupAxis)
              + sz * static_cast<u32>(kChunksPerGroupAxis * kChunksPerGroupAxis);
}
// The inverse of the pair above.
constexpr ChunkLocal localOfGroupSlot(u32 groupIndex, u32 slotIndex) {
    const u32 G = static_cast<u32>(kGroupsPerRegionAxis);
    const u32 S = static_cast<u32>(kChunksPerGroupAxis);
    const u32 gx = groupIndex % G, gy = (groupIndex / G) % G, gz = groupIndex / (G * G);
    const u32 sx = slotIndex % S,  sy = (slotIndex / S) % S,  sz = slotIndex / (S * S);
    return ChunkLocal{
        static_cast<i16>(static_cast<i32>((gx << kSlotBits) | sx) + kChunkLocalMin),
        static_cast<i16>(static_cast<i32>((gy << kSlotBits) | sy) + kChunkLocalMin),
        static_cast<i16>(static_cast<i32>((gz << kSlotBits) | sz) + kChunkLocalMin)};
}

// ---- world position <-> the hierarchy --------------------------------------------------------------

// Splits an authored f64 position. THE f64 OVERLOAD IS THE ONE THE LOADER SHOULD USE: .ocworld
// carries f64 and both level loaders narrowed to f32 before doing anything with it, which throws away
// the precision the hierarchy exists to keep. Splitting first and narrowing the small part after is
// the whole trick.
inline SplitPos splitCm(f64 x, f64 y, f64 z, i32 chunkSizeCm) {
    const f64 s = static_cast<f64>(chunkSizeCm);
    // floor(v/s) computed in f64 and then reused to form the remainder, so the two always agree --
    // computing the remainder independently is how a position lands in one chunk while its offset
    // says another.
    const f64 qx = std::floor(x / s), qy = std::floor(y / s), qz = std::floor(z / s);
    SplitPos out;
    out.chunk = ChunkCoord{static_cast<i32>(qx), static_cast<i32>(qy), static_cast<i32>(qz)};
    out.local = Vec3{static_cast<f32>(x - qx * s), static_cast<f32>(y - qy * s),
                     static_cast<f32>(z - qz * s)};
    return out;
}
inline SplitPos splitCm(const Vec3& worldCm, i32 chunkSizeCm) {
    return splitCm(static_cast<f64>(worldCm.x), static_cast<f64>(worldCm.y),
                   static_cast<f64>(worldCm.z), chunkSizeCm);
}

// A chunk's own origin in absolute world centimetres, EXACTLY. i64 rather than f32 on purpose: this
// is the number the hierarchy exists to keep exact, and handing it back as a float would undo that
// at the last step.
constexpr i64 chunkOriginCmAxis(i32 globalChunk, i32 chunkSizeCm) {
    return static_cast<i64>(globalChunk) * static_cast<i64>(chunkSizeCm);
}

// Rejoins a split position into an absolute world f32.
//
// LOSSY BEYOND kSubMillimetreCm, unavoidably, because that is what an absolute f32 is. It exists
// because the runtime is absolute f32 end to end today and something has to hand it one. Prefer
// toRegionRelativeCm below wherever the caller can carry a region.
inline Vec3 toWorldCm(const SplitPos& p, i32 chunkSizeCm) {
    return Vec3{static_cast<f32>(chunkOriginCmAxis(p.chunk.x, chunkSizeCm)) + p.local.x,
                static_cast<f32>(chunkOriginCmAxis(p.chunk.y, chunkSizeCm)) + p.local.y,
                static_cast<f32>(chunkOriginCmAxis(p.chunk.z, chunkSizeCm)) + p.local.z};
}

// The same position expressed relative to a region's origin -- which is what keeps the float small.
//
// THIS IS THE FUNCTION SLICE 10 IS BUILT ON. A region reaches 512 chunks from its centre, so at the
// default 16 m every component of the result is within 8.192 km of zero and f32's grid there is
// 0.625 mm, ANYWHERE in an unbounded world. That is the property absolute coordinates cannot have,
// and it is why the float origin snaps to a region rather than to a hysteresis radius.
inline Vec3 toRegionRelativeCm(const SplitPos& p, const RegionCoord& r, i32 chunkSizeCm) {
    const i64 s = static_cast<i64>(chunkSizeCm);
    const i64 ox = static_cast<i64>(r.x) * kChunksPerRegionAxis * s;
    const i64 oy = static_cast<i64>(r.y) * kChunksPerRegionAxis * s;
    const i64 oz = static_cast<i64>(r.z) * kChunksPerRegionAxis * s;
    return Vec3{static_cast<f32>(chunkOriginCmAxis(p.chunk.x, chunkSizeCm) - ox) + p.local.x,
                static_cast<f32>(chunkOriginCmAxis(p.chunk.y, chunkSizeCm) - oy) + p.local.y,
                static_cast<f32>(chunkOriginCmAxis(p.chunk.z, chunkSizeCm) - oz) + p.local.z};
}

// ---- compile-time proofs of the section 4.2 arithmetic -------------------------------------------
//
// These are static_asserts and not test cases on purpose: they are identities about the constants
// themselves, so if one is ever wrong the module should fail to COMPILE rather than fail a test run
// somebody might not have got to. ChunkCoordTest covers the behaviour; this covers the geometry.
static_assert(kChunkLocalMax - kChunkLocalMin + 1 == kChunksPerRegionAxis, "the local range is one region wide");
static_assert((1 << kChunkLocalBits) == kChunksPerRegionAxis, "10 bits is exactly 1024 values");
static_assert(3 * kChunkLocalBits == 30, "three axes pack into 30 bits, leaving 2 of a u32 spare");
static_assert(kGroupBits + kSlotBits == kChunkLocalBits, "the group/slot cut spends the 10 bits exactly");
static_assert((1 << kGroupBits) == kGroupsPerRegionAxis, "6 bits of group is 64");
static_assert((1 << kSlotBits) == kChunksPerGroupAxis, "4 bits of slot is 16");
static_assert(kGroupsPerRegion * kChunksPerGroup
                  == static_cast<u32>(kChunksPerRegionAxis) * static_cast<u32>(kChunksPerRegionAxis)
                         * static_cast<u32>(kChunksPerRegionAxis),
              "the two-level directory addresses every chunk in the region and no more");
static_assert(chunkSizeValid(kDefaultChunkSizeCm), "the default chunk size is inside the precision budget");
static_assert(chunkSizeValid(2047) && !chunkSizeValid(2048), "2047 cm is the boundary, exactly");
static_assert(floorDiv(-1, 1600) == -1 && floorDiv(0, 1600) == 0, "floorDiv floors rather than truncating");
static_assert(localOfAxis(0) == 0 && regionOfAxis(0) == 0, "region 0 is centred on the world origin");
static_assert(regionOfAxis(-512) == 0 && regionOfAxis(-513) == -1, "the region boundary sits at -512");
static_assert(localOfAxis(-513) == 511 && localOfAxis(512) == -512, "and wraps to the far end either way");

} // namespace aver::world
