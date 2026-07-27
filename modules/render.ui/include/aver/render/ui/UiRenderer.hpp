#pragma once
// Aver.Render.UI — the GPU half of the retained UI, and NOTHING ELSE.
//
// This is the counterpart to Aver.UI and the reason that module has no RHI dependency. Aver.UI
// produces a draw list; this turns one into draw calls. The split is the same one Aver.Render.Voxi
// draws between its settings DLL and its renderer, and it buys the same things: the widget tree
// stays testable with no GPU, a second backend is a second file HERE and no change THERE, and there
// is exactly one place in the engine that knows how a UI vertex reaches a rasteriser.
//
// It is a render FEATURE (rhi::IRenderFeature), registered on the device like any other, and it
// implements exactly one hook: overlayPass, which runs after the camera post chain with the
// backbuffer bound. That is not an implementation detail, it is the whole reason a UI cannot be
// drawn as part of the scene -- a HUD is authored in display colours and must not be tonemapped,
// exposed or bloomed with the world behind it. A white panel drawn into the scene target would be
// the brightest thing in the frame and would stop the eye adaptation down over the entire image.
#include "aver/rhi/RHI.hpp"
#include "aver/ui/UiDrawList.hpp"

namespace aver::render::ui {

class UiRenderer final : public rhi::IRenderFeature {
public:
    // Returns nullptr when the backend has no resources() -- the Null device and the D3D11/Vulkan
    // stubs -- which is how the app declines to draw a UI rather than failing to start. The caller
    // owns the result and must registerWith() it before any of it reaches the screen.
    static UiRenderer* create(rhi::IDevice& device);
    ~UiRenderer() override;

    const char* name() const override { return "Aver.Render.UI"; }

    // The list drawn by the NEXT overlayPass. COPIED, not referenced: the caller rebuilds its list
    // every frame from a widget tree that may be gone by the time the pass runs, and a borrowed
    // pointer here would be a dangling read the GPU could not report.
    //
    // Submitting nothing draws nothing; the previous frame's list is NOT retained. A UI that
    // disappears when its owner stops submitting is the behaviour a hidden menu wants, and the
    // alternative -- last-good-list -- leaves stale widgets on screen with no way to clear them.
    void submit(const aver::ui::UiDrawList& list);

    // ---- what a widget puts in UiDrawCmd::texture ----
    // The value IS an rhi::TextureHandle, widened. Aver.UI stores it as a plain u64 precisely so it
    // never learns the RHI's vocabulary, which leaves the meaning of the number to this module: it
    // is the one that has to resolve it, and a convention with one owner is not a convention.
    // 0 means the built-in opaque white texture, so an untextured rect needs no registration at all.
    static u64 textureId(rhi::TextureHandle t) { return static_cast<u64>(t); }

    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

private:
    UiRenderer() = default;
    bool init(rhi::IDevice& device);

    // One binding set per distinct texture, created on first use and kept. A UI touches a handful of
    // atlases, so a linear scan beats a hash and the whole cache stays inspectable.
    rhi::BindingSetHandle bindingFor(rhi::TextureHandle t);
    // Grow the vertex/index buffers to hold at least this much. Recreation is safe mid-frame:
    // destroyBuffer is deferred by the RHI's contract until the GPU has passed every frame that
    // could still reference the old one.
    bool ensureCapacity(u32 vertexCount, u32 indexCount);

    rhi::IDevice*          device_ = nullptr;
    rhi::IResourceFactory* res_    = nullptr;

    rhi::PipelineHandle pipeline_ = 0;
    rhi::ShaderHandle   vs_ = 0, ps_ = 0;
    rhi::TextureHandle  white_ = 0;         // 1x1 opaque, so an untextured draw is a textured draw

    // Rotated per overlayPass. Three deep against a backend that keeps two frames in flight: the
    // margin is one buffer of a few hundred kilobytes, and the failure it prevents is the CPU
    // overwriting vertices the GPU is still reading, which shows as UI that flickers geometry from
    // the frame before rather than as anything that names itself.
    static constexpr u32 kFramesInFlight = 3;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    u32 frame_ = 0;
    u32 vbCapacity_ = 0;   // in vertices
    u32 ibCapacity_ = 0;   // in indices

    // The submitted list, flattened. Held CPU-side between submit() and overlayPass because those
    // happen at different points in the frame and the app owns neither.
    std::vector<aver::ui::UiVertex>  verts_;
    std::vector<u32>                 idx_;
    struct Draw {
        u32 indexOffset = 0, indexCount = 0;
        rhi::TextureHandle texture = 0;
        aver::ui::UiClip clip{};
    };
    std::vector<Draw> draws_;

    struct TexBinding { rhi::TextureHandle texture = 0; rhi::BindingSetHandle set = 0; };
    std::vector<TexBinding> bindings_;
};

} // namespace aver::render::ui
