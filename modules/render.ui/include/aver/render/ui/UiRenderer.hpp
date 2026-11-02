#pragma once
// Aver.Render.UI — the GPU half of the retained UI. Aver.UI produces a draw list; this turns one
// into draw calls. A render feature whose draw hook is overlayPass, after the camera post chain with
// the backbuffer bound, so a HUD is never tonemapped, exposed or bloomed with the world. Its other hook,
// prePass, uploads the glyph atlas pages (docs/GAME_UI_TEXT.md).
#include "aver/render/ui/WorldTextPlace.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiGlyphAtlas.hpp"

#include <vector>

namespace aver::render::ui {

// Draws a UiDrawList onto the backbuffer as an overlay pass.
class UiRenderer final : public rhi::IRenderFeature {
public:
    // Builds a renderer on a device. Returns nullptr when the backend has no resources().
    // The caller owns the result and must registerWith() it.
    static UiRenderer* create(rhi::IDevice& device);
    ~UiRenderer() override;

    const char* name() const override { return "Aver.Render.UI"; }

    // The list drawn by the NEXT overlayPass, copied rather than referenced. Submitting nothing
    // draws nothing; the previous frame's list is not retained.
    void submit(const aver::ui::UiDrawList& list);

    // ---- what a widget puts in UiDrawCmd::texture ----
    // Widens an rhi::TextureHandle into the plain u64 Aver.UI stores. 0 means the built-in white.
    static u64 textureId(rhi::TextureHandle t) { return static_cast<u64>(t); }
    // A glyph atlas page's token: kUiAtlasTokenBit | page. The renderer maps it to the
    // page's GPU texture; a plain rhi::TextureHandle is 32-bit, so the two can never collide.
    static u64 atlasToken(u32 page) { return aver::ui::kUiAtlasTokenBit | page; }

    // ---- the glyph atlas ----
    // Non-owning. Hosts call this after create() and pass nullptr BEFORE the engine that owns the atlas
    // or the device is torn down. A null atlas makes prePass a no-op; page textures stay for reuse.
    void setTextAtlas(aver::ui::UiGlyphAtlas* atlas);
    // The GPU texture of an atlas page, 0 when it has none yet (the editor shows these as thumbnails).
    rhi::TextureHandle pageTexture(u32 page) const;

    // Forgets the binding set made for a texture the UI drew (G4 R5: a sub-viewport's target when it is rebuilt or destroyed). A host calls it before the
    // texture goes; a texture the UI never drew is a no-op.
    void dropBinding(rhi::TextureHandle t);

    // Uploads every dirty atlas page: creates the RGBA8Unorm page texture on first use, expands A8 to
    // premultiplied RGBA8 (c, c, c, c) through the coverage-gamma LUT row by row at the footprint's row
    // pitch into one transient upload buffer, then per page: barrier ShaderResource -> CopyDest,
    // copyBufferToTexture, barrier back. The staging buffer is destroyed at once (the backends retire it
    // behind the fence). INVARIANT: every glyph a frame's list references must be rasterised before that
    // frame's beginFrame. Hosts run the UI frame in onUpdate, which satisfies it; a glyph rasterised later
    // (the ImGui layout-editor preview) appears one frame late.
    void prePass(rhi::IRenderContext& ctx) override;

    // The player's colour-vision filter for the HUD (rhi::packColourVision; 0 = off, which changes no pixel). The scene
    // composite filters the world; this pass draws after it, so it filters itself.
    void setColourVision(f32 packed);
    f32 colourVision() const { return colourVision_; }

    // Draws the submitted list into the bound backbuffer.
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

    // ---- world text (E6): the labels the list carries (UiDrawList::worldTexts) ----
    //
    // Drawn in the scene's transparent pass, depth-tested against the scene (or over it), from the world glyph atlas, with
    // the module's own plain raster shader (world_text.hlsl): vertex colour times glyph coverage, no lighting, no ray query,
    // no loop. Hosts lend the atlas (aver_text3d_atlas) the way they lend the UI atlas, and null it before teardown.
    void setWorldAtlas(aver::ui::UiGlyphAtlas* atlas);
    // The GPU texture of a world atlas page (RGBA8, premultiplied white coverage or a distance field), 0 before its first upload.
    // This is the texture a material reads to draw glyphs of its own.
    rhi::TextureHandle worldPageTexture(u32 page) const;
    // World text on or off (default on).
    void setWorldTextEnabled(bool on) { worldEnabled_ = on; }
    bool worldTextEnabled() const { return worldEnabled_; }
    // The host says when the ray-driven renderer owns the frame. Labels are NOT drawn then unless setWorldTextInRayDriven(true):
    // that mode replaces the pass they draw in and has not been checked with them on a GPU.
    void setRayDrivenActive(bool on) { rayDriven_ = on; }
    void setWorldTextInRayDriven(bool on) { worldInRayDriven_ = on; }
    bool worldTextInRayDriven() const { return worldInRayDriven_; }
    // Distance-field edge softness used when a label does not set one (0 = from the screen-space derivative).
    void setWorldSdfSoftness(f32 s) { worldSoftness_ = s; }
    // Labels and quads drawn by the last transparent pass.
    u32 worldTextsDrawn() const { return worldDrawn_; }
    u32 worldTextsSkipped() const { return worldSkipped_; }

