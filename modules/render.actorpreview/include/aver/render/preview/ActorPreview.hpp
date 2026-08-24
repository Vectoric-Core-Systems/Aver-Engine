#pragma once
// The actor preview: an actor's authored composition, drawn into a texture of its own.
// A render feature that owns its colour and depth targets; the panel samples the colour one.
#include "aver/rhi/RHI.hpp"

// Only reached when the material system exists at all -- see this module's own CMakeLists.txt for
// why it names Aver.Render.PBR.Materials with `if(TARGET ...)` rather than unconditionally, the way
// it once named Aver.Formats.Material and broke every PBR=OFF build with an LNK1104 in some
// unrelated target. MaterialSystem is the full type, not a forward declaration, because ActorPreview
// owns one by value below (its identity/fallback textures for a graph that samples a map this
// preview was never given an asset for).
#if AVER_MODULE_PBR
#include "aver/pbr/MaterialSystem.hpp"
#endif

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
    // The material graph to shade this draw with, or 0 for the preview's own simple key-light-plus-
    // fill shader -- which is what EVERY EXISTING CALLER gets, unchanged, without touching a line of
    // its own code. A non-zero value is the id pbr::materialGraphs().add() returned when the graph
    // was registered. Meaningless (and quietly ignored, falling back to the simple shader) in a
    // PBR=OFF build or before any graph has ever compiled -- see ActorPreview::createMaterialPipeline.
    u32 materialGraphId = 0;
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

    // The colour target itself, as an RHI handle rather than a UI id.
    //
    // ADDED FOR THE THUMBNAIL CACHE, which needs to copyTexture OUT of this target into something
    // that outlives the frame -- a UI id is opaque to the RHI and cannot be a copy source. Exposing
    // the handle does not make the target any less transient: it is still cleared and redrawn every
    // prePass, and a caller that keeps the HANDLE rather than copying the PIXELS has kept a pointer
    // to next frame's picture.
    rhi::TextureHandle colorTexture() const { return color_; }
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

#if AVER_MODULE_PBR
    // (Re)builds materialPipeline_ against whatever pbr::materialGraphs() currently holds. Called
    // from prePass whenever the registry's revision has moved past materialGraphRev_ -- see
    // VoxiRenderer::prePass's own pull on the identical revision number, which this mirrors for the
    // identical reason: materials load into the registry long after this feature is constructed.
    //
    // KEEPS THE LAST GOOD PIPELINE ON FAILURE, rather than leaving graph-shaded draws with nothing
    // to bind: a graph an author is mid-wire on will not compile every keystroke, and a preview that
    // goes black the instant a link is half-made is worse than one that lags a moment behind. A
    // failed rebuild leaves materialGraphRev_ where it was, so prePass retries on the very next
    // frame rather than treating the failure as seen.
    bool createMaterialPipeline();
#endif

    rhi::IDevice* device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::TextureHandle color_ = 0, depth_ = 0;
    rhi::PipelineHandle pipeline_ = 0;
    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    u64 uiTextureId_ = 0;
    u32 width_ = 0, height_ = 0;
    bool everRendered_ = false;

#if AVER_MODULE_PBR
    // The SECOND pipeline: shades through averEvalMaterial instead of the fixed key-light-plus-fill
    // above. Built lazily -- see createMaterialPipeline -- and only ever exists once a project has
    // registered at least one graph, so a project with none costs this struct's few bytes and
    // nothing else: no extra shader, no extra pipeline, no extra draw-time branch worth measuring.
    rhi::PipelineHandle materialPipeline_ = 0;
    rhi::ShaderHandle materialVs_ = 0, materialPs_ = 0;
    u64 materialGraphRev_ = ~0ull;   // the revision materialPipeline_ was last built against
    // Kept alive for as long as ActorPreview is, rather than a local inside createMaterialPipeline:
    // ShaderDesc::defines is a raw pointer and a backend (or a test recording the descs, as
    // ActorPreviewTest does) may read it any time after createShader returns.
    std::string materialDefines_;
    // This preview's OWN identity textures and fallback binding set -- never a project's real
    // material assets, which this feature has no way to resolve and was never asked to. A graph
    // that samples a map still gets a defined answer (white, flat, or black, per slot) rather than
    // an unbound descriptor table, exactly the guarantee MaterialSystem's own fallback exists to
    // make for a draw with no specific material; see prePass for how gMaterialGraphId still reaches
    // it despite the constants otherwise being the identity material's.
    pbr::MaterialSystem materialFallback_;
#endif

    std::vector<PreviewDraw> draws_;
    PreviewCamera camera_{};
};

// The preview's HLSL, appended to the shared prelude.
const char* actorPreviewShaderSource();

} // namespace aver::render::preview
