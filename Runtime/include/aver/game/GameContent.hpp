// GameContent: the project's asset index, and the resolvers that read it.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_PBR
#  include "aver/pbr/Material.hpp"
// Needed for resolveMaterialTexture return type.
#  include "aver/pbr/MaterialSystem.hpp"
#endif

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Forward-declared to avoid widespread header inclusion.
namespace aver::fmt { struct OcMeshData; }

namespace aver::game {

// Every asset under the project's content root, keyed by ObjectId.
class GameContent {
public:
    // Adopts a project and indexes it. Safe to call again when the project changes; NOT per frame.
    void adopt(const fmt::ProjectDesc& project);

    // The native absolute path for an ObjectId, or empty.
    std::string pathFor(u64 id) const;

    // Paths of assets with matching extension (with dot, e.g. ".ocgraph"), sorted for determinism.
    std::vector<std::string> pathsWithExtension(std::string_view ext) const;

    usize size() const { return contentIndex_.size(); }
    const fmt::ProjectDesc& project() const { return project_; }

    // Every indexed asset, ObjectId -> absolute path, for a picker that lists assets by type.
    const std::unordered_map<u64, std::string>& index() const { return contentIndex_; }
    // Points one ObjectId at a file, for a caller that makes an asset reachable with no project open.
    void indexAsset(u64 id, std::string absolutePath) { contentIndex_[id] = std::move(absolutePath); }

    // Resolver for aver::anim::AnimSystem. `user` is a GameContent*.
    static std::string resolveAnimAsset(u64 id, void* user);

#if AVER_MODULE_PBR
    // The factory textures are uploaded through. Set once the device exists.
    void setTextureFactory(rhi::IResourceFactory* f) { textureFactory_ = f; }

    // Uploads the texture a material reference names. 0 keeps the slot's fallback.
    // `user` is a GameContent*.
    static pbr::MaterialSystem::ResolvedTexture resolveMaterialTexture(const pbr::TextureRef& ref, pbr::TextureSlot slot,
                                                     void* user);

    // Where an asset reference points on this machine, or empty.
    std::string resolveAssetPath(const pbr::TextureRef& ref) const;

    // The material a surface token names, loading it on first use. 0 when the project has none.
    pbr::MaterialHandle materialForSurface(const std::string& name);

    // Deliberately no loadProjectMaterials: materials load lazily per-surface instead.
    // `clearGraphRegistry` false leaves the process-wide pbr::materialGraphs() alone.
    void releaseProjectMaterials(bool clearGraphRegistry = true);

    // Level-scoped material release: destroys materials NOT in `keep` set.
    usize releaseMaterialsExcept(const std::unordered_set<std::string>& keep);
#endif

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    // Remembers that an interned surface token has an authored material behind it.
    void bindSurfaceMaterial(i32 token, pbr::MaterialHandle h) { surfaceMaterials_[token] = h; }
    pbr::MaterialHandle authoredFor(i32 token) const;
    // Every bound surface token, for a material picker.
    const std::unordered_map<i32, pbr::MaterialHandle>& surfaceMaterials() const { return surfaceMaterials_; }
#endif

#if AVER_MODULE_SCENE
    // The material token this mesh's own materialSlots[0] names, or 0 when it names none.
    i32 meshDefaultMaterial(u64 meshId) const;
    // The NAME a loaded mesh's materialSlots[0] carries, or empty -- for the editor's previews.
    const std::string& meshSlot0Name(u64 meshId) const;
#endif

    // Mesh lookup from content index (not gated on AVER_MODULE_SCENE).
    rhi::MeshHandle meshFor(u64 id) const;

#if AVER_MODULE_SCENE
    // Uploads the built-in primitives a .ocworld may name. Call once, before any project meshes.
    void registerBuiltins(rhi::IDevice& device);

    // One material-slot's worth of a mesh split.
    struct MeshPart {
        rhi::MeshHandle mesh = 0;
        i32             material = 0;
    };

