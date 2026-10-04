#pragma once
// The actor preview: an actor's authored composition, drawn into a texture of its own.
// A render feature that owns its colour and depth targets; the panel samples the colour one.
#include "aver/rhi/RHI.hpp"

// Only reached when the material system exists at all -- see this module's own CMakeLists.txt for
// why it names Aver.Render.PBR.Materials with `if(TARGET ...)` rather than unconditionally, the way
// it once named Aver.Formats.Material and broke every PBR=OFF build with an LNK1104 in some
// unrelated target. MaterialSystem is the full type, not a forward declaration, because ActorPreview
// owns one by value below: its own identity/fallback textures for a graph or a real material that
// does not drive every slot, AND -- since PreviewDraw::materialHandle -- the same instance's ordinary
// bindingSet()/constants() lookup for whatever real material a caller hands it.
#if AVER_MODULE_PBR
#include "aver/pbr/MaterialSystem.hpp"
#endif

#include <algorithm>
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
    // The REAL material to shade this draw's textures and factors with, or 0 -- which is what EVERY
    // EXISTING CALLER gets, unchanged, without touching a line of its own code -- for the preview's
    // own identity textures (white base colour, flat normal, full roughness, no metal), exactly
    // today's behaviour. A non-zero value is a pbr::MaterialHandle: pbr::MaterialLibrary is a
    // process-global singleton, so a handle a caller registered there (or was handed by an asset's
    // own load) is a handle ActorPreview's materialFallback_ can already resolve, through
    // pbr::MaterialSystem::bindingSet()/constants(), to that material's REAL binding set and REAL
    // constant block -- no different from how VoxiRenderer::materials() resolves the same handles
    // for the scene renderer.
    //
    // ORTHOGONAL TO materialGraphId ABOVE, not a replacement for it: this field selects WHICH
    // TEXTURES AND FACTORS averStockAuthored samples, materialGraphId selects WHICH GRAPH (if any)
    // runs on top of that stock read -- see actor_preview_material.hlsli's own comment on why
    // graphId's `default: break` arm already means "exactly the stock material", i.e. a plain
    // textured surface with no custom graph at all. Setting this alone, with materialGraphId still
    // 0, is exactly that case: a real material's own textures, no graph.
    //
    // Declared as u32 rather than pbr::MaterialHandle for the identical reason materialGraphId is:
    // this header must stay a complete type in a PBR=OFF build, where pbr::MaterialHandle's
    // declaring header (MaterialSystem.hpp, included only #if AVER_MODULE_PBR above) never reaches
    // this translation unit at all. Meaningless (and quietly ignored, falling back to the identity
    // pair) in a PBR=OFF build, or before a host has ever wired a texture resolver in -- see
    // ActorPreview::setMaterialTextureResolver for what "wired in" means and what happens before it
    // is.
    u32 materialHandle = 0;
};

// How the preview shades its draws, as the asset editors' view-mode dropdown names them.
enum class PreviewViewMode : u8 { Lit = 0, Unlit = 1, Wireframe = 2, Normals = 3 };

// Which helpers the preview draws around the meshes.
struct PreviewShowFlags {
    bool grid = true;     // the floor grid under the draw list
    bool bounds = false;  // the draw list's world AABB as a thin box
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
    // Drops every draw naming `mesh` (when non-zero) or `materialHandle` (when non-zero), for a caller
    // about to destroy either.
    void dropDrawsUsing(rhi::MeshHandle mesh, u32 materialHandle = 0) {
        draws_.erase(std::remove_if(draws_.begin(), draws_.end(), [&](const PreviewDraw& d) {
            return (mesh != 0 && d.mesh == mesh) || (materialHandle != 0 && d.materialHandle == materialHandle);
        }), draws_.end());
    }
    void setCamera(const PreviewCamera& c) { camera_ = c; }
    PreviewCamera& camera() { return camera_; }
    // Points the camera at the whole draw list.
    void frameAll();

    // The matrix the pass will use this frame, so a gizmo can project through the same camera.
    void viewProj(f32 out[16]) const { buildViewProj(out); }

    // Lit by default. Wireframe falls back to Lit if the backend refused the wireframe pipeline.
    void setViewMode(PreviewViewMode m) { viewMode_ = m; }
    PreviewViewMode viewMode() const { return viewMode_; }
    // Grid on, bounds off by default. The preview is shared, so an editor sets these when it claims it.
    void setShowFlags(const PreviewShowFlags& f) { show_ = f; }
    const PreviewShowFlags& showFlags() const { return show_; }

