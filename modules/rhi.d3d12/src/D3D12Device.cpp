// DirectX 12 backend for Aver.RHI — device, swapchain, depth buffer, a lit-mesh
// pipeline (HLSL Lambert shading compiled at runtime), immediate draw path, an
// offscreen GPU self-test, and a backbuffer capture for verification. Hand-rolled
// D3D12 structs (no d3dx12.h). Falls back to nullptr if D3D12 is unavailable.
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"   // light-space matrices for the shadow map

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>      // DXC: shader model 6.x (mesh shaders, DXR RayQuery)
#include <string>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"
// The impl header intentionally leaves this for the app's WndProc TU to declare.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

using Microsoft::WRL::ComPtr;

namespace aver::rhi {
namespace {

constexpr u32 kFrameCount = 2;
constexpr u32 kDefaultSampleCount = 4; // MSAA default; runtime-adjustable via setSampleCount
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

// ---------------------------------------------------------------- shader compilation
// DXC (DXIL, shader model 6.x) is required for mesh shaders and for DXR's RayQuery; FXC tops out
// at SM 5.1. dxcompiler.dll is NOT part of Windows, so it is redistributed next to the exe (see
// this module's CMakeLists). If it is missing we fall back to FXC and lose only the SM6-only
// features - the engine still runs, which is why the fallback exists at all.
class ShaderCompiler {
public:
    void init() {
        if (tried_) return;
        tried_ = true;
        dll_ = LoadLibraryW(L"dxcompiler.dll");
        if (!dll_) { AVER_WARN("[RHI.D3D12] dxcompiler.dll not found - falling back to FXC (SM 5.1)"); return; }
        auto create = reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(GetProcAddress(dll_, "DxcCreateInstance")));
        if (!create) { AVER_WARN("[RHI.D3D12] DxcCreateInstance missing - falling back to FXC"); return; }
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) {
            utils_.Reset(); compiler_.Reset();
            AVER_WARN("[RHI.D3D12] DXC init failed - falling back to FXC");
            return;
        }
        AVER_INFO("[RHI.D3D12] shader compiler: DXC (shader model 6.x)");
    }
    bool usingDxc() const { return compiler_ != nullptr; }

    // `target51` is the FXC target, e.g. "vs_5_1"; the SM6 equivalent is derived from it.
    // `sm6` overrides that (e.g. "ps_6_5" for RayQuery), and `define` adds one -D. Both require
    // DXC, so a caller using them must have checked the device caps first.
    HRESULT compile(const char* src, const char* entry, const char* target51, ID3DBlob** out,
                    const char* sm6 = nullptr, const char* define = nullptr) {
        init();
        if (!usingDxc()) {
            if (sm6 || define) return E_NOTIMPL;   // SM6-only path; caller must fall back
            ComPtr<ID3DBlob> err;
            const UINT flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
            HRESULT hr = D3DCompile(src, std::strlen(src), "aver.hlsl", nullptr, nullptr, entry, target51, flags, 0, out, &err);
            if (FAILED(hr) && err) AVER_ERROR("[RHI.D3D12] {} ({}): {}", entry, target51, static_cast<const char*>(err->GetBufferPointer()));
            return hr;
        }
        // "vs_5_1" -> "vs_6_0". Stages that need a higher model pass it in explicitly.
        std::string t6(target51);
        const size_t us = t6.rfind("_5_1");
        if (us != std::string::npos) t6 = t6.substr(0, us) + "_6_0";
        if (sm6) t6 = sm6;
        const std::wstring wEntry(entry, entry + std::strlen(entry));
        const std::wstring wTarget(t6.begin(), t6.end());
        std::wstring wDefine;
        if (define) wDefine.assign(define, define + std::strlen(define));

        DxcBuffer buf{src, std::strlen(src), DXC_CP_UTF8};
        std::vector<LPCWSTR> args = {
            L"-E", wEntry.c_str(),
            L"-T", wTarget.c_str(),
            L"-Zpr",           // pack matrices row-major: the engine's convention
            L"-HV", L"2021",
        };
        if (define) { args.push_back(L"-D"); args.push_back(wDefine.c_str()); }
        ComPtr<IDxcResult> result;
        HRESULT hr = compiler_->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), nullptr, IID_PPV_ARGS(&result));
        if (SUCCEEDED(hr)) result->GetStatus(&hr);
        if (FAILED(hr)) {
            ComPtr<IDxcBlobUtf8> errs;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength())
                AVER_ERROR("[RHI.D3D12] {} ({}): {}", entry, t6, errs->GetStringPointer());
            return hr;
        }
        ComPtr<IDxcBlob> obj;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj) return E_FAIL;
        // Repackage as ID3DBlob so every PSO call site stays unchanged.
        if (FAILED(D3DCreateBlob(obj->GetBufferSize(), out))) return E_FAIL;
        std::memcpy((*out)->GetBufferPointer(), obj->GetBufferPointer(), obj->GetBufferSize());
        return S_OK;
    }
private:
    bool tried_ = false;
    HMODULE dll_ = nullptr;
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
};

ShaderCompiler& shaderCompiler() { static ShaderCompiler c; return c; }

bool hrOk(HRESULT hr, const char* what) {
    if (FAILED(hr)) { AVER_ERROR("[RHI.D3D12] {} failed (hr=0x{:08X})", what, static_cast<u32>(hr)); return false; }
    return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

#if AVER_WITH_IMGUI
static bool imguiWndProc(void* hwnd, u32 msg, u64 w, i64 l) {
    return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                          static_cast<WPARAM>(w), static_cast<LPARAM>(l)) != 0;
}
#endif

D3D12_RESOURCE_DESC bufferDesc(u64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// Triangles per mesh-shader thread group. Must stay in step with AVER_MS_TRIS in the HLSL below;
// 64 tris = 192 verts / 64 prims, inside the 256/256 per-group output limit.
constexpr u32 kMsTrisPerGroup = 64;

const char* kShaderHLSL = R"(
cbuffer PerFrame : register(b0) {
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4   gCamPos;      // xyz
    float4   gLightDir;    // xyz = direction TO light
    float4   gLightColor;  // rgb
    float4   gAmbient;     // rgb
    float4   gSkyZenith;   // rgb
    float4   gSkyHorizon;  // rgb
    float4   gFogColor;    // rgb, a = density
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    float4x4 gLightViewProj;
    float4   gShadowParams; // x = 1/shadowMapSize, y = enabled
};
cbuffer PerObject : register(b1) {
    float4x4 gWorld;
    float4   gBaseColor;
    float4   gMaterial;   // x=metallic, y=roughness, z=unlit(0/1)
};

// ---- Voxi: voxel cone traced GI ----
RWTexture3D<float4> gVoxelUAV : register(u0);
Texture3D<float4>   gVoxelTex : register(t0);
SamplerState        gVoxelSamp : register(s0);

// Directional shadow map. Core feature-level 11_0 (no optional caps), so it works on every
// DX12 GPU - which is why shadowed light injection uses this rather than ray-traced shadows.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

#if AVER_RT
// DXR 1.1 inline ray tracing. Traced from the pixel shader itself - no state objects, no shader
// binding tables, no DispatchRays - so it drops into the existing raster pipeline.
RaytracingAccelerationStructure gScene : register(t2);

// Exact hard shadow: one occlusion ray toward the sun. ACCEPT_FIRST_HIT_AND_END_SEARCH makes it a
// pure any-hit visibility query, which is much cheaper than finding the closest hit.
float rtShadow(float3 wpos, float3 N, float3 L) {
    RayDesc r;
    r.Origin    = wpos + N * 0.02;   // offset along the normal so we do not hit ourselves
    r.Direction = L;
    r.TMin      = 0.001;
    r.TMax      = 100000.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
#endif

// 3x3 PCF. Returns 1 = fully lit, 0 = fully shadowed.
float shadowFactor(float3 wpos, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    float4 lp = mul(float4(wpos, 1.0), gLightViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;  // outside the map = lit
    float bias = max(0.0015 * (1.0 - ndl), 0.0003);               // depth bias, light-space units
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z - bias);
    return s / 9.0;
}

// Mip level being read by CSMip (b3: b0/b1 are taken by the graphics root signature).
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 _mipPad; };

// world -> [0,1] volume coords
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// ---- cone tracing (defined before PSMain: HLSL needs definition before use) ----
// March a cone through the volume, widening with distance and reading a coarser mip each step so
// one sample covers the cone's footprint. Front-to-back alpha compositing.
float4 traceCone(float3 originWS, float3 dir, float aperture) {
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x); // one voxel, world units
    float dist = voxelWorld * 2.0;                              // start off-surface to avoid self-hit
    float4 acc = 0;
    [loop] for (int step = 0; step < 24; ++step) {
        if (acc.a >= 0.95 || dist > gVoxelParams.z) break;
        float diameter = max(voxelWorld, 2.0 * aperture * dist);
        float mip = log2(diameter / voxelWorld);
        float3 uvw = voxelUVW(originWS + dir * dist);
        if (!insideVolume(uvw)) break;
        float4 s = gVoxelTex.SampleLevel(gVoxelSamp, uvw, mip);
        acc += (1.0 - acc.a) * s;
        dist += diameter * 0.5;
    }
    return acc;
}

// Six cones over the hemisphere: one along the normal, five in a ring. Enough for smooth bounce
// lighting without the cost of a full irradiance gather.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);
    const float aperture = 0.577;              // ~60 degree cone
    float4 sum = traceCone(wpos, N, aperture);
    float occ = sum.a;
    [unroll] for (int k = 0; k < 5; ++k) {
        float ang = 1.2566 * k;                // 2*pi/5
        float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
        float4 c = traceCone(wpos, d, aperture);
        sum += c; occ += c.a;
    }
    sum /= 6.0; occ /= 6.0;
    ao = saturate(1.0 - occ);
    return sum.rgb * gVoxelParams.y;
}

static const float PI = 3.14159265;
float3 acesTonemap(float3 x){ return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
float3 toGamma(float3 c){ return pow(max(c,0.0), 1.0/2.2); }
float3 srgbToLin(float3 c){ return pow(max(c,0.0), 2.2); }
float3 skyColor(float3 dir){ float3 c = lerp(gSkyHorizon.rgb, gSkyZenith.rgb, pow(saturate(dir.z*0.5+0.5), 0.65)); return srgbToLin(c); }
float3 fresnelSchlick(float ct, float3 F0){ return F0 + (1.0-F0)*pow(saturate(1.0-ct),5.0); }
float distGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float geomSchlick(float nd, float k){ return nd/(nd*(1.0-k)+k); }

// ---- PBR mesh with sky ambient + distance fog ----
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; };
struct VSOut { float4 pos : SV_POSITION; float3 nrmWS : NORMAL; float3 wpos : TEXCOORD0; };

VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.pos = mul(wp, gViewProj);
    o.nrmWS = mul(float4(i.nrm, 0.0), gWorld).xyz;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    float3 N = normalize(i.nrmWS);
    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(V + L);
    float metallic = saturate(gMaterial.x);
    float rough = clamp(gMaterial.y, 0.045, 1.0);

    if (gMaterial.z > 0.5) { // unlit (gizmo/grid): authored display colour, no lighting
        return float4(gBaseColor.rgb, gBaseColor.a);
    }

    float3 albedo = srgbToLin(gBaseColor.rgb);
    float3 lightC = srgbToLin(gLightColor.rgb) * 3.0; // sun radiance
    float ndv = saturate(dot(N, V));
    float ndl = saturate(dot(N, L));
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    // direct (Cook-Torrance GGX)
    float a = rough * rough;
    float k = (rough + 1.0); k = k * k / 8.0;
    float D = distGGX(saturate(dot(N, H)), a);
    float G = geomSchlick(ndv, k) * geomSchlick(ndl, k);
    float3 F = fresnelSchlick(saturate(dot(H, V)), F0);
    float3 spec = (D * G * F) / (4.0 * ndv * ndl + 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
#if AVER_RT
    const float sunVis = rtShadow(i.wpos, N, L);          // exact ray-traced occlusion
#else
    const float sunVis = shadowFactor(i.wpos, ndl);       // shadow map + PCF
#endif
    float3 direct = (kd * albedo / PI + spec) * lightC * ndl * sunVis;

    // ambient: sky hemisphere irradiance (linear) + crude spec reflection of the sky
    float3 ambient = kd * albedo * skyColor(N) * gAmbient.r;
    float3 R = reflect(-V, N);
    float3 envSpec = skyColor(R) * fresnelSchlick(ndv, F0) * (1.0 - rough);

    // Voxi indirect bounce: cone-traced diffuse GI + the ambient occlusion that falls out of it.
    float3 indirect = 0;
    if (gVoxelParams.w > 0.5) {
        float ao;
        indirect = kd * albedo * coneTracedIndirect(i.wpos, N, ao);
        ambient *= ao;   // the volume already knows what is occluded
    }
    float3 color = direct + ambient + indirect + envSpec * 0.35;

    // distance fog (linear space)
    float dist = length(i.wpos - gCamPos.xyz);
    float fog = 1.0 - exp(-dist * gFogColor.a);
    color = lerp(color, srgbToLin(gFogColor.rgb), saturate(fog));

    return float4(toGamma(acesTonemap(color)), gBaseColor.a);
}

// ---- procedural sky (fullscreen triangle via SV_VertexID) ----
struct SkyOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
SkyOut VSky(uint id : SV_VertexID) {
    SkyOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}
float4 PSky(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float3 sky = skyColor(ray);
    float sd = saturate(dot(ray, normalize(gLightDir.xyz)));
    float3 sunC = srgbToLin(gLightColor.rgb);
    sky += sunC * pow(sd, 2000.0) * 14.0;  // sun disk
    sky += sunC * pow(sd, 12.0) * 0.30;    // sun glow
    return float4(toGamma(acesTonemap(sky)), 1.0);
}

// ---- unlit coloured lines (grid / gizmo) ----
struct LVSIn  { float3 pos : POSITION; float3 col : COLOR; };
struct LVSOut { float4 pos : SV_POSITION; float3 col : COLOR; };
LVSOut VSLine(LVSIn i) {
    LVSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.pos = mul(wp, gViewProj);
    o.col = i.col;
    return o;
}
float4 PSLine(LVSOut i) : SV_TARGET { return float4(i.col, 1.0); }

// Depth-only pass from the sun's point of view (VSIn is declared above).
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gLightViewProj);
}

// ================= Voxi: voxelisation =================
// The scene is rasterised once per frame with no render target; the pixel shader computes direct
// lighting and writes radiance straight into the 3D volume. Merging "voxelise" and "inject light"
// into one pass avoids a second full scene traversal.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; };

VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = mul(float4(i.nrm, 0.0), gWorld).xyz;
    o.pos  = wp;                     // world space; the GS picks a projection axis
    return o;
}

// Project each triangle along its dominant axis so it covers the most pixels (and therefore the
// most voxels). The voxel index is recomputed from world position in the PS, so the choice of
// axis does not affect correctness - only coverage.
[maxvertexcount(3)]
void GSVoxel(triangle VoxOut inp[3], inout TriangleStream<VoxOut> os) {
    float3 n = abs(cross(inp[1].wpos - inp[0].wpos, inp[2].wpos - inp[0].wpos));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    [unroll] for (int k = 0; k < 3; ++k) {
        VoxOut o = inp[k];
        float3 v = voxelUVW(o.wpos);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0);
        os.Append(o);
    }
}

#if AVER_MS
// ================= Mesh shader geometry path =================
// A mesh shader has no input assembler, so it reads the vertex and index buffers itself. Both are
// bound as ROOT SRVs (raw/structured buffers are allowed there), which keeps the per-mesh cost at
// two GPU virtual addresses and needs no descriptor heap slots at all.
//
// One thread group per 64 triangles, expanded unindexed: 192 vertices / 64 primitives, inside the
// 256/256 per-group limit. Skipping vertex reuse costs a little duplicate transform work and buys
// a much simpler shader; meshlet building belongs with the asset pipeline, not here.
struct MeshVtx { float3 pos; float3 nrm; };
StructuredBuffer<MeshVtx> gVerts   : register(t3);
ByteAddressBuffer         gIndices : register(t4);
cbuffer MeshCB : register(b2) { uint gTriCount; uint3 _msPad; };

#define AVER_MS_TRIS 64

// Triangles this group owns, and where its slice of the output arrays starts.
uint msTriCount(uint gid) { return min(AVER_MS_TRIS, gTriCount - gid * AVER_MS_TRIS); }

[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSMain(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
            out vertices VSOut verts[AVER_MS_TRIS * 3],
            out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);   // must run for the whole group, before any output
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        float4 wp = mul(float4(v.pos, 1.0), gWorld);
        VSOut ov;
        ov.wpos  = wp.xyz;
        ov.pos   = mul(wp, gViewProj);
        ov.nrmWS = mul(float4(v.nrm, 0.0), gWorld).xyz;
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}

// Voxelisation without a geometry shader. The dominant-axis choice GSVoxel made per primitive is
// made here instead - the mesh shader is already per-primitive, so the GS stage disappears. That
// matters because GS is emulated on every AMD GCN part and is markedly slower there.
[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSVoxel(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
             out vertices VoxOut verts[AVER_MS_TRIS * 3],
             out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    float3 wp[3], nr[3];
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        wp[k] = mul(float4(v.pos, 1.0), gWorld).xyz;
        nr[k] = mul(float4(v.nrm, 0.0), gWorld).xyz;
    }
    float3 n = abs(cross(wp[1] - wp[0], wp[2] - wp[0]));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        float3 v = voxelUVW(wp[k]);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        VoxOut ov;
        ov.wpos = wp[k];
        ov.nrm  = nr[k];
        ov.pos  = float4(p * 2.0 - 1.0, 0.5, 1.0);
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS

void PSVoxel(VoxOut i) {
    float3 uvw = voxelUVW(i.wpos);
    if (!insideVolume(uvw)) return;
    float3 N = normalize(i.nrm);
    float3 L = normalize(gLightDir.xyz);
    float3 albedo = srgbToLin(gBaseColor.rgb);
    // SHADOWED injection: a surface in shadow must not emit sun radiance into the volume, or the
    // bounce lighting leaks through walls and shadowed areas glow.
    float ndl = saturate(dot(N, L));
    float3 radiance = albedo * (srgbToLin(gLightColor.rgb) * ndl * shadowFactor(i.wpos, ndl)
                                + skyColor(N) * gAmbient.r);
    int3 c = int3(uvw * gVoxelParams.x);
    gVoxelUAV[c] = float4(radiance, 1.0);   // alpha = occupancy
}

// Clear mip 0 before injection. PSVoxel only writes the voxels its triangles cover, so without a
// clear a voxel lit on one frame stays lit forever and moving geometry drags a radiance trail
// behind it. Only mip 0 needs this - CSMip fully overwrites every coarser level.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) { gVoxelUAV[id] = 0.0; }

// ================= Voxi: mip filtering =================
// Box-filter one mip into the next. Averaging radiance AND occupancy is what lets a wide cone
// step read a single blurry sample instead of marching every voxel. Reuses the volume bindings
// (t0 = whole chain, u0 = the destination mip) so no extra registers are needed.
[numthreads(4,4,4)]
void CSMip(uint3 id : SV_DispatchThreadID) {
    int3 s = int3(id) * 2;
    float4 a = 0;
    [unroll] for (int x=0;x<2;++x)
    [unroll] for (int y=0;y<2;++y)
    [unroll] for (int z=0;z<2;++z)
        a += gVoxelTex.Load(int4(s + int3(x,y,z), gSrcMip));
    gVoxelUAV[id] = a * 0.125;
}

// Debug: raymarch the volume straight to screen so voxelisation can be inspected on its own.
float4 PSVoxelDebug(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
    float4 acc = 0;
    float t = 0;
    [loop] for (int s = 0; s < 256; ++s) {
        if (acc.a >= 0.98) break;
        float3 uvw = voxelUVW(gCamPos.xyz + ray * t);
        t += voxelWorld;
        if (t > gVoxelParams.z * 2.0) break;
        if (!insideVolume(uvw)) continue;
        float4 v = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
        acc += (1.0 - acc.a) * v;
    }
    float3 bg = skyColor(ray);
    float3 col = acc.rgb + bg * (1.0 - acc.a);
    return float4(toGamma(acesTonemap(col)), 1.0);
}
)";

struct PerFrameCB {
    f32 viewProj[16];
    f32 invViewProj[16];
    f32 camPos[4];
    f32 lightDir[4];
    f32 lightColor[4];
    f32 ambient[4];
    f32 skyZenith[4];
    f32 skyHorizon[4];
    f32 fogColor[4];    // a = density
    f32 voxelOrigin[4];  // xyz = volume min corner, w = 1/volumeWorldSize
    f32 voxelParams[4];  // x = resolution, y = intensity, z = maxDistance, w = enabled
    f32 lightViewProj[16];
    f32 shadowParams[4]; // x = 1/shadowMapSize, y = enabled
};

struct GpuMesh {
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    ComPtr<ID3D12Resource> blas;   // DXR bottom-level AS, built lazily
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    u32 indexCount = 0;
};

struct GpuLineMesh {
    ComPtr<ID3D12Resource> vb;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    u32 count = 0;
};

class D3D12Device;

class D3D12Swapchain final : public ISwapchain {
public:
    explicit D3D12Swapchain(D3D12Device* dev) : dev_(dev) {}
    void present() override;
    void resize(u32 w, u32 h) override;
    u32 width() const override;
    u32 height() const override;
private:
    D3D12Device* dev_;
};

class D3D12Device final : public IDevice {
public:
    bool init(const DeviceDesc& desc);
    ~D3D12Device() override { waitForGpu(); uiShutdown(); if (fenceEvent_) CloseHandle(fenceEvent_); }

    bool uiInit(void* hwnd) override;
    void uiNewFrame() override;
    void uiShutdown() override;
    bool uiActive() const override { return uiActive_; }
    bool uiWantsMouse() const override;
    bool uiWantsKeyboard() const override;

    Backend backend() const override { return Backend::D3D12; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }
    u32 sampleCount() const override { return sampleCount_; }
    bool setSampleCount(u32 samples) override;

    ISwapchain* createSwapchain(const SwapchainDesc& d) override {
        if (!createSwapchainResources(d)) return nullptr;
        return new D3D12Swapchain(this);
    }

