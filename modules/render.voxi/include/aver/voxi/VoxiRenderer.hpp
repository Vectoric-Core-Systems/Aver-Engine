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
    void beginScene();
    void submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                f32 metallic, f32 roughness);

    // ---- rhi::IRenderFeature ----
    const char* name() const override { return "Voxi"; }
    // Shadow map -> acceleration structure -> clear volume -> voxelise+inject -> filter mips.
    // Runs before the scene's targets are bound, because every one of those passes owns its own
    // render targets and viewport.
    void prePass(rhi::IRenderContext& ctx) override;
    // True when GI or ray tracing is on: the scene must then be drawn with Voxi's pipeline
    // variants, because the cone trace and RayQuery live inside the lit pixel shader.
    bool overridesScenePipeline() const override;

    // The pipeline the scene should use this frame. Valid only when overridesScenePipeline().
    // `meshShaders` selects the mesh-shader geometry variant.
    rhi::PipelineHandle scenePipeline(bool meshShaders) const;

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
    rhi::DeviceCaps caps_{};
    Settings settings_{};

    // Shadow map: core FL 11_0, so shadowed injection works on every DX12 GPU — which is why it
    // exists alongside the ray-traced path rather than being replaced by it.
    rhi::TextureHandle shadowTex_ = 0;
    rhi::PipelineHandle shadowPso_ = 0;

    // Radiance volume: RGBA16F Tex3D with a full mip chain. Mip N is the cone footprint at
    // distance N.
    rhi::TextureHandle  voxelTex_ = 0;
    rhi::PipelineHandle voxelPso_ = 0, voxelMsPso_ = 0, mipPso_ = 0, clearPso_ = 0, debugPso_ = 0;
    rhi::BindingSetHandle bindings_ = 0;

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
    f32 sunDir_[3] = {0, 0, 1};
    f32 sunColor_[3] = {1, 1, 1};
    f32 ambient_ = 0.2f;

    u32  voxelMips_ = 0, voxelResBuilt_ = 0;
    bool giReady_ = false, rtSupported_ = false, rtActive_ = false, debugView_ = false;
};

} // namespace aver::voxi
