// AverAssetC — the standalone asset compiler the launcher's Aver Exchange feature invokes out of
// process.
//
// Not built on top of ConvertTool.cpp, but built FROM it: that harness proved the import, merge and
// reload-and-verify path against real files (see its own header comment for the polycount numbers
// that motivated the merge), but it is a dev tool that writes AVER_INFO text and only reads glTF. A
// caller across a process boundary needs a result it can parse without screen-scraping and formats
// beyond glTF, so this tool ports ConvertTool's merge algorithm and verify discipline unchanged and
// adds: OBJ/USDA/audio dispatch, per-mesh output by default (a multi-prop source stays separable
// unless --merge is asked for), and JSON-Lines results instead of log lines.
//
// THE RESULT CONTRACT. AVER_INFO/WARN/ERROR always write to stdout (Log.cpp's logWrite does that
// unconditionally; setLogSink only mirrors, it does not suppress), and every one of those lines
// starts with '['. So this tool's own result records are the only lines starting with '{': one JSON
// object PER ARTIFACT, flushed immediately after it is written, plus a required final
// {"summary":true,...} line on a clean exit. One line per artifact rather than one document for the
// whole run means a crash after writing N of M outputs still leaves N individually verified,
// trustworthy result lines -- a caller that never saw the summary line treats the whole run as
// incomplete, but nothing already reported as ok+verified is a lie.
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/ObjImport.hpp"
#include "aver/formats/UsdImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"   // the scene level a multi-node glTF now writes
#include "aver/platform/FileSystem.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#if AVER_HAVE_AUDIO_IMPORT
#include "aver/formats/OcAudio.hpp"
#endif
#if AVER_HAVE_MATERIAL_COMPILE
// Under the SAME guard as the `material` subcommand, and reusing its macro rather than adding a
// second one: both need exactly Aver.Formats.Material to be linked, and two names for one condition
// is a worse thing to keep in step than one name doing two jobs.
//
// ImportCook.hpp, not MaterialCook.hpp directly: cookAndRewriteSlots below is now a thin wrapper
// around fmt::cookImportedMaterials, which does the opacity fold, the texture-size cap and the call
// into cookMaterials itself. See ImportCook.hpp for why that policy layer lives in Aver.Formats.Material
// rather than in this tool.
#include "aver/formats/ImportCook.hpp"
#endif

// Material generation (texture set -> .ocmat) is OPTIONAL for the same reason audio import above is:
// Aver.Formats.Material only builds under AVER_MODULE_PBR (see its own CMakeLists.txt comment), so a
// tree configured without it still has to build this tool. stb_image_write is vendored the same way
// tools/MakeFoliage.cpp already uses it -- a second STB_IMAGE_WRITE_IMPLEMENTATION TU is fine since
// these are separate executables with no duplicate symbol to collide.
#if AVER_HAVE_MATERIAL_COMPILE
#include "aver/formats/OcMat.hpp"
// Image.hpp for decodeImage, which packMetalRough below needs to pack a separate roughness and
// metallic map into the engine's one combined slot. (generateMipChain's own THROUGH-the-mip-chain
// --max-texture cap moved to ImportCook.cpp with the rest of cookAndRewriteSlots -- see its header.)
#include "aver/platform/Image.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

// Emitting the material's own .ocgraph is optional ON TOP of material generation, and separately
// guarded, because it needs one library more: Aver.Render.PBR.Materials, for the compiler that
// proves the generated graph shades. Writing the file needs only Aver.Formats, which is always
// linked -- but a graph this tool could not COMPILE is a graph it must not point a .ocmat at, so
// there is no useful half of this feature to ship without the compiler and the guard covers both.
#if AVER_HAVE_MATERIAL_GRAPH
#include "aver/formats/OcGraph.hpp"
#include "aver/pbr/MaterialGraphHlsl.hpp"
#endif

// Clustering is entirely OPTIONAL, exactly as in ConvertTool.cpp: this tool must still import and
// write .ocmesh with AVER_MODULE_TRIFACTOR=OFF (the tree's default), so every use of it is behind
// this guard, reached only through the link interface (`if(TARGET Aver.Trifactor)` below).
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

namespace {

// ---- small helpers, ported from ConvertTool.cpp (see that file for the reasoning) ----

std::string stemOf(const std::string& path) {
    usize a = path.find_last_of("/\\");
    a = (a == std::string::npos) ? 0 : a + 1;
    const usize b = path.find_last_of('.');
    return path.substr(a, (b == std::string::npos || b < a) ? std::string::npos : b - a);
}

std::string extOf(const std::string& path) {
    const usize b = path.find_last_of('.');
    if (b == std::string::npos) return {};
    std::string e = path.substr(b);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// A name safe to hang on a file, so an unnamed or oddly-named source node cannot escape into a path.
std::string safe(const std::string& in, const std::string& fallback) {
    std::string out;
    for (char c : in)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out.push_back(c);
    return out.empty() ? fallback : out;
}

// ---- the JSON-Lines result contract ----

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void emit(const std::string& json) {
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);   // a caller reading line-by-line must see this before the process can crash
}

struct RunStats { int written = 0; int failed = 0; };

void emitArtifact(const std::string& input, const std::string& output, const char* kind,
                  bool ok, bool verified, const std::string& error, RunStats& stats,
                  const std::string& extra = {}) {
    std::string j = "{\"schemaVersion\":1,\"input\":\"" + jsonEscape(input) + "\",\"output\":\""
        + jsonEscape(output) + "\",\"kind\":\"" + kind + "\",\"ok\":" + (ok ? "true" : "false")
        + ",\"verified\":" + (verified ? "true" : "false");
    if (!error.empty()) j += ",\"error\":\"" + jsonEscape(error) + "\"";
    if (!extra.empty()) j += "," + extra;
    j += "}";
    emit(j);
    if (ok && verified) ++stats.written; else ++stats.failed;
}

void emitSummary(const std::string& input, const RunStats& stats, int exitCode) {
    emit("{\"schemaVersion\":1,\"summary\":true,\"input\":\"" + jsonEscape(input)
        + "\",\"itemsWritten\":" + std::to_string(stats.written)
        + ",\"itemsFailed\":" + std::to_string(stats.failed)
        + ",\"exitCode\":" + std::to_string(exitCode) + "}");
}

// ---- material generation: texture-set classification, roughness+metal packing, .ocmat writing ----
//
// Unlike `convert`, which owns exactly ONE input file end to end and dispatches by its extension,
// `material` takes a whole SET of texture files -- every map belonging to one downloaded item -- as
// input, because a texture set is only meaningful together (a lone roughness map binds nothing on
// its own). There is no per-file extension to dispatch on the way `convert` dispatches by extension,
// so this is its own subcommand rather than a mode of `convert`.

#if AVER_HAVE_MATERIAL_COMPILE

// WHERE --out-dir SITS INSIDE THE PROJECT, stated once because two different records depend on it.
//
// This tool is handed an ABSOLUTE --out-dir and has no idea where the content root is, yet both the
// .ocmat's texture paths and its GRAPHREF have to be CONTENT-RELATIVE -- the editor resolves each as
// `contentDir + "\\" + ref` (SandboxApp::resolveAssetPath and ::resolveMaterialGraph). The tool has
// always closed that gap by assuming the caller places it: it writes every texture reference as
// "Textures/<file>" while copying the file to --out-dir, which is only correct if --out-dir IS
// <content>/Textures.
//
// THE GRAPH MUST MAKE THE SAME ASSUMPTION OR IT SILENTLY DOES NOTHING. A GRAPHREF that resolves to a
// path with no file there is not an error a user sees: resolveMaterialGraph logs it and returns 0,
// and the material shades through the stock path looking almost right. Sharing one constant means the
// textures and the graph cannot disagree about the layout, and that a future change to it is one edit
// rather than two that must be found.
const std::string kContentRelPrefix = "Textures/";

// The filename with its extension, unlike stemOf/extOf: a texture's destination name in the project
// is the SAME name it downloaded with (Aver Exchange never renames a texture, only meshes get a
// synthesised name from --base).
std::string filenameOf(const std::string& path) {
    const usize a = path.find_last_of("/\\");
    return a == std::string::npos ? path : path.substr(a + 1);
}

// Which .ocmat slot a downloaded filename belongs to, recognising Poly Haven's (and most PBR texture
// sets') own naming convention -- ported from the launcher's TexturePlacer.DetectSlot
// (Aver launcher\src\Aver.Launcher.Exchange\TexturePlacer.cs), EXTENDED to also recognise bare
// "_rough_"/"_metal_": TexturePlacer left those two unbound because nothing on that side of the
// process boundary could pack them into the engine's single combined metalRough slot. This tool can,
// since it already links Aver.Platform for decodeImage.
enum class MapRole { BaseColor, Normal, Occlusion, MetalRoughPacked, RoughnessOnly, MetalOnly, Unrecognised };

MapRole classifyMap(const std::string& fileName) {
    std::string n = fileName;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("_diff_") != std::string::npos || n.find("_diffuse_") != std::string::npos
        || n.find("_basecolor_") != std::string::npos) return MapRole::BaseColor;
    if (n.find("_nor_gl_") != std::string::npos || n.find("_normal_") != std::string::npos) return MapRole::Normal;
    if (n.find("_arm_") != std::string::npos) return MapRole::MetalRoughPacked;
    if (n.find("_ao_") != std::string::npos || n.find("_occlusion_") != std::string::npos) return MapRole::Occlusion;
    if (n.find("_rough_") != std::string::npos) return MapRole::RoughnessOnly;
    if (n.find("_metal_") != std::string::npos) return MapRole::MetalOnly;

