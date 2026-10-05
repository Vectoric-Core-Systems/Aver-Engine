// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// DirectX 12 backend for Aver.RHI: device, swapchain, scene pipelines, the camera post chain,
// and the generic resource factory and render context. Hand-rolled D3D12 structs (no d3dx12.h).
#include "aver/rhi/RHI.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/ShaderCacheSweep.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
#include "aver/rhi/EditorLines.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/CrashReport.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <string>
#include <wrl/client.h>

#include <algorithm>   // std::find (the once-per-shape binding warning) and std::sort (the blended-mesh flush's back-to-front replay)
#include <cmath>
#include <cstdio>
#include <cstdlib>    // std::getenv (AVER_D3D12_ELIDE_DRAW_BINDING -- see applyDrawBinding)
#include <cstring>
#include <deque>      // pipelines_ -- see its declaration for why it is not a vector
#include <functional> // D3D12ResourceFactory::uploadBufferFilled's fill callback
#include <initializer_list>   // D3D12ResourceFactory::uploadBuffers' parameter (W4 Default-heap meshes)
#include <string>
#include <utility>
#include <vector>
#include <atomic>               // the present thread's counters
#include <condition_variable>   // ...and its request queue
#include <mutex>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION   // Windows 10 1803+; older SDK headers lack the name
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Seam a UI toolkit's D3D12 backend plugs into; no ImGui symbol lives in this file -- see
// UiBackend.hpp; Dear ImGui itself is in modules/rhi.d3d12.imgui.
#include "aver/rhi/d3d12/UiBackend.hpp"

using Microsoft::WRL::ComPtr;

namespace aver::rhi {

// Shader blob cache disk budget: 256 MB. See ShaderCacheSweep.hpp.
static constexpr u64 kShaderCacheBudgetBytes = 256ull * 1024ull * 1024ull;
namespace {

constexpr u32 kFrameCount = 2;
// Render targets: decoupled from kFrameCount and swapchain. kBackBufferCount allows frame interpolation.
constexpr u32 kBackBufferCount = 4;
constexpr u32 kSwapBufferCount = 3;
constexpr u32 kDefaultSampleCount = 4;
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
// Typeless depth for DSV and SRV views. See createDepthBuffer.
constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;

// Scene target format: linear HDR radiance, which the post chain tonemaps into the backbuffer.
constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// G-buffer target formats. See IDevice::setGBufferEnabled (RHI.hpp) for units and format requirements.
constexpr DXGI_FORMAT kGBufVelocityFormat    = DXGI_FORMAT_R16G16_FLOAT;       // Format::RG16F
constexpr DXGI_FORMAT kGBufViewZFormat       = DXGI_FORMAT_R32_FLOAT;          // Format::R32Float
constexpr DXGI_FORMAT kGBufNormalRoughFormat = DXGI_FORMAT_R10G10B10A2_UNORM;  // Format::RGB10A2Unorm

// G-buffer "nothing here" clear values. See createGBufferTargets for why each value was chosen.
constexpr f32 kGBufVelocityClear[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
constexpr f32 kGBufViewZClear[4]       = {0.0f, 0.0f, 0.0f, 0.0f};
constexpr f32 kGBufNormalRoughClear[4] = {0.5f, 0.5f, 0.5f, 0.0f};

// Bloom pyramid depth cap.
constexpr u32 kMaxBloomMips = 6;
// Post heap descriptor triples: prefilter, histogram, composite, downsample/upsample. +1 for CompositeUpscaled (AverSR).
constexpr u32 kPostTripleCount = 4 + (kMaxBloomMips - 1) * 2;
// + the histogram/exposure/local-exposure-grid/local-exposure-grid-blur UAVs (u0-u3).
constexpr u32 kPostDescriptorCount = kPostTripleCount * 3 + 4;
// Which triple is which. The bloom ones are ranges based at these.
constexpr u32 kPostTriplePrefilter = 0;
constexpr u32 kPostTripleHistogram = 1;
constexpr u32 kPostTripleComposite = 2;
// AverSR twin of kPostTripleComposite -- see kPostTripleCount's comment above.
constexpr u32 kPostTripleCompositeUpscaled = 3;
constexpr u32 kPostTripleDownBase  = 4;
constexpr u32 kPostTripleUpBase    = kPostTripleDownBase + (kMaxBloomMips - 1);
// The luminance window the histogram bins over, in log2. Anything outside lands in the end bins.
constexpr f32 kHistogramMinLogLum = -10.0f;
constexpr f32 kHistogramMaxLogLum = 12.0f;
// One histogram thread per four pixels each way.
constexpr u32 kHistogramDownscale = 4;

// Local exposure's bilateral grid (see PSComposite / CSLocalGrid / CSLocalBlur in post.hlsl): one
// tile per kLocalExpTile scene pixels square, kLocalExpBins log-luminance bins per tile,
// kLocalExpCellBytes = asuint(sum of log2 luminance) + asuint(pixel count) per bin.
constexpr u32 kLocalExpTile      = 32;
constexpr u32 kLocalExpBins      = 16;
constexpr u32 kLocalExpCellBytes = 8;

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

    // Compiles one entry point to bytecode. `target51` is the FXC target; `sm6` overrides the derived
    // SM6 target and requires DXC. `define` is a semicolon-separated -D list.
    // Blob cache key includes all input and hashes every shader file (prelude changes must invalidate).
    // kCacheVersion: bump when shipped compiler changes.
    static constexpr u32 kCacheVersion = 1;

    static u64 cacheKey(const char* src, const char* entry, const char* target, const std::vector<std::string>& defs) {
        u64 h = 0xcbf29ce484222325ull;
        const auto mix = [&h](const char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) { h ^= static_cast<unsigned char>(p[i]); h *= 0x100000001b3ull; }
            h ^= 0xffu; h *= 0x100000001b3ull;   // a field separator, so "ab"+"c" != "a"+"bc"
        };
        const u32 v = kCacheVersion;
        mix(reinterpret_cast<const char*>(&v), sizeof v);
        // Every shader file (memoised, one directory walk per process).
        const u64 corpus = shaderCorpusHash();
        mix(reinterpret_cast<const char*>(&corpus), sizeof corpus);
        mix(src, std::strlen(src));
        mix(entry, std::strlen(entry));
        mix(target, std::strlen(target));
        for (const std::string& d : defs) mix(d.data(), d.size());
        return h;
    }

    static std::string cachePath(u64 key) {
        const std::string dir = aver::userDataDir();
        if (dir.empty()) return {};
        char name[32];
        std::snprintf(name, sizeof name, "%016llx.dxil", static_cast<unsigned long long>(key));
        // Use filesystem::path join to avoid escape-sequence pitfalls.
        return (std::filesystem::path(dir) / "ShaderCache" / name).string();
    }

    // Running totals for shader-compile cost (reported on power-of-two cadence).
    static inline u32 s_compiles = 0;
    static inline f64 s_compileMs = 0.0;
    static inline u32 s_cacheHits = 0;

    HRESULT compile(const char* src, const char* entry, const char* target51, ID3DBlob** out,
                    const char* sm6 = nullptr, const char* define = nullptr) {
        init();
        const auto t0 = std::chrono::steady_clock::now();
        struct Report {
            std::chrono::steady_clock::time_point t0;
            ~Report() {
                s_compileMs += std::chrono::duration<f64, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
                ++s_compiles;
                if ((s_compiles & (s_compiles - 1)) == 0)
                    AVER_INFO("[RHI.D3D12] {} shader request(s): {} served from the blob cache, "
                              "{} compiled in {:.0f} ms",
                              s_compiles, s_cacheHits, s_compiles - s_cacheHits, s_compileMs);
            }
        } report{t0};
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
        // AVER_HLSL_2018 is a flag: compiles as HLSL 2018 instead of 2021 (for vendored source only).
        bool hlsl2018 = false;
        for (usize i = 0; i < wDefines.size();) {
            if (wDefines[i] == L"AVER_HLSL_2018") {
                hlsl2018 = true;
                wDefines.erase(wDefines.begin() + static_cast<isize>(i));
            } else {
                ++i;
            }
        }

        DxcBuffer buf{src, std::strlen(src), DXC_CP_UTF8};
        std::vector<LPCWSTR> args = {
            L"-E", wEntry.c_str(),
            L"-T", wTarget.c_str(),
            L"-Zpr",
            L"-HV", hlsl2018 ? L"2018" : L"2021",
            // Angled includes need -I list. DxcShaderInclude strips "./" and resolves against bin/shaders.
            L"-I", L".",
        };
        // AVER_ENABLE_16BIT_TYPES is a flag: ask DXC for real fp16 (-enable-16bit-types).
        // Opt-in per shader (opt-in changes what `half` means globally, so must not be global).
        bool want16Bit = false;
        for (usize i = 0; i < wDefines.size();) {
            if (wDefines[i] == L"AVER_ENABLE_16BIT_TYPES") {
                want16Bit = true;
                wDefines.erase(wDefines.begin() + static_cast<isize>(i));
            } else {
                ++i;
            }
        }
        if (want16Bit) args.push_back(L"-enable-16bit-types");
        for (const std::wstring& d : wDefines) { args.push_back(L"-D"); args.push_back(d.c_str()); }
        // Custom include handler for hot reload and per-file caching. Cache miss falls through to compile.
        const u64 ckey = cacheKey(src, entry, t6.c_str(), defs);
        if (const std::string cp = cachePath(ckey); !cp.empty()) {
            std::ifstream f(cp, std::ios::binary | std::ios::ate);
            if (f) {
                const std::streamoff n = f.tellg();
                if (n > 0) {
                    f.seekg(0);
                    if (SUCCEEDED(D3DCreateBlob(static_cast<SIZE_T>(n), out)) &&
                        f.read(static_cast<char*>((*out)->GetBufferPointer()), n)) {
                        ++s_cacheHits;
                        return S_OK;
                    }
                    if (*out) { (*out)->Release(); *out = nullptr; }
                }
            }
        }

        DxcShaderInclude includes(utils_.Get());
        ComPtr<IDxcResult> result;
        HRESULT hr = compiler_->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), &includes, IID_PPV_ARGS(&result));
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
        // Best-effort cache write; failures only cost a recompile next launch.
        if (const std::string cp = cachePath(ckey); !cp.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(cp).parent_path(), ec);
            std::ofstream w(cp, std::ios::binary | std::ios::trunc);
            if (w) w.write(static_cast<const char*>(obj->GetBufferPointer()),
                           static_cast<std::streamsize>(obj->GetBufferSize()));
        }
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
D3D12_RESOURCE_BARRIER transitionAt(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                                    int line) {
    // A NULL barrier removes the device a frame later with no pointer back here; name the line now.
    if (!res) AVER_ERROR("[RHI.D3D12] transition barrier on a NULL resource at D3D12Device.cpp:{}", line);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}
#define transition(r, b, a) transitionAt((r), (b), (a), __LINE__)

// Single-node heap properties of the given type.
D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

// ImGui UI functions moved to modules/rhi.d3d12.imgui. IUiBackend plugs in the Win32 thunk and descriptor sharing.

// Writes shading model and parameters into the per-draw b1 block. Must reach material shader (not just gMaterial.z).
void writeShadingConstants(f32* block, bool unlit = false) {
    const u32 model = unlit ? 1u : 0u;   // AVER_MODEL_UNLIT / AVER_MODEL_STANDARD in the prelude
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

// Scene/sky/line shading in modules/rhi/shaders/scene.hlsl (shared by both backends).
// Cached and keyed on shaderFileRevision() for hot reload (not function-local static).
const std::string& sceneShaderSource() {
    static std::string src;
    static u64 built = ~0ull;
    if (built != shaderFileRevision()) {
        src = std::string(sharedShaderPrelude()) + shaderFile("scene.hlsl");
        built = shaderFileRevision();
    }
    return src;
}

// PerFrameCB and PostCB in aver/rhi/FrameConstants.hpp (shared with Vulkan backend). See that header.
// Per-frame upload ring for the block above.
constexpr u32 kPostConstantRingBytes = 16 * 1024;

// MeshVertex layout for D3D12 IA pipelines. Uses offsetof (not literals) to catch field-order changes.
constexpr D3D12_INPUT_ELEMENT_DESC kMeshInputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, px), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, nx), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(MeshVertex, u),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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
    // Non-zero when a compute pass writes these vertices (see IDevice::createSkinTargetMesh).
    BufferHandle vbBuffer = 0;
    // Index buffer as RHI buffer; allows shaders to read geometry via descriptors.
    BufferHandle ibBuffer = 0;
    u32 vertexCount = 0;
    // Whether a compute pass writes these vertices (prevents spurious BLAS rebuilds).
    bool computeWritten = false;
    // Local-space bounding sphere (AABB midpoint and distance to corner).
    f32 boundsCentre[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsRadius = 0.0f;
    // AABB for exact point-in-bounds tests (sphere over-answers for wide shallow shapes).
    f32 boundsMin[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsMax[3] = {0.0f, 0.0f, 0.0f};

    // Index-buffer sharing for skin targets (own vertices, share source indices).
    // ibOwned false on sharer (never frees); ibShares counts live sharers on SOURCE.
    bool ibOwned = true;
    u32  ibShares = 0;
    MeshHandle ibSource = 0;

    // Vertex-buffer sharing for LOD meshes (LODs share vertex stream, differ in indices).
    // vbOwned false on sharer (never frees); vbShares counts live sharers on ROOT.
    bool vbOwned = true;
    u32  vbShares = 0;
    MeshHandle vbSource = 0;

    // False once destroyMesh releases this slot. Slot itself is kept for stale handle safety.
    bool alive = true;
};

// ---------------------------------------------------------------- generic RHI mapping

// Format mapping helper.
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
        // G-buffer formats (see IDevice::setGBufferEnabled).
        case Format::RG16F:          return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGB10A2Unorm:   return DXGI_FORMAT_R10G10B10A2_UNORM;
        // Single-channel pool formats.
        case Format::R16Unorm:       return DXGI_FORMAT_R16_UNORM;
        case Format::R16F:           return DXGI_FORMAT_R16_FLOAT;
        case Format::R8Uint:         return DXGI_FORMAT_R8_UINT;
        case Format::R16Uint:        return DXGI_FORMAT_R16_UINT;
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
        case DXGI_FORMAT_R16G16_FLOAT:        return Format::RG16F;
        case DXGI_FORMAT_R10G10B10A2_UNORM:   return Format::RGB10A2Unorm;
        case DXGI_FORMAT_R16_UNORM:           return Format::R16Unorm;
        case DXGI_FORMAT_R16_FLOAT:           return Format::R16F;
        case DXGI_FORMAT_R8_UINT:             return Format::R8Uint;
        case DXGI_FORMAT_R16_UINT:            return Format::R16Uint;
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
        case Format::R32Typeless:
        case Format::RG16F:          // 2 x half-float
        case Format::RGB10A2Unorm:   return 4;   // packed 10-10-10-2
        case Format::RG8Unorm:
        case Format::R16Unorm:
        case Format::R16F:
        case Format::R16Uint:        return 2;
        case Format::R8Unorm:
        case Format::R8Uint:         return 1;
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

// Tightly packed bytes in one source row this wide -- one row of BLOCKS for a block format, the
// unit the upload loop counts rows in.
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
        // Geometry readers need both VERTEX_AND_CONSTANT_BUFFER and NON_PIXEL_SHADER_RESOURCE.
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

// ---- DRED (--dred) name decoding -----------------------------------------------------------
// Both used only by D3D12Device::dumpDredOnDeviceRemoved, after the device that would otherwise
// answer these enums with something friendlier is already gone.

// Readable name for a DRED breadcrumb op this engine's recording can produce (draws, dispatches,
// mesh dispatches, RT AS builds, barriers/clears/copies/markers, ExecuteIndirect); anything else
// is printed by its raw D3D12_AUTO_BREADCRUMB_OP value in the caller.
const char* dredOpName(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                            return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:                           return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                             return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:                        return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:                 return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                             return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH:                         return "DispatchMesh";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:                         return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:                   return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:                      return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:                return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:                      return "ExecuteIndirect";
    default: return nullptr;
    }
}

// Readable name for a DRED page-fault allocation's type. Covers what this renderer actually
// allocates (RESOURCE covers every buffer/texture; STATE_OBJECT is the RT pipelines' own kind, not
// a PIPELINE_STATE); anything else is printed by its raw D3D12_DRED_ALLOCATION_TYPE value.
const char* dredAllocTypeName(D3D12_DRED_ALLOCATION_TYPE t) {
    switch (t) {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:     return "CommandQueue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:    return "PipelineState";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:      return "CommandList";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE:             return "Fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:   return "DescriptorHeap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP:              return "Heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:        return "QueryHeap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "CommandSignature";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:          return "Resource";
    case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT:      return "StateObject (RT pipeline)";
    default: return nullptr;
    }
}

// Narrow wide debug name to UTF-8-ish ASCII for logging (best-effort, wcstombs).
std::string dredNarrow(const wchar_t* w) {
    if (!w) return "<unnamed>";
    char buf[256];
    const usize n = std::wcstombs(buf, w, sizeof(buf) - 1);
    buf[n == static_cast<usize>(-1) ? 0 : n] = '\0';
    return buf;
}

// Log unfinished DRED breadcrumb node (returns false for clean nodes). Templated for NODE1 and NODE.
template <typename NodeT, typename ContextFn>
bool logBreadcrumbNodeIfUnfinished(u32 nodeIndex, const NodeT* node, ContextFn&& contextFor) {
    const UINT count = node->BreadcrumbCount;
    const UINT* lastPtr = node->pLastBreadcrumbValue;
    if (lastPtr && *lastPtr >= count) return false;   // every recorded op reported complete
    const UINT last = lastPtr ? *lastPtr : 0;
    AVER_ERROR("[RHI.D3D12][DRED] node {}: command list '{}' on queue '{}' -- {} of {} recorded ops "
               "completed{}", nodeIndex, dredNarrow(node->pCommandListDebugNameW),
               dredNarrow(node->pCommandQueueDebugNameW), last, count,
               lastPtr ? "" : " (driver never reported a completed count for this list)");
    // Show ops around the stop point (not the entire rest of the list).
    const UINT windowStart = last > 3 ? last - 3 : 0;
    const UINT windowEnd = (count == 0) ? 0 : (last < count ? last : count - 1);
    for (UINT i = windowStart; i <= windowEnd && i < count; ++i) {
        const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
        const char* opName = dredOpName(op);
        const std::string ctx = contextFor(i);
        const char* tag = (lastPtr && i >= last) ? "  <-- DID NOT COMPLETE" : "";
        if (opName) AVER_ERROR("[RHI.D3D12][DRED]   [{}] {}{}{}", i, opName, ctx, tag);
        else        AVER_ERROR("[RHI.D3D12][DRED]   [{}] op#{}{}{}", i, static_cast<int>(op), ctx, tag);
    }
    return true;
}

