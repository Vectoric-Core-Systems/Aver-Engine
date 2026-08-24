// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// DirectX 12 backend for Aver.RHI: device, swapchain, scene pipelines, the camera post chain,
// and the generic resource factory and render context. Hand-rolled D3D12 structs (no d3dx12.h).
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <string>
#include <wrl/client.h>

#include <algorithm>   // std::find, for the once-per-shape binding warning below
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The abstract seam a UI toolkit's D3D12 backend plugs into. This file has no ImGui include and no
// ImGui symbol anywhere in it -- see UiBackend.hpp for why, and modules/rhi.d3d12.imgui for where
// Dear ImGui itself now lives.
#include "aver/rhi/d3d12/UiBackend.hpp"

using Microsoft::WRL::ComPtr;

namespace aver::rhi {
namespace {

constexpr u32 kFrameCount = 2;
constexpr u32 kDefaultSampleCount = 4;
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
// The RESOURCE format the depth buffer is actually created with -- typeless, so it can carry BOTH a
// D32_FLOAT depth-stencil view (kDepthFormat, used everywhere above) AND an R32_FLOAT shader-resource
// view (IDevice::sceneDepthTexture(), read by modules/occlusion's HZB seed pass). The identical
// "one resource, two views" trick VoxiRenderer::createShadowResources already uses for its own shadow
// map, extended here to a resource that is also allowed to be MULTISAMPLED — a D32_FLOAT-created
// (non-typeless) resource can only ever be viewed as D32_FLOAT, so before sceneDepthTexture() existed
// there was no reason for this to be anything but the concrete format, and no consumer had ever asked
// to read it as a shader resource at all. See createDepthBuffer's own comment for the DSV-side
// consequence (an explicit view desc, where a bare `nullptr` used to suffice).
constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;

// Scene target format: linear HDR radiance, which the post chain tonemaps into the backbuffer.
constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Bloom pyramid depth cap.
constexpr u32 kMaxBloomMips = 6;
// Descriptor triples the post heap holds: prefilter, histogram, composite, then one per
// downsample and one per upsample. Each triple is contiguous, as a root table must be.
// +1 for kPostTripleCompositeUpscaled: the composite needs a SECOND triple because its scene SRV
// points at a different texture on the AverSR path (presentHdrTex_) than on the default one
// (`scene`), and a triple is contiguous by definition -- it cannot be rewritten per frame without
// racing the frames still in flight that were recorded against the old contents.
constexpr u32 kPostTripleCount = 4 + (kMaxBloomMips - 1) * 2;
constexpr u32 kPostDescriptorCount = kPostTripleCount * 3 + 2;   // + the histogram/exposure UAVs
// Which triple is which. The bloom ones are ranges based at these.
constexpr u32 kPostTriplePrefilter = 0;
constexpr u32 kPostTripleHistogram = 1;
constexpr u32 kPostTripleComposite = 2;
// The composite's AverSR twin: identical bindings except the scene SRV, which points at the
// already-upscaled present-resolution HDR image instead of the scene-resolution one.
constexpr u32 kPostTripleCompositeUpscaled = 3;
constexpr u32 kPostTripleDownBase  = 4;
constexpr u32 kPostTripleUpBase    = kPostTripleDownBase + (kMaxBloomMips - 1);
// The luminance window the histogram bins over, in log2. Anything outside lands in the end bins.
constexpr f32 kHistogramMinLogLum = -10.0f;
constexpr f32 kHistogramMaxLogLum = 12.0f;
// One histogram thread per four pixels each way.
constexpr u32 kHistogramDownscale = 4;

// ---------------------------------------------------------------- shader compilation
// Compiles HLSL through DXC (shader model 6.x) when dxcompiler.dll is present, else FXC (SM 5.1).
class ShaderCompiler {
public:
    // Loads DXC once, or settles on FXC.
    void init() {
        if (tried_) return;
        tried_ = true;
        if (capsOverride().active && capsOverride().noDxc) {
            AVER_INFO("[RHI.D3D12] shader compiler: FXC (SM 5.1) - DXC suppressed by --force-caps no-dxc");
            return;
        }
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

    // Compiles one entry point to bytecode. `target51` is the FXC target and the SM6 one is derived
    // from it; `sm6` overrides that and requires DXC. `define` is a semicolon-separated -D list.
    HRESULT compile(const char* src, const char* entry, const char* target51, ID3DBlob** out,
                    const char* sm6 = nullptr, const char* define = nullptr) {
        init();
        std::vector<std::string> defs;
        if (define) {
            const std::string all(define);
            for (size_t b = 0; b <= all.size();) {
                const size_t e = std::min(all.find(';', b), all.size());
                if (e > b) defs.emplace_back(all, b, e - b);
                b = e + 1;
            }
        }
        if (!usingDxc()) {
            if (sm6) return E_NOTIMPL;
            std::vector<std::string> names, values;
            names.reserve(defs.size()); values.reserve(defs.size());
            for (const std::string& d : defs) {
                const size_t eq = d.find('=');
                names.push_back(eq == std::string::npos ? d : d.substr(0, eq));
                values.push_back(eq == std::string::npos ? std::string("1") : d.substr(eq + 1));
            }
            std::vector<D3D_SHADER_MACRO> macros;
            macros.reserve(names.size() + 1);
            for (size_t i = 0; i < names.size(); ++i) macros.push_back({names[i].c_str(), values[i].c_str()});
            macros.push_back({nullptr, nullptr});
            ComPtr<ID3DBlob> err;
            const UINT flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
            HRESULT hr = D3DCompile(src, std::strlen(src), "aver.hlsl", macros.data(), nullptr, entry, target51, flags, 0, out, &err);
            if (FAILED(hr) && err) AVER_ERROR("[RHI.D3D12] {} ({}): {}", entry, target51, static_cast<const char*>(err->GetBufferPointer()));
            return hr;
        }
        std::string t6(target51);
        const size_t us = t6.rfind("_5_1");
        if (us != std::string::npos) t6 = t6.substr(0, us) + "_6_0";
        if (sm6) t6 = sm6;
        const std::wstring wEntry(entry, entry + std::strlen(entry));
        const std::wstring wTarget(t6.begin(), t6.end());
        std::vector<std::wstring> wDefines;
        for (const std::string& d : defs) wDefines.emplace_back(d.begin(), d.end());

        DxcBuffer buf{src, std::strlen(src), DXC_CP_UTF8};
        std::vector<LPCWSTR> args = {
            L"-E", wEntry.c_str(),
            L"-T", wTarget.c_str(),
            L"-Zpr",
            L"-HV", L"2021",
        };
        for (const std::wstring& d : wDefines) { args.push_back(L"-D"); args.push_back(d.c_str()); }
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

// The process-wide shader compiler.
ShaderCompiler& shaderCompiler() { static ShaderCompiler c; return c; }

// Logs and returns false on a failed HRESULT.
bool hrOk(HRESULT hr, const char* what) {
    if (FAILED(hr)) { AVER_ERROR("[RHI.D3D12] {} failed (hr=0x{:08X})", what, static_cast<u32>(hr)); return false; }
    return true;
}

// A whole-resource transition barrier.
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

// Single-node heap properties of the given type.
D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

// The former imguiWndProc/UiSrvPool/uiSrvAlloc/uiSrvFree lived here, gated on #if AVER_WITH_IMGUI.
// They moved to modules/rhi.d3d12.imgui/src/ImGuiUiBackend.cpp -- this file has no ImGui symbol left
// to gate. The single live UI backend's Win32 thunk and the descriptor-pool sharing it used to do
// directly now go through d3d12::IUiBackend (UiBackend.hpp); see uiBackendWndProcThunk and
// D3D12ResourceFactory::uiDescriptor further down for where each half landed.

// Writes the default shading model and its parameters into the tail of a per-draw b1 block.
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD in the material prelude
    std::memcpy(block + 24, &model, sizeof(model));   // a uint in the block, not a converted float
    block[25] = 0.04f;
    block[26] = 1.0f;
    block[27] = 0.0f;
    block[28] = block[29] = block[30] = block[31] = 0.0f;   // emissive
}

// Resource description for a linear buffer of `bytes`.
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

const char* kShaderHLSL = R"(
// Shades a scene surface: unshadowed, no bounce, through the prelude's FROZEN plainShadeSurface.
float4 PSMainPlain(VSOut i) : SV_TARGET { return plainShadeSurface(i, 1.0, float3(0,0,0), 1.0); }

// ================= volumetric clouds =================
// One raymarched layer, evaluated on sky pixels only, with analytic noise so it needs no SRV.
//
// LIVES HERE, NOT IN THE SHARED PRELUDE, ON PURPOSE: PSky below is the only caller anywhere in the
// engine. Fog and sky-ambient math stay in rhi::sharedShaderPrelude() because ordinary surface
// shading genuinely needs them (averApplyFog, averSkyIrradiance -- called from Voxi, PBR, every
// lit pixel shader there is); nothing outside the sky pass itself ever touches a cloud. Putting
// cloud code in the shared prelude would paste a 24-step raymarch and its noise functions into
// every module's shader source -- Voxi, PBR, UI, ActorPreview, the path tracer -- none of which
// call it, for no reason but that the fog/atmosphere code next to it in the file genuinely is
// shared. gCloudParams/gCloudMotion themselves still live in the shared PerFrame cbuffer (see
// their own comment there for why splitting the DATA out is separable follow-up work from moving
// the CODE that reads it).

// Hashes a 3D point to a scalar in [0,1).
float averHash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// Value noise with a smoothstep-interpolated lattice. Eight hashes a call.
float averValueNoise(float3 x) {
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = averHash13(i + float3(0,0,0)), n100 = averHash13(i + float3(1,0,0));
    float n010 = averHash13(i + float3(0,1,0)), n110 = averHash13(i + float3(1,1,0));
    float n001 = averHash13(i + float3(0,0,1)), n101 = averHash13(i + float3(1,0,1));
    float n011 = averHash13(i + float3(0,1,1)), n111 = averHash13(i + float3(1,1,1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

// Extinction per world unit for a fully dense cloud, derived from the layer's thickness.
float averCloudSigma() {
    const float kOpticalDepthAtFull = 9.0;   // a dense cumulus, edge to edge
    return gCloudParams.y * kOpticalDepthAtFull / max(gCloudParams.w - gCloudParams.z, 1.0);
}

// Cloud density at a world point. `detail` buys a second noise octave; the light march skips it.
float averCloudDensity(float3 wpos, bool detail) {
    float bottom = gCloudParams.z, top = gCloudParams.w;
    float h = saturate((wpos.z - bottom) / max(top - bottom, 1.0));
    float shape = saturate(h * 4.0) * saturate((1.0 - h) * 1.6);
    if (shape <= 0.001) return 0.0;

    float3 p = (wpos + float3(gCloudMotion.xy, 0.0)) * gCloudMotion.z;
    float cover = 1.0 - gCloudParams.x;

    // The cheapest octave, evaluated FIRST rather than last, as a conservative reject: `n` is at
    // most nLow + 0.6 (the base octave's full weight) + 0.3-or-0.15 (the detail octave's, if this
    // call even asked for one) -- every value-noise call returns in [0,1], so that is the best
    // case no matter what the other two octaves turn out to be. If even that best case cannot
    // clear `cover`, the remaining one or two noise samples this step would have bought are
    // guaranteed to leave d at 0 -- skip them. This never skips a point that would have been
    // nonzero; it only ever skips a point already proven to be empty.
    float nLow = averValueNoise(p * 0.41) * 0.25;
    const float bestCase = nLow + (detail ? 0.9 : 0.75);
    if (bestCase <= cover) return 0.0;

    float n = averValueNoise(p) * 0.6 + nLow;
    if (detail) n += averValueNoise(p * 3.17) * 0.3;
    else        n += 0.15;

    float d = saturate((n - cover) / max(1.0 - cover, 1e-3));
    return d * shape;
}

// Henyey-Greenstein phase function.
float averHG(float ct, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * ct, 1e-4), 1.5));
}

// Marches the layer and returns scattered radiance in rgb, TRANSMITTANCE in a. `outDist` comes back
// as the distance the scattering actually happened at, weighted by how much each step contributed,
// so the caller can put the right amount of air in front of it.
float4 averCloudLayer(float3 ro, float3 rd, float3 sunDir, float3 sunColour, out float outDist) {
    outDist = 0.0;
    if (gCloudMotion.w < 0.5) return float4(0, 0, 0, 1);
    float bottom = gCloudParams.z, top = gCloudParams.w;

    float t0, t1;
    if (rd.z > 1e-4) {
        if (ro.z > top) return float4(0, 0, 0, 1);
        t0 = max((bottom - ro.z) / rd.z, 0.0);
        t1 = (top - ro.z) / rd.z;
    } else if (rd.z < -1e-4) {
        if (ro.z < bottom) return float4(0, 0, 0, 1);
        t0 = max((top - ro.z) / rd.z, 0.0);
        t1 = (bottom - ro.z) / rd.z;
    } else {
        if (ro.z < bottom || ro.z > top) return float4(0, 0, 0, 1);
        t0 = 0.0; t1 = (top - bottom) * 64.0;
    }
    // Was 24: cut a third, relying on the jitter below (already there, already hiding banding as
    // noise instead of visible steps) to absorb the coarser sampling rather than adding anything
    // new to hide it.
    const int kSteps = 16;
    float featureSize = 1.0 / max(gCloudMotion.z, 1e-9);
    t1 = min(t1, t0 + kSteps * featureSize * 0.35);
    if (t1 <= t0) return float4(0, 0, 0, 1);

    float dt = (t1 - t0) / kSteps;
    float jitter = averHash13(rd * 811.7);
    float t = t0 + dt * jitter;

    float sigma = averCloudSigma();
    float3 scattered = 0.0;
    float transmittance = 1.0;
    // WAS 0.62. That still left the sky broadly white across a whole quadrant, not just around the
    // sun disk: with the sun at 59.5 degrees elevation, a near-zenith view keeps a wide swath of
    // view rays within a moderate angle of sunDir, and HG at g=0.62 stays meaningfully elevated out
    // past 45 degrees off-forward (phase(60 deg) is still ~0.07 against an isotropic average of
    // ~0.08 -- barely fallen at all). Rebalancing the sun/ambient weights below moved the pixel I
    // measured but could not fix that, because the problem was never how MUCH direct light there
    // was at any one point, it was how WIDE an area was getting a meaningful dose of it. 0.85 is a
    // realistic Mie asymmetry for actual water droplets (0.62 was already below the usual 0.75-0.85
    // range) and its narrower lobe is what a narrower dose looks like: phase(60 deg) drops to ~0.03,
    // under half of 0.62's, while the peak at dead-forward rises -- the glow right around the sun
    // gets brighter and tighter instead of smearing across the whole sky.
    float phase = averHG(dot(rd, sunDir), 0.85);
    float distWeight = 0.0;
    // THE SAME BRANCH PSky ITSELF TAKES A FEW LINES DOWN -- averSkyPhysical UNDER A PHYSICAL SKY,
    // skyColorFull ONLY as the authored fallback -- not skyColorFull unconditionally the way this
    // read before. skyColorFull is a two-colour gradient an artist set by hand; under `SKY model
    // physical` the real answer for "what colour is the sky overhead" is the Rayleigh/Mie/ozone
    // march every other physical surface in this file already asks for, and clouds lighting
    // themselves off the authored gradient instead was exactly the brute-forced colour this cloud
    // pass was supposed to have stopped doing. It reads as a flat, pale grey-white deck instead of
    // blue precisely because the one input that WAS carrying blue -- real Rayleigh scattering, which
    // is blue for a physical reason (short wavelengths scatter harder) -- was never being asked for.
    float3 ambientTop = averAtmoOn() ? averSkyPhysical(float3(0, 0, 1)) : skyColorFull(float3(0, 0, 1));

    [loop] for (int i = 0; i < kSteps; ++i) {
        if (transmittance < 0.02) break;
        float3 p = ro + rd * t;
        float d = averCloudDensity(p, true);
        if (d > 0.001) {
            float lt = 0.0;
            // Was 3 steps at 0.25*(top-bottom): samples at 0.5/1.5/2.5 step-widths ahead reached
            // (2.5+0.5)*0.25 = 0.75 of the layer's thickness toward the sun. 2 steps at a wider
            // 0.375*(top-bottom) sample 0.5/1.5 step-widths ahead, reaching (1.5+0.5)*0.375 = 0.75
            // -- the same total reach, one fewer (and this loop's most expensive) sample.
            float lstep = (top - bottom) * 0.375;
            [unroll] for (int j = 0; j < 2; ++j) {
                float3 lp = p + sunDir * (lstep * (j + 0.5));
                lt += averCloudDensity(lp, false) * lstep;
            }
            float sunT = exp(-lt * sigma);

            // NO POWDER TERM ON THE SUN ANY MORE, AND THAT IS WHY THIS LAYER HAS FORM AGAIN. It used
            // to be 1-exp(-2*d*dt*sigma) -- built from the density of THIS step -- and the result was
            // then multiplied by (1-stepT), which is 1-exp(-d*dt*sigma) from the same quantity. Two
            // factors, both going to zero with density, so the direct sun arrived scaled by roughly
            // density SQUARED while the ambient beside it scaled linearly. Wherever the layer was
            // thin -- which is most of what a coverage dial around a half actually draws -- the sun
            // term was squared away to nothing and the pixel was very nearly pure flat ambient. That
            // is the washed-out grey smear: not a missing effect, an extra factor. Single scatter
            // wants the one (1-stepT) below and nothing else.
            //
            // The form comes from the ambient instead, where it physically belongs. A sample near the
            // top of the layer sees most of the sky dome; one near the base sees it through every
            // metre of cloud above it, which is exactly why real cloud bottoms are grey and their
            // tops are white. Squaring the height ratio keeps the darkening in the lower half of the
            // layer rather than spreading it evenly. The old flat 0.9 of the zenith at every sample,
            // base and top alike, is what made this read as one uniform slab of fog.
            float hN = saturate((p.z - bottom) / max(top - bottom, 1.0));

            // THE 0.6 AND THE 3.0 ARE A MEASURED CORRECTION, NOT A GUESS. Isolating each term at a
            // pixel deep in the deck showed ambient alone reads a genuine sky blue (44,66,92) while
            // the full sum reads within a few codes of sun ALONE (145,144,140 vs 142,140,134) -- the
            // direct term was simply several times the ambient term's magnitude, so summing them
            // left ambient invisible whatever hue it carried, and that ratio does not move with
            // exposure: scaling both terms together by a common exposure factor cannot change their
            // RELATIVE weight, which is confirmed by testing exposures from 0.45 to 1.0 with the
            // original weights and getting the same near-neutral hue every time. What single-scatter
            // sun and single-scatter sky can never reproduce on their own is what makes a real cloud's
            // shadowed interior read blue-grey: MULTIPLE scattering, light bouncing many times through
            // the droplets before it escapes, which this pass does not simulate and which is most of
            // a real cloud's brightness. ambientTop is standing in for all of that missing bounce
            // light, not only the one sky-dome term its name suggests, so it was underweighted twice
            // over. Direct scaled down and ambient scaled up together land the shadowed underside of a
            // thick cloud as the blue-grey UE's own clouds show there, without the deck going dim and
            // moody the way scaling direct down alone did (tried at 0.3 with ambient untouched: the
            // hue was right, the whole sky read like dusk). This alone was not the whole fix -- see
            // where `phase` above is computed for the other half of it, the WIDTH of the area getting
            // any direct light at all, which this ratio cannot touch.
            float3 lit = sunColour * sunT * phase * 0.6 + ambientTop * lerp(0.12, 0.55, hN * hN) * 3.0;

            float stepT = exp(-d * dt * sigma);
            float w = transmittance * (1.0 - stepT);
            scattered += w * lit;
            outDist += w * t;
            distWeight += w;
            transmittance *= stepT;
        }
        t += dt;
    }
    outDist = distWeight > 1e-6 ? outDist / distWeight : t0;
    return float4(scattered, transmittance);
}

// Procedural sky for a fullscreen triangle. The only place the full scattering integral runs.
float4 PSky(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float3 L = normalize(gLightDir.xyz);
    float3 sky = averAtmoOn() ? averSkyPhysical(ray) : skyColor(ray);
    float sd = saturate(dot(ray, L));
    float3 sunC = srgbToLin(gLightColor.rgb) * gSkyParams.z;
    float cosR = gSkyParams.w;
    float disk = smoothstep(cosR - 0.0004, cosR + 0.0002, sd);
    sky += sunC * disk * 14.0;
    if (!averAtmoOn()) sky += sunC * pow(sd, 12.0) * 0.30;

    // THE DECK GETS THE AIR IN FRONT OF IT, like every other surface in the renderer. Without this
    // the cloud radiance was composited straight onto the sky at full strength however far away it
    // was, and near the horizon "far away" is tens of kilometres -- the deck stayed as crisp and as
    // bright at the horizon as it was overhead, then simply stopped where the march's distance cap
    // fell. A hard edge of full-contrast cloud sitting on a band of haze is most of what read as a
    // rim there. Veiled properly the deck loses contrast into exactly the haze the sky behind it is
    // already made of, so it recedes instead of ending, and the horizon needs no separate fade.
    float cloudDist;
    float4 cloud = averCloudLayer(gCamPos.xyz, ray, L, sunC, cloudDist);
    if (averAtmoOn() && cloud.a < 0.999) {
        float3 aerialT;
        float3 aerialIn = averAtmoAerial(gCamPos.xyz + ray * cloudDist, aerialT);
        cloud.rgb = cloud.rgb * aerialT + aerialIn * (1.0 - cloud.a);
    }
    sky = sky * cloud.a + cloud.rgb;

    return float4(sky, 1.0);
}

// Line vertex as authored: position and display colour.
struct LVSIn  { float3 pos : POSITION; float3 col : COLOR; };
// Line vertex after transform.
struct LVSOut { float4 pos : SV_POSITION; float3 col : COLOR; };
// Transforms a line vertex into clip space.
LVSOut VSLine(LVSIn i) {
    LVSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.pos = mul(wp, gViewProj);
    o.col = i.col;
    return o;
}
// Writes a display-authored line colour as the scene radiance that tonemaps back to it.
float4 PSLine(LVSOut i) : SV_TARGET { return float4(averInverseTonemap(srgbToLin(i.col)), 1.0); }
)";

// The shared prelude and this backend's shaders joined into one translation unit, built once.
const std::string& sceneShaderSource() {
    static const std::string src = std::string(sharedShaderPrelude()) + kShaderHLSL;
    return src;
}

// Per-frame constants. Mirrors `cbuffer PerFrame` in the shared shader prelude, field for field.
struct PerFrameCB {
    f32 viewProj[16];
    f32 invViewProj[16];
    f32 camPos[4];
    f32 lightDir[4];
    f32 lightColor[4];
    f32 ambient[4];
    f32 skyZenith[4];
    f32 skyHorizon[4];
    f32 fogColor[4];    // a = density at fogHeight
    f32 skyParams[4];    // x atmosphere height, y sky-light intensity, z sun intensity, w cos(sun angular radius)
    f32 groundColor[4];  // rgb ground albedo below the horizon
    f32 fogParams[4];    // x height falloff, y fog height, z start distance, w max opacity
    f32 cloudParams[4];  // x coverage, y density, z layer bottom, w layer top
    f32 cloudMotion[4];  // xy wind offset in world units, z 1/feature size, w enabled
    f32 atmoRayleigh[4]; // rgb scattering per km, w scale height km
    f32 atmoMie[4];      // x scatter, y extinction, z scale height km, w phase g
    f32 atmoOzone[4];    // rgb absorption per km, w tent half-width km
    f32 atmoPlanet[4];   // x planet radius km, y atmosphere top radius km, z world->km, w on/off
    f32 atmoTune[4];     // x ozone centre km, y multi-scatter gain, z view steps, w aerial steps
    f32 atmoSunE0[4];    // rgb sun irradiance above the air, w ground albedo
    f32 fogInscatterRef[4]; // rgb averFogInscatterRef's answer, baked once per frame on the CPU
    f32 furnace[4];      // x on, y radiance -- the white-furnace energy oracle
    f32 skySh[9][4];     // nine L2 SH coefficients of the sky, rgb; w unused
};

// Constants for every post pass. Mirrors `cbuffer AverPost : register(b0)` in
// rhi::postShaderSource(), field for field.
struct PostCB {
    f32 tone[4];    // exposure, bloom intensity, bloom threshold, bloom knee
    f32 dst[4];     // destination width, height, 1/width, 1/height
    f32 src[4];     // source width, height, 1/width, 1/height
    f32 adapt[4];   // min log2 luminance, 1/log2 range, adaption alpha, unused
    f32 limit[4];   // exposure min, exposure max, histogram low cut, high cut
    f32 misc[4];    // middle grey, auto-exposure on, bloom filter radius, unused
};
static_assert(sizeof(PostCB) == 96, "the HLSL cbuffer mirrors this byte for byte");

// Per-frame upload ring for the block above.
constexpr u32 kPostConstantRingBytes = 16 * 1024;

// The one description of rhi::MeshVertex to D3D12; every input-assembler pipeline shares it.
constexpr D3D12_INPUT_ELEMENT_DESC kMeshInputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
constexpr UINT kMeshInputLayoutCount = sizeof(kMeshInputLayout) / sizeof(kMeshInputLayout[0]);

// Root parameter indices for the backend's own two signatures.
constexpr UINT kSceneFrameParam  = 0;   // b0, the engine per-frame block
constexpr UINT kSceneObjectParam = 1;   // b1, kObjectConstantDwords root constants
// The mesh-shader signature repeats those two and appends the geometry the IA would have fetched.
constexpr UINT kMeshVertexParam = 2;
constexpr UINT kMeshIndexParam  = 3;
constexpr UINT kMeshCountParam  = 4;    // b5, triangle count
// FROZEN at 3: vertices at t(base), indices at t(base+1), and the prelude is told through -D.
constexpr UINT kSceneMeshSrvBase = 3;

// A mesh uploaded to the GPU, with the views the input assembler binds.
struct GpuMesh {
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    u32 indexCount = 0;
    // Non-zero when this mesh's vertices are an RHI buffer a compute pass writes rather than an
    // upload this device performed -- see IDevice::createSkinTargetMesh. `vb` still points at the
    // same resource, so every draw path reads it identically; this only records WHO writes it.
    BufferHandle vbBuffer = 0;
    // The index buffer as an RHI buffer, and the vertex count. Both exist so a SHADER can read this
    // mesh's geometry: a ray that hits a triangle has only an index into it, and reconstructing the
    // triangle needs descriptors over both streams. createMesh used to allocate raw committed
    // resources with no RhiBuffer entry at all, so no descriptor could ever name them.
    BufferHandle ibBuffer = 0;
    u32 vertexCount = 0;
    // Whether a COMPUTE PASS writes these vertices, as opposed to them merely living in an RHI
    // buffer. The distinction stopped being free the moment createMesh started routing through the
    // factory: every mesh then had a vbBuffer, so meshVertexBuffer -- whose contract is "zero for
    // every ordinary mesh" -- began answering non-zero for all of them, and the renderer rebuilt
    // every static mesh's acceleration structure every frame. Set only by createSkinTargetMesh.
    bool computeWritten = false;
    // Local-space bounding sphere -- AABB midpoint and the distance to a corner, computed once in
    // createMesh. See IDevice::meshBounds for why a corner rather than the farthest actual vertex.
    f32 boundsCentre[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsRadius = 0.0f;

    // ---- index-buffer sharing, which exists ONLY because of createSkinTargetMesh ----
    //
    // A skin target owns its vertices and SHARES its source's indices (`m.ib = src.ib` there --
    // skinning moves vertices and never renumbers triangles). Freeing an index buffer twice, or
    // freeing one out from under a mesh still drawing with it, is silent corruption: the resource
    // goes back to the heap and the next allocation writes over the triangles.
    //
    // So ownership is recorded rather than inferred. `ibOwned` is false on a skin target, which
    // therefore never frees the buffer; `ibShares` counts live sharers on the SOURCE, which
    // therefore refuses to be destroyed while any remain; `ibSource` is how a skin target finds
    // the source to decrement on its way out.
    bool ibOwned = true;
    u32  ibShares = 0;
    MeshHandle ibSource = 0;

    // False once destroyMesh has released this slot. The slot itself is KEPT -- see
    // IDevice::destroyMesh for why a stale handle must address a dead mesh rather than a live one.
    bool alive = true;
};

// A line list uploaded to the GPU.
struct GpuLineMesh {
    ComPtr<ID3D12Resource> vb;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    u32 count = 0;
};

// ---------------------------------------------------------------- generic RHI mapping

// Maps an RHI format to DXGI's.
DXGI_FORMAT toDxgiFormat(Format f) {
    switch (f) {
        case Format::RGBA8Unorm:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8UnormSrgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::RG8Unorm:       return DXGI_FORMAT_R8G8_UNORM;
        case Format::R8Unorm:        return DXGI_FORMAT_R8_UNORM;
        case Format::RGBA16F:        return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32Float:       return DXGI_FORMAT_R32_FLOAT;
        case Format::RG32Float:      return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32Uint:        return DXGI_FORMAT_R32_UINT;
        case Format::D32Float:       return DXGI_FORMAT_D32_FLOAT;
        case Format::R32Typeless:    return DXGI_FORMAT_R32_TYPELESS;
        case Format::BC1Unorm:       return DXGI_FORMAT_BC1_UNORM;
        case Format::BC1UnormSrgb:   return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::BC3Unorm:       return DXGI_FORMAT_BC3_UNORM;
        case Format::BC3UnormSrgb:   return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::BC5Unorm:       return DXGI_FORMAT_BC5_UNORM;
        case Format::BC7Unorm:       return DXGI_FORMAT_BC7_UNORM;
        case Format::BC7UnormSrgb:   return DXGI_FORMAT_BC7_UNORM_SRGB;
        case Format::Unknown:        break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// The format a typeless or depth resource is SAMPLED through.
DXGI_FORMAT toDxgiSrvFormat(Format f) {
    return (f == Format::R32Typeless || f == Format::D32Float) ? DXGI_FORMAT_R32_FLOAT : toDxgiFormat(f);
}
// The format a typeless or depth resource is bound as a DEPTH target through.
DXGI_FORMAT toDxgiDsvFormat(Format f) {
    return (f == Format::R32Typeless || f == Format::D32Float) ? DXGI_FORMAT_D32_FLOAT : toDxgiFormat(f);
}

// The semantic string for a vertex semantic. A literal, so it outlives the desc that borrows it.
const char* semanticName(VertexSemantic s) {
    switch (s) {
        case VertexSemantic::Position: return "POSITION";
        case VertexSemantic::Normal:   return "NORMAL";
        case VertexSemantic::TexCoord: return "TEXCOORD";
        case VertexSemantic::Color:    return "COLOR";
    }
    return "POSITION";
}

// Fills `out` with the input elements a caller-declared vertex layout names. Returns the count.
UINT buildInputLayout(const VertexLayout& l, D3D12_INPUT_ELEMENT_DESC (&out)[kMaxVertexAttribs]) {
    UINT n = 0;
    for (u32 i = 0; i < l.attribCount && i < kMaxVertexAttribs; ++i) {
        const VertexAttrib& a = l.attribs[i];
        const DXGI_FORMAT f = toDxgiFormat(a.format);
        if (f == DXGI_FORMAT_UNKNOWN) {
            AVER_ERROR("[RHI.D3D12] vertex attribute {} has no usable format", i);
            continue;
        }
        out[n].SemanticName = semanticName(a.semantic);
        out[n].SemanticIndex = a.semanticIndex;
        out[n].Format = f;
        out[n].InputSlot = 0;
        out[n].AlignedByteOffset = a.offset;
        out[n].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        out[n].InstanceDataStepRate = 0;
        ++n;
    }
    return n;
}

// Maps a DXGI format back to the RHI's own enum.
Format fromDxgiFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:      return Format::RGBA8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return Format::RGBA8UnormSrgb;
        case DXGI_FORMAT_R8G8_UNORM:          return Format::RG8Unorm;
        case DXGI_FORMAT_R8_UNORM:            return Format::R8Unorm;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return Format::RGBA16F;
        case DXGI_FORMAT_R32_FLOAT:           return Format::R32Float;
        case DXGI_FORMAT_R32G32_FLOAT:        return Format::RG32Float;
        case DXGI_FORMAT_R32_UINT:            return Format::R32Uint;
        case DXGI_FORMAT_D32_FLOAT:           return Format::D32Float;
        case DXGI_FORMAT_R32_TYPELESS:        return Format::R32Typeless;
        case DXGI_FORMAT_BC1_UNORM:           return Format::BC1Unorm;
        case DXGI_FORMAT_BC1_UNORM_SRGB:      return Format::BC1UnormSrgb;
        case DXGI_FORMAT_BC3_UNORM:           return Format::BC3Unorm;
        case DXGI_FORMAT_BC3_UNORM_SRGB:      return Format::BC3UnormSrgb;
        case DXGI_FORMAT_BC5_UNORM:           return Format::BC5Unorm;
        case DXGI_FORMAT_BC7_UNORM:           return Format::BC7Unorm;
        case DXGI_FORMAT_BC7_UNORM_SRGB:      return Format::BC7UnormSrgb;
        default:                              return Format::Unknown;
    }
}

// True for the formats a depth target uses.
bool isDepthFormat(Format f) { return f == Format::D32Float || f == Format::R32Typeless; }

// Bytes per texel, for interpreting the caller's rows only. Zero for block formats.
u32 texelBytes(Format f) {
    switch (f) {
        case Format::RGBA16F:        return 8;
        case Format::RGBA8Unorm:
        case Format::RGBA8UnormSrgb:
        case Format::R32Float:
        case Format::R32Uint:
        case Format::D32Float:
        case Format::R32Typeless:    return 4;
        case Format::RG8Unorm:       return 2;
        case Format::R8Unorm:        return 1;
        default:                     break;
    }
    return 0;
}

// Bytes per 4x4 block. Zero for anything that is not block-compressed.
u32 blockBytes(Format f) {
    switch (f) {
        case Format::BC1Unorm:
        case Format::BC1UnormSrgb: return 8;
        case Format::BC3Unorm:
        case Format::BC3UnormSrgb:
        case Format::BC5Unorm:
        case Format::BC7Unorm:
        case Format::BC7UnormSrgb: return 16;
        default:                   break;
    }
    return 0;
}

// Tightly packed bytes in one source row of a surface this wide â€” one row of BLOCKS for a block
// format, which is the unit the upload loop counts rows in.
u64 packedRowPitch(Format f, u32 widthTexels) {
    if (const u32 bb = blockBytes(f)) return u64((widthTexels + 3) / 4) * bb;
    return u64(widthTexels) * texelBytes(f);
}

// Maps an RHI resource state to D3D12's.
D3D12_RESOURCE_STATES toResourceStates(ResourceState s) {
    switch (s) {
        case ResourceState::ShaderResource:         return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case ResourceState::NonPixelShaderResource: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ResourceState::UnorderedAccess:        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::RenderTarget:           return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::DepthWrite:             return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::CopySource:             return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::CopyDest:               return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::VertexBuffer:           return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        // Every geometry reader at once. NON_PIXEL_SHADER_RESOURCE is the bit a BLAS build demands
        // of its vertex data, and it is not implied by VERTEX_AND_CONSTANT_BUFFER.
        case ResourceState::GeometryRead:           return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ResourceState::AccelerationStructure:  return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        case ResourceState::Common:                 break;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

// Maps an RHI comparison to D3D12's.
D3D12_COMPARISON_FUNC toComparison(CompareOp op) {
    switch (op) {
        case CompareOp::Less:      return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Always:    return D3D12_COMPARISON_FUNC_ALWAYS;
        case CompareOp::Never:     break;
    }
    return D3D12_COMPARISON_FUNC_NEVER;
}

// Maps an RHI filter to D3D12's.
D3D12_FILTER toFilter(Filter f) {
    switch (f) {
        case Filter::Point:  return D3D12_FILTER_MIN_MAG_MIP_POINT;
        case Filter::Linear: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        case Filter::ComparisonLinear: return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        case Filter::Anisotropic: return D3D12_FILTER_ANISOTROPIC;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D12_TEXTURE_ADDRESS_MODE toAddress(AddressMode a) {
    return a == AddressMode::Wrap ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

// Per-subresource state tracking, debug builds only.
#if defined(NDEBUG)
#define AVER_RHI_TRACK_STATE 0
#else
#define AVER_RHI_TRACK_STATE 1
#endif

#if AVER_RHI_TRACK_STATE
const char* stateName(ResourceState s) {
    switch (s) {
        case ResourceState::Common:                 return "Common";
        case ResourceState::ShaderResource:         return "ShaderResource";
        case ResourceState::NonPixelShaderResource: return "NonPixelShaderResource";
        case ResourceState::UnorderedAccess:        return "UnorderedAccess";
        case ResourceState::RenderTarget:           return "RenderTarget";
        case ResourceState::DepthWrite:             return "DepthWrite";
        case ResourceState::CopySource:             return "CopySource";
        case ResourceState::CopyDest:               return "CopyDest";
        case ResourceState::AccelerationStructure:  return "AccelerationStructure";
    }
    return "<unknown>";
}
#endif

void setDebugName(ID3D12Object* obj, const char* name) {
    if (!obj || !name) return;
    const std::wstring w(name, name + std::strlen(name));
    obj->SetName(w.c_str());
}

class D3D12Device;
class D3D12ResourceFactory;
class D3D12RenderContext;
struct RhiBindingSet;

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
    ~D3D12Device() override;

    bool uiInit(void* hwnd) override;
    void uiNewFrame() override;
    void uiShutdown() override;
    bool uiActive() const override { return uiActive_; }
    bool uiWantsMouse() const override;
    bool uiWantsKeyboard() const override;
    u64 uiTextureId(TextureHandle t) override;

    // Plugs a UI toolkit's D3D12 backend into this device -- see aver::rhi::d3d12::installUiBackend's
    // own comment (UiBackend.hpp) for why this exists and who calls it. NOT part of IDevice: only a
    // caller that already knows this is a D3D12Device (through that free function's own backend()
    // check) can reach it. Non-owning, same as setUpscaler.
    void setUiBackend(d3d12::IUiBackend* backend) { uiBackend_ = backend; }

    Backend backend() const override { return Backend::D3D12; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }

    // ----- generic RHI surface (render-feature modules) -----
    // The SCENE colour format, which is what a feature builds its scene pipelines against.
    Format backbufferFormat() const override { return fromDxgiFormat(kSceneColorFormat); }
    Format depthFormat() const override { return fromDxgiFormat(kDepthFormat); }
    // See RHI.hpp's own comment on this method and kDepthResourceFormat's above. OUT-OF-LINE, same
    // reason as renderContext()/resources() just below: it calls into D3D12ResourceFactory, whose
    // complete type is not visible yet at this point in the file.
    TextureHandle sceneDepthTexture() override;
    IResourceFactory* resources() override;
    // See IDevice::renderContext()'s own comment: the SAME context object the overridesScenePipeline
    // branch of drawMesh() below uses internally, exposed so a caller can interleave its own
    // setPipeline/dispatchMeshClusters-shaped calls for a SUBSET of instances in the same frame.
    // OUT-OF-LINE (not defined here): D3D12RenderContext is only forward-declared this early (line
    // 843), so returning `rhiContext_` as an IRenderContext* needs its complete type, which is not
    // visible again until its own definition below -- the same reason resources() just above is
    // declared here and defined out-of-line rather than inline.
    IRenderContext* renderContext() override;
    // NON-owning. Registering the same feature twice would double every hook, so it is ignored.
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    // NON-owning, same contract as the features above. Null (the default) keeps the untouched
    // single-pass composite path, which is what makes AverSR Off bit-identical to a build without
    // the module -- docs/AVERSR.md's own invariant.
    void setUpscaler(IUpscaler* u) override { upscaler_ = u; }
    IUpscaler* upscaler() const override { return upscaler_; }
    void notifyRenderTargetsChanged();
    // Creates or resizes the factory texture the scene renders into for the viewport.
    bool ensureViewportTexture();
    bool           viewportToTex_ = false;
    TextureHandle  viewportTex_ = 0;
    u32            viewportTexW_ = 0, viewportTexH_ = 0;
    // What the features were last told; a sample count of 0 forces the first notification.
    u32    notifiedSamples_ = 0;
    Format notifiedColor_   = Format::Unknown;
    Format notifiedDepth_   = Format::Unknown;
    u32    notifiedWidth_ = 0, notifiedHeight_ = 0;
    u32 sampleCount() const override { return sampleCount_; }
    bool setSampleCount(u32 samples) override;

    ISwapchain* createSwapchain(const SwapchainDesc& d) override {
        if (!createSwapchainResources(d)) return nullptr;
        return new D3D12Swapchain(this);
    }

    void setClearColor(f32 r, f32 g, f32 b, f32 a) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; clear_[3] = a; }
    void setVSync(bool on) override { vsync_ = on; }
    bool vsync() const override { return vsync_; }
    bool vsyncCanDisable() const override { return tearingSupported_; }

    void setViewportToTexture(bool on) override { viewportToTex_ = on; }
    bool viewportToTexture() const override { return viewportToTex_; }
    u64  viewportTextureId() override;

    void setViewportRect(u32 x, u32 y, u32 w, u32 h) override {
        if (w == 0 || h == 0 || x >= width_ || y >= height_) { vpX_ = vpY_ = vpW_ = vpH_ = 0; return; }
        const u32 cw = (x + w > width_) ? width_ - x : w;
        const u32 ch = (y + h > height_) ? height_ - y : h;
        // The caller thinks in present-space pixels (the editor confines the scene to a dockspace
        // sub-rect of the actual window), but the render target these coordinates end up addressing
        // is the SCENE one, which is smaller than the backbuffer whenever renderScale_ < 1 -- so the
        // rect is scaled into scene-space here, once, rather than at every one of its several
        // readers. Identity at renderScale_ == 1.0 (scaleToSceneW/H(v) == v exactly).
        vpX_ = scaleToSceneW(x);  vpY_ = scaleToSceneH(y);
        vpW_ = scaleToSceneW(cw); vpH_ = scaleToSceneH(ch);
        if (vpW_ == 0) vpW_ = 1;
        if (vpH_ == 0) vpH_ = 1;
    }

    void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 camPos[3]) override {
        std::memcpy(frameCB_.viewProj, viewProj, sizeof(frameCB_.viewProj));
        std::memcpy(frameCB_.invViewProj, invViewProj, sizeof(frameCB_.invViewProj));
        frameCB_.camPos[0] = camPos[0]; frameCB_.camPos[1] = camPos[1]; frameCB_.camPos[2] = camPos[2]; frameCB_.camPos[3] = 1;
    }
    bool camera(f32 viewProj[16], f32 invViewProj[16], f32 cameraPos[3]) const override {
        if (viewProj)    std::memcpy(viewProj, frameCB_.viewProj, sizeof(frameCB_.viewProj));
        if (invViewProj) std::memcpy(invViewProj, frameCB_.invViewProj, sizeof(frameCB_.invViewProj));
        if (cameraPos)   std::memcpy(cameraPos, frameCB_.camPos, 3 * sizeof(f32));
        return true;
    }
    // Same rect beginFrame() sets as the scene's actual D3D12 viewport (RSSetViewports below) --
    // vpW_ == 0 means no sub-rect was set, i.e. the whole scene target. SCENE-space (see
    // setViewportRect): what a reprojecting feature needs, since its own history textures are sized
    // off sceneWidth_/sceneHeight_ via onRenderTargetsChanged, not width_/height_.
    bool sceneViewport(f32 rect[4]) const override {
        if (!rect) return true;
        rect[0] = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
        rect[1] = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
        rect[2] = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
        rect[3] = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
        return true;
    }
    void setLight(const f32 dir[3], const f32 color[3], f32 ambient) override {
        frameCB_.lightDir[0] = dir[0]; frameCB_.lightDir[1] = dir[1]; frameCB_.lightDir[2] = dir[2]; frameCB_.lightDir[3] = 0;
        frameCB_.lightColor[0] = color[0]; frameCB_.lightColor[1] = color[1]; frameCB_.lightColor[2] = color[2]; frameCB_.lightColor[3] = 0;
        frameCB_.ambient[0] = frameCB_.ambient[1] = frameCB_.ambient[2] = ambient; frameCB_.ambient[3] = 0;
    }
    void setSkyAtmosphere(const SkyAtmosphere& s) override;
    SkyAtmosphere skyAtmosphere() const override { return sky_; }
    // Packs the physical atmosphere fields, and the four it derives, into the per-frame block.
    void packAtmosphere(const SkyAtmosphere& s);
    void setPostProcess(const PostSettings& p) override { post_ = p; }
    PostSettings postProcess() const override { return post_; }

    MeshHandle createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) override;
    MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) override;
    bool destroyMesh(MeshHandle mesh) override;
    bool destroyLineMesh(LineHandle mesh) override;
    BufferHandle meshVertexBuffer(MeshHandle mesh) const override {
        if (!mesh || mesh > meshes_.size()) return 0;
        const GpuMesh& m = meshes_[mesh - 1];
        // Gated on computeWritten, NOT on vbBuffer being present. Every mesh has an RHI vertex
        // buffer now; only a skin target has one somebody DISPATCHES into, and that is the question
        // this answers.
        return m.computeWritten ? m.vbBuffer : 0;
    }
    bool meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib,
                      u32* vertexCount, u32* indexCount) const override {
        if (!mesh || mesh > meshes_.size()) return false;
        const GpuMesh& m = meshes_[mesh - 1];
        if (!m.vbBuffer || !m.ibBuffer) return false;
        if (vb) *vb = m.vbBuffer;
        if (ib) *ib = m.ibBuffer;
        if (vertexCount) *vertexCount = m.vertexCount;
        if (indexCount) *indexCount = m.indexCount;
        return true;
    }
    bool meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const override {
        if (!mesh || mesh > meshes_.size()) return false;
        const GpuMesh& m = meshes_[mesh - 1];
        if (outCentre) { outCentre[0] = m.boundsCentre[0]; outCentre[1] = m.boundsCentre[1]; outCentre[2] = m.boundsCentre[2]; }
        if (outRadius) *outRadius = m.boundsRadius;
        return true;
    }
    void drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) override;
    LineHandle createLineMesh(const LineVertex* verts, u32 count) override;
    void drawLines(LineHandle mesh, const f32 world[16]) override;
    void setWireframe(bool on) override { wireframe_ = on; }
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(drawBinding_, set, constants, bytes);
    }
    void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(defaultDrawBinding_, set, constants, bytes);
    }

