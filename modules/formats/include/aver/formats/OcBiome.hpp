#pragma once
// `.ocbiome` -- one BIOME: where it applies (a mask) and what it grows (a list of foliage rules). A hand-editable
// text file, like .ocgraph: a biome is a short list of numbers a person reads and diffs, not bulk data.
//
//     OCBIOME 1
//     NAME Pine Forest
//     PRIORITY 2
//     MASK layer name=Forest min=0.4 edge=0.3
//     ENTRY foliage=Foliage/Pine.ocfoliage density=4 slope=0,35 height=0,9000 layer=Forest layerMin=0.5 clump=600,3,6 priority=2 prune=250
//
// The MASK line is one of
//     layer  name=<paint layer>  min= edge=            weight ramps in over [min, min+edge]
//     noise  scale= octaves= seed= min= edge=          noise value ramps in over [min, min+edge]
//     spline name=<spline> half= edge=                 inside half-width cm, ramps out over edge cm
//     volume name=<volume> edge=                       inside the named box, ramps in over edge cm from its XY border
// each with an optional `invert=1`. A mask value in (0, 1) keeps a candidate with that probability from a seeded
// per-point hash, so the border of two biomes (one inverted) interleaves instead of drawing a hard line. edge=0 is a hard cut.
//
// An ENTRY is one foliage type with its placement rules; everything it leaves out reads as the default in the struct.
// density is instances per 100 m^2 (the unit a terrain layer's "Foliage per 100 m^2" already uses). `slope` is degrees from
// +Z, `height` world Z cm, `layer`/`layerMin` a paint layer weight gate, `noise=scale,min,max,seed` a noise patch gate,
// `clump=radiusCm,minCount,maxCount` makes density mean the total and groups it around parents, `priority` and `prune`
// (radius cm; default the foliage type's collisionRadiusCm) decide which type survives overlap.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct OcBiomeMask {
    enum class Kind : u8 { None = 0, Layer, Noise, Spline, Volume };
    Kind kind = Kind::None;
    std::string name;            // layer / spline / volume name
    f32 min = 0.5f;              // layer weight or noise value where the ramp starts
    f32 edge = 0.0f;             // ramp width: weight/noise units, or cm for spline and volume
    f32 halfWidthCm = 500.0f;    // spline
    f32 noiseScaleCm = 4000.0f;
    i32 noiseOctaves = 3;
    i32 noiseSeed = 0;
    bool invert = false;
};

struct OcBiomeEntry {
    std::string foliage;          // content-relative .ocfoliage path
    f32 densityPer100m2 = 10.0f;
    f32 slopeMinDeg = 0.0f, slopeMaxDeg = 90.0f;
    f32 heightMinCm = -1.0e9f, heightMaxCm = 1.0e9f;
    std::string layer;            // empty = no paint-layer gate
    f32 layerMin = 0.5f;
    f32 noiseScaleCm = 0.0f;      // 0 = no noise gate
    f32 noiseMin = 0.5f, noiseMax = 1.0f;
    i32 noiseSeed = 0;
    f32 clumpRadiusCm = 0.0f;     // 0 = no clumping
    i32 clumpMin = 3, clumpMax = 6;
    i32 priority = 0;             // higher wins an overlap; ties go to the larger prune radius
    f32 pruneRadiusCm = -1.0f;    // < 0 = the foliage type's collisionRadiusCm
};

struct OcBiomeData {
    std::string name;
    i32 priority = 0;             // added (x1000) to each entry's priority when several biomes compete
    OcBiomeMask mask;
    std::vector<OcBiomeEntry> entries;

    // Every entry names a foliage type, density > 0 and finite, slope/height/noise ranges ordered, clump counts 1 <= min <= max
    // with a positive radius, mask fields finite with edge >= 0 and (for layer/spline/volume) a name.
    bool valid(std::string* why = nullptr) const;
};

bool parseOcBiome(const std::string& text, OcBiomeData& out, std::string* why = nullptr);
bool loadOcBiome(const std::string& path, OcBiomeData& out, std::string* why = nullptr);
// Canonical text: parse(write(x)) == x and write(parse(write(x))) == write(x). Refuses an invalid biome.
bool writeOcBiome(const OcBiomeData& in, std::string& out, std::string* why = nullptr);
bool saveOcBiome(const std::string& path, const OcBiomeData& in, std::string* why = nullptr);

} // namespace aver::fmt
