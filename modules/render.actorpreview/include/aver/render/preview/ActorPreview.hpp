#pragma once
// The actor preview: an actor's AUTHORED COMPOSITION, drawn into a texture of its own.
//
// What UE's Blueprint viewport shows, and for the same reason: an actor is a tree of placements in
// LOCAL space, and until you can see that tree you are editing coordinates blind. It draws the rows
// a `.Designer.cs` declares (see aver::fmt::parseActorScript), not entities -- no spawn, no world,
// no bridge, no play state. What is on screen is what the source says, which is what makes editing
// the source and dragging in the view the same operation.
//
// A FEATURE THAT OWNS ITS TARGETS, rather than a second viewport in the scene pass. The reasoning is
// in docs/ACTOR_EDITOR.md §3 and it is not a preference: `IDevice::setCamera` writes a CPU-side
// struct that is uploaded once at the top of beginFrame, so a second camera set after that is a
// no-op FOR THE CURRENT FRAME -- two rects in the scene pass would both draw with the previous
// frame's camera and present as a matrix bug. And exposure is one histogram over the whole target
// reducing to one scalar, so two viewports cannot have different exposure; opening this tab would
// make the LEVEL viewport ramp brightness for a second.
//
// THE CAMERA IS PUBLISHED AT b4, not b0. Slot 0 is the engine's per-frame block, the backend rebinds
// it on every setPipeline, and RHIResources.hpp says outright that a feature must never observe it
// unbound. Voxi's shadow pass publishes at kFeatureFrameConstantRegister and so does this. The
// consequence is not just a register number: averSkyAbove reads the sky out of b0, so this writes
// its own backdrop rather than sampling the level's atmosphere.
//
// WHAT IT DELIBERATELY IS NOT: fixed exposure, no bloom, no eye adaptation, no GI, no cascaded
// shadows, no MSAA. It will not match the level viewport and is not trying to. That has to be said
// in the panel as well as here, or it gets filed as a bug every month.
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

namespace aver::render::preview {

// One placement to draw. Flat, and derived from a parsed source row rather than from an entity --
// that is the whole design, and it is what lets the preview update from a text edit with no rebuild,
// no reload and no play session.
struct PreviewDraw {
    rhi::MeshHandle mesh = 0;
    f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // row-major, row-vector, cm
    f32 baseColor[4] = {0.8f, 0.8f, 0.82f, 1.0f};
    // How far this mesh reaches from its own origin, in ITS units, before the world matrix. Framing
    // needs it: a class-level mesh sits at the origin with no placement, so origins alone put every
    // such actor at the same distance and a unit sphere arrives as one pixel.
    f32 boundsRadius = 0.0f;
    f32 metallic = 0.0f;
    f32 roughness = 0.6f;
    // Highlighted in the view, because a viewport you cannot tell the selection in is a viewport you
    // cannot drag in.
    bool selected = false;
};

// Where the eye is. An ORBIT rather than a free camera: the subject is one object at the origin, the
// only useful motions are around it and towards it, and a fly camera in a preview is a camera the
// user gets lost in and then has to be given a "frame selection" button to escape.
struct PreviewCamera {
    f32 yawDeg = 35.0f;
    f32 pitchDeg = 20.0f;
    f32 distance = 400.0f;          // centimetres from the pivot
    f32 pivot[3] = {0.0f, 0.0f, 0.0f};
    f32 fovDeg = 45.0f;
    // Clamped so the orbit cannot pass through the pole, where the up vector flips and the view
    // rolls over with no hysteresis.
    void addOrbit(f32 dYaw, f32 dPitch);
    void addZoom(f32 factor);
};

class ActorPreview final : public rhi::IRenderFeature {
public:
    // Returns null when the backend has no GPU support, which is how this declines instead of
    // failing the editor -- the same contract every other feature here follows.
    static ActorPreview* create(rhi::IDevice& device, u32 size = 1024);
    ~ActorPreview() override;

    const char* name() const override { return "Aver.Render.ActorPreview"; }

    // ---- the editor side ----
    void setDrawList(std::vector<PreviewDraw> draws) { draws_ = std::move(draws); }
    void setCamera(const PreviewCamera& c) { camera_ = c; }
    PreviewCamera& camera() { return camera_; }
    // Frames the whole draw list. Called when a tab opens and when the model set changes, because an
    // actor authored ten metres across and one authored ten centimetres across both have to arrive
    // on screen without anybody scrolling.
    void frameAll();

    // The matrix the pass will use this frame. Exposed because a GIZMO has to project through the
    // very same camera the picture was drawn with -- deriving it independently is how a handle ends
    // up a few pixels off its object, and then a few more as the camera turns.
    void viewProj(f32 out[16]) const { buildViewProj(out); }

    // The texture the panel draws. 0 before the first render.
    u64 uiTextureId() const { return uiTextureId_; }
    u32 size() const { return size_; }
    // Squared and fixed at creation. The panel letterboxes rather than resizing per frame, because
    // destroying a texture the UI is drawing needs a waitIdle, and doing that during a panel drag
    // stalls the whole GPU once a frame.
    bool ready() const { return pipeline_ != 0; }

    // ---- the frame ----
    // prePass, not scenePass: this owns its targets and must run BEFORE the scene binds the
    // backbuffer, or it would have to put back everything it changed.
    void prePass(rhi::IRenderContext& ctx) override;

private:
    ActorPreview() = default;
    bool init(rhi::IDevice& device, u32 size);
    void buildViewProj(f32 out[16]) const;

    rhi::IDevice* device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::TextureHandle color_ = 0, depth_ = 0;
    rhi::PipelineHandle pipeline_ = 0;
    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    u64 uiTextureId_ = 0;
    u32 size_ = 0;
    // The colour target starts in ShaderResource because that is where every frame LEAVES it: the
    // UI samples it after the pass. Tracking the state this way means the pass's first barrier is
    // honest about where the resource actually is rather than about where it was created.
    bool everRendered_ = false;

    std::vector<PreviewDraw> draws_;
    PreviewCamera camera_{};
};

// The HLSL, exposed so a test can assert on what it declares rather than only on what it draws.
const char* actorPreviewShaderSource();

} // namespace aver::render::preview