    // ---- same-frame depth prepass -- see IDevice's own comment for the contract ----
    void setDepthPrepassEnabled(bool on) override { depthPrepassEnabled_ = on; }
    bool depthPrepassEnabled() const override { return depthPrepassEnabled_; }
    void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16]) override;
    // AUTO-CONSUMED by the very next drawMesh() call, not stored past it -- see the interface
    // comment. Plain assignment: nothing here decides whether the upcoming draw is ELIGIBLE (a
    // skinned mesh, say), only what the CALLER already believes about it; drawMesh() itself still
    // re-checks meshVertexBuffer(mesh) before trusting this, see its own comment.
    void setNextDrawPrepassed(bool prepassed) override { nextDrawPrepassed_ = prepassed; }
    void setLineDepth(bool testDepth) override { lineDepth_ = testDepth; }
    void setMeshShaders(bool enabled) override {
        const bool want = enabled && msSupported_;
        if (want != msActive_) AVER_INFO("[RHI.D3D12] geometry path: {}", want ? "mesh shaders" : "input assembler");
        if (enabled && !msSupported_ && !msRefusalLogged_) {
            msRefusalLogged_ = true;
            const char* why = caps_.meshShaderTier == 0 ? "mesh-shader tier 0"
                            : caps_.shaderModel < 65    ? "shader model below 6.5"
                            : !caps_.dxcAvailable       ? "no DXC (DXIL) compiler"
                                                        : "the mesh-shader path failed to initialise";
            AVER_INFO("[RHI.D3D12] mesh-shader geometry path requested but unavailable ({}); "
                      "staying on the input assembler", why);
        }
        msActive_ = want;
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
    void initGpuTiming();
    u32  gpuStamp();
    void collectGpuTiming();
    // The same span bookkeeping pushMarker/popMarker do, for the phases that are NOT inside any
    // render feature's markers -- the opaque scene draw the caller issues between beginFrame and
    // endFrame, and the post/composite/UI chain endFrame runs after it. Without these the two
    // largest items in the frame both land in "unmarked" and the report cannot tell them apart.
    //
    // NOT EXPRESSED AS A ScopedGpuStat (RHIResources.hpp), even though these are exactly the
    // begin/end pairs that class exists to replace, because the two calls here do not share a C++
    // scope: beginGpuSpan("scene draw") runs at the tail of beginFrame and endGpuSpan() runs at the
    // top of endFrame, two different functions the caller invokes across an entire frame's worth of
    // drawMesh calls in between. RAII can only close what a destructor can see go out of scope, and
    // nothing here goes out of scope between those two calls -- so this pair, and the identical one
    // bracketing "sky+post+ui" below, stay manual. Every OTHER begin/end and push/pop pair in this
    // codebase that a reviewer might expect this class to have replaced fits inside one function and
    // has been converted (VoxiRenderer.cpp's six pass-level markers, one wrapping VoxiRenderer's own
    // prePass); these two are the genuine exception, not an oversight.
    void beginGpuSpan(const char* label) {
        if (!tsEnabled_) return;
        // Parent is whatever is already open -- kNoParent if nothing is, i.e. this becomes a
        // top-level span. Pushed BEFORE tsOpen_ gains this span's own slot, so a span is never its
        // own parent.
        // AND THE CAP IS ENFORCED HERE, where it was previously only assumed. Dropping the span is
        // the right failure: its children reparent to whatever is still open, which is honest
        // nesting, and one missing row in a profile is far better than a silently wrong tree.
        if (tsSlice_[frameIndex_].size() >= kMaxGpuSpans) { ++tsDropped_; return; }
        const u32 parent = tsOpen_.empty() ? kNoParent : tsOpen_.back();
        tsSlice_[frameIndex_].push_back({label, gpuStamp(), kMaxGpuStamps, parent});
        tsOpen_.push_back(static_cast<u32>(tsSlice_[frameIndex_].size() - 1));
    }
    void endGpuSpan() {
        if (!tsEnabled_) return;
        if (tsDropped_) { --tsDropped_; return; }   // pairs with a refused open; see tsDropped_
        if (tsOpen_.empty()) return;
        const u32 i = tsOpen_.back();
        tsOpen_.pop_back();
        tsSlice_[frameIndex_][i].end = gpuStamp();
    }
    void resize(u32 w, u32 h);
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    bool selfTest(const f32 in[4], f32 out[4]) override;

private:
    void queryCaps();
    bool initAccelerationStructures();
    bool initMeshShaders();
    void dispatchMesh(const GpuMesh& m);
    void bindGraphicsRoot(ID3D12RootSignature* rs); // switch root signature + rebind shared params
    bool createPipeline();
    bool createSwapchainResources(const SwapchainDesc& d);
    void createRenderTargetViews();
    bool createDepthBuffer();
    bool createMsaaColor();
    void reconcileClearValue();
    void waitForGpu();
    // Blocks until the fence reaches `value`; false only when the device has been removed.
    bool waitFence(u64 value);

    // ---- the camera post chain (rhi::PostSettings) ----
    bool createPostPipelines();          // root signature, PSOs and the constant ring: once, at init
    bool createPostTargets();            // resolve target, bloom pyramid and descriptors: per resize
    void releasePostTargets();
    void runPostChain(ID3D12Resource* backbuffer);
    // Suballocate one pass's constants from this frame's post ring.
    D3D12_GPU_VIRTUAL_ADDRESS postConstants(const void* data, u32 bytes);
    D3D12_GPU_DESCRIPTOR_HANDLE postTriple(u32 triple) const;
    D3D12_CPU_DESCRIPTOR_HANDLE postTripleCpu(u32 triple) const;
    // Converts a display-authored colour to the scene radiance that tonemaps back to it.
    // CPU twin of the shared prelude's averInverseTonemap(srgbToLin(c)).
    static void toSceneReferred(const f32 display[4], f32 out[4]);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12DescriptorHeap> msaaRtvHeap_;
    ComPtr<ID3D12Resource> renderTargets_[kFrameCount];
    ComPtr<ID3D12Resource> msaaColor_;
    ComPtr<ID3D12Resource> depthBuffer_;
    // The generic-RHI wrapper around depthBuffer_ -- see sceneDepthTexture()'s own comment. STABLE
    // across a resize: adoptExternalDepthTexture re-fills this SAME slot rather than pushing a new
    // one, so a caller that cached the handle across frames (modules/occlusion does) never has to
    // notice depthBuffer_ was reallocated underneath it.
    TextureHandle depthTexHandle_ = 0;
    // True whenever depthBuffer_ has (re)allocated since depthTexHandle_ was last refreshed --
    // createDepthBuffer() sets this every time it runs; sceneDepthTexture() clears it once it has
    // re-adopted the current depthBuffer_.
    bool depthTexDirty_ = true;
    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    // Wait-before-reuse frame sync: the fence value that retires each backbuffer's last frame,
    // and the monotonic counter it is drawn from.
    u64 fenceValues_[kFrameCount] = {0, 0};
    u64 nextFence_ = 0;
    u32 frameIndex_ = 0;
    u32 rtvSize_ = 0;

    // ---- per-pass GPU timing ----
    //
    // WHY THIS EXISTS. Five separate theories about where this engine's frame time goes were argued
    // from indirect evidence and every one of them was wrong: the shadow cascades, the scene walk,
    // chunk streaming, volumetric clouds, and the build configuration. The reason they could all
    // survive so long is that the only number available was a whole-frame CPU delta, and a CPU
    // number that includes waiting for the GPU is indistinguishable from CPU work. There was no
    // way to ask which PASS was expensive, so everyone guessed. This is that missing question.
    //
    // Rides on the markers that already exist. pushMarker/popMarker bracket every pass, correctly
    // nested, so a timestamp on each side of them costs one EndQuery per marker -- about a dozen a
    // frame -- and needs no new call sites in any renderer.
    //
    // THE MARKERS WERE ALWAYS CORRECTLY NESTED; THE ACCOUNTING WAS NOT. tsOpen_ is a LIFO stack and
    // has been since this existed, so a marker opened while another was already open was always
    // recorded as well-formed push/pop pairs -- nothing about the recording was ever wrong. What was
    // wrong is what collectGpuTiming did with the result: every span, nested or not, was folded into
    // one flat list keyed only by label, and "unmarked" was computed as the whole frame minus the sum
    // of ALL of them. That arithmetic is only correct when every span is disjoint. The moment a span
    // opens while a parent is already open -- which pushMarker/popMarker have always permitted and
    // which VoxiRenderer::prePass's own outer "Voxi GI update" scope now does on purpose -- the child
    // gets summed once on its own and a second time as part of its parent's timestamp range, so
    // "accounted" overshoots and "unmarked" reads low or goes negative. GpuSpan::parent and the tree
    // GpuAccum now builds (see its own comment) are what let collectGpuTiming tell "nested inside" from
    // "next to" apart, so summing correctly means summing only the TOP-LEVEL spans.
    //
    // READ TWO FRAMES LATE, so nothing ever waits. Results are resolved into a per-frame slice of a
    // readback buffer and read back at the TOP of the next frame that reuses that slice, which
    // beginFrame has already fenced on. Reading this frame's own timings would mean blocking on the
    // GPU to ask how fast the GPU was -- the measurement would create the stall it reports.
    static constexpr u32 kMaxGpuSpans = 64;
    static constexpr u32 kMaxGpuStamps = kMaxGpuSpans * 2;
    // A span index used as "this span has no parent, it is top-level".
    //
    // NOT kMaxGpuSpans, WHICH IS WHAT THIS WAS AND WHY IT WAS WRONG. The original reasoning was that
    // kMaxGpuSpans can never be a real index because the span vector's own size is capped by it --
    // except nothing enforced that cap. Neither beginGpuSpan nor pushMarker checks
    // tsSlice_[frameIndex_].size() before push_back, and the only wrap guard that exists, in
    // gpuStamp(), bounds tsCount_ against kMaxGpuStamps -- the TIMESTAMP budget, a different number
    // for a different array. So a 65th span open at once landed at real index 64, bit-identical to
    // the sentinel, and its children would have been reparented to "top-level" -- which in a tree
    // report means their time is counted once inside their real parent and again as a root, and
    // "unmarked" silently absorbs the difference. A profiler that misreports under load is worse
    // than one that stops.
    //
    // Fixed twice over, because either alone would leave the other reader trusting a false premise:
    // the sentinel is now a value no index can ever take, AND the push is actually bounded below.
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    // WHY A PARENT FIELD AT ALL: begin/end already say how LONG a span took; they say nothing about
    // WHERE it sits relative to the others. That is fine while every span is disjoint -- sum them and
    // you have the frame -- but the moment one nests inside another (a feature's own pushMarker calls
    // inside the pass that wraps them, see VoxiRenderer::prePass's new outer scope), summing every
    // span double-counts the nested one: once for itself, once again inside its parent. This field is
    // what lets collectGpuTiming tell "child of X" from "sibling of X" apart, so it can build the
    // actual tree Unreal's stat GPU reports instead of a flat list that quietly assumes disjointness.
    // Set from tsOpen_.back() at the moment a span OPENS (beginGpuSpan/pushMarker) -- tsOpen_ is
    // already the innermost-last stack of everything currently open, so the parent is just whatever
    // was on top of it a moment before this span's own slot got pushed.
    struct GpuSpan { const char* label = nullptr; u32 begin = 0; u32 end = 0; u32 parent = kNoParent; };
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource>  tsReadback_;
    u64  tsFrequency_ = 0;              // GPU ticks per second, from the queue
    u32  tsCount_ = 0;                  // stamps issued so far this frame
    bool tsWrapped_ = false;            // ran out of slots; say so once rather than silently truncate
    // ONE SET OF SPANS PER FRAME SLICE, not one shared set. The stamps for a frame are read two
    // frames after they were issued, so the labels that describe them have to survive that long --
    // a single shared vector would have been overwritten by the frame in between.
    std::vector<GpuSpan> tsSlice_[kFrameCount];
    u32 tsSliceBegin_[kFrameCount] = {};
    u32 tsSliceEnd_[kFrameCount] = {};
    std::vector<u32>     tsOpen_;       // slots of markers still open, innermost last
    // How many span OPENS were refused this frame because the per-frame cap was already reached.
    // Every close checks this FIRST and consumes one rather than popping tsOpen_, which is what keeps
    // open/close pairing exact once the cap bites. Without it, a refused open followed by its own
    // close would pop somebody else's still-open span and stamp an `end` into the wrong row -- a
    // profiler failure that only appears under load, i.e. exactly when the profile matters.
    u32                  tsDropped_ = 0;
    // Accumulated across frames so the report is an average rather than one sampled frame -- now a
    // TREE, not a flat list, because "unmarked = frame - every span" stopped being correct the moment
    // spans could nest (see kNoParent's own comment). Each node's `ms` is INCLUSIVE time -- itself
    // plus everything nested inside it -- accumulated across frames exactly as the old flat entry's
    // was; what is new is `parent`, an index into this SAME vector (or kNoAccumParent for a top-level
    // node), which is what makes EXCLUSIVE time and indentation computable at report time instead of
    // needing to be tracked span by span as the frame is recorded.
    //
    // NODES ARE KEYED BY (label, parent), NOT BY LABEL ALONE. Two spans with the same text nested
    // under two different parents are two different things happening at two different points in the
    // frame -- collapsing them into one entry would average together numbers that do not belong
    // together. In this engine that pairing is stable frame to frame (a given pushMarker call site
    // always nests under the same caller), so in practice this keys exactly the way the old by-label
    // list did; the (label, parent) pair is what makes that an observation rather than an assumption.
    static constexpr u32 kNoAccumParent = 0xFFFFFFFFu;
    struct GpuAccum { std::string label; f64 ms = 0; u32 parent = kNoAccumParent; };
    std::vector<GpuAccum> tsAccum_;
    // Scratch, reused every call rather than reallocated: frame-local span index -> its GpuAccum
    // index, so a child processed after its parent (always true -- see the loop in collectGpuTiming)
    // can look up which accumulator node its own parent folded into.
    std::vector<u32> tsSpanToAccum_;
    f64  tsAccumFrameMs_ = 0;
    u32  tsAccumFrames_ = 0;
    u32  tsReports_ = 0;
    bool tsEnabled_ = false;

    // ---- redundant-state elision for the feature-overridden scene draw ----
    //
    // The scene pass issues one drawMesh per entity -- 1,656 of them on Electric Dreams -- and the
    // branch below re-sent the SAME pipeline, the SAME table-0 binding set and the SAME frame
    // constant block on every one of them. The pipeline is the expensive part: setPipeline binds a
    // root signature, sets a PSO and rebinds every declared root CBV, and rebinding a root signature
    // per draw makes a GPU re-fetch all root data. The frame CBV is the wasteful part: it copies the
    // whole block into the upload ring per draw, which is why a 2MB ring exhausts and doubles every
    // frame on this scene.
    //
    // Only ever elided as a GROUP. setPipeline's bindDeclaredRootCbvs resets the FEATURE frame slot
    // to the zero CBV, which is precisely why the frame block had to be re-sent after it -- so
    // skipping the constant buffer while still calling setPipeline would leave the shader reading
    // zeroes. Either all three are skipped or none are.
    PipelineHandle   fovPso_ = 0;
    BindingSetHandle fovSet_ = 0;
    u32              fovCbBytes_ = 0;
    std::vector<u8>  fovCb_;
    // Cleared by anything that could have bound something else since the last draw: setPipeline
    // always, setBindingSet on TABLE 0 only (table 1 is the per-material binding, which legitimately
    // changes every draw and does not disturb what is cached here), and the start of each frame and
    // of endFrame's post chain, which set pipelines on the command list directly.
    bool             fovValid_ = false;

    // (set base, table, pipeline base) triples already reported by setBindingSet's register-mismatch
    // check. ONE BINDING SET LEGITIMATELY SERVES TWO PIPELINES AT TWO BASES: the material system is
    // shared, and stamps every set with the base of whichever layout initialised it (Voxi's t9),
    // while the cluster pipeline's own table 1 sits at t4. That is correct -- a descriptor table is
    // bound by heap index and each pipeline is self-consistent about its own registers -- but the
    // check has no notion of it, so without this the warning fires on EVERY cluster draw of every
    // frame and drowns the log. Kept as a once-per-shape diagnostic rather than deleted, because on
    // a pipeline that only ever has one consumer it still catches a real mistake.
    std::vector<u64> bindingBaseWarned_;

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> skyPso_;
    ComPtr<ID3D12PipelineState> wirePso_;
    ComPtr<ID3D12PipelineState> linePso_;
    ComPtr<ID3D12PipelineState> lineOverlayPso_; // no depth test: editor gizmos on top
    // Per-draw binding table 1 plus its b2 constant block, copied from the caller.
    struct DrawBinding {
        BindingSetHandle set = 0;
        u8  constants[kMaxDrawConstantBytes] = {};
        u32 bytes = 0;
    };
    static void storeDrawBinding(DrawBinding& d, BindingSetHandle set, const void* constants, u32 bytes) {
        if (bytes > kMaxDrawConstantBytes) {
            AVER_ERROR("[RHI.D3D12] setDrawBinding constant block is {} bytes, over the {} limit", bytes, kMaxDrawConstantBytes);
            return;
        }
        d.set = set;
        d.bytes = (constants && bytes) ? bytes : 0;
        if (d.bytes) std::memcpy(d.constants, constants, d.bytes);
    }
    DrawBinding drawBinding_{}, defaultDrawBinding_{};
    bool drawBindingIgnored_ = false;   // the "backend's own pipeline drops it" warning, said once

    // ---- same-frame depth prepass (see IDevice::setDepthPrepassEnabled and drawMesh below) ----
    bool depthPrepassEnabled_ = false;   // --depth-prepass; OFF reproduces pre-existing behaviour
    // AUTO-CONSUMED: read and reset to false by the very next drawMesh() call, whether or not that
    // call actually used it (a mesh that turns out to be compute-written still clears it) -- see
    // setNextDrawPrepassed's own interface comment for why this must not be sticky.
    bool nextDrawPrepassed_ = false;
    // How many drawMeshDepthPrepass() draws this frame actually issued -- logged on change only,
    // same discipline as lastSceneDrawn_ below, so --depth-prepass with nothing eligible on screen is
    // diagnosable from the log rather than looking identical to the flag being ignored.
    u32 depthPrepassDrawsLastFrame_ = 0, depthPrepassDrawsThisFrame_ = 0;

    bool skyEnabled_ = false;
    // Set in beginFrame when a feature suppressed the scene (its own scenePass replaced the whole
    // frame), read in endFrame so the deferred sky draw doesn't run over what that feature drew.
    bool sceneSuppressed_ = false;
    // Set alongside sceneSuppressed_ in beginFrame; read by the deferred sky in endFrame. A
    // feature can suppress the scene GEOMETRY without owning the frame -- see
    // IRenderFeature::suppressesWholeFrame.
    bool frameSuppressed_ = false;
    // The authored atmosphere; the frame block above holds the packed form the shader reads.
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    bool lineDepth_ = true;
    std::vector<GpuLineMesh> lineMeshes_;
    ComPtr<ID3D12Resource> frameCBs_[kFrameCount];
    u8* frameCBPtr_[kFrameCount] = {nullptr, nullptr};

    // ---- camera post chain ------------------------------------------------------------------
    PostSettings post_{};
    // The MSAA resolve destination. Null when sampleCount_ == 1, where msaaColor_ is already it.
    ComPtr<ID3D12Resource> sceneResolved_;
    ComPtr<ID3D12Resource> bloomTex_;            // half-res RGBA16F pyramid
    u32 bloomMips_ = 0, bloomW_ = 0, bloomH_ = 0;
    // Per-mip resource state of the bloom pyramid.
    D3D12_RESOURCE_STATES bloomState_[kMaxBloomMips] = {};
    ComPtr<ID3D12Resource> histBuf_, expBuf_;    // 256-bin histogram, and the one adapted exposure
    bool expSeeded_ = false;
    ComPtr<ID3D12DescriptorHeap> postRtvHeap_;   // one RTV per bloom mip
    ComPtr<ID3D12DescriptorHeap> postSrvHeap_;   // shader-visible: the SRV triples + the UAV pair
    ComPtr<ID3D12RootSignature> postRootSig_;
    ComPtr<ID3D12PipelineState> bloomPrefilterPso_, bloomDownPso_, bloomUpPso_;
    // Composite permutations, indexed [bloom on][auto-exposure on].
    ComPtr<ID3D12PipelineState> compositePso_[2][2];
    ComPtr<ID3D12PipelineState> histogramPso_, exposurePso_;
    ComPtr<ID3D12Resource> postCBs_[kFrameCount];
    u8* postCBPtr_[kFrameCount] = {nullptr, nullptr};
    u32 postCBUsed_ = 0;
    u32 postSrvSize_ = 0, postRtvSize_ = 0;
    bool postReady_ = false;
    // Wall-clock seconds since the previous endFrame, for exposure adaptation.
    i64 lastFrameTick_ = 0;
    f32 frameSeconds_ = 1.0f / 60.0f;

    ComPtr<ID3D12Resource> captureBuf_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFp_{};
    bool captureReq_ = false, captureReady_ = false;
    u32 capX_ = 0, capY_ = 0;
    f32 captured_[4] = {0, 0, 0, 0};
    std::vector<u8> frameImage_;
    u32 frameImageW_ = 0, frameImageH_ = 0;

    // Non-owning; see UiBackend.hpp and setUiBackend above. Null in every game build and in an editor
    // tree configured AVER_ENABLE_UI=OFF -- the only two states this file's own compilation ever needs
    // to know about, since it never names Dear ImGui itself.
    d3d12::IUiBackend* uiBackend_ = nullptr;
    bool uiActive_ = false;

    std::vector<GpuMesh> meshes_;
    // Skin-target meshes created outside a frame and still holding uninitialised memory. Drained at
    // the top of the next frame; see seedSkinTargets.
    struct SkinSeed { MeshHandle dst; MeshHandle src; };
    std::vector<SkinSeed> skinSeeds_;
    void seedSkinTargets();
    PerFrameCB frameCB_{};
    u32 width_ = 0, height_ = 0;
    // The scene's OWN render-target size: width_/height_ scaled by renderScale_, rounded, floored at
    // 1. Equal to width_/height_ whenever renderScale_ == 1.0 (the default) -- computeSceneSize()
    // guarantees that exactly, integer division cancelling the multiply, so every byte on that path
    // stays identical to before renderScale_ existed. Everything present-resolution (the backbuffer,
    // the editor's composited viewport texture, ImGui, capture) is keyed off width_/height_ still;
    // only the scene's depth/MSAA-colour targets, the post chain's resolve+bloom pyramid, and what
    // render features are told via onRenderTargetsChanged key off this pair instead.
    u32 sceneWidth_ = 0, sceneHeight_ = 0;
    f32 renderScale_ = 1.0f;   // [0.25, 1.0]; see IDevice::setRenderScale
    void computeSceneSize() {
        sceneWidth_  = width_  ? static_cast<u32>(std::lround(static_cast<f32>(width_)  * renderScale_)) : 0;
        sceneHeight_ = height_ ? static_cast<u32>(std::lround(static_cast<f32>(height_) * renderScale_)) : 0;
        if (width_  && sceneWidth_  < 1) sceneWidth_  = 1;
        if (height_ && sceneHeight_ < 1) sceneHeight_ = 1;
    }
    // Present-space -> scene-space scaling for the editor's viewport sub-rect (setViewportRect takes
    // physical backbuffer pixels; the render target those pixels address is the scene one, which is
    // smaller than the backbuffer whenever renderScale_ < 1). Exact identity at renderScale_ == 1.0
    // (sceneWidth_ == width_, so v * sceneWidth_ / width_ == v).
    u32 scaleToSceneW(u32 v) const { return width_  ? static_cast<u32>((static_cast<u64>(v) * sceneWidth_)  / width_)  : v; }
    u32 scaleToSceneH(u32 v) const { return height_ ? static_cast<u32>((static_cast<u64>(v) * sceneHeight_) / height_) : v; }
    void setRenderScale(f32 scale) override {
        scale = std::fmax(0.25f, std::fmin(1.0f, scale));
        if (scale == renderScale_) return;
        renderScale_ = scale;
        if (!hasSwapchain_) return;   // applied next createSwapchainResources
        rebuildSceneTargets();
    }
    f32 renderScale() const override { return renderScale_; }
    // Tears down and rebuilds every target sized off sceneWidth_/sceneHeight_ after renderScale_
    // changes with a swapchain already live -- the same set resize() rebuilds, minus the swapchain
    // itself and the present-space viewport texture, neither of which renderScale_ touches.
    void rebuildSceneTargets() {
        waitForGpu();
        depthBuffer_.Reset();
        msaaColor_.Reset();
        computeSceneSize();
        vpX_ = vpY_ = vpW_ = vpH_ = 0;   // stored in scene-space; stale until the next setViewportRect
        createDepthBuffer();
        createMsaaColor();
        releasePostTargets();
        notifyRenderTargetsChanged();
        AVER_TRACE("[RHI.D3D12] render scale {:.2f} -> scene {}x{} (present {}x{})",
                   renderScale_, sceneWidth_, sceneHeight_, width_, height_);
    }
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0; // scene sub-rect; w/h == 0 means full backbuffer
    u32 sampleCount_ = kDefaultSampleCount;     // live MSAA sample count (1 = off)
    DeviceCaps caps_{};

    // ---- Debug layer message drain ----
    ComPtr<ID3D12InfoQueue> infoQueue_;
    std::vector<u32> seenMessageIds_;   // first occurrence only; the totals carry the rest
    u32 dbgCorruption_ = 0, dbgError_ = 0, dbgWarning_ = 0;
    void drainDebugMessages();

    // ---- DXR 1.1 ----
    // Acquired only so the generic factory can build acceleration structures.
    ComPtr<ID3D12Device5> device5_;
    ComPtr<ID3D12GraphicsCommandList4> cmdList4_;

    // ---- Mesh shader geometry path (D3D12 Ultimate) ----
    // Separate root signature: a mesh-shader PSO may not use one declaring an IA layout.
    ComPtr<ID3D12GraphicsCommandList6> cmdList6_;
    ComPtr<ID3D12RootSignature> msRootSig_;
    ComPtr<ID3D12PipelineState> msPso_;
    bool msSupported_ = false, msActive_ = false, msRefusalLogged_ = false;
    ID3D12RootSignature* boundRootSig_ = nullptr;   // raw: cache only, ownership stays in the ComPtrs
    ID3D12PipelineState* boundPso_ = nullptr;       // same cache, for drawMesh's SetPipelineState
    // Same idea, for the LAST descriptor heap array actually pushed onto cmdList_ via
    // SetDescriptorHeaps -- see D3D12RenderContext::setBindingSet for why this is worth caching at
    // all: this backend only ever has ONE generic heap (res_->heap_), so a scene of thousands of
    // draws through a render feature calls SetDescriptorHeaps with the identical single-entry array
    // every single draw. Every direct SetDescriptorHeaps call that bypasses setBindingSet (the post
    // chain's postSrvHeap_, the installed UI backend's own heap) must update this immediately after, the same
    // discipline boundRootSig_/boundPso_ already follow, or a later setBindingSet call would wrongly
    // believe res_->heap_ was still the one visible to the command list.
    ID3D12DescriptorHeap* boundHeap_ = nullptr;

    bool hasSwapchain_ = false;
    bool vsync_ = true;
    // Whether the swapchain was created able to tear. Fixed for its life.
    bool tearingSupported_ = false;
    f32 clear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    // The optimised clear value the current scene colour target was created with.
    f32 msaaClear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    // The same colour as the scene radiance that tonemaps back to it.
    f32 sceneClear_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::string adapterName_ = "D3D12 Device";
    // True when the device is running on the software rasteriser (WARP).
    bool softwareAdapter_ = false;
    bool warpConsRasterLogged_ = false;

    // ---- generic RHI (render-feature modules) ----
    // Raw pointers, deleted in the destructor: both types are incomplete here.
    D3D12ResourceFactory* rhiFactory_ = nullptr;
    D3D12RenderContext* rhiContext_ = nullptr;
    std::vector<IRenderFeature*> features_;   // non-owning

    // ---- AverSR ----
    // Null unless a host set one. EVERY branch below tests this handle, not a quality enum or a
    // build flag, so "no upscaler" and "no AverSR module in the build" are the same code path.
    IUpscaler* upscaler_ = nullptr;
    // A factory-created ALIAS of the scene colour, and the reason it has to exist: `scene`
    // (sceneResolved_/msaaColor_) is a raw ComPtr made by CreateCommittedResource directly, never
    // through the resource factory, so it has no TextureHandle -- and IUpscaler::execute needs one
    // for in.color. A per-frame CopyResource fills this. Scene resolution, RGBA16F, SRV only.
    TextureHandle sceneColorTex_ = 0;
    u32           sceneColorTexW_ = 0, sceneColorTexH_ = 0;
    // AverSR's output: HDR (pre-tonemap) at PRESENT resolution. The upscale runs on radiance and
    // PSComposite then tonemaps an image that is already the right size, so its own resample
    // becomes 1:1 and neither the shader nor its pipeline changes.
    TextureHandle presentHdrTex_ = 0;
    u32           presentHdrTexW_ = 0, presentHdrTexH_ = 0;
    bool          srLogged_ = false;   // the once-only "it really ran" line

    friend class D3D12ResourceFactory;
    friend class D3D12RenderContext;
};

