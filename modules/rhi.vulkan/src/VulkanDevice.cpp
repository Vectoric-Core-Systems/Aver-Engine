// Vulkan backend for Aver.RHI: instance/device bring-up, the swapchain, frame pacing, the
// backend's own fixed scene/sky/line/mesh pipelines, the camera post chain, and createVulkanDevice
// itself. This file owns everything the FILE MAP banner at the top of VulkanCommon.hpp assigns to
// VulkanDevice.cpp; read that banner before adding anything here, and see D3D12Device.cpp for the
// BEHAVIOUR this is matching -- structure differs a great deal, semantics should not.
//
// UNVERIFIED. House rule: no build, no run. Every signature below was checked BY EYE against the
// vendored vulkan_core.h (header version 1.3.296) and against VulkanCommon.hpp's own declarations,
// not compiled. See the honestState this session reports alongside this file for the specific
// places that could not be checked any other way.
#include "VulkanCommon.hpp"
#include "aver/rhi/ShaderFiles.hpp"   // scene.hlsl is loaded, not compiled in
#include "aver/rhi/vulkan/UiBackend.hpp"

#include <cstdlib>
#include "aver/rhi/Atmosphere.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

#include <filesystem>


namespace aver::rhi::vkb {

// ================================================================================================
// 0. File-local constants, HLSL text, and small helpers with no ties to a live VulkanDevice.
// ================================================================================================
namespace {

// Bloom pyramid depth cap and post-pass-slot bookkeeping. Vulkan-side twin of D3D12Device.cpp's
// identical constants; kept private to this TU exactly as D3D12's are private to its own.
constexpr u32 kMaxBloomMips = 6;
constexpr u32 kPostSlotPrefilter  = 0;
constexpr u32 kPostSlotHistogram  = 1;
constexpr u32 kPostSlotComposite  = 2;
constexpr u32 kPostSlotDownBase   = 3;
constexpr u32 kPostSlotUpBase     = kPostSlotDownBase + (kMaxBloomMips - 1);
// The AverSR path's own composite slot: same PSO as kPostSlotComposite but t0 is presentHdrTex_
// (already upscaled, HDR, present-sized) instead of the scene view. Written only when an upscaler is
// set (D3D12's kPostTripleCompositeUpscaled twin); an unwritten slot is never sampled since
// runPostChain only selects it after that frame's copy-and-execute succeeded.
constexpr u32 kPostSlotCompositeUpscaled = kPostSlotUpBase + (kMaxBloomMips - 1);
constexpr u32 kPostSlotCount      = kPostSlotCompositeUpscaled + 1;
constexpr f32 kHistogramMinLogLum = -10.0f;
constexpr f32 kHistogramMaxLogLum = 12.0f;
constexpr u32 kHistogramDownscale = 4;

// The mesh-shader geometry registers this backend's OWN fixed pipeline chooses, mirroring
// D3D12Device.cpp's kSceneMeshSrvBase=3 (an arbitrary frozen constant, not derived from any
// PipelineLayout -- see that constant's own comment). -D'd into the AVER_MS block the same way.
constexpr u32 kMeshSrvBase = 3;

// The scene/sky/line shading now lives in modules/rhi/shaders/scene.hlsl, loaded through
// rhi::shaderFile() and shared by both backends -- see that file for why it stopped being two
// hand-synced copies.

// The prelude and this backend's own shaders, joined once (Vulkan twin of D3D12Device.cpp's
// sceneShaderSource()). Keyed on shaderFileRevision(): a plain static made hot reload recompile
// stale text forever.
const std::string& sceneShaderSource() {
    static std::string src;
    static u64 built = ~0ull;
    if (built != shaderFileRevision()) {
        src = std::string(sharedShaderPrelude()) + shaderFile("scene.hlsl");
        built = shaderFileRevision();
    }
    return src;
}

// ---- the push-constant patch --------------------------------------------------------------------
// WHY: section 4 of VulkanCommon.hpp commits kObjectConstantRegister (b1) and MeshCB's triangle
// count (b5) to PUSH CONSTANTS -- the Vulkan analogue of D3D12's root 32-bit constants. DXC only
// emits a SPIR-V PushConstant block for an HLSL cbuffer annotated
// `[[vk::push_constant]]` in the SOURCE TEXT -- no compile flag promotes an ordinary
// `cbuffer X : register(bN)` -- and the shared prelude (RHIShaders.cpp, off-limits here) declares
// PerObject/MeshCB as ordinary cbuffers because D3D12 shares it and has no such concept. So this
// file patches the annotation into the ASSEMBLED source string it alone controls, before compiling.
//
// PerObject and MeshCB merge into ONE patched block, not two: SPIR-V allows at most one
// PushConstant interface block per entry point and MSMain uses fields from both (mesh bytes folded
// onto PerObject's tail, keeping PushConstantLayout's [0,128) object / mesh-count-after layout).
//
// gVerts/gIndices are NOT part of this patch: DXC's SPIR-V backend always lowers
// StructuredBuffer<T>/ByteAddressBuffer to a descriptor-bound resource, with no un-annotated HLSL
// spelling for a bare pointer -- see dispatchMesh() for how the fixed mesh pipeline binds them
// instead (a fresh descriptor set per draw), this backend's sharpest divergence from D3D12's raw
// root-SRV bind.
// Finds `cbuffer <name>` where it is DECLARED -- at the head of a line -- and returns the offset of
// its opening brace, or npos.
//
// BY NAME, NOT BY REGISTER, AND THAT IS THE WHOLE POINT. Every needle in this file used to spell the
// register out: "cbuffer PerObject : register(b1) {". The shared prelude now emits register numbers
// from C++ instead of hardcoding them --
//     cbuffer PerObject : register(AVER_CB_JOIN(b, AVER_OBJECT_CB)) {
// -- so every one of those needles missed, every fixed pipeline refused to build, and the backend
// fell back to D3D12 with a warning. It compiled green the whole time, because AVER_RHI_VULKAN was
// OFF in every shipped configuration. Matching the declaration and letting the register spelling be
// whatever the prelude wants removes a tripwire that fires on an unrelated edit and is invisible
// until someone turns this backend on.
//
// The line-head test keeps prose out: several files here discuss these cbuffers in comments, and
// patching one of those would produce a silently wrong shader instead of an error.
size_t findCbufferBrace(const std::string& src, const char* name, size_t* declPos = nullptr) {
    const std::string needle = std::string("cbuffer ") + name;
    size_t pos = src.find(needle);
    while (pos != std::string::npos) {
        size_t lineStart = src.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        bool blank = true;
        for (size_t i = lineStart; i < pos; ++i)
            if (src[i] != ' ' && src[i] != '\t') { blank = false; break; }
        // The character after the name must not be an identifier character, or "PerObject" would
        // match a hypothetical "PerObjectExtra".
        const size_t after = pos + needle.size();
        const bool wholeWord = after >= src.size() ||
                               (!std::isalnum(static_cast<unsigned char>(src[after])) && src[after] != '_');
        if (blank && wholeWord) {
            const size_t brace = src.find('{', pos);
            if (brace != std::string::npos) {
                if (declPos) *declPos = pos;
                return brace;
            }
        }
        pos = src.find(needle, pos + needle.size());
    }
    return std::string::npos;
}

bool patchPushConstants(std::string& src, bool mesh) {
    // Same reason as the call in patchCbuffersForLayout: findCbufferBrace matches the declaration,
    // but the body it extracts must not carry an unexpanded register macro into AverPcBlock.
    expandCbufferRegisters(src);
    // NOT ON A cbuffer -- the first version put it there, and DXC rejects that outright:
    //     error: 'push_constant' attribute only applies to global variables of struct type
    // Invisible until a compiler that could actually emit SPIR-V surfaced it (compilation used to
    // die earlier on "SPIR-V CodeGen not available").
    //
    // DXC wants a struct + ConstantBuffer<> of it, so each field is aliased back to a global of the
    // same name (`static float4x4 gWorld = gAverPc.gWorld;` is legal -- HLSL scopes struct members
    // separately from globals) so the prelude's own gWorld/... keeps resolving unchanged.
    //
    // THE ALIAS LIST IS HARDCODED AGAINST THE PRELUDE: if PerObject gains/loses a field this stops
    // matching and says so, rather than silently compiling a layout that disagrees with PushConstantLayout.
    size_t pos = std::string::npos;
    const size_t openBrace = findCbufferBrace(src, "PerObject", &pos);
    if (openBrace == std::string::npos) {
        AVER_ERROR("[RHI.Vulkan] the shared prelude no longer DECLARES a `cbuffer PerObject` at the "
                   "head of a line; this backend's push-constant patch has nothing to rewrite and "
                   "its fixed pipelines cannot compile correctly");
        return false;
    }
    const size_t close = src.find("};", openBrace);
    if (close == std::string::npos) {
        AVER_ERROR("[RHI.Vulkan] PerObject cbuffer has no closing brace; the shared prelude has drifted");
        return false;
    }
    // Kept verbatim (comments too) so the byte layout PushConstantLayout assumes stays exact.
    // From just past the opening brace, so the register spelling -- however the prelude writes it --
    // is dropped along with the `cbuffer PerObject : register(...)` header itself.
    const size_t bodyBegin = openBrace + 1;
    std::string body = src.substr(bodyBegin, close - bodyBegin);

    // Folded into the SAME block (see file header) -- its own declaration is deleted below. Located
    // the same way and for the same reason as PerObject: its register is emitted from C++ too.
    size_t meshDecl = std::string::npos;
    const size_t meshBrace = mesh ? findCbufferBrace(src, "MeshCB", &meshDecl) : std::string::npos;
    size_t meshEnd = std::string::npos;
    if (meshBrace != std::string::npos) {
        meshEnd = src.find('}', meshBrace);
        if (meshEnd != std::string::npos) {
            // Take the trailing ';' with it when there is one, so erasing leaves no stray statement.
            ++meshEnd;
            if (meshEnd < src.size() && src[meshEnd] == ';') ++meshEnd;
        }
    }
    const bool haveMesh = mesh && meshBrace != std::string::npos && meshEnd != std::string::npos;
    if (mesh && !haveMesh) {
        AVER_ERROR("[RHI.Vulkan] the shared prelude no longer DECLARES a `cbuffer MeshCB` this "
                   "backend's push-constant patch can fold into the push-constant block");
        return false;
    }
    if (haveMesh) body += "\n    uint gTriCount; uint3 _msPad;\n";

    std::string repl = "struct AverPcBlock {" + body + "};\n";
    repl += "[[vk::push_constant]] ConstantBuffer<AverPcBlock> gAverPc;\n";
    // One alias per field, so the prelude's own gWorld/gBaseColor/... keep resolving unchanged.
    repl += "static float4x4 gWorld        = gAverPc.gWorld;\n";
    repl += "static float4   gBaseColor    = gAverPc.gBaseColor;\n";
    repl += "static float4   gMaterial     = gAverPc.gMaterial;\n";
    repl += "static uint     gShadingModel = gAverPc.gShadingModel;\n";
    repl += "static float    gReflectance  = gAverPc.gReflectance;\n";
    repl += "static float    gF90          = gAverPc.gF90;\n";
    repl += "static float4   gEmissive     = gAverPc.gEmissive;\n";
    if (haveMesh) repl += "static uint gTriCount = gAverPc.gTriCount;\n";

    src.replace(pos, (close + 2) - pos, repl);

    // Delete MeshCB's own block now that its fields live in AverPcBlock. RE-LOCATED rather than
    // reusing the offsets found above: the replace() just above changed the string's length, so
    // meshDecl/meshEnd are stale the moment PerObject is rewritten.
    if (haveMesh) {
        size_t decl2 = std::string::npos;
        const size_t brace2 = findCbufferBrace(src, "MeshCB", &decl2);
        if (brace2 != std::string::npos) {
            size_t end2 = src.find('}', brace2);
            if (end2 != std::string::npos) {
                ++end2;
                if (end2 < src.size() && src[end2] == ';') ++end2;
                src.erase(decl2, end2 - decl2);
            }
        }
    }
    return true;
}

// ---- small Vulkan plumbing helpers, private to this TU ------------------------------------------

VkImageMemoryBarrier2 imgBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                                 VkAccessFlags2 srcAccess, VkPipelineStageFlags2 srcStage,
                                 VkAccessFlags2 dstAccess, VkPipelineStageFlags2 dstStage,
                                 u32 baseMip = 0, u32 mipCount = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseMip, mipCount, 0, 1};
    return b;
}
void pipelineBarrier(const VulkanApi& api, VkCommandBuffer cmd, const VkImageMemoryBarrier2* imgs, u32 imgCount,
                     const VkBufferMemoryBarrier2* bufs = nullptr, u32 bufCount = 0) {
    // A barrier against a resource that does not exist is DROPPED HERE, the one choke point every
    // device-side barrier goes through, rather than reaching the driver as
    //     "pImageMemoryBarriers[0].image Invalid VkImage Object 0x0"
    // -- naming the barrier but not which of this file's dozen-odd render targets was never created.
    // Callers build their arrays from members that are null until the target exists (msaaColor_,
    // sceneResolved_, bloom chain, presentHdrTex_, ...); this makes the gap harmless and LOUD.
    VkImageMemoryBarrier2 live[16];
    u32 liveCount = 0;
    for (u32 i = 0; i < imgCount; ++i) {
        if (imgs[i].image == VK_NULL_HANDLE) {
            AVER_WARN("[RHI.Vulkan] dropping an image barrier at index {} of {}: its VkImage is null, "
                      "so the target it names was never created", i, imgCount);
            continue;
        }
        if (liveCount < 16) live[liveCount++] = imgs[i];
    }
    if (liveCount == 0 && bufCount == 0) return;   // nothing left to say

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = liveCount; dep.pImageMemoryBarriers = liveCount ? live : nullptr;
    dep.bufferMemoryBarrierCount = bufCount; dep.pBufferMemoryBarriers = bufs;
    api.CmdPipelineBarrier2(cmd, &dep);
}
VkBufferMemoryBarrier2 bufBarrier(VkBuffer buf, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 srcStage,
                                  VkAccessFlags2 dstAccess, VkPipelineStageFlags2 dstStage,
                                  VkDeviceSize size = VK_WHOLE_SIZE) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf; b.offset = 0; b.size = size;
    return b;
}

// A zero-binding descriptor set layout, for a VkPipelineLayout slot section 4's scheme reserves
// (kVkSetTable0/1) but this backend's fixed pipelines never populate. Vulkan requires a real layout
// object at every set index up to the highest used -- no "skip this slot" -- but per spec a
// descriptor set layout need not outlive the vkCreatePipelineLayout call that consumes it, so callers
// create one, build the pipeline layout, and destroy it immediately; nothing here keeps it alive.
VkDescriptorSetLayout makeEmptySetLayout(const VulkanApi& api, VkDevice device) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 0;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    api.CreateDescriptorSetLayout(device, &ci, nullptr, &layout);
    return layout;
}


// Writes the default shading model + params into the tail of a per-draw b1 block. Byte-for-byte
// mirror of D3D12Device.cpp's writeShadingConstants: the PerObject cbuffer's tail fields
// (gShadingModel, gReflectance, gF90, _objPad, gEmissive) are the same 8 dwords on both backends.
void writeShadingConstants(f32* block, bool unlit = false) {
    const u32 model = unlit ? 1u : 0u;   // AVER_MODEL_UNLIT / AVER_MODEL_STANDARD
    std::memcpy(block + 24, &model, sizeof(model));
    block[25] = 0.04f;
    block[26] = 1.0f;
    block[27] = 0.0f;
    block[28] = block[29] = block[30] = block[31] = 0.0f;
}

} // namespace

// ================================================================================================
// 1. VulkanApi bootstrap.
// ================================================================================================
bool loadGlobalApi(VulkanApi& api) {
    api.module = LoadLibraryW(L"vulkan-1.dll");
    if (!api.module) {
        AVER_WARN("[RHI.Vulkan] vulkan-1.dll not found (no Vulkan-capable driver installed)");
        return false;
    }
    api.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(api.module, "vkGetInstanceProcAddr")));
    if (!api.GetInstanceProcAddr) {
        AVER_WARN("[RHI.Vulkan] vulkan-1.dll has no vkGetInstanceProcAddr - not a real Vulkan loader");
        FreeLibrary(api.module);
        api.module = nullptr;
        return false;
    }
#define AVER_VK_GLOBAL(name) \
    api.name = reinterpret_cast<PFN_vk##name>(reinterpret_cast<void*>(api.GetInstanceProcAddr(nullptr, "vk" #name)))
    AVER_VK_GLOBAL(CreateInstance);
    AVER_VK_GLOBAL(EnumerateInstanceExtensionProperties);
    AVER_VK_GLOBAL(EnumerateInstanceLayerProperties);
    AVER_VK_GLOBAL(EnumerateInstanceVersion);   // absent pre-1.1; nullptr is a valid "treat as 1.0"
#undef AVER_VK_GLOBAL
    if (!api.CreateInstance) {
        AVER_ERROR("[RHI.Vulkan] vkCreateInstance could not be resolved from vulkan-1.dll");
        return false;
    }
    return true;
}