    void setClearColor(f32 r, f32 g, f32 b, f32 a) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; clear_[3] = a; }

    void setViewportRect(u32 x, u32 y, u32 w, u32 h) override {
        if (w == 0 || h == 0 || x >= width_ || y >= height_) { vpX_ = vpY_ = vpW_ = vpH_ = 0; return; }
        vpX_ = x; vpY_ = y;
        vpW_ = (x + w > width_) ? width_ - x : w;   // clamp: an out-of-bounds scissor is a debug-layer error
        vpH_ = (y + h > height_) ? height_ - y : h;
    }

    void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 camPos[3]) override {
        std::memcpy(frameCB_.viewProj, viewProj, sizeof(frameCB_.viewProj));
        std::memcpy(frameCB_.invViewProj, invViewProj, sizeof(frameCB_.invViewProj));
        frameCB_.camPos[0] = camPos[0]; frameCB_.camPos[1] = camPos[1]; frameCB_.camPos[2] = camPos[2]; frameCB_.camPos[3] = 1;
    }
    void setLight(const f32 dir[3], const f32 color[3], f32 ambient) override {
        frameCB_.lightDir[0] = dir[0]; frameCB_.lightDir[1] = dir[1]; frameCB_.lightDir[2] = dir[2]; frameCB_.lightDir[3] = 0;
        frameCB_.lightColor[0] = color[0]; frameCB_.lightColor[1] = color[1]; frameCB_.lightColor[2] = color[2]; frameCB_.lightColor[3] = 0;
        frameCB_.ambient[0] = frameCB_.ambient[1] = frameCB_.ambient[2] = ambient; frameCB_.ambient[3] = 0;
    }
    void setSky(bool enabled, const f32 zenith[3], const f32 horizon[3], const f32 fogColor[3], f32 fogDensity) override {
        skyEnabled_ = enabled;
        for (int i = 0; i < 3; ++i) { frameCB_.skyZenith[i] = zenith[i]; frameCB_.skyHorizon[i] = horizon[i]; frameCB_.fogColor[i] = fogColor[i]; }
        frameCB_.skyZenith[3] = frameCB_.skyHorizon[3] = 0;
        frameCB_.fogColor[3] = fogDensity;
    }

    MeshHandle createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) override;
    void drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) override;
    LineHandle createLineMesh(const LineVertex* verts, u32 count) override;
    void drawLines(LineHandle mesh, const f32 world[16]) override;
    void setWireframe(bool on) override { wireframe_ = on; }
    void setLineDepth(bool testDepth) override { lineDepth_ = testDepth; }
    void setGi(const GiSettings& gi) override;
    void setRayTracing(bool enabled) override { rtEnabled_ = enabled && rtSupported_; }
    bool rayTracingActive() const override { return rtActive_; }
    void setMeshShaders(bool enabled) override {
        const bool want = enabled && msSupported_;
        if (want != msActive_) AVER_INFO("[RHI.D3D12] geometry path: {}", want ? "mesh shaders" : "input assembler");
        msEnabled_ = msActive_ = want;
    }
    bool meshShadersActive() const override { return msActive_; }

    void requestCapture(u32 x, u32 y) override { capX_ = x; capY_ = y; captureReq_ = true; captureReady_ = false; }
    bool getCapture(f32 out[4]) override {
        if (!captureReady_) return false;
        for (int i = 0; i < 4; ++i) out[i] = captured_[i];
        return true;
    }
    bool getFrameImage(std::vector<u8>& out, u32& w, u32& h) override {
        if (frameImage_.empty()) return false;
        out = frameImage_; w = frameImageW_; h = frameImageH_;
        return true;
    }

    void beginFrame() override;
    void endFrame() override;
    void present();
    void resize(u32 w, u32 h);
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    bool selfTest(const f32 in[4], f32 out[4]) override;

private:
    void queryCaps();
    bool initRayTracing();
    bool initMeshShaders();
    void dispatchMesh(const GpuMesh& m);
    void buildBlas(u32 meshIndex);
    void buildRtScene();
    void bindGiTables();                        // bind heap + both descriptor tables (Tier 1 safety)
    void bindGraphicsRoot(ID3D12RootSignature* rs); // switch root signature + rebind shared params
    void voxelBarrier(u32 sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    bool createShadowResources();
    void shadowPass();
    bool createGiPipelines();
    bool createVoxelVolume(u32 res);
    void voxelizePass();
    bool createPipeline();
    bool createSwapchainResources(const SwapchainDesc& d);
    void createRenderTargetViews();
    bool createDepthBuffer();
    bool createMsaaColor();
    void waitForGpu();

    ComPtr<IDXGIFactory4> factory_;   // 6 is optional (see init); 4 is the baseline
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12DescriptorHeap> msaaRtvHeap_;
    ComPtr<ID3D12Resource> renderTargets_[kFrameCount];
    ComPtr<ID3D12Resource> msaaColor_;
    ComPtr<ID3D12Resource> depthBuffer_;
    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    // Wait-before-reuse frame sync: `nextFence_` is a single monotonic counter; signalled once
    // per submitted frame. `fenceValues_[i]` records the fence value that retires backbuffer i's
    // last frame, so beginFrame waits on exactly that before reusing the buffer. Robust across
    // ResizeBuffers (which resets the buffer index) — no back-buffer-parity assumptions.
    u64 fenceValues_[kFrameCount] = {0, 0};
    u64 nextFence_ = 0;
    u32 frameIndex_ = 0;
    u32 rtvSize_ = 0;

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> skyPso_;
    ComPtr<ID3D12PipelineState> wirePso_;
    ComPtr<ID3D12PipelineState> linePso_;
    ComPtr<ID3D12PipelineState> lineOverlayPso_; // no depth test: editor gizmos on top
    bool skyEnabled_ = false;
    bool wireframe_ = false;
    bool lineDepth_ = true;
    std::vector<GpuLineMesh> lineMeshes_;
    ComPtr<ID3D12Resource> frameCBs_[kFrameCount];
    u8* frameCBPtr_[kFrameCount] = {nullptr, nullptr};

    ComPtr<ID3D12Resource> captureBuf_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFp_{};
    bool captureReq_ = false, captureReady_ = false;
    u32 capX_ = 0, capY_ = 0;
    f32 captured_[4] = {0, 0, 0, 0};
    std::vector<u8> frameImage_;
    u32 frameImageW_ = 0, frameImageH_ = 0;

    ComPtr<ID3D12DescriptorHeap> uiSrvHeap_;
    bool uiActive_ = false;

    std::vector<GpuMesh> meshes_;
    PerFrameCB frameCB_{};
    u32 width_ = 0, height_ = 0;
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0; // scene sub-rect; w/h == 0 means full backbuffer
    u32 sampleCount_ = kDefaultSampleCount;     // live MSAA sample count (1 = off)
    DeviceCaps caps_{};

    // ---- DXR 1.1 inline ray tracing ----
    ComPtr<ID3D12Device5> device5_;
    ComPtr<ID3D12GraphicsCommandList4> cmdList4_;
    ComPtr<ID3D12PipelineState> rtPso_;

    // ---- Mesh shader geometry path (D3D12 Ultimate) ----
    // A separate root signature: a mesh-shader PSO may not use one declaring an input-assembler
    // layout, and the MS path adds two root SRVs (vertices, indices) plus a triangle count.
    ComPtr<ID3D12GraphicsCommandList6> cmdList6_;
    ComPtr<ID3D12RootSignature> msRootSig_;
    ComPtr<ID3D12PipelineState> msPso_, msVoxelPso_;
    bool msSupported_ = false, msEnabled_ = false, msActive_ = false;
    ID3D12RootSignature* boundRootSig_ = nullptr;   // raw: cache only, ownership stays in the ComPtrs
    ComPtr<ID3D12Resource> tlas_, tlasScratch_, instanceBuf_;
    std::vector<ComPtr<ID3D12Resource>> asScratch_;   // BLAS scratch, kept alive
    u64 tlasBytes_ = 0, tlasScratchBytes_ = 0, instanceBufBytes_ = 0;
    bool rtSupported_ = false, rtEnabled_ = false, rtActive_ = false;

    // ---- Voxi voxel-cone-traced GI ----
    GiSettings gi_{};
    ComPtr<ID3D12Resource> voxelTex_;             // R16G16B16A16_FLOAT Texture3D, mipped
    // Shader-visible heap: [0]=voxel SRV (t0), [1]=shadow SRV (t1), [2+m]=voxel mip m UAV (u0).
    ComPtr<ID3D12DescriptorHeap> giHeap_;
    // Directional shadow map: core FL11_0, so shadowed injection works on every DX12 GPU.
    ComPtr<ID3D12Resource> shadowTex_;
    ComPtr<ID3D12DescriptorHeap> shadowDsvHeap_;
    ComPtr<ID3D12PipelineState> shadowPso_;
    bool shadowReady_ = false;
    ComPtr<ID3D12PipelineState> voxelPso_, voxelDebugPso_, mipPso_, clearPso_;
    ComPtr<ID3D12RootSignature> mipRootSig_, clearRootSig_;
    u32 giSrvSize_ = 0, voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false;
    // Draws are replayed into the voxel volume at the start of the NEXT frame; a one-frame-old
    // volume is imperceptible and avoids restructuring the app's submission order.
    struct VoxelDraw { MeshHandle mesh; f32 world[16]; f32 color[4]; f32 metallic, roughness; };
    std::vector<VoxelDraw> voxelDraws_, voxelDrawsPrev_;
    bool hasSwapchain_ = false;
    f32 clear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    std::string adapterName_ = "D3D12 Device";
};

void D3D12Swapchain::present() { dev_->present(); }
void D3D12Swapchain::resize(u32 w, u32 h) { dev_->resize(w, h); }
u32 D3D12Swapchain::width() const { return dev_->width(); }
u32 D3D12Swapchain::height() const { return dev_->height(); }

bool D3D12Device::init(const DeviceDesc& desc) {
    UINT factoryFlags = 0;
    if (desc.enableDebug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            AVER_TRACE("[RHI.D3D12] debug layer enabled");
        }
    }
    if (!hrOk(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2")) return false;

    // IDXGIFactory6::EnumAdapterByGpuPreference needs Windows 10 1803+. Fall back to plain
    // EnumAdapters1 so older Windows (common on the DX12-era GPUs we target) still gets a device.
    ComPtr<IDXGIFactory6> factory6;
    const bool haveGpuPref = SUCCEEDED(factory_.As(&factory6));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; ; ++i) {
        if (haveGpuPref) {
            if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
        } else {
            if (factory_->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        }
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
            char name[128];
            std::wcstombs(name, ad.Description, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            adapterName_ = name;
            break;
        }
        adapter.Reset();
    }
    if (!device_) { AVER_WARN("[RHI.D3D12] no compatible hardware adapter"); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!hrOk(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue")) return false;

    if (!hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence")) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) { AVER_ERROR("[RHI.D3D12] CreateEvent failed"); return false; }

    queryCaps();
    if (!(caps_.msaaMask & sampleCount_)) sampleCount_ = 1; // fall back if 4x is unsupported

    // Sensible default light so meshes are lit before the app sets one.
    const f32 d[3] = {0.3f, 0.4f, 0.85f}, c[3] = {1, 1, 1};
    setLight(d, c, 0.15f);

    if (!createPipeline()) return false;
    if (!createShadowResources()) AVER_WARN("[RHI.D3D12] shadow map unavailable; lighting will be unshadowed");
    if (!createGiPipelines()) AVER_WARN("[RHI.D3D12] Voxi GI pipelines unavailable; GI disabled");

    AVER_INFO("[RHI.D3D12] device ready on adapter '{}'", adapterName_);
    return true;
}

// Ask the hardware what it can actually do, so the editor only offers real options.
void D3D12Device::queryCaps() {
    caps_ = {};
    caps_.computeShaders = true; // any D3D12 feature-level 11_0 device has compute
    caps_.msaaMask = 1;          // 1 sample always works
    caps_.maxMsaaSamples = 1;
    for (u32 s : {2u, 4u, 8u}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms{};
        ms.Format = kBackbufferFormat;
        ms.SampleCount = s;
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ms, sizeof(ms))) && ms.NumQualityLevels > 0) {
            caps_.msaaMask |= s;
            caps_.maxMsaaSamples = s;
        }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) {
        caps_.typedUavLoads = o.TypedUAVLoadAdditionalFormats != FALSE;
        caps_.conservativeRaster = o.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
    }
    // Highest shader model the driver accepts. DXC emits DXIL, which needs SM 6.0+; mesh
    // shaders need SM 6.5 and RayQuery needs SM 6.5, so this gates both.
    for (D3D_SHADER_MODEL sm : {D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0}) {
        D3D12_FEATURE_DATA_SHADER_MODEL q{sm};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &q, sizeof(q)))) {
            caps_.shaderModel = 60 + (static_cast<u32>(q.HighestShaderModel) & 0x0F);
            break;
        }
    }
    if (caps_.shaderModel < 60) caps_.shaderModel = 51;
    shaderCompiler().init();
    caps_.dxcAvailable = shaderCompiler().usingDxc();

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
        caps_.meshShaderTier = (o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1) ? 1u : 0u;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) {
        if (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1)      caps_.rayTracingTier = 11;
        else if (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) caps_.rayTracingTier = 10;
    }
    AVER_INFO("[RHI.D3D12] caps: MSAA {}x, RT tier {}, SM {}, mesh-shader tier {}, DXC {}, cons-raster {}",
              caps_.maxMsaaSamples, caps_.rayTracingTier, caps_.shaderModel,
              caps_.meshShaderTier, caps_.dxcAvailable, caps_.conservativeRaster);
}

