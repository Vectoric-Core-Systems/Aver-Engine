// DirectX 12 backend for Aver.RHI — device, swapchain, depth buffer, a lit-mesh
// pipeline (HLSL Lambert shading compiled at runtime), immediate draw path, an
// offscreen GPU self-test, and a backbuffer capture for verification. Hand-rolled
// D3D12 structs (no d3dx12.h). Falls back to nullptr if D3D12 is unavailable.
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>      // DXC: shader model 6.x (mesh shaders, DXR RayQuery)
#include <string>
#include <wrl/client.h>

#include <cmath>     // the post chain's inverse tonemap and its adaptation curve
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

// The SCENE target's format, which is no longer the backbuffer's.
//
// Everything the scene draws is linear radiance now, and the tonemap that turns it into a display
// image is the last pass of the frame (see rhi::PostSettings). Eight bits of gamma-encoded colour
// cannot carry that: the sun disk is worth ~14, a specular highlight several times white, and both
// would clamp to 1 at the moment they were written -- taking with them exactly the range bloom and
// eye adaptation exist to read.
//
// RGBA16F rather than R11G11B10F because the alpha channel is load-bearing (averOpacity), and
// rather than RGBA32F because f16 already has more precision than the 8-bit backbuffer this
// resolves into and costs half the bandwidth.
constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// The bloom pyramid's depth cap. Six halvings from half resolution reaches a 32-pixel-wide level on
// a 4K frame, which is already wider than any halo anybody wants; more levels would cost dispatches
// to blur something the composite then samples at one texel.
constexpr u32 kMaxBloomMips = 6;
// Descriptor triples the post heap holds: prefilter, histogram, composite, then one per downsample
// and one per upsample. Laid out as (t0,t1,t2) runs because a root descriptor TABLE has to be
// contiguous, and building them all once at resize means no descriptor is ever written while a
// previous frame might still be reading it.
constexpr u32 kPostTripleCount = 3 + (kMaxBloomMips - 1) * 2;
constexpr u32 kPostDescriptorCount = kPostTripleCount * 3 + 2;   // + the histogram/exposure UAVs
// Which triple is which. The bloom ones are ranges based at these.
constexpr u32 kPostTriplePrefilter = 0;
constexpr u32 kPostTripleHistogram = 1;
constexpr u32 kPostTripleComposite = 2;
constexpr u32 kPostTripleDownBase  = 3;
constexpr u32 kPostTripleUpBase    = kPostTripleDownBase + (kMaxBloomMips - 1);
// The luminance window the histogram bins over, in log2. -10 is starlight and +12 is a bright sky;
// anything outside is clamped into the end bins, which is the correct behaviour for a statistic.
constexpr f32 kHistogramMinLogLum = -10.0f;
constexpr f32 kHistogramMaxLogLum = 12.0f;
// One thread per FOUR pixels each way. See CSHistogram.
constexpr u32 kHistogramDownscale = 4;

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
        // `--force-caps no-dxc` has to be honoured HERE, not only in the reported caps: a device
        // that claims no DXC while still compiling DXIL exercises nothing. This is the one place
        // the override does more than subtract from a number, and it is still only a removal.
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

    // `target51` is the FXC target, e.g. "vs_5_1"; the SM6 equivalent is derived from it.
    // `sm6` overrides that (e.g. "ps_6_5" for RayQuery) and requires DXC, so a caller using it must
    // have checked the device caps first. `define` is a semicolon-separated list of -D macros
    // ("AVER_MS=1;AVER_RT=1") and does NOT: the material prelude tells every raster shader which
    // registers its tables landed at, so a compiler that could not take macros could not build the
    // scene at all. That is why FXC gets them too.
    HRESULT compile(const char* src, const char* entry, const char* target51, ID3DBlob** out,
                    const char* sm6 = nullptr, const char* define = nullptr) {
        init();
        // "A=1;B=2" -> the two shapes the two compilers want. Split once, here, so the macro list
        // cannot drift between them.
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
            if (sm6) return E_NOTIMPL;   // SM6-only path; caller must fall back
            // D3D_SHADER_MACRO wants name and value as separate pointers, so each "A=1" is split
            // in place and both halves have to outlive the D3DCompile call.
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
        // "vs_5_1" -> "vs_6_0". Stages that need a higher model pass it in explicitly.
        std::string t6(target51);
        const size_t us = t6.rfind("_5_1");
        if (us != std::string::npos) t6 = t6.substr(0, us) + "_6_0";
        if (sm6) t6 = sm6;
        const std::wstring wEntry(entry, entry + std::strlen(entry));
        const std::wstring wTarget(t6.begin(), t6.end());
        // Keep each macro alive for the duration of the Compile call.
        std::vector<std::wstring> wDefines;
        for (const std::string& d : defs) wDefines.emplace_back(d.begin(), d.end());

        DxcBuffer buf{src, std::strlen(src), DXC_CP_UTF8};
        std::vector<LPCWSTR> args = {
            L"-E", wEntry.c_str(),
            L"-T", wTarget.c_str(),
            L"-Zpr",           // pack matrices row-major: the engine's convention
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

// ImGui 1.92 owns a *list* of atlas textures, not one. When the atlas is rebuilt -- because the
// editor reloaded its font at a new DPI, or simply because enough new glyphs were baked on demand
// to outgrow the current sheet -- the replacement is created while the outgoing texture is still
// referenced by in-flight draw data, and only released a frame or two later. The backend's legacy
// single-descriptor mode asserts on exactly that overlap, so the UI heap holds a small pool and
// hands descriptors out through these callbacks instead.
static constexpr u32 kUiSrvCount = 16;

namespace {
struct UiSrvPool {
    ID3D12DescriptorHeap* heap = nullptr;
    u64 cpuBase = 0;
    u64 gpuBase = 0;
    u32 stride = 0;
    bool used[kUiSrvCount] = {};
};
UiSrvPool g_uiSrv;
} // namespace

static void uiSrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                       D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    for (u32 i = 0; i < kUiSrvCount; ++i) {
        if (g_uiSrv.used[i]) continue;
        g_uiSrv.used[i] = true;
        cpu->ptr = static_cast<SIZE_T>(g_uiSrv.cpuBase + u64(i) * g_uiSrv.stride);
        gpu->ptr = g_uiSrv.gpuBase + u64(i) * g_uiSrv.stride;
        return;
    }
    // Never expected: ImGui keeps at most a couple of atlas textures alive at once.
    AVER_ERROR("[RHI.D3D12] UI SRV descriptor pool exhausted ({} in use)", kUiSrvCount);
    cpu->ptr = 0; gpu->ptr = 0;
}

static void uiSrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                      D3D12_GPU_DESCRIPTOR_HANDLE) {
    if (!g_uiSrv.stride || cpu.ptr < g_uiSrv.cpuBase) return;
    const u64 slot = (u64(cpu.ptr) - g_uiSrv.cpuBase) / g_uiSrv.stride;
    if (slot < kUiSrvCount) g_uiSrv.used[slot] = false;
}
#endif

// The tail of the per-draw b1 block: shading model, then its parameters. drawMesh has no material
// to hand over yet, so both paths write the same defaults, and they write them from ONE place --
// two copies that disagreed would shade the feature-overridden path differently from the backend's
// own with nothing to report it.
//
// The defaults are the values the shading maths used to hardcode: 0.04 dielectric reflectance,
// f90 = 1, no emission.
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD in the material prelude
    std::memcpy(block + 24, &model, sizeof(model));   // a uint in the block, not a converted float
    block[25] = 0.04f;
    block[26] = 1.0f;
    block[27] = 0.0f;
    block[28] = block[29] = block[30] = block[31] = 0.0f;   // emissive
}

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
// ---- scene pixel shader ----
// PSMainPlain, and only PSMainPlain: unshadowed, with no bounce. That is the whole of the shading
// this backend owns, because sun visibility and indirect radiance are things the material contract
// asks a RENDERER for rather than terms a material computes. A render feature that wants either
// supplies its own pixel shader and its own pipeline; the backend keeping a second copy is what
// step 11 of the refactor removed.
//
// It shades through plainShadeSurface, the prelude's FROZEN copy, and not through the Aver material
// contract: linking Aver.Render.PBR.Materials from a backend would invert the layering, and this
// path is unreachable in any build that has a render feature, so a substitute written here would
// diverge with nothing to catch it.
float4 PSMainPlain(VSOut i) : SV_TARGET { return plainShadeSurface(i, 1.0, float3(0,0,0), 1.0); }

// ---- procedural sky (fullscreen triangle via SV_VertexID) ----
float4 PSky(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float3 L = normalize(gLightDir.xyz);
    // THE ONLY PLACE THE FULL SCATTERING INTEGRAL RUNS. Everything else in the engine reads the
    // two-colour dome, which the physical model fits on the CPU each frame -- so a physical sky
    // costs one fullscreen march and nothing at all per shaded surface.
    float3 sky = averAtmoOn() ? averSkyPhysical(ray) : skyColor(ray);
    float sd = saturate(dot(ray, L));
    float3 sunC = srgbToLin(gLightColor.rgb) * gSkyParams.z;
    // The DISK is a hard-edged test against the authored angular radius, not a power curve: half a
    // degree is what the sun actually subtends, and a pow() large enough to look that tight is one
    // that aliases into a flickering dot as the camera turns. The soft shoulder is the last term.
    float cosR = gSkyParams.w;
    float disk = smoothstep(cosR - 0.0004, cosR + 0.0002, sd);
    sky += sunC * disk * 14.0;
    // The glow around it. Authored as a power curve, because the authored dome has no air in it to
    // scatter; under the physical model the Mie forward lobe already IS that glow, and adding this
    // on top would be counting the same light twice.
    if (!averAtmoOn()) sky += sunC * pow(sd, 12.0) * 0.30;

    // Clouds LAST, so they occlude the sun disk and the glow rather than being lit through them.
    // Costs nothing at all when the layer is off or the ray never enters it.
    float4 cloud = averCloudLayer(gCamPos.xyz, ray, L, sunC);
    sky = sky * cloud.a + cloud.rgb;

    // Linear radiance; the post chain tonemaps. The sun disk in particular is worth far more than 1
    // here, and clamping it to a display value at this point is exactly what would stop it blooming.
    return float4(sky, 1.0);
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
// A line's colour is authored as the pixel it wants ON SCREEN — grid greys, gizmo axis colours — so
// it is a DISPLAY value, not radiance. The scene target is HDR now and the frame is tonemapped at
// the end, so the value written here is the pre-image of that colour under the whole chain: decode
// the gamma the target no longer applies, then undo the tonemap that will be applied.
//
// It rides the camera's exposure with everything else rather than being divided back out. That
// keeps the DEFAULT (exposure 1) exactly what it always was, which is what the pixel oracle
// measures, and under eye adaptation an editor overlay that ignored exposure would be the one thing
// in the viewport that did.
float4 PSLine(LVSOut i) : SV_TARGET { return float4(averInverseTonemap(srgbToLin(i.col)), 1.0); }
)";

// The shared prelude and this backend's own shaders form ONE translation unit; every entry point
// below is compiled from the concatenation. Joined once and cached because the two halves are
// immutable and the compile sites are scattered across pipeline creation.
const std::string& sceneShaderSource() {
    static const std::string src = std::string(sharedShaderPrelude()) + kShaderHLSL;
    return src;
}

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
    // ---- the authored atmosphere (rhi::SkyAtmosphere). MIRRORED in the shared prelude's
    // `cbuffer PerFrame`, field for field, with the same no-compiler-behind-it warning as above.
    f32 skyParams[4];    // x atmosphere height, y sky-light intensity, z sun intensity, w cos(sun angular radius)
    f32 groundColor[4];  // rgb ground albedo below the horizon
    f32 fogParams[4];    // x height falloff, y fog height, z start distance, w max opacity
    f32 cloudParams[4];  // x coverage, y density, z layer bottom, w layer top
    f32 cloudMotion[4];  // xy wind offset in world units, z 1/feature size, w enabled
    // ---- the physical atmosphere (rhi::AtmosphereProfile), mirrored in the prelude the same way.
    f32 atmoRayleigh[4]; // rgb scattering per km, w scale height km
    f32 atmoMie[4];      // x scatter, y extinction, z scale height km, w phase g
    f32 atmoOzone[4];    // rgb absorption per km, w tent half-width km
    f32 atmoPlanet[4];   // x planet radius km, y atmosphere top radius km, z world->km, w on/off
    f32 atmoTune[4];     // x ozone centre km, y multi-scatter gain, z view steps, w aerial steps
    f32 atmoSunE0[4];    // rgb sun irradiance above the air, w ground albedo
};

// MIRRORS `cbuffer AverPost : register(b0)` in rhi::postShaderSource() FIELD FOR FIELD. Same
// treatment as PerFrameCB above and for the same reason: there is no compiler behind this, and a
// mismatch shades plausibly with the wrong parameters rather than failing.
//
// One block for the whole chain. A pass reads the fields it needs and ignores the rest, which is
// what lets seven passes share one root signature and one suballocation each.
struct PostCB {
    f32 tone[4];    // exposure, bloom intensity, bloom threshold, bloom knee
    f32 dst[4];     // destination width, height, 1/width, 1/height
    f32 src[4];     // source width, height, 1/width, 1/height
    f32 adapt[4];   // min log2 luminance, 1/log2 range, adaption alpha, unused
    f32 limit[4];   // exposure min, exposure max, histogram low cut, high cut
    f32 misc[4];    // middle grey, auto-exposure on, bloom filter radius, unused
};
static_assert(sizeof(PostCB) == 96, "the HLSL cbuffer mirrors this byte for byte");

// Per-frame upload for the block above. Sixteen passes at 256-byte alignment is 4 KB; the ring is
// sized well past that so a deeper pyramid never has to think about it.
constexpr u32 kPostConstantRingBytes = 16 * 1024;

// The ONE description of rhi::MeshVertex to D3D12. Every pipeline that reads geometry through the
// input assembler shares it -- the backend's scene pipelines and the generic factory's alike -- so
// adding a field to MeshVertex is a single edit here rather than a hunt through five PSO builders
// where a missed one fails at draw time with nothing naming the cause.
constexpr D3D12_INPUT_ELEMENT_DESC kMeshInputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
constexpr UINT kMeshInputLayoutCount = sizeof(kMeshInputLayout) / sizeof(kMeshInputLayout[0]);

// Root parameter indices for the backend's own two signatures. Named because the recording side
// passes them as bare integers to SetGraphicsRoot*, where a stale number binds the wrong parameter
// instead of failing -- the failure mode that cost this project a debugging session already.
constexpr UINT kSceneFrameParam  = 0;   // b0, the engine per-frame block
constexpr UINT kSceneObjectParam = 1;   // b1, kObjectConstantDwords root constants
// The mesh-shader signature repeats those two and appends the geometry the input assembler would
// otherwise have fetched. Only the PARAMETER indices live here; the registers come from the base
// below, which is also what the prelude is told through -D, so the two cannot drift apart.
constexpr UINT kMeshVertexParam = 2;
constexpr UINT kMeshIndexParam  = 3;
constexpr UINT kMeshCountParam  = 4;    // b5, triangle count
// This signature declares no SRV table at all, so any base is legal; it keeps its historical 3 so
// the change that introduced the -D macros moved no bytecode. Vertices at t(base), indices at
// t(base+1) — the same shape rootSignature() derives from layout.srvCount for feature pipelines.
constexpr UINT kSceneMeshSrvBase = 3;

struct GpuMesh {
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    u32 indexCount = 0;
};

struct GpuLineMesh {
    ComPtr<ID3D12Resource> vb;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    u32 count = 0;
};

// ---------------------------------------------------------------- generic RHI mapping
// Aver.RHI's own format/state vocabulary translated into D3D12's. Nothing in the existing frame
// path speaks these enums; they exist for the generic resource factory further down.

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

// A typeless depth resource is written through a DSV naming D32_FLOAT and read through an SRV
// naming R32_FLOAT; naming the typeless format in either view is rejected outright. So the two
// directions get their own mapping rather than one shared one.
DXGI_FORMAT toDxgiSrvFormat(Format f) {
    return (f == Format::R32Typeless || f == Format::D32Float) ? DXGI_FORMAT_R32_FLOAT : toDxgiFormat(f);
}
DXGI_FORMAT toDxgiDsvFormat(Format f) {
    return (f == Format::R32Typeless || f == Format::D32Float) ? DXGI_FORMAT_D32_FLOAT : toDxgiFormat(f);
}

// A caller-declared vertex layout (GraphicsPipelineDesc::vertexLayout) to D3D12's input elements.
// The semantic NAME is a string literal, so the pointer outlives the D3D12_INPUT_LAYOUT_DESC that
// borrows it. The ELEMENT ARRAY does not, which is why it is an out parameter the caller keeps alive
// across CreateGraphicsPipelineState rather than a temporary returned by value.
const char* semanticName(VertexSemantic s) {
    switch (s) {
        case VertexSemantic::Position: return "POSITION";
        case VertexSemantic::Normal:   return "NORMAL";
        case VertexSemantic::TexCoord: return "TEXCOORD";
        case VertexSemantic::Color:    return "COLOR";
    }
    return "POSITION";
}

