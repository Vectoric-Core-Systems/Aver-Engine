// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// DirectX 12 backend for Aver.RHI: device, swapchain, scene pipelines, the camera post chain,
// and the generic resource factory and render context. Hand-rolled D3D12 structs (no d3dx12.h).
#include "aver/rhi/RHI.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/ShaderCacheSweep.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
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
#include <initializer_list>   // D3D12ResourceFactory::uploadBuffers' parameter (W4 Default-heap meshes)
#include <string>
#include <utility>
#include <vector>

// The abstract seam a UI toolkit's D3D12 backend plugs into. This file has no ImGui include and no
// ImGui symbol anywhere in it -- see UiBackend.hpp for why, and modules/rhi.d3d12.imgui for where
// Dear ImGui itself now lives.
#include "aver/rhi/d3d12/UiBackend.hpp"

using Microsoft::WRL::ComPtr;

namespace aver::rhi {

// How much disk the shader blob cache may keep. 256 MB is roughly two orders of magnitude more than
// one full set of this engine's shaders, so an ordinary user never reaches it while a developer's
// months of accumulated variants are trimmed to a working set rather than to nothing. Evicting to
// zero on every launch would defeat the cache; not evicting at all is what this replaces. See
// aver/rhi/ShaderCacheSweep.hpp for why the bound is bytes and the order is oldest-first.
static constexpr u64 kShaderCacheBudgetBytes = 256ull * 1024ull * 1024ull;
namespace {

constexpr u32 kFrameCount = 2;
constexpr u32 kDefaultSampleCount = 4;
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
// Typeless RESOURCE format so the depth buffer carries both a D32_FLOAT DSV (kDepthFormat) and an
// R32_FLOAT SRV (IDevice::sceneDepthTexture(), read by modules/occlusion's HZB seed) -- the same
// "one resource, two views" trick as VoxiRenderer::createShadowResources, extended to MULTISAMPLED.
// See createDepthBuffer's comment for the DSV-side consequence (an explicit view desc, not nullptr).
constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;

// Scene target format: linear HDR radiance, which the post chain tonemaps into the backbuffer.
constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// G-buffer target formats -- see IDevice::setGBufferEnabled (RHI.hpp) for units; nothing in this
// engine reads them yet, this backend only writes them. Must match toDxgiFormat's
// Format::RG16F/R32Float/RGB10A2Unorm cases one-for-one, which gBufferVelocityTexture() etc. rely on.
constexpr DXGI_FORMAT kGBufVelocityFormat    = DXGI_FORMAT_R16G16_FLOAT;       // Format::RG16F
constexpr DXGI_FORMAT kGBufViewZFormat       = DXGI_FORMAT_R32_FLOAT;          // Format::R32Float
constexpr DXGI_FORMAT kGBufNormalRoughFormat = DXGI_FORMAT_R10G10B10A2_UNORM;  // Format::RGB10A2Unorm

// G-buffer "nothing here" clear values, shared by createGBufferTargets' optimised D3D12_CLEAR_VALUE
// and beginFrame's per-frame clear -- one constant per target so the two can't drift apart. See
// createGBufferTargets for why each value was chosen.
constexpr f32 kGBufVelocityClear[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
constexpr f32 kGBufViewZClear[4]       = {0.0f, 0.0f, 0.0f, 0.0f};
constexpr f32 kGBufNormalRoughClear[4] = {0.5f, 0.5f, 0.5f, 0.0f};

// Bloom pyramid depth cap.
constexpr u32 kMaxBloomMips = 6;
// Descriptor triples the post heap holds: prefilter, histogram, composite, then one per
// downsample/upsample; each triple contiguous, as a root table must be. +1 for
// kPostTripleCompositeUpscaled: AverSR's composite reads presentHdrTex_ instead of `scene`, and a
// contiguous triple can't be rewritten per frame without racing frames still in flight.
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
    //
    // Blob cache: MEASURED BEFORE IT WAS BUILT -- 64 compiles, 5,601ms on a PTTest launch, no cache
    // of any kind. Key is the whole input: source, -D list, entry, target profile, a hand-bumped
    // format version, and a hash of every shader FILE (see below).
    //
    // "COMPOSED SOURCE" STOPPED BEING THE WHOLE INPUT and this comment said it was. When shaders were
    // C++ string literals, `src` really did contain every prelude, so a changed prelude changed the
    // key. Once shaders became files that pull in what they need, `src` is the top-level text alone
    // -- and an edit to shared_prelude.hlsl (the sky, the fog, the tonemap, the frame constant
    // buffer) left every key identical. The cache hit, DXC was never asked, and the edit did nothing,
    // silently, for every shader. That is a correctness hole rather than a stale-cache annoyance, and
    // it cost a debugging session before it was found: a sky change measured as a no-op three times.
    //
    // kCacheVersion is the dxcompiler.dll escape hatch: a newer DXC can emit different DXIL for
    // identical input, invisible to the cache. Bump it when the shipped compiler changes.
    static constexpr u32 kCacheVersion = 1;

    static u64 cacheKey(const char* src, const char* entry, const char* target, const std::vector<std::string>& defs) {
        u64 h = 0xcbf29ce484222325ull;
        const auto mix = [&h](const char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) { h ^= static_cast<unsigned char>(p[i]); h *= 0x100000001b3ull; }
            h ^= 0xffu; h *= 0x100000001b3ull;   // a field separator, so "ab"+"c" != "a"+"bc"
        };
        const u32 v = kCacheVersion;
        mix(reinterpret_cast<const char*>(&v), sizeof v);
        // Every shader file, not just this one's own text -- see the note above. Memoised, so this is
        // one directory walk per process however many pipelines are built.
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
        // std::filesystem JOINS THIS rather than pasting a separator into a literal -- an earlier
        // "\ShaderCache\\" had an invalid \S escape MSVC silently dropped, so the cache landed in a
        // SIBLING directory (AverEngineShaderCache) instead: it worked perfectly, in the wrong place.
        return (std::filesystem::path(dir) / "ShaderCache" / name).string();
    }

    // Running totals for shader-compile cost, since nothing measured it before. Reported on a
    // power-of-two cadence -- same shape as VoxiRenderer's translucent-draw census -- for a total
    // without a line per compile.
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

