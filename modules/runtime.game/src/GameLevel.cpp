#include "aver/game/GameLevel.hpp"

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <system_error>

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"
#  include "aver/scene/scene_abi.h"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

namespace aver::game {

#if AVER_MODULE_SCENE
namespace {

// Builds a quaternion from (roll, pitch, yaw) degrees as Rz * Ry * Rx.
//
// Copied from SandboxApp.cpp:168 rather than shared, for the reason GameMath.hpp gives. The
// MULTIPLICATION ORDER IS THE CONTRACT: this is the exact inverse of the editor's euler extraction,
// and any level authored by the editor is only reproduced by a game that composes the same way
// round. Getting it backwards yaws things that should roll, which looks like bad authoring rather
// than like a bug in the loader.
Quat quatFromEulerDeg(const Vec3& e) {
    return (Quat::fromAxisAngle({0,0,1}, radians(e.z)) * Quat::fromAxisAngle({0,1,0}, radians(e.y)) *
            Quat::fromAxisAngle({1,0,0}, radians(e.x))).normalized();
}

} // namespace

void GameLevel::load(const std::string& path, GameContent& content) {
    unload();

    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }

    scene::World& world = scene::World::instance();
    for (const fmt::OcWorldPlacement& p : w.placements) {
        Transform xf;
        xf.position = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
        xf.rotation = quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch),
                                            static_cast<f32>(p.yaw)});
        xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};

        // The entity's CName IS the asset path. That is the editor's convention and the draw walk
        // does not depend on it, but a level loaded by the game and by the editor must produce the
        // same names or anything that looks an entity up by name diverges between the two.
        const scene::Entity e = world.create(p.asset, scene::kInvalidEntity, xf);
        if (e == scene::kInvalidEntity) continue;

        auto* mr = static_cast<scene::CMeshRenderer*>(world.addComponent(e, scene::kComponentMeshRenderer));
        if (mr) {
            mr->mesh = p.objectId;
            mr->material = p.material.empty() ? 0 : aver_scene_material(0, p.material.c_str());
            mr->flags |= scene::kMeshRendererVisible;
#if AVER_MODULE_PBR
            // Bind the authored material, if the project has one for this surface token. Done at
            // load rather than per draw because materialForSurface stats up to three paths on a
            // miss and caches the negative -- per frame that would be a filesystem hit per entity.
            if (mr->material) {
                const pbr::MaterialHandle h = content.materialForSurface(p.material);
                if (h) content.bindSurfaceMaterial(mr->material, h);
            }
#endif
        }
        levelEntities_.push_back(e);

#if AVER_MODULE_PHYSICS
        // Lifted verbatim, and a no-op until something calls aver_phys_init: the branch already
        // tests aver_phys_ready(), so it costs nothing to have it here before C9 wires physics up.
        if (p.collide && aver_phys_ready()) {
            const int32_t body = aver_phys_add_static_box(
                static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z),
                static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz));
            levelBodies_.push_back(body);
        }
#endif
    }

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
