#include "aver/world/LevelInstance.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <unordered_map>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

namespace aver::world {

namespace {

// Any LOCAL half-extent under this is a planar mesh -- a floor quad with zero thickness on that
// axis -- not a box with one very thin side. Compared against the LOCAL half-extent, before scale,
// so authoring a huge flat quad and scaling it up does not dodge the guard.
constexpr f32 kThinLocalHalfExtentCm = 1e-3f;

// The floor a thin axis gets scaled up to, so the box is not razor-thin. Named separately from
// kPlaceholderHalfExtentCm (LevelInstance.hpp): that one is a MESH's frozen bounds, this one is a
// WORLD-space minimum nothing authors, and the two happening to share a value is a coincidence, not
// a contract.
constexpr f32 kMinStaticHalfExtentCm = 1.0f;

// One axis of fitStaticBox's half-extents: the local half-extent scaled into world space, floored to
// kMinStaticHalfExtentCm when the axis was planar to begin with. `scaleAbs` is already abs()'d by the
// caller -- see fitStaticBox's own comment on why the sign belongs to centre, not here.
f32 scaledHalfExtent(f32 scaleAbs, f32 localHalf) {
    const f32 scaled = scaleAbs * localHalf;
    return localHalf < kThinLocalHalfExtentCm ? std::max(scaled, kMinStaticHalfExtentCm) : scaled;
}

} // namespace

StaticBoxFit fitStaticBox(const Transform& worldXf, const Vec3& localMin, const Vec3& localMax) {
    // A NON-FINITE OR INVERTED BOUND IS NOT A BOX. Falls back to the unit-cube placeholder's own
    // bounds -- the same box an unknown mesh has always collided as -- rather than building a box
    // from garbage or asserting on it.
    Vec3 lmin = localMin, lmax = localMax;
    const bool usable = std::isfinite(lmin.x) && std::isfinite(lmin.y) && std::isfinite(lmin.z) &&
                        std::isfinite(lmax.x) && std::isfinite(lmax.y) && std::isfinite(lmax.z) &&
                        lmin.x <= lmax.x && lmin.y <= lmax.y && lmin.z <= lmax.z;
    if (!usable) {
        lmin = Vec3{-kPlaceholderHalfExtentCm, -kPlaceholderHalfExtentCm, -kPlaceholderHalfExtentCm};
        lmax = Vec3{ kPlaceholderHalfExtentCm,  kPlaceholderHalfExtentCm,  kPlaceholderHalfExtentCm};
    }

    // lc/lh: the local box's own centre and half-extent, the same split every other bounds-consumer
    // in this codebase uses (see GameLevel.cpp's level-bounds walk).
    const Vec3 lc{(lmin.x + lmax.x) * 0.5f, (lmin.y + lmax.y) * 0.5f, (lmin.z + lmax.z) * 0.5f};
    const Vec3 lh{(lmax.x - lmin.x) * 0.5f, (lmax.y - lmin.y) * 0.5f, (lmax.z - lmin.z) * 0.5f};
    const Vec3& s = worldXf.scale;

    StaticBoxFit fit;
    // ABS ON THE HALF-EXTENT, NOT ON THE SCALE APPLIED TO THE CENTRE OFFSET: a negative scale mirrors
    // where the box's centre sits (below), but the box itself -- half-extents along its own axes --
    // has no sign to mirror. Getting this backwards would leave the unit cube's old behaviour (which
    // never noticed, because its centre offset is zero either way) unchanged while every off-centre
    // mesh got a half-extent that could go negative and get silently abs()'d back by the physics ABI.
    fit.halfExtents = Vec3{scaledHalfExtent(std::fabs(s.x), lh.x),
                           scaledHalfExtent(std::fabs(s.y), lh.y),
                           scaledHalfExtent(std::fabs(s.z), lh.z)};
    // The local centre, scaled (a negative scale DOES mirror an off-centre box through the pivot --
    // that is what a negative scale means) then rotated into world space. Zero for the unit cube on
    // every axis, which is exactly what makes the case below exact rather than approximate.
    fit.pivotToCentre = Vec3{s.x * lc.x, s.y * lc.y, s.z * lc.z};
    fit.centre = worldXf.position + worldXf.rotation.rotate(fit.pivotToCentre);
    fit.rotation = worldXf.rotation;
    return fit;
}

void scaleMeshForBody(const f32* localPositions, u32 vertexCount, const u32* localIndices,
                      u32 indexCount, const Vec3& scale,
                      std::vector<f32>& outPositions, std::vector<u32>& outIndices) {
    outPositions.resize(usize(vertexCount) * 3);
    for (u32 v = 0; v < vertexCount; ++v) {
        outPositions[usize(v) * 3 + 0] = localPositions[usize(v) * 3 + 0] * scale.x;
        outPositions[usize(v) * 3 + 1] = localPositions[usize(v) * 3 + 1] * scale.y;
        outPositions[usize(v) * 3 + 2] = localPositions[usize(v) * 3 + 2] * scale.z;
    }

    outIndices.assign(localIndices, localIndices + indexCount);
    // See this function's own header comment for why the sign of scale.x*scale.y*scale.z is exactly
    // the mirror test.
    if (scale.x * scale.y * scale.z < 0.0f)
        for (usize k = 0; k + 2 < outIndices.size(); k += 3)
            std::swap(outIndices[k + 1], outIndices[k + 2]);
}

#if AVER_MODULE_PHYSICS
i32 addStaticBoxBody(const Transform& worldXf, const Vec3& localMin, const Vec3& localMax) {
    const StaticBoxFit fit = fitStaticBox(worldXf, localMin, localMax);
    const i32 body = aver_phys_add_static_box(fit.centre.x, fit.centre.y, fit.centre.z,
                                              fit.halfExtents.x, fit.halfExtents.y, fit.halfExtents.z);
    if (!body) return body;
    // EXACTLY IDENTITY, not "close to": the placement that is not rotated at all is the overwhelming
    // common case (every level authored before rotation reached collision), and it must make the one
    // call aver_phys_add_static_box already is -- a second ABI call per placement that never needed
    // one is a cost this guard exists to avoid, not a correctness fix.
    const Quat& q = fit.rotation;
    if (!(q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f))
        aver_phys_body_set_rotation(body, q.x, q.y, q.z, q.w);
    return body;
}

i32 addStaticMeshBody(const Transform& worldXf, const f32* localPositions, u32 vertexCount,
                      const u32* indices, u32 indexCount) {
    if (!localPositions || !indices || vertexCount < 3 || indexCount < 3) return 0;

    std::vector<f32> positions;
    std::vector<u32> wound;
    scaleMeshForBody(localPositions, vertexCount, indices, indexCount, worldXf.scale, positions, wound);

    // aver_phys_add_mesh takes int32_t indices; a u32 index buffer reinterprets bit-for-bit, the same
    // cast modules/render.softbody/src/SoftBodyScene.cpp already makes for the identical ABI shape --
    // a real conversion would only matter if an index could exceed INT32_MAX, which nothing in this
    // engine's mesh formats can produce.
    const i32 body = aver_phys_add_mesh(positions.data(), static_cast<i32>(vertexCount),
                                        reinterpret_cast<const i32*>(wound.data()),
                                        static_cast<i32>(wound.size()),
                                        worldXf.position.x, worldXf.position.y, worldXf.position.z);
    if (!body) return body;
    // EXACTLY IDENTITY -- same guard, same reason, as addStaticBoxBody above.
    const Quat& q = worldXf.rotation;
    if (!(q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f))
        aver_phys_body_set_rotation(body, q.x, q.y, q.z, q.w);
    return body;
}

i32 addPivotBoxBody(const Transform& worldXf, const Vec3& localMin, const Vec3& localMax) {
    const StaticBoxFit fit = fitStaticBox(worldXf, localMin, localMax);
    // The box's eight corners around the pivot, in the placement's unrotated axes: the centre offset
    // plus or minus the half-extents, which is the hull the rotation below then turns.
    const Vec3& o = fit.pivotToCentre;
    const Vec3& h = fit.halfExtents;
    f32 corners[8 * 3];
    for (int k = 0; k < 8; ++k) {
        corners[k * 3 + 0] = o.x + ((k & 1) ? h.x : -h.x);
        corners[k * 3 + 1] = o.y + ((k & 2) ? h.y : -h.y);
        corners[k * 3 + 2] = o.z + ((k & 4) ? h.z : -h.z);
    }
    const i32 body = aver_phys_add_convex_hull(corners, 8, worldXf.position.x, worldXf.position.y,
                                               worldXf.position.z, /*dynamic*/ 0, 0.0f);
    if (!body) return body;
    // EXACTLY IDENTITY -- same guard, same reason, as addStaticBoxBody above.
    const Quat& q = fit.rotation;
    if (!(q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f))
        aver_phys_body_set_rotation(body, q.x, q.y, q.z, q.w);
    return body;
}
#endif

} // namespace aver::world

