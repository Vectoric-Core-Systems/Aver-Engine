// See FluidScene.hpp for the whole design. What follows is spawn/despawn/update/prePass, the private
// retire() they share, and the one small packing helper both spawn() and update() call.
#include "aver/render/FluidScene.hpp"
#include "FluidShaders.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/core/Log.hpp"

#include <vector>

namespace aver::render {

namespace {

// A ceiling, so a runaway spawner degrades to "no new volume" rather than exhausting Jolt and GPU
// memory in silence -- SoftBodyScene's kMaxResident, and for the same reason. Set to the same value:
// a fluid volume costs about the same per instance as a soft body (one Jolt object plus its own
// staging ring), and unlike a soft body's entity, fluid volumes are typically placed by a level
// author in the low tens rather than spawned by the hundreds at runtime.
constexpr u32 kMaxResident = 64;

// Copies FluidVolume's own positions and normals straight into the interleaved layout the rasteriser
// reads. NOT a general-purpose pack: it does no space conversion and computes no normal of its own,
// because both of those are already done by the time either caller reaches this -- see spawn()'s and
// update()'s own comments for why. `out` is resized to match; UVs are written as zero rather than
// left uninitialised, since this shell has no source parameterisation to carry across.
void packFluidVerts(const std::vector<f32>& positionsCm, const std::vector<f32>& normals,
                    std::vector<rhi::MeshVertex>& out) {
    const size_t n = positionsCm.size() / 3;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].px = positionsCm[i * 3 + 0];
        out[i].py = positionsCm[i * 3 + 1];
        out[i].pz = positionsCm[i * 3 + 2];
        out[i].nx = normals[i * 3 + 0];
        out[i].ny = normals[i * 3 + 1];
        out[i].nz = normals[i * 3 + 2];
        out[i].u = 0.0f;
        out[i].v = 0.0f;
    }
}

} // namespace

FluidScene::~FluidScene() { shutdown(); }

bool FluidScene::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    ready_ = res_ != nullptr;
    if (!ready_) {
        AVER_WARN("[Fluid] this device has no resource factory; fluid volumes cannot be spawned "
                  "this run");
        return ready_;
    }

    // The transparentPass shaders and pipeline -- compiled here, best-effort, the identical
    // two-step pattern ParticleRenderer::init and WaterRenderer::init both follow: this call uses
    // whatever sample count/formats the device reports right now, and onRenderTargetsChanged rebuilds
    // against the real scene target's shape the moment the device calls it (before the first real
    // frame). A failure here is logged and left alone rather than propagated to `ready_` -- see this
    // class's own header comment on vs_/ps_/pso_ for why the physics half of this feature must not go
    // inert just because its draw half could not compile.
    rhi::ShaderDesc sd;
    sd.source = kFluidHLSL;
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry = "VSFluid";
    sd.stage = rhi::ShaderStage::Vertex;
    vs_ = res_->createShader(sd);
    sd.entry = "PSFluid";
    sd.stage = rhi::ShaderStage::Pixel;
    ps_ = res_->createShader(sd);
    if (!vs_ || !ps_) {
        AVER_ERROR("[Fluid] the fluid surface shaders failed to compile; every volume this run will "
                   "spawn and simulate but draw nothing");
    } else {
        buildPipeline(dev.sampleCount(), dev.backbufferFormat(), dev.depthFormat());
    }

    return ready_;
}

void FluidScene::shutdown() {
    for (auto& kv : live_) retire(kv.second);
    live_.clear();
    if (res_) {
        res_->destroyPipeline(pso_);
        res_->destroyShader(vs_);
        res_->destroyShader(ps_);
    }
    pso_ = 0;
    vs_ = ps_ = 0;
    bakedSampleCount_ = 1;
    bakedColorFmt_ = rhi::Format::Unknown;
    bakedDepthFmt_ = rhi::Format::Unknown;
    dev_ = nullptr;
    res_ = nullptr;
    ready_ = false;
}

