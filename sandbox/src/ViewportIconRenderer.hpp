// ICONS DRAWN INSIDE THE 3D VIEWPORT -- textured, camera-facing quads at world positions, drawn as
// EDITOR CHROME in overlayPass, after the camera post chain.
//
// NOT EditorIcons.hpp, WHICH IS A DIFFERENT THING WITH A SIMILAR NAME. That header is the editor's
// Material Icons codepoint vocabulary: glyphs in an ImGui label, in a panel. This draws a PNG into
// the 3D view. Named for the viewport rather than the editor to keep the two apart on the page.
//
// WAS transparentPass (INTO THE PRE-TONEMAP HDR SCENE TARGET), MOVED HERE for the same reason
// IDevice::drawLines moved: transparentPass writes scene-linear radiance, so the icon's PNG colour
// had to be inverse-tonemapped to survive the trip -- which auto-exposure and bloom then multiplied
// right back up (x70-150 in a lit level), so a Player Start sprite glowed like every other piece of
// chrome before EditorLines' fix. overlayPass writes DISPLAY colour directly: no exposure, no
// tonemap, no bloom, so the artist's PNG bytes are exactly what is shown.
//
// WHY A RENDER FEATURE AND NOT AN ImGui OVERLAY (this reasoning is unchanged by the move above,
// only which pass answers it). Projecting a world position to screen and blitting with
// ImGui::GetForegroundDrawList would be a fraction of this code, and the editor already has every
// piece it needs for it -- IDevice::uiTextureId hands out an ImTextureID today. It was rejected for
// one reason: a plain ImGui overlay has no notion of the scene's geometry at all, so the marker
// would show through walls, floors and its own level's geometry. Occlusion is not a polish detail
// for a spawn marker; a marker you can see through a building is actively lying about where it is.
// overlayPass keeps that: IDevice::sceneDepthTexture() is readable there as an ordinary shader
// resource (IRenderFeature::overlayPass's own contract), so IconPS runs the same manual
// distance-from-eye depth test PSEditorLine does (editor_lines.hlsl) in place of the hardware depth
// test transparentPass used to give for free.
//
// MODELLED ON particles::ParticleRenderer, which does the same job for a different caller: same
// vertex split across two POSITION slots, same CPU-baked billboard, same per-frame-in-flight buffer
// ring. Where this one differs it is because it is simpler in some ways (no blend modes, no GI seam,
// no sorting) and less simple in others: it samples two textures (its own icon plus the scene depth,
// for occlusion) rather than none, so it owns binding sets the particle renderer's ordinary path
// does not need.
//
// VULKAN: overlayPass IS called there (unlike the old transparentPass, which VulkanDevice never
// implemented at all) -- once VulkanDevice's resource factory exists, this renders under Vulkan too,
// with no further changes needed here.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// One camera-facing quad corner, already positioned in world space.
struct ViewportIconVertex {
    f32 pos[3];    // POSITION0 (xy) + POSITION1 (z) -- see viewport_icon.hlsl's IconVSIn
    f32 uv[2];     // TEXCOORD0
    u32 color;     // COLOR0, RGBA8Unorm, STRAIGHT alpha (the shader premultiplies)
};
static_assert(sizeof(ViewportIconVertex) == 24, "the vertex layout in the .cpp names these offsets");

class ViewportIconRenderer final : public rhi::IRenderFeature {
public:
    // 0 is "no icon" -- a handle a caller can hold before loadIcon has succeeded, or after it
    // failed, and pass to addIcon harmlessly.
    using IconHandle = u32;
    static constexpr IconHandle kNoIcon = 0;

    // Compiles the shaders and builds a best-effort pipeline. False when the backend exposes no
    // resource factory, exactly as ParticleRenderer::init reports the same condition.
    bool init(rhi::IDevice& device);
    void shutdown();

