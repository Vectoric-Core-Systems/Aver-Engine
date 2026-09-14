// W5: the sRGB decode lookup table in modules/formats/src/Texture.cpp. srgbToLinear(u8) went from a
// direct std::pow evaluation to a 256-entry table built once on first use -- see that function's own
// comment for why. This file exists to prove the table changed nothing a caller can observe: it holds
// a VERBATIM COPY of the pre-change srgbToLinear/linearToSrgb/downsample (the std::pow form) and
// compares the real, public generateMipChain()'s output against an independently-built reference chain,
// byte for byte, for every input this table could ever be asked to decode.
//
// KEPT IN SYNC BY HAND. If downsample()'s actual filtering logic in Texture.cpp ever changes for a
// reason other than the W5 table lookup, the ref:: copy below must change with it or this test stops
// proving what it claims to -- it would still pass, having quietly started comparing the real function
// against a description of itself that is no longer accurate.
#include "aver/formats/Texture.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <random>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures. Same shape as MeshTest.cpp's own check().
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace ref {

// ---- verbatim copy of Texture.cpp's PRE-CHANGE srgbToLinear/linearToSrgb/downsample ----
// Every line below is the std::pow form that shipped before the W5 table existed, restated here
// rather than reused from Texture.cpp because the whole point is an INDEPENDENT computation to
// compare the real (now table-driven) code against.

f32 srgbToLinear(u8 v) {
    const f32 c = v * (1.0f / 255.0f);
    return c <= 0.04045f ? c * (1.0f / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

u8 linearToSrgb(f32 c) {
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(s * 255.0f + 0.5f);
}

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

// The SAME loop shape as fmt::generateMipChain() (Texture.cpp), so a reference chain can be built
// independently of the function under test rather than by calling it.
fmt::TextureData mipChain(const ImageData& level0, bool srgb, bool normalMap) {
    fmt::TextureData t;
    t.width = level0.width;
    t.height = level0.height;
    t.srgb = srgb;
    t.levels.push_back(level0);
    while (t.levels.back().width > 1 || t.levels.back().height > 1)
        t.levels.push_back(downsample(t.levels.back(), t.srgb && !normalMap, normalMap));
    return t;
}

} // namespace ref

// Every level's dimensions and pixel bytes agree, level for level.
static bool sameChain(const fmt::TextureData& a, const fmt::TextureData& b) {
    if (a.levels.size() != b.levels.size()) return false;
    for (usize i = 0; i < a.levels.size(); ++i) {
        const ImageData& la = a.levels[i];
        const ImageData& lb = b.levels[i];
        if (la.width != lb.width || la.height != lb.height) return false;
        if (la.pixels != lb.pixels) return false;
    }
    return true;
}

// A solid-colour image: every texel's RGB channels are `v`, alpha opaque. Two texels wide/tall is the
// smallest input generateMipChain() still builds a real (non-trivial) mip chain from.
static ImageData makeSolid(u32 w, u32 h, u8 v, bool srgb) {
    ImageData img;
    img.width = w;
    img.height = h;
    img.channels = 4;
    img.srgb = srgb;
    img.pixels.resize(static_cast<usize>(w) * h * 4);
    for (usize p = 0; p < img.pixels.size(); p += 4) {
        img.pixels[p + 0] = v;
        img.pixels[p + 1] = v;
        img.pixels[p + 2] = v;
        img.pixels[p + 3] = 255;
    }
    return img;
}

// A pseudo-random image, deterministic across runs (fixed seed) so a failure reproduces exactly.
static ImageData makeRandom(u32 w, u32 h, bool srgb, u32 seed) {
    ImageData img;
    img.width = w;
    img.height = h;
    img.channels = 4;
    img.srgb = srgb;
    img.pixels.resize(static_cast<usize>(w) * h * 4);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    for (u8& p : img.pixels) p = static_cast<u8>(dist(rng));
    return img;
}

// Runs generateMipChain() through the real, public API and returns the resulting TextureData.
static fmt::TextureData realChain(const ImageData& level0, bool srgb, bool normalMap) {
    fmt::TextureData t;
    t.width = level0.width;
    t.height = level0.height;
    t.srgb = srgb;
    t.levels.push_back(level0);
    fmt::generateMipChain(t, normalMap);
    return t;
}

int main() {
    AVER_INFO("=== W5: sRGB lookup table -- every table entry, individually ===");
    {
        // A 2x2 solid texture per possible u8 value, so every one of the table's 256 entries is
        // exercised by name rather than only by whatever a random image happens to contain.
        for (u32 v = 0; v <= 255; ++v) {
            const u8 vv = static_cast<u8>(v);
            const ImageData src = makeSolid(2, 2, vv, /*srgb=*/true);
            const fmt::TextureData got  = realChain(src, true, false);
            const fmt::TextureData want = ref::mipChain(src, true, false);
            check(sameChain(got, want),
                  "2x2 solid v=" + std::to_string(v) + " sRGB mip chain is byte-identical to the "
                  "pre-table std::pow reference");
        }
    }

    AVER_INFO("=== W5: a seeded-random 64x48 sRGB texture, full mip chain ===");
    {
        const ImageData src = makeRandom(64, 48, /*srgb=*/true, /*seed=*/12345);
        const fmt::TextureData got  = realChain(src, true, false);
        const fmt::TextureData want = ref::mipChain(src, true, false);
        check(got.levels.size() > 1, "the 64x48 source actually built more than one mip level");
        check(sameChain(got, want),
              "64x48 random sRGB texture: the WHOLE chain is byte-identical to the reference");
    }

    AVER_INFO("=== W5: the linear (data) path is unaffected -- srgbToLinear never runs on it ===");
    {
        const ImageData src = makeRandom(33, 17, /*srgb=*/false, /*seed=*/999);
        const fmt::TextureData got  = realChain(src, false, false);
        const fmt::TextureData want = ref::mipChain(src, false, false);
        check(sameChain(got, want),
              "linear data texture: mip chain byte-identical (box filter only, no sRGB decode taken)");
    }

    AVER_INFO("=== W5: the normal-map path is unaffected -- it filters as vectors, not sRGB ===");
    {
        // As loadTexture() would hand it to generateMipChain(): TextureLoadOptions forces
        // srgb = opt.srgb && !opt.normalMap, so a normal map's TextureData::srgb is always false --
        // the table's caller-visible behaviour here is "never invoked", which is what this proves.
        const ImageData src = makeRandom(33, 17, /*srgb=*/false, /*seed=*/424242);
        const fmt::TextureData got  = realChain(src, false, true);
        const fmt::TextureData want = ref::mipChain(src, false, true);
        check(sameChain(got, want),
              "normal map texture: mip chain byte-identical (tangent-space vector path)");
    }

    if (g_failures == 0) AVER_INFO("=== all sRGB table tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