// ---------------------------------------------------------------- generic RHI objects
// Backing records for the handle tables. A handle is index + 1, so 0 is never a live resource.

// A texture, its optional render/depth target views, and its resolved description.
struct RhiTexture {
    ComPtr<ID3D12Resource> res;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    TextureDesc desc{};                     // resolved: `mips` holds the real count, never 0
#if AVER_RHI_TRACK_STATE
    // Owned copy: the desc's debugName is the caller's pointer.
    std::string debugName;
    // One entry per mip; a subresource index is a mip index here.
    std::vector<ResourceState> states;
#endif
    // The descriptor uiTextureId() handed to the UI. Zero until the UI first asks, and forever zero
    // in a build with no UI backend installed -- unconditional now, not gated on AVER_WITH_IMGUI: this
    // file no longer knows that macro exists (see UiBackend.hpp).
    u64 uiSrvCpu = 0, uiSrvGpu = 0;
};

// A buffer, its description, and its persistent mapping.
struct RhiBuffer {
    ComPtr<ID3D12Resource> res;
    BufferDesc desc{};
    u8* mapped = nullptr;                   // upload buffers stay mapped for their whole life
#if AVER_RHI_TRACK_STATE
    std::string debugName;
    ResourceState state = ResourceState::Common;
    // Upload-heap and acceleration-structure buffers reject every transition.
    bool stateFixed = false;
#endif
};

// A compiled shader blob and the stage it was compiled for.
struct RhiShader {
    ComPtr<ID3DBlob> blob;
    ShaderStage stage = ShaderStage::Vertex;
};

static_assert(kMaxConstantSlots == 5, "slotParam's -1 initialisers are written out per slot");
static_assert(kBindingTableCount == 2, "srvParam/uavParam's -1 initialisers are written out per table");

// A pipeline state and where each declared binding landed in its root signature; -1 means undeclared.
struct RhiPipeline {
    ComPtr<ID3D12PipelineState> pso;
    ID3D12RootSignature* rootSig = nullptr;   // owned by the root-signature cache, not by this
    bool compute = false;
    bool mesh = false;
    // True when this mesh pipeline also has an amplification shader (GraphicsPipelineDesc::as != 0),
    // i.e. it is a dispatchMeshClusters() pipeline, not a dispatchMeshFor() one. Always false when
    // `mesh` is false.
    bool amplification = false;
    // One entry per declarable table; -1 where the layout declared nothing for it.
    i32  srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    // The first shader register each table covers.
    u32  srvBaseRegister[kBindingTableCount] = {};
    i32  slotParam[kMaxConstantSlots] = {-1, -1, -1, -1, -1};
    u32  slotDwords[kMaxConstantSlots] = {};  // 0 = the slot is a root CBV rather than root constants
    i32  msVertexParam = -1, msIndexParam = -1, msCountParam = -1;
    // -1 unless GraphicsPipelineDesc::instanced was set; see RHIResources.hpp's comment above
    // GraphicsPipelineDesc::instanced for what this root SRV carries.
    i32  instanceWorldParam = -1;
};

// One suballocated descriptor range plus the kind declared for each of its slots. Slots past the
// end of the kinds arrays are null-filled as a 2D texture.
struct RhiBindingSet {
    u32 srvCount = 0, uavCount = 0;
    // The run of shader registers the set was built for.
    u32 srvBaseRegister = 0, uavBaseRegister = 0;
    u32 heapBase = 0;   // SRVs occupy [heapBase, heapBase+srvCount), the UAVs follow immediately
    SlotKind srvKinds[kMaxBindingSlots] = {};
    SlotKind uavKinds[kMaxBindingSlots] = {};
    bool alive = false;
};

// A descriptor range handed back by destroyBindingSet, reusable once the fence passes.
struct RetiredRange {
    u32 first = 0;
    u32 count = 0;
    u64 fence = 0;
};

// A bottom-level acceleration structure for one mesh, with its build scratch.
// Scratch is released only through the deferred-destroy queue.
struct RhiBlas {
    ComPtr<ID3D12Resource> as, scratch;
    MeshHandle mesh = 0;
    bool built = false;
};

// A top-level acceleration structure, its scratch, and one instance buffer per frame in flight.
struct RhiTlas {
    ComPtr<ID3D12Resource> as, scratch;
    ComPtr<ID3D12Resource> instances[kFrameCount];
    u8* instancePtr[kFrameCount] = {};
    u32 maxInstances = 0;
};

// A root signature plus the parameter indices it was built with; shared by identical layouts.
struct RootSigEntry {
    PipelineLayout layout{};
    bool mesh = false;
    // Part of the cache key alongside layout/mesh: two identical layouts, one instanced and one not,
    // need DIFFERENT root signatures (the instanced one has an extra root SRV param), so they must
    // not collide on the same cache entry.
    bool instanced = false;
    ComPtr<ID3D12RootSignature> sig;
    i32 srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    i32 slotParam[kMaxConstantSlots] = {-1, -1, -1, -1, -1};
    i32 msVertexParam = -1, msIndexParam = -1, msCountParam = -1;
    i32 instanceWorldParam = -1;
};

// A destroyed object the GPU may still be reading. Released once the fence passes, never sooner.
struct RetiredObject {
    ComPtr<IUnknown> obj;
    u64 fence = 0;
};

// True when two sampler descriptions are identical.
bool sameSampler(const SamplerDesc& a, const SamplerDesc& b) {
    return a.filter == b.filter && a.address == b.address && a.compare == b.compare && a.maxLod == b.maxLod;
}
// True when two pipeline layouts are identical. Field by field: PipelineLayout has padding.
bool sameLayout(const PipelineLayout& a, const PipelineLayout& b) {
    if (a.srvCount != b.srvCount || a.uavCount != b.uavCount || a.samplerCount != b.samplerCount) return false;
    if (a.srvCount1 != b.srvCount1 || a.uavCount1 != b.uavCount1) return false;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) if (a.constantDwords[i] != b.constantDwords[i]) return false;
    for (u32 i = 0; i < a.samplerCount && i < 4; ++i) if (!sameSampler(a.samplers[i], b.samplers[i])) return false;
    return true;
}

// Descriptors every binding set suballocates from: one shader-visible heap for the whole device.
constexpr u32 kRhiHeapSize = 65536;
// Transient constant bytes per frame in flight.
// The upload ring's STARTING size per frame in flight. It grows from here on demand (ringAlloc), so
// this is the floor for a quiet scene rather than a budget anything has to fit inside.
//
// 2 MB, RAISED FROM 1 MB, because overflow is not free the way "it just grows" suggests: ringAlloc
// returns 0 on the frame it runs out, and every draw that asked for constants after that point loses
// them for that frame. Growth only takes effect at the NEXT epoch, so the overflowing frame renders
// wrong and the log says so -- every session on any real scene opened with
// "upload ring (1024 KB) exhausted this frame; growing to 2048 KB", which is one visibly wrong frame
// at startup that nobody was reading as a defect.
//
// 2 MB is where this scene actually settled, measured, not guessed. It does not remove the failure
// mode for a heavier scene -- that wants ringAlloc to fall back to a one-off allocation instead of
// returning 0 -- it removes the case that was hitting every single run.
constexpr u64 kRhiRingBytes = 2u << 20;
// The ceiling that growth stops at. 64 MB is about 260,000 per-draw constant slices in one frame --
// far past any draw count this renderer can submit at an interactive rate, so hitting it means
// something is wrong upstream, not that the ring is too small. A ceiling exists at all because the
// buffer is CPU-visible upload memory, one per frame in flight, and unbounded growth driven by a
// runaway draw loop would exhaust address space instead of reporting a problem.
constexpr u64 kRhiRingMaxBytes = 64ull << 20;

// The generic RHI factory: handle tables, the shared descriptor heap, and deferred destruction.
class D3D12ResourceFactory final : public IResourceFactory {
public:
    explicit D3D12ResourceFactory(D3D12Device* dev) : dev_(dev) {}
    ~D3D12ResourceFactory() override;

    bool init();
    void selfTest();

    TextureHandle    createTexture(const TextureDesc& d) override;
    BufferHandle     createBuffer(const BufferDesc& d) override;
    ShaderHandle     createShader(const ShaderDesc& d) override;
    PipelineHandle   createGraphicsPipeline(const GraphicsPipelineDesc& d) override;
    PipelineHandle   createComputePipeline(const ComputePipelineDesc& d) override;
    BindingSetHandle createBindingSet(const BindingSetDesc& d) override;
    BlasHandle       createBlas(MeshHandle mesh) override;
    TlasHandle       createTlas(u32 maxInstances) override;

    void destroyTexture(TextureHandle h) override;
    void destroyBuffer(BufferHandle h) override;
    void destroyBlas(BlasHandle h) override;
    MeshHandle blasMesh(BlasHandle h) const override;
    // Destroys every acceleration structure built from `mesh`. Concrete rather than part of
    // IResourceFactory: it is an implementation detail of D3D12Device::destroyMesh, and no caller
    // outside this file has any business asking for it.
    void destroyBlasForMesh(MeshHandle mesh);
    void destroyShader(ShaderHandle h) override;
    void destroyPipeline(PipelineHandle h) override;
    void destroyBindingSet(BindingSetHandle h) override;

    void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) override;
    void setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;
    // The underlying resource, for the context's copy and barrier paths. Null on a bad handle.
    ID3D12Resource* bufferResource(BufferHandle h) {
        return (h == 0 || h > buffers_.size()) ? nullptr : buffers_[h - 1].res.Get();
    }
    // Wraps a resource this factory did NOT create (D3D12Device::depthBuffer_, made directly via
    // CreateCommittedResource in createDepthBuffer() because it needs a DSV and a MULTISAMPLE-aware
    // one at that -- createTexture() never makes either) into an ordinary TextureHandle, so a caller
    // reaches it through setSrv/textureBarrier exactly like any factory-made texture. Concrete rather
    // than part of IResourceFactory, same reasoning as bufferResource above: this is the mechanics of
    // ONE specific adoption, not a general "wrap anything" entry point every backend would need to
    // grow. `existing`, when non-zero, is REUSED in place rather than allocating a new slot -- see
    // D3D12Device::depthTexHandle_'s own comment for why the handle has to stay stable across a
    // resize. Returns the (possibly reused) handle, or 0 if `res` is null.
    TextureHandle adoptExternalDepthTexture(ID3D12Resource* res, u32 width, u32 height, TextureHandle existing);
    void setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;

    bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) override;
    bool readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset) override;
    bool textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const override;
    bool textureInfo(TextureHandle h, TextureDesc& out) const override;
    void waitIdle() override;

    // Backs IDevice::uiTextureId.
    u64 uiDescriptor(TextureHandle h);

private:
    // The acceleration-structure substitution warning, said once per device.
    bool asSlotLogged_ = false;

    // Fills a freshly created texture from TextureDesc::initialData. The resource must already be
    // in COPY_DEST; on success it has been transitioned to `d.initialState` and the GPU has finished.
    bool uploadInitialData(ID3D12Resource* res, const D3D12_RESOURCE_DESC& td, const TextureDesc& d,
                           u32 mips);

    // Table lookups. Every one returns nullptr for an out-of-range or freed handle; callers log.
    RhiTexture*    texture(TextureHandle h);
    const RhiTexture* texture(TextureHandle h) const;
    RhiBuffer*     buffer(BufferHandle h);
    RhiShader*     shader(ShaderHandle h);
    RhiPipeline*   pipeline(PipelineHandle h);
    RhiBindingSet* bindingSet(BindingSetHandle h);
    RhiBlas*       blas(BlasHandle h);
    RhiTlas*       tlas(TlasHandle h);

    const RootSigEntry* rootSignature(const PipelineLayout& layout, bool mesh, bool instanced = false);
    // Descriptor slot `heapBase + index`, CPU side (for writing) and GPU side (for binding).
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(u32 index) const;
    void nullFill(const RhiBindingSet& s);
    // Reuses a retired range large enough for `count`, or bump-allocates. False when exhausted.
    bool allocRange(u32 count, u32& outFirst);

    // The fence value at which work recorded right now can be considered retired.
    u64  retireFence() const;
    void retire(ComPtr<IUnknown> obj);
    void collect();

    D3D12Device* dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    u32 heapStride_ = 0;
    u32 heapUsed_ = 0;

    std::vector<RhiTexture>    textures_;
    std::vector<RhiBuffer>     buffers_;
    std::vector<RhiShader>     shaders_;
    std::vector<RhiPipeline>   pipelines_;
    std::vector<RhiBindingSet> bindingSets_;
    std::vector<RhiBlas>       blases_;
    std::vector<RhiTlas>       tlases_;
    std::vector<RootSigEntry>  rootSigs_;
    std::vector<RetiredObject> retired_;
    std::vector<RetiredRange>  pendingRanges_;   // returned, still behind the fence
    std::vector<RetiredRange>  freeRanges_;      // reusable now

    friend class D3D12RenderContext;
    friend class D3D12Device;
};

// Records a render feature's generic RHI commands into the device's frame command list.
class D3D12RenderContext final : public IRenderContext {
public:
    D3D12RenderContext(D3D12Device* dev, D3D12ResourceFactory* res) : dev_(dev), res_(res) {}

    void setPipeline(PipelineHandle p) override;
    void setViewport(u32 x, u32 y, u32 w, u32 h) override;
    void setScissor(u32 x, u32 y, u32 w, u32 h) override;
    void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) override;
    void clearDepth(TextureHandle depth, f32 value) override;
    void clearColor(TextureHandle target, const f32 color[4]) override;
    void setBindingSet(BindingSetHandle set, u32 table) override;
    void setConstants(u32 slot, const void* data, u32 dwords) override;
    void setConstantBuffer(u32 slot, const void* data, u32 bytes) override;
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override;
    void drawMesh(MeshHandle mesh) override;
    void drawMeshInstanced(MeshHandle mesh, const f32* worlds, u32 instanceCount) override;
    void dispatchMeshFor(MeshHandle mesh) override;
    void dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) override;
    void dispatch(u32 gx, u32 gy, u32 gz) override;
    void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes,
                    u64 dstOffset, u64 srcOffset) override;
    void copyTexture(TextureHandle dst, TextureHandle src) override;
    void copyTextureToBuffer(BufferHandle dst, u64 dstOffset, TextureHandle src, u32 mip) override;
    void copyBufferToTexture(TextureHandle dst, u32 mip, BufferHandle src, u64 srcOffset) override;
    void drawFullscreen() override;
    void setVertexBuffer(BufferHandle b, u32 stride) override;
    void setIndexBuffer(BufferHandle b, Format indexFormat) override;
    void drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) override;
    void buildBlas(BlasHandle blas) override;
    void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    void textureBarrier(TextureHandle t, ResourceState from, ResourceState to, u32 subresource) override;
    void bufferBarrier(BufferHandle b, ResourceState from, ResourceState to) override;
    void uavBarrierTexture(TextureHandle t) override;
    void uavBarrierBuffer(BufferHandle b) override;
    void pushMarker(const char* label) override;
    void popMarker() override;

private:
    // Suballocate `bytes` of transient upload memory for the frame being recorded.
    D3D12_GPU_VIRTUAL_ADDRESS ringAlloc(const void* data, u32 bytes);
    // Give every root CBV the pipeline declares a valid address, so no draw can read an unset one.
    void bindDeclaredRootCbvs(const RhiPipeline* p);
    // Bind the sticky per-draw state, if the current pipeline declared anywhere to put it.
    void applyDrawBinding();
    D3D12_GPU_VIRTUAL_ADDRESS zeroCbv();

    D3D12Device* dev_;
    D3D12ResourceFactory* res_;
    const RhiPipeline* pipe_ = nullptr;
    ComPtr<ID3D12Resource> zeroCB_;   // shared zero-filled CBV for declared-but-unsupplied slots

    ComPtr<ID3D12Resource> ring_[kFrameCount];
    u8* ringPtr_[kFrameCount] = {};
    u64 ringUsed_[kFrameCount] = {};
    // How big each frame's ring actually IS, which stopped being kRhiRingBytes when the ring learned
    // to grow -- see ringAlloc. Zero means "not created yet".
    u64 ringBytes_[kFrameCount] = {};
    // The size the busiest frame so far asked for. Monotonic: a ring never shrinks back, because the
    // scene that needed it once will almost certainly need it again a frame later.
    u64 ringWanted_ = kRhiRingBytes;
    // The epoch an overflow was last reported in, so the message is once per frame rather than once
    // per failed allocation.
    u64 ringOverflowEpoch_ = ~0ull;
    // The ring resets itself when the device's monotonic fence counter moves on.
    u64 ringEpoch_ = ~0ull;

    // Sticky per-draw state (setDrawBinding), copied from the caller's block.
    BindingSetHandle drawSet_ = 0;
    u8  drawConstants_[kMaxDrawConstantBytes] = {};
    u32 drawConstantBytes_ = 0;
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

    ComPtr<IDXGIFactory6> factory6;
    const bool haveGpuPref = SUCCEEDED(factory_.As(&factory6));
    ComPtr<IDXGIAdapter1> adapter;

    if (desc.useWarp) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
            SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
            DXGI_ADAPTER_DESC1 ad{};
            warp->GetDesc1(&ad);
            char name[128];
            std::wcstombs(name, ad.Description, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            adapterName_ = name;
            softwareAdapter_ = true;
            AVER_WARN("[RHI.D3D12] using the WARP software rasteriser ('{}') - expect single-digit frame rates", adapterName_);
        } else {
            AVER_WARN("[RHI.D3D12] WARP requested but unavailable - falling back to hardware");
        }
    }

    for (UINT i = 0; !device_; ++i) {
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

    if (desc.enableDebug && SUCCEEDED(device_.As(&infoQueue_)))
        AVER_INFO("[RHI.D3D12] debug layer messages will be logged");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!hrOk(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue")) return false;
    initGpuTiming();

    if (!hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence")) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) { AVER_ERROR("[RHI.D3D12] CreateEvent failed"); return false; }

    queryCaps();
    if (!(caps_.msaaMask & sampleCount_)) sampleCount_ = 1;

    const SkyAtmosphere def{};
    setLight(def.sunDirection, def.sunColor, def.skyLightIntensity);

    if (!createPipeline()) return false;
    if (!createPostPipelines()) return false;

    // DXR 1.1 acceleration structures need only caps_ and device_, both already valid here -- same
    // as VulkanDevice::init(), which calls its own initAccelerationStructures() at this exact point
    // (right after queryCaps()/createPostPipelines(), before any swapchain exists). This backend used
    // to call it from createSwapchainResources() instead, gated on cmdList4_, which is itself only
    // acquired once a swapchain's command list exists. That made "hardware ray tracing works" depend
    // on "a window was created", so every --headless run silently carried device5_ == null regardless
    // of what queryCaps() had just measured and logged: createBlas/createTlas would refuse with
    // "without ray-tracing support" while the caps line above them claimed RT tier 11. Calling it
    // unconditionally here, before createSwapchainResources ever runs, is what makes D3D12 match
    // Vulkan's shape and makes device5_ available to a headless process the same as a windowed one.
    initAccelerationStructures();

    rhiFactory_ = new D3D12ResourceFactory(this);
    if (!rhiFactory_->init()) { delete rhiFactory_; rhiFactory_ = nullptr; }
    else rhiContext_ = new D3D12RenderContext(this, rhiFactory_);

    AVER_INFO("[RHI.D3D12] device ready on adapter '{}'", adapterName_);
    if (rhiFactory_) rhiFactory_->selfTest();
    return true;
}

// Logs the debug layer's stored messages, each distinct ID once, and counts every one by severity.
void D3D12Device::drainDebugMessages() {
    if (!infoQueue_) return;
    const UINT64 n = infoQueue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<u8> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(infoQueue_->GetMessage(i, m, &len))) continue;

        switch (m->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: ++dbgCorruption_; break;
            case D3D12_MESSAGE_SEVERITY_ERROR:      ++dbgError_;      break;
            case D3D12_MESSAGE_SEVERITY_WARNING:    ++dbgWarning_;    break;
            default: continue;
        }
        const u32 id = static_cast<u32>(m->ID);
        bool seen = false;
        for (u32 s : seenMessageIds_) if (s == id) { seen = true; break; }
        if (seen) continue;
        seenMessageIds_.push_back(id);
        const std::string text(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0);
        if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
            AVER_WARN("[RHI.D3D12] debug layer #{}: {}", id, text);
        else
            AVER_ERROR("[RHI.D3D12] debug layer #{}: {}", id, text);
    }
    infoQueue_->ClearStoredMessages();
}

// Waits for the GPU, reports the debug-layer totals, and tears the device down.
D3D12Device::~D3D12Device() {
    waitForGpu();
    if (infoQueue_) {
        drainDebugMessages();
        AVER_INFO("[RHI.D3D12] debug layer totals: {} corruption, {} error, {} warning",
                  dbgCorruption_, dbgError_, dbgWarning_);
    }
    delete rhiContext_;
    delete rhiFactory_;
    uiShutdown();
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

IResourceFactory* D3D12Device::resources() { return rhiFactory_; }
IRenderContext* D3D12Device::renderContext() { return rhiContext_; }

// See RHI.hpp's own comment on IDevice::sceneDepthTexture. Lazily (re)adopts depthBuffer_ into the
// factory's texture table whenever createDepthBuffer() has run since the last call -- see
// depthTexDirty_'s own comment for why that flag, not a size comparison, is the trigger: this method
// has no cheap way to tell "the SAME resource" from "a resource that happens to be the same size" on
// its own, and createDepthBuffer() already knows exactly when it replaced the resource.
TextureHandle D3D12Device::sceneDepthTexture() {
    if (!depthBuffer_ || !rhiFactory_) return 0;
    if (depthTexDirty_) {
        depthTexHandle_ = rhiFactory_->adoptExternalDepthTexture(
            depthBuffer_.Get(), sceneWidth_, sceneHeight_, depthTexHandle_);
        depthTexDirty_ = false;
    }
    return depthTexHandle_;
}

void D3D12Device::addRenderFeature(IRenderFeature* f) {
    if (!f) return;
    for (IRenderFeature* e : features_) if (e == f) return;
    features_.push_back(f);
    AVER_INFO("[RHI.D3D12] render feature registered: {}", f->name());
    // A feature registering after the device already knows its targets -- the common case, since
    // the swapchain exists before any feature does -- would otherwise only learn them on the NEXT
    // change, which may never come in a run that is never resized. sceneWidth_/sceneHeight_ are 0
    // only for a swapchain-less device, where there is nothing meaningful to tell it yet.
    if (sceneWidth_ > 0 && sceneHeight_ > 0)
        f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat(), sceneWidth_, sceneHeight_);
}

void D3D12Device::removeRenderFeature(IRenderFeature* f) {
    for (usize i = 0; i < features_.size(); ++i) {
        if (features_[i] != f) continue;
        features_.erase(features_.begin() + static_cast<isize>(i));
        return;
    }
}

// Fills caps_ from the hardware, then applies the --force-caps clamp.
void D3D12Device::queryCaps() {
    caps_ = {};
    caps_.computeShaders = true;
    caps_.msaaMask = 1;
    caps_.maxMsaaSamples = 1;
    for (u32 s : {2u, 4u, 8u}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms{};
        ms.Format = kSceneColorFormat;
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
        caps_.resourceBindingTier = static_cast<u32>(o.ResourceBindingTier);
    }
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
    const DeviceCaps hw = caps_;
    clampCaps(caps_);
    AVER_INFO("[RHI.D3D12] caps: MSAA {}x, RT tier {}, SM {}, mesh-shader tier {}, DXC {}, cons-raster {}, binding tier {}",
              caps_.maxMsaaSamples, caps_.rayTracingTier, caps_.shaderModel,
              caps_.meshShaderTier, caps_.dxcAvailable, caps_.conservativeRaster,
              caps_.resourceBindingTier);
    if (capsOverride().active)
        AVER_WARN("[RHI.D3D12] caps CLAMPED by --force-caps; the hardware reports MSAA {}x, RT tier {}, SM {}, mesh-shader tier {}, DXC {}, cons-raster {}, binding tier {}",
                  hw.maxMsaaSamples, hw.rayTracingTier, hw.shaderModel, hw.meshShaderTier,
                  hw.dxcAvailable, hw.conservativeRaster, hw.resourceBindingTier);
}

// Rebuild everything that bakes the sample count: the MSAA colour/depth targets and every PSO.
bool D3D12Device::setSampleCount(u32 samples) {
    if (samples == sampleCount_) return true;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) return false;
    if (!(caps_.msaaMask & samples)) return false;

    waitForGpu();
    const u32 prev = sampleCount_;
    sampleCount_ = samples;
    if (!createPipeline()) { sampleCount_ = prev; createPipeline(); return false; }
    if (msSupported_) initMeshShaders();
    if (hasSwapchain_) {
        depthBuffer_.Reset();
        msaaColor_.Reset();
        if (!createDepthBuffer() || !createMsaaColor()) { AVER_ERROR("[RHI.D3D12] MSAA {}x target creation failed", samples); return false; }
        releasePostTargets();
    }
    notifyRenderTargetsChanged();
    AVER_INFO("[RHI.D3D12] MSAA set to {}x", samples);
    return true;
}

// The offscreen the post chain composites into when the editor wants the scene as an image.
// Full backbuffer size, recreated only when that changes.
bool D3D12Device::ensureViewportTexture() {
    IResourceFactory* f = resources();
    if (!f || width_ == 0 || height_ == 0) return false;
    if (viewportTex_ && viewportTexW_ == width_ && viewportTexH_ == height_) return true;

    if (viewportTex_) {
        f->waitIdle();
        f->destroyTexture(viewportTex_);
        viewportTex_ = 0;
    }
    TextureDesc d;
    d.width = width_;
    d.height = height_;
    d.format = fromDxgiFormat(kBackbufferFormat);
    d.bind = ResourceBind::RenderTarget | ResourceBind::ShaderResource;
    d.initialState = ResourceState::ShaderResource;
    d.hasClearValue = true;
    d.debugName = "Viewport.Composite";
    viewportTex_ = f->createTexture(d);
    viewportTexW_ = width_;
    viewportTexH_ = height_;
    if (!viewportTex_) { AVER_ERROR("[RHI.D3D12] the viewport texture could not be created"); return false; }
    AVER_INFO("[RHI.D3D12] viewport composited to a texture ({}x{})", width_, height_);
    return true;
}

u64 D3D12Device::viewportTextureId() {
    if (!viewportToTex_ || !ensureViewportTexture()) return 0;
    return uiTextureId(viewportTex_);
}

// Tells every feature the render targets changed, but only when a pipeline-baked property did.
// SCENE size (sceneWidth_/sceneHeight_), not present size: a render feature's targets (Voxi's
// ray-traced shadow/reflection history, notably) need to match what they actually render into.
void D3D12Device::notifyRenderTargetsChanged() {
    if (sampleCount_ == notifiedSamples_ &&
        backbufferFormat() == notifiedColor_ && depthFormat() == notifiedDepth_ &&
        sceneWidth_ == notifiedWidth_ && sceneHeight_ == notifiedHeight_) return;
    notifiedSamples_ = sampleCount_;
    notifiedColor_   = backbufferFormat();
    notifiedDepth_   = depthFormat();
    notifiedWidth_   = sceneWidth_;
    notifiedHeight_  = sceneHeight_;
    for (IRenderFeature* f : features_)
        f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat(), sceneWidth_, sceneHeight_);
}

// Builds the backend's own root signature and its solid, wireframe, sky and line pipelines.
bool D3D12Device::createPipeline() {
    D3D12_ROOT_PARAMETER params[2] = {};
    params[kSceneFrameParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kSceneFrameParam].Descriptor.ShaderRegister = kEngineFrameConstantRegister;
    params[kSceneObjectParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kSceneObjectParam].Constants.ShaderRegister = 1;
    params[kSceneObjectParam].Constants.Num32BitValues = kObjectConstantDwords;
    for (auto& rp : params) rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "SerializeRootSignature")) return false;
    if (!hrOk(device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)), "CreateRootSignature")) return false;
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(shaderCompiler().compile(sceneShaderSource().c_str(), "VSMain", "vs_5_1", &vs))) {
        return false;
    }
    if (FAILED(shaderCompiler().compile(sceneShaderSource().c_str(), "PSMainPlain", "ps_5_1", &ps))) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {kMeshInputLayout, kMeshInputLayoutCount};
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
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = kDepthFormat;
    pso.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)), "CreateGraphicsPipelineState")) return false;

    ComPtr<ID3DBlob> vsky, psky;
    if (FAILED(shaderCompiler().compile(sceneShaderSource().c_str(), "VSky", "vs_5_1", &vsky))) { return false;
    }
    if (FAILED(shaderCompiler().compile(sceneShaderSource().c_str(), "PSky", "ps_5_1", &psky))) { return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC sp{};
    sp.pRootSignature = rootSig_.Get();
    sp.VS = {vsky->GetBufferPointer(), vsky->GetBufferSize()};
    sp.PS = {psky->GetBufferPointer(), psky->GetBufferSize()};
    sp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    sp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    sp.RasterizerState.MultisampleEnable = TRUE;
    sp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // DEPTH-TESTED NOW, NOT DISABLED. The sky draws AFTER opaque geometry (endFrame, not beginFrame
    // -- see the call site), so by the time this runs the depth buffer already holds every opaque
    // pixel's real depth and the cleared far value (1.0) everywhere nothing was drawn. VSky emits
    // o.pos = float4(ndc, 1.0, 1.0), so the sky rasterizes at EXACTLY 1.0.
    //
    // EQUAL, NOT GREATER_EQUAL -- caught by a gate run that turned the ENTIRE image one flat colour.
    // The sky's own depth is a CONSTANT at the maximum of the standard [0,1] range this scene uses
    // (opaque geometry writes DepthFunc=LESS, so smaller means closer and 1.0 is the far plane). A
    // constant pinned at the range's own maximum makes GREATER_EQUAL (new >= stored) trivially true
    // for every stored value there is -- there is nothing a stored depth could be that is NOT <= 1.0
    // -- so the sky depth-tested as "in front of" geometry that was, by construction, always in front
    // of it. EQUAL is the actual "still at the clear value" test: true only where nothing closer was
    // ever written, false everywhere real geometry left a smaller depth behind.
    sp.DepthStencilState.DepthEnable = TRUE;
    sp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    sp.SampleMask = UINT_MAX;
    sp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    sp.NumRenderTargets = 1;
    sp.RTVFormats[0] = kSceneColorFormat;
    sp.DSVFormat = kDepthFormat;
    sp.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&skyPso_)), "CreateGraphicsPipelineState(sky)")) return false;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&wirePso_)), "wire pso")) return false;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;

    ComPtr<ID3DBlob> vln, pln;
    if (FAILED(shaderCompiler().compile(sceneShaderSource().c_str(), "VSLine", "vs_5_1", &vln))) { return false;
    }
    if (FAILED(shaderCompiler().compile(sceneShaderSource().c_str(), "PSLine", "ps_5_1", &pln))) { return false;
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
    lp.RTVFormats[0] = kSceneColorFormat;
    lp.DSVFormat = kDepthFormat;
    lp.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&lp, IID_PPV_ARGS(&linePso_)), "line pso")) return false;

    lp.DepthStencilState.DepthEnable = FALSE;
    lp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (!hrOk(device_->CreateGraphicsPipelineState(&lp, IID_PPV_ARGS(&lineOverlayPso_)), "line overlay pso")) return false;

    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    static_assert(sizeof(PerFrameCB) % 16 == 0, "a constant buffer's rows are float4s");
    auto cbd = bufferDesc((sizeof(PerFrameCB) + 255) & ~usize(255));
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frameCBs_[i])), "create per-frame CB")) return false;
        D3D12_RANGE none{0, 0};
        frameCBs_[i]->Map(0, &none, reinterpret_cast<void**>(&frameCBPtr_[i]));
    }
    return true;
}

// ---------------------------------------------------------------- DXR 1.1 acceleration structures
namespace {
// Committed default-heap buffer sized for an acceleration structure or its scratch space.
// D3D12 honours `state` only for RAYTRACING_ACCELERATION_STRUCTURE; every other buffer starts COMMON.
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

// Acquires the DXR 1.1 interfaces the generic factory builds acceleration structures with.
bool D3D12Device::initAccelerationStructures() {
    if (caps_.rayTracingTier < 11 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    if (FAILED(device_.As(&device5_))) return false;
    AVER_INFO("[RHI.D3D12] DXR 1.1 acceleration structures available");
    return true;
}

namespace {
// One {type, value} pair of a D3D12 pipeline-state subobject stream, laid out as d3dx12.h does.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
    Subobject& operator=(const T& v) { value = v; return *this; }
};

// The subobject stream a mesh-shader PSO is created from. `as` is OPTIONAL: a default-constructed
// Subobject holds a zero-length D3D12_SHADER_BYTECODE, which the runtime treats exactly as if the AS
// subobject were omitted -- so D3D12Device::initMeshShaders' fixed voxelisation PSO, which never
// touches `s.as`, is unaffected by this member existing on the shared struct.
struct MeshPsoStream {
    Subobject<ID3D12RootSignature*,     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE>    rootSig;
    Subobject<D3D12_SHADER_BYTECODE,    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS>                as;
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

// Mesh shader path. Needs Tier 1 + SM 6.5 + DXC, i.e. the same D3D12 Ultimate floor as DXR 1.1.
bool D3D12Device::initMeshShaders() {
    msSupported_ = false;
    if (caps_.meshShaderTier == 0 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    ComPtr<ID3D12Device2> device2;
    if (FAILED(device_.As(&device2))) return false;
    if (!cmdList6_) return false;

    if (!msRootSig_) {
        D3D12_ROOT_PARAMETER p[5] = {};
        p[kSceneFrameParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[kSceneFrameParam].Descriptor.ShaderRegister = kEngineFrameConstantRegister;
        p[kSceneObjectParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kSceneObjectParam].Constants.ShaderRegister = 1;
        p[kSceneObjectParam].Constants.Num32BitValues = kObjectConstantDwords;
        p[kMeshVertexParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[kMeshVertexParam].Descriptor.ShaderRegister = kSceneMeshSrvBase;
        p[kMeshIndexParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[kMeshIndexParam].Descriptor.ShaderRegister = kSceneMeshSrvBase + 1;
        p[kMeshCountParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kMeshCountParam].Constants.ShaderRegister = kMeshGeometryConstantRegister;
        p[kMeshCountParam].Constants.Num32BitValues = 4;
        for (auto& rp : p) rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 5; rsd.pParameters = p;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> b, e;
        if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "ms root sig")) return false;
        if (!hrOk(device_->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&msRootSig_)), "ms root sig create")) return false;
    }

    const std::string msDefs = "AVER_MS=1;AVER_MS_VTX_REG=" + std::to_string(kSceneMeshSrvBase) +
                               ";AVER_MS_IDX_REG=" + std::to_string(kSceneMeshSrvBase + 1);
    ComPtr<ID3DBlob> ms, ps;
    auto ok = [&](const char* entry, const char* fxcTarget, const char* target, ComPtr<ID3DBlob>& out,
                  const char* defs) {
        return SUCCEEDED(shaderCompiler().compile(sceneShaderSource().c_str(), entry, fxcTarget, &out, target, defs));
    };
    if (!ok("MSMain",      "vs_5_1", "ms_6_5", ms, msDefs.c_str()) ||
        !ok("PSMainPlain", "ps_5_1", "ps_6_5", ps, msDefs.c_str())) {
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
    s.rtvs.value.RTFormats[0] = kSceneColorFormat;
    s.dsv = kDepthFormat;
    s.sample.value.Count = sampleCount_;
    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
    if (!hrOk(device2->CreatePipelineState(&sd, IID_PPV_ARGS(&msPso_)), "mesh pso")) return false;

    msSupported_ = true;
    AVER_INFO("[RHI.D3D12] mesh shader path ready (ms_6_5)");
    return true;
}

// ---------------------------------------------------------------- shared root binding
// Switches the graphics root signature and rebinds the shared parameters a switch resets.
void D3D12Device::bindGraphicsRoot(ID3D12RootSignature* rs) {
    if (boundRootSig_ == rs) return;
    boundRootSig_ = rs;
    cmdList_->SetGraphicsRootSignature(rs);
    cmdList_->SetGraphicsRootConstantBufferView(kSceneFrameParam, frameCBs_[frameIndex_]->GetGPUVirtualAddress());
}

// Creates the swapchain, its views, the depth and scene colour targets, and the frame command list.
bool D3D12Device::createSwapchainResources(const SwapchainDesc& d) {
    if (!d.windowHandle) { AVER_WARN("[RHI.D3D12] createSwapchain without a window (headless)"); return false; }
    width_ = d.width; height_ = d.height;
    computeSceneSize();

    {
        ComPtr<IDXGIFactory5> f5;
        BOOL allow = FALSE;
        if (SUCCEEDED(factory_.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            tearingSupported_ = allow != FALSE;
        AVER_INFO("[RHI.D3D12] tearing (vsync-off) {}", tearingSupported_ ? "supported" : "unavailable");
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_; sd.Height = height_;
    sd.Format = kBackbufferFormat;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrameCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    if (tearingSupported_) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

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
    cmdList_.As(&cmdList4_);
    cmdList_.As(&cmdList6_);
    cmdList_->Close();
    // initAccelerationStructures() already ran unconditionally from init() -- see the comment there.
    // cmdList4_ itself is still acquired here: buildBlas/buildTlas record onto it, and that recording
    // genuinely does need the swapchain's per-frame command list, which is a separate requirement
    // from device5_'s existence.
    if (cmdList6_) initMeshShaders();

    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    hrOk(device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_)), "capture buffer");

    fenceValues_[0] = fenceValues_[1] = 0; nextFence_ = 0;
    hasSwapchain_ = true;
    AVER_INFO("[RHI.D3D12] swapchain {}x{} + depth (D32) ({} buffers, FLIP_DISCARD)", width_, height_, kFrameCount);
    return true;
}

// Creates one render target view per swapchain backbuffer.
void D3D12Device::createRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < kFrameCount; ++i) {
        swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i]));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, rtv);
        rtv.ptr += rtvSize_;
    }
}

