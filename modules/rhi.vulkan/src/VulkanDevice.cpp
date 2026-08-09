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
#include "aver/rhi/Atmosphere.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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
constexpr u32 kPostSlotCount      = kPostSlotUpBase + (kMaxBloomMips - 1);
constexpr f32 kHistogramMinLogLum = -10.0f;
constexpr f32 kHistogramMaxLogLum = 12.0f;
constexpr u32 kHistogramDownscale = 4;

// The mesh-shader geometry registers this backend's OWN fixed pipeline chooses, mirroring
// D3D12Device.cpp's kSceneMeshSrvBase=3 (an arbitrary frozen constant, not derived from any
// PipelineLayout -- see that constant's own comment). -D'd into the AVER_MS block the same way.
constexpr u32 kMeshSrvBase = 3;

// ---- the backend's own scene/sky/line HLSL, mirroring D3D12Device.cpp's private kShaderHLSL ----
// D3D12Device.cpp's copy is anonymous-namespace text with no external linkage anywhere this module
// could reach, so this is a second, independently-owned copy of the SAME engine shading code (not
// third-party content -- it is this repository's own rendering logic), kept identical so the two
// backends render the same picture. If PSky's cloud march or PSLine's tonemap ever changes on the
// D3D12 side, this copy needs the same edit; there is no way to share the string across the two
// modules without a third one neither currently depends on, which is a bigger change than this
// pass's scope.
const char* kVulkanSceneHLSL = R"(
float4 PSMainPlain(VSOut i) : SV_TARGET { return plainShadeSurface(i, 1.0, float3(0,0,0), 1.0); }

float averHash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float averValueNoise(float3 x) {
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = averHash13(i + float3(0,0,0)), n100 = averHash13(i + float3(1,0,0));
    float n010 = averHash13(i + float3(0,1,0)), n110 = averHash13(i + float3(1,1,0));
    float n001 = averHash13(i + float3(0,0,1)), n101 = averHash13(i + float3(1,0,1));
    float n011 = averHash13(i + float3(0,1,1)), n111 = averHash13(i + float3(1,1,1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}
float averCloudSigma() {
    const float kOpticalDepthAtFull = 9.0;
    return gCloudParams.y * kOpticalDepthAtFull / max(gCloudParams.w - gCloudParams.z, 1.0);
}
float averCloudDensity(float3 wpos, bool detail) {
    float bottom = gCloudParams.z, top = gCloudParams.w;
    float h = saturate((wpos.z - bottom) / max(top - bottom, 1.0));
    float shape = saturate(h * 4.0) * saturate((1.0 - h) * 1.6);
    if (shape <= 0.001) return 0.0;
    float3 p = (wpos + float3(gCloudMotion.xy, 0.0)) * gCloudMotion.z;
    float cover = 1.0 - gCloudParams.x;
    float nLow = averValueNoise(p * 0.41) * 0.25;
    const float bestCase = nLow + (detail ? 0.9 : 0.75);
    if (bestCase <= cover) return 0.0;
    float n = averValueNoise(p) * 0.6 + nLow;
    if (detail) n += averValueNoise(p * 3.17) * 0.3;
    else        n += 0.15;
    float d = saturate((n - cover) / max(1.0 - cover, 1e-3));
    return d * shape;
}
float averHG(float ct, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * ct, 1e-4), 1.5));
}
float4 averCloudLayer(float3 ro, float3 rd, float3 sunDir, float3 sunColour, out float outDist) {
    outDist = 0.0;
    if (gCloudMotion.w < 0.5) return float4(0, 0, 0, 1);
    float bottom = gCloudParams.z, top = gCloudParams.w;
    float t0, t1;
    if (rd.z > 1e-4) {
        if (ro.z > top) return float4(0, 0, 0, 1);
        t0 = max((bottom - ro.z) / rd.z, 0.0);
        t1 = (top - ro.z) / rd.z;
    } else if (rd.z < -1e-4) {
        if (ro.z < bottom) return float4(0, 0, 0, 1);
        t0 = max((top - ro.z) / rd.z, 0.0);
        t1 = (bottom - ro.z) / rd.z;
    } else {
        if (ro.z < bottom || ro.z > top) return float4(0, 0, 0, 1);
        t0 = 0.0; t1 = (top - bottom) * 64.0;
    }
    const int kSteps = 16;
    float featureSize = 1.0 / max(gCloudMotion.z, 1e-9);
    t1 = min(t1, t0 + kSteps * featureSize * 0.35);
    if (t1 <= t0) return float4(0, 0, 0, 1);
    float dt = (t1 - t0) / kSteps;
    float jitter = averHash13(rd * 811.7);
    float t = t0 + dt * jitter;
    float sigma = averCloudSigma();
    float3 scattered = 0.0;
    float transmittance = 1.0;
    float phase = averHG(dot(rd, sunDir), 0.85);
    float distWeight = 0.0;
    float3 ambientTop = averAtmoOn() ? averSkyPhysical(float3(0, 0, 1)) : skyColorFull(float3(0, 0, 1));
    [loop] for (int i = 0; i < kSteps; ++i) {
        if (transmittance < 0.02) break;
        float3 p = ro + rd * t;
        float d = averCloudDensity(p, true);
        if (d > 0.001) {
            float lt = 0.0;
            float lstep = (top - bottom) * 0.375;
            [unroll] for (int j = 0; j < 2; ++j) {
                float3 lp = p + sunDir * (lstep * (j + 0.5));
                lt += averCloudDensity(lp, false) * lstep;
            }
            float sunT = exp(-lt * sigma);
            float hN = saturate((p.z - bottom) / max(top - bottom, 1.0));
            float3 lit = sunColour * sunT * phase * 0.6 + ambientTop * lerp(0.12, 0.55, hN * hN) * 3.0;
            float stepT = exp(-d * dt * sigma);
            float w = transmittance * (1.0 - stepT);
            scattered += w * lit;
            outDist += w * t;
            distWeight += w;
            transmittance *= stepT;
        }
        t += dt;
    }
    outDist = distWeight > 1e-6 ? outDist / distWeight : t0;
    return float4(scattered, transmittance);
}
float4 PSky(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float3 L = normalize(gLightDir.xyz);
    float3 sky = averAtmoOn() ? averSkyPhysical(ray) : skyColor(ray);
    float sd = saturate(dot(ray, L));
    float3 sunC = srgbToLin(gLightColor.rgb) * gSkyParams.z;
    float cosR = gSkyParams.w;
    float disk = smoothstep(cosR - 0.0004, cosR + 0.0002, sd);
    sky += sunC * disk * 14.0;
    if (!averAtmoOn()) sky += sunC * pow(sd, 12.0) * 0.30;
    float cloudDist;
    float4 cloud = averCloudLayer(gCamPos.xyz, ray, L, sunC, cloudDist);
    if (averAtmoOn() && cloud.a < 0.999) {
        float3 aerialT;
        float3 aerialIn = averAtmoAerial(gCamPos.xyz + ray * cloudDist, aerialT);
        cloud.rgb = cloud.rgb * aerialT + aerialIn * (1.0 - cloud.a);
    }
    sky = sky * cloud.a + cloud.rgb;
    return float4(sky, 1.0);
}
struct LVSIn  { float3 pos : POSITION; float3 col : COLOR; };
struct LVSOut { float4 pos : SV_POSITION; float3 col : COLOR; };
LVSOut VSLine(LVSIn i) {
    LVSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.pos = mul(wp, gViewProj);
    o.col = i.col;
    return o;
}
float4 PSLine(LVSOut i) : SV_TARGET { return float4(averInverseTonemap(srgbToLin(i.col)), 1.0); }
)";

