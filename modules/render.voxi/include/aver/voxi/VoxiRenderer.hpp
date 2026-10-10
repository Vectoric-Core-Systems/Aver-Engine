// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/SceneLight.hpp"
#include "aver/voxi/SceneDecal.hpp"
#include "aver/voxi/GiDispatchBounds.hpp"   // VoxelBox/GiDispatchConstants
#include "aver/render/denoise/Denoiser.hpp"
#include "aver/render/denoise/Nrd2.hpp"
#include "aver/render/denoise/Nrd2Capture.hpp"
#include "aver/voxi/NeuRaC.hpp"      // rc_ -- the radiance cache's buffers and resolve pass

#include <unordered_map>
#include <unordered_set>
#include "aver/formats/GiCache.hpp"

#include <array>
#include <string>
#include <vector>

// Voxel cone-traced global illumination, cascaded directional shadow, and DXR 1.1 ray-traced sun shadows.
namespace aver::voxi {

// Most foliage instances setFoliage places; limited by TLAS capacity and static prefix's 64 B/instance desc buffer.
inline constexpr u32 kMaxFoliageInstances = 8'000'000;

// Voxi render feature: owns volume, shadow atlas, acceleration structures, lit pipelines.
class VoxiRenderer final : public rhi::IRenderFeature {
public:
    bool init(rhi::IDevice& device);
    void shutdown();

    void setSettings(const Settings& s);
    // GI volume: centre in world units, half-edge extent.
    void setVolume(const f32 center[3], f32 extent);
    void setSunDirection(const f32 dirToLight[3]);
    // Cache dir: "<project>\\DerivedDataCache\\GI". Empty (default) disables cache.
    void setGiCacheDir(const std::string& dir);

    void setDebugView(bool on);
    void setGiPoisonView(bool on);
    void setGiVisPathView(bool on);

    void setNeuRaCView(u32 mode, bool grid) { neuracView_ = (mode & 7u) | (grid ? 8u : 0u); }
    bool neuracLive() const { return neuracLive_; }

    void setBlendedGiCone(bool on);
    bool blendedGiCone() const { return blendedGiCone_; }

    void setLightingLegacyBits(u32 bits);

    void resetGiHistory(bool quiet = false);
    void resetRtHistory(bool quiet = false);
    void resetAoHistory();   // alias of resetRtHistory
    void resetDenoiserHistory(bool quiet = false);

    void setUnlit(bool on) { unlit_ = on; }

    enum class ViewDebug : u32 {
        None           = 0,
        RayHitInstance = 2,   // hash(instance, voxi_rt.hlsli) -> colour
        RayHitMaterial = 3,   // hash(materialIndex) -> colour
        RayHitDistance = 4,   // hit distance (cm), log heat ramp
        Triangles      = 5,   // hash(instance, primitive) -> colour
        AmbientOcclusion = 6, // final AO factor, greyscale
    };

    void setViewDebug(ViewDebug m) { viewDebug_ = m; }

    void setPaused(bool on) { paused_ = on; }

    bool rayDrivenAvailable() const { return rtActive_ && rayDrivenPso_ != 0; }
    // Some draw's first bottom-level build waits for a later frame (they are budgeted per frame).
    bool accelBuildsPending() const { return blasBuildsDeferred_; }
    // Path Tracing ran last frame: multi-bounce ReSTIR GI and reflections in the staged frame.
    bool pathTracingRan() const { return ptRanThisFrame_; }

    void setConeTraceEnabled(bool on) { coneTraceEnabled_ = on; }
    bool coneTraceEnabled() const { return coneTraceEnabled_; }

    void setShadowRays(u32 n);
    u32 shadowRays() const { return rtShadowRays_; }

    void setPixelsPerRayTile(u32 n);
    u32  pixelsPerRayTile() const { return rtPixelsPerRayTile_; }

    void setGiUpdateInterval(u32 n);
    u32  giUpdateInterval() const { return giUpdateInterval_; }

    void setGiForceRebuild(bool on);
    bool giForceRebuild() const { return giForceRebuild_; }
    void setGiBoundedDispatch(bool on);
    bool giBoundedDispatch() const { return giBoundedDispatch_; }
    void setGiFreeAccumulator(bool on);
    bool giFreeAccumulator() const { return giFreeAccumulator_; }
    f64 lastAccelBuildCpuMs() const { return lastAccelBuildCpuMs_; }

    bool layeredBsdfActive() const { return layeredBsdf_; }

    u32 voxelResolutionBuilt() const { return voxelResBuilt_; }

    void setRayDrivenAblation(u32 mode) { rdAblate_ = mode; }
    void setRtDenoiseMotionTaper(f32 v) { rtDenoiseMotionTaper_ = v; }

    void setFrameTimeReport(bool on) { frameTimeReport_ = on; }

    static constexpr u32 kMaxShadowRays = 32;
    static constexpr u32 kMaxPixelsPerRayTile = 16;
    static constexpr u32 kMaxGiUpdateInterval = 8;

    static constexpr u32 kAirVisResolution = 32;
    static constexpr u32 kAirVisSlabLayers = 2;

    void beginScene() override;
    void submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                const void* drawConstants, u32 drawConstantBytes, bool translucent = false,
                bool hiddenFromOwner = false, bool movable = false);
    void setSubmitMovable(bool on) { submitMovable_ = on; }

    // ---- scene lights (CLight): point, spot and rectangular area lights ----
    // World space (SceneLight.hpp), filled by the host from scene::gatherLights. They share the staged
    // ray-driven lamp list with emissive-material lamps (32 per frame in all), D3D12 only.
    // docs/rendering/LIGHTS.md.
    // Replaces the set; call once per frame before the frame renders. Copies.
    void setSceneLights(const SceneLight* lights, u32 count);
    // Registers a baked IES table (kIesTableV * kIesTableH half floats, fmt::bakeIesTable +
    // iesTableToHalf) under `id`. An id is immutable for the session: a repeat registration is ignored.
    bool registerIesProfile(u64 id, const u16* tableHalf, f32 peakOverMean);
    // Registers an sRGB RGBA8 cookie image under `id`.
    bool registerCookie(u64 id, u32 width, u32 height, const u8* rgba8);
    bool hasLightAsset(u64 id) const { return lightAssetsCpu_.find(id) != lightAssetsCpu_.end(); }

    // ---- projected decals (CDecal): box projectors that repaint colour, normal and roughness ----
    // World space (SceneDecal.hpp), filled by the host from scene::gatherDecals. Applied to the surface
    // before it is lit, so the raster scene and the staged ray-driven path (primary, reflection and GI
    // hits) all see them. At most kMaxSceneDecals per frame; textured decals need the bindless texture
    // table (ray-tracing capable devices) and are skipped without it. docs/rendering/DECALS.md.
    // Replaces the set; call once per frame before the frame renders. Copies. An empty set costs nothing.
    void setSceneDecals(const SceneDecal* decals, u32 count);
    // Registers an RGBA8 decal image under `id` (an sRGB colour image for kind Colour). A mip chain is
    // built on first use. An id is immutable for the session: a repeat registration is ignored.
    bool registerDecalTexture(u64 id, u32 width, u32 height, const u8* rgba8, DecalImageKind kind);
    bool hasDecalTexture(u64 id) const { return decalTexCpu_.find(id) != decalTexCpu_.end(); }
    // Decals the last prePass uploaded (after culling and the cap).
    u32 decalsDrawnLastFrame() const { return decalCount_; }
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                    const void* drawConstants, u32 drawConstantBytes, bool blended = false) override;

    const char* name() const override { return "Voxi"; }
    void prePass(rhi::IRenderContext& ctx) override;
    bool overridesScenePipeline() const override;
    rhi::BindingSetHandle sceneBindingSet() const override { return bindings_; }
    bool sceneConstants(const void** data, u32* bytes) const override;

    bool blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const override;

    rhi::PipelineHandle scenePipeline(bool meshShaders, bool depthPrepassed = false,
                                      bool blended = false) const override;
    rhi::PipelineHandle depthPrepassPipeline() const override;

    rhi::BindlessTableHandle sceneBindlessTable() const override;
    bool suppressesScene() const override;
    bool suppressesWholeFrame() const override;
    // Live, not latched: a material graph loaded mid-frame (a Project Browser open) leaves the scene set's
    // shaders without it, and drawing a material that names it (the blended replay) hung the GPU.
    bool graphStale() const;
    void scenePass(rhi::IRenderContext& ctx) override;
    // Ray-driven frames record at endFrame so moving objects use this frame's transforms.
    bool wantsLateScenePass() const override;
    // This frame's translucent draws were composited inside the ray-driven frame; skip their replay.
    bool blendedDrawsResolvedInScene() const override { return translucentInPath_; }

    // Builds the variants otherwise compiled on first use (NRD2 Stage B, Path Tracing and NeuRaC
    // twins). For tests and loading screens; call after onRenderTargetsChanged.
    void buildAllVariants();
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    pbr::MaterialSystem& materials() { return materials_; }

    void bindGiResources(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase) const;

    const void* giFrameConstants() const { return &cb_; }
    u32 giFrameConstantBytes() const { return sizeof(cb_); }

    // Instanced foliage: ray-traced only, outside draw list. TRACED but never rasterised/voxelised/path-traced.
    struct FoliagePart {
        rhi::MeshHandle mesh = 0;
        u32 material = 0;            // pbr::MaterialLibrary handle, 0 = none
        f32 color[4] = {1, 1, 1, 1};
        f32 metallic = 0.0f, roughness = 1.0f;
    };
    struct FoliagePrototype { std::vector<FoliagePart> parts; };
    // world: engine row-vector [rows 0..2 scaled basis, row 3 translation, cm]. prototype: index.
    struct FoliageInstance { f32 world[12]; u32 prototype; };
    static constexpr u32 kMaxFoliagePartsPerPrototype = 16;
    static constexpr u32 kMaxFoliageInstances = voxi::kMaxFoliageInstances;
    void setFoliage(std::vector<FoliagePrototype> prototypes, std::vector<FoliageInstance> instances);
    void clearFoliage();
    struct FoliageStats { u32 prototypes = 0, parts = 0, instances = 0; u64 gpuBytes = 0; };
    FoliageStats foliageStats() const;

