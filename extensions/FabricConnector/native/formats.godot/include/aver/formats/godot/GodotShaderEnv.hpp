#pragma once
// Fabric G6 SH3: a Godot sky, fog or particles shader prepared for the engine. Like GodotShaderEngine.hpp for spatial shaders, this is the one place
// that decides how the shader's uniforms, samplers and built-ins land on the engine; tools/fabric writes the result into a .ocgraph (EmbeddedHlslEnv
// node, OcGraphHlsl.hpp) and the tests compile it in the engine's real translation units. Pure text and numbers in, plain structs out.
// Contract: docs/plans/fabric/G6_SHADERS.md, section SH3.
#include "aver/formats/godot/GodotShaderEngine.hpp"

#include <string>
#include <vector>

namespace aver::fmt::godot {

enum class GdEnvKind : int { Sky = 0, Fog = 1, Particles = 2 };
const char* gdEnvKindName(GdEnvKind k);

// Uniform registers an environment shader may use (sky: baked into the program; fog: a tail of the froxel constants; particles: the 16 user-parameter slots).
inline constexpr int kGdEnvMaxRegisters = 16;

struct GdEnvOptions {
    GdIncludeResolver includeResolver;                       // #include "path"
    std::map<std::string, GdConstValue> globalDefaults;      // project shader_globals: name -> default
    std::vector<std::string> globalNames;                    // when non-empty, a `global uniform` not in the list is an error
    std::string name;                                        // the shader's name (stem), used for the name prefix
};

struct GdEnvShader {
    bool ok = false;
    std::string error;                   // the first error, "file:line:col: message" for a shader error
    std::vector<GdShaderDiag> diags;
    GdEnvKind kind = GdEnvKind::Sky;
    std::string name;
    std::string ns;                      // identifier prefix of every global in the source
    // The emitted HLSL: every global carries `ns`; entries are <ns>main_sky | <ns>main_fog | <ns>main_start and <ns>main_process. Library names are the SH1
    // set (gd_*, Gd*): the engine renames them when it composes the program, so it can share a translation unit with a material shader.
    std::string source;
    bool hasStart = false, hasProcess = false;   // particles
    GdUniformBlock uniforms, globalUniforms;
    int registerCount = 0;               // normal block registers
    int globalRegisterCount = 0;         // global block registers (they follow the normal ones in `defaultRegisters`)
    std::vector<GdEngineSampler> samplers;   // all unbound: the shader reads each hint's default colour
    std::vector<GdEngineUniform> params;     // every non-sampler uniform
    std::vector<float> defaultRegisters;     // normal block then global block; 4 floats a register
    std::vector<GdEngineNote> notes;
    std::vector<GdHookUse> hooks;
    bool usesTime = false;
};

// Preprocess, check and emit a shader_type sky, fog or particles shader. ok = false (with `error`) for a shader with errors, another shader type, a sampler
// that is not a sampler2D, more than 16 uniform registers, an instance uniform, or a construct the engine path cannot carry (RADIANCE in a sky shader).
bool gdShaderForEnv(std::string_view source, std::string_view fileName, const GdEnvOptions& opt, GdEnvShader& out);

}  // namespace aver::fmt::godot
