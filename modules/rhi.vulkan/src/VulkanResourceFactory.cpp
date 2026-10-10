// VulkanResourceFactory -- Vulkan analog of D3D12ResourceFactory (modules/rhi.d3d12/src/
// D3D12Device.cpp, ~3607-4844). Owns every GPU object IResourceFactory creates -- textures, buffers,
// samplers (via the pipeline-layout cache), shaders, pipelines, binding sets, acceleration
// structures -- and the deferred-destruction queue their destroy calls feed.
//
// FILE MAP: see VulkanCommon.hpp's banner. Owns VulkanResourceFactory in full, the pipeline-layout/
// table-shape/sampler caches, nullFill/uploadInitialData, retireFence/retire/collect, and the four
// createXCommitted/destroyXCommitted free functions.
//
// STRUCTURAL PROBLEM D3D12 NEVER FACES: its root descriptor tables are type-erased ("N SRVs at t0"
// says nothing about Texture2D vs StructuredBuffer vs acceleration structure), and PipelineLayout
// mirrors that with per-table COUNTS only, never per-slot kinds. Vulkan's VkDescriptorSetLayoutBinding
// commits to one VkDescriptorType, and PipelineLayout/GraphicsPipelineDesc/ComputePipelineDesc are
// off-limits to edit (owned by modules/rhi/), so the shader is the only source of truth for a slot's kind -- why
// RhiShader keeps its compiled `spirv` words after building the VkShaderModule. See
// reflectTableSlotKinds() below, and its own comment for the descriptorLayout() smuggling trick.
#include "VulkanCommon.hpp"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstring>

