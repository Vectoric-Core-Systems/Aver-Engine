// UiShaderCompileTest -- the UI shaders compile for both targets with no device: ui.hlsl (UiVS, UiPS, UiPSMul) and world_text.hlsl (WorldTextVS,
// WorldTextPS) go to SPIR-V (the way rhi.vulkan's VulkanShaderCompiler builds them, minus the pipeline-layout register map) and to DXIL (the D3D12
// backend's DXC arguments). It proves DXC accepts the source, not that a driver runs it.
//
// With AVER_UI_SHADER_BASE naming a directory that holds an OLDER ui.hlsl (the file before parity wave 8a), the DXIL of UiVS and UiPS is compiled from
// both and compared byte for byte: the pipeline every UI draw used before must be unchanged. Skips (77) when the SPIR-V-capable dxcompiler.dll
// (third_party/dxc-spirv) is absent.
#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/DxcShaderInclude.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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

struct Variant {
    std::string name;
    std::string source;
    const char* entry;
    char stage;   // 'v' vertex, 'p' pixel
};

bool compile(IDxcCompiler3* compiler, IDxcUtils* utils, const Variant& v, bool spirv, std::string& err, std::vector<unsigned char>& bytes) {
    const wchar_t* prefix = v.stage == 'v' ? L"vs" : L"ps";
    const std::wstring target = std::wstring(prefix) + L"_6_0";
    const std::wstring entry(v.entry, v.entry + std::strlen(v.entry));
    std::vector<LPCWSTR> args = {L"-E", entry.c_str(), L"-T", target.c_str(), L"-Zpr", L"-HV", L"2021", L"-I", L"."};
    if (spirv) {
        args.push_back(L"-spirv"); args.push_back(L"-fspv-target-env=vulkan1.3");
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
    bytes.assign(static_cast<const unsigned char*>(obj->GetBufferPointer()), static_cast<const unsigned char*>(obj->GetBufferPointer()) + obj->GetBufferSize());
    return true;
}

// shaderFile() is a flat namespace: stage the shared and the UI shaders into one temp directory, optionally with a different ui.hlsl.
std::string stageShaders(const char* name, const fs::path& uiOverride) {
    const fs::path root = fs::path(AVER_REPO_ROOT) / "modules";
    const fs::path dst = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const char* mod : {"rhi", "render.ui"}) {
        for (const auto& e : fs::directory_iterator(root / mod / "shaders", ec)) {
            const std::string ext = e.path().extension().string();
            if (e.is_regular_file() && (ext == ".hlsl" || ext == ".hlsli"))
                fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }
    if (!uiOverride.empty()) fs::copy_file(uiOverride, dst / "ui.hlsl", fs::copy_options::overwrite_existing, ec);
    return dst.string();
}
} // namespace

int main() {
    AVER_INFO("=== UiShaderCompileTest ===");
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

    rhi::setShaderSourceDir(stageShaders("AverUiShaderCompileTest", {}));
    const std::string ui = rhi::shaderFile("ui.hlsl");
    const std::string world = rhi::shaderFile("world_text.hlsl");
    if (ui.empty() || world.empty()) {
        AVER_ERROR("  FAIL  the UI shaders were not found");
        return 1;
    }

    std::vector<Variant> variants = {
        {"ui UiVS", ui, "UiVS", 'v'},
        {"ui UiPS", ui, "UiPS", 'p'},
        {"ui UiPSMul", ui, "UiPSMul", 'p'},
        {"world_text WorldTextVS", world, "WorldTextVS", 'v'},
        {"world_text WorldTextPS", world, "WorldTextPS", 'p'},
    };

    int failures = 0;
    for (const Variant& v : variants) {
        for (const bool spirv : {true, false}) {
#if !defined(_WIN32)
            if (!spirv) continue;   // the DXIL half is Windows only (the D3D12 backend is)
#endif
            std::string err;
            std::vector<unsigned char> bytes;
            const std::string label = v.name + (spirv ? " (SPIR-V)" : " (DXIL)");
            if (compile(compiler.Get(), utils.Get(), v, spirv, err, bytes)) {
                AVER_INFO("  PASS  {} ({} bytes)", label, bytes.size());
            } else if (spirv && err.find("SPIR-V CodeGen not available") != std::string::npos) {
                AVER_WARN("  SKIP  dxcompiler.dll has no SPIR-V backend");
                return kSkip;
            } else {
                AVER_ERROR("  FAIL  {}: {}", label, err);
                ++failures;
            }
        }
    }

#if defined(_WIN32)
    if (const char* base = std::getenv("AVER_UI_SHADER_BASE")) {
        const fs::path older = fs::path(base) / "ui.hlsl";
        if (!fs::exists(older)) {
            AVER_ERROR("  FAIL  AVER_UI_SHADER_BASE names no ui.hlsl");
            ++failures;
        } else {
            rhi::setShaderSourceDir(stageShaders("AverUiShaderCompileTestBase", older));
            const std::string baseUi = rhi::shaderFile("ui.hlsl");
            for (const char* entry : {"UiVS", "UiPS"}) {
                const char stage = entry[2] == 'V' ? 'v' : 'p';
                std::string e1, e2;
                std::vector<unsigned char> a, b;
                const bool oa = compile(compiler.Get(), utils.Get(), {"new", ui, entry, stage}, false, e1, a);
                const bool ob = compile(compiler.Get(), utils.Get(), {"old", baseUi, entry, stage}, false, e2, b);
                if (oa && ob && a == b) AVER_INFO("  PASS  {} DXIL is byte-identical to the older ui.hlsl ({} bytes)", entry, a.size());
                else { AVER_ERROR("  FAIL  {} DXIL differs from the older ui.hlsl ({} vs {} bytes)", entry, a.size(), b.size()); ++failures; }
            }
        }
    }
#endif
    AVER_INFO("{} failure(s)", failures);
    return failures == 0 ? 0 : 1;
}
