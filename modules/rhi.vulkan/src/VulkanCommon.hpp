// Internal shared header for the Vulkan RHI backend. Included ONLY by .cpp files inside
// modules/rhi.vulkan/src/ — there is no include/ directory for this module (aver_add_module only
// exposes a module's include/ PUBLICly; see cmake/AvModule.cmake), so nothing outside this
// directory can see a VkFoo. That is the whole point of the RHI (house rule 3): the equivalent
// property for D3D12 already holds (d3d12.h appears nowhere outside modules/rhi.d3d12), and this
// header is what makes it hold here too.
//
// D3D12Device.cpp is one 5,423-line file implementing every backend object in one translation
// unit, in anonymous-namespace scope. This backend is FOUR TUs instead, so every type or function
// two of them must share needs EXTERNAL linkage, and that is what forces this header to exist at
// all. Everything below lives in namespace aver::rhi::vkb ("Vulkan backend") rather than directly
// in aver::rhi, so it reads unambiguously as backend-private even though it has external linkage —
// and so a same-named D3D12-side anonymous-namespace type (RhiTexture, hrOk, ...) is never a
// collision risk, textually or at link time.
//
// ================================================================================================
// FILE MAP — which of the four implementation files defines what. Read this before adding a
// method body anywhere: every declaration below names its one owner, and nothing here should ever
// be defined in two of them or in none of them.
// ================================================================================================
//
//   VulkanDevice.cpp
//     - class VulkanSwapchain (ISwapchain) — all four methods.
//     - class VulkanDevice (IDevice) — every override, plus:
//         init(), queryCaps(), initAccelerationStructures(), initMeshShaders(), dispatchMesh(),
//         createPipeline() (the FIXED scene/wire/sky/line PSOs, not the generic factory path),
//         createSwapchainResources()/createRenderTargetViews()/createDepthBuffer()/createMsaaColor(),
//         waitForGpu()/waitTimeline(), present()/resize(), notifyRenderTargetsChanged(),
//         ensureViewportTexture(), seedSkinTargets(), packAtmosphere(), toSceneReferred(),
//         the camera post chain (createPostPipelines/createPostTargets/releasePostTargets/
//         runPostChain/postConstants — kept in this file exactly as D3D12 keeps its post chain
//         inside D3D12Device.cpp rather than splitting it out),
//         debugMessengerCallback() (the VkDebugUtilsMessengerEXT callback; Vulkan delivers debug
//         output by CALLBACK, not by polling an InfoQueue the way D3D12's drainDebugMessages() did,
//         so there is no equivalent poll method here — the callback itself IS the equivalent),
//         loadGlobalApi()/loadInstanceApi()/loadDeviceApi()/unloadApi() (VulkanApi bootstrap: this
//         is the first file that ever needs an instance or device to resolve function pointers
//         against, and every other file reaches the already-filled table through
//         VulkanDevice::api()),
//         aver::rhi::detail::createVulkanDevice() itself (replacing the stub), matching the
//         forward declaration in modules/rhi/src/RHI.cpp exactly.
//
//   VulkanShaderCompiler.cpp
//     - class VulkanShaderCompiler — init()/usingDxc()/compile(). The DXC-with-`-spirv` wrapper;
//       the Vulkan analog of D3D12Device.cpp's private ShaderCompiler class, promoted to its own
//       TU (and to external linkage) because BOTH VulkanDevice.cpp (the fixed pipelines' shaders)
//       and VulkanResourceFactory.cpp (createShader) need it.
//     - vulkanShaderCompiler() — the process-wide singleton accessor.
//
//   VulkanResourceFactory.cpp
//     - class VulkanResourceFactory (IResourceFactory) — every override, plus every private
//       helper declared on it below: the descriptor-set-layout / pipeline-layout cache
//       (descriptorLayout(), tableSetLayout()), the sampler cache (getOrCreateSampler()), the
//       push-constant layout calculator (pushConstantLayout() — a free function, but this is its
//       one and only definition site since nothing else needs it before a pipeline is built),
//       nullFill(), uploadInitialData(), and the deferred-destruction machinery (retireFence()/
//       retire()/collect()).
//     - createBufferCommitted()/createImageCommitted()/destroyBufferCommitted()/
//       destroyImageCommitted() — free functions, the multi-TU analog of D3D12's
//       CreateCommittedResource, DEFINED HERE because this file already owns allocation policy
//       (memory-type selection, budget tracking) for its own createTexture/createBuffer, and
//       every other file that needs a raw buffer/image outside the handle tables (VulkanDevice's
//       per-frame CBs and line meshes, VulkanRenderContext's BLAS/TLAS scratch and instance
//       buffers) calls back into the SAME policy rather than growing a second one.
//
//   VulkanRenderContext.cpp
//     - class VulkanRenderContext (IRenderContext) — every override, plus ringAlloc(),
//       bindDeclaredDescriptors(), applyDrawBinding(), cmd().
//
// Every free INLINE helper below (format/state/filter/compare/vertex-layout conversions, vkOk,
// findMemoryType, sameLayout/sameSampler, storeDrawBinding, setVkObjectName) is defined RIGHT HERE
// in the header, not assigned to any one .cpp: each is a pure function of its arguments with no
// ties to a live VulkanDevice, so `inline` in a shared header is the direct, correct multi-TU
// equivalent of D3D12Device.cpp's anonymous-namespace free functions of the same shape (hrOk,
// toResourceStates, toComparison, toFilter, toDxgiFormat, semanticName, texelBytes, ...).
#pragma once