bool loadInstanceApi(VulkanApi& api, VkInstance instance, bool debugUtilsAvailable) {
#define AVER_VK_INST(name) \
    api.name = reinterpret_cast<PFN_vk##name>(reinterpret_cast<void*>(api.GetInstanceProcAddr(instance, "vk" #name)))
    AVER_VK_INST(DestroyInstance);
    AVER_VK_INST(EnumeratePhysicalDevices);
    AVER_VK_INST(GetPhysicalDeviceProperties);
    AVER_VK_INST(GetPhysicalDeviceProperties2);
    AVER_VK_INST(GetPhysicalDeviceFeatures);
    AVER_VK_INST(GetPhysicalDeviceFeatures2);
    AVER_VK_INST(GetPhysicalDeviceMemoryProperties);
    AVER_VK_INST(GetPhysicalDeviceMemoryProperties2);   // core 1.1; MAY resolve null -- see its own field comment
    AVER_VK_INST(GetPhysicalDeviceQueueFamilyProperties);
    AVER_VK_INST(GetPhysicalDeviceFormatProperties);
    AVER_VK_INST(GetPhysicalDeviceImageFormatProperties);
    AVER_VK_INST(EnumerateDeviceExtensionProperties);
    AVER_VK_INST(CreateDevice);
    AVER_VK_INST(GetDeviceProcAddr);
    AVER_VK_INST(DestroySurfaceKHR);
    AVER_VK_INST(GetPhysicalDeviceSurfaceSupportKHR);
    AVER_VK_INST(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    AVER_VK_INST(GetPhysicalDeviceSurfaceFormatsKHR);
    AVER_VK_INST(GetPhysicalDeviceSurfacePresentModesKHR);
    AVER_VK_INST(CreateWin32SurfaceKHR);
#undef AVER_VK_INST
    if (debugUtilsAvailable) {
        api.CreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            reinterpret_cast<void*>(api.GetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT")));
        api.DestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            reinterpret_cast<void*>(api.GetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")));
    }
    const bool ok = api.EnumeratePhysicalDevices && api.CreateDevice && api.GetDeviceProcAddr &&
                    api.CreateWin32SurfaceKHR && api.GetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (!ok) AVER_ERROR("[RHI.Vulkan] a required instance-level entry point failed to resolve");
    return ok;
}

bool loadDeviceApi(VulkanApi& api, VkDevice device, const std::vector<std::string>& deviceExtensions) {
    auto has = [&](const char* name) {
        for (const std::string& e : deviceExtensions) if (e == name) return true;
        return false;
    };
#define AVER_VK_DEV(name) \
    api.name = reinterpret_cast<PFN_vk##name>(reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vk" #name)))
    AVER_VK_DEV(DestroyDevice);
    AVER_VK_DEV(GetDeviceQueue);
    AVER_VK_DEV(DeviceWaitIdle);
    AVER_VK_DEV(QueueSubmit);
    AVER_VK_DEV(QueueSubmit2);
    AVER_VK_DEV(QueueWaitIdle);
    AVER_VK_DEV(QueuePresentKHR);
    AVER_VK_DEV(CreateSwapchainKHR);
    AVER_VK_DEV(DestroySwapchainKHR);
    AVER_VK_DEV(GetSwapchainImagesKHR);
    AVER_VK_DEV(AcquireNextImageKHR);
    AVER_VK_DEV(CreateImage);
    AVER_VK_DEV(DestroyImage);
    AVER_VK_DEV(GetImageMemoryRequirements);
    AVER_VK_DEV(BindImageMemory);
    AVER_VK_DEV(CreateImageView);
    AVER_VK_DEV(DestroyImageView);
    AVER_VK_DEV(CreateBuffer);
    AVER_VK_DEV(DestroyBuffer);
    AVER_VK_DEV(GetBufferMemoryRequirements);
    AVER_VK_DEV(BindBufferMemory);
    AVER_VK_DEV(GetBufferDeviceAddress);
    AVER_VK_DEV(AllocateMemory);
    AVER_VK_DEV(FreeMemory);
    AVER_VK_DEV(MapMemory);
    AVER_VK_DEV(UnmapMemory);
    AVER_VK_DEV(FlushMappedMemoryRanges);
    AVER_VK_DEV(CreateFence);
    AVER_VK_DEV(DestroyFence);
    AVER_VK_DEV(WaitForFences);
    AVER_VK_DEV(ResetFences);
    AVER_VK_DEV(GetFenceStatus);
    AVER_VK_DEV(CreateSemaphore);
    AVER_VK_DEV(DestroySemaphore);
    AVER_VK_DEV(WaitSemaphores);
    AVER_VK_DEV(SignalSemaphore);
    AVER_VK_DEV(GetSemaphoreCounterValue);
    AVER_VK_DEV(CreateCommandPool);
    AVER_VK_DEV(DestroyCommandPool);
    AVER_VK_DEV(ResetCommandPool);
    AVER_VK_DEV(AllocateCommandBuffers);
    AVER_VK_DEV(FreeCommandBuffers);
    AVER_VK_DEV(BeginCommandBuffer);
    AVER_VK_DEV(EndCommandBuffer);
    AVER_VK_DEV(ResetCommandBuffer);
    AVER_VK_DEV(CmdPipelineBarrier2);
    AVER_VK_DEV(CmdCopyBuffer);
    AVER_VK_DEV(CmdFillBuffer);
    AVER_VK_DEV(CmdCopyBufferToImage);
    AVER_VK_DEV(CmdCopyImage);
    AVER_VK_DEV(CmdCopyImageToBuffer);
    AVER_VK_DEV(CmdClearColorImage);
    AVER_VK_DEV(CmdBeginRendering);
    AVER_VK_DEV(CmdEndRendering);
    AVER_VK_DEV(CmdBindPipeline);
    AVER_VK_DEV(CmdBindDescriptorSets);
    AVER_VK_DEV(CmdPushConstants);
    AVER_VK_DEV(CmdBindVertexBuffers);
    AVER_VK_DEV(CmdBindIndexBuffer);
    AVER_VK_DEV(CmdSetViewport);
    AVER_VK_DEV(CmdSetScissor);
    AVER_VK_DEV(CmdDraw);
    AVER_VK_DEV(CmdDrawIndexed);
    AVER_VK_DEV(CmdDispatch);
    AVER_VK_DEV(CreateShaderModule);
    AVER_VK_DEV(DestroyShaderModule);
    AVER_VK_DEV(CreateGraphicsPipelines);
    AVER_VK_DEV(CreateComputePipelines);
    AVER_VK_DEV(DestroyPipeline);
    AVER_VK_DEV(CreatePipelineLayout);
    AVER_VK_DEV(DestroyPipelineLayout);
    AVER_VK_DEV(CreateDescriptorSetLayout);
    AVER_VK_DEV(DestroyDescriptorSetLayout);
    AVER_VK_DEV(CreateDescriptorPool);
    AVER_VK_DEV(DestroyDescriptorPool);
    AVER_VK_DEV(ResetDescriptorPool);
    AVER_VK_DEV(AllocateDescriptorSets);
    AVER_VK_DEV(FreeDescriptorSets);
    AVER_VK_DEV(UpdateDescriptorSets);
    AVER_VK_DEV(CreateSampler);
    AVER_VK_DEV(DestroySampler);
#undef AVER_VK_DEV
    // NOT GUARDED BY has() -- A REAL BUG. VK_EXT_debug_utils is an INSTANCE extension, pushed into
    // instExts not deviceExtensions, so has() (device-list only) was always false: all three
    // pointers stayed null, every validation message showed a bare handle with no resource name
    // (e.g. "VkImage 0x27b000000027b"), and
    // pushMarker/popMarker were silently inert (no RenderDoc/Nsight pass labels). Loading them
    // unconditionally is self-guarding: vkGetDeviceProcAddr returns null when the instance didn't
    // enable the extension, and all three call sites already null-check.
    {
        api.CmdBeginDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT")));
        api.CmdEndDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT")));
        api.SetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT")));
    }
    if (has(VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        api.CmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkCmdDrawMeshTasksEXT")));
    }
    if (has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
        api.GetAccelerationStructureBuildSizesKHR = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR")));
        api.CreateAccelerationStructureKHR = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR")));
        api.DestroyAccelerationStructureKHR = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR")));
        api.CmdBuildAccelerationStructuresKHR = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR")));
        api.GetAccelerationStructureDeviceAddressKHR = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            reinterpret_cast<void*>(api.GetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR")));
    }
    const bool ok = api.DestroyDevice && api.CreateSwapchainKHR && api.QueueSubmit2 && api.CmdPipelineBarrier2 &&
                    api.CmdBeginRendering && api.CreateGraphicsPipelines;
    if (!ok) AVER_ERROR("[RHI.Vulkan] a required device-level entry point failed to resolve");
    return ok;
}

void unloadApi(VulkanApi& api) {
    if (api.module) FreeLibrary(api.module);
    api = VulkanApi{};   // every PFN_ dangles from here on, by design; see the header's own note
}

// ================================================================================================
// 2. Debug messenger callback. Vulkan delivers debug output by CALLBACK; there is no poll method
//    anywhere in this backend the way D3D12Device.cpp's drainDebugMessages() polls an InfoQueue.
// ================================================================================================
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                      void* /*userData*/) {
    if (!data || !data->pMessage) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        AVER_ERROR("[RHI.Vulkan] validation: {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        AVER_WARN("[RHI.Vulkan] validation: {}", data->pMessage);
    else
        AVER_TRACE("[RHI.Vulkan] validation: {}", data->pMessage);
    return VK_FALSE;   // never suppress the call the validation layer was about to make
}

// ================================================================================================
// 3. VulkanSwapchain -- four thin forwarders, exactly like D3D12Swapchain.
// ================================================================================================
void VulkanSwapchain::present()          { dev_->present(); }
void VulkanSwapchain::resize(u32 w, u32 h) { dev_->resize(w, h); }
u32  VulkanSwapchain::width() const      { return dev_->width(); }
u32  VulkanSwapchain::height() const     { return dev_->height(); }

// ================================================================================================
// 4. VulkanDevice::init and everything instance/device/swapchain/frame-pacing bring-up needs.
// ================================================================================================
namespace {
// Picks a queue family supporting both graphics and compute -- this backend, like D3D12Device's
// single DIRECT queue, does everything (graphics, compute, copy, present) on one queue. ~0u on
// failure.
u32 pickQueueFamily(const VulkanApi& api, VkPhysicalDevice phys) {
    u32 count = 0;
    api.GetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    api.GetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());
    constexpr VkQueueFlags kWant = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (u32 i = 0; i < count; ++i)
        if ((props[i].queueFlags & kWant) == kWant) return i;
    return ~0u;
}

// Every 1.2/1.3 core feature this backend structurally depends on (kRequiredApiVersion's own
// comment: synchronization2, dynamic_rendering, buffer_device_address, timeline_semaphore -- no
// fallback path). Queried once per candidate physical device.
struct RequiredFeatureCheck {
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
};
bool queryRequiredFeatures(const VulkanApi& api, VkPhysicalDevice phys, RequiredFeatureCheck& out) {
    out.f2.pNext = &out.v12;
    out.v12.pNext = &out.v13;
    if (!api.GetPhysicalDeviceFeatures2) return false;
    api.GetPhysicalDeviceFeatures2(phys, &out.f2);
    return out.v12.bufferDeviceAddress && out.v12.timelineSemaphore &&
           out.v13.dynamicRendering && out.v13.synchronization2;
}

bool deviceHasExtension(const VulkanApi& api, VkPhysicalDevice phys, const char* name) {
    u32 count = 0;
    api.EnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    api.EnumerateDeviceExtensionProperties(phys, nullptr, &count, props.data());
    for (const auto& p : props) if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}
} // namespace

bool VulkanDevice::init(const DeviceDesc& desc) {
    if (desc.useWarp) {
        // Honest, not silent: DeviceDesc::useWarp has no Vulkan analogue (no OS-shipped software
        // rasteriser the way D3D12 ships WARP) -- see softwareAdapter_'s own comment.
        AVER_WARN("[RHI.Vulkan] useWarp was requested; Vulkan has no software-rasteriser adapter to "
                  "select, so hardware selection proceeds as normal");
    }
    if (!loadGlobalApi(api_)) return false;

    // ---- instance ----
    u32 supportedApi = VK_API_VERSION_1_0;
    if (api_.EnumerateInstanceVersion) api_.EnumerateInstanceVersion(&supportedApi);
    if (supportedApi < kRequiredApiVersion) {
        AVER_WARN("[RHI.Vulkan] loader reports API version {}.{}, below the {}.{} this backend requires",
                  VK_API_VERSION_MAJOR(supportedApi), VK_API_VERSION_MINOR(supportedApi),
                  VK_API_VERSION_MAJOR(kRequiredApiVersion), VK_API_VERSION_MINOR(kRequiredApiVersion));
        unloadApi(api_);
        return false;
    }

    bool debugUtilsRequested = desc.enableDebug;
    bool debugUtilsAvailable = false;
    if (debugUtilsRequested) {
        u32 extCount = 0;
        api_.EnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        api_.EnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) { debugUtilsAvailable = true; break; }
        if (!debugUtilsAvailable)
            AVER_WARN("[RHI.Vulkan] debug requested but {} is not present (no SDK installed?) - "
                      "continuing without validation/debug labels", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    std::vector<const char*> instExts(std::begin(kRequiredInstanceExtensions), std::end(kRequiredInstanceExtensions));
    if (debugUtilsAvailable) instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Aver";
    appInfo.pEngineName = "Aver Engine";
    appInfo.apiVersion = kRequiredApiVersion;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = static_cast<u32>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.data();
    // VK_LAYER_KHRONOS_validation, requested only when both asked for AND present. Requesting it
    // unconditionally fails instance creation without the SDK (most machines); never asking made
    // --debug-layer a misnomer -- only the debug-utils messenger installed, reporting what the
    // DRIVER volunteers, silent while it access-violated. Enumerated, not assumed, like every
    // optional extension here.
    std::vector<const char*> instLayers;
    if (desc.enableDebug && api_.EnumerateInstanceLayerProperties) {
        u32 layerCount = 0;
        api_.EnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layerProps(layerCount);
        if (layerCount) api_.EnumerateInstanceLayerProperties(&layerCount, layerProps.data());
        for (const VkLayerProperties& lp : layerProps) {
            if (std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                instLayers.push_back("VK_LAYER_KHRONOS_validation");
                AVER_INFO("[RHI.Vulkan] VK_LAYER_KHRONOS_validation enabled");
                break;
            }
        }
        if (instLayers.empty())
            AVER_WARN("[RHI.Vulkan] --debug-layer asked for validation but no validation layer is "
                      "installed; only driver-volunteered messages will appear. Install the LunarG "
                      "Vulkan SDK to get real validation.");
    }
    ici.enabledLayerCount = static_cast<u32>(instLayers.size());
    ici.ppEnabledLayerNames = instLayers.empty() ? nullptr : instLayers.data();

    if (!vkOk(api_.CreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) {
        unloadApi(api_);
        return false;
    }
    if (!loadInstanceApi(api_, instance_, debugUtilsAvailable)) {
        api_.DestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        unloadApi(api_);
        return false;
    }
    if (debugUtilsAvailable && api_.CreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugMessengerCallback;
        api_.CreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &debugMessenger_);
        AVER_INFO("[RHI.Vulkan] debug utils messenger installed");
    }

    // ---- physical device ----
    u32 physCount = 0;
    api_.EnumeratePhysicalDevices(instance_, &physCount, nullptr);
    if (physCount == 0) { AVER_WARN("[RHI.Vulkan] no Vulkan-capable physical device enumerated"); return false; }
    std::vector<VkPhysicalDevice> phys(physCount);
    api_.EnumeratePhysicalDevices(instance_, &physCount, phys.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    u32 bestQueueFamily = ~0u;
    int bestScore = -1;
    VkPhysicalDeviceProperties bestProps{};
    for (VkPhysicalDevice p : phys) {
        VkPhysicalDeviceProperties props{};
        api_.GetPhysicalDeviceProperties(p, &props);
        if (props.apiVersion < kRequiredApiVersion) continue;
        if (!deviceHasExtension(api_, p, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        RequiredFeatureCheck feats;
        if (!queryRequiredFeatures(api_, p, feats)) continue;
        const u32 qf = pickQueueFamily(api_, p);
        if (qf == ~0u) continue;
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 2
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                       : 0;
        if (score > bestScore) { bestScore = score; best = p; bestQueueFamily = qf; bestProps = props; }
    }
    if (!best) {
        AVER_WARN("[RHI.Vulkan] no physical device meets this backend's hard requirements (API {}.{}, "
                  "swapchain, dynamicRendering, synchronization2, bufferDeviceAddress, timelineSemaphore)",
                  VK_API_VERSION_MAJOR(kRequiredApiVersion), VK_API_VERSION_MINOR(kRequiredApiVersion));
        return false;
    }
    physicalDevice_ = best;
    physicalDeviceProps_ = bestProps;
    graphicsQueueFamily_ = bestQueueFamily;
    api_.GetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProps_);
    adapterName_ = physicalDeviceProps_.deviceName;
    softwareAdapter_ = false;   // see the field's own comment; always false here, honestly
    minUboAlignment_ = physicalDeviceProps_.limits.minUniformBufferOffsetAlignment;
    minStorageAlignment_ = physicalDeviceProps_.limits.minStorageBufferOffsetAlignment;
    maxPushConstantsSize_ = physicalDeviceProps_.limits.maxPushConstantsSize;

    // ---- optional device extensions (each gates exactly one DeviceCaps bit; see the header's list) ----
    std::vector<const char*> devExtsC(std::begin(kRequiredDeviceExtensions), std::end(kRequiredDeviceExtensions));
    std::vector<std::string> devExts(std::begin(kRequiredDeviceExtensions), std::end(kRequiredDeviceExtensions));
    for (const char* opt : kOptionalDeviceExtensions) {
        if (deviceHasExtension(api_, physicalDevice_, opt)) { devExtsC.push_back(opt); devExts.emplace_back(opt); }
    }
    const bool wantMeshShader = std::find(devExts.begin(), devExts.end(), std::string(VK_EXT_MESH_SHADER_EXTENSION_NAME)) != devExts.end();
    const bool wantAccelStruct = std::find(devExts.begin(), devExts.end(), std::string(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) != devExts.end();
    const bool wantRayQuery = std::find(devExts.begin(), devExts.end(), std::string(VK_KHR_RAY_QUERY_EXTENSION_NAME)) != devExts.end();
    // M6: no feature struct to chain and enable -- VK_EXT_memory_budget adds only a queryable
    // pNext struct (VkPhysicalDeviceMemoryBudgetPropertiesEXT), never a VkPhysicalDeviceFeatures2
    // bit -- so "was it in the enabled list" is the whole story, recorded here from the same
    // devExts vector every other want* bool above reads.
    memoryBudgetExt_ = std::find(devExts.begin(), devExts.end(), std::string(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) != devExts.end();

    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    v12.pNext = &v13;
    void* tail = &v12;
    if (wantMeshShader)  { meshFeat.pNext = tail; tail = &meshFeat; }
    if (wantAccelStruct) { asFeat.pNext = tail;   tail = &asFeat; }
    if (wantRayQuery)    { rqFeat.pNext = tail;   tail = &rqFeat; }
    f2.pNext = tail;
    api_.GetPhysicalDeviceFeatures2(physicalDevice_, &f2);
    v12.bufferDeviceAddress = VK_TRUE;   // already confirmed supported above
    v12.timelineSemaphore = VK_TRUE;

    // THE THREE BITS THE RAY PATH'S BINDLESS TABLE NEEDS, required explicitly rather than left to
    // chance: v12 already carries every bit the device SUPPORTS and gets handed to vkCreateDevice as
    // the set to ENABLE, so these would likely be enabled incidentally -- fine on the machine it was
    // written on, fragile on a driver that reports differently.
    //
    //   runtimeDescriptorArray                    -- index an array whose size the shader doesn't know
    //   shaderSampledImageArrayNonUniformIndexing -- index varies ACROSS THE WAVE (neighbouring pixels
    //                                                hit different materials)
    //   descriptorBindingPartiallyBound            -- leave holes: the table is fixed-N with unbound
    //                                                 slots (kUnboundTexture), else validation rejects it
    bindlessCapable_ = v12.runtimeDescriptorArray &&
                       v12.shaderSampledImageArrayNonUniformIndexing &&
                       v12.descriptorBindingPartiallyBound;
    if (bindlessCapable_) {
        v12.runtimeDescriptorArray                    = VK_TRUE;
        v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        v12.descriptorBindingPartiallyBound           = VK_TRUE;
    } else {
        // Cleared for the same reason the mesh-shader bits below are: an unsupported bit left set
        // in the struct handed to vkCreateDevice is a device-creation failure, not a quiet no.
        v12.runtimeDescriptorArray                    = VK_FALSE;
        v12.shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
        v12.descriptorBindingPartiallyBound           = VK_FALSE;
    }
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    const bool meshShaderFeaturesOk = !wantMeshShader || (meshFeat.taskShader && meshFeat.meshShader);
    const bool accelStructFeaturesOk = !wantAccelStruct || asFeat.accelerationStructure;
    const bool rayQueryFeaturesOk = !wantRayQuery || rqFeat.rayQuery;
    if (!meshShaderFeaturesOk) { meshFeat.taskShader = meshFeat.meshShader = VK_FALSE; }
    // CLEARED for the same SUPPORTED-vs-ENABLE reason as the bindless bits above: multiviewMeshShader
    // needs multiview and primitiveFragmentShadingRateMeshShader needs primitiveFragmentShadingRate,
    // dependencies the engine never turns on -- caught by validation after the driver silently accepted them.
    meshFeat.multiviewMeshShader = VK_FALSE;
    meshFeat.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    // asFeat/rqFeat untouched on purpose: an extension can be present with its feature bit false,
    // and GetPhysicalDeviceFeatures2 already reported that correctly -- nothing to clear.

    const f32 prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphicsQueueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<u32>(devExtsC.size());
    dci.ppEnabledExtensionNames = devExtsC.data();
    // pEnabledFeatures left null: VkPhysicalDeviceFeatures2 in pNext is how features are requested
    // here, and the spec forbids both at once.
    if (!vkOk(api_.CreateDevice(physicalDevice_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
    if (!loadDeviceApi(api_, device_, devExts)) return false;
    api_.GetDeviceQueue(device_, graphicsQueueFamily_, 0, &queue_);

    rtSupported_ = accelStructFeaturesOk && wantAccelStruct && rayQueryFeaturesOk && wantRayQuery;

    // ---- frame pacing: the timeline semaphore every retire-fence value lives on ----
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semInfo.pNext = &typeInfo;
    if (!vkOk(api_.CreateSemaphore(device_, &semInfo, nullptr, &timeline_), "timeline semaphore")) return false;
    nextTimelineValue_ = 0;
    for (u32 i = 0; i < kFrameCount; ++i) frameTimelineValues_[i] = 0;

    VkSemaphoreCreateInfo binInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (u32 i = 0; i < kFrameCount; ++i)
        if (!vkOk(api_.CreateSemaphore(device_, &binInfo, nullptr, &imageAvailable_[i]), "acquire semaphore")) return false;

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (!vkOk(api_.CreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "command pool")) return false;

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFrameCount;
    if (!vkOk(api_.AllocateCommandBuffers(device_, &cbai, commandBuffers_), "command buffers")) return false;

    queryCaps();
    if (!(caps_.msaaMask & sampleCount_)) sampleCount_ = 1;

    const SkyAtmosphere def{};
    setLight(def.sunDirection, def.sunColor, def.skyLightIntensity);

    if (!createPipeline()) { AVER_ERROR("[RHI.Vulkan] the backend's fixed scene pipelines failed to build"); return false; }
    if (!createPostPipelines()) { AVER_ERROR("[RHI.Vulkan] the camera post chain's pipelines failed to build"); return false; }
    if (rtSupported_) rtSupported_ = initAccelerationStructures();
    if (caps_.meshShaderTier > 0) initMeshShaders();

    rhiFactory_ = new VulkanResourceFactory(this);
    if (!rhiFactory_->init()) { delete rhiFactory_; rhiFactory_ = nullptr; }
    else rhiContext_ = new VulkanRenderContext(this, rhiFactory_);

    // M6: one snapshot at init, same C-7 shape the D3D12 backend's own init-time line uses (and the
    // one gpuTiming()-adjacent host-side "[GPU] video memory: ..." line reads this same call every
    // frame thereafter) -- MB, not bytes, since a raw byte count on a multi-GB budget is not
    // something a log line should make a human parse.
    {
        const VideoMemoryInfo vmem = videoMemory();
        if (vmem.supported) {
            AVER_INFO("[RHI.Vulkan] video memory at init: local {} MB used of {} MB budget, non-local {} MB used of {} MB budget",
                      vmem.localUsageBytes / (1024 * 1024), vmem.localBudgetBytes / (1024 * 1024),
                      vmem.nonLocalUsageBytes / (1024 * 1024), vmem.nonLocalBudgetBytes / (1024 * 1024));
        } else {
            AVER_INFO("[RHI.Vulkan] video memory: not reported (VK_EXT_memory_budget absent)");
        }
    }

    AVER_INFO("[RHI.Vulkan] device ready on adapter '{}'", adapterName_);
    if (rhiFactory_) rhiFactory_->selfTest();
    return true;
}

VulkanDevice::~VulkanDevice() {
    // THE UI TOOLKIT FIRST, unconditionally -- as ~D3D12Device does. Without this the toolkit's own
    // Vulkan objects (descriptor pool, font atlas, per-frame vertex/index rings) are never destroyed
    // here: they die later, with the backend object Sandbox owns, which by then is calling into a
    // VkDevice that no longer exists --
    //     [Vulkan Loader] ERROR: vkDestroyBuffer: Invalid device
    // -- and the layer counts all of them leaked at vkDestroyDevice (40, vs 13 before a UI backend
    // existed). uiShutdown waits for the GPU itself, so this is safe as teardown's first act.
    uiShutdown();
    waitForGpu();
    delete rhiContext_;
    delete rhiFactory_;

    releasePostTargets();
    // postSets_ come OUT of postDescriptorPool_, so the pool destroy below takes them all; the
    // vector just must not keep naming them afterwards.
    postSets_.clear();
    if (postSampler_) api_.DestroySampler(device_, postSampler_, nullptr);
    postSampler_ = VK_NULL_HANDLE;
    for (VkDescriptorPool& p : meshGeomPool_) {
        if (p) api_.DestroyDescriptorPool(device_, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (meshGeomLayout_) api_.DestroyDescriptorSetLayout(device_, meshGeomLayout_, nullptr);
    meshGeomLayout_ = VK_NULL_HANDLE;
    if (postDescriptorPool_) api_.DestroyDescriptorPool(device_, postDescriptorPool_, nullptr);
    if (postSetLayout_) api_.DestroyDescriptorSetLayout(device_, postSetLayout_, nullptr);
    if (postPipelineLayout_) api_.DestroyPipelineLayout(device_, postPipelineLayout_, nullptr);
    for (VkPipeline* pso : {&bloomPrefilterPso_, &bloomDownPso_, &bloomUpPso_, &histogramPso_, &exposurePso_})
        if (*pso) api_.DestroyPipeline(device_, *pso, nullptr);
    for (int b = 0; b < 2; ++b) for (int a = 0; a < 2; ++a)
        if (compositePso_[b][a]) api_.DestroyPipeline(device_, compositePso_[b][a], nullptr);
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (postRing_[i].buffer) destroyBufferCommitted(*this, postRing_[i].buffer, postRing_[i].memory);
    }
    if (histBuf_) destroyBufferCommitted(*this, histBuf_, histMemory_);
    if (expBuf_) destroyBufferCommitted(*this, expBuf_, expMemory_);
    if (captureBuf_) destroyBufferCommitted(*this, captureBuf_, captureMemory_);

    for (GpuMesh& m : meshes_) {
        if (m.vb && m.vbBuffer == 0) destroyBufferCommitted(*this, m.vb, m.vbMemory);
        if (m.ib && m.ibOwned && m.ibBuffer == 0) destroyBufferCommitted(*this, m.ib, m.ibMemory);
    }
    for (GpuLineMesh& lm : lineMeshes_) if (lm.vb) destroyBufferCommitted(*this, lm.vb, lm.vbMemory);

    for (u32 i = 0; i < kFrameCount; ++i)
        if (frameCBs_[i]) destroyBufferCommitted(*this, frameCBs_[i], frameCBMemory_[i]);
    if (sceneDescriptorPool_) api_.DestroyDescriptorPool(device_, sceneDescriptorPool_, nullptr);
    if (sceneFrameSetLayout_) api_.DestroyDescriptorSetLayout(device_, sceneFrameSetLayout_, nullptr);
    // After every pipeline layout that named it, which is what device teardown order already gives.
    if (emptySetLayout_) { api_.DestroyDescriptorSetLayout(device_, emptySetLayout_, nullptr); emptySetLayout_ = VK_NULL_HANDLE; }
    if (scenePipelineLayout_) api_.DestroyPipelineLayout(device_, scenePipelineLayout_, nullptr);
    if (meshPipelineLayout_) api_.DestroyPipelineLayout(device_, meshPipelineLayout_, nullptr);
    for (VkPipeline* pso : {&scenePso_, &skyPso_, &wirePso_, &linePso_, &lineOverlayPso_, &meshPso_})
        if (*pso) api_.DestroyPipeline(device_, *pso, nullptr);

    if (msaaColorView_) api_.DestroyImageView(device_, msaaColorView_, nullptr);
    if (msaaColor_) destroyImageCommitted(*this, msaaColor_, msaaColorMemory_);
    if (depthView_) api_.DestroyImageView(device_, depthView_, nullptr);
    if (depthBuffer_) destroyImageCommitted(*this, depthBuffer_, depthMemory_);
    if (sceneResolvedView_) api_.DestroyImageView(device_, sceneResolvedView_, nullptr);
    if (sceneResolved_) destroyImageCommitted(*this, sceneResolved_, sceneResolvedMemory_);

    for (VkImageView v : swapchainViews_) if (v) api_.DestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : renderFinished_) if (s) api_.DestroySemaphore(device_, s, nullptr);
    if (swapchain_) api_.DestroySwapchainKHR(device_, swapchain_, nullptr);
    if (surface_) {
        // vkDestroySurfaceKHR is an INSTANCE-level function (VulkanApi does not carry a device-level
        // duplicate); DestroySurfaceKHR was already resolved in loadInstanceApi.
        api_.DestroySurfaceKHR(instance_, surface_, nullptr);
    }
    for (u32 i = 0; i < kFrameCount; ++i) if (imageAvailable_[i]) api_.DestroySemaphore(device_, imageAvailable_[i], nullptr);
    if (timeline_) api_.DestroySemaphore(device_, timeline_, nullptr);
    if (commandPool_) api_.DestroyCommandPool(device_, commandPool_, nullptr);

    if (device_) api_.DestroyDevice(device_, nullptr);
    if (debugMessenger_ && api_.DestroyDebugUtilsMessengerEXT) api_.DestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    if (instance_) api_.DestroyInstance(instance_, nullptr);
    unloadApi(api_);
}

IResourceFactory* VulkanDevice::resources() { return rhiFactory_; }
IRenderContext* VulkanDevice::renderContext() { return rhiContext_; }

// M6. Sums VkPhysicalDeviceMemoryBudgetPropertiesEXT's two per-heap arrays into the LOCAL/NON_LOCAL
// split VideoMemoryInfo declares (RHI.hpp), matching D3D12's own DXGI_MEMORY_SEGMENT_GROUP split in
// spirit: LOCAL is the heaps flagged VK_MEMORY_HEAP_DEVICE_LOCAL_BIT (VRAM on a discrete card, the
// whole pool on a UMA/integrated one), NON_LOCAL is every other heap. Polled live, not cached at
// init, since the whole point of "budget" is that it moves as other processes and the desktop
// compositor claim or release their own share.
//
// UNMEASURED: this backend has no editor UI session on this machine to run it through (house rule:
// no run, no build). Checked by eye against the vendored vulkan_core.h struct layout and against
// D3D12Device's own videoMemory() contract in RHI.hpp; not exercised on a live VkDevice.
VideoMemoryInfo VulkanDevice::videoMemory() const {
    VideoMemoryInfo out;
    if (!memoryBudgetExt_ || !api_.GetPhysicalDeviceMemoryProperties2 || !physicalDevice_) return out;

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props2.pNext = &budget;
    api_.GetPhysicalDeviceMemoryProperties2(physicalDevice_, &props2);

    // Heap COUNT/FLAGS come from memoryProps_ (queried once at init via the plain, non-"2" call;
    // see its own field comment) rather than props2.memoryProperties above -- a physical device's
    // heap layout is architecturally fixed for its life either way, so this just avoids depending on
    // the "2" struct's own copy of the same fixed data being filled the way this call expects.
    for (u32 i = 0; i < memoryProps_.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
        const bool local = (memoryProps_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        (local ? out.localBudgetBytes : out.nonLocalBudgetBytes) += budget.heapBudget[i];
        (local ? out.localUsageBytes  : out.nonLocalUsageBytes)  += budget.heapUsage[i];
    }
    out.supported = true;
    return out;
}

// Lazily (re)adopts depthBuffer_ into the factory's texture table whenever createDepthBuffer() has
// run since the last call -- depthTexDirty_ (not a size comparison) is the trigger, since this
// method has no cheap way to tell "the SAME resource" from "one that happens to be the same size".
// Mirrors D3D12Device::sceneDepthTexture (D3D12Device.cpp:1998-2006); sceneWidth_/sceneHeight_ is the scene-space extent
// createDepthBuffer() actually allocated (not width_/height_, the present-space swapchain size).
TextureHandle VulkanDevice::sceneDepthTexture() {
    if (!depthBuffer_ || !rhiFactory_) return 0;
    if (depthTexDirty_) {
        depthTexHandle_ = rhiFactory_->adoptExternalDepthTexture(
            depthBuffer_, depthMemory_, sceneWidth_, sceneHeight_, depthTexHandle_);
        depthTexDirty_ = false;
    }
    return depthTexHandle_;
}

// ================================================================================================
// The in-window UI seam. Every one of these is a pass-through to whatever vkb::IUiBackend Sandbox
// installed, and a no-op returning IDevice's own default when none was -- which is every game
// build. NOTHING ABOUT DEAR IMGUI APPEARS IN THIS MODULE; see UiBackend.hpp for why the split
// exists and why its handles cross as opaque u64s.
// ================================================================================================
bool VulkanDevice::uiInit(void* windowHandle) {
    if (!uiBackend_ || uiUp_) return uiUp_;
    if (!device_ || !instance_ || !queue_) {
        AVER_WARN("[RHI.Vulkan] uiInit before the device is up; no UI backend can be started yet");
        return false;
    }
    vkb::UiBackendInitDesc d{};
    d.instance       = reinterpret_cast<u64>(instance_);
    d.physicalDevice = reinterpret_cast<u64>(physicalDevice_);
    d.device         = reinterpret_cast<u64>(device_);
    d.queue          = reinterpret_cast<u64>(queue_);
    d.queueFamily    = graphicsQueueFamily();
    // The factory's pool, deliberately: a toolkit allocating from its own second pool is a second
    // budget nothing reconciles, and this one is sized at 4096 sets against a handful in use.
    d.descriptorPool = rhiFactory_ ? reinterpret_cast<u64>(rhiFactory_->descriptorPool_) : 0;
    d.frameCount     = kFrameCount;
    d.imageCount     = static_cast<u32>(swapchainImages_.size());
    // The SWAPCHAIN format, not the HDR scene one: the UI is composed straight onto the backbuffer
    // in the overlay pass, after the post chain has already tonemapped into it.
    d.colorFormat    = static_cast<u32>(swapchainFormat_);
    d.sampleCount    = 1;          // the overlay pass is never multisampled
    d.dynamicRendering = true;     // this backend has no VkRenderPass to give -- see the desc

    uiUp_ = uiBackend_->init(windowHandle, d);
    if (!uiUp_) AVER_WARN("[RHI.Vulkan] the installed UI backend refused to initialise");
    return uiUp_;
}

void VulkanDevice::uiNewFrame() { if (uiBackend_ && uiUp_) uiBackend_->newFrame(); }

void VulkanDevice::uiShutdown() {
    if (!uiBackend_ || !uiUp_) return;
    // Everything the toolkit built is referenced by command buffers that may still be in flight.
    waitForGpu();
    // The toolkit owns every descriptor it made for us; its shutdown takes them all, so this map
    // must not outlive it holding ids that no longer mean anything.
    uiTexIds_.clear();
    uiBackend_->shutdown();
    uiUp_ = false;
}

bool VulkanDevice::uiWantsMouse() const    { return uiBackend_ && uiUp_ && uiBackend_->wantsMouse(); }
bool VulkanDevice::uiWantsKeyboard() const { return uiBackend_ && uiUp_ && uiBackend_->wantsKeyboard(); }

u64 VulkanDevice::uiTextureId(TextureHandle t) {
    if (!uiBackend_ || !uiUp_ || !t || !rhiFactory_) return 0;
    if (auto it = uiTexIds_.find(t); it != uiTexIds_.end()) return it->second;

    const RhiTexture* tex = rhiFactory_->texture(t);
    if (!tex || tex->srvView == VK_NULL_HANDLE) return 0;
    // A plain linear/clamp sampler, from the SAME cache every pipeline's immutable samplers come
    // from, so this adds no sampler the device did not already own.
    const VkSampler samp = rhiFactory_->getOrCreateSampler(SamplerDesc{});
    if (samp == VK_NULL_HANDLE) return 0;

    const u64 id = uiBackend_->textureId(reinterpret_cast<u64>(tex->srvView), reinterpret_cast<u64>(samp));
    if (id) uiTexIds_[t] = id;
    return id;
}

void VulkanDevice::releaseUiTextureId(TextureHandle t) {
    auto it = uiTexIds_.find(t);
    if (it == uiTexIds_.end()) return;
    if (uiBackend_ && uiUp_) uiBackend_->releaseTextureId(it->second);
    uiTexIds_.erase(it);
}

// Already inside aver::rhi::vkb -- no nested namespace, which would make this vkb::vkb::.
bool installUiBackend(IDevice* device, IUiBackend* backend) {
    if (!device || device->backend() != Backend::Vulkan) return false;
    static_cast<VulkanDevice*>(device)->uiBackend_ = backend;
    return true;
}

void VulkanDevice::addRenderFeature(IRenderFeature* f) {
    if (!f) return;
    for (IRenderFeature* e : features_) if (e == f) return;
    features_.push_back(f);
    AVER_INFO("[RHI.Vulkan] render feature registered: {}", f->name());
    if (width_ > 0 && height_ > 0)
        f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat(), width_, height_);
}
void VulkanDevice::removeRenderFeature(IRenderFeature* f) {
    for (usize i = 0; i < features_.size(); ++i)
        if (features_[i] == f) { features_.erase(features_.begin() + static_cast<isize>(i)); return; }
}

// Fills caps_ from the hardware, then applies the --force-caps clamp. Vulkan-side twin of
// D3D12Device::queryCaps -- see DeviceCaps's own field comments for what each honestly maps to.
void VulkanDevice::queryCaps() {
    caps_ = {};
    caps_.computeShaders = true;   // core 1.0 mandates a compute-capable queue existing

    caps_.msaaMask = 1;
    caps_.maxMsaaSamples = 1;
    VkImageFormatProperties ifp{};
    for (u32 s : {2u, 4u, 8u}) {
        if (api_.GetPhysicalDeviceImageFormatProperties(physicalDevice_, kVkSceneColorFormat, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &ifp) == VK_SUCCESS &&
            (ifp.sampleCounts & toVkSampleCount(s))) {
            caps_.msaaMask |= s;
            caps_.maxMsaaSamples = s;
        }
    }

    VkPhysicalDeviceFeatures f10{};
    api_.GetPhysicalDeviceFeatures(physicalDevice_, &f10);
    caps_.typedUavLoads = f10.shaderStorageImageReadWithoutFormat != VK_FALSE;

    caps_.conservativeRaster = deviceHasExtension(api_, physicalDevice_, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);

    vulkanShaderCompiler().init();
    caps_.dxcAvailable = vulkanShaderCompiler().usingDxc();

    const bool meshExt = deviceHasExtension(api_, physicalDevice_, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (meshExt) { f2.pNext = &meshFeat; api_.GetPhysicalDeviceFeatures2(physicalDevice_, &f2); }
    caps_.shaderModel = caps_.dxcAvailable ? 60 : 51;
    caps_.meshShaderTier = (meshExt && meshFeat.taskShader && meshFeat.meshShader && caps_.dxcAvailable) ? 1u : 0u;
    if (caps_.meshShaderTier > 0) caps_.shaderModel = 65;   // this backend's mesh path needs ms_6_5

    // No numbered DXR tier in Vulkan; report 11 (DXR-1.1-shaped: inline RayQuery only, no hit-group
    // pipeline) when the inline-query extension set is present, 0 otherwise -- a judgment call, not a
    // hardware-reported number (see kOptionalDeviceExtensions on why VK_KHR_ray_tracing_pipeline is
    // deliberately out of scope).
    //
    // rtSupported_ was set in init() from extension + feature-bit presence alone, before
    // shaderModel/dxcAvailable existed; RayQuery needs SM 6.5 like the mesh path, so that gate is
    // applied HERE. initAccelerationStructures() (called right after) narrows it once more,
    // confirming the acceleration-structure pointers actually resolved; not expected to fail if the
    // extension was genuinely present, but see this backend's own honestState for the residual,
    // unverified case.
    rtSupported_ = rtSupported_ && caps_.shaderModel >= 65 && caps_.dxcAvailable;
    caps_.rayTracingTier = rtSupported_ ? 11u : 0u;

    // D3D12_RESOURCE_BINDING_TIER has no Vulkan analogue -- report unknown honestly rather than
    // inventing a mapping from descriptor-indexing bits this backend doesn't otherwise rely on.
    caps_.resourceBindingTier = 0;

    // NOT DERIVED FROM resourceBindingTier (hardcoded 0 above): Vulkan answers this with
    // descriptor-indexing bits decided at device creation (bindlessCapable_) instead. Ray tracing is
    // still required, so --force-caps no-rt disables this too -- hence reading the CLAMPED tier below.

    const DeviceCaps hw = caps_;
    clampCaps(caps_);

    // caps_.rayTracingTier >= 11 && bindlessCapable_ is what the HARDWARE AND DRIVER can genuinely
    // do here: descriptor indexing really was requested and came back enabled above (:734-757), and
    // the inline-query ray path really works. That is deliberately NOT what this bit reports, though.
    // It hardcodes false regardless of those two conditions, because createBindlessTextureTable()
    // near the bottom of this file is a stub that always returns 0 -- this backend has no real
    // implementation to hand out yet. Reporting the hardware bit here would tell
    // VoxiRenderer::ensureTextureTable and PathTracer::ensureTexturing, both gated on exactly this
    // flag and nothing else, to go ahead and call a function that can only fail them, with nothing in
    // the log connecting a "yes" capability to the flat-albedo shading it actually produces. Consumers
    // across both backends treat this flag as "this device can hand me a working table", not merely
    // "the hardware bits are present" -- see RHIResources.hpp's own comment on
    // createBindlessTextureTable -- so honesty toward that contract means reading false here until
    // the stub below is replaced with a real implementation, at which point this should go back to
    // the hardware expression above.
    const bool hwBindlessCapable = caps_.rayTracingTier >= 11 && bindlessCapable_;
    caps_.rtBindlessTextures = false;
    AVER_INFO("[RHI.Vulkan] caps: MSAA {}x, RT tier {}, SM {}, mesh-shader tier {}, DXC {}, cons-raster {}, binding tier {}",
              caps_.maxMsaaSamples, caps_.rayTracingTier, caps_.shaderModel,
              caps_.meshShaderTier, caps_.dxcAvailable, caps_.conservativeRaster, caps_.resourceBindingTier);
    // Mirrors D3D12Device.cpp's own "ray-traced bindless textures: yes/no" line so the two backends'
    // logs read the same way for the same question, and adds the one thing D3D12 doesn't need: a
    // WARN, said once at device creation, for the specific case that would otherwise go unexplained
    // -- hardware that could do this sitting behind a flag that says no because the backend can't
    // deliver yet. Without this line, a reader sees "no" above and has to already know about the
    // stub to understand why capable hardware isn't being used.
    AVER_INFO("[RHI.Vulkan] ray-traced bindless textures: {}", caps_.rtBindlessTextures ? "yes" : "no");
    if (hwBindlessCapable)
        AVER_WARN("[RHI.Vulkan] descriptor indexing and inline ray query are both present and enabled "
                  "on this device -- the hardware could support ray-traced bindless texturing -- but "
                  "createBindlessTextureTable() has no implementation on this backend yet, so ray "
                  "hits keep shading from flat albedo until it does (said once)");
    if (capsOverride().active)
        AVER_WARN("[RHI.Vulkan] caps CLAMPED by --force-caps; the hardware reports MSAA {}x, RT tier {}, SM {}, "
                  "mesh-shader tier {}, DXC {}, cons-raster {}, binding tier {}",
                  hw.maxMsaaSamples, hw.rayTracingTier, hw.shaderModel, hw.meshShaderTier,
                  hw.dxcAvailable, hw.conservativeRaster, hw.resourceBindingTier);
}

bool VulkanDevice::initAccelerationStructures() {
    if (caps_.shaderModel < 65 || !caps_.dxcAvailable) return false;
    if (!api_.GetAccelerationStructureBuildSizesKHR || !api_.CreateAccelerationStructureKHR ||
        !api_.CmdBuildAccelerationStructuresKHR || !api_.GetAccelerationStructureDeviceAddressKHR)
        return false;
    AVER_INFO("[RHI.Vulkan] VK_KHR_acceleration_structure + VK_KHR_ray_query available (inline queries)");
    return true;
}

// ================================================================================================
// 5. The backend's own fixed scene/sky/wire/line pipelines.
// ================================================================================================
bool VulkanDevice::createPipeline() {
    // setSampleCount() calls this repeatedly to rebuild every PSO's baked-in rasterizationSamples --
    // unlike D3D12's ComPtr, a VkPipeline overwritten by vkCreateGraphicsPipelines without an
    // explicit vkDestroyPipeline first is simply leaked. Set/pipeline-LAYOUT objects are deliberately
    // NOT re-destroyed here (they don't depend on sampleCount_, only the PSOs built from them do),
    // matching the `if (!X)` guards on each block below.
    for (VkPipeline* pso : {&scenePso_, &skyPso_, &wirePso_, &linePso_, &lineOverlayPso_}) {
        if (*pso) { api_.DestroyPipeline(device_, *pso, nullptr); *pso = VK_NULL_HANDLE; }
    }

    // ---- set 2 (kVkSetConstants), binding 0: b0 PerFrame, a dynamic UBO on EVERY pipeline here ----
    if (!sceneFrameSetLayout_) {
        VkDescriptorSetLayoutBinding b0{};
        b0.binding = 0;
        b0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b0.descriptorCount = 1;
        b0.stageFlags = VK_SHADER_STAGE_ALL;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1; ci.pBindings = &b0;
        if (!vkOk(api_.CreateDescriptorSetLayout(device_, &ci, nullptr, &sceneFrameSetLayout_), "scene frame set layout")) return false;
    }
    if (!sceneDescriptorPool_) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFrameCount};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kFrameCount;
        pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!vkOk(api_.CreateDescriptorPool(device_, &pci, nullptr, &sceneDescriptorPool_), "scene descriptor pool")) return false;
        VkDescriptorSetLayout layouts[kFrameCount];
        for (u32 i = 0; i < kFrameCount; ++i) layouts[i] = sceneFrameSetLayout_;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = sceneDescriptorPool_;
        dai.descriptorSetCount = kFrameCount;
        dai.pSetLayouts = layouts;
        if (!vkOk(api_.AllocateDescriptorSets(device_, &dai, sceneFrameSet_), "scene frame sets")) return false;
    }
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (!frameCBs_[i]) {
            static_assert(sizeof(PerFrameCB) % 16 == 0, "a constant buffer's rows are float4s");
            if (!createBufferCommitted(*this, sizeof(PerFrameCB), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    frameCBs_[i], frameCBMemory_[i], nullptr, "PerFrame CB"))
                return false;
            api_.MapMemory(device_, frameCBMemory_[i], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&frameCBPtr_[i]));
        }
        VkDescriptorBufferInfo bi{frameCBs_[i], 0, sizeof(PerFrameCB)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sceneFrameSet_[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi;
        api_.UpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    // ---- pipeline layout: push constants (b1, patched into the assembled source) + set2 (b0) ----
    if (!scenePipelineLayout_) {
        // The SAME shared layout in both unused slots -- a VkDescriptorSetLayout may appear in any
        // number of pipeline layouts and in any number of their slots.
        if (!emptySetLayout_) emptySetLayout_ = makeEmptySetLayout(api_, device_);
        VkDescriptorSetLayout sets[kVkSetConstants + 1] = {emptySetLayout_, emptySetLayout_, sceneFrameSetLayout_};
        VkPushConstantRange pc{VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectOffset, PushConstantLayout::kObjectBytes};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = kVkSetConstants + 1; lci.pSetLayouts = sets;
        lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
        // NOT destroyed here -- see emptySetLayout_'s own comment. It outlives every pipeline layout
        // that names it and is torn down with the device.
        if (!vkOk(api_.CreatePipelineLayout(device_, &lci, nullptr, &scenePipelineLayout_), "scene pipeline layout"))
            return false;
    }

    std::string src = sceneShaderSource();
    if (!patchPerFrameSet(src)) {
        AVER_ERROR("[RHI.Vulkan] the shared prelude no longer contains the exact PerFrame cbuffer "
                   "text this backend re-binds to set {}; every fixed pipeline would name a "
                   "descriptor its layout does not have", kVkSetConstants);
        return false;
    }
    if (!patchPushConstants(src, /*mesh=*/false)) return false;

    std::vector<u32> vsSpv, psSpv, skyVsSpv, skyPsSpv, wireVsSpv, lineVsSpv, linePsSpv;
    if (!vulkanShaderCompiler().compile(src.c_str(), "VSMain", ShaderStage::Vertex, 60, nullptr, vsSpv)) return false;
    if (!vulkanShaderCompiler().compile(src.c_str(), "PSMainPlain", ShaderStage::Pixel, 60, nullptr, psSpv)) return false;
    if (!vulkanShaderCompiler().compile(src.c_str(), "VSky", ShaderStage::Vertex, 60, nullptr, skyVsSpv)) return false;
    if (!vulkanShaderCompiler().compile(src.c_str(), "PSky", ShaderStage::Pixel, 60, nullptr, skyPsSpv)) return false;
    if (!vulkanShaderCompiler().compile(src.c_str(), "VSLine", ShaderStage::Vertex, 60, nullptr, lineVsSpv)) return false;
    if (!vulkanShaderCompiler().compile(src.c_str(), "PSLine", ShaderStage::Pixel, 60, nullptr, linePsSpv)) return false;

    auto makeModule = [&](const std::vector<u32>& spv, VkShaderModule& out) {
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = spv.size() * sizeof(u32);
        mci.pCode = spv.data();
        return vkOk(api_.CreateShaderModule(device_, &mci, nullptr, &out), "shader module");
    };
    VkShaderModule vsMod{}, psMod{}, skyVsMod{}, skyPsMod{}, lineVsMod{}, linePsMod{};
    if (!makeModule(vsSpv, vsMod) || !makeModule(psSpv, psMod) || !makeModule(skyVsSpv, skyVsMod) ||
        !makeModule(skyPsSpv, skyPsMod) || !makeModule(lineVsSpv, lineVsMod) || !makeModule(linePsSpv, linePsMod))
        return false;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    VkFormat colorFmt = kVkSceneColorFormat;
    rendering.pColorAttachmentFormats = &colorFmt;
    rendering.depthAttachmentFormat = kVkDepthFormat;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsMod, "VSMain", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psMod, "PSMainPlain", nullptr},
    };

    VkVertexInputBindingDescription meshBinding{};
    VkVertexInputAttributeDescription meshAttribs[3];
    meshVertexInputState(meshBinding, meshAttribs);
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &meshBinding;
    vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = meshAttribs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;   // both dynamic

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = toVkSampleCount(sampleCount_);

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &blendAtt;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rendering;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dyn;
    gp.layout = scenePipelineLayout_;
    gp.renderPass = VK_NULL_HANDLE;   // dynamic rendering: no VkRenderPass/VkFramebuffer at all
    if (!vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &scenePso_), "scene pso")) return false;

    // ---- sky: no vertex input (VSky is SV_VertexID-driven), depth EQUAL / no write ----
    stages[0].module = skyVsMod; stages[0].pName = "VSky";
    stages[1].module = skyPsMod; stages[1].pName = "PSky";
    VkPipelineVertexInputStateCreateInfo emptyVin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    gp.pVertexInputState = &emptyVin;
    // EQUAL, not GREATER_EQUAL: VSky pins clip-space depth at exactly 1.0 (the far plane, this
    // scene's clear value), so EQUAL is the real "nothing closer was written here" test --
    // GREATER_EQUAL would be trivially true everywhere and paint the sky's cost over every opaque pixel.
    ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
    ds.depthWriteEnable = VK_FALSE;
    if (!vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &skyPso_), "sky pso")) return false;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.depthWriteEnable = VK_TRUE;

    // ---- wireframe: same as scene, FILL_MODE_LINE ----
    stages[0].module = vsMod; stages[0].pName = "VSMain";
    stages[1].module = psMod; stages[1].pName = "PSMainPlain";
    gp.pVertexInputState = &vin;
    rs.polygonMode = VK_POLYGON_MODE_LINE;
    if (!vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &wirePso_), "wire pso")) return false;
    rs.polygonMode = VK_POLYGON_MODE_FILL;

    // ---- lines: LineVertex input, line-list topology ----
    stages[0].module = lineVsMod; stages[0].pName = "VSLine";
    stages[1].module = linePsMod; stages[1].pName = "PSLine";
    VkVertexInputBindingDescription lineBinding{};
    VkVertexInputAttributeDescription lineAttribs[2];
    lineVertexInputState(lineBinding, lineAttribs);
    VkPipelineVertexInputStateCreateInfo lvin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    lvin.vertexBindingDescriptionCount = 1; lvin.pVertexBindingDescriptions = &lineBinding;
    lvin.vertexAttributeDescriptionCount = 2; lvin.pVertexAttributeDescriptions = lineAttribs;
    gp.pVertexInputState = &lvin;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    ds.depthWriteEnable = VK_FALSE;
    if (!vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &linePso_), "line pso")) return false;
    ds.depthTestEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    if (!vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &lineOverlayPso_), "line overlay pso")) return false;

    for (VkShaderModule m : {vsMod, psMod, skyVsMod, skyPsMod, lineVsMod, linePsMod}) api_.DestroyShaderModule(device_, m, nullptr);
    return true;
}

