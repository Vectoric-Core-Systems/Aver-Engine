// The particle render feature: draws what ParticleSystem simulates through rhi::IRenderFeature::transparentPass (the seam
// added for this; see RHIResources.hpp's comment on that hook and D3D12Device::endFrame for where it sits between the
// opaque draws and the deferred sky), and runs the GPU tier's compute in prePass. Expressed purely in generic RHI, the
// same discipline VoxiRenderer documents: this file may never include a backend header.
//
// ONE DRAW PATH. Sprites, ribbons and mesh particles are all drawn by one vertex shader family that reads
// StructuredBuffer<Particle> and an order list by SV_INSTANCE_ID. The CPU tier uploads its array (and a depth-sorted index
// list) into the same layout the GPU tier's pool buffers already have; the CPU-baked-vertex path of 0.7 is gone.
#pragma once
#include "aver/particles/ParticleGpu.hpp"
#include "aver/particles/ParticleLighting.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aver::particles {

// A small mesh for mesh particles, as plain arrays (positions and normals xyz, uvs xy, triangle indices).
struct ParticleMeshData {
    std::vector<f32> positions, normals, uvs;
    std::vector<u32> indices;
};
using ParticleMeshFn = bool (*)(u64 meshId, ParticleMeshData& out, void* user);

// The mesh vertex / index buffers, built once per mesh id.
class ParticleMeshCache {
public:
    static constexpr u32 kMaxTriangles = 2048;
    struct Entry {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 indexCount = 0;
        f32 diameter = 1.0f;   // bounding-box diagonal, cm; mesh particles scale by size / diameter
        bool failed = false;
    };
    void init(rhi::IResourceFactory* res) { res_ = res; }
    void shutdown();
    // The entry for `meshId`, built on first use through `fn`; null when there is no resolver or the mesh was refused (logged
    // once). Vertices are position(3) normal(3) uv(2) = 32 bytes.
    const Entry* get(u64 meshId, ParticleMeshFn fn, void* user);
private:
    rhi::IResourceFactory* res_ = nullptr;
    std::unordered_map<u64, Entry> entries_;
};

class ParticleRenderer final : public rhi::IRenderFeature {
public:
    // Creates the shaders, the quad template and the dummy resources, and (when the device allows it and `gpuEnabled`) the GPU
    // tier. False when the backend has no GPU resource factory to build against.
    bool init(rhi::IDevice& device);
    void shutdown();

    // The simulation this feature draws. Not owned: installed once by the composition root (normally &particleSystem()).
    // Installing it also installs the GPU tier into it when one exists.
    void setSystem(ParticleSystem* system);

    // Whether the GPU tier may be built at all (RENDER.PARTICLEGPU 0 turns it off). Call before init(); later calls tear the tier
    // down or build it.
    void setGpuEnabled(bool enabled);
    bool gpuAvailable() const { return gpu_.usable(); }
    ParticleGpu& gpu() { return gpu_; }

    // ---- hosts' resolvers ----
    // Maps ParticleEffect::textureId to a texture the host owns (null handle: draw the procedural dot).
    using TextureFn = rhi::TextureHandle (*)(u64 textureId, void* user);
    void setTextureResolver(TextureFn fn, void* user) { textureFn_ = fn; textureUser_ = user; }
    // Maps ParticleEffect::meshId to plain mesh arrays (limit 2,048 triangles).
    void setMeshResolver(ParticleMeshFn fn, void* user) { meshFn_ = fn; meshUser_ = user; }

    // ---- GI sampling seam ----
    //
    // A function pointer plus an opaque user pointer, the same shape as voxi::VoxiRenderer::setDepthProxy and
    // pbr::MaterialSystem::setTextureResolver. It is installed by whoever owns both this renderer and the GI system (today
    // Voxi; this header never spells the name). It is held possibly null: this module includes no voxi header and links no
    // voxi library, and with nothing installed here every particle renders without the GI term (lit emitters then use an
    // unshadowed sun and the sky).
    //
    // Why two functions, not one: sampling a GPU-resident volume from a pixel shader needs two different things at two
    // different moments: HLSL source text and register placement, needed once when a pipeline is (re)built; and live GPU
    // resources plus this frame's constants, needed before any GI draw. voxi::giShaderPrelude()/giShaderDefines() already exist
    // for exactly the first and voxi::VoxiRenderer::bindGiResources()/giFrameConstants() for the second. The seam below is a
    // thin function-pointer wrapper around those four, written entirely in a composition root that already includes voxi
    // headers -- never in here.
    struct GiSeam {
        void* user = nullptr;

