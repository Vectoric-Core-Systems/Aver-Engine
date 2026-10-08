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
#include "aver/formats/OcInstances.hpp"   // .ocinst -- where a PointInstancer's instances go by default
#include "aver/world/LevelTransform.hpp"   // its Euler encoding, for rotated USD instances
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
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// ---- --material-map: unbound USD subsets to materials by name ----

struct MaterialRule { std::string pattern, stem; };   // pattern "*" matches anything; stem "-" drops the faces

bool loadMaterialMap(const std::string& path, std::vector<MaterialRule>& rules) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        MaterialRule r;
        if (!(ls >> r.pattern) || r.pattern[0] == '#' || !(ls >> r.stem)) continue;
        for (char& ch : r.pattern) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        rules.push_back(std::move(r));
    }
    return true;
}

// Empty slots take the first rule whose pattern is in their submesh's leaf name (a GeomSubset's name), then
// each touched mesh is regrouped to one submesh per material.
void applyMaterialMap(std::vector<fmt::OcMeshData>& meshes, const std::vector<MaterialRule>& rules) {
    usize mapped = 0, dropped = 0, unmatched = 0;
    for (fmt::OcMeshData& m : meshes) {
        bool touched = false;
        std::vector<fmt::OcMeshSubmesh> keep;
        for (fmt::OcMeshSubmesh& sm : m.submeshes) {
            if (sm.materialSlot >= m.materialSlots.size() || !m.materialSlots[sm.materialSlot].empty()) {
                keep.push_back(sm);
                continue;
            }
            std::string leaf = sm.name.substr(sm.name.find_last_of('/') + 1);
            for (char& ch : leaf) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const MaterialRule* hit = nullptr;
            for (const MaterialRule& r : rules)
                if (r.pattern == "*" || leaf.find(r.pattern) != std::string::npos) { hit = &r; break; }
            if (!hit) { ++unmatched; keep.push_back(sm); continue; }
            touched = true;
            if (hit->stem == "-") { ++dropped; continue; }
            // Each subset has its own slot (buildMesh), so writing it in place touches no other submesh.
            m.materialSlots[sm.materialSlot] = hit->stem;
            ++mapped;
            keep.push_back(sm);
        }
        if (!touched && m.submeshes.size() <= 255) continue;
        m.submeshes = std::move(keep);
        fmt::groupSubmeshesByMaterial(m);
    }
    AVER_INFO("--material-map: {} submesh(es) mapped, {} dropped, {} matched no rule", mapped, dropped, unmatched);
}

// ---- writing and verifying one mesh, ported from ConvertTool.cpp unchanged ----