// ================================================================================================
// 6. Mesh shaders. See patchPushConstants' own comment for why b1+b5 merge into one push-constant
//    block, and dispatchMesh's for why gVerts/gIndices cannot follow the same road.
// ================================================================================================

bool VulkanDevice::initMeshShaders() {
    msSupported_ = false;
    // Same reason as createPipeline()'s identical guard (see above): an overwritten VkPipeline with
    // no vkDestroyPipeline first is a leak, not a rebuild.
    if (meshPso_) { api_.DestroyPipeline(device_, meshPso_, nullptr); meshPso_ = VK_NULL_HANDLE; }
    if (caps_.meshShaderTier == 0 || caps_.shaderModel < 65 || !caps_.dxcAvailable || !api_.CmdDrawMeshTasksEXT) return false;
    // The merged push-constant range is kObjectBytes+16 = 144 bytes, over the 128-byte floor every
    // Vulkan implementation is REQUIRED to support (VkPhysicalDeviceLimits::maxPushConstantsSize's
    // guaranteed minimum) -- unlike the plain scene pipeline's range, which fits inside it. Checked
    // explicitly so hardware at the floor gets the same "falls back to the input assembler" outcome.
    if (maxPushConstantsSize_ < PushConstantLayout::kObjectBytes + 16) {
        AVER_INFO("[RHI.Vulkan] mesh-shader push-constant range needs {} bytes; this device's "
                  "maxPushConstantsSize is only {}", PushConstantLayout::kObjectBytes + 16, maxPushConstantsSize_);
        return false;
    }

    // set 0 (kVkSetTable0), bindings kMeshSrvBase/+1: gVerts/gIndices, SSBOs, FRESH per draw -- see
    // dispatchMesh() for why (same StructuredBuffer-always-descriptor-bound reason as this file's
    // header comment).
    if (!meshGeomLayout_) {
        VkDescriptorSetLayoutBinding binds[2] = {};
        binds[0].binding = kMeshSrvBase; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
        binds[1].binding = kMeshSrvBase + 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2; ci.pBindings = binds;
        if (!vkOk(api_.CreateDescriptorSetLayout(device_, &ci, nullptr, &meshGeomLayout_), "mesh geometry set layout")) return false;
    }
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (meshGeomPool_[i]) continue;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = 128; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!vkOk(api_.CreateDescriptorPool(device_, &pci, nullptr, &meshGeomPool_[i]), "mesh geometry pool")) return false;
    }

    if (!meshPipelineLayout_) {
        if (!emptySetLayout_) emptySetLayout_ = makeEmptySetLayout(api_, device_);
        VkDescriptorSetLayout sets[kVkSetConstants + 1] = {meshGeomLayout_, emptySetLayout_, sceneFrameSetLayout_};
        // Merged push-constant range: object bytes [0,128) then the mesh count block right after --
        // see patchPushConstants for why they are ONE HLSL block, not two.
        VkPushConstantRange pc{VK_SHADER_STAGE_ALL, 0, PushConstantLayout::kObjectBytes + 16};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = kVkSetConstants + 1; lci.pSetLayouts = sets;
        lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
        if (!vkOk(api_.CreatePipelineLayout(device_, &lci, nullptr, &meshPipelineLayout_), "mesh pipeline layout"))
            return false;
    }

    std::string src = sceneShaderSource();
    if (!patchPerFrameSet(src)) {
        AVER_ERROR("[RHI.Vulkan] the shared prelude no longer contains the PerFrame cbuffer text "
                   "the mesh path re-binds to set {}", kVkSetConstants);
        return false;
    }
    if (!patchPushConstants(src, /*mesh=*/true)) return false;
    const std::string defines = "AVER_MS=1;AVER_MS_VTX_REG=" + std::to_string(kMeshSrvBase) +
                                ";AVER_MS_IDX_REG=" + std::to_string(kMeshSrvBase + 1);
    std::vector<u32> msSpv, psSpv;
    if (!vulkanShaderCompiler().compile(src.c_str(), "MSMain", ShaderStage::Mesh, 65, defines.c_str(), msSpv)) {
        AVER_WARN("[RHI.Vulkan] mesh shaders failed to compile; the input-assembler path stays in use");
        return false;
    }
    if (!vulkanShaderCompiler().compile(src.c_str(), "PSMainPlain", ShaderStage::Pixel, 65, defines.c_str(), psSpv)) {
        AVER_WARN("[RHI.Vulkan] mesh shaders failed to compile; the input-assembler path stays in use");
        return false;
    }
    VkShaderModule msMod{}, psMod{};
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = msSpv.size() * sizeof(u32); mci.pCode = msSpv.data();
    if (!vkOk(api_.CreateShaderModule(device_, &mci, nullptr, &msMod), "ms module")) return false;
    mci.codeSize = psSpv.size() * sizeof(u32); mci.pCode = psSpv.data();
    if (!vkOk(api_.CreateShaderModule(device_, &mci, nullptr, &psMod), "ms-path ps module")) { api_.DestroyShaderModule(device_, msMod, nullptr); return false; }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MESH_BIT_EXT, msMod, "MSMain", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psMod, "PSMainPlain", nullptr},
    };
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    VkFormat colorFmt = kVkSceneColorFormat;
    rendering.pColorAttachmentFormats = &colorFmt;
    rendering.depthAttachmentFormat = kVkDepthFormat;
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};   // none: MS emits its own
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = toVkSampleCount(sampleCount_);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &blendAtt;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rendering;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dyn;
    gp.layout = meshPipelineLayout_;
    const bool built = vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &meshPso_), "mesh pso");
    api_.DestroyShaderModule(device_, msMod, nullptr);
    api_.DestroyShaderModule(device_, psMod, nullptr);
    if (!built) return false;

    msSupported_ = true;
    AVER_INFO("[RHI.Vulkan] mesh shader path ready (ms_6_5)");
    return true;
}

void VulkanDevice::setMeshShaders(bool enabled) {
    const bool want = enabled && msSupported_;
    if (want != msActive_) AVER_INFO("[RHI.Vulkan] geometry path: {}", want ? "mesh shaders" : "input assembler");
    if (enabled && !msSupported_ && !msRefusalLogged_) {
        msRefusalLogged_ = true;
        const char* why = caps_.meshShaderTier == 0 ? "mesh-shader tier 0"
                        : caps_.shaderModel < 65    ? "shader model below 6.5"
                        : !caps_.dxcAvailable       ? "no DXC (SPIR-V) compiler"
                                                    : "the mesh-shader path failed to initialise";
        AVER_INFO("[RHI.Vulkan] mesh-shader geometry path requested but unavailable ({}); staying on the input assembler", why);
    }
    msActive_ = want;
}

