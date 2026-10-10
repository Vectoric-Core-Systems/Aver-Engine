// Internal shared header for the Vulkan RHI backend, included only by .cpp files in this
// directory (no include/, per house rule 3: no backend types outside the RHI -- matches D3D12's
// d3d12.h). Lives in aver::rhi::vkb to avoid colliding with D3D12Device.cpp's anonymous-namespace
// names of the same shape (RhiTexture, hrOk, ...). Four TUs share this header (vs D3D12's one
// 5,423-line TU), so anything shared between them needs external linkage.
//
// FILE MAP -- which .cpp defines what; each declaration below has exactly one owner.
//
//   VulkanDevice.cpp: VulkanSwapchain (all methods); VulkanDevice -- every override, plus init(),
//     queryCaps(), initAccelerationStructures(), initMeshShaders(), dispatchMesh(), createPipeline()
//     (fixed scene/wire/sky PSOs only -- lines are aver::rhi::EditorLines, built on the generic
//     factory, not a fixed PSO here), createSwapchainResources/createRenderTargetViews/
//     createDepthBuffer/createMsaaColor, waitForGpu/waitTimeline, present/resize,
//     notifyRenderTargetsChanged, ensureViewportTexture, seedSkinTargets, packAtmosphere,
//     toSceneReferred, the camera post chain (createPostPipelines/createPostTargets/
//     releasePostTargets/runPostChain/postConstants), debugMessengerCallback(),
//     loadGlobalApi/loadInstanceApi/loadDeviceApi/unloadApi (first file needing an instance/device
//     to resolve API pointers; elsewhere reaches the table via VulkanDevice::api()),
//     createVulkanDevice() (matches the forward decl in modules/rhi/src/RHI.cpp).
//
//   VulkanShaderCompiler.cpp: VulkanShaderCompiler (init/usingDxc/compile, the DXC `-spirv`
//     wrapper, its own TU since both VulkanDevice.cpp and VulkanResourceFactory.cpp need it) and
//     vulkanShaderCompiler(), the process-wide singleton accessor.
//
//   VulkanResourceFactory.cpp: VulkanResourceFactory -- every override plus the
//     descriptor/pipeline-layout cache (descriptorLayout(), tableSetLayout()), sampler cache
//     (getOrCreateSampler()), pushConstantLayout(), nullFill(), uploadInitialData(), and the
//     deferred-destruction machinery (retireFence/retire/collect); also
//     createBufferCommitted/createImageCommitted/destroyBufferCommitted/destroyImageCommitted
//     (D3D12's CreateCommittedResource analog); owned here since this file already owns
//     allocation policy and every other raw buffer/image caller shares it.
//
//   VulkanRenderContext.cpp: VulkanRenderContext -- every override, plus ringAlloc(),
//     bindDeclaredDescriptors(), applyDrawBinding(), cmd().
//
// Every free INLINE helper below (format/state/filter/compare/vertex-layout conversions, vkOk,
// findMemoryType, sameLayout/sameSampler, storeDrawBinding, setVkObjectName) lives here, each
// being a pure function of its arguments -- the multi-TU equivalent of D3D12Device.cpp's
// anonymous-namespace free functions of the same shape.
#pragma once

// Platform/loader config; also set as PRIVATE compile definitions in this module's CMakeLists.txt
// (the #ifndef guards just make this header self-sufficient). No vulkan-1.lib is installed (SDK
// deliberately absent -- see third_party/vulkan-headers/README.md), so VK_NO_PROTOTYPES: nothing
// here may call an unprefixed vkFoo() expecting the linker to resolve it. Every entry point is a
// function pointer in VulkanApi below, resolved at runtime from the driver's vulkan-1.dll
// (System32, not the SDK) via LoadLibraryW + GetProcAddress, starting from vkGetInstanceProcAddr.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>   // vendored at third_party/vulkan-headers/include, v1.3.296, Apache-2.0

#include "aver/rhi/RHI.hpp"
#include "aver/rhi/EditorLines.hpp"
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

constexpr u32 kFrameCount = 2;   // frames in flight, and also the backbuffer count since this backend requests exactly this many swapchain images; mirrors D3D12Device.cpp's kFrameCount
constexpr u32 kDefaultSampleCount = 4;

// SCENE colour/depth formats IDevice::backbufferFormat()/depthFormat() report -- NOT the real
// swapchain surface format (negotiated separately, VulkanDevice::swapchainFormat_); fixed for
// process lifetime, so every feature pipeline is built against whatever these report. Mirrors
// D3D12's kSceneColorFormat/kDepthFormat in spirit.
constexpr VkFormat kVkSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kVkDepthFormat      = VK_FORMAT_D32_SFLOAT;

// Minimum requirement, requested as VkApplicationInfo::apiVersion; failure makes init() return
// false and createVulkanDevice() report unavailable. Chosen because synchronization2,
// dynamic_rendering, buffer_device_address and timeline_semaphore -- all structurally required by
// this backend's frame-pacing and render-target model -- are CORE at 1.3, with no fallback for a
// 1.2-only driver implemented yet (would need VK_KHR_synchronization2 + VK_KHR_dynamic_rendering).
constexpr u32 kRequiredApiVersion = VK_API_VERSION_1_3;

// ---- required / optional extension name lists -------------------------------------------------
inline constexpr const char* kRequiredInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
};
// Requested only when DeviceDesc::enableDebug; absence is not fatal (no validation layers by
// default; without VK_EXT_debug_utils, object naming/labels become silent no-ops, same shape as
// pushMarker/popMarker's inert IRenderContext defaults).
inline constexpr const char* kOptionalInstanceExtensions[] = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
};
inline constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};
// Each optional extension gates exactly one DeviceCaps bit (or internal support flag) honestly to
// false/0 when absent -- see DeviceCaps's field comments and RHI.hpp:92's CapsOverride contract
// ("every field only ever reduces"). Requested if present, never required.
//
// EXCEPTION: VK_EXT_MEMORY_BUDGET at the tail gates no DeviceCaps bit, only
// VulkanDevice::videoMemory() (M6) -- its absence only makes that reporting call return
// VideoMemoryInfo::supported == false, honestly. Enumerated here anyway: one enumerate, one
// enable list, one place deciding what's actually on the device.
inline constexpr const char* kOptionalDeviceExtensions[] = {
    VK_EXT_MESH_SHADER_EXTENSION_NAME,                  // -> DeviceCaps::meshShaderTier
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,       // -> DeviceCaps::rayTracingTier
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,     // dependency of acceleration_structure
    VK_KHR_RAY_QUERY_EXTENSION_NAME,                    // -> DeviceCaps::rayTracingTier (inline
                                                         //    queries only; D3D12 only builds DXR
                                                         //    1.1 inline queries here, so
                                                         //    ray_tracing_pipeline isn't needed)
    VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,   // -> DeviceCaps::conservativeRaster
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,                // -> videoMemory() only (M6); no DeviceCaps
                                                         //    bit -- see above
};

// ================================================================================================
// 2. The Vulkan API: every entry point this backend calls, as a function-pointer table. No
//    prototype here is ever linked; every one is resolved at runtime by loadGlobalApi/
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
    // Core 1.1, resolved alongside GetPhysicalDeviceProperties2/Features2 above; MAY BE NULL on
    // a driver that reports 1.1 but doesn't export it, so
    // VulkanDevice::videoMemory() checks this pointer directly rather than trusting
    // memoryBudgetExt_ alone. Chains VkPhysicalDeviceMemoryBudgetPropertiesEXT (M6) for live
    // budget/usage per heap; plain GetPhysicalDeviceMemoryProperties stays the source for heap
    // COUNT/FLAGS/SIZE (static, already cached in memoryProps_).
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

    // ---- acceleration structures (KHR_acceleration_structure + KHR_deferred_host_operations); all optional, gated on kOptionalDeviceExtensions ----
    PFN_vkGetAccelerationStructureBuildSizesKHR GetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCreateAccelerationStructureKHR CreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR DestroyAccelerationStructureKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR CmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddressKHR = nullptr;
};

bool loadGlobalApi(VulkanApi& api);   // loads vulkan-1.dll, resolves vkGetInstanceProcAddr + every global entry point; false (logged) if either is missing -- unrecoverable
bool loadInstanceApi(VulkanApi& api, VkInstance instance, bool debugUtilsAvailable);   // resolves instance-level entries, incl. debug_utils when `debugUtilsAvailable`; never fails outright, an optional fn just stays null
bool loadDeviceApi(VulkanApi& api, VkDevice device, const std::vector<std::string>& deviceExtensions);   // resolves device-level entries, gating mesh_shader/acceleration_structure on `deviceExtensions`
void unloadApi(VulkanApi& api);   // frees the module; every PFN_ left dangling by design, only called from VulkanDevice's destructor tail

// ================================================================================================
// 3. Small pure helpers -- format/state/filter/compare/vertex-layout conversions, VkResult
//    checking, memory-type selection. Inline: each is a pure function of its arguments, so there
//    is no ownership question to assign to one .cpp.
// ================================================================================================

// Logs and returns false on a failed VkResult. Vulkan analog of D3D12Device.cpp's hrOk.
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
        case Format::R32Typeless:    return VK_FORMAT_D32_SFLOAT;   // Vulkan images keep ONE fixed format for life; sampled via a SEPARATE aspect-qualified view of the same image, not a format change -- simpler than D3D12's trick, not harder (architecture scout finding #6)
        case Format::RG16F:          return VK_FORMAT_R16G16_SFLOAT;   // RG16F and RGB10A2Unorm (next case) were both missing from this switch (not the enum); no default + no /W4 warning made toVkFormat silently return UNDEFINED here
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
        case VK_FORMAT_D32_SFLOAT:            return Format::D32Float;   // R32Typeless round-trips as D32Float -- correct wherever this is asked of a depth image
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
// The format a depth image (R32Typeless/D32Float) is SAMPLED through: a separate view of the SAME
// VkImage/VkFormat, VK_IMAGE_ASPECT_DEPTH_BIT -- unlike D3D12, only the aspect/layout differ.
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
// state; buffers have none), and VkAccessFlags2/VkPipelineStageFlags2 stated explicitly on every
// barrier. Built against VK_KHR_synchronization2's Vk{Image,Buffer}MemoryBarrier2 (core at 1.3, so
// no separate extension check beyond kRequiredApiVersion).
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
        // VertexBuffer/GeometryRead are buffer-only states in this engine's actual usage (D3D12's
        // own VertexBuffer/GeometryRead map to VERTEX_AND_CONSTANT_BUFFER, a BUFFER state); an
        // image should never be asked for either, but a defined fallback beats undefined behaviour.
        case ResourceState::VertexBuffer:
        case ResourceState::GeometryRead:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        // TERMINAL, per RHIResources.hpp:89 -- never a valid barrier argument either direction.
        // VulkanRenderContext::textureBarrier must reject this before reaching the barrier call,
        // exactly as the D3D12 debug-shadow's rejectAsState() does.
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
        // Every geometry reader at once -- input assembler, manual vertex fetch, AS build. Mirrors
        // D3D12's combined VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE (RHIResources.hpp:80-87).
        case ResourceState::GeometryRead:
            return {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR};
        case ResourceState::RenderTarget:
        case ResourceState::DepthWrite:
            break;   // not meaningful for a buffer; falls through to the Common/undefined default
        // TERMINAL -- see toVkImageBarrierInfo's identical case.
        case ResourceState::AccelerationStructure:
            return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
        case ResourceState::Common:
            break;
    }
    // Common is D3D12's implicit sync point (promotion/decay), so on a buffer it means "every prior and
    // later access": NONE here made copy -> Common -> shader-read carry no dependency at all, and a
    // dispatch read the buffers before the upload copy landed (NeuralGpuParityTest's Vulkan row).
    return {VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
}