    // ambientCG's own naming convention -- verified against two real downloaded asset ZIPs, not
    // guessed. A genuinely different shape from Poly Haven's underscore-PADDED style above: the
    // suffix sits PascalCase right before the extension ("Ground110_1K-JPG_Color.jpg"), not between
    // two underscores with a resolution token after it.
    if (n.find("_color.") != std::string::npos) return MapRole::BaseColor;
    if (n.find("_normalgl.") != std::string::npos) return MapRole::Normal;
    if (n.find("_roughness.") != std::string::npos) return MapRole::RoughnessOnly;
    if (n.find("_metalness.") != std::string::npos) return MapRole::MetalOnly;
    if (n.find("_ambientocclusion.") != std::string::npos) return MapRole::Occlusion;

    return MapRole::Unrecognised;
}

// Packs separate roughness and metallic maps into one texture the engine's metalRough slot can bind:
// metal in B, roughness in G, R unused -- matching what the shader actually samples
// (modules/render.pbr/src/PbrShaders.cpp reads mr.g as roughness and mr.b as metallic). Only reached
// when no combined map (Poly Haven's "arm") was already present in the set: a provider's own combined
// map is already correct, and re-deriving it would just be lossy work for nothing.
bool packMetalRough(const std::string& roughPath, const std::string& metalPath,
                    const std::string& outPath, std::string* err) {
    ImageData rough, metal;
    if (!decodeImage(roughPath, rough, err)) return false;
    if (!decodeImage(metalPath, metal, err)) return false;
    if (rough.width != metal.width || rough.height != metal.height) {
        if (err) *err = "roughness and metallic maps have different dimensions ("
            + std::to_string(rough.width) + "x" + std::to_string(rough.height) + " vs "
            + std::to_string(metal.width) + "x" + std::to_string(metal.height) + ")";
        return false;
    }
    const usize n = static_cast<usize>(rough.width) * rough.height;
    std::vector<u8> packed(n * 3);
    for (usize i = 0; i < n; ++i) {
        // Both source maps are greyscale, so stb's forced-RGBA decode already replicated the single
        // channel across R/G/B -- reading the R lane of each is reading its intensity.
        packed[i * 3 + 0] = 0;
        packed[i * 3 + 1] = rough.pixels[i * 4];
        packed[i * 3 + 2] = metal.pixels[i * 4];
    }
    if (!stbi_write_png(outPath.c_str(), static_cast<int>(rough.width), static_cast<int>(rough.height),
                        3, packed.data(), static_cast<int>(rough.width) * 3)) {
        if (err) *err = "failed to write the packed metalRough texture";
        return false;
    }
    return true;
}

#if AVER_HAVE_MATERIAL_GRAPH

// ---- the material .ocgraph ------------------------------------------------------------------------
//
// WHY A GENERATED GRAPH IS WORTH ANYTHING WHEN IT CHANGES NOTHING ON SCREEN. `.ocmat` has carried a
// GRAPHREF record naming a `DOMAIN material` .ocgraph since materials gained a node compiler, and
// until now NOTHING IN THE REPOSITORY WROTE ONE -- the reader, the HLSL compiler, the registry and
// the editor all existed for a file no tool produced. An imported material was therefore a dead end:
// you could shade with it, and you could not open it and change how it shades without hand-authoring
// a graph from nothing. This emits the graph that says what the material ALREADY does, which is the
// only useful place to start editing from.
//
// EXACT EQUIVALENCE IS THE WHOLE CONTRACT, and it is enforced rather than hoped for. The graph
// replaces averEvalMaterial wholesale: every pin it drives overwrites what averStockAuthored() put
// there, so a pin driven even slightly differently is a visible regression on every material this
// tool imports. Each pin below therefore reproduces material_prelude.hlsl's own arithmetic exactly --
//
//   BaseColor   gBaseColorFactor.rgb * map.baseColor      (averStockAuthored:525,528)
//   Roughness   gRoughnessFactor * map.metalRough.x, and metalRough is float2(mr.g, mr.b)
//               (averSampleMaps:302) -- so roughness is the GREEN channel, metallic the BLUE
//   Metallic    gMetallicFactor  * map.metalRough.y
//   Normal      map.normalTS = float3((tex.xyz*2-1).xy * gNormalScale, (tex.xyz*2-1).z)
//               (averSampleMaps:303-304) -- the *2-1 unpack is NOT optional
//   Occlusion   map.occlusion = tex.r  (averSampleMaps:305) -- the RED channel, a scalar
//
// -- and a pin whose per-material FACTOR is not 1 is LEFT UNDRIVEN rather than approximated. There is
// no graph node for gBaseColorFactor/gRoughnessFactor/gNormalScale, so a graph cannot reproduce a
// non-unit factor at all; skipping the pin leaves averStockAuthored's own value in place, which is
// exactly right, and `skipped` reports why so the omission is visible in the artifact record instead
// of being a silent difference. In practice this tool never sets those three, so the guard fires only
// if that changes -- which is the point of writing it as a check rather than a comment.
//
// EMISSIVE AND THE layer1* SLOTS ARE NEVER EMITTED because classifyMap never binds them; a graph
// node for a slot with no texture behind it would sample a default and write it over the stock value.

constexpr f64 kColSample = 0.0, kColConvert = 320.0, kColConvert2 = 520.0, kColOut = 760.0;
constexpr f64 kRowStep = 150.0;

