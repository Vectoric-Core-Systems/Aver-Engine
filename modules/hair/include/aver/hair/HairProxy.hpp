#pragma once
// Collision proxies for hair (design 2.15 item 5): Jolt's Hair collides with CONVEX HULLS only, so the head, neck and shoulders of a character are
// stood in for by small kinematic hulls that follow their bones. This is the pure half: the hull points of a proxy shape and the default humanoid
// set derived from joint names. HairSystem (phase H) creates the bodies from them with the existing physics ABI.
#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::hair {

// The points of a convex hull for a proxy shape, in centimetres, centred on the origin with the long axis along engine +Z:
//   sphere   sizeCm[0] = radius                                  32 points (a Fibonacci spiral on the sphere)
//   capsule  sizeCm[0] = radius, sizeCm[1] = half height of the straight part   32 points (the upper half of the spiral raised, the lower lowered)
//   box      sizeCm = half extents                               8 points
// 3 floats per point. Empty for a size that is not positive.
std::vector<f32> proxyHullPoints(fmt::OcHairProxyShape shape, const f32 sizeCm[3]);

// The default proxies of a humanoid when a groom lists none: a sphere for the head, a capsule for the neck, a capsule for each clavicle or
// shoulder and a box for the upper spine, each on the first joint whose name matches (case-insensitive) and sized in cm for a 175 cm character.
// Names that match nothing give nothing: a rig without them gets no proxies, and `missing` (optional) lists what was looked for and not found.
std::vector<fmt::OcHairProxy> defaultHumanoidProxies(const std::vector<std::string>& jointNames, std::vector<std::string>* missing = nullptr);

// True when `joint` names a bone of this kind: "head" matches head and Head_01 but not HeadTop_End; "neck", "spine", "shoulder" and "clavicle"
// match anywhere in the name.
bool jointMatches(std::string_view joint, std::string_view kind);

} // namespace aver::hair
