#pragma once
// The texture ASSET boundary. loadTexture dispatches on assetTypeFromPath the same way loadOcbeam /
// loadOcmap / loadOcproject sit behind their extensions, and hands back decoded levels ready to
// become a TextureDesc's initialData.
//
// The material system speaks ImageData and nothing else -- never a file format, never a codec. That
// seam is what lets a future asset cooker (which would emit block-compressed levels straight out of
// a .octex) drop in without touching a line of PBR code.
#include "aver/core/Types.hpp"
#include "aver/platform/Image.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct TextureLoadOptions {
    // Does the file hold COLOUR (sRGB-encoded) or DATA (linear)? No common image container states
    // this, so the caller must: a base-colour map is sRGB, a roughness/metallic/AO/normal map is not.
    bool srgb = true;
    // Normal maps filter as VECTORS. Taps are averaged and renormalised, and srgb is forced off --
    // an sRGB decode of a normal map bends the vectors, which shows up as lighting that is subtly
    // wrong everywhere and obviously wrong nowhere.
    bool normalMap = false;
    bool generateMips = true;
};

// A full mip chain. levels[0] is the source image; each subsequent level halves both extents,
// bottoming out at 1x1. Every level is ImageData, so the upload path needs no second vocabulary.
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    bool srgb = true;
    std::vector<ImageData> levels;

    bool valid() const { return !levels.empty() && levels.front().valid(); }
};

bool loadTexture(const std::string& path, TextureData& out,
                 const TextureLoadOptions& opt = {}, std::string* err = nullptr);

// Build levels 1..N from levels[0], replacing anything already there. Exposed on its own because a
// caller that decoded an image some other way (an embedded blob, a procedural source) still needs
// mips filtered CORRECTLY, and "correctly" is content-dependent: it needs to know whether the
// pixels are sRGB-encoded and whether they are a vector field. That is exactly why this cannot be a
// generic RHI service -- the RHI is not allowed to know what a normal map is.
void generateMipChain(TextureData& t, bool normalMap);

} // namespace aver::fmt