// Mesh-shader draw. Allocates a FRESH descriptor set for gVerts/gIndices from THIS frame-in-flight
// slot's pool (reset once per beginFrame, after that slot's fence wait proves the GPU is done with
// what the pool held two frames ago -- the same double-buffering discipline every other
// kFrameCount-sized array in this class already relies on) rather than rewriting one set in place: a descriptor set's
// binding is not a snapshot once recorded -- every vkCmdBindDescriptorSets referencing the SAME set
// sees whatever it was LAST written to by execution time, so reuse across draws in one frame would
// make earlier draws silently read the LAST mesh's geometry. A fresh set per draw costs an
// allocation, not correctness.
void VulkanDevice::dispatchMesh(const GpuMesh& m) {
    const u32 tris = m.indexCount / 3;
    if (!tris || !m.vb || !m.ib) return;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = meshGeomPool_[frameIndex_];
    dai.descriptorSetCount = 1; dai.pSetLayouts = &meshGeomLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (api_.AllocateDescriptorSets(device_, &dai, &set) != VK_SUCCESS) {
        AVER_ERROR("[RHI.Vulkan] mesh-geometry descriptor pool exhausted for this frame; dropping a mesh-shader draw");
        return;
    }
    VkDescriptorBufferInfo vbInfo{m.vb, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo ibInfo{m.ib, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[0].dstSet = set; writes[0].dstBinding = kMeshSrvBase;
    writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &vbInfo;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[1].dstSet = set; writes[1].dstBinding = kMeshSrvBase + 1;
    writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &ibInfo;
    api_.UpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    api_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipelineLayout_, 0, 1, &set, 0, nullptr);
    const u32 tc[4] = {tris, 0, 0, 0};
    api_.CmdPushConstants(cmd, meshPipelineLayout_, VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectBytes, sizeof(tc), tc);
    api_.CmdDrawMeshTasksEXT(cmd, (tris + kMeshShaderTrisPerGroup - 1) / kMeshShaderTrisPerGroup, 1, 1);
}

// ================================================================================================
// 7. Swapchain, its views, the depth/scene-colour targets, and resize.
// ================================================================================================
bool VulkanDevice::createSwapchainResources(const SwapchainDesc& d) {
    if (!d.windowHandle) { AVER_WARN("[RHI.Vulkan] createSwapchain without a window (headless)"); return false; }
    width_ = d.width; height_ = d.height;

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = static_cast<HWND>(d.windowHandle);
    if (!vkOk(api_.CreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_), "CreateWin32SurfaceKHR")) return false;

    VkBool32 canPresent = VK_FALSE;
    api_.GetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, graphicsQueueFamily_, surface_, &canPresent);
    if (!canPresent) {
        AVER_ERROR("[RHI.Vulkan] the selected queue family cannot present to this window's surface");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps{};
    api_.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    u32 fmtCount = 0;
    api_.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    api_.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, fmts.data());
    // A plain UNORM surface, not sRGB: PSComposite already applies toGamma() (rhi::postShaderSource()),
    // matching D3D12's non-sRGB backbuffer -- sRGB would gamma-encode a SECOND time on top.
    swapchainFormat_ = VK_FORMAT_UNDEFINED;
    // RGBA FIRST, AND THE ORDER IS LOAD-BEARING. B8G8R8A8_UNORM used to be preferred, but nothing in
    // rhi::Format can name it (fromVkFormat has no case, returns Unknown) -- ensureViewportTexture
    // asked, got Unknown, and createTexture refused it 80 times a run: why the editor's 3D viewport
    // was black on Vulkan while the UI around it drew fine. R8G8B8A8_UNORM also matches D3D12 (above).
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        for (const auto& f : fmts) if (f.format == want) { swapchainFormat_ = f.format; swapchainColorSpace_ = f.colorSpace; break; }
        if (swapchainFormat_ != VK_FORMAT_UNDEFINED) break;
    }
    if (swapchainFormat_ == VK_FORMAT_UNDEFINED && !fmts.empty()) {
        swapchainFormat_ = fmts[0].format;
        swapchainColorSpace_ = fmts[0].colorSpace;
        AVER_WARN("[RHI.Vulkan] no UNORM surface format available; using {} (may double-gamma-correct)",
                  static_cast<int>(swapchainFormat_));
    }

    u32 modeCount = 0;
    api_.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    api_.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());
    tearingSupported_ = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();
    AVER_INFO("[RHI.Vulkan] tearing (vsync-off) {}", tearingSupported_ ? "supported" : "unavailable");
    presentMode_ = vsync_ ? VK_PRESENT_MODE_FIFO_KHR
                 : tearingSupported_ ? VK_PRESENT_MODE_IMMEDIATE_KHR
                 : std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ? VK_PRESENT_MODE_MAILBOX_KHR
                 : VK_PRESENT_MODE_FIFO_KHR;

    u32 minImages = caps.minImageCount > kFrameCount ? caps.minImageCount : kFrameCount;
    if (caps.maxImageCount != 0 && minImages > caps.maxImageCount) minImages = caps.maxImageCount;
    swapchainExtent_ = (caps.currentExtent.width != 0xFFFFFFFFu) ? caps.currentExtent : VkExtent2D{width_, height_};
    width_ = swapchainExtent_.width; height_ = swapchainExtent_.height;
    // width_/height_ just took their final present-space value for this generation -- derive the
    // scene size now (same point D3D12's createSwapchainResources does) so createDepthBuffer()/
    // createMsaaColor() below see a live size, not the {0,0} construction default.
    computeSceneSize();

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = minImages;
    sc.imageFormat = swapchainFormat_;
    sc.imageColorSpace = swapchainColorSpace_;
    sc.imageExtent = swapchainExtent_;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // TRANSFER_SRC: capture
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = presentMode_;
    sc.clipped = VK_TRUE;
    if (!vkOk(api_.CreateSwapchainKHR(device_, &sc, nullptr, &swapchain_), "CreateSwapchainKHR")) return false;

    createRenderTargetViews();
    for (u32 i = 0; i < static_cast<u32>(swapchainImages_.size()); ++i) {
        VkSemaphore s = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        api_.CreateSemaphore(device_, &semInfo, nullptr, &s);
        renderFinished_.push_back(s);
    }

    if (!createDepthBuffer()) return false;
    if (!createMsaaColor()) return false;

    const VkDeviceSize captureBytes = VkDeviceSize(width_) * height_ * 4;
    if (!createBufferCommitted(*this, captureBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            captureBuf_, captureMemory_, nullptr, "capture readback"))
        AVER_WARN("[RHI.Vulkan] capture readback buffer could not be created; requestCapture will silently fail");

    hasSwapchain_ = true;
    AVER_INFO("[RHI.Vulkan] swapchain {}x{} + depth (D32) ({} images, present mode {})", width_, height_,
              swapchainImages_.size(), static_cast<int>(presentMode_));
    return true;
}

void VulkanDevice::createRenderTargetViews() {
    u32 count = 0;
    api_.GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchainImages_.resize(count);
    api_.GetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data());
    swapchainViews_.resize(count);
    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swapchainImages_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapchainFormat_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api_.CreateImageView(device_, &vi, nullptr, &swapchainViews_[i]);
    }
}

bool VulkanDevice::createDepthBuffer() {
    // Sized off the SCENE extent, not the present one (see sceneWidth_/sceneHeight_) -- equal to
    // width_/height_ at the default renderScale_==1.0, so a build that never calls setRenderScale
    // allocates the identical resource this always did.
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kVkDepthFormat;
    ci.extent = {sceneWidth_, sceneHeight_, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = toVkSampleCount(sampleCount_);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // SAMPLED, not just DEPTH_STENCIL_ATTACHMENT: IDevice::sceneDepthTexture() promises a "sampleable
    // handle", and modules/occlusion's HZB seed pass binds it as an SRV in a compute shader. Without
    // VK_IMAGE_USAGE_SAMPLED_BIT, the view adoptExternalDepthTexture builds over this same image
    // would violate the image's declared usage flags -- validation error at best, UB at worst. This
    // is additive: DEPTH_STENCIL_ATTACHMENT_BIT is unchanged, so depthView_'s per-frame use is as before.
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createImageCommitted(*this, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthBuffer_, depthMemory_, "scene depth")) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = depthBuffer_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kVkDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    const bool viewOk = vkOk(api_.CreateImageView(device_, &vi, nullptr, &depthView_), "depth view");
    // The resource just changed identity; sceneDepthTexture() re-adopts it lazily on next ask rather
    // than eagerly here, where rhiFactory_ isn't guaranteed to exist yet (this runs during swapchain
    // (re)creation, ahead of the first VulkanResourceFactory). Mirrors D3D12's identical depthTexDirty_=true.
    depthTexDirty_ = true;
    return viewOk;
}

bool VulkanDevice::createMsaaColor() {
    // Sized off the SCENE extent (see createDepthBuffer above); resized together with the depth buffer.
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kVkSceneColorFormat;
    ci.extent = {sceneWidth_, sceneHeight_, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = toVkSampleCount(sampleCount_);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toSceneReferred(clear_, sceneClear_);
    if (!createImageCommitted(*this, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, msaaColor_, msaaColorMemory_, "scene colour")) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = msaaColor_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kVkSceneColorFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkOk(api_.CreateImageView(device_, &vi, nullptr, &msaaColorView_), "msaa colour view");
}

// IDevice::setRenderScale, twin of D3D12Device::setRenderScale (C2-13, aver-render-scale-device-
// loss): clamp and early-out on no change against whichever value is currently authoritative --
// the parked one if a change is already pending, renderScale_ otherwise -- then PARK the new value
// rather than rebuilding here. This used to call rebuildSceneTargets() immediately, which was safe
// only as long as nothing called this mid-frame; the editor's preference load runs from buildUI(),
// between beginFrame() and endFrame(), and AverSR Auto's onUpdate reassert (wave 2) now calls this
// on every launch at Medium/Low's non-1.0 default. rebuildSceneTargets() frees and recreates the
// depth buffer, MSAA target and post chain while that frame's command buffer may already have them
// bound; waitForGpu() inside it only drains work already SUBMITTED, not a command buffer still
// being recorded on the CPU -- so a mid-frame call recorded a submit against freed images, which is
// what actually lost the device at present. So the value is parked and applied at the top of the
// next beginFrame, unconditionally, by applyPendingRenderScale(): no caller needs to know, and a
// rule with no exceptions cannot be got wrong by the next one. Before a swapchain exists there is
// no frame to be inside and nothing to rebuild -- createSwapchainResources()'s own
// computeSceneSize() picks up whatever renderScale_ was last set to, so that path still applies
// immediately, exactly as before.
void VulkanDevice::setRenderScale(f32 scale) {
    const f32 asked = scale;
    scale = std::fmax(0.25f, std::fmin(1.0f, scale));
    const f32 effective = pendingRenderScaleValid_ ? pendingRenderScale_ : renderScale_;
    if (scale == effective) return;
    if (!hasSwapchain_) {   // applied at the next createSwapchainResources
        AVER_INFO("[RHI.Vulkan] setRenderScale: {:.4f} -> {:.4f} (asked {:.4f}), before the swapchain",
                  renderScale_, scale, asked);
        renderScale_ = scale;
        return;
    }
    AVER_INFO("[RHI.Vulkan] setRenderScale: {:.4f} -> {:.4f} (asked {:.4f}), applied next frame",
              effective, scale, asked);
    pendingRenderScale_ = scale;
    pendingRenderScaleValid_ = true;
}

// Applies a PARKED setRenderScale() at a frame boundary; see this function's declaration comment
// (VulkanCommon.hpp) and setRenderScale's own comment above for the full account. Mirrors
// D3D12Device::applyPendingRenderScale() exactly, field for field.
void VulkanDevice::applyPendingRenderScale() {
    if (!pendingRenderScaleValid_) return;
    pendingRenderScaleValid_ = false;
    if (pendingRenderScale_ == renderScale_) return;
    renderScale_ = pendingRenderScale_;
    rebuildSceneTargets();
}

// Tears down and rebuilds every target sized off sceneWidth_/sceneHeight_ after renderScale_ moves
// with a swapchain already live. Structural mirror of D3D12Device::rebuildSceneTargets; the one
// difference is Vulkan's explicit VkImageView teardown (D3D12's ComPtr::Reset() frees the view
// implicitly -- a VkImageView is its own object here, destroyed by name before its VkImage).
void VulkanDevice::rebuildSceneTargets() {
    waitForGpu();   // nothing sized off the OLD scene extent may still be in flight when it is freed
    if (depthView_) { api_.DestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthBuffer_) { destroyImageCommitted(*this, depthBuffer_, depthMemory_); depthBuffer_ = VK_NULL_HANDLE; depthMemory_ = VK_NULL_HANDLE; }
    if (msaaColorView_) { api_.DestroyImageView(device_, msaaColorView_, nullptr); msaaColorView_ = VK_NULL_HANDLE; }
    if (msaaColor_) { destroyImageCommitted(*this, msaaColor_, msaaColorMemory_); msaaColor_ = VK_NULL_HANDLE; msaaColorMemory_ = VK_NULL_HANDLE; }
    computeSceneSize();
    vpX_ = vpY_ = vpW_ = vpH_ = 0;   // stored in scene-space; stale the instant sceneWidth_/sceneHeight_ moved
    createDepthBuffer();
    createMsaaColor();
    releasePostTargets();   // the resolve target and bloom pyramid are scene-sized too; rebuilt lazily by the next runPostChain
    notifyRenderTargetsChanged();
    AVER_TRACE("[RHI.Vulkan] render scale {:.2f} -> scene {}x{} (present {}x{})",
               renderScale_, sceneWidth_, sceneHeight_, width_, height_);
}

// ================================================================================================
// 8. Frame-pacing waits. See VulkanDevice's own field comments: ONE timeline semaphore stands in
//    for D3D12's fence_/fenceValues_/nextFence_ wholesale.
// ================================================================================================
bool VulkanDevice::waitTimeline(u64 value) {
    if (!timeline_ || value == 0) return true;
    u64 current = 0;
    if (api_.GetSemaphoreCounterValue(device_, timeline_, &current) == VK_SUCCESS && current >= value) return true;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1; wi.pSemaphores = &timeline_; wi.pValues = &value;
    for (u32 slice = 0;; ++slice) {
        const VkResult r = api_.WaitSemaphores(device_, &wi, 1'000'000'000ull);   // 1s slices
        if (r == VK_SUCCESS) return true;
        if (r == VK_ERROR_DEVICE_LOST) {
            AVER_ERROR("[RHI.Vulkan] the device was lost while waiting for the GPU");
            return false;
        }
        if (slice == 4)
            AVER_WARN("[RHI.Vulkan] still waiting on the GPU after 5 s; the device is healthy, so this "
                      "is a slow frame and not a hang");
    }
}

void VulkanDevice::waitForGpu() {
    if (!queue_ || !timeline_) return;
    const u64 v = ++nextTimelineValue_;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = timeline_; signalInfo.value = v; signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signalInfo;
    api_.QueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE);
    waitTimeline(v);
}

// ================================================================================================
// 9. Meshes, skin targets, lines. Vertex/index buffers for an ordinary mesh go THROUGH the generic
//    factory (rhiFactory_->createBuffer), exactly as D3D12Device::createMesh routes through
//    D3D12ResourceFactory -- this is what gives every mesh an RHI BufferHandle a shader can be given
//    descriptors over (IDevice::meshGeometry). Line meshes and per-frame CBs do NOT: they use
//    createBufferCommitted directly, per that free function's own comment, since nothing needs a
//    descriptor over a line list or this device's own per-frame constants.
// ================================================================================================
MeshHandle VulkanDevice::createMesh(const MeshVertex* verts, u32 vcount, const u32* indices, u32 icount) {
    if (!device_ || !rhiFactory_ || vcount == 0 || icount == 0) return 0;
    GpuMesh m;
    m.indexCount = icount;
    m.vertexCount = vcount;
    {
        f32 lo[3] = {verts[0].px, verts[0].py, verts[0].pz};
        f32 hi[3] = {verts[0].px, verts[0].py, verts[0].pz};
        for (u32 i = 1; i < vcount; ++i) {
            const f32 p[3] = {verts[i].px, verts[i].py, verts[i].pz};
            for (int a = 0; a < 3; ++a) { lo[a] = std::fmin(lo[a], p[a]); hi[a] = std::fmax(hi[a], p[a]); }
        }
        for (int a = 0; a < 3; ++a) m.boundsCentre[a] = 0.5f * (lo[a] + hi[a]);
        const f32 dx = hi[0] - m.boundsCentre[0], dy = hi[1] - m.boundsCentre[1], dz = hi[2] - m.boundsCentre[2];
        m.boundsRadius = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const u64 vbytes = static_cast<u64>(vcount) * sizeof(MeshVertex);
    const u64 ibytes = static_cast<u64>(icount) * sizeof(u32);

    // W4: --mesh-heap default (setStaticMeshHeapDefault) moves new static meshes off the Upload
    // heap this backend has always used, trading a one-shot upload-time staging copy for the
    // per-frame bus traffic every draw/shadow/voxelisation/BLAS-build fetching an Upload-heap
    // buffer back across the bus on a discrete card otherwise pays -- see IDevice's own contract
    // (RHI.hpp) and VoxiRenderer's BLAS-backing buffers, which already made this exact trade for
    // ray-tracing geometry. UNMEASURED here: this backend has no editor session on this machine to
    // run a load through (house rule: no run, no build).
    const bool useDefaultHeap = staticMeshDefaultHeap_;
    const BufferKind kind = useDefaultHeap ? BufferKind::Default : BufferKind::Upload;
    BufferDesc vd; vd.bytes = vbytes; vd.kind = kind; vd.debugName = "mesh vertices";
    m.vbBuffer = rhiFactory_->createBuffer(vd);
    BufferDesc idd; idd.bytes = ibytes; idd.kind = kind; idd.debugName = "mesh indices";
    m.ibBuffer = rhiFactory_->createBuffer(idd);

    RhiBuffer* vrb = rhiFactory_->buffer(m.vbBuffer);
    RhiBuffer* irb = rhiFactory_->buffer(m.ibBuffer);
    if (!vrb || !vrb->buffer || !irb || !irb->buffer) {
        AVER_ERROR("[RHI.Vulkan] createMesh could not allocate its buffers");
        if (m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        if (m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
        return 0;
    }
    m.vb = vrb->buffer; m.vbMemory = vrb->memory; m.vbAddress = vrb->address;
    m.ib = irb->buffer; m.ibMemory = irb->memory; m.ibAddress = irb->address;

    if (useDefaultHeap) {
        // ONE staging buffer, ONE one-shot submit for both buffers together -- see
        // uploadToDeviceBuffers' own comment for why this has to be synchronous (createMesh runs
        // mid-frame too -- the editor calls it from inside its own draw walk, e.g.
        // sandbox/src/SandboxRender.cpp's colourDecide building a cluster mesh, and from panel
        // code such as AssetEditor/AnimEditor) and why a SEPARATE command buffer is what makes
        // that legal (vkCmdCopyBuffer inside beginFrame's dynamic-rendering scope is not).
        const VkBuffer dsts[2] = {m.vb, m.ib};
        const void* srcs[2] = {verts, indices};
        const VkDeviceSize sizes[2] = {vbytes, ibytes};
        if (rhiFactory_->uploadToDeviceBuffers(dsts, srcs, sizes, 2, "rhi createMesh default-heap upload")) {
            if (!meshDefaultHeapLogged_) {
                meshDefaultHeapLogged_ = true;
                AVER_INFO("[RHI.Vulkan] static meshes on the Default heap (--mesh-heap default): "
                          "vertex and index buffers uploaded through a one-shot staging copy");
            }
        } else {
            // FALL BACK TO THE UPLOAD HEAP FOR THIS ONE MESH, not globally: staticMeshDefaultHeap_
            // stays on, so the NEXT mesh tries Default again -- a transient failure (a one-shot
            // command-buffer submit racing device teardown, say) should not silently downgrade every
            // later mesh for the rest of the run. WARN once: the failure mode that actually recurs
            // (out of device-local memory, a driver refusal) is systemic, and a line per mesh during
            // a big scene load would flood the log for one cause already stated once.
            if (!meshDefaultHeapFallbackWarned_) {
                meshDefaultHeapFallbackWarned_ = true;
                AVER_WARN("[RHI.Vulkan] Default-heap mesh upload failed; falling back to the Upload "
                          "heap for this mesh (further meshes still try the Default heap)");
            }
            rhiFactory_->destroyBuffer(m.vbBuffer);
            rhiFactory_->destroyBuffer(m.ibBuffer);
            BufferDesc vd2; vd2.bytes = vbytes; vd2.kind = BufferKind::Upload; vd2.debugName = "mesh vertices";
            m.vbBuffer = rhiFactory_->createBuffer(vd2);
            BufferDesc idd2; idd2.bytes = ibytes; idd2.kind = BufferKind::Upload; idd2.debugName = "mesh indices";
            m.ibBuffer = rhiFactory_->createBuffer(idd2);
            vrb = rhiFactory_->buffer(m.vbBuffer);
            irb = rhiFactory_->buffer(m.ibBuffer);
            if (!vrb || !vrb->buffer || !irb || !irb->buffer) {
                AVER_ERROR("[RHI.Vulkan] createMesh could not allocate its buffers on the Upload-heap fallback");
                if (m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
                if (m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
                return 0;
            }
            m.vb = vrb->buffer; m.vbMemory = vrb->memory; m.vbAddress = vrb->address;
            m.ib = irb->buffer; m.ibMemory = irb->memory; m.ibAddress = irb->address;
            rhiFactory_->writeBuffer(m.vbBuffer, verts, vbytes, 0);
            rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);
        }
    } else {
        rhiFactory_->writeBuffer(m.vbBuffer, verts, vbytes, 0);
        rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);
    }

    meshes_.push_back(std::move(m));
    return static_cast<MeshHandle>(meshes_.size());
}

// W11: a new mesh sharing `source`'s vertex buffer, with its own independent index buffer -- the
// LOD-ladder case IDevice::createMeshSharingVertices exists for (RHI.hpp's full contract): a
// coarser LOD level keeps the same vertex positions/normals/uvs and only thins out which triangles
// reference them, so duplicating the vertex stream per level is pure waste. See GpuMesh's
// vbOwned/vbShares/vbSource fields for the bookkeeping this mirrors from the existing
// ibOwned/ibShares/ibSource (createSkinTargetMesh) scheme, in the opposite direction.
MeshHandle VulkanDevice::createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) {
    return shareVertices(source, indices, indexCount, /*posed=*/false);
}
MeshHandle VulkanDevice::createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) {
    return shareVertices(posedSource, indices, indexCount, /*posed=*/true);
}
// Shared body -- D3D12Device::shareVertices' twin. `posed` flips which source is accepted, whether
// every index is range-checked, and whether the result is itself compute-written.
MeshHandle VulkanDevice::shareVertices(MeshHandle source, const u32* indices, u32 indexCount, bool posed) {
    const char* what = posed ? "createPosedPartMesh" : "createMeshSharingVertices";
    if (!device_ || !rhiFactory_ || !indices || indexCount == 0) return 0;
    if (source == 0 || source > meshes_.size()) {
        AVER_ERROR("[RHI.Vulkan] {} with an invalid source handle", what);
        return 0;
    }
    const GpuMesh& src = meshes_[source - 1];
    if (!src.alive || !src.vb || src.vertexCount == 0) return 0;
    // A compute-written (skin target) source's vertex buffer is rewritten every frame by whatever
    // last posed it -- sharing it would make every sharer's geometry jitter with that pose instead
    // of drawing its own, unposed LOD, which is not a bug a caller would think to suspect. RHI.hpp's
    // own contract states this refusal explicitly.
    if (src.computeWritten != posed) {
        if (posed) AVER_WARN("[RHI.Vulkan] createPosedPartMesh refused: mesh {} is not compute-written", source);
        else       AVER_WARN("[RHI.Vulkan] createMeshSharingVertices refused: source mesh {} is compute-written (a skin target)", source);
        return 0;
    }
    // Every index must name a vertex the posed buffer has -- see D3D12Device::shareVertices.
    if (posed) {
        for (u32 k = 0; k < indexCount; ++k)
            if (indices[k] >= src.vertexCount) {
                AVER_WARN("[RHI.Vulkan] createPosedPartMesh refused: index {} names vertex {} of {}-vertex mesh {}",
                          k, indices[k], src.vertexCount, source);
                return 0;
            }
    }

    // THE BUFFER THIS SHARE ACTUALLY POINTS AT: `source` itself when source owns its vertices, or
    // source's own root when source is ALREADY a share. A share never points at another share -- see
    // the vbShares increment below, which always lands on THIS resolved root, so a chain of
    // createMeshSharingVertices calls off the same original mesh collapses to one shared buffer and
    // one share count, never a multi-hop chain destroyMesh would have to walk.
    const MeshHandle root = src.vbOwned ? source : src.vbSource;
    if (root == 0 || root > meshes_.size()) return 0;
    const GpuMesh& rootMesh = meshes_[root - 1];
    if (!rootMesh.alive || !rootMesh.vb) return 0;

    const u64 ibytes = static_cast<u64>(indexCount) * sizeof(u32);
    const bool useDefaultHeap = staticMeshDefaultHeap_;
    const BufferKind kind = useDefaultHeap ? BufferKind::Default : BufferKind::Upload;
    const char* ibName = posed ? "mesh indices (posed part)" : "mesh indices (LOD, shared vertices)";
    BufferDesc idd; idd.bytes = ibytes; idd.kind = kind; idd.debugName = ibName;
    const BufferHandle ibBuffer = rhiFactory_->createBuffer(idd);
    if (!ibBuffer) { AVER_ERROR("[RHI.Vulkan] {} could not allocate its index buffer", what); return 0; }
    RhiBuffer* irb = rhiFactory_->buffer(ibBuffer);
    if (!irb || !irb->buffer) { rhiFactory_->destroyBuffer(ibBuffer); return 0; }

    GpuMesh m;
    // SHARED, not owned -- copied straight from the root, mirroring createSkinTargetMesh's identical
    // treatment of a shared index buffer just below (m.ib = src.ib; m.ibMemory = VK_NULL_HANDLE).
    m.vb = rootMesh.vb; m.vbMemory = rootMesh.vbMemory; m.vbAddress = rootMesh.vbAddress;
    m.vbBuffer = rootMesh.vbBuffer;
    m.vertexCount = rootMesh.vertexCount;
    m.vbOwned = false;
    m.vbSource = root;
    m.computeWritten = posed;   // a posed part IS posed geometry; see createPosedPartMesh

    m.ib = irb->buffer; m.ibMemory = irb->memory; m.ibAddress = irb->address;
    m.ibBuffer = ibBuffer;
    m.indexCount = indexCount;
    // ibOwned stays true (GpuMesh's default): this mesh's OWN index buffer, never shared.

    // Bounds are copied from `source` (RHI.hpp's contract), not from `root` -- identical today since
    // createSkinTargetMesh never changes bounds either, but the two need not always coincide and the
    // contract names source specifically.
    m.boundsCentre[0] = src.boundsCentre[0]; m.boundsCentre[1] = src.boundsCentre[1]; m.boundsCentre[2] = src.boundsCentre[2];
    m.boundsRadius = src.boundsRadius;

    if (useDefaultHeap) {
        const VkBuffer dsts[1] = {m.ib};
        const void* srcs[1] = {indices};
        const VkDeviceSize sizes[1] = {ibytes};
        if (rhiFactory_->uploadToDeviceBuffers(dsts, srcs, sizes, 1, "rhi createMeshSharingVertices default-heap upload")) {
            if (!meshDefaultHeapLogged_) {
                meshDefaultHeapLogged_ = true;
                AVER_INFO("[RHI.Vulkan] static meshes on the Default heap (--mesh-heap default): "
                          "vertex and index buffers uploaded through a one-shot staging copy");
            }
        } else {
            if (!meshDefaultHeapFallbackWarned_) {
                meshDefaultHeapFallbackWarned_ = true;
                AVER_WARN("[RHI.Vulkan] Default-heap mesh upload failed; falling back to the Upload "
                          "heap for this mesh (further meshes still try the Default heap)");
            }
            rhiFactory_->destroyBuffer(m.ibBuffer);
            BufferDesc idd2; idd2.bytes = ibytes; idd2.kind = BufferKind::Upload; idd2.debugName = ibName;
            m.ibBuffer = rhiFactory_->createBuffer(idd2);
            irb = rhiFactory_->buffer(m.ibBuffer);
            if (!irb || !irb->buffer) {
                AVER_ERROR("[RHI.Vulkan] {} could not allocate its index buffer on the Upload-heap fallback", what);
                if (m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
                return 0;
            }
            m.ib = irb->buffer; m.ibMemory = irb->memory; m.ibAddress = irb->address;
            rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);
        }
    } else {
        rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);
    }

    meshes_.push_back(std::move(m));
    const MeshHandle h = static_cast<MeshHandle>(meshes_.size());
    // AFTER push_back, by index rather than through the `rootMesh`/`src` references above: push_back
    // may have reallocated meshes_, exactly the reason createSkinTargetMesh's own ibShares increment
    // (:1791) is a fresh index lookup rather than a reuse of an earlier reference.
    meshes_[root - 1].vbShares += 1;
    return h;
}

MeshHandle VulkanDevice::createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) {
    if (!device_ || !rhiFactory_) return 0;
    if (source == 0 || source > meshes_.size()) {
        AVER_ERROR("[RHI.Vulkan] createSkinTargetMesh with an invalid source handle");
        return 0;
    }
    const GpuMesh& src = meshes_[source - 1];
    if (!src.vb || !src.ib || src.vertexCount == 0) return 0;

    BufferDesc bd;
    bd.bytes = static_cast<u64>(src.vertexCount) * sizeof(MeshVertex);
    bd.kind = BufferKind::Default;
    bd.allowUnorderedAccess = true;
    bd.debugName = "skin target vertices";
    const BufferHandle vh = rhiFactory_->createBuffer(bd);
    if (!vh) { AVER_ERROR("[RHI.Vulkan] createSkinTargetMesh could not allocate its vertex buffer"); return 0; }
    RhiBuffer* rb = rhiFactory_->buffer(vh);
    if (!rb || !rb->buffer) { rhiFactory_->destroyBuffer(vh); return 0; }

    GpuMesh m;
    m.vb = rb->buffer; m.vbMemory = rb->memory; m.vbAddress = rb->address;
    m.ib = src.ib; m.ibMemory = VK_NULL_HANDLE; m.ibAddress = src.ibAddress;   // SHARED, not owned
    m.indexCount = src.indexCount;
    m.vbBuffer = vh;
    m.ibBuffer = src.ibBuffer;
    m.ibOwned = false;
    m.ibSource = source;
    m.vertexCount = src.vertexCount;
    m.computeWritten = true;
    m.boundsCentre[0] = src.boundsCentre[0]; m.boundsCentre[1] = src.boundsCentre[1]; m.boundsCentre[2] = src.boundsCentre[2];
    m.boundsRadius = src.boundsRadius;

    meshes_.push_back(std::move(m));
    const MeshHandle h = static_cast<MeshHandle>(meshes_.size());
    meshes_[source - 1].ibShares += 1;   // AFTER push_back: push_back may have reallocated meshes_

    skinSeeds_.push_back({h, source});
    if (outVertices) *outVertices = vh;
    return h;
}

