// Golden test for the material asset path: the .ocmat reader/writer and the mip filter behind
// every texture the material system uploads.
//
// Separate from FormatTest because it is the only test that needs pbr::MaterialDesc, and dragging
// the material DLL into the loader test would make a failure to load Aver.Render.PBR.dll look like
// a broken .ocbeam parser.
//
// No GPU is touched: everything here is CPU-side, so it runs on a machine with no adapter at all.
// The upload step itself (assets::uploadTexture) is the one part not covered, because it IS the RHI
// call -- there is nothing left to test that createTexture does not do. packMaterial IS covered,
// even though it lives in the RHI-linking half of the module: it is a hand-maintained byte layout
// shared with a HLSL cbuffer, which is the single easiest thing here to get silently wrong.
#include "aver/formats/OcMat.hpp"
#include "aver/formats/Texture.hpp"
#include "aver/pbr/MaterialGpu.hpp"   // packMaterial: the block the shader actually reads
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// A file that uses every record the reader claims to implement, plus two it must SKIP rather than
// choke on. The spacing is deliberately irregular and one path contains a space: both are things a
// hand-authored file does and a whitespace-token parser gets wrong.
static const char* kFull = R"(OCMAT 1
# a comment line
NAME Scuffed Floor Tile
SHADER standard
BLEND masked 0.35
CULL none
FLAGS twosided=1 castshadow=0 worlduv=1
PARAM uvTiling 250

PARAM baseColorFactor 0.8 0.7 0.6 0.9
PARAM metallicFactor   0.25
PARAM roughnessFactor  0.75
PARAM emissiveFactor 0.1 0.2 0.3
PARAM normalScale 2.0
PARAM occlusionStrength 0.5
PARAM reflectance 0.02
PARAM f90 0.7
PARAM someFutureThing 1 2 3

TEX baseColor  {path:Textures/floor base.png} uv0 sRGB
TEX metalRough {guid:0x00000000DEADBEEF} uv0 linear
TEX normal     Textures/floor_n.png uv1 normal

FUTURERECORD whatever it likes
GRAPH{
  NODE 0 TexSample baseColor
  NESTED{ NODE 1 Multiply }
  OUT BaseColor 1
}
PARAM metallicFactor 0.9
)";

static void testFullParse() {
    AVER_INFO("=== .ocmat: every implemented record ===");
    pbr::MaterialDesc d;
    fmt::OcMatExtras ex;
    std::string err;
    if (!fmt::parseOcmat(kFull, d, &ex, &err)) {
        AVER_ERROR("   parse failed: {}", err);
        ++g_failures;
        return;
    }

    check(d.name == "Scuffed Floor Tile", "NAME keeps its spaces");
    check(ex.shader == "standard", "SHADER read");
    check(d.alphaMode == pbr::AlphaMode::Mask, "BLEND masked -> AlphaMode::Mask");
    check(near(d.alphaCutoff, 0.35f), "BLEND masked cutoff read");
    check(d.twoSided, "FLAGS twosided=1");
    check(!d.castShadow, "FLAGS castshadow=0");
    check(d.uvMode == pbr::UvMode::WorldAligned, "FLAGS worlduv=1 -> UvMode::WorldAligned");
    check(near(d.uvTiling, 250.0f), "PARAM uvTiling");

    check(near(d.baseColorFactor[0], 0.8f) && near(d.baseColorFactor[3], 0.9f), "baseColorFactor (4)");
    check(near(d.roughnessFactor, 0.75f), "roughnessFactor");
    check(near(d.emissiveFactor[2], 0.3f), "emissiveFactor (3)");
    check(near(d.normalScale, 2.0f), "normalScale");
    check(near(d.occlusionStrength, 0.5f), "occlusionStrength");
    check(near(d.reflectance, 0.02f), "reflectance (Aver PARAM)");
    check(near(d.f90, 0.7f), "f90 (Aver PARAM)");

    const auto& base = d.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)];
    check(base.path == "Textures/floor base.png", "TEX {path:...} survives a space in the path");
    const auto& mr = d.textures[static_cast<u32>(pbr::TextureSlot::MetalRough)];
    check(mr.id == 0xDEADBEEFull, "TEX {guid:0x...} parsed as an id");
    const auto& nrm = d.textures[static_cast<u32>(pbr::TextureSlot::Normal)];
    check(nrm.path == "Textures/floor_n.png", "TEX accepts a bare path (what the editor writes)");
    check(ex.uvSet[static_cast<u32>(pbr::TextureSlot::Normal)] == 1, "TEX uvN recorded");
    check(d.textures[static_cast<u32>(pbr::TextureSlot::Emissive)].empty(), "an unbound slot stays unset");

    check(ex.hasGraph, "GRAPH block detected");
    // The one that actually matters: a nested brace inside GRAPH{} must not end the skip early, or
    // the records after it get parsed as if they were graph nodes -- or worse, the graph's own nodes
    // get parsed as records. The trailing PARAM proves the skip ended in the right place.
    check(near(d.metallicFactor, 0.9f), "GRAPH{} skip counts nested braces and resumes after it");
}