UINT buildInputLayout(const VertexLayout& l, D3D12_INPUT_ELEMENT_DESC (&out)[kMaxVertexAttribs]) {
    UINT n = 0;
    for (u32 i = 0; i < l.attribCount && i < kMaxVertexAttribs; ++i) {
        const VertexAttrib& a = l.attribs[i];
        const DXGI_FORMAT f = toDxgiFormat(a.format);
        if (f == DXGI_FORMAT_UNKNOWN) {
            // Dropped rather than passed through: D3D12 rejects UNKNOWN in an input element, and its
            // rejection names the element index, not the attribute the caller actually wrote.
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

bool isDepthFormat(Format f) { return f == Format::D32Float || f == Format::R32Typeless; }

// Bytes per texel, for interpreting the CALLER's rows only. The destination pitch is never computed
// from this — GetCopyableFootprints is the only authority on that, and assuming the two match is the
// classic texture-upload bug.
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
        default:                     break;   // block formats have no texel size, by construction
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

// Tightly packed bytes in one source row of a surface this wide — which for a block format is one
// row of BLOCKS covering four texel rows. The upload loop copies fp.Footprint row units, and those
// units are block rows for a block format, so the source stride must be counted the same way or
// every mip after the first is read from the wrong offset.
u64 packedRowPitch(Format f, u32 widthTexels) {
    if (const u32 bb = blockBytes(f)) return u64((widthTexels + 3) / 4) * bb;
    return u64(widthTexels) * texelBytes(f);
}

// AccelerationStructure maps here for completeness (a buffer is CREATED in it), but it is terminal:
// the barrier entry points reject it before they ever get this far.
D3D12_RESOURCE_STATES toResourceStates(ResourceState s) {
    switch (s) {
        case ResourceState::ShaderResource:         return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case ResourceState::NonPixelShaderResource: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ResourceState::UnorderedAccess:        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::RenderTarget:           return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::DepthWrite:             return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::CopySource:             return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::CopyDest:               return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::AccelerationStructure:  return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        case ResourceState::Common:                 break;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

D3D12_COMPARISON_FUNC toComparison(CompareOp op) {
    switch (op) {
        case CompareOp::Less:      return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Always:    return D3D12_COMPARISON_FUNC_ALWAYS;
        case CompareOp::Never:     break;
    }
    return D3D12_COMPARISON_FUNC_NEVER;
}

D3D12_FILTER toFilter(Filter f) {
    switch (f) {
        case Filter::Point:  return D3D12_FILTER_MIN_MAG_MIP_POINT;
        case Filter::Linear: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        // Hardware PCF. Mip-point rather than mip-linear because a comparison sampler reads one
        // level of a shadow map, and mip-linear would silently blend two of them.
        case Filter::ComparisonLinear: return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        case Filter::Anisotropic: return D3D12_FILTER_ANISOTROPIC;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D12_TEXTURE_ADDRESS_MODE toAddress(AddressMode a) {
    return a == AddressMode::Wrap ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

// Per-subresource state tracking exists only in debug builds: it is a correctness net, not a
// runtime feature, and every barrier would otherwise pay for a lookup it never needs in shipping.
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

    Backend backend() const override { return Backend::D3D12; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }

    // ----- generic RHI surface (render-feature modules) -----
    // Derived from the DXGI constants rather than written out again, so the two can never drift.
    //
    // This is the SCENE colour format, which is what the interface asks for ("target formats a
    // feature must match when building pipelines that draw into the scene") and is no longer the
    // swapchain's: the scene renders HDR and the post chain resolves it to the 8-bit backbuffer.
    // A feature building its scene PSO against the swapchain's format would fail at draw time with
    // an RTV/PSO format mismatch, a long way from here.
    Format backbufferFormat() const override { return fromDxgiFormat(kSceneColorFormat); }
    Format depthFormat() const override { return fromDxgiFormat(kDepthFormat); }
    IResourceFactory* resources() override;
    // NON-owning. Registering the same feature twice would double every hook, so it is ignored.
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    void notifyRenderTargetsChanged();
    // The scene-as-a-texture path. Created through the FACTORY rather than as another device-owned
    // ComPtr, so it is an ordinary TextureHandle with an RTV and an SRV the UI can already name --
    // uiTextureId works on it unchanged, and nothing here grows a second resource lifetime to get
    // wrong.
    bool ensureViewportTexture();
    bool           viewportToTex_ = false;
    TextureHandle  viewportTex_ = 0;
    u32            viewportTexW_ = 0, viewportTexH_ = 0;
    // What the features were last told. Seeded with a sample count of 0, which no device ever
    // reports, so the first notification always goes out however the formats compare.
    u32    notifiedSamples_ = 0;
    Format notifiedColor_   = Format::Unknown;
    Format notifiedDepth_   = Format::Unknown;
    u32 sampleCount() const override { return sampleCount_; }
    bool setSampleCount(u32 samples) override;

    ISwapchain* createSwapchain(const SwapchainDesc& d) override {
        if (!createSwapchainResources(d)) return nullptr;
        return new D3D12Swapchain(this);
    }

    void setClearColor(f32 r, f32 g, f32 b, f32 a) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; clear_[3] = a; }
    // Takes effect on the NEXT present, with no swapchain rebuild: the tearing capability was baked
    // in at creation and this only chooses whether to use it.
    void setVSync(bool on) override { vsync_ = on; }
    bool vsync() const override { return vsync_; }
    bool vsyncCanDisable() const override { return tearingSupported_; }

    void setViewportToTexture(bool on) override { viewportToTex_ = on; }
    bool viewportToTexture() const override { return viewportToTex_; }
    u64  viewportTextureId() override;

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
    bool camera(f32 viewProj[16], f32 invViewProj[16], f32 cameraPos[3]) const override {
        if (viewProj)    std::memcpy(viewProj, frameCB_.viewProj, sizeof(frameCB_.viewProj));
        if (invViewProj) std::memcpy(invViewProj, frameCB_.invViewProj, sizeof(frameCB_.invViewProj));
        if (cameraPos)   std::memcpy(cameraPos, frameCB_.camPos, 3 * sizeof(f32));
        return true;
    }
    void setLight(const f32 dir[3], const f32 color[3], f32 ambient) override {
        frameCB_.lightDir[0] = dir[0]; frameCB_.lightDir[1] = dir[1]; frameCB_.lightDir[2] = dir[2]; frameCB_.lightDir[3] = 0;
        frameCB_.lightColor[0] = color[0]; frameCB_.lightColor[1] = color[1]; frameCB_.lightColor[2] = color[2]; frameCB_.lightColor[3] = 0;
        frameCB_.ambient[0] = frameCB_.ambient[1] = frameCB_.ambient[2] = ambient; frameCB_.ambient[3] = 0;
    }
    void setSkyAtmosphere(const SkyAtmosphere& s) override;
    SkyAtmosphere skyAtmosphere() const override { return sky_; }
    // The physical half of the pack, and the four authored fields it derives instead. Split out
    // because it is the only part that computes rather than copies.
    void packAtmosphere(const SkyAtmosphere& s);
    void setPostProcess(const PostSettings& p) override { post_ = p; }
    PostSettings postProcess() const override { return post_; }

    MeshHandle createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) override;
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
    void setLineDepth(bool testDepth) override { lineDepth_ = testDepth; }
    void setMeshShaders(bool enabled) override {
        const bool want = enabled && msSupported_;
        if (want != msActive_) AVER_INFO("[RHI.D3D12] geometry path: {}", want ? "mesh shaders" : "input assembler");
        // A refused request used to be indistinguishable from one that was never made: the path
        // simply stayed on the input assembler and nothing said why. Named once, with the capability
        // that is actually missing, because "mesh shaders are off" and "this GPU cannot" are
        // different facts and only the second is the user's to act on.
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
    // Block until the fence reaches `value`; false only when the device has been removed. See the
    // definition for why this is not a bounded wait.
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
    // The value a colour authored as a DISPLAY pixel has to be written as, now that the scene
    // target is HDR and the frame is tonemapped at the end. Exactly the CPU twin of the shared
    // prelude's averInverseTonemap(srgbToLin(c)); see PSLine for why it exists.
    static void toSceneReferred(const f32 display[4], f32 out[4]);

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
    // Per-draw binding table 1 plus its b2 block (setDrawBinding). Copied, because callers build
    // the block on the stack per draw. `defaultDrawBinding_` is what beginFrame resets to, so a
    // draw that names no per-draw resources gets the identity set rather than the previous draw's.
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

    bool skyEnabled_ = false;
    // The authored atmosphere, kept whole so skyAtmosphere() can hand it back. The frame block above
    // is the PACKED form the shader reads; this is the form a person edits.
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    bool lineDepth_ = true;
    std::vector<GpuLineMesh> lineMeshes_;
    ComPtr<ID3D12Resource> frameCBs_[kFrameCount];
    u8* frameCBPtr_[kFrameCount] = {nullptr, nullptr};

    // ---- camera post chain ------------------------------------------------------------------
    PostSettings post_{};
    // The MSAA resolve destination. Null when sampleCount_ == 1, where msaaColor_ IS the resolved
    // scene and a copy would be pure bandwidth.
    ComPtr<ID3D12Resource> sceneResolved_;
    ComPtr<ID3D12Resource> bloomTex_;            // half-res RGBA16F pyramid
    u32 bloomMips_ = 0, bloomW_ = 0, bloomH_ = 0;
    // Per-mip state, tracked because the pyramid walks up and down through its own subresources and
    // a barrier from the wrong state is a debug-layer error rather than a visible one.
    D3D12_RESOURCE_STATES bloomState_[kMaxBloomMips] = {};
    ComPtr<ID3D12Resource> histBuf_, expBuf_;    // 256-bin histogram, and the one adapted exposure
    bool expSeeded_ = false;
    ComPtr<ID3D12DescriptorHeap> postRtvHeap_;   // one RTV per bloom mip
    ComPtr<ID3D12DescriptorHeap> postSrvHeap_;   // shader-visible: the SRV triples + the UAV pair
    ComPtr<ID3D12RootSignature> postRootSig_;
    ComPtr<ID3D12PipelineState> bloomPrefilterPso_, bloomDownPso_, bloomUpPso_;
    // [bloom on][auto-exposure on]. Permutations rather than a uniform branch: this is the one pass
    // that touches every pixel of every frame, and the off variants must not sample a bloom texture
    // that was never built.
    ComPtr<ID3D12PipelineState> compositePso_[2][2];
    ComPtr<ID3D12PipelineState> histogramPso_, exposurePso_;
    ComPtr<ID3D12Resource> postCBs_[kFrameCount];
    u8* postCBPtr_[kFrameCount] = {nullptr, nullptr};
    u32 postCBUsed_ = 0;
    u32 postSrvSize_ = 0, postRtvSize_ = 0;
    bool postReady_ = false;
    // Wall-clock seconds since the previous endFrame, for the exposure adaptation. Measured here
    // rather than passed in: the device is the only thing that knows when a frame actually ended,
    // and threading a delta through IDevice for one feedback loop would put a clock in the
    // interface every backend then has to be trusted with.
    i64 lastFrameTick_ = 0;
    f32 frameSeconds_ = 1.0f / 60.0f;

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

    // ---- Debug layer message drain ----
    // The debug layer writes to the Win32 debug output, which nothing outside a debugger reads.
    // For an automated fallback matrix that is the same as not having it: a Tier-1 violation or
    // an unbound descriptor would be reported to nobody. Draining the info queue into the
    // engine's own log puts it in the run's captured output, so "zero validation errors" becomes
    // a measurement instead of an assumption.
    ComPtr<ID3D12InfoQueue> infoQueue_;
    std::vector<u32> seenMessageIds_;   // first occurrence only; the totals carry the rest
    u32 dbgCorruption_ = 0, dbgError_ = 0, dbgWarning_ = 0;
    void drainDebugMessages();

    // ---- DXR 1.1 ----
    // No pipeline of its own: nothing this backend draws traces a ray. These two interfaces exist
    // only so the generic factory can build acceleration structures for a render feature that does,
    // which is why they are acquired behind the same DXR-1.1 gate the feature checks for itself.
    ComPtr<ID3D12Device5> device5_;
    ComPtr<ID3D12GraphicsCommandList4> cmdList4_;

    // ---- Mesh shader geometry path (D3D12 Ultimate) ----
    // A GENERIC geometry path, not a feature: it replaces the input assembler for any draw. A
    // separate root signature, because a mesh-shader PSO may not use one declaring an
    // input-assembler layout, and the MS path adds two root SRVs (vertices, indices) plus a
    // triangle count.
    ComPtr<ID3D12GraphicsCommandList6> cmdList6_;
    ComPtr<ID3D12RootSignature> msRootSig_;
    ComPtr<ID3D12PipelineState> msPso_;
    bool msSupported_ = false, msActive_ = false, msRefusalLogged_ = false;
    ID3D12RootSignature* boundRootSig_ = nullptr;   // raw: cache only, ownership stays in the ComPtrs

    bool hasSwapchain_ = false;
    // Vsync ON by default, which is what a windowed editor should do: an uncapped editor spins the
    // GPU at several hundred frames a second to redraw a mostly-static viewport, and on a laptop
    // that is heat and fan noise for nothing. Off is for measuring, and for anyone who wants it.
    bool vsync_ = true;
    // Whether the swapchain was CREATED able to tear. Fixed for the swapchain's life -- see
    // createSwapchain. False means vsync-off is not available on this machine at all.
    bool tearingSupported_ = false;
    f32 clear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    // The value the CURRENT scene colour target was created with. A render target carries one
    // optimised clear value, and clearing it to anything else costs the fast-clear path (debug
    // layer #820). The app sets its clear colour AFTER the target exists — the editor pushes dark
    // chrome on its first frame — so the two are reconciled in beginFrame rather than assumed
    // equal at creation. Seeded to the same literal as clear_ so frame 0 needs no rebuild.
    f32 msaaClear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    // The same colour expressed as the scene radiance that tonemaps back to it. Derived from
    // msaaClear_ by createMsaaColor and used by every actual clear, so the two can never disagree.
    f32 sceneClear_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::string adapterName_ = "D3D12 Device";
    // True when the device is running on a software rasteriser (WARP). Kept as a device property
    // rather than a cap because it is not a capability the adapter reports being without — it is
    // which IMPLEMENTATION of D3D12 is executing, and one pipeline-state combination below is
    // unsafe on that implementation alone.
    bool softwareAdapter_ = false;
    bool warpConsRasterLogged_ = false;

    // ---- generic RHI (render-feature modules) ----
    // Raw pointers, deleted in the destructor: both types are incomplete here, and holding them
    // by value would force their definitions above this class instead of below it.
    D3D12ResourceFactory* rhiFactory_ = nullptr;
    D3D12RenderContext* rhiContext_ = nullptr;
    std::vector<IRenderFeature*> features_;   // non-owning

    // The factory and the context reach into the queue, fence, command list and mesh table. They
    // are extensions of this device, not clients of it, so they get direct access rather than an
    // accessor for every member they touch.
    friend class D3D12ResourceFactory;
    friend class D3D12RenderContext;
};

// ---------------------------------------------------------------- generic RHI objects
// Backing records for the handle tables. A handle is always index + 1, so 0 is invalid everywhere
// and a default-constructed handle can never name a live resource.

struct RhiTexture {
    ComPtr<ID3D12Resource> res;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;   // one CPU-only entry, only when bound as a colour target
    ComPtr<ID3D12DescriptorHeap> dsvHeap;   // ditto for depth
    TextureDesc desc{};                     // resolved: `mips` holds the real count, never 0
#if AVER_RHI_TRACK_STATE
    // Owned copy: the desc's debugName is the caller's pointer, and a barrier report is worthless
    // if it can only name a handle number.
    std::string debugName;
    // One entry per mip. This interface exposes only single-slice 2D/3D textures, so a subresource
    // index IS a mip index and no array/plane arithmetic is needed.
    std::vector<ResourceState> states;
#endif
#if AVER_WITH_IMGUI
    // The descriptor uiTextureId() handed to the UI, cached so drawing the same image every frame
    // costs nothing and cannot drain the small UI pool. Zero until the UI first asks.
    u64 uiSrvCpu = 0, uiSrvGpu = 0;
#endif
};

struct RhiBuffer {
    ComPtr<ID3D12Resource> res;
    BufferDesc desc{};
    u8* mapped = nullptr;                   // upload buffers stay mapped for their whole life
#if AVER_RHI_TRACK_STATE
    std::string debugName;
    ResourceState state = ResourceState::Common;
    // Upload-heap and acceleration-structure buffers are pinned to the state they were created in;
    // D3D12 rejects any transition of either, so a barrier against one is always a module bug.
    bool stateFixed = false;
#endif
};

struct RhiShader {
    ComPtr<ID3DBlob> blob;
    ShaderStage stage = ShaderStage::Vertex;
};

// The slotParam tables below spell out one -1 per slot. Growing the slot count without widening
// them would leave the new entry at 0, which is a VALID root parameter index — so the mistake would
// bind to the wrong parameter rather than being caught.
static_assert(kMaxConstantSlots == 5, "slotParam's -1 initialisers are written out per slot");
// Same trap, same reason: a third table would leave srvParam[2] at 0, which is a valid root
// parameter index, so the table would bind over whatever parameter 0 happens to be.
static_assert(kBindingTableCount == 2, "srvParam/uavParam's -1 initialisers are written out per table");

// Where each declared binding landed in the root signature. -1 means the layout never declared it,
// so binding it is a module bug worth reporting rather than a silent no-op.
struct RhiPipeline {
    ComPtr<ID3D12PipelineState> pso;
    ID3D12RootSignature* rootSig = nullptr;   // owned by the root-signature cache, not by this
    bool compute = false;
    bool mesh = false;
    // One entry per declarable table; -1 where the layout declared nothing for it.
    i32  srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    // The first shader register each table covers, kept so a set bound at the wrong table index can
    // be caught. Derived from the layout, never written down twice.
    u32  srvBaseRegister[kBindingTableCount] = {};
    i32  slotParam[kMaxConstantSlots] = {-1, -1, -1, -1, -1};
    u32  slotDwords[kMaxConstantSlots] = {};  // 0 = the slot is a root CBV rather than root constants
    i32  msVertexParam = -1, msIndexParam = -1, msCountParam = -1;
};

// The kinds arrays are the size BindingSetDesc declares them. A set may declare more slots than
// that; anything past the end has no declared kind and is null-filled as a 2D texture.

struct RhiBindingSet {
    u32 srvCount = 0, uavCount = 0;
    // Which run of shader registers the set was BUILT for. Carried only so binding it at the wrong
    // table index is reportable; nothing here consumes it to build a descriptor.
    u32 srvBaseRegister = 0, uavBaseRegister = 0;
    u32 heapBase = 0;   // SRVs occupy [heapBase, heapBase+srvCount), the UAVs follow immediately
    SlotKind srvKinds[kMaxBindingSlots] = {};
    SlotKind uavKinds[kMaxBindingSlots] = {};
    bool alive = false;
};

// A descriptor range handed back by destroyBindingSet. It cannot be reused the moment it is
// returned: a command list already recorded may still bind the table that covers it, so the range
// waits on the same fence the retired-object queue uses.
struct RetiredRange {
    u32 first = 0;
    u32 count = 0;
    u64 fence = 0;
};

// Scratch is owned by the acceleration structure and outlives every build that consumes it. It is
// only ever released through the deferred-destroy queue: freeing it at the point of reallocation
// hands the GPU a dangling address for whatever frame is still in flight.
struct RhiBlas {
    ComPtr<ID3D12Resource> as, scratch;
    MeshHandle mesh = 0;
    bool built = false;
};

struct RhiTlas {
    ComPtr<ID3D12Resource> as, scratch;
    // One instance buffer per frame in flight: the CPU packs next frame's instances while the GPU
    // may still be consuming this frame's build.
    ComPtr<ID3D12Resource> instances[kFrameCount];
    u8* instancePtr[kFrameCount] = {};
    u32 maxInstances = 0;
};

// A root signature plus the parameter indices it was built with. Two pipelines declaring the same
// PipelineLayout share one object, which is what lets a feature's binding set survive a pipeline
// switch — changing to a PSO with a different root signature drops every root binding.
struct RootSigEntry {
    PipelineLayout layout{};
    bool mesh = false;
    ComPtr<ID3D12RootSignature> sig;
    i32 srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    i32 slotParam[kMaxConstantSlots] = {-1, -1, -1, -1, -1};
    i32 msVertexParam = -1, msIndexParam = -1, msCountParam = -1;
};

// A destroyed object the GPU may still be reading. Released once the fence passes, never sooner.
struct RetiredObject {
    ComPtr<IUnknown> obj;
    u64 fence = 0;
};

// PipelineLayout has padding between its fields, so a memcmp would treat two identical layouts as
// different whenever the padding happens to differ. Compared field by field for that reason.
bool sameSampler(const SamplerDesc& a, const SamplerDesc& b) {
    return a.filter == b.filter && a.address == b.address && a.compare == b.compare && a.maxLod == b.maxLod;
}
bool sameLayout(const PipelineLayout& a, const PipelineLayout& b) {
    if (a.srvCount != b.srvCount || a.uavCount != b.uavCount || a.samplerCount != b.samplerCount) return false;
    if (a.srvCount1 != b.srvCount1 || a.uavCount1 != b.uavCount1) return false;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) if (a.constantDwords[i] != b.constantDwords[i]) return false;
    for (u32 i = 0; i < a.samplerCount && i < 4; ++i) if (!sameSampler(a.samplers[i], b.samplers[i])) return false;
    return true;
}

// Descriptors every binding set suballocates from. One heap for the whole device so binding a set
// never costs a heap switch. 64k descriptors -- about 2 MB, against the million a shader-visible
// heap may hold -- because the old 1024 capped the scene near a hundred and twenty materials, and
// overflow returns 0 from createBindingSet: the material then draws with whatever table was bound
// last, which reads on screen as ONE object having someone else's texture rather than as an error.
constexpr u32 kRhiHeapSize = 65536;
// Transient constants per frame in flight. Large enough that a feature republishing its constants
// at every pass never runs dry within a frame.
constexpr u64 kRhiRingBytes = 1u << 20;

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
    void destroyShader(ShaderHandle h) override;
    void destroyPipeline(PipelineHandle h) override;
    void destroyBindingSet(BindingSetHandle h) override;

    void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) override;

    bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) override;
    bool textureInfo(TextureHandle h, TextureDesc& out) const override;
    void waitIdle() override;

    // Backs IDevice::uiTextureId — see there. Lives on the factory because the handle table does.
    u64 uiDescriptor(TextureHandle h);

private:
    // Said once per device: the substitution below is the normal state of affairs on a GPU without
    // ray tracing, and repeating it per binding set buried the messages that mean something.
    bool asSlotLogged_ = false;

    // Fill a freshly created texture from TextureDesc::initialData. The resource must already be in
    // COPY_DEST; on success it has been transitioned to `d.initialState` and the GPU has finished.
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

    const RootSigEntry* rootSignature(const PipelineLayout& layout, bool mesh);
    // Descriptor slot `heapBase + index`, CPU side (for writing) and GPU side (for binding).
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(u32 index) const;
    void nullFill(const RhiBindingSet& s);
    // Reuse a retired range large enough for `count`, or bump-allocate. Returns false when the heap
    // is exhausted rather than wrapping onto descriptors a live set still owns.
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
    // The frame path binds a feature's descriptor table for the scene draws IT records, so it needs
    // the same internals the context does. Both live in this translation unit.
    friend class D3D12Device;
};

class D3D12RenderContext final : public IRenderContext {
public:
    D3D12RenderContext(D3D12Device* dev, D3D12ResourceFactory* res) : dev_(dev), res_(res) {}

    void setPipeline(PipelineHandle p) override;
    void setViewport(u32 x, u32 y, u32 w, u32 h) override;
    void setScissor(u32 x, u32 y, u32 w, u32 h) override;
    void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) override;
    void clearDepth(TextureHandle depth, f32 value) override;
    void setBindingSet(BindingSetHandle set, u32 table) override;
    void setConstants(u32 slot, const void* data, u32 dwords) override;
    void setConstantBuffer(u32 slot, const void* data, u32 bytes) override;
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override;
    void drawMesh(MeshHandle mesh) override;
    void dispatchMeshFor(MeshHandle mesh) override;
    void dispatch(u32 gx, u32 gy, u32 gz) override;
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
    // The ring resets itself when the device's monotonic fence counter moves on, because the frame
    // loop does not yet call into this context and so has nowhere to reset it from.
    u64 ringEpoch_ = ~0ull;

    // Sticky per-draw state (setDrawBinding). Copied rather than referenced: the caller's block is
    // usually a stack temporary rebuilt each draw, and the binding is consumed later.
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

    // IDXGIFactory6::EnumAdapterByGpuPreference needs Windows 10 1803+. Fall back to plain
    // EnumAdapters1 so older Windows (common on the DX12-era GPUs we target) still gets a device.
    ComPtr<IDXGIFactory6> factory6;
    const bool haveGpuPref = SUCCEEDED(factory_.As(&factory6));
    ComPtr<IDXGIAdapter1> adapter;

    // WARP first, when asked for. It is the only "different GPU" available without buying one:
    // the software rasteriser reports its own tiers, so it exercises decisions a capability
    // clamp cannot reach. If it is unavailable we fall through to the hardware search rather
    // than failing -- the same decline-and-carry-on rule the rest of the engine follows.
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

    if (!hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence")) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) { AVER_ERROR("[RHI.D3D12] CreateEvent failed"); return false; }

    queryCaps();
    if (!(caps_.msaaMask & sampleCount_)) sampleCount_ = 1; // fall back if 4x is unsupported

    // A default light so meshes are lit before the app authors a sky. Taken FROM the engine default
    // rather than restated: this had its own vector and its own 0.15 fill, which meant a host that
    // never called setSkyAtmosphere was lit by a sun nothing else in the engine agreed with, under a
    // fill light six commits stale. A default that disagrees with the default is worse than no
    // default, because it looks deliberate.
    const SkyAtmosphere def{};
    setLight(def.sunDirection, def.sunColor, def.skyLightIntensity);

    if (!createPipeline()) return false;
    // The camera post chain's size-independent half. Its targets are built lazily on the
    // first frame, because they need a swapchain and this runs before there is one.
    if (!createPostPipelines()) return false;

    // Generic RHI surface. Its failure is not fatal: a feature module that cannot get a factory
    // declines to initialise, which is exactly what the Null backend already does.
    rhiFactory_ = new D3D12ResourceFactory(this);
    if (!rhiFactory_->init()) { delete rhiFactory_; rhiFactory_ = nullptr; }
    else rhiContext_ = new D3D12RenderContext(this, rhiFactory_);

    AVER_INFO("[RHI.D3D12] device ready on adapter '{}'", adapterName_);
    if (rhiFactory_) rhiFactory_->selfTest();
    return true;
}

// Each distinct message ID is logged ONCE. The two benign warnings this engine already emits
// (#820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE, #1328 CREATERESOURCE_STATE_IGNORED) fire
// every frame, and a per-frame repeat would bury the one message that matters. The running
// totals still count every occurrence, so nothing is hidden — only repeated.
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
            default: continue;  // INFO and MESSAGE are chatter; the totals do not need them
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

// The factory holds GPU resources, so it must go before the fence event it retires them against.
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

void D3D12Device::addRenderFeature(IRenderFeature* f) {
    if (!f) return;
    for (IRenderFeature* e : features_) if (e == f) return;
    features_.push_back(f);
    AVER_INFO("[RHI.D3D12] render feature registered: {}", f->name());
}

void D3D12Device::removeRenderFeature(IRenderFeature* f) {
    for (usize i = 0; i < features_.size(); ++i) {
        if (features_[i] != f) continue;
        features_.erase(features_.begin() + static_cast<isize>(i));
        return;
    }
}

// Ask the hardware what it can actually do, so the editor only offers real options.
void D3D12Device::queryCaps() {
    caps_ = {};
    caps_.computeShaders = true; // any D3D12 feature-level 11_0 device has compute
    caps_.msaaMask = 1;          // 1 sample always works
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
    // The clamp is applied HERE, once, at the end of the hardware query. Every consumer -- this
    // backend's own pipeline selection, Voxi's feature status, the editor's settings UI -- reads
    // `caps_` afterwards, so a forced-down device is indistinguishable from a real one and no
    // code path anywhere has to know an override exists.
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

    waitForGpu(); // PSOs and targets are in flight until the GPU drains
    const u32 prev = sampleCount_;
    sampleCount_ = samples;
    if (!createPipeline()) { sampleCount_ = prev; createPipeline(); return false; } // PSOs carry SampleDesc
    if (msSupported_) initMeshShaders();  // the mesh-shader PSO bakes SampleDesc too
    if (hasSwapchain_) {
        depthBuffer_.Reset();
        msaaColor_.Reset();
        if (!createDepthBuffer() || !createMsaaColor()) { AVER_ERROR("[RHI.D3D12] MSAA {}x target creation failed", samples); return false; }
        // The post chain's resolve target exists only above one sample, and every descriptor it
        // holds names a scene texture that was just recreated. Dropped rather than patched: the
        // next frame rebuilds it, and a descriptor pointing at a released resource is the kind of
        // fault that shows up as a corrupt frame somewhere else entirely.
        releasePostTargets();
    }
    // Features own pipelines that bake the sample count too, and this call can only rebuild the
    // ones the backend owns. A stale feature PSO is a draw-time target incompatibility, not a
    // creation-time error, so it surfaces far from its cause.
    notifyRenderTargetsChanged();
    AVER_INFO("[RHI.D3D12] MSAA set to {}x", samples);
    return true;
}

// Only when something a PIPELINE bakes has actually changed, which is the sample count and the two
// target formats -- and nothing else.
//
// A RESIZE was calling this, and a resize changes none of them. What it changed was the target
// RESOURCES, which no pipeline references. The cost of that was not theoretical: every feature's
// onRenderTargetsChanged rebuilds its pipelines, and Voxi's rebuild recompiles every scene shader
// from HLSL source and hands the resulting DXIL back to the driver to turn into ISA. Dragging a
// window edge recompiled the entire renderer, once per resize message.
//
// It is also the best candidate this project has for the crash that has been killing the editor. Six
// separate crashes across four hours and three builds all landed at the IDENTICAL fault offset
// inside amdxc64.dll -- AMD's shader compiler -- which is where a rebuild goes and where a resize had
// no business sending it. Stated as a candidate rather than a diagnosis: the dumps cannot be
// symbolised on this machine, and what is claimed here is that the rebuild was unnecessary, which is
// true whatever the driver does with it.
// The offscreen the post chain composites into when the editor wants the scene as an image.
//
// FULL backbuffer size and recreated only when that changes. Sizing it to the panel would destroy
// and recreate a render target the UI is sampling every time somebody drags a splitter, and that
// needs a waitIdle -- a whole-GPU stall once a frame for the duration of the drag.
bool D3D12Device::ensureViewportTexture() {
    IResourceFactory* f = resources();
    if (!f || width_ == 0 || height_ == 0) return false;
    if (viewportTex_ && viewportTexW_ == width_ && viewportTexH_ == height_) return true;

    if (viewportTex_) {
        // Drained first: the UI sampled this texture last frame, and the destroy is deferred behind
        // the fence but the DESCRIPTOR the UI holds is not.
        f->waitIdle();
        f->destroyTexture(viewportTex_);
        viewportTex_ = 0;
    }
    TextureDesc d;
    d.width = width_;
    d.height = height_;
    // The presented format, expressed in the RHI's own enum: kBackbufferFormat is a DXGI value.
    d.format = fromDxgiFormat(kBackbufferFormat);
    d.bind = ResourceBind::RenderTarget | ResourceBind::ShaderResource;
    // Where every frame LEAVES it: the UI samples it after the composite. Seeding the tracker from
    // the state it actually ends in means the first barrier of frame 0 is honest.
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

void D3D12Device::notifyRenderTargetsChanged() {
    if (sampleCount_ == notifiedSamples_ &&
        backbufferFormat() == notifiedColor_ && depthFormat() == notifiedDepth_) return;
    notifiedSamples_ = sampleCount_;
    notifiedColor_   = backbufferFormat();
    notifiedDepth_   = depthFormat();
    for (IRenderFeature* f : features_)
        f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat());
}

bool D3D12Device::createPipeline() {
    // b0 = the engine per-frame block, b1 = 24 root constants (world + colour + material). That is
    // the whole signature: no descriptor tables, no samplers, no feature constant block. A render
    // feature brings its own root signature through the generic factory, so nothing declared here
    // has to anticipate what one of them might read.
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
    // This pipeline is what a scene with no render feature registered gets. Unshadowed, with no
    // indirect light, by design — see PSMainPlain.
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

    // Sky pipeline: fullscreen triangle, no input layout, no depth.
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
    sp.DepthStencilState.DepthEnable = FALSE;
    sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    sp.SampleMask = UINT_MAX;
    sp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    sp.NumRenderTargets = 1;
    sp.RTVFormats[0] = kSceneColorFormat;
    sp.DSVFormat = kDepthFormat;
    sp.SampleDesc.Count = sampleCount_;
    if (!hrOk(device_->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&skyPso_)), "CreateGraphicsPipelineState(sky)")) return false;

    // Wireframe variant of the mesh PSO.
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&wirePso_)), "wire pso")) return false;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;

    // Line PSO (grid / gizmo): pos+colour, line list, depth-tested, no depth write.
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

    // Overlay line PSO: same as above but no depth test — gizmos stay visible on top.
    lp.DepthStencilState.DepthEnable = FALSE;
    lp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (!hrOk(device_->CreateGraphicsPipelineState(&lp, IID_PPV_ARGS(&lineOverlayPso_)), "line overlay pso")) return false;

    // Per-frame constant buffers (one per frame in flight), persistently mapped.
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    // Sized from the struct and rounded UP to the 256-byte alignment a constant buffer binds on,
    // rather than written out as a literal. It was 256 with a comment claiming the struct was 144
    // when it was already 240, and the authored atmosphere would have walked 64 bytes past the end
    // of the mapping -- a heap corruption whose only symptom is whatever happened to live after it.
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
//
// `state` is honoured for exactly one value: RAYTRACING_ACCELERATION_STRUCTURE, which a result
// buffer must be created in and never leaves. Every OTHER buffer state is IGNORED by D3D12 — a
// buffer is created in COMMON whatever is asked for, and asking for something else earns debug
// layer #1328 — so scratch is created COMMON and relies on the implicit promotion a buffer gets
// from COMMON on first GPU use, which is what puts it in UNORDERED_ACCESS for the build.
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

