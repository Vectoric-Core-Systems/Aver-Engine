// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/voxi/Voxi.hpp"

#include <unordered_map>
#include "aver/formats/GiCache.hpp"

#include <array>
#include <string>
#include <vector>

// Voxi the render feature: voxel cone traced global illumination, a cascaded directional shadow map
// for light injection, and DXR 1.1 inline RayQuery sun shadows. Expressed purely in generic RHI.
namespace aver::voxi {

// Voxi's GPU side: owns the volume, the shadow atlas, the acceleration structures and the lit
// pipelines, and runs its passes through rhi::IRenderFeature.
// Aver.Render.Voxi (the settings DLL) must NOT gain an RHI dependency; this target is where RHI use lives.
class VoxiRenderer final : public rhi::IRenderFeature {
public:
    // Creates every GPU resource. Returns false if the device cannot support the baseline.
    bool init(rhi::IDevice& device);
    // Destroys every resource and returns the feature to its uninitialised state.
    void shutdown();

    // Sets the quality settings this feature renders with.
    void setSettings(const Settings& s);
    // Places the GI volume: centre in world units, half-edge extent.
    void setVolume(const f32 center[3], f32 extent);
    // Sets the sun DIRECTION, which is all this renderer needs: it fits the shadow cascades to the
    // light axis and nothing else here depends on the sun.
    //
    // IT DOES NOT TAKE A COLOUR OR AN AMBIENT, and that is the correction rather than an omission.
    // It used to take both, store them, and never read them -- the shaders get `gSunColor` and
    // `gAmbient` from the DEVICE's frame constant buffer, which `IDevice::setSkyAtmosphere` fills
    // (D3D12Device.cpp:2181, :2191). Accepting them here made this look like a second place the sun
    // could be configured, so anyone changing them expected a visual result and got nothing.
    // One source of truth for what the sun looks like; this one owns only where it points.
    void setSunDirection(const f32 dirToLight[3]);
    // Where a baked GI volume may be cached between runs: "<project>\\DerivedDataCache\\GI".
    // Empty (the default) disables the cache entirely and nothing is read or written.
    //
    // A SETTER RATHER THAN THIS CLASS FINDING THE PROJECT, for the same reason setVolume is one:
    // render.voxi knows about volumes and radiance, not about projects, manifests or where a user
    // keeps their files. The editor and the game runtime both already push everything else this
    // renderer needs per frame; this joins them.
    void setGiCacheDir(const std::string& dir);

    // Replaces the scene with a raymarch of the volume.
    void setDebugView(bool on);
    // The editor's Unlit view mode. Mirrors RhiDevice::setUnlit, which only ever reaches the
    // RASTER path -- ray-driven primary visibility bypasses drawMesh entirely, so it has to be
    // told separately or the mode silently does nothing in the default renderer.
    void setUnlit(bool on) { unlit_ = on; }

    // A/B MEASUREMENT TOGGLE, not a quality setting: OFF forces cb_.voxelParams.w to 0, the same
    // "gates the cone trace" flag prePass already computes from giEnabled(), so PSMainVoxi's
    // `if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(...)` (VoxiShaders.hpp) and
    // ClusterMaterialShader.hpp's identically-gated call both skip the trace and fall back to the
    // SAME neutral values (ind = 0, ao = 1) the path already uses whenever the volume is not ready --
    // so the frame stays valid with GI simply absent from it, exactly as it would on a device where
    // giReady_ never became true.
    //
    // DELIBERATELY SEPARATE FROM giEnabled()/Settings::globalIllumination, which this does not touch.
    // Turning GI off through the existing setting also stops the volume from being BUILT --
    // voxelizePass/filterMips/giShadowPass never run (see prePass's own giEnabled() gate) -- so an A/B
    // comparison built on it would be comparing "cone trace on, volume built" against "cone trace off,
    // volume also not built, three other passes also gone from the frame". That conflates the trace's
    // own cost with the build passes', which already have their own separate top-level spans in the
    // GPU timing tree and do not need a second, confounded way to be measured. This toggle changes
    // NOTHING upstream of the shader read: the volume still voxelises and mip-filters every tick it
    // normally would, cb_ still carries a fresh cascade and volume placement, giReady_ is unaffected --
    // the only difference between an A run and a B run is whether the per-pixel forward-shader lookup
    // that samples the finished volume actually executes. That isolates exactly the thing a GPU
    // timestamp cannot bracket on its own (see coneTracedIndirect's own comment in VoxiShaders.hpp):
    // measure the "scene draw" span with this on, then off, same camera, same frame count, and the
    // delta is the cone trace's cost and nothing else's.
    void setConeTraceEnabled(bool on) { coneTraceEnabled_ = on; }
    bool coneTraceEnabled() const { return coneTraceEnabled_; }

    // Sets the occlusion rays traced per pixel toward the sun's disc, clamped to [1, kMaxShadowRays].
    //
    // A KNOB RATHER THAN A CONSTANT because the cost of ray-traced shadows was a feeling and had to
    // become a number: it is linear in this, and nothing could measure that slope while it was a 4
    // compiled into the renderer. The sequence rtShadow walks is nested, so raising this ADDS
    // samples between the ones already there rather than moving them -- which is what makes a sweep
    // over 1, 2, 4, 8 a convergence series instead of four unrelated images.
    void setShadowRays(u32 n);
    u32 shadowRays() const { return rtShadowRays_; }

    // Sets the ray-traced shadow's temporal amortisation tile edge, rounded to the nearest power of
    // two in [1, kMaxPixelsPerRayTile] -- see Settings::rtPixelsPerRayTile for the full contract.
    // 1 (the default) traces every pixel every frame and is bit-identical to having no denoiser.
    void setPixelsPerRayTile(u32 n);
    u32  pixelsPerRayTile() const { return rtPixelsPerRayTile_; }

    // Sets how many frames apart the GI volume (voxelizePass + filterMips) is rebuilt, clamped to
    // [1, kMaxGiUpdateInterval]. 1 (the default) rebuilds every frame -- bit-identical to the original
    // always-fresh behaviour. N>1 reuses the previous frame's voxelised+filtered volume for the N-1
    // frames in between: the cone trace still runs every frame (it is a per-pixel forward-shader
    // lookup, unaffected by this), it just samples a volume that is up to N-1 frames stale. See
    // Settings::giUpdateInterval for the full contract.
    void setGiUpdateInterval(u32 n);
    u32  giUpdateInterval() const { return giUpdateInterval_; }

    // WHAT THE SHADERS WERE ACTUALLY COMPILED FOR, which is not the same question as what
    // Settings::layeredBsdf currently holds. The setting can be changed at any time; the pipelines
    // were built once, and this reports what they contain. The editor compares the two to decide
    // whether to say a reload is needed -- see the Shading model combo on the Rendering page.
    bool layeredBsdfActive() const { return layeredBsdf_; }

    // Turns on the frame-period report. See rtShadowRays_ for what it is for and what it is not.
    // MEASUREMENT ONLY -- see AVER_RD_ABLATE's own block in voxi.hlsl for what each value removes and
    // why a timestamp cannot answer this question. Must be set BEFORE init(), because it becomes a
    // shader define and the pipelines are compiled once there. Any non-zero value renders a
    // deliberately WRONG frame; it exists to be timed, never to be shipped or wired to a quality tier.
    void setRayDrivenAblation(u32 mode) { rdAblate_ = mode; }

    void setFrameTimeReport(bool on) { frameTimeReport_ = on; }

    // The largest ray count accepted. Not a hardware limit: there is a recorded TDR history on this
    // machine, and 64 rays per pixel at 8x MSAA is how a knob turns into a device removal.
    static constexpr u32 kMaxShadowRays = 32;
    // The largest tile edge accepted for ray-traced shadow amortisation: 16x16, one traced pixel
    // covering 256, which is already an aggressive enough amortisation that a fast-moving shadow
    // caster or camera visibly lags the tile converging behind it.
    static constexpr u32 kMaxPixelsPerRayTile = 16;
    // The largest GI revoxelise interval accepted: 8 frames of staleness is already a visible lag for
    // anything moving through the volume at a normal pace.
    static constexpr u32 kMaxGiUpdateInterval = 8;

