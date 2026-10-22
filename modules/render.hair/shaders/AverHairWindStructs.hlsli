// AverHairWindStructs.hlsli -- the two things AverHairWind.hlsl needs from Jolt's HairStructs.h, and nothing else.
//
// HairStructs.h also declares gContext and gIterationContext (b3 / b4 in every Jolt kernel). The wind kernel declares its own constants,
// gWind, at b3, so it must not pull those in: two constant blocks on one register is a compile error. The layout of JPH_HairVelocity below is
// the one HairStructs.h declares (two float3, 24 bytes) and tests/render.hair/HairRegsTest compares the member list textually.
// Included through AVER_HAIR_STRUCTS_INCLUDE by the wrapper AverHairWindCS.hlsl; the CPU build includes the vendored header instead.
#ifndef AVER_HAIR_WIND_STRUCTS_HLSLI
#define AVER_HAIR_WIND_STRUCTS_HLSLI

JPH_SHADER_CONSTANT(int, cHairPerVertexBatch, 64)

JPH_SHADER_STRUCT_BEGIN(JPH_HairVelocity)
	JPH_SHADER_STRUCT_MEMBER(JPH_float3,		Velocity)
	JPH_SHADER_STRUCT_MEMBER(JPH_float3,		AngularVelocity)
JPH_SHADER_STRUCT_END(JPH_HairVelocity)

#endif // AVER_HAIR_WIND_STRUCTS_HLSLI
