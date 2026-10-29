// ---- Aver Godot shader library (gdShaderHlslLibrary) -----------------------------------------------------------
// Define AVER_GD_NO_DERIVATIVES before this when compiling a stage without implicit derivatives (vertex, compute): texture() then samples level 0.
#ifndef AVER_GD_LIBRARY
#define AVER_GD_LIBRARY
#ifdef AVER_GD_NO_DERIVATIVES
#define GD_SMP(t, smp, uv) (t).SampleLevel(smp, uv, 0.0)
#define GD_SMPB(t, smp, uv, b) (t).SampleLevel(smp, uv, 0.0)
#else
#define GD_SMP(t, smp, uv) (t).Sample(smp, uv)
#define GD_SMPB(t, smp, uv, b) (t).SampleBias(smp, uv, b)
#endif
#ifdef AVER_GD_SLOT_SAMPLERS
struct GdSampler2D { uint slot; uint m; };
#else
struct GdSampler2D { Texture2D<float4> t; SamplerState s; };
#endif
struct GdISampler2D { Texture2D<int4> t; SamplerState s; };
struct GdUSampler2D { Texture2D<uint4> t; SamplerState s; };
struct GdSampler2DArray { Texture2DArray<float4> t; SamplerState s; };
struct GdISampler2DArray { Texture2DArray<int4> t; SamplerState s; };
struct GdUSampler2DArray { Texture2DArray<uint4> t; SamplerState s; };
struct GdSampler3D { Texture3D<float4> t; SamplerState s; };
struct GdISampler3D { Texture3D<int4> t; SamplerState s; };
struct GdUSampler3D { Texture3D<uint4> t; SamplerState s; };
struct GdSamplerCube { TextureCube<float4> t; SamplerState s; };
struct GdSamplerCubeArray { TextureCubeArray<float4> t; SamplerState s; };
float gd_mod(float x, float y) { return x - y * floor(x / y); }
float2 gd_mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 gd_mod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 gd_mod(float4 x, float4 y) { return x - y * floor(x / y); }
float2 gd_mod(float2 x, float y) { return x - y * floor(x / y); }
float3 gd_mod(float3 x, float y) { return x - y * floor(x / y); }
float4 gd_mod(float4 x, float y) { return x - y * floor(x / y); }
float gd_mix(float a, float b, float t) { return lerp(a, b, t); }
float2 gd_mix(float2 a, float2 b, float2 t) { return lerp(a, b, t); }
float3 gd_mix(float3 a, float3 b, float3 t) { return lerp(a, b, t); }
float4 gd_mix(float4 a, float4 b, float4 t) { return lerp(a, b, t); }
float2 gd_mix(float2 a, float2 b, float t) { return lerp(a, b, (float2)t); }
float3 gd_mix(float3 a, float3 b, float t) { return lerp(a, b, (float3)t); }
float4 gd_mix(float4 a, float4 b, float t) { return lerp(a, b, (float4)t); }
float gd_mix(float a, float b, bool t) { return t ? b : a; }
float2 gd_mix(float2 a, float2 b, bool2 t) { return float2(t.x ? b.x : a.x, t.y ? b.y : a.y); }
float3 gd_mix(float3 a, float3 b, bool3 t) { return float3(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z); }
float4 gd_mix(float4 a, float4 b, bool4 t) { return float4(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z, t.w ? b.w : a.w); }
float gd_sign(float x) { return (float)sign(x); }
int gd_sign(int x) { return sign(x); }
float2 gd_sign(float2 x) { return (float2)sign(x); }
int2 gd_sign(int2 x) { return sign(x); }
float3 gd_sign(float3 x) { return (float3)sign(x); }
int3 gd_sign(int3 x) { return sign(x); }
float4 gd_sign(float4 x) { return (float4)sign(x); }
int4 gd_sign(int4 x) { return sign(x); }
float gd_step(float e, float x) { return step(e, x); }
float2 gd_step(float2 e, float2 x) { return step(e, x); }
float3 gd_step(float3 e, float3 x) { return step(e, x); }
float4 gd_step(float4 e, float4 x) { return step(e, x); }
float2 gd_step(float e, float2 x) { return step((float2)e, x); }
float3 gd_step(float e, float3 x) { return step((float3)e, x); }
float4 gd_step(float e, float4 x) { return step((float4)e, x); }
float gd_smoothstep(float a, float b, float x) { return smoothstep(a, b, x); }
float2 gd_smoothstep(float2 a, float2 b, float2 x) { return smoothstep(a, b, x); }
float3 gd_smoothstep(float3 a, float3 b, float3 x) { return smoothstep(a, b, x); }
float4 gd_smoothstep(float4 a, float4 b, float4 x) { return smoothstep(a, b, x); }
float2 gd_smoothstep(float a, float b, float2 x) { return smoothstep((float2)a, (float2)b, x); }
float3 gd_smoothstep(float a, float b, float3 x) { return smoothstep((float3)a, (float3)b, x); }
float4 gd_smoothstep(float a, float b, float4 x) { return smoothstep((float4)a, (float4)b, x); }
float gd_asinh(float x) { return (float)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float gd_acosh(float x) { return log(x + sqrt(x * x - 1.0)); }
float gd_atanh(float x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float2 gd_asinh(float2 x) { return (float2)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float2 gd_acosh(float2 x) { return log(x + sqrt(x * x - 1.0)); }
float2 gd_atanh(float2 x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float3 gd_asinh(float3 x) { return (float3)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float3 gd_acosh(float3 x) { return log(x + sqrt(x * x - 1.0)); }
float3 gd_atanh(float3 x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float4 gd_asinh(float4 x) { return (float4)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float4 gd_acosh(float4 x) { return log(x + sqrt(x * x - 1.0)); }
float4 gd_atanh(float4 x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float gd_fwidthCoarse(float x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float gd_fwidthFine(float x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
float2 gd_fwidthCoarse(float2 x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float2 gd_fwidthFine(float2 x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
float3 gd_fwidthCoarse(float3 x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float3 gd_fwidthFine(float3 x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
float4 gd_fwidthCoarse(float4 x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float4 gd_fwidthFine(float4 x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
int gd_bitfieldExtract(int v, int off, int bits) { return bits == 0 ? (int)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint gd_bitfieldExtract(uint v, int off, int bits) { return bits == 0 ? (uint)0 : (v << (32 - off - bits)) >> (32 - bits); }
int gd_bitfieldInsert(int b, int v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int)(((uint)b & ~(uint)m) | (((uint)v << off) & (uint)m)); }
uint gd_bitfieldInsert(uint b, uint v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint)m) | ((v << off) & (uint)m); }
int gd_bitCount(int v) { return (int)countbits((uint)v); }
int gd_bitCount(uint v) { return (int)countbits(v); }
int gd_findLSB(int v) { return (int)firstbitlow(v); }
int gd_findLSB(uint v) { return (int)firstbitlow(v); }
int gd_findMSB(int v) { return (int)firstbithigh(v); }
int gd_findMSB(uint v) { return (int)firstbithigh(v); }
int2 gd_bitfieldExtract(int2 v, int off, int bits) { return bits == 0 ? (int2)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint2 gd_bitfieldExtract(uint2 v, int off, int bits) { return bits == 0 ? (uint2)0 : (v << (32 - off - bits)) >> (32 - bits); }
int2 gd_bitfieldInsert(int2 b, int2 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int2)(((uint2)b & ~(uint2)m) | (((uint2)v << off) & (uint2)m)); }
uint2 gd_bitfieldInsert(uint2 b, uint2 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint2)m) | ((v << off) & (uint2)m); }
int2 gd_bitCount(int2 v) { return (int2)countbits((uint2)v); }
int2 gd_bitCount(uint2 v) { return (int2)countbits(v); }
int2 gd_findLSB(int2 v) { return (int2)firstbitlow(v); }
int2 gd_findLSB(uint2 v) { return (int2)firstbitlow(v); }
int2 gd_findMSB(int2 v) { return (int2)firstbithigh(v); }
int2 gd_findMSB(uint2 v) { return (int2)firstbithigh(v); }
int3 gd_bitfieldExtract(int3 v, int off, int bits) { return bits == 0 ? (int3)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint3 gd_bitfieldExtract(uint3 v, int off, int bits) { return bits == 0 ? (uint3)0 : (v << (32 - off - bits)) >> (32 - bits); }
int3 gd_bitfieldInsert(int3 b, int3 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int3)(((uint3)b & ~(uint3)m) | (((uint3)v << off) & (uint3)m)); }
uint3 gd_bitfieldInsert(uint3 b, uint3 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint3)m) | ((v << off) & (uint3)m); }
int3 gd_bitCount(int3 v) { return (int3)countbits((uint3)v); }
int3 gd_bitCount(uint3 v) { return (int3)countbits(v); }
int3 gd_findLSB(int3 v) { return (int3)firstbitlow(v); }
int3 gd_findLSB(uint3 v) { return (int3)firstbitlow(v); }
int3 gd_findMSB(int3 v) { return (int3)firstbithigh(v); }
int3 gd_findMSB(uint3 v) { return (int3)firstbithigh(v); }
int4 gd_bitfieldExtract(int4 v, int off, int bits) { return bits == 0 ? (int4)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint4 gd_bitfieldExtract(uint4 v, int off, int bits) { return bits == 0 ? (uint4)0 : (v << (32 - off - bits)) >> (32 - bits); }
int4 gd_bitfieldInsert(int4 b, int4 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int4)(((uint4)b & ~(uint4)m) | (((uint4)v << off) & (uint4)m)); }
uint4 gd_bitfieldInsert(uint4 b, uint4 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint4)m) | ((v << off) & (uint4)m); }
int4 gd_bitCount(int4 v) { return (int4)countbits((uint4)v); }
int4 gd_bitCount(uint4 v) { return (int4)countbits(v); }
int4 gd_findLSB(int4 v) { return (int4)firstbitlow(v); }
int4 gd_findLSB(uint4 v) { return (int4)firstbitlow(v); }
int4 gd_findMSB(int4 v) { return (int4)firstbithigh(v); }
int4 gd_findMSB(uint4 v) { return (int4)firstbithigh(v); }
uint gd_packHalf2x16(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 gd_unpackHalf2x16(uint p) { return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16)); }
uint gd_packUnorm2x16(float2 v) { uint2 q = (uint2)round(saturate(v) * 65535.0); return q.x | (q.y << 16); }
float2 gd_unpackUnorm2x16(uint p) { return float2(p & 0xFFFFu, p >> 16) / 65535.0; }
uint gd_packSnorm2x16(float2 v) { int2 q = (int2)round(clamp(v, -1.0, 1.0) * 32767.0); return ((uint)q.x & 0xFFFFu) | (((uint)q.y & 0xFFFFu) << 16); }
float2 gd_unpackSnorm2x16(uint p) { int2 q = int2((int)(p << 16) >> 16, (int)p >> 16); return clamp(float2(q) / 32767.0, -1.0, 1.0); }
uint gd_packUnorm4x8(float4 v) { uint4 q = (uint4)round(saturate(v) * 255.0); return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24); }
float4 gd_unpackUnorm4x8(uint p) { return float4(p & 0xFFu, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu, p >> 24) / 255.0; }
uint gd_packSnorm4x8(float4 v) { int4 q = (int4)round(clamp(v, -1.0, 1.0) * 127.0); return ((uint)q.x & 0xFFu) | (((uint)q.y & 0xFFu) << 8) | (((uint)q.z & 0xFFu) << 16) | (((uint)q.w & 0xFFu) << 24); }
float4 gd_unpackSnorm4x8(uint p) { int4 q = int4((int)(p << 24) >> 24, (int)(p << 16) >> 24, (int)(p << 8) >> 24, (int)p >> 24); return clamp(float4(q) / 127.0, -1.0, 1.0); }
float2x2 gd_mat2_diag(float s) { return float2x2(s, 0.0, 0.0, s); }
float3x3 gd_mat3_diag(float s) { return float3x3(s, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, s); }
float4x4 gd_mat4_diag(float s) { return float4x4(s, 0.0, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, 0.0, s); }
float3x3 gd_mat3_from2(float2x2 m) { return float3x3(m[0].x, m[0].y, 0.0, m[1].x, m[1].y, 0.0, 0.0, 0.0, 1.0); }
float4x4 gd_mat4_from2(float2x2 m) { return float4x4(m[0].x, m[0].y, 0.0, 0.0, m[1].x, m[1].y, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0); }
float4x4 gd_mat4_from3(float3x3 m) { return float4x4(m[0].x, m[0].y, m[0].z, 0.0, m[1].x, m[1].y, m[1].z, 0.0, m[2].x, m[2].y, m[2].z, 0.0, 0.0, 0.0, 0.0, 1.0); }
bool gd_eq(float2x2 a, float2x2 b) { return all(a[0] == b[0]) && all(a[1] == b[1]); }
bool gd_eq(float3x3 a, float3x3 b) { return all(a[0] == b[0]) && all(a[1] == b[1]) && all(a[2] == b[2]); }
bool gd_eq(float4x4 a, float4x4 b) { return all(a[0] == b[0]) && all(a[1] == b[1]) && all(a[2] == b[2]) && all(a[3] == b[3]); }
float2x2 gd_outer(float2 c, float2 r) { return float2x2(c * r.x, c * r.y); }
float3x3 gd_outer(float3 c, float3 r) { return float3x3(c * r.x, c * r.y, c * r.z); }
float4x4 gd_outer(float4 c, float4 r) { return float4x4(c * r.x, c * r.y, c * r.z, c * r.w); }
float2x2 gd_inverse(float2x2 m) { float d = m[0][0] * m[1][1] - m[0][1] * m[1][0]; return float2x2(m[1][1], -m[0][1], -m[1][0], m[0][0]) / d; }
float3x3 gd_inverse(float3x3 m) {
    float3 c0 = cross(m[1], m[2]), c1 = cross(m[2], m[0]), c2 = cross(m[0], m[1]);
    return transpose(float3x3(c0, c1, c2)) / dot(m[0], c0);
}
float gd_det3(float a, float b, float c, float d, float e, float f, float g, float h, float i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); }
float4x4 gd_inverse(float4x4 m) {
    float4x4 c;
    [unroll] for (int r = 0; r < 4; ++r) {
        [unroll] for (int k = 0; k < 4; ++k) {
            float v[9];
            int n = 0;
            [unroll] for (int i = 0; i < 4; ++i) {
                [unroll] for (int j = 0; j < 4; ++j) {
                    if (i != r && j != k) { v[n] = m[i][j]; n = n + 1; }
                }
            }
            float minor = gd_det3(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
            c[r][k] = ((r + k) & 1) ? -minor : minor;
        }
    }
    return transpose(c) / dot(m[0], c[0]);
}
#ifdef AVER_GD_SLOT_SAMPLERS
int2 gd_textureSize(GdSampler2D s, int lod) { return gdSlotSize(s.slot, lod); }
int gd_textureQueryLevels(GdSampler2D s) { return gdSlotLevels(s.slot); }
float4 gd_texture(GdSampler2D s, float2 uv) { return gdSlotSample(s.slot, s.m, uv, 0.0); }
float4 gd_texture(GdSampler2D s, float2 uv, float bias) { return gdSlotSample(s.slot, s.m, uv, bias); }
float4 gd_textureLod(GdSampler2D s, float2 uv, float lod) { return gdSlotSampleLod(s.slot, s.m, uv, lod); }
float4 gd_textureGrad(GdSampler2D s, float2 uv, float2 dx, float2 dy) { return gdSlotSampleGrad(s.slot, s.m, uv, dx, dy); }
float2 gd_textureQueryLod(GdSampler2D s, float2 uv) { return gdSlotQueryLod(s.slot, uv); }
float4 gd_textureProj(GdSampler2D s, float3 p) { return gd_texture(s, p.xy / p.z); }
float4 gd_textureProj(GdSampler2D s, float4 p) { return gd_texture(s, p.xy / p.w); }
float4 gd_textureProj(GdSampler2D s, float3 p, float bias) { return gd_texture(s, p.xy / p.z, bias); }
float4 gd_textureProj(GdSampler2D s, float4 p, float bias) { return gd_texture(s, p.xy / p.w, bias); }
float4 gd_textureProjLod(GdSampler2D s, float3 p, float lod) { return gd_textureLod(s, p.xy / p.z, lod); }
float4 gd_textureProjLod(GdSampler2D s, float4 p, float lod) { return gd_textureLod(s, p.xy / p.w, lod); }
float4 gd_texelFetch(GdSampler2D s, int2 p, int lod) { return gdSlotFetch(s.slot, p, lod); }
float4 gd_textureGather(GdSampler2D s, float2 uv) { return gdSlotGather(s.slot, uv, 0); }
float4 gd_textureGather(GdSampler2D s, float2 uv, int comp) { return gdSlotGather(s.slot, uv, comp); }
int2 gdr_textureSize(GdSampler2D s, int lod) { return gdrSlotSize(s.slot, lod); }
int gdr_textureQueryLevels(GdSampler2D s) { return gdrSlotLevels(s.slot); }
float4 gdr_texture(GdSampler2D s, float2 uv) { return gdrSlotSample(s.slot, s.m, uv, 0.0); }
float4 gdr_texture(GdSampler2D s, float2 uv, float bias) { return gdrSlotSample(s.slot, s.m, uv, bias); }
float4 gdr_textureLod(GdSampler2D s, float2 uv, float lod) { return gdrSlotSampleLod(s.slot, s.m, uv, lod); }
float4 gdr_textureGrad(GdSampler2D s, float2 uv, float2 dx, float2 dy) { return gdrSlotSampleGrad(s.slot, s.m, uv, dx, dy); }
float2 gdr_textureQueryLod(GdSampler2D s, float2 uv) { return gdrSlotQueryLod(s.slot, uv); }
float4 gdr_textureProj(GdSampler2D s, float3 p) { return gdr_texture(s, p.xy / p.z); }
float4 gdr_textureProj(GdSampler2D s, float4 p) { return gdr_texture(s, p.xy / p.w); }
float4 gdr_textureProj(GdSampler2D s, float3 p, float bias) { return gdr_texture(s, p.xy / p.z, bias); }
float4 gdr_textureProj(GdSampler2D s, float4 p, float bias) { return gdr_texture(s, p.xy / p.w, bias); }
float4 gdr_textureProjLod(GdSampler2D s, float3 p, float lod) { return gdr_textureLod(s, p.xy / p.z, lod); }
float4 gdr_textureProjLod(GdSampler2D s, float4 p, float lod) { return gdr_textureLod(s, p.xy / p.w, lod); }
float4 gdr_texelFetch(GdSampler2D s, int2 p, int lod) { return gdrSlotFetch(s.slot, p, lod); }
float4 gdr_textureGather(GdSampler2D s, float2 uv) { return gdrSlotGather(s.slot, uv, 0); }
float4 gdr_textureGather(GdSampler2D s, float2 uv, int comp) { return gdrSlotGather(s.slot, uv, comp); }
#else
int2 gd_textureSize(GdSampler2D s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int gd_textureQueryLevels(GdSampler2D s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
float4 gd_texture(GdSampler2D s, float2 uv) { return GD_SMP(s.t, s.s, uv); }
float4 gd_texture(GdSampler2D s, float2 uv, float bias) { return GD_SMPB(s.t, s.s, uv, bias); }
float4 gd_textureLod(GdSampler2D s, float2 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 gd_textureGrad(GdSampler2D s, float2 uv, float2 dx, float2 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 gd_textureQueryLod(GdSampler2D s, float2 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 gd_textureProj(GdSampler2D s, float3 p) { return gd_texture(s, p.xy / p.z); }
float4 gd_textureProj(GdSampler2D s, float4 p) { return gd_texture(s, p.xy / p.w); }
float4 gd_textureProj(GdSampler2D s, float3 p, float bias) { return gd_texture(s, p.xy / p.z, bias); }
float4 gd_textureProj(GdSampler2D s, float4 p, float bias) { return gd_texture(s, p.xy / p.w, bias); }
float4 gd_textureProjLod(GdSampler2D s, float3 p, float lod) { return gd_textureLod(s, p.xy / p.z, lod); }
float4 gd_textureProjLod(GdSampler2D s, float4 p, float lod) { return gd_textureLod(s, p.xy / p.w, lod); }
float4 gd_texelFetch(GdSampler2D s, int2 p, int lod) { return s.t.Load(int3(p, lod)); }
float4 gd_textureGather(GdSampler2D s, float2 uv) { return s.t.Gather(s.s, uv); }
float4 gd_textureGather(GdSampler2D s, float2 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
#endif
int2 gd_textureSize(GdISampler2D s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int gd_textureQueryLevels(GdISampler2D s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
int4 gd_texture(GdISampler2D s, float2 uv) { int2 sz = gd_textureSize(s, 0); return s.t.Load(int3(clamp((int2)(uv * (float2)sz), int2(0, 0), sz - 1), 0)); }
int4 gd_texture(GdISampler2D s, float2 uv, float bias) { return gd_texture(s, uv); }
int4 gd_textureLod(GdISampler2D s, float2 uv, float lod) { return gd_texture(s, uv); }
float2 gd_textureQueryLod(GdISampler2D s, float2 uv) { return float2(0.0, 0.0); }
int4 gd_textureProj(GdISampler2D s, float3 p) { return gd_texture(s, p.xy / p.z); }
int4 gd_textureProj(GdISampler2D s, float4 p) { return gd_texture(s, p.xy / p.w); }
int4 gd_textureProj(GdISampler2D s, float3 p, float bias) { return gd_texture(s, p.xy / p.z, bias); }
int4 gd_textureProj(GdISampler2D s, float4 p, float bias) { return gd_texture(s, p.xy / p.w, bias); }
int4 gd_textureProjLod(GdISampler2D s, float3 p, float lod) { return gd_textureLod(s, p.xy / p.z, lod); }
int4 gd_textureProjLod(GdISampler2D s, float4 p, float lod) { return gd_textureLod(s, p.xy / p.w, lod); }
int4 gd_texelFetch(GdISampler2D s, int2 p, int lod) { return s.t.Load(int3(p, lod)); }
int4 gd_textureGather(GdISampler2D s, float2 uv) { return s.t.Gather(s.s, uv); }
int4 gd_textureGather(GdISampler2D s, float2 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int2 gd_textureSize(GdUSampler2D s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int gd_textureQueryLevels(GdUSampler2D s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
uint4 gd_texture(GdUSampler2D s, float2 uv) { int2 sz = gd_textureSize(s, 0); return s.t.Load(int3(clamp((int2)(uv * (float2)sz), int2(0, 0), sz - 1), 0)); }
uint4 gd_texture(GdUSampler2D s, float2 uv, float bias) { return gd_texture(s, uv); }
uint4 gd_textureLod(GdUSampler2D s, float2 uv, float lod) { return gd_texture(s, uv); }
float2 gd_textureQueryLod(GdUSampler2D s, float2 uv) { return float2(0.0, 0.0); }
uint4 gd_textureProj(GdUSampler2D s, float3 p) { return gd_texture(s, p.xy / p.z); }
uint4 gd_textureProj(GdUSampler2D s, float4 p) { return gd_texture(s, p.xy / p.w); }
uint4 gd_textureProj(GdUSampler2D s, float3 p, float bias) { return gd_texture(s, p.xy / p.z, bias); }
uint4 gd_textureProj(GdUSampler2D s, float4 p, float bias) { return gd_texture(s, p.xy / p.w, bias); }
uint4 gd_textureProjLod(GdUSampler2D s, float3 p, float lod) { return gd_textureLod(s, p.xy / p.z, lod); }
uint4 gd_textureProjLod(GdUSampler2D s, float4 p, float lod) { return gd_textureLod(s, p.xy / p.w, lod); }
uint4 gd_texelFetch(GdUSampler2D s, int2 p, int lod) { return s.t.Load(int3(p, lod)); }
uint4 gd_textureGather(GdUSampler2D s, float2 uv) { return s.t.Gather(s.s, uv); }
uint4 gd_textureGather(GdUSampler2D s, float2 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 gd_textureSize(GdSampler2DArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int gd_textureQueryLevels(GdSampler2DArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
float4 gd_texture(GdSampler2DArray s, float3 uv) { return GD_SMP(s.t, s.s, uv); }
float4 gd_texture(GdSampler2DArray s, float3 uv, float bias) { return GD_SMPB(s.t, s.s, uv, bias); }
float4 gd_textureLod(GdSampler2DArray s, float3 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 gd_textureGrad(GdSampler2DArray s, float3 uv, float2 dx, float2 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 gd_textureQueryLod(GdSampler2DArray s, float2 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 gd_texelFetch(GdSampler2DArray s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
float4 gd_textureGather(GdSampler2DArray s, float3 uv) { return s.t.Gather(s.s, uv); }
float4 gd_textureGather(GdSampler2DArray s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 gd_textureSize(GdISampler2DArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int gd_textureQueryLevels(GdISampler2DArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
int4 gd_texture(GdISampler2DArray s, float3 uv) { int3 sz = gd_textureSize(s, 0); int2 p = clamp((int2)(uv.xy * (float2)sz.xy), int2(0, 0), sz.xy - 1); return s.t.Load(int4(p, clamp((int)round(uv.z), 0, sz.z - 1), 0)); }
int4 gd_texture(GdISampler2DArray s, float3 uv, float bias) { return gd_texture(s, uv); }
int4 gd_textureLod(GdISampler2DArray s, float3 uv, float lod) { return gd_texture(s, uv); }
float2 gd_textureQueryLod(GdISampler2DArray s, float2 uv) { return float2(0.0, 0.0); }
int4 gd_texelFetch(GdISampler2DArray s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int4 gd_textureGather(GdISampler2DArray s, float3 uv) { return s.t.Gather(s.s, uv); }
int4 gd_textureGather(GdISampler2DArray s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 gd_textureSize(GdUSampler2DArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int gd_textureQueryLevels(GdUSampler2DArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
uint4 gd_texture(GdUSampler2DArray s, float3 uv) { int3 sz = gd_textureSize(s, 0); int2 p = clamp((int2)(uv.xy * (float2)sz.xy), int2(0, 0), sz.xy - 1); return s.t.Load(int4(p, clamp((int)round(uv.z), 0, sz.z - 1), 0)); }
uint4 gd_texture(GdUSampler2DArray s, float3 uv, float bias) { return gd_texture(s, uv); }
uint4 gd_textureLod(GdUSampler2DArray s, float3 uv, float lod) { return gd_texture(s, uv); }
float2 gd_textureQueryLod(GdUSampler2DArray s, float2 uv) { return float2(0.0, 0.0); }
uint4 gd_texelFetch(GdUSampler2DArray s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
uint4 gd_textureGather(GdUSampler2DArray s, float3 uv) { return s.t.Gather(s.s, uv); }
uint4 gd_textureGather(GdUSampler2DArray s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 gd_textureSize(GdSampler3D s, int lod) { uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d); }
int gd_textureQueryLevels(GdSampler3D s) { uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l; }
float4 gd_texture(GdSampler3D s, float3 uv) { return GD_SMP(s.t, s.s, uv); }
float4 gd_texture(GdSampler3D s, float3 uv, float bias) { return GD_SMPB(s.t, s.s, uv, bias); }
float4 gd_textureLod(GdSampler3D s, float3 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 gd_textureGrad(GdSampler3D s, float3 uv, float3 dx, float3 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 gd_textureQueryLod(GdSampler3D s, float3 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 gd_textureProj(GdSampler3D s, float4 p) { return gd_texture(s, p.xyz / p.w); }
float4 gd_textureProj(GdSampler3D s, float4 p, float bias) { return gd_texture(s, p.xyz / p.w, bias); }
float4 gd_textureProjLod(GdSampler3D s, float4 p, float lod) { return gd_textureLod(s, p.xyz / p.w, lod); }
float4 gd_texelFetch(GdSampler3D s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int3 gd_textureSize(GdISampler3D s, int lod) { uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d); }
int gd_textureQueryLevels(GdISampler3D s) { uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l; }
int4 gd_texture(GdISampler3D s, float3 uv) { int3 sz = gd_textureSize(s, 0); return s.t.Load(int4(clamp((int3)(uv * (float3)sz), int3(0, 0, 0), sz - 1), 0)); }
int4 gd_texture(GdISampler3D s, float3 uv, float bias) { return gd_texture(s, uv); }
int4 gd_textureLod(GdISampler3D s, float3 uv, float lod) { return gd_texture(s, uv); }
float2 gd_textureQueryLod(GdISampler3D s, float3 uv) { return float2(0.0, 0.0); }
int4 gd_textureProj(GdISampler3D s, float4 p) { return gd_texture(s, p.xyz / p.w); }
int4 gd_textureProj(GdISampler3D s, float4 p, float bias) { return gd_texture(s, p.xyz / p.w, bias); }
int4 gd_textureProjLod(GdISampler3D s, float4 p, float lod) { return gd_textureLod(s, p.xyz / p.w, lod); }
int4 gd_texelFetch(GdISampler3D s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int3 gd_textureSize(GdUSampler3D s, int lod) { uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d); }
int gd_textureQueryLevels(GdUSampler3D s) { uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l; }
uint4 gd_texture(GdUSampler3D s, float3 uv) { int3 sz = gd_textureSize(s, 0); return s.t.Load(int4(clamp((int3)(uv * (float3)sz), int3(0, 0, 0), sz - 1), 0)); }
uint4 gd_texture(GdUSampler3D s, float3 uv, float bias) { return gd_texture(s, uv); }
uint4 gd_textureLod(GdUSampler3D s, float3 uv, float lod) { return gd_texture(s, uv); }
float2 gd_textureQueryLod(GdUSampler3D s, float3 uv) { return float2(0.0, 0.0); }
uint4 gd_textureProj(GdUSampler3D s, float4 p) { return gd_texture(s, p.xyz / p.w); }
uint4 gd_textureProj(GdUSampler3D s, float4 p, float bias) { return gd_texture(s, p.xyz / p.w, bias); }
uint4 gd_textureProjLod(GdUSampler3D s, float4 p, float lod) { return gd_textureLod(s, p.xyz / p.w, lod); }
uint4 gd_texelFetch(GdUSampler3D s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int2 gd_textureSize(GdSamplerCube s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int gd_textureQueryLevels(GdSamplerCube s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
float4 gd_texture(GdSamplerCube s, float3 uv) { return GD_SMP(s.t, s.s, uv); }
float4 gd_texture(GdSamplerCube s, float3 uv, float bias) { return GD_SMPB(s.t, s.s, uv, bias); }
float4 gd_textureLod(GdSamplerCube s, float3 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 gd_textureGrad(GdSamplerCube s, float3 uv, float3 dx, float3 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 gd_textureQueryLod(GdSamplerCube s, float3 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 gd_textureGather(GdSamplerCube s, float3 uv) { return s.t.Gather(s.s, uv); }
float4 gd_textureGather(GdSamplerCube s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 gd_textureSize(GdSamplerCubeArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int gd_textureQueryLevels(GdSamplerCubeArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
float4 gd_texture(GdSamplerCubeArray s, float4 uv) { return GD_SMP(s.t, s.s, uv); }
float4 gd_texture(GdSamplerCubeArray s, float4 uv, float bias) { return GD_SMPB(s.t, s.s, uv, bias); }
float4 gd_textureLod(GdSamplerCubeArray s, float4 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 gd_textureGrad(GdSamplerCubeArray s, float4 uv, float3 dx, float3 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 gd_textureQueryLod(GdSamplerCubeArray s, float3 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv.xyz), s.t.CalculateLevelOfDetailUnclamped(s.s, uv.xyz)); }
float4 gd_textureGather(GdSamplerCubeArray s, float4 uv) { return s.t.Gather(s.s, uv); }
float4 gd_textureGather(GdSamplerCubeArray s, float4 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
#endif