// Tears down one resident's physics and GPU state. Also the unwind path inside spawn() when a later
// step fails after an earlier one succeeded -- every field it touches is guarded by its own
// null/zero check, so it is safe to call on a partially-built Resident.
void FluidScene::retire(Resident& r) {
    // The physics body first: a resource-factory failure below must not leave a soft body still
    // simulating for a handle nothing will draw again.
    if (r.body) {
        aver_phys_remove_body(r.body);
        r.body = 0;
    }

    if (res_) {
        for (rhi::BufferHandle& h : r.staging) {
            if (h) { res_->destroyBuffer(h); h = 0; }
        }
    }

    // r.vertices is deliberately NOT destroyed here through the factory, for the identical reason
    // SoftBodyScene::retire states: it is the buffer createSkinTargetMesh allocated as r.drawMesh's
    // vertex stream, and destroyMesh already frees it as part of freeing the mesh.
    if (dev_ && r.drawMesh) {
        dev_->destroyMesh(r.drawMesh);
    }
    r.drawMesh = 0;
    r.vertices = 0;

    // r.source, UNLIKE SoftBodyScene's equivalent, IS ours to free -- this module built it with its
    // own createMesh call in spawn() rather than borrowing it from a host's asset cache, so nothing
    // else will ever destroy it if this does not. Freeing it AFTER r.drawMesh, not before, matters:
    // createSkinTargetMesh built r.drawMesh to share r.source's index buffer, and destroyMesh's own
    // contract is to return false and free nothing for a mesh that is "still shared" -- so freeing
    // r.source first here would just fail quietly and leak it. By the time this line runs r.drawMesh
    // is already gone and holds no more shares, so this is the call that actually releases it.
    if (dev_ && r.source) {
        dev_->destroyMesh(r.source);
    }
    r.source = 0;
}

FluidHandle FluidScene::spawn(const water::FluidVolumeDesc& desc, rhi::IDevice& dev) {
    if (!ready_) return 0;
    if (live_.size() >= kMaxResident) {
        AVER_WARN("[Fluid] resident cap ({}) reached; refusing a new volume", kMaxResident);
        return 0;
    }

    Resident r(desc);
    r.vol.generateSeedShell();

    // The render mesh's OWN coordinate frame -- what createMesh's `source` is measured in, and
    // therefore what its once-computed bounds and any BLAS built from it are measured in too -- is
    // built from seedPositionsCm(), the LOCAL shell, not positionsCm(), the WORLD one
    // generateSeedShell() also populates. Every other MeshHandle in this engine keeps its own local
    // origin near its own geometry and lets a per-draw world matrix carry it to its actual placement
    // (meshBounds's own comment calls this out explicitly: a local-space bounding sphere, "before
    // any world transform"). Baking desc.centreCm into what the RHI treats as this mesh's local
    // frame would leave that bounding sphere sitting nowhere near the geometry it exists to cull,
    // for no benefit this module needs -- the physics body below is built from this exact same LOCAL
    // array for the identical reason.
    std::vector<rhi::MeshVertex> seedVerts;
    packFluidVerts(r.vol.seedPositionsCm(), r.vol.normals(), seedVerts);

    // vol.indices() is i32 (FluidVolume's own choice, matching aver_phys_softbody_create's
    // int32_t* below); IDevice::createMesh wants u32*. The reinterpret_cast is well-defined --
    // accessing an object through a pointer to its corresponding signed/unsigned type is explicitly
    // permitted, unlike most pointer-cast aliasing -- and every index here is a small non-negative
    // count that reads as the identical value either way.
    r.source = dev.createMesh(seedVerts.data(), static_cast<u32>(seedVerts.size()),
                              reinterpret_cast<const u32*>(r.vol.indices().data()),
                              static_cast<u32>(r.vol.indices().size()));
    if (!r.source) {
        AVER_WARN("[Fluid] this device would not upload the seed shell; refusing this volume");
        return 0;
    }

    r.drawMesh = dev.createSkinTargetMesh(r.source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        AVER_WARN("[Fluid] this device cannot make a skin-target mesh; refusing this volume");
        retire(r);
        return 0;
    }

    // The staging ring, one Upload buffer per frame-in-flight -- see this module's header for the
    // full argument (writeBuffer is immediate and unsynchronised, and this engine keeps
    // kFluidFramesInFlight frames going at once).
    rhi::BufferDesc bd;
    bd.bytes = static_cast<u64>(r.vol.vertexCount()) * sizeof(rhi::MeshVertex);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "Fluid staging";
    bool ringOk = true;
    for (u32 slot = 0; slot < kFluidFramesInFlight; ++slot) {
        r.staging[slot] = res_->createBuffer(bd);
        if (!r.staging[slot]) { ringOk = false; break; }
    }
    if (!ringOk) {
        AVER_WARN("[Fluid] could not allocate the staging ring; refusing this volume");
        retire(r);
        return 0;
    }

    const water::FluidVolumeDesc& d = r.vol.desc();
    // invMasses is NULL, deliberately: every particle keeps inverse mass 1, which means NOTHING IS
    // PINNED. A pinned particle is how a flag stays attached to its pole -- exactly the wrong
    // behaviour for a liquid, whose entire reason for existing here is that a hard enough slosh can
    // carry it clean over the basin rim and leave it there. Pinning even the bottom ring would
    // tether the whole body to its rest shape forever and defeat the one behaviour this module
    // exists to allow.
    // A desc that did not name a pressure gets one derived from its own size and subdivision. Done
    // HERE rather than inside water::FluidVolume because this is the module that owns the moment the
    // body is created -- FluidVolume deliberately knows nothing about soft bodies -- and done at all
    // because a single constant cannot be right for two pools of different depths. See
    // water::fluidPressureFor for the arithmetic.
    const f32 pressure = d.pressure < 0.0f ? water::fluidPressureFor(d) : d.pressure;
    r.body = aver_phys_softbody_create(r.vol.seedPositionsCm().data(), r.vol.vertexCount(),
                                       r.vol.indices().data(),
                                       static_cast<int32_t>(r.vol.indices().size()),
                                       nullptr,
                                       d.centreCm[0], d.centreCm[1], d.centreCm[2],
                                       d.compliance, pressure);
    if (!r.body) {
        AVER_WARN("[Fluid] Jolt refused to create a body for this volume; refusing it");
        retire(r);
        return 0;
    }

    const FluidHandle h = nextHandle_++;
    live_.emplace(h, std::move(r));
    return h;
}

