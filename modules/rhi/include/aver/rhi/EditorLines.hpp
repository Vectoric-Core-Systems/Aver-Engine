#pragma once
// EDITOR LINES AFTER THE CAMERA POST CHAIN -- the implementation behind IDevice::createLineMesh /
// drawLines / destroyLineMesh / setLineDepth / setLineWidth, shared by every backend so D3D12 and
// Vulkan draw the grid, gizmos, selection outlines and collider/nav overlays identically.
//
// Lines used to be rasterised straight into the HDR scene target with an inverse-tonemapped colour.
// Auto-exposure then multiplied them (x70-150 in a lit level) and bloom haloed them, so every gizmo,
// outline and marker glowed. Unreal composites its editor primitives after tonemapping; this does the
// same: a line shows exactly its authored display colour, at display resolution, untouched by
// exposure, tonemap, bloom, local exposure or the AverSR upscale.
//
// THICK LINES WITHOUT HARDWARE LINE WIDTH (1 px on D3D12): each segment of a line mesh is stored,
// once, as a quad whose four vertices all carry BOTH endpoints; editor_lines.hlsl's VSEditorLine
// projects the pair and pushes the corner sideways in SCREEN space by half the width, clipping the
// segment against the near plane first so a line running behind the camera cannot flip. A one-pixel
// feather anti-aliases the edge (premultiplied alpha).
//
// OCCLUSION WITHOUT THE SCENE'S DEPTH BUFFER BOUND: the display target and the scene depth differ in
// size under a render scale, so a depth-tested line samples the scene depth in its pixel shader and
// compares DISTANCES FROM THE EYE (both unprojected through gInvViewProjRel) with a small tolerance,
// which keeps a line lying ON a surface -- the grid on a floor, an outline on its own mesh -- visible.
//
// THE WIREFRAME VIEW (IDevice::setWireframe) draws here too, for the same reasons: while it is on the
// device queues every scene mesh with queueWire() instead of shading it, and replay() draws each one
// with FillMode::Wireframe in a flat Unreal wire colour, before the lines. No depth test, as in
// Unreal's Wireframe view: every edge shows, including the ones behind a surface.
//
// Built only on IResourceFactory / IRenderContext, so it has no backend code of its own. The owning
// device keeps the scene depth readable for the overlay stage and calls replay() there.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <vector>

namespace aver::rhi {

// One corner of a line segment's screen-space quad (see the header comment). Both endpoints on every
// corner; `corner` = {0 at a / 1 at b, -1 / +1 side}. The colour is this corner's endpoint's.
//
// Each position is TWO attributes, xy (RG32Float) + z (R32Float): the RHI's vertex Format enum has no
// three-component float, the same split ViewportIconVertex and ParticleVertex make. Vulkan numbers
// the attributes in declaration order, so this order is editor_lines.hlsl's ELIn order.
struct EditorLineVertex {
    f32 a[3];        // POSITION0 = a.xy, POSITION1 = a.z
    f32 b[3];        // POSITION2 = b.xy, POSITION3 = b.z
    u32 color;       // COLOR0, RGBA8Unorm, the display (sRGB-encoded) colour as authored, 0xAABBGGRR
    f32 corner[2];   // TEXCOORD0
};
static_assert(sizeof(EditorLineVertex) == 36, "editor_lines.hlsl's ELIn names these offsets");

class EditorLines {
public:
    // `res` is the owning device's own factory. False when the shaders did not compile; every call
    // below is then a harmless no-op, so a device without it simply draws no lines.
    bool init(IResourceFactory& res);
    // Releases every mesh, buffer, pipeline and shader. Safe to call twice.
    void shutdown();

    // IDevice::createLineMesh: `count` LineVertex, a line LIST (pairs). Builds the quad buffers once;
    // returns 0 for an empty/odd list or on failure. Handles start at 1 and are never reused (a
    // stale handle must address a dead mesh -- IDevice::destroyLineMesh's rule), so a mesh rebuilt
    // every frame still costs one 24-byte slot in meshes_ per rebuild; its GPU buffers do not (below).
    LineHandle create(const LineVertex* verts, u32 count);
    // IDevice::destroyLineMesh. The GPU buffers are retired, not freed on the spot: a draw of this
    // mesh may already be queued or in flight, so they are released kFramesInFlight replays later.
    // A small buffer (<= kMaxSpareBytes) is then SHELVED rather than destroyed, for a later create()
    // of about the same size to write into again: a mesh rebuilt every frame (the collider overlay's
    // moving bodies in Play) otherwise committed two upload buffers per frame AND grew the factory's
    // append-only buffer table by two slots per frame, forever.
    bool destroy(LineHandle mesh);
    // IDevice::drawLines: queues one draw with the CURRENT depth-test and width state.
    void queue(LineHandle mesh, const f32 world[16]);
    // The wireframe view: one scene mesh (a device MeshHandle, drawn through IRenderContext::drawMesh
    // with the engine's MeshVertex layout). `movable` picks Unreal's movable colour over its static
    // one; the device passes true for a compute-written (skinned / soft-body) mesh.
    void queueWire(MeshHandle mesh, const f32 world[16], bool movable);
    // IDevice::setLineDepth / setLineWidth. Sticky; captured per queue() call.
    void setDepthTest(bool on) { depthTest_ = on; }
    void setWidth(f32 pixels) { width_ = pixels >= 1.0f ? pixels : 1.0f; }
    bool depthTest() const { return depthTest_; }
    f32  width() const { return width_; }

    // Replays this frame's queue, then clears it. The device calls this in its overlay stage, right
    // after the post chain, with:
    //   - the display-resolution target (backbuffer or the editor's viewport texture) bound as the
    //     sole render target, format `targetFormat`, size targetW x targetH;
    //   - `sceneRect` = the 3D view's rect IN THAT TARGET'S PIXELS {x, y, w, h} (the scene viewport
    //     scaled from scene to display space); replay() sets viewport and scissor to it itself;
    //   - `sceneDepth` = IDevice::sceneDepthTexture(), already in ResourceState::ShaderResource, and
    //     `depthSamples` its sample count (> 1 reads it as Texture2DMS). 0 = no depth available:
    //     every line then draws on top.
    // Pipelines are (re)built lazily when targetFormat or depthSamples change.
    void replay(IRenderContext& ctx, u32 targetW, u32 targetH, const f32 sceneRect[4],
                TextureHandle sceneDepth, u32 depthSamples, Format targetFormat);
    // Drops the queue without drawing: a frame that never reached the overlay stage (suppressed,
    // device lost) must not carry its lines into the next one.
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
    // A retired buffer that outlived its replays and is held for reuse. `idle` counts replays since it
    // was shelved: acquire() takes it only once it is >= kFramesInFlight (a second, independent wait on
    // top of Retired's, since a reused buffer skips the factory's own fence-guarded release), and
    // retireTick() destroys it after kSpareLifeTicks of nobody wanting it.
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
    // The wireframe view's pair. Optional: if either fails, lines still draw and the view is empty.
    ShaderHandle wireVs_ = 0, wirePs_ = 0;
    PipelineHandle wirePso_ = 0;
    u32 builtSamples_ = 0;
    Format builtFormat_ = Format::Unknown;
    // Two binding sets per sample-count flavour would do; one per frame in flight is simplest and
    // keeps a rewritten descriptor from reaching a draw still executing (ViewportIconRenderer's rule).
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