// The device can build acceleration structures for a render feature that traces rays. This backend
// draws nothing that traces, so there is no pipeline here any more -- only the two interfaces the
// generic factory needs, behind the same DXR 1.1 gate the feature applies to itself. Acquiring them
// unconditionally would let createBlas succeed on hardware where the build later fails.
bool D3D12Device::initAccelerationStructures() {
    if (caps_.rayTracingTier < 11 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    if (FAILED(device_.As(&device5_))) return false;
    AVER_INFO("[RHI.D3D12] DXR 1.1 acceleration structures available");
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

// Mesh shader path. Needs Tier 1 + SM 6.5 + DXC, i.e. the same D3D12 Ultimate floor as DXR 1.1.
bool D3D12Device::initMeshShaders() {
    msSupported_ = false;
    if (caps_.meshShaderTier == 0 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    ComPtr<ID3D12Device2> device2;
    if (FAILED(device_.As(&device2))) return false;
    if (!cmdList6_) return false;   // DispatchMesh lives on ID3D12GraphicsCommandList6

    // Root signature WITHOUT the input-assembler flag (illegal with a mesh shader), plus two root
    // SRVs for the buffers the MS reads directly and a root constant for the triangle count.
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

    // AVER_MS gates the mesh-shader entry points: their syntax is only legal from SM 6.5, so the
    // SM 5.1 compiles of this same source must not see them. The geometry registers travel with it,
    // built from the same base the root signature above used — the prelude refuses to compile
    // without them rather than fall back to a literal that could disagree.
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
// The mesh-shader path needs its own root signature, so the graphics root signature changes
// mid-frame: meshes may use msRootSig_ while lines and the sky stay on rootSig_. Switching one
// resets every root binding, hence the re-bind here. Cached so a run of same-signature draws pays
// nothing. Both signatures declare the engine block at the same index, which is all this has to
// restore now that neither carries a descriptor table.
void D3D12Device::bindGraphicsRoot(ID3D12RootSignature* rs) {
    if (boundRootSig_ == rs) return;
    boundRootSig_ = rs;
    cmdList_->SetGraphicsRootSignature(rs);
    cmdList_->SetGraphicsRootConstantBufferView(kSceneFrameParam, frameCBs_[frameIndex_]->GetGPUVirtualAddress());
}

bool D3D12Device::createSwapchainResources(const SwapchainDesc& d) {
    if (!d.windowHandle) { AVER_WARN("[RHI.D3D12] createSwapchain without a window (headless)"); return false; }
    width_ = d.width; height_ = d.height;

    // TEARING SUPPORT IS A SWAPCHAIN-CREATION DECISION, not a present-time one. Present(0, ...)
    // without it does not disable vsync on a flip-model swapchain -- it queues frames and the queue
    // itself becomes the wait, so the frame rate stays pinned to the refresh and only the latency
    // gets worse. The ALLOW_TEARING flag has to be on the swapchain from the start AND the matching
    // flag passed to every Present, so this has to be asked BEFORE the swapchain exists and cannot
    // be turned on later without recreating it.
    //
    // Absent on a machine without a DXGI 1.5 factory, or with it disabled by policy: then vsync-off
    // simply is not available and asking for it keeps the interval at 1, which is the honest failure.
    // Reported through caps so the UI can grey the control rather than offer a switch that does
    // nothing.
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
    cmdList_.As(&cmdList4_);   // optional: only needed for acceleration-structure builds
    cmdList_.As(&cmdList6_);   // optional: only needed for DispatchMesh
    cmdList_->Close();
    if (cmdList4_) initAccelerationStructures();
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
    td.Format = kSceneColorFormat; td.SampleDesc.Count = sampleCount_;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Created with the colour that will actually be cleared to, and the pair is remembered so
    // beginFrame can tell when the app has moved one and not the other.
    //
    // The value STORED is scene-referred while the one remembered is the app's. setClearColor names
    // the pixel the app wants to see — the editor's dark chrome grey — and that is a display colour;
    // clearing an HDR target to it directly would hand the composite a value it then tonemaps, and
    // the chrome would come out lighter than the app asked for. See toSceneReferred.
    D3D12_CLEAR_VALUE cv{}; cv.Format = kSceneColorFormat;
    toSceneReferred(clear_, cv.Color);
    for (int i = 0; i < 4; ++i) { sceneClear_[i] = cv.Color[i]; msaaClear_[i] = clear_[i]; }
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&msaaColor_)), "msaa color")) return false;
    device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

// A colour target holds ONE optimised clear value, fixed at creation. `setClearColor` can move the
// runtime clear at any time — the editor pushes its dark chrome colour on the first frame it
// renders, long after the target was made — and clearing to a value the target was not created
// with drops the fast-clear path and warns (#820) on every single clear.
//
// So the reconciliation is here rather than in setClearColor: a setter that recreated a render
// target would stall the GPU from inside the app's update, and the app pushes the same colour every
// frame. Rebuilding costs one drain and happens only when the value ACTUALLY moved — once, in
// practice, on the frame after the editor first sets its colour.
void D3D12Device::reconcileClearValue() {
    bool same = true;
    for (int i = 0; i < 4; ++i) if (clear_[i] != msaaClear_[i]) { same = false; break; }
    if (same || !msaaColor_) return;

    f32 prev[4];           // createMsaaColor() overwrites msaaClear_, so keep the good value here
    for (int i = 0; i < 4; ++i) prev[i] = msaaClear_[i];

    waitForGpu();          // the old target may still be referenced by an in-flight frame
    msaaColor_.Reset();
    if (!createMsaaColor()) {
        // Nothing else can run without a scene colour target, and the new clear value is the one
        // thing we know is bad, so put the previous one back and rebuild with that instead.
        AVER_ERROR("[RHI.D3D12] scene colour target rebuild for clear ({:.3f},{:.3f},{:.3f},{:.3f}) failed; keeping the previous clear value",
                   clear_[0], clear_[1], clear_[2], clear_[3]);
        for (int i = 0; i < 4; ++i) clear_[i] = prev[i];
        msaaColor_.Reset();
        createMsaaColor();
        return;
    }
    // At one sample the post chain's scene SRV points straight at msaaColor_, which has just been
    // replaced — so the descriptors have to go with it.
    releasePostTargets();
    AVER_TRACE("[RHI.D3D12] scene colour target rebuilt for clear ({:.3f},{:.3f},{:.3f},{:.3f})",
               clear_[0], clear_[1], clear_[2], clear_[3]);
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

void D3D12Device::beginFrame() {
    if (!hasSwapchain_) return;
    reconcileClearValue();
    // Render into whichever backbuffer is current now; wait for its previous frame to finish on
    // the GPU before recycling its allocator. This is the only fence wait in the frame and it
    // can never block on an unsignalled value (fenceValues_ only holds already-signalled ones).
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    const u64 want = fenceValues_[frameIndex_];
    // The allocator below is only safe to reset once the GPU has passed the frame that used it, so
    // this wait is not advisory. waitFence returns false only for a device that is already gone, in
    // which case resetting is moot and every call after it will fail anyway.
    if (want != 0) waitFence(want);
    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get());
    boundRootSig_ = nullptr;   // a command-list reset drops every root binding
    postCBUsed_ = 0;           // ...and the post ring is per frame, bump-allocated from zero
    // The per-draw binding is sticky WITHIN a frame only. Carrying it across would mean a draw that
    // sets none inherits the last draw of the previous frame -- wrong for exactly the draws that
    // forgot, which reads as a content bug rather than as a renderer one.
    drawBinding_ = defaultDrawBinding_;

    // Scene renders into the MSAA color + depth targets; endFrame resolves to backbuffer.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    std::memcpy(frameCBPtr_[frameIndex_], &frameCB_, sizeof(PerFrameCB));
    // A feature rotates its own draw list here, so prePass replays the PREVIOUS frame's geometry
    // while submitDraw fills the next one.
    for (IRenderFeature* f : features_) f->beginScene();

    // Feature passes own their own targets and viewport, so they run before the scene's are bound.
    if (rhiContext_) {
        for (IRenderFeature* f : features_) f->prePass(*rhiContext_);
        if (!features_.empty()) boundRootSig_ = nullptr;   // a feature pass left its own root signature bound
    }

    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    // Always clear the FULL surface: once the scene is scissored to a sub-rect the sky no longer
    // covers every pixel, and anything outside would keep stale content from an earlier frame.
    // sceneClear_, not clear_: the target is HDR and the frame is tonemapped at the end, so what is
    // written here is the pre-image of the colour the app asked for. It matches the value the
    // resource was created with, which is what keeps the fast-clear path.
    cmdList_->ClearRenderTargetView(rtv, sceneClear_, 0, nullptr);
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

    bindGraphicsRoot(rootSig_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // A feature may replace the scene entirely (e.g. a debug visualisation), after the colour
    // target is bound. drawMesh and drawLines honour the same flag, or overlays float over it.
    for (IRenderFeature* f : features_) {
        if (!f->suppressesScene()) continue;
        if (rhiContext_) f->scenePass(*rhiContext_);
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
    // Features see EVERY draw, before any early return below. A feature that replays geometry into
    // its own passes needs the full list; capturing after a return leaves it permanently empty.
    // The current sticky draw binding (table 1 + its b2 block) travels with the draw too, so a
    // feature's replayed passes shade from the same surface this draw's lit pass will — the block is
    // this frame's scratch, so submitDraw's contract is that the feature copies what it needs.
    for (IRenderFeature* f : features_)
        f->submitDraw(mesh, world, color, metallic, roughness,
                      drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
    // A feature replacing the scene (the GI debug view) means no lit draw -- but only AFTER the
    // submission above, or the volume its raymarch reads would never be filled.
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;

    // A feature may own the scene's lit pipeline: shading it contributes can live INSIDE the pixel
    // shader rather than in a separate pass, which no amount of extra passes can express. It then
    // supplies the bindings and frame constants that pipeline reads, too.
    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline() || !rhiContext_) continue;
        const PipelineHandle fp = f->scenePipeline(msActive_ && msPso_, wireframe_);
        if (!fp) break;   // the feature declined this combination; use the backend's own pipeline
        rhiContext_->setPipeline(fp);
        if (const BindingSetHandle bs = f->sceneBindingSet()) rhiContext_->setBindingSet(bs, 0);
        // Table 1 and b2 travel with the draw, not with the feature. The context applies them at
        // the draw itself and ignores them when the feature's pipeline declared neither.
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        const void* cb = nullptr; u32 cbBytes = 0;
        if (f->sceneConstants(&cb, &cbBytes) && cb && cbBytes)
            rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
        f32 fc[kObjectConstantDwords];
        std::memcpy(fc, world, 16 * sizeof(f32));
        std::memcpy(fc + 16, color, 4 * sizeof(f32));
        fc[20] = metallic; fc[21] = roughness; fc[22] = 0.0f; fc[23] = 0.0f;
        writeShadingConstants(fc);
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        if (msActive_ && msPso_ && !wireframe_) rhiContext_->dispatchMeshFor(mesh);
        else                                    rhiContext_->drawMesh(mesh);
        boundRootSig_ = nullptr;   // the context bound the feature's root signature, not ours
        return;
    }

    // The backend's OWN scene signatures declare no descriptor table at all, so there is nowhere to
    // put table 1 or b2. Declaring one here would oblige EVERY draw on this path -- sky, lines,
    // gizmos -- to bind it as well (Tier 1 populates whole tables, referenced or not), and would
    // push kSceneMeshSrvBase off the value it is frozen at. This path exists for the case where no
    // feature owns the lit pass, so it is the one that can afford to be plain. Said once, because a
    // per-draw binding quietly evaporating is otherwise indistinguishable from one never set.
    if (drawBinding_.set && !drawBindingIgnored_) {
        AVER_WARN("[RHI.D3D12] a per-draw binding is set but the scene uses the backend's own pipeline, which declares no table 1; it is ignored");
        drawBindingIgnored_ = true;
    }

    const GpuMesh& m = meshes_[mesh - 1];
    // Wireframe exists only on the IA path, so it wins over the mesh-shader toggle.
    const bool useMs = msActive_ && msPso_ && !wireframe_;
    bindGraphicsRoot(useMs ? msRootSig_.Get() : rootSig_.Get());
    cmdList_->SetPipelineState(useMs ? msPso_.Get() : (wireframe_ ? wirePso_.Get() : pso_.Get()));
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
    // The grid and the gizmos would float over whatever replaced the scene, and a screenshot is the
    // only thing that ever shows it: the probe reads one pixel and misses an overlay entirely.
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;
    const GpuLineMesh& m = lineMeshes_[mesh - 1];
    bindGraphicsRoot(rootSig_.Get()); // lines have no mesh-shader variant; a mesh draw may have switched
    cmdList_->SetPipelineState(lineDepth_ ? linePso_.Get() : lineOverlayPso_.Get());
    cmdList_->SetGraphicsRoot32BitConstants(kSceneObjectParam, 16, world, 0); // gWorld only
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->DrawInstanced(m.count, 1, 0, 0);
}

// ================================================================= the camera post chain
//
// Everything from here to runPostChain implements rhi::PostSettings. It lives in the backend rather
// than in a render feature because it is not a pass over the scene — it is what turns the scene
// target into the image the swapchain presents, which is the backend's own job. The HLSL is the
// RHI module's (rhi::postShaderSource), so a second backend gets one implementation and not a
// second opinion on what a tonemap is.

// The CPU twin of the shared prelude's averInverseTonemap(srgbToLin(c)). Kept identical BY HAND,
// which is a thing worth being uncomfortable about — but the alternative is a GPU pass to convert
// one clear colour, and the value has to be known on the CPU anyway because it is baked into the
// render target's optimised clear value at creation.
void D3D12Device::toSceneReferred(const f32 display[4], f32 out[4]) {
    for (int i = 0; i < 3; ++i) {
        const f32 lin = std::pow(display[i] < 0.0f ? 0.0f : display[i], 2.2f);
        const f32 y = lin > 1.0329f - 1e-4f ? 1.0329f - 1e-4f : lin;
        const f32 a = 2.43f * y - 2.51f;   // strictly negative over this range
        const f32 b = 0.59f * y - 0.03f;
        const f32 c = 0.14f * y;
        const f32 d = b * b - 4.0f * a * c;
        out[i] = (-b - std::sqrt(d < 0.0f ? 0.0f : d)) / (2.0f * a);
    }
    out[3] = display[3];
}

// Root signature, PSOs and the per-frame constant ring. Built once: none of it depends on the
// window size, which is what keeps a resize to just the targets and the descriptors.
// Pack the authored atmosphere into the block the shaders read, and derive from it everything that
// used to be pushed separately -- most importantly the SUN DIRECTION, which is now a consequence of
// the authored elevation and azimuth rather than a second thing to keep in step with them.
void D3D12Device::setSkyAtmosphere(const SkyAtmosphere& s) {
    sky_ = s;
    skyEnabled_ = s.enabled;

    // Straight through, unnormalised, exactly as setLight always passed it: the shaders normalise,
    // and normalising here as well would change the last bits of a value an oracle measures.
    for (int i = 0; i < 3; ++i) frameCB_.lightDir[i] = s.sunDirection[i];
    frameCB_.lightDir[3] = 0.0f;

    // A colour temperature, when authored, REPLACES the authored colour rather than tinting it:
    // two ways to say what colour the sun is that both apply is two ways to be surprised.
    f32 sun[3] = {s.sunColor[0], s.sunColor[1], s.sunColor[2]};
    if (s.sunTemperatureK > 0.0f) blackbodySrgb(s.sunTemperatureK, sun);
    for (int i = 0; i < 3; ++i) frameCB_.lightColor[i] = sun[i];
    frameCB_.lightColor[3] = 0.0f;

    for (int i = 0; i < 3; ++i) {
        frameCB_.skyZenith[i]   = s.zenith[i];
        frameCB_.skyHorizon[i]  = s.horizon[i];
        frameCB_.fogColor[i]    = s.fogColor[i];
        frameCB_.groundColor[i] = s.groundAlbedo[i];
        // The sky IS the fill light, so its intensity has to reach the ambient term the BRDF reads
        // and not only the dome the camera sees. This is the same scalar setLight's `ambient`
        // argument carried, which is why setSkyAtmosphere supersedes it for the sun.
        frameCB_.ambient[i]     = s.skyLightIntensity;
    }
    frameCB_.skyZenith[3] = frameCB_.skyHorizon[3] = 0.0f;
    frameCB_.groundColor[3] = s.groundBlend;
    frameCB_.ambient[3]   = 0.0f;
    frameCB_.fogColor[3]  = s.fogDensity;

    frameCB_.skyParams[0] = s.atmosphereHeight > 0.01f ? s.atmosphereHeight : 0.01f;
    frameCB_.skyParams[1] = s.skyLightIntensity;
    frameCB_.skyParams[2] = s.sunIntensity;
    // The disk test is a dot product against this, so the half-angle is what gets stored.
    frameCB_.skyParams[3] = std::cos(s.sunAngularDiameterDeg * 0.5f * 0.017453292f);

    frameCB_.fogParams[0] = s.fogFalloff;
    frameCB_.fogParams[1] = s.fogHeight;
    frameCB_.fogParams[2] = s.fogStart;
    frameCB_.fogParams[3] = s.fogMaxOpacity;

    frameCB_.cloudParams[0] = s.cloudCoverage;
    frameCB_.cloudParams[1] = s.cloudDensity;
    frameCB_.cloudParams[2] = s.cloudBottom;
    frameCB_.cloudParams[3] = s.cloudTop > s.cloudBottom ? s.cloudTop : s.cloudBottom + 1.0f;
    // The wind is integrated on the CPU into a world-space OFFSET. Handing the shader a velocity and
    // a time would make the clouds' position depend on a float that grows without bound, and they
    // would visibly quantise after a few minutes of play.
    frameCB_.cloudMotion[0] = s.cloudWind[0] * s.cloudTime;
    frameCB_.cloudMotion[1] = s.cloudWind[1] * s.cloudTime;
    frameCB_.cloudMotion[2] = s.cloudScale;
    frameCB_.cloudMotion[3] = s.cloudsEnabled ? 1.0f : 0.0f;

    packAtmosphere(s);
}

// The physical model, and the four authored fields it TAKES OVER when it is on.
//
// The dome, its exponent and the sun's colour stop being authored and become consequences of the
// sun's elevation. They are still written into the same constant-buffer fields, which is the whole
// trick: every cheap consumer of the sky -- the ambient term, environment reflections, the fog
// in-scatter target, the ground's own radiance, the cloud fill -- keeps reading a two-colour dome
// and gets a physical one, with no per-pixel march anywhere but the sky pass itself.
//
// s.zenith, s.horizon, s.atmosphereHeight and s.sunColor are NOT modified: they stay exactly as the
// level authored them, so switching the model back restores them rather than having overwritten them.
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
    // The engine's world is centimetres by contract, and the model's is kilometres because that is
    // the only scale at which 6360 and 8 are both representable in a float.
    frameCB_.atmoPlanet[2] = 1e-5f;
    frameCB_.atmoPlanet[3] = on ? 1.0f : 0.0f;
    frameCB_.atmoTune[0] = a.ozoneCentreKm;
    frameCB_.atmoTune[1] = a.multiScatterGain;
    frameCB_.atmoTune[2] = static_cast<f32>(a.viewSteps > 1 ? a.viewSteps : 1);
    frameCB_.atmoTune[3] = static_cast<f32>(a.aerialSteps > 1 ? a.aerialSteps : 1);

    if (!on) {
        for (int i = 0; i < 4; ++i) frameCB_.atmoSunE0[i] = 0.0f;
        return;
    }

    // ONE ground albedo, not two: the model's ground term reads the same swatch the dome does.
    AtmosphereProfile fit = a;
    f32 groundLin[3];
    for (int i = 0; i < 3; ++i) groundLin[i] = std::pow(std::fmax(s.groundAlbedo[i], 0.0f), 2.2f);
    fit.groundAlbedo = 0.2126f * groundLin[0] + 0.7152f * groundLin[1] + 0.0722f * groundLin[2];

    // Irradiance ABOVE the air: the authored colour, decoded, times the authored intensity. The
    // model attenuates it; nothing else may, or the attenuation lands twice.
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
    // Sea level. The dome a camera 3 metres up sees differs from this by far less than a code.
    AtmosphereDome dome{};
    atmoFitDome(fit, 0.0f, sunCos, e0, sunRadius, dome);

    // Written back through the SAME sRGB encode the authored fields use, because the shader decodes
    // whatever is in these slots. The encode is a plain 2.2 power, so it round-trips above 1 too --
    // which the sky near a low sun genuinely is.
    for (int i = 0; i < 3; ++i) {
        frameCB_.skyZenith[i]  = std::pow(std::fmax(dome.zenith[i], 0.0f), 1.0f / 2.2f);
        frameCB_.skyHorizon[i] = std::pow(std::fmax(dome.horizon[i], 0.0f), 1.0f / 2.2f);
        // The DIRECT sun, reddened and dimmed by the air it came through. This is the field the lit
        // pass, the GI injection and the sun disk all read, so all three redden together.
        frameCB_.lightColor[i] =
            std::pow(std::fmax(e0[i] * dome.sunTransmittance[i] / std::fmax(s.sunIntensity, 1e-6f), 0.0f),
                     1.0f / 2.2f);
    }
    frameCB_.skyParams[0] = dome.exponent;
}

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

    // Bilinear, clamped. CLAMP and not WRAP: every filter here reaches outside its own footprint at
    // the edges, and wrapping would fold the far side of the frame into the near one — which reads
    // as a bright rim on the opposite border from whatever caused it.
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
    // No input assembler: every pass is either a fullscreen triangle generated from SV_VertexID or
    // a dispatch. Declaring the IA flag would only cost root-signature space.
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "post root signature")) return false;
    if (!hrOk(device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&postRootSig_)), "post root signature")) return false;

    const char* src = postShaderSource();
    ComPtr<ID3DBlob> vs;
    if (FAILED(shaderCompiler().compile(src, "PostVS", "vs_5_1", &vs))) return false;

    // One description, three pixel shaders. Fullscreen triangle, no depth, no input layout.
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
            // The upsample ADDS into the level above. Done by the blender rather than by reading the
            // destination in the shader, because a typed UAV load of RGBA16F is an optional D3D12
            // feature and this engine explicitly gates hardware without it.
            rt.BlendEnable = TRUE;
            rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ONE;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rtFormat;
        d.SampleDesc.Count = 1;   // every post target is single-sampled; the resolve happens first
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

    // The adaptation passes are compute, and compute alone: they need atomics into a buffer, which
    // no graphics pass can express. Every D3D12 FL 11_0 device has it (see caps_.computeShaders),
    // so there is no fallback to write.
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

    // One upload buffer per frame in flight, bump-allocated and reset in beginFrame. Separate from
    // the render context's ring because that one is reset by a feature's first allocation and this
    // runs after every feature has finished.
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = bufferDesc(kPostConstantRingBytes);
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&postCBs_[i])), "post constant ring")) return false;
        D3D12_RANGE none{0, 0};
        postCBs_[i]->Map(0, &none, reinterpret_cast<void**>(&postCBPtr_[i]));
    }

    // The histogram and the adapted exposure. Both are DEFAULT-heap buffers written only by the GPU;
    // the exposure one carries its own "has this ever been written" flag in its second dword, so the
    // first frame snaps to the measured value instead of easing up from an uninitialised one.
    // COMMON, not UNORDERED_ACCESS. A buffer is effectively created in COMMON whatever is asked for,
    // and naming anything else earns debug-layer warning #1328 on every run — which this project
    // counts per gate, so a permanent two is a permanent two.
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
    // Queried, NOT copied from rtvSize_. This runs at device init, before there is a swapchain, and
    // rtvSize_ is not filled in until one exists — so copying it lands every bloom mip's RTV on the
    // same descriptor and the whole pyramid renders into whichever mip was written last.
    postRtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::postTriple(u32 triple) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = postSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(triple) * 3 * postSrvSize_;
    return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::postTripleCpu(u32 triple) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = postSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(triple) * 3 * postSrvSize_;
    return h;
}

