// Internal shared header for the Vulkan RHI backend. Included ONLY by .cpp files inside
// modules/rhi.vulkan/src/ -- this module has no include/ directory (aver_add_module only exposes
// a module's include/ PUBLICly), so nothing outside this directory can see a VkFoo. That is house
// rule 3 (the RHI must not leak backend types), matching D3D12 (d3d12.h appears nowhere outside
// modules/rhi.d3d12).
//
// D3D12Device.cpp is one 5,423-line TU with every backend object in anonymous-namespace scope;
// this backend is FOUR TUs, so anything two share needs EXTERNAL linkage -- why this header
// exists. Everything below lives in aver::rhi::vkb, not aver::rhi, so a same-named D3D12
// anonymous-namespace type (RhiTexture, hrOk, ...) is never a collision risk.
//
// FILE MAP -- which of the four .cpp files defines what. Every declaration below names its one
// owner; nothing here should ever be defined in two of them or in none.
//
//   VulkanDevice.cpp
//     - VulkanSwapchain (ISwapchain), all methods.
//     - VulkanDevice (IDevice), every override, plus: init(), queryCaps(),
//       initAccelerationStructures(), initMeshShaders(), dispatchMesh(), createPipeline() (fixed
//       scene/wire/sky/line PSOs only), createSwapchainResources/createRenderTargetViews/
//       createDepthBuffer/createMsaaColor, waitForGpu/waitTimeline, present/resize,
//       notifyRenderTargetsChanged, ensureViewportTexture, seedSkinTargets, packAtmosphere,
//       toSceneReferred, the camera post chain (createPostPipelines/createPostTargets/
//       releasePostTargets/runPostChain/postConstants), debugMessengerCallback(),
//       loadGlobalApi/loadInstanceApi/loadDeviceApi/unloadApi (this is the first file that ever
//       needs an instance/device to resolve Vulkan API function pointers against; everywhere else
//       reaches the filled table via VulkanDevice::api()), and
//       aver::rhi::detail::createVulkanDevice() itself, matching the forward declaration in
//       modules/rhi/src/RHI.cpp exactly.
//
//   VulkanShaderCompiler.cpp
//     - VulkanShaderCompiler (init/usingDxc/compile): the DXC `-spirv` wrapper, its own TU
//       because both VulkanDevice.cpp and VulkanResourceFactory.cpp need it.
//     - vulkanShaderCompiler(), the process-wide singleton accessor.
//
//   VulkanResourceFactory.cpp
//     - VulkanResourceFactory (IResourceFactory), every override plus its private helpers: the
//       descriptor/pipeline-layout cache (descriptorLayout(), tableSetLayout()), the sampler
//       cache (getOrCreateSampler()), pushConstantLayout(), nullFill(), uploadInitialData(), and
//       the deferred-destruction machinery (retireFence/retire/collect).
//     - createBufferCommitted/createImageCommitted/destroyBufferCommitted/destroyImageCommitted:
//       free functions (D3D12's CreateCommittedResource analog), defined here since this file
//       already owns allocation policy and every other raw buffer/image caller shares it.
//
//   VulkanRenderContext.cpp
//     - VulkanRenderContext (IRenderContext) — every override, plus ringAlloc(),
//       bindDeclaredDescriptors(), applyDrawBinding(), cmd().
//
// Every free INLINE helper below (format/state/filter/compare/vertex-layout conversions, vkOk,
// findMemoryType, sameLayout/sameSampler, storeDrawBinding, setVkObjectName) lives here rather
// than in one .cpp: each is a pure function of its arguments, the multi-TU equivalent of
// D3D12Device.cpp's anonymous-namespace free functions of the same shape.
#pragma once

// ---------------------------------------------------------------------------------------------
// Platform / loader configuration; also set as PRIVATE compile definitions in this module's
// CMakeLists.txt (the #ifndef guards just make this header self-sufficient). VK_NO_PROTOTYPES
// matters: there is no vulkan-1.lib on this machine (SDK deliberately not installed -- see
// third_party/vulkan-headers/README.md), so nothing here may call an unprefixed vkFoo() and
// expect the linker to resolve it. Every entry point is a function pointer in VulkanApi below,
// resolved at RUNTIME from C:\Windows\System32\vulkan-1.dll (ships with the GPU driver, not the
// SDK) via LoadLibraryW + GetProcAddress, starting from vkGetInstanceProcAddr itself.
// ---------------------------------------------------------------------------------------------
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>   // vendored at third_party/vulkan-headers/include, v1.3.296, Apache-2.0

#include "aver/rhi/RHI.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "VulkanRegisterMap.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <deque>      // pipelines_ -- see its declaration for why it is not a vector
#include <functional>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

static_assert(VK_HEADER_VERSION == 296,
              "VulkanCommon.hpp was written against the vendored Vulkan-Headers v1.3.296; if the "
              "vendored tree was updated, re-check every struct/enum name this file names before "
              "trusting this assert alone.");

namespace aver::rhi::vkb {

// ================================================================================================
// 1. Fixed constants
// ================================================================================================

// Frames in flight, and — since this backend requests exactly this many swapchain images — also
// the backbuffer count. Mirrors D3D12Device.cpp's kFrameCount.
constexpr u32 kFrameCount = 2;
constexpr u32 kDefaultSampleCount = 4;

// The SCENE colour/depth formats IDevice::backbufferFormat()/depthFormat() report — NOT the real
// swapchain surface format, which is negotiated separately at swapchain creation (see
// VulkanDevice::swapchainFormat_) and never handed to a feature pipeline. Mirrors D3D12's
// kSceneColorFormat/kDepthFormat exactly in spirit: fixed for the process lifetime, because every
// feature pipeline is built against whatever these report.
constexpr VkFormat kVkSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kVkDepthFormat      = VK_FORMAT_D32_SFLOAT;

// This backend's minimum requirement, requested as VkApplicationInfo::apiVersion; failure here
// makes VulkanDevice::init() return false and createVulkanDevice() report unavailable, like any
// other backend that fails to initialise. Chosen because synchronization2, dynamic_rendering,
// buffer_device_address and timeline_semaphore -- all structurally required by this backend's
// frame-pacing and render-target model -- are CORE at 1.3, with no extension-based fallback for a
// 1.2-only driver carried until a real 1.2-only machine needs it (it would need
// VK_KHR_synchronization2 and VK_KHR_dynamic_rendering).
constexpr u32 kRequiredApiVersion = VK_API_VERSION_1_3;

// ---- required / optional extension name lists -------------------------------------------------
inline constexpr const char* kRequiredInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
};
// Requested only when DeviceDesc::enableDebug; absence is not fatal (see the vendored README: no
// validation layers by default, and VK_EXT_debug_utils itself may be missing on a non-SDK system
// too, in which case debug object naming / labels become silent no-ops, same shape as
// pushMarker/popMarker's own already-inert IRenderContext defaults).
inline constexpr const char* kOptionalInstanceExtensions[] = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
};
inline constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};
// Each optional extension gates exactly one DeviceCaps bit (or one internal support flag) HONESTLY
// to false/0 when absent — see DeviceCaps's own field comments and RHI.hpp:92's CapsOverride
// contract ("every field only ever reduces"). Requested if present; a backend that requested these
// and failed device creation when they were missing would be doing the opposite of what the task's
// ground truth asks for.
//
// THE ONE EXCEPTION IS VK_EXT_MEMORY_BUDGET at the tail: it gates no DeviceCaps bit at all, only
// VulkanDevice::videoMemory() (M6) -- there is no per-adapter capability this extension's absence
// should ever refuse a FEATURE over, only a reporting call that already has an honest
// unsupported/all-zero answer (VideoMemoryInfo::supported == false). It is requested here rather
// than through a second extension-enumeration pass for the identical reason every other entry in
// this list is: one enumerate, one enable list, one place that decides what is actually on the
// device.
inline constexpr const char* kOptionalDeviceExtensions[] = {
    VK_EXT_MESH_SHADER_EXTENSION_NAME,                  // -> DeviceCaps::meshShaderTier
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,       // -> DeviceCaps::rayTracingTier
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,     // dependency of acceleration_structure
    VK_KHR_RAY_QUERY_EXTENSION_NAME,                    // -> DeviceCaps::rayTracingTier (inline
                                                         //    RayQuery only: D3D12 here only ever
                                                         //    builds DXR 1.1 inline queries, so
                                                         //    VK_KHR_ray_tracing_pipeline isn't
                                                         //    needed)
    VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,   // -> DeviceCaps::conservativeRaster
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,                // -> VulkanDevice::videoMemory() only (M6);
                                                         //    gates no DeviceCaps bit -- see above
};

// ================================================================================================
// 2. The Vulkan API: every entry point this backend calls, as a function-pointer table. No
//    prototype in this table is ever linked; every one is resolved at runtime. See loadGlobalApi/
//    loadInstanceApi/loadDeviceApi below (declared here, DEFINED IN VulkanDevice.cpp).
// ================================================================================================
struct VulkanApi {
    HMODULE module = nullptr;   // the vulkan-1.dll module handle; FreeLibrary'd in unloadApi()

    // ---- loader-level ----
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;

    // ---- global-level: GetInstanceProcAddr(nullptr, name) ----
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;   // absent pre-1.1; treat as 1.0

    // ---- instance-level: GetInstanceProcAddr(instance_, name) ----
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    // Core 1.1, instance-level, resolved beside GetPhysicalDeviceProperties2/GetPhysicalDeviceFeatures2
    // above -- MAY BE NULL on a driver that somehow reports 1.1 support and does not export it, which
    // is why VulkanDevice::videoMemory() checks this pointer itself rather than trusting
    // memoryBudgetExt_ alone. Chains VkPhysicalDeviceMemoryBudgetPropertiesEXT (M6) to read the live
    // budget/usage per heap; plain GetPhysicalDeviceMemoryProperties above stays the source for heap
    // COUNT/FLAGS/SIZE, which do not change at runtime and are already cached in memoryProps_.
    PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT = nullptr;     // optional
    PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT = nullptr;   // optional

    // ---- device-level: GetDeviceProcAddr(device_, name) ----
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueSubmit2 QueueSubmit2 = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkGetFenceStatus GetFenceStatus = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;               // timeline
    PFN_vkSignalSemaphore SignalSemaphore = nullptr;             // timeline
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkResetCommandPool ResetCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2 = nullptr;
    PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
    PFN_vkCmdFillBuffer CmdFillBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering CmdEndRendering = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
    PFN_vkCmdDrawMeshTasksEXT CmdDrawMeshTasksEXT = nullptr;     // optional: mesh_shader
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets FreeDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT CmdBeginDebugUtilsLabelEXT = nullptr;   // optional
    PFN_vkCmdEndDebugUtilsLabelEXT CmdEndDebugUtilsLabelEXT = nullptr;       // optional
    PFN_vkSetDebugUtilsObjectNameEXT SetDebugUtilsObjectNameEXT = nullptr;   // optional

    // ---- acceleration structures (KHR_acceleration_structure + KHR_deferred_host_operations); all
    // optional, gated on kOptionalDeviceExtensions actually being present ----
    PFN_vkGetAccelerationStructureBuildSizesKHR GetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCreateAccelerationStructureKHR CreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR DestroyAccelerationStructureKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR CmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddressKHR = nullptr;
};

// Loads vulkan-1.dll and resolves vkGetInstanceProcAddr plus every global-level entry point.
// False (and logged) if the DLL or vkGetInstanceProcAddr itself is missing — the ONE failure this
// whole backend cannot recover from, since nothing else can be resolved without it.
bool loadGlobalApi(VulkanApi& api);
// Resolves every instance-level entry point, INCLUDING the optional debug_utils ones when
// `debugUtilsAvailable` says the instance was created with the extension. Never fails outright:
// an optional function simply stays nullptr, exactly like every other honestly-absent capability
// in this backend.
bool loadInstanceApi(VulkanApi& api, VkInstance instance, bool debugUtilsAvailable);
// Resolves every device-level entry point, gating the optional groups (mesh_shader,
// acceleration_structure) on whichever extensions `deviceExtensions` says were actually enabled.
bool loadDeviceApi(VulkanApi& api, VkDevice device, const std::vector<std::string>& deviceExtensions);
// Frees the module. Every PFN_ member is left dangling by design — nothing may call through `api`
// again after this, which is why it is only ever called from VulkanDevice's destructor tail.
void unloadApi(VulkanApi& api);

// ================================================================================================
// 3. Small pure helpers — format/state/filter/compare/vertex-layout conversions, VkResult
//    checking, memory-type selection. Fully defined here (inline): each is a pure function of its
//    arguments, so there is no ownership question to assign to one .cpp — see the file banner.
// ================================================================================================

// Logs and returns false on a failed VkResult. The Vulkan analog of D3D12Device.cpp's hrOk.
inline bool vkOk(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        AVER_ERROR("[RHI.Vulkan] {} failed (VkResult={})", what, static_cast<i32>(r));
        return false;
    }
    return true;
}