    // Uploads every .ocmesh under the project's content root.
    void loadProjectMeshes(rhi::IDevice& device);

    // On-demand meshes (those under a level's STREAM lazy= folders; indexed, not uploaded, by
    // loadProjectMeshes). Refcounted: the first acquire uploads, the last release forgets the mesh and
    // queues its GPU handles. True on success. Eager meshes are permanently loaded: acquire returns
    // whether they are, release is a no-op.
    bool acquireMesh(rhi::IDevice& device, u64 id);
    void releaseMesh(rhi::IDevice& device, u64 id);
    bool meshLoaded(u64 id) const;
    // Prepares a lazy mesh on a worker thread (read, vertices and parts, collision mesh and shape) so a
    // later acquireMesh only uploads. Workers take the lowest `priority` first (a streamer passes the
    // distance); asking again updates it. No-op for eager or loaded meshes.
    void prefetchMesh(u64 id, f32 priority = 0.0f);
    // True for eager, loaded or unknown meshes and for finished prefetches; false while a lazy mesh is
    // still being prepared or has no prefetch yet (the queue was full).
    bool meshReady(u64 id) const;
    // The physics mesh shape a prefetch built from this mesh's collision (0 if none); the caller owns it.
    i32 takeMeshShape(u64 id);
    // D3D12Device::destroyMesh frees the mesh's resources immediately, so released meshes' handles wait
    // here. Call once per frame; destroys those released at least 3 frames before `frameIndex`.
    void flushMeshReleases(rhi::IDevice& device, u64 frameIndex);

    // Callback after each mesh is uploaded, before split parts are built. `data` is null for built-ins.
    struct LoadedMesh {
        u64 id;
        const std::string& relativePath;
        const fmt::OcMeshData* data;
        const std::vector<rhi::MeshVertex>& vertices;
        const std::vector<u32>& indices;
        rhi::MeshHandle handle;
    };
    using MeshLoadedFn = void (*)(const LoadedMesh& mesh, void* user);
    void setMeshLoadedHook(MeshLoadedFn fn, void* user) { meshLoaded_ = fn; meshLoadedUser_ = user; }

    // Fired after a lazy mesh is uploaded by acquireMesh (same payload as the loaded hook).
    void setMeshAcquiredHook(MeshLoadedFn fn, void* user) { meshAcquired_ = fn; meshAcquiredUser_ = user; }
    // Fired when a lazy mesh is unloaded (last release); its tables are already erased.
    using MeshReleasedFn = void (*)(u64 id, void* user);
    void setMeshReleasedHook(MeshReleasedFn fn, void* user) { meshReleased_ = fn; meshReleasedUser_ = user; }

    // Whether loadProjectMeshes uploads a coarser LOD per mesh as its depth-pass stand-in.
    void setBuildDepthProxies(bool on) { buildDepthProxies_ = on; }

    // A mesh uploaded elsewhere, registered under `id` (the editor's --skin-scene-test).
    void registerMesh(u64 id, rhi::MeshHandle handle, const std::pair<Vec3, Vec3>& bounds);

    // The ids loadProjectMeshes loaded, in load order.
    const std::vector<u64>& projectMeshIds() const { return projectMeshIds_; }

    // The per-material split for a mesh with more than one submesh, or nullptr if never split.
    // GameRender.cpp's draw walk reads this to plan one draw per part instead of one draw per mesh.
    const std::vector<MeshPart>* partsFor(u64 id) const;

    // Posed parts split re-cut over a posed copy, index-for-index with partsFor(id).
    // Cached on first ask, nullptr when the mesh has no split or has no skin streams.
    const std::vector<MeshPart>* posedPartsFor(rhi::IDevice& device, u64 id,
                                               rhi::MeshHandle baseMesh, rhi::MeshHandle posedMesh);

