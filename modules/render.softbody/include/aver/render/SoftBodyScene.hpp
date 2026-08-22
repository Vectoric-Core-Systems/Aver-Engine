#pragma once
// The join between a simulated soft body and the rasteriser: which entities are soft, and what each
// one draws instead of its authored mesh.
//
// SkinnedScene (modules/render.skin) is the model, deliberately and almost line for line, because
// the problem is the same one: a per-ENTITY set of vertices that differ from the asset's, handed to
// the ordinary draw by substituting the MeshHandle rather than by teaching the draw path a new
// stream. That substitution already works -- both renderers do it for every skinned character today
// (GameRender.cpp:142, SandboxApp.cpp:4168) -- so nothing below this file changes.
//
// WHAT IS GENUINELY DIFFERENT FROM SKINNING, and it is the whole reason this is a separate feature
// rather than a flag on that one:
//
//   THE PRODUCER IS THE CPU. Skinning writes its vertices with a compute shader straight into the
//   buffer the input assembler reads. A soft body is simulated by Jolt on the CPU, so its positions
//   arrive in host memory and have to get to a Default-heap buffer that `writeBuffer` cannot touch
//   (it requires BufferKind::Upload and refuses anything else). The route is therefore an Upload
//   staging buffer, written from the CPU, then copyBuffer'd into the skin target under a barrier.
//
//   THE STAGING BUFFER MUST BE RINGED. `writeBuffer` is immediate and unsynchronised, and this
//   engine keeps frames in flight; writing one staging buffer every frame would overwrite bytes the
//   GPU is still copying. ParticleRenderer's kFramesInFlight ring is the precedent and the reason
//   this class carries one per resident rather than one buffer.
//
//   THE PRODUCER EMITS BARE POSITIONS. rhi::MeshVertex is {position, normal, uv} interleaved at a
//   32-byte stride, and aver_phys_softbody_vertices returns xyz only -- in WORLD space, while
//   drawMesh applies the entity's own transform and therefore expects mesh-local. Both gaps are
//   closed by softBodyPackVertices below, which is a pure function precisely so those two silent
//   failure modes are checkable without a GPU.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/scene/World.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Math.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aver::render {

// Frames the GPU may have in flight. Matches ParticleRenderer's own constant, and for the identical
// reason -- see the ring note in this file's header comment.
inline constexpr u32 kSoftBodyFramesInFlight = 3;

// ---- the pure half ---------------------------------------------------------------------------------

// Turns soft-body particle positions into the interleaved vertices the rasteriser reads.
//
// PURE, AND THEREFORE THE PART THAT IS ACTUALLY TESTED. Everything that can be silently wrong here
// is decidable with no GPU, no device and no physics:
//
//   * the SPACE. `worldPositionsXyz` is world-space centimetres, straight out of the physics ABI,
//     and drawMesh will apply the entity's world matrix on top -- so these must be brought back
//     into mesh-local first or the mesh renders at double its own transform. That failure looks
//     like a model flying away from its entity, which is obvious; the same bug at the origin looks
//     like nothing at all, which is why it is a test and not an eyeball.
//   * the NORMALS. The producer emits none, so they are recomputed here by accumulating each
//     triangle's face normal onto its three vertices and normalising. A soft body that deforms and
//     keeps its rest normals is lit as though it never moved.
//   * the UVs, which the producer also does not emit and which are simply carried across from the
//     source mesh, since simulation moves vertices but never re-parameterises them.
//
// `sourceUvs` may be null, in which case every uv is written as zero rather than left uninitialised.
// `out` must have room for `vertexCount` entries.
void softBodyPackVertices(const f32* worldPositionsXyz, u32 vertexCount,
                          const Mat4& worldToLocal,
                          const u32* indices, u32 indexCount,
                          const f32* sourceUvs,
                          rhi::MeshVertex* out);

// ---- the feature -----------------------------------------------------------------------------------

// The host answers what an asset id resolves to, because asset discovery is a project's business.
// Same two-resolver shape SkinnedScene uses, and deliberately so: one convention across the tier.
using SoftBodyMeshFn = rhi::MeshHandle (*)(u64 objectId, void* user);
// A PATH, and the feature decodes it -- NOT a decoded mesh handed in by the host.
//
// The first draft asked the host for a `const OcMeshData*`, which read cleanly and could not be
// satisfied: neither the editor nor the game runtime keeps decoded mesh data around, only
// id -> MeshHandle, so every host would have had to grow a second cache purely to feed this. A path
// they both ALREADY resolve (the same anim::AssetPathFn they hand to AnimSystem and SkinnedScene)
// costs them nothing, and the decode cache lives here where the one consumer is.
using SoftBodyPathFn = std::string (*)(u64 objectId, void* user);

