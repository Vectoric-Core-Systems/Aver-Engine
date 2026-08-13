// VulkanRenderContext: the command-recording half of the Vulkan RHI backend. Implements
// aver::rhi::IRenderContext (RHIResources.hpp) the way D3D12RenderContext.cpp does for D3D12 --
// this file OWNS class VulkanRenderContext's method bodies plus ringAlloc()/
// bindDeclaredDescriptors()/applyDrawBinding()/cmd(), per VulkanCommon.hpp's file-map banner. It is
// the ONLY file this task allows editing (house rule: "STAY IN YOUR FILE"); VulkanCommon.hpp,
// VulkanDevice.cpp, VulkanResourceFactory.cpp and VulkanShaderCompiler.cpp are all read-only from
// here even though this file calls into things they declare or define.
//
// THE ONE STRUCTURAL FORK FROM D3D12 EVERYTHING ELSE IN THIS FILE FOLLOWS FROM: D3D12's
// OMSetRenderTargets is a plain state-setting call with no scope to nest -- a D3D12RenderContext can
// call it any number of times, in any order relative to draws, and D3D12Device.cpp can ALSO bind its
// own targets directly on the same command list without either side needing to know about the other.
// Vulkan's render-target model here is VK_KHR_dynamic_rendering (core at kRequiredApiVersion, see
// VulkanCommon.hpp section 1's note on why dynamic_rendering is a hard requirement, not an optional
// extension): every vkCmdDraw*/vkCmdDrawMeshTasksEXT must run inside a vkCmdBeginRendering /
// vkCmdEndRendering scope, and scopes cannot nest. VulkanDevice.cpp manages ITS OWN such scope
// directly on the shared command buffer for the fixed scene/post/UI passes, entirely independently of
// this class (see VulkanCommon.hpp's file-map: createPipeline/createSwapchainResources/runPostChain
// are all VulkanDevice.cpp's own, not routed through IRenderContext) -- so this class can never be
// the one left holding an open scope when control returns to a caller, or it would corrupt bookkeeping
// it cannot see and does not own. The rule this file follows throughout, and the reason for every
// "why is there a Begin/End pair right here" comment below: no VulkanRenderContext method may return
// with a vkCmdBeginRendering scope still open. setRenderTargets() therefore only RECORDS what the
// next draw should render into (see RenderTargetState below); every draw-issuing method opens a scope
// over that recorded state, issues its one draw, and closes the scope again before returning;
// clearColor/clearDepth open and close their own single-attachment scope directly against the named
// texture, exactly mirroring D3D12's ClearRenderTargetView/ClearDepthStencilView (which clear through
// an RTV/DSV descriptor regardless of what is currently OM-bound, not "whatever setRenderTargets last
// recorded").
//
// SEVERAL PIECES OF TRANSIENT STATE BELOW LIVE AT FILE (ANONYMOUS-NAMESPACE) SCOPE RATHER THAN AS
// VulkanRenderContext MEMBERS. VulkanCommon.hpp is off-limits to this file (house rule), and its
// already-fixed member list for VulkanRenderContext has no field for "what did the last
// setRenderTargets call bind" or "which descriptor set backs the current pipeline's constants".
// There is exactly one VulkanRenderContext for the life of the process (VulkanDevice's own
// rhiContext_), so translation-unit-scoped state here has identical lifetime and identical
// single-instance semantics to a class member would -- it is documented at each use, not smuggled in.
#include "VulkanCommon.hpp"

#include <cstring>

