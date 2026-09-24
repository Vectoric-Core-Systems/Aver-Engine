// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/GiDispatchBounds.hpp"   // W3: VoxelBox/GiDispatchConstants -- see the .cpp for how
#include "aver/render/nrd/NrdRecorder.hpp"

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
    // (see D3D12Device::setSkyAtmosphere, which fills both). Accepting them here made this look like
    // a second place the sun
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
    // The ReSTIR-GI poison debug view (Part 2's non-finite guards, voxi_restir.hlsli): on, it paints
    // an unmistakable colour per guard wherever one fires this frame, instead of touching the scene
    // at all -- unlike setDebugView above, which REPLACES the whole scene with a raymarch, this rides
    // the existing PSMainVoxi/PSRayDriven pixel shaders and only overrides their own return value, so
    // it needs no separate suppressesScene()-style plumbing. Seven of its colours are painted only
    // inside giRestirIndirect (giMode==1); an eighth, violet (B1/F5), is painted directly in
    // PSMainVoxi/PSRayDriven for the ray-traced specular term's own ceiling hit and fires under either
    // giMode. See giRestirIndirect's own POISON DEBUG VIEW comment (and, for violet,
    // aver_IsGiRestirPoisonColour just above PSMainVoxi in voxi.hlsl) for the legend, and
    // gGiRestirParams.w (the repurposed cbuffer slot this forwards through, published unconditionally
    // now -- see beginShadowHistory's own F5 comment) for how it reaches the shader. Reachable by hand
    // via the console: `set voxi.giPoisonView true`.
    void setGiPoisonView(bool on);

    // U1/2.10 I: the ReSTIR-GI half-resolution VISIBILITY path-debug view, riding gAmbientParams.w
    // bit 64 (see beginShadowHistory's own packAmbientW call and voxi_restir.hlsli's path-view block
    // for the colour legend: yellow no ray, green reconstructed, blue half-res reconstruction, red
    // half-res fallback traced, white traced/Full). NOT the same view as setGiPoisonView above --
    // that one is suppressed while this is on, and vice versa; see giRestirIndirect's own comment for
    // the precedence. NO LOG, same shape as setGiPoisonView immediately above: SandboxApp reasserts
    // this from the console/--gi-vis-path-view slot every frame regardless of whether the user just
    // touched it, and a debug view is not the kind of change resetGiHistory's neighbours log either.
    // Reachable by hand via the console: `set voxi.giVisPathView true`.
    void setGiVisPathView(bool on);

    // W6/M5: LIVE PRICING SWITCH for the blended-history-write fix (see PSMainVoxi's own
    // gAverHistoryWrite gate, voxi.hlsl, and the optimisation-wave-2 plan's section 4). OFF (the
    // default) shades a blended (translucent) fragment's indirect diffuse through ReSTIR GI exactly
    // like an opaque one, paying its own share of the ReSTIR/shadow/reflection/AO history work that
    // W6's fix now lets it keep -- ON drops that fragment back to the voxel cone gather every other
    // tier already ships, the cheaper term this switch exists to price against. Carried as
    // gAmbientParams.w bit 16 (see beginShadowHistory's own packAmbientW call). REASSERT IDIOM, same
    // shape as setNrdLegacyCamera/setLightingLegacyBits above: guarded on an actual CHANGE, because
    // SandboxApp reasserts this from the console/--blended-gi slot every frame regardless of whether
    // the user just touched it. Reachable by hand via the console: `set voxi.blendedGiCone true`.
    void setBlendedGiCone(bool on);
    bool blendedGiCone() const { return blendedGiCone_; }

    // LIVE A/B SWITCH for the NRD camera-contract fix in beginShadowHistory (see that function's own
    // comment on its NRD block for the mechanism and the third_party citations). Default OFF, i.e.
    // bit-identical to the fixed behaviour for anyone who never touches this. ON reinstates the
    // OLD, WRONG pre-fix encoding -- identity worldToView, the combined viewProj for viewToClip,
    // read from LAST frame's curViewProj_/prevViewProj_ rather than this frame's fresh camera -- so
    // the user can compare the two by hand, on the live editor, with no rebuild. NEVER a setting to
    // leave on: it exists only so the fix's effect is provable, not because the old encoding is ever
    // preferable. Toggling it EITHER way resets NRD's history on the next frame (see the .cpp) --
    // the two encodings' notions of "the previous camera" are shaped differently, and blending one
    // denoiser history across the switch would silently mix them. Reachable by hand via the console:
    // `set voxi.nrdLegacyCamera true`.
    void setNrdLegacyCamera(bool on);

    // LIVE A/B SWITCHES for the lighting-contrast fix (contrast-fix plan, section 2's CONTRACT): a
    // u32 BITMASK, one bit per superseded piece of transport, forwarded byte-for-byte into
    // cb_.ambientParams[2] (gAmbientParams.z in voxi.hlsl/voxi_restir.hlsli/voxi_rt.hlsli/
    // voxi_cone.hlsli) EVERY FRAME WITH NO CONDITION -- see the .cpp for that write. The shaders
    // decode it inline, bit by bit, with no shared helper, so each one compiles on its own:
    //   bit 1  (R0)  the ReSTIR candidate/sky-occlusion rays sample a fixed 45-degree ring
    //   bit 2  (R1)  the receiver counts its own sky twice (traced AND through ambient)
    //   bit 4  (R2)  the ReSTIR candidate hit's own indirect sky has no visibility test
    //   bit 8  (R3)  a reused ReSTIR sample shades with no visibility test at all
    //   bit 16 (R6)  the cone gather is cosine-distributed AND cosine-weighted (an effective cos^2)
    //   bit 32 (M5)  a blended (translucent) fragment writes the shadow/reflection/AO histories and
    //                reads NRD's denoised output back the OLD, WRONG way -- see PSMainVoxi's
    //                gAverHistoryWrite gate (voxi.hlsl) and the optimisation-wave-2 plan's section 4.
    //                Unlike bits 1/4/8/16 above, this one also resets RT history (shadow/reflection/
    //                AO), not only GI and NRD -- see setLightingLegacyBits' own .cpp comment.
    // 0 (the default) means every fix in the plan is live; setting a bit REINSTATES that one piece
    // of the OLD, WRONG behaviour, for comparison only -- never a setting to leave on, the same
    // posture setNrdLegacyCamera above takes. See EditorConsole.hpp's five voxi.legacy* variables
    // (and --lighting-legacy, for a --frames capture with no console) for how a bit gets set, and
    // this method's own .cpp comment for which bits reset which cross-frame history.
    void setLightingLegacyBits(u32 bits);

    // ---- per-history reset commands, each a plain bool flip -- NO reallocation, ever ----
    // Driven from the console (resetgihistory/resetrthistory/resetaohistory/resetnrdhistory/
    // resetallhistory in EditorConsole.hpp, via voxi::Renderer's own request/consume flags -- see
    // Voxi.hpp's requestGiHistoryReset() and siblings) so the user can bisect which cross-frame GPU
    // history is carrying a burned-in artifact, one at a time, without a resize. Every one of these
    // is exactly what ensureShadowHistory already does to the same flag on a resize/RT-tier toggle --
    // see each method's own body for the precise citation -- just without the surrounding teardown/
    // rebuild, because none of these buffers needs one: the shader-side read gate treats the flag
    // going false as "no real previous frame" for one frame, and the WRITE side runs unconditionally
    // every frame regardless, so both ping-pong slices are clean again within two frames.
    // `quiet` skips the log line: voxi.debugResetHistoryEveryFrame calls these once a frame, where a
    // line per call would bury the log.
    void resetGiHistory(bool quiet = false);
    void resetRtHistory(bool quiet = false);
    void resetAoHistory();   // alias of resetRtHistory today -- see its own body for why
    void resetNrdHistory(bool quiet = false);

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

    // ---- M4/W3/W12: three independent measurement/optimisation dials, each off by default and each
    // reasserted every frame by the host (SandboxApp's --gi-force-rebuild/--gi-bounded-dispatch/
    // --gi-free-accumulator and their matching voxi.* console variables) -- so each setter below must
    // be cheap on a no-op call and log only on an actual change, the same "reassert idiom" every other
    // per-frame console dial in this class already follows. ----

    // M4: forces every GI tick past the snapshot gate, so a --gi-force-rebuild run measures a bake on
    // every single tick rather than the ~96-98% skip rate the gate normally achieves. ALSO bypasses
    // the on-disk GI cache in both directions (neither read nor written) -- see the .cpp's rebuild
    // branch for why a forced tick that hit the cache would measure a cache restore instead of the
    // bake it exists to time. Default false: bit-identical to today's gated, cached behaviour.
    void setGiForceRebuild(bool on);
    bool giForceRebuild() const { return giForceRebuild_; }
    // W3: bounds the clear/resolve/mip-filter dispatch to the box the draw list's own bounds say a
    // rebuild can possibly change, instead of the whole [0,res)^3 grid, on a rebuild that doesn't need
    // the full volume re-derived. See voxelizePass's own comment for the box selection and the
    // induction argument for why a voxel outside the box is already the right answer. Default false:
    // dispatch counts, barriers and the resulting image are bit-identical to today's full-grid
    // behaviour until this is turned on -- and even then the CENSUS half of W3 (how big the box WOULD
    // be) is measured every rebuild regardless of this flag, so its value can be judged before opting
    // in.
    void setGiBoundedDispatch(bool on);
    bool giBoundedDispatch() const { return giBoundedDispatch_; }
    // W12: frees the injection accumulator -- voxelResBuilt_^3*4 R32_UINT texels, 16 B/voxel, the
    // single largest GI resource that sits idle between bakes -- after the rebuild gate has gone
    // quiet (no rebuild, nothing converging) for kGiAccumulatorQuietTicks ticks in a row, and
    // recreates it the next time a rebuild actually needs one. Default false: the accumulator is
    // created once at startup and kept for the renderer's life, exactly as before this existed.
    //
    // THE ONE-TICK DELAY THIS BUYS. With the flag on, a rebuild that arrives while the accumulator is
    // freed does not run that same tick: it asks for the accumulator back (see the .cpp's gate) and
    // the rebuild actually happens on the NEXT tick, once manageInjectionAccumulator() has recreated
    // it. That is an image-timing difference -- one extra tick of staleness on the volume the very
    // first time a still scene starts moving again after being freed -- and it exists ONLY while this
    // flag is on; it is never a behaviour change for anyone who leaves it at the default.
    void setGiFreeAccumulator(bool on);
    bool giFreeAccumulator() const { return giFreeAccumulator_; }
    // M2(c): the CPU cost of buildAccelerationStructures' own per-draw loop over drawsPrev_ for the
    // last build that reached it -- population pass through the loop's closing brace, NOT the BLAS/
    // TLAS GPU recording around it, which already has its own GPU timestamp (rhi::ScopedGpuStat
    // "Voxi acceleration structures"). 0 until the first build that reaches the loop; an early return
    // (no ray tracing wanted, or an empty draw list) leaves whatever the previous build measured.
    f64 lastAccelBuildCpuMs() const { return lastAccelBuildCpuMs_; }

    // WHAT THE SHADERS WERE ACTUALLY COMPILED FOR, which is not the same question as what
    // Settings::layeredBsdf currently holds. The setting can be changed at any time; the pipelines
    // were built once, and this reports what they contain. The editor compares the two to decide
    // whether to say a reload is needed -- see the Shading model combo on the Rendering page.
    bool layeredBsdfActive() const { return layeredBsdf_; }

    // The edge length the GI volume was ACTUALLY built at, set once inside createVoxelVolume --
    // not Settings::voxelResolution, which can be edited at any time before the next reload picks
    // it up. The editor compares the two the same way it compares layeredBsdfActive() above, to
    // decide whether a resolution change needs a reload note.
    u32 voxelResolutionBuilt() const { return voxelResBuilt_; }

    // Turns on the frame-period report. See rtShadowRays_ for what it is for and what it is not.
    // MEASUREMENT ONLY -- see AVER_RD_ABLATE's own block in voxi.hlsl for what each value removes and
    // why a timestamp cannot answer this question. Must be set BEFORE init(), because it becomes a
    // shader define and the pipelines are compiled once there. Any non-zero value renders a
    // deliberately WRONG frame; it exists to be timed, never to be shipped or wired to a quality tier.
    void setRayDrivenAblation(u32 mode) { rdAblate_ = mode; }
    // --rt-denoise-motion: how fast the spatial shadow filter tapers off with the gather centre's
    // reprojection velocity. 0 = no taper, which is the shipped default. A MEASUREMENT DIAL, and it
    // needed this setter to actually be one: the field's own comment claimed it existed so the
    // filter's motion contribution could be isolated "at runtime instead of by rebuilding with a
    // line commented out", while nothing outside this class could set it. That is the same
    // knob-with-no-plumbing shape giSkyOcclusionRays and giSkyOcclusionTile were both caught in.
    void setRtDenoiseMotionTaper(f32 v) { rtDenoiseMotionTaper_ = v; }

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
    // W12: just the injection accumulator (voxelAccumTex_) at the given resolution -- the same desc,
    // debugName and error text createVoxelVolume has always used for it, factored out so
    // manageInjectionAccumulator() can recreate it after a free without duplicating either.
    bool createInjectionAccumulator(u32 resolution);
    // W12: recreates or frees the injection accumulator for this frame -- see the .cpp for the two
    // branches and prePass()'s own comment on why this must run before anything else this frame binds
    // bindings_.
    void manageInjectionAccumulator(rhi::IRenderContext& ctx);
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
    // STAGED RAY-DRIVEN PASSES (milestone 1): records CSRdVisibility, then CSRdShadow, then the
    // AVER_RD_SPLIT fullscreen draw, with a UAV barrier between each producer and its consumer --
    // see the .cpp for the exact GPU spans. Called from scenePass() ONLY once rdStagedActive() has
    // already said yes; every condition that decides whether to call this lives there, not here.
    void recordStagedRayDriven(rhi::IRenderContext& ctx);

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

    // ---- W10: buildAccelerationStructures' own per-build scratch, hoisted out of the function ----
    // USED TO BE TWO LOCALS -- `std::vector<rhi::TlasInstance> inst` and
    // `std::unordered_map<u64, pbr::MaterialConstants> matConstantsByKey` -- reallocated from empty
    // every single build, a std::vector and an unordered_map both paying a heap allocation, every
    // frame, for a container whose SHAPE (how many instances, how many distinct materials) barely
    // moves build to build even though its CONTENT does. .clear() at the top of
    // buildAccelerationStructures (right after the early-return, beside rtInstanceData_.clear() and
    // the rest of that build's per-frame state) keeps the underlying storage, so a steady-state scene
    // reuses the same allocation indefinitely; only a build whose instance/material count grows past
    // the previous high-water mark pays a reallocation, exactly like rebuiltThisFrame_ above already
    // does for the identical reason.
    std::vector<rhi::TlasInstance> tlasInstScratch_;
    std::unordered_map<u64, pbr::MaterialConstants> matConstantsScratch_;

    // ---- M2(c): what buildAccelerationStructures' own per-draw loop cost, CPU side ----
    // See lastAccelBuildCpuMs()'s own comment for the exact bracket this measures.
    f64 lastAccelBuildCpuMs_ = 0.0;
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
    // Settings::giMode, cached at setSettings like rtRenderMode_ above. Whether it is HONOURED is
    // giRestirWanted(), which also requires ray tracing to be wanted -- this field alone is just
    // "what was asked for", read by ensureShadowHistory to decide whether to (re)build
    // giReservoirs_/giSurfPosHist_/giSurfNrmHist_ and by prePass to fill cb_.giRestirParams.
    u32 giMode_ = 0;
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
    // ONE VERTEX SLICE PER VERTEX BUFFER, not per mesh: an LOD or a posed material part shares its
    // root's vertices under a handle of its own, and gets its root's slice rather than a copy.
    // Parallel to rtGeomMeshes_: non-zero where that mesh is the one whose copy fills the slice.
    std::vector<u8> rtGeomCopiesVerts_;
    std::unordered_map<u64, u32> rtGeomVertSlice_;   // (vb, vertex count) -> first vertex; scratch

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

    // ---- STAGED RAY-DRIVEN PASSES (milestone 1, voxi.rayDrivenStages) ----
    //
    // Splits the single PSRayDriven draw into three GPU passes: two compute dispatches that trace
    // the primary ray and the sun-shadow ray into the two resources below, then the SAME textured
    // ray-driven pixel shader recompiled to READ those records instead of tracing them itself. See
    // recordStagedRayDriven() for the recording order and rdStagedActive() for every condition that
    // has to hold before any of this runs -- the default (rayDrivenStages == 0) never looks at any
    // of these members at all, which is what keeps the single-pass path byte-for-byte unchanged.
    //
    // Compute twins of rdVisBuf_/rdSunVisTex_'s two producers. SAME layout (giLayout(kRtTextureCapacity))
    // as rayDrivenTexPso_/rayDrivenSplitTexPso_ below -- bindlessDefs and matDefs are identical, only
    // the entry point and the stage differ -- so the four staged pipelines share one root signature
    // (D3D12ResourceFactory::rootSignature dedupes on layout content, not on compute-vs-graphics).
    // 0 when CSRdVisibility/CSRdShadow (voxi.hlsl) failed to compile, exactly like every other
    // optional Voxi pipeline degrading in this file.
    rhi::PipelineHandle rdVisCsPso_    = 0;
    rhi::PipelineHandle rdShadowCsPso_ = 0;
    // Stage B: rayDrivenTexPso_/rayDrivenTexGbufPso_ recompiled with ";AVER_RD_SPLIT=1" appended to
    // their own defines -- same bindlessDefs/rdAblateDefs/render-target formats, so these are built
    // right beside their untextured twins rather than in a function of their own. 0 on a device that
    // built the textured pipeline but not this variant (a shader-only failure, since the layout is
    // identical) -- rdStagedActive() treats that as "staged unavailable", not a crash.
    rhi::PipelineHandle rayDrivenSplitTexPso_     = 0;
    rhi::PipelineHandle rayDrivenSplitTexGbufPso_ = 0;

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
    // NO LONGER A ONE-LINE INLINE ASSIGNMENT -- defined in the .cpp because a change here now has a
    // second job besides storing the new pointer/user pair. submit()'s per-mesh cache
    // (meshSubmitCache_ below) remembers depthProxyFn_'s ANSWER for a mesh, keyed only by the mesh
    // handle -- it has no field recording WHICH depthProxyFn_/depthProxyUser_ pair produced that
    // answer. Reassigning either argument without dropping every entry the cache holds would let the
    // very next submit() for an already-cached mesh keep answering with the OLD proxy function's
    // verdict, silently, for as long as that mesh stayed in the cache. See the .cpp definition for
    // the actual invalidation and meshSubmitCache_'s own comment for the cache this protects.
    void setDepthProxy(DepthProxyFn fn, void* user);

    // A/B MEASUREMENT TOGGLE for the memoisation meshSubmitCache_ implements below -- reachable the
    // same way coneTraceEnabled()/setConeTraceEnabled() above are, a runtime dial rather than a
    // quality setting. Default true: see meshSubmitCache_'s own comment for why this is pixel-neutral
    // BY CONSTRUCTION rather than merely believed to be. false sends submit() back to resolving both
    // memoised questions directly, every call, in the ORIGINAL order, with none of the caching
    // machinery touched -- so an A/B of this change comes from ONE binary, never a `-D` reconfigure
    // of two build trees (see aver-reconfigure-pollutes-build in the engine's own notes for why that
    // comparison would not be trustworthy here).
    void setMeshSubmitCacheEnabled(bool on) { meshSubmitCacheEnabled_ = on; }
    bool meshSubmitCacheEnabled() const { return meshSubmitCacheEnabled_; }

