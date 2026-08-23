#pragma once
// The join between a water::FluidVolume's simulation and the rasteriser -- the one place in this
// engine that is allowed to know both what water is and what the physics solver is. See this
// module's CMakeLists.txt for why that has to be true of exactly one place and why this is it.
//
// SoftBodyScene (modules/render.softbody) is the model, and the shape carries over almost exactly:
// a per-instance simulated mesh substituted in for an authored one via createSkinTargetMesh, staged
// through an Upload ring because the producer is the CPU, copied into the real vertex buffer under a
// barrier in prePass. Read that module first; nothing about the GPU half below departs from it.
//
// WHAT IS GENUINELY DIFFERENT FROM A SOFT BODY, and why this is not just SoftBodyScene with a new
// name:
//
//   THE GEOMETRY IS PROCEDURAL, NOT AN ASSET. A soft body softens something a level author modelled
//   and exported; a fluid volume's shell is generated arithmetic (water::generateFluidSeedShell),
//   built directly from a FluidVolumeDesc. There is nothing to decode, nothing to cache by id, and
//   therefore no dependency on Aver.Formats at all.
//
//   THERE IS NO OWNING ENTITY. A soft body is discovered by walking scene::World for a CSoftBody
//   component and reads that entity's CWorld to place itself; SoftBodyScene's update() does that
//   walk every frame. A fluid volume is spawned directly from a desc by whoever wants one -- there is
//   no component driving it and nothing here depends on Aver.Scene. That also means the vertices this
//   module packs are never converted back into a mesh-local frame the way softBodyPackVertices
//   converts a soft body's: see spawn()'s and update()'s own comments for why WORLD-space vertices
//   are exactly what belongs in the buffer this module hands to the rasteriser, and why the
//   composition root must draw the result with an IDENTITY world matrix rather than an entity's own.
//
//   THIS MODULE OWNS THE MESH IT UPLOADS, where SoftBodyScene never does. A soft body softens a mesh
//   the host's asset cache already owns; a fluid volume's `source` mesh is built by this module's own
//   call to IDevice::createMesh, so retiring a resident here also has to destroy `source`, which
//   SoftBodyScene's retire() never needed to.
//
// BE HONEST ABOUT WHAT THE SOLVER BEHIND THIS CANNOT DO, because it is the first thing anyone
// deciding whether to reach for this module needs to know and the last thing they should have to
// discover by watching it fail. aver_phys_softbody_create builds a FIXED-TOPOLOGY triangle shell:
// the vertex count and the edges between them are frozen the moment the body is created. That gives
// a single connected mass that sags under its own weight, sloshes when it is disturbed, and can
// deform up and over a rim and stay there -- a basin overflowing, not a fountain. It cannot splash: a
// splash is droplets separating from the body, which means particles that stop being connected to
// their neighbours, and this solver has no mechanism for a constraint to disappear. For the same
// reason it cannot merge two volumes into one. Anything that reads as "water breaking apart or
// joining" needs an actual particle system, not this.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/water/FluidVolume.hpp"
#include "aver/core/Types.hpp"

#include <unordered_map>
#include <vector>

namespace aver::render {

// Frames the GPU may have in flight. Same constant, same reasoning, as SoftBodyScene's own
// kSoftBodyFramesInFlight -- writeBuffer is immediate and unsynchronised, and this engine keeps this
// many frames going at once, so a single staging buffer would be overwritten while the GPU was still
// copying out of it.
inline constexpr u32 kFluidFramesInFlight = 3;

// An opaque, never-recycled handle to one spawned fluid volume. Zero is never valid, matching every
// other handle type in this RHI tier (MeshHandle, BufferHandle, ...).
using FluidHandle = u32;

class FluidScene final : public rhi::IRenderFeature {
public:
    ~FluidScene() override;

    // False leaves it INERT rather than broken: spawn() then refuses every volume and every other
    // method does nothing. Same contract SoftBodyScene::init states, for the same reason -- a device
    // without a resource factory is a supported configuration, not an error.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    // Builds one fluid volume from `desc`: its seed shell, its GPU mesh, and the soft body that
    // simulates it. Returns a handle good until despawn() or shutdown(), or ZERO if this device
    // cannot make a skin-target mesh, cannot allocate the staging ring, or Jolt refused the body --
    // the same "zero is a supported outcome, not a crash" contract SoftBodyScene's resident() states,
    // for the identical reason: a caller that cannot spawn a volume this run still has to keep
    // running.
    //
    // The mesh this call uploads shows the SEED shell -- water at rest, undeformed -- until the next
    // update() reaches this handle and the following prePass copies what it staged. A caller that
    // draws a freshly spawned handle before update() has run for it once will see the seed shell
    // sitting at LOCAL (0,0,0) rather than desc.centreCm; see update()'s own comment for why the
    // buffer's contents move from local to world space the moment simulation starts, and see
    // createSkinTargetMesh's own comment for why showing a not-yet-posed rest shape for one frame is
    // this engine's existing, accepted convention rather than a new gap opened here.
    FluidHandle spawn(const water::FluidVolumeDesc& desc, rhi::IDevice& dev);