// Creates the depth target at the current scene size and sample count.
bool D3D12Device::createDepthBuffer() {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = sceneWidth_; td.Height = sceneHeight_;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    // TYPELESS (kDepthResourceFormat), not kDepthFormat directly -- see that constant's own comment.
    td.Format = kDepthResourceFormat; td.SampleDesc.Count = sampleCount_;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // The clear value still names the concrete depth format: a D3D12_CLEAR_VALUE on a typeless
    // resource must be one of the formats that resource can be VIEWED as, and D32_FLOAT (kDepthFormat)
    // is the DSV's own view format below, unchanged from before this resource became typeless.
    D3D12_CLEAR_VALUE cv{}; cv.Format = kDepthFormat; cv.DepthStencil.Depth = 1.0f;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depthBuffer_)), "depth buffer")) return false;
    // An EXPLICIT view desc, where a bare `nullptr` used to be enough: CreateDepthStencilView infers
    // the view format from the resource's own format only when the resource is NOT typeless, and a
    // typeless resource additionally needs to be told whether it is multisampled -- the dimension is
    // not otherwise recoverable from a DXGI_FORMAT_R32_TYPELESS resource description alone.
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = kDepthFormat;
    dv.ViewDimension = (sampleCount_ > 1) ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthBuffer_.Get(), &dv, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    // The resource just changed identity (a fresh allocation, possibly at a new size/sample count);
    // sceneDepthTexture() re-adopts it lazily the next time something asks, rather than eagerly here
    // where no IResourceFactory call site is guaranteed to be safe yet (this runs during swapchain
    // (re)creation, ahead of rhiFactory_ existing on the very first call).
    depthTexDirty_ = true;
    return true;
}

// Creates the scene colour target at the current scene size and sample count.
bool D3D12Device::createMsaaColor() {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = sceneWidth_; td.Height = sceneHeight_;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kSceneColorFormat; td.SampleDesc.Count = sampleCount_;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{}; cv.Format = kSceneColorFormat;
    toSceneReferred(clear_, cv.Color);
    for (int i = 0; i < 4; ++i) { sceneClear_[i] = cv.Color[i]; msaaClear_[i] = clear_[i]; }
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&msaaColor_)), "msaa color")) return false;
    device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

// Rebuilds the scene colour target when setClearColor has moved off the value it was created with.
void D3D12Device::reconcileClearValue() {
    bool same = true;
    for (int i = 0; i < 4; ++i) if (clear_[i] != msaaClear_[i]) { same = false; break; }
    if (same || !msaaColor_) return;

    f32 prev[4];
    for (int i = 0; i < 4; ++i) prev[i] = msaaClear_[i];

    waitForGpu();
    msaaColor_.Reset();
    if (!createMsaaColor()) {
        AVER_ERROR("[RHI.D3D12] scene colour target rebuild for clear ({:.3f},{:.3f},{:.3f},{:.3f}) failed; keeping the previous clear value",
                   clear_[0], clear_[1], clear_[2], clear_[3]);
        for (int i = 0; i < 4; ++i) clear_[i] = prev[i];
        msaaColor_.Reset();
        createMsaaColor();
        return;
    }
    releasePostTargets();
    AVER_TRACE("[RHI.D3D12] scene colour target rebuilt for clear ({:.3f},{:.3f},{:.3f},{:.3f})",
               clear_[0], clear_[1], clear_[2], clear_[3]);
}

// Uploads a mesh to the GPU and returns its handle.
MeshHandle D3D12Device::createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) {
    if (!device_ || !rhiFactory_ || vcount == 0 || icount == 0) return 0;
    GpuMesh m;
    m.indexCount = icount;
    m.vertexCount = vcount;

    // ONE PASS OVER THE VERTICES, ONCE, AT CREATION -- not per frame, not per caller. An AABB, not a
    // tight sphere: min/max per axis, then centre = midpoint and radius = distance to a corner. That
    // radius is measured to the FARTHEST CORNER OF THE BOX, not the farthest actual vertex, so it is
    // never smaller than a true bounding sphere would be -- conservative in the direction that
    // matters for a culling test, where returning "might be visible" too often costs GPU cycles and
    // returning it too rarely costs a wrong picture.
    {
        f32 lo[3] = {verts[0].px, verts[0].py, verts[0].pz};
        f32 hi[3] = {verts[0].px, verts[0].py, verts[0].pz};
        for (u32 i = 1; i < vcount; ++i) {
            const f32 p[3] = {verts[i].px, verts[i].py, verts[i].pz};
            for (int a = 0; a < 3; ++a) { lo[a] = std::fmin(lo[a], p[a]); hi[a] = std::fmax(hi[a], p[a]); }
        }
        for (int a = 0; a < 3; ++a) m.boundsCentre[a] = 0.5f * (lo[a] + hi[a]);
        const f32 dx = hi[0] - m.boundsCentre[0], dy = hi[1] - m.boundsCentre[1], dz = hi[2] - m.boundsCentre[2];
        m.boundsRadius = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const u64 vbytes = static_cast<u64>(vcount) * sizeof(MeshVertex);
    const u64 ibytes = static_cast<u64>(icount) * sizeof(u32);

    // THROUGH THE FACTORY, not CreateCommittedResource. Same upload heap and same contents as
    // before -- but a factory buffer has an RhiBuffer entry, and only those can be given a
    // descriptor. Without that a shader could never read a mesh's own geometry, which is what a
    // ray needs the moment it wants to do anything more than ask whether something is there.
    BufferDesc vd;
    vd.bytes = vbytes;
    vd.kind = BufferKind::Upload;
    vd.debugName = "mesh vertices";
    m.vbBuffer = rhiFactory_->createBuffer(vd);

    BufferDesc idd;
    idd.bytes = ibytes;
    idd.kind = BufferKind::Upload;
    idd.debugName = "mesh indices";
    m.ibBuffer = rhiFactory_->createBuffer(idd);

    RhiBuffer* vrb = rhiFactory_->buffer(m.vbBuffer);
    RhiBuffer* irb = rhiFactory_->buffer(m.ibBuffer);
    if (!vrb || !vrb->res || !irb || !irb->res) {
        AVER_ERROR("[RHI.D3D12] createMesh could not allocate its buffers");
        if (m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        if (m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
        return 0;
    }
    m.vb = vrb->res;
    m.ib = irb->res;
    rhiFactory_->writeBuffer(m.vbBuffer, verts, vbytes, 0);
    rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);

    m.vbv.BufferLocation = m.vb->GetGPUVirtualAddress();
    m.vbv.SizeInBytes = static_cast<UINT>(vbytes);
    m.vbv.StrideInBytes = sizeof(MeshVertex);
    m.ibv.BufferLocation = m.ib->GetGPUVirtualAddress();
    m.ibv.SizeInBytes = static_cast<UINT>(ibytes);
    m.ibv.Format = DXGI_FORMAT_R32_UINT;

    meshes_.push_back(std::move(m));
    return static_cast<MeshHandle>(meshes_.size());
}

// Creates a mesh that shares `source`'s indices but owns a Default-heap, UAV-capable vertex buffer
// for a compute pass to write. See IDevice::createSkinTargetMesh for why this is a creation entry
// point and not a per-draw modifier.
MeshHandle D3D12Device::createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) {
    if (!device_ || !rhiFactory_) return 0;
    if (source == 0 || source > meshes_.size()) {
        AVER_ERROR("[RHI.D3D12] createSkinTargetMesh with an invalid source handle");
        return 0;
    }
    const GpuMesh& src = meshes_[source - 1];
    if (!src.vb || !src.ib || src.vbv.SizeInBytes == 0) return 0;

    // The vertex buffer goes through the RHI factory rather than being another committed resource
    // created here, because the skinning pass must be able to name it as a UAV -- and a UAV needs a
    // descriptor, which only the factory allocates.
    BufferDesc bd;
    bd.bytes = src.vbv.SizeInBytes;
    bd.kind  = BufferKind::Default;
    bd.allowUnorderedAccess = true;
    bd.debugName = "skin target vertices";
    const BufferHandle vh = rhiFactory_->createBuffer(bd);
    if (!vh) { AVER_ERROR("[RHI.D3D12] createSkinTargetMesh could not allocate its vertex buffer"); return 0; }

    RhiBuffer* rb = rhiFactory_->buffer(vh);
    if (!rb || !rb->res) { rhiFactory_->destroyBuffer(vh); return 0; }

    GpuMesh m;
    m.vb = rb->res;
    m.ib = src.ib;                 // SHARED: skinning moves vertices and never renumbers triangles
    m.ibv = src.ibv;
    m.indexCount = src.indexCount;
    m.vbBuffer = vh;
    // THE INDEX BUFFER'S HANDLE COMES ACROSS TOO, not just its raw pointer. meshGeometry() refuses
    // on `!m.vbBuffer || !m.ibBuffer` (:712), and ibBuffer defaulted to 0 here -- so every skin
    // target reported "no readable geometry" even though its indices are the source mesh's and are
    // perfectly readable. The visible consequence was that one skinned entity switched ray-traced
    // reflections off for the whole scene, because the BLAS build could not see its geometry.
    m.ibBuffer = src.ibBuffer;
    // ...and with it, the record that these indices are BORROWED. Without this the skin target
    // would free the source's index buffer on destruction and every mesh still drawing with it
    // would render from reclaimed memory.
    m.ibOwned = false;
    m.ibSource = source;
    // Likewise vertexCount, which stayed 0 and is what a geometry consumer sizes its read by.
    m.vertexCount = src.vertexCount;
    m.computeWritten = true;   // the whole point of this entry point
    m.vbv.BufferLocation = rb->res->GetGPUVirtualAddress();
    m.vbv.SizeInBytes = src.vbv.SizeInBytes;
    // From sizeof(MeshVertex) via the source, not re-derived: two independently-written strides is
    // exactly how a vertex buffer comes to be read at the wrong pitch.
    m.vbv.StrideInBytes = src.vbv.StrideInBytes;

    meshes_.push_back(std::move(m));
    const MeshHandle h = static_cast<MeshHandle>(meshes_.size());
    // Counted on the SOURCE, after the push_back -- `src` is a reference into meshes_ and the
    // push_back above may have reallocated it.
    meshes_[source - 1].ibShares += 1;

    // Seed it with the rest pose. Queued rather than done here because a copy needs an open command
    // list and this may well be called outside a frame -- and the point of seeding at all is that a
    // mesh drawn before anything poses it must show the bind pose rather than uninitialised memory.
    skinSeeds_.push_back({h, source});
    if (outVertices) *outVertices = vh;
    return h;
}

// Drains the queue of skin-target meshes awaiting their rest-pose seed. Runs at the top of a frame,
// where the command list is open and nothing has drawn yet.
void D3D12Device::seedSkinTargets() {
    if (skinSeeds_.empty() || !cmdList_) return;
    for (const SkinSeed& sd : skinSeeds_) {
        if (sd.dst == 0 || sd.dst > meshes_.size() || sd.src == 0 || sd.src > meshes_.size()) continue;
        GpuMesh& d = meshes_[sd.dst - 1];
        const GpuMesh& s = meshes_[sd.src - 1];
        if (!d.vb || !s.vb) continue;
        // The destination is a fresh Default-heap buffer in COMMON, so it PROMOTES to COPY_DEST
        // implicitly and needs no barrier to get there; the source is an upload-heap resource
        // permanently in GENERIC_READ and needs none either.
        cmdList_->CopyBufferRegion(d.vb.Get(), 0, s.vb.Get(), 0, d.vbv.SizeInBytes);

        // But the promotion LASTS FOR THE REST OF THE COMMAND LIST -- decay happens at submit, not
        // at the end of the copy. Without this the skinning pass's first barrier, in this same
        // list, claims Common on a resource the runtime knows is in COPY_DEST, and the debug layer
        // reports it once per frame forever. It is the promotion that is easy to reason about and
        // its lifetime that is not.
        auto back = transition(d.vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COMMON);
        cmdList_->ResourceBarrier(1, &back);
        AVER_TRACE("[RHI.D3D12] skin target {} seeded with the rest pose of mesh {}", sd.dst, sd.src);
    }
    skinSeeds_.clear();
}

// Creates the timestamp query heap and its readback buffer. Failure is not fatal: every timing
// call below no-ops when tsEnabled_ is false, so a device that cannot do timestamps still renders.
void D3D12Device::initGpuTiming() {
    if (!device_ || !queue_) return;
    if (FAILED(queue_->GetTimestampFrequency(&tsFrequency_)) || tsFrequency_ == 0) {
        AVER_INFO("[RHI.D3D12] the queue reports no timestamp frequency; per-pass GPU timing is off");
        return;
    }
    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = kMaxGpuStamps * kFrameCount;
    if (!hrOk(device_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&tsHeap_)), "timestamp query heap")) return;

    auto rb = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rd = bufferDesc(static_cast<u64>(kMaxGpuStamps) * kFrameCount * sizeof(u64));
    if (!hrOk(device_->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tsReadback_)), "timestamp readback")) {
        tsHeap_.Reset();
        return;
    }
    tsEnabled_ = true;
    AVER_INFO("[RHI.D3D12] per-pass GPU timing on ({} MHz timestamp clock)", tsFrequency_ / 1000000);
}

// Issues one timestamp and returns its slot, or kMaxGpuStamps when the frame has run out.
u32 D3D12Device::gpuStamp() {
    if (!tsEnabled_ || !cmdList_ || tsCount_ >= kMaxGpuStamps) {
        if (tsEnabled_ && tsCount_ >= kMaxGpuStamps && !tsWrapped_) {
            tsWrapped_ = true;
            AVER_WARN("[RHI.D3D12] more than {} GPU timestamps in one frame; the rest are unmeasured",
                      kMaxGpuStamps);
        }
        return kMaxGpuStamps;
    }
    const u32 slot = tsCount_++;
    cmdList_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex_ * kMaxGpuStamps + slot);
    return slot;
}

// Reads the timings this slice carried two frames ago -- already complete, because beginFrame
// fenced on it before calling here -- and folds them into the running TREE (see GpuAccum's own
// comment for why this is a tree and not the flat list it used to be).
void D3D12Device::collectGpuTiming() {
    if (!tsEnabled_ || !tsReadback_ || tsSlice_[frameIndex_].empty()) return;
    const u64 base = static_cast<u64>(frameIndex_) * kMaxGpuStamps * sizeof(u64);
    D3D12_RANGE rd{static_cast<SIZE_T>(base), static_cast<SIZE_T>(base + kMaxGpuStamps * sizeof(u64))};
    void* p = nullptr;
    if (FAILED(tsReadback_->Map(0, &rd, &p)) || !p) return;
    const u64* stamps = reinterpret_cast<const u64*>(static_cast<const u8*>(p) + base);

    const auto ms = [&](u64 a, u64 b) {
        return b > a ? 1000.0 * static_cast<f64>(b - a) / static_cast<f64>(tsFrequency_) : 0.0;
    };

    // Folds this frame's spans into the running tree, one accumulator node per (label, parent) pair
    // -- see GpuAccum's own comment for why that pair and not label alone. PARENTS BEFORE CHILDREN IS
    // GUARANTEED, not assumed: a span's parent is whatever was already open when the span itself
    // opened (beginGpuSpan/pushMarker), so a parent always lands in tsSlice_ at a LOWER index than
    // anything nested inside it, and walking the vector in order always resolves
    // tsSpanToAccum_[s.parent] before a child needs to read it.
    tsSpanToAccum_.assign(tsSlice_[frameIndex_].size(), kNoAccumParent);
    for (u32 i = 0; i < tsSlice_[frameIndex_].size(); ++i) {
        const GpuSpan& s = tsSlice_[frameIndex_][i];
        if (s.begin >= kMaxGpuStamps || s.end >= kMaxGpuStamps) continue;   // never closed; drop it
        const f64 d = ms(stamps[s.begin], stamps[s.end]);
        // A span whose PARENT was itself dropped (mismatched push/pop -- see gpuStamp's wrap warning
        // for the other way that happens) has nowhere honest to nest; folding it in as top-level, not
        // silently under whatever accum index happens to be lying in tsSpanToAccum_[s.parent], is
        // what turns that bug into a visibly wrong "unmarked" instead of a plausible-looking tree.
        const u32 accumParent = (s.parent == kNoParent) ? kNoAccumParent : tsSpanToAccum_[s.parent];
        auto it = std::find_if(tsAccum_.begin(), tsAccum_.end(), [&](const GpuAccum& a) {
            return a.label == s.label && a.parent == accumParent;
        });
        if (it == tsAccum_.end()) {
            tsAccum_.push_back({s.label, d, accumParent});
            tsSpanToAccum_[i] = static_cast<u32>(tsAccum_.size() - 1);
        } else {
            it->ms += d;
            tsSpanToAccum_[i] = static_cast<u32>(it - tsAccum_.begin());
        }
    }
    if (tsSliceBegin_[frameIndex_] < kMaxGpuStamps && tsSliceEnd_[frameIndex_] < kMaxGpuStamps)
        tsAccumFrameMs_ += ms(stamps[tsSliceBegin_[frameIndex_]], stamps[tsSliceEnd_[frameIndex_]]);
    D3D12_RANGE none{0, 0};
    tsReadback_->Unmap(0, &none);
    ++tsAccumFrames_;

    // Reported on a widening interval and as an AVERAGE over the frames since boot, because one
    // frame's timings on a streaming world say more about what streamed in than about the renderer.
    if ((tsReports_ & (tsReports_ + 1)) == 0 && tsAccumFrames_ >= 8) {
        const f64 n = static_cast<f64>(tsAccumFrames_);

        // Direct-children index, built once per report rather than kept live all the time: this
        // runs on a widening interval (a handful of times a minute at most), so an O(nodes) pass here
        // is free next to the cost of formatting the string it feeds.
        std::vector<std::vector<u32>> children(tsAccum_.size());
        std::vector<u32> topLevel;
        for (u32 i = 0; i < tsAccum_.size(); ++i) {
            if (tsAccum_[i].parent == kNoAccumParent) topLevel.push_back(i);
            else                                      children[tsAccum_[i].parent].push_back(i);
        }

        // unmarked = frame - TOP-LEVEL spans only, never every span -- summing every span (the old
        // formula, from when spans could not nest) double-counts anything nested, since a child's ms
        // is already folded into its parent's. This is the arithmetic fix nesting made mandatory, not
        // an optional cleanup alongside it -- see kNoParent's own comment on why the two are one change.
        f64 topLevelMs = 0;
        for (u32 i : topLevel) topLevelMs += tsAccum_[i].ms;

        // Printed as an indented tree, one line per node: label, INCLUSIVE ms/frame (itself plus
        // everything nested inside it -- exactly what the begin/end timestamps already measure, no
        // nesting-aware subtraction needed there), and EXCLUSIVE ms/frame (inclusive minus the sum of
        // direct children) in parentheses. Exclusive is the number this engine could never print
        // before nesting existed: it says where a pass's OWN time goes once its children's time is
        // taken back out, rather than leaving every nested child's cost smeared across its parent's
        // total the way a flat list always did.
        //
        // A LOCAL FUNCTOR, NOT A MEMBER FUNCTION OR std::function: this is the one place in the file
        // that needs a genuinely recursive local closure, `children`/`tsAccum_`/`n` are all local or
        // members already in scope, and neither this file nor RHIResources.hpp otherwise reaches for
        // <functional> -- a hand-rolled struct with operator() calling itself is the smallest thing
        // that does the job without adding an include for one call site.
        struct Appender {
            std::string& line;
            const std::vector<std::vector<u32>>& children;
            const std::vector<GpuAccum>& accum;
            f64 n;
            void operator()(u32 idx, u32 depth) const {
                const GpuAccum& a = accum[idx];
                f64 childMs = 0;
                for (u32 c : children[idx]) childMs += accum[c].ms;
                char buf[160];
                std::snprintf(buf, sizeof buf, "\n%*s%s %.1fms (excl %.1fms)",
                              static_cast<int>(depth) * 2 + 2, "", a.label.c_str(),
                              a.ms / n, (a.ms - childMs) / n);
                line += buf;
                for (u32 c : children[idx]) (*this)(c, depth + 1);
            }
        };
        std::string line;
        Appender append{line, children, tsAccum_, n};
        for (u32 i : topLevel) append(i, 0);

        AVER_INFO("[RHI.D3D12] GPU {:.1f}ms/frame over {} frames{} | unmarked {:.1f}ms",
                  tsAccumFrameMs_ / n, tsAccumFrames_, line, (tsAccumFrameMs_ - topLevelMs) / n);
    }
    ++tsReports_;
}

// Opens the frame: waits out the current backbuffer's last frame, resets recording, clears targets.
void D3D12Device::beginFrame() {
    if (!hasSwapchain_) return;
    reconcileClearValue();
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    const u64 want = fenceValues_[frameIndex_];
    if (want != 0) waitFence(want);
    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get());
    boundRootSig_ = nullptr;
    boundPso_ = pso_.Get();   // Reset's second argument IS the command list's initial bound PSO
    // Reset() does not carry descriptor heaps forward either -- a freshly reset command list has
    // none bound until the first SetDescriptorHeaps of the new recording, same as the root signature.
    boundHeap_ = nullptr;
    fovValid_ = false;   // a reset command list has nothing bound at all
    postCBUsed_ = 0;
    drawBinding_ = defaultDrawBinding_;
    // Carried into *_Last so anything that wants "did --depth-prepass draw anything last frame" can
    // read a settled number rather than one still being accumulated -- same handoff shape as
    // lastSceneDrawn_ in SandboxApp.cpp -- then zeroed for the frame about to record.
    depthPrepassDrawsLastFrame_ = depthPrepassDrawsThisFrame_;
    depthPrepassDrawsThisFrame_ = 0;
    nextDrawPrepassed_ = false;   // a reset command list has consumed nothing from last frame either

    // The fence above has retired whatever last used this slice, so its timestamps are readable
    // now. Collect BEFORE resetting the counters that are about to be reused.
    collectGpuTiming();
    tsCount_ = 0;
    tsOpen_.clear();
    tsDropped_ = 0;
    tsSlice_[frameIndex_].clear();
    tsSliceBegin_[frameIndex_] = gpuStamp();
    tsSliceEnd_[frameIndex_] = kMaxGpuStamps;
    // Before any feature's prePass and before any draw: a skin target must never be read in the
    // frame it was created, and this is the only point where that is guaranteed.
    seedSkinTargets();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    std::memcpy(frameCBPtr_[frameIndex_], &frameCB_, sizeof(PerFrameCB));
    for (IRenderFeature* f : features_) f->beginScene();

    if (rhiContext_) {
        for (IRenderFeature* f : features_) f->prePass(*rhiContext_);
        if (!features_.empty()) { boundRootSig_ = nullptr; boundPso_ = nullptr; }
    }

    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmdList_->ClearRenderTargetView(rtv, sceneClear_, 0, nullptr);
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Opened here rather than after the suppressesScene loop below, which can return early: this
    // must be open on EVERY path out of beginFrame, because endFrame closes it unconditionally.
    beginGpuSpan("scene draw");

    // vpX_/vpY_/vpW_/vpH_ are already scene-space (setViewportRect scales them); the fallback when
    // no sub-rect is set is the whole SCENE target, which this viewport draws into -- not width_/
    // height_, the present size.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    D3D12_VIEWPORT vp{rx, ry, rw, rh, 0.0f, 1.0f};
    D3D12_RECT sc{static_cast<LONG>(rx), static_cast<LONG>(ry), static_cast<LONG>(rx + rw), static_cast<LONG>(ry + rh)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);

    bindGraphicsRoot(rootSig_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    sceneSuppressed_ = false;
    frameSuppressed_ = false;
    for (IRenderFeature* f : features_) {
        if (!f->suppressesScene()) continue;
        if (rhiContext_) f->scenePass(*rhiContext_);
        cmdList_->SetPipelineState(pso_.Get());
        sceneSuppressed_ = true;
        if (f->suppressesWholeFrame()) frameSuppressed_ = true;
        return;
    }

    // THE SKY NO LONGER DRAWS HERE. It used to run first, depth-disabled, at 100% of the render
    // target's coverage regardless of how much of the final image opaque geometry would go on to
    // cover -- the single most expensive shader in this file (the atmosphere march in PSky) paying
    // full price on every pixel a wall, a tree or a character was about to sit in front of. It now
    // draws at the START of endFrame, once every opaque drawMesh call this frame has already
    // happened and the depth buffer holds their real depth -- see that function, and the sky PSO's
    // now depth-tested creation just above, for why moving it there is correct rather than a
    // reorder-and-hope: it lands on the SAME still-bound render target and depth buffer, before
    // endFrame's own first GPU work (the MSAA resolve) ever reads either.
    cmdList_->SetPipelineState(pso_.Get());
}

// Draws one mesh into the scene, through whichever pipeline owns the lit pass.
// Releases a mesh's GPU memory. See IDevice::destroyMesh for the handle-recycling argument.
bool D3D12Device::destroyMesh(MeshHandle mesh) {
    if (mesh == 0 || mesh > meshes_.size()) return false;
    GpuMesh& m = meshes_[mesh - 1];
    if (!m.alive) return false;   // already destroyed; saying so beats double-freeing

    // A source mesh whose indices someone else is still drawing with cannot go. Refusing loudly is
    // the whole point: freeing the buffer anyway would leave the skin target rendering from memory
    // the heap has handed to something else, which shows up as scrambled triangles somewhere
    // unrelated rather than as an error here.
    if (m.ibShares > 0) {
        AVER_WARN("[RHI.D3D12] destroyMesh({}) refused: {} skin target(s) still share its indices",
                  mesh, m.ibShares);
        return false;
    }

    // The acceleration structures FIRST. A BLAS holds this mesh's vertex and index GPU addresses,
    // so releasing the buffers while one is live would leave ray tracing traversing freed memory --
    // and unlike a raster draw, that faults the device rather than drawing a hole.
    if (rhiFactory_) rhiFactory_->destroyBlasForMesh(mesh);

    // Then the buffers, through the factory, so they retire behind the fence rather than being
    // released while a command list still in flight references them.
    if (rhiFactory_) {
        if (m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        // ONLY IF OWNED. A skin target's indices belong to its source.
        if (m.ibOwned && m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
    }
    // Give the source its share back, so a source held open only by this target can now go too.
    if (!m.ibOwned && m.ibSource != 0 && m.ibSource <= meshes_.size()) {
        GpuMesh& src = meshes_[m.ibSource - 1];
        if (src.ibShares > 0) src.ibShares -= 1;
    }

    // The slot is CLEARED AND KEPT, never recycled. A handle held past its mesh then names
    // something dead and draws nothing, instead of naming whatever was created next.
    m.vb.Reset();
    m.ib.Reset();
    m.vbv = D3D12_VERTEX_BUFFER_VIEW{};
    m.ibv = D3D12_INDEX_BUFFER_VIEW{};
    m.indexCount = 0;
    m.vertexCount = 0;
    m.vbBuffer = 0;
    m.ibBuffer = 0;
    m.computeWritten = false;
    m.ibSource = 0;
    m.boundsRadius = 0.0f;
    m.alive = false;
    return true;
}

// Draws `mesh`'s depth only, through whichever feature's depthPrepassPipeline() offers one -- see
// IDevice's own comment for the contract, and VoxiRenderer::depthPrepassPipeline for the one
// implementation today. STRUCTURALLY A SMALL COPY OF drawMesh()'s OWN feature-pipeline branch below
// (pipeline/table-0/frame-CB caching through the SAME fovPso_/fovSet_/fovCbBytes_ trio, the SAME
// table-1 material rebind, the SAME PerObject world write), because it is doing the same kind of
// draw through the same seam -- just a different pipeline and no colour/material tail. It is NOT
// folded into drawMesh() itself: this is called from a SEPARATE, earlier walk over the scene
// (SandboxApp's prepass phase, before its ordinary colour walk), never interleaved per-instance with
// colour draws -- interleaving them would split what should be ONE contiguous "depth prepass" GPU
// span (see the ScopedGpuStat around that walk) into hundreds of one-draw slivers, and this engine's
// GPU stat tree budgets 64 open spans a frame, not one per entity.
void D3D12Device::drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16]) {
    if (!hasSwapchain_ || !depthPrepassEnabled_ || !rhiContext_ || mesh == 0 || mesh > meshes_.size()) return;
    if (!meshes_[mesh - 1].alive) return;
    // Compute-written (skinned) meshes are excluded from the prepass -- see IDevice::
    // drawMeshDepthPrepass's own comment. The primary contract is the CALLER never offering one
    // (SandboxApp's prepass walk skips skinnedScene_ entities entirely); this is the defensive
    // second check, same shape as drawMesh()'s own re-derivation of `prepassed` just below.
    if (meshVertexBuffer(mesh) != 0) return;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline()) continue;
        const PipelineHandle pp = f->depthPrepassPipeline();
        if (!pp) return;   // this feature has no prepass PSO; nothing else offers one either today
        const BindingSetHandle bs = f->sceneBindingSet();
        const void* cb = nullptr; u32 cbBytes = 0;
        const bool haveCb = f->sceneConstants(&cb, &cbBytes) && cb && cbBytes;

        // The identical elision drawMesh() uses below, and deliberately the SAME cache variables:
        // whichever of the prepass PSO or the colour PSO ran last, a switch to the other always
        // re-sends table 0 and the frame CB even though both are the SAME Voxi resources either way
        // -- over-conservative, not wrong, and see drawMeshDepthPrepass's own file comment on why
        // that switch happens on every single eligible instance rather than being batched away.
        const bool same = fovValid_ && pp == fovPso_ && bs == fovSet_ &&
                          haveCb == (fovCbBytes_ != 0) &&
                          (!haveCb || (cbBytes == fovCbBytes_ && fovCb_.size() == cbBytes &&
                                       std::memcmp(fovCb_.data(), cb, cbBytes) == 0));
        if (!same) {
            rhiContext_->setPipeline(pp);
            if (bs) rhiContext_->setBindingSet(bs, 0);
            if (haveCb) rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
            fovPso_ = pp; fovSet_ = bs; fovCbBytes_ = haveCb ? cbBytes : 0;
            if (haveCb) fovCb_.assign(static_cast<const u8*>(cb), static_cast<const u8*>(cb) + cbBytes);
            else        fovCb_.clear();
            fovValid_ = true;
        }
        // Table 1: the SAME material binding the caller set via setDrawBinding for this instance's
        // upcoming colour draw -- PSDepthPrepass reads gBaseColorMap/gAlphaCutoff/gMaterialFlags out
        // of exactly that set, same registers PSMainVoxi reads them at (both pipelines share `gi`,
        // VoxiRenderer.cpp's giLayout()).
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        f32 fc[kObjectConstantDwords] = {};
        std::memcpy(fc, world, 16 * sizeof(f32));
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        rhiContext_->drawMesh(mesh);
        ++depthPrepassDrawsThisFrame_;
        boundRootSig_ = nullptr;
        boundPso_ = nullptr;
        return;
    }
}

void D3D12Device::drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) {
    // AUTO-CONSUME nextDrawPrepassed_ before any early return below, exactly per its own contract:
    // whether or not THIS call goes on to use it, the flag must not leak onto some later, unrelated
    // draw just because this one bailed out early (a dead mesh, no swapchain, a suppressed scene).
    const bool prepassed = nextDrawPrepassed_ && meshVertexBuffer(mesh) == 0;
    nextDrawPrepassed_ = false;
    if (!hasSwapchain_ || mesh == 0 || mesh > meshes_.size()) return;
    // A destroyed mesh draws NOTHING rather than drawing from a cleared vertex view. This is the
    // other half of not recycling handles: a caller that kept a handle too long gets a visible hole
    // it can trace, not a device removal.
    if (!meshes_[mesh - 1].alive) return;
    for (IRenderFeature* f : features_)
        f->submitDraw(mesh, world, color, metallic, roughness,
                      drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline() || !rhiContext_) continue;
        const PipelineHandle fp = f->scenePipeline(msActive_ && msPso_, wireframe_, prepassed);
        if (!fp) break;
        const BindingSetHandle bs = f->sceneBindingSet();
        const void* cb = nullptr; u32 cbBytes = 0;
        const bool haveCb = f->sceneConstants(&cb, &cbBytes) && cb && cbBytes;

        // Unchanged since the previous entity? Then the pipeline, its table-0 bindings and the frame
        // block are all still bound and still correct, and re-sending them is pure cost.
        const bool same = fovValid_ && fp == fovPso_ && bs == fovSet_ &&
                          haveCb == (fovCbBytes_ != 0) &&
                          (!haveCb || (cbBytes == fovCbBytes_ && fovCb_.size() == cbBytes &&
                                       std::memcmp(fovCb_.data(), cb, cbBytes) == 0));
        if (!same) {
            rhiContext_->setPipeline(fp);
            if (bs) rhiContext_->setBindingSet(bs, 0);
            if (haveCb) rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
            // Set AFTER the calls above: setPipeline/setBindingSet clear fovValid_ themselves.
            fovPso_ = fp; fovSet_ = bs; fovCbBytes_ = haveCb ? cbBytes : 0;
            if (haveCb) fovCb_.assign(static_cast<const u8*>(cb), static_cast<const u8*>(cb) + cbBytes);
            else        fovCb_.clear();
            fovValid_ = true;
        }
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        f32 fc[kObjectConstantDwords];
        std::memcpy(fc, world, 16 * sizeof(f32));
        std::memcpy(fc + 16, color, 4 * sizeof(f32));
        fc[20] = metallic; fc[21] = roughness; fc[22] = 0.0f; fc[23] = 0.0f;
        writeShadingConstants(fc);
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        if (msActive_ && msPso_ && !wireframe_) rhiContext_->dispatchMeshFor(mesh);
        else                                    rhiContext_->drawMesh(mesh);
        boundRootSig_ = nullptr;
        boundPso_ = nullptr;
        return;
    }

    if (drawBinding_.set && !drawBindingIgnored_) {
        AVER_WARN("[RHI.D3D12] a per-draw binding is set but the scene uses the backend's own pipeline, which declares no table 1; it is ignored");
        drawBindingIgnored_ = true;
    }

    const GpuMesh& m = meshes_[mesh - 1];
    const bool useMs = msActive_ && msPso_ && !wireframe_;
    bindGraphicsRoot(useMs ? msRootSig_.Get() : rootSig_.Get());
    // GUARDED THE SAME WAY bindGraphicsRoot ALREADY GUARDS THE ROOT SIGNATURE, right above. A scene
    // is typically hundreds to thousands of drawMesh calls sharing one PSO (same shading model, same
    // wireframe/mesh-shader mode), and this call re-issued SetPipelineState on every single one of
    // them regardless -- most drivers absorb a redundant identical PSO set cheaply, but it is still a
    // command-list entry paid for nothing. boundPso_ is invalidated everywhere boundRootSig_ already
    // is, since anything that can change the root signature (a feature pipeline, PSO recreation on
    // resize) can just as well change which PSO is actually bound.
    ID3D12PipelineState* wantPso = useMs ? msPso_.Get() : (wireframe_ ? wirePso_.Get() : pso_.Get());
    if (wantPso != boundPso_) { cmdList_->SetPipelineState(wantPso); boundPso_ = wantPso; }
    f32 consts[kObjectConstantDwords];
    std::memcpy(consts, world, 16 * sizeof(f32));
    std::memcpy(consts + 16, color, 4 * sizeof(f32));
    consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
    writeShadingConstants(consts);
    cmdList_->SetGraphicsRoot32BitConstants(kSceneObjectParam, kObjectConstantDwords, consts, 0);
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
    cmdList_->SetGraphicsRootShaderResourceView(kMeshVertexParam, m.vb->GetGPUVirtualAddress());
    cmdList_->SetGraphicsRootShaderResourceView(kMeshIndexParam, m.ib->GetGPUVirtualAddress());
    const u32 tc[4] = {tris, 0, 0, 0};
    cmdList_->SetGraphicsRoot32BitConstants(kMeshCountParam, 4, tc, 0);
    cmdList6_->DispatchMesh((tris + kMeshShaderTrisPerGroup - 1) / kMeshShaderTrisPerGroup, 1, 1);
}

// Uploads a line list to the GPU and returns its handle.
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

// Releases a line mesh. The SLOT stays, marked dead -- see IDevice::destroyLineMesh for why a
// stale handle must never be handed a live mesh.
bool D3D12Device::destroyLineMesh(LineHandle mesh) {
    if (mesh == 0 || mesh > lineMeshes_.size()) return false;
    GpuLineMesh& m = lineMeshes_[mesh - 1];
    if (!m.vb) return false;   // already released; saying so beats pretending it worked twice
    // DEFERRED, not immediate. The GPU may still be reading this buffer for a frame already in
    // flight, and an UPLOAD-heap resource released under a live command list is a use-after-free
    // that the debug layer reports somewhere else entirely, if at all. The resource factory already
    // owns a fence-keyed retire list for exactly this (D3D12ResourceFactory::retire), which is what
    // destroyMesh reaches indirectly through destroyBuffer -- a line buffer joins the same list
    // rather than growing a second mechanism beside it.
    if (rhiFactory_) rhiFactory_->retire(m.vb);
    m.vb.Reset();
    m.vbv = D3D12_VERTEX_BUFFER_VIEW{};
    m.count = 0;
    return true;
}