namespace aver::rhi::vkb {

namespace {

// ------------------------------------------------------------------------------------------------
// Dynamic-rendering scope bookkeeping for setRenderTargets() / the draw-issuing methods. See the
// file banner above for why this exists and why it cannot be a class member.
// ------------------------------------------------------------------------------------------------
struct RenderTargetState {
    VkRenderingAttachmentInfo colors[4]{};
    u32 colorCount = 0;
    VkRenderingAttachmentInfo depth{};
    bool hasDepth = false;
    u32 width = 0, height = 0;
};
RenderTargetState g_rt;

void beginRenderScope(const VulkanDevice& dev, VkCommandBuffer cb) {
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    // Falls back to the backbuffer's own size when setRenderTargets was never called (or bound
    // nothing usable) so a draw issued into "whatever was last recorded" still gets a valid,
    // non-zero render area rather than a validation-rejected 0x0 one -- a caller bug either way,
    // but one that should not itself crash the frame.
    const u32 w = g_rt.width ? g_rt.width : dev.width();
    const u32 h = g_rt.height ? g_rt.height : dev.height();
    ri.renderArea = VkRect2D{{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = g_rt.colorCount;
    ri.pColorAttachments = g_rt.colorCount ? g_rt.colors : nullptr;
    ri.pDepthAttachment = g_rt.hasDepth ? &g_rt.depth : nullptr;
    dev.api().CmdBeginRendering(cb, &ri);
}
void endRenderScope(const VulkanDevice& dev, VkCommandBuffer cb) {
    dev.api().CmdEndRendering(cb);
}

// ------------------------------------------------------------------------------------------------
// The descriptor set currently backing set kVkSetConstants (b0's PerFrameCB dynamic-UBO binding,
// plus whatever other zero-constantDwords slot the current pipeline declares). Allocated fresh by
// bindDeclaredDescriptors() on every setPipeline() call, valid until the NEXT setPipeline() call --
// exactly the same lifetime as the `pipe_` member already has, which is why this can safely sit
// beside it as file-scope state rather than needing its own class member. See the file banner for
// why it cannot BE a class member.
VkDescriptorSet g_constantsSet = VK_NULL_HANDLE;

// Re-binds `set` at kVkSetConstants with a fresh all-zero dynamic-offset array sized to the current
// pipeline's declared CBV-slot count. Called after bindDeclaredDescriptors() writes the set's
// default contents, and again after setConstantBuffer() rewrites one binding in place -- Vulkan
// requires the FULL dynamic-offset array on every bind, so a one-slot change still re-supplies
// every other slot's (unchanged) offset alongside it.
void rebindConstantsSet(const VulkanApi& api, VkCommandBuffer cb, const RhiPipeline* p, VkDescriptorSet set) {
    if (!p || !p->layoutEntry || set == VK_NULL_HANDLE) return;
    u32 offsets[kMaxConstantSlots] = {};
    u32 n = 0;
    for (u32 k = 0; k < kMaxConstantSlots; ++k)
        if (p->layoutEntry->layout.constantDwords[k] == 0) ++n;
    api.CmdBindDescriptorSets(cb, p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p->layoutEntry->pipelineLayout, kVkSetConstants, 1, &set, n, offsets);
}

// ---- ResourceState debug-shadow tracking, the Vulkan-side twin of D3D12Device.cpp's own
// rejectAsState/trackTextureBarrier/trackBufferBarrier (anonymous-namespace free functions there
// too -- neither is shared cross-TU, so neither belongs in VulkanCommon.hpp). ----
const char* stateName(ResourceState s) {
    switch (s) {
        case ResourceState::Common:                 return "Common";
        case ResourceState::ShaderResource:         return "ShaderResource";
        case ResourceState::NonPixelShaderResource: return "NonPixelShaderResource";
        case ResourceState::UnorderedAccess:        return "UnorderedAccess";
        case ResourceState::RenderTarget:           return "RenderTarget";
        case ResourceState::DepthWrite:              return "DepthWrite";
        case ResourceState::CopySource:              return "CopySource";
        case ResourceState::CopyDest:                return "CopyDest";
        case ResourceState::VertexBuffer:            return "VertexBuffer";
        case ResourceState::GeometryRead:            return "GeometryRead";
        case ResourceState::AccelerationStructure:   return "AccelerationStructure";
    }
    return "?";
}

bool rejectAsState(ResourceState from, ResourceState to, const char* what) {
    if (from != ResourceState::AccelerationStructure && to != ResourceState::AccelerationStructure) return false;
    AVER_ERROR("[RHI.Vulkan] {}: AccelerationStructure is terminal and cannot be transitioned", what);
    return true;
}

#if AVER_RHI_TRACK_STATE
void trackTextureBarrier(RhiTexture& t, ResourceState from, ResourceState to, u32 subresource) {
    const char* name = t.debugName.empty() ? "<unnamed>" : t.debugName.c_str();
    const u32 mips = static_cast<u32>(t.states.size());
    if (subresource == kAllSubresources) {
        for (u32 m = 0; m < mips; ++m) {
            if (t.states[m] == from) continue;
            AVER_ERROR("[RHI.Vulkan] barrier on '{}': whole-resource transition claims {} but mip {} is in {}",
                       name, stateName(from), m, stateName(t.states[m]));
            break;
        }
        for (ResourceState& s : t.states) s = to;
        return;
    }
    if (subresource >= mips) {
        AVER_ERROR("[RHI.Vulkan] barrier on '{}': subresource {} past the {} mips it has", name, subresource, mips);
        return;
    }
    if (t.states[subresource] != from)
        AVER_ERROR("[RHI.Vulkan] barrier on '{}': mip {} claims {} but is in {}",
                   name, subresource, stateName(from), stateName(t.states[subresource]));
    t.states[subresource] = to;
}
void trackBufferBarrier(RhiBuffer& b, ResourceState from, ResourceState to) {
    const char* name = b.debugName.empty() ? "<unnamed>" : b.debugName.c_str();
    if (b.stateFixed) {
        AVER_ERROR("[RHI.Vulkan] barrier on '{}': an upload or acceleration-structure buffer cannot be transitioned", name);
        return;
    }
    if (b.state != from)
        AVER_ERROR("[RHI.Vulkan] barrier on '{}': claims {} but is in {}", name, stateName(from), stateName(b.state));
    b.state = to;
}
#endif

} // namespace

// ====================================================================================================
// cmd() -- the one place every method below reaches the live command buffer through.
// ====================================================================================================
VkCommandBuffer VulkanRenderContext::cmd() const {
    return dev_ ? dev_->currentCommandBuffer() : VK_NULL_HANDLE;
}

// ====================================================================================================
// setPipeline
// ====================================================================================================
void VulkanRenderContext::setPipeline(PipelineHandle h) {
    pipe_ = nullptr;
    RhiPipeline* p = res_->pipeline(h);
    if (!p) { AVER_ERROR("[RHI.Vulkan] setPipeline with an invalid handle"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    dev_->api().CmdBindPipeline(cb, p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p->pipeline);
    pipe_ = p;
    bindDeclaredDescriptors(p);
}

// ====================================================================================================
// setViewport / setScissor -- dynamic pipeline state; valid to set at any point before the draw that
// consumes it, inside or outside a rendering scope. Not flipped for Vulkan's Y-down NDC: DXC's -spirv
// output already reproduces HLSL's SV_Position convention (top-left origin, Y-down) for a Vulkan
// target, so a plain, non-negative-height viewport is the right translation of D3D12's
// RSSetViewports here, not a bug waiting to be "fixed" with a flipped height.
// ====================================================================================================
void VulkanRenderContext::setViewport(u32 x, u32 y, u32 w, u32 h) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    VkViewport vp{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f};
    dev_->api().CmdSetViewport(cb, 0, 1, &vp);
}
void VulkanRenderContext::setScissor(u32 x, u32 y, u32 w, u32 h) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    VkRect2D sc{{static_cast<i32>(x), static_cast<i32>(y)}, {w, h}};
    dev_->api().CmdSetScissor(cb, 0, 1, &sc);
}

// ====================================================================================================
// setRenderTargets -- RECORDS the target views for the next draw's scope; see the file banner for
// why this does not itself open a vkCmdBeginRendering scope.
// ====================================================================================================
void VulkanRenderContext::setRenderTargets(const TextureHandle* colors, u32 count, TextureHandle depth) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    g_rt.colorCount = 0;
    u32 w = 0, h = 0;
    for (u32 i = 0; i < count && i < 4 && colors; ++i) {
        RhiTexture* t = res_->texture(colors[i]);
        if (!t || !t->rtvView) {
            AVER_ERROR("[RHI.Vulkan] setRenderTargets: colour {} was not created as a render target", i);
            continue;
        }
        VkRenderingAttachmentInfo& a = g_rt.colors[g_rt.colorCount++];
        a = VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = t->rtvView;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // caller already transitioned it (interface contract)
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;    // D3D12's OMSetRenderTargets never clears on bind either
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!w) { w = t->desc.width; h = t->desc.height; }
    }
    g_rt.hasDepth = false;
    if (depth) {
        RhiTexture* t = res_->texture(depth);
        if (t && t->dsvView) {
            g_rt.depth = VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            g_rt.depth.imageView = t->dsvView;
            g_rt.depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            g_rt.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            g_rt.depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            g_rt.hasDepth = true;
            if (!w) { w = t->desc.width; h = t->desc.height; }
        } else {
            AVER_ERROR("[RHI.Vulkan] setRenderTargets: depth was not created as a depth target");
        }
    }
    g_rt.width = w;
    g_rt.height = h;
}

// ====================================================================================================
// clearDepth / clearColor -- standalone single-attachment scopes against the NAMED texture, matching
// D3D12's ClearDepthStencilView/ClearRenderTargetView, which clear through their own descriptor
// regardless of what setRenderTargets last bound. See the file banner.
// ====================================================================================================
void VulkanRenderContext::clearDepth(TextureHandle depth, f32 value) {
    RhiTexture* t = res_->texture(depth);
    VkCommandBuffer cb = cmd();
    if (!t || !t->dsvView || !cb) { AVER_ERROR("[RHI.Vulkan] clearDepth on a non-depth texture"); return; }
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = t->dsvView;
    a.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue.depthStencil = VkClearDepthStencilValue{value, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = VkRect2D{{0, 0}, {t->desc.width, t->desc.height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &a;
    dev_->api().CmdBeginRendering(cb, &ri);
    dev_->api().CmdEndRendering(cb);
}

void VulkanRenderContext::clearColor(TextureHandle target, const f32 color[4]) {
    RhiTexture* t = res_->texture(target);
    VkCommandBuffer cb = cmd();
    if (!t || !t->rtvView || !cb) { AVER_ERROR("[RHI.Vulkan] clearColor on a non-render-target texture"); return; }
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = t->rtvView;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(a.clearValue.color.float32, color, sizeof(f32) * 4);
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = VkRect2D{{0, 0}, {t->desc.width, t->desc.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &a;
    dev_->api().CmdBeginRendering(cb, &ri);
    dev_->api().CmdEndRendering(cb);
}

// ====================================================================================================
// setBindingSet -- binds a table's descriptor set at kVkSetTable0/kVkSetTable1. See
// VulkanCommon.hpp section 4 for the set-index scheme.
// ====================================================================================================
void VulkanRenderContext::setBindingSet(BindingSetHandle set, u32 table) {
    if (!pipe_) { AVER_ERROR("[RHI.Vulkan] setBindingSet before setPipeline"); return; }
    if (table >= kBindingTableCount) {
        AVER_ERROR("[RHI.Vulkan] setBindingSet table {} past the {} declarable", table, kBindingTableCount);
        return;
    }
    if (!pipe_->layoutEntry) { AVER_ERROR("[RHI.Vulkan] setBindingSet: pipeline has no cached layout"); return; }
    RhiBindingSet* s = res_->bindingSet(set);
    VkCommandBuffer cb = cmd();
    if (!s || !cb) { AVER_ERROR("[RHI.Vulkan] setBindingSet with an invalid handle"); return; }

    const PipelineLayout& layout = pipe_->layoutEntry->layout;
    const u32 expectedBase = (table == 0) ? 0 : layout.srvCount;
    if (s->srvCount && s->srvBaseRegister != expectedBase)
        AVER_WARN("[RHI.Vulkan] binding set was built for t{} but table {} covers t{}",
                  s->srvBaseRegister, table, expectedBase);

    const bool tableDeclared = (table == 0) ? (layout.srvCount || layout.uavCount)
                                            : (layout.srvCount1 || layout.uavCount1);
    if (!tableDeclared) {
        AVER_WARN("[RHI.Vulkan] setBindingSet: pipeline declares no table {}", table);
        return;
    }
    dev_->api().CmdBindDescriptorSets(cb, pipe_->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      pipe_->layoutEntry->pipelineLayout, kVkSetTable0 + table, 1, &s->set, 0, nullptr);
}

// ====================================================================================================
// setConstants -- push-constant path (constantDwords[slot] != 0).
// ====================================================================================================
void VulkanRenderContext::setConstants(u32 slot, const void* data, u32 dwords) {
    if (!pipe_ || slot >= kMaxConstantSlots) { AVER_ERROR("[RHI.Vulkan] setConstants without a pipeline"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    const DescriptorLayoutEntry* le = pipe_->layoutEntry;
    if (!le) { AVER_ERROR("[RHI.Vulkan] setConstants: pipeline has no cached layout"); return; }
    if (le->layout.constantDwords[slot] == 0) {
        AVER_ERROR("[RHI.Vulkan] setConstants: slot {} declares constantDwords 0, so it is a root CBV — use setConstantBuffer", slot);
        return;
    }
    // b1 (kObjectConstantRegister) is always first/128 bytes by the fixed convention
    // PushConstantLayout::kObjectOffset/kObjectBytes name -- pushConstantLayout() does NOT populate
    // slotOffset[1]/slotBytes[1] for it (see PushConstantLayout's own comment), so this is the one
    // slot that must read the compile-time constants instead of the generic arrays.
    u32 offset, bytes;
    if (slot == kObjectConstantRegister) {
        offset = PushConstantLayout::kObjectOffset;
        bytes = PushConstantLayout::kObjectBytes;
    } else {
        offset = le->pushConstants.slotOffset[slot];
        bytes = le->pushConstants.slotBytes[slot];
    }
    const u32 declaredDwords = bytes / 4;
    u8 block[256] = {};
    const u32 n = declaredDwords < 64 ? declaredDwords : 64;
    const u32 copyDwords = dwords < n ? dwords : n;
    if (data && copyDwords) std::memcpy(block, data, static_cast<usize>(copyDwords) * sizeof(u32));
    dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL, offset, n * 4, block);
}

// ====================================================================================================
// setConstantBuffer -- dynamic-UBO path (constantDwords[slot] == 0). Rewrites binding `slot` of the
// current pipeline's kVkSetConstants set to point at a fresh ring suballocation, then re-binds the
// set so the change is visible to the next draw. See rebindConstantsSet's own comment on why a
// full rebind (not just a new dynamic offset) follows every write here: this file always bakes the
// real byte offset into the descriptor itself rather than trying to track "which buffer object did
// this binding last point at" across a ring regrow, a piece of state VulkanCommon.hpp's fixed
// member list has nowhere to hold (see the file banner).
// ====================================================================================================
void VulkanRenderContext::setConstantBuffer(u32 slot, const void* data, u32 bytes) {
    if (!pipe_ || slot >= kMaxConstantSlots) { AVER_ERROR("[RHI.Vulkan] setConstantBuffer without a pipeline"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    const DescriptorLayoutEntry* le = pipe_->layoutEntry;
    if (!le || le->layout.constantDwords[slot] != 0) {
        AVER_ERROR("[RHI.Vulkan] setConstantBuffer: slot {} declares {} root constants, not a CBV — use setConstants",
                   slot, le ? le->layout.constantDwords[slot] : 0u);
        return;
    }
    if (g_constantsSet == VK_NULL_HANDLE) {
        AVER_ERROR("[RHI.Vulkan] setConstantBuffer: no constants descriptor set is bound for the current pipeline");
        return;
    }
    const ConstantAllocation alloc = ringAlloc(data, bytes);
    if (!alloc.cpu) return;   // overflow already logged once this frame by ringAlloc

    VkDescriptorBufferInfo info{alloc.buffer, alloc.offset, bytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = g_constantsSet;
    w.dstBinding = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &info;
    dev_->api().UpdateDescriptorSets(dev_->vkDevice(), 1, &w, 0, nullptr);
    rebindConstantsSet(dev_->api(), cb, pipe_, g_constantsSet);
}

// ====================================================================================================
// setDrawBinding -- records the sticky per-draw binding; applied at the draw by applyDrawBinding().
// ====================================================================================================
void VulkanRenderContext::setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
    if (bytes > kMaxDrawConstantBytes) {
        AVER_ERROR("[RHI.Vulkan] setDrawBinding constant block is {} bytes, over the {} limit", bytes, kMaxDrawConstantBytes);
        return;
    }
    drawSet_ = set;
    drawConstantBytes_ = (constants && bytes) ? bytes : 0;
    if (drawConstantBytes_) std::memcpy(drawConstants_, constants, drawConstantBytes_);
}

// ====================================================================================================
// drawMesh -- draws a backend-owned mesh through the input assembler.
// ====================================================================================================
void VulkanRenderContext::drawMesh(MeshHandle mesh) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.Vulkan] drawMesh with an invalid mesh handle"); return; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    // A destroyed mesh's slot is cleared, never recycled (IDevice::destroyMesh's own contract) --
    // its vb/ib are VK_NULL_HANDLE at that point. D3D12's own drawMesh has no equivalent guard (a
    // null GPU virtual address there is merely an invalid root argument); passing VK_NULL_HANDLE to
    // vkCmdBindVertexBuffers/vkCmdBindIndexBuffer is more likely to be immediately fatal, so this
    // one check goes a little further than the reference for that reason alone -- not a behaviour
    // change for any handle that is actually alive.
    if (m.vb == VK_NULL_HANDLE || m.ib == VK_NULL_HANDLE) {
        AVER_ERROR("[RHI.Vulkan] drawMesh with a destroyed mesh handle");
        return;
    }
    applyDrawBinding();
    const VkDeviceSize zeroOffset = 0;
    dev_->api().CmdBindVertexBuffers(cb, 0, 1, &m.vb, &zeroOffset);
    dev_->api().CmdBindIndexBuffer(cb, m.ib, 0, VK_INDEX_TYPE_UINT32);
    beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawIndexed(cb, m.indexCount, 1, 0, 0, 0);
    endRenderScope(*dev_, cb);
}

// ====================================================================================================
// dispatchMeshFor -- draws a backend-owned mesh through the mesh-shader path.
// ====================================================================================================
void VulkanRenderContext::dispatchMeshFor(MeshHandle mesh) {
    if (!pipe_ || !pipe_->mesh) { AVER_ERROR("[RHI.Vulkan] dispatchMeshFor without a mesh-shader pipeline"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdDrawMeshTasksEXT) {
        AVER_ERROR("[RHI.Vulkan] vkCmdDrawMeshTasksEXT is unavailable on this device");
        return;
    }
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.Vulkan] dispatchMeshFor with an invalid mesh handle"); return; }
    const GpuMesh& m = dev_->meshes_[mesh - 1];
    const u32 tris = m.indexCount / 3;
    if (!tris) return;
    applyDrawBinding();
    const DescriptorLayoutEntry* le = pipe_->layoutEntry;
    if (!le || !le->mesh) { AVER_ERROR("[RHI.Vulkan] dispatchMeshFor: pipeline's cached layout is not a mesh layout"); return; }
    dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL,
                                 le->pushConstants.meshVertexAddrOffset, sizeof(VkDeviceAddress), &m.vbAddress);
    dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL,
                                 le->pushConstants.meshIndexAddrOffset, sizeof(VkDeviceAddress), &m.ibAddress);
    const u32 tc[4] = {tris, 0, 0, 0};
    dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL,
                                 le->pushConstants.meshCountOffset, sizeof(tc), tc);
    beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawMeshTasksEXT(cb, (tris + kMeshShaderTrisPerGroup - 1) / kMeshShaderTrisPerGroup, 1, 1);
    endRenderScope(*dev_, cb);
}

// ====================================================================================================
// dispatchMeshClusters -- amplification+mesh pipeline over one cluster cut. See RHIResources.hpp's
// own comment on why this is a separate entry point, and D3D12RenderContext::dispatchMeshClusters's
// "FIX" comment (reproduced below) for why every reserved push-constant range this pipeline layout
// carries gets real, valid bytes whenever there is something valid to put there.
// ====================================================================================================
void VulkanRenderContext::dispatchMeshClusters(MeshHandle mesh, u32 clusterCount) {
    if (!pipe_ || !pipe_->mesh || !pipe_->amplification) {
        AVER_ERROR("[RHI.Vulkan] dispatchMeshClusters without an amplification-shader pipeline");
        return;
    }
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdDrawMeshTasksEXT) {
        AVER_ERROR("[RHI.Vulkan] vkCmdDrawMeshTasksEXT is unavailable on this device");
        return;
    }
    if (clusterCount == 0) return;
    applyDrawBinding();
    const DescriptorLayoutEntry* le = pipe_->layoutEntry;
    if (!le || !le->mesh) { AVER_ERROR("[RHI.Vulkan] dispatchMeshClusters: pipeline's cached layout is not a mesh layout"); return; }
    // FIX (mirrors D3D12RenderContext::dispatchMeshClusters exactly): the pipeline layout this
    // pipeline was built with ALWAYS reserves the mesh-geometry push-constant ranges
    // (meshVertexAddrOffset/meshIndexAddrOffset/meshCountOffset), regardless of whether a
    // cluster-culling mesh shader actually reads all three. Leaving any of them unwritten is
    // undefined content read by whatever shader stage declares them -- the exact hazard the D3D12
    // reference calls out for an unset root argument, translated to push constants here.
    if (mesh != 0 && mesh <= dev_->meshes_.size() && dev_->meshes_[mesh - 1].alive) {
        const GpuMesh& m = dev_->meshes_[mesh - 1];
        dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL,
                                     le->pushConstants.meshVertexAddrOffset, sizeof(VkDeviceAddress), &m.vbAddress);
        dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL,
                                     le->pushConstants.meshIndexAddrOffset, sizeof(VkDeviceAddress), &m.ibAddress);
    }
    const u32 block[4] = {clusterCount, 0, 0, 0};
    dev_->api().CmdPushConstants(cb, le->pipelineLayout, VK_SHADER_STAGE_ALL,
                                 le->pushConstants.meshCountOffset, sizeof(block), block);
    const u32 groups = (clusterCount + kClusterAmplificationGroupSize - 1) / kClusterAmplificationGroupSize;
    beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawMeshTasksEXT(cb, groups, 1, 1);
    endRenderScope(*dev_, cb);
}

// ====================================================================================================
// dispatch -- compute. Deliberately never wrapped in a rendering scope: vkCmdDispatch must run
// OUTSIDE one, and since no VulkanRenderContext method ever leaves a scope open (see the file
// banner), this is automatically satisfied without an explicit check.
// ====================================================================================================
void VulkanRenderContext::dispatch(u32 gx, u32 gy, u32 gz) {
    if (!pipe_ || !pipe_->compute) { AVER_ERROR("[RHI.Vulkan] dispatch without a compute pipeline"); return; }
    VkCommandBuffer cb = cmd();
    if (cb) dev_->api().CmdDispatch(cb, gx, gy, gz);
}

// ====================================================================================================
// copyBuffer
// ====================================================================================================
void VulkanRenderContext::copyBuffer(BufferHandle dst, BufferHandle src, u64 bytes, u64 dstOffset, u64 srcOffset) {
    VkCommandBuffer cb = cmd();
    if (!cb || !res_) return;
    VkBuffer d = res_->bufferResource(dst);
    VkBuffer s = res_->bufferResource(src);
    if (!d || !s) { AVER_ERROR("[RHI.Vulkan] copyBuffer with an invalid handle"); return; }
    VkBufferCopy region{srcOffset, dstOffset, bytes};
    dev_->api().CmdCopyBuffer(cb, s, d, 1, &region);
}

// ====================================================================================================
// drawFullscreen -- the vertex shader builds its 3 vertices from gl_VertexIndex (SPIR-V's SV_VertexID
// equivalent, produced by the SAME DXC -spirv compile); no vertex buffer is bound.
// ====================================================================================================
void VulkanRenderContext::drawFullscreen() {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    beginRenderScope(*dev_, cb);
    dev_->api().CmdDraw(cb, 3, 1, 0, 0);
    endRenderScope(*dev_, cb);
}

// ====================================================================================================
// setVertexBuffer / setIndexBuffer -- caller-owned geometry path. Bound immediately, matching
// D3D12's own IASetVertexBuffers/IASetIndexBuffer-in-the-setter shape; boundVertexBuffer_/
// boundVertexStride_/boundIndexBuffer_ are kept too since VulkanCommon.hpp declares them.
// ====================================================================================================
void VulkanRenderContext::setVertexBuffer(BufferHandle h, u32 stride) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] setVertexBuffer with an invalid handle"); return; }
    if (stride == 0) { AVER_ERROR("[RHI.Vulkan] setVertexBuffer with a zero stride"); return; }
    boundVertexBuffer_ = b->buffer;
    boundVertexStride_ = stride;
    const VkDeviceSize zeroOffset = 0;
    dev_->api().CmdBindVertexBuffers(cb, 0, 1, &b->buffer, &zeroOffset);
}

