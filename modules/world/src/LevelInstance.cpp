#include "aver/world/LevelInstance.hpp"
#include "aver/core/Log.hpp"

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

    // PLACEMENT INDEX -> ENTITY, so a CHILD can name its parent. out.entities is NOT indexed by
    // placement -- class placements are skipped below -- so a second array is the only honest way to
    // answer "which entity is placement 7".
    //
    // A CHILD'S PARENT ALWAYS PRECEDES IT. parseOcworld only accepts a CHILD inside an open BEGIN
    // scope, and a scope can only be opened on a placement already read, so the parent index is
    // strictly less than the child's. That is what lets this be ONE forward pass instead of a
    // create-everything-then-reparent pass, and it is worth stating because the invariant is the
    // file format's, not this loop's.
    std::vector<scene::Entity> entityFor(w.placements.size(), scene::kInvalidEntity);
    // World-space transform per placement, accumulated down the chain. Physics bodies are placed in
    // WORLD coordinates while a child's authored transform is PARENT-RELATIVE, so a body built from
    // the authored numbers would sit at the child's local offset from the origin. That is silent --
    // the mesh draws correctly through the scene graph and only the collision is wrong.
    std::vector<Transform> worldXf(w.placements.size());

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
        //
        // ROOTS ONLY. `snap` reads Z as an offset above the TERRAIN, which is a world quantity,
        // while a child's Z is an offset from its parent -- adding a ground height to that mixes two
        // frames and puts the child somewhere neither the author nor the terrain asked for. A child
        // that asks to snap is told rather than silently obeyed or silently ignored.
        f64 pz = p.z;
        const bool isChild = p.parent >= 0 && p.parent < static_cast<i32>(w.placements.size());
        if (p.snapToGround && !isChild && opt.groundHeightAt) {
            f64 ground = 0.0;
            if (opt.groundHeightAt(p.x, p.y, ground)) pz = ground + p.z;
        } else if (p.snapToGround && isChild) {
            AVER_WARN("[Level] placement '{}' is a CHILD and asks to snap to ground; ignored -- its Z "
                      "is measured from its parent, not from the terrain", p.asset);
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
        // THE PARENT, IF ITS ENTITY EXISTS. A child of a CLASS placement finds kInvalidEntity here,
        // because this loop deliberately skips class placements and the host spawns them in a later
        // pass -- so it loads as a root, at its parent-relative transform, and says so rather than
        // being quietly misplaced with no record of why.
        scene::Entity parentEnt = scene::kInvalidEntity;
        if (isChild) {
            parentEnt = entityFor[static_cast<usize>(p.parent)];
            if (parentEnt == scene::kInvalidEntity)
                AVER_WARN("[Level] placement '{}' names a parent that produced no entity (a class "
                          "placement, or one that failed to create); loading it as a root",
                          p.asset);
        }

        // WORLD TRANSFORM, for the physics body below. Composed from the parent's, which is already
        // final because parents precede children.
        worldXf[i] = xf;
        if (isChild) {
            const Transform& pw = worldXf[static_cast<usize>(p.parent)];
            worldXf[i].scale    = Vec3{pw.scale.x * xf.scale.x, pw.scale.y * xf.scale.y,
                                       pw.scale.z * xf.scale.z};
            worldXf[i].rotation = pw.rotation * xf.rotation;
            const Vec3 scaled{xf.position.x * pw.scale.x, xf.position.y * pw.scale.y,
                              xf.position.z * pw.scale.z};
            worldXf[i].position = pw.position + pw.rotation.rotate(scaled);
        }

        const scene::Entity e = world.create(p.asset, parentEnt, xf);
        if (e == scene::kInvalidEntity) continue;
        entityFor[i] = e;

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
            // WORLD, NOT AUTHORED. For a root the two are the same and this is the line it always
            // was; for a child the authored numbers are parent-relative and using them would put the
            // collision somewhere the mesh is not.
            const Transform& wx = worldXf[i];
            body = aver_phys_add_static_box(
                wx.position.x, wx.position.y, wx.position.z,
                wx.scale.x, wx.scale.y, wx.scale.z);
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
