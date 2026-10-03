// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/GiDispatchBounds.hpp"   // W3: VoxelBox/GiDispatchConstants -- see the .cpp for how
#include "aver/render/denoise/Denoiser.hpp"
#include "aver/voxi/NeuRaC.hpp"      // rc_ -- the radiance cache's buffers and resolve pass

#include <unordered_map>
#include "aver/formats/GiCache.hpp"

#include <array>
#include <string>
#include <vector>

// Voxi the render feature: voxel cone traced global illumination, a cascaded directional shadow map
// for light injection, and DXR 1.1 inline RayQuery sun shadows. Expressed purely in generic RHI.
namespace aver::voxi {

// The most foliage instances VoxiRenderer::setFoliage places; more are dropped with a warning. Bounded
// by what one TLAS holds (rhi::kMaxTlasInstances, prefix plus draws) with room to spare, and by the
// static prefix's device-local desc buffer: 64 B an instance, ~512 MiB at this count.
inline constexpr u32 kMaxFoliageInstances = 8'000'000;

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
    // Sets the sun DIRECTION only, for fitting the shadow cascades to the light axis. No colour/
    // ambient param: those come from the device's frame CB (IDevice::setSkyAtmosphere) -- it used to
    // take both, store them, and never read them, so changing them looked wired up but did nothing.
    // This stays the one place that owns where the sun points, not what it looks like.
    void setSunDirection(const f32 dirToLight[3]);
    // Where a baked GI volume may be cached between runs: "<project>\\DerivedDataCache\\GI". Empty
    // (the default) disables the cache entirely. A setter, not a project lookup: render.voxi knows
    // nothing about projects/manifests, same reason setVolume is one.
    void setGiCacheDir(const std::string& dir);

    // Replaces the scene with a raymarch of the volume.
    void setDebugView(bool on);
    // ReSTIR-GI poison debug view (voxi_restir.hlsli's non-finite guards): paints a colour per guard
    // that fires this frame instead of replacing the scene (unlike setDebugView). Rides PSMainVoxi/
    // PSRayDriven's own return value; legend in giRestirIndirect's POISON DEBUG VIEW comment and
    // aver_IsGiRestirPoisonColour (voxi.hlsl). Seven colours fire only under giMode==1 (inside
    // giRestirIndirect); an eighth, violet (B1/F5), fires under either giMode, from PSMainVoxi/
    // PSRayDriven's own ray-traced specular ceiling hit. Reaches the shader via gGiRestirParams.w.
    // Console: `set voxi.giPoisonView true`.
    void setGiPoisonView(bool on);

    // ReSTIR-GI half-res visibility path-debug view (gAmbientParams.w bit 64; legend in
    // voxi_restir.hlsli's path-view block: yellow no ray, green reconstructed, blue half-res
    // reconstruction, red half-res fallback, white full). Mutually exclusive with setGiPoisonView --
    // see giRestirIndirect for the precedence. Reasserted every frame by SandboxApp; no log. Console:
    // `set voxi.giVisPathView true`.
    void setGiVisPathView(bool on);

    // The NeuRaC visualiser (gAmbientParams.w bits 8-11; legend at voxi_neurac_io.hlsli's
    // rcDebugColour): 0 off, 1 cached light, 2 coverage, 3 cascade, 4 cell state; `grid` adds the cell
    // edges. Paints only while the cache is live (neuracLive()): staged ray-driven GI with ReSTIR
    // visibility Cached. Reasserted every frame by the editor.
    void setNeuRaCView(u32 mode, bool grid) { neuracView_ = (mode & 7u) | (grid ? 8u : 0u); }
    // The radiance cache trained and read this frame.
    bool neuracLive() const { return neuracLive_; }

    // W6/M5 pricing switch (PSMainVoxi's gAverHistoryWrite gate, voxi.hlsl; optimisation-wave-2 plan
    // section 4): OFF (default) shades a blended fragment's indirect diffuse through ReSTIR GI like an
    // opaque one, paying its full share of the ReSTIR/shadow/reflection/AO history work; ON drops it
    // back to the cheaper voxel cone gather, to price the difference. Carried as gAmbientParams.w bit
    // 16. Reasserted every frame; only logs on a real change. Console: `set voxi.blendedGiCone true`.
    void setBlendedGiCone(bool on);
    bool blendedGiCone() const { return blendedGiCone_; }

    // A/B bitmask for the lighting-contrast fix: forwarded byte-for-byte into cb_.ambientParams[2]
    // (gAmbientParams.z in voxi.hlsl/voxi_restir.hlsli/voxi_rt.hlsli/voxi_cone.hlsli) every frame (see
    // .cpp). 0 (default) = every fix live; setting a bit reinstates that one piece of old, wrong
    // behaviour, for comparison only -- never leave on. Bits:
    //   bit 1  (R0) ReSTIR candidate/sky-occlusion rays sample a fixed 45-degree ring
    //   bit 2  (R1) receiver counts its own sky twice (traced AND via ambient)
    //   bit 4  (R2) ReSTIR candidate hit's indirect sky has no visibility test
    //   bit 8  (R3) reused ReSTIR sample shades with no visibility test
    //   bit 16 (R6) cone gather is cosine-distributed AND cosine-weighted (effective cos^2)
    //   bit 32 (M5) blended fragment writes shadow/reflection/AO histories the old wrong way
    //               (PSMainVoxi's gAverHistoryWrite gate); also resets RT history, not just GI/denoiser.
    // Set via EditorConsole.hpp's five voxi.legacy* variables, or --lighting-legacy for a --frames
    // capture with no console.
    void setLightingLegacyBits(u32 bits);

    // ---- per-history reset commands: plain bool flips, no reallocation ----
    // Console-driven (resetgihistory/resetrthistory/resetaohistory/resetdenoiserhistory/resetallhistory,
    // EditorConsole.hpp), via voxi::Renderer's own request/consume flags (Voxi.hpp's
    // requestGiHistoryReset() and siblings), so the user can bisect which cross-frame history carries
    // a burned-in artifact. Same flag ensureShadowHistory flips on a resize/tier toggle, minus the
    // teardown -- the read gate treats a false flag as "no previous frame" for one frame, and since
    // the write side runs unconditionally every frame regardless, both ping-pong slices are clean
    // again within two. `quiet` skips the log line (debugResetHistoryEveryFrame calls these every
    // frame).
    void resetGiHistory(bool quiet = false);
    void resetRtHistory(bool quiet = false);
    void resetAoHistory();   // alias of resetRtHistory today -- see its own body for why
    void resetDenoiserHistory(bool quiet = false);

    // The editor's Unlit view mode. Mirrors RhiDevice::setUnlit, which only ever reaches the
    // RASTER path -- ray-driven primary visibility bypasses drawMesh entirely, so it has to be
    // told separately or the mode silently does nothing in the default renderer.
    void setUnlit(bool on) { unlit_ = on; }

    // Ray-driven-only debug views (PSRayDriven paints them; the raster path PSMainVoxi never reads
    // them, which is why the editor falls back to the rasteriser for Wireframe/G-buffer views but to
    // ray-driven for these). Packed into cb_.viewParams[0], same float unlit_ uses -- see
    // FrameConstants below for the packing. 1 is reserved for Unlit (unlit_'s own bool, with its own
    // raster mirror RhiDevice::setUnlit).
    enum class ViewDebug : u32 {
        None           = 0,
        RayHitInstance = 2,   // hash(packed instance reference, voxi_rt.hlsli) -> colour
        RayHitMaterial = 3,   // hash(hit instance's materialIndex) -> colour
        RayHitDistance = 4,   // hit distance (cm) on a log heat ramp
        Triangles      = 5,   // hash(instance index, primitive index) -> colour
        AmbientOcclusion = 6, // final AO factor (sky visibility x material AO map), greyscale
    };

    // Selects one of the debug views above (None = off). Reasserted every frame from
    // SandboxApp::onUpdate, next to setUnlit's call site.
    void setViewDebug(ViewDebug m) { viewDebug_ = m; }

    // The editor's Wireframe view: nothing this feature renders is on screen (the device draws only
    // the meshes' edges, IDevice::setWireframe), so prePass skips every pass -- shadows, voxel GI,
    // acceleration structures, ray-driven stages, denoisers. Draws are still recorded, so the draw
    // list and the GI rebuild gate are current the frame the view is left. Reasserted every frame
    // from SandboxApp::onUpdate beside setViewDebug.
    void setPaused(bool on) { paused_ = on; }

    // Whether forcing rtRenderMode to 1 THIS frame would engage ray-driven primary visibility --
    // rayDrivenActive()'s own preconditions minus the mode check itself. Used by the editor to grey
    // out ray-driven-only debug views and gate an auto-switch to rtRenderMode 1.
    bool rayDrivenAvailable() const { return rtActive_ && rayDrivenPso_ != 0; }

    // A/B measurement toggle, not a quality setting: OFF forces cb_.voxelParams.w to 0, gating
    // PSMainVoxi's and ClusterMaterialShader.hpp's coneTracedIndirect calls alike, so the cone trace
    // is skipped (same neutral ind=0/ao=1 fallback as an unready volume), while leaving the
    // volume build (voxelizePass/filterMips/giShadowPass) running exactly as normal -- separate from
    // Settings::globalIllumination, which also stops the build, confounding trace cost with build
    // cost. Isolates exactly the per-pixel lookup a GPU timestamp cannot bracket on its own: measure
    // the scene-draw span on then off, same camera/frame count, and the delta is the trace's cost.
    void setConeTraceEnabled(bool on) { coneTraceEnabled_ = on; }
    bool coneTraceEnabled() const { return coneTraceEnabled_; }

    // Sets occlusion rays per pixel toward the sun's disc, clamped to [1, kMaxShadowRays]. A knob, not
    // a constant -- nothing could measure the (linear) cost/ray-count slope while it was a 4 compiled
    // into the renderer. The sample sequence is nested, so raising this adds samples between existing
    // ones -- 1,2,4,8 is a convergence series.
    void setShadowRays(u32 n);
    u32 shadowRays() const { return rtShadowRays_; }

    // Sets the ray-traced shadow's temporal amortisation tile edge, rounded to the nearest power of
    // two in [1, kMaxPixelsPerRayTile] -- see Settings::rtPixelsPerRayTile for the full contract.
    // 1 (the default) traces every pixel every frame and is bit-identical to having no denoiser.
    void setPixelsPerRayTile(u32 n);
    u32  pixelsPerRayTile() const { return rtPixelsPerRayTile_; }

    // Sets how many frames apart the GI volume (voxelizePass + filterMips) rebuilds, clamped to
    // [1, kMaxGiUpdateInterval]. 1 (default) rebuilds every frame, bit-identical to the original
    // always-fresh behaviour. N>1 reuses the previous volume for N-1 frames; the cone trace still runs
    // every frame, just samples a volume up to N-1 frames stale. Full contract: Settings::giUpdateInterval.
    void setGiUpdateInterval(u32 n);
    u32  giUpdateInterval() const { return giUpdateInterval_; }

    // ---- M4/W3/W12: three measurement/optimisation dials, off by default, reasserted every frame
    // by the host (--gi-force-rebuild/--gi-bounded-dispatch/--gi-free-accumulator and matching
    // voxi.* console variables) -- setters must be cheap on a no-op call and log only on a real
    // change (reassert idiom). ----

    // M4: forces every GI tick past the snapshot gate (normally ~96-98% skip rate), and bypasses the
    // on-disk GI cache both ways, so a forced tick measures the bake itself rather than a cache
    // restore. Default false: gated, cached behaviour unchanged.
    void setGiForceRebuild(bool on);
    bool giForceRebuild() const { return giForceRebuild_; }
    // W3: bounds the clear/resolve/mip-filter dispatch to the draw list's changed box instead of the
    // whole [0,res)^3 grid -- see voxelizePass for the box selection/induction argument. Default
    // false: bit-identical full-grid behaviour; the CENSUS half (box size) is measured every rebuild
    // regardless, so its value can be judged before opting in.
    void setGiBoundedDispatch(bool on);
    bool giBoundedDispatch() const { return giBoundedDispatch_; }
    // W12: frees the injection accumulator (voxelResBuilt_^3*4 R32_UINT texels, 16 B/voxel, the
    // largest idle GI resource between bakes -- 2048 MiB at Epic's 512) after kGiAccumulatorQuietTicks
    // quiet ticks, recreating it on the next rebuild. DEFAULT TRUE: the recreate path is the same code
    // a console toggle already exercised, and holding two GiB idle for a session that isn't lighting
    // anything is real waste, not a hypothetical one -- see reportVramUsage() for where the rest of
    // this renderer's memory goes. Costs one extra tick of volume staleness the first time a still
    // scene moves again after being freed -- only while this flag is on.
    void setGiFreeAccumulator(bool on);
    bool giFreeAccumulator() const { return giFreeAccumulator_; }
    // M2(c): CPU cost of buildAccelerationStructures' per-draw loop over drawsPrev_ for the last build
    // that reached it -- not the BLAS/TLAS GPU recording, which has its own GPU timestamp ("Voxi
    // acceleration structures"). 0 until the first build reaches the loop; an early return (no ray
    // tracing wanted, or an empty draw list) leaves the previous value.
    f64 lastAccelBuildCpuMs() const { return lastAccelBuildCpuMs_; }

    // What the shaders were actually compiled for -- not Settings::layeredBsdf, which can change any
    // time. Compared by the editor (Rendering page's Shading model combo) to decide if a reload is
    // needed.
    bool layeredBsdfActive() const { return layeredBsdf_; }

    // The edge length the GI volume was actually built at (set once in createVoxelVolume) -- not
    // Settings::voxelResolution, which can change before the next reload. Compared the same way as
    // layeredBsdfActive() above.
    u32 voxelResolutionBuilt() const { return voxelResBuilt_; }

    // Measurement only (see AVER_RD_ABLATE's block in voxi.hlsl for what each value removes). Must be
    // set BEFORE init() -- it becomes a shader define, compiled once. Any non-zero value renders a
    // deliberately wrong frame, for timing only, never shipped or tied to a tier.
    void setRayDrivenAblation(u32 mode) { rdAblate_ = mode; }
    // --rt-denoise-motion: how fast the spatial shadow filter tapers with the gather centre's
    // reprojection velocity. 0 (default) = no taper. A measurement dial that needed this setter to
    // be reachable at all -- the field's own comment claimed its motion contribution could be
    // isolated "at runtime instead of by rebuilding with a line commented out", but nothing outside
    // this class could set it. Same knob-with-no-plumbing gap giSkyOcclusionRays/giSkyOcclusionTile had.
    void setRtDenoiseMotionTaper(f32 v) { rtDenoiseMotionTaper_ = v; }

    void setFrameTimeReport(bool on) { frameTimeReport_ = on; }

    // Largest ray count accepted. Not a hardware limit: there's a recorded TDR history on this
    // machine, and 64 rays/pixel at 8x MSAA is how a knob turns into a device removal.
    static constexpr u32 kMaxShadowRays = 32;
    // Largest tile edge for shadow amortisation: 16x16 (one traced pixel covers 256) is already
    // aggressive enough that a fast-moving caster/camera visibly lags the tile converging behind it.
    static constexpr u32 kMaxPixelsPerRayTile = 16;
    // Largest GI revoxelise interval: 8 frames of staleness is already visible lag at normal motion.
    static constexpr u32 kMaxGiUpdateInterval = 8;

    // Fog occlusion volume edge, fixed (not derived from voxelResolution): CSAirVis marches whole
    // cones, not one ray/pixel, so a small constant bounds cost regardless of quality tier. 32, not
    // the 48 first tried -- a full 48^3 CSAirVis measured ~10 ms. Must equal voxi.hlsl's
    // AVER_AIRVIS_RES. See airVisTex_.
    static constexpr u32 kAirVisResolution = 32;
    // Z-layers CSAirVis refreshes per frame (round-robin): full volume refreshes every
    // kAirVisResolution/kAirVisSlabLayers frames, ~1/16 of a full pass's cost.
    static constexpr u32 kAirVisSlabLayers = 2;

    // Starts a new frame's draw list; the passes replay the previous one.
    void beginScene() override;
    // Records one draw into this frame's list. `translucent`/`hiddenFromOwner` both default false so
    // every existing caller -- the editor's off-screen shadow submit, the cluster path, the packaged
    // game -- keeps its behaviour untouched; only submitDraw's blended branch and the editor's
    // owner-hide branch pass true, respectively. See Draw::translucent for the flag's cost.
    void submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                const void* drawConstants, u32 drawConstantBytes, bool translucent = false,
                bool hiddenFromOwner = false, bool movable = false);
    // Sticky flag for draws that arrive through submitDraw (the raster route: device.drawMesh
    // broadcasts to every feature and carries no flag of its own). game::drawWorld sets it around a
    // movable entity's raster draws and clears it straight after; submitDraw ORs it into what it
    // records. See Draw::movable.
    void setSubmitMovable(bool on) { submitMovable_ = on; }
    // `blended` must be checked BEFORE a draw reaches draws_/drawsPrev_: everything downstream treats
    // list membership as "opaque scene geometry" (voxelizePass injects it as a light, shadowPass casts
    // a hard shadow, buildAccelerationStructures puts it in the TLAS) -- a translucent pane is wrong in
    // a different way each time (blocking indirect light, an opaque shadow from something see-through,
    // or a solid reflection of it). Defined in the .cpp, which counts and reports the drop.
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

    // Whether a blended draw's material actually samples the captured backdrop (see base class for
    // what this saves). Reads the pbr::MaterialConstants block every material-shaded draw carries;
    // `bytes` too small to be one means an unrecognised draw, answered conservatively (yes). .cpp.
    bool blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const override;

    // Returns the lit pipeline for this frame, or 0 to use the backend's own. `depthPrepassed` selects
    // the LessEqual/no-write depth-state variant for an instance depthPrepassPipeline() already wrote
    // depth for -- see createScenePipelines() for how the two agree. `blended` selects the
    // premultiplied-alpha, depth-write-off twin of the same vsMain/msMain+psVoxi/psRt shading, and
    // takes priority over depthPrepassed (the base class says IDevice::drawMesh never routes a blended
    // draw down the prepass path, so the two never arrive together; answering blended first avoids
    // reasoning about that combination). meshShaders IS honoured for a blended draw (its own mesh-
    // shader blended pipeline, not a dropped draw). The wireframe view never asks: the device draws
    // those meshes itself (IDevice::setWireframe).
    rhi::PipelineHandle scenePipeline(bool meshShaders, bool depthPrepassed = false,
                                      bool blended = false) const override;
    // The depth-only prepass pipeline: VSMain (same as scenePipeline()'s non-mesh-shader variants)
    // paired with PSDepthPrepass (alpha-tests/clips, writes no colour -- see VoxiShaders.hpp). 0
    // until createScenePipelines() runs once, and 0 forever if compile fails -- drawMeshDepthPrepass
    // degrades to a no-op, like every other optional Voxi pipeline (mesh-shader, ray-traced).
    rhi::PipelineHandle depthPrepassPipeline() const override;

    // True while the debug view replaces the scene, including the backend's line draws.
    rhi::BindlessTableHandle sceneBindlessTable() const override;
    bool suppressesScene() const override;
    // Only the debug raymarch owns the whole frame; ray-driven mode only replaces how the first
    // surface is found -- sky, gizmos, particles are as real as in a rastered frame. See
    // IRenderFeature::suppressesWholeFrame.
    bool suppressesWholeFrame() const override;
    // Draws the debug raymarch over the already-bound colour target.
    void scenePass(rhi::IRenderContext& ctx) override;

    // Rebuilds the pipelines that bake the sample count and target formats, and resizes the
    // screen-resolution ray-traced shadow history (see rtShadowHist_).
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    // The GPU residency of the material library, so the app can ask for a draw's binding set.
    pbr::MaterialSystem& materials() { return materials_; }

    // ---- modular seam: lets a foreign pipeline merge Voxi's table 0 into its own (Stage 3, GPU
    // per-cluster shading parity -- HLSL half in VoxiGiShaders.hpp, the one real caller is
    // SandboxApp.cpp's ensureLodMeshPipeline). Neither side learns the other's internals; they agree
    // only on a shape (kGiSrvCount/kGiUavCount) and a base register. ----

    // Writes the GI volume and shadow map into `set` at srvBase/srvBase+1 -- see giShaderDefines()
    // for why only these two of the table-0 union get a real descriptor. `res` is the caller's own
    // IResourceFactory; nothing here reads res_ or assumes it matches init()'s.
    void bindGiResources(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase) const;

    // This frame's `cbuffer VoxiFrame` bytes -- same block sceneConstants() hands the backend, byte
    // for byte what giShaderPrelude() (VoxiGiShaders.hpp) declares. Bind at the b-register passed to
    // giShaderDefines()'s frameConstantRegister; no change notification, only "read before you draw".
    const void* giFrameConstants() const { return &cb_; }
    u32 giFrameConstantBytes() const { return sizeof(cb_); }

    // ---- INSTANCED FOLIAGE: ray-traced only, outside the draw list ----
    // A level's foliage (Runtime's loadLevelFoliage, from .ocinst files) is up to millions of static
    // instances of a few prototypes -- far past kMaxDraws, and nothing a draw is needed for. It is
    // TRACED (primary visibility, sun shadow, reflections, GI, sky occlusion) and nothing else: never
    // rasterised, voxelised, shadow-mapped, lit by lamps or path traced, never in draws_, no collision,
    // not selectable. Each prototype becomes ONE multi-geometry BLAS (rhi::IResourceFactory::
    // createBlasMulti -- a geometry per material part, alpha-masked parts non-opaque), each instance ONE
    // TLAS instance in the TLAS's static prefix (setTlasStaticInstances), packed and uploaded once here:
    // nothing per frame grows with the instance count. Needs ray tracing; without it the set is refused.
    struct FoliagePart {
        rhi::MeshHandle mesh = 0;    // one material part's GPU mesh (GameContent::MeshPart::mesh, or the whole mesh when it has no parts)
        u32 material = 0;            // pbr::MaterialLibrary handle (GameContent::authoredFor(token)); 0 = none
        f32 color[4] = {1, 1, 1, 1}; // resolved look (as game::drawWorld's resolveDrawLook computes it) for when material is 0 / not built
        f32 metallic = 0.0f, roughness = 1.0f;
    };
    // Parts in GeometryIndex() order; at most kMaxFoliagePartsPerPrototype (a hit carries the part in 4
    // bits -- see AVER_RT_REF_GEOM_SHIFT, voxi_rt.hlsli). The caller drops blended/translucent parts.
    struct FoliagePrototype { std::vector<FoliagePart> parts; };
    // world: the engine's row-vector world matrix minus its (0,0,0,1) 4th column, world[r*3 + c] =
    // M[r][c] -- rows 0..2 the scaled basis, row 3 the translation, cm, engine world space. prototype:
    // index into setFoliage's prototypes vector.
    struct FoliageInstance { f32 world[12]; u32 prototype; };
    static constexpr u32 kMaxFoliagePartsPerPrototype = 16;
    static constexpr u32 kMaxFoliageInstances = voxi::kMaxFoliageInstances;
    // Replaces the level's foliage (clearFoliage first). Parts with no mesh, prototypes whose BLAS
    // cannot be made and instances naming such a prototype are dropped and counted in the log line;
    // instances past kMaxFoliageInstances are truncated with a warning. The BLASes are built and the TLAS
    // rebuilt on the next frame's acceleration-structure pass. A mesh a part names must outlive the set:
    // call clearFoliage before destroying it (a set found naming a destroyed mesh is dropped whole).
    void setFoliage(std::vector<FoliagePrototype> prototypes, std::vector<FoliageInstance> instances);
    // Removes the foliage and releases everything it held: BLASes, the TLAS prefix, the part table.
    void clearFoliage();
    struct FoliageStats { u32 prototypes = 0, parts = 0, instances = 0; u64 gpuBytes = 0; };
    // What the current set holds: prototypes/parts/instances that made it into the TLAS, and the GPU
    // bytes it added (BLASes, the TLAS growth plus prefix, the part table ring).
    FoliageStats foliageStats() const;

