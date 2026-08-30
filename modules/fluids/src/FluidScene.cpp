// See FluidScene.hpp for the whole design. What follows is spawn/despawn/update/prePass, the private
// retire() they share, and the one small packing helper both spawn() and update() call.
#include "aver/fluids/FluidScene.hpp"
#include "FluidShaders.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace aver::fluids {

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
//
// originCm, WHEN GIVEN, IS SUBTRACTED, and that is what keeps the buffer in the mesh-LOCAL frame the
// rest of this engine assumes. The seed shell is already built about local (0,0,0)
// (FluidVolume.cpp: "LOCAL + centre = WORLD"), but the SOLVER hands its particles back in absolute
// world space, so update() has to put them back. This buffer used to be left in world space and
// drawn with an identity matrix -- which renders correctly and sorts WRONG: the blended flush orders
// draws by world[12..14] (D3D12Device.cpp:5006-5013), and an identity matrix's translation is the
// world ORIGIN, so a pool authored anywhere else sorted as though it sat at (0,0,0) and could
// composite on the wrong side of a glass pane it was plainly in front of.
//
// Normals are NOT offset: a translation does not rotate them.
void packFluidVerts(const std::vector<f32>& positionsCm, const std::vector<f32>& normals,
                    std::vector<rhi::MeshVertex>& out, const f32* originCm = nullptr) {
    const f32 ox = originCm ? originCm[0] : 0.0f;
    const f32 oy = originCm ? originCm[1] : 0.0f;
    const f32 oz = originCm ? originCm[2] : 0.0f;
    const size_t n = positionsCm.size() / 3;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].px = positionsCm[i * 3 + 0] - ox;
        out[i].py = positionsCm[i * 3 + 1] - oy;
        out[i].pz = positionsCm[i * 3 + 2] - oz;
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

