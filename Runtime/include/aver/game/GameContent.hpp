// GameContent: the project's asset index, and the resolvers that read it.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_PBR
#  include "aver/pbr/Material.hpp"
// resolveMaterialTexture below returns MaterialSystem::ResolvedTexture by value, so the full type
// is needed here rather than a forward declaration.
#  include "aver/pbr/MaterialSystem.hpp"
#endif

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Forward-declared rather than included: only buildMeshParts' PRIVATE signature (GameContent.cpp)
// needs the full aver/formats/OcMesh.hpp, and this header is included widely enough that a leaf
// forward declaration is worth it over a header nothing else here reads.
namespace aver::fmt { struct OcMeshData; }

namespace aver::game {

// Every asset under the project's content root, keyed by ObjectId.
//
// DELIBERATELY UNGUARDED, and this is the one design decision in the lift worth arguing for. In
// SandboxApp this map and the walk that fills it sit inside `#if AVER_MODULE_PBR`
// (SandboxApp.cpp:1132-1347), which means a tree built PBR=OFF, SCENE=ON resolves no asset by id at
// all and hands the animation system no resolver -- an accident of where the code happened to be
// written, not a decision. Copying it verbatim would copy the accident: MOVING CODE DOES NOT CHANGE
// WHICH #if IT IS WRITTEN UNDER. So the guard is re-decided here, per symbol.
//
// Nothing about this class needs either module. It is an unordered_map of u64 to std::string, and
// fnv1a64 lives in Aver.Core.
//
// Not a god object: the resolvers take `void* user` pointing at a GameContent, never at the app, so
// nothing downstream acquires a handle on the whole game to look up a file.
class GameContent {
public:
    // Adopts a project and indexes it. Safe to call again when the project changes; NOT per frame --
    // it walks the whole content tree.
    void adopt(const fmt::ProjectDesc& project);

    // The native absolute path for an ObjectId, or empty.
    std::string pathFor(u64 id) const;

    // Every indexed asset's absolute path whose extension case-insensitively matches `ext` (pass it
    // WITH the dot, e.g. ".ocgraph"), sorted for a deterministic order run to run. Built by filtering
    // the SAME contentIndex_ that adopt() fills with a hardened, error_code-based recursive walk --
    // added for visual-scripting phase 2's graph discovery (GameApp::discoverProjectGraphs), but
    // deliberately generic rather than named pathsToGraphs: the walk this reuses already exists and
    // asking it for a second, easier-to-get-wrong directory scan would be the exact "moved code
    // doesn't change which #if it's under" mistake this class's own header comment warns against.
    std::vector<std::string> pathsWithExtension(std::string_view ext) const;

    usize size() const { return contentIndex_.size(); }
    const fmt::ProjectDesc& project() const { return project_; }

    // Every indexed asset, ObjectId -> absolute path, for a picker that lists assets by type.
    const std::unordered_map<u64, std::string>& index() const { return contentIndex_; }
    // Points one ObjectId at a file, for a caller that makes an asset reachable with no project open
    // (the editor's --skin-scene-test). adopt() replaces the whole index.
    void indexAsset(u64 id, std::string absolutePath) { contentIndex_[id] = std::move(absolutePath); }

    // Resolver for aver::anim::AnimSystem, which takes a plain function pointer: asset discovery is
    // the host's business, not the sampler's. `user` is a GameContent*.
    static std::string resolveAnimAsset(u64 id, void* user);

#if AVER_MODULE_PBR
    // The factory textures are uploaded through. Set once the device exists.
    void setTextureFactory(rhi::IResourceFactory* f) { textureFactory_ = f; }

    // Uploads the texture a material reference names. 0 keeps the slot's fallback.
    //
    // A plain function pointer with a void* because that is what MaterialSystem's resolver takes.
    // `user` is a GameContent*.
    static pbr::MaterialSystem::ResolvedTexture resolveMaterialTexture(const pbr::TextureRef& ref, pbr::TextureSlot slot,
                                                     void* user);

    // Where an asset reference points on this machine, or empty.
    std::string resolveAssetPath(const pbr::TextureRef& ref) const;

    // The material a surface token names, loading it on first use. 0 when the project has none.
    pbr::MaterialHandle materialForSurface(const std::string& name);

