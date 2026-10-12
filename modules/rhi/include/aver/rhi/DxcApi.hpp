#pragma once
// <dxcapi.h> on every platform, and the ComPtr every DXC caller in this tree uses with it.
//
// ON WINDOWS this is exactly what each caller used to write for itself: <unknwn.h> first (dxcapi.h
// derives from IUnknown without defining it -- see DxcShaderInclude.hpp for the ~40 errors that
// order prevents), the Windows SDK's <dxcapi.h>, and WRL's ComPtr.
//
// ELSEWHERE there is no SDK and no WRL. DXC's own dxcapi.h is vendored in
// third_party/dxc-spirv/include/dxc (Aver.RHI puts it on the include path) and pulls in WinAdapter.h,
// which supplies IUnknown, HRESULT, LPCWSTR, __uuidof and IID_PPV_ARGS. ComPtr is the one piece it
// does not, so the part of WRL's interface these callers use is spelled out below.
//
// LPCWSTR is wchar_t on both, 16-bit on Windows and 32-bit elsewhere; DXC takes its arguments in the
// platform's own wchar_t, so argument-building code is the same text on both.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <wrl/client.h>
#else
#include <dxcapi.h>
#endif

#include <cstddef>

namespace aver::rhi {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#else
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(std::nullptr_t) const { return p_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return p_ != nullptr; }
    // For IID_PPV_ARGS(&p): an out-parameter, so whatever was held is released first, as WRL's does.
    T** operator&() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
private:
    T* p_ = nullptr;
};
#endif

} // namespace aver::rhi