// Log DRED allocation list (existing or recently-freed). One line per node or "none reported".
template <typename AllocNodeT>
void logDredAllocationList(const char* label, const AllocNodeT* head) {
    u32 n = 0;
    for (const AllocNodeT* a = head; a; a = a->pNext, ++n) {
        const char* typeName = dredAllocTypeName(a->AllocationType);
        if (typeName) AVER_ERROR("[RHI.D3D12][DRED]   {} #{}: '{}' ({})", label, n, dredNarrow(a->ObjectNameW), typeName);
        else          AVER_ERROR("[RHI.D3D12][DRED]   {} #{}: '{}' (type {})", label, n, dredNarrow(a->ObjectNameW), static_cast<int>(a->AllocationType));
    }
    if (n == 0) AVER_INFO("[RHI.D3D12][DRED]   {}: none reported", label);
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

    // Plug in UI toolkit's D3D12 backend (see installUiBackend in UiBackend.hpp). Non-owning.
    void setUiBackend(d3d12::IUiBackend* backend) { uiBackend_ = backend; }

    Backend backend() const override { return Backend::D3D12; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }

    // ----- generic RHI surface (render-feature modules) -----
    Format backbufferFormat() const override { return fromDxgiFormat(kSceneColorFormat); }
    Format depthFormat() const override { return fromDxgiFormat(kDepthFormat); }
    // Out-of-line: calls D3D12ResourceFactory (type not visible here).
    TextureHandle sceneDepthTexture() override;
    TextureHandle sceneColorBackdropTexture() override { return blendBackdropTex_; }

    // G-buffer: velocity + view-space depth + normal/roughness.
    void setGBufferEnabled(bool on) override;
    bool gBufferEnabled() const override { return gbufferEnabled_; }
    bool gBufferWritten() const override {
        return gbufferEnabled_ && gbufVelocity_ &&
               (sampleCount_ == 1 || (gbufVelocityMs_ && !gbufResolveFailed_));
    }
    TextureHandle gBufferVelocityTexture() override;
    TextureHandle gBufferViewZTexture() override;
    TextureHandle gBufferNormalRoughnessTexture() override;
    // Previous viewProj snapshot (same setCamera convention).
    bool gBufferPrevViewProj(f32 out[16]) const override {
        if (!gbufferEnabled_) return false;
        if (out) std::memcpy(out, prevViewProj_, sizeof(prevViewProj_));
        return true;
    }
    bool gBufferHistoryInvalid() const override { return !gbufferEnabled_ || gbufHistoryInvalid_; }

    // Queries adapter3_ (filled after D3D12CreateDevice).
    VideoMemoryInfo videoMemory() const override;

    IResourceFactory* resources() override;
    // Out-of-line: RenderContext forward-declared.
    IRenderContext* renderContext() override;
    // Non-owning. Registering the same feature twice is ignored.
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    // Non-owning. Null keeps single-pass composite path.
    // AverSR's targets exist only while an upscaler is set and are built with the post targets, so
    // attaching or detaching one rebuilds them at the next frame start, like a render-scale change.
    void setUpscaler(IUpscaler* u) override {
        if ((u != nullptr) != (upscaler_ != nullptr) && hasSwapchain_) upscalerTargetsDirty_ = true;
        upscaler_ = u;
    }
    IUpscaler* upscaler() const override { return upscaler_; }
    void setFrameInterpolator(IFrameInterpolator* g) override {
        if (g != frameInterp_ && frameInterp_) frameInterp_->reset();
        frameInterp_ = g;
        frameInterpCut_ = true;
    }
    void setFrameInterpolation(bool on) override {
        if (on != frameInterpOn_) frameInterpCut_ = true;
        frameInterpOn_ = on;
    }
    bool frameInterpolation() const override { return frameInterpOn_; }
    bool frameInterpolated() const override { return frameInterpolated_; }
    void setFrameInterpCaptureGenerated(bool on) override { fgCaptureGenerated_ = on; }
    void setFrameInterpShowGeneratedOnly(bool on) override { fgShowGeneratedOnly_ = on; }
    f32 displayRefreshRate() const override { return fgDisplayHz_; }
    void frameMidpoint() override;
    // Presentation sync: vsync=1 or torn if supported.
    UINT presentSync() const { return vsync_ ? 1u : (tearingSupported_ ? 0u : 1u); }
    UINT presentFlags() const { return (!vsync_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u; }
    void noteSceneCut() override { frameInterpCut_ = true; }
    void notifyRenderTargetsChanged();
    // Creates or resizes the factory texture the scene renders into for the viewport.
    bool ensureViewportTexture();
    bool           viewportToTex_ = false;
    TextureHandle  viewportTex_ = 0;
    u32            viewportTexW_ = 0, viewportTexH_ = 0;
    // Last notification state (sample count 0 forces initial notification).
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
    bool setMirrorWindow(void* windowHandle, u32 width, u32 height) override;

    void setViewportRect(u32 x, u32 y, u32 w, u32 h) override {
        if (w == 0 || h == 0 || x >= width_ || y >= height_) { vpX_ = vpY_ = vpW_ = vpH_ = 0; return; }
        const u32 cw = (x + w > width_) ? width_ - x : w;
        const u32 ch = (y + h > height_) ? height_ - y : h;
        // Convert present-space to scene-space pixels (scaled by renderScale_).
        vpX_ = scaleToSceneW(x);  vpY_ = scaleToSceneH(y);
        vpW_ = scaleToSceneW(cw); vpH_ = scaleToSceneH(ch);
        if (vpW_ == 0) vpW_ = 1;
        if (vpH_ == 0) vpH_ = 1;
    }

    // Viewport aspect in scene-space.
    f32 viewportAspect() const override {
        const u32 w = vpW_ ? vpW_ : sceneWidth_;
        const u32 h = vpH_ ? vpH_ : sceneHeight_;
        return h ? static_cast<f32>(w) / static_cast<f32>(h) : 0.0f;
    }

    void setCamera(const f32 viewProj[16], const f32 invViewProjRel[16], const f32 camPos[3]) override {
        // Snapshot previous viewProj before overwrite (gbufCameraPrimed_ guards from zero state).
        if (gbufCameraPrimed_) std::memcpy(prevViewProj_, frameCB_.viewProj, sizeof(prevViewProj_));
        gbufCameraPrimed_ = true;

        std::memcpy(frameCB_.viewProj, viewProj, sizeof(frameCB_.viewProj));
        std::memcpy(frameCB_.viewProjNoJitter, viewProj, sizeof(frameCB_.viewProjNoJitter));
        std::memcpy(frameCB_.invViewProjRel, invViewProjRel, sizeof(frameCB_.invViewProjRel));
        frameCB_.camPos[0] = camPos[0]; frameCB_.camPos[1] = camPos[1]; frameCB_.camPos[2] = camPos[2]; frameCB_.camPos[3] = 1;
    }
    // TEMPORAL AA JITTER, applied to the UPLOADED copy only: frameCB_ (and so camera(), picking,
    // culling and every renderer's own previous-frame chain) stays unjittered. An 8-step Halton(2,3)
    // offset within the scene pixel, written into viewProj (clip.xy += j * clip.w) and its inverse,
    // so raster draws and ray-driven primary rays both sample it. Velocity written against an
    // unjittered previous matrix therefore carries +jitter, which the resolve subtracts.
    void jitterForUpload(PerFrameCB& cb) {
        cb.jitter[0] = cb.jitter[1] = cb.jitter[2] = cb.jitter[3] = 0.0f;
        // Camera motion: compared on the unjittered matrix, once per uploaded frame. No jitter while
        // moving -- the temporal upscaler resolves without history then (UpscalerInput::cameraMoving).
        taaCameraMoving_ = taaLastViewProjValid_ &&
                           std::memcmp(taaLastViewProj_, frameCB_.viewProj, sizeof(taaLastViewProj_)) != 0;
        std::memcpy(taaLastViewProj_, frameCB_.viewProj, sizeof(taaLastViewProj_));
        taaLastViewProjValid_ = true;
        if (taaCameraMoving_ || !upscaler_ || jitterSuppressed_) return;
        f32 jx = 0.0f, jy = 0.0f;   // scene pixels
        if (!upscaler_->jitterOverride(jx, jy)) {
            if (!any(upscaler_->needs(), UpscalerNeeds::Jitter)) return;
            auto halton = [](u32 i, u32 b) { f32 f = 1.0f, r = 0.0f; for (; i; i /= b) { f /= b; r += f * (i % b); } return r; };
            const u32 i = (taaJitterIndex_++ % 8u) + 1u;
            jx = halton(i, 2) - 0.5f; jy = halton(i, 3) - 0.5f;
        }
        const f32 w = static_cast<f32>(vpW_ ? vpW_ : sceneWidth_), h = static_cast<f32>(vpH_ ? vpH_ : sceneHeight_);
        if (w <= 0.0f || h <= 0.0f) return;
        const f32 nx = 2.0f * jx / w, ny = -2.0f * jy / h;            // NDC, y up
        for (u32 r = 0; r < 4; ++r) {                                    // viewProj * T
            cb.viewProj[r * 4 + 0] += cb.viewProj[r * 4 + 3] * nx;
            cb.viewProj[r * 4 + 1] += cb.viewProj[r * 4 + 3] * ny;
        }
        for (u32 c = 0; c < 4; ++c)                                      // T^-1 * invViewProjRel
            cb.invViewProjRel[12 + c] -= nx * cb.invViewProjRel[c] + ny * cb.invViewProjRel[4 + c];
        cb.jitter[0] = jx; cb.jitter[1] = jy;
    }
    void setJitterSuppressed(bool on) override { jitterSuppressed_ = on; }
    bool jitterSuppressed_ = false;
    u32 taaJitterIndex_ = 0;
    f32 taaJitterThisFrame_[2] = {0.0f, 0.0f};   // what this frame's upload carried, for the upscaler
    f32 taaLastViewProj_[16] = {};
    bool taaLastViewProjValid_ = false;
    bool taaCameraMoving_ = false;
    bool camera(f32 viewProj[16], f32 invViewProjRel[16], f32 cameraPos[3]) const override {
        if (viewProj)       std::memcpy(viewProj, frameCB_.viewProj, sizeof(frameCB_.viewProj));
        if (invViewProjRel) std::memcpy(invViewProjRel, frameCB_.invViewProjRel, sizeof(frameCB_.invViewProjRel));
        if (cameraPos)      std::memcpy(cameraPos, frameCB_.camPos, 3 * sizeof(f32));
        return true;
    }
    // Scene-space viewport rect (vpW_==0 means whole target).
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
    void setWaterWaves(const f32 (*waves)[4], u32 count, f32 amplitude) override {
        const u32 n = count > 3u ? 3u : count;
        for (u32 i = 0; i < 3; ++i)
            for (int a = 0; a < 4; ++a) frameCB_.wave[i][a] = (i < n && waves) ? waves[i][a] : 0.0f;
        frameCB_.waveParams[0] = amplitude;
        frameCB_.waveParams[1] = static_cast<f32>(n);
        frameCB_.waveParams[2] = frameCB_.waveParams[3] = 0.0f;
    }
    void setFrameTime(f32 seconds, f32 deltaSeconds) override {
        // Time wrapped to 3600s for seamless ripple effects.
        frameCB_.time[0] = std::fmod(seconds, 3600.0f);
        frameCB_.time[1] = seconds;
        frameCB_.time[2] = deltaSeconds;
        frameCB_.time[3] = 0.0f;
    }
    void setSkyAtmosphere(const SkyAtmosphere& s) override;
    SkyAtmosphere skyAtmosphere() const override { return sky_; }
    // Packs the physical atmosphere fields and derives four more for the per-frame block.
    void packAtmosphere(const SkyAtmosphere& s);
    void setPostProcess(const PostSettings& p) override { post_ = p; }
    PostSettings postProcess() const override { return post_; }
    // Exposure readout (filled by collectExposureReadout).
    bool postExposureReadout(f32& adaptedExposure) const override {
        if (!expReadoutSeeded_) return false;
        adaptedExposure = expReadoutValue_;
        return true;
    }

    MeshHandle createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) override;
    MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) override;
    bool destroyMesh(MeshHandle mesh) override;

    // Static mesh heap selection (see setStaticMeshHeapDefault).
    void setStaticMeshHeapDefault(bool onDefaultHeap) override { staticMeshDefaultHeap_ = onDefaultHeap; }
    bool staticMeshHeapDefault() const override { return staticMeshDefaultHeap_; }

    // Mesh sharing source's vertex buffer.
    MeshHandle createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) override;
    MeshHandle createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) override;
    bool destroyLineMesh(LineHandle mesh) override;
    BufferHandle meshVertexBuffer(MeshHandle mesh) const override {
        if (!mesh || mesh > meshes_.size()) return 0;
        const GpuMesh& m = meshes_[mesh - 1];
        // Only skin targets (compute-written) have a dispatchable vertex buffer.
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
    bool meshBoundsAabb(MeshHandle mesh, f32 outMin[3], f32 outMax[3]) const override {
        if (!mesh || mesh > meshes_.size()) return false;
        const GpuMesh& m = meshes_[mesh - 1];
        if (m.boundsRadius <= 0.0f) return false;   // Bounds never measured.
        for (int a = 0; a < 3; ++a) { outMin[a] = m.boundsMin[a]; outMax[a] = m.boundsMax[a]; }
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
    void setWireframe(bool on) override { wireframe_ = on; if (on) wireframeFrame_ = true; }
    void setUnlit(bool on) override { unlit_ = on; }
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(drawBinding_, set, constants, bytes);
    }
    void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(defaultDrawBinding_, set, constants, bytes);
    }

    // Same-frame depth prepass.
    void setDepthPrepassEnabled(bool on) override { depthPrepassEnabled_ = on; }
    bool depthPrepassEnabled() const override { return depthPrepassEnabled_; }
    void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) override;
    bool drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) override;
    // Shared body of the two above; true when a depth-only draw was actually recorded.
    bool depthOnlyDraw(MeshHandle mesh, const f32 world[16], const f32 color[4], bool allowComputeWritten);
    // Auto-consumed by next drawMesh call.
    void setNextDrawPrepassed(bool prepassed) override { nextDrawPrepassed_ = prepassed; }

    // Blended-mesh path (sticky across frames).
    void setDrawBlended(bool blended) override { drawBlended_ = blended; }
    bool drawBlended() const override { return drawBlended_; }

    void setLineDepth(bool testDepth) override { editorLines_.setDepthTest(testDepth); }
    void setLineWidth(f32 pixels) override { editorLines_.setWidth(pixels); }
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

    bool deviceLost() const override { return deviceLost_; }
    // Exposes drawMesh's scene suppression predicate for caller optimization.
    bool sceneSuppressed() const override {
        for (const IRenderFeature* f : features_) if (f->suppressesScene()) return true;
        return false;
    }
    GpuTimingReport gpuTiming() const override;
    void resetGpuTiming() override;
    void beginFrame() override;
    void endFrame() override;
    bool runStandaloneCompute(const std::function<void(IRenderContext&)>& record) override;
    void present();
    void initGpuTiming();
    u32  gpuStamp();
    void collectGpuTiming();
    void collectExposureReadout();
    // GPU timing markers for phases outside render features.
    void beginGpuSpan(const char* label) {
        if (!tsEnabled_) return;
        // Parent is whatever is already open (kNoParent if none); cap enforced to prevent dropped spans reparenting incorrectly.
        if (tsSlice_[frameIndex_].size() >= kMaxGpuSpans) { ++tsDropped_; return; }
        const u32 parent = tsOpen_.empty() ? kNoParent : tsOpen_.back();
        tsSlice_[frameIndex_].push_back({label, gpuStamp(), kMaxGpuStamps, parent});
        tsOpen_.push_back(static_cast<u32>(tsSlice_[frameIndex_].size() - 1));
    }
    void endGpuSpan() {
        if (!tsEnabled_) return;
        if (tsDropped_) { --tsDropped_; return; }   // pairs with a refused open
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
    // Blocks until the fence reaches value; false only when device has been removed.
    bool waitFence(u64 value);
    // Records device removal once with reason decoded.
    bool noteDeviceRemoved(const char* where, HRESULT hr);
    // Logs DRED captured data if --dred armed the interfaces.
    void dumpDredOnDeviceRemoved();

    // ---- the camera post chain (rhi::PostSettings) ----
    bool createPostPipelines();          // root signature, PSOs and constant ring: once, at init
    bool createPostTargets();            // resolve target, bloom pyramid and descriptors: per resize
    void releasePostTargets();
    // bbIdx names backbuffer's RTV (renderTargets_[bbIdx]).
    void runPostChain(ID3D12Resource* backbuffer, u32 bbIdx, bool generated = false);
    // Suballocate one pass's constants from this frame's post ring.
    D3D12_GPU_VIRTUAL_ADDRESS postConstants(const void* data, u32 bytes);
    D3D12_GPU_DESCRIPTOR_HANDLE postTriple(u32 triple) const;
    D3D12_CPU_DESCRIPTOR_HANDLE postTripleCpu(u32 triple) const;
    // Converts display-authored colour to scene radiance.
    static void toSceneReferred(const f32 display[4], f32 out[4]);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12DescriptorHeap> msaaRtvHeap_;
    // PRESENT IMAGES: frame draws into these, then present thread copies into swapchain.
    ComPtr<ID3D12Resource> renderTargets_[kBackBufferCount];
    ComPtr<ID3D12Resource> swapBuffers_[kSwapBufferCount];

    // ---- MIRROR WINDOW (IDevice::setMirrorWindow) ----
    // presentPass copies the viewport rect of each present image into mirrorImages_[same index]; the
    // present thread presents it to mirrorSwap_ beside the main swapchain. Changed only with the
    // present queue drained.
    ComPtr<IDXGISwapChain3> mirrorSwap_;
    ComPtr<ID3D12Resource> mirrorSwapBuffers_[kSwapBufferCount];
    ComPtr<ID3D12Resource> mirrorImages_[kBackBufferCount];
    HWND mirrorHwnd_ = nullptr;
    u32 mirrorW_ = 0, mirrorH_ = 0;
    std::atomic<bool> mirrorFailLogged_{false};
    void releaseMirror();
    void copyToMirror(u32 bbIdx);

    // ---- PRESENT THREAD (FSR3-style) ----
    // Present can block waiting for previous image to display. Render thread queues requests to a dedicated thread
    // which waits (GPU-side) for the image, copies it into the swapchain on its own queue, and presents.
    struct PresentRequest {
        u32 image;        // renderTargets_ index
        u64 ready;        // readyFence_ value the render queue signals once the image is drawn
        u64 serial;       // doneFence_ value this thread signals once the image has been copied
        UINT sync, flags;
        bool mirror = false;   // also present mirrorImages_[image] to the mirror window
    };
    static constexpr u32 kPresentAllocs = 4;
    static constexpr usize kPresentQueueMax = 4;   // requests waiting; the render thread waits beyond
    ComPtr<ID3D12CommandQueue> presentQueue_;
    ComPtr<ID3D12Fence> readyFence_, doneFence_;
    ComPtr<ID3D12CommandAllocator> presentAllocs_[kPresentAllocs];
    u64 presentAllocUse_[kPresentAllocs] = {};
    ComPtr<ID3D12GraphicsCommandList> presentList_;
    HANDLE presentEvent_ = nullptr;
    u64 readySerial_ = 0;                          // render thread only
    u64 requestSerial_ = 0;                        // render thread only
    u64 imageLastUse_[kBackBufferCount] = {};      // request serial that last copied each image
    u32 imageNext_ = 0;                            // the present image the next frame draws first
    std::deque<PresentRequest> presentQ_;
    std::mutex presentMu_;
    std::condition_variable presentCv_;
    std::thread presentThread_;
    bool presentStop_ = false;
    HRESULT presentError_ = S_OK;
    std::atomic<u64> presentBlockedUs_{0};         // time the present thread spent inside Present
    std::atomic<u64> presentCount_{0};             // Presents the present thread made
    bool startPresentThread();
    void stopPresentThread();
    void drainPresents();                          // waits until every queued image has been presented
    void presentThreadMain();
    bool queuePresent(u32 image, UINT sync, UINT flags);
    u32 realImage_ = 0;                            // the present image this frame's real image is drawn into
    void waitImageFree(u32 image);                 // GPU-side: the render queue waits for the copy
    bool createPresentImages();
    ComPtr<ID3D12Resource> msaaColor_;
    ComPtr<ID3D12Resource> depthBuffer_;
    // Generic-RHI wrapper -- stable across resize; adoptExternalDepthTexture re-fills this same slot.
    TextureHandle depthTexHandle_ = 0;
    // Dirty flag: set when depthBuffer_ reallocates; cleared when adopted.
    bool depthTexDirty_ = true;

    // G-buffer: velocity + view-space depth + normal/roughness (single-sample, OFF by default).
    bool gbufferEnabled_ = false;
    // ALWAYS single-sample.
    ComPtr<ID3D12Resource> gbufVelocity_;      // RG16F,        screen-space motion, texels/frame
    ComPtr<ID3D12Resource> gbufViewZ_;         // R32F,         view-space linear depth
    ComPtr<ID3D12Resource> gbufNormalRough_;   // RGB10A2Unorm, world normal (encoded) + roughness
    // One single-descriptor RTV heap per target.
    ComPtr<ID3D12DescriptorHeap> gbufVelocityRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> gbufViewZRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> gbufNormalRoughRtvHeap_;
    bool createGBufferTargets();
    void releaseGBufferTargets();
    // Re-adopts all three resources when gbufTexDirty_ indicates a create just ran.
    void refreshGBufferTexHandles();
    // Stable handles across resize (see depthTexHandle_).
    TextureHandle gbufVelocityTexHandle_ = 0, gbufViewZTexHandle_ = 0, gbufNormalRoughTexHandle_ = 0;
    // One dirty flag for all three.
    bool gbufTexDirty_ = true;
    // Logged once per MISMATCH, not once per frame.
    bool gbufMsaaWarned_ = false;
    // MSAA twins of the three targets: the scene pass writes these when sampleCount_ > 1, and
    // resolveGBufferMsaa() takes the nearest sample into the single-sample targets after the
    // transparent pass (gbuffer_msaa_resolve.hlsl), so every reader keeps working under MSAA.
    ComPtr<ID3D12Resource> gbufVelocityMs_, gbufViewZMs_, gbufNormalRoughMs_;
    ComPtr<ID3D12DescriptorHeap> gbufVelocityMsRtvHeap_, gbufViewZMsRtvHeap_, gbufNormalRoughMsRtvHeap_;
    TextureHandle    gbufMsTex_[3] = {};   // adopted, for the resolve's Texture2DMS SRVs
    bool             gbufMsTexDirty_ = true;
    PipelineHandle   gbufResolvePso_ = 0;
    BindingSetHandle gbufResolveSet_ = 0;
    bool             gbufResolveFailed_ = false;
    bool             gbufWrittenFrame_ = false;   // this frame's scene pass bound the G-buffer
    bool resolveGBufferMsaa();

    // Previous frame's view-projection (same convention as setCamera).
    f32  prevViewProj_[16] = {};
    // True once a camera has been set (guards first setCamera from zero state).
    bool gbufCameraPrimed_ = false;
    // Mirrors VoxiRenderer's rtHistValid_ (inverted).
    bool gbufHistoryInvalid_ = true;

    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    // Frame interpolation's second recording (real image, after the generated one).
    ComPtr<ID3D12CommandAllocator> allocatorsGen_[kFrameCount];
    bool submitGeneratedImage();
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    // runStandaloneCompute's own list and allocator, made on first use. Idle outside that call.
    ComPtr<ID3D12CommandAllocator> standaloneAlloc_;
    ComPtr<ID3D12GraphicsCommandList> standaloneList_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    // Wait-before-reuse frame sync.
    u64 fenceValues_[kFrameCount] = {0, 0};
    u64 nextFence_ = 0;
    u32 frameIndex_ = 0;   // frame-in-flight slot
    // Advanced ONLY by beginFrame after its fence wait.
    u64 frameSerial_ = 0;
    u32 bbIndex_ = 0;      // swapchain image this frame draws first
    u32 rtvSize_ = 0;

    // ---- frame interpolation (IDevice::setFrameInterpolator) ----
    IFrameInterpolator* frameInterp_ = nullptr;   // non-owning, host-installed
    bool frameInterpOn_ = false;               // requested
    bool frameInterpolated_ = false;           // this frame presented a generated image ahead of the real one
    bool frameInterpCut_ = true;               // the next frame must not be interpolated across
    u32  frameInterpOffReason_ = 0;            // last reason logged
    TextureHandle fgInputTex_ = 0;          // frame N's scene colour, copied
    u32 fgInputW_ = 0, fgInputH_ = 0;
    f32 fgPrevCamPos_[3] = {};
    f32 fgPrevCamFwd_[3] = {};
    bool fgCamPrimed_ = false;
    // Why frame interpolation cannot run this frame (0 = it can).
    u32 frameInterpBlocker();
    bool frameInterpCameraJumped();
    // Post chain, overlays and UI for ONE presented image.
    void presentPass(u32 bbIdx, bool generated, bool firstOfFrame, bool lastOfFrame);
    bool fgGeneratedPost_ = false;          // runPostChain is drawing the generated image
    bool fgCaptureGenerated_ = false;       // diagnostics: captures take the generated image
    bool fgShowGeneratedOnly_ = false;      // visualisation: the real image's slot shows the generated one too
    bool captureRecorded_ = false;          // this frame recorded the capture copy
    f32 fgDisplayHz_ = 0.0f;                // queried at swapchain creation and on every resize
    void queryDisplayRefresh();
    // ---- MIDPOINT PRESENT (IDevice::frameMidpoint) ----
    // Real image waits and is queued at the NEXT frame's midpoint, behind GPU work, so present thread
    // shows it once GPU is halfway through that frame. Generated and real alternate at GPU's pace (double rate).
    bool pendingReal_ = false;
    u32 pendingRealImage_ = 0;
    UINT pendingRealSync_ = 1, pendingRealFlags_ = 0;
    bool recording_ = false;                // cmdList_ is open between beginFrame and endFrame's submit
    ComPtr<ID3D12CommandAllocator> allocatorsMid_[kFrameCount];
    // Scene pass bindings, restored after midpoint reopens the list.
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtvs_[4] = {};
    u32 sceneRtvCount_ = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv_ = {};
    D3D12_VIEWPORT sceneVp_ = {};
    D3D12_RECT sceneSc_ = {};
    // CPU WAIT ACCOUNTING (with --gpu-timing).
    static constexpr u32 kWaitReport = 300;
    f64 waitFenceMs_ = 0.0, waitPresentMs_ = 0.0;
    u32 waitFrames_ = 0;
    static f64 qpcMs(i64 a, i64 b) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return static_cast<f64>(b - a) * 1000.0 / static_cast<f64>(f.QuadPart);
    }
    static i64 qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

    // Per-pass GPU timing via pushMarker/popMarker nesting; EndQuery per marker side.
    static constexpr u32 kMaxGpuSpans = 64;
    static constexpr u32 kMaxGpuStamps = kMaxGpuSpans * 2;
    // Sentinel for "no parent, top-level" (0xFFFFFFFFu).
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    // Begin/end say duration, not nesting position; parent set from tsOpen_.back().
    struct GpuSpan { const char* label = nullptr; u32 begin = 0; u32 end = 0; u32 parent = kNoParent; };
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource>  tsReadback_;
    u64  tsFrequency_ = 0;              // GPU ticks per second
    u32  tsCount_ = 0;                  // stamps issued so far this frame
    bool tsWrapped_ = false;            // ran out of slots
    // Per-frame-slice: stamps read two frames after issue.
    std::vector<GpuSpan> tsSlice_[kFrameCount];
    u32 tsSliceBegin_[kFrameCount] = {};
    u32 tsSliceEnd_[kFrameCount] = {};
    std::vector<u32>     tsOpen_;       // slots of markers still open, innermost last
    // Spans rejected at cap; closes check this first to avoid popping wrong span.
    u32                  tsDropped_ = 0;
    // Accumulated as a TREE; keyed by (label, parent).
    static constexpr u32 kNoAccumParent = 0xFFFFFFFFu;
    struct GpuAccum { std::string label; f64 ms = 0; u32 parent = kNoAccumParent; };
    std::vector<GpuAccum> tsAccum_;
    // Frame-local span index -> GpuAccum index.
    std::vector<u32> tsSpanToAccum_;
    f64  tsAccumFrameMs_ = 0;
    u32  tsAccumFrames_ = 0;
    u32  tsReports_ = 0;
    bool tsEnabled_ = false;

    // Redundant-state elision: one drawMesh per entity previously re-sent the same pipeline and frame constant block.
    PipelineHandle   fovPso_ = 0;
    BindingSetHandle fovSet_ = 0;
    u32              fovCbBytes_ = 0;
    std::vector<u8>  fovCb_;
    // Cleared by anything binding something else since last draw.
    bool             fovValid_ = false;

    // TABLE 1's redundant-state elision (per-draw material set + b2 constant).
    BindingSetHandle dbSet_ = 0;
    u8               dbConstants_[kMaxDrawConstantBytes] = {};
    u32              dbConstantBytes_ = 0;
    bool             dbValid_ = false;

    // Register-mismatch warnings already reported (deduped per-shape).
    std::vector<u64> bindingBaseWarned_;

    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> skyPso_;
    // Per-draw binding table 1 plus b2 constant.
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
    bool drawBindingIgnored_ = false;   // backend's own pipeline drops it; said once

    // ---- translucency: blended-mesh path ----
    bool drawBlended_ = false;

    // One drawMesh() call captured while drawBlended_ was true.
    struct BlendedDraw {
        MeshHandle mesh = 0;
        f32 world[16] = {};
        f32 color[4] = {};
        f32 metallic = 0.0f;
        f32 roughness = 0.0f;
        DrawBinding binding{};
    };
    // Filled by drawMesh(), drained (sorted back-to-front) by endFrame(), cleared at next beginFrame.
    std::vector<BlendedDraw> blendedDraws_;
    // Missing blended pipeline warning (per-frame, unlike drawBindingIgnored_).
    bool blendedPipelineMissingWarned_ = false;

    // ---- same-frame depth prepass ----
    bool depthPrepassEnabled_ = false;
    // AUTO-CONSUMED: read and reset by next drawMesh().
    bool nextDrawPrepassed_ = false;
    // The mesh drawMeshDepthOnly() last ACTUALLY depth-drew.
    MeshHandle depthOnlyMesh_ = 0;

    bool skyEnabled_ = false;
    // Set in beginFrame when a feature suppressed the scene.
    bool sceneSuppressed_ = false;
    // Set alongside sceneSuppressed_; read by deferred sky in endFrame.
    bool frameSuppressed_ = false;
    // Last logged outcome of scene-claim race.
    const IRenderFeature* lastSuppressWinner_ = nullptr;
    u32                   lastSuppressClaimants_ = 0;
    // The winner asked to record its scene at endFrame (wantsLateScenePass): set in beginFrame,
    // consumed by endFrame's first lines.
    IRenderFeature* lateSceneWinner_ = nullptr;
    // The authored atmosphere.
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    // Whether wireframe view was on at ANY point this frame.
    bool wireframeFrame_ = false;
    // See IDevice::setUnlit. Sticky like wireframe_.
    bool unlit_ = false;
    ComPtr<ID3D12Resource> frameCBs_[kFrameCount];
    u8* frameCBPtr_[kFrameCount] = {nullptr, nullptr};

    // ---- camera post chain ------------------------------------------------------------------
    PostSettings post_{};
    // MSAA resolve destination. Null when sampleCount_ == 1.
    ComPtr<ID3D12Resource> sceneResolved_;
    ComPtr<ID3D12Resource> bloomTex_;            // half-res RGBA16F pyramid
    u32 bloomMips_ = 0, bloomW_ = 0, bloomH_ = 0;
    // Per-mip resource state.
    D3D12_RESOURCE_STATES bloomState_[kMaxBloomMips] = {};
    ComPtr<ID3D12Resource> histBuf_, expBuf_;
    bool expSeeded_ = false;
    // Per-frame-slot READBACK copy of expBuf_'s first 8 bytes.
    ComPtr<ID3D12Resource> expReadback_[kFrameCount];
    // Set by runPostChain after slot's copy; cleared by collectExposureReadout.
    bool expReadbackPending_[kFrameCount] = {false, false};
    // Last value actually read from a completed GPU copy.
    f32  expReadoutValue_ = 0.0f;
    bool expReadoutSeeded_ = false;
    // Local exposure's bilateral grid, raw (u2) and blurred (u3); scene-size-dependent.
    ComPtr<ID3D12Resource> localGridBuf_, localGridBlurBuf_;
    u32 localGridW_ = 0, localGridH_ = 0;
    ComPtr<ID3D12DescriptorHeap> postRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> postSrvHeap_;
    ComPtr<ID3D12RootSignature> postRootSig_;
    ComPtr<ID3D12PipelineState> bloomPrefilterPso_, bloomDownPso_, bloomUpPso_;
    // Composite permutations: [bloom on][auto-exposure on].
    ComPtr<ID3D12PipelineState> compositePso_[2][2];
    ComPtr<ID3D12PipelineState> histogramPso_, exposurePso_;
    ComPtr<ID3D12PipelineState> localGridPso_, localBlurPso_;
    ComPtr<ID3D12Resource> postCBs_[kFrameCount];
    u8* postCBPtr_[kFrameCount] = {nullptr, nullptr};
    u32 postCBUsed_ = 0;
    u32 postSrvSize_ = 0, postRtvSize_ = 0;
    bool postReady_ = false;
    // Wall-clock seconds since previous endFrame.
    i64 lastFrameTick_ = 0;
    f32 frameSeconds_ = 1.0f / 60.0f;

    ComPtr<ID3D12Resource> captureBuf_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFp_{};
    bool captureReq_ = false, captureReady_ = false;
    u32 capX_ = 0, capY_ = 0;
    f32 captured_[4] = {0, 0, 0, 0};
    std::vector<u8> frameImage_;
    u32 frameImageW_ = 0, frameImageH_ = 0;

    // Non-owning; null in game builds and AVER_ENABLE_UI=OFF.
    d3d12::IUiBackend* uiBackend_ = nullptr;
    bool uiActive_ = false;

    std::vector<GpuMesh> meshes_;
    // Skin-target meshes created outside a frame, still uninitialised; drained at next frame start.
    struct SkinSeed { MeshHandle dst; MeshHandle src; };
    std::vector<SkinSeed> skinSeeds_;
    void seedSkinTargets();
    // Shared body of createMeshSharingVertices and createPosedPartMesh.
    MeshHandle shareVertices(MeshHandle source, const u32* indices, u32 indexCount, bool posed);

    // Static mesh default heap (false = Upload heap, the default).
    bool staticMeshDefaultHeap_ = false;
    // Default-heap first-mesh warning (once, on first actual build).
    bool staticMeshDefaultHeapLogged_ = false;
    // Fallback-to-Upload warning (once, on any failure).
    bool staticMeshDefaultHeapUploadFailWarned_ = false;
    PerFrameCB frameCB_{};
    u32 width_ = 0, height_ = 0;
    // Scene render-target size: width_/height_ scaled by renderScale_, floored at 1.
    u32 sceneWidth_ = 0, sceneHeight_ = 0;
    f32 renderScale_ = 1.0f;   // [0.25, 1.0]
    void computeSceneSize() {
        sceneWidth_  = width_  ? static_cast<u32>(std::lround(static_cast<f32>(width_)  * renderScale_)) : 0;
        sceneHeight_ = height_ ? static_cast<u32>(std::lround(static_cast<f32>(height_) * renderScale_)) : 0;
        if (width_  && sceneWidth_  < 1) sceneWidth_  = 1;
        if (height_ && sceneHeight_ < 1) sceneHeight_ = 1;
    }
    // Present-space -> scene-space scaling for viewport sub-rect.
    u32 scaleToSceneW(u32 v) const { return width_  ? static_cast<u32>((static_cast<u64>(v) * sceneWidth_)  / width_)  : v; }
    u32 scaleToSceneH(u32 v) const { return height_ ? static_cast<u32>((static_cast<u64>(v) * sceneHeight_) / height_) : v; }
    // Scale change deferred to next frame boundary (applied in beginFrame).
    void setRenderScale(f32 scale) override {
        const f32 asked = scale;
        scale = std::fmax(0.25f, std::fmin(1.0f, scale));
        const f32 effective = pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_;
        if (scale == effective) return;
        // Before swapchain exists, nothing to rebuild.
        if (!hasSwapchain_) {
            AVER_INFO("[RHI.D3D12] setRenderScale: {:.4f} -> {:.4f} (asked {:.4f}), before the swapchain",
                      renderScale_, scale, asked);
            renderScale_ = scale;
            return;
        }
        AVER_INFO("[RHI.D3D12] setRenderScale: {:.4f} -> {:.4f} (asked {:.4f}), applied next frame",
                  effective, scale, asked);
        pendingRenderScale_ = scale;
        pendingRenderScaleValid_ = true;
    }
    // Reports what caller ASKED FOR, not what is currently resident.
    f32 pendingOrCurrentRenderScale() const {
        return pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_;
    }
    void applyPendingRenderScale() {
        bool rebuild = upscalerTargetsDirty_;
        upscalerTargetsDirty_ = false;
        if (pendingRenderScaleValid_) {
            pendingRenderScaleValid_ = false;
            if (pendingRenderScale_ != renderScale_) { renderScale_ = pendingRenderScale_; rebuild = true; }
        }
        if (rebuild) rebuildSceneTargets();
    }
    bool upscalerTargetsDirty_ = false;
    f32  pendingRenderScale_ = 1.0f;
    bool pendingRenderScaleValid_ = false;
    f32 renderScale() const override { return pendingOrCurrentRenderScale(); }
    // Rebuilds every target sized off sceneWidth_/sceneHeight_ after renderScale_ changes.
    void rebuildSceneTargets() {
        waitForGpu();
        depthBuffer_.Reset();
        msaaColor_.Reset();
        computeSceneSize();
        vpX_ = vpY_ = vpW_ = vpH_ = 0;   // stored in scene-space
        createDepthBuffer();
        createMsaaColor();
        // Reset then recreate, GATED on gbufferEnabled_.
        if (gbufferEnabled_) {
            gbufVelocity_.Reset();    gbufViewZ_.Reset();    gbufNormalRough_.Reset();
            if (!createGBufferTargets())
                AVER_ERROR("[RHI.D3D12] G-buffer target rebuild for render scale {:.2f} failed", renderScale_);
        }
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
    std::vector<u32> seenMessageIds_;   // First occurrence only.
    std::vector<std::string> seenGbvTexts_;   // GPU-based validation (deduped by text).
    u32 dbgCorruption_ = 0, dbgError_ = 0, dbgWarning_ = 0;
    void drainDebugMessages();

    // ---- DXR 1.1 ----
    ComPtr<ID3D12Device5> device5_;
    ComPtr<ID3D12GraphicsCommandList4> cmdList4_;

    // ---- Mesh shader geometry path (D3D12 Ultimate) ----
    ComPtr<ID3D12GraphicsCommandList6> cmdList6_;
    ComPtr<ID3D12RootSignature> msRootSig_;
    ComPtr<ID3D12PipelineState> msPso_;
    bool msSupported_ = false, msActive_ = false, msRefusalLogged_ = false;
    ID3D12RootSignature* boundRootSig_ = nullptr;   // Cache only; ownership in ComPtrs.
    ID3D12PipelineState* boundPso_ = nullptr;
    // Last descriptor heap array bound via SetDescriptorHeaps -- see setBindingSet.
    ID3D12DescriptorHeap* boundHeap_ = nullptr;

    bool hasSwapchain_ = false;
    // Sticky: nothing recreates a device. See noteDeviceRemoved and IDevice::deviceLost.
    bool deviceLost_ = false;
    u32  presentedFrames_ = 0;   // Counted only when rhi::simulatedDeviceLoss() is armed.
    bool vsync_ = true;
    bool tearingSupported_ = false;   // Fixed for swapchain life.
    f32 clear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    f32 msaaClear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};   // Optimised clear value for scene colour.
    f32 sceneClear_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::string adapterName_ = "D3D12 Device";
    bool softwareAdapter_ = false;   // True when running on WARP.
    bool warpConsRasterLogged_ = false;
    // IDXGIAdapter3 for QueryVideoMemoryInfo. Null on older DXGI or unavailable driver.
    ComPtr<IDXGIAdapter3> adapter3_;

    // ---- generic RHI (render-feature modules) ----
    D3D12ResourceFactory* rhiFactory_ = nullptr;
    D3D12RenderContext* rhiContext_ = nullptr;
    std::vector<IRenderFeature*> features_;   // Non-owning.
    // Editor chrome (grid, gizmos, selection, collider/nav overlays).
    EditorLines editorLines_;

    // ---- AverSR ----
    IUpscaler* upscaler_ = nullptr;   // Null unless a host set one.
    // Alias of scene colour for IUpscaler::execute. Updated per-frame.
    TextureHandle sceneColorTex_ = 0;
    u32           sceneColorTexW_ = 0, sceneColorTexH_ = 0;
    // Blended pass backdrop.
    TextureHandle blendBackdropTex_ = 0;
    u32           blendBackdropW_ = 0, blendBackdropH_ = 0;
    bool          blendBackdropCopyLogged_ = false;
    // Translucency re-capture budget (per-layer, max 8 stacked glass).
    static constexpr u32 kMaxBlendLayerResolves = 8;
    bool          blendLayerCapWarned_ = false;
    // Power-of-two gated log of blended replay state.
    u32 blendStatDrawsLogged_ = ~0u;
    u32 blendStatResolvesLogged_ = ~0u;
    u32 blendStatCapturesLogged_ = ~0u;
    u32 blendStatChanges_ = 0;
    // AverSR output: HDR (pre-tonemap) at PRESENT resolution.
    TextureHandle presentHdrTex_ = 0;
    u32           presentHdrTexW_ = 0, presentHdrTexH_ = 0;
    bool          srLogged_ = false;

    friend class D3D12ResourceFactory;
    friend class D3D12RenderContext;
};

// ---------------------------------------------------------------- generic RHI objects
// Backing records for the handle tables. A handle is index + 1, so 0 is never a live resource.

// A texture, its optional render/depth target views, and resolved description.
struct RhiTexture {
    ComPtr<ID3D12Resource> res;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    TextureDesc desc{};                     // Resolved: mips holds real count.
    // Name identity for debug; not state tracking. Release diagnostics use it (59-byte cost).
    std::string debugName;
#if AVER_RHI_TRACK_STATE
    std::vector<ResourceState> states;
#endif
    // UI descriptor handle. Zero until first asked.
    u64 uiSrvCpu = 0, uiSrvGpu = 0;
};

// A buffer, description, and persistent mapping.
struct RhiBuffer {
    ComPtr<ID3D12Resource> res;
    BufferDesc desc{};
    u8* mapped = nullptr;                   // Upload buffers stay mapped for their whole life.
    std::string debugName;
#if AVER_RHI_TRACK_STATE
    ResourceState state = ResourceState::Common;
    bool stateFixed = false;   // Upload and AS buffers reject every transition.
#endif
};

// A compiled shader blob and the stage it was compiled for.
struct RhiShader {
    ComPtr<ID3DBlob>  blob;
    std::vector<u8>   bytes;   // Precompiled bytecode copy (ShaderDesc::bytecode).
    ShaderStage       stage = ShaderStage::Vertex;

    [[nodiscard]] bool valid() const { return blob || !bytes.empty(); }
    [[nodiscard]] D3D12_SHADER_BYTECODE code() const {
        if (blob) return {blob->GetBufferPointer(), blob->GetBufferSize()};
        return {bytes.data(), bytes.size()};
    }
};

static_assert(kMaxConstantSlots == 5, "slotParam's -1 initialisers are written out per slot");
static_assert(kBindingTableCount == 2, "srvParam/uavParam's -1 initialisers are written out per table");

// A pipeline state and where each declared binding landed in its root signature; -1 means undeclared.
struct RhiPipeline {
    ComPtr<ID3D12PipelineState> pso;
    ID3D12RootSignature* rootSig = nullptr;   // Owned by the root-signature cache.
    bool compute = false;
    bool mesh = false;
    bool amplification = false;   // True when mesh pipeline also has amplification shader.
    i32  srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    i32  bindlessParam = -1;   // -1 = no bindless table.
    u32  srvBaseRegister[kBindingTableCount] = {};
    i32  slotParam[kMaxConstantSlots] = {-1, -1, -1, -1, -1};
    u32  slotDwords[kMaxConstantSlots] = {};  // 0 = root CBV; non-zero = root constants.
    i32  msVertexParam = -1, msIndexParam = -1, msCountParam = -1;
    i32  instanceWorldParam = -1;   // -1 unless GraphicsPipelineDesc::instanced was set.
};

// One suballocated descriptor range plus the kind declared for each slot.
struct RhiBindingSet {
    u32 srvCount = 0, uavCount = 0;
    u32 srvBaseRegister = 0, uavBaseRegister = 0;
    u32 stageBase = 0;              // Range in CPU-only staging heap.
    u32 gpuBase[kFrameCount] = {};  // Shader-visible range per frame in flight.
    u64 version = 0;                // Bumped on every write into stageBase.
    u64 gpuVersion[kFrameCount] = {};   // Version each gpuBase[f] last received.
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
    ComPtr<ID3D12Resource> as, scratch;   // non-updatable: scratch only while a build is in flight
    u64 scratchBytes = 0;
    MeshHandle mesh = 0;
    bool built = false;
    bool allowUpdate = false;   // Built with ALLOW_UPDATE; may be refitted in place.
    u32 builtVertexCount = 0;   // Counts as of last full build.
    u32 builtIndexCount = 0;
    std::vector<BlasGeometry> geometries;   // For createBlasMulti; mesh = geometries[0].mesh.
};

// One instance as it was packed into a TLAS's last build or refit.
struct RhiTlasSlot {
    D3D12_GPU_VIRTUAL_ADDRESS blasVa = 0;
    u32 flags = 0;
    u32 mask = 0;
};

// A top-level acceleration structure, its scratch, and one instance buffer per frame in flight.
struct RhiTlas {
    ComPtr<ID3D12Resource> as, scratch;
    ComPtr<ID3D12Resource> instances[kFrameCount];
    u8* instancePtr[kFrameCount] = {};
    u32 maxInstances = 0;
    bool allowUpdate = false;   // Built with ALLOW_UPDATE; may be refitted in place.
    bool built = false;
    // Per-slot signature for refitTlas eligibility: same count, same BLAS, flags, and mask.
    std::vector<RhiTlasSlot> builtSlots;
    // Where each build/refit packs its new signature before swapping.
    std::vector<RhiTlasSlot> pendingSlots;

    // ---- the static prefix (IResourceFactory::setTlasStaticInstances) ----
    // Slots [0, staticCount) of staticDescs, packed once per build/refit.
    u32 staticCount = 0;
    BufferHandle staticDescs = 0;
    D3D12_RESOURCE_STATES staticDescsState = D3D12_RESOURCE_STATE_COMMON;
    // The distinct BLASes the prefix names.
    std::vector<BlasHandle> staticBlases;
    // The prefix length the last build/refit actually used (0 when it had none or dropped a broken one):
    // an update is only legal over the same descs its build had.
    u32 builtStatic = 0;
    bool staticBrokenLogged = false;
};

// A root signature plus the parameter indices it was built with; shared by identical layouts.
struct RhiBindlessTable {
    u32  heapBase = 0;
    u32  capacity = 0;
    bool alive = false;
};

struct RootSigEntry {
    PipelineLayout layout{};
    bool mesh = false;
    // Part of cache key: instanced and non-instanced layouts need different signatures.
    bool instanced = false;
    ComPtr<ID3D12RootSignature> sig;
    i32 srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    i32 slotParam[kMaxConstantSlots] = {-1, -1, -1, -1, -1};
    i32 msVertexParam = -1, msIndexParam = -1, msCountParam = -1;
    i32 instanceWorldParam = -1;
    i32 bindlessParam = -1;
};

// A destroyed object the GPU may still be reading. Released once the fence passes.
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
    // Part of the key. Without this bindless and non-bindless variants would collide.
    if (a.bindlessTextureCount != b.bindlessTextureCount) return false;
    // Part of the key: constant buffer space difference requires different root signatures.
    if (a.constantSpace != b.constantSpace || a.samplerSpace != b.samplerSpace) return false;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) if (a.constantDwords[i] != b.constantDwords[i]) return false;
    for (u32 i = 0; i < a.samplerCount && i < 4; ++i) if (!sameSampler(a.samplers[i], b.samplers[i])) return false;
    return true;
}

// Descriptors every binding set suballocates from: one shader-visible heap for the whole device.
constexpr u32 kRhiHeapSize = 65536;
// Transient constant bytes per frame in flight -- starting size; grows on demand.
constexpr u64 kRhiRingBytes = 2u << 20;
// Growth ceiling. 64MB is far past any interactive draw count.
constexpr u64 kRhiRingMaxBytes = 64ull << 20;

