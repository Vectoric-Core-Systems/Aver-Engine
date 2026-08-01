// The skinning compute pass: rest vertices plus bone matrices in, posed vertices out.
#include "aver/render/SkinningPass.hpp"
#include "aver/core/Log.hpp"

#include <cstring>

namespace aver::render {

namespace {

// b0 is the engine per-frame block and b1/b2 belong to the raster path, so the first slot a compute
// pass may declare for itself is b3.
constexpr u32 kSkinConstantSlot = 3;

// Bytes per element of the two static streams and the bone buffer. These are the strides handed to
// setSrvBuffer, and a structured-buffer view with a stride that disagrees with the shader's struct
// reads the wrong vertices rather than failing.
constexpr u32 kBindStride = 32;   // uint4 joints + float4 weights
constexpr u32 kBoneStride = 64;   // four float4 rows

// Linear-blend skinning, matching anim::skinVertices term for term. The matrices are the ENGINE's
// row-vector convention -- v * M -- so a transformed point is a linear combination of the matrix's
// ROWS, which is what makes the four-row struct the natural layout rather than a transposed one.
const char* kSkinHLSL = R"HLSL(
struct SkinVertex { float3 pos; float3 nrm; };
struct SkinBind   { uint4  joints; float4 weights; };
struct SkinBone   { float4 r0; float4 r1; float4 r2; float4 r3; };

StructuredBuffer<SkinVertex>   gRest  : register(t0);
StructuredBuffer<SkinBind>     gBind  : register(t1);
StructuredBuffer<SkinBone>     gBones : register(t2);
RWStructuredBuffer<SkinVertex> gOut   : register(u0);

cbuffer SkinParams : register(b3) {
    uint gVertexCount;
    uint gBoneCount;
    uint gSkinPad0;
    uint gSkinPad1;
};

[numthreads(64, 1, 1)]
void CSSkin(uint3 tid : SV_DispatchThreadID) {
    const uint v = tid.x;
    if (v >= gVertexCount) return;

    const SkinVertex r = gRest[v];
    const SkinBind   b = gBind[v];

    float3 p = float3(0, 0, 0);
    float3 n = float3(0, 0, 0);
    float  used = 0.0f;

    [unroll] for (uint i = 0; i < 4; ++i) {
        const float w = b.weights[i];
        const uint  j = b.joints[i];
        // Zero weight and an out-of-range bone are the same case: the influence does not exist.
        if (w == 0.0f || j >= gBoneCount) continue;

        const SkinBone m = gBones[j];
        p += w * (r.pos.x * m.r0 + r.pos.y * m.r1 + r.pos.z * m.r2 + m.r3).xyz;
        n += w * (r.nrm.x * m.r0 + r.nrm.y * m.r1 + r.nrm.z * m.r2).xyz;
        used += w;
    }

    // No surviving influence means the vertex is unrigged, not at the origin.
    if (used == 0.0f) { p = r.pos; n = r.nrm; }

    const float len = length(n);
    if (len > 1e-8f) n /= len;

    SkinVertex o;
    o.pos = p;
    o.nrm = n;
    gOut[v] = o;
}
)HLSL";

} // namespace

void skinTransition(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, rhi::ResourceState to) {
    if (!m.out || m.outState == to) return;
    ctx.bufferBarrier(m.out, m.outState, to);
    m.outState = to;
}

SkinningPass::~SkinningPass() { shutdown(); }

bool SkinningPass::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;

    rhi::ShaderDesc sd;
    sd.source = kSkinHLSL;
    sd.entry  = "CSSkin";
    sd.stage  = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    cs_ = res_->createShader(sd);
    if (!cs_) { AVER_ERROR("[Skin] skinning shader would not compile"); shutdown(); return false; }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout.srvCount = 3;
    pd.layout.uavCount = 1;
    pd.layout.constantDwords[kSkinConstantSlot] = 4;
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[Skin] skinning pipeline unavailable"); shutdown(); return false; }

    if (!createBoneRing()) { shutdown(); return false; }

    AVER_INFO("[Skin] GPU skinning ready ({} bones max, {} vertices per group)",
              kSkinMaxBones, kSkinGroupSize);
    return true;
}

// Allocates the per-frame bone buffers and the binding sets that name them. Both are a ring for the
// same reason: writing either while the GPU still reads last frame's is a use-after-write.
bool SkinningPass::createBoneRing() {
    rhi::BindingSetDesc bd;
    bd.srvCount = 3;
    bd.uavCount = 1;
    bd.srvKinds[0] = bd.srvKinds[1] = bd.srvKinds[2] = rhi::SlotKind::StructuredBuffer;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;

    for (u32 i = 0; i < kRing; ++i) {
        rhi::BufferDesc b;
        b.bytes = static_cast<u64>(kSkinMaxBones) * kBoneStride;
        b.kind  = rhi::BufferKind::Upload;
        b.debugName = "skin bones";
        bones_[i] = res_->createBuffer(b);
        sets_[i]  = res_->createBindingSet(bd);
        if (!bones_[i] || !sets_[i]) { AVER_ERROR("[Skin] bone ring allocation failed"); return false; }
    }
    return true;
}

void SkinningPass::shutdown() {
    if (res_) {
        for (u32 i = 0; i < kRing; ++i) {
            if (sets_[i])  res_->destroyBindingSet(sets_[i]);
            if (bones_[i]) res_->destroyBuffer(bones_[i]);
            sets_[i] = 0;
            bones_[i] = 0;
        }
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_)       res_->destroyShader(cs_);
    }
    pipeline_ = 0;
    cs_ = 0;
    frame_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
}

