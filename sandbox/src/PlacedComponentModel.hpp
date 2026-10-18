#pragma once
// The pure half of Details > Add Component for the animation and AI components: which components exist
// (kPlacedKinds, in the canonical order of the COMPONENT record, docs/formats/FORMAT_SPECS.md section 11),
// when one may be attached or removed, the creation defaults, and the asset candidates a picker offers.
// Header-only and ImGui-free so tests/editor can drive every rule; SandboxComponentDetails.cpp is the
// widget half. docs/plans/parity_demo/ADD_COMPONENT.md section 7.
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// One entry of the Add / Remove Component menus. `type` is the COMPONENT record type; `builtinId` is the
// fixed scene id for the two built-ins and 0 for the rest, which register by name and have a per-process id
// (resolve with World::componentId(componentName)).
struct PlacedKind {
    const char* type;
    const char* menuLabel;
    const char* tip;
    const char* header;            // the Details section header
    u32         builtinId;
    const char* componentName;     // the registered name; "" for a built-in
    // Lower-case, with the dot; unused slots null. A fixed array: an initializer_list member of a constexpr table
    // does not keep its backing array alive under MSVC, and reading it crashed.
    std::array<const char*, 2> assetExts{};
};

// Canonical order = apply order (dependencies first) = write order = the table in ADD_COMPONENT.md 3.4.
inline constexpr PlacedKind kPlacedKinds[] = {
    {"SkeletalMesh", "Skeletal Mesh",
     "Binds a skeleton (.ocskel) to this mesh so it can be posed and animated.",
     "Skeletal Mesh", scene::kComponentSkeletalMesh, "", {".ocskel"}},
    {"Animator", "Animator",
     "Plays an animation clip (.ocanim) on the skeleton. Needs a Skeletal Mesh.",
     "Animator", scene::kComponentAnimator, "", {".ocanim"}},
    {"RootMotion", "Root Motion",
     "Moves the entity with the root bone's motion while Play runs. Needs a Skeletal Mesh.",
     "Root Motion", 0, "CRootMotion", {}},
    {"MotionMatcher", "Motion Matching",
     "Picks the pose from a motion database (.ocmotion) that best fits how the entity moves. Needs a Skeletal Mesh.",
     "Motion Matching", 0, "CMotionMatcher", {".ocmotion"}},
    {"ControlRig", "Control Rig",
     "Runs a control rig (.ocrig) over the animated pose. Needs a Skeletal Mesh.",
     "Control Rig", 0, "CControlRig", {".ocrig"}},
    {"IkRig", "IK Rig",
     "Plants the feet on the ground and drives IK goals from an .ocikrig. Needs a Skeletal Mesh.",
     "IK Rig", 0, "CIkRig", {".ocikrig"}},
    {"Brain", "AI Brain",
     "Runs a behaviour tree (.ocbt) or state tree (.ocst) on this entity during Play.",
     "AI Brain", 0, "CSynapseBehavior", {".ocbt", ".ocst"}},
    {"SmartObject", "Smart Object",
     "Makes this entity something AI agents can claim and use (.ocso).",
     "Smart Object", 0, "CSmartObject", {".ocso"}},
    {"Cloth", "Cloth",
     "Simulates this mesh as cloth (pinned, painted or ruled). Skins to a Skeletal Mesh if there is one.",
     "Cloth", 0, "CCloth", {".ocsoft"}},
    {"Destructible", "Destructible",
     "Makes this mesh breakable: it dents under hits and tears into pieces past its material's limits.",
     "Destructible", 0, "CDestructible", {".ocsoft"}},
    {"MorphTargets", "Morph Targets",
     "Drives this mesh's blend shapes (facial expressions, lip-sync) from animation, script and the sequencer.",
     "Morph Targets", 0, "CMorphTargets", {".ocface"}},
};
inline constexpr usize kPlacedKindCount = sizeof(kPlacedKinds) / sizeof(kPlacedKinds[0]);

// The kind's record type, or nullptr.
inline const PlacedKind* findPlacedKind(std::string_view type) {
    for (const PlacedKind& k : kPlacedKinds)
        if (type == k.type) return &k;
    return nullptr;
}

inline std::span<const char* const> placedAssetExts(const PlacedKind& k) {
    usize n = 0;
    while (n < k.assetExts.size() && k.assetExts[n]) ++n;
    return std::span<const char* const>(k.assetExts.data(), n);
}

// What the selected entity is, as far as attaching a component goes. Filled by the panel from the live entity.
struct AttachContext {
    bool levelOwned = false;          // one of the level's own placements (levelEntities_), not a class part or preview
    bool hasMesh = false;             // carries a CMeshRenderer
    bool hasSkeleton = false;         // carries a CSkeletalMesh with skeleton != 0
    bool hasObjectAnim = false;       // the placement plays an `anim` transform clip (the other meaning of CAnimator)
    bool legacyOcmap = false;         // the open level is a legacy .ocmap, which cannot store a COMPONENT record
    bool prefabLinked = false;        // saved inside a PREFABINST record
    bool instanceMember = false;      // saved inside a LEVELINST record
    bool streamedOnlyRecord = false;  // a streamed-out placement: no live entity to attach to
    bool alreadyPresent = false;      // the entity carries this component already
    bool hasCloth = false;            // carries a CCloth (one body per entity root: not with a Destructible)
    bool hasDestructible = false;     // carries a CDestructible
};

namespace detail {
inline bool deny(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
}
} // namespace detail

