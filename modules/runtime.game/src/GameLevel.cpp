#include "aver/game/GameLevel.hpp"

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"

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

void GameLevel::unload() {
    scene::World& world = scene::World::instance();
    for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
    levelEntities_.clear();
#if AVER_MODULE_PHYSICS
    for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
    levelBodies_.clear();
#endif
    hasLevelFog_ = false;
    levelPath_.clear();
    levelName_.clear();
}
#endif // AVER_MODULE_SCENE

} // namespace aver::game
