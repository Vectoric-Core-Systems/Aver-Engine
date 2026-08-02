#include "aver/game/GameContent.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <system_error>

#if AVER_MODULE_SCENE
#  include "aver/anim/AnimSystem.hpp"
#endif

namespace aver::game {

void GameContent::adopt(const fmt::ProjectDesc& project) {
    project_ = project;
    contentIndex_.clear();

    const std::string content = project_.contentDir();
    if (content.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(content, ec)) {
        AVER_WARN("[Content] the project's content root does not exist: {}", content);
        return;
    }

    // The error_code overload of increment, so an unreadable subdirectory ends the walk instead of
    // throwing out of it.
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(it->path(), content, ec).string();
        if (ec || rel.empty()) continue;
        // FROZEN: the id hashes the forward-slash spelling, matching C# Assets.ObjectIdOf. This is
        // the property that makes packaging nearly free -- stage-game.ps1 copies Content/ under a
        // new root and every id in every .ocworld and .ocmat is unchanged, because the ids were
        // never a function of where the project lives.
        for (char& c : rel) if (c == '\\') c = '/';
        contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
    }
    AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);

#if AVER_MODULE_SCENE
    // The anim system does its own file discovery through this and caches by id, so a re-index has
    // to drop what it cached or a moved asset keeps resolving to its old path. Safe because adopt()
    // is a project-adoption call and never a per-frame one.
    //
    // Guarded on SCENE rather than PBR, which is where SandboxApp has it: the animation system has
    // nothing to do with physically based rendering, and the original guard is the cross-
    // contamination this lift exists to stop copying forward.
    anim::animSystem().clear();
    anim::animSystem().setResolver(&GameContent::resolveAnimAsset, this);
#endif
}

std::string GameContent::pathFor(u64 id) const {
    const auto it = contentIndex_.find(id);
    return it == contentIndex_.end() ? std::string() : it->second;
}

std::string GameContent::resolveAnimAsset(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->pathFor(id) : std::string();
}

} // namespace aver::game
