// GameContent: the project's asset index, and the resolvers that read it.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/rhi/RHI.hpp"

#include <string>
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

    usize size() const { return contentIndex_.size(); }
    const fmt::ProjectDesc& project() const { return project_; }

    // Resolver for aver::anim::AnimSystem, which takes a plain function pointer: asset discovery is
    // the host's business, not the sampler's. `user` is a GameContent*.
    static std::string resolveAnimAsset(u64 id, void* user);

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

    // Resolver for aver::render::SkinnedScene. Deliberately the SAME table the draw pass reads: a
    // skin target built from a different upload than the one on screen would be a rig skinning
    // geometry nobody can see.
    static rhi::MeshHandle resolveSceneMesh(u64 id, void* user);

    // The named surfaces gameplay can ask for, by interned material token.
    struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };
    const SurfaceLook* lookFor(i32 material) const;
#endif

private:
    fmt::ProjectDesc project_;
    std::unordered_map<u64, std::string> contentIndex_;

#if AVER_MODULE_SCENE
    std::unordered_map<u64, rhi::MeshHandle>       sceneMeshes_;
    std::unordered_map<u64, std::pair<Vec3, Vec3>> meshBounds_;
    std::vector<u64>                               projectMeshIds_;
    std::unordered_map<i32, SurfaceLook>           surfaceLooks_;
#endif
};

} // namespace aver::game
