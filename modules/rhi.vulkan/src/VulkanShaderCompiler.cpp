// VulkanShaderCompiler.cpp -- the DXC-with-`-spirv` wrapper declared in VulkanCommon.hpp section 8.
//
// This is the file the module's CMakeLists.txt has named since the backend was first written, and
// which had never actually been written: the target listed src/VulkanShaderCompiler.cpp (absent)
// and omitted src/VulkanPipeline.cpp (present), so AVER_RHI_VULKAN=ON could not even CONFIGURE.
// Both halves of that are fixed together -- see the CMakeLists.
//
// ================================================================================================
// WHAT THIS DOES, AND THE ONE THING IT CANNOT DO YET -- READ BEFORE TRUSTING A RENDERED FRAME.
// ================================================================================================
// It loads the same dxcompiler.dll the D3D12 backend loads (no second copy step -- see the
// CMakeLists) and asks it for SPIR-V instead of DXIL by adding `-spirv`. That part is complete and
// mirrors D3D12Device.cpp's own ShaderCompiler almost line for line, deliberately: two compilers
// that diverge in their argument list are two different languages wearing one name.
//
// WHAT IT CANNOT DO IS PUT DESCRIPTORS IN THE RIGHT SETS, and the reason is structural rather than
// an oversight in this file. Section 4 of VulkanCommon.hpp specifies four descriptor sets:
//
//     set 0 = PipelineLayout table 0   (t0..  at binding 0.., u0.. at binding kVkUavBindingBase..)
//     set 1 = PipelineLayout table 1   (restarted at binding 0 within its OWN set)
//     set 2 = the dynamic constant buffers
//     set 3 = immutable samplers
//
// DXC maps HLSL REGISTER SPACES onto Vulkan descriptor SETS -- `space1` becomes set 1 -- and the
// `-fvk-*-shift` arguments shift binding NUMBERS within one space. They cannot split one space
// across two sets. Every shader in this engine declares everything in the default space0
// (verified: no `space` annotation appears anywhere in RHIShaders.cpp or PbrShaders.cpp, and
// pbr::materialShaderDefines bases table 1's textures at t(srvCount) in that same space, which is
// precisely how D3D12's root signature wants them). So with these arguments every descriptor lands
// in set 0, and set 1 -- which descriptorLayout() builds and VulkanRenderContext binds -- receives
// nothing the shader will ever read.
//
// THE FIX IS NOT IN THIS FILE, which is why it is documented here instead of bodged here. Either
//   (a) the shared HLSL gains explicit register spaces, which is a cross-backend change because
//       D3D12 root signatures name spaces too, so both backends must move together; or
//   (b) compile() is given the PipelineLayout and emits one explicit `-fvk-bind-register` per
//       declared register, which needs a signature change to the interface VulkanCommon.hpp
//       section 8 declares and both of its callers use.
// (b) is the smaller change and keeps the shaders backend-neutral, which is the property the engine
// has otherwise held onto everywhere. Either way it is a deliberate decision, not a patch.
//
// So: this module now CONFIGURES, COMPILES and LINKS, and its shader compilation genuinely
// produces SPIR-V. It does not yet render a correct frame, and saying otherwise because the build
// went green is exactly the kind of unbacked claim this repository has been burned by before.
#include "VulkanCommon.hpp"

#include <cstdlib>
#include <cstdio>

// <unknwn.h> BEFORE <dxcapi.h>, and the order is load-bearing. dxcapi.h declares COM interfaces
// derived from IUnknown but does not itself pull in a header that defines it; the D3D12 backend
// never noticed because d3d12.h drags the whole Windows COM surface in ahead of it, and this
// module deliberately includes no D3D headers at all. Without this the build dies in ~20
// "'IUnknown': base class undefined" errors inside dxcapi.h that look like a broken SDK.
#include <unknwn.h>

#include <dxcapi.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

