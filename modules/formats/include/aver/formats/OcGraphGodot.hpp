#pragma once
// Fabric G6 SH2: a converted Godot spatial shader carried inside a DOMAIN material .ocgraph.
//
// The graph holds ONE node, `NODE gd GodotShader key=value ...`, whose attributes are the shader already emitted as HLSL by the Godot front-end
// (modules/formats.godot). The engine never sees GDScript-shading-language text: pbr::compileMaterialGraph reads this node (unpackGodotShader),
// and tools/fabric writes it (packGodotShader). Text values are percent-escaped so they survive the line format (whitespace, '#' and '%' are
// escaped) and are split into chunks so a long source does not make one enormous token. A graph without a GodotShader node is unaffected.
//
// Attributes: ns, nu, ni, ng (register counts of the three uniform blocks), fx (render-mode bits, GdFx), hasvs, hasfs, name, and the text chunks
// fs0..fsN (fragment-compile source), vs0..vsN (vertex-compile source), vary (varyings: one `flat|smooth:rows:decl` item per varying, ';'-joined).
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Render-mode bits the generated material function acts on after the surface is built (the pipeline state ones travel in the material's FX record).
namespace gdfx {
inline constexpr u32 kUnshaded = 1u << 0;
inline constexpr u32 kAmbientDisabled = 1u << 1;
inline constexpr u32 kSpecularDisabled = 1u << 2;
inline constexpr u32 kDiffuseWrap = 1u << 3;        // diffuse_lambert_wrap (stands in for the subsurface wrap term)
inline constexpr u32 kWorldVertexCoords = 1u << 4;  // vertex(): VERTEX / NORMAL are in world space
inline constexpr u32 kSkipVertexTransform = 1u << 5;// vertex(): the shader transforms VERTEX / NORMAL to view space itself
inline constexpr u32 kUsesNormalWrite = 1u << 6;    // fragment() writes NORMAL (converted to a tangent-space perturbation)
}  // namespace gdfx

struct GodotShaderVarying { std::string decl; bool flat = false; u32 rows = 1; };

struct GodotShaderPart {
    std::string name;            // the shader's name, for the generated comment
    std::string ns;              // identifier prefix of every global in the sources (GdHlslOptions::namePrefix)
    u32 regsU = 0, regsI = 0, regsG = 0;   // uniform registers: normal, instance, global (the material's table holds them in this order)
    u32 fx = 0;                  // gdfx bits
    bool hasFragment = false, hasVertex = false;
    std::string fragmentSource;  // emitted with only fragment() (and what it reaches); entry <ns>main_fragment
    std::string vertexSource;    // emitted with only vertex(); entry <ns>main_vertex
    std::vector<GodotShaderVarying> varyings;

    u32 totalRegs() const { return regsU + regsI + regsG; }
    std::string fragmentEntry() const { return ns + "main_fragment"; }
    std::string vertexEntry() const { return ns + "main_vertex"; }
};

// Percent-escape / unescape used for the attribute values (exposed for tests).
std::string gdGraphEscape(std::string_view s);
bool gdGraphUnescape(std::string_view s, std::string& out);

// Builds the graph: DOMAIN material and the single GodotShader node.
void packGodotShader(const GodotShaderPart& part, OcGraphData& g);

// True when the graph has a GodotShader node.
bool ocGraphHasGodotShader(const OcGraphData& g);

// Reads the node. False (with `err`) when it is malformed.
bool unpackGodotShader(const OcGraphData& g, GodotShaderPart& out, std::string* err = nullptr);

// ---- G6 SH3: sky, fog and particles shaders -----------------------------------------------------------------------------------------------------------
// A converted Godot sky / fog / particles shader is carried in a `DOMAIN godotenv` graph (a domain the gameplay and material sweeps skip) holding ONE
// `NODE gd GodotEnvShader` node: kind (sky | fog | particles), name, ns, regs / gregs (register counts), start / process (particles), reg (the default
// uniform table: 32-bit words in hex, ','-joined, four to a register) and the emitted source in src0..srcN chunks.
struct GodotEnvPart {
    std::string kind;            // "sky", "fog", "particles"
    std::string name;
    std::string ns;              // identifier prefix of every global in the source
    u32 regs = 0, globalRegs = 0;
    bool hasStart = false, hasProcess = false;
    std::vector<float> defaults; // (regs + globalRegs) * 4 floats
    std::string source;
};

void packGodotEnvShader(const GodotEnvPart& part, OcGraphData& g);
bool ocGraphHasGodotEnvShader(const OcGraphData& g);
bool unpackGodotEnvShader(const OcGraphData& g, GodotEnvPart& out, std::string* err = nullptr);

}  // namespace aver::fmt