// Draws a line list, unless a feature has replaced the whole frame.
void D3D12Device::drawLines(LineHandle mesh, const f32 world[16]) {
    if (!hasSwapchain_ || mesh == 0 || mesh > lineMeshes_.size()) return;
    // Gizmos and wireframes belong in a ray-driven viewport as much as in a rastered one, and they
    // depth-test against the real depth the ray pass writes.
    for (IRenderFeature* f : features_) if (f->suppressesWholeFrame()) return;
    const GpuLineMesh& m = lineMeshes_[mesh - 1];
    // A DESTROYED MESH DRAWS NOTHING. The slot is kept so a stale handle names something dead
    // rather than something live (see IDevice::destroyLineMesh); this is the half that makes that
    // true, instead of binding a null vertex view and asking the driver for zero primitives.
    if (!m.vb || m.count == 0) return;
    bindGraphicsRoot(rootSig_.Get());
    cmdList_->SetPipelineState(lineDepth_ ? linePso_.Get() : lineOverlayPso_.Get());
    cmdList_->SetGraphicsRoot32BitConstants(kSceneObjectParam, 16, world, 0);
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->DrawInstanced(m.count, 1, 0, 0);
}

// ================================================================= the camera post chain
// Everything from here to runPostChain implements rhi::PostSettings.

// The CPU twin of the shared prelude's averInverseTonemap(srgbToLin(c)). Kept identical by hand.
void D3D12Device::toSceneReferred(const f32 display[4], f32 out[4]) {
    for (int i = 0; i < 3; ++i) {
        const f32 lin = std::pow(display[i] < 0.0f ? 0.0f : display[i], 2.2f);
        const f32 y = lin > 1.0329f - 1e-4f ? 1.0329f - 1e-4f : lin;
        const f32 a = 2.43f * y - 2.51f;
        const f32 b = 0.59f * y - 0.03f;
        const f32 c = 0.14f * y;
        const f32 d = b * b - 4.0f * a * c;
        out[i] = (-b - std::sqrt(d < 0.0f ? 0.0f : d)) / (2.0f * a);
    }
    out[3] = display[3];
}

// Packs the authored atmosphere into the block the shaders read.
void D3D12Device::setSkyAtmosphere(const SkyAtmosphere& s) {
    sky_ = s;
    skyEnabled_ = s.enabled;

    for (int i = 0; i < 3; ++i) frameCB_.lightDir[i] = s.sunDirection[i];
    frameCB_.lightDir[3] = 0.0f;

    f32 sun[3] = {s.sunColor[0], s.sunColor[1], s.sunColor[2]};
    if (s.sunTemperatureK > 0.0f) blackbodySrgb(s.sunTemperatureK, sun);
    for (int i = 0; i < 3; ++i) frameCB_.lightColor[i] = sun[i];
    frameCB_.lightColor[3] = 0.0f;

    for (int i = 0; i < 3; ++i) {
        frameCB_.skyZenith[i]   = s.zenith[i];
        frameCB_.skyHorizon[i]  = s.horizon[i];
        frameCB_.fogColor[i]    = s.fogColor[i];
        frameCB_.groundColor[i] = s.groundAlbedo[i];
        frameCB_.ambient[i]     = s.skyLightIntensity;
    }
    frameCB_.skyZenith[3] = frameCB_.skyHorizon[3] = 0.0f;
    frameCB_.groundColor[3] = s.groundBlend;
    frameCB_.ambient[3]   = 0.0f;
    frameCB_.fogColor[3]  = s.fogDensity;

    frameCB_.skyParams[0] = s.atmosphereHeight > 0.01f ? s.atmosphereHeight : 0.01f;
    frameCB_.skyParams[1] = s.skyLightIntensity;
    frameCB_.skyParams[2] = s.sunIntensity;
    frameCB_.skyParams[3] = std::cos(s.sunAngularDiameterDeg * 0.5f * 0.017453292f);

    frameCB_.fogParams[0] = s.fogFalloff;
    frameCB_.fogParams[1] = s.fogHeight;
    frameCB_.fogParams[2] = s.fogStart;
    frameCB_.fogParams[3] = s.fogMaxOpacity;

    frameCB_.cloudParams[0] = s.cloudCoverage;
    frameCB_.cloudParams[1] = s.cloudDensity;
    frameCB_.cloudParams[2] = s.cloudBottom;
    frameCB_.cloudParams[3] = s.cloudTop > s.cloudBottom ? s.cloudTop : s.cloudBottom + 1.0f;
    // THE SEED IS AN OFFSET IN THE NOISE DOMAIN, which needs no shader change: the cloud density
    // function already samples at (wpos + cloudMotion.xy) * scale, and translating a noise field far
    // enough is indistinguishable from a different field. Reusing the wind offset costs no constant
    // -- the buffer is full -- and keeps the seed on exactly the axis the noise already varies on.
    //
    // Seed 0 adds nothing at all, so an unseeded sky is bit-identical to the sky before this
    // existed. The offsets are large and irrational-ish so two nearby seeds do not land in
    // neighbouring cells of the same feature.
    if (s.cloudSeed == 0) {
        // THE UNSEEDED PATH IS THE ORIGINAL EXPRESSION, not the seeded one with a zero added. That
        // is not superstition: x + 0.0f is bit-identical to x for every float EXCEPT negative zero,
        // which -0.0f + 0.0f turns into +0.0f. Nothing downstream can see that difference, but
        // "provably the same instruction" is worth more here than "numerically equivalent" -- the
        // recorded gate baselines are bit-exact codes, and this is how a change stays outside them.
        frameCB_.cloudMotion[0] = s.cloudWind[0] * s.cloudTime;
        frameCB_.cloudMotion[1] = s.cloudWind[1] * s.cloudTime;
    } else {
        u32 h = static_cast<u32>(s.cloudSeed) + 0x9E3779B9u;
        h = (h ^ (h >> 16)) * 0x21F0AAADu;
        h = (h ^ (h >> 15)) * 0x735A2D97u;
        h ^= h >> 15;
        frameCB_.cloudMotion[0] = s.cloudWind[0] * s.cloudTime + static_cast<f32>(h & 0xFFFFu) * 977.0f;
        frameCB_.cloudMotion[1] = s.cloudWind[1] * s.cloudTime + static_cast<f32>(h >> 16)     * 1361.0f;
    }
    frameCB_.cloudMotion[2] = s.cloudScale;
    frameCB_.cloudMotion[3] = s.cloudsEnabled ? 1.0f : 0.0f;

    packAtmosphere(s);
}

// Packs the physical atmosphere, and derives the dome, its exponent and the sun colour when it is on.
// The authored SkyAtmosphere fields are never modified, only the constant-buffer copies.
void D3D12Device::packAtmosphere(const SkyAtmosphere& s) {
    const AtmosphereProfile& a = s.air;
    const bool on = s.model == SkyModel::Physical;

    for (int i = 0; i < 3; ++i) {
        frameCB_.atmoRayleigh[i] = a.rayleighScatter[i];
        frameCB_.atmoOzone[i]    = a.ozoneAbsorb[i];
    }
    frameCB_.atmoRayleigh[3] = a.rayleighScaleKm > 1e-3f ? a.rayleighScaleKm : 1e-3f;
    frameCB_.atmoOzone[3]    = a.ozoneWidthKm > 1e-3f ? a.ozoneWidthKm : 1e-3f;
    frameCB_.atmoMie[0] = a.mieScatter;
    frameCB_.atmoMie[1] = a.mieExtinction;
    frameCB_.atmoMie[2] = a.mieScaleKm > 1e-3f ? a.mieScaleKm : 1e-3f;
    frameCB_.atmoMie[3] = a.miePhaseG;
    frameCB_.atmoPlanet[0] = a.planetRadiusKm;
    frameCB_.atmoPlanet[1] = a.planetRadiusKm + a.atmosphereHeightKm;
    frameCB_.atmoPlanet[2] = 1e-5f;
    frameCB_.atmoPlanet[3] = on ? 1.0f : 0.0f;
    frameCB_.atmoTune[0] = a.ozoneCentreKm;
    frameCB_.atmoTune[1] = a.multiScatterGain;
    frameCB_.atmoTune[2] = static_cast<f32>(a.viewSteps > 1 ? a.viewSteps : 1);
    frameCB_.atmoTune[3] = static_cast<f32>(a.aerialSteps > 1 ? a.aerialSteps : 1);

    // BEFORE the early-out: with the atmosphere off this function returns without touching the
    // tail, and an unwritten furnace row is whatever the last frame left there. A shading model
    // that silently enters furnace mode would be far harder to diagnose than one that never does.
    frameCB_.furnace[0] = s.furnaceRadiance > 0.0f ? 1.0f : 0.0f;
    frameCB_.furnace[1] = s.furnaceRadiance;
    frameCB_.furnace[2] = s.furnaceSun ? 1.0f : 0.0f;
    frameCB_.furnace[3] = 0.0f;

    if (!on) {
        for (int i = 0; i < 4; ++i) frameCB_.atmoSunE0[i] = 0.0f;
        // Zeroed for the same reason the furnace row is written above the early-out: a stale
        // row is a worse failure than an empty one. Nothing reads these with the atmosphere
        // off -- averSkyIrradiance gates on averAtmoOn() -- but leaving last frame's sky here
        // would make any future reader that forgets the gate fail intermittently.
        for (int k = 0; k < 9; ++k)
            for (int i = 0; i < 4; ++i) frameCB_.skySh[k][i] = 0.0f;
        return;
    }

    AtmosphereProfile fit = a;
    f32 groundLin[3];
    for (int i = 0; i < 3; ++i) groundLin[i] = std::pow(std::fmax(s.groundAlbedo[i], 0.0f), 2.2f);
    fit.groundAlbedo = 0.2126f * groundLin[0] + 0.7152f * groundLin[1] + 0.0722f * groundLin[2];

    f32 e0[3];
    for (int i = 0; i < 3; ++i)
        e0[i] = std::pow(std::fmax(frameCB_.lightColor[i], 0.0f), 2.2f) * s.sunIntensity;
    for (int i = 0; i < 3; ++i) frameCB_.atmoSunE0[i] = e0[i];
    frameCB_.atmoSunE0[3] = fit.groundAlbedo;

    const f32 len = std::sqrt(s.sunDirection[0] * s.sunDirection[0] +
                              s.sunDirection[1] * s.sunDirection[1] +
                              s.sunDirection[2] * s.sunDirection[2]);
    const f32 sunCos = len > 1e-6f ? s.sunDirection[2] / len : 1.0f;
    const f32 sunRadius = s.sunAngularDiameterDeg * 0.5f * 0.017453292f;
    AtmosphereDome dome{};
    atmoFitDome(fit, 0.0f, sunCos, e0, sunRadius, dome);

    for (int i = 0; i < 3; ++i) {
        frameCB_.skyZenith[i]  = std::pow(std::fmax(dome.zenith[i], 0.0f), 1.0f / 2.2f);
        frameCB_.skyHorizon[i] = std::pow(std::fmax(dome.horizon[i], 0.0f), 1.0f / 2.2f);
        frameCB_.lightColor[i] =
            std::pow(std::fmax(e0[i] * dome.sunTransmittance[i] / std::fmax(s.sunIntensity, 1e-6f), 0.0f),
                     1.0f / 2.2f);
    }
    frameCB_.skyParams[0] = dome.exponent;

    // averFogInscatterRef's answer, baked here once per frame instead of marched per pixel -- see
    // that function's own comment in RHIShaders.cpp for why this is exact, not an approximation.
    // Reads frameCB_.camPos and frameCB_.atmoPlanet, both already written for THIS frame: setCamera
    // runs before setSkyAtmosphere in every caller (GameApp::pushFrame, SandboxApp's own per-frame
    // setup), and atmoPlanet[2] (the world-to-km factor) was set a few lines up in this same
    // function, so neither is a frame stale.
    const f32 altKm = std::fmax(frameCB_.camPos[2] * frameCB_.atmoPlanet[2], 1e-3f);
    f32 fogRef[3];
    atmoFogInscatterRef(fit, altKm, s.sunDirection, e0, sunRadius, fogRef);
    for (int i = 0; i < 3; ++i) frameCB_.fogInscatterRef[i] = fogRef[i];
    frameCB_.fogInscatterRef[3] = 0.0f;

    // The sky as nine coefficients, for the AMBIENT term. Same argument as the fog reference
    // directly above -- no view direction and no world position enters it, so every pixel that
    // wants the sky's irradiance wants the same nine numbers.
    AtmosphereSkySH sh{};
    atmoSkyRadianceSH(fit, altKm, s.sunDirection, e0, sunRadius, sh);
    for (int k = 0; k < 9; ++k) {
        for (int i = 0; i < 3; ++i) frameCB_.skySh[k][i] = sh.c[k][i];
        frameCB_.skySh[k][3] = 0.0f;
    }
}

// Builds the post chain's root signature, PSOs and constant ring. Size-independent, so built once.
bool D3D12Device::createPostPipelines() {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;              // t0 scene, t1 bloom, t2 exposure
    srvRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2;              // u0 histogram, u1 exposure
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "post root signature")) return false;
    if (!hrOk(device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&postRootSig_)), "post root signature")) return false;

    const char* src = postShaderSource();
    ComPtr<ID3DBlob> vs;
    if (FAILED(shaderCompiler().compile(src, "PostVS", "vs_5_1", &vs))) return false;

    auto makeGfx = [&](const char* entry, DXGI_FORMAT rtFormat, bool additive, const char* defines,
                       ComPtr<ID3D12PipelineState>& out) {
        ComPtr<ID3DBlob> ps;
        if (FAILED(shaderCompiler().compile(src, entry, "ps_5_1", &ps, nullptr, defines))) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = postRootSig_.Get();
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (additive) {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ONE;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rtFormat;
        d.SampleDesc.Count = 1;
        return hrOk(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "post pso");
    };

    if (!makeGfx("PSBloomPrefilter", kSceneColorFormat, false, nullptr, bloomPrefilterPso_)) return false;
    if (!makeGfx("PSBloomDown",      kSceneColorFormat, false, nullptr, bloomDownPso_)) return false;
    if (!makeGfx("PSBloomUp",        kSceneColorFormat, true,  nullptr, bloomUpPso_)) return false;

    for (int bloom = 0; bloom < 2; ++bloom) {
        for (int autoExp = 0; autoExp < 2; ++autoExp) {
            std::string defs;
            if (bloom)   defs += "AVER_POST_BLOOM=1;";
            if (autoExp) defs += "AVER_POST_AUTOEXPOSURE=1;";
            if (!makeGfx("PSComposite", kBackbufferFormat, false,
                         defs.empty() ? nullptr : defs.c_str(), compositePso_[bloom][autoExp]))
                return false;
        }
    }

    auto makeCompute = [&](const char* entry, ComPtr<ID3D12PipelineState>& out) {
        ComPtr<ID3DBlob> cs;
        if (FAILED(shaderCompiler().compile(src, entry, "cs_5_1", &cs))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = postRootSig_.Get();
        d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        return hrOk(device_->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), "post compute pso");
    };
    if (!makeCompute("CSHistogram", histogramPso_)) return false;
    if (!makeCompute("CSExposure", exposurePso_)) return false;

    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = bufferDesc(kPostConstantRingBytes);
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&postCBs_[i])), "post constant ring")) return false;
        D3D12_RANGE none{0, 0};
        postCBs_[i]->Map(0, &none, reinterpret_cast<void**>(&postCBPtr_[i]));
    }

    // Created COMMON: D3D12 ignores any other buffer state here and warns (#1328).
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto hd = bufferDesc(256 * sizeof(u32));
    hd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &hd,
              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&histBuf_)), "post histogram")) return false;
    auto ed = bufferDesc(2 * sizeof(u32));
    ed.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &ed,
              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&expBuf_)), "post exposure")) return false;

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.NumDescriptors = kPostDescriptorCount;
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!hrOk(device_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&postSrvHeap_)), "post SRV heap")) return false;
    postSrvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.NumDescriptors = kMaxBloomMips;
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!hrOk(device_->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&postRtvHeap_)), "post RTV heap")) return false;
    postRtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

// GPU handle of descriptor triple `triple` in the post heap.
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::postTriple(u32 triple) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = postSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(triple) * 3 * postSrvSize_;
    return h;
}
// CPU handle of descriptor triple `triple` in the post heap.
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::postTripleCpu(u32 triple) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = postSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(triple) * 3 * postSrvSize_;
    return h;
}

// Drops the post chain's size-dependent targets.
void D3D12Device::releasePostTargets() {
    sceneResolved_.Reset();
    bloomTex_.Reset();
    // FACTORY HANDLES, SO destroyTexture -- NOT .Reset(). These two are the only targets in this
    // function created through the resource factory; the ComPtrs above own their resources directly
    // and a Reset is the whole of their teardown. Treating a handle the same way would leak the
    // factory's row and its descriptors every resize, which is exactly often enough to matter.
    if (D3D12ResourceFactory* f = rhiFactory_) {
        if (sceneColorTex_) { f->destroyTexture(sceneColorTex_); sceneColorTex_ = 0; }
        if (presentHdrTex_) { f->destroyTexture(presentHdrTex_); presentHdrTex_ = 0; }
    }
    sceneColorTexW_ = sceneColorTexH_ = presentHdrTexW_ = presentHdrTexH_ = 0;
    bloomMips_ = bloomW_ = bloomH_ = 0;
    postReady_ = false;
}

// The size-dependent half: the resolve destination, the bloom pyramid, and every descriptor that
// points at either. Rebuilt on resize and on a sample-count change; the pipelines above survive both.
bool D3D12Device::createPostTargets() {
    releasePostTargets();
    if (!postRootSig_ || sceneWidth_ == 0 || sceneHeight_ == 0) return false;

    if (sampleCount_ > 1) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = sceneWidth_; td.Height = sceneHeight_;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = kSceneColorFormat; td.SampleDesc.Count = 1;
        auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&sceneResolved_)), "scene resolve target"))
            return false;
    }

    // Bloom tracks the SCENE's own resolution, not the present one -- it samples straight off the
    // (possibly downscaled) scene target, same as the resolve destination above.
    bloomW_ = sceneWidth_ / 2 > 1 ? sceneWidth_ / 2 : 1;
    bloomH_ = sceneHeight_ / 2 > 1 ? sceneHeight_ / 2 : 1;
    bloomMips_ = 1;
    while (bloomMips_ < kMaxBloomMips &&
           (bloomW_ >> bloomMips_) >= 8 && (bloomH_ >> bloomMips_) >= 8) ++bloomMips_;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bd.Width = bloomW_; bd.Height = bloomH_;
    bd.DepthOrArraySize = 1; bd.MipLevels = static_cast<UINT16>(bloomMips_);
    bd.Format = kSceneColorFormat; bd.SampleDesc.Count = 1;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&bloomTex_)), "bloom pyramid"))
        return false;
    for (u32 m = 0; m < kMaxBloomMips; ++m) bloomState_[m] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // ---- descriptors, all of them, once ----
    ID3D12Resource* scene = sceneResolved_ ? sceneResolved_.Get() : msaaColor_.Get();

    D3D12_SHADER_RESOURCE_VIEW_DESC tex{};
    tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tex.Format = kSceneColorFormat;
    tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    tex.Texture2D.MipLevels = 1;

    auto writeTriple = [&](u32 triple, ID3D12Resource* t0, u32 mip0,
                           ID3D12Resource* t1, u32 mip1, bool expAtT2) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = postTripleCpu(triple);
        tex.Texture2D.MostDetailedMip = mip0;
        device_->CreateShaderResourceView(t0, &tex, h);
        h.ptr += postSrvSize_;
        tex.Texture2D.MostDetailedMip = mip1;
        device_->CreateShaderResourceView(t1, &tex, h);
        h.ptr += postSrvSize_;
        if (expAtT2) {
            D3D12_SHADER_RESOURCE_VIEW_DESC bv{};
            bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            bv.Format = DXGI_FORMAT_R32_TYPELESS;
            bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            bv.Buffer.NumElements = 2;
            bv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            device_->CreateShaderResourceView(expBuf_.Get(), &bv, h);
        } else {
            tex.Texture2D.MostDetailedMip = mip1;
            device_->CreateShaderResourceView(t1, &tex, h);
        }
    };

    writeTriple(kPostTriplePrefilter, scene, 0, scene, 0, true);
    writeTriple(kPostTripleHistogram, scene, 0, scene, 0, false);
    writeTriple(kPostTripleComposite, scene, 0, bloomTex_.Get(), 0, true);

    // ---- AverSR's two intermediates, and the composite triple that reads the upscaled one ----
    //
    // GUARDED ON upscaler_, ALL OF IT. Creating these unconditionally would cost every Off build two
    // HDR textures it never samples; writing the triple unconditionally would be worse -- it would
    // hand rhiFactory_->texture(0) to a writeTriple that dereferences it, and crash createPostTargets
    // on the DEFAULT path. (That exact null-deref was in the design this follows; an adversarial
    // review caught it before it was written.)
    if (upscaler_) {
        if (D3D12ResourceFactory* f = rhiFactory_) {
            // #1: a factory-created ALIAS of the scene colour. `scene` above is a raw ComPtr from
            // CreateCommittedResource, so it has no TextureHandle, and IUpscaler::execute needs one.
            // SRV only -- nothing draws into it, a CopyResource fills it each frame.
            TextureDesc sc;
            sc.width = sceneWidth_; sc.height = sceneHeight_;
            sc.format = fromDxgiFormat(kSceneColorFormat);
            sc.bind = ResourceBind::ShaderResource;
            sc.initialState = ResourceState::ShaderResource;
            sc.debugName = "AverSR.SceneColor";
            sceneColorTex_ = f->createTexture(sc);
            sceneColorTexW_ = sceneWidth_; sceneColorTexH_ = sceneHeight_;

            // #2: AverSR's output. HDR and PRESENT-sized -- the upscale runs on radiance, before the
            // tonemap, so PSComposite afterwards sees an image already at the right size and its own
            // resample degenerates to 1:1. That is what lets the composite shader stay untouched.
            TextureDesc ph;
            ph.width = width_; ph.height = height_;
            ph.format = fromDxgiFormat(kSceneColorFormat);
            ph.bind = ResourceBind::RenderTarget | ResourceBind::ShaderResource;
            ph.initialState = ResourceState::ShaderResource;
            ph.hasClearValue = true;
            ph.debugName = "AverSR.PresentHdr";
            presentHdrTex_ = f->createTexture(ph);
            presentHdrTexW_ = width_; presentHdrTexH_ = height_;

            RhiTexture* phr = presentHdrTex_ ? f->texture(presentHdrTex_) : nullptr;
            if (!sceneColorTex_ || !phr || !phr->res) {
                AVER_WARN("[RHI.D3D12] AverSR targets could not be created; upscaling stays off this resize");
                if (sceneColorTex_) { f->destroyTexture(sceneColorTex_); sceneColorTex_ = 0; }
                if (presentHdrTex_) { f->destroyTexture(presentHdrTex_); presentHdrTex_ = 0; }
            } else {
                writeTriple(kPostTripleCompositeUpscaled, phr->res.Get(), 0, bloomTex_.Get(), 0, true);
            }
        }
    }
    for (u32 m = 1; m < bloomMips_; ++m) {
        writeTriple(kPostTripleDownBase + (m - 1), bloomTex_.Get(), m - 1, bloomTex_.Get(), m - 1, false);
        writeTriple(kPostTripleUpBase   + (m - 1), bloomTex_.Get(), m,     bloomTex_.Get(), m,     false);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE uav = postSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    uav.ptr += static_cast<SIZE_T>(kPostTripleCount) * 3 * postSrvSize_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ud.Buffer.NumElements = 256;
    device_->CreateUnorderedAccessView(histBuf_.Get(), nullptr, &ud, uav);
    uav.ptr += postSrvSize_;
    ud.Buffer.NumElements = 2;
    device_->CreateUnorderedAccessView(expBuf_.Get(), nullptr, &ud, uav);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 m = 0; m < bloomMips_; ++m) {
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = kSceneColorFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = m;
        device_->CreateRenderTargetView(bloomTex_.Get(), &rd, rtv);
        rtv.ptr += postRtvSize_;
    }

    postReady_ = true;
    return true;
}

// Suballocates one pass's constants from this frame's post ring.
D3D12_GPU_VIRTUAL_ADDRESS D3D12Device::postConstants(const void* data, u32 bytes) {
    const u32 f = frameIndex_ < kFrameCount ? frameIndex_ : 0;
    if (!postCBPtr_[f]) return 0;
    const u32 offset = (postCBUsed_ + 255u) & ~255u;
    const u32 size = (bytes + 255u) & ~255u;
    if (offset + size > kPostConstantRingBytes) {
        AVER_ERROR("[RHI.D3D12] post constant ring exhausted ({} bytes per frame)", kPostConstantRingBytes);
        return 0;
    }
    std::memcpy(postCBPtr_[f] + offset, data, bytes);
    postCBUsed_ = offset + size;
    return postCBs_[f]->GetGPUVirtualAddress() + offset;
}

