#pragma once
// CPrefabLink -- the link from an entity back to the prefab node it was spawned from.
//
// Header-only and registered at RUNTIME (like Synapse's components), so it adds no id to the fixed
// built-in table and no source to Aver.Scene's CMake. Aver.Prefab's PrefabSystem registers it; any
// world that never builds a PrefabSystem simply has no such component.
//
// THE LINK IS ALL AN INSTANCE NEEDS ON THE ENTITY. Overrides are not stored: they are the difference
// between the live fields and what the prefab says, recomputed when needed (docs/PREFABS.md), so an
// edit made through any route -- Details, a gizmo, a script, undo -- is an override without that route
// knowing about prefabs. The link only says which node an entity is and which instance owns it.
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"
#include "aver/scene/World.hpp"

#include <cstddef>

namespace aver::scene {

struct CPrefabLink {
    u64    prefabId   = 0;               // fnv1a64 of the instance's prefab asset path
    u64    pathHash   = 0;               // fnv1a64 of this node's path inside it ("" = the root)
    Entity root       = kInvalidEntity;  // the instance root; a root links to itself
    u32    instanceId = 0;               // stable across delete + undo, which a handle is not
};

static_assert(sizeof(CPrefabLink) == 24, "CPrefabLink must be padding-free");

inline constexpr const char* kPrefabLinkComponentName = "CPrefabLink";

// Registers the component and its field table, idempotently. Returns its type id, 0 on failure.
inline u32 registerPrefabLink(World& world) {
    if (const u32 existing = world.componentId(kPrefabLinkComponentName)) return existing;
    ComponentBuilder b = world.registerComponent<CPrefabLink>(kPrefabLinkComponentName);
    b.field("prefabId",   FieldKind::I64,    static_cast<u16>(offsetof(CPrefabLink, prefabId)))
     .field("pathHash",   FieldKind::I64,    static_cast<u16>(offsetof(CPrefabLink, pathHash)))
     .field("root",       FieldKind::Entity, static_cast<u16>(offsetof(CPrefabLink, root)))
     .field("instanceId", FieldKind::I32,    static_cast<u16>(offsetof(CPrefabLink, instanceId)));
    return b.verify(sizeof(CPrefabLink)) ? b.typeId() : 0;
}

} // namespace aver::scene
