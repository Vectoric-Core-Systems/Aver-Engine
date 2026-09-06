#include "aver/formats/OcLightmap.hpp"

#include "aver/formats/Avr1.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aver::fmt {
namespace {

constexpr u32 kSubtypeLightmap = avrFourCC("LMAP");
constexpr u32 kChunkHeader     = avrFourCC("LMHD");
constexpr u32 kChunkUv         = avrFourCC("LMUV");
constexpr u32 kChunkTexels     = avrFourCC("LMTX");

// width, height, vertexCount, sourceHash, and the source path as a u32 length + bytes. The path is
// LAST so the fixed part stays at a constant offset -- a reader can validate the numbers before it
// has to trust a length it read out of the same file.
constexpr usize kFixedHeaderBytes = 4 + 4 + 4 + 8 + 4;

void putU32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}
void putU64(std::vector<u8>& b, u64 v) {
    putU32(b, static_cast<u32>(v & 0xFFFFFFFFull));
    putU32(b, static_cast<u32>(v >> 32));
}
void putF32(std::vector<u8>& b, f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(b, bits);
}
u32 getU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
u64 getU64(const u8* p) { return static_cast<u64>(getU32(p)) | (static_cast<u64>(getU32(p + 4)) << 32); }
f32 getF32(const u8* p) {
    const u32 bits = getU32(p);
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// RGB9E5: three 9-bit mantissas sharing one 5-bit exponent, the layout D3D calls
// R9G9B9E5_SHAREDEXP and Vulkan calls E5B9G9R9_UFLOAT_PACK32.
//
// WRITTEN OUT RATHER THAN SHARED WITH GPU CODE, deliberately: nothing in this engine samples a
// lightmap on the GPU yet, so there is no shared definition to reuse and inventing one now would be
// a second source of truth for a consumer that does not exist. When a sampler appears, the honest
// move is to hand the GPU the packed u32s directly in that format -- at which point this becomes
// the reference these bits are checked against, not a duplicate of it.
constexpr i32 kMantissaBits = 9;
constexpr i32 kExpBias      = 15;
constexpr i32 kMaxExp       = 31;
// The largest representable value: mantissa all ones, exponent at its ceiling.
constexpr f32 kMaxRgb9e5 =
    static_cast<f32>((1 << kMantissaBits) - 1) / static_cast<f32>(1 << kMantissaBits) *
    static_cast<f32>(1ull << (kMaxExp - kExpBias));

// NEGATIVES AND NaN CLAMP TO ZERO RATHER THAN WRAPPING. This encoding has no sign bit, so a
// negative input has no representation at all -- and silently reinterpreting its bits would
// reproduce exactly the failure the negative-radiance work documents, where a bad value read back
// as a confident bright one instead of an obviously broken one. Clamping is lossy and says so;
// wrapping would be lossy and lie.
f32 clampChannel(f32 v) {
    if (!(v > 0.0f)) return 0.0f;   // false for NaN as well as for negatives and zero
    return v > kMaxRgb9e5 ? kMaxRgb9e5 : v;
}

u32 encodeRGB9E5(f32 r, f32 g, f32 b) {
    r = clampChannel(r); g = clampChannel(g); b = clampChannel(b);
    const f32 maxc = (r > g ? (r > b ? r : b) : (g > b ? g : b));
    if (maxc <= 0.0f) return 0;

    // The shared exponent is chosen from the BRIGHTEST channel, which is what makes the other two
    // lose precision to it -- the trade the header argues for.
    i32 exp = static_cast<i32>(std::floor(std::log2(maxc))) + 1 + kExpBias;
    if (exp < 0) exp = 0;
    if (exp > kMaxExp) exp = kMaxExp;

    f32 scale = std::pow(2.0f, static_cast<f32>(exp - kExpBias - kMantissaBits));
    // One correction step: log2+floor can land a boundary value one exponent too low, and the
    // rounded mantissa then overflows its 9 bits. Cheaper and clearer than getting the boundary
    // exactly right analytically.
    i32 maxm = static_cast<i32>(std::floor(maxc / scale + 0.5f));
    if (maxm == (1 << kMantissaBits)) { ++exp; scale *= 2.0f; }
    if (exp > kMaxExp) { exp = kMaxExp; scale = std::pow(2.0f, static_cast<f32>(exp - kExpBias - kMantissaBits)); }

    auto quantise = [scale](f32 c) {
        i32 m = static_cast<i32>(std::floor(c / scale + 0.5f));
        if (m < 0) m = 0;
        if (m > (1 << kMantissaBits) - 1) m = (1 << kMantissaBits) - 1;
        return static_cast<u32>(m);
    };
    return quantise(r) | (quantise(g) << 9) | (quantise(b) << 18) |
           (static_cast<u32>(exp) << 27);
}

LightmapTexel decodeRGB9E5(u32 v) {
    const i32 exp = static_cast<i32>(v >> 27);
    const f32 scale = std::pow(2.0f, static_cast<f32>(exp - kExpBias - kMantissaBits));
    LightmapTexel t;
    t.r = static_cast<f32>(v & 0x1FF) * scale;
    t.g = static_cast<f32>((v >> 9) & 0x1FF) * scale;
    t.b = static_cast<f32>((v >> 18) & 0x1FF) * scale;
    return t;
}

} // namespace

