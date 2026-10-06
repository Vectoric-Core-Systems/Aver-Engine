#pragma once
// IES (ANSI/IES LM-63) photometric profiles: parser, symmetry-aware lookup, and the baked table a
// light uploads to the GPU. Type C photometry only (the luminaire convention); type A/B files are
// refused with a reason. See docs/rendering/LIGHTS.md.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// A parsed profile. `candela` is row-major by horizontal angle: candela[h * vertical.size() + v],
// already multiplied by the file's candela multiplier.
struct IesProfile {
    std::string name;                 // [LUMINAIRE] or [TEST] keyword text, if any
    std::vector<f32> vertical;        // degrees, ascending, within [0,180]
    std::vector<f32> horizontal;      // degrees, ascending, as the file lists them
    std::vector<f32> candela;
    i32  lampCount = 1;
    f32  lumensPerLamp = 0.0f;        // -1 in the file means absolute photometry; stored as 0
    f32  multiplier = 1.0f;
    f32  inputWatts = 0.0f;
    f32  maxCandela = 0.0f;
    bool valid() const { return !vertical.empty() && !horizontal.empty() &&
                                candela.size() == vertical.size() * horizontal.size(); }
};

// Parses LM-63 text. False with *err set when the file is not a usable type C profile.
bool parseIes(std::string_view text, IesProfile& out, std::string* err = nullptr);
bool loadIes(const std::string& path, IesProfile& out, std::string* err = nullptr);

// Candela toward (vDeg, hDeg): vDeg is the angle from the luminaire's nadir axis [0,180], hDeg the
// angle around it. Unfolds the file's horizontal symmetry (none, quadrant, bilateral, rotational),
// interpolates bilinearly, and returns 0 outside the vertical range the file defines.
f32 iesCandela(const IesProfile& p, f32 vDeg, f32 hDeg);

// GPU table. vCount rows (vertical, 0..180 inclusive, texel j = j*180/(vCount-1) degrees) by hCount
// columns (horizontal, wrapping, texel i = i*360/hCount degrees). Values are relative intensity,
// normalised so the sphere average is 1: scaling by a light's candela keeps its luminous flux
// (4*pi*candela lumens) whatever the profile's shape. The shader mirrors these constants.
inline constexpr u32 kIesTableV = 64;
inline constexpr u32 kIesTableH = 32;

struct IesTable {
    std::vector<f32> relative;        // kIesTableV * kIesTableH, row = vertical
    f32 peakOverMean = 1.0f;          // peak relative intensity; 1/this renormalises to "peak = 1"
    bool valid() const { return relative.size() == static_cast<usize>(kIesTableV) * kIesTableH; }
};

// Bakes a profile into the table. False (and an empty table) when the profile has no emission.
bool bakeIesTable(const IesProfile& p, IesTable& out);

// IEEE half conversion for uploading the table as R16F.
u16 floatToHalf(f32 v);
std::vector<u16> iesTableToHalf(const IesTable& t);

} // namespace aver::fmt
