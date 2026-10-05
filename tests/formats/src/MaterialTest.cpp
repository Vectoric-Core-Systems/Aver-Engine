// Golden test for the material asset path: the .ocmat reader and writer, the packed GPU block,
// the mip filter, and the C# material-script rewriter. CPU only; no GPU is touched.
#include <cstddef>
#include "aver/formats/OcMat.hpp"
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/MaterialCook.hpp"
#include "aver/formats/MaterialScript.hpp"
#include "aver/formats/Texture.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

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

// ---- the three-way MaterialConstants mirror, read rather than described -------------------------
//
// THE OFFSET BLOCK IN main() CALLED ITSELF A THREE-WAY CHECK AND OPENED NEITHER SHADER. It asserts
// C++ offsets, which pins one of the three copies to itself -- a tautology. The failure it says it
// exists to catch is a field REORDERED in one of the HLSL mirrors, where the total size never moves,
// the build succeeds, every suite passes, and materials shade with a neighbour's bytes. That could
// not have been caught, because the files were never read. voxi.hlsl's own RtMaterial comment says
// the order is "checked only by tests/formats/src/MaterialTest.cpp", which made the gap worse: the
// shader author was told a guard existed.
//
// WHAT IS CHECKED: the field ORDER in each shader block, against the order of the C++ struct. Not
// packing arithmetic -- HLSL's own rules differ between a cbuffer and a StructuredBuffer, and
// reimplementing them here would be a fourth thing to get wrong. Order plus the existing size and
// offset assertions is what actually catches the dangerous case, because every reorder of two
// same-typed floats moves a name past another name.
//
// The table IS the assertion. It is a fourth copy of the field list, deliberately: a test that
// derived the expected order from one of the files it checks would agree with itself.
struct MirrorField {
    const char* cpp;       // for the failure message
    const char* prelude;   // name in material_prelude.hlsl's `cbuffer AverMaterial`, or null
    const char* rt;        // name in voxi.hlsl's `struct RtMaterial`, or null
};
static const MirrorField kMirror[] = {
    {"baseColorFactor",     "gBaseColorFactor",    "baseColorFactor"},
    {"emissiveFactor",      "gEmissiveFactor",     "emissiveFactor"},
    {"metallicFactor",      "gMetallicFactor",     "metallicFactor"},
    {"roughnessFactor",     "gRoughnessFactor",    "roughnessFactor"},
    {"normalScale",         "gNormalScale",        "normalScale"},
    {"occlusionStrength",   "gOcclusionStrength",  "occlusionStrength"},
    {"alphaCutoff",         "gAlphaCutoff",        "alphaCutoff"},
    {"flags",               "gMaterialFlags",      "flags"},
    {"reflectance",         "gMatReflectance",     "reflectance"},
    {"f90",                 "gMatF90",             "f90"},
    {"uvTilesPerCm",        "gUvTilesPerCm",       "uvTilesPerCm"},
    {"slopeBlendLo",        "gSlopeBlendLo",       "slopeBlendLo"},
    {"slopeBlendHi",        "gSlopeBlendHi",       "slopeBlendHi"},
    {"layer1UvScale",       "gL1UvScale",          "layer1UvScale"},
    {"graphId",             "gMaterialGraphId",    "graphId"},
    {"ior",                 "gIor",                "ior"},
    {"transmission",        "gTransmission",       "transmission"},
    {"subsurfaceWeight",    "gSubsurfaceWeight",   "subsurfaceWeight"},
    {"subsurfaceRadius",    "gSubsurfaceRadius",   "subsurfaceRadius"},
    {"coatWeight",          "gCoatWeight",         "coatWeight"},
    {"coatRoughness",       "gCoatRoughness",      "coatRoughness"},
    {"coatF0",              "gCoatF0",             "coatF0"},
    {"_coatPad",            "_gCoatPad",           "_coatPad"},
    // The cbuffer splits the eight indices into two uint4 rows; the structured buffer declares one
    // uint[8]. Two rows here so the SECOND half's position is pinned too -- gTexIndex1 drifting
    // above gTexIndex0 would swap four texture slots with four others.
    {"texIndex",            "gTexIndex0",          "texIndex"},
    {"texIndex[4..7]",      "gTexIndex1",          nullptr},
    {"attenuationColor",    "gAttenuationColor",   "attenuationColor"},
    {"attenuationDistance", "gAttenuationDistance", "attenuationDistance"},
    {"lightIntensity",      "gLightIntensity",     "lightIntensity"},
    {"subsurfaceColor",     "gSubsurfaceColor",    "subsurfaceColor"},
};

// Reads a repo-relative file whole. Empty on failure, which the caller MUST treat as a failure:
// every `find` below would otherwise return npos and the ordering check would pass vacuously.
static std::string readRepoFile(const char* rel) {
    const std::string path = std::string(AVER_REPO_ROOT) + "/" + rel;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Replaces every comment with spaces, keeping every other byte at its own offset so positions in
// the result are still positions in the original.
//
// NOT COSMETIC. Both of these blocks carry long prose comments that NAME the fields around them --
// material_prelude.hlsl has a paragraph about `gIor` sitting eighteen lines above `float gIor;` --
// so a search that did not strip comments would find the mention rather than the declaration and
// pin the wrong position. That is the shape of a check that looks strict and tests nothing.
static std::string stripComments(std::string s) {
    for (usize i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') s[i++] = ' ';
        } else if (s[i] == '/' && s[i + 1] == '*') {
            const usize end = s.find("*/", i + 2);
            const usize stop = (end == std::string::npos) ? s.size() : end + 2;
            while (i < stop) s[i++] = ' ';
        }
    }
    return s;
}

// The text between `opener` and the first `};` after it -- one shader declaration block, comments
// blanked out.
// STRIPPED BEFORE THE BLOCK IS CUT OUT, not after: a `};` inside one of these files' prose would
// otherwise end the block early and the fields past it would read as missing.
static std::string blockAfter(const std::string& text, const char* opener) {
    const std::string clean = stripComments(text);
    const usize a = clean.find(opener);
    if (a == std::string::npos) return {};
    const usize b = clean.find("};", a);
    if (b == std::string::npos) return {};
    return clean.substr(a, b - a);
}

// Finds `name` as a whole declared identifier: preceded by whitespace and followed by one of
// ` ;[:` -- so `gIor` does not match inside `gIorSomething`, and a mention in a COMMENT that
// happens to sit above its own declaration cannot stand in for the declaration itself.
static usize declPos(const std::string& block, const char* name) {
    const std::string n = name;
    usize from = 0;
    for (;;) {
        const usize p = block.find(n, from);
        if (p == std::string::npos) return std::string::npos;
        const bool leftOk  = p > 0 && (block[p - 1] == ' ' || block[p - 1] == '\t' || block[p - 1] == '\n');
        const usize after  = p + n.size();
        const bool rightOk = after < block.size() &&
                             (block[after] == ';' || block[after] == '[' || block[after] == ' ' ||
                              block[after] == '\t' || block[after] == ':');
        if (leftOk && rightOk) return p;
        from = p + 1;
    }
}

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

    // AN UNRECOGNISED PARAM LOADS AND WARNS, exactly as the unknown TEX slot above does. Both
    // halves matter. The parse must SUCCEED, so an older engine can open a material written by a
    // newer one and keep the keys it does understand. And the warning must exist: until it did,
    // this chain simply ended, so a typo like `coatWieght` was dropped in total silence and the
    // material rendered with the default -- which looks exactly like a shading bug and is not one.
    //
    // The message goes to the log rather than to `err`, so what is asserted here is the contract:
    // it loads, it does not fail, and it leaves the value alone rather than half-applying it.
    check(fmt::parseOcmat("OCMAT 1\nPARAM coatWieght 0.9\n", d, nullptr, &err),
          "a file with an unrecognised PARAM still LOADS -- forward compatibility");
    check(d.coatWeight == 0.0f,
          "...and the misspelled key stored nothing, so the default stands");

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
        "PARAM ior 1.5\n"
        "PARAM transmission 0\n"
        "PARAM uvTiling 200\n";
    check(fmt::writeOcmat(plain, &plainEx) == kExpectedNoGraphRef,
          "...and the written text is byte-identical to before GRAPHREF existed"
          " (updated for PARAM ior/transmission, which now always appear too)");
}