// Appends a node. Takes its extras and pins BY VALUE rather than handing back a reference for the
// caller to fill in: `g.nodes.back()` is invalidated by the next push_back, and a builder that
// returned one would work until the day a node was added after it.
void addNode(fmt::OcGraphData& g, std::string id, std::string type, f64 x, f64 y,
             std::vector<std::string> extras = {}, std::vector<fmt::OcGraphPin> pins = {}) {
    fmt::OcGraphNode n;
    n.id = std::move(id);
    n.type = std::move(type);
    n.x = x;
    n.y = y;
    n.extraTokens = std::move(extras);
    n.pins = std::move(pins);
    g.nodes.push_back(std::move(n));
}

void addLink(fmt::OcGraphData& g, std::string sn, std::string sp, std::string dn, std::string dp) {
    g.links.push_back(fmt::OcGraphLink{std::move(sn), std::move(sp), std::move(dn), std::move(dp)});
}

bool isUnitFactor(f32 v) { return v >= 1.0f - 1e-6f && v <= 1.0f + 1e-6f; }

// AN OUTPUT PIN'S DECLARED TYPE IS LOAD-BEARING, and omitting it is not a cosmetic slip -- it
// silently changes what the graph computes. Emitter::outType reads the node's DECLARED output pin
// (`findPin(n, pin, true)`) and falls back to MatType::Float for a node that declares none; every
// generic operator then sizes itself with widestInput() off that answer. Two things follow, and this
// generator hit both before the pins below existed:
//
//   * Swizzle refused mask=z with "reads a component a float does not have" -- a LOUD failure, and
//     the only reason it was loud is that the mask is validated against the arity.
//   * The normal chain's Multiply sized itself to float, and widen() NARROWS silently by appending
//     .x (MaterialGraphHlsl.cpp:241-244) -- so `tex*2-1` would have quietly become
//     `float3(tex.x*2-1)`, a wrong normal on every imported material with no error anywhere.
//
// So every node this generator emits declares the output pin a downstream node reads, with its real
// width. The declaration is also what the node editor draws the pin from, so it is not duplicated
// bookkeeping -- it is the one place the width is stated.
fmt::OcGraphPin outPin(const char* name, const char* type) {
    return fmt::OcGraphPin{name, type, true, {}};
}
fmt::OcGraphPin inPinDefault(const char* name, const char* type, const char* value) {
    return fmt::OcGraphPin{name, type, false, value};
}

// The graph that reproduces `desc`'s stock shading. `skipped` collects a human-readable reason for
// every pin deliberately left undriven, so the caller can report them rather than lose them.
fmt::OcGraphData buildMaterialGraph(const pbr::MaterialDesc& desc, const std::string& name,
                                    std::vector<std::string>& skipped) {
    fmt::OcGraphData g;
    g.name = name;
    g.domain = "material";

    const auto bound = [&](pbr::TextureSlot s) {
        return !desc.textures[static_cast<u32>(s)].empty();
    };
    f64 row = 0.0;
    const auto nextRow = [&]() { const f64 y = row; row += kRowStep; return y; };

    // The one sink. Its INPUT PIN NAMES are the AverAuthored fields (MaterialGraphHlsl's
    // kOutputFields), which is why there is no "BaseColor node" -- there is a BaseColor PIN.
    addNode(g, "surface", "MaterialOutput", kColOut, 0.0);

    if (bound(pbr::TextureSlot::BaseColor)) {
        if (isUnitFactor(desc.baseColorFactor[0]) && isUnitFactor(desc.baseColorFactor[1])
            && isUnitFactor(desc.baseColorFactor[2])) {
            const f64 y = nextRow();
            addNode(g, "texBaseColor", "SampleTexture", kColSample, y, {"slot=basecolor"},
                    {outPin("rgb", "float3")});
            addLink(g, "texBaseColor", "rgb", "surface", "BaseColor");
        } else {
            skipped.push_back("BaseColor: baseColorFactor is not white and no node can express it");
        }
    }

    if (bound(pbr::TextureSlot::MetalRough)) {
        const f64 y = nextRow();
        addNode(g, "texMetalRough", "SampleTexture", kColSample, y, {"slot=metalrough"},
                {outPin("rgb", "float3")});
        // Roughness is .g and metallic is .b -- see averSampleMaps' own float2(mr.g, mr.b).
        if (isUnitFactor(desc.roughnessFactor)) {
            addNode(g, "roughness", "Swizzle", kColConvert, y, {"mask=y"},
                    {outPin("result", "float")});
            addLink(g, "texMetalRough", "rgb", "roughness", "x");
            addLink(g, "roughness", "result", "surface", "Roughness");
        } else {
            skipped.push_back("Roughness: roughnessFactor is not 1 and no node can express it");
        }
        if (isUnitFactor(desc.metallicFactor)) {
            addNode(g, "metallic", "Swizzle", kColConvert, y + kRowStep * 0.5, {"mask=z"},
                    {outPin("result", "float")});
            addLink(g, "texMetalRough", "rgb", "metallic", "x");
            addLink(g, "metallic", "result", "surface", "Metallic");
        } else {
            // Reached whenever runMaterial forced metallicFactor to 0 for a set with no metal map --
            // in which case there is no MetalRough texture either and this branch cannot run. Kept
            // because the guard belongs to the factor, not to that one caller's habits.
            skipped.push_back("Metallic: metallicFactor is not 1 and no node can express it");
        }
        row += kRowStep * 0.5;
    }

    if (bound(pbr::TextureSlot::Normal)) {
        if (isUnitFactor(desc.normalScale)) {
            const f64 y = nextRow();
            addNode(g, "texNormal", "SampleTexture", kColSample, y, {"slot=normal"},
                    {outPin("rgb", "float3")});
            // THE *2-1 UNPACK, in two nodes because there is no unpack node. Both scalars ride on an
            // input pin's default, which parseComponents BROADCASTS to the width of the other input
            // (float3 here) -- a single component with more wanted is replicated, not zero-padded.
            addNode(g, "normalScaled", "Multiply", kColConvert, y, {},
                    {inPinDefault("b", "float", "2"), outPin("result", "float3")});
            addNode(g, "normalUnpacked", "Subtract", kColConvert2, y, {},
                    {inPinDefault("b", "float", "1"), outPin("result", "float3")});
            addLink(g, "texNormal", "rgb", "normalScaled", "a");
            addLink(g, "normalScaled", "result", "normalUnpacked", "a");
            addLink(g, "normalUnpacked", "result", "surface", "Normal");
        } else {
            skipped.push_back("Normal: normalScale is not 1 and no node can express it");
        }
    }

    if (bound(pbr::TextureSlot::Occlusion)) {
        // NO FACTOR GUARD, and that is not an oversight: occlusionStrength is applied by
        // averBuildSurface AFTER the authored struct, not inside averStockAuthored, so it is not a
        // term this pin is responsible for reproducing.
        const f64 y = nextRow();
        addNode(g, "texOcclusion", "SampleTexture", kColSample, y, {"slot=occlusion"},
                {outPin("rgb", "float3")});
        addNode(g, "occlusionR", "Swizzle", kColConvert, y, {"mask=x"},
                {outPin("result", "float")});
        addLink(g, "texOcclusion", "rgb", "occlusionR", "x");
        addLink(g, "occlusionR", "result", "surface", "Occlusion");
    }

    return g;
}

