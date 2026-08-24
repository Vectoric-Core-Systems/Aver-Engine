// Golden test for the material asset path: the .ocmat reader and writer, the packed GPU block,
// the mip filter, and the C# material-script rewriter. CPU only; no GPU is touched.
#include "aver/formats/OcMat.hpp"
#include "aver/formats/MaterialScript.hpp"
#include "aver/formats/Texture.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

// True when two floats are within eps.
static bool near(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// A .ocmat using every implemented record, plus two the reader must skip.
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

// Parses kFull and checks every record it carries.
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
    check(near(d.metallicFactor, 0.9f), "GRAPH{} skip counts nested braces and resumes after it");
}

// Checks which malformed .ocmat files are refused and which are tolerated.
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

    check(fmt::parseOcmat("OCMAT 1\nTEX notASlot {path:x.png}\n", d, nullptr, &err),
          "an unknown TEX slot is skipped, not fatal");
    check(d.textures[0].empty(), "...and does not spill into slot 0");

    check(fmt::parseOcmat("OCMAT 1\nTEX baseColor {path:unterminated\n", d, nullptr, &err),
          "an unterminated {...} is skipped, not fatal");
    check(d.textures[0].empty(), "...and binds nothing");

    check(fmt::parseOcmat("OCMAT 1\nFLAGS worlduv=1\nPARAM uvTiling 0\n", d, nullptr, &err),
          "PARAM uvTiling 0 is tolerated");
    check(d.uvTiling > 0.0f, "...but not stored: the default survives");

    // The SECOND LAYER: three texture slots plus the slope band they fade in across. This is what
    // lets terrain be rock on cliffs and litter on flats without the landscape owning a shader.
    {
        pbr::MaterialDesc m;
        check(fmt::parseOcmat("OCMAT 1\nNAME M_Ground\n"
                              "TEX baseColor {path:a_diff.jpg} uv0 sRGB\n"
                              "TEX layer1BaseColor {path:rock_diff.jpg} uv0 sRGB\n"
                              "TEX layer1Normal {path:rock_nor.jpg} uv0 normal\n"
                              "PARAM slopeBlend 0.5 0.8 2\n", m, nullptr, &err),
              "a material with a second layer parses");
        check(m.textures[static_cast<u32>(pbr::TextureSlot::Layer1BaseColor)].path == "rock_diff.jpg",
              "layer1BaseColor binds by name");
        check(m.textures[static_cast<u32>(pbr::TextureSlot::Layer1Normal)].path == "rock_nor.jpg",
              "layer1Normal binds by name");
        check(m.slopeBlend, "slopeBlend turns the second layer on");
        check(std::fabs(m.slopeBlendLo - 0.5f) < 1e-6f && std::fabs(m.slopeBlendHi - 0.8f) < 1e-6f,
              "...and keeps its band");
        check(std::fabs(m.layer1UvScale - 2.0f) < 1e-6f, "...and layer 1's own uv scale");

        // Bounds the wrong way round are ORDERED, not trusted: smoothstep(hi, lo, x) returns
        // nonsense rather than failing, so a swapped pair would be a silently wrong surface.
        pbr::MaterialDesc sw;
        check(fmt::parseOcmat("OCMAT 1\nTEX layer1BaseColor {path:r.jpg}\nPARAM slopeBlend 0.9 0.2\n",
                              sw, nullptr, &err), "a reversed slope band parses");
        check(sw.slopeBlendLo < sw.slopeBlendHi, "...and is put back in order");

        // THE FLAG IS NOT SET WITHOUT A LAYER-1 MAP. Blending against nothing would fade every
        // slope to the fallback white texture, which reads as a lighting bug rather than a missing
        // texture -- the worst shape for a defect to have.
        pbr::MaterialDesc bare;
        check(fmt::parseOcmat("OCMAT 1\nPARAM slopeBlend 0.5 0.8\n", bare, nullptr, &err),
              "slopeBlend with no layer-1 texture still parses");
        const pbr::MaterialConstants bc = pbr::packMaterial(bare);
        check((bc.flags & pbr::MaterialFlag_SlopeBlend) == 0,
              "...but packs with the blend OFF, so nothing fades to the fallback");
        const pbr::MaterialConstants gc = pbr::packMaterial(m);
        check((gc.flags & pbr::MaterialFlag_SlopeBlend) != 0, "a real second layer packs the bit on");
        check((gc.flags & pbr::MaterialFlag_Layer1BaseColorMap) != 0, "...and its own slot bit");

        // Round trip: a stated blend survives, an unstated one is not invented.
        const std::string text = fmt::writeOcmat(m, nullptr);
        check(text.find("PARAM slopeBlend") != std::string::npos, "the writer emits the band");
        check(text.find("layer1BaseColor") != std::string::npos, "...and the layer-1 slot");
        // A material that NEVER MENTIONED the blend. Not `bare` above -- that one did state
        // slopeBlend, so writing it back is correct round-tripping even though packMaterial
        // refuses the flag for want of a layer-1 map. The two are different questions and
        // conflating them is what the first version of this check got wrong.
        pbr::MaterialDesc plain;
        check(fmt::parseOcmat("OCMAT 1\nNAME M_Plain\nTEX baseColor {path:a.jpg}\n",
                              plain, nullptr, &err), "an ordinary single-layer material parses");
        check(fmt::writeOcmat(plain, nullptr).find("slopeBlend") == std::string::npos,
              "a material that never mentioned a second layer writes no slopeBlend at all");
    }
}