// Rebuild everything that bakes the sample count: the MSAA colour/depth targets and every PSO.
bool D3D12Device::setSampleCount(u32 samples) {
    if (samples == sampleCount_) return true;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) return false;
    if (!(caps_.msaaMask & samples)) return false;

    waitForGpu(); // PSOs and targets are in flight until the GPU drains
    const u32 prev = sampleCount_;
    sampleCount_ = samples;
    if (!createPipeline()) { sampleCount_ = prev; createPipeline(); return false; } // PSOs carry SampleDesc
    if (rtSupported_) initRayTracing();   // the RayQuery PSO bakes SampleDesc too
    if (msSupported_) initMeshShaders();  // so does the mesh-shader PSO
    if (hasSwapchain_) {
        depthBuffer_.Reset();
        msaaColor_.Reset();
        if (!createDepthBuffer() || !createMsaaColor()) { AVER_ERROR("[RHI.D3D12] MSAA {}x target creation failed", samples); return false; }
    }
    AVER_INFO("[RHI.D3D12] MSAA set to {}x", samples);
    return true;
}

bool D3D12Device::createPipeline() {
    // Root signature: b0 = per-frame CBV, b1 = 20 root constants (world + colour).
    // b0 per-frame CBV, b1 root constants, t0 voxel volume (lit pass), u0 voxel volume (voxelise).
    D3D12_DESCRIPTOR_RANGE srvRange{}; srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvRange.NumDescriptors = 3; srvRange.BaseShaderRegister = 0; // t0 voxel, t1 shadow, t2 TLAS
    D3D12_DESCRIPTOR_RANGE uavRange{}; uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavRange.NumDescriptors = 1; uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 24;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &uavRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC shadowSamp{};
    shadowSamp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; // hardware PCF
    shadowSamp.AddressU = shadowSamp.AddressV = shadowSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSamp.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSamp.ShaderRegister = 1;
    shadowSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_STATIC_SAMPLER_DESC samplers[2] = {samp, shadowSamp};

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = samplers;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "SerializeRootSignature")) return false;
    if (!hrOk(device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)), "CreateRootSignature")) return false;
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "VSMain", "vs_5_1", &vs))) {
        return false;
    }
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "PSMain", "ps_5_1", &ps))) {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {layout, 2};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackbufferFormat;
    pso.DSVFormat = kDepthFormat;
    pso.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)), "CreateGraphicsPipelineState")) return false;

    // Sky pipeline: fullscreen triangle, no input layout, no depth.
    ComPtr<ID3DBlob> vsky, psky;
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "VSky", "vs_5_1", &vsky))) { return false;
    }
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "PSky", "ps_5_1", &psky))) { return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC sp{};
    sp.pRootSignature = rootSig_.Get();
    sp.VS = {vsky->GetBufferPointer(), vsky->GetBufferSize()};
    sp.PS = {psky->GetBufferPointer(), psky->GetBufferSize()};
    sp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    sp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    sp.RasterizerState.MultisampleEnable = TRUE;
    sp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    sp.DepthStencilState.DepthEnable = FALSE;
    sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    sp.SampleMask = UINT_MAX;
    sp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    sp.NumRenderTargets = 1;
    sp.RTVFormats[0] = kBackbufferFormat;
    sp.DSVFormat = kDepthFormat;
    sp.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&skyPso_)), "CreateGraphicsPipelineState(sky)")) return false;

    // Wireframe variant of the mesh PSO.
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&wirePso_)), "wire pso")) return false;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;

    // Line PSO (grid / gizmo): pos+colour, line list, depth-tested, no depth write.
    ComPtr<ID3DBlob> vln, pln;
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "VSLine", "vs_5_1", &vln))) { return false;
    }
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "PSLine", "ps_5_1", &pln))) { return false;
    }
    D3D12_INPUT_ELEMENT_DESC lineLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC lp{};
    lp.pRootSignature = rootSig_.Get();
    lp.VS = {vln->GetBufferPointer(), vln->GetBufferSize()};
    lp.PS = {pln->GetBufferPointer(), pln->GetBufferSize()};
    lp.InputLayout = {lineLayout, 2};
    lp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    lp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    lp.RasterizerState.MultisampleEnable = TRUE;
    lp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    lp.DepthStencilState.DepthEnable = TRUE;
    lp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    lp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    lp.SampleMask = UINT_MAX;
    lp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    lp.NumRenderTargets = 1;
    lp.RTVFormats[0] = kBackbufferFormat;
    lp.DSVFormat = kDepthFormat;
    lp.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&lp, IID_PPV_ARGS(&linePso_)), "line pso")) return false;

    // Overlay line PSO: same as above but no depth test — gizmos stay visible on top.
    lp.DepthStencilState.DepthEnable = FALSE;
    lp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (!hrOk(device_->CreateGraphicsPipelineState(&lp, IID_PPV_ARGS(&lineOverlayPso_)), "line overlay pso")) return false;

    // Per-frame constant buffers (one per frame in flight), persistently mapped.
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto cbd = bufferDesc(512); // PerFrameCB grew past 256 with the voxel fields
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frameCBs_[i])), "create per-frame CB")) return false;
        D3D12_RANGE none{0, 0};
        frameCBs_[i]->Map(0, &none, reinterpret_cast<void**>(&frameCBPtr_[i]));
    }
    return true;
}

// ---------------------------------------------------------------- DXR 1.1 (inline ray tracing)
namespace {
// Committed default-heap buffer sized for an acceleration structure or its scratch space.
ComPtr<ID3D12Resource> makeAsBuffer(ID3D12Device* dev, u64 bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 1;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}
} // namespace

bool D3D12Device::initRayTracing() {
    if (caps_.rayTracingTier < 11 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    if (FAILED(device_.As(&device5_))) return false;

    // Second PS variant compiled at SM 6.5 with AVER_RT so RayQuery is available. The SM 6.0
    // variant stays the default, so a device without DXR still gets a working renderer.
    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "VSMain", "vs_5_1", &vs))) return false;
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "PSMain", "ps_5_1", &ps, "ps_6_5", "AVER_RT=1"))) {
        AVER_WARN("[RHI.D3D12] RayQuery shader failed to compile; ray tracing stays off");
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rootSig_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.InputLayout = {layout, 2};
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.RasterizerState.MultisampleEnable = TRUE;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    p.SampleMask = UINT_MAX;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = kBackbufferFormat;
    p.DSVFormat = kDepthFormat;
    p.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&rtPso_)), "RayQuery pso")) return false;

    rtSupported_ = true;
    AVER_INFO("[RHI.D3D12] DXR 1.1 inline ray tracing ready (RayQuery, ps_6_5)");
    return true;
}

namespace {
// A mesh-shader PSO cannot be described by D3D12_GRAPHICS_PIPELINE_STATE_DESC; it needs the
// subobject-stream form. Rather than pull in d3dx12.h for two pipelines, this is the same
// alignas(void*) {type, value} pairing the header generates.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
    Subobject& operator=(const T& v) { value = v; return *this; }
};

struct MeshPsoStream {
    Subobject<ID3D12RootSignature*,     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE>    rootSig;
    Subobject<D3D12_SHADER_BYTECODE,    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS>                ms;
    Subobject<D3D12_SHADER_BYTECODE,    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS>                ps;
    Subobject<D3D12_RASTERIZER_DESC,    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER>        raster;
    Subobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL>     depth;
    Subobject<D3D12_BLEND_DESC,         D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND>             blend;
    Subobject<UINT,                     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK>       sampleMask;
    Subobject<D3D12_RT_FORMAT_ARRAY,    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rtvs;
    Subobject<DXGI_FORMAT,              D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT>  dsv;
    Subobject<DXGI_SAMPLE_DESC,         D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC>       sample;
};
} // namespace

// Mesh shader path. Needs Tier 1 + SM 6.5 + DXC, i.e. the same D3D12 Ultimate floor as RayQuery.
bool D3D12Device::initMeshShaders() {
    msSupported_ = false;
    if (caps_.meshShaderTier == 0 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    ComPtr<ID3D12Device2> device2;
    if (FAILED(device_.As(&device2))) return false;
    if (!cmdList6_) return false;   // DispatchMesh lives on ID3D12GraphicsCommandList6

    // Root signature WITHOUT the input-assembler flag (illegal with a mesh shader), plus two root
    // SRVs for the buffers the MS reads directly and a root constant for the triangle count.
    if (!msRootSig_) {
        D3D12_DESCRIPTOR_RANGE srvRange{}; srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvRange.NumDescriptors = 3; srvRange.BaseShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE uavRange{}; uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavRange.NumDescriptors = 1; uavRange.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER p[7] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[1].Constants.ShaderRegister = 1; p[1].Constants.Num32BitValues = 24;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable.NumDescriptorRanges = 1; p[2].DescriptorTable.pDescriptorRanges = &srvRange;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable.NumDescriptorRanges = 1; p[3].DescriptorTable.pDescriptorRanges = &uavRange;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[4].Descriptor.ShaderRegister = 3;  // vertices
        p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[5].Descriptor.ShaderRegister = 4;  // indices
        p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[6].Constants.ShaderRegister = 2; p[6].Constants.Num32BitValues = 4;
        for (auto& rp : p) rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC shadowSamp{};
        shadowSamp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSamp.AddressU = shadowSamp.AddressV = shadowSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        shadowSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadowSamp.MaxLOD = D3D12_FLOAT32_MAX; shadowSamp.ShaderRegister = 1; shadowSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_STATIC_SAMPLER_DESC samplers[2] = {samp, shadowSamp};

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 7; rsd.pParameters = p;
        rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = samplers;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> b, e;
        if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "ms root sig")) return false;
        if (!hrOk(device_->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&msRootSig_)), "ms root sig create")) return false;
    }

    // AVER_MS gates the mesh-shader entry points: their syntax is only legal from SM 6.5, so the
    // SM 5.1 compiles of this same source must not see them.
    ComPtr<ID3DBlob> ms, msVox, ps, psVox;
    auto ok = [&](const char* entry, const char* fxcTarget, const char* target, ComPtr<ID3DBlob>& out) {
        return SUCCEEDED(shaderCompiler().compile(kShaderHLSL, entry, fxcTarget, &out, target, "AVER_MS=1"));
    };
    if (!ok("MSMain",  "vs_5_1", "ms_6_5", ms)    ||
        !ok("MSVoxel", "vs_5_1", "ms_6_5", msVox) ||
        !ok("PSMain",  "ps_5_1", "ps_6_5", ps)    ||
        !ok("PSVoxel", "ps_5_1", "ps_6_5", psVox)) {
        AVER_WARN("[RHI.D3D12] mesh shaders failed to compile; the IA path stays in use");
        return false;
    }

    MeshPsoStream s{};
    s.rootSig = msRootSig_.Get();
    s.ms = D3D12_SHADER_BYTECODE{ms->GetBufferPointer(), ms->GetBufferSize()};
    s.ps = D3D12_SHADER_BYTECODE{ps->GetBufferPointer(), ps->GetBufferSize()};
    s.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    s.raster.value.CullMode = D3D12_CULL_MODE_NONE;
    s.raster.value.DepthClipEnable = TRUE;
    s.raster.value.MultisampleEnable = TRUE;
    s.depth.value.DepthEnable = TRUE;
    s.depth.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    s.depth.value.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    s.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    s.sampleMask = UINT_MAX;
    s.rtvs.value.NumRenderTargets = 1;
    s.rtvs.value.RTFormats[0] = kBackbufferFormat;
    s.dsv = kDepthFormat;
    s.sample.value.Count = sampleCount_;
    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
    if (!hrOk(device2->CreatePipelineState(&sd, IID_PPV_ARGS(&msPso_)), "mesh pso")) return false;

    // Voxelisation variant: no render target, no depth — the PS writes only the UAV.
    MeshPsoStream v{};
    v.rootSig = msRootSig_.Get();
    v.ms = D3D12_SHADER_BYTECODE{msVox->GetBufferPointer(), msVox->GetBufferSize()};
    v.ps = D3D12_SHADER_BYTECODE{psVox->GetBufferPointer(), psVox->GetBufferSize()};
    v.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    v.raster.value.CullMode = D3D12_CULL_MODE_NONE;
    v.raster.value.DepthClipEnable = FALSE;
    v.raster.value.ConservativeRaster = caps_.conservativeRaster
        ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    v.depth.value.DepthEnable = FALSE;
    v.sampleMask = UINT_MAX;
    v.rtvs.value.NumRenderTargets = 0;
    v.dsv = DXGI_FORMAT_UNKNOWN;
    v.sample.value.Count = 1;
    D3D12_PIPELINE_STATE_STREAM_DESC vd{sizeof(v), &v};
    if (!hrOk(device2->CreatePipelineState(&vd, IID_PPV_ARGS(&msVoxelPso_)), "mesh voxel pso")) return false;

    msSupported_ = true;
    AVER_INFO("[RHI.D3D12] mesh shader path ready (ms_6_5, GS-free voxelisation)");
    return true;
}

