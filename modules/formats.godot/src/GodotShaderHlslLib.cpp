// The fixed HLSL library the emitted Godot shader source is written against. Generated once from C++ loops so the per-size overloads
// (float .. float4, int, uint, bool) and the sampler families stay consistent. See docs/plans/fabric/G6_SHADERS.md for the table.
#include "aver/formats/godot/GodotShader.hpp"

#include <string>

namespace aver::fmt::godot {
namespace {

const char* kF[4] = {"float", "float2", "float3", "float4"};
const char* kI[4] = {"int", "int2", "int3", "int4"};
const char* kU[4] = {"uint", "uint2", "uint3", "uint4"};
const char* kB[4] = {"bool", "bool2", "bool3", "bool4"};

std::string repl(std::string t, int n) {
    // $F $I $U $B = the size-n float / int / uint / bool type; $n = 1..4
    const std::pair<const char*, const char*> subs[] = {{"$F", kF[n - 1]}, {"$I", kI[n - 1]}, {"$U", kU[n - 1]}, {"$B", kB[n - 1]}};
    for (const auto& [k, v] : subs)
        for (usize p = t.find(k); p != std::string::npos; p = t.find(k, p + 1)) t.replace(p, 2, v);
    for (usize p = t.find("$n"); p != std::string::npos; p = t.find("$n", p + 1)) t.replace(p, 2, std::to_string(n));
    return t;
}

void perSize(std::string& out, const char* tmpl, int nMin = 1) {
    for (int n = nMin; n <= 4; ++n) out += repl(tmpl, n);
}

struct Fam {
    const char* wrap;
    const char* tex;
    const char* texel;
    int kind;        // 0 = 2D, 1 = 2D array, 2 = 3D, 3 = cube, 4 = cube array
    bool isFloat;
};

std::string texFamily(const Fam& f) {
    const std::string W = f.wrap, R = f.texel;
    std::string C, G, S, IT, Q;   // coordinate, gradient, textureSize result, texel coordinate, query-lod coordinate
    switch (f.kind) {
    case 0: C = "float2"; G = "float2"; S = "int2"; IT = "int2"; Q = "float2"; break;
    case 1: C = "float3"; G = "float2"; S = "int3"; IT = "int3"; Q = "float2"; break;
    case 2: C = "float3"; G = "float3"; S = "int3"; IT = "int3"; Q = "float3"; break;
    case 3: C = "float3"; G = "float3"; S = "int2"; IT = ""; Q = "float3"; break;
    default: C = "float4"; G = "float3"; S = "int3"; IT = ""; Q = "float3"; break;
    }
    std::string o;
    auto fn = [&](const std::string& ret, const std::string& name, const std::string& params, const std::string& body) {
        o += ret + " gd_" + name + "(" + W + " s" + (params.empty() ? "" : ", " + params) + ") { " + body + " }\n";
    };
    // size and levels
    std::string dims;
    switch (f.kind) {
    case 0: dims = "uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h);"; break;
    case 1: dims = "uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e);"; break;
    case 2: dims = "uint w, h, d, l; s.t.GetDimensions((uint)lod, w, h, d, l); return int3(w, h, d);"; break;
    case 3: dims = "uint w, h, l; s.t.GetDimensions((uint)lod, w, h, l); return int2(w, h);"; break;
    default: dims = "uint w, h, e, l; s.t.GetDimensions((uint)lod, w, h, e, l); return int3(w, h, e);"; break;
    }
    fn(S, "textureSize", "int lod", dims);
    {
        std::string lv;
        switch (f.kind) {
        case 0: case 3: lv = "uint w, h, l; s.t.GetDimensions(0, w, h, l); return (int)l;"; break;
        case 2: lv = "uint w, h, d, l; s.t.GetDimensions(0, w, h, d, l); return (int)l;"; break;
        default: lv = "uint w, h, e, l; s.t.GetDimensions(0, w, h, e, l); return (int)l;"; break;
        }
        fn("int", "textureQueryLevels", "", lv);
    }
    if (f.isFloat) {
        fn(R, "texture", C + " uv", "return GD_SMP(s.t, s.s, uv);");
        fn(R, "texture", C + " uv, float bias", "return GD_SMPB(s.t, s.s, uv, bias);");
        fn(R, "textureLod", C + " uv, float lod", "return s.t.SampleLevel(s.s, uv, lod);");
        fn(R, "textureGrad", C + " uv, " + G + " dx, " + G + " dy", "return s.t.SampleGrad(s.s, uv, dx, dy);");
        std::string q = f.kind == 4 ? "uv.xyz" : "uv";
        fn("float2", "textureQueryLod", Q + " uv", "return float2(s.t.CalculateLevelOfDetail(s.s, " + q + "), s.t.CalculateLevelOfDetailUnclamped(s.s, " + q + "));");
    } else {
        // integer textures cannot be filtered: nearest texel through Load
        std::string body;
        if (f.kind == 0) body = "int2 sz = gd_textureSize(s, 0); return s.t.Load(int3(clamp((int2)(uv * (float2)sz), int2(0, 0), sz - 1), 0));";
        else if (f.kind == 1) body = "int3 sz = gd_textureSize(s, 0); int2 p = clamp((int2)(uv.xy * (float2)sz.xy), int2(0, 0), sz.xy - 1); return s.t.Load(int4(p, clamp((int)round(uv.z), 0, sz.z - 1), 0));";
        else if (f.kind == 2) body = "int3 sz = gd_textureSize(s, 0); return s.t.Load(int4(clamp((int3)(uv * (float3)sz), int3(0, 0, 0), sz - 1), 0));";
        if (!body.empty()) {
            fn(R, "texture", C + " uv", body);
            fn(R, "texture", C + " uv, float bias", "return gd_texture(s, uv);");
            fn(R, "textureLod", C + " uv, float lod", "return gd_texture(s, uv);");
        }
        fn("float2", "textureQueryLod", Q + " uv", "return float2(0.0, 0.0);");
    }
    // projective (2D and 3D only), by way of the family's own texture()
    if (f.kind == 0) {
        fn(R, "textureProj", "float3 p", "return gd_texture(s, p.xy / p.z);");
        fn(R, "textureProj", "float4 p", "return gd_texture(s, p.xy / p.w);");
        fn(R, "textureProj", "float3 p, float bias", "return gd_texture(s, p.xy / p.z, bias);");
        fn(R, "textureProj", "float4 p, float bias", "return gd_texture(s, p.xy / p.w, bias);");
        fn(R, "textureProjLod", "float3 p, float lod", "return gd_textureLod(s, p.xy / p.z, lod);");
        fn(R, "textureProjLod", "float4 p, float lod", "return gd_textureLod(s, p.xy / p.w, lod);");
    } else if (f.kind == 2) {
        fn(R, "textureProj", "float4 p", "return gd_texture(s, p.xyz / p.w);");
        fn(R, "textureProj", "float4 p, float bias", "return gd_texture(s, p.xyz / p.w, bias);");
        fn(R, "textureProjLod", "float4 p, float lod", "return gd_textureLod(s, p.xyz / p.w, lod);");
    }
    if (!IT.empty()) {
        fn(R, "texelFetch", IT + " p, int lod", f.kind == 0 ? "return s.t.Load(int3(p, lod));" : "return s.t.Load(int4(p, lod));");
    }
    if (f.kind != 2) {
        fn(R, "textureGather", C + " uv", "return s.t.Gather(s.s, uv);");
        fn(R, "textureGather", C + " uv, int comp",
           "if (comp == 1) return s.t.GatherGreen(s.s, uv); if (comp == 2) return s.t.GatherBlue(s.s, uv); if (comp == 3) return s.t.GatherAlpha(s.s, uv); return s.t.GatherRed(s.s, uv);");
    }
    if (std::string(f.wrap) == "GdSampler2D") {
        // SH2: under AVER_GD_SLOT_SAMPLERS a sampler2D is {engine texture slot, mode word} and every access goes through gdSlot* (material_godot.hlsl), which
        // dispatches on the slot and emulates the filter / repeat state on the engine's one shared sampler.
        // Two flavours: gd_* over gdSlot* (raster, implicit derivatives) and gdr_* over gdrSlot* (the ray flavour: no derivative anywhere). The ray
        // flavour of a converted source is the same text included with the gd_ names redirected by macro (MaterialGraphHlsl.cpp).
        std::string so;
        for (const char* flavour : {"", "r"}) {
            const std::string pre = std::string("gd") + flavour + "_", sl = std::string("gd") + flavour + "Slot";
            auto sf = [&](const std::string& ret, const std::string& name, const std::string& params, const std::string& body) {
                so += ret + " " + pre + name + "(GdSampler2D s" + (params.empty() ? "" : ", " + params) + ") { " + body + " }\n";
            };
            sf("int2", "textureSize", "int lod", "return " + sl + "Size(s.slot, lod);");
            sf("int", "textureQueryLevels", "", "return " + sl + "Levels(s.slot);");
            sf("float4", "texture", "float2 uv", "return " + sl + "Sample(s.slot, s.m, uv, 0.0);");
            sf("float4", "texture", "float2 uv, float bias", "return " + sl + "Sample(s.slot, s.m, uv, bias);");
            sf("float4", "textureLod", "float2 uv, float lod", "return " + sl + "SampleLod(s.slot, s.m, uv, lod);");
            sf("float4", "textureGrad", "float2 uv, float2 dx, float2 dy", "return " + sl + "SampleGrad(s.slot, s.m, uv, dx, dy);");
            sf("float2", "textureQueryLod", "float2 uv", "return " + sl + "QueryLod(s.slot, uv);");
            sf("float4", "textureProj", "float3 p", "return " + pre + "texture(s, p.xy / p.z);");
            sf("float4", "textureProj", "float4 p", "return " + pre + "texture(s, p.xy / p.w);");
            sf("float4", "textureProj", "float3 p, float bias", "return " + pre + "texture(s, p.xy / p.z, bias);");
            sf("float4", "textureProj", "float4 p, float bias", "return " + pre + "texture(s, p.xy / p.w, bias);");
            sf("float4", "textureProjLod", "float3 p, float lod", "return " + pre + "textureLod(s, p.xy / p.z, lod);");
            sf("float4", "textureProjLod", "float4 p, float lod", "return " + pre + "textureLod(s, p.xy / p.w, lod);");
            sf("float4", "texelFetch", "int2 p, int lod", "return " + sl + "Fetch(s.slot, p, lod);");
            sf("float4", "textureGather", "float2 uv", "return " + sl + "Gather(s.slot, uv, 0);");
            sf("float4", "textureGather", "float2 uv, int comp", "return " + sl + "Gather(s.slot, uv, comp);");
        }
        return "#ifdef AVER_GD_SLOT_SAMPLERS\n" + so + "#else\n" + o + "#endif\n";
    }
    return o;
}

std::string build() {
    std::string s;
    s += R"(// ---- Aver Godot shader library (gdShaderHlslLibrary) -----------------------------------------------------------
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
)";

