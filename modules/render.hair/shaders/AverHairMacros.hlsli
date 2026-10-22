// AverHairMacros.hlsli -- the macro set Jolt's hair kernels are written against, with explicit registers.
//
// Jolt's Shaders/ShaderCore.h declares a buffer as a bare `StructuredBuffer<T> name;` and a constant block as a bare `cbuffer name`, because its own
// compute backends find the register by reflection. Aver's RHI binds a compute shader's buffers as CONTIGUOUS slot ranges (t0.., u0..) and its
// constants at b3 / b4, so each kernel's wrapper (AverJolt<Name>.hlsl) defines JPH_SHADER_OVERRIDE_MACROS, which makes ShaderCore.h skip its own
// block, and includes THIS file instead. It is the HLSL branch of ShaderCore.h copied once with exactly one change: the three declarations that
// carry a binding add `: register(AVER_REG_<name>)`. The register macros come from the generated AverJolt<Name>.regs.hlsli
// (scripts/gen-hair-shader-regs.py); tests/render.hair/HairRegsTest re-derives them from Jolt's *Bindings.h and checks this file defines every
// macro the HLSL branch of ShaderCore.h does, so a Jolt update that moves a binding or adds a macro fails a test instead of mis-binding silently.
//
// Derived from Jolt Physics' Shaders/ShaderCore.h (MIT, SPDX-FileCopyrightText: 2025 Jorrit Rouwe). The notice ships through THIRD-PARTY-NOTICES.txt.
#ifndef AVER_HAIR_MACROS_HLSLI
#define AVER_HAIR_MACROS_HLSLI

#ifndef JPH_SHADER_OVERRIDE_MACROS
#define JPH_SHADER_OVERRIDE_MACROS
#endif

#define JPH_SUPPRESS_WARNING_PUSH
#define JPH_SUPPRESS_WARNING_POP
#define JPH_SUPPRESS_WARNINGS

typedef float JPH_float;
typedef float3 JPH_float3;
typedef float4 JPH_float4;
typedef uint JPH_uint;
typedef uint3 JPH_uint3;
typedef uint4 JPH_uint4;
typedef int JPH_int;
typedef int3 JPH_int3;
typedef int4 JPH_int4;
typedef float4 JPH_Quat;        // xyz = imaginary part, w = real part
typedef float4 JPH_Plane;       // xyz = normal, w = constant
typedef float4 JPH_Mat44[4];    // matrix, column major

#define JPH_SHADER_CONSTANT(type, name, value)  static const type name = value;

#define JPH_SHADER_CONSTANTS_BEGIN(type, name)  cbuffer name : register(AVER_REG_##name) {
#define JPH_SHADER_CONSTANTS_MEMBER(type, name) type c##name;
#define JPH_SHADER_CONSTANTS_END(type)          };

#define JPH_SHADER_FUNCTION_BEGIN(return_type, name, group_size_x, group_size_y, group_size_z) \
    [numthreads(group_size_x, group_size_y, group_size_z)] \
    return_type name(
#define JPH_SHADER_PARAM_THREAD_ID(name)        uint3 name : SV_DispatchThreadID
#define JPH_SHADER_FUNCTION_END                 )

#define JPH_SHADER_BUFFER(type)                 StructuredBuffer<type>
#define JPH_SHADER_RW_BUFFER(type)              RWStructuredBuffer<type>

#define JPH_SHADER_BIND_BEGIN(name)
#define JPH_SHADER_BIND_END(name)
#define JPH_SHADER_BIND_BUFFER(type, name)      JPH_SHADER_BUFFER(type) name : register(AVER_REG_##name);
#define JPH_SHADER_BIND_RW_BUFFER(type, name)   JPH_SHADER_RW_BUFFER(type) name : register(AVER_REG_##name);

#define JPH_AtomicAdd                           InterlockedAdd

// The part of ShaderCore.h that sits outside its CPU / HLSL split.
#define JPH_SHADER_STRUCT_BEGIN(name)           struct name {
#define JPH_SHADER_STRUCT_MEMBER(type, name)    type m##name;
#define JPH_SHADER_STRUCT_END(name)             };

#define JPH_IN(type)                            in type
#define JPH_OUT(type)                           out type
#define JPH_IN_OUT(type)                        in out type

#endif // AVER_HAIR_MACROS_HLSLI