    // Starts a new frame's draw list; the passes replay the previous one.
    void beginScene() override;
    // Records one draw into this frame's list.
    // `translucent` DEFAULTS FALSE so every existing caller -- the editor's off-screen shadow
    // submit, the cluster path, the packaged game -- keeps its exact behaviour without being touched.
    // Only submitDraw's blended branch passes true. See Draw::translucent for what the flag costs a
    // draw and what it buys it.
    // `hiddenFromOwner` DEFAULTS FALSE for the same reason `translucent` does: every existing
    // caller keeps its exact behaviour untouched. Only the editor's owner-hide branch passes true.
    void submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                const void* drawConstants, u32 drawConstantBytes, bool translucent = false,
                bool hiddenFromOwner = false);
    // `blended` is the one thing this override has to look at that submit() itself never sees, and
    // it has to look at it BEFORE anything reaches draws_/drawsPrev_, not after -- everything
    // downstream of that list treats membership in it as "this is opaque scene geometry": voxelizePass
    // injects a member as a LIGHT SOURCE into the GI volume, shadowPass casts a hard shadow from it,
    // and buildAccelerationStructures puts it in the TLAS every reflection ray can hit. A translucent
    // pane in any one of those three is wrong in a different way (blocking indirect light, casting an
    // opaque black shadow from something you can see through, or turning every reflection of it solid)
    // -- three defects for the price of one missed filter. Defined in the .cpp, where the drop is
    // counted and reported; see that definition for the census.
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                    const void* drawConstants, u32 drawConstantBytes, bool blended = false) override;

    const char* name() const override { return "Voxi"; }
    // Acceleration structures, shadow map, clear volume, voxelise + inject, filter mips.
    void prePass(rhi::IRenderContext& ctx) override;
    // True whenever the feature initialised: Voxi owns the lit pixel shader outright.
    bool overridesScenePipeline() const override;
    rhi::BindingSetHandle sceneBindingSet() const override { return bindings_; }
    // Hands the backend Voxi's per-frame constant block.
    bool sceneConstants(const void** data, u32* bytes) const override;

    // Returns the lit pipeline for this frame, or 0 to let the backend use its own. `depthPrepassed`
    // selects the LessEqual/no-write depth-state variant for an instance depthPrepassPipeline()
    // already wrote depth for this frame -- see IRenderFeature's own comment on the contract, and
    // createScenePipelines() for how the two are built to agree.
    //
    // `blended` selects the premultiplied-alpha, depth-write-off twin of the SAME shading -- the identical
    // vsMain/msMain vertex stage and psVoxi/psRt pixel stage the opaque family below uses, just a
    // different fixed-function state (see createScenePipelines()'s blended-twins step). It takes
    // priority over depthPrepassed in this override -- not because the two ever arrive together (the
    // base class's own comment says IDevice::drawMesh never routes a blended draw down the prepass
    // path) but because "blended" is a strictly narrower question with its own complete answer, and
    // answering it first keeps this function from having to reason about a combination that cannot
    // occur. meshShaders IS still honoured for a blended draw: a translucent instance submitted
    // through the mesh-shader path gets a mesh-shader blended pipeline rather than a dropped draw --
    // see the .cpp for why that axis could not be assumed away the way it was for the depth-prepass
    // twins. wireframe is NOT honoured, for a much older reason: this feature has never built a
    // wireframe scene pipeline at all, opaque or blended, and returns 0 for it exactly as before.
    rhi::PipelineHandle scenePipeline(bool meshShaders, bool wireframe, bool depthPrepassed = false,
                                      bool blended = false) const override;
    // The depth-only prepass pipeline: VSMain (the SAME compiled vertex shader scenePipeline()'s own
    // non-mesh-shader variants use) paired with PSDepthPrepass, which alpha-tests and clips but
    // writes no colour -- see VoxiShaders.hpp's PSDepthPrepass for what that costs and why it still
    // pays for itself. 0 until createScenePipelines() has run once, and 0 forever on a device/shader
    // combination that could not compile it -- IDevice::drawMeshDepthPrepass degrades to a no-op in
    // that case, exactly like every other optional Voxi pipeline (mesh-shader, ray-traced) already
    // degrades when its own compile fails.
    rhi::PipelineHandle depthPrepassPipeline() const override;

    // True while the debug view replaces the scene, including the backend's line draws.
    rhi::BindlessTableHandle sceneBindlessTable() const override;
    bool suppressesScene() const override;
    // ONLY the debug raymarch owns the whole frame. Ray-driven mode replaces how the first
    // surface is found and nothing else -- the sky, the gizmos and the particles in that frame are
    // as real as they are in a rastered one. See IRenderFeature::suppressesWholeFrame.
    bool suppressesWholeFrame() const override;
    // Draws the debug raymarch over the already-bound colour target.
    void scenePass(rhi::IRenderContext& ctx) override;

    // Rebuilds the pipelines that bake the sample count and target formats, and resizes the
    // screen-resolution ray-traced shadow history (see rtShadowHist_).
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    // The GPU residency of the material library, so the app can ask for a draw's binding set.
    pbr::MaterialSystem& materials() { return materials_; }

    // ---- the modular seam: letting a FOREIGN pipeline merge Voxi's table 0 into its own ----
    // (Stage 3, GPU per-cluster shading parity -- see VoxiGiShaders.hpp for the HLSL half of this
    // and SandboxApp.cpp's ensureLodMeshPipeline for the one real caller.) Voxi itself never learns
    // what a cluster is; the caller never learns voxelTex_/shadowTex_'s handles or kind. Both sides
    // only agree on a shape (VoxiGiShaders.hpp's kGiSrvCount/kGiUavCount) and a base register the
    // caller picked for itself.

    // Writes the GI volume and the shadow map into `set` at srvBase/srvBase+1 -- see
    // VoxiGiShaders.hpp's giShaderDefines() for why only these two of the table-0 union get a real
    // descriptor. `res` is the caller's own IResourceFactory (normally the same one this renderer
    // was init()ed with, but not assumed to be: nothing here reads res_).
    void bindGiResources(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase) const;

    // This frame's `cbuffer VoxiFrame` bytes -- the SAME block sceneConstants() hands the backend
    // for Voxi's own pipeline, byte for byte what VoxiGiShaders.hpp's giShaderPrelude() declares.
    // A caller binds this at whatever b-register it passed as giShaderDefines()'s
    // frameConstantRegister, every draw that uses that prelude -- there is no push/pull notification
    // when cb_ changes, only "read the current one before you draw".
    const void* giFrameConstants() const { return &cb_; }
    u32 giFrameConstantBytes() const { return sizeof(cb_); }

