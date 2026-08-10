// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/voxi/Voxi.hpp"

#include <unordered_map>
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
    // Replaces the scene with a raymarch of the volume.
    void setDebugView(bool on);

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

    // Turns on the frame-period report. See rtShadowRays_ for what it is for and what it is not.
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
    void submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                const void* drawConstants, u32 drawConstantBytes);
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                    const void* drawConstants, u32 drawConstantBytes) override {
        submit(mesh, world, baseColor, metallic, roughness, drawBinding, drawConstants, drawConstantBytes);
    }

    const char* name() const override { return "Voxi"; }
    // Acceleration structures, shadow map, clear volume, voxelise + inject, filter mips.
    void prePass(rhi::IRenderContext& ctx) override;
    // True whenever the feature initialised: Voxi owns the lit pixel shader outright.
    bool overridesScenePipeline() const override;
    rhi::BindingSetHandle sceneBindingSet() const override { return bindings_; }
    // Hands the backend Voxi's per-frame constant block.
    bool sceneConstants(const void** data, u32* bytes) const override;

    // Returns the lit pipeline for this frame, or 0 to let the backend use its own.
    rhi::PipelineHandle scenePipeline(bool meshShaders, bool wireframe) const override;

    // True while the debug view replaces the scene, including the backend's line draws.
    bool suppressesScene() const override;
    // Draws the debug raymarch over the already-bound colour target.
    void scenePass(rhi::IRenderContext& ctx) override;

    // Rebuilds the pipelines that bake the sample count and target formats, and resizes the
    // screen-resolution ray-traced shadow history (see rtShadowHist_).
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

    // The GPU residency of the material library, so the app can ask for a draw's binding set.
    pbr::MaterialSystem& materials() { return materials_; }

private:
    // Creates the cascaded shadow atlas.
    bool createShadowResources();
    // Creates the radiance volume, the injection accumulator and every binding set over them.
    bool createVoxelVolume(u32 resolution);
    // Creates every pipeline the feature runs.
    bool createPipelines();
    // Creates the subset that bakes sample count and render-target formats.
    bool createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
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
        u32 pad = 0;
    };
    // 64 + 4 + 4 + 12 + 4. A structured buffer packs tightly with natural alignment, so this is the
    // same 88 bytes on both sides -- and the stride handed to setSrvBuffer must agree with it or
    // every instance after the first reads the middle of its neighbour.
    static_assert(sizeof(RtInstance) == 88, "RtInstance is the HLSL RtInstance ABI");

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
    rhi::BufferHandle rtInstances_[kRtInstanceRing] = {};
    u32               rtInstanceSlot_ = 0;
    // What the table was built from. Rebuilt only when this changes, because concatenating every
    // mesh every frame would cost more than the reflections do.
    u64  rtGeometryKey_ = 0;
    bool rtGeometryReady_ = false;
    std::vector<RtInstance> rtInstanceData_;
    std::vector<rhi::MeshHandle> rtInstanceMesh_;   // parallel: which mesh each instance draws

    // Builds or refreshes the flat table for this frame's draw list. Returns false when it could
    // not be made, which is the signal to fall back to cone-traced reflections.
    bool buildGeometryTable(rhi::IRenderContext& ctx);
    bool rtLogged_ = false;

    // One replayed draw: its transform, its legacy colour parameters and its captured material.
    struct Draw {
        rhi::MeshHandle mesh;
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
    };
    std::vector<Draw> draws_, drawsPrev_;

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
        // The GI-only shadow map's light view-projection, fitted to the GI volume rather than the
        // camera -- see fitGiShadow(). Read only by PSVoxel through giShadowFactor(); PSMainVoxi
        // keeps using cascadeViewProj/shadowFactor above for the camera cascades.
        f32 giShadowViewProj[16] = {};
        // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable at all (0 falls back to
        // fully-lit indirect), z = normal-offset bias in world units, w unused.
        f32 giShadowParams[4] = {};
    } cb_;

    // THE MIRROR THIS FILE HAS ALWAYS HAD AND NEVER GUARDED. `cbuffer VoxiFrame : register(b4)` in
    // VoxiShaders.hpp repeats every field above by hand, and nothing checked that the two agreed --
    // the same unguarded-mirror bug already fixed for PathTracer's FrameCB and PcgVolume's VolumeCB.
    // VoxiFrame was simply the one that never got the assert. Appending here without appending there
    // reads garbage off the end of the block in every Voxi shader at once.
    static_assert(sizeof(FrameConstants) == 576,
                  "cbuffer VoxiFrame in VoxiShaders.hpp mirrors this byte for byte");
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
    rhi::SkyAtmosphere giSky_{};
    f32 giSnapCenter_[3] = {};
    f32 giSnapExtent_ = -1.0f;   // negative = no snapshot yet, so the first tick always rebuilds
    bool giSnapValid_ = false;
    u64 giSkipped_ = 0, giRebuilt_ = 0;   // for the one-time report; counts, not impressions
    bool giGateLogged_ = false;

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
    // no longer exists.
    bool ensureShadowHistory(u32 width, u32 height);
    // Whether PSMainVoxi will actually run its ray-traced-history code path this frame, for EITHER
    // effect. False while RT is inactive or the debug view has taken over the scene -- in either
    // case nothing will write rtShadowHist_ or rtReflHist_, so nothing about either should be
    // touched this frame.
    bool shadowHistoryActive() const {
        return rtActive_ && rtShadowHist_[0] && rtShadowHist_[1] &&
               rtReflHist_[0] && rtReflHist_[1] && !suppressesScene();
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
};

} // namespace aver::voxi
