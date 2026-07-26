#pragma once
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/voxi/Voxi.hpp"

#include <unordered_map>
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
                f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                const void* drawConstants, u32 drawConstantBytes);
    // The hook the backend actually calls is submitDraw; submit() is the name this module uses.
    void submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                    f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                    const void* drawConstants, u32 drawConstantBytes) override {
        submit(mesh, world, baseColor, metallic, roughness, drawBinding, drawConstants, drawConstantBytes);
    }

    // ---- rhi::IRenderFeature ----
    const char* name() const override { return "Voxi"; }
    // Acceleration structures -> shadow map -> clear volume -> voxelise+inject -> filter mips.
    // Runs before the scene's targets are bound, because every one of those passes owns its own
    // render targets and viewport.
    void prePass(rhi::IRenderContext& ctx) override;
    // True whenever the feature initialised. Voxi owns the lit pixel shader outright now: the sun
    // shadow (map or ray) and the cone-traced bounce both live INSIDE it, so a scene drawn with the
    // backend's own pipeline is unshadowed with no GI. That is the clean cut — shading a feature
    // contributes cannot be expressed as an extra pass.
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

    // The debug view replaces the scene entirely with a raymarch of the volume. Suppression has to
    // cover the backend's LINE draws as well, or the editor grid and gizmos float over the raymarch
    // — visible in a screenshot and completely invisible to the probe.
    bool suppressesScene() const override;
    void scenePass(rhi::IRenderContext& ctx) override;

    // Only the pipelines that BAKE the sample count and target formats are rebuilt: the four scene
    // variants and the debug view. The shadow, voxelise, clear, resolve and mip pipelines are pinned
    // to sampleCount 1 and target formats they own, so rebuilding them would be pure churn.
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth) override;

    // The GPU residency of the material library, so the app can ask for the binding set and the
    // constant block a draw needs. Exposed rather than duplicated: a second MaterialSystem would
    // build a second set of fallback textures and a second binding set per material, and the two
    // would disagree the moment either drained the dirty list first.
    //
    // This is NOT a way in to Voxi's own passes. It is the same object they use, and the register
    // its table is based at is this module's pipeline layout, which is exactly why it lives here.
    pbr::MaterialSystem& materials() { return materials_; }

    // Status for the settings UI / C# bindings, derived from the device caps captured at init.
    bool giReady() const { return giReady_; }
    bool rayTracingActive() const { return rtActive_; }

private:
    bool createShadowResources();
    bool createVoxelVolume(u32 resolution);
    bool createPipelines();
    // The subset that bakes sample count and render-target formats. Called from createPipelines()
    // and again from onRenderTargetsChanged(); a missing rebuild is a draw-time PSO/RTV
    // incompatibility, not a creation-time error, so it surfaces a long way from its cause.
    bool createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
    void shadowPass(rhi::IRenderContext& ctx);
    void buildAccelerationStructures(rhi::IRenderContext& ctx);
    void voxelizePass(rhi::IRenderContext& ctx);
    void filterMips(rhi::IRenderContext& ctx);

    // The GPU residency of the material library. It lives with the RENDERER because the register
    // its table is based at is a property of this module's pipeline layout, and because Voxi is the
    // thing that renders materials. The materials themselves are the library's, process-wide.
    pbr::MaterialSystem materials_;

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

    // Top-level structure, rebuilt every frame from the replayed draw list because instance
    // transforms are not static. Sized once for the draw-list cap so a rebuild never reallocates.
    rhi::TlasHandle tlas_ = 0;
    // One bottom-level structure per referenced mesh, created and built the first frame that mesh
    // appears and then kept for the run — meshes are static, so a built BLAS never goes stale.
    // Keyed by MeshHandle because the draw list names geometry that way and one mesh is usually
    // drawn many times per frame.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blas_;
    bool rtLogged_ = false;   // "RayQuery active" is worth saying once, not sixty times a second

    struct Draw {
        rhi::MeshHandle mesh;
        f32 world[16];
        f32 color[4];
        f32 metallic, roughness;
        // The authored material captured at submit time, so the voxelisation pass injects the GI
        // bounce from the SAME surface the lit pass shades rather than from the identity fallback --
        // the whole of what "Voxi consumes the material in the GI path" means. The set is the
        // material system's and outlives the one-frame replay delay; the b2 block is COPIED because
        // submitDraw hands over a borrowed pointer good only for that call. A zero set (only before a
        // default binding is registered) falls back.
        rhi::BindingSetHandle matSet = 0;
        u32 matBytes = 0;
        u8  mat[sizeof(pbr::MaterialConstants)] = {};
    };
    std::vector<Draw> draws_, drawsPrev_;

    f32 center_[3] = {0, 0, 0};
    f32 extent_ = 2000.0f;

    // Mirrors `cbuffer VoxiFrame : register(b4)` field for field. A mismatch is silent and shows as
    // misplaced GI or a uniformly lit scene, never as an error.
    struct FrameConstants {
        f32 voxelOrigin[4] = {};
        f32 voxelParams[4] = {};
        // One light view-projection per CASCADE, tightest first. Each maps world space to that
        // cascade's own [-1,1] clip box; the shader remaps into the atlas quadrant itself, because
        // baking the quadrant into the matrix would make a matrix that could not be reused for the
        // depth-only render pass that fills it.
        f32 cascadeViewProj[4][16] = {};
        // x = the RADIUS in world units at which each cascade stops being used. Radial, not planar,
        // because the cascades are fitted to bounding SPHERES -- see fitCascades for why.
        // y = normal-offset bias in world units for that cascade, scaled by its own texel size.
        // zw = unused.
        f32 cascadeSplit[4][4] = {};
        // x = 1/atlas size, y = shadow map usable, z = acceleration structure built this frame,
        // w = live cascade count
        f32 shadowParams[4] = {};
        // x = which cascade the depth-only shadow pass is currently filling. A field of its own
        // rather than a spare lane of shadowParams: that block means something to every other pass
        // in the module, and a value that is a cascade index for one pass and an acceleration-
        // structure flag for the rest is the kind of overload that survives review and then breaks
        // the day the passes are reordered.
        f32 shadowDraw[4] = {};
    } cb_;

    // Build the cascade matrices and splits for this frame's camera. Returns the number of usable
    // cascades, 0 when there is no camera to fit to.
    u32 fitCascades();
    bool giEnabled() const { return settings_.globalIllumination != Quality::Off; }
    // FROM the engine's default sky, not a restatement of it. sunDir_ was {0, 0, 1} -- a sun
    // straight overhead -- so on any frame before setSun() ran, the cascades and the GI injection
    // used a direction nothing else in the engine agreed with. It also made the degenerate-input
    // guard in fitCascades() unreachable: getSafeNormal({0,0,1}) has sizeSquared 1, so that
    // fallback could only ever fire for an exactly-zero vector, which is not what it was guarding.
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