usize ocLightmapBytes(const OcLightmap& lm) {
    return usize(lm.vertexCount) * 2 * sizeof(f32) + usize(lm.width) * usize(lm.height) * 4;
}

bool writeOcLightmap(const std::string& path, const OcLightmap& lm, std::string* err) {
    if (!lm.valid()) {
        if (err) *err = "refusing to write a lightmap whose uv/texel counts disagree with its own "
                        "width/height/vertexCount";
        return false;
    }
    // Stated as its own refusal rather than folded into valid(): a lightmap with no source mesh is
    // internally consistent, it just cannot ever be checked for staleness, which is the one job
    // sourceMesh/sourceHash exist to do.
    if (lm.sourceMesh.empty()) {
        if (err) *err = "refusing to write a lightmap with no sourceMesh: nothing could detect it "
                        "going stale";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::vector<u8> head;
    head.reserve(kFixedHeaderBytes + lm.sourceMesh.size());
    putU32(head, lm.width);
    putU32(head, lm.height);
    putU32(head, lm.vertexCount);
    putU64(head, lm.sourceHash);
    putU32(head, static_cast<u32>(lm.sourceMesh.size()));
    head.insert(head.end(), lm.sourceMesh.begin(), lm.sourceMesh.end());

    std::vector<u8> uv;
    uv.reserve(lm.uv.size() * sizeof(f32));
    for (f32 c : lm.uv) putF32(uv, c);

    std::vector<u8> tex;
    tex.reserve(lm.texels.size() * 4);
    for (const LightmapTexel& t : lm.texels) putU32(tex, encodeRGB9E5(t.r, t.g, t.b));

    Avr1File file;
    file.subtype = kSubtypeLightmap;
    file.add(kChunkHeader, std::move(head), kAvrChunkRequired);
    file.add(kChunkUv,     std::move(uv),   kAvrChunkRequired);
    // GpuUploadable for the same reason GiCache marks its volume so: this payload is destined for a
    // texture, and the container aligns it to 256 bytes rather than the base alignment.
    file.add(kChunkTexels, std::move(tex),  kAvrChunkRequired | kAvrChunkGpuUploadable);
    return saveAvr1(path, file, err);
}

bool readOcLightmap(const std::string& path, OcLightmap& out, std::string* err) {
    Avr1File file;
    if (!loadAvr1(path, file, err)) return false;
    if (file.subtype != kSubtypeLightmap) {
        if (err) *err = path + ": not a lightmap (wrong AVR1 subtype)";
        return false;
    }
    const AvrChunk* head = file.find(kChunkHeader);
    const AvrChunk* uv   = file.find(kChunkUv);
    const AvrChunk* tex  = file.find(kChunkTexels);
    if (!head || head->data.size() < kFixedHeaderBytes) {
        if (err) *err = path + ": the header chunk is missing or short";
        return false;
    }
    if (!uv || !tex) {
        if (err) *err = path + ": the uv or texel chunk is missing";
        return false;
    }

    const u8* p = head->data.data();
    OcLightmap lm;
    lm.width       = getU32(p);     p += 4;
    lm.height      = getU32(p);     p += 4;
    lm.vertexCount = getU32(p);     p += 4;
    lm.sourceHash  = getU64(p);     p += 8;
    const u32 pathLen = getU32(p);  p += 4;
    // The length is read out of the same file it describes, so it is checked against what is
    // actually there rather than trusted -- a truncated header would otherwise construct a string
    // from bytes past the end of the chunk.
    if (head->data.size() != kFixedHeaderBytes + pathLen) {
        if (err) *err = path + ": the header chunk is " + std::to_string(head->data.size()) +
                        " bytes but its own path length describes " +
                        std::to_string(kFixedHeaderBytes + pathLen);
        return false;
    }
    lm.sourceMesh.assign(reinterpret_cast<const char*>(p), pathLen);

    // CHECKED AGAINST THE ARITHMETIC, NOT A STORED SIZE -- the same rule loadGiCache follows, and
    // for the same reason: a stored size can agree with a payload that is simply wrong, but
    // width*height and vertexCount cannot. A payload whose length disagrees with the header's own
    // numbers is a truncated or corrupt file, not a lightmap.
    if (lm.width == 0 || lm.height == 0 || lm.vertexCount == 0) {
        if (err) *err = path + ": the header describes an empty lightmap";
        return false;
    }

    // A SANITY CEILING, AND IT IS LOAD-BEARING ON 64-BIT -- do not weaken it to "defence in depth".
    //
    // THIS COMMENT PREVIOUSLY SAID THE OPPOSITE, on evidence that turned out to be an artefact of
    // the test rather than a fact about the code, so the reasoning is written out here in full.
    //
    // The wrap is real and the arithmetic is exact. `usize(width) * usize(height) * 4` at
    // width = height = 2^31 gives 2^62 * 4, which is 2^64 -- zero in a 64-bit usize. Not "a
    // still-enormous number no real chunk size matches", which is what this comment used to claim:
    // exactly ZERO, which an empty texel chunk matches perfectly at the size check below. Control
    // then reaches `texels.resize(2^62)`, well past any vector's max_size, which throws
    // std::length_error out of a function whose header documents a false return. So without this
    // block a hostile file crashes the reader on a 64-bit build, not merely a 32-bit one.
    //
    // WHY THE OLD CLAIM SURVIVED: the test cited as proof ("passes with this block DISABLED") forged
    // its hostile file by patching bytes inside an already-written file, leaving the AVR1 chunk hash
    // describing the original bytes. parseAvr1 rejected it at the container layer and this function's
    // arithmetic never ran -- so disabling the ceiling could not have changed the outcome either way.
    // The test now builds its file THROUGH the writer, so the hashes are valid, the container admits
    // it, and the assertions name which refusal fired. See OcLightmapTest's hostile-header case.
    //
    // The rest of what it buys still stands: a header claiming 40000x40000 is refused before six
    // gigabytes' worth of arithmetic is done against it, the failure names the absurd number instead
    // of a byte-count mismatch, and a 32-bit build -- where the product itself can wrap small -- is
    // covered by construction. 16384 on an edge is four times the largest atlas anything here makes.
    constexpr u32 kMaxLightmapEdge = 16384;
    constexpr u32 kMaxLightmapVerts = 1u << 28;   // 268M vertices, far past any real mesh
    if (lm.width > kMaxLightmapEdge || lm.height > kMaxLightmapEdge) {
        if (err) *err = path + ": the header claims a " + std::to_string(lm.width) + "x" +
                        std::to_string(lm.height) + " atlas, past the " +
                        std::to_string(kMaxLightmapEdge) + "-texel edge limit";
        return false;
    }
    if (lm.vertexCount > kMaxLightmapVerts) {
        if (err) *err = path + ": the header claims " + std::to_string(lm.vertexCount) +
                        " vertices, past the supported limit";
        return false;
    }
    const usize wantUv  = usize(lm.vertexCount) * 2 * sizeof(f32);
    const usize wantTex = usize(lm.width) * usize(lm.height) * 4;
    if (uv->data.size() != wantUv) {
        if (err) *err = path + ": the uv chunk is " + std::to_string(uv->data.size()) +
                        " bytes but its header describes " + std::to_string(wantUv);
        return false;
    }
    if (tex->data.size() != wantTex) {
        if (err) *err = path + ": the texel chunk is " + std::to_string(tex->data.size()) +
                        " bytes but its header describes " + std::to_string(wantTex);
        return false;
    }

    lm.uv.resize(usize(lm.vertexCount) * 2);
    for (usize i = 0; i < lm.uv.size(); ++i) lm.uv[i] = getF32(uv->data.data() + i * 4);

    lm.texels.resize(usize(lm.width) * usize(lm.height));
    for (usize i = 0; i < lm.texels.size(); ++i)
        lm.texels[i] = decodeRGB9E5(getU32(tex->data.data() + i * 4));

    out = std::move(lm);
    return true;
}

} // namespace aver::fmt