inline VkFormat toVkFormat(Format f) {
    switch (f) {
        case Format::RGBA8Unorm:     return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8UnormSrgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::RG8Unorm:       return VK_FORMAT_R8G8_UNORM;
        case Format::R8Unorm:        return VK_FORMAT_R8_UNORM;
        case Format::RGBA16F:        return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32Float:       return VK_FORMAT_R32_SFLOAT;
        case Format::RG32Float:      return VK_FORMAT_R32G32_SFLOAT;
        case Format::R32Uint:        return VK_FORMAT_R32_UINT;
        case Format::D32Float:       return VK_FORMAT_D32_SFLOAT;
        // Vulkan images keep ONE fixed format for their whole life — there is no DXGI-style
        // typeless reinterpretation. R32Typeless is created and viewed as D32Float for the depth
        // attachment; a SEPARATE view of the SAME image (aspect-qualified, not format-qualified)
        // is what a shader samples it through. See toVkAttachmentFormat/toVkSampledFormat below,
        // and the architecture scout's finding #6 for why this is simpler than the D3D12 trick,
        // not harder.
        case Format::R32Typeless:    return VK_FORMAT_D32_SFLOAT;
        // THE G-BUFFER'S TWO FORMATS WERE MISSING FROM THIS TABLE, not from the enum: RG16F and
        // RGB10A2Unorm have been in Format since the G-buffer landed, and D3D12 maps both, but this
        // switch never gained them. It has no default and MSVC does not warn on an unhandled
        // enumerator at /W4, so the omission was silent -- toVkFormat returned VK_FORMAT_UNDEFINED
        // and any Vulkan attempt to create the velocity or normal target would have failed at the
        // create call with no hint of why. Added here with NRD's two because it is the same table.
        case Format::RG16F:          return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGB10A2Unorm:   return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case Format::R16Unorm:       return VK_FORMAT_R16_UNORM;
        case Format::R16F:           return VK_FORMAT_R16_SFLOAT;
        case Format::R8Uint:         return VK_FORMAT_R8_UINT;
        case Format::R16Uint:        return VK_FORMAT_R16_UINT;
        case Format::BC1Unorm:       return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Format::BC1UnormSrgb:   return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case Format::BC3Unorm:       return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC3UnormSrgb:   return VK_FORMAT_BC3_SRGB_BLOCK;
        case Format::BC5Unorm:       return VK_FORMAT_BC5_UNORM_BLOCK;
        case Format::BC7Unorm:       return VK_FORMAT_BC7_UNORM_BLOCK;
        case Format::BC7UnormSrgb:   return VK_FORMAT_BC7_SRGB_BLOCK;
        case Format::Unknown:        break;
    }
    return VK_FORMAT_UNDEFINED;
}
inline Format fromVkFormat(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM:        return Format::RGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:         return Format::RGBA8UnormSrgb;
        case VK_FORMAT_R8G8_UNORM:            return Format::RG8Unorm;
        case VK_FORMAT_R8_UNORM:              return Format::R8Unorm;
        case VK_FORMAT_R16G16B16A16_SFLOAT:   return Format::RGBA16F;
        case VK_FORMAT_R32_SFLOAT:            return Format::R32Float;
        case VK_FORMAT_R32G32_SFLOAT:         return Format::RG32Float;
        case VK_FORMAT_R32_UINT:              return Format::R32Uint;
        case VK_FORMAT_D32_SFLOAT:            return Format::D32Float;   // see toVkFormat's note; R32Typeless round-trips as D32Float, which is the RIGHT answer wherever this is asked of a depth image
        case VK_FORMAT_R16G16_SFLOAT:         return Format::RG16F;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Format::RGB10A2Unorm;
        case VK_FORMAT_R16_UNORM:             return Format::R16Unorm;
        case VK_FORMAT_R16_UINT:              return Format::R16Uint;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:  return Format::BC1Unorm;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:   return Format::BC1UnormSrgb;
        case VK_FORMAT_BC3_UNORM_BLOCK:       return Format::BC3Unorm;
        case VK_FORMAT_BC3_SRGB_BLOCK:        return Format::BC3UnormSrgb;
        case VK_FORMAT_BC5_UNORM_BLOCK:       return Format::BC5Unorm;
        case VK_FORMAT_BC7_UNORM_BLOCK:       return Format::BC7Unorm;
        case VK_FORMAT_BC7_SRGB_BLOCK:        return Format::BC7UnormSrgb;
        default:                              return Format::Unknown;
    }
}
inline bool isDepthFormatVk(Format f) { return f == Format::D32Float || f == Format::R32Typeless; }
// The format a depth-created (R32Typeless/D32Float) image is SAMPLED through: a separate
// VkImageView of the SAME VkImage, VK_IMAGE_ASPECT_DEPTH_BIT, same VkFormat (VK_FORMAT_D32_SFLOAT)
// — unlike D3D12 there is no format change between the two views, only the aspect mask and layout.
inline VkImageAspectFlags toVkAspect(Format f) {
    return isDepthFormatVk(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

inline u32 texelBytesVk(Format f) {
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
        case Format::R16Uint:        return 2;
        case Format::R8Unorm:        return 1;
        default:                     break;
    }
    return 0;
}
inline u32 blockBytesVk(Format f) {
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
inline u64 packedRowPitchVk(Format f, u32 widthTexels) {
    if (const u32 bb = blockBytesVk(f)) return u64((widthTexels + 3) / 4) * bb;
    return u64(widthTexels) * texelBytesVk(f);
}

inline VkCompareOp toVkCompareOp(CompareOp op) {
    switch (op) {
        case CompareOp::Less:      return VK_COMPARE_OP_LESS;
        case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Always:    return VK_COMPARE_OP_ALWAYS;
        case CompareOp::Never:     break;
    }
    return VK_COMPARE_OP_NEVER;
}
inline VkFilter toVkFilter(Filter f) {
    return (f == Filter::Point) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}
inline VkSamplerMipmapMode toVkMipmapMode(Filter f) {
    return (f == Filter::Point) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}
inline VkSamplerAddressMode toVkAddressMode(AddressMode a) {
    return a == AddressMode::Wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}
inline VkSampleCountFlagBits toVkSampleCount(u32 n) {
    switch (n) {
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        default: break;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

// ---- ResourceState -> Vulkan barrier arguments -------------------------------------------------
// Unlike D3D12_RESOURCE_STATES, Vulkan splits this into TWO axes: an image's VkImageLayout (a real
// state; buffers have none), and VkAccessFlags2/VkPipelineStageFlags2 that the caller states
// explicitly on every barrier regardless of whether it targets an image or a buffer. Built against
// VK_KHR_synchronization2's VkImageMemoryBarrier2/VkBufferMemoryBarrier2 (core at 1.3, so no
// separate extension check beyond kRequiredApiVersion).
struct VkImageBarrierInfo {
    VkImageLayout layout;
    VkAccessFlags2 access;
    VkPipelineStageFlags2 stage;
};
inline VkImageBarrierInfo toVkImageBarrierInfo(ResourceState s) {
    switch (s) {
        case ResourceState::ShaderResource:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};
        case ResourceState::NonPixelShaderResource:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT};
        case ResourceState::UnorderedAccess:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT};
        case ResourceState::RenderTarget:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
        case ResourceState::DepthWrite:
            return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT};
        case ResourceState::CopySource:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT};
        case ResourceState::CopyDest:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT};
        // VertexBuffer/GeometryRead: buffer-only states in this engine's actual usage (D3D12's own
        // VertexBuffer/GeometryRead map to VERTEX_AND_CONSTANT_BUFFER, a BUFFER state) — an image
        // should never be asked for either, but a defined fallback beats undefined behaviour.
        case ResourceState::VertexBuffer:
        case ResourceState::GeometryRead:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        // TERMINAL, per RHIResources.hpp:89 — never a valid barrier argument in either direction.
        // Returning GENERAL/NONE here is a defined-but-unusable fallback; the caller-side contract
        // (VulkanRenderContext::textureBarrier) must reject this before ever reaching the barrier
        // call, exactly as the D3D12 debug-shadow's rejectAsState() does.
        case ResourceState::AccelerationStructure:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
        case ResourceState::Common:
            break;
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
}
struct VkBufferBarrierInfo {
    VkAccessFlags2 access;
    VkPipelineStageFlags2 stage;
};
inline VkBufferBarrierInfo toVkBufferBarrierInfo(ResourceState s) {
    switch (s) {
        case ResourceState::ShaderResource:
            return {VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};
        case ResourceState::NonPixelShaderResource:
            return {VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT};
        case ResourceState::UnorderedAccess:
            return {VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT};
        case ResourceState::CopySource:
            return {VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT};
        case ResourceState::CopyDest:
            return {VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT};
        case ResourceState::VertexBuffer:
            return {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        // Every geometry reader at once — the input assembler, manual vertex fetch, and an
        // acceleration-structure build. Mirrors D3D12's combined VERTEX_AND_CONSTANT_BUFFER |
        // NON_PIXEL_SHADER_RESOURCE for the identical reason (RHIResources.hpp:80-87).
        case ResourceState::GeometryRead:
            return {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR};
        case ResourceState::RenderTarget:
        case ResourceState::DepthWrite:
            break;   // not meaningful for a buffer; falls through to the Common/undefined default
        // TERMINAL — see toVkImageBarrierInfo's identical note.
        case ResourceState::AccelerationStructure:
            return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
        case ResourceState::Common:
            break;
    }
    return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
}

// First memory type in `memProps` matching every bit of `typeBits` whose propertyFlags contain
// EVERY bit of `required`. ~0u ("not found") on failure — the caller must treat that as the same
// class of failure as a null CreateCommittedResource, not dereference it into an index.
inline u32 findMemoryType(const VkPhysicalDeviceMemoryProperties& memProps, u32 typeBits,
                          VkMemoryPropertyFlags required) {
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return ~0u;
}

// ---- vertex input: POSITIONAL locations, not semantic names ------------------------------------
// SPIR-V vertex-stage inputs carry only a numeric Location decoration; DXC's default HLSL->SPIR-V
// lowering assigns Location by INPUT-STRUCT DECLARATION ORDER. The shared prelude's VSIn (pos,
// nrm, uv0) already agrees with kMeshInputLayout's D3D12 semantic order, so binding by position
// 0/1/2 needs no [[vk::location(N)]] annotation. Re-verify against an actual DXC -spirv
// disassembly before trusting this in a real build -- it is read from source, not from a compile.
inline void meshVertexInputState(VkVertexInputBindingDescription& outBinding,
                                 VkVertexInputAttributeDescription (&outAttribs)[3]) {
    outBinding = VkVertexInputBindingDescription{0, static_cast<u32>(sizeof(MeshVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    outAttribs[0] = VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(MeshVertex, px))};
    outAttribs[1] = VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(MeshVertex, nx))};
    outAttribs[2] = VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT,    static_cast<u32>(offsetof(MeshVertex, u))};
}
inline void lineVertexInputState(VkVertexInputBindingDescription& outBinding,
                                 VkVertexInputAttributeDescription (&outAttribs)[2]) {
    outBinding = VkVertexInputBindingDescription{0, static_cast<u32>(sizeof(LineVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    outAttribs[0] = VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(LineVertex, px))};
    outAttribs[1] = VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(LineVertex, r))};
}
// Caller-owned VertexLayout -> Vulkan attributes at positional locations 0..attribCount-1, in
// DECLARATION order — VertexSemantic/semanticIndex name nothing in SPIR-V; only array position
// does. Returns the attribute count actually written (an unusable Format is skipped and logged,
// exactly like D3D12's buildInputLayout).
inline u32 buildVertexInputAttributes(const VertexLayout& l,
                                      VkVertexInputAttributeDescription (&out)[kMaxVertexAttribs]) {
    u32 n = 0;
    for (u32 i = 0; i < l.attribCount && i < kMaxVertexAttribs; ++i) {
        const VkFormat f = toVkFormat(l.attribs[i].format);
        if (f == VK_FORMAT_UNDEFINED) {
            AVER_ERROR("[RHI.Vulkan] vertex attribute {} has no usable format", i);
            continue;
        }
        out[n] = VkVertexInputAttributeDescription{n, 0, f, l.attribs[i].offset};
        ++n;
    }
    return n;
}

// True when two pipeline layouts declare identical bindings/constants/samplers — the Vulkan-side
// twin of D3D12Device.cpp's sameLayout/sameSampler, used by the descriptor/pipeline-layout cache.
inline bool sameSampler(const SamplerDesc& a, const SamplerDesc& b) {
    return a.filter == b.filter && a.address == b.address && a.compare == b.compare && a.maxLod == b.maxLod;
}
inline bool sameLayout(const PipelineLayout& a, const PipelineLayout& b) {
    if (a.srvCount != b.srvCount || a.uavCount != b.uavCount || a.samplerCount != b.samplerCount) return false;
    if (a.srvCount1 != b.srvCount1 || a.uavCount1 != b.uavCount1) return false;
    // Part of the key even though this backend REFUSES a non-zero space (see descriptorLayout).
    // A refusal that is not in the cache key is worse than no refusal at all: the first layout
    // through would cache an entry, and a later layout differing only in its space would find that
    // entry, skip the refusal, and be bound at the wrong descriptor set with no diagnostic.
    if (a.constantSpace != b.constantSpace || a.samplerSpace != b.samplerSpace) return false;
    for (u32 i = 0; i < kMaxConstantSlots; ++i) if (a.constantDwords[i] != b.constantDwords[i]) return false;
    for (u32 i = 0; i < a.samplerCount && i < 4; ++i) if (!sameSampler(a.samplers[i], b.samplers[i])) return false;
    return true;
}
inline bool sameTableShape(u32 srvA, u32 uavA, const SlotKind* srvKindsA, const SlotKind* uavKindsA,
                           u32 srvB, u32 uavB, const SlotKind* srvKindsB, const SlotKind* uavKindsB) {
    if (srvA != srvB || uavA != uavB) return false;
    for (u32 i = 0; i < srvA; ++i) if (srvKindsA[i] != srvKindsB[i]) return false;
    for (u32 i = 0; i < uavA; ++i) if (uavKindsA[i] != uavKindsB[i]) return false;
    return true;
}