// One BLAS per mesh, built once (meshes are static). Must run on a command list, so it happens
// at the top of a frame rather than in createMesh.
void D3D12Device::buildBlas(u32 meshIndex) {
    GpuMesh& m = meshes_[meshIndex];
    if (m.blas || m.indexCount == 0) return;

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geo.Triangles.VertexBuffer.StartAddress = m.vb->GetGPUVirtualAddress();
    geo.Triangles.VertexBuffer.StrideInBytes = sizeof(MeshVertex);
    geo.Triangles.VertexCount = m.vbv.SizeInBytes / sizeof(MeshVertex);
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geo.Triangles.IndexBuffer = m.ib->GetGPUVirtualAddress();
    geo.Triangles.IndexCount = m.indexCount;
    geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = 1;
    in.pGeometryDescs = &geo;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    m.blas = makeAsBuffer(device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    ComPtr<ID3D12Resource> scratch = makeAsBuffer(device_.Get(), info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!m.blas || !scratch) { m.blas.Reset(); return; }
    asScratch_.push_back(scratch);   // keep alive until the GPU has consumed it

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = m.blas->GetGPUVirtualAddress();
    cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = m.blas.Get();
    cmdList_->ResourceBarrier(1, &b);
}

// TLAS is rebuilt every frame from the same draw list the shadow and voxel passes replay.
void D3D12Device::buildRtScene() {
    if (!rtSupported_ || !rtEnabled_ || voxelDrawsPrev_.empty()) { rtActive_ = false; return; }

    for (const VoxelDraw& d : voxelDrawsPrev_)
        if (d.mesh > 0 && d.mesh <= meshes_.size()) buildBlas(d.mesh - 1);

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> inst;
    inst.reserve(voxelDrawsPrev_.size());
    for (const VoxelDraw& d : voxelDrawsPrev_) {
        if (d.mesh == 0 || d.mesh > meshes_.size()) continue;
        const GpuMesh& m = meshes_[d.mesh - 1];
        if (!m.blas) continue;
        D3D12_RAYTRACING_INSTANCE_DESC id{};
        // Engine matrices are row-vector (v*M); DXR wants a 3x4 column-vector [R|T], i.e. the
        // transpose of the upper 3x3 with translation in the last column.
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) id.Transform[r][c] = d.world[c * 4 + r];
            id.Transform[r][3] = d.world[12 + r];
        }
        id.InstanceMask = 0xFF;
        id.AccelerationStructure = m.blas->GetGPUVirtualAddress();
        inst.push_back(id);
    }
    if (inst.empty()) { rtActive_ = false; return; }

    const u64 bytes = inst.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (!instanceBuf_ || instanceBufBytes_ < bytes) {
        auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd2 = bufferDesc(bytes);
        if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd2, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&instanceBuf_)), "TLAS instances")) { rtActive_ = false; return; }
        instanceBufBytes_ = bytes;
    }
    void* p = nullptr; D3D12_RANGE none{0, 0};
    instanceBuf_->Map(0, &none, &p);
    std::memcpy(p, inst.data(), bytes);
    instanceBuf_->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = static_cast<UINT>(inst.size());
    in.InstanceDescs = instanceBuf_->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (!tlas_ || tlasBytes_ < info.ResultDataMaxSizeInBytes) {
        tlas_ = makeAsBuffer(device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        tlasBytes_ = info.ResultDataMaxSizeInBytes;
        tlasScratch_.Reset();
        // t2 = the TLAS. A raytracing-AS SRV takes a NULL resource; the address lives in the desc.
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_UNKNOWN;
        sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.RaytracingAccelerationStructure.Location = tlas_->GetGPUVirtualAddress();
        D3D12_CPU_DESCRIPTOR_HANDLE h = giHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += 2ull * giSrvSize_;
        device_->CreateShaderResourceView(nullptr, &sv, h);
    }
    if (!tlasScratch_ || tlasScratchBytes_ < info.ScratchDataSizeInBytes) {
        tlasScratch_ = makeAsBuffer(device_.Get(), info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        tlasScratchBytes_ = info.ScratchDataSizeInBytes;
    }
    if (!tlas_ || !tlasScratch_) { rtActive_ = false; return; }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.ScratchAccelerationStructureData = tlasScratch_->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
    cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = tlas_.Get();
    cmdList_->ResourceBarrier(1, &b);
    rtActive_ = true;
}

// ---------------------------------------------------------------- shared GI/shadow binding
// Both descriptor tables are bound for every graphics pass. At Resource Binding Tier 1 (NVIDIA
// Kepler / Maxwell gen 1, Intel Haswell/Broadwell) every descriptor in every table a root
// signature declares must be valid even if the shader never reads it, so this is not optional.
// The mesh-shader path needs its own root signature, so the graphics root signature now changes
// mid-frame: meshes may use msRootSig_ while lines and the sky stay on rootSig_. Switching one
// resets every root binding, hence the re-bind here. Cached so a run of same-signature draws pays
// nothing.
void D3D12Device::bindGraphicsRoot(ID3D12RootSignature* rs) {
    if (boundRootSig_ == rs) return;
    boundRootSig_ = rs;
    cmdList_->SetGraphicsRootSignature(rs);
    cmdList_->SetGraphicsRootConstantBufferView(0, frameCBs_[frameIndex_]->GetGPUVirtualAddress());
    bindGiTables();
}

void D3D12Device::bindGiTables() {
    if (!giHeap_) return;
    ID3D12DescriptorHeap* heaps[] = {giHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = giHeap_->GetGPUDescriptorHandleForHeapStart();
    cmdList_->SetGraphicsRootDescriptorTable(2, gh);                       // t0 voxel, t1 shadow
    D3D12_GPU_DESCRIPTOR_HANDLE uav0 = gh; uav0.ptr += 3ull * giSrvSize_;  // u0 = mip 0
    cmdList_->SetGraphicsRootDescriptorTable(3, uav0);
}

// Per-subresource (per-mip) transition of the radiance volume.
void D3D12Device::voxelBarrier(u32 sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (!voxelTex_) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = voxelTex_.Get();
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = sub;
    cmdList_->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------- shadow map
constexpr u32 kShadowSize = 2048;

bool D3D12Device::createShadowResources() {
    // Descriptor heap is shared with the GI volume so both live in one shader-visible table.
    if (!giHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 3 + 16 + 16;   // t0 voxel, t1 shadow, t2 TLAS, per-mip UAVs, per-mip SRVs
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!hrOk(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&giHeap_)), "GI/shadow descriptor heap")) return false;
        giSrvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Populate EVERY slot with a null view. Resource Binding Tier 1 hardware requires all
        // descriptors in a bound table to be valid even when the shader never reads them, and the
        // voxel slots stay empty whenever GI is switched off.
        D3D12_CPU_DESCRIPTOR_HANDLE h = giHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC ns{};
        ns.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ns.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        ns.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ns.Texture3D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC nu{};
        nu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        nu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        nu.Texture3D.WSize = 1;
        for (u32 i = 0; i < 3u + 16u + 16u; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE d = h; d.ptr += static_cast<SIZE_T>(i) * giSrvSize_;
            if (i >= 3 && i < 19) device_->CreateUnorderedAccessView(nullptr, nullptr, &nu, d);
            else                  device_->CreateShaderResourceView(nullptr, &ns, d);
        }
    }
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kShadowSize; td.Height = kShadowSize;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;      // typeless: DSV writes D32, SRV reads R32
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&shadowTex_)), "shadow map")) return false;

    D3D12_DESCRIPTOR_HEAP_DESC dh{}; dh.NumDescriptors = 1; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!hrOk(device_->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&shadowDsvHeap_)), "shadow DSV heap")) return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowTex_.Get(), &dv, shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh = giHeap_->GetCPUDescriptorHandleForHeapStart();
    sh.ptr += giSrvSize_; // slot 1 = t1
    device_->CreateShaderResourceView(shadowTex_.Get(), &sv, sh);
    ComPtr<ID3DBlob> vs, err;
    if (FAILED(shaderCompiler().compile(kShaderHLSL, "VSShadow", "vs_5_1", &vs))) {
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC sp{};
    sp.pRootSignature = rootSig_.Get();
    sp.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};   // depth-only: no pixel shader
    sp.InputLayout = {layout, 2};
    sp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    sp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    sp.RasterizerState.DepthClipEnable = TRUE;
    sp.RasterizerState.DepthBias = 0;                 // float depth: rely on slope-scale + shader bias
    sp.RasterizerState.SlopeScaledDepthBias = 1.5f;
    sp.DepthStencilState.DepthEnable = TRUE;
    sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    sp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    sp.SampleMask = UINT_MAX;
    sp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    sp.NumRenderTargets = 0;
    sp.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    sp.SampleDesc.Count = 1;
    if (!hrOk(device_->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&shadowPso_)), "shadow pso")) return false;

    shadowReady_ = true;
    AVER_INFO("[RHI.D3D12] shadow map {}^2 ready", kShadowSize);
    return true;
}

