// Particles the render feature: draws what ParticleSystem simulates, through
// rhi::IRenderFeature::transparentPass (the seam added for exactly this -- see RHIResources.hpp's
// own comment on that hook and D3D12Device::endFrame for where it sits between the opaque draws and
// the deferred sky). Expressed purely in generic RHI, the same discipline VoxiRenderer documents:
// this file may never include a backend header.
#pragma once
#include "aver/particles/ParticleSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <vector>

namespace aver::particles {

// One camera-facing quad's four corners, already positioned in world space (see
// ParticleRenderer.cpp's own comment on why the CPU bakes the billboard rather than the shader).
struct ParticleVertex {
    f32 pos[3];    // POSITION0 (xy) + POSITION1 (z) -- see ParticleShaders.hpp
    f32 uv[2];     // TEXCOORD0
    u32 color;     // COLOR0, RGBA8Unorm, PREMULTIPLIED -- see packPremultiplied() in the .cpp
};
static_assert(sizeof(ParticleVertex) == 24, "the vertex layout in the .cpp names these byte offsets");

// Draws every emitter ParticleSystem is tracking, one drawIndexed call each, sorted back-to-front
// WITHIN that call. UNLIT BY DEFAULT -- see ParticleShaders.hpp's own comment -- and stays exactly
// that way unless a GI seam is installed (setGiSeam) AND the drawn effect asks for it
// (ParticleEffect::receivesGI); either half missing and every particle draws at its own authored
// colour, precisely as it did before this seam existed.
class ParticleRenderer final : public rhi::IRenderFeature {
public:
    // Creates the shaders and an initial best-effort pipeline pair (rebuilt for real the moment
    // onRenderTargetsChanged reports the actual scene target -- see VoxiRenderer::init for the same
    // two-step pattern). False when the backend has no GPU resource factory to build against.
    bool init(rhi::IDevice& device);
    void shutdown();

    // The simulation this feature draws. Not owned: installed once by the composition root (normally
    // &particleSystem(), the process global), same idiom as VoxiRenderer::setDepthProxy's function
    // pointer -- this renderer never constructs or ticks a ParticleSystem itself.
    void setSystem(ParticleSystem* system) { system_ = system; }

    // ---- DECIDED 4: the GI sampling seam ----
    //
    // A function pointer plus an opaque user pointer, THE SAME SHAPE as
    // voxi::VoxiRenderer::setDepthProxy and pbr::MaterialSystem::setTextureResolver -- installed by
    // whoever owns both this renderer and whatever GI system exists (today: Voxi; this header never
    // spells the name). Held POSSIBLY-NULL: this module includes no voxi header and links no voxi
    // library, and with nothing installed here (AVER_MODULE_VOXI=OFF, or Voxi simply never wired up
    // this run) every particle renders exactly as it did before this struct existed -- see
    // pipelineFor()'s own comment for how that fallback is enforced, not merely hoped for.
    //
    // WHY TWO FUNCTIONS, NOT ONE. Sampling a GPU-resident volume from a pixel shader needs two
    // different things at two different moments: HLSL source text and register placement, needed
    // once when a pipeline is (re)built; and live GPU resources plus this frame's constants, needed
    // once per frame before any GI draw. voxi::giShaderPrelude()/giShaderDefines() already exist for
    // exactly the first (see VoxiGiShaders.hpp's own comment: "the modular seam: letting a FOREIGN
    // pipeline merge Voxi's table 0 into its own" -- built for precisely this kind of caller, its one
    // other user today being sandbox/src/ClusterMaterialShader.hpp), and
    // voxi::VoxiRenderer::bindGiResources()/giFrameConstants() already exist for the second. The seam
    // below is a thin function-pointer wrapper around those four, written entirely in a composition
    // root that already includes voxi headers -- never in here.
    struct GiSeam {
        void* user = nullptr;

        // PIPELINE-BUILD TIME. Called from buildPipelines() only -- never per-frame, never
        // per-particle. `srvBase`/`samplerBase`/`cbRegister` are where THIS renderer's OWN root
        // signature has free slots for exactly 2 SRVs, 2 samplers and one root-CBV register (the same
        // 2/2 shape giShaderDefines() itself always asks for -- see its own comment on why the second
        // of each is reserved even though the particle shader below never reads it). On success,
        // fills `outPrelude` with HLSL source to compile ahead of the particle module's own shader
        // text and `outDefines` with a ";"-separated -D list for it, and returns true. Returns false
        // for "nothing available right now" (Voxi not linked, or its own init() has not succeeded
        // this run) -- ParticleRenderer then builds only its ordinary, unlit pipelines, exactly as if
        // no seam were installed at all.
        //
        // THE NAME CONTRACT, stated because it is the one piece of coupling that survives crossing
        // this seam: the returned prelude must declare
        // `float3 coneTracedIndirect(float3 worldPos, float3 N, out float ao)` -- see
        // ParticleShaders.hpp's AVER_PARTICLES_GI block for the one call site. This is an HLSL text
        // contract, agreed the same way giShaderDefines()'s own callers already agree to it; nothing
        // in this C++ header, nor the .cpp, nor CMakeLists.txt ever names voxi.
        using PrepareFn = bool (*)(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                    std::string* outPrelude, std::string* outDefines, void* user);
        // PER-FRAME. Called at most once per transparentPass() call, and only when at least one
        // emitter drawn this frame wants GI (ParticleEffect::receivesGI, with a GI pipeline actually
        // built) -- never per-particle, never per-emitter. Refreshes `set`'s two SRV slots
        // (srvBase, srvBase+1) to point at whatever this frame's GI resources actually are -- a
        // volume or shadow map can be resized or rebuilt on any frame -- and writes this frame's
        // frame-constant bytes to `*outCbData`/`*outCbBytes`, bound verbatim at `cbRegister` before
        // every GI draw. The pointer is only guaranteed valid for the remainder of this frame.
        using BindFn = void (*)(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                                 const void** outCbData, u32* outCbBytes, void* user);
        PrepareFn prepare = nullptr;
        BindFn    bind    = nullptr;
    };
    // Installs (or, with a default-constructed GiSeam, clears) the seam. Safe to call before OR after
    // init(): if this renderer already has a device, the GI pipelines are (re)built immediately so a
    // seam installed after startup (the normal order -- Voxi attaches, then Particles, in both
    // composition roots) takes effect on the very next transparentPass rather than waiting for the
    // next onRenderTargetsChanged.
    void setGiSeam(const GiSeam& seam);