private:
    bool createShadowResources();
    bool createVoxelVolume(u32 resolution);
    bool createInjectionAccumulator(u32 resolution);
    void manageInjectionAccumulator(rhi::IRenderContext& ctx);
    bool createPipelines();
    bool createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
    rhi::PipelineHandle pickGbuf(rhi::PipelineHandle plain, rhi::PipelineHandle gbuf) const;
    void shadowPass(rhi::IRenderContext& ctx);
    // Both take the slice [begin, end) of drawsPrev_: `first` clears, `last` finishes (a staged
    // rebuild calls them once per frame per slice, giBuildStep).
    void giShadowPass(rhi::IRenderContext& ctx, usize begin, usize end, bool first, bool last);
    void buildAccelerationStructures(rhi::IRenderContext& ctx);
    void voxelizePass(rhi::IRenderContext& ctx, usize begin, usize end, bool first, bool last);
    void filterMips(rhi::IRenderContext& ctx);
    // ---- STAGED GI REBUILD: a rebuild too big for one frame's GPU submission, a slice per frame ----
    // Phase 0 draws the GI shadow map, phase 1 injects into the accumulator, and the last slice
    // resolves and filters. The volume being replaced stays in use until then.
    // 16M: on NeonDistrict 1.5M triangles cost ~3 ms of GI shadow, so a slice is ~30 ms shadow or
    // ~100 ms injection, and a full rebuild takes about 25 frames.
    static constexpr u64 kGiBuildTrisPerFrame = 16000000;
    struct GiBuild {
        bool  active = false;
        u32   phase = 0;            // 0 GI shadow map, 1 voxel injection
        usize cursor = 0;           // next draw in drawsPrev_
        u32   res = 0;              // voxelResBuilt_ it started at
        u32   frames = 0;
        bool  tryCache = true;      // a restored cached volume replaces phase 1
        bool  settleCloudOnly = false;
    };
    GiBuild giBuild_;
    void giBuildStep(rhi::IRenderContext& ctx);
    // Triangles the GI passes draw for drawsPrev_[begin, end), stopping once past `stopAt`.
    u64 giBuildTriangles(usize begin, usize end, u64 stopAt) const;
    // End of the slice from `begin` that fits kGiBuildTrisPerFrame (at least one draw).
    usize giBuildSliceEnd(usize begin) const;
    // W3's box for the build in progress: computed by the first injection slice, used by the last.
    VoxelBox giBuildDrawsBox_{};
    bool giBuildAnyUnbounded_ = false;
    bool giBuildLogged_ = false;
    bool ensureAirVis();
    void dispatchAirVis(rhi::IRenderContext& ctx, u32 zLo, u32 zHi);
    void recordStagedRayDriven(rhi::IRenderContext& ctx);

    pbr::MaterialSystem materials_;

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::DeviceCaps caps_{};
    Settings settings_{};

    rhi::TextureHandle shadowTex_ = 0;
    rhi::PipelineHandle shadowPso_ = 0;
    // Via drawMeshInstanced; 0 if compile failed, shadowPass() falls back.
    rhi::PipelineHandle shadowInstancedPso_ = 0;

    // GI-only shadow map: fitted to volume, not camera. Soft failure: giShadowFactor falls back to fully-lit.
    rhi::TextureHandle  giShadowTex_ = 0;
    rhi::PipelineHandle giShadowPso_ = 0, giShadowInstancedPso_ = 0;

    // Radiance volume: RGBA16F Tex3D, full mip chain. Mip N is cone footprint at distance N.
    rhi::TextureHandle  voxelTex_ = 0;
    // Injection accumulator: R32_UINT Tex3D, (res*4) x res x res, channels interleaved along x.
    rhi::TextureHandle  voxelAccumTex_ = 0;
    rhi::PipelineHandle voxelPso_ = 0, voxelMsPso_ = 0, mipPso_ = 0, clearPso_ = 0, debugPso_ = 0;
    rhi::PipelineHandle resolvePso_ = 0;
    // Fog occlusion: kAirVisResolution^3, R16F. At t17(SRV)/u16(UAV). Created with volume, released on off edge.
    rhi::TextureHandle  airVisTex_ = 0;
    // 1x1x1 placeholder for airVisTex_ when fog occlusion off or CSAirVis didn't compile.
    rhi::TextureHandle  airVisPlaceholder_ = 0;
    // CSAirVis (optional, needs SM 6.0 + DXC).
    rhi::PipelineHandle airVisPso_ = 0;
    // True from airVisTex_ creation until CSAirVis has written it once (avoids garbage at t17).
    bool airVisDirty_ = false;
    u32 airVisSlab_ = 0;
    // Bumped whenever the radiance volume's contents or placement may have changed.
    u64 voxelGen_ = 0;
    // Generation the current slab cycle runs against, and the slabs it still has to rewrite.
    u64 airVisGen_ = 0;
    u32 airVisLeft_ = 0;
    // Volume origin/scale and resolution CSAirVis last ran with.
    f32 airVisInputs_[5] = {};
    // Rising edge marks volume dirty (geometry may have changed while unrefreshed).
    bool airVisWasActive_ = false;
    // t0 volume, t1 shadow, t2 TLAS, u0 volume mip 0, u1 accumulator.
    rhi::BindingSetHandle bindings_ = 0;
    // UAVs only (u0, u1): no SRV live while clear runs.
    rhi::BindingSetHandle clearBindings_ = 0;
    // Accumulator-to-volume reduction.
    rhi::BindingSetHandle resolveBindings_ = 0;
    // One per mip: set m reads mip m-1, writes mip m.
    std::vector<rhi::BindingSetHandle> mipBindings_;

    rhi::PipelineHandle scenePso_ = 0, sceneMsPso_ = 0, sceneRtPso_ = 0, sceneMsRtPso_ = 0;
    // Blended twins: PremultipliedAlpha, depth.write=false. PSMainVoxi returns full-strength specular + coverage-weighted diffuse.
    rhi::PipelineHandle sceneBlendedPso_ = 0, sceneMsBlendedPso_ = 0, sceneRtBlendedPso_ = 0,
                        sceneMsRtBlendedPso_ = 0;
    // Ray-driven primary visibility: fullscreen triangle. Null unless device has ray queries.
    rhi::PipelineHandle rayDrivenPso_ = 0;
    // Depth prepass + two prepassed scene-colour twins. No mesh-shader twins (prepass only for plain drawMesh()).
    rhi::PipelineHandle depthPrepassPso_ = 0;
    rhi::PipelineHandle scenePsoPrepassed_ = 0, sceneRtPsoPrepassed_ = 0;

    // G-buffer twins: mesh-shader x ray-tracing x blended x depth-prepassed.
    // Identical vertex/mesh stage, pixel shader recompiled with AVER_GBUFFER=1, renderTargetCount 4.
    // Optional (0 = couldn't build); built alongside plain twins, gated on msOk/rtOk; runtime on/off via pickGbuf().
    rhi::PipelineHandle sceneGbufPso_ = 0, sceneMsGbufPso_ = 0, sceneRtGbufPso_ = 0, sceneMsRtGbufPso_ = 0;
    rhi::PipelineHandle scenePsoPrepassedGbuf_ = 0, sceneRtPsoPrepassedGbuf_ = 0;
    // PSRayDriven twin (RayDrivenGBufferOut + SV_DEPTH).
    rhi::PipelineHandle rayDrivenGbufPso_ = 0;

    // Created updatable only while Settings::rtRefitAccel at init(); plain createTlas otherwise.
    // Sized for kMaxDraws + static foliage prefix. Draw instance IDs stay dense regardless of prefix size.
    rhi::TlasHandle tlas_ = 0;
    // Translucent instances in tlas_. Zero triggers first-hit fast path for primary sun shadow (rtShadowEx).
    u32 rtTlasTranslucent_ = 0;
    // Last announce state: 0 never, 1 on, 2 off.
    u8 rtShadowFirstHitLogged_ = 0;
    // One BLAS per mesh. Compute-written meshes (GpuMesh::computeWritten) refit/rebuild per frame; static meshes created once.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blas_;
    bool dynamicBlasLogged_ = false;
    // Meshes already refreshed this frame (avoids double refit/rebuild). Kept for reuse across frames.
    std::vector<rhi::MeshHandle> rebuiltThisFrame_;
    // Last pass rebuild count. Logged only on change.
    u32 lastBlasRebuilds_ = 0xFFFFFFFFu;

    // Dynamic BLAS refit path (Settings::rtRefitAccel).
    // Distinct compute-written meshes from LAST FULL BUILD.
    std::vector<rhi::MeshHandle> rtDynamicMeshes_;
    // Refits since last rebuild. Periodic rebuild every kDynamicBlasRefitsPerRebuild.
    std::unordered_map<rhi::MeshHandle, u32> dynamicBlasRefits_;
    static constexpr u32 kDynamicBlasRefitsPerRebuild = 30;
    void refitOrRebuildDynamicBlas(rhi::IRenderContext& ctx, rhi::BlasHandle blas, rhi::MeshHandle mesh);
    bool refitOrRebuildTlas(rhi::IRenderContext& ctx);
    u32 tlasRefitStreak_ = 0;
    static constexpr u32 kTlasRefitsPerRebuild = 30;
    u64 rtDynamicBlasRefits_ = 0, rtDynamicBlasRebuilds_ = 0;
    u64 rtTlasRefits_ = 0, rtTlasRebuilds_ = 0;
    void refitDynamicAccelStructures(rhi::IRenderContext& ctx);

    // THE UNCHANGED GATE (Settings::rtSkipUnchangedTlas): skip rebuild if bit-identical.
    // Hash what per-draw loop reads from drawsPrev_. Movable draws' world omitted, patched separately.
    bool rtAccelSnapshotUnchanged() const;
    bool rtAccelMustForceRebuild() const;
    u64  rtAccelDrawsKey(bool reuseListKey = false) const;
    void takeRtAccelSnapshot();
    void reportRtAccelGate();
    void updateRtParamsPerFrame();

    u64  rtAccelKey_ = 0;
    // Draw-list half as last computed; reused until next build.
    mutable u64  rtAccelListKey_ = 0;
    mutable bool rtAccelListKeyValid_ = false;
    // Mesh filter for rtAccelMustForceRebuild(): direct-mapped, a slot is live only for the call that stamped it.
    // 16384 slots hold ~3000 meshes.
    static constexpr u32 kRtAccelMeshCheckSlots = 16384;
    static_assert((kRtAccelMeshCheckSlots & (kRtAccelMeshCheckSlots - 1)) == 0,
                  "kRtAccelMeshCheckSlots must be a power of two for the '& (kRtAccelMeshCheckSlots - 1)' mask");
    struct MeshCheckSlot { rhi::MeshHandle mesh = 0; u32 stamp = 0; };
    mutable std::vector<MeshCheckSlot> rtAccelMeshChecked_;
    mutable u32 rtAccelMeshStamp_ = 0;
    bool rtAccelSnapValid_ = false;
    u64  rtAccelSkipped_ = 0, rtAccelRebuilt_ = 0, rtAccelRefitOnly_ = 0, rtAccelMoverPatched_ = 0;
    mutable u32 rtAccelGateWhyMask_ = 0;
    u64  rtAccelGateNextReport_ = 64;
    u64  rtAccelGateLastTicks_ = 0, rtAccelGateLastSkipped_ = 0, rtAccelGateLastRefitOnly_ = 0,
         rtAccelGateLastMoverPatched_ = 0;

    // THE MOVER PATCH LANE: moving draw no longer forces full per-draw loop.
    // Movable draws collected during rtAccelDrawsKey() pass, sorted by patchRtMovers().
    // Patch writes current transforms; caller refits TLAS (same periodic schedule as ever).
    // World feeds TlasInstance::world, RtInstance::objectToWorld, prevObjectToWorld (from old as overwritten).
    bool rtMoverPatchActive() const;
    static constexpr u32 kRtNoInstance = 0xFFFFFFFFu;
    struct RtMover {
        u64 id = 0;                  // rtDrawHash(): everything except world
        u32 draw = 0;                // index into drawsPrev_
        u32 inst = kRtNoInstance;    // index into tlasInstScratch_/rtInstanceData_
        bool operator<(const RtMover& o) const { return id != o.id ? id < o.id : draw < o.draw; }
    };
    std::vector<RtMover> rtMovers_;
    mutable std::vector<RtMover> rtMoversNow_;
    enum class MoverPatch : u8 {
        Refused,     // movers no longer line up: run full build
        Unchanged,   // every mover where TLAS has it: nothing to write
        Patched,     // at least one transform rewritten, table re-uploaded
        MaterialsOnly, // only a mover's emissive changed: material + instance tables re-uploaded, no TLAS work
    };

    // Per-build scratch: hoisted to avoid frame reallocation. .clear()'d at buildAccelerationStructures top.
    std::vector<rhi::TlasInstance> tlasInstScratch_;
    std::unordered_map<u64, pbr::MaterialConstants> matConstantsScratch_;

    f64 lastAccelBuildCpuMs_ = 0.0;
    // Occlusion rays per pixel. 4 default (1 = hard aliased edge, linear cost).
    u32 rtShadowRays_ = 4;
    u32 rtShadowRaysUsed_ = 2;
    // Tile edge for temporal amortisation. 1 traces every pixel every frame.
    u32 rtPixelsPerRayTile_ = 1;
    // SPATIAL filter radius, mirrored from Settings by applySettings.
    u32 rtShadowDenoise_ = 0;
    // Spatial filter taper with velocity. 0 = no taper. Measurement knob.
    f32 rtDenoiseMotionTaper_ = 0.0f;
    // Settings::rtRenderMode cached. 1 asks for ray-driven primary visibility.
    u32 rtRenderMode_ = 0;
    // Settings::giMode cached. Whether honoured is giRestirWanted().
    u32 giMode_ = 0;
    // Whether coat lobe compiled. Latched at first setSettings (rebuilds ~20 PSOs). Project-level decision.
    bool layeredBsdf_ = false;
    bool layeredBsdfLatched_ = false;
    // Once per process, not per frame. Logging it every applySettings would be noise.
    bool layeredBsdfWarned_ = false;
    // Settings::ptBounces. Spent only while pathTracingWanted().
    u32 ptBounces_ = 1;
    // Frames apart GI volume rebuilds. 1 rebuilds every frame.
    u32 giUpdateInterval_ = 1;
    // Frame count (simulated, not wall-clock). Keeps trace schedule, disc-sample rotation deterministic.
    u32 rtFrameIndex_ = 0;

    // Frame-period sampler: wall-clock period between prePass calls.
    // CPU period (tracks GPU cost while GPU-bound). Run with --no-vsync or every reading is refresh interval.
    rhi::TextureHandle boundBackdrop_ = 0;
    u32  rdAblate_ = 0;
    bool frameTimeReport_ = false;
    u64  frameTimeLastNs_ = 0;
    u32  frameTimeSeen_ = 0;
    std::vector<f32> frameTimeMs_;
    static constexpr u32 kFrameTimeWarmup = 30;
    void reportFrameTime(const char* when);

    // VRAM usage: logged when category changes, not every frame.
    void reportVramUsage();
    // Scene-mesh BLAS total: resampled when blasRevision_ changes or every kVramBlasResampleFrames.
    u32 blasRevision_ = 0;
    u32 vramBlasSampledRevision_ = 0xFFFFFFFFu;
    u32 vramBlasSampledFrame_ = 0;
    u64 vramBlasSceneBytes_ = 0;
    static constexpr u32 kVramBlasResampleFrames = 64;
    u64 vramReportedRadianceBytes_ = 0;
    u64 vramReportedAccumBytes_ = 0;
    u64 vramReportedGiCacheBytes_ = 0;
    u64 vramReportedRdBytes_ = 0;
    u64 vramReportedBlasBytes_ = 0;
    u64 vramReportedTlasBytes_ = 0;
    u64 vramReportedFoliagePrefixBytes_ = 0;

    // Flat geometry table: reflection ray reads after hit.
    struct RtInstance {
        f32 objectToWorld[16];   // engine row-vector, matching TlasInstance::world
        u32 firstIndex = 0;      // mesh's indices start in flat table
        u32 firstVertex = 0;     // mesh's vertices start
        f32 albedo[3] = {1, 1, 1};
        // Material a ray can reach. Used to sit unread in Draw; metals now appear dark (no diffuse) instead of white.
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        // Dense index into this frame's rtMaterials_ (t9). Index 0 = fallback. Reflectance/f90/flags/emissive fetched.
        u32 materialIndex = 0;
        // objectToWorld AS DRAWN PREVIOUS FRAME (engine row-vector). Shader pushes object-space hit to find last-frame position.
        // Filled by carry-forward (full build) or patchRtMovers (mover patch). Foliage carries own (static).
        f32 prevObjectToWorld[16];
    };
    // 160 bytes total. Stride to setSrvBuffer must agree. Three places must agree; assert guards two.
    static_assert(sizeof(RtInstance) == 160, "RtInstance is the HLSL RtInstance ABI");

    rhi::BufferHandle rtVerts_ = 0, rtIndices_ = 0;
    u32  rtVertCapacity_ = 0, rtIndexCapacity_ = 0, rtInstanceCapacity_ = 0;

    // Instance table: RING on upload heap. Rewritten every frame while GPU reads previous copy.
    // One buffer per frame in flight; 3 matches PcgVolume's readback window.
    static constexpr u32 kRtInstanceRing = 8;   // up to two uploads a frame (prePass settle + late movers) x frames in flight

    // Distinct textures ray can sample. Fixed, not grown (baked into root signature). 4096 limit: 6% of 65536-descriptor heap.
    static constexpr u32 kRtTextureCapacity = 4096;
    rhi::BufferHandle rtInstances_[kRtInstanceRing] = {};
    u32               rtInstanceSlot_ = 0;
    // Table built from this. Only meshes new to the set are copied.
    u64  rtGeometryKey_ = 0;
    bool rtGeometryReady_ = false;
    std::vector<RtInstance> rtInstanceData_;
    std::vector<rhi::MeshHandle> rtInstanceMesh_;
    // Distinct meshes those instances name, sorted, with position in shared table. One per mesh, not per instance.
    std::vector<rhi::MeshHandle> rtGeomMeshes_;
    std::vector<u32> rtGeomFirstVertex_, rtGeomFirstIndex_;
    // Each mesh's range in rtVerts_/rtIndices_, kept while it stays in the set; one vertex slice per
    // vertex buffer (LOD/posed parts share their root's). The used counts are the append points.
    struct RtGeomSlot {
        rhi::BufferHandle vb = 0, ib = 0; u32 vc = 0, ic = 0, firstVertex = 0, firstIndex = 0;
        bool indexDone = false, vertsDone = false;   // copied in; until both, its instances are masked out
    };
    // Uploads into rtVerts_/rtIndices_ still to finish, copied in pieces within kRtGeomCopyBytesPerFrame:
    // a terrain tile's ~200 MB read from the upload heap was one ~260 ms frame.
    struct RtGeomCopy {
        bool verts; rhi::MeshHandle mesh; u64 sliceKey; rhi::BufferHandle src;
        u64 srcOffset, dst, bytes, done;
    };
    std::vector<RtGeomCopy> rtGeomPending_;
    static constexpr u64 kRtGeomCopyBytesPerFrame = 16ull << 20;
    std::unordered_map<rhi::MeshHandle, RtGeomSlot> rtGeomSlots_;
    u32 rtVertUsed_ = 0, rtIndexUsed_ = 0;

    // ---- per-frame vertex refresh for compute-skinned slices ----
    struct DynamicVertexSlice {
        rhi::BufferHandle vb = 0;
        u32 vertexCount = 0;
        u32 firstVertex = 0;   // offset into rtVerts_
    };
    std::vector<DynamicVertexSlice> rtDynamicVertexSlices_;
    std::vector<DynamicVertexSlice> rtDynamicVertexSlicesPending_;
    void refreshDynamicVertexSlices(rhi::IRenderContext& ctx);

    // Returns false when geometry table could not be made (fall back to cone-traced reflections).
    bool buildGeometryTable(rhi::IRenderContext& ctx);
    // Writes rtInstanceData_ into the next rtInstances_ ring slot; true when nothing to send or slot is bound.
    bool uploadRtInstanceTable();
    bool rtLogged_ = false;

    // ---- dense per-frame material table a ray hit indexes into (t9, gRtMaterials) ----
    u32  residentTexture(rhi::TextureHandle h);
    void ensureTextureTable();

    // TEXTURED ray-driven pipeline and variants.
    rhi::PipelineHandle rayDrivenTexPso_ = 0;
    rhi::PipelineHandle rayDrivenTexGbufPso_ = 0;
    rhi::PipelineHandle sceneRtBlendedTexPso_ = 0;

    // ---- STAGED RAY-DRIVEN PASSES ----
    rhi::PipelineHandle rdVisCsPso_    = 0;
    rhi::PipelineHandle rdShadowCsPso_ = 0;
    rhi::PipelineHandle rdGiCsPso_     = 0;
    rhi::PipelineHandle rdGiCbCsPso_   = 0;
    rhi::PipelineHandle rdSkyOccCsPso_ = 0;
    rhi::PipelineHandle rdReflCsPso_   = 0;
    // ---- SUB-STAGE SPLITS ----
    rhi::PipelineHandle rdShadowProbeCsPso_ = 0;
    rhi::PipelineHandle rdShadowTiledCsPso_ = 0;
    rhi::PipelineHandle rdTailVisCsPso_ = 0;      // CSRdTailVis: the tail lights' shadow fraction
    rhi::PipelineHandle rdTailFilterCsPso_ = 0;   // CSRdTailFilter: its 5x5, into gRdLocalOut.z
    rhi::PipelineHandle rdGiTraceCsPso_   = 0;
    rhi::PipelineHandle rdGiTraceCbCsPso_ = 0;
    rhi::PipelineHandle rdGiSplitCsPso_   = 0;
    rhi::PipelineHandle rdGiSplitCbCsPso_ = 0;
    // Radiance cache twins (AVER_NEURAC=1, built lazily).
    rhi::PipelineHandle rdGiCacheCsPso_        = 0;
    rhi::PipelineHandle rdGiCacheCbCsPso_      = 0;
    rhi::PipelineHandle rdGiTraceCacheCsPso_   = 0;
    rhi::PipelineHandle rdGiTraceCacheCbCsPso_ = 0;
    // Path Tracing twins (AVER_PT_PATHS=1, built lazily): multi-bounce ReSTIR candidates and reflections.
    rhi::PipelineHandle rdGiPtCsPso_        = 0;
    rhi::PipelineHandle rdGiPtCbCsPso_      = 0;
    rhi::PipelineHandle rdGiTracePtCsPso_   = 0;
    rhi::PipelineHandle rdGiTracePtCbCsPso_ = 0;
    // Path Tracing over the radiance cache (AVER_PT_PATHS + AVER_NEURAC): paths train the cache and end in it.
    rhi::PipelineHandle rdGiPtRcCsPso_      = 0;
    rhi::PipelineHandle rdGiTracePtRcCsPso_ = 0;
    rhi::PipelineHandle rdGiPtRcCbCsPso_      = 0;   // the same, half-rate checkerboard
    rhi::PipelineHandle rdGiTracePtRcCbCsPso_ = 0;
    rhi::PipelineHandle rdPtRefCsPso_       = 0;   // CSRdPtRef: Reference mode's per-pixel path
    rhi::PipelineHandle rdReflPtRcCsPso_      = 0; // CSRdRefl paths over the radiance cache
    rhi::PipelineHandle rdReflSplitPtRcCsPso_ = 0;
    rhi::PipelineHandle rdReflPtCsPso_      = 0;
    rhi::PipelineHandle rdReflSplitPtCsPso_ = 0;
    rhi::PipelineHandle rdReflSplitCsPso_  = 0;
    rhi::PipelineHandle rdReflFilterCsPso_ = 0;
    // Stage B compose: rayDrivenTexPso_ variants with AVER_RD_SPLIT=1.
    rhi::PipelineHandle rayDrivenSplitTexPso_     = 0;
    rhi::PipelineHandle rayDrivenSplitTexGbufPso_ = 0;

    rhi::BindlessTableHandle rtTexTable_ = 0;
    std::unordered_map<rhi::TextureHandle, u32> rtTexIndex_;
    u32  rtTexNext_ = 0;
    bool rtTexTableTried_ = false;
    bool rtTexLogged_ = false;

    rhi::BufferHandle rtMaterials_[kRtInstanceRing] = {};
    u32  rtMaterialSlot_ = 0;
    u32  rtMaterialCapacity_ = 0;
    bool rtMaterialsReady_ = false;
    bool rtMaterialAllocFailLogged_ = false;
    // Final sorted material table and GPU snapshot; compared byte-for-byte to detect changes.
    std::vector<pbr::MaterialConstants> rtMaterialData_;
    std::vector<pbr::MaterialConstants> rtMaterialUploaded_;
    // Per rtInstanceData_ element: material key, resolved to final dense index once all draws are seen.
    std::vector<u64> rtInstanceMatKey_;

    // Sorts distinct material keys, builds rtMaterialData_ in that order (index 0 is fallback),
    // re-uploads only when content differs, writes final index into rtInstanceData_[i].materialIndex.
    bool buildMaterialTable(const std::unordered_map<u64, pbr::MaterialConstants>& matConstantsByKey);
    // One surface's key and -- first time this build sees it -- its constants into matConstantsScratch_.
    u64 rtMaterialKey(rhi::BindingSetHandle matSet, const void* authoredBytes, const f32 color[4],
                      f32 metallic, f32 roughness);

    // ---- INSTANCED FOLIAGE (setFoliage): state behind the TLAS static prefix ----
    // Shader reads parts from t20 (gRtFoliageParts) and transforms from t21 (gRtFoliageDescs).
    struct FoliagePartState {
        rhi::MeshHandle mesh = 0;
        u32 material = 0;
        rhi::BindingSetHandle matSet = 0;
        f32 color[4] = {1, 1, 1, 1};
        f32 metallic = 0.0f, roughness = 1.0f;
    };
    std::vector<FoliagePartState> foliageParts_;
    std::vector<rhi::BlasHandle> foliageBlas_;
    std::vector<rhi::MeshHandle> foliageMeshes_;
    u32 foliageInstances_ = 0;
    u64 foliageBlasBytes_ = 0, foliagePrefixBytes_ = 0;
    u64 foliageGeneration_ = 0;
    // BLASes allocated by setFoliage, built by next buildAccelerationStructures before tlas_ rebuild.
    bool foliageBlasPending_ = false;
    // t2/t20/t21 must be rewritten before rays read them; set early in prePass before bindings_ bind.
    bool foliageBindingsDirty_ = false;
    // Part table as resolved (geometry/material indices, keyed), GPU snapshot, and per-part material key.
    std::vector<RtInstance> foliagePartData_;
    std::vector<RtInstance> foliagePartUploaded_;
    std::vector<u64> foliagePartMatKey_;
    rhi::BufferHandle foliagePartBuf_[kRtInstanceRing] = {};
    u32 foliagePartSlot_ = 0;
    u32 foliagePartCapacity_ = 0;
    rhi::BufferHandle foliagePartPlaceholder_ = 0, foliageDescPlaceholder_ = 0;
    void resolveFoliageMaterials();
    void uploadFoliagePartTable();
    void refreshFoliageBindings();
    u64 foliageKey() const;

    // ---- LOCAL LIGHTS (LAMPS): per-frame light list at t18 (gRdLocalLights) ----
    // One entry per authored draw with MaterialFlag_Light (lightIntensity > 0): world sphere colored by emission,
    // plus one per scene light. HLSL mirror: AverLightRec (render.pt/shaders/aver_lights.hlsli), 80-byte stride.
    using RdLocalLight = PackedLight;
    static_assert(sizeof(RdLocalLight) == 80, "RdLocalLight is the HLSL AverLightRec ABI");
    // At most this many per frame, sorted by 1-metre irradiance over max(distance², 1).
    static constexpr u32 kMaxLocalLights = 32;      // the raster path's working set (shaders' AVER_LIGHT_LIST_MAX)
    static constexpr u32 kMaxListLights = 4000;     // every emitter, up to this (UNIFIED_LIGHTS.md phase 3)
    static constexpr u32 kFlatLightList = 48;       // up to this many lights, every point walks the whole list
    static constexpr u32 kMaxLightsPerCell = 24;    // a light-grid cell keeps its most important lights
    static constexpr u32 kLightGridMaxDim = 32;     // cells per axis
    static constexpr f32 kLightGridHalfExtent = 20000.0f;   // cm around the camera
    // Upload-heap ring: writeBuffer is unsynchronised, so rotate before write to avoid reading old frame's copy.
    rhi::BufferHandle rdLocalLights_[kRtInstanceRing] = {};
    u32 rdLocalLightSlot_ = 0;
    u32 rdLocalLightCapacity_ = 0;
    // Stand-in bound when list is empty; every slot must have a valid descriptor (Tier 1).
    rhi::BufferHandle rdLocalLightsPlaceholder_ = 0;
    rhi::BufferHandle rdLocalLightsBound_ = 0;
    std::vector<RdLocalLight> rdLocalLightData_;
    u32 rdLocalLightCount_ = 0;   // the raster path's working set: the first lamps, at most kMaxLocalLights
    u32 rdLocalLampCount_ = 0;    // every lamp and scene light in the list (the sun and the grid follow them)
    u64 rdLocalLightHash_ = 0;
    std::vector<RdLocalLight> rdGridCache_;   // header + cell table + pool records of the last grid build
    u64 rdGridCacheKey_ = 0;
    bool rdGridCacheValid_ = false;
    u64 rdGridKeyNow_ = 0;                       // this frame's grid key (0 = no grid)
    u64 rdGridSlotKey_[kRtInstanceRing] = {};     // grid key each upload ring slot already holds
    // The light grid built once in canonical (byte) light order. A frame whose list only reorders the same lights (the
    // list is importance-ordered from the camera) deals the cached cells out through the new list indices (fill())
    // instead of rebuilding them: same table, same pool, same order within every cell.
    struct LightGridCanon {
        bool valid = false;
        f32 lo[3] = {}, cell = 0.0f;
        u32 dim[3] = {};
        std::vector<RdLocalLight> lights;   // non-directional lights, sorted by bytes
        std::vector<u32> slots;             // open addressing over identical-light runs: first canonical id + 1, 0 = empty
        std::vector<u32> runLen;            // per canonical id: length of the identical-light run it starts
        std::vector<u32> box;               // 6 per canonical id: cell range, c0 > c1 = not in the grid
        std::vector<u32> cellN;             // lights covering each cell
        std::vector<u32> cursorInit;        // pool offset per cell, kLarge for cells that keep a cut list
        std::vector<u32> largeCells;        // cells with more than kMaxLightsPerCell lights
        std::vector<u32> bigOff, bigCnt;    // per cell: stored cut list (bigOff into big)
        std::vector<std::pair<f32, u32>> big;   // (importance, canonical id): best first, plus any tied with the cut
        std::vector<f32> table, pool;       // pool is rewritten by fill()
        std::vector<u32> poolOff, canonOf, listOf, used, cursor;
        std::vector<std::pair<f32, u32>> cut;
        static constexpr u32 kLarge = 0xFFFFFFFFu;
        bool build(const RdLocalLight* lights, u32 lightCount, const f32 lo[3], f32 cell, const u32 dim[3]);
        bool fill(const RdLocalLight* lights, u32 lightCount, const f32 lo[3], f32 cell, const u32 dim[3]);
    };
    LightGridCanon rdGridCanon_;
    u64 rdGridLastSet_ = 0;                      // order-independent hash of the last frame's grid lights
    // True when every light-flagged draw made the list (none cut by 32-light cap); allows GI to omit lamp emission.
    bool rdLocalLightsCarryAll_ = false;
    struct RdLocalLightCand {
        f32 importance;
        RdLocalLight light;
        bool fromDraw;   // an emissive-material lamp rather than a scene light
    };
    std::vector<SceneLight> sceneLights_;
    // Light assets: CPU copy from the register calls, GPU texture made on first use and made resident
    // in the ray path's bindless table.
    struct LightAssetCpu { std::vector<u8> bytes; u32 width = 0, height = 0; bool ies = false; f32 peakOverMean = 1.0f; };
    struct LightAssetGpu { rhi::TextureHandle tex = 0; u32 index = 0xFFFFFFFFu; bool failed = false; };
    std::unordered_map<u64, LightAssetCpu> lightAssetsCpu_;
    std::unordered_map<u64, LightAssetGpu> lightAssetsGpu_;
    // Bindless index of a registered asset (creating its texture on first use), or kUnboundTexture.
    u32 lightAssetIndex(u64 id);
    void releaseLightAssets();
    void appendSceneLightCandidates(const f32 eye[3]);

    // ---- DECALS: per-frame record list at t24 (gDecals), count in cb_.decalParams[0] ----
    static_assert(sizeof(PackedDecal) == 192, "PackedDecal is the HLSL AverDecalRec ABI");
    // Upload-heap ring, as the local-light list: rotate before writing, never touch the bound copy.
    rhi::BufferHandle decalBuf_[kRtInstanceRing] = {};
    u32 decalBufSlot_ = 0;
    u32 decalBufCapacity_ = 0;
    rhi::BufferHandle decalPlaceholder_ = 0;   // bound while the list is empty: every slot needs a descriptor
    rhi::BufferHandle decalBound_ = 0;
    u32 decalCount_ = 0;
    std::vector<SceneDecal> sceneDecals_;
    struct DecalCand { f32 importance; f32 dist; i32 order; PackedDecal packed; };
    std::vector<DecalCand> decalCand_;
    std::vector<PackedDecal> decalData_;
    struct DecalTexCpu { std::vector<std::vector<u8>> levels; u32 width = 0, height = 0; DecalImageKind kind = DecalImageKind::Colour; };
    struct DecalTexGpu { rhi::TextureHandle tex = 0; u32 index = 0xFFFFFFFFu; bool failed = false; };
    std::unordered_map<u64, DecalTexCpu> decalTexCpu_;
    std::unordered_map<u64, DecalTexGpu> decalTexGpu_;
    bool decalFailLogged_ = false;
    bool decalLoggedRun_ = false;
    // Bindless index of a registered decal image (uploading it on first use), or kUnboundTexture.
    u32 decalTextureIndex(u64 id);
    void releaseDecalTextures();
    void buildDecals();
    std::vector<RdLocalLightCand> rdLocalLightCand_;
    bool rdLocalLightsFailLogged_ = false;
    void buildLocalLights();
    bool localLightsReady() const {
        return rdLocalLampCount_ > 0 && settings_.localLights && rdLocalOutThisFrame_ != 0;
    }
    // Writes cb_.cameraMedium[2]/[3] for the scene pass; returns live so caller marks history written.
    bool publishLocalLights(bool live, const char* pass);

    // ---- previous-frame per-instance transforms: RtInstance::prevObjectToWorld ----
    // Two writers: patchRtMovers() and full-build carry-forward (last frame's transforms by group match).
    // GROUP KEY: (mesh, drawBinding); RT instanceId is a POSITION, not stable identity.
    u64 rtInstanceGroupKey(rhi::MeshHandle mesh, rhi::BindingSetHandle matSet) const;
    // Parallel to rtInstanceData_: group key of the draw that produced it. Foliage not included.
    std::vector<u64> rtInstanceGroupKey_;
    // Rows whose prevObjectToWorld != objectToWorld; settled at next build's start (prev = current, re-upload).
    std::vector<u32> rtPrevPending_;

    // Carry-forward lookup over last frame's rows: (group key, 16 floats' raw bits) for exact match.
    static constexpr u32 kRtCarryNone = 0xFFFFFFFFu;
    // Nearest match farther than this is a different object; 20 m is generous for one-frame moves.
    static constexpr f32 kRtMaxCarryCm = 2000.0f;
    // Linear scan; only runs when group has at most this many unmatched rows.
    static constexpr u32 kRtCarryMaxScan = 64;
    struct RtCarryEntry {
        f32 world[16];
        u32 nextInGroup = kRtCarryNone;
        u32 nextExact = kRtCarryNone;
        u32 group = 0;
        bool used = false;
    };
    struct RtCarryGroup {
        u64 key = 0;
        u32 head = kRtCarryNone;
        u32 unused = 0;
        u32 nextInBucket = kRtCarryNone;
    };
    std::vector<RtCarryEntry> rtCarryEntries_;
    std::vector<RtCarryGroup> rtCarryGroups_;
    std::vector<u32> rtCarryGroupBuckets_;
    std::vector<u32> rtCarryExactBuckets_;
    void buildRtCarryLookup();
    // Exact match or nearest unmatched in range, else world itself.
    void carryPrevTransform(u64 groupKey, const f32* world, f32* outPrev);

    // One replayed draw: transform, colour, material.
    struct Draw {
        rhi::MeshHandle mesh;
        // DEPTH-ONLY passes' proxy (cascades, GI shadow, voxelisation); falls back to mesh.
        rhi::MeshHandle depthMesh = 0;
        f32 world[16];
        f32 color[4];
        f32 metallic, roughness;
        rhi::BindingSetHandle matSet = 0;
        u32 matBytes = 0;
        u8  mat[sizeof(pbr::MaterialConstants)] = {};
        // World-space bounding sphere from IDevice::meshBounds; negative radius means unknown.
        f32 boundsCentre[3] = {0, 0, 0};
        f32 boundsRadius = -1.0f;

        // TRANSLUCENT: in TLAS only; non-opaque shadow ray, out of shadow cascade/GI/voxelisation.
        bool translucent = false;

        // HIDDEN FROM OWNER: in everything except primary-visibility ray (ray begins inside mesh).
        bool hiddenFromOwner = false;

        // MOVABLE: moves during play (game::PlayMobility decides); out of voxelisation and GI rebuild gate.
        bool movable = false;
    };
    bool submitMovable_ = false;

    // Material system answers (is authored, textures); remembered for ONE key computation. Not a member.
    struct DrawMaterialMemoSlot {
        rhi::BindingSetHandle set = 0;
        u32 state = 0;
        u64 texHash = 0;
    };
    static constexpr u32 kDrawMaterialMemoSlots = 1024;
    static_assert((kDrawMaterialMemoSlots & (kDrawMaterialMemoSlots - 1)) == 0,
                  "kDrawMaterialMemoSlots must be a power of two for the '& (kDrawMaterialMemoSlots - 1)' mask");
    using DrawMaterialMemo = std::array<DrawMaterialMemoSlot, kDrawMaterialMemoSlots>;

    // Lane states of a drawsPrev_ entry's world and material byte hashes: pure functions of the draw, so
    // rtAccelDrawsKey and the GI keys share them for one list. Valid for the entry's token only.
    struct DrawLanes {
        static constexpr u8 kWorld = 1, kMat = 2;
        u64 world[4];
        u64 mat[4];
        u64 token = 0;
        u8  have = 0;
    };
    mutable std::vector<DrawLanes> drawLanes_;
    u64 drawLanesToken_ = 1;
    DrawLanes* drawLanes(usize i) const;
    void hashWorldInto(u64& h, const Draw& d, DrawLanes* lanes) const;

    // One draw's material identity folded into running hash h; `lanes` (optional) caches its byte hash.
    // skipEmissive: hash the constants with emissiveFactor zeroed (mover lane only; never cached).
    void hashDrawMaterialInto(u64& h, const Draw& d, DrawMaterialMemo& memo, bool skipEmissive = false,
                              DrawLanes* lanes = nullptr) const;

    // One draw's rtAccelDrawsKey() term; with moverLane a movable draw's term omits world matrix.
    u64 rtDrawHash(const Draw& d, DrawMaterialMemo& memo, bool moverLane, DrawLanes* lanes = nullptr) const;

    std::vector<Draw> draws_, drawsPrev_;
    // Pairs movers against list (drawsPrev_ in prePass; this frame's draws_ in the late scene pass)
    // and writes their transforms. late skips the key-pass check (the late pass builds its own movers).
    // deferGpu: a Patched result leaves the instance-table upload to the caller (see rtRefitDeferred_).
    MoverPatch patchRtMovers(const std::vector<Draw>& list, bool late, bool deferGpu);
    // The instance-table upload and TLAS/dynamic-BLAS refit a prePass mover patch left to the late pass
    // (wantsLateScenePass); held until latePatchMovers or a prePass that cannot defer runs them.
    bool rtRefitDeferred_ = false;
    // rtFrameIndex_ of the frame whose prePass already refitted the dynamic BLASes: their vertices are final by then,
    // so the late pass refits only the TLAS.
    u32 rtDynBlasRefitFrame_ = 0;
    // Late scene pass: brings movers to THIS frame's transforms before anything traces (D3D12).
    void latePatchMovers(rhi::IRenderContext& ctx);
    // Two facts about the list recorded as each draw appends and swapped by beginScene().
    std::vector<u32> translucentDraws_, translucentDrawsPrev_;
    // Indices into draws_ / drawsPrev_ of light-flagged draws, in list order.
    std::vector<u32> lampDraws_, lampDrawsPrev_;

    // ---- the blended-draw census ----
    // submitDraw() drops every blended draw before it reaches draws_ (silent drop would be indistinguishable from a bug).
    u64 blendedDropped_ = 0;
    u32 blendedDropLogs_ = 0;

