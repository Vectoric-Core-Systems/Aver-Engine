#pragma once
// An embedded HLSL program carried inside a DOMAIN material .ocgraph: any tool can translate a shader into HLSL against the AVER_CUSTOM_* hook macros and hand it
// to the engine this way (an engine extension does it for the shaders of a foreign project).
//
// The graph holds ONE node, `NODE hlsl EmbeddedHlsl key=value ...`, whose attributes are the shader already emitted as HLSL by its translator. The engine never
// sees the source language: pbr::compileMaterialGraph reads this node (unpackEmbeddedHlsl), and a tool writes it (packEmbeddedHlsl). Text values are
// percent-escaped so they survive the line format (whitespace, '#' and '%' are escaped) and are split into chunks so a long source does not make one enormous
// token. A graph without an EmbeddedHlsl node is unaffected. Files written before the rename carry `NODE gd GodotShader` and `NODE gd GodotEnvShader` (and
// `DOMAIN godotenv`): the reader takes them as aliases, the writer always uses the new spellings.
//
// Attributes: ns, nu, ni, ng (register counts of the three uniform blocks), fx (render-mode bits, hlslfx), hasvs, hasfs, name, and the text chunks
// fs0..fsN (fragment-compile source), vs0..vsN (vertex-compile source), vary (varyings: one `flat|smooth:rows:decl` item per varying, ';'-joined).
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Render-mode bits the generated material function acts on after the surface is built (the pipeline state ones travel in the material's FX record).
namespace hlslfx {
inline constexpr u32 kUnshaded = 1u << 0;
inline constexpr u32 kAmbientDisabled = 1u << 1;
inline constexpr u32 kSpecularDisabled = 1u << 2;
inline constexpr u32 kDiffuseWrap = 1u << 3;        // diffuse wrap (stands in for the subsurface wrap term)
inline constexpr u32 kWorldVertexCoords = 1u << 4;  // vertex(): VERTEX / NORMAL are in world space
inline constexpr u32 kSkipVertexTransform = 1u << 5;// vertex(): the shader transforms VERTEX / NORMAL to view space itself
inline constexpr u32 kUsesNormalWrite = 1u << 6;    // fragment() writes NORMAL (converted to a tangent-space perturbation)
}  // namespace hlslfx

struct EmbeddedHlslVarying { std::string decl; bool flat = false; u32 rows = 1; };

struct EmbeddedHlslPart {
    std::string name;            // the shader's name, for the generated comment
    std::string ns;              // identifier prefix of every global in the sources
    u32 regsU = 0, regsI = 0, regsG = 0;   // uniform registers: normal, instance, global (the material's table holds them in this order)
    u32 fx = 0;                  // hlslfx bits
    bool hasFragment = false, hasVertex = false;
    std::string fragmentSource;  // emitted with only fragment() (and what it reaches); entry <ns>main_fragment
    std::string vertexSource;    // emitted with only vertex(); entry <ns>main_vertex
    std::vector<EmbeddedHlslVarying> varyings;

    u32 totalRegs() const { return regsU + regsI + regsG; }
    std::string fragmentEntry() const { return ns + "main_fragment"; }
    std::string vertexEntry() const { return ns + "main_vertex"; }
};

// Percent-escape / unescape used for the attribute values (exposed for tests).
std::string hlslGraphEscape(std::string_view s);
bool hlslGraphUnescape(std::string_view s, std::string& out);

// Builds the graph: DOMAIN material and the single EmbeddedHlsl node.
void packEmbeddedHlsl(const EmbeddedHlslPart& part, OcGraphData& g);

// True when the graph has an EmbeddedHlsl node (or the older GodotShader spelling).
bool ocGraphHasEmbeddedHlsl(const OcGraphData& g);

// Reads the node. False (with `err`) when it is malformed.
bool unpackEmbeddedHlsl(const OcGraphData& g, EmbeddedHlslPart& out, std::string* err = nullptr);

// ---- sky, fog and particles programs --------------------------------------------------------------------------------------------------------------
// A sky / fog / particles program is carried in a `DOMAIN customenv` graph (a domain the gameplay and material sweeps skip) holding ONE
// `NODE hlsl EmbeddedHlslEnv` node: kind (sky | fog | particles), name, ns, regs / gregs (register counts), start / process (particles), reg (the default
// uniform table: 32-bit words in hex, ','-joined, four to a register) and the emitted source in src0..srcN chunks.
struct EmbeddedHlslEnvPart {
    std::string kind;            // "sky", "fog", "particles"
    std::string name;
    std::string ns;              // identifier prefix of every global in the source
    u32 regs = 0, globalRegs = 0;
    bool hasStart = false, hasProcess = false;
    std::vector<float> defaults; // (regs + globalRegs) * 4 floats
    std::string source;
};

void packEmbeddedHlslEnv(const EmbeddedHlslEnvPart& part, OcGraphData& g);
bool ocGraphHasEmbeddedHlslEnv(const OcGraphData& g);
bool unpackEmbeddedHlslEnv(const OcGraphData& g, EmbeddedHlslEnvPart& out, std::string* err = nullptr);

}  // namespace aver::fmt