    const char* name() const override { return "Particles"; }

    // Draws every tracked emitter into the scene colour target, depth-tested against it, without
    // writing depth -- see IRenderFeature::transparentPass's own comment for the whole contract.
    void transparentPass(rhi::IRenderContext& ctx) override;

    // Rebuilds the pipelines (PremultipliedAlpha, Additive, and their GI-sampling twins when a seam
    // is installed) to bake the scene target's current sample count and formats -- MSAA is baked into
    // GraphicsPipelineDesc::sampleCount at creation, and the engine's default sample count is not 1
    // (DECIDED 1).
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                u32 width, u32 height) override;

private:
    bool buildPipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
    // Which pipeline a draw's blend mode and receivesGI flag select -- see ParticleTypes.hpp's own
    // comment on ParticleEffect::blend for why only two of rhi::BlendMode's four values are actually
    // distinct here, and GiSeam's own comment for when the GI variant exists at all. `wantsGi` is
    // ALREADY the effect's receivesGI narrowed by whether a GI pipeline was actually built this run
    // -- this method does no further gating.
    rhi::PipelineHandle pipelineFor(rhi::BlendMode blend, bool wantsGi) const;
    bool ensureCapacity(usize vertexCount, usize indexCount);
    // The two SRV slots (volume, shadow) and two sampler slots (matching) a GI pipeline's table 0
    // reserves. 0-based: this renderer declares no other table-0 slot, unlike the mesh-shader cluster
    // path this mirrors (sandbox/src/SandboxApp.cpp's kClusterGiSrvBase et al.), which had a table 0
    // of its own to share Voxi's registers with.
    static constexpr u32 kGiSrvBase = 0, kGiSamplerBase = 0;

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    ParticleSystem* system_ = nullptr;

    rhi::ShaderHandle vs_ = 0, ps_ = 0;
    rhi::PipelineHandle premultPso_ = 0, additivePso_ = 0;
    // The GI-sampling twins of the two above -- built only while giSeam_.prepare succeeds (see
    // buildPipelines()). Share vs_ (the vertex shader never samples GI) but compile their own pixel
    // shader, giPs_, against giSeam_'s returned prelude/defines.
    rhi::ShaderHandle giPs_ = 0;
    rhi::PipelineHandle giPremultPso_ = 0, giAdditivePso_ = 0;
    // Table 0 for the two GI pipelines: kGiSrvBase (volume, Texture3D), kGiSrvBase+1 (shadow map,
    // Texture2D) -- refreshed once per frame by giSeam_.bind(), never touched by the non-GI pipelines.
    rhi::BindingSetHandle giBindingSet_ = 0;
    GiSeam giSeam_{};
    u32 pipelineSampleCount_ = 1;
    rhi::Format pipelineColor_ = rhi::Format::Unknown, pipelineDepth_ = rhi::Format::Unknown;

    // A per-frame-in-flight ring, matching UiRenderer's own kFramesInFlight and its own reasoning:
    // writeBuffer is immediate and unsynchronised, so reusing one buffer while the GPU may still be
    // reading last frame's draw from it is a race, not a shortcut.
    static constexpr u32 kFramesInFlight = 3;
    rhi::BufferHandle vb_[kFramesInFlight] = {};
    rhi::BufferHandle ib_[kFramesInFlight] = {};
    usize vbCapacity_ = 0, ibCapacity_ = 0;
    u32 frame_ = 0;

    // Rebuilt fresh every transparentPass call from ParticleSystem's current state -- scratch, not
    // persisted state, so their capacity is what actually survives frame to frame.
    std::vector<ParticleVertex> verts_;
    std::vector<u32> idx_;
    struct DrawCmd { u32 indexOffset, indexCount; rhi::PipelineHandle pipeline; };
    std::vector<DrawCmd> draws_;
    std::vector<u32> sortScratch_;
};

} // namespace aver::particles
