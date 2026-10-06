#pragma once
// Reading and writing one component field as an OcSaveField, by the scene's own field table.
// Internal to Aver.Prefab: the same job aver::save does for a whole world, narrowed to what a
// prefab diff needs (change detection, material by name, entity references by node path).
#include "aver/formats/OcSave.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <functional>
#include <string>

namespace aver::prefab::detail {

// Entity -> the node path it has inside the instance being read. False when it is outside it.
using PathLookup = std::function<bool(scene::Entity, std::string& path)>;
// Node path -> the live entity, kInvalidEntity when unknown.
using EntityLookup = std::function<scene::Entity(const std::string& path)>;

// Components a prefab neither captures nor diffs: derived or bookkeeping (CWorld, CHierarchy, CName)
// and the link itself.
bool excludedComponent(u32 type, u32 linkType);

// Fields that look authored but are runtime state (a particle emitter's age and seed). Skipped so
// they never show as an override.
bool ignoredField(const std::string& component, const std::string& field);

// CMeshRenderer.material is an i32 token interned per PROCESS; a prefab stores its NAME instead.
bool isMaterialField(const scene::FieldDesc& d);

// Reads field `fieldId` of `e`. False when it cannot be expressed (an entity reference that leaves
// the instance).
bool readField(const scene::World& w, scene::Entity e, u32 fieldId, const PathLookup& lookup,
               fmt::OcSaveField& out);

enum class Write { Same, Changed, Failed };

// Writes `f` into field `fieldId` of `e`, touching bookkeeping the component needs (CLocal's
// revision, CMeshRenderer's upload flag) only when the value actually changed.
Write writeField(scene::World& w, scene::Entity e, u32 fieldId, const fmt::OcSaveField& f,
                 const EntityLookup& lookup);

// All writable, non-ignored fields of component `type` on `e`.
bool readComponent(const scene::World& w, scene::Entity e, u32 type, const PathLookup& lookup,
                   fmt::OcSaveComponent& out);

// The zero value of `d`'s kind, in the shape readField produces (for a field a prefab never named).
fmt::OcSaveField zeroField(const scene::FieldDesc& d);

} // namespace aver::prefab::detail