// The prelude and this backend's own shaders, joined once. Vulkan-side twin of D3D12Device.cpp's
// sceneShaderSource().
const std::string& sceneShaderSource() {
    static const std::string src = std::string(sharedShaderPrelude()) + kVulkanSceneHLSL;
    return src;
}

// ---- the push-constant patch --------------------------------------------------------------------
// WHY THIS EXISTS: section 4 of VulkanCommon.hpp commits kObjectConstantRegister (b1) and the
// mesh-geometry triangle count (b5, MeshCB) to PUSH CONSTANTS -- the Vulkan analogue of D3D12's
// root 32-bit constants. DXC only emits an actual SPIR-V PushConstant block for an HLSL cbuffer
// that is itself annotated `[[vk::push_constant]]` in the SOURCE TEXT; there is no compile-time
// flag that promotes an ordinary `cbuffer X : register(bN)` to one, and the shared prelude
// (modules/rhi/src/RHIShaders.cpp, off-limits to this module) declares `PerObject`/`MeshCB` as
// perfectly ordinary cbuffers with no such annotation, because it is shared with the D3D12 backend,
// which has no such concept. So THIS FILE inserts the annotation itself, into the ASSEMBLED source
// string it alone controls, before handing that string to the shader compiler -- DXC still only
// ever compiles ordinary, well-documented `[[vk::push_constant]]` syntax; nothing here depends on
// any DXC behaviour beyond that.
//
// A SECOND, LESS OBVIOUS reason PerObject and MeshCB are merged into ONE patched block rather than
// two separately-annotated ones: SPIR-V allows at most one PushConstant-decorated interface block
// per entry point, and the mesh shader (MSMain) statically uses fields from BOTH. Two separate
// `[[vk::push_constant]]` cbuffers there would be invalid SPIR-V. Folding MeshCB's two fields onto
// the tail of PerObject's declaration -- deleting MeshCB's own block entirely -- keeps the byte
// layout exactly what PushConstantLayout already implies elsewhere in this header (object bytes at
// [0,128), the mesh count block immediately after), while staying inside SPIR-V's one-block rule.
// The vertex/index STRUCTURED BUFFERS (gVerts/gIndices) are NOT part of this patch and are NOT push
// constants here: `StructuredBuffer<T>`/`ByteAddressBuffer` always lower to a descriptor-bound
// resource in DXC's SPIR-V backend, annotation or not -- there is no HLSL spelling in the shared,
// un-annotated prelude that makes one a bare pointer. See dispatchMesh()'s own comment for how this
// backend's fixed mesh pipeline binds them instead (a freshly-allocated descriptor set per draw),
// which is the one place this backend's shape most sharply diverges from D3D12's raw root-SRV bind
// and from section 4's own "push constants carry the mesh geometry" text.
bool patchPushConstants(std::string& src, bool mesh) {
    const std::string needle = "cbuffer PerObject : register(b1) {";
    const size_t pos = src.find(needle);
    if (pos == std::string::npos) {
        AVER_ERROR("[RHI.Vulkan] shader source no longer contains the exact PerObject cbuffer text "
                   "this backend's push-constant patch matches against; the shared prelude has "
                   "drifted and this backend's fixed pipelines cannot compile correctly");
        return false;
    }
    src.replace(pos, needle.size(), "[[vk::push_constant]] cbuffer PerObject : register(b1) {");
    if (!mesh) return true;

    const std::string meshNeedle = "cbuffer MeshCB : register(b5) { uint gTriCount; uint3 _msPad; }";
    const size_t meshPos = src.find(meshNeedle);
    if (meshPos == std::string::npos) {
        AVER_ERROR("[RHI.Vulkan] shader source no longer contains the exact MeshCB cbuffer text this "
                   "backend's push-constant patch matches against");
        return false;
    }
    // Deleted outright: its two fields are re-declared on the tail of PerObject's own block below,
    // so MSMain's use of gTriCount still resolves, but from the ONE merged push-constant block.
    src.erase(meshPos, meshNeedle.size());
    const std::string objNeedle2 = "[[vk::push_constant]] cbuffer PerObject : register(b1) {";
    const size_t objPos2 = src.find(objNeedle2);
    if (objPos2 == std::string::npos) return false;   // cannot happen; just replaced it above
    src.insert(objPos2 + objNeedle2.size(), " uint gTriCount; uint3 _msPad;");
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
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = imgCount; dep.pImageMemoryBarriers = imgs;
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
// (kVkSetTable0/kVkSetTable1 on a pipeline that declares no generic tables) but this backend's own
// FIXED pipelines never populate. A VkPipelineLayout needs a real layout object at every set index
// up to the highest one it actually uses -- Vulkan has no notion of "skip this slot" -- but per the
// spec a descriptor set layout need not outlive the vkCreatePipelineLayout call that consumes it, so
// the caller creates one, builds the pipeline layout, and destroys it immediately; nothing here
// keeps it alive as a field.
VkDescriptorSetLayout makeEmptySetLayout(const VulkanApi& api, VkDevice device) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 0;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    api.CreateDescriptorSetLayout(device, &ci, nullptr, &layout);
    return layout;
}

