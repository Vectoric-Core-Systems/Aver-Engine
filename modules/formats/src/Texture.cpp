// Texture asset loading and mip-chain generation.
#include "aver/formats/Texture.hpp"
#include "aver/assets/AssetId.hpp"
#include "aver/core/Log.hpp"

#include <cmath>

namespace aver::fmt {

namespace {

// The exact sRGB transfer curve, matching a *_UNORM_SRGB view's hardware decode.
f32 srgbToLinear(u8 v) {
    const f32 c = v * (1.0f / 255.0f);
    return c <= 0.04045f ? c * (1.0f / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
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
                const f32 len = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
                const f32 inv = len > 1e-6f ? 1.0f / len : 0.0f;
                const f32 n[3] = {len > 1e-6f ? acc[0] * inv : 0.0f,
                                  len > 1e-6f ? acc[1] * inv : 0.0f,
                                  len > 1e-6f ? acc[2] * inv : 1.0f};
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

} // namespace

// Builds levels 1..N from levels[0], replacing anything already there.
void generateMipChain(TextureData& t, bool normalMap) {
    if (!t.valid()) return;
    t.levels.resize(1);
    while (t.levels.back().width > 1 || t.levels.back().height > 1) {
        t.levels.push_back(downsample(t.levels.back(), t.srgb && !normalMap, normalMap));
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
