#pragma once
// --skin-draw-test: proves the RASTERISER reads the skinned vertex buffer, not the rest mesh, and
// under --rt that the RAY TRACER does too.
//
// The self-test in Aver.Render.Skin proves the compute pass computes the right numbers, by reading
// them back and comparing against anim::skinVertices. It says nothing about whether anything ever
// DRAWS them -- a mesh still bound to its rest vertices passes that check perfectly while rendering
// a character frozen in bind pose.
//
// The instrument is a RELATION, not a value, and that is what makes it independent of the camera,
// the framing, the lighting and the render settings: the same pixels are probed while exactly ONE
// thing changes. No baseline, no gate line, nothing to re-record.
//
// TWO ASSERTIONS, because there are two ways to be wrong, and they need DIFFERENT experiments:
//
//   RASTER -- a pixel on the box must change when the pose changes.
//
//   ACCELERATION STRUCTURE -- a bottom-level structure memoised per mesh gives a moving character a
//   stationary shadow while the raster image looks perfect, so it needs its own test. TWO obvious
//   designs were tried and MEASURED FAILING, and both failures are why the schedule looks like it
//   does:
//     - "did a ground pixel change between the two poses" does not work. The sun cascade is
//       rasterised from the same posed vertices, so the ground changes when the pose does whether
//       or not the structure was rebuilt. With the rebuild disabled on purpose it still moved 0.17.
//     - toggling ray tracing on only AFTER the box had moved does not work either, and passed a
//       build with the rebuild disabled. The structure is not created until something asks to
//       trace, so enabling it late builds it fresh from wherever the box already is. The bug it is
//       meant to catch cannot occur.
//   So ray tracing is ON WHILE THE BOX IS NEAR -- that is the frame that builds the structure --
//   the box then moves, and the comparison is between two frames with the SAME away pose that
//   differ only in whether rays are traced. The cascade says "no shadow" in both. If tracing
//   darkens the ground, the only thing that can be casting is a structure still describing a box
//   that left.
//
// A skinned box is used rather than an imported asset because THE TREE CONTAINS NO SKINNED MESH --
// no .gltf, no .glb, no .ocmesh with skin streams anywhere. Building it in process also keeps the
// check honest about what it covers: the draw path, not the importer.
#include "aver/render/SkinningPass.hpp"

namespace aver::editor {

// Drives a skinned box through three experiments and reports whether the screen followed.
class SkinDrawTest final : public rhi::IRenderFeature {
public:
    ~SkinDrawTest() override;

    // Builds the box, its skin target, its rig and the ground it casts onto. False means the check
    // cannot run at all, which is reported as unavailable rather than as a failure.
    bool init(rhi::IDevice& dev);
    void shutdown();

    // Whether the acceleration-structure half can run at all -- it needs a device with ray tracing
    // and a renderer willing to use it. Without it the raster half still runs and the report says
    // which question went unanswered rather than implying both passed.
    void setRayTracingAvailable(bool on) { rtAvailable_ = on; }

    // What the renderer's ray-tracing setting should be RIGHT NOW. The app applies it; this class
    // owns the schedule because the schedule is the experiment.
    bool wantRayTracing() const { return phase_ != kPhaseAwayRtOff; }

    const char* name() const override { return "Aver.Skin.DrawTest"; }

    // Skins the box into its own vertex buffer, before anything reads it.
    void prePass(rhi::IRenderContext& ctx) override;
    // Returns the buffer to Common. A buffer's state does not survive the command list, so a frame
    // that ended in GeometryRead would have the NEXT frame's barrier claim a state the hardware
    // dropped -- silent without the debug layer, and this is the last hook before submit.
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

    // The handles the app should draw. Both are ordinary meshes in every respect; that one of them
    // happens to be written by compute is not something the draw path knows or needs to.
    rhi::MeshHandle mesh() const { return skinned_; }
    rhi::MeshHandle ground() const { return ground_; }

    // Advances the schedule and samples the next probe. Called once per frame by the app, which is
    // what owns the viewport rect the probes are expressed against and what applies
    // wantRayTracing() to the renderer.
    void tick(rhi::IDevice& dev, f32 vpX, f32 vpY, f32 vpW, f32 vpH);

    bool finished() const { return done_; }

private:
    void report();

    // Three experiments. The first pair vary the POSE (the raster question); the second pair vary
    // RAY TRACING with the pose held still (the structure question); the middle one belongs to both,
    // which is why there are three and not four.
    // ORDER IS THE EXPERIMENT. Near-with-tracing comes first because that is the frame in which a
    // bottom-level structure gets built at all; everything after it is asking whether that structure
    // kept up.
    enum Phase : u32 { kPhaseNearRtOn = 0, kPhaseAwayRtOn = 1, kPhaseAwayRtOff = 2, kPhaseCount = 3 };

    // One probe: where it is, and what it read in each phase.
    struct Probe {
        f32  u = 0.5f, v = 0.5f;   // fraction of the viewport rect
        f32  c[kPhaseCount][4] = {};
        bool have[kPhaseCount] = {};
    };
    // One on the box, the rest spread across the ground. More ground points than strictly needed:
    // where the shadow falls is the sun's business, and the assertions are over the SET rather than
    // over any one point.
    static constexpr u32 kProbes = 9;

    render::SkinningPass pass_;
    render::SkinnedMeshGpu gpu_{};
    rhi::MeshHandle rest_ = 0;      // the source mesh, whose vertices seed the target
    rhi::MeshHandle skinned_ = 0;   // what the app draws
    rhi::MeshHandle ground_ = 0;    // static, and the surface the shadow lands on
    rhi::BufferHandle vertices_ = 0;

    Probe probes_[kProbes];
    Mat4  poseNear_[1]{}, poseAway_[1]{};
    u32   step_ = 0;                // frames since this phase started
    u32   phase_ = kPhaseNearRtOn;
    bool  rtAvailable_ = false;
    bool  done_ = false;
};

} // namespace aver::editor