    // NO loadProjectMaterials(), deliberately. The runtime carried a copy of the editor's, nothing
    // ever called it, and it was removed: a level resolves each surface it references through
    // materialForSurface(), lazily and Binaries-first, so eagerly loading every project material would
    // only cost load time and memory. If an eager preload is ever wanted, wire one deliberately --
    // scanning Binaries\Materials as well as Content\Materials -- rather than reviving a stale copy.
    //
    // `clearGraphRegistry` false leaves the process-wide pbr::materialGraphs() alone, for a host that
    // registers graphs there itself (the editor's material graph editor does).
    void releaseProjectMaterials(bool clearGraphRegistry = true);
#endif

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    // Remembers that an interned surface token has an authored material behind it.
    void bindSurfaceMaterial(i32 token, pbr::MaterialHandle h) { surfaceMaterials_[token] = h; }
    pbr::MaterialHandle authoredFor(i32 token) const;
    // Every bound surface token, for a material picker.
    const std::unordered_map<i32, pbr::MaterialHandle>& surfaceMaterials() const { return surfaceMaterials_; }
#endif

#if AVER_MODULE_SCENE
    /// The material token this mesh's own materialSlots[0] names, or 0 when it names none.
    ///
    /// THE EDITOR'S RULE, HELD HERE TOO, and it has to be: a fallback the editor honours and the
    /// packaged game does not is this repo's most-repeated defect shape, and the divergence gate in
    /// scripts/verify-game.ps1 exists because of it. An entity whose CMeshRenderer.material is 0 is
    /// not saying "draw me flat", it is saying nothing -- and the mesh it points at already declares
    /// a material. A non-zero material stays an override, exactly as before.
    i32 meshDefaultMaterial(u64 meshId) const;
    // The NAME a loaded mesh's materialSlots[0] carries, or empty -- for the editor's previews,
    // which resolve it through materialForSurface.
    const std::string& meshSlot0Name(u64 meshId) const;
#endif

    // MESH LOOKUP, OUTSIDE THE SCENE BLOCK THE REST OF THIS SECTION IS IN. The name sceneMeshes_ is
    // historical: the table is the CONTENT INDEX's mesh id -> handle map, keyed by
    // fnv1a64(relative path), and nothing about a lookup in it needs an entity world. Two editor
    // callers prove it -- the foliage loader (guarded on AVER_MODULE_LANDSCAPE) asks whether a
    // species' mesh is loaded before accepting it, and Add > Primitive asks the same question about
    // a built-in -- and `scene-off` and `all-off` both failed on those two lines, which is how this
    // was found at all.
    //
    // WITH NO SCENE THE TABLE IS SIMPLY EMPTY, because what FILLS it (loadProjectMeshes, the .ocmesh
    // upload path) stays behind the guard. meshFor then returns 0 for everything, which is exactly
    // the answer both call sites already handle and already have a sentence for -- "not loaded --
    // skipping", "no built-in mesh registered". A degraded lookup, not a compile error.
    rhi::MeshHandle meshFor(u64 id) const;

#if AVER_MODULE_SCENE
    // Uploads the built-in primitives a .ocworld may name. Call once, before any project meshes.
    void registerBuiltins(rhi::IDevice& device);

    // One material-slot's worth of a mesh split for naming more than one. Ported from
    // SandboxApp::MeshPart (sandbox/src/SandboxApp.hpp) -- same two fields, same "0 means the slot
    // named nothing, ask the entity instead" convention buildMeshParts() (GameContent.cpp) and
    // GameRender.cpp's planEntityDraws() both read. Unlike the editor's copy, not gated behind
    // AVER_MODULE_LANDSCAPE: this class has no landscape dependency to inherit, and nothing about a
    // material-slot split is landscape-specific.
    struct MeshPart {
        rhi::MeshHandle mesh = 0;
        i32             material = 0;
    };

    // Uploads every .ocmesh under the project's content root.
    //
    // Takes an IDevice and not an Engine: a content cache with a handle on the whole engine is how
    // the SandboxApp god object started.
    void loadProjectMeshes(rhi::IDevice& device);

    // What loadProjectMeshes or registerBuiltins just uploaded, handed to a host that builds more
    // from the same data -- the editor's pick triangles, triangle counts and LOD ladder -- without
    // reading the file again. Called once per mesh, after its split parts are built. `data` is null
    // for a built-in, which has no .ocmesh.
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