FluidHandle FluidScene::spawn(const fluids::FluidVolumeDesc& desc, rhi::IDevice& dev) {
    if (!ready_) return 0;
    if (live_.size() >= kMaxResident) {
        AVER_WARN("[Fluid] resident cap ({}) reached; refusing a new volume", kMaxResident);
        return 0;
    }

    // THE MATERIAL LAYER RESOLVES HERE, AND ONLY HERE -- see fluids::fluidResolvePhysicsMaterial's own
    // doc comment (FluidVolume.hpp) for why this is the one call every fluid request converges on
    // regardless of how it was authored: a WATER record via SandboxApp::applyLevelWater's direct
    // construction, a graph's `COMP ... Fluid` line via either of the framework relay's two
    // providers, or any future direct caller of this function. A desc that never set `material` is
    // untouched by this (fluidResolvePhysicsMaterial's own early-out), so every caller from before this
    // layer existed spawns identically to before. A desc that DOES set `material` alongside a
    // hand-set raw `damping` REFUSES the whole spawn rather than picking a winner -- this is the
    // design brief's precedence rule, enforced at the one point it cannot be silently skipped.
    fluids::FluidVolumeDesc resolved = desc;
    std::string materialConflict;
    if (!fluids::fluidResolvePhysicsMaterial(resolved, &materialConflict)) {
        AVER_ERROR("[Fluid] refusing to spawn: {}", materialConflict);
        return 0;
    }

    Resident r(resolved);
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

    const fluids::FluidVolumeDesc& d = r.vol.desc();
    // invMasses is UNIFORM -- every particle gets the SAME inverse mass, never 0 -- so NOTHING IS
    // PINNED regardless of what that shared value is. A pinned particle is how a flag stays attached
    // to its pole -- exactly the wrong behaviour for a liquid, whose entire reason for existing here
    // is that a hard enough slosh can carry it clean over the basin rim and leave it there. Pinning
    // even the bottom ring would tether the whole body to its rest shape forever and defeat the one
    // behaviour this module exists to allow.
    //
    // The shared value itself is fluids::fluidParticleMassKg(d), inverted: 1/1.0f == 1.0f whenever
    // d.densityKgM3 is left at fluids::kFluidDensityUnset, so this array is filled with exactly 1.0f
    // in every slot for a desc that never asked for a real density -- bit-for-bit the same per-vertex
    // value aver_phys_softbody_create's own `invMasses ? invMasses[i] : 1.0f` fallback already used
    // when this was still a null pointer (PhysicsWorld.cpp's buildSoftShared). A desc that DID ask
    // for a density gets a real per-particle mass instead: denser water sags harder under the same
    // pressure and compliance, exactly the physics fluidPressureFor's own derivation now accounts
    // for below.
    const f32 particleMassKg = fluids::fluidParticleMassKg(d);
    const std::vector<f32> invMasses(static_cast<usize>(r.vol.vertexCount()), 1.0f / particleMassKg);
    // A desc that did not name a pressure gets one derived from its own size and subdivision. Done
    // HERE rather than inside fluids::FluidVolume because this is the module that owns the moment the
    // body is created -- FluidVolume deliberately knows nothing about soft bodies -- and done at all
    // because a single constant cannot be right for two pools of different depths. See
    // fluids::fluidPressureFor for the arithmetic -- as of the density field above, that arithmetic
    // reads d.densityKgM3 too, so this already balances the SAME real mass invMasses just derived.
    const f32 pressure = d.pressure < 0.0f ? fluids::fluidPressureFor(d) : d.pressure;
    // damping/iterations pass straight through, the identical "the desc already carries the real
    // number, this call site just forwards it" shape compliance and pressure already have -- see
    // FluidVolumeDesc's own comment on the two fields for what each does and why neither substitutes
    // for the other, and aver_phys_softbody_create's own doc comment for the clamp this ABI call
    // applies before either reaches Jolt.
    r.body = aver_phys_softbody_create(r.vol.seedPositionsCm().data(), r.vol.vertexCount(),
                                       r.vol.indices().data(),
                                       static_cast<int32_t>(r.vol.indices().size()),
                                       invMasses.data(),
                                       d.centreCm[0], d.centreCm[1], d.centreCm[2],
                                       d.compliance, pressure, d.damping,
                                       static_cast<int32_t>(d.iterations));
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

        // WHERE THE SHELL ACTUALLY IS, versus where it was authored to be.
        //
        // A soft body is free to go anywhere the solver takes it, and nothing downstream notices: the
        // mesh is drawn from whatever positions come back, so a volume that has escaped its authored
        // box renders happily over whatever it now covers. That is not hypothetical -- it is how the
        // PTTest pool came to paint the pit's concrete walls white, which read for a whole session as
        // a renderer bug (a "blown-out pit interior", plus scattered dark pixels that were really the
        // wall showing through) and cost a full bisect of the texturing work to attribute correctly.
        //
        // The authored box is desc.centreCm +/- desc.halfExtentCm (FluidVolume.hpp:182-187). A little
        // overshoot is normal and expected -- pressure inflates the shell and the surface oscillates --
        // so this only says what the numbers are and lets the reader judge; it deliberately does not
        // warn on a threshold nobody has calibrated.
        {
            const std::vector<f32>& p = r.vol.positionsCm();
            if (p.size() >= 3 && (updates_ & (updates_ + 1)) == 0) {
                f32 lo[3] = {p[0], p[1], p[2]}, hi[3] = {p[0], p[1], p[2]};
                for (usize v = 3; v + 2 < p.size(); v += 3)
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = std::min(lo[a], p[v + static_cast<usize>(a)]);
                        hi[a] = std::max(hi[a], p[v + static_cast<usize>(a)]);
                    }
                const fluids::FluidVolumeDesc& d = r.vol.desc();
                AVER_INFO("[Fluid] volume {} shell bounds after {} update(s): "
                          "x [{:.1f} {:.1f}] y [{:.1f} {:.1f}] z [{:.1f} {:.1f}] cm; "
                          "authored x [{:.1f} {:.1f}] y [{:.1f} {:.1f}] z [{:.1f} {:.1f}]",
                          h, updates_,
                          lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                          d.centreCm[0] - d.halfExtentCm[0], d.centreCm[0] + d.halfExtentCm[0],
                          d.centreCm[1] - d.halfExtentCm[1], d.centreCm[1] + d.halfExtentCm[1],
                          d.centreCm[2] - d.halfExtentCm[2], d.centreCm[2] + d.halfExtentCm[2]);
            }
        }

        // Packed straight from FluidVolume's own positions and normals, NOT through
        // render::softBodyPackVertices. That helper exists to solve two problems neither apply here:
        // it recomputes normals by re-walking the index buffer and accumulating face normals, which
        // is exactly what updateFromSimulation just finished doing to this same deformed shape, and
        // it converts world-space positions back to mesh-local because drawMesh is about to apply a
        // further entity transform on top. Nothing here applies a further transform -- see
        // drawHandle()'s own comment for why the composition root draws this buffer with an identity
        // world matrix -- so vol.positionsCm() is used exactly as FluidVolume already produced it,
        // in WORLD space, with no conversion and no second normal computation.
        // The solver's positions are ABSOLUTE WORLD; the mesh's frame is local about the volume's
        // centre, the same frame spawn() seeded it in. See packFluidVerts for why that difference
        // matters to sorting rather than only to placement.
        packFluidVerts(r.vol.positionsCm(), r.vol.normals(), packed, r.vol.desc().centreCm);

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

    ++updates_;
}