// setIndexBuffer accepts Format::R32Uint ONLY -- mirrors what D3D12RenderContext::setIndexBuffer
// actually enforces (rejects anything else), not its own header comment's aspirational "R16Uint
// -equivalent" (RHIResources.hpp's own comment; see the contract scout's note on this exact
// ambiguity). Matching the D3D12 backend's REAL behaviour keeps both backends' rejected-input
// behaviour identical.
void VulkanRenderContext::setIndexBuffer(BufferHandle h, Format indexFormat) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    RhiBuffer* b = res_->buffer(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] setIndexBuffer with an invalid handle"); return; }
    if (indexFormat != Format::R32Uint) { AVER_ERROR("[RHI.Vulkan] setIndexBuffer needs Format::R32Uint"); return; }
    boundIndexBuffer_ = b->buffer;
    dev_->api().CmdBindIndexBuffer(cb, b->buffer, 0, VK_INDEX_TYPE_UINT32);
}

// ====================================================================================================
// drawIndexed -- draws from the currently bound caller-owned buffers.
// ====================================================================================================
void VulkanRenderContext::drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    VkCommandBuffer cb = cmd();
    if (!cb || indexCount == 0) return;
    applyDrawBinding();
    beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawIndexed(cb, indexCount, 1, firstIndex, baseVertex, 0);
    endRenderScope(*dev_, cb);
}

