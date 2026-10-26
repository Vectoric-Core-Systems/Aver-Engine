// VoxiFroxelFog -- volumetric fog (light shafts) for Voxi: a camera-aligned froxel volume of sun light
// scattered by the air. docs/rendering/VOLUMETRIC_FOG.md.
//
// Owns the volumes, the two compute pipelines (scatter, integrate) and the sky draw. The scene shaders read the
// integrated volume through Voxi's main table 0 at u24 (voxi.hlsl, voxi_froxel.hlsli); the compute passes
// reuse that table for the TLAS, shadow map and air-visibility volume, so VoxiRenderer hands them its set.
//
// Frame order: ensure() (every frame, free while off) -> record() in prePass once the acceleration structure
// and shadow map are final -> the scene shaders read -> drawSky() from transparentPass. Not thread-safe.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/WorldShift.hpp"
#include "aver/voxi/Voxi.hpp"

namespace aver::voxi {

// The cbuffer both froxel shaders read at the feature-frame register. Mirrors `cbuffer FroxelCB` in
// voxi_froxel.hlsl byte for byte.
struct FroxelConstants {
    f32 cascadeViewProj[4][16];
    f32 cascadeSplit[4][4];
    f32 grid[4];          // xyz = volume dimensions, w = frame number
    f32 depth[4];         // near cm, far cm, ln(far / near), 1 / ln(far / near)
    f32 medium[4];        // height-fog share, uniform extinction per cm, sun intensity, Henyey-Greenstein g
    f32 temporal[4];      // x = history weight
    f32 prevViewProj[16];
    f32 shadow[4];        // sun visibility source (0 none, 1 cascades, 2 ray query), 1/atlas, cascades, ray bias
    f32 voxel[4];         // GI volume min corner, 1 / world size
    f32 voxelParams[4];   // x = air-visibility volume is real
};
static_assert(sizeof(FroxelConstants) == 496, "cbuffer FroxelCB in voxi_froxel.hlsl mirrors this byte for byte");
static_assert(sizeof(FroxelConstants) % 16 == 0, "must be a legal constant-buffer size");

class VoxiFroxelFog {
public:
    // Voxi main table 0's UAV slot the output volume is bound at; MUST match `register(u24)` in voxi.hlsl.
    static constexpr u32 kVolumeUavSlot = 24;
    // Table 1's registers follow table 0's: gFxHist register(t25), gFxCur register(u27) in voxi_froxel.hlsl.
    static constexpr u32 kHistSrvRegister = 25;
    static constexpr u32 kCurUavRegister  = 27;
    // Where the volume starts, cm. Nearer than this the air is taken as clear.
    static constexpr f32 kNearCm = 10.0f;

    // The Settings the passes read, clamped.
    struct Params {
        u32 gridX = 160, gridY = 90, gridZ = 64;
        f32 maxDistanceCm = 20000.0f;
        f32 density       = 0.00002f;
        f32 heightFog     = 1.0f;
        f32 intensity     = 1.0f;
        f32 anisotropy    = 0.6f;
        f32 temporal      = 0.85f;
    };
    static Params fromSettings(const Settings& s);

    // What the renderer knows about this frame.
    struct Frame {
        const f32* cascadeViewProj = nullptr;   // [4][16], Voxi's frame block
        const f32* cascadeSplit    = nullptr;   // [4][4]
        const f32* viewProj        = nullptr;   // this frame's camera, jitter-free; becomes next frame's history matrix
        f32 shadowAtlasTexel = 0.0f;
        u32 cascadeCount     = 0;
        u32 sunMode          = 0;               // 0 unshadowed, 1 cascade shadow map, 2 ray query
        rhi::TextureHandle shadowMap = 0;       // barriered for compute while sunMode == 1
        f32 rayBiasCm        = 2.0f;
        const f32* voxelOrigin = nullptr;       // [4]: GI volume min corner xyz, 1 / world size
        bool airVisUsable    = false;
        bool historyOk       = true;            // false after a camera cut
        u32  frameIndex      = 0;
    };

    // The floating origin moved (LARGE_WORLD 2.2.6): the history matrix is a SEPARATE copy of the camera, so it is composed
    // with the shift too, or the next frame reprojects the volume 16 km away and the fog flashes. f64, rounded once.
    void shiftWorld(const rhi::ShiftVec3d& delta) { rhi::composePrevViewProjInPlace(prevViewProj_, delta); }

