#pragma once
// `.ocland` — one landscape SECTION: a square heightfield grid, in the AVR1 container.
//
// WHAT IS IMPLEMENTED, and what is deliberately not, because a format header that lists only what it
// does invites a reader to assume the rest:
//
//   LHDR  the section header -- sample count, spacing, world origin, and the height quantisation.
//         Required, so a file without it is refused rather than half-read.
//   HGHT  the heights, u16-quantised, row-major, sampleCount^2 entries.
//
//   NOT here, and safe to omit: LMSK (per-layer splat weights), LHOL (a hole mask for caves and
//   tunnels), MADR (material addressing). Each is additive and non-Required, so a later writer can
//   add them and THIS reader will skip them and still produce a correct surface -- which is exactly
//   the property the container's Required flag exists to give. A landscape with no splat weights is
//   an untextured landscape, not a broken one.
//
// STREAMING GRANULARITY IS THE FILE. `parseAvr1` demands the size match the byte count exactly and
// `loadAvr1` reads the whole file, so a section is a whole-file read and never a partial one. That is
// what fixes the section size: 1025 samples is 4.0 MiB as f32 and 2.0 MiB quantised, both comfortably
// a single read. It is also why the quadtree that will sit above this subdivides a section in MEMORY
// rather than asking for part of a file.
//
// AXES. Heights are along +Z, because that is the engine's up. Sample (ix, iy) sits at world
//   (origin.x + ix*spacing, origin.y + iy*spacing, height)
// so a column runs along +X and a row along +Y. Worth stating plainly here because Jolt's heightfield
// is Y-up with its own row order, so the physics adapter has to transpose -- and a reader who assumed
// the two agreed would produce terrain that collides ninety degrees out from where it is drawn.
#include "aver/formats/Avr1.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// AVR1 subtype and chunk ids. Distinct four-character codes, so a truncated or mislabelled file is
// refused by the container rather than reinterpreted by a reader that assumed.
inline constexpr u32 kAvrSubtypeLand = avrFourCC("LAND");
inline constexpr u32 kOcLandChunkHeader  = avrFourCC("LHDR");
inline constexpr u32 kOcLandChunkHeights = avrFourCC("HGHT");

// The smallest and largest grids this accepts.
//
// The lower bound is 2 because one sample is a point, not a surface -- there is no quad to build. The
// upper bound is a sanity limit rather than a format limit: 4097^2 u16 is 33.5 MiB and `loadAvr1`
// reads it all at once, so anything past this is a file that will stall a frame rather than stream.
inline constexpr u32 kOcLandMinSamples = 2;
inline constexpr u32 kOcLandMaxSamples = 4097;

// A whole section, decoded.
struct OcLandData {
    u32 sampleCount = 0;              // the grid is sampleCount x sampleCount
    f32 spacingCm = 100.0f;           // world distance between adjacent samples
    f32 originCm[3] = {0.0f, 0.0f, 0.0f};   // world position of sample (0, 0), z included

    // Heights in CENTIMETRES, row-major, sampleCount^2 entries. Decoded from the file's u16 on load,
    // so nothing above this ever sees a quantised value or has to know the encoding.
    //
    // NOT relative to originCm.z: the origin's z is added in, so a height here is the world z of that
    // sample. One less thing for every consumer to remember to do.
    std::vector<f32> heights;

    // RECOMPUTED on load from the heights, never read from the file. A bounds field that is trusted is
    // a bounds field an editing tool can leave stale, and the failure -- geometry culled while it is
    // still on screen -- looks like a renderer bug rather than a stale number.
    f32 boundsMin[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsMax[3] = {0.0f, 0.0f, 0.0f};

    bool valid() const {
        return sampleCount >= kOcLandMinSamples && spacingCm > 0.0f &&
               heights.size() == static_cast<usize>(sampleCount) * sampleCount;
    }

    // Row-major, and BOUNDS-CHECKED to 0 rather than undefined: every caller of this is walking a
    // quadtree with its own idea of the extents, and an off-by-one there should read as a flat edge
    // rather than as a crash or a garbage height.
    f32 heightAt(u32 ix, u32 iy) const {
        if (ix >= sampleCount || iy >= sampleCount) return 0.0f;
        return heights[static_cast<usize>(iy) * sampleCount + ix];
    }

    // The world position of a sample, which is the one place the axis convention above is encoded.
    void worldAt(u32 ix, u32 iy, f32 out[3]) const {
        out[0] = originCm[0] + static_cast<f32>(ix) * spacingCm;
        out[1] = originCm[1] + static_cast<f32>(iy) * spacingCm;
        out[2] = heightAt(ix, iy);
    }

    // The section's footprint, in cm. (sampleCount - 1) spacings, not sampleCount: 1025 samples fence
    // off 1024 quads, and a section meant to abut its neighbour shares that last sample row with it.
    f32 extentCm() const { return static_cast<f32>(sampleCount - 1) * spacingCm; }
};

// Read. `why` receives the reason on failure, which is the difference between a diagnosable bad asset
// and a silent one.
bool parseOcLand(const u8* bytes, usize size, OcLandData& out, std::string* why = nullptr);
bool loadOcLand(const std::string& path, OcLandData& out, std::string* why = nullptr);

// Write. Heights are quantised to u16 across their own observed range, so the step is proportional to
// the terrain's relief rather than fixed: a 10 m dune and a 2 km mountain each get 65535 levels of
// their own span. The bias and scale needed to undo it go in LHDR.
bool writeOcLand(const OcLandData& in, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcLand(const std::string& path, const OcLandData& in, std::string* why = nullptr);

} // namespace aver::fmt