// Checks the GRAPHREF record: the path to a compiled DOMAIN material .ocgraph. Kept separate from
// testFullParse's kFull fixture because kFull already carries an inline GRAPH{} block, and this
// test needs a "both records in one file" case -- a different combination from what kFull covers.
static void testGraphRef() {
    AVER_INFO("=== .ocmat: GRAPHREF ===");
    std::string err;

    // A path with a space and mixed case -- read as the REST OF THE LINE, trimmed, not a single
    // token, and not normalised in any way.
    pbr::MaterialDesc d;
    fmt::OcMatExtras ex;
    check(fmt::parseOcmat("OCMAT 1\nGRAPHREF Materials/Graphs/Weathered Copper.ocgraph\n",
                          d, &ex, &err), "GRAPHREF parses: " + err);
    check(ex.graphRef == "Materials/Graphs/Weathered Copper.ocgraph",
          "...and keeps its spaces and its case");

    // Round trip: the writer emits it back with the path intact, and re-parsing recovers it.
    const std::string text = fmt::writeOcmat(d, &ex);
    check(text.find("GRAPHREF Materials/Graphs/Weathered Copper.ocgraph") != std::string::npos,
          "the writer emits GRAPHREF with the path intact");
    pbr::MaterialDesc back;
    fmt::OcMatExtras backEx;
    check(fmt::parseOcmat(text, back, &backEx, &err), "the writer's own output re-parses: " + err);
    check(backEx.graphRef == ex.graphRef, "...and GRAPHREF survives the round trip");

    // GRAPHREF and the inline GRAPH{} block are independent records -- naming both in one file is
    // not a parse error, and neither disturbs the other.
    pbr::MaterialDesc both;
    fmt::OcMatExtras bothEx;
    check(fmt::parseOcmat("OCMAT 1\n"
                          "GRAPHREF Materials/Graphs/Weathered Copper.ocgraph\n"
                          "PARAM metallicFactor 0.5\n"
                          "GRAPH{\n"
                          "  NODE 0 TexSample baseColor\n"
                          "}\n"
                          "PARAM roughnessFactor 0.25\n", both, &bothEx, &err),
          "a file naming both GRAPHREF and an inline GRAPH{} block is not a parse error: " + err);
    check(bothEx.graphRef == "Materials/Graphs/Weathered Copper.ocgraph", "...GRAPHREF still parses");
    check(bothEx.hasGraph, "...the inline block is still detected (and so still warns -- see the "
                           "unconditional AVER_WARN keyed off hasGraph)");
    check(near(both.metallicFactor, 0.5f) && near(both.roughnessFactor, 0.25f),
          "...and the GRAPH{} block still skips exactly, resuming right after it");

    // THE IMPORTANT ONE: a material with no GRAPHREF at all -- every material that exists today --
    // must write out BYTE-IDENTICALLY to what this writer produced before GRAPHREF existed. This
    // fixture leaves every field at its MaterialDesc/OcMatExtras default except NAME, so the
    // expected text below is exactly what writeOcmat's pre-existing code paths always produced; if
    // adding GRAPHREF support so much as moved a newline for a material that never mentions it,
    // this is what would catch it.
    pbr::MaterialDesc plain;
    fmt::OcMatExtras plainEx;
    check(fmt::parseOcmat("OCMAT 1\nNAME M_Plain\n", plain, &plainEx, &err),
          "a file with no GRAPHREF parses");
    check(plainEx.graphRef.empty(), "...and graphRef stays empty");

    static const char* kExpectedNoGraphRef =
        "OCMAT 1\n"
        "# Written by the Aver Engine editor.\n"
        "NAME M_Plain\n"
        "SHADER standard\n"
        "BLEND opaque\n"
        "CULL back\n"
        "FLAGS twosided=0 castshadow=1 worlduv=0\n"
        "\n"
        "PARAM baseColorFactor 1 1 1 1\n"
        "PARAM metallicFactor 1\n"
        "PARAM roughnessFactor 1\n"
        "PARAM emissiveFactor 0 0 0\n"
        "PARAM normalScale 1\n"
        "PARAM occlusionStrength 1\n"
        "PARAM reflectance 0.04\n"
        "PARAM f90 1\n"
        "PARAM uvTiling 200\n";
    check(fmt::writeOcmat(plain, &plainEx) == kExpectedNoGraphRef,
          "...and the written text is byte-identical to before GRAPHREF existed");
}