// PARAM ior and PARAM transmission: index of refraction and dielectric transmission weight, added
// so a .ocmat can author glass. Kept out of kFull/testFullParse deliberately -- kFull is reused by
// testRoundTrip and by the byte-exact fixture in testGraphRef, and this is exactly the kind of change
// that byte-exact fixture exists to catch, so it gets its own isolated text instead.
//
// THE DISCRIMINATING PART: before this PARAM branch existed, "ior"/"transmission" were unrecognised
// keys, and an unrecognised PARAM name is SILENTLY DROPPED (see the PARAM branch's final `else` --
// there isn't one, which is the bug). Parsing never failed and never warned; d.ior/d.transmission
// simply stayed at the MaterialDesc defaults (1.5f / 0.0f). So asserting the SPECIFIC values below,
// chosen to differ from both defaults, is what makes this test fail against the pre-fix parser: a
// pre-fix build would read back 1.5 and 0.0 no matter what the file said, not 1.8 and 0.85.
static void testDielectric() {
    AVER_INFO("=== .ocmat: PARAM ior / transmission ===");
    std::string err;

    pbr::MaterialDesc d;
    check(fmt::parseOcmat("OCMAT 1\nNAME M_Glass\nPARAM ior 1.8\nPARAM transmission 0.85\n",
                          d, nullptr, &err), "a material with ior/transmission parses: " + err);
    check(near(d.ior, 1.8f), "ior is read, not left at the 1.5 default");
    check(near(d.transmission, 0.85f), "transmission is read, not left at the 0.0 default");

    // Round trip: unconditional emission (matching reflectance/f90's convention, not slopeBlend's
    // conditional one -- see writeOcmat) plus a re-parse that recovers the same values.
    const std::string text = fmt::writeOcmat(d, nullptr);
    check(text.find("PARAM ior 1.8") != std::string::npos, "the writer emits PARAM ior");
    check(text.find("PARAM transmission 0.85") != std::string::npos,
          "the writer emits PARAM transmission");
    pbr::MaterialDesc back;
    check(fmt::parseOcmat(text, back, nullptr, &err), "the writer's own output re-parses: " + err);
    check(near(back.ior, d.ior) && near(back.transmission, d.transmission),
          "both survive the round trip");

    // A header-only file loads the glTF-adjacent defaults this format already promises for every
    // other factor (see testTolerance's "header-only file loads as the defaults").
    pbr::MaterialDesc def;
    check(fmt::parseOcmat("OCMAT 1\n", def, nullptr, &err), "a header-only file loads");
    check(near(def.ior, 1.5f), "...ior defaults to 1.5 (MaterialDesc's own default, not this format's)");
    check(near(def.transmission, 0.0f), "...transmission defaults to fully opaque");

    // REPAIRED, NOT PASSED THROUGH. 1.0 is vacuum, so an authored ior below it is not a value this
    // format should ever hand to a renderer; it is clamped up to 1.0 rather than kept as typed or
    // silently reverted to the 1.5 default -- either of which this assertion would catch (0.4 != 1.0,
    // and 1.5 != 1.0).
    pbr::MaterialDesc lo;
    check(fmt::parseOcmat("OCMAT 1\nPARAM ior 0.4\n", lo, nullptr, &err),
          "a sub-vacuum ior parses, rather than failing the file");
    check(near(lo.ior, 1.0f), "...but is clamped up to 1.0, not kept at 0.4 or reverted to 1.5");

    // Same idiom for transmission's upper bound: [0,1] by definition, so 1.4 is clamped to 1.0 rather
    // than stored raw (which this assertion would also catch: 1.4 != 1.0).
    pbr::MaterialDesc hi;
    check(fmt::parseOcmat("OCMAT 1\nPARAM transmission 1.4\n", hi, nullptr, &err),
          "an over-1 transmission parses, rather than failing the file");
    check(near(hi.transmission, 1.0f), "...but is clamped down to 1.0, not kept at 1.4");

    // And the lower bound: a negative transmission is clamped to 0, exercised alongside a nonzero
    // ior on the same line so a build that dropped the PARAM entirely (leaving both at their
    // defaults) cannot pass this by coincidence -- ior 1.9 is not 1.5, whatever transmission does.
    pbr::MaterialDesc neg;
    check(fmt::parseOcmat("OCMAT 1\nPARAM ior 1.9\nPARAM transmission -0.3\n", neg, nullptr, &err),
          "a negative transmission parses, rather than failing the file");
    check(near(neg.ior, 1.9f), "...ior alongside it still reads correctly");
    check(near(neg.transmission, 0.0f), "...but transmission is clamped up to 0, not kept negative");
}