// Scene -> backbuffer. Records the whole chain and leaves the backbuffer in RENDER_TARGET.
// A stage that would be a no-op is skipped rather than run with a zero weight.
void D3D12Device::runPostChain(ID3D12Resource* bb) {
    {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (lastFrameTick_ != 0 && freq.QuadPart > 0) {
            const f64 dt = static_cast<f64>(now.QuadPart - lastFrameTick_) / static_cast<f64>(freq.QuadPart);
            frameSeconds_ = static_cast<f32>(dt < 1e-4 ? 1e-4 : (dt > 0.25 ? 0.25 : dt));
        }
        lastFrameTick_ = now.QuadPart;
    }

    const bool msaa = sampleCount_ > 1;
    if (!postReady_ && !createPostTargets()) {
        static bool said = false;
        if (!said) { AVER_ERROR("[RHI.D3D12] the post chain is unavailable; the scene cannot be presented"); said = true; }
        auto toRt = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
        return;
    }

    const bool bloom = post_.bloomIntensity > 0.0f && bloomTex_;
    const bool autoExp = post_.autoExposure && caps_.computeShaders;
    ID3D12Resource* scene = msaa ? sceneResolved_.Get() : msaaColor_.Get();

    if (!expSeeded_) {
        const u32 zeros[258] = {};
        const D3D12_GPU_VIRTUAL_ADDRESS va = postConstants(zeros, sizeof zeros);
        if (va) {
            const u32 f = frameIndex_ < kFrameCount ? frameIndex_ : 0;
            const UINT64 off = va - postCBs_[f]->GetGPUVirtualAddress();
            D3D12_RESOURCE_BARRIER toCopy[2] = {
                transition(histBuf_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                transition(expBuf_.Get(),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cmdList_->ResourceBarrier(2, toCopy);
            cmdList_->CopyBufferRegion(histBuf_.Get(), 0, postCBs_[f].Get(), off, 256 * sizeof(u32));
            cmdList_->CopyBufferRegion(expBuf_.Get(), 0, postCBs_[f].Get(), off, 2 * sizeof(u32));
            D3D12_RESOURCE_BARRIER back[2] = {
                transition(histBuf_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                transition(expBuf_.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cmdList_->ResourceBarrier(2, back);
            expSeeded_ = true;
        }
    }

    // ---- resolve ----
    if (msaa) {
        D3D12_RESOURCE_BARRIER pre[1] = {
            transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
        };
        cmdList_->ResourceBarrier(1, pre);
        cmdList_->ResolveSubresource(scene, 0, msaaColor_.Get(), 0, kSceneColorFormat);
        auto toSrv = transition(scene, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &toSrv);
    } else {
        auto toSrv = transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &toSrv);
    }

    ID3D12DescriptorHeap* heaps[] = {postSrvHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    boundHeap_ = postSrvHeap_.Get();
    cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
    boundRootSig_ = nullptr;
    boundPso_ = nullptr;
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList_->IASetVertexBuffers(0, 0, nullptr);

    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = postSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    uavTable.ptr += static_cast<UINT64>(kPostTripleCount) * 3 * postSrvSize_;

    PostCB cb{};
    auto fillCommon = [&](u32 dstW, u32 dstH, u32 srcW, u32 srcH) {
        cb.tone[0] = post_.exposure;       cb.tone[1] = post_.bloomIntensity;
        cb.tone[2] = post_.bloomThreshold; cb.tone[3] = post_.bloomKnee;
        cb.dst[0] = static_cast<f32>(dstW); cb.dst[1] = static_cast<f32>(dstH);
        cb.dst[2] = 1.0f / cb.dst[0];       cb.dst[3] = 1.0f / cb.dst[1];
        cb.src[0] = static_cast<f32>(srcW); cb.src[1] = static_cast<f32>(srcH);
        cb.src[2] = 1.0f / cb.src[0];       cb.src[3] = 1.0f / cb.src[1];
        cb.adapt[0] = kHistogramMinLogLum;
        cb.adapt[1] = 1.0f / (kHistogramMaxLogLum - kHistogramMinLogLum);
        cb.adapt[2] = 1.0f - std::exp(-post_.exposureSpeed * frameSeconds_);
        cb.adapt[3] = 0.0f;
        cb.limit[0] = post_.exposureMin;         cb.limit[1] = post_.exposureMax;
        cb.limit[2] = post_.histogramLowPercent; cb.limit[3] = post_.histogramHighPercent;
        cb.misc[0] = post_.exposureKey;
        cb.misc[1] = autoExp ? 1.0f : 0.0f;
        cb.misc[2] = 1.0f;
        cb.misc[3] = 0.0f;
    };

    auto fullscreen = [&](ID3D12PipelineState* pso, u32 triple, u32 w, u32 h,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* rtv) {
        cmdList_->SetPipelineState(pso);
        cmdList_->OMSetRenderTargets(1, rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &sc);
        cmdList_->SetGraphicsRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetGraphicsRootDescriptorTable(1, postTriple(triple));
        cmdList_->SetGraphicsRootDescriptorTable(2, uavTable);
        cmdList_->DrawInstanced(3, 1, 0, 0);
    };

    auto bloomRtv = [&](u32 mip) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(mip) * postRtvSize_;
        return h;
    };
    auto bloomTo = [&](u32 mip, D3D12_RESOURCE_STATES to) {
        if (bloomState_[mip] == to) return;
        D3D12_RESOURCE_BARRIER b = transition(bloomTex_.Get(), bloomState_[mip], to);
        b.Transition.Subresource = mip;
        cmdList_->ResourceBarrier(1, &b);
        bloomState_[mip] = to;
    };
    auto mipW = [&](u32 m) { return bloomW_ >> m ? bloomW_ >> m : 1u; };
    auto mipH = [&](u32 m) { return bloomH_ >> m ? bloomH_ >> m : 1u; };

    // ---- eye adaptation ----
    // hw/hh (the downscaled dispatch grid) and the src dims below both derive from the SCENE target
    // this samples -- sceneWidth_/sceneHeight_, not the present width_/height_.
    if (autoExp) {
        const u32 hw = sceneWidth_ / kHistogramDownscale > 1 ? sceneWidth_ / kHistogramDownscale : 1;
        const u32 hh = sceneHeight_ / kHistogramDownscale > 1 ? sceneHeight_ / kHistogramDownscale : 1;
        fillCommon(hw, hh, sceneWidth_, sceneHeight_);

        cmdList_->SetComputeRootSignature(postRootSig_.Get());
        cmdList_->SetPipelineState(histogramPso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch((hw + 15) / 16, (hh + 15) / 16, 1);

        D3D12_RESOURCE_BARRIER uavB{};
        uavB.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavB.UAV.pResource = histBuf_.Get();
        cmdList_->ResourceBarrier(1, &uavB);

        cmdList_->SetPipelineState(exposurePso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch(1, 1, 1);

        cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
    }

    {
        auto expToSrv = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &expToSrv);
    }

    // ---- bloom ----
    if (bloom) {
        bloomTo(0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Prefilter's source is the scene target, not the present size.
        fillCommon(mipW(0), mipH(0), sceneWidth_, sceneHeight_);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = bloomRtv(0);
        fullscreen(bloomPrefilterPso_.Get(), kPostTriplePrefilter, mipW(0), mipH(0), &rtv);

        for (u32 m = 1; m < bloomMips_; ++m) {
            bloomTo(m - 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            bloomTo(m, D3D12_RESOURCE_STATE_RENDER_TARGET);
            fillCommon(mipW(m), mipH(m), mipW(m - 1), mipH(m - 1));
            rtv = bloomRtv(m);
            fullscreen(bloomDownPso_.Get(), kPostTripleDownBase + (m - 1), mipW(m), mipH(m), &rtv);
        }
        for (u32 m = bloomMips_; m-- > 1;) {
            bloomTo(m, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            bloomTo(m - 1, D3D12_RESOURCE_STATE_RENDER_TARGET);
            fillCommon(mipW(m - 1), mipH(m - 1), mipW(m), mipH(m));
            rtv = bloomRtv(m - 1);
            fullscreen(bloomUpPso_.Get(), kPostTripleUpBase + (m - 1), mipW(m - 1), mipH(m - 1), &rtv);
        }
        bloomTo(0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ---- AverSR: Pass A, the upscale (HDR, before the tonemap) ----
    //
    // ONLY WHEN AN UPSCALER IS SET. With none -- the default, and quality Off -- not a line of this
    // runs and the composite below takes exactly the path it always did. That is the whole of
    // docs/AVERSR.md's "Off must be bit-identical" invariant: one branch, on one pointer.
    //
    // BEFORE THE TONEMAP, deliberately. The composite fuses resize + exposure + bloom + ACES + gamma
    // into one pass, so an upscaler cannot simply replace it. Running the resample on scene RADIANCE
    // and handing the composite an image that is ALREADY present-sized leaves that shader and its
    // pipeline completely untouched -- its own resample just degenerates to 1:1.
    bool srUpscaled = false;
    if (upscaler_ && sceneColorTex_ && presentHdrTex_ && rhiFactory_ && rhiContext_) {
        RhiTexture* srcT = rhiFactory_->texture(sceneColorTex_);
        RhiTexture* dstT = rhiFactory_->texture(presentHdrTex_);
        if (srcT && srcT->res && dstT && dstT->res && dstT->rtvHeap) {
            // The copy that exists only because `scene` has no TextureHandle. `scene` is the COPY
            // SOURCE, so it transitions to COPY_SOURCE -- not CopyDest, which is the destination's
            // state and would be a validation error and a wrong barrier.
            D3D12_RESOURCE_BARRIER pre[2] = {
                transition(scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(srcT->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cmdList_->ResourceBarrier(2, pre);
            cmdList_->CopyResource(srcT->res.Get(), scene);
            D3D12_RESOURCE_BARRIER post[2] = {
                transition(scene, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                transition(srcT->res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            cmdList_->ResourceBarrier(2, post);

            auto toRt = transition(dstT->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRt);
            D3D12_CPU_DESCRIPTOR_HANDLE srRtv = dstT->rtvHeap->GetCPUDescriptorHandleForHeapStart();
            cmdList_->OMSetRenderTargets(1, &srRtv, FALSE, nullptr);
            // execute()'s documented contract: the CALLER binds the target and sets viewport and
            // scissor to the destination size; the implementation records only its own pipeline and
            // draw, and transitions nothing.
            D3D12_VIEWPORT srVp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
            D3D12_RECT srSc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
            cmdList_->RSSetViewports(1, &srVp);
            cmdList_->RSSetScissorRects(1, &srSc);

            UpscalerInput in{};
            in.color = sceneColorTex_;
            in.srcWidth = sceneWidth_;   in.srcHeight = sceneHeight_;
            in.dstWidth = width_;        in.dstHeight = height_;
            upscaler_->execute(*rhiContext_, in, presentHdrTex_);

            auto backToSrv = transition(dstT->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);

            // execute() bound its own descriptor heap and root signature. The composite below is
            // recorded straight after and assumes the post chain's -- restore both, or it draws with
            // whatever the upscaler happened to leave bound.
            ID3D12DescriptorHeap* heaps[] = {postSrvHeap_.Get()};
            cmdList_->SetDescriptorHeaps(1, heaps);
            cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
            srUpscaled = true;

            // ONCE, and it reports what actually happened rather than what was configured. The
            // difference matters: this feature spent its whole life "constructed and correct and
            // never called", and a log that fires on the SETTING would have said it was working
            // the entire time.
            if (!srLogged_) {
                srLogged_ = true;
                AVER_INFO("[AverSR] '{}' upscaling {}x{} -> {}x{} in HDR, before the tonemap",
                          upscaler_->name(), sceneWidth_, sceneHeight_, width_, height_);
            }
        }
    }

    // ---- composite ----
    // dst is present-space (the backbuffer or the viewport texture, both width_/height_); src is the
    // scene target this upscales (or 1:1 samples, at the default renderScale_ == 1.0) from --
    // sceneWidth_/sceneHeight_. This IS the actual render-scale upscale: the composite pixel shader
    // already samples by normalized UV through a bilinear sampler (gPostSamp), so the only change
    // needed here is telling it the source is a different size than the destination.
    //
    // ON THE AverSR PATH the source is already present-sized, so src == dst and the shader's own
    // bilinear stretch does nothing -- AverSR's filter produced those pixels, not gPostSamp's.
    if (srUpscaled) fillCommon(width_, height_, width_, height_);
    else            fillCommon(width_, height_, sceneWidth_, sceneHeight_);
    const u32 compositeTriple = srUpscaled ? kPostTripleCompositeUpscaled : kPostTripleComposite;
    {
        auto toRt = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;

        const RhiTexture* vt = (viewportToTex_ && ensureViewportTexture() && rhiFactory_)
                             ? rhiFactory_->texture(viewportTex_) : nullptr;
        if (vt && vt->rtvHeap) {
            const f32 blank[4] = {clear_[0], clear_[1], clear_[2], 1.0f};
            cmdList_->ClearRenderTargetView(bbRtv, blank, 0, nullptr);

            auto toRtTex = transition(vt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRtTex);
            D3D12_CPU_DESCRIPTOR_HANDLE trtv = vt->rtvHeap->GetCPUDescriptorHandleForHeapStart();
            fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0].Get(), compositeTriple,
                       width_, height_, &trtv);
            auto backToSrv = transition(vt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);
            cmdList_->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        } else {
            fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0].Get(), compositeTriple,
                       width_, height_, &bbRtv);
        }
    }

    // ---- restore ----
    {
        auto expBack = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList_->ResourceBarrier(1, &expBack);
    }
    auto sceneBack = msaa
        ? transition(scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST)
        : transition(scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList_->ResourceBarrier(1, &sceneBack);
    if (msaa) {
        auto msaaBack = transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &msaaBack);
    }
}

// Closes the frame: post chain, overlay features, UI, capture, then submit.
void D3D12Device::endFrame() {
    if (!hasSwapchain_) return;
    // Closes beginFrame's "scene draw" and opens the one covering everything after it: the deferred
    // sky, the post chain, the composite/tonemap, the editor's viewport blit, the overlay and ImGui.
    endGpuSpan();
    beginGpuSpan("sky+post+ui");
    fovValid_ = false;   // the post chain sets pipelines on the command list directly
    ID3D12Resource* bb = renderTargets_[frameIndex_].Get();

    // Same scene-space rect (and fallback to the scene target, not the present one) as beginFrame --
    // shared by the transparent pass and the sky draw just below, both of which land on the SAME
    // still-bound scene colour/depth targets and so want the identical viewport and scissor.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    D3D12_VIEWPORT sceneVp{rx, ry, rw, rh, 0.0f, 1.0f};
    D3D12_RECT sceneSc{static_cast<LONG>(rx), static_cast<LONG>(ry), static_cast<LONG>(rx + rw), static_cast<LONG>(ry + rh)};

    // ---- THE DEFERRED SKY DRAW, NOW BEFORE THE TRANSPARENT PASS ----
    //
    // REORDERED FROM THIS PASS'S ORIGINAL SHAPE (particles DECIDED 4's own investigation found this:
    // a --particle-test emitter placed anywhere that was NOT also over an opaque occluder rendered
    // nothing at all -- confirmed with a CPU-side reprojection of its own particle positions into NDC,
    // landing well inside [-1,1], and STILL invisible, which is what actually forced this file open).
    // The ORIGINAL comment here reasoned that running the sky first would mean "a particle drawn over
    // it would be blending onto sky that can never be un-drawn" and called that worse than testing
    // against real depth -- but blending a translucent particle over the sky IS the correct picture
    // for a particle in open air, and the ordering that comment defended has the opposite defect: sky
    // draws OPAQUE (BlendState left at its D3D12 default, BlendEnable=FALSE -- see skyPso_'s own
    // creation site), so with the transparent pass running FIRST and depth-WRITE off (still correct;
    // see that pipeline contract's own comment below), every pixel a particle touched with no opaque
    // occluder behind it was LEFT AT THE CLEAR DEPTH (1.0) by design -- and the sky's own DepthFunc
    // EQUAL test (skyPso_'s own comment) then matched that exact pixel and overwrote the particle's
    // blended colour with a flat, un-blended sky sample. A smoke column, falling snow or a waterfall's
    // mist -- every expressiveness-test case this module's own report checked ANALYTICALLY rather than
    // by rendering -- would have been invisible against open sky the moment it shipped, and nothing in
    // slice 1-3's own proof caught it because every screenshot through slice 3 happened to place its
    // test particles directly in front of an opaque cube, where real depth was always present.
    //
    // WHY THIS ORDER FIXES IT WITHOUT REOPENING THE PROBLEM THE OLD COMMENT WAS SOLVING. Sky still
    // draws ONLY where depth is still at the clear value (its own EQUAL test, unchanged) -- since
    // nothing has touched the depth buffer between beginFrame and here except opaque geometry, sky
    // fills in exactly the same set of pixels it always did, REGARDLESS of this reorder. The
    // transparent pass then draws AFTER it: for a pixel with a real occluder, the depth test still
    // rejects a particle that is further away, exactly as before (occlusion is untouched by this
    // change -- see the pipeline contract two paragraphs down). For a pixel with NO occluder, the sky
    // has already painted a real colour there, so a particle's blend now composites onto that colour
    // instead of onto whatever the opaque pass's own background happened to leave, and since the sky
    // ALSO never writes depth (DepthWriteMask ZERO), the particle's own depth test still runs against
    // the same clear value it always did and still passes. Nothing about occlusion, sort order, or the
    // depth-write-off contract changes; only what colour a particle blends ONTO in open sky does.
    // NOT sceneSuppressed_. Ray-driven primary visibility suppresses the scene without owning the
    // frame, and it writes depth 1.0 on a miss -- which is exactly what this pass's DepthFunc=EQUAL
    // is looking for, so the sky fills the missed pixels and leaves every hit alone. Testing the
    // wrong flag here is what left that mode with no clouds, no atmosphere and no sun.
    if (skyEnabled_ && !frameSuppressed_) {
        cmdList_->RSSetViewports(1, &sceneVp);
        cmdList_->RSSetScissorRects(1, &sceneSc);
        bindGraphicsRoot(rootSig_.Get());
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList_->SetPipelineState(skyPso_.Get());
        boundPso_ = skyPso_.Get();
        cmdList_->IASetVertexBuffers(0, 0, nullptr);
        cmdList_->DrawInstanced(3, 1, 0, 0);
    }

    // ---- THE TRANSPARENT PASS ----
    // IRenderFeature::transparentPass, called here: every opaque drawMesh call this frame, AND the
    // deferred sky draw just above, have already happened, so the depth buffer already holds every
    // real occluder's depth and the render target already holds real colour everywhere (an occluder's
    // own shading, or the sky) -- nothing in this scene is still "unpainted" by the time this runs, so
    // a particle drawn here always blends onto something real rather than a background colour the sky
    // would otherwise have overwritten it with (see the sky draw's own comment just above for the
    // failure mode that used to cause, and this reorder's fix). The loop costs one virtual call per
    // registered feature and nothing else -- the default implementation is an empty inline, so a
    // feature that does not override it (every feature this engine ships except particles) leaves the
    // target exactly as the sky pass left it and this frame is bit-for-bit what it always was.
    //
    // DEPTH-TEST ON, DEPTH-WRITE OFF is the pipeline every feature drawing here is expected to build
    // (GraphicsPipelineDesc::depth = {true, false, ...}), and write-OFF is still correct, unchanged by
    // the reorder above: with back-to-front sorting (see the particle module's own comment on why --
    // inter-emitter order is not sorted at all, only within one emitter), the FURTHEST fragment at a
    // pixel draws FIRST. If that first, furthest fragment also wrote depth, every fragment meant to
    // blend UNDER it -- every particle actually further away, drawn later at the same pixel -- would
    // fail the depth test outright and be silently dropped instead of blended: a translucent smoke
    // plume would render as a single opaque-looking slice at its nearest layer. Depth-WRITE stays off;
    // depth-TEST stays on, so a particle behind a wall is still correctly hidden by it, exactly as
    // before this pass moved.
    //
    // Viewport and scissor are preset to the scene rect (the same contract IRenderFeature::overlayPass
    // documents for the backbuffer); the pipeline is NOT preset; see transparentPass's own comment for
    // why a feature drawing here always binds one of its own. Root signature and viewport are re-set
    // explicitly (not trusted to still be whatever the sky draw above left them) for the same reason
    // the sky draw itself re-sets them rather than trusting beginFrame: cheap, idempotent, and correct
    // regardless of what ran in between.
    // frameSuppressed_, matching the sky above: a particle in a ray-driven frame is as real as one in
    // a rastered frame, and the depth contract it relies on is unchanged -- the ray pass writes real
    // SV_DEPTH, so a particle behind a wall is still hidden by that wall.
    if (rhiContext_ && !frameSuppressed_) {
        cmdList_->RSSetViewports(1, &sceneVp);
        cmdList_->RSSetScissorRects(1, &sceneSc);
        bindGraphicsRoot(rootSig_.Get());
        for (IRenderFeature* f : features_) f->transparentPass(*rhiContext_);
    }

    runPostChain(bb);

    // ---- overlay features, on the tonemapped backbuffer ----
    if (rhiContext_ && !features_.empty()) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;

        const RhiTexture* ovt = (viewportToTex_ && viewportTex_ && rhiFactory_)
                              ? rhiFactory_->texture(viewportTex_) : nullptr;
        const bool intoTexture = ovt && ovt->rtvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE overlayRtv = rtv;
        if (intoTexture) {
            auto toRt = transition(ovt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRt);
            overlayRtv = ovt->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        }
        cmdList_->OMSetRenderTargets(1, &overlayRtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &sc);
        for (IRenderFeature* f : features_) f->overlayPass(*rhiContext_, width_, height_);

        if (intoTexture) {
            auto backToSrv = transition(ovt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);
            cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
    }

    // The installed UI backend's own draw, after every overlay feature above and before capture.
    // uiActive_ is false in a build with no UI backend installed -- a game, or an editor tree
    // configured AVER_ENABLE_UI=OFF -- so this is a no-op there, same as the block it replaces always
    // was under #if AVER_WITH_IMGUI, just decided at runtime instead of at compile time.
    if (uiActive_ && uiBackend_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        boundHeap_ = uiBackend_->render(cmdList_.Get());
    }
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
    // Last thing before Close: the frame-end stamp, then resolve every stamp issued this frame into
    // this slice's own region of the readback buffer. ResolveQueryData is a GPU copy -- it does not
    // wait, and nothing reads the destination until beginFrame has fenced on it two frames later.
    endGpuSpan();   // "sky+post+ui"
    tsSliceEnd_[frameIndex_] = gpuStamp();
    if (tsEnabled_ && tsCount_ > 0)
        cmdList_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                   frameIndex_ * kMaxGpuStamps, tsCount_, tsReadback_.Get(),
                                   static_cast<u64>(frameIndex_) * kMaxGpuStamps * sizeof(u64));

    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (infoQueue_) drainDebugMessages();
}

// Presents the frame, signals its fence, and services a pending capture.
void D3D12Device::present() {
    if (!hasSwapchain_) return;
    const bool tearing = !vsync_ && tearingSupported_;
    const UINT interval = vsync_ ? 1u : 0u;
    const UINT flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT pr = swapChain_->Present(tearingSupported_ ? interval : 1u, flags);
    if (FAILED(pr))
        AVER_ERROR("[RHI.D3D12] Present failed 0x{:08X} removed=0x{:08X}", (u32)pr, (u32)device_->GetDeviceRemovedReason());

    queue_->Signal(fence_.Get(), ++nextFence_);
    fenceValues_[frameIndex_] = nextFence_;

    if (captureReq_ && captureBuf_) {
        waitForGpu();
        void* mapped = nullptr;
        if (SUCCEEDED(captureBuf_->Map(0, nullptr, &mapped))) {
            const u32 x = capX_ < width_ ? capX_ : width_ - 1;
            const u32 y = capY_ < height_ ? capY_ : height_ - 1;
            const u8* base = static_cast<const u8*>(mapped);
            const u8* px = base + static_cast<SIZE_T>(y) * captureFp_.Footprint.RowPitch + static_cast<SIZE_T>(x) * 4;
            captured_[0] = px[0] / 255.0f; captured_[1] = px[1] / 255.0f; captured_[2] = px[2] / 255.0f; captured_[3] = px[3] / 255.0f;

            frameImageW_ = width_; frameImageH_ = height_;
            frameImage_.resize(static_cast<usize>(width_) * height_ * 4);
            for (u32 yy = 0; yy < height_; ++yy) {
                std::memcpy(&frameImage_[static_cast<usize>(yy) * width_ * 4],
                            base + static_cast<usize>(yy) * captureFp_.Footprint.RowPitch,
                            static_cast<usize>(width_) * 4);
            }
            captureBuf_->Unmap(0, nullptr);
            captureReady_ = true;
        } else {
            AVER_ERROR("[RHI.D3D12] capture Map FAILED");
        }
        captureReq_ = false;
    }
}

// Resizes the swapchain and every target sized from it.
void D3D12Device::resize(u32 w, u32 h) {
    if (!hasSwapchain_ || w == 0 || h == 0 || (w == width_ && h == height_)) return;
    waitForGpu();
    for (auto& rt : renderTargets_) rt.Reset();
    depthBuffer_.Reset();
    msaaColor_.Reset();
    const UINT scFlags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!hrOk(swapChain_->ResizeBuffers(kFrameCount, w, h, kBackbufferFormat, scFlags), "ResizeBuffers")) {
        createRenderTargetViews();
        createDepthBuffer();
        createMsaaColor();
        return;
    }
    width_ = w; height_ = h;
    computeSceneSize();
    vpX_ = vpY_ = vpW_ = vpH_ = 0;
    for (u32 n = 0; n < kFrameCount; ++n) fenceValues_[n] = 0;
    createRenderTargetViews();
    createDepthBuffer();
    createMsaaColor();
    releasePostTargets();
    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_));
    notifyRenderTargetsChanged();
    AVER_TRACE("[RHI.D3D12] resized to {}x{}", w, h);
}

// Blocks until the fence reaches `value`. Returns false only when the device is gone.
bool D3D12Device::waitFence(u64 value) {
    if (!fence_ || !fenceEvent_) return true;
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) return false;

    for (u32 slice = 0;; ++slice) {
        if (WaitForSingleObject(fenceEvent_, 1000) != WAIT_TIMEOUT) return true;
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            AVER_ERROR("[RHI.D3D12] the device was removed while waiting for the GPU (0x{:08X})",
                       static_cast<u32>(removed));
            return false;
        }
        if (slice == 4)
            AVER_WARN("[RHI.D3D12] still waiting on the GPU after 5 s; the device is healthy, so "
                      "this is a slow frame (WARP, or a very large viewport) and not a hang");
    }
}

// Signals a fresh fence value and waits for the GPU to reach it.
void D3D12Device::waitForGpu() {
    if (!queue_ || !fence_ || !fenceEvent_) return;
    const u64 v = ++nextFence_;
    if (FAILED(queue_->Signal(fence_.Get(), v))) return;
    waitFence(v);
}

// The single live UI backend, for the free-function Win32 message thunk below -- mirrors this
// engine's existing "one process-wide window-message handler" model (registerUiWndProc itself takes
// only one function pointer, no user-data slot) and this file's own former g_uiSrv global before it.
// Set in uiInit, cleared in uiShutdown.
static d3d12::IUiBackend* g_activeUiBackend = nullptr;

// Feeds one window message to the installed UI backend. True when it consumed it.
static bool uiBackendWndProcThunk(void* hwnd, u32 msg, u64 w, i64 l) {
    return g_activeUiBackend && g_activeUiBackend->wndProc(hwnd, msg, w, l);
}

// Brings up the installed UI backend (see UiBackend.hpp) on this device and window. False if none
// was installed (setUiBackend was never called -- every game build, and an editor tree configured
// AVER_ENABLE_UI=OFF) or if it failed to initialise; either way every uiXxx() below then behaves
// exactly as IDevice's own no-op defaults do.
bool D3D12Device::uiInit(void* hwnd) {
    if (uiActive_) return true;
    if (!uiBackend_ || !device_ || !hwnd) return false;

    d3d12::UiBackendInitDesc desc;
    desc.device = device_.Get();
    desc.commandQueue = queue_.Get();
    desc.frameCount = kFrameCount;
    desc.rtvFormat = kBackbufferFormat;
    if (!uiBackend_->init(hwnd, desc)) return false;

    g_activeUiBackend = uiBackend_;
    registerUiWndProc(&uiBackendWndProcThunk);
    uiActive_ = true;
    AVER_INFO("[RHI.D3D12] UI backend initialised");
    return true;
}

// Starts the UI backend's frame.
void D3D12Device::uiNewFrame() {
    if (!uiActive_ || !uiBackend_) return;
    uiBackend_->newFrame();
}

// Tears the UI backend down.
void D3D12Device::uiShutdown() {
    if (!uiActive_ || !uiBackend_) return;
    uiBackend_->shutdown();
    registerUiWndProc(nullptr);
    g_activeUiBackend = nullptr;
    uiActive_ = false;
}

bool D3D12Device::uiWantsMouse() const {
    return uiActive_ && uiBackend_ && uiBackend_->wantsMouse();
}

bool D3D12Device::uiWantsKeyboard() const {
    return uiActive_ && uiBackend_ && uiBackend_->wantsKeyboard();
}

// The shader-visible descriptor the UI draws texture `t` with.
u64 D3D12Device::uiTextureId(TextureHandle t) {
    if (!uiActive_ || !rhiFactory_) return 0;
    return rhiFactory_->uiDescriptor(t);
}

// Clears an 8x8 offscreen to `in` and reads one texel back into `out`.
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

// ================================================================ generic RHI resource factory
// Everything below implements aver::rhi::IResourceFactory / IRenderContext, sharing the device,
// queue, fence, command list and mesh table with the renderer above.

// Drains the GPU, then releases everything the factory still owns.
D3D12ResourceFactory::~D3D12ResourceFactory() {
    dev_->waitForGpu();
    retired_.clear();
}

// Creates the one shader-visible descriptor heap every binding set suballocates from.
bool D3D12ResourceFactory::init() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kRhiHeapSize;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!hrOk(dev_->device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "rhi descriptor heap")) return false;
    heapStride_ = dev_->device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// ---- handle tables. A record whose resource is gone reads as an invalid handle.
RhiTexture* D3D12ResourceFactory::texture(TextureHandle h) {
    if (h == 0 || h > textures_.size()) return nullptr;
    RhiTexture& t = textures_[h - 1];
    return t.res ? &t : nullptr;
}
const RhiTexture* D3D12ResourceFactory::texture(TextureHandle h) const {
    if (h == 0 || h > textures_.size()) return nullptr;
    const RhiTexture& t = textures_[h - 1];
    return t.res ? &t : nullptr;
}
RhiBuffer* D3D12ResourceFactory::buffer(BufferHandle h) {
    if (h == 0 || h > buffers_.size()) return nullptr;
    RhiBuffer& b = buffers_[h - 1];
    return b.res ? &b : nullptr;
}
RhiShader* D3D12ResourceFactory::shader(ShaderHandle h) {
    if (h == 0 || h > shaders_.size()) return nullptr;
    RhiShader& s = shaders_[h - 1];
    return s.blob ? &s : nullptr;
}
RhiPipeline* D3D12ResourceFactory::pipeline(PipelineHandle h) {
    if (h == 0 || h > pipelines_.size()) return nullptr;
    RhiPipeline& p = pipelines_[h - 1];
    return p.pso ? &p : nullptr;
}
RhiBindingSet* D3D12ResourceFactory::bindingSet(BindingSetHandle h) {
    if (h == 0 || h > bindingSets_.size()) return nullptr;
    RhiBindingSet& s = bindingSets_[h - 1];
    return s.alive ? &s : nullptr;
}
RhiBlas* D3D12ResourceFactory::blas(BlasHandle h) {
    if (h == 0 || h > blases_.size()) return nullptr;
    RhiBlas& b = blases_[h - 1];
    return b.as ? &b : nullptr;
}
RhiTlas* D3D12ResourceFactory::tlas(TlasHandle h) {
    if (h == 0 || h > tlases_.size()) return nullptr;
    RhiTlas& t = tlases_[h - 1];
    return t.as ? &t : nullptr;
}

// CPU handle of descriptor slot `index` in the shared heap.
D3D12_CPU_DESCRIPTOR_HANDLE D3D12ResourceFactory::cpuSlot(u32 index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * heapStride_;
    return h;
}
// GPU handle of descriptor slot `index` in the shared heap.
D3D12_GPU_DESCRIPTOR_HANDLE D3D12ResourceFactory::gpuSlot(u32 index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * heapStride_;
    return h;
}

// Writes a null view of the declared dimension into every slot of a set. Tier 1 hardware reads
// undefined data from any descriptor in a bound table that was never written.
//
// PER-SLOT SlotKind IS WHAT MAKES A MIXED TABLE 0 POSSIBLE, and it is worth saying so here because
// the other backend cannot make the same claim. Stage 3's GPU per-cluster path (SandboxApp.cpp's
// ensureLodMeshPipeline) merges Voxi's table-0 union -- a Texture3D, an AccelerationStructure,
// three StructuredBuffers, and four more Texture2Ds -- into ITS OWN table 0 alongside the cluster
// geometry's StructuredBuffers, precisely BECAUSE this loop and setSrv/setUav elsewhere in this file
// already switch on SlotKind per slot rather than assuming one shape for the whole table.
// modules/rhi.vulkan/src/VulkanPipeline.cpp's descriptorLayout() does not: it assumes every table-0
// slot is a Texture2D, which was already wrong for Voxi's own Texture3D/acceleration-structure/
// structured-buffer slots before this change (a PRE-EXISTING defect, not introduced here, and not
// fixed here -- that needs a per-slot SlotKind on PipelineLayout, the same information this loop
// already reads off BindingSetDesc, threaded through to the Vulkan descriptor-set-layout builder).
// So the merge is D3D12 ONLY, and that is a property of the whole design, not a note in one file.
void D3D12ResourceFactory::nullFill(const RhiBindingSet& s) {
    for (u32 i = 0; i < s.srvCount; ++i) {
        const SlotKind kind = s.srvKinds[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (kind == SlotKind::AccelerationStructure) {
            if (dev_->caps_.rayTracingTier == 0) {
                if (!asSlotLogged_) {
                    asSlotLogged_ = true;
                    AVER_INFO("[RHI.D3D12] no ray tracing on this device: acceleration-structure slots "
                              "are filled with a null 2D view and the shaders that would trace decline");
                }
                sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
            } else {
                sv.Format = DXGI_FORMAT_UNKNOWN;
                sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                sv.RaytracingAccelerationStructure.Location = 0;
            }
        } else if (kind == SlotKind::StructuredBuffer) {
            // A null structured-buffer view needs a stride, or Tier 1 validation rejects it.
            sv.Format = DXGI_FORMAT_UNKNOWN;
            sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sv.Buffer.NumElements = 0;
            sv.Buffer.StructureByteStride = 4;
        } else if (kind == SlotKind::Texture3D) {
            sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            sv.Texture3D.MipLevels = 1;
        } else if (kind == SlotKind::Texture2DMS) {
            // D3D12_TEX2DMS_SRV is an empty struct (no mip/level fields to set at all) -- see
            // setSrv's own comment on this dimension.
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
        }
        dev_->device_->CreateShaderResourceView(nullptr, &sv, cpuSlot(s.heapBase + i));
    }

    for (u32 i = 0; i < s.uavCount; ++i) {
        SlotKind kind = s.uavKinds[i];
        if (kind == SlotKind::AccelerationStructure) {
            AVER_ERROR("[RHI.D3D12] binding set UAV slot {} declares AccelerationStructure, which is SRV-only", i);
            kind = SlotKind::Texture2D;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        if (kind == SlotKind::StructuredBuffer) {
            uv.Format = DXGI_FORMAT_UNKNOWN;
            uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uv.Buffer.NumElements = 0;
            uv.Buffer.StructureByteStride = 4;
        } else if (kind == SlotKind::Texture3D) {
            uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uv.Texture3D.WSize = 1;
        } else {
            uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        dev_->device_->CreateUnorderedAccessView(nullptr, nullptr, &uv, cpuSlot(s.heapBase + s.srvCount + i));
    }
}

// Reserves `count` contiguous descriptors: first fit over the free list, then bump-allocation.
bool D3D12ResourceFactory::allocRange(u32 count, u32& outFirst) {
    for (usize i = 0; i < freeRanges_.size(); ++i) {
        if (freeRanges_[i].count < count) continue;
        outFirst = freeRanges_[i].first;
        if (freeRanges_[i].count > count) {
            freeRanges_[i].first += count;
            freeRanges_[i].count -= count;
        } else {
            freeRanges_[i] = freeRanges_.back();
            freeRanges_.pop_back();
        }
        return true;
    }
    if (heapUsed_ + count > kRhiHeapSize) {
        AVER_ERROR("[RHI.D3D12] binding-set heap exhausted: {} of {} slots used, {} more wanted",
                   heapUsed_, kRhiHeapSize, count);
        return false;
    }
    outFirst = heapUsed_;
    heapUsed_ += count;
    return true;
}

// The fence value at which work recorded right now can be considered retired. Taken past the
// completed value too, because createSwapchainResources rewinds the frame counter.
u64 D3D12ResourceFactory::retireFence() const {
    const u64 completed = dev_->fence_ ? dev_->fence_->GetCompletedValue() : 0;
    return (dev_->nextFence_ > completed ? dev_->nextFence_ : completed) + 1;
}

// Queues an object for release once the GPU has passed the current frame.
void D3D12ResourceFactory::retire(ComPtr<IUnknown> obj) {
    if (!obj) return;
    retired_.push_back({std::move(obj), retireFence()});
}

// Releases every retired object and descriptor range whose fence has passed.
void D3D12ResourceFactory::collect() {
    if (!dev_->fence_ || (retired_.empty() && pendingRanges_.empty())) return;
    const u64 done = dev_->fence_->GetCompletedValue();
    for (usize i = 0; i < retired_.size();) {
        if (retired_[i].fence <= done) { retired_[i] = std::move(retired_.back()); retired_.pop_back(); }
        else ++i;
    }
    for (usize i = 0; i < pendingRanges_.size();) {
        if (pendingRanges_[i].fence <= done) {
            freeRanges_.push_back({pendingRanges_[i].first, pendingRanges_[i].count, 0});
            pendingRanges_[i] = pendingRanges_.back();
            pendingRanges_.pop_back();
        } else ++i;
    }
}

// ---- root-signature cache
// Returns the cached root signature for `layout`, building it on first use.
const RootSigEntry* D3D12ResourceFactory::rootSignature(const PipelineLayout& layout, bool mesh, bool instanced) {
    for (const RootSigEntry& e : rootSigs_)
        if (e.mesh == mesh && e.instanced == instanced && sameLayout(e.layout, layout)) return &e;

    RootSigEntry e;
    e.layout = layout;
    e.mesh = mesh;
    e.instanced = instanced;

    D3D12_DESCRIPTOR_RANGE ranges[2 * kBindingTableCount] = {};
    // +3 for the mesh geometry SRVs/count, +1 more for the instanced-draw world-matrix SRV.
    D3D12_ROOT_PARAMETER params[2 * kBindingTableCount + kMaxConstantSlots + 4] = {};
    u32 n = 0;
    const u32 srvCounts[kBindingTableCount] = {layout.srvCount, layout.srvCount1};
    const u32 uavCounts[kBindingTableCount] = {layout.uavCount, layout.uavCount1};
    u32 srvBase = 0, uavBase = 0;
    for (u32 t = 0; t < kBindingTableCount; ++t) {
        if (srvCounts[t]) {
            D3D12_DESCRIPTOR_RANGE& r = ranges[2 * t];
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            r.NumDescriptors = srvCounts[t];
            r.BaseShaderRegister = srvBase;
            params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[n].DescriptorTable.NumDescriptorRanges = 1;
            params[n].DescriptorTable.pDescriptorRanges = &r;
            e.srvParam[t] = static_cast<i32>(n++);
        }
        if (uavCounts[t]) {
            D3D12_DESCRIPTOR_RANGE& r = ranges[2 * t + 1];
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            r.NumDescriptors = uavCounts[t];
            r.BaseShaderRegister = uavBase;
            params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[n].DescriptorTable.NumDescriptorRanges = 1;
            params[n].DescriptorTable.pDescriptorRanges = &r;
            e.uavParam[t] = static_cast<i32>(n++);
        }
        srvBase += srvCounts[t];
        uavBase += uavCounts[t];
    }

    for (u32 s = 0; s < kMaxConstantSlots; ++s) {
        if (layout.constantDwords[s]) {
            params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[n].Constants.ShaderRegister = s;
            params[n].Constants.Num32BitValues = layout.constantDwords[s];
        } else {
            params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[n].Descriptor.ShaderRegister = s;
        }
        e.slotParam[s] = static_cast<i32>(n++);
    }

    if (mesh) {
        // FROZEN: geometry SRVs sit past both declared tables, pinned by RHIResources.hpp and the prelude.
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[n].Descriptor.ShaderRegister = declaredSrvCount(layout);
        e.msVertexParam = static_cast<i32>(n++);
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[n].Descriptor.ShaderRegister = declaredSrvCount(layout) + 1;
        e.msIndexParam = static_cast<i32>(n++);
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[n].Constants.ShaderRegister = kMeshGeometryConstantRegister;
        params[n].Constants.Num32BitValues = 4;
        e.msCountParam = static_cast<i32>(n++);
    }
    if (instanced) {
        // One more root SRV, past declaredSrvCount(layout) AND past the two mesh geometry SRVs
        // above when this is also a mesh pipeline (msVertexParam/msIndexParam already claimed
        // declaredSrvCount and +1 in that case) -- see RHIResources.hpp's comment above
        // GraphicsPipelineDesc::instanced for the register arithmetic a shader compiling against
        // this same layout must reproduce.
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[n].Descriptor.ShaderRegister = declaredSrvCount(layout) + (mesh ? 2 : 0);
        e.instanceWorldParam = static_cast<i32>(n++);
    }
    for (u32 i = 0; i < n; ++i) params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[4] = {};
    const u32 sampCount = layout.samplerCount < 4 ? layout.samplerCount : 4;
    for (u32 i = 0; i < sampCount; ++i) {
        samplers[i].Filter = toFilter(layout.samplers[i].filter);
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = toAddress(layout.samplers[i].address);
        samplers[i].ComparisonFunc = toComparison(layout.samplers[i].compare);
        samplers[i].MaxLOD = layout.samplers[i].maxLod;
        samplers[i].MaxAnisotropy = layout.samplers[i].maxAnisotropy ? layout.samplers[i].maxAnisotropy : 1;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = n;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = sampCount;
    rsd.pStaticSamplers = samplers;
    rsd.Flags = mesh ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                     : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        AVER_ERROR("[RHI.D3D12] rhi root signature: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "serialize failed");
        return nullptr;
    }
    if (!hrOk(dev_->device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
              IID_PPV_ARGS(&e.sig)), "rhi CreateRootSignature")) return nullptr;
    rootSigs_.push_back(std::move(e));
    return &rootSigs_.back();
}

// ---- creation
// Fills a new texture from TextureDesc::initialData on a one-shot command list, and blocks until
// the copy has retired.
bool D3D12ResourceFactory::uploadInitialData(ID3D12Resource* res, const D3D12_RESOURCE_DESC& td,
                                             const TextureDesc& d, u32 mips) {
    ID3D12Device* dev = dev_->device_.Get();
    if (packedRowPitch(d.format, 1) == 0) {
        AVER_ERROR("[RHI.D3D12] createTexture: initial data for a format with no CPU footprint");
        return false;
    }
    const u32 count = d.initialDataCount < mips ? d.initialDataCount : mips;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(count);
    std::vector<UINT>   rows(count);
    std::vector<UINT64> rowBytes(count);
    UINT64 total = 0;
    dev->GetCopyableFootprints(&td, 0, count, 0, fp.data(), rows.data(), rowBytes.data(), &total);

    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto ud = bufferDesc(total);
    ComPtr<ID3D12Resource> staging;
    if (!hrOk(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)),
              "rhi texture staging")) return false;
    setDebugName(staging.Get(), "rhi texture staging");

    u8* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (!hrOk(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "rhi staging Map")) return false;

    for (u32 s = 0; s < count; ++s) {
        const auto* src = static_cast<const u8*>(d.initialData[s]);
        if (!src) continue;
        const u64 srcPitch = (s == 0 && d.initialRowPitch) ? d.initialRowPitch
                                                           : packedRowPitch(d.format, fp[s].Footprint.Width);
        const u64 dstPitch = fp[s].Footprint.RowPitch;
        const u64 bytes    = rowBytes[s] < srcPitch ? rowBytes[s] : srcPitch;
        u8* dst = mapped + fp[s].Offset;
        for (u32 z = 0; z < fp[s].Footprint.Depth; ++z) {
            for (u32 y = 0; y < rows[s]; ++y) {
                const u64 row = u64(z) * rows[s] + y;
                std::memcpy(dst + row * dstPitch, src + row * srcPitch, static_cast<size_t>(bytes));
            }
        }
    }
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!hrOk(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
              "rhi upload alloc")) return false;
    if (!hrOk(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
              IID_PPV_ARGS(&list)), "rhi upload list")) return false;

    for (u32 s = 0; s < count; ++s) {
        if (!d.initialData[s]) continue;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp[s];
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = s;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    const D3D12_RESOURCE_STATES want = toResourceStates(d.initialState);
    if (want != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto b = transition(res, D3D12_RESOURCE_STATE_COPY_DEST, want);
        list->ResourceBarrier(1, &b);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    dev_->queue_->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> f;
    if (!hrOk(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "rhi upload fence")) return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->queue_->Signal(f.Get(), 1);
    if (f->GetCompletedValue() < 1 && ev) { f->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, INFINITE); }
    if (ev) CloseHandle(ev);

    retire(staging);
    collect();
    return true;
}

// Creates a texture with its views and any initial data, and returns its handle.
TextureHandle D3D12ResourceFactory::createTexture(const TextureDesc& d) {
    collect();
    if (d.width == 0 || d.height == 0) { AVER_ERROR("[RHI.D3D12] createTexture with a zero extent"); return 0; }
    const DXGI_FORMAT fmt = toDxgiFormat(d.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) { AVER_ERROR("[RHI.D3D12] createTexture with an unknown format"); return 0; }

    if (isBlockFormat(d.format)) {
        if (any(d.bind, ResourceBind::UnorderedAccess) || any(d.bind, ResourceBind::RenderTarget) ||
            any(d.bind, ResourceBind::DepthStencil)) {
            AVER_ERROR("[RHI.D3D12] createTexture: a block-compressed format is sample-only");
            return 0;
        }
        if ((d.width & 3u) || (d.height & 3u)) {
            AVER_ERROR("[RHI.D3D12] createTexture: block-compressed extents must be multiples of 4 ({}x{})",
                       d.width, d.height);
            return 0;
        }
        if (d.dim == TextureDim::Tex3D) {
            AVER_ERROR("[RHI.D3D12] createTexture: block-compressed volumes are not supported");
            return 0;
        }
    }

    const u32 depth = (d.dim == TextureDim::Tex3D && d.depth) ? d.depth : 1;
    u32 mips = d.mips;
    if (mips == 0) {
        u32 largest = d.width > d.height ? d.width : d.height;
        if (d.dim == TextureDim::Tex3D && depth > largest) largest = depth;
        mips = 1;
        for (u32 e = largest; e > 1; e >>= 1) ++mips;
    }

    D3D12_RESOURCE_DESC td{};
    td.Dimension = (d.dim == TextureDim::Tex3D) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = d.width;
    td.Height = d.height;
    td.DepthOrArraySize = static_cast<UINT16>(depth);
    td.MipLevels = static_cast<UINT16>(mips);
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    if (any(d.bind, ResourceBind::UnorderedAccess)) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (any(d.bind, ResourceBind::RenderTarget))    td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (any(d.bind, ResourceBind::DepthStencil))    td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    if (d.hasClearValue) {
        if (any(d.bind, ResourceBind::DepthStencil)) {
            cv.Format = toDxgiDsvFormat(d.format);
            cv.DepthStencil.Depth = d.clearDepth;
        } else {
            cv.Format = fmt;
            for (u32 i = 0; i < 4; ++i) cv.Color[i] = d.clearColor[i];
        }
    }

    const bool seeded = d.initialData && d.initialDataCount > 0;

    RhiTexture t;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(dev_->device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
              seeded ? D3D12_RESOURCE_STATE_COPY_DEST : toResourceStates(d.initialState),
              d.hasClearValue ? &cv : nullptr,
              IID_PPV_ARGS(&t.res)), "rhi texture")) return 0;
    setDebugName(t.res.Get(), d.debugName);

    if (seeded && !uploadInitialData(t.res.Get(), td, d, mips)) return 0;

    if (any(d.bind, ResourceBind::RenderTarget)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (hrOk(dev_->device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.rtvHeap)), "rhi RTV heap"))
            dev_->device_->CreateRenderTargetView(t.res.Get(), nullptr, t.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    if (any(d.bind, ResourceBind::DepthStencil)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (hrOk(dev_->device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.dsvHeap)), "rhi DSV heap")) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = toDxgiDsvFormat(d.format);
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev_->device_->CreateDepthStencilView(t.res.Get(), &dv, t.dsvHeap->GetCPUDescriptorHandleForHeapStart());
        }
    }

    t.desc = d;
    t.desc.mips = mips;
    t.desc.depth = depth;
    t.desc.debugName = nullptr;
#if AVER_RHI_TRACK_STATE
    if (d.debugName) t.debugName = d.debugName;
    t.states.assign(mips, d.initialState);
#endif
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

// See this method's own declaration (D3D12ResourceFactory class body) for what it is for.
TextureHandle D3D12ResourceFactory::adoptExternalDepthTexture(ID3D12Resource* resource, u32 width, u32 height,
                                                               TextureHandle existing) {
    if (!resource) return 0;
    RhiTexture t;
    t.res = resource;   // ComPtr(T*) AddRefs; ownership is SHARED with dev_->depthBuffer_'s own
                        // ComPtr, not stolen from it -- both go to zero together when the device does.
    t.desc.dim = TextureDim::Tex2D;
    t.desc.width = width; t.desc.height = height; t.desc.depth = 1; t.desc.mips = 1;
    // R32Typeless, not D32Float: this is the SAME "DSV sees D32Float, SRV sees R32Float" alias
    // VoxiRenderer's own shadow map uses (see Format::R32Typeless's own doc comment in
    // RHIResources.hpp), which is exactly why toDxgiSrvFormat below resolves it to R32_FLOAT without
    // this factory needing to know anything backend-specific about depth formats.
    t.desc.format = Format::R32Typeless;
    t.desc.bind = ResourceBind::ShaderResource | ResourceBind::DepthStencil;
    // Matches physical reality at the moment of adoption: createDepthBuffer() leaves the resource in
    // DEPTH_WRITE (its CreateCommittedResource call says so explicitly) and nothing transitions it
    // away from that before the render loop starts using it as a depth target every frame. A caller
    // wanting to READ it (modules/occlusion) is responsible for the DepthWrite <-> NonPixelShaderResource
    // round trip via the ordinary textureBarrier — this factory has no idea when that is safe to do.
    t.desc.initialState = ResourceState::DepthWrite;
    t.desc.debugName = nullptr;
#if AVER_RHI_TRACK_STATE
    t.debugName = "scene depth (adopted)";
    t.states.assign(1, ResourceState::DepthWrite);
#endif
    if (existing) {
        RhiTexture* slot = texture(existing);
        if (slot) { *slot = std::move(t); return existing; }
    }
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

// Creates a buffer, mapping it when it is an upload buffer, and returns its handle.
BufferHandle D3D12ResourceFactory::createBuffer(const BufferDesc& d) {
    collect();
    if (d.bytes == 0) { AVER_ERROR("[RHI.D3D12] createBuffer of zero bytes"); return 0; }

    D3D12_RESOURCE_DESC rd = bufferDesc(d.bytes);
    if (d.allowUnorderedAccess || d.kind == BufferKind::AccelStructure)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const bool upload = d.kind == BufferKind::Upload;
    const bool readback = d.kind == BufferKind::Readback;
    // A readback buffer is created in COPY_DEST and stays there: it is only ever a copy target.
    const D3D12_RESOURCE_STATES state =
        upload   ? D3D12_RESOURCE_STATE_GENERIC_READ
      : readback ? D3D12_RESOURCE_STATE_COPY_DEST
               : (d.kind == BufferKind::AccelStructure ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                                       : D3D12_RESOURCE_STATE_COMMON);
    RhiBuffer b;
    auto hp = heapProps(upload ? D3D12_HEAP_TYPE_UPLOAD
                      : readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(dev_->device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
              IID_PPV_ARGS(&b.res)), "rhi buffer")) return 0;
    setDebugName(b.res.Get(), d.debugName);
    if (upload || readback) {
        D3D12_RANGE none{0, 0};
        b.res->Map(0, &none, reinterpret_cast<void**>(&b.mapped));
    }
    b.desc = d;
    b.desc.debugName = nullptr;
#if AVER_RHI_TRACK_STATE
    if (d.debugName) b.debugName = d.debugName;
    b.state = d.kind == BufferKind::AccelStructure ? ResourceState::AccelerationStructure : ResourceState::Common;
    b.stateFixed = upload || d.kind == BufferKind::AccelStructure;
#endif
    buffers_.push_back(std::move(b));
    return static_cast<BufferHandle>(buffers_.size());
}

namespace {
// FXC target for a stage. Mesh borrows the vertex target, which the SM6 derivation then replaces.
const char* fxcTargetFor(ShaderStage s) {
    switch (s) {
        case ShaderStage::Pixel:    return "ps_5_1";
        case ShaderStage::Geometry: return "gs_5_1";
        case ShaderStage::Compute:  return "cs_5_1";
        case ShaderStage::Mesh:
        case ShaderStage::Amplification:
        case ShaderStage::Vertex:   break;
    }
    return "vs_5_1";
}
// Shader-target prefix for a stage.
const char* stagePrefixFor(ShaderStage s) {
    switch (s) {
        case ShaderStage::Pixel:         return "ps";
        case ShaderStage::Geometry:      return "gs";
        case ShaderStage::Compute:       return "cs";
        case ShaderStage::Mesh:          return "ms";
        case ShaderStage::Amplification: return "as";
        case ShaderStage::Vertex:        break;
    }
    return "vs";
}
} // namespace

// Compiles one shader and returns its handle.
ShaderHandle D3D12ResourceFactory::createShader(const ShaderDesc& d) {
    collect();
    if (!d.source || !d.entry) { AVER_ERROR("[RHI.D3D12] createShader without source or entry point"); return 0; }

    // Mesh and Amplification share one floor: both are D3D12 Ultimate stages, unavailable below
    // Tier 1 mesh-shader hardware regardless of what shader model the device otherwise reports. This
    // is the explicit half of the degrade -- house rule 6 -- the other half being that a PSO built
    // from a shader this rejects simply never gets created, so the caller's existing draw path is
    // untouched. See D3D12Device::initMeshShaders for the same gate applied to the backend's own
    // fixed voxelisation mesh-shader pipeline.
    const bool isMeshFamily = d.stage == ShaderStage::Mesh || d.stage == ShaderStage::Amplification;
    if (isMeshFamily && dev_->caps_.meshShaderTier == 0) {
        AVER_WARN("[RHI.D3D12] createShader '{}' needs mesh-shader hardware, which this device reports as tier 0", d.entry);
        return 0;
    }

    u32 model = d.minShaderModel;
    if (isMeshFamily && model < 65) model = 65;
    if (model > dev_->caps_.shaderModel) {
        AVER_WARN("[RHI.D3D12] createShader '{}' wants SM {} but the device reports {}", d.entry, model, dev_->caps_.shaderModel);
        return 0;
    }
    const bool needsDxc = model > 60 || isMeshFamily;
    if (needsDxc && !dev_->caps_.dxcAvailable) {
        AVER_WARN("[RHI.D3D12] createShader '{}' needs DXC, which is unavailable", d.entry);
        return 0;
    }

    std::string src;
    if (d.prelude) src = d.prelude;
    src += d.source;

    char sm6[16] = {};
    const char* sm6Target = nullptr;
    if (model > 60 || isMeshFamily) {
        std::snprintf(sm6, sizeof(sm6), "%s_%u_%u", stagePrefixFor(d.stage), model / 10, model % 10);
        sm6Target = sm6;
    }

    ComPtr<ID3DBlob> blob;
    if (FAILED(shaderCompiler().compile(src.c_str(), d.entry, fxcTargetFor(d.stage), &blob, sm6Target, d.defines)) || !blob) {
        AVER_ERROR("[RHI.D3D12] createShader '{}' failed to compile", d.entry);
        return 0;
    }
    RhiShader s;
    s.blob = std::move(blob);
    s.stage = d.stage;
    shaders_.push_back(std::move(s));
    return static_cast<ShaderHandle>(shaders_.size());
}

// Creates a graphics pipeline, on either the input-assembler or the mesh-shader path.
PipelineHandle D3D12ResourceFactory::createGraphicsPipeline(const GraphicsPipelineDesc& d) {
    collect();
    if ((d.vs == 0) == (d.ms == 0)) {
        AVER_ERROR("[RHI.D3D12] createGraphicsPipeline needs exactly one of vs / ms");
        return 0;
    }
    if (d.as != 0 && d.ms == 0) {
        AVER_ERROR("[RHI.D3D12] createGraphicsPipeline: an amplification shader (as) needs a mesh shader (ms) alongside it");
        return 0;
    }
    RhiShader* vs = shader(d.vs);
    RhiShader* gs = shader(d.gs);
    RhiShader* ms = shader(d.ms);
    RhiShader* ps = shader(d.ps);
    RhiShader* as = shader(d.as);
    if ((d.vs && !vs) || (d.gs && !gs) || (d.ms && !ms) || (d.ps && !ps) || (d.as && !as)) {
        AVER_ERROR("[RHI.D3D12] createGraphicsPipeline given an invalid shader handle");
        return 0;
    }

    const RootSigEntry* rs = rootSignature(d.layout, d.ms != 0, d.instanced);
    if (!rs) return 0;

    RhiPipeline p;
    p.rootSig = rs->sig.Get();
    p.mesh = d.ms != 0;
    p.amplification = d.as != 0;
    for (u32 t = 0; t < kBindingTableCount; ++t) { p.srvParam[t] = rs->srvParam[t]; p.uavParam[t] = rs->uavParam[t]; }
    p.srvBaseRegister[1] = d.layout.srvCount;
    p.msVertexParam = rs->msVertexParam;
    p.msIndexParam = rs->msIndexParam;
    p.msCountParam = rs->msCountParam;
    p.instanceWorldParam = rs->instanceWorldParam;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) { p.slotParam[i] = rs->slotParam[i]; p.slotDwords[i] = d.layout.constantDwords[i]; }

    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = (d.fill == FillMode::Wireframe) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    raster.CullMode = (d.cull == CullMode::Back) ? D3D12_CULL_MODE_BACK
                    : (d.cull == CullMode::Front) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = d.depthClip ? TRUE : FALSE;
    raster.MultisampleEnable = (d.sampleCount > 1) ? TRUE : FALSE;
    raster.DepthBias = static_cast<INT>(d.depthBias);
    raster.SlopeScaledDepthBias = d.slopeScaledDepthBias;
    const bool warpMeshConservative = d.conservativeRaster && d.ms != 0 && dev_->softwareAdapter_;
    if (warpMeshConservative && !dev_->warpConsRasterLogged_) {
        dev_->warpConsRasterLogged_ = true;
        AVER_WARN("[RHI.D3D12] conservative rasterisation disabled for mesh-shader pipelines on the "
                  "WARP software rasteriser (it faults); voxel coverage is thinner on this adapter");
    }
    raster.ConservativeRaster = (d.conservativeRaster && dev_->caps_.conservativeRaster && !warpMeshConservative)
        ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = d.depth.test ? TRUE : FALSE;
    depth.DepthWriteMask = d.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = toComparison(d.depth.op);

    D3D12_BLEND_DESC blend{};
    {
        auto& rt0 = blend.RenderTarget[0];
        rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        switch (d.blend) {
            case BlendMode::Opaque:
                break;
            case BlendMode::AlphaBlend:
                rt0.BlendEnable = TRUE;
                rt0.SrcBlend  = D3D12_BLEND_SRC_ALPHA;
                rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                rt0.BlendOp   = D3D12_BLEND_OP_ADD;
                rt0.SrcBlendAlpha  = D3D12_BLEND_ONE;
                rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt0.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::PremultipliedAlpha:
                rt0.BlendEnable = TRUE;
                rt0.SrcBlend  = rt0.SrcBlendAlpha  = D3D12_BLEND_ONE;
                rt0.DestBlend = rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt0.BlendOp   = rt0.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::Additive:
                rt0.BlendEnable = TRUE;
                rt0.SrcBlend  = rt0.SrcBlendAlpha  = D3D12_BLEND_ONE;
                rt0.DestBlend = rt0.DestBlendAlpha = D3D12_BLEND_ONE;
                rt0.BlendOp   = rt0.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                break;
        }
    }

    const u32 rtCount = d.renderTargetCount < 4 ? d.renderTargetCount : 4;
    const u32 samples = d.sampleCount ? d.sampleCount : 1;

    if (p.mesh) {
        ComPtr<ID3D12Device2> device2;
        if (FAILED(dev_->device_.As(&device2))) {
            AVER_ERROR("[RHI.D3D12] mesh pipeline needs ID3D12Device2");
            return 0;
        }
        MeshPsoStream s{};
        s.rootSig = rs->sig.Get();
        s.ms = D3D12_SHADER_BYTECODE{ms->blob->GetBufferPointer(), ms->blob->GetBufferSize()};
        if (as) s.as = D3D12_SHADER_BYTECODE{as->blob->GetBufferPointer(), as->blob->GetBufferSize()};
        if (ps) s.ps = D3D12_SHADER_BYTECODE{ps->blob->GetBufferPointer(), ps->blob->GetBufferSize()};
        s.raster = raster;
        s.depth = depth;
        s.blend = blend;
        s.sampleMask = UINT_MAX;
        s.rtvs.value.NumRenderTargets = rtCount;
        for (u32 i = 0; i < rtCount; ++i) s.rtvs.value.RTFormats[i] = toDxgiFormat(d.renderTargets[i]);
        s.dsv = toDxgiDsvFormat(d.depthFormat);
        s.sample.value.Count = samples;
        D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(s), &s};
        if (!hrOk(device2->CreatePipelineState(&sd, IID_PPV_ARGS(&p.pso)), "rhi mesh pipeline")) return 0;
    } else {
        D3D12_INPUT_ELEMENT_DESC elems[kMaxVertexAttribs] = {};
        const UINT elemCount = buildInputLayout(d.vertexLayout, elems);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs->sig.Get();
        pd.VS = {vs->blob->GetBufferPointer(), vs->blob->GetBufferSize()};
        if (gs) pd.GS = {gs->blob->GetBufferPointer(), gs->blob->GetBufferSize()};
        if (ps) pd.PS = {ps->blob->GetBufferPointer(), ps->blob->GetBufferSize()};
        pd.InputLayout = elemCount ? D3D12_INPUT_LAYOUT_DESC{elems, elemCount}
                                   : D3D12_INPUT_LAYOUT_DESC{kMeshInputLayout, kMeshInputLayoutCount};
        pd.RasterizerState = raster;
        pd.DepthStencilState = depth;
        pd.BlendState = blend;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = rtCount;
        for (u32 i = 0; i < rtCount; ++i) pd.RTVFormats[i] = toDxgiFormat(d.renderTargets[i]);
        pd.DSVFormat = toDxgiDsvFormat(d.depthFormat);
        pd.SampleDesc.Count = samples;
        if (!hrOk(dev_->device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p.pso)), "rhi graphics pipeline")) return 0;
    }
    pipelines_.push_back(std::move(p));
    return static_cast<PipelineHandle>(pipelines_.size());
}

