#include "aver/world/LevelInstance.hpp"

#if AVER_MODULE_SCENE

#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"

#  if AVER_MODULE_PHYSICS
#    include "aver/physics/physics_abi.h"
#  endif

namespace aver::world {

LevelInstance instantiate(const fmt::OcWorldData& w, const InstantiateOptions& opt) {
    LevelInstance out;
    out.entities.reserve(w.placements.size());
    out.placementIndex.reserve(w.placements.size());
    out.entityBody.reserve(w.placements.size());

    scene::World& world = scene::World::instance();

    for (usize i = 0; i < w.placements.size(); ++i) {
        const fmt::OcWorldPlacement& p = w.placements[i];

        // A CLASS INSTANCE, not an ordinary mesh placement -- see OcWorldPlacement::className's own
        // comment. Skipped here, FRAMEWORK-FREE (this module deliberately does not and should not
        // link Aver.Framework -- see this file's own CMakeLists.txt), so it produces no raw
        // world::create()/CMeshRenderer/static-body entity at all. The host that DOES link the
        // framework (GameLevel::load for the game, SandboxApp's own level-load path for the editor)
        // is what actually spawns it, through aver_fw_class_find/aver_fw_spawn, as a POST-PASS over
        // these same placements -- see that function's own comment for why a second pass rather than
        // a callback threaded through here. Without this `continue`, a class placement would get BOTH
        // a raw mesh/physics entity from this loop AND a real class instance from the post-pass,
        // silently overlapping.
        if (!p.className.empty()) continue;

        // THE NARROWING LIVES HERE NOW, once. OcWorldData carries f64 but every consumer has always
        // narrowed at the point of use, in two separate copies of this loop -- so the f64 was never
        // observable and the two copies were free to drift apart. See docs/CHUNKS.md B8 for why the
        // f64 does not help anyway: the TEXT writer formats with %.6g, so a coordinate past ~10 km
        // loses centimetres on every save regardless of the type it is held in.
        // A placement that asked to sit on the ground gets its authored Z treated as an OFFSET
        // from the surface, not as an absolute height -- so `z 0 snap` means "on the ground" and
        // `z 50 snap` means "half a metre above it", and both survive the terrain being resculpted.
        // No callback, or no ground under this point, and the authored Z stands unchanged.
        f64 pz = p.z;
        if (p.snapToGround && opt.groundHeightAt) {
            f64 ground = 0.0;
            if (opt.groundHeightAt(p.x, p.y, ground)) pz = ground + p.z;
        }

        Transform xf;
        xf.position = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(pz)};
        xf.rotation = quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch),
                                            static_cast<f32>(p.yaw)});
        xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};

        // The entity's CName IS the asset path. That is the editor's convention and the draw walk
        // does not depend on it, but a level loaded by the game and by the editor must produce the
        // same names or anything that looks an entity up by name diverges between the two. That was
        // a real risk while this loop existed twice; it is structural now.
        const scene::Entity e = world.create(p.asset, scene::kInvalidEntity, xf);
        if (e == scene::kInvalidEntity) continue;

        auto* mr = static_cast<scene::CMeshRenderer*>(
            world.addComponent(e, scene::kComponentMeshRenderer));
        if (mr) {
            mr->mesh = p.objectId;
            mr->material = p.material.empty() ? 0 : aver_scene_material(0, p.material.c_str());
            mr->flags |= scene::kMeshRendererVisible;
            if (mr->material && opt.bindMaterial) opt.bindMaterial(mr->material, p.material);
        }

        out.entities.push_back(e);
        out.placementIndex.push_back(static_cast<u32>(i));

        i32 body = -1;
#  if AVER_MODULE_PHYSICS
        // aver_phys_ready() being false is not a failure: it is the state a host is in before it has
        // called aver_phys_init, and the count logged afterwards is what makes an
        // initPhysics-after-openProject ordering mistake visible instead of silent.
        if (opt.createBodies && p.collide && aver_phys_ready()) {
            body = aver_phys_add_static_box(
                static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(pz),
                static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz));
            // The one line that makes this placement's body IDENTIFIABLE later -- a raycast that
            // hits it can now report `e`, not just an opaque physics handle nothing else understands.
            if (body) aver_phys_set_entity(body, static_cast<i32>(e));
            out.bodies.push_back(body);
        }
#  else
        (void)opt;
#  endif
        out.entityBody.push_back(body);
    }

    return out;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