bool SkinningPass::createMesh(const fmt::OcMeshData& mesh, SkinnedMeshGpu& out) {
    out = {};
    if (!res_) return false;
    if (!mesh.hasSkin()) {
        AVER_WARN("[Skin] createMesh on a mesh with no skin streams");
        return false;
    }

    const u32 n = mesh.vertexCount();

    // Interleave into the shader's element layout. Doing it here rather than binding the .ocmesh's
    // three parallel arrays is what keeps the shader to one fetch per vertex per stream.
    std::vector<f32> rest(static_cast<usize>(n) * 6);
    for (u32 v = 0; v < n; ++v) {
        rest[v * 6 + 0] = mesh.positions[v * 3 + 0];
        rest[v * 6 + 1] = mesh.positions[v * 3 + 1];
        rest[v * 6 + 2] = mesh.positions[v * 3 + 2];
        rest[v * 6 + 3] = mesh.normals[v * 3 + 0];
        rest[v * 6 + 4] = mesh.normals[v * 3 + 1];
        rest[v * 6 + 5] = mesh.normals[v * 3 + 2];
    }

    // Joints widen u16 -> u32. Packing two per dword would halve this buffer and cost a shift in
    // the inner loop; it is not done, because a packing bug here reads the WRONG BONE and that is
    // a silent, plausible-looking deformation rather than a visible failure.
    static_assert(fmt::kOcMeshInfluences == 4,
                  "the shader's uint4/float4 influence pair is this constant, spelled in HLSL");
    std::vector<u32> bind(static_cast<usize>(n) * 8);
    for (u32 v = 0; v < n; ++v) {
        for (u32 i = 0; i < 4; ++i) {
            const usize s = static_cast<usize>(v) * 4 + i;
            bind[v * 8 + i] = static_cast<u32>(mesh.joints[s]);
            const f32 w = mesh.weights[s];
            std::memcpy(&bind[v * 8 + 4 + i], &w, sizeof(f32));
        }
    }

    rhi::BufferDesc rd;
    rd.bytes = rest.size() * sizeof(f32);
    rd.kind  = rhi::BufferKind::Upload;
    rd.debugName = "skin rest";
    out.rest = res_->createBuffer(rd);

    rhi::BufferDesc bd;
    bd.bytes = bind.size() * sizeof(u32);
    bd.kind  = rhi::BufferKind::Upload;
    bd.debugName = "skin bind";
    out.bind = res_->createBuffer(bd);

    rhi::BufferDesc od;
    od.bytes = static_cast<u64>(n) * kSkinVertexStride;
    od.kind  = rhi::BufferKind::Default;
    od.allowUnorderedAccess = true;
    od.debugName = "skin out";
    out.out = res_->createBuffer(od);

    if (!out.rest || !out.bind || !out.out) {
        AVER_ERROR("[Skin] createMesh could not allocate its buffers");
        destroyMesh(out);
        return false;
    }

    res_->writeBuffer(out.rest, rest.data(), rd.bytes);
    res_->writeBuffer(out.bind, bind.data(), bd.bytes);
    out.vertexCount = n;
    out.outState = rhi::ResourceState::Common;
    return true;
}

void SkinningPass::destroyMesh(SkinnedMeshGpu& m) {
    if (res_) {
        if (m.rest) res_->destroyBuffer(m.rest);
        if (m.bind) res_->destroyBuffer(m.bind);
        if (m.out)  res_->destroyBuffer(m.out);
    }
    m = {};
}

void SkinningPass::dispatch(rhi::IRenderContext& ctx, SkinnedMeshGpu& m,
                            const Mat4* skin, u32 boneCount) {
    if (!pipeline_ || !m.valid() || !skin || boneCount == 0) return;

    // Bones past the ring's capacity are dropped rather than growing it mid-frame. A vertex bound
    // to one then finds its index out of range and keeps its rest position, which is the same rule
    // the CPU applies -- so the two still agree about a rig this pass cannot fully hold.
    const u32 bones = boneCount > kSkinMaxBones ? kSkinMaxBones : boneCount;
    if (boneCount > kSkinMaxBones)
        AVER_WARN("[Skin] {} bones exceeds the {} this pass can bind; the tail is dropped",
                  boneCount, kSkinMaxBones);

    frame_ = (frame_ + 1) % kRing;
    const rhi::BufferHandle boneBuf = bones_[frame_];
    const rhi::BindingSetHandle set = sets_[frame_];
    if (!boneBuf || !set) return;

    upload_.resize(static_cast<usize>(bones) * 16);
    for (u32 b = 0; b < bones; ++b) std::memcpy(&upload_[b * 16], skin[b].m, 16 * sizeof(f32));
    if (!res_->writeBuffer(boneBuf, upload_.data(), upload_.size() * sizeof(f32))) return;

    res_->setSrvBuffer(set, 0, m.rest, kSkinVertexStride, m.vertexCount, 0);
    res_->setSrvBuffer(set, 1, m.bind, kBindStride, m.vertexCount, 0);
    res_->setSrvBuffer(set, 2, boneBuf, kBoneStride, bones, 0);
    res_->setUavBuffer(set, 0, m.out, kSkinVertexStride, m.vertexCount, 0);

    ctx.pushMarker("Aver.Skin");
    skinTransition(ctx, m, rhi::ResourceState::UnorderedAccess);

    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(set);
    const u32 params[4] = {m.vertexCount, bones, 0, 0};
    ctx.setConstants(kSkinConstantSlot, params, 4);
    ctx.dispatch((m.vertexCount + kSkinGroupSize - 1) / kSkinGroupSize, 1, 1);

    skinTransition(ctx, m, rhi::ResourceState::VertexBuffer);
    ctx.popMarker();
}

} // namespace aver::render