// First memory type in `memProps` matching every bit of `typeBits` whose propertyFlags contain
// every bit of `required`. ~0u ("not found") on failure -- treat like a null
// CreateCommittedResource, never dereference into an index.
inline u32 findMemoryType(const VkPhysicalDeviceMemoryProperties& memProps, u32 typeBits,
                          VkMemoryPropertyFlags required) {
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return ~0u;
}

// ---- vertex input: POSITIONAL locations, not semantic names ------------------------------------
// SPIR-V vertex inputs carry only a numeric Location; DXC assigns it by INPUT-STRUCT DECLARATION
// ORDER, which already matches the shared prelude's VSIn (pos, nrm, uv0) / kMeshInputLayout's
// D3D12 order, so no [[vk::location(N)]] needed. UNVERIFIED against an actual DXC -spirv
// disassembly -- read from source, not from a compile.
inline void meshVertexInputState(VkVertexInputBindingDescription& outBinding,
                                 VkVertexInputAttributeDescription (&outAttribs)[3]) {
    outBinding = VkVertexInputBindingDescription{0, static_cast<u32>(sizeof(MeshVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    outAttribs[0] = VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(MeshVertex, px))};
    outAttribs[1] = VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(MeshVertex, nx))};
    outAttribs[2] = VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT,    static_cast<u32>(offsetof(MeshVertex, u))};
}
// Caller-owned VertexLayout -> Vulkan attributes at positional locations 0..attribCount-1, in
// declaration order (only array position matters in SPIR-V). Returns the count actually written;
// an unusable Format is skipped and logged, like D3D12's buildInputLayout.
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