        // PIPELINE-BUILD TIME. `srvBase` / `samplerBase` / `cbRegister` are where THIS renderer's root signature has free
        // slots for exactly 2 SRVs, 2 samplers and one root-CBV register. On success fills `outPrelude` with HLSL to compile
        // ahead of the particle shader and `outDefines` with a ";"-separated -D list, and returns true; false means "nothing
        // available right now". THE NAME CONTRACT: the prelude declares
        // `float3 coneTracedIndirect(float3 worldPos, float3 N, out float ao)` and `float shadowFactor(float3 worldPos, float3 N,
        // float ndl)` (sun visibility, 1 = lit) -- see particles.hlsl's AVER_PARTICLES_GI block.
        using PrepareFn = bool (*)(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                    std::string* outPrelude, std::string* outDefines, void* user);
        // PER GI DRAW. Refreshes `set`'s two SRV slots (srvBase, srvBase+1) and returns this frame's frame-constant bytes,
        // bound verbatim at cbRegister before the draw. The pointer is only guaranteed valid for the remainder of this frame.
        using BindFn = void (*)(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                                 const void** outCbData, u32* outCbBytes, void* user);
        PrepareFn prepare = nullptr;
        BindFn    bind    = nullptr;
    };
    // Installs (or, with a default-constructed GiSeam, clears) the seam. Safe before or after init(): the GI pixel shader is
    // (re)built at once so a seam installed after startup takes effect on the next frame.
    void setGiSeam(const GiSeam& seam);

    const char* name() const override { return "Particles"; }

    // Runs the GPU tier's compute for this frame's jobs (before the scene render scope; legal on Vulkan).
    void prePass(rhi::IRenderContext& ctx) override;

    // Draws every tracked emitter into the scene colour target, depth-tested against it, without writing depth.
    void transparentPass(rhi::IRenderContext& ctx) override;

    // Rebuilds the pipelines to bake the scene target's sample count and formats. MSAA is baked into
    // GraphicsPipelineDesc::sampleCount at creation, and the engine's default sample count is not 1.
    void onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth, u32 width, u32 height) override;

    // The camera a draw needs, for callers with their own pass (the editor preview): row-major view-projection, the
    // camera-relative inverse (rhi::PerFrameCB::invViewProjRel) and the eye.
    struct Camera {
        f32 viewProj[16] = {};
        f32 invViewProjRel[16] = {};
        f32 pos[3] = {};
    };
    // Draws `views` back to front (by bounds centre) in the CURRENT render pass, which must already have a viewport, scissor and
    // colour + depth targets bound. CPU-tier views only unless a GPU draw buffer exists for the handle.
    void drawViews(rhi::IRenderContext& ctx, const std::vector<InstanceView>& views, const Camera& cam,
                   const std::vector<scene::WorldLight>* lights = nullptr);

    // Counts of draws and uploaded particles in the last drawViews (for stats and tests).
    struct DrawStats { u32 draws = 0, cpuParticles = 0, gpuDraws = 0, culled = 0; };
    const DrawStats& lastDrawStats() const { return drawStats_; }

    // The pure helpers, exposed for tests and the preview.
    static bool sphereInFrustum(const f32 viewProj[16], const Vec3& centre, f32 radius);

private:
    bool buildShaders();
    bool buildPipelines(u32 sampleCount, rhi::Format color, rhi::Format depth);
    void destroyPipelines();
    bool buildGiShader();
    rhi::PipelineHandle pipelineFor(RenderKind kind, bool additive, bool wantGi);
    rhi::PipelineHandle makePipeline(RenderKind kind, bool additive, bool gi);
    rhi::BindingSetHandle acquireSet();
    bool ensureRings(usize particles, usize orders, usize trails);
    void installGpuTier();

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    ParticleSystem* system_ = nullptr;
    ParticleGpu gpu_;
    bool gpuEnabled_ = true;

    rhi::ShaderHandle vs_[3] = {};      // sprite, ribbon, mesh
    rhi::ShaderHandle ps_ = 0, giPs_ = 0;
    std::unordered_map<u32, rhi::PipelineHandle> pipelines_;
    GiSeam giSeam_{};
    u32 pipelineSampleCount_ = 1;
    rhi::Format pipelineColor_ = rhi::Format::Unknown, pipelineDepth_ = rhi::Format::Unknown;

    // The unit quad every sprite and ribbon instance expands (corner in -1..1).
    rhi::BufferHandle quadVb_ = 0, quadIb_ = 0;
    // Valid descriptors for every slot a draw does not use.
    rhi::BufferHandle dummyBuf_ = 0;
    rhi::TextureHandle whiteTex_ = 0, dummyTex3d_ = 0, dummyTex2d_ = 0;

    ParticleMeshCache meshes_;
    TextureFn textureFn_ = nullptr;
    void* textureUser_ = nullptr;
    ParticleMeshFn meshFn_ = nullptr;
    void* meshUser_ = nullptr;

    // Per-frame-in-flight upload rings (writeBuffer is immediate and unsynchronised, so one buffer reused while the GPU may still
    // read last frame's draw from it is a race, not a shortcut) and the binding sets that point into them.
    static constexpr u32 kFramesInFlight = 3;
    rhi::BufferHandle particleRing_[kFramesInFlight] = {}, orderRing_[kFramesInFlight] = {}, trailRing_[kFramesInFlight] = {};
    usize particleCap_ = 0, orderCap_ = 0, trailCap_ = 0;   // elements
    std::vector<rhi::BindingSetHandle> sets_[kFramesInFlight];
    u32 setCursor_ = 0;
    u32 frame_ = 0;

    std::vector<InstanceView> views_;
    std::vector<scene::WorldLight> lightScratch_;
    std::vector<u32> orderScratch_;
    std::vector<f32> depthScratch_;
    DrawStats drawStats_;
};

} // namespace aver::particles
