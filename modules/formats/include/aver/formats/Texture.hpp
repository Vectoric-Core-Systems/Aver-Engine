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
//
// preserveCoverageAt, DEFAULT OFF (-1.0f) so every existing caller -- including
// TextureSrgbTableTest's byte-exact comparison -- builds the identical chain it always has. >= 0.0f
// is the alpha-test REFERENCE VALUE (typically 0.5, in the same normalised [0,1] range downsample()
// already works in): each level past 0 has its alpha rescaled so the fraction of its texels at or
// above that reference matches level 0's fraction at the same reference. An alpha-tested texture
// loses COVERAGE, not just detail, as it shrinks -- a leaf card at 6% opaque coverage box-filters
// toward uniform low alpha within two or three halvings, and every texel of it starts failing the
// alpha test at once, which is moss and foliage thinning to nothing at a distance. Rescaling
// restores the fraction the alpha test cares about instead of leaving it to fall out of the filter.
// Never applied to a normal map (downsample() does not treat ITS alpha as coverage either).
void generateMipChain(TextureData& t, bool normalMap, f32 preserveCoverageAt = -1.0f);

// True when `img`'s alpha reads as an alpha-tested CUTOUT (a leaf, a fence, a chain-link fabric)
// rather than a blended or opaque one: at least 1% of texels sit below the 0.5 reference AND at
// least 85% sit near one of the two ends (<=16 or >=239) -- a cutout's histogram piles up at fully
// opaque and fully transparent with a real, non-trivial slice actually near the cut; a blended
// alpha (glass, a soft decal edge) spreads across the whole range instead. Feeds
// generateMipChain's preserveCoverageAt; see it for why the distinction matters.
bool alphaLooksLikeCutout(const ImageData& img);

} // namespace aver::fmt