// Checks packMaterial: the 80-byte block the shader reads.
static void testPack() {
    AVER_INFO("=== material: the packed GPU block ===");
    // 80 since the second (slope-blended) layer was added: 64 plus slopeBlendLo/Hi, layer1UvScale
    // and one pad. THIS CHECK EARNED ITS KEEP -- it is what caught the size change, and the reason
    // it matters is that PbrShaders.cpp's `cbuffer AverMaterial` mirrors this struct BY HAND.
    check(sizeof(pbr::MaterialConstants) == 80, "MaterialConstants is still 80 bytes");
    check(sizeof(pbr::MaterialConstants) % 16 == 0, "...and a legal constant-buffer size");

    pbr::MaterialDesc d;
    pbr::MaterialConstants c = pbr::packMaterial(d);
    check((c.flags & pbr::MaterialFlag_WorldAlignedUv) == 0, "a default material is mesh-UV");
    check(near(c.uvTilesPerCm, 1.0f / d.uvTiling), "tiling is packed as its RECIPROCAL");

    d.uvMode = pbr::UvMode::WorldAligned;
    d.uvTiling = 250.0f;
    c = pbr::packMaterial(d);
    check((c.flags & pbr::MaterialFlag_WorldAlignedUv) != 0, "world-aligned sets its flag bit");
    check(near(c.uvTilesPerCm, 1.0f / 250.0f), "250 cm per tile -> 0.004 tiles per cm");

    d.uvTiling = 0.0f;
    c = pbr::packMaterial(d);
    check(c.uvTilesPerCm == 0.0f, "a zero tiling packs to zero, never to inf");

    pbr::MaterialDesc t;
    t.textures[static_cast<u32>(pbr::TextureSlot::Normal)].path = "n.png";
    t.textures[static_cast<u32>(pbr::TextureSlot::Emissive)].id = 7;
    c = pbr::packMaterial(t);
    check((c.flags & pbr::MaterialFlag_NormalMap) != 0, "a path-bound slot sets its flag");
    check((c.flags & pbr::MaterialFlag_EmissiveMap) != 0, "an id-bound slot sets its flag too");
    check((c.flags & pbr::MaterialFlag_BaseColorMap) == 0, "an unbound slot does not");
}