private:
    // Creates the cascaded shadow atlas.
    bool createShadowResources();
    // Creates the radiance volume, the injection accumulator and every binding set over them.
    bool createVoxelVolume(u32 resolution);
    // W12: just the injection accumulator (voxelAccumTex_), factored out of createVoxelVolume so
    // manageInjectionAccumulator() can recreate it after a free without duplicating desc/logging.
    bool createInjectionAccumulator(u32 resolution);
    // W12: recreates or frees the injection accumulator this frame (see .cpp for the two branches);
    // must run before anything else this frame binds bindings_ (see prePass()).
    void manageInjectionAccumulator(rhi::IRenderContext& ctx);
    // Creates every pipeline the feature runs.
    bool createPipelines();
    // Creates the subset that bakes sample count and render-target formats -- two pipelines per
    // combination now, instead of one (see the "Gbuf" members below).
    bool createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
    // Picks `gbuf` when it exists AND dev_->gBufferEnabled(), else `plain`. Asks gBufferEnabled()
    // itself rather than taking a parameter, since IRenderFeature::scenePipeline's signature wasn't
    // widened for this. `plain` is the safe fallback: init() requires scenePso_ (the one pipeline with
    // no "or 0" in its own contract) non-zero already.
    rhi::PipelineHandle pickGbuf(rhi::PipelineHandle plain, rhi::PipelineHandle gbuf) const;
    // Renders the replayed draw list into each cascade of the shadow atlas.
    void shadowPass(rhi::IRenderContext& ctx);
    // The GI-only depth pass: one box over the GI volume, run on the frames voxelizePass runs.
    void giShadowPass(rhi::IRenderContext& ctx);
    // Builds a BLAS per referenced mesh and one TLAS over the replayed draw list. Gated on
    // rtAccelSnapshotUnchanged() by Settings::rtSkipUnchangedTlas -- see "THE UNCHANGED GATE" below.
    void buildAccelerationStructures(rhi::IRenderContext& ctx);
    // Clears the accumulator, rasterises the scene into the volume with direct light, resolves it.
    void voxelizePass(rhi::IRenderContext& ctx);
    // Box-filters each mip of the volume into the next.
    void filterMips(rhi::IRenderContext& ctx);
    // Fog occlusion: creates/releases airVisTex_ on airVisWanted()'s edge -- same "wanted() decides,
    // ensure*() acts" shape as ensureShadowHistory/ensureRdStagedResources. Called once from init()
    // (after createPipelines() settles whether CSAirVis compiled -- airVisWanted() can't be answered
    // honestly before that) and again from setSettings().
    bool ensureAirVis();
    // Fog occlusion: binds airVisPso_ and dispatches CSAirVis over z-layers [zLo, zHi) (whole volume
    // or one round-robin slab). Only caller is prePass(), which wraps it with voxelTex_'s
    // ShaderResource->NonPixelShaderResource->ShaderResource round trip (CSAirVis reads t0 from
    // compute); this only transitions airVisTex_ itself (ShaderResource<->UnorderedAccess around the
    // dispatch). Clears airVisDirty_ when the dispatch covers everything.
    void dispatchAirVis(rhi::IRenderContext& ctx, u32 zLo, u32 zHi);
    // Staged ray-driven passes: records CSRdVisibility, then CSRdShadow/CSRdGi/CSRdSkyOcc (last two
    // conditional) with no barrier between them (they write disjoint resources -- see .cpp for the
    // GPU spans), then the AVER_RD_SPLIT fullscreen draw. Called from scenePass() only once
    // rdStagedActive() says yes; every decision to call this lives there, not here.
    void recordStagedRayDriven(rhi::IRenderContext& ctx);

    pbr::MaterialSystem materials_;

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::DeviceCaps caps_{};
    Settings settings_{};

    rhi::TextureHandle shadowTex_ = 0;
    rhi::PipelineHandle shadowPso_ = 0;
    // Same depth-only pass via drawMeshInstanced instead of one drawMesh() per draw -- see
    // shadowPass()'s cascade grouping and VSShadowInstanced (VoxiShaders.hpp). 0 if compile failed;
    // shadowPass() falls back to shadowPso_ automatically.
    rhi::PipelineHandle shadowInstancedPso_ = 0;

    // GI-only shadow map: fitted to the GI volume, not the camera, so the cascades above stay
    // camera-fitted. Rendered only on frames voxelizePass runs (PSVoxel is its only reader). A zero
    // handle is a soft failure: giShadowFactor falls back to fully-lit indirect rather than failing
    // init() -- wrong-but-running beats a dead renderer for a bounce-light-only term.
    rhi::TextureHandle  giShadowTex_ = 0;
    rhi::PipelineHandle giShadowPso_ = 0, giShadowInstancedPso_ = 0;

    // Radiance volume: RGBA16F Tex3D, full mip chain. Mip N is the cone footprint at distance N.
    rhi::TextureHandle  voxelTex_ = 0;
    // Injection accumulator: R32_UINT Tex3D, (res*4) x res x res, channels interleaved along x.
    rhi::TextureHandle  voxelAccumTex_ = 0;
    rhi::PipelineHandle voxelPso_ = 0, voxelMsPso_ = 0, mipPso_ = 0, clearPso_ = 0, debugPso_ = 0;
    rhi::PipelineHandle resolvePso_ = 0;
    // Fog occlusion volume: kAirVisResolution^3, single-mip, R16F (or smallest UAV-storable
    // single-channel format). Bound at t17 (SRV) / u16 (UAV) in bindings_ -- see
    // kVoxiSrvCount/kVoxiUavCount (VoxiRenderer.cpp). Rests in ShaderResource between frames like
    // voxelTex_; dispatchAirVis() is the only place that flips it to UnorderedAccess. Created with
    // the voxel volume when Settings::fogOcclusion is on, released on the off edge (ensureAirVis()).
    rhi::TextureHandle  airVisTex_ = 0;
    // 1x1x1 stand-in for airVisTex_, bound at both t17 and u16 when the real texture doesn't exist
    // (fogOcclusion off, or CSAirVis didn't compile) -- same "Tier 1 needs a valid descriptor" reasoning
    // as voxelAccumPlaceholder_/u1, serving two slots since t17/u16 are the same resource's two views.
    // Created in createVoxelVolume(), destroyed in shutdown().
    rhi::TextureHandle  airVisPlaceholder_ = 0;
    // CSAirVis. Optional: needs SM 6.0 and DXC (see instancedShadowsOk in createPipelines()) -- a
    // device without either keeps the placeholder bound permanently and fogOcclusion has no effect.
    rhi::PipelineHandle airVisPso_ = 0;
    // True from airVisTex_'s (re)creation until CSAirVis has written it once -- a fresh volume holds
    // whatever the device handed back, and the GI rebuild gate can skip 96-98% of ticks, so waiting
    // for the next rebuild could leave garbage bound at t17. prePass() fills the whole volume in one
    // dispatch while set, then falls back to the per-frame slab round-robin.
    bool airVisDirty_ = false;
    // The next slab prePass()'s round-robin refreshes, in units of kAirVisSlabLayers z-layers.
    u32 airVisSlab_ = 0;
    // Whether last frame ran the air-vis refresh at all (voxel GI on, volume and pipeline present):
    // a rising edge marks the volume dirty, since geometry may have changed while nothing refreshed it.
    bool airVisWasActive_ = false;
    // t0 volume (whole chain), t1 shadow, t2 TLAS, u0 volume mip 0, u1 injection accumulator.
    rhi::BindingSetHandle bindings_ = 0;
    // UAVs only (u0 volume mip 0, u1 accumulator): no SRV may be live while the clear runs.
    rhi::BindingSetHandle clearBindings_ = 0;
    // Same shape as the clear set: the accumulator-to-volume reduction.
    rhi::BindingSetHandle resolveBindings_ = 0;
    // One per mip filter step: set m reads mip m-1 and writes mip m.
    std::vector<rhi::BindingSetHandle> mipBindings_;

    rhi::PipelineHandle scenePso_ = 0, sceneMsPso_ = 0, sceneRtPso_ = 0, sceneMsRtPso_ = 0;
    // Blended twins of the four handles above: same compiled vsMain/msMain/psVoxi/psRt binaries, a
    // GraphicsPipelineDesc identical to the opaque variant except blend = PremultipliedAlpha and
    // depth.write = false -- premultiplied so PSMainVoxi's blended branch can return full-strength
    // specular plus a coverage-weighted diffuse instead of attenuating both by the same alpha (see
    // that branch in VoxiShaders.hpp). Shading (shadow lookup, cone-traced indirect) is the same code
    // path an opaque draw gets; casting a shadow, appearing in a reflection hit and injecting
    // radiance are not, since submitDraw() drops a blended draw before it reaches draws_ (deliberate
    // now; before `blended` existed as a submitDraw parameter every draw was opaque by construction,
    // so the exclusion was accidental). All four exist (mirroring the mesh-shader x ray-tracing axes)
    // because, unlike depthPrepassPso_'s twins, there is no single known caller to narrow the set
    // against.
    rhi::PipelineHandle sceneBlendedPso_ = 0, sceneMsBlendedPso_ = 0, sceneRtBlendedPso_ = 0,
                        sceneMsRtBlendedPso_ = 0;
    // Ray-driven primary-visibility pass: one fullscreen triangle whose pixel shader traces the
    // camera ray itself. Null unless the device has ray queries (PSRayDriven only compiles into the
    // SM 6.5 AVER_RT variant).
    rhi::PipelineHandle rayDrivenPso_ = 0;
    // Depth prepass and its two "already prepassed" scene-colour twins -- see depthPrepassPipeline().
    // No mesh-shader twins: the prepass is only offered to the plain drawMesh() path (SandboxApp.cpp),
    // so scenePipeline() never needs a prepassed sceneMsPso_/sceneMsRtPso_ variant.
    rhi::PipelineHandle depthPrepassPso_ = 0;
    rhi::PipelineHandle scenePsoPrepassed_ = 0, sceneRtPsoPrepassed_ = 0;

    // ---- G-buffer twins: one more axis alongside mesh-shader x ray-tracing x blended x
    // depth-prepassed, not a runtime boolean threaded through scenePipeline() ----
    //
    // Every scene-lit pipeline above (the eight opaque/blended combinations, the two prepassed ones,
    // and rayDrivenPso_) gets a twin here: identical vertex/mesh-shader stage and fixed-function
    // state, but a pixel shader recompiled with AVER_GBUFFER=1 (changing its RETURN TYPE per
    // VoxiShaders.hpp's #if AVER_GBUFFER blocks, GBufferOut/RayDrivenGBufferOut) and renderTargetCount
    // 4 instead of 1 -- built in createScenePipelines()'s "Gbuf" step. A PSO's render-target count/
    // formats are fixed at creation on both backends, so this needs a second pipeline, not a branch
    // inside one. The three extra formats are fixed constants (RG16F/R32Float/RGB10A2Unorm,
    // SV_TARGET1/2/3, already fixed by IDevice::gBufferVelocityTexture()/gBufferViewZTexture()/
    // gBufferNormalRoughnessTexture() in RHI.hpp), not derived from onRenderTargetsChanged, since they
    // are a property of the G-buffer feature, not the swapchain/MSAA state. All optional exactly like
    // their non-Gbuf twins (0 = couldn't build: older shader model, no DXC, a compile failure --
    // pickGbuf() falls back to plain, never losing the base pipeline); built unconditionally alongside
    // the plain twins, gated on the same device caps (msOk/rtOk) -- runtime on/off is pickGbuf()'s
    // question (dev_->gBufferEnabled()), asked fresh every call, so flipping the switch needs no
    // rebuild.
    rhi::PipelineHandle sceneGbufPso_ = 0, sceneMsGbufPso_ = 0, sceneRtGbufPso_ = 0, sceneMsRtGbufPso_ = 0;
    rhi::PipelineHandle scenePsoPrepassedGbuf_ = 0, sceneRtPsoPrepassedGbuf_ = 0;
    // PSRayDriven's own twin (RayDrivenGBufferOut: same three channels plus SV_DEPTH, written by
    // this pass itself). Read only from scenePass(), never scenePipeline() -- ray-driven mode isn't
    // selected through that contract at all (see rayDrivenActive()/suppressesScene()).
    rhi::PipelineHandle rayDrivenGbufPso_ = 0;

    // Created updatable (createTlasUpdatable) only while Settings::rtRefitAccel reads true at init();
    // plain createTlas otherwise. See refitOrRebuildTlas. Sized for kMaxDraws draws AFTER a static
    // prefix holding the foliage (setFoliage), which grows and shrinks the structure with the set -- the
    // draws' instance ids stay their dense index whatever the prefix holds.
    rhi::TlasHandle tlas_ = 0;
    // Translucent-lane (kRtMaskTranslucent) instances in what tlas_ HOLDS -- set by refitOrRebuildTlas,
    // the one place tlas_ is written, so a frame the unchanged gate skips keeps the count of the build
    // it skipped to. Zero turns on gGiShadowParams.w bit 32 in prePass: the primary sun shadow
    // (rtShadowEx) traces its first-hit query instead of the transmittance walk, since with nothing to
    // tint the walk can only answer 0 or 1 (see rtShadowEx's FIRST-HIT FAST PATH, voxi_rt.hlsli).
    u32 rtTlasTranslucent_ = 0;
    // Which way the fast path was last announced: 0 never, 1 on, 2 off -- one line per change, not
    // one per frame.
    u8 rtShadowFirstHitLogged_ = 0;
    // One BLAS per referenced mesh, kept for the run, except a mesh whose vertices are compute-
    // written (IDevice::meshVertexBuffer, gated on GpuMesh::computeWritten), which is refreshed every
    // frame -- created via createBlasUpdatable (only while Settings::rtRefitAccel was on at the time
    // this mesh's BLAS was first built; plain createBlas otherwise), and refit in place (ctx.refitBlas)
    // or fully rebuilt (ctx.buildBlas) every tick by refitOrRebuildDynamicBlas, honouring Settings::
    // rtRefitAccel and the periodic kDynamicBlasRefitsPerRebuild rebuild below. A static mesh's own
    // BLAS is always created via plain createBlas and never touched again once built. FIXED: used to gate on meshVertexBuffer
    // returning GpuMesh::vbBuffer, true of every mesh since a3022e0, forcing a needless per-frame
    // rebuild on ordinary static geometry; D3D12Device.cpp now gates on computeWritten, set only by
    // createSkinTargetMesh and cleared by destroyMesh.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blas_;
    bool dynamicBlasLogged_ = false;   // the per-frame refresh is announced once, not every frame
    // Meshes already refreshed during this frame's pass over the draw list -- avoids refitting/
    // rebuilding a mesh drawn by two instances twice (wasted work + UAV barrier). Kept as a member so
    // the allocation is made once, not every frame.
    std::vector<rhi::MeshHandle> rebuiltThisFrame_;
    // Structures rebuilt/refit in the last pass over the draw list. Logged only on change (a line
    // every frame would be noise).
    u32 lastBlasRebuilds_ = 0xFFFFFFFFu;

    // ---- dynamic (compute-skinned) BLAS refit path (Settings::rtRefitAccel) ----
    // Distinct compute-written meshes that made it into the LAST FULL BUILD with a valid BLAS --
    // repopulated every full build, by the per-draw loop itself (covers a mesh seen for the first
    // time that build too, unlike rebuiltThisFrame_ above). Read by refitDynamicAccelStructures()
    // (the gate's refit-only pass below) to find the dynamic BLASes/vertex slices it must refresh
    // with no per-draw loop of its own to rediscover them from.
    std::vector<rhi::MeshHandle> rtDynamicMeshes_;
    // Consecutive refitBlas() calls since this mesh's BLAS was last fully rebuilt -- a refit traces a
    // little worse the further the pose has drifted from the build it refit from, so this forces a
    // fresh ctx.buildBlas every kDynamicBlasRefitsPerRebuild ticks regardless of Settings::
    // rtRefitAccel staying on the whole time. Keyed by mesh rather than folded into blas_ since most
    // meshes never refit at all.
    std::unordered_map<rhi::MeshHandle, u32> dynamicBlasRefits_;
    static constexpr u32 kDynamicBlasRefitsPerRebuild = 30;
    // Refits `blas` (mesh's CURRENT vertices) when Settings::rtRefitAccel allows it and this mesh's
    // own streak hasn't hit kDynamicBlasRefitsPerRebuild; otherwise a full ctx.buildBlas, which also
    // resets the streak. Shared by the per-draw loop's cached-dynamic-mesh branch and
    // refitDynamicAccelStructures() so both spend against the same per-mesh budget.
    void refitOrRebuildDynamicBlas(rhi::IRenderContext& ctx, rhi::BlasHandle blas, rhi::MeshHandle mesh);
    // Same shape as refitOrRebuildDynamicBlas, for tlas_: refits when Settings::rtRefitAccel allows it
    // and tlasRefitStreak_ hasn't hit kTlasRefitsPerRebuild, else a full ctx.buildTlas (which resets
    // the streak). Shared between buildAccelerationStructures' full-build tail and the gate's
    // refit-only pass below -- one streak, so alternating between the two still rebuilds on schedule.
    // Returns what ctx.refitTlas returned (true = refit in place).
    bool refitOrRebuildTlas(rhi::IRenderContext& ctx);
    u32 tlasRefitStreak_ = 0;
    static constexpr u32 kTlasRefitsPerRebuild = 30;
    // Lifetime counts for the "bottom-level builds" line and the gate report -- see
    // reportRtAccelGate() and the log at the end of buildAccelerationStructures.
    u64 rtDynamicBlasRefits_ = 0, rtDynamicBlasRebuilds_ = 0;
    u64 rtTlasRefits_ = 0, rtTlasRebuilds_ = 0;
    // The gate's own refit-only pass: refits every dynamic BLAS and tlas_ in place (or rebuilds them,
    // on their own periodic schedule), then refreshes their rtVerts_ slices -- no per-draw loop.
    // Runs from buildAccelerationStructures' skip branch when the gate would otherwise plainly skip
    // but Settings::rtRefitAccel is on and rtDynamicMeshes_ is non-empty, and from the mover-patch
    // branch when a dynamic mesh exists (it ends in the same TLAS refit the patch needs).
    void refitDynamicAccelStructures(rhi::IRenderContext& ctx);

    // ---- THE UNCHANGED GATE (Settings::rtSkipUnchangedTlas): skip a rebuild that would be
    // bit-identical to what's already in tlas_/rtInstanceData_ ----
    //
    // Same trick as the GI rebuild gate further down (giSnapshotUnchanged/giDrawsKey/takeGiSnapshot,
    // modelled line for line): hash what the per-draw loop reads from drawsPrev_, and if nothing
    // moved, leave tlas_/rtInstanceData_/their SRVs alone -- except that a movable draw's transform is
    // not hashed and is patched in instead (THE MOVER PATCH LANE, below). Measured 0.42 ms/frame on the owner's
    // static NewSponza scene (see Settings::rtSkipUnchangedTlas in Voxi.hpp) -- what that buys is a
    // from-scratch ctx.buildTlas plus an unconditional instance-buffer rewrite/upload every frame
    // regardless of motion.
    //
    // rtAccelSnapshotUnchanged() (mirrors giSnapshotUnchanged()) calls rtAccelMustForceRebuild()
    // first (conditions no key can make safe to skip: a compute-skinned mesh present, or a cached
    // BLAS the resource factory no longer attributes to its mesh) then rtAccelDrawsKey() (mirrors
    // giDrawsKey(): commutative hash, since occlusion culling reorders drawsPrev_ every frame).
    // takeRtAccelSnapshot() (mirrors takeGiSnapshot()) runs after every build that actually happened,
    // gate on or off.
    bool rtAccelSnapshotUnchanged() const;
    bool rtAccelMustForceRebuild() const;
    // Per-draw material hash is hashDrawMaterialInto(), shared with giDrawsKey().
    // The gate computes this once per frame and takeRtAccelSnapshot() would compute the SAME key again
    // straight after a rebuild -- at ~42,000 draws that second pass is most of a millisecond for nothing.
    // The draw-list half (the expensive one) is therefore remembered in rtAccelListKey_ when computed,
    // and `reuseListKey` asks for that copy instead of a recompute. Only the list half is reused: the
    // foliage term is a handful of parts, and resolveFoliageMaterials() runs inside the build between
    // the gate and the snapshot (it can add a material entry foliageKey() reads), so the snapshot must
    // still see foliage as the build LEFT it, exactly as before.
    u64  rtAccelDrawsKey(bool reuseListKey = false) const;
    void takeRtAccelSnapshot();
    // One-time-per-reason "why" log plus the widening-interval "N rebuilt / M refit-only / M skipped"
    // report (mirrors the GI gate's own in prePass()) -- one method since the plain-skip branch, the
    // refit-only branch and a real build's tail must all reach the same report.
    void reportRtAccelGate();
    // cb_.rtParams[0..2] (sun angular size as tangent, shadow ray count, ray bias): not a function
    // of drawsPrev_, so a skipped frame still needs them set -- shadowPass()/PSRayDriven read cb_
    // every frame regardless. Factored out so the skip branch doesn't duplicate it.
    void updateRtParamsPerFrame();

    u64  rtAccelKey_ = 0;
    // rtAccelDrawsKey()'s draw-list half as last computed, and whether it was computed THIS build.
    // buildAccelerationStructures() clears the flag on entry, so a copy from an earlier frame can never
    // be reused: it is only ever read by takeRtAccelSnapshot() in the same call that the gate filled it.
    mutable u64  rtAccelListKey_ = 0;
    mutable bool rtAccelListKeyValid_ = false;
    // rtAccelMustForceRebuild()'s "already checked this mesh" filter: direct-mapped, mesh handles hashed
    // into it, zeroed at the start of every call (0 is never a submitted mesh -- submit() drops it). A
    // collision only makes a mesh get checked again, never skipped. 16384 four-byte slots hold the
    // ~3,000 distinct meshes of a large level with few collisions; a member so the 64 KB is not
    // re-allocated every frame.
    static constexpr u32 kRtAccelMeshCheckSlots = 16384;
    static_assert((kRtAccelMeshCheckSlots & (kRtAccelMeshCheckSlots - 1)) == 0,
                  "kRtAccelMeshCheckSlots must be a power of two for the '& (kRtAccelMeshCheckSlots - 1)' mask");
    mutable std::vector<rhi::MeshHandle> rtAccelMeshChecked_;
    // False until the first successful build (mirrors giSnapExtent_'s negative-means-unset shape, as
    // a separate bool, not a sentinel, since 0 is a legal key).
    bool rtAccelSnapValid_ = false;
    // Ticks the gate ran; report only. rtAccelRefitOnly_ is a match that ran the lighter refit-only
    // pass (Settings::rtRefitAccel, rtDynamicMeshes_ non-empty) rather than a plain skip -- see
    // buildAccelerationStructures' skip branch. rtAccelMoverPatched_ is a match whose movable draws had
    // moved and were patched into tlas_/rtInstanceData_ by the mover patch lane below.
    u64  rtAccelSkipped_ = 0, rtAccelRebuilt_ = 0, rtAccelRefitOnly_ = 0, rtAccelMoverPatched_ = 0;
    mutable u32 rtAccelGateWhyMask_ = 0;   // one bit per rejection reason already reported, ever
    u64  rtAccelGateNextReport_ = 64;      // doubles each time, so steady state gets reported too
    u64  rtAccelGateLastTicks_ = 0, rtAccelGateLastSkipped_ = 0, rtAccelGateLastRefitOnly_ = 0,
         rtAccelGateLastMoverPatched_ = 0;

    // ---- THE MOVER PATCH LANE: a moving draw no longer forces the whole per-draw loop ----
    // In a Play session a level's route-animated props, the pawn and its viewmodel move every frame, and
    // rtAccelDrawsKey() used to hash every draw's world matrix, so ONE moving draw rejected the gate and the
    // full per-draw loop, the material and geometry tables and a TLAS pack ran over every draw (about 51,000
    // in NeonDistrict: an ESTIMATED ~25 ms of CPU a frame, UNMEASURED). PlayMobility already marks the
    // draws that move (Draw::movable), so for those the key leaves the world matrix OUT and keeps
    // everything else (mesh, flags, material, the movable bit itself -- see rtDrawHash). The gate then
    // still matches on a pure transform change, and this lane delivers the transforms instead:
    //   - every full build records, per movable draw, the instance it became (rtMovers_);
    //   - on a gate hit, patchRtMovers() writes each mover's CURRENT world into tlasInstScratch_ (what
    //     tlas_ is refit from) and rtInstanceData_ (what a ray hit reads), re-uploads the instance table,
    //     and the caller refits tlas_ (refitOrRebuildTlas, same periodic full rebuild as ever).
    // The world matrix feeds exactly those two places (TlasInstance::world, RtInstance::objectToWorld)
    // plus RtInstance::prevObjectToWorld, which the patch fills from the instance's old objectToWorld as
    // it overwrites it (and which rtPrevPending_ settles back to equal on a later frame, so a mover that
    // stops stops reporting motion). Instance bounds, material rows, geometry slices and the instance
    // mask/flags are not functions of the world, and the key still gates them.
    // Needs the gate and Settings::rtRefitAccel both on (rtMoverPatchActive); with either off the key
    // hashes every world as before, which is the old behaviour bit for bit.
    //
    // A recorded mover is matched to the CURRENT list by IDENTITY (rtDrawHash without the world), not by
    // draw index: the key is a commutative sum, so a reshuffled draw list (occlusion culling) still
    // matches it, and an index would then name the wrong draw. Two movers with equal identity share
    // mesh, material set and flags, so which of them takes which instance is free: patchRtMovers() breaks
    // the tie by NEAREST translation to the instance's current world (not draw order, which occlusion
    // culling reshuffles -- two identical cars would swap transforms and each report the other's motion).
    // The choice never changes the structure's content, only which instance carries which world.
    // Not covered: an authored draw's per-draw colour/metallic/roughness are not in its identity, so a
    // per-draw tint on a mover (only the show-culled debug view does that today) keeps its last
    // full-build value until the next one.
    bool rtMoverPatchActive() const;
    static constexpr u32 kRtNoInstance = 0xFFFFFFFFu;
    struct RtMover {
        u64 id = 0;                  // rtDrawHash(): everything the per-draw loop reads but the world
        u32 draw = 0;                // index into drawsPrev_ (the list the entry was taken from)
        u32 inst = kRtNoInstance;    // index into tlasInstScratch_/rtInstanceData_; none when no BLAS
        bool operator<(const RtMover& o) const { return id != o.id ? id < o.id : draw < o.draw; }
    };
    // The LAST FULL BUILD's movers, sorted. Empty when the lane is off, or no mover became an instance.
    std::vector<RtMover> rtMovers_;
    // THIS frame's movable draws, collected by rtAccelDrawsKey()'s pass over drawsPrev_ (it already
    // visits every draw) and sorted by patchRtMovers(). Valid exactly while rtAccelListKeyValid_ is.
    mutable std::vector<RtMover> rtMoversNow_;
    enum class MoverPatch : u8 {
        Refused,     // the movers no longer line up with the recorded instances: run the full build
        Unchanged,   // every mover is exactly where tlas_ has it: nothing to write
        Patched,     // at least one transform was rewritten and the instance table re-uploaded
    };
    // Verifies rtMoversNow_ against rtMovers_ (all of it, before touching anything), then writes the
    // moved worlds. Does NOT refit tlas_ -- the caller does, once, together with any dynamic BLAS.
    MoverPatch patchRtMovers();

    // ---- W10: buildAccelerationStructures' per-build scratch, hoisted out (used to be two locals,
    // `inst` and `matConstantsByKey`, reallocated from empty every build) to avoid a heap
    // allocation every frame for a container whose SHAPE barely changes build to build. .clear()'d
    // at the top of buildAccelerationStructures (keeps storage); only a build past the previous
    // high-water mark reallocates, same reasoning as rebuiltThisFrame_ above. ----
    std::vector<rhi::TlasInstance> tlasInstScratch_;
    std::unordered_map<u64, pbr::MaterialConstants> matConstantsScratch_;

    // M2(c): CPU cost of buildAccelerationStructures' per-draw loop -- see lastAccelBuildCpuMs().
    f64 lastAccelBuildCpuMs_ = 0.0;
    // Occlusion rays per pixel toward the sun's disc. 4 by default: 1 gives a hard aliased edge, and
    // cost is linear, so this is the first knob to turn down if ray tracing costs too much (recorded
    // TDR history on this machine keeps the default low). setShadowRays and --rt-rays move it. A
    // ceiling -- rays actually fired (rtShadowRaysUsed_) follow the sun disc's size, see
    // updateRtParamsPerFrame.
    u32 rtShadowRays_ = 4;
    u32 rtShadowRaysUsed_ = 2;
    // Tile edge for the shadow's temporal amortisation. See setPixelsPerRayTile / Settings for the
    // contract; 1 traces every pixel every frame.
    u32 rtPixelsPerRayTile_ = 1;
    // The SPATIAL filter radius, mirrored from Settings::rtShadowDenoise by applySettings.
    u32 rtShadowDenoise_ = 0;
    // How fast the spatial filter tapers with reprojection velocity. 0 = no taper. A measurement
    // knob, not a tier setting -- temporal accumulation fixes the motion flicker; this isolates the
    // spatial filter's own share of it at runtime.
    f32 rtDenoiseMotionTaper_ = 0.0f;
    // Settings::rtRenderMode, cached at setSettings. 1 asks for ray-driven primary visibility;
    // whether it's honoured is rayDrivenActive() (also needs device/pipeline support).
    u32 rtRenderMode_ = 0;
    // Settings::giMode, cached at setSettings. Whether it's honoured is giRestirWanted() (also needs
    // ray tracing wanted); this is just "what was asked for" -- read by ensureShadowHistory to decide
    // whether to (re)build giReservoirs_/giSurfPosHist_/giSurfNrmHist_, and by prePass for cb_.
    u32 giMode_ = 0;
    // Whether the coat lobe is compiled into this process's material pipelines. Latched at the first
    // setSettings and never again -- this renderer builds ~20 raster PSOs at init through DXC with no
    // disk cache, so a later change would mean recompiling all of them mid-session. A project-level
    // decision; changing it needs a project reload. See voxi::Settings.
    bool layeredBsdf_ = false;
    bool layeredBsdfLatched_ = false;
    // Once per process, not once per frame: the mismatch is a standing condition for the rest of the
    // session, so logging it every applySettings call (measured ~30 copies in an ordinary run) is
    // just noise. Same idiom as drawCapReported_ below.
    bool layeredBsdfWarned_ = false;
    // Settings::ptBounces. Spent only while pathTracingWanted() -- see where cb_.ptBounceParams
    // is filled, which is the one place that decision is made.
    u32 ptBounces_ = 1;
    // Frames apart the GI volume rebuilds. See setGiUpdateInterval/Settings; 1 rebuilds every frame.
    // Checked against rtFrameIndex_ in prePass().
    u32 giUpdateInterval_ = 1;
    // Advances once per prePass() call, unconditionally -- a count of simulated frames, never wall-
    // clock, so the shadow's trace schedule and disc-sample rotation are a deterministic function of
    // frame number (keeps a fixed --frames run reproducible), unlike rtHash alone, which is
    // spatial-only -- see rtHash's own comment (VoxiShaders.hpp).
    u32 rtFrameIndex_ = 0;

    // ---- the frame-period sampler ----
    // Measures the wall-clock period between successive prePass calls (the WHOLE frame -- editor UI,
    // the voxelise pass, the post chain -- not just Voxi's share; a CPU period, so only tracks GPU
    // cost while GPU-bound). Run with --no-vsync or every reading is the refresh interval. Exists so
    // "cost is linear in ray count" is a measurement, not an assertion: changing one thing and
    // re-reading the same number is a measurement, a profiler capture that can't be checked into the
    // repo is not.
    // The backdrop handle currently bound at t10; a re-bind only happens when it moves.
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

    // ---- gi-memory: where this renderer's VRAM goes ----
    // One INFO line, printed only when a category below actually changes -- so an owner watching the
    // log sees every allocation and every free (init, a resize, W12's accumulator free/recreate, the
    // GI cache's own free-after-use) without the line repeating once a frame forever after. Compared
    // as raw bytes, never as the MiB the line prints, so a change too small to move the rounded figure
    // still updates the snapshot and a category that hasn't moved never reprints. All seven reset to 0
    // in shutdown(); the radiance volume is the one category that is never legitimately 0 once
    // giReady_ (it exists the moment GI does, at res^3 or larger), so that alone guarantees the first
    // report after any re-init fires, even one that rebuilds every other category at an unchanged size.
    void reportVramUsage();
    // The scene-mesh half of reportVramUsage()'s BLAS total, so that function need not ask the resource
    // factory about every one of blas_'s ~3,000 structures every frame. Re-summed when blasRevision_
    // moved (blas_ gained or lost an entry -- every site that does bumps it) and otherwise every
    // kVramBlasResampleFrames frames: a structure the factory destroyed underneath blas_ (a mesh freed by
    // streaming) drops to 0 bytes with no callback here, so a running total kept only at insert/erase
    // would count it forever. A diagnostic line, so seeing that drop up to a second late costs nothing.
    u32 blasRevision_ = 0;
    u32 vramBlasSampledRevision_ = 0xFFFFFFFFu;   // != blasRevision_ at start: the first call samples
    u32 vramBlasSampledFrame_ = 0;                // rtFrameIndex_ at the last sample
    u64 vramBlasSceneBytes_ = 0;
    static constexpr u32 kVramBlasResampleFrames = 64;
    u64 vramReportedRadianceBytes_ = 0;
    u64 vramReportedAccumBytes_ = 0;
    u64 vramReportedGiCacheBytes_ = 0;
    u64 vramReportedRdBytes_ = 0;
    u64 vramReportedBlasBytes_ = 0;
    u64 vramReportedTlasBytes_ = 0;
    u64 vramReportedFoliagePrefixBytes_ = 0;

    // ---- flat geometry table a reflection ray reads after it hits something ----
    // A hit gives an instance id, primitive index and barycentrics; shading needs the triangle, so
    // every mesh's vertices/indices are concatenated into two buffers with a per-instance record
    // saying where each mesh starts -- three descriptors total, not one per mesh (non-bindless RHI).
    struct RtInstance {
        f32 objectToWorld[16];   // engine row-vector, matching TlasInstance::world
        u32 firstIndex = 0;      // where this mesh's indices start in the flat table
        u32 firstVertex = 0;     // and its vertices
        f32 albedo[3] = {1, 1, 1};
        // The rest of the material a ray can reach: these two used to sit unread in the same Draw
        // albedo is copied from, so a hit shaded as pure chalk (albedo through a Lambertian lobe) --
        // metals in particular came out white instead of dark, since a metal has no diffuse response.
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        // WAS "pad": an unread spare u32. Dense index into this frame's rtMaterials_ (t9,
        // gRtMaterials in VoxiShaders.hpp) -- see buildMaterialTable() for assignment; index 0
        // always means the material system's fallback.
        // Gets a hit real reflectance/f90/flags/emissive from the authored-vs-synthesized split in
        // buildAccelerationStructures. Texture sampling is still absent -- needs an RHI addition
        // (Texture2DArray or similar), not another field here; this only buys the constant-block
        // factors, not the maps.
        //
        // HAND-MAINTAINED ABI: HLSL packs a structured-buffer element by POSITION, not name, so
        // VoxiShaders.hpp's RtInstance mirror must match this field's exact position/name -- a
        // mismatch shifts every field after it and corrupts every ray hit with no compile error.
        // Only the byte-count static_assert below guards against it.
        u32 materialIndex = 0;
        // The instance's objectToWorld AS DRAWN IN THE PREVIOUS FRAME (engine row-vector, same
        // convention as objectToWorld): what the shader pushes an object-space hit through to find
        // where the same surface point was last frame, i.e. the motion vector of a MOVING object.
        // Equal to objectToWorld for anything that did not move -- static props, a new instance, a
        // mover that has stopped (rtPrevPending_ settles it) -- so a zero-motion instance needs no
        // special case on the GPU. Filled by buildAccelerationStructures' carry-forward (a full build)
        // and patchRtMovers (a mover patch); foliage parts and the placeholder carry their own
        // objectToWorld here, foliage being static. Appended AFTER materialIndex so every earlier
        // field keeps its offset; the HLSL mirror (voxi_rt.hlsli) appends the same float4x4.
        f32 prevObjectToWorld[16];
    };
    // 64+4+4+12+4+4+4+64 = 160 bytes, matching the HLSL side under natural alignment; the stride handed
    // to setSrvBuffer must agree too. Three places must agree and the assert only guards two (this
    // struct and the HLSL mirror) -- a wrong stride fails silently: every instance past the first
    // reads its neighbour's bytes, showing up as reflections/hits with the wrong surface's colour.
    static_assert(sizeof(RtInstance) == 160, "RtInstance is the HLSL RtInstance ABI");

    rhi::BufferHandle rtVerts_ = 0, rtIndices_ = 0;
    u32  rtVertCapacity_ = 0, rtIndexCapacity_ = 0, rtInstanceCapacity_ = 0;

    // The instance table is a RING, not one buffer: it lives on the upload heap and is rewritten
    // every frame while the GPU may still be reading the previous frame's copy (writeBuffer is an
    // unsynchronised memcpy) -- without it, a reflection samples a transform mid-motion, smearing
    // instances between two positions, only while ray tracing is on. One buffer per frame in flight
    // breaks the overlap; 3 matches PcgVolume's readback window so it needn't be re-derived if the
    // device ever triple buffers.
    static constexpr u32 kRtInstanceRing = 3;

    // Distinct textures a ray hit can sample. Fixed, not grown on demand -- baked into every root
    // signature that declares it, so growing means a pipeline rebuild. 4096: generous against real
    // projects (ElectricDreams resolves tens) while costing 6% of the shared 65536-descriptor heap.
    // Exhaustion refuses and logs; it does not wrap.
    static constexpr u32 kRtTextureCapacity = 4096;
    rhi::BufferHandle rtInstances_[kRtInstanceRing] = {};
    u32               rtInstanceSlot_ = 0;
    // What the table was built from. Rebuilt only when this changes, because concatenating every
    // mesh every frame would cost more than the reflections do.
    u64  rtGeometryKey_ = 0;
    bool rtGeometryReady_ = false;
    std::vector<RtInstance> rtInstanceData_;
    std::vector<rhi::MeshHandle> rtInstanceMesh_;   // parallel: which mesh each instance draws
    // Distinct meshes those instances name, sorted, plus where each sits in the shared vertex/index
    // table. One entry per mesh, not per instance -- see buildGeometryTable.
    std::vector<rhi::MeshHandle> rtGeomMeshes_;
    std::vector<u32> rtGeomFirstVertex_, rtGeomFirstIndex_;
    // One vertex slice per vertex buffer, not per mesh: an LOD or posed material part shares its
    // root's slice rather than getting a copy. Parallel to rtGeomMeshes_: non-zero where that mesh's
    // copy fills the slice.
    std::vector<u8> rtGeomCopiesVerts_;
    std::unordered_map<u64, u32> rtGeomVertSlice_;   // (vb, vertex count) -> first vertex; scratch

    // ---- per-frame vertex refresh for compute-skinned slices (STALE-POSE FIX) ----
    // buildGeometryTable's own copy loop above only re-copies a slice when the mesh SET changes --
    // right for ordinary geometry, wrong for a compute-written mesh's slice, whose CONTENTS change
    // every frame while its vb/vertex-count identity (and so its place in the "did the set change"
    // key) do not. rdSurfaceFromRecord (voxi.hlsl) rebuilds wpos/N/UV straight from gRtVerts, so a
    // stale slice shades a moving character from whatever pose it had when the slice was first laid
    // out. Independent of Settings::rtRefitAccel -- a correctness fix, not a refit trade.
    struct DynamicVertexSlice {
        rhi::BufferHandle vb = 0;
        u32 vertexCount = 0;
        u32 firstVertex = 0;   // this slice's offset into rtVerts_
    };
    // One entry per compute-written slice this table owns (the subset of rtGeomCopiesVerts_'s fresh
    // slices whose mesh is dev_->meshVertexBuffer() != 0), rebuilt every buildGeometryTable() call
    // alongside rtGeomFirstVertex_/rtGeomCopiesVerts_ -- so it stays correct across a full build, and
    // simply persists (like tlasInstScratch_) for refitDynamicAccelStructures() to replay on a
    // refit-only tick, with no fresh buildGeometryTable() call of its own.
    // COMMITTED ONLY ON SUCCESS: laid out into rtDynamicVertexSlicesPending_ and swapped in when
    // buildGeometryTable() returns true. A layout that fails part-way leaves the OLD rtVerts_ bound, and
    // copying at the new layout's offsets would overwrite other meshes' vertices; the list is cleared
    // instead (the dynamic slices then just keep their last pose until a layout succeeds).
    std::vector<DynamicVertexSlice> rtDynamicVertexSlices_;
    std::vector<DynamicVertexSlice> rtDynamicVertexSlicesPending_;
    // Re-copies every entry above from its mesh's CURRENT compute-written buffer into its rtVerts_
    // slice, with the GeometryRead<->CopySource round trip that state needs. Called after a
    // successful buildGeometryTable() on the full path, and directly (no fresh buildGeometryTable())
    // on the refit-only path -- see buildAccelerationStructures.
    void refreshDynamicVertexSlices(rhi::IRenderContext& ctx);

    // Builds or refreshes the flat table for this frame's draw list. Returns false when it could
    // not be made, which is the signal to fall back to cone-traced reflections.
    bool buildGeometryTable(rhi::IRenderContext& ctx);
    // Writes rtInstanceData_ into the next slot of the rtInstances_ ring (growing the ring first if the
    // table outgrew it) and binds that slot at t5. Shared by buildGeometryTable and the mover patch
    // lane, which re-sends the table after rewriting a few transforms. True when there is nothing to
    // send (foliage-only frame) or the slot is bound; false when a buffer could not be created.
    bool uploadRtInstanceTable();
    bool rtLogged_ = false;

    // ---- dense per-frame material table a ray hit indexes into (t9, gRtMaterials) ----
    // A fresh index assigned every build, not a stable one kept across frames: nothing in
    // pbr::MaterialSystem hands one out (MaterialLibrary's dense enumeration "shift[s] on destroy",
    // Material.hpp; MaterialSystem keeps GPU state in a handle-keyed map sized to "tens, not
    // thousands" of resident materials, MaterialSystem.hpp -- not a dense array). So this class
    // assigns its own index out of THIS build's draw list, the same shape rtGeomMeshes_ uses -- safe
    // because an index is only ever read together with the same build's rtMaterials_ upload.
    //
    // rtMaterials_[kRtInstanceRing]: an upload-heap RING, same reason as rtInstances_ -- writeBuffer
    // is "IMMEDIATE and unsynchronised" (RHIResources.hpp), so overwriting a slot a still-in-flight
    // frame's rays might read is a hazard even on the rare build where material content actually
    // changes -- rare buys a lower chance of hitting the race, not an exemption from it.
    // ---- bindless texture table a ray hit samples through ----
    // One table for the whole scene, append-only for the life of the device (a material's textures
    // are resolved once and live until shutdown). A project loading more than kRtTextureCapacity
    // distinct textures exhausts it; the refusal is logged and affected slots fall back to their
    // factor colour. Keyed on rhi::TextureHandle, not material, so a texture shared by many
    // materials occupies one slot.
    // Makes one texture resident in the table below; see the .cpp for the append-only rule.
    u32  residentTexture(rhi::TextureHandle h);
    // Creates that table on first need, once.
    void ensureTextureTable();

    // The TEXTURED ray-driven pipeline. Separate from rayDrivenPso_ because the bindless range is
    // part of the root signature: preferred when it exists, and rayDrivenPso_ is the fallback.
    rhi::PipelineHandle rayDrivenTexPso_ = 0;
    // ITS G-BUFFER TWIN -- FIXED a measurement trap: scenePass() used to pick the textured pipeline
    // only when the G-buffer was OFF (`rayDrivenTexPso_ != 0 && !dev_->gBufferEnabled()`), since no
    // textured pipeline wrote the four G-buffer targets and pickGbuf() must return a pair that agree
    // about their root signature. So --gbuffer silently untextured the renderer, and any A/B across
    // that flag compared a textured image to a flat-albedo one. Optional like every other twin here:
    // 0 falls back to the flat G-buffer pipeline.
    rhi::PipelineHandle rayDrivenTexGbufPso_ = 0;
    // The TEXTURED blended (glass) variant: PSMainVoxi compiled with the bindless table declared,
    // so a reflection seen IN a windowpane samples the reflected surface's texture. Preferred over
    // sceneRtBlendedPso_ whenever it built, for vertex-shader draws (there is no mesh-shader twin).
    // Blended draws never pick a G-buffer twin -- see scenePipeline().
    rhi::PipelineHandle sceneRtBlendedTexPso_ = 0;

    // ---- STAGED RAY-DRIVEN PASSES (milestone 1, voxi.rayDrivenStages) ----
    // Splits the single PSRayDriven draw into GPU passes: compute twins of rdVisBuf_/rdSunVisTex_'s
    // producers trace the primary ray, sun-shadow ray, GI, sky occlusion and reflection into
    // dedicated resources, then the same textured ray-driven pixel shader (recompiled to READ those
    // records) composes. See recordStagedRayDriven() for recording order and rdStagedActive() for
    // every precondition -- rayDrivenStages == 0 (and every fallback) never touches any member here,
    // keeping the single-pass path byte-for-byte unchanged. All seven staged pipelines (this pair, the two
    // below, rdGiCsPso_/rdSkyOccCsPso_, and rdReflCsPso_) share one root signature
    // (giLayout(kRtTextureCapacity), same as rayDrivenTexPso_/rayDrivenSplitTexPso_ -- only entry
    // point and stage differ; D3D12ResourceFactory::rootSignature dedupes on layout content, not
    // compute-vs-graphics), and each is 0 if its shader failed to compile, degrading like every
    // other optional Voxi pipeline in this file.
    //
    // rdVisCsPso_ (CSRdVisibility): traces the primary ray. rdShadowCsPso_ (CSRdShadow): traces the
    // sun-shadow ray. Dispatched together with no barrier between them (see recordStagedRayDriven).
    rhi::PipelineHandle rdVisCsPso_    = 0;
    rhi::PipelineHandle rdShadowCsPso_ = 0;
    // MILESTONE 2 lighting-stage twins, dispatched in the same barrier-free "Voxi RD lighting
    // stages" span as rdShadowCsPso_ (they write disjoint resources):
    // rdGiCsPso_ (CSRdGi): rebuilds the visibility hit's surface and calls giRestirIndirect exactly
    // as PSRayDriven's GI block does, writing gRdGiTex (u13). Dispatched only when
    // cb_.voxelParams[3] > 0.5 && cb_.giRestirParams[0] > 0.5 (mirrored in recordStagedRayDriven()).
    // 0 if CSRdGi fails to compile or under the same conditions as rdVisCsPso_/rdShadowCsPso_ -- SM
    // 6.6, same layout and defines as rdShadowCsPso_.
    rhi::PipelineHandle rdGiCsPso_     = 0;
    // MILESTONE 4: rdGiCbCsPso_ -- CSRdGi compiled again with AVER_GI_CHECKERBOARD=1, skipping half
    // the pixels on a checkerboard (the denoiser reconstructs the rest) instead of tracing every one. Optional on top of an optional pipeline: 0 if rdGiCsPso_ itself is 0, or if
    // only this permutation fails to compile (falls back to rayDrivenStages == 1's plain behaviour,
    // never to the single pass).
    rhi::PipelineHandle rdGiCbCsPso_   = 0;
    // rdSkyOccCsPso_ (CSRdSkyOcc): calls rtSkyOcclusionTemporal exactly as PSRayDriven's sky-
    // occlusion branch, writing gRdAoTex (u14). Dispatched when cb_.ambientParams[0] > 0.5 &&
    // (cb_.giRestirParams[0] > 0.5 || cb_.voxelParams[3] <= 0.5), else rdAo would otherwise still
    // read its initial 1.0; cone-GI's own sky occlusion never needs this dispatch. Same SM 6.6/
    // layout/defines shape as rdGiCsPso_ above.
    rhi::PipelineHandle rdSkyOccCsPso_ = 0;
    // MILESTONE 3: CSRdRefl, joining the same lighting-stages span -- writes gRdReflTex (u15) plus
    // the reflection history (u3) via the same rtReflectionTemporal call PSRayDriven's un-split
    // block uses. Dispatched when cb_.shadowParams[2] > 0.5 && cb_.rtParams[3] > 0.5 (the per-pixel
    // roughness half of the gate stays in the shader). 0 if CSRdRefl fails to compile or under the
    // same conditions as rdVisCsPso_/rdShadowCsPso_ -- SM 6.6, same layout and defines as
    // rdGiCsPso_/rdSkyOccCsPso_ above.
    rhi::PipelineHandle rdReflCsPso_   = 0;
    // ---- SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit / rayDrivenReflSplit):
    // each a probe/trace pass feeding a second pass over the same pixels the unsplit shader already
    // covers -- optional on top of an already-staged path; rdStagedActive() never inspects any handle
    // below, so a device that cannot build one just keeps the unsplit CSRdShadow/CSRdGi/CSRdRefl. ----
    //
    // A: rdShadowProbeCsPso_ (CSRdShadowProbe) traces one ray per 8x8 tile into gRdShadowTiles (u18);
    // rdShadowTiledCsPso_ is CSRdShadow recompiled with AVER_RD_SHADOW_TILES=1, ORing its tile's 3x3
    // neighbourhood and skipping the per-pixel ray where every probe agrees. Same layout/csDefs/SM
    // 6.6 as rdShadowCsPso_ above (6.6 needed for derivatives).
    rhi::PipelineHandle rdShadowProbeCsPso_ = 0;
    rhi::PipelineHandle rdShadowTiledCsPso_ = 0;
    // B: rdGiTraceCsPso_ (CSRdGiTrace) traces the fresh ReSTIR GI candidate into gRdGiCand (u17),
    // compacted to the traced half when compiled with AVER_GI_CHECKERBOARD=1 (rdGiTraceCbCsPso_).
    // rdGiSplitCsPso_/rdGiSplitCbCsPso_ are CSRdGi recompiled with AVER_GI_SPLIT=1, loading that
    // stored candidate instead of tracing its own.
    rhi::PipelineHandle rdGiTraceCsPso_   = 0;
    rhi::PipelineHandle rdGiTraceCbCsPso_ = 0;
    rhi::PipelineHandle rdGiSplitCsPso_   = 0;
    rhi::PipelineHandle rdGiSplitCbCsPso_ = 0;
    // RADIANCE CACHE twins (stage 1): CSRdGi / CSRdGiTrace and their checkerboard variants compiled
    // again with AVER_NEURAC=1, the ONLY compiles that contain the cache's scatter and
    // lookup code -- every other variant (single-pass PSRayDriven, raster PSMainVoxi, the split-read
    // CSRdGi) keeps byte-identical preprocessed text, which is what "off means off" and the AMD
    // register limit both need. Built LAZILY (createNeuRaCTwins) the first time Cached mode
    // is wanted on a staged D3D12 pipeline, never otherwise. Chosen per frame by testing bit 128 of
    // cb_.ambientParams[3] AND the handle being non-zero; a twin with the bit clear is plain
    // HalfResolution. The split-read CSRdGi (AVER_GI_SPLIT=1) needs no twin: it never traces.
    rhi::PipelineHandle rdGiCacheCsPso_        = 0;
    rhi::PipelineHandle rdGiCacheCbCsPso_      = 0;
    rhi::PipelineHandle rdGiTraceCacheCsPso_   = 0;
    rhi::PipelineHandle rdGiTraceCacheCbCsPso_ = 0;
    // C: rdReflSplitCsPso_(R1, CSRdRefl + AVER_RD_REFL_SPLIT=1) traces the ray and writes gRdReflTex
    // a PENDING marker in place of composing wherever a history is bound to gather against (see
    // gRdReflTex's header comment, voxi.hlsl, for the fourth alpha this adds). rdReflFilterCsPso_
    // (R2, CSRdReflFilter) reruns rtReflectionSpatial against R1's own gRtReflHistOut write this same
    // frame and finishes the compose; its own header comment (voxi.hlsl) says it needs no
    // compile-time guard of its own.
    rhi::PipelineHandle rdReflSplitCsPso_  = 0;
    rhi::PipelineHandle rdReflFilterCsPso_ = 0;
    // LOCAL LIGHTS (LAMPS): CSRdLocalLights sums every in-range lamp's diffuse irradiance per pixel,
    // traces one shadow ray toward one lamp picked by luminance share, and accumulates visibility in
    // rdLocalHist_ (t19/u19), dispatched right after rdShadowCsPso_. Optional on top of the staged
    // path: a failed compile only zeroes the staged light count, never falls back to single-pass.
    // Raster and single-pass shade lamps without it.
    rhi::PipelineHandle rdLocalLightsCsPso_ = 0;
    // Stage B: rayDrivenTexPso_/rayDrivenTexGbufPso_ recompiled with ";AVER_RD_SPLIT=1" appended,
    // built beside their untextured twins. 0 on a device that built the textured pipeline but not
    // this variant -- rdStagedActive() treats that as "staged unavailable", not a crash.
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
    // buildMaterialTable's buffer-creation failure, said once. Its size report needs no flag: it
    // speaks whenever the table's count changes (rtMaterialUploaded_'s size is the last one said).
    bool rtMaterialAllocFailLogged_ = false;
    // This build's dense table (final, sorted, index-order), and a CPU-side snapshot of what's
    // actually in rtMaterials_[rtMaterialSlot_] on the GPU. Compared byte for byte every build so an
    // unchanged scene costs one memcmp instead of a re-upload -- there's no revision counter upstream
    // to trust instead, so exact content comparison IS the revision signal.
    std::vector<pbr::MaterialConstants> rtMaterialData_;
    std::vector<pbr::MaterialConstants> rtMaterialUploaded_;
    // Per rtInstanceData_ element, same order (both grow in lockstep in buildAccelerationStructures'
    // per-draw loop): the key buildMaterialTable() resolves into a final dense index once the whole
    // draw list has been seen. Consumed and cleared by buildMaterialTable() at the end of that build.
    std::vector<u64> rtInstanceMatKey_;

    // Second pass over this build's draws, once the per-draw loop (and rtInstanceMatKey_ with it) is
    // complete: sorts the distinct material keys, builds rtMaterialData_ in that order (index 0
    // always the material system's fallback, never a stale/uninitialised row), re-uploads only when
    // content differs from the GPU's, and writes the final index into every
    // rtInstanceData_[i].materialIndex. Returns false only if the GPU buffer itself couldn't be
    // (re)created; the materialIndex values stay correct either way.
    // `matConstantsByKey`: resolved bytes for every distinct key, built by the same per-draw loop --
    // passed in rather than kept as a member so an early-exiting build leaves no stale generation.
    bool buildMaterialTable(const std::unordered_map<u64, pbr::MaterialConstants>& matConstantsByKey);
    // One surface's key into this build's material table and -- the first time this build sees the
    // key -- its constants into matConstantsScratch_ (textures made resident). Shared by the per-draw
    // loop and the foliage parts (resolveFoliageMaterials), so both key and resolve a material the same
    // way. `authoredBytes` is the surface's pbr::MaterialConstants, read only when matSet is authored.
    u64 rtMaterialKey(rhi::BindingSetHandle matSet, const void* authoredBytes, const f32 color[4],
                      f32 metallic, f32 roughness);

    // ---- INSTANCED FOLIAGE (setFoliage): the state behind the TLAS's static prefix ----
    // A hit on foliage never reaches rtInstanceData_: its TLAS instanceId is kRtFoliageIdBit | (the
    // prototype's first part below), and the shader's rtLoadInstance (voxi_rt.hlsli) reads the part's
    // record from the PART TABLE (t20, gRtFoliageParts -- an RtInstance per part, objectToWorld unused)
    // and the instance's transform from the prefix's own desc buffer (t21, gRtFoliageDescs). Both slots
    // hold a one-element placeholder whenever there is no foliage.
    struct FoliagePartState {
        rhi::MeshHandle mesh = 0;
        u32 material = 0;
        // materials_.bindingSet(material) as setFoliage resolved it (0 for material 0) -- the handle the
        // material-table key and the gate's hash use, exactly as a draw's matSet is.
        rhi::BindingSetHandle matSet = 0;
        f32 color[4] = {1, 1, 1, 1};
        f32 metallic = 0.0f, roughness = 1.0f;
    };
    // Every prototype's parts, contiguous, in each BLAS's geometry order.
    std::vector<FoliagePartState> foliageParts_;
    // One BLAS per prototype that got one, owned here (createBlasMulti), destroyed by clearFoliage.
    std::vector<rhi::BlasHandle> foliageBlas_;
    // The distinct meshes the parts name, sorted -- the geometry table must hold them even when no draw does.
    std::vector<rhi::MeshHandle> foliageMeshes_;
    u32 foliageInstances_ = 0;   // what the TLAS prefix holds
    u64 foliageBlasBytes_ = 0, foliagePrefixBytes_ = 0;
    // Bumped by every setFoliage/clearFoliage and folded into rtAccelDrawsKey(), so the unchanged gate
    // can never skip the build that has to pick a new set up.
    u64 foliageGeneration_ = 0;
    // The BLASes are allocated by setFoliage (it has no render context) and built by the next
    // buildAccelerationStructures that builds tlas_, just before that build.
    bool foliageBlasPending_ = false;
    // t2/t20/t21 must be rewritten before any ray reads them: setTlasStaticInstances reallocates the
    // TLAS and replaces its desc buffer, and clearFoliage frees the part table. Written early in the
    // next prePass, before anything binds bindings_ -- never from setFoliage itself, which can run
    // mid-frame after the set was bound (Vulkan forbids that write; D3D12 would hand this frame an
    // unbuilt TLAS).
    bool foliageBindingsDirty_ = false;
    // The part table as this build resolved it (firstIndex/firstVertex from the geometry table,
    // materialIndex from the material table, both of which move between builds), what the GPU holds,
    // and each part's material key -- same shapes as rtInstanceData_/rtMaterialUploaded_/rtInstanceMatKey_.
    std::vector<RtInstance> foliagePartData_;
    std::vector<RtInstance> foliagePartUploaded_;
    std::vector<u64> foliagePartMatKey_;
    // An upload-heap ring, same reason as rtMaterials_: rewritten only when content changes, but
    // writeBuffer is unsynchronised, so a rewrite must not land on the slot an in-flight frame reads.
    rhi::BufferHandle foliagePartBuf_[kRtInstanceRing] = {};
    u32 foliagePartSlot_ = 0;
    u32 foliagePartCapacity_ = 0;   // elements each ring slot was sized for
    rhi::BufferHandle foliagePartPlaceholder_ = 0, foliageDescPlaceholder_ = 0;
    // Resolves every part's material key into matConstantsScratch_ (textures made resident, as a
    // draw's are) and foliagePartMatKey_ -- the per-draw loop's material half, per part. O(parts).
    void resolveFoliageMaterials();
    // Re-uploads foliagePartData_ when it differs from what the GPU holds, and binds t20 to it.
    void uploadFoliagePartTable();
    // Rewrites t2/t20/t21 when foliageBindingsDirty_ says they moved.
    void refreshFoliageBindings();
    // The gate's view of the foliage: its generation plus every part's live material bytes, folded like
    // one draw. O(parts), never O(instances).
    u64 foliageKey() const;

    // ---- LOCAL LIGHTS (LAMPS): the per-frame light list at t18 (gRdLocalLights) ----
    //
    // One entry per authored draw whose material has MaterialFlag_Light (lightIntensity > 0): the
    // draw's world bounding sphere becomes a sphere light coloured by its emissiveFactor. HLSL mirror
    // `struct RdLocalLight { float4 posRadius; float4 radianceRange; }` -- same 32-byte stride, which
    // setSrvBuffer's stride argument hands the shader.
    //   posRadius     = world centre (cm), sphere radius (cm, >= 1)
    //   radianceRange = rgb colour (max component 1) x the lamp's 1-metre irradiance (its emissive
    //                   peak and bounding-sphere radius, times lightIntensity -- see buildLocalLights);
    //                   w = range (cm)
    struct RdLocalLight {
        f32 posRadius[4];
        f32 radianceRange[4];
    };
    static_assert(sizeof(RdLocalLight) == 32, "RdLocalLight is the HLSL RdLocalLight ABI");
    // At most this many per frame, the most important by that same 1-metre irradiance over max(camera
    // distance in metres squared, 1). Bounds the per-pixel loop in CSRdLocalLights.
    static constexpr u32 kMaxLocalLights = 32;
    // AN UPLOAD-HEAP RING, the same shape and the same reason as rtInstances_: writeBuffer is an
    // unsynchronised memcpy into mapped memory, and this list is rewritten every frame while the GPU
    // may still be reading the previous frame's copy -- a lamp would light the pixel from where the
    // NEXT frame puts it. Rotating before the write keeps this frame off the slot the last one bound.
    rhi::BufferHandle rdLocalLights_[kRtInstanceRing] = {};
    u32 rdLocalLightSlot_ = 0;
    u32 rdLocalLightCapacity_ = 0;     // elements each ring slot was sized for
    // A one-element stand-in bound at t18 whenever the list is empty -- every declared slot must hold
    // a valid descriptor of its declared kind (Tier 1). Created in createVoxelVolume, kept until
    // shutdown.
    rhi::BufferHandle rdLocalLightsPlaceholder_ = 0;
    // What t18 names right now, so an empty list rebinds the placeholder once, not every frame.
    rhi::BufferHandle rdLocalLightsBound_ = 0;
    // THIS frame's list as uploaded (canonical order -- see buildLocalLights), its length, and an
    // FNV-1a hash of exactly those bytes plus the count, which rdLocalHistHash_ is compared against.
    std::vector<RdLocalLight> rdLocalLightData_;
    u32 rdLocalLightCount_ = 0;
    u64 rdLocalLightHash_ = 0;
    // True when EVERY draw whose material asks to be a light made this frame's list -- none cut by the
    // 32-light cap, none without bounds. Only then may the GI estimators leave a lamp-flagged hit's own
    // emission out (gCameraMedium.w bit 2): a flagged lamp that missed the list would otherwise get
    // neither its direct light nor its glow in GI, and go dark.
    bool rdLocalLightsCarryAll_ = false;
    // Per-draw candidates, kept as a member so a steady scene reuses the allocation.
    struct RdLocalLightCand {
        f32 importance;
        RdLocalLight light;
    };
    std::vector<RdLocalLightCand> rdLocalLightCand_;
    bool rdLocalLightsFailLogged_ = false;   // ring allocation failure, said once
    // Builds this frame's list from drawsPrev_ -- including draws the TLAS or the camera cull hides,
    // so a lamp behind the camera still lights what is on screen -- and uploads it to t18. Empty
    // (placeholder bound, count 0) unless voxi.localLights is on, the backend is D3D12 and a TLAS exists
    // this frame: every ray-traced scene mode (raster, single-pass, staged) can light with it. Called from
    // prePass() between buildAccelerationStructures() and the first bind of bindings_.
    void buildLocalLights();
    // What every lamp-shading scene pass needs this frame: a non-empty list, voxi.localLights, and u19
    // bound to the real history pair (rdLocalOutThisFrame_, which beginShadowHistory leaves 0 on a frame
    // it bound nothing -- no ray-traced history, or the debug view). Each mode adds its own requirement on
    // top: the staged path CSRdLocalLights' pipeline, the single pass kRdSinglePassLamps (VoxiRenderer.cpp).
    bool localLightsReady() const {
        return rdLocalLightCount_ > 0 && settings_.localLights && rdLocalOutThisFrame_ != 0;
    }
    // Writes cb_.cameraMedium[2]/[3] for the scene pass about to be recorded -- the light count and the
    // history-valid / carries-all bits when `live`, both 0 otherwise -- and returns `live`, so the caller
    // marks the history written (rdLocalHistFrame_/rdLocalHistHash_) once its pass is recorded. Called
    // once per frame by whichever pass shades the opaque scene: prePass's tail for the raster draws,
    // scenePass for the single pass, recordStagedRayDriven for the staged passes. The values then stay
    // for the rest of the frame, the blended replay included. `pass` names it in the once-only log.
    bool publishLocalLights(bool live, const char* pass);

    // ---- previous-frame per-instance transforms: RtInstance::prevObjectToWorld's bookkeeping ----
    //
    // averGBufferVelocity() (voxi.hlsl) reprojects one world position through this frame's
    // camera and last frame's -- correct only for a surface that did not move. A MOVING instance
    // needs its own previous object-to-world, which the ray-driven shader reads from
    // RtInstance::prevObjectToWorld. It has two writers, both fed from what the TLAS actually drew
    // (not from this frame's entities: the draw list the structure reads is one frame stale by design,
    // so motion must be the difference between successive TLAS transforms to match the image):
    //   - patchRtMovers() copies a mover's current objectToWorld into prevObjectToWorld just before it
    //     writes the new world (the mover patch lane above);
    //   - a FULL build carries last frame's drawn transforms forward by group + nearest match, below.
    // rtPrevPending_ then makes sure a prev that differs from its current does not outlive the frame it
    // describes.
    //
    // GROUP KEY: the RT instanceId is a POSITION in this frame's replay, not a stable identity (insert
    // or drop an earlier draw and every id after it renames a different instance), and giDrawsKey()
    // folds the whole list into one hash. Mesh handle alone collides on instanced props. So an
    // instance's group is (mesh, drawBinding) -- this RHI isn't bindless, so drawBinding is shared per
    // MATERIAL -- and within a group instances are told apart by WHERE they were, not by an ordinal:
    // an ordinal shifts for the whole group the moment one instance spawns or despawns, which is
    // exactly the failure the old ordinal tracker had.
    //
    // Groups by (mesh, drawBinding). FNV-1a, matching giDrawsKey()/buildGeometryTable's own mixing
    // constants so a reader who already knows those two recognises the recipe.
    u64 rtInstanceGroupKey(rhi::MeshHandle mesh, rhi::BindingSetHandle matSet) const;

    // Parallel to rtInstanceData_ (one entry per row, filled in the same loop that pushes the row): the
    // group key of the draw that produced it. Foliage parts are not in rtInstanceData_ and not here.
    // Cleared with the other per-build vectors; sized differently from rtInstanceData_ (an empty vector
    // after a shutdown) it tells the next build there is nothing trustworthy to carry forward.
    std::vector<u64> rtInstanceGroupKey_;

    // Rows of rtInstanceData_ whose prevObjectToWorld != objectToWorld as of the latest upload. The
    // instance table is only re-uploaded when something changes, so a mover that stops would otherwise
    // leave its last motion in the table for the GPU to report forever: the start of the next gate-hit
    // frame settles these rows (prev = current) and re-uploads once if it did. See the gate branch in
    // buildAccelerationStructures.
    std::vector<u32> rtPrevPending_;

    // A full build's carry-forward lookup over LAST frame's rows (built from rtInstanceData_ before it
    // is cleared). Member containers, cleared and refilled each build so a steady 42-51k-draw scene
    // reuses their capacity. Exact matches go through a hash of (group, the 16 floats' raw bits), so
    // the common case -- the instance is exactly where it was -- is O(1) average; a used entry is
    // unlinked from its exact chain so a chain only ever holds unmatched rows.
    static constexpr u32 kRtCarryNone = 0xFFFFFFFFu;
    // Engine units are centimetres. A nearest match farther than this is a different object that
    // happens to share a mesh and material, not the same one moved: handing it that object's motion
    // would smear the pixel across the map, and WRONG motion is worse than none (the fallback, zero).
    // 20 m is generous for anything that moves in one frame, including a fast vehicle at low fps.
    static constexpr f32 kRtMaxCarryCm = 2000.0f;
    // Nearest-translation matching is a linear scan over a group's unmatched rows, so it only runs when
    // there are at most this many: a group of thousands of identical props is O(n^2) otherwise, and a
    // prop in such a group that did not match EXACTLY is far more likely new than moved.
    static constexpr u32 kRtCarryMaxScan = 64;
    struct RtCarryEntry {
        f32 world[16];                       // last frame's drawn objectToWorld
        u32 nextInGroup = kRtCarryNone;      // chain through the entries of one group
        u32 nextExact = kRtCarryNone;        // chain through entries with the same (group, world) hash bucket
        u32 group = 0;                       // index into rtCarryGroups_
        bool used = false;
    };
    struct RtCarryGroup {
        u64 key = 0;
        u32 head = kRtCarryNone;             // first entry of the group
        u32 unused = 0;                      // entries not yet matched by an instance of this build
        u32 nextInBucket = kRtCarryNone;     // chain through the groups of one bucket
    };
    std::vector<RtCarryEntry> rtCarryEntries_;
    std::vector<RtCarryGroup> rtCarryGroups_;
    std::vector<u32> rtCarryGroupBuckets_;   // power-of-two, bucket -> first group
    std::vector<u32> rtCarryExactBuckets_;   // power-of-two, bucket -> first entry
    // Fills the lookup above from rtInstanceData_/rtInstanceGroupKey_. Leaves it empty when there is
    // nothing to carry (first build, after a reset, or the two vectors out of step).
    void buildRtCarryLookup();
    // Writes `outPrev` for an instance of `groupKey` drawn at `world` this build: the exact last-frame
    // match, else the nearest unmatched one in range, else `world` itself. Marks the entry it took.
    void carryPrevTransform(u64 groupKey, const f32* world, f32* outPrev);

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

        // TRANSLUCENT: in the TLAS, out of everything else. Reaches the acceleration structure marked
        // non-opaque (a shadow ray attenuates rather than stops) but stays out of the shadow cascade,
        // GI shadow map and voxelisation -- all binary depth-only passes with no pixel shader to
        // compute a transmittance in. A flag on the draw, not a second list: a parallel list would
        // mean four walk sites each remembering to visit it, but one flag tested in one place per
        // pass cannot be forgotten by a fifth pass added later.
        bool translucent = false;

        // HIDDEN FROM ITS OWNER: in everything, out of the ray-driven primary ray alone. The exact
        // complement of `translucent`: stays in every pass it already reached -- shadow cascade, GI
        // voxelisation, reflections, bounce rays -- removed only from the primary-visibility
        // traversal, because that ray begins inside this mesh. See AVER_RT_MASK_OWNER_HIDDEN in
        // voxi.hlsl for why the fix belongs in the instance mask.
        bool hiddenFromOwner = false;

        // MOVABLE: moves during a play session (game::PlayMobility decides). In the TLAS, the
        // cascades and every ray pass like any draw; out of voxelisation, the GI rebuild gate's hash
        // and the GI shadow map. The volume is a bake, so a draw that moves every frame made the gate
        // rebuild it every frame. The RT acceleration-structure gate leaves the transform alone out of
        // its key (rtAccelDrawsKey) and the mover patch lane writes it into the instance list instead
        // of rebuilding everything -- the draw stays in the structure, only how it is kept current differs.
        bool movable = false;
    };
    bool submitMovable_ = false;   // see setSubmitMovable

    // One binding set's answers to hashDrawMaterialInto's two MaterialSystem lookups (is it an authored
    // material, and which textures does it hold), remembered for the length of ONE key computation. A
    // city has ~42,000 draws over a few hundred materials, and each draw used to pay two unordered
    // lookups per key, two or three keys a frame. LOCAL TO ONE CALL by construction (each key function
    // declares its own), never a member: the answers are only valid while the material system is not
    // touched, which no key loop does, and a memo outliving the call could hide a material edit -- the
    // failure these keys exist to catch. Direct-mapped on the handle (dense small ids, so a few hundred
    // materials never collide); a collision just repeats the lookups.
    struct DrawMaterialMemoSlot {
        rhi::BindingSetHandle set = 0;
        u32 state = 0;      // 0 = empty; else bit 0 set, bit 1 = authored, bit 2 = has a texture table
        u64 texHash = 0;    // the texture table's hash, when bit 2 is set
    };
    static constexpr u32 kDrawMaterialMemoSlots = 1024;
    static_assert((kDrawMaterialMemoSlots & (kDrawMaterialMemoSlots - 1)) == 0,
                  "kDrawMaterialMemoSlots must be a power of two for the '& (kDrawMaterialMemoSlots - 1)' mask");
    using DrawMaterialMemo = std::array<DrawMaterialMemoSlot, kDrawMaterialMemoSlots>;

    // One draw's material identity folded into the running hash `h` -- shared by rtAccelDrawsKey() and
    // giDrawsKey()/giDrawsSubKeys() so the two gates cannot drift apart (giDrawsKey() used to hash
    // only colour/metallic/roughness, so an authored edit -- a lamp's emissiveFactor in the Material
    // Editor -- never rebuilt the voxel GI). Declared after struct Draw because it takes one. `memo` is
    // the calling key function's own DrawMaterialMemo, zero-initialised at its top.
    void hashDrawMaterialInto(u64& h, const Draw& d, DrawMaterialMemo& memo) const;

    // One draw's finalised term of rtAccelDrawsKey(). With `moverLane` a movable draw's term leaves its
    // world matrix out and carries the movable bit instead, which makes it the draw's IDENTITY for the
    // mover patch lane (RtMover::id); without it (and for every non-movable draw) it is the old
    // world-inclusive hash bit for bit.
    u64 rtDrawHash(const Draw& d, DrawMaterialMemo& memo, bool moverLane) const;

    std::vector<Draw> draws_, drawsPrev_;
    // Two facts about the list, recorded by submit() as each draw is appended and swapped along with the
    // list by beginScene(), so a pass that only cares about the rare draw need not test all ~42,000:
    //   translucentDraws_     the indices, ascending, of every translucent draw (scenePass's caustic and
    //                         camera-medium scan visits only these, in the same order it would have met them);
    //   lightFlaggedDraws_    how many draws carry MaterialFlag_Light in a full-size material block
    //                         (buildLocalLights skips its scan when that is 0 -- no lamp can exist then).
    // The *Prev_ twins describe drawsPrev_, which is what every pass reads.
    std::vector<u32> translucentDraws_, translucentDrawsPrev_;
    u32 lightFlaggedDraws_ = 0, lightFlaggedDrawsPrev_ = 0;

    // ---- the blended-draw census ----
    // submitDraw() drops every `blended` draw before it reaches draws_ (see that function's own
    // comment for the three things dropping it costs a translucent surface; `blended` only exists as
    // a parameter since the shading-contract change that added it to IRenderFeature::submitDraw, so
    // nothing was ever excluded before that). Silently absorbing them would look identical to a scene
    // with no glass at all -- indistinguishable from the bug this drop exists to prevent (a pane
    // rendering as an opaque black shadow, or blocking GI, because the filter was accidentally
    // removed).
    // Counts, not impressions -- same reasoning as giSkipped_/giRebuilt_ below: a running total, not
    // reset per frame, since the question ("is this filter still firing") is answered by the total.
    u64 blendedDropped_ = 0;
    // Widening-interval report counter, same shape as voxelCullLogs_ in voxelizePass: logs at counts
    // 1, 3, 7, 15, ... so the first few drops (most likely to matter watching a scene load) are seen
    // quickly without growing linearly on a level with thousands of glass panes.
    u32 blendedDropLogs_ = 0;