// Sets a debug object name via VK_EXT_debug_utils when the instance loaded it; a silent no-op
// otherwise, same shape as IRenderContext::pushMarker/popMarker's own already-inert defaults.
inline void setVkObjectName(const VulkanApi& api, VkDevice device, VkObjectType type, u64 handle, const char* name) {
    if (!name || !api.SetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    api.SetDebugUtilsObjectNameEXT(device, &info);
}

// ================================================================================================
// 4. Descriptor-set / push-constant scheme. Every declared PipelineLayout slot lands in EXACTLY
//    one of these two mechanisms -- the single biggest structural fork from D3D12's
//    root-signature model.
// ================================================================================================
//
//   set kVkSetTable0 (0)    = table 0: SRV registers t0..t(srvCount-1) at binding 0..srvCount-1,
//                             UAV registers u0..u(uavCount-1) at binding kVkUavBindingBase..+uavCount-1.
//   set kVkSetTable1 (1)    = table 1, same scheme, restarted within its own set (srvCount1/uavCount1).
//   set kVkSetConstants (2) = one UNIFORM_BUFFER_DYNAMIC binding per declared slot k where
//                             constantDwords[k] == 0 (a "root CBV"). Slot 0 (b0, PerFrame) is
//                             ALWAYS zero by convention (RHIResources.hpp:209) and so is ALWAYS
//                             bound here, on every pipeline — VulkanRenderContext writes its
//                             dynamic offset automatically on every setPipeline()
//                             (RHIResources.hpp:340-341).
//   set kVkSetSamplers (3)  = s0..s(samplerCount-1), IMMUTABLE (pImmutableSampler, matching
//                             D3D12's static samplers): allocated and bound ONCE per
//                             DescriptorLayoutEntry.
//
//   PUSH CONSTANTS carry every D3D12 root 32-bit CONSTANT (constantDwords[k] != 0): always
//   kObjectConstantRegister (b1, 32 dwords) plus any feature-declared slot, plus — mesh/
//   amplification pipelines only — the mesh-geometry block (vertex/index buffer device addresses
//   via VK_KHR_buffer_device_address -- push constants because D3D12's raw root-SRV bind has no
//   Vulkan descriptor-model equivalent, and this is the direct translation -- and the count block
//   at kMeshGeometryConstantRegister/b5).
//   Exact byte offsets in PushConstantLayout/pushConstantLayout() below; VulkanResourceFactory and
//   VulkanRenderContext must agree on them byte-for-byte.
constexpr u32 kVkSetTable0    = 0;
constexpr u32 kVkSetTable1    = 1;
constexpr u32 kVkSetConstants = 2;
constexpr u32 kVkSetSamplers  = 3;
// Set 4: PER-INSTANCE WORLD MATRICES for GraphicsPipelineDesc::instanced, as one
// StructuredBuffer<float4x4>. Its own set because the shared HLSL declares it at register
// t(declaredSrvCount) -- one past the layout's own SRVs -- which would otherwise land on
// kVkUavBindingBase (UAV slot 0) in set 0. Absent entirely from a non-instanced pipeline.
constexpr u32 kVkSetInstances = 4;
constexpr u32 kVkDescriptorSetCount = 5;   // table0, table1, constants, samplers, instances
// UAV bindings start here so a layout whose srvCount grows later never renumbers an already-cached
// UAV binding; kMaxBindingSlots (24) bounds one set's slot count, so SRV/UAV ranges never collide.
constexpr u32 kVkUavBindingBase = kMaxBindingSlots;

// Moves the shared prelude's per-frame constant buffer to the descriptor set this backend
// actually binds it in. MUST be applied to any HLSL including sharedShaderPrelude() before
// compiling.
//
// WHY: the prelude (modules/rhi/shaders/shared_prelude.hlsl, shared with D3D12) declares the
// per-frame cbuffer with no register space, so DXC maps it to SPIR-V set 0 (table 0, SRVs/UAVs)
// instead of kVkSetConstants where the per-frame UBO actually lives -- every shader touching
// gViewProj named a descriptor its own pipeline layout didn't contain.
//
// MATCHED ON THE DECLARATION, NOT THE REGISTER, AND THAT IS THE SECOND TIME THIS BROKE SILENTLY.
// The needle here used to be the literal `cbuffer PerFrame : register(b0)`. Two migrations later
// the prelude reads
//
//     cbuffer PerFrame : register(AVER_CB_JOIN(b, AVER_FRAME_CB)) {
//
// -- the register number is emitted from C++ now rather than spelled in HLSL -- so the find()
// returned npos, every fixed scene pipeline refused to build, and the backend fell back to D3D12
// with a warning. NOTHING CAUGHT IT because AVER_RHI_VULKAN was OFF in every configuration that
// ships, so the whole backend compiled green while being dead. That is the same shape as the
// SPIR-V codegen blocker one layer down; see this module's CMakeLists.
//
// So match `cbuffer PerFrame` at the head of a line and let the register spelling be whatever the
// prelude wants. The insertion point is before the keyword either way, and
// [[vk::binding(binding, set)]] overrides only the SPIR-V placement -- whatever `register(...)`
// resolves to still names the D3D-side slot, which is what keeps this a Vulkan-only edit.
//
// TRAP: AMD's two compilers disagreed on the symptom -- the integrated GPU gave a diagnosable
// VK_ERROR_INVALID_SHADER_NV, but the discrete card's LLPC called abort() instead, surfacing as
// the process dying with 0xC0000409 inside amdvlk64.dll with no message. Same defect; one symptom
// looks like a bug in this repo and the other looks like a crash.
//
// Vulkan-only DXC syntax, inserted into the assembled string this module owns; the shared prelude
// is never modified and D3D12 never sees it. Same approach as patchPushConstants in
// VulkanDevice.cpp.
// Rewrites `register(AVER_CB_JOIN(b, AVER_<X>_CB))` to the plain `register(bN)` it expands to.
//
// THE ONE PLACE THE MACRO STOPS. Three separate parts of this backend read a cbuffer's register
// number straight out of the HLSL text -- patchPerFrameSet below, patchPushConstants in
// VulkanDevice.cpp, and findCbufferBlock/findNextCbuffer in VulkanResourceFactory.cpp. All three
// were written when the prelude spelled `register(b1)`. RHIShaders.cpp now emits those numbers as
// macros instead (shaderConstantsHlsl), so all three quietly stopped finding anything: PerFrame
// landed in set 0, PerObject was never folded into push constants, and the AMD driver abort()ed on
// the resulting pipeline with no message. Teaching each parser about macros would be three chances
// to get it wrong; expanding once, here, restores the text every one of them already understands.
//
// SAFE BECAUSE IT IS THE SAME TOKEN. `AVER_CB_JOIN(b, AVER_OBJECT_CB)` and `b1` compile to the
// identical register -- this substitutes the preprocessor's own answer, using the same constants
// RHIShaders.cpp used to define the macros, so the two cannot drift apart. Idempotent: after it
// runs there is no macro left to match.
//
// D3D12 NEVER SEES THIS. It is applied to the assembled copy this module owns, like every other
// patch here.
inline void expandCbufferRegisters(std::string& src) {
    struct Reg { const char* macro; u32 value; };
    const Reg regs[] = {
        {"AVER_FRAME_CB",         kEngineFrameConstantRegister},
        {"AVER_OBJECT_CB",        kObjectConstantRegister},
        {"AVER_DRAW_CB",          kDrawConstantRegister},
        {"AVER_FEATURE_FRAME_CB", kFeatureFrameConstantRegister},
        {"AVER_MESH_GEOM_CB",     kMeshGeometryConstantRegister},
    };
    for (const Reg& r : regs) {
        const std::string from = std::string("AVER_CB_JOIN(b, ") + r.macro + ")";
        const std::string to   = "b" + std::to_string(r.value);
        for (std::size_t p = src.find(from); p != std::string::npos; p = src.find(from, p + to.size()))
            src.replace(p, from.size(), to);
    }
}

inline bool patchPerFrameSet(std::string& src) {
    expandCbufferRegisters(src);
    // AT THE HEAD OF A LINE, so a comment or a doc block that merely NAMES the cbuffer cannot be
    // patched instead of it -- several files in this tree discuss `cbuffer PerFrame` in prose, and
    // annotating one of those would silently produce a shader with the binding still on set 0 and no
    // error to show for it.
    const std::string needle = "cbuffer PerFrame";
    std::size_t pos = src.find(needle);
    while (pos != std::string::npos) {
        // Only whitespace may precede it on its line.
        std::size_t lineStart = src.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        bool blank = true;
        for (std::size_t i = lineStart; i < pos; ++i)
            if (src[i] != ' ' && src[i] != '\t') { blank = false; break; }
        // AND A WHOLE-WORD TEST, which findCbufferBrace in VulkanDevice.cpp has and this was written
        // without -- so `cbuffer PerFrameExtra` would have been annotated as if it were PerFrame,
        // putting the wrong block on kVkSetConstants and leaving the real one in set 0. No such
        // cbuffer exists today; the asymmetry between two functions doing the same job is the defect,
        // because the next person to add one would find out from a driver abort.
        const std::size_t after = pos + needle.size();
        const bool wholeWord = after >= src.size() ||
                               (!std::isalnum(static_cast<unsigned char>(src[after])) && src[after] != '_');
        if (blank && wholeWord) {
            src.insert(pos, "[[vk::binding(0, " + std::to_string(kVkSetConstants) + ")]] ");
            return true;
        }
        pos = src.find(needle, pos + needle.size());
    }
    return false;
}

// patchPerFrameSet handles only b0 deliberately: it runs at createShader time, before any
// PipelineLayout exists, and b0 is the one register every layout treats identically
// (kEngineFrameConstantRegister). Everything else is patchCbuffersForLayout's job, at PIPELINE
// creation, once a layout is available to consult -- an earlier note called a general version
// impossible, which held only with no layout in hand.
//
// TRAP: annotating every cbuffer into kVkSetConstants once moved SkinParams from set 0 to set 2,
// and validation flagged set 2 binding 3 as undeclared too -- SkinningPass declares
// constantDwords[3] = 4, so b3 is push constants, and a block can't be both. AMD's discrete driver
// reported none of it (same silent LLPC abort()). Run --debug-layer, and believe the layer over
// the shader text.



// The push-constant byte layout for one built pipeline. objectOffset/objectBytes are the SAME on
// every pipeline (b1 is always present, always first, always 128 bytes); slotOffset/slotBytes
// cover every OTHER declared root-constant slot, back to back in slot order; the mesh-geometry
// fields are valid only when the owning pipeline is a mesh pipeline. totalBytes is the whole
// range's size, and MUST be <= the device's queried maxPushConstantsSize (VulkanDevice::
// maxPushConstantsSize()) — a layout that does not fit cannot be built; see the file banner's
// note on pushConstantLayout() being where that check's INPUT is computed (the check itself, and
// the resulting createGraphicsPipeline/createComputePipeline failure, belongs to
// VulkanResourceFactory.cpp).
struct PushConstantLayout {
    static constexpr u32 kObjectOffset = 0;
    static constexpr u32 kObjectBytes  = kObjectConstantDwords * 4;   // 128
    u32 slotOffset[kMaxConstantSlots] = {};   // valid where the source PipelineLayout's constantDwords[k] != 0 (k != 1, which is always kObjectOffset/kObjectBytes)
    u32 slotBytes[kMaxConstantSlots]  = {};
    u32 meshVertexAddrOffset = 0;   // VkDeviceAddress (8 bytes) -- valid iff `mesh`
    u32 meshIndexAddrOffset  = 0;   // VkDeviceAddress (8 bytes) -- valid iff `mesh`
    u32 meshCountOffset      = 0;   // 4 dwords (16 bytes), mirrors kMeshGeometryConstantRegister (b5) -- valid iff `mesh`
    u32 totalBytes = 0;
};
// Computes the layout above for one PipelineLayout. DEFINED IN VulkanResourceFactory.cpp (see the
// file banner) — it is the pipeline-layout cache's job because the answer is cached alongside the
// VkPipelineLayout it describes, in DescriptorLayoutEntry below.
PushConstantLayout pushConstantLayout(const PipelineLayout& layout, bool mesh);

// A stable key for "everything about this PipelineLayout that changes how a shader compiles", so a
// module compiled for one layout is reused by every other layout that agrees and by no other. Packs
// the root-constant slot mask, all four table counts, the sampler count and `mesh`.
//
// It must cover every per-layout compile step. It began as the constant-slot mask alone, which was
// complete while patchCbuffersForLayout was the only such step, and stopped being complete the
// moment buildRegisterBinds made the table counts matter. Anything added later must extend it too.
u32 shaderVariantKey(const PipelineLayout& layout, bool mesh);

// Lowers EVERY cbuffer in a shader to what its PipelineLayout says it is -- the general form of
// what VulkanDevice.cpp does for its three FIXED pipelines by exact text. One source of truth,
// `constantDwords[N]`:
//
//   NON-ZERO -> root constants: folded into a single [[vk::push_constant]] struct (SPIR-V allows
//       at most one per entry point), concatenated in pushConstantLayout()'s byte order. PADDED:
//       b1 sits at byte 0, so a shader declaring only b3 needs its fields starting at byte 128 or
//       it reads the object block instead.
//   ZERO -> a descriptor: a dynamic UBO at binding == register in kVkSetConstants, per
//       descriptorLayout(). Left alone if patchPerFrameSet already annotated it.
//
// TAKES A LAYOUT, unlike patchPerFrameSet: a cbuffer's kind isn't in the shader text --
// `cbuffer SkinParams : register(b3)` reads the same whether b3 is push constants or a descriptor,
// only constantDwords[3] tells them apart. A source-only patch would have to guess (tried,
// reverted: turns a diagnosable "wrong set" into an equally broken "right set, wrong kind"). So
// createShader keeps RhiShader's source and moduleForLayout re-patches at PIPELINE creation, once
// the layout is known.
//
// False only on a malformed block or an unpaddable gap, both logged. A shader already agreeing
// with its layout comes back byte-identical -- moduleForLayout's signal to reuse the
// layout-agnostic module rather than compile a second one.
//
// `annotateDescriptors` false leaves ZERO-dword blocks alone, for callers placing them via
// -fvk-bind-register (buildRegisterBinds) instead; the push-constant half always runs since a
// folded block has no register left for the map to express.
bool patchCbuffersForLayout(std::string& src, const PipelineLayout& layout, bool mesh,
                            bool annotateDescriptors);

// ================================================================================================
// 5. Per-frame constant ring: the Vulkan analog of D3D12RenderContext::ringAlloc. One HOST_VISIBLE
//    buffer per frame in flight, bump-allocated at the device's queried
//    minUniformBufferOffsetAlignment (NOT a fixed 256 -- see VulkanDevice::minUboAlignment()) and
//    bound as a UNIFORM_BUFFER_DYNAMIC through set kVkSetConstants; the dynamic OFFSET returned
//    here is what a raw root-CBV's GPU address was in the D3D12 model. OWNED BY
//    VulkanRenderContext.cpp -- see ConstantRing's own note on the one exception.
// ================================================================================================
// Transient constant bytes per frame in flight: starting size and the ceiling growth stops at.
//
// DECLARED HERE, ABOVE ConstantRing, BECAUSE ConstantRing USES IT -- these two lines used to sit
// thirty lines below that struct, so every TU failed on `kRhiRingBytes: identifier not found`,
// unnoticed because the module also couldn't be CONFIGURED (CMakeLists named a nonexistent
// source file, so CMake failed first).
//
// Not shared with D3D12's own constant of the same name: its ring is an UPLOAD-heap buffer that
// is always mappable, while this one may not be HOST_COHERENT (see ConstantRing::coherent below).
constexpr u64 kRhiRingBytes = 1u << 20;
constexpr u64 kRhiRingMaxBytes = 64ull << 20;

struct ConstantRing {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u8* mapped = nullptr;
    bool coherent = false;   // whether `memory`'s type is HOST_COHERENT; false means every write
                              // needs an explicit vkFlushMappedMemoryRanges before the GPU reads it
                              // -- Vulkan memory coherency varies by vendor/type, unlike D3D12's
                              // UPLOAD heap which is always simply mappable. MUST be checked, not
                              // assumed; see the architecture scout's own flag on this.
    VkDeviceSize bytes = 0;    // current allocation size; grows geometrically, mirrors D3D12's ringBytes_
    VkDeviceSize used = 0;     // this frame's bump cursor, reset every beginFrame
    VkDeviceSize wanted = kRhiRingBytes;   // sticky high-water mark across frames
};
// One suballocation's address, ready to feed straight into vkCmdBindDescriptorSets'
// pDynamicOffsets (via `offset`) or a direct memcpy (via `cpu`).
struct ConstantAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    u8* cpu = nullptr;   // nullptr on overflow -- the caller must treat that exactly like a failed
                          // D3D12 ringAlloc: log once per frame (ringOverflowEpoch_-shaped), then
                          // skip the draw/pass rather than write past the buffer.
};

// ================================================================================================
// 6. Per-draw CPU-side structs.
//    PerFrameCB and PostCB USED TO BE HAND-COPIED HERE from D3D12Device.cpp -- this file said so
//    itself, warning that "a single stray float here is a silent cross-backend shading divergence
//    no compiler catches". Both are now defined once in aver/rhi/FrameConstants.hpp, which the two
//    backends include, so that divergence is no longer expressible.
// ================================================================================================

// Per-draw binding table 1 plus its b2 constant block, copied from the caller. Identical in shape
// to D3D12Device.cpp's private DrawBinding; defined here (not per-class) because both
// VulkanDevice (drawBinding_/defaultDrawBinding_) and VulkanRenderContext (its own sticky
// setDrawBinding state) need it.
struct DrawBinding {
    BindingSetHandle set = 0;
    u8  constants[kMaxDrawConstantBytes] = {};
    u32 bytes = 0;
};
inline void storeDrawBinding(DrawBinding& d, BindingSetHandle set, const void* constants, u32 bytes) {
    if (bytes > kMaxDrawConstantBytes) {
        AVER_ERROR("[RHI.Vulkan] setDrawBinding constant block is {} bytes, over the {} limit", bytes, kMaxDrawConstantBytes);
        return;
    }
    d.set = set;
    d.bytes = (constants && bytes) ? bytes : 0;
    if (d.bytes) std::memcpy(d.constants, constants, d.bytes);
}