void D3D12Device::releasePostTargets() {
    sceneResolved_.Reset();
    bloomTex_.Reset();
    bloomMips_ = bloomW_ = bloomH_ = 0;
    postReady_ = false;
}

// The size-dependent half: the resolve destination, the bloom pyramid, and every descriptor that
// points at either. Rebuilt on resize and on a sample-count change; the pipelines above survive both.
bool D3D12Device::createPostTargets() {
    releasePostTargets();
    if (!postRootSig_ || width_ == 0 || height_ == 0) return false;

    // A resolve destination exists only when there is something to resolve. At one sample the scene
    // target IS the resolved image, and a copy would be a full-resolution round trip for nothing.
    if (sampleCount_ > 1) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width_; td.Height = height_;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = kSceneColorFormat; td.SampleDesc.Count = 1;
        auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&sceneResolved_)), "scene resolve target"))
            return false;
    }

    // The pyramid starts at HALF resolution. A bloom halo is low-frequency by definition, so the
    // level nobody can distinguish from full res is the one that costs a quarter of the bandwidth.
    bloomW_ = width_ / 2 > 1 ? width_ / 2 : 1;
    bloomH_ = height_ / 2 > 1 ? height_ / 2 : 1;
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
    // No optimised clear value: nothing in the chain ever clears the pyramid. The prefilter writes
    // every texel of mip 0 and each downsample writes every texel of its own level.
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

    // A triple is (t0, t1, t2). Slots a pass does not read are filled with the scene view rather
    // than left null: an unwritten descriptor in a shader-visible heap is undefined to READ, even
    // by a shader that never samples it, and the debug layer is entitled to say so.
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

    // t2 is the EXPOSURE buffer, because the bloom threshold has to be applied to exposed
    // radiance -- see averBloomPrefilter.
    writeTriple(kPostTriplePrefilter, scene, 0, scene, 0, true);
    writeTriple(kPostTripleHistogram, scene, 0, scene, 0, false);
    writeTriple(kPostTripleComposite, scene, 0, bloomTex_.Get(), 0, true);
    for (u32 m = 1; m < bloomMips_; ++m) {
        // Down step m reads mip m-1; up step m reads mip m and is blended into mip m-1.
        writeTriple(kPostTripleDownBase + (m - 1), bloomTex_.Get(), m - 1, bloomTex_.Get(), m - 1, false);
        writeTriple(kPostTripleUpBase   + (m - 1), bloomTex_.Get(), m,     bloomTex_.Get(), m,     false);
    }

    // The UAV pair sits past every triple, contiguous, because it is bound as one table.
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

    // One RTV per bloom mip.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 m = 0; m < bloomMips_; ++m) {
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = kSceneColorFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = m;
        device_->CreateRenderTargetView(bloomTex_.Get(), &rd, rtv);
        rtv.ptr += postRtvSize_;
    }

    // expSeeded_ is deliberately NOT reset here. The histogram and exposure buffers belong to
    // createPostPipelines and outlive every resize, so re-seeding them would issue a barrier out of
    // COMMON for a resource that has been in UNORDERED_ACCESS since the first frame.
    postReady_ = true;
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Device::postConstants(const void* data, u32 bytes) {
    const u32 f = frameIndex_ < kFrameCount ? frameIndex_ : 0;
    if (!postCBPtr_[f]) return 0;
    const u32 offset = (postCBUsed_ + 255u) & ~255u;   // a root CBV binds on 256-byte alignment
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
//
// Every stage past the composite is SKIPPED when it would be a no-op — no bloom means no pyramid
// and no passes at all, not a pyramid weighted to zero. That is what keeps the default cost of
// this at exactly one fullscreen triangle over the frame plus the resolve that was always there.
void D3D12Device::runPostChain(ID3D12Resource* bb) {
    // Measured here because this is the one point that runs exactly once per presented frame.
    {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (lastFrameTick_ != 0 && freq.QuadPart > 0) {
            const f64 dt = static_cast<f64>(now.QuadPart - lastFrameTick_) / static_cast<f64>(freq.QuadPart);
            // Clamped: a frame that took a second (a breakpoint, a device reset, an alt-tab) must
            // not snap the adaptation, and a zero would freeze it.
            frameSeconds_ = static_cast<f32>(dt < 1e-4 ? 1e-4 : (dt > 0.25 ? 0.25 : dt));
        }
        lastFrameTick_ = now.QuadPart;
    }

    const bool msaa = sampleCount_ > 1;
    if (!postReady_ && !createPostTargets()) {
        // Without the chain there is no way to get an HDR scene onto an 8-bit backbuffer at all, so
        // this is fatal to the image rather than to the frame. Say it once and present what is
        // there, which is a black window — better than a silent hang in a resize loop.
        static bool said = false;
        if (!said) { AVER_ERROR("[RHI.D3D12] the post chain is unavailable; the scene cannot be presented"); said = true; }
        auto toRt = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
        return;
    }

    const bool bloom = post_.bloomIntensity > 0.0f && bloomTex_;
    const bool autoExp = post_.autoExposure && caps_.computeShaders;
    ID3D12Resource* scene = msaa ? sceneResolved_.Get() : msaaColor_.Get();

    // A DEFAULT-heap resource has undefined contents until something writes it — "usually zero" is
    // a driver's habit, not the spec's promise. The adaptation reads its own previous value and its
    // own seeded flag, so garbage here is an exposure that starts somewhere arbitrary and then eases
    // towards the right one over a second, which reads as a bug in the adaptation itself.
    if (!expSeeded_) {
        const u32 zeros[258] = {};
        const D3D12_GPU_VIRTUAL_ADDRESS va = postConstants(zeros, sizeof zeros);
        if (va) {
            const u32 f = frameIndex_ < kFrameCount ? frameIndex_ : 0;
            const UINT64 off = va - postCBs_[f]->GetGPUVirtualAddress();
            // From COMMON, which is where the buffers really are — see createPostPipelines. This is
            // the one place they are ever in it: the closing barrier below leaves them in
            // UNORDERED_ACCESS for the rest of the process.
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
        // The resolve happens in LINEAR HDR now rather than on gamma-encoded 8-bit values, which is
        // the physically correct order: averaging display codes darkens edges, because the encode is
        // concave. It is also why an edge pixel can differ from what the old path produced.
        auto toSrv = transition(scene, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &toSrv);
    } else {
        auto toSrv = transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &toSrv);
    }

    ID3D12DescriptorHeap* heaps[] = {postSrvHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
    boundRootSig_ = nullptr;   // this signature is not one bindGraphicsRoot tracks
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
        // Frame-rate independent: the same speed reaches the same fraction of the way in the same
        // WALL time whether the frame took 4 ms or 40. A raw per-frame alpha would make the eye
        // adapt faster on a faster machine, which is a feel bug nobody attributes to the renderer.
        cb.adapt[2] = 1.0f - std::exp(-post_.exposureSpeed * frameSeconds_);
        cb.adapt[3] = 0.0f;
        cb.limit[0] = post_.exposureMin;         cb.limit[1] = post_.exposureMax;
        cb.limit[2] = post_.histogramLowPercent; cb.limit[3] = post_.histogramHighPercent;
        cb.misc[0] = post_.exposureKey;
        cb.misc[1] = autoExp ? 1.0f : 0.0f;
        // One destination texel of skirt on every upsample. Wider looks softer and starts to reveal
        // the pyramid's own levels as concentric rings; narrower stops the levels overlapping at all
        // and the halo becomes a stack of boxes.
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
        b.Transition.Subresource = mip;   // per-SUBRESOURCE: the pyramid reads one mip while writing another
        cmdList_->ResourceBarrier(1, &b);
        bloomState_[mip] = to;
    };
    auto mipW = [&](u32 m) { return bloomW_ >> m ? bloomW_ >> m : 1u; };
    auto mipH = [&](u32 m) { return bloomH_ >> m ? bloomH_ >> m : 1u; };

    // ---- eye adaptation ----
    if (autoExp) {
        const u32 hw = width_ / kHistogramDownscale > 1 ? width_ / kHistogramDownscale : 1;
        const u32 hh = height_ / kHistogramDownscale > 1 ? height_ / kHistogramDownscale : 1;
        fillCommon(hw, hh, width_, height_);

        cmdList_->SetComputeRootSignature(postRootSig_.Get());
        cmdList_->SetPipelineState(histogramPso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch((hw + 15) / 16, (hh + 15) / 16, 1);

        // The reduction reads every bin the pass above wrote, and zeroes them for the next frame.
        D3D12_RESOURCE_BARRIER uavB{};
        uavB.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavB.UAV.pResource = histBuf_.Get();
        cmdList_->ResourceBarrier(1, &uavB);

        cmdList_->SetPipelineState(exposurePso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch(1, 1, 1);

        // The graphics root signature was replaced by the compute one above on the same slot list;
        // rebind before the composite or its table bindings land on the wrong parameter.
        cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
    }

    // The exposure buffer stays READABLE from here to the end of the chain: the bloom prefilter
    // below reads it to threshold on exposed radiance, and the composite reads it to apply the
    // exposure itself. Moved out of UnorderedAccess once, not twice.
    {
        auto expToSrv = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &expToSrv);
    }

    // ---- bloom ----
    if (bloom) {
        bloomTo(0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fillCommon(mipW(0), mipH(0), width_, height_);
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

    // ---- composite ----
    fillCommon(width_, height_, width_, height_);
    {
        // The backbuffer is made a render target either way: the composite lands there in the normal
        // path, and the UI lands there in both.
        auto toRt = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;

        const RhiTexture* vt = (viewportToTex_ && ensureViewportTexture() && rhiFactory_)
                             ? rhiFactory_->texture(viewportTex_) : nullptr;
        if (vt && vt->rtvHeap) {
            // Into the texture, and the BACKBUFFER IS CLEARED rather than left alone. The UI draws
            // over it and would otherwise be compositing onto whatever the previous frame left --
            // which on a flip-model swapchain is a frame or two old and reads as ghosting.
            const f32 blank[4] = {clear_[0], clear_[1], clear_[2], 1.0f};
            cmdList_->ClearRenderTargetView(bbRtv, blank, 0, nullptr);

            auto toRtTex = transition(vt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRtTex);
            D3D12_CPU_DESCRIPTOR_HANDLE trtv = vt->rtvHeap->GetCPUDescriptorHandleForHeapStart();
            fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0].Get(), kPostTripleComposite,
                       width_, height_, &trtv);
            auto backToSrv = transition(vt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);
            // The UI pass expects the backbuffer bound; the fullscreen helper above left the texture
            // bound instead.
            cmdList_->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        } else {
            fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0].Get(), kPostTripleComposite,
                       width_, height_, &bbRtv);
        }
    }

    // ---- restore ----
    // Back to the states the next frame's beginFrame assumes. Doing it here rather than there keeps
    // every state this function moved in one place, which is the only way the pyramid's per-mip
    // bookkeeping stays auditable.
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

void D3D12Device::endFrame() {
    if (!hasSwapchain_) return;
    ID3D12Resource* bb = renderTargets_[frameIndex_].Get();

    // The scene target is HDR linear radiance; the post chain is what turns it into the display
    // image the backbuffer holds. It replaces what used to be a straight resolve/copy here, and it
    // leaves the backbuffer in RENDER_TARGET, which is the state the UI pass below expects.
    runPostChain(bb);

    // ---- overlay features, on the tonemapped backbuffer ----
    // Bound here rather than by the feature because the backbuffer is not a TextureHandle any module
    // can name. Depth is deliberately absent: an overlay composites in submission order and a depth
    // test against the scene's buffer would let world geometry occlude a HUD.
    if (rhiContext_ && !features_.empty()) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;

        // INTO THE VIEWPORT TEXTURE when there is one, not onto the backbuffer.
        //
        // This is what makes a game's HUD visible at all in the editor, and its absence was invisible
        // in the worst way: the overlay WAS drawn, correctly, onto the backbuffer -- and then ImGui
        // painted the viewport texture over that exact region a few lines later and covered it. The
        // pass ran, the draws were submitted, nothing errored, and nothing appeared. `--ui-demo`
        // showed it too, so this predates and outlives any one HUD.
        //
        // Compositing into the texture is also the semantically right answer rather than merely the
        // working one: the HUD belongs to the VIEWPORT, so it should travel with the viewport into
        // whatever panel draws it, be clipped by that panel, and not float over the editor's chrome.
        const RhiTexture* ovt = (viewportToTex_ && viewportTex_ && rhiFactory_)
                              ? rhiFactory_->texture(viewportTex_) : nullptr;
        const bool intoTexture = ovt && ovt->rtvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE overlayRtv = rtv;
        if (intoTexture) {
            // The composite left it in SRV a few lines above; it has to be a render target again for
            // the length of this pass and back afterwards, or the ImGui draw that samples it reads a
            // resource in the wrong state.
            auto toRt = transition(ovt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRt);
            overlayRtv = ovt->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        }
        cmdList_->OMSetRenderTargets(1, &overlayRtv, FALSE, nullptr);
        // The post chain leaves the viewport at whatever its last pass wanted, which for a bloom
        // pyramid is a fraction of the screen. Restored here so a feature that sets neither still
        // draws over the whole backbuffer, which is what the hook's contract promises.
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &sc);
        // The post chain bound its own root signature and descriptor heap; both are re-stated by the
        // feature's own setPipeline / setBindingSet, which is why nothing is reset here.
        for (IRenderFeature* f : features_) f->overlayPass(*rhiContext_, width_, height_);

        if (intoTexture) {
            auto backToSrv = transition(ovt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);
            // ImGui expects the backbuffer bound, and the block above left the texture bound.
            cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
    }

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
    // Tested at the call site, not inside the drain: with the layer off `infoQueue_` is null for the
    // device's whole life, and a frame that asked for no validation should not carry even a call.
    if (infoQueue_) drainDebugMessages();
}