    // Tears down one volume's physics and GPU state immediately. A no-op for an unknown or
    // already-despawned handle.
    void despawn(FluidHandle h);

    // Reads back every live volume's current particle positions and stages them for the copy prePass
    // will issue. Call ONCE per frame, AFTER aver_phys_step, BEFORE prePass.
    //
    // THE STAGE HAPPENS HERE, NOT IN prePass, which is where SoftBodyScene puts it. That is a real
    // difference from the model this class otherwise follows exactly, and the reason is that
    // writeBuffer needs no IRenderContext -- IResourceFactory::writeBuffer is immediate and
    // unsynchronised on its own terms -- so nothing is gained by waiting for prePass to do it, and
    // update() already has to be here to read the physics body back regardless. What genuinely
    // cannot happen without an IRenderContext is the barriered copyBuffer, so that alone is what
    // prePass is left to do.
    void update();

    // What the composition root should draw for this handle, or ZERO for an unknown or despawned
    // one -- draw the entity's authored placeholder instead of nothing, the same convention
    // SoftBodyScene::drawHandle documents.
    //
    // DRAW WITH AN IDENTITY WORLD MATRIX. Once update() has run for a handle at least once, the
    // vertex buffer this returns holds water::FluidVolume::positionsCm() verbatim -- WORLD-space,
    // because that is what the physics ABI returns and what FluidVolume's own contract promises a
    // renderer ("what a renderer draws", in FluidVolume.hpp's own words). A soft body's buffer needs
    // an entity's world matrix on top because softBodyPackVertices deliberately converts back to
    // mesh-local first; this buffer was never converted, so applying any further translation or
    // rotation at draw time would move it a second time.
    rhi::MeshHandle drawHandle(FluidHandle h) const;

    // The aver_phys_softbody_* handle behind this volume, or ZERO for an unknown/despawned handle or
    // one whose body creation failed (see spawn()'s own comment on that outcome). Exists for a
    // composition root that wants to reach into the LIVE solver state directly -- nudging nearby
    // particles from outside the simulation loop, the way aver_phys_softbody_apply_impulse does for a
    // player wading through a pool -- rather than duplicating what this class already tracks in a
    // second handle table of its own.
    int32_t physicsBody(FluidHandle h) const;

    const char* name() const override { return "Aver.Render.Fluid.Scene"; }
    // The copy happens in prePass, which is the only hook that runs before the scene pass reads the
    // buffer it writes -- update() has no IRenderContext to issue a barrier or a copy with.
    void prePass(rhi::IRenderContext& ctx) override;

    u32 residentCount() const { return static_cast<u32>(live_.size()); }

private:
    // One fluid volume's simulation and GPU residency. Owns the FluidVolume itself -- there is no
    // asset cache to keep it in, the way SoftBodyScene's decoded_ keeps an OcMeshData -- because a
    // fluid volume's shape is authored once, per instance, by its own desc, and shared by nothing
    // else.
    struct Resident {
        explicit Resident(const water::FluidVolumeDesc& desc) : vol(desc) {}

        water::FluidVolume vol;

        // Built by THIS module's own createMesh call, unlike SoftBodyScene's `source`, which belongs
        // to the host's asset cache -- see retire()'s own comment for what that ownership costs.
        rhi::MeshHandle    source   = 0;
        rhi::MeshHandle    drawMesh = 0;   // createSkinTargetMesh's return; what the scene pass draws
        rhi::BufferHandle  vertices = 0;   // drawMesh's vertex stream, Default-heap, written by copyBuffer
        rhi::BufferHandle  staging[kFluidFramesInFlight]{};   // Upload; the ring, see the header above
        u32                ringSlot = 0;

        // COMMON, NOT GeometryRead, and for the same real reason SoftBodyScene::Resident::state
        // documents: createSkinTargetMesh seeds the buffer with `source`'s vertices and then
        // explicitly barriers it back to Common, on both backends, so the first transition this
        // class issues has to start from the state the runtime actually left it in.
        rhi::ResourceState state = rhi::ResourceState::Common;

        i32  body = 0;   // aver_phys_softbody_create's handle

        // Set by update() once it has staged fresh bytes for this handle, cleared by prePass once it
        // has copied them. Unlike SoftBodyScene::Resident, there is no persistent CPU-side vertex
        // array here to go with the flag: the packed rhi::MeshVertex array is built, staged and
        // discarded within a single update() call (see update()'s own comment for why writeBuffer
        // already happens there), so nothing needs to survive from update() into prePass except this
        // bit and which ring slot it was written to.
        bool stagedThisFrame = false;
    };

    void retire(Resident& r);

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    bool ready_ = false;

    std::unordered_map<FluidHandle, Resident> live_;
    FluidHandle nextHandle_ = 1;
};

} // namespace aver::render