// A mesh uploaded to the GPU. Vulkan-flavoured twin of D3D12Device.cpp's GpuMesh — same fields,
// same index-buffer-sharing discipline (see its comment there for WHY: createSkinTargetMesh shares
// its source's index buffer, so ownership is recorded rather than inferred), same "slot cleared,
// never recycled" contract for destroyMesh (IDevice::destroyMesh's own comment explains why a
// stale handle must address a dead mesh rather than a different live one).
struct GpuMesh {
    VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VkDeviceMemory vbMemory = VK_NULL_HANDLE, ibMemory = VK_NULL_HANDLE;
    // buffer_device_address raw pointers -- what a mesh-shader pipeline's push-constant geometry
    // block carries (see PushConstantLayout::meshVertexAddrOffset/meshIndexAddrOffset), the direct
    // Vulkan analog of D3D12's raw root-SRV bind for the same two buffers.
    VkDeviceAddress vbAddress = 0, ibAddress = 0;
    u32 indexCount = 0;
    // The RHI-level buffer handles mirroring `vb`/`ib`, so a shader can be given descriptors over
    // this mesh's own geometry (IDevice::meshGeometry) -- same purpose as D3D12's vbBuffer/ibBuffer.
    BufferHandle vbBuffer = 0;
    BufferHandle ibBuffer = 0;
    u32 vertexCount = 0;
    // Non-zero only for a createSkinTargetMesh() result; see IDevice::meshVertexBuffer's own
    // "cache expiry" contract for why this predicate must stay exact.
    bool computeWritten = false;
    f32 boundsCentre[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsRadius = 0.0f;
    bool ibOwned = true;
    u32  ibShares = 0;
    MeshHandle ibSource = 0;
    // W11: VERTEX-buffer sharing, the mirror image of ibOwned/ibShares/ibSource above rather than a
    // reuse of them -- a createSkinTargetMesh share and a createMeshSharingVertices share point in
    // OPPOSITE directions (the former shares indices and owns its vertices; the latter shares
    // vertices and owns its indices), and a single pair of fields could not distinguish "my indices
    // are shared" from "my vertices are shared" for a mesh that is somehow both. See
    // IDevice::createMeshSharingVertices's contract (RHI.hpp) for the LOD-ladder case this exists
    // for. vbSource, when !vbOwned, always names the ROOT (never another share) -- see
    // VulkanDevice::createMeshSharingVertices's own note on why a share of a share collapses.
    bool vbOwned = true;
    u32  vbShares = 0;
    MeshHandle vbSource = 0;
    bool alive = true;
};
struct GpuLineMesh {
    VkBuffer vb = VK_NULL_HANDLE;
    VkDeviceMemory vbMemory = VK_NULL_HANDLE;
    u32 count = 0;
};

// Per-subresource state tracking, debug builds only -- same macro shape as D3D12Device.cpp's own,
// redefined here since it is a private convenience macro, not shared cross-module state.
#if defined(NDEBUG)
#define AVER_RHI_TRACK_STATE 0
#else
#define AVER_RHI_TRACK_STATE 1
#endif

// ================================================================================================
// 7. Handle-table records. A handle is index + 1 into the owning vector, so 0 is never live; a
//    dead slot is kept (never recycled) with its Vk handles reset to VK_NULL_HANDLE, exactly
//    mirroring D3D12Device.cpp's own RhiTexture..RhiTlas discipline (a lookup is "alive" iff its
//    primary handle is non-null).
// ================================================================================================

struct RhiTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView srvView = VK_NULL_HANDLE;    // whole resource, every mip, the SAMPLED aspect
    VkImageView rtvView = VK_NULL_HANDLE;    // single-mip colour-attachment view, when ResourceBind::RenderTarget
    VkImageView dsvView = VK_NULL_HANDLE;    // depth-attachment view, when ResourceBind::DepthStencil (or isDepthFormatVk)
    std::vector<VkImageView> uavViews;       // one storage-image view per mip, built lazily by setUav
    TextureDesc desc{};                      // resolved: `mips` holds the real count, never 0
    // OUTSIDE AVER_RHI_TRACK_STATE, unlike `states` below (see D3D12's RhiTexture::debugName). THE
    // MISTAKE THIS AVOIDS: textureBarrier/uavBarrierTexture name the texture in their null-VkImage
    // error, but the field used to sit inside that macro, so neither message compiled under
    // NDEBUG -- unnoticed because Aver.RHI.Vulkan had never been built Release. Moving it was the
    // fix: the validation layer reports a null-VkImage failure at the barrier call site, not at
    // whatever earlier code left the image null, so having the name there is a one-line fix vs a hunt.
    std::string debugName;                   // owned copy: the desc's debugName is the caller's pointer
#if AVER_RHI_TRACK_STATE
    std::vector<ResourceState> states;       // one entry per mip; a subresource index is a mip index here
#endif
    // The descriptor handed to the UI, cast to u64 -- see uiDescriptor()'s own note that this stays
    // unused while VulkanDevice does not override IDevice's ui* methods (see the CMakeLists note on
    // why imgui_impl_vulkan is a deliberately deferred decision). Kept so the field exists the day
    // it is wired up, rather than adding it to this struct's ABI later.
    u64 uiDescriptor = 0;
    // True for a record WRAPPING a VkImage/VkDeviceMemory this factory did NOT allocate -- today
    // only adoptExternalDepthTexture's republication of VulkanDevice::depthBuffer_/depthMemory_
    // (IDevice::sceneDepthTexture's contract, RHI.hpp). VulkanDevice owns and tears those down
    // itself, so destroyTexture()/~VulkanResourceFactory() MUST skip destroyImageCommitted() when
    // set, or a resize/shutdown double-frees an already-recycled image. The VIEWS this record
    // builds (srvView etc.) are NOT foreign and are always destroyed normally regardless.
    bool externallyOwned = false;
};

struct RhiBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;   // valid whenever created with SHADER_DEVICE_ADDRESS usage (see toVkBufferUsage) -- every buffer this factory creates, so always valid once `buffer` is
    BufferDesc desc{};
    u8* mapped = nullptr;          // Upload/Readback-kind buffers stay mapped for their whole life
    bool coherent = false;         // see ConstantRing::coherent's identical note
    std::string debugName;         // identity, not state tracking -- see RhiTexture::debugName
#if AVER_RHI_TRACK_STATE
    ResourceState state = ResourceState::Common;
    bool stateFixed = false;       // Upload-heap-equivalent and AccelStructure-kind buffers reject every transition
#endif
};

// One compile of a shader for one PUSH-CONSTANT SHAPE. See RhiShader::variants.
struct RhiShaderVariant {
    u32 key = 0;                  // pushConstantKey() of the PipelineLayout this was compiled for
    std::vector<u32> spirv;
    VkShaderModule module = VK_NULL_HANDLE;
};

struct RhiShader {
    // The LAYOUT-AGNOSTIC compile: every cbuffer a descriptor. Still the module most pipelines use,
    // and always the one reflectTableSlotKinds reads -- patching only ever moves CBUFFERS, so the
    // SRV/UAV bindings it reflects are identical in every variant.
    std::vector<u32> spirv;
    ShaderStage stage = ShaderStage::Vertex;
    VkShaderModule module = VK_NULL_HANDLE;

    // WHY A SHADER KEEPS ITS SOURCE: a cbuffer's descriptor-vs-root-constant kind is a property of
    // the PIPELINE LAYOUT, not the shader text, and createShader has no idea yet which pipeline
    // will use its module -- see patchCbuffersForLayout's fuller writeup. Re-patched per layout at
    // PIPELINE creation instead. See moduleForLayout().
    std::string source;           // prelude + source, already patchPerFrameSet'd
    std::string entry;
    std::string defines;
    u32 minShaderModel = 60;
    std::vector<RhiShaderVariant> variants;
};

// The cached VkDescriptorSetLayout for one table SHAPE (srvCount/uavCount + each slot's SlotKind).
// SEPARATE from DescriptorLayoutEntry below, and at a FINER grain: several different
// PipelineLayouts can share one table shape, and a BindingSetHandle created against
// BindingSetDesc's matching fields is valid to bind at ANY table index any pipeline currently
// declares that exact shape at -- Vulkan descriptor-set-layout compatibility is per-SHAPE, not
// per-pipeline. Both createBindingSet() and every PipelineLayout's own table set layout MUST come
// from this ONE cache (VulkanResourceFactory::tableSetLayout()), or two textually-identical shapes
// could mint two different VkDescriptorSetLayout objects and silently break that contract.
struct TableShapeEntry {
    u32 srvCount = 0, uavCount = 0;
    SlotKind srvKinds[kMaxBindingSlots] = {};
    SlotKind uavKinds[kMaxBindingSlots] = {};
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
};

// One cached (SamplerDesc -> VkSampler) pair, for the IMMUTABLE samplers set kVkSetSamplers bakes in.
struct SamplerCacheEntry {
    SamplerDesc desc{};
    VkSampler sampler = VK_NULL_HANDLE;
};

// A full pipeline-layout cache entry: everything a built RhiPipeline needs to bind and push
// constants against, shared by every RhiPipeline built from an equal PipelineLayout. The Vulkan
// analog of D3D12Device.cpp's RootSigEntry, at the same cache granularity (looked up by
// sameLayout(), one entry per DISTINCT layout — not per pipeline).
struct DescriptorLayoutEntry {
    PipelineLayout layout{};
    bool mesh = false;
    VkDescriptorSetLayout tableSetLayouts[kBindingTableCount] = {};   // shared via tableSetLayout(); NOT owned here
    VkDescriptorSetLayout constantsSetLayout = VK_NULL_HANDLE;        // owned: one dynamic-UBO binding per descriptor-CBV slot
    VkDescriptorSetLayout samplersSetLayout = VK_NULL_HANDLE;         // owned; VK_NULL_HANDLE when layout.samplerCount == 0
    VkDescriptorSet samplersSet = VK_NULL_HANDLE;                     // allocated ONCE, immutable-sampler-only, never rewritten; VK_NULL_HANDLE when samplerCount == 0
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    PushConstantLayout pushConstants{};
    // GraphicsPipelineDesc::instanced, which is a SIBLING of PipelineLayout rather than part of it,
    // so it has to be part of this cache key too -- two pipelines with identical layouts but
    // different instancing need different VkPipelineLayouts.
    bool instanced = false;
    // The shared one-binding set-4 layout, from VulkanResourceFactory::instanceSetLayout(). NOT owned
    // here (one layout serves every instanced pipeline, exactly like tableSetLayouts above).
    VkDescriptorSetLayout instancesSetLayout = VK_NULL_HANDLE;
};

// A pipeline state and the layout-cache entry it was built against.
struct RhiPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    const DescriptorLayoutEntry* layoutEntry = nullptr;   // owned by VulkanResourceFactory's cache, not by this
    bool compute = false;
    bool mesh = false;
    // True when this mesh pipeline also has an amplification/task shader (GraphicsPipelineDesc::as
    // != 0), i.e. it is a dispatchMeshClusters() pipeline rather than a dispatchMeshFor() one.
    bool amplification = false;
    // Built with GraphicsPipelineDesc::instanced -- set kVkSetInstances exists in its pipeline layout
    // and its vertex shader reads gInstanceWorlds. drawMeshInstanced checks this exactly as
    // D3D12RenderContext::drawMeshInstanced checks pipe_->instanceWorldParam >= 0.
    bool instanced = false;
};

// One allocated VkDescriptorSet for table 0 or table 1, plus the slot kinds it was declared with
// (for nullFill's dimension-matching and setSrv/setUav's slot-kind validation). Allocated from
// VulkanResourceFactory's single shared descriptorPool_ -- the Vulkan analog of D3D12's one
// shared shader-visible heap, at DESCRIPTOR-SET rather than DESCRIPTOR-RANGE granularity (see the
// architecture scout's finding #2 on why sets, not a bindless array, is the strategy here).
// ONE SLOT'S RESOLVED DESCRIPTOR, kept so the write can be REPLAYED into a different ring slot.
// Vulkan has no way to copy "whatever is in this binding" out of a descriptor set, so the only way
// to bring a second set up to date is to remember what was written and write it again.
struct BindingSlotState {
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;   // MAX_ENUM == never written
    VkDescriptorImageInfo      image{};
    VkDescriptorBufferInfo     buffer{};
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
    // A one-off mip view built by setSrv for THIS slot, which the slot therefore owns: it has to
    // outlive the replay into every other ring slot, so it cannot be retired at the end of the call
    // that made it (which is what setSrv used to do).
    VkImageView ownedView = VK_NULL_HANDLE;
};

// A binding set, RINGED kFrameCount DEEP.
//
// WHY: a descriptor set can't be rewritten while a command buffer that bound it is still pending
// ("... destroyed or updated without UPDATE_AFTER_BIND") -- worse than a warning, it puts the
// command buffer in an INVALID state and the driver DROPS every call recorded after it. With one
// set per binding set that happened every frame the engine rebound anything (Voxi's RT history
// ping-pong rewrites four slots per frame), taking the whole overlay down with it.
//
// beginFrame() already waits on the timeline value retiring frameIndexInFlight(), so the CURRENT
// frame's ring slot is provably free to write. Cost: a write lands in one ring slot only, so the
// others go stale -- hence srvSlots/uavSlots and staleMask, replayed lazily by bindingSetForFrame().
//
// INVARIANT: nothing writes a binding set AFTER binding it within the same frame -- a ring by
// FRAME can't help with that. Measured, not assumed: writeBindingSlot warns if this is ever
// violated, and it was zero across a 12-frame run when this was built.
struct RhiBindingSet {
    VkDescriptorSet sets[kFrameCount] = {};
    // Bit f: ring slot f has not seen the writes below and must be replayed before it is bound.
    u32 staleMask = 0;
    BindingSlotState srvSlots[kMaxBindingSlots];
    BindingSlotState uavSlots[kMaxBindingSlots];
    u64 lastBoundSerial = ~0ull;   // the frame this set was last bound in; ~0 == never
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;   // which TableShapeEntry this matches; not owned here
    u32 srvCount = 0, uavCount = 0;
    u32 srvBaseRegister = 0, uavBaseRegister = 0;
    SlotKind srvKinds[kMaxBindingSlots] = {};
    SlotKind uavKinds[kMaxBindingSlots] = {};
    bool alive = false;
};

// A bottom-level acceleration structure for one mesh, with its build scratch. Requires
// VK_KHR_acceleration_structure; every field stays VK_NULL_HANDLE/0/false when that extension is
// absent, and createBlas must then fail exactly like D3D12 does when caps_.rayTracingTier == 0.
struct RhiBlas {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuffer asBuffer = VK_NULL_HANDLE;      VkDeviceMemory asMemory = VK_NULL_HANDLE;
    VkBuffer scratchBuffer = VK_NULL_HANDLE; VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
    VkDeviceAddress asAddress = 0;
    MeshHandle mesh = 0;
    bool built = false;
};
// A top-level acceleration structure, its scratch, and one instance buffer per frame in flight.
struct RhiTlas {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuffer asBuffer = VK_NULL_HANDLE;      VkDeviceMemory asMemory = VK_NULL_HANDLE;
    VkBuffer scratchBuffer = VK_NULL_HANDLE; VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
    VkDeviceAddress asAddress = 0;
    VkBuffer instanceBuffers[kFrameCount] = {};
    VkDeviceMemory instanceMemory[kFrameCount] = {};
    u8* instancePtr[kFrameCount] = {};
    u32 maxInstances = 0;
};

// A destroyed object the GPU may still be reading, released only once `fence` has retired. The
// Vulkan analog of D3D12Device.cpp's RetiredObject, generalised with a type-erased deleter instead
// of a ComPtr<IUnknown> -- Vulkan handles share no common base a ComPtr-style Release() could work
// through, so the closure IS the generalisation (capture whatever handles + the owning VkDevice a
// given destroy needs).
struct RetiredObject {
    u64 fence = 0;
    std::function<void()> destroy;
};

// ================================================================================================
// 8. VulkanShaderCompiler — DXC, `-spirv`. DEFINED IN VulkanShaderCompiler.cpp.
// ================================================================================================
//
// This backend has no FXC-equivalent fallback: DXC is the ONLY compiler in scope (see
// third_party/vulkan-headers/README.md — adding glslang/shaderc would be a second source of truth
// for the same HLSL), so a Vulkan device with no usable DXC simply cannot compile shaders, and
// compile() failing is what createShader must treat as this device having no working shader path
// at all, not as one shader among many failing.
// VkRegisterBind / kMaxRegisterBinds / buildRegisterBinds live in VulkanRegisterMap.hpp,
// included at the top of this file -- see that header for why they are separable.

class VulkanShaderCompiler {
public:
    // Loads dxcompiler.dll once (the SAME DLL the D3D12 backend loads for DXIL — see this module's
    // CMakeLists.txt for why no separate copy step is needed) and resolves DxcCreateInstance.
    void init();
    bool usingDxc() const;

