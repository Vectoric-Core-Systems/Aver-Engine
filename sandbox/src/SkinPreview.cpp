// The editors' GPU skinning preview. See SkinPreview.hpp.
#include "SkinPreview.hpp"

#include "ActorEditor.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/runtime/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace aver::editor {

bool SkinPreview::init(rhi::IDevice& dev) {
    dev_ = &dev;
    ready_ = pass_.init(dev);
    if (!ready_) AVER_WARN("[AnimEditor] GPU skinning unavailable; the preview stays on bone boxes");
    return ready_;
}

void SkinPreview::shutdown() {
    releaseMeshes();
    pass_.shutdown();
    ready_ = false;
    dev_ = nullptr;
    meshPath_.clear();
    boneCount_ = 0;
}

void SkinPreview::releaseMeshes() {
    if (gpu_.valid()) pass_.destroyMesh(gpu_);
    gpu_ = {};
    if (dev_) {
        if (drawMesh_) dev_->destroyMesh(drawMesh_);
        if (source_)   dev_->destroyMesh(source_);
    }
    drawMesh_ = 0;
    source_ = 0;
    vertexCount_ = 0;
    triangleCount_ = 0;
    meshPath_.clear();
    slot0Material_.clear();
    targetNames_.clear();
    targetDefaults_.clear();
    morphEntries_ = 0;
    morphStaged_.clear();
}

bool SkinPreview::bind(rhi::IDevice& dev, const std::string& meshPath, u32 boneCount) {
    if (!ready_) return false;
    if (meshPath == meshPath_ && gpu_.valid() && boneCount == boneCount_) return true;

    // The preview may still list the old draw mesh; drop it before it dies.
    if (drawMesh_)
        if (render::preview::ActorPreview* p = sharedPreviewIfCreated()) p->dropDrawsUsing(drawMesh_);
    releaseMeshes();
    meshPath_ = meshPath;
    boneCount_ = boneCount;
    // Cleared here, not only on success: a partial rebind failure (no skin streams, a truncated file) must not
    // leave a stale name from the previous mesh for buildPreview's cache to keep resolving.
    slot0Material_.clear();
    if (meshPath.empty()) return false;

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(meshPath, md, &why)) {
        AVER_WARN("[AnimEditor] {}", why);
        return false;
    }
    // Read regardless of the checks below: harmless if unused (falls back to bone boxes), and simpler than
    // re-opening the file later just for this field.
    if (!md.materialSlots.empty()) slot0Material_ = md.materialSlots[0];

    const bool skinned = md.hasSkin() && boneCount > 0;
    const bool morphed = md.hasMorph();
    if (!skinned && !morphed) {
        // Not an error and not silent: a mesh with no skin streams is ordinary; falls back to bone boxes rather
        // than a static T-pose that would look like a broken clip.
        if (boneCount > 0) AVER_INFO("[AnimEditor] {} has no skin streams; the preview stays on bone boxes", meshPath);
        return false;
    }

    // OcMeshData keeps positions/normals/uvs as flat arrays; the RHI wants one interleaved MeshVertex stream. Built
    // here (not via PreviewMeshCache, which resolves content-relative paths -- this mesh is found by walking the
    // skeleton's own directory) since the skinning pass needs the decoded OcMeshData anyway, for its streams.
    const usize vcount = md.positions.size() / 3;
    if (vcount == 0 || md.normals.size() < vcount * 3 || md.uvs.size() < vcount * 2) return false;
    std::vector<rhi::MeshVertex> verts0(vcount);
    for (usize i = 0; i < vcount; ++i) {
        verts0[i].px = md.positions[i * 3 + 0];
        verts0[i].py = md.positions[i * 3 + 1];
        verts0[i].pz = md.positions[i * 3 + 2];
        verts0[i].nx = md.normals[i * 3 + 0];
        verts0[i].ny = md.normals[i * 3 + 1];
        verts0[i].nz = md.normals[i * 3 + 2];
        verts0[i].u  = md.uvs[i * 2 + 0];
        verts0[i].v  = md.uvs[i * 2 + 1];
    }
    source_ = dev.createMesh(verts0.data(), static_cast<u32>(vcount),
                             md.indices.data(), static_cast<u32>(md.indices.size()));
    if (!source_) return false;
    rhi::BufferHandle verts = 0;
    // Draw handle whose vertex buffer IS the skin target: the preview rasterises posed vertices directly, no second
    // copy.
    drawMesh_ = dev.createSkinTargetMesh(source_, &verts);
    if (!drawMesh_ || !verts) { releaseMeshes(); return false; }
    const u64 morphKey = morphed ? (fnv1a64(std::string_view(meshPath)) | 1ull) : 0ull;
    if (!pass_.createMesh(md, skinned ? boneCount : 0, gpu_, verts, morphKey)) { releaseMeshes(); return false; }

    if (morphed && gpu_.morphKey) {
        for (const fmt::OcMorphTarget& t : md.morphTargets) {
            targetNames_.push_back(t.name);
            targetDefaults_.push_back(t.defaultWeight);
            morphEntries_ += static_cast<u32>(t.vertices.size());
        }
    }

    // Bounds radius for frameAll, from the rest mesh -- posed mesh doesn't move enough to matter, and per-frame GPU
    // readback isn't available anyway.
    vertexCount_ = static_cast<u32>(vcount);
    triangleCount_ = static_cast<u32>(md.indices.size() / 3);
    radius_ = 1.0f;
    for (usize i = 0; i < vcount; ++i) {
        const f32 x = md.positions[i * 3 + 0], y = md.positions[i * 3 + 1], z = md.positions[i * 3 + 2];
        radius_ = std::max(radius_, std::sqrt(x * x + y * y + z * z));
    }
    return true;
}

void SkinPreview::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || !gpu_.valid()) return;
    if (gpu_.noSkin) {
        pass_.dispatchMorph(ctx, gpu_, morphStaged_.data(), static_cast<u32>(morphStaged_.size()));
        return;
    }
    if (staged_.empty()) return;
    pass_.dispatch(ctx, gpu_, staged_.data(), static_cast<u32>(staged_.size()),
                   morphStaged_.data(), static_cast<u32>(morphStaged_.size()));
}

void SkinPreview::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    (void)width; (void)height;
    if (!ready_ || !gpu_.valid()) return;
    // Unconditional, not "only if dispatched" (see SkinnedScene::overlayPass): skinTransition is a no-op if already
    // in state; a buffer left in GeometryRead at submit is a validation error next frame.
    render::skinTransition(ctx, gpu_, rhi::ResourceState::Common);
}

// ---- the shared instance ----

namespace {
SkinPreview g_skin;
bool g_skinTried = false;
// Keeps the device the feature was registered with, so teardown unregisters from the same one.
rhi::IDevice* g_skinDevice = nullptr;
} // namespace

SkinPreview& skinPreviewInstance() { return g_skin; }

SkinPreview* sharedSkinPreview(Engine& e) {
    if (!g_skinTried && e.device()) {
        g_skinTried = true;
        if (g_skin.init(*e.device())) {
            g_skinDevice = e.device();
            g_skinDevice->addRenderFeature(&g_skin);
        }
    }
    return g_skin.ready() ? &g_skin : nullptr;
}

void shutdownSharedSkinPreview() {
    if (g_skinDevice) g_skinDevice->removeRenderFeature(&g_skin);
    g_skinDevice = nullptr;
    g_skin.shutdown();
    g_skinTried = false;
}

} // namespace aver::editor