public:
    // A cheaper stand-in for the depth-only passes: every mesh was drawn at full resolution into all
    // four cascades, the GI shadow map and the voxel grid -- six times a frame -- while the lit pass
    // beside them already picks an LOD per instance. On a streamed scene that is the largest item in
    // the frame and none of it is visible (a shadow doesn't resolve silhouette detail a coarser level
    // drops). A function pointer, not a dependency: the LOD ladder belongs to whoever loaded the
    // mesh, not here -- this renderer must not learn what Trifactor is, only "anything cheaper for
    // this handle?", accepting 0 for no; nobody installing a resolver is the default and it renders
    // exactly as before.
    using DepthProxyFn = rhi::MeshHandle (*)(rhi::MeshHandle mesh, void* user);
    // No longer a one-line inline assignment -- defined in the .cpp, not inline: submit()'s per-mesh
    // cache (meshSubmitCache_) remembers depthProxyFn_'s answer per mesh handle with no record of
    // which fn/user pair produced it, so reassigning either without dropping every cache entry would
    // let a cached mesh keep silently answering with the OLD proxy's verdict. See the .cpp for the
    // invalidation.
    void setDepthProxy(DepthProxyFn fn, void* user);

    // A/B measurement toggle for the memoisation meshSubmitCache_ implements below, same shape as
    // coneTraceEnabled(). Default true, pixel-neutral by construction (see meshSubmitCache_). false
    // sends submit() back to resolving both memoised questions directly every call, in the original
    // order, so an A/B comes from one binary rather than a `-D` reconfigure of two build trees (see
    // aver-reconfigure-pollutes-build for why that comparison would not be trustworthy here).
    void setMeshSubmitCacheEnabled(bool on) { meshSubmitCacheEnabled_ = on; }
    bool meshSubmitCacheEnabled() const { return meshSubmitCacheEnabled_; }