// Writes a parsed material back out and re-parses it.
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

    check(text.find("CULL none") != std::string::npos, "CULL is written from twoSided, not echoed");

    check(text.find("uv0 sRGB") != std::string::npos, "base colour is written sRGB");
    check(text.find("uv1 normal") != std::string::npos, "the normal slot is written 'normal'");
}

// Checks each BLEND mode through parse and write.
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

// Checks generateMipChain: sRGB filtering in linear light, and renormalised normal maps.
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

    const u8 mid = t.levels[2].pixels[0];
    check(mid >= 186 && mid <= 190, "sRGB mips are filtered in linear light (got " + std::to_string(mid) + ", not ~128)");

    // Builds a 2x2 normal map from four texels and filters it to 1x1.
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

    const fmt::TextureData lean = mip1of({
        204, 128, 230, 255,   51, 128, 230, 255,    // (+0.6, 0, +0.8) and (-0.6, 0, +0.8)
        204, 128, 230, 255,   51, 128, 230, 255,
    });
    check(lean.levels.size() == 2, "2x2 normal map produces levels 2, 1");
    const u8* avg = lean.levels[1].pixels.data();
    check(avg[0] == 128, "opposed X cancels to the flat value");
    check(avg[2] >= 253, "the average is renormalised, so Z returns to ~1 (got " + std::to_string(avg[2]) + ")");

    const fmt::TextureData cancel = mip1of({
        255, 128, 128, 255,     0, 127, 127, 255,
        255, 128, 128, 255,     0, 127, 127, 255,
    });
    const u8* flat = cancel.levels[1].pixels.data();
    check(flat[0] == 128 && flat[1] == 128 && flat[2] == 255,
          "an exactly-cancelling block falls back to the flat normal, never to a NaN");
}


// Reads a .ocmat emitted by the C# material compiler, so the two writers cannot drift apart.
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
    check(d.textures[oc].path == "Textures/T_Crate_MR.png", "occlusion keeps its own binding to the shared texture");

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