// One destination/source/size triple for D3D12ResourceFactory::uploadBuffers.
struct BufferUploadItem {
    ID3D12Resource* dst = nullptr;   // Must be freshly created Default-heap buffer in COMMON.
    const void* src = nullptr;
    u64 bytes = 0;
};

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
    BindlessTableHandle createBindlessTextureTable(u32 capacity) override;
    void destroyBindlessTextureTable(BindlessTableHandle h) override;
    bool setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle t) override;
    u32  bindlessTableCapacity(BindlessTableHandle h) const override;
    u32  bindlessHeapBase(BindlessTableHandle h) const {
        return (h == 0 || h > bindlessTables_.size()) ? 0u : bindlessTables_[h - 1].heapBase;
    }
    BindingSetHandle createBindingSet(const BindingSetDesc& d) override;
    BlasHandle       createBlas(MeshHandle mesh) override;
    TlasHandle       createTlas(u32 maxInstances) override;
    // UPDATABLE twins: built with ALLOW_UPDATE and scratch sized for update, refittable in place.
    BlasHandle       createBlasUpdatable(MeshHandle mesh) override;
    TlasHandle       createTlasUpdatable(u32 maxInstances) override;
    BlasHandle       createBlasMulti(const BlasGeometry* geometries, u32 count) override;
    bool             setTlasStaticInstances(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    BufferHandle     tlasStaticInstanceBuffer(TlasHandle tlas) const override;
    u64              blasMemoryBytes(BlasHandle h) const override;
    u64              tlasMemoryBytes(TlasHandle h) const override;

    void destroyTexture(TextureHandle h) override;
    void destroyBuffer(BufferHandle h) override;
    void destroyBlas(BlasHandle h) override;
    MeshHandle blasMesh(BlasHandle h) const override;
    BlasHandle blasForMesh(MeshHandle mesh) const override;
    // Destroys every acceleration structure built from `mesh`.
    void destroyBlasForMesh(MeshHandle mesh);
    void destroyShader(ShaderHandle h) override;
    void destroyPipeline(PipelineHandle h) override;
    void destroyBindingSet(BindingSetHandle h) override;

    void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void clearSrv(BindingSetHandle set, u32 slot) override;
    void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) override;
    void setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;
    // The underlying resource, for the context's copy and barrier paths. Null on a bad handle.
    ID3D12Resource* bufferResource(BufferHandle h) {
        return (h == 0 || h > buffers_.size()) ? nullptr : buffers_[h - 1].res.Get();
    }
    // Wraps external resources (e.g. depth buffer made directly) into ordinary handles.
    TextureHandle adoptExternalDepthTexture(ID3D12Resource* res, u32 width, u32 height, TextureHandle existing);
    TextureHandle adoptExternalRenderTargetTexture(ID3D12Resource* res, Format fmt, u32 width, u32 height,
                                                   const char* debugName, TextureHandle existing);
    void setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;

    bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) override;
    bool readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset) override;
    bool textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const override;
    bool textureInfo(TextureHandle h, TextureDesc& out) const override;
    void waitIdle() override;

    // Backs IDevice::uiTextureId.
    u64 uiDescriptor(TextureHandle h);

private:
    bool asSlotLogged_ = false;

    BlasHandle createBlasImpl(MeshHandle mesh, bool allowUpdate);
    TlasHandle createTlasImpl(u32 maxInstances, bool allowUpdate);

    // Fills a freshly created texture from TextureDesc::initialData.
    bool uploadInitialData(ID3D12Resource* res, const D3D12_RESOURCE_DESC& td, const TextureDesc& d,
                           u32 mips);

    // Fills freshly created DEFAULT-heap buffers via one UPLOAD staging buffer.
    bool uploadBuffers(std::initializer_list<BufferUploadItem> items);
    // Same as uploadBuffers but for ONE destination; fill writes the bytes directly.
    bool uploadBufferFilled(ID3D12Resource* dst, u64 bytes, const std::function<void(u8*)>& fill);

    // Table lookups. Every one returns nullptr for an out-of-range or freed handle.
    RhiTexture*    texture(TextureHandle h);
    const RhiTexture* texture(TextureHandle h) const;
    RhiBuffer*     buffer(BufferHandle h);
    RhiShader*     shader(ShaderHandle h);
    RhiPipeline*   pipeline(PipelineHandle h);
    RhiBindingSet* bindingSet(BindingSetHandle h);
    RhiBlas*       blas(BlasHandle h);
    RhiTlas*       tlas(TlasHandle h);

    const RootSigEntry* rootSignature(const PipelineLayout& layout, bool mesh, bool instanced = false);
    // Descriptor slot in the shared SHADER-VISIBLE heap (CPU side for writing, GPU side for binding).
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(u32 index) const;
    // CPU handle of descriptor slot `index` in the CPU-only STAGING heap.
    D3D12_CPU_DESCRIPTOR_HANDLE stagingCpu(u32 index) const;
    // Bumps version and drops device's "skip rebind" caches if they hold this set.
    void noteBindingSetWritten(BindingSetHandle set, RhiBindingSet& s);
    void nullFill(RhiBindingSet& s);
    // The null view for ONE slot of a given kind.
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc(SlotKind kind);
    // Reuses a retired range large enough for `count`, or bump-allocates. False when exhausted.
    bool allocRange(u32 count, u32& outFirst);
    // Allocator over the CPU-only staging heap's free list.
    bool allocStageRange(u32 count, u32& outFirst);

    // The fence value at which work recorded right now can be considered retired.
    u64  retireFence() const;
    // Stands in for the recording frame's present fence until present() knows it.
    static constexpr u64 kPendingFrameFence = ~0ull;
    void resolvePendingRetires(u64 fence);
    void retire(ComPtr<IUnknown> obj);
    void collect();

    D3D12Device* dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    u32 heapStride_ = 0;
    u32 heapUsed_ = 0;
    // CPU-only descriptor heap where binding sets stage writes before copying to shader-visible heap.
    ComPtr<ID3D12DescriptorHeap> stageHeap_;
    u32 stageHeapUsed_ = 0;

    std::vector<RhiTexture>    textures_;
    std::vector<RhiBuffer>     buffers_;
    std::vector<RhiShader>     shaders_;
    // Deque, NOT vector: D3D12RenderContext caches a raw pointer that nine read sites use across passes.
    std::deque<RhiPipeline>    pipelines_;
    std::vector<RhiBindingSet> bindingSets_;
    std::vector<RhiBindlessTable> bindlessTables_;
    std::vector<RhiBlas>       blases_;
    std::vector<RhiTlas>       tlases_;
    std::vector<RootSigEntry>  rootSigs_;
    std::vector<RetiredObject> retired_;
    std::vector<RetiredRange>  pendingRanges_;   // Returned, still behind the fence.
    std::vector<RetiredRange>  freeRanges_;
    std::vector<RetiredRange>  stagePendingRanges_;
    std::vector<RetiredRange>  stageFreeRanges_;

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
    void setBindlessTable(BindlessTableHandle table) override;
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
    bool refitBlas(BlasHandle blas) override;
    bool refitTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    void textureBarrier(TextureHandle t, ResourceState from, ResourceState to, u32 subresource) override;
    void bufferBarrier(BufferHandle b, ResourceState from, ResourceState to) override;
    void uavBarrierTexture(TextureHandle t) override;
    void uavBarrierBuffer(BufferHandle b) override;
    void pushMarker(const char* label) override;
    void popMarker() override;

    // Restarts the constant ring from its start; only with the GPU idle (runStandaloneCompute).
    void resetRing() { ringEpoch_ = ~0ull; }

private:
    // Suballocate `bytes` of transient upload memory for the frame being recorded.
    D3D12_GPU_VIRTUAL_ADDRESS ringAlloc(const void* data, u32 bytes);
    // Give every root CBV the pipeline declares a valid address.
    void bindDeclaredRootCbvs(const RhiPipeline* p);
    void applyDrawBinding();
    D3D12_GPU_VIRTUAL_ADDRESS zeroCbv();
    // Packs `instances` (filtered as buildTlas does) into `t`'s THIS-frame instance buffer.
    u32 packTlasInstances(RhiTlas& t, const TlasInstance* instances, u32 count, const char* caller,
                          std::vector<RhiTlasSlot>& outSlots);
    // How many static-prefix slots this build may use: t.staticCount, or 0 with no prefix.
    u32 usableStaticPrefix(RhiTlas& t);
    // Where a build reads its instance descriptors.
    D3D12_GPU_VIRTUAL_ADDRESS tlasBuildDescs(RhiTlas& t, u32 staticUsed, u32 written);

    D3D12Device* dev_;
    D3D12ResourceFactory* res_;
    const RhiPipeline* pipe_ = nullptr;
    ComPtr<ID3D12Resource> zeroCB_;   // Shared zero-filled CBV for declared-but-unsupplied slots.

    ComPtr<ID3D12Resource> ring_[kFrameCount];
    u8* ringPtr_[kFrameCount] = {};
    u64 ringUsed_[kFrameCount] = {};
    u64 ringBytes_[kFrameCount] = {};   // Actual size of each frame's ring.
    u64 ringWanted_ = kRhiRingBytes;   // Size the busiest frame asked for.
    u64 ringOverflowEpoch_ = ~0ull;   // Once-per-frame overflow reporting.
    u64 ringEpoch_ = ~0ull;   // Resets when a new frame begins.

    // Sticky per-draw state.
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
            if (gpuValidationEnabled()) {
                ComPtr<ID3D12Debug1> dbg1;
                if (SUCCEEDED(dbg.As(&dbg1))) {
                    dbg1->SetEnableGPUBasedValidation(TRUE);
                    AVER_INFO("[RHI.D3D12] --gpu-validation: GPU-based validation on (slow)");
                } else {
                    AVER_WARN("[RHI.D3D12] --gpu-validation: ID3D12Debug1 unavailable, GPU-based validation off");
                }
            }
        }
    }
    // DRED must be requested off the DEBUG interface before D3D12CreateDevice.
    if (dredEnabled()) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings1)))) {
            dredSettings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            AVER_INFO("[RHI.D3D12] --dred: DRED forced on (auto-breadcrumbs + page faults + breadcrumb context)");
        } else if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            AVER_INFO("[RHI.D3D12] --dred: DRED forced on (auto-breadcrumbs + page faults; no breadcrumb "
                      "context -- ID3D12DeviceRemovedExtendedDataSettings1 unavailable on this driver/SDK)");
        } else {
            AVER_WARN("[RHI.D3D12] --dred requested, but no DRED settings interface is available "
                      "(driver/SDK too old) -- a device loss will not be able to dump breadcrumbs");
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
            warp.As(&adapter3_);
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
            adapter.As(&adapter3_);
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
    setDebugName(queue_.Get(), "Aver main direct queue");
    initGpuTiming();

    {
        const VideoMemoryInfo vmem = videoMemory();
        if (vmem.supported) {
            AVER_INFO("[RHI.D3D12] video memory at init: local {} MB used of {} MB budget, non-local {} MB used of {} MB budget",
                      vmem.localUsageBytes / (1024ull * 1024ull), vmem.localBudgetBytes / (1024ull * 1024ull),
                      vmem.nonLocalUsageBytes / (1024ull * 1024ull), vmem.nonLocalBudgetBytes / (1024ull * 1024ull));
        }
    }

    {
        const std::string udir = aver::userDataDir();
        if (!udir.empty()) {
            const auto res = rhi::sweepShaderCache(std::filesystem::path(udir) / "ShaderCache",
                                                   kShaderCacheBudgetBytes);
            if (res.filesRemoved)
                AVER_INFO("[RHI.D3D12] shader cache swept: {} blob(s), {:.1f} MB freed; {:.1f} MB in {} file(s) kept",
                          res.filesRemoved, static_cast<f64>(res.bytesRemoved) / (1024.0 * 1024.0),
                          static_cast<f64>(res.bytesRemaining) / (1024.0 * 1024.0), res.filesRemaining);
        }
    }

    if (!hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence")) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) { AVER_ERROR("[RHI.D3D12] CreateEvent failed"); return false; }

    queryCaps();
    if (!(caps_.msaaMask & sampleCount_)) sampleCount_ = 1;

    const SkyAtmosphere def{};
    setLight(def.sunDirection, def.sunColor, def.skyLightIntensity);

    if (!createPipeline()) return false;
    if (!createPostPipelines()) return false;

    // DXR 1.1 init: both caps_ and device_ are valid here.
    initAccelerationStructures();

    rhiFactory_ = new D3D12ResourceFactory(this);
    if (!rhiFactory_->init()) { delete rhiFactory_; rhiFactory_ = nullptr; }
    else {
        rhiContext_ = new D3D12RenderContext(this, rhiFactory_);
        editorLines_.init(*rhiFactory_);
    }

    AVER_INFO("[RHI.D3D12] device ready on adapter '{}'", adapterName_);
    crash::setGpuName(adapterName_);
    if (rhiFactory_) rhiFactory_->selfTest();
    return true;
}

// Logs the debug layer's stored messages, each distinct ID once, and counts by severity.
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
        const std::string text(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0);
        // GPU-based validation shares one ID but differs in resource/dispatch; dedupe on text instead.
        const bool gbv = text.rfind("GPU-BASED VALIDATION", 0) == 0;
        bool seen = false;
        if (gbv) {
            for (const std::string& s : seenGbvTexts_) if (s == text) { seen = true; break; }
            if (seen || seenGbvTexts_.size() >= 48) continue;
            seenGbvTexts_.push_back(text);
        } else {
            for (u32 s : seenMessageIds_) if (s == id) { seen = true; break; }
            if (seen) continue;
            seenMessageIds_.push_back(id);
        }
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
    stopPresentThread();
    drainPresents();
    if (presentEvent_) CloseHandle(presentEvent_);
    if (infoQueue_) {
        drainDebugMessages();
        AVER_INFO("[RHI.D3D12] debug layer totals: {} corruption, {} error, {} warning",
                  dbgCorruption_, dbgError_, dbgWarning_);
    }
    editorLines_.shutdown();
    delete rhiContext_;
    delete rhiFactory_;
    uiShutdown();
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

IResourceFactory* D3D12Device::resources() { return rhiFactory_; }
IRenderContext* D3D12Device::renderContext() { return rhiContext_; }

// Runs `record` against the shared render context with cmdList_ pointed at a private list, so the
// context's own binding, barrier and constant paths work with no swapchain. The GPU is idle on entry
// and exit, which is what makes the frame ring and descriptor slots reusable here.
bool D3D12Device::runStandaloneCompute(const std::function<void(IRenderContext&)>& record) {
    if (!record || !rhiContext_ || !device_ || !queue_ || deviceLost_ || recording_) return false;

    if (!standaloneList_) {
        if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&standaloneAlloc_)),
                  "CreateCommandAllocator (standalone)")) return false;
        if (!hrOk(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, standaloneAlloc_.Get(), nullptr,
                  IID_PPV_ARGS(&standaloneList_)), "CreateCommandList (standalone)")) {
            standaloneAlloc_.Reset();
            return false;
        }
        setDebugName(standaloneList_.Get(), "Aver standalone compute list");
        standaloneList_->Close();
    }

    waitForGpu();
    if (deviceLost_) return false;
    if (!hrOk(standaloneAlloc_->Reset(), "standalone allocator Reset") ||
        !hrOk(standaloneList_->Reset(standaloneAlloc_.Get(), nullptr), "standalone list Reset")) return false;

    // pushMarker/popMarker still touch the span list; timestamps stay off and the spans are dropped.
    struct Scope {
        D3D12Device& d;
        bool ts;
        usize spans;
        explicit Scope(D3D12Device& dev) : d(dev), ts(dev.tsEnabled_), spans(dev.tsSlice_[dev.frameIndex_].size()) {
            d.tsEnabled_ = false;
            std::swap(d.cmdList_, d.standaloneList_);
            d.boundRootSig_ = nullptr; d.boundPso_ = nullptr; d.boundHeap_ = nullptr;
            d.fovValid_ = false; d.dbValid_ = false;
        }
        ~Scope() {
            std::swap(d.cmdList_, d.standaloneList_);
            d.tsEnabled_ = ts;
            std::vector<GpuSpan>& s = d.tsSlice_[d.frameIndex_];
            if (s.size() > spans) s.erase(s.begin() + static_cast<isize>(spans), s.end());
            d.tsOpen_.clear();
            d.boundRootSig_ = nullptr; d.boundPso_ = nullptr; d.boundHeap_ = nullptr;
            d.fovValid_ = false; d.dbValid_ = false;
        }
    };
    {
        Scope scope(*this);
        rhiContext_->resetRing();
        record(*rhiContext_);
    }

    if (!hrOk(standaloneList_->Close(), "standalone list Close")) return false;
    ID3D12CommandList* lists[] = {standaloneList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitForGpu();
    if (infoQueue_) drainDebugMessages();
    return !deviceLost_;
}

// Polls video memory budget and usage. See VideoMemoryInfo for LOCAL/NON_LOCAL split.
VideoMemoryInfo D3D12Device::videoMemory() const {
    VideoMemoryInfo info;
    if (!adapter3_) return info;   // no IDXGIAdapter3 on this device -- see adapter3_'s own comment
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    if (FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
        return info;   // leaves `info` at its unsupported, all-zero default
    info.supported = true;
    info.localBudgetBytes = local.Budget;
    info.localUsageBytes = local.CurrentUsage;
    info.nonLocalBudgetBytes = nonLocal.Budget;
    info.nonLocalUsageBytes = nonLocal.CurrentUsage;
    return info;
}

// Lazily re-adopts depthBuffer_ when createDepthBuffer() runs, using depthTexDirty_ as trigger.
TextureHandle D3D12Device::sceneDepthTexture() {
    if (!depthBuffer_ || !rhiFactory_) return 0;
    if (depthTexDirty_) {
        depthTexHandle_ = rhiFactory_->adoptExternalDepthTexture(
            depthBuffer_.Get(), sceneWidth_, sceneHeight_, depthTexHandle_);
        depthTexDirty_ = false;
    }
    return depthTexHandle_;
}

// Turns the G-buffer feature on/off. Applied immediately when swapchain exists.
void D3D12Device::setGBufferEnabled(bool on) {
    if (on == gbufferEnabled_) return;
    gbufferEnabled_ = on;
    gbufMsaaWarned_ = false;
    gbufHistoryInvalid_ = true;
    if (!hasSwapchain_) return;
    waitForGpu();
    if (gbufferEnabled_) {
        if (!createGBufferTargets()) {
            AVER_ERROR("[RHI.D3D12] G-buffer target creation failed; leaving the feature disabled");
            releaseGBufferTargets();
            gbufferEnabled_ = false;
            return;
        }
        AVER_INFO("[RHI.D3D12] G-buffer enabled ({}x{})", sceneWidth_, sceneHeight_);
    } else {
        releaseGBufferTargets();
        AVER_INFO("[RHI.D3D12] G-buffer disabled");
    }
    notifyRenderTargetsChanged();
}

// Re-adopts all three G-buffer resources to refresh handles on any accessor's call.
void D3D12Device::refreshGBufferTexHandles() {
    if (!gbufTexDirty_) return;
    gbufVelocityTexHandle_ = rhiFactory_->adoptExternalRenderTargetTexture(
        gbufVelocity_.Get(), Format::RG16F, sceneWidth_, sceneHeight_,
        "GBuffer.Velocity (adopted)", gbufVelocityTexHandle_);
    gbufViewZTexHandle_ = rhiFactory_->adoptExternalRenderTargetTexture(
        gbufViewZ_.Get(), Format::R32Float, sceneWidth_, sceneHeight_,
        "GBuffer.ViewZ (adopted)", gbufViewZTexHandle_);
    gbufNormalRoughTexHandle_ = rhiFactory_->adoptExternalRenderTargetTexture(
        gbufNormalRough_.Get(), Format::RGB10A2Unorm, sceneWidth_, sceneHeight_,
        "GBuffer.NormalRoughness (adopted)", gbufNormalRoughTexHandle_);
    gbufTexDirty_ = false;
}

// Nearest-sample resolve of the MSAA G-buffer twins into the single-sample targets. Pipeline and
// SRVs through the RHI factory; the targets are bound raw (adopted textures carry no RTV).
bool D3D12Device::resolveGBufferMsaa() {
    if (gbufResolveFailed_ || !rhiFactory_ || !rhiContext_) return false;
    if (!gbufVelocityMs_ || !gbufViewZMs_ || !gbufNormalRoughMs_) return false;
    if (!gbufResolvePso_) {
        const std::string& src = shaderFile("gbuffer_msaa_resolve.hlsl");
        ShaderDesc vsd{};
        vsd.source = src.c_str(); vsd.entry = "GBufResolveVS"; vsd.stage = ShaderStage::Vertex; vsd.minShaderModel = 60;
        ShaderDesc psd = vsd;
        psd.entry = "GBufResolvePS"; psd.stage = ShaderStage::Pixel;
        const ShaderHandle vs = src.empty() ? 0 : rhiFactory_->createShader(vsd);
        const ShaderHandle ps = src.empty() ? 0 : rhiFactory_->createShader(psd);
        if (vs && ps) {
            GraphicsPipelineDesc d{};
            d.vs = vs; d.ps = ps;
            d.layout.srvCount = 3;
            d.layout.slotKindsDeclared = true;
            for (u32 i = 0; i < 3; ++i) d.layout.srvKinds[i] = SlotKind::Texture2DMS;
            d.cull = CullMode::None;
            d.depthClip = false;
            d.renderTargetCount = 3;
            d.renderTargets[0] = Format::RG16F;
            d.renderTargets[1] = Format::R32Float;
            d.renderTargets[2] = Format::RGB10A2Unorm;
            d.sampleCount = 1;
            gbufResolvePso_ = rhiFactory_->createGraphicsPipeline(d);
        }
        if (vs) rhiFactory_->destroyShader(vs);
        if (ps) rhiFactory_->destroyShader(ps);
        if (!gbufResolveSet_) {
            BindingSetDesc bd{};
            bd.srvCount = 3;
            for (u32 i = 0; i < 3; ++i) bd.srvKinds[i] = SlotKind::Texture2DMS;
            gbufResolveSet_ = rhiFactory_->createBindingSet(bd);
        }
        if (!gbufResolvePso_ || !gbufResolveSet_) {
            gbufResolveFailed_ = true;
            AVER_WARN("[RHI.D3D12] gbuffer_msaa_resolve.hlsl would not build; the G-buffer stays unwritten under MSAA");
            return false;
        }
    }
    if (gbufMsTexDirty_) {
        gbufMsTex_[0] = rhiFactory_->adoptExternalRenderTargetTexture(gbufVelocityMs_.Get(), Format::RG16F,
            sceneWidth_, sceneHeight_, "GBuffer.Velocity MSAA (adopted)", gbufMsTex_[0]);
        gbufMsTex_[1] = rhiFactory_->adoptExternalRenderTargetTexture(gbufViewZMs_.Get(), Format::R32Float,
            sceneWidth_, sceneHeight_, "GBuffer.ViewZ MSAA (adopted)", gbufMsTex_[1]);
        gbufMsTex_[2] = rhiFactory_->adoptExternalRenderTargetTexture(gbufNormalRoughMs_.Get(), Format::RGB10A2Unorm,
            sceneWidth_, sceneHeight_, "GBuffer.NormalRoughness MSAA (adopted)", gbufMsTex_[2]);
        gbufMsTexDirty_ = false;
    }
    for (u32 i = 0; i < 3; ++i) rhiFactory_->setSrv(gbufResolveSet_, i, gbufMsTex_[i], kAllMips);

    ID3D12Resource* ms[3] = {gbufVelocityMs_.Get(), gbufViewZMs_.Get(), gbufNormalRoughMs_.Get()};
    D3D12_RESOURCE_BARRIER toRead[3], toRt[3];
    for (u32 i = 0; i < 3; ++i) {
        toRead[i] = transition(ms[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        toRt[i]   = transition(ms[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cmdList_->ResourceBarrier(3, toRead);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
        gbufVelocityRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
        gbufViewZRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
        gbufNormalRoughRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
    };
    cmdList_->OMSetRenderTargets(3, rtvs, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(sceneWidth_), static_cast<f32>(sceneHeight_), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(sceneWidth_), static_cast<LONG>(sceneHeight_)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);
    rhiContext_->setPipeline(gbufResolvePso_);
    rhiContext_->setBindingSet(gbufResolveSet_, 0);
    rhiContext_->drawFullscreen();
    cmdList_->ResourceBarrier(3, toRt);
    // The RHI bound its own root signature, heap and pipeline.
    boundRootSig_ = nullptr; boundPso_ = nullptr; boundHeap_ = nullptr;
    fovValid_ = false; dbValid_ = false;
    return true;
}

// Re-checks gbufferEnabled_ before returning (0 means off after disable, even if handles are non-zero).
TextureHandle D3D12Device::gBufferVelocityTexture() {
    if (!gbufferEnabled_ || !gbufVelocity_ || !rhiFactory_) return 0;
    refreshGBufferTexHandles();
    return gbufVelocityTexHandle_;
}
TextureHandle D3D12Device::gBufferViewZTexture() {
    if (!gbufferEnabled_ || !gbufViewZ_ || !rhiFactory_) return 0;
    refreshGBufferTexHandles();
    return gbufViewZTexHandle_;
}
TextureHandle D3D12Device::gBufferNormalRoughnessTexture() {
    if (!gbufferEnabled_ || !gbufNormalRough_ || !rhiFactory_) return 0;
    refreshGBufferTexHandles();
    return gbufNormalRoughTexHandle_;
}

void D3D12Device::addRenderFeature(IRenderFeature* f) {
    if (!f) return;
    for (IRenderFeature* e : features_) if (e == f) return;
    features_.push_back(f);
    AVER_INFO("[RHI.D3D12] render feature registered: {}", f->name());
    // Notify early if swapchain exists, since onRenderTargetsChanged may never be called again.
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
    // 64-bit atomics from device, not shader model.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))))
            caps_.shaderInt64Atomics = o1.Int64ShaderOps != FALSE;
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

    // Ray-traced bindless: tier 1.1 + tier 3 binding (checked here, else descriptor index out of bounds).
    caps_.rtBindlessTextures = caps_.rayTracingTier >= 11 && caps_.resourceBindingTier >= 3;
    if (caps_.rayTracingTier >= 11 && caps_.resourceBindingTier < 3)
        AVER_WARN("[RHI.D3D12] DXR 1.1 with resource binding tier {} -- not the tier 3 this path "
                  "assumes, so ray-traced texturing stays off. Please report this device.",
                  caps_.resourceBindingTier);

    AVER_INFO("[RHI.D3D12] caps: MSAA {}x, RT tier {}, SM {}, mesh-shader tier {}, DXC {}, cons-raster {}, binding tier {}",
              caps_.maxMsaaSamples, caps_.rayTracingTier, caps_.shaderModel,
              caps_.meshShaderTier, caps_.dxcAvailable, caps_.conservativeRaster,
              caps_.resourceBindingTier);
    AVER_INFO("[RHI.D3D12] ray-traced bindless textures: {}", caps_.rtBindlessTextures ? "yes" : "no");
    // Logged beside SM: engine compiles SM 6.5; device may report 6.6 but atomic support differs.
    AVER_INFO("[RHI.D3D12] 64-bit shader atomics: {}", caps_.shaderInt64Atomics ? "yes" : "no");
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
        // The G-buffer's MSAA twins follow the sample count; the single-sample targets stay.
        gbufMsaaWarned_ = false;
        gbufHistoryInvalid_ = true;
        if (gbufferEnabled_) {
            releaseGBufferTargets();
            if (!createGBufferTargets()) {
                AVER_ERROR("[RHI.D3D12] G-buffer target creation failed at MSAA {}x; disabling it", samples);
                releaseGBufferTargets();
                gbufferEnabled_ = false;
            }
        }
    }
    notifyRenderTargetsChanged();
    AVER_INFO("[RHI.D3D12] MSAA set to {}x", samples);
    return true;
}

// Offscreen for post-chain compositing. Full backbuffer size.
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

void D3D12Device::releaseMirror() {
    for (auto& i : mirrorImages_) i.Reset();
    for (auto& b : mirrorSwapBuffers_) b.Reset();
    mirrorSwap_.Reset();
    mirrorHwnd_ = nullptr;
    mirrorW_ = mirrorH_ = 0;
}

bool D3D12Device::setMirrorWindow(void* windowHandle, u32 w, u32 h) {
    if (!hasSwapchain_ || deviceLost_ || !presentQueue_) return false;
    HWND hwnd = static_cast<HWND>(windowHandle);
    if (hwnd && hwnd == mirrorHwnd_ && w == mirrorW_ && h == mirrorH_) return true;
    // The present thread reads the mirror; nothing may be queued or in flight while it changes.
    waitForGpu();
    drainPresents();
    pendingReal_ = false;
    const bool resizeOnly = hwnd && hwnd == mirrorHwnd_ && mirrorSwap_;
    for (auto& i : mirrorImages_) i.Reset();
    for (auto& b : mirrorSwapBuffers_) b.Reset();
    if (!hwnd || w == 0 || h == 0) { releaseMirror(); return true; }

    const UINT scFlags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (resizeOnly) {
        if (!hrOk(mirrorSwap_->ResizeBuffers(kSwapBufferCount, w, h, kBackbufferFormat, scFlags), "ResizeBuffers (mirror)")) {
            releaseMirror();
            return false;
        }
    } else {
        releaseMirror();
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w; sd.Height = h;
        sd.Format = kBackbufferFormat;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kSwapBufferCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SampleDesc.Count = 1;
        sd.Flags = scFlags;
        ComPtr<IDXGISwapChain1> sc1;
        if (!hrOk(factory_->CreateSwapChainForHwnd(presentQueue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1),
                  "CreateSwapChainForHwnd (mirror)"))
            return false;
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (!hrOk(sc1.As(&mirrorSwap_), "As IDXGISwapChain3 (mirror)")) { releaseMirror(); return false; }
        mirrorHwnd_ = hwnd;
    }
    for (u32 i = 0; i < kSwapBufferCount; ++i)
        if (!hrOk(mirrorSwap_->GetBuffer(i, IID_PPV_ARGS(&mirrorSwapBuffers_[i])), "mirror GetBuffer")) {
            releaseMirror();
            return false;
        }
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = kBackbufferFormat;
    td.SampleDesc.Count = 1;
    auto heap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    for (u32 i = 0; i < kBackBufferCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&mirrorImages_[i])), "mirror image")) {
            releaseMirror();
            return false;
        }
        setDebugName(mirrorImages_[i].Get(), "Aver mirror image");
    }
    mirrorW_ = w; mirrorH_ = h;
    mirrorFailLogged_ = false;
    AVER_INFO("[RHI.D3D12] mirror window {}x{}", w, h);
    return true;
}

// Notifies features of render-target changes (scene size, sample count, format).
void D3D12Device::notifyRenderTargetsChanged() {
    if (sampleCount_ == notifiedSamples_ &&
        backbufferFormat() == notifiedColor_ && depthFormat() == notifiedDepth_ &&
        sceneWidth_ == notifiedWidth_ && sceneHeight_ == notifiedHeight_) return;
    notifiedSamples_ = sampleCount_;
    notifiedColor_   = backbufferFormat();
    notifiedDepth_   = depthFormat();
    notifiedWidth_   = sceneWidth_;
    notifiedHeight_  = sceneHeight_;
    // Invalidate G-buffer history: resolution/format changed, so can't reproject old frames.
    gbufHistoryInvalid_ = true;
    for (IRenderFeature* f : features_)
        f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat(), sceneWidth_, sceneHeight_);
}

// Builds the backend's own root signature and its solid, sky and line pipelines.
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
    // Sky at EXACTLY the far plane (1.0). EQUAL test (not GREATER_EQUAL) to avoid rendering in front of opaque.
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

    // EditorLines owns its own PSOs, built lazily and replayed after the post chain.

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
// Default-heap buffer for acceleration structures or scratch space.
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

// Acquires the DXR 1.1 interfaces for acceleration structures.
bool D3D12Device::initAccelerationStructures() {
    if (caps_.rayTracingTier < 11 || caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    if (FAILED(device_.As(&device5_))) return false;
    AVER_INFO("[RHI.D3D12] DXR 1.1 acceleration structures available");
    return true;
}

namespace {
// One {type, value} pair of a D3D12 pipeline-state subobject stream.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
    Subobject& operator=(const T& v) { value = v; return *this; }
};

// Mesh-shader PSO stream. `as` is optional (zero-length bytecode = omitted).
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

// Mesh shader path (Tier 1 + SM 6.5 + DXC).
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
void D3D12Device::bindGraphicsRoot(ID3D12RootSignature* rs) {
    if (boundRootSig_ == rs) return;
    boundRootSig_ = rs;
    cmdList_->SetGraphicsRootSignature(rs);
    // Root signature change discards all bound arguments. Clear both fovValid_ and dbValid_.
    fovValid_ = false;
    dbValid_ = false;
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
    sd.BufferCount = kSwapBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    if (tearingSupported_) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // Swapchain on present queue: copies and Presents never queue behind rendering.
    D3D12_COMMAND_QUEUE_DESC pq{};
    pq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    pq.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (!hrOk(device_->CreateCommandQueue(&pq, IID_PPV_ARGS(&presentQueue_)), "CreateCommandQueue (present)")) return false;
    setDebugName(presentQueue_.Get(), "Aver present queue");

    HWND hwnd = static_cast<HWND>(d.windowHandle);
    ComPtr<IDXGISwapChain1> sc1;
    if (!hrOk(factory_->CreateSwapChainForHwnd(presentQueue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd")) return false;
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!hrOk(sc1.As(&swapChain_), "As IDXGISwapChain3")) return false;
    imageNext_ = 0;
    bbIndex_ = 0;
    frameIndex_ = 0;
    queryDisplayRefresh();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kBackBufferCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!hrOk(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "RTV heap")) return false;
    rtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dd{};
    dd.NumDescriptors = 1;
    dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (!hrOk(device_->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsvHeap_)), "DSV heap")) return false;

    if (!createPresentImages()) return false;

    D3D12_DESCRIPTOR_HEAP_DESC mh{};
    mh.NumDescriptors = 1;
    mh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!hrOk(device_->CreateDescriptorHeap(&mh, IID_PPV_ARGS(&msaaRtvHeap_)), "MSAA RTV heap")) return false;

    if (!createDepthBuffer()) return false;
    if (!createMsaaColor()) return false;
    // G-buffer optional, deferred if enabled before swapchain exists.
    if (gbufferEnabled_ && !createGBufferTargets()) {
        AVER_ERROR("[RHI.D3D12] G-buffer target creation failed at swapchain creation; disabling the feature");
        releaseGBufferTargets();
        gbufferEnabled_ = false;
    }

    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])), "CreateCommandAllocator")) return false;
        if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocatorsGen_[i])), "CreateCommandAllocator (frame interpolation)")) return false;
        if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocatorsMid_[i])), "CreateCommandAllocator (frame midpoint)")) return false;
    }
    if (!hrOk(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&cmdList_)), "CreateCommandList")) return false;
    // Single command list for all draws and dispatches; DRED breadcrumb needs its name.
    setDebugName(cmdList_.Get(), "Aver main direct command list");
    cmdList_.As(&cmdList4_);
    cmdList_.As(&cmdList6_);
    cmdList_->Close();
    // initAccelerationStructures() already ran. buildBlas/buildTlas record onto cmdList4_ if present.
    if (cmdList6_) initMeshShaders();

    D3D12_RESOURCE_DESC bbDesc = renderTargets_[0]->GetDesc();
    UINT64 total = 0;
    device_->GetCopyableFootprints(&bbDesc, 0, 1, 0, &captureFp_, nullptr, nullptr, &total);
    auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = bufferDesc(total);
    hrOk(device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captureBuf_)), "capture buffer");

    fenceValues_[0] = fenceValues_[1] = 0; nextFence_ = 0;
    if (!startPresentThread()) return false;
    hasSwapchain_ = true;
    AVER_INFO("[RHI.D3D12] swapchain {}x{} + depth (D32) ({} buffers, FLIP_DISCARD; {} present images, "
              "presented from the present thread)", width_, height_, kSwapBufferCount, kBackBufferCount);
    return true;
}