// True when two pipeline layouts declare identical bindings/constants/samplers -- Vulkan-side
// twin of D3D12Device.cpp's sameLayout/sameSampler, used by the descriptor/pipeline-layout cache.
inline bool sameSampler(const SamplerDesc& a, const SamplerDesc& b) {
    return a.filter == b.filter && a.address == b.address && a.compare == b.compare && a.maxLod == b.maxLod;
}
inline bool sameLayout(const PipelineLayout& a, const PipelineLayout& b) {
    if (a.srvCount != b.srvCount || a.uavCount != b.uavCount || a.samplerCount != b.samplerCount) return false;
    if (a.srvCount1 != b.srvCount1 || a.uavCount1 != b.uavCount1) return false;
    if (a.constantSpace != b.constantSpace || a.samplerSpace != b.samplerSpace) return false;   // in the key even though a non-zero space is REFUSED (descriptorLayout), so a later layout differing only there can't skip that refusal via a cached entry
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

// Sets a debug object name via VK_EXT_debug_utils when loaded; silent no-op otherwise, same
// shape as IRenderContext::pushMarker/popMarker's inert defaults.
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
//   set kVkSetTable0 (0)    = table 0: SRV t0..t(srvCount-1) at binding 0..srvCount-1, UAV
//                             u0..u(uavCount-1) at binding kVkUavBindingBase..+uavCount-1.
//   set kVkSetTable1 (1)    = table 1, same scheme, restarted in its own set (srvCount1/uavCount1).
//   set kVkSetConstants (2) = one UNIFORM_BUFFER_DYNAMIC binding per slot k where
//                             constantDwords[k] == 0 (a "root CBV"). Slot 0 (b0, PerFrame) is
//                             ALWAYS zero (RHIResources.hpp:209) and ALWAYS bound here on every
//                             pipeline -- VulkanRenderContext writes its dynamic offset on every
//                             setPipeline() (RHIResources.hpp:340-341).
//   set kVkSetSamplers (3)  = s0..s(samplerCount-1), IMMUTABLE (pImmutableSampler, matching
//                             D3D12's static samplers), allocated/bound ONCE per DescriptorLayoutEntry.
//
//   PUSH CONSTANTS carry every root 32-bit CONSTANT (constantDwords[k] != 0): always
//   kObjectConstantRegister (b1, 32 dwords) plus any feature-declared slot, plus -- mesh/
//   amplification only -- the mesh-geometry block (vertex/index device addresses via
//   VK_KHR_buffer_device_address, the direct translation of D3D12's raw root-SRV bind, plus the
//   count block at kMeshGeometryConstantRegister/b5). Exact byte offsets in
//   PushConstantLayout/pushConstantLayout(); VulkanResourceFactory and VulkanRenderContext must
//   agree on them byte-for-byte.
constexpr u32 kVkSetTable0    = 0;
constexpr u32 kVkSetTable1    = 1;
constexpr u32 kVkSetConstants = 2;
constexpr u32 kVkSetSamplers  = 3;
constexpr u32 kVkSetInstances = 4;   // PER-INSTANCE WORLD MATRICES (GraphicsPipelineDesc::instanced), one StructuredBuffer<float4x4>; own set because the HLSL declares it at register t(declaredSrvCount), one past the layout's SRVs, else landing on UAV slot 0 in set 0. Absent from a non-instanced pipeline.
constexpr u32 kVkDescriptorSetCount = 5;   // table0, table1, constants, samplers, instances
constexpr u32 kVkUavBindingBase = kMaxBindingSlots;   // UAV bindings start here so a growing srvCount never renumbers an already-cached UAV binding; kMaxBindingSlots (25) bounds one set's slot count so SRV/UAV ranges never collide

// Moves the shared prelude's per-frame constant buffer to the descriptor set this backend
// actually binds it in. MUST be applied to any HLSL including sharedShaderPrelude() before
// compiling.
//
// WHY: the prelude (shared_prelude.hlsl, shared with D3D12) declares the per-frame cbuffer with no
// register space, so DXC maps it to SPIR-V set 0 instead of kVkSetConstants -- every shader
// touching gViewProj named a descriptor its own pipeline layout didn't contain. Matches on the
// DECLARATION (`cbuffer PerFrame`), not the register, since the prelude emits the register from a
// macro rather than spelling it in HLSL (matching on the register broke silently once already,
// uncaught because AVER_RHI_VULKAN was OFF in every shipped configuration); expandCbufferRegisters
// below expands that macro to plain `register(bN)` first so the text search still works.
// [[vk::binding(binding, set)]] overrides only the SPIR-V placement, so this stays a Vulkan-only
// edit on the assembled copy this module owns -- same approach as patchPushConstants in
// VulkanDevice.cpp.
//
// MEASURED TRAP: a wrong-set shader gave AMD's integrated GPU a diagnosable
// VK_ERROR_INVALID_SHADER_NV, but the discrete card's LLPC called abort() instead (0xC0000409, no
// message) -- same defect, two very different symptoms.
//
// Rewrites `register(AVER_CB_JOIN(b, AVER_<X>_CB))` to the plain `register(bN)` it expands to.
// THE ONE PLACE THE MACRO STOPS: three parsers here read a cbuffer's register straight out of
// HLSL text (patchPerFrameSet below, patchPushConstants in VulkanDevice.cpp,
// findCbufferBlock/findNextCbuffer in VulkanResourceFactory.cpp) and expect a literal register,
// not a macro -- RHIShaders.cpp now emits these numbers as macros (shaderConstantsHlsl) instead of
// spelling them, so expanding once, here, fixes all three. SAFE/idempotent: substitutes the
// preprocessor's own answer for the same constants that defined the macros.
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
    const std::string needle = "cbuffer PerFrame";   // matches only at the head of a line: several files in this tree discuss `cbuffer PerFrame` in prose, and patching one of those would silently leave the binding on set 0 with no error
    std::size_t pos = src.find(needle);
    while (pos != std::string::npos) {
        // Only whitespace may precede it on its line.
        std::size_t lineStart = src.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        bool blank = true;
        for (std::size_t i = lineStart; i < pos; ++i)
            if (src[i] != ' ' && src[i] != '\t') { blank = false; break; }
        const std::size_t after = pos + needle.size();   // whole-word test too (findCbufferBrace in VulkanDevice.cpp has one, this didn't): without it `cbuffer PerFrameExtra` would be patched as PerFrame, misplacing both blocks; no such cbuffer exists today, but the next one added would surface as a driver abort()
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

// Handles only b0: runs at createShader time, before any PipelineLayout exists, and b0
// (kEngineFrameConstantRegister) is the one register every layout treats identically. Everything
// else is patchCbuffersForLayout's job, at PIPELINE creation once a layout exists to consult (an
// earlier note called a general version impossible, which held only with no layout in hand).
//
// TRAP: blindly annotating every cbuffer into kVkSetConstants once moved SkinParams from set 0 to
// set 2 while SkinningPass declares constantDwords[3] = 4 (b3 is push constants) -- a block can't
// be both, and AMD's discrete driver reported nothing (same silent LLPC abort()). Trust
// --debug-layer over the shader text.

// Push-constant byte layout for one built pipeline. kObjectOffset/kObjectBytes are the same on
// every pipeline (b1 always first, 128 bytes); slotOffset/slotBytes cover every other declared
// root-constant slot back to back; mesh-geometry fields are valid only for a mesh pipeline.
// totalBytes must be <= the queried maxPushConstantsSize (VulkanDevice::maxPushConstantsSize()) --
// checked in VulkanResourceFactory.cpp, which fails pipeline creation if it doesn't fit.
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
PushConstantLayout pushConstantLayout(const PipelineLayout& layout, bool mesh);   // DEFINED IN VulkanResourceFactory.cpp; cached alongside the VkPipelineLayout it describes, in DescriptorLayoutEntry below

// A stable key for everything about a PipelineLayout that changes how a shader compiles, so a
// module compiled for one layout is reused by every layout that agrees and no other. Packs the
// root-constant slot mask, all four table counts, the sampler count and `mesh` -- must cover
// every per-layout compile step: began as the constant-slot mask alone (complete while
// patchCbuffersForLayout was the only such step), grew once already when buildRegisterBinds made
// table counts matter; extend it again if another step is added.
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
// Takes a layout (unlike patchPerFrameSet) because a cbuffer's kind isn't in the shader text --
// `cbuffer SkinParams : register(b3)` reads the same whether b3 is push constants or a descriptor;
// only constantDwords[3] tells them apart. A source-only patch would have to guess (tried,
// reverted: turns a diagnosable "wrong set" into an equally broken "right set, wrong kind"). So
// createShader keeps RhiShader's source and moduleForLayout re-patches at PIPELINE
// creation, once the layout is known.
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
// Declared above ConstantRing because it uses them -- these two lines used to sit 30 lines below
// that struct, so every TU failed on `kRhiRingBytes: identifier not found`, unnoticed because the
// module also couldn't be CONFIGURED (CMakeLists named a nonexistent source file, so CMake failed
// first). Not shared with D3D12's constant of the same name: its ring is an UPLOAD-heap buffer
// always mappable, while this one may not be HOST_COHERENT (see ConstantRing::coherent below).
constexpr u64 kRhiRingBytes = 1u << 20;
constexpr u64 kRhiRingMaxBytes = 64ull << 20;

struct ConstantRing {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u8* mapped = nullptr;
    bool coherent = false;   // whether `memory`'s type is HOST_COHERENT; false means every write
                              // needs an explicit vkFlushMappedMemoryRanges before the GPU reads it
                              // -- varies by vendor/type, unlike D3D12's UPLOAD heap. Must be
                              // checked, not assumed.
    VkDeviceSize bytes = 0;    // current allocation size; grows geometrically, mirrors D3D12's ringBytes_
    VkDeviceSize used = 0;     // this frame's bump cursor, reset every beginFrame
    VkDeviceSize wanted = kRhiRingBytes;   // sticky high-water mark across frames
};
// One suballocation's address: vkCmdBindDescriptorSets' pDynamicOffsets (via `offset`) or a direct
// memcpy (via `cpu`).
struct ConstantAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    u8* cpu = nullptr;   // nullptr on overflow -- treat like a failed D3D12 ringAlloc: log once
                          // per frame (ringOverflowEpoch_-shaped), skip the draw/pass.
};

// ================================================================================================
// 6. Per-draw CPU-side structs. PerFrameCB and PostCB used to be hand-copied here from
//    D3D12Device.cpp -- warning that "a single stray float here is a silent cross-backend shading
//    divergence no compiler catches" -- and now live once in aver/rhi/FrameConstants.hpp, included
//    by both backends, so that divergence is no longer expressible.
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

// A mesh uploaded to the GPU. Vulkan-flavoured twin of D3D12Device.cpp's GpuMesh -- same fields,
// same index-buffer-sharing discipline (createSkinTargetMesh shares its source's index buffer;
// ownership is recorded, not inferred), same "slot cleared, never recycled" destroyMesh contract
// (a stale handle must address a dead mesh, never a different live one).
struct GpuMesh {
    VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VkDeviceMemory vbMemory = VK_NULL_HANDLE, ibMemory = VK_NULL_HANDLE;
    VkDeviceAddress vbAddress = 0, ibAddress = 0;   // buffer_device_address for a mesh pipeline's push-constant geometry block (PushConstantLayout::meshVertexAddrOffset/meshIndexAddrOffset); analog of D3D12's raw root-SRV bind
    u32 indexCount = 0;
    BufferHandle vbBuffer = 0;   // RHI-level handles mirroring vb/ib, so a shader can get descriptors over this mesh's geometry (IDevice::meshGeometry); same purpose as D3D12's vbBuffer/ibBuffer
    BufferHandle ibBuffer = 0;
    u32 vertexCount = 0;
    bool computeWritten = false;   // non-zero only for a createSkinTargetMesh()/createPosedPartMesh() result; IDevice::meshVertexBuffer's "cache expiry" contract needs this exact
    f32 boundsCentre[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsRadius = 0.0f;
    bool ibOwned = true;
    u32  ibShares = 0;
    MeshHandle ibSource = 0;
    // W11: VERTEX-buffer sharing, mirroring ibOwned/ibShares/ibSource in the OPPOSITE direction
    // (createSkinTargetMesh shares indices/owns vertices; createMeshSharingVertices shares
    // vertices/owns indices -- a single pair of fields could not distinguish "my indices are
    // shared" from "my vertices are shared" for a mesh that is somehow both; see
    // IDevice::createMeshSharingVertices for the LOD-ladder case this is for). vbSource, when
    // !vbOwned, always names the ROOT, never another share (a share of a share collapses to it --
    // see VulkanDevice::createMeshSharingVertices).
    bool vbOwned = true;
    u32  vbShares = 0;
    MeshHandle vbSource = 0;
    bool alive = true;
};

// One createBlasMulti geometry as the size query and the build both describe it -- the same triangle
// description createBlasImpl/recordBlasBuild give their one mesh, OPAQUE only where the part asked.
// Shared by VulkanResourceFactory.cpp and VulkanRenderContext.cpp, which must agree or the build
// writes past what the query sized.
inline VkAccelerationStructureGeometryKHR vkMultiBlasGeometry(const GpuMesh& m, bool opaque) {
    VkAccelerationStructureGeometryTrianglesDataKHR tri{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = m.vbAddress;
    tri.vertexStride = sizeof(MeshVertex);
    tri.maxVertex = m.vertexCount > 0 ? m.vertexCount - 1 : 0;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = m.ibAddress;
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.geometry.triangles = tri;
    geom.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    return geom;
}

// One engine instance as a Vulkan TLAS build reads it. Shared by VulkanRenderContext::
// packTlasInstances (per frame) and VulkanResourceFactory::setTlasStaticInstances (the prefix, packed
// once), so the two can never disagree about a transform or a flag. The caller has already refused an
// id past 24 bits.
inline VkAccelerationStructureInstanceKHR vkInstanceFromTlas(const TlasInstance& in, VkDeviceAddress blas) {
    static_assert(sizeof(VkAccelerationStructureInstanceKHR) == kTlasInstanceDescBytes,
                  "tlasStaticInstanceBuffer's element size is the Vulkan instance struct");
    VkAccelerationStructureInstanceKHR id{};
    // Engine matrices are row-major/row-vector (v*M); VkTransformMatrixKHR is the same row-major
    // 3x4 [R|T] layout D3D12_RAYTRACING_INSTANCE_DESC::Transform already uses, so this is the
    // identical transpose D3D12's toInstanceDesc performs, not a Vulkan-specific one.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) id.transform.matrix[r][c] = in.world[c * 4 + r];
        id.transform.matrix[r][3] = in.world[12 + r];
    }
    id.mask = in.mask;
    // instanceCustomIndex (CommittedInstanceID() in HLSL), NOT
    // instanceShaderBindingTableRecordOffset -- the latter indexes a shader binding table this
    // backend never builds (VK_KHR_ray_query only, no VK_KHR_ray_tracing_pipeline; see the
    // contract's note on which extension the engine's inline RayQuery shaders actually need).
    id.instanceCustomIndex = in.instanceId;
    // MAPPED, NOT CAST. The engine's TlasInstanceFlags values were chosen to match D3D12's, and
    // VkGeometryInstanceFlagBitsKHR happens to use the same bit positions today -- but "happens
    // to" is not a contract between two vendors' headers, and a silent divergence here would put
    // a wrong flag on every instance with nothing to grep for. Written out so the two are only
    // ever equal on purpose.
    VkGeometryInstanceFlagsKHR vkFlags = 0;
    if (in.flags & TlasInstanceFlag_TriangleCullDisable)
        vkFlags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    if (in.flags & TlasInstanceFlag_TriangleFrontCcw)
        vkFlags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
    if (in.flags & TlasInstanceFlag_ForceOpaque)
        vkFlags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    if (in.flags & TlasInstanceFlag_ForceNonOpaque)
        vkFlags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    id.flags = vkFlags;
    id.accelerationStructureReference = blas;
    return id;
}
// Per-subresource state tracking, debug builds only; private macro, not shared cross-module state.
#if defined(NDEBUG)
#define AVER_RHI_TRACK_STATE 0
#else
#define AVER_RHI_TRACK_STATE 1
#endif

// ================================================================================================
// 7. Handle-table records. A handle is index + 1 into the owning vector, so 0 is never live; a
//    dead slot is kept (never recycled), its Vk handles reset to VK_NULL_HANDLE -- "alive" iff the
//    primary handle is non-null, mirroring D3D12Device.cpp's RhiTexture..RhiTlas.
// ================================================================================================

struct RhiTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView srvView = VK_NULL_HANDLE;    // whole resource, every mip, the SAMPLED aspect
    VkImageView rtvView = VK_NULL_HANDLE;    // single-mip colour-attachment view, when ResourceBind::RenderTarget
    VkImageView dsvView = VK_NULL_HANDLE;    // depth-attachment view, when ResourceBind::DepthStencil (or isDepthFormatVk)
    std::vector<VkImageView> uavViews;       // one storage-image view per mip, built lazily by setUav
    TextureDesc desc{};                      // resolved: `mips` holds the real count, never 0
    // OUTSIDE AVER_RHI_TRACK_STATE, unlike `states`: textureBarrier/uavBarrierTexture name the
    // texture in their null-VkImage error, which must compile under NDEBUG too -- it used to sit
    // inside that macro, so the error never compiled under NDEBUG, unnoticed because
    // Aver.RHI.Vulkan had never been built Release.
    std::string debugName;                   // owned copy: the desc's debugName is the caller's pointer
#if AVER_RHI_TRACK_STATE
    std::vector<ResourceState> states;       // one entry per mip; a subresource index is a mip index here
#endif
    bool externallyOwned = false;   // true when WRAPPING a VkImage/VkDeviceMemory this factory did NOT allocate (today only adoptExternalDepthTexture's depthBuffer_/depthMemory_, IDevice::sceneDepthTexture's contract in RHI.hpp, torn down by VulkanDevice itself): destroyTexture()/~VulkanResourceFactory() MUST skip destroyImageCommitted() when set, or a resize/shutdown double-frees the image. The VIEWS this record builds are never foreign.
};

struct RhiBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;   // valid once `buffer` is: every buffer here is created with SHADER_DEVICE_ADDRESS usage (toVkBufferUsage)
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
    std::vector<u32> spirv;   // the LAYOUT-AGNOSTIC compile: every cbuffer a descriptor. Still the module most pipelines use and always what reflectTableSlotKinds reads -- patching only moves CBUFFERS, so SRV/UAV bindings are identical in every variant
    ShaderStage stage = ShaderStage::Vertex;
    VkShaderModule module = VK_NULL_HANDLE;

    // Kept because a cbuffer's descriptor-vs-root-constant kind is a property of the PIPELINE
    // LAYOUT, not the shader text (patchCbuffersForLayout), unknown yet at createShader time;
    // re-patched per layout at pipeline creation (moduleForLayout()).
    std::string source;           // prelude + source, already patchPerFrameSet'd
    std::string entry;
    std::string defines;
    u32 minShaderModel = 60;
    std::vector<RhiShaderVariant> variants;
};

