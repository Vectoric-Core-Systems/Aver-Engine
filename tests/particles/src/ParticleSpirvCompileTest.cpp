// ParticleSpirvCompileTest -- the particle shaders compile for both targets with no device: every GPU simulation kernel and the draw shaders go to SPIR-V (the way
// rhi.vulkan's VulkanShaderCompiler builds them, minus the pipeline-layout register map) and to DXIL (the D3D12 backend's DXC arguments). It proves DXC accepts
// the source, not that a driver runs it: the kernels' behaviour is ParticleModulesTest (the math, on the CPU) and ParticleGpuParityTest (a device).
// Skips (77) when the SPIR-V-capable dxcompiler.dll (third_party/dxc-spirv) is absent.
#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
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
constexpr u32 kVkUavBindingBase = 28;   // rhi.vulkan VulkanCommon.hpp: the UAV register shift

struct Variant {
    std::string name;
    std::string source;
    const char* entry;
    char stage;   // 'v' vertex, 'p' pixel, 'c' compute
    u32 sm;
};

// spirv false: DXIL for D3D12.
bool compile(IDxcCompiler3* compiler, IDxcUtils* utils, const Variant& v, bool spirv, std::string& err, size_t& bytes) {
    const wchar_t* prefix = v.stage == 'v' ? L"vs" : v.stage == 'p' ? L"ps" : L"cs";
    const std::wstring target = std::wstring(prefix) + L"_" + std::to_wstring(v.sm / 10) + L"_" + std::to_wstring(v.sm % 10);
    const std::wstring entry(v.entry, v.entry + std::strlen(v.entry));
    std::vector<LPCWSTR> args = {L"-E", entry.c_str(), L"-T", target.c_str(), L"-Zpr", L"-HV", L"2021", L"-I", L"."};
    const std::wstring shift = std::to_wstring(kVkUavBindingBase);
    if (spirv) {
        args.push_back(L"-spirv"); args.push_back(L"-fspv-target-env=vulkan1.3");
        args.push_back(L"-fvk-u-shift"); args.push_back(shift.c_str()); args.push_back(L"0");
        if (v.stage == 'v') args.push_back(L"-fvk-invert-y");
    }
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
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj || obj->GetBufferSize() == 0) {
        err = "DXC reported success but produced no object";
        return false;
    }
    bytes = obj->GetBufferSize();
    return true;
}

// shaderFile() is a flat namespace: stage the shared and the particle shaders into one temp directory.
std::string stageShaders() {
    const fs::path root = fs::path(AVER_REPO_ROOT) / "modules";
    const fs::path dst = fs::temp_directory_path() / "AverParticleSpirvTestShaders";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const char* mod : {"rhi", "particles"}) {
        for (const auto& e : fs::directory_iterator(root / mod / "shaders", ec)) {
            const std::string ext = e.path().extension().string();
            if (e.is_regular_file() && (ext == ".hlsl" || ext == ".hlsli"))
                fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }
    return dst.string();
}
} // namespace

int main() {
    AVER_INFO("=== ParticleSpirvCompileTest ===");
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
    const std::string sim = rhi::shaderFile("particles_sim.hlsl");
    const std::string draw = std::string(rhi::sharedShaderPrelude()) + rhi::shaderFile("particles.hlsl");
    const std::string sort = rhi::shaderFile("particles_sort.hlsl");
    if (sim.empty() || draw.empty() || sort.empty()) {
        AVER_ERROR("  FAIL  the particle shaders were not found");
        return 1;
    }

    std::vector<Variant> variants;
    for (const char* e : {"CSInit", "CSBegin", "CSSpawn", "CSArgsUpdate", "CSUpdate", "CSShift", "CSFinish", "CSProbe", "CSRngProbe"})
        variants.push_back({std::string("particles_sim ") + e, sim, e, 'c', 60});
    for (const char* e : {"CSSortKeys", "CSSortLocal", "CSSortGlobal", "CSSortLocalMerge", "CSSortFinal"})
        variants.push_back({std::string("particles_sort ") + e, sort, e, 'c', 60});
    variants.push_back({"particles draw ParticleSpriteVS", draw, "ParticleSpriteVS", 'v', 60});
    variants.push_back({"particles draw ParticleRibbonVS", draw, "ParticleRibbonVS", 'v', 60});
    variants.push_back({"particles draw ParticleMeshVS", draw, "ParticleMeshVS", 'v', 60});
    variants.push_back({"particles draw ParticlePS", draw, "ParticlePS", 'p', 60});

    int failures = 0;
    for (const Variant& v : variants) {
        for (const bool spirv : {true, false}) {
#if !defined(_WIN32)
            if (!spirv) continue;   // the DXIL half is Windows only, as in VoxiSpirvCompileTest (the D3D12 backend is)
#endif
            std::string err;
            size_t bytes = 0;
            const std::string label = v.name + (spirv ? " (SPIR-V)" : " (DXIL)");
            if (compile(compiler.Get(), utils.Get(), v, spirv, err, bytes)) {
                AVER_INFO("  PASS  {} ({} bytes)", label, bytes);
            } else if (spirv && err.find("SPIR-V CodeGen not available") != std::string::npos) {
                AVER_WARN("  SKIP  dxcompiler.dll has no SPIR-V backend");
                return kSkip;
            } else {
                AVER_ERROR("  FAIL  {}: {}", label, err);
                ++failures;
            }
        }
    }
    AVER_INFO("{} failure(s)", failures);
    return failures == 0 ? 0 : 1;
}