// Checks the rewriter that writes an edited material back into its C# source.
static void testScriptRewrite() {
    AVER_INFO("=== rewriting the C# source ===");

    static const char* kSource =
        "using Aver.Materials;\n"
        "\n"
        "namespace SkyForge.Materials;\n"
        "\n"
        "/// <summary>Timber crates.</summary>\n"
        "[AverMaterial(\"M_Crate\")]\n"
        "public sealed class Crate : Material\n"
        "{\n"
        "    public static void Configure(MaterialBuilder b) => b\n"
        "        .Comment(\"Timber crates: warm, against a deliberately cool room.\")\n"
        "        .WorldUv(true).Tiling(60f)\n"
        "        .NormalScale(1.2f).Reflectance(0.035f).F90(0.6f)\n"
        "        .Texture(Slot.BaseColor,  \"Textures/T_Crate_BC.png\");\n"
        "}\n"
        "\n"
        "[AverMaterial(\"M_Other\")]\n"
        "public sealed class Other : Material\n"
        "{\n"
        "    public static void Configure(MaterialBuilder b) => b.Roughness(0.25f);\n"
        "}\n";

    pbr::MaterialDesc d;
    d.name = "M_Crate";
    d.uvMode = pbr::UvMode::WorldAligned;
    d.uvTiling = 75.0f;
    d.normalScale = 1.2f;
    d.reflectance = 0.035f;
    d.f90 = 0.6f;
    d.roughnessFactor = 0.5f;
    d.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path = "Textures/T_Crate_BC.png";

    std::string out, err;
    check(fmt::rewriteMaterialScript(kSource, "M_Crate", d, nullptr, out, &err),
          "it rewrites: " + err);

    check(out.find(".Tiling(75f)") != std::string::npos, "the edited value is written");
    check(out.find("60f") == std::string::npos, "and the old one is gone");
    check(out.find(".Roughness(0.5f)") != std::string::npos, "a newly-set value appears");

    check(out.find("Timber crates: warm, against a deliberately cool room.") != std::string::npos,
          "the .Comment prose is preserved");

    check(out.find("using Aver.Materials;") != std::string::npos, "the usings survive");
    check(out.find("namespace SkyForge.Materials;") != std::string::npos, "the namespace survives");
    check(out.find("/// <summary>Timber crates.</summary>") != std::string::npos, "the doc comment survives");
    check(out.find("public sealed class Crate : Material") != std::string::npos, "the class declaration survives");

    check(out.find("public static void Configure(MaterialBuilder b) => b.Roughness(0.25f);") != std::string::npos,
          "a second material in the same file is left exactly as it was");

    check(out.find(".Metallic(") == std::string::npos, "an unchanged default is not emitted");
    check(out.find(".OcclusionStrength(") == std::string::npos, "nor another");

    std::string twice;
    check(fmt::rewriteMaterialScript(out, "M_Crate", d, nullptr, twice, &err), "it rewrites its own output");
    check(twice == out, "and rewriting twice changes nothing");

    AVER_INFO("=== what the rewriter refuses ===");
    {
        std::string o, e;
        check(!fmt::rewriteMaterialScript(kSource, "M_Missing", d, nullptr, o, &e),
              "a name that is not in the file is refused");
        check(o.empty(), "and nothing is written on refusal");

        static const char* kRich =
            "[AverMaterial(\"M_Rich\")]\n"
            "public sealed class Rich : Material\n"
            "{\n"
            "    public static void Configure(MaterialBuilder b)\n"
            "    {\n"
            "        float k = 0.5f;\n"
            "        b.Roughness(k);\n"
            "    }\n"
            "}\n";
        check(!fmt::rewriteMaterialScript(kRich, "M_Rich", d, nullptr, o, &e),
              "a Configure with more than one statement is refused, not truncated");

        static const char* kBrace =
            "[AverMaterial(\"M_Odd\")]\n"
            "public sealed class Odd : Material\n"
            "{\n"
            "    public static void Configure(MaterialBuilder b) => b\n"
            "        .Texture(Slot.BaseColor, \"Textures/a;b}c.png\");\n"
            "}\n";
        pbr::MaterialDesc od;
        od.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path = "Textures/a;b}c.png";
        check(fmt::rewriteMaterialScript(kBrace, "M_Odd", od, nullptr, o, &e),
              "a path containing a brace and a semicolon does not end the body early: " + e);
        check(o.find("a;b}c.png") != std::string::npos, "and it survives the round trip");
    }

    AVER_INFO("=== a brand new material source ===");
    {
        pbr::MaterialDesc nd;
        nd.roughnessFactor = 0.3f;
        const std::string src = fmt::newMaterialScript("M_Glass", "MyGame.Materials", nd, nullptr);
        check(src.find("[AverMaterial(\"M_Glass\")]") != std::string::npos, "it carries the bound name");
        check(src.find("class Glass : Material") != std::string::npos, "the class name drops the M_ prefix");
        check(src.find(".Roughness(0.3f)") != std::string::npos, "and the value is in it");

        std::string o, e;
        nd.roughnessFactor = 0.9f;
        check(fmt::rewriteMaterialScript(src, "M_Glass", nd, nullptr, o, &e),
              "a newly-created source is rewritable: " + e);
        check(o.find(".Roughness(0.9f)") != std::string::npos, "with the new value");
    }
}

// Runs every material test. Returns the failure count.
int main() {
    testFullParse();
    testTolerance();
    testGraphRef();
    testPack();
    testRoundTrip();
    testBlendModes();
    testMipChain();
    testGeneratedByCsharp();
    testScriptRewrite();

    if (g_failures == 0) AVER_INFO("=== all material tests passed ===");
    else AVER_ERROR("=== {} material assertion(s) failed ===", g_failures);
    return g_failures;
}