    // Forgets every project mesh (lazy refcounts survive; the next loadProjectMeshes re-uploads held
    // ones; adopt() drops them). If `destroyBaseHandles` is false, only forgets the handles.
    void releaseProjectMeshes(rhi::IDevice& device, bool destroyBaseHandles = true);

    usize meshCount() const { return sceneMeshes_.size(); }
    usize projectMeshCount() const { return projectMeshIds_.size(); }

    // Bounds as loaded from the .ocmesh, or nullptr. Used by the draw walk to cull.
    const std::pair<Vec3, Vec3>* boundsFor(u64 id) const;

    // Collision-only triangle mesh for `id`, lazily built and cached. `positions` is LOCAL space.
    // Simplifies to roughly 2 cm error unless Trifactor's coarsest-LOD pick has fewer triangles.
    // Disk-cached under <project>/Saved/DerivedDataCache/Collision. nullptr for no .ocmesh.
    struct CollisionMesh {
        std::vector<f32> positions;
        std::vector<u32> indices;
        u32 lod = 0;
        f32 errorCm = 0.0f;
    };
    const CollisionMesh* collisionMeshFor(u64 id);

    // How many times collisionMeshFor was answered from the disk cache.
    u32 collisionCacheHits() const { return collisionCacheHits_; }

    // Depth proxy map for LOD-based shadow/voxel optimization.
    const std::unordered_map<rhi::MeshHandle, rhi::MeshHandle>& depthProxyMap() const { return depthProxyMap_; }

    // Resolver for aver::render::SkinnedScene. Deliberately the SAME table the draw pass reads.
    static rhi::MeshHandle resolveSceneMesh(u64 id, void* user);

    // The named surfaces gameplay can ask for, by interned material token.
    struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };
    const SurfaceLook* lookFor(i32 material) const;
#endif

#if AVER_MODULE_PARTICLES
    // Loads every .ocparticle under the project's content root into particles::particleEffects().
    // Keyed by fnv1a64(relative path), the SAME id space contentIndex_ uses for all project assets.
    void loadProjectParticleEffects();
#endif

private:
    fmt::ProjectDesc project_;
    std::unordered_map<u64, std::string> contentIndex_;

    std::unordered_map<u64, rhi::MeshHandle>       sceneMeshes_;
#if AVER_MODULE_SCENE
    std::unordered_map<u64, std::pair<Vec3, Vec3>> meshBounds_;
    // mesh id -> the material token its materialSlots[0] names. See meshDefaultMaterial.
    std::unordered_map<u64, i32>                   meshSlot0Material_;
    std::unordered_map<u64, std::string>           meshSlot0Name_;
    std::vector<u64>                               projectMeshIds_;
    std::unordered_map<i32, SurfaceLook>           surfaceLooks_;
    // Depth proxy map: LOD meshes used instead of full detail in depth passes.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxyMap_;
    // mesh id -> its per-material split. See MeshPart.
    std::unordered_map<u64, std::vector<MeshPart>> meshParts_;
    // mesh id -> per part: indices UNREMAPPED in base mesh's vertex numbering (shared by skin targets).
    std::unordered_map<u64, std::vector<std::vector<u32>>> meshPartBaseIndices_;
    // posed MeshHandle -> its posed parts. Empty = refused, cached. Keyed by handle (never recycled).
    struct PosedParts { u64 meshId = 0; std::vector<MeshPart> parts; };
    std::unordered_map<rhi::MeshHandle, PosedParts> posedParts_;
    // collisionMeshFor's cache: mesh id -> collision mesh, or null pointer cached for "none".
    std::unordered_map<u64, std::unique_ptr<CollisionMesh>> collisionMeshCache_;
    // Incremented once per collisionMeshFor call answered from disk cache.
    u32 collisionCacheHits_ = 0;
    MeshLoadedFn meshLoaded_ = nullptr;
    void* meshLoadedUser_ = nullptr;
    bool buildDepthProxies_ = true;
    MeshLoadedFn meshAcquired_ = nullptr;
    void* meshAcquiredUser_ = nullptr;
    MeshReleasedFn meshReleased_ = nullptr;
    void* meshReleasedUser_ = nullptr;
    bool lazyLoading_ = false;   // true while acquireMesh uploads

