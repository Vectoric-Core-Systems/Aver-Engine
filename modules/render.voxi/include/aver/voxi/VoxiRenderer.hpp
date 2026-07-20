#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/voxi/Voxi.hpp"

#include <vector>

// Voxi — the render feature: voxel cone traced global illumination, a directional shadow map for
// light injection, and DXR 1.1 inline RayQuery sun shadows.
//
// Everything GPU-side that Voxi needs lives here, expressed purely in terms of the generic RHI.
// The backend (Aver.RHI.D3D12) knows nothing about voxels, cones, radiance or acceleration
// structures; it provides textures, buffers, pipelines and a command context, and calls prePass()
// once per frame at a defined point.
//
// NOTE ON THE TWO VOXI TARGETS:
//   Aver.Render.Voxi          — SHARED DLL, Core-only. Settings + the C ABI that C# P/Invokes.
//                               Must NOT gain an RHI dependency or the scripting boundary breaks.
//   Aver.Render.Voxi.Renderer — static lib, this file. Depends on Aver.Core + Aver.RHI (the
//                               generic interface, NOT Aver.RHI.D3D12).
namespace aver::voxi {

class VoxiRenderer final : public rhi::IRenderFeature {
public:
    // Takes the generic device interface — no D3D12 types cross this boundary. Returns false if
    // the device cannot support even the baseline (no compute), leaving the engine to run without
    // the feature rather than failing.
    bool init(rhi::IDevice& device);
    void shutdown();

    // ---- configuration (mirrors the settings module; see Voxi.hpp) ----
    void setSettings(const Settings& s);
    void setVolume(const f32 center[3], f32 extent);
    void setSun(const f32 dirToLight[3], const f32 color[3], f32 ambient);
    void setDebugView(bool on);      // raymarch the volume to screen instead of shading

    // ---- per-frame scene submission ----
    // The engine replays the scene here. Voxi consumes it for the shadow map, the voxel volume and
    // the acceleration structure; it deliberately runs a frame behind so the app's submission
    // order does not have to change.
    void beginScene() override;
    void submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                f32 metallic, f32 roughness);
    // The hook the backend actually calls is submitDraw; submit() is the name this module uses.
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness) override {
        submit(mesh, world, baseColor, metallic, roughness);
    }

    // ---- rhi::IRenderFeature ----
    const char* name() const override { return "Voxi"; }
    // Shadow map -> acceleration structure -> clear volume -> voxelise+inject -> filter mips.
    // Runs before the scene's targets are bound, because every one of those passes owns its own
    // render targets and viewport.
    void prePass(rhi::IRenderContext& ctx) override;
    // True when GI or ray tracing is on: the scene must then be drawn with Voxi's pipeline
    // variants, because the cone trace and RayQuery live inside the lit pixel shader.
    bool overridesScenePipeline() const override;
    // Voxi's binding set is handed out unconditionally, even while the backend still owns the scene
    // pipelines: t0/t1/t2/u0 are Voxi's descriptors now, and Tier 1 requires every declared table to
    // be bound on every pass.
    rhi::BindingSetHandle sceneBindingSet() const override { return bindings_; }
    bool sceneConstants(const void** data, u32* bytes) const override;

    // The pipeline the scene should use this frame. Valid only when overridesScenePipeline().
    // `wireframe` is passed because there is no mesh-shader wireframe variant, so it beats
    // meshShaders. Returning 0 declines a combination and the backend uses its own pipeline.
    // The signature must match the base exactly: a near-miss HIDES rather than overrides, and the
    // backend would then call the base's `return 0` and silently fall back with no diagnostic.
    rhi::PipelineHandle scenePipeline(bool meshShaders, bool wireframe) const override;

    // Status for the settings UI / C# bindings, derived from the device caps captured at init.
    bool giReady() const { return giReady_; }
    bool rayTracingActive() const { return rtActive_; }