// Writes the default shading model and its parameters into the tail of a per-draw b1 block. Byte-
// for-byte mirror of D3D12Device.cpp's own writeShadingConstants: the PerObject cbuffer's tail
// fields (gShadingModel, gReflectance, gF90, _objPad, gEmissive) are the same 8 dwords on both
// backends because both compile the SAME shared PerObject declaration.
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD
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
    AVER_VK_DEV(CmdCopyBufferToImage);
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
    if (has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
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
    // No validation layer is REQUESTED here even when the SDK happens to be installed: per the
    // vendored README, layers are a debugging aid the user opts into by installing the SDK, not a
    // build/run dependency; asking for VK_LAYER_KHRONOS_validation unconditionally would fail
    // instance creation outright on every machine that does not have it, which is every machine
    // this backend is written to run on by default.
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
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    const bool meshShaderFeaturesOk = !wantMeshShader || (meshFeat.taskShader && meshFeat.meshShader);
    const bool accelStructFeaturesOk = !wantAccelStruct || asFeat.accelerationStructure;
    const bool rayQueryFeaturesOk = !wantRayQuery || rqFeat.rayQuery;
    if (!meshShaderFeaturesOk) { meshFeat.taskShader = meshFeat.meshShader = VK_FALSE; }
    // (leave the optional feature structs zeroed/disabled when their feature bits are not actually
    // supported, even though the extension itself was present -- an extension can be exposed with
    // its feature bits false; requesting a feature the device did not report is invalid device
    // creation, so this is load-bearing, not defensive.)

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

    AVER_INFO("[RHI.Vulkan] device ready on adapter '{}'", adapterName_);
    if (rhiFactory_) rhiFactory_->selfTest();
    return true;
}

VulkanDevice::~VulkanDevice() {
    waitForGpu();
    delete rhiContext_;
    delete rhiFactory_;

    releasePostTargets();
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

    // No numbered DXR tier in Vulkan; report 11 (DXR-1.1-shaped: inline RayQuery only, no separate
    // hit-group pipeline) exactly when the inline-query extension set is present, 0 otherwise -- see
    // kOptionalDeviceExtensions' own comment on why VK_KHR_ray_tracing_pipeline is deliberately not
    // in scope. A judgment call, not a hardware-reported number; documented here for that reason.
    //
    // rtSupported_ was set in init() from extension + feature-bit presence alone, BEFORE this
    // function (and therefore before shaderModel/dxcAvailable above) had run -- RayQuery needs SM
    // 6.5 the same way this backend's mesh path does, so that gate is applied HERE, narrowing
    // rtSupported_ in place, rather than leaving caps_.rayTracingTier computed from the pre-gate
    // value. initAccelerationStructures(), called right after this function returns, narrows it
    // once more (confirming the acceleration-structure function pointers actually resolved); that
    // last step cannot fail if the extension was genuinely present, so it is not expected to move
    // this field again, but see this backend's own honestState for the residual, unverified case.
    rtSupported_ = rtSupported_ && caps_.shaderModel >= 65 && caps_.dxcAvailable;
    caps_.rayTracingTier = rtSupported_ ? 11u : 0u;

    // D3D12_RESOURCE_BINDING_TIER has no Vulkan analogue at all -- report unknown honestly, per the
    // header's own comment on this field, rather than inventing a mapping from descriptor-indexing
    // feature bits this backend does not otherwise rely on.
    caps_.resourceBindingTier = 0;

    const DeviceCaps hw = caps_;
    clampCaps(caps_);
    AVER_INFO("[RHI.Vulkan] caps: MSAA {}x, RT tier {}, SM {}, mesh-shader tier {}, DXC {}, cons-raster {}, binding tier {}",
              caps_.maxMsaaSamples, caps_.rayTracingTier, caps_.shaderModel,
              caps_.meshShaderTier, caps_.dxcAvailable, caps_.conservativeRaster, caps_.resourceBindingTier);
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
    // setSampleCount() calls this a second (third, ...) time to rebuild every PSO's baked-in
    // rasterizationSamples -- unlike D3D12's ComPtr<ID3D12PipelineState>, whose operator& releases
    // the old object automatically on reassignment, a VkPipeline handle overwritten by
    // vkCreateGraphicsPipelines without an explicit vkDestroyPipeline first is simply leaked. The
    // set/pipeline-LAYOUT objects below are deliberately NOT re-destroyed here (they do not depend
    // on sampleCount_, only the PSOs built from them do), matching the `if (!X)` guards already on
    // each of those blocks.
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
        VkDescriptorSetLayout empty0 = makeEmptySetLayout(api_, device_);   // kVkSetTable0, unused
        VkDescriptorSetLayout empty1 = makeEmptySetLayout(api_, device_);   // kVkSetTable1, unused
        VkDescriptorSetLayout sets[kVkSetConstants + 1] = {empty0, empty1, sceneFrameSetLayout_};
        VkPushConstantRange pc{VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectOffset, PushConstantLayout::kObjectBytes};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = kVkSetConstants + 1; lci.pSetLayouts = sets;
        lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
        const bool ok = vkOk(api_.CreatePipelineLayout(device_, &lci, nullptr, &scenePipelineLayout_), "scene pipeline layout");
        api_.DestroyDescriptorSetLayout(device_, empty0, nullptr);
        api_.DestroyDescriptorSetLayout(device_, empty1, nullptr);
        if (!ok) return false;
    }

    std::string src = sceneShaderSource();
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
    // EQUAL, not GREATER_EQUAL -- same reasoning as D3D12Device.cpp's own sky PSO comment: VSky pins
    // clip-space depth at exactly 1.0 (the far plane, this scene's clear value), so EQUAL is the
    // actual "nothing closer was ever written here" test; GREATER_EQUAL would be trivially true
    // everywhere and paint the sky's cost over every opaque pixel too.
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
namespace { VkDescriptorPool g_meshGeomPool[kFrameCount] = {}; VkDescriptorSetLayout g_meshGeomLayout = VK_NULL_HANDLE; }

