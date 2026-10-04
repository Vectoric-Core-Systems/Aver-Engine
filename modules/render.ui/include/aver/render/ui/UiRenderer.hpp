#pragma once
// Aver.Render.UI — the GPU half of the retained UI. Aver.UI produces a draw list; this turns one
// into draw calls. A render feature whose only hook is overlayPass, after the camera post chain
// with the backbuffer bound, so a HUD is never tonemapped, exposed or bloomed with the world.
#include "aver/rhi/RHI.hpp"
#include "aver/ui/UiDrawList.hpp"

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

    // Draws the submitted list into the bound backbuffer.
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

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

    rhi::PipelineHandle pipeline_ = 0;
    rhi::ShaderHandle   vs_ = 0, ps_ = 0;
    rhi::TextureHandle  white_ = 0;         // 1x1 opaque

    // Rotated per overlayPass. Five deep: a backend keeps two frames in flight, and with frame
    // generation on each frame runs overlayPass twice (the generated image, then the real one), so
    // four rotations can still be in flight; one spare as before.
    static constexpr u32 kFramesInFlight = 5;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    u32 frame_ = 0;
    u32 vbCapacity_ = 0;   // in vertices
    u32 ibCapacity_ = 0;   // in indices

    // The submitted list, flattened and held CPU-side between submit() and overlayPass().
    std::vector<aver::ui::UiVertex>  verts_;
    std::vector<u32>                 idx_;
    // One draw call: an index range, its texture and its scissor.
    struct Draw {
        u32 indexOffset = 0, indexCount = 0;
        rhi::TextureHandle texture = 0;
        aver::ui::UiClip clip{};
    };
    std::vector<Draw> draws_;

    // One cached texture-to-binding-set pair.
    struct TexBinding { rhi::TextureHandle texture = 0; rhi::BindingSetHandle set = 0; };
    std::vector<TexBinding> bindings_;
};

} // namespace aver::render::ui
