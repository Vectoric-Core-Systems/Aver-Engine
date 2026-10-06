#pragma once
// .ocprefab -- a prefab asset -- and the prefab INSTANCE records an .ocworld carries.
//
// A prefab is a saved entity hierarchy: nodes with a stable uid, a parent, a name and a list of
// components. Components and fields are written BY NAME (the OcSave shapes, reused), so the format
// never reaches Aver.Scene and a component registered at runtime saves without this file knowing it.
//
// Three ideas carry the whole design (docs/PREFABS.md has the long form):
//   * IDENTITY IS A PATH. A node is addressed by its uid inside its prefab, and a node inside a nested
//     prefab by "<nodeUid>/<uid inside the nested prefab>". The prefab's own root is the empty path.
//     An override and a live entity's link both name a node this way, so they survive the prefab being
//     edited, reordered or having nodes added -- an array index would not.
//   * AN OVERRIDE IS A DELTA, not a copy: "set this field", "add this component", "remove this
//     component" on one node. An instance (in a level, or a nested node in another prefab) is the
//     prefab plus its list of overrides, which is why editing the prefab can reach every instance.
//   * A NESTED PREFAB IS A NODE with `prefab` set. It holds no components of its own; everything that
//     differs from the nested asset (its position included) is an override on that node.
//
// Text form, one record per line, '#' comments, unknown records skipped:
//   OCPREFAB 1
//   NAME Crate
//   NEXTUID 4
//   NODE 1 parent 0 name Crate
//     COMP CMeshRenderer
//       F mesh i64 123456789
//       F material string ~M_Wood
//   NODE 3 parent 1 name Lid prefab Prefabs/Lid.ocprefab
//     OVERRIDE set - CLocal position vec3 0 0 50
//     OVERRIDE addcomp 5 CTags
//
// And in a level (.ocworld), after the placements, additive so an older reader skips them:
//   PREFABINST Prefabs/Crate.ocprefab pos 100 0 0 rot 0 0 90 scale 1 1 1 name ~Crate%201
//     POVERRIDE set 3 CMeshRenderer material string ~M_Red
//   ENDPREFABINST
#include "aver/formats/OcSave.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// FieldKind values as the text names them, in AVER_SCENE_KIND_* order. Mirrors scene_abi.h (this
// module may not include it); aver::save has the drift asserts for the same set.
inline constexpr u32 kOcPrefabKindF32 = 0, kOcPrefabKindVec3 = 1, kOcPrefabKindQuat = 2,
                     kOcPrefabKindI32 = 3, kOcPrefabKindBool = 4, kOcPrefabKindI64 = 5,
                     kOcPrefabKindEntity = 6, kOcPrefabKindString = 7, kOcPrefabKindMat4 = 8;

// The pseudo-component an override names to change a node's own name ("@Node", field "name").
inline constexpr const char* kOcPrefabNodeComponent = "@Node";

enum class OcOverrideOp : u8 { Set, AddComponent, RemoveComponent };

// One property delta on one node.
//   path       "" is the prefab's root, "5" a node, "3/5" a node inside nested node 3.
//   component  a registered component name, or kOcPrefabNodeComponent.
//   value      Set only: `value.name` is the field, `value.kind` and the payload its value.
//              An ENTITY value names its target by node path in `value.s` (`value.i == -1` is none) --
//              a handle or a uid would mean nothing once the nested structure changes.
struct OcPrefabOverride {
    OcOverrideOp op = OcOverrideOp::Set;
    std::string path;
    std::string component;
    OcSaveField value;
};

// One node of a prefab.
struct OcPrefabNode {
    u32 uid = 0;              // unique within the prefab, never reused (OcPrefabData::nextUid)
    u32 parent = 0;           // uid of the parent, 0 for the root
    std::string name;
    std::string className;    // framework class by NAME, as OcSaveEntity::className
    // Non-empty: this node is an instance of that prefab. `components` is then unused and every
    // change from the nested asset lives in `overrides`, whose paths are relative to it.
    std::string prefab;
    // Entity-kind fields hold the TARGET NODE UID in `i` (0 or -1 = none) inside a prefab.
    std::vector<OcSaveComponent> components;
    std::vector<OcPrefabOverride> overrides;   // only meaningful when `prefab` is set
};

struct OcPrefabData {
    u32 version = 1;
    std::string name;
    u32 nextUid = 1;
    // PARENTS BEFORE CHILDREN, exactly one root (parent == 0). parse and valid() enforce both.
    std::vector<OcPrefabNode> nodes;

    const OcPrefabNode* find(u32 uid) const;
    OcPrefabNode* find(u32 uid);
    // The root's uid, or 0 for an empty prefab.
    u32 rootUid() const;
    // Takes the next unused uid.
    u32 allocUid() { return nextUid++; }
    // Unique non-zero uids, parents first, one root, nextUid past every uid.
    bool valid(std::string* why = nullptr) const;
};

// One prefab instance placed in a level. The transform is the INSTANCE's -- the prefab's own root
// transform is never used -- in the same units and angle convention as a placement.
struct OcPrefabInstance {
    std::string prefab;      // asset reference, content-relative like a placement's asset
    std::string name;        // outliner label; empty = the prefab root's name
    f64 x = 0, y = 0, z = 0;
    f64 yaw = 0, pitch = 0, roll = 0;   // degrees
    f64 sx = 1, sy = 1, sz = 1;
    std::vector<OcPrefabOverride> overrides;
};

// ---- the asset ----------------------------------------------------------------------------------
bool parseOcPrefab(std::string_view text, OcPrefabData& out, std::string* err = nullptr);
bool loadOcPrefab(const std::string& path, OcPrefabData& out, std::string* err = nullptr);
std::string writeOcPrefab(const OcPrefabData& p);
bool saveOcPrefab(const std::string& path, const OcPrefabData& p, std::string* err = nullptr);

// ---- the level records --------------------------------------------------------------------------
// One tokenised .ocworld line. Returns true when the line was one of ours (PREFABINST, POVERRIDE,
// ENDPREFABINST) and was consumed; `open` is the index of the instance a POVERRIDE attaches to, -1
// outside one, and is the only state the parser keeps between lines.
bool parseOcPrefabInstanceLine(const std::vector<std::string_view>& tokens,
                               std::vector<OcPrefabInstance>& out, i32& open);
// Appends the records for every instance, preceded by a blank line when there are any. Writes
// nothing for an empty list, so a level without prefabs round-trips byte for byte.
void appendOcPrefabInstances(std::string& out, const std::vector<OcPrefabInstance>& instances);

// ---- helpers shared by the runtime and the tests ----------------------------------------------------
// "a" + "b" -> "a/b"; either side empty yields the other.
std::string ocPrefabJoinPath(std::string_view a, std::string_view b);
// Splits "3/5/2" into head "3" and rest "5/2"; a one-segment path has an empty rest.
void ocPrefabSplitPath(std::string_view path, std::string& head, std::string& rest);
// Same kind and same payload (floats compared bit for bit, which a %.9g round trip preserves).
bool ocPrefabFieldEqual(const OcSaveField& a, const OcSaveField& b);
// Same op, path, component and field name; used to replace an override rather than stack a second.
bool ocPrefabSameTarget(const OcPrefabOverride& a, const OcPrefabOverride& b);

} // namespace aver::fmt
