// VoxiSpirvCompileTest -- the Vulkan half of VoxiShaderCompileTest: compiles the RayQuery-heavy
// shaders (Voxi's scene/ray-driven variants, the water displacement kernel, the reference path
// tracer, NRD2's temporal passes, the NeuRaC guide and resolve) to SPIR-V with DXC, the way rhi.vulkan's
// VulkanShaderCompiler does, with no device.
// Without it a RayQuery construct DXC cannot lower to SPIR-V only surfaces on a Vulkan run.
// Skips (77) when the SPIR-V-capable dxcompiler.dll (third_party/dxc-spirv) is absent.
//
// Scope: the layout-agnostic compile createShader does first (no -fvk-bind-register map, no
// push-constant patching) -- it proves DXC accepts the source, not that descriptors land right.
// The defines below mirror VoxiRenderer::createScenePipelines, PathTracer.cpp, WaterSurface.cpp, Nrd2.cpp
// (AVER_NRD2_PASS, no prelude) and NeuRaC.cpp / NeuRaCGuide.cpp (SM 6.2, no defines).

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using rhi::ComPtr;
namespace fs = std::filesystem;

#ifndef AVER_REPO_ROOT
#error "AVER_REPO_ROOT must name the repository root"
#endif

namespace {
constexpr int kSkip = 77;

// Mirrors of constants private to their modules (kVoxiSrvCount, kMaterialSamplerSlot in
// VoxiRenderer.cpp; kRtTextureCapacity in VoxiRenderer.hpp; kEnergyLutDim, kBindlessTexCapacity in
// PathTracer.cpp; kMaxGerstnerWaves in GerstnerWave.hpp; kVkUavBindingBase in VulkanCommon.hpp). Only
// their values reach the HLSL.
constexpr u32 kVoxiSrvCount = 25;
constexpr u32 kMaterialSamplerSlot = 2;
constexpr u32 kRtTextureCapacity = 4096;
constexpr u32 kPtEnergyDim = 32;
constexpr u32 kPtTexCapacity = 4096;
constexpr u32 kMaxWaves = 4;
constexpr u32 kVkUavBindingBase = 28;   // rhi.vulkan VulkanCommon.hpp: kMaxBindingSlots (25 -> 28: many lights)

struct Variant {
    std::string name;
    std::string source;     // prelude + source, as the backend concatenates them
    const char* entry;
    char stage;             // 'v' vertex, 'p' pixel, 'c' compute
    u32 sm;                 // minShaderModel, e.g. 65
    std::string defines;    // semicolon-separated
    bool rayQuery = false;  // must declare the RayQueryKHR capability, i.e. RayQuery was not compiled out
};

std::vector<std::wstring> splitDefines(const std::string& all) {
    std::vector<std::wstring> out;
    for (size_t b = 0; b <= all.size();) {
        const size_t e = all.find(';', b) == std::string::npos ? all.size() : all.find(';', b);
        if (e > b) out.emplace_back(all.begin() + b, all.begin() + e);
        b = e + 1;
    }
    return out;
}

// The argument list is VulkanShaderCompiler::compile()'s, minus the pipeline-layout register map.
// Keep the two in step: a flag added there belongs here.
bool compileSpirv(IDxcCompiler3* compiler, IDxcUtils* utils, const Variant& v, std::string& err, size_t& bytes) {
    constexpr u32 kOpCapability = 17, kCapRayQueryKHR = 4472;
    const wchar_t* prefix = v.stage == 'v' ? L"vs" : v.stage == 'p' ? L"ps" : L"cs";
    const std::wstring target = std::wstring(prefix) + L"_" + std::to_wstring(v.sm / 10) + L"_" + std::to_wstring(v.sm % 10);
    const std::wstring entry(v.entry, v.entry + std::strlen(v.entry));
    std::vector<std::wstring> defs = splitDefines(v.defines);
    bool hlsl2018 = false;
    for (size_t i = 0; i < defs.size();) {
        if (defs[i] == L"AVER_HLSL_2018") { hlsl2018 = true; defs.erase(defs.begin() + static_cast<std::ptrdiff_t>(i)); }
        else ++i;
    }
    std::vector<LPCWSTR> args = {
        L"-E", entry.c_str(), L"-T", target.c_str(), L"-Zpr", L"-HV", hlsl2018 ? L"2018" : L"2021",
        L"-spirv", L"-fspv-target-env=vulkan1.3", L"-I", L".",
    };
    const std::wstring shift = std::to_wstring(kVkUavBindingBase);
    args.push_back(L"-fvk-u-shift"); args.push_back(shift.c_str()); args.push_back(L"0");
    if (v.stage == 'v') args.push_back(L"-fvk-invert-y");
    for (const std::wstring& d : defs) { args.push_back(L"-D"); args.push_back(d.c_str()); }

    DxcBuffer buf{v.source.c_str(), v.source.size(), DXC_CP_UTF8};
    rhi::DxcShaderInclude includes(utils);
    ComPtr<IDxcResult> result;
    HRESULT hr = compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), &includes, IID_PPV_ARGS(&result));
    if (SUCCEEDED(hr) && result) result->GetStatus(&hr);
    ComPtr<IDxcBlobUtf8> errs;
    if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength())
        err.assign(errs->GetStringPointer(), errs->GetStringLength());
    if (FAILED(hr)) return false;
    ComPtr<IDxcBlob> obj;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj || obj->GetBufferSize() == 0 ||
        obj->GetBufferSize() % 4 != 0) {
        err = "DXC reported success but produced no SPIR-V";
        return false;
    }
    bytes = obj->GetBufferSize();
    if (v.rayQuery) {
        // Capabilities lead the module, right after the 5-word header.
        const u32* w = static_cast<const u32*>(obj->GetBufferPointer());
        const size_t n = bytes / 4;
        bool found = false;
        for (size_t i = 5; i < n && (w[i] & 0xffffu) == kOpCapability; i += w[i] >> 16)
            if (i + 1 < n && w[i + 1] == kCapRayQueryKHR) found = true;
        if (!found) { err = "SPIR-V lacks the RayQueryKHR capability: the ray queries were compiled out"; return false; }
    }
    return true;
}