namespace aver::rhi::vkb {

namespace {
using Microsoft::WRL::ComPtr;

// Splits a semicolon-separated define list, identical in shape to ShaderDesc::defines and to the
// splitting D3D12Device.cpp's ShaderCompiler::compile does -- one behaviour, two backends.
std::vector<std::string> splitDefines(const char* defines) {
    std::vector<std::string> out;
    if (!defines || !*defines) return out;
    const std::string all(defines);
    for (size_t b = 0; b <= all.size();) {
        const size_t e = all.find(';', b) == std::string::npos ? all.size() : all.find(';', b);
        if (e > b) out.emplace_back(all, b, e - b);
        b = e + 1;
    }
    return out;
}
} // namespace

// Loads dxcompiler.dll once and resolves DxcCreateInstance.
//
// NO FXC FALLBACK, unlike the D3D12 backend: FXC emits DXBC, which is not SPIR-V and never will be.
// Section 8's own note says it -- a Vulkan device with no usable DXC has no shader path at all, and
// a failure here must read as "this device cannot compile shaders", not "one shader failed".
void VulkanShaderCompiler::init() {
    if (tried_) return;
    tried_ = true;

    dll_ = LoadLibraryW(L"dxcompiler.dll");
    if (!dll_) {
        AVER_WARN("[RHI.Vulkan] dxcompiler.dll not found -- this backend has NO shader compiler "
                  "(there is no FXC fallback: FXC emits DXBC, not SPIR-V)");
        return;
    }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(
        reinterpret_cast<void*>(GetProcAddress(dll_, "DxcCreateInstance")));
    if (!create) {
        AVER_WARN("[RHI.Vulkan] DxcCreateInstance missing from dxcompiler.dll -- no shader compiler");
        return;
    }

    IDxcUtils* utils = nullptr;
    IDxcCompiler3* compiler = nullptr;
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        if (utils) utils->Release();
        if (compiler) compiler->Release();
        AVER_WARN("[RHI.Vulkan] DXC init failed -- no shader compiler");
        return;
    }
    // Held as void* so VulkanCommon.hpp never has to include <dxcapi.h>; this is the one file that
    // does. Released in no destructor on purpose -- the instance is a function-local static with
    // process lifetime (see vulkanShaderCompiler below), so these outlive every caller and the
    // process reclaims them. Matching D3D12Device.cpp's ShaderCompiler, which does the same.
    utils_ = utils;
    compiler_ = compiler;
    AVER_INFO("[RHI.Vulkan] shader compiler: DXC (-spirv, shader model 6.x)");
}

bool VulkanShaderCompiler::usingDxc() const { return compiler_ != nullptr; }