// Drains the queue of skin-target meshes awaiting their rest-pose seed, via the SAME IRenderContext
// a feature module would use -- correct because seedSkinTargets always runs after rhiContext_
// exists (createMesh/createSkinTargetMesh both require rhiFactory_, which is what constructs it).
void VulkanDevice::seedSkinTargets() {
    if (skinSeeds_.empty() || !rhiContext_) return;
    for (const SkinSeed& sd : skinSeeds_) {
        if (sd.dst == 0 || sd.dst > meshes_.size() || sd.src == 0 || sd.src > meshes_.size()) continue;
        GpuMesh& d = meshes_[sd.dst - 1];
        const GpuMesh& s = meshes_[sd.src - 1];
        if (!d.vb || !s.vb || !d.vbBuffer || !s.vbBuffer) continue;
        const u64 bytes = static_cast<u64>(d.vertexCount) * sizeof(MeshVertex);
        rhiContext_->bufferBarrier(d.vbBuffer, ResourceState::Common, ResourceState::CopyDest);
        rhiContext_->copyBuffer(d.vbBuffer, s.vbBuffer, bytes, 0, 0);
        // Left in Common, not UnorderedAccess: the next feature that skins this buffer issues its
        // own Common -> UnorderedAccess barrier before writing, mirroring D3D12's promotion reasoning.
        rhiContext_->bufferBarrier(d.vbBuffer, ResourceState::CopyDest, ResourceState::Common);
        AVER_TRACE("[RHI.Vulkan] skin target {} seeded with the rest pose of mesh {}", sd.dst, sd.src);
    }
    skinSeeds_.clear();
}

bool VulkanDevice::destroyMesh(MeshHandle mesh) {
    if (mesh == 0 || mesh > meshes_.size()) return false;
    GpuMesh& m = meshes_[mesh - 1];
    if (!m.alive) return false;
    if (m.ibShares > 0) {
        AVER_WARN("[RHI.Vulkan] destroyMesh({}) refused: {} skin target(s) still share its indices", mesh, m.ibShares);
        return false;
    }
    // W11: symmetric to the ibShares refusal above, for the other buffer this GpuMesh can share --
    // freeing a mesh whose VERTEX buffer a createMeshSharingVertices() LOD sibling still points at
    // would leave that sibling drawing freed memory.
    if (m.vbShares > 0) {
        AVER_WARN("[RHI.Vulkan] destroyMesh({}) refused: {} LOD mesh(es) still share its vertices", mesh, m.vbShares);
        return false;
    }
    if (rhiFactory_) rhiFactory_->destroyBlasForMesh(mesh);
    if (rhiFactory_) {
        // Only destroy the vertex buffer when THIS mesh owns it -- a share's vbBuffer handle names
        // the ROOT's buffer (createMeshSharingVertices copies it verbatim), and destroying it here
        // would free memory the root and every OTHER sharer still draws. Mirrors the ibOwned gate on
        // the very next line, which has made the identical check for the index buffer all along.
        if (m.vbOwned && m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        if (m.ibOwned && m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
    }
    if (!m.ibOwned && m.ibSource != 0 && m.ibSource <= meshes_.size()) {
        GpuMesh& src = meshes_[m.ibSource - 1];
        if (src.ibShares > 0) src.ibShares -= 1;
    }
    if (!m.vbOwned && m.vbSource != 0 && m.vbSource <= meshes_.size()) {
        GpuMesh& root = meshes_[m.vbSource - 1];
        if (root.vbShares > 0) root.vbShares -= 1;
    }
    m.vb = VK_NULL_HANDLE; m.ib = VK_NULL_HANDLE;
    m.vbMemory = VK_NULL_HANDLE; m.ibMemory = VK_NULL_HANDLE;
    m.vbAddress = m.ibAddress = 0;
    m.indexCount = 0; m.vertexCount = 0;
    m.vbBuffer = 0; m.ibBuffer = 0;
    m.computeWritten = false;
    m.ibSource = 0;
    m.vbOwned = true;
    m.vbSource = 0;
    m.boundsRadius = 0.0f;
    m.alive = false;
    return true;
}

BufferHandle VulkanDevice::meshVertexBuffer(MeshHandle mesh) const {
    if (!mesh || mesh > meshes_.size()) return 0;
    const GpuMesh& m = meshes_[mesh - 1];
    return m.computeWritten ? m.vbBuffer : 0;
}
bool VulkanDevice::meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib, u32* vertexCount, u32* indexCount) const {
    if (!mesh || mesh > meshes_.size()) return false;
    const GpuMesh& m = meshes_[mesh - 1];
    if (!m.vbBuffer || !m.ibBuffer) return false;
    if (vb) *vb = m.vbBuffer;
    if (ib) *ib = m.ibBuffer;
    if (vertexCount) *vertexCount = m.vertexCount;
    if (indexCount) *indexCount = m.indexCount;
    return true;
}
bool VulkanDevice::meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const {
    if (!mesh || mesh > meshes_.size()) return false;
    const GpuMesh& m = meshes_[mesh - 1];
    if (outCentre) { outCentre[0] = m.boundsCentre[0]; outCentre[1] = m.boundsCentre[1]; outCentre[2] = m.boundsCentre[2]; }
    if (outRadius) *outRadius = m.boundsRadius;
    return true;
}

LineHandle VulkanDevice::createLineMesh(const LineVertex* verts, u32 count) {
    if (!device_ || count == 0) return 0;
    GpuLineMesh m;
    m.count = count;
    const u64 bytes = static_cast<u64>(count) * sizeof(LineVertex);
    if (!createBufferCommitted(*this, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m.vb, m.vbMemory, nullptr, "line vb"))
        return 0;
    void* p = nullptr;
    api_.MapMemory(device_, m.vbMemory, 0, VK_WHOLE_SIZE, 0, &p);
    std::memcpy(p, verts, bytes);
    api_.UnmapMemory(device_, m.vbMemory);
    lineMeshes_.push_back(m);
    return static_cast<LineHandle>(lineMeshes_.size());
}

bool VulkanDevice::destroyLineMesh(LineHandle mesh) {
    if (mesh == 0 || mesh > lineMeshes_.size()) return false;
    GpuLineMesh& m = lineMeshes_[mesh - 1];
    if (m.vb == VK_NULL_HANDLE) return false;   // already released; saying so beats reporting a second success
    // DEFERRED, not immediate -- same reason as the D3D12 twin: a frame already submitted may still
    // be reading this buffer, and vkDestroyBuffer on it is UB that surfaces as an unrelated
    // validation error. VulkanResourceFactory::retire runs the deleter once the timeline semaphore
    // passes the value this frame retires behind -- the same machinery destroyBuffer uses.
    VkBuffer buf = m.vb;
    VkDeviceMemory mem = m.vbMemory;
    if (rhiFactory_) rhiFactory_->retire([this, buf, mem]() { destroyBufferCommitted(*this, buf, mem); });
    else destroyBufferCommitted(*this, buf, mem);   // no factory: nothing was ever submitted either
    // The slot is CLEARED AND KEPT, never recycled -- see IDevice::destroyLineMesh.
    m.vb = VK_NULL_HANDLE;
    m.vbMemory = VK_NULL_HANDLE;
    m.count = 0;
    return true;
}

void VulkanDevice::drawLines(LineHandle mesh, const f32 world[16]) {
    if (!hasSwapchain_ || mesh == 0 || mesh > lineMeshes_.size()) return;
    // suppressesWholeFrame, matching D3D12: gizmos belong in a ray-driven viewport.
    for (IRenderFeature* f : features_) if (f->suppressesWholeFrame()) return;
    const GpuLineMesh& m = lineMeshes_[mesh - 1];
    // A DESTROYED MESH DRAWS NOTHING -- the half that makes the kept-slot contract true.
    if (m.vb == VK_NULL_HANDLE || m.count == 0) return;
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    const u32 zeroOffset = 0;
    api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lineDepth_ ? linePso_ : lineOverlayPso_);
    api_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, kVkSetConstants, 1,
                               &sceneFrameSet_[frameIndex_], 1, &zeroOffset);
    api_.CmdPushConstants(cmd, scenePipelineLayout_, VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectOffset, 64, world);
    // Byte 65 onward is gBaseColor, whose .x carries the glow multiplier -- PSLine reads nothing else
    // from it. Pushed on EVERY line draw because push constants persist: otherwise the grid/sculpt
    // ring would inherit the last mesh's base-colour red as brightness. Mirrors D3D12Device::drawLines.
    api_.CmdPushConstants(cmd, scenePipelineLayout_, VK_SHADER_STAGE_ALL,
                          PushConstantLayout::kObjectOffset + 64, 4, &lineGlow_);
    VkDeviceSize off = 0;
    api_.CmdBindVertexBuffers(cmd, 0, 1, &m.vb, &off);
    api_.CmdDraw(cmd, m.count, 1, 0, 0);
}

// ================================================================================================
// 10. Camera, light, sky/atmosphere. PerFrameCB is a byte-for-byte mirror of D3D12Device.cpp's own
//     (and, one level further down, of `cbuffer PerFrame` in the shared prelude) -- these setters
//     are therefore straight memcpy/field writes, identical in shape to D3D12Device's.
// ================================================================================================
void VulkanDevice::setViewportRect(u32 x, u32 y, u32 w, u32 h) {
    if (w == 0 || h == 0 || x >= width_ || y >= height_) { vpX_ = vpY_ = vpW_ = vpH_ = 0; return; }
    const u32 cw = (x + w > width_) ? width_ - x : w;
    const u32 ch = (y + h > height_) ? height_ - y : h;
    // The caller thinks in present-space pixels, but the render target is the SCENE one --
    // msaaColor_/depthBuffer_ are sceneWidth_ x sceneHeight_, smaller than the backbuffer whenever
    // renderScale_ < 1 -- so the rect is scaled into scene space here once, rather than at every
    // reader. Identity at renderScale_==1.0. Mirrors D3D12Device::setViewportRect.
    vpX_ = scaleToSceneW(x);  vpY_ = scaleToSceneH(y);
    vpW_ = scaleToSceneW(cw); vpH_ = scaleToSceneH(ch);
    if (vpW_ == 0) vpW_ = 1;
    if (vpH_ == 0) vpH_ = 1;
}
void VulkanDevice::setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 camPos[3]) {
    std::memcpy(frameCB_.viewProj, viewProj, sizeof(frameCB_.viewProj));
    std::memcpy(frameCB_.invViewProj, invViewProj, sizeof(frameCB_.invViewProj));
    frameCB_.camPos[0] = camPos[0]; frameCB_.camPos[1] = camPos[1]; frameCB_.camPos[2] = camPos[2]; frameCB_.camPos[3] = 1;
}
bool VulkanDevice::camera(f32 viewProj[16], f32 invViewProj[16], f32 cameraPos[3]) const {
    if (viewProj) std::memcpy(viewProj, frameCB_.viewProj, sizeof(frameCB_.viewProj));
    if (invViewProj) std::memcpy(invViewProj, frameCB_.invViewProj, sizeof(frameCB_.invViewProj));
    if (cameraPos) std::memcpy(cameraPos, frameCB_.camPos, 3 * sizeof(f32));
    return true;
}
bool VulkanDevice::sceneViewport(f32 rect[4]) const {
    // Same rect beginFrame() sets as the scene's Vulkan viewport -- vpW_==0 means no sub-rect, i.e.
    // the whole scene target. SCENE-space: what a reprojecting feature needs, since its history
    // textures are sized off sceneWidth_/sceneHeight_, not width_/height_. Mirrors D3D12Device::sceneViewport.
    if (!rect) return true;
    rect[0] = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    rect[1] = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    rect[2] = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    rect[3] = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    return true;
}
void VulkanDevice::setLight(const f32 dir[3], const f32 color[3], f32 ambient) {
    frameCB_.lightDir[0] = dir[0]; frameCB_.lightDir[1] = dir[1]; frameCB_.lightDir[2] = dir[2]; frameCB_.lightDir[3] = 0;
    frameCB_.lightColor[0] = color[0]; frameCB_.lightColor[1] = color[1]; frameCB_.lightColor[2] = color[2]; frameCB_.lightColor[3] = 0;
    frameCB_.ambient[0] = frameCB_.ambient[1] = frameCB_.ambient[2] = ambient; frameCB_.ambient[3] = 0;
}

void VulkanDevice::toSceneReferred(const f32 display[4], f32 out[4]) {
    for (int i = 0; i < 3; ++i) {
        const f32 lin = std::pow(display[i] < 0.0f ? 0.0f : display[i], 2.2f);
        const f32 y = lin > 1.0329f - 1e-4f ? 1.0329f - 1e-4f : lin;
        const f32 a = 2.43f * y - 2.51f;
        const f32 b = 0.59f * y - 0.03f;
        const f32 c = 0.14f * y;
        const f32 d = b * b - 4.0f * a * c;
        out[i] = (-b - std::sqrt(d < 0.0f ? 0.0f : d)) / (2.0f * a);
    }
    out[3] = display[3];
}

void VulkanDevice::setSkyAtmosphere(const SkyAtmosphere& s) {
    sky_ = s;
    skyEnabled_ = s.enabled;

    for (int i = 0; i < 3; ++i) frameCB_.lightDir[i] = s.sunDirection[i];
    frameCB_.lightDir[3] = 0.0f;

    f32 sun[3] = {s.sunColor[0], s.sunColor[1], s.sunColor[2]};
    if (s.sunTemperatureK > 0.0f) {
        blackbodySrgb(s.sunTemperatureK, sun);
        // Mirrors D3D12Device::setSkyAtmosphere: blackbodySrgb returns LINEAR sRGB, but
        // lightColor is DISPLAY-ENCODED (packAtmosphere's e0 below and the shared HLSL prelude's
        // srgbToLin(gLightColor) both decode it with pow(x, 2.2)), so the kelvin path has to
        // re-encode here or a temperature-driven sun gets decoded twice.
        for (int i = 0; i < 3; ++i) sun[i] = std::pow(std::fmax(sun[i], 0.0f), 1.0f / 2.2f);
    }
    for (int i = 0; i < 3; ++i) frameCB_.lightColor[i] = sun[i];
    frameCB_.lightColor[3] = 0.0f;

    for (int i = 0; i < 3; ++i) {
        frameCB_.skyZenith[i]   = s.zenith[i];
        frameCB_.skyHorizon[i]  = s.horizon[i];
        frameCB_.fogColor[i]    = s.fogColor[i];
        frameCB_.groundColor[i] = s.groundAlbedo[i];
        frameCB_.ambient[i]     = s.skyLightIntensity;
    }
    frameCB_.skyZenith[3] = frameCB_.skyHorizon[3] = 0.0f;
    frameCB_.groundColor[3] = s.groundBlend;
    frameCB_.ambient[3] = 0.0f;
    frameCB_.fogColor[3] = s.fogDensity;

    frameCB_.skyParams[0] = s.atmosphereHeight > 0.01f ? s.atmosphereHeight : 0.01f;
    frameCB_.skyParams[1] = s.skyLightIntensity;
    frameCB_.skyParams[2] = s.sunIntensity;
    frameCB_.skyParams[3] = std::cos(s.sunAngularDiameterDeg * 0.5f * 0.017453292f);

    frameCB_.fogParams[0] = s.fogFalloff;
    frameCB_.fogParams[1] = s.fogHeight;
    frameCB_.fogParams[2] = s.fogStart;
    frameCB_.fogParams[3] = s.fogMaxOpacity;

    frameCB_.cloudParams[0] = s.cloudCoverage;
    frameCB_.cloudParams[1] = s.cloudDensity;
    frameCB_.cloudParams[2] = s.cloudBottom;
    frameCB_.cloudParams[3] = s.cloudTop > s.cloudBottom ? s.cloudTop : s.cloudBottom + 1.0f;
    if (s.cloudSeed == 0) {
        frameCB_.cloudMotion[0] = s.cloudWind[0] * s.cloudTime;
        frameCB_.cloudMotion[1] = s.cloudWind[1] * s.cloudTime;
    } else {
        u32 h = static_cast<u32>(s.cloudSeed) + 0x9E3779B9u;
        h = (h ^ (h >> 16)) * 0x21F0AAADu;
        h = (h ^ (h >> 15)) * 0x735A2D97u;
        h ^= h >> 15;
        frameCB_.cloudMotion[0] = s.cloudWind[0] * s.cloudTime + static_cast<f32>(h & 0xFFFFu) * 977.0f;
        frameCB_.cloudMotion[1] = s.cloudWind[1] * s.cloudTime + static_cast<f32>(h >> 16) * 1361.0f;
    }
    frameCB_.cloudMotion[2] = s.cloudScale;
    frameCB_.cloudMotion[3] = s.cloudsEnabled ? 1.0f : 0.0f;

    packAtmosphere(s);
}

void VulkanDevice::packAtmosphere(const SkyAtmosphere& s) {
    const AtmosphereProfile& a = s.air;
    const bool on = s.model == SkyModel::Physical;

    for (int i = 0; i < 3; ++i) {
        frameCB_.atmoRayleigh[i] = a.rayleighScatter[i];
        frameCB_.atmoOzone[i] = a.ozoneAbsorb[i];
    }
    frameCB_.atmoRayleigh[3] = a.rayleighScaleKm > 1e-3f ? a.rayleighScaleKm : 1e-3f;
    frameCB_.atmoOzone[3] = a.ozoneWidthKm > 1e-3f ? a.ozoneWidthKm : 1e-3f;
    frameCB_.atmoMie[0] = a.mieScatter;
    frameCB_.atmoMie[1] = a.mieExtinction;
    frameCB_.atmoMie[2] = a.mieScaleKm > 1e-3f ? a.mieScaleKm : 1e-3f;
    frameCB_.atmoMie[3] = a.miePhaseG;
    frameCB_.atmoPlanet[0] = a.planetRadiusKm;
    frameCB_.atmoPlanet[1] = a.planetRadiusKm + a.atmosphereHeightKm;
    frameCB_.atmoPlanet[2] = 1e-5f;
    frameCB_.atmoPlanet[3] = on ? 1.0f : 0.0f;
    frameCB_.atmoTune[0] = a.ozoneCentreKm;
    frameCB_.atmoTune[1] = a.multiScatterGain;
    frameCB_.atmoTune[2] = static_cast<f32>(a.viewSteps > 1 ? a.viewSteps : 1);
    frameCB_.atmoTune[3] = static_cast<f32>(a.aerialSteps > 1 ? a.aerialSteps : 1);

    frameCB_.furnace[0] = s.furnaceRadiance > 0.0f ? 1.0f : 0.0f;
    frameCB_.furnace[1] = s.furnaceRadiance;
    frameCB_.furnace[2] = s.furnaceSun ? 1.0f : 0.0f;
    frameCB_.furnace[3] = 0.0f;

    if (!on) {
        for (int i = 0; i < 4; ++i) frameCB_.atmoSunE0[i] = 0.0f;
        return;
    }

    AtmosphereProfile fit = a;
    f32 groundLin[3];
    for (int i = 0; i < 3; ++i) groundLin[i] = std::pow(std::fmax(s.groundAlbedo[i], 0.0f), 2.2f);
    fit.groundAlbedo = 0.2126f * groundLin[0] + 0.7152f * groundLin[1] + 0.0722f * groundLin[2];

    f32 e0[3];
    for (int i = 0; i < 3; ++i) e0[i] = std::pow(std::fmax(frameCB_.lightColor[i], 0.0f), 2.2f) * s.sunIntensity;
    for (int i = 0; i < 3; ++i) frameCB_.atmoSunE0[i] = e0[i];
    frameCB_.atmoSunE0[3] = fit.groundAlbedo;

    const f32 len = std::sqrt(s.sunDirection[0] * s.sunDirection[0] + s.sunDirection[1] * s.sunDirection[1] +
                              s.sunDirection[2] * s.sunDirection[2]);
    const f32 sunCos = len > 1e-6f ? s.sunDirection[2] / len : 1.0f;
    const f32 sunRadius = s.sunAngularDiameterDeg * 0.5f * 0.017453292f;
    AtmosphereDome dome{};
    atmoFitDome(fit, 0.0f, sunCos, e0, sunRadius, dome);

    for (int i = 0; i < 3; ++i) {
        frameCB_.skyZenith[i] = std::pow(std::fmax(dome.zenith[i], 0.0f), 1.0f / 2.2f);
        frameCB_.skyHorizon[i] = std::pow(std::fmax(dome.horizon[i], 0.0f), 1.0f / 2.2f);
        frameCB_.lightColor[i] = std::pow(std::fmax(e0[i] * dome.sunTransmittance[i] / std::fmax(s.sunIntensity, 1e-6f), 0.0f), 1.0f / 2.2f);
    }
    frameCB_.skyParams[0] = dome.exponent;

    const f32 altKm = std::fmax(frameCB_.camPos[2] * frameCB_.atmoPlanet[2], 1e-3f);
    f32 fogRef[3];
    atmoFogInscatterRef(fit, altKm, s.sunDirection, e0, sunRadius, fogRef);
    for (int i = 0; i < 3; ++i) frameCB_.fogInscatterRef[i] = fogRef[i];
    frameCB_.fogInscatterRef[3] = 0.0f;

    // Mirrors D3D12Device::packAtmosphere field for field; see its comment.
    AtmosphereSkySH sh{};
    atmoSkyRadianceSH(fit, altKm, s.sunDirection, e0, sunRadius, sh);
    // THE SAME CALIBRATION D3D12Device APPLIES, and it has to be the same or the two backends
    // disagree about how much light the sky delivers -- see that copy for why it is 1 (the
    // atmosphere's SH is already in the sun's units) and where the old 8 came from. Change one,
    // change both.
    constexpr f32 kSkyIrradianceCalibration = 1.0f;
    for (int k = 0; k < 9; ++k) {
        for (int i = 0; i < 3; ++i) frameCB_.skySh[k][i] = sh.c[k][i] * kSkyIrradianceCalibration;
        frameCB_.skySh[k][3] = 0.0f;
    }
}