// ====================================================================================================
// buildBlas -- records a bottom-level acceleration structure build for its mesh.
// ====================================================================================================
void VulkanRenderContext::buildBlas(BlasHandle h) {
    RhiBlas* b = res_->blas(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] buildBlas with an invalid handle"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdBuildAccelerationStructuresKHR) {
        AVER_ERROR("[RHI.Vulkan] buildBlas without ray-tracing support");
        return;
    }
    if (b->mesh == 0 || b->mesh > dev_->meshes_.size()) return;
    const GpuMesh& m = dev_->meshes_[b->mesh - 1];

    VkAccelerationStructureGeometryTrianglesDataKHR tri{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;   // MeshVertex::px,py,pz -- see RHI.hpp's layout
    tri.vertexData.deviceAddress = m.vbAddress;
    tri.vertexStride = sizeof(MeshVertex);
    tri.maxVertex = m.vertexCount ? m.vertexCount - 1 : 0;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = m.ibAddress;

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.geometry.triangles = tri;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkBufferDeviceAddressInfo scratchInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    scratchInfo.buffer = b->scratchBuffer;

    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    bi.scratchData.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &scratchInfo);
    bi.dstAccelerationStructure = b->as;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = m.indexCount / 3;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    dev_->api().CmdBuildAccelerationStructuresKHR(cb, 1, &bi, &pRange);

    // The Vulkan analog of D3D12's UAV barrier on the freshly-built AS: order the build's writes
    // against whatever later reads it (a TLAS build referencing this BLAS, or an inline RayQuery).
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    dev_->api().CmdPipelineBarrier2(cb, &dep);
    b->built = true;
}