// Compiles one HLSL entry point to SPIR-V. See this file's banner for what the binding arguments
// below do and do not achieve.
bool VulkanShaderCompiler::compile(const char* src, const char* entry, ShaderStage stage,
                                    u32 minShaderModel, const char* defines,
                                    std::vector<u32>& outSpirv,
                                    const VkRegisterBind* binds, u32 bindCount) {
    init();
    if (!usingDxc() || !src || !entry) return false;

    // "%s_%u_%u" from the stage prefix and the requested model, the same shape D3D12Device.cpp
    // builds. minShaderModel arrives as e.g. 65 for 6.5; 0 means "no opinion", so take 6.0.
    const u32 sm = minShaderModel ? minShaderModel : 60;
    const std::string target = std::string(dxcTargetPrefix(stage)) + "_" +
                                std::to_string(sm / 10) + "_" + std::to_string(sm % 10);

    const std::wstring wEntry(entry, entry + std::strlen(entry));
    const std::wstring wTarget(target.begin(), target.end());
    std::vector<std::wstring> wDefines;
    for (const std::string& d : splitDefines(defines)) wDefines.emplace_back(d.begin(), d.end());

    // -Zpr and -HV 2021 are NOT optional garnish: they are exactly what the D3D12 path passes, and
    // a matrix-order or language-version difference between the two backends compiling the SAME
    // source is a class of bug that shows up as geometry that is subtly wrong rather than as an
    // error. Both backends must read this HLSL identically.
    //
    // -fvk-u-shift kVkUavBindingBase 0 is the one shift that IS correct today and matters: section 4
    // puts a table's UAVs at binding kVkUavBindingBase+i so a growing srvCount never renumbers
    // them, and tableSetLayout() builds exactly that. Without this, u0 and t0 would both claim
    // binding 0 of set 0.
    const std::wstring uShift = std::to_wstring(kVkUavBindingBase);
    DxcBuffer buf{src, std::strlen(src), DXC_CP_UTF8};
    std::vector<LPCWSTR> args = {
        L"-E", wEntry.c_str(),
        L"-T", wTarget.c_str(),
        L"-Zpr",
        L"-HV", L"2021",
        L"-spirv",
        // Vulkan 1.3: dynamic rendering, buffer device address, mesh shaders -- everything this
        // backend's VulkanDevice already requires of the device.
        L"-fspv-target-env=vulkan1.3",
    };
    // THE SHIFT AND THE EXPLICIT MAP ARE MUTUALLY EXCLUSIVE -- DXC rejects the combination outright
    // ("-fvk-u-shift cannot be used together with -fvk-bind-register"), which is how this was found.
    // It is not a limitation to work around: a map names a set and binding for EVERY register it
    // covers, so a shift would have nothing left to shift. Callers that supply a map get no shifts;
    // callers that do not (the scene path) get the shift they have always had.
    if (!(binds && bindCount)) {
        args.push_back(L"-fvk-u-shift");
        args.push_back(uShift.c_str());
        args.push_back(L"0");
    }

    // EXPLICIT PER-REGISTER BINDINGS, when the caller supplied a map. Each becomes
    //     -fvk-bind-register <class><number> <space> <binding> <set>
    // which is the only DXC facility that can put one register in a set of its own choosing. The
    // strings must outlive `args`, hence the vector -- LPCWSTR is a borrowed pointer and a
    // temporary here would be freed before Compile() ever read it.
    std::vector<std::wstring> bindArgs;
    if (binds && bindCount) {
        bindArgs.reserve(static_cast<usize>(bindCount) * 4);
        for (u32 i = 0; i < bindCount; ++i) {
            const VkRegisterBind& b = binds[i];
            bindArgs.push_back(std::wstring(1, static_cast<wchar_t>(b.type)) + std::to_wstring(b.number));
            bindArgs.push_back(std::to_wstring(b.space));
            bindArgs.push_back(std::to_wstring(b.binding));
            bindArgs.push_back(std::to_wstring(b.set));
        }
        for (u32 i = 0; i < bindCount; ++i) {
            args.push_back(L"-fvk-bind-register");
            args.push_back(bindArgs[i * 4 + 0].c_str());
            args.push_back(bindArgs[i * 4 + 1].c_str());
            args.push_back(bindArgs[i * 4 + 2].c_str());
            args.push_back(bindArgs[i * 4 + 3].c_str());
        }
    }
    // -fvk-invert-y ONLY ON THE STAGES THAT WRITE SV_Position. Vulkan's NDC is y-down where D3D's is
    // y-up, and the engine's projection matrices are built for D3D -- so the flip has to happen
    // somewhere, and doing it in codegen keeps one set of matrices for both backends. DXC refuses
    // the flag anywhere else outright:
    //     error: -fvk-invert-y can only be used in VS/DS/GS/MS/Lib
    // which is why it cannot simply be passed on every compile. A pixel or compute shader has no
    // position to invert.
    switch (stage) {
        case ShaderStage::Vertex:
        case ShaderStage::Geometry:
        case ShaderStage::Mesh:
            args.push_back(L"-fvk-invert-y");
            break;
        default:
            break;   // Pixel, Compute, Amplification: nothing to invert
    }
    for (const std::wstring& d : wDefines) { args.push_back(L"-D"); args.push_back(d.c_str()); }

    auto* compiler = static_cast<IDxcCompiler3*>(compiler_);
    ComPtr<IDxcResult> result;
    HRESULT hr = compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                   IID_PPV_ARGS(&result));
    if (SUCCEEDED(hr) && result) result->GetStatus(&hr);
    if (FAILED(hr)) {
        ComPtr<IDxcBlobUtf8> errs;
        if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) &&
            errs && errs->GetStringLength())
            AVER_ERROR("[RHI.Vulkan] {} ({}): {}", entry, target, errs->GetStringPointer());
        else
            AVER_ERROR("[RHI.Vulkan] {} ({}): DXC failed with no diagnostic", entry, target);
        return false;
    }

    ComPtr<IDxcBlob> obj;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj ||
        obj->GetBufferSize() == 0) {
        AVER_ERROR("[RHI.Vulkan] {} ({}): DXC reported success but produced no SPIR-V", entry, target);
        return false;
    }
    // SPIR-V is a stream of 32-bit words by definition; a size that is not a multiple of 4 means
    // whatever came back is not SPIR-V, and handing it to vkCreateShaderModule would be undefined.
    const size_t bytes = obj->GetBufferSize();
    if (bytes % sizeof(u32) != 0) {
        AVER_ERROR("[RHI.Vulkan] {} ({}): DXC returned {} bytes, not a whole number of SPIR-V words",
                   entry, target, static_cast<u64>(bytes));
        return false;
    }
    // outSpirv is left untouched on every failure above, which section 8's declaration promises.
    outSpirv.resize(bytes / sizeof(u32));
    std::memcpy(outSpirv.data(), obj->GetBufferPointer(), bytes);

    // AVER_VK_DUMP_SPIRV=<dir> writes every compiled module out as <entry>.spv.
    //
    // THIS IS THE DIAGNOSTIC THE VALIDATION LAYERS WOULD OTHERWISE BE. Khronos no longer publishes
    // Windows validation-layer binaries -- only Android ones -- so on a machine without the LunarG
    // SDK there is no layer to tell you that a pipeline's shaders and its descriptor layout
    // disagree, and AMD's driver answers that disagreement by faulting rather than erroring. The
    // SPIR-V itself carries the answer: OpDecorate DescriptorSet / Binding on every resource says
    // exactly where the compiler put it, which is precisely what -fvk-bind-register is supposed to
    // control and therefore precisely what needs checking.
    //
    // OFF UNLESS THE VARIABLE IS SET, and reading an env var per compile is nothing next to
    // invoking DXC. It writes raw .spv words, so any SPIR-V tool -- or twenty lines of Python, since
    // OpDecorate is opcode 71 in a trivially-walkable stream -- can read it.
    if (const char* dumpDir = std::getenv("AVER_VK_DUMP_SPIRV")) {
        const std::string path = std::string(dumpDir) + "/" + entry + ".spv";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(outSpirv.data(), 1, bytes, f);
            std::fclose(f);
            AVER_TRACE("[RHI.Vulkan] dumped {} ({} bytes)", path, bytes);
        }
    }
    return true;
}

// The process-wide compiler, shared by VulkanDevice's fixed pipelines and
// VulkanResourceFactory::createShader -- mirrors D3D12Device.cpp's own shaderCompiler().
VulkanShaderCompiler& vulkanShaderCompiler() {
    static VulkanShaderCompiler c;
    return c;
}

} // namespace aver::rhi::vkb
