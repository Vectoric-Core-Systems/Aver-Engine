#pragma once
// PtSceneView -- an opt-in, progressive REFERENCE render of the REAL scene, through the same
// brute-force path tracer PtFurnaceTest already proves correct. Where PtFurnaceTest brings its own
// geometry to check the integrator's arithmetic, this is the other half: it is handed the scene the
// RASTER path already draws (through rhi::IRenderFeature::submitDraw, the exact hook VoxiRenderer's
// own draw-list capture uses) and the REAL camera (through rhi::IDevice::camera(), the exact accessor
// VoxiRenderer::fitCascades already reads), and turns them into a small, still-camera accumulator
// that converges toward a physically-based image the raster path cannot produce on its own -- soft
// indirect light, with no cheats and no denoiser hiding the noise.
//
// WHAT IT IS: a reference view, not a render mode. It suppresses the raster scene while registered
// (rhi::IRenderFeature::suppressesScene() is unconditionally true) and draws its own accumulator
// instead of it, at a small FIXED resolution independent of the swapchain, accumulating a bounded
// number of samples every still frame and resetting the moment the camera moves or the STATIC draw
// list changes -- an image that keeps accumulating across a camera move is not a reference image, it
// is a smear. It is registered ONLY when asked for -- SandboxApp's --pt-scene flag at startup, or the
// editor's own Path Tracing settings-page Quality combo (voxi::Settings::pathTracing, real as of the
// gap that used to leave it clamped to Off on every device -- see Voxi.cpp's status(Feature::
// PathTracing)) at any later frame, both funnelled through the SAME syncPtSceneView() reconciler --
// never by default: while unregistered it costs nothing, exactly like PtFurnaceTest.
//
// WHAT IT DELIBERATELY DOES NOT DO, stated here because a first-time user hitting any of these should
// read this comment before filing a bug:
//   - LIGHT SOURCE. Two sources reach this view, and only two. ptEnvironment() (PtShaders.hpp) is
//     skyColor() and nothing else -- INDIRECT/ambient light, collected when a bounce MISSES.
//     ptDirectSun() (PtShaders.hpp) is a next-event shadow ray fired at the one authored directional
//     light on every HIT -- DIRECT sun light, including on surfaces the sky itself cannot see (an
//     overhang, a wall facing away from open sky). Together that is "outdoors, or an interior that
//     sees sky and/or sun through a real opening". There is still no CLight (point/spot/area) and no
//     emissive term: a room lit only by placed lights or glowing materials, with no sky above it and
//     no line of sight to the sun, still correctly, honestly renders BLACK. That is not a bug in
//     this feature -- it is exactly what those two sources not existing here means.
//   - GEOMETRY. Only draws whose mesh has no compute-written vertex buffer are included -- the same
//     predicate VoxiRenderer::buildAccelerationStructures already applies (VoxiRenderer.cpp, gated on
//     IDevice::meshVertexBuffer). Skinned characters, particles, and anything else that writes its
//     own vertices every frame are silently absent: including them would mean the scene "changes"
//     every frame and the accumulator would never progress past sample 0.
//   - MATERIALS. PtSurface carries one flat linear albedo, produced by the host's AlbedoResolver
//     when it recognises the draw's binding, and by the legacy per-draw base colour otherwise (which
//     is also what every draw gets when no resolver is installed at all). No textures are sampled,
//     no metallic/roughness (the integrator is pure Lambertian), no emissive. A textured material
//     renders as its flat, decoded base colour.
//   - PERFORMANCE. No denoiser, no importance sampling of lights, no spectral anything, not
//     real-time. It is a progressive, still-camera-only reference; see kAccumWidth/kAccumHeight/
//     kMaxBounces/kSamplesPerStep/kMaxSamples below for the bounded numbers it stays inside.
#include "aver/pt/PathTracer.hpp"

#include <functional>
#include <vector>

namespace aver::pt {

class PtSceneView final : public rhi::IRenderFeature {
public:
    ~PtSceneView() override;

    // Compiles the integrator. False means this device cannot run it (no ray tracing, or the compute
    // pipeline would not build) -- reported as "unavailable", never as a wrong answer, exactly like
    // PtFurnaceTest::init(). Allocates no scene or accumulator yet: those are built lazily, from the
    // first frame's real draw list, inside prePass().
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool available() const { return pt_.available(); }

    // Turns one draw's OPAQUE binding into a flat linear albedo, or returns false to leave the draw
    // on its legacy per-draw base colour.
    //
    // THE HOST RESOLVES, NOT THIS MODULE -- the same division LandscapeRenderer::setSurfaceBinding
    // already uses, and for the same reason: submitDraw() is handed a binding handle and a block of
    // raw bytes with no type attached, and only the composition root knows what bound them. This
    // module links Core and the GENERIC RHI alone (see CMakeLists.txt) and so cannot name a pbr::
    // type to check against even if it wanted to. Guessing from the bytes instead -- "it is a
    // material if the block is the right SIZE" -- is what this seam exists to avoid: any unrelated
    // block of the same size would pass, silently, and the only symptom would be a wrong colour.
    //
    // Left unset, every draw uses its base colour, which is exactly right for a caller that has no
    // material system at all (PtFurnaceTest brings its own geometry and never sets one).
    using AlbedoResolver = std::function<bool(rhi::BindingSetHandle set, const void* constants,
                                              u32 bytes, f32 outAlbedo[3])>;
    void setAlbedoResolver(AlbedoResolver r) { resolveAlbedo_ = std::move(r); }

