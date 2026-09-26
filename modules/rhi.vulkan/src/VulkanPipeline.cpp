// VulkanPipeline.cpp — pipeline construction for the Vulkan RHI backend: shader-module wiring,
// pipeline LAYOUTS built from rhi::PipelineLayout, vertex input built from rhi::VertexLayout,
// dynamic rendering (no VkRenderPass/VkFramebuffer anywhere in this backend), and
// createGraphicsPipeline/createComputePipeline themselves.
//
// ================================================================================================
// SCOPE NOTE, READ FIRST — a real discrepancy with VulkanCommon.hpp's own FILE MAP comment.
// ================================================================================================
// VulkanCommon.hpp's banner (written before this file existed) attributes EVERY IResourceFactory
// override, without exception, to VulkanResourceFactory.cpp. This pass's actual task assignment
// carves the PIPELINE-shaped slice of that surface out into this separate file instead:
//
//     defined HERE:                              defined in VulkanResourceFactory.cpp:
//     - createGraphicsPipeline                   - createTexture / createBuffer / createShader
//     - createComputePipeline                    - createBindingSet / createBlas / createTlas
//     - descriptorLayout()                       - every destroy*() / setSrv / setUav / ...
//     - tableSetLayout()                         - writeBuffer / readBuffer / textureInfo
//     - getOrCreateSampler()                     - waitIdle / retireFence / retire / collect
//     - pushConstantLayout() (free function)      - the texture/buffer/shader/bindingSet/blas/tlas
//                                                    table lookups, nullFill, uploadInitialData
//
// This is the SAME class (VulkanCommon.hpp declares `class VulkanResourceFactory` exactly once;
// C++ member functions may be defined in any translation unit that has seen the class body), so
// nothing below redeclares anything — every signature here is copied character-for-character from
// VulkanCommon.hpp. What it DOES mean is a real coordination risk this file cannot see past: if
// whoever is writing VulkanResourceFactory.cpp in parallel was handed the STALE file-map instead of
// this task's actual split, both files would define createGraphicsPipeline (etc.) and the module
// would fail to LINK, not compile — a duplicate-symbol error, not a silent bug. I have no visibility
// into that file's contents or its author's instructions; flagging this here and in this session's
// own honestState is the most I can do about it from inside one file.
//
// `pipelines_`, `descriptorLayouts_`, `tableShapes_`, `samplers_` and `descriptorPool_` are private
// members of VulkanResourceFactory declared in VulkanCommon.hpp; ordinary C++ access control makes
// them reachable from any member-function definition of that class regardless of which .cpp the
// definition lives in, so touching them here is not a layering violation.
// ================================================================================================
#include "VulkanCommon.hpp"

#include <vector>

