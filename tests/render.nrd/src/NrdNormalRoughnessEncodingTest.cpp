// NrdNormalRoughnessEncodingTest -- the G-buffer's normal/roughness packing round-trips through
// NRD's own decoder, within R10G10B10A2's precision.
//
// WHAT THIS DECIDES THAT NEITHER OTHER SUITE IN THIS MODULE DOES. NrdLinkTest proves the library
// links and plans a frame; NrdRecordTest proves that plan records onto a real command list. Neither
// looks at a single bit of what ends up IN the normal/roughness texture -- and that texture's
// CONTENTS is exactly where this module's real bug lived: NRD was built for
// NRD_NORMAL_ENCODING_R10G10B10A2_UNORM (an octahedral-style pack where roughness rides in z, its
// sign carrying n.z's sign), and the G-buffer was writing NRD's #else layout instead (plain
// normal*0.5+0.5 with roughness in w) -- the SAME TEXTURE FORMAT, the WRONG CONTENTS. A build that
// links, a plan that records, and an image that renders all say nothing about that, which is why
// the mismatch survived a whole session as a prose comment asserting the wrong thing.
//
// CPU-ONLY, DELIBERATELY: averPackNormalRoughness (modules/render.voxi/shaders/voxi.hlsl) and NRD's
// own _NRD_DecodeNormalRoughness101010 (third_party/nrd/Shaders/NRD.hlsli) are both pure arithmetic
// on three floats -- nothing here needs a GPU, a device, or even AVER_WITH_NRD, so this test
// transcribes the same sequence of operations in C++ instead of standing up a device to run the
// real HLSL. See NrdLinkTest/NrdRecordTest's own split of "needs a device" vs "doesn't" for the
// precedent this follows; unlike both of those, this one runs identically on every machine and
// every configuration, gated on nothing.
//
// THE NEGATIVE CONTROL IS THE POINT. This bug class -- a wrong channel LAYOUT rather than a wrong
// VALUE -- is silent by construction: the shader still compiles, the texture still binds, the
// denoiser still runs and still paints something. A round-trip test that can only ever pass proves
// nothing about whether it would have CAUGHT the actual defect, so this suite also packs known
// normals with the OLD naive layout (normal*0.5+0.5, roughness in w) and asserts NRD's real decoder
// recovers something WRONG from them. If that assertion ever starts passing, this test has quietly
// stopped being able to detect the bug it was written for.
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

// One packed RGB10A2 sample, still in [0,1] float -- quantizeR10G10B10A2 below is what actually
// models the format's precision loss; this struct alone is just algebra.
struct Packed { float x = 0, y = 0, z = 0; };
struct Decoded { float x = 0, y = 0, z = 0, roughness = 0; };

float normalizeInPlace(float& x, float& y, float& z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-8f) { x /= len; y /= len; z /= len; }
    return len;
}

// Mirrors averPackNormalRoughness (modules/render.voxi/shaders/voxi.hlsl), which itself transcribes
// NRD's _NRD_EncodeNormalRoughness101010 (third_party/nrd/Shaders/NRD.hlsli) byte-exact -- see that
// HLSL function's own comment for why transcribed rather than included. `n` need not already be
// unit length; the L1-normalize below is the first step of the real algorithm, not a shortcut this
// test takes.
Packed packNrdEncoding(float nx, float ny, float nz, float roughness) {
    const float l1 = std::abs(nx) + std::abs(ny) + std::abs(nz);
    nx /= l1; ny /= l1; nz /= l1;

    Packed r;
    r.y = ny * 0.5f + 0.5f;
    r.x = nx * 0.5f + r.y;
    r.y -= nx * 0.5f;

    // Can't be exactly 0, or it erases n.z's sign bit -- same clamp as the HLSL original.
    roughness = std::max(roughness, 1.5f / 512.0f);
    const float s = nz < 0.0f ? -roughness : roughness;
    r.z = s * 0.5f + 0.5f;
    return r;
}

// THE BUG THIS MODULE SHIPPED, kept here ONLY to drive the negative control. Not a general-purpose
// helper -- it exists so the test can prove it fails against NRD's real decoder, not merely assert
// that the fixed packer passes.
Packed packNaiveLayout(float nx, float ny, float nz, float /*roughness*/) {
    // roughness went into w under this layout, which decodeNrdEncoding below never reads -- exactly
    // the shape of the original defect: a caller downstream never sees it either.
    return Packed{ nx * 0.5f + 0.5f, ny * 0.5f + 0.5f, nz * 0.5f + 0.5f };
}

// _NRD_DecodeNormalRoughness101010, transcribed from third_party/nrd/Shaders/NRD.hlsli (see
// packNrdEncoding's own comment for why transcribed, not included), followed by
// _NRD_SafeNormalize's own rsqrt(dot+1e-9) -- NRD_FrontEnd_UnpackNormalAndRoughness applies exactly
// that normalize to r.xyz after this same decode, so leaving it out would test an incomplete
// pipeline.
Decoded decodeNrdEncoding(const Packed& p) {
    const float t = p.z * 2.0f - 1.0f;   // signed roughness

    Decoded r;
    r.x = p.x - p.y;
    r.y = p.x + p.y - 1.0f;
    r.z = (t < 0.0f ? -1.0f : 1.0f) * (1.0f - std::abs(r.x) - std::abs(r.y));
    r.roughness = std::abs(t);

    const float invLen = 1.0f / std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + 1e-9f);
    r.x *= invLen; r.y *= invLen; r.z *= invLen;
    return r;
}