// ================================================================================================
// 11. The central per-instance draw. Structurally identical to D3D12Device::drawMesh: every
//     feature's submitDraw first, then suppression, then any overridesScenePipeline feature's own
//     PSO/bindings for THIS draw, then the backend's own fixed pipeline as the fallback.
//
//     UNLIKE D3D12Device, there is no boundRootSig_/boundPso_ cache: this backend re-binds the
//     pipeline and b0 descriptor set on every call. Redundant, not incorrect -- an optimisation left
//     for later, not a behavioural difference a caller could observe.
// ================================================================================================
void VulkanDevice::drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) {
    // AUTO-CONSUME nextDrawPrepassed_ before any early return, per its contract (RHI.hpp:462-475),
    // same as D3D12Device::drawMesh (D3D12Device.cpp:3096-3100): the flag must not leak onto a later,
    // unrelated draw just because this one bailed out early (dead mesh, no swapchain).
    // A compute-written mesh counts as prepassed ONLY if drawMeshDepthOnly just depth-drew this exact
    // handle -- see D3D12Device::drawMesh's twin.
    const bool prepassed = nextDrawPrepassed_ &&
        (meshVertexBuffer(mesh) == 0 || (depthOnlyMesh_ != 0 && mesh == depthOnlyMesh_));
    nextDrawPrepassed_ = false;
    depthOnlyMesh_ = 0;
    if (!hasSwapchain_ || mesh == 0 || mesh > meshes_.size()) return;
    if (!meshes_[mesh - 1].alive) return;
    // `blended` explicit and false: TRANSLUCENT MESHES ARE D3D12-ONLY TODAY. IDevice::setDrawBlended
    // (RHI.hpp:613) is the seam a caller marks a draw translucent with, but VulkanDevice doesn't
    // override it -- the base setter is a no-op and drawBlended() always answers false. So every draw
    // here is opaque by construction; both explicit `false`s below say so rather than leaning on a default.
    for (IRenderFeature* f : features_)
        f->submitDraw(mesh, world, color, metallic, roughness, drawBinding_.set, drawBinding_.constants, drawBinding_.bytes, /*blended=*/false);
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;

    // CONCRETELY WRONG, NOT JUST UNSUPPORTED: a caller that marks a glass mesh blended and draws it
    // here gets the same opaque path as everything else -- scenePso_/wirePso_/meshPso_ each build a
    // single hardcoded-opaque VkPipelineColorBlendAttachmentState (createPipeline() ~1321,
    // initMeshShaders() ~1490) with no BlendMode read anywhere. The glass paints solid instead of
    // vanishing; nothing logs or asserts it.
    //
    // WHAT REAL SUPPORT WOULD TAKE: (a) override setDrawBlended/drawBlended with a sticky bool reset
    // in beginFrame, like D3D12Device's; (b) after the submitDraw loop above (so a feature like the
    // path tracer still sees the blended instance), divert a blended mesh into a new per-frame vector
    // instead of walking scenePipeline here; (c) in endFrame, after the sky draw (its depth-EQUAL
    // no-write PSO would paint over blended geometry drawn earlier) and before transparentPass, sort
    // that vector back-to-front and replay it through a blendEnable=VK_TRUE variant of each PSO
    // (toVkBlendAttachment already builds one for BlendMode::AlphaBlend,
    // VulkanResourceFactory.cpp:500-528) paired with depth-write-off.
    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline() || !rhiContext_) continue;
        // A PREPASSED DRAW GOES DOWN THE SAME GEOMETRY PATH ITS DEPTH WAS WRITTEN THROUGH -- the
        // input assembler, because drawMeshDepthPrepass has no mesh-shader twin. Passing msActive_
        // here unconditionally handed every prepassed draw the ORDINARY mesh-shader pipeline (Less,
        // write on), which then rejected the equal depth the prepass had just written for the same
        // triangle, and the frame came out almost entirely unshaded. D3D12Device::drawMesh's twin
        // carries the full account and the measurement.
        const bool featureMs = msActive_ && meshPso_ && !prepassed;
        const PipelineHandle fp = f->scenePipeline(featureMs, wireframe_, prepassed, false);
        if (!fp) break;
        rhiContext_->setPipeline(fp);
        if (const BindingSetHandle bs = f->sceneBindingSet()) rhiContext_->setBindingSet(bs, 0);
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        const void* cb = nullptr; u32 cbBytes = 0;
        if (f->sceneConstants(&cb, &cbBytes) && cb && cbBytes) rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
        f32 fc[kObjectConstantDwords];
        std::memcpy(fc, world, 16 * sizeof(f32));
        std::memcpy(fc + 16, color, 4 * sizeof(f32));
        fc[20] = metallic; fc[21] = roughness; fc[22] = unlit_ ? 1.0f : 0.0f; fc[23] = 0.0f;
        // UNLIT REACHES THE MATERIAL SHADER TOO, not just gMaterial.z. The twin of
        // D3D12Device.cpp's call; see writeShadingConstants there for why both fields are written.
        writeShadingConstants(fc, unlit_);
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        // featureMs, not msActive_: the draw call has to match the pipeline chosen above.
        if (featureMs && !wireframe_) rhiContext_->dispatchMeshFor(mesh);
        else                          rhiContext_->drawMesh(mesh);
        return;
    }

    if (drawBinding_.set && !drawBindingIgnored_) {
        AVER_WARN("[RHI.Vulkan] a per-draw binding is set but the scene uses the backend's own pipeline, "
                  "which declares no table 1; it is ignored");
        drawBindingIgnored_ = true;
    }

    const GpuMesh& m = meshes_[mesh - 1];
    const bool useMs = msActive_ && meshPso_ && !wireframe_;
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    VkPipeline pso = useMs ? meshPso_ : (wireframe_ ? wirePso_ : scenePso_);
    VkPipelineLayout layout = useMs ? meshPipelineLayout_ : scenePipelineLayout_;
    const u32 zeroOffset = 0;
    api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    api_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, kVkSetConstants, 1,
                               &sceneFrameSet_[frameIndex_], 1, &zeroOffset);

    f32 consts[kObjectConstantDwords];
    std::memcpy(consts, world, 16 * sizeof(f32));
    std::memcpy(consts + 16, color, 4 * sizeof(f32));
    consts[20] = metallic; consts[21] = roughness; consts[22] = unlit_ ? 1.0f : 0.0f; consts[23] = 0.0f;
    writeShadingConstants(consts, unlit_);
    api_.CmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectOffset, sizeof(consts), consts);

    if (useMs) { dispatchMesh(m); return; }
    VkDeviceSize off = 0;
    api_.CmdBindVertexBuffers(cmd, 0, 1, &m.vb, &off);
    api_.CmdBindIndexBuffer(cmd, m.ib, 0, VK_INDEX_TYPE_UINT32);
    api_.CmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
}

// ================================================================================================
// 11b. drawMeshDepthPrepass -- draws `mesh`'s depth ONLY, through whichever registered feature both
//     overridesScenePipeline() and offers a non-zero depthPrepassPipeline(). See IDevice's comment
//     (RHI.hpp:449-460) and D3D12Device::drawMeshDepthPrepass (D3D12Device.cpp:3033-3093), which
//     this copies structurally: same walk over features_, same table-0/frame-CB/table-1/object-
//     constant sequence, just a different pipeline and no colour/material tail. Not folded into
//     drawMesh(): the caller (SandboxApp's prepass phase) runs this from a separate, earlier walk,
//     never interleaved per-instance with colour draws (matters to the GPU stat tree's span budget).
//
//     UNLIKE D3D12Device, there is no fovPso_/fovSet_/fovCbBytes_ elision cache here: drawMesh()
//     above already re-binds pipeline and b0 set on every call, so this does the same -- redundant
//     relative to D3D12's cached path, not incorrect.
// ================================================================================================
// D3D12Device's twins, same split: the frame-wide prepass is gated and counted, the per-draw one
// (IDevice::drawMeshDepthOnly, for alpha-masked draws) is neither, and both share one body.
void VulkanDevice::drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
    if (!depthPrepassEnabled_) return;
    if (depthOnlyDraw(mesh, world, color, /*allowComputeWritten=*/false)) ++depthPrepassDrawsThisFrame_;
}
bool VulkanDevice::drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
    const bool wrote = depthOnlyDraw(mesh, world, color, /*allowComputeWritten=*/true);
    depthOnlyMesh_ = wrote ? mesh : 0;
    return wrote;
}
bool VulkanDevice::depthOnlyDraw(MeshHandle mesh, const f32 world[16], const f32 color[4],
                                 bool allowComputeWritten) {
    if (!hasSwapchain_ || !rhiContext_ || mesh == 0 || mesh > meshes_.size()) return false;
    if (!meshes_[mesh - 1].alive) return false;
    // Compute-written meshes: excluded from the FRAME-WIDE prepass, accepted by the per-draw path
    // (m.vb IS the posed buffer; drawMesh honours `prepassed` via depthOnlyMesh_). See D3D12's twin.
    if (!allowComputeWritten && meshVertexBuffer(mesh) != 0) return false;
    // No raster colour pass to consume it, so no raster depth -- see D3D12Device::depthOnlyDraw.
    if (wireframe_) return false;
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return false;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline()) continue;
        const PipelineHandle pp = f->depthPrepassPipeline();
        if (!pp) return false;   // this feature has no prepass PSO; nothing else offers one either today
        rhiContext_->setPipeline(pp);
        if (const BindingSetHandle bs = f->sceneBindingSet()) rhiContext_->setBindingSet(bs, 0);
        const void* cb = nullptr; u32 cbBytes = 0;
        if (f->sceneConstants(&cb, &cbBytes) && cb && cbBytes) rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
        // Table 1: the SAME material binding the caller set via setDrawBinding for this instance's
        // upcoming colour draw -- PSDepthPrepass reads gBaseColorMap/gAlphaCutoff/gMaterialFlags out
        // of exactly that set, same registers PSMainVoxi reads them at (both pipelines share `gi`,
        // VoxiRenderer.cpp's giLayout()).
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        f32 fc[kObjectConstantDwords] = {};
        std::memcpy(fc, world, 16 * sizeof(f32));
        // gBaseColor -- PSDepthPrepass's alpha test multiplies by its .a. See D3D12Device's twin.
        if (color) std::memcpy(fc + 16, color, 4 * sizeof(f32));
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        rhiContext_->drawMesh(mesh);
        return true;
    }
    return false;
}

// ================================================================================================
// 12. ISwapchain / IDevice surface: createSwapchain, setSampleCount, viewport-to-texture,
//     notifyRenderTargetsChanged.
// ================================================================================================
ISwapchain* VulkanDevice::createSwapchain(const SwapchainDesc& desc) {
    if (!createSwapchainResources(desc)) return nullptr;
    return new VulkanSwapchain(this);
}

bool VulkanDevice::setSampleCount(u32 samples) {
    if (samples == sampleCount_) return true;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) return false;
    if (!(caps_.msaaMask & samples)) return false;

    waitForGpu();
    const u32 prev = sampleCount_;
    sampleCount_ = samples;
    if (!createPipeline()) { sampleCount_ = prev; createPipeline(); return false; }
    if (msSupported_) initMeshShaders();
    if (hasSwapchain_) {
        if (depthView_) { api_.DestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
        if (depthBuffer_) { destroyImageCommitted(*this, depthBuffer_, depthMemory_); depthBuffer_ = VK_NULL_HANDLE; depthMemory_ = VK_NULL_HANDLE; }
        if (msaaColorView_) { api_.DestroyImageView(device_, msaaColorView_, nullptr); msaaColorView_ = VK_NULL_HANDLE; }
        if (msaaColor_) { destroyImageCommitted(*this, msaaColor_, msaaColorMemory_); msaaColor_ = VK_NULL_HANDLE; msaaColorMemory_ = VK_NULL_HANDLE; }
        if (!createDepthBuffer() || !createMsaaColor()) { AVER_ERROR("[RHI.Vulkan] MSAA {}x target creation failed", samples); return false; }
        releasePostTargets();
    }
    notifyRenderTargetsChanged();
    AVER_INFO("[RHI.Vulkan] MSAA set to {}x", samples);
    return true;
}

bool VulkanDevice::ensureViewportTexture() {
    IResourceFactory* f = resources();
    if (!f || width_ == 0 || height_ == 0) return false;
    if (viewportTex_ && viewportTexW_ == width_ && viewportTexH_ == height_) return true;
    if (viewportTex_) { f->waitIdle(); f->destroyTexture(viewportTex_); viewportTex_ = 0; }
    TextureDesc d;
    d.width = width_; d.height = height_;
    d.format = fromVkFormat(swapchainFormat_);
    d.bind = ResourceBind::RenderTarget | ResourceBind::ShaderResource;
    d.initialState = ResourceState::ShaderResource;
    d.hasClearValue = true;
    d.debugName = "Viewport.Composite";
    viewportTex_ = f->createTexture(d);
    viewportTexW_ = width_; viewportTexH_ = height_;
    if (!viewportTex_) { AVER_ERROR("[RHI.Vulkan] the viewport texture could not be created"); return false; }
    AVER_INFO("[RHI.Vulkan] viewport composited to a texture ({}x{})", width_, height_);
    return true;
}
u64 VulkanDevice::viewportTextureId() {
    // Once a UI backend exists this is just another texture: the editor draws the composited scene
    // in its viewport panel via this handle. Returned a hard 0 while there was no toolkit to make
    // one -- why the viewport rendered black the first time the UI itself came up.
    if (!viewportToTex_ || !ensureViewportTexture()) return 0;
    return uiTextureId(viewportTex_);
}

void VulkanDevice::notifyRenderTargetsChanged() {
    // SCENE size, not present size: a render feature builds its pipelines and size-dependent targets
    // against whatever a scene draw actually rasterises into (sceneWidth_/sceneHeight_, now that
    // renderScale_ can decouple the two). Mirrors D3D12Device::notifyRenderTargetsChanged exactly.
    if (sampleCount_ == notifiedSamples_ && backbufferFormat() == notifiedColor_ && depthFormat() == notifiedDepth_ &&
        sceneWidth_ == notifiedWidth_ && sceneHeight_ == notifiedHeight_) return;
    notifiedSamples_ = sampleCount_;
    notifiedColor_ = backbufferFormat();
    notifiedDepth_ = depthFormat();
    notifiedWidth_ = sceneWidth_;
    notifiedHeight_ = sceneHeight_;
    for (IRenderFeature* f : features_) f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat(), sceneWidth_, sceneHeight_);
}

// ================================================================================================
// 13. beginFrame / endFrame / present / resize. The frame-pacing core: see the header's own note on
//     timeline_/frameTimelineValues_/imageAvailable_/renderFinished_ for the full scheme.
// ================================================================================================
void VulkanDevice::beginFrame() {
    if (!hasSwapchain_) return;
    // FIRST, BEFORE ANYTHING RECORDS: a render-scale change frees and recreates the depth buffer,
    // MSAA target and post chain, and doing that mid-recording is what lost the device at Present
    // (C2-13, aver-render-scale-device-loss). See setRenderScale's comment for the full account.
    applyPendingRenderScale();

    ++frameSerial_;
    frameIndex_ = (frameIndex_ + 1) % kFrameCount;
    waitTimeline(frameTimelineValues_[frameIndex_]);
    if (meshGeomPool_[frameIndex_]) api_.ResetDescriptorPool(device_, meshGeomPool_[frameIndex_], 0);

    api_.ResetCommandBuffer(commandBuffers_[frameIndex_], 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api_.BeginCommandBuffer(commandBuffers_[frameIndex_], &bi);
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    const VkResult ar = api_.AcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[frameIndex_], VK_NULL_HANDLE, &imageIndex_);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        // The window changed since the last resize() call reached us; rebuild at the size we still
        // believe is current and skip drawing this frame -- the caller's next resize() (or the next
        // beginFrame after one) will pick up the real size.
        api_.EndCommandBuffer(cmd);
        resize(width_, height_);
        return;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { vkOk(ar, "vkAcquireNextImageKHR"); return; }

    postRing_[frameIndex_].used = 0;
    drawBinding_ = defaultDrawBinding_;
    depthOnlyMesh_ = 0;   // backstop; drawMesh consumes it -- see D3D12Device's beginFrame
    seedSkinTargets();

    std::memcpy(frameCBPtr_[frameIndex_], &frameCB_, sizeof(PerFrameCB));

    for (IRenderFeature* f : features_) f->beginScene();
    if (rhiContext_) for (IRenderFeature* f : features_) f->prePass(*rhiContext_);

    const bool msaa = sampleCount_ > 1;
    VkImageMemoryBarrier2 startBarriers[3];
    u32 startCount = 0;
    startBarriers[startCount++] = imgBarrier(msaaColor_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    startBarriers[startCount++] = imgBarrier(depthBuffer_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                             VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    if (msaa && sceneResolved_)
        startBarriers[startCount++] = imgBarrier(sceneResolved_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    pipelineBarrier(api_, cmd, startBarriers, startCount);

    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView = msaaColorView_;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colorAtt.clearValue.color.float32, sceneClear_, sizeof(sceneClear_));
    // AND sceneResolvedView_, which the barrier above already guards on: createPostTargets builds
    // the resolve target only when sampleCount_ > 1 AND it gets far enough to build it, so "MSAA is
    // on" alone doesn't mean there's somewhere to resolve TO. Asking for a resolve without one is
    // rejected outright ("resolveMode ... is not VK_RESOLVE_MODE_NONE, resolveImageView must not be
    // VK_NULL_HANDLE") -- the two guards disagreeing was the bug.
    if (msaa && sceneResolvedView_) {
        // The MSAA resolve happens HERE, ending this rendering scope -- no separate vkCmdResolveImage
        // call exists in this backend; VkRenderingAttachmentInfo's own resolveMode/resolveImageView/
        // resolveImageLayout fields do it inline, simpler than D3D12's ResolveSubresource, not missing.
        colorAtt.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        colorAtt.resolveImageView = sceneResolvedView_;
        colorAtt.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depthView_;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    // The rendering scope's area is the SCENE extent -- msaaColorView_/depthView_ are now
    // sceneWidth_ x sceneHeight_, and Vulkan dynamic rendering requires renderArea to fit inside every attachment.
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {sceneWidth_, sceneHeight_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;
    sceneScopeOpened_ = pushRenderScope(cmd, ri);

    // vpX_/vpY_/vpW_/vpH_ are already in SCENE space (setViewportRect rescales via scaleToSceneW/H
    // before storing). The "no sub-rect" fallback must be the scene's own extent, not the present
    // one, or renderScale_<1 would set a viewport larger than the render area -- validation error, UB without it.
    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
    VkViewport vp{rx, ry, rw, rh, 0.0f, 1.0f};
    VkRect2D sc{{static_cast<i32>(rx), static_cast<i32>(ry)}, {static_cast<u32>(rw), static_cast<u32>(rh)}};
    api_.CmdSetViewport(cmd, 0, 1, &vp);
    api_.CmdSetScissor(cmd, 0, 1, &sc);

    sceneSuppressed_ = false;
    frameSuppressed_ = false;
    IRenderFeature* winner = nullptr;
    u32 claimants = 0;
    for (IRenderFeature* f : features_) {
        if (!f->suppressesScene()) continue;
        ++claimants;
        if (!winner) winner = f;
    }
    // Kept in step with D3D12Device::beginFrame (full account there): only the first claimant paints,
    // registration order decides it, and nothing said so -- including the SINGLE-claimant case, which
    // is the one that actually cost a measurement. See IRenderFeature::suppressesScene.
    if (winner != lastSuppressWinner_ || claimants != lastSuppressClaimants_) {
        if (claimants > 1)
            AVER_WARN("[RHI.Vulkan] {} render features claim the whole scene; '{}' wins on "
                      "registration order and the others will not paint. The rasteriser draws "
                      "NOTHING while this holds -- if you are comparing render paths, this is not "
                      "the comparison you think it is.", claimants, winner->name());
        else if (winner)
            AVER_INFO("[RHI.Vulkan] '{}' is painting the scene; the rasteriser's drawMesh calls are "
                      "being dropped. Any 'scene draw' timing below is that feature, not raster.",
                      winner->name());
        else if (lastSuppressWinner_)
            AVER_INFO("[RHI.Vulkan] the rasteriser is painting the scene again; no feature is "
                      "suppressing it.");
    }
    lastSuppressWinner_    = winner;
    lastSuppressClaimants_ = claimants;
    if (winner) {
        if (rhiContext_) winner->scenePass(*rhiContext_);
        sceneSuppressed_ = true;
        if (winner->suppressesWholeFrame()) frameSuppressed_ = true;
        return;
    }
    // Unlike D3D12Device::beginFrame, nothing is bound here as a "default" pipeline: every draw
    // entry point binds its own pipeline before drawing, and there's no field to cache "last bound"
    // against (see drawMesh's own comment) -- nothing for a default bind here to save.
}

// KNOWN DEFECT, MEASURED, NOT YET FIXED: A PASS THAT WANTS ITS OWN RENDER TARGET DOES NOT GET ONE.
//
// This joins whatever scope is already open and DISCARDS the caller's VkRenderingInfo -- right for
// the case it was written for (two owners recording into one command buffer; dynamic-rendering
// scopes cannot nest), wrong whenever the inner request names DIFFERENT attachments.
//
// WHAT IT COSTS: VoxiRenderer::shadowPass calls setRenderTargets(nullptr, 0, shadowTex_)
// (VoxiRenderer.cpp:1175) and draws from inside the scene scope beginFrame already opened, so those
// draws join the SCENE's attachments and the cascade shadow map is never written. Nothing looks
// obviously broken -- light-space geometry lands outside the scene viewport and is just clipped --
// the frame simply has no cascade shadows. Probed beside the cube in the default scene with --no-rt
// (forces the cascade path; ray-traced shadows are the default and DO work here):
//     D3D12  darkest floor pixel (13, 15, 19)  -- a shadow
//     Vulkan darkest floor pixel (66, 66, 66)  -- unshadowed floor colour, no shadow anywhere
// giShadowPass (:1293) has the same shape, so GI's shadow map is equally empty and voxel light
// injection is unshadowed on this backend.
//
// THE ONE-LINE FIX (tried, reverted): closing the open scope and opening the requested one does
// retarget the shadow map correctly ("[Voxi shadow map]" attaches, draws arrive), but three
// mismatches surface immediately -- the bound pipeline's rasterizationSamples and
// VkPipelineRenderingCreateInfo formats are the SCENE's, not the shadow map's, and the shadow image
// is never transitioned to DEPTH_ATTACHMENT_OPTIMAL. Validation went 10 -> 40 and the floor stopped
// drawing entirely.
//
// THE REAL FIX belongs with setRenderTargets: the scope must FOLLOW the currently-bound targets
// (close on retarget, reopen lazily at the next draw, persist across draws), and pipelines must be
// built against the sample count and formats of the targets they'll actually use -- a change to how
// every pass gets its scope, wanted deliberately rather than as a patch under a shadow bug.
bool VulkanDevice::pushRenderScope(VkCommandBuffer cmd, const VkRenderingInfo& ri) {
    if (renderScopeDepth_ != 0) return false;   // already inside one: join it rather than nest
    api_.CmdBeginRendering(cmd, &ri);
    ++renderScopeDepth_;
    return true;
}

void VulkanDevice::popRenderScope(VkCommandBuffer cmd, bool opened) {
    if (!opened || renderScopeDepth_ == 0) return;
    --renderScopeDepth_;
    api_.CmdEndRendering(cmd);
}

void VulkanDevice::endFrame() {
    if (!hasSwapchain_) return;
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    // THE DEFERRED SKY DRAW -- same timing as D3D12Device::endFrame: after every opaque drawMesh
    // call, before the rendering scope closes. frameSuppressed_, not sceneSuppressed_ -- see D3D12's comment.
    if (skyEnabled_ && !frameSuppressed_) {
        // Still inside the rendering scope over msaaColorView_/depthView_ (SCENE-sized) -- see beginFrame's fallback.
        const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
        const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
        const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(sceneWidth_);
        const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(sceneHeight_);
        VkViewport vp{rx, ry, rw, rh, 0.0f, 1.0f};
        VkRect2D sc{{static_cast<i32>(rx), static_cast<i32>(ry)}, {static_cast<u32>(rw), static_cast<u32>(rh)}};
        api_.CmdSetViewport(cmd, 0, 1, &vp);
        api_.CmdSetScissor(cmd, 0, 1, &sc);
        const u32 zeroOffset = 0;
        api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPso_);
        api_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, kVkSetConstants, 1,
                                   &sceneFrameSet_[frameIndex_], 1, &zeroOffset);
        api_.CmdDraw(cmd, 3, 1, 0, 0);
    }

    if (!sceneSuppressed_ || true) {
        // Always closes the scope this frame's beginFrame opened, suppressed or not -- a suppressed
        // scene still drew INTO msaaColor_/depthView_ via scenePass(), it just skipped the sky.
        popRenderScope(cmd, sceneScopeOpened_);
        sceneScopeOpened_ = false;
    }

    // Barrier the resolved scene (or, with no MSAA, msaaColor_ itself) into something the post
    // chain's shaders can sample.
    const bool msaa = sampleCount_ > 1;
    VkImage sceneImg = msaa ? sceneResolved_ : msaaColor_;
    VkImageMemoryBarrier2 toSrv = imgBarrier(sceneImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    pipelineBarrier(api_, cmd, &toSrv, 1);

    runPostChain(swapchainImages_[imageIndex_], swapchainViews_[imageIndex_], swapchainFormat_);

    // ---- overlay features, on the composited backbuffer ----
    if (rhiContext_ && !features_.empty()) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = swapchainViews_[imageIndex_];
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {width_, height_}};
        ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
        const bool overlayScope = pushRenderScope(cmd, ri);
        VkViewport vp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {width_, height_}};
        api_.CmdSetViewport(cmd, 0, 1, &vp);
        api_.CmdSetScissor(cmd, 0, 1, &sc);
        for (IRenderFeature* f : features_) f->overlayPass(*rhiContext_, width_, height_);
        // THE UI LAST, over everything else the overlay drew, and INSIDE the scope this opened --
        // IUiBackend::render must not open its own (see its comment). Only reachable with a backend
        // installed, which no game build does.
        if (uiBackend_ && uiUp_) uiBackend_->render(reinterpret_cast<u64>(cmd));
        popRenderScope(cmd, overlayScope);
    }

    if (captureReq_ && captureBuf_) {
        VkImageMemoryBarrier2 toCopy = imgBarrier(swapchainImages_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT,
                                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT);
        pipelineBarrier(api_, cmd, &toCopy, 1);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width_, height_, 1};
        api_.CmdCopyImageToBuffer(cmd, swapchainImages_[imageIndex_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuf_, 1, &region);
        VkImageMemoryBarrier2 toPresent = imgBarrier(swapchainImages_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT,
                                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                     VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        pipelineBarrier(api_, cmd, &toPresent, 1);
    } else {
        VkImageMemoryBarrier2 toPresent = imgBarrier(swapchainImages_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        pipelineBarrier(api_, cmd, &toPresent, 1);
    }
    api_.EndCommandBuffer(cmd);

    const u64 signalValue = ++nextTimelineValue_;
    frameTimelineValues_[frameIndex_] = signalValue;

    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = imageAvailable_[frameIndex_];
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfos[2]{};
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore = timeline_; signalInfos[0].value = signalValue; signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = renderFinished_[imageIndex_]; signalInfos[1].value = 0; signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1; submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cbInfo;
    submit.signalSemaphoreInfoCount = 2; submit.pSignalSemaphoreInfos = signalInfos;
    vkOk(api_.QueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "QueueSubmit2");
}

void VulkanDevice::present() {
    if (!hasSwapchain_) return;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &renderFinished_[imageIndex_];
    pi.swapchainCount = 1; pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    const VkResult pr = api_.QueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        resize(width_, height_);
    } else if (pr != VK_SUCCESS) {
        AVER_ERROR("[RHI.Vulkan] vkQueuePresentKHR failed (VkResult={})", static_cast<i32>(pr));
    }

    if (captureReq_ && captureBuf_) {
        waitForGpu();
        const u32 x = capX_ < width_ ? capX_ : width_ - 1;
        const u32 y = capY_ < height_ ? capY_ : height_ - 1;
        void* mapped = nullptr;
        if (api_.MapMemory(device_, captureMemory_, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
            const u8* base = static_cast<const u8*>(mapped);
            const usize rowPitch = static_cast<usize>(width_) * 4;
            const u8* px = base + static_cast<usize>(y) * rowPitch + static_cast<usize>(x) * 4;
            captured_[0] = px[0] / 255.0f; captured_[1] = px[1] / 255.0f; captured_[2] = px[2] / 255.0f; captured_[3] = px[3] / 255.0f;
            frameImageW_ = width_; frameImageH_ = height_;
            frameImage_.resize(static_cast<usize>(width_) * height_ * 4);
            std::memcpy(frameImage_.data(), base, frameImage_.size());
            api_.UnmapMemory(device_, captureMemory_);
            captureReady_ = true;
        } else {
            AVER_ERROR("[RHI.Vulkan] capture Map FAILED");
        }
        captureReq_ = false;
    }
}

void VulkanDevice::resize(u32 w, u32 h) {
    if (!hasSwapchain_ || w == 0 || h == 0) return;
    waitForGpu();

    if (depthView_) { api_.DestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthBuffer_) { destroyImageCommitted(*this, depthBuffer_, depthMemory_); depthBuffer_ = VK_NULL_HANDLE; depthMemory_ = VK_NULL_HANDLE; }
    if (msaaColorView_) { api_.DestroyImageView(device_, msaaColorView_, nullptr); msaaColorView_ = VK_NULL_HANDLE; }
    if (msaaColor_) { destroyImageCommitted(*this, msaaColor_, msaaColorMemory_); msaaColor_ = VK_NULL_HANDLE; msaaColorMemory_ = VK_NULL_HANDLE; }
    for (VkImageView v : swapchainViews_) if (v) api_.DestroyImageView(device_, v, nullptr);
    swapchainViews_.clear(); swapchainImages_.clear();
    for (VkSemaphore s : renderFinished_) if (s) api_.DestroySemaphore(device_, s, nullptr);
    renderFinished_.clear();

    VkSurfaceCapabilitiesKHR caps{};
    api_.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
    VkExtent2D extent = (caps.currentExtent.width != 0xFFFFFFFFu) ? caps.currentExtent : VkExtent2D{w, h};
    u32 minImages = caps.minImageCount > kFrameCount ? caps.minImageCount : kFrameCount;
    if (caps.maxImageCount != 0 && minImages > caps.maxImageCount) minImages = caps.maxImageCount;

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = minImages;
    sc.imageFormat = swapchainFormat_;
    sc.imageColorSpace = swapchainColorSpace_;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = presentMode_;
    sc.clipped = VK_TRUE;
    sc.oldSwapchain = old;
    if (!vkOk(api_.CreateSwapchainKHR(device_, &sc, nullptr, &swapchain_), "resize CreateSwapchainKHR")) {
        swapchain_ = old;
        return;
    }
    if (old) api_.DestroySwapchainKHR(device_, old, nullptr);

    width_ = extent.width; height_ = extent.height;
    // Recompute the scene size for the new present size -- createDepthBuffer()/createMsaaColor()
    // below read sceneWidth_/sceneHeight_, so this must run first or they'd allocate at the STALE size.
    computeSceneSize();
    vpX_ = vpY_ = vpW_ = vpH_ = 0;   // scene-space; stale the instant sceneWidth_/sceneHeight_ moved
    for (u32 i = 0; i < kFrameCount; ++i) frameTimelineValues_[i] = nextTimelineValue_;   // already-retired: we just waited

    createRenderTargetViews();
    createDepthBuffer();
    createMsaaColor();
    releasePostTargets();

    if (captureBuf_) { destroyBufferCommitted(*this, captureBuf_, captureMemory_); captureBuf_ = VK_NULL_HANDLE; captureMemory_ = VK_NULL_HANDLE; }
    const VkDeviceSize captureBytes = VkDeviceSize(width_) * height_ * 4;
    createBufferCommitted(*this, captureBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          captureBuf_, captureMemory_, nullptr, "capture readback");

    notifyRenderTargetsChanged();
    AVER_TRACE("[RHI.Vulkan] resized to {}x{}", width_, height_);
}

// ================================================================================================
// 14. The camera post chain. Bloom pyramid, eye adaptation, tonemap/gamma composite -- same passes,
//     same formulas, same PostCB fields as D3D12Device.cpp's; see rhi::postShaderSource() for the
//     HLSL every one binds against.
//
//     DESCRIPTOR STRATEGY, and why it differs from D3D12's descriptor HEAP: D3D12 writes every
//     SRV/UAV/sampler triple ONCE at resize time and reuses the same heap slots every frame after --
//     only the DATA changes, an ordinary GPU-GPU hazard the barriers below already order. This
//     backend copies that shape rather than updating descriptors per frame: Vulkan forbids rewriting
//     a descriptor set a not-yet-completed command buffer still references (see dispatchMesh's
//     note), so writing kPostSlotCount sets ONCE per resize sidesteps that hazard entirely.
// ================================================================================================

// The post chain's register map, at FILE scope because createPostPipelines AND createPostTargets
// compile the same shaders and must map them identically -- two copies that could drift produce a
// pipeline whose shaders and layout disagree, and on AMD that doesn't error, it takes the process.
namespace {
const VkRegisterBind kPostBinds[] = {
        {'b', 0, 0, kVkSetConstants, 0},
        {'t', 0, 0, kVkSetConstants, 1},
        {'t', 1, 0, kVkSetConstants, 2},
        {'t', 2, 0, kVkSetConstants, 3},
        {'u', 0, 0, kVkSetConstants, 4},
        {'u', 1, 0, kVkSetConstants, 5},
        {'s', 0, 0, kVkSetConstants, 6},
    };
constexpr u32 kPostBindCount = static_cast<u32>(sizeof(kPostBinds) / sizeof(kPostBinds[0]));
}


bool VulkanDevice::createPostPipelines() {
    // THIS CHAIN REFUSED TO BUILD AT ALL until now, for three unrelated reasons:
    //
    //   THE SET -- a WRONG TOOL, not a real limit. rhi::postShaderSource() declares b0/t0/t1/t2/s0/
    //   u0/u1 with no register space, and DXC maps space to descriptor SET, so everything landed in
    //   set 0 while this layout wants kVkSetConstants. -fvk-u-shift only moves binding NUMBERS within
    //   one space; the fix is -fvk-bind-register, mapping each register to an explicit (set,
    //   binding) -- kPostBinds is that map.
    //
    //   THE COMBINED SAMPLERS -- the LAYOUT was wrong, not the shader: it asked for
    //   COMBINED_IMAGE_SAMPLER, but DXC always emits separate Texture2D + SamplerState, so the layout
    //   now declares SAMPLED_IMAGE plus an immutable SAMPLER at binding 6.
    //
    //   THE DESCRIPTOR POOL WAS HALF THE SIZE IT NEEDED (see pool-size array below) -- the one that
    //   actually killed the process, since AMD answers a bound null descriptor set with an access
    //   violation rather than an error.
    //
    // THE FIRST TWO WERE VERIFIED WITHOUT VALIDATION LAYERS (no LunarG SDK here, none for Windows)
    // via AVER_VK_DUMP_SPIRV: OpDecorate DescriptorSet/Binding on each module showed AverPost at
    // set 2 binding 0, gPostSceneTex at 2/1, gPostSamp at 2/6, gPostHist at 2/4 -- exactly as
    // kPostBinds asks. The compiler was doing its job; the pool was not.

    if (!postSampler_) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (!vkOk(api_.CreateSampler(device_, &si, nullptr, &postSampler_), "post sampler")) return false;
    }
    if (!postSetLayout_) {
        // SEVEN, not six: t0/t1 are SAMPLED_IMAGE and s0 is its own immutable SAMPLER at binding 6
        // (what DXC emits for Texture2D + SamplerState). bindingCount used to say six, leaving
        // binding 6 built but never handed to Vulkan -- validation caught it: "SPIR-V uses descriptor
        // [Set 2, Binding 6, variable gPostSamp] but the binding was not declared in pSetLayouts[2]".
        VkDescriptorSetLayoutBinding binds[7] = {};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[6] = {6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &postSampler_};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 7; ci.pBindings = binds;
        if (!vkOk(api_.CreateDescriptorSetLayout(device_, &ci, nullptr, &postSetLayout_), "post set layout")) return false;
    }
    if (!postPipelineLayout_) {
        if (!emptySetLayout_) emptySetLayout_ = makeEmptySetLayout(api_, device_);
        VkDescriptorSetLayout sets[kVkSetConstants + 1] = {emptySetLayout_, emptySetLayout_, postSetLayout_};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = kVkSetConstants + 1; lci.pSetLayouts = sets;
        if (!vkOk(api_.CreatePipelineLayout(device_, &lci, nullptr, &postPipelineLayout_), "post pipeline layout"))
            return false;
    }
    if (!postDescriptorPool_) {
        VkDescriptorPoolSize sizes[4] = {
            // EVERY COUNT IS PER FRAME IN FLIGHT -- the pool-size bug from this function's header
            // comment. It used to be sized for kPostSlotCount alone, half of the
            // `kFrameCount * kPostSlotCount` sets actually allocated below, so vkAllocateDescriptorSets
            // returned VK_ERROR_OUT_OF_POOL_MEMORY and the unallocated sets stayed VK_NULL_HANDLE and
            // got bound anyway -- latent (behind `#if 0`, never compiled) until AMD's access violation.
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFrameCount * kPostSlotCount},
            // SAMPLED_IMAGE and SAMPLER separately, matching the layout above -- an immutable sampler
            // still consumes a pool slot; immutable means the set can't rewrite it, not that it's free.
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFrameCount * kPostSlotCount * 2},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kFrameCount * kPostSlotCount},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrameCount * kPostSlotCount * 3},
        };
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kFrameCount * kPostSlotCount; pci.poolSizeCount = 4; pci.pPoolSizes = sizes;
        if (!vkOk(api_.CreateDescriptorPool(device_, &pci, nullptr, &postDescriptorPool_), "post descriptor pool")) return false;
    }

    const char* src = postShaderSource();
    std::vector<u32> vsSpv;
    if (!vulkanShaderCompiler().compile(src, "PostVS", ShaderStage::Vertex, 60, nullptr, vsSpv, kPostBinds, kPostBindCount)) return false;
    VkShaderModule vsMod{};
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = vsSpv.size() * sizeof(u32); mci.pCode = vsSpv.data();
    if (!vkOk(api_.CreateShaderModule(device_, &mci, nullptr, &vsMod), "post vs module")) return false;

    VkPipelineVertexInputStateCreateInfo emptyVin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;   // the post chain never runs multisampled
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    auto makeGfx = [&](const char* entry, VkFormat rtFormat, bool additive, const char* defines, VkPipeline& out) {
        std::vector<u32> psSpv;
        if (!vulkanShaderCompiler().compile(src, entry, ShaderStage::Pixel, 60, defines, psSpv, kPostBinds, kPostBindCount)) return false;
        VkShaderModule psMod{};
        VkShaderModuleCreateInfo pmci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        pmci.codeSize = psSpv.size() * sizeof(u32); pmci.pCode = psSpv.data();
        if (!vkOk(api_.CreateShaderModule(device_, &pmci, nullptr, &psMod), "post ps module")) return false;

        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsMod, "PostVS", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psMod, entry, nullptr},
        };
        VkPipelineColorBlendAttachmentState blendAtt{};
        blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (additive) {
            blendAtt.blendEnable = VK_TRUE;
            blendAtt.srcColorBlendFactor = blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAtt.dstColorBlendFactor = blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAtt.colorBlendOp = blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &blendAtt;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &rtFormat;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &rendering;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &emptyVin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &dyn;
        gp.layout = postPipelineLayout_;
        const bool ok = vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &out), "post pso");
        api_.DestroyShaderModule(device_, psMod, nullptr);
        return ok;
    };
    if (!makeGfx("PSBloomPrefilter", kVkSceneColorFormat, false, nullptr, bloomPrefilterPso_)) return false;
    if (!makeGfx("PSBloomDown", kVkSceneColorFormat, false, nullptr, bloomDownPso_)) return false;
    if (!makeGfx("PSBloomUp", kVkSceneColorFormat, true, nullptr, bloomUpPso_)) return false;
    // PSComposite's 4 permutations are NOT built here, unlike every other post PSO: their RT format
    // is the SWAPCHAIN's negotiated format, which doesn't exist yet (this runs from init(), before
    // any window). D3D12 can build its composite PSOs here because RTVFormats is a fixed compile-time
    // constant it doesn't negotiate; this backend DOES negotiate swapchainFormat_, and a Vulkan
    // pipeline via dynamic rendering is bound to an EXACT attachment format at creation, no
    // VkRenderPass-style fallback. So composite PSOs are built lazily in createPostTargets(), the
    // first point swapchainFormat_ is known.

    auto makeCompute = [&](const char* entry, VkPipeline& out) {
        std::vector<u32> csSpv;
        if (!vulkanShaderCompiler().compile(src, entry, ShaderStage::Compute, 60, nullptr, csSpv, kPostBinds, kPostBindCount)) return false;
        VkShaderModule csMod{};
        VkShaderModuleCreateInfo cmci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        cmci.codeSize = csSpv.size() * sizeof(u32); cmci.pCode = csSpv.data();
        if (!vkOk(api_.CreateShaderModule(device_, &cmci, nullptr, &csMod), "post cs module")) return false;
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, csMod, entry, nullptr};
        cp.layout = postPipelineLayout_;
        const bool ok = vkOk(api_.CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cp, nullptr, &out), "post compute pso");
        api_.DestroyShaderModule(device_, csMod, nullptr);
        return ok;
    };
    if (!makeCompute("CSHistogram", histogramPso_)) return false;
    if (!makeCompute("CSExposure", exposurePso_)) return false;
    api_.DestroyShaderModule(device_, vsMod, nullptr);

    if (!histBuf_) {
        if (!createBufferCommitted(*this, 256 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, histBuf_, histMemory_, nullptr, "post histogram")) return false;
    }
    if (!expBuf_) {
        if (!createBufferCommitted(*this, 2 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, expBuf_, expMemory_, nullptr, "post exposure")) return false;
    }
    // Both frame-in-flight rings are created EAGERLY, unlike VulkanRenderContext's own (which can be
    // lazy): this ring's buffer is what binding 0 of every persistent post-chain descriptor set
    // points at, and that binding must be valid the moment createPostTargets() finishes -- no later
    // safe point to rewrite it (sets are written ONCE and never touched again).
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (postRing_[i].buffer) continue;
        if (!createBufferCommitted(*this, kRhiRingBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                postRing_[i].buffer, postRing_[i].memory, nullptr, "post constant ring"))
            return false;
        postRing_[i].bytes = kRhiRingBytes; postRing_[i].wanted = kRhiRingBytes; postRing_[i].coherent = true;
        api_.MapMemory(device_, postRing_[i].memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&postRing_[i].mapped));
    }
    return true;
}