        DxcBuffer buf{src, std::strlen(src), DXC_CP_UTF8};
        std::vector<LPCWSTR> args = {
            L"-E", wEntry.c_str(),
            L"-T", wTarget.c_str(),
            L"-Zpr",
            L"-HV", L"2021",
            // ANGLED INCLUDES. A custom IDxcIncludeHandler is consulted for `#include "x"` on its
            // own, because a quoted include searches the including file's own directory -- but
            // `#include <x>` searches ONLY the -I list, and with no -I at all that list is empty,
            // so DXC never asks the handler and reports `file not found with <angled> include; use
            // "quotes" instead`. That is not a style note this engine can act on: the angled
            // includes are inside VENDORED third-party HLSL (RTXDI's Utils/RandomSamplerState.hlsli
            // includes <Rtxdi/Utils/Math.hlsli>), and rewriting a vendored tree to suit us is the
            // thing third_party/*/AVER_README.md exists to avoid.
            //
            // "." IS NOT A FILESYSTEM PATH HERE. It only gives the angled search a single entry to
            // form candidates from; the candidate ("./Rtxdi/Utils/Math.hlsli") still goes through
            // DxcShaderInclude, which strips the "./" and resolves it against bin/shaders exactly
            // as it does a quoted one. So this changes WHICH includes reach the handler, not where
            // the handler looks -- --shader-source, the cache and hot reload all still apply.
            L"-I", L".",
        };
        // ONE DEFINE IS A FLAG, NOT A DEFINE: AVER_ENABLE_16BIT_TYPES asks DXC for real fp16
        // (-enable-16bit-types) for THIS compile only, and is consumed here rather than passed on.
        //
        // WHY IT IS OPT-IN PER SHADER AND NOT A GLOBAL ARGUMENT. That switch changes what `half`
        // MEANS: without it DXC widens half to fp32, with it half is genuinely 16-bit. There are 93
        // uses of half/min16float across this engine's shaders -- water, the material prelude, the
        // path tracer's denoiser among them -- every one of which would silently change precision
        // the day the flag went on globally. That is a renderer-wide numerical change wearing the
        // costume of a build flag, and it would not show up as a compile error anywhere.
        //
        // A shader that genuinely needs fp16 TYPES (float16_t and friends -- RTXGI's SHaRC packs
        // its resolved radiance as float16_t4, and cannot compile at all without this) asks for it
        // by name and gets it alone. Requires SM 6.2 or better; this engine targets 6.5.
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
        // A real include handler (rhi::shaderFile(), not DXC's default filesystem one -- see
        // DxcShaderInclude.hpp: --shader-source, the cache, CRLF normalisation, hot reload) so a
        // .hlsl can #include another instead of relying on concatenated preludes.
        // A cache HIT skips DXC entirely; any read failure falls through to a normal compile -- a
        // cache must never be the reason a pipeline fails to build.
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
        // Written best-effort and never checked: a read-only install, a full disk or a race with
        // another process losing this write costs one recompile next launch and nothing else.
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

// imguiWndProc/UiSrvPool/uiSrvAlloc/uiSrvFree (once gated on AVER_WITH_IMGUI) moved to
// modules/rhi.d3d12.imgui/src/ImGuiUiBackend.cpp -- no ImGui symbol left here. The Win32 thunk and
// descriptor-pool sharing now go through d3d12::IUiBackend; see uiBackendWndProcThunk and
// D3D12ResourceFactory::uiDescriptor for where each half landed.

// Writes the shading model and its parameters into the tail of a per-draw b1 block.
//
// `unlit` REACHES THE MATERIAL SHADER, which gMaterial.z alone does not. Two different shaders read
// two different fields for the same idea: the backend's own fallback tests gMaterial.z (block[22]),
// while anything built on the material prelude branches on gShadingModel. Writing only the first
// left an overriding feature's shader -- Voxi's, i.e. the one that actually draws scenes -- with no
// way to see the mode at all, which is why unlit had to be diverted away from it entirely.
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

// The scene/sky/line shading now lives in modules/rhi/shaders/scene.hlsl, loaded through
// rhi::shaderFile() and shared by both backends -- see that file for why it stopped being two
// hand-synced copies.

// Shared prelude + this backend's shaders, cached and KEYED ON shaderFileRevision() rather than a
// plain function-local static -- the old form cached ONCE, so hot reload kept rebuilding pipelines
// from the same stale string. RHIShaders.cpp's sharedShaderPrelude() warns of exactly this shape;
// this was one of the sites that hadn't heeded it.
const std::string& sceneShaderSource() {
    static std::string src;
    static u64 built = ~0ull;
    if (built != shaderFileRevision()) {
        src = std::string(sharedShaderPrelude()) + shaderFile("scene.hlsl");
        built = shaderFileRevision();
    }
    return src;
}

// PerFrameCB and PostCB now live in aver/rhi/FrameConstants.hpp -- ONE definition, shared with the
// Vulkan backend, which used to keep a hand-copied twin of both. See that header for why.

// Per-frame upload ring for the block above.
constexpr u32 kPostConstantRingBytes = 16 * 1024;

// The one description of rhi::MeshVertex to D3D12; every IA pipeline shares it. Uses offsetof, NOT
// 0/12/24 literals -- Vulkan's twin already derives these (VulkanCommon.hpp), so hardcoded literals
// here would let MeshVertex's field order drift out from under D3D12 alone (NORMAL reading what is
// now UV) while every static_assert and Vulkan stayed green.
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
    // Non-zero when a compute pass writes these vertices rather than this device's own upload -- see
    // IDevice::createSkinTargetMesh. `vb` points at the same resource either way; this only records
    // WHO writes it.
    BufferHandle vbBuffer = 0;
    // Index buffer as an RHI buffer, plus vertexCount below -- so a SHADER can read this mesh's
    // geometry (a ray hit has only a triangle index; reconstructing it needs descriptors over both
    // streams). createMesh used to allocate raw committed resources with no RhiBuffer entry, so no
    // descriptor could ever name them.
    BufferHandle ibBuffer = 0;
    u32 vertexCount = 0;
    // Whether a COMPUTE PASS writes these vertices, vs. merely living in an RHI buffer. Needed once
    // createMesh started routing every mesh through the factory: every mesh then had a vbBuffer, so
    // meshVertexBuffer's "zero for an ordinary mesh" contract broke and the renderer rebuilt every
    // static mesh's acceleration structure every frame. Set by createSkinTargetMesh, and by
    // createPosedPartMesh (whose vertices ARE a skin target's).
    bool computeWritten = false;
    // Local-space bounding sphere -- AABB midpoint and the distance to a corner, computed once in
    // createMesh. See IDevice::meshBounds for why a corner rather than the farthest actual vertex.
    f32 boundsCentre[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsRadius = 0.0f;
    // The box the sphere above was derived from, which createMesh used to compute and discard. A
    // sphere is right for a frustum cull and wrong for "is this point inside the volume" -- the
    // sphere around a wide shallow pool bulges above its surface, answering yes while standing on
    // the deck. Six floats per mesh buys an exact answer.
    f32 boundsMin[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsMax[3] = {0.0f, 0.0f, 0.0f};

    // Index-buffer sharing exists only for createSkinTargetMesh: a skin target owns its vertices but
    // SHARES its source's indices (skinning moves vertices, never renumbers triangles). Freeing
    // twice, or freeing one still in use, is silent corruption. So ownership is recorded: `ibOwned`
    // is false on a skin target (never frees); `ibShares` counts live sharers on the SOURCE (refuses
    // to be destroyed while any remain); `ibSource` is how a skin target finds it to decrement.
    bool ibOwned = true;
    u32  ibShares = 0;
    MeshHandle ibSource = 0;

    // W11: the mirror-image sharing relationship, for createMeshSharingVertices -- an LOD ladder's
    // coarser levels reuse the SAME vertex stream (positions/normals/uvs never change across an
    // asset's LODs) and thin out only which triangles reference it, the inverse split from a skin
    // target's own-vertices/shared-indices shape just above. `vbOwned` is false on a sharer (never
    // frees vb/vbBuffer); `vbShares` counts live sharers on the ROOT mesh (refuses destruction while
    // any remain); `vbSource` is how a sharer finds its root to decrement on destruction. Deliberately
    // NOT reusing ibSource/ibShares/ibOwned for this: a mesh can be both a skin target's index-sharing
    // SOURCE and a vertex-sharing ROOT at once (nothing here rules that combination out), and one
    // field per relationship is what keeps those two counts from being able to collide.
    bool vbOwned = true;
    u32  vbShares = 0;
    MeshHandle vbSource = 0;

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
        // The G-buffer's two formats (RHIResources.hpp, added alongside IDevice::setGBufferEnabled).
        // MISSING HERE would not fail loudly: callers already treat DXGI_FORMAT_UNKNOWN as "no DXGI
        // equivalent" and press on (see buildInputLayout above), so an unhandled case would silently
        // reach a texture-create or SRV/UAV call instead of refusing to compile.
        case Format::RG16F:          return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGB10A2Unorm:   return DXGI_FORMAT_R10G10B10A2_UNORM;
        // NRD's pool formats -- see the enum's own note in RHIResources.hpp.
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

    // Plugs a UI toolkit's D3D12 backend in -- see installUiBackend's comment (UiBackend.hpp) for why
    // and who calls it. NOT part of IDevice: reachable only via that free function's own backend()
    // check. Non-owning, same as setUpscaler.
    void setUiBackend(d3d12::IUiBackend* backend) { uiBackend_ = backend; }

    Backend backend() const override { return Backend::D3D12; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }

    // ----- generic RHI surface (render-feature modules) -----
    // The SCENE colour format, which is what a feature builds its scene pipelines against.
    Format backbufferFormat() const override { return fromDxgiFormat(kSceneColorFormat); }
    Format depthFormat() const override { return fromDxgiFormat(kDepthFormat); }
    // OUT-OF-LINE: calls into D3D12ResourceFactory, whose complete type isn't visible yet here. See
    // RHI.hpp's own comment on this method and kDepthResourceFormat's above.
    TextureHandle sceneDepthTexture() override;
    TextureHandle sceneColorBackdropTexture() override { return blendBackdropTex_; }

    // G-buffer: velocity + view-space depth + normal/roughness -- see IDevice's comment block
    // (RHI.hpp) for the full contract. OUT-OF-LINE, same reason as sceneDepthTexture() above.
    void setGBufferEnabled(bool on) override;
    bool gBufferEnabled() const override { return gbufferEnabled_; }
    TextureHandle gBufferVelocityTexture() override;
    TextureHandle gBufferViewZTexture() override;
    TextureHandle gBufferNormalRoughnessTexture() override;
    // Row-major, row-vector, same convention as setCamera's `viewProj` -- see setCamera's comment
    // below for when this snapshot is taken and why it isn't a second copy of VoxiRenderer's own
    // gPrevViewProj.
    bool gBufferPrevViewProj(f32 out[16]) const override {
        if (!gbufferEnabled_) return false;
        if (out) std::memcpy(out, prevViewProj_, sizeof(prevViewProj_));
        return true;
    }
    bool gBufferHistoryInvalid() const override { return !gbufferEnabled_ || gbufHistoryInvalid_; }

    // OUT-OF-LINE: queries adapter3_, which init() only fills in after D3D12CreateDevice succeeds.
    // See VideoMemoryInfo's own comment (RHI.hpp) for the field meanings.
    VideoMemoryInfo videoMemory() const override;

    IResourceFactory* resources() override;
    // Same context object drawMesh()'s overridesScenePipeline branch uses internally, exposed so a
    // caller can interleave its own setPipeline/dispatchMeshClusters calls for a SUBSET of instances
    // in the same frame. OUT-OF-LINE, same reason as sceneDepthTexture() above (D3D12RenderContext
    // is only forward-declared this early).
    IRenderContext* renderContext() override;
    // NON-owning. Registering the same feature twice would double every hook, so it is ignored.
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    // NON-owning. Null (the default) keeps the untouched single-pass composite path -- what makes
    // AverSR Off bit-identical to a build without the module (docs/AVERSR.md's invariant).
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
        // Caller thinks in present-space pixels (editor confines the scene to a dockspace sub-rect),
        // but the target these address is the SCENE one, smaller than the backbuffer whenever
        // renderScale_ < 1 -- so the rect is scaled into scene-space here once, not at every reader.
        // Identity at renderScale_ == 1.0 (scaleToSceneW/H(v) == v exactly).
        vpX_ = scaleToSceneW(x);  vpY_ = scaleToSceneH(y);
        vpW_ = scaleToSceneW(cw); vpH_ = scaleToSceneH(ch);
        if (vpW_ == 0) vpW_ = 1;
        if (vpH_ == 0) vpH_ = 1;
    }

    // Scene-space throughout, and that is why this is safe to hand out raw: vpW_/vpH_ were already
    // converted above, sceneWidth_/sceneHeight_ are the same space, and a RATIO of two values in
    // one space is the ratio in every space. See IDevice::viewportAspect for why it exists.
    f32 viewportAspect() const override {
        const u32 w = vpW_ ? vpW_ : sceneWidth_;
        const u32 h = vpH_ ? vpH_ : sceneHeight_;
        return h ? static_cast<f32>(w) / static_cast<f32>(h) : 0.0f;
    }

    void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 camPos[3]) override {
        // Snapshot OUTGOING viewProj as "previous" before overwrite: frameCB_.viewProj still holds
        // the LAST setCamera's matrix here, the same value VoxiRenderer's curViewProj_ reads via
        // camera() -- capturing off the same field keeps one clock rather than a second gPrevViewProj
        // ticking separately (see prevViewProj_'s comment).
        // Guarded on gbufCameraPrimed_ so the first call doesn't seed prevViewProj_ with frameCB_'s
        // zero rest state, which never described a rendered frame. No caller reads it before
        // gBufferHistoryInvalid() clears anyway, but this keeps the array honest for one that skips
        // the validity check.
        if (gbufCameraPrimed_) std::memcpy(prevViewProj_, frameCB_.viewProj, sizeof(prevViewProj_));
        gbufCameraPrimed_ = true;

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
    // Same rect beginFrame() sets as the D3D12 viewport (RSSetViewports below) -- vpW_ == 0 means no
    // sub-rect, i.e. the whole scene target. SCENE-space: what a reprojecting feature needs, since
    // its history textures are sized off sceneWidth_/sceneHeight_, not width_/height_.
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
        // Wrapped on the way IN, so no shader has to remember to do it. 3600 keeps a float32 at
        // roughly 0.2 ms of resolution indefinitely; a ripple whose period divides an hour crosses
        // the wrap without a seam.
        frameCB_.time[0] = std::fmod(seconds, 3600.0f);
        frameCB_.time[1] = seconds;
        frameCB_.time[2] = deltaSeconds;
        frameCB_.time[3] = 0.0f;
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

    // W4: which heap createMesh() puts a static mesh's vertex/index buffers on, for calls made AFTER
    // this setter -- see IDevice::setStaticMeshHeapDefault (RHI.hpp) for the full contract. The
    // setter logs nothing itself; the first Default-heap mesh createMesh() actually builds logs the
    // C-7 line once (staticMeshDefaultHeapLogged_'s own comment), which is the observable event a
    // reader of the log actually wants, not the flag flip that may precede it by any number of frames.
    void setStaticMeshHeapDefault(bool onDefaultHeap) override { staticMeshDefaultHeap_ = onDefaultHeap; }
    bool staticMeshHeapDefault() const override { return staticMeshDefaultHeap_; }

    // W11: a new mesh sharing `source`'s vertex buffer, with its own index buffer. See
    // IDevice::createMeshSharingVertices (RHI.hpp) for the refcounting and refusal contract.
    MeshHandle createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) override;
    MeshHandle createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) override;
    bool destroyLineMesh(LineHandle mesh) override;
    BufferHandle meshVertexBuffer(MeshHandle mesh) const override {
        if (!mesh || mesh > meshes_.size()) return 0;
        const GpuMesh& m = meshes_[mesh - 1];
        // Gated on computeWritten, not vbBuffer's presence -- every mesh has an RHI vertex buffer
        // now; only a skin target has one something DISPATCHES into.
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
        if (m.boundsRadius <= 0.0f) return false;   // never measured; a point is not an answer
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
    void setWireframe(bool on) override { wireframe_ = on; }
    void setUnlit(bool on) override { unlit_ = on; }
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(drawBinding_, set, constants, bytes);
    }
    void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(defaultDrawBinding_, set, constants, bytes);
    }

    // ---- same-frame depth prepass -- see IDevice's own comment for the contract ----
    void setDepthPrepassEnabled(bool on) override { depthPrepassEnabled_ = on; }
    bool depthPrepassEnabled() const override { return depthPrepassEnabled_; }
    void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) override;
    bool drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) override;
    // Shared body of the two above; true when a depth-only draw was actually recorded.
    // allowComputeWritten: true ONLY from drawMeshDepthOnly (the per-draw path, whose colour draw of
    // the same handle follows immediately); the frame-wide drawMeshDepthPrepass passes false.
    bool depthOnlyDraw(MeshHandle mesh, const f32 world[16], const f32 color[4], bool allowComputeWritten);
    // AUTO-CONSUMED by the next drawMesh() call only -- see the interface comment. Plain assignment:
    // this records what the CALLER believes, not eligibility; drawMesh() re-checks it -- a static
    // mesh via meshVertexBuffer(mesh)==0, a compute-written one only if it is depthOnlyMesh_.
    void setNextDrawPrepassed(bool prepassed) override { nextDrawPrepassed_ = prepassed; }

    // Translucency: blended-mesh path -- see IDevice::setDrawBlended (RHI.hpp) for the mechanism
    // (capture at drawMesh(), replay in endFrame() between the deferred sky and transparentPass, and
    // what excluding a draw from the BLAS/voxel-GI/shadow costs). STICKY like setDrawBinding, unlike
    // setNextDrawPrepassed: reset only in beginFrame, so a run of glass panes sets this once.
    void setDrawBlended(bool blended) override { drawBlended_ = blended; }
    bool drawBlended() const override { return drawBlended_; }

    void setLineDepth(bool testDepth) override { lineDepth_ = testDepth; }
    void setLineGlow(f32 gain) override { lineGlow_ = gain; }
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
    // The same predicate drawMesh applies internally, exposed so a caller can skip issuing a draw
    // it knows to be editor chrome. See IDevice::sceneSuppressed.
    bool sceneSuppressed() const override {
        for (const IRenderFeature* f : features_) if (f->suppressesScene()) return true;
        return false;
    }
    GpuTimingReport gpuTiming() const override;
    void beginFrame() override;
    void endFrame() override;
    void present();
    void initGpuTiming();
    u32  gpuStamp();
    void collectGpuTiming();
    // Same span bookkeeping as pushMarker/popMarker, for phases NOT inside any render feature's
    // markers -- the opaque scene draw and the post/composite/UI chain -- without which the two
    // largest items in the frame land in "unmarked".
    //
    // NOT a ScopedGpuStat (RHIResources.hpp): the begin/end calls here don't share a C++ scope
    // (beginGpuSpan at the tail of beginFrame, endGpuSpan at the top of endFrame, with a whole
    // frame's drawMesh calls between), and RAII can only close what a destructor sees go out of
    // scope. This and its "sky+post+ui" twin below are the only pairs that don't fit in one function.
    void beginGpuSpan(const char* label) {
        if (!tsEnabled_) return;
        // Parent is whatever is already open (kNoParent if none), pushed BEFORE this span's own slot
        // so it's never its own parent. THE CAP IS ENFORCED HERE (previously only assumed): a
        // dropped span's children reparent to whatever is still open -- one missing profile row beats
        // a silently wrong tree.
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
    // Records a device removal once, with the reason decoded. See the definition.
    bool noteDeviceRemoved(const char* where, HRESULT hr);

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
    // Generic-RHI wrapper around depthBuffer_ -- see sceneDepthTexture()'s comment. STABLE across a
    // resize: adoptExternalDepthTexture re-fills this SAME slot, so a caller caching the handle
    // (modules/occlusion does) never sees depthBuffer_ reallocated underneath it.
    TextureHandle depthTexHandle_ = 0;
    // True whenever depthBuffer_ has (re)allocated since depthTexHandle_ was last refreshed --
    // createDepthBuffer() sets this every time it runs; sceneDepthTexture() clears it once it has
    // re-adopted the current depthBuffer_.
    bool depthTexDirty_ = true;

    // G-buffer: velocity + view-space depth + normal/roughness. See IDevice::setGBufferEnabled
    // (RHI.hpp) for the contract and createGBufferTargets() for the MSAA decision. OFF by default;
    // every member below stays zero/null/false until enabled -- what makes "never call this" (every
    // build today) bit-identical to a build without this feature.
    bool gbufferEnabled_ = false;
    // ALWAYS single-sample -- see createGBufferTargets() for why, and what that forces beginFrame's
    // bind-time branch to do when sampleCount_ > 1.
    ComPtr<ID3D12Resource> gbufVelocity_;      // RG16F,        screen-space motion, texels/frame
    ComPtr<ID3D12Resource> gbufViewZ_;         // R32F,         view-space linear depth
    ComPtr<ID3D12Resource> gbufNormalRough_;   // RGB10A2Unorm, world normal (encoded) + roughness
    // One single-descriptor RTV heap per target (same shape msaaRtvHeap_ uses) rather than one
    // 3-descriptor heap: nothing here needs descriptor-index arithmetic, and a dedicated heap per
    // resource means recreating one target never disturbs another's view.
    ComPtr<ID3D12DescriptorHeap> gbufVelocityRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> gbufViewZRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> gbufNormalRoughRtvHeap_;
    bool createGBufferTargets();
    void releaseGBufferTargets();
    // Re-adopts all three resources into the factory's texture table when gbufTexDirty_ says a
    // create just ran. Called only by the three accessors below, each of which already confirmed
    // gbufferEnabled_ -- this method does not check it itself.
    void refreshGBufferTexHandles();
    // The generic-RHI wrapper handles -- see depthTexHandle_'s own comment just above for why a
    // STABLE handle across a resize matters, and sceneDepthTexture() for the lazy-adopt pattern
    // these three follow identically via D3D12ResourceFactory::adoptExternalRenderTargetTexture.
    TextureHandle gbufVelocityTexHandle_ = 0, gbufViewZTexHandle_ = 0, gbufNormalRoughTexHandle_ = 0;
    // One dirty flag for all three: createGBufferTargets makes all three or none, so refreshing
    // happens at exactly one moment -- tracking per-texture would just be three copies of one bool.
    bool gbufTexDirty_ = true;
    // Said once per MISMATCH, not once per frame -- see beginFrame's own comment at the
    // OMSetRenderTargets call this guards for what the mismatch is and why a warning fires there.
    bool gbufMsaaWarned_ = false;

    // Previous frame's view-projection. Row-major, row-vector, same convention as setCamera's
    // `viewProj`. Holds frame N-1's viewProj off the SAME frameCB_.viewProj field VoxiRenderer's
    // curViewProj_/prevViewProj_ read via camera() -- see setCamera's comment for why this isn't a
    // second, independently-timed "previous camera". VoxiRenderer's own copy lands earlier, inside
    // its prePass; this one lands at setCamera, ahead of prePass.
    f32  prevViewProj_[16] = {};
    // True once a camera has been set at all -- guards the first setCamera call from seeding
    // prevViewProj_ with frameCB_'s all-zero starting matrix, which is not a real previous frame.
    bool gbufCameraPrimed_ = false;
    // Mirrors VoxiRenderer's rtHistValid_ (inverted spelling) at two points: forced true by
    // notifyRenderTargetsChanged() when resolution/sample-count/format actually changes (old
    // contents belong to a config that no longer exists), and set to !(this frame wrote real data)
    // at the same beginFrame bind-time decision that binds or skips the three targets below --
    // never touched anywhere else, so there is exactly one place for each of the two resets.
    bool gbufHistoryInvalid_ = true;

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

    // Per-pass GPU timing. Five theories about frame time (shadow cascades, scene walk, chunk
    // streaming, volumetric clouds, build config) were argued from a whole-frame CPU delta and were
    // all wrong -- a number that includes waiting for the GPU can't say which PASS is expensive.
    // Rides on the existing pushMarker/popMarker nesting: a timestamp on each side costs one
    // EndQuery per marker, about a dozen a frame, no new call sites.
    //
    // THE MARKERS WERE ALWAYS CORRECTLY NESTED; THE ACCOUNTING WAS NOT. The old bug was in
    // collectGpuTiming: it folded every span into one flat list and computed "unmarked" as frame
    // minus their sum -- correct only when spans are disjoint. Once a span opens while a parent is
    // already open (permitted; VoxiRenderer::prePass's outer scope does it), the child gets summed
    // once alone and again inside its parent, so "unmarked" goes low or negative. GpuSpan::parent
    // and the GpuAccum tree let collectGpuTiming sum only TOP-LEVEL spans.
    //
    // READ TWO FRAMES LATE: results resolve into a per-frame readback slice read at the top of the
    // next frame reusing that slice. Reading this frame's own timings would stall the GPU to ask how
    // fast it was -- the measurement would create the stall it reports.
    static constexpr u32 kMaxGpuSpans = 64;
    static constexpr u32 kMaxGpuStamps = kMaxGpuSpans * 2;
    // Sentinel for "no parent, top-level". NOT kMaxGpuSpans, which is what this was and why it broke:
    // nothing enforced the span vector stayed under that size, so a 65th open span landed at real
    // index 64 -- bit-identical to the sentinel -- and its children silently reparented to
    // "top-level", double-counted under their real parent AND as a root. Fixed twice: the sentinel
    // is now a value no index can take, and the push is actually bounded.
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    // Begin/end say how long a span took, not where it sits -- fine while spans are disjoint, wrong
    // once one nests (a feature's pushMarker inside the pass wrapping it). This is what lets
    // collectGpuTiming build a tree instead of assuming disjointness. Set from tsOpen_.back() (the
    // innermost still-open span) when this one opens.
    struct GpuSpan { const char* label = nullptr; u32 begin = 0; u32 end = 0; u32 parent = kNoParent; };
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource>  tsReadback_;
    u64  tsFrequency_ = 0;              // GPU ticks per second, from the queue
    u32  tsCount_ = 0;                  // stamps issued so far this frame
    bool tsWrapped_ = false;            // ran out of slots; say so once rather than silently truncate
    // Per-frame-slice, not shared: stamps are read two frames after issue, so labels must survive
    // that long -- a shared vector would be overwritten by the frame in between.
    std::vector<GpuSpan> tsSlice_[kFrameCount];
    u32 tsSliceBegin_[kFrameCount] = {};
    u32 tsSliceEnd_[kFrameCount] = {};
    std::vector<u32>     tsOpen_;       // slots of markers still open, innermost last
    // Span opens refused this frame (cap already reached). Every close checks this FIRST, consuming
    // one rather than popping tsOpen_ -- otherwise a refused open's matching close would pop somebody
    // else's still-open span and stamp `end` into the wrong row, a failure that only shows under load.
    u32                  tsDropped_ = 0;
    // Accumulated across frames as a TREE, not a flat list -- "unmarked = frame minus every span"
    // broke once spans could nest (see kNoParent). Each node's `ms` is INCLUSIVE; `parent` (index
    // into this vector, or kNoAccumParent for top-level) makes exclusive time and indentation
    // computable at report time. Keyed by (label, parent): two same-text spans under different
    // parents are different things, and that pairing is stable frame to frame.
    static constexpr u32 kNoAccumParent = 0xFFFFFFFFu;
    struct GpuAccum { std::string label; f64 ms = 0; u32 parent = kNoAccumParent; };
    std::vector<GpuAccum> tsAccum_;
    // Scratch, reused rather than reallocated: frame-local span index -> its GpuAccum index, so a
    // child (processed after its parent) can look up which node its parent folded into.
    std::vector<u32> tsSpanToAccum_;
    f64  tsAccumFrameMs_ = 0;
    u32  tsAccumFrames_ = 0;
    u32  tsReports_ = 0;
    bool tsEnabled_ = false;

    // Redundant-state elision for the feature-overridden scene draw. One drawMesh per entity --
    // 1,656 of them on Electric Dreams -- used to re-send the SAME pipeline, table-0 binding set and
    // frame constant block every time. setPipeline rebinding a root signature per draw makes the GPU
    // re-fetch all root data; the frame CBV copy is why a 2MB ring exhausted and doubled every frame.
    //
    // Only ever elided as a GROUP: setPipeline's bindDeclaredRootCbvs resets the FEATURE frame slot
    // to the zero CBV, so skipping the constant buffer alone would leave the shader reading zeroes.
    PipelineHandle   fovPso_ = 0;
    BindingSetHandle fovSet_ = 0;
    u32              fovCbBytes_ = 0;
    std::vector<u8>  fovCb_;
    // Cleared by anything that could bind something else since the last draw: setPipeline always,
    // setBindingSet on TABLE 0 only (table 1 is per-material and legitimately changes every draw),
    // bindGraphicsRoot on a genuine root signature change (see that function's comment: this was
    // missing for a long time -- the same hole dbValid_ shipped with until d8326985 fixed it there
    // alone -- until it was closed here too), and the start of each frame / endFrame's post chain.
    bool             fovValid_ = false;

    // TABLE 1's OWN redundant-state elision -- the per-draw material binding set plus its b2
    // constant block, applied by D3D12RenderContext::applyDrawBinding. See that function for the
    // full justification, the default-off toggle, and why this exists at all (fov* above is
    // deliberately blind to table 1, on purpose, for a different reason).
    //
    // A DELIBERATE SEPARATE FLAG FROM fovValid_, not a reuse of it, even though both die at the same
    // setPipeline/beginFrame/endFrame events: fovValid_ is ALSO cleared by setBindingSet on table 0
    // (its own comment explains why table 0 rebinding must not touch table 1), and folding this into
    // fovValid_ would wrongly throw this cache away on every table-0 rebind too, defeating most of
    // what it exists to save. dbSet_/dbConstants_/dbConstantBytes_ mirror what applyDrawBinding last
    // actually BOUND on the command list, not merely what a caller last requested -- see that
    // function for why the distinction matters.
    BindingSetHandle dbSet_ = 0;
    u8               dbConstants_[kMaxDrawConstantBytes] = {};
    u32              dbConstantBytes_ = 0;
    bool             dbValid_ = false;

    // (set base, table, pipeline base) triples already reported by setBindingSet's register-mismatch
    // check. ONE BINDING SET LEGITIMATELY SERVES TWO PIPELINES AT TWO BASES (the shared material
    // system stamps a set with whichever layout initialised it, e.g. Voxi's t9, while the cluster
    // pipeline's table 1 sits at t4) -- correct, but without this the warning fires on every cluster
    // draw and drowns the log. Kept as a once-per-shape diagnostic: on a single-consumer pipeline it
    // still catches a real mistake.
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

    // ---- translucency: the blended-mesh path (see IDevice::setDrawBlended's own comment in
    // RHI.hpp for the whole mechanism, and the two overrides just above this class's public section
    // that expose it) ----
    bool drawBlended_ = false;   // sticky, like drawBinding_ just above; reset to false in beginFrame

    // One drawMesh() call captured while drawBlended_ was true, queued here instead of being drawn
    // immediately -- see setDrawBlended's comment for why the deferred sky forces this to be a
    // capture-and-replay rather than something a caller could sort into place itself. `binding`
    // reuses the SAME DrawBinding struct and storeDrawBinding() helper drawBinding_ itself uses just
    // above: setDrawBinding's own contract ("constants is COPIED; the caller may reuse its buffer
    // immediately") has to keep holding all the way to this capture's eventual replay in endFrame,
    // frames' worth of other drawMesh calls later, not merely until drawMesh() returns -- exactly
    // the guarantee storeDrawBinding's memcpy-into-a-fixed-array already gives drawBinding_, so
    // capturing through it rather than a second copy of the same copying logic is free correctness,
    // not a convenience.
    struct BlendedDraw {
        MeshHandle mesh = 0;
        f32 world[16] = {};
        f32 color[4] = {};
        f32 metallic = 0.0f;
        f32 roughness = 0.0f;
        DrawBinding binding{};
    };
    // Filled by drawMesh() between beginFrame and endFrame, drained (sorted back-to-front and
    // replayed) by endFrame's own flush, cleared at the top of the NEXT beginFrame -- see that
    // clear's own comment for why clearing only there, and not right after the flush drains it, is
    // both sufficient and the simpler invariant to maintain.
    std::vector<BlendedDraw> blendedDraws_;
    // The "no feature offers scenePipeline(..., blended=true)" warning, said at most once PER FRAME
    // -- reset in beginFrame, UNLIKE drawBindingIgnored_ just above, which is said once in the
    // entire run and never reset. The two conditions are not the same shape: drawBindingIgnored_
    // guards a static fact about which pipeline the whole scene draws through, true or false for the
    // life of the process, so repeating it would just print the identical line forever. A feature
    // offering no blended pipeline is a condition that can persist for the entire run just as easily
    // (nobody has authored a blended Voxi PSO, say) -- and under a once-ever warning that would mean
    // printing on frame one and then falling silent while every pane of glass after it kept
    // invisibly vanishing, which is a worse outcome for exactly the person trying to find out why.
    bool blendedPipelineMissingWarned_ = false;

    // ---- same-frame depth prepass (see IDevice::setDepthPrepassEnabled and drawMesh below) ----
    bool depthPrepassEnabled_ = false;   // --depth-prepass; OFF reproduces pre-existing behaviour
    // AUTO-CONSUMED: read and reset to false by the very next drawMesh() call, whether or not that
    // call actually used it (a mesh that turns out to be compute-written still clears it) -- see
    // setNextDrawPrepassed's own interface comment for why this must not be sticky.
    bool nextDrawPrepassed_ = false;
    // The mesh drawMeshDepthOnly() last ACTUALLY depth-drew, or 0. drawMesh() honours
    // nextDrawPrepassed_ for a compute-written (skinned/soft-body) mesh only when it is this exact
    // handle: that mesh's posed vertex buffer IS its vbv, so the depth-only draw and the colour draw
    // read the same bytes. Consumed (zeroed) by every drawMesh() and by beginFrame, like the flag.
    MeshHandle depthOnlyMesh_ = 0;
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
    // Last logged outcome of beginFrame's scene-claim race, so the warning fires on a CHANGE rather
    // than every frame. Compared, never dereferenced -- a feature removed between frames leaves a
    // dangling pointer here whose only use is an inequality test that then logs, which is correct.
    const IRenderFeature* lastSuppressWinner_ = nullptr;
    u32                   lastSuppressClaimants_ = 0;
    // The authored atmosphere; the frame block above holds the packed form the shader reads.
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    // See IDevice::setUnlit. Sticky exactly as wireframe_ is -- neither is reset per frame.
    bool unlit_ = false;
    bool lineDepth_ = true;
    // 1.0 is exactly the pre-glow behaviour; see IDevice::setLineGlow.
    f32  lineGlow_  = 1.0f;
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
    // Local exposure's bilateral grid, raw (u2) and blurred (u3) -- SCENE-SIZE DEPENDENT, unlike
    // histBuf_/expBuf_ above: (re)created in createPostTargets/releasePostTargets, not
    // createPostPipelines, sized for the CURRENT sceneWidth_/sceneHeight_. Null (and the passes that
    // write them skipped) if the allocation ever fails; see createPostTargets.
    ComPtr<ID3D12Resource> localGridBuf_, localGridBlurBuf_;
    u32 localGridW_ = 0, localGridH_ = 0;        // grid dimensions in tiles, matching the buffers above
    ComPtr<ID3D12DescriptorHeap> postRtvHeap_;   // one RTV per bloom mip
    ComPtr<ID3D12DescriptorHeap> postSrvHeap_;   // shader-visible: the SRV triples + the UAV quad
    ComPtr<ID3D12RootSignature> postRootSig_;
    ComPtr<ID3D12PipelineState> bloomPrefilterPso_, bloomDownPso_, bloomUpPso_;
    // Composite permutations, indexed [bloom on][auto-exposure on].
    ComPtr<ID3D12PipelineState> compositePso_[2][2];
    ComPtr<ID3D12PipelineState> histogramPso_, exposurePso_;
    ComPtr<ID3D12PipelineState> localGridPso_, localBlurPso_;   // local exposure's bilateral grid
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

    // Non-owning; see UiBackend.hpp and setUiBackend above. Null in every game build and in an
    // AVER_ENABLE_UI=OFF editor tree -- this file never names Dear ImGui itself.
    d3d12::IUiBackend* uiBackend_ = nullptr;
    bool uiActive_ = false;

    std::vector<GpuMesh> meshes_;
    // Skin-target meshes created outside a frame and still holding uninitialised memory. Drained at
    // the top of the next frame; see seedSkinTargets.
    struct SkinSeed { MeshHandle dst; MeshHandle src; };
    std::vector<SkinSeed> skinSeeds_;
    void seedSkinTargets();
    // Shared body of createMeshSharingVertices and createPosedPartMesh; see the latter.
    MeshHandle shareVertices(MeshHandle source, const u32* indices, u32 indexCount, bool posed);

    // W4: false (the default) reproduces today's behaviour exactly -- every static mesh createMesh()
    // builds lives on the Upload heap. See IDevice::setStaticMeshHeapDefault (RHI.hpp) for the
    // trade this makes.
    bool staticMeshDefaultHeap_ = false;
    // C-7's "static meshes on the Default heap" line fires once, on the FIRST Default-heap mesh
    // createMesh() actually builds -- not on the setter flipping, which can happen any number of
    // frames before (or after, if it flips back) a mesh is next created.
    bool staticMeshDefaultHeapLogged_ = false;
    // W4 4a: on ANY failure uploading a Default-heap mesh's buffers, createMesh() falls back to the
    // Upload path for that one mesh and warns here -- once, since a failure mode that recurs every
    // mesh for the rest of the run would otherwise spam identically for each one.
    bool staticMeshDefaultHeapUploadFailWarned_ = false;
    PerFrameCB frameCB_{};
    u32 width_ = 0, height_ = 0;
    // Scene's OWN render-target size: width_/height_ scaled by renderScale_, rounded, floored at 1.
    // Equal to width_/height_ at renderScale_ == 1.0 (the default), so every byte on that path stays
    // identical to before renderScale_ existed. Present-resolution things (backbuffer, viewport
    // texture, ImGui, capture) key off width_/height_; only depth/MSAA-colour, the post chain, and
    // onRenderTargetsChanged key off this pair.
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
    // DEFERRED TO A FRAME BOUNDARY, ALWAYS. Applying a scale change where asked destroys and recreates
    // the depth buffer, MSAA target and post chain -- but the editor's preference load runs from
    // buildUI(), between beginFrame() and endFrame(), so that frame's command list had already bound
    // resources rebuildSceneTargets went on to free, and endFrame presented dead resources: device
    // removed AT PRESENT. The tell: --render-scale on the command line, applying from onInit outside
    // any frame, worked fine at the identical value -- same function, only the timing differed.
    //
    // waitForGpu() inside rebuildSceneTargets does NOT save this -- it drains submitted work, not a
    // command list still being recorded on the CPU.
    //
    // So the value is parked and applied at the top of the next beginFrame, unconditionally: no
    // caller should have to know, and a rule with no exceptions cannot be got wrong by the next one.
    void setRenderScale(f32 scale) override {
        const f32 asked = scale;
        scale = std::fmax(0.25f, std::fmin(1.0f, scale));
        const f32 effective = pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_;
        if (scale == effective) return;
        // Before a swapchain exists there is no frame to be inside and nothing to rebuild --
        // createSwapchainResources sizes itself off renderScale_ when it runs.
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
    // Reports what the last caller ASKED FOR, not what is currently resident, so a read-back
    // immediately after a set sees the value it just wrote rather than the old one for one frame.
    f32 pendingOrCurrentRenderScale() const {
        return pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_;
    }
    void applyPendingRenderScale() {
        if (!pendingRenderScaleValid_) return;
        pendingRenderScaleValid_ = false;
        if (pendingRenderScale_ == renderScale_) return;
        renderScale_ = pendingRenderScale_;
        rebuildSceneTargets();
    }
    f32  pendingRenderScale_ = 1.0f;
    bool pendingRenderScaleValid_ = false;
    f32 renderScale() const override { return pendingOrCurrentRenderScale(); }
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
        // Same "reset then recreate at the new scene size" every OTHER scene target here just did --
        // GATED on gbufferEnabled_, unlike depthBuffer_/msaaColor_ just above, which are unconditional:
        // those two exist for every build; these three exist ONLY while a caller has explicitly opted
        // in, and recreating them regardless of that flag would allocate ~54 MB no build asking
        // for "off" ever agreed to pay for. See createGBufferTargets' own comment for everything else.
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
    // Same idea, for the LAST descriptor heap array pushed onto cmdList_ via SetDescriptorHeaps --
    // see D3D12RenderContext::setBindingSet for why: this backend has only ONE generic heap
    // (res_->heap_), so thousands of draws call SetDescriptorHeaps with the identical single-entry
    // array. Every direct SetDescriptorHeaps call bypassing setBindingSet (post chain's postSrvHeap_,
    // the UI backend's own heap) must update this immediately after, or a later setBindingSet would
    // wrongly believe res_->heap_ was still bound.
    ID3D12DescriptorHeap* boundHeap_ = nullptr;

    bool hasSwapchain_ = false;
    // Sticky: nothing here recreates a device. See noteDeviceRemoved and IDevice::deviceLost.
    bool deviceLost_ = false;
    // Counted only when rhi::simulatedDeviceLoss() is armed; see present().
    u32  presentedFrames_ = 0;
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
    // QueryVideoMemoryInfo's interface -- acquired once at init from whichever adapter device_ was
    // actually created against (WARP or hardware; see init()'s two adapter-selection branches), via
    // .As() off the IDXGIAdapter1 each already holds locally. NULL when that cast fails -- an older
    // DXGI, or a driver that never exposes IDXGIAdapter3 -- which is exactly what videoMemory() below
    // tests to report VideoMemoryInfo::supported false rather than call through a null pointer.
    ComPtr<IDXGIAdapter3> adapter3_;

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
    // The blended pass's backdrop -- see IDevice::sceneColorBackdropTexture for why it exists.
    // Created UNCONDITIONALLY, unlike sceneColorTex_ above, which only the upscaler path needs.
    TextureHandle blendBackdropTex_ = 0;
    u32           blendBackdropW_ = 0, blendBackdropH_ = 0;
    bool          blendBackdropCopyLogged_ = false;
    // Per-layer backdrop re-capture: how many extra MSAA resolves one frame's translucency may buy.
    // EIGHT IS A BUDGET, NOT AN ALGORITHM LIMIT -- each re-capture is a full-target resolve, so this
    // keeps "correct stacked glass" from turning a hundred panes into a hundred resolves. Eight
    // covers every arrangement seen so far (water under a glass walkway under a rail is three);
    // surfaces beyond it degrade to the single-capture behaviour that shipped before -- a known
    // approximation, not a new failure.
    static constexpr u32 kMaxBlendLayerResolves = 8;
    bool          blendLayerCapWarned_ = false;
    // M3: a cheap on-change log of what the blended replay actually did, so a session's log can
    // answer "is this frame even doing translucency work" without turning on GPU timing. Widened
    // rather than once-per-change (a scene that gains and loses one pane of glass every other frame
    // would otherwise log every other frame forever): see the log site for the power-of-two gate.
    // ~0u is not a real (draws, resolves) pair -- it forces the FIRST comparison after startup to
    // count as a change and log once, rather than requiring the accidental case drawn==0 && resolves
    // ==0 to already match.
    u32 blendStatDrawsLogged_ = ~0u;
    u32 blendStatResolvesLogged_ = ~0u;
    u32 blendStatChanges_ = 0;
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
    // DELIBERATELY NOT UNDER AVER_RHI_TRACK_STATE, unlike the state array below. That macro buys
    // per-subresource STATE TRACKING, a real debug-only cost; a NAME is identity, needed most in the
    // build a user actually runs -- sweeping the two together made release diagnostics silently
    // anonymous, and in Vulkan (two messages read the name outside the guard) didn't COMPILE under
    // NDEBUG.
    //
    // MEASURED before making it unconditional: a 60-frame editor session ends with 23 textures and
    // 33 buffers, all named, 952 bytes of names total, 29 long enough to escape the small-string
    // buffer. That's the entire cost.
    //
    // Owned copy: desc.debugName is the caller's pointer, nulled at creation once copied.
    std::string debugName;
#if AVER_RHI_TRACK_STATE
    // One entry per mip; a subresource index is a mip index here.
    std::vector<ResourceState> states;
#endif
    // The descriptor uiTextureId() handed to the UI. Zero until first asked, and forever zero with
    // no UI backend installed -- unconditional now, not gated on AVER_WITH_IMGUI (see UiBackend.hpp).
    u64 uiSrvCpu = 0, uiSrvGpu = 0;
};

// A buffer, its description, and its persistent mapping.
struct RhiBuffer {
    ComPtr<ID3D12Resource> res;
    BufferDesc desc{};
    u8* mapped = nullptr;                   // upload buffers stay mapped for their whole life
    std::string debugName;                  // identity, not state tracking -- see RhiTexture::debugName
#if AVER_RHI_TRACK_STATE
    ResourceState state = ResourceState::Common;
    // Upload-heap and acceleration-structure buffers reject every transition.
    bool stateFixed = false;
#endif
};

// A compiled shader blob and the stage it was compiled for.
// A shader's bytecode, from one of two places. `blob` is what DXC (or FXC) produced from HLSL text;
// `bytes` is a COPY of bytecode the caller already had (ShaderDesc::bytecode -- NRD's precompiled
// permutations are the first). Exactly one is ever populated, and code() is the only thing that
// should ask which: every consumer wants a {pointer, length} pair and does not care where it came
// from. Copying rather than borrowing is ShaderDesc::bytecode's documented contract.
struct RhiShader {
    ComPtr<ID3DBlob>  blob;
    std::vector<u8>   bytes;
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
    ID3D12RootSignature* rootSig = nullptr;   // owned by the root-signature cache, not by this
    bool compute = false;
    bool mesh = false;
    // True when this mesh pipeline also has an amplification shader (GraphicsPipelineDesc::as != 0),
    // i.e. it is a dispatchMeshClusters() pipeline, not a dispatchMeshFor() one. Always false when
    // `mesh` is false.
    bool amplification = false;
    // One entry per declarable table; -1 where the layout declared nothing for it.
    i32  srvParam[kBindingTableCount] = {-1, -1}, uavParam[kBindingTableCount] = {-1, -1};
    i32  bindlessParam = -1;   // -1 = this pipeline declared no bindless table
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
//
// VERSIONED: every write (setSrv/setUav/clearSrv/setSrvTlas/setSrvBuffer/setUavBuffer/nullFill)
// lands in `stageBase`, a range in a CPU-only staging heap, and bumps `version`. `stageBase` is
// authoritative; nothing ever reads it directly on the GPU. There is one shader-visible copy per
// frame in flight (`gpuBase[f]`), and setBindingSet refreshes gpuBase[fi] from stageBase only when
// gpuVersion[fi] is behind version. This is what makes an in-place-write into a live shader-visible
// heap (the previous scheme) safe again: with kFrameCount=2 and beginFrame waiting only on the
// fence for THIS backbuffer, the CPU can be recording frame N+1 while the GPU still executes frame
// N against gpuBase[N]'s contents -- a set rewritten for frame N+1 must not touch what frame N's
// not-yet-retired dispatches will read. gpuBase[fi] is only ever rewritten once beginFrame has
// waited for the frame that last used slot fi, so the copy can never race a GPU read of that slot.
struct RhiBindingSet {
    u32 srvCount = 0, uavCount = 0;
    // The run of shader registers the set was built for.
    u32 srvBaseRegister = 0, uavBaseRegister = 0;
    u32 stageBase = 0;              // range in the CPU-only staging heap; SRVs then UAVs, as before
    u32 gpuBase[kFrameCount] = {};  // one shader-visible range per frame in flight
    u64 version = 0;                // bumped on every write into stageBase
    // The version each gpuBase[f] last received. Starts at 0 so the very first setBindingSet for a
    // freshly created set (version already >=1 from nullFill) always copies before it is bound.
    u64 gpuVersion[kFrameCount] = {};
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
// The ray path's bindless texture array: `capacity` contiguous descriptors in the shared heap.
// Suballocated by the SAME allocRange() every binding set uses, so it is not a second allocator --
// it is one more customer of the existing one, and it shows up in the same exhaustion message.
struct RhiBindlessTable {
    u32  heapBase = 0;
    u32  capacity = 0;
    bool alive = false;
};

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
    i32 bindlessParam = -1;
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
    // Part of the key. Without this a bindless variant and a non-bindless one with otherwise
    // identical counts would share a cached root signature, and whichever built first would decide
    // whether the table exists -- silently, for both.
    if (a.bindlessTextureCount != b.bindlessTextureCount) return false;
    // Part of the key for the same reason bindlessTextureCount is. Two layouts identical but for
    // the space their constant buffer sits in produce root signatures a shader compiled for the
    // other one CANNOT be created against -- and the failure would land on whichever pipeline was
    // built second, naming neither.
    if (a.constantSpace != b.constantSpace || a.samplerSpace != b.samplerSpace) return false;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) if (a.constantDwords[i] != b.constantDwords[i]) return false;
    for (u32 i = 0; i < a.samplerCount && i < 4; ++i) if (!sameSampler(a.samplers[i], b.samplers[i])) return false;
    return true;
}

// Descriptors every binding set suballocates from: one shader-visible heap for the whole device.
constexpr u32 kRhiHeapSize = 65536;
// Transient constant bytes per frame in flight. The upload ring's STARTING size; grows on demand
// (ringAlloc), so this is a floor for a quiet scene, not a budget.
//
// 2MB, raised from 1MB: overflow isn't free -- ringAlloc returns 0 on the frame it runs out, and
// growth only takes effect next epoch, so the overflowing frame renders wrong. Every session on any
// real scene opened with "upload ring (1024 KB) exhausted this frame; growing to 2048 KB" -- one
// visibly wrong frame at startup nobody read as a defect. 2MB is MEASURED, not guessed; it doesn't
// remove the failure mode for a heavier scene (which wants ringAlloc to fall back to a one-off
// allocation instead of returning 0), it removes the case hitting every single run.
constexpr u64 kRhiRingBytes = 2u << 20;
// Growth ceiling. 64MB is ~260,000 per-draw constant slices in one frame -- far past any interactive
// draw count, so hitting it means something is wrong upstream. The buffer is CPU-visible upload
// memory, one per frame in flight; unbounded growth from a runaway draw loop would exhaust address
// space instead of reporting a problem.
constexpr u64 kRhiRingMaxBytes = 64ull << 20;

// One destination/source/size triple for D3D12ResourceFactory::uploadBuffers. A NAMED struct rather
// than an anonymous one nested in the parameter list, so a caller outside this class (W4's
// D3D12Device::createMesh, uploading a Default-heap mesh's vertex and index buffers together) can
// spell the type of the initializer_list it is building.
struct BufferUploadItem {
    ID3D12Resource* dst = nullptr;   // must already be a freshly created Default-heap buffer in COMMON
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
    // Where the table starts in the shared heap; the render context needs it to set the root table.
    u32  bindlessHeapBase(BindlessTableHandle h) const {
        return (h == 0 || h > bindlessTables_.size()) ? 0u : bindlessTables_[h - 1].heapBase;
    }
    BindingSetHandle createBindingSet(const BindingSetDesc& d) override;
    BlasHandle       createBlas(MeshHandle mesh) override;
    TlasHandle       createTlas(u32 maxInstances) override;

    void destroyTexture(TextureHandle h) override;
    void destroyBuffer(BufferHandle h) override;
    void destroyBlas(BlasHandle h) override;
    MeshHandle blasMesh(BlasHandle h) const override;
    BlasHandle blasForMesh(MeshHandle mesh) const override;
    // Destroys every acceleration structure built from `mesh`. Concrete rather than part of
    // IResourceFactory: it is an implementation detail of D3D12Device::destroyMesh, and no caller
    // outside this file has any business asking for it.
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
    // Wraps a resource this factory did NOT create (D3D12Device::depthBuffer_, made directly via
    // CreateCommittedResource because it needs a MULTISAMPLE-aware DSV -- createTexture() makes
    // neither) into an ordinary TextureHandle, reachable via setSrv/textureBarrier like any
    // factory-made texture. Concrete rather than part of IResourceFactory: this is ONE specific
    // adoption, not a general entry point. `existing`, when non-zero, is REUSED in place -- see
    // depthTexHandle_'s comment for why the handle must stay stable across a resize. Returns 0 if
    // `res` is null.
    TextureHandle adoptExternalDepthTexture(ID3D12Resource* res, u32 width, u32 height, TextureHandle existing);
    // SAME mechanics as adoptExternalDepthTexture above, generalised over `fmt` and left in
    // RenderTarget rather than DepthWrite: the three G-buffer targets are ordinary render-target
    // resources made directly via CreateCommittedResource for the same reason, never typeless, never
    // a DSV alias. `existing` REUSED in place for the same reason: the handles must stay stable
    // across a resize.
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
    // The acceleration-structure substitution warning, said once per device.
    bool asSlotLogged_ = false;

    // Fills a freshly created texture from TextureDesc::initialData. The resource must already be
    // in COPY_DEST; on success it has been transitioned to `d.initialState` and the GPU has finished.
    bool uploadInitialData(ID3D12Resource* res, const D3D12_RESOURCE_DESC& td, const TextureDesc& d,
                           u32 mips);

    // W4: fills one or more freshly created DEFAULT-heap buffers via a single UPLOAD-heap staging
    // buffer and a one-shot command list, blocking until the copy has retired -- the same shape as
    // uploadInitialData just above, generalised from one texture's mip chain to an arbitrary set of
    // buffer destinations. Exists because writeBuffer() refuses any buffer that isn't mapped (i.e.
    // BufferKind::Upload), and a mesh created on the Default heap (setStaticMeshHeapDefault) still
    // has to receive its vertex/index bytes from SOMEWHERE before the first frame that might draw or
    // BLAS-build it -- see D3D12Device::createMesh and ::createMeshSharingVertices, its two callers.
    //
    // Every `dst` MUST already be a freshly created Default-heap buffer in D3D12_RESOURCE_STATE_COMMON
    // (createBuffer's own initial state for BufferKind::Default): with no prior history on the
    // resource, the runtime implicitly promotes it to COPY_DEST on this list's own CopyBufferRegion,
    // exactly as seedSkinTargets' own comment explains for the identical situation. This function then
    // transitions each `dst` explicitly back to COMMON before closing the list, so that promotion does
    // not linger for whatever list a caller records next -- the same explicit undo seedSkinTargets
    // performs on its own copy.
    //
    // Returns false on any failure (staging allocation, the one-shot list, or the fence wait); the
    // caller is then responsible for falling back or leaving its destination buffers unpopulated.
    bool uploadBuffers(std::initializer_list<BufferUploadItem> items);

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
    // Descriptor slot `heapBase + index`, CPU side (for writing) and GPU side (for binding), in the
    // shared SHADER-VISIBLE heap.
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(u32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSlot(u32 index) const;
    // CPU handle of descriptor slot `index` in the CPU-only STAGING heap -- see RhiBindingSet's
    // comment. Binding sets are the only customer; the bindless table writes straight into cpuSlot.
    D3D12_CPU_DESCRIPTOR_HANDLE stagingCpu(u32 index) const;
    // Every write into a binding set's staging range ends here: bumps the version setBindingSet
    // compares, AND drops the device's two "same set as last draw, skip the rebind" caches if they
    // hold this set. Before versioning a write landed in the very slots the bound table pointed at,
    // so skipping the rebind was harmless; now the write reaches the GPU range only through
    // setBindingSet's copy, and a cache that skipped it would draw with the old descriptors.
    void noteBindingSetWritten(BindingSetHandle set, RhiBindingSet& s);
    void nullFill(RhiBindingSet& s);
    // The null view for ONE slot of a given kind. Shared by nullFill (which writes every slot at
    // creation) and clearSrv (which returns one slot to that state later), so the two can never
    // disagree about what "null" means for a kind -- a divergence that would show up only as a
    // device removal on whichever path was not updated.
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc(SlotKind kind);
    // Reuses a retired range large enough for `count`, or bump-allocates. False when exhausted.
    // Shared by the bindless table and a binding set's per-frame SHADER-VISIBLE ranges.
    bool allocRange(u32 count, u32& outFirst);
    // Same allocator shape as allocRange, over the CPU-only staging heap's own free list. A binding
    // set's staging range is the ONE customer; kept as a separate list rather than parameterising
    // allocRange because the two heaps must never hand out overlapping slot numbers from one budget.
    bool allocStageRange(u32 count, u32& outFirst);

    // The fence value at which work recorded right now can be considered retired.
    u64  retireFence() const;
    void retire(ComPtr<IUnknown> obj);
    void collect();

    D3D12Device* dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    u32 heapStride_ = 0;
    u32 heapUsed_ = 0;
    // CPU-only descriptor heap (D3D12_DESCRIPTOR_HEAP_FLAG_NONE) binding sets stage their writes
    // into before setBindingSet copies them to the shader-visible heap -- see RhiBindingSet's
    // comment for why. Same increment size as heap_ (GetDescriptorHandleIncrementSize depends on
    // heap TYPE, not the shader-visible flag), so heapStride_ serves both.
    ComPtr<ID3D12DescriptorHeap> stageHeap_;
    u32 stageHeapUsed_ = 0;

    std::vector<RhiTexture>    textures_;
    std::vector<RhiBuffer>     buffers_;
    std::vector<RhiShader>     shaders_;
    // std::deque, NOT std::vector -- the odd one out among its neighbours, on purpose.
    //
    // D3D12RenderContext::setPipeline caches a raw `const RhiPipeline* pipe_` (&pipelines_[h-1]);
    // setBindingSet/setConstants/setConstantBuffer/applyDrawBinding/drawMeshInstanced/dispatchMeshFor/
    // dispatchMeshClusters/dispatch all read root-parameter indices through it for the rest of the
    // pass, feeding SetGraphicsRootDescriptorTable -- a stale read doesn't fault, it hands the driver
    // a plausible root parameter belonging to nothing, in an already-recorded command list.
    //
    // A vector's push_back relocates every element on growth, and pipelines ARE created mid-recording
    // (OcclusionCuller::ensureSized builds PSOs inside the per-frame entity walk on a resolution
    // change). MEASURED: an ordinary 60-frame editor run relocates this table twelve times, and on the
    // twelfth -- the 95th pipeline, capacity 94->141 -- the context is holding a pipe_ the relocation
    // just freed. Only the next setPipeline overwriting pipe_ before anything reads it saves this
    // today; nothing enforces that ordering.
    //
    // deque::push_back never invalidates references to existing elements, so the cached pointer can't
    // go stale -- cheaper than re-resolving the handle at every one of the nine read sites. Neighbours
    // below stay vectors: nothing caches a raw pointer into them across a call that could grow them.
    std::deque<RhiPipeline>    pipelines_;
    std::vector<RhiBindingSet> bindingSets_;
    std::vector<RhiBindlessTable> bindlessTables_;
    std::vector<RhiBlas>       blases_;
    std::vector<RhiTlas>       tlases_;
    std::vector<RootSigEntry>  rootSigs_;
    std::vector<RetiredObject> retired_;
    std::vector<RetiredRange>  pendingRanges_;   // returned, still behind the fence (shader-visible heap)
    std::vector<RetiredRange>  freeRanges_;      // reusable now (shader-visible heap)
    std::vector<RetiredRange>  stagePendingRanges_;   // same, for the CPU-only staging heap
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
            warp.As(&adapter3_);   // best-effort; null leaves videoMemory() reporting unsupported
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
            adapter.As(&adapter3_);   // best-effort; null leaves videoMemory() reporting unsupported
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

    // Logged once, here rather than left to whatever periodic caller polls videoMemory() later,
    // because a session's very first log lines are the ones a memory-pressure bug report actually
    // has -- by the time anyone notices stutter and goes looking, the run may be hours old. Silent
    // when unsupported (no IDXGIAdapter3, or the query itself failed): there is no C-7 text defined
    // for that case on this line, unlike the Vulkan backend's explicit "not reported".
    {
        const VideoMemoryInfo vmem = videoMemory();
        if (vmem.supported) {
            AVER_INFO("[RHI.D3D12] video memory at init: local {} MB used of {} MB budget, non-local {} MB used of {} MB budget",
                      vmem.localUsageBytes / (1024ull * 1024ull), vmem.localBudgetBytes / (1024ull * 1024ull),
                      vmem.nonLocalUsageBytes / (1024ull * 1024ull), vmem.nonLocalBudgetBytes / (1024ull * 1024ull));
        }
    }

    // BOUND THE SHADER BLOB CACHE. Its key is the whole compiler input, which is what makes it
    // correct with no invalidation logic -- a changed input simply misses -- and the price of that is
    // that nothing ever becomes stale, so nothing was ever removed. Every shader edit left its
    // predecessor behind in a machine-wide directory no UI and no CLI could clear. On a machine that
    // has been developing shaders for months, that is the whole history of every variant compiled.
    //
    // ONCE AT DEVICE INIT, not per compile: the sweep is a directory walk, and doing it per miss
    // would put a stat of the whole cache on the path whose cost the cache exists to remove.
    // Failure is ignored on purpose -- a cache that cannot be swept still serves hits, and a startup
    // that refused to run because it could not delete a file would be a far worse trade.
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

    // DXR 1.1 needs only caps_ and device_, both valid here -- same as VulkanDevice::init(). This
    // backend used to call initAccelerationStructures() from createSwapchainResources() instead,
    // gated on cmdList4_ (only exists once a swapchain's command list does), so "ray tracing works"
    // depended on "a window was created": every --headless run silently carried device5_ == null
    // while the caps line claimed RT tier 11. Calling it here matches Vulkan's shape and makes
    // device5_ available headless too.
    initAccelerationStructures();

    rhiFactory_ = new D3D12ResourceFactory(this);
    if (!rhiFactory_->init()) { delete rhiFactory_; rhiFactory_ = nullptr; }
    else rhiContext_ = new D3D12RenderContext(this, rhiFactory_);

    AVER_INFO("[RHI.D3D12] device ready on adapter '{}'", adapterName_);
    // Handed to the crash reporter here, not at install() time, because the adapter isn't known
    // until now -- a crash report without adapter/driver info is close to useless.
    crash::setGpuName(adapterName_);
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

// Polls the OS's current video memory budget and usage for this adapter. See VideoMemoryInfo's own
// comment (RHI.hpp) for what the LOCAL/NON_LOCAL split means and why `supported` false leaves every
// field 0 rather than a stale or guessed value.
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

// See RHI.hpp's comment on IDevice::sceneDepthTexture. Lazily (re)adopts depthBuffer_ whenever
// createDepthBuffer() has run since the last call -- see depthTexDirty_'s comment for why that flag,
// not a size comparison, is the trigger: this method can't cheaply tell "the SAME resource" from
// "one that happens to be the same size".
TextureHandle D3D12Device::sceneDepthTexture() {
    if (!depthBuffer_ || !rhiFactory_) return 0;
    if (depthTexDirty_) {
        depthTexHandle_ = rhiFactory_->adoptExternalDepthTexture(
            depthBuffer_.Get(), sceneWidth_, sceneHeight_, depthTexHandle_);
        depthTexDirty_ = false;
    }
    return depthTexHandle_;
}

// Turns the G-buffer feature on/off. See IDevice::setGBufferEnabled (RHI.hpp) for the "additive and
// defaulted the whole way down" contract: OFF (default, every build today) allocates none of these
// three targets, so this only creates or releases anything on a REAL edge (on != gbufferEnabled_).
//
// APPLIED IMMEDIATELY when a swapchain exists, unlike setRenderScale's deferred-to-next-
// createSwapchainResources approach -- there's no equivalent later hook this could rely on (that
// value is READ by createSwapchainResources itself; this flag isn't), so acting now is the only
// correct time, exactly like setSampleCount.
void D3D12Device::setGBufferEnabled(bool on) {
    if (on == gbufferEnabled_) return;
    gbufferEnabled_ = on;
    gbufMsaaWarned_ = false;   // the mismatch this guards, if any, is a NEW one under the new state
    // Forced true DIRECTLY here, not left to notifyRenderTargetsChanged()'s gate below: that gate
    // fires only on a sampleCount_/format/sceneWidth_/sceneHeight_ change, which this toggle is not,
    // so calling it alone would silently miss invalidating history right when it matters most --
    // beginFrame's own bind-time decision reasserts this every frame regardless (see
    // gbufHistoryInvalid_'s comment), so this assignment only has to be right for the next frame.
    gbufHistoryInvalid_ = true;
    if (!hasSwapchain_) return;   // applied next createSwapchainResources, same as setRenderScale
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
    // Also runs the SAME notification every other render-target-shaped change goes through --
    // harmless today (none of the four actually move here, so its own gate no-ops), cheaper than
    // being the one call site that skips it.
    notifyRenderTargetsChanged();
}

// Re-adopts all three G-buffer resources in one place, so any of the three accessors below refreshes
// every handle regardless of which is asked first -- see gbufTexDirty_'s comment. Does NOT check
// gbufferEnabled_ itself: every caller has already done so, and this would be a second place that
// check could drift out of sync.
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

// Each accessor below re-checks gbufferEnabled_ (NOT just its own resource) before returning
// anything -- see IDevice::gBufferVelocityTexture (RHI.hpp) for why "0 means off" must hold even
// right after setGBufferEnabled(false), when the adopted handle integers are deliberately left
// non-zero (see releaseGBufferTargets' comment).
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
    // A feature registering after the device already knows its targets -- the common case -- would
    // otherwise learn them only on the NEXT change, which may never come in a run that's never
    // resized. sceneWidth_/sceneHeight_ are 0 only for a swapchain-less device.
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
    // 64-BIT SHADER ATOMICS, asked of the DEVICE. See DeviceCaps::shaderInt64Atomics for why this
    // is not read off the shader model. OPTIONS1 carries Int64ShaderOps, which is the plain
    // 64-bit integer op support a buffer atomic needs; a driver too old to know the query simply
    // fails it and leaves the bit false, which is the correct conservative answer.
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

    // DERIVED AFTER THE CLAMP, ON PURPOSE. --force-caps no-rt exercises the non-RT fallback on
    // hardware that doesn't need it; deriving this before the clamp would leave the bit set while
    // rayTracingTier read 0, so the one flag meant to disable this path wouldn't. Reading the clamped
    // value means every existing override already covers the new bit.
    //
    // Tier 1_1 is the floor the ray-driven path demands, and any device offering it is assumed
    // D3D12_RESOURCE_BINDING_TIER_3 -- checked, since being wrong here means an out-of-bounds
    // descriptor index, not a missing feature.
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
    // LOGGED BESIDE THE SHADER MODEL ON PURPOSE, because the two disagree here and the disagreement
    // is the point: this engine COMPILES at shader model 6.5 while the device may REPORT 6.6, and a
    // consumer that infers 64-bit atomic support from the compile target -- which is exactly what
    // RTXGI's SHaRC does by default -- gets a different answer from the one the hardware just gave.
    // Printing both is what lets somebody notice that rather than discover it as a corrupted hash
    // map or a needlessly allocated lock buffer.
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
        // G-buffer targets themselves need no resize -- always single-sample, unaffected by
        // sampleCount_ -- but WHETHER they can be BOUND alongside msaaColor_ just changed (see
        // createGBufferTargets' MSAA comment), so a warning that fired for the old sampleCount_
        // mustn't suppress the one a NEW mismatch deserves.
        gbufMsaaWarned_ = false;
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
    // Same reasoning VoxiRenderer applies to rtHistValid_ here: whatever the G-buffer held belonged
    // to a resolution/sample-count/format that no longer exists, so reprojecting against it now would
    // reproject against a frame that never happened. beginFrame's bind-time decision is the only
    // other writer and runs AFTER this every frame, so this reset is visible only in the brief window
    // between here and that -- see gbufHistoryInvalid_'s comment for the full contract.
    gbufHistoryInvalid_ = true;
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
    // DEPTH-TESTED NOW, NOT DISABLED. Sky draws AFTER opaque geometry (endFrame, not beginFrame), so
    // the depth buffer already holds real depth for opaque pixels and the cleared far value (1.0)
    // elsewhere; VSky emits o.pos = float4(ndc, 1.0, 1.0), rasterizing at EXACTLY 1.0.
    //
    // EQUAL, NOT GREATER_EQUAL -- caught by a gate that turned the whole image one flat colour.
    // Opaque writes DepthFunc=LESS (1.0 is the far plane), so GREATER_EQUAL is trivially true at the
    // sky's pinned max (nothing can be > 1.0): the sky depth-tested "in front of" geometry that was
    // always in front of it. EQUAL is the real "still at the clear value" test.
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
    // A ROOT SIGNATURE CHANGE DISCARDS EVERY BOUND ROOT ARGUMENT: table 1 and the b2 draw CBV that
    // applyDrawBinding's dbValid_ assumes survived, AND table 0 and the feature frame CBV that
    // drawMesh's fovValid_ assumes survived, both stale the instant this line runs. Cleared HERE
    // rather than at this function's call sites because there are several (beginFrame, drawMesh's raw
    // path, drawLines, the sky dome, the blended replay's setup bind, transparentPass's setup bind)
    // and a new one would not think to do it -- and the failure mode is not a slow frame but a draw
    // reading whatever the previous root signature left behind, which is the black-foliage bug
    // SandboxRender.cpp:1188 records for table 1's own history (d8326985's own comment here cited
    // :1112, which was already wrong when it was written -- the walk had moved past that line by
    // then; corrected while this paragraph was being rewritten rather than left to mislead a second
    // reader). Only reached on a genuine change, because of the early-out above, so an unchanged root
    // signature still costs nothing.
    //
    // fovValid_ USED TO BE DELIBERATELY LEFT OUT HERE (see d8326985, which gave dbValid_ this clear
    // alone, reasoning that fovValid_ "has exactly the same exposure, predates this cache, and ships
    // today" but that fixing it belonged in its own change rather than being smuggled into one meant
    // to be provably inert). This IS that change. The mechanism: drawMesh's feature-pipeline branch
    // can leave fovValid_ true while forcing boundRootSig_/boundPso_ to nullptr right after its draw
    // (so THIS backend's own raw path never mistakes the feature's root signature for its own) --
    // if any later bindGraphicsRoot call is then reached before fovValid_ is next cleared some other
    // way, it sees boundRootSig_ == nullptr, takes the branch above for real, and silently discards
    // the table-0 binding and frame CBV that a subsequent "same pipeline as fovPso_/fovSet_/fovCb_"
    // comparison in drawMesh (or drawMeshDepthPrepass, or the blended replay) would then trust without
    // re-sending them -- a draw reading whatever this function's root signature and PSO left behind
    // instead of the feature's, a wrong image rather than a slow one, the exact shape of the foliage
    // bug above, just for table 0 instead of table 1.
    //
    // NOT KNOWN TO FIRE TODAY: every bindGraphicsRoot call this file makes outside drawMesh's own raw
    // path (beginFrame's opening bind, the sky dome, the blended replay's setup bind, transparentPass's
    // setup bind, all in endFrame) runs after endFrame's own top-of-function fovValid_ = false, with
    // nothing in between able to set it true again before each of those sites is reached in turn --
    // traced by hand, not assumed, which is why THOSE three sites in runPostChain that gained a
    // matching dbValid_ clear in d8326985 (its own opening bind, the eye-adaptation compute round
    // trip, and the post-upscaler restore) are deliberately NOT given one here: dbValid_'s clears
    // there are the same kind of already-redundant belt-and-suspenders dbValid_ got everywhere a root
    // signature changes, not evidence fovValid_ needs the same at those particular three -- adding it
    // there would be inert churn, not a fix. And within drawMesh's own raw path, VoxiRenderer (the one
    // feature shipped today that overridesScenePipeline()) only ever answers scenePipeline() with 0
    // for wireframe_, a flag that does not vary between entities within one frame, so today's single
    // feature cannot actually produce the mid-walk mix of feature-path and raw-path draws the
    // mechanism above needs. The gap was real and reachable in principle regardless -- RHIResources.hpp
    // documents a feature answering scenePipeline() per draw as ordinary, drawMesh's `if (!fp) break`
    // exists specifically to fall back correctly when it does, and nothing stops a second feature or a
    // future VoxiRenderer change from making that per-draw answer vary -- which is why this is fixed
    // now rather than left for whichever future feature trips it first.
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
    // Only when a caller enabled the feature BEFORE this swapchain existed (setGBufferEnabled's early
    // return defers to here for that case) -- an ordinary build, which never calls
    // setGBufferEnabled, skips this and stays bit-identical to before this feature existed. NOT fatal
    // to swapchain creation on failure, unlike the depth/colour targets above: this is optional and
    // additive, so the swapchain still comes up, just without the G-buffer.
    if (gbufferEnabled_ && !createGBufferTargets()) {
        AVER_ERROR("[RHI.D3D12] G-buffer target creation failed at swapchain creation; disabling the feature");
        releaseGBufferTargets();
        gbufferEnabled_ = false;
    }

    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!hrOk(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])), "CreateCommandAllocator")) return false;
    }
    if (!hrOk(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&cmdList_)), "CreateCommandList")) return false;
    cmdList_.As(&cmdList4_);
    cmdList_.As(&cmdList6_);
    cmdList_->Close();
    // initAccelerationStructures() already ran unconditionally from init() -- see the comment there.
    // cmdList4_ is still acquired here: buildBlas/buildTlas record onto it, a separate requirement
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
    // EXPLICIT view desc, where a bare `nullptr` used to suffice: CreateDepthStencilView infers the
    // view format only when the resource is NOT typeless, and a typeless one also needs to be told
    // whether it's multisampled -- not otherwise recoverable from a TYPELESS resource description.
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = kDepthFormat;
    dv.ViewDimension = (sampleCount_ > 1) ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthBuffer_.Get(), &dv, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    // Resource just changed identity (fresh allocation, possibly new size/sample count);
    // sceneDepthTexture() re-adopts it lazily on next ask, rather than eagerly here where no
    // IResourceFactory call is guaranteed safe yet (ahead of rhiFactory_ existing on the first call).
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