// ---------------------------------------------------------------------------------------------
// Platform / loader configuration. Both are also set as PRIVATE compile definitions in this
// module's CMakeLists.txt; the #ifndef guards here just make this header self-sufficient even if
// that ever changes. VK_NO_PROTOTYPES matters: there is no vulkan-1.lib anywhere on this machine
// (confirmed — the SDK that ships it is deliberately not installed, see
// third_party/vulkan-headers/README.md), so nothing in this module may reference an unprefixed
// vkFoo() symbol expecting the linker to resolve it. Every entry point is a function pointer in
// VulkanApi below, resolved at RUNTIME from C:\Windows\System32\vulkan-1.dll (which IS present —
// it ships with the GPU driver, not the SDK) via LoadLibraryW + GetProcAddress, starting from
// vkGetInstanceProcAddr itself.
// ---------------------------------------------------------------------------------------------
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>   // vendored at third_party/vulkan-headers/include, v1.3.296, Apache-2.0

#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <cstdlib>
#include <string>
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

// This backend's minimum requirement. Requested as VkApplicationInfo::apiVersion; if instance or
// device creation then fails, VulkanDevice::init() returns false and createVulkanDevice() reports
// unavailable exactly like any other backend that fails to initialise — see DeviceDesc's own
// comment on why "can be asked for and honestly reports absent" matters. Chosen because
// synchronization2, dynamic_rendering, buffer_device_address and timeline_semaphore — every one of
// which this backend's frame-pacing and render-target model depends on structurally, not
// optionally — are all CORE at 1.3. There is no "requires the extension instead" fallback path:
// see the architecture scout's note that a 1.2-only driver would need VK_KHR_synchronization2 and
// VK_KHR_dynamic_rendering as extensions, and the deliberate choice made here is not to carry that
// second path until a real 1.2-only machine actually needs it.
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
inline constexpr const char* kOptionalDeviceExtensions[] = {
    VK_EXT_MESH_SHADER_EXTENSION_NAME,                  // -> DeviceCaps::meshShaderTier
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,       // -> DeviceCaps::rayTracingTier
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,     // dependency of acceleration_structure
    VK_KHR_RAY_QUERY_EXTENSION_NAME,                    // -> DeviceCaps::rayTracingTier (inline
                                                         //    RayQuery; see the contract scout's
                                                         //    finding that D3D12 here only ever
                                                         //    builds DXR 1.1 inline queries, never
                                                         //    a hit-group pipeline+DispatchRays, so
                                                         //    VK_KHR_ray_tracing_pipeline is NOT
                                                         //    needed)
    VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,   // -> DeviceCaps::conservativeRaster
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
        case Format::R32Typeless:    return 4;
        case Format::RG8Unorm:       return 2;
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
// nrm, uv0, in that order — modules/rhi/src/RHIShaders.cpp, off-limits to this module) already
// agrees with kMeshInputLayout's D3D12 semantic order, so binding by position 0/1/2 needs no
// [[vk::location(N)]] annotation added there. See the contract scout's finding 6.5 for the full
// reasoning, and re-verify against an actual DXC -spirv disassembly before trusting this in a real
// build — it is read from source, not from a compile.
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
//    one of these two mechanisms, by ONE fixed rule everything in this backend must agree with —
//    see the contract's write-up of why this is the single biggest structural fork from D3D12's
//    root-signature model.
// ================================================================================================
//
//   set kVkSetTable0 (0)     = PipelineLayout table 0: SRV registers t0..t(srvCount-1) at binding
//                              0..srvCount-1, UAV registers u0..u(uavCount-1) at binding
//                              kVkUavBindingBase..+uavCount-1.
//   set kVkSetTable1 (1)     = table 1, same scheme, restarted at binding 0/kVkUavBindingBase
//                              within its OWN set (srvCount1/uavCount1).
//   set kVkSetConstants (2)  = one VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC binding, at binding
//                              k, for every DECLARED slot k in [0, kMaxConstantSlots) whose
//                              constantDwords[k] == 0 (a "root CBV" in the D3D12 model). Slot 0
//                              (b0, the engine PerFrame block) is ALWAYS zero by convention
//                              (RHIResources.hpp:209) and so is ALWAYS one of these bindings, on
//                              EVERY pipeline, feature-declared or fixed — VulkanRenderContext
//                              writes its dynamic offset automatically on every setPipeline(),
//                              exactly as "bound by the backend on every pipeline bind"
//                              (RHIResources.hpp:340-341) already promises, with no caller action.
//   set kVkSetSamplers (3)   = s0..s(samplerCount-1) at binding 0..samplerCount-1, IMMUTABLE —
//                              baked into the VkDescriptorSetLayout at pipeline-layout build time
//                              via pImmutableSampler, matching D3D12's static samplers: never
//                              written at bind time, allocated and bound ONCE per DescriptorLayoutEntry.
//
//   PUSH CONSTANTS carry everything that is a D3D12 root 32-bit CONSTANT (constantDwords[k] != 0),
//   which in practice is exactly kObjectConstantRegister (b1, ALWAYS present, ALWAYS
//   kObjectConstantDwords=32 dwords) plus whichever other slot(s) a feature declares that way, PLUS
//   — mesh/amplification pipelines only — the reserved mesh-geometry block: the vertex and index
//   buffer device addresses (VK_KHR_buffer_device_address; D3D12's raw root-SRV bind has no
//   descriptor-model equivalent, and this is the direct one) and the 4-dword count block D3D12
//   root-binds at kMeshGeometryConstantRegister (b5). See PushConstantLayout/pushConstantLayout()
//   below for the exact byte offsets, which VulkanResourceFactory (building VkPushConstantRange)
//   and VulkanRenderContext (calling vkCmdPushConstants) must agree on byte-for-byte.
constexpr u32 kVkSetTable0    = 0;
constexpr u32 kVkSetTable1    = 1;
constexpr u32 kVkSetConstants = 2;
constexpr u32 kVkSetSamplers  = 3;
constexpr u32 kVkDescriptorSetCount = 4;
// UAV bindings within a table's set start here, so a layout whose declared srvCount grows later
// never renumbers an already-cached UAV binding. kMaxBindingSlots (16, RHIResources.hpp) already
// bounds one binding SET's slot count, so 0..15 for SRVs / 16..31 for UAVs never collide.
constexpr u32 kVkUavBindingBase = kMaxBindingSlots;

// Moves the shared prelude's per-frame constant buffer to the descriptor set this backend
// actually binds it in. MUST be applied to any HLSL that includes sharedShaderPrelude() before
// it is handed to the compiler.
//
// WHY: the prelude (modules/rhi/src/RHIShaders.cpp, shared with D3D12 and off-limits to this
// module) declares `cbuffer PerFrame : register(b0)` with no register space, because a space is
// meaningless to the D3D12 root signature that reads the same text. DXC maps HLSL register space
// to SPIR-V descriptor set, so with no space the block lands at SET 0 -- and set 0 in this
// backend is table 0 (SRVs/UAVs, see section 4), with the per-frame UBO at set kVkSetConstants.
// Every shader that touches gViewProj therefore named a descriptor its own pipeline layout did
// not contain.
//
// HOW THAT FAILED, and why it was not obvious: AMD's two shader compilers disagree about what to
// do with it. The integrated GPU's driver returns VK_ERROR_INVALID_SHADER_NV from
// vkCreateGraphicsPipelines -- recoverable, diagnosable. The discrete card's LLPC calls abort()
// instead, which surfaces as the process dying with 0xC0000409 inside amdvlk64.dll and no
// message at all. Same defect, and only one of the two ways of hitting it looks like a bug in
// this repo.
//
// The annotation is Vulkan-only DXC syntax and is inserted into the assembled string this module
// alone owns -- the shared prelude is never modified, and the D3D12 backend never sees this. Same
// approach, and the same reasoning, as patchPushConstants in VulkanDevice.cpp.
inline bool patchPerFrameSet(std::string& src) {
    const std::string needle = "cbuffer PerFrame : register(b0)";
    const std::size_t pos = src.find(needle);
    if (pos == std::string::npos) return false;
    // Prefixed rather than rewriting the register: `register(b0)` still names the D3D-side slot,
    // and [[vk::binding(binding, set)]] overrides only the SPIR-V placement.
    src.insert(pos, "[[vk::binding(0, " + std::to_string(kVkSetConstants) + ")]] ");
    return true;
}

// WHY THERE IS NO GENERAL VERSION OF patchPerFrameSet, having tried to write one.
//
// The obvious generalisation -- annotate EVERY `cbuffer X : register(bN)` into kVkSetConstants --
// is wrong, and wrong in a way that only the validation layer shows. It moved SkinParams from set 0
// to set 2 and the layer immediately said set 2 binding 3 was not declared either, because
// SkinningPass declares `constantDwords[3] = 4`: b3 is PUSH CONSTANTS, and a block cannot be both a
// push constant and a descriptor.
//
// THE RULE IS NOT A PROPERTY OF THE SHADER TEXT. A cbuffer is a descriptor iff its slot's
// constantDwords is ZERO, and that lives in the PipelineLayout, which shader compilation does not
// have -- VulkanResourceFactory::createShader compiles a module with no idea which pipeline will
// later use it. So no patch operating on source alone can decide this correctly, and one that
// guesses turns a diagnosable "wrong set" into an equally broken "right set, wrong kind".
//
// WHAT IS ACTUALLY LEFT, named so the next person does not rediscover it one crash at a time:
// patchPushConstants (VulkanDevice.cpp) handles exactly ONE push-constant block, PerObject at b1,
// by exact text. Every OTHER non-zero constantDwords slot -- SkinParams at b3 today -- still
// compiles as a descriptor and names a binding no layout contains. Fixing it means teaching this
// backend to emit [[vk::push_constant]] for an ARBITRARY slot, which needs the PipelineLayout
// threaded into compilation. That is a real change to the interface, not a patch, and it is the
// same conclusion VulkanShaderCompiler.cpp's own banner reached by a different route.
//
// AMD's discrete driver does not report any of this. LLPC calls abort(), so each one is the process
// dying at 0xC0000409 with nothing printed -- which is why the validation layer, not another guess,
// is the tool for the rest of it.



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

// ================================================================================================
// 5. Per-frame constant ring: the Vulkan analog of D3D12RenderContext::ringAlloc. One HOST_VISIBLE
//    buffer per frame in flight, bump-allocated at the device's queried
//    minUniformBufferOffsetAlignment (NOT a fixed 256 — see VulkanDevice::minUboAlignment()) and
//    bound as a VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC through set kVkSetConstants; the dynamic
//    OFFSET returned here is what a raw root-CBV's GPU address was in the D3D12 model, preserving
//    "no fresh descriptor write per draw". OWNED BY VulkanRenderContext.cpp (the scene/post-chain
//    ring) — see ConstantRing's own note on the one exception.
// ================================================================================================
// Transient constant bytes per frame in flight: starting size and the ceiling growth stops at.
//
// DECLARED HERE, ABOVE ConstantRing, BECAUSE ConstantRing USES IT. These two lines used to sit
// about thirty lines BELOW that struct, so every translation unit in this module failed on
// `kRhiRingBytes: identifier not found`. Nothing noticed, because the module could not be
// CONFIGURED either -- its CMakeLists named a source file that did not exist, so CMake failed
// before a compiler ever ran. Two breakages stacked, and fixing the outer one is what finally
// surfaced this one.
//
// The D3D12 backend has its own constant of the same name in its own .cpp, deliberately not
// shared: its ring is an UPLOAD-heap buffer that is always mappable, while this one may not be
// HOST_COHERENT (see ConstantRing::coherent below). Same idea, different object, measured
// separately.
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
// 6. Per-frame / per-draw CPU-side structs. PerFrameCB is a BYTE-FOR-BYTE copy of
//    D3D12Device.cpp's own PerFrameCB (which itself must mirror `cbuffer PerFrame` in the shared
//    HLSL prelude field for field) — copied here rather than re-derived, per the contract scout's
//    explicit warning that a single stray float here is a silent cross-backend shading divergence
//    no compiler catches.
// ================================================================================================
struct PerFrameCB {
    f32 viewProj[16];
    f32 invViewProj[16];
    f32 camPos[4];
    f32 lightDir[4];
    f32 lightColor[4];
    f32 ambient[4];
    f32 skyZenith[4];
    f32 skyHorizon[4];
    f32 fogColor[4];
    f32 skyParams[4];
    f32 groundColor[4];
    f32 fogParams[4];
    f32 cloudParams[4];
    f32 cloudMotion[4];
    f32 atmoRayleigh[4];
    f32 atmoMie[4];
    f32 atmoOzone[4];
    f32 atmoPlanet[4];
    f32 atmoTune[4];
    f32 atmoSunE0[4];
    f32 fogInscatterRef[4];
    f32 furnace[4];
    f32 skySh[9][4];     // nine L2 SH coefficients of the sky, rgb; w unused
};
// Constants for every post pass. Byte-for-byte mirror of D3D12Device.cpp's PostCB, which itself
// mirrors `cbuffer AverPost : register(b0)` in rhi::postShaderSource().
struct PostCB {
    f32 tone[4];
    f32 dst[4];
    f32 src[4];
    f32 adapt[4];
    f32 limit[4];
    f32 misc[4];
};
static_assert(sizeof(PostCB) == 96, "the HLSL cbuffer mirrors this byte for byte");

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
#if AVER_RHI_TRACK_STATE
    std::string debugName;                   // owned copy: the desc's debugName is the caller's pointer
    std::vector<ResourceState> states;       // one entry per mip; a subresource index is a mip index here
#endif
    // The descriptor handed to the UI, cast to u64 -- see uiDescriptor()'s own note that this stays
    // unused while VulkanDevice does not override IDevice's ui* methods (see the CMakeLists note on
    // why imgui_impl_vulkan is a deliberately deferred decision). Kept so the field exists the day
    // it is wired up, rather than adding it to this struct's ABI later.
    u64 uiDescriptor = 0;
    // True for a record that WRAPS a VkImage/VkDeviceMemory this factory did NOT allocate -- today
    // that is only VulkanResourceFactory::adoptExternalDepthTexture's re-publication of
    // VulkanDevice::depthBuffer_/depthMemory_ as an ordinary TextureHandle (see IDevice::
    // sceneDepthTexture's contract in RHI.hpp). `image`/`memory` there are owned and torn down by
    // VulkanDevice itself (its destructor, resize(), setSampleCount()); this factory's own teardown
    // paths (destroyTexture(), ~VulkanResourceFactory()) both check this flag and must skip
    // destroyImageCommitted() when it is set, or a resize/shutdown double-frees a VkImage/
    // VkDeviceMemory Vulkan has already recycled. The VIEWS this record builds for itself (srvView
    // below) are NOT foreign -- they are still destroyed the ordinary way regardless of this flag.
    bool externallyOwned = false;
};