// The cached VkDescriptorSetLayout for one table SHAPE (srvCount/uavCount + each slot's SlotKind),
// separate from DescriptorLayoutEntry and finer-grained: Vulkan descriptor-set-layout
// compatibility is per-SHAPE, not per-pipeline, so a BindingSetHandle matching BindingSetDesc's
// fields binds at any table index any pipeline declares that shape at. createBindingSet() and
// every PipelineLayout's table set layout MUST share this ONE cache
// (VulkanResourceFactory::tableSetLayout()), or identical shapes could mint two different
// VkDescriptorSetLayout objects and break that contract.
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
    bool instanced = false;   // GraphicsPipelineDesc::instanced, a SIBLING of PipelineLayout so part of this cache key too: identical layouts with different instancing need different VkPipelineLayouts
    VkDescriptorSetLayout instancesSetLayout = VK_NULL_HANDLE;   // shared one-binding set-4 layout from instanceSetLayout(); NOT owned here, like tableSetLayouts above
};

// A pipeline state and the layout-cache entry it was built against.
struct RhiPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    const DescriptorLayoutEntry* layoutEntry = nullptr;   // owned by VulkanResourceFactory's cache, not by this
    bool compute = false;
    bool mesh = false;
    bool amplification = false;   // GraphicsPipelineDesc::as != 0: a dispatchMeshClusters() pipeline, not dispatchMeshFor()
    bool instanced = false;       // GraphicsPipelineDesc::instanced: set kVkSetInstances exists, vertex shader reads gInstanceWorlds; drawMeshInstanced checks this exactly as D3D12RenderContext::drawMeshInstanced checks pipe_->instanceWorldParam >= 0
};

// Allocated VkDescriptorSets for table 0/1, plus the declared slot kinds (for nullFill's
// dimension-matching and setSrv/setUav's slot-kind validation) -- from VulkanResourceFactory's
// shared descriptorPool_, the Vulkan analog of D3D12's shared shader-visible heap, at
// DESCRIPTOR-SET rather than DESCRIPTOR-RANGE granularity (sets, not a bindless array, is the
// strategy here -- see the architecture scout's finding #2).
// One slot's resolved descriptor, kept so the write can be REPLAYED into a different ring slot --
// Vulkan has no way to copy a binding out of a descriptor set, so bringing a second set up to date
// means remembering what was written and writing it again.
struct BindingSlotState {
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;   // MAX_ENUM == never written
    VkDescriptorImageInfo      image{};
    VkDescriptorBufferInfo     buffer{};
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
    VkImageView ownedView = VK_NULL_HANDLE;   // a one-off mip view built by setSrv for THIS slot, owned by it: must outlive the replay into every other ring slot, so can't be retired at the end of the call that made it
};

// A binding set, RINGED kFrameCount DEEP.
//
// WHY: rewriting a descriptor set while a command buffer that bound it is still pending puts that
// buffer in an INVALID state ("... destroyed or updated without UPDATE_AFTER_BIND") and the
// driver DROPS every call recorded after it -- with one set per binding set, that happened every
// frame anything rebound (Voxi's RT history ping-pong rewrites four slots per frame), taking the
// whole overlay down.
//
// beginFrame() already waits on the timeline value retiring frameIndexInFlight(), so the CURRENT
// ring slot is provably free to write. Cost: a write lands in one ring slot only, so the others go
// stale -- hence srvSlots/uavSlots and staleMask, replayed lazily by bindingSetForFrame().
//
// INVARIANT: nothing writes a binding set after binding it within the same frame -- a ring by
// FRAME can't help with that. MEASURED: writeBindingSlot warns on violation; zero across a
// 12-frame run when this was built.
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
    // Set at creation by createBlasUpdatable: built with ALLOW_UPDATE_BIT_KHR and scratchBuffer
    // above already sized for an update as well as a build, so VulkanRenderContext::refitBlas may
    // update this BLAS in place instead of rebuilding it from scratch. Mirrors D3D12ResourceFactory's
    // RhiBlas::allowUpdate.
    bool allowUpdate = false;
    // This mesh's vertex/index counts as of the last FULL build -- refitBlas's eligibility test. An
    // in-place update must keep the SAME geometry description Vulkan built with (maxVertex,
    // primitiveCount included), only moved vertex positions; a mesh that has grown or shrunk since
    // needs a full rebuild instead.
    u32 builtVertexCount = 0;
    u32 builtIndexCount = 0;
    // What asBuffer/scratchBuffer were allocated at, once, by createBlasImpl -- so refitBlas can
    // refuse a rebuild the mesh has outgrown instead of writing past them (same guard as D3D12's).
    VkDeviceSize asSize = 0;
    VkDeviceSize scratchSize = 0;
    // Non-empty for a createBlasMulti structure: every geometry, in GeometryIndex() order. `mesh` above
    // is then geometries[0].mesh. Mirrors D3D12ResourceFactory's RhiBlas::geometries.
    std::vector<BlasGeometry> geometries;
};

// One instance as it was packed into a TLAS's last build or refit -- BLAS identity plus the Vulkan
// instance flags and mask, but deliberately NOT the transform or instance id, which refitTlas lets
// change freely. Mirrors D3D12ResourceFactory's RhiTlasSlot (blasVa there, blasAddress here -- same
// idea, this backend's GPU-address type).
struct RhiTlasSlot {
    VkDeviceAddress blasAddress = 0;
    u32 flags = 0;
    u32 mask = 0;
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
    // Set at creation by createTlasUpdatable: see RhiBlas::allowUpdate above -- same idea, this
    // structure's scratchBuffer is sized for an update too.
    bool allowUpdate = false;
    // Has a full BUILD ever landed. An UPDATE needs a valid source structure, so refitTlas falls
    // back to a full build until this is true, same as RhiBlas::built gates refitBlas.
    bool built = false;
    // The last build's or refit's per-slot signature, AFTER buildTlas/refitTlas's own filtering (an
    // invalid-BLAS or oversized-id instance never appears here), in submission order. refitTlas
    // updates in place only when the new filtered list is the SAME SIZE and every entry's
    // {BLAS, flags, mask} still matches -- transforms and instance ids may differ freely.
    std::vector<RhiTlasSlot> builtSlots;
    // Where each build/refit packs its NEW signature before swapping it with builtSlots -- kept, not a
    // local, so a per-frame refit reuses one allocation instead of making one per call.
    std::vector<RhiTlasSlot> pendingSlots;
    // What asBuffer/scratchBuffer were allocated at, for tlasMemoryBytes.
    VkDeviceSize asSize = 0;
    VkDeviceSize scratchSize = 0;

    // ---- the static prefix (IResourceFactory::setTlasStaticInstances); mirrors D3D12's RhiTlas ----
    // Slots [0, staticCount) of staticDescs (device-local, (staticCount + maxInstances) instances, an
    // ordinary buffer so a shader can read it by handle), packed once; each build copies its per-frame
    // instances in at staticCount, with the barriers recordTlasBuild owns.
    u32 staticCount = 0;
    BufferHandle staticDescs = 0;
    // The distinct BLASes the prefix names, checked before every build -- O(distinct), not O(prefix).
    std::vector<BlasHandle> staticBlases;
    // The prefix length the last build/refit used (0 with none, or a dropped broken one): an update is
    // only legal over the same instances its build had.
    u32 builtStatic = 0;
    bool staticBrokenLogged = false;
};

// A destroyed object the GPU may still be reading, released once `fence` retires. Vulkan analog
// of D3D12Device.cpp's RetiredObject, generalised with a type-erased deleter instead of a
// ComPtr<IUnknown> -- Vulkan handles share no common base a Release() could work through, so the
// closure IS the generalisation.
struct RetiredObject {
    u64 fence = 0;
    std::function<void()> destroy;
};

// ================================================================================================
// 8. VulkanShaderCompiler — DXC, `-spirv`. DEFINED IN VulkanShaderCompiler.cpp.
// ================================================================================================
//
// DXC is the ONLY compiler in scope, no FXC-equivalent fallback (see third_party/vulkan-headers/
// README.md -- adding glslang/shaderc would be a second source of truth for the same HLSL) -- a
// device with no usable DXC cannot compile shaders at all, so compile() failing means no working
// shader path, not one shader among many failing.
// VkRegisterBind / kMaxRegisterBinds / buildRegisterBinds live in VulkanRegisterMap.hpp instead,
// included at the top of this file -- see that header for why they are separable.

class VulkanShaderCompiler {
public:
    // Loads dxcompiler.dll once (the same DLL D3D12 loads for DXIL; see this module's
    // CMakeLists.txt for why no separate copy step is needed) and resolves DxcCreateInstance.
    void init();
    bool usingDxc() const;

    // Compiles one HLSL entry point to SPIR-V. `src` is the fully assembled source (prelude
    // already concatenated). `stage`+`minShaderModel` derive the DXC target profile (e.g.
    // "cs_6_5") via dxcTargetPrefix(stage); `defines` is semicolon-separated, same shape as
    // ShaderDesc::defines. Appends `-spirv` and the binding-shift args section 4's descriptor
    // scheme requires (must agree with tableSetLayout()/descriptorLayout() byte for byte). False
    // (and logged) on failure, `outSpirv` left untouched.
    //
    // `quiet` downgrades DXC's diagnostics from ERROR to DEBUG, for a caller that expects this
    // compile to possibly fail and has a recovery path (moduleForLayout's bind-map attempt).
    bool compile(const char* src, const char* entry, ShaderStage stage, u32 minShaderModel,
                 const char* defines, std::vector<u32>& outSpirv,
                 const VkRegisterBind* binds = nullptr, u32 bindCount = 0, bool quiet = false);

private:
    bool tried_ = false;
    HMODULE dll_ = nullptr;
    void* utils_ = nullptr;   // IDxcUtils*/IDxcCompiler3* (compiler_ below), kept as void* so this header never includes <dxcapi.h>/<wrl/client.h> -- no more business leaking outside this module than a VkFoo has; cast back to the real types only inside VulkanShaderCompiler.cpp
    void* compiler_ = nullptr;
};
VulkanShaderCompiler& vulkanShaderCompiler();   // process-wide instance shared by VulkanDevice's fixed pipelines and VulkanResourceFactory::createShader; mirrors D3D12Device.cpp's shaderCompiler()

// DXC target-profile prefix for a stage ("vs"/"ps"/"gs"/"cs"/"ms"/"as") -- identical convention to
// D3D12Device.cpp's stagePrefixFor, reused by both createShader and the fixed-pipeline compiles to
// build the same "%s_%u_%u" target string.
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
// so VulkanDevice can hold one and befriend the installer without including that public header --
// it must not pull vulkan.h into anything that includes it.
class IUiBackend;
struct UiBackendInitDesc;
bool installUiBackend(IDevice* device, IUiBackend* backend);
class VulkanRenderContext;

