// LevelComponents: the .ocworld COMPONENT record -> live components on a placement's entity, for BOTH hosts (the
// editor's load path and the packaged game's), plus the way back (capture) the editor's save and undo use.
// docs/plans/parity_demo/ADD_COMPONENT.md. One applier means the editor and the packaged game cannot disagree.
//
// The format does not know what a type means (fmt::OcPlacedComponent); this file does. Components attach to LEVEL
// PLACEMENTS only (entities with a CMeshRenderer); a graph-class placement has no entity and is never a host.
//
// Storage arrives zero-filled with no constructor, so every apply attaches through the owning system and then writes
// every authored field explicitly (an IkRig weight left 0 would attach and do nothing).
#pragma once
#if AVER_MODULE_SCENE

#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/LevelInstance.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::game {

// Canonical type order = apply order (dependencies first) = write order = the order of the table in 3.4 and of editor::kPlacedKinds (7.2).
inline constexpr std::string_view kPlacedComponentTypes[] = {
    "SkeletalMesh","Animator","RootMotion","MotionMatcher","ControlRig","IkRig","Brain","SmartObject","SoftBody",
    "Cloth","Destructible"};

using PathOfIdFn = std::function<std::string(u64 id)>;   // content-relative path, "" when unknown

// Add-or-update one component on `e`. False (and a one-line AVER_WARN) when the type is unknown to this build, a required
// component is missing, an asset key is absent, or the system refuses. Never partially leaves a half-written component.
bool applyPlacedComponent(scene::World&, scene::Entity, const fmt::OcPlacedComponent&);
// Removes the component (Brain: stops the tree first). False when absent.
bool removePlacedComponent(scene::World&, scene::Entity, std::string_view type);
// The authored state of one component as a record; nullopt when the entity does not carry it. Runtime OUT fields are never read.
std::optional<fmt::OcPlacedComponent> capturePlacedComponent(scene::World&, scene::Entity, std::string_view type, const PathOfIdFn&);
std::vector<fmt::OcPlacedComponent> capturePlacedComponents(scene::World&, scene::Entity, const PathOfIdFn&);   // canonical order
// Applies p.components (sorted into canonical order). Returns the number applied. `placementIndex` only labels the warnings.
u32 applyPlacedComponents(scene::World&, scene::Entity, const fmt::OcWorldPlacement&, i64 placementIndex = -1);
// The post-pass: for k in inst.entities, applies w.placements[inst.placementIndex[k]].components.
// Logs ONE `AVER_INFO("[Level] {} placed component(s) on {} placement(s)", applied, placementsTouched)` when applied > 0 (the line the demo and the
// acceptance checks grep for), and one AVER_WARN per refused record: "[Level] COMPONENT {Type} refused on placement {index}: {reason}".
void applyLevelComponents(scene::World&, const fmt::OcWorldData&, const world::LevelInstance&);

} // namespace aver::game

#endif // AVER_MODULE_SCENE
