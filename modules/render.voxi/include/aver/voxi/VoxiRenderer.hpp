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
    // Sets the directional light the shadow pass and the injection use.
    void setSun(const f32 dirToLight[3], const f32 color[3], f32 ambient);
    // Replaces the scene with a raymarch of the volume.
    void setDebugView(bool on);

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

    // True when the GI path initialised.
    bool giReady() const { return giReady_; }
    // True when an acceleration structure was built for this frame.
    bool rayTracingActive() const { return rtActive_; }

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
    // where the cache expires every frame and the structure is rebuilt. That rebuild is a full
    // PREFER_FAST_TRACE build rather than a refit, because the RHI has no update verb: correct, and
    // the most expensive build mode there is. It is the first thing to look at when ray tracing
    // plus several skinned characters stops being free.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blas_;
    bool dynamicBlasLogged_ = false;   // the per-frame rebuild is announced once, not every frame
    // Occlusion rays per pixel toward the sun's disc. FOUR by default: one gives the hard aliased
    // edge this replaced, and the cost is linear, so this is the knob to turn down first if ray
    // tracing ever starts costing frames. There is a recorded TDR history on this machine, so it
    // deliberately does not default high.
    u32 rtShadowRays_ = 4;
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
    f32 sunColor_[3] = {rhi::SkyAtmosphere{}.sunColor[0],
                        rhi::SkyAtmosphere{}.sunColor[1],
                        rhi::SkyAtmosphere{}.sunColor[2]};
    f32 ambient_     = rhi::SkyAtmosphere{}.skyLightIntensity;

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
};

} // namespace aver::voxi