// Creates the three G-buffer targets at the CURRENT scene size (sceneWidth_/sceneHeight_, matching
// createDepthBuffer/createMsaaColor, not width_/height_ -- the editor docks the 3D view in a
// sub-rect, and the wrong pair would crash on a mismatched bind or sample the wrong texel). Callers
// Reset() the three ComPtrs first.
//
// ALWAYS SINGLE-SAMPLE regardless of sampleCount_: a G-buffer is read back by a COMPUTE pass (the
// FidelityFX denoiser, eventually FSR2/3/TAA/SSR), none of which consume Texture2DMS -- designing
// that consumer is out of scope here -- and velocity/depth/normal are per-sample data MSAA
// averaging wouldn't correctly resolve anyway (pick-one-sample, not blend).
//
// CONSEQUENCE: OMSetRenderTargets requires every bound target to share one SampleDesc, so when
// sampleCount_ > 1 these can't be bound alongside msaaColor_. beginFrame's bind-time branch skips
// binding these three then, still clears them to their "nothing here" sentinel, and warns once
// (gbufMsaaWarned_). setGBufferEnabled(true) with MSAA on is accepted, not refused, but silently
// yields an all-sentinel G-buffer without that warning.
bool D3D12Device::createGBufferTargets() {
    if (sceneWidth_ == 0 || sceneHeight_ == 0) return false;

    auto makeTarget = [&](DXGI_FORMAT fmt, const f32 clearColor[4], ComPtr<ID3D12Resource>& outRes,
                          ComPtr<ID3D12DescriptorHeap>& outHeap, const char* debugName) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = sceneWidth_; td.Height = sceneHeight_;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = 1;   // see this function's own top comment
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

    // The three clear values live at file scope (kGBufVelocityClear and its siblings, above
    // kGBufVelocityFormat) so this resource's OPTIMISED clear value and beginFrame's per-frame clear
    // can't drift apart. See that comment block for why each value was chosen (velocity 0 = a real
    // "static" reading; viewZ 0 = a depth no real pixel produces; normal+roughness (0.5,0.5,0.5,0) =
    // the encoding's zero, decoding to the invalid (0,0,0) direction).
    if (!makeTarget(kGBufVelocityFormat, kGBufVelocityClear, gbufVelocity_, gbufVelocityRtvHeap_, "GBuffer.Velocity"))
        return false;
    if (!makeTarget(kGBufViewZFormat, kGBufViewZClear, gbufViewZ_, gbufViewZRtvHeap_, "GBuffer.ViewZ"))
        return false;
    if (!makeTarget(kGBufNormalRoughFormat, kGBufNormalRoughClear, gbufNormalRough_, gbufNormalRoughRtvHeap_,
                    "GBuffer.NormalRoughness"))
        return false;

    gbufTexDirty_ = true;   // freshly (re)allocated; the adopted TextureHandles must re-adopt
    return true;
}

