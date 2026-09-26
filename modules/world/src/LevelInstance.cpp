#include "aver/world/LevelInstance.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    fit.centre = worldXf.position + worldXf.rotation.rotate(Vec3{s.x * lc.x, s.y * lc.y, s.z * lc.z});
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
#endif

} // namespace aver::world

#if AVER_MODULE_SCENE

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
            // AUTHORED VISIBILITY, not an unconditional seed: addComponent hands back zero-filled
            // storage (see kMeshRendererHiddenFromOwner's own comment on that, Components.hpp), so
            // a placement that asked to be hidden must simply not OR the bit in, rather than being
            // set and then cleared -- the two read the same, but only one matches p.visible being
            // the single source of truth a save later reads back out of this same bit.
            if (p.visible) mr->flags |= scene::kMeshRendererVisible;
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
            const auto bodyStart = std::chrono::steady_clock::now();

            // TRIANGLES FIRST. A mesh's per-material-merged geometry can be concave -- an archway, a
            // courtyard wall -- and one box per mesh fills that concavity solid; asking for triangles
            // before ever building a box means concave architecture only ever gets the box when
            // nothing has (or can) give it triangles. Only trusted on a TRUE return with at least one
            // triangle's worth of indices, same as localBoundsFor/groundHeightAt: a host that answers
            // false, or leaves the counts at 0, has made no promise about the pointers either.
            bool usedMesh = false;
            u32 meshTriCount = 0;
            if (opt.localTrianglesFor) {
                const f32* triPositions = nullptr; u32 triVertexCount = 0;
                const u32* triIndices = nullptr;   u32 triIndexCount = 0;
                if (opt.localTrianglesFor(p.objectId, triPositions, triVertexCount,
                                          triIndices, triIndexCount) &&
                    triVertexCount > 0 && triIndexCount >= 3) {
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
                body = addStaticBoxBody(wx, lmin, lmax);
            }

            out.bodyCreationSeconds +=
                std::chrono::duration<f64>(std::chrono::steady_clock::now() - bodyStart).count();
            if (body) {
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

    return out;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