static void testTolerance() {
    AVER_INFO("=== .ocmat: what must fail and what must not ===");
    pbr::MaterialDesc d;
    std::string err;

    check(!fmt::parseOcmat("PARAM metallicFactor 1\n", d, nullptr, &err),
          "a file with no OCMAT line is refused");
    check(!fmt::parseOcmat("OCMAT 2\n", d, nullptr, &err), "an unknown OCMAT version is refused");
    check(fmt::parseOcmat("OCMAT 1\n", d, nullptr, &err), "a header-only file loads as the defaults");
    check(near(d.metallicFactor, 1.0f) && near(d.roughnessFactor, 1.0f),
          "...and those defaults are glTF's, not zero");

    // Malformed input must not take a field with it. A TEX naming a slot that does not exist has to
    // leave every real slot alone rather than landing in slot 0 by falling through a lookup.
    check(fmt::parseOcmat("OCMAT 1\nTEX notASlot {path:x.png}\n", d, nullptr, &err),
          "an unknown TEX slot is skipped, not fatal");
    check(d.textures[0].empty(), "...and does not spill into slot 0");

    check(fmt::parseOcmat("OCMAT 1\nTEX baseColor {path:unterminated\n", d, nullptr, &err),
          "an unterminated {...} is skipped, not fatal");
    check(d.textures[0].empty(), "...and binds nothing");

    // A tiling of zero would make the world-aligned projection read one texel across a whole
    // surface. The reader drops it rather than storing it, so a typo cannot produce a material
    // whose only symptom is a flat colour -- which is indistinguishable from having no texture.
    check(fmt::parseOcmat("OCMAT 1\nFLAGS worlduv=1\nPARAM uvTiling 0\n", d, nullptr, &err),
          "PARAM uvTiling 0 is tolerated");
    check(d.uvTiling > 0.0f, "...but not stored: the default survives");
}

// The GPU block is a cross-module ABI with no compiler behind it, so what packMaterial produces is
// checked here rather than assumed. A wrong flag bit or a missing reciprocal shades plausibly.
static void testPack() {
    AVER_INFO("=== material: the packed GPU block ===");
    check(sizeof(pbr::MaterialConstants) == 64, "MaterialConstants is still 64 bytes");

    pbr::MaterialDesc d;
    pbr::MaterialConstants c = pbr::packMaterial(d);
    check((c.flags & pbr::MaterialFlag_WorldAlignedUv) == 0, "a default material is mesh-UV");
    check(near(c.uvTilesPerCm, 1.0f / d.uvTiling), "tiling is packed as its RECIPROCAL");

    d.uvMode = pbr::UvMode::WorldAligned;
    d.uvTiling = 250.0f;
    c = pbr::packMaterial(d);
    check((c.flags & pbr::MaterialFlag_WorldAlignedUv) != 0, "world-aligned sets its flag bit");
    check(near(c.uvTilesPerCm, 1.0f / 250.0f), "250 cm per tile -> 0.004 tiles per cm");

    // A desc can be built in C++ without going through the ABI's validation, so the pack step has
    // to defend itself: 1/0 is inf, and inf * a world position is the NaN UV that reaches the
    // sampler. Zero tiles per cm reads one texel instead, which is wrong but finite.
    d.uvTiling = 0.0f;
    c = pbr::packMaterial(d);
    check(c.uvTilesPerCm == 0.0f, "a zero tiling packs to zero, never to inf");

    // The texture flags are what tell a branch-free shader which maps are real, since it cannot see
    // an unbound slot -- every one is null-filled with a valid view of the right dimension.
    pbr::MaterialDesc t;
    t.textures[static_cast<u32>(pbr::TextureSlot::Normal)].path = "n.png";
    t.textures[static_cast<u32>(pbr::TextureSlot::Emissive)].id = 7;
    c = pbr::packMaterial(t);
    check((c.flags & pbr::MaterialFlag_NormalMap) != 0, "a path-bound slot sets its flag");
    check((c.flags & pbr::MaterialFlag_EmissiveMap) != 0, "an id-bound slot sets its flag too");
    check((c.flags & pbr::MaterialFlag_BaseColorMap) == 0, "an unbound slot does not");
}