// Writes one .ocmesh, reloads it, and confirms the skin (the one stream that used to be dropped
// silently) survived the round trip. Reports the outcome as one JSON line; never throws.
bool writeAndVerifyMesh(const std::string& input, const std::string& path, const fmt::OcMeshData& m,
                        RunStats& stats) {
    std::string why;
    if (m.submeshes.size() > 255) {   // a merged USD prefab of many unmaterialled parts
        fmt::OcMeshData merged = m;
        const usize before = merged.submeshes.size();
        fmt::coalesceAdjacentSubmeshes(merged);
        AVER_INFO("{}: {} submeshes merged to {} (neighbours naming the same material)", path, before,
                  merged.submeshes.size());
        if (merged.submeshes.size() <= 255) return writeAndVerifyMesh(input, path, merged, stats);
    }
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
    // A folder under the output directory to write this item into (one path component, sanitised by
    // writeMeshItems); empty = the output directory itself.
    std::string subdir;
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
// One placement as the level records it, whichever importer found it. glTF and ordinary USD prims
// carry only a translation (their rotation and scale are baked into the mesh); a USD PointInstancer
// instance carries its own rotation and scale, because every instance shares one mesh.
struct ScenePlacement {
    i32  meshIndex = -1;
    Vec3 position{0, 0, 0};
    Quat rotation = Quat::identity();
    Vec3 scale{1, 1, 1};
    bool collide = true;
    // Stem of the object clip this placement plays (an animated glTF node), "" for none. The level
    // writer makes it content-relative and adds the extension, exactly as it does for a mesh stem.
    std::string animClip;
};

// The viewpoint a level opens at, when the source scene named one (a USD stage's camera).
struct SceneCamera {
    Vec3 position{0, 0, 0};
    f32  yawDeg = 0.0f, pitchDeg = 0.0f;
};

// The sun, when the source scene lit itself with one (a USD DistantLight, or a DomeLight's sun).
struct SceneSun {
    Vec3 direction{0, 0, 1};   // toward the sun, engine space
    f32  angularDeg = 0.53f;
};

// Fills 12 contiguous floats at `out` with the row-vector world transform OcInstances.hpp's own
// convention describes, for one kept USD PointInstancer instance being written as FOLIAGE instead of
// a PLACE/PLACEG entity.
//
// THIS MUST MATCH TODAY'S ENTITY PLACEMENT EXACTLY, and that is why it is not simply
// `Transform{position, rotation, scale}.toMatrix()`. A PLACEG line's rotation is not stored as this
// quaternion -- writeSceneLevel converts it to degrees with world::eulerDegFromQuat first (see its
// own `op.roll/pitch/yaw` lines, a few lines below), and world::instantiate (LevelInstance.cpp)
// reconstructs the rotation those degrees encode with world::quatFromEulerDeg when the level loads.
// That round trip is not always lossless -- eulerDegFromQuat's own header comment documents the one
// gimbal-lock corner where it can only recover the rotation up to a pinned roll/yaw split -- so an
// instance within 0.26 degrees of vertical built straight from `rotation` would end up at a visibly
// DIFFERENT orientation than the identical prim gets today as an ordinary entity. Going through the
// same two functions LevelInstance.cpp's instantiate() calls (both already reachable here via
// LevelTransform.hpp) reproduces that round trip bit for bit instead of inventing a second, mismatched
// quaternion-to-matrix path.
//
// Transform::toMatrix() (aver::Transform, Math.hpp) is the SAME row-vector scale*rotate*translate matrix
// world::instantiate builds for a root placement (an instancer instance is always a root -- USD
// PointInstancers are never nested under a level's own BEGIN/END scopes), so dropping its trailing
// (0,0,0,1) column is exactly OcInstances.hpp's own t[r*3+c] = M.m[r][c] convention.
void foliageInstanceTransform(const Vec3& position, const Quat& rotation, const Vec3& scale, f32* out) {
    Transform xf;
    xf.position = position;
    xf.rotation = world::quatFromEulerDeg(world::eulerDegFromQuat(rotation));
    xf.scale = scale;
    const Mat4 m = xf.toMatrix();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = m.m[r][c];
}

bool writeSceneLevel(const std::string& outDir, const std::string& contentDir,
                     const std::string& base,
                     const std::vector<ScenePlacement>& placements,
                     const std::vector<std::string>& stems,
                     const SceneCamera* camera = nullptr, const SceneSun* sun = nullptr,
                     // Kept USD PointInstancer instances written as FOLIAGE (the default) rather than
                     // as PLACE/PLACEG entities -- null or empty when there are none, or when
                     // --instances-as entities asked for the old behaviour instead (they are folded
                     // into `placements` in that case, and this stays null).
                     const std::vector<ScenePlacement>* foliageInstances = nullptr) {
    const bool haveFoliage = foliageInstances && !foliageInstances->empty();
    if (placements.empty() && !haveFoliage) return true; // a single-object file needs no level
    if (contentDir.empty()) {
        AVER_WARN("{} placement(s) were recovered from the scene graph, but --content-dir was not "
                  "given, so a mesh path cannot be made content-relative and no level was written. "
                  "The meshes are correct and centred; place them by hand, or re-run with "
                  "--content-dir.", placements.size() + (haveFoliage ? foliageInstances->size() : 0));
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
    for (const ScenePlacement& p : placements) {
        if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size()) continue;
        const std::string& stem = stems[usize(p.meshIndex)];
        if (stem.empty()) continue;                      // merged away, or failed to write
        fmt::OcWorldPlacement op;
        op.asset = prefix + stem + ".ocmesh";
        op.x = p.position.x; op.y = p.position.y; op.z = p.position.z;
        // Through the level format's own Euler encoding, so the loader's quatFromEulerDeg gives back
        // exactly this rotation.
        const Vec3 e = world::eulerDegFromQuat(p.rotation);
        op.roll = e.x; op.pitch = e.y; op.yaw = e.z;
        op.sx = p.scale.x; op.sy = p.scale.y; op.sz = p.scale.z;
        op.collide = p.collide;
        if (!p.animClip.empty()) op.animClip = prefix + p.animClip + ".ocanim";
        w.placements.push_back(std::move(op));
    }
    if (w.placements.empty() && !haveFoliage) return true;
    if (camera) {
        // The editor opens the level here instead of framing the whole of it; speed 0 = the user's own.
        w.hasCamera = true;
        w.camX = camera->position.x; w.camY = camera->position.y; w.camZ = camera->position.z;
        w.camYaw = camera->yawDeg; w.camPitch = camera->pitchDeg;
        w.camSpeed = 0.0;
    }
    if (sun) {
        // Direction and disc size only: colour, illuminance and the sky stay the engine's physical
        // defaults, whose atmosphere reddens and dims a low sun by itself.
        w.hasSun = true;
        w.sunDir[0] = sun->direction.x; w.sunDir[1] = sun->direction.y; w.sunDir[2] = sun->direction.z;
        w.sunAngularDeg = sun->angularDeg;
    }

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
    // THE .ocinst SITS BESIDE THE .ocworld IT WAS BUILT FOR, under the SAME (possibly suffixed) stem
    // -- so a re-import that lands as "JungleRuins_2.ocworld" (because "JungleRuins.ocworld" already
    // existed) gets "JungleRuins_2.ocinst" rather than a table silently misnamed after the level that
    // did NOT get overwritten. `path` already carries that final stem, decided by the dedup loop above.
    const std::string levelStem = stemOf(path);
    if (haveFoliage) {
        // GROUP BY PROTOTYPE MESH ASSET. Two passes rather than one: the first counts each prototype's
        // instances (and records the order prototypes are first seen in, so the groups in the file
        // read in a stable, deterministic order run after run), which is what lets the second pass
        // write every instance straight into its final slot in ONE allocation -- no growing buffer, no
        // second bulk copy, the same "no per-instance allocation" contract OcInstances.hpp's own
        // fast-path comment asks of a caller building a multi-million-row table.
        std::vector<i32> order;
        std::unordered_map<i32, u32> counts;
        for (const ScenePlacement& p : *foliageInstances) {
            if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size() || stems[usize(p.meshIndex)].empty())
                continue;                                   // merged away, or failed to write
            auto [it, inserted] = counts.try_emplace(p.meshIndex, 0u);
            if (inserted) order.push_back(p.meshIndex);
            ++it->second;
        }

        fmt::OcInstanceData inst;
        std::unordered_map<i32, u32> baseOf, cursorOf;
        u32 running = 0;
        for (const i32 mi : order) {
            const u32 count = counts[mi];
            // CAST-SHADOW BY DEFAULT: OcInstances.hpp's kOcInstanceFlagCastShadow is not yet an
            // authorable choice anywhere in this pipeline, so every group this importer writes sets
            // it -- foliage that could not cast a shadow at all would be a visible regression against
            // the entities it replaces, which always could.
            inst.groups.push_back(fmt::OcInstanceGroup{prefix + stems[usize(mi)] + ".ocmesh",
                                                       fmt::kOcInstanceFlagCastShadow, running, count});
            baseOf[mi] = running;
            cursorOf[mi] = 0;
            running += count;
        }
        inst.transforms.resize(usize(running) * 12);
        for (const ScenePlacement& p : *foliageInstances) {
            if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size() || stems[usize(p.meshIndex)].empty())
                continue;
            u32& cursor = cursorOf[p.meshIndex];
            foliageInstanceTransform(p.position, p.rotation, p.scale,
                                     &inst.transforms[usize(baseOf[p.meshIndex] + cursor) * 12]);
            ++cursor;
        }

        const std::string instPath = mapsDir + "/" + levelStem + ".ocinst";
        std::string instWhy;
        if (!fmt::saveOcInstances(instPath, inst, &instWhy)) {
            AVER_WARN("could not write the foliage instance table {}: {}", instPath, instWhy);
            return false;
        }
        // Content-relative, exactly like every other asset path this function writes (`op.asset`
        // above) -- but relative to `contentDir` directly, NOT through `prefix` (which is `outDir`'s
        // own relative path, the Meshes folder): mapsDir is always literally `contentDir + "/Maps"`,
        // so the .ocinst's own content-relative path is always exactly "Maps/<levelStem>.ocinst".
        w.foliageFiles.push_back("Maps/" + levelStem + ".ocinst");

        std::error_code sizeEc;
        const std::uintmax_t bytes = std::filesystem::file_size(instPath, sizeEc);
        AVER_INFO("wrote {}: {} group(s), {} instance(s), {:.1f} MiB -- ray-traced foliage, no "
                  "collision, not individually selectable",
                  instPath, inst.groups.size(), running,
                  sizeEc ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0));
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
        // The stem carries the subfolder, so de-duplication and the level's asset path both see it.
        if (!items[i].subdir.empty()) {
            const std::string sub = safe(items[i].subdir, "Group");
            createDirectories(outDir + "/" + sub);
            stem = sub + "/" + stem;
        }
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