// Present images and RTVs, created in COMMON state (PRESENT).
bool D3D12Device::createPresentImages() {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width_;
    td.Height = height_;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = kBackbufferFormat;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto heap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < kBackBufferCount; ++i) {
        if (!hrOk(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&renderTargets_[i])), "present image"))
            return false;
        setDebugName(renderTargets_[i].Get(), "Aver present image");
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, rtv);
        rtv.ptr += rtvSize_;
        imageLastUse_[i] = 0;
    }
    for (u32 i = 0; i < kSwapBufferCount; ++i)
        if (!hrOk(swapChain_->GetBuffer(i, IID_PPV_ARGS(&swapBuffers_[i])), "swapchain GetBuffer")) return false;
    return true;
}

void D3D12Device::createRenderTargetViews() { createPresentImages(); }

// ---- the present thread ----

bool D3D12Device::startPresentThread() {
    if (presentThread_.joinable()) return true;
    if (!readyFence_ && !hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readyFence_)), "present ready fence"))
        return false;
    if (!doneFence_ && !hrOk(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&doneFence_)), "present done fence"))
        return false;
    for (u32 i = 0; i < kPresentAllocs; ++i)
        if (!presentAllocs_[i] && !hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                        IID_PPV_ARGS(&presentAllocs_[i])), "present allocator"))
            return false;
    if (!presentList_) {
        if (!hrOk(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presentAllocs_[0].Get(), nullptr,
                                             IID_PPV_ARGS(&presentList_)), "present command list"))
            return false;
        setDebugName(presentList_.Get(), "Aver present copy list");
        presentList_->Close();
    }
    if (!presentEvent_) presentEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    presentStop_ = false;
    presentError_ = S_OK;
    presentThread_ = std::thread([this] { presentThreadMain(); });
    return true;
}

// Signals the thread to exit and joins it.
void D3D12Device::stopPresentThread() {
    if (!presentThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(presentMu_);
        presentStop_ = true;
    }
    presentCv_.notify_all();
    presentThread_.join();
}

void D3D12Device::drainPresents() {
    {
        std::unique_lock<std::mutex> lk(presentMu_);
        presentCv_.wait_for(lk, std::chrono::seconds(2), [&] { return presentQ_.empty(); });
    }
    if (doneFence_ && requestSerial_ && doneFence_->GetCompletedValue() < requestSerial_ && presentEvent_) {
        doneFence_->SetEventOnCompletion(requestSerial_, presentEvent_);
        WaitForSingleObject(presentEvent_, 2000);
    }
}

void D3D12Device::presentThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    u32 slot = 0;
    for (;;) {
        PresentRequest r;
        {
            std::unique_lock<std::mutex> lk(presentMu_);
            presentCv_.wait(lk, [&] { return presentStop_ || !presentQ_.empty(); });
            if (presentQ_.empty()) return;
            r = presentQ_.front();
        }
        // Image drawn once render queue passes `ready`; present queue waits on GPU.
        presentQueue_->Wait(readyFence_.Get(), r.ready);

        slot = (slot + 1) % kPresentAllocs;
        if (presentAllocUse_[slot] && doneFence_->GetCompletedValue() < presentAllocUse_[slot]) {
            doneFence_->SetEventOnCompletion(presentAllocUse_[slot], presentEvent_);
            WaitForSingleObject(presentEvent_, 2000);
        }
        presentAllocs_[slot]->Reset();
        presentList_->Reset(presentAllocs_[slot].Get(), nullptr);
        ID3D12Resource* dst = swapBuffers_[swapChain_->GetCurrentBackBufferIndex()].Get();
        ID3D12Resource* src = renderTargets_[r.image].Get();
        D3D12_RESOURCE_BARRIER pre[2] = {
            transition(dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        presentList_->ResourceBarrier(2, pre);
        presentList_->CopyResource(dst, src);
        D3D12_RESOURCE_BARRIER post[2] = {
            transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
            transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        };
        presentList_->ResourceBarrier(2, post);
        const bool mirror = r.mirror && mirrorSwap_ && mirrorImages_[r.image];
        if (mirror) {
            ID3D12Resource* mdst = mirrorSwapBuffers_[mirrorSwap_->GetCurrentBackBufferIndex()].Get();
            ID3D12Resource* msrc = mirrorImages_[r.image].Get();
            D3D12_RESOURCE_BARRIER mpre[2] = {
                transition(mdst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                transition(msrc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            };
            presentList_->ResourceBarrier(2, mpre);
            presentList_->CopyResource(mdst, msrc);
            D3D12_RESOURCE_BARRIER mpost[2] = {
                transition(mdst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                transition(msrc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            };
            presentList_->ResourceBarrier(2, mpost);
        }
        presentList_->Close();
        ID3D12CommandList* lists[] = {presentList_.Get()};
        presentQueue_->ExecuteCommandLists(1, lists);

        const i64 t0 = qpcNow();
        const HRESULT hr = swapChain_->Present(r.sync, r.flags);
        // The mirror never waits for vblank and never fails the main present.
        if (mirror) {
            const HRESULT mhr = mirrorSwap_->Present(0, tearingSupported_ ? DXGI_PRESENT_ALLOW_TEARING : 0u);
            if (FAILED(mhr) && !mirrorFailLogged_.exchange(true)) {
                AVER_WARN("[RHI.D3D12] mirror window Present failed 0x{:08X}", static_cast<u32>(mhr));
            }
        }
        presentBlockedUs_ += static_cast<u64>(qpcMs(t0, qpcNow()) * 1000.0);
        ++presentCount_;
        presentQueue_->Signal(doneFence_.Get(), r.serial);
        presentAllocUse_[slot] = r.serial;
        {
            std::lock_guard<std::mutex> lk(presentMu_);
            presentQ_.pop_front();
            if (FAILED(hr)) presentError_ = hr;
        }
        presentCv_.notify_all();
    }
}

// Blocks while kPresentQueueMax requests are queued (display cannot keep up).
bool D3D12Device::queuePresent(u32 image, UINT sync, UINT flags) {
    if (FAILED(queue_->Signal(readyFence_.Get(), readySerial_ + 1))) {
        noteDeviceRemoved("the present-image fence signal", DXGI_ERROR_DEVICE_REMOVED);
        return false;
    }
    ++readySerial_;
    PresentRequest r{image, readySerial_, ++requestSerial_, sync, flags, mirrorSwap_ != nullptr};
    imageLastUse_[image] = r.serial;
    HRESULT err = S_OK;
    const i64 t0 = qpcNow();
    {
        std::unique_lock<std::mutex> lk(presentMu_);
        presentCv_.wait(lk, [&] { return presentQ_.size() < kPresentQueueMax || FAILED(presentError_); });
        err = presentError_;
        if (SUCCEEDED(err)) presentQ_.push_back(r);
    }
    waitPresentMs_ += qpcMs(t0, qpcNow());
    presentCv_.notify_all();
    if (FAILED(err)) {
        if (err == DXGI_ERROR_DEVICE_REMOVED || err == DXGI_ERROR_DEVICE_RESET) noteDeviceRemoved("Present", err);
        else AVER_ERROR("[RHI.D3D12] Present failed 0x{:08X}", (u32)err);
        return false;
    }
    return true;
}

// Waits (GPU-side) until the present thread has copied the image out.
void D3D12Device::waitImageFree(u32 image) {
    if (imageLastUse_[image]) queue_->Wait(doneFence_.Get(), imageLastUse_[image]);
}

// Creates depth target at the current scene size and sample count.
bool D3D12Device::createDepthBuffer() {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = sceneWidth_; td.Height = sceneHeight_;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    // TYPELESS resource (see kDepthResourceFormat comment).
    td.Format = kDepthResourceFormat; td.SampleDesc.Count = sampleCount_;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // Clear value uses concrete format (D32_FLOAT), matching the DSV view format below.
    D3D12_CLEAR_VALUE cv{}; cv.Format = kDepthFormat; cv.DepthStencil.Depth = 1.0f;
    auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depthBuffer_)), "depth buffer")) return false;
    // Explicit view desc required for typeless resource: includes sample count and format.
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = kDepthFormat;
    dv.ViewDimension = (sampleCount_ > 1) ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthBuffer_.Get(), &dv, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    // Re-adopts lazily on next sceneDepthTexture() call.
    depthTexDirty_ = true;
    return true;
}

// Creates scene colour target at the current scene size and sample count.
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

// Creates three G-buffer targets at SCENE size, single-sample (read by compute passes).
// With MSAA, cannot bind alongside msaaColor_ due to SampleDesc mismatch.
bool D3D12Device::createGBufferTargets() {
    if (sceneWidth_ == 0 || sceneHeight_ == 0) return false;

    auto makeTarget = [&](DXGI_FORMAT fmt, const f32 clearColor[4], ComPtr<ID3D12Resource>& outRes,
                          ComPtr<ID3D12DescriptorHeap>& outHeap, const char* debugName, u32 samples = 1) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = sceneWidth_; td.Height = sceneHeight_;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = samples;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{}; cv.Format = fmt;
        cv.Color[0] = clearColor[0]; cv.Color[1] = clearColor[1];
        cv.Color[2] = clearColor[2]; cv.Color[3] = clearColor[3];
        auto def = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        if (!hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&outRes)), debugName))
            return false;
        setDebugName(outRes.Get(), debugName);

        if (!outHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 1;
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (!hrOk(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&outHeap)), "G-buffer RTV heap"))
                return false;
        }
        device_->CreateRenderTargetView(outRes.Get(), nullptr, outHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    };

    // Clear values at file scope so OPTIMISED clear values and per-frame clears can't drift.
    if (!makeTarget(kGBufVelocityFormat, kGBufVelocityClear, gbufVelocity_, gbufVelocityRtvHeap_, "GBuffer.Velocity"))
        return false;
    if (!makeTarget(kGBufViewZFormat, kGBufViewZClear, gbufViewZ_, gbufViewZRtvHeap_, "GBuffer.ViewZ"))
        return false;
    if (!makeTarget(kGBufNormalRoughFormat, kGBufNormalRoughClear, gbufNormalRough_, gbufNormalRoughRtvHeap_,
                    "GBuffer.NormalRoughness"))
        return false;
    if (sampleCount_ > 1) {
        if (!makeTarget(kGBufVelocityFormat, kGBufVelocityClear, gbufVelocityMs_, gbufVelocityMsRtvHeap_,
                        "GBuffer.Velocity MSAA", sampleCount_) ||
            !makeTarget(kGBufViewZFormat, kGBufViewZClear, gbufViewZMs_, gbufViewZMsRtvHeap_,
                        "GBuffer.ViewZ MSAA", sampleCount_) ||
            !makeTarget(kGBufNormalRoughFormat, kGBufNormalRoughClear, gbufNormalRoughMs_, gbufNormalRoughMsRtvHeap_,
                        "GBuffer.NormalRoughness MSAA", sampleCount_))
            return false;
        gbufMsTexDirty_ = true;
    }

    gbufTexDirty_ = true;   // freshly (re)allocated; the adopted TextureHandles must re-adopt
    return true;
}

// Frees ~54 MB; TextureHandles stay live (gBufferEnabled checks the flag, not the handle).
void D3D12Device::releaseGBufferTargets() {
    gbufVelocity_.Reset();    gbufVelocityRtvHeap_.Reset();
    gbufViewZ_.Reset();       gbufViewZRtvHeap_.Reset();
    gbufNormalRough_.Reset(); gbufNormalRoughRtvHeap_.Reset();
    gbufVelocityMs_.Reset();    gbufVelocityMsRtvHeap_.Reset();
    gbufViewZMs_.Reset();       gbufViewZMsRtvHeap_.Reset();
    gbufNormalRoughMs_.Reset(); gbufNormalRoughMsRtvHeap_.Reset();
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

    // AABB, not tight sphere; radius conservatively rounds up (false positives cost GPU, false negatives wrong pictures).
    {
        f32 lo[3] = {verts[0].px, verts[0].py, verts[0].pz};
        f32 hi[3] = {verts[0].px, verts[0].py, verts[0].pz};
        for (u32 i = 1; i < vcount; ++i) {
            const f32 p[3] = {verts[i].px, verts[i].py, verts[i].pz};
            for (int a = 0; a < 3; ++a) { lo[a] = std::fmin(lo[a], p[a]); hi[a] = std::fmax(hi[a], p[a]); }
        }
        for (int a = 0; a < 3; ++a) { m.boundsMin[a] = lo[a]; m.boundsMax[a] = hi[a]; }
        for (int a = 0; a < 3; ++a) m.boundsCentre[a] = 0.5f * (lo[a] + hi[a]);
        const f32 dx = hi[0] - m.boundsCentre[0], dy = hi[1] - m.boundsCentre[1], dz = hi[2] - m.boundsCentre[2];
        m.boundsRadius = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const u64 vbytes = static_cast<u64>(vcount) * sizeof(MeshVertex);
    const u64 ibytes = static_cast<u64>(icount) * sizeof(u32);

    // Requested heap, forced to Upload if Default fails.
    bool useDefault = staticMeshDefaultHeap_;

    // Through FACTORY for descriptors; lambda avoids duplication.
    auto allocateAndUpload = [&](bool onDefaultHeap) -> bool {
        BufferDesc vd;
        vd.bytes = vbytes;
        vd.kind = onDefaultHeap ? BufferKind::Default : BufferKind::Upload;
        vd.debugName = "mesh vertices";
        m.vbBuffer = rhiFactory_->createBuffer(vd);

        BufferDesc idd;
        idd.bytes = ibytes;
        idd.kind = onDefaultHeap ? BufferKind::Default : BufferKind::Upload;
        idd.debugName = "mesh indices";
        m.ibBuffer = rhiFactory_->createBuffer(idd);

        RhiBuffer* vrb = rhiFactory_->buffer(m.vbBuffer);
        RhiBuffer* irb = rhiFactory_->buffer(m.ibBuffer);
        if (!vrb || !vrb->res || !irb || !irb->res) {
            AVER_ERROR("[RHI.D3D12] createMesh could not allocate its buffers");
            if (m.vbBuffer) { rhiFactory_->destroyBuffer(m.vbBuffer); m.vbBuffer = 0; }
            if (m.ibBuffer) { rhiFactory_->destroyBuffer(m.ibBuffer); m.ibBuffer = 0; }
            return false;
        }
        m.vb = vrb->res;
        m.ib = irb->res;

        if (onDefaultHeap) {
            // SYNCHRONOUS (mid-frame calls need initialized memory); see seedSkinTargets for deferred.
            if (!rhiFactory_->uploadBuffers({{m.vb.Get(), verts, vbytes}, {m.ib.Get(), indices, ibytes}})) {
                rhiFactory_->destroyBuffer(m.vbBuffer); m.vbBuffer = 0; m.vb.Reset();
                rhiFactory_->destroyBuffer(m.ibBuffer); m.ibBuffer = 0; m.ib.Reset();
                if (!staticMeshDefaultHeapUploadFailWarned_) {
                    staticMeshDefaultHeapUploadFailWarned_ = true;
                    AVER_WARN("[RHI.D3D12] Default-heap upload failed for a static mesh; falling back "
                              "to the Upload heap for it, and for any that fail the same way after it "
                              "(said once)");
                }
                return false;
            }
        } else {
            rhiFactory_->writeBuffer(m.vbBuffer, verts, vbytes, 0);
            rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);
        }
        return true;
    };

    if (!allocateAndUpload(useDefault)) {
        if (!useDefault) return 0;   // the ordinary Upload path itself failed -- nothing left to try
        useDefault = false;
        if (!allocateAndUpload(useDefault)) return 0;
    } else if (useDefault && !staticMeshDefaultHeapLogged_) {
        staticMeshDefaultHeapLogged_ = true;
        AVER_INFO("[RHI.D3D12] static meshes on the Default heap (--mesh-heap default): vertex and "
                  "index buffers uploaded through a one-shot staging copy");
    }

    m.vbv.BufferLocation = m.vb->GetGPUVirtualAddress();
    m.vbv.SizeInBytes = static_cast<UINT>(vbytes);
    m.vbv.StrideInBytes = sizeof(MeshVertex);
    m.ibv.BufferLocation = m.ib->GetGPUVirtualAddress();
    m.ibv.SizeInBytes = static_cast<UINT>(ibytes);
    m.ibv.Format = DXGI_FORMAT_R32_UINT;

    meshes_.push_back(std::move(m));
    return static_cast<MeshHandle>(meshes_.size());
}

// Shares source's vertex buffer, owns its index buffer (inverse of createSkinTargetMesh).
MeshHandle D3D12Device::createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) {
    return shareVertices(source, indices, indexCount, /*posed=*/false);
}
MeshHandle D3D12Device::createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) {
    return shareVertices(posedSource, indices, indexCount, /*posed=*/true);
}
// Shared body. `posed` flips: which source accepted, range-check, and compute-writeable status.
MeshHandle D3D12Device::shareVertices(MeshHandle source, const u32* indices, u32 indexCount, bool posed) {
    const char* what = posed ? "createPosedPartMesh" : "createMeshSharingVertices";
    if (!device_ || !rhiFactory_ || !indices || indexCount == 0) return 0;
    if (source == 0 || source > meshes_.size()) {
        AVER_ERROR("[RHI.D3D12] {} with an invalid source handle", what);
        return 0;
    }
    const GpuMesh& src = meshes_[source - 1];
    // Silent refusals (LOD importer probes; see IDevice contract).
    if (!src.alive || !src.vb || src.vbv.SizeInBytes == 0) return 0;
    // LOD silently refuses compute-written source; posed requires it.
    if (src.computeWritten != posed) {
        if (posed) AVER_WARN("[RHI.D3D12] createPosedPartMesh refused: mesh {} is not compute-written", source);
        return 0;
    }
    // Validate index range (posed buffer is compute-written every frame).
    if (posed) {
        for (u32 k = 0; k < indexCount; ++k)
            if (indices[k] >= src.vertexCount) {
                AVER_WARN("[RHI.D3D12] createPosedPartMesh refused: index {} names vertex {} of {}-vertex mesh {}",
                          k, indices[k], src.vertexCount, source);
                return 0;
            }
    }

    // Collapse sharing chain to root (destroyMesh needs only one decrement).
    const MeshHandle root = src.vbOwned ? source : src.vbSource;
    if (root == 0 || root > meshes_.size() || !meshes_[root - 1].alive) return 0;

    GpuMesh m;
    // Bounds copied from source (coarser index list never exceeds shared buffer extents).
    m.vb = src.vb;
    m.vbv = src.vbv;
    m.vbBuffer = src.vbBuffer;
    m.vertexCount = src.vertexCount;
    m.boundsCentre[0] = src.boundsCentre[0];
    m.boundsCentre[1] = src.boundsCentre[1];
    m.boundsCentre[2] = src.boundsCentre[2];
    m.boundsRadius = src.boundsRadius;
    for (int a = 0; a < 3; ++a) { m.boundsMin[a] = src.boundsMin[a]; m.boundsMax[a] = src.boundsMax[a]; }
    m.vbOwned = false;
    m.vbSource = root;
    m.computeWritten = posed;   // a posed part IS posed geometry; see createPosedPartMesh

    const u64 ibytes = static_cast<u64>(indexCount) * sizeof(u32);
    // Use same Upload/Default policy as createMesh() (lambda avoids duplication).
    bool useDefault = staticMeshDefaultHeap_;
    auto allocateIndices = [&](bool onDefaultHeap) -> bool {
        BufferDesc idd;
        idd.bytes = ibytes;
        idd.kind = onDefaultHeap ? BufferKind::Default : BufferKind::Upload;
        idd.debugName = posed ? "mesh indices (posed part)" : "mesh indices (shared vertices)";
        m.ibBuffer = rhiFactory_->createBuffer(idd);
        RhiBuffer* irb = rhiFactory_->buffer(m.ibBuffer);
        if (!irb || !irb->res) {
            AVER_ERROR("[RHI.D3D12] {} could not allocate its index buffer", what);
            if (m.ibBuffer) { rhiFactory_->destroyBuffer(m.ibBuffer); m.ibBuffer = 0; }
            return false;
        }
        m.ib = irb->res;
        if (onDefaultHeap) {
            if (!rhiFactory_->uploadBuffers({{m.ib.Get(), indices, ibytes}})) {
                rhiFactory_->destroyBuffer(m.ibBuffer); m.ibBuffer = 0; m.ib.Reset();
                if (!staticMeshDefaultHeapUploadFailWarned_) {
                    staticMeshDefaultHeapUploadFailWarned_ = true;
                    AVER_WARN("[RHI.D3D12] Default-heap upload failed for a static mesh; falling back "
                              "to the Upload heap for it, and for any that fail the same way after it "
                              "(said once)");
                }
                return false;
            }
        } else {
            rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);
        }
        return true;
    };

    if (!allocateIndices(useDefault)) {
        if (!useDefault) return 0;
        useDefault = false;
        if (!allocateIndices(useDefault)) return 0;
    } else if (useDefault && !staticMeshDefaultHeapLogged_) {
        staticMeshDefaultHeapLogged_ = true;
        AVER_INFO("[RHI.D3D12] static meshes on the Default heap (--mesh-heap default): vertex and "
                  "index buffers uploaded through a one-shot staging copy");
    }

    m.indexCount = indexCount;
    m.ibv.BufferLocation = m.ib->GetGPUVirtualAddress();
    m.ibv.SizeInBytes = static_cast<UINT>(ibytes);
    m.ibv.Format = DXGI_FORMAT_R32_UINT;
    m.ibOwned = true;   // this mesh's OWN index buffer, unlike its borrowed vertices

    meshes_.push_back(std::move(m));
    const MeshHandle h = static_cast<MeshHandle>(meshes_.size());
    // Counted on ROOT after push_back (reallocation may invalidate src).
    meshes_[root - 1].vbShares += 1;
    return h;
}

// Creates a mesh that shares source's indices but owns a Default-heap, UAV-capable vertex buffer
// for a compute pass to write. See IDevice::createSkinTargetMesh for why this is a creation entry point.
MeshHandle D3D12Device::createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) {
    if (!device_ || !rhiFactory_) return 0;
    if (source == 0 || source > meshes_.size()) {
        AVER_ERROR("[RHI.D3D12] createSkinTargetMesh with an invalid source handle");
        return 0;
    }
    const GpuMesh& src = meshes_[source - 1];
    if (!src.vb || !src.ib || src.vbv.SizeInBytes == 0) return 0;

    // Through FACTORY for UAV descriptor; not a committed resource.
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
    // Bounds copied from source (deformation can exceed seed; better than origin point).
    m.boundsCentre[0] = src.boundsCentre[0];
    m.boundsCentre[1] = src.boundsCentre[1];
    m.boundsCentre[2] = src.boundsCentre[2];
    m.boundsRadius    = src.boundsRadius;
    for (int a = 0; a < 3; ++a) { m.boundsMin[a] = src.boundsMin[a]; m.boundsMax[a] = src.boundsMax[a]; }
    // ibBuffer handle needed for descriptors (refusal was: mesh reported no geometry despite readable indices).
    m.ibBuffer = src.ibBuffer;
    // ibBuffer is BORROWED (don't free source's indices).
    m.ibOwned = false;
    m.ibSource = source;
    // vertexCount: geometry consumer sizes reads by it.
    m.vertexCount = src.vertexCount;
    m.computeWritten = true;   // the whole point of this entry point
    m.vbv.BufferLocation = rb->res->GetGPUVirtualAddress();
    m.vbv.SizeInBytes = src.vbv.SizeInBytes;
    // Stride from source, not re-derived (independent strides -> misread pitch).
    m.vbv.StrideInBytes = src.vbv.StrideInBytes;

    meshes_.push_back(std::move(m));
    const MeshHandle h = static_cast<MeshHandle>(meshes_.size());
    // Counted on SOURCE after push_back (reallocation may invalidate src).
    meshes_[source - 1].ibShares += 1;

    // Queue rest-pose seed (needs open command list; guarantees bind pose if drawn before skinning).
    skinSeeds_.push_back({h, source});
    if (outVertices) *outVertices = vh;
    return h;
}

// Drain rest-pose queue; run at frame start before any draw.
void D3D12Device::seedSkinTargets() {
    if (skinSeeds_.empty() || !cmdList_) return;
    for (const SkinSeed& sd : skinSeeds_) {
        if (sd.dst == 0 || sd.dst > meshes_.size() || sd.src == 0 || sd.src > meshes_.size()) continue;
        GpuMesh& d = meshes_[sd.dst - 1];
        const GpuMesh& s = meshes_[sd.src - 1];
        if (!d.vb || !s.vb) continue;
        // Destination promoted COMMON->COPY_DEST; source (if Default) COMMON->COPY_SOURCE.
        const bool srcIsDefault = [&] {
            RhiBuffer* srb = rhiFactory_->buffer(s.vbBuffer);
            return srb && srb->desc.kind == BufferKind::Default;
        }();
        cmdList_->CopyBufferRegion(d.vb.Get(), 0, s.vb.Get(), 0, d.vbv.SizeInBytes);

        // Promotion lasts for this command list (decay at submit); skinning pass must find COMMON.
        auto back = transition(d.vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COMMON);
        cmdList_->ResourceBarrier(1, &back);
        if (srcIsDefault) {
            // Mirror COPY_SOURCE transition (same lifetime, same reason for explicit undo).
            auto srcBack = transition(s.vb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                      D3D12_RESOURCE_STATE_COMMON);
            cmdList_->ResourceBarrier(1, &srcBack);
        }
        AVER_TRACE("[RHI.D3D12] skin target {} seeded with the rest pose of mesh {}", sd.dst, sd.src);
    }
    skinSeeds_.clear();
}

// Creates the timestamp query heap and its readback buffer. Failure is not fatal.
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