void D3D12Device::present() {
    if (!hasSwapchain_) return;
    // Both halves or neither: DXGI rejects the tearing PRESENT flag on a swapchain that was not
    // created with the tearing SWAPCHAIN flag, and it must never be combined with a non-zero sync
    // interval. vsync_ false without tearing support therefore stays at interval 1 rather than
    // silently presenting a stutter -- see createSwapchain for why Present(0) alone does not work.
    const bool tearing = !vsync_ && tearingSupported_;
    const UINT interval = vsync_ ? 1u : 0u;
    const UINT flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT pr = swapChain_->Present(tearingSupported_ ? interval : 1u, flags);
    if (FAILED(pr))
        AVER_ERROR("[RHI.D3D12] Present failed 0x{:08X} removed=0x{:08X}", (u32)pr, (u32)device_->GetDeviceRemovedReason());

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
        } else {
            AVER_ERROR("[RHI.D3D12] capture Map FAILED");
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
    // THE FLAGS MUST MATCH WHAT THE SWAPCHAIN WAS CREATED WITH. This passed 0, which was right for as
    // long as creation passed no flags -- and stopped being right the moment vsync-off added
    // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING there. DXGI does not reinterpret a resize as a request to
    // drop a capability: it returns E_INVALIDARG, and EVERY resize on a tearing-capable machine
    // failed from that point on.
    const UINT scFlags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!hrOk(swapChain_->ResizeBuffers(kFrameCount, w, h, kBackbufferFormat, scFlags), "ResizeBuffers")) {
        // A FAILED RESIZE MUST NOT LEAVE THE DEVICE WITH NOTHING TO DRAW INTO. The back buffers were
        // released above, before the call that can fail, because ResizeBuffers requires it -- so the
        // early return this replaces left renderTargets_ full of nulls, width_/height_ unchanged and
        // no views rebuilt. The next beginFrame then handed a null resource to a barrier, which is
        // not a reported error but a fault inside the driver.
        //
        // A failed ResizeBuffers leaves the swapchain UNCHANGED, so its buffers are still there at
        // the old size and re-acquiring them puts the device back exactly where it was. The window
        // is then the wrong size for the swapchain, which is a stretched frame -- visibly wrong, and
        // recoverable on the next resize, which is what an error path should cost.
        createRenderTargetViews();
        createDepthBuffer();
        createMsaaColor();
        return;
    }
    width_ = w; height_ = h;
    vpX_ = vpY_ = vpW_ = vpH_ = 0; // drop the stale rect; the app re-pushes it next frame
    // GPU is idle, so no backbuffer has pending work; clear per-buffer fences (beginFrame reacquires
    // the current index and will not wrongly wait). No back-buffer-parity assumptions to break.
    for (u32 n = 0; n < kFrameCount; ++n) fenceValues_[n] = 0;
    createRenderTargetViews();
    createDepthBuffer();
    createMsaaColor();
    // Every post target is sized from the frame, and every post descriptor names one. Dropped here
    // and rebuilt on the next frame, which is also where a failure has somewhere to be reported.
    releasePostTargets();
    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_));
    notifyRenderTargetsChanged();   // after the device's own targets are rebuilt, never before
    AVER_TRACE("[RHI.D3D12] resized to {}x{}", w, h);
}