// shaderFile() is a flat namespace; stage every module's shaders into one temp directory so the test
// does not depend on a stale bin/shaders deploy.
std::string stageShaders() {
    const fs::path root = fs::path(AVER_REPO_ROOT) / "modules";
    const fs::path dst = fs::temp_directory_path() / "AverVoxiSpirvTestShaders";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const char* mod : {"rhi", "render.pbr", "render.voxi", "render.pt", "fluids", "render.denoise"}) {
        for (const auto& e : fs::directory_iterator(root / mod / "shaders", ec)) {
            const std::string ext = e.path().extension().string();
            if (e.is_regular_file() && (ext == ".hlsl" || ext == ".hlsli"))
                fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }
    return dst.string();
}
}  // namespace

int main() {
    AVER_INFO("=== VoxiSpirvCompileTest ===");

    // The SPIR-V-capable DXC: vendored in the tree on Windows, staged into bin/ by cmake/AvDxc.cmake
    // everywhere else (Microsoft's build, or the Vulkan SDK's).
#if defined(_WIN32)
    const fs::path vendored = fs::path(AVER_REPO_ROOT) / "third_party" / "dxc-spirv" / "dxcompiler.dll";
#elif defined(__APPLE__)
    const fs::path vendored = fs::path(executableDir()) / "libdxcompiler.dylib";
#else
    const fs::path vendored = fs::path(executableDir()) / "libdxcompiler.so";
#endif
    void* dll = fs::exists(vendored) ? loadLibrary(reinterpret_cast<const char*>(vendored.u8string().c_str())) : nullptr;
    if (!dll) {
        AVER_WARN("  SKIP  {} (the SPIR-V-capable DXC) is absent", vendored.string());
        return kSkip;
    }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(librarySymbol(dll, "DxcCreateInstance"));
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        AVER_WARN("  SKIP  {} did not initialise", vendored.string());
        return kSkip;
    }

    rhi::setShaderSourceDir(stageShaders());

    // The Voxi prelude is the shared one plus the material system's; no graphs are registered here, so
    // there is no AVER_MATERIAL_GRAPH block, same as a project without graphs.
    const std::string voxiSrc = std::string(rhi::sharedShaderPrelude()) + pbr::materialShaderPrelude() +
                                rhi::shaderFile("voxi.hlsl");
    const std::string ptSrc = std::string(rhi::sharedShaderPrelude()) + rhi::shaderFile("pt_pathtrace.hlsl");
    const std::string waterSrc = rhi::shaderFile("water_displace.hlsl");
    // NRD2, the guide and the resolve are compiled with no prelude (Nrd2.cpp, NeuRaCGuide.cpp, NeuRaC.cpp).
    const std::string nrd2Src = rhi::shaderFile("nrd2.hlsl");
    const std::string guideSrc = rhi::shaderFile("voxi_neurac_guide.hlsl");
    const std::string resolveSrc = rhi::shaderFile("voxi_neurac_resolve.hlsl");

    const std::string mat = pbr::materialShaderDefines(kVoxiSrvCount, kMaterialSamplerSlot, /*layeredBsdf=*/false);
    const std::string matLayered = pbr::materialShaderDefines(kVoxiSrvCount, kMaterialSamplerSlot, /*layeredBsdf=*/true);
    const std::string bindless = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" + std::to_string(kRtTextureCapacity);
    const std::string singlePass = ";AVER_RD_SINGLE_PASS=1";
    auto vx = [&](const std::string& extra) { return extra.empty() ? mat : mat + ";" + extra; };

    std::vector<Variant> variants = {
        {"Voxi VSMain (bindless)",              voxiSrc, "VSMain",    'v', 51, vx(bindless)},
        {"Voxi VSky (bindless)",                voxiSrc, "VSky",      'v', 51, vx(bindless)},
        {"Voxi PSMainVoxi RT",                  voxiSrc, "PSMainVoxi", 'p', 65, vx("AVER_RT=1"), true},
        {"Voxi PSMainVoxi blended pass",        voxiSrc, "PSMainVoxi", 'p', 65, vx(bindless + ";AVER_BLENDED_PASS=1"), true},
        {"Voxi PSMainVoxi RT G-buffer",         voxiSrc, "PSMainVoxi", 'p', 65, vx("AVER_RT=1;AVER_GBUFFER=1"), true},
        {"Voxi PSRayDriven single pass",        voxiSrc, "PSRayDriven", 'p', 65, vx(bindless + singlePass), true},
        {"Voxi PSRayDriven split",              voxiSrc, "PSRayDriven", 'p', 65, vx(bindless + ";AVER_RD_SPLIT=1"), true},
        {"Voxi PSRayDriven split G-buffer",     voxiSrc, "PSRayDriven", 'p', 65, vx(bindless + ";AVER_GBUFFER=1;AVER_RD_SPLIT=1"), true},
        {"Voxi PSRayDriven split (layered)",    voxiSrc, "PSRayDriven", 'p', 65, matLayered + ";" + bindless + ";AVER_RD_SPLIT=1", true},
        {"Voxi CSRdVisibility",                 voxiSrc, "CSRdVisibility", 'c', 65, vx(bindless), true},
        {"Voxi CSRdShadow",                     voxiSrc, "CSRdShadow", 'c', 66, vx(bindless), true},
        {"Voxi CSRdGi",                         voxiSrc, "CSRdGi",     'c', 66, vx(bindless)},
        // MANY LIGHTS (docs/plans/wave4/MANY_LIGHTS.md 3.9): authored SM 6.5-clean, so the same source is the Vulkan pass.
        {"Voxi CSRdManyLights",                 voxiSrc, "CSRdManyLights", 'c', 65, vx(bindless), true},
        {"Water CSWaterDisplace",               waterSrc, "CSWaterDisplace", 'c', 60, "AVER_MAX_WAVES=" + std::to_string(kMaxWaves)},
        {"PT CSPathTrace",                      ptSrc, "CSPathTrace", 'c', 65, "AVER_PT_ENERGY_DIM=" + std::to_string(kPtEnergyDim), true},
        {"PT CSPathTrace bindless",             ptSrc, "CSPathTrace", 'c', 65,
         "AVER_PT_BINDLESS=1;AVER_PT_TEX_CAPACITY=" + std::to_string(kPtTexCapacity) + ";AVER_PT_ENERGY_DIM=" + std::to_string(kPtEnergyDim), true},
        // NRD2's temporal stage (Vulkan has no in-frame NRD2, but these must still lower to SPIR-V).
        {"NRD2 CSNrd2Reproject",                nrd2Src, "CSNrd2Reproject", 'c', 60, "AVER_NRD2_PASS=5"},
        {"NRD2 CSNrd2Prefilter",                nrd2Src, "CSNrd2Prefilter", 'c', 60, "AVER_NRD2_PASS=6"},
        {"NRD2 CSNrd2Temporal",                 nrd2Src, "CSNrd2Temporal",  'c', 60, "AVER_NRD2_PASS=7"},
        {"NeuRaC CSNeuracGuide",                guideSrc, "CSNeuracGuide",  'c', 62, ""},
        {"NeuRaC CSRcResolve",                  resolveSrc, "CSRcResolve",  'c', 62, ""},
    };

    int failures = 0;
    for (const Variant& v : variants) {
        std::string err;
        size_t bytes = 0;
        if (compileSpirv(compiler.Get(), utils.Get(), v, err, bytes)) {
            AVER_INFO("  PASS  {} ({} bytes of SPIR-V)", v.name, bytes);
        } else if (err.find("SPIR-V CodeGen not available") != std::string::npos) {
            AVER_WARN("  SKIP  dxcompiler.dll has no SPIR-V backend");
            return kSkip;
        } else {
            AVER_ERROR("  FAIL  {}: {}", v.name, err);
            ++failures;
        }
    }
    if (failures) {
        AVER_ERROR("=== VoxiSpirvCompileTest FAILED === ({} of {} variant(s))", failures, variants.size());
        return 1;
    }
    AVER_INFO("=== VoxiSpirvCompileTest PASSED === ({} variants)", variants.size());
    return 0;
}
