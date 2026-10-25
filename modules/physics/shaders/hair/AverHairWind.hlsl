// AverHairWind.hlsl -- wind for Jolt's Hair (design section 2.18). Jolt Hair has no wind (Hair.h lists it as missing), so this kernel runs
// in the same compute queue BEFORE Hair::Update's first dispatch and pulls the velocity of every free simulated vertex towards the wind:
//
//     vel += (wind - vel) * k,   k = x / (1 + x),   x = drag * dt * (0.25 + 0.75 * strandFraction)
//
// k is the implicit-Euler form of 1 - exp(-x): the same behaviour without needing exp in the CPU wrapper, and k stays in [0, 1) whatever x
// is. Tips catch more wind than roots. Turbulence is a hash noise of (strand, 8-step block) blended between blocks, like the soft-body wind
// (SoftBodyAero.cpp): no clock and no rand, so the same run twice is bit identical.
//
// ONE SOURCE, TWO COMPILERS. It is written with Jolt's JPH_SHADER_* macros (Shaders/ShaderCore.h), so the RHI host compiles it as HLSL
// through DXC (the macros are overridden by AverHairMacros.hlsli) and HairWindWrapper.cpp compiles it as C++ for the CPU backend
// the way Jolt's own HairWrapper.cpp does. The includes name files flat, as the engine's shader namespace is; the C++ wrapper redirects
// them with AVER_HAIR_STRUCTS_INCLUDE. Defining AVER_HAIR_WIND_BINDINGS_ONLY skips the kernel: the CPU wrapper includes this file a second
// time to build its Bind() switch, exactly as it does Jolt's *Bindings.h files.
//
// Only the velocity buffer is touched (JPH_HairVelocity, Shaders/HairStructs.h), through the bindings below. Roots (the first vertex of each
// strand) and pinned vertices are skipped.
#ifndef AVER_HAIR_STRUCTS_INCLUDE
#define AVER_HAIR_STRUCTS_INCLUDE "HairStructs.h"
#endif
#include AVER_HAIR_STRUCTS_INCLUDE

// Constants of this kernel, in cbuffer order: three uints and a float fill one 16-byte row, a float3 and a float the next (32 bytes in all;
// the host struct in PhysicsHair.cpp mirrors it). Names are prefixed because Jolt's own gContext members share this scope on the CPU.
JPH_SHADER_CONSTANTS_BEGIN(JPH_AverHairWindContext, gWind)
	JPH_SHADER_CONSTANTS_MEMBER(JPH_uint,		WindNumStrands)
	JPH_SHADER_CONSTANTS_MEMBER(JPH_uint,		WindNumVertices)
	JPH_SHADER_CONSTANTS_MEMBER(JPH_uint,		WindStep)			///< updates since the hair was created
	JPH_SHADER_CONSTANTS_MEMBER(JPH_float,		WindDragDt)			///< drag (1/s) times the clamped delta time
	JPH_SHADER_CONSTANTS_MEMBER(JPH_float3,		WindVelocity)		///< m/s in hair-local Jolt space
	JPH_SHADER_CONSTANTS_MEMBER(JPH_float,		WindTurbulence)		///< m/s amplitude
JPH_SHADER_CONSTANTS_END(JPH_AverHairWindContext)

JPH_SHADER_BIND_BEGIN(JPH_AverHairWind)
	JPH_SHADER_BIND_BUFFER(JPH_uint, gVerticesFixed)
	JPH_SHADER_BIND_BUFFER(JPH_uint, gStrandFractions)
	JPH_SHADER_BIND_RW_BUFFER(JPH_HairVelocity, gVelocities)
JPH_SHADER_BIND_END(JPH_AverHairWind)

#ifndef AVER_HAIR_WIND_BINDINGS_ONLY

uint AverWindMix(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

uint AverWindHash(uint a, uint d)
{
	uint h = AverWindMix(a ^ 0x9e3779b9u);
	h = AverWindMix(h ^ (d * 0x27d4eb2fu));
	return h;
}

// [-1, 1)
float AverWindSigned(uint h)
{
	return float(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

float AverWindNoise(uint strand, uint block, uint k, float frac)
{
	float a = AverWindSigned(AverWindHash(strand, block * 3u + k));
	float b = AverWindSigned(AverWindHash(strand, (block + 1u) * 3u + k));
	return a + (b - a) * frac;
}

JPH_SHADER_FUNCTION_BEGIN(void, main, cHairPerVertexBatch, 1, 1)
	JPH_SHADER_PARAM_THREAD_ID(tid)
JPH_SHADER_FUNCTION_END
{
	// Skip the root of each strand, it is fixed
	uint vtx = tid.x + cWindNumStrands;
	if (vtx >= cWindNumVertices)
		return;
	if ((gVerticesFixed[vtx >> 5] & (1u << (vtx & 31u))) != 0u)
		return;

	uint strand = vtx % cWindNumStrands;
	float fraction = float((gStrandFractions[vtx >> 2] >> ((vtx & 3u) << 3)) & 0xffu) * (1.0f / 255.0f);

	float3 wind = cWindVelocity;
	if (cWindTurbulence != 0.0f)
	{
		uint block = cWindStep >> 3;
		float frac = float(cWindStep & 7u) * 0.125f;
		wind += float3(AverWindNoise(strand, block, 0u, frac), AverWindNoise(strand, block, 1u, frac), AverWindNoise(strand, block, 2u, frac)) * cWindTurbulence;
	}

	float x = cWindDragDt * (0.25f + 0.75f * fraction);
	float k = x / (1.0f + x);

	JPH_HairVelocity vel = gVelocities[vtx];
	vel.mVelocity += (wind - vel.mVelocity) * k;
	gVelocities[vtx] = vel;
}

#endif // AVER_HAIR_WIND_BINDINGS_ONLY