private:
    DepthProxyFn depthProxyFn_ = nullptr;
    void*        depthProxyUser_ = nullptr;

    // ---- submit()'s per-mesh memoisation (Phase C follow-up to a7ff716d/80730751) ----
    // submit() re-resolves two mesh-only questions -- depthProxyFn_'s answer and meshBounds'
    // local-space answer -- on every one of the (up to) 16,000 calls a frame, including every
    // frustum-culled entity (the walk routes culled entities here on purpose, so shadows/GI/the TLAS
    // never depend on what the raster camera can see -- see submit()'s own header comment), though a
    // scene rarely has more than a few hundred distinct meshes.
    //
    // A MEMBER, NOT A LOCAL: GameRender.cpp's MeshLookupCacheSlot table is emptied at the start of
    // each drawWorld() call (a generation stamp on a reused table), but submit() runs once per entity
    // with no enclosing call to scope it to -- the only span wide enough is this renderer's own
    // lifetime. See beginScene()'s own comment for the clear point this settled on.
    //
    // A FIXED, DIRECT-MAPPED TABLE, MIXED BEFORE MASKING -- reusing 80730751's shape: a single
    // "last mesh" slot scored ~100% on one mesh submitted 16,000 times running but close to 0% on
    // interleaved content, which is the shape real scenes take. A raw mesh id is masked only after
    // mixMeshId() re-mixes it (see that function's own comment for why this file's reason differs
    // from GameRender.cpp's despite the identical shape).
    //
    // 2048 SLOTS, NOT 64 -- the one deviation from 80730751's precedent: GameRender's 64 slots hold a
    // handful of camera-relevant species, but submit() sees the renderer's WHOLE draw population, so
    // 64 slots would reopen the same near-0% collision failure at a different slot count. It was 256,
    // sized for "a few hundred" meshes; a city level submits ~42,000 draws over ~2,900 distinct meshes
    // and 256 slots measured an ~86% hit rate on it, the other 14% re-asking depthProxyFn_ and meshBounds.
    // 2048 slots is ~74 KB of small structs on this object -- zero allocation, like 80730751's own table,
    // and clearing it is free (see meshSubmitCacheGen_), so the size no longer costs anything per frame.
    static constexpr u32 kMeshSubmitCacheSlots = 2048;
    static_assert((kMeshSubmitCacheSlots & (kMeshSubmitCacheSlots - 1)) == 0,
                  "kMeshSubmitCacheSlots must be a power of two for '& (kMeshSubmitCacheSlots - 1)' "
                  "below to be equivalent to '% kMeshSubmitCacheSlots'");

    // One slot's answer, for one mesh handle, to both of submit()'s per-mesh questions.
    // `mesh == 0` means empty, not a separate flag: submit()'s first line is `if (mesh == 0) return;`,
    // so a default-constructed slot already reads correctly as "never touched".
    // Caches depthProxyFn_'s RAW return, not submit()'s substituted answer -- submit() still runs its
    // own "0 means no proxy" substitution against the cached value every call, so a hit reproduces an
    // uncached call bit-for-bit for the SAME fn/user pair (guaranteed by setDepthProxy()'s drop above).
    // Caches meshBounds' LOCAL-SPACE answer only, before submit() transforms it through `world` into
    // this instance's world-space sphere -- caching the transformed result would hand every instance
    // of a mesh the first instance's world-space bounds, a correctness bug that reads like a culling
    // tuning problem, not a caching one (see submit()'s comment at the transform: entities vanishing
    // from a shadow cascade at the wrong moment).
    // A FALSE verdict from meshBounds is cached too: boundsResolved is set on either outcome, and
    // haveBounds alone records which -- meshBounds' contract (RHI.hpp; D3D12Device.cpp,
    // VulkanDevice.cpp) guarantees a handle cannot flip from "no bounds" to "yes" without a re-upload,
    // which the next beginScene() clear catches.
    struct MeshSubmitCacheSlot {
        rhi::MeshHandle mesh = 0;
        // meshSubmitCacheGen_ as of the moment this slot was filled; a slot from an older generation is
        // an empty one whatever else it holds (see dropMeshSubmitCache()).
        u32 generation = 0;

        bool depthProxyResolved = false;
        rhi::MeshHandle depthProxyMesh = 0;

        bool boundsResolved = false;
        bool haveBounds = false;
        f32  localCentre[3] = {0.0f, 0.0f, 0.0f};
        f32  localRadius = 0.0f;
    };
    // A fixed member array, never resized -- zero allocation, paid once on this object. Dropped whole,
    // not slot by slot, at the top of every beginScene() and again inside setDepthProxy() the moment
    // either of its arguments actually changes -- by bumping meshSubmitCacheGen_, not by rewriting the
    // table: at 2048 slots a per-frame `= {}` was ~74 KB of stores every frame to reset slots that most
    // frames' meshes overwrite anyway.
    std::array<MeshSubmitCacheSlot, kMeshSubmitCacheSlots> meshSubmitCache_{};
    // The current generation; starts at 1 so the all-zero slots of a fresh table read as empty.
    u32 meshSubmitCacheGen_ = 1;
    // Empties the table: every slot from an earlier generation now reads as never touched. On the
    // (4-billion-call) wrap the table IS rewritten, so an ancient slot can never match a reused number.
    void dropMeshSubmitCache() {
        if (++meshSubmitCacheGen_ != 0) return;
        meshSubmitCache_ = {};
        meshSubmitCacheGen_ = 1;
    }

    // Finds mesh's slot above, evicting a different mesh's leftover answers first so a hit never
    // reads a previous occupant's fields under the new key -- same hazard/fix as GameRender.cpp's
    // findMeshLookupSlot. Defined in the .cpp beside submit(), its only caller.
    MeshSubmitCacheSlot& meshSubmitCacheSlot(rhi::MeshHandle mesh);

    // See setMeshSubmitCacheEnabled() for the contract; this is just the storage for it.
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
    // mesh -> its group's index in the vector above. Groups are only ever appended, never erased,
    // so an index stays valid for the renderer's lifetime. Finding a draw's group used to be a
    // linear walk of every group, per draw, per cascade: on a city level (~3,500 distinct meshes,
    // ~51,000 draws) that was ~90 M comparisons a cascade before a single triangle was drawn.
    std::unordered_map<rhi::MeshHandle, u32> shadowGroupIndex_;
    std::unordered_map<rhi::MeshHandle, u32> giShadowGroupIndex_;
    // The group for `mesh` in `groups`, appending it (and indexing it) the first time it is seen.
    static ShadowInstanceGroup& shadowGroupFor(std::vector<ShadowInstanceGroup>& groups,
                                               std::unordered_map<rhi::MeshHandle, u32>& index,
                                               rhi::MeshHandle mesh);

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
        // x = 1 while rtShadowHist_'s slots are bound this frame (unbound = Tier 1 null-filled, not
        // a real texture -- the shader must not touch t6/u2 when this is 0). y = 1 once that texture also holds a real previous frame (0 right after
        // creation/resize). z = current frame index (rtFrameIndex_), never wall-clock. w = pixels-
        // per-ray tile edge as its bit count (0 = no tiling).
        f32 rtHistParams[4] = {};
        // The CAMERA's view-projection from the frame before this one, for reprojecting into last
        // frame's ray-traced shadow history. Meaningful only while rtHistParams.y is set.
        f32 prevViewProj[16] = {};
        // LAST frame's scene viewport rect (x, y, w, h in target pixels; IDevice::sceneViewport) --
        // the reprojected NDC lands here, not at [0,1] of the whole history texture: the editor
        // docks the 3D view in a sub-rect of the backbuffer, same as prevViewProj above.
        f32 sceneViewport[4] = {};
        // THIS frame's scene viewport rect, same layout. Not redundant with sceneViewport above: a
        // reader projecting with THIS frame's gViewProj (refraction) needs this frame's rect, and
        // reusing the previous one silently mismatches for a frame after any viewport change.
        f32 sceneViewportCur[4] = {};
        // THE MEDIUM THE CAMERA IS CURRENTLY INSIDE. x = 1 when the eye is within a blended,
        // single-sided volume; y = that material's ior. Needed because a closed volume seen from
        // within has no front faces (every face points away from the eye), so the back-face discard
        // that keeps a water box from compositing four alpha coats would also delete the surface
        // once you swim under it -- the shader inverts the discard instead; see PSMainVoxi.
        // z, w: LOCAL LIGHTS (LAMPS), riding this row's two spare floats. z = light count in t18 as
        // a float (rdLocalLightCount()), 0 for prePass and raised by publishLocalLights() for
        // whichever scene pass shades (raster, single-pass or staged primary; kept through the
        // blended replay). w = two bits as a float:
        // 1 = t19 holds a usable previous frame under the SAME light set (rdLocalHistValid()), 2 =
        // every lamp-flagged draw is in the list so GI may drop emitters' own emission
        // (rdLocalCarriesEmitters()).
        f32 cameraMedium[4] = {};
        // THE WATER VOLUME THAT CASTS CAUSTICS, world centimetres: min.xyz/max.xyz of its AABB,
        // min.w = 1 when one exists, max.w its strength. The volume's top (max.z) is the surface
        // light refracts through, and everything in the footprint below that height is lit through
        // it. Published from the renderer, not read from the fluids module --
        // Voxi renders and plugins do not, so what arrives here is just a box.
        f32 causticMin[4] = {};
        f32 causticMax[4] = {};
        // The GI-only shadow map's light view-projection, fitted to the GI volume, not the camera --
        // see fitGiShadow(). Read only by PSVoxel through giShadowFactor(); PSMainVoxi keeps using
        // cascadeViewProj/shadowFactor above for the camera cascades.
        f32 giShadowViewProj[16] = {};
        // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable (0 falls back to fully-lit
        // indirect), z = normal-offset bias in world units. w = staged ray-driven bit-field toggles (`uint bits =
        // (uint)gGiShadowParams.w`): bit 1 rtSecondaryShadowOpaque, bit 2 rtSkyOcclusionHalfRate,
        // bit 4 rtReflectionHalfRate, bit 8 rtGiHitShadowMap (Voxi.hpp has each one's own comment) --
        // packed every frame in prePass, not fitGiShadow(), so a gated frame still carries them. Bit
        // 16 is different in kind: "this frame's staged textures (gRdSunVisTex/gRdGiTex/gRdAoTex/
        // gRdReflTex) hold this frame's values AND blendedReuseStagedLighting is on" -- ORed in only
        // by recordStagedRayDriven on a staged frame; prePass's from-scratch write leaves it 0
        // otherwise, which makes it self-clearing. Bit 32 is automatic, not a toggle: ORed in by
        // prePass right after buildAccelerationStructures when tlas_ holds no translucent instance
        // (rtTlasTranslucent_ == 0), so rtShadowEx traces its first-hit query instead of the walk.
        f32 giShadowParams[4] = {};
        // Spatial shadow denoiser (gRtDenoiseParams). x = filter radius in pixels (0 = off), y = how
        // much of the filtered value to take (0 discards it while still paying for the taps, so cost
        // can be measured before trusting the filter), z = taper rate vs. reprojection velocity (0 =
        // no taper), w = 1 while the AMBIENT history pair is bound (its own bit since, unlike
        // shadow/reflection, it's allocated only at High/Epic -- see aoHistoryWanted()).
        f32 rtDenoiseParams[4] = {};
        // Ray-driven bounce control (gRtBounceParams). x = bounces after the first hit (1 = what
        // reflections already do); y/z/w unused. Own float4, not a spare gRtDenoiseParams slot: a
        // "denoise" field carrying a bounce count reads fine for a week then costs an afternoon.
        f32 ptBounceParams[4] = {};
        // GI gather control (gGiParams). x = cones the diffuse gather traces, total (incl. axial).
        // y/z/w are REFRACTION, not spare: y = refractionMode, z = refractionStrength, w =
        // refractionEdgeFade, written every frame in VoxiRenderer.cpp and read by
        // averRefractedBackdropUV. Own float4 for the same reason
        // ptBounceParams has one.
        f32 giParams[4] = {};
        // Ambient control (gAmbientParams). x = sky-visibility rays the ambient term traces per
        // pixel, 0 = use the cone gather's own occlusion (every tier below the top). y = coherence
        // tile edge the sky-occlusion rays share a direction across (1 = fresh direction per pixel;
        // was claimed spare here while already read by the shader).
        // z = setLightingLegacyBits' u32 bitmask, stored as a float and decoded with a u32 cast (0 =
        // every lighting-contrast fix live; see that method's bit table). w = U1/2.9's own bitmask,
        // packed/decoded by givis::packAmbientW (same numeric-cast idiom as z, not a
        // bit-reinterpretation):
        //   bits 0-1  (& 3u)      Settings::giRestirVisibility (0 NoRay, 1 Reconstructed, 2 HalfRes, 3 Full; Cached (4) packs as 2 + bit 128)
        //   bit 4     (& 4u)      half-res ReSTIR visibility pair (t16/u10, giVisHist_) bound this frame
        //   bit 8     (& 8u)      t16 holds a real previous frame
        //   bit 16    (& 16u)     W6/M5 setBlendedGiCone: blended fragment's indirect diffuse uses
        //                         the cone gather instead of ReSTIR
        //   bit 32    (& 32u)     backend replays translucent draws blended THIS frame (D3D12 only)
        //   bit 64    (& 64u)     setGiVisPathView's debug view
        //   bit 128   (& 128u)    radiance cache live this frame (neuracLive_); read only by
        //                         the AVER_NEURAC twin pipelines
        //   bits 12-15 (>>12 & 15u) Settings::giRestirSpatialSamples (15 = auto)
        //   bits 18-22 (>>18 & 31u) Settings::giRestirMaxHistory (reuse.maxHistory)
        // Single writer: beginShadowHistory (published twice -- unconditionally near the top with
        // histBound/histValid false, then again once the giSurf block knows the sixth pair's real
        // state).
        // Own float4, not a spare gRtDenoiseParams/gGiShadowParams slot, for the reason
        // ptBounceParams states.
        f32 ambientParams[4] = {};
        // EDITOR VIEW MODES the ray-driven path honours itself (gViewParams). x = a small integer
        // MODE: 0 normal, 1 unlit (flat albedo, no lighting -- unlit_/setUnlit), 2-5 the ViewDebug
        // enum above (RayHitInstance/RayHitMaterial/RayHitDistance/Triangles). Composed as `viewDebug_ != ViewDebug::None ? f32(viewDebug_) : (unlit_ ? 1 : 0)`
        // so the two dropdown families stay mutually exclusive on one float. PSRayDriven decodes
        // with `(uint)(gViewParams.x + 0.5)`; PSMainVoxi never reads it, which is why these are
        // ray-driven-only. A pass-level field, not per-draw: a ray hit has no per-draw cbuffer
        // (gShadingModel rides the raster-only b1 block) -- the asymmetry is why unlit needed its
        // own raster mirror (RhiDevice::setUnlit) rather than reaching the ray-driven path directly.
        //
        // y = Settings::giRadianceCeiling (AVER_VOX_MAXRAD in voxi.hlsl/voxi_gi.hlsli; see that
        // field's own comment in Voxi.hpp). 0 reads as "unset", falling back to the shader's 16.0
        // default, so an all-zero block (giFrameConstants() read before the first prePass --
        // SandboxApp.cpp documents this window for the cluster-GI binder) behaves as before. A
        // repurposed bit, packing/size unchanged, same shape as gGiRestirParams.w below. z/w are
        // unused (the reuse-similarity tolerances are now literals in giIsSimilarSurface,
        // voxi_restir.hlsli).
        f32 viewParams[4] = {};
        // ReSTIR GI control (gGiRestirParams). x = 1 while giMode==1 is ACTUALLY running this
        // frame (giRestirWanted(): hardware, tier and giMode all agree) -- not a raw copy of
        // Settings::giMode, since the reservoir/surface-history pair are only allocated when
        // giRestirWanted() is true, and branching on the raw setting would read null-filled t12/u6/u7
        // on a device that can't run it. y = 1 once the surface-history pair also holds a real
        // previous frame (own flag, not gRtHistParams.y -- see giHistValid_ for why the shared one is
        // wrong the one frame this pair is freshly created while the shadow/reflection pair already
        // isn't). z = which of the reservoir buffer's two array slices this frame writes, sharing
        // rtHistWriteIdx_'s cadence. w = the ReSTIR-GI poison
        // debug view (giPoisonView_/setGiPoisonView), a repurposed bit so the struct's size/offsets
        // are unchanged -- published unconditionally near the top of beginShadowHistory so it reaches
        // PSMainVoxi/PSRayDriven's violet specular-ceiling marker (B1/F5, voxi.hlsl) on every giMode,
        // not only frames that reach the giRestirWanted() block below -- see beginShadowHistory's own
        // F5 comment for why. See voxi_restir.hlsli's giRestirIndirect for the giMode==1 shader side.
        f32 giRestirParams[4] = {};
    } cb_;

    // `cbuffer VoxiFrame : register(b4)` in modules/render.voxi/shaders/voxi.hlsl repeats every field
    // above by hand and nothing else checks the two agree (the same unguarded-mirror bug already
    // fixed for PathTracer's FrameCB/PcgVolume's VolumeCB). Appending here without appending there
    // reads garbage off the end of the block in every Voxi shader; inserting in the middle -- what
    // adding sceneViewportCur beside its previous-frame twin did -- shifts every field after it,
    // silently, everywhere. ALSO MIRRORED in voxi_gi.hlsli's own `cbuffer
    // VoxiFrame` (declares the full block so a caller can bind giFrameConstants() verbatim, and its
    // tail comment says "when you append there, append here too") -- giRestirParams above is
    // appended there too for exactly that reason, even though nothing in its cone-only prelude reads it.
    static_assert(sizeof(FrameConstants) == 736,
                  "cbuffer VoxiFrame in modules/render.voxi/shaders/voxi.hlsl mirrors this byte for byte");
    static_assert(sizeof(FrameConstants) % 16 == 0, "must be a legal constant-buffer size");

    // ---- the GI rebuild gate: skip a revoxelisation whose result would be bit-identical ----
    // giUpdateInterval amortises the rebuild; it never removes one -- at interval 4 a static scene
    // still pays the full ~108 ms of voxelizePass+filterMips every fourth frame to recompute exactly
    // what it computed last time, and at Epic (interval 1) it pays it every frame. This gate hashes what the result depends on (same trick
    // buildGeometryTable uses via `key == rtGeometryKey_`) and reuses the volume, unchanged, when
    // nothing moved -- not an approximation, the identical answer.
    //
    // Not a static/dynamic split or an on-disk cache: both were designed and rejected on
    // correctness. PSVoxel bakes LIGHTING (albedo * sun * visibility + sky) into each voxel, not
    // material, so a static voxel's stored
    // radiance goes stale when a dynamic occluder crosses the sun's path over it -- invisible to a
    // static/dynamic split. An on-disk cache also has no readback path here and a key that includes
    // the sun, a live editor slider. Gating the whole pass has neither problem: any change to the
    // draw list, sun or volume placement falls straight through to the existing full rebuild.
    //
    // What it does not buy: streaming new chunks or dragging the time-of-day slider changes the
    // inputs every tick, same as today. The win is real only while the scene is actually still.
    bool giSnapshotUnchanged() const;
    void takeGiSnapshot();
    u64 giDrawsKey() const;
    // Whether PSVoxel bakes the sky into the volume: the negation of what prePass writes to
    // cb_.viewParams[2] (gViewParams.z in PSVoxel). The SETTING, not whether ReSTIR GI actually ran this frame -- that also goes false
    // for a debug-view or empty-TLAS frame, and keying the bake on it rebuilt the volume twice per
    // debug-view toggle. The gate and the GI cache key both carry it: a volume baked with the sky is
    // a different answer from one baked without.
    bool voxelSkyInjected() const { return !giRestirWanted(); }

    u64 giDrawsKey_ = 0;
    // Which part of the draw list moved. giDrawsKey_ alone says only "different" -- making the hash
    // order-independent was expected to stop rejects under camera rotation and changed the skip rate
    // by two points -- so these split the same inputs into independent axes to measure, not guess.
    u64 giDrawsCount_ = 0;     // how many draws were hashed
    u64 giDrawsMeshKey_ = 0;   // mesh handles only, order-independent
    u64 giDrawsWorldKey_ = 0;  // world transforms only
    u64 giDrawsMatKey_ = 0;    // material identity (hashDrawMaterialInto)
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
    // See setGiBoundedDispatch for the contract and voxelizePass for the box-selection/induction
    // argument. Backing store plus the bookkeeping a rebuild needs about the LAST rebuild's box.
    //
    // ON BY DEFAULT: measured on PTTest NewSponza -- injected-draw box is 1.3% of the 512^3 grid, but
    // clear/resolve/mip ran over 100.0% with this off vs 1.3% with it on. Ray-driven, 400 frames
    // moving camera: GPU total 30.64ms -> 30.25ms (0.39ms, ~1.3% of the frame -- real and free, but
    // deliberately not oversold: the
    // named spans it drains, voxelise 0.26ms and mip filter 0.05ms, were never the bulk of anything;
    // worth taking because the work is pure waste). Pixel-neutral, verified: full-resolution capture
    // against the same frame with it off is 99.3% bit-identical at 0.0024 mean absolute difference
    // (residual in the GI temporal noise already present frame to frame).
    //
    // The induction in voxelizePass makes it safe; every edge that could break it (no previous box,
    // an unbounded draw, the volume moving or resizing) forces a full rebuild instead.
    // setGiBoundedDispatch clears giBoxPrevValid_ on any toggle, so flipping the default cannot
    // inherit a box that was never enforced.
    bool giBoundedDispatch_ = true;
    // This rebuild's own box0 (what clear/resolve ran over), stashed so filterMips can derive each
    // mip level's box via mipBox() without voxelizePass passing it as a parameter or filterMips
    // recomputing the pre-pass walk over drawsPrev_ a second time.
    VoxelBox giDispatchBox0_{};
    // The LAST rebuild's own draws box (union of every surviving draw's world AABB, not box0 --
    // box0 also folds in THIS rebuild's draws box, and unioning it forward would let the covered
    // region grow forever instead of tracking only the two rebuilds a voxel can still depend on).
    // See voxelizePass for why the union of exactly these two draws boxes is enough.
    VoxelBox giBoxPrevDraws_{};
    // False whenever the box above can't be trusted: no rebuild has run yet, the last rebuild had an
    // unbounded draw, or the volume was just restored from the on-disk cache (giCacheRestore sets
    // this false on a hit -- a restore doesn't know what box the file's bake used). Forces the NEXT
    // rebuild's box0 to be the full grid.
    bool giBoxPrevValid_ = false;
    // The resolution/centre/extent the box above was recorded under -- a volume that moved, resized
    // or rebuilt at a different resolution since invalidates the box even though giBoxPrevValid_
    // itself is still true.
    u32 giBoxRes_ = 0;
    f32 giBoxCentre_[3] = {};
    f32 giBoxExtent_ = -1.0f;

    // ---- W12: free the injection accumulator after the gate has gone quiet for a while ----
    // See setGiFreeAccumulator for the contract and manageInjectionAccumulator() (.cpp) for the two
    // branches and the Vulkan ordering hazard that fixes where it's called from.
    // DEFAULT TRUE (gi-memory): was a measurement-only console toggle; promoted to the shipped default
    // because the accumulator it frees is the single largest idle GI allocation (2048 MiB at Epic's
    // 512, see createInjectionAccumulator) and the recreate path below is the exact code that toggle
    // already proved out. voxi.giFreeAccumulator (EditorConsole.hpp) and --gi-free-accumulator 0 both
    // still turn it back off for an A/B.
    bool giFreeAccumulator_ = true;
    // Set by the rebuild gate the tick it finds voxelAccumTex_ missing and needs it -- consumed (and
    // cleared) by manageInjectionAccumulator() the NEXT prePass (the one-tick delay
    // setGiFreeAccumulator documents). !giFreeAccumulator_ alone is also enough to recreate, so this
    // flag only matters while giFreeAccumulator_ is on.
    bool giAccumWanted_ = false;
    // Latched so a recreate failure (out of memory, most likely) warns once, not every tick -- same idiom as
    // layeredBsdfWarned_/denoiseWarnedMsaa_. Reset on the next successful recreate, so a later failure
    // with a different cause isn't silenced by an earlier one already reported.
    bool giAccumRecreateFailedLogged_ = false;
    // BACKOFF for a failed recreate (gi-memory): counts down to the next retry instead of retrying
    // every tick while it's nonzero -- see manageInjectionAccumulator's (a) branch. 0 means "try this
    // tick", the state every successful recreate resets it to.
    u32 giAccumRecreateBackoffTicks_ = 0;
    // The cooldown the NEXT failure will set giAccumRecreateBackoffTicks_ to, doubling on each
    // consecutive failure (capped at kGiAccumRecreateBackoffMax) and reset to 0 the moment a recreate
    // succeeds -- 0 here means "start at kGiAccumRecreateBackoffMin", exactly like a session that has
    // never failed yet -- so an isolated failure under momentary memory pressure is retried soon, but
    // a sustained out-of-memory condition backs off instead of spinning the allocator every frame for
    // something that keeps failing.
    u32 giAccumRecreateBackoffNext_ = 0;
    static constexpr u32 kGiAccumRecreateBackoffMin = 30;     // ~0.5 s at 60 Hz
    static constexpr u32 kGiAccumRecreateBackoffMax = 1800;   // ~30 s at 60 Hz
    // Consecutive GI ticks with nothing to do: incremented while giConvergeTicks_ == 0 (a converging
    // bake is busy, whatever the snapshot gate alone would have said), reset to 0 by
    // any rebuild. Compared against kGiAccumulatorQuietTicks below to decide when to free.
    u32 giQuietTicks_ = 0;
    // A tiny stand-in UAV (4x1x1, R32_UINT) bound to bindings_/clearBindings_/resolveBindings_ slot 1
    // in place of voxelAccumTex_ while it's freed --
    // every binding set naming a resource must be rebound before that resource is destroyed
    // (aver-view-outlives-its-buffer.md). Created
    // once on the first free, kept until shutdown().
    rhi::TextureHandle voxelAccumPlaceholder_ = 0;
    // UNMEASURED: how many quiet GI ticks before freeing -- a guess at "long enough that an idle
    // session is done lighting, short enough that scrubbing a timeline doesn't recreate every few
    // seconds". 240 ticks is a few seconds at Epic's default giUpdateInterval of 1; raising or
    // lowering it trades how eagerly the memory returns against how often an idle-then-resumed
    // session pays the recreate delay.
    static constexpr u32 kGiAccumulatorQuietTicks = 240;

    // ---- the GI derived-data cache ----
    // What it adds to the gate above: giSnapshotUnchanged avoids ~93% of rebuilds within a run but
    // remembers nothing across one, so every level load pays a full revoxelisation. This is that
    // memory -- the resolved volume, written beside the project, keyed by exactly the gate's own
    // inputs (giCacheKey() reads giDrawsKey_/giSky_/centre/extent), so a cache hit and a gate hit
    // mean the same thing by construction.
    std::string giCacheDir_;
    // Tried once per key, hit or miss (a miss must not re-read the same absent file every rebuild).
    // FIXED: keyed on the WHOLE key, not giCacheKey().drawsKey alone (one of six fields) -- keying on the draw list only
    // meant a camera move (which changes centre, reject 2, without changing the draws) rebuilt from
    // scratch even though the same volume had been cached minutes earlier; the memo is about a FILE,
    // and the file name is the whole key.
    fmt::GiCacheKey giCacheTriedKey_{};
    bool giCacheTried_ = false;
    // Readback is not immediate: the copy is a GPU command, so a bake schedules it, waits
    // kGiCacheReadbackDelay frames (longer than the deepest frame-in-flight), then reads/writes the file.
    static constexpr u32 kGiCacheReadbackDelay = 4;
    // How long a volume must stay settled before it's worth a file: quiet gate ticks after the
    // bounces converge (~2s at 60fps). Settling alone takes ~6 ticks, so without this every pause in
    // a sun drag queued a 19 MB entry for a state nobody returns to (one owner session wrote 11 files
    // on exit, 7 of them intermediate drag positions).
    static constexpr u32 kGiCacheDwellTicks = 120;
    rhi::BufferHandle giCacheReadback_ = 0;
    rhi::BufferHandle giCacheUpload_ = 0;
    u64  giCacheBufBytes_ = 0;
    u32  giCacheDumpCountdown_ = 0;   // 0 = nothing pending
    fmt::GiCacheKey giCachePendingKey_{};
    // ONLY A SETTLED VOLUME IS CACHED: a rebuild only raises giCacheSettlePending_, and the first
    // QUIET gate tick after convergence schedules the one readback -- not every rebuild, which used
    // to copy 18 MB per tick during a sun/emissive drag and flush about once a second of dragging,
    // each one caught mid multi-bounce convergence.
    bool giCacheSettlePending_ = false;
    // True while every bake since the last settled volume was triggered by the cloud clock alone
    // (giRebuildCloudOnly_ latched across the series; false again by the quiet tick). giCacheScheduleDump declines such
    // a volume, for the reason its own comment gives.
    bool giCacheSettleCloudOnly_ = false;
    // Keys this session already restored or queued, so a revisited level/sun angle doesn't rewrite a
    // file the cache already holds. Bounded like the directory itself (giCacheFlush's
    // kGiCacheKeepFiles); oldest dropped first.
    std::vector<fmt::GiCacheKey> giCacheKnownKeys_;
    bool giCacheKeyKnown(const fmt::GiCacheKey& k) const;
    void giCacheRememberKey(const fmt::GiCacheKey& k);

    // ---- the write-behind buffer ----
    // Bakes land in RAM and go to disk in batches: without this, every completed bake was an ~18 MiB
    // file write on the frame it finished, so nudging the sun for a minute meant tens of writes and a
    // directory swept after each one. Volumes now accumulate here and reach the filesystem when the
    // RAM budget is exceeded or the editor shuts down, and only SETTLED ones are queued at all.
    // Safe because of what this cache IS, not because the window is small: GiCache.hpp states the
    // contract plainly: "nothing here is authored and nothing here is precious: the whole directory
    // can be deleted at any time and the only cost is one rebuild." Losing the buffer to a crash
    // costs exactly that.
    std::vector<fmt::GiCacheEntry> giCachePendingEntries_;
    u64 giCachePendingBytes_ = 0;
    // Default 256 MiB: ~14 entries at the ~18 MiB a 128^3 volume takes, comfortably more than a
    // session produces. Editor Preferences > Derived Data Cache moves it.
    u64 giCacheRamBudget_ = 256ull * 1024ull * 1024ull;
    // True when the rebuild about to run was triggered by NOTHING but the cloud clock (the gate
    // tests clouds last so this can mean that); giCacheSettleCloudOnly_ latches it so
    // giCacheScheduleDump can decline writing a volume whose key can't describe what changed.
    // Cleared every gate evaluation. mutable because giSnapshotUnchanged is const (answers a
    // question, doesn't change the world) and this records which answer it gave -- the same reason
    // giGateWhyMask_ beside it is written from that const method.
    mutable bool giRebuildCloudOnly_ = false;

    // The one definition of "voxelizePass will inject this draw" -- giDrawsKey, giDrawsSubKeys and
    // voxelizePass must all agree exactly; they used to hand-copy three predicates and drifted on the
    // volume-bounds cull. One function is how that stops happening.
    bool giVoxelisedDraw(const Draw& d) const;
    // Latched so the "this volume is bigger than the whole budget" warning is said once, not once per bake.
    // Never cleared: raising the budget later doesn't make the earlier warning wrong.
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
    // Schedules a readback of the settled voxelTex_ so it can be buffered once the GPU is past it.
    // False only when it should be asked again (a readback is already in flight); true once this
    // volume needs nothing more -- copied, already known, or declined for a reason that holds until
    // the next bake.
    bool giCacheScheduleDump(rhi::IRenderContext& ctx);
    // Warns once that a volume is too big to cache. Shared by the two places that can decide it:
    // giCacheScheduleDump before the readback, giCacheTick as a guard after it.
    void giCacheWarnOversize(u64 bytes);
    // Ticks the countdown and writes the file when it reaches zero.
    void giCacheTick();
    // Sizes giCacheReadback_/giCacheUpload_ and giCacheMipOffsets_ for the current volume.
    bool giCacheEnsureBuffers();
    // gi-memory: releases giCacheReadback_/giCacheUpload_ (a matched pair, together the radiance
    // volume's own size again -- ~1170 MiB apiece at 512^3) once the copy each one exists for has been
    // RECORDED. Safe to call immediately after recording, not after the GPU has actually run the copy:
    // destroyBuffer is fence-deferred on both backends, the same guarantee createInjectionAccumulator's
    // caller relies on. giCacheEnsureBuffers() already recreates from a 0 handle on demand (its own
    // "too small, destroy and recreate" branch takes an identical path), so freeing here needs no
    // companion change there.
    void giCacheFreeBuffers();

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
    // rtShadow()/rtReflection() trace fresh rays per pixel per frame with no cross-frame reuse (see
    // module README's "Next" section and STATUS.md 4d item 4). This blends each frame's fresh sample with a REPROJECTED
    // sample of the previous frame's result, so a static or slow-moving result converges toward the
    // old brute-force ray count over several frames instead of paying for it every frame.
    //
    // PING-PONGED, not one texture: reprojection reads a different texel than the one this frame
    // writes, so read/write on the same resource in the same frame would race. Shadow and reflection
    // have their own texture pairs (shape differs) but share one write index, valid flag, frame
    // index and tile schedule -- both resize/swap/invalidate in lockstep, driven by the same "is RT
    // active" condition.
    //
    // Two RG32Float textures (x = visibility, y = linear depth) for the shadow; two RGBA16F textures
    // (rgb = reflected colour, a = linear depth or a NEGATIVE sentinel meaning "this ray missed,
    // nothing to reuse" -- see rtReflectionTemporal) for reflections. One texture per pair is this
    // frame's write target (UAV), the other last frame's result (SRV); the depth channel tells a
    // disocclusion apart from a legitimate reprojection.
    rhi::TextureHandle rtShadowHist_[2] = {0, 0};
    rhi::TextureHandle rtReflHist_[2] = {0, 0};
    // THE THIRD PAIR, for the sky-occlusion ray: at one ray/pixel that estimator is a BINARY mask
    // (`open = hit ? 0 : 1`), the noisiest thing a Monte Carlo estimate can be. Firing four
    // incoherent rays per 4x4 tile trades salt-and-pepper for visible blocks; accumulating against a reprojected history
    // buys the sample count over time instead: one ray, no tile, no blocks.
    // RG32Float like the shadow pair: x = openness, y = linear depth for the disocclusion test.
    rhi::TextureHandle rtAoHist_[2] = {0, 0};
    // THE SKY-OCCLUSION RAY'S HIT DISTANCE, [0,1] as a fraction of the ray's own TMax
    // (Settings::giMaxDistance). Not ping-ponged, not a history: this frame's raw measurement,
    // overwritten whole every frame. Nothing in Voxi's own passes reads it: it is the signal the
    // denoiser filters (Aver.Render.Denoise's one-channel variant), and its output is read back at
    // t14. R16Unorm, one channel: a fraction, and 2 bytes/pixel beside the 112 MB the RG32Float
    // history pair already costs is not worth optimising further.
    rhi::TextureHandle rtAoHitDist_ = 0;

    // ---- THE DENOISER (Aver.Render.Denoise: AMD FidelityFX Denoiser), over both noisy signals ----
    // The sky-occlusion hit distance above and the ReSTIR GI radiance below, each with its own
    // history. Every input but the signal comes from the G-buffer (view Z, motion vectors, packed
    // normal), off by default; a null G-buffer input is refused by Denoiser::record, so the failure
    // is a log line and an undenoised frame, never a crash. Optional at every level, deliberately:
    // absent without the G-buffer (which only D3D12 provides), absent if its shaders fail to compile.
    // An output of 0 means "not denoised this frame", falling back to the hand-written temporal
    // filter every tier below already ships.
    render::denoise::Denoiser denoiser_;
    rhi::TextureHandle denoiseAoOutput_ = 0;   // denoised sky occlusion (t14), 0 when not denoised
    rhi::TextureHandle denoiseGiOutput_ = 0;   // denoised ReSTIR GI radiance (t15), 0 when not denoised
    // Advanced once per frame at the top of beginShadowHistory: its low bit is half-rate GI's parity.
    u32  denoiseFrame_ = 0;
    bool denoiseWarnedMsaa_ = false;
    // 3.4 b: set the first time this frame's G-buffer inputs (viewZ/motionVectors/normalRoughness)
    // disagree in size with the signal (rtAoHitDist_/giRadiance_) the denoiser is about to be
    // resized to -- see beginShadowHistory's rect-vs-resource guard for why this can happen even
    // though targets are reallocated together on an ordinary resize (3.4 a). Cleared once sizes
    // agree again, UNLIKE the warned-once flag above: this is expected to self-heal within a frame
    // or two, and a later unrelated mismatch should still warn.
    bool denoiseWarnedInputSizeMismatch_ = false;
    // The ReSTIR GI radiance handed to the denoiser (u9): rgb linear indirect diffuse. NOT
    // ping-ponged, unlike every history pair here: this frame's raw measurement handed to a filter
    // that keeps its own history. Allocated only when ReSTIR GI is the active estimator.
    rhi::TextureHandle giRadiance_ = 0;
    // ---- MILESTONE 4: half-rate ReSTIR GI on a checkerboard (rayDrivenStages == 2) ----
    // denoiseGiRanThisFrame_: true only when THIS frame's denoiser dispatch actually produced the GI
    // output. recordStagedRayDriven (later this frame) reads this to decide whether CSRdGi may skip
    // half the pixels and trust the denoiser to reconstruct them -- a frame whose GI readback never
    // ran (denoiser off, unavailable, or ReSTIR GI not the estimator) must trace every pixel instead.
    bool denoiseGiRanThisFrame_ = false;
    // Whether the GI radiance the denoiser is about to read was itself traced at half rate (LAST
    // frame's CSRdGi), and with which parity. Latched at the very top of beginShadowHistory from
    // giCbWrittenThisFrame_/giCbParityWritten_ below.
    bool denoiseGiInputHalfRate_  = false;
    u32  denoiseGiHalfRateParity_ = 0;
    // WHILE THE SUN MOVES, THE DENOISER KEEPS A SHORT HISTORY -- see Settings::
    // denoiserSunMovingSamples. Counts down from 2 on every frame the sun moved, since the denoiser
    // reads LAST frame's GI write -- the frame after the sun stops still carries a moving-sun input.
    u32  denoiseSunMovingHold_ = 0;

    // ---- ReSTIR GI: the reservoir buffer and the previous-frame surface it resamples against ----
    // giReservoirs_ holds GiPackedReservoir (voxi_reservoir.hlsli), 32 bytes each; one
    // RWStructuredBuffer holds BOTH ping-pong slices, row-major per slice
    // (index = slice*(w*h) + y*w + x, giReservoirIndex) -- one buffer, one binding (u6), never
    // rebound mid-frame; only which slice is read/written changes (cb_.giRestirParams.z), not a
    // descriptor swap. Sized from giSurfPosHist_'s own resolution, not a separately-tracked
    // width/height: both are created together in ensureShadowHistory from the same width/height,
    // and the shader derives the identical w*h slice pitch from gGiSurfNrmHist.GetDimensions()
    // (voxi_restir.hlsli's giReservoirIndex) -- one source of truth rather than a cbuffer field
    // that could drift from the texture it describes.
    rhi::BufferHandle giReservoirs_ = 0;
    // Element count (GiPackedReservoir units, w*h*2) the buffer was sized for -- caching this means
    // a resize that neither outgrows it nor drops below half of it (perPixelBufferNeedsRealloc, the
    // .cpp) skips the destroy/recreate entirely, while a render scale applied after start-up still
    // gives the memory back.
    u32  giReservoirElemCapacity_ = 0;

    // ---- STAGED RAY-DRIVEN PASSES (milestone 1): the resources CSRdVisibility/CSRdShadow/the
    // AVER_RD_SPLIT pixel shader pass a record through, u11/u12 in every Voxi binding set. ----
    // rdVisBuf_: one uint4 (16 bytes) per pixel of the SCENE RENDER TARGET (PSRayDriven's i.pos.xy
    // indexes it, which can be larger than the scene-viewport sub-rect the compute dispatches cover).
    // A StructuredBuffer, reallocated in ensureRdStagedResources only when it no longer fits the
    // target or holds more than twice what it needs -- see rdStagedRowPitch_ for the pitch this
    // buffer and cb_.viewParams.w both agree on.
    rhi::BufferHandle rdVisBuf_ = 0;
    // Element count (uint4 units) rdVisBuf_ was sized for -- same 2x-band cache as
    // giReservoirElemCapacity_.
    u32  rdVisBufElemCapacity_ = 0;
    // rdSunVisTex_: RGBA16F, rgb = the sun ray's transmittance (CSRdShadow), same resolution as
    // rdVisBuf_. A plain RW 2D texture, not ping-ponged: nothing here accumulates temporally (that's
    // rtShadowHist_'s job); this is just this frame's raw result handed from Stage S to Stage B.
    rhi::TextureHandle rdSunVisTex_ = 0;
    // MILESTONE 2's lighting-stage output pair, u13/u14, sharing rdSunVisTex_'s exact lifecycle (same
    // size/shape, recreated outright on any size change, created/destroyed together in
    // ensureRdStagedResources/shutdown()):
    // rdGiTex_: rgb = ReSTIR GI's indirect diffuse (giRestirIndirect's return, gRdGiTex in CSRdGi/
    // PSRayDriven's AVER_RD_SPLIT block) -- the same channels the un-split GI block reads today. a
    // is unused (always 1.0, CSRdGi's own write). Not needed when CSRdGi never dispatched (ReSTIR not the estimator, or cone-GI mode).
    rhi::TextureHandle rdGiTex_ = 0;
    // rdAoTex_: r = the sky-occlusion ray's transmittance (rtSkyOcclusionTemporal, gRdAoTex), gba
    // unused. Only worth reading on frames CSRdSkyOcc actually ran.
    rhi::TextureHandle rdAoTex_ = 0;
    // MILESTONE 3: CSRdRefl's output, u15, sharing the same lifecycle as the milestone 2 pair.
    // rdReflTex_: rgb = the ray-traced reflection's radiance (whichever of rtReflectionTemporal's
    // history or skyColor() would have picked, clamped to AVER_VOX_MAXRAD), a = the
    // stage's own decision -- 1.0 when CSRdRefl actually traced this pixel (reflections enabled and
    // surface rough enough to matter), 0.0 otherwise (miss, out-of-bounds, or too rough). Stage B's
    // AVER_RD_SPLIT branch reads that alpha rather than re-deciding with its own roughness.
    // A FOURTH ALPHA under SUB-STAGE C (Settings::rayDrivenReflSplit): alpha < -0.5 is R1's PENDING
    // marker (rgb carries that pixel's skyR); CSRdReflFilter (R2) overwrites every PENDING texel
    // before Stage B ever reads this texture, so Stage B's contract above is unchanged. See
    // gRdReflTex's header comment (voxi.hlsl) for the full four-alpha account.
    rhi::TextureHandle rdReflTex_ = 0;
    // ---- SUB-STAGE SPLITS' OWN BUFFERS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): u17/u18,
    // sharing rdVisBuf_'s "reallocated only outside a 2x band" StructuredBuffer shape -- not
    // rdSunVisTex_'s recreate-outright shape, since these buffers have no fixed view the way a
    // Texture2D UAV has.
    // Allocated unconditionally
    // alongside every other staged resource, in lockstep with rdVisBuf_, whether or not either
    // setting is on -- so flipping the setting mid-session never finds an undersized buffer. ----
    // rdGiCandBuf_: one RdGiCand (48 bytes, voxi_restir.hlsli) per pixel, same pitch as rdVisBuf_ --
    // CSRdGiTrace writes it, CSRdGi's AVER_GI_SPLIT branch reads it back at the identical index.
    rhi::BufferHandle rdGiCandBuf_ = 0;
    u32  rdGiCandBufElemCapacity_ = 0;
    // rdShadowTileBuf_: one uint mask per 8x8 tile (ceil(W/8) x ceil(H/8) tiles) -- CSRdShadowProbe
    // writes it, CSRdShadow's AVER_RD_SHADOW_TILES branch ORs its 3x3 neighbourhood from it.
    rhi::BufferHandle rdShadowTileBuf_ = 0;
    u32  rdShadowTileElemCapacity_ = 0;
    // The render-target size the rd*Tex_/rdVisBuf_ resources were last (re)created at, and rdVisBuf_'s
    // row pitch in pixels -- the same number cb_.viewParams.w carries into the shaders (set around
    // the staged uploads in recordStagedRayDriven, 0 otherwise) that index
    // rdVisBuf_ (CSRdVisibility/CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl/PSRayDriven's AVER_RD_SPLIT branch). Tracked separately from rtShadowHistW_/H_ (though they usually agree): staged mode
    // can be switched on after ray tracing already sized the shadow history.
    u32  rdStagedW_ = 0, rdStagedH_ = 0;
    u32  rdStagedRowPitch_ = 0;
    // MILESTONE 4: giCbWrittenThisFrame_ -- written every recordStagedRayDriven call (true only when
    // it picked the checkerboard CSRdGi variant; false when CSRdGi wasn't dispatched at all this
    // frame). Consumed at the top of NEXT frame's beginShadowHistory (latched into
    // denoiseGiInputHalfRate_, then reset here), not read directly by the denoiser, since its
    // dispatch this frame reads LAST frame's CSRdGi write. giCbParityWritten_ is the checkerboard
    // parity that write used (denoiseFrame_ & 1 at the time), latched the same way.
    bool giCbWrittenThisFrame_ = false;
    u32  giCbParityWritten_ = 0;
    // Tiny stand-ins bound at u11-u15 whenever the real resources don't exist (staged mode off, not
    // D3D12, or allocation failed) -- every declared UAV slot always has a valid descriptor of the
    // right kind, same contract voxelAccumPlaceholder_ keeps for u1. Created once on first need, kept
    // until shutdown. rdGiPlaceholder_/rdAoPlaceholder_/rdReflPlaceholder_ are ordinary RGBA16F 1x1
    // UAV textures like rdSunVisPlaceholder_ -- three separate handles mirroring rdGiTex_/rdAoTex_/
    // rdReflTex_ being three independent resources, not a ping-pong pair.
    rhi::BufferHandle  rdVisBufPlaceholder_ = 0;
    rhi::TextureHandle rdSunVisPlaceholder_ = 0;
    rhi::TextureHandle rdGiPlaceholder_     = 0;
    rhi::TextureHandle rdAoPlaceholder_     = 0;
    // MILESTONE 3's own placeholder, same shape as rdGiPlaceholder_/rdAoPlaceholder_.
    rhi::TextureHandle rdReflPlaceholder_   = 0;
    // SUB-STAGE SPLITS' OWN PLACEHOLDERS, same 1-element StructuredBuffer shape as
    // rdVisBufPlaceholder_ (index-addressed buffers, never texture views).
    rhi::BufferHandle rdGiCandBufPlaceholder_     = 0;
    rhi::BufferHandle rdShadowTileBufPlaceholder_ = 0;
    // (Re)creates or releases the staged resources for the given render-target size, rebinding
    // u11/u12/u13/u14/u15/u17/u18 to the placeholders above when staged mode isn't wanted or the
    // size is 0. Called from onRenderTargetsChanged (resize) and setSettings (rayDrivenStages
    // on/off edge) -- same two call sites ensureShadowHistory has, for the same reason.
    bool ensureRdStagedResources(u32 width, u32 height);
    // Whether Settings::rayDrivenStages asks for the staged split at all -- not whether it will run
    // this frame (rdStagedActive()). >= 1u, not == 1u: MILESTONE 4's value 2 (half-rate ReSTIR GI on
    // a checkerboard) still wants every pass value 1 wants; it's an addition, not a new path.
    bool rdStagedWanted() const { return settings_.rayDrivenStages >= 1u; }
    // Whether the staged resources are worth ALLOCATING at all -- rdStagedWanted() plus
    // rayTracingWanted(), the same VRAM-consciousness aoHistoryWanted()/giRestirWanted() apply: the
    // staged compute passes read the same RT geometry/instance tables (gRtInstances/gRtVerts/gRtIndices) the single-pass primary does,
    // so there's nothing to do while ray tracing is off. rdGiTex_/rdAoTex_/rdReflTex_ ride this same
    // wanted-ness rather than their own narrower one (giRestirWanted()/aoHistoryWanted()/the reflections-enabled test), since all three are allocated unconditionally
    // alongside rdSunVisTex_ (a placeholder must always exist once a real resource ever has).
    bool rdStagedResourcesWanted() const { return rdStagedWanted() && rayTracingWanted(); }
    // Whether THIS frame's ray-driven primary will run as staged passes rather than the single
    // PSRayDriven draw. Requires, in order: rdStagedWanted(), rayDrivenActive() (a primary to stage
    // at all), D3D12 (compute inside the scene pass is Vulkan-illegal -- see
    // D3D12RenderContext::dispatch/setPipeline, verified read-only), rdVisCsPso_/rdShadowCsPso_,
    // the staged resources, and the TEXTURED ray-driven pipeline scenePass() would bind this frame
    // (matching pickGbuf()'s plain/G-buffer pick) together with its own AVER_RD_SPLIT twin.
    //
    // MILESTONE 2's own conditional requirement: rdGiCsPso_/rdSkyOccCsPso_ are checked only when
    // this frame's cb_ says the matching dispatch would fire in recordStagedRayDriven() --
    // cb_.voxelParams[3] > 0.5 && cb_.giRestirParams[0] > 0.5 for CSRdGi, cb_.ambientParams[0] > 0.5
    // && (cb_.giRestirParams[0] > 0.5 || cb_.voxelParams[3] <= 0.5) for CSRdSkyOcc -- the same
    // conditions PSRayDriven's own AVER_RD_SPLIT branch tests before trusting gRdGiTex/gRdAoTex. A
    // project that never enables ReSTIR GI/sky occlusion keeps full staged mode even if those
    // pipelines never compiled; one that does and finds the pipeline missing falls back for the
    // whole frame, since Stage B would otherwise read a texture nothing wrote this frame.
    //
    // MILESTONE 3's own requirement, same shape: rdReflCsPso_ is checked only when cb_.shadowParams[2]
    // > 0.5 && cb_.rtParams[3] > 0.5 says CSRdRefl would fire (the per-pixel roughness half of the
    // gate has no CPU-side equivalent, which is why gRdReflTex.a exists -- see rdReflTex_).
    //
    // `reason`, when non-null, is set to a human-readable explanation the ONE time this returns false
    // while rdStagedWanted() && rayDrivenActive() -- scenePass() logs it once and falls back, never
    // touching it again once rdStagedFallbackLogged_ latches. Left null when rdStagedWanted() or
    // rayDrivenActive() alone said no, since neither is a fallback worth logging.
    bool rdStagedActive(const char** reason = nullptr) const;
    // Latches the ONE fallback warning rdStagedActive()'s `reason` produces -- same "said once"
    // idiom as giAccumRecreateFailedLogged_/layeredBsdfWarned_.
    bool rdStagedFallbackLogged_ = false;
    // The other half: said once, the first frame the staged passes actually record, so a start-up
    // fallback (RT history not ready yet) isn't the last word.
    bool rdStagedRunLogged_ = false;
    // MILESTONE 4's own "said once, each half of the story" pair: rdGiCbRunLogged_ the first frame
    // recordStagedRayDriven dispatches the checkerboard CSRdGi variant, rdGiCbFallbackLogged_ the
    // first frame rayDrivenStages == 2 is requested and staged but the checkerboard dispatch did
    // NOT happen (naming which of giDispatch/rdGiCbCsPso_/denoiseGiRanThisFrame_ said no) -- see recordStagedRayDriven for where each fires.
    bool rdGiCbRunLogged_ = false;
    bool rdGiCbFallbackLogged_ = false;
    // SUB-STAGE SPLITS' OWN PAIRS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit /
    // rayDrivenReflSplit), the same "said once, each half" shape: *RunLogged_ the first frame each
    // split actually dispatches its probe/trace/filter pass, *FallbackLogged_ the first frame the
    // setting is on but the split didn't run because a pipeline was missing -- see
    // recordStagedRayDriven.
    bool rdShadowTilesRunLogged_ = false;
    bool rdShadowTilesFallbackLogged_ = false;
    bool rdGiSplitRunLogged_ = false;
    bool rdGiSplitFallbackLogged_ = false;
    bool rdReflSplitRunLogged_ = false;
    bool rdReflSplitFallbackLogged_ = false;

    // ---- ReSTIR GI: the previous-frame surface it resamples against (giSurfPosHist_/
    // giSurfNrmHist_) ----
    // THE ENGINE GAP: giLoadPrevSurface(idx) needs a PREVIOUS frame's primary
    // surface, and nothing in Voxi carried one before this pair -- the deferred G-buffer is
    // current-frame-only, and AVER_GBUFFER_HISTORY's gGBufNormalHist (voxi.hlsl) is an unfinished
    // half-attempt at the same gap (declared, gated behind a define nothing sets to 1, never grew a
    // ping-pong pair). Left as found: reusing that admittedly-guessed slot for a different feature
    // (a crease term gated on the G-buffer) risks the drift its own comment warns about, and ReSTIR
    // GI needs to work whether or not the G-buffer is enabled. This is a new pair.
    //
    // TWO RG32Float TEXTURES, NOT ONE RGBA32F: rhi::Format has no four-channel 32-bit float format
    // (checked against the enum directly: only RGBA16F/R32Float/RG32Float), so a single texture wasn't an option. Full 32-bit precision
    // matters here: giReconnectionJacobian's partial-Jacobian terms are distance-squared RATIOS,
    // and RGBA16F's ~11-bit mantissa's relative error is already comparable to that ratio at a few
    // thousand centimetres of scene extent.
    //
    // giSurfPosHist_: xy = world position x/y. giSurfNrmHist_: x = world position z, y =
    // giOctEncode's packed uint (octahedral snorm16x2), bit-reinterpreted with asfloat -- the same
    // packing the reservoir normal uses. Position/normal split across the pair (not
    // xy/z-of-position in one, normal whole in the other) so a reader wanting just the position
    // or just validity knows which texture to touch.
    //
    // 0 in the packed-normal channel means "nothing written here" (a sky miss, or a pixel from
    // before this pair existed): the write side nudges an exact-zero encoding to 1 so the sentinel is
    // never produced by a real normal (see the write site in voxi.hlsl). An explicit sentinel rather than trusting a fresh allocation
    // to read as zero -- not a documented API guarantee (this repo's own render-scale/device-loss and
    // stale-worktree incidents are reminders of that), and giSpatioTemporalReuse's jittered
    // temporal retries (up to 4 within 1.5 px) already tolerate an occasional false "no surface
    // here".
    //
    // Depth for the giIsSimilarSurface test is not stored a third time: re-derived as
    // `mul(float4(storedWorldPos, 1.0), gPrevViewProj).w` -- free (gPrevViewProj already exists),
    // exact, and one fewer thing that could disagree with the position it describes.
    //
    // Both ping-ponged like rtShadowHist_/rtReflHist_/rtAoHist_ (share rtHistWriteIdx_'s swap cadence
    // in beginShadowHistory) but NOT their rtHistValid_ flag -- see giHistValid_ for why sharing it
    // would be wrong.
    rhi::TextureHandle giSurfPosHist_[2] = {0, 0};
    rhi::TextureHandle giSurfNrmHist_[2] = {0, 0};

    // ---- U1/2.11: the half-resolution ReSTIR VISIBILITY history pair (t16/u10) ----
    // See giVisHistWanted() for when this is wanted (a strict subset of giRestirWanted() -- only
    // HalfResolution mode), and ensureShadowHistory/beginShadowHistory for creation and swap. Half
    // the linear dimension of giSurfPosHist_/giSurfNrmHist_, rounded up: 2.10 D/E write one
    // full-resolution pixel per 2x2 block per frame, so one texel per block suffices. RGBA16F: r =
    // F3 reuse-visibility EMA, g = F2 traced-luminance EMA, b = F2 unoccluded-sky-luminance EMA,
    // a = 1 written / 0 never (giVisReconstruct's validity test).
    rhi::TextureHandle giVisHist_[2] = {0, 0};
    // True only once a full write+swap cycle has happened with the pair bound this frame -- same
    // shape as giHistValid_ below, independent for the same reason: giRestirVisibility_ can move to
    // or from HalfResolution mid-session while giHistValid_/rtHistValid_ are already true from an
    // unrelated cycle, so trusting either would feed a "previous frame" that never existed for THIS pair.
    bool giVisHistValid_ = false;
    // RESOURCE STATE, not content trust: true once the read side was left in UnorderedAccess by an
    // earlier write since the pair was (re)created. Cleared only where the pair is destroyed, never
    // by a validity reset that leaves the textures alone.
    bool giVisHistPrimed_ = false;
    // Latched so an allocation failure warns once, not every frame -- same idiom as
    // giAccumRecreateFailedLogged_/denoiseWarnedMsaa_. Cleared on the next successful create.
    bool giVisHistFailLogged_ = false;

    // ---- LOCAL LIGHTS (LAMPS): the visibility history pair, t19 (read) / u19 (write) ----
    // Two RGBA16F textures at the shadow history's size, created/released in ensureShadowHistory
    // alongside rtShadowHist_ (gated on rdLocalHistWanted()), swapped every active frame on the SAME
    // rtHistWriteIdx_ -- so reprojecting into t19 lands on the texel the shadow history's depth test
    // just vouched for. a = accumulated visibility (only channel used; rgb stays 0). Written (u19) by
    // whichever pass shades the opaque scene with lamps: raster, single-pass, or CSRdLocalLights.
    //
    // Rests in ShaderResource like rtShadowHist_ (ShaderResource is D3D12's PIXEL_SHADER_RESOURCE):
    // raster/single-pass read t19 from PIXEL shaders.
    // Its one compute reader, CSRdLocalLights, visits NonPixelShaderResource around the staged
    // lighting group like t6 does -- a compute read in the pixel-only state is the class of bug
    // 21524cd3 fixed in exposure metering. Stage B and the staged blended replay read the u19 side,
    // which stays in UnorderedAccess.
    rhi::TextureHandle rdLocalHist_[2] = {0, 0};
    // A 1x1 RGBA16F SRV+UAV stand-in bound at both t19/u19 when the pair doesn't exist, same shape as
    // airVisPlaceholder_. Created in createVoxelVolume, destroyed in shutdown.
    rhi::TextureHandle rdLocalHistPlaceholder_ = 0;
    // The write side bound at u19 THIS frame (beginShadowHistory), 0 when the pair wasn't bound --
    // what localLightsReady() tests. Recorded rather than re-derived from rtHistWriteIdx_, which
    // endShadowHistory has already flipped by the time scenePass runs.
    rhi::TextureHandle rdLocalOutThisFrame_ = 0;
    // RESOURCE STATE, not content trust: true once the read side was left in UnorderedAccess by an
    // earlier active frame since the pair was (re)created. Cleared only where the pair is destroyed.
    bool rdLocalHistPrimed_ = false;
    bool rdLocalHistFailLogged_ = false;   // allocation failure, said once; cleared on success
    // CONTENT TRUST: the rtFrameIndex_ of the last frame a scene pass wrote u19 (0 = never) and the
    // light-list hash it wrote under -- marked wherever that pass is recorded (CSRdLocalLights, the
    // single-pass draw, or prePass's tail for raster). gCameraMedium.w is 1 only when that frame was
    // the previous one and the hash still matches -- a changed light set or skipped/lampless frame
    // restarts accumulation.
    u32 rdLocalHistFrame_ = 0;
    u64 rdLocalHistHash_ = 0;
    // "Local lights running" said once, the first frame any scene pass shades with lamps.
    bool rdLocalLightsRunLogged_ = false;
    // Settings::giRestirVisibility, cached at setSettings. 2 (HalfResolution) matches the struct
    // default (Voxi.hpp, Quality::Medium's ladder rung), so a renderer rendering before its first
    // setSettings call behaves as Medium rather than NoRay (0). 0..3 are the shader's own `& 3u`
    // modes; 4 (Cached) exists only on the CPU and is packed as wire mode 2 plus bit 128 (see
    // givis::packAmbientW), so the shader never sees a value above 3.
    u32 giRestirVisibility_ = 2;
    // RADIANCE CACHE state (stage 1). rc_ owns the accumulator/cells/info-ring buffers, the resolve
    // pipeline and its sets; it is created on the first frame Cached is wanted and destroyed when it
    // stops being (updateNeuRaC). neuracLive_ is what this frame's packAmbientW packs
    // as bit 128: Cached requested && giRestirWanted() && staged ray-driven on D3D12 && rc_.valid()
    // && the twin pipelines exist. Recomputed every frame, before beginShadowHistory reads it.
    NeuRaC rc_;
    bool neuracLive_ = false;
    u64 tlasBlasGeneration_ = 0;   // IResourceFactory::blasGeneration at the last full TLAS build
    u64 tlasBuiltBlasGeneration_ = 0;   // ... at the last tlas_ build or refit
    std::vector<u32> voxDrawZ_;   // voxelizePass: each draw's voxel z range, pairs [lo, hi)
    u32 neuracView_ = 0;   // setNeuRaCView: mode in bits 0-2, grid bit 3 (packed into gAmbientParams.w << 8)
    // The NeuRaC::Bindings::generation last written into table 0's t22/u20/u21; a different
    // value from beginFrame means the buffers changed and bindings_ must be rewritten (before the
    // first bind of the frame -- Vulkan ringed sets forbid writing a bound set).
    u32 rcBoundGeneration_ = 0;
    // True while t22/u20/u21 hold real cache buffers. false = null-filled (never activated) or on
    // the placeholder (torn down).
    bool rcSlotsBound_ = false;
    // 64 B UAV-capable stand-in rebound to u20/u21 at teardown (a UAV slot cannot be cleared once
    // set -- setUav/setUavBuffer refuse handle 0), so no descriptor outlives the buffer it names.
    // Created lazily at teardown only, never if Cached was never requested.
    rhi::BufferHandle rcPlaceholder_ = 0;
    // Once-only logs: "Cached unsupported here, acting as HalfResolution" and the twin-build result.
    bool rcUnsupportedLogged_ = false;
    // rc_.create() failed (it logged why): do not retry every frame. Cleared when Cached stops being
    // the wanted mode, so switching away and back gets a fresh attempt.
    bool rcCreateFailed_ = false;
    // createNeuRaCTwins ran since the last createScenePipelines (which clears it): a failed
    // build is not retried every frame, but a pipeline rebuild retries against fresh bytecode.
    bool rcTwinsTried_ = false;
    // Build/teardown/per-frame hooks (VoxiRenderer.cpp). updateNeuRaC runs in prePass between
    // buildLocalLights and beginShadowHistory; createNeuRaCTwins builds the four
    // AVER_NEURAC=1 compute pipelines; teardownNeuRaC rebinds t22/u20/u21 away from
    // the buffers and then destroys them.
    void updateNeuRaC(rhi::IRenderContext& ctx);
    bool createNeuRaCTwins();
    void teardownNeuRaC();
    // Settings::giRestirSpatialSamples, cached the same defensive way: Voxi.cpp clamps it to [0,15]
    // and std::min repeats the ceiling here so this can't disagree with packAmbientW's `& 15u` mask.
    // 15 (AUTO) matches the struct default, so pre-setSettings frames leave the motion discount's
    // numSamples alone rather than forcing temporal-only reuse.
    u32 giRestirSpatialSamples_ = 15;
    u32 giRestirMaxHistory_ = 1;       // Settings::giRestirMaxHistory, gAmbientParams.w bits 18-23
    // voxi.blendedGiCone's live backing store -- see setBlendedGiCone.
    bool blendedGiCone_ = false;
    // voxi.giVisPathView's live backing store -- see setGiVisPathView.
    bool giVisPathView_ = false;

    // False right after the pair is (re)created (construction, resize, or giMode's on/off edge) and
    // true only once a full write+swap cycle has happened with giRestirWanted() true.
    //
    // DELIBERATELY SEPARATE FROM rtHistValid_: rtShadowHist_/rtReflHist_/rtAoHist_ all share that one
    // flag because they're driven by the same condition family (rayTracingWanted(),
    // aoHistoryWanted()), so by the time any exists,
    // rtHistValid_ already means "been through a real cycle". giMode is an INDEPENDENT switch a user
    // can flip mid-session while rtHistValid_ is already true from the shadow/reflection pair cycling
    // -- trusting the shared flag would feed the resampling a "previous frame" that never existed
    // for this freshly-allocated pair. Its own flag closes exactly that gap and no other.
    bool giHistValid_ = false;
    // RESOURCE STATE, not content trust like giHistValid_ above: true once the read side was left in
    // UnorderedAccess by a write since the pair was (re)created. Cleared only where destroyed.
    bool giHistPrimed_ = false;

    u32  rtShadowHistW_ = 0, rtShadowHistH_ = 0;
    u32  rtHistWriteIdx_ = 0;
    // False right after creation or a resize: the textures hold no real previous frame yet, and
    // cb_.rtParams.w must say so rather than let the shader blend against garbage.
    bool rtHistValid_ = false;
    // RESOURCE STATE, not content trust: true once rtShadowHist_/rtReflHist_/rtAoHist_'s read side was
    // left in UnorderedAccess by a write since the pairs were (re)created. Cleared only where the pairs
    // are destroyed, never by a validity reset (setSettings, resetRtHistory/resetAoHistory, the
    // skipped-frame branch) that leaves the textures alone.
    bool rtHistPrimed_ = false;
    // THE SUN THE HISTORY WAS ACCUMULATED UNDER. The temporal denoiser blends up to 90% of the
    // previous frame's visibility with a purely GEOMETRIC validity test (reprojection + depth) that
    // knows nothing about the light moving -- with a still camera and a moving sun, the shadow keeps
    // ~90% of a value traced against the OLD sun direction: correct ray, picture ten frames behind --
    // this is "the shadows don't update properly when the light has moved".
    //
    // THE SUN FIELDS ONLY, not the whole SkyAtmosphere (comparing all of it would be a regression, not
    // a fix): the struct also carries a cloud clock that advances every frame, and comparing all of it
    // would invalidate the history every frame, undoing the denoiser entirely. giSnapshotUnchanged
    // excludes cloudTime for the same reason.
    f32  rtHistSunDir_[3]   = {0, 0, 0};
    f32  rtHistSunColor_[3] = {0, 0, 0};
    f32  rtHistSunIntensity_ = -1.0f;   // negative so the first frame always counts as a change
    // True when this frame's sun differs from the one above; see beginShadowHistory.
    bool rtHistSunMoved() const;
    // True when it differs by MORE than one slider-drag step (direction beyond kGiSunJumpDeg, or
    // colour/intensity beyond kGiSunJumpRel). The ReSTIR reservoir void keys on this, not
    // rtHistSunMoved -- see giRestirParams[1] in beginShadowHistory for why a drag keeps its reuse
    // and a jump must not.
    bool rtHistSunJumped() const;
    // This frame's camera view-projection, captured where fitCascades() already reads the camera,
    // and copied into prevViewProj_ at the end of prePass for NEXT frame's cb_.prevViewProj.
    f32  curViewProj_[16] = {};
    f32  prevViewProj_[16] = {};
    // Same idea, for the scene viewport rect the reprojected NDC needs (IDevice::sceneViewport) --
    // read fresh each frame in beginShadowHistory rather than piggy-backing on fitCascades.
    f32  curSceneViewport_[4] = {};
    f32  prevSceneViewport_[4] = {};
    // (Re)creates BOTH rtShadowHist_ and rtReflHist_ at the given resolution if they don't already
    // match, resetting rtHistValid_ when it does. DESTROYS all four instead when rayTracingWanted()
    // is false.
    bool ensureShadowHistory(u32 width, u32 height);
    // Rebinds t19/u19 to rdLocalHistPlaceholder_, then destroys rdLocalHist_ and clears its flags.
    void releaseLocalHistory();
    // The size onRenderTargetsChanged last asked for -- kept because the histories are also created/
    // destroyed on the ray-tracing on/off edge seen in setSettings(), which isn't told a resolution.
    u32  rtHistWantW_ = 0, rtHistWantH_ = 0;
    // The pbr::MaterialGraphRegistry revision the scene pipelines were last compiled against. See
    // prePass, which rebuilds them when it moves. ~0 so the first frame can't accidentally match.
    u64  scenePipelineGraphRev_ = ~0ull;
    // The shader-file revision these pipelines were compiled from -- a PULL on a number, like
    // scenePipelineGraphRev_ above, stamped where they're built (not left at a sentinel): ~0 here made
    // the very first prePass rebuild every scene pipeline once, for nothing, on every launch -- caught
    // via a hot-reload log reading "shader files changed (revision 0)" before anything had changed.
    u64  scenePipelineShaderRev_ = 0;
    // Whether anything will EVER write the ray-traced histories under the current settings. Four
    // textures (2x RG32Float + 2x RGBA16F) at scene render size (32 bytes/pixel between them -- 144 MB
    // at 2750x1639, 225 MB at 3532x1987) used to be allocated whenever GI came up, with no reference to
    // ray tracing -- so a project that never writes a RENDER.RAYTRACING key (rayTracing off by default)
    // paid well over 100 MB of VRAM for a feature that never runs, and VRAM pressure severe enough to
    // force eviction looks like an unexplained frame-rate drop.
    bool rayTracingWanted() const { return rtSupported_ && settings_.rayTracing != Quality::Off; }
    // The AMBIENT history pair is wanted only where the ray that fills it is traced -- High and Epic
    // (Low/Medium keep the cone gather's own occlusion; giSkyOcclusionRaysForQuality returns 0).
    // Allocating it elsewhere is 112 MB of full-screen target at 3532x1987 held for a feature that
    // doesn't run, the same argument ensureShadowHistory makes for releasing everything when ray
    // tracing is off.
    bool aoHistoryWanted() const { return rayTracingWanted() && settings_.giSkyOcclusionRays > 0; }
    // Whether ReSTIR GI (giMode == 1) will ACTUALLY run this frame: ray tracing wanted (its candidate
    // ray needs the same acceleration structure/geometry table the shadow/reflection rays use) AND
    // giEnabled() (giMode only matters for the diffuse indirect term, both call sites nesting it inside
    // `gVoxelParams.w > 0.5`) AND giMode_ itself. Gates giReservoirs_/the surface-history pair the
    // same way aoHistoryWanted() gates the ambient pair, and is what cb_.giRestirParams.x reports --
    // never the raw setting, so hardware that can't run it gets the cone gather back silently rather
    // than a shader reading null-filled t12/u6/u7.
    bool giRestirWanted() const { return rayTracingWanted() && giEnabled() && giMode_ == 1u; }

    // U1/2.11: whether the half-resolution ReSTIR VISIBILITY history pair (t16/u10, giVisHist_) is
    // wanted -- a STRICT SUBSET of giRestirWanted(), further gated on giRestirVisibility ==
    // HalfResolution (2) or Cached (4, which packs as wire mode 2 and so traces exactly the same
    // half-res pixels and reads the same pair); Full (3), Reconstructed (1) and NoRay (0) never touch
    // this pair, so allocating it for them would be VRAM for a mode that isn't running. Gates
    // giVisHist_'s allocation in ensureShadowHistory the same way giRestirWanted() gates the
    // surface-history pair's.
    bool giVisHistWanted() const {
        return giRestirWanted() && (giRestirVisibility_ == 2u || giRestirVisibility_ == 4u);
    }

    // RADIANCE CACHE (stage 1): whether Cached mode (giRestirVisibility_ == 4) is the requested
    // mode AND ReSTIR GI is running. Whether the cache is actually LIVE this frame is the narrower
    // neuracLive_ (also needs staged ray-driven on D3D12, a created cache and built twins).
    bool neuracWanted() const { return giRestirWanted() && giRestirVisibility_ == 4u; }

    // LOCAL LIGHTS (LAMPS): whether rdLocalHist_ is worth allocating -- ray tracing on, localLights
    // on, and D3D12 (the only backend buildLocalLights fills a list on). Every ray-traced scene mode
    // (raster, single-pass, staged) lights with lamps, so neither render mode nor rayDrivenStages gates it; two full-screen RGBA16F
    // textures held with ray tracing off or on Vulkan would be VRAM for nothing, same argument as
    // aoHistoryWanted()/giVisHistWanted(). Not gated on rdLocalLightsCsPso_: only the staged path
    // needs it, and a hot-reload can change that with no edge ensureShadowHistory sees. dev_ is null
    // before init(), reading as "not wanted" until onRenderTargetsChanged asks again.
    bool rdLocalHistWanted() const {
        return rayTracingWanted() && settings_.localLights && dev_ && dev_->backend() == rhi::Backend::D3D12;
    }

    // Fog occlusion: whether airVisTex_ will ACTUALLY be created/kept -- the setting alone isn't
    // enough, same "ask what will really run" shape as aoHistoryWanted()/giRestirWanted(), since
    // airVisPso_ can be 0 below SM 6.0 or without DXC. Gates ensureAirVis()'s create/release branch
    // and setSettings()'s own edge, the same pattern wasAoWanted/wasGiRestirWanted follow.
    bool airVisWanted() const { return settings_.fogOcclusion && airVisPso_ != 0; }