// ====================================================================================================
// buildTlas -- packs the instance buffer and records a top-level acceleration structure build.
// ====================================================================================================
void VulkanRenderContext::buildTlas(TlasHandle h, const TlasInstance* instances, u32 count) {
    RhiTlas* t = res_->tlas(h);
    if (!t) { AVER_ERROR("[RHI.Vulkan] buildTlas with an invalid handle"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdBuildAccelerationStructuresKHR) {
        AVER_ERROR("[RHI.Vulkan] buildTlas without ray-tracing support");
        return;
    }
    if (count > t->maxInstances) {
        AVER_WARN("[RHI.Vulkan] buildTlas: {} instances clamped to the {} this TLAS was sized for", count, t->maxInstances);
        count = t->maxInstances;
    }
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    if (!t->instancePtr[f]) return;

    auto* dst = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(t->instancePtr[f]);
    u32 written = 0;
    for (u32 i = 0; i < count && instances; ++i) {
        const RhiBlas* b = res_->blas(instances[i].blas);
        if (!b || !b->as) { AVER_WARN("[RHI.Vulkan] buildTlas: instance {} names an invalid BLAS", i); continue; }
        VkAccelerationStructureInstanceKHR id{};
        // Engine matrices are row-major/row-vector (v*M); VkTransformMatrixKHR is the same row-major
        // 3x4 [R|T] layout D3D12_RAYTRACING_INSTANCE_DESC::Transform already uses, so this is the
        // identical transpose D3D12RenderContext::buildTlas performs, not a Vulkan-specific one.
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) id.transform.matrix[r][c] = instances[i].world[c * 4 + r];
            id.transform.matrix[r][3] = instances[i].world[12 + r];
        }
        id.mask = instances[i].mask;
        // Rejected rather than truncated: instanceCustomIndex is a 24-bit bitfield, so a larger
        // value would silently alias onto another instance's id and a hit would resolve to the
        // wrong geometry -- identical reasoning to D3D12's own InstanceID rejection.
        if (instances[i].instanceId > kMaxTlasInstanceId) {
            AVER_ERROR("[RHI.Vulkan] buildTlas: instance {} has id {} which does not fit in 24 bits; "
                       "it is dropped rather than aliased onto another instance", i, instances[i].instanceId);
            continue;
        }
        // instanceCustomIndex (CommittedInstanceID() in HLSL), NOT
        // instanceShaderBindingTableRecordOffset -- the latter indexes a shader binding table this
        // backend never builds (VK_KHR_ray_query only, no VK_KHR_ray_tracing_pipeline; see the
        // contract's note on which extension the engine's inline RayQuery shaders actually need).
        id.instanceCustomIndex = instances[i].instanceId;
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addrInfo.accelerationStructure = b->as;
        id.accelerationStructureReference = dev_->api().GetAccelerationStructureDeviceAddressKHR(dev_->vkDevice(), &addrInfo);
        dst[written++] = id;
    }

    VkAccelerationStructureGeometryInstancesDataKHR instData{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    VkBufferDeviceAddressInfo instBufInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    instBufInfo.buffer = t->instanceBuffers[f];
    instData.data.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &instBufInfo);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = instData;

    VkBufferDeviceAddressInfo scratchInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    scratchInfo.buffer = t->scratchBuffer;

    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    bi.scratchData.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &scratchInfo);
    bi.dstAccelerationStructure = t->as;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = written;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    dev_->api().CmdBuildAccelerationStructuresKHR(cb, 1, &bi, &pRange);

    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    dev_->api().CmdPipelineBarrier2(cb, &dep);
}

