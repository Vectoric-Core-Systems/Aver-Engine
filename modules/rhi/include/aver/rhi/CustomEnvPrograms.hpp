#pragma once
// The HLSL a custom sky, fog or particles program becomes in the engine's own translation units.
//
// A tool translates a shader to HLSL against the AVER_CUSTOM_* hooks and stores the emitted source (EmbeddedHlslEnv node); these functions put it together with the shipped hook files
// (custom_env_common.hlsl, custom_sky.hlsl, custom_fog.hlsl, custom_particles.hlsl) and the shipped helper library (material_embedded_lib.hlsl) into the text a
// consumer adds to its prelude. They read shader files through shaderFile(), build strings and touch no device. An empty result means a shipped file is missing.
//
// Nothing here runs unless a program of the kind is registered: the consumers add the text only then, so every other shader is byte for byte unchanged.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::rhi {

// One program. regs / globalRegs: uniform registers of the normal and global blocks; values: that many registers, four floats each.
struct CustomEnvProgram {
    std::string name;
    std::string ns;            // identifier prefix of every global in `source`
    std::string source;        // the emitted HLSL (entries <ns>main_sky | <ns>main_fog | <ns>main_start, <ns>main_process)
    u32 regs = 0, globalRegs = 0;
    bool hasStart = false, hasProcess = false;
    std::vector<float> values;
};

inline constexpr u32 kCustomFogShaderMax = 4;      // fog programs in one frame (a shader word in a fog volume's shape field names one, 1-based)
inline constexpr u32 kCustomFogShaderRegs = 16;    // registers reserved per fog program in the froxel constants

// The library and the translated source with the embedded names renamed (emb_* -> cenv_*, Emb* -> CEnv*, embSlot* -> cenvSlot*, EMB_SMP -> CENV_SMP, AVER_CUSTOM_LIBRARY -> AVER_CENV_LIBRARY, ...)
// so the program can share a translation unit with an embedded material shader. Exposed for the tests.
std::string customEnvRename(std::string text);

// The text spliced at kCustomSkyMarker in the shared prelude. The prelude also needs `#define AVER_CUSTOM_SKY 1` (a variant define) to route the sky functions to it.
std::string customSkyProgram(const CustomEnvProgram& p);
inline constexpr const char* kCustomSkyMarker = "//@AVER_CUSTOM_SKY_SPLICE@";
// Returns `prelude` with the marker line replaced by `program` (unchanged when `program` is empty or the marker is absent).
std::string spliceCustomSky(std::string_view prelude, const std::string& program);

// Added after the shared prelude and before voxi_froxel.hlsl (variant AVER_FOG_VOLUMES=1;AVER_FOG_SHADERS=1). Defines AVER_CUSTOM_FOG_COUNT.
std::string customFogProgram(const std::vector<const CustomEnvProgram*>& shaders);

// Added after the shared prelude and before particles_sim.hlsl (variant AVER_CUSTOM_PARTICLES=1). Defines AVER_CUSTOM_HAS_START / AVER_CUSTOM_HAS_PROCESS.
std::string customParticlesProgram(const CustomEnvProgram& p);

// The register table as a static float4 array initialiser body ("float4(a, b, c, d), ...") for a program's values (normal block, then global block).
std::string customEnvRegisterInit(const CustomEnvProgram& p, u32 first, u32 count);

}  // namespace aver::rhi