// `convert` converts argv[2] (a glTF/GLB, OBJ, USD stage, or -- when this build has audio import -- a
// WAV/MP3/M4A/FLAC) into argv following --out-dir, naming outputs after --base or the source stem.
// `material` (when this build has material compiling) converts a whole texture SET the same way --
// see runMaterial's own comment for why it is a separate subcommand rather than another `convert`
// extension. Every result, and a final summary, are reported as JSON Lines on stdout; AVER_* log
// lines (always '['-prefixed) may be interleaved and are for human troubleshooting only. Exit 0:
// everything declared verified. Exit 1: an import or verify failure. Exit 2: bad usage.
int main(int argc, char** argv) {
    static const char* kUsage =
        "usage: AverAssetC convert <input-file> --out-dir <dir> [--base <name>] [--merge] [--lod <ratio>]"
        "\n                          [--instances-as foliage|entities]   USD PointInstancer instances as"
        "\n                                                              baked .ocinst foliage (default) or PLACE entities"
        "\n                          [--max-instances <n>] [--max-instance-tris <n>]   USD PointInstancer budget, 0 = none;"
        "\n                                                              defaults depend on --instances-as (see docs/ASSET_IMPORT.md)"
        "\n                          [--focus camera|none|<x>,<y>] [--focus-radius <cm>] [--keep-all-below <n>]"
        "\n                          [--exclude <prim path>[,<prim path>...]]   USD prims to leave out"
        "\n                          [--material-map <file>]   USD: '<name substring> <material stem|->' per line,"
        "\n                                                              for subsets with no bound material"
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
    // WHERE A KEPT POINTINSTANCER INSTANCE GOES. FOLIAGE (the default) is what this comment used to
    // call the only option: a scattered stage declares millions of instances (Jungle Ruins: 8.7
    // million), and until .ocinst existed the level held one ENTITY per kept instance -- past ~16k
    // entities the frame was CPU-bound, so --max-instances defaulted to a number nowhere near the
    // stage's own count. Instanced foliage pays no such per-instance entity cost (ray-traced only,
    // one TLAS instance and no draw, no collision, not individually selectable -- see OcInstances.hpp
    // and docs/ASSET_IMPORT.md), so the budget below is sized for the FORMAT's own ceiling instead of
    // the entity system's, and --instances-as entities is what asks for the old behaviour verbatim.
    bool instancesAsEntities = false;
    // A USD stage's PointInstancer budget (UsdImportOptions). 0 on the command line lifts a limit;
    // left at 0 here and resolved AFTER argument parsing, once --instances-as is known, since the two
    // modes want different defaults (see just below main's argument loop).
    u64 maxInstances = 0;
    u64 maxInstanceTris = 0;
    bool maxInstancesSet = false, maxInstanceTrisSet = false;
    // Densest around the stage's own camera -- the view it was built to be seen from -- within 100 m,
    // and any prototype with 2000 instances or fewer kept whole (see UsdImportOptions::focus).
    fmt::UsdInstanceFocus focusMode = fmt::UsdInstanceFocus::FirstCamera;
    Vec3 focusPoint{0, 0, 0};
    f32 focusRadius = 10000.0f;
    u64 keepAllBelow = 2000;
    std::vector<std::string> excludePrims;   // --exclude, repeatable and/or comma-separated
    std::string materialMapPath;              // --material-map
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
        else if (a == "--instances-as" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "entities")     instancesAsEntities = true;
            else if (v == "foliage") instancesAsEntities = false;
            else {
                AVER_ERROR("--instances-as must be 'foliage' or 'entities', not '{}'", v);
                return exitCode(ExitCode::Usage);
            }
        }
        else if (a == "--max-instances" && i + 1 < argc) {
            maxInstances = std::strtoull(argv[++i], nullptr, 10);
            maxInstancesSet = true;
        }
        else if (a == "--max-instance-tris" && i + 1 < argc) {
            maxInstanceTris = std::strtoull(argv[++i], nullptr, 10);
            maxInstanceTrisSet = true;
        }
        // --focus camera | none | <x>,<y>  (engine centimetres)
        else if (a == "--focus" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "camera")    focusMode = fmt::UsdInstanceFocus::FirstCamera;
            else if (v == "none") focusMode = fmt::UsdInstanceFocus::Uniform;
            else {
                focusMode = fmt::UsdInstanceFocus::Point;
                const usize comma = v.find(',');
                focusPoint.x = static_cast<f32>(std::atof(v.substr(0, comma).c_str()));
                focusPoint.y = comma == std::string::npos ? 0.0f : static_cast<f32>(std::atof(v.substr(comma + 1).c_str()));
            }
        }
        else if (a == "--focus-radius" && i + 1 < argc) focusRadius = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--keep-all-below" && i + 1 < argc) keepAllBelow = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--material-map" && i + 1 < argc) materialMapPath = argv[++i];
        else if (a == "--exclude" && i + 1 < argc) {
            const std::string list = argv[++i];
            usize start = 0;
            while (start <= list.size()) {
                const usize comma = list.find(',', start);
                const std::string one = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!one.empty()) excludePrims.push_back(one);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else if (!haveInput) { input = a; haveInput = true; }
    }
    if (!haveInput || outDir.empty()) {
        AVER_ERROR("{}", kUsage);
        return exitCode(ExitCode::Usage);
    }
    while (!outDir.empty() && (outDir.back() == '\\' || outDir.back() == '/')) outDir.pop_back();
    const std::string base = !baseOverride.empty() ? baseOverride : stemOf(input);

    // THE PER-MODE DEFAULTS, applied only where the command line left a budget unset.
    if (instancesAsEntities) {
        // UNCHANGED from every build before --instances-as existed.
        if (!maxInstancesSet)   maxInstances   = 12000;
        if (!maxInstanceTrisSet) maxInstanceTris = 150000000;
    } else {
        // FOLIAGE: instancing shares one BLAS per prototype, so the per-instance triangle cost that
        // motivated maxInstanceTris for entities does not apply here -- unlimited (0) is the default.
        if (!maxInstanceTrisSet) maxInstanceTris = 0;
        if (!maxInstancesSet) maxInstances = 4000000;
        // THE FORMAT'S OWN HARD CEILING, aver::voxi::VoxiRenderer::kMaxFoliageInstances
        // (modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp) -- duplicated here as a literal
        // rather than an include, since this tool does not and should not link Aver.Voxi for one
        // constant. 0 means "no limit" on the command line (UsdImportOptions' own contract), but
        // there is no such thing as unlimited FOLIAGE: a table past this many rows only wastes convert
        // time and disk, since setFoliage truncates past it (with its own warning) the moment the
        // level loads. Clamping here, rather than only at load, is what makes "what was kept" in the
        // log below the number that actually survives.
        constexpr u64 kMaxFoliageInstances = 8000000;
        if (maxInstances == 0 || maxInstances > kMaxFoliageInstances) maxInstances = kMaxFoliageInstances;
    }

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
        std::vector<ScenePlacement> scene;
        scene.reserve(res.placements.size());
        for (const fmt::GltfPlacement& p : res.placements) scene.push_back(ScenePlacement{p.meshIndex, p.position});

        // ---- object clips, BEFORE the level so a placement only names a clip that was written ----
        //
        // The motion of a plain glTF node (a car on a route, a fan) as <base>_<node>.ocanim beside the
        // skeletal clips. Names are taken after the skeletal clips', so a node named like an armature
        // action cannot overwrite it.
        {
            // Compared lower-cased: Windows treats "Car" and "car" as ONE file, so the second clip
            // would overwrite the first.
            const auto fileKey = [](std::string s) {
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            std::unordered_set<std::string> taken;
            for (usize i = 0; i < res.animations.size(); ++i)
                taken.insert(fileKey(safe(i < res.animationNames.size() ? res.animationNames[i] : "",
                                          "Clip" + std::to_string(i))));
            for (usize i = 0; i < res.objectAnimations.size(); ++i) {
                std::string name = safe(i < res.objectAnimationNames.size() ? res.objectAnimationNames[i] : "",
                                        "ObjectClip" + std::to_string(i));
                if (!taken.insert(fileKey(name)).second) {
                    const std::string root = name;
                    for (int n = 2; !taken.insert(fileKey(name = root + "_" + std::to_string(n))).second; ++n) {}
                }
                const fmt::OcAnimation& clip = res.objectAnimations[i];
                const std::string p = outDir + "/" + base + "_" + name + ".ocanim";
                if (!fmt::saveOcAnim(p, clip, &why)) {
                    emitArtifact(input, p, "animation", false, false, why, stats);
                    anyFailed = true;
                    continue;
                }
                emitArtifact(input, p, "animation", true, true, {}, stats,
                             "\"durationSeconds\":" + std::to_string(clip.duration));
                const i32 pl = i < res.objectAnimationPlacement.size() ? res.objectAnimationPlacement[i] : -1;
                if (pl >= 0 && usize(pl) < scene.size()) {
                    // A placement plays ONE clip, so the FIRST one the file lists keeps it; a later clip
                    // for the same node is still written (it can be picked by hand) but says so here.
                    if (scene[usize(pl)].animClip.empty()) {
                        scene[usize(pl)].animClip = base + "_" + name;
                    } else {
                        AVER_WARN("'{}' is animated by more than one glTF animation: its placement plays '{}'; "
                                  "'{}' was written but not attached", res.placements[usize(pl)].name,
                                  scene[usize(pl)].animClip, base + "_" + name);
                    }
                }
            }
        }
        if (!writeSceneLevel(outDir, contentDir, base, scene, stems)) anyFailed = true;

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

    if (ext == ".usd" || ext == ".usda" || ext == ".usdc") {
        fmt::UsdImportResult res;
        std::string why;
        // THE WHOLE STAGE: sublayers, binary layers, references and PointInstancers. A single text
        // layer imports exactly as importUsd would read it.
        fmt::UsdImportOptions uopt;
        uopt.maxInstances = maxInstances;
        uopt.maxInstanceTriangles = maxInstanceTris;
        uopt.focus = focusMode;
        uopt.focusPoint = focusPoint;
        uopt.focusRadius = focusRadius;
        uopt.keepAllBelow = keepAllBelow;
        uopt.excludePrims = excludePrims;
        // Instanced foliage is thinned by what can be SEEN at a distance (a tree stays dense far out,
        // moss does not): UsdImportOptions::focusBySize. Entities keep the per-prototype share.
        uopt.focusBySize = !instancesAsEntities;
        if (!fmt::importUsdStage(input, res, uopt, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.instancing.instancers) {
            AVER_INFO("PointInstancers: {} instancer(s), {} prototype mesh(es), {} of {} instance(s) kept "
                      "(--max-instances {}, --max-instance-tris {}, --instances-as {})",
                      res.instancing.instancers, res.instancing.prototypes, res.instancing.keptInstances,
                      res.instancing.sourceInstances, maxInstances, maxInstanceTris,
                      instancesAsEntities ? "entities" : "foliage");
            if (res.instancing.focused)
                AVER_INFO("  focus ({:.0f}, {:.0f}), radius {:.0f} cm: {} of {} instance(s) inside it kept",
                          res.instancing.focusPoint.x, res.instancing.focusPoint.y, res.instancing.focusRadius,
                          res.instancing.keptInFocus, res.instancing.sourceInFocus);
            for (const fmt::UsdPrototypeStats& p : res.instancing.perPrototype) {
                const std::string name = p.meshIndex >= 0 && usize(p.meshIndex) < res.meshNames.size()
                                             ? res.meshNames[usize(p.meshIndex)] : std::string("?");
                if (p.fullDensityRadius > 0.0f)
                    AVER_INFO("  prototype {}: {} triangle(s), ~{:.0f} cm radius, {} of {} instance(s) kept, "
                              "full density within {:.0f} m", name, p.triangles, p.radius, p.kept, p.instances,
                              p.fullDensityRadius / 100.0f);
                else
                    AVER_INFO("  prototype {}: {} triangle(s), ~{:.0f} cm radius, {} of {} instance(s) kept", name,
                              p.triangles, p.radius, p.kept, p.instances);
            }
        }
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
        if (!materialMapPath.empty()) {
            std::vector<MaterialRule> rules;
            if (!loadMaterialMap(materialMapPath, rules)) {
                emitArtifact(input, {}, "mesh", false, false, "cannot read --material-map " + materialMapPath, stats);
                emitSummary(input, stats, 1);
                return exitCode(ExitCode::Failed);
            }
            applyMaterialMap(res.meshes, rules);
        }

        // A stage spread over several folders is written the same way, one subfolder per source
        // folder; a single-folder stage stays flat.
        std::vector<std::string> groups = res.meshGroups;
        groups.resize(res.meshes.size());
        bool grouped = false;
        for (const std::string& g : groups) if (!g.empty() && g != groups.front()) { grouped = true; break; }

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
            // MOVED, not copied: a composed stage's terrain alone can be gigabytes of vertices.
            items.push_back(MeshItem{std::move(name), std::move(res.meshes[i]), -1, grouped ? groups[i] : std::string{}});
        }
        std::vector<std::string> stems;
        bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats, &stems);
        std::vector<ScenePlacement> scene;
        std::vector<ScenePlacement> foliageInstances;
        scene.reserve(res.placements.size());
        for (const fmt::UsdPlacement& p : res.placements) {
            ScenePlacement sp{p.meshIndex, p.position,
                              Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}, p.scale};
            // AN INSTANCE IS SCATTER -- grass, moss, a sapling -- and a collision body per blade would
            // cost physics thousands of bodies for surfaces nobody should be stopped by. The stage's
            // own meshes (terrain, ruins, water) keep the default. An empty `name` is exactly how
            // UsdPlacement marks a PointInstancer instance (see its own header comment).
            const bool isInstancerInstance = p.name.empty();
            sp.collide = !isInstancerInstance;
            // FOLIAGE, NOT AN ENTITY, unless --instances-as entities asked for the old behaviour: a
            // kept instance goes into its own list instead of `scene`, so writeSceneLevel writes it
            // into the level's .ocinst table and a FOLIAGE record rather than a PLACE/PLACEG line.
            if (isInstancerInstance && !instancesAsEntities) foliageInstances.push_back(sp);
            else scene.push_back(sp);
        }
        SceneCamera cam;
        if (!res.cameras.empty()) {
            const fmt::UsdCameraPose& c = res.cameras.front();
            cam = SceneCamera{c.position, c.yawDeg, c.pitchDeg};
            AVER_INFO("level camera: the stage's {} at ({:.0f}, {:.0f}, {:.0f}) yaw {:.1f} pitch {:.1f}", c.name,
                      c.position.x, c.position.y, c.position.z, c.yawDeg, c.pitchDeg);
        }
        SceneSun sun;
        if (res.sun.found) {
            sun = SceneSun{res.sun.direction, res.sun.angularDiameterDeg};
            const f32 elev = std::asin(std::fmax(-1.0f, std::fmin(1.0f, res.sun.direction.z))) * 57.2957795f;
            const f32 azim = std::atan2(res.sun.direction.y, res.sun.direction.x) * 57.2957795f;
            AVER_INFO("level sun: from {} -- elevation {:.1f}, azimuth {:.1f} degrees", res.sun.source, elev, azim);
        }
        if (!writeSceneLevel(outDir, contentDir, base, scene, stems, res.cameras.empty() ? nullptr : &cam,
                             res.sun.found ? &sun : nullptr,
                             foliageInstances.empty() ? nullptr : &foliageInstances))
            anyFailed = true;
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