// PARAM subsurfaceWeight and PARAM subsurfaceRadius: the wrap-diffuse-plus-back-scatter
// approximation (see Material.hpp's long comment on what it is and is not). Kept out of
// kFull/testFullParse for exactly the reason testDielectric gives for ior/transmission -- kFull
// feeds testRoundTrip and the byte-exact fixture in testGraphRef, and this is precisely the kind of
// addition that fixture exists to catch -- so it gets its own isolated text here too.
//
// THE DISCRIMINATING PART, same shape as testDielectric: before this PARAM branch existed,
// "subsurfaceWeight"/"subsurfaceRadius" were unrecognised keys and an unrecognised PARAM name is
// silently dropped, so d.subsurfaceWeight/d.subsurfaceRadius would stay at the MaterialDesc default
// of 0.0f no matter what the file said. Asserting the specific nonzero values below is what makes
// this test fail against a pre-fix parser, which would read back 0/0 regardless.
static void testSubsurface() {
    AVER_INFO("=== .ocmat: PARAM subsurfaceWeight / subsurfaceRadius ===");
    std::string err;

    pbr::MaterialDesc d;
    check(fmt::parseOcmat("OCMAT 1\nNAME M_Skin\nPARAM subsurfaceWeight 0.6\nPARAM subsurfaceRadius 0.35\n"
                          "PARAM subsurfaceColor 0.8 0.3 0.25\n",
                          d, nullptr, &err), "a material with subsurfaceWeight/subsurfaceRadius parses: " + err);
    check(near(d.subsurfaceWeight, 0.6f), "subsurfaceWeight is read, not left at the 0.0 default");
    check(near(d.subsurfaceRadius, 0.35f), "subsurfaceRadius is read, not left at the 0.0 default");
    check(near(d.subsurfaceColor[0], 0.8f) && near(d.subsurfaceColor[1], 0.3f) && near(d.subsurfaceColor[2], 0.25f),
          "subsurfaceColor is read, not left at the white default");

    // Round trip. UNLIKE ior/transmission's unconditional emission just above in this file's other
    // test, this pair is written CONDITIONALLY -- the same convention slopeBlend uses, and for the
    // same reason (see writeOcmat's comment on the subsurfaceWeight line): the weight is the
    // feature's own off switch, not merely a number, so a material that stated it must get the line
    // back and a material that never mentioned it must not gain one. A nonzero weight round-trips
    // here; the "never mentioned it" case is the assertion below this one.
    const std::string text = fmt::writeOcmat(d, nullptr);
    check(text.find("PARAM subsurfaceWeight 0.6") != std::string::npos,
          "the writer emits PARAM subsurfaceWeight when the material asked for it");
    check(text.find("PARAM subsurfaceRadius 0.35") != std::string::npos,
          "the writer emits PARAM subsurfaceRadius alongside it");
    check(text.find("PARAM subsurfaceColor 0.8 0.3 0.25") != std::string::npos,
          "...and PARAM subsurfaceColor alongside that");
    pbr::MaterialDesc back;
    check(fmt::parseOcmat(text, back, nullptr, &err), "the writer's own output re-parses: " + err);
    check(near(back.subsurfaceWeight, d.subsurfaceWeight) && near(back.subsurfaceRadius, d.subsurfaceRadius),
          "both survive the round trip");
    check(near(back.subsurfaceColor[0], d.subsurfaceColor[0]) &&
          near(back.subsurfaceColor[1], d.subsurfaceColor[1]) &&
          near(back.subsurfaceColor[2], d.subsurfaceColor[2]),
          "subsurfaceColor survives the round trip too");

    // A header-only file loads the feature in its OFF state -- 0.0, exactly like every material
    // authored before this pair existed (Material.hpp: "every material authored before this existed
    // shades bit-identically").
    pbr::MaterialDesc def;
    check(fmt::parseOcmat("OCMAT 1\n", def, nullptr, &err), "a header-only file loads");
    check(near(def.subsurfaceWeight, 0.0f), "...subsurfaceWeight defaults to 0, the feature's own off switch");
    check(near(def.subsurfaceRadius, 0.0f), "...subsurfaceRadius defaults to 0 too");
    check(near(def.subsurfaceColor[0], 1.0f) && near(def.subsurfaceColor[1], 1.0f) &&
          near(def.subsurfaceColor[2], 1.0f),
          "...subsurfaceColor defaults to white, scattering in the surface's own colour");

    // THE WRITTEN-ABSENCE CASE, which testDielectric has no equivalent of because ior/transmission
    // are unconditional: a material that never asked for the wrap term must not gain a
    // "PARAM subsurfaceWeight 0" line it never authored. That line would carry no information beyond
    // "not in use" and would turn every pre-existing .ocmat fixture into a diff the moment this field
    // was added -- see writeOcmat's comment on why this is opt-in like slopeBlend, not like
    // reflectance/f90/ior/transmission.
    const std::string offText = fmt::writeOcmat(def, nullptr);
    check(offText.find("subsurfaceWeight") == std::string::npos,
          "a material with subsurfaceWeight 0 writes NO subsurface line at all");
    check(offText.find("subsurfaceRadius") == std::string::npos,
          "...neither half of the pair appears, not even alone");
    // THE DISCRIMINATING PART FOR COLOUR SPECIFICALLY: def.subsurfaceColor is (1,1,1), not zero, so a
    // writer that (wrongly) gated this line on "is the colour non-default" rather than on the weight
    // alone would still pass a test that only checked a zeroed colour. It must stay silent regardless.
    check(offText.find("subsurfaceColor") == std::string::npos,
          "...nor subsurfaceColor, even though white is not itself the zero value");

    // Clamped from BOTH directions, same idiom as testDielectric's ior/transmission bounds: both
    // fields are [0,1] by definition (see Material.hpp), so an authored value outside that range is
    // clamped rather than kept raw -- which the assertions below would catch (1.4 != 1.0, 2.0 != 1.0).
    // subsurfaceColor is clamped the same way, per channel, and this line exercises both of its
    // directions at once: channel 0 and 2 are over 1, channel 1 is negative.
    // The upper bound needs no extra discriminator: 1.0 already differs from the 0.0 default, so a
    // build that dropped the PARAM entirely could not pass this by coincidence.
    pbr::MaterialDesc hi;
    check(fmt::parseOcmat("OCMAT 1\nPARAM subsurfaceWeight 1.4\nPARAM subsurfaceRadius 2.0\n"
                          "PARAM subsurfaceColor 1.5 -0.3 2.0\n",
                          hi, nullptr, &err),
          "an over-1 subsurfaceWeight/subsurfaceRadius parses, rather than failing the file");
    check(near(hi.subsurfaceWeight, 1.0f), "...subsurfaceWeight is clamped down to 1.0, not kept at 1.4");
    check(near(hi.subsurfaceRadius, 1.0f), "...subsurfaceRadius is clamped down to 1.0, not kept at 2.0");
    check(near(hi.subsurfaceColor[0], 1.0f) && near(hi.subsurfaceColor[1], 0.0f) &&
          near(hi.subsurfaceColor[2], 1.0f),
          "...subsurfaceColor is clamped per channel into [0,1], both directions at once");

    // ---- the coat: written only when on, read back exactly, clamped at both ends ----
    //
    // THE ROUND TRIP IS THE POINT. An unrecognised PARAM key is SILENTLY DROPPED by parseOcmat --
    // no error, no warning, the key simply vanishes -- so a writer that emits a name the parser does
    // not handle produces a material quietly missing its coat with nothing anywhere to grep for.
    // Writing then parsing is the only check that catches that, and there are TWO writers to keep
    // honest: this one and scripting/csharp/Aver.Materials/MaterialBuilder.cs, whose own remarks say
    // "the two must agree" and which nothing else verifies.
    check(offText.find("coatWeight") == std::string::npos,
          "a material with coatWeight 0 writes NO coat line at all");

    pbr::MaterialDesc coated;
    coated.coatWeight    = 0.8f;
    coated.coatRoughness = 0.15f;
    coated.coatF0        = 0.05f;
    const std::string coatText = fmt::writeOcmat(coated, nullptr);
    check(coatText.find("PARAM coatWeight") != std::string::npos, "an authored coat writes coatWeight");
    check(coatText.find("PARAM coatRoughness") != std::string::npos, "...and coatRoughness");
    check(coatText.find("PARAM coatF0") != std::string::npos, "...and coatF0");

    pbr::MaterialDesc coatBack;
    check(fmt::parseOcmat(coatText, coatBack, nullptr, &err), "the coat it wrote parses back");
    check(near(coatBack.coatWeight, 0.8f),    "...coatWeight survives the round trip");
    check(near(coatBack.coatRoughness, 0.15f),"...coatRoughness survives the round trip");
    check(near(coatBack.coatF0, 0.05f),       "...coatF0 survives the round trip");

    pbr::MaterialDesc coatHi;
    check(fmt::parseOcmat("OCMAT 1\nPARAM coatWeight 1.6\nPARAM coatRoughness 3.0\n",
                          coatHi, nullptr, &err),
          "an over-1 coatWeight/coatRoughness parses, rather than failing the file");
    check(near(coatHi.coatWeight, 1.0f),    "...coatWeight is clamped down to 1.0, not kept at 1.6");
    check(near(coatHi.coatRoughness, 1.0f), "...coatRoughness is clamped down to 1.0, not kept at 3.0");

    // The lower bound is the harder case: 0 is BOTH the correctly-clamped result and the MaterialDesc
    // default, so a build that silently failed to recognise these two PARAM names would read back
    // exactly the same 0/0 that correct clamping produces. PARAM ior rides along on the same file,
    // unrelated to subsurface entirely, purely as the discriminator testDielectric's own negative-
    // transmission case uses for the identical reason: ior 1.9 is not its 1.5 default, so it proves
    // this file was parsed line by line rather than abandoned, whatever the subsurface fields do.
    pbr::MaterialDesc lo;
    check(fmt::parseOcmat("OCMAT 1\nPARAM ior 1.9\nPARAM subsurfaceWeight -0.3\nPARAM subsurfaceRadius -1.0\n"
                          "PARAM subsurfaceColor -0.5 0.4 -2.0\n",
                          lo, nullptr, &err),
          "a negative subsurfaceWeight/subsurfaceRadius parses, rather than failing the file");
    check(near(lo.ior, 1.9f), "...ior alongside it still reads correctly, so the file was not simply dropped");
    check(near(lo.subsurfaceWeight, 0.0f), "...but subsurfaceWeight is clamped up to 0, not kept negative");
    check(near(lo.subsurfaceRadius, 0.0f), "...and subsurfaceRadius is clamped up to 0 too");
    check(near(lo.subsurfaceColor[0], 0.0f) && near(lo.subsurfaceColor[1], 0.4f) &&
          near(lo.subsurfaceColor[2], 0.0f),
          "...and subsurfaceColor's negative channels are clamped up to 0, the positive one kept as authored");
}