// Writes the graph, reloads it, and COMPILES it -- the same write-then-reload discipline the .ocmat
// and .ocmesh writers use, with one step more.
//
// PARSING IS NOT ENOUGH HERE. A graph that round-trips is a graph that is well-formed TEXT; the thing
// that can actually go wrong is a node type, slot name, swizzle mask or pin arity this build's
// emitter rejects, and none of those are syntax. compileMaterialGraph is the check that matches the
// claim being made, and it needs no device (MaterialGraphTest links exactly these libraries and
// creates none), so there is no reason to make the weaker check instead.
//
// Returns the path to record as GRAPHREF, or an empty string when the graph could not be written or
// would not compile -- in which case the caller writes NO GraphRef and the material shades through
// the stock path exactly as it did before this tool emitted graphs at all.
std::string writeAndVerifyMaterialGraph(const std::string& input, const std::string& path,
                                        const pbr::MaterialDesc& desc, const std::string& name,
                                        RunStats& stats) {
    std::vector<std::string> skipped;
    const fmt::OcGraphData g = buildMaterialGraph(desc, name, skipped);

    // A graph with no driven pin describes nothing; writing one would mean a GRAPHREF whose graph
    // overrides no field, which is a file to maintain for no effect.
    if (g.links.empty()) {
        AVER_WARN("no material pin could be driven exactly; no .ocgraph written");
        return {};
    }

    std::string why;
    if (!fmt::saveOcgraph(path, g, &why)) {
        emitArtifact(input, path, "materialgraph", false, false, why, stats);
        return {};
    }
    fmt::OcGraphData back;
    if (!fmt::loadOcgraph(path, back, &why)) {
        emitArtifact(input, path, "materialgraph", true, false, "reload failed: " + why, stats);
        return {};
    }
    const pbr::MaterialGraphBody body = pbr::compileMaterialGraph(back);
    if (!body.ok) {
        emitArtifact(input, path, "materialgraph", true, false,
                     "the generated graph does not compile: " + body.error, stats);
        return {};
    }

    std::string extra = "\"drivenPins\":" + std::to_string(g.links.size());
    if (!skipped.empty()) {
        extra += ",\"skippedPins\":" + std::to_string(skipped.size());
        for (const std::string& s : skipped) AVER_WARN("material graph: {}", s);
    }
    emitArtifact(input, path, "materialgraph", true, true, {}, stats, extra);
    return path;
}

#endif // AVER_HAVE_MATERIAL_GRAPH

// Writes one .ocmat, reloads it, and confirms every texture slot the caller bound survived the round
// trip -- the same write-then-reload discipline writeAndVerifyMesh uses for .ocmesh.
//
// `graphRef` is the GRAPHREF record: the .ocgraph that shades this material, or empty for none. It is
// a PARAMETER rather than something this function derives because the graph has to be written and
// PROVEN TO COMPILE before the .ocmat may point at it -- a GRAPHREF naming a graph that does not
// compile is a material that fails to shade at all, so the ordering is load-bearing, not incidental.
bool writeAndVerifyMaterial(const std::string& input, const std::string& path,
                            const pbr::MaterialDesc& desc, const std::string& graphRef,
                            RunStats& stats) {
    fmt::OcMatExtras extras;   // default-constructed: standard/opaque/back -- a bare texture-set
                               // material gets no shader/blend/cull override.
    extras.graphRef = graphRef;
    std::string why;
    if (!fmt::saveOcmat(path, desc, &extras, &why)) {
        emitArtifact(input, path, "material", false, false, why, stats);
        return false;
    }
    pbr::MaterialDesc back;
    fmt::OcMatExtras backExtras;
    if (!fmt::loadOcmat(path, back, &backExtras, &why)) {
        emitArtifact(input, path, "material", true, false, "reload failed: " + why, stats);
        return false;
    }
    int boundSlots = 0;
    for (u32 s = 0; s < pbr::kTextureSlotCount; ++s) {
        const bool wasBound = !desc.textures[s].empty();
        if (wasBound) ++boundSlots;
        if (wasBound != !back.textures[s].empty()) {
            emitArtifact(input, path, "material", true, false,
                         "a bound texture slot did not survive the round trip", stats);
            return false;
        }
    }
    // VERIFIED LIKE ANY OTHER FIELD, because a GRAPHREF that silently failed to write is the one
    // failure mode that looks exactly like success: the .ocmat loads, the material renders, and it
    // renders through the stock path rather than the graph the caller was told it got.
    if (backExtras.graphRef != graphRef) {
        emitArtifact(input, path, "material", true, false,
                     "the graph reference did not survive the round trip", stats);
        return false;
    }
    std::string extra = "\"boundSlots\":" + std::to_string(boundSlots);
    if (!graphRef.empty()) extra += ",\"graphRef\":\"" + jsonEscape(graphRef) + "\"";
    emitArtifact(input, path, "material", true, true, {}, stats, extra);
    return true;
}