// Models the RGB10A2_UNORM store this texture format actually performs (rhi::Format::RGB10A2Unorm,
// RHI.hpp's gBufferNormalRoughnessTexture): 10 bits per channel, [0,1] clamped and rounded to the
// nearest of 1024 levels. Comparing exact floats without this step would validate ALGEBRA, not the
// FORMAT -- and quantization is exactly where a near-boundary value (a grazing normal, a
// near-minimum roughness) is most likely to misbehave.
Packed quantizeR10G10B10A2(const Packed& p) {
    auto q = [](float v) {
        v = std::max(0.0f, std::min(1.0f, v));
        return std::round(v * 1023.0f) / 1023.0f;
    };
    return Packed{ q(p.x), q(p.y), q(p.z) };
}

// 10-bit quantization plus the decode's own divisions can move a component by a couple of
// thousandths; this is generous enough to absorb that everywhere in the test below while still
// being far tighter than the errors the negative control below produces (tenths, not thousandths).
constexpr float kTolerance = 0.02f;

struct Case { float nx, ny, nz, roughness; const char* label; };

// Deliberately including: a negative z (the sign-in-z trick's whole reason to exist), grazing
// normals where n.z is near zero (the boundary the sign trick has to get right), an input roughness
// of exactly 0 (must clamp away from it without losing the sign bit), and both signs on every axis.
const Case kCases[] = {
    { 0.0f,      0.0f,      1.0f,      0.5f,   "n=+Z, mid roughness" },
    { 0.0f,      0.0f,     -1.0f,      0.5f,   "n=-Z, mid roughness (negative z)" },
    { 1.0f,      0.0f,      0.0f,      0.1f,   "n=+X, low roughness" },
    { -1.0f,     0.0f,      0.0f,      0.9f,   "n=-X, high roughness" },
    { 0.0f,      1.0f,      0.0f,      0.3f,   "n=+Y" },
    { 0.0f,     -1.0f,      0.0f,      0.7f,   "n=-Y" },
    { 0.577350f, 0.577350f, 0.577350f, 0.02f,  "n=(1,1,1)/sqrt3, near-minimum roughness" },
    { 0.577350f, 0.577350f, -0.577350f,0.02f,  "n=(1,1,-1)/sqrt3, negative z, near-minimum roughness" },
    { 0.707107f, 0.707107f, 0.001f,    0.5f,   "grazing normal, n.z just above 0" },
    { 0.707107f, 0.707107f, -0.001f,   0.5f,   "grazing normal, n.z just below 0 (sign-flip test)" },
    { 0.0f,      0.0f,      1.0f,      0.0f,   "roughness=0 input still clamps away from exactly 0" },
};

}  // namespace

int main() {
    AVER_INFO("=== NrdNormalRoughnessEncodingTest ===");

    // ---- the round trip: pack -> quantize (models the real texture format) -> decode ------------
    for (const Case& c : kCases) {
        float nx = c.nx, ny = c.ny, nz = c.nz;
        normalizeInPlace(nx, ny, nz);
        const float expectedRoughness = std::max(c.roughness, 1.5f / 512.0f);

        const Packed  packed  = quantizeR10G10B10A2(packNrdEncoding(nx, ny, nz, c.roughness));
        const Decoded decoded = decodeNrdEncoding(packed);

        const float dot = decoded.x * nx + decoded.y * ny + decoded.z * nz;
        check(dot > 1.0f - kTolerance,
              std::string(c.label) + ": decoded normal matches within tolerance (dot=" +
              std::to_string(dot) + ")");
        check(std::abs(decoded.roughness - expectedRoughness) < kTolerance,
              std::string(c.label) + ": decoded roughness matches within tolerance (got " +
              std::to_string(decoded.roughness) + ", expected " +
              std::to_string(expectedRoughness) + ")");
    }

    // ---- the negative control: the OLD layout must NOT survive NRD's decoder ----------------------
    // n=(1,0,0) is a good witness: naive-packed it decodes (via the real NRD maths above) to
    // roughly (0.707, 0.707, 0) -- a ~45-degree miss, not a rounding error. A test that let this
    // pass would not have caught the bug it exists for.
    AVER_INFO("(the next result is EXPECTED to fail the match -- it is the negative control)");
    {
        const Packed  naive   = quantizeR10G10B10A2(packNaiveLayout(1.0f, 0.0f, 0.0f, 0.5f));
        const Decoded decoded = decodeNrdEncoding(naive);
        const float dot = decoded.x * 1.0f + decoded.y * 0.0f + decoded.z * 0.0f;
        AVER_INFO("  naive-layout decode: ({:.4f}, {:.4f}, {:.4f}), dot-with-truth={:.4f}",
                  decoded.x, decoded.y, decoded.z, dot);
        check(dot < 1.0f - kTolerance,
              "NEGATIVE CONTROL: the naive (normal*0.5+0.5) layout decodes WRONG through NRD's "
              "decoder -- if this ever starts matching, the test has stopped being able to detect "
              "the encoding-mismatch bug class it was written for");
    }

    if (g_failures != 0) {
        AVER_ERROR("=== NrdNormalRoughnessEncodingTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== NrdNormalRoughnessEncodingTest passed ===");
    return 0;
}
