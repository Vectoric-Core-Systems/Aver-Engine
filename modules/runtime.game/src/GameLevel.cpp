#include "aver/game/GameLevel.hpp"

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <system_error>

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/world/LevelInstance.hpp"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

namespace aver::game {

#if AVER_MODULE_SCENE

// quatFromEulerDeg was here, as a file-static copied out of SandboxApp.cpp. It is now
// aver::world::quatFromEulerDeg in modules/world/include/aver/world/LevelTransform.hpp, reached
// through LevelInstance.hpp, and the editor re-exports the same definition -- so the rotation
// contract the comment here used to insist on is now enforced by there being one of it.

void GameLevel::load(const std::string& path, GameContent& content) {
    unload();

    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }

    // The placement loop is aver::world::instantiate now, shared with the editor. What is left here
    // is the part that is genuinely the GAME's: which material cache to bind into, and what to keep.
    world::InstantiateOptions opt;
#if AVER_MODULE_PBR
    // Bind the authored material, if the project has one for this surface token. Done at load rather
    // than per draw because materialForSurface stats up to three paths on a miss and caches the
    // negative -- per frame that would be a filesystem hit per entity.
    opt.bindMaterial = [&content](i32 token, const std::string& surface) {
        const pbr::MaterialHandle h = content.materialForSurface(surface);
        if (h) content.bindSurfaceMaterial(token, h);
    };
#else
    (void)content;
#endif

    const world::LevelInstance inst = world::instantiate(w, opt);
    levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif

    if (w.hasFog) {
        levelFog_    = static_cast<f32>(w.fogDensity);
        fogColor_[0] = static_cast<f32>(w.fogColor[0]);
        fogColor_[1] = static_cast<f32>(w.fogColor[1]);
        fogColor_[2] = static_cast<f32>(w.fogColor[2]);
        hasLevelFog_ = true;
    }
    // applyLevelSky is C6's business: it writes rhi::SkyAtmosphere, which the game does not push to
    // the device until the camera commit lands.

