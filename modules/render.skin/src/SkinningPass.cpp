// The skinning compute pass: rest vertices plus bone matrices in, posed vertices out.
#include "aver/render/SkinningPass.hpp"
#include "aver/core/Log.hpp"

#include <cstring>
#include "aver/rhi/ShaderFiles.hpp"   // this pass's HLSL is a deployed file, not a literal

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
    sd.source = rhi::shaderFile("skin.hlsl").c_str();
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

    AVER_INFO("[Skin] GPU skinning ready ({} bones max, {} vertices per group)",
              kSkinMaxBones, kSkinGroupSize);
    return true;
}

void SkinningPass::shutdown() {
    if (res_) {
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_)       res_->destroyShader(cs_);
    }
    pipeline_ = 0;
    cs_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
}

bool SkinningPass::createMesh(const fmt::OcMeshData& mesh, u32 boneCount, SkinnedMeshGpu& out,
                              rhi::BufferHandle existingOut) {
    out = {};
    if (!res_) return false;
    if (!mesh.hasSkin()) {
        AVER_WARN("[Skin] createMesh on a mesh with no skin streams");
        return false;
    }
    if (boneCount == 0) { AVER_ERROR("[Skin] createMesh with a zero-bone rig"); return false; }

    const u32 n = mesh.vertexCount();
    const u32 caps = boneCount > kSkinMaxBones ? kSkinMaxBones : boneCount;
    if (boneCount > kSkinMaxBones)
        AVER_WARN("[Skin] a {}-bone rig exceeds the {} an instance can bind; the tail is dropped",
                  boneCount, kSkinMaxBones);

    // Interleave into the shader's element layout -- which is rhi::MeshVertex. Doing it here rather
    // than binding the .ocmesh's parallel arrays is what keeps the shader to one fetch per stream,
    // and what lets the OUTPUT be a vertex buffer nothing has to be taught about.
    //
    // A mesh with no uv stream is zero-filled rather than refused: uv is carried through and never
    // read by this shader, so its absence costs a texture lookup and not a wrong position.
    const bool haveUv = mesh.uvs.size() == static_cast<usize>(n) * 2;
    if (!haveUv && !mesh.uvs.empty())
        AVER_WARN("[Skin] the uv stream is {} floats for {} vertices; it is dropped rather than misread",
                  mesh.uvs.size(), n);

    const u32 kFloatsPerVertex = kSkinVertexStride / sizeof(f32);
    std::vector<f32> rest(static_cast<usize>(n) * kFloatsPerVertex, 0.0f);
    for (u32 v = 0; v < n; ++v) {
        f32* d = &rest[static_cast<usize>(v) * kFloatsPerVertex];
        d[0] = mesh.positions[v * 3 + 0];
        d[1] = mesh.positions[v * 3 + 1];
        d[2] = mesh.positions[v * 3 + 2];
        d[3] = mesh.normals[v * 3 + 0];
        d[4] = mesh.normals[v * 3 + 1];
        d[5] = mesh.normals[v * 3 + 2];
        if (haveUv) { d[6] = mesh.uvs[v * 2 + 0]; d[7] = mesh.uvs[v * 2 + 1]; }
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

    // A supplied output is a mesh's own vertex buffer, which this instance writes but does not own.
    if (existingOut) { out.out = existingOut; out.ownsOut = false; }
    else {
        rhi::BufferDesc od;
        od.bytes = static_cast<u64>(n) * kSkinVertexStride;
        od.kind  = rhi::BufferKind::Default;
        od.allowUnorderedAccess = true;
        od.debugName = "skin out";
        out.out = res_->createBuffer(od);
        out.ownsOut = true;
    }

    // The bone ring and its binding sets, per instance. See SkinnedMeshGpu's comment for the bug
    // that made this per-instance rather than per-pass.
    rhi::BindingSetDesc sd;
    sd.srvCount = 3;
    sd.uavCount = 1;
    sd.srvKinds[0] = sd.srvKinds[1] = sd.srvKinds[2] = rhi::SlotKind::StructuredBuffer;
    sd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;

    bool ringOk = true;
    for (u32 i = 0; i < SkinnedMeshGpu::kRing; ++i) {
        rhi::BufferDesc nb;
        nb.bytes = static_cast<u64>(caps) * kBoneStride;
        nb.kind  = rhi::BufferKind::Upload;
        nb.debugName = "skin bones";
        out.bones[i] = res_->createBuffer(nb);
        out.sets[i]  = res_->createBindingSet(sd);
        if (!out.bones[i] || !out.sets[i]) ringOk = false;
    }

    if (!out.rest || !out.bind || !out.out || !ringOk) {
        AVER_ERROR("[Skin] createMesh could not allocate its buffers");
        destroyMesh(out);
        return false;
    }

    res_->writeBuffer(out.rest, rest.data(), rd.bytes);
    res_->writeBuffer(out.bind, bind.data(), bd.bytes);
    out.vertexCount = n;
    out.boneCapacity = caps;
    out.outState = rhi::ResourceState::Common;
    return true;
}

void SkinningPass::destroyMesh(SkinnedMeshGpu& m) {
    if (res_) {
        if (m.rest) res_->destroyBuffer(m.rest);
        if (m.bind) res_->destroyBuffer(m.bind);
        if (m.out && m.ownsOut) res_->destroyBuffer(m.out);
        for (u32 i = 0; i < SkinnedMeshGpu::kRing; ++i) {
            if (m.sets[i])  res_->destroyBindingSet(m.sets[i]);
            if (m.bones[i]) res_->destroyBuffer(m.bones[i]);
        }
    }
    m = {};
}

void SkinningPass::dispatch(rhi::IRenderContext& ctx, SkinnedMeshGpu& m,
                            const Mat4* skin, u32 boneCount) {
    if (!pipeline_ || !m.valid() || !skin || boneCount == 0) return;

    // Bones past what this instance's ring was sized for are dropped rather than growing it
    // mid-frame. A vertex bound to one then finds its index out of range and keeps its rest
    // position -- the same rule the CPU applies, so the two still agree about a rig too large to
    // hold. Silently truncating without saying so is what would make that a mystery instead.
    const u32 bones = boneCount > m.boneCapacity ? m.boneCapacity : boneCount;
    if (boneCount > m.boneCapacity)
        AVER_WARN("[Skin] {} bones exceeds the {} this instance was built for; the tail is dropped",
                  boneCount, m.boneCapacity);

    // The instance's own ring: one dispatch per instance per frame, so advancing per call gives
    // exactly one frame of separation between a CPU write and the GPU read it could race.
    m.frame = (m.frame + 1) % SkinnedMeshGpu::kRing;
    const rhi::BufferHandle boneBuf = m.bones[m.frame];
    const rhi::BindingSetHandle set = m.sets[m.frame];
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

    skinTransition(ctx, m, rhi::ResourceState::GeometryRead);
    ctx.popMarker();
}

} // namespace aver::render