public:
    // LOD proxy for depth-only passes: function pointer, not a dependency (LOD ladder belongs to mesh loader).
    using DepthProxyFn = rhi::MeshHandle (*)(rhi::MeshHandle mesh, void* user);
    // Defined in .cpp: submit() caches depthProxyFn_'s answer per mesh; reassign without dropping cache = silent wrong results.
    void setDepthProxy(DepthProxyFn fn, void* user);

    // A/B toggle for meshSubmitCache_ memoisation; false sends submit() back to direct lookups.
    void setMeshSubmitCacheEnabled(bool on) { meshSubmitCacheEnabled_ = on; }
    bool meshSubmitCacheEnabled() const { return meshSubmitCacheEnabled_; }

private:
    DepthProxyFn depthProxyFn_ = nullptr;
    void*        depthProxyUser_ = nullptr;

    // ---- submit()'s per-mesh memoisation ----
    // 2048 fixed direct-mapped slots; reuses 80730751's shape. Cleared whole (not slot-by-slot) via meshSubmitCacheGen_.
    static constexpr u32 kMeshSubmitCacheSlots = 2048;
    static_assert((kMeshSubmitCacheSlots & (kMeshSubmitCacheSlots - 1)) == 0,
                  "kMeshSubmitCacheSlots must be a power of two for '& (kMeshSubmitCacheSlots - 1)' "
                  "below to be equivalent to '% kMeshSubmitCacheSlots'");

    // One slot's answer to depthProxyFn_ and meshBounds (local-space, not transformed).
    struct MeshSubmitCacheSlot {
        rhi::MeshHandle mesh = 0;
        u32 generation = 0;

        bool depthProxyResolved = false;
        rhi::MeshHandle depthProxyMesh = 0;

        bool boundsResolved = false;
        bool haveBounds = false;
        f32  localCentre[3] = {0.0f, 0.0f, 0.0f};
        f32  localRadius = 0.0f;
    };
    std::array<MeshSubmitCacheSlot, kMeshSubmitCacheSlots> meshSubmitCache_{};
    u32 meshSubmitCacheGen_ = 1;
    // Emptied by bumping meshSubmitCacheGen_ (every slot from earlier generation reads as empty).
    void dropMeshSubmitCache() {
        if (++meshSubmitCacheGen_ != 0) return;
        meshSubmitCache_ = {};
        meshSubmitCacheGen_ = 1;
    }

    // Finds mesh's slot, evicting a previous occupant first (same hazard/fix as GameRender.cpp).
    MeshSubmitCacheSlot& meshSubmitCacheSlot(rhi::MeshHandle mesh);

    bool meshSubmitCacheEnabled_ = true;

    // shadowPass() scratch: culled draws grouped by mesh for drawMeshInstanced().
    struct ShadowInstanceGroup { rhi::MeshHandle mesh = 0; std::vector<f32> worlds; };
    std::vector<ShadowInstanceGroup> shadowInstanceGroups_;
    std::vector<ShadowInstanceGroup> giShadowInstanceGroups_;
    // mesh -> group index (groups appended, never erased).
    std::unordered_map<rhi::MeshHandle, u32> shadowGroupIndex_;
    std::unordered_map<rhi::MeshHandle, u32> giShadowGroupIndex_;
    static ShadowInstanceGroup& shadowGroupFor(std::vector<ShadowInstanceGroup>& groups,
                                               std::unordered_map<rhi::MeshHandle, u32>& index,
                                               rhi::MeshHandle mesh);

    f32 center_[3] = {0, 0, 0};
    f32 extent_ = 2000.0f;

    // Mirrors `cbuffer VoxiFrame : register(b4)` field for field.
    struct FrameConstants {
        f32 voxelOrigin[4] = {};
        f32 voxelParams[4] = {};
        f32 cascadeViewProj[4][16] = {};
        f32 cascadeSplit[4][4] = {};
        f32 shadowParams[4] = {};
        f32 shadowDraw[4] = {};
        f32 rtParams[4] = {};
        f32 rtHistParams[4] = {};
        f32 prevViewProj[16] = {};
        f32 sceneViewport[4] = {};
        f32 sceneViewportCur[4] = {};
        // THE MEDIUM THE CAMERA IS INSIDE: x = in blended volume, y = ior. Needed: closed volumes seen from within have no front faces.
        // z, w: LOCAL LIGHTS count and bits (valid history, carries emitters).
        f32 cameraMedium[4] = {};
        // WATER VOLUME FOR CAUSTICS: min.xyz/max.xyz AABB; min.w = exists, max.w = strength.
        f32 causticMin[4] = {};
        f32 causticMax[4] = {};
        // GI-only shadow map's view-projection, fitted to GI volume (read by PSVoxel via giShadowFactor).
        f32 giShadowViewProj[16] = {};
        // x = 1/kGiShadowSize, y = usable, z = normal-offset bias, w = bit-field toggles (rtSecondaryShadowOpaque, etc.).
        f32 giShadowParams[4] = {};
        // Spatial shadow denoiser: x = filter radius, y = blend amount, z = taper rate, w = AMBIENT history bound.
        f32 rtDenoiseParams[4] = {};
        // Ray-driven bounce control: x = bounces after first hit (1 = reflections).
        f32 ptBounceParams[4] = {};
        // GI gather control: x = diffuse gather cones; y/z/w = REFRACTION parameters.
        f32 giParams[4] = {};
        // Ambient control: x = sky-occlusion rays, y = coherence tile, z/w = bitmasks (decoded as u32).
        f32 ambientParams[4] = {};
        // EDITOR VIEW MODES: x = mode (0 normal, 1 unlit, 2-5 ViewDebug); y = giRadianceCeiling; z/w unused.
        f32 viewParams[4] = {};
        // ReSTIR GI control: x = running, y = history valid, z = write buffer slice, w = poison debug view.
        f32 giRestirParams[4] = {};
        // x = projected decals in t24 (0 = every decal call is skipped); y = light-grid header record in t18 (0 = none);
        // z, w = shadow rays per pixel / per hit for the light list (Settings::lightRaysPerBlock / PerHit).
        f32 decalParams[4] = {};
    } cb_;

    // `cbuffer VoxiFrame : register(b4)` in voxi.hlsl and voxi_gi.hlsli mirrors this byte-for-byte (no guard).
    // Append/insert changes must update both and voxi_gi.hlsli's copy.
    static_assert(sizeof(FrameConstants) == 752,
                  "cbuffer VoxiFrame in modules/render.voxi/shaders/voxi.hlsl mirrors this byte for byte");
    static_assert(sizeof(FrameConstants) % 16 == 0, "must be a legal constant-buffer size");

    // ---- GI rebuild gate: skip voxelisation whose result would be bit-identical ----
    // Hashes inputs the result depends on; reuses volume unchanged when nothing moved.
    bool giSnapshotUnchanged() const;
    void takeGiSnapshot();
    u64 giDrawsKey() const;
    // Whether PSVoxel bakes sky into volume: negation of cb_.viewParams[2]. THE SETTING, not runtime state.
    bool voxelSkyInjected() const { return !giRestirWanted(); }

    u64 giDrawsKey_ = 0;
    u64 giDrawsCount_ = 0;
    u64 giDrawsMeshKey_ = 0;
    u64 giDrawsWorldKey_ = 0;
    u64 giDrawsMatKey_ = 0;
    mutable u64 giDrawsRejects_ = 0;
    mutable u64 giDrawsCountMoved_ = 0, giDrawsMeshMoved_ = 0, giDrawsWorldMoved_ = 0, giDrawsMatMoved_ = 0;
    mutable u64 giDrawsNextReport_ = 32;
    std::vector<rhi::MeshHandle> giSnapMeshes_;
    mutable u32 giDrawsDiffReports_ = 0;
    void giDrawsSubKeys(u64& count, u64& mesh, u64& world, u64& mat) const;
    rhi::SkyAtmosphere giSky_{};
    f32 giSnapCenter_[3] = {};
    f32 giSnapExtent_ = -1.0f;
    bool giSnapValid_ = false;
    bool giSnapVoxelSky_ = true;
    u64 giSkipped_ = 0, giRebuilt_ = 0;
    mutable u32  giGateWhyMask_ = 0;
    u32          voxelCullLogs_ = 0;
    bool         drawCapReported_ = false;
    u64  giGateNextReport_ = 64;
    u64  giGateLastTicks_ = 0, giGateLastSkipped_ = 0;

    // ---- M4: force every tick past gate, bypassing cache in both directions ----
    bool giForceRebuild_ = false;

    // ---- W3: bound clear/resolve/mip-filter dispatch to the actual changed region ----
    // See setGiBoundedDispatch for contract and voxelizePass for details.
    bool giBoundedDispatch_ = true;
    VoxelBox giDispatchBox0_{};
    // LAST rebuild's draws box (union of AABB, not box0).
    VoxelBox giBoxPrevDraws_{};
    // False when box cannot be trusted: no rebuild yet, unbounded draw, or restored from cache.
    bool giBoxPrevValid_ = false;
    // Volume resolution/centre/extent the box was recorded under; moves invalidate it.
    u32 giBoxRes_ = 0;
    f32 giBoxCentre_[3] = {};
    f32 giBoxExtent_ = -1.0f;

    // Free the injection accumulator after the gate has gone quiet for a while (see setGiFreeAccumulator).
    // voxi.giFreeAccumulator (EditorConsole.hpp) and --gi-free-accumulator 0 both turn it back off.
    bool giFreeAccumulator_ = true;
    // Set by the rebuild gate when voxelAccumTex_ is missing; consumed by manageInjectionAccumulator() the next prePass.
    bool giAccumWanted_ = false;
    // Allocation failure warning, said once; reset on next successful recreate.
    bool giAccumRecreateFailedLogged_ = false;
    // Backoff: counts down to the next retry instead of retrying every tick while nonzero.
    u32 giAccumRecreateBackoffTicks_ = 0;
    // Cooldown for the next failure, doubling on each consecutive failure (capped at kGiAccumRecreateBackoffMax).
    u32 giAccumRecreateBackoffNext_ = 0;
    static constexpr u32 kGiAccumRecreateBackoffMin = 30;
    static constexpr u32 kGiAccumRecreateBackoffMax = 1800;
    // Consecutive GI ticks with nothing to do; incremented while giConvergeTicks_ == 0, reset by any rebuild.
    u32 giQuietTicks_ = 0;
    // Tiny stand-in UAV (4x1x1, R32_UINT) bound to bindings_/clearBindings_/resolveBindings_ slot 1 in place of voxelAccumTex_ while freed.
    // Every binding set naming a resource must be rebound before that resource is destroyed.
    rhi::TextureHandle voxelAccumPlaceholder_ = 0;
    // Quiet GI ticks before freeing (~1 s). 240 kept 2 GiB alive through a level load's first seconds,
    // longer still while paging slowed the frame rate; a later rebuild waits one tick for the recreate.
    static constexpr u32 kGiAccumulatorQuietTicks = 60;
    // Freed and needed again within kGiAccumChurnFrames: the quiet ticks before the next free double (to
    // kGiAccumulatorQuietTicksMax). A 2 GiB create or release is a 60-200 ms frame; flying through a
    // streamed level would otherwise pay it every few seconds.
    static constexpr u32 kGiAccumulatorQuietTicksMax = 3840;
    static constexpr u64 kGiAccumChurnFrames = 1200;
    u32 giAccumQuietNeeded_ = kGiAccumulatorQuietTicks;
    u64 giAccumFreedFrame_ = 0;   // rtFrameIndex_ + 1 at the last free, 0 never
    // Per-frame BLAS build budget (structure bytes; scratch is of the same order). See buildAccelerationStructures.
    // ~1.4 ms per MB (NeonDistrict: 512 MB was ~0.7 s in one submission); 8 MB keeps a streamed level's
    // builds near 10 ms a frame.
    static constexpr u64 kBlasBuildBytesPerFrame = 8ull << 20;
    bool blasBuildsDeferred_ = false;   // some draw's first BLAS build waits for the next frame
    // Meshes past 2 * kRtChunkTriangles are traced as BLASes over index ranges of kRtChunkTriangles,
    // built a few a frame (rtChunksFor). rtUnchunked_: meshes already found small enough.
    static constexpr u32 kRtChunkTriangles = 131072;
    struct RtChunks { std::vector<rhi::BlasHandle> blas; std::vector<u32> firstIndex, indexCount; };
    std::unordered_map<rhi::MeshHandle, RtChunks> rtChunks_;
    std::unordered_set<rhi::MeshHandle> rtUnchunked_;
    RtChunks* rtChunksFor(rhi::MeshHandle mesh);
    std::vector<u32> rtInstanceIndexOffset_;   // per RT instance: its chunk's first index (0 = whole mesh)
    bool blasDeferLogged_ = false;

    // ---- the GI derived-data cache ----
    // Resolved volume written beside the project, keyed by the gate's inputs (giCacheKey).
    std::string giCacheDir_;
    // Tried once per key, hit or miss (must not re-read absent file every rebuild).
    fmt::GiCacheKey giCacheTriedKey_{};
    bool giCacheTried_ = false;
    // Readback is a GPU command; waits kGiCacheReadbackDelay frames, then reads/writes the file.
    static constexpr u32 kGiCacheReadbackDelay = 4;
    // How long a volume must stay settled before being worth a file.
    static constexpr u32 kGiCacheDwellTicks = 120;
    rhi::BufferHandle giCacheReadback_ = 0;
    rhi::BufferHandle giCacheUpload_ = 0;
    u64  giCacheBufBytes_ = 0;
    u32  giCacheDumpCountdown_ = 0;
    fmt::GiCacheKey giCachePendingKey_{};
    // Only a settled volume is cached: a rebuild raises giCacheSettlePending_, first quiet gate tick after convergence schedules the readback.
    bool giCacheSettlePending_ = false;
    // True while every bake since the last settled volume was triggered by the cloud clock alone.
    bool giCacheSettleCloudOnly_ = false;
    // Keys this session already restored or queued, bounded like the directory itself.
    std::vector<fmt::GiCacheKey> giCacheKnownKeys_;
    bool giCacheKeyKnown(const fmt::GiCacheKey& k) const;
    void giCacheRememberKey(const fmt::GiCacheKey& k);

    // ---- the write-behind buffer ----
    // Bakes land in RAM and go to disk in batches: volumes accumulate here and reach the filesystem when the RAM budget is exceeded or on shutdown.
    // Safe because GiCache.hpp states: "nothing here is authored and precious: the whole directory can be deleted anytime".
    std::vector<fmt::GiCacheEntry> giCachePendingEntries_;
    u64 giCachePendingBytes_ = 0;
    // Default 256 MiB. Editor Preferences > Derived Data Cache moves it.
    u64 giCacheRamBudget_ = 256ull * 1024ull * 1024ull;
    // True when the rebuild was triggered by nothing but the cloud clock; latched so giCacheScheduleDump can decline writing.
    // Cleared every gate evaluation. mutable because giSnapshotUnchanged is const.
    mutable bool giRebuildCloudOnly_ = false;

    // One definition of "voxelizePass will inject this draw" -- giDrawsKey, giDrawsSubKeys and voxelizePass must all agree exactly.
    bool giVoxelisedDraw(const Draw& d) const;
    // Volume-oversized warning, said once; never cleared.
    bool giCacheOversizeWarned_ = false;