    // Compiles one HLSL entry point to SPIR-V. `src` is the FULLY ASSEMBLED source (prelude
    // already concatenated by the caller). `stage`+`minShaderModel` derive the DXC target profile
    // (e.g. "cs_6_5") via dxcTargetPrefix(stage); `defines` is semicolon-separated, same shape as
    // ShaderDesc::defines. Appends `-spirv` and the binding-shift arguments section 4's descriptor
    // scheme requires (must agree with tableSetLayout()/descriptorLayout() byte for byte). False
    // (and logged) on failure, `outSpirv` left untouched.
    //
    // `quiet` downgrades DXC's diagnostics from ERROR to DEBUG, for a caller that EXPECTS this
    // compile to possibly fail and has a recovery path (moduleForLayout's bind-map attempt).
    bool compile(const char* src, const char* entry, ShaderStage stage, u32 minShaderModel,
                 const char* defines, std::vector<u32>& outSpirv,
                 const VkRegisterBind* binds = nullptr, u32 bindCount = 0, bool quiet = false);

private:
    bool tried_ = false;
    HMODULE dll_ = nullptr;
    // IDxcUtils*/IDxcCompiler3*, kept as void* so this header never has to include <dxcapi.h> (or
    // <wrl/client.h>) — DXC's COM types have no more business leaking into VulkanDevice.cpp/
    // VulkanResourceFactory.cpp than a VkFoo has leaking outside this module. Cast back to the real
    // types only inside VulkanShaderCompiler.cpp, the one file that includes dxcapi.h.
    void* utils_ = nullptr;
    void* compiler_ = nullptr;
};
// The process-wide compiler instance, shared by VulkanDevice's fixed pipelines and
// VulkanResourceFactory::createShader alike -- mirrors D3D12Device.cpp's own shaderCompiler().
VulkanShaderCompiler& vulkanShaderCompiler();

// DXC target-profile prefix for a stage ("vs"/"ps"/"gs"/"cs"/"ms"/"as") -- identical convention to
// D3D12Device.cpp's stagePrefixFor, reusable here since both createShader (VulkanResourceFactory.cpp)
// and the fixed-pipeline shader compiles (VulkanDevice.cpp) need to build the same "%s_%u_%u" target
// string.
inline const char* dxcTargetPrefix(ShaderStage s) {
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

// ================================================================================================
// 9. Forward declarations + committed-allocation helpers (DEFINED IN VulkanResourceFactory.cpp —
//    see the file banner).
// ================================================================================================
class VulkanDevice;
class VulkanResourceFactory;

// The in-window UI seam, defined in the PUBLIC header aver/rhi/vulkan/UiBackend.hpp. Declared here
// so VulkanDevice can hold one and befriend the installer without this header including that one --
// it is public and must not pull vulkan.h into anything that includes it.
class IUiBackend;
struct UiBackendInitDesc;
bool installUiBackend(IDevice* device, IUiBackend* backend);
class VulkanRenderContext;

// Creates a buffer, allocates device memory of the first type matching `required`, and binds them
// -- the multi-TU analog of D3D12's CreateCommittedResource for buffers. `outAddress`, when
// non-null, receives vkGetBufferDeviceAddress's result (the buffer is always created with
// SHADER_DEVICE_ADDRESS usage; see toVkBufferUsage). False (and logged) on any failure, with
// whatever partial VkBuffer/VkDeviceMemory it made already destroyed -- the caller never has to
// clean up a partially-failed call.
bool createBufferCommitted(VulkanDevice& dev, VkDeviceSize bytes, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags required, VkBuffer& outBuffer, VkDeviceMemory& outMemory,
                           VkDeviceAddress* outAddress = nullptr, const char* debugName = nullptr);
// Same shape, for images. `imageInfo` is the caller's fully-populated VkImageCreateInfo (usage,
// format, extent, mips, samples already set); only the memory allocation and bind are generic here.
bool createImageCommitted(VulkanDevice& dev, const VkImageCreateInfo& imageInfo,
                          VkMemoryPropertyFlags required, VkImage& outImage, VkDeviceMemory& outMemory,
                          const char* debugName = nullptr);
void destroyBufferCommitted(VulkanDevice& dev, VkBuffer buffer, VkDeviceMemory memory);
void destroyImageCommitted(VulkanDevice& dev, VkImage image, VkDeviceMemory memory);

// Buffer/image usage flags this factory grants EVERY buffer/image it creates, regardless of what
// the caller declared ResourceBind/BufferKind as -- a deliberate "everything is everything"
// simplification (D3D12 has no per-resource usage-flag concept to mirror narrowly; a D3D12 buffer
// resource is generically bindable), traded for one line of policy instead of a matrix of usage
// combinations no test here can yet exercise. Revisit if a specific combination turns out to be
// something a real driver refuses.
// `rtAvailable` gates the one usage bit that is NOT always legal: a device without
// VK_KHR_acceleration_structure rejects a buffer asking for AS build-input usage outright.
inline VkBufferUsageFlags toVkBufferUsage(const BufferDesc& d, bool rtAvailable) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (d.kind == BufferKind::AccelStructure)
        u |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    // BUILD-INPUT USAGE BELONGS ON ORDINARY BUFFERS TOO, not only AccelStructure ones -- it used to
    // be the latter only, and a BLAS builds from a plain vertex/index buffer (BufferKind::Default),
    // so every build was rejected (missing
    // VK_BUFFER_USAGE_2_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR). Worse: on failure
    // vkCmdBuildAccelerationStructuresKHR INVALIDATES the command buffer, so everything recorded
    // after it -- the whole overlay, editor UI included -- got dropped by the driver. Granting it
    // to every buffer follows "everything is everything": the alternative, a BufferKind for "might
    // be raytraced", is unknowable at upload time for a mesh some later frame decides to trace.
    if (rtAvailable) u |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    return u;
}
inline VkImageUsageFlags toVkImageUsage(ResourceBind bind, bool isDepth) {
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (any(bind, ResourceBind::ShaderResource))  u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(bind, ResourceBind::UnorderedAccess)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(bind, ResourceBind::RenderTarget))    u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(bind, ResourceBind::DepthStencil) || isDepth) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return u;
}

// ================================================================================================
// 10. VulkanSwapchain (ISwapchain). Every method a thin forwarder to VulkanDevice, exactly like
//     D3D12Swapchain. DEFINED IN VulkanDevice.cpp.
// ================================================================================================
class VulkanSwapchain final : public ISwapchain {
public:
    explicit VulkanSwapchain(VulkanDevice* dev) : dev_(dev) {}
    void present() override;
    void resize(u32 width, u32 height) override;
    u32 width() const override;
    u32 height() const override;
private:
    VulkanDevice* dev_;
};

// ================================================================================================
// 11. VulkanDevice (IDevice). DEFINED IN VulkanDevice.cpp.
// ================================================================================================
class VulkanDevice final : public IDevice {
public:
    // Not an override (IDevice has no init()); createVulkanDevice() calls this once, exactly as
    // D3D12Device::init() is called from its own createD3D12Device().
    bool init(const DeviceDesc& desc);
    ~VulkanDevice() override;

    // ---- IDevice ----
    Backend backend() const override { return Backend::Vulkan; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }
    // M6: the adapter's current VRAM budget/usage, split LOCAL/NON_LOCAL exactly as
    // VideoMemoryInfo's own comment (RHI.hpp) specifies. NOT inline (queries the driver every call,
    // unlike every trivial getter around it) -- defined in VulkanDevice.cpp beside queryCaps().
    // supported == false whenever VK_EXT_memory_budget or GetPhysicalDeviceMemoryProperties2 is
    // absent, honestly, rather than a stale or guessed number.
    VideoMemoryInfo videoMemory() const override;
    IResourceFactory* resources() override;
    IRenderContext* renderContext() override;
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    // Same predicate drawMesh applies internally (suppressesScene early return), exposed so a
    // caller can decline a draw it knows to be editor chrome -- see IDevice::sceneSuppressed for
    // why the filter can't live in drawMesh itself. Implemented here rather than left on the
    // default false: that default would silently mean "nothing is suppressing" even though
    // drawMesh has the identical path, a cross-backend divergence that's hard to notice.
    bool sceneSuppressed() const override {
        for (const IRenderFeature* f : features_) if (f->suppressesScene()) return true;
        return false;
    }
    // Non-owning, like addRenderFeature/removeRenderFeature above. Null is the PERMANENT default
    // (nothing here ever assigns upscaler_ on its own) and every branch that matters gates on this
    // pointer rather than a quality enum or build flag, so "no upscaler set" and "no AverSR module
    // in this build" are the same code path -- see D3D12Device::setUpscaler for the same invariant.
    void setUpscaler(IUpscaler* u) override { upscaler_ = u; }
    IUpscaler* upscaler() const override { return upscaler_; }
    Format backbufferFormat() const override { return fromVkFormat(kVkSceneColorFormat); }
    Format depthFormat() const override { return fromVkFormat(kVkDepthFormat); }
    u32 sampleCount() const override { return sampleCount_; }
    bool setSampleCount(u32 samples) override;
    // Decouples the scene's render targets from the swapchain's: the scene renders at
    // round(present * scale) while everything present-resolution (backbuffer, viewport texture,
    // capture) stays pinned to width_/height_ (IDevice::setRenderScale's contract, RHI.hpp).
    // NOT inline: the setter does real work -- PARKS the value (pendingRenderScale_) rather than
    // rebuilding here. See applyPendingRenderScale's comment for why (aver-render-scale-device-
    // loss); this is the Vulkan twin of D3D12Device::setRenderScale, fixed there in fa74459 and
    // left outstanding here until wave 2 (C2-13) -- AverSR Auto now calls this from onUpdate on
    // every launch, at Medium/Low's non-1.0 default, so the immediate-rebuild path was no longer
    // a rare CLI-only corner.
    void setRenderScale(f32 scale) override;
    // Reports what the last caller ASKED FOR (the parked value when one is pending), not what is
    // currently resident, so a read-back immediately after a set sees the value it just wrote
    // rather than the old one for one frame. Mirrors D3D12Device::pendingOrCurrentRenderScale().
    f32 renderScale() const override { return pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_; }
    ISwapchain* createSwapchain(const SwapchainDesc& desc) override;
    void beginFrame() override;
    void endFrame() override;
    void setClearColor(f32 r, f32 g, f32 b, f32 a) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; clear_[3] = a; }
    void setVSync(bool on) override { vsync_ = on; }
    bool vsync() const override { return vsync_; }
    bool vsyncCanDisable() const override { return tearingSupported_; }
    void setViewportRect(u32 x, u32 y, u32 w, u32 h) override;
    // D3D12Device's twin, same scene-space reasoning -- see IDevice::viewportAspect.
    f32  viewportAspect() const override {
        const u32 w = vpW_ ? vpW_ : sceneWidth_;
        const u32 h = vpH_ ? vpH_ : sceneHeight_;
        return h ? static_cast<f32>(w) / static_cast<f32>(h) : 0.0f;
    }
    void setViewportToTexture(bool on) override { viewportToTex_ = on; }
    bool viewportToTexture() const override { return viewportToTex_; }
    u64 viewportTextureId() override;
    bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) override;
    MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount, const u32* indices, u32 indexCount) override;
    // W4: chooses the heap createMesh() (and createMeshSharingVertices()'s own new index buffer)
    // upload to for every call made AFTER this one -- trivial store/load, exactly like setVSync/
    // vsync() beside it; see IDevice's own contract (RHI.hpp) for the false=Upload/true=Default
    // meaning and why a mesh already built keeps whatever heap it was built on.
    void setStaticMeshHeapDefault(bool onDefaultHeap) override { staticMeshDefaultHeap_ = onDefaultHeap; }
    bool staticMeshHeapDefault() const override { return staticMeshDefaultHeap_; }
    // W11: a new mesh sharing `source`'s vertex buffer with its own index buffer -- see
    // IDevice::createMeshSharingVertices's full contract (RHI.hpp) and GpuMesh's vbOwned/vbShares/
    // vbSource fields above. NOT inline: real allocation and (conditionally) a one-shot upload;
    // defined in VulkanDevice.cpp beside createMesh.
    MeshHandle createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) override;
    bool destroyMesh(MeshHandle mesh) override;
    bool destroyLineMesh(LineHandle mesh) override;
    MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) override;
    BufferHandle meshVertexBuffer(MeshHandle mesh) const override;
    bool meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib, u32* vertexCount, u32* indexCount) const override;
    bool meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const override;
    void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 cameraPos[3]) override;
    bool camera(f32 viewProj[16], f32 invViewProj[16], f32 cameraPos[3]) const override;
    bool sceneViewport(f32 rect[4]) const override;
    void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) override;
    void setWaterWaves(const f32 (*waves)[4], u32 count, f32 amplitude) override {
        const u32 n = count > 3u ? 3u : count;
        for (u32 i = 0; i < 3; ++i)
            for (int a = 0; a < 4; ++a) frameCB_.wave[i][a] = (i < n && waves) ? waves[i][a] : 0.0f;
        frameCB_.waveParams[0] = amplitude;
        frameCB_.waveParams[1] = static_cast<f32>(n);
        frameCB_.waveParams[2] = frameCB_.waveParams[3] = 0.0f;
    }
    void setFrameTime(f32 seconds, f32 deltaSeconds) override {
        frameCB_.time[0] = std::fmod(seconds, 3600.0f);   // see the D3D12 twin for why it wraps
        frameCB_.time[1] = seconds;
        frameCB_.time[2] = deltaSeconds;
        frameCB_.time[3] = 0.0f;
    }
    void setSkyAtmosphere(const SkyAtmosphere& s) override;
    SkyAtmosphere skyAtmosphere() const override { return sky_; }
    void setPostProcess(const PostSettings& p) override { post_ = p; }
    PostSettings postProcess() const override { return post_; }
    void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4], f32 metallic, f32 roughness) override;
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(drawBinding_, set, constants, bytes);
    }
    void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(defaultDrawBinding_, set, constants, bytes);
    }

    // ---- same-frame depth prepass -- full contract on IDevice (RHI.hpp); mirrors D3D12Device.cpp
    // :1005-1013. OFF by default: --depth-prepass measured as a LOSS on this engine's scenes (see
    // aver-frame-budget in project memory), so this exists for correctness/parity, not speed, and
    // the default must reproduce today's no-prepass Vulkan behaviour exactly.
    void setDepthPrepassEnabled(bool on) override { depthPrepassEnabled_ = on; }
    bool depthPrepassEnabled() const override { return depthPrepassEnabled_; }
    void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16]) override;
    // AUTO-CONSUMED by the very next drawMesh() call, not stored past it (see IDevice). Plain
    // assignment: this only records what the CALLER already believes about the upcoming draw's
    // eligibility (e.g. a skinned mesh); drawMesh() still re-checks meshVertexBuffer(mesh) before
    // trusting it, exactly as D3D12Device::drawMesh does.
    void setNextDrawPrepassed(bool prepassed) override { nextDrawPrepassed_ = prepassed; }
    // Contract on IDevice::sceneDepthTexture (RHI.hpp); mirrors D3D12Device::sceneDepthTexture
    // (D3D12Device.cpp:1993-2006). Lazily (re)adopts depthBuffer_ into rhiFactory_'s texture table
    // via depthTexDirty_ (see that flag's comment for why a dirty bit, not a size compare). Only
    // consumer today is modules/occlusion's HZB seed pass.
    TextureHandle sceneDepthTexture() override;
    LineHandle createLineMesh(const LineVertex* verts, u32 count) override;
    void drawLines(LineHandle mesh, const f32 world[16]) override;
    void setMeshShaders(bool enabled) override;
    bool meshShadersActive() const override { return msActive_; }
    void setWireframe(bool on) override { wireframe_ = on; }
    void setUnlit(bool on) override { unlit_ = on; }
    void setLineDepth(bool testDepth) override { lineDepth_ = testDepth; }
    void setLineGlow(f32 gain) override { lineGlow_ = gain; }
    void requestCapture(u32 x, u32 y) override { capX_ = x; capY_ = y; captureReq_ = true; captureReady_ = false; }
    bool getCapture(f32 outRGBA[4]) override;
    bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) override;
    // ---- the UI methods ----
    //
    // DELEGATE to whatever aver::rhi::vulkan::IUiBackend has been installed; false/no-op ("no
    // in-window UI") when none has, which is every game build. The seam is
    // modules/rhi.vulkan/include/aver/rhi/vulkan/UiBackend.hpp; the concrete toolkit lives in its
    // own module so nothing about Dear ImGui compiles into anything that merely links the RHI. NO
    // ImGui INCLUDE OR SYMBOL MAY APPEAR IN THIS MODULE -- same rule Aver.RHI.D3D12 keeps.
    //
    // OPEN: which toolkit fills the seam. imgui_impl_vulkan is not vendored, and vendoring it is a
    // real dependency call, not implied by "implement the Vulkan backend".
    bool uiInit(void* windowHandle) override;
    void uiNewFrame() override;
    void uiShutdown() override;
    bool uiActive() const override { return uiBackend_ != nullptr && uiUp_; }
    bool uiWantsMouse() const override;
    bool uiWantsKeyboard() const override;
    // A handle the UI toolkit can draw one of THIS backend's textures through -- the editor's 3D
    // viewport and every asset thumbnail come through here. Zero with no backend installed, which
    // is what it always returned before one could be.
    u64 uiTextureId(TextureHandle t) override;
    // Called by the factory when a texture goes away, so the descriptor the toolkit made for it does
    // not outlive the image view it points at. A leak per destroyed texture otherwise, and the
    // thumbnail cache destroys them constantly.
    void releaseUiTextureId(TextureHandle t);

    // ---- plain (non-override) helpers, mirroring D3D12Device's own public non-interface surface ----
    void present();
    void resize(u32 w, u32 h);
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // ---- accessors for the free helpers and the two friend classes below ----
    const VulkanApi& api() const { return api_; }
    // Opens a dynamic-rendering scope, or joins the one already open. Returns whether it actually
    // opened one -- the caller MUST pass that back to popRenderScope, so an inner no-op open does
    // not close its outer owner's scope. See renderScopeDepth_ for what that cost.
    bool pushRenderScope(VkCommandBuffer cmd, const VkRenderingInfo& ri);
    // `opened` is pushRenderScope's return value. False closes nothing.
    void popRenderScope(VkCommandBuffer cmd, bool opened);
    bool renderScopeActive() const { return renderScopeDepth_ != 0; }

    VkDevice vkDevice() const { return device_; }
    VkPhysicalDevice vkPhysicalDevice() const { return physicalDevice_; }
    const VkPhysicalDeviceMemoryProperties& memoryProperties() const { return memoryProps_; }
    VkQueue graphicsQueue() const { return queue_; }
    u32 graphicsQueueFamily() const { return graphicsQueueFamily_; }
    u32 frameIndexInFlight() const { return frameIndex_; }
    u64 frameSerial() const { return frameSerial_; }
    VkCommandBuffer currentCommandBuffer() const { return commandBuffers_[frameIndex_]; }
    // The timeline value work recorded RIGHT NOW will retire behind -- the Vulkan analog of
    // D3D12ResourceFactory::retireFence(). One past nextTimelineValue_: the CURRENT frame's submit
    // signals nextTimelineValue_ + 1 (see the frame-pacing note on timeline_/nextTimelineValue_
    // below), so anything retired at that value is safe once THIS frame's submit has completed.
    u64 retireFenceValue() const { return nextTimelineValue_ + 1; }
    VkSemaphore timelineSemaphore() const { return timeline_; }
    VkDeviceSize minUboAlignment() const { return minUboAlignment_; }
    // The STORAGE-buffer equivalent, and a separate number: a device may align the two differently,
    // and gInstanceWorlds is a storage buffer bound with a dynamic offset, so it is this one that
    // constrains its ring offsets.
    VkDeviceSize minStorageAlignment() const { return minStorageAlignment_; }
    u32 maxPushConstantsSize() const { return maxPushConstantsSize_; }
    const DeviceCaps& cachedCaps() const { return caps_; }

