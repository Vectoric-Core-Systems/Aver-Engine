// ---- Aver embedded shader library (gdShaderHlslLibrary) --------------------------------------------------------
// Define AVER_CUSTOM_NO_DERIVATIVES before this when compiling a stage without implicit derivatives (vertex, compute): texture() then samples level 0.
#ifndef AVER_CUSTOM_LIBRARY
#define AVER_CUSTOM_LIBRARY
#ifdef AVER_CUSTOM_NO_DERIVATIVES
#define EMB_SMP(t, smp, uv) (t).SampleLevel(smp, uv, 0.0)
#define EMB_SMPB(t, smp, uv, b) (t).SampleLevel(smp, uv, 0.0)
#else
#define EMB_SMP(t, smp, uv) (t).Sample(smp, uv)
#define EMB_SMPB(t, smp, uv, b) (t).SampleBias(smp, uv, b)
#endif
#ifdef AVER_CUSTOM_SLOT_SAMPLERS
struct EmbSampler2D { uint slot; uint m; };
#else
struct EmbSampler2D { Texture2D<float4> t; SamplerState s; };
#endif
struct EmbISampler2D { Texture2D<int4> t; SamplerState s; };
struct EmbUSampler2D { Texture2D<uint4> t; SamplerState s; };
struct EmbSampler2DArray { Texture2DArray<float4> t; SamplerState s; };
struct EmbISampler2DArray { Texture2DArray<int4> t; SamplerState s; };
struct EmbUSampler2DArray { Texture2DArray<uint4> t; SamplerState s; };
struct EmbSampler3D { Texture3D<float4> t; SamplerState s; };
struct EmbISampler3D { Texture3D<int4> t; SamplerState s; };
struct EmbUSampler3D { Texture3D<uint4> t; SamplerState s; };
struct EmbSamplerCube { TextureCube<float4> t; SamplerState s; };
struct EmbSamplerCubeArray { TextureCubeArray<float4> t; SamplerState s; };
float emb_mod(float x, float y) { return x - y * floor(x / y); }
float2 emb_mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 emb_mod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 emb_mod(float4 x, float4 y) { return x - y * floor(x / y); }
float2 emb_mod(float2 x, float y) { return x - y * floor(x / y); }
float3 emb_mod(float3 x, float y) { return x - y * floor(x / y); }
float4 emb_mod(float4 x, float y) { return x - y * floor(x / y); }
float emb_mix(float a, float b, float t) { return lerp(a, b, t); }
float2 emb_mix(float2 a, float2 b, float2 t) { return lerp(a, b, t); }
float3 emb_mix(float3 a, float3 b, float3 t) { return lerp(a, b, t); }
float4 emb_mix(float4 a, float4 b, float4 t) { return lerp(a, b, t); }
float2 emb_mix(float2 a, float2 b, float t) { return lerp(a, b, (float2)t); }
float3 emb_mix(float3 a, float3 b, float t) { return lerp(a, b, (float3)t); }
float4 emb_mix(float4 a, float4 b, float t) { return lerp(a, b, (float4)t); }
float emb_mix(float a, float b, bool t) { return t ? b : a; }
float2 emb_mix(float2 a, float2 b, bool2 t) { return float2(t.x ? b.x : a.x, t.y ? b.y : a.y); }
float3 emb_mix(float3 a, float3 b, bool3 t) { return float3(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z); }
float4 emb_mix(float4 a, float4 b, bool4 t) { return float4(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z, t.w ? b.w : a.w); }
float emb_sign(float x) { return (float)sign(x); }
int emb_sign(int x) { return sign(x); }
float2 emb_sign(float2 x) { return (float2)sign(x); }
int2 emb_sign(int2 x) { return sign(x); }
float3 emb_sign(float3 x) { return (float3)sign(x); }
int3 emb_sign(int3 x) { return sign(x); }
float4 emb_sign(float4 x) { return (float4)sign(x); }
int4 emb_sign(int4 x) { return sign(x); }
float emb_step(float e, float x) { return step(e, x); }
float2 emb_step(float2 e, float2 x) { return step(e, x); }
float3 emb_step(float3 e, float3 x) { return step(e, x); }
float4 emb_step(float4 e, float4 x) { return step(e, x); }
float2 emb_step(float e, float2 x) { return step((float2)e, x); }
float3 emb_step(float e, float3 x) { return step((float3)e, x); }
float4 emb_step(float e, float4 x) { return step((float4)e, x); }
float emb_smoothstep(float a, float b, float x) { return smoothstep(a, b, x); }
float2 emb_smoothstep(float2 a, float2 b, float2 x) { return smoothstep(a, b, x); }
float3 emb_smoothstep(float3 a, float3 b, float3 x) { return smoothstep(a, b, x); }
float4 emb_smoothstep(float4 a, float4 b, float4 x) { return smoothstep(a, b, x); }
float2 emb_smoothstep(float a, float b, float2 x) { return smoothstep((float2)a, (float2)b, x); }
float3 emb_smoothstep(float a, float b, float3 x) { return smoothstep((float3)a, (float3)b, x); }
float4 emb_smoothstep(float a, float b, float4 x) { return smoothstep((float4)a, (float4)b, x); }
float emb_asinh(float x) { return (float)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float emb_acosh(float x) { return log(x + sqrt(x * x - 1.0)); }
float emb_atanh(float x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float2 emb_asinh(float2 x) { return (float2)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float2 emb_acosh(float2 x) { return log(x + sqrt(x * x - 1.0)); }
float2 emb_atanh(float2 x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float3 emb_asinh(float3 x) { return (float3)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float3 emb_acosh(float3 x) { return log(x + sqrt(x * x - 1.0)); }
float3 emb_atanh(float3 x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float4 emb_asinh(float4 x) { return (float4)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float4 emb_acosh(float4 x) { return log(x + sqrt(x * x - 1.0)); }
float4 emb_atanh(float4 x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float emb_fwidthCoarse(float x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float emb_fwidthFine(float x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
float2 emb_fwidthCoarse(float2 x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float2 emb_fwidthFine(float2 x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
float3 emb_fwidthCoarse(float3 x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float3 emb_fwidthFine(float3 x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
float4 emb_fwidthCoarse(float4 x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }
float4 emb_fwidthFine(float4 x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }
int emb_bitfieldExtract(int v, int off, int bits) { return bits == 0 ? (int)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint emb_bitfieldExtract(uint v, int off, int bits) { return bits == 0 ? (uint)0 : (v << (32 - off - bits)) >> (32 - bits); }
int emb_bitfieldInsert(int b, int v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int)(((uint)b & ~(uint)m) | (((uint)v << off) & (uint)m)); }
uint emb_bitfieldInsert(uint b, uint v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint)m) | ((v << off) & (uint)m); }
int emb_bitCount(int v) { return (int)countbits((uint)v); }
int emb_bitCount(uint v) { return (int)countbits(v); }
int emb_findLSB(int v) { return (int)firstbitlow(v); }
int emb_findLSB(uint v) { return (int)firstbitlow(v); }
int emb_findMSB(int v) { return (int)firstbithigh(v); }
int emb_findMSB(uint v) { return (int)firstbithigh(v); }
int2 emb_bitfieldExtract(int2 v, int off, int bits) { return bits == 0 ? (int2)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint2 emb_bitfieldExtract(uint2 v, int off, int bits) { return bits == 0 ? (uint2)0 : (v << (32 - off - bits)) >> (32 - bits); }
int2 emb_bitfieldInsert(int2 b, int2 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int2)(((uint2)b & ~(uint2)m) | (((uint2)v << off) & (uint2)m)); }
uint2 emb_bitfieldInsert(uint2 b, uint2 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint2)m) | ((v << off) & (uint2)m); }
int2 emb_bitCount(int2 v) { return (int2)countbits((uint2)v); }
int2 emb_bitCount(uint2 v) { return (int2)countbits(v); }
int2 emb_findLSB(int2 v) { return (int2)firstbitlow(v); }
int2 emb_findLSB(uint2 v) { return (int2)firstbitlow(v); }
int2 emb_findMSB(int2 v) { return (int2)firstbithigh(v); }
int2 emb_findMSB(uint2 v) { return (int2)firstbithigh(v); }
int3 emb_bitfieldExtract(int3 v, int off, int bits) { return bits == 0 ? (int3)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint3 emb_bitfieldExtract(uint3 v, int off, int bits) { return bits == 0 ? (uint3)0 : (v << (32 - off - bits)) >> (32 - bits); }
int3 emb_bitfieldInsert(int3 b, int3 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int3)(((uint3)b & ~(uint3)m) | (((uint3)v << off) & (uint3)m)); }
uint3 emb_bitfieldInsert(uint3 b, uint3 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint3)m) | ((v << off) & (uint3)m); }
int3 emb_bitCount(int3 v) { return (int3)countbits((uint3)v); }
int3 emb_bitCount(uint3 v) { return (int3)countbits(v); }
int3 emb_findLSB(int3 v) { return (int3)firstbitlow(v); }
int3 emb_findLSB(uint3 v) { return (int3)firstbitlow(v); }
int3 emb_findMSB(int3 v) { return (int3)firstbithigh(v); }
int3 emb_findMSB(uint3 v) { return (int3)firstbithigh(v); }
int4 emb_bitfieldExtract(int4 v, int off, int bits) { return bits == 0 ? (int4)0 : (v << (32 - off - bits)) >> (32 - bits); }
uint4 emb_bitfieldExtract(uint4 v, int off, int bits) { return bits == 0 ? (uint4)0 : (v << (32 - off - bits)) >> (32 - bits); }
int4 emb_bitfieldInsert(int4 b, int4 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (int4)(((uint4)b & ~(uint4)m) | (((uint4)v << off) & (uint4)m)); }
uint4 emb_bitfieldInsert(uint4 b, uint4 v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~(uint4)m) | ((v << off) & (uint4)m); }
int4 emb_bitCount(int4 v) { return (int4)countbits((uint4)v); }
int4 emb_bitCount(uint4 v) { return (int4)countbits(v); }
int4 emb_findLSB(int4 v) { return (int4)firstbitlow(v); }
int4 emb_findLSB(uint4 v) { return (int4)firstbitlow(v); }
int4 emb_findMSB(int4 v) { return (int4)firstbithigh(v); }
int4 emb_findMSB(uint4 v) { return (int4)firstbithigh(v); }
uint emb_packHalf2x16(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 emb_unpackHalf2x16(uint p) { return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16)); }
uint emb_packUnorm2x16(float2 v) { uint2 q = (uint2)round(saturate(v) * 65535.0); return q.x | (q.y << 16); }
float2 emb_unpackUnorm2x16(uint p) { return float2(p & 0xFFFFu, p >> 16) / 65535.0; }
uint emb_packSnorm2x16(float2 v) { int2 q = (int2)round(clamp(v, -1.0, 1.0) * 32767.0); return ((uint)q.x & 0xFFFFu) | (((uint)q.y & 0xFFFFu) << 16); }
float2 emb_unpackSnorm2x16(uint p) { int2 q = int2((int)(p << 16) >> 16, (int)p >> 16); return clamp(float2(q) / 32767.0, -1.0, 1.0); }
uint emb_packUnorm4x8(float4 v) { uint4 q = (uint4)round(saturate(v) * 255.0); return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24); }
float4 emb_unpackUnorm4x8(uint p) { return float4(p & 0xFFu, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu, p >> 24) / 255.0; }
uint emb_packSnorm4x8(float4 v) { int4 q = (int4)round(clamp(v, -1.0, 1.0) * 127.0); return ((uint)q.x & 0xFFu) | (((uint)q.y & 0xFFu) << 8) | (((uint)q.z & 0xFFu) << 16) | (((uint)q.w & 0xFFu) << 24); }
float4 emb_unpackSnorm4x8(uint p) { int4 q = int4((int)(p << 24) >> 24, (int)(p << 16) >> 24, (int)(p << 8) >> 24, (int)p >> 24); return clamp(float4(q) / 127.0, -1.0, 1.0); }
float2x2 emb_mat2_diag(float s) { return float2x2(s, 0.0, 0.0, s); }
float3x3 emb_mat3_diag(float s) { return float3x3(s, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, s); }
float4x4 emb_mat4_diag(float s) { return float4x4(s, 0.0, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, 0.0, s); }
float3x3 emb_mat3_from2(float2x2 m) { return float3x3(m[0].x, m[0].y, 0.0, m[1].x, m[1].y, 0.0, 0.0, 0.0, 1.0); }
float4x4 emb_mat4_from2(float2x2 m) { return float4x4(m[0].x, m[0].y, 0.0, 0.0, m[1].x, m[1].y, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0); }
float4x4 emb_mat4_from3(float3x3 m) { return float4x4(m[0].x, m[0].y, m[0].z, 0.0, m[1].x, m[1].y, m[1].z, 0.0, m[2].x, m[2].y, m[2].z, 0.0, 0.0, 0.0, 0.0, 1.0); }
bool emb_eq(float2x2 a, float2x2 b) { return all(a[0] == b[0]) && all(a[1] == b[1]); }
bool emb_eq(float3x3 a, float3x3 b) { return all(a[0] == b[0]) && all(a[1] == b[1]) && all(a[2] == b[2]); }
bool emb_eq(float4x4 a, float4x4 b) { return all(a[0] == b[0]) && all(a[1] == b[1]) && all(a[2] == b[2]) && all(a[3] == b[3]); }
float2x2 emb_outer(float2 c, float2 r) { return float2x2(c * r.x, c * r.y); }
float3x3 emb_outer(float3 c, float3 r) { return float3x3(c * r.x, c * r.y, c * r.z); }
float4x4 emb_outer(float4 c, float4 r) { return float4x4(c * r.x, c * r.y, c * r.z, c * r.w); }
float2x2 emb_inverse(float2x2 m) { float d = m[0][0] * m[1][1] - m[0][1] * m[1][0]; return float2x2(m[1][1], -m[0][1], -m[1][0], m[0][0]) / d; }
float3x3 emb_inverse(float3x3 m) {
    float3 c0 = cross(m[1], m[2]), c1 = cross(m[2], m[0]), c2 = cross(m[0], m[1]);
    return transpose(float3x3(c0, c1, c2)) / dot(m[0], c0);
}
float emb_det3(float a, float b, float c, float d, float e, float f, float g, float h, float i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); }
float4x4 emb_inverse(float4x4 m) {
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
            float minor = emb_det3(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
            c[r][k] = ((r + k) & 1) ? -minor : minor;
        }
    }
    return transpose(c) / dot(m[0], c[0]);
}
#ifdef AVER_CUSTOM_SLOT_SAMPLERS
int2 emb_textureSize(EmbSampler2D s, int lod) { return embSlotSize(s.slot, lod); }
int emb_textureQueryLevels(EmbSampler2D s) { return embSlotLevels(s.slot); }
float4 emb_texture(EmbSampler2D s, float2 uv) { return embSlotSample(s.slot, s.m, uv, 0.0); }
float4 emb_texture(EmbSampler2D s, float2 uv, float bias) { return embSlotSample(s.slot, s.m, uv, bias); }
float4 emb_textureLod(EmbSampler2D s, float2 uv, float lod) { return embSlotSampleLod(s.slot, s.m, uv, lod); }
float4 emb_textureGrad(EmbSampler2D s, float2 uv, float2 dx, float2 dy) { return embSlotSampleGrad(s.slot, s.m, uv, dx, dy); }
float2 emb_textureQueryLod(EmbSampler2D s, float2 uv) { return embSlotQueryLod(s.slot, uv); }
float4 emb_textureProj(EmbSampler2D s, float3 p) { return emb_texture(s, p.xy / p.z); }
float4 emb_textureProj(EmbSampler2D s, float4 p) { return emb_texture(s, p.xy / p.w); }
float4 emb_textureProj(EmbSampler2D s, float3 p, float bias) { return emb_texture(s, p.xy / p.z, bias); }
float4 emb_textureProj(EmbSampler2D s, float4 p, float bias) { return emb_texture(s, p.xy / p.w, bias); }
float4 emb_textureProjLod(EmbSampler2D s, float3 p, float lod) { return emb_textureLod(s, p.xy / p.z, lod); }
float4 emb_textureProjLod(EmbSampler2D s, float4 p, float lod) { return emb_textureLod(s, p.xy / p.w, lod); }
float4 emb_texelFetch(EmbSampler2D s, int2 p, int lod) { return embSlotFetch(s.slot, p, lod); }
float4 emb_textureGather(EmbSampler2D s, float2 uv) { return embSlotGather(s.slot, uv, 0); }
float4 emb_textureGather(EmbSampler2D s, float2 uv, int comp) { return embSlotGather(s.slot, uv, comp); }
int2 embr_textureSize(EmbSampler2D s, int lod) { return embrSlotSize(s.slot, lod); }
int embr_textureQueryLevels(EmbSampler2D s) { return embrSlotLevels(s.slot); }
float4 embr_texture(EmbSampler2D s, float2 uv) { return embrSlotSample(s.slot, s.m, uv, 0.0); }
float4 embr_texture(EmbSampler2D s, float2 uv, float bias) { return embrSlotSample(s.slot, s.m, uv, bias); }
float4 embr_textureLod(EmbSampler2D s, float2 uv, float lod) { return embrSlotSampleLod(s.slot, s.m, uv, lod); }
float4 embr_textureGrad(EmbSampler2D s, float2 uv, float2 dx, float2 dy) { return embrSlotSampleGrad(s.slot, s.m, uv, dx, dy); }
float2 embr_textureQueryLod(EmbSampler2D s, float2 uv) { return embrSlotQueryLod(s.slot, uv); }
float4 embr_textureProj(EmbSampler2D s, float3 p) { return embr_texture(s, p.xy / p.z); }
float4 embr_textureProj(EmbSampler2D s, float4 p) { return embr_texture(s, p.xy / p.w); }
float4 embr_textureProj(EmbSampler2D s, float3 p, float bias) { return embr_texture(s, p.xy / p.z, bias); }
float4 embr_textureProj(EmbSampler2D s, float4 p, float bias) { return embr_texture(s, p.xy / p.w, bias); }
float4 embr_textureProjLod(EmbSampler2D s, float3 p, float lod) { return embr_textureLod(s, p.xy / p.z, lod); }
float4 embr_textureProjLod(EmbSampler2D s, float4 p, float lod) { return embr_textureLod(s, p.xy / p.w, lod); }
float4 embr_texelFetch(EmbSampler2D s, int2 p, int lod) { return embrSlotFetch(s.slot, p, lod); }
float4 embr_textureGather(EmbSampler2D s, float2 uv) { return embrSlotGather(s.slot, uv, 0); }
float4 embr_textureGather(EmbSampler2D s, float2 uv, int comp) { return embrSlotGather(s.slot, uv, comp); }
#else
int2 emb_textureSize(EmbSampler2D s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int emb_textureQueryLevels(EmbSampler2D s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
float4 emb_texture(EmbSampler2D s, float2 uv) { return EMB_SMP(s.t, s.s, uv); }
float4 emb_texture(EmbSampler2D s, float2 uv, float bias) { return EMB_SMPB(s.t, s.s, uv, bias); }
float4 emb_textureLod(EmbSampler2D s, float2 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 emb_textureGrad(EmbSampler2D s, float2 uv, float2 dx, float2 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 emb_textureQueryLod(EmbSampler2D s, float2 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 emb_textureProj(EmbSampler2D s, float3 p) { return emb_texture(s, p.xy / p.z); }
float4 emb_textureProj(EmbSampler2D s, float4 p) { return emb_texture(s, p.xy / p.w); }
float4 emb_textureProj(EmbSampler2D s, float3 p, float bias) { return emb_texture(s, p.xy / p.z, bias); }
float4 emb_textureProj(EmbSampler2D s, float4 p, float bias) { return emb_texture(s, p.xy / p.w, bias); }
float4 emb_textureProjLod(EmbSampler2D s, float3 p, float lod) { return emb_textureLod(s, p.xy / p.z, lod); }
float4 emb_textureProjLod(EmbSampler2D s, float4 p, float lod) { return emb_textureLod(s, p.xy / p.w, lod); }
float4 emb_texelFetch(EmbSampler2D s, int2 p, int lod) { return s.t.Load(int3(p, lod)); }
float4 emb_textureGather(EmbSampler2D s, float2 uv) { return s.t.Gather(s.s, uv); }
float4 emb_textureGather(EmbSampler2D s, float2 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
#endif
int2 emb_textureSize(EmbISampler2D s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int emb_textureQueryLevels(EmbISampler2D s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
int4 emb_texture(EmbISampler2D s, float2 uv) { int2 sz = emb_textureSize(s, 0); return s.t.Load(int3(clamp((int2)(uv * (float2)sz), int2(0, 0), sz - 1), 0)); }
int4 emb_texture(EmbISampler2D s, float2 uv, float bias) { return emb_texture(s, uv); }
int4 emb_textureLod(EmbISampler2D s, float2 uv, float lod) { return emb_texture(s, uv); }
float2 emb_textureQueryLod(EmbISampler2D s, float2 uv) { return float2(0.0, 0.0); }
int4 emb_textureProj(EmbISampler2D s, float3 p) { return emb_texture(s, p.xy / p.z); }
int4 emb_textureProj(EmbISampler2D s, float4 p) { return emb_texture(s, p.xy / p.w); }
int4 emb_textureProj(EmbISampler2D s, float3 p, float bias) { return emb_texture(s, p.xy / p.z, bias); }
int4 emb_textureProj(EmbISampler2D s, float4 p, float bias) { return emb_texture(s, p.xy / p.w, bias); }
int4 emb_textureProjLod(EmbISampler2D s, float3 p, float lod) { return emb_textureLod(s, p.xy / p.z, lod); }
int4 emb_textureProjLod(EmbISampler2D s, float4 p, float lod) { return emb_textureLod(s, p.xy / p.w, lod); }
int4 emb_texelFetch(EmbISampler2D s, int2 p, int lod) { return s.t.Load(int3(p, lod)); }
int4 emb_textureGather(EmbISampler2D s, float2 uv) { return s.t.Gather(s.s, uv); }
int4 emb_textureGather(EmbISampler2D s, float2 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int2 emb_textureSize(EmbUSampler2D s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int emb_textureQueryLevels(EmbUSampler2D s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
uint4 emb_texture(EmbUSampler2D s, float2 uv) { int2 sz = emb_textureSize(s, 0); return s.t.Load(int3(clamp((int2)(uv * (float2)sz), int2(0, 0), sz - 1), 0)); }
uint4 emb_texture(EmbUSampler2D s, float2 uv, float bias) { return emb_texture(s, uv); }
uint4 emb_textureLod(EmbUSampler2D s, float2 uv, float lod) { return emb_texture(s, uv); }
float2 emb_textureQueryLod(EmbUSampler2D s, float2 uv) { return float2(0.0, 0.0); }
uint4 emb_textureProj(EmbUSampler2D s, float3 p) { return emb_texture(s, p.xy / p.z); }
uint4 emb_textureProj(EmbUSampler2D s, float4 p) { return emb_texture(s, p.xy / p.w); }
uint4 emb_textureProj(EmbUSampler2D s, float3 p, float bias) { return emb_texture(s, p.xy / p.z, bias); }
uint4 emb_textureProj(EmbUSampler2D s, float4 p, float bias) { return emb_texture(s, p.xy / p.w, bias); }
uint4 emb_textureProjLod(EmbUSampler2D s, float3 p, float lod) { return emb_textureLod(s, p.xy / p.z, lod); }
uint4 emb_textureProjLod(EmbUSampler2D s, float4 p, float lod) { return emb_textureLod(s, p.xy / p.w, lod); }
uint4 emb_texelFetch(EmbUSampler2D s, int2 p, int lod) { return s.t.Load(int3(p, lod)); }
uint4 emb_textureGather(EmbUSampler2D s, float2 uv) { return s.t.Gather(s.s, uv); }
uint4 emb_textureGather(EmbUSampler2D s, float2 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 emb_textureSize(EmbSampler2DArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int emb_textureQueryLevels(EmbSampler2DArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
float4 emb_texture(EmbSampler2DArray s, float3 uv) { return EMB_SMP(s.t, s.s, uv); }
float4 emb_texture(EmbSampler2DArray s, float3 uv, float bias) { return EMB_SMPB(s.t, s.s, uv, bias); }
float4 emb_textureLod(EmbSampler2DArray s, float3 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 emb_textureGrad(EmbSampler2DArray s, float3 uv, float2 dx, float2 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 emb_textureQueryLod(EmbSampler2DArray s, float2 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 emb_texelFetch(EmbSampler2DArray s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
float4 emb_textureGather(EmbSampler2DArray s, float3 uv) { return s.t.Gather(s.s, uv); }
float4 emb_textureGather(EmbSampler2DArray s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 emb_textureSize(EmbISampler2DArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int emb_textureQueryLevels(EmbISampler2DArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
int4 emb_texture(EmbISampler2DArray s, float3 uv) { int3 sz = emb_textureSize(s, 0); int2 p = clamp((int2)(uv.xy * (float2)sz.xy), int2(0, 0), sz.xy - 1); return s.t.Load(int4(p, clamp((int)round(uv.z), 0, sz.z - 1), 0)); }
int4 emb_texture(EmbISampler2DArray s, float3 uv, float bias) { return emb_texture(s, uv); }
int4 emb_textureLod(EmbISampler2DArray s, float3 uv, float lod) { return emb_texture(s, uv); }
float2 emb_textureQueryLod(EmbISampler2DArray s, float2 uv) { return float2(0.0, 0.0); }
int4 emb_texelFetch(EmbISampler2DArray s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int4 emb_textureGather(EmbISampler2DArray s, float3 uv) { return s.t.Gather(s.s, uv); }
int4 emb_textureGather(EmbISampler2DArray s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 emb_textureSize(EmbUSampler2DArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int emb_textureQueryLevels(EmbUSampler2DArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
uint4 emb_texture(EmbUSampler2DArray s, float3 uv) { int3 sz = emb_textureSize(s, 0); int2 p = clamp((int2)(uv.xy * (float2)sz.xy), int2(0, 0), sz.xy - 1); return s.t.Load(int4(p, clamp((int)round(uv.z), 0, sz.z - 1), 0)); }
uint4 emb_texture(EmbUSampler2DArray s, float3 uv, float bias) { return emb_texture(s, uv); }
uint4 emb_textureLod(EmbUSampler2DArray s, float3 uv, float lod) { return emb_texture(s, uv); }
float2 emb_textureQueryLod(EmbUSampler2DArray s, float2 uv) { return float2(0.0, 0.0); }
uint4 emb_texelFetch(EmbUSampler2DArray s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
uint4 emb_textureGather(EmbUSampler2DArray s, float3 uv) { return s.t.Gather(s.s, uv); }
uint4 emb_textureGather(EmbUSampler2DArray s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 emb_textureSize(EmbSampler3D s, int lod) { uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d); }
int emb_textureQueryLevels(EmbSampler3D s) { uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l; }
float4 emb_texture(EmbSampler3D s, float3 uv) { return EMB_SMP(s.t, s.s, uv); }
float4 emb_texture(EmbSampler3D s, float3 uv, float bias) { return EMB_SMPB(s.t, s.s, uv, bias); }
float4 emb_textureLod(EmbSampler3D s, float3 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 emb_textureGrad(EmbSampler3D s, float3 uv, float3 dx, float3 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 emb_textureQueryLod(EmbSampler3D s, float3 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 emb_textureProj(EmbSampler3D s, float4 p) { return emb_texture(s, p.xyz / p.w); }
float4 emb_textureProj(EmbSampler3D s, float4 p, float bias) { return emb_texture(s, p.xyz / p.w, bias); }
float4 emb_textureProjLod(EmbSampler3D s, float4 p, float lod) { return emb_textureLod(s, p.xyz / p.w, lod); }
float4 emb_texelFetch(EmbSampler3D s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int3 emb_textureSize(EmbISampler3D s, int lod) { uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d); }
int emb_textureQueryLevels(EmbISampler3D s) { uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l; }
int4 emb_texture(EmbISampler3D s, float3 uv) { int3 sz = emb_textureSize(s, 0); return s.t.Load(int4(clamp((int3)(uv * (float3)sz), int3(0, 0, 0), sz - 1), 0)); }
int4 emb_texture(EmbISampler3D s, float3 uv, float bias) { return emb_texture(s, uv); }
int4 emb_textureLod(EmbISampler3D s, float3 uv, float lod) { return emb_texture(s, uv); }
float2 emb_textureQueryLod(EmbISampler3D s, float3 uv) { return float2(0.0, 0.0); }
int4 emb_textureProj(EmbISampler3D s, float4 p) { return emb_texture(s, p.xyz / p.w); }
int4 emb_textureProj(EmbISampler3D s, float4 p, float bias) { return emb_texture(s, p.xyz / p.w, bias); }
int4 emb_textureProjLod(EmbISampler3D s, float4 p, float lod) { return emb_textureLod(s, p.xyz / p.w, lod); }
int4 emb_texelFetch(EmbISampler3D s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int3 emb_textureSize(EmbUSampler3D s, int lod) { uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d); }
int emb_textureQueryLevels(EmbUSampler3D s) { uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l; }
uint4 emb_texture(EmbUSampler3D s, float3 uv) { int3 sz = emb_textureSize(s, 0); return s.t.Load(int4(clamp((int3)(uv * (float3)sz), int3(0, 0, 0), sz - 1), 0)); }
uint4 emb_texture(EmbUSampler3D s, float3 uv, float bias) { return emb_texture(s, uv); }
uint4 emb_textureLod(EmbUSampler3D s, float3 uv, float lod) { return emb_texture(s, uv); }
float2 emb_textureQueryLod(EmbUSampler3D s, float3 uv) { return float2(0.0, 0.0); }
uint4 emb_textureProj(EmbUSampler3D s, float4 p) { return emb_texture(s, p.xyz / p.w); }
uint4 emb_textureProj(EmbUSampler3D s, float4 p, float bias) { return emb_texture(s, p.xyz / p.w, bias); }
uint4 emb_textureProjLod(EmbUSampler3D s, float4 p, float lod) { return emb_textureLod(s, p.xyz / p.w, lod); }
uint4 emb_texelFetch(EmbUSampler3D s, int3 p, int lod) { return s.t.Load(int4(p, lod)); }
int2 emb_textureSize(EmbSamplerCube s, int lod) { uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h); }
int emb_textureQueryLevels(EmbSamplerCube s) { uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l; }
float4 emb_texture(EmbSamplerCube s, float3 uv) { return EMB_SMP(s.t, s.s, uv); }
float4 emb_texture(EmbSamplerCube s, float3 uv, float bias) { return EMB_SMPB(s.t, s.s, uv, bias); }
float4 emb_textureLod(EmbSamplerCube s, float3 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 emb_textureGrad(EmbSamplerCube s, float3 uv, float3 dx, float3 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 emb_textureQueryLod(EmbSamplerCube s, float3 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv), s.t.CalculateLevelOfDetailUnclamped(s.s, uv)); }
float4 emb_textureGather(EmbSamplerCube s, float3 uv) { return s.t.Gather(s.s, uv); }
float4 emb_textureGather(EmbSamplerCube s, float3 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
int3 emb_textureSize(EmbSamplerCubeArray s, int lod) { uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e); }
int emb_textureQueryLevels(EmbSamplerCubeArray s) { uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l; }
float4 emb_texture(EmbSamplerCubeArray s, float4 uv) { return EMB_SMP(s.t, s.s, uv); }
float4 emb_texture(EmbSamplerCubeArray s, float4 uv, float bias) { return EMB_SMPB(s.t, s.s, uv, bias); }
float4 emb_textureLod(EmbSamplerCubeArray s, float4 uv, float lod) { return s.t.SampleLevel(s.s, uv, lod); }
float4 emb_textureGrad(EmbSamplerCubeArray s, float4 uv, float3 dx, float3 dy) { return s.t.SampleGrad(s.s, uv, dx, dy); }
float2 emb_textureQueryLod(EmbSamplerCubeArray s, float3 uv) { return float2(s.t.CalculateLevelOfDetail(s.s, uv.xyz), s.t.CalculateLevelOfDetailUnclamped(s.s, uv.xyz)); }
float4 emb_textureGather(EmbSamplerCubeArray s, float4 uv) { return s.t.Gather(s.s, uv); }
float4 emb_textureGather(EmbSamplerCubeArray s, float4 uv, int comp) { if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv); }
#endif
