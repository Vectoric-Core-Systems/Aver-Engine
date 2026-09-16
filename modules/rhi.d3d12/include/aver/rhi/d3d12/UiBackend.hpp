// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// The seam a D3D12-native in-window UI toolkit plugs into. This is what makes the ImGui/RHI split
// possible: Aver.RHI.D3D12 (this module) knows only this abstract shape and NOTHING about Dear ImGui
// -- no ImGui include, no ImGui symbol, anywhere in D3D12Device.cpp. The concrete implementation
// lives in the separate Aver.RHI.D3D12.ImGui module (modules/rhi.d3d12.imgui), which only Sandbox
// links. Before this existed, Aver.RHI.D3D12 linked `imgui` PUBLIC (see this module's own
// CMakeLists.txt history), so AverGame.exe got Dear ImGui compiled into it whenever a tree was
// configured AVER_ENABLE_UI=ON, purely because it links Aver.RHI.D3D12 through Aver.Runtime -- see
// Runtime/CMakeLists.txt's own header comment for the defect this closes.
//
// DELIBERATELY D3D12-TYPED, NOT A GENERIC "UI ABSTRACTION LAYER". ImGui_ImplDX12_RenderDrawData
// needs a raw ID3D12GraphicsCommandList*, and ImGui_ImplDX12_Init needs an ID3D12Device*/
// ID3D12CommandQueue*/ID3D12DescriptorHeap* -- pretending this interface could mean anything on a
// backend without those types would buy nothing today: Vulkan has no ImGui integration to split in
// the first place (VulkanDevice.cpp never overrides IDevice::uiInit). A Vulkan ImGui backend, if one
// is ever built, gets its own IUiBackend-shaped seam inside modules/rhi.vulkan; nothing here.
#pragma once
#include "aver/core/Types.hpp"

#include <d3d12.h>
#include <dxgiformat.h>

namespace aver::rhi { class IDevice; }

namespace aver::rhi::d3d12 {

// What a UI toolkit's D3D12 backend needs to stand itself up. Filled in by D3D12Device::uiInit from
// state a caller holding only IDevice* cannot otherwise reach -- the raw ID3D12Device/CommandQueue,
// and the frame count / backbuffer format every other draw path in this backend already agrees on.
struct UiBackendInitDesc {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    u32 frameCount = 0;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
};

// One D3D12-native in-window UI toolkit. IDevice::uiInit/uiNewFrame/uiShutdown/uiActive/uiWantsMouse/
// uiWantsKeyboard/uiTextureId all delegate here once installUiBackend (below) has plugged one in;
// with none installed -- the default, and the only state a game build ever reaches -- every one of
// those IDevice virtuals behaves exactly as it always did for a backend with no UI support.
//
// NON-OWNING FROM D3D12Device's SIDE, exactly like IDevice::setUpscaler's IUpscaler* -- the caller
// (Sandbox) keeps the concrete object alive, and must keep it alive until AFTER the device's own
// uiShutdown() has run (D3D12Device's destructor calls it unconditionally). Sandbox holds it in a
// std::unique_ptr that is a member of the Application subclass, which is not destroyed until after
// Engine::run() -- and therefore the device -- has already returned; see SandboxApp.cpp's own comment
// on uiBackend_ for why that ordering needs no explicit shutdown-time detach, unlike
// aver::sr::SpatialUpscaler's.
class IUiBackend {
public:
    virtual ~IUiBackend() = default;

    // Brings the toolkit up for `windowHandle` (HWND). False on failure -- uiInit then behaves
    // exactly as if no backend had been installed at all.
    virtual bool init(void* windowHandle, const UiBackendInitDesc& desc) = 0;
    virtual void newFrame() = 0;
    virtual void shutdown() = 0;

    // Draws the toolkit's queued widgets into whatever render target `cmdList` currently has bound
    // (the caller has already bound it and set the viewport/scissor to the full backbuffer). Returns
    // the descriptor heap the toolkit just bound on `cmdList`, so the caller's own heap-binding cache
    // (see D3D12Device::boundHeap_'s own comment) stays correct for whatever draws next.
    virtual ID3D12DescriptorHeap* render(ID3D12GraphicsCommandList* cmdList) = 0;

    virtual bool wantsMouse() const = 0;
    virtual bool wantsKeyboard() const = 0;

    // Forwards one raw Win32 message (see aver::rhi::registerUiWndProc). True if the toolkit
    // consumed it.
    virtual bool wndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam) = 0;

    // The toolkit's OWN dynamically-created textures (a font atlas today; ImGui 1.92's texture-update
    // API can create more later) must land in the SAME descriptor heap uiTextureId()'s textures do --
    // a D3D12 command list can only have one CBV_SRV_UAV heap bound at a time.
    // D3D12ResourceFactory::destroyTexture/uiDescriptor call these to share the toolkit's own pool for
    // exactly that reason, rather than this module growing a second, competing heap. False/no-op when
    // nothing is installed or initialised, exactly like uiTextureId's own defaults.
    virtual bool allocTextureSrv(u64* outCpu, u64* outGpu) = 0;
    virtual void freeTextureSrv(u64 cpu) = 0;
};

// Plugs `backend` into `device`'s UI slot, non-owning. False (a no-op) if `device` is not a D3D12
// device -- checked via IDevice::backend(), the tag every backend already reports, since this header
// has no other way to ask "are you really the concrete type I expect" (no RTTI is used anywhere in
// this codebase). `backend` must outlive `device`'s uiShutdown() call -- see IUiBackend's own comment
// on ownership.
bool installUiBackend(IDevice* device, IUiBackend* backend);

} // namespace aver::rhi::d3d12