namespace aver::rhi::vkb {

namespace {

// The SPIR-V entry-point name every VkPipelineShaderStageCreateInfo below is built with.
//
// RhiShader (VulkanCommon.hpp, section 7) carries exactly {spirv, stage, module} — NO entry-point
// string. That is only a safe design if every shader this backend ever compiles is normalised to
// the SAME SPIR-V entry name at compile time, since nothing downstream of createShader has any
// other way to recover what HLSL's ShaderDesc::entry originally said. DXC's DEFAULT `-spirv`
// lowering does NOT do this normalisation on its own — it preserves the original HLSL entry name
// (e.g. "VSMain") as the SPIR-V OpEntryPoint unless told otherwise with
// `-fspv-entrypoint-name=<name>`. So this file's use of the literal "main" below is a HARD
// ASSUMPTION that VulkanShaderCompiler::compile() (VulkanShaderCompiler.cpp, a different file in
// this same pass) passes that flag for every compile. I cannot see that file's contents from here.
// If the assumption is wrong, every pipeline built below fails LOUDLY: vkCreateGraphicsPipelines /
// vkCreateComputePipelines returns VK_ERROR_INVALID_SHADER_NV or a validation error for a missing
// entry point, caught by vkOk() and logged with the Vulkan call name — not a silent black frame.
// Re-verify against an actual DXC -spirv disassembly (`dxc -spirv -Fc ...`) the first time this
// backend can actually be built and run.
constexpr const char* kSpirvEntryPoint = "main";

VkPipelineShaderStageCreateInfo vkStageInfo(VkShaderModule module, VkShaderStageFlagBits stage) {
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    si.stage = stage;
    si.module = module;
    si.pName = kSpirvEntryPoint;
    return si;
}

// SlotKind -> VkDescriptorType. D3D12's descriptor TABLE is dimension-agnostic (a whole range is
// just "N SRV descriptors"; the dimension lives on the individual CPU descriptor CreateSRV/UAV
// writes, not the table), which is why PipelineLayout (modules/rhi/, shared, off-limits to this
// pass) carries SRV/UAV *counts* only and no per-slot kind at all. Vulkan has no such dimension-
// agnostic descriptor type: a VkDescriptorSetLayoutBinding fixes ONE concrete VkDescriptorType per
// binding, so something on this backend has to supply the kind D3D12 never needed. See
// tableSetLayout()'s own comment for where that information can and cannot come from, and
// descriptorLayout()'s for the real, unresolved gap this leaves for a pipeline built from a bare
// PipelineLayout (as opposed to a BindingSetDesc, which DOES carry real kinds).
//
// VulkanResourceFactory.cpp's own setSrv/setUav (a different file in this pass) needs this exact
// same SlotKind -> VkDescriptorType mapping to fill in vkUpdateDescriptorSets' VkWriteDescriptorSet
// ::descriptorType correctly. Since VulkanCommon.hpp is out of scope for this file to extend with a
// shared inline helper, that file almost certainly carries its own, textually duplicated copy of
// this same switch. Keeping the two in sync by hand is a real, low-grade maintenance hazard this
// pass accepts rather than fixes — flagged here so whoever next touches either copy knows the other
// one exists.
VkDescriptorType toVkDescriptorType(SlotKind kind, bool uav) {
    switch (kind) {
        case SlotKind::Texture2D:
        case SlotKind::Texture3D:
            return uav ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case SlotKind::StructuredBuffer:
            // HLSL's StructuredBuffer<T> (read-only) and RWStructuredBuffer<T> (read-write) both
            // lower to VK_DESCRIPTOR_TYPE_STORAGE_BUFFER under DXC's SPIR-V path -- Vulkan has no
            // separate "read-only storage buffer" descriptor type; NonWritable is a SPIR-V
            // decoration on the read-only variant, not a different VkDescriptorType.
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SlotKind::AccelerationStructure:
            // SRV-only per SlotKind's own declared contract (RHIResources.hpp); a caller that put
            // this kind on a UAV slot made an error this layer does not reject, beyond returning the
            // same type either way, matching this file's "fail loudly at the actual Vulkan call,
            // not with an assert here" posture.
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}

} // namespace

// ================================================================================================
// pushConstantLayout — section 4 / 6 of VulkanCommon.hpp's own write-up. Computes the byte layout
// EVERY later vkCmdPushConstants call (VulkanRenderContext.cpp, a different file) must agree with
// down to the byte, since it is what VulkanResourceFactory::createGraphicsPipeline/
// createComputePipeline (below) actually reserves with the device via VkPushConstantRange.
// ================================================================================================
PushConstantLayout pushConstantLayout(const PipelineLayout& layout, bool mesh) {
    PushConstantLayout out;

    // b1 (kObjectConstantRegister) is ALWAYS the first 128 bytes, by the engine-wide convention
    // documented at RHIResources.hpp:333-335 ("ALWAYS present, ALWAYS kObjectConstantDwords=32
    // dwords") -- PushConstantLayout::kObjectOffset/kObjectBytes are compile-time constants for
    // exactly this reason, and the loop below deliberately does not touch slot 1 again: reading
    // layout.constantDwords[kObjectConstantRegister] here would be redundant with, and could in
    // principle disagree with, the fixed 128-byte reservation every caller of this layout already
    // assumes.
    u32 cursor = PushConstantLayout::kObjectBytes;

    for (u32 k = 0; k < kMaxConstantSlots; ++k) {
        if (k == kObjectConstantRegister) continue;         // accounted for above, unconditionally
        if (layout.constantDwords[k] == 0) continue;        // a root CBV (kVkSetConstants), not a push constant
        out.slotOffset[k] = cursor;
        out.slotBytes[k] = layout.constantDwords[k] * 4;
        cursor += out.slotBytes[k];                          // dwords*4 is always a multiple of 4, so `cursor` stays 4-byte aligned for the next slot without any extra padding logic
    }

    if (mesh) {
        // The mesh-geometry block: two VkDeviceAddress values (D3D12's raw root-SRV bind for the
        // same two buffers has no Vulkan descriptor-model equivalent; a bindless GPU pointer in a
        // push constant is the direct analogue) plus the 4-dword triangle/cluster count block that
        // mirrors kMeshGeometryConstantRegister (b5) on the D3D12 side. Align to 8 bytes first: any
        // odd number of 32-bit slots above can leave `cursor` on a 4-but-not-8-byte boundary, and a
        // misaligned 8-byte GPU load of a device address is worth avoiding even though the Vulkan
        // push-constant spec itself only requires 4-byte alignment.
        cursor = (cursor + 7u) & ~7u;
        out.meshVertexAddrOffset = cursor; cursor += 8;
        out.meshIndexAddrOffset  = cursor; cursor += 8;
        out.meshCountOffset      = cursor; cursor += 16;    // 4 dwords, matches kMeshGeometryConstantRegister's D3D12 shape
    }

    out.totalBytes = cursor;
    return out;
}

// ================================================================================================
// VulkanResourceFactory::tableSetLayout — the table-SHAPE cache (TableShapeEntry, VulkanCommon.hpp
// section 7). Shared, by contract, between this file's own descriptorLayout() and
// VulkanResourceFactory.cpp's createBindingSet(): both must resolve an identical (srvCount,
// uavCount, kinds) shape to the SAME VkDescriptorSetLayout, or a BindingSetHandle built for one
// pipeline could be bound to a DIFFERENT, incompatible VkPipelineLayout at the same set index.
// ================================================================================================
VkDescriptorSetLayout VulkanResourceFactory::tableSetLayout(u32 srvCountIn, u32 uavCountIn,
                                                             const SlotKind* srvKindsIn, const SlotKind* uavKindsIn) {
    const u32 srvCount = srvCountIn < kMaxBindingSlots ? srvCountIn : kMaxBindingSlots;
    const u32 uavCount = uavCountIn < kMaxBindingSlots ? uavCountIn : kMaxBindingSlots;

    // A null kind array means "every slot is the BindingSetDesc default", mirroring
    // BindingSetDesc::srvKinds/uavKinds's own zero-initialisation to SlotKind::Texture2D
    // (RHIResources.hpp). descriptorLayout() below relies on this for table0/table1, since a bare
    // PipelineLayout carries counts only and never per-slot kinds -- see that function's own note
    // for the real, unresolved structural gap this leaves open for any pipeline whose eventual
    // binding set uses a non-Texture2D kind.
    SlotKind srvDefault[kMaxBindingSlots] = {};   // value-initialised: SlotKind::Texture2D == 0
    SlotKind uavDefault[kMaxBindingSlots] = {};
    const SlotKind* srvKinds = srvKindsIn ? srvKindsIn : srvDefault;
    const SlotKind* uavKinds = uavKindsIn ? uavKindsIn : uavDefault;

    for (const TableShapeEntry& e : tableShapes_) {
        if (sameTableShape(e.srvCount, e.uavCount, e.srvKinds, e.uavKinds, srvCount, uavCount, srvKinds, uavKinds))
            return e.layout;
    }

    TableShapeEntry entry;
    entry.srvCount = srvCount;
    entry.uavCount = uavCount;
    for (u32 i = 0; i < srvCount; ++i) entry.srvKinds[i] = srvKinds[i];
    for (u32 i = 0; i < uavCount; ++i) entry.uavKinds[i] = uavKinds[i];

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(static_cast<size_t>(srvCount) + uavCount);
    for (u32 i = 0; i < srvCount; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = i;                                       // t(i), matching section 4's "binding 0..srvCount-1"
        b.descriptorType = toVkDescriptorType(entry.srvKinds[i], /*uav=*/false);
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_ALL;                  // matches D3D12's own ShaderVisibility = ALL for every root parameter, not a narrower per-stage mask
        bindings.push_back(b);
    }
    for (u32 i = 0; i < uavCount; ++i) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = kVkUavBindingBase + i;                   // u(i), at a FIXED offset so a growing srvCount never renumbers an already-cached UAV binding
        b.descriptorType = toVkDescriptorType(entry.uavKinds[i], /*uav=*/true);
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<u32>(bindings.size());
    ci.pBindings = bindings.empty() ? nullptr : bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateDescriptorSetLayout(dev_->vkDevice(), &ci, nullptr, &layout),
              "tableSetLayout: vkCreateDescriptorSetLayout"))
        return VK_NULL_HANDLE;