    // Decodes a PNG from an absolute path and uploads it. Returns kNoIcon on any failure, having
    // logged why -- a missing icon file must cost the caller a marker, never a frame.
    //
    // DELIBERATELY BY PATH, not by asset id: these are editor chrome staged beside the executable
    // (sandbox/CMakeLists.txt), the same way logo.png and compile-status.png are. They are not
    // project content and must not appear in a project's asset database.
    IconHandle loadIcon(const std::string& pngPath, const char* debugName);

    // Queues one icon for THIS frame. Cleared every overlayPass, so a caller re-adds every frame
    // it wants the icon -- immediate-mode, like the editor's own line drawing, because the thing
    // being drawn is a view of live editor state rather than a persistent scene object.
    //
    // `halfSize` is in world units (centimetres here), so the quad is a fixed WORLD size and shrinks
    // with distance like everything else in the scene. That was the choice made over a fixed SCREEN
    // size: a marker that stays the same pixel size wherever the camera goes reads as UI pasted on
    // top, and stops telling you how far away the thing is.
    void addIcon(const Vec3& worldPos, f32 halfSize, IconHandle icon, f32 alpha = 1.0f);

    const char* name() const override { return "EditorViewportIcons"; }
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

private:
    // `depthSamples` is the SCENE depth's sample count -- IDevice::sampleCount(), what
    // onRenderTargetsChanged's own `sampleCount` reports too (its `color`/`depth` are the SCENE
    // target's formats, irrelevant now that this pass targets the overlay/display target instead,
    // kOverlayTargetFormat -- see that constant's own comment for why it's a fixed value, not
    // queried). Rebuilds IconPS and the pipeline together, like OcclusionCuller's HZB seed kernel
    // recompiles for AVER_HZB_MS rather than keeping a permanent MS/non-MS pair -- a sample-count
    // change is rare enough that recompiling on it costs nothing worth avoiding.
    bool ensurePipeline(u32 depthSamples);
    bool ensureCapacity(usize vertexCount, usize indexCount);

    struct Icon {
        rhi::TextureHandle texture = 0;
        rhi::BindingSetHandle set = 0;   // one per icon: table 0, slot t0 is this icon's texture
    };
    struct Request { Vec3 pos; f32 halfSize; IconHandle icon; f32 alpha; };
    struct DrawCmd { u32 indexOffset, indexCount; IconHandle icon; };

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    rhi::PipelineHandle pso_ = 0;
    // 0 sample count = pso_ not built yet, forcing ensurePipeline's first call to build it regardless
    // of what dev_->sampleCount() happens to read.
    u32 builtDepthSamples_ = 0;

    // Index 0 is never used, so IconHandle 0 can mean "none" without a sentinel comparison at every
    // call site -- icons_[0] is a permanently empty slot.
    std::vector<Icon> icons_{Icon{}};

    // Same three-deep ring, and the same reason, as ParticleRenderer's: writeBuffer is immediate and
    // unsynchronised, so reusing one buffer while the GPU may still be reading last frame's draw
    // from it is a race rather than a saving. Five, not three: with frame generation on, overlayPass
    // runs twice per frame (generated image, then real), so four rotations can be in flight.
    static constexpr u32 kFramesInFlight = 5;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    usize vbCapacity_ = 0, ibCapacity_ = 0;
    u32 frame_ = 0;

    // TABLE 1: the scene depth, rewritten every frame (the handle is stable across a resize per
    // IDevice::sceneDepthTexture's own contract, but the descriptor is cheap to refresh and a stale
    // one after a resize would be a silent wrong-occlusion bug, not a crash -- OcclusionCuller's own
    // seed set does the same unconditional rewrite for the identical reason). One set PER FRAME IN
    // FLIGHT, not one rewritten in place: EditorLines.hpp's depthSet_ ring exists for exactly this --
    // rewriting a single set while a prior frame's draw might still be reading it through the GPU is
    // the same race D3D12's descriptor-race fix (ee333dc1) closed elsewhere.
    rhi::BindingSetHandle depthSet_[kFramesInFlight] = {};

    std::vector<Request> pending_;
    std::vector<ViewportIconVertex> verts_;
    std::vector<u32> idx_;
    std::vector<DrawCmd> draws_;
};

} // namespace aver::editor