bool VulkanDevice::initMeshShaders() {
    msSupported_ = false;
    // Same reason as createPipeline()'s identical block: setSampleCount() calls this again, and an
    // overwritten VkPipeline handle with no vkDestroyPipeline first is a leak, not a rebuild.
    if (meshPso_) { api_.DestroyPipeline(device_, meshPso_, nullptr); meshPso_ = VK_NULL_HANDLE; }
    if (caps_.meshShaderTier == 0 || caps_.shaderModel < 65 || !caps_.dxcAvailable || !api_.CmdDrawMeshTasksEXT) return false;
    // The merged push-constant range below is kObjectBytes+16 = 144 bytes, over the 128-byte floor
    // every Vulkan implementation is REQUIRED to support (VkPhysicalDeviceLimits::
    // maxPushConstantsSize's guaranteed minimum) -- unlike the plain scene pipeline's push-constant
    // range, which fits inside that floor exactly. Checked explicitly rather than left to
    // vkCreatePipelineLayout to reject, so a hardware genuinely at the floor gets the same honest
    // "falls back to the input assembler" outcome as any other missing mesh-shader prerequisite.
    if (maxPushConstantsSize_ < PushConstantLayout::kObjectBytes + 16) {
        AVER_INFO("[RHI.Vulkan] mesh-shader push-constant range needs {} bytes; this device's "
                  "maxPushConstantsSize is only {}", PushConstantLayout::kObjectBytes + 16, maxPushConstantsSize_);
        return false;
    }

    // set 0 (kVkSetTable0), bindings kMeshSrvBase/+1: gVerts / gIndices, SSBOs, FRESH per draw --
    // see dispatchMesh() for why this cannot be a push-constant buffer-device-address the way
    // section 4 of VulkanCommon.hpp describes for the generic factory path: the shared prelude's
    // StructuredBuffer<MeshVtx>/ByteAddressBuffer declarations always lower to descriptor-bound
    // resources in DXC's SPIR-V backend, with no HLSL spelling in the un-annotated prelude that
    // makes one a bare pointer instead.
    if (!g_meshGeomLayout) {
        VkDescriptorSetLayoutBinding binds[2] = {};
        binds[0].binding = kMeshSrvBase; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
        binds[1].binding = kMeshSrvBase + 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2; ci.pBindings = binds;
        if (!vkOk(api_.CreateDescriptorSetLayout(device_, &ci, nullptr, &g_meshGeomLayout), "mesh geometry set layout")) return false;
    }
    for (u32 i = 0; i < kFrameCount; ++i) {
        if (g_meshGeomPool[i]) continue;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = 128; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!vkOk(api_.CreateDescriptorPool(device_, &pci, nullptr, &g_meshGeomPool[i]), "mesh geometry pool")) return false;
    }

    if (!meshPipelineLayout_) {
        VkDescriptorSetLayout empty1 = makeEmptySetLayout(api_, device_);
        VkDescriptorSetLayout sets[kVkSetConstants + 1] = {g_meshGeomLayout, empty1, sceneFrameSetLayout_};
        // Merged push-constant range: object bytes [0,128) then the mesh count block right after --
        // see patchPushConstants for why they are ONE HLSL block, not two.
        VkPushConstantRange pc{VK_SHADER_STAGE_ALL, 0, PushConstantLayout::kObjectBytes + 16};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = kVkSetConstants + 1; lci.pSetLayouts = sets;
        lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
        const bool ok = vkOk(api_.CreatePipelineLayout(device_, &lci, nullptr, &meshPipelineLayout_), "mesh pipeline layout");
        api_.DestroyDescriptorSetLayout(device_, empty1, nullptr);
        if (!ok) return false;
    }

    std::string src = sceneShaderSource();
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
// slot's pool (reset once per beginFrame, after that slot's fence wait has already proven the GPU
// is done with whatever the pool held two frames ago -- the same double-buffering discipline every
// other kFrameCount-sized array in this class already relies on) rather than rewriting one set in
// place: a descriptor set's binding, once recorded into a command buffer, is not a snapshot -- every
// vkCmdBindDescriptorSets referencing the SAME set object sees whatever it was LAST written to by
// the time the command buffer executes, so reusing one set across several drawMesh calls in the same
// frame would make every earlier draw silently read the LAST mesh's geometry. A fresh set per draw
// costs an allocation; it does not cost correctness.
void VulkanDevice::dispatchMesh(const GpuMesh& m) {
    const u32 tris = m.indexCount / 3;
    if (!tris || !m.vb || !m.ib) return;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = g_meshGeomPool[frameIndex_];
    dai.descriptorSetCount = 1; dai.pSetLayouts = &g_meshGeomLayout;
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
    // A plain UNORM surface, not sRGB: PSComposite already applies toGamma() itself (see
    // rhi::postShaderSource()), matching D3D12's own non-sRGB DXGI_FORMAT_R8G8B8A8_UNORM backbuffer
    // -- an sRGB surface would gamma-encode a SECOND time on top of that.
    swapchainFormat_ = VK_FORMAT_UNDEFINED;
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
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
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kVkDepthFormat;
    ci.extent = {width_, height_, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = toVkSampleCount(sampleCount_);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createImageCommitted(*this, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthBuffer_, depthMemory_, "scene depth")) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = depthBuffer_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kVkDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    return vkOk(api_.CreateImageView(device_, &vi, nullptr, &depthView_), "depth view");
}