private:
    // Creates the cascaded shadow atlas.
    bool createShadowResources();
    // Creates the radiance volume, the injection accumulator and every binding set over them.
    bool createVoxelVolume(u32 resolution);
    // Creates every pipeline the feature runs.
    bool createPipelines();
    // Creates the subset that bakes sample count and render-target formats -- now TWO pipelines per
    // combination instead of one, see the "Gbuf" members below for the full shape.
    bool createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
    // Picks between a pipeline and its G-buffer twin: `gbuf` when it exists AND the backend has the
    // G-buffer switched on, `plain` otherwise. See the "Gbuf" pipeline members' own comment for why
    // this asks dev_->gBufferEnabled() itself instead of taking a parameter -- IRenderFeature::
    // scenePipeline's signature (RHIResources.hpp) was not widened for this feature. `plain` is
    // always the safe fallback: init() already requires scenePso_ (the one pipeline with no "or 0"
    // in its own contract) to be non-zero, so every call site already trusted it before this existed.
    rhi::PipelineHandle pickGbuf(rhi::PipelineHandle plain, rhi::PipelineHandle gbuf) const;
    // Renders the replayed draw list into each cascade of the shadow atlas.
    void shadowPass(rhi::IRenderContext& ctx);
    // The GI-only depth pass: one box over the GI volume, run on the frames voxelizePass runs.
    void giShadowPass(rhi::IRenderContext& ctx);
    // Builds a BLAS per referenced mesh and one TLAS over the replayed draw list.
    void buildAccelerationStructures(rhi::IRenderContext& ctx);
    // Clears the accumulator, rasterises the scene into the volume with direct light, resolves it.
    void voxelizePass(rhi::IRenderContext& ctx);
    // Box-filters each mip of the volume into the next.
    void filterMips(rhi::IRenderContext& ctx);

    pbr::MaterialSystem materials_;

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::DeviceCaps caps_{};
    Settings settings_{};

    rhi::TextureHandle shadowTex_ = 0;
    rhi::PipelineHandle shadowPso_ = 0;
    // Same depth-only pass, drawn with IRenderContext::drawMeshInstanced instead of one drawMesh()
    // per surviving draw -- see shadowPass()'s per-cascade mesh grouping and VSShadowInstanced in
    // VoxiShaders.hpp. 0 on a device/shader-compile combination that couldn't build it; shadowPass()
    // then falls back to shadowPso_'s one-draw-per-instance path automatically.
    rhi::PipelineHandle shadowInstancedPso_ = 0;

    // The GI-only shadow map: one box fitted to the GI VOLUME, never the camera, so the cascades
    // above are free to stay fitted to what the camera can actually see. Rendered on exactly the
    // frames voxelizePass runs, because PSVoxel is its only reader.
    //
    // A ZERO HANDLE IS A SOFT FAILURE, matching shadowInstancedPso_ above: giShadowFactor falls back
    // to fully-lit indirect rather than failing init(). Wrong-but-running beats a dead renderer for
    // a term that only affects bounce light.
    rhi::TextureHandle  giShadowTex_ = 0;
    rhi::PipelineHandle giShadowPso_ = 0, giShadowInstancedPso_ = 0;

    // Radiance volume: RGBA16F Tex3D, full mip chain. Mip N is the cone footprint at distance N.
    rhi::TextureHandle  voxelTex_ = 0;
    // Injection accumulator: R32_UINT Tex3D, (res*4) x res x res, channels interleaved along x.
    rhi::TextureHandle  voxelAccumTex_ = 0;
    rhi::PipelineHandle voxelPso_ = 0, voxelMsPso_ = 0, mipPso_ = 0, clearPso_ = 0, debugPso_ = 0;
    rhi::PipelineHandle resolvePso_ = 0;
    // t0 volume (whole chain), t1 shadow, t2 TLAS, u0 volume mip 0, u1 injection accumulator.
    rhi::BindingSetHandle bindings_ = 0;
    // UAVs only (u0 volume mip 0, u1 accumulator): no SRV may be live while the clear runs.
    rhi::BindingSetHandle clearBindings_ = 0;
    // Same shape as the clear set: the accumulator-to-volume reduction.
    rhi::BindingSetHandle resolveBindings_ = 0;
    // One per mip filter step: set m reads mip m-1 and writes mip m.
    std::vector<rhi::BindingSetHandle> mipBindings_;

    rhi::PipelineHandle scenePso_ = 0, sceneMsPso_ = 0, sceneRtPso_ = 0, sceneMsRtPso_ = 0;
    // The blended twins of the four handles immediately above: same compiled vsMain/msMain/psVoxi/
    // psRt shader binaries (createScenePipelines never compiles a second copy of any of them), a
    // GraphicsPipelineDesc identical to the matching opaque variant except
    // blend = BlendMode::PremultipliedAlpha and depth.write = false -- premultiplied rather than
    // straight so PSMainVoxi's blended branch can hand back a specular reflection at full strength and
    // a diffuse term weighted by coverage instead of attenuating both by the same alpha (see that
    // branch's own comment in VoxiShaders.hpp for the packing). See scenePipeline()'s own comment for
    // the selection rule and createScenePipelines()'s blended-twins step for what these do and do not
    // give a translucent draw -- shading (shadow lookup, cone-traced indirect) is the SAME code path
    // an opaque draw gets; casting a shadow, appearing in a reflection hit and injecting radiance are
    // not, because submitDraw() DROPS a blended draw explicitly before it ever reaches draws_ -- see
    // that function's own comment. (It used to be simpler than that and wrong in a way nothing
    // exercised: `blended` did not exist as a submitDraw parameter before the shading-contract change
    // that added it, so every draw this override ever saw was opaque by construction and there was
    // nothing to filter. The exclusion is deliberate now; it was accidental before.)
    //
    // ALL FOUR EXIST, mirroring the mesh-shader x ray-tracing axes of the opaque family exactly --
    // unlike depthPrepassPso_'s colour-pass twins two members below, which get to skip the
    // mesh-shader axis on the strength of one known caller (SandboxApp.cpp's plain drawMesh()
    // prepass), scenePipeline()'s blended branch has no such guarantee about how a translucent draw
    // will arrive, so the smallest CORRECT set here is the full mirror, not the smallest one that
    // happens to work with today's one caller.
    rhi::PipelineHandle sceneBlendedPso_ = 0, sceneMsBlendedPso_ = 0, sceneRtBlendedPso_ = 0,
                        sceneMsRtBlendedPso_ = 0;
    // The ray-driven primary-visibility pass: one fullscreen triangle whose pixel shader traces
    // the camera ray itself. Null unless the device has ray queries, because PSRayDriven only
    // compiles into the SM 6.5 AVER_RT variant.
    rhi::PipelineHandle rayDrivenPso_ = 0;
    // The depth prepass and its two "already prepassed" scene-colour twins -- see
    // depthPrepassPipeline()'s own comment. NO mesh-shader twins: the prepass is only ever offered to
    // the plain drawMesh() path (see SandboxApp.cpp's caller), so scenePipeline() never needs a
    // prepassed variant of sceneMsPso_/sceneMsRtPso_ and this feature does not build one.
    rhi::PipelineHandle depthPrepassPso_ = 0;
    rhi::PipelineHandle scenePsoPrepassed_ = 0, sceneRtPsoPrepassed_ = 0;

    // ---- the G-buffer twins: one more axis alongside mesh-shader x ray-tracing x blended x
    // depth-prepassed, not a fifth boolean threaded through scenePipeline() ----
    //
    // Every scene-lit pipeline above (the eight opaque/blended combinations, the two prepassed
    // ones, and rayDrivenPso_ two pages up) gets a twin here: the IDENTICAL vertex/mesh-shader
    // stage, the IDENTICAL fixed-function state (blend mode, depth test/write), but a PIXEL shader
    // recompiled with "AVER_GBUFFER=1" appended to its define string and a renderTargetCount of 4
    // instead of 1 -- see createScenePipelines()'s "Gbuf" step for exactly where each is built.
    // VoxiShaders.hpp's own #if AVER_GBUFFER blocks (GBufferOut / RayDrivenGBufferOut) are what make
    // that define change the pixel shader's RETURN TYPE, which is why this needs a SECOND compiled
    // shader and a SECOND pipeline object rather than a runtime branch inside one: a PSO's render-
    // target count and formats are fixed at creation on both backends, so "1 target normally, 4
    // when the G-buffer is on" cannot be one PSO that sometimes writes a different shape.
    //
    // THE THREE EXTRA FORMATS ARE FIXED, NOT PASSED IN, because onRenderTargetsChanged only ever
    // hands this class a colour format, a depth format and a sample count -- the three new targets'
    // formats are a property of the G-BUFFER FEATURE, not of the swapchain/MSAA state, so they are
    // compiled in as the same three constants IDevice::gBufferVelocityTexture() /
    // gBufferViewZTexture() / gBufferNormalRoughnessTexture()'s own comments (RHI.hpp) already fix:
    // RG16F, R32Float, RGB10A2Unorm, in SV_TARGET1/2/3 order.
    //
    // ALL OPTIONAL, THE SAME WAY THEIR NON-GBUF TWINS ALREADY ARE. A 0 here means exactly what a 0
    // means for sceneMsPso_/sceneRtPso_ today: this particular combination could not be built (an
    // older shader model, DXC unavailable, a compile failure), and pickGbuf() falls back to the
    // plain twin -- so a device that cannot compile any of these keeps rendering exactly as it does
    // today, just without a G-buffer, rather than losing the base pipeline underneath it too.
    //
    // NOT WHETHER THE G-BUFFER IS ON RIGHT NOW -- these are built unconditionally, alongside their
    // plain twins, gated only on the SAME device capabilities (msOk/rtOk) that already gate the
    // plain ones. Runtime on/off (dev_->gBufferEnabled(), toggled independently of any resize or
    // material-graph change) is pickGbuf()'s question, asked fresh every scenePipeline() call, not
    // this class's at creation time -- exactly so flipping the switch mid-run needs no rebuild.
    rhi::PipelineHandle sceneGbufPso_ = 0, sceneMsGbufPso_ = 0, sceneRtGbufPso_ = 0, sceneMsRtGbufPso_ = 0;
    rhi::PipelineHandle sceneBlendedGbufPso_ = 0, sceneMsBlendedGbufPso_ = 0,
                        sceneRtBlendedGbufPso_ = 0, sceneMsRtBlendedGbufPso_ = 0;
    rhi::PipelineHandle scenePsoPrepassedGbuf_ = 0, sceneRtPsoPrepassedGbuf_ = 0;
    // PSRayDriven's own twin (RayDrivenGBufferOut: the same three channels plus SV_DEPTH, which
    // this pass writes itself -- see VoxiShaders.hpp). Read only from scenePass(), never from
    // scenePipeline(): ray-driven mode is not selected through the IRenderFeature::scenePipeline()
    // contract at all (see rayDrivenActive()/suppressesScene()), so this has no place in that
    // function's branches and would be dead code if added there.
    rhi::PipelineHandle rayDrivenGbufPso_ = 0;

    rhi::TlasHandle tlas_ = 0;
    // One bottom-level structure per referenced mesh. Built once and kept for the run, EXCEPT for a
    // mesh whose vertices are written by compute -- IDevice::meshVertexBuffer is what says which --
    // where the cache expires every frame and the structure is rebuilt.
    //
    // FIXED: this used to say the predicate above was true of every mesh, forcing a two-rebuild-a-
    // frame cost onto ordinary static geometry that was never compute-written at all. That was
    // meshVertexBuffer returning GpuMesh::vbBuffer, which createMesh fills in for every mesh --
    // compute-written or not -- since a3022e0 gave a shader the ability to read a mesh's geometry.
    // D3D12Device.cpp now gates it on GpuMesh::computeWritten, set only by createSkinTargetMesh and
    // cleared by destroyMesh, so an ordinary static mesh's BLAS is cached across frames as this
    // comment always intended, and only a genuine skin target pays the per-frame rebuild.
    //
    // Either way the rebuild is a full PREFER_FAST_TRACE build rather than a refit, because the RHI
    // has no update verb. See the module README for the two verbs it would take; do not reach for
    // PERFORM_UPDATE against a structure that was not built with ALLOW_UPDATE.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blas_;
    bool dynamicBlasLogged_ = false;   // the per-frame rebuild is announced once, not every frame
    // The meshes already rebuilt during THIS frame's pass over the draw list.
    //
    // The draw list holds one entry per INSTANCE, so a mesh drawn twice used to be handed to
    // buildBlas twice -- two full builds of one structure, the second overwriting the first with the
    // identical answer, plus a second UAV barrier. Kept as a member so the allocation is made once
    // rather than every frame.
    std::vector<rhi::MeshHandle> rebuiltThisFrame_;
    // What the last frame's pass over the draw list actually cost, in structures. Logged only when
    // it CHANGES: a number this important should be visible, and a line every frame is noise.
    u32 lastBlasRebuilds_ = 0xFFFFFFFFu;
    // Occlusion rays per pixel toward the sun's disc. FOUR by default: one gives the hard aliased
    // edge this replaced, and the cost is linear, so this is the knob to turn down first if ray
    // tracing ever starts costing frames. There is a recorded TDR history on this machine, so it
    // deliberately does not default high. setShadowRays and --rt-rays move it.
    u32 rtShadowRays_ = 4;
    // Tile edge for the shadow's temporal amortisation. See setPixelsPerRayTile / Settings for the
    // contract; 1 traces every pixel every frame.
    u32 rtPixelsPerRayTile_ = 1;
    // The SPATIAL filter radius, mirrored from Settings::rtShadowDenoise by applySettings.
    u32 rtShadowDenoise_ = 0;
    // How fast the spatial filter tapers off with the gather centre's reprojection velocity.
    // 0 = no taper. A MEASUREMENT KNOB, not a tier setting: temporal accumulation is what actually
    // fixes the motion flicker, and this exists so the spatial filter's own share of it can be
    // isolated at runtime instead of by rebuilding with a line commented out.
    f32 rtDenoiseMotionTaper_ = 0.0f;
    // Settings::rtRenderMode, cached at setSettings like the knobs above it. 1 asks for
    // ray-driven primary visibility; whether it is HONOURED is rayDrivenActive(), which also
    // requires the device and the pipeline to have cooperated.
    u32 rtRenderMode_ = 0;
    // Whether the coat lobe is compiled into this process's material pipelines.
    //
    // LATCHED AT THE FIRST setSettings AND NEVER AGAIN, which is the design and not an accident:
    // this renderer builds twenty-odd raster PSOs at init, through DXC at runtime with no disk
    // cache, so honouring a later change would mean recompiling all of them mid-session. The value
    // is a project-level decision; changing it takes a project reload. See voxi::Settings.
    bool layeredBsdf_ = false;
    bool layeredBsdfLatched_ = false;
    // ONCE PER PROCESS, not once per frame. The mismatch this reports is a STANDING condition -- the
    // setting really does disagree with the compiled shaders for the rest of the session -- so the
    // warning it drives fired on EVERY applySettings call. Measured in an ordinary bounded run: ~30
    // copies of the same line, which is not a louder warning, it is a log nobody can read. Same
    // idiom as drawCapReported_ a few members down, for the same reason.
    bool layeredBsdfWarned_ = false;
    // Settings::ptBounces. Spent only while pathTracingWanted() -- see where cb_.ptBounceParams
    // is filled, which is the one place that decision is made.
    u32 ptBounces_ = 1;
    // Frames apart the GI volume is rebuilt. See setGiUpdateInterval / Settings for the contract; 1
    // rebuilds every frame. Read against rtFrameIndex_ in prePass() -- see that call site.
    u32 giUpdateInterval_ = 1;
    // Advances once per prePass() call, unconditionally -- a pure count of simulated frames, never
    // wall-clock -- so the ray-traced shadow's per-pixel trace schedule and disc-sample rotation are
    // a deterministic function of frame NUMBER. A fixed --frames count therefore always reaches the
    // same value at the same point in a run, which is what keeps that schedule reproducible for
    // testing even though it is no longer spatial-only the way rtHash alone is (see rtHash's own
    // comment in VoxiShaders.hpp).
    u32 rtFrameIndex_ = 0;

    // ---- the frame-period sampler ----
    //
    // WHAT IT MEASURES, stated because the number is easy to over-read: the wall-clock period
    // between successive prePass calls. That is the WHOLE frame -- editor UI, the voxelise pass, the
    // post chain -- and not Voxi's share of it, and it is a CPU period, so it only tracks GPU cost
    // while the GPU is the thing being waited on. Run it with --no-vsync or every reading is the
    // refresh interval.
    //
    // It exists because "the cost is linear in the ray count" was an assertion with no instrument
    // behind it. Changing exactly one thing and re-reading the same number is a measurement; a
    // profiler capture that cannot be checked into the repository is not.
    // The backdrop handle currently bound at t10, so a re-bind only happens when it moves.
    rhi::TextureHandle boundBackdrop_ = 0;
    u32  rdAblate_ = 0;            // --rd-ablate: AVER_RD_ABLATE for PSRayDriven, 0 = normal
    bool frameTimeReport_ = false;
    u64  frameTimeLastNs_ = 0;          // steady_clock, nanoseconds; 0 = no previous frame
    u32  frameTimeSeen_ = 0;            // frames sampled, including the discarded warm-up
    std::vector<f32> frameTimeMs_;      // one period per frame past the warm-up
    // Frames discarded before sampling starts. Pipeline creation, the first shader compiles and the
    // first uploads all land in the first handful of frames and are not what is being priced.
    static constexpr u32 kFrameTimeWarmup = 30;
    // Reports the collected periods, and clears nothing: the run's whole population is the sample.
    void reportFrameTime(const char* when);

    // ---- the flat geometry table a reflection ray reads after it hits something ----
    //
    // A hit gives back an instance id, a primitive index and barycentrics. Turning that into a
    // shaded colour needs the triangle, so every referenced mesh's vertices and indices are
    // concatenated into two buffers and a per-instance record says where each mesh starts. This is
    // the shape a non-bindless RHI can express: three descriptors total, not one per mesh.
    struct RtInstance {
        f32 objectToWorld[16];   // engine row-vector, matching TlasInstance::world
        u32 firstIndex = 0;      // where this mesh's indices start in the flat table
        u32 firstVertex = 0;     // and its vertices
        f32 albedo[3] = {1, 1, 1};
        // THE REST OF THE MATERIAL, or as much of it as a ray can reach. A hit used to shade as
        // pure chalk -- albedo through a Lambertian lobe -- because these two were sitting
        // unread in the same Draw the albedo is copied from. Metals in particular came out white
        // instead of dark, since a metal has no diffuse response at all.
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        // WAS "pad": an unread spare u32, now the one thing that actually closes the gap the
        // comment above used to end on. Dense index into THIS FRAME's rtMaterials_ (bound at t9,
        // gRtMaterials in VoxiShaders.hpp) -- see buildMaterialTable() for how it is assigned, why
        // a "dense" index needs no cross-frame identity, and why index 0 always means the material
        // system's fallback. Reading it gets a hit real reflectance/f90/flags/emissive instead of
        // the three hardcoded/defaulted values a ray used to be stuck with -- see this file's own
        // buildAccelerationStructures for the authored-vs-synthesized split that fills it.
        //
        // TEXTURE SAMPLING IS STILL ABSENT, and remains the separate problem the old comment named:
        // a hit needs every material's textures reachable from one shader, and doing that needs an
        // RHI addition (a Texture2DArray or equivalent), not another field here. materialIndex only
        // buys the FACTORS a material's constant block carries, not the maps it names.
        //
        // THIS FIELD IS A HAND-MAINTAINED ABI, same as every other field in this struct: HLSL packs
        // a structured-buffer element by POSITION, not name, so VoxiShaders.hpp's own RtInstance
        // mirror must declare a field in this EXACT position with this EXACT name for the rename to
        // be silent and safe on both sides. A position mismatch (added/removed/reordered on only
        // one side) shifts every field after it and corrupts every ray hit with no compile error on
        // either side -- nothing here verifies the two declarations agree beyond the byte-count
        // static_assert below.
        u32 materialIndex = 0;
    };
    // 64 + 4 + 4 + 12 + 4 + 4 + 4. A structured buffer packs tightly with natural alignment, so
    // this is the same 96 bytes on both sides -- and the stride handed to setSrvBuffer must agree
    // with it or every instance after the first reads the middle of its neighbour.
    //
    // THREE PLACES HAVE TO AGREE, and the assert only guards two of them: this struct, the HLSL
    // RtInstance in VoxiShaders.hpp, and the stride. The stride is the one that fails SILENTLY --
    // no compile error, just every instance past the first reading its neighbour's bytes, which
    // shows up as reflections and ray hits shading with the wrong surface's colour.
    static_assert(sizeof(RtInstance) == 96, "RtInstance is the HLSL RtInstance ABI");

    rhi::BufferHandle rtVerts_ = 0, rtIndices_ = 0;
    u32  rtVertCapacity_ = 0, rtIndexCapacity_ = 0, rtInstanceCapacity_ = 0;

    // THE INSTANCE TABLE IS A RING, not one buffer. It lives on the UPLOAD heap and is rewritten
    // every frame from the CPU, while the GPU is still reading the previous frame's copy of it out
    // of the same memory -- writeBuffer is a memcpy into a persistently mapped allocation, not a
    // queued copy, so nothing serialises the two. The symptom is a reflection sampling a transform
    // that belongs to the frame being built rather than the one being drawn: instances smeared
    // between two positions during camera motion, and only while ray tracing is on.
    //
    // One buffer per frame in flight breaks the overlap. Three, matching PcgVolume's readback
    // window, so this does not have to be re-derived if the device ever triple buffers.
    static constexpr u32 kRtInstanceRing = 3;

    // How many DISTINCT textures a ray hit can sample from. Fixed rather than grown on demand: the
    // range is baked into every root signature that declares it, so growing it means rebuilding
    // pipelines mid-session, which this renderer only ever does at a safe point anyway.
    //
    // 4096 because it is generous against real projects (ElectricDreams resolves tens) while
    // costing 4096 of the shared 65536-descriptor heap -- 6% of it, for the one feature that cannot
    // work without a large contiguous range. Exhaustion refuses and logs; it does not wrap.
    static constexpr u32 kRtTextureCapacity = 4096;
    rhi::BufferHandle rtInstances_[kRtInstanceRing] = {};
    u32               rtInstanceSlot_ = 0;
    // What the table was built from. Rebuilt only when this changes, because concatenating every
    // mesh every frame would cost more than the reflections do.
    u64  rtGeometryKey_ = 0;
    bool rtGeometryReady_ = false;
    std::vector<RtInstance> rtInstanceData_;
    std::vector<rhi::MeshHandle> rtInstanceMesh_;   // parallel: which mesh each instance draws
    // THE DISTINCT MESHES those instances name, sorted, plus where each one's geometry sits in the
    // shared vertex/index table. One entry per MESH, not per instance -- see buildGeometryTable for
    // what that distinction was costing.
    std::vector<rhi::MeshHandle> rtGeomMeshes_;
    std::vector<u32> rtGeomFirstVertex_, rtGeomFirstIndex_;

    // Builds or refreshes the flat table for this frame's draw list. Returns false when it could
    // not be made, which is the signal to fall back to cone-traced reflections.
    bool buildGeometryTable(rhi::IRenderContext& ctx);
    bool rtLogged_ = false;

    // ---- the dense per-frame material table a ray hit indexes into (t9, gRtMaterials) ----
    //
    // WHY A DENSE INDEX ASSIGNED FRESH EVERY BUILD, RATHER THAN A STABLE ONE KEPT ACROSS FRAMES:
    // nothing in pbr::MaterialSystem hands one out. MaterialLibrary's own dense enumeration "shift[s]
    // on destroy" (Material.hpp) and MaterialSystem keeps its GPU state in a handle-keyed
    // unordered_map sized to "tens, not thousands" of resident materials (MaterialSystem.hpp), not a
    // small dense array a ray could index directly. So this class assigns its OWN index, from
    // scratch, out of THIS build's draw list -- the identical shape rtGeomMeshes_ already uses for
    // the flat geometry table, and safe for the identical reason: an index is only ever read
    // together with the SAME build's rtMaterials_ upload, so material 3 meaning something different
    // next build is not a bug, it is simply never compared against next build's answer.
    //
    // rhi::BufferHandle rtMaterials_[kRtInstanceRing]: an UPLOAD-heap RING, the same shape and the
    // same reason as rtInstances_ above -- writeBuffer is "IMMEDIATE and unsynchronised"
    // (RHIResources.hpp), so overwriting the slot a still-in-flight frame's rays might be reading
    // is a hazard every time it happens, not only when it happens every frame. rtInstances_ needs
    // the ring on EVERY build because its content (transforms) changes every build; this buffer
    // needs the identical ring for the SAME reason on the rare build where content changes at all --
    // "rare" bought no exemption from the race, only a lower chance of hitting it by accident, which
    // is precisely the kind of bug that survives testing and ships.
    // ---- the bindless texture table a ray hit samples through ----
    //
    // ONE TABLE FOR THE WHOLE SCENE, append-only for the life of the device. A material's textures
    // are resolved once by MaterialSystem and live until shutdown, so an index handed out here stays
    // valid; nothing frees a slot mid-session. That is a real limitation rather than a claim of
    // permanence -- a project that loaded more than kRtTextureCapacity DISTINCT textures would
    // exhaust it, and the refusal is logged and the affected slots fall back to their factor colour
    // rather than sampling a neighbour's texture.
    //
    // Keyed on rhi::TextureHandle, not on material: the same texture shared by forty materials
    // occupies one slot, which is the difference between a table sized for materials and one sized
    // for images.
    // Makes one texture resident in the table below; see the .cpp for the append-only rule.
    u32  residentTexture(rhi::TextureHandle h);
    // Creates that table on first need, once.
    void ensureTextureTable();

    // The TEXTURED ray-driven pipeline. Separate from rayDrivenPso_ because the bindless range is
    // part of the root signature: preferred when it exists, and rayDrivenPso_ is the fallback.
    rhi::PipelineHandle rayDrivenTexPso_ = 0;
    // ITS G-BUFFER TWIN, and the reason it exists is a measurement trap rather than a feature.
    //
    // scenePass() used to pick the textured pipeline only when the G-buffer was OFF:
    //     const bool textured = rayDrivenTexPso_ != 0 && !dev_->gBufferEnabled();
    // because there was no textured pipeline that also wrote the four targets, and pickGbuf() must
    // return a pair that agree about their root signature. The consequence was silent and severe:
    // passing --gbuffer did not merely add three render targets, it UNTEXTURED the renderer. Any A/B
    // taken across that flag was comparing a textured image against a flat-albedo one and attributing
    // the difference to the G-buffer. That flag sits directly on the path of every measurement the
    // deferred-lighting work needs, which is why this is being closed before that work starts rather
    // than after it produces a number somebody believes.
    //
    // Optional exactly like every other twin here: 0 when it did not compile, and the picker falls
    // back to the flat G-buffer pipeline rather than to an untextured non-G-buffer one.
    rhi::PipelineHandle rayDrivenTexGbufPso_ = 0;
    // The TEXTURED blended (glass) variant: PSMainVoxi compiled with the bindless table declared,
    // so a reflection seen IN a windowpane samples the reflected surface's texture. Preferred over
    // sceneRtBlendedPso_ whenever it built and the G-buffer is off.
    rhi::PipelineHandle sceneRtBlendedTexPso_ = 0;

    rhi::BindlessTableHandle rtTexTable_ = 0;
    std::unordered_map<rhi::TextureHandle, u32> rtTexIndex_;
    u32  rtTexNext_ = 0;
    bool rtTexTableTried_ = false;   // so a failed creation is attempted once, not every build
    bool rtTexLogged_ = false;

    rhi::BufferHandle rtMaterials_[kRtInstanceRing] = {};
    u32  rtMaterialSlot_ = 0;       // which ring slot is currently bound (last written, or still valid)
    u32  rtMaterialCapacity_ = 0;   // elements the ring's buffers were sized for
    bool rtMaterialsReady_ = false;
    bool rtMaterialLogged_ = false;   // the cost/re-upload-cadence report (buildMaterialTable), once
    // This build's dense table, already in FINAL (sorted, index-order) form, and a CPU-side
    // snapshot of whatever is actually sitting in rtMaterials_[rtMaterialSlot_] on the GPU right
    // now. Compared byte for byte every build (buildMaterialTable) so a scene whose materials have
    // not changed costs one memcmp instead of a re-upload -- there is no revision counter anywhere
    // upstream to trust instead (see buildMaterialTable's own comment), so exact content comparison
    // IS this class's revision signal, not an approximation of one.
    std::vector<pbr::MaterialConstants> rtMaterialData_;
    std::vector<pbr::MaterialConstants> rtMaterialUploaded_;
    // Per rtInstanceData_ element, in the SAME order (both grow in lockstep in
    // buildAccelerationStructures' own per-draw loop): the key buildMaterialTable() sorts and
    // resolves into a final dense index once this build's whole draw list has been seen. Consumed
    // and cleared by buildMaterialTable() at the end of the same build that filled it.
    std::vector<u64> rtInstanceMatKey_;

    // Second pass over this build's draws, run once buildAccelerationStructures' own per-draw loop
    // (and rtInstanceMatKey_ with it) is complete: sorts the distinct material keys that loop
    // recorded, builds rtMaterialData_ in that order (index 0 always the material system's fallback
    // material -- the defined sentinel a draw whose material could not be resolved gets, never a
    // stale or uninitialised row), re-uploads only when the content actually differs from what the
    // GPU already holds, and writes the final index into every rtInstanceData_[i].materialIndex.
    // Returns false only when the GPU buffer itself could not be (re)created; rtInstanceData_'s
    // materialIndex values are still correct dense indices into rtMaterialData_ either way, they
    // may simply not have reached the GPU.
    //
    // `matConstantsByKey` is the resolved bytes for every distinct key rtInstanceMatKey_ names,
    // built by the SAME per-draw loop -- passed in rather than kept as a member because nothing
    // outside one build needs it, and a build that exits early should not leave a stale generation
    // sitting on the object for the next build to find half-populated.
    bool buildMaterialTable(const std::unordered_map<u64, pbr::MaterialConstants>& matConstantsByKey);

    // ---- previous-frame per-instance transforms: tracked here; NOT YET reachable by any shader ----
    //
    // WHAT THIS IS FOR. averGBufferVelocity() (VoxiShaders.hpp) computes motion by reprojecting ONE
    // world position through this frame's camera and last frame's -- correct only for a surface that
    // did not move, which is that function's own stated, deliberate gap. Fixing it for a MOVING
    // instance needs that instance's own previous object-to-world, not the camera's. This is the
    // bookkeeping half of that fix: a map from "which instance" to "what world matrix it carried last
    // time", rebuilt every buildAccelerationStructures() call.
    //
    // THE KEY. Two identities already exist in this class and neither survives contact with this
    // problem unchanged:
    //   - giDrawsKey() (above) folds the WHOLE draw list into one u64 -- exactly right for "did
    //     anything change", useless for "which draw is THIS one".
    //   - the RT instance table's own instanceId (buildAccelerationStructures, `i.instanceId = ...
    //     rtInstanceData_.size() ...`) is this frame's POSITION in the post-BLAS-filter replay of
    //     drawsPrev_. Position is not identity: insert or drop one draw anywhere earlier in the list
    //     and every instanceId after it names a different physical instance next frame, for a reason
    //     that has nothing to do with the instance sitting at that slot.
    // MESH HANDLE ALONE fails for the reason the task this was built from states plainly: a streamed
    // scene draws the same mesh hundreds of times (instanced foliage, repeated props), so every one
    // of those instances would collide on a single key.
    //
    // SO: group by (mesh handle, drawBinding) -- drawBinding is the surface's material descriptor
    // table, and this RHI is not bindless (see RtInstance's own comment on why texturing a ray hit
    // needs a second table at all), so drawBinding is shared per MATERIAL rather than allocated per
    // instance. Two draws sharing both a mesh AND a material are precisely the ones a scene actually
    // submits many of -- and within one group, disambiguate by an ORDINAL: the Nth draw carrying that
    // (mesh, drawBinding) pair this build, in submission order.
    //
    // THE POPULATION GATE is what keeps a wrong match rarer than an absent one, which the task this
    // was scoped from is explicit about wanting: an ordinal is only TRUSTED -- looked up against last
    // build's map at all -- when the group's total population this build is IDENTICAL to what it was
    // last build. If one instance in a group of otherwise-identical trees appeared or vanished, every
    // OTHER tree's ordinal in that group may have silently shifted (submission order is whatever the
    // caller's own iteration produced; this class has no way to ask whether it stayed stable across a
    // change to the set), so a population change drops trust for the WHOLE GROUP for this one build
    // rather than risk handing ordinal 2's old transform to whatever now occupies ordinal 2. The
    // group resynchronises on its own the very next build the population holds steady -- every build
    // that is not the exact one adding or removing that combination.
    //
    // THE TWO HONEST FALLBACKS THE TASK ASKS FOR, and why they are the SAME CODE PATH rather than two:
    // an instance that did not exist last build (a brand-new key, or one whose group was just gated
    // off) and an instance that was merely SKIPPED for a build (dropped by the kMaxDraws cap, or
    // briefly absent from whatever feeds submitDraw) are indistinguishable from here -- both simply
    // have no entry in last build's map. Both get the same answer: this build's OWN current transform
    // reported back as "previous", i.e. zero velocity, never a stale or interpolated guess.
    //
    // WHERE THIS DELIBERATELY STOPS. rtInstancePrevWorld_ below is computed, measured (see the
    // AVER_INFO this class logs once, sized from the ACTUAL instance count rather than assumed), and
    // never bound to a descriptor. Making it reachable by a shader needs either widening RtInstance --
    // a 96-byte struct whose HLSL mirror and byte stride (VoxiShaders.hpp; this file's own
    // setSrvBuffer call in buildGeometryTable) would have to change together, or every ray-driven hit
    // and reflection ray reads a neighbour's bytes, silently (see RtInstance's own "THREE PLACES HAVE
    // TO AGREE" comment) -- or widening table 0's declared slot count (VoxiGiShaders.hpp's
    // kGiSrvCount/giTableKinds), which needs a matching HLSL declaration too. Both files belong to the
    // agent doing this feature's shader half, editing concurrently with this one, and that agent's own
    // landed code already answers the question: RtInstance is UNCHANGED at 96 bytes, and
    // averGBufferVelocity() (VoxiShaders.hpp) reprojects `wpos` through gViewProj/gPrevViewProj alone
    // -- camera-relative, correct for a motionless surface, silently zero for one that moved, exactly
    // the gap this comment opened with. Reaching in from THIS file to widen a struct the other agent
    // has deliberately left alone, with no way to confirm a matching HLSL edit lands in the SAME
    // build, risks corrupting the stride every ray-driven pixel and reflection ray reads TODAY -- a
    // regression in the shipped default path (ray-driven primary visibility, Settings::rtRenderMode =
    // 1), not a missing improvement to a new one. A wrong stride is worse than a right one arriving a
    // build later, the identical reasoning the population gate above already applies one level down.
    // So: dynamic objects do NOT get true motion vectors in this slice, from either the raster or the
    // ray-driven pipeline -- both compute velocity the STATIC way, per averGBufferVelocity's own
    // comment. This tracker is the ready-to-connect other half; wiring it is the next build's job,
    // gated on that coordinated ABI change landing in VoxiShaders.hpp.
    //
    // OFF UNTIL SOMETHING READS IT, and this is the switch the comment above means by "wiring it is
    // the next build's job". Every map operation below runs once per draw per frame to produce
    // rtInstancePrevWorld_, which -- as that comment states plainly -- is never bound to a
    // descriptor. A repo-wide grep agrees: the only read of the vector is `.size()` in the one-time
    // memory log. So the whole tracker is, today, work whose sole output is discarded.
    //
    // A CONSTANT RATHER THAN A DELETION, deliberately. This is not dead code that nobody meant; it is
    // a carefully reasoned half of a feature whose other half needs a coordinated RtInstance ABI
    // change that another agent owns. Deleting it would throw away the population gate, the ordinal
    // scheme and the two-map swap, all of which are correct and all of which would have to be
    // rediscovered. Flipping this to true is step one of finishing the job; until then the compiler
    // removes the cost entirely.
    //
    // MEASURE BEFORE BELIEVING IT MATTERS: on PTTest (20 entities) the frame is GPU-bound with the
    // CPU scene walk at 0.0 ms, so this buys nothing there. It is a per-draw cost, so what it is
    // worth scales with the draw count, not with this scene.
    static constexpr bool kTrackPrevTransforms = false;

    // Groups by (mesh, drawBinding). FNV-1a, matching giDrawsKey()/buildGeometryTable's own mixing
    // constants so a reader who already knows those two recognises the recipe rather than learning a
    // third one.
    u64 prevTransformGroupKey(rhi::MeshHandle mesh, rhi::BindingSetHandle matSet) const;

    // This build's per-group population (mesh+drawBinding -> instance count), and the SAME map from
    // the previous successful build -- compared in buildAccelerationStructures to decide whether a
    // group's ordinals are trustworthy this time. Two separate maps, swapped (never merged) at the
    // end of a build that reaches its normal exit, so a group this build's draw list does not name at
    // all is simply absent from "this build" and therefore mismatches "last build" the moment it
    // returns -- exactly the signal that group's stale transforms must not be trusted if it reappears.
    std::unordered_map<u64, u32> prevGroupCountThisBuild_;
    std::unordered_map<u64, u32> prevGroupCountLastBuild_;
    // This build's running per-group ordinal counter. Cleared at the START of every build -- this is
    // a counter still climbing while a build is in progress, not the FINAL population count the two
    // maps above hold, and conflating the two would make every instance but the group's last look
    // like a population change against itself.
    std::unordered_map<u64, u32> prevGroupOrdinal_;
    // (mesh, drawBinding, ordinal) -> the world transform that exact instance carried last build, and
    // this build's own answer being assembled to replace it. Swapped whole at the end of a build,
    // never merged in place, for the identical reason the population maps above are swapped rather
    // than merged: a key this build's draw list does not name is dropped rather than surviving
    // indefinitely so a later, unrelated instance that happens to reuse the key cannot inherit it.
    std::unordered_map<u64, std::array<f32, 16>> prevTransformByKey_;
    std::unordered_map<u64, std::array<f32, 16>> nextTransformByKey_;
    // This build's answer in rtInstanceData_'s OWN index order: element i is the previous-frame
    // transform for the exact instance rtInstanceData_[i] describes (buildAccelerationStructures
    // pushes to both in the same loop, in lockstep), or that SAME instance's CURRENT transform when
    // no trustworthy previous one was found -- zero velocity, not a lookup failure standing in for
    // one by accident. Ready for a future RtInstance::prevObjectToWorld field to be filled from,
    // element for element, the day that coordinated ABI change lands.
    std::vector<std::array<f32, 16>> rtInstancePrevWorld_;
    // The memory-cost report (task item 3) is said once, sized from the REAL instance count of
    // whatever scene is actually loaded rather than a number copied from a task description that
    // will drift the moment someone changes kMaxDraws or the content streams differently.
    bool prevTransformMemoryLogged_ = false;

    // One replayed draw: its transform, its legacy colour parameters and its captured material.
    struct Draw {
        rhi::MeshHandle mesh;
        // What the DEPTH-ONLY passes draw instead: the four cascades, the GI shadow map and
        // voxelisation. Always valid -- it falls back to `mesh` -- so a pass can use it
        // unconditionally without asking whether a proxy existed.
        rhi::MeshHandle depthMesh = 0;
        f32 world[16];
        f32 color[4];
        f32 metallic, roughness;
        rhi::BindingSetHandle matSet = 0;
        u32 matBytes = 0;
        u8  mat[sizeof(pbr::MaterialConstants)] = {};
        // World-space bounding sphere, captured once at submit time from IDevice::meshBounds and
        // this draw's own world matrix. A NEGATIVE radius means the backend had no bounds to give --
        // the signal for a cull test to treat this draw as unknown and always submit it, rather than
        // mistake "no answer" for a real, radius-zero point.
        f32 boundsCentre[3] = {0, 0, 0};
        f32 boundsRadius = -1.0f;

        // TRANSLUCENT: in the TLAS, out of everything else.
        //
        // A draw carrying this reaches the ray-tracing acceleration structure marked non-opaque, so a
        // shadow ray can intercept it and attenuate rather than stop -- and stays OUT of the shadow
        // cascade, the GI shadow map and voxelisation, all three of which are binary depth-only
        // passes with no channel for a transmittance and no pixel shader to compute one in.
        //
        // A FLAG ON THE DRAW, NOT A SECOND LIST, because every one of those passes already walks
        // drawsPrev_ and a parallel list would mean four walk sites each needing to remember to visit
        // it. One flag tested in one place per pass cannot be forgotten by a fifth pass added later.
        bool translucent = false;

        // HIDDEN FROM ITS OWNER: in everything, out of the ray-driven primary ray alone.
        //
        // The peer of `translucent` above and the exact complement of it: that flag keeps a draw in
        // the TLAS and out of the depth-only passes; this one keeps a draw in every pass it already
        // reached -- shadow cascade, GI voxelisation, reflections, bounce rays -- and removes it
        // from ONE traversal, the primary visibility ray, because that ray begins inside this
        // mesh. See AVER_RT_MASK_OWNER_HIDDEN in voxi.hlsl for why the fix belongs in the instance
        // mask rather than in whether the instance exists.
        //
        // A FLAG ON THE DRAW, for the reason `translucent` gives directly above: the alternative is
        // a parallel list every pass has to remember to visit.
        bool hiddenFromOwner = false;
    };
    std::vector<Draw> draws_, drawsPrev_;

    // ---- the blended-draw census ----
    //
    // submitDraw() drops every `blended` draw before it ever reaches draws_ -- see that function's
    // own comment for the three things dropping it costs a translucent surface. That drop used to be
    // impossible to observe: `blended` did not exist as a parameter until the shading-contract change
    // that added it to IRenderFeature::submitDraw, so every call this renderer ever received was
    // opaque by construction and nothing was ever excluded. Now that translucent draws actually
    // arrive here, silently absorbing them would look identical to a scene that simply has no glass
    // in it -- which is indistinguishable from the bug this drop exists to prevent (a pane rendering
    // as an opaque black shadow, or blocking GI, because the filter was accidentally removed).
    //
    // COUNTS, NOT IMPRESSIONS -- same reasoning as giSkipped_/giRebuilt_ below: a running total across
    // the renderer's lifetime, not reset per frame, because the interesting question ("is this
    // filter still firing at all") is answered by the total ever growing, not by any one frame's rate.
    u64 blendedDropped_ = 0;
    // Widening-interval report counter, the identical shape voxelCullLogs_ uses in voxelizePass:
    // logs at counts 1, 3, 7, 15, ... so the first few drops (most likely to matter to someone
    // watching a scene load) are seen quickly and the log does not grow linearly with a level that
    // has thousands of glass panes in it.
    u32 blendedDropLogs_ = 0;

