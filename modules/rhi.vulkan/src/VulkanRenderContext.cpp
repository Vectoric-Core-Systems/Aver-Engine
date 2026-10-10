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

#include <algorithm>
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

// Returns whether a scope was actually OPENED. False means one was already open -- VulkanDevice's
// own scene, overlay or upscale pass, with this draw coming from a feature it called from inside
// that pass -- and this draw simply records into it. Closing it here instead would leave the
// owner's remaining draws, and its own CmdEndRendering, outside any pass at all; see
// VulkanDevice::renderScopeDepth_ for the three validation errors that produced.
bool beginRenderScope(VulkanDevice& dev, VkCommandBuffer cb) {
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
    return dev.pushRenderScope(cb, ri);
}
void endRenderScope(VulkanDevice& dev, VkCommandBuffer cb, bool opened) {
    dev.popRenderScope(cb, opened);
}

// ------------------------------------------------------------------------------------------------
// The descriptor set currently backing set kVkSetConstants (b0's PerFrameCB dynamic-UBO binding,
// plus whatever other zero-constantDwords slot the current pipeline declares). Allocated fresh by
// bindDeclaredDescriptors() on every setPipeline() call, valid until the NEXT setPipeline() call --
// exactly the same lifetime as the `pipe_` member already has, which is why this can safely sit
// beside it as file-scope state rather than needing its own class member. See the file banner for
// why it cannot BE a class member.
VkDescriptorSet g_constantsSet = VK_NULL_HANDLE;