static void testRoundTrip() {
    AVER_INFO("=== .ocmat: round trip ===");
    pbr::MaterialDesc a;
    fmt::OcMatExtras exA;
    std::string err;
    if (!fmt::parseOcmat(kFull, a, &exA, &err)) { ++g_failures; return; }

    const std::string text = fmt::writeOcmat(a, &exA);
    pbr::MaterialDesc b;
    fmt::OcMatExtras exB;
    if (!fmt::parseOcmat(text, b, &exB, &err)) {
        AVER_ERROR("   re-parse of our own output failed: {}", err);
        ++g_failures;
        return;
    }

    check(b.name == a.name, "name round-trips");
    check(b.alphaMode == a.alphaMode && near(b.alphaCutoff, a.alphaCutoff), "alpha rule round-trips");
    check(b.twoSided == a.twoSided && b.castShadow == a.castShadow, "flags round-trip");
    check(b.uvMode == a.uvMode && near(b.uvTiling, a.uvTiling), "UV mapping round-trips");
    bool factors = true;
    for (u32 i = 0; i < 4; ++i) factors = factors && near(b.baseColorFactor[i], a.baseColorFactor[i]);
    for (u32 i = 0; i < 3; ++i) factors = factors && near(b.emissiveFactor[i], a.emissiveFactor[i]);
    factors = factors && near(b.metallicFactor, a.metallicFactor)
                      && near(b.roughnessFactor, a.roughnessFactor)
                      && near(b.normalScale, a.normalScale)
                      && near(b.occlusionStrength, a.occlusionStrength)
                      && near(b.reflectance, a.reflectance)
                      && near(b.f90, a.f90);
    check(factors, "every PARAM round-trips");

    bool textures = true;
    for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
        textures = textures && b.textures[i].path == a.textures[i].path
                            && b.textures[i].id   == a.textures[i].id;
    }
    check(textures, "every TEX binding round-trips, id and path alike");

    // CULL is DERIVED from twoSided on write, so a file that said `CULL none` and `twosided=1` must
    // not come back as a file that says `CULL back` and `twosided=1`. The two can never disagree.
    check(text.find("CULL none") != std::string::npos, "CULL is written from twoSided, not echoed");

    // The colour space a slot is written with is the slot's, not whatever the source file claimed.
    check(text.find("uv0 sRGB") != std::string::npos, "base colour is written sRGB");
    check(text.find("uv1 normal") != std::string::npos, "the normal slot is written 'normal'");
}

static void testBlendModes() {
    AVER_INFO("=== .ocmat: BLEND ===");
    pbr::MaterialDesc d;
    fmt::OcMatExtras ex;
    std::string err;

    check(fmt::parseOcmat("OCMAT 1\nBLEND translucent\n", d, &ex, &err)
          && d.alphaMode == pbr::AlphaMode::Blend && !ex.additive, "translucent -> Blend");
    check(fmt::parseOcmat("OCMAT 1\nBLEND additive\n", d, &ex, &err)
          && d.alphaMode == pbr::AlphaMode::Blend && ex.additive,
          "additive is recorded as a MODE and mapped to the nearest legal alpha rule");
    check(fmt::writeOcmat(d, &ex).find("BLEND additive") != std::string::npos,
          "...and is written back as additive, not as translucent");
    check(fmt::parseOcmat("OCMAT 1\nBLEND opaque\n", d, &ex, &err)
          && d.alphaMode == pbr::AlphaMode::Opaque, "opaque -> Opaque");
}

