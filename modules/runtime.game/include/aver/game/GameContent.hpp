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

    // Loads every .ocmat under Content\Materials. NON-RECURSIVE, matching the editor.
    void loadProjectMaterials();
    void releaseProjectMaterials();
#endif

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    // Remembers that an interned surface token has an authored material behind it.
    void bindSurfaceMaterial(i32 token, pbr::MaterialHandle h) { surfaceMaterials_[token] = h; }
    pbr::MaterialHandle authoredFor(i32 token) const;
#endif

#if AVER_MODULE_SCENE
    // Uploads the built-in primitives a .ocworld may name. Call once, before any project meshes.
    void registerBuiltins(rhi::IDevice& device);

    // Uploads every .ocmesh under the project's content root.
    //
    // Takes an IDevice and not an Engine: the editor's version takes Engine& and uses it for
    // nothing but e.device()->createMesh, and a content cache with a handle on the whole engine is
    // how the SandboxApp god object started.
    void loadProjectMeshes(rhi::IDevice& device);

    // Drops the project's meshes from the id table. The built-in primitives survive, which is why
    // they are tracked separately.
    void releaseProjectMeshes();

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
    // over the whole content root, matching loadProjectMeshes rather than loadProjectMaterials'
    // Content\Materials convention: DECIDED 3 gave .ocparticle no such folder rule.
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
    std::vector<u64>                               projectMeshIds_;
    std::unordered_map<i32, SurfaceLook>           surfaceLooks_;
    // Depth proxy map: LOD meshes used instead of full detail in depth passes
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxyMap_;
#endif

#if AVER_MODULE_PBR
    rhi::IResourceFactory* textureFactory_ = nullptr;
    std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;
#endif
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;
#endif
};

} // namespace aver::game
