#pragma once
// The join between a simulated fluid volume and the rasteriser: owns each volume's particle solver
// (PbfFluid), the voxelised scene colliders it collides with (SolidField, built once at spawn from the
// physics world's overlap query -- the only thing here that touches Aver.Physics, see this module's
// CMakeLists.txt), its surface mesher, and the GPU mesh the surface is drawn through.
//
// RENDER PATH, UNCHANGED FROM THE SHELL'S: a per-instance mesh made with createSkinTargetMesh, staged
// through an Upload ring (the producer is the CPU) and copied into the real vertex buffer under a
// barrier in prePass -- SoftBodyScene's shape (modules/render.softbody). The mesh has a FIXED vertex
// and index count, so the surface is a triangle soup in the first 3*triangles vertices of a buffer
// sized for kFluidMaxSurfaceTris, the rest collapsed to a point (degenerate, culled). The host draws
// drawHandle(h) with a translation to volumeOrigin(h): vertices are mesh-local about the volume centre.
//
// WHAT THIS CAN DO THAT THE SHELL COULD NOT: splash, pour, spread over a floor and fill whatever the
// colliders leave -- it is particles. WHAT IT CANNOT: it is not coupled back to rigid bodies (no
// buoyancy from the particles, no push on a character; scene colliders are baked static at spawn),
// has no surface tension, and its cost scales with the particle count (docs/FLUIDS.md, "Cost").
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/fluids/FluidSurface.hpp"
#include "aver/fluids/FluidVolume.hpp"
#include "aver/fluids/PbfFluid.hpp"
#include "aver/core/Types.hpp"

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

namespace aver::fluids {

// Frames the GPU may have in flight (SoftBodyScene's kSoftBodyFramesInFlight, same reasoning:
// writeBuffer is immediate and unsynchronised, so one staging buffer would be overwritten mid-copy).
inline constexpr u32 kFluidFramesInFlight = 3;

// Surface triangles a volume's mesh holds at most (3 vertices each, 32 bytes a vertex: 5.8 MB at the
// cap, times the staging ring and the vertex buffer).
inline constexpr u32 kFluidMaxSurfaceTris = 60000;

// An opaque, never-recycled handle to one spawned fluid volume. Zero is never valid.
using FluidHandle = u32;

class FluidScene final : public rhi::IRenderFeature {
public:
    ~FluidScene() override;

    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    // Builds a volume from `desc` (after fluidResolvePhysicsMaterial) and returns its handle, or 0.
    FluidHandle spawn(const fluids::FluidVolumeDesc& desc, rhi::IDevice& dev);

    void despawn(FluidHandle h);

    // Steps every volume by the time since the last call (fixed 1/60 substeps, at most
    // kPbfMaxStepsPerAdvance per call) and stages the new surface. Call once per frame.
    void update();
    // The same with the elapsed time given by the caller.
    void update(f32 dtSeconds);

    rhi::MeshHandle drawHandle(FluidHandle h) const;

    // The volume's centre in world cm, the translation to draw drawHandle(h) at.
    bool volumeOrigin(FluidHandle h, f32 out[3]) const;

    // Retained so existing callers compile: a particle fluid has no physics body, always 0.
    int32_t physicsBody(FluidHandle h) const;

    // Particles in the volume and triangles in this frame's surface (0 for an unknown handle).
    u32 particleCount(FluidHandle h) const;
    u32 triangleCount(FluidHandle h) const;

    // Kicks the particles within radiusCm of a world-cm point by velCmPerS (linear falloff).
    void addImpulse(FluidHandle h, const f32 centreCm[3], f32 radiusCm, const f32 velCmPerS[3]);

    const char* name() const override { return "Aver.Fluids.Scene"; }
    void prePass(rhi::IRenderContext& ctx) override;

    u32 residentCount() const { return static_cast<u32>(live_.size()); }

private:
    struct Resident {
        explicit Resident(const fluids::FluidVolumeDesc& d) : desc(d) {}

        fluids::FluidVolumeDesc desc;
        fluids::PbfFluid sim;
        fluids::SolidField solid;            // referenced by sim: Resident never moves (unique_ptr)
        fluids::FluidSurfaceMesher mesher;

        rhi::MeshHandle    source   = 0;     // the fixed-topology template (identity indices)
        rhi::MeshHandle    drawMesh = 0;     // createSkinTargetMesh's return; what the scene pass draws
        rhi::BufferHandle  vertices = 0;     // drawMesh's vertex stream, written by copyBuffer
        rhi::BufferHandle  staging[kFluidFramesInFlight]{};
        u32                ringSlot = 0;
        rhi::ResourceState state = rhi::ResourceState::Common;

        u32  maxVerts = 0;
        u32  usedVerts = 0;                  // vertices the last staged surface used
        u32  copyVerts = 0;                  // vertices prePass copies this frame
        std::vector<rhi::MeshVertex> packed;
        bool stagedThisFrame = false;
        bool warnedTruncated = false;
    };

    void retire(Resident& r);
    void stageSurface(Resident& r);

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    bool ready_ = false;

    std::unordered_map<FluidHandle, std::unique_ptr<Resident>> live_;
    FluidHandle nextHandle_ = 1;

    std::chrono::steady_clock::time_point lastUpdate_{};
    bool haveLastUpdate_ = false;
};

} // namespace aver::fluids