public:
    // THE SKY-OCCLUSION RAY'S HIT DISTANCE FOR THIS FRAME, or 0 when the ray isn't running at this
    // tier (Low/Medium never trace it) or ray tracing is off. See rtAoHitDist_. Rests in
    // ResourceState::UnorderedAccess -- a caller must transition before sampling; this hands out a
    // handle, not a promise about state, like every other texture accessor that hands out something a
    // different pass wrote. 0 is the answer, not an error: skip the denoiser rather
    // than filter a texture that doesn't exist.
    [[nodiscard]] rhi::TextureHandle ambientHitDistanceTexture() const { return rtAoHitDist_; }

private:
    // Path tracing wanted -- a different question from ray tracing wanted, deliberately asking the
    // other setting: both need the hardware, but a project may want ray-traced shadows with no path
    // tracing (the default).
    bool pathTracingWanted() const { return rtSupported_ && settings_.pathTracing != Quality::Off; }
    // The DEBUG RAYMARCH has taken over the scene. Split out from suppressesScene() when a second
    // reason to suppress arrived (rayDrivenActive() below) -- conflating the two would be a bug, not
    // a tidiness question (see shadowHistoryActive()).
    bool debugViewActive() const { return giReady_ && giEnabled() && debugView_; }
    // RAY-DRIVEN PRIMARY VISIBILITY is running this frame: asked for, device can trace rays, and
    // PSRayDriven compiled. Any failure falls back to the rasteriser -- a working image, not a black one.
    bool rayDrivenActive() const { return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0; }