// Block until the fence reaches `value`. Returns false ONLY when the device is gone.
//
// The single flat five-second wait this replaces did the one thing a fence wait must never do: on
// timeout it logged and CARRIED ON, and every caller then went and reset a command allocator the GPU
// was still reading. That is D3D12 error #541 followed by device removal, and it is guaranteed
// corruption rather than a risk of it.
//
// A longer timeout would only move the cliff. What is actually wanted is "wait until the work is
// done, unless the device died", and the device is the only thing that can say a wait is hopeless —
// so the wait is chunked and the removal reason is what ends it. A slow frame is now slow, not fatal:
// WARP raymarching a voxel volume at 3532x1987 and then running a full-resolution post chain over it
// takes seconds per frame, and that is a legitimate thing to be waiting for.
bool D3D12Device::waitFence(u64 value) {
    if (!fence_ || !fenceEvent_) return true;
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) return false;

    // One second a slice, so a genuinely wedged GPU is still noticed promptly and said out loud
    // once, rather than hanging silently forever.
    for (u32 slice = 0;; ++slice) {
        if (WaitForSingleObject(fenceEvent_, 1000) != WAIT_TIMEOUT) return true;
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            AVER_ERROR("[RHI.D3D12] the device was removed while waiting for the GPU (0x{:08X})",
                       static_cast<u32>(removed));
            return false;
        }
        if (slice == 4)   // five seconds in, and still healthy: say so once and keep waiting
            AVER_WARN("[RHI.D3D12] still waiting on the GPU after 5 s; the device is healthy, so "
                      "this is a slow frame (WARP, or a very large viewport) and not a hang");
    }
}

void D3D12Device::waitForGpu() {
    if (!queue_ || !fence_ || !fenceEvent_) return;
    const u64 v = ++nextFence_;
    if (FAILED(queue_->Signal(fence_.Get(), v))) return;
    waitFence(v);
}

bool D3D12Device::uiInit(void* hwnd) {
#if AVER_WITH_IMGUI
    if (uiActive_) return true;
    if (!device_ || !hwnd) return false;

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.NumDescriptors = kUiSrvCount;
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!hrOk(device_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&uiSrvHeap_)), "UI SRV heap")) return false;
    g_uiSrv = UiSrvPool{};
    g_uiSrv.heap = uiSrvHeap_.Get();
    g_uiSrv.cpuBase = uiSrvHeap_->GetCPUDescriptorHandleForHeapStart().ptr;
    g_uiSrv.gpuBase = uiSrvHeap_->GetGPUDescriptorHandleForHeapStart().ptr;
    g_uiSrv.stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
    info.SrvDescriptorAllocFn = &uiSrvAlloc;
    info.SrvDescriptorFreeFn = &uiSrvFree;
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
    g_uiSrv = UiSrvPool{};
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

// Defined after the factory (below) so the call can be spelled; declared with the other ui* members
// because that is where the UI vocabulary lives.
u64 D3D12Device::uiTextureId(TextureHandle t) {
#if AVER_WITH_IMGUI
    if (!uiActive_ || !rhiFactory_) return 0;
    return rhiFactory_->uiDescriptor(t);
#else
    (void)t;
    return 0;
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

// ================================================================ generic RHI resource factory
// Everything below implements aver::rhi::IResourceFactory / IRenderContext. It is a second,
// self-contained path alongside the renderer above: it shares the device, queue, fence, command
// list and mesh table, and nothing else. No existing pass is routed through it.

D3D12ResourceFactory::~D3D12ResourceFactory() {
    // The device drains the GPU before deleting us, but a factory torn down for any other reason
    // must not free memory a frame still in flight is reading.
    dev_->waitForGpu();
    retired_.clear();
}

bool D3D12ResourceFactory::init() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kRhiHeapSize;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!hrOk(dev_->device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "rhi descriptor heap")) return false;
    heapStride_ = dev_->device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// ---- handle tables. A record whose resource is gone reads as an invalid handle, so a use after
// destroy is reported rather than silently touching a recycled slot.
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