    // Uploads one .ocmesh and fills every per-mesh table. False (nothing kept) on failure.
    bool loadOneMesh(rhi::IDevice& device, u64 id, const std::string& full, const std::string& rel);
    // CPU half of an upload (vertices, per-material parts), made on a prefetch worker or inline.
    struct PartCpu {
        std::vector<rhi::MeshVertex> verts;
        std::vector<u32> indices, baseIndices;   // baseIndices: skinned meshes only
        u32 slot = ~0u;
        std::string name;
    };
    struct MeshCpu {
        std::vector<rhi::MeshVertex> verts;
        std::vector<PartCpu> parts;
    };
    static void prepareMeshCpu(const fmt::OcMeshData& md, const std::string& rel, MeshCpu& out);
    bool uploadMesh(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md, const std::string& rel, MeshCpu* cpu = nullptr);
    std::unordered_map<u64, i32> readyShapes_;   // mesh id -> prefetched physics shape, until taken

    // prefetchMesh's workers and finished reads. takePrefetch: 1 read (md/collision filled), 0 the read
    // failed (why filled), -1 nothing finished for `id`.
    struct MeshPrefetch;
    std::shared_ptr<MeshPrefetch> prefetch_;
    int takePrefetch(u64 id, fmt::OcMeshData& md, MeshCpu& cpu, std::unique_ptr<CollisionMesh>& collision,
                     i32& shape, std::string& why);
    void dropPrefetches();
    // Erases every table entry for `id` and queues its GPU handles for flushMeshReleases.
    void unloadToPending(u64 id);

    struct PendingMesh {
        rhi::MeshHandle base = 0;
        std::vector<rhi::MeshHandle> others;   // parts, posed parts, depth proxy
        u64 releasedAt = 0;
        bool warned = false;
    };
    std::vector<PendingMesh> pendingMeshes_;
    u64 lastFlushFrame_ = 0;
    std::unordered_map<u64, std::string> lazyRel_;   // lazy mesh id -> content-relative path
    std::unordered_map<u64, u32>         meshRefs_;  // lazy mesh id -> acquire count

    // Whether a material-slot NAME should collide: false only for Mask or Blend alphaMode.
    bool collisionSlotCollides(const std::string& slotName);

    // Splits `md` into one compacted MeshHandle + material token per submesh, when it names more than
    // one -- a no-op otherwise. Ported from SandboxApp::buildMeshParts (sandbox/src/SandboxAssets.cpp):
    // same compaction (each part gets its OWN remapped vertex/index arrays, not a view into `verts`,
    // because IDevice::createMesh copies what it is given and a part sharing the parent's whole buffer
    // would upload it once per part), same slot-name-to-material-token rule (a submesh's materialSlot
    // names a string in md.materialSlots, resolved through aver_scene_material the same way
    // meshSlot0Material_ already is above), same "one surviving part is not a split" fallback. Called
    // unconditionally from loadProjectMeshes, not gated on AVER_MODULE_LANDSCAPE the way the editor's
    // call site is -- see MeshPart's own comment for why that guard does not belong here.
    void buildMeshParts(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md,
                         std::vector<PartCpu>& cpuParts, const std::string& rel);
#endif

#if AVER_MODULE_PBR
    rhi::IResourceFactory* textureFactory_ = nullptr;
    std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;

    // Turns an .ocmat's GRAPHREF path into the id materialForSurface() stores.
    // Ported from SandboxApp::resolveMaterialGraph, same cache-by-compiled-path through pbr::materialGraphs().
    u32 resolveMaterialGraph(const std::string& graphRef) const;
#endif
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;
#endif
};

} // namespace aver::game