// Releases the three G-buffer targets and their RTV heaps, the mirror of createGBufferTargets(),
// called from setGBufferEnabled(false) so disabling frees its ~54 MB immediately rather than leaving
// them on the GPU for the rest of the run. The three adopted TextureHandles are deliberately left as
// they are: every accessor checks gBufferEnabled() FIRST and returns 0 without looking at the handle,
// so nothing outside this file can observe a handle that outlived its resource (RHI.hpp's "check the
// flag, not the handle" contract).
void D3D12Device::releaseGBufferTargets() {
    gbufVelocity_.Reset();    gbufVelocityRtvHeap_.Reset();
    gbufViewZ_.Reset();       gbufViewZRtvHeap_.Reset();
    gbufNormalRough_.Reset(); gbufNormalRoughRtvHeap_.Reset();
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

    // ONE PASS OVER THE VERTICES, ONCE, AT CREATION. An AABB, not a tight sphere: min/max per axis,
    // centre = midpoint, radius = distance to a CORNER of the box (not the farthest actual vertex),
    // so it's never smaller than a true bounding sphere -- conservative in the direction a culling
    // test wants: false positives cost GPU cycles, false negatives cost a wrong picture.
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

    // W4: which heap this mesh's buffers land on. Starts as the caller's current standing request
    // (setStaticMeshHeapDefault) and is forced false below if the Default-heap path fails, so one
    // mesh's bad luck (an out-of-memory staging allocation, say) falls back instead of failing this
    // call outright -- the Upload heap is what every mesh already lived on before this flag existed.
    bool useDefault = staticMeshDefaultHeap_;

    // THROUGH THE FACTORY, not CreateCommittedResource: same heap and contents either way, but only
    // a factory buffer has an RhiBuffer entry and can be given a descriptor -- without it a shader
    // could never read a mesh's own geometry, which a ray needs beyond a plain hit test.
    //
    // A lambda rather than inlining the Default/Upload branches into two near-duplicate blocks below:
    // it is called once for the caller's requested heap and, on failure of a Default-heap attempt,
    // called again for Upload -- the ONLY two call shapes this needs, and writing the allocate+upload
    // sequence out twice is exactly how the two copies drift (one gets a bugfix the other doesn't).
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
            // SYNCHRONOUS, deliberately, not queued the way seedSkinTargets defers its own copy:
            // createMesh is also called MID-FRAME (the scene walk streaming a chunk in, SandboxApp's
            // part split), and a mesh drawn or BLAS-built later in that SAME frame must never read
            // uninitialised Default-heap memory. Queuing this the way skin targets are seeded would
            // reopen exactly that window for every ordinary static mesh. The one-shot list this
            // issues reaches the queue and is waited on before createMesh returns, so the frame's own
            // command list -- recorded afterwards -- is guaranteed to see initialised data.
            // UNMEASURED: the GPU round trip this adds at load time, once per mesh, has not been
            // timed against the per-frame bus traffic it removes; see W4's brief.
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

// W11: creates a mesh that SHARES `source`'s vertex buffer and owns its own index buffer -- the
// inverse split from createSkinTargetMesh just below (shared indices, own compute-written vertices).
// See IDevice::createMeshSharingVertices (RHI.hpp) for the full refcounting and refusal contract;
// this is its D3D12 implementation.
MeshHandle D3D12Device::createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) {
    return shareVertices(source, indices, indexCount, /*posed=*/false);
}
MeshHandle D3D12Device::createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) {
    return shareVertices(posedSource, indices, indexCount, /*posed=*/true);
}
// Shared body. `posed` flips exactly three things: which source is accepted (compute-written or not),
// whether every index is range-checked, and whether the result is itself compute-written.
MeshHandle D3D12Device::shareVertices(MeshHandle source, const u32* indices, u32 indexCount, bool posed) {
    const char* what = posed ? "createPosedPartMesh" : "createMeshSharingVertices";
    if (!device_ || !rhiFactory_ || !indices || indexCount == 0) return 0;
    if (source == 0 || source > meshes_.size()) {
        AVER_ERROR("[RHI.D3D12] {} with an invalid source handle", what);
        return 0;
    }
    const GpuMesh& src = meshes_[source - 1];
    // Silent refusals, the same shape as createSkinTargetMesh's own "not fully formed" checks just
    // below: a caller offering one of these is expected to fall back to createMesh (IDevice's own
    // contract says so), not to be told why in the log every time an LOD importer merely PROBES
    // whether sharing is available for a given asset.
    if (!src.alive || !src.vb || src.vbv.SizeInBytes == 0) return 0;
    // LOD sharing refuses a compute-written source SILENTLY (its caller probes and falls back); a
    // posed part REQUIRES one and says so -- its caller's only fallback is the whole-mesh draw.
    if (src.computeWritten != posed) {
        if (posed) AVER_WARN("[RHI.D3D12] createPosedPartMesh refused: mesh {} is not compute-written", source);
        return 0;
    }
    // Every index must name a vertex the posed buffer actually has: these indices come from content
    // (a slice of the base mesh's list), not from this device, and an out-of-range index is a GPU
    // read past the end of a buffer that compute rewrites every frame.
    if (posed) {
        for (u32 k = 0; k < indexCount; ++k)
            if (indices[k] >= src.vertexCount) {
                AVER_WARN("[RHI.D3D12] createPosedPartMesh refused: index {} names vertex {} of {}-vertex mesh {}",
                          k, indices[k], src.vertexCount, source);
                return 0;
            }
    }

    // Collapse a sharing CHAIN to its one root rather than letting a sharer become another sharer's
    // source: sharing FROM `source` shares the SAME underlying buffer `source` itself shares (or
    // owns), so the root is source's own root when source is itself a sharer. This is what keeps
    // destroyMesh's give-back a single decrement on one root, never a walk through N levels.
    const MeshHandle root = src.vbOwned ? source : src.vbSource;
    if (root == 0 || root > meshes_.size() || !meshes_[root - 1].alive) return 0;

    GpuMesh m;
    // Copied from `src`, not re-derived from `root`: src.vb/vbv/vbBuffer already mirror the root's
    // buffer whether src is the root itself or itself a sharer (this function sets them identically
    // either way, just below), so reading them off src is correct and needs no special case for a
    // chain. Bounds likewise -- a coarser index list over the SAME vertex positions can never exceed
    // the shared buffer's own extents, so the source's bounds are exactly the sharer's bounds too;
    // createMesh on this same vertex array would compute the identical sphere and AABB.
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
    // Through the SAME Upload/Default policy createMesh() itself uses (staticMeshDefaultHeap_ and its
    // one-mesh fallback-on-failure), so a coarser LOD level built while --mesh-heap default is set
    // behaves exactly like an ordinary mesh created under it -- see createMesh's own comment on why
    // this is a lambda rather than a second near-duplicate of the allocate+upload sequence.
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
    // Counted on the ROOT, AFTER push_back -- `src` and any earlier reference to `root`'s slot in
    // meshes_ may have been invalidated by the reallocation above, the identical hazard
    // createSkinTargetMesh's own ibShares increment documents (see its comment there).
    meshes_[root - 1].vbShares += 1;
    return h;
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
    // Otherwise defaults to centre (0,0,0), radius 0: createMesh measures an AABB at creation, but a
    // skin target has no CPU vertices (the skinning pass writes them on the GPU), so every soft body
    // and skinned mesh reported a radius-0 sphere at the origin -- a frustum cull could vanish the
    // whole body once that point left view, the same failure shape as the cull that once starved
    // shadows/GI of off-screen casters. Found when a point-in-volume test against the pool's water
    // reported the camera outside a sphere it was 20cm inside of.
    //
    // The source's bounds are honest, not perfect: the seed shell is where geometry starts and stays
    // at rest, but deformation (a sloshing fluid) can push a vertex outside it -- still strictly
    // better than a point at the origin, and the only answer available without a GPU readback.
    m.boundsCentre[0] = src.boundsCentre[0];
    m.boundsCentre[1] = src.boundsCentre[1];
    m.boundsCentre[2] = src.boundsCentre[2];
    m.boundsRadius    = src.boundsRadius;
    for (int a = 0; a < 3; ++a) { m.boundsMin[a] = src.boundsMin[a]; m.boundsMax[a] = src.boundsMax[a]; }
    // THE INDEX BUFFER'S HANDLE COMES ACROSS TOO, not just its raw pointer: meshGeometry() refuses
    // on `!m.vbBuffer || !m.ibBuffer`, and ibBuffer defaulted to 0 here, so every skin target reported
    // "no readable geometry" though its indices are perfectly readable -- one skinned entity switched
    // ray-traced reflections off for the whole scene because the BLAS build couldn't see its geometry.
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
        // implicitly and needs no barrier to get there. The source USED TO BE guaranteed an
        // upload-heap resource permanently in GENERIC_READ, needing no barrier either -- W4 broke
        // that guarantee: with --mesh-heap default, `s` may itself be a Default-heap mesh (an
        // ordinary static mesh someone is skinning FROM), whose buffer this same copy also promotes
        // implicitly, COMMON -> COPY_SOURCE this time, and which therefore needs the identical
        // explicit undo the destination gets below. An upload-heap source is NEVER barriered here --
        // GENERIC_READ is that heap type's one fixed state (RhiBuffer::stateFixed under
        // AVER_RHI_TRACK_STATE refuses any transition off it), and the runtime never actually moves
        // it off GENERIC_READ for a copy source in the first place.
        const bool srcIsDefault = [&] {
            RhiBuffer* srb = rhiFactory_->buffer(s.vbBuffer);
            return srb && srb->desc.kind == BufferKind::Default;
        }();
        cmdList_->CopyBufferRegion(d.vb.Get(), 0, s.vb.Get(), 0, d.vbv.SizeInBytes);

        // The promotion LASTS FOR THE REST OF THE COMMAND LIST -- decay happens at submit, not at
        // the end of the copy. Without this the skinning pass's first barrier claims Common on a
        // resource the runtime knows is COPY_DEST, and the debug layer reports it once per frame
        // forever.
        auto back = transition(d.vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COMMON);
        cmdList_->ResourceBarrier(1, &back);
        if (srcIsDefault) {
            // Mirrors `back` just above for COPY_SOURCE instead of COPY_DEST -- same reasoning,
            // same lifetime (this command list), same reason it must be explicit rather than left to
            // decay at submit: the skinning pass's own later read of `s.vb` (this mesh is still an
            // ordinary drawable mesh, not exclusively a skin source) must find it back at COMMON.
            auto srcBack = transition(s.vb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                      D3D12_RESOURCE_STATE_COMMON);
            cmdList_->ResourceBarrier(1, &srcBack);
        }
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
    // -- see GpuAccum's comment. PARENTS BEFORE CHILDREN IS GUARANTEED: a span's parent was already
    // open when the span itself opened, so it always lands at a LOWER index in tsSlice_, and walking
    // in order always resolves tsSpanToAccum_[s.parent] before a child needs it.
    tsSpanToAccum_.assign(tsSlice_[frameIndex_].size(), kNoAccumParent);
    for (u32 i = 0; i < tsSlice_[frameIndex_].size(); ++i) {
        const GpuSpan& s = tsSlice_[frameIndex_][i];
        if (s.begin >= kMaxGpuStamps || s.end >= kMaxGpuStamps) continue;   // never closed; drop it
        const f64 d = ms(stamps[s.begin], stamps[s.end]);
        // A span whose PARENT was itself dropped (mismatched push/pop) has nowhere honest to nest;
        // folding it in as top-level, not under whatever stale index sits in tsSpanToAccum_[s.parent],
        // turns that bug into a visibly wrong "unmarked" instead of a plausible-looking tree.
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

        // Printed as an indented tree: label, INCLUSIVE ms/frame (begin/end's own measurement), and
        // EXCLUSIVE ms/frame (inclusive minus direct children) in parentheses -- the number this
        // engine could never print before nesting existed, since a flat list smeared a child's cost
        // across its parent's total.
        //
        // A local functor, not std::function: the one recursive local closure this file needs,
        // everything it touches already in scope, and no reach for <functional> otherwise.
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

// Public mirror of tsAccum_ for a caller outside this file (the command console's frame-time
// breakdown -- see IDevice::gpuTiming for the two-frames-old rationale). A STRAIGHT COPY: tsAccum_
// is already the flat, parent-indexed tree GpuTimingNode mirrors, so this just divides each node's
// ms by the frame count and copies label/parent unchanged. kNoAccumParent and
// GpuTimingNode::kNoParent share the same sentinel (0xFFFFFFFFu), so no remapping needed.
GpuTimingReport D3D12Device::gpuTiming() const {
    GpuTimingReport report;
    // `supported` is the capability axis: false here means this device cannot report timings at
    // all (timestamp queries unavailable on this adapter), independent of whether any frame has
    // been collected yet -- see GpuTimingReport's own comment on why the two are kept apart.
    report.supported = tsEnabled_;
    if (!tsEnabled_ || tsAccumFrames_ == 0) return report;
    report.framesAccumulated = tsAccumFrames_;
    const f64 n = static_cast<f64>(tsAccumFrames_);
    report.nodes.reserve(tsAccum_.size());
    for (const GpuAccum& a : tsAccum_)
        report.nodes.push_back(GpuTimingNode{a.label, a.ms / n, a.parent});
    return report;
}

// Opens the frame: waits out the current backbuffer's last frame, resets recording, clears targets.
void D3D12Device::beginFrame() {
    if (!hasSwapchain_ || deviceLost_) return;
    // FIRST, BEFORE ANYTHING RECORDS: a render-scale change frees and recreates the depth buffer,
    // MSAA target and post chain, and doing that mid-recording is what removed the device at Present.
    // See setRenderScale's comment for the full account.
    applyPendingRenderScale();
    reconcileClearValue();
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    const u64 want = fenceValues_[frameIndex_];
    // THE RESULT IS ACTED ON -- discarding it was the whole bug. waitFence detected a removed device
    // and returned false, but nobody looked, so every frame kept resetting an allocator and recording
    // for a device that would never run it, paying waitFence's full one-second timeout each time.
    // Returning here makes the loss cost one frame instead of every frame.
    if (want != 0 && !waitFence(want)) return;
    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get());
    boundRootSig_ = nullptr;
    boundPso_ = pso_.Get();   // Reset's second argument IS the command list's initial bound PSO
    // Reset() does not carry descriptor heaps forward either -- a freshly reset command list has
    // none bound until the first SetDescriptorHeaps of the new recording, same as the root signature.
    boundHeap_ = nullptr;
    fovValid_ = false;   // a reset command list has nothing bound at all
    dbValid_ = false;    // table 1's own cache dies here too -- see its member comment for why it
                          // can't just ride fovValid_ instead of getting its own line
    postCBUsed_ = 0;
    drawBinding_ = defaultDrawBinding_;
    drawBlended_ = false;   // sticky per-draw state resets exactly like drawBinding_ just above
    depthOnlyMesh_ = 0;     // drawMesh consumes it; this is the backstop for an unpaired depth-only draw
    // Cleared here, not right after endFrame's flush drains it: both leave an empty list (nothing
    // between a flush and the next beginFrame calls drawMesh), but clearing only here keeps ONE place
    // deciding "a new frame's captures start empty" -- the same discipline drawBinding_,
    // nextDrawPrepassed_ and the depth-prepass counters below all follow.
    blendedDraws_.clear();
    blendedPipelineMissingWarned_ = false;   // said at most once per frame; see its own member comment
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

    // G-buffer bind decision -- see createGBufferTargets()'s top comment for the full MSAA reasoning.
    // Bound ONLY when the feature is on, the three targets actually exist (might not yet, on the
    // first frame after a failed setGBufferEnabled(true)), AND the scene colour target is
    // single-sample -- OMSetRenderTargets requires every bound target to share one SampleDesc, and
    // these three are ALWAYS single-sample, so binding them alongside an MSAA msaaColor_ can never
    // be valid.
    const bool gbufWritable = gbufferEnabled_ && sampleCount_ == 1 &&
                              gbufVelocity_ && gbufViewZ_ && gbufNormalRough_;
    if (gbufferEnabled_ && sampleCount_ > 1 && !gbufMsaaWarned_) {
        // ONCE PER MISMATCH, not once per frame (gbufMsaaWarned_ clears when setGBufferEnabled or
        // setSampleCount actually changes something): this state can legitimately persist a whole
        // editor session, and repeating every frame would train a reader to stop reading warnings.
        AVER_WARN("[RHI.D3D12] G-buffer is enabled but MSAA is {}x; it REQUIRES sampleCount() == 1 "
                  "to be bound (D3D12 requires every render target in one OMSetRenderTargets call to "
                  "share a sample count, and the G-buffer's three targets are always single-sample) "
                  "-- it is being CLEARED but NOT WRITTEN this frame, and every frame after, until "
                  "MSAA drops to 1x", sampleCount_);
        gbufMsaaWarned_ = true;
    }
    // See gbufHistoryInvalid_'s comment for the two-part contract this is HALF of (the other half
    // lives in notifyRenderTargetsChanged): this frame's write status, decided fresh every frame
    // rather than only on a change, so disabling the feature or drifting into an MSAA mismatch is
    // invalidated on the VERY NEXT frame with no separate edge-triggered reset.
    gbufHistoryInvalid_ = !gbufWritable;

    if (gbufWritable) {
        // 4 render targets: scene colour at slot 0, then velocity/viewZ/normal-roughness at 1/2/3 --
        // the SAME order GraphicsPipelineDesc::renderTargets declares SV_TARGET0..3 in. A PSO that
        // only writes SV_TARGET0 (every pipeline this backend builds, and any unmigrated
        // IRenderFeature) simply never touches slots 1-3; that's ordinary, well-defined D3D12 MRT
        // behaviour (a draw that writes fewer targets than are bound leaves the others exactly as
        // they were), not a validation error -- why those slots are cleared to their "nothing here"
        // sentinel below rather than left at a previous frame's bytes.
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
            rtv,
            gbufVelocityRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
            gbufViewZRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
            gbufNormalRoughRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
        };
        cmdList_->OMSetRenderTargets(4, rtvs, FALSE, &dsv);
    } else {
        // EXACTLY the call this line made before the G-buffer existed: 1 render target, the DSV.
        // "Feature disabled" (gbufferEnabled_ == false -- every build today) and "feature enabled
        // but MSAA makes it unbindable" both reproduce this identical bind, which is the guarantee
        // the render-gate oracle depends on -- see setGBufferEnabled's own top comment ("additive
        // and defaulted the whole way down").
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }
    cmdList_->ClearRenderTargetView(rtv, sceneClear_, 0, nullptr);
    if (gbufferEnabled_ && gbufVelocity_ && gbufViewZ_ && gbufNormalRough_) {
        // Cleared REGARDLESS of gbufWritable -- even on the MSAA-mismatch path above, where these
        // three are not bound this frame, a caller that reads gBufferVelocityTexture() etc. directly
        // (rather than trusting the bind) still finds this frame's honest "nothing here" sentinel,
        // not a value quietly going stale for however long the mismatch persists. ClearRenderTargetView
        // only needs the resource in RENDER_TARGET state, which createGBufferTargets left it in and
        // nothing in this file ever transitions it out of.
        cmdList_->ClearRenderTargetView(gbufVelocityRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                        kGBufVelocityClear, 0, nullptr);
        cmdList_->ClearRenderTargetView(gbufViewZRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                        kGBufViewZClear, 0, nullptr);
        cmdList_->ClearRenderTargetView(gbufNormalRoughRtvHeap_->GetCPUDescriptorHandleForHeapStart(),
                                        kGBufNormalRoughClear, 0, nullptr);
    }
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
    IRenderFeature* winner = nullptr;
    u32 claimants = 0;
    for (IRenderFeature* f : features_) {
        if (!f->suppressesScene()) continue;
        ++claimants;
        if (!winner) winner = f;
    }
    // WHO IS PAINTING THE SCENE, SAID OUT LOUD, once per change -- silence here is what made this
    // expensive. The trap is NOT the two-claimant case: a manifest asking for both a path-traced
    // reference view and ray-driven primary visibility has two claimants, ray-driven wins on
    // registration order, and turning ray-driven off to "compare against the rasteriser" leaves
    // exactly ONE claimant (the path tracer) quietly taking the frame while raster never runs. That
    // measured a fullscreen path-traced blit as "the raster path" (`scene draw 0.1ms` against a
    // 3.7ms ray span, reported as a 3x). So the single-claimant case is logged too, at INFO -- a
    // legitimate configuration, just never silent. See IRenderFeature::suppressesScene.
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
    if (winner) {
        if (rhiContext_) winner->scenePass(*rhiContext_);
        cmdList_->SetPipelineState(pso_.Get());
        sceneSuppressed_ = true;
        if (winner->suppressesWholeFrame()) frameSuppressed_ = true;
        return;
    }

    // THE SKY NO LONGER DRAWS HERE. It used to run first, depth-disabled, at 100% coverage regardless
    // of how much the final image's opaque geometry would go on to cover -- the most expensive shader
    // in this file (PSky's atmosphere march) paying full price behind every wall, tree and character.
    // It now draws at the START of endFrame, after every opaque drawMesh this frame, once the depth
    // buffer holds real depth -- see that function and the sky PSO's now depth-tested creation above
    // for why this is correct: it lands on the SAME still-bound render target and depth buffer,
    // before endFrame's first GPU work (the MSAA resolve) reads either.
    cmdList_->SetPipelineState(pso_.Get());
}