// Places every texture in the set, packs roughness+metal when the set needs it, and writes the
// resulting .ocmat -- everything downstream of argument parsing for the `material` subcommand.
int runMaterial(int argc, char** argv) {
    std::vector<std::string> inputs;
    std::string outDir, baseOverride;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out-dir" && i + 1 < argc)   outDir = argv[++i];
        else if (a == "--base" && i + 1 < argc) baseOverride = argv[++i];
        else                                    inputs.push_back(a);
    }
    if (inputs.empty() || outDir.empty()) {
        AVER_ERROR("usage: AverAssetC material <texture-file>... --out-dir <dir> --base <name>");
        return exitCode(ExitCode::Usage);
    }
    while (!outDir.empty() && (outDir.back() == '\\' || outDir.back() == '/')) outDir.pop_back();
    const std::string base = !baseOverride.empty() ? baseOverride : stemOf(inputs[0]);
    std::string allInputs;
    for (usize i = 0; i < inputs.size(); ++i) { if (i) allInputs += ";"; allInputs += inputs[i]; }

    RunStats stats;
    pbr::MaterialDesc desc;
    desc.name = base;
    bool anyFailed = false;
    std::string roughPath, metalPath;   // set aside until we know whether a combined map also showed up

    for (const std::string& in : inputs) {
        const std::string fname = filenameOf(in);
        const std::string dest = outDir + "/" + fname;
        std::error_code ec;
        std::filesystem::copy_file(in, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            emitArtifact(in, dest, "material", false, false, ec.message(), stats);
            anyFailed = true;
            continue;
        }

        const MapRole role = classifyMap(fname);
        const std::string texRef = kContentRelPrefix + fname;
        auto bind = [&](pbr::TextureSlot slot, const char* slotName) {
            desc.textures[static_cast<u32>(slot)] = pbr::TextureRef{texRef, 0};
            emitArtifact(in, dest, "material", true, true, {}, stats,
                         std::string("\"slot\":\"") + slotName + "\"");
        };
        switch (role) {
            case MapRole::BaseColor: bind(pbr::TextureSlot::BaseColor, "baseColor"); break;
            case MapRole::Normal:    bind(pbr::TextureSlot::Normal, "normal"); break;
            case MapRole::Occlusion: bind(pbr::TextureSlot::Occlusion, "occlusion"); break;
            case MapRole::MetalRoughPacked: bind(pbr::TextureSlot::MetalRough, "metalRough"); break;
            case MapRole::RoughnessOnly:
                roughPath = dest;
                emitArtifact(in, dest, "material", true, true, {}, stats, "\"slot\":\"pending\"");
                break;
            case MapRole::MetalOnly:
                metalPath = dest;
                emitArtifact(in, dest, "material", true, true, {}, stats, "\"slot\":\"pending\"");
                break;
            case MapRole::Unrecognised:
                emitArtifact(in, dest, "material", true, true, {}, stats, "\"slot\":\"none\"");
                break;
        }
    }

    if (!roughPath.empty() && !metalPath.empty()
        && desc.textures[static_cast<u32>(pbr::TextureSlot::MetalRough)].empty()) {
        const std::string packedPath = outDir + "/" + base + "_metalRough.png";
        const std::string packInput = roughPath + ";" + metalPath;
        std::string why;
        if (!packMetalRough(roughPath, metalPath, packedPath, &why)) {
            emitArtifact(packInput, packedPath, "material", false, false, why, stats);
            anyFailed = true;
        } else {
            ImageData back;
            if (!decodeImage(packedPath, back, &why)) {
                emitArtifact(packInput, packedPath, "material", true, false, "reload failed: " + why, stats);
                anyFailed = true;
            } else {
                desc.textures[static_cast<u32>(pbr::TextureSlot::MetalRough)] =
                    pbr::TextureRef{kContentRelPrefix + base + "_metalRough.png", 0};
                emitArtifact(packInput, packedPath, "material", true, true, {}, stats,
                             "\"slot\":\"metalRough\",\"packedFrom\":\"roughness+metal\"");
            }
        }
    }

    bool anyBound = false;
    for (const pbr::TextureRef& t : desc.textures) if (!t.empty()) { anyBound = true; break; }
    if (anyBound) {
        // pbr::MaterialDesc::metallicFactor defaults to 1.0 (glTF's own spec default) -- correct
        // when a metalRough texture IS bound (the factor multiplies the sampled value), wrong when
        // one is not: a texture set with only a Roughness map and no Metal/arm map at all (a real,
        // common case -- ambientCG's own Ground110 set has no metal content and ships no Metalness
        // map for it) would otherwise render fully metallic with nothing overriding that default.
        if (desc.textures[static_cast<u32>(pbr::TextureSlot::MetalRough)].empty()) {
            desc.metallicFactor = 0.0f;
        }
        // THE GRAPH IS WRITTEN FIRST, AND THE .ocmat POINTS AT IT ONLY IF IT COMPILED. A GRAPHREF
        // naming a graph that does not compile is worse than no graph at all -- the material stops
        // shading rather than falling back -- so the ordering here is the safety property, not a
        // convenience. An empty graphRef is the exact behaviour this tool had before graphs existed.
        std::string graphRef;
#if AVER_HAVE_MATERIAL_GRAPH
        const std::string graphPath = outDir + "/" + base + ".ocgraph";
        if (!writeAndVerifyMaterialGraph(allInputs, graphPath, desc, base, stats).empty()) {
            // CONTENT-RELATIVE, through the SAME prefix the texture references use -- OcMat.hpp's
            // GRAPHREF comment is explicit that the path never carries the content directory on the
            // front, and the graph is written beside the .ocmat in --out-dir, so it is exactly as
            // deep in the project as the textures are. See kContentRelPrefix for why that is an
            // assumption rather than something this tool can look up.
            graphRef = kContentRelPrefix + base + ".ocgraph";
        }
#endif
        const std::string matPath = outDir + "/" + base + ".ocmat";
        if (!writeAndVerifyMaterial(allInputs, matPath, desc, graphRef, stats)) anyFailed = true;
    } else {
        AVER_WARN("no texture in this set matched a recognised material slot; nothing to bind, no .ocmat written");
    }

    emitSummary(allInputs, stats, anyFailed ? 1 : 0);
    return anyFailed ? 1 : 0;
}

#endif // AVER_HAVE_MATERIAL_COMPILE

// ---- optional decimation + clustering, ported from ConvertTool.cpp unchanged ----

#if AVER_MODULE_TRIFACTOR
bool addMeshlets(fmt::OcMeshData& m, std::string* why) {
    aver::trifactor::LodDag dag;
    if (!aver::trifactor::buildClusters(m, dag, why)) return false;
    std::string hierWhy;
    if (!aver::trifactor::buildLodHierarchy(m, dag, &hierWhy))
        AVER_WARN("buildLodHierarchy: {} (saving LOD 0 only)", hierWhy);
    return aver::trifactor::packLodDag(dag, m, why);
}

// Best-effort, exactly like ConvertTool: a mesh too small/degenerate to cluster still gets saved,
// just without an MLET chunk.
void applyLodAndClustering(fmt::OcMeshData& m, f32 lodRatio, const std::string& label) {
    std::string why;
    if (lodRatio > 0.0f) {
        const usize before = m.indices.size() / 3;
        if (!aver::trifactor::simplifyMesh(m, lodRatio, &why)) {
            AVER_WARN("--lod {}: {} (saving '{}' at full density)", lodRatio, why, label);
        } else {
            AVER_INFO("simplified '{}' to {:.1f}%: {} -> {} tris", label, double(lodRatio) * 100.0,
                      before, m.indices.size() / 3);
        }
    }
    if (!addMeshlets(m, &why)) {
        AVER_WARN("clustering '{}': {} (saving without meshlets)", label, why);
    } else {
        AVER_INFO("clustered '{}': {} LOD(s), {} meshlets at LOD 0", label, m.lodCount(), m.meshlets.size());
    }
}
#endif

// ---- writing and verifying one mesh, ported from ConvertTool.cpp unchanged ----

// Writes one .ocmesh, reloads it, and confirms the skin (the one stream that used to be dropped
// silently) survived the round trip. Reports the outcome as one JSON line; never throws.
bool writeAndVerifyMesh(const std::string& input, const std::string& path, const fmt::OcMeshData& m,
                        RunStats& stats) {
    std::string why;
    if (!fmt::saveOcMesh(path, m, &why)) {
        emitArtifact(input, path, "mesh", false, false, why, stats);
        return false;
    }
    fmt::OcMeshData back;
    if (!fmt::loadOcMesh(path, back, &why)) {
        emitArtifact(input, path, "mesh", true, false, "reload failed: " + why, stats);
        return false;
    }
    if (m.hasSkin() && !back.hasSkin()) {
        emitArtifact(input, path, "mesh", true, false, "the skin did not survive the round trip", stats);
        return false;
    }
    std::string extra = "\"vertices\":" + std::to_string(back.vertexCount())
        + ",\"triangles\":" + std::to_string(back.indices.size() / 3)
        + ",\"hasSkin\":" + (back.hasSkin() ? "true" : "false");
    emitArtifact(input, path, "mesh", true, true, {}, stats, extra);
    return true;
}

// One imported mesh plus the skin identity it needs to merge safely. `skinIndex` mirrors
// GltfImportResult::meshSkinIndex: -1 when unskinned, otherwise an index two items share only when
// they are provably bound to the same skeleton (see GltfImport.cpp). OBJ and USD never populate a
// skin, so their items are always -1, which makes the merge below unconditionally safe for them.
struct MeshItem {
    std::string name;
    fmt::OcMeshData data;
    i32 skinIndex = -1;
};