void VulkanDevice::releasePostTargets() {
    for (VkImageView v : bloomAttachmentViews_) if (v) api_.DestroyImageView(device_, v, nullptr);
    for (VkImageView v : bloomSampledViews_) if (v) api_.DestroyImageView(device_, v, nullptr);
    bloomAttachmentViews_.clear(); bloomSampledViews_.clear();
    if (bloomTex_) { destroyImageCommitted(*this, bloomTex_, bloomMemory_); bloomTex_ = VK_NULL_HANDLE; bloomMemory_ = VK_NULL_HANDLE; }
    if (sceneResolvedView_) { api_.DestroyImageView(device_, sceneResolvedView_, nullptr); sceneResolvedView_ = VK_NULL_HANDLE; }
    if (sceneResolved_) { destroyImageCommitted(*this, sceneResolved_, sceneResolvedMemory_); sceneResolved_ = VK_NULL_HANDLE; sceneResolvedMemory_ = VK_NULL_HANDLE; }
    // FACTORY HANDLES, so destroyTexture -- not a raw Vk* teardown. The only two targets here created
    // through VulkanResourceFactory rather than createImageCommitted directly (see AverSR's field
    // comments for why). Mirrors D3D12Device::releasePostTargets' identical teardown.
    if (rhiFactory_) {
        if (sceneColorTex_) { rhiFactory_->destroyTexture(sceneColorTex_); sceneColorTex_ = 0; }
        if (presentHdrTex_) { rhiFactory_->destroyTexture(presentHdrTex_); presentHdrTex_ = 0; }
    }
    sceneColorTexW_ = sceneColorTexH_ = presentHdrTexW_ = presentHdrTexH_ = 0;
    if (postDescriptorPool_) api_.ResetDescriptorPool(device_, postDescriptorPool_, 0);   // safe: waitForGpu() always precedes this (resize/setSampleCount)
    postSets_.clear();
    bloomMips_ = bloomW_ = bloomH_ = 0;
    postReady_ = false;
}

