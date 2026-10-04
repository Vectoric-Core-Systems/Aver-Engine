// Textured, camera-facing quads at world positions, drawn as editor chrome in overlayPass.
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

    // Queues one icon for this frame; cleared every overlayPass (immediate-mode).
    void addIcon(const Vec3& worldPos, f32 halfSize, IconHandle icon, f32 alpha = 1.0f);

    const char* name() const override { return "EditorViewportIcons"; }
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

private:
    // Rebuilds pipeline if depth sample count changed.
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
    u32 builtDepthSamples_ = 0;   // 0 = not built yet

    // Index 0 is sentinel; never used.
    std::vector<Icon> icons_{Icon{}};

    // Ring buffer: 5 frames for frame interpolation support.
    static constexpr u32 kFramesInFlight = 5;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    usize vbCapacity_ = 0, ibCapacity_ = 0;
    u32 frame_ = 0;

    // Scene depth binding set per frame in flight (avoid race with GPU reads).
    rhi::BindingSetHandle depthSet_[kFramesInFlight] = {};

    std::vector<Request> pending_;
    std::vector<ViewportIconVertex> verts_;
    std::vector<u32> idx_;
    std::vector<DrawCmd> draws_;
};

} // namespace aver::editor