    const char* name() const override { return "Aver.PathTracer.SceneView"; }

    // ---- rhi::IRenderFeature ----
    void beginScene() override;
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                    const void* drawConstants, u32 drawConstantBytes) override;
    void prePass(rhi::IRenderContext& ctx) override;
    // Unconditional: this feature IS the reference view whenever it is registered at all. Whether it
    // ever runs is decided by the CALLER choosing whether to register it (see the class comment),
    // not by a runtime toggle here -- there is exactly one registered instance and it always replaces
    // the scene, the same shape PtFurnaceTest's registration already uses for "on at all == on".
    bool suppressesScene() const override { return true; }
    void scenePass(rhi::IRenderContext& ctx) override;
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    // Samples accumulated toward the CURRENT image (resets to 0 whenever the camera or the static
    // scene changes). For reporting convergence, not for anything this class reads itself.
    u32 samplesAccumulated() const { return sampleCursor_; }
    bool sceneReady() const { return sceneReady_; }

private:
    // One replayed STATIC draw: enough to rebuild a PtSurface from, nothing else. Captured a frame
    // behind the real draw list for the same reason VoxiRenderer::draws_/drawsPrev_ is: a frame's
    // list is not known complete until every drawMesh() call for it has happened.
    struct Draw {
        rhi::MeshHandle mesh = 0;
        f32 world[16];
        f32 albedo[3];
    };
    std::vector<Draw> draws_, drawsPrev_;

    // Tears down the OLD target and scene, rebuilds pt_'s scene from drawsPrev_, and creates a fresh
    // target and accumulator. Returns false when nothing static survived the filter, or the device
    // refused an acceleration structure or the accumulator itself.
    bool rebuildScene(rhi::IRenderContext& ctx);
    // (Re)writes the present binding set's SRV over target_.accum. Called after every rebuild, since
    // rebuildScene() always hands back a NEW buffer handle.
    bool ensurePresentResources();
    // Reads the REAL camera (rhi::IDevice::camera()) and turns it into a PtCamera with the
    // ACCUMULATOR's own aspect -- never the real viewport's, see the class comment. False when there
    // is no camera to read yet, or the projection is degenerate.
    bool deriveCamera(PtCamera& out) const;
    // An order-sensitive FNV-1a hash of drawsPrev_ (mesh, world bits, albedo bits) -- the exact same
    // shape and constants VoxiRenderer::giDrawsKey() uses, for the same reason: a cache whose
    // invalidation rule is "close enough" is a cache that goes stale silently, so this is bit-exact,
    // not tolerance-based.
    u64 drawsKey() const;

    // Empty unless the host installed one; see setAlbedoResolver.
    AlbedoResolver resolveAlbedo_;

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    PathTracer              pt_;
    PtTarget                target_;

    PtCamera curCam_{};
    bool     haveCam_ = false;

    u64  sceneKey_ = 0;
    bool sceneReady_ = false;
    u32  sampleCursor_ = 0;

    // ---- the presentation pass: gPtAccum (a StructuredBuffer) drawn to the scene colour target ----
    rhi::PipelineHandle   presentPso_ = 0;
    rhi::BindingSetHandle presentSet_ = 0;
    u32        presentSampleCount_ = 0;
    rhi::Format presentColorFormat_ = rhi::Format::Unknown;

    bool loggedUnavailable_ = false;
    bool loggedFirstFrame_  = false;
    bool dropCapLogged_     = false;
    bool convergedLogged_   = false;

    // ---- the bounded numbers this stays inside; see the class comment's PERFORMANCE point ----
    //
    // Small and fixed, independent of the swapchain: this is a reference view, never a render mode,
    // and its cost must not scale with however large the editor's viewport happens to be.
    static constexpr u32 kAccumWidth  = 480;
    static constexpr u32 kAccumHeight = 270;
    static constexpr u32 kMaxBounces  = 4;
    // 8 samples per still frame, at (bounces+1)*samples = 40 RayQuery traces per pixel per dispatch
    // over 480x270 = ~5.2M traces/dispatch -- see PtSceneView.cpp's own comment at the accumulate()
    // call site for how that compares to VoxiRenderer's own measured, TDR-conscious ray budget.
    static constexpr u32 kSamplesPerStep = 8;
    // Stops issuing further accumulate() dispatches once this many samples have landed, so leaving
    // the camera still for a long unattended session does not keep issuing GPU work forever. The
    // image stays on screen; it simply stops changing. 200 steps * 8 spp = 1600 samples/pixel.
    static constexpr u32 kMaxSamples = 1600;
    // Warn-and-drop past this many static instances in one snapshot, mirroring
    // VoxiRenderer::kMaxDraws's own reasoning: addScene()'s createTlas(count) sizes to the snapshot's
    // EXACT count, with no headroom, so an unbounded snapshot would try to build one BLAS per
    // instance with no limit.
    static constexpr u32 kMaxInstances = 4096;
    // Root CBV register for the presentation pixel shader's own tiny constant block (accumulator
    // width/height). This pipeline never includes rhi::sharedShaderPrelude() on the PIXEL side (only
    // the vertex shader borrows its VSky entry point), so b1 here is unrelated to what b1 means to a
    // pipeline that does -- same reasoning AverSrFxaa.cpp's kFxaaConstantRegister gives.
    static constexpr u32 kPresentConstantRegister = 1;
};

} // namespace aver::pt