// Render the scene depth from the sun, covering the GI volume's bounds.
void D3D12Device::shadowPass() {
    if (!shadowReady_ || voxelDrawsPrev_.empty()) { frameCB_.shadowParams[1] = 0.0f; return; }

    // Fit an orthographic light frustum around the volume so the map's resolution is spent where
    // the GI actually samples it.
    const Vec3 centre{gi_.center[0], gi_.center[1], gi_.center[2]};
    const f32 r = gi_.extent > 1.0f ? gi_.extent : 1.0f;
    Vec3 dir{frameCB_.lightDir[0], frameCB_.lightDir[1], frameCB_.lightDir[2]};
    dir = dir.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) dir = Vec3{0.3f, 0.4f, 0.85f}.getSafeNormal();
    const Vec3 eye = centre + dir * (r * 2.0f);              // gLightDir points TOWARD the light
    const Vec3 up = std::fabs(dir.z) > 0.95f ? Vec3{1,0,0} : Vec3{0,0,1};
    const Mat4 view = Mat4::lookAtLH(eye, centre, up);
    Mat4 proj;                                                // orthographic, row-vector convention
    proj.m[0][0] = 1.0f / r; proj.m[1][1] = 1.0f / r;
    proj.m[2][2] = 1.0f / (r * 4.0f); proj.m[3][2] = 0.0f; proj.m[3][3] = 1.0f;
    const Mat4 lvp = view * proj;
    std::memcpy(frameCB_.lightViewProj, &lvp.m[0][0], sizeof(frameCB_.lightViewProj));
    frameCB_.shadowParams[0] = 1.0f / static_cast<f32>(kShadowSize);
    frameCB_.shadowParams[1] = 1.0f;
    std::memcpy(frameCBPtr_[frameIndex_], &frameCB_, sizeof(PerFrameCB)); // re-upload with the matrix

    auto toDepth = transition(shadowTex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList_->ResourceBarrier(1, &toDepth);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    cmdList_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, static_cast<f32>(kShadowSize), static_cast<f32>(kShadowSize), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(kShadowSize), static_cast<LONG>(kShadowSize)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);
    // Resource Binding Tier 1 requires EVERY table the root signature declares to be bound, even
    // when the shader ignores them - otherwise this is UB on Kepler/Maxwell-gen1/Haswell.
    // (bindGraphicsRoot binds them all.) The shadow pass stays on the IA path: it is depth-only,
    // so a mesh-shader variant would buy nothing.
    bindGraphicsRoot(rootSig_.Get());
    cmdList_->SetPipelineState(shadowPso_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const VoxelDraw& d : voxelDrawsPrev_) {
        if (d.mesh == 0 || d.mesh > meshes_.size()) continue;
        const GpuMesh& m = meshes_[d.mesh - 1];
        f32 consts[24]{};
        std::memcpy(consts, d.world, 16 * sizeof(f32));
        cmdList_->SetGraphicsRoot32BitConstants(1, 24, consts, 0);
        cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
        cmdList_->IASetIndexBuffer(&m.ibv);
        cmdList_->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
    }
    auto toRead = transition(shadowTex_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList_->ResourceBarrier(1, &toRead);
}

// ---------------------------------------------------------------- Voxi GI
void D3D12Device::setGi(const GiSettings& gi) {
    gi_ = gi;
    if (gi_.enabled && gi_.resolution != voxelResBuilt_) {
        waitForGpu();
        if (!createVoxelVolume(gi_.resolution)) { gi_.enabled = false; giReady_ = false; }
    }
    // Push volume placement into the per-frame CB the shaders read. Done even when GI is off:
    // the shadow map fits its frustum to these same bounds.
    const f32 size = gi_.extent * 2.0f;
    frameCB_.voxelOrigin[0] = gi_.center[0] - gi_.extent;
    frameCB_.voxelOrigin[1] = gi_.center[1] - gi_.extent;
    frameCB_.voxelOrigin[2] = gi_.center[2] - gi_.extent;
    frameCB_.voxelOrigin[3] = 1.0f / size;                    // shader multiplies, so store the reciprocal
    frameCB_.voxelParams[0] = static_cast<f32>(voxelResBuilt_);
    frameCB_.voxelParams[1] = gi_.intensity;
    frameCB_.voxelParams[2] = gi_.maxDistance;
    frameCB_.voxelParams[3] = (giReady_ && !gi_.debugView) ? 1.0f : 0.0f; // gate the cone trace
}

// Cubic radiance volume with a full mip chain: mip N is the cone footprint at distance N.
bool D3D12Device::createVoxelVolume(u32 res) {
    giReady_ = false;
    voxelTex_.Reset();
    voxelMips_ = 1;
    for (u32 r = res; r > 1; r >>= 1) ++voxelMips_;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    td.Width = res; td.Height = res; td.DepthOrArraySize = static_cast<UINT16>(res);
    td.MipLevels = static_cast<UINT16>(voxelMips_);
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&voxelTex_)), "voxel volume")) return false;

    // Heap slots: [0] SRV over the whole chain, [1] shadow map SRV, [2 + m] UAV for mip m.
    if (!giHeap_) { AVER_ERROR("[RHI.D3D12] GI heap missing"); return false; }
    D3D12_CPU_DESCRIPTOR_HANDLE h = giHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = voxelMips_;
    device_->CreateShaderResourceView(voxelTex_.Get(), &sv, h);
    for (u32 m = 0; m < voxelMips_ && m < 16; ++m) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = td.Format;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uv.Texture3D.MipSlice = m;
        uv.Texture3D.WSize = res >> m;
        D3D12_CPU_DESCRIPTOR_HANDLE mh = h; mh.ptr += static_cast<SIZE_T>(3 + m) * giSrvSize_;
        device_->CreateUnorderedAccessView(voxelTex_.Get(), nullptr, &uv, mh);
        // Single-mip SRV so the mip filter can read level m while writing level m+1: a
        // whole-chain SRV would require every mip in the read state at once.
        D3D12_SHADER_RESOURCE_VIEW_DESC ms{};
        ms.Format = td.Format;
        ms.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        ms.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ms.Texture3D.MostDetailedMip = m;
        ms.Texture3D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh2 = h; sh2.ptr += static_cast<SIZE_T>(19 + m) * giSrvSize_;
        device_->CreateShaderResourceView(voxelTex_.Get(), &ms, sh2);
    }
    voxelResBuilt_ = res;
    giReady_ = voxelPso_ && mipPso_ && clearPso_;
    AVER_INFO("[RHI.D3D12] Voxi volume {}^3, {} mips", res, voxelMips_);
    return true;
}

bool D3D12Device::createGiPipelines() {
    ComPtr<ID3DBlob> vs, gs, ps;
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        if (FAILED(shaderCompiler().compile(kShaderHLSL, entry, target, &out))) {
            return false;
        }
        return true;
    };
    if (!compile("VSVoxel", "vs_5_1", vs) || !compile("GSVoxel", "gs_5_1", gs) || !compile("PSVoxel", "ps_5_1", ps)) return false;

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    // Rasterise with NO render target and no depth: the pixel shader's only output is the UAV.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC vp{};
    vp.pRootSignature = rootSig_.Get();
    vp.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    vp.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    vp.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    vp.InputLayout = {layout, 2};
    vp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    vp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    vp.RasterizerState.DepthClipEnable = FALSE;
    // Conservative raster makes thin/edge geometry still light up a voxel (tier checked in caps).
    vp.RasterizerState.ConservativeRaster = caps_.conservativeRaster
        ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    vp.DepthStencilState.DepthEnable = FALSE;
    vp.SampleMask = UINT_MAX;
    vp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    vp.NumRenderTargets = 0;
    vp.DSVFormat = DXGI_FORMAT_UNKNOWN;
    vp.SampleDesc.Count = 1;
    if (!hrOk(device_->CreateGraphicsPipelineState(&vp, IID_PPV_ARGS(&voxelPso_)), "voxel pso")) return false;

    // Debug: fullscreen raymarch of the volume (shares the sky's fullscreen VS).
    ComPtr<ID3DBlob> vsky, dbg;
    if (!compile("VSky", "vs_5_1", vsky) || !compile("PSVoxelDebug", "ps_5_1", dbg)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dp{};
    dp.pRootSignature = rootSig_.Get();
    dp.VS = {vsky->GetBufferPointer(), vsky->GetBufferSize()};
    dp.PS = {dbg->GetBufferPointer(), dbg->GetBufferSize()};
    dp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    dp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    dp.RasterizerState.MultisampleEnable = TRUE;
    dp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    dp.DepthStencilState.DepthEnable = FALSE;
    dp.SampleMask = UINT_MAX;
    dp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    dp.NumRenderTargets = 1;
    dp.RTVFormats[0] = kBackbufferFormat;
    dp.DSVFormat = kDepthFormat;
    dp.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&dp, IID_PPV_ARGS(&voxelDebugPso_)), "voxel debug pso")) return false;

    // Compute root signature for mip filtering: t0 (src mip) + u0 (dst mip).
    if (!mipRootSig_) {
        D3D12_DESCRIPTOR_RANGE sr{}; sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr.NumDescriptors = 1; sr.BaseShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE ur{}; ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ur.NumDescriptors = 1; ur.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER mp[3] = {};
        mp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        mp[0].DescriptorTable.NumDescriptorRanges = 1; mp[0].DescriptorTable.pDescriptorRanges = &sr;
        mp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        mp[1].DescriptorTable.NumDescriptorRanges = 1; mp[1].DescriptorTable.pDescriptorRanges = &ur;
        mp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b3 = source mip level
        mp[2].Constants.ShaderRegister = 3; mp[2].Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC md{}; md.NumParameters = 3; md.pParameters = mp;
        ComPtr<ID3DBlob> b, e2;
        if (!hrOk(D3D12SerializeRootSignature(&md, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e2), "mip root sig")) return false;
        if (!hrOk(device_->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&mipRootSig_)), "mip root sig create")) return false;
    }
    ComPtr<ID3DBlob> cs;
    if (!compile("CSMip", "cs_5_1", cs)) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = mipRootSig_.Get();
    cp.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (!hrOk(device_->CreateComputePipelineState(&cp, IID_PPV_ARGS(&mipPso_)), "mip pso")) return false;

    // Clear needs only u0. Its own root signature keeps the unused SRV table off the binding, so
    // nothing has to be bound to a descriptor pointing at a resource in the wrong state.
    if (!clearRootSig_) {
        D3D12_DESCRIPTOR_RANGE ur{}; ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ur.NumDescriptors = 1; ur.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER cpm{};
        cpm.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cpm.DescriptorTable.NumDescriptorRanges = 1; cpm.DescriptorTable.pDescriptorRanges = &ur;
        D3D12_ROOT_SIGNATURE_DESC cd{}; cd.NumParameters = 1; cd.pParameters = &cpm;
        ComPtr<ID3DBlob> b, e3;
        if (!hrOk(D3D12SerializeRootSignature(&cd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e3), "clear root sig")) return false;
        if (!hrOk(device_->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&clearRootSig_)), "clear root sig create")) return false;
    }
    ComPtr<ID3DBlob> csc;
    if (!compile("CSClear", "cs_5_1", csc)) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC clp{};
    clp.pRootSignature = clearRootSig_.Get();
    clp.CS = {csc->GetBufferPointer(), csc->GetBufferSize()};
    if (!hrOk(device_->CreateComputePipelineState(&clp, IID_PPV_ARGS(&clearPso_)), "voxel clear pso")) return false;

    AVER_INFO("[RHI.D3D12] Voxi pipelines ready (conservative raster {})", caps_.conservativeRaster ? "on" : "off");
    return true;
}

bool D3D12Device::createSwapchainResources(const SwapchainDesc& d) {
    if (!d.windowHandle) { AVER_WARN("[RHI.D3D12] createSwapchain without a window (headless)"); return false; }
    width_ = d.width; height_ = d.height;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_; sd.Height = height_;
    sd.Format = kBackbufferFormat;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrameCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;

    HWND hwnd = static_cast<HWND>(d.windowHandle);
    ComPtr<IDXGISwapChain1> sc1;
    if (!hrOk(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd")) return false;
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!hrOk(sc1.As(&swapChain_), "As IDXGISwapChain3")) return false;
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kFrameCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!hrOk(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "RTV heap")) return false;
    rtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dd{};
    dd.NumDescriptors = 1;
    dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!hrOk(device_->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsvHeap_)), "DSV heap")) return false;

    createRenderTargetViews();

    D3D12_DESCRIPTOR_HEAP_DESC mh{};
    mh.NumDescriptors = 1;
    mh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!hrOk(device_->CreateDescriptorHeap(&mh, IID_PPV_ARGS(&msaaRtvHeap_)), "MSAA RTV heap")) return false;

    if (!createDepthBuffer()) return false;
    if (!createMsaaColor()) return false;

    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])), "CreateCommandAllocator")) return false;
    }
    if (!hrOk(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&cmdList_)), "CreateCommandList")) return false;
    cmdList_.As(&cmdList4_);   // optional: only needed for acceleration-structure builds
    cmdList_.As(&cmdList6_);   // optional: only needed for DispatchMesh
    cmdList_->Close();
    if (cmdList4_) initRayTracing();
    if (cmdList6_) initMeshShaders();

    // Capture readback buffer sized to the backbuffer footprint.
    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    hrOk(device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_)), "capture buffer");

    fenceValues_[0] = fenceValues_[1] = 0; nextFence_ = 0; // no frames retired yet
    hasSwapchain_ = true;
    AVER_INFO("[RHI.D3D12] swapchain {}x{} + depth (D32) ({} buffers, FLIP_DISCARD)", width_, height_, kFrameCount);
    return true;
}