public:
    // A CHEAPER STAND-IN FOR THE DEPTH-ONLY PASSES. Every mesh this renderer received was drawn at
    // full resolution into all four cascades, the GI shadow map and the voxel grid -- six times a
    // frame -- while the lit pass beside them was already picking an LOD per instance. On a streamed
    // scene that is the largest single item in the frame and none of it is visible: a shadow does not
    // resolve the silhouette detail a coarser level drops.
    //
    // A FUNCTION POINTER RATHER THAN A DEPENDENCY, because the LOD ladder belongs to whoever loaded
    // the mesh, not here. This renderer must not learn what Trifactor is: it asks "have you got
    // anything cheaper for this handle?" and accepts 0 for no. Nobody installing a resolver is the
    // default, and it renders exactly as it did before.
    using DepthProxyFn = rhi::MeshHandle (*)(rhi::MeshHandle mesh, void* user);
    void setDepthProxy(DepthProxyFn fn, void* user) { depthProxyFn_ = fn; depthProxyUser_ = user; }

private:
    DepthProxyFn depthProxyFn_ = nullptr;
    void*        depthProxyUser_ = nullptr;

    // shadowPass() scratch: this cascade's culled draws, grouped by mesh, so every instance of one
    // mesh reaches the GPU in a single drawMeshInstanced() call rather than one drawMesh() each.
    // A member (not a cascade-local) so its buffers' capacity survives from cascade to cascade and
    // frame to frame instead of reallocating four times a frame -- see shadowPass()'s own comment
    // at the reset for why clearing `worlds` (not erasing the group) is what makes that stick.
    struct ShadowInstanceGroup { rhi::MeshHandle mesh = 0; std::vector<f32> worlds; };
    std::vector<ShadowInstanceGroup> shadowInstanceGroups_;
    // The same grouping for the GI-only pass. Its own vector rather than a shared scratch buffer:
    // giShadowPass and shadowPass run in the same frame and would otherwise stamp on each other.
    std::vector<ShadowInstanceGroup> giShadowInstanceGroups_;

    f32 center_[3] = {0, 0, 0};
    f32 extent_ = 2000.0f;

    // Mirrors `cbuffer VoxiFrame : register(b4)` field for field.
    struct FrameConstants {
        f32 voxelOrigin[4] = {};
        f32 voxelParams[4] = {};
        // One light view-projection per cascade, tightest first; world space to that cascade's clip box.
        f32 cascadeViewProj[4][16] = {};
        // x = radius at which the cascade stops being used, y = normal-offset bias in world units.
        f32 cascadeSplit[4][4] = {};
        // x = 1/atlas size, y = shadow map usable, z = acceleration structure built, w = cascade count
        f32 shadowParams[4] = {};
        // x = the cascade the depth-only shadow pass is currently filling
        f32 shadowDraw[4] = {};
        // x = tan of the sun's angular radius, y = occlusion rays per pixel, z = base ray bias,
        // w = 1 once the flat geometry table is ready for a reflection ray's hit lookup.
        f32 rtParams[4] = {};
        // x = 1 while rtShadowHist_'s slots are actually bound this frame -- the shader must not
        // touch t6/u2 when this is 0, since an unbound slot is Tier 1 null-filled, not a real
        // texture. y = 1 once THAT texture also holds a real previous frame (0 right after creation
        // or a resize, when it is bound but its contents are not a previous frame). z = the current
        // frame index (rtFrameIndex_), a pure per-frame count, never wall-clock. w = the pixels-
        // per-ray tile edge AS ITS BIT COUNT (0 = no tiling, every pixel traces every frame).
        f32 rtHistParams[4] = {};
        // The CAMERA's view-projection from the frame before this one, for reprojecting a pixel's
        // world position into last frame's ray-traced shadow history. Only meaningful while
        // rtHistParams.y is set.
        f32 prevViewProj[16] = {};
        // LAST frame's scene viewport rect (x, y, w, h in target pixels) -- see IDevice::sceneViewport.
        // The reprojected NDC lands in THIS rect, not at [0,1] of the whole history texture: the
        // editor docks the 3D view in a sub-rect of the backbuffer, same as prevViewProj above.
        f32 sceneViewport[4] = {};
        // THIS frame's scene viewport rect, in the same (x, y, w, h) target pixels.
        //
        // A SECOND COPY IS NOT REDUNDANT: sceneViewport above is the PREVIOUS frame's, because every
        // reader of it pairs it with prevViewProj to reproject into last frame's history. A reader
        // that projects with THIS frame's gViewProj -- refraction does -- needs this frame's rect,
        // and reusing the previous one silently mismatches for a frame after any viewport change.
        f32 sceneViewportCur[4] = {};
        // THE MEDIUM THE CAMERA IS CURRENTLY INSIDE. x = 1 when the eye is within a blended,
        // single-sided volume; y = that material's ior; z, w spare.
        //
        // Needed because a closed volume seen FROM WITHIN has no front faces at all -- every face
        // points away from the eye -- so the back-face discard that keeps a water box from
        // compositing four coats of alpha also deletes the surface entirely once you swim under it.
        // The shader inverts that discard rather than switching it off; see PSMainVoxi.
        f32 cameraMedium[4] = {};
        // THE WATER VOLUME THAT CASTS CAUSTICS, in world centimetres: min.xyz and max.xyz of its
        // axis-aligned box, with min.w = 1 when there is one at all and max.w its strength.
        //
        // The volume's TOP (max.z) is the surface light refracts through, and everything inside the
        // footprint and below that height is lit through it. Published from the renderer rather than
        // read from the fluids module because Voxi renders and plugins do not: what arrives here is
        // a box, and nothing about this says "water".
        f32 causticMin[4] = {};
        f32 causticMax[4] = {};
        // The GI-only shadow map's light view-projection, fitted to the GI volume rather than the
        // camera -- see fitGiShadow(). Read only by PSVoxel through giShadowFactor(); PSMainVoxi
        // keeps using cascadeViewProj/shadowFactor above for the camera cascades.
        f32 giShadowViewProj[16] = {};
        // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable at all (0 falls back to
        // fully-lit indirect), z = normal-offset bias in world units, w unused.
        f32 giShadowParams[4] = {};
        // The SPATIAL shadow denoiser -- mirrored as gRtDenoiseParams. x = filter radius in
        // pixels (0 = off), y = how much of the filtered value to take (0 discards it while
        // still paying for the taps, which is how the cost is measured before the filter is
        // trusted), z = how fast to taper the filter off as the gather centre's reprojection
        // velocity rises (0 = no taper, the behaviour before the knob existed), w unused.
        f32 rtDenoiseParams[4] = {};
        // Ray-driven bounce control -- mirrored as gRtBounceParams. x = how many bounces a ray
        // takes AFTER the first hit (1 is one bounce, which is what reflections already do);
        // y/z/w unused. Its own float4 rather than a spare slot in gRtDenoiseParams: a field
        // whose name says "denoise" carrying a bounce count is the kind of thing that reads
        // fine for a week and then costs an afternoon.
        f32 ptBounceParams[4] = {};
        // GI gather control -- mirrored as gGiParams. x = how many cones the diffuse gather
        // traces, total, including the axial one. y/z/w are REFRACTION, not spare:
        // y = Settings::refractionMode, z = refractionStrength, w = refractionEdgeFade, all
        // three written every frame in VoxiRenderer.cpp and read by averRefractedBackdropUV.
        // Its own float4 for the reason ptBounceParams above has one.
        f32 giParams[4] = {};
        // Ambient control -- mirrored as gAmbientParams. x = how many sky-visibility rays the
        // ambient term traces per pixel, 0 meaning "use the cone gather's own occlusion", which is
        // what every tier below the top does and what this renderer did before the rays existed.
        // y/z/w spare, and genuinely so TODAY -- see the giParams note above for what happens when
        // that sentence stops being true and nobody edits it.
        //
        // Its own float4, not a spare component of gRtDenoiseParams or gGiShadowParams, for the
        // reason ptBounceParams states: a field whose name says "denoise" carrying a ray count
        // reads fine for a week and then costs an afternoon.
        f32 ambientParams[4] = {};
        // EDITOR VIEW MODES that the ray-driven path has to honour itself. x = unlit (flat
        // authored albedo, no lighting); y/z/w spare.
        //
        // A PASS-LEVEL FIELD, not a per-draw one, because a ray hit has no per-draw cbuffer
        // to read: gShadingModel rides in the b1 block that the raster path sets per mesh,
        // and PSRayDriven never binds it. That asymmetry is the whole reason unlit reached
        // the rasteriser and not the renderer that actually draws the scene by default.
        f32 viewParams[4] = {};
    } cb_;

    // THE MIRROR THIS FILE HAS ALWAYS HAD AND NEVER GUARDED. `cbuffer VoxiFrame : register(b4)` in
    // modules/render.voxi/shaders/voxi.hlsl repeats every field above by hand, and nothing checked
    // that the two agreed -- the same unguarded-mirror bug already fixed for PathTracer's FrameCB
    // and PcgVolume's VolumeCB. VoxiFrame was simply the one that never got the assert. Appending
    // here without appending there reads garbage off the end of the block in every Voxi shader at
    // once, and INSERTING in the middle -- which is what adding sceneViewportCur beside its
    // previous-frame twin does -- shifts every field after it instead, which is worse: it is silent
    // and it is wrong everywhere rather than at the end.
    //
    // The file this used to name, VoxiShaders.hpp, no longer exists: the HLSL moved out of C++
    // string literals into shaders/ and the message was never updated. It sent me to a deleted file
    // when this assert did its job. Naming the real one now.
    static_assert(sizeof(FrameConstants) == 720,
                  "cbuffer VoxiFrame in modules/render.voxi/shaders/voxi.hlsl mirrors this byte for byte");
    static_assert(sizeof(FrameConstants) % 16 == 0, "must be a legal constant-buffer size");

    // ---- the GI rebuild gate: skip a revoxelisation whose result would be bit-identical ----
    //
    // giUpdateInterval AMORTISES the rebuild; it never removes one. At interval 4 a wholly static
    // scene still pays the full ~108 ms of voxelizePass+filterMips every fourth frame to compute
    // exactly what it computed last time, and at Epic (interval 1) it pays it every frame.
    //
    // This is the same trick buildGeometryTable already uses for the ray-tracing geometry table
    // (see its `key == rtGeometryKey_` early-out): hash what the result depends on, and if nothing
    // moved, keep the result. The volume texture is already resolved and mip-filtered and nothing
    // has touched it, so reusing it is not an approximation -- it is the identical answer.
    //
    // WHY THIS AND NOT A STATIC/DYNAMIC SPLIT OR AN ON-DISK CACHE. Both were designed and both were
    // rejected on correctness: PSVoxel bakes LIGHTING into each voxel (albedo * sun * visibility +
    // sky), not material, so a static voxel's stored radiance goes stale when a DYNAMIC occluder
    // moves through the sun's path over it -- which a static/dynamic split cannot see. An on-disk
    // cache additionally has no readback path in this RHI and a key that includes the sun, which is
    // a live editor slider. Gating the whole pass has neither problem: any change to the draw list,
    // the sun, or the volume placement changes the comparison and falls straight through to the
    // existing, already-correct full rebuild. It can only ever skip work whose output is identical.
    //
    // WHAT IT DOES NOT BUY: a camera streaming new chunks, or someone dragging the time-of-day
    // slider, changes the inputs every tick and gets exactly today's behaviour. The win is real
    // only while the scene is actually still, which for an editor is most of the time.
    bool giSnapshotUnchanged() const;
    void takeGiSnapshot();
    u64 giDrawsKey() const;

    u64 giDrawsKey_ = 0;
    // WHICH PART OF THE DRAW LIST MOVED. giDrawsKey_ alone says only "different", and knowing that
    // was not enough twice over: making the hash order-independent was expected to stop the gate
    // rejecting under camera rotation and changed the skip rate by two points. These split the same
    // inputs into independent axes so the next answer is measured rather than guessed.
    u64 giDrawsCount_ = 0;     // how many draws were hashed
    u64 giDrawsMeshKey_ = 0;   // mesh handles only, order-independent
    u64 giDrawsWorldKey_ = 0;  // world transforms only
    u64 giDrawsMatKey_ = 0;    // colour/metallic/roughness only
    mutable u64 giDrawsRejects_ = 0;
    mutable u64 giDrawsCountMoved_ = 0, giDrawsMeshMoved_ = 0, giDrawsWorldMoved_ = 0, giDrawsMatMoved_ = 0;
    mutable u64 giDrawsNextReport_ = 32;
    std::vector<rhi::MeshHandle> giSnapMeshes_;   // sorted mesh multiset at the last bake
    mutable u32 giDrawsDiffReports_ = 0;
    void giDrawsSubKeys(u64& count, u64& mesh, u64& world, u64& mat) const;
    rhi::SkyAtmosphere giSky_{};
    f32 giSnapCenter_[3] = {};
    f32 giSnapExtent_ = -1.0f;   // negative = no snapshot yet, so the first tick always rebuilds
    bool giSnapValid_ = false;
    u64 giSkipped_ = 0, giRebuilt_ = 0;   // for the one-time report; counts, not impressions
    mutable u32  giGateWhyMask_ = 0;   // one bit per rejection reason already reported
    u32          voxelCullLogs_ = 0;   // voxelize passes so far; the cull ratio reports at 2^n of them
    bool         drawCapReported_ = false;   // the draw-list-full warning is worth saying once, not every frame
    u64  giGateNextReport_ = 64;   // doubles each time, so the steady state gets reported too
    u64  giGateLastTicks_ = 0, giGateLastSkipped_ = 0;

    // ---- the GI derived-data cache -------------------------------------------------------------
    //
    // WHAT IT ADDS TO THE GATE ABOVE. giSnapshotUnchanged already avoids ~93% of rebuilds within a
    // run; what it cannot do is remember anything across one. So every level load pays a full
    // revoxelisation before the first lit frame, and so does every return to a level already looked
    // at. This is that memory: the resolved volume, written beside the project, keyed by exactly the
    // inputs the gate keys on.
    //
    // THE KEY IS THE GATE'S KEY, not a second one. giCacheKey() reads giDrawsKey_/giSky_/centre/
    // extent -- the same fields giSnapshotUnchanged compares -- so a cache hit and a gate hit mean
    // the same thing by construction rather than by two implementations agreeing.
    std::string giCacheDir_;
    // Tried once per key, hit or miss: a miss must not re-read the same absent file every rebuild.
    //
    // THE WHOLE KEY, and it used to be giCacheKey().drawsKey -- one sixth of it. giCacheFileName
    // hashes ALL SIX fields, so a memo keyed on the draw list alone said "already tried" about a
    // DIFFERENT FILE than the one it had tried. Once a draw set had been looked up, no other volume
    // for those same draws was ever read again for the rest of the run.
    //
    // That is the common case rather than a corner. giSnapshotUnchanged rejects on a changed volume
    // CENTRE (reject 2), which is exactly what a camera moving through a STATIC level does: the
    // rebuild fires, giCacheRestore is called, and it returned false without opening anything --
    // revoxelising from scratch a volume this same session may have written minutes earlier. The
    // cache was doing its job on the first bake of a level and silently nothing after it.
    //
    // Keying on the whole key restores the property stated directly above: the memo is about a FILE,
    // and the file name is the whole key.
    fmt::GiCacheKey giCacheTriedKey_{};
    bool giCacheTried_ = false;
    // READBACK IS NOT IMMEDIATE. The copy is a GPU command; its results are only there once the GPU
    // has passed it. So a bake schedules the copy, waits kGiCacheReadbackDelay frames -- longer than
    // the deepest frame-in-flight -- and only then reads and writes the file.
    static constexpr u32 kGiCacheReadbackDelay = 4;
    rhi::BufferHandle giCacheReadback_ = 0;
    rhi::BufferHandle giCacheUpload_ = 0;
    u64  giCacheBufBytes_ = 0;
    u32  giCacheDumpCountdown_ = 0;   // 0 = nothing pending
    fmt::GiCacheKey giCachePendingKey_{};

    // ---- the write-behind buffer -------------------------------------------------------------
    //
    // BAKES LAND IN RAM AND GO TO DISK IN BATCHES. Every completed bake used to be an ~18 MiB file
    // write on the frame it finished, and an author nudging the sun produces a bake per nudge --
    // so a minute of lighting work was tens of writes and a directory that had to be swept after
    // each one. The volumes now accumulate here and reach the filesystem when the budget is
    // exceeded or the editor shuts down.
    //
    // SAFE BECAUSE OF WHAT THIS CACHE IS, not because the window is small. GiCache.hpp states the
    // contract plainly: "nothing here is authored and nothing here is precious: the whole directory
    // can be deleted at any time and the only cost is one rebuild." Losing the buffer to a crash
    // costs exactly one revoxelisation, which is the same thing a cold cache costs.
    std::vector<fmt::GiCacheEntry> giCachePendingEntries_;
    u64 giCachePendingBytes_ = 0;
    // Default 256 MiB: about fourteen entries at the ~18 MiB a 128^3 volume takes, comfortably more
    // than a lighting session produces, and small enough to be unremarkable next to the volume
    // textures themselves. Editor Preferences > Derived Data Cache moves it.
    u64 giCacheRamBudget_ = 256ull * 1024ull * 1024ull;
    // True when the rebuild about to run was triggered by NOTHING but the cloud clock. The gate
    // tests clouds last precisely so this can mean that, and giCacheScheduleDump uses it to
    // decline writing a volume whose key cannot describe what changed. Cleared at the top of
    // every gate evaluation.
    // mutable because giSnapshotUnchanged is const and should stay that way: it ANSWERS a question
    // about the world rather than changing it, and this records which answer it gave -- the same
    // reason giGateWhyMask_ beside it is written from that const method.
    mutable bool giRebuildCloudOnly_ = false;

    // THE ONE DEFINITION OF "voxelizePass will inject this draw". giDrawsKey, giDrawsSubKeys and
    // voxelizePass all have to agree about this exactly -- both hash functions say so in as many
    // words -- and they agreed on two of the three tests while the third, the volume-bounds cull,
    // existed only in the pass. Three hand-copied predicates is how that happened; one is how it
    // stops happening.
    bool giVoxelisedDraw(const Draw& d) const;
    // Latched so the "this volume is bigger than the whole budget" warning is said once rather than
    // once per bake. Never cleared: raising the budget mid-session does not make the earlier
    // explanation wrong, and a second copy of it would only be noise.
    bool giCacheOversizeWarned_ = false;