// Read completed timings from this slot (already fenced); fold into tree.
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

    // Fold spans into tree (parents before children guaranteed; lower indices first).
    tsSpanToAccum_.assign(tsSlice_[frameIndex_].size(), kNoAccumParent);
    for (u32 i = 0; i < tsSlice_[frameIndex_].size(); ++i) {
        const GpuSpan& s = tsSlice_[frameIndex_][i];
        if (s.begin >= kMaxGpuStamps || s.end >= kMaxGpuStamps) continue;   // never closed; drop it
        const f64 d = ms(stamps[s.begin], stamps[s.end]);
        // If parent was dropped, nest as top-level (visible bug, not plausible).
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

    // Log on widening interval, averaged over frames.
    if ((tsReports_ & (tsReports_ + 1)) == 0 && tsAccumFrames_ >= 8) {
        const f64 n = static_cast<f64>(tsAccumFrames_);

        std::vector<std::vector<u32>> children(tsAccum_.size());
        std::vector<u32> topLevel;
        for (u32 i = 0; i < tsAccum_.size(); ++i) {
            if (tsAccum_[i].parent == kNoAccumParent) topLevel.push_back(i);
            else                                      children[tsAccum_[i].parent].push_back(i);
        }

        // unmarked = frame minus top-level spans (nested spans already in parent totals).
        f64 topLevelMs = 0;
        for (u32 i : topLevel) topLevelMs += tsAccum_[i].ms;

        // Tree: inclusive ms/frame, exclusive ms/frame (children).
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

// Straight copy: tsAccum_ is already parent-indexed tree; kNoAccumParent/kNoParent share sentinel.
GpuTimingReport D3D12Device::gpuTiming() const {
    GpuTimingReport report;
    // supported = capability (timestamps unavailable); independent of collected frames.
    report.supported = tsEnabled_;
    if (!tsEnabled_ || tsAccumFrames_ == 0) return report;
    report.framesAccumulated = tsAccumFrames_;
    const f64 n = static_cast<f64>(tsAccumFrames_);
    report.nodes.reserve(tsAccum_.size());
    for (const GpuAccum& a : tsAccum_)
        report.nodes.push_back(GpuTimingNode{a.label, a.ms / n, a.parent});
    return report;
}

// Drop tree and frame count (restarts cadence); in-flight unchanged; tsSpanToAccum_ rebuilt per call.
void D3D12Device::resetGpuTiming() {
    tsAccum_.clear();
    tsAccumFrameMs_ = 0;
    tsAccumFrames_ = 0;
    tsReports_ = 0;
}

// Read completed exposure slot (fenced above); leave unchanged if skipped.
void D3D12Device::collectExposureReadout() {
    const u32 f = frameIndex_ < kFrameCount ? frameIndex_ : 0;
    if (!expReadbackPending_[f] || !expReadback_[f]) return;
    expReadbackPending_[f] = false;
    D3D12_RANGE rd{0, 2 * sizeof(u32)};
    void* p = nullptr;
    if (FAILED(expReadback_[f]->Map(0, &rd, &p)) || !p) return;
    f32 value = 0.0f; u32 seeded = 0;
    std::memcpy(&value, p, sizeof value);
    std::memcpy(&seeded, static_cast<const u8*>(p) + sizeof value, sizeof seeded);
    D3D12_RANGE none{0, 0};
    expReadback_[f]->Unmap(0, &none);
    // CSExposure guarantees seeded==1 first run; postExposureReadout contract is false either way.
    if (seeded && std::isfinite(value) && value > 0.0f) {
        expReadoutValue_ = value;
        expReadoutSeeded_ = true;
    }
}

// Opens the frame: waits out the current backbuffer's last frame, resets recording, clears targets.
void D3D12Device::beginFrame() {
    if (!hasSwapchain_ || deviceLost_) return;
    // FIRST: render-scale changes must happen before recording (see setRenderScale comment).
    applyPendingRenderScale();
    reconcileClearValue();
    // Frame slot rotates; present images rotate by 1-2 depending on frame interpolation.
    frameIndex_ = (frameIndex_ + 1) % kFrameCount;
    bbIndex_ = imageNext_;
    const u64 want = fenceValues_[frameIndex_];
    // THE RESULT MUST BE CHECKED: ignored waitFence removed-device was the bug.
    const i64 tWait0 = qpcNow();
    if (want != 0 && !waitFence(want)) return;
    waitFenceMs_ += qpcMs(tWait0, qpcNow());
    ++frameSerial_;   // after the wait: this slot's previous frame has retired (see frameSerial_)
    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get());
    recording_ = true;
    boundRootSig_ = nullptr;
    boundPso_ = pso_.Get();   // Reset's second argument IS the command list's initial bound PSO
    // Reset doesn't carry heaps/root-sig forward; cache dies here.
    boundHeap_ = nullptr;
    fovValid_ = false;   // a reset command list has nothing bound at all
    dbValid_ = false;    // table 1's cache dies here too -- see its member comment for why
    postCBUsed_ = 0;
    drawBinding_ = defaultDrawBinding_;
    drawBlended_ = false;   // sticky per-draw state resets exactly like drawBinding_ just above
    // Re-latched by this frame's own setWireframe(true), if any; wireframe_ itself stays sticky.
    wireframeFrame_ = wireframe_;
    depthOnlyMesh_ = 0;     // drawMesh consumes it; this is the backstop for an unpaired depth-only draw
    // Cleared here, not after flush (both end empty, but clear only here decides frame-start state).
    blendedDraws_.clear();
    blendedPipelineMissingWarned_ = false;   // said at most once per frame; see its own member comment
    nextDrawPrepassed_ = false;   // a reset command list has consumed nothing from last frame either

    // Deferred destroys reclaimed every frame (not just on next create/destroy).
    if (rhiFactory_) rhiFactory_->collect();
    // Fence retired previous use; collect before reusing counters.
    collectGpuTiming();
    // Same fence, same slot, same reasoning -- see collectExposureReadout's own comment.
    collectExposureReadout();
    tsCount_ = 0;
    tsOpen_.clear();
    tsDropped_ = 0;
    tsSlice_[frameIndex_].clear();
    tsSliceBegin_[frameIndex_] = gpuStamp();
    tsSliceEnd_[frameIndex_] = kMaxGpuStamps;
    // Before any feature's prePass and before any draw: seed skin targets.
    seedSkinTargets();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = msaaRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    {
        PerFrameCB up = frameCB_;
        jitterForUpload(up);
        taaJitterThisFrame_[0] = up.jitter[0]; taaJitterThisFrame_[1] = up.jitter[1];
        std::memcpy(frameCBPtr_[frameIndex_], &up, sizeof(PerFrameCB));
    }
    for (IRenderFeature* f : features_) f->beginScene();

    if (rhiContext_) {
        for (IRenderFeature* f : features_) f->prePass(*rhiContext_);
        if (!features_.empty()) { boundRootSig_ = nullptr; boundPso_ = nullptr; }
    }

    // G-buffer bindable when the feature is on and its targets exist; under MSAA the scene pass
    // writes the multisampled twins, which the end of the frame resolves (resolveGBufferMsaa).
    const bool gbufMs = sampleCount_ > 1;
    const bool gbufWritable = gbufferEnabled_ && gbufVelocity_ && gbufViewZ_ && gbufNormalRough_ &&
                              (!gbufMs || (gbufVelocityMs_ && gbufViewZMs_ && gbufNormalRoughMs_ &&
                                           !gbufResolveFailed_));
    if (gbufferEnabled_ && !gbufWritable && !gbufMsaaWarned_) {
        AVER_WARN("[RHI.D3D12] G-buffer is enabled but cannot be written at MSAA {}x; readers see "
                  "cleared targets until that is fixed", sampleCount_);
        gbufMsaaWarned_ = true;
    }
    // Two-part contract: this frame's write status; invalidates next frame on disable or failure.
    gbufHistoryInvalid_ = !gbufWritable;
    gbufWrittenFrame_ = gbufWritable;

    if (gbufWritable) {
        // 4 RTs: scene at slot 0, velocity/viewZ/normal-roughness at 1/2/3 (unwritten slots untouched).
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
            rtv,
            (gbufMs ? gbufVelocityMsRtvHeap_ : gbufVelocityRtvHeap_)->GetCPUDescriptorHandleForHeapStart(),
            (gbufMs ? gbufViewZMsRtvHeap_ : gbufViewZRtvHeap_)->GetCPUDescriptorHandleForHeapStart(),
            (gbufMs ? gbufNormalRoughMsRtvHeap_ : gbufNormalRoughRtvHeap_)->GetCPUDescriptorHandleForHeapStart(),
        };
        cmdList_->OMSetRenderTargets(4, rtvs, FALSE, &dsv);
        for (u32 i = 0; i < 4; ++i) sceneRtvs_[i] = rtvs[i];
        sceneRtvCount_ = 4;
    } else {
        sceneRtvs_[0] = rtv;
        sceneRtvCount_ = 1;
        // Exactly pre-G-buffer bind (feature disabled or MSAA conflict; render oracle depends on this).
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }
    cmdList_->ClearRenderTargetView(rtv, sceneClear_, 0, nullptr);
    if (gbufferEnabled_ && gbufVelocity_ && gbufViewZ_ && gbufNormalRough_) {
        // Cleared regardless of gbufWritable (MSAA mismatch: readers find sentinel, not stale value).
        cmdList_->ClearRenderTargetView(gbufVelocityRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                        kGBufVelocityClear, 0, nullptr);
        cmdList_->ClearRenderTargetView(gbufViewZRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                        kGBufViewZClear, 0, nullptr);
        cmdList_->ClearRenderTargetView(gbufNormalRoughRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                        kGBufNormalRoughClear, 0, nullptr);
        if (gbufWritable && gbufMs) {
            cmdList_->ClearRenderTargetView(gbufVelocityMsRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                            kGBufVelocityClear, 0, nullptr);
            cmdList_->ClearRenderTargetView(gbufViewZMsRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                            kGBufViewZClear, 0, nullptr);
            cmdList_->ClearRenderTargetView(gbufNormalRoughMsRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                            kGBufNormalRoughClear, 0, nullptr);
        }
    }
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Opened here rather than after the suppressesScene loop below, which can return early.
    beginGpuSpan("scene draw");

    // vpX_/vpY_/vpW_/vpH_ already scene-space; fallback is whole scene target.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    D3D12_VIEWPORT vp{rx, ry, rw, rh, 0.0f, 1.0f};
    D3D12_RECT sc{static_cast<LONG>(rx), static_cast<LONG>(ry), static_cast<LONG>(rx + rw), static_cast<LONG>(ry + rh)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);
    sceneDsv_ = dsv;
    sceneVp_ = vp;
    sceneSc_ = sc;

    bindGraphicsRoot(rootSig_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    sceneSuppressed_ = false;
    frameSuppressed_ = false;
    IRenderFeature* winner = nullptr;
    u32 claimants = 0;
    for (IRenderFeature* f : features_) {
        if (!f->suppressesScene()) continue;
        ++claimants;
        if (!winner) winner = f;
    }
    // Log scene painter once per change (silent expensive surprise if ray-driven off, PT running).
    if (winner != lastSuppressWinner_ || claimants != lastSuppressClaimants_) {
        if (claimants > 1)
            AVER_WARN("[RHI.D3D12] {} render features claim the whole scene; '{}' wins on "
                      "registration order and the others will not paint. The rasteriser draws "
                      "NOTHING while this holds -- if you are comparing render paths, this is not "
                      "the comparison you think it is.", claimants, winner->name());
        else if (winner)
            AVER_INFO("[RHI.D3D12] '{}' is painting the scene; the rasteriser's drawMesh calls are "
                      "being dropped. Any 'scene draw' timing below is that feature, not raster.",
                      winner->name());
        else if (lastSuppressWinner_)
            AVER_INFO("[RHI.D3D12] the rasteriser is painting the scene again; no feature is "
                      "suppressing it.");
    }
    lastSuppressWinner_    = winner;
    lastSuppressClaimants_ = claimants;
    lateSceneWinner_ = nullptr;
    if (winner) {
        // A late scene is recorded at the top of endFrame, after this frame's draws have been
        // submitted, so moving objects are drawn where they are this frame (not last frame).
        if (winner->wantsLateScenePass()) lateSceneWinner_ = winner;
        else if (rhiContext_) winner->scenePass(*rhiContext_);
        cmdList_->SetPipelineState(pso_.Get());
        sceneSuppressed_ = true;
        if (winner->suppressesWholeFrame()) frameSuppressed_ = true;
        return;
    }

    // Sky now draws at frame start, after real depth exists (was depth-disabled first).
    cmdList_->SetPipelineState(pso_.Get());
}

// Draws one mesh into the scene, through whichever pipeline owns the lit pass.
// Releases a mesh's GPU memory. See IDevice::destroyMesh for the handle-recycling argument.
bool D3D12Device::destroyMesh(MeshHandle mesh) {
    if (mesh == 0 || mesh > meshes_.size()) return false;
    GpuMesh& m = meshes_[mesh - 1];
    if (!m.alive) return false;   // already destroyed; saying so beats double-freeing

    // A source mesh whose indices someone else still shares cannot go (would corrupt unrelated renders).
    if (m.ibShares > 0) {
        AVER_WARN("[RHI.D3D12] destroyMesh({}) refused: {} skin target(s) still share its indices",
                  mesh, m.ibShares);
        return false;
    }
    // Mirror refusal for vertex-sharing ROOT (would corrupt unrelated renders).
    if (m.vbShares > 0) {
        AVER_WARN("[RHI.D3D12] destroyMesh({}) refused: {} mesh(es) still share its vertices",
                  mesh, m.vbShares);
        return false;
    }

    // The acceleration structures FIRST. A BLAS holds this mesh's vertex and index GPU addresses.
    if (rhiFactory_) rhiFactory_->destroyBlasForMesh(mesh);

    // Then the buffers, through the factory, so they retire behind the fence.
    if (rhiFactory_) {
        // ONLY IF OWNED. A vertex-sharing mesh's vertices belong to its root.
        if (m.vbOwned && m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        // ONLY IF OWNED. A skin target's indices belong to its source.
        if (m.ibOwned && m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
    }
    // Give the source its share back, so a source held open only by this target can now go too.
    if (!m.ibOwned && m.ibSource != 0 && m.ibSource <= meshes_.size()) {
        GpuMesh& src = meshes_[m.ibSource - 1];
        if (src.ibShares > 0) src.ibShares -= 1;
    }
    // Mirror give-back for a vertex-sharing mesh's root.
    if (!m.vbOwned && m.vbSource != 0 && m.vbSource <= meshes_.size()) {
        GpuMesh& root = meshes_[m.vbSource - 1];
        if (root.vbShares > 0) root.vbShares -= 1;
    }

    // Slot CLEARED AND KEPT (never recycled); a stale handle draws nothing, not random memory.
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
    m.vbOwned = true;
    m.vbSource = 0;
    m.boundsRadius = 0.0f;
    m.alive = false;
    return true;
}

// Depth-only pass for feature depthPrepassPipeline. Not folded into drawMesh(): called
// separately to keep the prepass span contiguous.
void D3D12Device::drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
    if (!depthPrepassEnabled_) return;
    depthOnlyDraw(mesh, world, color, /*allowComputeWritten=*/false);
}

// Per-draw depth for alpha-masked draw; not frame-wide gated.
bool D3D12Device::drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
    const bool wrote = depthOnlyDraw(mesh, world, color, /*allowComputeWritten=*/true);
    depthOnlyMesh_ = wrote ? mesh : 0;
    return wrote;
}

// The shared body, so the two entry points above cannot drift apart. True when a depth-only draw was
// actually recorded.
bool D3D12Device::depthOnlyDraw(MeshHandle mesh, const f32 world[16], const f32 color[4],
                                bool allowComputeWritten) {
    if (!hasSwapchain_ || !rhiContext_ || mesh == 0 || mesh > meshes_.size()) return false;
    if (!meshes_[mesh - 1].alive) return false;
    // Compute-written meshes excluded from frame-wide prepass but accepted per-draw; depth and
    // colour read same bytes via depthOnlyMesh_ check.
    if (!allowComputeWritten && meshVertexBuffer(mesh) != 0) return false;
    // When wireframe is active or scene is suppressed, no colour draw consumes the prepass depth.
    if (wireframe_) return false;
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return false;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline()) continue;
        const PipelineHandle pp = f->depthPrepassPipeline();
        if (!pp) return false;   // this feature has no prepass PSO; nothing else offers one either today
        const BindingSetHandle bs = f->sceneBindingSet();
        const void* cb = nullptr; u32 cbBytes = 0;
        const bool haveCb = f->sceneConstants(&cb, &cbBytes) && cb && cbBytes;

        // Cache: reuse fovPso_/fovSet_/fovCbBytes_ to avoid re-sending identical bindings.
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
        // Table 1: material binding shared with colour draw (giLayout).
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        f32 fc[kObjectConstantDwords] = {};
        std::memcpy(fc, world, 16 * sizeof(f32));
        // gBaseColor.a used in alpha test; defaults to zero (IDevice::drawMeshDepthPrepass).
        if (color) std::memcpy(fc + 16, color, 4 * sizeof(f32));
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        rhiContext_->drawMesh(mesh);
        boundRootSig_ = nullptr;
        boundPso_ = nullptr;
        return true;
    }
    return false;
}

void D3D12Device::drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) {
    // Consume nextDrawPrepassed_ before early returns. Prepassed only if drawMeshDepthOnly wrote this handle.
    const bool prepassed = nextDrawPrepassed_ &&
        (meshVertexBuffer(mesh) == 0 || (depthOnlyMesh_ != 0 && mesh == depthOnlyMesh_));
    nextDrawPrepassed_ = false;
    depthOnlyMesh_ = 0;
    if (!hasSwapchain_ || mesh == 0 || mesh > meshes_.size()) return;
    // Destroyed mesh draws nothing, making bugs visible.
    if (!meshes_[mesh - 1].alive) return;

    // Wireframe: mesh queued for EditorLines overlay after post chain.
    if (wireframe_) {
        for (IRenderFeature* f : features_)
            f->submitDraw(mesh, world, color, metallic, roughness,
                          drawBinding_.set, drawBinding_.constants, drawBinding_.bytes, drawBlended_);
        editorLines_.queueWire(mesh, world, meshVertexBuffer(mesh) != 0);
        return;
    }

    // Blended draw: every feature sees it (submitDraw reads `blended` to decide); not in
    // backend's opaque consumers. Not gated on suppressesScene; endFrame gates on frameSuppressed_.
    if (drawBlended_) {
        for (IRenderFeature* f : features_)
            f->submitDraw(mesh, world, color, metallic, roughness,
                          drawBinding_.set, drawBinding_.constants, drawBinding_.bytes, /*blended=*/true);

        BlendedDraw& bd = blendedDraws_.emplace_back();
        bd.mesh = mesh;
        std::memcpy(bd.world, world, 16 * sizeof(f32));
        std::memcpy(bd.color, color, 4 * sizeof(f32));
        bd.metallic = metallic;
        bd.roughness = roughness;
        storeDrawBinding(bd.binding, drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        return;
    }

    for (IRenderFeature* f : features_)
        f->submitDraw(mesh, world, color, metallic, roughness,
                      drawBinding_.set, drawBinding_.constants, drawBinding_.bytes, /*blended=*/false);
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline() || !rhiContext_) continue;
        // Unlit uses feature's pipeline (not fallback, which hardcodes gBaseColor).
        // Prepassed draw must use IA path (depth prepass writes through IA/vsMain, not mesh shaders).
        const bool featureMs = msActive_ && msPso_ && !prepassed;
        const PipelineHandle fp = f->scenePipeline(featureMs, prepassed, false);
        if (!fp) break;
        const BindingSetHandle bs = f->sceneBindingSet();
        const void* cb = nullptr; u32 cbBytes = 0;
        const bool haveCb = f->sceneConstants(&cb, &cbBytes) && cb && cbBytes;

        // Cache: skip pipeline/table-0/frame-CB if unchanged.
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
        fc[20] = metallic; fc[21] = roughness; fc[22] = unlit_ ? 1.0f : 0.0f; fc[23] = 0.0f;
        writeShadingConstants(fc, unlit_);
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        // featureMs: draw call must match pipeline chosen (prepassed = IA path).
        if (featureMs) rhiContext_->dispatchMeshFor(mesh);
        else           rhiContext_->drawMesh(mesh);
        boundRootSig_ = nullptr;
        boundPso_ = nullptr;
        return;
    }

    if (drawBinding_.set && !drawBindingIgnored_) {
        AVER_WARN("[RHI.D3D12] a per-draw binding is set but the scene uses the backend's own pipeline, which declares no table 1; it is ignored");
        drawBindingIgnored_ = true;
    }

    const GpuMesh& m = meshes_[mesh - 1];
    const bool useMs = msActive_ && msPso_;
    bindGraphicsRoot(useMs ? msRootSig_.Get() : rootSig_.Get());
    // Cache PSO: skip re-issuing SetPipelineState if unchanged (hundreds to thousands calls per scene).
    ID3D12PipelineState* wantPso = useMs ? msPso_.Get() : pso_.Get();
    if (wantPso != boundPso_) { cmdList_->SetPipelineState(wantPso); boundPso_ = wantPso; }
    f32 consts[kObjectConstantDwords];
    std::memcpy(consts, world, 16 * sizeof(f32));
    std::memcpy(consts + 16, color, 4 * sizeof(f32));
    consts[20] = metallic; consts[21] = roughness; consts[22] = unlit_ ? 1.0f : 0.0f; consts[23] = 0.0f;
    writeShadingConstants(consts, unlit_);
    cmdList_->SetGraphicsRoot32BitConstants(kSceneObjectParam, kObjectConstantDwords, consts, 0);
    if (useMs) { dispatchMesh(m); return; }
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList_->IASetVertexBuffers(0, 1, &m.vbv);
    cmdList_->IASetIndexBuffer(&m.ibv);
    cmdList_->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
}

// Mesh-shader draw via DispatchMesh; group count from triangle count (must match AVER_MS_TRIS).
void D3D12Device::dispatchMesh(const GpuMesh& m) {
    const u32 tris = m.indexCount / 3;
    if (!tris) return;
    cmdList_->SetGraphicsRootShaderResourceView(kMeshVertexParam, m.vb->GetGPUVirtualAddress());
    cmdList_->SetGraphicsRootShaderResourceView(kMeshIndexParam, m.ib->GetGPUVirtualAddress());
    const u32 tc[4] = {tris, 0, 0, 0};
    cmdList_->SetGraphicsRoot32BitConstants(kMeshCountParam, 4, tc, 0);
    cmdList6_->DispatchMesh((tris + kMeshShaderTrisPerGroup - 1) / kMeshShaderTrisPerGroup, 1, 1);
}

// Uploads line list to GPU; EditorLines owns buffers and slot table.
LineHandle D3D12Device::createLineMesh(const LineVertex* verts, u32 count) {
    return editorLines_.create(verts, count);
}

// Releases line mesh (stale handles not reused).
bool D3D12Device::destroyLineMesh(LineHandle mesh) {
    return editorLines_.destroy(mesh);
}

// Queues line draw for overlay stage after post chain.
void D3D12Device::drawLines(LineHandle mesh, const f32 world[16]) {
    if (!hasSwapchain_) return;
    // Gizmos/wireframes depth-test against ray pass depth.
    for (IRenderFeature* f : features_) if (f->suppressesWholeFrame()) return;
    editorLines_.queue(mesh, world);
}

// ================================================================= the camera post chain
// Everything from here to runPostChain implements rhi::PostSettings.

