// The skinning compute pass: rest vertices plus bone matrices (and blend-shape weights) in, posed vertices out.
#include "aver/render/SkinningPass.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
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

// The shader's t0..t5, all StructuredBuffer. Tier 1 wants a valid descriptor in every declared slot, so an
// instance without bones or blend shapes binds the pass's dummy there.
constexpr u32 kSkinSrvCount = 6;
constexpr u64 kDummyBytes = 64;

// SkinParams.gFlags
constexpr u32 kFlagMorph = 1u, kFlagNoSkin = 2u;

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
    pd.layout.srvCount = kSkinSrvCount;
    pd.layout.uavCount = 1;
    pd.layout.constantDwords[kSkinConstantSlot] = 4;
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[Skin] skinning pipeline unavailable"); shutdown(); return false; }

    rhi::BufferDesc dd;
    dd.bytes = kDummyBytes;
    dd.kind  = rhi::BufferKind::Upload;
    dd.debugName = "skin dummy";
    dummy_ = res_->createBuffer(dd);
    if (!dummy_) { AVER_ERROR("[Skin] skinning dummy buffer unavailable"); shutdown(); return false; }
    const u8 zeros[kDummyBytes] = {};
    res_->writeBuffer(dummy_, zeros, kDummyBytes);

    AVER_INFO("[Skin] GPU skinning ready ({} bones max, {} vertices per group, blend shapes up to {} active)",
              kSkinMaxBones, kSkinGroupSize, kMorphMaxActive);
    return true;
}

void SkinningPass::shutdown() {
    if (res_) {
        for (auto& kv : morphs_) {
            if (kv.second.rank)  res_->destroyBuffer(kv.second.rank);
            if (kv.second.delta) res_->destroyBuffer(kv.second.delta);
        }
        if (dummy_)    res_->destroyBuffer(dummy_);
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_)       res_->destroyShader(cs_);
    }
    morphs_.clear();
    dummy_ = 0;
    pipeline_ = 0;
    cs_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
}

bool SkinningPass::acquireMorph(u64 key, const std::vector<fmt::OcMorphTarget>& targets, u32 vertexCount) {
    if (const auto it = morphs_.find(key); it != morphs_.end()) { ++it->second.refs; return true; }
    MorphGpu g;
    std::string why;
    if (!buildMorphLayout(targets, vertexCount, g.layout, &why)) {
        AVER_WARN("[Skin] blend shapes of mesh 0x{:016X} are not used: {}", key, why);
        return false;
    }
    rhi::BufferDesc rb;
    rb.bytes = std::max<u64>(g.layout.rank.size() * sizeof(u32), 16);
    rb.kind  = rhi::BufferKind::Upload;
    rb.debugName = "morph rank";
    g.rank = res_->createBuffer(rb);
    rhi::BufferDesc db;
    db.bytes = std::max<u64>(g.layout.delta.size() * sizeof(f32), 16);
    db.kind  = rhi::BufferKind::Upload;
    db.debugName = "morph delta";
    g.delta = res_->createBuffer(db);
    if (!g.rank || !g.delta) {
        if (g.rank)  res_->destroyBuffer(g.rank);
        if (g.delta) res_->destroyBuffer(g.delta);
        AVER_WARN("[Skin] could not allocate the blend-shape buffers of mesh 0x{:016X}", key);
        return false;
    }
    res_->writeBuffer(g.rank, g.layout.rank.data(), g.layout.rank.size() * sizeof(u32));
    if (!g.layout.delta.empty()) res_->writeBuffer(g.delta, g.layout.delta.data(), g.layout.delta.size() * sizeof(f32));
    g.refs = 1;
    AVER_INFO("[Skin] mesh 0x{:016X}: {} blend shape(s), {} entries, {:.2f} MiB on the GPU", key, g.layout.targetCount,
              g.layout.totalEntries(), double(g.layout.bytes()) / (1024.0 * 1024.0));
    morphs_.emplace(key, std::move(g));
    return true;
}

void SkinningPass::releaseMorph(u64 key) {
    const auto it = morphs_.find(key);
    if (it == morphs_.end()) return;
    if (--it->second.refs > 0) return;
    if (res_) {
        if (it->second.rank)  res_->destroyBuffer(it->second.rank);
        if (it->second.delta) res_->destroyBuffer(it->second.delta);
    }
    morphs_.erase(it);
}

const MorphLayout* SkinningPass::morphLayout(u64 key) const {
    const auto it = morphs_.find(key);
    return it == morphs_.end() ? nullptr : &it->second.layout;
}

u64 SkinningPass::morphGpuBytes() const {
    u64 b = 0;
    for (const auto& kv : morphs_) b += kv.second.layout.bytes();
    return b;
}

