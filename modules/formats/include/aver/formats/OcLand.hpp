#pragma once
// `.ocland` — one landscape SECTION: a square heightfield grid, in the AVR1 container.
// Sample (ix, iy) sits at world (origin.x + ix*spacing, origin.y + iy*spacing, height), heights along
// +Z; Jolt's heightfield is Y-up with its own row order, so the physics adapter transposes.
#include "aver/formats/Avr1.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// AVR1 subtype and chunk ids for the format.
inline constexpr u32 kAvrSubtypeLand = avrFourCC("LAND");
inline constexpr u32 kOcLandChunkHeader  = avrFourCC("LHDR");
inline constexpr u32 kOcLandChunkHeights = avrFourCC("HGHT");

// OPTIONAL, like OcAnim's NOTF/CURV or OcNav's NLNK: the AUTHORED quantisation range (min/max height,
// world Z, same units as OcLandData::heights). Absent in every file written before this chunk existed
// -- see OcLandData::hasQuantRange for why a missing chunk still gets a range pinned on load rather
// than treated as "no range at all".
inline constexpr u32 kOcLandChunkRange = avrFourCC("LRNG");

// The smallest and largest grids this accepts.
inline constexpr u32 kOcLandMinSamples = 2;
inline constexpr u32 kOcLandMaxSamples = 4097;

// A whole section, decoded.
struct OcLandData {
    u32 sampleCount = 0;              // the grid is sampleCount x sampleCount
    f32 spacingCm = 100.0f;           // world distance between adjacent samples
    f32 originCm[3] = {0.0f, 0.0f, 0.0f};   // world position of sample (0, 0), z included

    // Centimetres, row-major, sampleCount^2 entries, world z with originCm.z already added in.
    std::vector<f32> heights;

    // Recomputed on load from the heights, never read from the file.
    f32 boundsMin[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsMax[3] = {0.0f, 0.0f, 0.0f};

    // The AUTHORED quantisation range (world Z, same units as `heights`): the min/max writeOcLand
    // quantises against. NOT the live min/max of `heights` -- that was the FIRST bug. Recomputing the
    // range from the live heights on every save meant editing ONE sample could move the whole
    // section's range, which silently re-quantised every OTHER sample: untouched terrain drifted on
    // each save.
    //
    // hasQuantRange is false only for a struct that has never been loaded (freshly authored in
    // memory, e.g. by TerrainGenTool or a test fixture) -- there is no prior range to protect yet, so
    // writeOcLand derives one fresh from the live heights, exactly as before the fix. Once a section
    // has been loaded, hasQuantRange is true and quantMinCm/quantMaxCm are PINNED: parseOcLand sets
    // them either from the optional LRNG chunk, or -- for a file saved before LRNG existed -- derived
    // once from that file's own LHDR bias/scale (the range it happened to be quantised against last),
    // which is the one-time migration.
    //
    // writeOcLand reuses the pinned range for any live height that still falls inside it -- so an
    // in-range edit leaves the range, and every untouched sample's quantised code, exactly where it
    // was (the first bug, still fixed). But the range is NOT frozen: a live height OUTSIDE it GROWS
    // the range to cover the new extreme, and the range never shrinks. The pinned range reused
    // UNCONDITIONALLY, with anything outside it clamped, was the SECOND bug -- landscape sculpting is
    // the one authoring mode that ships, and raising ground past the old ceiling is the most ordinary
    // thing a user does with it; clamping silently flattened exactly that. See writeOcLand for the
    // growth/headroom policy and its trade-off.
    f32 quantMinCm = 0.0f;
    f32 quantMaxCm = 0.0f;
    bool hasQuantRange = false;

    // True when the sample count, spacing and heights agree.
    bool valid() const {
        return sampleCount >= kOcLandMinSamples && spacingCm > 0.0f &&
               heights.size() == static_cast<usize>(sampleCount) * sampleCount;
    }

    // The height at a sample. Returns 0 when out of range.
    f32 heightAt(u32 ix, u32 iy) const {
        if (ix >= sampleCount || iy >= sampleCount) return 0.0f;
        return heights[static_cast<usize>(iy) * sampleCount + ix];
    }

    // The world position of a sample.
    void worldAt(u32 ix, u32 iy, f32 out[3]) const {
        out[0] = originCm[0] + static_cast<f32>(ix) * spacingCm;
        out[1] = originCm[1] + static_cast<f32>(iy) * spacingCm;
        out[2] = heightAt(ix, iy);
    }

    // The section's footprint in cm: (sampleCount - 1) spacings, so neighbours share an edge row.
    f32 extentCm() const { return static_cast<f32>(sampleCount - 1) * spacingCm; }
};

// Reads a section. `why` receives the reason on failure.
bool parseOcLand(const u8* bytes, usize size, OcLandData& out, std::string* why = nullptr);
bool loadOcLand(const std::string& path, OcLandData& out, std::string* why = nullptr);

// Writes a section. Heights are quantised to u16 against the AUTHORED range (OcLandData::quantMinCm/
// quantMaxCm, pinned once at load) when `in.hasQuantRange`, GROWN (with headroom) to cover any live
// height outside it, and never shrunk; a struct that has never been loaded quantises across its own
// live extent instead. The range used goes in LHDR's bias/scale, and is also written to the optional
// LRNG chunk so a later load can pin the SAME range instead of deriving a new one.
bool writeOcLand(const OcLandData& in, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcLand(const std::string& path, const OcLandData& in, std::string* why = nullptr);

} // namespace aver::fmt