// ====================================================================================================
// textureBarrier / bufferBarrier / uavBarrierTexture / uavBarrierBuffer -- built against
// VK_KHR_synchronization2 (core at kRequiredApiVersion), using VulkanCommon.hpp's
// toVkImageBarrierInfo/toVkBufferBarrierInfo ResourceState -> (layout, access, stage) tables.
// ====================================================================================================
void VulkanRenderContext::textureBarrier(TextureHandle h, ResourceState from, ResourceState to, u32 subresource) {
    if (rejectAsState(from, to, "textureBarrier")) return;
    RhiTexture* t = res_->texture(h);
    VkCommandBuffer cb = cmd();
    if (!t || !cb) { AVER_ERROR("[RHI.Vulkan] textureBarrier with an invalid handle"); return; }
#if AVER_RHI_TRACK_STATE
    trackTextureBarrier(*t, from, to, subresource);
#endif
    const VkImageBarrierInfo srcInfo = toVkImageBarrierInfo(from);
    const VkImageBarrierInfo dstInfo = toVkImageBarrierInfo(to);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcInfo.stage;   b.srcAccessMask = srcInfo.access;
    b.dstStageMask = dstInfo.stage;   b.dstAccessMask = dstInfo.access;
    b.oldLayout = srcInfo.layout;     b.newLayout = dstInfo.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange.aspectMask = toVkAspect(t->desc.format);
    if (subresource == kAllSubresources) {
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    } else {
        b.subresourceRange.baseMipLevel = subresource;
        b.subresourceRange.levelCount = 1;
    }
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    dev_->api().CmdPipelineBarrier2(cb, &dep);
}

void VulkanRenderContext::bufferBarrier(BufferHandle h, ResourceState from, ResourceState to) {
    if (rejectAsState(from, to, "bufferBarrier")) return;
    RhiBuffer* b = res_->buffer(h);
    VkCommandBuffer cb = cmd();
    if (!b || !cb) { AVER_ERROR("[RHI.Vulkan] bufferBarrier with an invalid handle"); return; }
#if AVER_RHI_TRACK_STATE
    trackBufferBarrier(*b, from, to);
#endif
    const VkBufferBarrierInfo srcInfo = toVkBufferBarrierInfo(from);
    const VkBufferBarrierInfo dstInfo = toVkBufferBarrierInfo(to);
    VkBufferMemoryBarrier2 bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bar.srcStageMask = srcInfo.stage;   bar.srcAccessMask = srcInfo.access;
    bar.dstStageMask = dstInfo.stage;   bar.dstAccessMask = dstInfo.access;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.buffer = b->buffer;
    bar.offset = 0;
    bar.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &bar;
    dev_->api().CmdPipelineBarrier2(cb, &dep);
}