bool SkinningPass::createMesh(const fmt::OcMeshData& mesh, u32 boneCount, SkinnedMeshGpu& out,
                              rhi::BufferHandle existingOut, u64 morphKey) {
    out = {};
    if (!res_) return false;
    bool wantMorph = morphKey != 0 && mesh.hasMorph();
    const bool skinned = mesh.hasSkin() && boneCount > 0;
    if (!skinned && !wantMorph) {
        if (!mesh.hasSkin()) AVER_WARN("[Skin] createMesh on a mesh with no skin streams");
        else AVER_ERROR("[Skin] createMesh with a zero-bone rig");
        return false;
    }

    const u32 n = mesh.vertexCount();
    const u32 caps = boneCount > kSkinMaxBones ? kSkinMaxBones : boneCount;
    if (skinned && boneCount > kSkinMaxBones)
        AVER_WARN("[Skin] a {}-bone rig exceeds the {} an instance can bind; the tail is dropped",
                  boneCount, kSkinMaxBones);

    // The blend-shape data, first: a morph-only mesh that cannot hold it cannot be created at all, and the
    // skinned ones carry on unmorphed.
    if (wantMorph) {
        if (acquireMorph(morphKey, mesh.morphTargets, n)) {
            out.morphKey = morphKey;
        } else {
            wantMorph = false;
            if (!skinned) return false;
        }
    }
    out.noSkin = !skinned;

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
    std::vector<u32> bind;
    if (skinned) {
        bind.resize(static_cast<usize>(n) * 8);
        for (u32 v = 0; v < n; ++v) {
            for (u32 i = 0; i < 4; ++i) {
                const usize s = static_cast<usize>(v) * 4 + i;
                bind[v * 8 + i] = static_cast<u32>(mesh.joints[s]);
                const f32 w = mesh.weights[s];
                std::memcpy(&bind[v * 8 + 4 + i], &w, sizeof(f32));
            }
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
    if (skinned) out.bind = res_->createBuffer(bd);

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
    // that made this per-instance rather than per-pass. A morph-only instance has no bones, only sets.
    rhi::BindingSetDesc sd;
    sd.srvCount = kSkinSrvCount;
    sd.uavCount = 1;
    for (u32 i = 0; i < kSkinSrvCount; ++i) sd.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
    sd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;

    bool ringOk = true;
    for (u32 i = 0; i < SkinnedMeshGpu::kRing; ++i) {
        if (skinned) {
            rhi::BufferDesc nb;
            nb.bytes = static_cast<u64>(caps) * kBoneStride;
            nb.kind  = rhi::BufferKind::Upload;
            nb.debugName = "skin bones";
            out.bones[i] = res_->createBuffer(nb);
            if (!out.bones[i]) ringOk = false;
        }
        if (out.morphKey) {
            rhi::BufferDesc ab;
            ab.bytes = static_cast<u64>(kMorphMaxActive) * kMorphActiveStride;
            ab.kind  = rhi::BufferKind::Upload;
            ab.debugName = "morph active";
            out.active[i] = res_->createBuffer(ab);
            if (!out.active[i]) ringOk = false;
        }
        out.sets[i] = res_->createBindingSet(sd);
        if (!out.sets[i]) ringOk = false;
    }

    if (!out.rest || (skinned && !out.bind) || !out.out || !ringOk) {
        AVER_ERROR("[Skin] createMesh could not allocate its buffers");
        destroyMesh(out);
        return false;
    }

    res_->writeBuffer(out.rest, rest.data(), rd.bytes);
    if (skinned) res_->writeBuffer(out.bind, bind.data(), bd.bytes);
    out.vertexCount = n;
    out.boneCapacity = skinned ? caps : 0;
    out.outState = rhi::ResourceState::Common;
    return true;
}

void SkinningPass::destroyMesh(SkinnedMeshGpu& m) {
    if (res_) {
        if (m.rest) res_->destroyBuffer(m.rest);
        if (m.bind) res_->destroyBuffer(m.bind);
        if (m.out && m.ownsOut) res_->destroyBuffer(m.out);
        for (u32 i = 0; i < SkinnedMeshGpu::kRing; ++i) {
            if (m.sets[i])   res_->destroyBindingSet(m.sets[i]);
            if (m.bones[i])  res_->destroyBuffer(m.bones[i]);
            if (m.active[i]) res_->destroyBuffer(m.active[i]);
        }
    }
    if (m.morphKey) releaseMorph(m.morphKey);
    m = {};
}

void SkinningPass::dispatch(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, const Mat4* skin, u32 boneCount,
                            const MorphTargetWeight* w, u32 n) {
    if (m.noSkin) return;   // a morph-only instance has no bones to take
    record(ctx, m, skin, boneCount, w, n);
}

void SkinningPass::dispatchMorph(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, const MorphTargetWeight* w, u32 n) {
    if (!m.noSkin) return;
    record(ctx, m, nullptr, 0, w, n);
}

void SkinningPass::record(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, const Mat4* skin, u32 boneCount,
                          const MorphTargetWeight* w, u32 n) {
    if (!pipeline_ || !m.valid()) return;
    const bool skinned = !m.noSkin;
    if (skinned && (!skin || boneCount == 0)) return;

    // Bones past what this instance's ring was sized for are dropped rather than growing it
    // mid-frame. A vertex bound to one then finds its index out of range and keeps its rest
    // position -- the same rule the CPU applies, so the two still agree about a rig too large to
    // hold. Silently truncating without saying so is what would make that a mystery instead.
    u32 bones = 0;
    if (skinned) {
        bones = boneCount > m.boneCapacity ? m.boneCapacity : boneCount;
        if (boneCount > m.boneCapacity)
            AVER_WARN("[Skin] {} bones exceeds the {} this instance was built for; the tail is dropped",
                      boneCount, m.boneCapacity);
    }

    // The instance's own ring: one dispatch per instance per frame, so advancing per call gives
    // exactly one frame of separation between a CPU write and the GPU read it could race.
    m.frame = (m.frame + 1) % SkinnedMeshGpu::kRing;
    const rhi::BindingSetHandle set = m.sets[m.frame];
    if (!set) return;
    rhi::BufferHandle boneBuf = 0;
    if (skinned) {
        boneBuf = m.bones[m.frame];
        if (!boneBuf) return;
        upload_.resize(static_cast<usize>(bones) * 16);
        for (u32 b = 0; b < bones; ++b) std::memcpy(&upload_[b * 16], skin[b].m, 16 * sizeof(f32));
        if (!res_->writeBuffer(boneBuf, upload_.data(), upload_.size() * sizeof(f32))) return;
    }

    // Blend shapes: this instance's active list, written into its own ring slot.
    const MorphGpu* mg = nullptr;
    u32 activeN = 0;
    if (m.morphKey) {
        const auto it = morphs_.find(m.morphKey);
        if (it != morphs_.end()) mg = &it->second;
        if (mg && w && n > 0 && m.active[m.frame]) {
            active_.resize(kMorphMaxActive);
            activeN = buildActiveList(mg->layout, w, n, active_.data());
            if (activeN > 0 &&
                !res_->writeBuffer(m.active[m.frame], active_.data(), static_cast<u64>(activeN) * kMorphActiveStride))
                activeN = 0;
        }
    }
    m.activeCount = activeN;

    res_->setSrvBuffer(set, 0, m.rest, kSkinVertexStride, m.vertexCount, 0);
    if (skinned) {
        res_->setSrvBuffer(set, 1, m.bind, kBindStride, m.vertexCount, 0);
        res_->setSrvBuffer(set, 2, boneBuf, kBoneStride, bones, 0);
    } else {
        res_->setSrvBuffer(set, 1, dummy_, kBindStride, 1, 0);
        res_->setSrvBuffer(set, 2, dummy_, kBoneStride, 1, 0);
    }
    if (mg) {
        res_->setSrvBuffer(set, 3, mg->rank, kMorphRankStride, std::max<u32>(1, static_cast<u32>(mg->layout.rank.size() / 2)), 0);
        res_->setSrvBuffer(set, 4, mg->delta, kMorphDeltaStride, std::max<u32>(1, mg->layout.totalEntries()), 0);
    } else {
        res_->setSrvBuffer(set, 3, dummy_, kMorphRankStride, 1, 0);
        res_->setSrvBuffer(set, 4, dummy_, kMorphDeltaStride, 1, 0);
    }
    if (activeN > 0) res_->setSrvBuffer(set, 5, m.active[m.frame], kMorphActiveStride, activeN, 0);
    else             res_->setSrvBuffer(set, 5, dummy_, kMorphActiveStride, 1, 0);
    res_->setUavBuffer(set, 0, m.out, kSkinVertexStride, m.vertexCount, 0);

    ctx.pushMarker("Aver.Skin");
    skinTransition(ctx, m, rhi::ResourceState::UnorderedAccess);

    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(set);
    const u32 flags = (activeN > 0 ? kFlagMorph : 0u) | (skinned ? 0u : kFlagNoSkin);
    const u32 params[4] = {m.vertexCount, bones, flags, activeN};
    ctx.setConstants(kSkinConstantSlot, params, 4);
    ctx.dispatch((m.vertexCount + kSkinGroupSize - 1) / kSkinGroupSize, 1, 1);

    skinTransition(ctx, m, rhi::ResourceState::GeometryRead);
    ctx.popMarker();
}

} // namespace aver::render
