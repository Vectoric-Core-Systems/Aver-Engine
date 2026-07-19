// DirectX 12 backend for Aver.RHI — device, swapchain, depth buffer, a lit-mesh
// pipeline (HLSL Lambert shading compiled at runtime), immediate draw path, an
// offscreen GPU self-test, and a backbuffer capture for verification. Hand-rolled
// D3D12 structs (no d3dx12.h). Falls back to nullptr if D3D12 is unavailable.
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
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
constexpr u32 kSampleCount = 4; // MSAA
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

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
};
cbuffer PerObject : register(b1) {
    float4x4 gWorld;
    float4   gBaseColor;
    float4   gMaterial;   // x=metallic, y=roughness, z=unlit(0/1)
};

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
    float3 direct = (kd * albedo / PI + spec) * lightC * ndl;

    // ambient: sky hemisphere irradiance (linear) + crude spec reflection of the sky
    float3 ambient = kd * albedo * skyColor(N) * gAmbient.r;
    float3 R = reflect(-V, N);
    float3 envSpec = skyColor(R) * fresnelSchlick(ndv, F0) * (1.0 - rough);
    float3 color = direct + ambient + envSpec * 0.35;

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
    f32 fogColor[4]; // a = density
};

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

    ISwapchain* createSwapchain(const SwapchainDesc& d) override {
        if (!createSwapchainResources(d)) return nullptr;
        return new D3D12Swapchain(this);
    }

    void setClearColor(f32 r, f32 g, f32 b, f32 a) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; clear_[3] = a; }

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
    bool createPipeline();
    bool createSwapchainResources(const SwapchainDesc& d);
    void createRenderTargetViews();
    bool createDepthBuffer();
    bool createMsaaColor();
    void moveToNextFrame();
    void waitForGpu();

    ComPtr<IDXGIFactory6> factory_;
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
    u64 fenceValues_[kFrameCount] = {0, 0};
    u32 frameIndex_ = 0;
    u32 rtvSize_ = 0;

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> skyPso_;
    ComPtr<ID3D12PipelineState> wirePso_;
    ComPtr<ID3D12PipelineState> linePso_;
    bool skyEnabled_ = false;
    bool wireframe_ = false;
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

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
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

    // Sensible default light so meshes are lit before the app sets one.
    const f32 d[3] = {0.3f, 0.4f, 0.85f}, c[3] = {1, 1, 1};
    setLight(d, c, 0.15f);

    if (!createPipeline()) return false;

    AVER_INFO("[RHI.D3D12] device ready on adapter '{}'", adapterName_);
    return true;
}

bool D3D12Device::createPipeline() {
    // Root signature: b0 = per-frame CBV, b1 = 20 root constants (world + colour).
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 24;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (!hrOk(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "SerializeRootSignature")) return false;
    if (!hrOk(device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)), "CreateRootSignature")) return false;

    UINT compileFlags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShaderHLSL, std::strlen(kShaderHLSL), "aver.hlsl", nullptr, nullptr, "VSMain", "vs_5_1", compileFlags, 0, &vs, &err))) {
        AVER_ERROR("[RHI.D3D12] VS compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(D3DCompile(kShaderHLSL, std::strlen(kShaderHLSL), "aver.hlsl", nullptr, nullptr, "PSMain", "ps_5_1", compileFlags, 0, &ps, &err))) {
        AVER_ERROR("[RHI.D3D12] PS compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
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
    pso.SampleDesc.Count = kSampleCount;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)), "CreateGraphicsPipelineState")) return false;

    // Sky pipeline: fullscreen triangle, no input layout, no depth.
    ComPtr<ID3DBlob> vsky, psky;
    if (FAILED(D3DCompile(kShaderHLSL, std::strlen(kShaderHLSL), "aver.hlsl", nullptr, nullptr, "VSky", "vs_5_1", compileFlags, 0, &vsky, &err))) {
        AVER_ERROR("[RHI.D3D12] VSky compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return false;
    }
    if (FAILED(D3DCompile(kShaderHLSL, std::strlen(kShaderHLSL), "aver.hlsl", nullptr, nullptr, "PSky", "ps_5_1", compileFlags, 0, &psky, &err))) {
        AVER_ERROR("[RHI.D3D12] PSky compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return false;
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
    sp.SampleDesc.Count = kSampleCount;
    if (!hrOk(device_->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&skyPso_)), "CreateGraphicsPipelineState(sky)")) return false;

    // Wireframe variant of the mesh PSO.
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (!hrOk(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&wirePso_)), "wire pso")) return false;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;

    // Line PSO (grid / gizmo): pos+colour, line list, depth-tested, no depth write.
    ComPtr<ID3DBlob> vln, pln;
    if (FAILED(D3DCompile(kShaderHLSL, std::strlen(kShaderHLSL), "aver.hlsl", nullptr, nullptr, "VSLine", "vs_5_1", compileFlags, 0, &vln, &err))) {
        AVER_ERROR("[RHI.D3D12] VSLine compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return false;
    }
    if (FAILED(D3DCompile(kShaderHLSL, std::strlen(kShaderHLSL), "aver.hlsl", nullptr, nullptr, "PSLine", "ps_5_1", compileFlags, 0, &pln, &err))) {
        AVER_ERROR("[RHI.D3D12] PSLine compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return false;
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
    lp.SampleDesc.Count = kSampleCount;
    if (!hrOk(device_->CreateGraphicsPipelineState(&lp, IID_PPV_ARGS(&linePso_)), "line pso")) return false;

    // Per-frame constant buffers (one per frame in flight), persistently mapped.
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto cbd = bufferDesc(256);
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frameCBs_[i])), "create per-frame CB")) return false;
        D3D12_RANGE none{0, 0};
        frameCBs_[i]->Map(0, &none, reinterpret_cast<void**>(&frameCBPtr_[i]));
    }
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
    cmdList_->Close();

    // Capture readback buffer sized to the backbuffer footprint.
    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    hrOk(device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_)), "capture buffer");

    fenceValues_[frameIndex_] = 1;
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
    td.Format = kDepthFormat; td.SampleDesc.Count = kSampleCount;
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
    td.Format = kBackbufferFormat; td.SampleDesc.Count = kSampleCount;
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

