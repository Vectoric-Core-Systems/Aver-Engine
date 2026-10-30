#pragma once
// An equirectangular panorama (an HDRI) fitted onto Aver's authored sky dome. A panorama sky becomes a SKY record: the dome has a
// zenith colour, a horizon colour, a blend exponent and a ground albedo, so the fit takes band averages of the panorama by elevation. The renderers
// do not sample the panorama itself (the ray-driven pass draws the primary sky and may not be touched), so this is
// an approximation of the sky's brightness and colour by height, not its detail.
//
// Convention: row 0 is straight up, the last row straight down, columns wrap around the vertical axis (Y up in the source; Aver's Z up is the same
// elevation). Pixels are linear, scene-referred radiance.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct PanoramaImage {
    u32 width = 0, height = 0;
    std::vector<f32> rgb;   // width * height * 3, linear
    bool valid() const { return width > 0 && height > 0 && rgb.size() == static_cast<usize>(width) * height * 3; }
};

// Radiance .hdr (RGBE, flat or run-length encoded) from memory. False with `err` on anything it cannot read.
bool decodeRadianceHdr(const u8* bytes, usize size, PanoramaImage& out, std::string* err = nullptr);
// A panorama file: .hdr through decodeRadianceHdr, anything the image decoder reads (png, jpg, tga) as 8-bit sRGB decoded to linear.
// OpenEXR is not read. False with `err` when the file cannot be read.
bool loadPanorama(const std::string& path, PanoramaImage& out, std::string* err = nullptr);

struct SkyDomeFit {
    f64 zenith[3] = {0, 0, 0};    // authored (display-encoded) colour, as SKY zenith / horizon: radiance ^ (1/2.2); may exceed 1 for HDR skies
    f64 horizon[3] = {0, 0, 0};
    f64 groundAlbedo[3] = {0, 0, 0};   // lower hemisphere's mean radiance over the upper hemisphere's, per channel, in [0,1]
    f64 domeExponent = 0.65;           // the blend exponent that best follows the luminance by elevation; zenith and horizon are solved for it
                                       // (the dome reaches `horizon` only straight down, so it is not the colour seen at the horizon line)
};

// Fits the dome to `img` scaled by `energy` (the energy multiplier). Rotation about the vertical axis does not change
// a dome that depends on elevation only, so it is not a parameter.
bool fitSkyDome(const PanoramaImage& img, f64 energy, SkyDomeFit& out);

}  // namespace aver::fmt