// The mip filter is the other half of "a texture looks right": every material the system uploads
// goes through it, and getting it wrong is a texture that shifts colour as it recedes.
static void testMipChain() {
    AVER_INFO("=== texture: mip chain ===");

    // A 4x4 checkerboard of black and white, as sRGB colour.
    fmt::TextureData t;
    t.width = 4; t.height = 4; t.srgb = true;
    ImageData l0;
    l0.width = 4; l0.height = 4; l0.channels = 4; l0.srgb = true;
    l0.pixels.resize(4 * 4 * 4);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            const u8 v = ((x + y) & 1) ? 255 : 0;
            u8* p = &l0.pixels[(y * 4 + x) * 4];
            p[0] = p[1] = p[2] = v; p[3] = 255;
        }
    t.levels.push_back(l0);

    fmt::generateMipChain(t, /*normalMap*/ false);
    check(t.levels.size() == 3, "4x4 produces levels 4, 2, 1");
    check(t.levels[1].width == 2 && t.levels[2].width == 1, "each level halves");

    // Half black and half white averaged in LINEAR light is 0.5 linear, which is 188 in sRGB -- not
    // 128. A filter that averaged the encoded bytes would give 127/128 here, and that is exactly the
    // bug that makes a checkerboard darken as it recedes.
    const u8 mid = t.levels[2].pixels[0];
    check(mid >= 186 && mid <= 190, "sRGB mips are filtered in linear light (got " + std::to_string(mid) + ", not ~128)");

    // Build a 2x2 normal map from four explicit texels and filter it to 1x1.
    auto mip1of = [](std::initializer_list<u8> texels) {
        fmt::TextureData n;
        n.width = 2; n.height = 2; n.srgb = false;
        ImageData l;
        l.width = 2; l.height = 2; l.channels = 4; l.srgb = false;
        l.pixels = texels;
        n.levels.push_back(std::move(l));
        fmt::generateMipChain(n, /*normalMap*/ true);
        return n;
    };

    // Two normals leaning hard in opposite directions along X, both with the strongly positive Z
    // every real tangent-space normal has. X must cancel to flat, and the result must come back
    // RENORMALISED -- averaging encoded bytes and stopping there is what makes a normal map lose
    // its slope with distance instead of just its detail.
    const fmt::TextureData lean = mip1of({
        204, 128, 230, 255,   51, 128, 230, 255,    // (+0.6, 0, +0.8) and (-0.6, 0, +0.8)
        204, 128, 230, 255,   51, 128, 230, 255,
    });
    check(lean.levels.size() == 2, "2x2 normal map produces levels 2, 1");
    const u8* avg = lean.levels[1].pixels.data();
    check(avg[0] == 128, "opposed X cancels to the flat value");
    check(avg[2] >= 253, "the average is renormalised, so Z returns to ~1 (got " + std::to_string(avg[2]) + ")");

    // The degenerate case the filter guards: four texels that cancel to the EXACT zero vector.
    // Normalising that is a division by zero, and the NaN it produces reaches the cone trace as the
    // GPU hang this project already has one of to its name.
    const fmt::TextureData cancel = mip1of({
        255, 128, 128, 255,     0, 127, 127, 255,
        255, 128, 128, 255,     0, 127, 127, 255,
    });
    const u8* flat = cancel.levels[1].pixels.data();
    check(flat[0] == 128 && flat[1] == 128 && flat[2] == 255,
          "an exactly-cancelling block falls back to the flat normal, never to a NaN");
}