// Merges every item into the first, following ConvertTool.cpp's exact rule: two items resolving to
// the SAME skin index share one bone-index space and merge their joints/weights too; a different (or
// one-sided) skin would remap joints wrongly, so the merge stops there instead, keeping only what
// was already merged. See ConvertTool.cpp for the full reasoning -- this is that algorithm, lifted
// out so both tools call the same logic rather than risk two copies drifting apart.
fmt::OcMeshData mergeAll(const std::vector<MeshItem>& items, usize& mergedCount, std::string* warn) {
    fmt::OcMeshData m = items[0].data;
    mergedCount = 1;
    const i32 accSkin = items[0].skinIndex;
    for (usize mi = 1; mi < items.size(); ++mi) {
        const fmt::OcMeshData& src = items[mi].data;
        if (src.positions.empty() || src.indices.empty()) continue;

        const i32 srcSkin = items[mi].skinIndex;
        const bool sameSkeleton = src.hasSkin() && m.hasSkin() && accSkin >= 0 && srcSkin == accSkin;
        if ((src.hasSkin() || m.hasSkin()) && !sameSkeleton) {
            if (warn) {
                *warn = "'" + items[mi].name + "' is skinned by a different skeleton than the "
                    + std::to_string(mergedCount) + " mesh(es) merged so far; only "
                    + std::to_string(mergedCount) + " of " + std::to_string(items.size())
                    + " meshes were merged";
            }
            break;
        }

        const u32 base = m.vertexCount();
        const u32 firstIndex = static_cast<u32>(m.indices.size());
        m.positions.insert(m.positions.end(), src.positions.begin(), src.positions.end());
        m.normals.insert(m.normals.end(), src.normals.begin(), src.normals.end());
        m.uvs.insert(m.uvs.end(), src.uvs.begin(), src.uvs.end());
        if (sameSkeleton) {
            m.joints.insert(m.joints.end(), src.joints.begin(), src.joints.end());
            m.weights.insert(m.weights.end(), src.weights.begin(), src.weights.end());
        }
        for (const u32 idx : src.indices) m.indices.push_back(idx + base);

        const u32 slotBase = static_cast<u32>(m.materialSlots.size());
        m.materialSlots.insert(m.materialSlots.end(), src.materialSlots.begin(), src.materialSlots.end());
        if (src.submeshes.empty()) {
            m.submeshes.push_back(fmt::OcMeshSubmesh{items[mi].name, slotBase, firstIndex,
                                                     static_cast<u32>(src.indices.size()), base,
                                                     src.vertexCount()});
        } else {
            for (fmt::OcMeshSubmesh sm : src.submeshes) {
                sm.materialSlot += slotBase;
                sm.indexStart   += firstIndex;
                sm.baseVertex   += base;
                m.submeshes.push_back(std::move(sm));
            }
        }

        m.boundsMin = Vec3{std::min(m.boundsMin.x, src.boundsMin.x), std::min(m.boundsMin.y, src.boundsMin.y),
                           std::min(m.boundsMin.z, src.boundsMin.z)};
        m.boundsMax = Vec3{std::max(m.boundsMax.x, src.boundsMax.x), std::max(m.boundsMax.y, src.boundsMax.y),
                           std::max(m.boundsMax.z, src.boundsMax.z)};
        ++mergedCount;
    }
    return m;
}

// Writes `items` as one merged .ocmesh (--merge) or one .ocmesh per item (the default), naming each
// output after its own mesh name -- sanitised, and de-duplicated within this run so two same-named
// source meshes cannot overwrite one another in the staging directory. Shared by all three mesh
// formats; glTF calls this and then separately writes its skeleton/clips, since OBJ and USD have
// neither.
// `outStems`, when given, receives the stem each item was written under, PARALLEL TO `items` --
// empty for an item that was merged away. The scene writer needs it because the stem is decided
// here, by safe()/dedup, and re-deriving it there would be a second implementation of a naming rule
// that only has to disagree once to write a level full of dangling mesh paths.

// Writes a level that puts the imported meshes back where the source scene had them.
//
// WHY THIS EXISTS. The importer used to bake a glTF node's translation into its vertices, so a
// 115-piece scene imported as 115 meshes each carrying its own world position in its geometry: the
// pieces lined up if you placed them all at the origin, and every one of them had its pivot metres
// away from itself. Taking the translation out fixes the pivot but throws the scene away unless
// somebody writes it down. This is somebody writing it down.
//
// CONTENT-RELATIVE, resolved the way the editor resolves it (contentDir + "\\" + ref), which is why
// this needs --content-dir. Without one there is no way to express the mesh path so a level can find
// it, and the level is skipped with a line saying so rather than written full of paths that resolve
// to nothing.
bool writeSceneLevel(const std::string& outDir, const std::string& contentDir,
                     const std::string& base,
                     const std::vector<fmt::GltfPlacement>& placements,
                     const std::vector<std::string>& stems) {
    if (placements.empty()) return true;                 // a single-object file needs no level
    if (contentDir.empty()) {
        AVER_WARN("{} placement(s) were recovered from the scene graph, but --content-dir was not "
                  "given, so a mesh path cannot be made content-relative and no level was written. "
                  "The meshes are correct and centred; place them by hand, or re-run with "
                  "--content-dir.", placements.size());
        return true;
    }

    std::error_code ec;
    std::filesystem::path rel =
        std::filesystem::relative(std::filesystem::path(outDir), std::filesystem::path(contentDir), ec);
    if (ec || rel.empty() || rel.generic_string().rfind("..", 0) == 0) {
        AVER_WARN("--out-dir is not inside --content-dir, so mesh paths cannot be made "
                  "content-relative; no level was written");
        return true;
    }
    std::string prefix = rel.generic_string();
    if (prefix == ".") prefix.clear(); else prefix += "/";

    fmt::OcWorldData w;
    w.name = base;
    for (const fmt::GltfPlacement& p : placements) {
        if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size()) continue;
        const std::string& stem = stems[usize(p.meshIndex)];
        if (stem.empty()) continue;                      // merged away, or failed to write
        fmt::OcWorldPlacement op;
        op.asset = prefix + stem + ".ocmesh";
        op.x = p.position.x; op.y = p.position.y; op.z = p.position.z;
        // Rotation and scale stay baked in the geometry, so the placement is a pure translation.
        w.placements.push_back(std::move(op));
    }
    if (w.placements.empty()) return true;

    // Into <content>/Maps, where the editor's level list looks, rather than beside the meshes: a
    // .ocworld sitting in a Meshes folder is findable by nothing.
    const std::string mapsDir = contentDir + "/Maps";
    createDirectories(mapsDir);
    // NEVER OVERWRITE A LEVEL THAT IS ALREADY THERE. The level is named after the SOURCE FILE, so
    // importing a "JungleRuins.gltf" into a project that already has a JungleRuins.ocworld would
    // replace a level somebody has been building with a bare list of freshly imported meshes. An
    // import is not a thing anyone expects to destroy their work, and this one runs unattended from
    // the launcher. Suffixing is the boring, recoverable answer.
    std::string path = mapsDir + "/" + base + ".ocworld";
    if (fileExists(path)) {
        int n = 2;
        std::string alt;
        do { alt = mapsDir + "/" + base + "_" + std::to_string(n++) + ".ocworld"; }
        while (fileExists(alt) && n < 1000);
        AVER_WARN("{} already exists and was NOT touched; the imported scene went to {} instead",
                  path, alt);
        path = alt;
    }
    std::string why;
    if (!fmt::saveOcworld(path, w, &why)) {
        AVER_WARN("could not write the scene level {}: {}", path, why);
        return false;
    }
    AVER_INFO("wrote {} with {} placement(s) -- open it to see the source scene reassembled",
              path, w.placements.size());
    return true;
}