struct RhiBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;   // valid whenever created with SHADER_DEVICE_ADDRESS usage (see toVkBufferUsage) -- every buffer this factory creates, so always valid once `buffer` is
    BufferDesc desc{};
    u8* mapped = nullptr;          // Upload/Readback-kind buffers stay mapped for their whole life
    bool coherent = false;         // see ConstantRing::coherent's identical note
#if AVER_RHI_TRACK_STATE
    std::string debugName;
    ResourceState state = ResourceState::Common;
    bool stateFixed = false;       // Upload-heap-equivalent and AccelStructure-kind buffers reject every transition
#endif
};

struct RhiShader {
    std::vector<u32> spirv;
    ShaderStage stage = ShaderStage::Vertex;
    VkShaderModule module = VK_NULL_HANDLE;
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
};

// One allocated VkDescriptorSet for table 0 or table 1, plus the slot kinds it was declared with
// (for nullFill's dimension-matching and setSrv/setUav's slot-kind validation). Allocated from
// VulkanResourceFactory's single shared descriptorPool_ -- the Vulkan analog of D3D12's one
// shared shader-visible heap, at DESCRIPTOR-SET rather than DESCRIPTOR-RANGE granularity (see the
// architecture scout's finding #2 on why sets, not a bindless array, is the strategy here).
struct RhiBindingSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
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
// One explicit HLSL-register -> (set, binding) mapping, emitted as DXC's -fvk-bind-register.
//
// WHY THIS EXISTS, and why the -fvk-*-shift arguments beside it could not do the job: a shift moves
// binding NUMBERS within one space and cannot move anything into a different SET, nor permute two
// registers of the same class independently. The post chain needs both -- its six resources live in
// set kVkSetConstants in an order (b0, t0, t1, t2, u0, u1) that interleaves register classes -- so
// it needs a per-register map, which is exactly what this is. This is option (b) from the decision
// recorded at the top of VulkanShaderCompiler.cpp, chosen there precisely because it keeps the
// shared HLSL backend-neutral: no `space` annotation is added, and D3D12 reads the same source
// unchanged.
struct VkRegisterBind {
    char type;      // 'b', 't', 'u' or 's' -- the HLSL register class
    u32  number;    // the register index within that class
    u32  space;     // the HLSL register space, 0 for everything this engine declares
    u32  set;       // the Vulkan descriptor set it must land in
    u32  binding;   // and the binding within that set
};

