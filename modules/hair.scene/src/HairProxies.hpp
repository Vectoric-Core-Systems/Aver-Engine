#pragma once
// HairProxies -- the kinematic convex-hull bodies that stand in for a character's head, neck and shoulders so Jolt's hair, which collides with convex
// hulls only, has something to rest on (docs/plans/wave11/SOFTBODY_HAIR.md 2.15 item 5). Built from the existing physics ABI; src-local to Aver.Hair.Scene.
//
// The proxies live on their own layer (HairOptions::proxyLayer) and that layer touches only the hair's: a proxy never pushes a prop or a character, and
// nothing pushes a proxy, because it is kinematic and driven to the bone's pose each tick.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcHair.hpp"

#include <string>
#include <vector>

namespace aver::hair::detail {

struct ProxyBody {
    i32 body = 0;               // the physics body handle
    u32 bone = 0;               // index into the skeleton
    Vec3 offsetCm{0, 0, 0};     // from the bone, in the bone's frame
    bool placed = false;        // teleported to its first pose
    fmt::OcHairProxyShape shape = fmt::OcHairProxyShape::Sphere;   // kept for the debug overlay
    f32 sizeCm[3] = {0, 0, 0};  // as built (the owner's scale applied)
    bool script = false;        // added by a script (Entity.AddHairCollider), not by the groom or the default set
};

// Sets the layer matrix: proxies touch only hairLayer; freeLayer touches nothing. Idempotent; the host's project settings may reset the matrix, so the
// system calls it again whenever it builds.
void setupHairLayers(i32 hairLayer, i32 proxyLayer, i32 freeLayer);

// Creates the bodies of `groom.proxies`, or the default humanoid set when it lists none. `uniformScale` is the owner's world scale (the hulls are sized
// in the owner's model space). Returns how many bodies were made; `missing` (optional) gets what a default search did not find.
u32 createProxies(const fmt::OcHairData& groom, const fmt::OcSkeleton& skel, f32 uniformScale, i32 proxyLayer, std::vector<ProxyBody>& out,
                  std::vector<std::string>* missing = nullptr);

// One proxy from its record (the groom's, or one a script adds): false with a log line when the skeleton lacks its bone or the shape has no size.
// `uniformScale` as for createProxies.
bool createProxy(const fmt::OcHairProxy& spec, const fmt::OcSkeleton& skel, f32 uniformScale, i32 proxyLayer, std::vector<ProxyBody>& out);

// Destroys the bodies from index `first` on and drops them from the list (the script-added tail of an instance's proxies).
void destroyProxiesFrom(std::vector<ProxyBody>& bodies, usize first);

// Drives every proxy to its bone's pose. `model` are the skeleton's model-space matrices, `world` the owner's world matrix; the first call after
// creation teleports instead of sweeping. `dt` is the time to reach the pose (the physics layer derives the velocity from it).
void moveProxies(std::vector<ProxyBody>& bodies, const std::vector<Mat4>& model, const Mat4& world, f32 dt);

void destroyProxies(std::vector<ProxyBody>& bodies);

} // namespace aver::hair::detail