void FluidScene::despawn(FluidHandle h) {
    const auto it = live_.find(h);
    if (it == live_.end()) return;
    retire(it->second);
    live_.erase(it);
}

void FluidScene::update(f32 elapsedSeconds) {
    // Stored unconditionally, even on the !ready_ early-out below (a device the caller never
    // finished initialising), rather than left at whatever update() last saw: transparentPass
    // separately no-ops without pso_, so there is no failure mode this ordering could paper over,
    // and skipping the store on the early-out path would leave gFluidTime frozen from whichever
    // frame LAST called update() with ready_ true, which is a strictly worse answer than "matches
    // this frame's real clock" for a feature that draws nothing until it is ready anyway.
    elapsedSeconds_ = elapsedSeconds;
    if (!ready_) return;

    // Reused across every resident this call rather than allocated per-volume: each buffer's
    // capacity only ever grows to the largest volume touched so far. SoftBodyScene::update keeps the
    // identical shared scratch for the identical reason.
    std::vector<f32> physicsXyz;
    std::vector<rhi::MeshVertex> packed;

    for (auto& kv : live_) {
        const FluidHandle h = kv.first;
        Resident& r = kv.second;
        // Sized from the SOLVER's own count, not just trusted to still equal r.vol.vertexCount() --
        // the two are supposed to be the same particle count forever once a body is built (nothing
        // in this ABI adds or removes soft-body particles after creation), but reading the body's
        // own answer and checking it against what FluidVolume expects is what turns a silent
        // mismatch into the warning below instead of a buffer overrun.
        const int32_t bodyCount = aver_phys_softbody_vertex_count(r.body);
        const i32 expected = r.vol.vertexCount();
        physicsXyz.resize(static_cast<size_t>(bodyCount) * 3);
        const int32_t got = aver_phys_softbody_vertices(r.body, physicsXyz.data(), bodyCount);
        if (got != expected) {
            // The handle went bad, or the particle count changed, without this resident going
            // anywhere. Leave prePass nothing new to copy rather than stage a short, torn read -- the
            // mesh keeps showing its last good frame instead of collapsing toward the origin.
            AVER_WARN("[Fluid] volume {} read back {} of {} expected particles this frame; its mesh "
                      "keeps last frame's pose", h, got, expected);
            r.stagedThisFrame = false;
            continue;
        }

        // updateFromSimulation also recomputes normals from this frame's deformed shape -- the seed
        // shell's normals stop being correct the instant the body moves, which is the whole point of
        // simulating it.
        r.vol.updateFromSimulation(physicsXyz.data(), got);

        // Packed straight from FluidVolume's own positions and normals, NOT through
        // render::softBodyPackVertices. That helper exists to solve two problems neither apply here:
        // it recomputes normals by re-walking the index buffer and accumulating face normals, which
        // is exactly what updateFromSimulation just finished doing to this same deformed shape, and
        // it converts world-space positions back to mesh-local because drawMesh is about to apply a
        // further entity transform on top. Nothing here applies a further transform -- see
        // drawHandle()'s own comment for why the composition root draws this buffer with an identity
        // world matrix -- so vol.positionsCm() is used exactly as FluidVolume already produced it,
        // in WORLD space, with no conversion and no second normal computation.
        packFluidVerts(r.vol.positionsCm(), r.vol.normals(), packed);

        r.ringSlot = (r.ringSlot + 1) % kFluidFramesInFlight;
        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(packed.size()) * sizeof(rhi::MeshVertex);
        if (!stage || !res_->writeBuffer(stage, packed.data(), bytes)) {
            AVER_WARN("[Fluid] volume {} could not stage this frame's vertices; its mesh keeps "
                      "last frame's pose", h);
            r.stagedThisFrame = false;
            continue;
        }
        r.stagedThisFrame = true;
    }
}

