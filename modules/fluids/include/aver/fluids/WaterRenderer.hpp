// The water surface's render feature: an ordinary rhi::IRenderFeature drawing a Gerstner-displaced
// grid through transparentPass, the exact same seam and the exact same depth-tested/depth-write-off
// contract ParticleRenderer draws through (modules/particles/include/aver/particles/
// ParticleRenderer.hpp) -- see this class's own top-of-body comment for the one place its geometry
// strategy diverges from that precedent, and WHY. Expressed purely in generic rhi::, the same
// discipline VoxiRenderer and ParticleRenderer both document: this file may never include a backend
// header.
#pragma once
#include "aver/fluids/GerstnerWave.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstddef>

namespace aver::fluids {

// Draws one Gerstner-displaced ocean grid, recentred under the camera every frame without ever
// re-uploading its geometry. See the class body's own comment for why that is safe and, unlike
// ParticleRenderer's per-frame ring buffer, actually the CHEAPER choice here.
class WaterRenderer final : public rhi::IRenderFeature {
public:
    // ---------------------------------------------------------------------------------------------
    // WHY THE GRID IS BUILT ONCE, AS A STATIC BUFFER -- NOT RE-UPLOADED EVERY FRAME LIKE
    // ParticleRenderer's RING.
    //
    // ParticleRenderer's vertex buffer changes every single frame because particles MOVE in world
    // space and are simulated on the CPU: last frame's positions are simply wrong this frame, and
    // there is no way around re-uploading them (see ParticleRenderer.cpp's transparentPass, which
    // rebuilds verts_/idx_ from ParticleSystem's current state on every call).
    //
    // This grid's VERTICES NEVER MOVE. A vertex at local (x, z) always sits at local (x, z); only
    // the VERTEX SHADER displaces it, reading the same Gerstner wave list every water vertex in the
    // frame reads, from the per-feature constant buffer WaterRenderer.cpp uploads once per
    // transparentPass call (kFeatureFrameConstantRegister -- the exact root-CBV register
    // PathTracer's own feature-frame block uses, per PathTracer.cpp:88-90's precedent). Re-uploading
    // an unchanging 129x129 grid of local XZ positions every frame -- the SAME bytes, frame after
    // frame -- would be pure waste: a writeBuffer call and a GPU-visible copy for data that is
    // already correct sitting in a buffer from three frames ago.
    //
    // What DOES need to track the camera is the grid's ORIGIN: an infinite ocean cannot be one fixed
    // mesh, so the grid instead RECENTRES by changing a world-space origin uniform (`gridOrigin` in
    // the per-feature CB, computed from snapWorldToGridCm(camPos, cellSizeCm) -- see WaterRenderer.
    // cpp's transparentPass) rather than by regenerating geometry. This is the standard
    // clipmap/ocean-grid technique: the mesh is a fixed-topology "puck" that rides along under the
    // camera, snapped to a cell boundary so consecutive frames' origins differ by whole cells and no
    // vertex visibly slides -- the exact property GerstnerWaveTest's snapWorldToGridCm cases pin
    // down (idempotent, monotonic) independently of any of this rendering code.
    // ---------------------------------------------------------------------------------------------

    // Compiles the shaders and builds the static grid VB/IB once. Stores `dev`/its resource factory.
    // False when the backend exposes no resource factory or a shader/buffer fails to build -- see
    // ParticleRenderer::init's identical contract and its own comment on the two-step
    // best-effort-then-onRenderTargetsChanged pattern this follows too.
    bool init(rhi::IDevice& dev);
    void shutdown();

    // Advances the wave clock. Called once per gameplay frame by the composition root -- mirroring
    // particles::particleSystem().tick(world, dt)'s existing pattern of an external, explicit
    // per-frame drive call -- because IRenderFeature's own hooks (beginScene, prePass,
    // transparentPass, ...) never receive a dt; nothing in the RHI's own frame loop knows what
    // "seconds" means to a feature that keeps its own clock. Both hosts get it through
    // game::GameWater::update; without a caller the waves never leave t = 0.
    void tick(f32 dtSeconds);

    void setWaterLevelCm(f32 zCm) { waterLevelCm_ = zCm; }
    f32 waterLevelCm() const { return waterLevelCm_; }

    // Copies up to kMaxGerstnerWaves waves, silently clamping `count` down to that if the caller
    // asked for more, and logging that clamp exactly ONCE per WaterRenderer instance (not once per
    // call) so a host that calls this every frame with a fixed, oversized list does not spam the log
    // -- the general shape D3D12Device's own "log once" warnings use.
    void setWaves(const GerstnerWave* waves, size_t count);