class SoftBodyScene final : public rhi::IRenderFeature {
public:
    ~SoftBodyScene() override;

    // False leaves it INERT rather than broken: every method then does nothing and soft entities
    // draw their authored mesh at rest. Same contract SkinnedScene states, for the same reason --
    // a device without a resource factory is a supported configuration.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    void setResolvers(SoftBodyMeshFn mesh, SoftBodyPathFn path, void* user) {
        mesh_ = mesh; path_ = path; user_ = user;
    }

    // Reconciles residency and stages this frame's vertices. Called ONCE per frame by the host,
    // AFTER the physics step and World::flush and BEFORE the draw pass -- the draw pass asks
    // drawHandle() for a handle that must already exist.
    void update(scene::World& world, rhi::IDevice& dev);

    // What the scene pass should draw for this entity, or ZERO meaning "draw its authored mesh".
    // Zero is never "draw nothing": a body that failed to simulate must still appear.
    rhi::MeshHandle drawHandle(scene::Entity e) const;

    const char* name() const override { return "Aver.SoftBody.Scene"; }
    // The copy happens in prePass, which is the only hook that runs before the scene pass reads the
    // buffer it writes. Doing it in update() would be wrong: update() has no IRenderContext, and a
    // copy needs one to issue its barriers.
    void prePass(rhi::IRenderContext& ctx) override;

    u32 residentCount() const { return static_cast<u32>(live_.size()); }
    u32 simulatedLastFrame() const { return simulatedLastFrame_; }

private:
    // One soft entity's GPU and physics residency.
    struct Resident {
        rhi::MeshHandle   drawMesh = 0;   // createSkinTargetMesh's return; what the scene pass draws
        rhi::BufferHandle vertices = 0;   // its vertex buffer, Default-heap, written by copyBuffer
        rhi::BufferHandle staging[kSoftBodyFramesInFlight]{};   // Upload; the ring, see the header
        u32               ringSlot = 0;
        // COMMON, NOT GeometryRead, AND THE DIFFERENCE IS A REAL BUG rather than a formality.
        // createSkinTargetMesh seeds the buffer with the source mesh's vertices and then explicitly
        // barriers it BACK TO COMMON -- both backends do this and both say why in a comment
        // (D3D12Device.cpp:2726, VulkanDevice.cpp:1644). A first barrier claiming GeometryRead
        // therefore names a state the runtime disagrees with, and D3D12's debug layer reports it
        // "once per frame forever". Caught by review before it ever ran.
        rhi::ResourceState state = rhi::ResourceState::Common;
        i32               body = 0;       // aver_phys_softbody_create_* handle
        // Whether the SKINNED create path took it. Recorded because the two produce visibly
        // different behaviour -- tethered to a rig versus free-floating -- and "why is this cloth
        // not following the character" is otherwise unanswerable without a debugger.
        bool              skinned = false;
        u64               meshId = 0;
        u32               vertexCount = 0;
        const fmt::OcMeshData* source = nullptr;   // owned by the host's cache, not by this
        std::vector<rhi::MeshVertex> scratch;      // packed here, then written into the ring
        bool              packedThisFrame = false;
    };

    Resident* resident(scene::World& world, scene::Entity e, rhi::IDevice& dev);
    void retire(Resident& r);

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    bool ready_ = false;

    SoftBodyMeshFn mesh_ = nullptr;
    SoftBodyPathFn path_ = nullptr;
    void*          user_ = nullptr;

    // Decoded once per mesh id, not per entity: two soft bodies sharing one .ocmesh share this
    // entry, and only their SIMULATED vertices differ. SkinnedScene keeps the identical cache for
    // the identical reason.
    std::unordered_map<u64, fmt::OcMeshData> decoded_;

    std::unordered_map<scene::Entity, Resident> live_;
    // Meshes this device could not make a soft body from, so the failure is reported ONCE rather
    // than every frame forever. SkinnedScene keeps the same table for the same reason.
    std::unordered_map<u64, bool> softable_;
    u32 simulatedLastFrame_ = 0;
};

} // namespace aver::render