// CPU twin of shared prelude averInverseTonemap(srgbToLin(c)).
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
    if (s.sunTemperatureK > 0.0f) {
        blackbodySrgb(s.sunTemperatureK, sun);
        // Re-encode to display space (blackbodySrgb returns linear sRGB; readers apply srgbToLin).
        for (int i = 0; i < 3; ++i) sun[i] = std::pow(std::fmax(sun[i], 0.0f), 1.0f / 2.2f);
    }
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
    // Cloud seed is noise-domain offset; seed=0 keeps unseeded path bit-identical.
    if (s.cloudSeed == 0) {
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

    // Write furnace and SH before atmosphere off check (avoid stale rows).
    frameCB_.furnace[0] = s.furnaceRadiance > 0.0f ? 1.0f : 0.0f;
    frameCB_.furnace[1] = s.furnaceRadiance;
    frameCB_.furnace[2] = s.furnaceSun ? 1.0f : 0.0f;
    frameCB_.furnace[3] = 0.0f;

    if (!on) {
        for (int i = 0; i < 4; ++i) frameCB_.atmoSunE0[i] = 0.0f;
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

    // Fog inscatter baked per frame (see RHIShaders.cpp).
    const f32 altKm = std::fmax(frameCB_.camPos[2] * frameCB_.atmoPlanet[2], 1e-3f);
    f32 fogRef[3];
    atmoFogInscatterRef(fit, altKm, s.sunDirection, e0, sunRadius, fogRef);
    for (int i = 0; i < 3; ++i) frameCB_.fogInscatterRef[i] = fogRef[i];
    frameCB_.fogInscatterRef[3] = 0.0f;

    // Sky as nine SH coefficients for ambient (no view/world position dependence).
    AtmosphereSkySH sh{};
    atmoSkyRadianceSH(fit, altKm, s.sunDirection, e0, sunRadius, sh);
    // Scale = 1 (atmosphere's source term is sigma*phase*sunTransmittance*E0).
    constexpr f32 kSkyIrradianceCalibration = 1.0f;
    for (int k = 0; k < 9; ++k) {
        for (int i = 0; i < 3; ++i) frameCB_.skySh[k][i] = sh.c[k][i] * kSkyIrradianceCalibration;
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
    // u0 histogram, u1 exposure, u2 local-exposure grid, u3 its blur. Visibility ALL below already
    // covers the pixel shader, so PSComposite's direct UAV read of u3 needs no extra binding work.
    uavRange.NumDescriptors = 4;
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
    // Local exposure bilateral grid PSO (dispatch gated per-frame in runPostChain).
    if (!makeCompute("CSLocalGrid", localGridPso_)) return false;
    if (!makeCompute("CSLocalBlur", localBlurPso_)) return false;

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
    // Local exposure grid is scene-size dependent (histBuf_/expBuf_ are not).
    localGridBuf_.Reset();
    localGridBlurBuf_.Reset();
    localGridW_ = localGridH_ = 0;
    // Factory-created textures: use destroyTexture (not Reset) to avoid leaking descriptors.
    if (D3D12ResourceFactory* f = rhiFactory_) {
        if (sceneColorTex_) { f->destroyTexture(sceneColorTex_); sceneColorTex_ = 0; }
        if (presentHdrTex_) { f->destroyTexture(presentHdrTex_); presentHdrTex_ = 0; }
        if (blendBackdropTex_) { f->destroyTexture(blendBackdropTex_); blendBackdropTex_ = 0; }
    }
    sceneColorTexW_ = sceneColorTexH_ = presentHdrTexW_ = presentHdrTexH_ = 0;
    blendBackdropW_ = blendBackdropH_ = 0;
    bloomMips_ = bloomW_ = bloomH_ = 0;
    // Exposure readout: independent of scene resolution but dropped at resize (see expReadback_).
    for (u32 i = 0; i < kFrameCount; ++i) {
        expReadback_[i].Reset();
        expReadbackPending_[i] = false;
    }
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

    // ---- exposure readout ----
    // Readback buffer per frame slot for IDevice::postExposureReadout (UI display, non-fatal).
    {
        auto rbHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
        auto rbDesc = bufferDesc(2 * sizeof(u32));
        for (u32 i = 0; i < kFrameCount; ++i) {
            if (!hrOk(device_->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&expReadback_[i])), "post exposure readback"))
                expReadback_[i].Reset();
        }
    }

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

    // AverSR: two intermediates + composite triple, guarded on upscaler_ (avoid unused HDR textures).
    // Blended backdrop: created always (glass needs it); SRV-only, filled by CopyResource.
    if (D3D12ResourceFactory* f = rhiFactory_) {
        TextureDesc bdz;
        bdz.width = sceneWidth_; bdz.height = sceneHeight_;
        bdz.format = fromDxgiFormat(kSceneColorFormat);
        bdz.bind = ResourceBind::ShaderResource;
        bdz.initialState = ResourceState::ShaderResource;
        bdz.debugName = "Blended.Backdrop";
        blendBackdropTex_ = f->createTexture(bdz);
        blendBackdropW_ = sceneWidth_; blendBackdropH_ = sceneHeight_;
    }

    if (upscaler_) {
        if (D3D12ResourceFactory* f = rhiFactory_) {
            // #1: Factory-created alias of scene colour (SRV only, filled by CopyResource each frame).
            TextureDesc sc;
            sc.width = sceneWidth_; sc.height = sceneHeight_;
            sc.format = fromDxgiFormat(kSceneColorFormat);
            sc.bind = ResourceBind::ShaderResource;
            sc.initialState = ResourceState::ShaderResource;
            sc.debugName = "AverSR.SceneColor";
            sceneColorTex_ = f->createTexture(sc);
            sceneColorTexW_ = sceneWidth_; sceneColorTexH_ = sceneHeight_;

            // #2: AverSR output at present size (upscale on radiance before tonemap).
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

    // ---- local exposure bilateral grid: u2 raw, u3 blurred ----
    // gridW/gridH = ceil(scene / kLocalExpTile) (matches post.hlsl GetDimensions).
    localGridW_ = (sceneWidth_ + kLocalExpTile - 1) / kLocalExpTile;
    localGridH_ = (sceneHeight_ + kLocalExpTile - 1) / kLocalExpTile;
    const u64 gridBytes = static_cast<u64>(localGridW_) * localGridH_ * kLocalExpBins * kLocalExpCellBytes;
    auto makeGridBuf = [&](ComPtr<ID3D12Resource>& out, const char* name) {
        auto gd = bufferDesc(gridBytes);
        gd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        // Created COMMON (#1328); no zero-seed needed (CSLocalGrid overwrites every frame).
        return hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &gd,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)), name);
    };
    if (!makeGridBuf(localGridBuf_, "local exposure grid") ||
        !makeGridBuf(localGridBlurBuf_, "local exposure grid blur")) {
        localGridBuf_.Reset();
        localGridBlurBuf_.Reset();
        AVER_WARN("[RHI.D3D12] local exposure's bilateral grid could not be allocated; local exposure stays off until the next resize");
    } else {
        // cmdList_ recording (called from runPostChain mid-frame).
        D3D12_RESOURCE_BARRIER toUav[2] = {
            transition(localGridBuf_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(localGridBlurBuf_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cmdList_->ResourceBarrier(2, toUav);

        uav.ptr += postSrvSize_;
        ud.Buffer.NumElements = static_cast<UINT>(gridBytes / 4);   // raw view: one element per DWORD
        device_->CreateUnorderedAccessView(localGridBuf_.Get(), nullptr, &ud, uav);
        uav.ptr += postSrvSize_;
        device_->CreateUnorderedAccessView(localGridBlurBuf_.Get(), nullptr, &ud, uav);
    }

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

// Scene -> backbuffer. Records chain, leaves backbuffer in RENDER_TARGET. Skip no-op stages.
void D3D12Device::runPostChain(ID3D12Resource* bb, u32 bbIdx, bool generated) {
    // Frame clock advances once per real frame (generated images don't count toward eye adaptation).
    if (!fgGeneratedPost_) {
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
    // Frozen in wireframe view (scene target is only clear colour).
    const bool autoExp = post_.autoExposure && caps_.computeShaders && !wireframeFrame_;
    ID3D12Resource* scene = msaa ? sceneResolved_.Get() : msaaColor_.Get();
    // Scene read by both compute and pixel shaders (CSHistogram/CSLocalGrid + composite).
    const D3D12_RESOURCE_STATES kSceneRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

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
        auto toSrv = transition(scene, D3D12_RESOURCE_STATE_RESOLVE_DEST, kSceneRead);
        cmdList_->ResourceBarrier(1, &toSrv);
    } else {
        auto toSrv = transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, kSceneRead);
        cmdList_->ResourceBarrier(1, &toSrv);
    }

    ID3D12DescriptorHeap* heaps[] = {postSrvHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    boundHeap_ = postSrvHeap_.Get();
    cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
    // Post chain drives command list directly; invalidate caches (table 1 is no longer bound).
    dbValid_ = false;
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
        // Shared with VulkanDevice.cpp: field additions must sync with the other backend.
        cb.misc[3] = static_cast<f32>(post_.tonemap);
        cb.clampRadiance[0] = post_.maxRadiance;
        // y/z: local exposure's shadow/highlight strengths (post.hlsl PSComposite gPostClamp).
        cb.clampRadiance[1] = post_.localExposureShadows;
        cb.clampRadiance[2] = post_.localExposureHighlights;
        cb.clampRadiance[3] = 1.0f - std::exp(-post_.exposureSpeedDark * frameSeconds_);

        // gPostRegion: the docked editor's viewport sub-rect, normalised in post chain source space
        // (sceneWidth_/sceneHeight_). Matching pre-existing behaviour when undocked (vpW_==0).
        if (vpW_ > 0 && vpH_ > 0 && sceneWidth_ > 0 && sceneHeight_ > 0) {
            cb.region[0] = std::fmin(std::fmax(static_cast<f32>(vpX_) / static_cast<f32>(sceneWidth_),  0.0f), 1.0f);
            cb.region[1] = std::fmin(std::fmax(static_cast<f32>(vpY_) / static_cast<f32>(sceneHeight_), 0.0f), 1.0f);
            cb.region[2] = std::fmin(std::fmax(static_cast<f32>(vpW_) / static_cast<f32>(sceneWidth_),  0.0f), 1.0f);
            cb.region[3] = std::fmin(std::fmax(static_cast<f32>(vpH_) / static_cast<f32>(sceneHeight_), 0.0f), 1.0f);
        } else {
            cb.region[0] = 0.0f; cb.region[1] = 0.0f; cb.region[2] = 1.0f; cb.region[3] = 1.0f;
        }

        // gPostEye: perceptual eye-adaptation dials. y is calibration constant (see kLuminanceToCdm2).
        cb.eye[0] = post_.adaptationRealism;
        cb.eye[1] = kLuminanceToCdm2;
        cb.eye[2] = post_.nightVision;
        cb.eye[3] = post_.meteringCenterWeight;
    };

    // vx/vy: destination's top-left offset, default 0 for passes filling whole target. Composite only.
    auto fullscreen = [&](ID3D12PipelineState* pso, u32 triple, u32 w, u32 h,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, u32 vx = 0, u32 vy = 0) {
        cmdList_->SetPipelineState(pso);
        cmdList_->OMSetRenderTargets(1, rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{static_cast<f32>(vx), static_cast<f32>(vy), static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f};
        D3D12_RECT sc{static_cast<LONG>(vx), static_cast<LONG>(vy),
                      static_cast<LONG>(vx + w), static_cast<LONG>(vy + h)};
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
    // Dispatch grid and src dims derive from SCENE target. Metered over sub-rect in docked mode.
    // NOT on generated image: adaptation steps once per real frame.
    if (autoExp && !fgGeneratedPost_) {
        const u32 regionW = vpW_ ? vpW_ : sceneWidth_;
        const u32 regionH = vpH_ ? vpH_ : sceneHeight_;
        const u32 hw = regionW / kHistogramDownscale > 1 ? regionW / kHistogramDownscale : 1;
        const u32 hh = regionH / kHistogramDownscale > 1 ? regionH / kHistogramDownscale : 1;
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
        dbValid_ = false;
    }

    // ---- local exposure ----
    // Independent of autoExp; runs before composite. Skipped when both strengths are 0.
    const bool localExp = (post_.localExposureShadows > 0.0f || post_.localExposureHighlights > 0.0f) &&
                          caps_.computeShaders && localGridBuf_ && localGridBlurBuf_;
    if (localExp) {
        fillCommon(sceneWidth_, sceneHeight_, sceneWidth_, sceneHeight_);

        cmdList_->SetComputeRootSignature(postRootSig_.Get());
        cmdList_->SetPipelineState(localGridPso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        // kPostTripleHistogram: t0/t1/t2 all scene (CSHistogram's same triple).
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch(localGridW_, localGridH_, 1);

        // CSLocalGrid's write must land before CSLocalBlur reads it.
        D3D12_RESOURCE_BARRIER gridUav{};
        gridUav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        gridUav.UAV.pResource = localGridBuf_.Get();
        cmdList_->ResourceBarrier(1, &gridUav);

        cmdList_->SetPipelineState(localBlurPso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch((localGridW_ + 7) / 8, (localGridH_ + 7) / 8, 1);

        // CSLocalBlur's write must land before PSComposite reads it.
        D3D12_RESOURCE_BARRIER blurUav{};
        blurUav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blurUav.UAV.pResource = localGridBlurBuf_.Get();
        cmdList_->ResourceBarrier(1, &blurUav);

        cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
        dbValid_ = false;
    }

    {
        // Metered-exposure readout (IDevice::postExposureReadout): only while CSExposure ran THIS frame.
        // Transitions expBuf_ through COPY_SOURCE and back to keep state balanced on both paths.
        const bool readExp = autoExp && !fgGeneratedPost_ && expReadback_[frameIndex_ < kFrameCount ? frameIndex_ : 0];
        if (readExp) {
            const u32 f = frameIndex_ < kFrameCount ? frameIndex_ : 0;
            auto expToCopy = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmdList_->ResourceBarrier(1, &expToCopy);
            cmdList_->CopyBufferRegion(expReadback_[f].Get(), 0, expBuf_.Get(), 0, 2 * sizeof(u32));
            auto copyToSrv = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &copyToSrv);
            expReadbackPending_[f] = true;
        } else {
            auto expToSrv = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &expToSrv);
        }
    }

    // ---- bloom ----
    if (bloom) {
        bloomTo(0, D3D12_RESOURCE_STATE_RENDER_TARGET);
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

    // AverSR: Pass A, upscale (HDR, before tonemap). Only runs with upscaler set.
    bool srUpscaled = false;
    if (upscaler_ && sceneColorTex_ && presentHdrTex_ && rhiFactory_ && rhiContext_) {
        RhiTexture* srcT = rhiFactory_->texture(sceneColorTex_);
        RhiTexture* dstT = rhiFactory_->texture(presentHdrTex_);
        if (srcT && srcT->res && dstT && dstT->res && dstT->rtvHeap) {
            // Copy to upscaler's input: scene is COPY_SOURCE, not COPY_DEST.
            D3D12_RESOURCE_BARRIER pre[2] = {
                transition(scene, kSceneRead, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(srcT->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cmdList_->ResourceBarrier(2, pre);
            cmdList_->CopyResource(srcT->res.Get(), scene);
            D3D12_RESOURCE_BARRIER post[2] = {
                transition(scene, D3D12_RESOURCE_STATE_COPY_SOURCE, kSceneRead),
                transition(srcT->res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            cmdList_->ResourceBarrier(2, post);

            auto toRt = transition(dstT->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRt);
            D3D12_CPU_DESCRIPTOR_HANDLE srRtv = dstT->rtvHeap->GetCPUDescriptorHandleForHeapStart();
            cmdList_->OMSetRenderTargets(1, &srRtv, FALSE, nullptr);
            // Caller binds target and sets viewport/scissor to destination size; implementation records
            // only its own pipeline and draw, and transitions nothing.
            D3D12_VIEWPORT srVp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
            D3D12_RECT srSc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
            cmdList_->RSSetViewports(1, &srVp);
            cmdList_->RSSetScissorRects(1, &srSc);

            UpscalerInput in{};
            in.color = sceneColorTex_;
            in.srcWidth = sceneWidth_;   in.srcHeight = sceneHeight_;
            in.dstWidth = width_;        in.dstHeight = height_;
            in.jitterX = taaJitterThisFrame_[0];
            in.jitterY = taaJitterThisFrame_[1];
            in.generated = generated;
            in.cameraMoving = taaCameraMoving_;
            // A temporal upscaler reads this frame's G-buffer velocity and view Z, NeuRAA also the
            // normal; they rest as render targets, so they are made readable around execute() and put back.
            const UpscalerNeeds srNeeds = upscaler_->needs();
            const bool gbufForSr = (any(srNeeds, UpscalerNeeds::MotionVectors) || any(srNeeds, UpscalerNeeds::Normal)) &&
                                   gBufferWritten() && gbufWrittenFrame_ && gbufVelocity_ && gbufViewZ_ && gbufNormalRough_;
            if (gbufForSr) {
                D3D12_RESOURCE_BARRIER b[3] = {
                    transition(gbufVelocity_.Get(),    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    transition(gbufViewZ_.Get(),       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    transition(gbufNormalRough_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                };
                cmdList_->ResourceBarrier(3, b);
                in.motionVectors   = gBufferVelocityTexture();
                in.depth           = gBufferViewZTexture();
                in.normalRoughness = gBufferNormalRoughnessTexture();
            }
            if (any(srNeeds, UpscalerNeeds::PrimaryVisibility) && !generated)
                for (const IRenderFeature* f : features_)
                    if (f->primaryVisibility(in.visibility)) break;
            upscaler_->execute(*rhiContext_, in, presentHdrTex_);
            if (gbufForSr) {
                D3D12_RESOURCE_BARRIER b[3] = {
                    transition(gbufVelocity_.Get(),    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    transition(gbufViewZ_.Get(),       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    transition(gbufNormalRough_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                };
                cmdList_->ResourceBarrier(3, b);
            }

            // Looked up again: execute() may create textures (an upscaler's history on first use), and
            // a grown texture table moves, leaving the earlier dstT dangling (a NULL barrier, device lost).
            dstT = rhiFactory_->texture(presentHdrTex_);
            if (dstT && dstT->res) {
                auto backToSrv = transition(dstT->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cmdList_->ResourceBarrier(1, &backToSrv);
            }

            // Restore post chain's descriptor heap and root signature.
            ID3D12DescriptorHeap* heaps[] = {postSrvHeap_.Get()};
            cmdList_->SetDescriptorHeaps(1, heaps);
            cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
            dbValid_ = false;
            srUpscaled = true;

            // Log once, reporting what actually happened.
            if (!srLogged_) {
                srLogged_ = true;
                AVER_INFO("[AverSR] '{}' upscaling {}x{} -> {}x{} in HDR, before the tonemap",
                          upscaler_->name(), sceneWidth_, sceneHeight_, width_, height_);
            }
        }
    }

    // ---- composite ----
    // Dst is present-space; src is scene target (1:1 at renderScale_==1.0).
    // ON AverSR PATH, t0 is already present-sized; local exposure reads scene's grid size.
    fillCommon(width_, height_, sceneWidth_, sceneHeight_);
    const u32 compositeTriple = srUpscaled ? kPostTripleCompositeUpscaled : kPostTripleComposite;

    // Composite confined to gPostRegion's docked sub-rect scaled into PRESENT-space.
    // Full canvas at (0,0,1,1) identity when undocked, matching pre-existing behaviour.
    u32 compositeX = static_cast<u32>(cb.region[0] * static_cast<f32>(width_)  + 0.5f);
    u32 compositeY = static_cast<u32>(cb.region[1] * static_cast<f32>(height_) + 0.5f);
    u32 compositeW = static_cast<u32>(cb.region[2] * static_cast<f32>(width_)  + 0.5f);
    u32 compositeH = static_cast<u32>(cb.region[3] * static_cast<f32>(height_) + 0.5f);
    if (compositeX > width_)  compositeX = width_;
    if (compositeY > height_) compositeY = height_;
    if (compositeW < 1) compositeW = 1;
    if (compositeH < 1) compositeH = 1;
    if (compositeX + compositeW > width_)  compositeW = width_  - compositeX;
    if (compositeY + compositeH > height_) compositeH = height_ - compositeY;
    {
        auto toRt = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += static_cast<SIZE_T>(bbIdx) * rtvSize_;

        const RhiTexture* vt = (viewportToTex_ && ensureViewportTexture() && rhiFactory_)
                             ? rhiFactory_->texture(viewportTex_) : nullptr;
        if (vt && vt->rtvHeap) {
            const f32 blank[4] = {clear_[0], clear_[1], clear_[2], 1.0f};
            cmdList_->ClearRenderTargetView(bbRtv, blank, 0, nullptr);

            auto toRtTex = transition(vt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRtTex);
            // Outside sub-rect, vt keeps prior content. Confined viewport/scissor means DrawInstanced
            // never rasterizes there. Only SandboxShell's Level ImGui::Image reads vt, with same crop.
            D3D12_CPU_DESCRIPTOR_HANDLE trtv = vt->rtvHeap->GetCPUDescriptorHandleForHeapStart();
            fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0].Get(), compositeTriple,
                       compositeW, compositeH, &trtv, compositeX, compositeY);
            auto backToSrv = transition(vt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);
            cmdList_->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        } else {
            fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0].Get(), compositeTriple,
                       compositeW, compositeH, &bbRtv, compositeX, compositeY);
        }
    }

    // ---- restore ----
    {
        auto expBack = transition(expBuf_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList_->ResourceBarrier(1, &expBack);
    }
    auto sceneBack = msaa
        ? transition(scene, kSceneRead, D3D12_RESOURCE_STATE_RESOLVE_DEST)
        : transition(scene, kSceneRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList_->ResourceBarrier(1, &sceneBack);
    if (msaa) {
        auto msaaBack = transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &msaaBack);
    }
}

// Closes the frame: post chain, overlay, UI, capture, then submit.
void D3D12Device::endFrame() {
    if (!hasSwapchain_) return;
    // LATE SCENE (wantsLateScenePass): the scene's targets, viewport and root are rebound, since draw
    // submission may have changed them, and the winner records inside the still-open "scene draw" span.
    if (IRenderFeature* late = lateSceneWinner_) {
        lateSceneWinner_ = nullptr;
        if (recording_ && rhiContext_) {
            cmdList_->OMSetRenderTargets(sceneRtvCount_, sceneRtvs_, FALSE, &sceneDsv_);
            cmdList_->RSSetViewports(1, &sceneVp_);
            cmdList_->RSSetScissorRects(1, &sceneSc_);
            boundRootSig_ = nullptr;
            boundPso_ = nullptr;
            fovValid_ = false;
            dbValid_ = false;
            bindGraphicsRoot(rootSig_.Get());
            cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            late->scenePass(*rhiContext_);
            cmdList_->SetPipelineState(pso_.Get());
            boundPso_ = pso_.Get();
        }
    }
    // Closes scene draw, opens one covering post chain, composite, editor viewport, overlay, ImGui.
    endGpuSpan();
    beginGpuSpan("sky+post+ui");
    fovValid_ = false;
    dbValid_ = false;

    // Scene-space rect (and fallback to scene target, not present) shared by transparent pass and sky.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    D3D12_VIEWPORT sceneVp{rx, ry, rw, rh, 0.0f, 1.0f};
    D3D12_RECT sceneSc{static_cast<LONG>(rx), static_cast<LONG>(ry), static_cast<LONG>(rx + rw), static_cast<LONG>(ry + rh)};

    // Sky-then-transparency: sky fills clear-depth pixels (EQUAL test), particles fail occluder test.
    // Unoccluded particles now blend onto sky's real colour instead of clear value.
    // Gated on frameSuppressed_, not sceneSuppressed_: ray-driven fills missed pixels with depth 1.0.
    if (skyEnabled_ && !frameSuppressed_ && !wireframeFrame_) {
        beginGpuSpan("sky dome");
        cmdList_->RSSetViewports(1, &sceneVp);
        cmdList_->RSSetScissorRects(1, &sceneSc);
        bindGraphicsRoot(rootSig_.Get());
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList_->SetPipelineState(skyPso_.Get());
        boundPso_ = skyPso_.Get();
        cmdList_->IASetVertexBuffers(0, 0, nullptr);
        cmdList_->DrawInstanced(3, 1, 0, 0);
        endGpuSpan();
    }

    // Blended mesh flush: every drawMesh(..., drawBlended_=true) was captured instead of drawn.
    // Must land between sky and transparent pass. Guarded on frameSuppressed_, not sceneSuppressed_.
    // A scene feature that composited translucency in its own pass owns those draws this frame.
    bool blendedInScene = false;
    if (sceneSuppressed_) for (IRenderFeature* f : features_) blendedInScene = blendedInScene || f->blendedDrawsResolvedInScene();
    if (!blendedDraws_.empty() && rhiContext_ && !frameSuppressed_ && !blendedInScene) {
        // Backdrop copy only for draws that read it (IRenderFeature::blendedDrawReadsBackdrop).
        // Captures the target AT THAT POINT, not just "before first translucent draw".
        // Size check is load-bearing: mismatch between backdrop and scene target can briefly occur.
        auto resolveBlendBackdrop = [&]() {
            if (blendBackdropTex_ && rhiFactory_ && msaaColor_ &&
                blendBackdropW_ == sceneWidth_ && blendBackdropH_ == sceneHeight_) {
                if (RhiTexture* bt = rhiFactory_->texture(blendBackdropTex_)) {
                    if (bt->res) {
                        const bool ms = sampleCount_ > 1;
                        const D3D12_RESOURCE_STATES srcTo = ms ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                                               : D3D12_RESOURCE_STATE_COPY_SOURCE;
                        const D3D12_RESOURCE_STATES dstTo = ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                                                               : D3D12_RESOURCE_STATE_COPY_DEST;
                        D3D12_RESOURCE_BARRIER pre[2] = {
                            transition(msaaColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, srcTo),
                            transition(bt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, dstTo),
                        };
                        cmdList_->ResourceBarrier(2, pre);
                        if (ms) cmdList_->ResolveSubresource(bt->res.Get(), 0, msaaColor_.Get(), 0,
                                                             kSceneColorFormat);
                        else    cmdList_->CopyResource(bt->res.Get(), msaaColor_.Get());
                        D3D12_RESOURCE_BARRIER post[2] = {
                            transition(msaaColor_.Get(), srcTo, D3D12_RESOURCE_STATE_RENDER_TARGET),
                            transition(bt->res.Get(), dstTo, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        };
                        cmdList_->ResourceBarrier(2, post);
                        if (!blendBackdropCopyLogged_) {
                            blendBackdropCopyLogged_ = true;
                            AVER_INFO("[RHI.D3D12] blended backdrop: {} {}x{} from the scene target",
                                      ms ? "resolved" : "copied", blendBackdropW_, blendBackdropH_);
                        }
                    }
                }
            }
        };

        u32 blendLayerResolves = 0;
        bool blendBackdropCaptured = false;
        u32 blendBackdropCaptures = 0;
        u32 blendDrawsDone = 0;
        bool blendCapReached = false;

        beginGpuSpan("blended replay");
        IRenderFeature* owner = nullptr;
        for (IRenderFeature* f : features_) {
            if (f->overridesScenePipeline()) { owner = f; break; }
        }
        const PipelineHandle blendedPso = owner
            ? owner->scenePipeline(msActive_ && msPso_, false, true) : 0;

        if (!blendedPso) {
            // Returning 0 for blended=true means no blended variant. Drop rather than fall back to opaque.
            if (!blendedPipelineMissingWarned_) {
                AVER_WARN("[RHI.D3D12] {} blended draw(s) this frame but no feature offers "
                          "scenePipeline(..., blended=true); dropping them rather than drawing them "
                          "opaque (said once per frame)", blendedDraws_.size());
                blendedPipelineMissingWarned_ = true;
            }
        } else {
            // Back-to-front sort: furthest first. Distance = camera translation only (world[12,13,14]).
            const f32 cx = frameCB_.camPos[0], cy = frameCB_.camPos[1], cz = frameCB_.camPos[2];
            std::sort(blendedDraws_.begin(), blendedDraws_.end(),
                      [cx, cy, cz](const BlendedDraw& a, const BlendedDraw& b) {
                const f32 adx = a.world[12] - cx, ady = a.world[13] - cy, adz = a.world[14] - cz;
                const f32 bdx = b.world[12] - cx, bdy = b.world[13] - cy, bdz = b.world[14] - cz;
                // Squared distance (monotonic with real distance, avoids sqrt cost).
                return (adx * adx + ady * ady + adz * adz) > (bdx * bdx + bdy * bdy + bdz * bdz);
            });

            cmdList_->RSSetViewports(1, &sceneVp);
            cmdList_->RSSetScissorRects(1, &sceneSc);
            bindGraphicsRoot(rootSig_.Get());

            // FOV cache reuse: already false entering this block; force-invalidated after loop.
            for (const BlendedDraw& bd : blendedDraws_) {
                // Stale handle: capture can outlive its mesh within the same frame.
                if (bd.mesh == 0 || bd.mesh > meshes_.size() || !meshes_[bd.mesh - 1].alive) continue;
                ++blendDrawsDone;

                // Only draws that sample backdrop need it captured.
                if (owner->blendedDrawReadsBackdrop(bd.binding.constants, bd.binding.bytes)) {
                    if (!blendBackdropCaptured) {
                        resolveBlendBackdrop();
                        ++blendBackdropCaptures;
                        blendBackdropCaptured = true;
                    } else if (blendLayerResolves < kMaxBlendLayerResolves) {
                        // Re-capture: each surface sees every layer drawn since last capture.
                        ++blendLayerResolves;
                        resolveBlendBackdrop();
                        ++blendBackdropCaptures;
                    } else {
                        blendCapReached = true;
                        if (!blendLayerCapWarned_) {
                            blendLayerCapWarned_ = true;
                            AVER_WARN("[RHI.D3D12] more than {} overlapping translucent layers this "
                                      "frame; the ones beyond that composite against a backdrop missing "
                                      "the nearer surfaces, as every layer did before per-layer capture "
                                      "existed (said once)", kMaxBlendLayerResolves);
                        }
                    }
                }

                const BindingSetHandle bs = owner->sceneBindingSet();
                const void* cb = nullptr; u32 cbBytes = 0;
                const bool haveCb = owner->sceneConstants(&cb, &cbBytes) && cb && cbBytes;
                const bool same = fovValid_ && blendedPso == fovPso_ && bs == fovSet_ &&
                                  haveCb == (fovCbBytes_ != 0) &&
                                  (!haveCb || (cbBytes == fovCbBytes_ && fovCb_.size() == cbBytes &&
                                               std::memcmp(fovCb_.data(), cb, cbBytes) == 0));
                if (!same) {
                    rhiContext_->setPipeline(blendedPso);
                    rhiContext_->setBindlessTable(owner->sceneBindlessTable());
                    if (bs) rhiContext_->setBindingSet(bs, 0);
                    if (haveCb) rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
                    fovPso_ = blendedPso; fovSet_ = bs; fovCbBytes_ = haveCb ? cbBytes : 0;
                    if (haveCb) fovCb_.assign(static_cast<const u8*>(cb), static_cast<const u8*>(cb) + cbBytes);
                    else        fovCb_.clear();
                    fovValid_ = true;
                }
                rhiContext_->setDrawBinding(bd.binding.set, bd.binding.constants, bd.binding.bytes);
                f32 fc[kObjectConstantDwords];
                std::memcpy(fc, bd.world, 16 * sizeof(f32));
                std::memcpy(fc + 16, bd.color, 4 * sizeof(f32));
                // fc[22] stays 0: unlit blended mesh not plumbed speculatively.
                fc[20] = bd.metallic; fc[21] = bd.roughness; fc[22] = 0.0f; fc[23] = 0.0f;
                writeShadingConstants(fc);
                rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
                if (msActive_ && msPso_) rhiContext_->dispatchMeshFor(bd.mesh);
                else                     rhiContext_->drawMesh(bd.mesh);
                boundRootSig_ = nullptr;
                boundPso_ = nullptr;
            }
            fovValid_ = false;
            dbValid_ = false;
        }

        // Logged on CHANGE, not every frame.
        if (blendDrawsDone != blendStatDrawsLogged_ || blendLayerResolves != blendStatResolvesLogged_ ||
            blendBackdropCaptures != blendStatCapturesLogged_) {
            ++blendStatChanges_;
            if ((blendStatChanges_ & (blendStatChanges_ - 1)) == 0) {
                AVER_INFO("[RHI.D3D12] blended replay: {} translucent draw(s), {} layer re-capture(s) "
                          "(cap {}){}, {} backdrop capture(s)",
                          blendDrawsDone, blendLayerResolves, kMaxBlendLayerResolves,
                          blendCapReached ? ", cap reached" : "", blendBackdropCaptures);
            }
            blendStatDrawsLogged_ = blendDrawsDone;
            blendStatResolvesLogged_ = blendLayerResolves;
            blendStatCapturesLogged_ = blendBackdropCaptures;
        }
        endGpuSpan();
    }

    // IRenderFeature::transparentPass: after opaque drawMesh and sky. Depth test on, depth write OFF.
    // Root signature/viewport re-set explicitly. Gated on frameSuppressed_: ray-driven frame's
    // particle is as real as a rastered one. Not in wireframe view.
    if (rhiContext_ && !frameSuppressed_ && !wireframeFrame_) {
        cmdList_->RSSetViewports(1, &sceneVp);
        cmdList_->RSSetScissorRects(1, &sceneSc);
        bindGraphicsRoot(rootSig_.Get());
        for (IRenderFeature* f : features_) f->transparentPass(*rhiContext_);
    }

    // MSAA: readers get the single-sample G-buffer from here on (post chain, next frame's denoiser).
    if (gbufWrittenFrame_ && sampleCount_ > 1 && !resolveGBufferMsaa()) gbufHistoryInvalid_ = true;

    // ---- frame interpolation (docs/rendering/NEURAFI.md) ----
    // Generated image from HDR scene target presented FIRST on its own swapchain image, through
    // post chain, overlays, and UI; real frame follows on next image.
    frameInterpolated_ = false;
    TextureHandle generatedImage = 0;
    if (frameInterpOn_ && frameInterp_) {
        const bool jumped = frameInterpCameraJumped();
        if (frameInterpBlocker() == 0) {
            // Scene target is msaaColor_ (sampleCount_==1 here), in RENDER_TARGET.
            ID3D12Resource* scene = msaaColor_.Get();
            if (fgInputTex_ && (fgInputW_ != sceneWidth_ || fgInputH_ != sceneHeight_)) {
                rhiFactory_->destroyTexture(fgInputTex_);
                fgInputTex_ = 0;
            }
            if (!fgInputTex_) {
                TextureDesc td{};
                td.width = sceneWidth_;
                td.height = sceneHeight_;
                td.format = Format::RGBA16F;
                td.bind = ResourceBind::ShaderResource;
                td.initialState = ResourceState::ShaderResource;
                td.debugName = "Frame interpolation input (scene colour)";
                fgInputTex_ = rhiFactory_->createTexture(td);
                fgInputW_ = sceneWidth_;
                fgInputH_ = sceneHeight_;
            }
            RhiTexture* it = fgInputTex_ ? rhiFactory_->texture(fgInputTex_) : nullptr;
            if (it && it->res && scene) {
                D3D12_RESOURCE_BARRIER pre[2] = {
                    transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition(it->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                };
                cmdList_->ResourceBarrier(2, pre);
                cmdList_->CopyResource(it->res.Get(), scene);
                D3D12_RESOURCE_BARRIER post[2] = {
                    transition(scene, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    transition(it->res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                };
                cmdList_->ResourceBarrier(2, post);

                FrameInterpInput in{};
                in.color = fgInputTex_;
                in.velocity = gBufferVelocityTexture();
                in.viewZ = gBufferViewZTexture();
                in.width = sceneWidth_;
                in.height = sceneHeight_;
                in.sceneCut = frameInterpCut_ || jumped || gbufHistoryInvalid_;
                generatedImage = frameInterp_->generate(*rhiContext_, in);
                frameInterpCut_ = false;
                // Generator bound its own pipelines and sets.
                boundRootSig_ = nullptr; boundPso_ = nullptr; boundHeap_ = nullptr;
                fovValid_ = false; dbValid_ = false;
            }
        } else {
            frameInterpCut_ = true;
        }
    }

    // Move `src` (factory texture at PIXEL_SHADER_RESOURCE) into scene target the post chain reads.
    auto toScene = [&](TextureHandle src) {
        RhiTexture* st = rhiFactory_->texture(src);
        ID3D12Resource* scene = msaaColor_.Get();
        D3D12_RESOURCE_BARRIER pre[2] = {
            transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(st->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cmdList_->ResourceBarrier(2, pre);
        cmdList_->CopyResource(scene, st->res.Get());
        D3D12_RESOURCE_BARRIER post[2] = {
            transition(scene, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(st->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmdList_->ResourceBarrier(2, post);
    };
    RhiTexture* genT = generatedImage ? rhiFactory_->texture(generatedImage) : nullptr;
    if (genT && genT->res) {
        toScene(generatedImage);
        presentPass(bbIndex_, true, true, false);
        // Generated image goes to present thread ahead of real frame's post chain and UI.
        if (!submitGeneratedImage()) return;
        toScene(fgShowGeneratedOnly_ ? generatedImage : fgInputTex_);
        realImage_ = (bbIndex_ + 1) % kBackBufferCount;
        presentPass(realImage_, false, false, true);
        frameInterpolated_ = true;
    } else {
        realImage_ = bbIndex_;
        presentPass(bbIndex_, false, true, true);
    }
    imageNext_ = (realImage_ + 1) % kBackBufferCount;

    // Frame-end timestamp, then resolve every stamp issued this frame.
    endGpuSpan();   // "sky+post+ui"
    tsSliceEnd_[frameIndex_] = gpuStamp();
    if (tsEnabled_ && tsCount_ > 0)
        cmdList_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                   frameIndex_ * kMaxGpuStamps, tsCount_, tsReadback_.Get(),
                                   static_cast<u64>(frameIndex_) * kMaxGpuStamps * sizeof(u64));

    // Device lost: do not submit; beginFrame returns early when device is gone.
    if (deviceLost_) return;
    cmdList_->Close();
    recording_ = false;
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    waitImageFree(realImage_);
    queue_->ExecuteCommandLists(1, lists);
    if (infoQueue_) drainDebugMessages();
}

// Closes and submits the frame (scene, generator, post, overlays, UI), queues the generated image,
// and reopens the command list. Returns false if the device was lost.
bool D3D12Device::submitGeneratedImage() {
    if (deviceLost_) return false;
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    waitImageFree(bbIndex_);
    queue_->ExecuteCommandLists(1, lists);
    if (pendingReal_) {
        pendingReal_ = false;
        if (!queuePresent(pendingRealImage_, pendingRealSync_, pendingRealFlags_)) return false;
    }
    if (!queuePresent(bbIndex_, presentSync(), presentFlags())) return false;
    allocatorsGen_[frameIndex_]->Reset();
    cmdList_->Reset(allocatorsGen_[frameIndex_].Get(), nullptr);
    boundRootSig_ = nullptr; boundPso_ = nullptr; boundHeap_ = nullptr;
    fovValid_ = false; dbValid_ = false;
    return true;
}

// Submits recorded work and queues the pending real image. No-op if no pending real or not recording.
void D3D12Device::frameMidpoint() {
    if (!pendingReal_ || !recording_ || deviceLost_) return;
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    pendingReal_ = false;
    queuePresent(pendingRealImage_, pendingRealSync_, pendingRealFlags_);
    allocatorsMid_[frameIndex_]->Reset();
    cmdList_->Reset(allocatorsMid_[frameIndex_].Get(), nullptr);
    boundRootSig_ = nullptr; boundPso_ = nullptr; boundHeap_ = nullptr;
    fovValid_ = false; dbValid_ = false;
    // Reset list; scene pass expects these targets, viewport and topology.
    cmdList_->OMSetRenderTargets(sceneRtvCount_, sceneRtvs_, FALSE, &sceneDsv_);
    cmdList_->RSSetViewports(1, &sceneVp_);
    cmdList_->RSSetScissorRects(1, &sceneSc_);
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// The display refresh rate from the display mode. Returns 0 if unavailable.
void D3D12Device::queryDisplayRefresh() {
    fgDisplayHz_ = 0.0f;
    if (!swapChain_) return;
    ComPtr<IDXGIOutput> out;
    if (FAILED(swapChain_->GetContainingOutput(&out)) || !out) return;
    DXGI_OUTPUT_DESC od{};
    if (FAILED(out->GetDesc(&od))) return;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        fgDisplayHz_ = static_cast<f32>(dm.dmDisplayFrequency);
}

// Why frame interpolation cannot run this frame; 0 when it can.
u32 D3D12Device::frameInterpBlocker() {
    u32 why = 0;
    const char* text = nullptr;
    if (!rhiContext_ || !rhiFactory_) {
        why = 1; text = "the device has no generic RHI context";
    } else if (sampleCount_ > 1) {
        why = 3; text = "MSAA is on; it needs the G-buffer, which is written only at 1x anti-aliasing";
    } else if (!gbufferEnabled_ || !gbufVelocity_ || !gbufViewZ_) {
        why = 4; text = "the G-buffer (motion and depth) is not enabled";
    } else if (wireframeFrame_ || frameSuppressed_) {
        why = 5;   // per-frame view states: not worth a log line
    }
    if (why != frameInterpOffReason_) {
        if (text) AVER_INFO("[RHI.D3D12] frame interpolation paused: {}", text);
        else if (why == 0) AVER_INFO("[RHI.D3D12] frame interpolation running");
        frameInterpOffReason_ = why;
    }
    return why;
}

// A camera jump the generator must not interpolate across: teleport, cut, or snap turn.
bool D3D12Device::frameInterpCameraJumped() {
    constexpr f32 kJumpDistance = 250.0f;   // world units (cm) in one frame: 150 m/s at 60 fps
    constexpr f32 kJumpCos = 0.866f;        // 30 degrees of turn in one frame
    const f32* m = frameCB_.invViewProjRel;
    auto unproject = [&](f32 z, f32 out[3]) {
        const f32 w = m[11] * z + m[15];
        const f32 iw = std::fabs(w) > 1e-20f ? 1.0f / w : 0.0f;
        for (int k = 0; k < 3; ++k) out[k] = (m[8 + k] * z + m[12 + k]) * iw;
    };
    f32 a[3], b[3];
    unproject(0.0f, a);
    unproject(1.0f, b);
    f32 fwd[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const f32 len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!(len > 1e-12f)) return false;   // no camera yet
    for (f32& c : fwd) c /= len;
    const f32* p = frameCB_.camPos;
    bool jumped = false;
    if (fgCamPrimed_) {
        const f32 dx = p[0] - fgPrevCamPos_[0], dy = p[1] - fgPrevCamPos_[1], dz = p[2] - fgPrevCamPos_[2];
        const f32 turn = fwd[0] * fgPrevCamFwd_[0] + fwd[1] * fgPrevCamFwd_[1] + fwd[2] * fgPrevCamFwd_[2];
        jumped = dx * dx + dy * dy + dz * dz > kJumpDistance * kJumpDistance || turn < kJumpCos;
    }
    for (int k = 0; k < 3; ++k) { fgPrevCamPos_[k] = p[k]; fgPrevCamFwd_[k] = fwd[k]; }
    fgCamPrimed_ = true;
    return jumped;
}

// Copies the viewport rect of present image `bbIdx` (or of the viewport texture, where the editor
// composites it) into mirrorImages_[bbIdx], at the mirror's origin.
void D3D12Device::copyToMirror(u32 bbIdx) {
    ID3D12Resource* dst = mirrorSwap_ ? mirrorImages_[bbIdx].Get() : nullptr;
    if (!dst) return;
    const RhiTexture* vt = (viewportToTex_ && viewportTex_ && rhiFactory_) ? rhiFactory_->texture(viewportTex_) : nullptr;
    const bool fromTexture = vt && vt->res;
    ID3D12Resource* src = fromTexture ? vt->res.Get() : renderTargets_[bbIdx].Get();
    const D3D12_RESOURCE_STATES srcState = fromTexture ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                       : D3D12_RESOURCE_STATE_RENDER_TARGET;
    // vpX_.. are scene pixels; the image is in present pixels.
    const f32 sx = sceneWidth_  ? static_cast<f32>(width_)  / static_cast<f32>(sceneWidth_)  : 1.0f;
    const f32 sy = sceneHeight_ ? static_cast<f32>(height_) / static_cast<f32>(sceneHeight_) : 1.0f;
    u32 x = vpW_ ? static_cast<u32>(static_cast<f32>(vpX_) * sx + 0.5f) : 0u;
    u32 y = vpW_ ? static_cast<u32>(static_cast<f32>(vpY_) * sy + 0.5f) : 0u;
    u32 w = vpW_ ? static_cast<u32>(static_cast<f32>(vpW_) * sx + 0.5f) : width_;
    u32 h = vpW_ ? static_cast<u32>(static_cast<f32>(vpH_) * sy + 0.5f) : height_;
    if (x >= width_ || y >= height_) return;
    w = std::min({w, mirrorW_, width_ - x});
    h = std::min({h, mirrorH_, height_ - y});
    if (w == 0 || h == 0) return;

    D3D12_RESOURCE_BARRIER pre[2] = {
        transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmdList_->ResourceBarrier(2, pre);
    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{x, y, 0, x + w, y + h, 1};
    cmdList_->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
    D3D12_RESOURCE_BARRIER post[2] = {
        transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState),
        transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
    };
    cmdList_->ResourceBarrier(2, post);
}

// One presented image: post chain, editor lines and overlay features, UI, then PRESENT.
void D3D12Device::presentPass(u32 bbIdx, bool generated, bool firstOfFrame, bool lastOfFrame) {
    ID3D12Resource* bb = renderTargets_[bbIdx].Get();
    beginGpuSpan(generated ? "post chain (generated)" : "post chain");
    fgGeneratedPost_ = generated;
    runPostChain(bb, bbIdx, generated);
    fgGeneratedPost_ = false;
    endGpuSpan();   // "post chain"

    // ---- editor lines + overlay features ----
    beginGpuSpan("overlay");
    // The 3D view's rect in scene pixels.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    if (rhiContext_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(bbIdx) * rtvSize_;

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

        // Make scene depth readable for editorLines and overlayPass, then flip it back.
        const TextureHandle sceneDepth = sceneDepthTexture();
        if (sceneDepth) rhiContext_->textureBarrier(sceneDepth, ResourceState::DepthWrite, ResourceState::ShaderResource,
                                                    kAllSubresources);

        // Convert 3D view rect from scene pixels to present pixels.
        const f32 toDispX = sceneWidth_  ? static_cast<f32>(width_)  / static_cast<f32>(sceneWidth_)  : 1.0f;
        const f32 toDispY = sceneHeight_ ? static_cast<f32>(height_) / static_cast<f32>(sceneHeight_) : 1.0f;
        const f32 displayRect[4] = {rx * toDispX, ry * toDispY, rw * toDispX, rh * toDispY};
        editorLines_.replay(*rhiContext_, width_, height_, displayRect, sceneDepth, sampleCount_,
                            fromDxgiFormat(kBackbufferFormat), firstOfFrame, lastOfFrame);
        // replay() bypassed our caches; invalidate them.
        boundRootSig_ = nullptr; boundPso_ = nullptr; boundHeap_ = nullptr;
        fovValid_ = false; dbValid_ = false;

        for (IRenderFeature* f : features_) f->overlayPass(*rhiContext_, width_, height_);

        if (sceneDepth) rhiContext_->textureBarrier(sceneDepth, ResourceState::ShaderResource, ResourceState::DepthWrite,
                                                    kAllSubresources);

        if (intoTexture) {
            auto backToSrv = transition(ovt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &backToSrv);
            cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
    } else {
        // No generic context; discard editor-chrome draws to avoid leaking them to the next frame.
        if (lastOfFrame) editorLines_.discardQueue();
    }

    // UI backend draw, before capture.
    endGpuSpan();   // "overlay"
    copyToMirror(bbIdx);   // the game view without the editor UI
    if (uiActive_ && uiBackend_) {
        beginGpuSpan("editor UI");
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(bbIdx) * rtvSize_;
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        boundHeap_ = uiBackend_->render(cmdList_.Get());
        endGpuSpan();   // "editor UI"
    }
    // Captures take the real image, or generated if setFrameInterpCaptureGenerated is on.
    if (captureReq_ && captureBuf_ && (fgCaptureGenerated_ ? generated : lastOfFrame)) {
        captureRecorded_ = true;
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
}

// Presents the frame, signals its fence, and services a pending capture.
void D3D12Device::present() {
    if (!hasSwapchain_ || deviceLost_) return;
    // Simulated device loss, checked before Present for reproducibility.
    if (const u32 loseAt = simulatedDeviceLoss(); loseAt != 0 && ++presentedFrames_ >= loseAt) {
        noteDeviceRemoved("--device-lost-at (SIMULATED, the hardware is fine)", DXGI_ERROR_DEVICE_HUNG);
        return;
    }
    // Queue real image (held back for midpoint with frame interpolation, or presented now).
    if (pendingReal_) {
        pendingReal_ = false;
        if (!queuePresent(pendingRealImage_, pendingRealSync_, pendingRealFlags_)) return;
    }
    if (frameInterpolated_) {
        pendingReal_ = true;
        pendingRealImage_ = realImage_;
        pendingRealSync_ = presentSync();
        pendingRealFlags_ = presentFlags();
    } else if (!queuePresent(realImage_, presentSync(), presentFlags())) {
        return;
    }
    if (tsEnabled_ && ++waitFrames_ >= kWaitReport) {
        AVER_INFO("[RHI.D3D12] per frame (avg of {}): render thread blocked at the start-of-frame fence "
                  "{:.2f} ms and on a full present queue {:.2f} ms; the present thread spent {:.2f} ms in Present "
                  "and presented {:.2f} images", waitFrames_, waitFenceMs_ / waitFrames_, waitPresentMs_ / waitFrames_,
                  static_cast<f64>(presentBlockedUs_.exchange(0)) / 1000.0 / waitFrames_,
                  static_cast<f64>(presentCount_.exchange(0)) / waitFrames_);
        waitFenceMs_ = waitPresentMs_ = 0.0;
        waitFrames_ = 0;
    }

    // Signal only if the queue accepts it; never advance the fence value on failure.
    if (FAILED(queue_->Signal(fence_.Get(), nextFence_ + 1))) {
        noteDeviceRemoved("the present fence signal", DXGI_ERROR_DEVICE_REMOVED);
        return;
    }
    ++nextFence_;
    fenceValues_[frameIndex_] = nextFence_;
    // What this frame retired while it recorded is released once this frame is done.
    if (rhiFactory_) rhiFactory_->resolvePendingRetires(nextFence_);

    // Service a pending capture if this frame recorded the copy.
    if (captureReq_ && captureBuf_ && captureRecorded_) {
        captureRecorded_ = false;
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
    // Drain queued presents before releasing images.
    drainPresents();
    pendingReal_ = false;   // held-back real image is not shown across a resize
    for (auto& rt : renderTargets_) rt.Reset();
    for (auto& sb : swapBuffers_) sb.Reset();
    depthBuffer_.Reset();
    msaaColor_.Reset();
    // Reset alongside depthBuffer_/msaaColor_ regardless of which branch runs below.
    if (gbufferEnabled_) { gbufVelocity_.Reset(); gbufViewZ_.Reset(); gbufNormalRough_.Reset(); }
    const UINT scFlags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    frameInterpCut_ = true;   // never interpolate across a resize
    queryDisplayRefresh();    // the window may have moved to another display
    if (!hrOk(swapChain_->ResizeBuffers(kSwapBufferCount, w, h, kBackbufferFormat, scFlags), "ResizeBuffers")) {
        createRenderTargetViews();
        createDepthBuffer();
        createMsaaColor();
        if (gbufferEnabled_ && !createGBufferTargets())
            AVER_ERROR("[RHI.D3D12] G-buffer target creation failed after a rejected resize");
        return;
    }
    width_ = w; height_ = h;
    computeSceneSize();
    vpX_ = vpY_ = vpW_ = vpH_ = 0;
    for (u32 n = 0; n < kFrameCount; ++n) fenceValues_[n] = 0;
    createRenderTargetViews();
    createDepthBuffer();
    createMsaaColor();
    // Create G-buffer targets only when enabled (see rebuildSceneTargets).
    if (gbufferEnabled_ && !createGBufferTargets())
        AVER_ERROR("[RHI.D3D12] G-buffer target creation failed for the resized {}x{} scene", sceneWidth_, sceneHeight_);
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
// Records the device removal once with the reason decoded.
bool D3D12Device::noteDeviceRemoved(const char* where, HRESULT hr) {
    if (deviceLost_) return true;
    deviceLost_ = true;
    HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
    // Simulated loss: fall back to the caller's reason since GetDeviceRemovedReason returns S_OK.
    if (SUCCEEDED(reason)) reason = hr;
    const char* what = "unknown";
    switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:      what = "the GPU stopped responding to a command this engine submitted (TDR)"; break;
    case DXGI_ERROR_DEVICE_RESET:     what = "the driver reset the device, usually after another application hung it"; break;
    case DXGI_ERROR_DEVICE_REMOVED:   what = "the adapter was physically removed, or its driver was updated or restarted"; break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: what = "the driver failed internally"; break;
    case DXGI_ERROR_INVALID_CALL:     what = "the runtime rejected a call outright -- run with --debug-layer, which names it"; break;
    default: break;
    }
    // CRITICAL: logs at CRITICAL severity and wakes the crash reporter.
    AVER_CRITICAL("[RHI.D3D12] THE GPU DEVICE HAS BEEN LOST, noticed at {} (0x{:08X}): {}. Nothing "
                  "further will be drawn -- this engine cannot recreate a device, so the editor has "
                  "to be restarted. The last frame stays on screen.",
                  where, static_cast<u32>(reason), what);
    // DRED capture, if --dred armed it.
    dumpDredOnDeviceRemoved();
    return true;
}

// Logs DRED (--dred) diagnostics: GetDeviceRemovedReason, auto-breadcrumb trail, and page-fault allocations.
// Runs after the device is gone; every pointer is a COM object that may now return E_FAIL.
void D3D12Device::dumpDredOnDeviceRemoved() {
    if (!dredEnabled()) return;   // --dred was never passed
    if (!device_) {
        AVER_WARN("[RHI.D3D12][DRED] --dred was on, but there is no device left to query");
        return;
    }
    AVER_ERROR("[RHI.D3D12][DRED] GetDeviceRemovedReason = 0x{:08X}",
               static_cast<u32>(device_->GetDeviceRemovedReason()));

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    ComPtr<ID3D12DeviceRemovedExtendedData>  dred;
    const bool have1 = SUCCEEDED(device_.As(&dred1));
    if (!have1 && FAILED(device_.As(&dred))) {
        AVER_WARN("[RHI.D3D12][DRED] the device-removed-extended-data interface is unavailable -- "
                  "was --dred actually in effect before this device was created?");
        return;
    }

    // ---- Auto-breadcrumbs: which command list/queue was mid-flight, and where ----
    u32 finished = 0, unfinished = 0;
    if (have1) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 out{};
        if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&out))) {
            u32 idx = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = out.pHeadAutoBreadcrumbNode; node; node = node->pNext, ++idx) {
                // Breadcrumb context: BeginEvent/SetMarker strings attached to this command list.
                auto contextFor = [node](UINT i) -> std::string {
                    for (UINT c = 0; c < node->BreadcrumbContextsCount; ++c)
                        if (node->pBreadcrumbContexts[c].BreadcrumbIndex == i)
                            return " [" + dredNarrow(node->pBreadcrumbContexts[c].pContextString) + "]";
                    return "";
                };
                if (logBreadcrumbNodeIfUnfinished(idx, node, contextFor)) ++unfinished; else ++finished;
            }
        } else {
            AVER_WARN("[RHI.D3D12][DRED] GetAutoBreadcrumbsOutput1 failed");
        }
    } else {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT out{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&out))) {
            auto noContext = [](UINT) { return std::string(); };
            u32 idx = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = out.pHeadAutoBreadcrumbNode; node; node = node->pNext, ++idx) {
                if (logBreadcrumbNodeIfUnfinished(idx, node, noContext)) ++unfinished; else ++finished;
            }
            AVER_INFO("[RHI.D3D12][DRED] no breadcrumb context on this driver/SDK "
                      "(ID3D12DeviceRemovedExtendedDataSettings1 was unavailable at init) -- marker "
                      "names above are not attributed to an op");
        } else {
            AVER_WARN("[RHI.D3D12][DRED] GetAutoBreadcrumbsOutput failed");
        }
    }
    if (finished) AVER_INFO("[RHI.D3D12][DRED] {} command list(s) reported every op complete -- not where this hung", finished);
    if (unfinished == 0)
        AVER_WARN("[RHI.D3D12][DRED] no unfinished command list in the breadcrumb trail -- the hang "
                  "may be outside anything this device recorded (Present itself, a driver-side "
                  "kernel, or a different queue than the main direct one)");

    // ---- Page faults ----
    if (have1) {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
        if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pf))) {
            AVER_ERROR("[RHI.D3D12][DRED] page-fault VA = 0x{:016X}{}", pf.PageFaultVA,
                       pf.PageFaultVA == 0 ? " (zero -- this device loss was not a page fault)" : "");
            logDredAllocationList("existing allocation", pf.pHeadExistingAllocationNode);
            logDredAllocationList("recently freed allocation", pf.pHeadRecentFreedAllocationNode);
        } else {
            AVER_WARN("[RHI.D3D12][DRED] GetPageFaultAllocationOutput1 failed");
        }
    } else {
        D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
            AVER_ERROR("[RHI.D3D12][DRED] page-fault VA = 0x{:016X}{}", pf.PageFaultVA,
                       pf.PageFaultVA == 0 ? " (zero -- this device loss was not a page fault)" : "");
            logDredAllocationList("existing allocation", pf.pHeadExistingAllocationNode);
            logDredAllocationList("recently freed allocation", pf.pHeadRecentFreedAllocationNode);
        } else {
            AVER_WARN("[RHI.D3D12][DRED] GetPageFaultAllocationOutput failed");
        }
    }
}

bool D3D12Device::waitFence(u64 value) {
    if (!fence_ || !fenceEvent_) return true;
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) return false;

    for (u32 slice = 0;; ++slice) {
        if (WaitForSingleObject(fenceEvent_, 1000) != WAIT_TIMEOUT) return true;
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            noteDeviceRemoved("a fence wait", removed);
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

// The single live UI backend, for the Win32 message thunk below.
// Set in uiInit, cleared in uiShutdown.
static d3d12::IUiBackend* g_activeUiBackend = nullptr;

// Feeds one window message to the installed UI backend. True when it consumed it.
static bool uiBackendWndProcThunk(void* hwnd, u32 msg, u64 w, i64 l) {
    return g_activeUiBackend && g_activeUiBackend->wndProc(hwnd, msg, w, l);
}

// Brings up the installed UI backend on this device and window. Returns false if none was installed or init failed.
bool D3D12Device::uiInit(void* hwnd) {
    if (uiActive_) return true;
    if (!uiBackend_ || !device_ || !hwnd) return false;

    d3d12::UiBackendInitDesc desc;
    desc.device = device_.Get();
    desc.commandQueue = queue_.Get();
    // Double the frame count: ImGui's DX12 backend advances its ring once per render,
    // and frame interpolation renders UI twice per frame (generated, then real).
    desc.frameCount = kFrameCount * 2;
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

// Shuts down the UI backend.
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
// Implements aver::rhi::IResourceFactory / IRenderContext, sharing the device with the renderer above.

// Drains the GPU, then releases everything the factory still owns.
D3D12ResourceFactory::~D3D12ResourceFactory() {
    dev_->waitForGpu();
    retired_.clear();
}

// Creates the shader-visible descriptor heap and CPU-only staging heap.
bool D3D12ResourceFactory::init() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kRhiHeapSize;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!hrOk(dev_->device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "rhi descriptor heap")) return false;
    heapStride_ = dev_->device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC sd{};
    sd.NumDescriptors = kRhiHeapSize;
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!hrOk(dev_->device_->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&stageHeap_)), "rhi staging descriptor heap")) return false;
    return true;
}

// ---- handle tables: a record whose resource is gone reads as an invalid handle ----
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
    return s.valid() ? &s : nullptr;
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
// CPU handle of descriptor slot `index` in the CPU-only staging heap.
D3D12_CPU_DESCRIPTOR_HANDLE D3D12ResourceFactory::stagingCpu(u32 index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = stageHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * heapStride_;
    return h;
}

void D3D12ResourceFactory::noteBindingSetWritten(BindingSetHandle set, RhiBindingSet& s) {
    ++s.version;
    if (dev_->fovSet_ == set) dev_->fovValid_ = false;
    if (dev_->dbSet_  == set) dev_->dbValid_  = false;
}

// Writes a null view of the declared dimension into every slot of a set.
// Tier 1 hardware reads undefined data from any descriptor never written.
D3D12_SHADER_RESOURCE_VIEW_DESC D3D12ResourceFactory::nullSrvDesc(SlotKind kind) {
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
        // Null structured-buffer view needs a stride.
        sv.Format = DXGI_FORMAT_UNKNOWN;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Buffer.NumElements = 0;
        sv.Buffer.StructureByteStride = 4;
    } else if (kind == SlotKind::Texture3D) {
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = 1;
    } else if (kind == SlotKind::Texture2DMS) {
        // D3D12_TEX2DMS_SRV is an empty struct; see setSrv.
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
    }
    return sv;
}

// Writes into the STAGING heap; see RhiBindingSet's comment.
void D3D12ResourceFactory::nullFill(RhiBindingSet& s) {
    for (u32 i = 0; i < s.srvCount; ++i) {
        const D3D12_SHADER_RESOURCE_VIEW_DESC sv = nullSrvDesc(s.srvKinds[i]);
        dev_->device_->CreateShaderResourceView(nullptr, &sv, stagingCpu(s.stageBase + i));
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
        dev_->device_->CreateUnorderedAccessView(nullptr, nullptr, &uv, stagingCpu(s.stageBase + s.srvCount + i));
    }
    ++s.version;
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

// Reserves `count` contiguous descriptors in the CPU-only staging heap.
bool D3D12ResourceFactory::allocStageRange(u32 count, u32& outFirst) {
    for (usize i = 0; i < stageFreeRanges_.size(); ++i) {
        if (stageFreeRanges_[i].count < count) continue;
        outFirst = stageFreeRanges_[i].first;
        if (stageFreeRanges_[i].count > count) {
            stageFreeRanges_[i].first += count;
            stageFreeRanges_[i].count -= count;
        } else {
            stageFreeRanges_[i] = stageFreeRanges_.back();
            stageFreeRanges_.pop_back();
        }
        return true;
    }
    if (stageHeapUsed_ + count > kRhiHeapSize) {
        AVER_ERROR("[RHI.D3D12] binding-set staging heap exhausted: {} of {} slots used, {} more wanted",
                   stageHeapUsed_, kRhiHeapSize, count);
        return false;
    }
    outFirst = stageHeapUsed_;
    stageHeapUsed_ += count;
    return true;
}

// The fence value at which work recorded right now can be considered retired.
u64 D3D12ResourceFactory::retireFence() const {
    // Retired while a frame records: that frame's open command list may already use it, and a mid-frame
    // waitForGpu (the occlusion culler waits every frame) signals the value below before the frame is
    // submitted. So it waits for the frame's own present fence instead (resolvePendingRetires).
    if (dev_->recording_) return kPendingFrameFence;
    const u64 completed = dev_->fence_ ? dev_->fence_->GetCompletedValue() : 0;
    return (dev_->nextFence_ > completed ? dev_->nextFence_ : completed) + 1;
}

// present() signalled `fence` for the frame that recorded these retirements.
void D3D12ResourceFactory::resolvePendingRetires(u64 fence) {
    for (RetiredObject& r : retired_)          if (r.fence == kPendingFrameFence) r.fence = fence;
    for (RetiredRange& r : pendingRanges_)     if (r.fence == kPendingFrameFence) r.fence = fence;
    for (RetiredRange& r : stagePendingRanges_) if (r.fence == kPendingFrameFence) r.fence = fence;
}

// Queues an object for release once the GPU has passed the current frame.
void D3D12ResourceFactory::retire(ComPtr<IUnknown> obj) {
    if (!obj) return;
    retired_.push_back({std::move(obj), retireFence()});
}

// Releases every retired object and descriptor range whose fence has passed.
void D3D12ResourceFactory::collect() {
    if (!dev_->fence_ || (retired_.empty() && pendingRanges_.empty() && stagePendingRanges_.empty())) return;
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
    for (usize i = 0; i < stagePendingRanges_.size();) {
        if (stagePendingRanges_[i].fence <= done) {
            stageFreeRanges_.push_back({stagePendingRanges_[i].first, stagePendingRanges_[i].count, 0});
            stagePendingRanges_[i] = stagePendingRanges_.back();
            stagePendingRanges_.pop_back();
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
    D3D12_ROOT_PARAMETER params[2 * kBindingTableCount + kMaxConstantSlots + 5] = {};
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

    // Byte-identity with prior serialisation required by gates (assigning 0 maintains unchanged fields).
    for (u32 s = 0; s < kMaxConstantSlots; ++s) {
        if (layout.constantDwords[s]) {
            params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[n].Constants.ShaderRegister = s;
            params[n].Constants.RegisterSpace = layout.constantSpace;
            params[n].Constants.Num32BitValues = layout.constantDwords[s];
        } else {
            params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[n].Descriptor.ShaderRegister = s;
            params[n].Descriptor.RegisterSpace = layout.constantSpace;
        }
        e.slotParam[s] = static_cast<i32>(n++);
    }

    if (mesh) {
        // Geometry SRVs sit past both declared tables (pinned by RHIResources.hpp and prelude).
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
        // World-matrix SRV past mesh geometry SRVs (see RHIResources.hpp).
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[n].Descriptor.ShaderRegister = declaredSrvCount(layout) + (mesh ? 2 : 0);
        e.instanceWorldParam = static_cast<i32>(n++);
    }
    // Bindless table appended last in register space 1 (preserves byte-identity for raster pipelines).
    D3D12_DESCRIPTOR_RANGE bindlessRange{};
    if (layout.bindlessTextureCount) {
        bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindlessRange.NumDescriptors = layout.bindlessTextureCount;
        bindlessRange.BaseShaderRegister = 0;
        bindlessRange.RegisterSpace = 1;
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[n].DescriptorTable.NumDescriptorRanges = 1;
        params[n].DescriptorTable.pDescriptorRanges = &bindlessRange;
        e.bindlessParam = static_cast<i32>(n++);
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
        samplers[i].RegisterSpace = layout.samplerSpace;
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
// Fills a new texture from TextureDesc::initialData on a one-shot command list, and blocks until the copy retires.
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

// Blocks on GPU round trip per call; see declaration.
bool D3D12ResourceFactory::uploadBuffers(std::initializer_list<BufferUploadItem> items) {
    ID3D12Device* dev = dev_->device_.Get();
    u64 total = 0;
    for (const auto& it : items) total += it.bytes;
    if (total == 0) return true;   // nothing to copy is not a failure

    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto ud = bufferDesc(total);
    ComPtr<ID3D12Resource> staging;
    if (!hrOk(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)),
              "rhi buffer staging")) return false;
    setDebugName(staging.Get(), "rhi buffer staging");

    u8* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (!hrOk(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "rhi buffer staging Map"))
        return false;
    std::vector<u64> offsets;
    offsets.reserve(items.size());
    {
        u64 offset = 0;
        for (const auto& it : items) {
            offsets.push_back(offset);
            if (it.src && it.bytes) std::memcpy(mapped + offset, it.src, static_cast<size_t>(it.bytes));
            offset += it.bytes;
        }
    }
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!hrOk(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
              "rhi buffer upload alloc")) return false;
    if (!hrOk(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
              IID_PPV_ARGS(&list)), "rhi buffer upload list")) return false;

    {
        size_t idx = 0;
        for (const auto& it : items) {
            if (it.dst && it.bytes) list->CopyBufferRegion(it.dst, 0, staging.Get(), offsets[idx], it.bytes);
            ++idx;
        }
    }

    // Explicit transition to COMMON (not left to submit-time decay).
    std::vector<D3D12_RESOURCE_BARRIER> back;
    back.reserve(items.size());
    for (const auto& it : items)
        if (it.dst && it.bytes)
            back.push_back(transition(it.dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
    if (!back.empty()) list->ResourceBarrier(static_cast<UINT>(back.size()), back.data());

    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    dev_->queue_->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> f;
    if (!hrOk(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "rhi buffer upload fence"))
        return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->queue_->Signal(f.Get(), 1);
    if (f->GetCompletedValue() < 1 && ev) { f->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, INFINITE); }
    if (ev) CloseHandle(ev);

    retire(staging);
    collect();
    return true;
}

// uploadBuffers with in-place fill; same one-shot list and blocking wait.
bool D3D12ResourceFactory::uploadBufferFilled(ID3D12Resource* dst, u64 bytes, const std::function<void(u8*)>& fill) {
    if (!dst || bytes == 0) return true;
    ID3D12Device* dev = dev_->device_.Get();
    auto up = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto ud = bufferDesc(bytes);
    ComPtr<ID3D12Resource> staging;
    if (!hrOk(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)),
              "rhi buffer staging")) return false;
    setDebugName(staging.Get(), "rhi buffer staging");
    u8* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (!hrOk(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "rhi buffer staging Map"))
        return false;
    fill(mapped);
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!hrOk(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
              "rhi buffer upload alloc")) return false;
    if (!hrOk(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
              IID_PPV_ARGS(&list)), "rhi buffer upload list")) return false;
    list->CopyBufferRegion(dst, 0, staging.Get(), 0, bytes);
    const D3D12_RESOURCE_BARRIER back = transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &back);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    dev_->queue_->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> f;
    if (!hrOk(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "rhi buffer upload fence"))
        return false;
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
    if (d.debugName) t.debugName = d.debugName;
#if AVER_RHI_TRACK_STATE
    t.states.assign(mips, d.initialState);
#endif
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

// Adopts an external depth texture; see declaration for ownership contract.
TextureHandle D3D12ResourceFactory::adoptExternalDepthTexture(ID3D12Resource* resource, u32 width, u32 height,
                                                               TextureHandle existing) {
    if (!resource) return 0;
    RhiTexture t;
    t.res = resource;   // ComPtr AddRefs; SHARED ownership with dev_->depthBuffer_.
    t.desc.dim = TextureDim::Tex2D;
    t.desc.width = width; t.desc.height = height; t.desc.depth = 1; t.desc.mips = 1;
    // R32Typeless aliased as D32Float (DSV) / R32Float (SRV), per Format::R32Typeless doc.
    t.desc.format = Format::R32Typeless;
    t.desc.bind = ResourceBind::ShaderResource | ResourceBind::DepthStencil;
    // Matches createDepthBuffer() state; caller must textureBarrier if reading.
    t.desc.initialState = ResourceState::DepthWrite;
    t.desc.debugName = nullptr;
    t.debugName = "scene depth (adopted)";
#if AVER_RHI_TRACK_STATE
    t.states.assign(1, ResourceState::DepthWrite);
#endif
    if (existing) {
        RhiTexture* slot = texture(existing);
        if (slot) { *slot = std::move(t); return existing; }
    }
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

// Adopts an external render-target texture; see declaration for ownership contract.
TextureHandle D3D12ResourceFactory::adoptExternalRenderTargetTexture(ID3D12Resource* resource, Format fmt,
                                                                     u32 width, u32 height,
                                                                     const char* debugName,
                                                                     TextureHandle existing) {
    if (!resource) return 0;
    RhiTexture t;
    t.res = resource;   // ComPtr AddRefs; SHARED ownership with D3D12Device (gbufVelocity_ etc.).
    t.desc.dim = TextureDim::Tex2D;
    t.desc.width = width; t.desc.height = height; t.desc.depth = 1; t.desc.mips = 1;
    t.desc.format = fmt;
    t.desc.bind = ResourceBind::ShaderResource | ResourceBind::RenderTarget;
    // Matches createGBufferTargets() state (RENDER_TARGET); caller must textureBarrier if reading.
    t.desc.initialState = ResourceState::RenderTarget;
    t.desc.debugName = nullptr;
    if (debugName) t.debugName = debugName;
#if AVER_RHI_TRACK_STATE
    t.states.assign(1, ResourceState::RenderTarget);
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
    // Readback buffer stays in COPY_DEST (copy-only target).
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
    if (d.debugName) b.debugName = d.debugName;
#if AVER_RHI_TRACK_STATE
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

    // Precompiled path: DXIL bytecode bypasses checks that require source/entry (see ShaderDesc::bytecode).
    if (d.precompiled()) {
        // Mesh and Amplification need hardware support, not bytecode property.
        if ((d.stage == ShaderStage::Mesh || d.stage == ShaderStage::Amplification)
            && dev_->caps_.meshShaderTier == 0) {
            AVER_WARN("[RHI.D3D12] createShader (precompiled) needs mesh-shader hardware, which this "
                      "device reports as tier 0");
            return 0;
        }
        // DXIL container fourcc check (prevents SPIR-V being passed to D3D12).
        const u8* src = static_cast<const u8*>(d.bytecode);
        if (d.bytecodeSize < 4 || src[0] != 'D' || src[1] != 'X' || src[2] != 'B' || src[3] != 'C') {
            AVER_ERROR("[RHI.D3D12] createShader (precompiled): {} bytes not beginning with the DXIL "
                       "container fourcc -- SPIR-V, or a truncated blob?", d.bytecodeSize);
            return 0;
        }
        RhiShader s;
        s.bytes.assign(src, src + d.bytecodeSize);
        s.stage = d.stage;
        shaders_.push_back(std::move(s));
        return static_cast<ShaderHandle>(shaders_.size());
    }

    if (!d.source || !d.entry) { AVER_ERROR("[RHI.D3D12] createShader without source or entry point"); return 0; }

    // Mesh and Amplification: D3D12 Ultimate stages, tier-1+ mesh-shader hardware required.
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
    p.bindlessParam = rs->bindlessParam;
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
        // Blended pipeline writes target 0 only; mask other G-buffer targets.
        if (d.blend != BlendMode::Opaque && d.renderTargetCount > 1) {
            blend.IndependentBlendEnable = TRUE;
            for (u32 i = 1; i < 8; ++i) {
                blend.RenderTarget[i] = {};
                blend.RenderTarget[i].SrcBlend = blend.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
                blend.RenderTarget[i].DestBlend = blend.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
                blend.RenderTarget[i].BlendOp = blend.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
                blend.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
                blend.RenderTarget[i].RenderTargetWriteMask = 0;
            }
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
        s.ms = ms->code();
        if (as) s.as = as->code();
        if (ps) s.ps = ps->code();
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
        pd.VS = vs->code();
        if (gs) pd.GS = gs->code();
        if (ps) pd.PS = ps->code();
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
    p.bindlessParam = rs->bindlessParam;
    p.srvBaseRegister[1] = d.layout.srvCount;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) { p.slotParam[i] = rs->slotParam[i]; p.slotDwords[i] = d.layout.constantDwords[i]; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = rs->sig.Get();
    cp.CS = cs->code();
    if (!hrOk(dev_->device_->CreateComputePipelineState(&cp, IID_PPV_ARGS(&p.pso)), "rhi compute pipeline")) return 0;
    pipelines_.push_back(std::move(p));
    return static_cast<PipelineHandle>(pipelines_.size());
}

// Reserves a descriptor range for a binding set, null-fills it, and returns its handle.
// ---- the ray path's bindless texture table ----

BindlessTableHandle D3D12ResourceFactory::createBindlessTextureTable(u32 capacity) {
    collect();
    if (capacity == 0) { AVER_ERROR("[RHI.D3D12] createBindlessTextureTable with capacity 0"); return 0; }
    RhiBindlessTable t;
    t.capacity = capacity;
    if (!allocRange(capacity, t.heapBase)) {
        AVER_ERROR("[RHI.D3D12] bindless texture table of {} descriptors did not fit; ray-traced "
                   "texturing will stay off and the flat-albedo path will be used instead", capacity);
        return 0;
    }
    // Every slot null-filled before binding (null descriptors prevent device-removal risk).
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    for (u32 i = 0; i < capacity; ++i)
        dev_->device_->CreateShaderResourceView(nullptr, &sv, cpuSlot(t.heapBase + i));
    t.alive = true;
    bindlessTables_.push_back(t);
    AVER_INFO("[RHI.D3D12] bindless texture table: {} descriptors at heap slot {}", capacity, t.heapBase);
    return static_cast<BindlessTableHandle>(bindlessTables_.size());
}

void D3D12ResourceFactory::destroyBindlessTextureTable(BindlessTableHandle h) {
    if (h == 0 || h > bindlessTables_.size()) return;
    RhiBindlessTable& t = bindlessTables_[h - 1];
    if (!t.alive) return;
    t.alive = false;
    // GPU may still be reading this range from a frame in flight.
    pendingRanges_.push_back({t.heapBase, t.capacity, retireFence()});
}

u32 D3D12ResourceFactory::bindlessTableCapacity(BindlessTableHandle h) const {
    if (h == 0 || h > bindlessTables_.size()) return 0;
    return bindlessTables_[h - 1].alive ? bindlessTables_[h - 1].capacity : 0;
}

bool D3D12ResourceFactory::setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle th) {
    if (h == 0 || h > bindlessTables_.size() || !bindlessTables_[h - 1].alive) {
        AVER_ERROR("[RHI.D3D12] setBindlessTexture with an invalid table handle");
        return false;
    }
    RhiBindlessTable& t = bindlessTables_[h - 1];
    // Out-of-bounds writes corrupt adjacent binding sets; this engine lost a device this way.
    if (index >= t.capacity) {
        AVER_ERROR("[RHI.D3D12] setBindlessTexture index {} past the table's {} slots -- refused. "
                   "The material keeps its factor colour instead of a texture.", index, t.capacity);
        return false;
    }
    RhiTexture* tex = texture(th);
    if (!tex) { AVER_ERROR("[RHI.D3D12] setBindlessTexture with an invalid texture handle"); return false; }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = toDxgiSrvFormat(tex->desc.format);
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = tex->desc.mips;
    dev_->device_->CreateShaderResourceView(tex->res.Get(), &sv, cpuSlot(t.heapBase + index));
    return true;
}

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
    // One staging range plus per-frame shader-visible ranges; early failure returns ranges to free lists.
    if (!allocStageRange(count, s.stageBase)) return 0;
    u32 framesAllocated = 0;
    for (; framesAllocated < kFrameCount; ++framesAllocated) {
        if (!allocRange(count, s.gpuBase[framesAllocated])) break;
    }
    if (framesAllocated < kFrameCount) {
        for (u32 f = 0; f < framesAllocated; ++f) freeRanges_.push_back({s.gpuBase[f], count, 0});
        stageFreeRanges_.push_back({s.stageBase, count, 0});
        return 0;
    }
    s.alive = true;
    nullFill(s);
    bindingSets_.push_back(s);
    return static_cast<BindingSetHandle>(bindingSets_.size());
}

namespace {
// Shared by prebuild query, build, and update; all three must agree on Flags/NumDescs/geometry.
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs(const GpuMesh& m, D3D12_RAYTRACING_GEOMETRY_DESC& geo,
                                                                 bool allowUpdate) {
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
    if (allowUpdate) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.NumDescs = 1;
    in.pGeometryDescs = &geo;
    return in;
}

// One geometry per part, OPAQUE only where requested. False if a part's mesh is gone.
bool blasMultiInputs(const std::vector<GpuMesh>& meshes, const std::vector<BlasGeometry>& parts,
                     std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geos,
                     D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in) {
    geos.assign(parts.size(), D3D12_RAYTRACING_GEOMETRY_DESC{});
    for (usize i = 0; i < parts.size(); ++i) {
        const MeshHandle h = parts[i].mesh;
        if (h == 0 || h > meshes.size() || !meshes[h - 1].alive || meshes[h - 1].indexCount == 0) return false;
        const GpuMesh& m = meshes[h - 1];
        D3D12_RAYTRACING_GEOMETRY_DESC& geo = geos[i];
        geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geo.Flags = parts[i].opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geo.Triangles.VertexBuffer.StartAddress = m.vb->GetGPUVirtualAddress();
        geo.Triangles.VertexBuffer.StrideInBytes = sizeof(MeshVertex);
        geo.Triangles.VertexCount = m.vbv.SizeInBytes / sizeof(MeshVertex);
        geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geo.Triangles.IndexBuffer = m.ib->GetGPUVirtualAddress();
        geo.Triangles.IndexCount = m.indexCount;
        geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    }
    in = {};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = static_cast<UINT>(geos.size());
    in.pGeometryDescs = geos.data();
    return true;
}

// Shared by prebuild query, build, and update; NumDescs aside (update keeps its build-time descs).
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs(u32 numDescs, bool allowUpdate) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (allowUpdate) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.NumDescs = numDescs;
    return in;
}

// Convert engine instance to DXR format. Shared by per-frame packing and static prefix.
D3D12_RAYTRACING_INSTANCE_DESC toInstanceDesc(const TlasInstance& in, D3D12_GPU_VIRTUAL_ADDRESS blasVa) {
    static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) == kTlasInstanceDescBytes,
                  "tlasStaticInstanceBuffer's element size is the DXR instance desc");
    D3D12_RAYTRACING_INSTANCE_DESC id{};
    // Engine: row-major row-vector (v*M); DXR: 3x4 column-vector [R|T].
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) id.Transform[r][c] = in.world[c * 4 + r];
        id.Transform[r][3] = in.world[12 + r];
    }
    id.InstanceMask = in.mask;
    id.InstanceID = in.instanceId;
    // Flag mapping: engine flags have D3D12 twins; unknown bits fail to compile, not silent.
    u32 d3dFlags = 0;
    if (in.flags & TlasInstanceFlag_TriangleCullDisable)
        d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    if (in.flags & TlasInstanceFlag_TriangleFrontCcw)
        d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
    if (in.flags & TlasInstanceFlag_ForceOpaque)
        d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
    if (in.flags & TlasInstanceFlag_ForceNonOpaque)
        d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
    id.Flags = d3dFlags;
    id.AccelerationStructure = blasVa;
    return id;
}
} // namespace

// Allocates a bottom-level acceleration structure for a mesh, sized by the prebuild query.
BlasHandle D3D12ResourceFactory::createBlas(MeshHandle mesh) { return createBlasImpl(mesh, false); }
// Same, but with ALLOW_UPDATE and scratch for an update too.
BlasHandle D3D12ResourceFactory::createBlasUpdatable(MeshHandle mesh) { return createBlasImpl(mesh, true); }

BlasHandle D3D12ResourceFactory::createBlasImpl(MeshHandle mesh, bool allowUpdate) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createBlas without ray-tracing support"); return 0; }
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.D3D12] createBlas with an invalid mesh handle"); return 0; }
    // Destroyed mesh: slot exists, bounds pass, but vertex view is cleared.
    if (!dev_->meshes_[mesh - 1].alive) { AVER_WARN("[RHI.D3D12] createBlas for destroyed mesh {}", mesh); return 0; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    if (m.indexCount == 0) { AVER_ERROR("[RHI.D3D12] createBlas for a mesh with no indices"); return 0; }

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = blasInputs(m, geo, allowUpdate);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

    RhiBlas b;
    b.mesh = mesh;
    b.allowUpdate = allowUpdate;
    b.as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    // An updatable structure's scratch must cover whichever of a build or an update asks for more --
    // it is reused for both, and D3D12 sizes the two independently.
    b.scratchBytes = allowUpdate ? std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes)
                                 : info.ScratchDataSizeInBytes;
    // A static BLAS gets its scratch in buildBlas and gives it back after (docs/rendering/VRAM.md).
    if (allowUpdate) b.scratch = makeAsBuffer(dev_->device_.Get(), b.scratchBytes, D3D12_RESOURCE_STATE_COMMON);
    if (!b.as || (allowUpdate && !b.scratch)) { AVER_ERROR("[RHI.D3D12] createBlas allocation failed"); return 0; }
    blases_.push_back(std::move(b));
    return static_cast<BlasHandle>(blases_.size());
}