public:
    // PREDICTS suppressesScene() (debugViewActive() || rayDrivenActive()) for THIS frame, for the one
    // caller that needs an answer before this frame's real one exists: SandboxApp::syncPtSceneView(),
    // called from onUpdate() before beginFrame() runs this frame's prePass() (see that function's own
    // header comment for why it must run there).
    //
    // debugViewActive() is read straight, no race: it depends on giReady_/giEnabled()/debugView_,
    // none of which prePass() is about to touch.
    //
    // rayDrivenActive() is the racy half being fixed: it reads rtActive_, which is reset false at the
    // top of buildAccelerationStructures() and set true only after a build (or, with
    // rtSkipUnchangedTlas, an rtAccelSnapshotUnchanged()-confirmed skip) over drawsPrev_ -- either way,
    // "the TLAS is usable this frame", all this race cares about. buildAccelerationStructures()
    // runs inside THIS frame's beginFrame(), AFTER beginScene() has already done
    // `drawsPrev_.swap(draws_); draws_.clear();` -- so the drawsPrev_ this frame's build is about to consume was already consumed by LAST
    // frame's build; reading rtActive_ here answers last frame's question a second time. Reading
    // draws_ (today's, unswapped, whatever onRender submitted last frame) answers the question THIS
    // frame's election is actually about to ask.
    //
    // rtRenderMode_ and rayDrivenPso_ carry no such race: neither is reset or reassigned inside
    // beginFrame, only by setSettings() or pipeline creation, long before any reading frame starts.
    //
    // NOT REPLAYED: whether the per-draw loop over drawsPrev_ actually yields a non-empty TLAS
    // instance list -- buildAccelerationStructures() can still end with rtActive_ == false on a
    // non-empty draws_ if every mesh fails to produce a BLAS (e.g. destroyed since last drawn).
    // draws_ non-empty is treated as sufficient: the gap this leaves is the same shape as the
    // one-frame race being fixed (a rare, brief disagreement), not a new kind, and only opens the frame a draw's mesh is destroyed
    // the very frame it would otherwise enter the TLAS.
    bool willSuppressSceneThisFrame() const {
        const bool rayDrivenWillBeActive = rtSupported_ && settings_.rayTracing != Quality::Off &&
                                            !draws_.empty() && rtRenderMode_ == 1u &&
                                            rayDrivenPso_ != 0;
        return debugViewActive() || rayDrivenWillBeActive;
    }