void D3D12Device::beginFrame() {
    if (!hasSwapchain_) return;
    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get());

    // Scene renders into the MSAA color + depth targets; endFrame resolves to backbuffer.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    if (!skyEnabled_) cmdList_->ClearRenderTargetView(rtv, clear_, 0, nullptr); // sky covers all pixels
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp{0, 0, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);

    std::memcpy(frameCBPtr_[frameIndex_], &frameCB_, sizeof(PerFrameCB));
    cmdList_->SetGraphicsRootSignature(rootSig_.Get());
    cmdList_->SetGraphicsRootConstantBufferView(0, frameCBs_[frameIndex_]->GetGPUVirtualAddress());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

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
    const GpuMesh& m = meshes_[mesh - 1];
    cmdList_->SetPipelineState(wireframe_ ? wirePso_.Get() : pso_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    f32 consts[24];
    std::memcpy(consts, world, 16 * sizeof(f32));
    std::memcpy(consts + 16, color, 4 * sizeof(f32));
    consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
    cmdList_->SetGraphicsRoot32BitConstants(1, 24, consts, 0);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->IASetIndexBuffer(&m.ibv);
    cmdList_->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
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
    const GpuLineMesh& m = lineMeshes_[mesh - 1];
    cmdList_->SetPipelineState(linePso_.Get());
    cmdList_->SetGraphicsRoot32BitConstants(1, 16, world, 0); // gWorld only
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->DrawInstanced(m.count, 1, 0, 0);
}

void D3D12Device::endFrame() {
    if (!hasSwapchain_) return;
    ID3D12Resource* bb = renderTargets_[frameIndex_].Get();

    // Resolve the MSAA scene target into the (single-sample) backbuffer.
    D3D12_RESOURCE_BARRIER pre[2] = {
        transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
        transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST),
    };
    cmdList_->ResourceBarrier(2, pre);
    cmdList_->ResolveSubresource(bb, 0, msaaColor_.Get(), 0, kBackbufferFormat);
    D3D12_RESOURCE_BARRIER post[2] = {
        transition(bb, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
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
    moveToNextFrame();
}

void D3D12Device::resize(u32 w, u32 h) {
    if (!hasSwapchain_ || w == 0 || h == 0 || (w == width_ && h == height_)) return;
    waitForGpu();
    for (auto& rt : renderTargets_) rt.Reset();
    depthBuffer_.Reset();
    msaaColor_.Reset();
    if (!hrOk(swapChain_->ResizeBuffers(kFrameCount, w, h, kBackbufferFormat, 0), "ResizeBuffers")) return;
    width_ = w; height_ = h;
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
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

void D3D12Device::moveToNextFrame() {
    const u64 current = fenceValues_[frameIndex_];
    queue_->Signal(fence_.Get(), current);
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < fenceValues_[frameIndex_]) {
        fence_->SetEventOnCompletion(fenceValues_[frameIndex_], fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    fenceValues_[frameIndex_] = current + 1;
}

void D3D12Device::waitForGpu() {
    if (!queue_ || !fence_ || !fenceEvent_) return;
    const u64 v = fenceValues_[frameIndex_];
    if (FAILED(queue_->Signal(fence_.Get(), v))) return;
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    fenceValues_[frameIndex_] = v + 1;
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