    // Whether loadProjectMeshes uploads one coarser LOD per mesh as its depth-pass stand-in
    // (depthProxyMap). On by default; off for a host that uploads the whole LOD ladder itself and
    // answers the depth passes from that.
    void setBuildDepthProxies(bool on) { buildDepthProxies_ = on; }

    // A mesh uploaded elsewhere, registered under `id` (the editor's --skin-scene-test).
    void registerMesh(u64 id, rhi::MeshHandle handle, const std::pair<Vec3, Vec3>& bounds);

    // The ids loadProjectMeshes loaded, in load order.
    const std::vector<u64>& projectMeshIds() const { return projectMeshIds_; }

    // The per-material split for a mesh with more than one submesh, or nullptr for a mesh that was
    // never split -- either it names one material slot (the common case), or every submesh past the
    // first was refused (buildMeshParts' "ONE SURVIVING PART IS NOT A SPLIT" rule, GameContent.cpp).
    // GameRender.cpp's draw walk is the sole reader: it plans one draw per part instead of one draw
    // for the whole mesh whenever this returns non-null.
    const std::vector<MeshPart>* partsFor(u64 id) const;

    // The split above RE-CUT OVER A POSED COPY: one IDevice::createPosedPartMesh per part,
    // index-for-index with partsFor(id) (same materials; mesh 0 where the base part is 0). Built once
    // per posed handle on first ask, and cached. nullptr -- cached too -- when the mesh has no split,
    // carries no skin streams, the posed copy was not cut from THIS upload of `baseMesh` (its index
    // buffer differs, e.g. across an editor mesh reload), or the device refuses (D3D11). A null answer
    // means "keep the single whole-mesh draw", which is exactly the behaviour before this existed.
    //
    // WHY: a skinned entity draws its POSED copy, a different handle from the one partsFor's split was
    // cut from, so it used to draw as ONE mesh under the entity's own material -- a character whose
    // hair cards are their own material slot never drew them with the hair material.
    const std::vector<MeshPart>* posedPartsFor(rhi::IDevice& device, u64 id,
                                               rhi::MeshHandle baseMesh, rhi::MeshHandle posedMesh);

    // Forgets every project mesh, destroying its split parts and its depth proxy. The base handle is
    // destroyed too unless `destroyBaseHandles` is false, which only forgets it: the editor's mesh
    // reload passes false, because caches keyed by MeshHandle (its depth proxies and LOD ladders among
    // them) are not cleared with the meshes, and a destroyed handle's number can be reused. Built-ins
    // survive. The packaged game never reloads, so it never calls this.
    void releaseProjectMeshes(rhi::IDevice& device, bool destroyBaseHandles = true);

    usize meshCount() const { return sceneMeshes_.size(); }
    usize projectMeshCount() const { return projectMeshIds_.size(); }

    // Bounds as loaded from the .ocmesh, or nullptr. Used by the draw walk to cull.
    const std::pair<Vec3, Vec3>* boundsFor(u64 id) const;

    // A collision-only triangle mesh for `id`, lazily read from its .ocmesh and cached on first ask
    // (see GameContent.cpp for exactly how the LOD is picked and the mesh compacted). `positions` is
    // LOCAL space, 3 f32 per vertex; `indices` is 3 per triangle into `positions`, remapped down to
    // only the vertices this LOD actually references -- NOT LOD 0's full vertex array, which a
    // coarser level shares but mostly does not touch. `lod` is which stored level this came from (0
    // is the mesh's finest, i.e. `OcMeshData::indices`); `errorCm` is that level's own geometric
    // error, in centimetres (0 for LOD 0, by the format's own convention).
    //
    // nullptr for a mesh with no .ocmesh at all -- a built-in (registerBuiltins never indexes one)
    // or an unknown id -- or one whose file failed to load. EITHER answer is cached, so a bad id is
    // stat'd/read at most once no matter how many placements name it. A caller getting nullptr keeps
    // colliding that mesh as the fitted box (world::addStaticBoxBody) -- this function never falls
    // back to a box itself, because it has no box to fall back to; the caller does.
    struct CollisionMesh {
        std::vector<f32> positions;
        std::vector<u32> indices;
        u32 lod = 0;
        f32 errorCm = 0.0f;
    };
    const CollisionMesh* collisionMeshFor(u64 id);