// Acceleration structure over multiple meshes; sized by prebuild query.
BlasHandle D3D12ResourceFactory::createBlasMulti(const BlasGeometry* geometries, u32 count) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createBlasMulti without ray-tracing support"); return 0; }
    if (!geometries || count == 0) { AVER_ERROR("[RHI.D3D12] createBlasMulti with no geometry"); return 0; }
    RhiBlas b;
    b.geometries.assign(geometries, geometries + count);
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geos;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    if (!blasMultiInputs(dev_->meshes_, b.geometries, geos, in)) {
        AVER_ERROR("[RHI.D3D12] createBlasMulti: a geometry names an invalid, destroyed or index-less mesh");
        return 0;
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    b.mesh = geometries[0].mesh;
    b.as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    b.scratchBytes = info.ScratchDataSizeInBytes;
    if (!b.as) { AVER_ERROR("[RHI.D3D12] createBlasMulti allocation failed"); return 0; }
    blases_.push_back(std::move(b));
    return static_cast<BlasHandle>(blases_.size());
}

// Allocates a top-level acceleration structure for up to `maxInstances` instances.
TlasHandle D3D12ResourceFactory::createTlas(u32 maxInstances) { return createTlasImpl(maxInstances, false); }
// Same, but with ALLOW_UPDATE and scratch for an update too.
TlasHandle D3D12ResourceFactory::createTlasUpdatable(u32 maxInstances) { return createTlasImpl(maxInstances, true); }

TlasHandle D3D12ResourceFactory::createTlasImpl(u32 maxInstances, bool allowUpdate) {
    collect();
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] createTlas without ray-tracing support"); return 0; }
    if (maxInstances == 0) { AVER_ERROR("[RHI.D3D12] createTlas for zero instances"); return 0; }

    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = tlasInputs(maxInstances, allowUpdate);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

    RhiTlas t;
    t.maxInstances = maxInstances;
    t.allowUpdate = allowUpdate;
    t.as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    const u64 scratchBytes = allowUpdate ? std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes)
                                          : info.ScratchDataSizeInBytes;
    t.scratch = makeAsBuffer(dev_->device_.Get(), scratchBytes, D3D12_RESOURCE_STATE_COMMON);
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