namespace aver::rhi::vkb {

// ================================================================================================
// 0. Local helpers: descriptor-type mapping, SPIR-V reflection, one-shot command submission, and
//    the lazily-built null resources nullFill() writes into unset slots. All private to this TU.
// ================================================================================================
namespace {

// ---- SlotKind <-> VkDescriptorType -------------------------------------------------------------
// `rtSupported` substitutes a sampled-image descriptor for an SRV AccelerationStructure slot when
// VK_KHR_acceleration_structure is unavailable, mirroring D3D12ResourceFactory::nullFill's own
// substitution (D3D12Device.cpp ~3686-3699: "no ray tracing on this device: acceleration-structure
// slots are filled with a null 2D view"). D3D12 can do this purely at write time (root table
// slots have no fixed type); Vulkan cannot -- the binding must already say SAMPLED_IMAGE, since
// vkCreateDescriptorSetLayout for ACCELERATION_STRUCTURE_KHR is invalid without the extension, so
// the substitution has to happen here too, at LAYOUT-BUILD time.
VkDescriptorType toVkSrvDescriptorType(SlotKind kind, bool rtSupported) {
    switch (kind) {
        case SlotKind::StructuredBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SlotKind::AccelerationStructure:
            return rtSupported ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case SlotKind::Texture2D:
        case SlotKind::Texture3D:
        default: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
}
VkDescriptorType toVkUavDescriptorType(SlotKind kind) {
    switch (kind) {
        case SlotKind::StructuredBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SlotKind::AccelerationStructure:
            AVER_ERROR("[RHI.Vulkan] binding set UAV slot declares AccelerationStructure, which is SRV-only");
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case SlotKind::Texture2D:
        case SlotKind::Texture3D:
        default: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}
VkImageViewType viewTypeFor(TextureDim dim) { return dim == TextureDim::Tex3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D; }

// ---- minimal SPIR-V reflection ------------------------------------------------------------------
// UNVERIFIED against a real compile (no build was permitted this pass — see the caller's own
// honestState). Opcode/decoration/storage-class numbers below are the public, stable SPIR-V values;
// re-check with `spirv-dis` the day a build is possible, per this module's own house rule.
enum SpvOp : u32 {
    kSpvOpEntryPoint = 15,
    kSpvOpTypeImage = 25,
    kSpvOpTypeSampler = 26,
    kSpvOpTypeStruct = 30,
    kSpvOpTypePointer = 32,
    kSpvOpTypeAccelerationStructureKHR = 5341,
    kSpvOpVariable = 59,
    kSpvOpDecorate = 71,
};
enum SpvDecoration : u32 { kSpvDecBlock = 2, kSpvDecBufferBlock = 3, kSpvDecBinding = 33, kSpvDecDescriptorSet = 34 };
enum SpvStorageClass : u32 { kSpvSCUniformConstant = 0, kSpvSCUniform = 2, kSpvSCStorageBuffer = 12 };

struct SpvBinding { u32 set = ~0u, binding = ~0u; VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM; };

// Walks one compiled module's word stream and appends every resource-variable (set, binding,
// VkDescriptorType) it finds. A push-constant, stage-IO, or otherwise undecorated variable has no
// DescriptorSet/Binding pair and is silently skipped, not guessed. Bounds-checked throughout: a
// malformed or truncated module stops the scan rather than reading out of range.
void reflectSpirvBindings(const std::vector<u32>& code, std::vector<SpvBinding>& out) {
    if (code.size() < 5 || code[0] != 0x07230203u) return;
    const u32 bound = code[3];
    if (bound == 0 || bound > (1u << 22)) return;   // sanity ceiling; a real module's bound is small

    struct TypeInfo { u32 opcode = 0; u32 storageClass = ~0u; u32 pointeeType = ~0u; u32 imageSampled = ~0u; bool isBufferBlock = false; };
    std::vector<TypeInfo> types(bound);
    std::vector<u32> varStorageClass(bound, ~0u);
    std::vector<u32> varPointerType(bound, ~0u);
    std::vector<std::pair<u32, u32>> setOf, bindingOf;

    usize i = 5;
    while (i < code.size()) {
        const u32 word0 = code[i];
        const u32 wordCount = word0 >> 16;
        const u32 opcode = word0 & 0xFFFFu;
        if (wordCount == 0 || i + wordCount > code.size()) break;
        switch (opcode) {
            case kSpvOpTypeImage:
                if (wordCount > 1 && code[i + 1] < bound) {
                    types[code[i + 1]].opcode = opcode;
                    types[code[i + 1]].imageSampled = wordCount > 7 ? code[i + 7] : 0;
                }
                break;
            case kSpvOpTypeSampler:
            case kSpvOpTypeAccelerationStructureKHR:
            case kSpvOpTypeStruct:
                if (wordCount > 1 && code[i + 1] < bound) types[code[i + 1]].opcode = opcode;
                break;
            case kSpvOpTypePointer:
                if (wordCount >= 4 && code[i + 1] < bound) {
                    types[code[i + 1]].opcode = opcode;
                    types[code[i + 1]].storageClass = code[i + 2];
                    types[code[i + 1]].pointeeType = code[i + 3];
                }
                break;
            case kSpvOpVariable:
                if (wordCount >= 4 && code[i + 2] < bound) {
                    varPointerType[code[i + 2]] = code[i + 1];
                    varStorageClass[code[i + 2]] = code[i + 3];
                }
                break;
            case kSpvOpDecorate:
                if (wordCount >= 3) {
                    const u32 target = code[i + 1], decoration = code[i + 2];
                    if (decoration == kSpvDecDescriptorSet && wordCount >= 4) setOf.push_back({target, code[i + 3]});
                    else if (decoration == kSpvDecBinding && wordCount >= 4) bindingOf.push_back({target, code[i + 3]});
                    else if (decoration == kSpvDecBufferBlock && target < bound) types[target].isBufferBlock = true;
                }
                break;
            default: break;
        }
        i += wordCount;
    }

    auto findU32 = [](const std::vector<std::pair<u32, u32>>& v, u32 id) -> u32 {
        for (auto& p : v) if (p.first == id) return p.second;
        return ~0u;
    };

    for (u32 id = 0; id < bound; ++id) {
        const u32 sc = varStorageClass[id];
        if (sc == ~0u) continue;
        const u32 set = findU32(setOf, id);
        const u32 binding = findU32(bindingOf, id);
        if (set == ~0u || binding == ~0u) continue;

        const u32 ptrType = varPointerType[id];
        if (ptrType >= bound || types[ptrType].opcode != kSpvOpTypePointer) continue;
        const u32 pointee = types[ptrType].pointeeType;
        if (pointee >= bound) continue;

        VkDescriptorType dt = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (sc == kSpvSCStorageBuffer) {
            dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else if (sc == kSpvSCUniform) {
            dt = types[pointee].isBufferBlock ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else if (sc == kSpvSCUniformConstant) {
            const u32 op = types[pointee].opcode;
            if (op == kSpvOpTypeImage)
                dt = (types[pointee].imageSampled == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            else if (op == kSpvOpTypeSampler) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if (op == kSpvOpTypeAccelerationStructureKHR) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        }
        if (dt == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
        out.push_back({set, binding, dt});
    }
}

// OpEntryPoint's literal name, read back from the module itself: RhiShader keeps no entry-point
// string (spirv/stage/module only -- VulkanCommon.hpp section 7), and pName needs a live C-string;
// the module's OpEntryPoint is the one place that name is guaranteed to survive. Decoded from
// SPIR-V rather than threading ShaderDesc::entry through a field the header lacks.
std::string spirvEntryPointName(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return {};
    usize i = 5;
    while (i < code.size()) {
        const u32 word0 = code[i];
        const u32 wordCount = word0 >> 16;
        const u32 opcode = word0 & 0xFFFFu;
        if (wordCount == 0 || i + wordCount > code.size()) break;
        if (opcode == kSpvOpEntryPoint && wordCount >= 4) {
            const char* bytes = reinterpret_cast<const char*>(&code[i + 3]);
            const usize maxBytes = static_cast<usize>(wordCount - 3) * 4;
            usize len = 0;
            while (len < maxBytes && bytes[len] != '\0') ++len;
            return std::string(bytes, len);
        }
        i += wordCount;
    }
    return {};
}

SlotKind fromVkDescriptorTypeSrv(VkDescriptorType t) {
    if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) return SlotKind::StructuredBuffer;
    if (t == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) return SlotKind::AccelerationStructure;
    return SlotKind::Texture2D;   // SAMPLED_IMAGE, or anything unrecognised -- see the caller's own note
}
SlotKind fromVkDescriptorTypeUav(VkDescriptorType t) {
    return (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ? SlotKind::StructuredBuffer : SlotKind::Texture2D;
}

// Per-call reflected table shape, SMUGGLED from createGraphicsPipeline/createComputePipeline into
// descriptorLayout() around each call: descriptorLayout()'s signature is FIXED by VulkanCommon.hpp
// (`(const PipelineLayout&, bool)`, shared across 4 TUs) and carries no shader info, yet building
// VkDescriptorSetLayout for table 0/1 needs exactly that. Set immediately before each
// descriptorLayout() call and cleared after; a plain static is safe since this factory takes no
// lock and is never called concurrently with itself.
struct PendingTableKinds {
    bool active = false;
    SlotKind srv0[kMaxBindingSlots], uav0[kMaxBindingSlots], srv1[kMaxBindingSlots], uav1[kMaxBindingSlots];
    // Which slots reflection actually FOUND, as opposed to left at the Texture2D default. A slot no
    // shader in the pipeline references is indistinguishable from one that genuinely IS a Texture2D
    // unless this is recorded, and that difference is the whole slot-kind bug.
    bool srv0Seen[kMaxBindingSlots] = {};
    bool uav0Seen[kMaxBindingSlots] = {};
    bool srv1Seen[kMaxBindingSlots] = {};
    bool uav1Seen[kMaxBindingSlots] = {};
};
PendingTableKinds gPendingKinds;

// Reflects every given shader stage and fills `out` with the REAL kind each declared PipelineLayout
// slot needs, defaulting an unmentioned slot to Texture2D (logged) -- an unread register is legal
// (RHIResources.hpp: dispatchMeshClusters always reserves 3 regardless of usage).
// WHICH MODULE'S BINDINGS THIS READS IS LOAD-BEARING. The LAYOUT-AGNOSTIC module from createShader
// puts every register in set 0 at binding == register number (DXC's space-less layout), so a
// SAMPLER at s2 and an SRV at t2 collide at set 0 binding 2 -- caused a real bug (sampler
// overwrote an acceleration structure actually declared there), reported three steps later as
// "Binding 2 ... from VkPipelineLayout is type VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE but binding 2 ...
// trying to bind, is type VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR".
// The PER-LAYOUT variant (moduleForLayout) puts samplers in set 3 and table 1 in set 1, so its
// set-0 bindings mean what this function assumes. Callers must pass that variant.
void reflectTableSlotKinds(const std::vector<u32>* const* stageSpirv, u32 stageCount,
                           const PipelineLayout& layout, PendingTableKinds& out) {
    out.active = true;
    for (u32 i = 0; i < kMaxBindingSlots; ++i) {
        out.srv0[i] = out.uav0[i] = out.srv1[i] = out.uav1[i] = SlotKind::Texture2D;
        out.srv0Seen[i] = out.uav0Seen[i] = out.srv1Seen[i] = out.uav1Seen[i] = false;
    }

    std::vector<SpvBinding> bindings;
    for (u32 s = 0; s < stageCount; ++s)
        if (stageSpirv[s] && !stageSpirv[s]->empty()) reflectSpirvBindings(*stageSpirv[s], bindings);

    bool anyFound = false;
    for (const SpvBinding& b : bindings) {
        if (b.set == kVkSetTable0) {
            if (b.binding < layout.srvCount) { out.srv0[b.binding] = fromVkDescriptorTypeSrv(b.type); out.srv0Seen[b.binding] = true; anyFound = true; }
            else if (b.binding >= kVkUavBindingBase && b.binding - kVkUavBindingBase < layout.uavCount) {
                out.uav0[b.binding - kVkUavBindingBase] = fromVkDescriptorTypeUav(b.type); out.uav0Seen[b.binding - kVkUavBindingBase] = true; anyFound = true;
            }
        } else if (b.set == kVkSetTable1) {
            if (b.binding < layout.srvCount1) { out.srv1[b.binding] = fromVkDescriptorTypeSrv(b.type); out.srv1Seen[b.binding] = true; anyFound = true; }
            else if (b.binding >= kVkUavBindingBase && b.binding - kVkUavBindingBase < layout.uavCount1) {
                out.uav1[b.binding - kVkUavBindingBase] = fromVkDescriptorTypeUav(b.type); out.uav1Seen[b.binding - kVkUavBindingBase] = true; anyFound = true;
            }
        }
    }
    if (!anyFound && (layout.srvCount || layout.uavCount || layout.srvCount1 || layout.uavCount1))
        AVER_WARN("[RHI.Vulkan] reflectTableSlotKinds found no descriptor-bound variables at all for a "
                  "pipeline declaring table slots; every slot defaults to Texture2D, which is wrong for "
                  "any StructuredBuffer/AccelerationStructure register");
}

// ---- one-shot command submission ----------------------------------------------------------------
// Records `fn` on the graphics queue and blocks until it retires. D3D12's CreateCommittedResource
// can start a resource in any D3D12_RESOURCE_STATES directly; a Vulkan image is always born
// UNDEFINED, so "create a texture already in RenderTarget state" needs a real barrier submitted and
// waited on -- this generalises the pattern uploadInitialData needs too, for seeded or unseeded textures.
bool runOneShotCommands(VulkanDevice& dev, const std::function<void(VkCommandBuffer)>& fn, const char* what) {
    const VulkanApi& api = dev.api();
    VkDevice device = dev.vkDevice();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = dev.graphicsQueueFamily();
    if (!vkOk(api.CreateCommandPool(device, &pi, nullptr, &pool), what)) return false;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    bool ok = vkOk(api.AllocateCommandBuffers(device, &ai, &cmd), what);

    if (ok) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ok = vkOk(api.BeginCommandBuffer(cmd, &bi), what);
    }
    if (ok) {
        fn(cmd);
        ok = vkOk(api.EndCommandBuffer(cmd), what);
    }
    if (ok) {
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ok = vkOk(api.CreateFence(device, &fi, nullptr, &fence), what);
        if (ok) {
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            ok = vkOk(api.QueueSubmit(dev.graphicsQueue(), 1, &si, fence), what);
            if (ok) ok = vkOk(api.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), what);
            api.DestroyFence(device, fence, nullptr);
        }
    }
    api.DestroyCommandPool(device, pool, nullptr);   // frees the one command buffer too
    return ok;
}

// One-shot image layout transition, UNDEFINED -> `to`, every mip and the whole aspect. Used for a
// freshly created, unseeded texture whose TextureDesc::initialState is not Common -- see this
// namespace's runOneShotCommands comment for why Vulkan needs this where D3D12 needs nothing.
void transitionFreshImage(VulkanDevice& dev, VkImage image, VkImageAspectFlags aspect, u32 mips, ResourceState to) {
    const VkImageBarrierInfo info = toVkImageBarrierInfo(to);
    runOneShotCommands(dev, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = info.stage;
        b.dstAccessMask = info.access;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = info.layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, mips, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        dev.api().CmdPipelineBarrier2(cmd, &dep);
    }, "rhi createTexture initial-layout transition");
}

// ---- lazily-built dummy resources nullFill() writes into every unset declared slot --------------
// Tier 1 requires a valid, dimension-matched descriptor in every declared slot even when unused
// (RHIResources.hpp; mirrors D3D12ResourceFactory::nullFill, D3D12Device.cpp ~3678-3739). Core
// Vulkan has no equivalent of `CreateShaderResourceView(nullptr, &desc, handle)` without
// VK_EXT_robustness2's nullDescriptor feature (not in this backend's extension list), so every
// write needs a REAL resource: a 1x1 image (shared by Texture2D/Texture3D
// null-fills -- the descriptor TYPE must match the layout, not the view's dimensionality) and a
// 16-byte buffer, built once for the process. File-local statics, not factory members, because
// VulkanResourceFactory's class body is fixed by VulkanCommon.hpp; assumes one live factory/process.
struct NullResources {
    bool ready = false;
    VkImage image2D = VK_NULL_HANDLE, image3D = VK_NULL_HANDLE;
    VkDeviceMemory image2DMemory = VK_NULL_HANDLE, image3DMemory = VK_NULL_HANDLE;
    VkImageView view2D = VK_NULL_HANDLE, view3D = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    // A degenerate, 0-instance TLAS, built only when the device has ray tracing -- the one case a
    // dummy IMAGE can't stand in for, since an AccelerationStructure slot's layout is really
    // VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR (see toVkSrvDescriptorType), nothing else legal.
    VkAccelerationStructureKHR dummyTlas = VK_NULL_HANDLE;
    VkBuffer dummyTlasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory dummyTlasMemory = VK_NULL_HANDLE;
    VkBuffer dummyTlasScratch = VK_NULL_HANDLE;
    VkDeviceMemory dummyTlasScratchMemory = VK_NULL_HANDLE;
};
NullResources gNull;

bool ensureNullResources(VulkanDevice& dev) {
    if (gNull.ready) return true;
    const VulkanApi& api = dev.api();
    VkDevice device = dev.vkDevice();

    VkImageCreateInfo ci2{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci2.imageType = VK_IMAGE_TYPE_2D;
    ci2.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci2.extent = {1, 1, 1};
    ci2.mipLevels = 1;
    ci2.arrayLayers = 1;
    ci2.samples = VK_SAMPLE_COUNT_1_BIT;
    ci2.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ci2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createImageCommitted(dev, ci2, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gNull.image2D, gNull.image2DMemory, "AverRhiNullTexture2D"))
        return false;

    VkImageCreateInfo ci3 = ci2;
    ci3.imageType = VK_IMAGE_TYPE_3D;
    if (!createImageCommitted(dev, ci3, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gNull.image3D, gNull.image3DMemory, "AverRhiNullTexture3D"))
        return false;

    // VK_IMAGE_LAYOUT_GENERAL: this one image backs BOTH a null SRV and a null UAV write below, so
    // it must be valid for both simultaneously -- GENERAL is the one layout that always is.
    transitionFreshImage(dev, gNull.image2D, VK_IMAGE_ASPECT_COLOR_BIT, 1, ResourceState::UnorderedAccess);
    transitionFreshImage(dev, gNull.image3D, VK_IMAGE_ASPECT_COLOR_BIT, 1, ResourceState::UnorderedAccess);

    VkImageViewCreateInfo vi2{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi2.image = gNull.image2D;
    vi2.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi2.format = ci2.format;
    vi2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!vkOk(api.CreateImageView(device, &vi2, nullptr, &gNull.view2D), "rhi null texture2D view")) return false;
    VkImageViewCreateInfo vi3 = vi2;
    vi3.image = gNull.image3D;
    vi3.viewType = VK_IMAGE_VIEW_TYPE_3D;
    if (!vkOk(api.CreateImageView(device, &vi3, nullptr, &gNull.view3D), "rhi null texture3D view")) return false;

    if (!createBufferCommitted(dev, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gNull.buffer, gNull.bufferMemory, nullptr, "AverRhiNullBuffer"))
        return false;

    if (dev.cachedCaps().rayTracingTier != 0 && api.CreateAccelerationStructureKHR && api.GetAccelerationStructureBuildSizesKHR) {
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geom;
        const u32 zeroInstances = 0;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        api.GetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &zeroInstances, &sizes);
        // A degenerate build can legally report a zero-ish size on some drivers; floor it so the
        // buffer creation below is never asked for zero bytes.
        const VkDeviceSize asBytes = sizes.accelerationStructureSize ? sizes.accelerationStructureSize : 256;
        const VkDeviceSize scratchBytes = sizes.buildScratchSize ? sizes.buildScratchSize : 256;
        if (createBufferCommitted(dev, asBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gNull.dummyTlasBuffer, gNull.dummyTlasMemory, nullptr, "AverRhiNullTlasBuffer") &&
            createBufferCommitted(dev, scratchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gNull.dummyTlasScratch, gNull.dummyTlasScratchMemory, nullptr, "AverRhiNullTlasScratch")) {
            VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            aci.buffer = gNull.dummyTlasBuffer;
            aci.size = asBytes;
            aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            if (vkOk(api.CreateAccelerationStructureKHR(device, &aci, nullptr, &gNull.dummyTlas), "rhi null TLAS")) {
                VkDeviceAddress scratchAddr = 0;
                VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                dai.buffer = gNull.dummyTlasScratch;
                scratchAddr = api.GetBufferDeviceAddress(device, &dai);
                bi.dstAccelerationStructure = gNull.dummyTlas;
                bi.scratchData.deviceAddress = scratchAddr;
                VkAccelerationStructureBuildRangeInfoKHR range{0, 0, 0, 0};
                const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
                runOneShotCommands(dev, [&](VkCommandBuffer cmd) {
                    api.CmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pRange);
                }, "rhi null TLAS build");
            }
        }
    } else if (dev.cachedCaps().rayTracingTier != 0) {
        AVER_WARN("[RHI.Vulkan] ray tracing reported available but the acceleration-structure entry "
                  "points did not load; SRV AccelerationStructure slots will be left unwritten when unset");
    }

    gNull.ready = true;
    return true;
}

void destroyNullResources(VulkanDevice& dev) {
    if (!gNull.ready && !gNull.image2D && !gNull.image3D && !gNull.buffer) return;
    const VulkanApi& api = dev.api();
    VkDevice device = dev.vkDevice();
    if (gNull.dummyTlas) api.DestroyAccelerationStructureKHR(device, gNull.dummyTlas, nullptr);
    destroyBufferCommitted(dev, gNull.dummyTlasScratch, gNull.dummyTlasScratchMemory);
    destroyBufferCommitted(dev, gNull.dummyTlasBuffer, gNull.dummyTlasMemory);
    if (gNull.view2D) api.DestroyImageView(device, gNull.view2D, nullptr);
    if (gNull.view3D) api.DestroyImageView(device, gNull.view3D, nullptr);
    destroyImageCommitted(dev, gNull.image2D, gNull.image2DMemory);
    destroyImageCommitted(dev, gNull.image3D, gNull.image3DMemory);
    destroyBufferCommitted(dev, gNull.buffer, gNull.bufferMemory);
    gNull = NullResources{};
}

// ---- pipeline fixed-function state conversions ---------------------------------------------------
VkPipelineColorBlendAttachmentState toVkBlendAttachment(BlendMode m) {
    VkPipelineColorBlendAttachmentState a{};
    a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (m) {
        case BlendMode::Opaque: break;
        case BlendMode::AlphaBlend:
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.colorBlendOp = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::PremultipliedAlpha:
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Additive:
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
    }
    return a;
}

} // namespace

// ================================================================================================
// 1. pushConstantLayout() — free function, declared in VulkanCommon.hpp section 4.
// ================================================================================================
PushConstantLayout pushConstantLayout(const PipelineLayout& layout, bool mesh) {
    PushConstantLayout pc;
    // b1 (object block) is always present and first -- every module building a PipelineLayout sets
    // constantDwords[kObjectConstantRegister] = kObjectConstantDwords itself (e.g. VoxiRenderer.cpp's
    // giLayout()), so this reproduces that rather than re-deriving it from the caller.
    pc.slotOffset[kObjectConstantRegister] = PushConstantLayout::kObjectOffset;
    pc.slotBytes[kObjectConstantRegister] = PushConstantLayout::kObjectBytes;
    u32 offset = PushConstantLayout::kObjectOffset + PushConstantLayout::kObjectBytes;
    for (u32 k = 0; k < kMaxConstantSlots; ++k) {
        if (k == kObjectConstantRegister) continue;      // placed first, above
        if (layout.constantDwords[k] == 0) continue;     // a root CBV (dynamic UBO), not a push constant
        pc.slotOffset[k] = offset;
        pc.slotBytes[k] = layout.constantDwords[k] * 4;
        offset += pc.slotBytes[k];
    }
    if (mesh) {
        offset = (offset + 7u) & ~7u;   // 8-byte align the two VkDeviceAddress fields
        pc.meshVertexAddrOffset = offset; offset += static_cast<u32>(sizeof(VkDeviceAddress));
        pc.meshIndexAddrOffset = offset; offset += static_cast<u32>(sizeof(VkDeviceAddress));
        pc.meshCountOffset = offset; offset += 16;   // 4 dwords, mirrors D3D12's kMeshGeometryConstantRegister block
    }
    pc.totalBytes = offset;
    return pc;
}

// ------------------------------------------------------------------------------------------------
// 1b. pushConstantKey / patchPushConstantSlots -- declared in VulkanCommon.hpp section 4; the reason
//     RhiShader keeps its source. Both defined in terms of pushConstantLayout()'s byte offsets above.
// ------------------------------------------------------------------------------------------------
namespace {

inline bool isIdentCh(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
inline bool isSpaceCh(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool isDigitCh(char c) { return c >= '0' && c <= '9'; }

// A copy with every comment blanked to spaces, for FIELD-NAME extraction only -- the struct body is
// emitted verbatim from the ORIGINAL, so nothing here can move a byte.
std::string blankComments(const std::string& in) {
    std::string out = in;
    size_t i = 0;
    while (i + 1 < out.size()) {
        if (out[i] == '/' && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && out[i + 1] == '*') {
            while (i + 1 < out.size() && !(out[i] == '*' && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i + 1 < out.size()) { out[i] = ' '; out[i + 1] = ' '; i += 2; }
        } else {
            ++i;
        }
    }
    return out;
}

// `cbuffer <name> : register(b<reg> [, space<n>]) { ... } [;]` with ANY spacing -- deliberately a
// scanner and not a needle, because the exact-text matching this replaces is what made every new
// constant block a new crash. Walks forward from `from`; returns the register it found in `reg`.
bool findNextCbuffer(const std::string& src, size_t from, u32& reg,
                     size_t& begin, size_t& bodyBegin, size_t& bodyEnd, size_t& end) {
    auto skipWs = [&src](size_t j) { while (j < src.size() && isSpaceCh(src[j])) ++j; return j; };
    size_t pos = from;
    while ((pos = src.find("cbuffer", pos)) != std::string::npos) {
        const size_t after = pos + 7;
        const bool wholeWord = (pos == 0 || !isIdentCh(src[pos - 1])) &&
                               (after >= src.size() || !isIdentCh(src[after]));
        if (!wholeWord) { pos = after; continue; }
        size_t i = skipWs(after);
        const size_t nameBegin = i;
        while (i < src.size() && isIdentCh(src[i])) ++i;
        if (i == nameBegin) { pos = after; continue; }
        i = skipWs(i);
        if (i >= src.size() || src[i] != ':') { pos = after; continue; }
        i = skipWs(i + 1);
        if (src.compare(i, 8, "register") != 0) { pos = after; continue; }
        i = skipWs(i + 8);
        if (i >= src.size() || src[i] != '(') { pos = after; continue; }
        i = skipWs(i + 1);
        if (i >= src.size() || (src[i] != 'b' && src[i] != 'B')) { pos = after; continue; }
        ++i;
        const size_t digitsBegin = i;
        u32 n = 0;
        while (i < src.size() && isDigitCh(src[i])) { n = n * 10 + static_cast<u32>(src[i] - '0'); ++i; }
        if (i == digitsBegin) { pos = after; continue; }
        const size_t rparen = src.find(')', i);
        if (rparen == std::string::npos) { pos = after; continue; }
        const size_t brace = skipWs(rparen + 1);
        if (brace >= src.size() || src[brace] != '{') { pos = after; continue; }

        int depth = 0;
        size_t j = brace;
        for (; j < src.size(); ++j) {
            if (src[j] == '{') ++depth;
            else if (src[j] == '}' && --depth == 0) break;
        }
        if (j >= src.size()) { pos = after; continue; }   // unbalanced; not this block to blame
        reg = n;
        begin = pos;
        bodyBegin = brace + 1;
        bodyEnd = j;
        const size_t semi = skipWs(j + 1);
        end = (semi < src.size() && src[semi] == ';') ? semi + 1 : j + 1;
        return true;
    }
    return false;
}

// The one register, for the push-constant fold, which needs a specific slot rather than the walk.
bool findCbufferBlock(const std::string& src, u32 reg,
                      size_t& begin, size_t& bodyBegin, size_t& bodyEnd, size_t& end) {
    size_t from = 0;
    u32 found = 0;
    while (findNextCbuffer(src, from, found, begin, bodyBegin, bodyEnd, end)) {
        if (found == reg) return true;
        from = begin + 7;
    }
    return false;
}

// True when a [[vk::...]] attribute already sits immediately before `begin` -- patchPerFrameSet runs
// first and annotates b0, and annotating it twice is a compile error.
bool alreadyAnnotated(const std::string& src, size_t begin) {
    size_t i = begin;
    while (i > 0 && isSpaceCh(src[i - 1])) --i;
    return i >= 2 && src[i - 1] == ']' && src[i - 2] == ']';
}

struct PcField { std::string decl; std::string name; };

// Every field a cbuffer body declares, so each keeps resolving by its own name after the block
// becomes a struct member. Handles `float4 a, b;`, arrays, and leading type modifiers.
void collectFields(const std::string& body, std::vector<PcField>& out) {
    static const char* kMods[] = {"row_major", "column_major", "precise", "globallycoherent",
                                  "unorm", "snorm", "uniform", "const"};
    const std::string clean = blankComments(body);
    size_t p = 0;
    while (p < clean.size()) {
        const size_t semi = clean.find(';', p);
        if (semi == std::string::npos) break;
        const std::string stmt = clean.substr(p, semi - p);
        p = semi + 1;

        size_t q = 0;
        auto word = [&stmt](size_t& r) {
            while (r < stmt.size() && isSpaceCh(stmt[r])) ++r;
            const size_t b = r;
            while (r < stmt.size() && !isSpaceCh(stmt[r]) && stmt[r] != ',') ++r;
            return stmt.substr(b, r - b);
        };
        std::string typeText;
        std::string w = word(q);
        bool isMod = true;
        while (!w.empty() && isMod) {
            isMod = false;
            for (const char* m : kMods) if (w == m) { isMod = true; break; }
            if (isMod) { typeText += w; typeText += " "; w = word(q); }
        }
        if (w.empty()) continue;                 // a blank or comment-only statement
        typeText += w;

        // The rest is a comma-separated declarator list: `gFoo`, `gBar[4]`, ...
        const std::string rest = stmt.substr(q);
        size_t d = 0;
        while (d <= rest.size()) {
            const size_t comma = rest.find(',', d);
            const std::string decl = rest.substr(d, comma == std::string::npos ? std::string::npos : comma - d);
            d = (comma == std::string::npos) ? rest.size() + 1 : comma + 1;
            size_t a = 0;
            while (a < decl.size() && isSpaceCh(decl[a])) ++a;
            const size_t nb = a;
            while (a < decl.size() && isIdentCh(decl[a])) ++a;
            if (a == nb) continue;
            PcField f;
            f.name = decl.substr(nb, a - nb);
            f.decl = typeText + " " + decl.substr(nb);      // name + any array suffix, verbatim
            out.push_back(std::move(f));
        }
    }
}

}  // namespace

u32 buildRegisterBinds(const PipelineLayout& layout, VkRegisterBind* out, u32 maxOut, bool instanced) {
    u32 n = 0;
    auto put = [&](char type, u32 number, u32 set, u32 binding) {
        if (n >= maxOut) return;
        out[n++] = VkRegisterBind{type, number, 0, set, binding};
    };

    // Table 0: t0.. and u0.. at binding 0.. / kVkUavBindingBase.. in set kVkSetTable0.
    for (u32 i = 0; i < layout.srvCount && i < kMaxBindingSlots; ++i) put('t', i, kVkSetTable0, i);
    for (u32 i = 0; i < layout.uavCount && i < kMaxBindingSlots; ++i) put('u', i, kVkSetTable0, kVkUavBindingBase + i);

    // Table 1: HLSL numbers it CONTINUOUSLY above table 0 (t(srvCount).., u(uavCount)..) because
    // that is what D3D12's root signature wants, but it gets its own set here, restarted at 0.
    for (u32 i = 0; i < layout.srvCount1 && i < kMaxBindingSlots; ++i) put('t', layout.srvCount + i, kVkSetTable1, i);
    for (u32 i = 0; i < layout.uavCount1 && i < kMaxBindingSlots; ++i) put('u', layout.uavCount + i, kVkSetTable1, kVkUavBindingBase + i);

    // Samplers: their own set, immutable, binding == register. descriptorLayout() builds exactly
    // this, capped at 4 by PipelineLayout::samplers.
    const u32 samplers = layout.samplerCount < 4 ? layout.samplerCount : 4;
    for (u32 i = 0; i < samplers; ++i) put('s', i, kVkSetSamplers, i);

    // DESCRIPTOR constant buffers: THE MAP HAS TO BE COMPLETE. Once any -fvk-bind-register is
    // supplied, DXC requires one for EVERY resource the shader declares ("missing -fvk-bind-register
    // for resource"), which is also why patchCbuffersForLayout's [[vk::binding]] half is switched off
    // whenever this map is in use -- two answers to the same question otherwise.
    // Slots with NON-ZERO constantDwords are absent: folded into [[vk::push_constant]] with their
    // `register(bN)` deleted, so there's no resource left for DXC to map.
    for (u32 k = 0; k < kMaxConstantSlots; ++k)
        if (layout.constantDwords[k] == 0) put('b', k, kVkSetConstants, k);

    // gInstanceWorlds, instanced pipelines only. Register is NOT constant: HLSL takes it from
    // AVER_INSTANCE_SRV (VoxiRenderer.cpp: rhi::declaredSrvCount(layout), one past the last SRV
    // across both tables), so it must be derived the same way here or the two disagree silently.
    // Own set (kVkSetInstances): that number can equal kVkUavBindingBase and collide with set 0's UAVs.
    if (instanced) put('t', declaredSrvCount(layout), kVkSetInstances, 0);

    return n;
}

u32 shaderVariantKey(const PipelineLayout& layout, bool mesh, bool instanced) {
    // EVERY input the per-layout compile depends on must be in here, or two layouts differing only
    // in an omitted field silently share one module (buildRegisterBinds made SRV/UAV/sampler counts
    // matter, not just the constant-slot mask). kMaxBindingSlots (25, raised from 16 in
    // optimisation-wave-2) must fit the 5-bit field each count gets below (covers 0..31) --
    // raising it past 31 would corrupt every field after.
    static_assert(kMaxBindingSlots <= 31, "shaderVariantKey packs each *Count into a 5-bit field below");
    u32 key = 0;
    key |= 1u << kObjectConstantRegister;      // pushConstantLayout always places b1, declared or not
    for (u32 k = 0; k < kMaxConstantSlots; ++k)
        if (layout.constantDwords[k]) key |= 1u << k;
    // samplerCount is capped at 4, so needs only 3 bits.
    key |= (layout.srvCount  & 31u) << 5;
    key |= (layout.uavCount  & 31u) << 10;
    key |= (layout.srvCount1 & 31u) << 15;
    key |= (layout.uavCount1 & 31u) << 20;
    key |= (layout.samplerCount & 7u) << 25;
    if (mesh) key |= 1u << 31;
    // Bit 30 (28..30 were free): an instanced pipeline compiles a DIFFERENT module from the same
    // source/layout (one extra -fvk-bind-register); without this the two would alias to one cached
    // variant and whichever compiled first would serve both.
    if (instanced) key |= 1u << 30;
    return key;
}

bool patchCbuffersForLayout(std::string& src, const PipelineLayout& layout, bool mesh,
                            bool annotateDescriptors) {
    // BEFORE ANY SCANNING: findCbufferBlock/findNextCbuffer read the register number from text, but
    // the prelude now writes it as a macro -- without this, b1 never folds into push constants and
    // the pipeline names a descriptor its layout doesn't declare. See expandCbufferRegisters.
    expandCbufferRegisters(src);
    const PushConstantLayout pc = pushConstantLayout(layout, mesh);

    // The slots pushConstantLayout gives bytes to, in the order it gives them: b1 first, then
    // ascending. Walking them in THIS order is what makes the padding below correct.
    u32 order[kMaxConstantSlots];
    u32 n = 0;
    order[n++] = kObjectConstantRegister;
    for (u32 k = 0; k < kMaxConstantSlots; ++k)
        if (k != kObjectConstantRegister && layout.constantDwords[k]) order[n++] = k;

    // SCAN A COMMENT-BLANKED COPY, NOT THE SOURCE: a comment merely MENTIONING a cbuffer (shaders here
    // are heavily commented, some quoting the declarations this function rewrites) would otherwise
    // match as real and get an annotation inserted into prose -- DXC then reports "an attribute list
    // cannot appear here" on a line with no declaration.
    // blankComments overwrites each comment char with a space (same LENGTH as source), so offsets index both.
    std::string scan = blankComments(src);

    struct Block { u32 slot; size_t begin, end; std::string body; };
    std::vector<Block> blocks;
    for (u32 i = 0; i < n; ++i) {
        size_t b = 0, bb = 0, be = 0, e = 0;
        if (!findCbufferBlock(scan, order[i], b, bb, be, e)) continue;
        blocks.push_back({order[i], b, e, src.substr(bb, be - bb)});
    }

    // ---- the DESCRIPTOR half: every cbuffer the layout does NOT make root constants -------------
    // Skipped when the caller supplies a -fvk-bind-register map (it places these itself).
    // descriptorLayout() declares a dynamic UBO at binding == register in kVkSetConstants for each
    // such slot, so the shader must name it there or DXC leaves it space-less in set 0, undeclared.
    if (annotateDescriptors) {
        struct Ann { size_t at; u32 reg; };
        std::vector<Ann> anns;
        size_t from = 0;
        u32 reg = 0;
        size_t b = 0, bb = 0, be = 0, e = 0;
        while (findNextCbuffer(scan, from, reg, b, bb, be, e)) {
            from = b + 7;
            if (reg >= kMaxConstantSlots) continue;          // outside the layout's slots entirely
            // ANY slot the push-constant half claims -- NOT the same as "constantDwords is non-zero":
            // pushConstantLayout places kObjectConstantRegister unconditionally. A layout leaving
            // constantDwords[1] at zero while its shader includes the prelude's PerObject block has
            // b1 folded AND (without this check) annotated, landing on the generated struct as
            // `[[vk::binding(1, 2)]] struct AverPcBlock {` -- DXC rejects that attribute placement.
            bool folded = false;
            for (u32 oi = 0; oi < n; ++oi) if (order[oi] == reg) { folded = true; break; }
            if (folded) continue;
            if (alreadyAnnotated(src, b)) continue;          // patchPerFrameSet got here first
            anns.push_back({b, reg});
        }
        // Back to front, so each earlier offset survives the insertion after it.
        for (size_t i = anns.size(); i-- > 0;)
            src.insert(anns[i].at, "[[vk::binding(" + std::to_string(anns[i].reg) + ", " +
                                       std::to_string(kVkSetConstants) + ")]] ");
        // Every offset in `blocks` above a shifted point is now stale, so re-blank and redo the search.
        if (!anns.empty()) {
            scan = blankComments(src);
            for (Block& blk : blocks) {
                size_t nb = 0, nbb = 0, nbe = 0, ne = 0;
                if (findCbufferBlock(scan, blk.slot, nb, nbb, nbe, ne)) { blk.begin = nb; blk.end = ne; }
            }
        }
    }

    if (blocks.empty()) return true;   // uses none of its layout's root-constant slots

    std::string fields;
    std::vector<PcField> aliases;
    u32 cursor = 0;
    for (const Block& blk : blocks) {
        const u32 off = pc.slotOffset[blk.slot];
        if (off < cursor) {
            AVER_ERROR("[RHI.Vulkan] push-constant slot b{} overlaps the previous block (byte {} < {})",
                       blk.slot, off, cursor);
            return false;
        }
        if (off > cursor) {
            const u32 gap = off - cursor;
            if (gap % 16 != 0) {
                AVER_ERROR("[RHI.Vulkan] push-constant slot b{} starts at byte {}, leaving a {}-byte gap "
                           "that is not a whole number of 16-byte rows -- HLSL cannot express it as "
                           "padding, so this layout cannot be lowered to a push-constant block",
                           blk.slot, off, gap);
                return false;
            }
            // NOT `uint _pad[N]`: an array element in a constant block occupies a full 16-byte row,
            // so a uint array would be four times too long. uint4 makes the packing explicit.
            fields += "    uint4 _averPcPad" + std::to_string(blk.slot) + "[" + std::to_string(gap / 16) + "];\n";
        }
        fields += blk.body;
        if (fields.empty() || fields.back() != '\n') fields += "\n";
        collectFields(blk.body, aliases);
        cursor = off + pc.slotBytes[blk.slot];
    }
    if (aliases.empty()) {
        AVER_ERROR("[RHI.Vulkan] a root-constant cbuffer was found but no field could be read out of it");
        return false;
    }

    std::string repl = "struct AverPcBlock {\n" + fields + "};\n";
    repl += "[[vk::push_constant]] ConstantBuffer<AverPcBlock> gAverPc;\n";
    // One alias per field, AFTER the struct, so the shader body's own gWorld/gVertexCount/... keep
    // resolving unchanged and nothing above this point is affected.
    for (const PcField& f : aliases) repl += "static " + f.decl + " = gAverPc." + f.name + ";\n";

    // Erase the originals from the BACK of the source forward, so each earlier span stays valid.
    std::vector<const Block*> byPos;
    for (const Block& blk : blocks) byPos.push_back(&blk);
    std::sort(byPos.begin(), byPos.end(), [](const Block* a, const Block* b) { return a->begin > b->begin; });
    const size_t insertAt = byPos.back()->begin;
    for (const Block* blk : byPos) src.erase(blk->begin, blk->end - blk->begin);
    src.insert(insertAt, repl);
    return true;
}

// ================================================================================================
// 2. createBufferCommitted / createImageCommitted / destroyBufferCommitted / destroyImageCommitted
//    — declared in VulkanCommon.hpp section 9.
// ================================================================================================
bool createBufferCommitted(VulkanDevice& dev, VkDeviceSize bytes, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags required, VkBuffer& outBuffer, VkDeviceMemory& outMemory,
                           VkDeviceAddress* outAddress, const char* debugName) {
    outBuffer = VK_NULL_HANDLE;
    outMemory = VK_NULL_HANDLE;
    const VulkanApi& api = dev.api();
    VkDevice device = dev.vkDevice();

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vkOk(api.CreateBuffer(device, &bi, nullptr, &outBuffer), debugName ? debugName : "rhi buffer")) return false;

    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(device, outBuffer, &req);

    u32 typeIndex = findMemoryType(dev.memoryProperties(), req.memoryTypeBits, required);
    if (typeIndex == ~0u && (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        typeIndex = findMemoryType(dev.memoryProperties(), req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (typeIndex == ~0u) {
        AVER_ERROR("[RHI.Vulkan] {}: no memory type matches required flags 0x{:x} (type bits 0x{:x})",
                   debugName ? debugName : "createBufferCommitted", static_cast<u32>(required), req.memoryTypeBits);
        api.DestroyBuffer(device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }

    // Every buffer here carries VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT (toVkBufferUsage's
    // "everything is everything" policy, or the caller's explicit AS-buffer usage), so its memory
    // must always be allocated with the matching flag -- D3D12 needs no such opt-out (GPU virtual
    // addresses are always available there, no allocation-time flag).
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &flagsInfo;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (!vkOk(api.AllocateMemory(device, &ai, nullptr, &outMemory), debugName ? debugName : "rhi buffer memory")) {
        api.DestroyBuffer(device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }
    if (!vkOk(api.BindBufferMemory(device, outBuffer, outMemory, 0), debugName ? debugName : "rhi buffer bind")) {
        api.FreeMemory(device, outMemory, nullptr);
        api.DestroyBuffer(device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (outAddress) {
        VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        dai.buffer = outBuffer;
        *outAddress = api.GetBufferDeviceAddress(device, &dai);
    }
    setVkObjectName(api, device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(outBuffer), debugName);
    return true;
}

bool createImageCommitted(VulkanDevice& dev, const VkImageCreateInfo& imageInfo,
                          VkMemoryPropertyFlags required, VkImage& outImage, VkDeviceMemory& outMemory,
                          const char* debugName) {
    outImage = VK_NULL_HANDLE;
    outMemory = VK_NULL_HANDLE;
    const VulkanApi& api = dev.api();
    VkDevice device = dev.vkDevice();
    if (!vkOk(api.CreateImage(device, &imageInfo, nullptr, &outImage), debugName ? debugName : "rhi image")) return false;

    VkMemoryRequirements req{};
    api.GetImageMemoryRequirements(device, outImage, &req);
    const u32 typeIndex = findMemoryType(dev.memoryProperties(), req.memoryTypeBits, required);
    if (typeIndex == ~0u) {
        AVER_ERROR("[RHI.Vulkan] {}: no memory type matches required flags 0x{:x} (type bits 0x{:x})",
                   debugName ? debugName : "createImageCommitted", static_cast<u32>(required), req.memoryTypeBits);
        api.DestroyImage(device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (!vkOk(api.AllocateMemory(device, &ai, nullptr, &outMemory), debugName ? debugName : "rhi image memory")) {
        api.DestroyImage(device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }
    if (!vkOk(api.BindImageMemory(device, outImage, outMemory, 0), debugName ? debugName : "rhi image bind")) {
        api.FreeMemory(device, outMemory, nullptr);
        api.DestroyImage(device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        outMemory = VK_NULL_HANDLE;
        return false;
    }
    setVkObjectName(api, device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(outImage), debugName);
    return true;
}

void destroyBufferCommitted(VulkanDevice& dev, VkBuffer buffer, VkDeviceMemory memory) {
    if (buffer) dev.api().DestroyBuffer(dev.vkDevice(), buffer, nullptr);
    if (memory) dev.api().FreeMemory(dev.vkDevice(), memory, nullptr);
}
void destroyImageCommitted(VulkanDevice& dev, VkImage image, VkDeviceMemory memory) {
    if (image) dev.api().DestroyImage(dev.vkDevice(), image, nullptr);
    if (memory) dev.api().FreeMemory(dev.vkDevice(), memory, nullptr);
}

// ================================================================================================
// 2b. W4: one-shot device-buffer upload (VulkanCommon.hpp has the full contract). The STAGING-BUFFER
//     half a Default-heap mesh needs that Upload-heap doesn't (writeBuffer refuses a non-mapped
//     buffer -- its own ERROR, "not BufferKind::Upload"). A VulkanResourceFactory member
//     (VulkanDevice::createMesh/createMeshSharingVertices
//     call it), calling runOneShotCommands/createBufferCommitted/destroyBufferCommitted as
//     transitionFreshImage does, all in this TU.
// ================================================================================================
bool VulkanResourceFactory::uploadToDeviceBuffers(const VkBuffer* dsts, const void* const* srcs,
                                                   const VkDeviceSize* sizes, u32 count, const char* what) {
    if (!dsts || !srcs || !sizes || count == 0) return true;
    VkDeviceSize total = 0;
    for (u32 i = 0; i < count; ++i) total += sizes[i];
    if (total == 0) return true;

    // ONE staging buffer for every destination, not one per buffer -- the whole point of batching
    // createMesh's vertex AND index upload into a single call is one map/memcpy/submit/wait instead
    // of two, and a second staging allocation here would throw that batching away.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!createBufferCommitted(*dev_, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               staging, stagingMemory, nullptr, what ? what : "rhi uploadToDeviceBuffers staging"))
        return false;

    u8* mapped = nullptr;
    if (!vkOk(dev_->api().MapMemory(dev_->vkDevice(), stagingMemory, 0, VK_WHOLE_SIZE, 0,
                                    reinterpret_cast<void**>(&mapped)),
              what ? what : "rhi uploadToDeviceBuffers map")) {
        destroyBufferCommitted(*dev_, staging, stagingMemory);
        return false;
    }
    std::vector<VkDeviceSize> offsets(count);
    VkDeviceSize cursor = 0;
    for (u32 i = 0; i < count; ++i) {
        offsets[i] = cursor;
        if (srcs[i] && sizes[i]) std::memcpy(mapped + cursor, srcs[i], static_cast<usize>(sizes[i]));
        cursor += sizes[i];
    }
    // HOST_COHERENT was required above (alongside HOST_VISIBLE), so no vkFlushMappedMemoryRanges is
    // needed before the copy commands below read it -- same reasoning as writeBuffer's own coherent
    // fast path.
    dev_->api().UnmapMemory(dev_->vkDevice(), stagingMemory);

    const bool ok = runOneShotCommands(*dev_, [&](VkCommandBuffer cmd) {
        for (u32 i = 0; i < count; ++i) {
            if (!sizes[i]) continue;
            VkBufferCopy region{offsets[i], 0, sizes[i]};
            dev_->api().CmdCopyBuffer(cmd, staging, dsts[i], 1, &region);
        }
    }, what ? what : "rhi uploadToDeviceBuffers copy");

    // The wait inside runOneShotCommands has already retired this submit, so the staging buffer is
    // safe to free immediately -- there is nothing left for the caller to clean up from this call
    // either way, matching the contract's own "the staging buffer is destroyed either way" note.
    destroyBufferCommitted(*dev_, staging, stagingMemory);
    return ok;
}

// ================================================================================================
// 3. Construction / destruction / init / selfTest
// ================================================================================================

VulkanResourceFactory::~VulkanResourceFactory() {
    // D3D12ResourceFactory's destructor is just `waitForGpu(); retired_.clear()` -- ComPtr RAII on
    // every RhiTexture/RhiBuffer/... member releases the rest. A VkImage/VkBuffer is a plain u64, not
    // a smart pointer, so the same "clear the vectors" here would leak every live allocation until
    // the whole VkDevice tears down. Order:
    // wait for the GPU (nothing below may run concurrently with a live submit), run every already-
    // retired destroy callback (its fence has passed), then walk each handle table and free what's left.
    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();
    dev_->waitForGpu();

    for (auto& r : retired_) if (r.destroy) r.destroy();
    retired_.clear();

    for (auto& t : textures_) {
        if (t.srvView) api.DestroyImageView(device, t.srvView, nullptr);
        if (t.rtvView) api.DestroyImageView(device, t.rtvView, nullptr);
        if (t.dsvView) api.DestroyImageView(device, t.dsvView, nullptr);
        for (VkImageView v : t.uavViews) if (v) api.DestroyImageView(device, v, nullptr);
        // externallyOwned records (today only the depth texture adoptExternalDepthTexture builds
        // over VulkanDevice::depthBuffer_/depthMemory_) do NOT own image/memory (see that flag's
        // comment on RhiTexture). VulkanDevice::~VulkanDevice() destroys the real depth buffer itself,
        // just below this factory's `delete rhiFactory_`; freeing it here too double-destroys a
        // VkImage/VkDeviceMemory Vulkan may have already recycled by then.
        if (!t.externallyOwned) destroyImageCommitted(*dev_, t.image, t.memory);
    }
    textures_.clear();

    for (auto& b : buffers_) destroyBufferCommitted(*dev_, b.buffer, b.memory);
    buffers_.clear();

    // THE VARIANTS TOO, not just the layout-agnostic module: a shader compiled per push-constant
    // shape owns one VkShaderModule per variant (RhiShader::variants); a shader that lives to
    // shutdown never passes through destroyShader, so every variant leaked here before this loop --
    // most of the modules the validation layer counted at vkDestroyDevice, growing with every
    // distinct layout the frame used.
    for (auto& s : shaders_) {
        for (RhiShaderVariant& v : s.variants)
            if (v.module && v.module != s.module) api.DestroyShaderModule(device, v.module, nullptr);
        s.variants.clear();
        if (s.module) api.DestroyShaderModule(device, s.module, nullptr);
    }
    shaders_.clear();

    for (auto& p : pipelines_) if (p.pipeline) api.DestroyPipeline(device, p.pipeline, nullptr);
    pipelines_.clear();

    // bindingSets_ hold no Vk-owned resource beyond the VkDescriptorSet itself, and every one of
    // those goes away in one shot with the pool destroy below.
    bindingSets_.clear();

    for (auto& b : blases_) {
        if (b.as) api.DestroyAccelerationStructureKHR(device, b.as, nullptr);
        destroyBufferCommitted(*dev_, b.asBuffer, b.asMemory);
        destroyBufferCommitted(*dev_, b.scratchBuffer, b.scratchMemory);
    }
    blases_.clear();

    for (auto& t : tlases_) {
        if (t.as) api.DestroyAccelerationStructureKHR(device, t.as, nullptr);
        destroyBufferCommitted(*dev_, t.asBuffer, t.asMemory);
        destroyBufferCommitted(*dev_, t.scratchBuffer, t.scratchMemory);
        for (u32 i = 0; i < kFrameCount; ++i) destroyBufferCommitted(*dev_, t.instanceBuffers[i], t.instanceMemory[i]);
    }
    tlases_.clear();

    for (auto& e : descriptorLayouts_) {
        if (e.constantsSetLayout) api.DestroyDescriptorSetLayout(device, e.constantsSetLayout, nullptr);
        if (e.samplersSetLayout) api.DestroyDescriptorSetLayout(device, e.samplersSetLayout, nullptr);
        if (e.pipelineLayout) api.DestroyPipelineLayout(device, e.pipelineLayout, nullptr);
        // tableSetLayouts[] are owned by tableShapes_ below, not here -- see TableShapeEntry's own
        // comment on why the two caches are separate.
    }
    descriptorLayouts_.clear();

    for (auto& e : tableShapes_) if (e.layout) api.DestroyDescriptorSetLayout(device, e.layout, nullptr);
    tableShapes_.clear();

    for (auto& e : samplers_) if (e.sampler) api.DestroySampler(device, e.sampler, nullptr);
    samplers_.clear();

    if (instanceSetLayout_) api.DestroyDescriptorSetLayout(device, instanceSetLayout_, nullptr);
    instanceSetLayout_ = VK_NULL_HANDLE;

    destroyNullResources(*dev_);

    // Every VkDescriptorSet allocated from this pool (every binding set, every samplers set) dies
    // in one shot here, which is why nothing above calls vkFreeDescriptorSets individually.
    if (descriptorPool_) api.DestroyDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    for (u32 f = 0; f < kFrameCount; ++f) {
        for (ConstantsPool& p : constantsPools_[f]) if (p.pool) api.DestroyDescriptorPool(device, p.pool, nullptr);
        constantsPools_[f].clear();
        constantsPoolCursor_[f] = 0;
    }
}

bool VulkanResourceFactory::init() {
    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();

    // Sizing is a policy call with no D3D12 number to derive it from: D3D12's kRhiHeapSize (65536)
    // counts descriptor SLOTS in one heap; Vulkan counts descriptor SETS plus a per-TYPE pool budget.
    // Sized generously, unmeasured -- revisit if vkAllocateDescriptorSets/vkCreateDescriptorPool ever
    // reports VK_ERROR_OUT_OF_POOL_MEMORY.
    constexpr u32 kMaxSets = 4096;
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8192},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
        // No UNIFORM_BUFFER_DYNAMIC: constants sets have their own per-frame pools (allocConstantsSet).
        // gInstanceWorlds, one per instanced draw (drawMeshInstanced allocates a fresh set each
        // time and retires it a frame later, so this budget covers kFrameCount frames of them).
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 512},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 256},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 256},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // FREE_DESCRIPTOR_SET_BIT: destroyBindingSet must be able to return its one set individually --
    // the Vulkan analog of D3D12's reusable descriptor RANGE (allocRange/freeRanges_) -- which a
    // Vulkan pool cannot do unless created with this flag (otherwise it can only be reset as a whole).
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kMaxSets;
    pi.poolSizeCount = static_cast<u32>(sizeof(sizes) / sizeof(sizes[0]));
    pi.pPoolSizes = sizes;
    if (!vkOk(api.CreateDescriptorPool(device, &pi, nullptr, &descriptorPool_), "rhi descriptor pool")) return false;

    // RhiPipeline::layoutEntry is a RAW POINTER into descriptorLayouts_ (VulkanCommon.hpp section 12:
    // "owned by VulkanResourceFactory's cache, not by this"). A vector that reallocates on growth
    // would dangle every already-built pipeline's pointer once a later layout pushes past capacity.
    // D3D12's RootSigEntry cache has no such hazard (RhiPipeline stores the raw ID3D12RootSignature*,
    // not a pointer to the cache entry). Reserving up front is the whole mitigation -- past 256
    // distinct (PipelineLayout, mesh) combinations needs a real fix, not just a bigger number.
    descriptorLayouts_.reserve(256);

    return true;
}

namespace {
const char* kSelfTestCS = R"(
// NO [[vk::binding]] HERE, DELIBERATELY, AND IT USED TO BE HERE. With no space annotation DXC puts
// b0 at set 0 -- which is table 0 (SRVs/UAVs) -- and the validation layer said exactly that: "uses
// descriptor [Set 0, Binding 0, variable SelfTestCB] but the binding was not declared in
// pSetLayouts[0]". The annotation fixed it, and then stopped being the right tool: this pipeline
// declares a UAV, so moduleForLayout now supplies a -fvk-bind-register map, and DXC requires such a
// map to cover EVERY resource. Two answers for where b0 goes is one more than DXC accepts.
cbuffer SelfTestCB : register(b0) { uint4 gValue; };
RWTexture3D<float4> gOut : register(u0);
[numthreads(4,4,4)]
void CSSelfTest(uint3 id : SV_DispatchThreadID) { gOut[id] = float4(gValue); }
)";
} // namespace

void VulkanResourceFactory::selfTest() {
    TextureDesc td{};
    td.dim = TextureDim::Tex3D;
    td.width = td.height = td.depth = 32;
    td.mips = 0;
    td.format = Format::RGBA16F;
    td.bind = ResourceBind::ShaderResource | ResourceBind::UnorderedAccess;
    td.initialState = ResourceState::UnorderedAccess;
    td.debugName = "AverRhiSelfTestVolume";
    const TextureHandle tex = createTexture(td);
    TextureDesc got{};
    const bool info = tex != 0 && textureInfo(tex, got);
    AVER_INFO("[RHI.Vulkan] factory self-test: texture {} (32^3 RGBA16F, {} mips resolved)",
              tex ? "ok" : "FAILED", info ? got.mips : 0u);

    BufferDesc bd{};
    bd.bytes = 4096;
    bd.kind = BufferKind::Upload;
    bd.debugName = "AverRhiSelfTestBuffer";
    const BufferHandle buf = createBuffer(bd);
    AVER_INFO("[RHI.Vulkan] factory self-test: buffer {}", buf ? "ok" : "FAILED");

    ShaderDesc sd{};
    sd.source = kSelfTestCS;
    sd.entry = "CSSelfTest";
    sd.stage = ShaderStage::Compute;
    sd.minShaderModel = 60;   // this backend's compiler is DXC-only; SM 5.1 (FXC-only) is not reachable
    const ShaderHandle cs = createShader(sd);
    ComputePipelineDesc cd{};
    cd.cs = cs;
    cd.layout.uavCount = 1;
    // ZERO, NOT 4 -- the SHADER'S choice, not a tuning. Non-zero means ROOT CONSTANTS lowered to
    // Vulkan PUSH CONSTANTS, but kSelfTestCS declares `cbuffer SelfTestCB : register(b0)`, which DXC
    // lowers to a DESCRIPTOR unless the HLSL says [[vk::push_constant]]. Push constants here would
    // promise what the shader doesn't ask for (a uniform buffer at set 0 binding 0); validation said
    // exactly that: "uses descriptor [Set 0, Binding 0, variable SelfTestCB] but the binding was not
    // declared". Zero requests a root CBV, a descriptor, matching what the shader wrote.
    cd.layout.constantDwords[0] = 0;
    const PipelineHandle pipe = cs ? createComputePipeline(cd) : 0;
    AVER_INFO("[RHI.Vulkan] factory self-test: compute pipeline {}", pipe ? "ok" : "FAILED");

    BindingSetDesc bsd{};
    bsd.srvCount = 1;
    bsd.uavCount = 1;
    bsd.srvKinds[0] = SlotKind::Texture3D;
    bsd.uavKinds[0] = SlotKind::Texture3D;
    const BindingSetHandle set = createBindingSet(bsd);
    if (set && tex) setUav(set, 0, tex, 0);
    AVER_INFO("[RHI.Vulkan] factory self-test: binding set {}", set ? "ok" : "FAILED");
    destroyBindingSet(set);

    BufferDesc sd2{};
    sd2.bytes = 4096;
    sd2.kind = BufferKind::Default;
    sd2.allowUnorderedAccess = true;
    sd2.debugName = "AverRhiSelfTestStructured";
    const BufferHandle sbuf = createBuffer(sd2);

    BufferDesc rb{};
    rb.bytes = 4096;
    rb.kind = BufferKind::Readback;
    rb.debugName = "AverRhiSelfTestReadback";
    const BufferHandle rbuf = createBuffer(rb);

    BindingSetDesc bufSet{};
    bufSet.srvCount = 1;
    bufSet.uavCount = 1;
    bufSet.srvKinds[0] = SlotKind::StructuredBuffer;
    bufSet.uavKinds[0] = SlotKind::StructuredBuffer;
    const BindingSetHandle bset = createBindingSet(bufSet);
    if (bset && sbuf) {
        setSrvBuffer(bset, 0, sbuf, 16, 256, 0);
        setUavBuffer(bset, 0, sbuf, 16, 256, 0);
    }
    u8 probe[16] = {};
    const bool readOk = rbuf && readBuffer(rbuf, probe, sizeof probe, 0);
    AVER_INFO("[RHI.Vulkan] factory self-test: buffer views {} (structured SRV+UAV, readback {})",
              (sbuf && rbuf && bset) ? "ok" : "FAILED", readOk ? "ok" : "FAILED");

    destroyBindingSet(bset);
    destroyBuffer(rbuf);
    destroyBuffer(sbuf);
    destroyPipeline(pipe);
    destroyShader(cs);
    destroyBuffer(buf);
    destroyTexture(tex);
    collect();
}

// ================================================================================================
// 4. Table lookups
// ================================================================================================
RhiTexture* VulkanResourceFactory::texture(TextureHandle h) {
    if (h == 0 || h > textures_.size()) return nullptr;
    RhiTexture& t = textures_[h - 1];
    return t.image ? &t : nullptr;
}
const RhiTexture* VulkanResourceFactory::texture(TextureHandle h) const {
    if (h == 0 || h > textures_.size()) return nullptr;
    const RhiTexture& t = textures_[h - 1];
    return t.image ? &t : nullptr;
}
RhiBuffer* VulkanResourceFactory::buffer(BufferHandle h) {
    if (h == 0 || h > buffers_.size()) return nullptr;
    RhiBuffer& b = buffers_[h - 1];
    return b.buffer ? &b : nullptr;
}
const RhiBuffer* VulkanResourceFactory::buffer(BufferHandle h) const {
    if (h == 0 || h > buffers_.size()) return nullptr;
    const RhiBuffer& b = buffers_[h - 1];
    return b.buffer ? &b : nullptr;
}
RhiShader* VulkanResourceFactory::shader(ShaderHandle h) {
    if (h == 0 || h > shaders_.size()) return nullptr;
    RhiShader& s = shaders_[h - 1];
    return s.module ? &s : nullptr;
}
RhiPipeline* VulkanResourceFactory::pipeline(PipelineHandle h) {
    if (h == 0 || h > pipelines_.size()) return nullptr;
    RhiPipeline& p = pipelines_[h - 1];
    return p.pipeline ? &p : nullptr;
}
RhiBindingSet* VulkanResourceFactory::bindingSet(BindingSetHandle h) {
    if (h == 0 || h > bindingSets_.size()) return nullptr;
    RhiBindingSet& s = bindingSets_[h - 1];
    return s.alive ? &s : nullptr;
}
RhiBlas* VulkanResourceFactory::blas(BlasHandle h) {
    if (h == 0 || h > blases_.size()) return nullptr;
    RhiBlas& b = blases_[h - 1];
    return b.as ? &b : nullptr;
}
RhiTlas* VulkanResourceFactory::tlas(TlasHandle h) {
    if (h == 0 || h > tlases_.size()) return nullptr;
    RhiTlas& t = tlases_[h - 1];
    return t.as ? &t : nullptr;
}
VkBuffer VulkanResourceFactory::bufferResource(BufferHandle h) const {
    return (h == 0 || h > buffers_.size()) ? VK_NULL_HANDLE : buffers_[h - 1].buffer;
}

// ================================================================================================
// 5. Descriptor-set-layout / pipeline-layout cache, sampler cache
// ================================================================================================
VkDescriptorSetLayout VulkanResourceFactory::tableSetLayout(u32 srvCount, u32 uavCount,
                                                             const SlotKind* srvKinds, const SlotKind* uavKinds) {
    for (auto& e : tableShapes_) {
        if (sameTableShape(e.srvCount, e.uavCount, e.srvKinds, e.uavKinds, srvCount, uavCount, srvKinds, uavKinds))
            return e.layout;
    }

    const bool rtSupported = dev_->cachedCaps().rayTracingTier != 0;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(static_cast<usize>(srvCount) + uavCount);
    for (u32 i = 0; i < srvCount; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = i;
        b.descriptorType = toVkSrvDescriptorType(srvKinds[i], rtSupported);
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(b);
    }
    for (u32 i = 0; i < uavCount; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = kVkUavBindingBase + i;
        b.descriptorType = toVkUavDescriptorType(uavKinds[i]);
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<u32>(bindings.size());
    ci.pBindings = bindings.empty() ? nullptr : bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateDescriptorSetLayout(dev_->vkDevice(), &ci, nullptr, &layout), "rhi table descriptor set layout"))
        return VK_NULL_HANDLE;

    TableShapeEntry e;
    e.srvCount = srvCount;
    e.uavCount = uavCount;
    for (u32 i = 0; i < srvCount && i < kMaxBindingSlots; ++i) e.srvKinds[i] = srvKinds[i];
    for (u32 i = 0; i < uavCount && i < kMaxBindingSlots; ++i) e.uavKinds[i] = uavKinds[i];
    e.layout = layout;
    tableShapes_.push_back(e);
    return layout;
}

VkSampler VulkanResourceFactory::getOrCreateSampler(const SamplerDesc& d) {
    for (auto& e : samplers_) if (sameSampler(e.desc, d)) return e.sampler;

    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = toVkFilter(d.filter);
    ci.minFilter = toVkFilter(d.filter);
    ci.mipmapMode = toVkMipmapMode(d.filter);
    ci.addressModeU = ci.addressModeV = ci.addressModeW = toVkAddressMode(d.address);
    ci.minLod = 0.0f;
    ci.maxLod = d.maxLod;
    // compareEnable/compareOp are meaningful exactly when the caller asked for ComparisonLinear --
    // SamplerDesc's own comment marks `compare` as "ComparisonLinear only", so the caller is trusted
    // to have set a real op alongside that filter, matching D3D12's D3D12_FILTER_COMPARISON_* path.
    ci.compareEnable = (d.filter == Filter::ComparisonLinear) ? VK_TRUE : VK_FALSE;
    ci.compareOp = toVkCompareOp(d.compare);
    ci.anisotropyEnable = (d.filter == Filter::Anisotropic) ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = d.maxAnisotropy ? static_cast<f32>(d.maxAnisotropy) : 1.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateSampler(dev_->vkDevice(), &ci, nullptr, &s), "rhi sampler")) return VK_NULL_HANDLE;
    samplers_.push_back({d, s});
    return s;
}

// The one descriptor-set layout every GraphicsPipelineDesc::instanced pipeline shares: a single
// STORAGE_BUFFER_DYNAMIC at binding 0 (gInstanceWorlds).
// SHARED RATHER THAN PER-ENTRY: shape never depends on the PipelineLayout, and Vulkan descriptor-set
// compatibility is by LAYOUT, so one set allocated here binds against any instanced pipeline
// (VulkanRenderContext::drawMeshInstanced relies on this).
// DYNAMIC so the per-draw ring offset rides in pDynamicOffsets instead of a fresh write per draw --
// same mechanism the kVkSetConstants set already uses.
VkDescriptorSetLayout VulkanResourceFactory::instanceSetLayout() {
    if (instanceSetLayout_ != VK_NULL_HANDLE) return instanceSetLayout_;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &b;
    if (!vkOk(dev_->api().CreateDescriptorSetLayout(dev_->vkDevice(), &ci, nullptr, &instanceSetLayout_),
              "rhi instance descriptor set layout"))
        return VK_NULL_HANDLE;
    return instanceSetLayout_;
}

const DescriptorLayoutEntry* VulkanResourceFactory::descriptorLayout(const PipelineLayout& layout, bool mesh, bool instanced) {
    // REFUSED, LOUDLY, RATHER THAN MIS-BOUND SILENTLY.
    // D3D12 honours PipelineLayout::constantSpace/samplerSpace as a free-floating root-parameter tag;
    // Vulkan turns a register space into a DESCRIPTOR SET, a real layout-compatibility unit, and this
    // backend's binding model is a fixed five-set scheme (tables 0/1, constants 2, samplers 3,
    // instances 4) that VulkanRenderContext hard-codes at every CmdBindDescriptorSets call. Honouring
    // a non-zero space needs a different set layout AND a different bind index everywhere.
    // Doing that halfway is the dangerous outcome: descriptors built correctly but bound at the wrong
    // set produce no validation error (no validation-layer binaries on this dev machine -- see
    // VulkanShaderCompiler.cpp) and no crash, just a shader silently reading the wrong memory --
    // this engine has been bitten by that exact shape before.
    // AND IT WOULD STILL NOT RUN PRECOMPILED LIBRARIES, the only callers that want this: their
    // SPIR-V (compiled offline with its own -fvk register shifts) has its (set, binding) pairs baked
    // in, needing a bespoke VkDescriptorSetLayout built from reflected bindings, not a renumbered space. This
    // refusal costs the Vulkan path nothing it could otherwise have had.
    if (layout.constantSpace != 0 || layout.samplerSpace != 0) {
        AVER_ERROR("[RHI.Vulkan] pipeline layout asks for constants in register space {} and samplers "
                   "in space {}; this backend implements space 0 only. A shader compiled for another "
                   "space needs its real descriptor sets read out of its SPIR-V, not renumbered here.",
                   layout.constantSpace, layout.samplerSpace);
        return nullptr;
    }

    SlotKind srv0[kMaxBindingSlots], uav0[kMaxBindingSlots], srv1[kMaxBindingSlots], uav1[kMaxBindingSlots];
    for (u32 i = 0; i < kMaxBindingSlots; ++i) srv0[i] = uav0[i] = srv1[i] = uav1[i] = SlotKind::Texture2D;

    // WHAT THE CALLER DECLARED BEATS WHAT THE SHADER HAPPENS TO USE -- the whole fix for the
    // slot-kind bug class: reflection can't see a slot the shader doesn't reference (DXC eliminates
    // it), so it silently defaults to Texture2D, producing e.g. SAMPLED_IMAGE at set 0 binding 2
    // against a binding set holding an ACCELERATION_STRUCTURE, caught only at the draw, three
    // steps from the cause.
    // A declared layout goes through tableSetLayout() with the SAME kinds BindingSetDesc gives it,
    // so both sides land on the same cached VkDescriptorSetLayout by construction, not agreement.
    if (layout.slotKindsDeclared) {
        std::memcpy(srv0, layout.srvKinds,  sizeof(srv0));
        std::memcpy(uav0, layout.uavKinds,  sizeof(uav0));
        std::memcpy(srv1, layout.srvKinds1, sizeof(srv1));
        std::memcpy(uav1, layout.uavKinds1, sizeof(uav1));
    } else if (gPendingKinds.active) {
        std::memcpy(srv0, gPendingKinds.srv0, sizeof(srv0));
        std::memcpy(uav0, gPendingKinds.uav0, sizeof(uav0));
        std::memcpy(srv1, gPendingKinds.srv1, sizeof(srv1));
        std::memcpy(uav1, gPendingKinds.uav1, sizeof(uav1));
        // Every slot reflection could not account for is a guess, and a guess that is wrong is only
        // reported at the draw. Naming them here puts the warning next to the cause.
        std::string guessed;
        for (u32 i = 0; i < layout.srvCount && i < kMaxBindingSlots; ++i)
            if (!gPendingKinds.srv0Seen[i]) { guessed += guessed.empty() ? "t" : ", t"; guessed += std::to_string(i); }
        for (u32 i = 0; i < layout.srvCount1 && i < kMaxBindingSlots; ++i)
            if (!gPendingKinds.srv1Seen[i]) { guessed += guessed.empty() ? "t" : ", t"; guessed += std::to_string(layout.srvCount + i); }
        for (u32 i = 0; i < layout.uavCount && i < kMaxBindingSlots; ++i)
            if (!gPendingKinds.uav0Seen[i]) { guessed += guessed.empty() ? "u" : ", u"; guessed += std::to_string(i); }
        if (!guessed.empty())
            AVER_WARN("[RHI.Vulkan] no shader in this pipeline uses {} -- their kinds are GUESSED as "
                      "Texture2D. If a binding set declares them as anything else the draw will be "
                      "rejected; set PipelineLayout::slotKindsDeclared and fill in the kinds",
                      guessed);
    } else if (layout.srvCount || layout.uavCount || layout.srvCount1 || layout.uavCount1) {
        AVER_WARN("[RHI.Vulkan] descriptorLayout building table shapes with no reflected kinds "
                  "(called outside createGraphicsPipeline/createComputePipeline?); every slot "
                  "defaults to Texture2D");
    }

    const VkDescriptorSetLayout wantTable0 = tableSetLayout(layout.srvCount, layout.uavCount, srv0, uav0);
    const VkDescriptorSetLayout wantTable1 = tableSetLayout(layout.srvCount1, layout.uavCount1, srv1, uav1);
    if (wantTable0 == VK_NULL_HANDLE || wantTable1 == VK_NULL_HANDLE) return nullptr;

    // Cache lookup is deliberately MORE restrictive than sameLayout() alone (VulkanCommon.hpp pins
    // it verbatim; counts/constants/samplers only, never SlotKind): two PipelineLayouts with
    // identical counts but different resource kinds (e.g. one's u0 is StructuredBuffer, another's
    // Texture3D, both uavCount==1) MUST NOT share an entry, or the second pipeline silently gets
    // the first's table layout. tableSetLayout() already dedupes by REAL shape, so
    // comparing its OUTPUT handles adds that precision for free, without touching this struct's fields.
    for (auto& e : descriptorLayouts_) {
        // e.instanced is part of the key for the same reason e.mesh is: it changes the SETS this
        // pipeline layout has, so two entries that differ only in it are not interchangeable.
        if (e.mesh == mesh && e.instanced == instanced && sameLayout(e.layout, layout) &&
            e.tableSetLayouts[kVkSetTable0] == wantTable0 && e.tableSetLayouts[kVkSetTable1] == wantTable1)
            return &e;
    }

    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();

    DescriptorLayoutEntry e;
    e.layout = layout;
    e.mesh = mesh;
    e.instanced = instanced;
    e.tableSetLayouts[kVkSetTable0] = wantTable0;
    e.tableSetLayouts[kVkSetTable1] = wantTable1;
    e.pushConstants = pushConstantLayout(layout, mesh);
    if (e.pushConstants.totalBytes > dev_->maxPushConstantsSize()) {
        AVER_ERROR("[RHI.Vulkan] pipeline layout needs {} bytes of push constants, over this device's "
                   "{}-byte limit", e.pushConstants.totalBytes, dev_->maxPushConstantsSize());
        return nullptr;
    }

    // ---- constants set: one dynamic-UBO binding per declared zero-dword slot, always including b0
    {
        VkDescriptorSetLayoutBinding bindings[kMaxConstantSlots];
        u32 n = 0;
        for (u32 k = 0; k < kMaxConstantSlots; ++k) {
            if (k != kEngineFrameConstantRegister && layout.constantDwords[k] != 0) continue;
            bindings[n] = VkDescriptorSetLayoutBinding{};
            bindings[n].binding = k;
            bindings[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindings[n].descriptorCount = 1;
            bindings[n].stageFlags = VK_SHADER_STAGE_ALL;
            ++n;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = n;
        ci.pBindings = bindings;
        if (!vkOk(api.CreateDescriptorSetLayout(device, &ci, nullptr, &e.constantsSetLayout), "rhi constants descriptor set layout"))
            return nullptr;
    }

    // ---- samplers set: immutable only, VK_NULL_HANDLE (DescriptorLayoutEntry's own comment) when
    // this layout declares none, so the pipeline layout below can OMIT set index 3 entirely.
    // EXCEPT WHEN SET 4 EXISTS: pSetLayouts has no holes, so an instanced pipeline (needs set
    // kVkSetInstances/4) can't omit set 3 even with no samplers. An EMPTY descriptor set layout is
    // the legal way to say "nothing at this index" -- allocates no descriptors, binds nothing.
    if (layout.samplerCount == 0 && instanced) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 0;
        if (!vkOk(api.CreateDescriptorSetLayout(device, &ci, nullptr, &e.samplersSetLayout),
                  "rhi empty samplers descriptor set layout (instanced pipeline)")) {
            api.DestroyDescriptorSetLayout(device, e.constantsSetLayout, nullptr);
            return nullptr;
        }
        // No samplersSet is allocated: an empty layout has nothing to allocate, and nothing binds
        // set 3 on this pipeline (see where samplersSet is bound -- it is guarded on the handle).
    } else if (layout.samplerCount > 0) {
        const u32 n = layout.samplerCount < 4 ? layout.samplerCount : 4;
        VkDescriptorSetLayoutBinding bindings[4];
        VkSampler immutable[4];
        for (u32 i = 0; i < n; ++i) {
            immutable[i] = getOrCreateSampler(layout.samplers[i]);
            bindings[i] = VkDescriptorSetLayoutBinding{};
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
            bindings[i].pImmutableSamplers = &immutable[i];
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = n;
        ci.pBindings = bindings;
        if (!vkOk(api.CreateDescriptorSetLayout(device, &ci, nullptr, &e.samplersSetLayout), "rhi samplers descriptor set layout")) {
            api.DestroyDescriptorSetLayout(device, e.constantsSetLayout, nullptr);
            return nullptr;
        }
        // Allocated and populated ONCE, here -- nothing is ever WRITTEN to it at draw time
        // (pImmutableSamplers bakes every VkSampler into the layout itself), so this is the one and
        // only time this set needs to exist.
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &e.samplersSetLayout;
        if (!vkOk(api.AllocateDescriptorSets(device, &ai, &e.samplersSet), "rhi samplers descriptor set")) {
            api.DestroyDescriptorSetLayout(device, e.samplersSetLayout, nullptr);
            api.DestroyDescriptorSetLayout(device, e.constantsSetLayout, nullptr);
            return nullptr;
        }
    }

    if (instanced) {
        e.instancesSetLayout = instanceSetLayout();
        if (e.instancesSetLayout == VK_NULL_HANDLE) {
            if (e.samplersSet) api.FreeDescriptorSets(device, descriptorPool_, 1, &e.samplersSet);
            if (e.samplersSetLayout) api.DestroyDescriptorSetLayout(device, e.samplersSetLayout, nullptr);
            api.DestroyDescriptorSetLayout(device, e.constantsSetLayout, nullptr);
            return nullptr;
        }
    }

    VkDescriptorSetLayout setLayouts[kVkDescriptorSetCount] = {
        e.tableSetLayouts[kVkSetTable0], e.tableSetLayouts[kVkSetTable1], e.constantsSetLayout,
        e.samplersSetLayout, e.instancesSetLayout,
    };
    // TRAILING sets are what may be dropped, never interior ones -- pSetLayouts has no holes. So an
    // instanced pipeline takes all five (set 3 is empty-but-present above when it has no samplers),
    // a sampler pipeline four, and everything else three.
    const u32 setLayoutCount = instanced                 ? kVkDescriptorSetCount
                             : (layout.samplerCount > 0) ? kVkSetSamplers + 1
                                                         : kVkSetSamplers;

    VkPushConstantRange pcRange{};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = setLayoutCount;
    pli.pSetLayouts = setLayouts;
    if (e.pushConstants.totalBytes > 0) {
        pcRange.stageFlags = VK_SHADER_STAGE_ALL;
        pcRange.size = e.pushConstants.totalBytes;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcRange;
    }
    if (!vkOk(api.CreatePipelineLayout(device, &pli, nullptr, &e.pipelineLayout), "rhi pipeline layout")) {
        if (e.samplersSet) api.FreeDescriptorSets(device, descriptorPool_, 1, &e.samplersSet);
        if (e.samplersSetLayout) api.DestroyDescriptorSetLayout(device, e.samplersSetLayout, nullptr);
        api.DestroyDescriptorSetLayout(device, e.constantsSetLayout, nullptr);
        return nullptr;
    }

    descriptorLayouts_.push_back(e);
    return &descriptorLayouts_.back();
}

// ================================================================================================
// 6. Deferred destruction
// ================================================================================================
u64 VulkanResourceFactory::retireFence() const { return dev_->retireFenceValue(); }

void VulkanResourceFactory::retire(std::function<void()> destroyFn) {
    if (!destroyFn) return;
    retired_.push_back({retireFence(), std::move(destroyFn)});
}

void VulkanResourceFactory::collect() {
    if (retired_.empty()) return;
    u64 done = 0;
    dev_->api().GetSemaphoreCounterValue(dev_->vkDevice(), dev_->timelineSemaphore(), &done);
    for (usize i = 0; i < retired_.size();) {
        if (retired_[i].fence <= done) {
            if (retired_[i].destroy) retired_[i].destroy();
            retired_[i] = std::move(retired_.back());
            retired_.pop_back();
        } else {
            ++i;
        }
    }
}

namespace {
constexpr u32 kConstantsPoolSets = 1024;
constexpr u32 kConstantsPoolsMax = 64;   // per slot: 65,536 sets, far past any real frame
}  // namespace

VkDescriptorSet VulkanResourceFactory::allocConstantsSet(VkDescriptorSetLayout layout) {
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    std::vector<ConstantsPool>& chain = constantsPools_[f];
    u32& cur = constantsPoolCursor_[f];
    while (cur < chain.size() && chain[cur].used >= kConstantsPoolSets) ++cur;
    if (cur == chain.size()) {
        if (chain.size() >= kConstantsPoolsMax) {
            AVER_ERROR("[RHI.Vulkan] constants descriptor pools exhausted ({} sets in one frame)",
                       kConstantsPoolSets * kConstantsPoolsMax);
            return VK_NULL_HANDLE;
        }
        // Every binding of a constants set is one dynamic UBO, at most kMaxConstantSlots - 1 of them (b1 is
        // push constants), so the set count is the only limit that can bind.
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kConstantsPoolSets * (kMaxConstantSlots - 1)};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kConstantsPoolSets;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        ConstantsPool p;
        if (!vkOk(dev_->api().CreateDescriptorPool(dev_->vkDevice(), &pci, nullptr, &p.pool), "constants descriptor pool"))
            return VK_NULL_HANDLE;
        chain.push_back(p);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = chain[cur].pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().AllocateDescriptorSets(dev_->vkDevice(), &ai, &set), "vkAllocateDescriptorSets (constants set)"))
        return VK_NULL_HANDLE;
    ++chain[cur].used;
    return set;
}

void VulkanResourceFactory::resetConstantsPools(u32 slot) {
    if (slot >= kFrameCount) return;
    for (ConstantsPool& p : constantsPools_[slot]) {
        if (p.used) dev_->api().ResetDescriptorPool(dev_->vkDevice(), p.pool, 0);
        p.used = 0;
    }
    constantsPoolCursor_[slot] = 0;
}

// ================================================================================================
// 7. Textures / buffers
// ================================================================================================
// The layout a texture<->buffer copy of one mip uses on this backend.
// TIGHTLY PACKED, unlike D3D12's: VkBufferImageCopy can leave bufferRowLength/bufferImageHeight at 0
// ("rows exactly as wide as the image"), so there's no 256-byte row rule here and rowBytes ==
// rowPitch always. Same arithmetic uploadInitialData uses for its staging footprint, so an upload
// and readback of the same mip agree by construction, not by two functions happening to match.
bool VulkanResourceFactory::textureCopyFootprint(TextureHandle t, u32 mip, TextureCopyFootprint& out) const {
    const RhiTexture* tex = const_cast<VulkanResourceFactory*>(this)->texture(t);
    if (!tex || tex->image == VK_NULL_HANDLE || mip >= tex->desc.mips) return false;

    const u32 w = tex->desc.width  >> mip ? tex->desc.width  >> mip : 1u;
    const u32 h = tex->desc.height >> mip ? tex->desc.height >> mip : 1u;
    const u32 d = tex->desc.depth  >> mip ? tex->desc.depth  >> mip : 1u;
    const u64 rowPitch = packedRowPitchVk(tex->desc.format, w);
    if (rowPitch == 0) return false;

    const u32 rows = isBlockFormat(tex->desc.format) ? (h + 3) / 4 : h;
    out.rowPitch   = static_cast<u32>(rowPitch);
    out.rowBytes   = static_cast<u32>(rowPitch);
    out.rows       = rows;
    out.depth      = d;
    out.totalBytes = rowPitch * rows * d;
    return true;
}

bool VulkanResourceFactory::uploadInitialData(VkImage image, const VkImageCreateInfo& ci, const TextureDesc& d, u32 mips) {
    if (packedRowPitchVk(d.format, 1) == 0) {
        AVER_ERROR("[RHI.Vulkan] createTexture: initial data for a format with no CPU footprint");
        return false;
    }
    const u32 count = d.initialDataCount < mips ? d.initialDataCount : mips;
    const VkImageAspectFlags aspect = toVkAspect(d.format);

    struct Footprint { u64 offset; u32 width, height, depth, rowPitch; };
    std::vector<Footprint> fp(count);
    u64 total = 0;
    for (u32 s = 0; s < count; ++s) {
        const u32 w = std::max(1u, ci.extent.width >> s);
        const u32 h = std::max(1u, ci.extent.height >> s);
        const u32 dep = std::max(1u, ci.extent.depth >> s);
        const u64 rowPitch = packedRowPitchVk(d.format, w);
        fp[s] = {total, w, h, dep, static_cast<u32>(rowPitch)};
        total += rowPitch * (isBlockFormat(d.format) ? (h + 3) / 4 : h) * dep;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!createBufferCommitted(*dev_, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               staging, stagingMemory, nullptr, "rhi texture staging"))
        return false;

    u8* mapped = nullptr;
    if (!vkOk(dev_->api().MapMemory(dev_->vkDevice(), stagingMemory, 0, total, 0, reinterpret_cast<void**>(&mapped)), "rhi staging map")) {
        destroyBufferCommitted(*dev_, staging, stagingMemory);
        return false;
    }
    for (u32 s = 0; s < count; ++s) {
        const auto* src = static_cast<const u8*>(d.initialData[s]);
        if (!src) continue;
        const u64 srcPitch = (s == 0 && d.initialRowPitch) ? d.initialRowPitch : fp[s].rowPitch;
        const u32 rows = isBlockFormat(d.format) ? (fp[s].height + 3) / 4 : fp[s].height;
        u8* dst = mapped + fp[s].offset;
        for (u32 z = 0; z < fp[s].depth; ++z) {
            for (u32 y = 0; y < rows; ++y) {
                const u64 row = static_cast<u64>(z) * rows + y;
                std::memcpy(dst + row * fp[s].rowPitch, src + row * srcPitch, static_cast<usize>(fp[s].rowPitch));
            }
        }
    }
    dev_->api().UnmapMemory(dev_->vkDevice(), stagingMemory);

    const VkImageBarrierInfo finalInfo = toVkImageBarrierInfo(d.initialState);
    const bool ok = runOneShotCommands(*dev_, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = image;
        toDst.subresourceRange = {aspect, 0, mips, 0, 1};
        VkDependencyInfo dep0{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep0.imageMemoryBarrierCount = 1;
        dep0.pImageMemoryBarriers = &toDst;
        dev_->api().CmdPipelineBarrier2(cmd, &dep0);

        for (u32 s = 0; s < count; ++s) {
            if (!d.initialData[s]) continue;
            VkBufferImageCopy region{};
            region.bufferOffset = fp[s].offset;
            region.imageSubresource = {aspect, s, 0, 1};
            region.imageExtent = {fp[s].width, fp[s].height, fp[s].depth};
            dev_->api().CmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        VkImageMemoryBarrier2 toFinal{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toFinal.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toFinal.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toFinal.dstStageMask = finalInfo.stage;
        toFinal.dstAccessMask = finalInfo.access;
        toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toFinal.newLayout = (finalInfo.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_GENERAL : finalInfo.layout;
        toFinal.srcQueueFamilyIndex = toFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.image = image;
        toFinal.subresourceRange = {aspect, 0, mips, 0, 1};
        VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep1.imageMemoryBarrierCount = 1;
        dep1.pImageMemoryBarriers = &toFinal;
        dev_->api().CmdPipelineBarrier2(cmd, &dep1);
    }, "rhi texture upload");

    destroyBufferCommitted(*dev_, staging, stagingMemory);
    return ok;
}

TextureHandle VulkanResourceFactory::createTexture(const TextureDesc& d) {
    collect();
    if (d.width == 0 || d.height == 0) { AVER_ERROR("[RHI.Vulkan] createTexture with a zero extent"); return 0; }
    const VkFormat fmt = toVkFormat(d.format);
    if (fmt == VK_FORMAT_UNDEFINED) { AVER_ERROR("[RHI.Vulkan] createTexture with an unknown format"); return 0; }

    if (isBlockFormat(d.format)) {
        if (any(d.bind, ResourceBind::UnorderedAccess) || any(d.bind, ResourceBind::RenderTarget) || any(d.bind, ResourceBind::DepthStencil)) {
            AVER_ERROR("[RHI.Vulkan] createTexture: a block-compressed format is sample-only");
            return 0;
        }
        if ((d.width & 3u) || (d.height & 3u)) {
            AVER_ERROR("[RHI.Vulkan] createTexture: block-compressed extents must be multiples of 4 ({}x{})", d.width, d.height);
            return 0;
        }
        if (d.dim == TextureDim::Tex3D) { AVER_ERROR("[RHI.Vulkan] createTexture: block-compressed volumes are not supported"); return 0; }
    }

    const u32 depth = (d.dim == TextureDim::Tex3D && d.depth) ? d.depth : 1;
    u32 mips = d.mips;
    if (mips == 0) {
        u32 largest = d.width > d.height ? d.width : d.height;
        if (d.dim == TextureDim::Tex3D && depth > largest) largest = depth;
        mips = 1;
        for (u32 e = largest; e > 1; e >>= 1) ++mips;
    }

    const bool isDepth = isDepthFormatVk(d.format);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = (d.dim == TextureDim::Tex3D) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {d.width, d.height, depth};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = toVkImageUsage(d.bind, isDepth);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const bool seeded = d.initialData && d.initialDataCount > 0;
    RhiTexture t{};
    if (!createImageCommitted(*dev_, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.image, t.memory, d.debugName)) return 0;

    if (seeded) {
        if (!uploadInitialData(t.image, ci, d, mips)) { destroyImageCommitted(*dev_, t.image, t.memory); return 0; }
    } else if (d.initialState != ResourceState::Common) {
        transitionFreshImage(*dev_, t.image, toVkAspect(d.format), mips, d.initialState);
    }

    const VkImageAspectFlags aspect = toVkAspect(d.format);
    if (any(d.bind, ResourceBind::ShaderResource)) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = viewTypeFor(d.dim);
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, mips, 0, 1};
        if (!vkOk(dev_->api().CreateImageView(dev_->vkDevice(), &vi, nullptr, &t.srvView), "rhi texture SRV view")) {
            destroyImageCommitted(*dev_, t.image, t.memory);
            return 0;
        }
    }
    if (any(d.bind, ResourceBind::RenderTarget)) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = viewTypeFor(d.dim);
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkOk(dev_->api().CreateImageView(dev_->vkDevice(), &vi, nullptr, &t.rtvView), "rhi texture RTV view");
    }
    if (any(d.bind, ResourceBind::DepthStencil) || isDepth) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkOk(dev_->api().CreateImageView(dev_->vkDevice(), &vi, nullptr, &t.dsvView), "rhi texture DSV view");
    }
    t.uavViews.assign(any(d.bind, ResourceBind::UnorderedAccess) ? mips : 0, VK_NULL_HANDLE);

    t.desc = d;
    t.desc.mips = mips;
    t.desc.depth = depth;
    t.desc.debugName = nullptr;
    if (d.debugName) t.debugName = d.debugName;
#if AVER_RHI_TRACK_STATE
    t.states.assign(mips, d.initialState);
#endif
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

// See this method's declaration (VulkanCommon.hpp) for what it's for; a structural, not line-for-line,
// port of D3D12ResourceFactory::adoptExternalDepthTexture (D3D12Device.cpp:4975-5006). No
// typeless-resource trick needed here (toVkFormat's Format::R32Typeless comment): only one VkFormat
// (kVkDepthFormat) exists for this image, and SRV/DSV differ only by VkImageView/VkImageLayout.
TextureHandle VulkanResourceFactory::adoptExternalDepthTexture(VkImage image, VkDeviceMemory memory,
                                                                 u32 width, u32 height, TextureHandle existing) {
    if (!image) return 0;

    // The view a shader samples through -- SEPARATE from VulkanDevice's own depthView_ (created,
    // used as the per-frame depth ATTACHMENT, destroyed on its own schedule: destructor, resize(),
    // setSampleCount()), even with textually identical parameters. Sharing one VkImageView between
    // this record and VulkanDevice would double-destroy it, same as sharing `image` itself would
    // double-free it without RhiTexture::externallyOwned. Same format/aspect as depthView_
    // (VK_FORMAT_D32_SFLOAT / VK_IMAGE_ASPECT_DEPTH_BIT); Vulkan samples depth directly, no
    // DXGI_TYPELESS-style trick needed (see toVkAspect).
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kVkDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView srv = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateImageView(dev_->vkDevice(), &vi, nullptr, &srv), "adopted depth SRV view"))
        return 0;

    RhiTexture t{};
    t.image = image;
    t.memory = memory;   // shared, not owned -- see RhiTexture::externallyOwned's own comment
    t.srvView = srv;
    t.externallyOwned = true;
    t.desc.dim = TextureDim::Tex2D;
    t.desc.width = width; t.desc.height = height; t.desc.depth = 1; t.desc.mips = 1;
    // R32Typeless: the SAME "DSV sees D32Float, SRV sees a plain float" generic-RHI tag
    // D3D12ResourceFactory::adoptExternalDepthTexture uses (Format::R32Typeless doc comment,
    // RHIResources.hpp) -- the exact alias VoxiRenderer's own shadow map relies on -- kept even
    // though toVkFormat/toVkAspect resolve it identically to D32Float
    // here (both -> kVkDepthFormat/VK_IMAGE_ASPECT_DEPTH_BIT), so a caller branching on "is this the
    // aliased-depth format" (e.g. occlusion's SlotKind selection) sees the SAME answer on both backends.
    t.desc.format = Format::R32Typeless;
    t.desc.bind = ResourceBind::ShaderResource | ResourceBind::DepthStencil;
    // Matches physical reality at adoption: createDepthBuffer() transitions the freshly created
    // depth image to VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL on first use (beginFrame()'s
    // swapchain-acquire barriers), and nothing moves it away before each frame's scenePass() writes
    // it again. A reader (modules/occlusion) does its own DepthWrite<->NonPixelShaderResource round
    // trip via textureBarrier -- this factory has no idea when that's safe (mirrors the D3D12 comment).
    t.desc.initialState = ResourceState::DepthWrite;
    t.desc.debugName = nullptr;
    t.debugName = "scene depth (adopted)";
#if AVER_RHI_TRACK_STATE
    t.states.assign(1, ResourceState::DepthWrite);
#endif

    if (existing) {
        RhiTexture* slot = texture(existing);
        if (slot) {
            // Re-filling the SAME slot on a resize/setSampleCount rebuild, like D3D12's own
            // `*slot = std::move(t); return existing;` -- STABLE so a cached handle never notices
            // depthBuffer_ was reallocated (VulkanDevice::depthTexHandle_'s comment). Unlike D3D12's
            // ComPtr RhiTexture, move-assignment does NOT release the outgoing slot's srvView (a real
            // VkImageView this factory owns, though the image/memory it viewed were never ours) --
            // must destroy it explicitly or every resize leaks one.
            // Retired, not destroyed immediately: a draw recorded before this call may still read it.
            if (slot->srvView) {
                VkImageView old = slot->srvView;
                retire([this, old]() { dev_->api().DestroyImageView(dev_->vkDevice(), old, nullptr); });
            }
            *slot = std::move(t);
            return existing;
        }
    }
    textures_.push_back(std::move(t));
    return static_cast<TextureHandle>(textures_.size());
}

namespace {
// The buffer for `d`; touches no factory state, so stagers call it on any thread.
bool makeBuffer(VulkanDevice& dev, const BufferDesc& d, RhiBuffer& b) {
    const bool hostVisible = d.kind == BufferKind::Upload || d.kind == BufferKind::Readback;
    const VkMemoryPropertyFlags required = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // A null CreateAccelerationStructureKHR is how this backend knows the extension is absent --
    // the loader leaves every unavailable entry point null (see VulkanApi).
    const bool rtAvailable = dev.api().CreateAccelerationStructureKHR != nullptr;
    if (!createBufferCommitted(dev, d.bytes, toVkBufferUsage(d, rtAvailable), required, b.buffer, b.memory, &b.address, d.debugName))
        return false;
    if (hostVisible) {
        if (!vkOk(dev.api().MapMemory(dev.vkDevice(), b.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.mapped)), "rhi buffer map")) {
            destroyBufferCommitted(dev, b.buffer, b.memory);
            b.buffer = VK_NULL_HANDLE;
            b.memory = VK_NULL_HANDLE;
            return false;
        }
        b.coherent = true;   // required flags above always ask for HOST_COHERENT; only findMemoryType's
                              // HOST_VISIBLE-only fallback (see createBufferCommitted) could make this
                              // untrue, and this engine's hardware is not expected to need it -- flagged, not assumed.
    }
    b.desc = d;
    b.desc.debugName = nullptr;
    if (d.debugName) b.debugName = d.debugName;
#if AVER_RHI_TRACK_STATE
    b.state = (d.kind == BufferKind::AccelStructure) ? ResourceState::AccelerationStructure : ResourceState::Common;
    b.stateFixed = d.kind == BufferKind::Upload || d.kind == BufferKind::AccelStructure;
#endif
    return true;
}

struct VulkanBufferStaging final : BufferStaging {
    std::shared_ptr<VulkanDevice::StagerState> state;
    RhiBuffer b;
    ~VulkanBufferStaging() override {   // unadopted: never seen by the GPU
        if (!state || !b.buffer) return;
        std::lock_guard<std::mutex> l(state->m);
        if (state->dev) destroyBufferCommitted(*state->dev, b.buffer, b.memory);
    }
};

class VulkanBufferStager final : public IBufferStager {
public:
    explicit VulkanBufferStager(std::shared_ptr<VulkanDevice::StagerState> state) : state_(std::move(state)) {}
    std::unique_ptr<BufferStaging> stage(const BufferDesc& d) override {
        if (d.bytes == 0) return nullptr;
        auto s = std::make_unique<VulkanBufferStaging>();
        std::lock_guard<std::mutex> l(state_->m);
        if (!state_->dev || !makeBuffer(*state_->dev, d, s->b)) return nullptr;
        s->state = state_;
        return s;
    }

private:
    std::shared_ptr<VulkanDevice::StagerState> state_;
};
} // namespace

BufferHandle VulkanResourceFactory::createBuffer(const BufferDesc& d) {
    collect();
    if (d.bytes == 0) { AVER_ERROR("[RHI.Vulkan] createBuffer of zero bytes"); return 0; }
    RhiBuffer b{};
    if (!makeBuffer(*dev_, d, b)) return 0;
    buffers_.push_back(std::move(b));
    return static_cast<BufferHandle>(buffers_.size());
}

std::shared_ptr<IBufferStager> VulkanResourceFactory::bufferStager() {
    if (!bufferStager_) bufferStager_ = std::make_shared<VulkanBufferStager>(dev_->stagerState());
    return bufferStager_;
}

BufferHandle VulkanResourceFactory::adoptBuffer(std::unique_ptr<BufferStaging> staged) {
    auto* s = dynamic_cast<VulkanBufferStaging*>(staged.get());
    if (!s || !s->b.buffer || !s->state || s->state->dev != dev_) return 0;
    collect();
    buffers_.push_back(std::move(s->b));
    s->b.buffer = VK_NULL_HANDLE;   // owned by the factory from here
    return static_cast<BufferHandle>(buffers_.size());
}

BufferHandle VulkanResourceFactory::adoptHostBuffer(VkBuffer buffer, VkDeviceMemory memory, VkDeviceAddress address,
                                                    u8* mapped, const BufferDesc& d) {
    collect();
    RhiBuffer b{};
    b.buffer = buffer;
    b.memory = memory;
    b.address = address;
    b.mapped = mapped;
    b.coherent = true;
    b.desc = d;
    b.desc.debugName = nullptr;
    if (d.debugName) b.debugName = d.debugName;
#if AVER_RHI_TRACK_STATE
    b.stateFixed = true;
#endif
    buffers_.push_back(std::move(b));
    return static_cast<BufferHandle>(buffers_.size());
}

// ================================================================================================
// 8. Shaders
// ================================================================================================
ShaderHandle VulkanResourceFactory::createShader(const ShaderDesc& d) {
    collect();

    // THE PRECOMPILED PATH -- SPIR-V somebody else produced (ShaderDesc::bytecode).
    // IT LEAVES RhiShader::source EMPTY, AND THAT IS THE WHOLE INTEGRATION: moduleForLayout's first
    // act is `if (s.source.empty()) return s.module;`, the existing behavior for a shader whose
    // source was released -- correct here for a stronger reason too: the SPIR-V's (set, binding)
    // numbers are BAKED IN, and this backend normally re-derives placement via a -fvk-bind-register
    // recompile (moduleForLayout), which cannot be done to bytecode. A caller supplying precompiled
    // SPIR-V is asserting its bindings already match the target PipelineLayout; nothing here checks
    // that, and Vulkan reports the mismatch at pipeline creation if wrong.
    if (d.precompiled()) {
        if ((d.stage == ShaderStage::Mesh || d.stage == ShaderStage::Amplification)
            && dev_->cachedCaps().meshShaderTier == 0) {
            AVER_WARN("[RHI.Vulkan] createShader (precompiled) needs mesh-shader hardware, which this "
                      "device reports as tier 0");
            return 0;
        }
        // SPIR-V is a stream of 32-bit words; vkCreateShaderModule requires pCode 4-byte aligned and
        // codeSize a multiple of 4. Refusing a bad size here names the caller's mistake (passing it on
        // would be UB in the driver); copying into a u32 vector gives the alignment for free --
        // ShaderDesc::bytecode promises a copy anyway, so this costs nothing extra.
        if ((d.bytecodeSize % sizeof(u32)) != 0) {
            AVER_ERROR("[RHI.Vulkan] createShader (precompiled): {} bytes is not a whole number of "
                       "SPIR-V words -- this is DXIL or a truncated module, not SPIR-V", d.bytecodeSize);
            return 0;
        }
        RhiShader s;
        s.stage = d.stage;
        s.spirv.resize(static_cast<usize>(d.bytecodeSize / sizeof(u32)));
        std::memcpy(s.spirv.data(), d.bytecode, static_cast<usize>(d.bytecodeSize));
        // The magic number, checked because the alignment test above passes for any 4-byte multiple
        // and DXIL is one. Getting this wrong is a caller handing the wrong one of a library's two blobs
        // to the wrong backend, which is worth catching by name rather than as a driver error.
        if (s.spirv.empty() || s.spirv[0] != 0x07230203u) {
            AVER_ERROR("[RHI.Vulkan] createShader (precompiled): bytecode does not begin with the "
                       "SPIR-V magic number -- wrong backend's blob?");
            return 0;
        }
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = s.spirv.size() * sizeof(u32);
        mi.pCode    = s.spirv.data();
        if (!vkOk(dev_->api().CreateShaderModule(dev_->vkDevice(), &mi, nullptr, &s.module),
                  "rhi precompiled shader module")) return 0;
        shaders_.push_back(std::move(s));
        return static_cast<ShaderHandle>(shaders_.size());
    }

    if (!d.source || !d.entry) { AVER_ERROR("[RHI.Vulkan] createShader without source or entry point"); return 0; }

    const bool isMeshFamily = d.stage == ShaderStage::Mesh || d.stage == ShaderStage::Amplification;
    if (isMeshFamily && dev_->cachedCaps().meshShaderTier == 0) {
        AVER_WARN("[RHI.Vulkan] createShader '{}' needs mesh-shader hardware, which this device reports as tier 0", d.entry);
        return 0;
    }
    u32 model = d.minShaderModel;
    if (isMeshFamily && model < 65) model = 65;
    if (model > dev_->cachedCaps().shaderModel) {
        AVER_WARN("[RHI.Vulkan] createShader '{}' wants SM {} but the device reports {}", d.entry, model, dev_->cachedCaps().shaderModel);
        return 0;
    }
    if (!dev_->cachedCaps().dxcAvailable) {
        AVER_WARN("[RHI.Vulkan] createShader '{}' needs DXC, which is unavailable -- this backend has "
                  "no FXC-equivalent fallback (SPIR-V comes only from DXC's -spirv path)", d.entry);
        return 0;
    }

    std::string src;
    if (d.prelude) src = d.prelude;
    src += d.source;
    RhiShader s;
    s.stage = d.stage;
    // RAW, before patchPerFrameSet below. moduleForLayout re-derives placement from the
    // PipelineLayout, and an annotation baked in here would compete with the -fvk-bind-register map
    // it supplies -- DXC would then have two answers for where b0 goes.
    s.source = src;

    // Same re-bind the fixed pipelines apply, same reason: a shader prepending sharedShaderPrelude()
    // carries the prelude's set-0 PerFrame block, where tableSetLayout() puts SRVs. Applied to every
    // shader (patchPerFrameSet no-ops without the block); affects only the LAYOUT-AGNOSTIC module below.
    patchPerFrameSet(src);

    s.entry = d.entry;
    if (d.defines) s.defines = d.defines;
    s.minShaderModel = model;
    if (!vulkanShaderCompiler().compile(src.c_str(), d.entry, d.stage, model, d.defines, s.spirv) || s.spirv.empty()) {
        AVER_ERROR("[RHI.Vulkan] createShader '{}' failed to compile", d.entry);
        return 0;
    }

    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = s.spirv.size() * sizeof(u32);
    mi.pCode = s.spirv.data();
    if (!vkOk(dev_->api().CreateShaderModule(dev_->vkDevice(), &mi, nullptr, &s.module), "rhi shader module")) return 0;

    shaders_.push_back(std::move(s));
    return static_cast<ShaderHandle>(shaders_.size());
}

void VulkanResourceFactory::destroyShader(ShaderHandle h) {
    RhiShader* s = shader(h);
    if (!s) return;
    // Immediate, not deferred: a VkPipeline stops referencing its VkShaderModule once
    // vkCreate*Pipelines returns (D3D12ResourceFactory::destroyShader resets its blob for the same
    // reason). A variant whose source needed no patch ALIASES s->module (moduleForLayout) -- must not double-destroy.
    for (RhiShaderVariant& v : s->variants)
        if (v.module && v.module != s->module) dev_->api().DestroyShaderModule(dev_->vkDevice(), v.module, nullptr);
    s->variants.clear();
    if (s->module) dev_->api().DestroyShaderModule(dev_->vkDevice(), s->module, nullptr);
    s->module = VK_NULL_HANDLE;
    s->spirv.clear();
    s->source.clear();
}

// ================================================================================================
// 9. Pipelines
// ================================================================================================
VkShaderModule VulkanResourceFactory::moduleForLayout(RhiShader& s, const PipelineLayout& layout, bool mesh,
                                                      bool instanced, const std::vector<u32>** outSpirv) {
    // A variant that needed no recompile ALIASES the base module and carries no SPIR-V of its own,
    // so the base's is the right answer for it too.
    auto give = [&](const RhiShaderVariant& v) {
        if (outSpirv) *outSpirv = v.spirv.empty() ? &s.spirv : &v.spirv;
        return v.module;
    };
    if (outSpirv) *outSpirv = &s.spirv;
    if (s.source.empty()) return s.module;    // destroyed, or never carried source
    const u32 key = shaderVariantKey(layout, mesh, instanced);
    for (const RhiShaderVariant& v : s.variants)
        if (v.key == key) return give(v);

    // Where every register this layout declares must land. Supplying this ALSO suppresses
    // -fvk-u-shift inside compile() -- the two are mutually exclusive -- which is why the map
    // includes the UAVs the shift used to handle rather than leaving them to it.
    VkRegisterBind binds[kMaxRegisterBinds];
    const u32 bindCount = buildRegisterBinds(layout, binds, kMaxRegisterBinds, instanced);

    // The map, when there is one, is the single authority on placement; the [[vk::binding]] half of
    // the patch would otherwise answer the same question a second time.
    std::string src = s.source;
    if (!patchCbuffersForLayout(src, layout, mesh, /*annotateDescriptors=*/bindCount == 0))
        return VK_NULL_HANDLE;

    RhiShaderVariant var;
    var.key = key;
    if (src == s.source && bindCount == 0) {
        // Nothing to re-place (no root-constant block, no table/sampler register): the layout-agnostic
        // module already is the right one, cached under this key so the next pipeline of the same
        // shape skips even the attempt. NOTE: var.module aliases s.module -- destroyShader relies on it.
        var.module = s.module;
        s.variants.push_back(std::move(var));
        return give(s.variants.back());
    }

    if (!vulkanShaderCompiler().compile(src.c_str(), s.entry.c_str(), s.stage, s.minShaderModel,
                                        s.defines.empty() ? nullptr : s.defines.c_str(), var.spirv,
                                        bindCount ? binds : nullptr, bindCount,
                                        /*quiet=*/bindCount != 0) ||
        var.spirv.empty()) {
        if (bindCount == 0) {
            AVER_ERROR("[RHI.Vulkan] '{}' compiles on its own but not with its pipeline's "
                       "push-constant blocks folded in", s.entry);
            return VK_NULL_HANDLE;
        }

        // WHEN THE LAYOUT DOES NOT DESCRIBE THE WHOLE SHADER, FALL BACK RATHER THAN FAIL.
        // DXC requires the -fvk-bind-register map to cover EVERY resource, but a PipelineLayout
        // doesn't always name every register a shader declares. Real cases, both outside the layout:
        //   - MESH GEOMETRY: the fixed mesh path declares gVerts/gIndices via its own g_meshGeomLayout.
        //   - DECLARED-BUT-UNUSED: VoxiShaders.hpp declares gVoxelTex/gVoxelSamp for every includer;
        //     a UAV-only pipeline like CSClear (uavCount 2, nothing else) uses neither, but DXC still
        //     wants them mapped. Here the fallback is EXACTLY equivalent: the misplaced resources are
        //     ones DXC eliminates anyway. (The INSTANCE buffer used to be a third case; now mapped
        //     directly to kVkSetInstances binding 0 via buildRegisterBinds, since `instanced` reaches
        //     here; a pipeline without `instanced` that still declares gInstanceWorlds can't occur --
        //     VoxiRenderer only defines AVER_INSTANCE_SRV for pipelines it also marks instanced.)
        // Inventing a binding would turn a compile error into a validation error -- worse, since the
        // pipeline would exist and be wrong. Retrying WITHOUT the map restores the pre-map placement:
        // imperfect but no regression, and layouts that ARE complete still get the correct one.
        // EXCEPT MESH STAGES, where that reasoning fails: gVerts/gIndices/MeshCB are what the shader
        // exists to read, survive to the SPIR-V, and land on set-0 bindings this layout never
        // declares. MEASURED with --debug-layer on MSVoxel:
        //   uses descriptor [Set 0, Binding 19, variable "gVerts"] which has a VkDescriptorType mismatch
        //   uses descriptor [Set 0, Binding 20, variable "gIndices"] but the binding was not declared
        //   uses descriptor [Set 0, Binding 5,  variable "MeshCB"]  which has a VkDescriptorType mismatch
        // and without the layer, no message at all -- AMD's LLPC calls abort() (0xC0000409, amdvlk64.dll).
        // Only a FEATURE mesh pipeline reaches here (the FIXED mesh path builds in VulkanDevice.cpp
        // against g_meshGeomLayout and is unaffected); refusing returns null, which every caller
        // already treats as "unavailable" (Voxi logs "mesh-shader voxelise variant unavailable; the
        // GS path stands in") -- a missing fast path beats a dead process.
        // THE REAL FIX: describe mesh geometry in the PipelineLayout so buildRegisterBinds can map
        // it, as was done for the instance buffer above. Until then, honest rather than lucky.
        if (s.stage == ShaderStage::Mesh || s.stage == ShaderStage::Amplification) {
            AVER_WARN("[RHI.Vulkan] '{}' is a mesh-stage shader whose PipelineLayout does not "
                      "describe its geometry registers; refusing the pipeline rather than building "
                      "one the driver aborts on. The caller's non-mesh path stands in.", s.entry);
            return VK_NULL_HANDLE;
        }
        AVER_WARN("[RHI.Vulkan] '{}' declares a register its PipelineLayout does not describe "
                  "(a resource declared by a shared header and not used here); "
                  "falling back to the default register->set mapping", s.entry);
        var.spirv.clear();
        std::string fallbackSrc = s.source;
        if (!patchCbuffersForLayout(fallbackSrc, layout, mesh, /*annotateDescriptors=*/true))
            return VK_NULL_HANDLE;
        if (!vulkanShaderCompiler().compile(fallbackSrc.c_str(), s.entry.c_str(), s.stage, s.minShaderModel,
                                            s.defines.empty() ? nullptr : s.defines.c_str(), var.spirv) ||
            var.spirv.empty()) {
            AVER_ERROR("[RHI.Vulkan] '{}' compiles on its own but not with its pipeline's constant "
                       "blocks lowered", s.entry);
            // The patched HLSL, when AVER_VK_DUMP_SPIRV names a directory. A failure here is in
            // text this backend GENERATED, so the generated text is the only useful evidence.
            if (const char* dir = std::getenv("AVER_VK_DUMP_SPIRV")) {
                const std::string path = std::string(dir) + "/" + s.entry + ".failed.hlsl";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(fallbackSrc.data(), 1, fallbackSrc.size(), f);
                    std::fclose(f);
                    AVER_WARN("[RHI.Vulkan] wrote the generated source to {}", path);
                }
            }
            return VK_NULL_HANDLE;
        }
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = var.spirv.size() * sizeof(u32);
    mi.pCode = var.spirv.data();
    if (!vkOk(dev_->api().CreateShaderModule(dev_->vkDevice(), &mi, nullptr, &var.module), "rhi shader module (push-constant variant)"))
        return VK_NULL_HANDLE;
    s.variants.push_back(std::move(var));
    return give(s.variants.back());
}

PipelineHandle VulkanResourceFactory::createGraphicsPipeline(const GraphicsPipelineDesc& d) {
    collect();
    if ((d.vs == 0) == (d.ms == 0)) { AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline needs exactly one of vs / ms"); return 0; }
    if (d.as != 0 && d.ms == 0) { AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline: an amplification shader (as) needs a mesh shader (ms) alongside it"); return 0; }

    RhiShader* vs = shader(d.vs);
    RhiShader* gs = shader(d.gs);
    RhiShader* ms = shader(d.ms);
    RhiShader* ps = shader(d.ps);
    RhiShader* as = shader(d.as);
    if ((d.vs && !vs) || (d.gs && !gs) || (d.ms && !ms) || (d.ps && !ps) || (d.as && !as)) {
        AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline given an invalid shader handle");
        return 0;
    }

    const bool mesh = d.ms != 0;

    // THE MODULES BEFORE THE LAYOUT -- see createComputePipeline for why this order, and
    // reflectTableSlotKinds for what reading the wrong module cost.
    RhiShader* const shaders[5] = {vs, gs, ms, ps, as};
    VkShaderModule stageModules[5] = {};
    const std::vector<u32>* stageSpirv[5] = {};
    for (u32 i = 0; i < 5; ++i) {
        if (!shaders[i]) continue;
        stageModules[i] = moduleForLayout(*shaders[i], d.layout, mesh, d.instanced, &stageSpirv[i]);
        if (!stageModules[i]) return 0;
    }

    PendingTableKinds saved = gPendingKinds;
    reflectTableSlotKinds(stageSpirv, 5, d.layout, gPendingKinds);
    const DescriptorLayoutEntry* entry = descriptorLayout(d.layout, mesh, d.instanced);
    gPendingKinds = saved;
    if (!entry) return 0;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = (d.fill == FillMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = (d.cull == CullMode::Back) ? VK_CULL_MODE_BACK_BIT : (d.cull == CullMode::Front) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // D3D12's DepthClipEnable=TRUE means "clip normally"; Vulkan's depthClampEnable=TRUE means the
    // OPPOSITE ("clamp instead of clip") -- the two booleans are inverted twins, not synonyms.
    raster.depthClampEnable = d.depthClip ? VK_FALSE : VK_TRUE;
    raster.depthBiasEnable = (d.depthBias != 0.0f || d.slopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = d.depthBias;
    raster.depthBiasSlopeFactor = d.slopeScaledDepthBias;
    raster.lineWidth = 1.0f;

    VkPipelineRasterizationConservativeStateCreateInfoEXT consState{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
    if (d.conservativeRaster && dev_->cachedCaps().conservativeRaster) {
        consState.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
        raster.pNext = &consState;
    }

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = d.depth.test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = d.depth.write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = toVkCompareOp(d.depth.op);

    const VkPipelineColorBlendAttachmentState blendAttachment = toVkBlendAttachment(d.blend);
    const u32 rtCount = d.renderTargetCount < 4 ? d.renderTargetCount : 4;
    VkPipelineColorBlendAttachmentState blendAttachments[4];
    for (u32 i = 0; i < rtCount; ++i) blendAttachments[i] = blendAttachment;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = rtCount;
    blend.pAttachments = blendAttachments;

    const u32 samples = d.sampleCount ? d.sampleCount : 1;
    VkPipelineMultisampleStateCreateInfo ms_{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms_.rasterizationSamples = toVkSampleCount(samples);

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkFormat colorFormats[4];
    for (u32 i = 0; i < rtCount; ++i) colorFormats[i] = toVkFormat(d.renderTargets[i]);
    VkPipelineRenderingCreateInfo rc{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rc.colorAttachmentCount = rtCount;
    rc.pColorAttachmentFormats = colorFormats;
    rc.depthAttachmentFormat = toVkFormat(d.depthFormat);

    // Entry-point names must stay alive through vkCreateGraphicsPipelines; kept in this function's
    // own scope, not RhiShader's (which retains none -- see this file's banner).
    std::string names[5];
    bool stageFailed = false;
    // The module built for this shader above, by identity -- addStage is handed RhiShader*s.
    auto moduleFor = [&](const RhiShader* sh) {
        for (u32 i = 0; i < 5; ++i) if (shaders[i] == sh) return stageModules[i];
        return VkShaderModule{VK_NULL_HANDLE};
    };
    VkPipelineShaderStageCreateInfo stageInfos[5];
    u32 stageCount = 0;
    auto addStage = [&](RhiShader* sh, VkShaderStageFlagBits flag) {
        if (!sh) return;
        names[stageCount] = spirvEntryPointName(sh->spirv);
        VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si.stage = flag;
        si.module = moduleFor(sh);
        if (!si.module) { stageFailed = true; return; }
        si.pName = names[stageCount].c_str();
        stageInfos[stageCount++] = si;
    };

    RhiPipeline p{};
    p.mesh = mesh;
    p.amplification = d.as != 0;
    p.instanced = d.instanced;
    p.layoutEntry = entry;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rc;
    pi.pRasterizationState = &raster;
    pi.pDepthStencilState = &depth;
    pi.pColorBlendState = &blend;
    pi.pMultisampleState = &ms_;
    pi.pDynamicState = &dyn;
    pi.pViewportState = &vp;
    pi.layout = entry->pipelineLayout;
    pi.renderPass = VK_NULL_HANDLE;   // dynamic rendering (core at 1.3): no VkRenderPass object anywhere in this backend

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkVertexInputBindingDescription binding{};
    VkVertexInputAttributeDescription meshAttribs[3];
    VkVertexInputAttributeDescription layoutAttribs[kMaxVertexAttribs];

    if (mesh) {
        addStage(as, VK_SHADER_STAGE_TASK_BIT_EXT);
        addStage(ms, VK_SHADER_STAGE_MESH_BIT_EXT);
        addStage(ps, VK_SHADER_STAGE_FRAGMENT_BIT);
        // Mesh pipelines carry no fixed-function vertex-input stage at all -- these members are
        // ignored (and legally null) whenever a mesh shader stage is present, per VK_EXT_mesh_shader.
        pi.pVertexInputState = nullptr;
        pi.pInputAssemblyState = nullptr;
    } else {
        addStage(vs, VK_SHADER_STAGE_VERTEX_BIT);
        addStage(gs, VK_SHADER_STAGE_GEOMETRY_BIT);
        addStage(ps, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (d.vertexLayout.attribCount > 0) {
            binding = {0, d.vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
            const u32 n = buildVertexInputAttributes(d.vertexLayout, layoutAttribs);
            vin.vertexBindingDescriptionCount = 1;
            vin.pVertexBindingDescriptions = &binding;
            vin.vertexAttributeDescriptionCount = n;
            vin.pVertexAttributeDescriptions = layoutAttribs;
        } else {
            meshVertexInputState(binding, meshAttribs);
            vin.vertexBindingDescriptionCount = 1;
            vin.pVertexBindingDescriptions = &binding;
            vin.vertexAttributeDescriptionCount = 3;
            vin.pVertexAttributeDescriptions = meshAttribs;
        }
        pi.pVertexInputState = &vin;
        pi.pInputAssemblyState = &ia;
    }
    if (stageFailed) return 0;
    pi.stageCount = stageCount;
    pi.pStages = stageInfos;

    if (!vkOk(dev_->api().CreateGraphicsPipelines(dev_->vkDevice(), VK_NULL_HANDLE, 1, &pi, nullptr, &p.pipeline), "rhi graphics pipeline"))
        return 0;
    pipelines_.push_back(p);
    return static_cast<PipelineHandle>(pipelines_.size());
}

PipelineHandle VulkanResourceFactory::createComputePipeline(const ComputePipelineDesc& d) {
    collect();
    RhiShader* cs = shader(d.cs);
    if (!cs || cs->stage != ShaderStage::Compute) {
        AVER_ERROR("[RHI.Vulkan] createComputePipeline given a handle that is not a compute shader");
        return 0;
    }

    // THE MODULE BEFORE THE LAYOUT. reflectTableSlotKinds must read the per-layout variant (see its
    // own comment), and building that needs only the layout's COUNTS -- never its slot kinds -- so
    // there is no circularity here, just an order that has to be this way round.
    const std::vector<u32>* csSpirv = nullptr;
    const VkShaderModule csModule = moduleForLayout(*cs, d.layout, false, /*instanced=*/false, &csSpirv);
    if (!csModule) return 0;

    const std::vector<u32>* stageSpirv[1] = {csSpirv};
    PendingTableKinds saved = gPendingKinds;
    reflectTableSlotKinds(stageSpirv, 1, d.layout, gPendingKinds);
    const DescriptorLayoutEntry* entry = descriptorLayout(d.layout, false, /*instanced=*/false);
    gPendingKinds = saved;
    if (!entry) return 0;

    const std::string entryName = spirvEntryPointName(cs->spirv);
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    si.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    si.module = csModule;
    si.pName = entryName.c_str();

    RhiPipeline p{};
    p.compute = true;
    p.layoutEntry = entry;

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = si;
    ci.layout = entry->pipelineLayout;
    if (!vkOk(dev_->api().CreateComputePipelines(dev_->vkDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline), "rhi compute pipeline"))
        return 0;
    pipelines_.push_back(p);
    return static_cast<PipelineHandle>(pipelines_.size());
}

void VulkanResourceFactory::destroyPipeline(PipelineHandle h) {
    RhiPipeline* p = pipeline(h);
    if (!p) return;
    VkPipeline pipe = p->pipeline;
    retire([this, pipe]() { dev_->api().DestroyPipeline(dev_->vkDevice(), pipe, nullptr); });
    p->pipeline = VK_NULL_HANDLE;
    p->layoutEntry = nullptr;
    collect();
}

// ================================================================================================
// 10. Binding sets
// ================================================================================================
// Applies one recorded slot to one descriptor set. The single place a BindingSlotState is turned
// back into a VkWriteDescriptorSet, so the write path and the replay path cannot drift.
namespace {
void applySlot(const VulkanApi& api, VkDevice device, VkDescriptorSet dst, u32 binding,
               const BindingSlotState& st) {
    if (st.type == VK_DESCRIPTOR_TYPE_MAX_ENUM || dst == VK_NULL_HANDLE) return;
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = dst;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = st.type;
    if (st.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        if (st.accel == VK_NULL_HANDLE) return;
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures = &st.accel;
        w.pNext = &asInfo;
    } else if (st.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        w.pBufferInfo = &st.buffer;
    } else {
        w.pImageInfo = &st.image;
    }
    api.UpdateDescriptorSets(device, 1, &w, 0, nullptr);
}
}  // namespace

void VulkanResourceFactory::writeBindingSlot(RhiBindingSet& s, bool isUav, u32 slot, const BindingSlotState& st) {
    // THE INVARIANT THE RING RESTS ON: a ring by FRAME cannot save a set written after it was bound
    // in the SAME frame -- the set at fault is the one this frame is already using. Measured as
    // zero when the ring was built; if it ever fires, the ring is not enough and this needs to
    // become a per-bind allocation instead.
    if (s.lastBoundSerial == dev_->frameSerial())
        AVER_WARN("[RHI.Vulkan] binding set slot {} written AFTER the set was bound in frame {} -- the "
                  "frame ring cannot cover this; the write will be seen by a command buffer that has "
                  "already bound the set", slot, dev_->frameSerial());

    BindingSlotState& dstSlot = isUav ? s.uavSlots[slot] : s.srvSlots[slot];
    // The previous one-off view, if this slot owned one, is dead the moment nothing can replay it.
    if (dstSlot.ownedView != VK_NULL_HANDLE && dstSlot.ownedView != st.ownedView) {
        const VkImageView dead = dstSlot.ownedView;
        retire([this, dead]() { dev_->api().DestroyImageView(dev_->vkDevice(), dead, nullptr); });
    }
    dstSlot = st;

    const u32 binding = isUav ? (kVkUavBindingBase + slot) : slot;
    const u32 cur = dev_->frameIndexInFlight();
    applySlot(dev_->api(), dev_->vkDevice(), s.sets[cur], binding, dstSlot);
    // Every OTHER ring slot has now missed this write. They are brought up to date by
    // bindingSetForFrame, when their frame comes round and their fence has been waited on.
    for (u32 f = 0; f < kFrameCount; ++f)
        if (f != cur) s.staleMask |= (1u << f);
}

VkDescriptorSet VulkanResourceFactory::bindingSetForFrame(RhiBindingSet& s) {
    const u32 cur = dev_->frameIndexInFlight();
    if (s.staleMask & (1u << cur)) {
        // Safe here and only here: beginFrame() waited on this slot's timeline value, so no pending
        // command buffer still references it, and nothing has bound it yet this frame.
        for (u32 i = 0; i < s.srvCount && i < kMaxBindingSlots; ++i)
            applySlot(dev_->api(), dev_->vkDevice(), s.sets[cur], i, s.srvSlots[i]);
        for (u32 i = 0; i < s.uavCount && i < kMaxBindingSlots; ++i)
            applySlot(dev_->api(), dev_->vkDevice(), s.sets[cur], kVkUavBindingBase + i, s.uavSlots[i]);
        s.staleMask &= ~(1u << cur);
    }
    return s.sets[cur];
}

BindingSetHandle VulkanResourceFactory::createBindingSet(const BindingSetDesc& d) {
    collect();
    const u32 count = d.srvCount + d.uavCount;
    if (count == 0) { AVER_ERROR("[RHI.Vulkan] createBindingSet declaring no slots"); return 0; }
    if (d.srvCount > kMaxBindingSlots || d.uavCount > kMaxBindingSlots) {
        AVER_ERROR("[RHI.Vulkan] createBindingSet declares {} SRV / {} UAV slots, over the {} limit", d.srvCount, d.uavCount, kMaxBindingSlots);
        return 0;
    }

    const VkDescriptorSetLayout layout = tableSetLayout(d.srvCount, d.uavCount, d.srvKinds, d.uavKinds);
    if (layout == VK_NULL_HANDLE) return 0;

    RhiBindingSet s{};
    s.layout = layout;
    s.srvCount = d.srvCount;
    s.uavCount = d.uavCount;
    s.srvBaseRegister = d.srvBaseRegister;
    s.uavBaseRegister = d.uavBaseRegister;
    for (u32 i = 0; i < kMaxBindingSlots; ++i) { s.srvKinds[i] = d.srvKinds[i]; s.uavKinds[i] = d.uavKinds[i]; }

    // kFrameCount sets of the SAME layout -- the ring. The pool is sized at 4096 sets against a
    // handful of binding sets per feature, so multiplying by kFrameCount is not close to a problem.
    VkDescriptorSetLayout ringLayouts[kFrameCount];
    for (u32 f = 0; f < kFrameCount; ++f) ringLayouts[f] = layout;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = kFrameCount;
    ai.pSetLayouts = ringLayouts;
    if (!vkOk(dev_->api().AllocateDescriptorSets(dev_->vkDevice(), &ai, s.sets), "rhi binding set ring")) return 0;

    s.alive = true;
    nullFill(s);
    bindingSets_.push_back(s);
    return static_cast<BindingSetHandle>(bindingSets_.size());
}

void VulkanResourceFactory::destroyBindingSet(BindingSetHandle h) {
    RhiBindingSet* s = bindingSet(h);
    if (!s) return;
    std::array<VkDescriptorSet, kFrameCount> ring{};
    for (u32 f = 0; f < kFrameCount; ++f) ring[f] = s->sets[f];
    retire([this, ring]() {
        std::array<VkDescriptorSet, kFrameCount> local = ring;
        dev_->api().FreeDescriptorSets(dev_->vkDevice(), descriptorPool_, kFrameCount, local.data());
    });
    // Every one-off mip view a slot took ownership of dies with the set.
    for (u32 i = 0; i < kMaxBindingSlots; ++i) {
        for (const BindingSlotState* st : {&s->srvSlots[i], &s->uavSlots[i]}) {
            if (st->ownedView == VK_NULL_HANDLE) continue;
            const VkImageView dead = st->ownedView;
            retire([this, dead]() { dev_->api().DestroyImageView(dev_->vkDevice(), dead, nullptr); });
        }
        s->srvSlots[i] = BindingSlotState{};
        s->uavSlots[i] = BindingSlotState{};
    }
    s->alive = false;
    for (u32 f = 0; f < kFrameCount; ++f) s->sets[f] = VK_NULL_HANDLE;
    collect();
}

// Writes a valid dummy descriptor into every DECLARED slot of every RING slot.
// All of them, only at creation: a replay (bindingSetForFrame) rewrites only what was explicitly
// set, so an unwritten slot must be made valid here, in each ring slot separately (distinct
// descriptor sets). Safe only here: nothing can be referencing a set that is still being created.
void VulkanResourceFactory::nullFill(const RhiBindingSet& s) {
    if (!ensureNullResources(*dev_)) {
        AVER_ERROR("[RHI.Vulkan] nullFill: dummy null resources could not be built; every declared "
                  "slot in this binding set is left UNWRITTEN, which is undefined to read");
        return;
    }
    const bool rtSupported = dev_->cachedCaps().rayTracingTier != 0;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> asInfos;
    imageInfos.reserve(s.srvCount + s.uavCount);
    bufferInfos.reserve(s.srvCount + s.uavCount);
    asInfos.reserve(s.srvCount);
    writes.reserve(s.srvCount + s.uavCount);

    for (u32 i = 0; i < s.srvCount; ++i) {
        const SlotKind kind = s.srvKinds[i];
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = VK_NULL_HANDLE;   // filled per ring slot below
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = toVkSrvDescriptorType(kind, rtSupported);
        if (kind == SlotKind::StructuredBuffer) {
            bufferInfos.push_back({gNull.buffer, 0, VK_WHOLE_SIZE});
            w.pBufferInfo = &bufferInfos.back();
            writes.push_back(w);
        } else if (kind == SlotKind::AccelerationStructure && rtSupported) {
            if (!gNull.dummyTlas) continue;   // ensureNullResources logged why, if it failed
            asInfos.push_back(VkWriteDescriptorSetAccelerationStructureKHR{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR});
            asInfos.back().accelerationStructureCount = 1;
            asInfos.back().pAccelerationStructures = &gNull.dummyTlas;
            w.pNext = &asInfos.back();
            writes.push_back(w);
        } else {
            const bool tex3D = (kind == SlotKind::Texture3D);
            imageInfos.push_back({VK_NULL_HANDLE, tex3D ? gNull.view3D : gNull.view2D, VK_IMAGE_LAYOUT_GENERAL});
            w.pImageInfo = &imageInfos.back();
            writes.push_back(w);
        }
    }
    for (u32 i = 0; i < s.uavCount; ++i) {
        SlotKind kind = s.uavKinds[i];
        if (kind == SlotKind::AccelerationStructure) kind = SlotKind::Texture2D;   // logged already, in toVkUavDescriptorType
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = VK_NULL_HANDLE;   // filled per ring slot below
        w.dstBinding = kVkUavBindingBase + i;
        w.descriptorCount = 1;
        w.descriptorType = toVkUavDescriptorType(kind);
        if (kind == SlotKind::StructuredBuffer) {
            bufferInfos.push_back({gNull.buffer, 0, VK_WHOLE_SIZE});
            w.pBufferInfo = &bufferInfos.back();
        } else {
            const bool tex3D = (kind == SlotKind::Texture3D);
            imageInfos.push_back({VK_NULL_HANDLE, tex3D ? gNull.view3D : gNull.view2D, VK_IMAGE_LAYOUT_GENERAL});
            w.pImageInfo = &imageInfos.back();
        }
        writes.push_back(w);
    }

    // NOTE: push_back above can reallocate imageInfos/bufferInfos/asInfos and invalidate EARLIER
    // entries' addresses that `writes[]` already captured -- reserve() at the top sizes each vector
    // to its worst case up front specifically to rule that out; do not remove those calls without
    // re-deriving this. ONCE PER RING SLOT, for the reason in this function's own header comment above.
    if (writes.empty()) return;
    for (u32 f = 0; f < kFrameCount; ++f) {
        if (s.sets[f] == VK_NULL_HANDLE) continue;
        for (VkWriteDescriptorSet& w : writes) w.dstSet = s.sets[f];
        dev_->api().UpdateDescriptorSets(dev_->vkDevice(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
}

// ================================================================================================
// 11. Acceleration structures
// ================================================================================================
namespace {
bool accelStructureSupported(VulkanDevice& dev) {
    return dev.cachedCaps().rayTracingTier != 0 && dev.api().CreateAccelerationStructureKHR &&
           dev.api().GetAccelerationStructureBuildSizesKHR && dev.api().GetAccelerationStructureDeviceAddressKHR;
}
} // namespace

BlasHandle VulkanResourceFactory::createBlasImpl(MeshHandle mesh, bool allowUpdate) {
    collect();
    if (!accelStructureSupported(*dev_)) { AVER_WARN("[RHI.Vulkan] createBlas without ray-tracing support"); return 0; }
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.Vulkan] createBlas with an invalid mesh handle"); return 0; }
    if (!dev_->meshes_[mesh - 1].alive) { AVER_WARN("[RHI.Vulkan] createBlas for destroyed mesh {}", mesh); return 0; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    if (m.indexCount == 0) { AVER_ERROR("[RHI.Vulkan] createBlas for a mesh with no indices"); return 0; }

    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();

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
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    // ALLOW_UPDATE_BIT_KHR must be in the size query AND every later build/update for this
    // structure to stay updatable -- Vulkan sizes (and validates an update against) the flags the
    // structure was queried/built with, same as D3D12's ALLOW_UPDATE requirement.
    const VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        (allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = flags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;

    const u32 primCount = m.indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    api.GetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &primCount, &sizeInfo);
    // Scratch sized for whichever of a build or an update is larger -- this ONE scratch buffer
    // serves both buildBlas (full BUILD) and refitBlas (in-place UPDATE) for the structure's whole
    // life; updateScratchSize is 0 (and irrelevant) when allowUpdate is false.
    const VkDeviceSize scratchSize = allowUpdate ? std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize)
                                                  : sizeInfo.buildScratchSize;

    RhiBlas b{};
    b.mesh = mesh;
    b.allowUpdate = allowUpdate;
    b.asSize = sizeInfo.accelerationStructureSize;
    b.scratchSize = scratchSize;
    if (!createBufferCommitted(*dev_, sizeInfo.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.asBuffer, b.asMemory, nullptr, "rhi BLAS buffer") ||
        !createBufferCommitted(*dev_, scratchSize,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.scratchBuffer, b.scratchMemory, nullptr, "rhi BLAS scratch")) {
        AVER_ERROR("[RHI.Vulkan] createBlas allocation failed");
        destroyBufferCommitted(*dev_, b.asBuffer, b.asMemory);
        destroyBufferCommitted(*dev_, b.scratchBuffer, b.scratchMemory);
        return 0;
    }

    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = b.asBuffer;
    aci.size = sizeInfo.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (!vkOk(api.CreateAccelerationStructureKHR(device, &aci, nullptr, &b.as), "rhi BLAS create")) {
        destroyBufferCommitted(*dev_, b.asBuffer, b.asMemory);
        destroyBufferCommitted(*dev_, b.scratchBuffer, b.scratchMemory);
        return 0;
    }
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure = b.as;
    b.asAddress = api.GetAccelerationStructureDeviceAddressKHR(device, &dai);

    // NOT built here -- buildBlas() (VulkanRenderContext.cpp) records the actual
    // vkCmdBuildAccelerationStructuresKHR, matching D3D12's own split between allocation
    // (D3D12ResourceFactory::createBlas) and the build command (D3D12RenderContext::buildBlas).
    b.built = false;
    blases_.push_back(b);
    return static_cast<BlasHandle>(blases_.size());
}

BlasHandle VulkanResourceFactory::createBlas(MeshHandle mesh) { return createBlasImpl(mesh, false); }

// One structure over several meshes -- see IResourceFactory::createBlasMulti. Sized by one size query
// over every geometry (vkMultiBlasGeometry, the description recordBlasBuildMulti builds from); built
// later by buildBlas like any other. Mirrors D3D12ResourceFactory::createBlasMulti.
BlasHandle VulkanResourceFactory::createBlasMulti(const BlasGeometry* geometries, u32 count) {
    collect();
    if (!accelStructureSupported(*dev_)) { AVER_WARN("[RHI.Vulkan] createBlasMulti without ray-tracing support"); return 0; }
    if (!geometries || count == 0) { AVER_ERROR("[RHI.Vulkan] createBlasMulti with no geometry"); return 0; }
    std::vector<VkAccelerationStructureGeometryKHR> geoms(count);
    std::vector<u32> primCounts(count);
    for (u32 i = 0; i < count; ++i) {
        const MeshHandle h = geometries[i].mesh;
        if (h == 0 || h > dev_->meshes_.size() || !dev_->meshes_[h - 1].alive || dev_->meshes_[h - 1].indexCount == 0) {
            AVER_ERROR("[RHI.Vulkan] createBlasMulti: geometry {} names an invalid, destroyed or index-less mesh", i);
            return 0;
        }
        geoms[i] = vkMultiBlasGeometry(dev_->meshes_[h - 1], geometries[i].opaque);
        const u32 ic = dev_->meshes_[h - 1].indexCount;
        if (geometries[i].firstIndex >= ic) {
            AVER_ERROR("[RHI.Vulkan] createBlasMulti: geometry {} starts past its mesh's indices", i);
            return 0;
        }
        const u32 count = geometries[i].indexCount ? std::min(geometries[i].indexCount, ic - geometries[i].firstIndex)
                                                    : ic - geometries[i].firstIndex;
        primCounts[i] = count / 3;
    }

    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = count;
    bi.pGeometries = geoms.data();
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    api.GetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, primCounts.data(), &sizeInfo);

    RhiBlas b{};
    b.mesh = geometries[0].mesh;
    b.geometries.assign(geometries, geometries + count);
    b.asSize = sizeInfo.accelerationStructureSize;
    b.scratchSize = sizeInfo.buildScratchSize;
    if (!createBufferCommitted(*dev_, sizeInfo.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.asBuffer, b.asMemory, nullptr, "rhi BLAS buffer") ||
        !createBufferCommitted(*dev_, sizeInfo.buildScratchSize,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b.scratchBuffer, b.scratchMemory, nullptr, "rhi BLAS scratch")) {
        AVER_ERROR("[RHI.Vulkan] createBlasMulti allocation failed");
        destroyBufferCommitted(*dev_, b.asBuffer, b.asMemory);
        destroyBufferCommitted(*dev_, b.scratchBuffer, b.scratchMemory);
        return 0;
    }
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = b.asBuffer;
    aci.size = sizeInfo.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (!vkOk(api.CreateAccelerationStructureKHR(device, &aci, nullptr, &b.as), "rhi multi-geometry BLAS create")) {
        destroyBufferCommitted(*dev_, b.asBuffer, b.asMemory);
        destroyBufferCommitted(*dev_, b.scratchBuffer, b.scratchMemory);
        return 0;
    }
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure = b.as;
    b.asAddress = api.GetAccelerationStructureDeviceAddressKHR(device, &dai);
    blases_.push_back(std::move(b));
    return static_cast<BlasHandle>(blases_.size());
}

// UPDATABLE twin of createBlas -- see IResourceFactory's contract comment above
// createBlasUpdatable. Same allocation, plus ALLOW_UPDATE_BIT_KHR and a build-AND-update-sized
// scratch, so VulkanRenderContext::refitBlas can update this BLAS in place instead of rebuilding it
// from scratch every frame (the compute-skinned-mesh case RHIResources.hpp motivates this with).
BlasHandle VulkanResourceFactory::createBlasUpdatable(MeshHandle mesh) { return createBlasImpl(mesh, true); }

TlasHandle VulkanResourceFactory::createTlasImpl(u32 maxInstances, bool allowUpdate) {
    collect();
    if (!accelStructureSupported(*dev_)) { AVER_WARN("[RHI.Vulkan] createTlas without ray-tracing support"); return 0; }
    if (maxInstances == 0) { AVER_ERROR("[RHI.Vulkan] createTlas for zero instances"); return 0; }

    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

    // See createBlasImpl's identical comment: the size query's flags must be the flags this
    // structure will actually be built/updated with.
    const VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        (allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = flags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    api.GetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxInstances, &sizeInfo);
    // Scratch sized for whichever of a build or an update is larger -- see createBlasImpl.
    const VkDeviceSize scratchSize = allowUpdate ? std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize)
                                                  : sizeInfo.buildScratchSize;

    RhiTlas t{};
    t.maxInstances = maxInstances;
    t.allowUpdate = allowUpdate;
    t.asSize = sizeInfo.accelerationStructureSize;
    t.scratchSize = scratchSize;
    if (!createBufferCommitted(*dev_, sizeInfo.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.asBuffer, t.asMemory, nullptr, "rhi TLAS buffer") ||
        !createBufferCommitted(*dev_, scratchSize,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.scratchBuffer, t.scratchMemory, nullptr, "rhi TLAS scratch")) {
        AVER_ERROR("[RHI.Vulkan] createTlas allocation failed");
        destroyBufferCommitted(*dev_, t.asBuffer, t.asMemory);
        destroyBufferCommitted(*dev_, t.scratchBuffer, t.scratchMemory);
        return 0;
    }

    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = t.asBuffer;
    aci.size = sizeInfo.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (!vkOk(api.CreateAccelerationStructureKHR(device, &aci, nullptr, &t.as), "rhi TLAS create")) {
        destroyBufferCommitted(*dev_, t.asBuffer, t.asMemory);
        destroyBufferCommitted(*dev_, t.scratchBuffer, t.scratchMemory);
        return 0;
    }
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure = t.as;
    t.asAddress = api.GetAccelerationStructureDeviceAddressKHR(device, &dai);

    const u64 bytes = static_cast<u64>(maxInstances) * sizeof(VkAccelerationStructureInstanceKHR);
    for (u32 i = 0; i < kFrameCount; ++i) {
        // TRANSFER_SRC: with a static prefix set, recordTlasBuild copies these in behind it instead of
        // building from them directly.
        if (!createBufferCommitted(*dev_, bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   t.instanceBuffers[i], t.instanceMemory[i], nullptr, "rhi TLAS instances")) {
            AVER_ERROR("[RHI.Vulkan] createTlas instance buffer {} failed", i);
            return 0;
        }
        vkOk(api.MapMemory(device, t.instanceMemory[i], 0, bytes, 0, reinterpret_cast<void**>(&t.instancePtr[i])), "rhi TLAS instance map");
    }

    tlases_.push_back(t);
    return static_cast<TlasHandle>(tlases_.size());
}

TlasHandle VulkanResourceFactory::createTlas(u32 maxInstances) { return createTlasImpl(maxInstances, false); }

// UPDATABLE twin of createTlas -- see IResourceFactory's contract comment above
// createTlasUpdatable. Same allocation, plus ALLOW_UPDATE_BIT_KHR and a build-AND-update-sized
// scratch, so VulkanRenderContext::refitTlas can update this TLAS in place instead of rebuilding it
// from scratch every frame.
TlasHandle VulkanResourceFactory::createTlasUpdatable(u32 maxInstances) { return createTlasImpl(maxInstances, true); }

// The static prefix -- see IResourceFactory::setTlasStaticInstances; mirrors D3D12ResourceFactory's.
// Everything is validated and allocated BEFORE anything is replaced, so a refusal leaves the TLAS
// exactly as it was.
bool VulkanResourceFactory::setTlasStaticInstances(TlasHandle h, const TlasInstance* instances, u32 count) {
    collect();
    RhiTlas* t = tlas(h);
    if (!t) { AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances with an invalid handle"); return false; }
    if (!accelStructureSupported(*dev_)) { AVER_WARN("[RHI.Vulkan] setTlasStaticInstances without ray-tracing support"); return false; }
    if (count == 0 && t->staticCount == 0 && !t->staticDescs) return true;   // no prefix to remove
    if (count && !instances) { AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances: {} instances and no array", count); return false; }
    if (static_cast<u64>(count) + t->maxInstances > kMaxTlasInstances) {
        AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances: {} static + {} per-frame instances is past the {} one "
                   "TLAS may hold -- refused, the previous prefix kept", count, t->maxInstances, kMaxTlasInstances);
        return false;
    }

    // REFUSED, NOT COMPACTED -- a shader finds a prefix instance by its slot index; see D3D12's twin.
    std::vector<BlasHandle> distinct;
    for (u32 i = 0; i < count; ++i) {
        const RhiBlas* b = blas(instances[i].blas);
        if (!b || !b->as) {
            AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances: instance {} names an invalid BLAS -- refused whole, "
                       "the previous prefix kept", i);
            return false;
        }
        if (instances[i].instanceId > kMaxTlasInstanceId) {
            AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances: instance {} has id {} which does not fit in 24 bits "
                       "-- refused whole, the previous prefix kept", i, instances[i].instanceId);
            return false;
        }
        if (distinct.empty() || distinct.back() != instances[i].blas) distinct.push_back(instances[i].blas);
    }
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());

    // The structure and scratch for prefix + the TLAS's own per-frame maximum, queried with the flags it
    // is built with (see createTlasImpl).
    const VulkanApi& api = dev_->api();
    VkDevice device = dev_->vkDevice();
    u32 total = count + t->maxInstances;
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
               (t->allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    api.GetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &total, &sizeInfo);
    const VkDeviceSize scratchSize = t->allowUpdate ? std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize)
                                                    : sizeInfo.buildScratchSize;

    VkBuffer asBuffer = VK_NULL_HANDLE, scratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory asMemory = VK_NULL_HANDLE, scratchMemory = VK_NULL_HANDLE;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    const auto releaseNew = [&]() {
        if (as) api.DestroyAccelerationStructureKHR(device, as, nullptr);
        destroyBufferCommitted(*dev_, asBuffer, asMemory);
        destroyBufferCommitted(*dev_, scratchBuffer, scratchMemory);
    };
    if (!createBufferCommitted(*dev_, sizeInfo.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, asBuffer, asMemory, nullptr, "rhi TLAS buffer") ||
        !createBufferCommitted(*dev_, scratchSize,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchBuffer, scratchMemory, nullptr, "rhi TLAS scratch")) {
        AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances: {:.1f} MiB structure / {:.1f} MiB scratch for {} instances "
                   "could not be allocated -- the previous prefix kept",
                   static_cast<f64>(sizeInfo.accelerationStructureSize) / (1024.0 * 1024.0),
                   static_cast<f64>(scratchSize) / (1024.0 * 1024.0), total);
        releaseNew();
        return false;
    }
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = asBuffer;
    aci.size = sizeInfo.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (!vkOk(api.CreateAccelerationStructureKHR(device, &aci, nullptr, &as), "rhi TLAS create (static prefix)")) {
        releaseNew();
        return false;
    }

    BufferHandle descs = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    u64 fillBytes = 0;
    const u64 descBytes = static_cast<u64>(total) * sizeof(VkAccelerationStructureInstanceKHR);
    if (count) {
        BufferDesc bd;
        bd.bytes = descBytes;
        bd.kind = BufferKind::Default;
        bd.debugName = "rhi TLAS static instances";
        descs = createBuffer(bd);
        // Packed STRAIGHT into the mapped staging memory -- uploadToDeviceBuffers would need the whole
        // prefix built once more on the CPU first, at up to hundreds of MiB. The copy is the next build's.
        fillBytes = static_cast<u64>(count) * sizeof(VkAccelerationStructureInstanceKHR);
        u8* mapped = nullptr;
        bool filled = descs &&
            createBufferCommitted(*dev_, fillBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  staging, stagingMemory, nullptr, "rhi TLAS static instances staging") &&
            vkOk(api.MapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&mapped)),
                 "rhi TLAS static instances map");
        if (filled) {
            auto* out = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(mapped);
            for (u32 i = 0; i < count; ++i)
                out[i] = vkInstanceFromTlas(instances[i], blases_[instances[i].blas - 1].asAddress);
            api.UnmapMemory(device, stagingMemory);
        }
        if (!filled) {
            destroyBufferCommitted(*dev_, staging, stagingMemory);
            AVER_ERROR("[RHI.Vulkan] setTlasStaticInstances: the {:.1f} MiB instance buffer could not be created "
                       "or filled -- the previous prefix kept", static_cast<f64>(descBytes) / (1024.0 * 1024.0));
            if (descs) destroyBuffer(descs);
            releaseNew();
            return false;
        }
#if AVER_RHI_TRACK_STATE
        // Its synchronisation belongs to the build (recordTlasBuild); a caller's bufferBarrier on it is
        // reported rather than obeyed.
        buffers_[descs - 1].stateFixed = true;
#endif
    }

    // Replaced, never written in place: frames still in flight traverse the old structure, released
    // once they retire. The new one holds nothing until the next build.
    {
        VkAccelerationStructureKHR oldAs = t->as;
        VkBuffer oldAsBuf = t->asBuffer, oldScratchBuf = t->scratchBuffer;
        VkDeviceMemory oldAsMem = t->asMemory, oldScratchMem = t->scratchMemory;
        retire([this, oldAs, oldAsBuf, oldScratchBuf, oldAsMem, oldScratchMem]() {
            if (oldAs) dev_->api().DestroyAccelerationStructureKHR(dev_->vkDevice(), oldAs, nullptr);
            destroyBufferCommitted(*dev_, oldAsBuf, oldAsMem);
            destroyBufferCommitted(*dev_, oldScratchBuf, oldScratchMem);
        });
    }
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure = as;
    t->as = as;
    t->asBuffer = asBuffer;
    t->asMemory = asMemory;
    t->scratchBuffer = scratchBuffer;
    t->scratchMemory = scratchMemory;
    t->asAddress = api.GetAccelerationStructureDeviceAddressKHR(device, &dai);
    t->asSize = sizeInfo.accelerationStructureSize;
    t->scratchSize = scratchSize;
    if (t->staticDescs) destroyBuffer(t->staticDescs);
    t->staticDescs = descs;
    t->staticCount = count;
    if (t->staticStaging) {   // an earlier prefix never built
        VkBuffer b = t->staticStaging;
        VkDeviceMemory m = t->staticStagingMemory;
        retire([this, b, m]() { destroyBufferCommitted(*dev_, b, m); });
    }
    t->staticStaging = staging;
    t->staticStagingMemory = stagingMemory;
    t->staticStagingBytes = staging ? fillBytes : 0;
    t->staticBlases = std::move(distinct);
    t->staticBrokenLogged = false;
    t->built = false;
    t->builtStatic = 0;
    t->builtSlots.clear();
    AVER_INFO("[RHI.Vulkan] TLAS {} static prefix: {} instance(s) over {} BLAS(es) + {} per frame -- structure "
              "{:.1f} MiB, scratch {:.1f} MiB, instance descs {:.1f} MiB (build sizes)",
              h, count, t->staticBlases.size(), t->maxInstances,
              static_cast<f64>(sizeInfo.accelerationStructureSize) / (1024.0 * 1024.0),
              static_cast<f64>(scratchSize) / (1024.0 * 1024.0),
              count ? static_cast<f64>(descBytes) / (1024.0 * 1024.0) : 0.0);
    return true;
}

BufferHandle VulkanResourceFactory::tlasStaticInstanceBuffer(TlasHandle h) const {
    if (h == 0 || h > tlases_.size()) return 0;
    const RhiTlas& t = tlases_[h - 1];
    return t.staticCount ? t.staticDescs : 0;
}

u64 VulkanResourceFactory::blasMemoryBytes(BlasHandle h) const {
    if (h == 0 || h > blases_.size()) return 0;
    const RhiBlas& b = blases_[h - 1];
    return b.as ? static_cast<u64>(b.asSize + b.scratchSize) : 0;
}

u64 VulkanResourceFactory::tlasMemoryBytes(TlasHandle h) const {
    if (h == 0 || h > tlases_.size()) return 0;
    const RhiTlas& t = tlases_[h - 1];
    if (!t.as) return 0;
    u64 bytes = static_cast<u64>(t.asSize + t.scratchSize) +
                static_cast<u64>(kFrameCount) * t.maxInstances * sizeof(VkAccelerationStructureInstanceKHR);
    if (const RhiBuffer* d = buffer(t.staticDescs)) bytes += d->desc.bytes;
    return bytes;
}

void VulkanResourceFactory::destroyTexture(TextureHandle h) {
    // BEFORE anything is torn down: a UI toolkit may hold a descriptor pointing at this texture's
    // view, and that descriptor has to go first or it outlives what it points at.
    if (dev_) dev_->releaseUiTextureId(h);
    RhiTexture* t = texture(h);
    if (!t) return;
    VkImage image = t->image;
    VkDeviceMemory memory = t->memory;
    // See RhiTexture::externallyOwned: an adopted depth-texture record's image/memory belong to
    // VulkanDevice, not this factory, so the retired callback must skip destroyImageCommitted for
    // them -- captured alongside image/memory (not nulled out here) so the callback's logic and this
    // function's early `t->image = VK_NULL_HANDLE` bookkeeping match every other destroy* path here.
    const bool owned = !t->externallyOwned;
    VkImageView srv = t->srvView, rtv = t->rtvView, dsv = t->dsvView;
    std::vector<VkImageView> uavs = std::move(t->uavViews);
    retire([this, image, memory, srv, rtv, dsv, uavs, owned]() {
        if (srv) dev_->api().DestroyImageView(dev_->vkDevice(), srv, nullptr);
        if (rtv) dev_->api().DestroyImageView(dev_->vkDevice(), rtv, nullptr);
        if (dsv) dev_->api().DestroyImageView(dev_->vkDevice(), dsv, nullptr);
        for (VkImageView v : uavs) if (v) dev_->api().DestroyImageView(dev_->vkDevice(), v, nullptr);
        if (owned) destroyImageCommitted(*dev_, image, memory);
    });
    t->image = VK_NULL_HANDLE;
    t->memory = VK_NULL_HANDLE;
    t->srvView = t->rtvView = t->dsvView = VK_NULL_HANDLE;
    t->uavViews.clear();
    t->externallyOwned = false;
    collect();
}

void VulkanResourceFactory::destroyBuffer(BufferHandle h) {
    RhiBuffer* b = buffer(h);
    if (!b) return;
    VkBuffer buf = b->buffer;
    VkDeviceMemory mem = b->memory;
    retire([this, buf, mem]() { destroyBufferCommitted(*dev_, buf, mem); });
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->mapped = nullptr;
    collect();
}

void VulkanResourceFactory::destroyBlas(BlasHandle h) {
    if (h == 0 || h > blases_.size()) return;
    RhiBlas& b = blases_[h - 1];
    if (!b.as && !b.asBuffer) return;
    VkAccelerationStructureKHR as = b.as;
    VkBuffer asBuf = b.asBuffer, scratchBuf = b.scratchBuffer;
    VkDeviceMemory asMem = b.asMemory, scratchMem = b.scratchMemory;
    retire([this, as, asBuf, scratchBuf, asMem, scratchMem]() {
        if (as) dev_->api().DestroyAccelerationStructureKHR(dev_->vkDevice(), as, nullptr);
        destroyBufferCommitted(*dev_, asBuf, asMem);
        destroyBufferCommitted(*dev_, scratchBuf, scratchMem);
    });
    b.as = VK_NULL_HANDLE;
    b.asBuffer = VK_NULL_HANDLE;
    b.scratchBuffer = VK_NULL_HANDLE;
    b.mesh = 0;
    b.built = false;
    b.allowUpdate = false;
    b.builtVertexCount = 0;
    b.builtIndexCount = 0;
    b.geometries.clear();
    collect();
}

MeshHandle VulkanResourceFactory::blasMesh(BlasHandle h) const {
    if (h == 0 || h > blases_.size()) return 0;
    return blases_[h - 1].mesh;
}

// The twin of D3D12ResourceFactory::blasForMesh; see IResourceFactory for the contract and why only
// BUILT structures may be handed over. Kept identical on both backends deliberately -- a dedup that
// fires on one and not the other is two different frames from the same scene.
BlasHandle VulkanResourceFactory::blasForMesh(MeshHandle mesh) const {
    if (mesh == 0) return 0;
    for (usize i = 0; i < blases_.size(); ++i)
        if (blases_[i].mesh == mesh && blases_[i].built && blases_[i].geometries.empty())
            return static_cast<BlasHandle>(i + 1);
    return 0;
}

// A createBlasMulti structure goes with ANY of its meshes -- see D3D12's twin.
void VulkanResourceFactory::destroyBlasForMesh(MeshHandle mesh) {
    if (mesh == 0) return;
    for (usize i = 0; i < blases_.size(); ++i) {
        const RhiBlas& b = blases_[i];
        bool names = b.mesh == mesh;
        for (const BlasGeometry& g : b.geometries) names = names || g.mesh == mesh;
        if (names) destroyBlas(static_cast<BlasHandle>(i + 1));
    }
}

// ================================================================================================
// 12. Descriptor population
// ================================================================================================
void VulkanResourceFactory::setSrv(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.Vulkan] setSrv with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.Vulkan] setSrv slot {} past the {} declared", slot, s->srvCount); return; }

    VkImageView view = t->srvView;
    VkImageView builtView = VK_NULL_HANDLE;
    if (mip != kAllMips) {
        if (mip >= t->desc.mips) { AVER_ERROR("[RHI.Vulkan] setSrv mip {} is past the {} this texture has", mip, t->desc.mips); return; }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = viewTypeFor(t->desc.dim);
        vi.format = toVkFormat(t->desc.format);
        vi.subresourceRange = {toVkAspect(t->desc.format), mip, 1, 0, 1};
        if (!vkOk(dev_->api().CreateImageView(dev_->vkDevice(), &vi, nullptr, &builtView), "rhi setSrv mip view")) return;
        view = builtView;
    }
    BindingSlotState st;
    st.type = toVkSrvDescriptorType(s->srvKinds[slot], dev_->cachedCaps().rayTracingTier != 0);
    st.image = VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // The one-off mip view is now OWNED BY THE SLOT rather than retired at the end of this call: a
    // replay into another ring slot happens frames later and would otherwise write a dead view.
    // writeBindingSlot retires whichever view the slot held before.
    st.ownedView = builtView;
    writeBindingSlot(*s, /*isUav=*/false, slot, st);
}

void VulkanResourceFactory::setUav(BindingSetHandle set, u32 slot, TextureHandle h, u32 mip) {
    RhiBindingSet* s = bindingSet(set);
    RhiTexture* t = texture(h);
    if (!s || !t) { AVER_ERROR("[RHI.Vulkan] setUav with an invalid handle"); return; }
    if (slot >= s->uavCount) { AVER_ERROR("[RHI.Vulkan] setUav slot {} past the {} declared", slot, s->uavCount); return; }
    if (mip == kAllMips || mip >= t->desc.mips) { AVER_ERROR("[RHI.Vulkan] setUav needs a single valid mip"); return; }

    if (mip >= t->uavViews.size()) t->uavViews.resize(t->desc.mips, VK_NULL_HANDLE);
    if (t->uavViews[mip] == VK_NULL_HANDLE) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = viewTypeFor(t->desc.dim);
        vi.format = toVkFormat(t->desc.format);
        vi.subresourceRange = {toVkAspect(t->desc.format), mip, 1, 0, 1};
        if (!vkOk(dev_->api().CreateImageView(dev_->vkDevice(), &vi, nullptr, &t->uavViews[mip]), "rhi setUav mip view")) return;
    }
    BindingSlotState st;
    st.type = toVkUavDescriptorType(s->uavKinds[slot]);
    // t->uavViews[mip] is cached on the texture and outlives every replay, so the slot does not own it.
    st.image = VkDescriptorImageInfo{VK_NULL_HANDLE, t->uavViews[mip], VK_IMAGE_LAYOUT_GENERAL};
    writeBindingSlot(*s, /*isUav=*/true, slot, st);
}

// Returns one SRV slot to the state nullFill gave it (IResourceFactory::clearSrv): a descriptor
// left naming a destroyed resource is a device loss, not a wrong pixel.
// "Null" HERE MEANS THE DUMMY, NOT VK_NULL_HANDLE: Vulkan has no null descriptor without
// robustness2, so this writes the same gNull dummy (1x1 image / empty buffer / TLAS) nullFill does,
// so a cleared slot is indistinguishable from one never bound. Goes through writeBindingSlot for
// the same ring reason as setSrv: the write must reach every frame's copy of the set.
void VulkanResourceFactory::clearSrv(BindingSetHandle set, u32 slot) {
    RhiBindingSet* s = bindingSet(set);
    if (!s) { AVER_ERROR("[RHI.Vulkan] clearSrv with an invalid set"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.Vulkan] clearSrv slot {} past the {} declared", slot, s->srvCount); return; }
    if (!ensureNullResources(*dev_)) {
        AVER_ERROR("[RHI.Vulkan] clearSrv: the dummy null resources are unavailable, so slot {} keeps "
                   "whatever it held -- which is the descriptor the caller is trying to stop using", slot);
        return;
    }
    const bool rtSupported = dev_->cachedCaps().rayTracingTier != 0;
    const SlotKind kind = s->srvKinds[slot];
    BindingSlotState st;
    st.type = toVkSrvDescriptorType(kind, rtSupported);
    if (kind == SlotKind::StructuredBuffer) {
        st.buffer = VkDescriptorBufferInfo{gNull.buffer, 0, VK_WHOLE_SIZE};
    } else if (kind == SlotKind::AccelerationStructure && rtSupported) {
        st.accel = gNull.dummyTlas;
    } else {
        const bool tex3D = (kind == SlotKind::Texture3D);
        st.image = VkDescriptorImageInfo{VK_NULL_HANDLE, tex3D ? gNull.view3D : gNull.view2D,
                                         VK_IMAGE_LAYOUT_GENERAL};
    }
    // NOT owned: gNull's views are global and outlive every binding set, so the slot must not retire
    // one. writeBindingSlot still retires whatever view the slot owned BEFORE this call.
    st.ownedView = VK_NULL_HANDLE;
    writeBindingSlot(*s, /*isUav=*/false, slot, st);
}

void VulkanResourceFactory::setSrvTlas(BindingSetHandle set, u32 slot, TlasHandle h) {
    RhiBindingSet* s = bindingSet(set);
    RhiTlas* t = tlas(h);
    if (!s || !t) { AVER_ERROR("[RHI.Vulkan] setSrvTlas with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.Vulkan] setSrvTlas slot {} past the {} declared", slot, s->srvCount); return; }

    BindingSlotState st;
    st.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    st.accel = t->as;   // by VALUE: applySlot points at its own copy, so no pointer outlives the call
    writeBindingSlot(*s, /*isUav=*/false, slot, st);
}

void VulkanResourceFactory::setSrvBuffer(BindingSetHandle set, u32 slot, BufferHandle bh, u32 stride, u32 count, u32 firstElement) {
    RhiBindingSet* s = bindingSet(set);
    RhiBuffer* b = buffer(bh);
    if (!s || !b) { AVER_ERROR("[RHI.Vulkan] setSrvBuffer with an invalid handle"); return; }
    if (slot >= s->srvCount) { AVER_ERROR("[RHI.Vulkan] setSrvBuffer slot {} past the {} declared", slot, s->srvCount); return; }
    if (s->srvKinds[slot] != SlotKind::StructuredBuffer) { AVER_ERROR("[RHI.Vulkan] setSrvBuffer on slot {}, which was declared as a texture", slot); return; }
    if (stride == 0) { AVER_ERROR("[RHI.Vulkan] setSrvBuffer with a zero stride"); return; }

    BindingSlotState st;
    st.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    st.buffer = VkDescriptorBufferInfo{b->buffer, static_cast<VkDeviceSize>(firstElement) * stride,
                                       static_cast<VkDeviceSize>(count) * stride};
    writeBindingSlot(*s, /*isUav=*/false, slot, st);
}

void VulkanResourceFactory::setUavBuffer(BindingSetHandle set, u32 slot, BufferHandle bh, u32 stride, u32 count, u32 firstElement) {
    RhiBindingSet* s = bindingSet(set);
    RhiBuffer* b = buffer(bh);
    if (!s || !b) { AVER_ERROR("[RHI.Vulkan] setUavBuffer with an invalid handle"); return; }
    if (slot >= s->uavCount) { AVER_ERROR("[RHI.Vulkan] setUavBuffer slot {} past the {} declared", slot, s->uavCount); return; }
    if (s->uavKinds[slot] != SlotKind::StructuredBuffer) { AVER_ERROR("[RHI.Vulkan] setUavBuffer on slot {}, which was declared as a texture", slot); return; }
    if (stride == 0) { AVER_ERROR("[RHI.Vulkan] setUavBuffer with a zero stride"); return; }

    BindingSlotState st;
    st.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    st.buffer = VkDescriptorBufferInfo{b->buffer, static_cast<VkDeviceSize>(firstElement) * stride,
                                       static_cast<VkDeviceSize>(count) * stride};
    writeBindingSlot(*s, /*isUav=*/true, slot, st);
}

// ================================================================================================
// 13. Reads, writes, misc
// ================================================================================================
bool VulkanResourceFactory::writeBuffer(BufferHandle h, const void* src, u64 bytes, u64 offset) {
    RhiBuffer* b = buffer(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] writeBuffer with an invalid handle"); return false; }
    if (!b->mapped) { AVER_ERROR("[RHI.Vulkan] writeBuffer on a buffer that is not BufferKind::Upload"); return false; }
    if (!src || bytes == 0) return true;
    if (offset + bytes > b->desc.bytes) {
        AVER_ERROR("[RHI.Vulkan] writeBuffer of {} bytes at {} overruns a {}-byte buffer", bytes, offset, b->desc.bytes);
        return false;
    }
    std::memcpy(b->mapped + offset, src, static_cast<usize>(bytes));
    if (!b->coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = b->memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        dev_->api().FlushMappedMemoryRanges(dev_->vkDevice(), 1, &range);
    }
    return true;
}

bool VulkanResourceFactory::readBuffer(BufferHandle h, void* dst, u64 bytes, u64 offset) {
    RhiBuffer* b = buffer(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] readBuffer with an invalid handle"); return false; }
    if (b->desc.kind != BufferKind::Readback || !b->mapped) {
        AVER_ERROR("[RHI.Vulkan] readBuffer on a buffer that is not BufferKind::Readback");
        return false;
    }
    if (!dst || bytes == 0) return true;
    if (offset + bytes > b->desc.bytes) {
        AVER_ERROR("[RHI.Vulkan] readBuffer of {} bytes at {} overruns a {}-byte buffer", bytes, offset, b->desc.bytes);
        return false;
    }
    std::memcpy(dst, b->mapped + offset, static_cast<usize>(bytes));
    return true;
}

bool VulkanResourceFactory::textureInfo(TextureHandle h, TextureDesc& out) const {
    const RhiTexture* t = texture(h);
    if (!t) return false;
    out = t->desc;
    return true;
}

void VulkanResourceFactory::waitIdle() {
    dev_->waitForGpu();
    collect();
}

} // namespace aver::rhi::vkb