public:
    // The write-behind budget, in bytes. Lowering it below what is already buffered flushes immediately.
    void setGiCacheRamBudget(u64 bytes);
    u64  giCacheRamBudget() const { return giCacheRamBudget_; }
    // What is buffered but not yet written -- for the preferences page to show.
    u64  giCachePendingBytes() const { return giCachePendingBytes_; }
    u32  giCachePendingCount() const { return static_cast<u32>(giCachePendingEntries_.size()); }
    // Writes every buffered entry, sweeps the directory once, and empties the buffer. Returns how many files were written.
    u32  giCacheFlush();
private:
    // Per-mip byte offsets inside the readback buffer, in the BACKEND's footprint layout.
    std::vector<u64> giCacheMipOffsets_;
    bool giCacheUnsupported_ = false;

    // The key describing the volume as it stands after takeGiSnapshot.
    fmt::GiCacheKey giCacheKey() const;
    // Tries to fill voxelTex_ from disk. True when the volume now holds the cached answer.
    bool giCacheRestore(rhi::IRenderContext& ctx);
    // Schedules a readback of the settled voxelTex_. False only when it should be asked again (readback in flight); true once the volume needs nothing more.
    bool giCacheScheduleDump(rhi::IRenderContext& ctx);
    // Warns once that a volume is too big to cache.
    void giCacheWarnOversize(u64 bytes);
    // Ticks the countdown and writes the file when it reaches zero.
    void giCacheTick();
    // Sizes giCacheReadback_/giCacheUpload_ and giCacheMipOffsets_ for the current volume.
    bool giCacheEnsureBuffers();
    // Releases giCacheReadback_/giCacheUpload_ once the copy each one exists for has been RECORDED.
    // Safe to call immediately after recording, not after the GPU has run the copy.
    void giCacheFreeBuffers();

    // Builds this frame's cascade matrices and splits. Returns the usable cascade count, 0 if none.
    u32 fitCascades();
    // Builds the GI-only shadow map's matrix, fitted to the GI volume. Writes cb_.giShadowViewProj/giShadowParams.
    void fitGiShadow();
    // The GI-only map's own world-space bounding sphere, for giShadowPass's per-draw cull.
    f32 giShadowCentre_[3] = {};
    f32 giShadowRadius_ = 0.0f;
    // Per-cascade world-space bounding sphere, filled in by fitCascades and read by shadowPass.
    f32 cascadeCentre_[4][3] = {};
    f32 cascadeRadius_[4] = {};

    // ---- ray-traced temporal history: sun shadow AND reflections ----
    // Trace fresh rays per pixel per frame with no cross-frame reuse. Blend each frame's fresh sample with a REPROJECTED sample of the previous frame,
    // so a static or slow-moving result converges toward the brute-force ray count over several frames.
    //
    // PING-PONGED, not one texture: reprojection reads a different texel than the one this frame writes.
    // Shadow and reflection have their own texture pairs but share one write index, valid flag, frame index and tile schedule.
    //
    // Two RG32Float textures for the shadow; two RGBA16F textures for reflections. One texture per pair is this frame's write target (UAV),
    // the other last frame's result (SRV); the depth channel tells disocclusion apart from a legitimate reprojection.
    rhi::TextureHandle rtShadowHist_[2] = {0, 0};
    rhi::TextureHandle rtReflHist_[2] = {0, 0};
    // The sky-occlusion ray: at one ray/pixel the estimator is BINARY, the noisiest Monte Carlo estimate. Firing four incoherent rays per 4x4 tile
    // trades salt-and-pepper for visible blocks; accumulating against a reprojected history buys the sample count over time.
    // RG32Float: x = openness, y = linear depth for the disocclusion test.
    rhi::TextureHandle rtAoHist_[2] = {0, 0};
    // Sky-occlusion ray's hit distance, [0,1] as a fraction of the ray's TMax. Not ping-ponged, not a history: this frame's raw measurement.
    // Nothing in Voxi's own passes reads it: denoiser filters it (Aver.Render.Denoise), output read back at t14.
    rhi::TextureHandle rtAoHitDist_ = 0;

    // ---- THE DENOISER (Aver.Render.Denoise: AMD FidelityFX Denoiser), over both noisy signals ----
    // Sky-occlusion hit distance and ReSTIR GI radiance, each with its own history. Every input but the signal comes from the G-buffer.
    // Optional at every level: absent without G-buffer, absent if shaders fail to compile. Output of 0 means "not denoised this frame".
    render::denoise::Denoiser denoiser_;
    rhi::TextureHandle denoiseAoOutput_ = 0;
    rhi::TextureHandle denoiseGiOutput_ = 0;
    // Reflection denoising: CSRdRefl's fresh sample + hit distance (u23), its 1x1 stand-in while off,
    // and the denoiser's result (t23).
    rhi::TextureHandle rdReflDnIn_ = 0, rdReflDnPlaceholder_ = 0;
    bool bindReflDnPlaceholder();
    rhi::TextureHandle denoiseReflOutput_ = 0;
    bool rdReflDnBound_ = false;   // u23 holds rdReflDnIn_ this frame (CSRdRefl writes it)

    // ---- NRD2 (docs/rendering/NRD2.md): single-frame denoiser of Stage B's demodulated lighting ----
    // Replaces FidelityFX and every Voxi history for the frame (beginShadowHistory decides). Stage B's
    // NRD2 variant writes its targets through u2/u3/u9/u23, slots such a frame leaves unused.
    render::denoise::Nrd2 nrd2_;
    rhi::PipelineHandle rayDrivenSplitNrd2Pso_ = 0;   // Stage B, AVER_NRD2=1 (built on first use)
    rhi::PipelineHandle rdHalfFillCsPso_ = 0;         // CSRdHalfFill: half-rate tracing filled this frame
    bool nrd2Tried_ = false;      // the build above ran since the last createScenePipelines
    bool nrd2Frame_ = false;      // this frame runs NRD2
    bool nrd2Bound_ = false;      // u2/u3/u9/u23 hold NRD2's targets
    bool nrd2FallbackLogged_ = false;
    bool nrd2RunLogged_ = false;
    // A capture asked for before NRD2 was built: handed to nrd2_ once ensureNrd2 has created it.
    render::denoise::Nrd2CaptureConfig nrd2CaptureCfg_{};
    bool nrd2CapturePending_ = false;
    rhi::Format sceneColorFmt_ = rhi::Format::Unknown, sceneDepthFmt_ = rhi::Format::Unknown;
    u32 sceneSampleCount_ = 1;
    bool nrd2Wanted() const;
    bool ensureNrd2();             // pipelines (built once) and targets for this frame; false = not this frame
    void bindNrd2Targets();
    // Advanced once per frame at the top of beginShadowHistory; its low bit is half-rate GI's parity.
    u32  denoiseFrame_ = 0;
    bool denoiseWarnedMsaa_ = false;
    // Set the first time this frame's G-buffer inputs disagree in size with the signal the denoiser is about to be resized to.
    // Cleared once sizes agree again (expected to self-heal within a frame or two).
    bool denoiseWarnedInputSizeMismatch_ = false;
    // ReSTIR GI radiance handed to the denoiser: rgb linear indirect diffuse. NOT ping-ponged.
    rhi::TextureHandle giRadiance_ = 0;
    // ---- MILESTONE 4: half-rate ReSTIR GI on a checkerboard (rayDrivenStages == 2) ----
    // denoiseGiRanThisFrame_: true only when THIS frame's denoiser dispatch produced the GI output.
    bool denoiseGiRanThisFrame_ = false;
    // Whether the GI radiance the denoiser is about to read was traced at half rate (LAST frame's CSRdGi), and which parity.
    bool denoiseGiInputHalfRate_  = false;
    u32  denoiseGiHalfRateParity_ = 0;
    // While the sun moves, the denoiser keeps a short history. Counts down from 2 on every frame the sun moved.
    u32  denoiseSunMovingHold_ = 0;

    // ---- ReSTIR GI: the reservoir buffer and the previous-frame surface it resamples against ----
    // giReservoirs_ holds GiPackedReservoir (voxi_reservoir.hlsli), 32 bytes each; one RWStructuredBuffer holds BOTH ping-pong slices, row-major per slice.
    // Only which slice is read/written changes (cb_.giRestirParams.z), not a descriptor swap.
    rhi::BufferHandle giReservoirs_ = 0;
    // Element count (GiPackedReservoir units, w*h*2) the buffer was sized for.
    u32  giReservoirElemCapacity_ = 0;

    // ---- STAGED RAY-DRIVEN PASSES (milestone 1): the resources CSRdVisibility/CSRdShadow/the
    // AVER_RD_SPLIT pixel shader pass a record through, u11/u12 in every Voxi binding set. ----
    // rdVisBuf_: one uint4 (16 bytes) per pixel of the SCENE RENDER TARGET.
    // A StructuredBuffer, reallocated in ensureRdStagedResources only when it no longer fits the target or holds more than twice what it needs.
    rhi::BufferHandle rdVisBuf_ = 0;
    // Element count (uint4 units) rdVisBuf_ was sized for.
    u32  rdVisBufElemCapacity_ = 0;
    // rdSunVisTex_: RGBA16F, rgb = the sun ray's transmittance (CSRdShadow), same resolution as rdVisBuf_.
    rhi::TextureHandle rdSunVisTex_ = 0;
    // MILESTONE 2's lighting-stage output pair, u13/u14, sharing rdSunVisTex_'s exact lifecycle.
    // rdGiTex_: rgb = ReSTIR GI's indirect diffuse, a is unused (always 1.0).
    rhi::TextureHandle rdGiTex_ = 0;
    // rdAoTex_: r = the sky-occlusion ray's transmittance, gba unused.
    rhi::TextureHandle rdAoTex_ = 0;
    // MILESTONE 3: CSRdRefl's output, u15.
    // rdReflTex_: rgb = the ray-traced reflection's radiance, a = 1.0 when CSRdRefl traced this pixel, 0.0 otherwise.
    rhi::TextureHandle rdReflTex_ = 0;
    // ---- SUB-STAGE SPLITS' OWN BUFFERS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): u17/u18,
    // sharing rdVisBuf_'s StructuredBuffer shape.
    // rdGiCandBuf_: one RdGiCand (64 bytes, voxi_restir.hlsli) per pixel.
    rhi::BufferHandle rdGiCandBuf_ = 0;
    u32  rdGiCandBufElemCapacity_ = 0;
    // rdShadowTileBuf_: one uint mask per 8x8 tile (ceil(W/8) x ceil(H/8) tiles).
    rhi::BufferHandle rdShadowTileBuf_ = 0;
    u32  rdShadowTileElemCapacity_ = 0;
    // The render-target size the rd*Tex_/rdVisBuf_ resources were last created at, and rdVisBuf_'s row pitch in pixels.
    u32  rdStagedW_ = 0, rdStagedH_ = 0;
    u32  rdStagedRowPitch_ = 0;
    bool rdVisWrittenThisFrame_ = false;   // set by recordStagedRayDriven, cleared in prePass
    // MILESTONE 4: giCbWrittenThisFrame_ -- written every recordStagedRayDriven call.
    // Consumed at the top of NEXT frame's beginShadowHistory, reset here.
    bool giCbWrittenThisFrame_ = false;
    u32  giCbParityWritten_ = 0;
    // Tiny stand-ins bound at u11-u15 whenever the real resources don't exist.
    // Every declared UAV slot always has a valid descriptor of the right kind.
    rhi::BufferHandle  rdVisBufPlaceholder_ = 0;
    rhi::TextureHandle rdSunVisPlaceholder_ = 0;
    rhi::TextureHandle rdGiPlaceholder_     = 0;
    rhi::TextureHandle rdAoPlaceholder_     = 0;
    // MILESTONE 3's own placeholder.
    rhi::TextureHandle rdReflPlaceholder_   = 0;
    // SUB-STAGE SPLITS' OWN PLACEHOLDERS.
    rhi::BufferHandle rdGiCandBufPlaceholder_     = 0;
    rhi::BufferHandle rdShadowTileBufPlaceholder_ = 0;
    // (Re)creates or releases the staged resources for the given render-target size.
    // Called from onRenderTargetsChanged (resize) and setSettings (rayDrivenStages on/off edge).
    bool ensureRdStagedResources(u32 width, u32 height);
    // Whether Settings::rayDrivenStages asks for the staged split at all.
    bool rdStagedWanted() const { return settings_.rayDrivenStages >= 1u; }
    // Whether the staged resources are worth ALLOCATING at all.
    bool rdStagedResourcesWanted() const { return rdStagedWanted() && rayTracingWanted(); }
    // Whether THIS frame's ray-driven primary will run as staged passes rather than the single PSRayDriven draw.
    bool rdStagedActive(const char** reason = nullptr) const;
    // Latches the ONE fallback warning rdStagedActive()'s `reason` produces.
    bool rdStagedFallbackLogged_ = false;
    // Said once, the first frame the staged passes actually record.
    bool rdStagedRunLogged_ = false;
    // MILESTONE 4's own "said once, each half of the story" pair.
    bool rdGiCbRunLogged_ = false;
    bool rdGiCbFallbackLogged_ = false;
    // SUB-STAGE SPLITS' OWN PAIRS.
    bool rdShadowTilesRunLogged_ = false;
    bool rdShadowTilesFallbackLogged_ = false;
    bool rdGiSplitRunLogged_ = false;
    bool rdGiSplitFallbackLogged_ = false;
    bool rdReflSplitRunLogged_ = false;
    bool rdReflSplitFallbackLogged_ = false;

    // ---- ReSTIR GI: the previous-frame surface it resamples against (giSurfPosHist_/giSurfNrmHist_) ----
    // giLoadPrevSurface(idx) needs a PREVIOUS frame's primary surface. Two RG32Float TEXTURES, NOT ONE RGBA32F
    // (rhi::Format has no four-channel 32-bit float format). Full 32-bit precision matters: giReconnectionJacobian's partial-Jacobian terms
    // are distance-squared RATIOS.
    // giSurfPosHist_: xy = world position x/y. giSurfNrmHist_: x = world position z, y = packed normal.
    // 0 in the packed-normal channel means "nothing written here" (sky miss or before this pair existed).
    // Depth for the giIsSimilarSurface test is re-derived as `mul(float4(storedWorldPos, 1.0), gPrevViewProj).w`.
    rhi::TextureHandle giSurfPosHist_[2] = {0, 0};
    rhi::TextureHandle giSurfNrmHist_[2] = {0, 0};

    // ---- U1/2.11: the half-resolution ReSTIR VISIBILITY history pair (t16/u10) ----
    // See giVisHistWanted() for when this is wanted. Half the linear dimension of giSurfPosHist_/giSurfNrmHist_, rounded up.
    // RGBA16F: r = F3 reuse-visibility EMA, g = F2 traced-luminance EMA, b = F2 unoccluded-sky-luminance EMA, a = 1 written / 0 never.
    rhi::TextureHandle giVisHist_[2] = {0, 0};
    // True only once a full write+swap cycle has happened with the pair bound this frame.
    bool giVisHistValid_ = false;
    // RESOURCE STATE: true once the read side was left in UnorderedAccess by an earlier write since the pair was (re)created.
    bool giVisHistPrimed_ = false;
    // Allocation failure warning, said once; cleared on next successful create.
    bool giVisHistFailLogged_ = false;

    // ---- LOCAL LIGHTS (LAMPS): the visibility history pair, t19 (read) / u19 (write) ----
    // Two RGBA16F textures at the shadow history's size. a = accumulated visibility (only channel used; rgb stays 0).
    // Rests in ShaderResource like rtShadowHist_. The raster pixel shaders read t19 and write u19.
    rhi::TextureHandle rdLocalHist_[2] = {0, 0};
    // A 1x1 RGBA16F SRV+UAV stand-in bound at both t19/u19 when the pair doesn't exist.
    rhi::TextureHandle rdLocalHistPlaceholder_ = 0;
    // The write side bound at u19 THIS frame (beginShadowHistory), 0 when the pair wasn't bound.
    rhi::TextureHandle rdLocalOutThisFrame_ = 0;
    // RESOURCE STATE: true once the read side was left in UnorderedAccess by an earlier active frame since the pair was (re)created.
    bool rdLocalHistPrimed_ = false;
    bool rdLocalHistFailLogged_ = false;
    // CONTENT TRUST: the rtFrameIndex_ of the last frame a scene pass wrote u19 and the light-list hash it wrote under.
    u32 rdLocalHistFrame_ = 0;
    u64 rdLocalHistHash_ = 0;
    // "Local lights running" said once, the first frame any scene pass shades with lamps.
    bool rdLocalLightsRunLogged_ = false;
    // Settings::giRestirVisibility, cached at setSettings. 2 (HalfResolution) is the struct default.
    u32 giRestirVisibility_ = 2;
    // RADIANCE CACHE state (stage 1). Created on the first frame Cached is wanted and destroyed when it stops being.
    NeuRaC rc_;
    bool neuracLive_ = false;
    u32 neuracView_ = 0;   // setNeuRaCView: mode in bits 0-2, grid bit 3 (packed into gAmbientParams.w << 8)
    // The NeuRaC::Bindings::generation last written into table 0's t22/u20/u21; a different
    // value from beginFrame means the buffers changed and bindings_ must be rewritten (before the
    // first bind of the frame -- Vulkan ringed sets forbid writing a bound set).
    u32 rcBoundGeneration_ = 0;
    // True while t22/u20/u21 hold real cache buffers.
    bool rcSlotsBound_ = false;
    // 64 B UAV-capable stand-in rebound to u20/u21 at teardown. Created lazily at teardown only.
    rhi::BufferHandle rcPlaceholder_ = 0;
    // Once-only logs about cache support and twin-build result.
    bool rcUnsupportedLogged_ = false;
    // rc_.create() failed: do not retry every frame. Cleared when Cached stops being the wanted mode.
    bool rcCreateFailed_ = false;
    // createNeuRaCTwins ran since the last createScenePipelines.
    bool rcTwinsTried_ = false;
    // Path Tracing's progressive accumulation (u22, Stage B): per pixel, the running mean in rgb and
    // (half-float depth in m << 16 | frame count) in w. Sized by ensurePtAccum; restarted by key change.
    static constexpr u32 kPtAccumElemBytes = 16;
    static constexpr u32 kPtAccumMaxFrames = 1024;   // past this the mean becomes a 1/1024 moving average
    static constexpr u32 kPtAccumRefFrames = 16384;  // Reference mode's cap (the count is 16 bits)
    static constexpr u32 kPtAccumLampFrames = 8;     // a frame whose lamps changed keeps an 8-frame average
    static constexpr u32 kPtAccumKeyFloats = 48;
    rhi::BufferHandle ptAccumBuf_ = 0;
    rhi::BufferHandle ptAccumPlaceholder_ = 0;
    u32  ptAccumElemCapacity_ = 0;
    bool ptAccumValid_ = false;
    f32  ptAccumKey_[kPtAccumKeyFloats] = {};
    u64  ptAccumLampHash_ = 0;
    // Path Tracing in Reference mode this frame: the staged GI, sky-occlusion, lamp and reflection
    // passes are skipped and Stage B traces one path per pixel.
    bool ptReferenceWanted() const { return pathTracingWanted() && settings_.ptMode == 1u; }
    bool ensurePtAccum();
    // createPathTraceTwins ran since the last createScenePipelines; the mode ran this frame; said-once logs.
    bool ptTwinsTried_ = false;
    bool translucentInPath_ = false;   // Stage B composited translucency this frame (ptBounceParams.w)
    bool ptRanThisFrame_ = false;
    bool ptRunLogged_ = false;
    bool ptFallbackLogged_ = false;
    bool createPathTraceTwins();
    // Build/teardown/per-frame hooks (VoxiRenderer.cpp).
    void updateNeuRaC(rhi::IRenderContext& ctx);
    bool createNeuRaCTwins();
    void teardownNeuRaC();
    // Settings::giRestirSpatialSamples, cached defensively.
    u32 giRestirSpatialSamples_ = 0;
    u32 giRestirMaxHistory_ = 8;
    // voxi.blendedGiCone's live backing store.
    bool blendedGiCone_ = false;
    // voxi.giVisPathView's live backing store.
    bool giVisPathView_ = false;

    // False right after the pair is (re)created and true only once a full write+swap cycle has happened with giRestirWanted() true.
    // DELIBERATELY SEPARATE FROM rtHistValid_ because giMode is an INDEPENDENT switch a user can flip mid-session.
    bool giHistValid_ = false;
    // RESOURCE STATE: true once the read side was left in UnorderedAccess by a write since the pair was (re)created.
    bool giHistPrimed_ = false;

    u32  rtShadowHistW_ = 0, rtShadowHistH_ = 0;
    u32  rtHistWriteIdx_ = 0;
    // False right after creation or a resize: the textures hold no real previous frame yet.
    bool rtHistValid_ = false;
    // RESOURCE STATE: true once rtShadowHist_/rtReflHist_/rtAoHist_'s read side was left in UnorderedAccess by a write since creation.
    bool rtHistPrimed_ = false;
    // THE SUN THE HISTORY WAS ACCUMULATED UNDER. The temporal denoiser blends the previous frame with a geometric validity test that ignores sun movement.
    f32  rtHistSunDir_[3]   = {0, 0, 0};
    f32  rtHistSunColor_[3] = {0, 0, 0};
    f32  rtHistSunIntensity_ = -1.0f;
    // True when this frame's sun differs from the one above.
    bool rtHistSunMoved() const;
    // True when it differs by MORE than one slider-drag step.
    bool rtHistSunJumped() const;
    // This frame's camera view-projection, captured where fitCascades() reads the camera.
    f32  curViewProj_[16] = {};
    f32  prevViewProj_[16] = {};
    // The reflection denoiser runs a frame late, on the G-buffer's frame (prev*) and the one before.
    f32  curInvViewProjRel_[16] = {}, prevInvViewProjRel_[16] = {};
    f32  curCamPos_[3] = {}, prevCamPos_[3] = {};
    f32  prev2ViewProj_[16] = {};
    f32  prev2SceneViewport_[4] = {};
    // Same idea, for the scene viewport rect the reprojected NDC needs.
    f32  curSceneViewport_[4] = {};
    f32  prevSceneViewport_[4] = {};
    // (Re)creates rtShadowHist_ and rtReflHist_ at the given resolution. DESTROYS all four when rayTracingWanted() is false.
    bool ensureShadowHistory(u32 width, u32 height);
    // Rebinds t19/u19 to rdLocalHistPlaceholder_, then destroys rdLocalHist_.
    void releaseLocalHistory();
    // The size onRenderTargetsChanged last asked for.
    u32  rtHistWantW_ = 0, rtHistWantH_ = 0;
    // The pbr::MaterialGraphRegistry revision the scene pipelines were last compiled against.
    u64  scenePipelineGraphRev_ = ~0ull;
    // The shader-file revision these pipelines were compiled from.
    u64  scenePipelineShaderRev_ = 0;
    // Whether anything will EVER write the ray-traced histories under the current settings.
    bool rayTracingWanted() const { return rtSupported_ && settings_.rayTracing != Quality::Off; }
    // The AMBIENT history pair is wanted only where the ray that fills it is traced.
    bool aoHistoryWanted() const { return rayTracingWanted() && settings_.giSkyOcclusionRays > 0; }
    // Whether ReSTIR GI (giMode == 1) will ACTUALLY run this frame.
    bool giRestirWanted() const { return rayTracingWanted() && giEnabled() && giMode_ == 1u; }

    // Whether the half-resolution ReSTIR VISIBILITY history pair (t16/u10, giVisHist_) is wanted -- a STRICT SUBSET of giRestirWanted().
    bool giVisHistWanted() const {
        return giRestirWanted() && (giRestirVisibility_ == 2u || giRestirVisibility_ == 4u);
    }

    // RADIANCE CACHE (stage 1): whether Cached mode (giRestirVisibility_ == 4) is the requested mode AND ReSTIR GI is running.
    // ReSTIR Path Tracing always runs it: its paths train the cache and end in it.
    bool neuracWanted() const {
        return giRestirWanted() &&
               (giRestirVisibility_ == 4u || (pathTracingWanted() && settings_.ptMode == 0u));
    }

    // LOCAL LIGHTS (LAMPS): whether rdLocalHist_ is worth allocating.
    bool rdLocalHistWanted() const {
        // Always with ray tracing: the staged visible-surface pass writes the pixel's light slots here (u19),
        // the sun's among them (UNIFIED_LIGHTS.md), whether or not lamps are on.
        return rayTracingWanted() && dev_ && dev_->caps().computeInScenePass;
    }

    // Fog occlusion: whether airVisTex_ will ACTUALLY be created/kept.
    bool airVisWanted() const { return settings_.fogOcclusion && airVisPso_ != 0; }

