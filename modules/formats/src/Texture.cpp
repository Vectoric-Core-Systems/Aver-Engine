// Texture asset loading and mip-chain generation.
#include "aver/formats/Texture.hpp"
#include "aver/assets/AssetId.hpp"
#include "aver/core/Log.hpp"

#include <array>
#include <cmath>

namespace aver::fmt {

namespace {

// The exact sRGB transfer curve, matching a *_UNORM_SRGB view's hardware decode.
//
// W5: LOOKED UP, NOT RECOMPUTED, but bit-identically -- std::pow ran once per tap per channel inside
// downsample() (below), which is 3 calls per source texel per mip level, and a project's textures all
// decode through this same function on every open. The table has exactly 256 entries because the
// input is a u8, so it is complete: every value this function could ever be asked for is already in
// it, built once on first use rather than recomputed on every call.
//
// BIT-IDENTICAL BY CONSTRUCTION: kTable[i] is computed by the exact same expression this function
// used to evaluate directly, from the exact same u8 promotion path (v -> c = v * (1/255) -> the
// branch), with no /fp:fast anywhere in this tree (checked: no CMakeLists.txt here sets it) to let the
// compiler reorder or approximate either version differently. TextureSrgbTableTest compares
// generateMipChain's output against a verbatim copy of the pre-change std::pow form, byte for byte,
// for exactly this reason -- a table that was merely CLOSE would still change every decoded texel in
// the project.
//
// The hitch this saves is UNMEASURED: nothing here has timed a project open before and after.
f32 srgbToLinear(u8 v) {
    static const std::array<f32, 256> kTable = [] {
        std::array<f32, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const u8 vv = static_cast<u8>(i);
            const f32 c = vv * (1.0f / 255.0f);
            t[static_cast<usize>(i)] =
                c <= 0.04045f ? c * (1.0f / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return kTable[v];
}
// The inverse: linear to an 8-bit sRGB-encoded value.
u8 linearToSrgb(f32 c) {
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(s * 255.0f + 0.5f);
}

// One box-filter halving step. Filters in linear space for sRGB and as vectors for a normal map.
ImageData downsample(const ImageData& src, bool srgb, bool normalMap) {
    ImageData dst;
    dst.width  = src.width  > 1 ? src.width  / 2 : 1;
    dst.height = src.height > 1 ? src.height / 2 : 1;
    dst.channels = src.channels;
    dst.srgb = src.srgb;
    dst.pixels.resize(static_cast<usize>(dst.width) * dst.height * 4);

    for (u32 y = 0; y < dst.height; ++y) {
        const u32 sy0 = y * 2 < src.height ? y * 2 : src.height - 1;
        const u32 sy1 = sy0 + 1 < src.height ? sy0 + 1 : sy0;
        for (u32 x = 0; x < dst.width; ++x) {
            const u32 sx0 = x * 2 < src.width ? x * 2 : src.width - 1;
            const u32 sx1 = sx0 + 1 < src.width ? sx0 + 1 : sx0;
            const usize tap[4] = {
                (static_cast<usize>(sy0) * src.width + sx0) * 4,
                (static_cast<usize>(sy0) * src.width + sx1) * 4,
                (static_cast<usize>(sy1) * src.width + sx0) * 4,
                (static_cast<usize>(sy1) * src.width + sx1) * 4,
            };

            f32 acc[4] = {0, 0, 0, 0};
            for (const usize t : tap) {
                if (normalMap) {
                    for (int c = 0; c < 3; ++c) acc[c] += src.pixels[t + c] * (2.0f / 255.0f) - 1.0f;
                } else if (srgb) {
                    for (int c = 0; c < 3; ++c) acc[c] += srgbToLinear(src.pixels[t + c]);
                } else {
                    for (int c = 0; c < 3; ++c) acc[c] += src.pixels[t + c] * (1.0f / 255.0f);
                }
                acc[3] += src.pixels[t + 3] * (1.0f / 255.0f);   // alpha is linear coverage, always
            }
            for (f32& a : acc) a *= 0.25f;

            u8* o = &dst.pixels[(static_cast<usize>(y) * dst.width + x) * 4];
            if (normalMap) {
                // NOT renormalised: the averaged vector's length is how much the normals under this
                // texel disagree, which the shader's specular anti-aliasing reads (Toksvig). Floored at
                // 0.25 so opposing normals keep a direction to renormalise.
                const f32 len = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
                const f32 k = len > 1e-6f ? (len < 0.25f ? 0.25f / len : 1.0f) : 0.0f;
                const f32 n[3] = {len > 1e-6f ? acc[0] * k : 0.0f,
                                  len > 1e-6f ? acc[1] * k : 0.0f,
                                  len > 1e-6f ? acc[2] * k : 0.25f};
                for (int c = 0; c < 3; ++c) {
                    const f32 e = n[c] * 0.5f + 0.5f;
                    o[c] = static_cast<u8>((e < 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e)) * 255.0f + 0.5f);
                }
            } else if (srgb) {
                for (int c = 0; c < 3; ++c) o[c] = linearToSrgb(acc[c]);
            } else {
                for (int c = 0; c < 3; ++c) {
                    const f32 v = acc[c] < 0.0f ? 0.0f : (acc[c] > 1.0f ? 1.0f : acc[c]);
                    o[c] = static_cast<u8>(v * 255.0f + 0.5f);
                }
            }
            const f32 a = acc[3] < 0.0f ? 0.0f : (acc[3] > 1.0f ? 1.0f : acc[3]);
            o[3] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }
    return dst;
}

// The fraction of `level`'s texels at or above `refAlpha01` (normalised [0,1]) -- exactly what an
// alpha test at that reference value would keep.
f32 alphaCoverage(const ImageData& level, f32 refAlpha01) {
    const usize count = static_cast<usize>(level.width) * level.height;
    if (count == 0) return 0.0f;
    const u8 ref = static_cast<u8>(refAlpha01 * 255.0f + 0.5f);
    usize kept = 0;
    for (usize i = 0; i < count; ++i)
        if (level.pixels[i * 4 + 3] >= ref) ++kept;
    return static_cast<f32>(kept) / static_cast<f32>(count);
}

// Rescales `level`'s alpha so ITS OWN coverage at `refAlpha01` matches `targetCoverage` (level 0's
// coverage at the same reference) -- the standard alpha-test mip fix. Histograms the level's alpha,
// walks it from 255 down to find the threshold t whose cumulative count already covers the target
// fraction, then scales every alpha by refAlpha01*255 / t: a texel exactly at t lands back on the
// reference and everything else moves proportionally with it.
void rescaleAlphaForCoverage(ImageData& level, f32 targetCoverage, f32 refAlpha01) {
    const usize count = static_cast<usize>(level.width) * level.height;
    if (count == 0) return;

    u32 hist[256] = {};
    for (usize i = 0; i < count; ++i) ++hist[level.pixels[i * 4 + 3]];

    const u32 wantKept = static_cast<u32>(targetCoverage * static_cast<f32>(count) + 0.5f);
    u32 cumulative = 0;
    i32 t = 0;
    for (i32 v = 255; v >= 0; --v) {
        cumulative += hist[static_cast<usize>(v)];
        if (cumulative >= wantKept) { t = v; break; }
    }
    if (t <= 0) return;   // level is already all-or-nothing -- nothing to rescale against

    const f32 scale = (refAlpha01 * 255.0f) / static_cast<f32>(t);
    for (usize i = 0; i < count; ++i) {
        u8& a = level.pixels[i * 4 + 3];
        const f32 scaled = static_cast<f32>(a) * scale;
        const f32 clamped = scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled);
        a = static_cast<u8>(clamped + 0.5f);
    }
}

} // namespace

// True when `img`'s alpha reads as an alpha-tested cutout. See the header for the exact rule and
// why it is stated this way.
bool alphaLooksLikeCutout(const ImageData& img) {
    if (!img.valid()) return false;
    const usize count = static_cast<usize>(img.width) * img.height;
    if (count == 0) return false;

    usize belowHalf = 0, nearBinary = 0;
    for (usize i = 0; i < count; ++i) {
        const u8 a = img.pixels[i * 4 + 3];
        if (a < 128) ++belowHalf;                 // < 0.5 normalised
        if (a <= 16 || a >= 239) ++nearBinary;     // near-fully-transparent or near-fully-opaque
    }
    const f32 n = static_cast<f32>(count);
    return (static_cast<f32>(belowHalf) / n) >= 0.01f &&
           (static_cast<f32>(nearBinary) / n) >= 0.85f;
}

// Builds levels 1..N from levels[0], replacing anything already there.
void generateMipChain(TextureData& t, bool normalMap, f32 preserveCoverageAt) {
    if (!t.valid()) return;
    t.levels.resize(1);
    while (t.levels.back().width > 1 || t.levels.back().height > 1) {
        t.levels.push_back(downsample(t.levels.back(), t.srgb && !normalMap, normalMap));
    }
    // COVERAGE-PRESERVING ALPHA, OFF BY DEFAULT (preserveCoverageAt < 0) and never for a normal
    // map: a normal map's alpha is not a coverage channel to begin with (downsample() filters it
    // the same as any other channel, never as a test), so there is nothing here to preserve.
    if (preserveCoverageAt >= 0.0f && !normalMap && t.levels.size() > 1) {
        const f32 targetCoverage = alphaCoverage(t.levels.front(), preserveCoverageAt);
        for (usize i = 1; i < t.levels.size(); ++i)
            rescaleAlphaForCoverage(t.levels[i], targetCoverage, preserveCoverageAt);
    }
}

// Loads and decodes a texture asset, optionally generating its mip chain.
bool loadTexture(const std::string& path, TextureData& out, const TextureLoadOptions& opt,
                 std::string* err) {
    const AssetType type = assetTypeFromPath(path);
    if (type != AssetType::Texture) {
        if (err) *err = "not a texture asset: " + path;
        return false;
    }

    ImageData img;
    if (!decodeImage(path, img, err)) return false;

    out = TextureData{};
    out.width = img.width;
    out.height = img.height;
    out.srgb = opt.srgb && !opt.normalMap;
    img.srgb = out.srgb;
    out.levels.push_back(std::move(img));

    if (opt.generateMips) generateMipChain(out, opt.normalMap);
    return true;
}

} // namespace aver::fmt