    entry.layout = layout;
    tableShapes_.push_back(entry);
    return layout;
}

// ================================================================================================
// VulkanResourceFactory::getOrCreateSampler — the (SamplerDesc -> VkSampler) cache backing the
// IMMUTABLE samplers set (kVkSetSamplers) descriptorLayout() bakes into a pipeline layout below.
// ================================================================================================
VkSampler VulkanResourceFactory::getOrCreateSampler(const SamplerDesc& d) {
    for (const SamplerCacheEntry& e : samplers_)
        if (sameSampler(e.desc, d)) return e.sampler;

    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = toVkFilter(d.filter);
    ci.minFilter = toVkFilter(d.filter);
    ci.mipmapMode = toVkMipmapMode(d.filter);
    ci.addressModeU = ci.addressModeV = ci.addressModeW = toVkAddressMode(d.address);
    ci.anisotropyEnable = (d.filter == Filter::Anisotropic) ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = d.maxAnisotropy ? static_cast<f32>(d.maxAnisotropy) : 1.0f;
    // `compare` is documented as "ComparisonLinear only" (SamplerDesc's own comment) -- gate on the
    // filter, not on `compare != CompareOp::Never`, so a caller that leaves compare at its default
    // Never for a non-comparison sampler never accidentally enables comparison sampling.
    ci.compareEnable = (d.filter == Filter::ComparisonLinear) ? VK_TRUE : VK_FALSE;
    ci.compareOp = toVkCompareOp(d.compare);
    ci.minLod = 0.0f;
    ci.maxLod = d.maxLod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    VkSampler sampler = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateSampler(dev_->vkDevice(), &ci, nullptr, &sampler), "getOrCreateSampler: vkCreateSampler"))
        return VK_NULL_HANDLE;

    SamplerCacheEntry entry;
    entry.desc = d;
    entry.sampler = sampler;
    samplers_.push_back(entry);
    return sampler;
}

