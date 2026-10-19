#pragma once
// The draw side of soft bodies: what each piece of a simulated cloth or destructible draws instead of its
// authored mesh. The simulation, tears and pieces are Aver.SoftBody.Scene's (SoftBodySystem); this class only
// draws what the system publishes (PieceView) and no longer creates bodies.
//
// SkinnedScene (modules/render.skin) is the model, almost line for line: a per-ENTITY set of vertices that
// differ from the asset's, handed to the ordinary draw by substituting the MeshHandle rather than by teaching
// the draw path a new stream. Both hosts already do it for skinned characters (game::drawWorld asks for the
// posed handle; the editor fills in the soft-body half through SandboxApp::posedHandle).
//
// WHAT IS DIFFERENT FROM SKINNING:
//   THE PRODUCER IS THE CPU. Jolt simulates on the CPU, so positions arrive in host memory and have to reach a
//   Default-heap buffer that writeBuffer cannot touch: an Upload staging buffer, then copyBuffer under a barrier.
//   THE STAGING IS RINGED, kSoftBodyFramesInFlight deep: writeBuffer is immediate and unsynchronised.
//   THE PRODUCER EMITS BARE WORLD POSITIONS while rhi::MeshVertex is {position, normal, uv} interleaved and
//   drawMesh applies the entity transform on top. softBodyPackPieces closes both gaps and is a pure function so
//   the space conversion, the recomputed normals, the UV passthrough and the despawn shrink are checkable
//   without a GPU.
//   THE TOPOLOGY CHANGES. A tear changes the vertex and index count, which a skin target cannot do, so a piece
//   whose epoch changed gets a FRESH source mesh and skin target, and the old ones are retired
//   kSoftBodyFramesInFlight + 1 frames later (the "retire during recording" rule).
//   WAVE 10 (docs/plans/wave10/SOFTBODY_DEPTH.md): a view can carry generated vertices (caps; their UVs come with the
//   view), a debris or cap entity starts invisible and is revealed by this class once a resident is packed for it, a
//   BAKED piece (a settled one) gets one plain static mesh built once and never packs again, and resident builds have
//   their own per-frame budget (Options::maxDrawBuildsPerFrame), separate from the tear budget.
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/scene/World.hpp"
#include "aver/softbody/SoftBodySystem.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::render {

// Frames the GPU may have in flight. Matches ParticleRenderer's constant, for the same reason.
inline constexpr u32 kSoftBodyFramesInFlight = 3;

// ---- the pure half ---------------------------------------------------------------------------------

// Turns world-space particle positions into the interleaved vertices the rasteriser reads: positions through
// worldToLocal (row-vector: local = world * M), normals recomputed from the raw face normals of `indices`
// (a degenerate triangle contributes nothing, never a NaN), UVs passed through (zero when null).
// `out` must have room for `vertexCount` entries.
void softBodyPackVertices(const f32* worldPositionsXyz, u32 vertexCount,
                          const Mat4& worldToLocal,
                          const u32* indices, u32 indexCount,
                          const f32* sourceUvs,
                          rhi::MeshVertex* out);

// The same for a PieceView: draw vertices that may share a particle (a UV seam) or be duplicated by a tear
// keep their own UVs; `indices` holds only the visible triangles, so dead triangles do not light the normals;
// scale01 < 1 pulls every vertex toward `centroidWorld` (the despawn shrink).
void softBodyPackPieces(const Vec3* worldPositions, u32 vertexCount,
                        const Mat4& worldToLocal,
                        const u32* indices, u32 indexCount,
                        const f32* uvs,
                        f32 scale01, const Vec3& centroidWorld,
                        rhi::MeshVertex* out);

// The one-time static mesh of a baked piece: positions relative to `origin` (the entity sits there with an identity
// rotation), normals recomputed from `indices`, UVs passed through. Deterministic; no GPU.
void softBodyBakeVertices(const Vec3* worldPositions, u32 vertexCount,
                          const Vec3& origin,
                          const u32* indices, u32 indexCount,
                          const f32* uvs,
                          rhi::MeshVertex* out);

// ---- the feature -----------------------------------------------------------------------------------