private:
    bool createShadowResources();
    bool createVoxelVolume(u32 resolution);
    bool createPipelines();
    void shadowPass(rhi::IRenderContext& ctx);
    void buildAccelerationStructures(rhi::IRenderContext& ctx);
    void voxelizePass(rhi::IRenderContext& ctx);
    void filterMips(rhi::IRenderContext& ctx);

    rhi::IDevice* dev_ = nullptr;
    // Cached at init: a null factory is how a backend without GPU support declines the feature, so
    // the check happens once rather than at every call site.
    rhi::IResourceFactory* res_ = nullptr;
    rhi::DeviceCaps caps_{};
    Settings settings_{};

    // Shadow map: core FL 11_0, so shadowed injection works on every DX12 GPU — which is why it
    // exists alongside the ray-traced path rather than being replaced by it.
    rhi::TextureHandle shadowTex_ = 0;
    rhi::PipelineHandle shadowPso_ = 0;

    // Radiance volume: RGBA16F Tex3D with a full mip chain. Mip N is the cone footprint at
    // distance N.
    rhi::TextureHandle  voxelTex_ = 0;
    // Injection accumulator: R32_UINT Tex3D, (res*4)^1 x res x res, channels interleaved along x.
    // Exists so injection can sum atomically instead of racing to store, which is what makes the
    // volume identical from frame to frame. Never mipped, never read as an SRV, and it stays in
    // UnorderedAccess for its whole life -- no barrier sequence to get wrong.
    rhi::TextureHandle  voxelAccumTex_ = 0;
    rhi::PipelineHandle voxelPso_ = 0, voxelMsPso_ = 0, mipPso_ = 0, clearPso_ = 0, debugPso_ = 0;
    rhi::PipelineHandle resolvePso_ = 0;
    // The lit/voxelise table: t0 volume (whole chain), t1 shadow, t2 TLAS, u0 volume mip 0,
    // u1 injection accumulator.
    rhi::BindingSetHandle bindings_ = 0;
    // Deliberately its OWN set, declaring the UAVs alone: while the clear runs, every mip of the
    // volume sits in UnorderedAccess, and an SRV descriptor over it would be a live view of a
    // resource in the wrong state.
    rhi::BindingSetHandle clearBindings_ = 0;
    // The accumulator-to-volume reduction. Same shape as the clear set, and separate for the same
    // reason: it runs while the volume is in UnorderedAccess.
    rhi::BindingSetHandle resolveBindings_ = 0;
    // One per mip filter step: set m reads mip m-1 and writes mip m. A single-mip SRV is what makes
    // reading and writing the same resource in one dispatch legal.
    std::vector<rhi::BindingSetHandle> mipBindings_;

    // Scene lit-pass variants Voxi owns (see overridesScenePipeline).
    rhi::PipelineHandle scenePso_ = 0, sceneMsPso_ = 0, sceneRtPso_ = 0, sceneMsRtPso_ = 0;

    rhi::TlasHandle tlas_ = 0;

    struct Draw {
        rhi::MeshHandle mesh;
        f32 world[16];
        f32 color[4];
        f32 metallic, roughness;
    };
    std::vector<Draw> draws_, drawsPrev_;

    f32 center_[3] = {0, 0, 0};
    f32 extent_ = 2000.0f;

    // Mirrors `cbuffer VoxiFrame : register(b4)` field for field. A mismatch is silent and shows as
    // misplaced GI or a uniformly lit scene, never as an error.
    struct FrameConstants {
        f32 voxelOrigin[4] = {};
        f32 voxelParams[4] = {};
        f32 lightViewProj[16] = {};
        f32 shadowParams[4] = {};
    } cb_;
    bool giEnabled() const { return settings_.globalIllumination != Quality::Off; }
    f32 sunDir_[3] = {0, 0, 1};
    f32 sunColor_[3] = {1, 1, 1};
    f32 ambient_ = 0.2f;

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
};

} // namespace aver::voxi