private:
    // Whether the ray-traced history textures will be read and written this frame. Tests
    // debugViewActive(), NOT suppressesScene() -- the debug raymarch replaces shading entirely (no
    // history touch needed), but PSRayDriven still calls rtShadowTemporal like PSMainVoxi does, so it
    // needs the history prepared; asking suppressesScene() here would leave it sampling and writing
    // resources this frame never transitioned. Deliberately does not
    // require the ambient pair (High/Epic only) -- that would switch shadow accumulation off at
    // Low/Medium as a side effect; the ambient path has its own flag, gRtDenoiseParams.w.
    bool shadowHistoryActive() const {
        return rtActive_ && rtShadowHist_[0] && rtShadowHist_[1] &&
               rtReflHist_[0] && rtReflHist_[1] && !debugViewActive();
    }
    // Swaps the read/write roles, transitions all six textures, rebinds them and sets
    // cb_.prevViewProj/rtHistParams for this frame. Called before shadowPass() so the UAVs are
    // writable and the constants are ready by the time the scene loop runs PSMainVoxi.
    void beginShadowHistory(rhi::IRenderContext& ctx);
    // Advances prevViewProj_/rtHistWriteIdx_/rtHistValid_ for NEXT frame, once fitCascades() (called
    // from shadowPass(), after beginShadowHistory) has filled curViewProj_. Called at the end of
    // prePass; shared by both histories since one write index serves both.
    void endShadowHistory();

    bool giEnabled() const { return settings_.globalIllumination != Quality::Off; }
    f32 sunDir_[3]   = {rhi::SkyAtmosphere{}.sunDirection[0],
                        rhi::SkyAtmosphere{}.sunDirection[1],
                        rhi::SkyAtmosphere{}.sunDirection[2]};
    // No sunColor_ and no ambient_ here on purpose. See setSunDirection.

    // How many extra bakes a change buys: PSVoxel's feedback term adds one bounce per rebuild, so
    // this is the bounce depth reached after the scene settles. Per-bounce gain is min(albedo x 3,
    // 0.8) per channel (AVER_VOX_MAX_BOUNCE_GAIN): 5 ticks leave 2% residual in a stone room (gain ~0.45) and a
    // third in a capped one, which later rebuilds finish. Each tick is one voxelise, paid only right
    // after something moved.
    static constexpr u32 kGiConvergeTicks = 5;
    u32  giConvergeTicks_ = 0;

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
    // See setGiPoisonView. Independent of debugView_ above: never sets debugViewActive()/suppressesScene().
    bool giPoisonView_ = false;
    // See setLightingLegacyBits' bit table. 0 (every fix in the contrast-fix plan live) until a console command or
    // --lighting-legacy sets a bit; forwarded into cb_.ambientParams[2] every frame unconditionally,
    // like giPoisonView_ feeds giRestirParams.w.
    u32 lightingLegacyBits_ = 0;
    bool unlit_ = false;   // --unlit / the viewport view-mode dropdown; see setUnlit
    // See setViewDebug. None (0) is bit-identical to every build before this view existed:
    // cb_.viewParams[0]'s composition falls straight through to unlit_ below it.
    ViewDebug viewDebug_ = ViewDebug::None;
    bool paused_ = false;   // see setPaused
    // See setConeTraceEnabled. Defaults true, bit-identical to every build before this toggle existed.
    bool coneTraceEnabled_ = true;
};

} // namespace aver::voxi