bool VulkanDevice::createMsaaColor() {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kVkSceneColorFormat;
    ci.extent = {width_, height_, 1};
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

    BufferDesc vd; vd.bytes = vbytes; vd.kind = BufferKind::Upload; vd.debugName = "mesh vertices";
    m.vbBuffer = rhiFactory_->createBuffer(vd);
    BufferDesc idd; idd.bytes = ibytes; idd.kind = BufferKind::Upload; idd.debugName = "mesh indices";
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
    rhiFactory_->writeBuffer(m.vbBuffer, verts, vbytes, 0);
    rhiFactory_->writeBuffer(m.ibBuffer, indices, ibytes, 0);

    meshes_.push_back(std::move(m));
    return static_cast<MeshHandle>(meshes_.size());
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
        // Left in Common, not UnorderedAccess: the NEXT feature to actually skin this buffer issues
        // its own Common -> UnorderedAccess barrier before writing it, exactly mirroring D3D12's own
        // "the promotion lasts for the rest of the command list" reasoning.
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
    if (rhiFactory_) rhiFactory_->destroyBlasForMesh(mesh);
    if (rhiFactory_) {
        if (m.vbBuffer) rhiFactory_->destroyBuffer(m.vbBuffer);
        if (m.ibOwned && m.ibBuffer) rhiFactory_->destroyBuffer(m.ibBuffer);
    }
    if (!m.ibOwned && m.ibSource != 0 && m.ibSource <= meshes_.size()) {
        GpuMesh& src = meshes_[m.ibSource - 1];
        if (src.ibShares > 0) src.ibShares -= 1;
    }
    m.vb = VK_NULL_HANDLE; m.ib = VK_NULL_HANDLE;
    m.vbMemory = VK_NULL_HANDLE; m.ibMemory = VK_NULL_HANDLE;
    m.vbAddress = m.ibAddress = 0;
    m.indexCount = 0; m.vertexCount = 0;
    m.vbBuffer = 0; m.ibBuffer = 0;
    m.computeWritten = false;
    m.ibSource = 0;
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

void VulkanDevice::drawLines(LineHandle mesh, const f32 world[16]) {
    if (!hasSwapchain_ || mesh == 0 || mesh > lineMeshes_.size()) return;
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;
    const GpuLineMesh& m = lineMeshes_[mesh - 1];
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    const u32 zeroOffset = 0;
    api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lineDepth_ ? linePso_ : lineOverlayPso_);
    api_.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, kVkSetConstants, 1,
                               &sceneFrameSet_[frameIndex_], 1, &zeroOffset);
    api_.CmdPushConstants(cmd, scenePipelineLayout_, VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectOffset, 64, world);
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
    vpX_ = x; vpY_ = y;
    vpW_ = (x + w > width_) ? width_ - x : w;
    vpH_ = (y + h > height_) ? height_ - y : h;
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
    if (!rect) return true;
    rect[0] = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    rect[1] = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    rect[2] = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(width_);
    rect[3] = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(height_);
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
    if (s.sunTemperatureK > 0.0f) blackbodySrgb(s.sunTemperatureK, sun);
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
}

