#pragma once
// Fabric G6 SH3: the HLSL a converted Godot sky, fog or particles shader becomes in the engine's own translation units (docs/plans/fabric/G6_SHADERS.md).
//
// The converter (formats.godot gdShaderForEnv) stores the emitted shader source; these functions put it together with the shipped hook files
// (godot_env_common.hlsl, godot_sky.hlsl, godot_fog.hlsl, godot_particles.hlsl) and the shipped Godot helper library (material_godot_lib.hlsl) into the text a
// consumer adds to its prelude. They read shader files through shaderFile(), build strings and touch no device. An empty result means a shipped file is missing.
//
// Nothing here runs unless a converted shader of the kind is registered: the consumers add the text only then, so every other shader is byte for byte unchanged.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::rhi {

// One converted shader. regs / globalRegs: uniform registers of the normal and global blocks; values: that many registers, four floats each.
struct GodotEnvProgram {
    std::string name;
    std::string ns;            // identifier prefix of every global in `source`
    std::string source;        // the emitted HLSL (entries <ns>main_sky | <ns>main_fog | <ns>main_start, <ns>main_process)
    u32 regs = 0, globalRegs = 0;
    bool hasStart = false, hasProcess = false;
    std::vector<float> values;
};

inline constexpr u32 kGodotFogShaderMax = 4;       // fog shaders in one frame (a shader word in a fog volume's shape field names one, 1-based)
inline constexpr u32 kGodotFogShaderRegs = 16;     // registers reserved per fog shader in the froxel constants

// The library and the converted source with the SH1 names renamed (gd_* -> gde_*, Gd* -> GdE*, gdSlot* -> gdeSlot*, GD_SMP -> GDE_SMP, AVER_GD_LIBRARY -> AVER_GDE_LIBRARY, ...)
// so the program can share a translation unit with a converted spatial shader. Exposed for the tests.
std::string godotEnvRename(std::string text);

// The text spliced at kGodotSkyMarker in the shared prelude. The prelude also needs `#define AVER_GD_SKY 1` (a variant define) to route the sky functions to it.
std::string godotSkyProgram(const GodotEnvProgram& p);
inline constexpr const char* kGodotSkyMarker = "//@AVER_GD_SKY_SPLICE@";
// Returns `prelude` with the marker line replaced by `program` (unchanged when `program` is empty or the marker is absent).
std::string spliceGodotSky(std::string_view prelude, const std::string& program);

// Added after the shared prelude and before voxi_froxel.hlsl (variant AVER_FOG_VOLUMES=1;AVER_FOG_SHADERS=1). Defines AVER_GD_FOG_COUNT.
std::string godotFogProgram(const std::vector<const GodotEnvProgram*>& shaders);

// Added after the shared prelude and before particles_sim.hlsl (variant AVER_GD_PARTICLES=1). Defines AVER_GD_HAS_START / AVER_GD_HAS_PROCESS.
std::string godotParticlesProgram(const GodotEnvProgram& p);

// The register table as a static float4 array initialiser body ("float4(a, b, c, d), ...") for a program's values (normal block, then global block).
std::string godotEnvRegisterInit(const GodotEnvProgram& p, u32 first, u32 count);

}  // namespace aver::rhi