    VoxiFroxelFog() = default;
    ~VoxiFroxelFog() { destroy(0); }
    VoxiFroxelFog(const VoxiFroxelFog&)            = delete;
    VoxiFroxelFog& operator=(const VoxiFroxelFog&) = delete;

    // The sky draw bakes the scene's sample count and formats; call before ensure() (cheap).
    void setTargets(u32 sampleCount, rhi::Format color, rhi::Format depth);

    // Brings resources in line with the setting: creates them (and compiles on first use) when `on`, frees them
    // when not. `layout` is Voxi's main layout with one SRV and one UAV declared in table 1 (t25/u25); `table0`
    // is Voxi's main binding set, whose u24 this keeps pointing at the volume (or a placeholder). Returns true
    // when record()/drawSky() can run this frame.
    bool ensure(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout, bool rtCapable, bool shaderOk,
                bool on, const Params& p, rhi::BindingSetHandle table0);

    // Both compute passes. The volume rests in UnorderedAccess afterwards, readable by the scene shaders.
    void record(rhi::IRenderContext& ctx, rhi::BindingSetHandle table0, const Frame& f);

    // Applies the volume to the sky (pixels at the far plane). Inside transparentPass: the scene colour and depth
    // targets are bound, depth test on, write off.
    void drawSky(rhi::IRenderContext& ctx);

    // Frees everything. Points u24 of `table0` (when non-zero) at a placeholder first.
    void destroy(rhi::BindingSetHandle table0);

    bool alive() const { return final_ != 0; }
    // Forgets last frame: the next record() uses no history.
    void resetHistory() { historyValid_ = false; }

    // The volume's depth mapping, as voxi.hlsl reads it (gShadowDraw.yz).
    f32 nearCm() const { return kNearCm; }
    f32 invLogRange() const { return invLogRange_; }
    f32 farCm() const { return farCm_; }
    u32 sunModeUsed() const { return sunModeUsed_; }
    // Whether the scatter pipeline carries the ray-query sun shadow (offer sunMode 2 only then).
    bool rayQueryBuilt() const { return rtBuilt_; }

private:
    bool buildPipelines(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout, bool rtCapable);
    bool buildSky(rhi::IResourceFactory& res);
    bool createVolumes(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout, u32 x, u32 y, u32 z,
                       rhi::BindingSetHandle table0);
    void destroyVolumes(rhi::BindingSetHandle table0);
    void destroyPipelines();

    rhi::IResourceFactory* res_ = nullptr;

    // The integrated volume (what scene shaders read) and the two scatter volumes ping-ponged for history.
    rhi::TextureHandle final_ = 0;
    rhi::TextureHandle tex_[2] = {};
    rhi::TextureHandle placeholder_ = 0;
    // Table 1 of the compute layout: set[p] reads tex_[p] (history, t25) and writes tex_[1 - p] (u25).
    rhi::BindingSetHandle sets_[2] = {};
    rhi::BindingSetHandle skySet_  = 0;
    u32 parity_ = 0;

    rhi::PipelineHandle scatterPso_ = 0, integratePso_ = 0, skyPso_ = 0;
    u64  builtRev_ = ~0ull;      // shader-file revision the pipelines were compiled from
    bool buildFailed_ = false;   // at builtRev_; retried when the shader files change
    bool rtBuilt_ = false;       // the scatter pipeline carries the ray-query path

    u32 sampleCount_ = 0;
    rhi::Format color_ = rhi::Format::Unknown, depth_ = rhi::Format::Unknown;
    u32 skySamples_ = 0;
    rhi::Format skyColor_ = rhi::Format::Unknown, skyDepth_ = rhi::Format::Unknown;

    u32 gx_ = 0, gy_ = 0, gz_ = 0;
    u32 failedX_ = 0, failedY_ = 0, failedZ_ = 0;   // a grid size whose volumes would not create
    Params params_{};
    f32 farCm_ = 0.0f, invLogRange_ = 0.0f;

    FroxelConstants cb_{};
    f32 prevViewProj_[16] = {};
    bool historyValid_ = false;
    u32  lastFrame_ = 0;
    u32  sunModeUsed_ = 0;
    bool unavailableLogged_ = false;
    bool sunModeLogged_[3] = {};
};

} // namespace aver::voxi