class VulkanShaderCompiler {
public:
    // Loads dxcompiler.dll once (the SAME DLL the D3D12 backend loads for DXIL — see this module's
    // CMakeLists.txt for why no separate copy step is needed) and resolves DxcCreateInstance.
    void init();
    bool usingDxc() const;

    // Compiles one HLSL entry point to SPIR-V. `src` is the FULLY ASSEMBLED source (prelude already
    // concatenated by the caller — VulkanResourceFactory::createShader owns that concatenation,
    // mirroring D3D12ResourceFactory::createShader's own `src = d.prelude; src += d.source`).
    // `stage`+`minShaderModel` derive the DXC target profile (e.g. "cs_6_5") via
    // dxcTargetPrefix(stage) below; `defines` is semicolon-separated, identical shape to
    // ShaderDesc::defines. Appends `-spirv` (and the binding-shift arguments the descriptor scheme
    // in section 4 requires — see this method's own definition for the exact -fvk-*-shift values,
    // which must agree with tableSetLayout()/descriptorLayout() byte for byte, register for
    // register). False (and logged) on any failure, `outSpirv` left untouched.
    bool compile(const char* src, const char* entry, ShaderStage stage, u32 minShaderModel,
                 const char* defines, std::vector<u32>& outSpirv,
                 const VkRegisterBind* binds = nullptr, u32 bindCount = 0);

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
inline VkBufferUsageFlags toVkBufferUsage(const BufferDesc& d) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (d.kind == BufferKind::AccelStructure)
        u |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
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
    IResourceFactory* resources() override;
    IRenderContext* renderContext() override;
    void addRenderFeature(IRenderFeature* f) override;
    void removeRenderFeature(IRenderFeature* f) override;
    // Non-owning, exactly like addRenderFeature/removeRenderFeature just above -- see
    // D3D12Device::setUpscaler's own comment (D3D12Device.cpp) for the invariant this preserves:
    // null is the PERMANENT default (nothing here ever assigns upscaler_ on its own), and every
    // branch that matters is gated on this pointer rather than a quality enum or a build flag, so
    // "no upscaler set" and "no AverSR module linked into this build at all" are the same code
    // path. Trivial enough to stay inline, same as setClearColor/setVSync below.
    void setUpscaler(IUpscaler* u) override { upscaler_ = u; }
    IUpscaler* upscaler() const override { return upscaler_; }
    Format backbufferFormat() const override { return fromVkFormat(kVkSceneColorFormat); }
    Format depthFormat() const override { return fromVkFormat(kVkDepthFormat); }
    u32 sampleCount() const override { return sampleCount_; }
    bool setSampleCount(u32 samples) override;
    // Decouples the scene's own render targets from the swapchain's -- see IDevice::setRenderScale's
    // contract comment (RHI.hpp) for the exact semantics this must reproduce: the scene renders at
    // round(present * scale) while everything present-resolution (the backbuffer, the viewport
    // texture, capture) stays pinned to width_/height_, untouched. NOT inline: this backend had no
    // scene/present size split before this method existed (createDepthBuffer/createMsaaColor/
    // beginFrame all sized directly off width_/height_ -- see those methods' own updated comments),
    // so the setter has real work to do -- computeSceneSize() plus a full rebuildSceneTargets() pass
    // when a swapchain already exists. Defined in VulkanDevice.cpp with every other IDevice
    // override, per the FILE MAP banner at the top of this header.
    void setRenderScale(f32 scale) override;
    f32 renderScale() const override { return renderScale_; }
    ISwapchain* createSwapchain(const SwapchainDesc& desc) override;
    void beginFrame() override;
    void endFrame() override;
    void setClearColor(f32 r, f32 g, f32 b, f32 a) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; clear_[3] = a; }
    void setVSync(bool on) override { vsync_ = on; }
    bool vsync() const override { return vsync_; }
    bool vsyncCanDisable() const override { return tearingSupported_; }
    void setViewportRect(u32 x, u32 y, u32 w, u32 h) override;
    void setViewportToTexture(bool on) override { viewportToTex_ = on; }
    bool viewportToTexture() const override { return viewportToTex_; }
    u64 viewportTextureId() override;
    bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) override;
    MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount, const u32* indices, u32 indexCount) override;
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

    // ---- same-frame depth prepass -- see IDevice's own comment (RHI.hpp) for the full contract,
    // and D3D12Device's own block of the same name (D3D12Device.cpp:1005-1013) for the shape this
    // mirrors. OFF by default: --depth-prepass measured as a LOSS on this engine's scenes (see
    // aver-frame-budget in project memory), so this exists for correctness/parity, not speed, and
    // the default must reproduce today's Vulkan behaviour (no prepass) exactly.
    void setDepthPrepassEnabled(bool on) override { depthPrepassEnabled_ = on; }
    bool depthPrepassEnabled() const override { return depthPrepassEnabled_; }
    void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16]) override;
    // AUTO-CONSUMED by the very next drawMesh() call, not stored past it -- see IDevice's own
    // comment. Plain assignment: nothing here decides whether the upcoming draw is ELIGIBLE (a
    // skinned mesh, say), only what the CALLER already believes about it; drawMesh() itself still
    // re-checks meshVertexBuffer(mesh) before trusting this, exactly as D3D12Device::drawMesh does.
    void setNextDrawPrepassed(bool prepassed) override { nextDrawPrepassed_ = prepassed; }
    // See IDevice::sceneDepthTexture's own contract comment (RHI.hpp) and D3D12Device::
    // sceneDepthTexture (D3D12Device.cpp:1993-2006) for the shape this mirrors: lazily (re)adopts
    // depthBuffer_ into rhiFactory_'s texture table via depthTexDirty_ -- see that flag's own
    // comment for why a dirty bit, not a size comparison, is the trigger. modules/occlusion's HZB
    // seed pass is the one consumer today; see this method's own definition (VulkanDevice.cpp) for
    // what had to change ELSEWHERE (createDepthBuffer's image usage flags, a new ownership flag on
    // RhiTexture) for the handle this returns to actually be safe to sample and safe to tear down.
    TextureHandle sceneDepthTexture() override;
    LineHandle createLineMesh(const LineVertex* verts, u32 count) override;
    void drawLines(LineHandle mesh, const f32 world[16]) override;
    void setMeshShaders(bool enabled) override;
    bool meshShadersActive() const override { return msActive_; }
    void setWireframe(bool on) override { wireframe_ = on; }
    void setLineDepth(bool testDepth) override { lineDepth_ = testDepth; }
    void requestCapture(u32 x, u32 y) override { capX_ = x; capY_ = y; captureReq_ = true; captureReady_ = false; }
    bool getCapture(f32 outRGBA[4]) override;
    bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) override;
    // NOTE ON THE UI METHODS (uiInit/uiNewFrame/uiShutdown/uiActive/uiWantsMouse/uiWantsKeyboard/
    // uiTextureId): deliberately NOT overridden. They stay at IDevice's own defaults (false/no-op —
    // RHI.hpp:433-444), which is an honest "no in-window UI on this backend yet", not a broken
    // implementation of one. D3D12 hosts Dear ImGui through imgui_impl_dx12; Vulkan's equivalent is
    // imgui_impl_vulkan, a DIFFERENT, NOT-currently-vendored ImGui backend — adding it is a real new
    // dependency (license, vendoring, a render-target/command-buffer contract of its own) and an
    // explicit separate decision, not implied by "implement the Vulkan RHI backend". See this
    // module's CMakeLists.txt for the same note next to AVER_ENABLE_UI.

    // ---- plain (non-override) helpers, mirroring D3D12Device's own public non-interface surface ----
    void present();
    void resize(u32 w, u32 h);
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // ---- accessors for the free helpers and the two friend classes below ----
    const VulkanApi& api() const { return api_; }
    VkDevice vkDevice() const { return device_; }
    VkPhysicalDevice vkPhysicalDevice() const { return physicalDevice_; }
    const VkPhysicalDeviceMemoryProperties& memoryProperties() const { return memoryProps_; }
    VkQueue graphicsQueue() const { return queue_; }
    u32 graphicsQueueFamily() const { return graphicsQueueFamily_; }
    u32 frameIndexInFlight() const { return frameIndex_; }
    VkCommandBuffer currentCommandBuffer() const { return commandBuffers_[frameIndex_]; }
    // The timeline value work recorded RIGHT NOW will retire behind -- the Vulkan analog of
    // D3D12ResourceFactory::retireFence(). One past nextTimelineValue_: the CURRENT frame's submit
    // signals nextTimelineValue_ + 1 (see the frame-pacing note on timeline_/nextTimelineValue_
    // below), so anything retired at that value is safe once THIS frame's submit has completed.
    u64 retireFenceValue() const { return nextTimelineValue_ + 1; }
    VkSemaphore timelineSemaphore() const { return timeline_; }
    VkDeviceSize minUboAlignment() const { return minUboAlignment_; }
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
    // changes with a swapchain already live. Mirrors D3D12Device::rebuildSceneTargets() exactly:
    // wait for the GPU (nothing sized off the old scene extent may still be in flight), drop the
    // depth/MSAA-colour images, recompute the scene size, invalidate the stored viewport sub-rect
    // (vpX_/vpY_/vpW_/vpH_ are stored in SCENE space -- see setViewportRect's own comment -- and are
    // stale the instant sceneWidth_/sceneHeight_ move), recreate the two scene targets at the new
    // size, drop the post chain's own size-dependent targets (releasePostTargets(), lazily rebuilt
    // by the next runPostChain), and renotify every registered render feature of the new size.
    void rebuildSceneTargets();
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
    // must be binary). See the architecture scout's finding #1. ----
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 nextTimelineValue_ = 0;
    u64 frameTimelineValues_[kFrameCount] = {};   // the timeline value that retires each FRAME-IN-FLIGHT slot's last submit
    VkSemaphore imageAvailable_[kFrameCount] = {};
    std::vector<VkSemaphore> renderFinished_;     // one per SWAPCHAIN IMAGE (sized at swapchain creation), not per frame in flight
    u32 frameIndex_ = 0;    // 0..kFrameCount-1, the frame-IN-FLIGHT slot
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
    // one, so a caller that cached the handle across frames (modules/occlusion does not today, but
    // nothing stops a future one) never has to notice depthBuffer_ was reallocated underneath it.
    // Field-for-field mirror of D3D12Device's own depthTexHandle_/depthTexDirty_ pair
    // (D3D12Device.cpp:1130-1138).
    TextureHandle depthTexHandle_ = 0;
    // True whenever depthBuffer_ has (re)allocated since depthTexHandle_ was last refreshed --
    // createDepthBuffer() sets this every time it runs (initial creation, resize(), and
    // setSampleCount()'s MSAA-target rebuild all call it); sceneDepthTexture() clears it once it
    // has re-adopted the current depthBuffer_.
    bool depthTexDirty_ = true;

    // ---- fixed scene/line/sky pipelines (built via VulkanShaderCompiler + vkCreateGraphicsPipelines
    // directly -- NOT through VulkanResourceFactory's generic PipelineLayout cache, exactly as
    // D3D12Device builds its own rootSig_/pso_ separately from D3D12ResourceFactory's cache) ----
    VkDescriptorSetLayout sceneFrameSetLayout_ = VK_NULL_HANDLE;
    // ONE empty descriptor-set layout, for the DEVICE'S LIFETIME, used as the placeholder in every
    // pipeline layout whose set 0 or set 1 goes unused.
    //
    // Three call sites used to each build their own throwaway pair and destroy them the instant
    // CreatePipelineLayout returned. That reads as tidy and is not: the pipeline layout goes on
    // referencing them, and every pipeline built from it afterwards is created against a layout
    // whose set layouts are gone. The validation layer reports it once per site --
    //     "pCreateInfos[0].layout (VkPipelineLayout ...) references deleted object
    //      VkDescriptorSetLayout ..."
    // -- which is exactly three, and was three. Keeping one alive costs a single empty layout and
    // removes the whole class.
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
    SkyAtmosphere sky_{};
    bool wireframe_ = false;
    bool lineDepth_ = true;
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

    u32 width_ = 0, height_ = 0;
    // The scene's OWN render-target size: width_/height_ scaled by renderScale_, rounded, floored at
    // 1. Equal to width_/height_ whenever renderScale_ == 1.0 (the default) -- computeSceneSize()
    // guarantees that exactly (integer arithmetic cancels the multiply-then-divide identically to
    // D3D12Device.cpp's own sceneWidth_/sceneHeight_ pair, which this is a field-for-field mirror
    // of), so a build that never calls setRenderScale renders byte-for-byte what this backend
    // rendered before renderScale_ existed. Present-resolution state (width_/height_ themselves, the
    // swapchain, the capture buffer, ImGui were it hosted) never reads this pair; only the scene
    // depth/MSAA-colour targets (createDepthBuffer/createMsaaColor), the scene rendering-scope's
    // render area and default viewport (beginFrame/endFrame's sky draw), the post chain's resolve
    // target and bloom pyramid (createPostTargets/runPostChain), and what render features are told
    // via onRenderTargetsChanged key off this pair instead.
    u32 sceneWidth_ = 0, sceneHeight_ = 0;
    f32 renderScale_ = 1.0f;   // [0.25, 1.0]; see IDevice::setRenderScale
    void computeSceneSize() {
        sceneWidth_  = width_  ? static_cast<u32>(std::lround(static_cast<f32>(width_)  * renderScale_)) : 0;
        sceneHeight_ = height_ ? static_cast<u32>(std::lround(static_cast<f32>(height_) * renderScale_)) : 0;
        if (width_  && sceneWidth_  < 1) sceneWidth_  = 1;
        if (height_ && sceneHeight_ < 1) sceneHeight_ = 1;
    }
    // Present-space -> scene-space scaling for the editor's viewport sub-rect (setViewportRect takes
    // physical backbuffer pixels; the render target those pixels end up addressing is the SCENE one,
    // smaller than the backbuffer whenever renderScale_ < 1). Exact identity at renderScale_ == 1.0
    // (sceneWidth_ == width_, so v * sceneWidth_ / width_ == v) -- mirrors D3D12Device's
    // scaleToSceneW/H exactly, including the u64 intermediate so the multiply cannot overflow at
    // 4K-class present sizes.
    u32 scaleToSceneW(u32 v) const { return width_  ? static_cast<u32>((static_cast<u64>(v) * sceneWidth_)  / width_)  : v; }
    u32 scaleToSceneH(u32 v) const { return height_ ? static_cast<u32>((static_cast<u64>(v) * sceneHeight_) / height_) : v; }
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;   // scene sub-rect; w/h == 0 means full backbuffer
    u32 sampleCount_ = kDefaultSampleCount;
    DeviceCaps caps_{};

    // ---- mesh-shader geometry path ----
    bool msSupported_ = false, msActive_ = false, msRefusalLogged_ = false;
    // ---- ray tracing: VK_KHR_acceleration_structure + VK_KHR_ray_query present ----
    bool rtSupported_ = false;

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
    BindingSetHandle createBindingSet(const BindingSetDesc& d) override;
    BlasHandle       createBlas(MeshHandle mesh) override;
    TlasHandle       createTlas(u32 maxInstances) override;

    void destroyTexture(TextureHandle h) override;
    void destroyBuffer(BufferHandle h) override;
    void destroyBlas(BlasHandle h) override;
    MeshHandle blasMesh(BlasHandle h) const override;
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
    void setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle tlas) override;
    void setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;
    void setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle b, u32 stride, u32 count, u32 firstElement) override;

    bool writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) override;
    bool readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset) override;
    bool textureInfo(TextureHandle h, TextureDesc& out) const override;
    void waitIdle() override;

    // Backs IDevice::uiTextureId -- unused while VulkanDevice does not override the ui* methods
    // (see VulkanDevice's own note); kept so the surface exists the day that changes.
    u64 uiDescriptor(TextureHandle h);
    // The raw handle for VulkanRenderContext's copy/barrier/vertex-bind paths -- mirrors D3D12's
    // D3D12ResourceFactory::bufferResource() exactly, including "null on a bad handle".
    VkBuffer bufferResource(BufferHandle h) const;
    // Wraps a VkImage/VkDeviceMemory this factory did NOT create (VulkanDevice::depthBuffer_/
    // depthMemory_, made directly in VulkanDevice::createDepthBuffer() because it needs the
    // attachment-only depthView_ VulkanDevice itself renders through, at whatever sample count
    // setSampleCount() last chose -- createTexture() never makes either) into an ordinary
    // TextureHandle, so a caller reaches it through setSrv/textureBarrier exactly like any
    // factory-made texture. Concrete rather than part of IResourceFactory, same reasoning as
    // bufferResource above and the exact mirror of D3D12ResourceFactory::adoptExternalDepthTexture:
    // this is the mechanics of ONE specific adoption, not a general "wrap anything" entry point
    // every backend would need to grow. Builds its OWN VkImageView for sampled reads (aspect
    // VK_IMAGE_ASPECT_DEPTH_BIT, format kVkDepthFormat -- see RhiTexture::externallyOwned's comment
    // for why that view, unlike `image`/`memory`, IS this record's to own and destroy) rather than
    // reusing VulkanDevice's own depthView_, which VulkanDevice destroys on its own schedule.
    // `existing`, when non-zero, is REUSED in place rather than allocating a new slot -- see
    // VulkanDevice::depthTexHandle_'s own comment for why the handle has to stay stable across a
    // resize. Returns the (possibly reused) handle, or 0 if `image` is null or the view fails.
    TextureHandle adoptExternalDepthTexture(VkImage image, VkDeviceMemory memory, u32 width, u32 height,
                                             TextureHandle existing);

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
    const DescriptorLayoutEntry* descriptorLayout(const PipelineLayout& layout, bool mesh);
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
    // WHY IT STAYS: because the engine null-fills UNCONDITIONALLY as a self-imposed policy, not
    // because any device requires it. docs/VULKAN.md's own audit says so of the D3D12 original --
    // "nullFill never consults caps_.resourceBindingTier ... cite Tier 1 as the origin, not the
    // condition" -- and replicating that here keeps the two backends behaving identically, which is
    // worth more than the descriptors it saves.
    //
    // WHAT THIS COMMENT USED TO SAY, AND WHY IT IS NO LONGER TRUE: it read "rather than relying on
    // VK_EXT_descriptor_indexing's descriptorBindingPartiallyBound (which this backend does not
    // assume the driver has)". That reasoning has expired. descriptorBindingPartiallyBound,
    // runtimeDescriptorArray, shaderSampledImageArrayNonUniformIndexing and descriptorIndexing
    // itself are all fields of VkPhysicalDeviceVulkan12Features -- CORE since Vulkan 1.2, no
    // extension to hope for -- and this backend already requires VK_API_VERSION_1_3
    // (VulkanCommon.hpp:140) and already hard-requires four bits from that same family with no
    // fallback path at all: bufferDeviceAddress, timelineSemaphore, dynamicRendering and
    // synchronization2 (queryRequiredFeatures, VulkanDevice.cpp:596-603).
    //
    // So a bindless texture array here is a FEATURE-BIT QUERY, not an extension gamble, and it is
    // not blocked by anything this comment described. The engine's FL 11_0 minimum spec is a
    // D3D12 statement (docs/MINIMUM_SPECS.md); the hardware it protects cannot reach Vulkan 1.3 and
    // therefore never runs this backend at all.
    void nullFill(const RhiBindingSet& s);

    VulkanDevice* dev_;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;   // the ONE pool every binding set + every fixed/post/feature descriptor set allocates from; created with FREE_DESCRIPTOR_SET_BIT so destroyBindingSet can actually free its set (deferred through retire()/collect(), same as every other destroy here)

    std::vector<RhiTexture>    textures_;
    std::vector<RhiBuffer>     buffers_;
    std::vector<RhiShader>     shaders_;
    std::vector<RhiPipeline>   pipelines_;
    std::vector<RhiBindingSet> bindingSets_;
    std::vector<RhiBlas>       blases_;
    std::vector<RhiTlas>       tlases_;
    std::vector<DescriptorLayoutEntry> descriptorLayouts_;
    std::vector<TableShapeEntry>       tableShapes_;
    std::vector<SamplerCacheEntry>     samplers_;
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

    void setPipeline(PipelineHandle p) override;
    void setViewport(u32 x, u32 y, u32 w, u32 h) override;
    void setScissor(u32 x, u32 y, u32 w, u32 h) override;
    void setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) override;
    void clearDepth(TextureHandle depth, f32 value) override;
    void clearColor(TextureHandle target, const f32 color[4]) override;
    void setBindingSet(BindingSetHandle set, u32 table) override;
    void setConstants(u32 slot, const void* data, u32 dwords) override;
    void setConstantBuffer(u32 slot, const void* data, u32 bytes) override;
    void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) override;
    void drawMesh(MeshHandle mesh) override;
    // drawMeshInstanced (IRenderContext::drawMeshInstanced, RHIResources.hpp) is deliberately NOT
    // overridden here. D3D12RenderContext::drawMeshInstanced (D3D12Device.cpp:6068-6091) is an
    // ACCELERATION path, not a semantics change: one DrawIndexedInstanced reading per-instance world
    // matrices from a ring-allocated buffer through a root SRV bound to a raw GPU virtual address,
    // gated on the bound pipeline having reserved that extra root parameter at build time
    // (GraphicsPipelineDesc::instanced). The interface's own base-class default -- one setConstants
    // + drawMesh per instance -- is called out in that default's own comment as "a correct,
    // unaccelerated fallback... exactly what a caller would otherwise write by hand", and it is
    // ALREADY fully correct on this backend without any override: setConstants(kObjectConstantRegister,
    // ...) and drawMesh(mesh) both exist as ordinary overrides right here, so the inherited loop
    // reproduces D3D12's SEMANTICS (every instance drawn with its own world matrix) exactly.
    //
    // Porting the ACCELERATED path is a real, separate piece of work this method's body cannot hold
    // alone: GraphicsPipelineDesc::instanced is entirely unconsumed by VulkanPipeline.cpp today (no
    // reserved push-constant or descriptor slot for a per-instance buffer exists yet), so building it
    // means changing the pipeline-LAYOUT construction other in-flight work in this same module also
    // touches, for a backend this file's own header banner already says has never been compiled. The
    // natural Vulkan shape, if it is ever built, is a push-constant VkDeviceAddress read via
    // gl_InstanceIndex-equivalent shader indexing rather than a new descriptor binding -- every
    // Vulkan-created buffer already carries a valid device address (RhiBuffer::address) for exactly
    // this reason -- but that is a decision for whoever owns VulkanPipeline.cpp next, not something
    // to bolt on unreviewed from inside this render context.
    void dispatchMeshFor(MeshHandle mesh) override;
    void dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) override;
    void dispatch(u32 gx, u32 gy, u32 gz) override;
    void copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes, u64 dstOffset, u64 srcOffset) override;
    void copyTexture(TextureHandle dst, TextureHandle src) override;
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