    // ---- the level's declared density fields ----
    //
    // Resolved here, once, rather than every time something samples one: turning an authored record
    // into a spec is cheap but it is not free, and a raymarch asking per pixel would pay for it a
    // million times.
    pcgFields_.clear();
    for (const fmt::OcPcgVolume& v : w.pcgVolumes) {
        PcgField f;
        f.name = v.name;
        f.infinite = v.infinite;

        // fBm layers derived from the octave count, matching Aver.Pcg's fbmLayers so a field
        // authored in a level and one built in F# from the same numbers agree.
        const u32 layers = static_cast<u32>(v.octaves) < pcg::kMaxLayers
                         ? static_cast<u32>(v.octaves) : pcg::kMaxLayers;
        pcg::NoiseLayer ls[pcg::kMaxLayers]{};
        for (u32 i = 0; i < layers; ++i) {
            ls[i].frequency  = std::pow(2.0f, static_cast<f32>(i));
            ls[i].amplitude  = 1.0f / std::pow(2.0f, static_cast<f32>(i));
            ls[i].octaves    = 4;
            ls[i].lacunarity = 2.0f;
            ls[i].gain       = 0.5f;
            ls[i].seedOffset = static_cast<i32>(i + 1) * 7919;
        }

        if (v.infinite) {
            f.infiniteSpec.seed = v.seed;
            f.infiniteSpec.layerCount = layers;
            for (u32 i = 0; i < layers; ++i) f.infiniteSpec.layers[i] = ls[i];
            f.infiniteSpec.cellSizeCm    = static_cast<f32>(v.cellSizeCm);
            f.infiniteSpec.coverageFloor = static_cast<f32>(v.coverageFloor);
            f.infiniteSpec.coverageBias  = static_cast<f32>(v.coverageBias);
        } else {
            f.boundsMin = Vec3{static_cast<f32>(v.boundsMin[0]), static_cast<f32>(v.boundsMin[1]),
                               static_cast<f32>(v.boundsMin[2])};
            f.boundsMax = Vec3{static_cast<f32>(v.boundsMax[0]), static_cast<f32>(v.boundsMax[1]),
                               static_cast<f32>(v.boundsMax[2])};
            // Resolution from the bounds and the cell size, so `cell` means the same thing in both
            // cases: one lattice cell spans that much world. Clamped to something a GPU will
            // actually allocate -- a 100 km volume at 16 m cells is 6250^3 voxels.
            const f32 c = v.cellSizeCm > 0.0 ? static_cast<f32>(v.cellSizeCm) : 1600.0f;
            auto res = [c](f32 lo, f32 hi) -> u32 {
                const f32 n = (hi - lo) / c;
                const i32 i = static_cast<i32>(n < 1.0f ? 1.0f : n);
                return static_cast<u32>(i > 256 ? 256 : i);
            };
            f.boundedSpec.resX = res(f.boundsMin.x, f.boundsMax.x);
            f.boundedSpec.resY = res(f.boundsMin.y, f.boundsMax.y);
            f.boundedSpec.resZ = res(f.boundsMin.z, f.boundsMax.z);
            f.boundedSpec.seed = v.seed;
            f.boundedSpec.layerCount = layers;
            for (u32 i = 0; i < layers; ++i) f.boundedSpec.layers[i] = ls[i];
            f.boundedSpec.coverageFloor = static_cast<f32>(v.coverageFloor);
            f.boundedSpec.coverageBias  = static_cast<f32>(v.coverageBias);
        }
        pcgFields_.push_back(std::move(f));
    }
    for (const PcgField& f : pcgFields_) {
        if (f.infinite)
            AVER_INFO("[PCG] level field '{}': INFINITE, seed {}, {} cm cells, {} layer(s)",
                      f.name, f.infiniteSpec.seed, f.infiniteSpec.cellSizeCm, f.infiniteSpec.layerCount);
        else
            AVER_INFO("[PCG] level field '{}': bounded {}x{}x{}, seed {}, {} layer(s)",
                      f.name, f.boundedSpec.resX, f.boundedSpec.resY, f.boundedSpec.resZ,
                      f.boundedSpec.seed, f.boundedSpec.layerCount);
    }

    levelPath_ = path;
    levelName_ = w.name;
    AVER_INFO("[Level] '{}' loaded from {} ({} placement(s))", w.name, path, w.placements.size());
#if AVER_MODULE_PHYSICS
    // One static body per COLLIDING placement, so this is checkable against the map file itself:
    // it must equal the count of PLACE lines without `nocollide`. Reported even when zero, because
    // zero bodies with colliding placements means physics was not ready at load time -- the exact
    // ordering bug initPhysics-before-openProject exists to prevent.
    AVER_INFO("[Level] {} static physics body(ies) from {} placement(s)",
              levelBodies_.size(), w.placements.size());
#endif
}

void GameLevel::loadStartMap(const fmt::ProjectDesc& project, GameContent& content) {
    if (!project.valid() || project.startMap.empty()) return;
    const std::string path = project.contentDir() + "\\" + project.startMap;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        AVER_INFO("[Level] start map '{}' does not exist yet - the world starts empty", project.startMap);
        levelName_ = std::filesystem::path(project.startMap).stem().string();
        levelPath_ = path;
        return;
    }
    load(path, content);
}

const GameLevel::PcgField* GameLevel::pcgField(const std::string& name) const {
    for (const PcgField& f : pcgFields_) if (f.name == name) return &f;
    return nullptr;
}

void GameLevel::unload() {
    scene::World& world = scene::World::instance();
    for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
    levelEntities_.clear();
#if AVER_MODULE_PHYSICS
    for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
    levelBodies_.clear();
#endif
    hasLevelFog_ = false;
    pcgFields_.clear();
    levelPath_.clear();
    levelName_.clear();
}
#endif // AVER_MODULE_SCENE

} // namespace aver::game
