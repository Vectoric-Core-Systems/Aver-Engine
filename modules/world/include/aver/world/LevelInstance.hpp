// Turning parsed .ocworld placements into live scene entities -- ONCE, for both hosts.
//
// WHY THIS EXISTS. Until this module, the same loop lived twice: modules/runtime.game/src/GameLevel.cpp
// and sandbox/src/SandboxApp.cpp. That was deliberate at the time -- modules/runtime.game/CMakeLists.txt
// says so in its own header: "The lift is a COPY: sandbox/src/SandboxApp.cpp is not edited by it, so
// the editor cannot regress. De-duplication is a later slice, proven by the gates staying identical."
// This is that slice, and that is the bar it is held to.
//
// The two copies had ALREADY diverged before they were merged, which is the argument for merging
// them: the editor applied SUN/SKY at load and drove the cloud layer from a PCGVOLUME named "Sky";
// the game applied only FOG and deferred the sky. Each narrowed f64 -> f32 in its own near-identical
// loop. Every future change to how a placement becomes an entity -- chunk ownership above all --
// would have had to be made twice, correctly, forever.
//
// WHAT IS SHARED AND WHAT IS NOT. Shared: the transform narrowing, the rotation contract, entity
// creation, the mesh renderer, material interning, and the static physics box. NOT shared, because
// they are genuinely host policy: the editor's label table and entity->body map, the game's resolved
// PCG field specs, sky application, camera framing, selection. Those stay with their hosts and read
// this function's output.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/world/LevelTransform.hpp"

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"
#  include "aver/scene/World.hpp"

#  include <functional>
#  include <string>
#  include <vector>

namespace aver::world {

struct InstantiateOptions {
    // Called once per placement that interned a NON-ZERO surface token, with that token and the
    // authored surface name. Both hosts resolve the surface's .ocmat and cache the handle, but by
    // different means -- the game through GameContent, the editor into its own map -- so the
    // resolution is theirs and only the timing is shared.
    //
    // Called from inside the placement loop, in placement order, so a host that interns further
    // materials from within it gets the same token sequence it would have got before.
    std::function<void(i32 token, const std::string& surface)> bindMaterial;

    // False to skip static-body creation entirely. Bodies are also skipped when the module is built
    // without physics, or when aver_phys_ready() is false -- which is not an error: it is the state
    // a host is in before it has called aver_phys_init.
    bool createBodies = true;
};

// The result, in placement order. `entities` holds only the placements that produced an entity, so
// it is NOT indexed by placement -- use `placementIndex` to get back to the record.
struct LevelInstance {
    std::vector<scene::Entity> entities;
    // Parallel to `entities`: the index into OcWorldData::placements each one came from.
    std::vector<u32> placementIndex;
    // Parallel to `entities`: the static body that placement created, or -1 if it made none.
    std::vector<i32> entityBody;
    // The bodies that were actually created, in creation order, for a host that only needs to
    // remove them again. Equal to `entityBody` with the -1s dropped.
    std::vector<i32> bodies;
};

// Creates one entity per placement in the process-global World.
//
// ORDER IS OBSERVABLE AND IS PRESERVED. Entity indices, material token values and physics body ids
// are all assigned sequentially by the order of the calls made here, and the render gates compare
// bit-exact images that depend on all three. This walks placements in file order and does, per
// placement, exactly what the two hosts did: create, add the mesh renderer, intern the material,
// bind it, then the body.
LevelInstance instantiate(const fmt::OcWorldData& w, const InstantiateOptions& opt = {});

} // namespace aver::world

#endif // AVER_MODULE_SCENE