    // The draw list's world-space AABB, from each mesh's own AABB where the backend measured one and
    // its boundsRadius sphere otherwise. False when nothing in the list has a mesh.
    bool worldBounds(f32 lo[3], f32 hi[3]) const;

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

#if AVER_MODULE_PBR
    // Lets the HOST tell this preview how to turn a material's TextureRef into a real GPU texture --
    // the exact callback pbr::MaterialSystem::setTextureResolver() itself takes, forwarded straight
    // to materialFallback_. This feature never installs one on its own: it links only Aver.Core and
    // the RHI (see createMaterialPipeline's own "no coat in the preview" comment on why), has no
    // project loaded and no path to resolve a TextureRef against, and reaching into the editor to
    // find one would couple this module to whichever editor happened to ask first. The pattern this
    // mirrors already exists once: Runtime/src/GameApp.cpp and sandbox/src/SandboxApp.cpp both call
    // `voxiRenderer_.materials().setTextureResolver(...)` on VoxiRenderer's own exposed
    // pbr::MaterialSystem the same way, after the host itself decides how a TextureRef becomes a
    // texture.
    //
    // UNTIL A HOST CALLS THIS, every TextureRef a bound PreviewDraw::materialHandle names resolves to
    // nothing (pbr::MaterialSystem::resolveTexture declines with no resolver set) and every slot
    // keeps sampling materialFallback_'s own identity pixel for that slot -- today's degrade, and the
    // correct one: white base colour, flat normal, full roughness, no metal, exactly as if
    // materialHandle had stayed 0.
    void setMaterialTextureResolver(pbr::MaterialSystem::TextureResolver fn, void* user) {
        materialFallback_.setTextureResolver(fn, user);
    }
#endif

private:
    ActorPreview() = default;
    // Builds the targets, the shaders and the pipeline.
    bool init(rhi::IDevice& device, u32 width, u32 height);
    // Creates a colour and depth target pair at a size.
    bool createTargets(u32 width, u32 height);
    // Composes the orbit camera's view and projection.
    void buildViewProj(f32 out[16]) const;
    // Builds the wireframe, backdrop, grid and bounds pipelines and their two meshes. Non-fatal:
    // whatever fails is skipped and the preview draws without it.
    void createChrome(const rhi::GraphicsPipelineDesc& meshDesc);

#if AVER_MODULE_PBR
    // (Re)builds materialPipeline_ against whatever pbr::materialGraphs() currently holds. Called
    // from prePass the first time ANY draw asks for it (a materialGraphId, a materialHandle, or
    // both) and again whenever the registry's revision has moved past materialGraphRev_ since -- see
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

    // pipeline_'s shaders with FillMode::Wireframe: fill mode is per pipeline in this RHI.
    rhi::PipelineHandle wirePipeline_ = 0;
    rhi::PipelineHandle backdropPipeline_ = 0, gridPipeline_ = 0, boundsPipeline_ = 0;
    rhi::ShaderHandle backdropVs_ = 0, backdropPs_ = 0, gridPs_ = 0, boundsPs_ = 0;
    rhi::MeshHandle gridQuad_ = 0, boundsBox_ = 0;
    PreviewViewMode viewMode_ = PreviewViewMode::Lit;
    PreviewShowFlags show_{};

#if AVER_MODULE_PBR
    // The SECOND pipeline: shades through averEvalMaterial instead of the fixed key-light-plus-fill
    // above -- the only pipeline in this preview that samples a bound texture table at all, so it is
    // what a real PreviewDraw::materialHandle needs even with materialGraphId still 0 (see that
    // field's own comment on averStockAuthored running unconditionally). Built lazily -- see
    // prePass's own trigger and createMaterialPipeline -- and only ever exists once some draw has
    // asked for a graph OR a real material, so a project with neither costs this struct's few bytes
    // and nothing else: no extra shader, no extra pipeline, no extra draw-time branch worth
    // measuring.
    rhi::PipelineHandle materialPipeline_ = 0;
    rhi::ShaderHandle materialVs_ = 0, materialPs_ = 0;
    u64 materialGraphRev_ = ~0ull;   // the revision materialPipeline_ was last built against
    // Kept alive for as long as ActorPreview is, rather than a local inside createMaterialPipeline:
    // ShaderDesc::defines is a raw pointer and a backend (or a test recording the descs, as
    // ActorPreviewTest does) may read it any time after createShader returns.
    std::string materialDefines_;
    // TWO JOBS, ONE INSTANCE, because they are the same MaterialSystem machinery either way. First,
    // as its name still says: this preview's OWN identity textures and binding set, the answer a
    // draw with PreviewDraw::materialHandle == 0 gets -- white, flat, or black per slot, never a
    // project's real assets, exactly the guarantee MaterialSystem's own fallback exists to make for
    // a draw with no specific material; see prePass for how gMaterialGraphId still reaches it despite
    // the constants otherwise being the identity material's. Second, since PreviewDraw grew a real
    // pbr::MaterialHandle field: THIS is also what resolves that handle to a REAL binding set and
    // REAL constants, through the ordinary bindingSet()/constants() any consumer of
    // pbr::MaterialLibrary's process-global registry uses (VoxiRenderer::materials() included) --
    // this feature was never given its own separate way to reach a project's materials, and does not
    // need one, because MaterialLibrary is the one place every handle already lives. What it still
    // cannot do UNSUPERVISED is turn a material's TextureRef into pixels: that needs a
    // TextureResolver, which only a host can honestly supply -- see setMaterialTextureResolver.
    pbr::MaterialSystem materialFallback_;
#endif

    std::vector<PreviewDraw> draws_;
    PreviewCamera camera_{};
};

// The preview's HLSL, appended to the shared prelude.
const char* actorPreviewShaderSource();

} // namespace aver::render::preview