// Builds PSComposite's 4 permutations against swapchainFormat_ -- deferred from createPostPipelines
// (see that comment). Guarded to run once: swapchainFormat_ is fixed for the swapchain's whole life
// (only width_/height_ change across a resize), so nothing here rebuilds on later createPostTargets() calls.
bool VulkanDevice::createPostTargets() {
    if (!compositePso_[0][0] && postPipelineLayout_ && swapchainFormat_ != VK_FORMAT_UNDEFINED) {
        std::vector<u32> vsSpv;
        const char* src = postShaderSource();
        if (!vulkanShaderCompiler().compile(src, "PostVS", ShaderStage::Vertex, 60, nullptr, vsSpv, kPostBinds, kPostBindCount)) return false;
        VkShaderModule vsMod{};
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = vsSpv.size() * sizeof(u32); mci.pCode = vsSpv.data();
        if (!vkOk(api_.CreateShaderModule(device_, &mci, nullptr, &vsMod), "post vs module (composite)")) return false;

        VkPipelineVertexInputStateCreateInfo emptyVin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAtt{};
        blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &blendAtt;
        VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;
        VkFormat rtFormat = swapchainFormat_;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &rtFormat;

        bool ok = true;
        for (int bloom = 0; bloom < 2 && ok; ++bloom) {
            for (int autoExp = 0; autoExp < 2 && ok; ++autoExp) {
                std::string defs;
                if (bloom) defs += "AVER_POST_BLOOM=1;";
                if (autoExp) defs += "AVER_POST_AUTOEXPOSURE=1;";
                std::vector<u32> psSpv;
                // kPostBinds, like every other post compile. This ONE call was missing it, leaving
                // the composite pass's b0/t0/s0 in set 0 -- "variable AverPost ... not declared in
                // pSetLayouts[0]" -- while siblings built from the same source were correctly in set 2.
                ok = vulkanShaderCompiler().compile(src, "PSComposite", ShaderStage::Pixel, 60,
                                                    defs.empty() ? nullptr : defs.c_str(), psSpv,
                                                    kPostBinds, kPostBindCount);
                if (!ok) break;
                VkShaderModule psMod{};
                VkShaderModuleCreateInfo pmci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                pmci.codeSize = psSpv.size() * sizeof(u32); pmci.pCode = psSpv.data();
                if (!vkOk(api_.CreateShaderModule(device_, &pmci, nullptr, &psMod), "post ps module (composite)")) { ok = false; break; }
                VkPipelineShaderStageCreateInfo stages[2] = {
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsMod, "PostVS", nullptr},
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psMod, "PSComposite", nullptr},
                };
                VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                gp.pNext = &rendering;
                gp.stageCount = 2; gp.pStages = stages;
                gp.pVertexInputState = &emptyVin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
                gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &dyn;
                gp.layout = postPipelineLayout_;
                ok = vkOk(api_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &compositePso_[bloom][autoExp]), "composite pso");
                api_.DestroyShaderModule(device_, psMod, nullptr);
            }
        }
        api_.DestroyShaderModule(device_, vsMod, nullptr);
        if (!ok) return false;
    }

    releasePostTargets();
    if (!postPipelineLayout_ || width_ == 0 || height_ == 0) return false;

    // sceneResolved_ is sized off the SCENE extent, not the present one -- it resolves msaaColor_,
    // itself now sceneWidth_ x sceneHeight_. width_/height_==0 iff sceneWidth_/sceneHeight_==0 too
    // (computeSceneSize), so the guard just above already covers this.
    if (sampleCount_ > 1) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kVkSceneColorFormat;
        ci.extent = {sceneWidth_, sceneHeight_, 1};
        ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!createImageCommitted(*this, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sceneResolved_, sceneResolvedMemory_, "scene resolve")) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = sceneResolved_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kVkSceneColorFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!vkOk(api_.CreateImageView(device_, &vi, nullptr, &sceneResolvedView_), "scene resolve view")) return false;
    }

    // Bloom tracks the SCENE's resolution, not the present one -- samples off the (possibly
    // downscaled) scene target, same as sceneResolved_. Mirrors D3D12's identical bloomW_/bloomH_ derivation.
    bloomW_ = sceneWidth_ / 2 > 1 ? sceneWidth_ / 2 : 1;
    bloomH_ = sceneHeight_ / 2 > 1 ? sceneHeight_ / 2 : 1;
    bloomMips_ = 1;
    while (bloomMips_ < kMaxBloomMips && (bloomW_ >> bloomMips_) >= 8 && (bloomH_ >> bloomMips_) >= 8) ++bloomMips_;

    VkImageCreateInfo bd{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    bd.imageType = VK_IMAGE_TYPE_2D;
    bd.format = kVkSceneColorFormat;
    bd.extent = {bloomW_, bloomH_, 1};
    bd.mipLevels = bloomMips_; bd.arrayLayers = 1;
    bd.samples = VK_SAMPLE_COUNT_1_BIT;
    bd.tiling = VK_IMAGE_TILING_OPTIMAL;
    bd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bd.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createImageCommitted(*this, bd, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bloomTex_, bloomMemory_, "bloom pyramid")) return false;
    bloomAttachmentViews_.resize(bloomMips_);
    bloomSampledViews_.resize(bloomMips_);
    for (u32 m = 0; m < bloomMips_; ++m) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = bloomTex_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kVkSceneColorFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        api_.CreateImageView(device_, &vi, nullptr, &bloomAttachmentViews_[m]);
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        api_.CreateImageView(device_, &vi, nullptr, &bloomSampledViews_[m]);
    }

    // ---- AverSR's two intermediates, and the descriptor slot that reads the upscaled one ----
    //
    // GUARDED ON upscaler_, ALL OF IT -- mirrors D3D12Device::createPostTargets: an Off build
    // (upscaler_==nullptr, the permanent default) must not pay for two HDR textures it never samples,
    // nor write a descriptor slot pointing at texture(0) -- kPostSlotCompositeUpscaled simply stays
    // allocated-but-unwritten, safe because runPostChain only selects it after a successful copy.
    if (upscaler_ && rhiFactory_) {
        // #1: a factory-created ALIAS of the scene colour. The raw VkImage (sceneResolvedView_ or
        // msaaColorView_) was never allocated through VulkanResourceFactory, so it has no
        // TextureHandle, and IUpscaler::execute needs one for UpscalerInput::color -- a per-frame
        // vkCmdCopyImage fills this; sampled-only, nothing draws into it directly.
        TextureDesc sc;
        sc.width = sceneWidth_; sc.height = sceneHeight_;
        sc.format = fromVkFormat(kVkSceneColorFormat);
        sc.bind = ResourceBind::ShaderResource;
        sc.initialState = ResourceState::ShaderResource;
        sc.debugName = "AverSR.SceneColor";
        sceneColorTex_ = rhiFactory_->createTexture(sc);
        sceneColorTexW_ = sceneWidth_; sceneColorTexH_ = sceneHeight_;

        // #2: AverSR's output, HDR and PRESENT-sized -- the upscale runs on radiance before the
        // tonemap, so PSComposite sees an image already the right size and its own resample
        // degenerates to 1:1, letting the composite shader/pipeline stay untouched by AverSR.
        TextureDesc ph;
        ph.width = width_; ph.height = height_;
        ph.format = fromVkFormat(kVkSceneColorFormat);
        ph.bind = ResourceBind::RenderTarget | ResourceBind::ShaderResource;
        ph.initialState = ResourceState::ShaderResource;
        ph.hasClearValue = true;
        ph.debugName = "AverSR.PresentHdr";
        presentHdrTex_ = rhiFactory_->createTexture(ph);
        presentHdrTexW_ = width_; presentHdrTexH_ = height_;

        RhiTexture* srcT = sceneColorTex_ ? rhiFactory_->texture(sceneColorTex_) : nullptr;
        RhiTexture* dstT = presentHdrTex_ ? rhiFactory_->texture(presentHdrTex_) : nullptr;
        if (!srcT || !srcT->image || !dstT || !dstT->image || !dstT->rtvView) {
            AVER_WARN("[RHI.Vulkan] AverSR targets could not be created; upscaling stays off until the next resize");
            if (sceneColorTex_) { rhiFactory_->destroyTexture(sceneColorTex_); sceneColorTex_ = 0; }
            if (presentHdrTex_) { rhiFactory_->destroyTexture(presentHdrTex_); presentHdrTex_ = 0; }
        }
        // The descriptor set itself is written below, alongside every other slot -- writeSlot needs
        // dstT->srvView, which is only known once creation above has actually succeeded.
    }

    // ---- descriptors: kFrameCount * kPostSlotCount of them, written ONCE for this resize
    // generation. Doubled per frame-in-flight slot NOT because the textures/buffers differ (they
    // don't: one bloom pyramid, one histogram, one exposure scalar, shared like D3D12's single
    // postSrvHeap_), but because binding 0 in every one is a UNIFORM_BUFFER_DYNAMIC over THIS
    // FRAME'S postRing_[frameIndex_] specifically -- two different ring buffers (one per frame in
    // flight) can't share one descriptor slot 0 without rewriting it, which this scheme exists to avoid.
    VkImageView sceneView = sceneResolvedView_ ? sceneResolvedView_ : msaaColorView_;
    const u32 totalSlots = kFrameCount * kPostSlotCount;
    postSets_.assign(totalSlots, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(totalSlots, postSetLayout_);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = postDescriptorPool_;
    dai.descriptorSetCount = totalSlots; dai.pSetLayouts = layouts.data();
    if (!vkOk(api_.AllocateDescriptorSets(device_, &dai, postSets_.data()), "post descriptor sets")) return false;

    auto writeSlot = [&](u32 fi, u32 slot, VkImageView t0, VkImageView t1, bool expAtT2) {
        VkDescriptorSet set = postSets_[fi * kPostSlotCount + slot];
        VkDescriptorBufferInfo ringInfo{postRing_[fi].buffer, 0, sizeof(PostCB)};
        VkDescriptorImageInfo t0i{VK_NULL_HANDLE, t0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo t1i{VK_NULL_HANDLE, t1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo t2i{expAtT2 ? expBuf_ : histBuf_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo u0i{histBuf_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo u1i{expBuf_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[6] = {};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[0].dstSet = set; w[0].dstBinding = 0;
        w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &ringInfo;
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[1].dstSet = set; w[1].dstBinding = 1;
        w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[1].pImageInfo = &t0i;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[2].dstSet = set; w[2].dstBinding = 2;
        w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[2].pImageInfo = &t1i;
        w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[3].dstSet = set; w[3].dstBinding = 3;
        w[3].descriptorCount = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &t2i;
        w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[4].dstSet = set; w[4].dstBinding = 4;
        w[4].descriptorCount = 1; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &u0i;
        w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[5].dstSet = set; w[5].dstBinding = 5;
        w[5].descriptorCount = 1; w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo = &u1i;
        // SIX WRITES, NOT SEVEN. Binding 6 is an IMMUTABLE sampler: the layout supplies it and
        // writing to it is a validation error, not merely redundant.
        api_.UpdateDescriptorSets(device_, 6, w, 0, nullptr);
    };
    // The upscaled-composite slot's source, resolved AFTER the AverSR block so a failed creation
    // there (presentHdrTex_ left at 0) leaves this null and the loop below skips writing the slot.
    RhiTexture* presentHdrT = (upscaler_ && presentHdrTex_ && rhiFactory_) ? rhiFactory_->texture(presentHdrTex_) : nullptr;
    for (u32 fi = 0; fi < kFrameCount; ++fi) {
        writeSlot(fi, kPostSlotPrefilter, sceneView, sceneView, true);
        writeSlot(fi, kPostSlotHistogram, sceneView, sceneView, false);
        writeSlot(fi, kPostSlotComposite, sceneView, bloomSampledViews_[0], true);
        for (u32 m = 1; m < bloomMips_; ++m) {
            writeSlot(fi, kPostSlotDownBase + (m - 1), bloomSampledViews_[m - 1], bloomSampledViews_[m - 1], false);
            writeSlot(fi, kPostSlotUpBase + (m - 1), bloomSampledViews_[m], bloomSampledViews_[m], false);
        }
        // Same PSO and t1/expAtT2 as kPostSlotComposite -- only t0 differs (AverSR output vs raw
        // scene view). Left unwritten when no upscaler is set (see AverSR block comment above).
        if (presentHdrT && presentHdrT->srvView)
            writeSlot(fi, kPostSlotCompositeUpscaled, presentHdrT->srvView, bloomSampledViews_[0], true);
    }

    postReady_ = true;
    postTargetsFresh_ = true;
    return true;
}

ConstantAllocation VulkanDevice::postConstants(const void* data, u32 bytes) {
    ConstantRing& ring = postRing_[frameIndex_];
    const VkDeviceSize align = minUboAlignment_ ? minUboAlignment_ : 256;
    const VkDeviceSize offset = (ring.used + align - 1) & ~(align - 1);
    const VkDeviceSize need = offset + bytes;
    if (need > ring.bytes) {
        // Growth mid-frame would invalidate every earlier allocation's address this frame already
        // handed out -- acceptable only because kRhiRingBytes (1 MiB) vastly exceeds one frame's
        // post-chain usage (well under 2 KiB); not expected to trigger after the first frame.
        VkDeviceSize newSize = ring.wanted ? ring.wanted : kRhiRingBytes;
        while (newSize < need && newSize < kRhiRingMaxBytes) newSize *= 2;
        if (newSize < need) {
            AVER_ERROR("[RHI.Vulkan] post constant ring exhausted ({} bytes needed, {} max)", uint64_t(need), uint64_t(kRhiRingMaxBytes));
            return {};
        }
        if (ring.buffer) destroyBufferCommitted(*this, ring.buffer, ring.memory);
        if (!createBufferCommitted(*this, newSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                ring.buffer, ring.memory, nullptr, "post constant ring")) {
            ring = ConstantRing{};
            return {};
        }
        ring.bytes = newSize; ring.wanted = newSize; ring.coherent = true; ring.used = 0;
        api_.MapMemory(device_, ring.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&ring.mapped));
        // Re-point binding 0 of every post-chain descriptor set for THIS frame-in-flight slot at the
        // (moved) ring buffer -- createPostPipelines() sizes the ring at kRhiRingBytes precisely so this almost never runs.
        const u32 base = frameIndex_ * kPostSlotCount;
        for (u32 slot = 0; slot < kPostSlotCount && base + slot < postSets_.size(); ++slot) {
            VkDescriptorBufferInfo bi{ring.buffer, 0, sizeof(PostCB)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = postSets_[base + slot]; w.dstBinding = 0; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi;
            api_.UpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }
    std::memcpy(ring.mapped + offset, data, bytes);
    ring.used = offset + bytes;
    ConstantAllocation a;
    a.buffer = ring.buffer; a.offset = offset; a.cpu = ring.mapped + offset;
    return a;
}

// Scene -> swapchain image. Records the whole chain and leaves `bbImage` in COLOR_ATTACHMENT_OPTIMAL
// (endFrame's barrier takes it to PRESENT_SRC_KHR after). The scene colour source (msaaColor_, or
// sceneResolved_ when MSAA'd) is ALREADY in SHADER_READ_ONLY_OPTIMAL by the time this runs; endFrame
// barriers it there right before calling this, mirroring D3D12's resolve-then-SRV-barrier.
//
// viewportToTex_ IS honoured now, in the composite below. It used to be ignored because this
// backend had no uiTextureId to hand the image out through, so nothing could consume it. Now the
// editor asks for exactly this (SandboxApp calls setViewportToTexture(true), draws
// viewportTextureId() in its viewport panel); compositing to the swapchain regardless left that panel
// BLACK -- the UI covered the backbuffer the scene had been put on, and the image it wanted was never rendered.
void VulkanDevice::runPostChain(VkImage bbImage, VkImageView bbView, VkFormat /*bbFormat*/) {
    {
        const auto now = std::chrono::steady_clock::now();
        if (lastFrameTick_ != 0) {
            const f64 dt = std::chrono::duration<f64>(now.time_since_epoch()).count() -
                          static_cast<f64>(lastFrameTick_) / 1e9;
            frameSeconds_ = static_cast<f32>(dt < 1e-4 ? 1e-4 : (dt > 0.25 ? 0.25 : dt));
        }
        lastFrameTick_ = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    const u32 setBase = frameIndex_ * kPostSlotCount;
    auto slotSet = [&](u32 slot) { return postSets_[setBase + slot]; };

    if (!postReady_ && !createPostTargets()) {
        static bool said = false;
        if (!said) { AVER_ERROR("[RHI.Vulkan] the post chain is unavailable; the scene cannot be presented"); said = true; }
        VkImageMemoryBarrier2 toRt = imgBarrier(bbImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        pipelineBarrier(api_, cmd, &toRt, 1);
        return;
    }

    // THE ONE FRAME sceneResolved_ HAS NO LAYOUT: built lazily from inside this function, so it
    // exists only AFTER this frame's scene pass was already recorded -- the MSAA resolve that
    // normally defines its layout hasn't run yet. The composite below samples it and the layer
    // catches the read at submit: "expects ... SHADER_READ_ONLY_OPTIMAL -- instead ... UNDEFINED."
    // Once per run; from the next frame the resolve has been through it. UNDEFINED discards nothing that exists.
    if (postTargetsFresh_) {
        postTargetsFresh_ = false;
        if (sceneResolved_) {
            VkImageMemoryBarrier2 seed = imgBarrier(sceneResolved_, VK_IMAGE_ASPECT_COLOR_BIT,
                                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            pipelineBarrier(api_, cmd, &seed, 1);
        }
    }

    const bool bloom = post_.bloomIntensity > 0.0f && bloomTex_;
    const bool autoExp = post_.autoExposure && caps_.computeShaders;

    if (!expSeeded_) {
        // ZEROED WITH vkCmdFillBuffer RATHER THAN COPIED FROM THE CONSTANT RING. The copy it replaces
        // was a validation error too, twice a run: the post ring is VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
        // only, and vkCmdCopyBuffer requires TRANSFER_SRC on its source. Adding that flag would have
        // silenced the layer, but this removes the staging entirely -- 1 KB of zeros no longer gets
        // written on the CPU and uploaded to seed two buffers the GPU can clear by itself, dropping
        // the ring-allocation failure path with it.
        {
            VkBufferMemoryBarrier2 pre[2] = {
                bufBarrier(histBuf_, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT),
                bufBarrier(expBuf_, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT),
            };
            pipelineBarrier(api_, cmd, nullptr, 0, pre, 2);
            api_.CmdFillBuffer(cmd, histBuf_, 0, 256 * sizeof(u32), 0u);
            api_.CmdFillBuffer(cmd, expBuf_, 0, 2 * sizeof(u32), 0u);
            VkBufferMemoryBarrier2 post[2] = {
                bufBarrier(histBuf_, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
                bufBarrier(expBuf_, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            };
            pipelineBarrier(api_, cmd, nullptr, 0, post, 2);
            expSeeded_ = true;
        }
    }

    PostCB cb{};
    auto fillCommon = [&](u32 dstW, u32 dstH, u32 srcW, u32 srcH) {
        cb.tone[0] = post_.exposure; cb.tone[1] = post_.bloomIntensity; cb.tone[2] = post_.bloomThreshold; cb.tone[3] = post_.bloomKnee;
        cb.dst[0] = f32(dstW); cb.dst[1] = f32(dstH); cb.dst[2] = 1.0f / cb.dst[0]; cb.dst[3] = 1.0f / cb.dst[1];
        cb.src[0] = f32(srcW); cb.src[1] = f32(srcH); cb.src[2] = 1.0f / cb.src[0]; cb.src[3] = 1.0f / cb.src[1];
        cb.adapt[0] = kHistogramMinLogLum; cb.adapt[1] = 1.0f / (kHistogramMaxLogLum - kHistogramMinLogLum);
        cb.adapt[2] = 1.0f - std::exp(-post_.exposureSpeed * frameSeconds_); cb.adapt[3] = 0.0f;
        cb.limit[0] = post_.exposureMin; cb.limit[1] = post_.exposureMax;
        cb.limit[2] = post_.histogramLowPercent; cb.limit[3] = post_.histogramHighPercent;
        cb.misc[0] = post_.exposureKey; cb.misc[1] = autoExp ? 1.0f : 0.0f; cb.misc[2] = 1.0f;
        // The twin of D3D12Device.cpp's fill; see there for why both move together.
        cb.misc[3] = static_cast<f32>(post_.tonemap);
        cb.clampRadiance[0] = post_.maxRadiance;
        cb.clampRadiance[1] = cb.clampRadiance[2] = cb.clampRadiance[3] = 0.0f;
    };
    auto bindSetFor = [&](VkPipelineBindPoint bp, u32 slot) {
        const ConstantAllocation ca = postConstants(&cb, sizeof cb);
        const u32 offset = static_cast<u32>(ca.offset);
        const VkDescriptorSet set = slotSet(slot);
        api_.CmdBindDescriptorSets(cmd, bp, postPipelineLayout_, kVkSetConstants, 1, &set, 1, &offset);
    };
    auto fullscreen = [&](VkPipeline pso, u32 slot, u32 w, u32 h, VkImageView rtv) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = rtv; att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
        const bool postScope = pushRenderScope(cmd, ri);
        VkViewport vp{0.0f, 0.0f, f32(w), f32(h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {w, h}};
        api_.CmdSetViewport(cmd, 0, 1, &vp);
        api_.CmdSetScissor(cmd, 0, 1, &sc);
        api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        bindSetFor(VK_PIPELINE_BIND_POINT_GRAPHICS, slot);
        api_.CmdDraw(cmd, 3, 1, 0, 0);
        popRenderScope(cmd, postScope);
    };
    auto mipW = [&](u32 m) { return bloomW_ >> m ? bloomW_ >> m : 1u; };
    auto mipH = [&](u32 m) { return bloomH_ >> m ? bloomH_ >> m : 1u; };

    VkImageLayout mipLayout[kMaxBloomMips];
    for (u32 i = 0; i < kMaxBloomMips; ++i) mipLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    auto bloomTo = [&](u32 mip, VkImageLayout to, VkAccessFlags2 dstAccess, VkPipelineStageFlags2 dstStage) {
        if (mipLayout[mip] == to) return;
        const bool fromWrite = mipLayout[mip] == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkImageMemoryBarrier2 b = imgBarrier(bloomTex_, VK_IMAGE_ASPECT_COLOR_BIT, mipLayout[mip], to,
                                            fromWrite ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE,
                                            fromWrite ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                            dstAccess, dstStage, mip, 1);
        pipelineBarrier(api_, cmd, &b, 1);
        mipLayout[mip] = to;
    };

    // ---- eye adaptation ----
    // hw/hh (downscaled dispatch grid) and src dims derive from the SCENE target (sceneWidth_/
    // sceneHeight_, not present width_/height_) -- mirrors D3D12's identical block.
    if (autoExp) {
        const u32 hw = sceneWidth_ / kHistogramDownscale > 1 ? sceneWidth_ / kHistogramDownscale : 1;
        const u32 hh = sceneHeight_ / kHistogramDownscale > 1 ? sceneHeight_ / kHistogramDownscale : 1;
        fillCommon(hw, hh, sceneWidth_, sceneHeight_);
        api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histogramPso_);
        bindSetFor(VK_PIPELINE_BIND_POINT_COMPUTE, kPostSlotHistogram);
        api_.CmdDispatch(cmd, (hw + 15) / 16, (hh + 15) / 16, 1);

        VkBufferMemoryBarrier2 uavB = bufBarrier(histBuf_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        pipelineBarrier(api_, cmd, nullptr, 0, &uavB, 1);

        api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposurePso_);
        bindSetFor(VK_PIPELINE_BIND_POINT_COMPUTE, kPostSlotHistogram);
        api_.CmdDispatch(cmd, 1, 1, 1);
    }
    {
        VkBufferMemoryBarrier2 expToSrv = bufBarrier(expBuf_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                     VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        pipelineBarrier(api_, cmd, nullptr, 0, &expToSrv, 1);
    }

    // ---- bloom ----
    if (bloom) {
        bloomTo(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        // Prefilter's source is the scene target, not the present size -- see bloomW_/bloomH_'s own
        // derivation in createPostTargets, which already tracks sceneWidth_/sceneHeight_.
        fillCommon(mipW(0), mipH(0), sceneWidth_, sceneHeight_);
        fullscreen(bloomPrefilterPso_, kPostSlotPrefilter, mipW(0), mipH(0), bloomAttachmentViews_[0]);

        for (u32 m = 1; m < bloomMips_; ++m) {
            bloomTo(m - 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            bloomTo(m, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            fillCommon(mipW(m), mipH(m), mipW(m - 1), mipH(m - 1));
            fullscreen(bloomDownPso_, kPostSlotDownBase + (m - 1), mipW(m), mipH(m), bloomAttachmentViews_[m]);
        }
        for (u32 m = bloomMips_; m-- > 1;) {
            bloomTo(m, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            bloomTo(m - 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            fillCommon(mipW(m - 1), mipH(m - 1), mipW(m), mipH(m));
            fullscreen(bloomUpPso_, kPostSlotUpBase + (m - 1), mipW(m - 1), mipH(m - 1), bloomAttachmentViews_[m - 1]);
        }
        bloomTo(0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    }

    // ---- AverSR: the upscale (HDR, before the tonemap) ----
    //
    // ONLY WHEN AN UPSCALER IS SET, and this generation's createPostTargets built both AverSR
    // targets -- with either false, none of this runs and the composite takes its usual path. That
    // is docs/AVERSR.md's "Off must be bit-identical" invariant, expressed as one pointer + one
    // handle test, mirroring D3D12's identical guard.
    //
    // BEFORE THE TONEMAP, deliberately: the composite fuses resize + exposure + bloom + ACES + gamma
    // into one pass, so an upscaler can't simply replace it. Resampling scene RADIANCE and handing
    // the composite an already present-sized image leaves that shader/pipeline untouched -- its own
    // resample degenerates to 1:1.
    bool srUpscaled = false;
    if (upscaler_ && sceneColorTex_ && presentHdrTex_ && rhiFactory_ && rhiContext_) {
        RhiTexture* srcT = rhiFactory_->texture(sceneColorTex_);
        RhiTexture* dstT = rhiFactory_->texture(presentHdrTex_);
        if (srcT && srcT->image && dstT && dstT->image && dstT->rtvView) {
            const VkImage sceneImg = (sampleCount_ > 1) ? sceneResolved_ : msaaColor_;
            // The copy exists only because `sceneImg` has no TextureHandle (see sceneColorTex_'s
            // field comment). Both images return to SHADER_READ_ONLY_OPTIMAL right after: sceneImg
            // because every OTHER reader already expects it there (bloom prefilter, or endFrame's
            // barrier with bloom off), and srcT because that's the layout its creation
            // (transitionFreshImage, ResourceState::ShaderResource) put it in --
            // this private raw-VkImage dance hands it back exactly where it found it, never touching
            // VulkanResourceFactory's own state tracking.
            VkImageMemoryBarrier2 preCopy[2] = {
                imgBarrier(sceneImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT),
                imgBarrier(srcT->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT),
            };
            pipelineBarrier(api_, cmd, preCopy, 2);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {sceneWidth_, sceneHeight_, 1};
            api_.CmdCopyImage(cmd, sceneImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcT->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            VkImageMemoryBarrier2 postCopy[2] = {
                imgBarrier(sceneImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
                imgBarrier(srcT->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            };
            pipelineBarrier(api_, cmd, postCopy, 2);

            // execute()'s documented contract (RHIResources.hpp): the CALLER binds the target and
            // sets viewport/scissor; the implementation only records its own pipeline/draw. Vulkan's
            // dynamic rendering needs an explicit CmdBeginRendering/CmdEndRendering BRACKET around
            // that hand-off (D3D12's OMSetRenderTargets alone suffices) -- mechanical, not a divergence.
            VkImageMemoryBarrier2 toUpscaleRt = imgBarrier(dstT->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            pipelineBarrier(api_, cmd, &toUpscaleRt, 1);
            VkRenderingAttachmentInfo srAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            srAtt.imageView = dstT->rtvView; srAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            srAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; srAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo srRi{VK_STRUCTURE_TYPE_RENDERING_INFO};
            srRi.renderArea = {{0, 0}, {width_, height_}}; srRi.layerCount = 1;
            srRi.colorAttachmentCount = 1; srRi.pColorAttachments = &srAtt;
            const bool srScope = pushRenderScope(cmd, srRi);
            VkViewport srVp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
            VkRect2D srSc{{0, 0}, {width_, height_}};
            api_.CmdSetViewport(cmd, 0, 1, &srVp);
            api_.CmdSetScissor(cmd, 0, 1, &srSc);

            UpscalerInput in{};
            in.color = sceneColorTex_;
            in.srcWidth = sceneWidth_;   in.srcHeight = sceneHeight_;
            in.dstWidth = width_;        in.dstHeight = height_;
            upscaler_->execute(*rhiContext_, in, presentHdrTex_);

            popRenderScope(cmd, srScope);
            VkImageMemoryBarrier2 backToSrv = imgBarrier(dstT->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            pipelineBarrier(api_, cmd, &backToSrv, 1);

            // Unlike D3D12 (which must restore its cached descriptor heap/root signature here), nothing
            // needs restoring: every post-chain draw already rebinds its own pipeline/descriptor set
            // from scratch before drawing (fullscreen()/bindSetFor()), so nothing persists to go stale.
            srUpscaled = true;

            // ONCE, and it reports what actually happened rather than what was configured -- see
            // D3D12Device::runPostChain's identical line for why that distinction matters here.
            if (!srLogged_) {
                srLogged_ = true;
                AVER_INFO("[AverSR] '{}' upscaling {}x{} -> {}x{} in HDR, before the tonemap",
                          upscaler_->name(), sceneWidth_, sceneHeight_, width_, height_);
            }
        }
    }

    // ---- composite: straight to the swapchain image ----
    // dst is present-space (width_/height_); src is the scene target (sceneWidth_/sceneHeight_,
    // or 1:1 at default renderScale_==1.0). This IS the render-scale upscale: PSComposite already
    // bilinear-samples by normalized UV, so only the source size differs from the destination. ON
    // THE AverSR PATH src==dst already and the shader's stretch does nothing -- AverSR's filter
    // produced those pixels, not the composite's sampler.
    if (srUpscaled) fillCommon(width_, height_, width_, height_);
    else            fillCommon(width_, height_, sceneWidth_, sceneHeight_);

    // WHERE THE COMPOSITE LANDS: the swapchain image normally, or the viewport texture when the
    // editor asked for one -- only the destination changes (same pipeline/source/extent), since the
    // texture is created at width_ x height_ in the swapchain's format (see ensureViewportTexture).
    VkImage dstImage = bbImage;
    VkImageView dstView = bbView;
    RhiTexture* vpTex = nullptr;
    if (viewportToTex_ && rhiFactory_ && ensureViewportTexture()) {
        vpTex = rhiFactory_->texture(viewportTex_);
        if (vpTex && vpTex->image && vpTex->rtvView) { dstImage = vpTex->image; dstView = vpTex->rtvView; }
        else vpTex = nullptr;
    }

    // UNDEFINED as the old layout is right for both: swapchain image and viewport texture are fully
    // overwritten here -- neither's previous contents are read by this pass.
    VkImageMemoryBarrier2 toRt = imgBarrier(dstImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    pipelineBarrier(api_, cmd, &toRt, 1);
    fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0],
              srUpscaled ? kPostSlotCompositeUpscaled : kPostSlotComposite, width_, height_, dstView);

    // Back to SHADER_READ so the UI can sample it in the overlay pass that follows. Only for the
    // texture -- the swapchain image's own transition to PRESENT is endFrame's business.
    if (vpTex) {
        VkImageMemoryBarrier2 toSrv = imgBarrier(vpTex->image, VK_IMAGE_ASPECT_COLOR_BIT,
                                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        pipelineBarrier(api_, cmd, &toSrv, 1);

        // The backbuffer still has to reach COLOR_ATTACHMENT for the overlay pass, which no longer
        // happens as a side effect of compositing into it.
        VkImageMemoryBarrier2 bbToRt = imgBarrier(bbImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                  VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        pipelineBarrier(api_, cmd, &bbToRt, 1);
    }
}

// ================================================================================================
// 15. Capture and self-test.
// ================================================================================================
bool VulkanDevice::getCapture(f32 outRGBA[4]) {
    if (!captureReady_) return false;
    for (int i = 0; i < 4; ++i) outRGBA[i] = captured_[i];
    return true;
}
bool VulkanDevice::getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) {
    if (frameImage_.empty()) return false;
    outRGBA = frameImage_; w = frameImageW_; h = frameImageH_;
    return true;
}

// GPU self-test: clears a tiny offscreen target to `in`, reads the pixel back into `out`. Runs on
// its own one-shot command buffer + plain fence rather than the frame command buffer, since a
// caller may reasonably invoke this before the first frame or between frames.
bool VulkanDevice::selfTest(const f32 in[4], f32 out[4]) {
    if (!device_) return false;
    constexpr VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;   // core-mandatory support; no swapchain needed
    constexpr u32 kW = 8, kH = 8;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFmt;
    ci.extent = {kW, kH, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img{}; VkDeviceMemory imgMem{};
    if (!createImageCommitted(*this, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, imgMem, "selfTest RT")) return false;

    VkBuffer readback{}; VkDeviceMemory readbackMem{};
    const VkDeviceSize bytes = VkDeviceSize(kW) * kH * 4;
    if (!createBufferCommitted(*this, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, readbackMem, nullptr, "selfTest readback")) {
        destroyImageCommitted(*this, img, imgMem);
        return false;
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    api_.AllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api_.BeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier2 toDst = imgBarrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT);
    pipelineBarrier(api_, cmd, &toDst, 1);
    VkClearColorValue clear{}; std::memcpy(clear.float32, in, sizeof(f32) * 4);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    api_.CmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    VkImageMemoryBarrier2 toSrc = imgBarrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                             VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT);
    pipelineBarrier(api_, cmd, &toSrc, 1);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    api_.CmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
    api_.EndCommandBuffer(cmd);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    api_.CreateFence(device_, &fci, nullptr, &fence);
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &cbInfo;
    api_.QueueSubmit2(queue_, 1, &submit, fence);
    api_.WaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    api_.DestroyFence(device_, fence, nullptr);
    api_.FreeCommandBuffers(device_, commandPool_, 1, &cmd);

    void* mapped = nullptr;
    bool ok = false;
    if (api_.MapMemory(device_, readbackMem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        const u8* px = static_cast<const u8*>(mapped);
        out[0] = px[0] / 255.0f; out[1] = px[1] / 255.0f; out[2] = px[2] / 255.0f; out[3] = px[3] / 255.0f;
        api_.UnmapMemory(device_, readbackMem);
        ok = true;
    }
    destroyBufferCommitted(*this, readback, readbackMem);
    destroyImageCommitted(*this, img, imgMem);
    return ok;
}

} // namespace aver::rhi::vkb

// ================================================================================================
// 16. createVulkanDevice -- the forward declaration in modules/rhi/src/RHI.cpp names it exactly
//     this: aver::rhi::detail::createVulkanDevice(const DeviceDesc&). Mirrors D3D12's own
//     createD3D12Device (an ordinary `new` + init() + rollback-on-failure), NOT redesigned here.
// ================================================================================================
namespace aver::rhi::detail {

IDevice* createVulkanDevice(const DeviceDesc& desc) {
    auto* dev = new vkb::VulkanDevice();
    if (!dev->init(desc)) {
        delete dev;
        return nullptr;
    }
    return dev;
}

} // namespace aver::rhi::detail


// aver::rhi::vkb, NOT aver::rhi: these define members of vkb::VulkanResourceFactory/
// VulkanRenderContext, but the vkb namespace closed further up -- opening bare aver::rhi here put
// the definitions where those class names don't resolve. The backend stopped compiling the moment
// these were added, and it went unnoticed for a long stretch because AVER_RHI_VULKAN defaulted to
// OFF back then, so no default build ever reached this translation unit to fail on it.
// CMakeLists.txt:58 has since flipped the option to ON -- that is the whole point of the "port
// everything to Vulkan" push this file is part of -- so do not let a future edit reintroduce the
// belief encoded in that old parenthetical: this file DOES get built by default now, and a compile
// error here breaks every default build immediately, the way one always should have.
namespace aver::rhi::vkb {

// ---- the ray path's bindless texture table: DECLINED on this backend, for now ----
//
// Declined rather than stubbed to a silent zero. Every caller is gated on
// DeviceCaps::rtBindlessTextures, and VulkanDevice::queryCaps() (see the caps_.rtBindlessTextures
// assignment above the two AVER_INFO caps lines) deliberately hardcodes that bit to false on THIS
// backend even on hardware where descriptor indexing and inline ray query are both genuinely present
// and enabled -- precisely because this function has nothing real to back the flag up with yet.
// So arriving here with the normal gated callers (VoxiRenderer::ensureTextureTable,
// PathTracer::ensureTexturing) should be impossible, not merely unexpected: it would mean a NEW
// caller forgot to check the flag, or that caps_.rtBindlessTextures got reconnected to the hardware
// expression without this stub being replaced first. Either way this still declines cleanly rather
// than asserting, and the flat-albedo path takes over exactly as it does on a device that genuinely
// lacks the capability.

BindlessTableHandle VulkanResourceFactory::createBindlessTextureTable(u32 capacity) {
    AVER_WARN("[RHI.Vulkan] bindless texture tables are not implemented on this backend yet "
              "({} descriptors requested); ray-traced texturing stays off here", capacity);
    return 0;
}

void VulkanResourceFactory::destroyBindlessTextureTable(BindlessTableHandle) {}

bool VulkanResourceFactory::setBindlessTexture(BindlessTableHandle, u32, TextureHandle) { return false; }

u32 VulkanResourceFactory::bindlessTableCapacity(BindlessTableHandle) const { return 0; }

void VulkanRenderContext::setBindlessTable(BindlessTableHandle) {}

} // namespace aver::rhi::vkb