// True when `kind` may be attached; otherwise false with the reason in *why. The rules are the "Needs" lines
// of ADD_COMPONENT.md 4.1-4.8.
inline bool canAttach(const AttachContext& c, const PlacedKind& kind, std::string* why = nullptr) {
    using detail::deny;
    if (c.alreadyPresent) return deny(why, "This entity already has one.");
    if (c.streamedOnlyRecord) return deny(why, "This placement is streamed out; load it to add components.");
    if (c.legacyOcmap) return deny(why, "The legacy .ocmap format cannot store components. Save the level as .ocworld.");
    if (c.prefabLinked) return deny(why, "This entity is part of a prefab instance, which the prefab saves.");
    if (c.instanceMember) return deny(why, "This entity belongs to a level instance, which its child level saves.");
    if (!c.levelOwned) return deny(why, "Not a level placement");
    if (!c.hasMesh) return deny(why, "Needs a mesh: only a placed mesh can carry this component.");

    const std::string_view t = kind.type;
    if (t == "SkeletalMesh") {
        if (c.hasObjectAnim)
            return deny(why, "This placement plays an object animation; remove it (Animation section) before adding a skeleton.");
        return true;
    }
    if (t == "Animator" || t == "RootMotion" || t == "MotionMatcher" || t == "ControlRig" || t == "IkRig") {
        if (!c.hasSkeleton) return deny(why, "Needs a Skeletal Mesh with a skeleton chosen.");
        return true;
    }
    if (t == "Cloth" && c.hasDestructible) return deny(why, "This entity is already Destructible; one soft body per entity.");
    if (t == "Destructible" && c.hasCloth) return deny(why, "This entity is already Cloth; one soft body per entity.");
    return true;   // Brain, SmartObject, Cloth, Destructible and MorphTargets need only a mesh placement
}

// True when `kind` may be removed. A Skeletal Mesh with dependents is refused (no cascade, so one undo entry
// stays one component); everything else is always removable.
inline bool canRemove(bool hasAnimator, bool hasRoot, bool hasMatcher, bool hasControlRig, bool hasIk,
                      const PlacedKind& kind, std::string* why = nullptr) {
    if (std::string_view(kind.type) != "SkeletalMesh") return true;
    std::string deps;
    const auto add = [&](bool has, const char* label) {
        if (!has) return;
        if (!deps.empty()) deps += ", ";
        deps += label;
    };
    add(hasAnimator, "Animator");
    add(hasRoot, "Root Motion");
    add(hasMatcher, "Motion Matching");
    add(hasControlRig, "Control Rig");
    add(hasIk, "IK Rig");
    if (deps.empty()) return true;
    if (why) *why = "Remove " + deps + " first.";
    return false;
}

// One picker candidate: the content-relative path and its asset id (the content index key, fnv1a64 of the path).
struct PlacedAssetChoice {
    std::string label;
    u64 id = 0;
};

inline std::string lowerExtOf(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return e;
}

// The pure core of the asset picker's candidate list: every content-index entry whose extension is in `exts`
// and that `accept` (when given) lets through, labelled by `toRelative(absPath)`, sorted by label.
inline std::vector<PlacedAssetChoice> listAssets(
        const std::unordered_map<u64, std::string>& contentIndex, std::span<const char* const> exts,
        const std::function<std::string(const std::string&)>& toRelative,
        const std::function<bool(u64 id, const std::string& absPath)>& accept = {}) {
    std::vector<PlacedAssetChoice> out;
    for (const auto& [id, abs] : contentIndex) {
        const std::string ext = lowerExtOf(abs);
        bool wanted = false;
        for (const char* e : exts)
            if (ext == e) { wanted = true; break; }
        if (!wanted) continue;
        if (accept && !accept(id, abs)) continue;
        out.push_back({toRelative ? toRelative(abs) : abs, id});
    }
    std::sort(out.begin(), out.end(), [](const PlacedAssetChoice& a, const PlacedAssetChoice& b) {
        return a.label != b.label ? a.label < b.label : a.id < b.id;
    });
    return out;
}

// A new record with the creation defaults of the menu entry. Canonical form: keys at their default are
// omitted, so what the menu creates compares equal to what capture reads back from the component.
// The one place a creation default differs from the record's read default is MotionMatcher.input
// (velocity here, script when absent in a hand-written record).
inline fmt::OcPlacedComponent makeNewRecord(const PlacedKind& kind) {
    fmt::OcPlacedComponent r;
    r.type = kind.type;
    const std::string_view t = kind.type;
    if (t == "RootMotion") r.kv.emplace_back("mode", "apply");
    else if (t == "MotionMatcher") r.kv.emplace_back("input", "velocity");
    else if (t == "Cloth") r.kv.emplace_back("preset", "cloth_light");
    else if (t == "Destructible") r.kv.emplace_back("preset", "metal");
    return r;
}

namespace detail {
inline std::string lowerOf(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}
} // namespace detail

// "Meshes/KenneyChar.ocmesh" + a listing of content-relative paths: the .ocskel in the same folder with the
// same stem, or "" when there is none. Names a skeleton a new Skeletal Mesh can start with.
inline std::string presetSkeletonFor(const std::string& meshPath, const std::vector<std::string>& listing) {
    const std::filesystem::path mesh(meshPath);
    const std::string dir = detail::lowerOf(mesh.parent_path().generic_string());
    const std::string stem = detail::lowerOf(mesh.stem().string());
    if (stem.empty()) return {};
    for (const std::string& cand : listing) {
        const std::filesystem::path p(cand);
        if (detail::lowerOf(p.extension().string()) != ".ocskel") continue;
        if (detail::lowerOf(p.stem().string()) != stem) continue;
        if (detail::lowerOf(p.parent_path().generic_string()) != dir) continue;
        return cand;
    }
    return {};
}

} // namespace aver::editor