    void transparentPass(rhi::IRenderContext& ctx) override;
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth, u32 width, u32 height) override;

private:
    UiRenderer() = default;
    // Creates the white texel, the shaders, the pipeline and the first buffers.
    bool init(rhi::IDevice& device);

    // The binding set for a texture, created on first use and kept.
    rhi::BindingSetHandle bindingFor(rhi::TextureHandle t);
    // Grows the vertex/index buffers to hold at least this much. Safe mid-frame.
    bool ensureCapacity(u32 vertexCount, u32 indexCount);

    rhi::IDevice*          device_ = nullptr;
    rhi::IResourceFactory* res_    = nullptr;

    // One pipeline per (blend, filter, address), the default (Mix, Linear, Clamp) made at init and the rest on first use
    // (P41). Only the fixed-function blend and the baked sampler differ; Mul also takes the UiPSMul pixel shader.
    rhi::PipelineHandle pipelineFor(aver::ui::UiBlend blend, aver::ui::UiFilter filter, aver::ui::UiAddress address);
    rhi::PipelineHandle makePipeline(aver::ui::UiBlend blend, aver::ui::UiFilter filter, aver::ui::UiAddress address);
    static constexpr u32 kPipelineSlots = static_cast<u32>(aver::ui::UiBlend::Count) * 2 * 3;
    rhi::PipelineHandle pipelines_[kPipelineSlots] = {};
    bool pipelineTried_[kPipelineSlots] = {};
    rhi::PipelineHandle pipeline_ = 0;      // the default one
    rhi::ShaderHandle   vs_ = 0, ps_ = 0, psMul_ = 0;
    rhi::TextureHandle  white_ = 0;         // 1x1 opaque

    // Rotated per overlayPass. Five deep: a backend keeps two frames in flight, and with frame
    // generation on each frame runs overlayPass twice (the generated image, then the real one), so
    // four rotations can still be in flight; one spare as before.
    static constexpr u32 kFramesInFlight = 5;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    u32 frame_ = 0;
    f32 colourVision_ = 0;
    u32 vbCapacity_ = 0;   // in vertices
    u32 ibCapacity_ = 0;   // in indices

    // The submitted list, flattened and held CPU-side between submit() and overlayPass().
    std::vector<aver::ui::UiVertex>  verts_;
    std::vector<u32>                 idx_;
    // One draw call: an index range, its texture (a handle or an atlas token) and its scissor.
    struct Draw {
        u32 indexOffset = 0, indexCount = 0;
        u64 texture = 0;
        aver::ui::UiClip clip{};
        aver::ui::UiFilter filter = aver::ui::UiFilter::Linear;
        aver::ui::UiAddress address = aver::ui::UiAddress::Clamp;
        aver::ui::UiBlend blend = aver::ui::UiBlend::Mix;
    };
    std::vector<Draw> draws_;

    // One cached texture-to-binding-set pair.
    struct TexBinding { rhi::TextureHandle texture = 0; rhi::BindingSetHandle set = 0; };
    std::vector<TexBinding> bindings_;

    // The glyph atlas: one GPU texture per CPU page.
    aver::ui::UiGlyphAtlas* atlas_ = nullptr;
    struct PageTexture {
        rhi::TextureHandle tex = 0;
        u32 size = 0;
    };
    std::vector<PageTexture> pages_;

    // Uploads the dirty pages of one atlas into its page textures (the UI's and the world's share this). `tag` names them.
    void uploadAtlas(rhi::IRenderContext& ctx, aver::ui::UiGlyphAtlas& atlas, std::vector<PageTexture>& pages, const char* tag);

    // ---- world text ----
    bool initWorld();
    rhi::PipelineHandle worldPipelineFor(u32 flags);
    void destroyWorldPipelines();
    bool ensureWorldCapacity(u32 vertexCount, u32 indexCount);
    aver::ui::UiGlyphAtlas* worldAtlas_ = nullptr;
    std::vector<PageTexture> worldPages_;
    // A copy of the submitted labels, held between submit() and transparentPass().
    std::vector<aver::ui::UiWorldText> worldTexts_;
    std::vector<aver::ui::UiWorldTextRun> worldRuns_;
    std::vector<aver::ui::UiVertex> worldVerts_;
    std::vector<u32> worldIdx_;
    rhi::ShaderHandle worldVs_ = 0, worldPs_ = 0;
    bool worldShadersTried_ = false;
    rhi::PipelineHandle worldPipes_[8] = {};
    bool worldPipeTried_[8] = {};
    u32 worldSampleCount_ = 1;
    rhi::Format worldColor_ = rhi::Format::Unknown, worldDepth_ = rhi::Format::Unknown;
    u32 sceneHeight_ = 0;
    rhi::BufferHandle wvb_[kFramesInFlight] = {};
    rhi::BufferHandle wib_[kFramesInFlight] = {};
    u32 wvbCapacity_ = 0, wibCapacity_ = 0;
    u32 worldFrame_ = 0;
    bool worldEnabled_ = true;
    bool rayDriven_ = false;
    bool worldInRayDriven_ = false;
    f32 worldSoftness_ = 0.0f;
    u32 worldDrawn_ = 0, worldSkipped_ = 0;
};

} // namespace aver::render::ui
