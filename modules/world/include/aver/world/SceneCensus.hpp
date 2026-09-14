#pragma once
// What a loaded level actually put in the world, described so two different hosts can be compared.
//
// WHY THIS EXISTS. AverGame.exe was deleted in b262c73 because a second host "rendered a different
// subset of the scene than the editor", and nothing in the tree could notice -- there was no CI, no
// packaging test, and verify-payload.ps1 compares Sandbox.exe against Sandbox.exe. Restoring the
// executable without a check for that would be restoring the problem, so this is the check.
//
// A CENSUS, NOT A PIXEL DIFF, and that is a deliberate choice rather than a weaker substitute.
// Comparing frames between the two hosts would measure the wrong thing: the editor's 3D viewport is
// a sub-rect of its window with panels around it, so the two have different aspect ratios and
// therefore different projections; and their cameras differ by design (the editor frames the level
// on load, the game spawns at a PlayerStart). A pixel comparison would be dominated by framing and
// would fail for reasons that have nothing to do with divergence.
//
// What the removal commit actually complained about was the SUBSET -- placements that one host spawns
// and the other does not, materials one resolves and the other silently falls back on. Every field
// here is camera-independent, viewport-independent and chrome-independent, so a difference means a
// real difference in what was loaded.
//
// HEADER-ONLY and Core+Scene only, so both hosts can include it without either growing a dependency:
// Sandbox already links Aver.World, and so does Aver.Runtime.Game.
#include "aver/core/Types.hpp"

#if AVER_MODULE_SCENE
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
// For aver_scene_material_name: the census identifies a material by the NAME it was interned under,
// never by its token. See SceneCensus::pairHash for what that distinction cost to discover.
#include "aver/scene/scene_abi.h"
#endif

#include <string>
#include <unordered_set>

namespace aver::world {

struct SceneCensus {
    u32 entities        = 0;   // live entities in the world
    u32 meshRenderers   = 0;   // ... carrying a CMeshRenderer
    u32 visible         = 0;   // ... whose visible bit is set
    u32 distinctMeshes  = 0;
    u32 distinctMats    = 0;
    // AN ORDER-INDEPENDENT HASH of every (mesh, material) pair in the world. This is the field that
    // makes the census a real check rather than four counts that could coincide: two hosts can agree
    // on how MANY meshes and materials they resolved and still disagree about WHICH, which is
    // precisely the "different subset" failure. Summed rather than XORed, because XOR cancels a
    // duplicated pair and a level legitimately places the same mesh with the same material twice.
    //
    // BUILT FROM THE MATERIAL'S NAME, NEVER ITS TOKEN, and that distinction is the difference
    // between a gate and a false alarm. aver_scene_material() mints tokens sequentially in
    // INTERNING ORDER (SceneAbi.cpp: `table.size() + 1`), so the same material carries a different
    // token in each host purely because they register their built-ins and project materials in a
    // different order. Hashing the token made two hosts that had loaded identical content report
    // different hashes -- measured, on PTTest, with every count matching. The mesh id is safe as-is:
    // it is fnv1a64 of the content-relative path, so it is derived from the content itself.
    u64 pairHash        = 0;
};

#if AVER_MODULE_SCENE
// Walks the live world. Takes World& rather than reading the singleton itself so a test can drive it.
inline SceneCensus takeSceneCensus(scene::World& w) {
    SceneCensus c;
    std::unordered_set<u64> meshes;
    std::unordered_set<std::string> mats;   // NAMES, not tokens -- see pairHash's comment
    c.entities = w.count();
    for (u32 i = 0; i < w.count(); ++i) {
        const scene::Entity e = w.at(i);
        if (!w.valid(e)) continue;
        const scene::CMeshRenderer* mr =
            w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        if (!mr) continue;
        ++c.meshRenderers;
        if (mr->flags & scene::kMeshRendererVisible) ++c.visible;
        const char* matName = aver_scene_material_name(mr->material);
        if (!matName) matName = "";
        meshes.insert(mr->mesh);
        mats.insert(matName);
        // FNV-1a over the pair, then summed. Stable across runs AND across hosts, because both
        // halves are derived from content: the mesh id is fnv1a64 of its content-relative path, and
        // the material is hashed by name.
        u64 h = 1469598103934665603ull;
        const auto mix8 = [&h](u64 v) {
            for (int b = 0; b < 8; ++b) { h ^= (v >> (b * 8)) & 0xff; h *= 1099511628211ull; }
        };
        mix8(mr->mesh);
        for (const char* p = matName; *p; ++p) {
            h ^= static_cast<u8>(*p);
            h *= 1099511628211ull;
        }
        c.pairHash += h;
    }
    c.distinctMeshes = static_cast<u32>(meshes.size());
    c.distinctMats   = static_cast<u32>(mats.size());
    return c;
}
#endif

// ONE LINE, ONE FORMAT, parsed by scripts/verify-game.ps1. Both hosts print exactly this, so the
// comparison is a string compare and there is no second definition of the format to drift.
inline std::string formatSceneCensus(const SceneCensus& c) {
    return "entities=" + std::to_string(c.entities) +
           " meshRenderers=" + std::to_string(c.meshRenderers) +
           " visible=" + std::to_string(c.visible) +
           " meshes=" + std::to_string(c.distinctMeshes) +
           " materials=" + std::to_string(c.distinctMats) +
           " pairHash=" + std::to_string(c.pairHash);
}

} // namespace aver::world