D3D12_CPU_DESCRIPTOR_HANDLE D3D12ResourceFactory::cpuSlot(u32 index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * heapStride_;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12ResourceFactory::gpuSlot(u32 index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * heapStride_;
    return h;
}

// Resource Binding Tier 1 hardware reads undefined data from any descriptor in a bound table that
// was never written — whether or not the shader touches that slot — and a null descriptor whose
// dimension disagrees with what the shader declared is undefined too. The declared SlotKind is the
// only source of that dimension: counts alone cannot supply it, and neither can guessing.
//
// The format is a best effort (a null view reads no memory, so only the dimension is load-bearing):
// RGBA8 for 2D surfaces, RGBA16F for 3D ones, which are typically HDR.
void D3D12ResourceFactory::nullFill(const RhiBindingSet& s) {
    for (u32 i = 0; i < s.srvCount; ++i) {
        const SlotKind kind = s.srvKinds[i];   // createBindingSet rejected counts past the limit
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (kind == SlotKind::AccelerationStructure) {
            // A null acceleration structure is an address of zero; the view takes no resource at
            // all. Declaring one on a device without DXR would be rejected, so fall back there.
            if (dev_->caps_.rayTracingTier == 0) {
                // Expected on every device without DXR, and every binding set with the slot hits it,
                // so it is stated once and as information. A WARN per set read as a fault on exactly
                // the hardware the fallback exists for.
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
        } else if (kind == SlotKind::Texture3D) {
            sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            sv.Texture3D.MipLevels = 1;
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
            // There is no such thing as an acceleration-structure UAV: it is read-only to shaders.
            AVER_ERROR("[RHI.D3D12] binding set UAV slot {} declares AccelerationStructure, which is SRV-only", i);
            kind = SlotKind::Texture2D;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        if (kind == SlotKind::Texture3D) {
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

// First fit, splitting anything larger than asked for: the tail was never bound, so it goes
// straight back onto the free list rather than waiting on another fence.
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

// The frame being recorded right now retires on the fence value present() will signal next, so that
// is the earliest point at which releasing is safe. Taken past the completed value as well as past
// the frame counter, because createSwapchainResources rewinds that counter — and a retire fence
// that looks already-passed would free a resource the current command list still names.
u64 D3D12ResourceFactory::retireFence() const {
    const u64 completed = dev_->fence_ ? dev_->fence_->GetCompletedValue() : 0;
    return (dev_->nextFence_ > completed ? dev_->nextFence_ : completed) + 1;
}

void D3D12ResourceFactory::retire(ComPtr<IUnknown> obj) {
    if (!obj) return;
    retired_.push_back({std::move(obj), retireFence()});
}

void D3D12ResourceFactory::collect() {
    if (!dev_->fence_ || (retired_.empty() && pendingRanges_.empty())) return;
    const u64 done = dev_->fence_->GetCompletedValue();
    for (usize i = 0; i < retired_.size();) {
        if (retired_[i].fence <= done) { retired_[i] = std::move(retired_.back()); retired_.pop_back(); }
        else ++i;
    }
    // Descriptor ranges go through the same gate: no command list that could still bind them is
    // outstanding once their fence has passed.
    for (usize i = 0; i < pendingRanges_.size();) {
        if (pendingRanges_[i].fence <= done) {
            freeRanges_.push_back({pendingRanges_[i].first, pendingRanges_[i].count, 0});
            pendingRanges_[i] = pendingRanges_.back();
            pendingRanges_.pop_back();
        } else ++i;
    }
}

// ---- root-signature cache
const RootSigEntry* D3D12ResourceFactory::rootSignature(const PipelineLayout& layout, bool mesh) {
    for (const RootSigEntry& e : rootSigs_)
        if (e.mesh == mesh && sameLayout(e.layout, layout)) return &e;

    RootSigEntry e;
    e.layout = layout;
    e.mesh = mesh;

    // Four possible ranges: SRV and UAV for each of the two declarable tables. A range is a separate
    // root parameter per table, NOT two ranges under one, because the two tables are bound
    // independently — one root parameter would mean one GPU handle covering both, so swapping the
    // per-draw half would have to rewrite the feature's half as well.
    D3D12_DESCRIPTOR_RANGE ranges[2 * kBindingTableCount] = {};
    D3D12_ROOT_PARAMETER params[2 * kBindingTableCount + kMaxConstantSlots + 3] = {};
    u32 n = 0;
    // Table 1 is based immediately above table 0, so the two runs are contiguous in register space
    // and a shader sees one flat t0..tN however the root signature happens to split them.
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

    // Logical constant slot k maps to register b(k). A non-zero word count makes it root constants
    // written with setConstants; zero makes it a root CBV written with setConstantBuffer, which is
    // what the upload ring binds into. Both forms are declared up front, so a slot changing hands
    // never rebuilds a PSO — and a slot is never both, which is what the context enforces.
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
        // A mesh shader has no input assembler, so the backend hands it the geometry directly. The
        // registers are pinned by RHIResources.hpp and matched by sharedShaderPrelude(): SRVs just
        // past the declared tables -- BOTH of them, so no layout can collide with them however it
        // splits its SRVs between the two -- and the triangle count above
        // b4, which is reserved for a feature's own frame constants. Root descriptors rather than
        // heap slots keep the per-draw cost at two virtual addresses.
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
    for (u32 i = 0; i < n; ++i) params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[4] = {};
    const u32 sampCount = layout.samplerCount < 4 ? layout.samplerCount : 4;
    for (u32 i = 0; i < sampCount; ++i) {
        samplers[i].Filter = toFilter(layout.samplers[i].filter);
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = toAddress(layout.samplers[i].address);
        samplers[i].ComparisonFunc = toComparison(layout.samplers[i].compare);
        samplers[i].MaxLOD = layout.samplers[i].maxLod;
        // Read for D3D12_FILTER_ANISOTROPIC only, but always in range: zero is rejected outright,
        // and a rejected static sampler fails the whole root signature rather than one sampler.
        samplers[i].MaxAnisotropy = layout.samplers[i].maxAnisotropy ? layout.samplers[i].maxAnisotropy : 1;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = n;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = sampCount;
    rsd.pStaticSamplers = samplers;
    // A mesh-shader PSO may not use a root signature that declares an input-assembler layout.
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
//
// Textures created with initial data are filled here, on a one-shot command list of their own.
// createTexture is an INIT-TIME call — it runs before beginFrame, so there is no open list to
// record into — and blocking until the copy retires is the honest cost of an asset load.
bool D3D12ResourceFactory::uploadInitialData(ID3D12Resource* res, const D3D12_RESOURCE_DESC& td,
                                             const TextureDesc& d, u32 mips) {
    ID3D12Device* dev = dev_->device_.Get();
    if (packedRowPitch(d.format, 1) == 0) {
        AVER_ERROR("[RHI.D3D12] createTexture: initial data for a format with no CPU footprint");
        return false;
    }
    const u32 count = d.initialDataCount < mips ? d.initialDataCount : mips;

    // The driver, not arithmetic, decides where each subresource sits in an upload buffer and how
    // wide its rows are. Every offset below comes from here.
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
        // Only subresource 0 may name a source pitch; the rest are tightly packed for their own mip
        // extent, which is what a decoder or a mip generator hands over.
        const u64 srcPitch = (s == 0 && d.initialRowPitch) ? d.initialRowPitch
                                                           : packedRowPitch(d.format, fp[s].Footprint.Width);
        const u64 dstPitch = fp[s].Footprint.RowPitch;
        const u64 bytes    = rowBytes[s] < srcPitch ? rowBytes[s] : srcPitch;
        u8* dst = mapped + fp[s].Offset;
        // ROW BY ROW: the destination pitch is aligned to 256 bytes and the source's is not, so one
        // flat memcpy of the whole surface skews the image by a few pixels per scanline.
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

    // Hand the resource over in exactly the state the desc named, so the tracker's seed is true and
    // the module's first barrier is checked against something real.
    const D3D12_RESOURCE_STATES want = toResourceStates(d.initialState);
    if (want != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto b = transition(res, D3D12_RESOURCE_STATE_COPY_DEST, want);
        list->ResourceBarrier(1, &b);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    dev_->queue_->ExecuteCommandLists(1, lists);

    // A fence of its own rather than the frame fence: this runs outside the frame loop and must not
    // move a counter beginFrame reasons about.
    ComPtr<ID3D12Fence> f;
    if (!hrOk(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "rhi upload fence")) return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->queue_->Signal(f.Get(), 1);
    if (f->GetCompletedValue() < 1 && ev) { f->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, INFINITE); }
    if (ev) CloseHandle(ev);

    // Through the normal gate even though the copy has already retired: the staging buffer is not
    // special, and one release path is one place for the lifetime rule to live.
    retire(staging);
    collect();
    return true;
}

TextureHandle D3D12ResourceFactory::createTexture(const TextureDesc& d) {
    collect();
    if (d.width == 0 || d.height == 0) { AVER_ERROR("[RHI.D3D12] createTexture with a zero extent"); return 0; }
    const DXGI_FORMAT fmt = toDxgiFormat(d.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) { AVER_ERROR("[RHI.D3D12] createTexture with an unknown format"); return 0; }

    // A block format can only ever be sampled. Asking for anything else, or for extents that do not
    // tile 4x4, fails inside CreateCommittedResource with a bare E_INVALIDARG that names neither the
    // texture nor the reason, so it is refused here where the message can say which.
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
        // Full chain. A volume's mips halve on all three axes, so the depth counts towards it.
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

    // The clear value is part of the resource: a target cleared to something else loses fast clear
    // and the debug layer says so every frame.
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

    // With initial data the resource is BORN in COPY_DEST and only reaches d.initialState once the
    // upload has landed. The caller never sees that: uploadInitialData does the transition, and the
    // state tracking below is still seeded from d.initialState.
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
            // Spelled out rather than inherited: a typeless resource has no view format to inherit.
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = toDxgiDsvFormat(d.format);
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev_->device_->CreateDepthStencilView(t.res.Get(), &dv, t.dsvHeap->GetCPUDescriptorHandleForHeapStart());
        }
    }

    t.desc = d;
    t.desc.mips = mips;
    t.desc.depth = depth;
    t.desc.debugName = nullptr;   // borrowed pointer; outliving the caller's string is not our call
#if AVER_RHI_TRACK_STATE
    if (d.debugName) t.debugName = d.debugName;
    // Every mip starts in the declared initial state — which is exactly the claim the first barrier
    // of frame 0 will be checked against.
    t.states.assign(mips, d.initialState);
#endif
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

BufferHandle D3D12ResourceFactory::createBuffer(const BufferDesc& d) {
    collect();
    if (d.bytes == 0) { AVER_ERROR("[RHI.D3D12] createBuffer of zero bytes"); return 0; }

    D3D12_RESOURCE_DESC rd = bufferDesc(d.bytes);
    if (d.allowUnorderedAccess || d.kind == BufferKind::AccelStructure)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Three states, and none of them is a choice the caller gets to make. An upload buffer is only
    // ever legal in GENERIC_READ; an acceleration-structure buffer is born in the terminal AS state
    // and never leaves it; and EVERY other buffer is created in COMMON because that is the only
    // thing D3D12 honours for one — anything else is ignored with debug layer #1328, and a resource
    // whose tracked state came from an ignored request is a tracker that reports fiction. See the
    // note on BufferDesc in RHIResources.hpp.
    const bool upload = d.kind == BufferKind::Upload;
    const D3D12_RESOURCE_STATES state =
        upload ? D3D12_RESOURCE_STATE_GENERIC_READ
               : (d.kind == BufferKind::AccelStructure ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                                       : D3D12_RESOURCE_STATE_COMMON);
    RhiBuffer b;
    auto hp = heapProps(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(dev_->device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
              IID_PPV_ARGS(&b.res)), "rhi buffer")) return 0;
    setDebugName(b.res.Get(), d.debugName);
    if (upload) {
        D3D12_RANGE none{0, 0};
        b.res->Map(0, &none, reinterpret_cast<void**>(&b.mapped));
    }
    b.desc = d;
    b.desc.debugName = nullptr;
#if AVER_RHI_TRACK_STATE
    if (d.debugName) b.debugName = d.debugName;
    // Tracked as the state actually created in, which for everything but an acceleration structure
    // is Common — and a plain buffer decays back to Common at the end of every command list, so it
    // is the honest seed rather than merely the initial one.
    b.state = d.kind == BufferKind::AccelStructure ? ResourceState::AccelerationStructure : ResourceState::Common;
    b.stateFixed = upload || d.kind == BufferKind::AccelStructure;
#endif
    buffers_.push_back(std::move(b));
    return static_cast<BufferHandle>(buffers_.size());
}

namespace {
// FXC target for a stage. Mesh has no FXC equivalent at all; it borrows a vertex target purely so
// the SM6 derivation in ShaderCompiler::compile has something well-formed to replace.
const char* fxcTargetFor(ShaderStage s) {
    switch (s) {
        case ShaderStage::Pixel:    return "ps_5_1";
        case ShaderStage::Geometry: return "gs_5_1";
        case ShaderStage::Compute:  return "cs_5_1";
        case ShaderStage::Mesh:
        case ShaderStage::Vertex:   break;
    }
    return "vs_5_1";
}
const char* stagePrefixFor(ShaderStage s) {
    switch (s) {
        case ShaderStage::Pixel:    return "ps";
        case ShaderStage::Geometry: return "gs";
        case ShaderStage::Compute:  return "cs";
        case ShaderStage::Mesh:     return "ms";
        case ShaderStage::Vertex:   break;
    }
    return "vs";
}
} // namespace

ShaderHandle D3D12ResourceFactory::createShader(const ShaderDesc& d) {
    collect();
    if (!d.source || !d.entry) { AVER_ERROR("[RHI.D3D12] createShader without source or entry point"); return 0; }

    // Mesh-shader syntax is only legal from SM 6.5, so a lower request is raised rather than
    // compiled into an error the caller cannot act on.
    u32 model = d.minShaderModel;
    if (d.stage == ShaderStage::Mesh && model < 65) model = 65;
    if (model > dev_->caps_.shaderModel) {
        AVER_WARN("[RHI.D3D12] createShader '{}' wants SM {} but the device reports {}", d.entry, model, dev_->caps_.shaderModel);
        return 0;
    }
    // FXC tops out at SM 5.1 and has no mesh-shader target at all. It DOES take -D macros, so a
    // shader that only needs them is not a reason to demand DXC.
    const bool needsDxc = model > 60 || d.stage == ShaderStage::Mesh;
    if (needsDxc && !dev_->caps_.dxcAvailable) {
        AVER_WARN("[RHI.D3D12] createShader '{}' needs DXC, which is unavailable", d.entry);
        return 0;
    }

    std::string src;
    if (d.prelude) src = d.prelude;
    src += d.source;

    char sm6[16] = {};
    const char* sm6Target = nullptr;
    if (model > 60 || d.stage == ShaderStage::Mesh) {
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

PipelineHandle D3D12ResourceFactory::createGraphicsPipeline(const GraphicsPipelineDesc& d) {
    collect();
    if ((d.vs == 0) == (d.ms == 0)) {
        AVER_ERROR("[RHI.D3D12] createGraphicsPipeline needs exactly one of vs / ms");
        return 0;
    }
    RhiShader* vs = shader(d.vs);
    RhiShader* gs = shader(d.gs);
    RhiShader* ms = shader(d.ms);
    RhiShader* ps = shader(d.ps);
    if ((d.vs && !vs) || (d.gs && !gs) || (d.ms && !ms) || (d.ps && !ps)) {
        AVER_ERROR("[RHI.D3D12] createGraphicsPipeline given an invalid shader handle");
        return 0;
    }

    const RootSigEntry* rs = rootSignature(d.layout, d.ms != 0);
    if (!rs) return 0;

    RhiPipeline p;
    p.rootSig = rs->sig.Get();
    p.mesh = d.ms != 0;
    for (u32 t = 0; t < kBindingTableCount; ++t) { p.srvParam[t] = rs->srvParam[t]; p.uavParam[t] = rs->uavParam[t]; }
    p.srvBaseRegister[1] = d.layout.srvCount;   // table 1 is based immediately above table 0
    p.msVertexParam = rs->msVertexParam;
    p.msIndexParam = rs->msIndexParam;
    p.msCountParam = rs->msCountParam;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) { p.slotParam[i] = rs->slotParam[i]; p.slotDwords[i] = d.layout.constantDwords[i]; }

    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = (d.fill == FillMode::Wireframe) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    raster.CullMode = (d.cull == CullMode::Back) ? D3D12_CULL_MODE_BACK
                    : (d.cull == CullMode::Front) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = d.depthClip ? TRUE : FALSE;
    raster.MultisampleEnable = (d.sampleCount > 1) ? TRUE : FALSE;
    raster.DepthBias = static_cast<INT>(d.depthBias);
    raster.SlopeScaledDepthBias = d.slopeScaledDepthBias;
    // Conservatively rasterising MESH-SHADER output faults WARP: `--warp --ms --gi` dies on the
    // first frame that dispatches MSVoxel with 0xC0000005 inside d3d10warp.dll, at a constant fault
    // offset. It is not the engine's geometry — replacing every emitted triangle with one fixed,
    // tiny, well-inside-NDC triangle reproduces it exactly, and removing either the mesh shader or
    // the conservative flag makes it go away. WARP runs shaders on the CPU in this process, so its
    // faults are ours to survive rather than a driver's to absorb. Dropping the flag for this one
    // combination costs a little voxelisation coverage on the software rasteriser and nothing at
    // all on hardware, where nothing here changes.
    const bool warpMeshConservative = d.conservativeRaster && d.ms != 0 && dev_->softwareAdapter_;
    if (warpMeshConservative && !dev_->warpConsRasterLogged_) {
        dev_->warpConsRasterLogged_ = true;
        AVER_WARN("[RHI.D3D12] conservative rasterisation disabled for mesh-shader pipelines on the "
                  "WARP software rasteriser (it faults); voxel coverage is thinner on this adapter");
    }
    // Silently dropped where unsupported, exactly as the desc promises.
    raster.ConservativeRaster = (d.conservativeRaster && dev_->caps_.conservativeRaster && !warpMeshConservative)
        ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = d.depth.test ? TRUE : FALSE;
    depth.DepthWriteMask = d.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = toComparison(d.depth.op);

    // Set once and used by BOTH the mesh-shader stream and the classic desc below, which is why
    // this is a local rather than written twice: the two paths must be pixel-identical, and the
    // mesh path exists precisely to be a drop-in for the IA one.
    D3D12_BLEND_DESC blend{};
    {
        auto& rt0 = blend.RenderTarget[0];
        rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        switch (d.blend) {
            case BlendMode::Opaque:
                break;                                   // BlendEnable stays FALSE
            case BlendMode::AlphaBlend:
                rt0.BlendEnable = TRUE;
                rt0.SrcBlend  = D3D12_BLEND_SRC_ALPHA;
                rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                rt0.BlendOp   = D3D12_BLEND_OP_ADD;
                // ALPHA IS NOT THE SAME EQUATION AS COLOUR, and the difference is the one everybody
                // gets wrong. src.a + dst.a saturates: two overlapping half-transparent draws give
                // an opaque result, so an offscreen target composited later has wrong coverage
                // wherever anything overlapped. ONE / INV_SRC_ALPHA is the correct accumulation and
                // costs nothing.
                rt0.SrcBlendAlpha  = D3D12_BLEND_ONE;
                rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt0.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                break;
            case BlendMode::PremultipliedAlpha:
                // ONE, not SRC_ALPHA: the source has its alpha folded in already. The alpha channel
                // uses the same accumulation as AlphaBlend and for the same reason.
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
        // Geometry addressed by MeshHandle presents the engine's MeshVertex, which is what an empty
        // vertexLayout selects and what every scene pipeline uses. A pipeline that declares its own
        // layout owns its own buffers too (setVertexBuffer / drawIndexed). drawFullscreen binds no
        // vertex buffer at all and ignores whichever is in force.
        //
        // The element array is a LOCAL that must outlive the desc borrowing it, so it is declared
        // here rather than returned from the helper.
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
    p.srvBaseRegister[1] = d.layout.srvCount;   // table 1 is based immediately above table 0
    for (u32 i = 0; i < kMaxConstantSlots; ++i) { p.slotParam[i] = rs->slotParam[i]; p.slotDwords[i] = d.layout.constantDwords[i]; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = rs->sig.Get();
    cp.CS = {cs->blob->GetBufferPointer(), cs->blob->GetBufferSize()};
    if (!hrOk(dev_->device_->CreateComputePipelineState(&cp, IID_PPV_ARGS(&p.pso)), "rhi compute pipeline")) return 0;
    pipelines_.push_back(std::move(p));
    return static_cast<PipelineHandle>(pipelines_.size());
}

BindingSetHandle D3D12ResourceFactory::createBindingSet(const BindingSetDesc& d) {
    collect();
    const u32 count = d.srvCount + d.uavCount;
    if (count == 0) { AVER_ERROR("[RHI.D3D12] createBindingSet declaring no slots"); return 0; }
    // Rejected, not clamped: a surplus slot has no declared SlotKind, so it could only be null-filled
    // by guessing its dimension -- exactly the Tier 1 hazard SlotKind exists to remove. Failing here
    // is visible; guessing corrupts only on hardware nobody testing this owns.
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
    // A reused range still holds the previous set's descriptors, so this is what makes recycling
    // safe rather than merely cheap.
    nullFill(s);
    bindingSets_.push_back(s);
    return static_cast<BindingSetHandle>(bindingSets_.size());
}

namespace {
// Geometry description shared by the prebuild query and the build itself. The INPUTS keeps a
// pointer to `geo`, so the caller owns both and must keep them together.
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

// The backend owns sizing: how much result and scratch space a build needs is driver-dependent and
// only the prebuild query knows it.
BlasHandle D3D12ResourceFactory::createBlas(MeshHandle mesh) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createBlas without ray-tracing support"); return 0; }
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.D3D12] createBlas with an invalid mesh handle"); return 0; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    if (m.indexCount == 0) { AVER_ERROR("[RHI.D3D12] createBlas for a mesh with no indices"); return 0; }

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = blasInputs(m, geo);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

    RhiBlas b;
    b.mesh = mesh;
    b.as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    // Sized once and kept for the structure's life. A mesh is static, so the size never changes,
    // and reallocating scratch under a build the GPU has not run yet is exactly the hazard the
    // deferred-destroy queue exists to prevent.
    b.scratch = makeAsBuffer(dev_->device_.Get(), info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);
    if (!b.as || !b.scratch) { AVER_ERROR("[RHI.D3D12] createBlas allocation failed"); return 0; }
    blases_.push_back(std::move(b));
    return static_cast<BlasHandle>(blases_.size());
}

TlasHandle D3D12ResourceFactory::createTlas(u32 maxInstances) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createTlas without ray-tracing support"); return 0; }
    if (maxInstances == 0) { AVER_ERROR("[RHI.D3D12] createTlas for zero instances"); return 0; }

    // Sized for the worst case up front, so a per-frame rebuild never has to reallocate anything.
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

// ---- destruction. Nothing is released here: it is queued behind the fence, because the frame
// currently being recorded may still name the resource.
void D3D12ResourceFactory::destroyTexture(TextureHandle h) {
    RhiTexture* t = texture(h);
    if (!t) return;
#if AVER_WITH_IMGUI
    // The UI descriptor pool has no fence gate of its own — freeing a slot makes it immediately
    // reusable by the next font-atlas rebuild. Destroying a texture the UI drew therefore has the
    // same precondition as recreating one a binding set points at: waitIdle() first.
    if (t->uiSrvCpu) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{static_cast<SIZE_T>(t->uiSrvCpu)};
        uiSrvFree(nullptr, cpu, D3D12_GPU_DESCRIPTOR_HANDLE{});
        t->uiSrvCpu = t->uiSrvGpu = 0;
    }
#endif
    retire(t->res);
    retire(t->rtvHeap);
    retire(t->dsvHeap);
    t->res.Reset(); t->rtvHeap.Reset(); t->dsvHeap.Reset();
    collect();
}

void D3D12ResourceFactory::destroyBuffer(BufferHandle h) {
    RhiBuffer* b = buffer(h);
    if (!b) return;
    retire(b->res);
    b->res.Reset();
    b->mapped = nullptr;
    collect();
}

void D3D12ResourceFactory::destroyShader(ShaderHandle h) {
    RhiShader* s = shader(h);
    if (!s) return;
    s->blob.Reset();   // CPU-side bytecode: consumed at PSO creation, never referenced by the GPU
}

void D3D12ResourceFactory::destroyPipeline(PipelineHandle h) {
    RhiPipeline* p = pipeline(h);
    if (!p) return;
    retire(p->pso);
    p->pso.Reset();
    p->rootSig = nullptr;   // the cache keeps the root signature alive for the pipelines sharing it
    collect();
}

void D3D12ResourceFactory::destroyBindingSet(BindingSetHandle h) {
    RhiBindingSet* s = bindingSet(h);
    if (!s) return;
    // Behind the fence, not straight onto the free list: a command list already recorded may still
    // bind the table covering these descriptors, and recycling under it would rewrite live slots.
    pendingRanges_.push_back({s->heapBase, s->srvCount + s->uavCount, retireFence()});
    s->alive = false;
    collect();
}

// ---- population
void D3D12ResourceFactory::setSrv(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setSrv with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] setSrv slot {} past the {} declared", slot, s->srvCount); return; }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = toDxgiSrvFormat(t->desc.format);
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const bool whole = (mip == kAllMips);
    if (t->desc.dim == TextureDim::Tex3D) {
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

void D3D12ResourceFactory::setUav(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setUav with an invalid handle"); return; }
    if (slot >= s->uavCount) { AVER_ERROR("[RHI.D3D12] setUav slot {} past the {} declared", slot, s->uavCount); return; }
    // A UAV always targets exactly one level, so kAllMips has no meaning here.
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

void D3D12ResourceFactory::setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle h) {
    RhiBindingSet* s = bindingSet(set);
    RhiTlas* t = tlas(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setSrvTlas with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] setSrvTlas slot {} past the {} declared", slot, s->srvCount); return; }
    // An acceleration-structure SRV takes a NULL resource: the address lives in the view itself.
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.RaytracingAccelerationStructure.Location = t->as->GetGPUVirtualAddress();
    dev_->device_->CreateShaderResourceView(nullptr, &sv, cpuSlot(s->heapBase + slot));
}

// Upload buffers are mapped for their whole life (see createBuffer), so this is the memcpy it looks
// like. Every rejection below is a case where the alternative is a write that lands somewhere: a
// GPU-local buffer has no CPU pointer at all, and an over-long write past the end of an upload heap
// corrupts whatever the allocator placed after it with no fault to say so.
bool D3D12ResourceFactory::writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) {
    if (h == 0 || h > buffers_.size()) { AVER_ERROR("[RHI.D3D12] writeBuffer with an invalid handle"); return false; }
    RhiBuffer& b = buffers_[h - 1];
    if (!b.mapped) { AVER_ERROR("[RHI.D3D12] writeBuffer on a buffer that is not BufferKind::Upload"); return false; }
    if (!src || bytes == 0) return true;   // writing nothing is not a failure
    if (offset + bytes > b.desc.bytes) {
        AVER_ERROR("[RHI.D3D12] writeBuffer of {} bytes at {} overruns a {}-byte buffer",
                   bytes, offset, b.desc.bytes);
        return false;
    }
    std::memcpy(b.mapped + offset, src, static_cast<usize>(bytes));
    return true;
}

bool D3D12ResourceFactory::textureInfo(TextureHandle h, TextureDesc& out) const {
    const RhiTexture* t = texture(h);
    if (!t) return false;
    out = t->desc;
    return true;
}

void D3D12ResourceFactory::waitIdle() {
    dev_->waitForGpu();
    collect();
}

// The SRV goes in the UI's own shader-visible heap, not the factory's: that heap is the one bound
// when the UI's draw data is recorded, and a descriptor in any other heap is unreachable from there.
u64 D3D12ResourceFactory::uiDescriptor(TextureHandle h) {
#if AVER_WITH_IMGUI
    RhiTexture* t = texture(h);
    if (!t || !t->res) return 0;
    if (t->uiSrvGpu) return t->uiSrvGpu;
    if (!any(t->desc.bind, ResourceBind::ShaderResource)) {
        AVER_ERROR("[RHI.D3D12] uiTextureId on a texture not created as a shader resource");
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uiSrvAlloc(nullptr, &cpu, &gpu);
    if (!gpu.ptr) return 0;   // pool exhausted; uiSrvAlloc has already said so

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = toDxgiSrvFormat(t->desc.format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = t->desc.mips;
    dev_->device_->CreateShaderResourceView(t->res.Get(), &sv, cpu);

    t->uiSrvCpu = cpu.ptr;
    t->uiSrvGpu = gpu.ptr;
    return t->uiSrvGpu;
#else
    (void)h;
    return 0;
#endif
}

// Temporary: proves the factory works end to end before anything depends on it. Deleted once a
// feature module exercises the same path for real.
namespace {
const char* kSelfTestCS = R"(
cbuffer SelfTestCB : register(b0) { uint4 gValue; };
RWTexture3D<float4> gOut : register(u0);
[numthreads(4,4,4)]
void CSSelfTest(uint3 id : SV_DispatchThreadID) { gOut[id] = float4(gValue); }
)";
} // namespace

void D3D12ResourceFactory::selfTest() {
    TextureDesc td{};
    td.dim = TextureDim::Tex3D;
    td.width = td.height = td.depth = 32;
    td.mips = 0;                 // full chain, so the resolved count is worth reporting
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
    // 5.1, so the self-test proves the factory on an FXC-only device too. It is four lines of HLSL
    // that use nothing SM 6.x introduced, and the default of 6.0 made the one path most in need of
    // an end-to-end check the one path that skipped it.
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
    bsd.srvKinds[0] = SlotKind::Texture3D;   // matches the volume above, and exercises the null fill
    bsd.uavKinds[0] = SlotKind::Texture3D;
    const BindingSetHandle set = createBindingSet(bsd);
    if (set && tex) setUav(set, 0, tex, 0);   // the SRV slot stays null-filled on purpose
    AVER_INFO("[RHI.D3D12] factory self-test: binding set {}", set ? "ok" : "FAILED");

    // Descriptor reclaim, safety half: a returned range must NOT be handed out again while its
    // fence is outstanding. Getting this wrong rewrites descriptors a recorded command list still
    // binds, which no probe value could ever show.
    //
    // The other half — that the range IS reused once the fence passes — is deliberately NOT
    // asserted here. Proving it needs a GPU wait, and waiting during init() advances the fence the
    // device's own wait-before-reuse depends on, whose counter createSwapchainResources then
    // rewinds. That desynchronises frame pacing for the first frames of every run.
    const u32 firstBase = set ? bindingSets_[set - 1].heapBase : 0;
    destroyBindingSet(set);
    const BindingSetHandle early = createBindingSet(bsd);
    const bool heldBack = early && bindingSets_[early - 1].heapBase != firstBase;
    AVER_INFO("[RHI.D3D12] factory self-test: descriptor reclaim {} (returned range held behind the fence)",
              heldBack ? "ok" : "FAILED");

    destroyBindingSet(early);
    destroyPipeline(pipe);
    destroyShader(cs);
    destroyBuffer(buf);
    destroyTexture(tex);
}

// ================================================================ generic RHI command recording

void D3D12RenderContext::setPipeline(PipelineHandle h) {
    pipe_ = nullptr;
    RhiPipeline* p = res_->pipeline(h);
    if (!p) { AVER_ERROR("[RHI.D3D12] setPipeline with an invalid handle"); return; }
    if (!dev_->cmdList_) return;
    if (p->compute) {
        dev_->cmdList_->SetComputeRootSignature(p->rootSig);
    } else {
        dev_->cmdList_->SetGraphicsRootSignature(p->rootSig);
        // The frame path caches which graphics root signature is bound; a foreign one must
        // invalidate that cache or the next device draw would skip rebinding its own.
        dev_->boundRootSig_ = nullptr;
    }
    dev_->cmdList_->SetPipelineState(p->pso.Get());
    pipe_ = p;

    bindDeclaredRootCbvs(p);
}

// Binding a root signature discards EVERY root argument, and the cache declares all kMaxConstantSlots
// whether or not the bound shader reads them, so after this point any slot nobody writes is a root
// CBV pointing nowhere. Executing a draw with one is undefined behaviour and the D3D12 debug layer
// cannot see it — it validates API use, and this is not API misuse. On this RDNA part it faulted the
// IA+GS voxelise pipeline outright (device hung, 0x141 TDR) while the mesh pipeline survived and
// merely read zeros, writing fully-opaque black voxels. Same defect, two symptoms.
//
// So every declared slot is given a real address here rather than left to the feature to remember:
// slot 0 gets the engine PerFrame block, which is data no feature owns or could supply, and the rest
// get a zero-filled buffer that setConstants/setConstantBuffer then overwrite as usual. Nothing the
// caller does or forgets can leave an unset root CBV live at a draw.
void D3D12RenderContext::bindDeclaredRootCbvs(const RhiPipeline* p) {
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    const D3D12_GPU_VIRTUAL_ADDRESS zero = zeroCbv();
    for (u32 s = 0; s < kMaxConstantSlots; ++s) {
        if (p->slotParam[s] < 0 || p->slotDwords[s] != 0) continue;   // absent, or root constants
        D3D12_GPU_VIRTUAL_ADDRESS va = zero;
        if (s == kEngineFrameConstantRegister && dev_->frameCBs_[f])
            va = dev_->frameCBs_[f]->GetGPUVirtualAddress();
        if (!va) continue;
        const UINT param = static_cast<UINT>(p->slotParam[s]);
        if (p->compute) dev_->cmdList_->SetComputeRootConstantBufferView(param, va);
        else            dev_->cmdList_->SetGraphicsRootConstantBufferView(param, va);
    }
}

// One 256-byte zeroed upload buffer for the device's lifetime. It is never written after creation,
// so every pipeline that has no data for a declared slot can share it.
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

void D3D12RenderContext::setViewport(u32 x, u32 y, u32 w, u32 h) {
    if (!dev_->cmdList_) return;
    D3D12_VIEWPORT vp{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f};
    dev_->cmdList_->RSSetViewports(1, &vp);
}

void D3D12RenderContext::setScissor(u32 x, u32 y, u32 w, u32 h) {
    if (!dev_->cmdList_) return;
    D3D12_RECT sc{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + w), static_cast<LONG>(y + h)};
    dev_->cmdList_->RSSetScissorRects(1, &sc);
}

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

void D3D12RenderContext::clearDepth(TextureHandle depth, f32 value) {
    RhiTexture* t = res_->texture(depth);
    if (!t || !t->dsvHeap || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] clearDepth on a non-depth texture"); return; }
    dev_->cmdList_->ClearDepthStencilView(t->dsvHeap->GetCPUDescriptorHandleForHeapStart(),
                                          D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
}

void D3D12RenderContext::setBindingSet(BindingSetHandle set, u32 table) {
    if (!pipe_) { AVER_ERROR("[RHI.D3D12] setBindingSet before setPipeline"); return; }
    if (table >= kBindingTableCount) { AVER_ERROR("[RHI.D3D12] setBindingSet table {} past the {} declarable", table, kBindingTableCount); return; }
    RhiBindingSet* s = res_->bindingSet(set);
    if (!s || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setBindingSet with an invalid handle"); return; }

    ID3D12DescriptorHeap* heaps[] = {res_->heap_.Get()};
    dev_->cmdList_->SetDescriptorHeaps(1, heaps);
    // Swapping the bound heap invalidates the frame path's own table bindings as well as its root
    // signature, so its cache has to be dropped alongside.
    dev_->boundRootSig_ = nullptr;

    // A set built for a different base register still binds cleanly and reads the wrong resources,
    // so this is checked rather than trusted. Warn, not refuse: leaving a declared table unbound is
    // undefined at Tier 1, which is strictly worse than binding a suspect one.
    if (s->srvCount && s->srvBaseRegister != pipe_->srvBaseRegister[table])
        AVER_WARN("[RHI.D3D12] binding set was built for t{} but table {} covers t{}",
                  s->srvBaseRegister, table, pipe_->srvBaseRegister[table]);

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

void D3D12RenderContext::setConstants(u32 slot, const void* data, u32 dwords) {
    if (!pipe_ || slot >= kMaxConstantSlots || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setConstants without a pipeline"); return; }
    const i32 param = pipe_->slotParam[slot];
    const u32 declared = pipe_->slotDwords[slot];
    if (param < 0 || declared == 0) {
        AVER_ERROR("[RHI.D3D12] setConstants: slot {} declares constantDwords 0, so it is a root CBV — use setConstantBuffer", slot);
        return;
    }

    // The whole declared block is written every time, zero-filling anything the caller did not
    // supply, so a short write can never leave the previous pass's values in the tail.
    u32 block[64] = {};
    const u32 n = declared < 64 ? declared : 64;
    const u32 copy = dwords < n ? dwords : n;
    if (data && copy) std::memcpy(block, data, copy * sizeof(u32));
    if (pipe_->compute) dev_->cmdList_->SetComputeRoot32BitConstants(static_cast<UINT>(param), n, block, 0);
    else                dev_->cmdList_->SetGraphicsRoot32BitConstants(static_cast<UINT>(param), n, block, 0);
}

void D3D12RenderContext::setConstantBuffer(u32 slot, const void* data, u32 bytes) {
    if (!pipe_ || slot >= kMaxConstantSlots || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setConstantBuffer without a pipeline"); return; }
    const i32 param = pipe_->slotParam[slot];
    if (param < 0 || pipe_->slotDwords[slot] != 0) {
        AVER_ERROR("[RHI.D3D12] setConstantBuffer: slot {} declares {} root constants, not a CBV — use setConstants",
                   slot, pipe_->slotDwords[slot]);
        return;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = ringAlloc(data, bytes);
    if (!va) return;
    if (pipe_->compute) dev_->cmdList_->SetComputeRootConstantBufferView(static_cast<UINT>(param), va);
    else                dev_->cmdList_->SetGraphicsRootConstantBufferView(static_cast<UINT>(param), va);
}

// Sticky per-draw state. Recorded here and applied at the draw, not at the call, so a feature may
// set it before the pipeline it will draw with is even chosen.
void D3D12RenderContext::setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
    if (bytes > kMaxDrawConstantBytes) {
        // Refused rather than clipped: a short block leaves the tail reading whatever the ring held,
        // which shades plausibly and wrongly instead of failing.
        AVER_ERROR("[RHI.D3D12] setDrawBinding constant block is {} bytes, over the {} limit", bytes, kMaxDrawConstantBytes);
        return;
    }
    drawSet_ = set;
    drawConstantBytes_ = (constants && bytes) ? bytes : 0;
    if (drawConstantBytes_) std::memcpy(drawConstants_, constants, drawConstantBytes_);
}

// A pipeline that declared neither table 1 nor a b2 CBV gets nothing, silently: passes with no use
// for per-draw resources (the volume clear, the mip filter) must not have to clear the state.
void D3D12RenderContext::applyDrawBinding() {
    if (!pipe_) return;
    if (drawSet_ && pipe_->srvParam[1] >= 0) setBindingSet(drawSet_, 1);
    if (drawConstantBytes_ && pipe_->slotParam[kDrawConstantRegister] >= 0 &&
        pipe_->slotDwords[kDrawConstantRegister] == 0)
        setConstantBuffer(kDrawConstantRegister, drawConstants_, drawConstantBytes_);
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12RenderContext::ringAlloc(const void* data, u32 bytes) {
    if (!data || bytes == 0) return 0;
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    if (!ring_[f]) {
        auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto rd = bufferDesc(kRhiRingBytes);
        if (!hrOk(dev_->device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ring_[f])), "rhi upload ring")) return 0;
        D3D12_RANGE none{0, 0};
        ring_[f]->Map(0, &none, reinterpret_cast<void**>(&ringPtr_[f]));
    }
    // One reset per frame. The frame loop has no knowledge of this ring yet, so the device's
    // monotonic fence counter is what identifies "a new frame" here.
    if (ringEpoch_ != dev_->nextFence_) { ringEpoch_ = dev_->nextFence_; ringUsed_[f] = 0; }

    const u64 offset = (ringUsed_[f] + 255ull) & ~255ull;   // a root CBV must be 256-byte aligned
    const u64 size = (static_cast<u64>(bytes) + 255ull) & ~255ull;
    if (offset + size > kRhiRingBytes) {
        AVER_ERROR("[RHI.D3D12] upload ring exhausted ({} bytes per frame)", kRhiRingBytes);
        return 0;
    }
    std::memcpy(ringPtr_[f] + offset, data, bytes);
    ringUsed_[f] = offset + size;
    return ring_[f]->GetGPUVirtualAddress() + offset;
}

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

void D3D12RenderContext::dispatch(u32 gx, u32 gy, u32 gz) {
    if (!pipe_ || !pipe_->compute) { AVER_ERROR("[RHI.D3D12] dispatch without a compute pipeline"); return; }
    if (dev_->cmdList_) dev_->cmdList_->Dispatch(gx, gy, gz);
}

void D3D12RenderContext::drawFullscreen() {
    if (!dev_->cmdList_) return;
    // The pipeline's own vertex shader builds the triangle from SV_VertexID; there is nothing to
    // feed the input assembler.
    dev_->cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->cmdList_->IASetVertexBuffers(0, 0, nullptr);
    dev_->cmdList_->DrawInstanced(3, 1, 0, 0);
}

// Caller-owned geometry. These are thin on purpose: the input assembler is stateful in D3D12 too,
// so binding at the bind call and drawing at the draw call is the same shape the API already has,
// and adding a cache of "what is currently bound" here would be a second opinion about it.
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

void D3D12RenderContext::setIndexBuffer(BufferHandle h, Format indexFormat) {
    if (!dev_->cmdList_) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b) { AVER_ERROR("[RHI.D3D12] setIndexBuffer with an invalid handle"); return; }
    // Refused rather than defaulted to 32-bit: a 16-bit buffer read as 32-bit indexes past the end
    // of the vertex buffer, which is a hang on some drivers and garbage geometry on the rest.
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

void D3D12RenderContext::drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    if (!dev_->cmdList_ || indexCount == 0) return;
    applyDrawBinding();
    dev_->cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->cmdList_->DrawIndexedInstanced(indexCount, 1, firstIndex, baseVertex, 0);
}

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
        // Engine matrices are row-major / row-vector (v*M); DXR wants a 3x4 column-vector [R|T],
        // i.e. the transpose of the upper 3x3 with the translation in the last column. Getting this
        // wrong leaves the raster image perfectly correct and puts every ray somewhere else.
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) id.Transform[r][c] = instances[i].world[c * 4 + r];
            id.Transform[r][3] = instances[i].world[12 + r];
        }
        id.InstanceMask = instances[i].mask;
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

// An acceleration structure is created in its state and stays there for life; the API rejects a
// transition into or out of it, so the request is reported and dropped rather than recorded.
namespace {
bool rejectAsState(ResourceState from, ResourceState to, const char* what) {
    if (from != ResourceState::AccelerationStructure && to != ResourceState::AccelerationStructure) return false;
    AVER_ERROR("[RHI.D3D12] {}: AccelerationStructure is terminal and cannot be transitioned", what);
    return true;
}

// Debug-only shadow copy of what state each subresource is actually in, checked against the `from`
// the caller claimed. A whole-resource transition is legal only when EVERY mip is already in
// `from`, so a mip-chain walk that forgets to reconcile one level produces a barrier that is
// silently wrong: the image still renders, and only the tracking says otherwise. Reporting here
// names the resource and the offending mip, which a raw debug-layer message cannot.
//
// The barrier is recorded either way. This validates, it does not second-guess the caller.
void trackTextureBarrier(RhiTexture& t, ResourceState from, ResourceState to, u32 subresource) {
#if AVER_RHI_TRACK_STATE
    const char* name = t.debugName.empty() ? "<unnamed>" : t.debugName.c_str();
    const u32 mips = static_cast<u32>(t.states.size());
    if (subresource == kAllSubresources) {
        for (u32 m = 0; m < mips; ++m) {
            if (t.states[m] == from) continue;
            // One report per barrier: a run of mips left behind is one mistake, not N.
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

void D3D12RenderContext::textureBarrier(TextureHandle h, ResourceState from, ResourceState to, u32 subresource) {
    if (rejectAsState(from, to, "textureBarrier")) return;
    RhiTexture* t = res_->texture(h);
    if (!t || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] textureBarrier with an invalid handle"); return; }
    trackTextureBarrier(*t, from, to, subresource);
    D3D12_RESOURCE_BARRIER b = transition(t->res.Get(), toResourceStates(from), toResourceStates(to));
    // For the single-slice 2D/3D textures this interface exposes, a subresource IS a mip index.
    b.Transition.Subresource = (subresource == kAllSubresources) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : subresource;
    dev_->cmdList_->ResourceBarrier(1, &b);
}

void D3D12RenderContext::bufferBarrier(BufferHandle h, ResourceState from, ResourceState to) {
    if (rejectAsState(from, to, "bufferBarrier")) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] bufferBarrier with an invalid handle"); return; }
    trackBufferBarrier(*b, from, to);
    D3D12_RESOURCE_BARRIER bar = transition(b->res.Get(), toResourceStates(from), toResourceStates(to));
    dev_->cmdList_->ResourceBarrier(1, &bar);
}

void D3D12RenderContext::uavBarrierTexture(TextureHandle h) {
    RhiTexture* t = res_->texture(h);
    if (!t || !dev_->cmdList_) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = t->res.Get();
    dev_->cmdList_->ResourceBarrier(1, &b);
}

void D3D12RenderContext::uavBarrierBuffer(BufferHandle h) {
    RhiBuffer* b = res_->buffer(h);
    if (!b || !dev_->cmdList_) return;
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = b->res.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
}

// Metadata 1 is the ANSI-string form PIX and RenderDoc both understand; no event runtime needed.
void D3D12RenderContext::pushMarker(const char* label) {
    if (!label || !dev_->cmdList_) return;
    dev_->cmdList_->BeginEvent(1, label, static_cast<UINT>(std::strlen(label) + 1));
}

void D3D12RenderContext::popMarker() {
    if (dev_->cmdList_) dev_->cmdList_->EndEvent();
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