// ================================================================================================
// 11. The central per-instance draw. Structurally identical to D3D12Device::drawMesh: every
//     feature's submitDraw first, then suppression, then any overridesScenePipeline feature's own
//     PSO/bindings for THIS draw, then the backend's own fixed pipeline as the fallback.
//
//     UNLIKE D3D12Device, there is no boundRootSig_/boundPso_ cache here: VulkanCommon.hpp declares
//     no such field on VulkanDevice, so this backend simply re-binds the pipeline and the b0
//     descriptor set on every call. Redundant, not incorrect -- a real optimisation opportunity for
//     later, not a behavioural difference a caller could observe.
// ================================================================================================
void VulkanDevice::drawMesh(MeshHandle mesh, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) {
    if (!hasSwapchain_ || mesh == 0 || mesh > meshes_.size()) return;
    if (!meshes_[mesh - 1].alive) return;
    for (IRenderFeature* f : features_)
        f->submitDraw(mesh, world, color, metallic, roughness, drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
    for (IRenderFeature* f : features_) if (f->suppressesScene()) return;

    for (IRenderFeature* f : features_) {
        if (!f->overridesScenePipeline() || !rhiContext_) continue;
        const PipelineHandle fp = f->scenePipeline(msActive_ && meshPso_, wireframe_);
        if (!fp) break;
        rhiContext_->setPipeline(fp);
        if (const BindingSetHandle bs = f->sceneBindingSet()) rhiContext_->setBindingSet(bs, 0);
        rhiContext_->setDrawBinding(drawBinding_.set, drawBinding_.constants, drawBinding_.bytes);
        const void* cb = nullptr; u32 cbBytes = 0;
        if (f->sceneConstants(&cb, &cbBytes) && cb && cbBytes) rhiContext_->setConstantBuffer(kFeatureFrameConstantRegister, cb, cbBytes);
        f32 fc[kObjectConstantDwords];
        std::memcpy(fc, world, 16 * sizeof(f32));
        std::memcpy(fc + 16, color, 4 * sizeof(f32));
        fc[20] = metallic; fc[21] = roughness; fc[22] = 0.0f; fc[23] = 0.0f;
        writeShadingConstants(fc);
        rhiContext_->setConstants(kObjectConstantRegister, fc, kObjectConstantDwords);
        if (msActive_ && meshPso_ && !wireframe_) rhiContext_->dispatchMeshFor(mesh);
        else                                      rhiContext_->drawMesh(mesh);
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
    consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
    writeShadingConstants(consts);
    api_.CmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, PushConstantLayout::kObjectOffset, sizeof(consts), consts);

    if (useMs) { dispatchMesh(m); return; }
    VkDeviceSize off = 0;
    api_.CmdBindVertexBuffers(cmd, 0, 1, &m.vb, &off);
    api_.CmdBindIndexBuffer(cmd, m.ib, 0, VK_INDEX_TYPE_UINT32);
    api_.CmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
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
    // uiTextureId is never overridden (see the header's own note on the UI methods staying at
    // IDevice's defaults), so there is no non-zero id to hand back yet even once the texture
    // exists; honest 0, matching what IDevice::uiTextureId's own default already returns.
    if (!viewportToTex_ || !ensureViewportTexture()) return 0;
    return 0;
}

void VulkanDevice::notifyRenderTargetsChanged() {
    if (sampleCount_ == notifiedSamples_ && backbufferFormat() == notifiedColor_ && depthFormat() == notifiedDepth_ &&
        width_ == notifiedWidth_ && height_ == notifiedHeight_) return;
    notifiedSamples_ = sampleCount_;
    notifiedColor_ = backbufferFormat();
    notifiedDepth_ = depthFormat();
    notifiedWidth_ = width_;
    notifiedHeight_ = height_;
    for (IRenderFeature* f : features_) f->onRenderTargetsChanged(sampleCount_, backbufferFormat(), depthFormat(), width_, height_);
}

// ================================================================================================
// 13. beginFrame / endFrame / present / resize. The frame-pacing core: see the header's own note on
//     timeline_/frameTimelineValues_/imageAvailable_/renderFinished_ for the full scheme.
// ================================================================================================
void VulkanDevice::beginFrame() {
    if (!hasSwapchain_) return;

    frameIndex_ = (frameIndex_ + 1) % kFrameCount;
    waitTimeline(frameTimelineValues_[frameIndex_]);
    if (g_meshGeomPool[frameIndex_]) api_.ResetDescriptorPool(device_, g_meshGeomPool[frameIndex_], 0);

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
    if (msaa) {
        // The MSAA resolve happens HERE, as part of ending this rendering scope -- there is no
        // separate vkCmdResolveImage call anywhere in this backend (VulkanApi carries no such
        // pointer): VkRenderingAttachmentInfo's own resolveMode/resolveImageView/resolveImageLayout
        // fields do it inline, which is simpler than D3D12's separate ResolveSubresource step, not a
        // missing feature.
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

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {width_, height_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;
    api_.CmdBeginRendering(cmd, &ri);

    const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
    const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
    const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(width_);
    const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(height_);
    VkViewport vp{rx, ry, rw, rh, 0.0f, 1.0f};
    VkRect2D sc{{static_cast<i32>(rx), static_cast<i32>(ry)}, {static_cast<u32>(rw), static_cast<u32>(rh)}};
    api_.CmdSetViewport(cmd, 0, 1, &vp);
    api_.CmdSetScissor(cmd, 0, 1, &sc);

    sceneSuppressed_ = false;
    for (IRenderFeature* f : features_) {
        if (!f->suppressesScene()) continue;
        if (rhiContext_) f->scenePass(*rhiContext_);
        sceneSuppressed_ = true;
        return;
    }
    // Unlike D3D12Device::beginFrame, nothing is bound here as a "default" pipeline: every draw
    // entry point (drawMesh/drawLines/dispatchMeshFor) binds its own pipeline before drawing, and
    // there is no VulkanDevice field to cache "the last bound one" against (see drawMesh's own
    // comment) -- there would be nothing for a default bind here to save.
}

void VulkanDevice::endFrame() {
    if (!hasSwapchain_) return;
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    // THE DEFERRED SKY DRAW -- same timing as D3D12Device::endFrame's own: after every opaque
    // drawMesh call this frame, before the rendering scope this scene lives in is closed.
    if (skyEnabled_ && !sceneSuppressed_) {
        const f32 rx = vpW_ ? static_cast<f32>(vpX_) : 0.0f;
        const f32 ry = vpW_ ? static_cast<f32>(vpY_) : 0.0f;
        const f32 rw = vpW_ ? static_cast<f32>(vpW_) : static_cast<f32>(width_);
        const f32 rh = vpW_ ? static_cast<f32>(vpH_) : static_cast<f32>(height_);
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
        api_.CmdEndRendering(cmd);
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
        api_.CmdBeginRendering(cmd, &ri);
        VkViewport vp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {width_, height_}};
        api_.CmdSetViewport(cmd, 0, 1, &vp);
        api_.CmdSetScissor(cmd, 0, 1, &sc);
        for (IRenderFeature* f : features_) f->overlayPass(*rhiContext_, width_, height_);
        api_.CmdEndRendering(cmd);
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
    vpX_ = vpY_ = vpW_ = vpH_ = 0;
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
//     same formulas, same PostCB fields as D3D12Device.cpp's own; see rhi::postShaderSource() for
//     the HLSL every one of these binds against.
//
//     DESCRIPTOR STRATEGY, and why it differs from D3D12's descriptor HEAP: D3D12Device writes every
//     SRV/UAV/sampler descriptor triple ONCE, in createPostTargets (resize time), and simply re-uses
//     the same heap slots every frame after that -- nothing about which mip a given pass reads or
//     writes ever changes between two resizes, only the DATA in those textures does, which is an
//     ordinary GPU-GPU hazard the barriers below already order. This backend copies that shape
//     exactly rather than reaching for D3D12's OWN alternative of updating descriptors every frame:
//     Vulkan forbids rewriting a descriptor set that a not-yet-completed command buffer still
//     references (see dispatchMesh's identical note), so writing kPostSlotCount descriptor sets
//     ONCE per resize and never touching them again for the rest of that resize generation is not
//     just simplest here, it is the one shape that sidesteps that hazard entirely.
// ================================================================================================
namespace { VkSampler g_postSampler = VK_NULL_HANDLE; std::vector<VkDescriptorSet> g_postSets; }

bool VulkanDevice::createPostPipelines() {
    if (!g_postSampler) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (!vkOk(api_.CreateSampler(device_, &si, nullptr, &g_postSampler), "post sampler")) return false;
    }
    if (!postSetLayout_) {
        VkDescriptorSetLayoutBinding binds[6] = {};
        binds[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &g_postSampler};
        binds[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &g_postSampler};
        binds[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
        binds[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 6; ci.pBindings = binds;
        if (!vkOk(api_.CreateDescriptorSetLayout(device_, &ci, nullptr, &postSetLayout_), "post set layout")) return false;
    }
    if (!postPipelineLayout_) {
        VkDescriptorSetLayout empty0 = makeEmptySetLayout(api_, device_);
        VkDescriptorSetLayout empty1 = makeEmptySetLayout(api_, device_);
        VkDescriptorSetLayout sets[kVkSetConstants + 1] = {empty0, empty1, postSetLayout_};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = kVkSetConstants + 1; lci.pSetLayouts = sets;
        const bool ok = vkOk(api_.CreatePipelineLayout(device_, &lci, nullptr, &postPipelineLayout_), "post pipeline layout");
        api_.DestroyDescriptorSetLayout(device_, empty0, nullptr);
        api_.DestroyDescriptorSetLayout(device_, empty1, nullptr);
        if (!ok) return false;
    }
    if (!postDescriptorPool_) {
        VkDescriptorPoolSize sizes[3] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kPostSlotCount},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPostSlotCount * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kPostSlotCount * 3},
        };
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kPostSlotCount; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (!vkOk(api_.CreateDescriptorPool(device_, &pci, nullptr, &postDescriptorPool_), "post descriptor pool")) return false;
    }

    const char* src = postShaderSource();
    std::vector<u32> vsSpv;
    if (!vulkanShaderCompiler().compile(src, "PostVS", ShaderStage::Vertex, 60, nullptr, vsSpv)) return false;
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
        if (!vulkanShaderCompiler().compile(src, entry, ShaderStage::Pixel, 60, defines, psSpv)) return false;
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
    // PSComposite's 4 permutations are NOT built here, unlike every other post PSO above: their
    // render-target format is the SWAPCHAIN's negotiated format, which does not exist yet -- this
    // function runs from VulkanDevice::init(), before any window or swapchain (mirroring
    // D3D12Device::init() calling createPostPipelines() before any swapchain exists there too).
    // D3D12 gets away with building its own composite PSOs here anyway because
    // D3D12_GRAPHICS_PIPELINE_STATE_DESC::RTVFormats is a FIXED compile-time constant
    // (kBackbufferFormat) it does not negotiate; this backend DOES negotiate swapchainFormat_ from
    // what the surface actually offers (see that field's own comment), and a Vulkan graphics
    // pipeline built via dynamic rendering is bound to an EXACT attachment format at creation time --
    // there is no "compatible enough" fallback the way a traditional VkRenderPass's format
    // compatibility rules might allow. So the composite PSOs are instead built once, lazily, in
    // createPostTargets() -- the first point in this backend's lifecycle where swapchainFormat_ is
    // actually known.

    auto makeCompute = [&](const char* entry, VkPipeline& out) {
        std::vector<u32> csSpv;
        if (!vulkanShaderCompiler().compile(src, entry, ShaderStage::Compute, 60, nullptr, csSpv)) return false;
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
    // Both frame-in-flight slots' rings are created EAGERLY, here, rather than lazily on first use
    // the way VulkanRenderContext's own ring can afford to: this ring's buffer is also what binding
    // 0 of every persistent post-chain descriptor set below points at, and that binding must already
    // be valid the moment createPostTargets() finishes -- there is no later point before the first
    // draw where it would still be safe to rewrite it (see createPostTargets' own note on why these
    // sets are written ONCE and never touched again).
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
    if (postDescriptorPool_) api_.ResetDescriptorPool(device_, postDescriptorPool_, 0);   // safe: waitForGpu() always precedes this (resize/setSampleCount)
    g_postSets.clear();
    bloomMips_ = bloomW_ = bloomH_ = 0;
    postReady_ = false;
}

// Builds PSComposite's 4 permutations against swapchainFormat_ -- deferred from
// createPostPipelines() to here; see that function's own comment on why. Guarded so it only
// actually runs once: swapchainFormat_ is fixed for the swapchain's whole life (only width_/height_
// change across a resize, per createSwapchainResources' own negotiation), so nothing here needs
// rebuilding on the resize calls that bring createPostTargets() back a second, third, ... time.
bool VulkanDevice::createPostTargets() {
    if (!compositePso_[0][0] && postPipelineLayout_ && swapchainFormat_ != VK_FORMAT_UNDEFINED) {
        std::vector<u32> vsSpv;
        const char* src = postShaderSource();
        if (!vulkanShaderCompiler().compile(src, "PostVS", ShaderStage::Vertex, 60, nullptr, vsSpv)) return false;
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
                ok = vulkanShaderCompiler().compile(src, "PSComposite", ShaderStage::Pixel, 60,
                                                    defs.empty() ? nullptr : defs.c_str(), psSpv);
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

    if (sampleCount_ > 1) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kVkSceneColorFormat;
        ci.extent = {width_, height_, 1};
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

    bloomW_ = width_ / 2 > 1 ? width_ / 2 : 1;
    bloomH_ = height_ / 2 > 1 ? height_ / 2 : 1;
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

    // ---- descriptors: kFrameCount * kPostSlotCount of them, written ONCE for this resize
    // generation. Doubled per frame-in-flight slot, NOT because the textures/buffers they read
    // differ (they do not: one bloom pyramid, one histogram, one exposure scalar, shared by both
    // slots exactly as D3D12's single postSrvHeap_ is), but because binding 0 in EVERY one of them
    // is a UNIFORM_BUFFER_DYNAMIC over THIS FRAME'S postRing_[frameIndex_] buffer specifically --
    // and unlike an ordinary dynamic-offset move within one buffer, two DIFFERENT ring buffers (one
    // per frame in flight, for the usual CPU-writes-while-GPU-still-reads-the-other reason) cannot
    // share one descriptor slot 0 without rewriting it, which is exactly what this whole scheme
    // exists to avoid.
    VkImageView sceneView = sceneResolvedView_ ? sceneResolvedView_ : msaaColorView_;
    const u32 totalSlots = kFrameCount * kPostSlotCount;
    g_postSets.assign(totalSlots, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(totalSlots, postSetLayout_);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = postDescriptorPool_;
    dai.descriptorSetCount = totalSlots; dai.pSetLayouts = layouts.data();
    if (!vkOk(api_.AllocateDescriptorSets(device_, &dai, g_postSets.data()), "post descriptor sets")) return false;

    auto writeSlot = [&](u32 fi, u32 slot, VkImageView t0, VkImageView t1, bool expAtT2) {
        VkDescriptorSet set = g_postSets[fi * kPostSlotCount + slot];
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
        w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &t0i;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[2].dstSet = set; w[2].dstBinding = 2;
        w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &t1i;
        w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[3].dstSet = set; w[3].dstBinding = 3;
        w[3].descriptorCount = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &t2i;
        w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[4].dstSet = set; w[4].dstBinding = 4;
        w[4].descriptorCount = 1; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &u0i;
        w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[5].dstSet = set; w[5].dstBinding = 5;
        w[5].descriptorCount = 1; w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo = &u1i;
        api_.UpdateDescriptorSets(device_, 6, w, 0, nullptr);
    };
    for (u32 fi = 0; fi < kFrameCount; ++fi) {
        writeSlot(fi, kPostSlotPrefilter, sceneView, sceneView, true);
        writeSlot(fi, kPostSlotHistogram, sceneView, sceneView, false);
        writeSlot(fi, kPostSlotComposite, sceneView, bloomSampledViews_[0], true);
        for (u32 m = 1; m < bloomMips_; ++m) {
            writeSlot(fi, kPostSlotDownBase + (m - 1), bloomSampledViews_[m - 1], bloomSampledViews_[m - 1], false);
            writeSlot(fi, kPostSlotUpBase + (m - 1), bloomSampledViews_[m], bloomSampledViews_[m], false);
        }
    }

    postReady_ = true;
    return true;
}

ConstantAllocation VulkanDevice::postConstants(const void* data, u32 bytes) {
    ConstantRing& ring = postRing_[frameIndex_];
    const VkDeviceSize align = minUboAlignment_ ? minUboAlignment_ : 256;
    const VkDeviceSize offset = (ring.used + align - 1) & ~(align - 1);
    const VkDeviceSize need = offset + bytes;
    if (need > ring.bytes) {
        // Growth mid-frame would invalidate every earlier allocation's address this frame already
        // handed out -- acceptable here only because kRhiRingBytes (1 MiB) is vastly larger than one
        // frame's post-chain usage (well under 2 KiB, a dozen-odd PostCB-sized allocations), so this
        // path is not expected to be taken after the very first frame. Flagged, not fully engineered
        // around, given that headroom.
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
        // Re-point binding 0 of every post-chain descriptor set belonging to THIS frame-in-flight
        // slot at the (moved) ring buffer -- createPostPipelines() sizes the ring at kRhiRingBytes
        // up front precisely so this almost never has to run; see this function's own opening note.
        const u32 base = frameIndex_ * kPostSlotCount;
        for (u32 slot = 0; slot < kPostSlotCount && base + slot < g_postSets.size(); ++slot) {
            VkDescriptorBufferInfo bi{ring.buffer, 0, sizeof(PostCB)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = g_postSets[base + slot]; w.dstBinding = 0; w.descriptorCount = 1;
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
// (endFrame's own barrier takes it to PRESENT_SRC_KHR afterward -- see that function). The scene
// colour source (msaaColor_, or sceneResolved_ when MSAA'd) is ALREADY in SHADER_READ_ONLY_OPTIMAL
// by the time this runs; endFrame barriers it there right before calling this, mirroring D3D12's own
// resolve-then-SRV-barrier immediately ahead of runPostChain.
//
// viewportToTex_ is NOT honoured here: it exists to let the editor UI draw the scene as an ordinary
// image, but this backend does not override uiTextureId (see VulkanDevice's own note on the UI
// methods), so nothing could ever consume that texture id anyway -- compositing straight to the
// swapchain image unconditionally is the honest behaviour for a backend with no UI surface, not a
// silently dropped feature.
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
    auto slotSet = [&](u32 slot) { return g_postSets[setBase + slot]; };

    if (!postReady_ && !createPostTargets()) {
        static bool said = false;
        if (!said) { AVER_ERROR("[RHI.Vulkan] the post chain is unavailable; the scene cannot be presented"); said = true; }
        VkImageMemoryBarrier2 toRt = imgBarrier(bbImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        pipelineBarrier(api_, cmd, &toRt, 1);
        return;
    }

    const bool bloom = post_.bloomIntensity > 0.0f && bloomTex_;
    const bool autoExp = post_.autoExposure && caps_.computeShaders;

    if (!expSeeded_) {
        u32 zeros[258] = {};
        const ConstantAllocation za = postConstants(zeros, sizeof(zeros));
        if (za.buffer) {
            VkBufferMemoryBarrier2 pre[2] = {
                bufBarrier(histBuf_, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT),
                bufBarrier(expBuf_, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT),
            };
            pipelineBarrier(api_, cmd, nullptr, 0, pre, 2);
            VkBufferCopy hc{za.offset, 0, 256 * sizeof(u32)};
            api_.CmdCopyBuffer(cmd, za.buffer, histBuf_, 1, &hc);
            VkBufferCopy ec{za.offset, 0, 2 * sizeof(u32)};
            api_.CmdCopyBuffer(cmd, za.buffer, expBuf_, 1, &ec);
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
        cb.misc[0] = post_.exposureKey; cb.misc[1] = autoExp ? 1.0f : 0.0f; cb.misc[2] = 1.0f; cb.misc[3] = 0.0f;
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
        api_.CmdBeginRendering(cmd, &ri);
        VkViewport vp{0.0f, 0.0f, f32(w), f32(h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {w, h}};
        api_.CmdSetViewport(cmd, 0, 1, &vp);
        api_.CmdSetScissor(cmd, 0, 1, &sc);
        api_.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        bindSetFor(VK_PIPELINE_BIND_POINT_GRAPHICS, slot);
        api_.CmdDraw(cmd, 3, 1, 0, 0);
        api_.CmdEndRendering(cmd);
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
    if (autoExp) {
        const u32 hw = width_ / kHistogramDownscale > 1 ? width_ / kHistogramDownscale : 1;
        const u32 hh = height_ / kHistogramDownscale > 1 ? height_ / kHistogramDownscale : 1;
        fillCommon(hw, hh, width_, height_);
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
        fillCommon(mipW(0), mipH(0), width_, height_);
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

    // ---- composite: straight to the swapchain image ----
    fillCommon(width_, height_, width_, height_);
    VkImageMemoryBarrier2 toRt = imgBarrier(bbImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    pipelineBarrier(api_, cmd, &toRt, 1);
    fullscreen(compositePso_[bloom ? 1 : 0][autoExp ? 1 : 0], kPostSlotComposite, width_, height_, bbView);
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
