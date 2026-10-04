#pragma once
// The join between the scene and the skinning pass: which entities are skinned, and what each one
// draws instead of its static mesh.
//
// Aver.Render.Skin already knew how to skin a mesh and Aver.Anim.Scene already knew how to pose a
// skeleton, and nothing connected them -- CSkeletalMesh had no reader anywhere in the tree. This is
// that reader.
//
// It keeps residency per ENTITY rather than per mesh, because the posed vertices of two characters
// sharing one .ocmesh are two different sets of vertices. That is also what gives each of them its
// own acceleration structure, which a shared MeshHandle could never do.
#include "aver/render/SkinningPass.hpp"
#include "aver/anim/AnimSystem.hpp"
#include "aver/scene/World.hpp"

#include <unordered_map>
#include <vector>

namespace aver::render {

// The host answers both questions about an asset id, because asset discovery is a project's
// business and this module has no idea where content lives. Same shape as anim::AssetPathFn, and
// deliberately so: one resolver convention across the whole join tier.
using SkinPathFn = std::string     (*)(u64 objectId, void* user);
using SkinMeshFn = rhi::MeshHandle (*)(u64 objectId, void* user);

// Reconciles per-entity skinning residency and records the dispatches.
class SkinnedScene final : public rhi::IRenderFeature {
public:
    ~SkinnedScene() override;

    // False leaves it inert rather than broken: every method then does nothing and skinned entities
    // draw at rest. HLSL is compiled at RUNTIME, so this can fail on a machine where the build was
    // perfectly green.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    void setResolvers(SkinPathFn path, SkinMeshFn mesh, void* user) {
        path_ = path; mesh_ = mesh; user_ = user;
    }

    // Reconciles residency and stages this frame's matrices. Called ONCE per frame by the host,
    // AFTER AnimSystem::tick and World::flush and BEFORE the draw pass -- the draw pass asks
    // drawHandle() for a handle that must already exist.
    //
    // The matrices are COPIED rather than pointed at: AnimSystem::skinning is valid only until the
    // next tick, and prePass runs on the far side of the whole render.
    void update(scene::World& world, anim::AnimSystem& anim, rhi::IDevice& dev);

    // What the scene pass should draw for this entity, or ZERO meaning "draw its static mesh".
    // Zero is never the "draw nothing" answer -- a character that fails to skin must still appear.
    rhi::MeshHandle drawHandle(scene::Entity e) const;

    const char* name() const override { return "Aver.Skin.Scene"; }
    void prePass(rhi::IRenderContext& ctx) override;
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

    u32 residentCount() const { return static_cast<u32>(live_.size()); }
    u32 posedLastFrame() const { return posedLastFrame_; }
    // Entities whose CMeshRenderer bounds were rewritten this frame, so a caller can tell that
    // culling is being fed something other than the rest box.
    u32 boundsUpdatedLastFrame() const { return boundsLastFrame_; }

private:
    // One skinned entity's GPU residency.
    struct Resident {
        SkinnedMeshGpu    gpu{};
        rhi::MeshHandle   drawMesh = 0;   // createSkinTargetMesh's return; what the scene pass draws
        rhi::BufferHandle vertices = 0;   // its vertex buffer, which the compute pass writes
        u64  meshId = 0;                  // so a retired record can be handed to the next of its kind
        u32  boneCount = 0;
        std::vector<Mat4> staged;         // this frame's matrices: written in update, read in prePass
        bool atRest = false;              // `out` currently holds the rest pose; do not re-dispatch

        // One rest-space box per bone, and the mesh's own rest extent. Built ONCE at acquire, so
        // the posed bounds cost O(bones) per frame instead of O(vertices) -- see anim::posedBounds
        // for why an O(bones) answer is still guaranteed to contain every posed vertex.
        std::vector<Vec3> boneMin, boneMax;
        std::vector<u8>   boneUsed;
        Vec3 restMin{0, 0, 0}, restMax{0, 0, 0};
    };

    Resident* acquire(scene::World& world, anim::AnimSystem& anim, rhi::IDevice& dev,
                      scene::Entity e, u64 meshId);
    void retire(scene::Entity e);

    // KEYED BY THE FULL Entity HANDLE, index and generation both. Keying by index alone is the bug
    // AnimSystem::posed_ shipped with: a recycled slot inherits the previous occupant -- there, its
    // pose; here, its vertex buffer and its silhouette.
    std::unordered_map<scene::Entity, Resident> live_;

    // Residency whose entity died, keyed by MESH ID so the next entity drawing the same mesh takes
    // it back. NOT a free: the RHI has no destroyMesh, so a record merely dropped leaks a vertex
    // buffer for the run, and a character streaming in and out is exactly that loop.
    struct Retired { Resident r; u32 cooldown; };
    std::unordered_multimap<u64, Retired> free_;

    SkinningPass pass_;
    SkinPathFn   path_ = nullptr;
    SkinMeshFn   mesh_ = nullptr;
    void*        user_ = nullptr;
    bool         ready_ = false;
    u32          posedLastFrame_ = 0;
    u32          boundsLastFrame_ = 0;

    // Decoded meshes, cached by mesh id rather than per entity: the joints and weights are needed to
    // build residency and the host discards its OcMeshData after upload.
    std::unordered_map<u64, fmt::OcMeshData> decoded_;
    // Mesh ids already refused, so a mesh with no skin is not re-read from disk every frame.
    std::unordered_map<u64, bool> skinnable_;
};

} // namespace aver::render