private:
    void queryCaps();
    bool initAccelerationStructures();
    bool initMeshShaders();
    void dispatchMesh(const GpuMesh& m);
    bool createPipeline();               // the FIXED scene/wire/sky/line PSOs (not the generic factory path)
    bool createSwapchainResources(const SwapchainDesc& d);
    void createRenderTargetViews();
    bool createDepthBuffer();
    bool createMsaaColor();
    // Tears down and rebuilds every target sized off sceneWidth_/sceneHeight_ after renderScale_
    // changes with a swapchain already live. Mirrors D3D12Device::rebuildSceneTargets(): wait for
    // the GPU, drop the depth/MSAA-colour images, recompute the scene size, invalidate the stored
    // viewport sub-rect (vpX_/vpY_/vpW_/vpH_ are stored in SCENE space -- see setViewportRect --
    // and go stale the instant sceneWidth_/sceneHeight_ move), recreate the scene targets, drop
    // the post chain's size-dependent targets (releasePostTargets()), and renotify every
    // registered render feature of the new size.
    void rebuildSceneTargets();
    // Applies a PARKED setRenderScale() at a frame boundary, mirroring D3D12Device::
    // applyPendingRenderScale exactly: called as the very first statement of beginFrame(), before
    // anything records into that frame's command buffer. rebuildSceneTargets() frees and recreates
    // the depth buffer, MSAA target and post chain -- doing that while a command buffer already
    // has those images bound (setRenderScale can be called mid-frame, e.g. from the editor's
    // buildUI() between beginFrame() and endFrame(), or now from AverSR Auto's onUpdate reassert)
    // records a submit against freed resources, which is what actually loses the device at
    // present -- waitForGpu() inside rebuildSceneTargets only drains work already SUBMITTED, not a
    // command buffer still being recorded on the CPU. See aver-render-scale-device-loss.
    void applyPendingRenderScale();
    void waitForGpu();
    // Blocks until the timeline semaphore reaches `value`. False only when the device has been
    // lost -- the Vulkan analog of D3D12's waitFence(); VK_ERROR_DEVICE_LOST is the one VkResult
    // this cannot recover from, same as a D3D12 device-removed HRESULT.
    bool waitTimeline(u64 value);

    // ---- the camera post chain (rhi::PostSettings); field-for-field mirror of D3D12's own,
    // Vulkan-flavoured (see the member list below) ----
    bool createPostPipelines();
    bool createPostTargets();
    void releasePostTargets();
    void runPostChain(VkImage backbufferImage, VkImageView backbufferView, VkFormat backbufferFormat);
    ConstantAllocation postConstants(const void* data, u32 bytes);
    static void toSceneReferred(const f32 display[4], f32 out[4]);

    void notifyRenderTargetsChanged();
    bool ensureViewportTexture();
    void seedSkinTargets();
    void packAtmosphere(const SkyAtmosphere& s);

    // ---- bootstrap ----
    VulkanApi api_{};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalDeviceProps_{};
    VkPhysicalDeviceMemoryProperties memoryProps_{};
    u32 graphicsQueueFamily_ = ~0u;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 maxPushConstantsSize_ = 128;      // queried; 128 is only the GUARANTEED minimum until it is
    VkDeviceSize minUboAlignment_ = 256;  // queried: limits.minUniformBufferOffsetAlignment
    VkDeviceSize minStorageAlignment_ = 256;  // queried: limits.minStorageBufferOffsetAlignment
    // M6: whether VK_EXT_memory_budget actually made it into the enabled device-extension list --
    // set once, right after that list is built, from the same devExts vector every other want*
    // bool in init() reads. videoMemory() also re-checks api_.GetPhysicalDeviceMemoryProperties2
    // itself (see that pointer's own comment), so this flag alone is necessary but not sufficient.
    bool memoryBudgetExt_ = false;

    // ---- surface / swapchain ----
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchainColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    // ---- frame pacing: ONE timeline semaphore replaces D3D12's fence_/fenceValues_/nextFence_
    // wholesale -- vkWaitSemaphores against it is retireFenceValue()'s CPU wait, and every retired-
    // object fence value in RetiredObject is a value on this SAME timeline. Per-swapchain-image
    // BINARY semaphores are still required for the acquire/present handoff itself, which core
    // Vulkan never lets a timeline semaphore do alone (vkAcquireNextImageKHR's semaphore parameter
    // must be binary). ----
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 nextTimelineValue_ = 0;
    u64 frameTimelineValues_[kFrameCount] = {};   // the timeline value that retires each FRAME-IN-FLIGHT slot's last submit
    VkSemaphore imageAvailable_[kFrameCount] = {};
    std::vector<VkSemaphore> renderFinished_;     // one per SWAPCHAIN IMAGE (sized at swapchain creation), not per frame in flight
    u32 frameIndex_ = 0;    // 0..kFrameCount-1, the frame-IN-FLIGHT slot
    // MONOTONIC, unlike frameIndex_, which wraps at kFrameCount and so cannot distinguish "this
    // frame" from "two frames ago in the same slot".
    u64 frameSerial_ = 0;

    // The installed in-window UI toolkit, or null. NON-OWNING -- see IUiBackend's own comment on
    // ownership; Sandbox holds the concrete object and outlives this device's uiShutdown().
    vkb::IUiBackend* uiBackend_ = nullptr;
    bool uiUp_ = false;   // init() succeeded and shutdown() has not run
    // One toolkit descriptor per texture, made on first use. Keyed by handle rather than by view so
    // releaseUiTextureId can find it from what destroyTexture knows.
    std::unordered_map<TextureHandle, u64> uiTexIds_;
    u32 imageIndex_ = 0;    // the acquired swapchain image index this frame

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFrameCount] = {};

    // ---- fixed scene targets ----
    VkImage msaaColor_ = VK_NULL_HANDLE;      VkDeviceMemory msaaColorMemory_ = VK_NULL_HANDLE;      VkImageView msaaColorView_ = VK_NULL_HANDLE;
    VkImage depthBuffer_ = VK_NULL_HANDLE;    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;          VkImageView depthView_ = VK_NULL_HANDLE;
    // MSAA resolve destination. VK_NULL_HANDLE when sampleCount_ == 1, where msaaColor_ is already it.
    VkImage sceneResolved_ = VK_NULL_HANDLE;  VkDeviceMemory sceneResolvedMemory_ = VK_NULL_HANDLE;  VkImageView sceneResolvedView_ = VK_NULL_HANDLE;

    // The generic-RHI wrapper around depthBuffer_ -- see sceneDepthTexture()'s own comment. STABLE
    // across a resize: adoptExternalDepthTexture re-fills this SAME slot rather than pushing a new
    // one, so a future caller that caches the handle across frames never has to notice depthBuffer_
    // was reallocated. Field-for-field mirror of D3D12Device's own depthTexHandle_/depthTexDirty_
    // pair (D3D12Device.cpp:1130-1138).
    TextureHandle depthTexHandle_ = 0;
    // True whenever depthBuffer_ has (re)allocated since depthTexHandle_ was last refreshed --
    // createDepthBuffer() sets this every time it runs (initial creation, resize(), and
    // setSampleCount()'s MSAA-target rebuild all call it); sceneDepthTexture() clears it once it
    // has re-adopted the current depthBuffer_.
    bool depthTexDirty_ = true;

    // ---- fixed scene/line/sky pipelines (built directly via VulkanShaderCompiler +
    // vkCreateGraphicsPipelines, NOT through VulkanResourceFactory's generic cache, mirroring how
    // D3D12Device builds its own rootSig_/pso_ separately) ----
    // HOW MANY vkCmdBeginRendering SCOPES ARE OPEN on the shared command buffer -- zero or one;
    // dynamic-rendering scopes CANNOT NEST. Two independent owners record into the same buffer
    // with no visibility into each other: this class opens a scope around the scene/overlay
    // passes and calls IRenderFeature::scenePass/overlayPass inside it, while VulkanRenderContext
    // opens its own scope around every draw (needed for prePass(), where nothing is open). A
    // feature drawing through the context from inside one of this class's passes used to issue a
    // NESTED vkCmdBeginRendering ("invalid ... inside an active render pass"), whose matching
    // vkCmdEndRendering closed THIS class's scope early -- three validation errors, one cause: the
    // nested-begin error itself, the pass's now-unterminated remaining draws, and the orphaned
    // vkCmdEndRendering. pushRenderScope/popRenderScope are now the ONLY way either owner opens a
    // scope, so the inner request becomes a no-op instead.
    u32 renderScopeDepth_ = 0;
    // The scene pass opens in beginFrame and closes in endFrame, so "did I open it" has to outlive
    // the call that answered it.
    bool sceneScopeOpened_ = false;

    VkDescriptorSetLayout sceneFrameSetLayout_ = VK_NULL_HANDLE;
    // ONE empty descriptor-set layout, for the DEVICE'S LIFETIME, used as the placeholder in every
    // pipeline layout whose set 0 or set 1 goes unused.
    //
    // TRAP: three call sites used to each build their own throwaway pair and destroy it the
    // instant CreatePipelineLayout returned -- but the pipeline layout goes on referencing them, so
    // every pipeline later built from it referenced an already-destroyed VkDescriptorSetLayout.
    // Reported three times (once per site). Keeping one alive removes the whole class of bug.
    VkDescriptorSetLayout emptySetLayout_ = VK_NULL_HANDLE;   // set kVkSetConstants, binding 0 (b0) only
    VkPipelineLayout scenePipelineLayout_ = VK_NULL_HANDLE;        // push constants: b1 object block only
    VkPipeline scenePso_ = VK_NULL_HANDLE, skyPso_ = VK_NULL_HANDLE, wirePso_ = VK_NULL_HANDLE,
               linePso_ = VK_NULL_HANDLE, lineOverlayPso_ = VK_NULL_HANDLE;   // no depth test: editor gizmos on top
    // Separate layout: a mesh-shader pipeline has no vertex input state and pushes the mesh-geometry
    // block too (vb/ib device addresses + triangle count), mirroring D3D12's msRootSig_ split.
    VkPipelineLayout meshPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline meshPso_ = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet sceneFrameSet_[kFrameCount] = {};
    VkBuffer frameCBs_[kFrameCount] = {};     VkDeviceMemory frameCBMemory_[kFrameCount] = {};   u8* frameCBPtr_[kFrameCount] = {};

    DrawBinding drawBinding_{}, defaultDrawBinding_{};
    bool drawBindingIgnored_ = false;   // the "backend's own pipeline drops it" warning, said once

    // ---- same-frame depth prepass (see IDevice::setDepthPrepassEnabled and drawMesh below) --
    // field-for-field mirror of D3D12Device's own trio (D3D12Device.cpp:1311-1320); see that
    // block's comment for why nextDrawPrepassed_ must be auto-consumed rather than sticky.
    bool depthPrepassEnabled_ = false;   // --depth-prepass; OFF reproduces pre-existing behaviour
    bool nextDrawPrepassed_ = false;
    // Draws issued by drawMeshDepthPrepass this frame -- NOT wired to a log line the way D3D12's
    // depthPrepassDrawsLastFrame_/depthPrepassDrawsThisFrame_ pair is (that pair only exists to
    // feed an AVER_INFO on change, which nothing in this backend's endFrame does yet); kept as one
    // counter rather than the D3D12 last/this pair since there is no per-frame log comparison here
    // to drive off it. A future port of that diagnostic can split it into the same pair D3D12 uses.
    u32 depthPrepassDrawsThisFrame_ = 0;

    bool skyEnabled_ = false;
    bool sceneSuppressed_ = false;   // set in beginFrame when a feature suppressed the scene; read in endFrame so the deferred sky draw doesn't run over it
    // The narrower question: did that feature own the WHOLE frame, or only the scene geometry? See
    // IRenderFeature::suppressesWholeFrame. Kept in lockstep with the D3D12 backend deliberately --
    // a sky that appears on one backend and not the other is the worst shape of bug this repo has.
    bool frameSuppressed_ = false;
    // Last logged outcome of beginFrame's scene-claim race, so the warning fires on a CHANGE rather
    // than every frame. Compared, never dereferenced. Mirrors D3D12Device's pair of the same name.
    const IRenderFeature* lastSuppressWinner_ = nullptr;
    u32                   lastSuppressClaimants_ = 0;
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    // See IDevice::setUnlit. Sticky exactly as wireframe_ is.
    bool unlit_ = false;
    bool lineDepth_ = true;
    // 1.0 is exactly the pre-glow behaviour; see IDevice::setLineGlow.
    f32  lineGlow_  = 1.0f;
    std::vector<GpuLineMesh> lineMeshes_;
    PerFrameCB frameCB_{};

    // ---- camera post chain ----
    PostSettings post_{};
    VkImage bloomTex_ = VK_NULL_HANDLE; VkDeviceMemory bloomMemory_ = VK_NULL_HANDLE;
    std::vector<VkImageView> bloomAttachmentViews_;   // one per mip, colour-attachment-capable (loadOp target)
    std::vector<VkImageView> bloomSampledViews_;      // one per mip, sampled (read by the next pass)
    u32 bloomMips_ = 0, bloomW_ = 0, bloomH_ = 0;
    VkBuffer histBuf_ = VK_NULL_HANDLE; VkDeviceMemory histMemory_ = VK_NULL_HANDLE;   // 256-bin histogram, StructuredBuffer-shaped
    VkBuffer expBuf_ = VK_NULL_HANDLE;  VkDeviceMemory expMemory_ = VK_NULL_HANDLE;    // the one adapted-exposure scalar
    bool expSeeded_ = false;
    VkDescriptorPool postDescriptorPool_ = VK_NULL_HANDLE;
    // MEMBERS, NOT FILE-SCOPE STATICS -- these four used to live in anonymous-namespace blocks in
    // VulkanDevice.cpp, so nothing destroyed them: the pools, layout and sampler were still alive
    // at vkDestroyDevice (the validation layer's last-reported leak). Worse: a static outlives the
    // device that filled it, so a second VulkanDevice in one process (device-loss recovery, a
    // backend switch) would find them already non-null and reuse handles from a destroyed device.
    // Set by createPostTargets, consumed once by runPostChain: the frame the targets are built is
    // the one frame where sceneResolved_ has no layout yet.
    bool postTargetsFresh_ = false;
    VkSampler postSampler_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> postSets_;              // allocated from postDescriptorPool_, die with it
    VkDescriptorPool meshGeomPool_[kFrameCount] = {};
    VkDescriptorSetLayout meshGeomLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout postPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline bloomPrefilterPso_ = VK_NULL_HANDLE, bloomDownPso_ = VK_NULL_HANDLE, bloomUpPso_ = VK_NULL_HANDLE;
    VkPipeline compositePso_[2][2] = {};   // indexed [bloom on][auto-exposure on]
    VkPipeline histogramPso_ = VK_NULL_HANDLE, exposurePso_ = VK_NULL_HANDLE;
    ConstantRing postRing_[kFrameCount]{};   // reuses section 5's ConstantRing shape; this is the ONE place outside VulkanRenderContext.cpp that owns a ring, exactly as D3D12Device::postConstants() suballocates its own ring separate from D3D12RenderContext::ringAlloc()
    bool postReady_ = false;
    i64 lastFrameTick_ = 0;
    f32 frameSeconds_ = 1.0f / 60.0f;

    // ---- AverSR ----
    // Null unless a host set one. EVERY branch that matters below tests THIS POINTER, not a quality
    // enum or a build flag -- see setUpscaler's own comment for why that is what makes "no upscaler"
    // and "no AverSR module in the build" the same code path. Field-for-field mirror of
    // D3D12Device's own upscaler_/sceneColorTex_/presentHdrTex_ trio.
    IUpscaler* upscaler_ = nullptr;
    // A factory-created ALIAS of the scene colour, and the reason it has to exist: the scene target
    // (sceneResolved_, or msaaColor_ when sampleCount_ == 1) is a raw VkImage this file allocates
    // directly via createImageCommitted, never through VulkanResourceFactory -- so it has no
    // TextureHandle, and IUpscaler::execute needs one for UpscalerInput::color. A per-frame
    // vkCmdCopyImage fills this. Scene resolution, RGBA16F, sampled-only.
    TextureHandle sceneColorTex_ = 0;
    u32           sceneColorTexW_ = 0, sceneColorTexH_ = 0;
    // AverSR's output: HDR (pre-tonemap) at PRESENT resolution. The upscale runs on radiance and
    // PSComposite then tonemaps an image that is already the right size, so its own resample
    // degenerates to 1:1 and neither the shader nor its pipeline changes for the upscaled path.
    TextureHandle presentHdrTex_ = 0;
    u32           presentHdrTexW_ = 0, presentHdrTexH_ = 0;
    bool          srLogged_ = false;   // the once-only "it really ran" line -- see runPostChain

    // ---- capture ----
    VkBuffer captureBuf_ = VK_NULL_HANDLE; VkDeviceMemory captureMemory_ = VK_NULL_HANDLE;
    bool captureReq_ = false, captureReady_ = false;
    u32 capX_ = 0, capY_ = 0;
    f32 captured_[4] = {0, 0, 0, 0};
    std::vector<u8> frameImage_;
    u32 frameImageW_ = 0, frameImageH_ = 0;

    std::vector<GpuMesh> meshes_;
    struct SkinSeed { MeshHandle dst; MeshHandle src; };
    std::vector<SkinSeed> skinSeeds_;

    // ---- W4: default-heap static meshes (--mesh-heap default) ----
    bool staticMeshDefaultHeap_ = false;   // setStaticMeshHeapDefault's stored value; false = today's Upload-heap behaviour
    // The C-7 "static meshes on the Default heap" line, once for this device's life -- logged on the
    // first mesh actually built that way, not at flag-set time, so a run that sets the flag but never
    // builds a mesh (an empty scene) says nothing.
    bool meshDefaultHeapLogged_ = false;
    // The upload-failure fallback WARN, once for this device's life -- see createMesh's own comment
    // on why a per-mesh warning would flood the log on a big scene load when the failure mode is
    // systemic (out of device-local memory, a driver refusal) rather than per-mesh.
    bool meshDefaultHeapFallbackWarned_ = false;

    u32 width_ = 0, height_ = 0;
    // The scene's OWN render-target size: width_/height_ scaled by renderScale_, rounded, floored
    // at 1; equal to width_/height_ at renderScale_ == 1.0 (the default), so a build that never
    // calls setRenderScale renders byte-for-byte what it did before renderScale_ existed.
    // Present-resolution state (width_/height_, the swapchain, the capture buffer) never reads
    // this pair; only the scene depth/MSAA-colour targets, the scene render-scope's area and
    // default viewport, the post chain's targets, and onRenderTargetsChanged do.
    u32 sceneWidth_ = 0, sceneHeight_ = 0;
    f32 renderScale_ = 1.0f;   // [0.25, 1.0]; see IDevice::setRenderScale
    // PARKED value from a setRenderScale() call made with a swapchain already live: renderScale_
    // itself does not move until applyPendingRenderScale() runs at the top of the next beginFrame.
    // pendingRenderScaleValid_ false means "nothing parked" -- renderScale_ is authoritative, same
    // meaning as D3D12Device::pendingRenderScaleValid_.
    f32  pendingRenderScale_ = 1.0f;
    bool pendingRenderScaleValid_ = false;
    void computeSceneSize() {
        sceneWidth_  = width_  ? static_cast<u32>(std::lround(static_cast<f32>(width_)  * renderScale_)) : 0;
        sceneHeight_ = height_ ? static_cast<u32>(std::lround(static_cast<f32>(height_) * renderScale_)) : 0;
        if (width_  && sceneWidth_  < 1) sceneWidth_  = 1;
        if (height_ && sceneHeight_ < 1) sceneHeight_ = 1;
    }
    // Present-space -> scene-space scaling for the editor's viewport sub-rect (setViewportRect
    // takes physical backbuffer pixels, but the target they address is the smaller SCENE one
    // whenever renderScale_ < 1). Exact identity at renderScale_ == 1.0. Mirrors D3D12Device's
    // scaleToSceneW/H, including the u64 intermediate so the multiply can't overflow at 4K-class
    // present sizes.
    u32 scaleToSceneW(u32 v) const { return width_  ? static_cast<u32>((static_cast<u64>(v) * sceneWidth_)  / width_)  : v; }
    u32 scaleToSceneH(u32 v) const { return height_ ? static_cast<u32>((static_cast<u64>(v) * sceneHeight_) / height_) : v; }
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;   // scene sub-rect; w/h == 0 means full backbuffer
    u32 sampleCount_ = kDefaultSampleCount;
    DeviceCaps caps_{};

    // ---- mesh-shader geometry path ----
    bool msSupported_ = false, msActive_ = false, msRefusalLogged_ = false;
    // ---- ray tracing: VK_KHR_acceleration_structure + VK_KHR_ray_query present ----
    bool rtSupported_ = false;

    // Whether the descriptor-indexing features the ray path's bindless texture table needs were
    // both SUPPORTED and ENABLED at device creation. Recorded there because that is the only place
    // that knows; read by queryCaps to set DeviceCaps::rtBindlessTextures.
    bool bindlessCapable_ = false;

    bool hasSwapchain_ = false;
    bool vsync_ = true;
    bool tearingSupported_ = false;   // VK_PRESENT_MODE_IMMEDIATE_KHR present in the surface's list; fixed for the swapchain's life
    f32 clear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    f32 sceneClear_[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // the same colour as the scene radiance that tonemaps back to it
    std::string adapterName_ = "Vulkan Device";
    // Always false, honestly: there is no OS-shipped software rasteriser the way WARP ships with
    // D3D12 (DeviceDesc::useWarp has no Vulkan analogue). A caller asking for it gets a WARNING at
    // init(), not a silent ignore -- see the contract's note on this.
    bool softwareAdapter_ = false;

    bool viewportToTex_ = false;
    TextureHandle viewportTex_ = 0;
    u32 viewportTexW_ = 0, viewportTexH_ = 0;
    u32 notifiedSamples_ = 0;
    Format notifiedColor_ = Format::Unknown, notifiedDepth_ = Format::Unknown;
    u32 notifiedWidth_ = 0, notifiedHeight_ = 0;

    // ---- generic RHI (render-feature modules) ----
    VulkanResourceFactory* rhiFactory_ = nullptr;
    VulkanRenderContext* rhiContext_ = nullptr;
    std::vector<IRenderFeature*> features_;   // non-owning

    friend class VulkanResourceFactory;
    // Unqualified: VulkanDevice is itself in aver::rhi::vkb, so vkb:: would name vkb::vkb::.
    friend bool installUiBackend(IDevice*, IUiBackend*);
    friend class VulkanRenderContext;
};

