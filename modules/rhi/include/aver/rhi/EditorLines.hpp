#pragma once
// Editor lines rendered after the post chain: grid, gizmos, selection outlines, collider overlays.
// Drawn at display resolution, unaffected by exposure, tonemap, bloom, or AverSR upscale.
//
// Lines are thick quads; each segment stores both endpoints, pushed sideways in screen space with
// 1 px feather anti-alias. Depth-tested against scene depth (sampled in pixel shader, comparing
// unprojected distances from camera). Wireframe view draws here too.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <vector>

namespace aver::rhi {

// One corner of a line segment's screen-space quad. Both endpoints on every corner;
// `corner` = {0 at a / 1 at b, -1 / +1 side}. Colour is this corner's endpoint's.
// Position is split: xy (RG32Float) + z (R32Float) — no three-component float in Format enum.
// Vulkan attribute order matches declaration order (editor_lines.hlsl's ELIn order).
struct EditorLineVertex {
    f32 a[3];        // POSITION0 = a.xy, POSITION1 = a.z
    f32 b[3];        // POSITION2 = b.xy, POSITION3 = b.z
    u32 color;       // COLOR0, RGBA8Unorm, the display (sRGB-encoded) colour as authored, 0xAABBGGRR
    f32 corner[2];   // TEXCOORD0
};
static_assert(sizeof(EditorLineVertex) == 36, "editor_lines.hlsl's ELIn names these offsets");

class EditorLines {
public:
    // `res` is the owning device's own factory. False when shaders did not compile; calls then no-op.
    bool init(IResourceFactory& res);
    // Releases every mesh, buffer, pipeline and shader. Safe to call twice.
    void shutdown();

    // IDevice::createLineMesh: `count` LineVertex, a line LIST (pairs). Returns 0 for empty/odd or on failure.
    // Handles start at 1, never reused (stale handle must address a dead mesh).
    LineHandle create(const LineVertex* verts, u32 count);
    // IDevice::destroyLineMesh. GPU buffers retired kFramesInFlight replays later (a draw may be in flight).
    // Small buffers shelved for later reuse; rebuilt every frame (moving bodies overlay) no longer commits two uploads/frame.
    bool destroy(LineHandle mesh);
    // IDevice::drawLines: queue one draw with current depth-test and width state.
    void queue(LineHandle mesh, const f32 world[16]);
    // Wireframe view: one scene mesh. `movable` picks Unreal's movable colour over static.
    void queueWire(MeshHandle mesh, const f32 world[16], bool movable);
    // IDevice::setLineDepth / setLineWidth. Sticky; captured per queue() call.
    void setDepthTest(bool on) { depthTest_ = on; }
    void setWidth(f32 pixels) { width_ = pixels >= 1.0f ? pixels : 1.0f; }
    bool depthTest() const { return depthTest_; }
    f32  width() const { return width_; }

    // Replays this frame's queue, then clears it. Called in overlay stage with display-resolution target,
    // scene viewport rect in display space, scene depth texture (ResourceState::ShaderResource, or 0 if unavailable),
    // depth sample count (> 1 = Texture2DMS). Pipelines built lazily on targetFormat or depthSamples change.
    // With frame interpolation, firstOfFrame=false skips retire tick (recycles buffer GPU may read), lastOfFrame=false
    // on first replay keeps queue for second. Defaults are for single-replay frames.
    void replay(IRenderContext& ctx, u32 targetW, u32 targetH, const f32 sceneRect[4],
                TextureHandle sceneDepth, u32 depthSamples, Format targetFormat,
                bool firstOfFrame = true, bool lastOfFrame = true);
    // Drops queue without drawing (frame suppressed or device lost).
    void discardQueue() { queue_.clear(); wireQueue_.clear(); }

    static constexpr u32 kFramesInFlight = 3;

private:
    struct Mesh {
        BufferHandle vb = 0, ib = 0;
        u32 vbCap = 0, ibCap = 0;   // the buffers' real sizes (>= what the mesh fills); 0 = too big to shelve
        u32 indexCount = 0;
        bool alive = false;
    };
    struct Draw {
        LineHandle mesh = 0;
        f32 world[16] = {};
        f32 width = 1.0f;
        bool depthTest = true;
    };
    struct WireDraw {
        MeshHandle mesh = 0;
        f32 world[16] = {};
        bool movable = false;
    };
    struct Retired { BufferHandle vb = 0, ib = 0; u32 vbCap = 0, ibCap = 0; u32 framesLeft = 0; };
    // Retired buffer held for reuse. `idle` counts replays since shelved; acquire() takes it once >= kFramesInFlight,
    // retireTick() destroys it after kSpareLifeTicks idle.
    struct Spare { BufferHandle h = 0; u32 cap = 0; u32 idle = 0; };
    static constexpr u32   kMaxSpareBytes   = 8u << 20;    // a bigger buffer is destroyed, not shelved
    static constexpr usize kMaxSpare        = 16;          // shelved buffers held at once (a mesh is two)
    static constexpr u32   kSpareLifeTicks  = 600;         // replays a shelved buffer waits for a taker
    static constexpr u32   kBufferGranule   = 64u << 10;   // acquire() rounds up to this; a committed buffer is 64 KiB-aligned anyway

    bool buildPipelines(u32 depthSamples, Format targetFormat);
    void retireTick();
    // An upload buffer of at least `bytes`: a shelved one of about that size if there is one, else new.
    // `cap` receives its real size, or 0 when it is too big to be shelved later.
    BufferHandle acquire(u64 bytes, const char* name, u32& cap);
    // Keeps a no-longer-drawn buffer for acquire(), or destroys it when it is too big or the pool is full.
    void shelve(BufferHandle h, u32 cap);

    IResourceFactory* res_ = nullptr;
    ShaderHandle vs_ = 0, ps_ = 0, psMs_ = 0;
    PipelineHandle pso_ = 0, psoMs_ = 0;
    // Wireframe view's pair. Optional: if either fails, lines still draw and view is empty.
    ShaderHandle wireVs_ = 0, wirePs_ = 0;
    PipelineHandle wirePso_ = 0;
    u32 builtSamples_ = 0;
    Format builtFormat_ = Format::Unknown;
    // Binding set per frame in flight (simplest; prevents rewritten descriptor from reaching in-flight draw).
    BindingSetHandle depthSet_[kFramesInFlight] = {};
    BindingSetHandle depthSetMs_[kFramesInFlight] = {};
    u32 frame_ = 0;

    std::vector<Mesh> meshes_{Mesh{}};   // slot 0 = the null handle
    std::vector<Draw> queue_;
    std::vector<WireDraw> wireQueue_;
    std::vector<Retired> retired_;
    std::vector<Spare> spare_;
    bool depthTest_ = true;
    f32  width_ = 1.0f;
};

} // namespace aver::rhi
