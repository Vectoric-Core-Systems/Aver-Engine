#pragma once
// Strand curves from a model file into explicit .ochair strands (design 2.13, source 2). The glTF importer (Aver.Formats) reads LINES and
// LINE_STRIP primitives into ImportedHairCurves; this turns them into strands the physics can take: at least 3 points, at most 255, no two
// neighbours coincident, a width per strand.
//
// Pure arithmetic on arrays, like the rest of Aver.Hair.
#include "aver/core/Types.hpp"
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/OcHair.hpp"

#include <string>

namespace aver::hair {

struct CurveImportOptions {
    f32 minSegmentCm = 0.01f;       // points closer than this to the one before are dropped (a rod of zero length cannot simulate)
    u32 maxPoints = fmt::kOcHairMaxStrandVertices;   // a longer strand is resampled to this many points by arc length
    f32 defaultWidthCm = 0.0f;      // written as the strand width when the file had none; 0 = no width table at all
};

struct CurveImportReport {
    u32 strandsIn = 0;
    u32 strandsOut = 0;
    u32 droppedShort = 0;           // fewer than 3 usable points
    u32 droppedPoints = 0;          // coincident neighbours removed
    u32 resampled = 0;              // strands longer than maxPoints
    u32 materialsClamped = 0;       // glTF material indices above 255
};

// Converts. False with `why` set when the file had no curve or none survived.
bool curvesToStrands(const fmt::ImportedHairCurves& in, fmt::OcHairStrands& out, const CurveImportOptions& opt = {},
                     CurveImportReport* report = nullptr, std::string* why = nullptr);

} // namespace aver::hair