// PARAM lightIntensity: turns a material into a ray-driven local light, a multiplier on the light
// its own emissive glow and size already cast at 1 metre, in the sun's own units. Kept out of
// kFull/testFullParse for the same reason testDielectric and
// testSubsurface give for their own PARAMs -- kFull feeds testRoundTrip and the byte-exact fixture
// in testGraphRef, and this is exactly the kind of addition that fixture exists to catch.
//
// THE DISCRIMINATING PART, same shape as testDielectric/testSubsurface: before this PARAM branch
// existed, "lightIntensity" was an unrecognised key and is silently dropped, so d.lightIntensity
// would stay at the MaterialDesc default of 0.0f no matter what the file said. Asserting a specific
// nonzero value is what makes this test fail against a pre-fix parser, which would read back 0
// regardless.
static void testLightIntensity() {
    AVER_INFO("=== .ocmat: PARAM lightIntensity ===");
    std::string err;

    pbr::MaterialDesc d;
    check(fmt::parseOcmat("OCMAT 1\nNAME M_Lamp\nPARAM lightIntensity 8\n", d, nullptr, &err),
          "a material with lightIntensity parses: " + err);
    check(near(d.lightIntensity, 8.0f), "lightIntensity is read, not left at the 0.0 default");

    // Round trip. OMITTED WHEN OFF, the same convention subsurfaceWeight/coatWeight use (see
    // writeOcmat's comment on the lightIntensity line): 0 is the feature's own off switch, not
    // merely a number, so a material that stated it must get the line back and one that never
    // mentioned it must not gain one.
    const std::string text = fmt::writeOcmat(d, nullptr);
    check(text.find("PARAM lightIntensity 8") != std::string::npos,
          "the writer emits PARAM lightIntensity when the material asked for it");
    pbr::MaterialDesc back;
    check(fmt::parseOcmat(text, back, nullptr, &err), "the writer's own output re-parses: " + err);
    check(near(back.lightIntensity, d.lightIntensity), "lightIntensity survives the round trip");

    // A header-only file loads the feature in its OFF state, exactly like every material authored
    // before this field existed.
    pbr::MaterialDesc def;
    check(fmt::parseOcmat("OCMAT 1\n", def, nullptr, &err), "a header-only file loads");
    check(near(def.lightIntensity, 0.0f), "...lightIntensity defaults to 0, the feature's own off switch");

    // THE WRITTEN-ABSENCE CASE: a material that never asked to be a light must not gain a
    // "PARAM lightIntensity 0" line it never authored.
    const std::string offText = fmt::writeOcmat(def, nullptr);
    check(offText.find("lightIntensity") == std::string::npos,
          "a material with lightIntensity 0 writes NO light line at all");

    // FLOORED, NOT CEILINGED: unlike subsurfaceWeight/transmission, there is no upper bound -- a
    // lamp can legitimately want to be far brighter than the sun -- so only the negative case is
    // clamped, and it is clamped up to 0 rather than kept negative or reverted to some other default.
    pbr::MaterialDesc neg;
    check(fmt::parseOcmat("OCMAT 1\nPARAM ior 1.9\nPARAM lightIntensity -3.0\n", neg, nullptr, &err),
          "a negative lightIntensity parses, rather than failing the file");
    check(near(neg.ior, 1.9f), "...ior alongside it still reads correctly, so the file was not simply dropped");
    check(near(neg.lightIntensity, 0.0f), "...but lightIntensity is clamped up to 0, not kept negative");

    // A large value is kept AS AUTHORED -- the discriminating case for "no ceiling", since a clamped
    // implementation would quietly cap this the way subsurfaceWeight's [0,1] does.
    pbr::MaterialDesc bright;
    check(fmt::parseOcmat("OCMAT 1\nPARAM lightIntensity 500\n", bright, nullptr, &err),
          "a very bright lightIntensity parses");
    check(near(bright.lightIntensity, 500.0f), "...and is kept exactly, not capped to some ceiling");
}

