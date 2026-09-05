#pragma once
#include "aver/rhi/ShaderFiles.hpp"

// <unknwn.h> BEFORE <dxcapi.h>, AND THE ORDER IS LOAD-BEARING -- the same trap
// VulkanShaderCompiler.cpp already documents at its own include of these two. dxcapi.h declares COM
// interfaces derived from IUnknown without pulling in a header that defines it. The D3D12 backend
// never noticed, because d3d12.h drags the whole Windows COM surface in ahead of it -- so a header
// like this one, included EARLY in that file, dies in ~40 errors inside dxcapi.h that read like a
// broken SDK. Included here rather than left to the includer, so this header stands on its own
// wherever it lands.
#include <unknwn.h>

#include <dxcapi.h>

#include <string>

// An #include handler for DXC, backed by rhi::shaderFile().
//
// WHY THIS EXISTS. Both backends called IDxcCompiler3::Compile with a NULL include handler, so an
// `#include` inside a .hlsl was a compile error and shader composition had to be C++ string
// concatenation instead: ShaderDesc::prelude, plus explicit std::string additions wherever more than
// one prelude was needed. That worked, and it cost two things worth getting back.
//
//   A migrated .hlsl was NOT SELF-CONTAINED. Its dependency on the shared prelude lived in a comment
//   ("compiled as the TAIL of rhi::sharedShaderPrelude()") rather than in the file, so it could not
//   be handed to dxc, an editor or a linter on its own -- which is the entire reason
//   tools/DumpClusterPs.cpp exists.
//
//   EVERY SHADER PAID FOR EVERY PRELUDE. shared_prelude.hlsl is 63 KB and material_prelude.hlsl is
//   68 KB, prepended whole to shaders as small as the 1.1 KB UI one -- about 98% of what DXC parses
//   there is text that shader never references, across dozens of pipeline creations. An #include
//   lets a shader ask for what it actually uses.
//
// NOT IDxcUtils::CreateDefaultIncludeHandler(), WHICH WOULD HAVE BEEN ONE LINE. That reads the
// filesystem directly, and this engine's shader loading is deliberately not a filesystem read: it is
// a process-wide cache, a --shader-source override searched ahead of the deployed copy, CRLF
// normalisation on load, and a revision counter that hot reload keys on. A default handler bypasses
// all four, so an #included file would ignore --shader-source, never reload, and arrive with CRLFs
// the rest of the pipeline has already normalised away. Routing through shaderFile() means an
// included file behaves exactly like a top-level one.
//
// HEADER-ONLY, AND IN Aver.RHI ON PURPOSE. It needs dxcapi.h, which the generic RHI does not
// otherwise want -- but nothing in Aver.RHI's own sources includes this file, so the library gains no
// DXC dependency. Only the two backends include it, both of which already load dxcompiler.dll and
// hold an IDxcUtils. Putting one copy here rather than one in each backend is the same reasoning
// FrameConstants.hpp records: this is a thing both backends need, and two hand-maintained copies of
// it is the defect, not the convenience.
namespace aver::rhi {

class DxcShaderInclude final : public IDxcIncludeHandler {
public:
    explicit DxcShaderInclude(IDxcUtils* utils) : utils_(utils) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR path, IDxcBlob** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!utils_ || !path) return E_FAIL;

        // DXC hands back a resolved path, which for a bare `#include "x.hlsl"` is "./x.hlsl". Only the
        // FILENAME is meaningful here: shaderFile() owns the search (a --shader-source directory, then
        // <exe>/shaders), so a directory prefix from the compiler would only get in its way.
        std::wstring w(path);
        const size_t slash = w.find_last_of(L"/\\");
        if (slash != std::wstring::npos) w = w.substr(slash + 1);
        std::string name(w.begin(), w.end());   // shader filenames are ASCII by construction

        const std::string& text = shaderFile(name);
        // E_FAIL, NOT AN EMPTY BLOB. shaderFile() answers a missing file with an empty string so the
        // caller can carry on; here that would compile an empty include and report the failure as
        // whatever declaration went missing three files away. Failing the load makes DXC say "cannot
        // open include file", which names the actual problem.
        if (text.empty()) return E_FAIL;

        IDxcBlobEncoding* blob = nullptr;
        const HRESULT hr = utils_->CreateBlob(text.data(), static_cast<UINT32>(text.size()),
                                              DXC_CP_UTF8, &blob);
        if (FAILED(hr)) return hr;
        *out = blob;   // ownership passes to DXC, which releases it
        return S_OK;
    }

    // A STACK OBJECT WITH NO REFERENCE COUNTING, deliberately. Every use of this handler lives
    // entirely inside one Compile() call, so its lifetime is the caller's frame and DXC's AddRef /
    // Release pairs cannot outlive it. Returning 1 from both is the documented shape for a
    // caller-owned handler; actually deleting on Release would free a stack object.
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** obj) noexcept override {
        if (!obj) return E_POINTER;
        if (iid == __uuidof(IDxcIncludeHandler) || iid == __uuidof(IUnknown)) {
            *obj = this;
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }

private:
    IDxcUtils* utils_ = nullptr;
};

} // namespace aver::rhi