// ================================================================================================
// VulkanResourceFactory::descriptorLayout — the pipeline-layout cache (DescriptorLayoutEntry,
// VulkanCommon.hpp section 7), the Vulkan analogue of D3D12ResourceFactory::rootSignature at the
// same cache granularity (one entry per DISTINCT PipelineLayout+mesh combination, looked up by
// sameLayout()).
// ================================================================================================
const DescriptorLayoutEntry* VulkanResourceFactory::descriptorLayout(const PipelineLayout& layout, bool mesh) {
    for (const DescriptorLayoutEntry& e : descriptorLayouts_)
        if (e.mesh == mesh && sameLayout(e.layout, layout)) return &e;

    DescriptorLayoutEntry e;
    e.layout = layout;
    e.mesh = mesh;

    // ---- table0 / table1 (kVkSetTable0 / kVkSetTable1) ----
    //
    // THE UNRESOLVED GAP: PipelineLayout carries srvCount/uavCount ONLY, never a per-slot SlotKind
    // (see toVkDescriptorType's own comment on why D3D12's descriptor tables never needed one and
    // Vulkan's do). Passing nullptr here makes tableSetLayout() assume every declared slot is
    // SlotKind::Texture2D -- correct for a lot of real pipelines (UiRenderer's is exactly this
    // shape), but WRONG whenever the BindingSetHandle actually bound to this pipeline at draw time
    // was built from a BindingSetDesc that declared a different kind for the same slot (this is a
    // REAL, exercised case elsewhere in this engine: render.skin/render.pcg/render.pt declare
    // StructuredBuffer slots, render.voxi declares Texture3D and AccelerationStructure slots). When
    // that happens, this function and createBindingSet() (VulkanResourceFactory.cpp) resolve to
    // TWO DIFFERENT TableShapeEntry cache entries for what the caller intended as one shape, and the
    // resulting VkDescriptorSet is not layout-compatible with this VkPipelineLayout's set at that
    // index -- a real Vulkan validation failure (and, absent validation layers on this machine per
    // the vendored-headers README, plausibly a silent GPU-side misread instead of a caught error).
    //
    // There is no fix available FROM THIS FILE: PipelineLayout is declared in
    // modules/rhi/include/aver/rhi/RHIResources.hpp, shared with the D3D12 backend and off limits to
    // this pass. The durable fix is giving PipelineLayout its own per-slot SlotKind array so both
    // backends can agree on what a table slot actually holds; until that lands, any feature module
    // whose binding sets use a non-Texture2D kind is not correctly served by this backend's generic
    // pipeline path. Flagging this as the single most important known defect in this file.
    e.tableSetLayouts[kVkSetTable0] = tableSetLayout(layout.srvCount, layout.uavCount, nullptr, nullptr);
    e.tableSetLayouts[kVkSetTable1] = tableSetLayout(layout.srvCount1, layout.uavCount1, nullptr, nullptr);
    if (e.tableSetLayouts[kVkSetTable0] == VK_NULL_HANDLE || e.tableSetLayouts[kVkSetTable1] == VK_NULL_HANDLE) {
        AVER_ERROR("[RHI.Vulkan] descriptorLayout: failed to build a table descriptor-set layout");
        return nullptr;   // nothing owned yet: the table layouts are the shared cache's, not this call's to release
    }

    // ---- constants set (kVkSetConstants) ----
    // One VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC binding per declared slot k whose
    // constantDwords[k] == 0, skipping k == kObjectConstantRegister (b1 is ALWAYS a push constant,
    // never a CBV -- see pushConstantLayout's identical carve-out). Slot 0 (b0, the engine PerFrame
    // block) is RESERVED to constantDwords[0] == 0 by convention (RHIResources.hpp:209), so binding
    // 0 is always present here; this set is never empty.
    {
        VkDescriptorSetLayoutBinding constantBindings[kMaxConstantSlots] = {};
        u32 constantBindingCount = 0;
        for (u32 k = 0; k < kMaxConstantSlots; ++k) {
            if (k == kObjectConstantRegister) continue;
            if (layout.constantDwords[k] != 0) continue;
            VkDescriptorSetLayoutBinding& b = constantBindings[constantBindingCount++];
            b.binding = k;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_ALL;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = constantBindingCount;
        ci.pBindings = constantBindings;
        if (!vkOk(dev_->api().CreateDescriptorSetLayout(dev_->vkDevice(), &ci, nullptr, &e.constantsSetLayout),
                  "descriptorLayout: vkCreateDescriptorSetLayout (constants set)"))
            return nullptr;
    }

    // ---- samplers set (kVkSetSamplers), IMMUTABLE ----
    VkSampler sampVk[4] = {};
    const u32 sampCount = layout.samplerCount < 4 ? layout.samplerCount : 4;
    if (sampCount) {
        bool anySamplerFailed = false;
        for (u32 i = 0; i < sampCount; ++i) {
            sampVk[i] = getOrCreateSampler(layout.samplers[i]);
            anySamplerFailed = anySamplerFailed || (sampVk[i] == VK_NULL_HANDLE);
        }
        if (anySamplerFailed) {
            AVER_ERROR("[RHI.Vulkan] descriptorLayout: failed to build one of this layout's {} static samplers", sampCount);
            dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.constantsSetLayout, nullptr);
            return nullptr;
        }

        VkDescriptorSetLayoutBinding sb[4] = {};
        for (u32 i = 0; i < sampCount; ++i) {
            sb[i].binding = i;
            sb[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            sb[i].descriptorCount = 1;
            sb[i].stageFlags = VK_SHADER_STAGE_ALL;
            sb[i].pImmutableSamplers = &sampVk[i];   // Vulkan copies the VkSampler handle into the layout at CreateDescriptorSetLayout time; sampVk[] does not need to outlive this call
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = sampCount;
        ci.pBindings = sb;
        if (!vkOk(dev_->api().CreateDescriptorSetLayout(dev_->vkDevice(), &ci, nullptr, &e.samplersSetLayout),
                  "descriptorLayout: vkCreateDescriptorSetLayout (samplers set)")) {
            dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.constantsSetLayout, nullptr);
            return nullptr;
        }

        // Allocated ONCE, right now: every binding here is fully determined by its immutable
        // sampler at allocation time, so nothing ever calls vkUpdateDescriptorSets on this set --
        // matching DescriptorLayoutEntry's own comment ("allocated ONCE, ... never rewritten").
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &e.samplersSetLayout;
        if (!vkOk(dev_->api().AllocateDescriptorSets(dev_->vkDevice(), &ai, &e.samplersSet),
                  "descriptorLayout: vkAllocateDescriptorSets (samplers set)")) {
            dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.samplersSetLayout, nullptr);
            dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.constantsSetLayout, nullptr);
            return nullptr;
        }
    }
    // e.samplersSetLayout / e.samplersSet stay VK_NULL_HANDLE when sampCount == 0, exactly as
    // DescriptorLayoutEntry's own field comments promise -- the tracked field, distinct from the
    // PLACEHOLDER used below to fill vkCreatePipelineLayout's pSetLayouts[kVkSetSamplers].

    // ---- push constants ----
    e.pushConstants = pushConstantLayout(layout, mesh);
    if (e.pushConstants.totalBytes > dev_->maxPushConstantsSize()) {
        AVER_ERROR("[RHI.Vulkan] descriptorLayout: layout needs {} bytes of push constants, over this "
                   "device's {}-byte limit", e.pushConstants.totalBytes, dev_->maxPushConstantsSize());
        if (e.samplersSet) dev_->api().FreeDescriptorSets(dev_->vkDevice(), descriptorPool_, 1, &e.samplersSet);
        if (e.samplersSetLayout) dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.samplersSetLayout, nullptr);
        dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.constantsSetLayout, nullptr);
        return nullptr;
    }

    // ---- the pipeline layout itself: all FOUR sets always present, at their fixed indices, per
    // section 4's scheme -- this uniformity (not just "however many this PipelineLayout declares")
    // is what lets a table-1 BindingSetHandle bind at set index 1 for ANY pipeline that shares its
    // shape there, matching TableShapeEntry's own "valid to bind at ANY table index" contract ----
    VkDescriptorSetLayout setLayouts[kVkDescriptorSetCount];
    setLayouts[kVkSetTable0] = e.tableSetLayouts[kVkSetTable0];
    setLayouts[kVkSetTable1] = e.tableSetLayouts[kVkSetTable1];
    setLayouts[kVkSetConstants] = e.constantsSetLayout;
    // No spare member on this class to cache a dedicated "empty set" placeholder (VulkanCommon.hpp's
    // member list is fixed for this pass) -- reuse the SAME empty-shape (0,0) entry table0/table1
    // already share whenever a pipeline declares nothing there. An empty (bindingCount == 0)
    // VkDescriptorSetLayout is trivially valid wherever a "nothing here" set is needed, regardless
    // of which set index it is later bound at.
    setLayouts[kVkSetSamplers] = sampCount ? e.samplersSetLayout : tableSetLayout(0, 0, nullptr, nullptr);

    VkPushConstantRange pcRange{VK_SHADER_STAGE_ALL, 0, e.pushConstants.totalBytes};
    VkPipelineLayoutCreateInfo plCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCi.setLayoutCount = kVkDescriptorSetCount;
    plCi.pSetLayouts = setLayouts;
    if (e.pushConstants.totalBytes > 0) {
        plCi.pushConstantRangeCount = 1;
        plCi.pPushConstantRanges = &pcRange;
    }
    if (!vkOk(dev_->api().CreatePipelineLayout(dev_->vkDevice(), &plCi, nullptr, &e.pipelineLayout),
              "descriptorLayout: vkCreatePipelineLayout")) {
        if (e.samplersSet) dev_->api().FreeDescriptorSets(dev_->vkDevice(), descriptorPool_, 1, &e.samplersSet);
        if (e.samplersSetLayout) dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.samplersSetLayout, nullptr);
        dev_->api().DestroyDescriptorSetLayout(dev_->vkDevice(), e.constantsSetLayout, nullptr);
        return nullptr;
    }

    // MITIGATION, NOT A FIX: RhiPipeline::layoutEntry (VulkanCommon.hpp section 7) stores a raw
    // `const DescriptorLayoutEntry*` returned from this function, read back on every draw. Since
    // descriptorLayouts_ is a std::vector (its element type and container are fixed by
    // VulkanCommon.hpp, not this file's to change), pushing a NEW, distinct layout can reallocate
    // the vector and invalidate every pointer an already-built RhiPipeline is holding -- a real
    // use-after-free this container shape does not protect against. Reserving a generous capacity
    // up front makes this unreachable for how many DISTINCT PipelineLayouts this engine's feature
    // modules realistically declare (a small double-digit count across render.pbr/render.skin/
    // render.voxi/render.pt/render.ui/render.pcg at the time of writing), but it is NOT a real fix
    // -- that would mean changing descriptorLayouts_'s element storage to something pointer-stable
    // (std::deque<DescriptorLayoutEntry>, or a vector of unique_ptr<DescriptorLayoutEntry>), which
    // is a change to VulkanCommon.hpp and out of scope here. Flagging this loudly rather than
    // silently trusting the reserve() below to be enough.
    if (descriptorLayouts_.capacity() == 0) descriptorLayouts_.reserve(64);

    descriptorLayouts_.push_back(e);
    return &descriptorLayouts_.back();
}