    // Depth proxy map for LOD-based shadow/voxel optimization.
    const std::unordered_map<rhi::MeshHandle, rhi::MeshHandle>& depthProxyMap() const { return depthProxyMap_; }

    // Resolver for aver::render::SkinnedScene. Deliberately the SAME table the draw pass reads: a
    // skin target built from a different upload than the one on screen would be a rig skinning
    // geometry nobody can see.
    static rhi::MeshHandle resolveSceneMesh(u64 id, void* user);

    // The named surfaces gameplay can ask for, by interned material token.
    struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };
    const SurfaceLook* lookFor(i32 material) const;
#endif

#if AVER_MODULE_PARTICLES
    // Loads every .ocparticle under the project's content root into particles::particleEffects(),
    // keyed by fnv1a64(relative path) -- the SAME id space contentIndex_ already uses for every other
    // project asset (adopt()'s own "FROZEN" comment), so a CParticleEmitter::effect a level or a
    // script names resolves the identical way a CMeshRenderer::mesh or CAnimator::clip does. Recursive
    // over the whole content root, matching loadProjectMeshes rather than the Materials-folder
    // convention .ocmat follows: DECIDED 3 gave .ocparticle no such folder rule.
    //
    // particles::particleEffects() is the SAME process-global table SandboxApp.cpp's
    // loadProjectParticleEffects() fills and --particle-test's hardcoded content calls set() on
    // directly -- there is no GameContent-owned cache to keep in sync, matching resolveAnimAsset's
    // relationship to aver::anim::animSystem() one block up.
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
    // Depth proxy map: LOD meshes used instead of full detail in depth passes
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxyMap_;
    // mesh id -> its per-material split, for a mesh whose .ocmesh names more than one. See MeshPart.
    std::unordered_map<u64, std::vector<MeshPart>> meshParts_;
    // mesh id -> per part, index-for-index with meshParts_[id]: that part's slice of md.indices
    // UNREMAPPED, i.e. in the base mesh's vertex numbering, which a skin target shares verbatim.
    // Kept only for a mesh with skin streams (md.hasSkin()), the only kind SkinnedScene poses. The
    // parts themselves are compacted and renumbered, so their own indices cannot be reused over a
    // posed buffer -- this is the one piece of information the split used to throw away.
    std::unordered_map<u64, std::vector<std::vector<u32>>> meshPartBaseIndices_;
    // posed MeshHandle -> its posed parts. `parts` empty = refused, cached so it is not retried every
    // frame. Keyed by handle: RHI handles are never recycled (IDevice::destroyMesh's contract).
    struct PosedParts { u64 meshId = 0; std::vector<MeshPart> parts; };
    std::unordered_map<rhi::MeshHandle, PosedParts> posedParts_;
    // collisionMeshFor's cache: mesh id -> its collision mesh, or a present key holding a null
    // pointer for "asked for and there is none" (missing file, load failure, or a built-in) -- see
    // that function's own comment for why a negative result is cached too. Cleared alongside
    // meshBounds_/meshSlot0Material_ in releaseProjectMeshes: it is keyed by the same mesh ids and a
    // reload can put a different .ocmesh behind one of them.
    std::unordered_map<u64, std::unique_ptr<CollisionMesh>> collisionMeshCache_;
    MeshLoadedFn meshLoaded_ = nullptr;
    void* meshLoadedUser_ = nullptr;
    bool buildDepthProxies_ = true;

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
                         const std::vector<rhi::MeshVertex>& verts, const std::string& rel);
#endif

#if AVER_MODULE_PBR
    rhi::IResourceFactory* textureFactory_ = nullptr;
    std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;

    // Turns an .ocmat's GRAPHREF path into the id materialForSurface() stores in
    // MaterialDesc::graphId. Ported from SandboxApp::resolveMaterialGraph
    // (sandbox/src/SandboxAssets.cpp): same content-relative resolution, same cache-by-compiled-path
    // through pbr::materialGraphs().idOf(), same compile-on-miss through fmt::loadOcgraph() +
    // pbr::materialGraphs().add(), same 0 (stock shading) fallback on any failure.
    u32 resolveMaterialGraph(const std::string& graphRef) const;
#endif
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;
#endif
};

} // namespace aver::game