    // scalar / vector math helpers
    perSize(s, "$F gd_mod($F x, $F y) { return x - y * floor(x / y); }\n");
    perSize(s, "$F gd_mod($F x, float y) { return x - y * floor(x / y); }\n", 2);
    perSize(s, "$F gd_mix($F a, $F b, $F t) { return lerp(a, b, t); }\n");
    perSize(s, "$F gd_mix($F a, $F b, float t) { return lerp(a, b, ($F)t); }\n", 2);
    s += "float gd_mix(float a, float b, bool t) { return t ? b : a; }\n";
    for (int n = 2; n <= 4; ++n) {
        std::string body = std::string(kF[n - 1]) + "(";
        for (int c = 0; c < n; ++c) {
            const char ch = "xyzw"[c];
            body += std::string(c ? ", " : "") + "t." + ch + " ? b." + ch + " : a." + ch;
        }
        s += std::string(kF[n - 1]) + " gd_mix(" + kF[n - 1] + " a, " + kF[n - 1] + " b, " + kB[n - 1] + " t) { return " + body + "); }\n";
    }
    perSize(s, "$F gd_sign($F x) { return ($F)sign(x); }\n$I gd_sign($I x) { return sign(x); }\n");
    perSize(s, "$F gd_step($F e, $F x) { return step(e, x); }\n");
    perSize(s, "$F gd_step(float e, $F x) { return step(($F)e, x); }\n", 2);
    perSize(s, "$F gd_smoothstep($F a, $F b, $F x) { return smoothstep(a, b, x); }\n");
    perSize(s, "$F gd_smoothstep(float a, float b, $F x) { return smoothstep(($F)a, ($F)b, x); }\n", 2);
    perSize(s, "$F gd_asinh($F x) { return ($F)sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }\n$F gd_acosh($F x) { return log(x + sqrt(x * x - 1.0)); }\n"
               "$F gd_atanh($F x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }\n");
    perSize(s, "$F gd_fwidthCoarse($F x) { return abs(ddx_coarse(x)) + abs(ddy_coarse(x)); }\n$F gd_fwidthFine($F x) { return abs(ddx_fine(x)) + abs(ddy_fine(x)); }\n");