private:
    DepthProxyFn depthProxyFn_ = nullptr;
    void*        depthProxyUser_ = nullptr;

    // ---- submit()'s per-mesh memoisation (Phase C follow-up to a7ff716d/80730751) ----
    //
    // WHAT THIS REPLACES. submit() re-resolves two questions that are functions of `mesh` ALONE --
    // depthProxyFn_'s answer and dev_->meshBounds' local-space answer -- on every one of the (up to)
    // 16,000 calls it gets a frame, one per entity, INCLUDING every frustum-culled one, since the
    // walk routes culled entities here on purpose so shadows/GI/the TLAS never depend on what the
    // raster camera can see (submit()'s own header comment). A scene rarely has more than a few
    // hundred DISTINCT meshes, so almost every one of those 16,000 pairs of calls is asking a
    // question this renderer already knows the answer to.
    //
    // A MEMBER, NOT A LOCAL, AND THAT IS THE WHOLE DIFFERENCE FROM 80730751's IDENTICALLY-SHAPED
    // CACHE. GameRender.cpp's MeshLookupCacheSlot table is local to ONE drawWorld() call and is gone
    // at its own closing brace, so it can never be asked a question about a frame it did not run in
    // -- there is no "next call" for it to leak into. submit() has no enclosing call of its own to be
    // local TO: it runs once per ENTITY, so a cache scoped to submit() itself would be constructed
    // fresh, and therefore empty, on the very call it exists to speed up. The only scope wide enough
    // to span many submit() calls is this renderer's OWN lifetime -- which is what turns "when does
    // it get cleared" from a non-question (a local's closing brace, always) into a real design
    // decision. See beginScene()'s own comment (VoxiRenderer.cpp) for the answer this class settled
    // on, and setDepthProxy()'s own comment two members up for the other half of it.
    //
    // A FIXED, DIRECT-MAPPED TABLE, MIXED BEFORE MASKING -- THE SAME SHAPE 80730751 CHOSE, reused
    // rather than reinvented. A single "last mesh" slot was rejected there for scoring ~100% on a
    // synthetic scene of one mesh submitted 16,000 times in a row and close to 0% on an interleaved
    // one, which is the shape real content actually takes (GameRender.cpp's own
    // kMeshLookupCacheSlots comment carries the full argument, and it applies here without
    // modification: nothing about WHICH function is doing the caching changes how often two
    // different meshes get submitted back to back). A raw mesh id is masked only after mixMeshId()
    // (VoxiRenderer.cpp) re-mixes it first -- see that function's own comment for why this file's
    // reasoning for doing so is NOT the same as GameRender.cpp's, even though the shape is identical.
    //
    // 256 SLOTS, NOT 64 -- THE ONE DELIBERATE DEVIATION FROM 80730751's PRECEDENT, sized against a
    // different population. GameRender's 64 slots hold "a handful" of interleaved species -- camera-
    // relevant content a level author placed by eye. submit() is reached by this renderer's WHOLE
    // draw population every frame, culled entities included, and the very redundancy this cache
    // exists to remove is stated above in terms of "a few hundred distinct meshes". A few hundred
    // keys into 64 slots averages several meshes per slot before any actual collision is even
    // considered, which reopens exactly the "close to 0% on the target scene" failure 80730751
    // rejected a one-slot cache for, just at a different slot count. 256 keeps collisions rare at
    // that population while still costing nothing but kMeshSubmitCacheSlots small structs on this
    // object -- no allocation, the same "ZERO ALLOCATION" property 80730751's own table has.
    static constexpr u32 kMeshSubmitCacheSlots = 256;
    static_assert((kMeshSubmitCacheSlots & (kMeshSubmitCacheSlots - 1)) == 0,
                  "kMeshSubmitCacheSlots must be a power of two for '& (kMeshSubmitCacheSlots - 1)' "
                  "below to be equivalent to '% kMeshSubmitCacheSlots'");

    // ONE SLOT'S ANSWER, FOR ONE MESH HANDLE, TO BOTH of submit()'s per-mesh questions.
    //
    // `mesh == 0` MEANS EMPTY, NOT A SEPARATE FLAG: submit()'s own first line is
    // `if (mesh == 0) return;`, so no live entry is ever asked to remember an answer for handle 0,
    // and a default-constructed slot -- mesh == 0 -- already reads, correctly, as "never touched".
    //
    // CACHES depthProxyFn_'s RAW RETURN, NOT submit()'s OWN SUBSTITUTED ANSWER -- submit() still runs
    // its own "0 means no proxy, fall back to the mesh itself" substitution against the cached value
    // on every single call, exactly where that substitution always ran, so a cache hit reproduces
    // bit-for-bit what an uncached call would have computed for the SAME depthProxyFn_/
    // depthProxyUser_ pair. "The same pair" is guaranteed by setDepthProxy() dropping this whole
    // table the moment either argument actually changes -- see that method's own comment above.
    //
    // CACHES meshBounds' LOCAL-SPACE ANSWER ONLY -- the centre and radius IN THE MESH'S OWN SPACE,
    // before submit() transforms them through `world` into THIS INSTANCE's world-space sphere.
    // `world` differs per entity even when `mesh` does not, so the transformed, world-space result
    // stays recomputed on every call with no exception; caching THAT instead would hand every
    // instance of a mesh the FIRST instance's world-space bounds -- a correctness bug wearing this
    // cache's clothing, not a spelling of it. See submit()'s own comment at the transform for the
    // failure this was designed away from: entities vanishing from a shadow cascade at the wrong
    // moment, which reads like a culling tuning problem, not a caching one, for as long as nobody
    // suspects the cache.
    //
    // A FALSE VERDICT FROM meshBounds IS CACHED TOO, not left to fall through to a repeat lookup on
    // every later call for the same mesh: boundsResolved is set on EITHER outcome, and haveBounds
    // alone records which one it was. meshBounds' own contract (RHI.hpp) and every backend's
    // implementation (D3D12Device.cpp, VulkanDevice.cpp) answer purely from a mesh's own upload-time
    // state, so the same handle cannot flip from "no bounds" to "yes" without a re-upload -- which is
    // this table's own next-beginScene() clear's job to catch, not this struct's.
    struct MeshSubmitCacheSlot {
        rhi::MeshHandle mesh = 0;

        bool depthProxyResolved = false;
        rhi::MeshHandle depthProxyMesh = 0;

        bool boundsResolved = false;
        bool haveBounds = false;
        f32  localCentre[3] = {0.0f, 0.0f, 0.0f};
        f32  localRadius = 0.0f;
    };
    // A FIXED MEMBER ARRAY, NEVER RESIZED -- the "ZERO ALLOCATION" half of the precedent, paid once
    // as part of this object rather than per frame. Cleared WHOLE, not slot by slot, at the top of
    // every beginScene() (see that method's own comment in VoxiRenderer.cpp for why a per-frame clear
    // is the invalidation point this class settled on) and again inside setDepthProxy() the moment
    // either of its two arguments actually changes.
    std::array<MeshSubmitCacheSlot, kMeshSubmitCacheSlots> meshSubmitCache_{};

    // Finds mesh's slot in meshSubmitCache_ above, evicting a DIFFERENT mesh's leftover answers first
    // so a hit can never read a previous occupant's depth-proxy or bounds fields under the new key --
    // the identical hazard, and the identical fix, as GameRender.cpp's findMeshLookupSlot. Defined in
    // the .cpp beside submit(), its only caller.
    MeshSubmitCacheSlot& meshSubmitCacheSlot(rhi::MeshHandle mesh);

    // See setMeshSubmitCacheEnabled()'s own comment two members up for the contract; this is just the
    // storage for it.
    bool meshSubmitCacheEnabled_ = true;

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
        // velocity rises (0 = no taper, the behaviour before the knob existed);
        // w = 1 while the AMBIENT history pair is bound this frame. It needs its own bit rather
        // than riding gRtHistParams.x with the shadow and reflection pairs, because unlike those
        // two it is allocated only at the tiers that trace the sky-occlusion ray -- High and
        // Epic; see aoHistoryWanted().
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
        // y IS THE COHERENCE TILE EDGE the sky-occlusion rays share a direction across (1 = a fresh
        // direction per pixel, the identity). It was claimed spare here while already being read by
        // the shader -- which is precisely what the giParams note above warns happens to a row
        // described as free. z is now ALSO spent: setLightingLegacyBits' u32 legacy bitmask (see its
        // own comment above for the bit table), stored as a float and decoded back with a u32 cast in
        // every shader that reads gAmbientParams.z -- 0 means every lighting-contrast fix is live.
        //
        // w IS NOW ALSO SPENT -- U1/2.9's own bitmask, packed and decoded by givis::packAmbientW
        // (GiVisibility.hpp), the SAME numeric-cast idiom z above already uses (a plain
        // static_cast<f32> of the u32 word, decoded back with `(uint)gAmbientParams.w` in every
        // shader that reads it -- NOT a bit-reinterpretation). bits 0-1 (& 3u) are
        // Settings::giRestirVisibility (0 NoRay, 1 Reconstructed, 2 HalfResolution, 3 Full), clamped;
        // bit 4 (& 4u) says the half-resolution ReSTIR visibility pair (t16/u10, giVisHist_) is bound
        // this frame; bit 8 (& 8u) says t16 holds a real previous frame; bit 16 (& 16u) is W6/M5's
        // setBlendedGiCone -- a blended fragment's indirect diffuse takes the voxel cone gather
        // instead of ReSTIR; bit 32 (& 32u) says the backend replays translucent draws blended THIS
        // frame (D3D12 only -- see setBlendedGiCone's own header comment for why Vulkan never sets
        // it); bit 64 (& 64u) is setGiVisPathView's debug view; bits 7-11 (>> 7 & 31u) are
        // Settings::giRestirMovingAge (0..31, clamped) -- the moving-camera ReSTIR reservoir-age cap
        // that replaces the existing 30-frame maxReservoirAge while the camera moves (see that
        // field's own comment in Voxi.hpp for the fade this exists to remove and giRestirIndirect's
        // own decode block, voxi_restir.hlsli, for where the override is applied). Single writer:
        // beginShadowHistory, which publishes it twice -- once unconditionally near the top of the
        // function (histBound/histValid false, the same F5 reasoning giRestirParams.w's own comment
        // below gives for why giMode 0 needs a live value too) and again inside the giSurf block
        // once it actually knows whether the sixth pair bound and holds a valid previous frame.
        //
        // Its own float4, not a spare component of gRtDenoiseParams or gGiShadowParams, for the
        // reason ptBounceParams states: a field whose name says "denoise" carrying a ray count
        // reads fine for a week and then costs an afternoon.
        f32 ambientParams[4] = {};
        // EDITOR VIEW MODES that the ray-driven path has to honour itself. x = unlit (flat
        // authored albedo, no lighting).
        //
        // A PASS-LEVEL FIELD, not a per-draw one, because a ray hit has no per-draw cbuffer
        // to read: gShadingModel rides in the b1 block that the raster path sets per mesh,
        // and PSRayDriven never binds it. That asymmetry is the whole reason unlit reached
        // the rasteriser and not the renderer that actually draws the scene by default.
        //
        // y WAS SPARE; NOW Settings::giRadianceCeiling (mirrored as AVER_VOX_MAXRAD in voxi.hlsl /
        // voxi_gi.hlsli -- see that field's own comment in Voxi.hpp for the full story). 0 here reads
        // as "unset" and the shader macro falls back to 16.0, so a FrameConstants block nobody has
        // written yet (giFrameConstants() read before this renderer's first prePass -- SandboxApp.cpp
        // documents exactly that all-zero-block window for the cluster-GI binder) behaves exactly as
        // it always has. A repurposed bit, not a new field -- packing/size unchanged, same shape as
        // gGiRestirParams.w below.
        //
        // Z AND W ARE NOW ALSO SPENT, NOT SPARE ANY MORE: Settings::giRestirDepthThreshold /
        // giRestirNormalThreshold (Voxi.hpp) -- RTXDI's own reuse-similarity tolerances
        // (stparams.depthThreshold/normalThreshold, voxi_restir.hlsli), lifted here from a shader
        // literal so the still-open moving-camera ReSTIR GI fade bisection can sweep them without a
        // rebuild. See either field's own comment for that investigation and
        // Settings::giRestirSpatialSamples (packed into gAmbientParams.w instead) for the sibling
        // dial this pairs with. Two floats, not two more packed bits, because a reuse tolerance is a
        // small continuous number, not an enumerable choice like gAmbientParams.w's bitfields.
        f32 viewParams[4] = {};
        // RTXDI ReSTIR GI control -- mirrored as gGiRestirParams. x = 1 while giMode==1 is ACTUALLY
        // running this frame (giRestirWanted(): hardware, tier and giMode all agree) -- NOT a raw
        // copy of Settings::giMode, because the reservoir buffer/surface-history pair are only ever allocated
        // when giRestirWanted() is true, and a shader branching on the raw setting instead would
        // read the null-filled t12/u6/u7 slots on a device that requested ReSTIR GI but cannot run
        // it. y = 1 once the surface-history pair ALSO holds a real previous frame (own flag, not gRtHistParams.y
        // -- see giHistValid_ for why the shared one is the wrong test the one frame this pair is
        // freshly created while the shadow/reflection pair already isn't). z = which of the
        // reservoir buffer's two array slices (rtxdi::c_NumReSTIRGIReservoirBuffers) THIS frame
        // writes -- shares rtHistWriteIdx_'s cadence (see beginShadowHistory) since both flip in the
        // same lockstep whenever ray tracing is active. w = the ReSTIR-GI poison debug view
        // (giPoisonView_ / setGiPoisonView); was "spare" -- a repurposed bit, not a new field, so
        // this struct's size and every other field's offset are unchanged. Published unconditionally,
        // near the top of beginShadowHistory, so it reaches PSMainVoxi/PSRayDriven's OWN violet
        // specular-ceiling marker (B1/F5, voxi.hlsl) on every giMode, not only the frames that reach
        // this comment's giRestirWanted() block further down -- see beginShadowHistory's own F5
        // comment for why. See voxi_restir.hlsli's giRestirIndirect for the giMode==1 shader side.
        f32 giRestirParams[4] = {};
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
    //
    // ALSO MIRRORED IN modules/render.voxi/shaders/voxi_gi.hlsli's OWN `cbuffer VoxiFrame` -- that
    // file's header comment states plainly it declares the FULL block so a caller can bind
    // giFrameConstants() verbatim, and its own tail comment says "when you append there, append
    // here too". giRestirParams above is appended in both places for exactly that reason, even
    // though nothing in voxi_gi.hlsli's cone-only prelude reads it.
    static_assert(sizeof(FrameConstants) == 736,
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
    // Whether PSVoxel bakes the sky into the volume: the negation of what prePass writes to
    // cb_.viewParams[2] (and PSVoxel reads as gViewParams.z). The SETTING, not whether ReSTIR GI
    // actually ran this frame -- that also goes false for a debug-view or empty-TLAS frame, and keying
    // the bake on it rebuilt the volume twice per debug-view toggle. The gate and the GI cache key both
    // carry it: a volume baked with the sky is a different answer from one baked without.
    bool voxelSkyInjected() const { return !giRestirWanted(); }

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
    bool giSnapVoxelSky_ = true;   // voxelSkyInjected() at the last bake
    u64 giSkipped_ = 0, giRebuilt_ = 0;   // for the one-time report; counts, not impressions
    mutable u32  giGateWhyMask_ = 0;   // one bit per rejection reason already reported
    u32          voxelCullLogs_ = 0;   // voxelize passes so far; the cull ratio reports at 2^n of them
    bool         drawCapReported_ = false;   // the draw-list-full warning is worth saying once, not every frame
    u64  giGateNextReport_ = 64;   // doubles each time, so the steady state gets reported too
    u64  giGateLastTicks_ = 0, giGateLastSkipped_ = 0;

    // ---- M4: force every tick past the gate above, bypassing the cache in both directions --------
    // See setGiForceRebuild's own comment (public section, next to setGiUpdateInterval) for the full
    // contract. Backing store only; the setter is where a change gets logged.
    bool giForceRebuild_ = false;

    // ---- W3: bound the clear/resolve/mip-filter dispatch to the box a rebuild can actually change --
    // See setGiBoundedDispatch's own comment for the contract and voxelizePass's own comment for the
    // box-selection rule and the induction argument that makes it safe. Backing store plus the
    // bookkeeping a rebuild needs to know what the LAST rebuild's box covered.
    //
    // ON BY DEFAULT SINCE IT WAS MEASURED. It was written, reasoned about at length, and then left
    // switched off, so every GI rebuild cleared, resolved and mip-filtered the WHOLE 512^3 grid to
    // touch a couple of percent of it. The census line says it outright on PTTest NewSponza:
    //
    //   injected-draw box 1.3% of the 512^3 grid, clear/resolve/mip over 100.0% (full: bounded off)
    //   injected-draw box 1.3% of the 512^3 grid, clear/resolve/mip over   1.3% (bounded)
    //
    // MEASURED, ray-driven, 400 frames with a moving camera: GPU total 30.64ms -> 30.25ms. That is
    // 0.39ms, about 1.3% of the frame -- real and free, but deliberately not oversold: the named
    // spans it drains (Voxi voxelise 0.26ms, Voxi mip filter 0.05ms) were never the bulk of
    // anything. The reason to take it is that the work is pure waste, not that it is a big number.
    //
    // PIXEL-NEUTRAL, verified rather than argued: full-resolution capture against the same frame
    // with it off is 99.3% bit-identical at 0.0024 mean absolute difference, with the residual in
    // the GI temporal noise this renderer already has frame to frame.
    //
    // The induction in voxelizePass is what makes it safe, and every edge that could break it
    // forces a full rebuild instead: no previous box, an unbounded draw, the volume moving or
    // resizing. setGiBoundedDispatch also clears giBoxPrevValid_ on ANY toggle in either
    // direction, so flipping this default cannot inherit a box that was never enforced.
    bool giBoundedDispatch_ = true;
    // This rebuild's own box0 (what clear/resolve actually ran over), stashed so filterMips can
    // derive each mip level's own box from it via mipBox() without voxelizePass having to pass it as
    // a parameter or filterMips having to recompute the pre-pass walk over drawsPrev_ a second time.
    VoxelBox giDispatchBox0_{};
    // The LAST rebuild's own draws box (the union of every surviving draw's world AABB, NOT box0 --
    // box0 also folds in THIS rebuild's own draws box, and unioning box0 into next rebuild's box would
    // let the covered region grow forever instead of tracking only the two rebuilds a voxel's value
    // can actually still depend on). See voxelizePass's own comment for why the union of exactly these
    // two draws boxes is enough.
    VoxelBox giBoxPrevDraws_{};
    // False whenever the box above cannot be trusted as "last rebuild's box": no rebuild has run yet,
    // the last rebuild had a draw with no usable AABB (anyUnbounded), or the volume was just restored
    // whole from the on-disk cache (giCacheRestore sets this false on a hit -- a restore doesn't know
    // what box the file's bake used). False forces the NEXT rebuild's box0 to be the full grid.
    bool giBoxPrevValid_ = false;
    // The resolution/centre/extent the box above was recorded under. A volume that moved, resized or
    // rebuilt at a different resolution since invalidates the box even though giBoxPrevValid_ itself
    // is still true -- the STORED coordinates would describe a different volume than the one about to
    // be voxelised.
    u32 giBoxRes_ = 0;
    f32 giBoxCentre_[3] = {};
    f32 giBoxExtent_ = -1.0f;

    // ---- W12: free the injection accumulator after the gate has gone quiet for a while -----------
    // See setGiFreeAccumulator's own comment for the contract and manageInjectionAccumulator()'s own
    // comment (.cpp) for the two branches and the Vulkan ordering hazard that fixes where it is called
    // from.
    bool giFreeAccumulator_ = false;
    // Set by the rebuild gate the tick it finds voxelAccumTex_ missing and needs it -- consumed (and
    // cleared) by manageInjectionAccumulator() the NEXT prePass, which is the one-tick delay
    // setGiFreeAccumulator's own comment documents. Also true's-equivalent path: !giFreeAccumulator_
    // alone is enough to recreate (turning the flag off must bring the accumulator straight back), so
    // this flag only matters while giFreeAccumulator_ is actually on.
    bool giAccumWanted_ = false;
    // Latched so a recreate failure (out of memory, most likely) warns once rather than every tick it
    // keeps failing -- same idiom as layeredBsdfWarned_/nrdWarnedMsaa_ elsewhere in this class. Reset
    // on the next successful recreate, so a LATER failure (a different cause) is not silenced by an
    // EARLIER one already having been reported.
    bool giAccumRecreateFailedLogged_ = false;
    // Consecutive GI ticks with nothing to do: the skip branch increments this while
    // giConvergeTicks_ == 0 (a converging bake is busy, whatever the snapshot gate alone would have
    // said), and any rebuild resets it to 0. Compared against kGiAccumulatorQuietTicks below to decide
    // whether the accumulator has been idle long enough to free.
    u32 giQuietTicks_ = 0;
    // A tiny stand-in UAV (4x1x1, R32_UINT) bound to bindings_/clearBindings_/resolveBindings_ slot 1
    // in place of voxelAccumTex_ while it is freed -- every binding set that names a resource must be
    // rebound to something else BEFORE that resource is destroyed (aver-view-outlives-its-buffer.md),
    // and this is that something else. Created once on the first free, kept until shutdown().
    rhi::TextureHandle voxelAccumPlaceholder_ = 0;
    // UNMEASURED. How many consecutive quiet GI ticks before the accumulator is freed -- a guess at
    // "long enough that a still editor session is actually done lighting for now, short enough that
    // scrubbing a timeline or nudging the sun doesn't recreate it every few seconds", not a number
    // anyone has timed a real editing session against. 240 ticks is a few seconds at Epic's default
    // giUpdateInterval of 1; raising or lowering it only trades how eagerly the memory comes back
    // against how often a idle-then-resumed session pays the one-tick recreate delay.
    static constexpr u32 kGiAccumulatorQuietTicks = 240;

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
    // THE THIRD PAIR, and the one the sky-occlusion ray needs for the same reason the shadow
    // ray needed the first: at one ray per pixel that estimator is `open = hit ? 0 : 1`, a
    // BINARY mask -- the noisiest thing a Monte Carlo estimate can be. It was made bearable by
    // firing four rays and correlating them across a 4x4 tile, which trades salt-and-pepper for
    // visible BLOCKS and costs four incoherent rays to do it. Accumulating against a reprojected
    // history buys the sample count over time instead: one ray, no tile, no blocks.
    // RG32Float like the shadow pair, x = openness, y = linear depth for the disocclusion test.
    rhi::TextureHandle rtAoHist_[2] = {0, 0};
    // THE SKY-OCCLUSION RAY'S HIT DISTANCE, in [0,1] as a fraction of the ray's own TMax
    // (Settings::giMaxDistance). NOT ping-ponged and NOT a history: it is this frame's raw
    // measurement, overwritten whole every frame, and the thing that reads it keeps whatever
    // history it wants of its own.
    //
    // WHY IT EXISTS AT ALL, since nothing in Voxi reads it. An external denoiser cannot filter an
    // occlusion signal from the occlusion alone -- it needs to know how FAR away the occluder was,
    // because that is what sets how wide a filter may spread the sample without crossing a real
    // edge. NVIDIA NRD's REBLUR_DIFFUSE_OCCLUSION calls this IN_DIFF_HITDIST and will not run
    // without it (modules/render.nrd/README.md). The ray already computes the distance and threw
    // it away; this is where it stops being thrown away.
    //
    // R16Unorm, one channel: the value is a fraction, 16 bits of it is finer than the ray count
    // could justify, and 2 bytes/pixel beside the 112 MB the RG32Float history pair above already
    // costs at the tiers where either exists is not a number worth optimising.
    rhi::TextureHandle rtAoHitDist_ = 0;

    // ---- NVIDIA NRD, denoising the sky-occlusion signal rtAoHitDist_ above feeds ----
    //
    // WHY THIS SIGNAL FIRST, out of everything that is noisy: it is the one already measured. NRD's
    // own AVER_README puts ambient/sky occlusion at 78% of the engine's remaining speckle, and
    // IN_DIFF_HITDIST -- the one input NRD needs that an engine does not usually already have -- is
    // written today, because closing that was the third of NRD's three original blockers. Every
    // other input (view Z, motion vectors, packed normal/roughness) comes from the G-buffer.
    //
    // IT REQUIRES THE G-BUFFER, which is off by default, so nrdWanted() asks for it rather than
    // assuming it. A denoiser handed a null input texture is refused by Recorder::record rather
    // than binding one, so the failure is a log line and an undenoised frame, never a crash.
    //
    // OPTIONAL AT EVERY LEVEL AND THAT IS THE POINT: absent on Vulkan (NRD's register spaces), absent
    // in a build with AVER_WITH_NRD off, absent without the G-buffer. nrdOutput_ being 0 means "not
    // denoised this frame" and the shader falls back to the hand-written temporal filter, which is
    // what every tier below this already ships.
    render::nrd::Recorder nrd_;
    bool                  nrdActive_  = false;   // create() succeeded AND this frame has its inputs
    rhi::TextureHandle    nrdOutput_  = 0;       // OUT_DIFF_HITDIST, 0 when not denoised
    u32                   nrdFrame_   = 0;
    bool                  nrdWarnedEncoding_ = false;
    bool                  nrdWarnedMsaa_     = false;
    // ---- NRD's OWN previous camera, separate from curViewProj_/prevViewProj_ ----
    // Those two describe the COMBINED viewProj every other reprojection consumer in this file wants;
    // NRD needs the FACTORISED pair (CameraFactor.hpp), a different shape, so it keeps its own
    // latch rather than reusing theirs -- see beginShadowHistory's NRD block for where these are
    // read and written. Row-major, same layout as viewProj itself (so a memcpy into
    // FrameSettings::worldToViewPrev/viewToClipPrev is exactly as direct as into worldToView/
    // viewToClip). Valid only once nrdPrevCameraValid_ is true: latched after a frame's
    // FrameSettings were built from a SUCCESSFULLY FACTORISED camera, and invalidated both by a
    // factorisation failure and by beginShadowHistory's own skipped-frame branch (shadowHistoryActive()
    // false) -- see each site's own comment for why.
    f32                   nrdPrevWorldToView_[16] = {};
    f32                   nrdPrevViewToClip_[16]  = {};
    bool                  nrdPrevCameraValid_     = false;
    // THE CAMERA ONE FURTHER BACK, for Settings::nrdCameraMatchesInputs: NRD denoises last frame's
    // inputs, so its "previous" camera is the one from two frames ago. Shifted from the latch above
    // every time that one is written, and invalidated at every site that invalidates it.
    f32                   nrdPrev2WorldToView_[16] = {};
    f32                   nrdPrev2ViewToClip_[16]  = {};
    bool                  nrdPrev2CameraValid_     = false;
    // The mode last frame's dispatch used; starts at Settings::nrdCameraMatchesInputs's default.
    bool                  nrdCameraMatchedLast_    = false;
    // Set the first time CameraFactor::factor() rejects this frame's camera (or dev_->camera() has
    // none to give) while the legacy A/B switch is off -- see beginShadowHistory's own comment for
    // why that frame skips the NRD dispatch entirely rather than falling back to a wrong encoding.
    // Warned once for the same reason nrdWarnedMsaa_ is: a condition that cannot self-heal by
    // waiting would just repeat the same WARN every frame.
    bool                  nrdWarnedCameraFactor_ = false;
    // 3.4 b: set the first time this frame's G-buffer inputs (viewZ/motionVectors/normalRoughness)
    // disagree in size with the signal NRD is about to be resized to (rtAoHitDist_/giRadiance_) --
    // see beginShadowHistory's own NRD-rect-vs-resource guard for why this can happen even though
    // targets are reallocated together on an ordinary resize (3.4 a). Cleared the moment the sizes
    // agree again, UNLIKE nrdWarnedMsaa_/nrdWarnedCameraFactor_ above: this condition is expected to
    // self-heal within a frame or two, and a LATER, unrelated mismatch episode should still warn.
    bool                  nrdWarnedInputSizeMismatch_ = false;
    // voxi.nrdLegacyCamera's live backing store -- see setNrdLegacyCamera's own comment (VoxiRenderer.hpp)
    // for what ON reinstates and why toggling either way resets NRD's history.
    bool                  nrdLegacyCamera_ = false;
    // Set the first time applyReblurTuning() fails while nrd_ reports itself valid -- a real error
    // (NRD rejected the settings), not the ordinary "not created yet" case setReblurTuning() already
    // no-ops on silently. Warned once, same shape as nrdWarnedEncoding_/nrdWarnedMsaa_ above: this can
    // run every frame once NRD exists, and a per-frame WARN for a condition that will not self-heal is
    // noise, not information.
    bool                  nrdWarnedReblurRetune_ = false;
    // The ReSTIR GI radiance handed to NRD (u9): rgb indirect diffuse, a normalised hit distance.
    // NOT ping-ponged, unlike every history pair here -- it is this frame's raw measurement handed
    // to a filter that keeps its OWN history in NRD's permanent pool, so a second copy would buy
    // nothing. Allocated only when ReSTIR GI is the active estimator, because nothing else writes it.
    rhi::TextureHandle    giRadiance_ = 0;
    rhi::TextureHandle    nrdGiOutput_ = 0;   // OUT_DIFF_RADIANCE_HITDIST, 0 when not denoised
    // Whether the OCCLUSION denoiser actually ran last frame. It is skipped under ray-driven
    // primary visibility, whose output nothing reads (see beginShadowHistory), so on the frame it
    // rejoins -- the user switching back to raster -- its NRD history is from before the gap and
    // must be reset rather than reprojected with one frame's camera delta.
    bool                  nrdAoRanLastFrame_ = false;

    // ---- RTXDI ReSTIR GI: the reservoir buffer and the previous-frame surface it resamples against ----
    //
    // giReservoirs_ IS THE STORAGE THE VENDORED SDK OWNS THE SHAPE OF. RTXDI_PackedGIReservoir
    // (third_party/rtxdi/Include/Rtxdi/GI/ReSTIRGIParameters.h) is 32 bytes; one RWStructuredBuffer
    // holds BOTH of rtxdi::c_NumReSTIRGIReservoirBuffers' ping-pong copies, addressed by
    // RTXDI_ReservoirPositionToPointer's own `reservoirArrayIndex * reservoirArrayPitch` term
    // (Utils/ReservoirAddressing.hlsli) -- so this is ONE buffer, ONE binding (u6, StructuredBuffer),
    // never rebound mid-frame the way the ping-ponged TEXTURE pairs below are; only which array
    // slice each side of the pointer arithmetic reads/writes changes, which is cb_.giRestirParams.z,
    // not a descriptor swap.
    //
    // SIZED FROM giSurfPosHist_'s OWN RESOLUTION, not a separately-tracked width/height: the reservoir
    // buffer and the surface history are created together in ensureShadowHistory from the same
    // width/height, and the SHADER derives the identical block-pitch formula from
    // gGiSurfHist.GetDimensions() (voxi.hlsl's giReservoirBufferParams) -- so there is exactly one
    // source of truth for the resolution both sides compute pitch from, rather than a cbuffer field
    // that could drift from the texture it is supposed to describe.
    rhi::BufferHandle giReservoirs_ = 0;
    // Element count (RTXDI_PackedGIReservoir units) the buffer was actually sized for -- the whole
    // POINT of caching this is so a resize that does not grow the pitch (most resolution changes,
    // since the pitch rounds up to 16-pixel blocks) skips the destroy/recreate entirely, the same
    // "only rebuild when the size actually changed" rule ensureShadowHistory already applies to
    // rtShadowHist_ itself.
    u32  giReservoirElemCapacity_ = 0;

    // ---- STAGED RAY-DRIVEN PASSES (milestone 1): the two resources CSRdVisibility/CSRdShadow/the
    // AVER_RD_SPLIT pixel shader pass a record through, u11/u12 in every Voxi binding set ----
    //
    // rdVisBuf_: one uint4 (16 bytes) per pixel of the SCENE RENDER TARGET PSRayDriven's i.pos.xy
    // indexes -- NOT the (possibly smaller) scene-viewport sub-rect the staged compute dispatches
    // actually cover, because i.pos.xy is a render-target-space pixel centre and can be as large as
    // that sub-rect's own offset plus its extent. Sized and grown exactly like giReservoirs_ above:
    // a StructuredBuffer, resized in ensureRdStagedResources (mirroring ensureShadowHistory) only
    // when the row pitch actually changes, never on every resize. See rdStagedRowPitch_ for the
    // pitch this buffer and cb_.viewParams.w both agree on.
    rhi::BufferHandle rdVisBuf_ = 0;
    // Element count (uint4 units) rdVisBuf_ was actually sized for -- the "only rebuild when it grew"
    // cache, the identical shape giReservoirElemCapacity_ keeps for its own buffer.
    u32  rdVisBufElemCapacity_ = 0;
    // rdSunVisTex_: RGBA16F, rgb = the sun ray's transmittance (see CSRdShadow, voxi.hlsl) at the
    // SAME render-target resolution as rdVisBuf_ -- a plain RW 2D texture rather than a ping-ponged
    // pair, because nothing here accumulates temporally; rtShadowTemporal's own history (rtShadowHist_
    // above) already owns that, and this is just this frame's raw result handed from Stage S to Stage B.
    rhi::TextureHandle rdSunVisTex_ = 0;
    // The render-target size rdVisBuf_/rdSunVisTex_ were last (re)created at, and rdVisBuf_'s row
    // pitch in pixels -- the SAME number cb_.viewParams.w carries into the shaders (set around the
    // three staged uploads in recordStagedRayDriven, 0 otherwise) and
    // CSRdVisibility/CSRdShadow/the AVER_RD_SPLIT branch of PSRayDriven all index rdVisBuf_ with.
    // Tracked separately from rtShadowHistW_/H_ even though the two almost always agree: staged mode
    // can be switched on after ray tracing already sized the shadow history, and ensureRdStagedResources
    // must still know whether IT has caught up, independent of what ensureShadowHistory has done.
    u32  rdStagedW_ = 0, rdStagedH_ = 0;
    u32  rdStagedRowPitch_ = 0;
    // Tiny stand-ins bound at u11/u12 whenever rdVisBuf_/rdSunVisTex_ do not exist -- staged mode is
    // off, the device is not D3D12, or the real resources failed to allocate -- so every declared UAV
    // slot in every Voxi binding set always has a valid descriptor of the right kind, the identical
    // contract voxelAccumPlaceholder_ keeps for u1. Created once, on first need, kept until shutdown.
    rhi::BufferHandle  rdVisBufPlaceholder_ = 0;
    rhi::TextureHandle rdSunVisPlaceholder_ = 0;
    // (Re)creates or releases rdVisBuf_/rdSunVisTex_ for the given render-target size, rebinding u11/
    // u12 to the placeholders above when staged mode is not wanted or the size is 0. Called from
    // onRenderTargetsChanged (a resize) and from setSettings on the rayDrivenStages on/off edge, the
    // identical two call sites ensureShadowHistory itself has and for the identical reason: neither
    // alone sees every edge that changes what this should hold.
    bool ensureRdStagedResources(u32 width, u32 height);
    // Whether Settings::rayDrivenStages asks for the staged split at all -- NOT whether it will
    // actually run this frame, see rdStagedActive() for that.
    bool rdStagedWanted() const { return settings_.rayDrivenStages == 1u; }
    // Whether rdVisBuf_/rdSunVisTex_ are worth ALLOCATING at all -- rdStagedWanted() plus
    // rayTracingWanted(), the same VRAM-consciousness aoHistoryWanted()/giRestirWanted() already
    // apply to their own pairs: the staged compute passes read the SAME RT geometry/instance tables
    // (gRtInstances/gRtVerts/gRtIndices) the single-pass ray-driven primary does, so nothing here has
    // anything to do while ray tracing itself is off, whatever the setting asks for.
    bool rdStagedResourcesWanted() const { return rdStagedWanted() && rayTracingWanted(); }
    // Whether THIS frame's ray-driven primary will run as the three staged passes rather than the
    // single PSRayDriven draw. Requires, in order: rdStagedWanted() (the setting), rayDrivenActive()
    // (there is a ray-driven primary to stage at all -- staged mode has nothing to say about the
    // rasteriser or the debug raymarch), the backend being D3D12 (recording compute inside the scene
    // pass is Vulkan-illegal -- see D3D12RenderContext::dispatch/setPipeline, verified read-only),
    // both compute pipelines, both resources, and the TEXTURED ray-driven pipeline being the one
    // scenePass() would actually bind this frame (matching whichever of the plain/G-buffer pair
    // pickGbuf() selects) together with ITS OWN AVER_RD_SPLIT twin. `reason`, when non-null, is set
    // to a human-readable explanation the ONE time this returns false while rdStagedWanted() is true
    // and rayDrivenActive() is true -- scenePass() logs it once and falls back, never touching it
    // again once rdStagedFallbackLogged_ latches. Left untouched (still null) when rdStagedWanted()
    // or rayDrivenActive() alone is what said no, since neither of those is a fallback worth a log --
    // the feature simply was not asked for, or there is nothing to stage this frame.
    bool rdStagedActive(const char** reason = nullptr) const;
    // Latches the ONE fallback warning rdStagedActive()'s `reason` output produces -- the identical
    // "said once" idiom giAccumRecreateFailedLogged_/layeredBsdfWarned_ already use elsewhere in this
    // class, so a project that requests staged mode on hardware/a backend that cannot give it does
    // not repeat the same line every frame for the rest of the session.
    bool rdStagedFallbackLogged_ = false;
    // The other half of that log: said once, the first frame the staged passes actually record, so a
    // one-off fallback logged on a start-up frame (RT history not ready yet) is not the last word.
    bool rdStagedRunLogged_ = false;

    // THE ENGINE GAP THE TASK BRIEF NAMED: RAB_GetGBufferSurface(idx, /*prevFrame*/true) needs a
    // PREVIOUS frame's primary surface, and nothing in Voxi carried one before this pair existed --
    // the deferred G-buffer (when even on) is current-frame-only, and AVER_GBUFFER_HISTORY's own
    // gGBufNormalHist (voxi.hlsl) is exactly this gap's own unfinished half-attempt: declared,
    // referenced in three tap loops, gated behind a define nothing ever sets to 1, because it never
    // grew the ping-pong pair (VoxiRenderer.hpp/.cpp) or the resolve of what "t10 IS A GUESS" (its
    // own comment) should really be. THAT one is left exactly as found -- reusing an admittedly-
    // guessed slot for a DIFFERENT feature (a crease term for the spatial shadow/reflection filters,
    // gated on the G-buffer being on at all) risks the very drift its own comment warns about, and
    // ReSTIR GI needs to work whether or not the G-buffer is even enabled. This is a NEW pair.
    //
    // TWO RG32Float TEXTURES, NOT ONE RGBA32F -- rhi::Format (RHIResources.hpp) HAS NO FOUR-CHANNEL
    // 32-BIT FLOAT FORMAT (checked against the enum directly: RGBA16F, R32Float and RG32Float are
    // the only float formats it declares), so a single-texture design was not an option regardless
    // of how the four values were packed. Full 32-bit float precision is worth the second texture:
    // RTXDI_CalculateJacobian's partial-Jacobian terms are distance-squared RATIOS, and RGBA16F's
    // ~11-bit mantissa was considered and rejected on paper -- at a few thousand centimetres of scene
    // extent its relative error is already comparable to the very ratio the Jacobian is measuring.
    //
    // giSurfPosHist_: xy = the world position's x/y. giSurfNrmHist_: x = the world position's z,
    // y = RTXDI_EncodeNormalizedVectorToSnorm2x16's packed uint, bit-reinterpreted with asfloat so a
    // single RG32Float channel holds a whole octahedral-packed normal -- reusing the vendored SDK's
    // own packing rather than inventing another, and splitting position/normal across the PAIR
    // rather than, say, xy/z-of-position in one and normal whole in the other, so a reader who wants
    // just the position (RAB_GetSurfaceWorldPos) or just validity (the packed-normal channel) knows
    // which texture to touch without cross-referencing both for every field.
    //
    // 0 IN THE PACKED-NORMAL CHANNEL is reserved as "nothing written here" (a sky miss in
    // PSRayDriven, or a pixel from BEFORE this pair was ever written): the write side nudges an
    // exact-zero encoding to 1 so that sentinel is never produced by a legitimate normal (see the
    // write site in voxi.hlsl) -- WHY A SENTINEL AT ALL rather than trusting a freshly-created
    // texture to read as zero: this repo's own render-scale/device-loss and stale-worktree incidents
    // are both reminders that "the driver probably zero-fills a fresh allocation" is not a
    // documented API guarantee, and RTXDI's own 5-sample temporal search plus fallback-sampling mode
    // is designed to shrug off an occasional false "no surface here" from one rejected candidate --
    // so an EXPLICIT, cheap-to-check sentinel costs nothing this file doesn't already have machinery
    // to tolerate, and buys not depending on that assumption.
    //
    // DEPTH FOR THE RAB SIMILARITY TEST IS NOT STORED A THIRD TIME: it is re-derived as
    // `mul(float4(storedWorldPos, 1.0), gPrevViewProj).w` (voxi.hlsl's RAB_GetGBufferSurface) --
    // free (gPrevViewProj already exists for the shadow/reflection histories), exact (not requantised
    // through a texture format), and one fewer thing that could disagree with the position it is
    // supposed to describe.
    //
    // THE ENGINE GAP THE TASK BRIEF NAMED, this pair's whole reason to exist:
    // RAB_GetGBufferSurface(idx, /*prevFrame*/true) needs a PREVIOUS frame's primary surface, and
    // nothing in Voxi carried one before this -- the deferred G-buffer (when even on) is
    // current-frame-only, and AVER_GBUFFER_HISTORY's own gGBufNormalHist (voxi.hlsl) is exactly this
    // gap's own unfinished half-attempt: declared, referenced in three tap loops, gated behind a
    // define nothing ever sets to 1, because it never grew a ping-pong pair of its own or resolved
    // what "t10 IS A GUESS" (its own comment) should really be. THAT one is left exactly as found --
    // reusing an admittedly-guessed slot for a DIFFERENT feature (a crease term for the spatial
    // shadow/reflection filters, tied to the G-buffer being on at all) risks the very drift its own
    // comment warns about, and ReSTIR GI needs to work whether or not the G-buffer is even enabled.
    //
    // BOTH PING-PONGED LIKE rtShadowHist_/rtReflHist_/rtAoHist_ -- share their rtHistWriteIdx_ swap
    // cadence in beginShadowHistory (all flip in lockstep whenever ray tracing is active at all),
    // but NOT their rtHistValid_ flag -- see giHistValid_ below for why sharing it would be wrong.
    rhi::TextureHandle giSurfPosHist_[2] = {0, 0};
    rhi::TextureHandle giSurfNrmHist_[2] = {0, 0};

    // ---- U1/2.11: the half-resolution ReSTIR VISIBILITY history pair (t16/u10) ----
    // See giVisHistWanted() for when this is wanted (a strict subset of giRestirWanted() -- only
    // Settings::giRestirVisibility's HalfResolution mode), and ensureShadowHistory/beginShadowHistory
    // for creation, the starting bind and the per-frame ping-pong swap. Half the linear dimension of
    // giSurfPosHist_/giSurfNrmHist_, rounded up: 2.10 D/E write exactly one full-resolution pixel per
    // 2x2 block per frame, so one texel per block is all this needs to hold. RGBA16F: r = the F3
    // reuse-visibility EMA, g = the F2 traced-luminance EMA, b = the F2 unoccluded-sky-luminance EMA,
    // a = 1 written / 0 never (giVisReconstruct's own validity test against it, voxi_restir.hlsli).
    rhi::TextureHandle giVisHist_[2] = {0, 0};
    // True only once a full write+swap cycle has happened with the pair actually bound this frame --
    // the SAME shape giHistValid_ below has, and independent for the identical reason THAT flag is
    // independent of rtHistValid_: giRestirVisibility_ can move to or away from
    // HalfResolution mid-session while giHistValid_ (or rtHistValid_) is already true from an
    // unrelated cycle, so trusting either shared flag here would feed giVisReconstruct a "previous
    // frame" that never actually existed for THIS pair.
    bool giVisHistValid_ = false;
    // True once giVisHist_'s read side has been left in UnorderedAccess by a write on some earlier
    // active frame since the pair was (re)created -- RESOURCE STATE, not content trust like
    // giVisHistValid_ above. Cleared only where the pair itself is destroyed, never by a validity
    // reset (setSettings, resetGiHistory, the skipped-frame branch) that leaves the textures alone.
    bool giVisHistPrimed_ = false;
    // Latched so an allocation failure (the pair is small, but a device can already be out of memory
    // by the time this frame's create runs) warns once rather than every frame it keeps failing --
    // same idiom as giAccumRecreateFailedLogged_/nrdWarnedMsaa_ elsewhere in this class. Cleared on
    // the next successful create, same reason giAccumRecreateFailedLogged_'s own comment gives.
    bool giVisHistFailLogged_ = false;
    // Settings::giRestirVisibility, cached at setSettings like giMode_ beside it. 2 (HalfResolution)
    // matches the struct default Voxi.hpp gives it (Quality::Medium's own ladder rung), so a renderer
    // that somehow renders a frame before its first setSettings call behaves as Medium would rather
    // than as NoRay (0), which is what an un-initialised u32 read as before this field existed.
    u32 giRestirVisibility_ = 2;
    // Settings::giRestirSpatialSamples, cached at setSettings the same way giRestirVisibility_ just
    // above is, for the identical defensive reason: Voxi.cpp's setSettings already range-clamps
    // Settings::giRestirSpatialSamples to [0,15], and std::min repeats that ceiling here so this
    // member can never disagree with givis::packAmbientW's own `& 15u` mask of it. 15 (AUTO) matches
    // the struct default Voxi.hpp gives it, so a renderer that somehow renders a frame before its
    // first setSettings call leaves the motion discount's own numSamples alone rather than forcing
    // temporal-only reuse on an un-initialised zero.
    u32 giRestirSpatialSamples_ = 15;
    u32 giRestirMaxHistory_ = 1;       // Settings::giRestirMaxHistory, gAmbientParams.w bits 18-23
    // voxi.blendedGiCone's live backing store -- see setBlendedGiCone's own comment.
    bool blendedGiCone_ = false;
    // voxi.giVisPathView's live backing store -- see setGiVisPathView's own comment.
    bool giVisPathView_ = false;

    // False right after the pair is (re)created (construction, a resize, or giMode's OWN on/off
    // edge) and true only once a full write+swap cycle has happened with giRestirWanted() true.
    //
    // DELIBERATELY SEPARATE FROM rtHistValid_, even though every other ping-ponged pair in this file
    // shares that one flag (see aoHistoryWanted()'s own pair, gated by cb_.rtDenoiseParams.w but
    // read by the shader against the SHARED gRtHistParams.y, not a fourth flag of its own). Sharing
    // it here would be wrong in a case those three pairs cannot hit: rtShadowHist_/rtReflHist_/
    // rtAoHist_ are all driven by the SAME condition family (rayTracingWanted(), aoHistoryWanted()),
    // so by the time any of them exists, rtHistValid_ already means "has been through at least one
    // real cycle under conditions close enough to today's". giMode is an INDEPENDENT switch a user
    // can flip mid-session while ray tracing has already been running for a long time -- turning it
    // on would find rtHistValid_ already true (the shadow/reflection pair have been cycling for
    // frames) while this pair itself was allocated THIS frame and holds whatever a fresh
    // allocation happens to hold. Trusting the shared flag there would feed RTXDI's temporal
    // resampling a "previous frame" that never actually existed. Its own flag closes exactly that
    // gap and no other.
    bool giHistValid_ = false;
    // True once giSurfPosHist_/giSurfNrmHist_'s read side has been left in UnorderedAccess by a
    // write on some earlier active frame since the pair was (re)created -- RESOURCE STATE, not
    // content trust like giHistValid_ above. Cleared only where the pair itself is destroyed, never
    // by a validity reset that leaves the textures alone.
    bool giHistPrimed_ = false;

    u32  rtShadowHistW_ = 0, rtShadowHistH_ = 0;
    u32  rtHistWriteIdx_ = 0;
    // False right after creation or a resize: the textures hold no real previous frame yet, and
    // cb_.rtParams.w must say so rather than let the shader blend against garbage.
    bool rtHistValid_ = false;
    // True once rtShadowHist_/rtReflHist_/rtAoHist_'s read side has been left in UnorderedAccess by
    // a write on some earlier active frame since the pairs were (re)created -- RESOURCE STATE, not
    // content trust like rtHistValid_ above. Cleared only where the pairs themselves are destroyed,
    // never by a validity reset (setSettings, resetRtHistory/resetAoHistory, the skipped-frame
    // branch) that leaves the textures alone.
    bool rtHistPrimed_ = false;
    // THE SUN THE HISTORY WAS ACCUMULATED UNDER. The temporal denoiser blends up to 90% of the
    // previous frame's visibility, and its only validity test is GEOMETRIC -- a screen-space
    // reprojection plus a depth match. Nothing in it knows the light can move. So with a still
    // camera and a moving sun the reprojection passes every frame and the displayed shadow keeps
    // ~90% of a value traced against the OLD sun direction: the ray is correct and the picture is
    // ten frames behind it. That is "the shadows don't update properly when the light has moved".
    //
    // THE SUN FIELDS ONLY, NOT THE WHOLE SkyAtmosphere, and that restriction is the difference
    // between a fix and a regression: the struct carries a cloud clock that advances every frame,
    // so comparing all of it would invalidate the history on every frame and turn the denoiser back
    // into the raw one-ray noise it exists to remove. giSnapshotUnchanged excludes cloudTime for the
    // same reason; this is the narrower question of what the SHADOW ray actually depends on.
    f32  rtHistSunDir_[3]   = {0, 0, 0};
    f32  rtHistSunColor_[3] = {0, 0, 0};
    f32  rtHistSunIntensity_ = -1.0f;   // negative so the first frame always counts as a change
    // True when this frame's sun differs from the one above; see beginShadowHistory.
    bool rtHistSunMoved() const;
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
    // Builds a render::nrd::Denoiser::ReblurTuning from settings_'s three live REBLUR dials
    // (reblurDiffusePrepassBlurRadius/reblurMaxAccumulatedFrameNum/reblurMaxStabilizedFrameNum) plus
    // the engine's own fixed hitDistA/B/C/enableAntiFirefly, and hands it to nrd_.setReblurTuning.
    // Called both right after nrd_.create() succeeds (so creation-time tuning already reflects
    // whatever was set before NRD existed, rather than ReblurTuning{}'s bare defaults) and from
    // setSettings() every time it runs thereafter (nrd::SetDenoiserSettings is documented, NRD.h, as
    // legal to call every frame -- see setSettings' own comment) -- so a console change to one of the
    // three dials takes effect on the NEXT frame without tearing the NRD instance down. A no-op
    // before nrd_.valid(): setReblurTuning() itself checks that and returns false.
    void applyReblurTuning();
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
    // The AMBIENT history pair is wanted only where the ray that fills it is traced -- High and Epic.
    // Low and Medium keep the cone gather's own occlusion (giSkyOcclusionRaysForQuality returns 0),
    // and allocating a pair they never write is 112 MB of full-screen target at 3532x1987 held for a
    // feature that does not run. That is the same argument ensureShadowHistory already makes for
    // releasing all of them when ray tracing is off, and the tiers it applies to are precisely the
    // ones most likely to be memory-bound.
    bool aoHistoryWanted() const { return rayTracingWanted() && settings_.giSkyOcclusionRays > 0; }
    // Whether ReSTIR GI (Settings::giMode == 1) will ACTUALLY run this frame: ray tracing must be
    // wanted (hardware AND the rayTracing tier both say yes -- giMode's own candidate ray needs the
    // identical acceleration structure and geometry table the shadow/reflection rays already use),
    // in addition to giMode_ itself asking for it. Gates giReservoirs_/the surface-history pair's allocation the
    // same way aoHistoryWanted() gates the ambient pair's, and is what cb_.giRestirParams.x reports
    // to the shader -- NEVER the raw setting, so a project that requests ReSTIR GI on hardware that
    // cannot run it gets the cone gather back silently rather than a shader reading a null-filled
    // t12/u6/u7.
    // giEnabled() too, not just ray tracing: giMode only ever matters for the diffuse INDIRECT term
    // (both call sites nest their giMode branch inside `gVoxelParams.w > 0.5`, itself gated on
    // giEnabled()), so a project with globalIllumination == Off has no use for the reservoir
    // buffer/surface history regardless of what giMode asks for -- same VRAM-consciousness
    // aoHistoryWanted() applies to its own pair.
    bool giRestirWanted() const { return rayTracingWanted() && giEnabled() && giMode_ == 1u; }

    // U1/2.11: whether the half-resolution ReSTIR VISIBILITY history pair (t16/u10, giVisHist_) is
    // wanted this frame -- a STRICT SUBSET of giRestirWanted() above, gated further on
    // Settings::giRestirVisibility actually asking for HalfResolution (2). Full (3), Reconstructed
    // (1) and NoRay (0) never read or write this pair, so allocating it for them would be VRAM held
    // for a mode that is not running -- the same VRAM-consciousness aoHistoryWanted()/
    // giRestirWanted() already apply to their own pairs. Gates giVisHist_'s own allocation in
    // ensureShadowHistory the same way giRestirWanted() gates the surface-history pair's.
    bool giVisHistWanted() const { return giRestirWanted() && giRestirVisibility_ == 2u; }

public:
    // THE SKY-OCCLUSION RAY'S HIT DISTANCE FOR THIS FRAME, or 0 when the ray is not running at this
    // tier (Low and Medium never trace it) or ray tracing is off entirely. See rtAoHitDist_.
    //
    // The texture rests in ResourceState::UnorderedAccess, which a caller must transition from
    // before sampling it -- this returns a handle, not a promise about state, exactly like every
    // other texture accessor that hands out something a different pass wrote.
    //
    // 0 IS THE ANSWER, NOT AN ERROR, and a caller must handle it: the tier that has no ray has
    // nothing to denoise, and the correct behaviour there is to skip the denoiser rather than to
    // filter a texture that does not exist.
    [[nodiscard]] rhi::TextureHandle ambientHitDistanceTexture() const { return rtAoHitDist_; }

private:
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

public:
    // PREDICTS suppressesScene() (== debugViewActive() || rayDrivenActive(), see the .cpp) for THIS
    // frame, for the one caller that needs an answer before this frame's real one exists yet:
    // SandboxApp::syncPtSceneView(), called from onUpdate(), BEFORE device_->beginFrame() runs this
    // frame's prePass() (see that function's own header comment for why it must run there).
    //
    // debugViewActive() is read straight -- no race. It depends on giReady_/giEnabled()/debugView_,
    // none of which prePass() is about to touch, so its value right now already IS this frame's value.
    //
    // rayDrivenActive() is the racy half being fixed. It reads rtActive_, and rtActive_ is reset to
    // false at the TOP of buildAccelerationStructures() and set true only after a successful TLAS
    // build over drawsPrev_ -- and buildAccelerationStructures() runs from prePass(), which runs
    // inside THIS frame's beginFrame(), AFTER beginScene() (called immediately before prePass(), over
    // every feature, in that same beginFrame()) has already done `drawsPrev_.swap(draws_);
    // draws_.clear();`. So the drawsPrev_ this frame's build is about to consume is NOT the member's
    // value right now -- that list was already consumed by LAST frame's build, back when THIS frame's
    // onUpdate() had not even run yet. It is today's draws_: whatever onRender submitted last frame,
    // sitting here unswapped, at the exact moment onUpdate() calls this accessor. Reading rtActive_
    // (what rayDrivenActive() does) answers last frame's question a second time; reading draws_
    // instead answers the question THIS frame's election is actually about to ask.
    //
    // rtRenderMode_ and rayDrivenPso_ carry no such race and are read unchanged: neither is reset or
    // reassigned anywhere inside beginFrame -- both are only ever written by setSettings() or pipeline
    // creation, long before any frame that reads them starts, so their value right now already IS
    // this frame's value.
    //
    // NOT REPLAYED: whether the per-draw loop over drawsPrev_ actually yields a non-empty TLAS
    // instance list. buildAccelerationStructures() can still end with rtActive_ == false on a
    // non-empty draws_ if every draw's mesh fails to produce a BLAS (e.g. a mesh destroyed since it
    // was last drawn) -- a per-mesh runtime outcome this accessor has no cheap way to replay without
    // duplicating that whole loop here. draws_ non-empty is treated as sufficient: the gap this
    // leaves is the SAME SHAPE as the one-frame race being fixed (a rare, brief disagreement), never
    // a new kind of one, and it only opens on a frame where a draw's mesh is destroyed the very frame
    // its draw would otherwise have entered the TLAS.
    bool willSuppressSceneThisFrame() const {
        const bool rayDrivenWillBeActive = rtSupported_ && settings_.rayTracing != Quality::Off &&
                                            !draws_.empty() && rtRenderMode_ == 1u &&
                                            rayDrivenPso_ != 0;
        return debugViewActive() || rayDrivenWillBeActive;
    }

private:
    // Whether the ray-traced history textures will be read and written this frame.
    //
    // TESTS debugViewActive(), NOT suppressesScene(), and the difference is load-bearing. Both
    // suppress the scene, but for opposite reasons: the debug raymarch replaces the shading
    // entirely, so nothing touches the history and preparing it would be waste. PSRayDriven calls
    // rtShadowTemporal exactly as PSMainVoxi does, so it needs the history prepared and bound --
    // asking suppressesScene() here would leave it sampling and writing resources this frame never
    // transitioned.
    // DELIBERATELY DOES NOT REQUIRE THE AMBIENT PAIR. It gates the shadow and reflection histories,
    // which exist at every ray-tracing tier; the ambient pair exists only at High and Epic, so
    // requiring it here would switch SHADOW accumulation off at Low and Medium as a side effect.
    // The ambient path has its own flag, gRtDenoiseParams.w -- see beginShadowHistory.
    bool shadowHistoryActive() const {
        return rtActive_ && rtShadowHist_[0] && rtShadowHist_[1] &&
               rtReflHist_[0] && rtReflHist_[1] && !debugViewActive();
    }
    // Swaps the read/write roles, transitions all six textures, rebinds them and sets
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

    // HOW MANY EXTRA BAKES A CHANGE BUYS. PSVoxel's feedback term adds one bounce per rebuild, so
    // this is literally the bounce depth the volume converges to after the scene settles. 5 puts the
    // residual of a 0.5-albedo room at 3% -- past the point anything is visible -- and each tick is
    // one voxelise, paid only in the frames right after something actually moved.
    static constexpr u32 kGiConvergeTicks = 5;
    u32  giConvergeTicks_ = 0;

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
    // See setGiPoisonView's own comment. Independent of debugView_ above -- the two can never be
    // confused for each other because this one never sets debugViewActive()/suppressesScene().
    bool giPoisonView_ = false;
    // See setLightingLegacyBits' own header comment for the bit table. 0 (every fix in the
    // contrast-fix plan live) until a console command or --lighting-legacy sets a bit; forwarded
    // into cb_.ambientParams[2] every frame with no condition, same as giPoisonView_ feeds
    // giRestirParams.w every frame regardless of whether anyone has ever touched it.
    u32 lightingLegacyBits_ = 0;
    bool unlit_ = false;   // --unlit / the viewport view-mode dropdown; see setUnlit
    // See setConeTraceEnabled's own comment. Defaults to true, i.e. bit-identical to every build
    // before this toggle existed -- nobody who never calls the setter sees any difference at all.
    bool coneTraceEnabled_ = true;
};

} // namespace aver::voxi