    // `shallowRGB` tints the REAL reflected sky/sun (skyColor(R), the same dome PbrShaders/
    // VoxiShaders reflect off every other surface) rather than replacing it with a flat colour --
    // PSWater's own comment (WaterShaders.hpp) has the full argument, and this is the field this
    // class's own header comment on shallowColor_/deepColor_ below describes precisely. `deepRGB` is
    // the water body's own transmitted colour, still what the eye reads looking straight down through
    // an undisturbed surface.
    void setColors(const f32 shallowRGB[3], const f32 deepRGB[3]);

    // Places the grid over a horizontal rectangle instead of letting it recentre under the camera --
    // see transparentPass's own comment for why scaling the grid onto the bounds, rather than
    // discarding fragments outside them, is the design here. minXCm/minYCm/maxXCm/maxYCm are engine
    // X/Y in centimetres, matching aver::fmt::OcWaterPlacement::boundsMin/boundsMax's own axis order.
    void setWaterBoundsCm(f32 minXCm, f32 minYCm, f32 maxXCm, f32 maxYCm);

    // Returns to the unbounded, camera-recentred grid -- the behaviour a WATER record with no bounds
    // clause already gets, and the state this instance starts in.
    void clearWaterBounds();

    const char* name() const override { return "Water"; }

    // Rebuilds pso_ to bake the scene target's current sample count and formats -- MSAA IS BAKED
    // INTO THE PIPELINE AT CREATION (the same DECIDED-1 rule ParticleRenderer::onRenderTargetsChanged
    // follows, and the renderfeature scout's central risk item for this slice: get depth.write wrong
    // here and every draw behind the water plane either z-fights or vanishes).
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    // Draws the recentred grid, depth-tested against the opaque scene, blended over it. See
    // WaterRenderer.cpp's own comment on the draw sequence and on why depth-WRITE stays off.
    void transparentPass(rhi::IRenderContext& ctx) override;

    // Deliberately NOT overridden: suppressesScene/suppressesWholeFrame/scenePipeline/
    // overridesScenePipeline. Water in this slice is ordinary blended geometry drawn OVER an
    // otherwise-completely-normal scene -- it never replaces the scene pass, never hides the sky,
    // never wants its own pipeline bound for opaque draws. ParticleRenderer is the exact precedent:
    // the one other IRenderFeature that overrides transparentPass and touches none of those four
    // (modules/particles/include/aver/particles/ParticleRenderer.hpp has no override of any of
    // them either). A feature that DID want to replace scene geometry -- VoxiRenderer's ray-driven
    // mode, PtSceneView -- looks nothing like this one, and this one should not grow toward it by
    // habit.

private:
    // Builds (or rebuilds) pso_ against the given target shape. Called from init() with the device's
    // best-effort-at-startup values, and again from onRenderTargetsChanged whenever they actually
    // change -- ParticleRenderer::buildPipelines' own two-caller shape.
    bool buildPipeline(u32 sampleCount, rhi::Format color, rhi::Format depth);

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    rhi::PipelineHandle pso_ = 0;

    rhi::BufferHandle gridVb_ = 0, gridIb_ = 0;
    u32 gridIndexCount_ = 0;

    GerstnerWave waves_[kMaxGerstnerWaves]{};
    size_t waveCount_ = 0;
    bool loggedWaveOverflow_ = false;   // see setWaves' own comment: logged once, not once per call

    f32 waterLevelCm_ = 0.0f;

    // Unbounded (the default) is the infinite-ocean grid every WATER record got before bounds
    // existed: transparentPass recentres the grid under the camera and never touches these four.
    // Bounded is a pool -- see transparentPass's own comment for how min/max become the grid's
    // origin and scale.
    bool boundsEnabled_ = false;
    f32 boundsMinXCm_ = 0.0f, boundsMinYCm_ = 0.0f;
    f32 boundsMaxXCm_ = 0.0f, boundsMaxYCm_ = 0.0f;

    // shallowColor_ TINTS the real reflected sky (WaterShaders.hpp's envReflection); it no longer
    // stands in for the sky outright the way it did before this pass. deepColor_ is the water body's
    // transmitted colour, weighted by (1 - fresnel) rather than lerped against shallowColor_ (see
    // PSWater's own comment). Both values are unretouched from before this pass -- still linear,
    // still unvalidated placeholders per README.md -- because this change fixes what they MEAN, not
    // what they ARE; a real tuning pass against a screenshot is still open work.
    f32 shallowColor_[3] = {0.05f, 0.35f, 0.45f};
    f32 deepColor_[3]    = {0.01f, 0.05f, 0.12f};

    f32 elapsedSeconds_ = 0.0f;
    bool loggedBadDt_ = false;   // see WaterRenderer.cpp's tick(): logged once, not once per spike

    u32 bakedSampleCount_ = 1;
    rhi::Format bakedColorFmt_ = rhi::Format::Unknown;
    rhi::Format bakedDepthFmt_ = rhi::Format::Unknown;
};

} // namespace aver::fluids
