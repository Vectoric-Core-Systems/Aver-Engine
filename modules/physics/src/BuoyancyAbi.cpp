// BuoyancyAbi.cpp -- the ABI boundary for water::WaterVolumeTable: converts engine units/axes to
// Jolt's, then just stores the result. The Jolt call itself lives in Buoyancy.cpp; this file never
// touches a JPH::PhysicsSystem or a BodyID.
//
// DELIBERATELY DOES NOT VALIDATE `body` AGAINST g_world's bodies/sensors MAPS. It cannot: those maps
// are private to PhysicsWorld.cpp's translation unit (see Buoyancy.hpp's own comment on why findBody
// and forEachBody are injected rather than reached for directly). This is safe because
// WaterVolumeTable::evaluate's findBody lookup -- called from the step loop -- is the actual point
// where an invalid or since-removed handle silently drops out; nothing here needs to duplicate that
// check to stay correct, only to stay cheap, and a wasted table entry for a dead handle costs nothing
// worse than a few bytes until the next clear() or clearAll().
#include "aver/physics/physics_abi.h"
#include "Buoyancy.hpp"
#include "Convert.hpp"

using namespace aver;
using namespace aver::physics;
using namespace aver::phys::water;

extern "C" {

// Registers or replaces the global water plane. `heightCm` is the engine's +Z-up height the plane
// sits at; any point at that height works as the plane's anchor since ApplyBuoyancyImpulse only ever
// projects a body's shape onto the (position, normal) plane, so (0, 0, heightCm) is as good as any
// other. `normalUnit` goes through toJoltUnit, NOT toJolt -- Convert.hpp's own documented distinction,
// and the exact mistake this function must not make: a unit normal describes a direction, not a
// point, and scaling it would silently detune every drag/buoyancy computation that reads it.
// `fluidVelocityCmS` is a velocity, so it goes through toJoltDir (scaled and permuted, cm/s -> m/s),
// the same treatment velocities get everywhere else in this module.
//
// Always returns 1: unlike a per-body override there is no handle to be invalid, and disabling the
// plane without losing its height/normal/drag is `buoyancy <= 0`, honoured inside
// WaterVolumeTable::evaluate rather than rejected here.
int32_t aver_phys_set_water_plane(float heightCm,
                                  const float normalUnit[3],
                                  float buoyancy, float linearDrag, float angularDrag,
                                  const float fluidVelocityCmS[3]) {
    WaterVolume v;
    v.surfacePosition = toJolt(Vec3(0.0f, 0.0f, heightCm));
    v.surfaceNormal   = toJoltUnit(Vec3(normalUnit[0], normalUnit[1], normalUnit[2]));
    v.buoyancy        = buoyancy;
    v.linearDrag      = linearDrag;
    v.angularDrag     = angularDrag;
    v.fluidVelocity   = toJoltDir(Vec3(fluidVelocityCmS[0], fluidVelocityCmS[1], fluidVelocityCmS[2]));
    waterVolumes().setPlane(v);
    return 1;
}

// Removes the global plane outright (as opposed to disabling it via a non-positive buoyancy, which
// keeps its height/normal/drag around for a later re-enable).
void aver_phys_clear_water_plane(void) {
    waterVolumes().clearPlane();
}

// 1 while a plane is set, writing its height back into `outHeightCm` (if non-null) via the exact
// inverse of the conversion aver_phys_set_water_plane applied -- fromJolt of the stored surface point
// round-trips to the original heightCm because that point's x and y were always zero.
int32_t aver_phys_water_plane(float* outHeightCm) {
    WaterVolumeTable& table = waterVolumes();
    if (!table.hasPlane()) return 0;
    if (outHeightCm) *outHeightCm = fromJolt(table.plane().surfacePosition).z;
    return 1;
}

// Registers or replaces a per-body override, taking precedence over the global plane for `body`.
//
// REJECTS A DEAD HANDLE, not merely a zero one, because physics_abi.h says it does and because every
// sibling setter in this module does (aver_phys_body_position, aver_phys_set_entity,
// aver_phys_remove_body all resolve the handle first). Accepting a stale handle here would register
// water against a body that no longer exists, which evaluate() would then look up and skip on every
// substep for the life of the process -- work that is never done and never noticed.
//
// The existence check goes through aver_phys_body_position rather than the module's own findBody:
// `World` is file-local to PhysicsWorld.cpp on purpose, and the public call already returns 0 for a
// dead handle by its own documented contract. Borrowing that beats exporting an internal.
int32_t aver_phys_set_water_volume(int32_t body,
                                   const float surfacePosCm[3],
                                   const float surfaceNormalUnit[3],
                                   float buoyancy, float linearDrag, float angularDrag,
                                   const float fluidVelocityCmS[3]) {
    if (body == 0) return 0;
    float probe[3];
    if (aver_phys_body_position(body, probe) == 0) return 0;

    WaterVolume v;
    v.surfacePosition = toJolt(Vec3(surfacePosCm[0], surfacePosCm[1], surfacePosCm[2]));
    v.surfaceNormal   = toJoltUnit(Vec3(surfaceNormalUnit[0], surfaceNormalUnit[1], surfaceNormalUnit[2]));
    v.buoyancy        = buoyancy;
    v.linearDrag      = linearDrag;
    v.angularDrag     = angularDrag;
    v.fluidVelocity   = toJoltDir(Vec3(fluidVelocityCmS[0], fluidVelocityCmS[1], fluidVelocityCmS[2]));
    waterVolumes().set(body, v);
    return 1;
}

// Removes a body's override, returning it to the plane (if any is set). Returns 1 only when an
// override actually existed to remove -- a handle that was never registered, or one that is simply
// dead, both read as "nothing to clear" from here, and both correctly return 0 without this function
// needing to tell the two apart.
int32_t aver_phys_clear_water_volume(int32_t body) {
    WaterVolumeTable& table = waterVolumes();
    const size_t before = table.count();
    table.clear(body);
    return table.count() != before ? 1 : 0;
}

// How many bodies had buoyancy applied on the last evaluate() -- see Buoyancy.hpp for why this
// number exists at all.
int32_t aver_phys_buoyant_body_count(void) {
    return waterVolumes().lastAppliedCount();
}

} // extern "C"