// Checks packMaterial: the 96-byte block the shader reads.
static void testPack() {
    AVER_INFO("=== material: the packed GPU block ===");
    // 96 since `ior` and `transmission` were added: 80 (itself 64 plus slopeBlendLo/Hi,
    // layer1UvScale and one pad, when the slope-blended second layer landed) plus those two floats,
    // plus two pad floats to stay 16-byte aligned.
    //
    // THIS CHECK EARNED ITS KEEP TWICE NOW -- it is what caught both size changes, and the reason it
    // matters is that PbrShaders.cpp's `cbuffer AverMaterial` mirrors this struct BY HAND. A
    // mismatch there does not fail to build: it silently shifts every field after the insertion
    // point, so a material's roughness starts reading its neighbour's bytes and the image goes
    // subtly wrong with nothing to grep for. Note this is a RUNTIME check rather than a
    // static_assert, which is why the 80 -> 96 growth compiled clean and would have failed the suite
    // instead -- keep it that way, since the point is to be told, not to be stopped.
    check(sizeof(pbr::MaterialConstants) == 176, "MaterialConstants is 176 bytes");
    check(sizeof(pbr::MaterialConstants) % 16 == 0, "...and a legal constant-buffer size");

    // ---- THE FIELD LAYOUT, NOT JUST THE TOTAL ----
    //
    // The size check above catches a field ADDED to one mirror and not the others. It cannot catch a
    // field REORDERED, or two fields of the same type swapped, because the total does not move -- and
    // that is the failure with no symptom to grep for: every value stays a plausible float, just the
    // wrong one, and a material reads its neighbour's roughness.
    //
    // THERE ARE THREE HAND-MAINTAINED COPIES of this struct and nothing has ever tied their ORDER
    // together: the C++ here, `cbuffer AverMaterial` in modules/render.pbr/shaders/
    // material_prelude.hlsl, and `struct RtMaterial` in modules/render.voxi/shaders/voxi.hlsl (the
    // ray-traced path's own copy, which is easy to forget precisely because it is in another module).
    //
    // Offsets are asserted rather than derived, so this fails when someone moves a field without
    // moving it in the shaders too. When it fails, fix all three; do not just update the number.
    //
    // THAT SENTENCE WAS A CLAIM THIS BLOCK COULD NOT BACK until the mirror check at the end of it
    // existed. offsetof only ever asked the C++ struct about the C++ struct; a field moved in
    // material_prelude.hlsl or voxi.hlsl and nowhere else passed every line below. The two shaders
    // are now actually read -- see kMirror.
    // EVERY FIELD, not the section boundaries. This list used to hold 14 of the struct's 26 offsets,
    // which sounds thorough and left the single most dangerous stretch unguarded: emissiveFactor
    // through alphaCutoff is SIX CONSECUTIVE SAME-TYPED FLOATS. Swap metallicFactor and
    // roughnessFactor in one of the three mirrors and the size never moves, the build succeeds, every
    // suite here passes, and every material in the engine shades with its metallic and roughness
    // exchanged. That is exactly the failure this block's own comment describes; it just was not
    // checking for it. A gap in a guard is worse than no guard, because it is read as coverage.
    check(offsetof(pbr::MaterialConstants, baseColorFactor)  ==  0, "baseColorFactor at 0");
    check(offsetof(pbr::MaterialConstants, emissiveFactor)   == 16, "emissiveFactor at 16");
    check(offsetof(pbr::MaterialConstants, metallicFactor)   == 28, "metallicFactor at 28");
    check(offsetof(pbr::MaterialConstants, roughnessFactor)  == 32, "roughnessFactor at 32");
    check(offsetof(pbr::MaterialConstants, normalScale)      == 36, "normalScale at 36");
    check(offsetof(pbr::MaterialConstants, occlusionStrength) == 40, "occlusionStrength at 40");
    check(offsetof(pbr::MaterialConstants, alphaCutoff)      == 44, "alphaCutoff at 44");
    check(offsetof(pbr::MaterialConstants, flags)            == 48, "flags at 48");
    check(offsetof(pbr::MaterialConstants, reflectance)      == 52, "reflectance at 52");
    check(offsetof(pbr::MaterialConstants, f90)              == 56, "f90 at 56");
    check(offsetof(pbr::MaterialConstants, uvTilesPerCm)     == 60, "uvTilesPerCm at 60");
    check(offsetof(pbr::MaterialConstants, slopeBlendLo)     == 64, "slopeBlendLo at 64");
    check(offsetof(pbr::MaterialConstants, slopeBlendHi)     == 68, "slopeBlendHi at 68");
    check(offsetof(pbr::MaterialConstants, layer1UvScale)    == 72, "layer1UvScale at 72");
    check(offsetof(pbr::MaterialConstants, graphId)          == 76, "graphId at 76");
    check(offsetof(pbr::MaterialConstants, ior)              == 80, "ior at 80");
    check(offsetof(pbr::MaterialConstants, transmission)     == 84, "transmission at 84");
    check(offsetof(pbr::MaterialConstants, subsurfaceWeight) == 88, "subsurfaceWeight at 88");
    check(offsetof(pbr::MaterialConstants, subsurfaceRadius) == 92, "subsurfaceRadius at 92");
    check(offsetof(pbr::MaterialConstants, coatWeight)       == 96, "coatWeight at 96");
    check(offsetof(pbr::MaterialConstants, coatRoughness)    == 100, "coatRoughness at 100");
    check(offsetof(pbr::MaterialConstants, coatF0)           == 104, "coatF0 at 104");
    // Checked even though nothing reads it: _coatPad is what keeps the coat row 16 bytes, and the
    // HLSL mirrors have no such field -- they rely on uint4 TexIndex0 being pushed to the next row
    // automatically. The two agree, for different reasons, so the C++ side's reason is worth pinning.
    check(offsetof(pbr::MaterialConstants, _coatPad)         == 108, "_coatPad at 108");

    // The bindless texture-index block. Checked at its offset like every field above, and then
    // checked for CONTENT too, because this is the one field where a plausible-looking zero is the
    // dangerous value: 0 is a real index into the ray path's texture table, so a material that
    // never got resident textures must carry kUnboundTexture rather than a default-constructed 0,
    // or it samples whatever landed in slot 0 and looks like a content bug rather than a code one.
    check(offsetof(pbr::MaterialConstants, texIndex)         == 112, "texIndex at 112");
    // The volume-absorption row, appended after texIndex when the block grew 144 -> 160. Offsets
    // asserted for the same reason as every field above: three hand-maintained mirrors, and a field
    // moved in one of them shades a material with its neighbour's bytes rather than failing to build.
    check(offsetof(pbr::MaterialConstants, attenuationColor)    == 144, "attenuationColor at 144");
    check(offsetof(pbr::MaterialConstants, attenuationDistance) == 156, "attenuationDistance at 156");
    // The lamp-light row, appended after attenuationDistance when the block grew 160 -> 176. Same
    // reasoning as every offset above: three hand-maintained mirrors, and a field moved in one of
    // them shades a material with its neighbour's bytes rather than failing to build.
    check(offsetof(pbr::MaterialConstants, lightIntensity)      == 160, "lightIntensity at 160");
    // subsurfaceColor spends the row's other three floats -- the ones lightIntensity's own comment
    // said were padding -- so the block stays 176 bytes. Same reasoning as every offset above.
    check(offsetof(pbr::MaterialConstants, subsurfaceColor)     == 164, "subsurfaceColor at 164");

    // ---- AND NOW THE OTHER TWO MIRRORS, WHICH THIS BLOCK NEVER USED TO OPEN ----
    //
    // Everything above pins the C++ struct to itself. See kMirror's own comment for why that is a
    // tautology and what these two files can silently do that no offsetof can see.
    {
        const std::string prelude = readRepoFile("modules/render.pbr/shaders/material_prelude.hlsl");
        // BOTH HALVES OF THE VOXI SHADER, because it is no longer one file. `struct RtMaterial`
        // moved into voxi_rt.hlsli when voxi.hlsl was split, and reading only voxi.hlsl made
        // blockAfter() return nothing -- which this block's own guard comment predicts would let the
        // ordering loop pass without comparing anything. Concatenating the two keeps the assertion
        // about the SHADER rather than about a filename, so the struct moving between voxi.hlsl and
        // its includes is a refactor this test correctly ignores.
        const std::string voxi    = readRepoFile("modules/render.voxi/shaders/voxi.hlsl") + "\n" +
                                    readRepoFile("modules/render.voxi/shaders/voxi_rt.hlsli");
        // A MISSING FILE IS A FAILURE, NOT A SKIP. Every declPos below would return npos against an
        // empty string and the ordering loop would pass without comparing anything -- the exact
        // shape of guard this whole block exists because of.
        check(!prelude.empty(), "material_prelude.hlsl is readable");
        check(!voxi.empty(),    "voxi.hlsl is readable");

        const std::string cb = blockAfter(prelude, "cbuffer AverMaterial");
        // The ray path's RtMaterial is a typedef of material_prelude.hlsl's AverMaterialData (one
        // material struct for every path), so that struct is the second mirror.
        const std::string rt = blockAfter(prelude, "struct AverMaterialData");
        check(voxi.find("typedef AverMaterialData RtMaterial") != std::string::npos,
              "voxi_rt.hlsli's RtMaterial is the prelude's AverMaterialData");
        check(!cb.empty(), "...and declares `cbuffer AverMaterial`");
        check(!rt.empty(), "...and declares `struct AverMaterialData`");

        // Every field of the table must be DECLARED in each mirror that claims to carry it. A field
        // added to the C++ struct and forgotten in a shader shifts everything after it.
        bool allPresent = true;
        for (const MirrorField& f : kMirror) {
            if (f.prelude && declPos(cb, f.prelude) == std::string::npos) {
                AVER_ERROR("   ...`{}` (C++ {}) is not declared in cbuffer AverMaterial", f.prelude, f.cpp);
                allPresent = false;
            }
            if (f.rt && declPos(rt, f.rt) == std::string::npos) {
                AVER_ERROR("   ...`{}` (C++ {}) is not declared in struct RtMaterial", f.rt, f.cpp);
                allPresent = false;
            }
        }
        check(allPresent, "every MaterialConstants field is declared in both HLSL mirrors");

        // THE ORDER, which is the whole point: positions must be strictly increasing. Swapping two
        // same-typed floats in either shader moves one name past the other and lands here, where
        // before it landed in a rendered image weeks later.
        bool ordered = true;
        usize lastCb = 0, lastRt = 0;
        const char* lastCbName = "<start>"; const char* lastRtName = "<start>";
        for (const MirrorField& f : kMirror) {
            if (f.prelude) {
                const usize p = declPos(cb, f.prelude);
                if (p != std::string::npos) {
                    if (p < lastCb) {
                        AVER_ERROR("   ...cbuffer AverMaterial declares `{}` BEFORE `{}`; the C++ "
                                   "struct has them the other way round", f.prelude, lastCbName);
                        ordered = false;
                    }
                    lastCb = p; lastCbName = f.prelude;
                }
            }
            if (f.rt) {
                const usize p = declPos(rt, f.rt);
                if (p != std::string::npos) {
                    if (p < lastRt) {
                        AVER_ERROR("   ...struct RtMaterial declares `{}` BEFORE `{}`; the C++ "
                                   "struct has them the other way round", f.rt, lastRtName);
                        ordered = false;
                    }
                    lastRt = p; lastRtName = f.rt;
                }
            }
        }
        check(ordered, "...and in the SAME ORDER as the C++ struct, in both");

        // The table must not itself fall behind the struct. sizeof is the only handle the test has
        // on "a field was added": if someone appends a row to MaterialConstants and does not extend
        // kMirror, the loops above would still pass while checking a prefix.
        check(sizeof(pbr::MaterialConstants) == 176,
              "...and kMirror covers the whole struct (extend it if this size ever changes)");
    }
    {
        // ZERO IS THE OFF STATE, and the whole no-flag-bit design rests on it: a default material
        // must carry a distance of 0 so averVolumeTransmittance returns exactly 1 and every material
        // authored before this row existed shades bit-identically.
        const pbr::MaterialConstants fresh = pbr::packMaterial(pbr::MaterialDesc{});
        check(fresh.attenuationDistance == 0.0f, "...and a default material has NO volume");
    }
    {
        const pbr::MaterialConstants fresh = pbr::packMaterial(pbr::MaterialDesc{});
        bool allUnbound = true;
        for (u32 i = 0; i < pbr::kTextureSlotCount; ++i)
            if (fresh.texIndex[i] != pbr::kUnboundTexture) allUnbound = false;
        check(allUnbound, "...and packMaterial leaves every slot UNBOUND, not 0");
        check(pbr::kUnboundTexture != 0u, "...which means something, because 0 is a valid index");
    }

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

    // THE TRANSPARENCY FIELDS ACTUALLY SURVIVING THE PACK, which nothing asserted before.
    //
    // This block exists because the gap was real and load-bearing: `transmission` was copied into
    // MaterialConstants by exactly one line, and deleting that line left the whole suite green while
    // every transmissive material in the engine silently lost its transmission. A field that reaches
    // the GPU through one unasserted assignment is a field that will one day stop reaching it.
    pbr::MaterialDesc g;
    g.baseColorFactor[0] = 0.86f; g.baseColorFactor[1] = 0.93f;
    g.baseColorFactor[2] = 0.88f; g.baseColorFactor[3] = 0.12f;
    g.transmission = 0.92f;
    g.ior          = 1.52f;
    g.alphaMode    = pbr::AlphaMode::Blend;
    g.alphaCutoff  = 0.25f;
    c = pbr::packMaterial(g);
    check(near(c.transmission, 0.92f), "transmission survives packMaterial");
    check(near(c.ior, 1.52f), "ior survives packMaterial");
    check(near(c.alphaCutoff, 0.25f), "alphaCutoff survives packMaterial");
    check(near(c.baseColorFactor[3], 0.12f), "baseColorFactor.a is NOT sRGB-decoded, only rgb is");
    check(near(c.baseColorFactor[0], std::pow(0.86f, 2.2f), 1e-4f),
          "while baseColorFactor.rgb IS: the .ocmat holds it sRGB-encoded, decoded with pow 2.2");
    check((c.flags & pbr::MaterialFlag_AlphaBlend) != 0, "BLEND translucent sets AlphaBlend");
    check((c.flags & pbr::MaterialFlag_AlphaMask) == 0, "...and not AlphaMask");

    // castShadow, whose ONLY consumer is the flag bit checked here -- see MaterialGpu.hpp.
    check((c.flags & pbr::MaterialFlag_CastShadow) != 0, "castShadow defaults to true and is packed");
    g.castShadow = false;
    c = pbr::packMaterial(g);
    check((c.flags & pbr::MaterialFlag_CastShadow) == 0, "castShadow=0 clears the bit");

    // subsurfaceWeight/subsurfaceRadius surviving the pack, and MaterialFlag_Subsurface tracking the
    // WEIGHT alone -- same shape as the transmission block above, and the same reason it matters:
    // packMaterial assigns both fields by hand (MaterialGpu.cpp's own comment: "every one of its
    // members is assigned here rather than some being left at the zero the `MaterialConstants c{}`
    // above gives them"), so a deleted assignment line would silently zero the GPU-side value while
    // this suite stayed green.
    check((c.flags & pbr::MaterialFlag_Subsurface) == 0,
          "subsurfaceWeight defaults to 0.0 and packs with MaterialFlag_Subsurface clear");
    g.subsurfaceWeight = 0.6f;
    g.subsurfaceRadius = 0.35f;
    c = pbr::packMaterial(g);
    check(near(c.subsurfaceWeight, 0.6f), "subsurfaceWeight survives packMaterial");
    check(near(c.subsurfaceRadius, 0.35f), "subsurfaceRadius survives packMaterial");
    check((c.flags & pbr::MaterialFlag_Subsurface) != 0,
          "subsurfaceWeight > 0 sets MaterialFlag_Subsurface (bit 14)");

    // subsurfaceColor is DECODED like baseColorFactor above (sRGB -> linear, pow 2.2), NOT forwarded
    // raw the way attenuationColor and subsurfaceWeight/Radius are just above -- it is picked in the
    // same colour swatch widget baseColorFactor is, so it needs the same gamma correction
    // (MaterialGpu.cpp's packMaterial: "picked as a colour, in the same swatch widget"). 0 and 1 are
    // both fixed points of pow(x, 2.2), so 0.5 is the one value that actually discriminates a decode
    // from a pass-through: pow(0.5, 2.2) ~= 0.2176.
    g.subsurfaceColor[0] = 0.5f; g.subsurfaceColor[1] = 1.0f; g.subsurfaceColor[2] = 0.0f;
    c = pbr::packMaterial(g);
    check(std::fabs(c.subsurfaceColor[0] - 0.2176f) < 0.001f,
          "subsurfaceColor is sRGB-decoded: 0.5 -> ~0.2176, not passed through raw");
    check(near(c.subsurfaceColor[1], 1.0f), "...1.0 decodes to 1.0 (pow(1,2.2)=1, a fixed point)");
    check(near(c.subsurfaceColor[2], 0.0f), "...and 0.0 decodes to 0.0, the other fixed point");

    // KEYED ON THE WEIGHT ALONE (packMaterial's own comment): a radius with no weight scatters
    // nothing, so the flag must stay clear even though the radius is still nonzero here.
    g.subsurfaceWeight = 0.0f;
    c = pbr::packMaterial(g);
    check((c.flags & pbr::MaterialFlag_Subsurface) == 0,
          "subsurfaceWeight=0 clears the bit even with subsurfaceRadius still set to 0.35");

    // lightIntensity surviving the pack, and MaterialFlag_Light tracking it alone -- same shape as
    // the subsurface/transmission blocks above, and the same reason: packMaterial assigns this field
    // by hand, so a deleted assignment line would silently zero the GPU-side value while this suite
    // stayed green.
    check((c.flags & pbr::MaterialFlag_Light) == 0,
          "lightIntensity defaults to 0.0 and packs with MaterialFlag_Light clear");
    g.lightIntensity = 8.0f;
    c = pbr::packMaterial(g);
    check(near(c.lightIntensity, 8.0f), "lightIntensity survives packMaterial");
    check((c.flags & pbr::MaterialFlag_Light) != 0,
          "lightIntensity > 0 sets MaterialFlag_Light (bit 16)");
    g.lightIntensity = 0.0f;
    c = pbr::packMaterial(g);
    check((c.flags & pbr::MaterialFlag_Light) == 0, "lightIntensity=0 clears the bit again");
}