void VulkanRenderContext::uavBarrierTexture(TextureHandle h) {
    RhiTexture* t = res_->texture(h);
    VkCommandBuffer cb = cmd();
    if (!t || !cb) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = VkImageSubresourceRange{toVkAspect(t->desc.format), 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    dev_->api().CmdPipelineBarrier2(cb, &dep);
}

void VulkanRenderContext::uavBarrierBuffer(BufferHandle h) {
    RhiBuffer* b = res_->buffer(h);
    VkCommandBuffer cb = cmd();
    if (!b || !cb) return;
    VkBufferMemoryBarrier2 bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bar.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    bar.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    bar.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.buffer = b->buffer;
    bar.offset = 0;
    bar.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &bar;
    dev_->api().CmdPipelineBarrier2(cb, &dep);
}

// ====================================================================================================
// pushMarker / popMarker -- VK_EXT_debug_utils labels; a silent no-op when the instance did not
// load the extension (optional, see VulkanCommon.hpp's kOptionalInstanceExtensions), exactly the
// same shape as these methods' own already-inert IRenderContext defaults.
//
// A DECLARED GAP, NOT A SILENT ONE: this pair opens and closes a PIX/RenderDoc-equivalent label and
// NOTHING ELSE. D3D12RenderContext's own pushMarker/popMarker (D3D12Device.cpp) do that AND issue a
// GPU timestamp on each side, folding the result into a per-label (now per-node -- see D3D12Device's
// GpuSpan/GpuAccum) running average an AVER_INFO line prints periodically. This backend has no
// counterpart: no VkQueryPool of VK_QUERY_TYPE_TIMESTAMP, no vkCmdWriteTimestamp2 either side of the
// label, no equivalent of D3D12Device::collectGpuTiming reading a resolved query back two frames
// late. A ScopedGpuStat (RHIResources.hpp) taken through THIS context still compiles and still runs
// -- it costs two calls into the no-ops above -- but it produces a debug-utils label for RenderDoc/
// Nsight and reports NO timing anywhere. Building the Vulkan half honestly means a query pool sized
// like tsHeap_, a write on each side of these two functions, and a resolve-and-average loop shaped
// like collectGpuTiming's, all of it scoped separately from this change (which was asked to give the
// D3D12 backend real GPU stats and price the GI cone trace on it, not to build a second timing engine
// for a backend that currently only paints debug labels). Recorded here loudly, in the file, rather
// than left to be discovered the day someone asks Vulkan's GPU timing report for a number it does not
// have -- a divergence this large staying unwritten is worse than the gap itself.
// ====================================================================================================
void VulkanRenderContext::pushMarker(const char* label) {
    VkCommandBuffer cb = cmd();
    if (!label || !cb || !dev_->api().CmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = label;
    dev_->api().CmdBeginDebugUtilsLabelEXT(cb, &l);
}
void VulkanRenderContext::popMarker() {
    VkCommandBuffer cb = cmd();
    if (cb && dev_->api().CmdEndDebugUtilsLabelEXT) dev_->api().CmdEndDebugUtilsLabelEXT(cb);
}

// ====================================================================================================
// ringAlloc -- suballocates transient upload memory from this frame's ring, at
// dev_->minUboAlignment(). Vulkan-flavoured twin of D3D12RenderContext::ringAlloc: same growth-at
// -frame-boundary discipline (see its header comment there for why growth never happens mid-frame),
// same "ask for headroom, not just what this call needed" over-allocation on overflow, same
// once-per-frame-not-once-per-call overflow logging. dev_->retireFenceValue() (nextTimelineValue_+1,
// incremented once per submitted frame) stands in for D3D12's nextFence_ as the epoch signal.
// ====================================================================================================
ConstantAllocation VulkanRenderContext::ringAlloc(const void* data, u32 bytes) {
    if (!data || bytes == 0) return {};
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    ConstantRing& ring = ring_[f];
    const u64 epoch = dev_->retireFenceValue();

    if (ringEpoch_ != epoch) {
        ringEpoch_ = epoch;
        ring.used = 0;
        if (ring.wanted > ring.bytes) {
            // Safe to drop only now: kFrameCount frames have retired since this buffer was last
            // recorded into, the same invariant that makes the cursor reset above safe.
            if (ring.buffer) destroyBufferCommitted(*dev_, ring.buffer, ring.memory);
            ring.buffer = VK_NULL_HANDLE;
            ring.memory = VK_NULL_HANDLE;
            ring.mapped = nullptr;
            ring.bytes = 0;
        }
    }

    if (!ring.buffer) {
        VkDeviceSize want = ring.wanted > kRhiRingBytes ? ring.wanted : kRhiRingBytes;
        if (want > kRhiRingMaxBytes) want = kRhiRingMaxBytes;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        bool coherent = true;
        if (!createBufferCommitted(*dev_, want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   buf, mem, nullptr, "rhi.vulkan constant ring")) {
            // Not every HOST_VISIBLE heap on every vendor is also HOST_COHERENT (see ConstantRing::
            // coherent's own comment) -- fall back to HOST_VISIBLE alone and flush explicitly below.
            coherent = false;
            if (!createBufferCommitted(*dev_, want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, buf, mem, nullptr,
                                       "rhi.vulkan constant ring")) {
                AVER_ERROR("[RHI.Vulkan] failed to grow the constant ring to {} KB", want / 1024);
                return {};
            }
        }
        void* ptr = nullptr;
        if (!vkOk(dev_->api().MapMemory(dev_->vkDevice(), mem, 0, want, 0, &ptr), "vkMapMemory (constant ring)") || !ptr) {
            destroyBufferCommitted(*dev_, buf, mem);
            return {};
        }
        ring.buffer = buf;
        ring.memory = mem;
        ring.mapped = static_cast<u8*>(ptr);
        ring.bytes = want;
        ring.coherent = coherent;
        if (want > kRhiRingBytes)
            AVER_INFO("[RHI.Vulkan] constant ring grown to {} KB for frame {}", want / 1024, f);
    }

    const VkDeviceSize align = dev_->minUboAlignment() ? dev_->minUboAlignment() : 256;
    const VkDeviceSize offset = (ring.used + align - 1) & ~(align - 1);
    const VkDeviceSize size = (static_cast<VkDeviceSize>(bytes) + align - 1) & ~(align - 1);
    if (offset + size > ring.bytes) {
        const VkDeviceSize want = (offset + size) * 2;
        if (want > ring.wanted) ring.wanted = want > kRhiRingMaxBytes ? kRhiRingMaxBytes : want;
        if (ringOverflowEpoch_ != ringEpoch_) {
            ringOverflowEpoch_ = ringEpoch_;
            if (ring.bytes >= kRhiRingMaxBytes)
                AVER_ERROR("[RHI.Vulkan] constant ring exhausted at its {} KB ceiling -- draws in this "
                          "frame are losing their constants. Reduce draw count or raise kRhiRingMaxBytes.",
                          kRhiRingMaxBytes / 1024);
            else
                AVER_WARN("[RHI.Vulkan] constant ring ({} KB) exhausted this frame; growing to {} KB",
                         ring.bytes / 1024, ring.wanted / 1024);
        }
        return {};
    }
    std::memcpy(ring.mapped + offset, data, bytes);
    if (!ring.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = ring.memory;
        range.offset = offset;
        range.size = size;
        dev_->api().FlushMappedMemoryRanges(dev_->vkDevice(), 1, &range);
    }
    ring.used = offset + size;
    ConstantAllocation out;
    out.buffer = ring.buffer;
    out.offset = offset;
    out.cpu = ring.mapped + offset;
    return out;
}

// ====================================================================================================
// bindDeclaredDescriptors -- gives every descriptor-CBV slot the pipeline declares a valid dynamic
// -offset binding before the next draw. Vulkan analog of D3D12RenderContext::bindDeclaredRootCbvs:
// slot 0 (b0) always gets this frame's real PerFrameCB; every OTHER declared slot gets the shared
// zero-filled buffer as a DEFAULT (so a draw issued before an explicit setConstantBuffer call reads
// zeros, not garbage or a validation-layer-visible unbound descriptor) -- setConstantBuffer()
// overwrites just that one binding later, per-slot, when the caller actually supplies data.
//
// A fresh VkDescriptorSet is allocated (from VulkanResourceFactory's shared, FREE_DESCRIPTOR_SET_BIT
// pool) on EVERY call rather than cached per (layout, frame-in-flight) pair: VulkanCommon.hpp's
// DescriptorLayoutEntry has no field to cache such a set in (only samplersSet, which is genuinely
// immutable-forever and so can be), and this class has no member to hold a cross-call cache either
// (see the file banner). The cost is one allocate + a handful of small descriptor writes per
// setPipeline() call, not per draw -- acceptable for a first, correctness-first pass; caching this
// per DescriptorLayoutEntry is the natural next optimisation once there is a place to put the cache.
// ====================================================================================================
void VulkanRenderContext::bindDeclaredDescriptors(const RhiPipeline* p) {
    VkCommandBuffer cb = cmd();
    if (!cb || !p || !p->layoutEntry) return;
    const DescriptorLayoutEntry& le = *p->layoutEntry;
    if (le.constantsSetLayout == VK_NULL_HANDLE) {
        // Every real pipeline declares at least b0 (RHIResources.hpp:209/340-341's reserved-slot
        // convention) -- reaching this means either a MockFactory-shaped pipeline with no constants
        // at all, or a genuine gap in how this pipeline was built. Either way there is nothing to
        // bind, so this call becomes a no-op rather than a hard failure.
        g_constantsSet = VK_NULL_HANDLE;
        return;
    }

    // Retire the PREVIOUS pipeline's constants set once the GPU is past every frame that could still
    // reference it -- deferred exactly like every other object this backend destroys.
    if (g_constantsSet != VK_NULL_HANDLE) {
        VkDescriptorSet stale = g_constantsSet;
        VulkanDevice* devPtr = dev_;
        VulkanResourceFactory* resPtr = res_;
        res_->retire([devPtr, resPtr, stale]() {
            VkDescriptorSet s = stale;
            devPtr->api().FreeDescriptorSets(devPtr->vkDevice(), resPtr->descriptorPool_, 1, &s);
        });
        g_constantsSet = VK_NULL_HANDLE;
    }

    if (zeroCB_ == VK_NULL_HANDLE) {
        if (!createBufferCommitted(*dev_, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   zeroCB_, zeroCBMemory_, nullptr, "rhi.vulkan zero CBV")) {
            AVER_ERROR("[RHI.Vulkan] failed to create the shared zero-filled CBV");
            return;
        }
        void* ptr = nullptr;
        if (vkOk(dev_->api().MapMemory(dev_->vkDevice(), zeroCBMemory_, 0, 256, 0, &ptr), "vkMapMemory (zero CBV)") && ptr) {
            std::memset(ptr, 0, 256);
            dev_->api().UnmapMemory(dev_->vkDevice(), zeroCBMemory_);
        }
    }

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = res_->descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &le.constantsSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!vkOk(dev_->api().AllocateDescriptorSets(dev_->vkDevice(), &ai, &set), "vkAllocateDescriptorSets (constants set)"))
        return;

    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    VkDescriptorBufferInfo infos[kMaxConstantSlots]{};
    VkWriteDescriptorSet writes[kMaxConstantSlots]{};
    u32 n = 0;
    for (u32 k = 0; k < kMaxConstantSlots; ++k) {
        if (le.layout.constantDwords[k] != 0) continue;
        infos[n].buffer = (k == kEngineFrameConstantRegister) ? dev_->frameCBs_[f] : zeroCB_;
        infos[n].offset = 0;
        infos[n].range = VK_WHOLE_SIZE;
        writes[n] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[n].dstSet = set;
        writes[n].dstBinding = k;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[n].pBufferInfo = &infos[n];
        ++n;
    }
    if (n) dev_->api().UpdateDescriptorSets(dev_->vkDevice(), n, writes, 0, nullptr);

    g_constantsSet = set;
    u32 zeroOffsets[kMaxConstantSlots] = {};
    dev_->api().CmdBindDescriptorSets(cb, p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      le.pipelineLayout, kVkSetConstants, 1, &set, n, zeroOffsets);
}

// ====================================================================================================
// applyDrawBinding -- binds the sticky per-draw state (table 1 + its b2 block), if the current
// pipeline declared anywhere to put it. Mirrors D3D12RenderContext::applyDrawBinding.
// ====================================================================================================
void VulkanRenderContext::applyDrawBinding() {
    if (!pipe_ || !pipe_->layoutEntry) return;
    const PipelineLayout& layout = pipe_->layoutEntry->layout;
    if (drawSet_ && (layout.srvCount1 || layout.uavCount1)) setBindingSet(drawSet_, 1);
    if (drawConstantBytes_ && layout.constantDwords[kDrawConstantRegister] == 0)
        setConstantBuffer(kDrawConstantRegister, drawConstants_, drawConstantBytes_);
}

} // namespace aver::rhi::vkb