// WHAT THE CURRENT CONSTANTS SET HOLDS, slot by slot.
//
// Kept because setConstantBuffer can no longer rewrite the bound set in place: doing so is an
// update-after-bind on the command buffer that already bound it, which the layer reports as
//     VkDescriptorSet ... was destroyed or updated without UPDATE_AFTER_BIND
// and which puts that command buffer into an INVALID state -- after which the driver DROPS every
// call recorded later, the whole overlay and the editor UI with it. So a write takes a FRESH set
// instead, and a fresh set has to be populated in full rather than inheriting the one slot just
// written. Hence the mirror.
//
// A dynamic offset would have been cheaper and cannot do the job: these are
// UNIFORM_BUFFER_DYNAMIC, but setConstantBuffer points a slot at the per-frame constant RING, a
// different VkBuffer from the zero/frame CB the slot was declared with, and a dynamic offset
// cannot change which buffer a descriptor names.
VkDescriptorBufferInfo g_constantsInfos[kMaxConstantSlots]{};
bool g_constantsSlotLive[kMaxConstantSlots]{};

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
    // THE set for this frame, with any writes it missed replayed in first. Never s->sets[..] direct:
    // picking the ring slot and catching it up are the same decision. See RhiBindingSet.
    const VkDescriptorSet ringSet = res_->bindingSetForFrame(*s);
    if (ringSet == VK_NULL_HANDLE) { AVER_ERROR("[RHI.Vulkan] setBindingSet: the set has no ring slot for this frame"); return; }
    // Recorded so writeBindingSlot can catch a write-after-bind, which is the one thing a per-frame
    // ring cannot cover.
    s->lastBoundSerial = dev_->frameSerial();

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
                                      pipe_->layoutEntry->pipelineLayout, kVkSetTable0 + table, 1, &ringSet, 0, nullptr);
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

    // A FRESH SET, NOT A REWRITE OF THE BOUND ONE. g_constantsSet has already been bound into this
    // command buffer by bindDeclaredDescriptors, and updating a bound set invalidates the buffer --
    // see g_constantsInfos for the full account. Allocating here mirrors what that function already
    // does on every setPipeline, so this is the same cost and the same lifetime rule, not a new one.
    if (!g_constantsSlotLive[slot]) {
        AVER_ERROR("[RHI.Vulkan] setConstantBuffer: slot {} is not one this pipeline declares as a CBV", slot);
        return;
    }
    g_constantsInfos[slot] = VkDescriptorBufferInfo{alloc.buffer, alloc.offset, bytes};

    const VkDescriptorSet fresh = res_->allocConstantsSet(le->constantsSetLayout);
    if (fresh == VK_NULL_HANDLE) return;

    VkDescriptorBufferInfo infos[kMaxConstantSlots]{};
    VkWriteDescriptorSet writes[kMaxConstantSlots]{};
    u32 n = 0;
    for (u32 k = 0; k < kMaxConstantSlots; ++k) {
        if (!g_constantsSlotLive[k]) continue;
        infos[n] = g_constantsInfos[k];
        writes[n] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[n].dstSet = fresh;
        writes[n].dstBinding = k;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[n].pBufferInfo = &infos[n];
        ++n;
    }
    if (n) dev_->api().UpdateDescriptorSets(dev_->vkDevice(), n, writes, 0, nullptr);

    // The set this replaces stays valid for what is already recorded: its pool resets only when the
    // frame slot retires (allocConstantsSet).
    g_constantsSet = fresh;
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
// ====================================================================================================
// instanceAlloc -- ringAlloc's twin for the per-instance world matrices.
//
// A SECOND RING RATHER THAN A SECOND USAGE FLAG ON THE FIRST. ring_ is created
// VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT and a StructuredBuffer is a STORAGE buffer, so the same memory
// cannot serve both without widening every constant allocation's usage. Two rings also keep the two
// bump cursors independent, which matters because an instanced draw's payload (64 bytes per
// instance, thousands of instances) is orders of magnitude larger than a constant block and would
// otherwise push the constant ring into growth it does not need.
// ====================================================================================================
ConstantAllocation VulkanRenderContext::instanceAlloc(const void* data, u32 bytes) {
    if (!data || bytes == 0) return {};
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    ConstantRing& ring = instanceRing_[f];
    const u64 epoch = dev_->retireFenceValue();

    if (instanceEpoch_ != epoch) {
        instanceEpoch_ = epoch;
        ring.used = 0;
        if (ring.wanted > ring.bytes) {
            // Same invariant ringAlloc relies on: kFrameCount frames have retired since this buffer
            // was last recorded into, so dropping it now cannot pull memory out from under a submit.
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
        if (!createBufferCommitted(*dev_, want, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   buf, mem, nullptr, "rhi.vulkan instance ring")) {
            coherent = false;
            if (!createBufferCommitted(*dev_, want, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, buf, mem, nullptr,
                                       "rhi.vulkan instance ring")) {
                AVER_ERROR("[RHI.Vulkan] failed to grow the instance ring to {} KB", want / 1024);
                return {};
            }
        }
        void* ptr = nullptr;
        if (!vkOk(dev_->api().MapMemory(dev_->vkDevice(), mem, 0, want, 0, &ptr), "vkMapMemory (instance ring)") || !ptr) {
            destroyBufferCommitted(*dev_, buf, mem);
            return {};
        }
        ring.buffer = buf;
        ring.memory = mem;
        ring.mapped = static_cast<u8*>(ptr);
        ring.bytes = want;
        ring.coherent = coherent;
        if (want > kRhiRingBytes)
            AVER_INFO("[RHI.Vulkan] instance ring grown to {} KB for frame {}", want / 1024, f);
    }

    const VkDeviceSize align = dev_->minStorageAlignment() ? dev_->minStorageAlignment() : 256;
    const VkDeviceSize offset = (ring.used + align - 1) & ~(align - 1);
    const VkDeviceSize size = (static_cast<VkDeviceSize>(bytes) + align - 1) & ~(align - 1);
    if (offset + size > ring.bytes) {
        const VkDeviceSize want = (offset + size) * 2;
        if (want > ring.wanted) ring.wanted = want > kRhiRingMaxBytes ? kRhiRingMaxBytes : want;
        if (instanceOverflowEpoch_ != instanceEpoch_) {
            instanceOverflowEpoch_ = instanceEpoch_;
            if (ring.bytes >= kRhiRingMaxBytes)
                AVER_ERROR("[RHI.Vulkan] instance ring exhausted at its {} KB ceiling -- instanced draws "
                          "in this frame are being skipped.", kRhiRingMaxBytes / 1024);
            else
                AVER_WARN("[RHI.Vulkan] instance ring ({} KB) exhausted this frame; growing to {} KB",
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
// drawMeshInstanced -- one vkCmdDrawIndexed for every instance of one mesh.
// ====================================================================================================
void VulkanRenderContext::drawMeshInstanced(MeshHandle mesh, const f32* worlds, u32 instanceCount) {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    if (mesh == 0 || mesh > dev_->meshes_.size()) { AVER_ERROR("[RHI.Vulkan] drawMeshInstanced with an invalid mesh handle"); return; }
    if (!worlds || instanceCount == 0) return;

    // NOT BUILT FOR INSTANCING -> the base class's one-draw-per-instance loop, exactly as
    // D3D12RenderContext::drawMeshInstanced falls back when instanceWorldParam < 0. That fallback is
    // only correct when the bound shader reads gWorld, which is true precisely when the pipeline was
    // NOT built instanced -- the instanced entry points are inside #ifdef AVER_INSTANCE_SRV and
    // VoxiRenderer only compiles them for pipelines it also marks instanced.
    if (!pipe_ || !pipe_->instanced) {
        AVER_ERROR("[RHI.Vulkan] drawMeshInstanced against a pipeline built without "
                   "GraphicsPipelineDesc::instanced; falling back to {} individual draws", instanceCount);
        IRenderContext::drawMeshInstanced(mesh, worlds, instanceCount);
        return;
    }

    const GpuMesh& m = dev_->meshes_[mesh - 1];
    if (m.vb == VK_NULL_HANDLE || m.ib == VK_NULL_HANDLE) {
        AVER_ERROR("[RHI.Vulkan] drawMeshInstanced with a destroyed mesh handle");
        return;
    }

    const ConstantAllocation alloc = instanceAlloc(worlds, instanceCount * 16 * static_cast<u32>(sizeof(f32)));
    if (!alloc.buffer) return;   // ring exhausted this frame; instanceAlloc already logged it

    // A FRESH SET PER DRAW, retired after the frame. The alternative -- one set per frame plus a
    // dynamic offset -- needs the descriptor's RANGE fixed at bind time, and a range that must cover
    // the largest possible draw while still satisfying offset + range <= buffer size forces the ring
    // to carry a permanent tail of that size. Allocating here instead keeps the range exact
    // (offset..offset+bytes) whatever the instance count, and is the same shape setConstantBuffer
    // already uses on this backend. Instanced draws are rare -- one per distinct mesh per cascade --
    // so this is tens of sets a frame, not thousands.
    const DescriptorLayoutEntry& le = *pipe_->layoutEntry;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = res_->descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &le.instancesSetLayout;
    if (!vkOk(dev_->api().AllocateDescriptorSets(dev_->vkDevice(), &ai, &set), "rhi instance descriptor set"))
        return;

    // The dynamic offset carries the ring offset, so the descriptor itself starts at 0 and spans
    // exactly this draw's block. bufferInfo.offset stays 0 for the same reason the constants set's
    // does: a dynamic descriptor's offset field and its pDynamicOffsets entry ADD, and putting the
    // whole displacement in one of them keeps the alignment rule (minStorageBufferOffsetAlignment,
    // which instanceAlloc already rounded to) applying to a single number.
    VkDescriptorBufferInfo bi{alloc.buffer, 0, static_cast<VkDeviceSize>(instanceCount) * 64};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    w.pBufferInfo = &bi;
    dev_->api().UpdateDescriptorSets(dev_->vkDevice(), 1, &w, 0, nullptr);

    VulkanDevice* devPtr = dev_;
    VulkanResourceFactory* resPtr = res_;
    res_->retire([devPtr, resPtr, set]() {
        VkDescriptorSet dead = set;
        devPtr->api().FreeDescriptorSets(devPtr->vkDevice(), resPtr->descriptorPool_, 1, &dead);
    });

    applyDrawBinding();
    const u32 dynamicOffset = static_cast<u32>(alloc.offset);
    dev_->api().CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, le.pipelineLayout,
                                      kVkSetInstances, 1, &set, 1, &dynamicOffset);
    const VkDeviceSize zeroOffset = 0;
    dev_->api().CmdBindVertexBuffers(cb, 0, 1, &m.vb, &zeroOffset);
    dev_->api().CmdBindIndexBuffer(cb, m.ib, 0, VK_INDEX_TYPE_UINT32);
    const bool ownScope = beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawIndexed(cb, m.indexCount, instanceCount, 0, 0, 0);
    endRenderScope(*dev_, cb, ownScope);
}

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
    const bool ownScope = beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawIndexed(cb, m.indexCount, 1, 0, 0, 0);
    endRenderScope(*dev_, cb, ownScope);
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
    const bool ownScope = beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawMeshTasksEXT(cb, (tris + kMeshShaderTrisPerGroup - 1) / kMeshShaderTrisPerGroup, 1, 1);
    endRenderScope(*dev_, cb, ownScope);
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
    const bool ownScope = beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawMeshTasksEXT(cb, groups, 1, 1);
    endRenderScope(*dev_, cb, ownScope);
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

// Copies one whole texture into another. See IRenderContext::copyTexture for why whole-resource.
// Copies one mip of a texture into a buffer, and back. The caller owns the layout transitions
// (ResourceState::CopySource / CopyDest), exactly as it does for copyBuffer.
//
// LEGAL ONLY OUTSIDE A RENDERING SCOPE, which is a Vulkan rule rather than a choice here:
// vkCmdCopyImageToBuffer and vkCmdCopyBufferToImage are transfer commands and cannot be recorded
// between vkCmdBeginRendering and vkCmdEndRendering. That is satisfied by WHERE the one caller
// stands rather than by anything enforced here -- every render feature's prePass runs before
// VulkanDevice::beginFrame opens the scene scope (VulkanDevice.cpp: prePass at :2460, the scope at
// :2514), which is exactly where a GI bake reads its volume back. A caller that issued one of these
// from inside a draw pass would get a validation error naming the active render pass, and the fix
// would be to move the call, not to suspend the scope.
void VulkanRenderContext::copyTextureToBuffer(BufferHandle dst, u64 dstOffset, TextureHandle src, u32 mip) {
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdCopyImageToBuffer) return;
    RhiBuffer* d = res_->buffer(dst);
    RhiTexture* t = res_->texture(src);
    if (!d || !d->buffer || !t || !t->image) {
        AVER_ERROR("[RHI.Vulkan] copyTextureToBuffer with an invalid handle");
        return;
    }
    if (mip >= t->desc.mips) {
        AVER_ERROR("[RHI.Vulkan] copyTextureToBuffer: mip {} is past the {} this texture has", mip, t->desc.mips);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = dstOffset;
    // 0/0: rows exactly as wide as the image, which is what textureCopyFootprint reports.
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {toVkAspect(t->desc.format), mip, 0, 1};
    region.imageExtent = {t->desc.width  >> mip ? t->desc.width  >> mip : 1u,
                          t->desc.height >> mip ? t->desc.height >> mip : 1u,
                          t->desc.depth  >> mip ? t->desc.depth  >> mip : 1u};
    dev_->api().CmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     d->buffer, 1, &region);
}

void VulkanRenderContext::copyBufferToTexture(TextureHandle dst, u32 mip, BufferHandle src, u64 srcOffset) {
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdCopyBufferToImage) return;
    RhiTexture* t = res_->texture(dst);
    RhiBuffer* s = res_->buffer(src);
    if (!t || !t->image || !s || !s->buffer) {
        AVER_ERROR("[RHI.Vulkan] copyBufferToTexture with an invalid handle");
        return;
    }
    if (mip >= t->desc.mips) {
        AVER_ERROR("[RHI.Vulkan] copyBufferToTexture: mip {} is past the {} this texture has", mip, t->desc.mips);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = srcOffset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {toVkAspect(t->desc.format), mip, 0, 1};
    region.imageExtent = {t->desc.width  >> mip ? t->desc.width  >> mip : 1u,
                          t->desc.height >> mip ? t->desc.height >> mip : 1u,
                          t->desc.depth  >> mip ? t->desc.depth  >> mip : 1u};
    dev_->api().CmdCopyBufferToImage(cb, s->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     1, &region);
}

void VulkanRenderContext::copyTexture(TextureHandle dst, TextureHandle src) {
    VkCommandBuffer cb = cmd();
    if (!cb || !res_) return;
    RhiTexture* d = res_->texture(dst);
    RhiTexture* s = res_->texture(src);
    if (!d || !s) { AVER_ERROR("[RHI.Vulkan] copyTexture with an invalid handle"); return; }
    if (d->desc.width  != s->desc.width  || d->desc.height != s->desc.height ||
        d->desc.depth  != s->desc.depth  || d->desc.format != s->desc.format ||
        d->desc.mips   != s->desc.mips   || d->desc.dim    != s->desc.dim) {
        AVER_ERROR("[RHI.Vulkan] copyTexture between mismatched textures -- refused");
        return;
    }
    // ONE REGION PER MIP. vkCmdCopyImage takes explicit extents per region and does NOT derive them
    // from the subresource, so a mip chain copied with a single region at the base extent would
    // write every mip at mip 0's size -- past the end of every smaller one. D3D12's CopyResource
    // handles the whole chain itself, which is exactly the kind of asymmetry that leaves one backend
    // right and the other quietly corrupt.
    std::vector<VkImageCopy> regions;
    regions.reserve(d->desc.mips);
    for (u32 m = 0; m < d->desc.mips; ++m) {
        VkImageCopy r{};
        r.srcSubresource.aspectMask = toVkAspect(s->desc.format);
        r.srcSubresource.mipLevel = m;
        r.srcSubresource.baseArrayLayer = 0;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.extent.width  = (d->desc.width  >> m) ? (d->desc.width  >> m) : 1u;
        r.extent.height = (d->desc.height >> m) ? (d->desc.height >> m) : 1u;
        r.extent.depth  = d->desc.dim == TextureDim::Tex3D
                        ? ((d->desc.depth >> m) ? (d->desc.depth >> m) : 1u) : 1u;
        regions.push_back(r);
    }
    dev_->api().CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             static_cast<uint32_t>(regions.size()), regions.data());
}

// ====================================================================================================
// drawFullscreen -- the vertex shader builds its 3 vertices from gl_VertexIndex (SPIR-V's SV_VertexID
// equivalent, produced by the SAME DXC -spirv compile); no vertex buffer is bound.
// ====================================================================================================
void VulkanRenderContext::drawFullscreen() {
    VkCommandBuffer cb = cmd();
    if (!cb) return;
    const bool ownScope = beginRenderScope(*dev_, cb);
    dev_->api().CmdDraw(cb, 3, 1, 0, 0);
    endRenderScope(*dev_, cb, ownScope);
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
    const bool ownScope = beginRenderScope(*dev_, cb);
    dev_->api().CmdDrawIndexed(cb, indexCount, 1, firstIndex, baseVertex, 0);
    endRenderScope(*dev_, cb, ownScope);
}

// ====================================================================================================
// buildBlas / refitBlas -- records a bottom-level acceleration structure build, or an in-place
// update, for its mesh. recordBlasBuild is the shared tail: geometry description, scratch and the
// post-build barrier are identical either way, only mode/src differ (see its declaration).
// ====================================================================================================
void VulkanRenderContext::recordBlasBuild(RhiBlas& b, const GpuMesh& m, VkBuildAccelerationStructureModeKHR mode) {
    VkCommandBuffer cb = cmd();

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
    scratchInfo.buffer = b.scratchBuffer;

    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Same flags every time this structure is built or updated (required by both the D3D12 and
    // Vulkan update contracts -- RHIResources.hpp's comment above IRenderContext::refitBlas):
    // ALLOW_UPDATE_BIT_KHR whenever it was created updatable, whether this call is itself a build
    // or an update.
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
              (b.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    bi.mode = mode;
    // In-place update: source and destination are the SAME structure -- legal per the Vulkan spec
    // and the whole point of a refit, no second structure to age out of sync with this one.
    bi.srcAccelerationStructure = (mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) ? b.as : VK_NULL_HANDLE;
    bi.dstAccelerationStructure = b.as;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    bi.scratchData.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &scratchInfo);

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

    b.built = true;
    b.builtVertexCount = m.vertexCount;
    b.builtIndexCount = m.indexCount;
}

void VulkanRenderContext::buildBlas(BlasHandle h) {
    RhiBlas* b = res_->blas(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] buildBlas with an invalid handle"); return; }
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdBuildAccelerationStructuresKHR) {
        AVER_ERROR("[RHI.Vulkan] buildBlas without ray-tracing support");
        return;
    }
    if (b->mesh == 0 || b->mesh > dev_->meshes_.size()) return;
    if (!b->geometries.empty()) {
        if (!recordBlasBuildMulti(*b))
            AVER_ERROR("[RHI.Vulkan] buildBlas: multi-geometry BLAS {} names a mesh that is gone", h);
        return;
    }
    recordBlasBuild(*b, dev_->meshes_[b->mesh - 1], VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
}

bool VulkanRenderContext::recordBlasBuildMulti(RhiBlas& b) {
    std::vector<VkAccelerationStructureGeometryKHR> geoms(b.geometries.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(b.geometries.size());
    for (usize i = 0; i < b.geometries.size(); ++i) {
        const MeshHandle h = b.geometries[i].mesh;
        if (h == 0 || h > dev_->meshes_.size() || !dev_->meshes_[h - 1].alive) return false;
        geoms[i] = vkMultiBlasGeometry(dev_->meshes_[h - 1], b.geometries[i].opaque);
        ranges[i] = VkAccelerationStructureBuildRangeInfoKHR{};
        const u32 ic = dev_->meshes_[h - 1].indexCount;
        const u32 first = std::min(b.geometries[i].firstIndex, ic);
        const u32 count = b.geometries[i].indexCount ? std::min(b.geometries[i].indexCount, ic - first) : ic - first;
        ranges[i].primitiveOffset = first * static_cast<u32>(sizeof(u32));   // bytes into the index data
        ranges[i].primitiveCount = count / 3;
    }
    VkCommandBuffer cb = cmd();
    VkBufferDeviceAddressInfo scratchInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    scratchInfo.buffer = b.scratchBuffer;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;   // createBlasMulti's size query's flags
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.dstAccelerationStructure = b.as;
    bi.geometryCount = static_cast<u32>(geoms.size());
    bi.pGeometries = geoms.data();
    bi.scratchData.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &scratchInfo);
    const VkAccelerationStructureBuildRangeInfoKHR* pRanges = ranges.data();
    dev_->api().CmdBuildAccelerationStructuresKHR(cb, 1, &bi, &pRanges);

    // Same post-build barrier recordBlasBuild records.
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
    b.built = true;
    return true;
}

// refitBlas -- updates `h`'s BLAS from its mesh's CURRENT vertices instead of rebuilding from
// scratch, when eligible (created updatable, already built, mesh's vertex/index counts unchanged
// since that build -- see RHIResources.hpp's contract comment above IRenderContext::refitBlas).
// Otherwise falls back to a full build, exactly like the default this overrides.
bool VulkanRenderContext::refitBlas(BlasHandle h) {
    RhiBlas* b = res_->blas(h);
    if (!b) { AVER_ERROR("[RHI.Vulkan] refitBlas with an invalid handle"); return false; }
    if (b->mesh == 0 || b->mesh > dev_->meshes_.size()) { buildBlas(h); return false; }
    // A createBlasMulti structure is never updatable: the refitBlas contract's full-build fallback.
    if (!b->geometries.empty()) { buildBlas(h); return false; }
    const GpuMesh& m = dev_->meshes_[b->mesh - 1];
    const bool countsChanged = m.vertexCount != b->builtVertexCount || m.indexCount != b->builtIndexCount;
    // Same guard as D3D12RenderContext::refitBlas: a BUILT structure whose mesh changed counts falls back
    // to a full build into buffers sized once at creation -- refuse it when the mesh has outgrown them,
    // rather than write past them. Skinned meshes (what refit is for) never change counts.
    if (b->built && countsChanged && dev_->api().GetAccelerationStructureBuildSizesKHR) {
        VkAccelerationStructureGeometryTrianglesDataKHR tri{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = m.vbAddress;
        tri.vertexStride = sizeof(MeshVertex);
        tri.maxVertex = m.vertexCount ? m.vertexCount - 1 : 0;
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = m.ibAddress;
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = tri;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                   (b->allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geom;
        const u32 primCount = m.indexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        dev_->api().GetAccelerationStructureBuildSizesKHR(dev_->vkDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                          &bi, &primCount, &sizes);
        const VkDeviceSize scratchNeed = b->allowUpdate ? std::max(sizes.buildScratchSize, sizes.updateScratchSize)
                                                        : sizes.buildScratchSize;
        if (sizes.accelerationStructureSize > b->asSize || scratchNeed > b->scratchSize) {
            AVER_ERROR("[RHI.Vulkan] refitBlas: mesh {} moved from {}v/{}i to {}v/{}i, past what its BLAS was "
                       "allocated for at creation -- rebuilding it in place would write past that allocation, "
                       "so this refit is refused; the caller must destroy and recreate the BLAS",
                       b->mesh, b->builtVertexCount, b->builtIndexCount, m.vertexCount, m.indexCount);
            return false;
        }
    }
    const bool eligible = b->allowUpdate && b->built && !countsChanged;
    if (!eligible) { buildBlas(h); return false; }

    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdBuildAccelerationStructuresKHR) {
        AVER_ERROR("[RHI.Vulkan] refitBlas without ray-tracing support");
        return false;
    }
    recordBlasBuild(*b, m, VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR);
    return true;
}

// ====================================================================================================
// buildTlas / refitTlas -- packs the instance buffer and records a top-level acceleration structure
// build, or an in-place update. packTlasInstances is the shared packing loop (buildTlas always used
// to do this inline; refitTlas needs the exact same filtering to decide whether an update is even
// legal), recordTlasBuild the shared build/update tail (mirrors recordBlasBuild above).
// ====================================================================================================
u32 VulkanRenderContext::packTlasInstances(RhiTlas& t, const TlasInstance* instances, u32 count,
                                           const char* caller, std::vector<RhiTlasSlot>& outSlots) {
    outSlots.clear();
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    if (!t.instancePtr[f]) return 0;
    outSlots.reserve(count);

    auto* dst = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(t.instancePtr[f]);
    u32 written = 0;
    for (u32 i = 0; i < count && instances; ++i) {
        const RhiBlas* b = res_->blas(instances[i].blas);
        if (!b || !b->as) { AVER_WARN("[RHI.Vulkan] {}: instance {} names an invalid BLAS", caller, i); continue; }
        // Rejected rather than truncated: instanceCustomIndex is a 24-bit bitfield, so a larger
        // value would silently alias onto another instance's id and a hit would resolve to the
        // wrong geometry -- identical reasoning to D3D12's own InstanceID rejection.
        if (instances[i].instanceId > kMaxTlasInstanceId) {
            AVER_ERROR("[RHI.Vulkan] {}: instance {} has id {} which does not fit in 24 bits; "
                       "it is dropped rather than aliased onto another instance", caller, i, instances[i].instanceId);
            continue;
        }
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addrInfo.accelerationStructure = b->as;
        const VkAccelerationStructureInstanceKHR id = vkInstanceFromTlas(
            instances[i], dev_->api().GetAccelerationStructureDeviceAddressKHR(dev_->vkDevice(), &addrInfo));
        dst[written++] = id;
        outSlots.push_back({id.accelerationStructureReference, static_cast<u32>(id.flags), id.mask});
    }
    return written;
}

// See the declaration. The prefix's BLASes are checked here, per build, because destroyMesh can take
// one away at any time and the prefix holds raw device addresses -- O(distinct BLASes), not O(prefix).
u32 VulkanRenderContext::usableStaticPrefix(RhiTlas& t) {
    if (t.staticCount == 0 || !res_->buffer(t.staticDescs)) return 0;
    for (BlasHandle b : t.staticBlases) {
        if (res_->blas(b)) continue;
        if (!t.staticBrokenLogged) {
            AVER_ERROR("[RHI.Vulkan] TLAS static prefix names BLAS {}, destroyed since it was set -- its {} "
                       "instance(s) are left out of every build until the prefix is replaced or removed",
                       b, t.staticCount);
            t.staticBrokenLogged = true;
        }
        return 0;
    }
    return t.staticCount;
}

void VulkanRenderContext::recordTlasBuild(RhiTlas& t, u32 staticUsed, u32 written, VkBuildAccelerationStructureModeKHR mode) {
    VkCommandBuffer cb = cmd();
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;

    VkAccelerationStructureGeometryInstancesDataKHR instData{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    VkBufferDeviceAddressInfo instBufInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    instBufInfo.buffer = t.instanceBuffers[f];
    if (staticUsed) {
        // THE STATIC PREFIX: this frame's instances copied in at slot staticCount, the prefix's own
        // slots never written again. First barrier: the copy waits for whatever read the buffer last (the
        // previous build, a shader); second: the build and this frame's shaders wait for the copy -- and,
        // on the first build, for setTlasStaticInstances' one-shot upload, earlier on this same queue.
        const VkBuffer descs = res_->buffer(t.staticDescs)->buffer;
        const auto barrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkBufferMemoryBarrier2 bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            bar.srcStageMask = srcStage;   bar.srcAccessMask = srcAccess;
            bar.dstStageMask = dstStage;   bar.dstAccessMask = dstAccess;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.buffer = descs;
            bar.offset = 0;
            bar.size = VK_WHOLE_SIZE;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers = &bar;
            dev_->api().CmdPipelineBarrier2(cb, &dep);
        };
        const VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        const VkAccessFlags2 kReads = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        if (written) {
            barrier(kReaders, kReads, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferCopy region{0, static_cast<VkDeviceSize>(staticUsed) * sizeof(VkAccelerationStructureInstanceKHR),
                                static_cast<VkDeviceSize>(written) * sizeof(VkAccelerationStructureInstanceKHR)};
            dev_->api().CmdCopyBuffer(cb, t.instanceBuffers[f], descs, 1, &region);
        }
        barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReaders, kReads);
        instBufInfo.buffer = descs;
    }
    instData.data.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &instBufInfo);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = instData;

    VkBufferDeviceAddressInfo scratchInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    scratchInfo.buffer = t.scratchBuffer;

    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // Same flags every time -- see recordBlasBuild's identical comment.
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
              (t.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    bi.mode = mode;
    // In-place update: source and destination are the SAME structure, as legal here as it is for
    // a BLAS (recordBlasBuild) -- refitTlas's whole reason to exist.
    bi.srcAccelerationStructure = (mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) ? t.as : VK_NULL_HANDLE;
    bi.dstAccelerationStructure = t.as;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    bi.scratchData.deviceAddress = dev_->api().GetBufferDeviceAddress(dev_->vkDevice(), &scratchInfo);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = staticUsed + written;
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

    t.built = true;
    t.builtStatic = staticUsed;
}

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

    const u32 written = packTlasInstances(*t, instances, count, "buildTlas", t->pendingSlots);
    recordTlasBuild(*t, usableStaticPrefix(*t), written, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
    t->builtSlots.swap(t->pendingSlots);
}

// refitTlas -- updates `h`'s TLAS in place when the filtered instance list (after the SAME
// filtering packTlasInstances/buildTlas has always applied) has the same count as the last
// build/refit AND every slot still names the same BLAS with the same flags and mask; transforms and
// instance ids may differ freely. Otherwise falls back to a full build, exactly like the default
// this overrides. See RHIResources.hpp's contract comment above IRenderContext::refitTlas.
bool VulkanRenderContext::refitTlas(TlasHandle h, const TlasInstance* instances, u32 count) {
    RhiTlas* t = res_->tlas(h);
    if (!t) { AVER_ERROR("[RHI.Vulkan] refitTlas with an invalid handle"); return false; }
    if (!t->allowUpdate || !t->built) { buildTlas(h, instances, count); return false; }
    VkCommandBuffer cb = cmd();
    if (!cb || !dev_->api().CmdBuildAccelerationStructuresKHR) {
        AVER_ERROR("[RHI.Vulkan] refitTlas without ray-tracing support");
        return false;
    }
    if (count > t->maxInstances) {
        AVER_WARN("[RHI.Vulkan] refitTlas: {} instances clamped to the {} this TLAS was sized for", count, t->maxInstances);
        count = t->maxInstances;
    }
    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    if (!t->instancePtr[f]) { buildTlas(h, instances, count); return false; }

    // Packed regardless of eligibility below -- an update needs the freshly packed buffer exactly
    // as much as a full build would, and this is the ONLY way to know the new filtered shape to
    // compare against builtSlots.
    std::vector<RhiTlasSlot>& slots = t->pendingSlots;
    const u32 written = packTlasInstances(*t, instances, count, "refitTlas", slots);
    const u32 staticUsed = usableStaticPrefix(*t);

    // The prefix's own slots are identical build to build by construction, so only the per-frame
    // slots are compared -- plus the prefix length itself, which a dropped prefix changes.
    const bool sameShape = written == t->builtSlots.size() && staticUsed == t->builtStatic &&
        std::equal(slots.begin(), slots.end(), t->builtSlots.begin(),
                   [](const RhiTlasSlot& a, const RhiTlasSlot& b) {
                       return a.blasAddress == b.blasAddress && a.flags == b.flags && a.mask == b.mask;
                   });

    recordTlasBuild(*t, staticUsed, written, sameShape ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                                        : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
    t->builtSlots.swap(t->pendingSlots);
    return sameShape;
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
    // A RECORD CAN EXIST WITHOUT A RESOURCE, and this guard is what stops that becoming a barrier
    // against VK_NULL_HANDLE -- "pImageMemoryBarriers[0].image Invalid VkImage Object 0x0", which
    // the layer reports at the barrier and not at whatever left the image null. Naming the texture
    // here is the difference between a one-line fix and a hunt.
    if (!t->image) {
        AVER_ERROR("[RHI.Vulkan] textureBarrier on '{}' whose VkImage is null -- the texture record "
                   "exists but its resource does not; the barrier is skipped",
                   t->debugName.empty() ? "<unnamed>" : t->debugName);
        return;
    }
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
    if (!t->image) {
        AVER_ERROR("[RHI.Vulkan] uavBarrierTexture on '{}' whose VkImage is null",
                   t->debugName.empty() ? "<unnamed>" : t->debugName);
        return;
    }
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
VulkanRenderContext::~VulkanRenderContext() {
    // Safe without a wait of its own: VulkanDevice::~VulkanDevice calls waitForGpu() before it
    // deletes this, so nothing in flight still reads the ring.
    for (ConstantRing& r : ring_) {
        // Explicitly, even though vkFreeMemory unmaps implicitly -- the mapping is this class's, and
        // dropping it here keeps the pairing with the MapMemory in ringAlloc visible.
        if (r.memory && r.mapped) dev_->api().UnmapMemory(dev_->vkDevice(), r.memory);
        r.mapped = nullptr;
        destroyBufferCommitted(*dev_, r.buffer, r.memory);
        r.buffer = VK_NULL_HANDLE;
        r.memory = VK_NULL_HANDLE;
        r.bytes = r.used = 0;
    }
    for (ConstantRing& r : instanceRing_) {
        if (r.memory && r.mapped) dev_->api().UnmapMemory(dev_->vkDevice(), r.memory);
        r.mapped = nullptr;
        destroyBufferCommitted(*dev_, r.buffer, r.memory);
        r.buffer = VK_NULL_HANDLE;
        r.memory = VK_NULL_HANDLE;
        r.bytes = r.used = 0;
    }
    destroyBufferCommitted(*dev_, zeroCB_, zeroCBMemory_);
    zeroCB_ = VK_NULL_HANDLE;
    zeroCBMemory_ = VK_NULL_HANDLE;
}

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
// A fresh VkDescriptorSet is allocated (VulkanResourceFactory::allocConstantsSet, per-frame-slot pools
// reset when the slot retires) on EVERY call rather than cached per (layout, frame-in-flight) pair: VulkanCommon.hpp's
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

    // The PREVIOUS pipeline's set is reclaimed with its frame slot's pools (allocConstantsSet).
    g_constantsSet = VK_NULL_HANDLE;

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

    const VkDescriptorSet set = res_->allocConstantsSet(le.constantsSetLayout);
    if (set == VK_NULL_HANDLE) return;

    const u32 f = dev_->frameIndexInFlight() < kFrameCount ? dev_->frameIndexInFlight() : 0;
    VkDescriptorBufferInfo infos[kMaxConstantSlots]{};
    VkWriteDescriptorSet writes[kMaxConstantSlots]{};
    u32 n = 0;
    for (bool& live : g_constantsSlotLive) live = false;
    for (u32 k = 0; k < kMaxConstantSlots; ++k) {
        if (le.layout.constantDwords[k] != 0) continue;
        infos[n].buffer = (k == kEngineFrameConstantRegister) ? dev_->frameCBs_[f] : zeroCB_;
        infos[n].offset = 0;
        infos[n].range = VK_WHOLE_SIZE;
        // Mirrored so setConstantBuffer can rebuild a full set from it.
        g_constantsInfos[k] = infos[n];
        g_constantsSlotLive[k] = true;
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

    // ---- the IMMUTABLE sampler set, once per pipeline bind ----------------------------------------
    //
    // Nothing is ever WRITTEN to it -- pImmutableSamplers bakes every VkSampler into the layout, so
    // descriptorLayout() allocates and fills it exactly once. But "never written" is not "never
    // bound": a pipeline whose shader names a sampler statically uses set 3, and Vulkan requires
    // every set a pipeline statically uses to be bound before the draw. It was not, and the layer
    // said so the moment samplers stopped colliding with table 0 and moved to their own set:
    //     "The VkPipeline statically uses descriptor set 3, but all sets 0 to 3 are not compatible
    //      ... The set (3) is out of bounds for the number of sets bound (3)"
    // Binding it here rather than per draw costs one call per setPipeline and nothing per draw,
    // which is the whole point of the set being immutable.
    if (le.samplersSet != VK_NULL_HANDLE)
        dev_->api().CmdBindDescriptorSets(cb, p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          le.pipelineLayout, kVkSetSamplers, 1, &le.samplersSet, 0, nullptr);
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