// ================================================================================================
// VulkanResourceFactory::createGraphicsPipeline — matches D3D12ResourceFactory::
// createGraphicsPipeline's BEHAVIOUR (validation order, what is fatal vs. silently degraded), not
// its structure: root-signature-shaped state here is descriptor-set/push-constant-shaped, and
// render-target binding is dynamic rendering (VkPipelineRenderingCreateInfo) rather than a
// VkRenderPass/VkFramebuffer pair -- VulkanApi (VulkanCommon.hpp section 2) never resolves
// vkCreateRenderPass or vkCreateFramebuffer at all, so that choice was already made before this
// file existed; it is not re-decided here.
// ================================================================================================
PipelineHandle VulkanResourceFactory::createGraphicsPipeline(const GraphicsPipelineDesc& d) {
    collect();

    // Exactly one of vs / ms: true here is an XNOR (both zero, or both non-zero) -- i.e. NOT
    // exactly one -- mirroring D3D12's own `(d.vs == 0) == (d.ms == 0)` check bit for bit.
    if ((d.vs != 0) == (d.ms != 0)) {
        AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline needs exactly one of vs / ms");
        return 0;
    }
    if (d.as != 0 && d.ms == 0) {
        AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline: an amplification shader (as) needs a mesh shader (ms) alongside it");
        return 0;
    }
    if (d.ms != 0 && dev_->cachedCaps().meshShaderTier == 0) {
        AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline: mesh shader pipeline requested but this "
                   "device reports meshShaderTier 0 (VK_EXT_mesh_shader unavailable, or its "
                   "mesh/task features unsupported)");
        return 0;
    }

    RhiShader* vs = shader(d.vs);
    RhiShader* gs = shader(d.gs);
    RhiShader* ms = shader(d.ms);
    RhiShader* ps = shader(d.ps);
    RhiShader* as = shader(d.as);
    if ((d.vs && !vs) || (d.gs && !gs) || (d.ms && !ms) || (d.ps && !ps) || (d.as && !as)) {
        AVER_ERROR("[RHI.Vulkan] createGraphicsPipeline given an invalid shader handle");
        return 0;
    }

    const DescriptorLayoutEntry* dl = descriptorLayout(d.layout, d.ms != 0);
    if (!dl) return 0;

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    if (ms) {
        stages.push_back(vkStageInfo(ms->module, VK_SHADER_STAGE_MESH_BIT_EXT));
        if (as) stages.push_back(vkStageInfo(as->module, VK_SHADER_STAGE_TASK_BIT_EXT));
    } else {
        stages.push_back(vkStageInfo(vs->module, VK_SHADER_STAGE_VERTEX_BIT));
        if (gs) stages.push_back(vkStageInfo(gs->module, VK_SHADER_STAGE_GEOMETRY_BIT));
    }
    if (ps) stages.push_back(vkStageInfo(ps->module, VK_SHADER_STAGE_FRAGMENT_BIT));

    // ---- vertex input: built from GraphicsPipelineDesc::vertexLayout, or MeshVertex's own fixed
    // layout when the caller left it empty (mirrors D3D12's `elemCount ? ... : kMeshInputLayout`).
    // Skipped entirely for a mesh pipeline: the spec says pVertexInputState/pInputAssemblyState are
    // ignored once a mesh shader stage is present, and some drivers are stricter than "ignored" --
    // nullptr is the safe reading, not an empty-but-present struct. ----
    VkVertexInputBindingDescription vbind{};
    VkVertexInputAttributeDescription vattribs[kMaxVertexAttribs] = {};
    u32 vattribCount = 0;
    if (!ms) {
        if (d.vertexLayout.attribCount > 0) {
            vbind = VkVertexInputBindingDescription{0, d.vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
            vattribCount = buildVertexInputAttributes(d.vertexLayout, vattribs);
        } else {
            VkVertexInputAttributeDescription meshAttribs[3];
            meshVertexInputState(vbind, meshAttribs);
            for (u32 i = 0; i < 3; ++i) vattribs[i] = meshAttribs[i];
            vattribCount = 3;
        }
    }
    VkPipelineVertexInputStateCreateInfo vertexInputState{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInputState.vertexBindingDescriptionCount = ms ? 0 : 1;
    vertexInputState.pVertexBindingDescriptions = ms ? nullptr : &vbind;
    vertexInputState.vertexAttributeDescriptionCount = vattribCount;
    vertexInputState.pVertexAttributeDescriptions = vattribs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    // The only topology GraphicsPipelineDesc can express (it has no topology field at all, matching
    // D3D12's own hardcoded D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) -- lines go through
    // VulkanDevice's own fixed line PSOs (createLineMesh/drawLines), never through this generic
    // factory path.
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport + scissor are dynamic (see dynState below): IRenderContext::setViewport/setScissor
    // are two separate calls, neither derived from the other (RHIResources.hpp:455's own comment),
    // so the pipeline itself only needs to declare COUNTS here, not values.
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationConservativeStateCreateInfoEXT consRaster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
    consRaster.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    // D3D12_RASTERIZER_DESC's DepthClipEnable and Vulkan's depthClampEnable are INVERSES of each
    // other: D3D12 "clip" (the default, DepthClipEnable defaults TRUE) means Vulkan
    // depthClampEnable = FALSE (do not clamp, i.e. actually clip). d.depthClip defaults true too, so
    // the common case is depthClampEnable = FALSE on both backends.
    raster.depthClampEnable = d.depthClip ? VK_FALSE : VK_TRUE;
    raster.polygonMode = (d.fill == FillMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = (d.cull == CullMode::Back) ? VK_CULL_MODE_BACK_BIT
                     : (d.cull == CullMode::Front) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    // D3D12_RASTERIZER_DESC{} zero-initialises FrontCounterClockwise to FALSE, i.e. CLOCKWISE
    // winding is front-facing on the D3D12 backend by default; matched here bit for bit rather than
    // re-derived from first principles. UNVERIFIED against an actual render (no build/run this
    // pass) -- if a mesh comes out back-face-culled that should not be, or vice versa, once this can
    // actually be built, this is the first line to re-check, alongside whether a Y-flip is needed
    // anywhere in the viewport/scissor path VulkanRenderContext.cpp owns.
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.depthBiasEnable = (d.depthBias != 0.0f || d.slopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = d.depthBias;
    raster.depthBiasSlopeFactor = d.slopeScaledDepthBias;
    raster.lineWidth = 1.0f;
    // D3D12 additionally disables conservative rasterisation for mesh pipelines specifically on
    // WARP, its software rasteriser (D3D12Device.cpp's warpMeshConservative carve-out). This backend
    // has no WARP analogue at all -- there is no OS-shipped Vulkan software rasteriser -- so that
    // carve-out has nothing to mirror here.
    if (d.conservativeRaster && dev_->cachedCaps().conservativeRaster) raster.pNext = &consRaster;

    VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msaa.rasterizationSamples = toVkSampleCount(d.sampleCount ? d.sampleCount : 1);

    VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthState.depthTestEnable = d.depth.test ? VK_TRUE : VK_FALSE;
    depthState.depthWriteEnable = d.depth.write ? VK_TRUE : VK_FALSE;
    depthState.depthCompareOp = toVkCompareOp(d.depth.op);

    // D3D12_BLEND_DESC only ever populates RenderTarget[0] and leaves IndependentBlendEnable at its
    // default FALSE, which means D3D12 applies THAT SAME state to every active render target. Mirror
    // that here by writing the identical VkPipelineColorBlendAttachmentState into every entry up to
    // rtCount, rather than leaving entries 1..rtCount-1 at Vulkan's own all-zero (blend-disabled)
    // default -- a silent behavioural divergence a naive "just fill index 0" port would introduce.
    const u32 rtCount = d.renderTargetCount < 4 ? d.renderTargetCount : 4;
    VkPipelineColorBlendAttachmentState blendAttachments[4] = {};
    for (u32 i = 0; i < rtCount; ++i) {
        VkPipelineColorBlendAttachmentState& a = blendAttachments[i];
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        switch (d.blend) {
            case BlendMode::Opaque:
                break;
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
    }
    VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blendState.attachmentCount = rtCount;
    blendState.pAttachments = blendAttachments;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dynStates;

    // ---- dynamic rendering, in place of a VkRenderPass/VkFramebuffer pair ----
    VkFormat colorFormats[4] = {};
    for (u32 i = 0; i < rtCount; ++i) colorFormats[i] = toVkFormat(d.renderTargets[i]);
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = rtCount;
    renderingInfo.pColorAttachmentFormats = colorFormats;
    renderingInfo.depthAttachmentFormat = toVkFormat(d.depthFormat);   // toVkFormat(Format::Unknown) == VK_FORMAT_UNDEFINED already, correctly meaning "no depth attachment"

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &renderingInfo;
    pci.stageCount = static_cast<u32>(stages.size());
    pci.pStages = stages.data();
    pci.pVertexInputState = ms ? nullptr : &vertexInputState;
    pci.pInputAssemblyState = ms ? nullptr : &inputAssembly;
    pci.pViewportState = &viewportState;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &msaa;
    pci.pDepthStencilState = &depthState;
    pci.pColorBlendState = &blendState;
    pci.pDynamicState = &dynState;
    pci.layout = dl->pipelineLayout;
    pci.renderPass = VK_NULL_HANDLE;   // dynamic rendering: no render-pass object exists on this backend at all

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateGraphicsPipelines(dev_->vkDevice(), VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline),
              "createGraphicsPipeline: vkCreateGraphicsPipelines"))
        return 0;

    RhiPipeline p;
    p.pipeline = pipeline;
    p.layoutEntry = dl;
    p.compute = false;
    p.mesh = (ms != nullptr);
    p.amplification = (as != nullptr);
    pipelines_.push_back(p);
    return static_cast<PipelineHandle>(pipelines_.size());   // handle = index + 1; 0 stays invalid
}

// ================================================================================================
// VulkanResourceFactory::createComputePipeline
// ================================================================================================
PipelineHandle VulkanResourceFactory::createComputePipeline(const ComputePipelineDesc& d) {
    collect();

    RhiShader* cs = shader(d.cs);
    if (!cs || cs->stage != ShaderStage::Compute) {
        AVER_ERROR("[RHI.Vulkan] createComputePipeline given a handle that is not a compute shader");
        return 0;
    }

    const DescriptorLayoutEntry* dl = descriptorLayout(d.layout, /*mesh=*/false);
    if (!dl) return 0;

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = vkStageInfo(cs->module, VK_SHADER_STAGE_COMPUTE_BIT);
    ci.layout = dl->pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().CreateComputePipelines(dev_->vkDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline),
              "createComputePipeline: vkCreateComputePipelines"))
        return 0;

    RhiPipeline p;
    p.pipeline = pipeline;
    p.layoutEntry = dl;
    p.compute = true;
    p.mesh = false;
    p.amplification = false;
    pipelines_.push_back(p);
    return static_cast<PipelineHandle>(pipelines_.size());
}

} // namespace aver::rhi::vkb
