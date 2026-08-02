// GameContent: the project's asset index, and the resolvers that read it.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/formats/OcProject.hpp"

#include <string>
#include <unordered_map>

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

private:
    fmt::ProjectDesc project_;
    std::unordered_map<u64, std::string> contentIndex_;
};

} // namespace aver::game
