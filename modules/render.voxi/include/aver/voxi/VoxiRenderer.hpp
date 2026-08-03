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

    // Turns on the frame-period report. See rtShadowRays_ for what it is for and what it is not.
    void setFrameTimeReport(bool on) { frameTimeReport_ = on; }

    // The largest ray count accepted. Not a hardware limit: there is a recorded TDR history on this
    // machine, and 64 rays per pixel at 8x MSAA is how a knob turns into a device removal.
    static constexpr u32 kMaxShadowRays = 32;

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

    // Rebuilds the pipelines that bake the sample count and target formats.
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth) override;

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
    // THAT PREDICATE IS CURRENTLY TRUE OF EVERY MESH, which is measured and not suspected: the
    // editor's placeholder scene has no skinned geometry at all and still logs
    // `mesh 1 has compute-written vertices`, then two rebuilds a frame forever. meshVertexBuffer is
    // contracted to be "non-zero when a mesh's vertices are WRITTEN BY COMPUTE ... zero for every
    // ordinary mesh", and the D3D12 backend returns GpuMesh::vbBuffer, which createMesh has filled
    // in for every mesh since a3022e0 gave a shader the ability to read a mesh's geometry. The
    // backend needs a flag set only by createSkinTargetMesh; this side cannot tell the difference
    // and must not guess. Priced at 0.071 ms/frame for this scene's two 14-vertex meshes -- almost
    // all of it fixed per-build overhead, so it grows with mesh COUNT before it grows with triangles.
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

    rhi::BufferHandle rtVerts_ = 0, rtIndices_ = 0, rtInstances_ = 0;
    u32  rtVertCapacity_ = 0, rtIndexCapacity_ = 0, rtInstanceCapacity_ = 0;
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
    };
    std::vector<Draw> draws_, drawsPrev_;

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
        // x = tan of the sun's angular radius, y = occlusion rays per pixel, z = base ray bias.
        f32 rtParams[4] = {};
    } cb_;

    // Builds this frame's cascade matrices and splits. Returns the usable cascade count, 0 if none.
    u32 fitCascades();
    bool giEnabled() const { return settings_.globalIllumination != Quality::Off; }
    f32 sunDir_[3]   = {rhi::SkyAtmosphere{}.sunDirection[0],
                        rhi::SkyAtmosphere{}.sunDirection[1],
                        rhi::SkyAtmosphere{}.sunDirection[2]};
    // No sunColor_ and no ambient_ here on purpose. See setSunDirection.

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
};

} // namespace aver::voxi
