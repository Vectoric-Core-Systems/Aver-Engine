#pragma once
// Fabric G6 SH2: a Godot spatial shader prepared for the engine's material path. This is the one place that decides how a shader's uniforms, samplers,
// render modes and built-ins land on an Aver material; tools/fabric writes the result into a .ocgraph (GodotShader node) and a .ocmat, and the tests
// compile it in the engine's real material prelude. Pure text and numbers in, plain structs out: no engine type and no device.
// Contract: docs/plans/fabric/G6_SHADERS.md, section SH2.
#include "aver/formats/godot/GodotShader.hpp"

#include <map>
#include <string>
#include <vector>

namespace aver::fmt::godot {

// The material's texture slots (pbr::TextureSlot) a shader's sampler uniforms are spread over. Slot 7 is the uniform table. Colour-managed samplers
// (source_color) take the slots the engine decodes as sRGB; the others take the linear ones.
inline constexpr int kGdEngineSrgbSlots[3] = {0, 4, 5};
inline constexpr int kGdEngineLinearSlots[4] = {1, 3, 6, 2};
inline constexpr int kGdEngineTableSlot = 7;
inline constexpr int kGdEngineMaxRegisters = 256;

struct GdEngineOptions {
    GdIncludeResolver includeResolver;                       // #include "path"
    std::map<std::string, GdConstValue> globalDefaults;      // project shader_globals: name -> default value (a global uniform not listed is zero)
    std::vector<std::string> globalNames;                    // when non-empty, a `global uniform` not in the list is an error
    GdFilter defaultFilter = GdFilter::LinearMipmap;         // rendering/textures/default_filters
    GdRepeat defaultRepeat = GdRepeat::Disable;
    std::string name;                                        // the shader's name (stem), used for the name prefix
};

// A sampler uniform and where it landed.
struct GdEngineSampler {
    std::string name;
    int slot = -1;               // material texture slot, -1 = none left / not bindable
    bool sourceColor = false;
    GdTexHint hint = GdTexHint::None;
    GdFilter filter = GdFilter::LinearMipmap;
    GdRepeat repeat = GdRepeat::Disable;
    bool bindable = true;        // false: screen / depth / normal-roughness textures and global samplers; the shader sees the hint's default colour
    std::string reason;          // why it is not bindable
};

// A non-sampler uniform with its place in the material's table (the register is absolute: the block's base is already added).
struct GdEngineUniform {
    std::string name;
    GdType type;
    GdUniformScope scope = GdUniformScope::Normal;
    bool sourceColor = false;
    GdUniformSlot slot;
    GdConstValue def;            // the shader's default (valid = false when it had none)
};

// One thing the converter must report: severity "approximated" or "unsupported", the subject (a built-in, a render mode, a uniform) and the reason.
struct GdEngineNote { std::string severity, subject, reason; };

struct GdEngineShader {
    bool ok = false;
    std::string error;                   // the first error, "file:line:col: message" for a shader error
    std::vector<GdShaderDiag> diags;     // every diagnostic (warnings included)
    std::string name;
    std::string ns;                      // the identifier prefix of every global in the sources
    std::string fragmentSource, vertexSource;
    bool hasFragment = false, hasVertex = false, hasLight = false;
    GdUniformBlock uniforms, instanceUniforms, globalUniforms;
    std::vector<GdVaryingSlot> varyings;
    std::vector<GdEngineSampler> samplers;
    std::vector<GdEngineUniform> params;     // every non-sampler uniform, normal block first, then instance, then global
    std::vector<float> defaultRegisters; // the table at the shader's defaults: normal block, then instance, then global; 4 floats a register
    // Render-mode bits for the generated material function (aver::fmt::hlslfx).
    unsigned fxBits = 0;
    // Material state read from render_mode and the fragment's writes (the converter turns these into the FX record and the blend mode).
    int cull = 1;                        // 0 default, 1 back (Godot's default), 2 front, 3 none
    int blend = 0;                       // 0 mix, 1 add, 2 sub, 3 mul, 4 premultiplied alpha
    int depthDraw = 0;                   // 0 opaque only (default), 1 always, 2 never
    bool depthTestDisabled = false, shadowsDisabled = false;
    bool writesAlpha = false, writesAlphaScissor = false, usesDiscard = false;
    std::vector<GdEngineNote> notes;
    // The built-in variables the emitted sources use, with whether each is read or written (for the report and the tests).
    std::vector<GdHookUse> hooks;
};

// Preprocess, check and emit `source`. ok = false (with `error`) for a shader that has errors, is not shader_type spatial, or uses a construct the
// engine path cannot carry (a non-2D sampler, more samplers than texture slots, more than 256 uniform registers, ...).
bool gdShaderForEngine(std::string_view source, std::string_view fileName, const GdEngineOptions& opt, GdEngineShader& out);

// Writes the value of uniform `u` (components in GdConstValue order) into the table. Missing components stay zero.
void gdPackUniform(const GdUniformSlot& slot, const GdUniform& u, const std::vector<double>& value, std::vector<float>& regs);
// The same for a prepared uniform (its slot's register is absolute in the table).
void gdPackUniform(const GdEngineUniform& u, const std::vector<double>& value, std::vector<float>& regs);
// How many numbers one value of `type` carries (a matrix column-major, an array element after element).
int gdValueComponents(const GdType& type);

}  // namespace aver::fmt::godot