rhi::MeshHandle FluidScene::drawHandle(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0 : it->second.drawMesh;
}

bool FluidScene::volumeOrigin(FluidHandle h, f32 out[3]) const {
    const auto it = live_.find(h);
    if (it == live_.end()) return false;
    const fluids::FluidVolumeDesc& d = it->second.vol.desc();
    out[0] = d.centreCm[0];
    out[1] = d.centreCm[1];
    out[2] = d.centreCm[2];
    return true;
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

    // PREMULTIPLIED alpha -- REVERSED from this pipeline's own prior choice, matching
    // WaterRenderer.cpp's identical reversal and for the identical reason: see that file's own
    // blend-state comment for the full argument. PSFluid (FluidShaders.hpp) used to output straight
    // alpha over a flat deep/shallow lerp, which AlphaBlend composited correctly because nothing in
    // that rgb needed protecting from attenuation. PSFluid now outputs a real specular term --
    // skyColor(R) mirror reflection plus GGX sun glitter, exactly PSWater's fix applied to the same
    // substance -- that must reach the framebuffer at full strength however transparent this pool is
    // authored to be. Straight AlphaBlend would multiply that reflection by alpha, reproducing the
    // "flat cartoon" defect one level deeper. Premultiplied (rgb = specular + diffuse*alpha, a = alpha
    // -- PSFluid's own final line) sends `specular` through unattenuated and lets only the transmitted
    // body colour shrink with coverage.
    gd.blend = rhi::BlendMode::PremultipliedAlpha;

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

// transparentPass IS GONE, AND THE ABSENCE IS THE FEATURE. This class used to bind its own pipeline
// and issue its own drawIndexed here, which is what kept water out of the TLAS, out of the shadow
// term, out of fog and out of back-to-front sorting -- everything IDevice::drawMesh already arranges
// for any other surface. sandbox/src/SandboxApp.cpp now draws drawHandle() through setDrawBlended()
// + drawMesh() with a real .ocmat, so a fluid volume is an ordinary translucent mesh and this class
// is what its own header always said it was modelled on: SoftBodyScene, a feature that produces
// GEOMETRY and lets the composition root decide how it is shaded.
//
// Deleted rather than left as an empty override, so nothing can quietly start drawing here again.

} // namespace aver::fluids