void D3D12Device::createRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < kFrameCount; ++i) {
        swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i]));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, rtv);
        rtv.ptr += rtvSize_;
    }
}

bool D3D12Device::createDepthBuffer() {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width_; td.Height = height_;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kDepthFormat; td.SampleDesc.Count = sampleCount_;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{}; cv.Format = kDepthFormat; cv.DepthStencil.Depth = 1.0f;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depthBuffer_)), "depth buffer")) return false;
    device_->CreateDepthStencilView(depthBuffer_.Get(), nullptr, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool D3D12Device::createMsaaColor() {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width_; td.Height = height_;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kBackbufferFormat; td.SampleDesc.Count = sampleCount_;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{}; cv.Format = kBackbufferFormat;
    cv.Color[0] = 0.10f; cv.Color[1] = 0.12f; cv.Color[2] = 0.16f; cv.Color[3] = 1.0f;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&msaaColor_)), "msaa color")) return false;
    device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

MeshHandle D3D12Device::createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) {
    if (!device_ || vcount == 0 || icount == 0) return 0;
    GpuMesh m;
    m.indexCount = icount;
    const u64 vbytes = static_cast<u64>(vcount) * sizeof(MeshVertex);
    const u64 ibytes = static_cast<u64>(icount) * sizeof(u32);
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);

    auto vd = bufferDesc(vbytes);
    if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.vb)), "mesh VB")) return 0;
    auto id = bufferDesc(ibytes);
    if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &id, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.ib)), "mesh IB")) return 0;

    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    m.vb->Map(0, &none, &p); std::memcpy(p, verts, vbytes); m.vb->Unmap(0, nullptr);
    m.ib->Map(0, &none, &p); std::memcpy(p, indices, ibytes); m.ib->Unmap(0, nullptr);

    m.vbv.BufferLocation = m.vb->GetGPUVirtualAddress();
    m.vbv.SizeInBytes = static_cast<UINT>(vbytes);
    m.vbv.StrideInBytes = sizeof(MeshVertex);
    m.ibv.BufferLocation = m.ib->GetGPUVirtualAddress();
    m.ibv.SizeInBytes = static_cast<UINT>(ibytes);
    m.ibv.Format = DXGI_FORMAT_R32_UINT;

    meshes_.push_back(std::move(m));
    return static_cast<MeshHandle>(meshes_.size()); // handle = index + 1
}

// Rasterise last frame's draws into the radiance volume, then build the mip chain.
void D3D12Device::voxelizePass() {
    // Note: an empty draw list still runs, so that a scene emptied of geometry clears rather than
    // keeping the last frame's radiance forever.
    if (!giReady_ || !gi_.enabled) return;
    const u32 res = voxelResBuilt_;

    // The chain rests in PIXEL_SHADER_RESOURCE between frames; take it back for writing.
    voxelBarrier(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_RESOURCE_BARRIER uav0{}; uav0.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav0.UAV.pResource = voxelTex_.Get();

    // Zero mip 0 first: injection writes only the voxels it covers, so stale radiance would
    // otherwise accumulate and moving objects would leave trails.
    {
        ID3D12DescriptorHeap* heaps[] = {giHeap_.Get()};
        cmdList_->SetDescriptorHeaps(1, heaps);   // this runs before bindGiTables()
        D3D12_GPU_DESCRIPTOR_HANDLE mip0 = giHeap_->GetGPUDescriptorHandleForHeapStart();
        mip0.ptr += static_cast<UINT64>(3) * giSrvSize_;
        cmdList_->SetComputeRootSignature(clearRootSig_.Get());
        cmdList_->SetPipelineState(clearPso_.Get());
        cmdList_->SetComputeRootDescriptorTable(0, mip0);
        const u32 g = (res + 3) / 4;
        cmdList_->Dispatch(g, g, g);
        cmdList_->ResourceBarrier(1, &uav0);   // injection must see the cleared volume
    }

    // Mesh shaders remove the geometry shader from voxelisation entirely - the reason this path
    // exists, since GS is emulated on every AMD GCN part.
    const bool useMs = msActive_ && msVoxelPso_;
    bindGraphicsRoot(useMs ? msRootSig_.Get() : rootSig_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gh = giHeap_->GetGPUDescriptorHandleForHeapStart();

    // No RTV/DSV: the pixel shader writes only the UAV. Viewport defines the raster resolution.
    cmdList_->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, static_cast<f32>(res), static_cast<f32>(res), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(res), static_cast<LONG>(res)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);
    cmdList_->SetPipelineState(useMs ? msVoxelPso_.Get() : voxelPso_.Get());
    if (!useMs) cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const VoxelDraw& d : voxelDrawsPrev_) {
        if (d.mesh == 0 || d.mesh > meshes_.size()) continue;
        const GpuMesh& m = meshes_[d.mesh - 1];
        f32 consts[24];
        std::memcpy(consts, d.world, 16 * sizeof(f32));
        std::memcpy(consts + 16, d.color, 4 * sizeof(f32));
        consts[20] = d.metallic; consts[21] = d.roughness; consts[22] = 0.0f; consts[23] = 0.0f;
        cmdList_->SetGraphicsRoot32BitConstants(1, 24, consts, 0);
        if (useMs) { dispatchMesh(m); continue; }
        cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
        cmdList_->IASetIndexBuffer(&m.ibv);
        cmdList_->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
    }

    // Build the mip chain: each level box-filters the one above it.
    D3D12_RESOURCE_BARRIER uavB{}; uavB.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uavB.UAV.pResource = voxelTex_.Get();
    cmdList_->ResourceBarrier(1, &uavB);
    cmdList_->SetComputeRootSignature(mipRootSig_.Get());
    cmdList_->SetPipelineState(mipPso_.Get());
    for (u32 m = 1; m < voxelMips_; ++m) {
        // Level m-1 becomes readable; level m stays writable. Per-subresource transitions are
        // what make read-while-write on one resource legal.
        voxelBarrier(m - 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_GPU_DESCRIPTOR_HANDLE src = gh; src.ptr += static_cast<UINT64>(19 + (m - 1)) * giSrvSize_;
        D3D12_GPU_DESCRIPTOR_HANDLE dst = gh; dst.ptr += static_cast<UINT64>(3 + m) * giSrvSize_;
        cmdList_->SetComputeRootDescriptorTable(0, src);
        cmdList_->SetComputeRootDescriptorTable(1, dst);
        const u32 srcMip[4] = {0, 0, 0, 0};   // the view already starts at the source mip
        cmdList_->SetComputeRoot32BitConstants(2, 4, srcMip, 0);
        const u32 d = (res >> m) > 0 ? (res >> m) : 1u;
        const u32 g = (d + 3) / 4;
        cmdList_->Dispatch(g, g, g);
        cmdList_->ResourceBarrier(1, &uavB);
    }
    // Hand the whole chain to the lit pass as a shader resource.
    voxelBarrier(voxelMips_ - 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    voxelBarrier(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Device::beginFrame() {
    if (!hasSwapchain_) return;
    // Render into whichever backbuffer is current now; wait for its previous frame to finish on
    // the GPU before recycling its allocator. This is the only fence wait in the frame and it
    // can never block on an unsignalled value (fenceValues_ only holds already-signalled ones).
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    const u64 want = fenceValues_[frameIndex_];
    if (want != 0 && fence_->GetCompletedValue() < want) {
        fence_->SetEventOnCompletion(want, fenceEvent_);
        if (WaitForSingleObject(fenceEvent_, 5000) == WAIT_TIMEOUT)
            AVER_ERROR("[RHI.D3D12] GPU frame wait timed out (device removed 0x{:08X})", static_cast<u32>(device_->GetDeviceRemovedReason()));
    }
    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get());
    boundRootSig_ = nullptr;   // a command-list reset drops every root binding

    // Scene renders into the MSAA color + depth targets; endFrame resolves to backbuffer.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    // Voxi runs BEFORE the lit pass so the volume is ready to cone-trace. It replays the previous
    // frame's draws (see voxelDrawsPrev_) and owns its own viewport/targets, so it must happen
    // before the scene RTV is bound below.
    std::memcpy(frameCBPtr_[frameIndex_], &frameCB_, sizeof(PerFrameCB));
    voxelDrawsPrev_.swap(voxelDraws_);
    voxelDraws_.clear();
    buildRtScene();   // BLAS/TLAS for RayQuery, from the same replayed draw list
    shadowPass();     // must precede voxelisation: injection samples the shadow map
    voxelizePass();

    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    // Always clear the FULL surface: once the scene is scissored to a sub-rect the sky no longer
    // covers every pixel, and anything outside would keep stale content from an earlier frame.
    cmdList_->ClearRenderTargetView(rtv, clear_, 0, nullptr);
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Scene renders into the requested sub-rect (the editor's central dock node), or the whole
    // backbuffer when none was set. Sky, meshes, grid and gizmos all share this one command list.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(width_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(height_);
    D3D12_VIEWPORT vp{rx, ry, rw, rh, 0.0f, 1.0f};
    D3D12_RECT sc{static_cast<LONG>(rx), static_cast<LONG>(ry), static_cast<LONG>(rx + rw), static_cast<LONG>(ry + rh)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);

    bindGraphicsRoot(rootSig_.Get()); // volume (t0) + shadow map (t1) for the lit pass
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Debug view replaces the whole scene with a raymarch of the volume.
    if (giReady_ && gi_.enabled && gi_.debugView) {
        cmdList_->SetPipelineState(voxelDebugPso_.Get());
        cmdList_->IASetVertexBuffers(0, 0, nullptr);
        cmdList_->DrawInstanced(3, 1, 0, 0);
        cmdList_->SetPipelineState(pso_.Get());
        return;
    }

    // Procedural sky first (fullscreen, no depth), then meshes draw over it.
    if (skyEnabled_) {
        cmdList_->SetPipelineState(skyPso_.Get());
        cmdList_->IASetVertexBuffers(0, 0, nullptr);
        cmdList_->DrawInstanced(3, 1, 0, 0);
    }
    cmdList_->SetPipelineState(pso_.Get());
}

void D3D12Device::drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) {
    if (!hasSwapchain_ || mesh == 0 || mesh > meshes_.size()) return;
    // Remember the draw so the shadow and voxel passes can replay it next frame. Captured
    // unconditionally: the shadow map needs it even when GI is switched off.
    if (voxelDraws_.size() < 4096) {
        VoxelDraw vd; vd.mesh = mesh;
        std::memcpy(vd.world, world, 16 * sizeof(f32));
        std::memcpy(vd.color, color, 4 * sizeof(f32));
        vd.metallic = metallic; vd.roughness = roughness;
        voxelDraws_.push_back(vd);
    }
    // The GI debug view replaces the scene with a raymarch of the volume, so skip the lit draw --
    // but only AFTER capturing above, or the volume would never be filled.
    if (gi_.enabled && gi_.debugView) return;
    const GpuMesh& m = meshes_[mesh - 1];
    // Wireframe and RayQuery only exist on the IA path, so they win over the mesh-shader toggle.
    const bool useMs = msActive_ && msPso_ && !wireframe_ && !(rtActive_ && rtPso_);
    bindGraphicsRoot(useMs ? msRootSig_.Get() : rootSig_.Get());
    cmdList_->SetPipelineState(useMs ? msPso_.Get()
                                     : (wireframe_ ? wirePso_.Get() : ((rtActive_ && rtPso_) ? rtPso_.Get() : pso_.Get())));
    f32 consts[24];
    std::memcpy(consts, world, 16 * sizeof(f32));
    std::memcpy(consts + 16, color, 4 * sizeof(f32));
    consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
    cmdList_->SetGraphicsRoot32BitConstants(1, 24, consts, 0);
    if (useMs) { dispatchMesh(m); return; }
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->IASetIndexBuffer(&m.ibv);
    cmdList_->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
}

// Mesh-shader draw: no input assembler, so the buffers go in as root SRVs and the group count is
// derived from the triangle count. Must match AVER_MS_TRIS in the shader.
void D3D12Device::dispatchMesh(const GpuMesh& m) {
    const u32 tris = m.indexCount / 3;
    if (!tris) return;
    cmdList_->SetGraphicsRootShaderResourceView(4, m.vb->GetGPUVirtualAddress());
    cmdList_->SetGraphicsRootShaderResourceView(5, m.ib->GetGPUVirtualAddress());
    const u32 tc[4] = {tris, 0, 0, 0};
    cmdList_->SetGraphicsRoot32BitConstants(6, 4, tc, 0);
    cmdList6_->DispatchMesh((tris + kMsTrisPerGroup - 1) / kMsTrisPerGroup, 1, 1);
}

LineHandle D3D12Device::createLineMesh(const LineVertex* verts, u32 count) {
    if (!device_ || count == 0) return 0;
    GpuLineMesh m;
    m.count = count;
    const u64 bytes = static_cast<u64>(count) * sizeof(LineVertex);
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto d = bufferDesc(bytes);
    if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.vb)), "line vb")) return 0;
    void* p = nullptr; D3D12_RANGE none{0, 0};
    m.vb->Map(0, &none, &p); std::memcpy(p, verts, bytes); m.vb->Unmap(0, nullptr);
    m.vbv.BufferLocation = m.vb->GetGPUVirtualAddress();
    m.vbv.SizeInBytes = static_cast<UINT>(bytes);
    m.vbv.StrideInBytes = sizeof(LineVertex);
    lineMeshes_.push_back(std::move(m));
    return static_cast<LineHandle>(lineMeshes_.size());
}

