#pragma once
// The editors' GPU skinning preview: turns a UI-sampled pose (and blend-shape weights) into posed vertices once per
// frame, before the shared ActorPreview draws them. Extracted from the Anim editor (it was AnimSkinFeature there)
// so the Face Rig tab drives the same feature; one shared instance, registered before the shared preview.
//
// A render FEATURE rather than a draw()-side call, because the dispatch needs an IRenderContext the UI has none of --
// the shape of SkinnedScene's prePass / overlayPass pair: dispatch leaves the buffer in GeometryRead for the draw, and
// every frame must hand it back to Common before submit (D3D12 decays buffer state at end-of-command-list).
//
// Blend shapes (docs/plans/wave9/MORPH_TARGETS.md 2.10): a mesh with no skin streams but with morph targets binds as a
// morph-only instance (boneCount 0), and a skinned mesh with targets morphs before it skins.
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/render/SkinningPass.hpp"
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

namespace aver {
class Engine;
}

namespace aver::editor {

class SkinPreview final : public rhi::IRenderFeature {
public:
    const char* name() const override { return "Aver.AnimEditor.Skin"; }

    bool init(rhi::IDevice& dev);
    void shutdown();

    // Frees the skinning buffers and both meshes. The draw mesh goes first: it shares the source's index buffer, so
    // the device refuses the source while it lives. A caller still drawing drawMesh() must drop that draw first.
    void releaseMeshes();

    bool ready() const { return ready_; }
    rhi::MeshHandle drawMesh() const { return drawMesh_; }
    f32 boundsRadius() const { return radius_; }
    // For the preview's stats overlay.
    u32 vertexCount() const { return vertexCount_; }
    u32 triangleCount() const { return triangleCount_; }
    // Mesh's slot-0 surface name (OcMeshData::materialSlots[0]), or empty.
    const std::string& slot0Material() const { return slot0Material_; }

    // Binds a rig + mesh pair (boneCount 0 = no rig: only a mesh with blend shapes binds). Idempotent for the same
    // pair, so the editor can call it every frame without rebuilding GPU resources.
    bool bind(rhi::IDevice& dev, const std::string& meshPath, u32 boneCount);

    // This frame's skinning matrices, copied because prePass runs later than the UI that produced them.
    void stage(const std::vector<Mat4>& skin) { staged_ = skin; }
    // This frame's blend-shape weights (indices into targetNames()). Empty = the rest shape.
    void stageMorph(std::vector<render::MorphTargetWeight> w) { morphStaged_ = std::move(w); }

    // What is bound right now (another tab may have bound something else since: a tab that finds these differ from what it
    // asked for binds again).
    const std::string& boundPath() const { return meshPath_; }
    u32 boundBones() const { return boneCount_; }

    // The bound mesh's blend shapes.
    bool hasMorph() const { return !targetNames_.empty(); }
    bool morphOnly() const { return gpu_.noSkin; }
    const std::vector<std::string>& targetNames() const { return targetNames_; }
    const std::vector<f32>& targetDefaults() const { return targetDefaults_; }
    u32 morphEntries() const { return morphEntries_; }
    u64 morphGpuBytes() const { return pass_.morphGpuBytes(); }
    // How many targets the last dispatch applied (at most 64).
    u32 morphActive() const { return gpu_.activeCount; }

    void prePass(rhi::IRenderContext& ctx) override;
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

private:
    rhi::IDevice*           dev_ = nullptr;
    render::SkinningPass    pass_;
    render::SkinnedMeshGpu  gpu_;
    rhi::MeshHandle         drawMesh_ = 0;
    rhi::MeshHandle         source_ = 0;   // drawMesh_'s rest mesh; shares indices, so freed after it
    std::string             meshPath_;
    u32                     boneCount_ = 0;
    u32                     vertexCount_ = 0, triangleCount_ = 0;
    f32                     radius_ = 1.0f;
    bool                    ready_ = false;
    std::vector<Mat4>       staged_;
    std::vector<render::MorphTargetWeight> morphStaged_;
    std::vector<std::string> targetNames_;
    std::vector<f32>        targetDefaults_;
    u32                     morphEntries_ = 0;
    std::string             slot0Material_;   // OcMeshData::materialSlots[0] of the bound mesh, or ""
};

// The one shared instance, created and registered with the device once, before the shared preview (feature order is
// prePass order, and the preview renders its image in its own prePass). Null when the device has no compute.
SkinPreview* sharedSkinPreview(Engine& e);
// The instance itself (valid before and after sharedSkinPreview), for teardown.
SkinPreview& skinPreviewInstance();
// Unregisters the feature from the device it was registered with and frees everything. Repeatable.
void shutdownSharedSkinPreview();

} // namespace aver::editor