// Static prefix validation and allocation before replacement; refusal leaves TLAS unchanged.
bool D3D12ResourceFactory::setTlasStaticInstances(TlasHandle h, const TlasInstance* instances, u32 count) {
    collect();
    RhiTlas* t = tlas(h);
    if (!t) { AVER_ERROR("[RHI.D3D12] setTlasStaticInstances with an invalid handle"); return false; }
    if (!dev_->device5_) { AVER_WARN("[RHI.D3D12] setTlasStaticInstances without ray-tracing support"); return false; }
    if (count == 0 && t->staticCount == 0 && !t->staticDescs) return true;   // no prefix to remove
    if (count && !instances) { AVER_ERROR("[RHI.D3D12] setTlasStaticInstances: {} instances and no array", count); return false; }
    if (static_cast<u64>(count) + t->maxInstances > kMaxTlasInstances) {
        AVER_ERROR("[RHI.D3D12] setTlasStaticInstances: {} static + {} per-frame instances is past the {} one "
                   "TLAS may hold -- refused, the previous prefix kept", count, t->maxInstances, kMaxTlasInstances);
        return false;
    }

    // Instances have fixed slot indices; dropping one corrupts all later ones. Gather distinct BLASes.
    std::vector<BlasHandle> distinct;
    for (u32 i = 0; i < count; ++i) {
        const RhiBlas* b = blas(instances[i].blas);
        if (!b || !b->as) {
            AVER_ERROR("[RHI.D3D12] setTlasStaticInstances: instance {} names an invalid BLAS -- refused whole, "
                       "the previous prefix kept", i);
            return false;
        }
        if (instances[i].instanceId > kMaxTlasInstanceId) {
            AVER_ERROR("[RHI.D3D12] setTlasStaticInstances: instance {} has id {} which does not fit in 24 bits "
                       "-- refused whole, the previous prefix kept", i, instances[i].instanceId);
            return false;
        }
        if (distinct.empty() || distinct.back() != instances[i].blas) distinct.push_back(instances[i].blas);
    }
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());

    // Sized for prefix + per-frame maximum from the prebuild query.
    const u32 total = count + t->maxInstances;
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = tlasInputs(total, t->allowUpdate);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    const u64 scratchBytes = t->allowUpdate ? std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes)
                                            : info.ScratchDataSizeInBytes;
    ComPtr<ID3D12Resource> as = makeAsBuffer(dev_->device_.Get(), info.ResultDataMaxSizeInBytes,
                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    ComPtr<ID3D12Resource> scratch = makeAsBuffer(dev_->device_.Get(), scratchBytes, D3D12_RESOURCE_STATE_COMMON);
    if (!as || !scratch) {
        AVER_ERROR("[RHI.D3D12] setTlasStaticInstances: {:.1f} MiB structure / {:.1f} MiB scratch for {} instances "
                   "could not be allocated -- the previous prefix kept",
                   static_cast<f64>(info.ResultDataMaxSizeInBytes) / (1024.0 * 1024.0),
                   static_cast<f64>(scratchBytes) / (1024.0 * 1024.0), total);
        return false;
    }

    BufferHandle descs = 0;
    const u64 descBytes = static_cast<u64>(total) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (count) {
        BufferDesc bd;
        bd.bytes = descBytes;
        bd.kind = BufferKind::Default;
        bd.debugName = "rhi TLAS static instances";
        descs = createBuffer(bd);
        ID3D12Resource* descRes = bufferResource(descs);
        const bool filled = descRes && uploadBufferFilled(descRes, static_cast<u64>(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
            [&](u8* dst) {
                auto* out = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(dst);
                for (u32 i = 0; i < count; ++i)
                    out[i] = toInstanceDesc(instances[i], blases_[instances[i].blas - 1].as->GetGPUVirtualAddress());
            });
        if (!filled) {
            AVER_ERROR("[RHI.D3D12] setTlasStaticInstances: the {:.1f} MiB instance buffer could not be created "
                       "or filled -- the previous prefix kept", static_cast<f64>(descBytes) / (1024.0 * 1024.0));
            if (descs) destroyBuffer(descs);
            return false;
        }
#if AVER_RHI_TRACK_STATE
        // State managed by build; caller's bufferBarrier on it is reported, not obeyed.
        buffers_[descs - 1].stateFixed = true;
#endif
    }

    // Replaced, never written in place; frames in flight traverse old structure until fence retires it.
    retire(t->as);
    retire(t->scratch);
    t->as = as;
    t->scratch = scratch;
    if (t->staticDescs) destroyBuffer(t->staticDescs);
    t->staticDescs = descs;
    t->staticCount = count;
    t->staticDescsState = D3D12_RESOURCE_STATE_COMMON;
    t->staticBlases = std::move(distinct);
    t->staticBrokenLogged = false;
    t->built = false;
    t->builtStatic = 0;
    t->builtSlots.clear();
    AVER_INFO("[RHI.D3D12] TLAS {} static prefix: {} instance(s) over {} BLAS(es) + {} per frame -- structure "
              "{:.1f} MiB, scratch {:.1f} MiB, instance descs {:.1f} MiB (prebuild sizes)",
              h, count, t->staticBlases.size(), t->maxInstances,
              static_cast<f64>(info.ResultDataMaxSizeInBytes) / (1024.0 * 1024.0),
              static_cast<f64>(scratchBytes) / (1024.0 * 1024.0),
              count ? static_cast<f64>(descBytes) / (1024.0 * 1024.0) : 0.0);
    return true;
}

BufferHandle D3D12ResourceFactory::tlasStaticInstanceBuffer(TlasHandle h) const {
    if (h == 0 || h > tlases_.size()) return 0;
    const RhiTlas& t = tlases_[h - 1];
    return t.staticCount ? t.staticDescs : 0;
}

u64 D3D12ResourceFactory::blasMemoryBytes(BlasHandle h) const {
    if (h == 0 || h > blases_.size()) return 0;
    const RhiBlas& b = blases_[h - 1];
    return (b.as ? b.as->GetDesc().Width : 0) + (b.scratch ? b.scratch->GetDesc().Width : 0);
}

u64 D3D12ResourceFactory::tlasMemoryBytes(TlasHandle h) const {
    if (h == 0 || h > tlases_.size()) return 0;
    const RhiTlas& t = tlases_[h - 1];
    u64 bytes = (t.as ? t.as->GetDesc().Width : 0) + (t.scratch ? t.scratch->GetDesc().Width : 0);
    for (u32 i = 0; i < kFrameCount; ++i) bytes += t.instances[i] ? t.instances[i]->GetDesc().Width : 0;
    if (t.staticDescs && t.staticDescs <= buffers_.size() && buffers_[t.staticDescs - 1].res)
        bytes += buffers_[t.staticDescs - 1].res->GetDesc().Width;
    return bytes;
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

// Releases acceleration structure and its scratch; slot kept to distinguish dead from other.
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
    b.geometries.clear();
    collect();
}

MeshHandle D3D12ResourceFactory::blasMesh(BlasHandle h) const {
    if (h == 0 || h > blases_.size()) return 0;
    return blases_[h - 1].mesh;
}

// Linear scan: one per distinct ray-traced mesh, walked when a feature first meets it, not per frame.
// Dead slot excludes itself. Multi-mesh structure never qualifies.
BlasHandle D3D12ResourceFactory::blasForMesh(MeshHandle mesh) const {
    if (mesh == 0) return 0;
    for (usize i = 0; i < blases_.size(); ++i)
        if (blases_[i].mesh == mesh && blases_[i].built && blases_[i].geometries.empty())
            return static_cast<BlasHandle>(i + 1);
    return 0;
}

// Destroys every structure built from `mesh` -- multi-mesh if ANY geometry names it.
void D3D12ResourceFactory::destroyBlasForMesh(MeshHandle mesh) {
    if (mesh == 0) return;
    for (usize i = 0; i < blases_.size(); ++i) {
        const RhiBlas& b = blases_[i];
        bool names = b.mesh == mesh;
        for (const BlasGeometry& g : b.geometries) names = names || g.mesh == mesh;
        if (names) destroyBlas(static_cast<BlasHandle>(i + 1));
    }
}

// Frees a shader's bytecode.
void D3D12ResourceFactory::destroyShader(ShaderHandle h) {
    RhiShader* s = shader(h);
    if (!s) return;
    s->blob.Reset();
    // shrink_to_fit: clear() leaves capacity allocated; precompiled shader bytes are its whole point.
    s->bytes.clear();
    s->bytes.shrink_to_fit();
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

// Returns a binding set's descriptor ranges -- staging and per-frame shader-visible -- reusable once fence passes.
void D3D12ResourceFactory::destroyBindingSet(BindingSetHandle h) {
    RhiBindingSet* s = bindingSet(h);
    if (!s) return;
    const u32 count = s->srvCount + s->uavCount;
    stagePendingRanges_.push_back({s->stageBase, count, retireFence()});
    for (u32 f = 0; f < kFrameCount; ++f) pendingRanges_.push_back({s->gpuBase[f], count, retireFence()});
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
        // A multisampled resource can't have more than one mip (D3D12_TEX2DMS_SRV carries no
        // mip/level fields), so `mip` is ignored -- the ONE view this dimension expresses already
        // covers the whole resource.
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
    // Staging heap; copied to shader-visible per-frame range at setBindingSet.
    dev_->device_->CreateShaderResourceView(t->res.Get(), &sv, stagingCpu(s->stageBase + slot));
    noteBindingSetWritten(set, *s);
}

// Writes a texture UAV for one mip into one slot of a binding set.
void D3D12ResourceFactory::setUav(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.D3D12] setUav with an invalid handle"); return; }
    if (slot >= s->uavCount) { AVER_ERROR("[RHI.D3D12] setUav slot {} past the {} declared", slot, s->uavCount); return; }
    if (mip == kAllMips || mip >= t->desc.mips) { AVER_ERROR("[RHI.D3D12] setUav needs a single valid mip"); return; }
    // CreateUnorderedAccessView without UnorderedAccess flag removes the device outright (D3D12 RemoveDevice).
    // This check converts device removal into a named error.
    if (!(static_cast<u32>(t->desc.bind) & static_cast<u32>(ResourceBind::UnorderedAccess))) {
        AVER_ERROR("[RHI.D3D12] setUav slot {} binds texture '{}' ({}x{}), which was created without "
                   "ResourceBind::UnorderedAccess -- refusing, because creating the view would remove "
                   "the device. Add UnorderedAccess to its TextureDesc::bind.",
                   slot, t->desc.debugName ? t->desc.debugName : "(unnamed)", t->desc.width,
                   t->desc.height);
        return;
    }

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
    // Staging heap; copied to shader-visible per-frame range at setBindingSet.
    dev_->device_->CreateUnorderedAccessView(t->res.Get(), nullptr, &uv, stagingCpu(s->stageBase + s->srvCount + slot));
    noteBindingSetWritten(set, *s);
}

// Returns one SRV slot to null; must write to staging, not live GPU range.
void D3D12ResourceFactory::clearSrv(BindingSetHandle set, u32 slot) {
    RhiBindingSet* s = bindingSet(set);
    if (!s) { AVER_ERROR("[RHI.D3D12] clearSrv with an invalid set"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] clearSrv slot {} past the {} declared", slot, s->srvCount); return; }
    const D3D12_SHADER_RESOURCE_VIEW_DESC sv = nullSrvDesc(s->srvKinds[slot]);
    dev_->device_->CreateShaderResourceView(nullptr, &sv, stagingCpu(s->stageBase + slot));
    noteBindingSetWritten(set, *s);
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
    // Staging heap; copied to shader-visible per-frame range at setBindingSet.
    dev_->device_->CreateShaderResourceView(nullptr, &sv, stagingCpu(s->stageBase + slot));
    noteBindingSetWritten(set, *s);
}

// True when a structured view of `count` elements of `stride` from `firstElement` fits inside the buffer.
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
    // Staging heap; copied to shader-visible per-frame range at setBindingSet.
    dev_->device_->CreateShaderResourceView(buffers_[bh - 1].res.Get(), &sv, stagingCpu(s->stageBase + slot));
    noteBindingSetWritten(set, *s);
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
    // Staging heap; copied to shader-visible per-frame range at setBindingSet.
    dev_->device_->CreateUnorderedAccessView(buffers_[bh - 1].res.Get(), nullptr, &uv,
                                             stagingCpu(s->stageBase + s->srvCount + slot));
    noteBindingSetWritten(set, *s);
}

// Copies `bytes` out of a readback buffer. Does no synchronisation.
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

// Descriptor for UI rendering; allocated from UI backend's own pool on first use.
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

    // Probes descriptor allocator's free list via stageBase (both heap types use the same fence discipline).
    const u32 firstBase = set ? bindingSets_[set - 1].stageBase : 0;
    destroyBindingSet(set);
    const BindingSetHandle early = createBindingSet(bsd);
    const bool heldBack = early && bindingSets_[early - 1].stageBase != firstBase;
    AVER_INFO("[RHI.D3D12] factory self-test: descriptor reclaim {} (returned range held behind the fence)",
              heldBack ? "ok" : "FAILED");

    destroyBindingSet(early);

    // Buffer views: descriptor validation only (dispatch can't be checked at init without a command list).
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
    dev_->boundPso_ = p->pso.Get();   // matches what the line above just bound.
    dev_->fovValid_ = false;          // pipeline changed; see fovValid_.
    dev_->dbValid_ = false;           // root signature change discards all bound arguments.
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

    // One generic heap: elide redundant binds the same way boundRootSig_/boundPso_ do.
    ID3D12DescriptorHeap* const heap = res_->heap_.Get();
    if (dev_->boundHeap_ != heap) {
        ID3D12DescriptorHeap* heaps[] = {heap};
        dev_->cmdList_->SetDescriptorHeaps(1, heaps);
        dev_->boundHeap_ = heap;
    }
    dev_->boundRootSig_ = nullptr;
    dev_->boundPso_ = nullptr;
    // Invalidate table 0 cache only; table 1 (per-material) has its own cache.
    if (table == 0) dev_->fovValid_ = false;

    // Check once per shape, not per draw.
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

    // fi is the frame index just synced in beginFrame; gpuBase[fi] is safe from GPU reads.
    const u32 fi = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    if (s->gpuVersion[fi] != s->version) {
        dev_->device_->CopyDescriptorsSimple(s->srvCount + s->uavCount, res_->cpuSlot(s->gpuBase[fi]),
                                             res_->stagingCpu(s->stageBase), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s->gpuVersion[fi] = s->version;
    }

    if (s->srvCount && pipe_->srvParam[table] >= 0) {
        const D3D12_GPU_DESCRIPTOR_HANDLE h = res_->gpuSlot(s->gpuBase[fi]);
        if (pipe_->compute) dev_->cmdList_->SetComputeRootDescriptorTable(static_cast<UINT>(pipe_->srvParam[table]), h);
        else                dev_->cmdList_->SetGraphicsRootDescriptorTable(static_cast<UINT>(pipe_->srvParam[table]), h);
    }
    if (s->uavCount && pipe_->uavParam[table] >= 0) {
        const D3D12_GPU_DESCRIPTOR_HANDLE h = res_->gpuSlot(s->gpuBase[fi] + s->srvCount);
        if (pipe_->compute) dev_->cmdList_->SetComputeRootDescriptorTable(static_cast<UINT>(pipe_->uavParam[table]), h);
        else                dev_->cmdList_->SetGraphicsRootDescriptorTable(static_cast<UINT>(pipe_->uavParam[table]), h);
    }
}

// Binds the ray path's bindless texture table, for a pipeline that declared one.
void D3D12RenderContext::setBindlessTable(BindlessTableHandle table) {
    if (!pipe_ || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setBindlessTable before setPipeline"); return; }
    // No-op on a pipeline without a bindless table (caller doesn't need to know the variant).
    if (pipe_->bindlessParam < 0) return;
    const u32 cap = res_->bindlessTableCapacity(table);
    if (cap == 0) { AVER_ERROR("[RHI.D3D12] setBindlessTable with an invalid table handle"); return; }

    // Heap must be bound before the table (in case setBindingSet hasn't been called yet).
    ID3D12DescriptorHeap* const heap = res_->heap_.Get();
    if (dev_->boundHeap_ != heap) {
        ID3D12DescriptorHeap* heaps[] = {heap};
        dev_->cmdList_->SetDescriptorHeaps(1, heaps);
        dev_->boundHeap_ = heap;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE h = res_->gpuSlot(res_->bindlessHeapBase(table));
    if (pipe_->compute) dev_->cmdList_->SetComputeRootDescriptorTable(static_cast<UINT>(pipe_->bindlessParam), h);
    else                dev_->cmdList_->SetGraphicsRootDescriptorTable(static_cast<UINT>(pipe_->bindlessParam), h);
}

// Writes root constants into a slot the pipeline declared as root constants.
void D3D12RenderContext::setConstants(u32 slot, const void* data, u32 dwords) {
    if (!pipe_ || slot >= kMaxConstantSlots || !dev_->cmdList_) { AVER_ERROR("[RHI.D3D12] setConstants without a pipeline"); return; }
    const i32 param = pipe_->slotParam[slot];
    const u32 declared = pipe_->slotDwords[slot];
    if (param < 0 || declared == 0) {
        AVER_ERROR("[RHI.D3D12] setConstants: slot {} declares constantDwords 0, so it is a root CBV -- use setConstantBuffer", slot);
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
        AVER_ERROR("[RHI.D3D12] setConstantBuffer: slot {} declares {} root constants, not a CBV -- use setConstants",
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

// Binds the sticky per-draw state (table 1), with optional redundant-state elision via AVER_D3D12_ELIDE_DRAW_BINDING.
void D3D12RenderContext::applyDrawBinding() {
    if (!pipe_) return;

    // Read once per process; when disabled, function is unoptimized.
    static const bool kElide = [] {
        const char* v = std::getenv("AVER_D3D12_ELIDE_DRAW_BINDING");
        return v && v[0] != '\0' && v[0] != '0';
    }();

    if (drawSet_ && pipe_->srvParam[1] >= 0) {
        // Skip re-binding if the descriptor table hasn't changed.
        const bool elided = kElide && dev_->dbValid_ && dev_->dbSet_ == drawSet_;
        if (!elided) {
            setBindingSet(drawSet_, 1);
            if (kElide) dev_->dbSet_ = drawSet_;
        }
    }
    if (drawConstantBytes_ && pipe_->slotParam[kDrawConstantRegister] >= 0 &&
        pipe_->slotDwords[kDrawConstantRegister] == 0) {
        // Skip re-uploading if the constant block is unchanged.
        const bool elided = kElide && dev_->dbValid_ &&
                            dev_->dbConstantBytes_ == drawConstantBytes_ &&
                            std::memcmp(dev_->dbConstants_, drawConstants_, drawConstantBytes_) == 0;
        if (!elided) {
            setConstantBuffer(kDrawConstantRegister, drawConstants_, drawConstantBytes_);
            if (kElide) {
                dev_->dbConstantBytes_ = drawConstantBytes_;
                std::memcpy(dev_->dbConstants_, drawConstants_, drawConstantBytes_);
            }
        }
    }
    if (kElide) dev_->dbValid_ = true;
}

// Copies `bytes` into this frame's upload ring and returns their GPU address.
// Ring grows at frame boundary only (mid-frame reallocation would hand the GPU freed memory).
D3D12_GPU_VIRTUAL_ADDRESS D3D12RenderContext::ringAlloc(const void* data, u32 bytes) {
    if (!data || bytes == 0) return 0;
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;

    // New frame: reset cursor and apply requested growth (safe after kFrameCount frames).
    if (ringEpoch_ != dev_->frameSerial_) {
        ringEpoch_ = dev_->frameSerial_;
        ringUsed_[f] = 0;
        if (ringWanted_ > ringBytes_[f]) {
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
        // Request headroom for upcoming allocations this frame.
        const u64 want = (offset + size) * 2ull;
        if (want > ringWanted_) ringWanted_ = want > kRhiRingMaxBytes ? kRhiRingMaxBytes : want;
        // Log once per frame, not per call.
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
        // Pipeline not built with GraphicsPipelineDesc::instanced; fall back to one draw per instance.
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

// Dispatches an amplification+mesh-shader pipeline over one cluster cut.
// Cluster arrays are pre-bound; this supplies group count and vertex buffer.
void D3D12RenderContext::dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) {
    if (!pipe_ || !pipe_->mesh || !pipe_->amplification) {
        AVER_ERROR("[RHI.D3D12] dispatchMeshClusters without an amplification-shader pipeline");
        return;
    }
    if (!dev_->cmdList6_) { AVER_ERROR("[RHI.D3D12] DispatchMesh is unavailable on this command list"); return; }
    if (clusterCount == 0) return;
    applyDrawBinding();
    // Root sig reserves msVertexParam/msIndexParam/msCountParam; bind all three to avoid uninitialized args.
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

// Layout for a texture<->buffer copy of one mip. Use GetCopyableFootprints, not width*bpp arithmetic.
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
    // Validated here (not in debug layer) because mismatched CopyResource silently corrupts on release.
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
    if (!b->scratch) {
        b->scratch = makeAsBuffer(dev_->device_.Get(), b->scratchBytes, D3D12_RESOURCE_STATE_COMMON);
        if (!b->scratch) { AVER_ERROR("[RHI.D3D12] buildBlas: no build scratch for BLAS {}", h); return; }
    }
    // A static BLAS's scratch is retired behind this frame's fence once the build is recorded.
    struct ReleaseScratch {
        D3D12ResourceFactory* res; RhiBlas* b;
        ~ReleaseScratch() { if (!b->allowUpdate && b->scratch) { res->retire(b->scratch); b->scratch.Reset(); } }
    } releaseScratch{res_, b};
    if (!b->geometries.empty()) {
        // createBlasMulti: every geometry at once, into the allocation its prebuild query sized.
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geos;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        if (!blasMultiInputs(dev_->meshes_, b->geometries, geos, bd.Inputs)) {
            AVER_ERROR("[RHI.D3D12] buildBlas: multi-geometry BLAS {} names a mesh that is gone", h);
            return;
        }
        bd.ScratchAccelerationStructureData = b->scratch->GetGPUVirtualAddress();
        bd.DestAccelerationStructureData = b->as->GetGPUVirtualAddress();
        dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        bar.UAV.pResource = b->as.Get();
        dev_->cmdList_->ResourceBarrier(1, &bar);
        b->built = true;
        return;
    }
    const GpuMesh& m = dev_->meshes_[b->mesh - 1];

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = blasInputs(m, geo, b->allowUpdate);
    bd.ScratchAccelerationStructureData = b->scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = b->as->GetGPUVirtualAddress();
    dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = b->as.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
    b->built = true;
    b->builtVertexCount = m.vertexCount;
    b->builtIndexCount = m.indexCount;
}

// Updates `h` in place from its mesh's current vertices when eligible, otherwise falls back to a
// full buildBlas -- see RHIResources.hpp's refitBlas contract.
bool D3D12RenderContext::refitBlas(BlasHandle h) {
    RhiBlas* b = res_->blas(h);
    if (!b) { AVER_ERROR("[RHI.D3D12] refitBlas with an invalid handle"); return false; }
    if (!dev_->cmdList4_ || !dev_->device5_) { AVER_ERROR("[RHI.D3D12] refitBlas without ray-tracing support"); return false; }
    if (b->mesh == 0 || b->mesh > dev_->meshes_.size()) return false;
    // A createBlasMulti structure is never updatable: the refitBlas contract's full-build fallback.
    if (!b->geometries.empty()) { buildBlas(h); return false; }
    const GpuMesh& m = dev_->meshes_[b->mesh - 1];

    // Not eligible if not updatable, never built, or counts changed.
    const bool countsChanged = m.vertexCount != b->builtVertexCount || m.indexCount != b->builtIndexCount;
    if (!b->allowUpdate || !b->built || countsChanged) {
        // Buffers sized at creation; a genuine count change risks GPU corruption. Refit is for compute-skinned meshes (position only).
        if (b->built && countsChanged) {
            D3D12_RAYTRACING_GEOMETRY_DESC geo{};
            const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = blasInputs(m, geo, b->allowUpdate);
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            dev_->device5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
            const u64 scratchBytes = b->allowUpdate
                ? std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes)
                : info.ScratchDataSizeInBytes;
            if (info.ResultDataMaxSizeInBytes > b->as->GetDesc().Width || scratchBytes > b->scratchBytes) {
                AVER_ERROR("[RHI.D3D12] refitBlas: mesh {} moved from {}v/{}i to {}v/{}i, past what its BLAS "
                           "was allocated for at creation -- rebuilding it in place would write past that "
                           "allocation, so this refit is refused; the caller must destroy and recreate the BLAS",
                           b->mesh, b->builtVertexCount, b->builtIndexCount, m.vertexCount, m.indexCount);
                return false;
            }
        }
        buildBlas(h);
        return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = blasInputs(m, geo, /*allowUpdate=*/true);
    bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    bd.ScratchAccelerationStructureData = b->scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = b->as->GetGPUVirtualAddress();
    // In place: DXR allows Source == Dest for an update, and that is what "refit" means here.
    bd.SourceAccelerationStructureData = b->as->GetGPUVirtualAddress();
    dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = b->as.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
    return true;
}

// Packs `instances` into this frame's instance buffer and records signature for refitTlas eligibility.
u32 D3D12RenderContext::packTlasInstances(RhiTlas& t, const TlasInstance* instances, u32 count, const char* caller,
                                          std::vector<RhiTlasSlot>& outSlots) {
    if (count > t.maxInstances) {
        AVER_WARN("[RHI.D3D12] {}: {} instances clamped to the {} this TLAS was sized for", caller, count, t.maxInstances);
        count = t.maxInstances;
    }
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    outSlots.clear();
    if (!t.instancePtr[f]) return 0;
    outSlots.reserve(count);

    auto* dst = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(t.instancePtr[f]);
    u32 written = 0;
    for (u32 i = 0; i < count && instances; ++i) {
        const RhiBlas* b = res_->blas(instances[i].blas);
        if (!b || !b->as) { AVER_WARN("[RHI.D3D12] {}: instance {} names an invalid BLAS", caller, i); continue; }
        // Rejected rather than truncated: InstanceID is a 24-bit bitfield, so a larger value would
        // silently alias onto another instance's id and a hit would resolve to the wrong geometry.
        if (instances[i].instanceId > kMaxTlasInstanceId) {
            AVER_ERROR("[RHI.D3D12] {}: instance {} has id {} which does not fit in 24 bits; "
                       "it is dropped rather than aliased onto another instance",
                       caller, i, instances[i].instanceId);
            continue;
        }
        const D3D12_RAYTRACING_INSTANCE_DESC id = toInstanceDesc(instances[i], b->as->GetGPUVirtualAddress());
        dst[written++] = id;
        // The mask as the GPU keeps it (InstanceMask is 8 bits), so bits it never sees can't defeat a refit.
        outSlots.push_back({id.AccelerationStructure, id.Flags, instances[i].mask & 0xFFu});
    }
    return written;
}

// Packs the instance buffer and records a top-level acceleration structure build.
void D3D12RenderContext::buildTlas(TlasHandle h, const TlasInstance* instances, u32 count) {
    RhiTlas* t = res_->tlas(h);
    if (!t) { AVER_ERROR("[RHI.D3D12] buildTlas with an invalid handle"); return; }
    if (!dev_->cmdList4_ || !dev_->device5_) { AVER_ERROR("[RHI.D3D12] buildTlas without ray-tracing support"); return; }
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    if (!t->instancePtr[f]) return;

    const u32 written = packTlasInstances(*t, instances, count, "buildTlas", t->pendingSlots);
    const u32 staticUsed = usableStaticPrefix(*t);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = tlasInputs(staticUsed + written, t->allowUpdate);
    in.InstanceDescs = tlasBuildDescs(*t, staticUsed, written);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.ScratchAccelerationStructureData = t->scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = t->as->GetGPUVirtualAddress();
    {
        // Earlier ray queries this frame may still be reading the TLAS; order them before the rewrite.
        D3D12_RESOURCE_BARRIER pre{};
        pre.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        pre.UAV.pResource = t->as.Get();
        dev_->cmdList_->ResourceBarrier(1, &pre);
    }
    dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = t->as.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
    t->built = true;
    t->builtStatic = staticUsed;
    t->builtSlots.swap(t->pendingSlots);
}

// Check prefix BLASes here (destroyMesh can invalidate them) before build.
u32 D3D12RenderContext::usableStaticPrefix(RhiTlas& t) {
    if (t.staticCount == 0 || !res_->bufferResource(t.staticDescs)) return 0;
    for (BlasHandle b : t.staticBlases) {
        if (res_->blas(b)) continue;
        if (!t.staticBrokenLogged) {
            AVER_ERROR("[RHI.D3D12] TLAS static prefix names BLAS {}, destroyed since it was set -- its {} "
                       "instance(s) are left out of every build until the prefix is replaced or removed",
                       b, t.staticCount);
            t.staticBrokenLogged = true;
        }
        return 0;
    }
    return t.staticCount;
}

// Copy lands at slot staticCount; barriers order it between prior reads and this build.
D3D12_GPU_VIRTUAL_ADDRESS D3D12RenderContext::tlasBuildDescs(RhiTlas& t, u32 staticUsed, u32 written) {
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    if (staticUsed == 0) return t.instances[f]->GetGPUVirtualAddress();
    ID3D12Resource* descs = res_->bufferResource(t.staticDescs);
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (written) {
        if (t.staticDescsState != D3D12_RESOURCE_STATE_COPY_DEST) {
            const D3D12_RESOURCE_BARRIER toCopy = transition(descs, t.staticDescsState, D3D12_RESOURCE_STATE_COPY_DEST);
            dev_->cmdList_->ResourceBarrier(1, &toCopy);
            t.staticDescsState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        dev_->cmdList_->CopyBufferRegion(descs, static_cast<u64>(staticUsed) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                         t.instances[f].Get(), 0,
                                         static_cast<u64>(written) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    }
    if (t.staticDescsState != kRead) {
        const D3D12_RESOURCE_BARRIER toRead = transition(descs, t.staticDescsState, kRead);
        dev_->cmdList_->ResourceBarrier(1, &toRead);
        t.staticDescsState = kRead;
    }
    return descs->GetGPUVirtualAddress();
}

// Updates `h` in place when the filtered instance list matches the last build/refit's signature
// exactly (same count, every slot naming the same BLAS with the same flags and mask -- transforms
// and instance ids may differ), otherwise records a full build instead. See RHIResources.hpp's
// refitTlas contract. REQUIRED after any buildBlas/refitBlas of a BLAS this TLAS references.
bool D3D12RenderContext::refitTlas(TlasHandle h, const TlasInstance* instances, u32 count) {
    RhiTlas* t = res_->tlas(h);
    if (!t) { AVER_ERROR("[RHI.D3D12] refitTlas with an invalid handle"); return false; }
    if (!dev_->cmdList4_ || !dev_->device5_) { AVER_ERROR("[RHI.D3D12] refitTlas without ray-tracing support"); return false; }
    const u32 f = dev_->frameIndex_ < kFrameCount ? dev_->frameIndex_ : 0;
    if (!t->instancePtr[f]) return false;

    std::vector<RhiTlasSlot>& slots = t->pendingSlots;
    const u32 written = packTlasInstances(*t, instances, count, "refitTlas", slots);
    const u32 staticUsed = usableStaticPrefix(*t);

    // Prefix slots are identical by construction; compare per-frame slots and prefix length only.
    bool eligible = t->allowUpdate && t->built && slots.size() == t->builtSlots.size() &&
                    staticUsed == t->builtStatic;
    for (size_t i = 0; eligible && i < slots.size(); ++i) {
        const RhiTlasSlot& a = slots[i];
        const RhiTlasSlot& prev = t->builtSlots[i];
        if (a.blasVa != prev.blasVa || a.flags != prev.flags || a.mask != prev.mask) eligible = false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = tlasInputs(staticUsed + written, t->allowUpdate);
    in.InstanceDescs = tlasBuildDescs(*t, staticUsed, written);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.ScratchAccelerationStructureData = t->scratch->GetGPUVirtualAddress();
    bd.DestAccelerationStructureData = t->as->GetGPUVirtualAddress();
    {
        // Earlier ray queries this frame may still be reading the TLAS; order them before the rewrite.
        D3D12_RESOURCE_BARRIER pre{};
        pre.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        pre.UAV.pResource = t->as.Get();
        dev_->cmdList_->ResourceBarrier(1, &pre);
    }
    if (eligible) {
        bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        // In place: DXR allows Source == Dest for an update, and that is what "refit" means here.
        bd.SourceAccelerationStructureData = t->as->GetGPUVirtualAddress();
    }
    dev_->cmdList4_->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bar.UAV.pResource = t->as.Get();
    dev_->cmdList_->ResourceBarrier(1, &bar);
    t->built = true;
    t->builtStatic = staticUsed;
    t->builtSlots.swap(t->pendingSlots);
    return eligible;
}

namespace {
// True when a barrier names the terminal AccelerationStructure state, which D3D12 rejects.
bool rejectAsState(ResourceState from, ResourceState to, const char* what) {
    if (from != ResourceState::AccelerationStructure && to != ResourceState::AccelerationStructure) return false;
    AVER_ERROR("[RHI.D3D12] {}: AccelerationStructure is terminal and cannot be transitioned", what);
    return true;
}

// Track texture barrier state transitions (debug only).
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

// Track buffer barrier state transitions (debug only).
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
    if (!t->res) AVER_ERROR("[RHI.D3D12] textureBarrier on destroyed texture {} '{}'", h,
                            t->desc.debugName ? t->desc.debugName : "");
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

// Opens a debug marker region and nested GPU timing span (prefer ScopedGpuStat when possible).
void D3D12RenderContext::pushMarker(const char* label) {
    if (!label || !dev_->cmdList_) return;
    dev_->cmdList_->BeginEvent(1, label, static_cast<UINT>(std::strlen(label) + 1));
    // Label is always a string literal (safe to store pointer, allocation-free); parent tracks nesting.
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
// Defined here (not in header) to access D3D12Device's full definition for the static_cast.
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
