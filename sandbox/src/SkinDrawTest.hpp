#pragma once
// --skin-draw-test: proves the RASTERISER reads the skinned vertex buffer, not the rest mesh.
//
// The self-test in Aver.Render.Skin proves the compute pass computes the right numbers, by reading
// them back and comparing against anim::skinVertices. It says nothing about whether anything ever
// DRAWS them -- a mesh still bound to its rest vertices passes that check perfectly while rendering
// a character frozen in bind pose.
//
// The instrument here is a RELATION, not a value, and that is what makes it independent of the
// camera, the framing, the lighting and the render settings: the same pixel is probed under two
// DIFFERENT poses. If the draw path reads the posed buffer the pixel must change. If it reads the
// rest buffer the pixel cannot change, because the rest buffer is the same in both frames. No
// baseline, no gate line, and nothing to re-record.
//
// A skinned box is used rather than an imported asset because THE TREE CONTAINS NO SKINNED MESH --
// no .gltf, no .glb, no .ocmesh with skin streams anywhere. Building it in process also keeps the
// check honest about what it covers: the draw path, not the importer.
#include "aver/render/SkinningPass.hpp"

namespace aver::editor {

// Drives a skinned box through two poses and reports whether the screen followed.
class SkinDrawTest final : public rhi::IRenderFeature {
public:
    ~SkinDrawTest() override;

    // Builds the box, its skin target and its rig. False means the check cannot run at all, which
    // is reported as unavailable rather than as a failure.
    bool init(rhi::IDevice& dev);
    void shutdown();

    const char* name() const override { return "Aver.Skin.DrawTest"; }

    // Skins the box into its own vertex buffer, before anything reads it.
    void prePass(rhi::IRenderContext& ctx) override;
    // Returns the buffer to Common. A buffer's state does not survive the command list, so a frame
    // that ended in VertexBuffer would have the NEXT frame's barrier claim a state the hardware
    // dropped -- silent without the debug layer, and this is the last hook before submit.
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

    // The handle the app should draw. It is an ordinary mesh in every respect; that it happens to
    // be written by compute is not something the draw path knows or needs to.
    rhi::MeshHandle mesh() const { return skinned_; }

    // Advances the schedule and samples the probe pixel. Called once per frame by the app, which
    // is what owns the viewport rect the probe is expressed against.
    void tick(rhi::IDevice& dev, u32 px, u32 py);

    bool finished() const { return stage_ >= kDone; }

private:
    void report();

    enum : u32 { kPoseAAt = 0, kGrabA = 4, kReadA = 5, kPoseBAt = 6, kGrabB = 10, kReadB = 11, kDone = 12 };

    render::SkinningPass pass_;
    render::SkinnedMeshGpu gpu_{};
    rhi::IDevice*   dev_ = nullptr;
    rhi::MeshHandle rest_ = 0;      // the source mesh, whose vertices seed the target
    rhi::MeshHandle skinned_ = 0;   // what the app draws
    rhi::BufferHandle vertices_ = 0;

    Mat4 poseA_[1]{}, poseB_[1]{};
    u32  stage_ = 0;
    bool poseIsB_ = false;
    f32  a_[4] = {0, 0, 0, 0};
    f32  b_[4] = {0, 0, 0, 0};
    bool haveA_ = false, haveB_ = false;
};

} // namespace aver::editor