// Creates a compute pipeline.
PipelineHandle D3D12ResourceFactory::createComputePipeline(const ComputePipelineDesc& d) {
    collect();
    RhiShader* cs = shader(d.cs);
    if (!cs || cs->stage != ShaderStage::Compute) {
        AVER_ERROR("[RHI.D3D12] createComputePipeline given a handle that is not a compute shader");
        return 0;
    }
    const RootSigEntry* rs = rootSignature(d.layout, false);
    if (!rs) return 0;

    RhiPipeline p;
    p.compute = true;
    p.rootSig = rs->sig.Get();
    for (u32 t = 0; t < kBindingTableCount; ++t) { p.srvParam[t] = rs->srvParam[t]; p.uavParam[t] = rs->uavParam[t]; }
    p.srvBaseRegister[1] = d.layout.srvCount;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) { p.slotParam[i] = rs->slotParam[i]; p.slotDwords[i] = d.layout.constantDwords[i]; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = rs->sig.Get();
    cp.CS = {cs->blob->GetBufferPointer(), cs->blob->GetBufferSize()};
    if (!hrOk(dev_->device_->CreateComputePipelineState(&cp, IID_PPV_ARGS(&p.pso)), "rhi compute pipeline")) return 0;
    pipelines_.push_back(std::move(p));
    return static_cast<PipelineHandle>(pipelines_.size());
}

// Reserves a descriptor range for a binding set, null-fills it, and returns its handle.
BindingSetHandle D3D12ResourceFactory::createBindingSet(const BindingSetDesc& d) {
    collect();
    const u32 count = d.srvCount + d.uavCount;
    if (count == 0) { AVER_ERROR("[RHI.D3D12] createBindingSet declaring no slots"); return 0; }
    if (d.srvCount > kMaxBindingSlots || d.uavCount > kMaxBindingSlots) {
        AVER_ERROR("[RHI.D3D12] createBindingSet declares {} SRV / {} UAV slots, over the {} limit",
                   d.srvCount, d.uavCount, kMaxBindingSlots);
        return 0;
    }

    RhiBindingSet s;
    s.srvCount = d.srvCount;
    s.uavCount = d.uavCount;
    s.srvBaseRegister = d.srvBaseRegister;
    s.uavBaseRegister = d.uavBaseRegister;
    for (u32 i = 0; i < kMaxBindingSlots; ++i) { s.srvKinds[i] = d.srvKinds[i]; s.uavKinds[i] = d.uavKinds[i]; }
    if (!allocRange(count, s.heapBase)) return 0;
    s.alive = true;
    nullFill(s);
    bindingSets_.push_back(s);
    return static_cast<BindingSetHandle>(bindingSets_.size());
}

namespace {
// Geometry description shared by the prebuild query and the build. The result points at `geo`.
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs(const GpuMesh& m, D3D12_RAYTRACING_GEOMETRY_DESC& geo) {
    geo = {};
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
    return in;
}
} // namespace

// Allocates a bottom-level acceleration structure for a mesh, sized by the prebuild query.
BlasHandle D3D12ResourceFactory::createBlas(MeshHandle mesh) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createBlas without ray-tracing support"); return 0; }
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.D3D12] createBlas with an invalid mesh handle"); return 0; }
    // A DESTROYED mesh is invalid too, and separately worth naming: the handle is in range and the
    // slot exists, so the bounds test above passes and the build would proceed over a cleared vertex
    // view. Returning 0 lets the caller record "no structure for this mesh" and stop asking.
    if (!dev_->meshes_[mesh - 1].alive) { AVER_WARN("[RHI.D3D12] createBlas for destroyed mesh {}", mesh); return 0; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    if (m.indexCount == 0) { AVER_ERROR("[RHI.D3D12] createBlas for a mesh with no indices"); return 0; }

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = blasInputs(m, geo);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

    RhiBlas b;
    b.mesh = mesh;
    b.as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    b.scratch = makeAsBuffer(dev_->device_.Get(), info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);
    if (!b.as || !b.scratch) { AVER_ERROR("[RHI.D3D12] createBlas allocation failed"); return 0; }
    blases_.push_back(std::move(b));
    return static_cast<BlasHandle>(blases_.size());
}

// Allocates a top-level acceleration structure for up to `maxInstances` instances.
TlasHandle D3D12ResourceFactory::createTlas(u32 maxInstances) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createTlas without ray-tracing support"); return 0; }
    if (maxInstances == 0) { AVER_ERROR("[RHI.D3D12] createTlas for zero instances"); return 0; }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = maxInstances;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

    RhiTlas t;
    t.maxInstances = maxInstances;
    t.as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    t.scratch = makeAsBuffer(dev_->device_.Get(), info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);
    if (!t.as || !t.scratch) { AVER_ERROR("[RHI.D3D12] createTlas allocation failed"); return 0; }

    const u64 bytes = static_cast<u64>(maxInstances) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = bufferDesc(bytes);
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(dev_->device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&t.instances[i])), "rhi TLAS instances")) return 0;
        D3D12_RANGE none{0, 0};
        t.instances[i]->Map(0, &none, reinterpret_cast<void**>(&t.instancePtr[i]));
    }
    tlases_.push_back(std::move(t));
    return static_cast<TlasHandle>(tlases_.size());
}

// ---- destruction. Nothing is released here: it is queued behind the fence.
// Retires a texture and its views, and returns any UI descriptor it held.
void D3D12ResourceFactory::destroyTexture(TextureHandle h) {
    RhiTexture* t = texture(h);
    if (!t) return;
    if (t->uiSrvCpu && dev_->uiBackend_) {
        dev_->uiBackend_->freeTextureSrv(t->uiSrvCpu);
        t->uiSrvCpu = t->uiSrvGpu = 0;
    }
    retire(t->res);
    retire(t->rtvHeap);
    retire(t->dsvHeap);
    t->res.Reset(); t->rtvHeap.Reset(); t->dsvHeap.Reset();
    collect();
}

// Retires a buffer.
void D3D12ResourceFactory::destroyBuffer(BufferHandle h) {
    RhiBuffer* b = buffer(h);
    if (!b) return;
    retire(b->res);
    b->res.Reset();
    b->mapped = nullptr;
    collect();
}

// Releases an acceleration structure, its scratch with it.
//
// The slot is cleared and kept, exactly as a mesh slot is, so a stale BlasHandle names something
// dead rather than something else's structure. `mesh` going to 0 is what blasMesh() reports and
// what lets VoxiRenderer's cache notice on its own that its entry has expired.
void D3D12ResourceFactory::destroyBlas(BlasHandle h) {
    if (h == 0 || h > blases_.size()) return;
    RhiBlas& b = blases_[h - 1];
    if (!b.as && !b.scratch) return;
    retire(b.as);
    retire(b.scratch);
    b.as.Reset();
    b.scratch.Reset();
    b.mesh = 0;
    b.built = false;
    collect();
}

MeshHandle D3D12ResourceFactory::blasMesh(BlasHandle h) const {
    if (h == 0 || h > blases_.size()) return 0;
    return blases_[h - 1].mesh;
}

// Destroys every structure built from `mesh`. A linear scan, and that is proportionate: blases_ has
// one entry per distinct mesh ever ray-traced, which is the same order as the mesh table itself and
// is walked once per destroy rather than once per frame.
void D3D12ResourceFactory::destroyBlasForMesh(MeshHandle mesh) {
    if (mesh == 0) return;
    for (usize i = 0; i < blases_.size(); ++i)
        if (blases_[i].mesh == mesh) destroyBlas(static_cast<BlasHandle>(i + 1));
}

// Frees a shader's bytecode.
void D3D12ResourceFactory::destroyShader(ShaderHandle h) {
    RhiShader* s = shader(h);
    if (!s) return;
    s->blob.Reset();
}

// Retires a pipeline state; its root signature stays in the cache.
void D3D12ResourceFactory::destroyPipeline(PipelineHandle h) {
    RhiPipeline* p = pipeline(h);
    if (!p) return;
    retire(p->pso);
    p->pso.Reset();
    p->rootSig = nullptr;
    collect();
}

// Returns a binding set's descriptor range, reusable once the fence passes.
void D3D12ResourceFactory::destroyBindingSet(BindingSetHandle h) {
    RhiBindingSet* s = bindingSet(h);
    if (!s) return;
    pendingRanges_.push_back({s->heapBase, s->srvCount + s->uavCount, retireFence()});
    s->alive = false;
    collect();
}

// ---- population
// Writes a texture SRV into one slot of a binding set. kAllMips views the whole chain.
void D3D12ResourceFactory::setSrv(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setSrv with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] setSrv slot {} past the {} declared", slot, s->srvCount); return; }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = toDxgiSrvFormat(t->desc.format);
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const bool whole = (mip == kAllMips);
    if (s->srvKinds[slot] == SlotKind::Texture2DMS) {
        // A multisampled resource cannot have more than one mip (D3D12_TEX2DMS_SRV carries no
        // mip/level fields at all, unlike every other dimension below), so `mip` is meaningless here
        // and simply ignored -- the ONE view this dimension can express already covers the whole
        // (single-level) resource.
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else if (t->desc.dim == TextureDim::Tex3D) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MostDetailedMip = whole ? 0 : mip;
        sv.Texture3D.MipLevels = whole ? t->desc.mips : 1;
    } else {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = whole ? 0 : mip;
        sv.Texture2D.MipLevels = whole ? t->desc.mips : 1;
    }
    dev_->device_->CreateShaderResourceView(t->res.Get(), &sv, cpuSlot(s->heapBase + slot));
}

// Writes a texture UAV for one mip into one slot of a binding set.
void D3D12ResourceFactory::setUav(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setUav with an invalid handle"); return; }
    if (slot >= s->uavCount) { AVER_ERROR("[RHI.D3D12] setUav slot {} past the {} declared", slot, s->uavCount); return; }
    if (mip == kAllMips || mip >= t->desc.mips) { AVER_ERROR("[RHI.D3D12] setUav needs a single valid mip"); return; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = toDxgiSrvFormat(t->desc.format);
    if (t->desc.dim == TextureDim::Tex3D) {
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uv.Texture3D.MipSlice = mip;
        uv.Texture3D.WSize = (t->desc.depth >> mip) ? (t->desc.depth >> mip) : 1u;
    } else {
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uv.Texture2D.MipSlice = mip;
    }
    dev_->device_->CreateUnorderedAccessView(t->res.Get(), nullptr, &uv, cpuSlot(s->heapBase + s->srvCount + slot));
}

// Writes an acceleration-structure SRV into one slot of a binding set.
void D3D12ResourceFactory::setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle h) {
    RhiBindingSet* s = bindingSet(set);
    RhiTlas* t = tlas(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setSrvTlas with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] setSrvTlas slot {} past the {} declared", slot, s->srvCount); return; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.RaytracingAccelerationStructure.Location = t->as->GetGPUVirtualAddress();
    dev_->device_->CreateShaderResourceView(nullptr, &sv, cpuSlot(s->heapBase + slot));
}

// True when a structured view of `count` elements of `stride` starting at `firstElement` fits
// inside the buffer it is being created over. Logs and returns false when it does not.
//
// WHY THIS IS CHECKED HERE AND NOT LEFT TO THE DEBUG LAYER. A view that overruns its buffer is not
// rejected by D3D12 at CREATION time in a normal run -- there is no synchronous validation without
// the debug layer -- so the first thing that notices is a shader reading or writing past the
// allocation, which surfaces as DXGI_ERROR_DEVICE_HUNG and takes the process with it. That is the
// worst possible shape for a diagnostic: it arrives later than the mistake, on a different thread,
// with no reference to the descriptor that caused it, and only on the machines unlucky enough to
// have something mapped past the end.
//
// It cost this project exactly that once already: the path tracer's denoise buffers were allocated
// once at 480x270 and then handed a 1280x720 view when the quality rung changed, and the report that
// came back was "sometimes path tracing crashes the engine when turning it to higher settings" --
// with no other information available, because the only tool that named it was --debug-layer, which
// nobody runs by default. One comparison at descriptor-write time turns that into a log line naming
// the slot, the counts and the buffer.
bool viewFitsBuffer(const RhiBuffer& b, u32 stride, u32 count, u32 firstElement, const char* what,
                    u32 slot) {
    const u64 needed = (static_cast<u64>(firstElement) + count) * stride;
    if (needed <= b.desc.bytes) return true;
    AVER_ERROR("[RHI.D3D12] {} slot {}: a view of {} element(s) x {} byte(s) from element {} needs "
               "{} byte(s), but the buffer is only {} -- REFUSED. Left unchecked this is not an "
               "error at all until a shader walks off the end of it and the device is removed.",
               what, slot, count, stride, firstElement, needed, b.desc.bytes);
    return false;
}

// Puts a structured-buffer SRV in a slot.
void D3D12ResourceFactory::setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle bh,
                                        u32 stride, u32 count, u32 firstElement) {
    RhiBindingSet* s = bindingSet(set);
    if (!s || bh == 0 || bh > buffers_.size()) { AVER_ERROR("[RHI.D3D12] setSrvBuffer with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] setSrvBuffer slot {} past the {} declared", slot, s->srvCount); return; }
    if (s->srvKinds[slot] != SlotKind::StructuredBuffer) {
        AVER_ERROR("[RHI.D3D12] setSrvBuffer on slot {}, which was declared as a texture", slot);
        return;
    }
    if (stride == 0) { AVER_ERROR("[RHI.D3D12] setSrvBuffer with a zero stride"); return; }
    if (!viewFitsBuffer(buffers_[bh - 1], stride, count, firstElement, "setSrvBuffer", slot)) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Buffer.FirstElement = firstElement;
    sv.Buffer.NumElements = count;
    sv.Buffer.StructureByteStride = stride;
    dev_->device_->CreateShaderResourceView(buffers_[bh - 1].res.Get(), &sv, cpuSlot(s->heapBase + slot));
}

// Puts a structured-buffer UAV in a slot.
void D3D12ResourceFactory::setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle bh,
                                        u32 stride, u32 count, u32 firstElement) {
    RhiBindingSet* s = bindingSet(set);
    if (!s || bh == 0 || bh > buffers_.size()) { AVER_ERROR("[RHI.D3D12] setUavBuffer with an invalid handle"); return; }
    if (slot >= s->uavCount) { AVER_ERROR("[RHI.D3D12] setUavBuffer slot {} past the {} declared", slot, s->uavCount); return; }
    if (s->uavKinds[slot] != SlotKind::StructuredBuffer) {
        AVER_ERROR("[RHI.D3D12] setUavBuffer on slot {}, which was declared as a texture", slot);
        return;
    }
    if (stride == 0) { AVER_ERROR("[RHI.D3D12] setUavBuffer with a zero stride"); return; }
    if (!viewFitsBuffer(buffers_[bh - 1], stride, count, firstElement, "setUavBuffer", slot)) return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_UNKNOWN;
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Buffer.FirstElement = firstElement;
    uv.Buffer.NumElements = count;
    uv.Buffer.StructureByteStride = stride;
    dev_->device_->CreateUnorderedAccessView(buffers_[bh - 1].res.Get(), nullptr, &uv,
                                             cpuSlot(s->heapBase + s->srvCount + slot));
}

// Copies `bytes` out of a readback buffer. Does no synchronisation; see the interface.
bool D3D12ResourceFactory::readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset) {
    if (h == 0 || h > buffers_.size()) { AVER_ERROR("[RHI.D3D12] readBuffer with an invalid handle"); return false; }
    RhiBuffer& b = buffers_[h - 1];
    if (b.desc.kind != BufferKind::Readback || !b.mapped) {
        AVER_ERROR("[RHI.D3D12] readBuffer on a buffer that is not BufferKind::Readback");
        return false;
    }
    if (!dst || bytes == 0) return true;
    if (offset + bytes > b.desc.bytes) {
        AVER_ERROR("[RHI.D3D12] readBuffer of {} bytes at {} overruns a {}-byte buffer", bytes, offset, b.desc.bytes);
        return false;
    }
    std::memcpy(dst, b.mapped + offset, static_cast<usize>(bytes));
    return true;
}

// Copies `bytes` into an upload buffer at `offset`. False for a non-upload buffer or an overrun.
bool D3D12ResourceFactory::writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) {
    if (h == 0 || h > buffers_.size()) { AVER_ERROR("[RHI.D3D12] writeBuffer with an invalid handle"); return false; }
    RhiBuffer& b = buffers_[h - 1];
    if (!b.mapped) { AVER_ERROR("[RHI.D3D12] writeBuffer on a buffer that is not BufferKind::Upload"); return false; }
    if (!src || bytes == 0) return true;
    if (offset + bytes > b.desc.bytes) {
        AVER_ERROR("[RHI.D3D12] writeBuffer of {} bytes at {} overruns a {}-byte buffer",
                   bytes, offset, b.desc.bytes);
        return false;
    }
    std::memcpy(b.mapped + offset, src, static_cast<usize>(bytes));
    return true;
}

// Copies a texture's resolved description into `out`.
bool D3D12ResourceFactory::textureInfo(TextureHandle h, TextureDesc& out) const {
    const RhiTexture* t = texture(h);
    if (!t) return false;
    out = t->desc;
    return true;
}

// Drains the GPU and releases everything now retired.
void D3D12ResourceFactory::waitIdle() {
    dev_->waitForGpu();
    collect();
}

// The descriptor the UI draws a texture with, allocated from the installed UI backend's own pool on
// first use (see IUiBackend::allocTextureSrv's comment for why it must be THAT pool). Zero with no
// backend installed, same as before this delegated rather than calling ImGui's pool directly.
u64 D3D12ResourceFactory::uiDescriptor(TextureHandle h) {
    if (!dev_->uiBackend_) return 0;
    RhiTexture* t = texture(h);
    if (!t || !t->res) return 0;
    if (t->uiSrvGpu) return t->uiSrvGpu;
    if (!any(t->desc.bind, ResourceBind::ShaderResource)) {
        AVER_ERROR("[RHI.D3D12] uiTextureId on a texture not created as a shader resource");
        return 0;
    }

    u64 cpu = 0, gpu = 0;
    if (!dev_->uiBackend_->allocTextureSrv(&cpu, &gpu) || !gpu) return 0;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = toDxgiSrvFormat(t->desc.format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = t->desc.mips;
    dev_->device_->CreateShaderResourceView(t->res.Get(), &sv, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(cpu)});

    t->uiSrvCpu = cpu;
    t->uiSrvGpu = gpu;
    return t->uiSrvGpu;
}

// Compute shader for the factory self-test: fills a volume with a constant.
namespace {
const char* kSelfTestCS = R"(
cbuffer SelfTestCB : register(b0) { uint4 gValue; };
RWTexture3D<float4> gOut : register(u0);
[numthreads(4,4,4)]
void CSSelfTest(uint3 id : SV_DispatchThreadID) { gOut[id] = float4(gValue); }
)";
} // namespace

// Exercises the factory end to end at init and logs each step.
void D3D12ResourceFactory::selfTest() {
    TextureDesc td{};
    td.dim = TextureDim::Tex3D;
    td.width = td.height = td.depth = 32;
    td.mips = 0;
    td.format = Format::RGBA16F;
    td.bind = ResourceBind::ShaderResource | ResourceBind::UnorderedAccess;
    td.initialState = ResourceState::UnorderedAccess;
    td.debugName = "AverRhiSelfTestVolume";
    const TextureHandle tex = createTexture(td);
    TextureDesc got{};
    const bool info = tex != 0 && textureInfo(tex, got);
    AVER_INFO("[RHI.D3D12] factory self-test: texture {} (32^3 RGBA16F, {} mips resolved)",
              tex ? "ok" : "FAILED", info ? got.mips : 0u);

    BufferDesc bd{};
    bd.bytes = 4096;
    bd.kind = BufferKind::Upload;
    bd.debugName = "AverRhiSelfTestBuffer";
    const BufferHandle buf = createBuffer(bd);
    AVER_INFO("[RHI.D3D12] factory self-test: buffer {}", buf ? "ok" : "FAILED");

    ShaderDesc sd{};
    sd.source = kSelfTestCS;
    sd.entry = "CSSelfTest";
    sd.stage = ShaderStage::Compute;
    sd.minShaderModel = 51;
    const ShaderHandle cs = createShader(sd);
    ComputePipelineDesc cd{};
    cd.cs = cs;
    cd.layout.uavCount = 1;
    cd.layout.constantDwords[0] = 4;
    const PipelineHandle pipe = cs ? createComputePipeline(cd) : 0;
    AVER_INFO("[RHI.D3D12] factory self-test: compute pipeline {}", pipe ? "ok" : "FAILED");

    BindingSetDesc bsd{};
    bsd.srvCount = 1;
    bsd.uavCount = 1;
    bsd.srvKinds[0] = SlotKind::Texture3D;
    bsd.uavKinds[0] = SlotKind::Texture3D;
    const BindingSetHandle set = createBindingSet(bsd);
    if (set && tex) setUav(set, 0, tex, 0);
    AVER_INFO("[RHI.D3D12] factory self-test: binding set {}", set ? "ok" : "FAILED");

    const u32 firstBase = set ? bindingSets_[set - 1].heapBase : 0;
    destroyBindingSet(set);
    const BindingSetHandle early = createBindingSet(bsd);
    const bool heldBack = early && bindingSets_[early - 1].heapBase != firstBase;
    AVER_INFO("[RHI.D3D12] factory self-test: descriptor reclaim {} (returned range held behind the fence)",
              heldBack ? "ok" : "FAILED");

    destroyBindingSet(early);

    // ---- buffer views, which is what a compute skinning pass needs and what did not exist ----
    //
    // The DESCRIPTOR half is what this checks: a Tier 1 null descriptor of the wrong dimension is
    // undefined, and a structured-buffer view with no stride is rejected outright -- both of which
    // the debug layer catches here rather than in a shader that silently reads zeros. The dispatch
    // half cannot be checked from here: this runs at init, and there is no open command list.
    BufferDesc sd2{};
    sd2.bytes = 4096;
    sd2.kind = BufferKind::Default;
    sd2.allowUnorderedAccess = true;
    sd2.debugName = "AverRhiSelfTestStructured";
    const BufferHandle sbuf = createBuffer(sd2);

    BufferDesc rb{};
    rb.bytes = 4096;
    rb.kind = BufferKind::Readback;
    rb.debugName = "AverRhiSelfTestReadback";
    const BufferHandle rbuf = createBuffer(rb);

    BindingSetDesc bufSet{};
    bufSet.srvCount = 1;
    bufSet.uavCount = 1;
    bufSet.srvKinds[0] = SlotKind::StructuredBuffer;
    bufSet.uavKinds[0] = SlotKind::StructuredBuffer;
    const BindingSetHandle bset = createBindingSet(bufSet);   // null-filled on creation
    if (bset && sbuf) {
        setSrvBuffer(bset, 0, sbuf, 16, 256, 0);
        setUavBuffer(bset, 0, sbuf, 16, 256, 0);
    }
    // A readback buffer must be readable, and an unwritten one reads as whatever the heap held --
    // so this checks the CALL succeeds, not the contents.
    u8 probe[16] = {};
    const bool readOk = rbuf && readBuffer(rbuf, probe, sizeof probe, 0);
    AVER_INFO("[RHI.D3D12] factory self-test: buffer views {} (structured SRV+UAV, readback {})",
              (sbuf && rbuf && bset) ? "ok" : "FAILED", readOk ? "ok" : "FAILED");

    destroyBindingSet(bset);
    destroyBuffer(rbuf);
    destroyBuffer(sbuf);
    destroyPipeline(pipe);
    destroyShader(cs);
    destroyBuffer(buf);
    destroyTexture(tex);
}

// ================================================================ generic RHI command recording

// Binds a pipeline and its root signature, then gives every declared root CBV a valid address.
void D3D12RenderContext::setPipeline(PipelineHandle h) {
    pipe_ = nullptr;
    RhiPipeline* p = res_->pipeline(h);
    if (!p) { AVER_ERROR("[RHI.D3D12] setPipeline with an invalid handle"); return; }
    if (!dev_->cmdList_) return;
    if (p->compute) {
        dev_->cmdList_->SetComputeRootSignature(p->rootSig);
    } else {
        dev_->cmdList_->SetGraphicsRootSignature(p->rootSig);
        dev_->boundRootSig_ = nullptr;
    }
    dev_->cmdList_->SetPipelineState(p->pso.Get());
    dev_->boundPso_ = p->pso.Get();   // matches what the line above just bound, not a guess
    dev_->fovValid_ = false;          // a different pipeline is bound now; see fovValid_'s comment
    pipe_ = p;

    bindDeclaredRootCbvs(p);
}

// Gives every root CBV the pipeline declares a valid address: slot 0 the engine PerFrame block,
// the rest a zero-filled buffer. Drawing with an unset root CBV is undefined and can hang the GPU.
void D3D12RenderContext::bindDeclaredRootCbvs(const RhiPipeline* p) {
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    const D3D12_GPU_VIRTUAL_ADDRESS zero = zeroCbv();
    for (u32 s = 0; s < kMaxConstantSlots; ++s) {
        if (p->slotParam[s] < 0 || p->slotDwords[s] != 0) continue;
        D3D12_GPU_VIRTUAL_ADDRESS va = zero;
        if (s == kEngineFrameConstantRegister && dev_->frameCBs_[f])
            va = dev_->frameCBs_[f]->GetGPUVirtualAddress();
        if (!va) continue;
        const UINT param = static_cast<UINT>(p->slotParam[s]);
        if (p->compute) dev_->cmdList_->SetComputeRootConstantBufferView(param, va);
        else            dev_->cmdList_->SetGraphicsRootConstantBufferView(param, va);
    }
}