bool writeMeshItems(const std::string& input, const std::string& outDir, const std::string& base,
                    const std::vector<MeshItem>& items, bool merge, f32 lodRatio, RunStats& stats,
                    std::vector<std::string>* outStems = nullptr) {
    bool anyFailed = false;
    if (outStems) outStems->assign(items.size(), std::string{});
    if (merge && items.size() > 1) {
        usize mergedCount = 0;
        std::string warn;
        fmt::OcMeshData m = mergeAll(items, mergedCount, &warn);
        if (!warn.empty()) AVER_WARN("{}", warn);
#if AVER_MODULE_TRIFACTOR
        applyLodAndClustering(m, lodRatio, base);
#endif
        if (!writeAndVerifyMesh(input, outDir + "/" + base + ".ocmesh", m, stats)) anyFailed = true;
        // Merged: one file, so there is no per-item stem to report and a scene cannot be rebuilt
        // from it. Left empty rather than pointing every placement at the merged mesh, which would
        // stamp the whole model once per placement.
        return anyFailed;
    }

    std::vector<std::string> used;
    for (usize i = 0; i < items.size(); ++i) {
        fmt::OcMeshData m = items[i].data;
#if AVER_MODULE_TRIFACTOR
        applyLodAndClustering(m, lodRatio, items[i].name.empty() ? base : items[i].name);
#else
        (void)lodRatio;
#endif
        std::string stem = items.size() == 1 ? base : safe(items[i].name, base + std::to_string(i));
        std::string candidate = stem;
        int suffix = 1;
        while (std::find(used.begin(), used.end(), candidate) != used.end())
            candidate = stem + "_" + std::to_string(++suffix);
        used.push_back(candidate);
        if (!writeAndVerifyMesh(input, outDir + "/" + candidate + ".ocmesh", m, stats)) anyFailed = true;
        else if (outStems) (*outStems)[i] = candidate;
    }
    return anyFailed;
}


// Thin wrapper around fmt::cookImportedMaterials (aver/formats/ImportCook.hpp), which now owns the
// cook itself: opacity folding, the texture-size cap, the call into fmt::cookMaterials, and the
// mesh material-slot rewrite. MOVED there because the editor's own import needs the identical policy
// and could not reach into this tool's .cpp for it. What stays here is what is genuinely this TOOL's:
// the geometry-only fallback when no --content-dir was given, and turning the shared function's
// out-params back into this tool's own AVER_INFO/AVER_WARN lines so its console output is unchanged.
//
// A no-op when `contentDir` is empty, which is the launcher-less case: geometry still imports, and
// the caller is told what it is leaving behind rather than losing it silently.
void cookAndRewriteSlots(std::vector<fmt::ImportedMaterial>& materials,
                         std::vector<fmt::ImportedImage>& images,
                         const std::string& contentDir, const std::string& base,
                         std::vector<fmt::OcMeshData>& meshes, u32 maxTexture) {
    if (materials.empty() && images.empty()) return;
    // Only the READABLE images are worth counting at the user: an entry whose bytes could not be
    // loaded is carried so a second material naming the same missing file is not retried, and
    // reporting it here would name a texture the cook is never going to write either way.
    usize readable = 0;
    for (const fmt::ImportedImage& img : images) if (img.ok) ++readable;
#if AVER_HAVE_MATERIAL_COMPILE
    if (contentDir.empty()) {
        AVER_WARN("this file has {} material(s) and {} image(s); pass --content-dir <dir> to "
                  "write them, otherwise only geometry is imported",
                  materials.size(), readable);
        return;
    }
    std::vector<std::string> cwarn;
    std::string cerr;
    u32 materialsWritten = 0, texturesWritten = 0;
    // overwriteExisting = true: a stated output directory is a directive here -- this tool is
    // invoked per asset by the launcher, and a re-import that silently kept the old material would
    // be a worse surprise than one that replaced it. Exactly this tool's previous, hardcoded choice.
    if (!fmt::cookImportedMaterials(materials, images, contentDir, base, meshes, maxTexture,
                                    /*overwriteExisting=*/true, &cwarn, &cerr,
                                    &materialsWritten, &texturesWritten)) {
        AVER_WARN("materials: {}", cerr);
        return;
    }
    for (const std::string& w : cwarn) AVER_WARN("materials: {}", w);
    AVER_INFO("wrote {} material(s) and {} texture(s) under {}",
              materialsWritten, texturesWritten, contentDir);
#else
    (void)contentDir; (void)base; (void)meshes; (void)maxTexture;
    AVER_WARN("this build has no PBR module, so the file's {} material(s) and {} image(s) were not "
              "imported; geometry only", materials.size(), readable);
#endif
}

} // namespace