public:
    // The write-behind budget, in bytes. Lowering it below what is already buffered flushes
    // immediately rather than leaving the buffer over its own limit until the next bake.
    void setGiCacheRamBudget(u64 bytes);
    u64  giCacheRamBudget() const { return giCacheRamBudget_; }
    // What is buffered but not yet written -- for the preferences page to show.
    u64  giCachePendingBytes() const { return giCachePendingBytes_; }
    u32  giCachePendingCount() const { return static_cast<u32>(giCachePendingEntries_.size()); }
    // Writes every buffered entry, sweeps the directory once, and empties the buffer. Returns how
    // many files were written. Safe to call with nothing pending.
    u32  giCacheFlush();
private:
    // Per-mip byte offsets inside the readback buffer, in the BACKEND's footprint layout.
    std::vector<u64> giCacheMipOffsets_;
    bool giCacheUnsupported_ = false;   // textureCopyFootprint said no; stop asking

    // The key describing the volume as it stands after takeGiSnapshot.
    fmt::GiCacheKey giCacheKey() const;
    // Tries to fill voxelTex_ from disk. True when the volume now holds the cached answer.
    bool giCacheRestore(rhi::IRenderContext& ctx);
    // Schedules a readback of voxelTex_ so it can be written out once the GPU is past it.
    void giCacheScheduleDump(rhi::IRenderContext& ctx);
    // Warns once that a volume is too big to cache. Shared by the two places that can decide it:
    // giCacheScheduleDump before the readback, giCacheTick as a guard after it.
    void giCacheWarnOversize(u64 bytes);
    // Ticks the countdown and writes the file when it reaches zero.
    void giCacheTick();
    // Sizes giCacheReadback_/giCacheUpload_ and giCacheMipOffsets_ for the current volume.
    bool giCacheEnsureBuffers();

    // Builds this frame's cascade matrices and splits. Returns the usable cascade count, 0 if none.
    u32 fitCascades();
    // Builds the GI-only shadow map's matrix, fitted to the GI volume. Writes cb_.giShadowViewProj/
    // giShadowParams and the cull sphere below.
    void fitGiShadow();
    // The GI-only map's own world-space bounding sphere, for giShadowPass's per-draw cull. Separate
    // from cascadeCentre_/cascadeRadius_ because after this change those are PURELY camera-fitted.
    f32 giShadowCentre_[3] = {};
    f32 giShadowRadius_ = 0.0f;
    // Per-cascade world-space bounding sphere, filled in by fitCascades and read by shadowPass to
    // skip a draw in a cascade its own bounds cannot reach. Sized like cb_.cascadeSplit above --
    // kShadowCascades is private to the .cpp, so this repeats its value (4) rather than reach for it.
    f32 cascadeCentre_[4][3] = {};
    f32 cascadeRadius_[4] = {};

    // ---- ray-traced temporal history: sun shadow AND reflections ----
    //
    // rtShadow() and rtReflection() each trace fresh rays per pixel per frame with no reuse across
    // frames -- see the module README's "Next" section and STATUS.md 4d item 4, which already flag
    // temporal accumulation as the missing piece. This blends each frame's fresh sample with a
    // REPROJECTED sample of the previous frame's result, screen-space, so a static or slowly-moving
    // result converges toward the old brute-force ray count over several frames instead of paying
    // for it every frame.
    //
    // PING-PONGED, not one texture: reprojection reads a DIFFERENT texel than the one this frame
    // writes, so reading and writing the same resource in the same frame would race between pixels.
    // The two effects have their OWN texture pairs (shape differs -- shadow is a scalar visibility,
    // reflection is an RGB colour) but share every piece of bookkeeping below: one write index, one
    // valid flag, one frame index, one tile schedule. They always resize, swap and go valid/invalid
    // in lockstep, since both are driven by the exact same "is RT active this frame" condition.
    //
    // Two full-screen RG32Float textures (x = visibility, y = linear depth) for the shadow, and two
    // full-screen RGBA16F textures (rgb = reflected colour, a = linear depth, or a NEGATIVE
    // sentinel meaning "this ray missed -- nothing here to reuse, see rtReflectionTemporal in
    // VoxiShaders.hpp") for reflections. In each pair, one texture is this frame's write target
    // (UAV), the other is last frame's result, read as this frame's history (SRV). The depth
    // channel is what lets a disocclusion be told apart from a reprojection that legitimately lands
    // on an already-populated texel.
    rhi::TextureHandle rtShadowHist_[2] = {0, 0};
    rhi::TextureHandle rtReflHist_[2] = {0, 0};
    u32  rtShadowHistW_ = 0, rtShadowHistH_ = 0;
    u32  rtHistWriteIdx_ = 0;
    // False right after creation or a resize: the textures hold no real previous frame yet, and
    // cb_.rtParams.w must say so rather than let the shader blend against garbage.
    bool rtHistValid_ = false;
    // This frame's camera view-projection, captured where fitCascades() already reads the camera,
    // and copied into prevViewProj_ at the end of prePass for NEXT frame's cb_.prevViewProj.
    f32  curViewProj_[16] = {};
    f32  prevViewProj_[16] = {};
    // Same idea, for the scene viewport rect the reprojected NDC needs (IDevice::sceneViewport) --
    // read fresh each frame in beginShadowHistory rather than piggy-backing on fitCascades, since it
    // has nothing to do with the shadow cascades fitCascades computes.
    f32  curSceneViewport_[4] = {};
    f32  prevSceneViewport_[4] = {};
    // (Re)creates BOTH rtShadowHist_ and rtReflHist_ at the given resolution if they do not already
    // match, and resets rtHistValid_ when it does -- the old contents belong to a resolution that
    // no longer exists. DESTROYS all four instead when rayTracingWanted() is false; see there.
    bool ensureShadowHistory(u32 width, u32 height);
    // The size onRenderTargetsChanged last asked for, kept because the histories are created and
    // destroyed on the ray-tracing on/off edge as well as on a resize -- and setSettings, which is
    // where that edge is seen, is not told a resolution.
    u32  rtHistWantW_ = 0, rtHistWantH_ = 0;
    // The pbr::MaterialGraphRegistry revision the scene pipelines were last compiled against. See
    // prePass, which rebuilds them when it moves. ~0 so the first frame after construction cannot
    // accidentally match a real revision.
    u64  scenePipelineGraphRev_ = ~0ull;
    // The shader-file revision these pipelines were compiled from. Same shape and the same reason as
    // scenePipelineGraphRev_ above: a PULL on a number, so a future caller cannot forget to push.
    // STAMPED WHERE THE PIPELINES ARE BUILT, not left at a sentinel. ~0 here meant the very first
    // prePass saw ~0 != 0 and rebuilt every scene pipeline once, on every launch, for nothing --
    // caught by reading a hot-reload log that said "shader files changed (revision 0)" before
    // anything had changed.
    u64  scenePipelineShaderRev_ = 0;
    // Whether anything will EVER write the ray-traced histories under the current settings. Four
    // textures at the SCENE render size (2x RG32Float + 2x RGBA16F, so 32 bytes per pixel between
    // them -- 144 MB at this machine's 2750x1639 scene view, 225 MB at a full 3532x1987) used to be
    // allocated whenever GI came up, with no reference to ray tracing at all. rayTracing defaults to
    // Quality::Off and a project that never writes a RENDER.RAYTRACING key never turns it on, so the
    // out-of-the-box configuration paid well over a hundred megabytes of VRAM for a feature switched
    // off -- and VRAM pressure severe enough to force eviction looks exactly like an unexplained
    // frame-rate drop, which is the complaint that led here.
    bool rayTracingWanted() const { return rtSupported_ && settings_.rayTracing != Quality::Off; }
    // PATH TRACING WANTED, which is a different question from ray tracing wanted and deliberately
    // asks the other setting. Both need the hardware -- a path tracer is built out of rays -- but
    // a project may want ray-traced shadows and no path tracing at all, which is the default.
    bool pathTracingWanted() const { return rtSupported_ && settings_.pathTracing != Quality::Off; }
    // Whether PSMainVoxi will actually run its ray-traced-history code path this frame, for EITHER
    // effect. False while RT is inactive or the debug view has taken over the scene -- in either
    // case nothing will write rtShadowHist_ or rtReflHist_, so nothing about either should be
    // touched this frame.
    // The DEBUG RAYMARCH has taken over the scene. Split out from suppressesScene() when a second
    // reason to suppress arrived -- see rayDrivenActive() directly below, and shadowHistoryActive()
    // for why conflating the two would have been a bug rather than a tidiness question.
    bool debugViewActive() const { return giReady_ && giEnabled() && debugView_; }
    // RAY-DRIVEN PRIMARY VISIBILITY is running this frame: the author asked for it, the device can
    // trace rays, and PSRayDriven compiled. Any of those failing falls back to the rasteriser,
    // which is a working image rather than a black one.
    bool rayDrivenActive() const { return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0; }

    // Whether the ray-traced history textures will be read and written this frame.
    //
    // TESTS debugViewActive(), NOT suppressesScene(), and the difference is load-bearing. Both
    // suppress the scene, but for opposite reasons: the debug raymarch replaces the shading
    // entirely, so nothing touches the history and preparing it would be waste. PSRayDriven calls
    // rtShadowTemporal exactly as PSMainVoxi does, so it needs the history prepared and bound --
    // asking suppressesScene() here would leave it sampling and writing resources this frame never
    // transitioned.
    bool shadowHistoryActive() const {
        return rtActive_ && rtShadowHist_[0] && rtShadowHist_[1] &&
               rtReflHist_[0] && rtReflHist_[1] && !debugViewActive();
    }
    // Swaps the read/write roles, transitions all four textures, rebinds them and sets
    // cb_.prevViewProj / rtHistParams for this frame. Called before shadowPass() so the UAVs are
    // writable and the constants are ready by the time the scene loop runs PSMainVoxi.
    void beginShadowHistory(rhi::IRenderContext& ctx);
    // Advances prevViewProj_ / rtHistWriteIdx_ / rtHistValid_ for NEXT frame, now that fitCascades()
    // (called from shadowPass(), after beginShadowHistory) has filled curViewProj_ with this frame's
    // camera. Called at the end of prePass. Shared by both histories -- see the member comment above
    // for why one write index serves both.
    void endShadowHistory();

    bool giEnabled() const { return settings_.globalIllumination != Quality::Off; }
    f32 sunDir_[3]   = {rhi::SkyAtmosphere{}.sunDirection[0],
                        rhi::SkyAtmosphere{}.sunDirection[1],
                        rhi::SkyAtmosphere{}.sunDirection[2]};
    // No sunColor_ and no ambient_ here on purpose. See setSunDirection.

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
    bool unlit_ = false;   // --unlit / the viewport view-mode dropdown; see setUnlit
    // See setConeTraceEnabled's own comment. Defaults to true, i.e. bit-identical to every build
    // before this toggle existed -- nobody who never calls the setter sees any difference at all.
    bool coneTraceEnabled_ = true;
};

} // namespace aver::voxi