// One 256-byte zeroed upload buffer, shared by every declared-but-unsupplied constant slot.
D3D12_GPU_VIRTUAL_ADDRESS D3D12RenderContext::zeroCbv() {
    if (!zeroCB_) {
        auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = bufferDesc(256);
        if (!hrOk(dev_->device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zeroCB_)), "rhi zero CBV")) return 0;
        void* ptr = nullptr;
        D3D12_RANGE none{0, 0};
        if (SUCCEEDED(zeroCB_->Map(0, &none, &ptr)) && ptr) {
            std::memset(ptr, 0, 256);
            zeroCB_->Unmap(0, nullptr);
        }
    }
    return zeroCB_ ? zeroCB_->GetGPUVirtualAddress() : 0;
}

// Sets the viewport rectangle.
void D3D12RenderContext::setViewport(u32 x, u32 y, u32 w, u32 h) {
    if (!dev_->cmdList_) return;
    D3D12_VIEWPORT vp{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f};
    dev_->cmdList_->RSSetViewports(1, &vp);
}

// Sets the scissor rectangle.
void D3D12RenderContext::setScissor(u32 x, u32 y, u32 w, u32 h) {
    if (!dev_->cmdList_) return;
    D3D12_RECT sc{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + w), static_cast<LONG>(y + h)};
    dev_->cmdList_->RSSetScissorRects(1, &sc);
}

// Binds up to four colour targets and an optional depth target.
void D3D12RenderContext::setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) {
    if (!dev_->cmdList_) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]{};
    u32 n = 0;
    for (u32 i = 0; i < count && i < 4 && colors; ++i) {
        RhiTexture* t = res_->texture(colors[i]);
        if (!t || !t->rtvHeap) { AVER_ERROR("[RHI.D3D12] setRenderTargets: colour {} was not created as a render target", i); continue; }
        rtv[n++] = t->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool haveDepth = false;
    if (depth) {
        RhiTexture* t = res_->texture(depth);
        if (t && t->dsvHeap) { dsv = t->dsvHeap->GetCPUDescriptorHandleForHeapStart(); haveDepth = true; }
        else AVER_ERROR("[RHI.D3D12] setRenderTargets: depth was not created as a depth target");
    }
    dev_->cmdList_->OMSetRenderTargets(n, n ? rtv : nullptr, FALSE, haveDepth ? &dsv : nullptr);
}

// Clears a depth target to `value`.
void D3D12RenderContext::clearDepth(TextureHandle depth, f32 value) {
    RhiTexture* t = res_->texture(depth);
    if (!t || !t->dsvHeap || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] clearDepth on a non-depth texture"); return; }
    dev_->cmdList_->ClearDepthStencilView(t->dsvHeap->GetCPUDescriptorHandleForHeapStart(),
                                          D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
}

// Clears a colour target to `color`. Added because a render feature with an off-screen colour
// target and no full-screen background pass -- ActorPreview is the first one -- had no way to clear
// it at all: this RHI's only clear entry point was clearDepth. Without it the target kept every
// previous frame's pixels wherever the current frame did not draw over them, compositing forever.
void D3D12RenderContext::clearColor(TextureHandle target, const f32 color[4]) {
    RhiTexture* t = res_->texture(target);
    if (!t || !t->rtvHeap || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] clearColor on a non-render-target texture"); return; }
    dev_->cmdList_->ClearRenderTargetView(t->rtvHeap->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
}

// Binds a binding set's descriptors at the given table index of the current pipeline.
void D3D12RenderContext::setBindingSet(BindingSetHandle set, u32 table) {
    if (!pipe_) { AVER_ERROR("[RHI.D3D12] setBindingSet before setPipeline"); return; }
    if (table >= kBindingTableCount) { AVER_ERROR("[RHI.D3D12] setBindingSet table {} past the {} declarable", table, kBindingTableCount); return; }
    RhiBindingSet* s = res_->bindingSet(set);
    if (!s || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setBindingSet with an invalid handle"); return; }

    // This backend has exactly one generic heap, so a scene of thousands of feature-overridden draws
    // (D3D12Device::drawMesh's per-entity branch) called this with the SAME single-entry array every
    // single draw. Elided the same way boundRootSig_/boundPso_ already elide their own redundant
    // sets below -- see boundHeap_'s member comment for the invalidation this depends on staying
    // correct at every OTHER site that calls SetDescriptorHeaps directly.
    ID3D12DescriptorHeap* const heap = res_->heap_.Get();
    if (dev_->boundHeap_ != heap) {
        ID3D12DescriptorHeap* heaps[] = {heap};
        dev_->cmdList_->SetDescriptorHeaps(1, heaps);
        dev_->boundHeap_ = heap;
    }
    dev_->boundRootSig_ = nullptr;
    dev_->boundPso_ = nullptr;
    // TABLE 0 ONLY. Table 1 is the per-material binding that applyDrawBinding sets on every single
    // draw; invalidating on that would mean the cache never once survived to the next entity, which
    // is the whole point of it. Table 1 does not disturb what table 0 holds.
    if (table == 0) dev_->fovValid_ = false;

    // ONCE PER SHAPE, not once per draw -- see bindingBaseWarned_'s own comment for why a mismatch
    // here is expected on a pipeline that borrows the shared material system's binding sets.
    if (s->srvCount && s->srvBaseRegister != pipe_->srvBaseRegister[table]) {
        const u64 key = (static_cast<u64>(s->srvBaseRegister) << 40) |
                        (static_cast<u64>(table) << 32) |
                        static_cast<u64>(pipe_->srvBaseRegister[table]);
        if (std::find(dev_->bindingBaseWarned_.begin(), dev_->bindingBaseWarned_.end(), key) ==
            dev_->bindingBaseWarned_.end()) {
            dev_->bindingBaseWarned_.push_back(key);
            AVER_WARN("[RHI.D3D12] binding set was built for t{} but table {} covers t{} "
                      "(said once per shape; harmless when one set serves two pipelines)",
                      s->srvBaseRegister, table, pipe_->srvBaseRegister[table]);
        }
    }

    if (s->srvCount && pipe_->srvParam[table] >= 0) {
        const D3D12_GPU_DESCRIPTOR_HANDLE h = res_->gpuSlot(s->heapBase);
        if (pipe_->compute) dev_->cmdList_->SetComputeRootDescriptorTable(static_cast<UINT>(pipe_->srvParam[table]), h);
        else                dev_->cmdList_->SetGraphicsRootDescriptorTable(static_cast<UINT>(pipe_->srvParam[table]), h);
    }
    if (s->uavCount && pipe_->uavParam[table] >= 0) {
        const D3D12_GPU_DESCRIPTOR_HANDLE h = res_->gpuSlot(s->heapBase + s->srvCount);
        if (pipe_->compute) dev_->cmdList_->SetComputeRootDescriptorTable(static_cast<UINT>(pipe_->uavParam[table]), h);
        else                dev_->cmdList_->SetGraphicsRootDescriptorTable(static_cast<UINT>(pipe_->uavParam[table]), h);
    }
}

// Writes root constants into a slot the pipeline declared as root constants.
void D3D12RenderContext::setConstants(u32 slot, const void* data, u32 dwords) {
    if (!pipe_ || slot >= kMaxConstantSlots || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setConstants without a pipeline"); return; }
    const i32 param = pipe_->slotParam[slot];
    const u32 declared = pipe_->slotDwords[slot];
    if (param < 0 || declared == 0) {
        AVER_ERROR("[RHI.D3D12] setConstants: slot {} declares constantDwords 0, so it is a root CBV â€” use setConstantBuffer", slot);
        return;
    }

    u32 block[64] = {};
    const u32 n = declared < 64 ? declared : 64;
    const u32 copy = dwords < n ? dwords : n;
    if (data && copy) std::memcpy(block, data, copy * sizeof(u32));
    if (pipe_->compute) dev_->cmdList_->SetComputeRoot32BitConstants(static_cast<UINT>(param), n, block, 0);
    else                dev_->cmdList_->SetGraphicsRoot32BitConstants(static_cast<UINT>(param), n, block, 0);
}

// Uploads `bytes` to the frame ring and binds it as the root CBV for `slot`.
void D3D12RenderContext::setConstantBuffer(u32 slot, const void* data, u32 bytes) {
    if (!pipe_ || slot >= kMaxConstantSlots || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setConstantBuffer without a pipeline"); return; }
    const i32 param = pipe_->slotParam[slot];
    if (param < 0 || pipe_->slotDwords[slot] != 0) {
        AVER_ERROR("[RHI.D3D12] setConstantBuffer: slot {} declares {} root constants, not a CBV â€” use setConstants",
                   slot, pipe_->slotDwords[slot]);
        return;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = ringAlloc(data, bytes);
    if (!va) return;
    if (pipe_->compute) dev_->cmdList_->SetComputeRootConstantBufferView(static_cast<UINT>(param), va);
    else                dev_->cmdList_->SetGraphicsRootConstantBufferView(static_cast<UINT>(param), va);
}

// Records the sticky per-draw binding, which is applied at the draw rather than here.
void D3D12RenderContext::setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
    if (bytes > kMaxDrawConstantBytes) {
        AVER_ERROR("[RHI.D3D12] setDrawBinding constant block is {} bytes, over the {} limit", bytes, kMaxDrawConstantBytes);
        return;
    }
    drawSet_ = set;
    drawConstantBytes_ = (constants && bytes) ? bytes : 0;
    if (drawConstantBytes_) std::memcpy(drawConstants_, constants, drawConstantBytes_);
}

// Binds the sticky per-draw state, if the current pipeline declared anywhere to put it.
void D3D12RenderContext::applyDrawBinding() {
    if (!pipe_) return;
    if (drawSet_ && pipe_->srvParam[1] >= 0) setBindingSet(drawSet_, 1);
    if (drawConstantBytes_ && pipe_->slotParam[kDrawConstantRegister] >= 0 &&
        pipe_->slotDwords[kDrawConstantRegister] == 0)
        setConstantBuffer(kDrawConstantRegister, drawConstants_, drawConstantBytes_);
}

// Copies `bytes` into this frame's upload ring and returns their GPU address.
//
// THE RING GROWS, and it did not used to. It was a fixed 1 MB per frame, and every per-draw constant
// costs a 256-byte-aligned slice, so a scene crossed the cliff at roughly four thousand draws: past
// that, ringAlloc returned 0 and the draw silently lost its constants. Worse than the dropped draws
// was the diagnosis -- one AVER_ERROR per failed call, which on a 5,760-instance forest produced
// over four million log lines in a 600-frame run and cost far more time than the rendering did. A
// content change (denser scatter) is not supposed to be able to do that to a backend.
//
// Growth happens at the FRAME BOUNDARY, never mid-frame: addresses already handed out this frame
// point into the live buffer, and reallocating under them would hand the GPU freed memory. So an
// exhausted frame still loses its remaining constants -- there is no way to rescue it -- but it
// records the size it actually wanted, and the next frame through this buffer is big enough. In
// practice that means one bad frame on the way up, not a permanently broken scene.
D3D12_GPU_VIRTUAL_ADDRESS D3D12RenderContext::ringAlloc(const void* data, u32 bytes) {
    if (!data || bytes == 0) return 0;
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;

    // New epoch: reset the cursor, and take the growth the previous frames asked for.
    if (ringEpoch_ != dev_->nextFence_) {
        ringEpoch_ = dev_->nextFence_;
        ringUsed_[f] = 0;
        if (ringWanted_ > ringBytes_[f]) {
            // This buffer is only safe to drop now because kFrameCount frames have retired since it
            // was last recorded into -- the same invariant that makes the cursor reset safe.
            ring_[f].Reset();
            ringPtr_[f] = nullptr;
            ringBytes_[f] = 0;
        }
    }

    if (!ring_[f]) {
        u64 want = ringWanted_ > kRhiRingBytes ? ringWanted_ : kRhiRingBytes;
        if (want > kRhiRingMaxBytes) want = kRhiRingMaxBytes;
        auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = bufferDesc(want);
        if (!hrOk(dev_->device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ring_[f])), "rhi upload ring")) return 0;
        D3D12_RANGE none{0, 0};
        ring_[f]->Map(0, &none, reinterpret_cast<void**>(&ringPtr_[f]));
        ringBytes_[f] = want;
        if (want > kRhiRingBytes)
            AVER_INFO("[RHI.D3D12] upload ring grown to {} KB for frame {}", want / 1024, f);
    }

    const u64 offset = (ringUsed_[f] + 255ull) & ~255ull;
    const u64 size = (static_cast<u64>(bytes) + 255ull) & ~255ull;
    if (offset + size > ringBytes_[f]) {
        // Ask for headroom rather than exactly what this call needed: the frame is still running and
        // more allocations are almost certainly coming behind this one.
        const u64 want = (offset + size) * 2ull;
        if (want > ringWanted_) ringWanted_ = want > kRhiRingMaxBytes ? kRhiRingMaxBytes : want;
        // ONCE PER FRAME, not once per call. See this function's header comment: the per-call form
        // of this message was itself the dominant cost of the frame it was reporting on.
        if (ringOverflowEpoch_ != ringEpoch_) {
            ringOverflowEpoch_ = ringEpoch_;
            if (ringBytes_[f] >= kRhiRingMaxBytes)
                AVER_ERROR("[RHI.D3D12] upload ring exhausted at its {} KB ceiling -- draws in this "
                           "frame are losing their constants. Reduce draw count or raise "
                           "kRhiRingMaxBytes.", kRhiRingMaxBytes / 1024);
            else
                AVER_WARN("[RHI.D3D12] upload ring ({} KB) exhausted this frame; growing to {} KB",
                          ringBytes_[f] / 1024, ringWanted_ / 1024);
        }
        return 0;
    }
    std::memcpy(ringPtr_[f] + offset, data, bytes);
    ringUsed_[f] = offset + size;
    return ring_[f]->GetGPUVirtualAddress() + offset;
}

// Draws a device mesh through the input assembler.
void D3D12RenderContext::drawMesh(MeshHandle mesh) {
    if (!dev_->cmdList_) return;
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.D3D12] drawMesh with an invalid mesh handle"); return; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    applyDrawBinding();
    dev_->cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    dev_->cmdList_->IASetIndexBuffer(&m.ibv);
    dev_->cmdList_->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
}

// Draws `instanceCount` copies of a device mesh in one DrawIndexedInstanced, with per-instance world
// transforms read from a root-SRV-bound StructuredBuffer -- see IRenderContext::drawMeshInstanced and
// RHIResources.hpp's comment above GraphicsPipelineDesc::instanced for the whole mechanism.
void D3D12RenderContext::drawMeshInstanced(MeshHandle mesh, const f32* worlds, u32 instanceCount) {
    if (!dev_->cmdList_) return;
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.D3D12] drawMeshInstanced with an invalid mesh handle"); return; }
    if (instanceCount == 0) return;
    if (!pipe_ || pipe_->instanceWorldParam < 0) {
        // The bound pipeline wasn't built with GraphicsPipelineDesc::instanced -- fall back to the
        // base class's one-draw-per-instance behaviour rather than binding an undeclared root param
        // (which would be an uninitialised root argument on whatever slot happened to sit there).
        AVER_ERROR("[RHI.D3D12] drawMeshInstanced against a pipeline built without "
                   "GraphicsPipelineDesc::instanced; falling back to {} individual draws", instanceCount);
        IRenderContext::drawMeshInstanced(mesh, worlds, instanceCount);
        return;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = ringAlloc(worlds, instanceCount * 16 * static_cast<u32>(sizeof(f32)));
    if (!va) return;   // ring exhausted this frame; ringAlloc already logged it
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    applyDrawBinding();
    dev_->cmdList_->SetGraphicsRootShaderResourceView(static_cast<UINT>(pipe_->instanceWorldParam), va);
    dev_->cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    dev_->cmdList_->IASetIndexBuffer(&m.ibv);
    dev_->cmdList_->DrawIndexedInstanced(m.indexCount, instanceCount, 0, 0, 0);
}

// Draws a device mesh through the mesh-shader path.
void D3D12RenderContext::dispatchMeshFor(MeshHandle mesh) {
    if (!pipe_ || !pipe_->mesh) { AVER_ERROR("[RHI.D3D12] dispatchMeshFor without a mesh-shader pipeline"); return; }
    if (!dev_->cmdList6_) { AVER_ERROR("[RHI.D3D12] DispatchMesh is unavailable on this command list"); return; }
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.D3D12] dispatchMeshFor with an invalid mesh handle"); return; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    const u32 tris = m.indexCount / 3;
    if (!tris) return;
    applyDrawBinding();
    dev_->cmdList_->SetGraphicsRootShaderResourceView(static_cast<UINT>(pipe_->msVertexParam), m.vb->GetGPUVirtualAddress());
    dev_->cmdList_->SetGraphicsRootShaderResourceView(static_cast<UINT>(pipe_->msIndexParam), m.ib->GetGPUVirtualAddress());
    const u32 tc[4] = {tris, 0, 0, 0};
    dev_->cmdList_->SetGraphicsRoot32BitConstants(static_cast<UINT>(pipe_->msCountParam), 4, tc, 0);
    dev_->cmdList6_->DispatchMesh((tris + kMeshShaderTrisPerGroup - 1) / kMeshShaderTrisPerGroup, 1, 1);
}

// Dispatches an amplification+mesh-shader pipeline over one cluster cut. See the declaration in
// RHIResources.hpp for why this is a separate entry point from dispatchMeshFor rather than a second
// meaning bolted onto it. The cluster arrays (MeshletDesc/Bounds/Vertices/Triangles) are whatever
// the caller already bound via setBindingSet/setSrvBuffer before this call -- this function only
// supplies the group count and, when `mesh` names a live mesh, that mesh's plain vertex buffer,
// the same way dispatchMeshFor supplies it, since every cluster's MeshletVertices are global indices
// into that same buffer (FORMAT_SPECS 5.7).
void D3D12RenderContext::dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) {
    if (!pipe_ || !pipe_->mesh || !pipe_->amplification) {
        AVER_ERROR("[RHI.D3D12] dispatchMeshClusters without an amplification-shader pipeline");
        return;
    }
    if (!dev_->cmdList6_) { AVER_ERROR("[RHI.D3D12] DispatchMesh is unavailable on this command list"); return; }
    if (clusterCount == 0) return;
    applyDrawBinding();
    // FIX: the root signature this pipeline was built with (rootSignature(layout, /*mesh=*/true))
    // ALWAYS reserves msVertexParam/msIndexParam/msCountParam as three extra root parameters -- see
    // D3D12ResourceFactory::rootSignature's own "FROZEN: geometry SRVs sit past both declared tables"
    // comment -- regardless of whether a cluster-culling pipeline's own shaders read all three.
    // Leaving any of them unset is an UNINITIALIZED ROOT ARGUMENT: undefined per the D3D12 spec, and
    // exactly the kind of thing this house's own TDR history says to take seriously. The cluster mesh
    // shader never reads the flat index buffer (msIndexParam) or a triangle count (msCountParam,
    // which the geometry convention reserves for exactly that), but this call still binds real,
    // valid addresses to both -- this mesh's own index buffer, and clusterCount itself -- so nothing
    // is ever an unset root argument even though nothing here reads them.
    if (mesh != 0 && mesh <= dev_->meshes_.size() && dev_->meshes_[mesh - 1].alive) {
        const GpuMesh& m = dev_->meshes_[mesh - 1];
        if (pipe_->msVertexParam >= 0)
            dev_->cmdList_->SetGraphicsRootShaderResourceView(static_cast<UINT>(pipe_->msVertexParam), m.vb->GetGPUVirtualAddress());
        if (pipe_->msIndexParam >= 0)
            dev_->cmdList_->SetGraphicsRootShaderResourceView(static_cast<UINT>(pipe_->msIndexParam), m.ib->GetGPUVirtualAddress());
    }
    if (pipe_->msCountParam >= 0) {
        const UINT block[4] = {clusterCount, 0, 0, 0};
        dev_->cmdList_->SetGraphicsRoot32BitConstants(static_cast<UINT>(pipe_->msCountParam), 4, block, 0);
    }
    const u32 groups = (clusterCount + kClusterAmplificationGroupSize - 1) / kClusterAmplificationGroupSize;
    dev_->cmdList6_->DispatchMesh(groups, 1, 1);
}

// Dispatches the bound compute pipeline.
void D3D12RenderContext::dispatch(u32 gx, u32 gy, u32 gz) {
    if (!pipe_ || !pipe_->compute) { AVER_ERROR("[RHI.D3D12] dispatch without a compute pipeline"); return; }
    if (dev_->cmdList_) dev_->cmdList_->Dispatch(gx, gy, gz);
}

// Copies whole bytes between two buffers. Both must already be in the right state.
void D3D12RenderContext::copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes,
                                    u64 dstOffset, u64 srcOffset) {
    if (!dev_->cmdList_ || !res_) return;
    ID3D12Resource* d = res_->bufferResource(dst);
    ID3D12Resource* s = res_->bufferResource(src);
    if (!d || !s) { AVER_ERROR("[RHI.D3D12] copyBuffer with an invalid handle"); return; }
    dev_->cmdList_->CopyBufferRegion(d, dstOffset, s, srcOffset, bytes);
}

// The layout D3D12 requires for a texture<->buffer copy of one mip.
//
// GetCopyableFootprints IS THE AUTHORITY, not arithmetic on width*bpp. D3D12 aligns every copy row
// to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256), so a 16-wide RGBA16F mip carries 128 bytes of data in
// a 256-byte row, and the padding is real: a caller that assumes tight packing reads one row's data
// interleaved with another row's padding. Asking the runtime also means a format whose block size
// this file does not model still gets the right answer.
bool D3D12ResourceFactory::textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const {
    const RhiTexture* tex = const_cast<D3D12ResourceFactory*>(this)->texture(t);
    if (!tex || !tex->res) return false;
    if (mip >= tex->desc.mips) return false;

    const D3D12_RESOURCE_DESC rd = tex->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    dev_->device_->GetCopyableFootprints(&rd, mip, 1, 0, &fp, &rows, &rowBytes, &total);
    if (total == 0) return false;

    out.totalBytes = total;
    out.rowPitch   = fp.Footprint.RowPitch;
    out.rowBytes   = static_cast<u32>(rowBytes);
    out.rows       = rows;
    out.depth      = fp.Footprint.Depth;
    return true;
}

// Copies one mip of a texture into a buffer. The texture must already be in CopySource and the
// buffer in CopyDest -- this issues the copy and nothing else, exactly as copyBuffer does.
void D3D12RenderContext::copyTextureToBuffer(BufferHandle dst, u64 dstOffset, TextureHandle src, u32 mip) {
    if (!dev_->cmdList_ || !res_) return;
    ID3D12Resource* d = res_->bufferResource(dst);
    RhiTexture* s = res_->texture(src);
    if (!d || !s || !s->res) { AVER_ERROR("[RHI.D3D12] copyTextureToBuffer with an invalid handle"); return; }
    if (mip >= s->desc.mips) { AVER_ERROR("[RHI.D3D12] copyTextureToBuffer: mip {} is past the chain", mip); return; }

    const D3D12_RESOURCE_DESC rd = s->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    // The offset is baked into the footprint rather than passed to CopyTextureRegion, which is what
    // D3D12 wants: a PLACED_FOOTPRINT names where in the buffer the image begins.
    dev_->device_->GetCopyableFootprints(&rd, mip, 1, dstOffset, &fp, &rows, &rowBytes, &total);

    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = d;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = s->res.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = mip;
    dev_->cmdList_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// The same copy the other way: a buffer holding one mip's bytes, in the footprint layout, into the
// texture. The buffer must be in CopySource and the texture in CopyDest.
void D3D12RenderContext::copyBufferToTexture(TextureHandle dst, u32 mip, BufferHandle src, u64 srcOffset) {
    if (!dev_->cmdList_ || !res_) return;
    RhiTexture* d = res_->texture(dst);
    ID3D12Resource* s = res_->bufferResource(src);
    if (!d || !d->res || !s) { AVER_ERROR("[RHI.D3D12] copyBufferToTexture with an invalid handle"); return; }
    if (mip >= d->desc.mips) { AVER_ERROR("[RHI.D3D12] copyBufferToTexture: mip {} is past the chain", mip); return; }

    const D3D12_RESOURCE_DESC rd = d->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    dev_->device_->GetCopyableFootprints(&rd, mip, 1, srcOffset, &fp, &rows, &rowBytes, &total);

    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = d->res.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = mip;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = s;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = fp;
    dev_->cmdList_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// Copies one whole texture into another. See IRenderContext::copyTexture for why whole-resource.
void D3D12RenderContext::copyTexture(TextureHandle dst, TextureHandle src) {
    if (!dev_->cmdList_ || !res_) return;
    RhiTexture* d = res_->texture(dst);
    RhiTexture* s = res_->texture(src);
    if (!d || !s) { AVER_ERROR("[RHI.D3D12] copyTexture with an invalid handle"); return; }
    // CHECKED HERE RATHER THAN LEFT TO THE DEBUG LAYER, because a mismatched CopyResource is
    // undefined behaviour on a release runtime -- it does not fail, it corrupts. A caller that gets
    // this wrong should see a log line, not a texture full of another texture's memory.
    if (d->desc.width  != s->desc.width  || d->desc.height != s->desc.height ||
        d->desc.depth  != s->desc.depth  || d->desc.format != s->desc.format ||
        d->desc.mips   != s->desc.mips   || d->desc.dim    != s->desc.dim) {
        AVER_ERROR("[RHI.D3D12] copyTexture between mismatched textures -- refused");
        return;
    }
    dev_->cmdList_->CopyResource(d->res.Get(), s->res.Get());
}


// Draws a fullscreen triangle; the vertex shader builds it from SV_VertexID.
void D3D12RenderContext::drawFullscreen() {
    if (!dev_->cmdList_) return;
    dev_->cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->cmdList_->IASetVertexBuffers(0, 0, nullptr);
    dev_->cmdList_->DrawInstanced(3, 1, 0, 0);
}

// Binds a caller-owned vertex buffer at slot 0.
void D3D12RenderContext::setVertexBuffer(BufferHandle h, u32 stride) {
    if (!dev_->cmdList_) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b) { AVER_ERROR("[RHI.D3D12] setVertexBuffer with an invalid handle"); return; }
    if (stride == 0) { AVER_ERROR("[RHI.D3D12] setVertexBuffer with a zero stride"); return; }
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b->res->GetGPUVirtualAddress();
    v.SizeInBytes = static_cast<UINT>(b->desc.bytes);
    v.StrideInBytes = stride;
    dev_->cmdList_->IASetVertexBuffers(0, 1, &v);
}

// Binds a caller-owned index buffer. 32-bit indices only.
void D3D12RenderContext::setIndexBuffer(BufferHandle h, Format indexFormat) {
    if (!dev_->cmdList_) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b) { AVER_ERROR("[RHI.D3D12] setIndexBuffer with an invalid handle"); return; }
    if (indexFormat != Format::R32Uint) {
        AVER_ERROR("[RHI.D3D12] setIndexBuffer needs Format::R32Uint");
        return;
    }
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b->res->GetGPUVirtualAddress();
    v.SizeInBytes = static_cast<UINT>(b->desc.bytes);
    v.Format = DXGI_FORMAT_R32_UINT;
    dev_->cmdList_->IASetIndexBuffer(&v);
}

// Draws indexed geometry from the currently bound caller-owned buffers.
void D3D12RenderContext::drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    if (!dev_->cmdList_ || indexCount == 0) return;
    applyDrawBinding();
    dev_->cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->cmdList_->DrawIndexedInstanced(indexCount, 1, firstIndex, baseVertex, 0);
}

// Records a bottom-level acceleration structure build for its mesh.
void D3D12RenderContext::buildBlas(BlasHandle h) {
    RhiBlas* b = res_->blas(h);
    if (!b) { AVER_ERROR("[RHI.D3D12] buildBlas with an invalid handle"); return; }
    if (!dev_->cmdList4_ || !dev_->device5_) { AVER_ERROR("[RHI.D3D12] buildBlas without ray-tracing support"); return; }
    if (b->mesh == 0 || b->mesh > dev_->meshes_.size()) return;
    const GpuMesh& m = dev_->meshes_[b->mesh - 1];

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = blasInputs(m, geo);
    bd.ScratchAccelerationStructureData = b->scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = b->as->GetGPUVirtualAddress();
    dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = b->as.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
    b->built = true;
}

// Packs the instance buffer and records a top-level acceleration structure build.
void D3D12RenderContext::buildTlas(TlasHandle h, const TlasInstance* instances, u32 count) {
    RhiTlas* t = res_->tlas(h);
    if (!t) { AVER_ERROR("[RHI.D3D12] buildTlas with an invalid handle"); return; }
    if (!dev_->cmdList4_ || !dev_->device5_) { AVER_ERROR("[RHI.D3D12] buildTlas without ray-tracing support"); return; }
    if (count > t->maxInstances) {
        AVER_WARN("[RHI.D3D12] buildTlas: {} instances clamped to the {} this TLAS was sized for", count, t->maxInstances);
        count = t->maxInstances;
    }
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    if (!t->instancePtr[f]) return;

    auto* dst = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(t->instancePtr[f]);
    u32 written = 0;
    for (u32 i = 0; i < count && instances; ++i) {
        const RhiBlas* b = res_->blas(instances[i].blas);
        if (!b || !b->as) { AVER_WARN("[RHI.D3D12] buildTlas: instance {} names an invalid BLAS", i); continue; }
        D3D12_RAYTRACING_INSTANCE_DESC id{};
        // Engine matrices are row-major / row-vector (v*M); DXR wants a 3x4 column-vector [R|T].
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) id.Transform[r][c] = instances[i].world[c * 4 + r];
            id.Transform[r][3] = instances[i].world[12 + r];
        }
        id.InstanceMask = instances[i].mask;
        // Rejected rather than truncated: InstanceID is a 24-bit bitfield, so a larger value would
        // silently alias onto another instance's id and a hit would resolve to the wrong geometry.
        if (instances[i].instanceId > kMaxTlasInstanceId) {
            AVER_ERROR("[RHI.D3D12] buildTlas: instance {} has id {} which does not fit in 24 bits; "
                       "it is dropped rather than aliased onto another instance",
                       i, instances[i].instanceId);
            continue;
        }
        id.InstanceID = instances[i].instanceId;
        id.AccelerationStructure = b->as->GetGPUVirtualAddress();
        dst[written++] = id;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = written;
    in.InstanceDescs = t->instances[f]->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.ScratchAccelerationStructureData = t->scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = t->as->GetGPUVirtualAddress();
    dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = t->as.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
}

namespace {
// True when a barrier names the terminal AccelerationStructure state, which D3D12 rejects.
bool rejectAsState(ResourceState from, ResourceState to, const char* what) {
    if (from != ResourceState::AccelerationStructure && to != ResourceState::AccelerationStructure) return false;
    AVER_ERROR("[RHI.D3D12] {}: AccelerationStructure is terminal and cannot be transitioned", what);
    return true;
}

// Checks a texture barrier's claimed `from` against the debug shadow state, and updates it.
void trackTextureBarrier(RhiTexture& t, ResourceState from, ResourceState to, u32 subresource) {
#if AVER_RHI_TRACK_STATE
    const char* name = t.debugName.empty() ? "<unnamed>" : t.debugName.c_str();
    const u32 mips = static_cast<u32>(t.states.size());
    if (subresource == kAllSubresources) {
        for (u32 m = 0; m < mips; ++m) {
            if (t.states[m] == from) continue;
            AVER_ERROR("[RHI.D3D12] barrier on '{}': whole-resource transition claims {} but mip {} is in {}",
                       name, stateName(from), m, stateName(t.states[m]));
            break;
        }
        for (ResourceState& s : t.states) s = to;
        return;
    }
    if (subresource >= mips) {
        AVER_ERROR("[RHI.D3D12] barrier on '{}': subresource {} past the {} mips it has", name, subresource, mips);
        return;
    }
    if (t.states[subresource] != from)
        AVER_ERROR("[RHI.D3D12] barrier on '{}': mip {} claims {} but is in {}",
                   name, subresource, stateName(from), stateName(t.states[subresource]));
    t.states[subresource] = to;
#else
    (void)t; (void)from; (void)to; (void)subresource;
#endif
}

// Checks a buffer barrier's claimed `from` against the debug shadow state, and updates it.
void trackBufferBarrier(RhiBuffer& b, ResourceState from, ResourceState to) {
#if AVER_RHI_TRACK_STATE
    const char* name = b.debugName.empty() ? "<unnamed>" : b.debugName.c_str();
    if (b.stateFixed) {
        AVER_ERROR("[RHI.D3D12] barrier on '{}': an upload or acceleration-structure buffer cannot be transitioned", name);
        return;
    }
    if (b.state != from)
        AVER_ERROR("[RHI.D3D12] barrier on '{}': claims {} but is in {}", name, stateName(from), stateName(b.state));
    b.state = to;
#else
    (void)b; (void)from; (void)to;
#endif
}
} // namespace

// Records a texture transition barrier for one mip, or for the whole resource.
void D3D12RenderContext::textureBarrier(TextureHandle h, ResourceState from, ResourceState to, u32 subresource) {
    if (rejectAsState(from, to, "textureBarrier")) return;
    RhiTexture* t = res_->texture(h);
    if (!t || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] textureBarrier with an invalid handle"); return; }
    trackTextureBarrier(*t, from, to, subresource);
    D3D12_RESOURCE_BARRIER b = transition(t->res.Get(), toResourceStates(from), toResourceStates(to));
    b.Transition.Subresource = (subresource == kAllSubresources) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : subresource;
    dev_->cmdList_->ResourceBarrier(1, &b);
}

// Records a buffer transition barrier.
void D3D12RenderContext::bufferBarrier(BufferHandle h, ResourceState from, ResourceState to) {
    if (rejectAsState(from, to, "bufferBarrier")) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] bufferBarrier with an invalid handle"); return; }
    trackBufferBarrier(*b, from, to);
    D3D12_RESOURCE_BARRIER bar = transition(b->res.Get(), toResourceStates(from), toResourceStates(to));
    dev_->cmdList_->ResourceBarrier(1, &bar);
}

// Records a UAV barrier on a texture.
void D3D12RenderContext::uavBarrierTexture(TextureHandle h) {
    RhiTexture* t = res_->texture(h);
    if (!t || !dev_->cmdList_) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = t->res.Get();
    dev_->cmdList_->ResourceBarrier(1, &b);
}

// Records a UAV barrier on a buffer.
void D3D12RenderContext::uavBarrierBuffer(BufferHandle h) {
    RhiBuffer* b = res_->buffer(h);
    if (!b || !dev_->cmdList_) return;
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = b->res.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
}

// Opens a debug marker region. Metadata 1 is the ANSI-string form PIX and RenderDoc understand.
// Also opens a GPU timing span nested under whatever is already open -- pushMarker/popMarker are
// the mechanism ScopedGpuStat (RHIResources.hpp) wraps, so a caller that used to write a bare
// pushMarker/popMarker pair by hand should prefer that instead; this pair remains for callers a
// single C++ scope cannot cover (see beginGpuSpan's own comment on the one place that is true here).
void D3D12RenderContext::pushMarker(const char* label) {
    if (!label || !dev_->cmdList_) return;
    dev_->cmdList_->BeginEvent(1, label, static_cast<UINT>(std::strlen(label) + 1));
    // The label is a string LITERAL at every call site, so storing the pointer is safe and keeps
    // this allocation-free on the hot path -- see GpuSpan's own comment.
    //
    // Parent, same rule as beginGpuSpan: whatever is already open when this marker opens, kNoParent
    // if this is the first one -- a top-level marker such as VoxiRenderer::prePass's outer "Voxi GI
    // update" scope, opened before anything else this frame has pushed.
    if (dev_->tsSlice_[dev_->frameIndex_].size() >= D3D12Device::kMaxGpuSpans) {
        ++dev_->tsDropped_;   // see tsDropped_: popMarker consumes this instead of popping
        return;
    }
    const u32 parent = dev_->tsOpen_.empty() ? D3D12Device::kNoParent : dev_->tsOpen_.back();
    dev_->tsSlice_[dev_->frameIndex_].push_back(
        {label, dev_->gpuStamp(), D3D12Device::kMaxGpuStamps, parent});
    dev_->tsOpen_.push_back(static_cast<u32>(dev_->tsSlice_[dev_->frameIndex_].size() - 1));
}

void D3D12RenderContext::popMarker() {
    if (!dev_->cmdList_) return;
    if (dev_->tsDropped_) { --dev_->tsDropped_; dev_->cmdList_->EndEvent(); return; }
    if (!dev_->tsOpen_.empty()) {
        const u32 i = dev_->tsOpen_.back();
        dev_->tsOpen_.pop_back();
        dev_->tsSlice_[dev_->frameIndex_][i].end = dev_->gpuStamp();
    }
    dev_->cmdList_->EndEvent();
}

} // namespace

namespace d3d12 {
// See UiBackend.hpp for the full contract. Defined here, not there, because only this translation
// unit has D3D12Device's full definition to reach through the static_cast below -- the header only
// ever hands out a pointer through IDevice, never the concrete type.
bool installUiBackend(IDevice* device, IUiBackend* backend) {
    if (!device || device->backend() != Backend::D3D12) return false;
    static_cast<D3D12Device*>(device)->setUiBackend(backend);
    return true;
}
} // namespace d3d12

namespace detail {
// Creates and initialises the D3D12 device, or returns null.
IDevice* createD3D12Device(const DeviceDesc& desc) {
    auto* dev = new D3D12Device();
    if (!dev->init(desc)) { delete dev; return nullptr; }
    return dev;
}
} // namespace detail

} // namespace aver::rhi