// `convert` converts argv[2] (a glTF/GLB, OBJ, USDA, or -- when this build has audio import -- a
// WAV/MP3/M4A/FLAC) into argv following --out-dir, naming outputs after --base or the source stem.
// `material` (when this build has material compiling) converts a whole texture SET the same way --
// see runMaterial's own comment for why it is a separate subcommand rather than another `convert`
// extension. Every result, and a final summary, are reported as JSON Lines on stdout; AVER_* log
// lines (always '['-prefixed) may be interleaved and are for human troubleshooting only. Exit 0:
// everything declared verified. Exit 1: an import or verify failure. Exit 2: bad usage.
int main(int argc, char** argv) {
    static const char* kUsage =
        "usage: AverAssetC convert <input-file> --out-dir <dir> [--base <name>] [--merge] [--lod <ratio>]"
#if AVER_HAVE_MATERIAL_COMPILE
        "\n                          [--content-dir <dir>]   write materials and textures too"
        "\n                          [--max-texture <n>]     downscale imported textures to n px"
#endif
#if AVER_HAVE_MATERIAL_COMPILE
        "\n       AverAssetC material <texture-file>... --out-dir <dir> --base <name>"
#endif
        ;

    if (argc < 2) {
        AVER_ERROR("{}", kUsage);
        return exitCode(ExitCode::Usage);
    }
    const std::string subcommand = argv[1];

#if AVER_HAVE_MATERIAL_COMPILE
    if (subcommand == "material") return runMaterial(argc, argv);
#endif

    if (subcommand != "convert") {
        AVER_ERROR("{}", kUsage);
        return exitCode(ExitCode::Usage);
    }

    std::string input, outDir, baseOverride, contentDir;
    u32 maxTexture = 0;             // 0 = every texture through at its source resolution
    bool merge = false;
    bool haveInput = false;
    f32 lodRatio = 0.0f;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out-dir" && i + 1 < argc)      outDir = argv[++i];
        else if (a == "--base" && i + 1 < argc)    baseOverride = argv[++i];
        else if (a == "--merge")                   merge = true;
        else if (a == "--lod" && i + 1 < argc)      lodRatio = static_cast<f32>(std::atof(argv[++i]));
        // A SEPARATE FLAG FROM --out-dir, deliberately. The out-directory is where meshes go and is
        // routinely a scratch path; the content root is where the engine looks for Materials/ and
        // Textures/. Inferring one from the other would be a second fragile convention beside the
        // one kContentRelPrefix already admits is an assumption.
        else if (a == "--content-dir" && i + 1 < argc) contentDir = argv[++i];
        // --max-texture <n>: no imported texture wider or taller than n pixels. A 4K set costs 87 MB
        // of mips PER TEXTURE once the engine builds the chain, so three maps on each of a dozen
        // plants is a gigabyte of VRAM before anything else is in the scene.
        else if (a == "--max-texture" && i + 1 < argc) maxTexture = u32(std::atoi(argv[++i]));
        else if (!haveInput) { input = a; haveInput = true; }
    }
    if (!haveInput || outDir.empty()) {
        AVER_ERROR("{}", kUsage);
        return exitCode(ExitCode::Usage);
    }
    while (!outDir.empty() && (outDir.back() == '\\' || outDir.back() == '/')) outDir.pop_back();
    const std::string base = !baseOverride.empty() ? baseOverride : stemOf(input);

    RunStats stats;
    const std::string ext = extOf(input);

    if (ext == ".gltf" || ext == ".glb") {
        fmt::GltfImportResult res;
        std::string why;
        if (!fmt::importGltf(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }

        // ---- materials and their textures, BEFORE the meshes are copied into `items` ----
        //
        // THIS TOOL, NOT JUST THE HARNESS. The cook shipped first in tests/formats ConvertTool,
        // which is the dev harness; this is the compiler the launcher actually invokes, so until
        // now a glTF imported by the product arrived with its materials thrown away while the same
        // file imported by the harness did not.
        cookAndRewriteSlots(res.materials, res.images, contentDir, base, res.meshes, maxTexture);

        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            items.push_back(MeshItem{
                i < res.meshNames.size() ? res.meshNames[i] : std::string{},
                res.meshes[i],
                i < res.meshSkinIndex.size() ? res.meshSkinIndex[i] : -1});
        }

        std::vector<std::string> stems;
        bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats, &stems);
        if (!writeSceneLevel(outDir, contentDir, base, res.placements, stems)) anyFailed = true;

        bool anySkinned = false;
        for (const fmt::OcMeshData& mesh : res.meshes) if (mesh.hasSkin()) { anySkinned = true; break; }
        if (anySkinned && res.skeletons.empty()) {
            AVER_WARN("the file carries skin but no skin node, so its joint indices address a "
                      "skeleton that was not written");
        }

        // ---- skeleton(s), named after `base` so a clip's skeletonRef resolves by stem ----
        for (usize i = 0; i < res.skeletons.size(); ++i) {
            const std::string p = outDir + "/" + base + (i == 0 ? "" : std::to_string(i)) + ".ocskel";
            if (!fmt::saveOcSkel(p, res.skeletons[i], &why)) {
                emitArtifact(input, p, "skeleton", false, false, why, stats);
                anyFailed = true;
            } else {
                emitArtifact(input, p, "skeleton", true, true, {}, stats,
                             "\"bones\":" + std::to_string(res.skeletons[i].bones.size()));
            }
        }

        // ---- clips ----
        for (usize i = 0; i < res.animations.size(); ++i) {
            fmt::OcAnimation clip = res.animations[i];
            clip.skeletonRef = base;
            const std::string name = safe(i < res.animationNames.size() ? res.animationNames[i] : "",
                                          "Clip" + std::to_string(i));
            const std::string p = outDir + "/" + base + "_" + name + ".ocanim";
            if (!fmt::saveOcAnim(p, clip, &why)) {
                emitArtifact(input, p, "animation", false, false, why, stats);
                anyFailed = true;
            } else {
                emitArtifact(input, p, "animation", true, true, {}, stats,
                             "\"durationSeconds\":" + std::to_string(clip.duration));
            }
        }

        emitSummary(input, stats, anyFailed ? 1 : 0);
        return anyFailed ? 1 : 0;
    }

    if (ext == ".obj") {
        fmt::ObjImportResult res;
        std::string why;
        if (!fmt::importObj(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }

        // ---- the .mtl's materials, which until now were parsed and then dropped ----
        //
        // The map_* paths a .mtl writes are relative to the .obj's OWN directory, not to the
        // project content root, so that is what the converter resolves against. Warnings from it
        // name any texture the file points at and does not have.
        {
            std::vector<fmt::ImportedMaterial> mats;
            std::vector<fmt::ImportedImage> imgs;
            std::vector<std::string> mwarn;
            const usize slash = input.find_last_of("/\\");
            const std::string baseDir = slash == std::string::npos ? std::string() : input.substr(0, slash);
            fmt::objMaterialsToImported(res.materials, baseDir, mats, imgs, &mwarn);
            for (const std::string& w : mwarn) AVER_WARN("materials: {}", w);
            cookAndRewriteSlots(mats, imgs, contentDir, base, res.meshes, maxTexture);
        }

        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            items.push_back(MeshItem{i < res.meshNames.size() ? res.meshNames[i] : std::string{},
                                     res.meshes[i], -1});
        }
        const bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats);
        emitSummary(input, stats, anyFailed ? 1 : 0);
        return anyFailed ? 1 : 0;
    }

    if (ext == ".usd" || ext == ".usda") {
        fmt::UsdImportResult res;
        std::string why;
        if (!fmt::importUsd(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }

        // The importer has already resolved each mesh's `rel material:binding` to the material's own
        // name, so this rewrites those names to the cooked stems exactly as the glTF path does. A
        // mesh whose slot stayed empty -- no binding, or one behind a reference this importer does
        // not compose -- is untouched, and the import said so under `unsupported`.
        cookAndRewriteSlots(res.materials, res.images, contentDir, base, res.meshes, maxTexture);

        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            // THE LEAF OF THE PRIM PATH, not the whole path. UsdImportResult::meshNames holds
            // "/root/Grass_B_02/Grass_B_02" because that is what identifies a prim; safe() then
            // strips the slashes for a filename and produces rootGrass_B_02Grass_B_02.ocmesh, which
            // is unreadable and gets worse the deeper a scene nests. writeMeshItems already
            // de-duplicates a stem it has used before, so two prims sharing a leaf name are still
            // told apart -- by a numeric suffix rather than by a path nobody can read.
            std::string name = i < res.meshNames.size() ? res.meshNames[i] : std::string{};
            const usize leaf = name.find_last_of('/');
            if (leaf != std::string::npos) name = name.substr(leaf + 1);
            items.push_back(MeshItem{std::move(name), res.meshes[i], -1});
        }
        const bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats);
        emitSummary(input, stats, anyFailed ? 1 : 0);
        return anyFailed ? 1 : 0;
    }

#if AVER_HAVE_AUDIO_IMPORT
    if (fmt::isImportableAudio(input)) {
        audio::SoundData sound;
        fmt::AudioImportResult imp = fmt::audioImportFile(input, sound);
        if (!imp.ok) {
            emitArtifact(input, {}, "audio", false, false, imp.error, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        const std::string path = outDir + "/" + base + ".ocaudio";
        std::string why;
        if (!fmt::saveOcAudio(path, sound, stemOf(input), &why)) {
            emitArtifact(input, path, "audio", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        audio::SoundData back;
        const bool reloaded = fmt::loadOcAudio(path, back, &why);
        if (!reloaded || back.frames() != sound.frames() || back.channels != sound.channels) {
            emitArtifact(input, path, "audio", true, false,
                         !reloaded && !why.empty() ? why : "reloaded audio does not match the source",
                         stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        emitArtifact(input, path, "audio", true, true, {}, stats,
                     "\"frames\":" + std::to_string(back.frames())
                     + ",\"channels\":" + std::to_string(back.channels)
                     + ",\"sampleRate\":" + std::to_string(back.sampleRate));
        emitSummary(input, stats, 0);
        return exitCode(ExitCode::Ok);
    }
#endif

    AVER_ERROR("unsupported input format: {}", input);
    emitArtifact(input, {}, "unknown", false, false, "unsupported input format", stats);
    emitSummary(input, stats, 1);
    return exitCode(ExitCode::Failed);
}
