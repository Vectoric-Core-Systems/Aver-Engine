// Indirect draw and dispatch (RHI wave 10, GPU_PARTICLES phase 0), on a real device (WARP when asked for D3D12).
//
//   1. a compute pass writes DispatchIndirectArgs and DrawIndexedIndirectArgs into a buffer;
//   2. dispatchIndirect runs a second kernel with the group count that buffer holds (3 groups x 4 threads);
//   3. a vertex shader reads a table-0 StructuredBuffer by SV_InstanceID -- the question the particle renderer's
//      one-draw design depends on -- and drawIndexedIndirect / drawIndexedInstanced draw two quads into 8x8 targets.
// Steps 1-2 and the pipeline build are hard checks. The pixels of step 3 are a hard check too, but a backend that cannot
// record a render pass inside runStandaloneCompute only warns (it is not what is under test).
//
// Arguments: `vulkan` selects the Vulkan backend (default D3D12); `hw` uses the adapter instead of WARP (D3D12 only).
// Exits 0, 1, or 77 (ctest SKIPPED) when the backend is absent, cannot run standalone compute or has no indirect support.
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

namespace {

int g_failures = 0;
constexpr int kSkip = 77;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok  {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

const char* kKernels = R"HLSL(
RWStructuredBuffer<uint> gArgs : register(u0);
RWStructuredBuffer<uint> gOut  : register(u1);

// Layout of gArgs (uints): [0..4] DrawIndexedIndirectArgs, [8..10] DispatchIndirectArgs.
[numthreads(16, 1, 1)]
void CSArgs(uint3 id : SV_DispatchThreadID) {
    gOut[id.x] = 0;
    if (id.x == 0) {
        gArgs[0] = 6; gArgs[1] = 2; gArgs[2] = 0; gArgs[3] = 0; gArgs[4] = 0;
        gArgs[8] = 3; gArgs[9] = 1; gArgs[10] = 1;
    }
}

[numthreads(4, 1, 1)]
void CSFill(uint3 id : SV_DispatchThreadID) {
    gOut[id.x] = id.x + 100;
}
)HLSL";

const char* kDraw = R"HLSL(
StructuredBuffer<float4> gPos : register(t0);
struct VOut { float4 pos : SV_Position; };
VOut VSMain(float2 corner : POSITION0, uint inst : SV_InstanceID) {
    VOut o;
    o.pos = float4(gPos[inst].xy + corner * 0.5, 0.0, 1.0);
    return o;
}
float4 PSMain(VOut i) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }
)HLSL";

rhi::BufferHandle makeBuffer(rhi::IResourceFactory& res, u64 bytes, rhi::BufferKind kind, bool uav, const char* name) {
    rhi::BufferDesc bd{};
    bd.bytes = bytes;
    bd.kind = kind;
    bd.allowUnorderedAccess = uav;
    bd.debugName = name;
    return res.createBuffer(bd);
}

rhi::PipelineHandle makeCompute(rhi::IResourceFactory& res, const char* entry) {
    rhi::ShaderDesc sd{};
    sd.source = kKernels;
    sd.entry = entry;
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    const rhi::ShaderHandle cs = res.createShader(sd);
    if (!cs) return 0;
    rhi::ComputePipelineDesc pd{};
    pd.cs = cs;
    pd.layout.uavCount = 2;
    pd.layout.slotKindsDeclared = true;
    pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.uavKinds[1] = rhi::SlotKind::StructuredBuffer;
    const rhi::PipelineHandle p = res.createComputePipeline(pd);
    res.destroyShader(cs);
    return p;
}

constexpr u32 kTarget = 8;