#if AVER_MODULE_SCENE

#  include "aver/core/Hash.hpp"
#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"

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
    // Per placement: it, or an ancestor of it, has an `anim` clip. A parent precedes its child, so the
    // parent's answer is final when the child asks.
    std::vector<char> movesWithClip(w.placements.size(), 0);

#if AVER_MODULE_PHYSICS
    // MESH ID -> the shared physics shape for it (this call's own, or the caller's opt.meshShapes), or 0 for "already asked and
    // Jolt refused" (cached too, so a mesh whose triangles Jolt cannot use is not retried once per
    // placement that names it). This call's own are released once every placement below has made its own body from it --
    // see the end of this function -- because a body's own ScaledShape keeps the shape alive by then.
    std::unordered_map<u64, i32> localShapes;
    std::unordered_map<u64, i32>& meshShapeCache = opt.meshShapes ? *opt.meshShapes : localShapes;
    // So a level of many animated placements that cannot be made kinematic logs one warning, not one each.
    bool warnedStaticAnimated = false;
#endif

    // done == total == 0 for an empty world never runs the loop below, so it is reported here once
    // rather than leaving a caller unable to tell "no callback yet" from "nothing to report".
    if (opt.progress && w.placements.empty()) opt.progress(0, 0);

    for (usize i = 0; i < w.placements.size(); ++i) {
        const fmt::OcWorldPlacement& p = w.placements[i];

        // PROGRESS, BEFORE THE CLASS-PLACEMENT `continue` BELOW so a level built mostly of class
        // placements still reports -- see InstantiateOptions::progress's own comment for why `done`
        // counts every placement walked rather than only the ones that produced a body.
        if (opt.progress) {
            const usize done = i + 1;
            if (done % 256 == 0 || done == w.placements.size()) opt.progress(done, w.placements.size());
        }

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
            // AUTHORED VISIBILITY, not an unconditional seed: addComponent hands back zero-filled
            // storage (see kMeshRendererHiddenFromOwner's own comment on that, Components.hpp), so
            // a placement that asked to be hidden must simply not OR the bit in, rather than being
            // set and then cleared -- the two read the same, but only one matches p.visible being
            // the single source of truth a save later reads back out of this same bit.
            if (p.visible) mr->flags |= scene::kMeshRendererVisible;
            if (mr->material && opt.bindMaterial) opt.bindMaterial(mr->material, p.material);
        }

        // OBJECT ANIMATION: the clip is named by content path, and its id is fnv1a64 of that path like
        // every other content id. addComponent hands back zero-filled storage, so every field is set.
        const bool animated = !p.animClip.empty();
        if (animated) {
            if (auto* an = static_cast<scene::CAnimator*>(world.addComponent(e, scene::kComponentAnimator))) {
                an->clip        = fnv1a64(std::string_view(p.animClip));
                an->time        = p.animTime;
                an->speed       = p.animSpeed;
                an->blendWeight = 1.0f;
                an->flags       = p.animOnce ? scene::kAnimatorOnce : 0u;
                // AN AUTHORED `animspeed 0` MEANS HELD at animtime. CAnimator reads a speed of 0 as 1
                // (a zero-filled component must play), so without this the level would play at full speed.
                if (p.animSpeed == 0.0f) an->flags |= scene::kAnimatorPaused;
            }
        }
        // THE SUBTREE OF AN ANIMATED PLACEMENT MOVES WITH IT, through the hierarchy: its colliding
        // descendants need kinematic bodies too, or they stay at the load pose while the mesh drives away.
        const bool parentMoves = isChild && parentEnt != scene::kInvalidEntity &&
                                 movesWithClip[static_cast<usize>(p.parent)];
        movesWithClip[i] = animated || parentMoves;

        out.entities.push_back(e);
        out.placementIndex.push_back(static_cast<u32>(i));

        i32 body = -1;