// Kept for the hosts' existing wiring: the mesh resolver is no longer needed (the scene makes its own source
// mesh per epoch from the decoded asset); the PATH resolver is forwarded to the system's decode caches.
using SoftBodyMeshFn = rhi::MeshHandle (*)(u64 objectId, void* user);
using SoftBodyPathFn = std::string (*)(u64 objectId, void* user);

class SoftBodyScene final : public rhi::IRenderFeature {
public:
    ~SoftBodyScene() override;

    // False leaves it INERT rather than broken: soft entities then draw their authored mesh at rest.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    // The system whose pieces are drawn. Defaults to softbody::softBodySystem().
    void setSystem(softbody::SoftBodySystem* sys) { sys_ = sys; }
    softbody::SoftBodySystem& system() { return sys_ ? *sys_ : softbody::softBodySystem(); }

    void setResolvers(SoftBodyMeshFn mesh, SoftBodyPathFn path, void* user);

    // Reconciles residency with the system's PieceViews and stages this frame's vertices. Called ONCE per
    // frame by the host, after the soft-body system's tick and World::flush, before the draw pass.
    void update(scene::World& world, rhi::IDevice& dev);

    // What the scene pass should draw for this entity, or ZERO meaning "draw its authored mesh".
    rhi::MeshHandle drawHandle(scene::Entity e) const;

    const char* name() const override { return "Aver.Render.SoftBody"; }
    // The copy happens in prePass, the only hook that runs before the scene pass reads the buffer it writes.
    void prePass(rhi::IRenderContext& ctx) override;

    u32 residentCount() const { return static_cast<u32>(live_.size()); }
    u32 simulatedLastFrame() const { return simulatedLastFrame_; }
    u32 epochRebuildsLastFrame() const { return rebuildsLastFrame_; }
    // New residents and epoch rebuilds this frame (the number the Tuning HUD shows as "draw builds").
    u32 drawBuildsLastFrame() const { return rebuildsLastFrame_; }
    u32 bakedCount() const {
        u32 n = 0;
        for (const auto& kv : live_) n += kv.second.baked ? 1u : 0u;
        return n;
    }
    u32 retiredCount() const { return static_cast<u32>(retired_.size()); }

private:
    struct Resident {
        rhi::MeshHandle   drawMesh = 0;
        rhi::MeshHandle   source = 0;     // owns the index buffer the skin target shares
        rhi::BufferHandle vertices = 0;   // the skin target's vertex buffer, Default heap, written by copyBuffer
        rhi::BufferHandle staging[kSoftBodyFramesInFlight]{};
        u32               ringSlot = 0;
        // COMMON, NOT GeometryRead: createSkinTargetMesh barriers the buffer back to COMMON on both backends.
        rhi::ResourceState state = rhi::ResourceState::Common;
        u32               epoch = 0;
        u32               vertexCount = 0;
        scene::Entity     entity = scene::kInvalidEntity;
        std::vector<f32>  uvs;            // 2 per draw vertex, copied at the epoch
        std::vector<rhi::MeshVertex> scratch;
        bool              packedThisFrame = false;
        bool              packedOnce = false;
        bool              baked = false;  // a plain static mesh: no source, no skin target, no staging ring
    };
    struct Retired {
        rhi::MeshHandle drawMesh = 0, source = 0;
        rhi::BufferHandle staging[kSoftBodyFramesInFlight]{};
        u32 framesLeft = 0;
    };
    using Key = std::pair<scene::Entity, u32>;

    bool buildResident(Resident& r, const softbody::PieceView& v, rhi::IDevice& dev);
    bool buildBaked(Resident& r, const softbody::PieceView& v, rhi::IDevice& dev);
    static void reveal(scene::World& world, scene::Entity e);
    void retire(Resident& r);
    void destroyNow(Retired& t);
    void ageRetired();

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    bool ready_ = false;
    softbody::SoftBodySystem* sys_ = nullptr;

    std::map<Key, Resident> live_;
    std::unordered_map<scene::Entity, Key> byEntity_;
    std::vector<Retired> retired_;
    u32 simulatedLastFrame_ = 0;
    u32 rebuildsLastFrame_ = 0;
};

} // namespace aver::render