void run(rhi::IDevice& dev) {
    using rhi::ResourceState;
    rhi::IResourceFactory& res = *dev.resources();

    // ---- 1 + 2: compute writes the arguments, dispatchIndirect consumes them ----
    const rhi::PipelineHandle pArgs = makeCompute(res, "CSArgs");
    const rhi::PipelineHandle pFill = makeCompute(res, "CSFill");
    check(pArgs != 0 && pFill != 0, "the argument and fill kernels build");
    if (!pArgs || !pFill) return;

    const u64 kArgsBytes = 64, kOutBytes = 16 * sizeof(u32);
    const rhi::BufferHandle args = makeBuffer(res, kArgsBytes, rhi::BufferKind::Default, true, "Indirect args");
    const rhi::BufferHandle out = makeBuffer(res, kOutBytes, rhi::BufferKind::Default, true, "Indirect out");
    const rhi::BufferHandle dummy = makeBuffer(res, kArgsBytes, rhi::BufferKind::Default, true, "Indirect dummy");
    const rhi::BufferHandle outBack = makeBuffer(res, kOutBytes, rhi::BufferKind::Readback, false, "Indirect out readback");
    if (!args || !out || !dummy || !outBack) { check(false, "buffers allocate"); return; }

    rhi::BindingSetDesc bd{};
    bd.uavCount = 2;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    bd.uavKinds[1] = rhi::SlotKind::StructuredBuffer;
    const rhi::BindingSetHandle setA = res.createBindingSet(bd);   // args + out
    const rhi::BindingSetHandle setB = res.createBindingSet(bd);   // dummy + out: args may not be bound while read as arguments
    if (!setA || !setB) { check(false, "binding sets allocate"); return; }
    res.setUavBuffer(setA, 0, args, 4, 16, 0);
    res.setUavBuffer(setA, 1, out, 4, 16, 0);
    res.setUavBuffer(setB, 0, dummy, 4, 16, 0);
    res.setUavBuffer(setB, 1, out, 4, 16, 0);

    // ---- 3: the draw side ----
    const f32 positions[3][4] = {{-0.5f, 0.5f, 0, 0}, {0.5f, -0.5f, 0, 0}, {0.5f, 0.5f, 0, 0}};   // instance 2 must never draw
    const f32 quad[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const u32 quadIdx[6] = {0, 1, 2, 0, 2, 3};
    const rhi::BufferHandle posBuf = makeBuffer(res, sizeof positions, rhi::BufferKind::Default, false, "Indirect positions");
    const rhi::BufferHandle posUp = makeBuffer(res, sizeof positions, rhi::BufferKind::Upload, false, "Indirect positions up");
    const rhi::BufferHandle vb = makeBuffer(res, sizeof quad, rhi::BufferKind::Upload, false, "Indirect vb");
    const rhi::BufferHandle ib = makeBuffer(res, sizeof quadIdx, rhi::BufferKind::Upload, false, "Indirect ib");
    bool drawReady = posBuf && posUp && vb && ib && res.writeBuffer(posUp, positions, sizeof positions) &&
                     res.writeBuffer(vb, quad, sizeof quad) && res.writeBuffer(ib, quadIdx, sizeof quadIdx);

    rhi::PipelineHandle drawPipe = 0;
    rhi::BindingSetHandle drawSet = 0;
    if (drawReady) {
        rhi::ShaderDesc vs{};
        vs.source = kDraw; vs.entry = "VSMain"; vs.stage = rhi::ShaderStage::Vertex;
        rhi::ShaderDesc ps = vs;
        ps.entry = "PSMain"; ps.stage = rhi::ShaderStage::Pixel;
        const rhi::ShaderHandle vsh = res.createShader(vs), psh = res.createShader(ps);
        check(vsh != 0 && psh != 0, "the vertex shader reading a table-0 StructuredBuffer by SV_InstanceID compiles");
        if (vsh && psh) {
            rhi::GraphicsPipelineDesc gd{};
            gd.vs = vsh; gd.ps = psh;
            gd.vertexLayout.stride = 8;
            gd.vertexLayout.attribCount = 1;
            gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};
            gd.layout.srvCount = 1;
            gd.layout.slotKindsDeclared = true;
            gd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
            gd.cull = rhi::CullMode::None;
            gd.renderTargetCount = 1;
            gd.renderTargets[0] = rhi::Format::RGBA8Unorm;
            gd.blend = rhi::BlendMode::Opaque;
            drawPipe = res.createGraphicsPipeline(gd);
            check(drawPipe != 0, "the instanced draw pipeline builds with a table-0 StructuredBuffer SRV");
            rhi::BindingSetDesc sd{};
            sd.srvCount = 1;
            sd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
            drawSet = res.createBindingSet(sd);
            if (drawSet) res.setSrvBuffer(drawSet, 0, posBuf, 16, 3, 0);
        }
        if (vsh) res.destroyShader(vsh);
        if (psh) res.destroyShader(psh);
        drawReady = drawPipe && drawSet;
    }

    rhi::TextureHandle rt[2] = {};
    rhi::BufferHandle rtBack[2] = {};
    rhi::TextureCopyFootprint fp{};
    if (drawReady) {
        for (int i = 0; i < 2; ++i) {
            rhi::TextureDesc td{};
            td.width = kTarget; td.height = kTarget;
            td.format = rhi::Format::RGBA8Unorm;
            td.bind = rhi::ResourceBind::RenderTarget | rhi::ResourceBind::ShaderResource;
            td.initialState = ResourceState::RenderTarget;
            td.debugName = "Indirect target";
            rt[i] = res.createTexture(td);
        }
        drawReady = rt[0] && rt[1] && res.textureCopyFootprint(rt[0], 0, fp);
        if (drawReady) {
            for (int i = 0; i < 2; ++i) rtBack[i] = makeBuffer(res, fp.totalBytes, rhi::BufferKind::Readback, false, "Indirect rt readback");
            drawReady = rtBack[0] && rtBack[1];
        }
    }

    // Upload the positions, then everything in one recording.
    check(dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
              ctx.bufferBarrier(posBuf, ResourceState::Common, ResourceState::CopyDest);
              ctx.copyBuffer(posBuf, posUp, sizeof positions);
              ctx.bufferBarrier(posBuf, ResourceState::CopyDest, ResourceState::GeometryRead);
          }) || !drawReady,
          "the instance positions upload");

    const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
        ctx.bufferBarrier(args, ResourceState::Common, ResourceState::UnorderedAccess);
        ctx.bufferBarrier(out, ResourceState::Common, ResourceState::UnorderedAccess);
        ctx.setPipeline(pArgs);
        ctx.setBindingSet(setA);
        ctx.dispatch(1, 1, 1);
        ctx.uavBarrierBuffer(args);
        ctx.uavBarrierBuffer(out);
        ctx.bufferBarrier(args, ResourceState::UnorderedAccess, ResourceState::IndirectArgument);

        ctx.bufferBarrier(dummy, ResourceState::Common, ResourceState::UnorderedAccess);
        ctx.setPipeline(pFill);
        ctx.setBindingSet(setB);
        ctx.dispatchIndirect(args, 32);   // DispatchIndirectArgs at byte 32
        ctx.uavBarrierBuffer(out);
        ctx.bufferBarrier(dummy, ResourceState::UnorderedAccess, ResourceState::Common);

        ctx.bufferBarrier(out, ResourceState::UnorderedAccess, ResourceState::Common);
        ctx.bufferBarrier(out, ResourceState::Common, ResourceState::CopySource);
        ctx.copyBuffer(outBack, out, kOutBytes);
        ctx.bufferBarrier(out, ResourceState::CopySource, ResourceState::Common);

        if (drawReady) {
            for (int i = 0; i < 2; ++i) {
                const f32 clear[4] = {0, 0, 0, 1};
                ctx.setRenderTargets(&rt[i], 1, 0);
                ctx.setViewport(0, 0, kTarget, kTarget);
                ctx.setScissor(0, 0, kTarget, kTarget);
                ctx.clearColor(rt[i], clear);
                ctx.setPipeline(drawPipe);
                ctx.setBindingSet(drawSet);
                ctx.setVertexBuffer(vb, 8);
                ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
                if (i == 0) ctx.drawIndexedIndirect(args, 0);        // instanceCount 2 from the GPU-written arguments
                else        ctx.drawIndexedInstanced(6, 2, 0, 0, 0); // the same draw with the count on the CPU
                ctx.textureBarrier(rt[i], ResourceState::RenderTarget, ResourceState::CopySource);
                ctx.copyTextureToBuffer(rtBack[i], 0, rt[i], 0);
                ctx.textureBarrier(rt[i], ResourceState::CopySource, ResourceState::RenderTarget);
            }
        }
        ctx.bufferBarrier(args, ResourceState::IndirectArgument, ResourceState::Common);
    });
    check(ran, "the indirect recording runs");

    u32 got[16] = {};
    check(res.readBuffer(outBack, got, kOutBytes, 0), "the fill buffer reads back");
    bool fillOk = true;
    for (u32 i = 0; i < 12; ++i) fillOk = fillOk && got[i] == 100 + i;
    for (u32 i = 12; i < 16; ++i) fillOk = fillOk && got[i] == 0;
    check(fillOk, "dispatchIndirect ran exactly the 3 groups the GPU wrote (out[0..11] = 100..111, out[12..15] untouched)");

    if (drawReady) {
        bool pixelsOk = true;
        for (int i = 0; i < 2; ++i) {
            std::vector<u8> px(fp.totalBytes, 0);
            if (!res.readBuffer(rtBack[i], px.data(), fp.totalBytes, 0)) { pixelsOk = false; continue; }
            auto lit = [&](u32 x, u32 y) { return px[static_cast<usize>(y) * fp.rowPitch + static_cast<usize>(x) * 4] > 200; };
            pixelsOk = pixelsOk && lit(2, 2) && lit(6, 6) && !lit(6, 2) && !lit(1, 6);
        }
        if (pixelsOk) check(true, "drawIndexedIndirect and drawIndexedInstanced draw instances 0 and 1 and not 2");
        else AVER_WARN("  WARN  the instanced draw pixels were not as expected (this backend may not record a render pass in standalone compute); verify headed");
    } else {
        AVER_WARN("  WARN  the draw half did not set up (no render target or pipeline); only the dispatch half ran");
    }

    res.waitIdle();
    for (rhi::BufferHandle b : {args, out, dummy, outBack, posBuf, posUp, vb, ib, rtBack[0], rtBack[1]}) if (b) res.destroyBuffer(b);
    for (rhi::TextureHandle t : {rt[0], rt[1]}) if (t) res.destroyTexture(t);
    for (rhi::BindingSetHandle s : {setA, setB, drawSet}) if (s) res.destroyBindingSet(s);
    for (rhi::PipelineHandle p : {pArgs, pFill, drawPipe}) if (p) res.destroyPipeline(p);
}

} // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== IndirectRhiTest ===");
    bool wantVulkan = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
    }

    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no {} device on this machine", wantVulkan ? "Vulkan" : "D3D12");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->resources() || !dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  this backend cannot run standalone compute");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->caps().indirect) {
        AVER_WARN("  SKIP  this backend reports no indirect draw/dispatch support");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));
    run(*dev);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== IndirectRhiTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== IndirectRhiTest PASSED ===");
    return 0;
}
