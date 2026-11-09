#include "AssetC.hpp"

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

// ---- material generation: texture-set classification, roughness+metal packing, .ocmat writing ----
//
// Unlike `convert`, which owns exactly ONE input file end to end and dispatches by its extension,
// `material` takes a whole SET of texture files -- every map belonging to one downloaded item -- as
// input, because a texture set is only meaningful together (a lone roughness map binds nothing on
// its own). There is no per-file extension to dispatch on the way `convert` dispatches by extension,
// so this is its own subcommand rather than a mode of `convert`.

#if AVER_HAVE_MATERIAL_COMPILE

namespace assetc {

namespace {

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

} // namespace

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
        // Through FileSystem rather than std::filesystem::copy_file, which has no long-path handling.
        std::vector<u8> copyBytes;
        if (!readFileBytes(in, copyBytes) || !writeFileBytes(dest, copyBytes.data(), copyBytes.size())) {
            emitArtifact(in, dest, "material", false, false, "could not copy " + in + " to " + dest, stats);
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
    return exitCode(anyFailed ? ExitCode::Failed : ExitCode::Ok);
}

} // namespace assetc
#endif // AVER_HAVE_MATERIAL_COMPILE