// Draws one mesh into the scene, through whichever pipeline owns the lit pass.
// Releases a mesh's GPU memory. See IDevice::destroyMesh for the handle-recycling argument.
bool D3D12Device::destroyMesh(MeshHandle mesh) {
    if (mesh == 0 || mesh > meshes_.size()) return false;
    GpuMesh& m = meshes_[mesh - 1];
    if (!m.alive) return false;   // already destroyed; saying so beats double-freeing

    // A source mesh whose indices someone else still shares cannot go. Refusing loudly is the point:
    // freeing anyway would leave the skin target rendering from memory the heap handed to something
    // else -- scrambled triangles somewhere unrelated, not an error here.
    if (m.ibShares > 0) {
        AVER_WARN("[RHI.D3D12] destroyMesh({}) refused: {} skin target(s) still share its indices",
                  mesh, m.ibShares);
        return false;
    }
    // W11: the mirror-image refusal for a vertex-sharing ROOT -- same reasoning as ibShares just
    // above (createSkinTargetMesh's sharers), aimed at createMeshSharingVertices's sharers instead.
    // Freeing a shared vertex buffer out from under a still-live LOD mesh would leave it drawing (or
    // being BLAS-built, or read as ray-traced geometry) from memory the heap has handed to something
    // else -- silent corruption elsewhere, not an error here.
    if (m.vbShares > 0) {
        AVER_WARN("[RHI.D3D12] destroyMesh({}) refused: {} mesh(es) still share its vertices",
                  mesh, m.vbShares);
        return false;
    }

    // The acceleration structures FIRST. A BLAS holds this mesh's vertex and index GPU addresses,
    // so releasing the buffers while one is live would leave ray tracing traversing freed memory --
    // and unlike a raster draw, that faults the device rather than drawing a hole.
    if (rhiFactory_) rhiFactory_->destroyBlasForMesh(mesh);

    // Then the buffers, through the factory, so they retire behind the fence rather than being
    // released while a command list still in flight references them.
    if (rhiFactory_) {
        // ONLY IF OWNED. A vertex-sharing mesh's vertices belong to its root (W11, mirroring the
        // index-ownership check just below for a skin target).
        if (m.vbOwned && m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        // ONLY IF OWNED. A skin target's indices belong to its source.
        if (m.ibOwned && m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
    }
    // Give the source its share back, so a source held open only by this target can now go too.
    if (!m.ibOwned && m.ibSource != 0 && m.ibSource <= meshes_.size()) {
        GpuMesh& src = meshes_[m.ibSource - 1];
        if (src.ibShares > 0) src.ibShares -= 1;
    }
    // W11: the same give-back for a vertex-sharing mesh's root.
    if (!m.vbOwned && m.vbSource != 0 && m.vbSource <= meshes_.size()) {
        GpuMesh& root = meshes_[m.vbSource - 1];
        if (root.vbShares > 0) root.vbShares -= 1;
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
    m.vbOwned = true;
    m.vbSource = 0;
    m.boundsRadius = 0.0f;
    m.alive = false;
    return true;
}

// Draws `mesh`'s depth only, through whichever feature's depthPrepassPipeline() offers one -- see
// IDevice's comment for the contract, and VoxiRenderer::depthPrepassPipeline for the one
// implementation today. STRUCTURALLY A SMALL COPY OF drawMesh()'s feature-pipeline branch below
// (same fovPso_/fovSet_/fovCbBytes_ caching, table-1 rebind, PerObject world write) -- same kind of
// draw through the same seam, just a different pipeline and no colour/material tail. NOT folded into
// drawMesh() itself: called from a SEPARATE, earlier walk (SandboxApp's prepass phase), never
// interleaved per-instance with colour draws -- that would split one contiguous "depth prepass" GPU
// span into hundreds of one-draw slivers, and this engine's GPU stat tree budgets 64 open spans a
// frame, not one per entity.
// The frame-wide prepass: gated on its switch, and counted -- that count is the pass's own census.
void D3D12Device::drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
    if (!depthPrepassEnabled_) return;
    if (depthOnlyDraw(mesh, world, color, /*allowComputeWritten=*/false)) ++depthPrepassDrawsThisFrame_;
}

// One draw's own depth, for an alpha-masked draw -- see IDevice::drawMeshDepthOnly. NOT gated on the
// frame-wide switch and NOT counted in its census, which describes the frame-wide pass only.
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
    // COMPUTE-WRITTEN (skinned/soft-body) MESHES STAY EXCLUDED FROM THE FRAME-WIDE PREPASS -- that
    // walk never resolves a posed handle, so it would depth-draw the base mesh -- and are ACCEPTED by
    // the per-draw path (drawMeshDepthOnly). That is safe for three reasons, each checked:
    //   - a skin target's vbv/vb point at the compute-written buffer itself (createSkinTargetMesh),
    //     and D3D12RenderContext::drawMesh binds m.vbv -- so depth and colour read the same bytes;
    //   - skinning dispatches from SkinnedScene::prePass inside beginFrame, before any walk, and
    //     leaves the buffer in GeometryRead -- so this frame's pose, in a readable state;
    //   - the caller's colour draw of the SAME handle follows immediately (GameRender's colour loop).
    // drawMesh() then honours `prepassed` for it only via depthOnlyMesh_, the exact handle drawn here.
    // Without that third gate the colour draw would take the Less/write pipeline and reject the equal
    // depth just written -- the hair would vanish instead of leaking.
    if (!allowComputeWritten && meshVertexBuffer(mesh) != 0) return false;
    // NO RASTER COLOUR PASS TO CONSUME IT, so no raster depth either. Wireframe draws through the
    // backend's own Less/write pipeline (scenePipeline declines it), which would reject the mesh's
    // own edges against depth written here -- every prepassed mesh vanished in wireframe. And when a
    // feature suppresses the scene (ray-driven primary visibility, a debug view), drawMesh returns
    // before any colour draw while that feature's own pass has already written the frame's depth;
    // writing raster depth over it wherever raster rounds nearer is wrong, not merely wasted. Both
    // mirror drawMesh's own tests, so this pass and the colour pass cannot disagree about whether a
    // raster colour draw happens. Found by adversarial review.
    if (wireframe_) return false;
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return false;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline()) continue;
        const PipelineHandle pp = f->depthPrepassPipeline();
        if (!pp) return false;   // this feature has no prepass PSO; nothing else offers one either today
        const BindingSetHandle bs = f->sceneBindingSet();
        const void* cb = nullptr; u32 cbBytes = 0;
        const bool haveCb = f->sceneConstants(&cb, &cbBytes) && cb && cbBytes;

        // Same elision drawMesh() uses below, deliberately the SAME cache variables: switching
        // between the prepass PSO and the colour PSO always re-sends table 0 and the frame CB even
        // though both are the SAME Voxi resources -- over-conservative, not wrong.
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
        // Table 1: the SAME material binding set via setDrawBinding for this instance's colour draw
        // -- PSDepthPrepass reads gBaseColorMap/gAlphaCutoff/gMaterialFlags from it at the same
        // registers PSMainVoxi does (both share `gi`, VoxiRenderer.cpp's giLayout()).
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        f32 fc[kObjectConstantDwords] = {};
        std::memcpy(fc, world, 16 * sizeof(f32));
        // gBaseColor, in the slots drawMesh fills: PSDepthPrepass's alpha test multiplies by its .a.
        // Left zero, every alpha-masked material computed alpha 0 and clipped every pixel. See
        // IDevice::drawMeshDepthPrepass.
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
    // AUTO-CONSUME nextDrawPrepassed_ before any early return below, per its own contract: the flag
    // must not leak onto a later, unrelated draw just because this one bailed out early.
    // A compute-written mesh counts as prepassed ONLY if drawMeshDepthOnly just depth-drew this exact
    // handle; otherwise its depth may be absent and LessEqual/no-write would drop it entirely.
    const bool prepassed = nextDrawPrepassed_ &&
        (meshVertexBuffer(mesh) == 0 || (depthOnlyMesh_ != 0 && mesh == depthOnlyMesh_));
    nextDrawPrepassed_ = false;
    depthOnlyMesh_ = 0;
    if (!hasSwapchain_ || mesh == 0 || mesh > meshes_.size()) return;
    // A destroyed mesh draws NOTHING rather than drawing from a cleared vertex view. This is the
    // other half of not recycling handles: a caller that kept a handle too long gets a visible hole
    // it can trace, not a device removal.
    if (!meshes_[mesh - 1].alive) return;

    // Every feature still sees a blended draw; only the BACKEND's own opaque consumers don't -- see
    // IDevice::setDrawBlended (RHI.hpp).
    //
    // submitDraw runs UNCONDITIONALLY here (blended=true), before the capture: Voxi must IGNORE a
    // blended draw (voxelising/shadowing/TLAS-ing glass leaks light, paints a black silhouette, makes
    // its reflections opaque) while the path tracer must ACCEPT it (a dielectric is the one surface
    // it models correctly) -- each feature's own submitDraw reads `blended` to decide, so this call
    // site's job is only to hand every feature the draw, never to pre-filter.
    //
    // The capture keeps a translucent instance out of THIS BACKEND's own opaque consumers
    // (scenePipeline, the depth prepass, the deferred sky's depth-EQUAL fill, the TLAS/voxel-GI
    // builders) -- none of which can fold coverage into what they build. Backend ordering, not a
    // feature decision.
    //
    // `prepassed` is silently dropped here: a blended pipeline never writes depth, so there's nothing
    // for a same-frame prepass to feed.
    //
    // Not gated on suppressesScene() here: the blended flush in endFrame gates on frameSuppressed_
    // instead, because a feature suppressing only the raster surface still wants its glass.
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
        // UNLIT STAYS ON THE FEATURE'S PIPELINE. It used to break out to this backend's own PSO,
        // on the reasoning that the unlit bypass lived only in the shared prelude's
        // `gMaterial.z > 0.5` and Voxi's scene shader had no unlit branch to take. The premise was
        // true and the consequence was PURE WHITE: the backend's fallback shader is compiled before
        // the material prelude exists, so it cannot sample a base-colour texture, and the draw loops
        // deliberately hand it gBaseColor = 1,1,1 for every authored material precisely because the
        // real colour is supposed to arrive through a texture binding it never reads. Unlit on any
        // textured mesh was therefore a white silhouette rather than its flat albedo.
        //
        // The material prelude has had the right mechanism the whole time -- s.display and
        // s.displayColor, gated on gShadingModel == AVER_MODEL_UNLIT, which PSMainVoxi already
        // checks through averDisplayColour. Nothing ever WROTE that model; writeShadingConstants
        // hardcoded STANDARD on both backends. It is written now, so the feature's own shader
        // resolves unlit against the sampled material and this diversion is no longer needed.
        //
        // Wireframe is NOT the same case and keeps its own route: it needs a different rasteriser
        // state, not a different shading branch, and VoxiRenderer::scenePipeline declines it one
        // level down by returning 0.
        // `blended` explicit and false: the OPAQUE scene walk. A translucent mesh never reaches here
        // -- setDrawBlended(true) diverts it into the capture-and-replay path above -- so writing
        // false out loud says so, rather than leaning on the parameter's default.
        // ---- A PREPASSED DRAW GOES DOWN THE SAME GEOMETRY PATH ITS DEPTH WAS WRITTEN THROUGH ----
        //
        // drawMeshDepthPrepass above ALWAYS draws through the input assembler and vsMain -- it has
        // no mesh-shader twin -- and the whole premise of the LessEqual/no-write colour pipelines
        // is that the colour pass reproduces that depth bit for bit. VoxiRenderer::scenePipeline
        // says so outright: "the prepass is only offered to the plain drawMesh() path, so
        // `depthPrepassed && meshShaders` should never both be true". This call site never honoured
        // it. It passed msActive_ unconditionally, so with the device on mesh shaders
        // (RENDER.MESHSHADERS 1, which PTTest sets) every prepassed draw asked for the ORDINARY
        // mesh-shader pipeline -- Less, depth write on -- and then tested against the depth the
        // prepass had just written for the identical triangle. Less rejects equal. Every eligible
        // fragment was discarded, and with [earlydepthstencil] on PSMainVoxi it happened before the
        // pixel shader ran, which is why the broken frame was also 47x cheaper:
        //
        //   raster scene draws  47.51ms -> 1.02ms   and the image 0.1% bit-identical, MAD 19.4
        //
        // Two earlier diagnoses missed it for a reason worth keeping. --no-lod-mesh-shader turns off
        // TRIFACTOR's LOD mesh shaders, not this device path (the log still read "geometry path:
        // mesh shaders"), and forcing scenePipeline's prepassed branch off changed nothing because
        // that branch was never being reached. Both "eliminations" tested a switch that was already
        // in the state the test assumed.
        //
        // Routing the prepassed draw onto the input assembler restores the contract exactly: same
        // compiled vsMain in both passes, so identical positions, identical rasteriser snapping,
        // identical depth -- and LessEqual then keeps precisely the visible surface. MSMain computes
        // the position with the same two multiplies (shared_prelude.hlsl), so this is not expected
        // to move a pixel against the mesh-shader frame either, but the depth equality this pass
        // RELIES on no longer depends on two different shader stages happening to round alike.
        // Only prepassed draws move; everything else keeps the mesh-shader path it had.
        const bool featureMs = msActive_ && msPso_ && !prepassed;
        const PipelineHandle fp = f->scenePipeline(featureMs, wireframe_, prepassed, false);
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
        fc[20] = metallic; fc[21] = roughness; fc[22] = unlit_ ? 1.0f : 0.0f; fc[23] = 0.0f;
        writeShadingConstants(fc, unlit_);
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        // featureMs, not msActive_: the draw call has to match the pipeline chosen above, and a
        // prepassed draw was just given an input-assembler pipeline. See featureMs's own comment.
        if (featureMs && !wireframe_) rhiContext_->dispatchMeshFor(mesh);
        else                          rhiContext_->drawMesh(mesh);
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
    // GUARDED THE SAME WAY bindGraphicsRoot already guards the root signature, above. A scene is
    // typically hundreds to thousands of drawMesh calls sharing one PSO, and this used to re-issue
    // SetPipelineState on every one regardless -- a command-list entry paid for nothing. boundPso_ is
    // invalidated everywhere boundRootSig_ already is, since anything that can change the root
    // signature can just as well change which PSO is bound.
    ID3D12PipelineState* wantPso = useMs ? msPso_.Get() : (wireframe_ ? wirePso_.Get() : pso_.Get());
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
    // DEFERRED, not immediate: the GPU may still be reading this buffer for a frame in flight, and
    // releasing an UPLOAD-heap resource under a live command list is a use-after-free the debug layer
    // reports somewhere else entirely, if at all. The resource factory's fence-keyed retire list
    // (D3D12ResourceFactory::retire) already exists for this -- destroyMesh reaches it through
    // destroyBuffer, so a line buffer joins the same list rather than growing a second mechanism.
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
    // Dword 16 is gBaseColor.x, unread by PSLine otherwise -- so the glow multiplier rides in the
    // per-object block a line draw already binds, no root-signature change.
    //
    // WRITTEN EVERY CALL, not only when it differs from 1.0: root constants persist across draws, so
    // skipping the write would hand the grid/navmesh overlay/sculpt ring the red channel of whatever
    // material was drawn last as their brightness. A line's glow must not depend on what preceded it.
    cmdList_->SetGraphicsRoot32BitConstants(kSceneObjectParam, 1, &lineGlow_, 16);
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
    if (s.sunTemperatureK > 0.0f) {
        blackbodySrgb(s.sunTemperatureK, sun);
        // blackbodySrgb hands back LINEAR sRGB, but lightColor is DISPLAY-ENCODED -- s.sunColor
        // above is authored that way, and every reader (packAtmosphere's e0 a few lines below,
        // the shared HLSL prelude's srgbToLin(gLightColor)) decodes it with pow(x, 2.2). Without
        // this re-encode a kelvin-driven sun was decoded TWICE: once inside blackbodySrgb's own
        // XYZ->linear-sRGB matrix, and again by every one of those readers.
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
    // THE SEED IS AN OFFSET IN THE NOISE DOMAIN, needing no shader change: the cloud density function
    // already samples at (wpos + cloudMotion.xy) * scale, and translating a noise field far enough is
    // indistinguishable from a different one. Reusing the wind offset costs no constant (the buffer
    // is full) and keeps the seed on the axis the noise already varies on.
    //
    // Seed 0 adds nothing, so an unseeded sky is bit-identical to before this existed. Offsets are
    // large and irrational-ish so nearby seeds don't land in neighbouring cells of the same feature.
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
    // that function's comment in RHIShaders.cpp for why this is exact, not an approximation. Reads
    // frameCB_.camPos and frameCB_.atmoPlanet, both already written for THIS frame: setCamera runs
    // before setSkyAtmosphere in every caller, and atmoPlanet[2] was set a few lines up.
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
    // THE SKY IS ALREADY ON THE SUN'S SCALE -- 1, NOT THE 8 THIS USED TO BE.
    //
    // The atmosphere's source term is sigma * phase * sunTransmittance * E0 (Atmosphere.cpp), so these
    // coefficients are radiance in E0's own units and need no conversion. Integrated over the upper
    // hemisphere they give a clear-sky diffuse share of ~16% on a horizontal surface at 45 degrees of
    // sun elevation -- inside AtmosphereTest's 15-30% gate for the dome, and where a real clear sky
    // sits (10-20% of the sun's horizontal irradiance).
    //
    // THE 8 CAME FROM A DISPLAY-SPACE MEASUREMENT: luminance percentiles of a tonemapped frame (p50
    // 119 -> 125 with sky light 0 -> 1), with the p50 change divided by the MAX pixel, which sits on
    // the tonemap's shoulder. The same post-tonemap-metric trap that once "measured" NRD losing 40-80%
    // of GI. It made the diffuse sky ~1.5x the sun's own horizontal irradiance -- outdoor shadows at
    // ~40% of sunlit and blue, where physics says ~10-20% -- while the visible dome and reflections
    // stayed at 1x, so the sky lit the scene 8x brighter than it looked. Removed on the owner's call
    // (2026-09-24). Judge any future sky-scale change in linear HDR (--tonemap 0), never on 8-bit.
    //
    // Kept as a named constant so skyLightIntensity (an authored multiplier, written back on save)
    // is never the place a calibration gets folded in: a scale applied on load would compound on
    // every round trip. The Vulkan twin carries the same value.
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
    // Local exposure's bilateral grid. Built unconditionally, same as every PSO above -- runPostChain
    // is what decides per-frame whether either pass actually dispatches.
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
    // Local exposure's bilateral grid: scene-size dependent, so it is dropped and rebuilt here every
    // resize exactly like the two targets above -- histBuf_/expBuf_ are NOT touched here because
    // their size (256 bins; one exposure value) never depends on scene resolution.
    localGridBuf_.Reset();
    localGridBlurBuf_.Reset();
    localGridW_ = localGridH_ = 0;
    // FACTORY HANDLES, SO destroyTexture -- NOT .Reset(). These are the only targets here created
    // through the resource factory; treating a handle like the ComPtrs above would leak the
    // factory's row and its descriptors every resize.
    if (D3D12ResourceFactory* f = rhiFactory_) {
        if (sceneColorTex_) { f->destroyTexture(sceneColorTex_); sceneColorTex_ = 0; }
        if (presentHdrTex_) { f->destroyTexture(presentHdrTex_); presentHdrTex_ = 0; }
        if (blendBackdropTex_) { f->destroyTexture(blendBackdropTex_); blendBackdropTex_ = 0; }
    }
    sceneColorTexW_ = sceneColorTexH_ = presentHdrTexW_ = presentHdrTexH_ = 0;
    blendBackdropW_ = blendBackdropH_ = 0;
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

    // AverSR's two intermediates, and the composite triple that reads the upscaled one. GUARDED ON
    // upscaler_, ALL OF IT: creating these unconditionally costs every Off build two HDR textures it
    // never samples, and writing the triple unconditionally would hand rhiFactory_->texture(0) to a
    // writeTriple that dereferences it, crashing createPostTargets on the DEFAULT path (an
    // adversarial review caught this exact null-deref before it was written).
    //
    // THE BLENDED PASS'S BACKDROP, created whether or not anything upscales -- glass needs it always.
    // Same shape as the AverSR alias below (SRV only, a CopyResource fills it): `scene` is a raw
    // ComPtr with no TextureHandle, and a binding set needs one. See
    // IDevice::sceneColorBackdropTexture for what it's for.
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

    // ---- local exposure's bilateral grid: u2 raw, u3 blurred ----
    // gridW/gridH mirror EXACTLY what post.hlsl derives from GetDimensions() of the same scene
    // texture (t0) -- ceil(scene / kLocalExpTile) -- so the buffer's allocated size and the shader's
    // own byte-offset arithmetic never disagree.
    localGridW_ = (sceneWidth_ + kLocalExpTile - 1) / kLocalExpTile;
    localGridH_ = (sceneHeight_ + kLocalExpTile - 1) / kLocalExpTile;
    const u64 gridBytes = static_cast<u64>(localGridW_) * localGridH_ * kLocalExpBins * kLocalExpCellBytes;
    auto makeGridBuf = [&](ComPtr<ID3D12Resource>& out, const char* name) {
        auto gd = bufferDesc(gridBytes);
        gd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        // Created COMMON, same reason as histBuf_/expBuf_ above (#1328) -- then transitioned once,
        // just below. Unlike those two this needs no zero-seed: CSLocalGrid overwrites all
        // kLocalExpBins of every tile it touches every frame it runs, so stale bytes from a previous
        // resize's allocation (or the driver's own zero-fill) are never read before being written.
        return hrOk(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &gd,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)), name);
    };
    if (!makeGridBuf(localGridBuf_, "local exposure grid") ||
        !makeGridBuf(localGridBlurBuf_, "local exposure grid blur")) {
        localGridBuf_.Reset();
        localGridBlurBuf_.Reset();
        AVER_WARN("[RHI.D3D12] local exposure's bilateral grid could not be allocated; local exposure stays off until the next resize");
    } else {
        // cmdList_ IS recording here: createPostTargets is only ever called from within runPostChain
        // (see its "!postReady_" call site), mid-frame, below other barriers already issued on it.
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
    // The post chain drives the command list directly, below the context that owns table 1's cache,
    // so nothing HERE would tell that cache its root arguments are gone.
    //
    // REDUNDANT TODAY, AND KEPT ANYWAY -- said plainly because the first version of this comment
    // claimed the clear was needed, which was wrong. endFrame force-clears both caches at its own
    // top, before it calls this function, and nothing between there and here can set dbValid_ true
    // again; that was traced by hand rather than assumed. So this is belt-and-braces at a site that
    // genuinely does discard root arguments, not a fix for a live gap. It stays because the cost is
    // one store per frame and the alternative is a correctness argument that depends on the call
    // order of a function three levels up. fovValid_ is deliberately NOT given a matching clear here
    // for the same reason -- see bindGraphicsRoot, which is where the one REAL gap was.
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
        // FILLED ON BOTH BACKENDS IN THE SAME CHANGE. VulkanDevice.cpp has the twin of this lambda and
        // its own comment says it uses "the same formulas, same PostCB fields as D3D12Device.cpp's" --
        // a field added here and not there is a silently different image on the other backend.
        cb.misc[3] = static_cast<f32>(post_.tonemap);
        cb.clampRadiance[0] = post_.maxRadiance;
        // y/z: local exposure's shadow/highlight strengths -- gPostClamp.y/z in post.hlsl's
        // PSComposite. FILLED HERE, UNCONDITIONALLY, same reasoning as misc/adapt above: every pass
        // shares this one fillCommon, so the composite and the two new compute passes all see the
        // same values without a separate path. w stays spare.
        cb.clampRadiance[1] = post_.localExposureShadows;
        cb.clampRadiance[2] = post_.localExposureHighlights;
        cb.clampRadiance[3] = 0.0f;

        // gPostRegion: the docked editor's viewport sub-rect, in the post chain's own normalised
        // SOURCE space -- gPostSceneTex/gPostBloomTex/the local-exposure grid, all of which are sized
        // off sceneWidth_/sceneHeight_ (or, on the AverSR path, presentHdrTex_ -- see the composite's
        // own comment for why the same normalised rect still applies there).
        //
        // vpX_/vpY_/vpW_/vpH_ ARE ALREADY SCENE-SPACE, NOT PRESENT-SPACE -- setViewportRect converts
        // the caller's present-space pixels via scaleToSceneW/H before storing them (see that
        // function's own comment), so normalising by sceneWidth_/sceneHeight_ here, not width_/
        // height_, is what turns them into gPostSceneTex's own uv. Dividing by width_/height_ instead
        // would undershoot whenever renderScale_ < 1 (sceneWidth_ < width_), reading the wrong,
        // smaller fraction of the scene texture than the docked panel actually covers.
        //
        // (0,0,1,1) identity whenever no sub-rect is set (undocked editor / game runtime, vpW_==0),
        // so every pass on that path samples exactly what it did before this field existed. The
        // sceneWidth_/sceneHeight_ > 0 guards are belt-and-braces against a division by zero: vpW_/
        // vpH_ each floor to 1 in setViewportRect even if scaleToSceneW/H returned 0, so a 0-sized
        // scene target (window not yet sized) could otherwise pair a nonzero vpW_ with a zero divisor.
        if (vpW_ > 0 && vpH_ > 0 && sceneWidth_ > 0 && sceneHeight_ > 0) {
            cb.region[0] = std::fmin(std::fmax(static_cast<f32>(vpX_) / static_cast<f32>(sceneWidth_),  0.0f), 1.0f);
            cb.region[1] = std::fmin(std::fmax(static_cast<f32>(vpY_) / static_cast<f32>(sceneHeight_), 0.0f), 1.0f);
            cb.region[2] = std::fmin(std::fmax(static_cast<f32>(vpW_) / static_cast<f32>(sceneWidth_),  0.0f), 1.0f);
            cb.region[3] = std::fmin(std::fmax(static_cast<f32>(vpH_) / static_cast<f32>(sceneHeight_), 0.0f), 1.0f);
        } else {
            cb.region[0] = 0.0f; cb.region[1] = 0.0f; cb.region[2] = 1.0f; cb.region[3] = 1.0f;
        }
    };

    // vx/vy: the destination's top-left offset, default 0 for every pass that always fills its whole
    // target (bloom's pyramid, the histogram). Only the composite passes it non-zero, to confine the
    // draw to the docked viewport's sub-rect -- see the composite call sites below.
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
    // hw/hh (the downscaled dispatch grid) and the src dims below both derive from the SCENE target
    // this samples -- sceneWidth_/sceneHeight_, not the present width_/height_.
    //
    // DOCKED: metered over the sub-rect's OWN scene-pixel extent (vpW_/vpH_, already scene-space --
    // see fillCommon's gPostRegion comment), not the whole scene target, so auto-exposure stops
    // averaging in the black dead zone outside the 3D viewport panel that CSHistogram's uv mapping
    // (gPostRegion, below) now confines the actual sampling to. Full scene extent, unchanged, when
    // undocked (vpW_/vpH_ == 0).
    if (autoExp) {
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
        dbValid_ = false;   // same reason as the chain's opening bind -- see bindGraphicsRoot
    }

    // ---- local exposure ----
    // INDEPENDENT OF autoExp: both strengths can be nonzero with auto-exposure off (a fixed
    // post_.exposure still wants regions pulled toward middle grey), so this is gated on its own
    // condition and always runs before the composite, not only when the block above ran. Skipped
    // entirely -- no dispatch, no barrier, same bindings otherwise -- when both strengths are 0,
    // which is what keeps the chain byte-for-byte identical to before this feature existed.
    const bool localExp = (post_.localExposureShadows > 0.0f || post_.localExposureHighlights > 0.0f) &&
                          caps_.computeShaders && localGridBuf_ && localGridBlurBuf_;
    if (localExp) {
        // dst == src == the scene itself: this pass has no separate destination texture (it writes
        // the grid buffer, not a render target), so both fillCommon args are the scene size post.hlsl
        // would also reach via GetDimensions() on t0.
        fillCommon(sceneWidth_, sceneHeight_, sceneWidth_, sceneHeight_);

        cmdList_->SetComputeRootSignature(postRootSig_.Get());
        cmdList_->SetPipelineState(localGridPso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        // kPostTripleHistogram: t0/t1/t2 all just `scene`, same triple CSHistogram reads above -- all
        // this pass needs is t0.
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch(localGridW_, localGridH_, 1);

        // Barrier #1: CSLocalGrid's write of u2 must land before CSLocalBlur reads it.
        D3D12_RESOURCE_BARRIER gridUav{};
        gridUav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        gridUav.UAV.pResource = localGridBuf_.Get();
        cmdList_->ResourceBarrier(1, &gridUav);

        cmdList_->SetPipelineState(localBlurPso_.Get());
        cmdList_->SetComputeRootConstantBufferView(0, postConstants(&cb, sizeof cb));
        cmdList_->SetComputeRootDescriptorTable(1, postTriple(kPostTripleHistogram));
        cmdList_->SetComputeRootDescriptorTable(2, uavTable);
        cmdList_->Dispatch((localGridW_ + 7) / 8, (localGridH_ + 7) / 8, 1);

        // Barrier #2: CSLocalBlur's write of u3 must land before PSComposite's pixel-shader UAV read
        // of it, below. Both buffers stay in UNORDERED_ACCESS throughout -- a UAV barrier, not a
        // state transition, exactly like histBuf_ between CSHistogram and CSExposure above.
        D3D12_RESOURCE_BARRIER blurUav{};
        blurUav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blurUav.UAV.pResource = localGridBlurBuf_.Get();
        cmdList_->ResourceBarrier(1, &blurUav);

        cmdList_->SetGraphicsRootSignature(postRootSig_.Get());
        dbValid_ = false;   // same reason as the eye-adaptation block above -- see bindGraphicsRoot
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

    // AverSR: Pass A, the upscale (HDR, before the tonemap).
    //
    // ONLY WHEN AN UPSCALER IS SET: with none (default, quality Off) not a line of this runs and the
    // composite below takes exactly the path it always did -- the whole of docs/AVERSR.md's "Off
    // must be bit-identical" invariant, one branch on one pointer.
    //
    // BEFORE THE TONEMAP, deliberately: the composite fuses resize + exposure + bloom + ACES + gamma
    // into one pass, so an upscaler can't simply replace it. Running the resample on scene RADIANCE
    // and handing the composite an already present-sized image leaves that shader and pipeline
    // untouched -- its resample degenerates to 1:1.
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
            // The upscaler bound its OWN descriptor heap and root signature (see the comment above),
            // so this is the one site where the root arguments were discarded by code outside this
            // file entirely. Exactly why the cache is cleared at the binds rather than at a list of
            // callers somebody has to keep complete.
            dbValid_ = false;
            srUpscaled = true;

            // ONCE, reporting what actually happened, not what was configured: this feature spent
            // its whole life "constructed and correct and never called", and logging on the SETTING
            // would have said it was working the entire time.
            if (!srLogged_) {
                srLogged_ = true;
                AVER_INFO("[AverSR] '{}' upscaling {}x{} -> {}x{} in HDR, before the tonemap",
                          upscaler_->name(), sceneWidth_, sceneHeight_, width_, height_);
            }
        }
    }

    // ---- composite ----
    // dst is present-space (backbuffer or viewport texture, both width_/height_); src is the scene
    // target this upscales (or 1:1 samples at renderScale_ == 1.0) from -- sceneWidth_/sceneHeight_.
    // This IS the render-scale upscale: the composite shader already samples by normalized UV
    // through a bilinear sampler (gPostSamp), so telling it a different source size is all it takes.
    //
    // ON THE AverSR PATH t0 is already present-sized and gPostSamp's stretch does nothing -- AverSR's
    // filter produced those pixels. src STAYS THE SCENE SIZE there all the same: PSComposite never
    // reads src for its own sampling (it samples by UV), and local exposure reads it as the size its
    // grid was built at, which is always the scene's.
    fillCommon(width_, height_, sceneWidth_, sceneHeight_);
    const u32 compositeTriple = srUpscaled ? kPostTripleCompositeUpscaled : kPostTripleComposite;

    // THE COMPOSITE'S OWN VIEWPORT/SCISSOR, confined to the docked sub-rect gPostRegion (just written
    // above by fillCommon) already describes in normalised source space -- scaled back up into
    // PRESENT-space pixels here since the destination (viewport texture or backbuffer) is present-
    // sized, unlike gPostRegion's scene-space source. Full canvas (0,0,width_,height_) whenever
    // region is the (0,0,1,1) identity -- vx==0, vy==0, compositeW==width_, compositeH==height_ --
    // so this is byte-identical to before this feature existed on every undocked path.
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
        bbRtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;

        const RhiTexture* vt = (viewportToTex_ && ensureViewportTexture() && rhiFactory_)
                             ? rhiFactory_->texture(viewportTex_) : nullptr;
        if (vt && vt->rtvHeap) {
            const f32 blank[4] = {clear_[0], clear_[1], clear_[2], 1.0f};
            cmdList_->ClearRenderTargetView(bbRtv, blank, 0, nullptr);

            auto toRtTex = transition(vt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRtTex);
            // OUTSIDE the sub-rect, vt keeps whatever it held before this draw -- no clear precedes
            // it, and the confined viewport/scissor below means DrawInstanced's fullscreen triangle
            // never rasterizes there. That is fine, not merely tolerated: the ONLY reader of vt is
            // SandboxShell's "Level" ImGui::Image, and its uv0/uv1 crop to this exact same present-
            // space sub-rect (at.x/DisplaySize.x etc.), so nothing ever samples the untouched area --
            // it is stale content, never garbage, and never displayed either way.
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
    dbValid_ = false;    // same reason, same moment -- see dbValid_'s member comment
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

    // Reordered from a shape whose comment reasoned that sky-first meant "a particle would blend onto
    // sky that can never be un-drawn" -- false: sky draws OPAQUE with DepthFunc EQUAL against the
    // clear depth, so with transparency running FIRST and depth-write off, a particle over open air
    // sat at clear depth (1.0), and the sky's EQUAL test overwrote its blended colour with flat sky --
    // smoke, snow, mist invisible against open sky. Found via --particle-test: an emitter reprojected
    // correctly into NDC yet rendered nothing with no opaque occluder behind it; every earlier
    // screenshot happened to place particles in front of a cube.
    //
    // This order fixes it without reopening the old problem: the sky still only fills pixels still at
    // clear depth (unchanged EQUAL test, nothing else touches depth before it), so it fills exactly
    // the same pixels regardless of order. The transparent pass runs AFTER it: an occluded pixel still
    // fails the particle's depth test exactly as before (occlusion untouched), while an unoccluded
    // pixel now holds the sky's real colour to blend onto -- only what colour it blends onto changes.
    //
    // Gated on frameSuppressed_, not sceneSuppressed_: ray-driven primary visibility suppresses the
    // scene without owning the frame, and writes depth 1.0 on a miss -- exactly what DepthFunc=EQUAL
    // looks for -- so the sky still fills missed pixels. Testing the wrong flag left that mode with
    // no clouds, atmosphere or sun.
    if (skyEnabled_ && !frameSuppressed_) {
        // Nested spans, reversing an earlier decision: "sky+post+ui" measured 8.2ms, 46% of the frame
        // and the largest span in it, conflating a fullscreen atmosphere march with an editor's UI
        // compositing -- at 2750x1639 with a docked editor the UI may BE most of it. Four children
        // plus the parent's exclusive time make that decidable instead of guessed.
        beginGpuSpan("sky dome");
        cmdList_->RSSetViewports(1, &sceneVp);
        cmdList_->RSSetScissorRects(1, &sceneSc);
        bindGraphicsRoot(rootSig_.Get());
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList_->SetPipelineState(skyPso_.Get());
        boundPso_ = skyPso_.Get();
        cmdList_->IASetVertexBuffers(0, 0, nullptr);
        cmdList_->DrawInstanced(3, 1, 0, 0);
        endGpuSpan();   // "sky dome"
    }

    // THE BLENDED-MESH FLUSH: every drawMesh() call this frame that ran with drawBlended_ true was
    // captured instead of drawn -- see IDevice::setDrawBlended (RHI.hpp) for why. This replay has to
    // land in EXACTLY this gap: after the sky draw above (whose depth-EQUAL opaque fill would
    // otherwise overwrite a blended mesh drawn earlier, the same failure the particle pass was moved
    // to fix), and before IRenderFeature::transparentPass below, which particles already expect to be
    // the last thing touching the render target before the post chain. A caller sorting its own draw
    // list could never get this ordering right -- the sky isn't in that list -- which is why the
    // capture exists rather than a sorted replay at the call site.
    //
    // Guarded on frameSuppressed_, not sceneSuppressed_: a feature owning only the raster surface
    // still wants its sky/particles/glass; captured draws a whole-frame owner never saw just sit
    // unflushed until the next beginFrame clears them.
    //
    // No new GPU span or per-frame counter: this and the transparent pass below both run inside
    // "sky+post+ui", and a missing blended pipeline already logs by name
    // (blendedPipelineMissingWarned_).
    if (!blendedDraws_.empty() && rhiContext_ && !frameSuppressed_) {
        // MEASURED: even after splitting the sky dome, post chain, overlay and ImGui out of
        // "sky+post+ui", that span still reported 4.7ms exclusive -- more than those four combined,
        // and this replay was the only unmarked thing left. The glass was the cost, not "sky and post".

        // The backdrop copy runs only when there are blended draws, before the first translucent
        // draw so it captures the opaque scene exactly. Copies FROM msaaColor_, not `scene`: `scene`
        // is the resolve destination and the resolve hasn't run yet.
        //
        // The size check is load-bearing -- omitting it removed the device on every resize.
        // CopyResource/ResolveSubresource require matching dimensions, and a window resize,
        // render-scale change or AverSR rung change can briefly mismatch the backdrop (sized from
        // sceneWidth_/sceneHeight_ in createPostTargets) against the scene colour target (recreated
        // by the swapchain path). Skip cleanly instead of asserting: the shader's GetDimensions guard
        // falls back to a scalar composite for one frame, beating a removed device.

        // A lambda because one capture per frame isn't enough for stacked translucency:
        // averBlendedOutputBackdrop subtracts a sample of this texture to cancel the hardware's
        // dst*(1-alpha) blend, which only holds for the FIRST translucent surface over a pixel -- a
        // second surface's backdrop doesn't yet contain the first, so the residue can go negative (a
        // black artefact under the tonemap clamp). MEASURED on PTTest's glass walkway over the pool:
        // it read pale and opaque, hiding the water, and the sun grew a black crescent where the
        // water alone gave a clean disc. Re-resolving between layers (furthest-first sort below)
        // keeps dst and this texture in agreement.
        auto resolveBlendBackdrop = [&]() {
            if (blendBackdropTex_ && rhiFactory_ && msaaColor_ &&
                blendBackdropW_ == sceneWidth_ && blendBackdropH_ == sceneHeight_) {
                if (RhiTexture* bt = rhiFactory_->texture(blendBackdropTex_)) {
                    if (bt->res) {
                        // MSAA IS 4x BY DEFAULT, so a single-sample-only version of this never ran
                        // for anybody. A multisampled target can't CopyResource into single-sample --
                        // it must be RESOLVED, which is what the backdrop wants anyway since the
                        // shader samples it once per pixel.
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

        // The first capture, in the position and for the reason the one-shot version had: after the
        // opaque scene and the sky, before any translucent draw.
        resolveBlendBackdrop();
        u32 blendLayerResolves = 0;
        // M3 counters -- see blendStatDrawsLogged_'s own comment for why these are logged on change
        // rather than every frame. `blendDrawsDone` counts draws that survive the stale-handle check
        // below, not blendedDraws_.size(): a capture can outlive its mesh within the same frame, and
        // the log line should say what actually reached the GPU, not what was merely queued.
        u32 blendDrawsDone = 0;
        bool blendCapReached = false;

        beginGpuSpan("blended replay");
        // Only the FIRST feature that overridesScenePipeline() is asked, the same assumption the
        // opaque walk and drawMeshDepthPrepass make (never two scene-pipeline-overriding features at
        // once). `blended`=true and `depthPrepassed`=false for every draw here, neither varying per
        // instance, so the pipeline lookup happens ONCE for the whole flush rather than once per draw
        // -- if nothing offers a blended variant, every capture drops for the identical reason.
        IRenderFeature* owner = nullptr;
        for (IRenderFeature* f : features_) {
            if (f->overridesScenePipeline()) { owner = f; break; }
        }
        const PipelineHandle blendedPso = owner
            ? owner->scenePipeline(msActive_ && msPso_, wireframe_, false, true) : 0;

        if (!blendedPso) {
            // Returning 0 for blended=true is the documented "no blended variant" answer
            // (IRenderFeature::scenePipeline's comment), same as finding no overriding feature at all
            // (the backend's own fallback pipeline has no blended permutation either). DROPPING
            // rather than falling back to opaque is deliberate: a pane of glass rendered opaque reads
            // as a solid wall, worse than one simply not there. See blendedPipelineMissingWarned_'s
            // comment for why this is once PER FRAME, unlike drawBindingIgnored_'s once-ever above.
            if (!blendedPipelineMissingWarned_) {
                AVER_WARN("[RHI.D3D12] {} blended draw(s) this frame but no feature offers "
                          "scenePipeline(..., blended=true); dropping them rather than drawing them "
                          "opaque (said once per frame)", blendedDraws_.size());
                blendedPipelineMissingWarned_ = true;
            }
        } else {
            // Back-to-front, furthest first: with depth-write off, a fragment blended earlier sits
            // UNDER one blended later at the same pixel, so the furthest surface draws first.
            // Distance is camera to world TRANSLATION ONLY -- elements 12,13,14 of a row-major,
            // row-vector world matrix -- an object-centre approximation, wrong only for large,
            // mutually-interpenetrating meshes this path isn't meant for; the same coarse
            // approximation nearly every real-time transparency sort makes.
            const f32 cx = frameCB_.camPos[0], cy = frameCB_.camPos[1], cz = frameCB_.camPos[2];
            std::sort(blendedDraws_.begin(), blendedDraws_.end(),
                      [cx, cy, cz](const BlendedDraw& a, const BlendedDraw& b) {
                const f32 adx = a.world[12] - cx, ady = a.world[13] - cy, adz = a.world[14] - cz;
                const f32 bdx = b.world[12] - cx, bdy = b.world[13] - cy, bdz = b.world[14] - cz;
                // Squared distance: monotonic with the real distance, so it sorts identically and
                // the sqrt every real distance would need is pure cost paid for nothing.
                return (adx * adx + ady * ady + adz * adz) > (bdx * bdx + bdy * bdy + bdz * bdz);
            });

            cmdList_->RSSetViewports(1, &sceneVp);
            cmdList_->RSSetScissorRects(1, &sceneSc);
            bindGraphicsRoot(rootSig_.Get());

            // The fov* pipeline-elision cache (see fovValid_'s comment) is reused honestly here, not
            // force-invalidated: it's already false entering this block, so the first blended draw
            // always re-binds for real, and from then on the same "did anything change" comparison
            // the opaque walk uses applies here -- what makes a sorted run of many panes sharing one
            // feature and material cheap. It IS force-invalidated after the loop: runPostChain right
            // after sets pipelines directly on the command list, and a stale "true" would describe
            // state about to be overwritten.
            bool blendDrewAny = false;
            for (const BlendedDraw& bd : blendedDraws_) {
                // A capture can outlive its mesh within the SAME frame (destroyMesh() called between
                // capture and flush) -- the same "stale handle draws nothing" rule drawMesh() applies
                // to a live call, applied here to a deferred one.
                if (bd.mesh == 0 || bd.mesh > meshes_.size() || !meshes_[bd.mesh - 1].alive) continue;
                ++blendDrawsDone;

                // Re-capture so this surface sees the ones behind it: everything drawn so far this
                // flush is further from the camera, exactly what this draw's correction needs to
                // cancel against. Skipped for the first surviving draw, whose capture already ran.
                //
                // Capped: each re-capture is a full-target MSAA resolve, so a hundred panes would pay
                // a hundred of them. Beyond the cap, later surfaces fall back to the pre-fix
                // approximation; logged once so an over-cap scene says so.
                if (blendDrewAny) {
                    if (blendLayerResolves < kMaxBlendLayerResolves) {
                        ++blendLayerResolves;
                        // NO OMSetRenderTargets AFTERWARDS -- not an omission. The resolve moves
                        // msaaColor_ to RESOLVE_SOURCE and back; a resource BARRIER changes state, it
                        // doesn't unbind, so the targets set before this loop are still bound.
                        resolveBlendBackdrop();
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
                blendDrewAny = true;

                const BindingSetHandle bs = owner->sceneBindingSet();
                const void* cb = nullptr; u32 cbBytes = 0;
                const bool haveCb = owner->sceneConstants(&cb, &cbBytes) && cb && cbBytes;
                const bool same = fovValid_ && blendedPso == fovPso_ && bs == fovSet_ &&
                                  haveCb == (fovCbBytes_ != 0) &&
                                  (!haveCb || (cbBytes == fovCbBytes_ && fovCb_.size() == cbBytes &&
                                               std::memcmp(fovCb_.data(), cb, cbBytes) == 0));
                if (!same) {
                    rhiContext_->setPipeline(blendedPso);
                    // The owner's bindless texture table, if its blended pipeline declared one --
                    // a no-op otherwise, unconditional rather than a second capability question. See
                    // IRenderFeature::sceneBindlessTable for why the device asks instead of the
                    // feature binding it: the feature isn't on the stack during a replay it doesn't drive.
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
                // fc[22] STAYS 0 HERE, unlike the two live drawMesh paths: honouring setUnlit would
                // mean capturing it into BlendedDraw beside bd.metallic/bd.roughness. Nothing wants
                // an unlit blended mesh today, and plumbing it speculatively risks two flags that
                // disagree.
                fc[20] = bd.metallic; fc[21] = bd.roughness; fc[22] = 0.0f; fc[23] = 0.0f;
                writeShadingConstants(fc);
                rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
                if (msActive_ && msPso_ && !wireframe_) rhiContext_->dispatchMeshFor(bd.mesh);
                else                                    rhiContext_->drawMesh(bd.mesh);
                boundRootSig_ = nullptr;
                boundPso_ = nullptr;
            }
            fovValid_ = false;   // see the block comment above for why this is hygiene, not a fix
            dbValid_ = false;    // table 1's cache, same hygiene, same reason: runPostChain next sets
                                  // pipelines on the command list directly, outside setPipeline
        }

        // M3: logged on CHANGE, not every frame -- see blendStatDrawsLogged_'s own comment. Reached
        // for BOTH branches above: the !blendedPso branch left blendDrawsDone/blendLayerResolves/
        // blendCapReached at their initial 0/0/false, which is the honest answer ("nothing replayed")
        // and still worth a log line the first time a frame queues blended draws no feature can take.
        if (blendDrawsDone != blendStatDrawsLogged_ || blendLayerResolves != blendStatResolvesLogged_) {
            ++blendStatChanges_;
            if ((blendStatChanges_ & (blendStatChanges_ - 1)) == 0) {
                AVER_INFO("[RHI.D3D12] blended replay: {} translucent draw(s), {} layer re-capture(s) (cap {}){}",
                          blendDrawsDone, blendLayerResolves, kMaxBlendLayerResolves,
                          blendCapReached ? ", cap reached" : "");
            }
            blendStatDrawsLogged_ = blendDrawsDone;
            blendStatResolvesLogged_ = blendLayerResolves;
        }
        endGpuSpan();   // "blended replay"
    }

    // IRenderFeature::transparentPass, called here because every opaque drawMesh and the deferred sky
    // above have already run: the depth buffer holds every real occluder and the target holds real
    // colour everywhere, so a particle here always blends onto something real (see the sky draw's
    // comment for the failure this reorder fixes). A feature that doesn't override the default empty
    // implementation leaves the frame bit-for-bit unchanged.
    //
    // Depth-test on, depth-write off is the pipeline contract every feature here must build: write
    // must stay off, since with back-to-front sorting a fragment that wrote depth would make every
    // fragment meant to blend under it fail the depth test -- a smoke plume would render as one
    // opaque slice at its nearest layer. Test stays on so a particle behind a wall is still hidden.
    //
    // Viewport/scissor are preset to the scene rect; the pipeline is not. Root signature and viewport
    // are re-set explicitly rather than trusted from the sky draw: cheap, idempotent, correct
    // regardless of what ran in between. Gated on frameSuppressed_ like the sky: a particle in a
    // ray-driven frame is as real as a rastered one, and the ray pass writes real SV_DEPTH.
    if (rhiContext_ && !frameSuppressed_) {
        cmdList_->RSSetViewports(1, &sceneVp);
        cmdList_->RSSetScissorRects(1, &sceneSc);
        bindGraphicsRoot(rootSig_.Get());
        for (IRenderFeature* f : features_) f->transparentPass(*rhiContext_);
    }

    beginGpuSpan("post chain");
    runPostChain(bb);
    endGpuSpan();   // "post chain"

    // ---- overlay features, on the tonemapped backbuffer ----
    beginGpuSpan("overlay");
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

    // The installed UI backend's own draw, after every overlay feature and before capture. uiActive_
    // is false with no UI backend installed (a game, or AVER_ENABLE_UI=OFF), so this no-ops there,
    // same as the old #if AVER_WITH_IMGUI block, decided at runtime instead of compile time.
    endGpuSpan();   // "overlay" -- closed here so the span covers every overlay feature and nothing
                    // else; ImGui gets its own below.
    if (uiActive_ && uiBackend_) {
        // THE ONE THAT DECIDES WHETHER ANY OF THIS IS A RENDERING COST. If "editor UI" dominates,
        // the 8.2ms parent is mostly a docked ImGui at 2750x1639 and a game build never pays it.
        beginGpuSpan("editor UI");
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        boundHeap_ = uiBackend_->render(cmdList_.Get());
        endGpuSpan();   // "editor UI"
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

    // NOTHING IS SUBMITTED ONCE THE DEVICE IS GONE, and the reason is not politeness: beginFrame
    // returns before resetting the command list when the device is lost, so the list here is
    // whatever was left from the last good frame -- already closed. Closing it again and submitting
    // it would be two API misuses stacked on top of a failure that has already been reported.
    if (deviceLost_) return;
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (infoQueue_) drainDebugMessages();
}

// Presents the frame, signals its fence, and services a pending capture.
void D3D12Device::present() {
    if (!hasSwapchain_ || deviceLost_) return;
    const bool tearing = !vsync_ && tearingSupported_;
    const UINT interval = vsync_ ? 1u : 0u;
    const UINT flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    // Staged loss, counted in PRESENTED frames so it is reproducible run to run. Checked before the
    // real Present so the frame it fires on behaves exactly like one the device died during.
    if (const u32 loseAt = simulatedDeviceLoss(); loseAt != 0 && ++presentedFrames_ >= loseAt) {
        noteDeviceRemoved("--device-lost-at (SIMULATED, the hardware is fine)", DXGI_ERROR_DEVICE_HUNG);
        return;
    }
    const HRESULT pr = swapChain_->Present(tearingSupported_ ? interval : 1u, flags);
    // PRESENT IS WHERE A REMOVAL USUALLY SURFACES FIRST, so it is the most likely place to learn
    // about one. It used to log and carry on, which is how a single lost device turned into a
    // screenful of identical errors and then a hard fault somewhere else entirely.
    if (FAILED(pr)) {
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
            noteDeviceRemoved("Present", pr);
            return;
        }
        AVER_ERROR("[RHI.D3D12] Present failed 0x{:08X} removed=0x{:08X}", (u32)pr,
                   (u32)device_->GetDeviceRemovedReason());
    }

    // THE FENCE VALUE IS ONLY ADVANCED IF THE SIGNAL WAS ACCEPTED. Writing it unconditionally --
    // which is what this did -- records a value the GPU can never reach when the queue has already
    // failed, and the NEXT beginFrame for this backbuffer then waits on it: a one-second stall per
    // frame, forever, chasing a number nothing will ever signal.
    if (FAILED(queue_->Signal(fence_.Get(), nextFence_ + 1))) {
        noteDeviceRemoved("the present fence signal", DXGI_ERROR_DEVICE_REMOVED);
        return;
    }
    ++nextFence_;
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
    // Reset alongside depthBuffer_/msaaColor_ above regardless of which branch below runs -- both
    // recreate at the CURRENT sceneWidth_/sceneHeight_, and createGBufferTargets expects the ComPtrs
    // already released, matching createDepthBuffer/createMsaaColor's own contract.
    if (gbufferEnabled_) { gbufVelocity_.Reset(); gbufViewZ_.Reset(); gbufNormalRough_.Reset(); }
    const UINT scFlags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!hrOk(swapChain_->ResizeBuffers(kFrameCount, w, h, kBackbufferFormat, scFlags), "ResizeBuffers")) {
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
    // Gated on gbufferEnabled_ for the identical reason rebuildSceneTargets() is -- see that
    // method's own comment: these three exist only for a build that opted in, so an unconditional
    // recreate here would allocate ~54 MB every resize of every build, feature on or not.
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
// Records that the device has gone, exactly once, with the reason decoded.
//
// ONCE: everything downstream of a removal fails too, and a per-call log would bury the one line
// that says what happened under thousands that say what happened next. `where` names the call that
// noticed, not the call that caused it -- a removal is discovered late by construction, but it's
// still the most useful thing available.
bool D3D12Device::noteDeviceRemoved(const char* where, HRESULT hr) {
    if (deviceLost_) return true;
    deviceLost_ = true;
    HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
    // A SIMULATED loss (see rhi::setSimulatedDeviceLoss) leaves a perfectly healthy device behind,
    // so the reason it reports is S_OK and would decode as "unknown". Fall back to what the caller
    // said in that case -- the caller is the only one who knows this was staged.
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
    // CRITICAL, NOT ERROR -- the event this severity was introduced for. The process may keep running
    // a long time (the last frame stays on screen, the editor's UI still responds), but it's now a
    // dead engine walking, and much downstream will fail for a reason traced back to this line.
    // Logging Critical also wakes the crash reporter (CrashReport.hpp): a separate process starts
    // watching NOW, while still healthy enough to spawn anything, rather than launching a helper
    // mid-collapse.
    AVER_CRITICAL("[RHI.D3D12] THE GPU DEVICE HAS BEEN LOST, noticed at {} (0x{:08X}): {}. Nothing "
                  "further will be drawn -- this engine cannot recreate a device, so the editor has "
                  "to be restarted. The last frame stays on screen.",
                  where, static_cast<u32>(reason), what);
    return true;
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
// was installed (every game build, and AVER_ENABLE_UI=OFF) or it failed to initialise; either way
// every uiXxx() below then behaves exactly as IDevice's own no-op defaults do.
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

// Creates the one shader-visible descriptor heap every binding set and the bindless table
// suballocate from, plus the CPU-only staging heap binding sets stage their writes into (see
// RhiBindingSet's comment).
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

// Writes a null view of the declared dimension into every slot of a set. Tier 1 hardware reads
// undefined data from any descriptor in a bound table that was never written.
//
// PER-SLOT SlotKind IS WHAT MAKES A MIXED TABLE 0 POSSIBLE, unlike the other backend: Stage 3's GPU
// per-cluster path merges Voxi's table-0 union (a Texture3D, an AccelerationStructure, three
// StructuredBuffers, four Texture2Ds) into its own table 0 because this loop and setSrv/setUav
// switch on SlotKind per slot rather than assuming one shape. VulkanPipeline.cpp's
// descriptorLayout() assumes every table-0 slot is a Texture2D (already wrong for Voxi, a
// pre-existing defect not fixed here) -- the merge stays D3D12 only, a property of the whole design.
// The null view for one SRV slot, by kind. Lifted out of nullFill so clearSrv writes the identical
// descriptor: a slot returned to null later must be indistinguishable from one that was never bound.
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
    return sv;
}

// Writes into the STAGING heap, like every other writer below -- see RhiBindingSet's comment.
// Takes `s` by non-const reference (its only caller, createBindingSet, owns a fresh local) so it can
// bump `version` itself: a set nullFill has just touched must copy to its shader-visible range on
// the very first setBindingSet, the same as any other write.
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

// Same shape as allocRange, over the CPU-only staging heap's own free list -- see stageFreeRanges_'s
// comment for why this is a separate list rather than allocRange taking a heap parameter.
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
    // +3 mesh geometry SRVs/count, +1 instanced-draw world-matrix SRV, +1 bindless table.
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

    // RegisterSpace is written unconditionally rather than under `if (layout.constantSpace)`,
    // because params[] is zero-initialised: assigning 0 is what it already held, so a layout that
    // leaves constantSpace at its default serialises to exactly the bytes it did before this field
    // existed. Same for the static samplers below. That byte-identity is the property that makes
    // this additive for every pipeline in the engine, and it is checked by the gates rather than
    // asserted here -- every non-RT gate must stay bit-identical across this change.
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
        // One more root SRV, past declaredSrvCount(layout) AND past the two mesh geometry SRVs when
        // this is also a mesh pipeline (msVertexParam/msIndexParam already claimed those) -- see
        // RHIResources.hpp's comment above GraphicsPipelineDesc::instanced for the register
        // arithmetic a shader compiling against this layout must reproduce.
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[n].Descriptor.ShaderRegister = declaredSrvCount(layout) + (mesh ? 2 : 0);
        e.instanceWorldParam = static_cast<i32>(n++);
    }
    // THE BINDLESS TEXTURE TABLE, APPENDED LAST AND IN REGISTER SPACE 1.
    //
    // Last, so every layout leaving bindlessTextureCount at 0 (every raster pipeline, including all
    // on the FL 11_0 minimum-spec path) serialises to exactly the bytes it did before this branch
    // existed -- nothing above this line reads the new field.
    //
    // Space 1, so the range can't collide with any t-register the two ordinary tables, the mesh
    // geometry SRVs or the instanced world-matrix SRV already claimed in space 0 (assigned by
    // arithmetic over declaredSrvCount(), frozen by the RHI header and shader prelude). A separate
    // space costs nothing and cannot alias.
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

// See the declaration's own comment for the promotion contract every `dst` must already satisfy.
// UNMEASURED: this blocks the calling thread on a GPU round trip per call (one per mesh, since
// createMesh calls this once for its vertex+index pair together), and nothing here has timed what
// that costs at load time on real content -- see W4's brief for why that cost was accepted anyway.
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

    // EXPLICIT, not left to submit-time decay -- see this function's own comment on why the
    // promotion must not linger past this one-shot list.
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

// See this method's own declaration (D3D12ResourceFactory class body) for what it is for.
TextureHandle D3D12ResourceFactory::adoptExternalDepthTexture(ID3D12Resource* resource, u32 width, u32 height,
                                                               TextureHandle existing) {
    if (!resource) return 0;
    RhiTexture t;
    t.res = resource;   // ComPtr(T*) AddRefs; ownership is SHARED with dev_->depthBuffer_'s own
                        // ComPtr, not stolen from it -- both go to zero together when the device does.
    t.desc.dim = TextureDim::Tex2D;
    t.desc.width = width; t.desc.height = height; t.desc.depth = 1; t.desc.mips = 1;
    // R32Typeless, not D32Float: the SAME "DSV sees D32Float, SRV sees R32Float" alias
    // VoxiRenderer's shadow map uses (Format::R32Typeless's doc comment, RHIResources.hpp), so
    // toDxgiSrvFormat below resolves it to R32_FLOAT with no backend-specific depth-format knowledge.
    t.desc.format = Format::R32Typeless;
    t.desc.bind = ResourceBind::ShaderResource | ResourceBind::DepthStencil;
    // Matches physical reality at adoption: createDepthBuffer() leaves the resource in DEPTH_WRITE
    // and nothing transitions it away before the render loop uses it as a depth target every frame.
    // A caller wanting to READ it (modules/occlusion) is responsible for the DepthWrite <->
    // NonPixelShaderResource round trip via textureBarrier -- this factory can't know when that's safe.
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

// See this method's own declaration (D3D12ResourceFactory class body) for what it is for.
TextureHandle D3D12ResourceFactory::adoptExternalRenderTargetTexture(ID3D12Resource* resource, Format fmt,
                                                                     u32 width, u32 height,
                                                                     const char* debugName,
                                                                     TextureHandle existing) {
    if (!resource) return 0;
    RhiTexture t;
    t.res = resource;   // ComPtr(T*) AddRefs; SHARED with the D3D12Device member's own ComPtr
                        // (gbufVelocity_ etc.) -- same contract adoptExternalDepthTexture documents.
    t.desc.dim = TextureDim::Tex2D;
    t.desc.width = width; t.desc.height = height; t.desc.depth = 1; t.desc.mips = 1;
    t.desc.format = fmt;
    t.desc.bind = ResourceBind::ShaderResource | ResourceBind::RenderTarget;
    // Matches physical reality at adoption: createGBufferTargets() creates these three resources
    // directly into RENDER_TARGET and nothing transitions them away before the render loop binds
    // them every frame -- identical in spirit to depthBuffer_'s DEPTH_WRITE above, and the same
    // caller obligation applies: round-trip through textureBarrier, which this factory can't do on
    // the reader's behalf.
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

    // THE PRECOMPILED PATH, TAKEN FIRST AND SHORT-CIRCUITING EVERY GATE BELOW. Not one of the
    // checks that follow can be applied to bytecode: there is no source to compile, no entry point
    // to name, and no shader model to request -- DXIL declares its own, so a minShaderModel test
    // here would be testing a field the caller was told is ignored. The device's own SM cap is not
    // re-checked either; CreateComputePipelineState rejects bytecode the device cannot run, which
    // is a real check rather than one made from a struct field. See ShaderDesc::bytecode.
    if (d.precompiled()) {
        // MESH AND AMPLIFICATION STILL NEED THE HARDWARE, which is a property of the device and not
        // of the bytecode, so this one gate survives.
        if ((d.stage == ShaderStage::Mesh || d.stage == ShaderStage::Amplification)
            && dev_->caps_.meshShaderTier == 0) {
            AVER_WARN("[RHI.D3D12] createShader (precompiled) needs mesh-shader hardware, which this "
                      "device reports as tier 0");
            return 0;
        }
        // THE CONTAINER FOURCC, checked for the same reason the Vulkan side checks SPIR-V's magic
        // number: the one mistake a caller can actually make here is handing the wrong backend's
        // blob to the wrong backend, and NRD supplies DXIL and SPIR-V side by side so the two are
        // one field apart. Caught by name here; caught by CreateComputePipelineState as an opaque
        // E_INVALIDARG otherwise. Every DXIL container starts with 'DXBC' -- the fourcc kept its
        // old name across the DXBC-to-DXIL change.
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

    // Mesh and Amplification share one floor: both are D3D12 Ultimate stages, unavailable below
    // Tier 1 mesh-shader hardware regardless of shader model. The explicit half of the degrade
    // (house rule 6); the other half is that a PSO built from a rejected shader never gets created,
    // so the caller's existing draw path is untouched. See D3D12Device::initMeshShaders for the same
    // gate on the backend's own fixed voxelisation pipeline.
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
        // allocRange already named the heap and the shortfall. Say what was being asked for, since
        // this is by far the largest single request anything in the engine makes of that heap.
        AVER_ERROR("[RHI.D3D12] bindless texture table of {} descriptors did not fit; ray-traced "
                   "texturing will stay off and the flat-albedo path will be used instead", capacity);
        return 0;
    }
    // EVERY SLOT NULL-FILLED BEFORE ANYTHING IS BOUND: a descriptor table is validated as a whole
    // when bound, not per-slot on use, so one uninitialised descriptor anywhere is a device-removal
    // risk even if unindexed -- the same reason nullFill() exists for ordinary binding sets.
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
    // Through the SAME deferred path a binding set uses: the GPU may still be reading this range
    // from a frame in flight, and returning it to the free list now would let the next allocation
    // overwrite descriptors that are still being sampled.
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
    // REFUSED, NOT CLAMPED, NOT WRAPPED: writing past the range would land a descriptor in whatever
    // binding set was allocated after it, read by a completely unrelated draw -- this engine has
    // already lost a device to exactly this shape of error. The caller falls back to unbound.
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
    // One staging range (the authoritative one) plus one shader-visible range per frame in flight --
    // see RhiBindingSet's comment. Nothing has touched the GPU with any of these yet, so a failure
    // partway through hands the ranges already taken straight back to the free lists rather than
    // leaking them behind a fence that will never retire them.
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

// The same linear scan destroyBlasForMesh already does, and proportionate for the same reason: one
// entry per distinct mesh ever ray-traced, walked when a feature first meets a mesh rather than per
// frame. `built` is what makes the result safe to hand over -- see IResourceFactory::blasForMesh.
// A destroyed structure clears both `mesh` and `built`, so a dead slot excludes itself here.
BlasHandle D3D12ResourceFactory::blasForMesh(MeshHandle mesh) const {
    if (mesh == 0) return 0;
    for (usize i = 0; i < blases_.size(); ++i)
        if (blases_[i].mesh == mesh && blases_[i].built) return static_cast<BlasHandle>(i + 1);
    return 0;
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
    // shrink_to_fit, not clear(): clear() alone leaves the capacity allocated, and a precompiled
    // shader's bytes are the whole point of this branch existing -- NRD's 159 permutations are
    // megabytes if none of them is ever really given back.
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

// Returns a binding set's descriptor ranges -- the staging range and every per-frame shader-visible
// range -- reusable once the fence passes.
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
    // Written into the STAGING heap -- see RhiBindingSet's comment. Never read by the GPU directly;
    // setBindingSet copies it to whichever shader-visible range the current frame needs, gated on
    // the fence beginFrame already waited for.
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
    // REFUSED RATHER THAN ATTEMPTED, BECAUSE ATTEMPTING IT REMOVES THE DEVICE. A texture created
    // without ResourceBind::UnorderedAccess has no D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, and
    // CreateUnorderedAccessView on one is not a failed call that returns an error -- D3D12 treats it
    // as undefined behaviour and triggers RemoveDevice with DXGI_ERROR_INVALID_CALL on the spot. The
    // whole adapter goes, the upload ring and every later CreateRootSignature fail, and the post
    // chain cannot present. With the debug layer off it is completely silent about the cause; with it
    // on it is debug layer #340, which is how this was finally found after it had been misattributed
    // to NRD dispatch counts, to a resource state and to an unbound root CBV in turn.
    //
    // One line here converts the worst diagnostic in the engine into a named texture and a live frame.
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
    // Staging heap, same reason as setSrv above.
    dev_->device_->CreateUnorderedAccessView(t->res.Get(), nullptr, &uv, stagingCpu(s->stageBase + s->srvCount + slot));
    noteBindingSetWritten(set, *s);
}

// Writes an acceleration-structure SRV into one slot of a binding set.
// Returns one SRV slot to null. See IResourceFactory::clearSrv for why a stale descriptor is a
// device removal rather than a wrong pixel.
//
// The write lands in the STAGING heap, never the shader-visible one the GPU can be reading mid-frame
// -- see RhiBindingSet's comment. It cannot race a GPU read because nothing reads the staging heap;
// setBindingSet is the only thing that ever copies out of it, into whichever frame's range is safe to
// touch. What this must NOT do is nothing -- the caller reaches this because the texture it was
// showing is being destroyed, and leaving the old descriptor live in the eventual GPU copy is the one
// outcome that crashes.
void D3D12ResourceFactory::clearSrv(BindingSetHandle set, u32 slot) {
    RhiBindingSet* s = bindingSet(set);
    if (!s) { AVER_ERROR("[RHI.D3D12] clearSrv with an invalid set"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.D3D12] clearSrv slot {} past the {} declared", slot, s->srvCount); return; }
    const D3D12_SHADER_RESOURCE_VIEW_DESC sv = nullSrvDesc(s->srvKinds[slot]);
    dev_->device_->CreateShaderResourceView(nullptr, &sv, stagingCpu(s->stageBase + slot));
    noteBindingSetWritten(set, *s);
}

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
    // Staging heap, same reason as setSrv above.
    dev_->device_->CreateShaderResourceView(nullptr, &sv, stagingCpu(s->stageBase + slot));
    noteBindingSetWritten(set, *s);
}

// True when a structured view of `count` elements of `stride` starting at `firstElement` fits
// inside the buffer it is being created over. Logs and returns false when it does not.
//
// Checked here, not left to the debug layer: an overrunning view isn't rejected by D3D12 at creation,
// so the first thing that notices is a shader reading/writing past the allocation, surfacing as
// DXGI_ERROR_DEVICE_HUNG on a different thread with no reference to the descriptor that caused it.
//
// Cost this project exactly that once: the path tracer's denoise buffers, allocated at 480x270, were
// handed a 1280x720 view when the quality rung changed, and the report was "sometimes path tracing
// crashes the engine at higher settings" -- the only lead was --debug-layer, which nobody runs by
// default. One comparison at descriptor-write time turns that into a log line naming the slot, the
// counts and the buffer.
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
    // Staging heap, same reason as setSrv above.
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
    // Staging heap, same reason as setSrv above.
    dev_->device_->CreateUnorderedAccessView(buffers_[bh - 1].res.Get(), nullptr, &uv,
                                             stagingCpu(s->stageBase + s->srvCount + slot));
    noteBindingSetWritten(set, *s);
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

    // Probes the STAGING allocator's free list, not the shader-visible one: stageBase is the one
    // range every binding set always has exactly one of, so it is the unambiguous proxy for "does
    // this allocator hold a destroyed range behind the fence rather than handing it straight back".
    // The shader-visible allocator (pendingRanges_/freeRanges_, shared with the bindless table) is
    // the SAME allocator code and fence discipline, so this remains a faithful check of both.
    const u32 firstBase = set ? bindingSets_[set - 1].stageBase : 0;
    destroyBindingSet(set);
    const BindingSetHandle early = createBindingSet(bsd);
    const bool heldBack = early && bindingSets_[early - 1].stageBase != firstBase;
    AVER_INFO("[RHI.D3D12] factory self-test: descriptor reclaim {} (returned range held behind the fence)",
              heldBack ? "ok" : "FAILED");

    destroyBindingSet(early);

    // Buffer views, what a compute skinning pass needs and what didn't exist. The DESCRIPTOR half is
    // what this checks: a Tier 1 null descriptor of the wrong dimension is undefined, and a
    // structured-buffer view with no stride is rejected outright, both caught here by the debug layer
    // rather than in a shader that silently reads zeros. The dispatch half can't be checked here:
    // this runs at init, with no open command list.
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
    dev_->dbValid_ = false;           // table 1's cache dies too: a graphics root signature change
                                       // discards every bound root argument including table 1's
                                       // descriptor table, and bindDeclaredRootCbvs below re-zeroes
                                       // this cache's own b2 slot regardless -- see dbValid_'s comment
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

    // This backend has exactly one generic heap, so thousands of feature-overridden draws called this
    // with the SAME single-entry array every time. Elided the same way boundRootSig_/boundPso_ elide
    // their own redundant sets -- see boundHeap_'s comment for the invalidation this depends on at
    // every OTHER SetDescriptorHeaps call site.
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
    //
    // Whether table 1 can ALSO be elided on its own terms is a different question from whether it
    // should invalidate fov* -- see applyDrawBinding's dbValid_ cache for that one. Kept as a
    // separate flag and a separate comment on purpose, so the two reasons never collapse into one.
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

    // fi is the SAME backbuffer index beginFrame just waited a fence for, so gpuBase[fi] is a range
    // the GPU has finished reading (or never touched) -- rewriting it here can never race a dispatch
    // that is still in flight. Copied only when this set changed since fi's range last received a
    // copy (kFrameCount frames ago, i.e. the last time this same backbuffer was recorded); a set
    // that is untouched between two uses of the same backbuffer costs nothing here. See
    // RhiBindingSet's comment for the full account of why the OLD single-range scheme raced the GPU.
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
    // A DELIBERATE NO-OP on a pipeline without one, rather than an error: the caller binds this
    // once per pass and should not have to know which PSO variant the renderer picked this frame.
    if (pipe_->bindlessParam < 0) return;
    const u32 cap = res_->bindlessTableCapacity(table);
    if (cap == 0) { AVER_ERROR("[RHI.D3D12] setBindlessTable with an invalid table handle"); return; }

    // The heap must be bound before the table can be, exactly as setBindingSet does it -- a pass
    // that binds this WITHOUT ever calling setBindingSet (a fullscreen ray pass may well not) would
    // otherwise set a root table against whatever heap the last caller left bound.
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
//
// TABLE 1 REDUNDANT-STATE ELISION, OFF BY DEFAULT. Read the whole comment before touching either
// half below: getting this cache's invalidation wrong is not a slow frame, it is a WRONG-MATERIAL
// DRAW whose failure mode is a plausible image. This house has that scar already -- SandboxRender.cpp
// records a cluster-dispatch path that once left table 1 sticky from an earlier draw's material, and
// the mesh rendered black "even though every value measured correct for a different material". A
// broken frame from a stale table 1 does not look broken; it looks like a different, wrong object.
//
// THE FINDING this closes. setBindingSet's own comment a few dozen lines above explains why the fov*
// group (pipeline + table 0 + the feature frame CBV) deliberately EXCLUDES table 1 from ITS elision
// -- invalidating fov* on every table-1 rebind would mean fov* never once survived to the next
// entity, since table 1 legitimately changes whenever the material does. That reasoning is about
// fov*, and it is correct. It is silent on a different question: whether table 1 can ALSO be elided
// on its OWN terms, by comparing THIS draw's (drawSet_, drawConstants_) against the PREVIOUS draw's
// rather than against table 0. Until this change, nothing did. setBindingSet(drawSet_, 1) below and
// the b2 constant-buffer upload after it both ran UNCONDITIONALLY on every one of this function's
// five call sites (drawMesh, drawMeshInstanced, dispatchMeshFor, dispatchMeshClusters, drawIndexed)
// -- a descriptor-table bind, a 256-byte-aligned ring allocation, a memcpy and a root CBV set, every
// single draw, even across thousands of consecutive draws sharing one material. Verified before
// writing this: setBindingSet(_, 1) has exactly one call site in this whole file, the one inside
// this very function below, so there is no other path that can move table 1 behind this cache's
// back.
//
// WHY ELIDING IS SAFE HERE. dev_->dbSet_/dbConstants_/dbConstantBytes_ mirror what this function
// last actually BOUND on the command list, not merely what a caller last requested through
// setDrawBinding -- each half below only updates its own cached copy at the moment it issues the
// real call, so a half this function SKIPS never drifts the cache away from hardware truth, and a
// draw that changes only one half (same material set, different shading constants, or the reverse)
// still pays only for the half that changed. dev_->dbValid_ is cleared at every point that can
// invalidate root arguments out from under this cache WITHOUT going through setBindingSet or
// setConstantBuffer: D3D12RenderContext::setPipeline (a new root signature discards every bound root
// argument, and bindDeclaredRootCbvs re-zeroes this cache's own b2 slot immediately afterwards
// regardless), D3D12Device::beginFrame (cmdList_->Reset leaves nothing bound at all), and the two
// points in D3D12Device::endFrame where the sky/post chain and the blended-mesh replay's own
// pipeline swaps touch the command list directly, bypassing setPipeline -- the exact same reason
// fovValid_ is force-cleared at those same two points. dbValid_ is a SEPARATE bool from fovValid_,
// not a reuse of it: fovValid_ is ALSO cleared by setBindingSet on table 0 (see that function's
// comment for why), and table 0 rebinding must NOT throw this cache away -- folding the two together
// would silently undo most of what this one exists to save.
//
// STILL DEFAULT OFF. A state cache is the category with the worst track record in this codebase --
// the fov* comment two functions up names its own near-miss, and SandboxRender.cpp's foliage above
// is a second one, in a different cache entirely -- and this one cannot be validated the way the
// previous two commits in this programme were, by construction rather than by a screenshot: eliding
// a redundant bind has no visible effect to diff UNLESS the elision is wrong, and a wrong elision's
// image does not necessarily look wrong either (see the foliage scar again). The only real evidence
// is a PIX capture showing table 1's root-parameter set and the b2 CBV set visibly disappearing
// between consecutive same-material draws, paired with an image diff against the same capture with
// the toggle off showing zero pixel difference. Until the user has run that A/B, this stays off.
// AVER_D3D12_ELIDE_DRAW_BINDING flips it without a rebuild specifically so that A/B is one binary,
// not a rebuild-and-compare against a second one that could differ for reasons having nothing to do
// with this cache.
//
// VULKAN HAS NO TWIN OF THIS YET. VulkanDevice.cpp's own drawMesh comment says outright that backend
// re-binds its pipeline and b0 set on every single call, "redundant, not incorrect... an
// optimisation left for later" -- table 1 there is further behind than table 0 was on THIS backend
// before the profiler and the mesh-probe cache that preceded this change even existed. The owner's
// stated goal is Vulkan parity, so this cache will want a twin on that backend eventually. Not built
// here: this pass touches D3D12Device.cpp only.
void D3D12RenderContext::applyDrawBinding() {
    if (!pipe_) return;

    // Read once for the process, not once per draw -- see the block comment above for what must be
    // verified before anyone flips this permanently. AVER_D3D12_ELIDE_DRAW_BINDING absent, empty or
    // "0" leaves this function byte-for-byte the sequence it always issued: both `elided` checks
    // below short-circuit on kElide first, so neither cache field is even read, let alone written,
    // when this stays off.
    static const bool kElide = [] {
        const char* v = std::getenv("AVER_D3D12_ELIDE_DRAW_BINDING");
        return v && v[0] != '\0' && v[0] != '0';
    }();

    if (drawSet_ && pipe_->srvParam[1] >= 0) {
        // Same table-1 binding set as the last draw this cache saw applied? Then the descriptors it
        // points at are already the ones bound -- re-issuing the same SetGraphicsRootDescriptorTable
        // would describe hardware state that is already correct.
        const bool elided = kElide && dev_->dbValid_ && dev_->dbSet_ == drawSet_;
        if (!elided) {
            setBindingSet(drawSet_, 1);
            if (kElide) dev_->dbSet_ = drawSet_;
        }
    }
    if (drawConstantBytes_ && pipe_->slotParam[kDrawConstantRegister] >= 0 &&
        pipe_->slotDwords[kDrawConstantRegister] == 0) {
        // Byte-identical b2 block to the one already bound? Then the ring allocation, the memcpy into
        // it and the root CBV set below would upload and point at bytes the GPU is already reading.
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
    // Set AFTER both halves above, same discipline fovValid_ follows near the top of this file: a
    // bind this call makes must land in the cache before the NEXT applyDrawBinding trusts either half
    // of it. Left false (never written) while kElide is false, so a disabled cache never reports
    // itself valid to some later call that flips the flag mid-process.
    if (kElide) dev_->dbValid_ = true;
}

// Copies `bytes` into this frame's upload ring and returns their GPU address.
//
// THE RING GROWS, and it didn't used to: a fixed 1MB per frame crossed the cliff at ~4,000 draws --
// past that ringAlloc returned 0 and the draw silently lost its constants. Worse than the drops: one
// AVER_ERROR per failed call produced over four million log lines on a 5,760-instance forest in a
// 600-frame run, costing more time than the rendering did.
//
// Growth happens at the FRAME BOUNDARY, never mid-frame: addresses already handed out point into the
// live buffer, and reallocating under them would hand the GPU freed memory. An exhausted frame still
// loses its remaining constants -- unrescuable -- but records the size it wanted, so the next frame
// is big enough. One bad frame on the way up, not a permanently broken scene.
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
// RHIResources.hpp for why this is a separate entry point from dispatchMeshFor. The cluster arrays
// (MeshletDesc/Bounds/Vertices/Triangles) are whatever the caller already bound via
// setBindingSet/setSrvBuffer -- this only supplies the group count and, when `mesh` is live, its
// plain vertex buffer, since every cluster's MeshletVertices are global indices into that buffer
// (FORMAT_SPECS 5.7).
void D3D12RenderContext::dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) {
    if (!pipe_ || !pipe_->mesh || !pipe_->amplification) {
        AVER_ERROR("[RHI.D3D12] dispatchMeshClusters without an amplification-shader pipeline");
        return;
    }
    if (!dev_->cmdList6_) { AVER_ERROR("[RHI.D3D12] DispatchMesh is unavailable on this command list"); return; }
    if (clusterCount == 0) return;
    applyDrawBinding();
    // FIX: the root signature this pipeline was built with ALWAYS reserves
    // msVertexParam/msIndexParam/msCountParam as three extra root parameters (see
    // D3D12ResourceFactory::rootSignature's "FROZEN: geometry SRVs sit past both declared tables"
    // comment), regardless of whether this pipeline's shaders read all three. Leaving any unset is an
    // UNINITIALIZED ROOT ARGUMENT, undefined per spec -- exactly what this house's TDR history says
    // to take seriously. The cluster mesh shader never reads msIndexParam or msCountParam, but this
    // call still binds real addresses to both (this mesh's index buffer, and clusterCount) so nothing
    // is ever unset even though nothing here reads them.
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
// GetCopyableFootprints IS THE AUTHORITY, not arithmetic on width*bpp: D3D12 aligns every copy row
// to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256), so a 16-wide RGBA16F mip carries 128 bytes in a
// 256-byte row, and a caller assuming tight packing reads one row's data interleaved with another's
// padding. Asking the runtime also gets the right answer for a format whose block size isn't modeled here.
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
        // The values are chosen to match D3D12_RAYTRACING_INSTANCE_FLAGS one for one, so this is a
        // copy rather than a translation -- but it is written as an explicit mask-and-assign, not a
        // blind cast, so that a flag added on the engine side which does NOT have a D3D12 twin
        // fails to compile here instead of being handed to the driver as an unknown bit.
        u32 d3dFlags = 0;
        if (instances[i].flags & TlasInstanceFlag_TriangleCullDisable)
            d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        if (instances[i].flags & TlasInstanceFlag_TriangleFrontCcw)
            d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
        if (instances[i].flags & TlasInstanceFlag_ForceOpaque)
            d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        if (instances[i].flags & TlasInstanceFlag_ForceNonOpaque)
            d3dFlags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        id.Flags = d3dFlags;
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

// Opens a debug marker region. Metadata 1 is the ANSI-string form PIX and RenderDoc understand. Also
// opens a GPU timing span nested under whatever is already open -- the mechanism ScopedGpuStat
// (RHIResources.hpp) wraps; prefer that over a bare pushMarker/popMarker pair unless a single C++
// scope can't cover it (see beginGpuSpan's comment for the one place that's true here).
void D3D12RenderContext::pushMarker(const char* label) {
    if (!label || !dev_->cmdList_) return;
    dev_->cmdList_->BeginEvent(1, label, static_cast<UINT>(std::strlen(label) + 1));
    // The label is a string LITERAL at every call site, so storing the pointer is safe and keeps
    // this allocation-free on the hot path -- see GpuSpan's comment.
    //
    // Parent, same rule as beginGpuSpan: whatever is already open when this marker opens, kNoParent
    // if this is the first (e.g. VoxiRenderer::prePass's outer "Voxi GI update" scope).
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