rhi::MeshHandle FluidScene::drawHandle(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0 : it->second.drawMesh;
}

int32_t FluidScene::physicsBody(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0 : it->second.body;
}

void FluidScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (!r.stagedThisFrame) continue;   // update() had nothing new to show this frame

        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(r.vol.vertexCount()) * sizeof(rhi::MeshVertex);

        // GeometryRead -> CopyDest for the copy, then straight back -- the only two states this
        // buffer is ever in, exactly as SoftBodyScene::prePass documents and for the same reason.
        // The round trip happens every frame a resident is staged, not once, because GeometryRead is
        // a READ state and the input assembler may have consumed the buffer in that state on any
        // prior frame.
        ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::CopyDest);
        ctx.copyBuffer(r.vertices, stage, bytes);
        ctx.bufferBarrier(r.vertices, rhi::ResourceState::CopyDest, rhi::ResourceState::GeometryRead);
        r.state = rhi::ResourceState::GeometryRead;

        r.stagedThisFrame = false;   // consumed; the next copy needs a fresh stage from update()
    }
}

bool FluidScene::buildPipeline(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    if (!res_ || !vs_ || !ps_) return false;

    res_->destroyPipeline(pso_);
    pso_ = 0;

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs_;
    gd.ps = ps_;

    // A root CBV at b(kFeatureFrameConstantRegister) -- "zero means root CBV" per PipelineLayout's
    // own comment -- carrying ONLY an elapsed-seconds clock for PSFluid's ripple perturbation
    // (FluidShaders.hpp's cbuffer FluidFrame). This is new since this pipeline was first built: the
    // original version of this method had no per-frame block at all, on the reasoning (still true
    // for the colours and the Fresnel/specular constants) that nothing PSFluid read varied per frame
    // or per volume. A moving ripple is the one exception -- animation is, by definition, a value
    // that must change frame to frame -- so unlike WaterFrame at 192 bytes of wave/grid state, this
    // is the smallest thing that could possibly go in that slot.
    gd.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;

    // gd.vertexLayout is left at its default (empty), which the backend reads as "the engine's own
    // MeshVertex" (GraphicsPipelineDesc::vertexLayout's own doc comment, RHIResources.hpp) rather
    // than a custom layout -- see FluidShaders.hpp's own comment on VSFluidIn for why that is not
    // optional here: rhi::Format has no three-component float entry, so a custom VertexLayout could
    // not describe this mesh's float3 POSITION0/NORMAL0 even if this pipeline wanted one to. This is
    // exactly the buffer drawMesh's own opaque pipeline already reads (both are rhi::MeshVertex,
    // stride 32), so binding it here needs no conversion, no second copy, and no new vertex format.

    // DEPTH-TESTED, DEPTH-WRITE OFF -- the transparentPass contract (RHIResources.hpp) and the exact
    // state ParticleRenderer::buildPipelines and WaterRenderer::buildPipeline both use. Write MUST
    // stay off: a blended surface that also wrote depth would occlude anything a LATER transparent
    // draw this same frame tried to put behind it, and would corrupt whatever runs after
    // transparentPass and reads the scene depth buffer as it was left (D3D12Device.cpp's own comment
    // at the transparentPass call site spells out the historical bug this guards against).
    gd.depth.test = true;
    gd.depth.write = false;

    // BACK, and the difference from WaterRenderer matters more than it looks. That renderer draws a
    // single-layer GRID: culling is irrelevant to it, and None is right there because the camera can
    // sit on either side of one sheet of ocean. This mesh is not a sheet -- it is a CLOSED BOX, the
    // soft-body shell FluidVolume generates, with six faces enclosing a volume.
    //
    // Drawn unculled, every pixel of the top surface also blends the far interior walls and the
    // bottom face stacked behind it: three or four layers of 15-100% alpha compositing over each
    // other. Measured on the FirstPerson pool, that is exactly what it looked like -- a saturated
    // cyan slab with dark blotches where face count changed, reading as coloured glass rather than
    // as water, and showing the box's own side planes as distinct facets through the surface. One
    // layer is what a water surface IS; the rest is the inside of a bag nobody should be seeing.
    //
    // The enterable-pool case that argued for None is real but is not solved by disabling culling:
    // a camera under the surface sees the shell's underside, which is back-facing, so None makes it
    // visible at the cost of breaking the ordinary above-water view that every player has all the
    // time. That trade is the wrong way round, and underwater rendering needs its own treatment
    // (Underwater.hpp) rather than a culling mode that degrades the common case to half-serve the
    // rare one.
    gd.cull = rhi::CullMode::Back;

    gd.renderTargetCount = 1;
    gd.renderTargets[0] = color;
    gd.depthFormat = depth;
    // MSAA IS BAKED INTO THE PIPELINE AT CREATION, the same DECIDED-1 rule ParticleRenderer's and
    // WaterRenderer's own buildPipeline(s) comments cite: the scene target's ACTUAL sample count, not
    // a hardcoded 1, rebuilt here whenever onRenderTargetsChanged reports it changed.
    gd.sampleCount = sampleCount;

    // Straight (non-premultiplied) alpha, matching PSFluid's own output exactly -- see
    // FluidShaders.hpp's comment on why this is the correct pairing, ported from WaterRenderer.cpp's
    // identical reasoning: PremultipliedAlpha would double-apply alpha to a shader that never
    // multiplies its own rgb by it, darkening the surface at every grazing angle.
    gd.blend = rhi::BlendMode::AlphaBlend;

    pso_ = res_->createGraphicsPipeline(gd);

    bakedSampleCount_ = sampleCount;
    bakedColorFmt_ = color;
    bakedDepthFmt_ = depth;

    if (pso_) {
        AVER_INFO("[Fluid] transparent-pass pipeline (re)built: {}x MSAA", sampleCount);
    } else {
        AVER_ERROR("[Fluid] transparent-pass pipeline build failed at {}x MSAA", sampleCount);
    }
    return pso_ != 0;
}