#  if AVER_MODULE_PHYSICS
        // aver_phys_ready() being false is not a failure: it is the state a host is in before it has
        // called aver_phys_init, and the count logged afterwards is what makes an
        // initPhysics-after-openProject ordering mistake visible instead of silent.
        //
        // A VEHICLE PLACEMENT GETS NO BODY HERE, collide or not: play builds its dynamic chassis
        // (VehicleSystem), and a static or kinematic box left at the placement would be a second solid
        // car under the first, one the moving car's own wheels and chassis collide with. The generator
        // writes these `nocollide`; this is for the one that does not.
        if (opt.createBodies && p.collide && p.vehiclePreset.empty() && aver_phys_ready()) {
            // WORLD, NOT AUTHORED. For a root the two are the same and this is the line it always
            // was; for a child the authored numbers are parent-relative and using them would put the
            // collision somewhere the mesh is not.
            const Transform& wx = worldXf[i];
            const bool moving = movesWithClip[i] != 0;
            const auto bodyStart = std::chrono::steady_clock::now();

            // TRIANGLES FIRST. A mesh's per-material-merged geometry can be concave -- an archway, a
            // courtyard wall -- and one box per mesh fills that concavity solid; asking for triangles
            // before ever building a box means concave architecture only ever gets the box when
            // nothing has (or can) give it triangles. Only trusted on a TRUE return with at least one
            // triangle's worth of indices, same as localBoundsFor/groundHeightAt: a host that answers
            // false, or leaves the counts at 0, has made no promise about the pointers either.
            bool usedMesh = false;
            u32 meshTriCount = 0;
            // False when the body's origin is not the entity's pivot, so driving it to the entity's
            // transform would misplace it. Mesh bodies are pivot-framed already.
            bool followsEntity = true;
            if (opt.localTrianglesFor) {
                const f32* triPositions = nullptr; u32 triVertexCount = 0;
                const u32* triIndices = nullptr;   u32 triIndexCount = 0;
                if (opt.localTrianglesFor(p.objectId, triPositions, triVertexCount,
                                          triIndices, triIndexCount) &&
                    triVertexCount > 0 && triIndexCount >= 3) {
                    // SHARED SHAPE FIRST: aver_phys_create_mesh_shape ONCE per unique mesh id, however
                    // many placements name it, each wrapping it at its own transform through Jolt's
                    // ScaledShape (aver_phys_add_mesh_shape_body) -- no per-placement triangle scaling
                    // and no second BVH build for a mesh this level already placed once. A Banyan
                    // tree's leaf cards, named by hundreds of placements, used to bake and BVH-build
                    // hundreds of identical meshes; now they build one.
                    i32 shapeHandle = 0;
                    if (const auto cached = meshShapeCache.find(p.objectId); cached != meshShapeCache.end()) {
                        shapeHandle = cached->second;
                    } else {
                        shapeHandle = aver_phys_create_mesh_shape(
                            triPositions, static_cast<i32>(triVertexCount),
                            reinterpret_cast<const i32*>(triIndices), static_cast<i32>(triIndexCount));
                        meshShapeCache.emplace(p.objectId, shapeHandle);
                        if (shapeHandle) ++out.uniqueMeshShapeCount;
                    }
                    if (shapeHandle) {
                        body = aver_phys_add_mesh_shape_body(
                            shapeHandle, wx.position.x, wx.position.y, wx.position.z,
                            wx.rotation.x, wx.rotation.y, wx.rotation.z, wx.rotation.w,
                            wx.scale.x, wx.scale.y, wx.scale.z);
                    }
                    // THE FALLBACK: a scale with a zero component, which a ScaledShape cannot
                    // represent at all (aver_phys_add_mesh_shape_body's own comment) but
                    // scaleMeshForBody's baking can, or a shape this mesh's own triangles never built
                    // in the first place. Same triangles either way, so a body addStaticMeshBody
                    // cannot make either is a body this placement was never going to get -- exactly
                    // the pre-existing rule (below), preserved: TRIANGLES WERE OFFERED, so this
                    // placement does not fall through to the box even if every attempt to use them
                    // returns 0.
                    if (!body)
                        body = addStaticMeshBody(wx, triPositions, triVertexCount, triIndices, triIndexCount);
                    usedMesh = true;
                    meshTriCount = triIndexCount / 3;
                }
            }
            if (!usedMesh) {
                // THE UNIT-CUBE PLACEHOLDER UNLESS THE HOST KNOWS BETTER. A placement's authored scale
                // used to go straight into aver_phys_add_static_box as a half-extent -- exactly right
                // for the built-in cube (local bounds exactly [-1,1] cm) and silently wrong for
                // anything else: an imported mesh placed at scale 1 got a 2x2x2 cm box at its pivot,
                // i.e. no collision an object of any real size could ever reach.
                Vec3 lmin{-kPlaceholderHalfExtentCm, -kPlaceholderHalfExtentCm, -kPlaceholderHalfExtentCm};
                Vec3 lmax{ kPlaceholderHalfExtentCm,  kPlaceholderHalfExtentCm,  kPlaceholderHalfExtentCm};
                if (opt.localBoundsFor) {
                    Vec3 hostMin, hostMax;
                    if (opt.localBoundsFor(p.objectId, hostMin, hostMax)) { lmin = hostMin; lmax = hostMax; }
                }
                // A MOVING PLACEMENT'S BOX IS BUILT AROUND THE PIVOT, because its body gets driven to
                // the entity's transform and a box centred elsewhere would jump by the offset. If Jolt
                // refuses that hull the plain box still collides, but as a static body.
                body = moving ? addPivotBoxBody(wx, lmin, lmax) : 0;
                if (!body) {
                    body = addStaticBoxBody(wx, lmin, lmax);
                    if (moving) followsEntity = false;
                }
            }

            out.bodyCreationSeconds +=
                std::chrono::duration<f64>(std::chrono::steady_clock::now() - bodyStart).count();
            if (body) {
                // KINEMATIC BEFORE THE ENTITY STAMP: only a kinematic body follows its animation, and
                // aver_phys_body_set_motion_type rebuilds a static body underneath its handle.
                if (moving) {
                    if (followsEntity && aver_phys_body_set_motion_type(body, AVER_PHYS_MOTION_KINEMATIC)) {
                        out.animatedBodies.push_back(AnimatedBody{e, body});
                    } else if (!warnedStaticAnimated) {
                        warnedStaticAnimated = true;
                        AVER_WARN("[Level] animated placement '{}' (or one under an animated parent) keeps a "
                                  "static body (its collision {}); it will not carry anything or push "
                                  "anything as it moves",
                                  p.asset, followsEntity ? "could not be made kinematic"
                                                         : "could not be built around its pivot");
                    }
                }
                // The one line that makes this placement's body IDENTIFIABLE later -- a raycast that
                // hits it can now report `e`, not just an opaque physics handle nothing else understands.
                aver_phys_set_entity(body, static_cast<i32>(e));
                if (usedMesh) { ++out.meshBodyCount; out.meshTriangleCount += meshTriCount; }
                else ++out.boxBodyCount;
            }
            out.bodies.push_back(body);
        }
