// ICONS DRAWN INSIDE THE 3D VIEWPORT -- textured, camera-facing quads at world positions.
//
// NOT EditorIcons.hpp, WHICH IS A DIFFERENT THING WITH A SIMILAR NAME. That header is the editor's
// Material Icons codepoint vocabulary: glyphs in an ImGui label, in a panel. This draws a PNG into
// the SCENE, tested against the scene's depth buffer. Named for the viewport rather than the editor
// to keep the two apart on the page.
//
// WHY A RENDER FEATURE AND NOT AN ImGui OVERLAY. Projecting a world position to screen and blitting
// with ImGui::GetForegroundDrawList would be a fraction of this code, and the editor already has
// every piece it needs for it -- IDevice::uiTextureId hands out an ImTextureID today. It was
// rejected for one reason: an overlay is composited over the finished frame, so the marker would
// show through walls, floors and its own level's geometry. Occlusion is not a polish detail for a
// spawn marker; a marker you can see through a building is actively lying about where it is.
// transparentPass is the seam that gives depth testing with depth-write off, which is exactly
// "occluded by geometry, occludes nothing".
//
// MODELLED ON particles::ParticleRenderer, which does the same job for a different caller: same
// vertex split across two POSITION slots, same CPU-baked billboard, same per-frame-in-flight buffer
// ring, same best-effort-then-onRenderTargetsChanged pipeline build. Where this one differs it is
// because it is simpler -- one pipeline, no blend modes, no GI seam, no sorting -- except for the
// one place it is not: it samples a texture, so it owns a binding set the particle renderer's
// ordinary path does not need.
//
// D3D12 ONLY IN PRACTICE, and that is inherited rather than chosen: VulkanDevice has no
// transparentPass call at all (only a comment noting it as future work), so particles do not render
// under Vulkan either and neither will these. Stated because it is a real gap, not hidden by a
// silent early return -- the feature registers and draws nothing there.
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

    // Queues one icon for THIS frame. Cleared every transparentPass, so a caller re-adds every frame
    // it wants the icon -- immediate-mode, like the editor's own line drawing, because the thing
    // being drawn is a view of live editor state rather than a persistent scene object.
    //
    // `halfSize` is in world units (centimetres here), so the quad is a fixed WORLD size and shrinks
    // with distance like everything else in the scene. That was the choice made over a fixed SCREEN
    // size: a marker that stays the same pixel size wherever the camera goes reads as UI pasted on
    // top, and stops telling you how far away the thing is.
    void addIcon(const Vec3& worldPos, f32 halfSize, IconHandle icon, f32 alpha = 1.0f);

    const char* name() const override { return "EditorViewportIcons"; }
    void transparentPass(rhi::IRenderContext& ctx) override;
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

private:
    bool buildPipeline(u32 sampleCount, rhi::Format color, rhi::Format depth);
    bool ensureCapacity(usize vertexCount, usize indexCount);

    struct Icon {
        rhi::TextureHandle texture = 0;
        rhi::BindingSetHandle set = 0;   // one per icon: slot t0 is this icon's texture
    };
    struct Request { Vec3 pos; f32 halfSize; IconHandle icon; f32 alpha; };
    struct DrawCmd { u32 indexOffset, indexCount; IconHandle icon; };

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    rhi::PipelineHandle pso_ = 0;
    u32 pipelineSampleCount_ = 1;
    rhi::Format pipelineColor_ = rhi::Format::Unknown, pipelineDepth_ = rhi::Format::Unknown;

    // Index 0 is never used, so IconHandle 0 can mean "none" without a sentinel comparison at every
    // call site -- icons_[0] is a permanently empty slot.
    std::vector<Icon> icons_{Icon{}};

    // Same three-deep ring, and the same reason, as ParticleRenderer's: writeBuffer is immediate and
    // unsynchronised, so reusing one buffer while the GPU may still be reading last frame's draw
    // from it is a race rather than a saving.
    static constexpr u32 kFramesInFlight = 3;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    usize vbCapacity_ = 0, ibCapacity_ = 0;
    u32 frame_ = 0;

    std::vector<Request> pending_;
    std::vector<ViewportIconVertex> verts_;
    std::vector<u32> idx_;
    std::vector<DrawCmd> draws_;
};

} // namespace aver::editor
