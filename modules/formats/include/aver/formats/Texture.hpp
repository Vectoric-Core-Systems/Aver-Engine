#pragma once
// The texture ASSET boundary: loadTexture dispatches on the asset type and hands back decoded
// ImageData levels ready to become a TextureDesc's initialData.
#include "aver/core/Types.hpp"
#include "aver/platform/Image.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// What the caller must state about the file's content, since no common container does.
struct TextureLoadOptions {
    bool srgb = true;        // true for colour (sRGB-encoded), false for data (linear)
    bool normalMap = false;  // filter as vectors, and force srgb off
    bool generateMips = true;
};

// A full mip chain. levels[0] is the source image; each level halves both extents down to 1x1.
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    bool srgb = true;
    std::vector<ImageData> levels;

    bool valid() const { return !levels.empty() && levels.front().valid(); }
};

// Loads and decodes a texture asset.
bool loadTexture(const std::string& path, TextureData& out,
                 const TextureLoadOptions& opt = {}, std::string* err = nullptr);

// Builds levels 1..N from levels[0], replacing anything already there. Filters in linear space for
// sRGB content and as vectors for a normal map.
void generateMipChain(TextureData& t, bool normalMap);

} // namespace aver::fmt