public:
    // THE SKY-OCCLUSION RAY'S HIT DISTANCE FOR THIS FRAME, or 0 when the ray isn't running.
    // Rests in ResourceState::UnorderedAccess. 0 is the answer, not an error.
    [[nodiscard]] rhi::TextureHandle ambientHitDistanceTexture() const { return rtAoHitDist_; }

    // The staged ray-driven visibility record, on frames CSRdVisibility wrote it (NeuRAA reads it).
    bool primaryVisibility(rhi::PrimaryVisibility& out) const override;

    // NRD2 phase 3 training capture (render::denoise::Nrd2Capture). Steps on NRD2 frames only, so it
    // waits for the Denoiser setting at NRD2 (--denoiser 2).
    void startNrd2Capture(const render::denoise::Nrd2CaptureConfig& cfg);
    [[nodiscard]] bool nrd2CaptureHolding() const { return nrd2_.captureHolding(); }
    [[nodiscard]] bool nrd2CaptureActive() const { return nrd2_.captureActive() || nrd2CapturePending_; }
    // NRD2 phase 4 network weights (user file over shipped) and what the network is doing.
    void setNrd2WeightsPaths(std::string user, std::string shipped) {
        nrd2_.setNetworkWeights(std::move(user), std::move(shipped));
    }
    [[nodiscard]] render::denoise::Nrd2NetworkStatus nrd2NetworkStatus() const { return nrd2_.networkStatus(); }