#  else
        (void)opt;
#  endif
        out.entityBody.push_back(body);
    }

#if AVER_MODULE_PHYSICS
    // RELEASED HERE, NOT PER PLACEMENT: every body built from a cached shape above took its own
    // reference (through the ScaledShape aver_phys_add_mesh_shape_body wraps it in), so this call's
    // own claim on the handle is no longer needed once the loop is done handing them out. A shape
    // this call built but every placement naming it refused (0 in the cache) has nothing to release.
    for (const auto& [meshId, handle] : localShapes) {
        (void)meshId;
        if (handle) aver_phys_release_mesh_shape(handle);
    }
    // A BULK ADD LEAVES THE BROAD PHASE UNBALANCED: every body above went in one at a time, and every
    // ray, overlap and character sweep pays for the lopsided trees until they are rebuilt. Once per
    // big instantiate (a level load), never for a handful (a chunk, a paste) -- it walks every body.
    if (out.bodies.size() >= 256) aver_phys_optimize_broadphase();
#endif

    return out;
}

#  if AVER_MODULE_PHYSICS
namespace {

// The fastest a kinematic body is DRIVEN, in cm/s (60 m/s), at its pivot or at the far edge of it as
// it turns. Above that the pose did not move, it jumped -- a clip wrapping with a gap, a level reset, a
// script placing the entity -- and the velocity MoveKinematic would derive flings whatever stands on it.
constexpr f32 kMaxDriveSpeedCmS = 6000.0f;

// The pose each body was last driven to, by body handle. The animation's own history is what tells a
// jump from a fast move: the body itself cannot say, because the physics step may not have run (Play
// paused, a project with no GameMode) and its position reads as the centre of mass, not the pivot. A
// host may rebuild its AnimatedBody list (the editor does when a body is remade), so this lives here.
// Entries the last call did not visit are dropped, which is also what forgets a level's bodies when it
// unloads.
struct Driven {
    Transform pose;
    // An over-estimate of the half-diagonal of the body's world AABB at ANY rotation (the first one
    // measured x kReachTurnGrowth), or -1 until measured. See poseJumped.
    f32 reachBound = -1.0f;
    u32 stamp = 0;
};
std::unordered_map<i32, Driven> g_driven;
u32 g_driveStamp = 0;

// The most a body's reach can grow by turning, sqrt(3), with 4% over for float rounding. See poseJumped.
constexpr f32 kReachTurnGrowth = 1.8f;

// True when `body` going from the pose `from` last drove it to, to the pose `to`, in `dt` (> 0) seconds
// needs more than kMaxDriveSpeedCmS.
bool poseJumped(i32 body, Driven& from, const Transform& to, f32 dt) {
    if ((to.position - from.pose.position).size() / dt > kMaxDriveSpeedCmS) return true;

    // The turn is an angle, and what it costs a passenger depends on how far the body reaches from its
    // pivot: half the diagonal of its bounds is an honest over-estimate of that.
    const Quat& q0 = from.pose.rotation;
    const f32 d = std::fabs(q0.x * to.rotation.x + q0.y * to.rotation.y +
                            q0.z * to.rotation.z + q0.w * to.rotation.w);
    const f32 angle = 2.0f * std::acos(std::min(d, 1.0f));
    if (angle <= 0.0f) return false;

    // THE BOUNDS ARE ASKED OF PHYSICS ONCE PER BODY, NOT ONCE PER TURNING FRAME. The reach is half the
    // diagonal of the body's WORLD box, which grows and shrinks as the body turns, and a rotation that
    // has not changed rarely compares exactly equal after a frame of float noise, so the query used to
    // run on nearly every frame of every moving body (a locked lookup into Jolt, twice).
    // A box side cannot be longer than the distance between two points inside it. So at ANY angle the
    // world box has every side no longer than D, the diameter of the shape's own bounds, and a
    // half-diagonal of at most sqrt(3)/2 * D; while the box measured at the first angle encloses those
    // bounds, so its half-diagonal is at least D/2. The reach measured once, times kReachTurnGrowth,
    // therefore bounds the reach at every other angle. While that bound cannot make this turn a jump,
    // the real reach cannot either, and the answer is the same without asking; only a turn big enough
    // for the bound to matter pays for the exact query, as before.
    if (from.reachBound >= 0.0f && angle * from.reachBound / dt <= kMaxDriveSpeedCmS) return false;
    f32 lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    if (!aver_phys_body_aabb(body, lo, hi)) return false;
    const f32 reach = 0.5f * Vec3{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}.size();
    if (from.reachBound < 0.0f) from.reachBound = reach * kReachTurnGrowth;
    return angle * reach / dt > kMaxDriveSpeedCmS;
}

} // namespace
#  endif