    // bit operations
    perSize(s, "$I gd_bitfieldExtract($I v, int off, int bits) { return bits == 0 ? ($I)0 : (v << (32 - off - bits)) >> (32 - bits); }\n"
               "$U gd_bitfieldExtract($U v, int off, int bits) { return bits == 0 ? ($U)0 : (v << (32 - off - bits)) >> (32 - bits); }\n"
               "$I gd_bitfieldInsert($I b, $I v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return ($I)((($U)b & ~($U)m) | ((($U)v << off) & ($U)m)); }\n"
               "$U gd_bitfieldInsert($U b, $U v, int off, int bits) { uint m = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1u) << off); return (b & ~($U)m) | ((v << off) & ($U)m); }\n"
               "$I gd_bitCount($I v) { return ($I)countbits(($U)v); }\n$I gd_bitCount($U v) { return ($I)countbits(v); }\n"
               "$I gd_findLSB($I v) { return ($I)firstbitlow(v); }\n$I gd_findLSB($U v) { return ($I)firstbitlow(v); }\n"
               "$I gd_findMSB($I v) { return ($I)firstbithigh(v); }\n$I gd_findMSB($U v) { return ($I)firstbithigh(v); }\n");

    // pack / unpack
    s += R"(uint gd_packHalf2x16(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 gd_unpackHalf2x16(uint p) { return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16)); }
uint gd_packUnorm2x16(float2 v) { uint2 q = (uint2)round(saturate(v) * 65535.0); return q.x | (q.y << 16); }
float2 gd_unpackUnorm2x16(uint p) { return float2(p & 0xFFFFu, p >> 16) / 65535.0; }
uint gd_packSnorm2x16(float2 v) { int2 q = (int2)round(clamp(v, -1.0, 1.0) * 32767.0); return ((uint)q.x & 0xFFFFu) | (((uint)q.y & 0xFFFFu) << 16); }
float2 gd_unpackSnorm2x16(uint p) { int2 q = int2((int)(p << 16) >> 16, (int)p >> 16); return clamp(float2(q) / 32767.0, -1.0, 1.0); }
uint gd_packUnorm4x8(float4 v) { uint4 q = (uint4)round(saturate(v) * 255.0); return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24); }
float4 gd_unpackUnorm4x8(uint p) { return float4(p & 0xFFu, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu, p >> 24) / 255.0; }
uint gd_packSnorm4x8(float4 v) { int4 q = (int4)round(clamp(v, -1.0, 1.0) * 127.0); return ((uint)q.x & 0xFFu) | (((uint)q.y & 0xFFu) << 8) | (((uint)q.z & 0xFFu) << 16) | (((uint)q.w & 0xFFu) << 24); }
float4 gd_unpackSnorm4x8(uint p) { int4 q = int4((int)(p << 24) >> 24, (int)(p << 16) >> 24, (int)(p << 8) >> 24, (int)p >> 24); return clamp(float4(q) / 127.0, -1.0, 1.0); }
)";

    // matrices: HLSL rows hold GLSL columns
    s += R"(float2x2 gd_mat2_diag(float s) { return float2x2(s, 0.0, 0.0, s); }
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
)";

    const Fam fams[] = {
        {"GdSampler2D", "Texture2D<float4>", "float4", 0, true},          {"GdISampler2D", "Texture2D<int4>", "int4", 0, false},
        {"GdUSampler2D", "Texture2D<uint4>", "uint4", 0, false},          {"GdSampler2DArray", "Texture2DArray<float4>", "float4", 1, true},
        {"GdISampler2DArray", "Texture2DArray<int4>", "int4", 1, false},  {"GdUSampler2DArray", "Texture2DArray<uint4>", "uint4", 1, false},
        {"GdSampler3D", "Texture3D<float4>", "float4", 2, true},          {"GdISampler3D", "Texture3D<int4>", "int4", 2, false},
        {"GdUSampler3D", "Texture3D<uint4>", "uint4", 2, false},          {"GdSamplerCube", "TextureCube<float4>", "float4", 3, true},
        {"GdSamplerCubeArray", "TextureCubeArray<float4>", "float4", 4, true},
    };
    for (const Fam& f : fams) s += texFamily(f);
    s += "#endif\n";
    return s;
}

}  // namespace

const char* gdShaderHlslLibrary() {
    static const std::string lib = build();
    return lib.c_str();
}

}  // namespace aver::fmt::godot
