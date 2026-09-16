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
    void releaseProjectMaterials();
#endif

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    // Remembers that an interned surface token has an authored material behind it.
    void bindSurfaceMaterial(i32 token, pbr::MaterialHandle h) { surfaceMaterials_[token] = h; }
    pbr::MaterialHandle authoredFor(i32 token) const;

    /// The material token this mesh's own materialSlots[0] names, or 0 when it names none.
    ///
    /// THE EDITOR'S RULE, HELD HERE TOO, and it has to be: a fallback the editor honours and the
    /// packaged game does not is this repo's most-repeated defect shape, and the divergence gate in
    /// scripts/verify-game.ps1 exists because of it. An entity whose CMeshRenderer.material is 0 is
    /// not saying "draw me flat", it is saying nothing -- and the mesh it points at already declares
    /// a material. A non-zero material stays an override, exactly as before.
    i32 meshDefaultMaterial(u64 meshId) const;
#endif

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
    // Takes an IDevice and not an Engine: the editor's version takes Engine& and uses it for
    // nothing but e.device()->createMesh, and a content cache with a handle on the whole engine is
    // how the SandboxApp god object started.
    void loadProjectMeshes(rhi::IDevice& device);

    // The per-material split for a mesh with more than one submesh, or nullptr for a mesh that was
    // never split -- either it names one material slot (the common case), or every submesh past the
    // first was refused (buildMeshParts' "ONE SURVIVING PART IS NOT A SPLIT" rule, GameContent.cpp).
    // GameRender.cpp's draw walk is the sole reader: it plans one draw per part instead of one draw
    // for the whole mesh whenever this returns non-null.
    const std::vector<MeshPart>* partsFor(u64 id) const;

    // THERE IS DELIBERATELY NO releaseProjectMeshes() TWIN of releaseProjectMaterials().
    // There was one, it had zero callers, and it was wrong: it erased sceneMeshes_/meshBounds_/
    // meshSlot0Material_ entries without ever calling IDevice::destroyMesh on the handles they held,
    // so the first caller to wire it up would have leaked the GPU vertex/index buffers (and any BLAS
    // built from them) instead of freeing them. It could not have done otherwise -- it took no
    // device, and this class only gets one as an argument to loadProjectMeshes.
    //
    // Nothing needs it today: openProject runs exactly once per process in the packaged game, so the
    // device's own teardown reclaims everything. Whoever adds a project-reload path should write the
    // correct version then -- taking rhi::IDevice&, and destroying before erasing every handle this
    // class owns per mesh: the base handle, its meshParts_ split, and its depthProxyMap_ level. Do not
    // copy the editor's SandboxApp::releaseProjectMeshes for this: it erases sceneMeshes_ without
    // destroying the base handle.

    rhi::MeshHandle meshFor(u64 id) const;
    usize meshCount() const { return sceneMeshes_.size(); }
    usize projectMeshCount() const { return projectMeshIds_.size(); }

    // Bounds as loaded from the .ocmesh, or nullptr. Used by the draw walk to cull.
    const std::pair<Vec3, Vec3>* boundsFor(u64 id) const;

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

#if AVER_MODULE_SCENE
    std::unordered_map<u64, rhi::MeshHandle>       sceneMeshes_;
    std::unordered_map<u64, std::pair<Vec3, Vec3>> meshBounds_;
    // mesh id -> the material token its materialSlots[0] names. See meshDefaultMaterial.
    std::unordered_map<u64, i32>                   meshSlot0Material_;
    std::vector<u64>                               projectMeshIds_;
    std::unordered_map<i32, SurfaceLook>           surfaceLooks_;
    // Depth proxy map: LOD meshes used instead of full detail in depth passes
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxyMap_;
    // mesh id -> its per-material split, for a mesh whose .ocmesh names more than one. See MeshPart.
    std::unordered_map<u64, std::vector<MeshPart>> meshParts_;

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