// The one translucency predicate, and the shadow transmittance derived from it.
static void testTranslucency() {
    AVER_INFO("=== material: isTranslucent / shadowTransmittance ===");

    pbr::MaterialDesc opaque;
    check(!pbr::isTranslucent(opaque), "a default material is not translucent");

    // THE CASE THAT MAKES THIS NOT A GLASS SPECIAL CASE: transmission on an OPAQUE blend mode.
    pbr::MaterialDesc opaqueTransmissive;
    opaqueTransmissive.transmission = 0.5f;
    check(pbr::isTranslucent(opaqueTransmissive),
          "transmission alone makes a material translucent, whatever its blend mode");

    pbr::MaterialDesc blended;
    blended.alphaMode = pbr::AlphaMode::Blend;
    check(pbr::isTranslucent(blended), "BLEND translucent is translucent with no transmission set");

    f32 t[3] = {0, 0, 0};

    // castShadow off means it casts NOTHING, whatever else it says.
    pbr::MaterialDesc noShadow;
    noShadow.castShadow = false;
    noShadow.baseColorFactor[0] = noShadow.baseColorFactor[1] = noShadow.baseColorFactor[2] = 0.0f;
    noShadow.baseColorFactor[3] = 1.0f;   // fully opaque black: would cast a SOLID shadow otherwise
    pbr::shadowTransmittance(noShadow, t);
    check(near(t[0], 1.0f) && near(t[1], 1.0f) && near(t[2], 1.0f),
          "castShadow=0 passes all light, even on an opaque black material");

    // A solid material blocks everything.
    pbr::MaterialDesc solid;
    solid.baseColorFactor[3] = 1.0f;
    pbr::shadowTransmittance(solid, t);
    check(near(t[0], 0.0f) && near(t[1], 0.0f) && near(t[2], 0.0f), "an opaque material blocks all light");

    // A tinted pane: k = max(1 - 0.12, 0.92) = 0.92, tinted by the base colour.
    pbr::MaterialDesc glass;
    glass.alphaMode = pbr::AlphaMode::Blend;
    glass.baseColorFactor[0] = 0.5f; glass.baseColorFactor[1] = 1.0f;
    glass.baseColorFactor[2] = 0.5f; glass.baseColorFactor[3] = 0.12f;
    glass.transmission = 0.92f;
    pbr::shadowTransmittance(glass, t);
    check(near(t[0], 0.46f), "tinted: red is halved by the base colour");
    check(near(t[1], 0.92f), "tinted: green passes at the full k");
    check(near(t[2], 0.46f), "tinted: blue is halved too");
    check(t[0] < t[1], "AND THE SHADOW IS ACTUALLY TINTED, not merely dimmed");
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

    // ---- volume absorption round-trips, and is OPT-IN in the output ----
    //
    // kFull authors no volume, so the writer must emit neither PARAM -- the same byte-stability rule
    // subsurface and the coat already follow. Writing "attenuationDistance 0" into every material
    // would turn every fixture in this tree into a diff for a line meaning "not in use".
    check(text.find("attenuationDistance") == std::string::npos,
          "a material with no volume writes no attenuation PARAMs");
    check(b.attenuationDistance == 0.0f, "...and re-parses with no volume");

    // Now one that DOES author a volume. Checked by writing and re-parsing rather than by parsing a
    // literal, because the failure this guards is a writer that emits a token the parser does not
    // accept -- which a one-directional test cannot see.
    {
        pbr::MaterialDesc v = a;
        v.attenuationColor[0] = 0.15f; v.attenuationColor[1] = 0.85f; v.attenuationColor[2] = 0.35f;
        v.attenuationDistance = 12.5f;
        const std::string vtext = fmt::writeOcmat(v, &exA);
        check(vtext.find("PARAM attenuationDistance") != std::string::npos,
              "an authored volume IS written");
        pbr::MaterialDesc w;
        std::string verr;
        if (!fmt::parseOcmat(vtext, w, nullptr, &verr)) {
            AVER_ERROR("   re-parse of an authored volume failed: {}", verr);
            ++g_failures;
        } else {
            check(near(w.attenuationDistance, 12.5f), "attenuationDistance round-trips");
            check(near(w.attenuationColor[0], 0.15f) && near(w.attenuationColor[1], 0.85f)
               && near(w.attenuationColor[2], 0.35f),
                  "attenuationColor round-trips all three channels, in order");
        }
    }
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

// Checks generateMipChain: sRGB filtering in linear light, and normal maps averaged as vectors that keep
// their length (the spread specular anti-aliasing reads).
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
    check(avg[2] >= 229 && avg[2] <= 231, "the average keeps its length, 0.8 for this spread (got " + std::to_string(avg[2]) + ")");

    const fmt::TextureData cancel = mip1of({
        255, 128, 128, 255,     0, 127, 127, 255,
        255, 128, 128, 255,     0, 127, 127, 255,
    });
    const u8* flat = cancel.levels[1].pixels.data();
    check(flat[0] == 128 && flat[1] == 128 && flat[2] >= 159 && flat[2] <= 160,
          "an exactly-cancelling block falls back to the flat direction at the 0.25 length floor, never to a NaN");
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

// ---- the shared material cook: glTF, OBJ and USD all land here --------------------------------
//
// Writes into a scratch directory rather than the project, because that is the only way to exercise
// the part with judgement in it: which files get written, under what names, and which texture slots
// end up bound. The .ocmat CONTENT is already covered above by the parser tests -- this is about the
// step between an importer and those files.
static void testCook() {
    AVER_INFO("=== the shared material cook ===");

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "aver_material_cook_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Two images the cook is asked to write. Only the magic bytes matter: the cook copies bytes and
    // never decodes, so the sniff is the only thing reading them.
    const u8 kPngMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const u8 kTiffMagic[4] = {'I', 'I', '*', 0x00};

    std::vector<fmt::ImportedImage> images(2);
    images[0].bytes.assign(kPngMagic, kPngMagic + 8);
    images[0].bytes.resize(64, 0);
    images[0].ext = ".png";
    images[0].suggestedName = "colour";
    images[0].sourcePath = "colour.png";
    images[0].ok = true;

    images[1].bytes.assign(kTiffMagic, kTiffMagic + 4);
    images[1].bytes.resize(64, 0);
    images[1].ext = ".tif";
    images[1].suggestedName = "basecolour";
    images[1].sourcePath = "textures/basecolour.tif";
    images[1].ok = true;

    std::vector<fmt::ImportedMaterial> mats(3);
    mats[0].name = "Good";
    mats[0].baseColorTex.imageIndex = 0;
    mats[1].name = "Tiffy";
    mats[1].baseColorTex.imageIndex = 1;
    // A generated material that happened to be called M_Crate would silently repaint every crate in
    // the project, because an authored .ocmat WINS over the built-in look of the same name.
    mats[2].name = "M_Crate";

    fmt::MaterialCookOptions opt;
    opt.contentDir = dir.string();
    opt.assetBase  = "Fixture";
    opt.overwriteExisting = true;

    fmt::MaterialCookResult res;
    std::vector<std::string> warn;
    std::string err;
    check(fmt::cookMaterials(mats, images, opt, res, &warn, &err), "the cook runs: " + err);
    check(res.materialsWritten == 3, "all three materials were written");
    check(res.texturesWritten == 1, "but only ONE texture -- the TIFF is not copied in");

    check(std::filesystem::exists(dir / "Textures" / "Fixture" / "colour.png", ec),
          "the PNG landed under Textures/<assetBase>/");
    check(!std::filesystem::exists(dir / "Textures" / "Fixture" / "basecolour.tif", ec),
          "and the TIFF did NOT, because nothing in the engine could decode it");

    check(!res.texturePaths[0].empty() && res.texturePaths[1].empty(),
          "so only the PNG has a content-relative path");

    bool namedTiff = false;
    for (const std::string& w : warn)
        if (w.find("basecolour.tif") != std::string::npos && w.find("TIFF") != std::string::npos)
            namedTiff = true;
    check(namedTiff, "the refusal names the file AND the container, rather than failing silently");

    // THE DISCRIMINATING PART for the sniff: the material must come out UNTEXTURED. Writing the file
    // and emitting a TEX record for it would turn a stated import limit into a material that fails
    // to load at run time, a long way from the import that caused it.
    std::string text;
    {
        std::ifstream f(dir / "Materials" / "Fixture_Tiffy.ocmat");
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    check(!text.empty(), "the TIFF material was still written -- geometry keeps a material to bind");
    check(text.find("TEX baseColor") == std::string::npos,
          "with NO baseColor TEX record, so its slot is unbound rather than pointing at nothing");

    std::string good;
    {
        std::ifstream f(dir / "Materials" / "Fixture_Good.ocmat");
        good.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    check(good.find("TEX baseColor {path:Textures/Fixture/colour.png}") != std::string::npos,
          "while the decodable one IS bound, by a content-relative path");

    // THE PREFIX IS WHAT PROTECTS THE BUILT-IN LOOKS, not the reserved-name check underneath it: a
    // source material called M_Crate is written as <assetBase>_M_Crate, which cannot collide. The
    // check stays as a backstop for an assetBase policy that ever allowed an empty prefix, but it is
    // unreachable today -- so this asserts the protection that is actually load-bearing.
    check(!res.materialSlotNames[2].empty() && !fmt::isReservedLookName(res.materialSlotNames[2]),
          "a source material named after a built-in look cannot be written under that name");
    check(res.materialSlotNames[2] == "Fixture_M_Crate",
          "because the assetBase prefix is what keeps it out of the built-ins' namespace");

    std::filesystem::remove_all(dir, ec);
}

// A FLAT IMPORTED COLOUR, END TO END: glTF file -> importer -> cook -> .ocmat on disk -> parser ->
// packed GPU block. glTF's baseColorFactor is LINEAR and the .ocmat holds it sRGB-encoded, so the cook
// has to encode it for the pack step's decode to hand the shader the colour the file stated. It used
// to cross unconverted and be decoded anyway: a flat 0.5 grey reached the shader as 0.5^2.2 = 0.22.
// An OBJ-style factor (Kd, recorded as sRGB) rides alongside and must cross untouched.
static void testImportedColour() {
    AVER_INFO("=== an imported flat colour reaches the packed block unchanged ===");

    // The smallest document the importer accepts: an accessor with no bufferView reads as zeros, so
    // the mesh is a degenerate triangle and no buffer is needed -- only the material matters here.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
        "\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"materials\":[{\"name\":\"Grey\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.5,0.5,0.25]}}]}";
    fmt::GltfImportResult res;
    std::string why;
    check(fmt::importGltfFromMemory(reinterpret_cast<const u8*>(json.data()), json.size(), "", res, {}, &why),
          "the glTF fixture imports: " + why);
    if (res.materials.size() != 1) { check(false, "and has its one material"); return; }

    std::vector<fmt::ImportedMaterial> mats = res.materials;
    fmt::ImportedMaterial kd;
    kd.name = "Kd";
    kd.baseColorFactor[0] = kd.baseColorFactor[1] = kd.baseColorFactor[2] = 0.5f;
    kd.baseColorFactorLinear = false;
    mats.push_back(kd);

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "aver_material_colour_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    fmt::MaterialCookOptions opt;
    opt.contentDir = dir.string();
    opt.assetBase  = "Colour";
    opt.overwriteExisting = true;
    fmt::MaterialCookResult cooked;
    std::string err;
    check(fmt::cookMaterials(mats, {}, opt, cooked, nullptr, &err), "the cook runs: " + err);

    pbr::MaterialDesc grey, kdDesc;
    check(fmt::loadOcmat((dir / "Materials" / "Colour_Grey.ocmat").string(), grey),
          "the glTF material's .ocmat reads back");
    check(fmt::loadOcmat((dir / "Materials" / "Colour_Kd.ocmat").string(), kdDesc),
          "and so does the sRGB one's");

    check(near(grey.baseColorFactor[0], std::pow(0.5f, 1.0f / 2.2f), 1e-4f),
          "the .ocmat holds the linear 0.5 sRGB-encoded (0.7297), the file format's own encoding");
    check(near(grey.baseColorFactor[3], 0.25f), "alpha is coverage and crosses as it is");
    const pbr::MaterialConstants gc = pbr::packMaterial(grey);
    check(near(gc.baseColorFactor[0], 0.5f, 1e-4f) && near(gc.baseColorFactor[1], 0.5f, 1e-4f) &&
          near(gc.baseColorFactor[2], 0.5f, 1e-4f),
          "and the shader gets the 0.5 the glTF stated -- not 0.22");
    check(near(gc.baseColorFactor[3], 0.25f), "with alpha still 0.25");

    check(near(kdDesc.baseColorFactor[0], 0.5f, 1e-5f),
          "a factor already in sRGB (OBJ's Kd) is written unconverted");
    const pbr::MaterialConstants kc = pbr::packMaterial(kdDesc);
    check(near(kc.baseColorFactor[0], std::pow(0.5f, 2.2f), 1e-4f),
          "and decodes as the sRGB colour it always was");

    std::filesystem::remove_all(dir, ec);
}

// Runs every material test. Returns the failure count.
int main() {
    testFullParse();
    testTolerance();
    testGraphRef();
    testDielectric();
    testSubsurface();
    testLightIntensity();
    testPack();
    testTranslucency();
    testRoundTrip();
    testBlendModes();
    testMipChain();
    testGeneratedByCsharp();
    testScriptRewrite();
    testCook();
    testImportedColour();

    if (g_failures == 0) AVER_INFO("=== all material tests passed ===");
    else AVER_ERROR("=== {} material assertion(s) failed ===", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