u32 driveKinematicBodies(scene::World& world, const std::vector<AnimatedBody>& bodies, f32 dt) {
    u32 undriven = 0;
#  if AVER_MODULE_PHYSICS
    if (bodies.empty() || !aver_phys_ready()) { g_driven.clear(); return 0; }
    ++g_driveStamp;
    // MoveKinematic sets the velocity that reaches the pose in `dt`, but the solver integrates it in
    // whole fixed steps, so a frame shorter than one step would overshoot by their ratio.
    const f32 driveDt = std::max(dt, aver_phys_fixed_step());
    for (const AnimatedBody& b : bodies) {
        if (b.body <= 0 || !world.valid(b.entity)) continue;
        // worldMatrix composes the parent chain on demand, so it already holds the CLocal the
        // animation tick wrote this frame -- no flush needed first.
        const Transform xf = transformFromMatrix(world.worldMatrix(b.entity));
        // A pose Jolt would refuse (move_kinematic returns 0 for one) must not reach the setters below.
        if (!std::isfinite(xf.position.x) || !std::isfinite(xf.position.y) || !std::isfinite(xf.position.z) ||
            !std::isfinite(xf.rotation.x) || !std::isfinite(xf.rotation.y) || !std::isfinite(xf.rotation.z) ||
            !std::isfinite(xf.rotation.w))
            continue;
        // A JUMP, or a step with no time in it, is a teleport: set the pose and leave it at rest. The
        // passenger is not carried, which beats being thrown across the level. The first drive of a
        // body has no history to be a jump from.
        const auto [slot, firstDrive] = g_driven.try_emplace(b.body);
        Driven& mem = slot->second;
        const bool teleport = dt <= 0.0f || (!firstDrive && poseJumped(b.body, mem, xf, dt));
        mem.pose = xf;
        mem.stamp = g_driveStamp;
        if (teleport) {
            if (!aver_phys_body_set_position(b.body, xf.position.x, xf.position.y, xf.position.z)) ++undriven;
            aver_phys_body_set_rotation(b.body, xf.rotation.x, xf.rotation.y, xf.rotation.z, xf.rotation.w);
            aver_phys_body_set_velocity(b.body, 0.0f, 0.0f, 0.0f);
            aver_phys_body_set_angular_velocity(b.body, 0.0f, 0.0f, 0.0f);
            continue;
        }
        if (!aver_phys_body_move_kinematic(b.body, xf.position.x, xf.position.y, xf.position.z,
                                           xf.rotation.x, xf.rotation.y, xf.rotation.z, xf.rotation.w,
                                           driveDt))
            ++undriven;
    }
    for (auto it = g_driven.begin(); it != g_driven.end(); )
        it = (it->second.stamp == g_driveStamp) ? std::next(it) : g_driven.erase(it);
#  else
    (void)world; (void)bodies; (void)dt;
#  endif
    return undriven;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