// The VkDebugUtilsMessengerEXT callback, installed only when DeviceDesc::enableDebug AND
// VK_EXT_debug_utils is present. Vulkan delivers debug output by CALLBACK, not by a poll like
// D3D12's InfoQueue -- so there is no drainDebugMessages()-shaped method anywhere in this header;
// this free function IS the equivalent, routed straight to AVER_WARN/AVER_ERROR by severity.
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT type,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                      void* userData);

// ================================================================================================
// 12. VulkanResourceFactory (IResourceFactory). DEFINED IN VulkanResourceFactory.cpp.
// ================================================================================================
class VulkanResourceFactory final : public IResourceFactory {
public:
    explicit VulkanResourceFactory(VulkanDevice* dev) : dev_(dev) {}
    ~VulkanResourceFactory() override;

    bool init();
    void selfTest();

    TextureHandle    createTexture(const TextureDesc& d) override;
    BufferHandle     createBuffer(const BufferDesc& d) override;
    ShaderHandle     createShader(const ShaderDesc& d) override;
    PipelineHandle   createGraphicsPipeline(const GraphicsPipelineDesc& d) override;
    PipelineHandle   createComputePipeline(const ComputePipelineDesc& d) override;
    // NOT IMPLEMENTED ON THIS BACKEND YET, and refused out loud rather than stubbed silently.
    // DeviceCaps::rtBindlessTextures already gates every caller, and this backend sets that bit
    // from real descriptor-indexing features -- so reaching these at all means the gate was
    // bypassed, which is worth a log rather than a quiet zero. See the Vulkan parity stage.
    BindlessTableHandle createBindlessTextureTable(u32 capacity) override;
    void destroyBindlessTextureTable(BindlessTableHandle h) override;
    bool setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle t) override;
    u32  bindlessTableCapacity(BindlessTableHandle h) const override;
    BindingSetHandle createBindingSet(const BindingSetDesc& d) override;
    BlasHandle       createBlas(MeshHandle mesh) override;
    TlasHandle       createTlas(u32 maxInstances) override;

    void destroyTexture(TextureHandle h) override;
    void destroyBuffer(BufferHandle h) override;
    void destroyBlas(BlasHandle h) override;
    MeshHandle blasMesh(BlasHandle h) const override;
    BlasHandle blasForMesh(MeshHandle mesh) const override;
    // Destroys every acceleration structure built from `mesh`. Concrete, not part of
    // IResourceFactory -- an implementation detail of VulkanDevice::destroyMesh, mirroring
    // D3D12ResourceFactory::destroyBlasForMesh exactly, including WHY: a BLAS left behind would
    // keep pointing ray tracing at this mesh's freed vertex/index memory.
    void destroyBlasForMesh(MeshHandle mesh);
    void destroyShader(ShaderHandle h) override;
    void destroyPipeline(PipelineHandle h) override;
    void destroyBindingSet(BindingSetHandle h) override;

    void setSrv(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void setUav(BindingSetHandle set, u32 slot, TextureHandle t, u32 mip) override;
    void clearSrv(BindingSetHandle set, u32 slot) override;
    void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) override;
    void setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;
    void setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;

    bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) override;
    bool readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset) override;
    bool textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const override;
    bool textureInfo(TextureHandle h, TextureDesc& out) const override;
    void waitIdle() override;

    // Backs IDevice::uiTextureId -- unused while VulkanDevice does not override the ui* methods
    // (see VulkanDevice's own note); kept so the surface exists the day that changes.
    u64 uiDescriptor(TextureHandle h);
    // The raw handle for VulkanRenderContext's copy/barrier/vertex-bind paths -- mirrors D3D12's
    // D3D12ResourceFactory::bufferResource() exactly, including "null on a bad handle".
    VkBuffer bufferResource(BufferHandle h) const;
    // Wraps a VkImage/VkDeviceMemory this factory did NOT create (VulkanDevice::depthBuffer_/
    // depthMemory_) into an ordinary TextureHandle, so a caller reaches it through
    // setSrv/textureBarrier like any factory-made texture. Concrete rather than on
    // IResourceFactory: the mechanics of ONE specific adoption (mirrors
    // D3D12ResourceFactory::adoptExternalDepthTexture), not a general "wrap anything" entry point.
    // Builds its OWN VkImageView for sampled reads (VK_IMAGE_ASPECT_DEPTH_BIT, kVkDepthFormat --
    // see RhiTexture::externallyOwned for why that view IS this record's to own and destroy)
    // rather than reusing VulkanDevice's own depthView_, which it destroys on its own schedule.
    // `existing`, when non-zero, is REUSED in place rather than allocating a new slot (see
    // VulkanDevice::depthTexHandle_ for why the handle must stay stable across a resize). Returns
    // the (possibly reused) handle, or 0 on a null image or a failed view.
    TextureHandle adoptExternalDepthTexture(VkImage image, VkDeviceMemory memory, u32 width, u32 height,
                                             TextureHandle existing);

    // W4: fills every `dsts[i]` (`count` of them, `sizes[i]` bytes from `srcs[i]`) via ONE
    // HOST_VISIBLE staging buffer and ONE one-shot command-buffer submit -- see
    // VulkanDevice::createMesh's own comment for why this exists (a Default-heap buffer refuses
    // writeBuffer, which requires BufferKind::Upload's persistent mapping) and why it is
    // SYNCHRONOUS (createMesh runs mid-frame too; vkCmdCopyBuffer is illegal inside the
    // dynamic-rendering scope beginFrame opens, and this records into a SEPARATE command buffer via
    // runOneShotCommands, which also means the wait inside it has already retired the staging
    // buffer by the time this returns -- nothing here is left for the caller to free). Every `dst`
    // must already be a valid buffer this factory (or createBufferCommitted) made with
    // TRANSFER_DST usage, which toVkBufferUsage's "everything is everything" policy grants every
    // buffer unconditionally. `what` names the call in any failure log, same convention as
    // createBufferCommitted's own debugName parameter. False on any failure -- the staging buffer
    // is destroyed either way, so the caller never has anything left to clean up from this call.
    bool uploadToDeviceBuffers(const VkBuffer* dsts, const void* const* srcs, const VkDeviceSize* sizes,
                               u32 count, const char* what);

    // ---- table lookups, reachable from VulkanRenderContext (friend) for render-target binding,
    // barriers, and drawMesh's mesh-table resolution ----
    RhiTexture*       texture(TextureHandle h);
    const RhiTexture* texture(TextureHandle h) const;
    RhiBuffer*        buffer(BufferHandle h);
    const RhiBuffer*  buffer(BufferHandle h) const;
    RhiShader*        shader(ShaderHandle h);
    RhiPipeline*      pipeline(PipelineHandle h);
    RhiBindingSet*    bindingSet(BindingSetHandle h);
    RhiBlas*          blas(BlasHandle h);
    RhiTlas*          tlas(TlasHandle h);

    // The pipeline-layout cache, keyed by sameLayout(). Builds (and caches) every
    // VkDescriptorSetLayout/VkPipelineLayout a PipelineLayout needs, INCLUDING the once-only
    // immutable-samplers set. See section 4's full scheme write-up.
    const DescriptorLayoutEntry* descriptorLayout(const PipelineLayout& layout, bool mesh, bool instanced = false);
    // The shared set-4 layout every instanced pipeline uses. Created on first use, owned by this
    // factory, destroyed with it.
    VkDescriptorSetLayout instanceSetLayout();
    // The table-SHAPE cache underneath descriptorLayout() -- see TableShapeEntry's own comment on
    // why this is a separate, finer-grained cache createBindingSet() shares with it.
    VkDescriptorSetLayout tableSetLayout(u32 srvCount, u32 uavCount,
                                         const SlotKind* srvKinds, const SlotKind* uavKinds);
    VkSampler getOrCreateSampler(const SamplerDesc& d);

    // The timeline value work recorded right now retires behind. Mirrors D3D12ResourceFactory::
    // retireFence() exactly (same forwarding to VulkanDevice::retireFenceValue()).
    u64 retireFence() const;
    // Queues `destroyFn` to run once `retireFence()`'s value has passed on the timeline semaphore.
    void retire(std::function<void()> destroyFn);
    // Walks the retired queue, running and popping every entry whose fence has passed. Called at
    // the top of every creation/destruction entry point, exactly matching D3D12's own collect()
    // call sites.
    void collect();