void FluidScene::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                        u32 width, u32 height) {
    (void)width; (void)height;
    if (sampleCount == bakedSampleCount_ && color == bakedColorFmt_ && depth == bakedDepthFmt_) return;
    buildPipeline(sampleCount, color, depth);
}

void FluidScene::transparentPass(rhi::IRenderContext& ctx) {
    if (!ready_ || !pso_ || !dev_ || live_.empty()) return;

    // Viewport, scissor and the scene colour+depth targets are ALL already set -- the
    // transparentPass contract (RHIResources.hpp) -- so nothing here touches any of them; only the
    // pipeline and each resident's own geometry are ours.
    //
    // A DELIBERATELY TINY PER-FRAME CONSTANT BUFFER, unlike WaterRenderer's 192-byte WaterFrame at
    // the same register. VSFluid still displaces nothing and still reads worldPos/worldNrm straight
    // off the staged mesh (see update()'s own comment) -- that reasoning has not changed. What
    // changed is PSFluid: it now perturbs its shading normal with a small animated ripple to give the
    // coarse 8x8 shell a surface detail no amount of Fresnel/colour tuning could add (see
    // FluidShaders.hpp's own comment on gFluidTime for why a moving pattern needs a moving number
    // from SOMEWHERE, and why that number is elapsedSeconds_ rather than a fresh clock of this
    // method's own). Four floats, one upload, once per frame -- not per resident, since every live
    // pool shares the same clock.
    const float fluidFrame[4] = {elapsedSeconds_, 0.0f, 0.0f, 0.0f};
    ctx.pushMarker("Aver.Fluid");
    ctx.setPipeline(pso_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, fluidFrame, sizeof(fluidFrame));
    for (auto& kv : live_) {
        const Resident& r = kv.second;
        if (!r.drawMesh) continue;   // this handle's spawn() never finished building a drawable mesh

        // meshGeometry(), not r.vertices/r.source directly: it is the one place this RHI already
        // resolves a MeshHandle to the buffers and counts a raw draw call needs, and it is what every
        // other feature drawing outside the ordinary drawMesh() path already goes through (PathTracer,
        // VoxiRenderer -- see meshGeometry()'s own doc comment: "this is what a RAY needs and a raster
        // draw never did", equally true of a raster draw issued from OUTSIDE drawMesh(), which is
        // exactly this one).
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vertexCount = 0, indexCount = 0;
        if (!dev_->meshGeometry(r.drawMesh, &vb, &ib, &vertexCount, &indexCount) || !vb || !ib)
            continue;
        ctx.setVertexBuffer(vb, sizeof(rhi::MeshVertex));
        ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
        ctx.drawIndexed(indexCount, 0, 0);
    }
    ctx.popMarker();
}

} // namespace aver::render