// Creates a buffer, allocates device memory of the first type matching `required`, and binds them
// -- the multi-TU analog of D3D12's CreateCommittedResource for buffers. `outAddress`, when
// non-null, receives vkGetBufferDeviceAddress's result (always created with SHADER_DEVICE_ADDRESS
// usage; see toVkBufferUsage). False (and logged) on any failure, with any partial
// VkBuffer/VkDeviceMemory already destroyed -- nothing left for the caller to clean up.
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

// Buffer/image usage flags this factory grants EVERY buffer/image it creates, regardless of the
// caller's declared ResourceBind/BufferKind -- deliberate "everything is everything"
// simplification (D3D12 has no narrow per-resource usage-flag concept to mirror), traded for one
// line of policy instead of an untested matrix of usage combinations. Revisit if a driver refuses
// a specific combination.
// `rtAvailable` gates the one usage bit that is NOT always legal: without
// VK_KHR_acceleration_structure, AS build-input usage is rejected outright.
inline VkBufferUsageFlags toVkBufferUsage(const BufferDesc& d, bool rtAvailable) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (d.kind == BufferKind::AccelStructure)
        u |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    // BUILD-INPUT USAGE BELONGS ON ORDINARY BUFFERS TOO: a BLAS builds from a plain vertex/index
    // buffer (BufferKind::Default), and granting this only to AccelStructure buffers rejected
    // every such build (missing ..._BUILD_INPUT_READ_ONLY_BIT_KHR) -- worse, on failure
    // vkCmdBuildAccelerationStructuresKHR INVALIDATES the command buffer, dropping everything
    // recorded after it, editor UI included. "Everything is everything": a BufferKind for "might
    // be raytraced" is unknowable at upload time for a mesh some later frame decides to trace.
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
    bool init(const DeviceDesc& desc);   // not an override (IDevice has no init()); createVulkanDevice() calls this once, like D3D12Device::init() from createD3D12Device()
    ~VulkanDevice() override;

    // ---- IDevice ----
    Backend backend() const override { return Backend::Vulkan; }
    const char* adapterName() const override { return adapterName_.c_str(); }
    DeviceCaps caps() const override { return caps_; }
    VideoMemoryInfo videoMemory() const override;   // M6: current VRAM budget/usage, LOCAL/NON_LOCAL exactly as VideoMemoryInfo's own comment (RHI.hpp) specifies; NOT inline (queries the driver every call); supported == false, honestly, whenever VK_EXT_memory_budget or GetPhysicalDeviceMemoryProperties2 is absent
    IResourceFactory* resources() override;
    IRenderContext* renderContext() override;
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    // Same predicate drawMesh applies internally (suppressesScene early return), exposed so a
    // caller can decline a draw it knows to be editor chrome -- see IDevice::sceneSuppressed for
    // why the filter can't live in drawMesh itself. Implemented here rather than left on the
    // default false, which would silently mean "nothing is suppressing" -- a cross-backend
    // divergence that's hard to notice.
    bool sceneSuppressed() const override {
        for (const IRenderFeature* f : features_) if (f->suppressesScene()) return true;
        return false;
    }
    // Non-owning, like addRenderFeature/removeRenderFeature above. Null is the PERMANENT default
    // (nothing here ever assigns upscaler_ on its own); every branch that matters gates on this
    // pointer rather than a quality enum or build flag, so "no upscaler set" and "no AverSR module
    // in this build" are the same code path -- see D3D12Device::setUpscaler for the same invariant.
    // Mirrors D3D12Device: AverSR's targets are built with the post targets, so attaching or
    // detaching an upscaler rebuilds them at the next frame start.
    void setUpscaler(IUpscaler* u) override {
        if ((u != nullptr) != (upscaler_ != nullptr) && hasSwapchain_) upscalerTargetsDirty_ = true;
        upscaler_ = u;
    }
    bool upscalerTargetsDirty_ = false;
    IUpscaler* upscaler() const override { return upscaler_; }
    Format backbufferFormat() const override { return fromVkFormat(kVkSceneColorFormat); }
    Format depthFormat() const override { return fromVkFormat(kVkDepthFormat); }
    u32 sampleCount() const override { return sampleCount_; }
    bool setSampleCount(u32 samples) override;
    // Decouples the scene's render targets from the swapchain's: the scene renders at
    // round(present * scale) while everything present-resolution (backbuffer, viewport texture,
    // capture) stays pinned to width_/height_ (IDevice::setRenderScale's contract, RHI.hpp).
    // NOT inline: PARKS the value (pendingRenderScale_) rather than rebuilding here -- see
    // applyPendingRenderScale's comment for why. D3D12's twin was fixed for the device-loss race
    // in fa74459; OPEN here (aver-render-scale-device-loss, wave 2/C2-13) -- AverSR Auto now calls
    // this from onUpdate on every launch, at Medium/Low's non-1.0 default, so this is no longer a
    // rare CLI-only corner.
    void setRenderScale(f32 scale) override;
    // Reports what the last caller asked for (the parked value when one is pending), not what is
    // currently resident, so a read-back right after a set doesn't see stale state for one frame.
    // Mirrors D3D12Device::pendingOrCurrentRenderScale().
    f32 renderScale() const override { return pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_; }
    ISwapchain* createSwapchain(const SwapchainDesc& desc) override;
    void beginFrame() override;
    void endFrame() override;
    bool runStandaloneCompute(const std::function<void(IRenderContext&)>& record) override;
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
    // W4: chooses the heap createMesh() (and createMeshSharingVertices()'s new index buffer)
    // uploads to for every call made after this one -- trivial store/load, exactly like setVSync/
    // vsync() beside it; see IDevice's own contract (RHI.hpp) for the false=Upload/true=Default
    // meaning and why a mesh already built keeps whatever heap it was built on.
    void setStaticMeshHeapDefault(bool onDefaultHeap) override { staticMeshDefaultHeap_ = onDefaultHeap; }
    bool staticMeshHeapDefault() const override { return staticMeshDefaultHeap_; }
    // W11: a new mesh sharing `source`'s vertex buffer with its own index buffer -- see
    // IDevice::createMeshSharingVertices's full contract (RHI.hpp) and GpuMesh's vbOwned/vbShares/
    // vbSource fields above. NOT inline: real allocation and (conditionally) a one-shot upload;
    // defined in VulkanDevice.cpp beside createMesh.
    MeshHandle createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) override;
    MeshHandle createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) override;
    bool destroyMesh(MeshHandle mesh) override;
    bool destroyLineMesh(LineHandle mesh) override;
    MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) override;
    BufferHandle meshVertexBuffer(MeshHandle mesh) const override;
    bool meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib, u32* vertexCount, u32* indexCount) const override;
    bool meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const override;
    void setCamera(const f32 viewProj[16], const f32 invViewProjRel[16], const f32 cameraPos[3]) override;
    bool camera(f32 viewProj[16], f32 invViewProjRel[16], f32 cameraPos[3]) const override;
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
    // See IDevice::postExposureReadout (RHI.hpp) for the contract, and D3D12Device's identical
    // override -- expReadoutValue_/expReadoutSeeded_ are filled a few frames late by
    // collectExposureReadout, from a GPU copy runPostChain records only while auto exposure is
    // actually running.
    bool postExposureReadout(f32& adaptedExposure) const override {
        if (!expReadoutSeeded_) return false;
        adaptedExposure = expReadoutValue_;
        return true;
    }
    void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4], f32 metallic, f32 roughness) override;
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(drawBinding_, set, constants, bytes);
    }
    void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override {
        storeDrawBinding(defaultDrawBinding_, set, constants, bytes);
    }

    // ---- same-frame depth prepass -- full contract on IDevice (RHI.hpp); mirrors D3D12Device.cpp
    // :1005-1013. OFF by default: --depth-prepass measured as a LOSS on this engine's scenes
    // (aver-frame-budget), so this exists for correctness/parity, not speed, and the default must
    // reproduce today's no-prepass Vulkan behaviour exactly.
    void setDepthPrepassEnabled(bool on) override { depthPrepassEnabled_ = on; }
    bool depthPrepassEnabled() const override { return depthPrepassEnabled_; }
    void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) override;
    bool drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) override;
    bool depthOnlyDraw(MeshHandle mesh, const f32 world[16], const f32 color[4], bool allowComputeWritten);   // shared body of the two above; true when actually recorded. allowComputeWritten: true only from drawMeshDepthOnly -- see D3D12Device's twin
    // AUTO-CONSUMED by the very next drawMesh() call, not stored past it (see IDevice). Plain
    // assignment: only records what the caller believes about the upcoming draw; drawMesh() still
    // re-checks it (static via meshVertexBuffer(mesh)==0, compute-written only if it is
    // depthOnlyMesh_), exactly as D3D12Device::drawMesh does.
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
    void setWireframe(bool on) override { wireframe_ = on; if (on) wireframeFrame_ = true; }
    void setUnlit(bool on) override { unlit_ = on; }
    // Sticky state, captured per queue() call -- see aver::rhi::EditorLines::setDepthTest/setWidth.
    // (setLineGlow is gone: lines no longer go through the HDR scene target, so there is no bloom
    // left to make them glow -- see IDevice::setLineWidth, RHI.hpp.)
    void setLineDepth(bool testDepth) override { editorLines_.setDepthTest(testDepth); }
    void setLineWidth(f32 pixels) override { editorLines_.setWidth(pixels); }
    void requestCapture(u32 x, u32 y) override { capX_ = x; capY_ = y; captureReq_ = true; captureReady_ = false; }
    bool getCapture(f32 outRGBA[4]) override;
    bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) override;
    // ---- the UI methods: delegate to whatever aver::rhi::vulkan::IUiBackend has been installed;
    // false/no-op ("no in-window UI") when none has, which is every game build. Seam:
    // modules/rhi.vulkan/include/aver/rhi/vulkan/UiBackend.hpp; the concrete toolkit lives in its
    // own module so nothing about Dear ImGui compiles into anything that merely links the RHI. NO
    // ImGui INCLUDE OR SYMBOL MAY APPEAR IN THIS MODULE -- same rule Aver.RHI.D3D12 keeps.
    // OPEN: which toolkit fills the seam -- imgui_impl_vulkan is not vendored, and vendoring it is
    // a real dependency call, not implied by "implement the Vulkan backend". ----
    bool uiInit(void* windowHandle) override;
    void uiNewFrame() override;
    void uiShutdown() override;
    bool uiActive() const override { return uiBackend_ != nullptr && uiUp_; }
    bool uiWantsMouse() const override;
    bool uiWantsKeyboard() const override;
    u64 uiTextureId(TextureHandle t) override;   // handle the UI toolkit draws one of THIS backend's textures through -- the editor viewport and every asset thumbnail come through here; zero with no backend installed
    void releaseUiTextureId(TextureHandle t);   // called when a texture goes away, so the toolkit's descriptor doesn't outlive the image view; a leak otherwise, and the thumbnail cache destroys textures constantly

    // ---- plain (non-override) helpers, mirroring D3D12Device's own public non-interface surface ----
    void present();
    void resize(u32 w, u32 h);
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // ---- accessors for the free helpers and the two friend classes below ----
    const VulkanApi& api() const { return api_; }
    bool pushRenderScope(VkCommandBuffer cmd, const VkRenderingInfo& ri);   // opens a dynamic-rendering scope, or joins the one open; returns whether it opened one -- caller MUST pass that to popRenderScope, so an inner no-op open can't close its outer owner's scope (see renderScopeDepth_)
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
    // below), so anything retired at that value is safe once this frame's submit has completed.
    u64 retireFenceValue() const { return nextTimelineValue_ + 1; }
    VkSemaphore timelineSemaphore() const { return timeline_; }
    VkDeviceSize minUboAlignment() const { return minUboAlignment_; }
    // The STORAGE-buffer equivalent, a separate number since a device may align the two
    // differently; gInstanceWorlds is a storage buffer bound with a dynamic offset, so this one
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
    // the GPU, drop the depth/MSAA-colour images,
    // recompute the scene size, invalidate the stored viewport sub-rect (vpX_/vpY_/vpW_/vpH_ are in
    // SCENE space -- see setViewportRect -- and go stale the instant sceneWidth_/sceneHeight_
    // move), recreate the scene targets, drop the post chain's size-dependent targets
    // (releasePostTargets()), and renotify every registered render feature of the new size.
    void rebuildSceneTargets();
    // Applies a PARKED setRenderScale() at a frame boundary, mirroring D3D12Device::
    // applyPendingRenderScale exactly: called as the first statement of beginFrame(), before
    // anything records into that frame's command buffer. Must run there and
    // not mid-frame -- rebuildSceneTargets() frees and recreates the depth buffer, MSAA target and
    // post chain, and doing that while a command buffer already has those images bound (possible
    // mid-frame, e.g. editor buildUI() or AverSR Auto's onUpdate reassert) records a submit against
    // freed resources, which loses the device: waitForGpu() only drains work already SUBMITTED,
    // not a command buffer still being recorded on the CPU. See aver-render-scale-device-loss.
    void applyPendingRenderScale();
    void waitForGpu();
    // Blocks until the timeline semaphore reaches `value`. False only when the device has been
    // lost -- the Vulkan analog of D3D12's waitFence(); VK_ERROR_DEVICE_LOST is the one VkResult
    // this cannot recover from, same as a D3D12 device-removed HRESULT.
    bool waitTimeline(u64 value);

    // ---- the camera post chain (rhi::PostSettings); field-for-field mirror of D3D12's own, Vulkan-flavoured ----
    bool createPostPipelines();
    bool createPostTargets();
    void releasePostTargets();
    // Reads back the metered-exposure slot THIS frame's beginFrame just waited on -- see
    // expReadback_'s own comment. Mirrors D3D12Device::collectExposureReadout field-for-field.
    void collectExposureReadout();
    void runPostChain(VkImage backbufferImage, VkImageView backbufferView, VkFormat backbufferFormat);
    ConstantAllocation postConstants(const void* data, u32 bytes);
    static void toSceneReferred(const f32 display[4], f32 out[4]);

    void notifyRenderTargetsChanged();
    bool ensureViewportTexture();
    void seedSkinTargets();
    // Shared body of createMeshSharingVertices and createPosedPartMesh; see D3D12Device's twin.
    MeshHandle shareVertices(MeshHandle source, const u32* indices, u32 indexCount, bool posed);
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

    // ---- frame pacing: ONE timeline semaphore replaces D3D12's fence_/fenceValues_/nextFence_ --
    // vkWaitSemaphores against it is retireFenceValue()'s CPU wait, and every retired-object fence
    // value in RetiredObject is a value on this SAME timeline. Per-swapchain-image BINARY
    // semaphores are still required for acquire/present, since core Vulkan never lets a timeline
    // semaphore do that alone (vkAcquireNextImageKHR's semaphore parameter must be binary). ----
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 nextTimelineValue_ = 0;
    u64 frameTimelineValues_[kFrameCount] = {};   // the timeline value that retires each FRAME-IN-FLIGHT slot's last submit
    VkSemaphore imageAvailable_[kFrameCount] = {};
    std::vector<VkSemaphore> renderFinished_;     // one per SWAPCHAIN IMAGE (sized at swapchain creation), not per frame in flight
    u32 frameIndex_ = 0;    // 0..kFrameCount-1, the frame-IN-FLIGHT slot
    u64 frameSerial_ = 0;   // MONOTONIC, unlike frameIndex_, which wraps at kFrameCount and can't tell "this frame" from "two frames ago in the same slot"
    bool frameOpen_ = false;   // between beginFrame and endFrame's command-buffer end; runStandaloneCompute refuses then

    // The installed in-window UI toolkit, or null. NON-OWNING -- see IUiBackend's own comment on
    // ownership; Sandbox holds the concrete object and outlives this device's uiShutdown().
    vkb::IUiBackend* uiBackend_ = nullptr;
    bool uiUp_ = false;   // init() succeeded and shutdown() has not run
    std::unordered_map<TextureHandle, u64> uiTexIds_;   // one toolkit descriptor per texture, made on first use; keyed by handle so releaseUiTextureId can find it from what destroyTexture knows
    u32 imageIndex_ = 0;    // the acquired swapchain image index this frame

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFrameCount] = {};

    // ---- fixed scene targets ----
    VkImage msaaColor_ = VK_NULL_HANDLE;      VkDeviceMemory msaaColorMemory_ = VK_NULL_HANDLE;      VkImageView msaaColorView_ = VK_NULL_HANDLE;
    VkImage depthBuffer_ = VK_NULL_HANDLE;    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;          VkImageView depthView_ = VK_NULL_HANDLE;
    // MSAA resolve destination. VK_NULL_HANDLE when sampleCount_ == 1, where msaaColor_ is already it.
    VkImage sceneResolved_ = VK_NULL_HANDLE;  VkDeviceMemory sceneResolvedMemory_ = VK_NULL_HANDLE;  VkImageView sceneResolvedView_ = VK_NULL_HANDLE;

    // Generic-RHI wrapper around depthBuffer_ -- see sceneDepthTexture()'s own comment. STABLE
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
    VkPipeline scenePso_ = VK_NULL_HANDLE, skyPso_ = VK_NULL_HANDLE;
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
    // Mesh drawMeshDepthOnly() last actually depth-drew, or 0; see D3D12Device's twin.
    MeshHandle depthOnlyMesh_ = 0;

    bool skyEnabled_ = false;
    bool sceneSuppressed_ = false;   // set in beginFrame when a feature suppressed the scene; read in endFrame so the deferred sky draw doesn't run over it
    // The narrower question: did that feature own the WHOLE frame, or only the scene geometry? See
    // IRenderFeature::suppressesWholeFrame. Kept in lockstep with D3D12 deliberately -- a sky that
    // appears on one backend and not the other is the worst shape of bug this repo has.
    bool frameSuppressed_ = false;
    // Last logged outcome of beginFrame's scene-claim race, so the warning fires on a CHANGE rather
    // than every frame. Compared, never dereferenced. Mirrors D3D12Device's pair of the same name.
    const IRenderFeature* lastSuppressWinner_ = nullptr;
    u32                   lastSuppressClaimants_ = 0;
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    // Wireframe on at ANY point this frame -- see D3D12Device::wireframeFrame_.
    bool wireframeFrame_ = false;
    // See IDevice::setUnlit. Sticky exactly as wireframe_ is.
    bool unlit_ = false;
    // Editor chrome (grid, gizmos, selection outlines, collider/nav overlays), replayed in endFrame's
    // overlay stage after the post chain -- see aver/rhi/EditorLines.hpp. Owns its own meshes/queue/
    // pipelines; createLineMesh/destroyLineMesh/drawLines/setLineDepth/setLineWidth all delegate here.
    EditorLines editorLines_;
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
    // Per-frame-slot HOST_VISIBLE|HOST_COHERENT readback of expBuf_'s first 8 bytes, for
    // IDevice::postExposureReadout -- a live UI number with no business stalling the frame on the
    // GPU. runPostChain records the copy right after CSExposure runs (only while autoExp is true,
    // so a slot never gets a copy of a value CSExposure did not just produce this frame);
    // collectExposureReadout maps and reads it back once this slot's timeline value has retired.
    // Mirrors D3D12Device's expReadback_ field-for-field, including WHERE it lives: created in
    // createPostTargets and released in releasePostTargets rather than alongside expBuf_ itself in
    // createPostPipelines -- fixed 8 bytes, so resize()'s waitForGpu() makes recreating it there
    // exactly as safe, and it keeps the readback tied to the one function pair that already owns
    // "the GPU is idle, drop anything mid-flight".
    VkBuffer expReadback_[kFrameCount] = {}; VkDeviceMemory expReadbackMemory_[kFrameCount] = {};
    // Set by runPostChain right after recording that slot's copy; cleared by
    // collectExposureReadout once consumed, so a skipped/recreated slot is never misread as fresh.
    bool expReadbackPending_[kFrameCount] = {};
    // What postExposureReadout() hands back -- the last value read from a completed GPU copy, a
    // few frames behind expBuf_ itself.
    f32  expReadoutValue_ = 0.0f;
    bool expReadoutSeeded_ = false;
    // Local exposure's bilateral grid of log-luminance (u2 raw, u3 blurred) -- SCENE-sized, unlike
    // histBuf_/expBuf_ above, so createPostTargets() destroys and rebuilds them on every resize;
    // final free at shutdown happens in the destructor, next to histBuf_/expBuf_'s own.
    VkBuffer localGridBuf_ = VK_NULL_HANDLE;     VkDeviceMemory localGridMemory_ = VK_NULL_HANDLE;
    VkBuffer localGridBlurBuf_ = VK_NULL_HANDLE; VkDeviceMemory localGridBlurMemory_ = VK_NULL_HANDLE;
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
    VkPipeline localGridPso_ = VK_NULL_HANDLE, localBlurPso_ = VK_NULL_HANDLE;   // CSLocalGrid, CSLocalBlur -- local exposure's two compute passes
    ConstantRing postRing_[kFrameCount]{};   // reuses section 5's ConstantRing shape; the ONE place outside VulkanRenderContext.cpp that owns a ring, as D3D12Device::postConstants() does separately from D3D12RenderContext::ringAlloc()
    bool postReady_ = false;
    i64 lastFrameTick_ = 0;
    f32 frameSeconds_ = 1.0f / 60.0f;

    // ---- AverSR ----
    // Null unless a host set one; every branch below tests THIS POINTER, not a quality enum or
    // build flag (see setUpscaler), so "no upscaler" and "no AverSR module in the build" are the
    // same code path. Field-for-field mirror of D3D12Device's upscaler_/sceneColorTex_/presentHdrTex_ trio.
    IUpscaler* upscaler_ = nullptr;
    // A factory-created ALIAS of the scene colour: the scene target (sceneResolved_, or
    // msaaColor_ when sampleCount_ == 1) is a raw VkImage allocated directly via
    // createImageCommitted, never through VulkanResourceFactory, so it has no TextureHandle --
    // and IUpscaler::execute needs one for UpscalerInput::color. A per-frame vkCmdCopyImage fills
    // this. Scene resolution, RGBA16F, sampled-only.
    TextureHandle sceneColorTex_ = 0;
    u32           sceneColorTexW_ = 0, sceneColorTexH_ = 0;
    // AverSR's output: HDR (pre-tonemap) at PRESENT resolution. The upscale runs on radiance and
    // PSComposite then tonemaps an image already the right size, so its own resample degenerates
    // to 1:1 and neither shader nor pipeline changes for the upscaled path.
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
    // calls setRenderScale renders byte-for-byte what it did before. Present-resolution state
    // (width_/height_, the swapchain, the capture buffer) never reads this pair; only the scene
    // depth/MSAA-colour targets, the render-scope's area/default viewport, the post chain's
    // targets, and onRenderTargetsChanged do.
    u32 sceneWidth_ = 0, sceneHeight_ = 0;
    f32 renderScale_ = 1.0f;   // [0.25, 1.0]; see IDevice::setRenderScale
    // PARKED value from a setRenderScale() call made with a swapchain already live: renderScale_
    // doesn't move until applyPendingRenderScale() runs at the top of the next beginFrame.
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
    // takes physical backbuffer pixels, but the target is the smaller SCENE one whenever
    // renderScale_ < 1). Exact identity at renderScale_ == 1.0. Mirrors D3D12Device's
    // scaleToSceneW/H, incl. the u64 intermediate so the multiply can't overflow at 4K-class
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
    bool independentBlend_ = false;   // the device feature (blended multi-target pipelines mask targets 1+)

    // Whether the descriptor-indexing features the ray path's bindless texture table needs were
    // both SUPPORTED and ENABLED at device creation, the only place that knows. Read by queryCaps
    // to set DeviceCaps::rtBindlessTextures.
    bool bindlessCapable_ = false;

    bool hasSwapchain_ = false;
    bool vsync_ = true;
    bool tearingSupported_ = false;   // VK_PRESENT_MODE_IMMEDIATE_KHR present in the surface's list; fixed for the swapchain's life
    f32 clear_[4] = {0.10f, 0.12f, 0.16f, 1.0f};
    f32 sceneClear_[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // the same colour as the scene radiance that tonemaps back to it
    std::string adapterName_ = "Vulkan Device";

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
// D3D12's InfoQueue, so there is no drainDebugMessages()-shaped method here -- this free function
// IS the equivalent, routed to AVER_WARN/AVER_ERROR by severity.
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
    // NOT IMPLEMENTED ON THIS BACKEND YET; refused out loud rather than stubbed silently.
    // DeviceCaps::rtBindlessTextures already gates every caller, and this backend sets that bit
    // from real descriptor-indexing features, so reaching these at all means the gate was
    // bypassed -- worth a log, not a quiet zero. See the Vulkan parity stage.
    BindlessTableHandle createBindlessTextureTable(u32 capacity) override;
    void destroyBindlessTextureTable(BindlessTableHandle h) override;
    bool setBindlessTexture(BindlessTableHandle h, u32 index, TextureHandle t) override;
    u32  bindlessTableCapacity(BindlessTableHandle h) const override;
    BindingSetHandle createBindingSet(const BindingSetDesc& d) override;
    BlasHandle       createBlas(MeshHandle mesh) override;
    TlasHandle       createTlas(u32 maxInstances) override;
    // UPDATABLE twins: same allocation, built with ALLOW_UPDATE_BIT_KHR and a scratch sized for an
    // update too, so IRenderContext::refitBlas/refitTlas can update the result in place. See
    // IResourceFactory's contract comment above createBlasUpdatable/createTlasUpdatable.
    BlasHandle       createBlasUpdatable(MeshHandle mesh) override;
    TlasHandle       createTlasUpdatable(u32 maxInstances) override;
    BlasHandle       createBlasMulti(const BlasGeometry* geometries, u32 count) override;
    bool             setTlasStaticInstances(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    BufferHandle     tlasStaticInstanceBuffer(TlasHandle tlas) const override;
    u64              blasMemoryBytes(BlasHandle h) const override;
    u64              tlasMemoryBytes(TlasHandle h) const override;

    void destroyTexture(TextureHandle h) override;
    void destroyBuffer(BufferHandle h) override;
    void destroyBlas(BlasHandle h) override;
    MeshHandle blasMesh(BlasHandle h) const override;
    BlasHandle blasForMesh(MeshHandle mesh) const override;
    // Destroys every acceleration structure built from `mesh` -- a createBlasMulti one if ANY of its
    // geometries names it. Concrete, not part of IResourceFactory -- an implementation detail of
    // VulkanDevice::destroyMesh, mirroring
    // D3D12ResourceFactory::destroyBlasForMesh: a BLAS left behind would keep pointing ray tracing
    // at this mesh's freed vertex/index memory.
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

    // The raw handle for VulkanRenderContext's copy/barrier/vertex-bind paths -- mirrors D3D12's
    // bufferResource() exactly, including "null on a bad handle".
    VkBuffer bufferResource(BufferHandle h) const;
    // Wraps a VkImage/VkDeviceMemory this factory did NOT create (VulkanDevice::depthBuffer_/
    // depthMemory_) into an ordinary TextureHandle, reachable via setSrv/textureBarrier like any
    // factory-made texture. Concrete rather than on IResourceFactory: ONE specific adoption
    // (mirrors D3D12ResourceFactory::adoptExternalDepthTexture), not a general wrap-anything entry
    // point. Builds its OWN VkImageView (VK_IMAGE_ASPECT_DEPTH_BIT, kVkDepthFormat -- see
    // RhiTexture::externallyOwned) rather than reusing VulkanDevice's depthView_, which it destroys
    // on its own schedule. `existing`, when non-zero, is REUSED in place so the handle stays stable
    // across a resize (see VulkanDevice::depthTexHandle_). Returns the (possibly reused) handle, or
    // 0 on a null image or a failed view.
    TextureHandle adoptExternalDepthTexture(VkImage image, VkDeviceMemory memory, u32 width, u32 height,
                                             TextureHandle existing);

    // W4: fills every `dsts[i]` (`count` of them, `sizes[i]` bytes from `srcs[i]`) via ONE
    // HOST_VISIBLE staging buffer and ONE one-shot submit -- see createMesh's own comment for why
    // this exists (a Default-heap buffer refuses writeBuffer, needing Upload's persistent mapping)
    // and why it is SYNCHRONOUS (createMesh runs mid-frame too; vkCmdCopyBuffer is illegal inside
    // beginFrame's dynamic-rendering scope, so this uses a SEPARATE command buffer via
    // runOneShotCommands whose wait has already retired the staging buffer by the time this
    // returns -- nothing here is left for the caller to free). Every `dst` must be a buffer this
    // factory made with TRANSFER_DST usage, which toVkBufferUsage grants every buffer
    // unconditionally. `what` names the call in any failure log. False on any failure -- the
    // staging buffer is destroyed either way.
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
    // The table-SHAPE cache underneath descriptorLayout() -- see TableShapeEntry for why this is a
    // separate, finer-grained cache createBindingSet() shares with it.
    VkDescriptorSetLayout tableSetLayout(u32 srvCount, u32 uavCount,
                                         const SlotKind* srvKinds, const SlotKind* uavKinds);
    VkSampler getOrCreateSampler(const SamplerDesc& d);

    // The timeline value work recorded right now retires behind. Mirrors D3D12ResourceFactory::
    // retireFence() exactly, forwarding to VulkanDevice::retireFenceValue().
    u64 retireFence() const;
    // Queues `destroyFn` to run once `retireFence()`'s value has passed on the timeline semaphore.
    void retire(std::function<void()> destroyFn);
    // Walks the retired queue, running and popping every entry whose fence has passed. Called at
    // the top of every creation/destruction entry point, matching D3D12's own collect() call sites.
    void collect();

    // Constants sets (kVkSetConstants): setPipeline and setConstantBuffer take a fresh one per call.
    // They come from per-frame-slot pool chains that grow on demand and are reset whole once the slot's
    // GPU work has retired, so one frame or standalone submission can record any number of dispatches.
    VkDescriptorSet allocConstantsSet(VkDescriptorSetLayout layout);
    // Only after the GPU has finished everything recorded in `slot` (beginFrame's wait, standalone's idle).
    void resetConstantsPools(u32 slot);

private:
    // Fills a freshly created image from TextureDesc::initialData. The image must already be in
    // TRANSFER_DST_OPTIMAL; on success it has been transitioned to `d.initialState` and the GPU has
    // finished (a fence wait, not just a barrier -- the staging buffer is freed right after).
    bool uploadInitialData(VkImage image, const VkImageCreateInfo& ci, const TextureDesc& d, u32 mips);
    // Shared by createBlas/createBlasUpdatable and createTlas/createTlasUpdatable: identical except
    // for `allowUpdate`, which decides the build flags the prebuild size query is run against and
    // whether scratch is sized for an update as well as a build. Mirrors D3D12ResourceFactory's
    // createBlasImpl/createTlasImpl.
    BlasHandle createBlasImpl(MeshHandle mesh, bool allowUpdate);
    TlasHandle createTlasImpl(u32 maxInstances, bool allowUpdate);
    // Writes a valid, dimension-matched descriptor into every slot a BindingSetDesc declared but
    // the caller never wrote.
    //
    // WHY IT STAYS: unconditional, self-imposed policy, not a device requirement -- matches the
    // D3D12 original (docs/VULKAN.md), keeping the two backends identical, worth more than the
    // descriptors saved.
    //
    // CORRECTION: this used to justify the policy by saying descriptorBindingPartiallyBound
    // couldn't be assumed present. That no longer holds -- it, runtimeDescriptorArray,
    // shaderSampledImageArrayNonUniformIndexing and descriptorIndexing are all CORE
    // VkPhysicalDeviceVulkan12Features fields given this backend's hard-required VK_API_VERSION_1_3
    // plus four sibling 1.2/1.3 bits with no fallback (bufferDeviceAddress, timelineSemaphore,
    // dynamicRendering, synchronization2 -- queryRequiredFeatures, VulkanDevice.cpp:596-603), so a
    // bindless array here would be a feature-bit query, not an extension gamble -- unlike the
    // engine's D3D12-only FL 11_0 minimum (docs/MINIMUM_SPECS.md), which can't reach Vulkan 1.3 at
    // all.
    void nullFill(const RhiBindingSet& s);

    // Writes one descriptor into the CURRENT frame's ring slot and records it so the other ring
    // slots can be brought up to date later. See RhiBindingSet for why the ring exists.
    void writeBindingSlot(RhiBindingSet& s, bool isUav, u32 slot, const BindingSlotState& st);
    // The set to bind THIS frame, replaying any missed writes first. The one place a caller
    // should get a VkDescriptorSet out of an RhiBindingSet.
    VkDescriptorSet bindingSetForFrame(RhiBindingSet& s);

    // The VkShaderModule to build a pipeline of THIS layout from -- s.module when the layout makes
    // no cbuffer into root constants, otherwise a variant with patchPushConstantSlots applied,
    // compiled on first use and cached in s.variants. Null on a compile failure, which the caller
    // must treat as pipeline creation failing.
    // `outSpirv`, when given, receives the SPIR-V the returned module was built from -- what
    // reflectTableSlotKinds has to read, NOT RhiShader::spirv.
    VkShaderModule moduleForLayout(RhiShader& s, const PipelineLayout& layout, bool mesh, bool instanced,
                                   const std::vector<u32>** outSpirv = nullptr);

    VulkanDevice* dev_;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;   // the ONE pool every binding set + every fixed/post/feature descriptor set allocates from; FREE_DESCRIPTOR_SET_BIT so destroyBindingSet can actually free its set (deferred through retire()/collect(), same as every destroy here)

    std::vector<RhiTexture>    textures_;
    std::vector<RhiBuffer>     buffers_;
    std::vector<RhiShader>     shaders_;
    // std::deque, NOT std::vector -- same defect as its D3D12 twin: VulkanRenderContext caches a
    // raw `const RhiPipeline* pipe_` from &pipelines_[h-1], so a push_back that reallocates while
    // a command buffer is open leaves it dangling. FOUR growth sites here vs D3D12's two. MEASURED
    // on the D3D12 side (this backend can't be run): an ordinary 60-frame editor session relocates
    // the table twelve times, once while a pipeline is bound -- a container issue, fixed here too
    // even though OFF by default. Compile-verified with -DAVER_RHI_VULKAN=ON; NOT run -- this
    // backend has no editor UI.
    std::deque<RhiPipeline>    pipelines_;
    std::vector<RhiBindingSet> bindingSets_;
    std::vector<RhiBlas>       blases_;
    std::vector<RhiTlas>       tlases_;
    std::vector<DescriptorLayoutEntry> descriptorLayouts_;
    std::vector<TableShapeEntry>       tableShapes_;
    std::vector<SamplerCacheEntry>     samplers_;
    VkDescriptorSetLayout instanceSetLayout_ = VK_NULL_HANDLE;   // lazily built by instanceSetLayout(); owned here, destroyed in the shutdown sweep
    std::vector<RetiredObject>         retired_;

    struct ConstantsPool {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        u32 used = 0;   // sets allocated since the last reset (tracked, so allocation never fails on a full pool)
    };
    std::vector<ConstantsPool> constantsPools_[kFrameCount];
    u32 constantsPoolCursor_[kFrameCount] = {};

    friend class VulkanRenderContext;
    friend class VulkanDevice;
};

// ================================================================================================
// 13. VulkanRenderContext (IRenderContext). DEFINED IN VulkanRenderContext.cpp.
// ================================================================================================
class VulkanRenderContext final : public IRenderContext {
public:
    VulkanRenderContext(VulkanDevice* dev, VulkanResourceFactory* res) : dev_(dev), res_(res) {}
    // The context OWNS Vulkan memory (the per-frame constant ring, the shared zero CBV) and for a
    // long time had no destructor, so all three buffers were still alive at vkDestroyDevice.
    // IRenderContext's destructor is virtual and VulkanDevice deletes this through the base
    // pointer; this declaration was the only thing missing.
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
    // ONE vkCmdDrawIndexed WITH instanceCount, reading per-instance world matrices from a
    // StructuredBuffer at set kVkSetInstances -- twin of D3D12RenderContext::drawMeshInstanced's
    // root SRV at a raw GPU virtual address. NOT AN OPTIMISATION: an earlier note called the
    // inherited fallback (setConstants + drawMesh per instance) already correct here -- false:
    // Voxi's VSShadowInstanced/VSGiShadowInstanced read gInstanceWorlds[instanceID], never gWorld,
    // so the fallback drew every instance at instanceID 0, giving shadow cascades and the GI-only
    // shadow map undefined transforms on Vulkan. A DESCRIPTOR, not a push-constant device address
    // as an earlier design proposed: every buffer here DOES carry a VkDeviceAddress, but the HLSL
    // is SHARED with D3D12 (StructuredBuffer<float4x4> gInstanceWorlds : register(tN)) -- a
    // device-address design would fork text D3D12 compiles unchanged, exactly what this backend's
    // -fvk-bind-register map exists to avoid.
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
    // Binds a caller-owned index buffer. R32Uint ONLY -- matches D3D12RenderContext::setIndexBuffer's
    // REAL enforcement (rejects anything else), not its own header comment's aspirational
    // "R16Uint-equivalent" (the contract scout's finding), so both backends reject the same inputs.
    void setIndexBuffer(BufferHandle b, Format indexFormat) override;
    void drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) override;
    void buildBlas(BlasHandle blas) override;
    void buildTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    // ---- in-place updates (refit) -- see IRenderContext's contract comment above refitBlas/
    // refitTlas for the eligibility rules both follow. ----
    bool refitBlas(BlasHandle blas) override;
    bool refitTlas(TlasHandle tlas, const TlasInstance* instances, u32 count) override;
    void textureBarrier(TextureHandle t, ResourceState from, ResourceState to, u32 subresource) override;
    void bufferBarrier(BufferHandle b, ResourceState from, ResourceState to) override;
    void uavBarrierTexture(TextureHandle t) override;
    void uavBarrierBuffer(BufferHandle b) override;
    // DEBUG LABELS ONLY -- NO GPU TIMING BEHIND THESE, unlike D3D12RenderContext's pushMarker/
    // popMarker, which also timestamp each side and feed a per-node GPU timing tree (GpuSpan/
    // GpuAccum there). A declared gap, not a silent one; see this pair's comment in
    // VulkanRenderContext.cpp for what real parity would need.
    void pushMarker(const char* label) override;
    void popMarker() override;

    // Suballocates transient upload memory from THIS frame's ring, at dev_->minUboAlignment().
    // PUBLIC: VulkanDevice's post-chain methods call this SAME allocator for their per-pass
    // constants -- D3D12Device::postConstants() reuses D3D12RenderContext::ringAlloc's identical
    // logic, but here the two are genuinely the same function across the TU boundary, not
    // duplicated logic.
    ConstantAllocation ringAlloc(const void* data, u32 bytes);

private:
    // Gives every descriptor-CBV slot the current pipeline declares (constantDwords[k] == 0,
    // always including b0) a valid dynamic-offset binding before the next draw, so no draw can
    // read an unset one -- the Vulkan analog of D3D12RenderContext::bindDeclaredRootCbvs. Slot 0
    // (b0) is ALWAYS written here from VulkanDevice's current-frame PerFrameCB, with no caller
    // action, per section 4's scheme.
    void bindDeclaredDescriptors(const RhiPipeline* p);
    // Binds the sticky per-draw state (table 1 + its b2 block), if the current pipeline declared
    // anywhere to put it. Mirrors D3D12RenderContext::applyDrawBinding.
    void applyDrawBinding();
    // dev_->currentCommandBuffer() -- the one place every method above reaches the live command
    // buffer through, so a future frame-pacing change touches this one line, not every override.
    VkCommandBuffer cmd() const;

    // ---- shared tails for buildBlas/refitBlas and buildTlas/refitTlas ----
    // Records the actual BLAS build or in-place update for `b`'s mesh and marks it built.
    // MODE_BUILD_KHR (source left null) for a full build, MODE_UPDATE_KHR with src == dst == b.as
    // for an in-place refit -- both need the SAME flags the structure was created/last built with
    // (ALLOW_UPDATE_BIT_KHR whenever `b.allowUpdate`), matching the update contract in
    // RHIResources.hpp's comment above IRenderContext::refitBlas.
    void recordBlasBuild(RhiBlas& b, const GpuMesh& m, VkBuildAccelerationStructureModeKHR mode);
    // The createBlasMulti twin: one triangle geometry per b.geometries entry, opaque per entry, always
    // a full build (never updatable). False, nothing recorded, when a part's mesh is gone.
    bool recordBlasBuildMulti(RhiBlas& b);
    // Fills THIS frame's instance buffer for `t` from `instances` (`count` of them), applying the
    // same filtering buildTlas has always done (an instance naming a dead BLAS, or an id that does
    // not fit in 24 bits, is dropped -- logged under `caller`). Returns how many were actually
    // written, and fills `outSlots` with each written instance's {BLAS, flags, mask} in submission
    // order -- refitTlas's eligibility check against RhiTlas::builtSlots. Shared by buildTlas and
    // refitTlas so one packing loop serves both.
    u32 packTlasInstances(RhiTlas& t, const TlasInstance* instances, u32 count, const char* caller,
                          std::vector<RhiTlasSlot>& outSlots);
    // How many static-prefix slots this build may use: t.staticCount, or 0 with no prefix -- or with one
    // naming a BLAS destroyed since it was set, which is dropped (logged once) rather than traversed.
    u32 usableStaticPrefix(RhiTlas& t);
    // Records the actual TLAS build or in-place update over `staticUsed` prefix instances plus the
    // `written` ones packTlasInstances already put in this frame's instance buffer, and marks `t` built.
    // With a prefix, first copies those `written` in behind it on the GPU (barriers included) and builds
    // from the prefix buffer; without one, builds from the instance buffer exactly as before. Same
    // MODE_BUILD_KHR/MODE_UPDATE_KHR split as recordBlasBuild above.
    void recordTlasBuild(RhiTlas& t, u32 staticUsed, u32 written, VkBuildAccelerationStructureModeKHR mode);

    VulkanDevice* dev_;
    VulkanResourceFactory* res_;
    const RhiPipeline* pipe_ = nullptr;
    // Shared zero-filled UBO for declared-but-unsupplied CBV slots -- the Vulkan analog of D3D12's
    // zeroCB_/zeroCbv().
    VkBuffer zeroCB_ = VK_NULL_HANDLE;
    VkDeviceMemory zeroCBMemory_ = VK_NULL_HANDLE;

    ConstantRing ring_[kFrameCount]{};
    // Per-instance world matrices, ringed like ring_ above for the same reason, but a SEPARATE
    // buffer: the constant ring is VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, a StructuredBuffer is
    // STORAGE. Same growth, same epoch-based reset.
    ConstantRing instanceRing_[kFrameCount]{};
    // Suballocates from instanceRing_ for one draw. Returns a null buffer on overflow, which the
    // caller must treat as "skip the draw", never as offset 0.
    ConstantAllocation instanceAlloc(const void* data, u32 bytes);
    // instanceRing_'s own frame epoch. NOT shared with ringEpoch_: both rings reset their bump
    // cursor on the first allocation of a new frame, and a shared epoch would let whichever ring
    // allocated first consume the frame transition, leaving the other never reset.
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