// The .ocmat grammar has TWO writers now: modules/formats/src/OcMat.cpp, and the C# MaterialBuilder
// that Aver.MaterialCompiler runs at build time. That is a real cost, paid so that baking a material
// does not require a loaded engine -- and this is the test that stops the two drifting.
//
// The text below is EXACTLY what MaterialBuilder.Emit produces, pasted verbatim. If somebody changes
// the C# emitter and not the C++ reader (or the reverse), this goes red rather than a project's
// surfaces quietly losing a parameter.
static void testGeneratedByCsharp() {
    AVER_INFO("=== .ocmat written by the C# material compiler ===");

    // Verbatim output of `avermatc` for SkyForge.Materials.Crate.
    static const char* kGenerated =
        "OCMAT 1\n"
        "# GENERATED by the Aver material compiler from SkyForge.Materials.Crate.\n"
        "# Edits here are overwritten. Change the C# source instead.\n"
        "# Timber crates: warm, against a deliberately cool room.\n"
        "NAME M_Crate\n"
        "SHADER standard\n"
        "BLEND opaque\n"
        "CULL back\n"
        "FLAGS twosided=0 castshadow=1 worlduv=1\n"
        "\n"
        "PARAM baseColorFactor 1 1 1 1\n"
        "PARAM metallicFactor 1\n"
        "PARAM roughnessFactor 1\n"
        "PARAM emissiveFactor 0 0 0\n"
        "PARAM normalScale 1.2\n"
        "PARAM occlusionStrength 1\n"
        "PARAM reflectance 0.035\n"
        "PARAM f90 0.6\n"
        "PARAM uvTiling 60\n"
        "\n"
        "TEX baseColor {path:Textures/T_Crate_BC.png} uv0 sRGB\n"
        "TEX metalRough {path:Textures/T_Crate_MR.png} uv0 linear\n"
        "TEX normal {path:Textures/T_Crate_N.png} uv0 normal\n"
        "TEX occlusion {path:Textures/T_Crate_MR.png} uv0 linear\n";

    pbr::MaterialDesc d;
    fmt::OcMatExtras ex;
    std::string err;
    check(fmt::parseOcmat(kGenerated, d, &ex, &err), "the C++ reader accepts what C# wrote: " + err);

    check(d.name == "M_Crate", "NAME survives");
    check(!d.twoSided, "twosided=0 reads as single-sided");
    check(d.castShadow, "castshadow=1 reads as casting");
    check(d.uvMode == pbr::UvMode::WorldAligned, "worlduv=1 reads as world-aligned");
    check(d.alphaMode == pbr::AlphaMode::Opaque, "BLEND opaque reads as opaque");
    check(near(d.normalScale, 1.2f), "normalScale 1.2");
    check(near(d.reflectance, 0.035f), "reflectance 0.035");
    check(near(d.f90, 0.6f), "f90 0.6");
    check(near(d.uvTiling, 60.0f), "uvTiling 60 cm");
    check(near(d.metallicFactor, 1.0f) && near(d.roughnessFactor, 1.0f), "the factors default through");
    check(near(d.emissiveFactor[0], 0.0f), "no emission");

    const u32 bc = static_cast<u32>(pbr::TextureSlot::BaseColor);
    const u32 nm = static_cast<u32>(pbr::TextureSlot::Normal);
    const u32 oc = static_cast<u32>(pbr::TextureSlot::Occlusion);
    check(d.textures[bc].path == "Textures/T_Crate_BC.png", "the base colour path survives");
    check(d.textures[nm].path == "Textures/T_Crate_N.png", "and the normal map's");
    // metalRough and occlusion deliberately share one texture in this material. A reader that keyed
    // bindings by path rather than by slot would collapse them into one.
    check(d.textures[oc].path == "Textures/T_Crate_MR.png", "occlusion keeps its own binding to the shared texture");

    // And back out through the C++ writer: what the two produce must PARSE THE SAME, even where the
    // bytes differ (the C++ writer adds its own header comment and column alignment).
    const std::string rewritten = fmt::writeOcmat(d, &ex);
    pbr::MaterialDesc d2;
    check(fmt::parseOcmat(rewritten.c_str(), d2, nullptr, &err), "the C++ writer's output parses: " + err);
    check(near(d2.normalScale, d.normalScale) && near(d2.reflectance, d.reflectance) &&
          near(d2.f90, d.f90) && near(d2.uvTiling, d.uvTiling),
          "every parameter survives the round trip through BOTH writers");
    check(d2.uvMode == d.uvMode && d2.twoSided == d.twoSided && d2.castShadow == d.castShadow,
          "and so do the flags");
    check(d2.textures[oc].path == d.textures[oc].path, "and the shared-texture binding");
}

int main() {
    testFullParse();
    testTolerance();
    testPack();
    testRoundTrip();
    testBlendModes();
    testMipChain();
    testGeneratedByCsharp();

    if (g_failures == 0) AVER_INFO("=== all material tests passed ===");
    else AVER_ERROR("=== {} material assertion(s) failed ===", g_failures);
    return g_failures;
}
