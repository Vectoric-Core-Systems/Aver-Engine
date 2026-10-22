// HairWindWrapper.cpp -- AverHairWind.hlsl compiled as C++ for Jolt's CPU compute backend, the way Jolt's own Shaders/HairWrapper.cpp does
// it for its fifteen kernels. It is what lets HairTest check wind headless, and what a headless server runs.
//
// The shader file is included TWICE, as Jolt includes its kernel and then its *Bindings.h: once inside the wrapper class (the kernel becomes
// Main), once inside Bind() with the binding macros redefined (the buffers become switch cases). AVER_HAIR_WIND_BINDINGS_ONLY makes the second
// pass skip the kernel. AVER_HAIR_STRUCTS_INCLUDE points the shader's flat include at the vendored header.
#include <Jolt/Jolt.h>

#ifdef JPH_USE_CPU_COMPUTE

#define AVER_HAIR_STRUCTS_INCLUDE <Jolt/Shaders/HairStructs.h>

#define JPH_SHADER_NAME AverHairWind
#include <Jolt/Compute/CPU/WrapShaderBegin.h>
#include "../shaders/hair/AverHairWind.hlsl"
#include <Jolt/Compute/CPU/WrapShaderBindings.h>
#define AVER_HAIR_WIND_BINDINGS_ONLY
#include "../shaders/hair/AverHairWind.hlsl"
#undef AVER_HAIR_WIND_BINDINGS_ONLY
#include <Jolt/Compute/CPU/WrapShaderEnd.h>

JPH_NAMESPACE_BEGIN

// Registers the wind kernel beside the fifteen HairRegisterShaders registers.
void JPH_EXPORT AverHairRegisterShaders(ComputeSystemCPU* inComputeSystem)
{
	JPH_REGISTER_SHADER(inComputeSystem, AverHairWind);
}

JPH_NAMESPACE_END

#endif // JPH_USE_CPU_COMPUTE