private:
    // Fills a freshly created image from TextureDesc::initialData. The image must already be in
    // TRANSFER_DST_OPTIMAL; on success it has been transitioned to `d.initialState` and the GPU has
    // finished (a fence wait, not just a barrier -- the staging buffer is freed right after).
    bool uploadInitialData(VkImage image, const VkImageCreateInfo& ci, const TextureDesc& d, u32 mips);
    // Writes a valid, dimension-matched descriptor into every slot a BindingSetDesc declared but
    // the caller never wrote.
    //
    // WHY IT STAYS: the engine null-fills UNCONDITIONALLY as self-imposed policy, not because any
    // device requires it -- docs/VULKAN.md's audit of the D3D12 original says the same, and
    // matching it here keeps the two backends identical, worth more than the descriptors saved.
    //
    // CORRECTION: this used to justify the policy by saying descriptor_indexing's
    // descriptorBindingPartiallyBound couldn't be assumed present. That no longer holds -- it and
    // runtimeDescriptorArray/shaderSampledImageArrayNonUniformIndexing/descriptorIndexing are all
    // CORE VkPhysicalDeviceVulkan12Features fields, and this backend already hard-requires
    // VK_API_VERSION_1_3 plus four sibling 1.2/1.3 bits with no fallback (bufferDeviceAddress,
    // timelineSemaphore, dynamicRendering, synchronization2 -- queryRequiredFeatures,
    // VulkanDevice.cpp:596-603). So a bindless array here would be a feature-bit query, not an
    // extension gamble; the engine's FL 11_0 minimum (docs/MINIMUM_SPECS.md) is D3D12-only, since
    // that hardware can't reach Vulkan 1.3 and never runs this backend at all.
    void nullFill(const RhiBindingSet& s);

    // Writes one descriptor into the CURRENT frame's ring slot and records it so the other ring
    // slots can be brought up to date later. See RhiBindingSet for why the ring exists.
    void writeBindingSlot(RhiBindingSet& s, bool isUav, u32 slot, const BindingSlotState& st);
    // The set to bind THIS frame, replaying any writes it has missed first. The one place a caller
    // should get a VkDescriptorSet out of an RhiBindingSet.
    VkDescriptorSet bindingSetForFrame(RhiBindingSet& s);

    // The VkShaderModule to build a pipeline of THIS layout from -- s.module when the layout makes
    // no cbuffer the shader declares into root constants, otherwise a variant compiled with
    // patchPushConstantSlots applied, created on first use and cached in s.variants. Null on a
    // compile failure, which the caller must treat as pipeline creation failing.
    // `outSpirv`, when given, receives the SPIR-V the returned module was built from -- which is
    // what reflectTableSlotKinds has to read, NOT RhiShader::spirv. See that function.
    VkShaderModule moduleForLayout(RhiShader& s, const PipelineLayout& layout, bool mesh, bool instanced,
                                   const std::vector<u32>** outSpirv = nullptr);

    VulkanDevice* dev_;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;   // the ONE pool every binding set + every fixed/post/feature descriptor set allocates from; created with FREE_DESCRIPTOR_SET_BIT so destroyBindingSet can actually free its set (deferred through retire()/collect(), same as every other destroy here)

    std::vector<RhiTexture>    textures_;
    std::vector<RhiBuffer>     buffers_;
    std::vector<RhiShader>     shaders_;
    // std::deque, NOT std::vector -- same reason and same defect as its D3D12 twin. VulkanRenderContext
    // caches a raw `const RhiPipeline* pipe_` from &pipelines_[h-1], so a push_back that reallocates
    // while a command buffer is open leaves it dangling. FOUR growth sites here vs D3D12's two.
    // Measured on the D3D12 side (this backend can't be run): an ordinary 60-frame editor session
    // relocates the table twelve times, once while a pipeline is bound -- a container issue, not a
    // D3D12-specific one, fixed here too even though this backend is OFF by default.
    // Compile-verified with -DAVER_RHI_VULKAN=ON; NOT run, because this backend has no editor UI.
    std::deque<RhiPipeline>    pipelines_;
    std::vector<RhiBindingSet> bindingSets_;
    std::vector<RhiBlas>       blases_;
    std::vector<RhiTlas>       tlases_;
    std::vector<DescriptorLayoutEntry> descriptorLayouts_;
    std::vector<TableShapeEntry>       tableShapes_;
    std::vector<SamplerCacheEntry>     samplers_;
    VkDescriptorSetLayout instanceSetLayout_ = VK_NULL_HANDLE;   // lazily built by instanceSetLayout(); owned here, destroyed in the shutdown sweep
    std::vector<RetiredObject>         retired_;

    friend class VulkanRenderContext;
    friend class VulkanDevice;
};

// ================================================================================================
// 13. VulkanRenderContext (IRenderContext). DEFINED IN VulkanRenderContext.cpp.
// ================================================================================================
class VulkanRenderContext final : public IRenderContext {
public:
    VulkanRenderContext(VulkanDevice* dev, VulkanResourceFactory* res) : dev_(dev), res_(res) {}
    // The context OWNS Vulkan memory -- the per-frame constant ring and the shared zero CBV -- and
    // for a long time had no destructor at all, so all three buffers and their allocations were
    // still alive at vkDestroyDevice. IRenderContext's destructor is virtual and VulkanDevice's own
    // destructor deletes this through the base pointer, so the only thing that was missing was
    // this declaration.
    ~VulkanRenderContext() override;

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
    // ONE vkCmdDrawIndexed WITH instanceCount, reading per-instance world matrices out of a
    // StructuredBuffer bound at set kVkSetInstances -- the twin of D3D12RenderContext::
    // drawMeshInstanced, which does the same through a root SRV at a raw GPU virtual address.
    //
    // NOT AN OPTIMISATION: an earlier note claimed the inherited base-class fallback (one
    // setConstants(kObjectConstantRegister) + drawMesh per instance) was already correct here.
    // False -- the pipelines Voxi calls this with (VSShadowInstanced/VSGiShadowInstanced) read
    // gInstanceWorlds[instanceID] and never gWorld, so the fallback set a constant the shader
    // ignores and drew every instance at instanceID 0: shadow cascades and the GI-only shadow map
    // were drawn from undefined transforms on Vulkan.
    //
    // A DESCRIPTOR, NOT a push-constant device address as an earlier design proposed: every buffer
    // here does carry a VkDeviceAddress, but the HLSL is SHARED with D3D12 and declares
    // StructuredBuffer<float4x4> gInstanceWorlds : register(tN) -- a descriptor by construction. A
    // device-address design would fork text D3D12 compiles unchanged, exactly what this backend's
    // explicit -fvk-bind-register map exists to avoid.
    void drawMeshInstanced(MeshHandle mesh, const f32* worlds, u32 instanceCount) override;
    void dispatchMeshFor(MeshHandle mesh) override;
    void dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) override;
    void dispatch(u32 gx, u32 gy, u32 gz) override;
    void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes, u64 dstOffset, u64 srcOffset) override;
    void copyTexture(TextureHandle dst, TextureHandle src) override;
    void copyTextureToBuffer(BufferHandle dst, u64 dstOffset, TextureHandle src, u32 mip) override;
    void copyBufferToTexture(TextureHandle dst, u32 mip, BufferHandle src, u64 srcOffset) override;
    void drawFullscreen() override;
    void setVertexBuffer(BufferHandle b, u32 stride) override;
    // Binds a caller-owned index buffer. R32Uint ONLY -- mirrors what D3D12RenderContext::
    // setIndexBuffer actually enforces (rejects anything else), NOT what its own header comment
    // aspirationally says ("R16Uint-equivalent"); see the contract scout's finding on this exact
    // ambiguity. Matching the D3D12 backend's REAL behaviour, not its comment, is what keeps the
    // two backends' rejected-input behaviour identical.
    void setIndexBuffer(BufferHandle b, Format indexFormat) override;
    void drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) override;
    void buildBlas(BlasHandle blas) override;
    void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    void textureBarrier(TextureHandle t, ResourceState from, ResourceState to, u32 subresource) override;
    void bufferBarrier(BufferHandle b, ResourceState from, ResourceState to) override;
    void uavBarrierTexture(TextureHandle t) override;
    void uavBarrierBuffer(BufferHandle b) override;
    // DEBUG LABELS ONLY -- NO GPU TIMING BEHIND THESE, unlike D3D12RenderContext's own pushMarker/
    // popMarker (D3D12Device.cpp), which also issue a timestamp on each side and feed a per-node GPU
    // timing tree (GpuSpan/GpuAccum there). See this pair's own long comment in
    // VulkanRenderContext.cpp for what a real parity implementation would need and why it is a
    // declared gap here rather than a silent one.
    void pushMarker(const char* label) override;
    void popMarker() override;

    // Suballocates transient upload memory from THIS frame's ring, at dev_->minUboAlignment().
    // PUBLIC: VulkanDevice's own post-chain methods (VulkanDevice.cpp) call this SAME allocator for
    // their per-pass constants, exactly as D3D12Device::postConstants() reuses
    // D3D12RenderContext::ringAlloc's identical logic rather than duplicating it -- except here the
    // two are genuinely the SAME function across the TU boundary, not merely the same logic twice.
    ConstantAllocation ringAlloc(const void* data, u32 bytes);

private:
    // Gives every descriptor-CBV slot the current pipeline declares (constantDwords[k] == 0, always
    // including b0) a valid dynamic-offset binding before the next draw, so no draw can read an
    // unset one -- the Vulkan analog of D3D12RenderContext::bindDeclaredRootCbvs. Slot 0 (b0) is
    // ALWAYS written here from VulkanDevice's current-frame PerFrameCB, with no caller action, per
    // section 4's scheme.
    void bindDeclaredDescriptors(const RhiPipeline* p);
    // Binds the sticky per-draw state (table 1 + its b2 block), if the current pipeline declared
    // anywhere to put it. Mirrors D3D12RenderContext::applyDrawBinding.
    void applyDrawBinding();
    // dev_->currentCommandBuffer() -- the one place every method above reaches the live command
    // buffer through, so a future frame-pacing change touches this one line, not every override.
    VkCommandBuffer cmd() const;

    VulkanDevice* dev_;
    VulkanResourceFactory* res_;
    const RhiPipeline* pipe_ = nullptr;
    // Shared zero-filled UBO for declared-but-unsupplied CBV slots -- the Vulkan analog of D3D12's
    // zeroCB_/zeroCbv().
    VkBuffer zeroCB_ = VK_NULL_HANDLE;
    VkDeviceMemory zeroCBMemory_ = VK_NULL_HANDLE;

    ConstantRing ring_[kFrameCount]{};
    // The per-instance world matrices, ringed exactly like ring_ above and for the same reason, but
    // a SEPARATE buffer because the constant ring is created VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT and a
    // StructuredBuffer is a STORAGE buffer. Same growth and same epoch-based reset.
    ConstantRing instanceRing_[kFrameCount]{};
    // Suballocates from instanceRing_ for one draw. Returns a null buffer on overflow, which the
    // caller must treat as "skip the draw", never as offset 0.
    ConstantAllocation instanceAlloc(const void* data, u32 bytes);
    // instanceRing_'s own frame epoch. NOT shared with ringEpoch_: both rings reset their bump
    // cursor on the first allocation of a new frame, and one shared epoch would let whichever ring
    // allocated first consume the transition, leaving the other never reset.
    u64 instanceEpoch_ = ~0ull;
    u64 instanceOverflowEpoch_ = ~0ull;   // one overflow message per frame, same as ringOverflowEpoch_
    u64 ringEpoch_ = ~0ull;            // resets the ring when dev_'s timeline moves to a new frame
    u64 ringOverflowEpoch_ = ~0ull;    // the epoch an overflow was last reported in, once per frame not once per call

    BindingSetHandle drawSet_ = 0;
    u8  drawConstants_[kMaxDrawConstantBytes] = {};
    u32 drawConstantBytes_ = 0;

    // ---- caller-owned geometry path (setVertexBuffer/setIndexBuffer/drawIndexed) ----
    VkBuffer boundVertexBuffer_ = VK_NULL_HANDLE;
    u32 boundVertexStride_ = 0;
    VkBuffer boundIndexBuffer_ = VK_NULL_HANDLE;
};

} // namespace aver::rhi::vkb