void D3D12Device::drawLines(LineHandle mesh, const f32 world[16]) {
    if (!hasSwapchain_ || mesh == 0 || mesh > lineMeshes_.size()) return;
    if (gi_.enabled && gi_.debugView) return; // grid/gizmo would overlay the volume raymarch
    const GpuLineMesh& m = lineMeshes_[mesh - 1];
    bindGraphicsRoot(rootSig_.Get()); // lines have no mesh-shader variant; a mesh draw may have switched
    cmdList_->SetPipelineState(lineDepth_ ? linePso_.Get() : lineOverlayPso_.Get());
    cmdList_->SetGraphicsRoot32BitConstants(1, 16, world, 0); // gWorld only
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->DrawInstanced(m.count, 1, 0, 0);
}

void D3D12Device::endFrame() {
    if (!hasSwapchain_) return;
    ID3D12Resource* bb = renderTargets_[frameIndex_].Get();

    // Move the scene target into the backbuffer. With MSAA on that is a resolve; with MSAA off
    // (1 sample) ResolveSubresource is illegal, so copy instead.
    const bool msaa = sampleCount_ > 1;
    const D3D12_RESOURCE_STATES srcState = msaa ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
    const D3D12_RESOURCE_STATES dstState = msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST   : D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_BARRIER pre[2] = {
        transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, srcState),
        transition(bb, D3D12_RESOURCE_STATE_PRESENT, dstState),
    };
    cmdList_->ResourceBarrier(2, pre);
    if (msaa) cmdList_->ResolveSubresource(bb, 0, msaaColor_.Get(), 0, kBackbufferFormat);
    else      cmdList_->CopyResource(bb, msaaColor_.Get());
    D3D12_RESOURCE_BARRIER post[2] = {
        transition(bb, dstState, D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(msaaColor_.Get(), srcState, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmdList_->ResourceBarrier(2, post);

#if AVER_WITH_IMGUI
    if (uiActive_) {
        ImGui::Render();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {uiSrvHeap_.Get()};
        cmdList_->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList_.Get());
    }
#endif
    if (captureReq_ && captureBuf_) {
        auto toCopy = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList_->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = captureBuf_.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = captureFp_;
        cmdList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto toPresent = transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        cmdList_->ResourceBarrier(1, &toPresent);
    } else {
        auto toPresent = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        cmdList_->ResourceBarrier(1, &toPresent);
    }
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
}

void D3D12Device::present() {
    if (!hasSwapchain_) return;
    swapChain_->Present(1, 0);

    // Mark this frame on the timeline and record it for the backbuffer we just rendered, so the
    // next beginFrame that recycles this buffer waits for exactly this frame to retire.
    queue_->Signal(fence_.Get(), ++nextFence_);
    fenceValues_[frameIndex_] = nextFence_;

    if (captureReq_ && captureBuf_) {
        waitForGpu(); // ensure the copy completed
        void* mapped = nullptr;
        // nullptr read-range = "may read whole resource" (avoids E_INVALIDARG when
        // RowPitch*height exceeds the buffer's copyable-footprint total).
        if (SUCCEEDED(captureBuf_->Map(0, nullptr, &mapped))) {
            const u32 x = capX_ < width_ ? capX_ : width_ - 1;
            const u32 y = capY_ < height_ ? capY_ : height_ - 1;
            const u8* base = static_cast<const u8*>(mapped);
            const u8* px = base + static_cast<SIZE_T>(y) * captureFp_.Footprint.RowPitch + static_cast<SIZE_T>(x) * 4;
            captured_[0] = px[0] / 255.0f; captured_[1] = px[1] / 255.0f; captured_[2] = px[2] / 255.0f; captured_[3] = px[3] / 255.0f;

            // Also keep the whole frame (tight RGBA) for screenshots.
            frameImageW_ = width_; frameImageH_ = height_;
            frameImage_.resize(static_cast<usize>(width_) * height_ * 4);
            for (u32 yy = 0; yy < height_; ++yy) {
                std::memcpy(&frameImage_[static_cast<usize>(yy) * width_ * 4],
                            base + static_cast<usize>(yy) * captureFp_.Footprint.RowPitch,
                            static_cast<usize>(width_) * 4);
            }
            captureBuf_->Unmap(0, nullptr);
            captureReady_ = true;
        }
        captureReq_ = false;
    }
}

void D3D12Device::resize(u32 w, u32 h) {
    if (!hasSwapchain_ || w == 0 || h == 0 || (w == width_ && h == height_)) return;
    waitForGpu(); // GPU idle: all backbuffer references retired before ResizeBuffers
    for (auto& rt : renderTargets_) rt.Reset();
    depthBuffer_.Reset();
    msaaColor_.Reset();
    if (!hrOk(swapChain_->ResizeBuffers(kFrameCount, w, h, kBackbufferFormat, 0), "ResizeBuffers")) return;
    width_ = w; height_ = h;
    vpX_ = vpY_ = vpW_ = vpH_ = 0; // drop the stale rect; the app re-pushes it next frame
    // GPU is idle, so no backbuffer has pending work; clear per-buffer fences (beginFrame reacquires
    // the current index and will not wrongly wait). No back-buffer-parity assumptions to break.
    for (u32 n = 0; n < kFrameCount; ++n) fenceValues_[n] = 0;
    createRenderTargetViews();
    createDepthBuffer();
    createMsaaColor();
    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_));
    AVER_TRACE("[RHI.D3D12] resized to {}x{}", w, h);
}

void D3D12Device::waitForGpu() {
    if (!queue_ || !fence_ || !fenceEvent_) return;
    const u64 v = ++nextFence_;
    if (FAILED(queue_->Signal(fence_.Get(), v))) return;
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fenceEvent_);
        if (WaitForSingleObject(fenceEvent_, 5000) == WAIT_TIMEOUT)
            AVER_ERROR("[RHI.D3D12] waitForGpu timed out (device removed 0x{:08X})", static_cast<u32>(device_->GetDeviceRemovedReason()));
    }
}

bool D3D12Device::uiInit(void* hwnd) {
#if AVER_WITH_IMGUI
    if (uiActive_) return true;
    if (!device_ || !hwnd) return false;

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.NumDescriptors = 1;
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!hrOk(device_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&uiSrvHeap_)), "UI SRV heap")) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // don't write imgui.ini into the cwd
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) { AVER_ERROR("[RHI.D3D12] ImGui_ImplWin32_Init failed"); return false; }

    ImGui_ImplDX12_InitInfo info{};
    info.Device = device_.Get();
    info.CommandQueue = queue_.Get(); // used for the font-atlas upload (ImGui 1.92)
    info.NumFramesInFlight = static_cast<int>(kFrameCount);
    info.RTVFormat = kBackbufferFormat;
    info.SrvDescriptorHeap = uiSrvHeap_.Get();
    info.LegacySingleSrvCpuDescriptor = uiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor = uiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&info)) {
        AVER_ERROR("[RHI.D3D12] ImGui_ImplDX12_Init failed");
        return false;
    }
    registerUiWndProc(&imguiWndProc);
    uiActive_ = true;
    AVER_INFO("[RHI.D3D12] ImGui UI initialised (docking)");
    return true;
#else
    (void)hwnd;
    return false;
#endif
}

void D3D12Device::uiNewFrame() {
#if AVER_WITH_IMGUI
    if (!uiActive_) return;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
#endif
}

void D3D12Device::uiShutdown() {
#if AVER_WITH_IMGUI
    if (!uiActive_) return;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    registerUiWndProc(nullptr);
    uiActive_ = false;
#endif
}

bool D3D12Device::uiWantsMouse() const {
#if AVER_WITH_IMGUI
    return uiActive_ && ImGui::GetIO().WantCaptureMouse;
#else
    return false;
#endif
}

bool D3D12Device::uiWantsKeyboard() const {
#if AVER_WITH_IMGUI
    return uiActive_ && ImGui::GetIO().WantCaptureKeyboard;
#else
    return false;
#endif
}

bool D3D12Device::selfTest(const f32 in[4], f32 out[4]) {
    constexpr UINT kW = 8, kH = 8;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW; td.Height = kH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kBackbufferFormat; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{}; cv.Format = kBackbufferFormat;
    for (int i = 0; i < 4; ++i) cv.Color[i] = in[i];

    auto defHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> rt;
    if (!hrOk(device_->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)), "selfTest RT")) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (!hrOk(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "selfTest RTV heap")) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(rt.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    ComPtr<ID3D12Resource> readback;
    if (!hrOk(device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "selfTest readback")) return false;

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "selfTest alloc")) return false;
    if (!hrOk(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "selfTest list")) return false;

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, in, 0, nullptr);
    auto toCopy = transition(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = rt.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue_->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> f;
    if (!hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "selfTest fence")) return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue_->Signal(f.Get(), 1);
    if (f->GetCompletedValue() < 1) { f->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, INFINITE); }
    CloseHandle(ev);

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(fp.Footprint.RowPitch)};
    if (!hrOk(readback->Map(0, &range, &mapped), "selfTest Map")) return false;
    const auto* px = static_cast<const u8*>(mapped);
    out[0] = px[0] / 255.0f; out[1] = px[1] / 255.0f; out[2] = px[2] / 255.0f; out[3] = px[3] / 255.0f;
    readback->Unmap(0, nullptr);
    return true;
}

} // namespace

namespace detail {
IDevice* createD3D12Device(const DeviceDesc& desc) {
    auto* dev = new D3D12Device();
    if (!dev->init(desc)) { delete dev; return nullptr; }
    return dev;
}
} // namespace detail

} // namespace aver::rhi
