// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// The seam a Vulkan-native in-window UI toolkit plugs into -- the exact counterpart of
// modules/rhi.d3d12/include/aver/rhi/d3d12/UiBackend.hpp, and built because that header called for
// it by name: "A Vulkan ImGui backend, if one is ever built, gets its own IUiBackend-shaped seam
// inside modules/rhi.vulkan; nothing here."
//
// It exists for the same reason the D3D12 one does: Aver.RHI.Vulkan knows only this abstract shape
// and NOTHING about Dear ImGui -- no ImGui include, no ImGui symbol, anywhere in VulkanDevice.cpp.
// The concrete implementation lives in a separate module that only Sandbox links, so a game build
// does not get a UI toolkit compiled into it merely by linking the RHI.
//
// DELIBERATELY VULKAN-TYPED, NOT A GENERIC "UI ABSTRACTION LAYER", for the same reason its D3D12
// twin is D3D12-typed: a Vulkan UI toolkit needs the raw VkInstance/VkPhysicalDevice/VkDevice/queue
// and the formats this backend renders with, and pretending one interface could span both backends
// would buy nothing -- the two toolkits share no types at all.
//
// HOUSE RULE 3 IS WHY THIS HEADER IS SHAPED THE WAY IT IS. Nothing outside modules/rhi.vulkan/src
// may see a Vulkan type, and this header is public. It therefore carries the handles as OPAQUE
// pointer-sized values rather than VkDevice/VkQueue/etc., exactly the way uiTextureId already hands
// a texture across as a u64: the implementing module includes vulkan.h itself and casts. That keeps
// the RHI's public surface free of vulkan.h while still handing over everything a toolkit needs.
#pragma once
#include "aver/core/Types.hpp"

namespace aver::rhi { class IDevice; }

namespace aver::rhi::vkb {

// What a UI toolkit's Vulkan backend needs to stand itself up. Filled in by VulkanDevice::uiInit
// from state a caller holding only IDevice* cannot otherwise reach.
//
// Every handle is an OPAQUE u64 -- see this file's header comment on house rule 3. The implementing
// module reinterpret_casts each back to its real Vulkan type; they are pointer-sized dispatchable
// handles (instance/physical device/device/queue) or a 64-bit non-dispatchable handle (the pool),
// so a u64 is lossless for both on the 64-bit targets this engine builds for.
struct UiBackendInitDesc {
    u64 instance = 0;         // VkInstance
    u64 physicalDevice = 0;   // VkPhysicalDevice
    u64 device = 0;           // VkDevice
    u64 queue = 0;            // VkQueue -- the graphics queue this backend submits every frame on
    u32 queueFamily = 0;
    u64 descriptorPool = 0;   // VkDescriptorPool the toolkit may allocate its own sets from

    // How many frames this backend keeps in flight (kFrameCount), and how many swapchain images it
    // negotiated. A toolkit rings its own vertex/index buffers against the first of these.
    u32 frameCount = 0;
    u32 imageCount = 0;

    // The colour format the toolkit will be asked to draw into, as a VkFormat. This backend composes
    // the UI straight onto the swapchain image, so it is the swapchain format, NOT the HDR scene
    // one.
    u32 colorFormat = 0;      // VkFormat
    u32 sampleCount = 1;      // VkSampleCountFlagBits value; the overlay pass is always 1

    // TRUE, ALWAYS, and stated rather than assumed: this backend has no VkRenderPass to hand over.
    // It renders exclusively through VK_KHR_dynamic_rendering (core at the 1.3 it requires -- see
    // VulkanCommon.hpp section 1), so a toolkit built against a render-pass contract cannot be
    // plugged in here without one being invented for it.
    bool dynamicRendering = true;
};

// One Vulkan-native in-window UI toolkit. IDevice::uiInit/uiNewFrame/uiShutdown/uiActive/
// uiWantsMouse/uiWantsKeyboard all delegate here once installUiBackend (below) has plugged one in;
// with none installed -- the default, and the only state a game build ever reaches -- every one of
// those IDevice virtuals behaves exactly as it did before this seam existed.
//
// NON-OWNING FROM VulkanDevice'S SIDE, exactly like its D3D12 twin and like IDevice::setUpscaler's
// IUpscaler*: the caller (Sandbox) keeps the concrete object alive until AFTER the device's own
// uiShutdown() has run.
class IUiBackend {
public:
    virtual ~IUiBackend() = default;

    // Brings the toolkit up for `windowHandle` (an HWND on Windows). False on failure -- uiInit then
    // behaves exactly as if no backend had been installed at all.
    virtual bool init(void* windowHandle, const UiBackendInitDesc& desc) = 0;
    virtual void newFrame() = 0;
    virtual void shutdown() = 0;

    // Draws the toolkit's queued widgets into whatever colour attachment `commandBuffer` currently
    // has open. THE CALLER HAS ALREADY OPENED A DYNAMIC-RENDERING SCOPE over the backbuffer and set
    // the viewport and scissor to the whole of it -- this must not open its own, and must not close
    // the caller's. `commandBuffer` is a VkCommandBuffer as an opaque u64.
    virtual void render(u64 commandBuffer) = 0;

    virtual bool wantsMouse() const = 0;
    virtual bool wantsKeyboard() const = 0;

    // Forwards one raw Win32 message (see aver::rhi::registerUiWndProc). True if the toolkit
    // consumed it.
    virtual bool wndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam) = 0;

    // A descriptor the toolkit can draw one of THIS BACKEND'S textures through, as the u64
    // IDevice::uiTextureId hands out. Unlike D3D12 -- where every texture must live in the one
    // bound CBV_SRV_UAV heap, which is why its seam shares an allocator -- a Vulkan toolkit binds a
    // descriptor SET per texture and needs no shared pool, so this is a straight
    // "make me an ImTextureID for this image view + sampler" rather than an allocator pair.
    // Returns 0 when the toolkit cannot (not initialised, out of pool space).
    virtual u64 textureId(u64 imageView, u64 sampler) = 0;
    // Releases what textureId returned. Called when the texture behind it is destroyed.
    virtual void releaseTextureId(u64 id) = 0;
};

// Plugs `backend` into `device`'s UI slot, non-owning. False (a no-op) if `device` is not a Vulkan
// device -- checked via IDevice::backend(), the tag every backend already reports, since this header
// has no other way to ask "are you really the concrete type I expect" (no RTTI is used anywhere in
// this codebase). `backend` must outlive `device`'s uiShutdown() -- see IUiBackend's own comment.
bool installUiBackend(IDevice* device, IUiBackend* backend);

} // namespace aver::rhi::vkb