private:
    // Path tracing wanted -- a different question from ray tracing wanted, deliberately asking the other setting.
    bool pathTracingWanted() const { return rtSupported_ && settings_.pathTracing != Quality::Off; }
    // The DEBUG RAYMARCH has taken over the scene.
    bool debugViewActive() const { return giReady_ && giEnabled() && debugView_; }
    // RAY-DRIVEN PRIMARY VISIBILITY is running this frame.
    bool rayDrivenActive() const { return rtActive_ && rtRenderMode_ == 1u && rayDrivenPso_ != 0; }

public:
    // PREDICTS suppressesScene() (debugViewActive() || rayDrivenActive()) for THIS frame.
    // Fixes a race: rayDrivenActive() reads rtActive_ from LAST frame's build over drawsPrev_, which this frame's onRender submitted.
    // drawsPrev_ non-empty is treated as sufficient: the gap is the same shape as the one-frame race (a rare, brief disagreement).
    bool willSuppressSceneThisFrame() const {
        const bool rayDrivenWillBeActive = rtSupported_ && settings_.rayTracing != Quality::Off &&
                                            !draws_.empty() && rtRenderMode_ == 1u &&
                                            rayDrivenPso_ != 0;
        return debugViewActive() || rayDrivenWillBeActive;
    }

