#pragma once
// The actor preview: an actor's authored composition, drawn into a texture of its own.
// A render feature that owns its colour and depth targets; the panel samples the colour one.
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

namespace aver::render::preview {

// One placement to draw, derived from a parsed designer-file row rather than from an entity.
struct PreviewDraw {
    rhi::MeshHandle mesh = 0;
    f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // row-major, row-vector, cm
    f32 baseColor[4] = {0.8f, 0.8f, 0.82f, 1.0f};
    f32 boundsRadius = 0.0f;   // mesh's own units, before the world matrix
    f32 metallic = 0.0f;
    f32 roughness = 0.6f;
    bool selected = false;
};

// The preview's orbit camera: yaw, pitch and distance about a pivot.
struct PreviewCamera {
    f32 yawDeg = 35.0f;
    f32 pitchDeg = 20.0f;
    f32 distance = 400.0f;          // centimetres from the pivot
    f32 pivot[3] = {0.0f, 0.0f, 0.0f};
    f32 fovDeg = 45.0f;
    // Turns the orbit, clamping pitch short of the pole.
    void addOrbit(f32 dYaw, f32 dPitch);
    // Scales the orbit distance by a factor.
    void addZoom(f32 factor);

    // Slides the pivot across the view plane by a mouse delta in screen pixels.
    // `viewportHeightPx` is the RENDERED height, not the widget's.
    void panPixels(f32 dxPx, f32 dyPx, f32 viewportHeightPx);
};

// Draws a list of placements into its own targets, ahead of the scene pass.
class ActorPreview final : public rhi::IRenderFeature {
public:
    // Creates the feature. Returns null when the backend has no GPU support.
    static ActorPreview* create(rhi::IDevice& device, u32 width = 1024, u32 height = 0);
    ~ActorPreview() override;

    const char* name() const override { return "Aver.Render.ActorPreview"; }

    void setDrawList(std::vector<PreviewDraw> draws) { draws_ = std::move(draws); }
    void setCamera(const PreviewCamera& c) { camera_ = c; }
    PreviewCamera& camera() { return camera_; }
    // Points the camera at the whole draw list.
    void frameAll();

    // The matrix the pass will use this frame, so a gizmo can project through the same camera.
    void viewProj(f32 out[16]) const { buildViewProj(out); }

    // The texture the panel draws. 0 before the first render.
    u64 uiTextureId() const { return uiTextureId_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // Rebuilds the targets at a new size, keeping the old ones and returning false on failure.
    // Costs a waitIdle, so the caller must debounce rather than call it per frame.
    bool resize(u32 width, u32 height);

    bool ready() const { return pipeline_ != 0; }

    // Draws the preview into its own targets, before the scene binds the backbuffer.
    void prePass(rhi::IRenderContext& ctx) override;

private:
    ActorPreview() = default;
    // Builds the targets, the shaders and the pipeline.
    bool init(rhi::IDevice& device, u32 width, u32 height);
    // Creates a colour and depth target pair at a size.
    bool createTargets(u32 width, u32 height);
    // Composes the orbit camera's view and projection.
    void buildViewProj(f32 out[16]) const;

    rhi::IDevice* device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::TextureHandle color_ = 0, depth_ = 0;
    rhi::PipelineHandle pipeline_ = 0;
    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    u64 uiTextureId_ = 0;
    u32 width_ = 0, height_ = 0;
    bool everRendered_ = false;

    std::vector<PreviewDraw> draws_;
    PreviewCamera camera_{};
};

// The preview's HLSL, appended to the shared prelude.
const char* actorPreviewShaderSource();

} // namespace aver::render::preview