private:
    // Whether the ray-traced history textures will be read and written this frame.
    // Tests debugViewActive(), NOT suppressesScene() -- the debug raymarch replaces shading entirely, but PSRayDriven still calls rtShadowTemporal.
    bool shadowHistoryActive() const {
        return rtActive_ && rtShadowHist_[0] && rtShadowHist_[1] &&
               rtReflHist_[0] && rtReflHist_[1] && !debugViewActive();
    }
    // Swaps the read/write roles, transitions all six textures, rebinds them and sets cb_.prevViewProj/rtHistParams for this frame.
    void beginShadowHistory(rhi::IRenderContext& ctx);
    // Advances prevViewProj_/rtHistWriteIdx_/rtHistValid_ for NEXT frame, once fitCascades() has filled curViewProj_.
    void endShadowHistory();

    bool giEnabled() const { return settings_.globalIllumination != Quality::Off; }
    f32 sunDir_[3]   = {rhi::SkyAtmosphere{}.sunDirection[0],
                        rhi::SkyAtmosphere{}.sunDirection[1],
                        rhi::SkyAtmosphere{}.sunDirection[2]};

    // How many extra bakes a change buys: PSVoxel's feedback term adds one bounce per rebuild.
    static constexpr u32 kGiConvergeTicks = 5;
    u32  giConvergeTicks_ = 0;

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
    // See setGiPoisonView. Independent of debugView_.
    bool giPoisonView_ = false;
    // See setLightingLegacyBits. 0 (every fix live) until a console command or --lighting-legacy sets a bit.
    u32 lightingLegacyBits_ = 0;
    bool unlit_ = false;
    // See setViewDebug. None (0) is bit-identical to every build before this view existed.
    ViewDebug viewDebug_ = ViewDebug::None;
    bool paused_ = false;
    // See setConeTraceEnabled. Defaults true, bit-identical to every build before this toggle existed.
    bool coneTraceEnabled_ = true;
};

} // namespace aver::voxi
