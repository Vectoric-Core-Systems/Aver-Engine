#pragma once
// The pure half of the Cloth and Destructible Details sections (docs/SOFTBODY.md): the preset rows, what the
// COMPONENT record keys mean and when a key is at its default (so the canonical record omits it), the material
// numbers the sliders show for a preset or an .ocsoft, and the bookkeeping of "Create .ocsoft". Header-only and
// ImGui-free so tests/editor can drive every rule; SandboxComponentDetails.cpp is the widget half.
//
// The component state itself is read and written through game::applyPlacedComponent / capturePlacedComponent
// (LevelComponents.cpp), so what the section edits is exactly what the level saves and reloads; this header only
// knows the record spelling.
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/softbody/Material.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

namespace aver::editor::softmodel {

// ---- presets -----------------------------------------------------------------------------------------------

struct PresetRow {
    softbody::SoftPreset preset;
    const char* label;
    const char* key;
};

inline constexpr std::array<PresetRow, 5> kPresetRows = {{
    {softbody::SoftPreset::ClothLight, "Cloth light", "cloth_light"},
    {softbody::SoftPreset::ClothHeavy, "Cloth heavy", "cloth_heavy"},
    {softbody::SoftPreset::Metal,      "Metal",       "metal"},
    {softbody::SoftPreset::Wood,       "Wood",        "wood"},
    {softbody::SoftPreset::Glass,      "Glass-like brittle", "glass"},
}};

inline const PresetRow& presetRow(softbody::SoftPreset p) {
    for (const PresetRow& r : kPresetRows)
        if (r.preset == p) return r;
    return kPresetRows[0];
}

// ---- COMPONENT record keys --------------------------------------------------------------------------------

inline const std::string* findKey(const fmt::OcPlacedComponent& r, std::string_view key) { return r.find(key); }

// Sets `key` (dropping its unresolved `key#` hex twin), or removes it when `value` is empty.
inline void setKey(fmt::OcPlacedComponent& r, const std::string& key, const std::string& value) {
    const std::string hexKey = key + "#";
    r.kv.erase(std::remove_if(r.kv.begin(), r.kv.end(), [&](const auto& kv) { return kv.first == key || kv.first == hexKey; }), r.kv.end());
    if (!value.empty()) r.kv.emplace_back(key, value);
}

inline double numberOf(const fmt::OcPlacedComponent& r, std::string_view key, double def) {
    const std::string* v = r.find(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    const double d = std::strtod(v->c_str(), &end);
    return end && *end == '\0' ? d : def;
}

// The preset a record names (the type's default when absent), false for an unknown key.
inline bool presetOf(const fmt::OcPlacedComponent& r, softbody::SoftPreset def, softbody::SoftPreset& out) {
    out = def;
    if (const std::string* v = r.find("preset")) return softbody::presetFromKey(*v, out);
    return true;
}

// Picking a preset writes its key; the asset and every override are kept (an .ocsoft's own material wins at run
// time, so the combo mostly matters for a component with no asset).
inline fmt::OcPlacedComponent applyPreset(fmt::OcPlacedComponent r, softbody::SoftPreset p) {
    setKey(r, "preset", presetRow(p).key);
    return r;
}

// The key/default pairs of each kind. A key at its default is omitted from the canonical record, so what the
// menu creates compares equal to what capture reads back.
struct KeyDefault { const char* key; double def; };
inline constexpr KeyDefault kClothKeys[] = {{"wind", 1}, {"stiffness", 1}, {"reach", 1}, {"freeze", 0}, {"tearable", 0},
                                            {"world", 1}, {"proxy", 1}, {"windon", 1}};
inline constexpr KeyDefault kDestructibleKeys[] = {{"toughness", 1}, {"impact", 1}, {"lifetime", 0}, {"maxpieces", 0},
                                                   {"indestructible", 0}, {"nodebris", 0}};

// Drops every key that is at its default. Idempotent.
inline void canonicalize(fmt::OcPlacedComponent& r) {
    const auto strip = [&](const KeyDefault* keys, usize n) {
        for (usize i = 0; i < n; ++i) {
            const std::string* v = r.find(keys[i].key);
            if (v && numberOf(r, keys[i].key, keys[i].def + 1.0) == keys[i].def) setKey(r, keys[i].key, std::string());
        }
    };
    if (r.type == "Cloth") strip(kClothKeys, std::size(kClothKeys));
    else if (r.type == "Destructible") strip(kDestructibleKeys, std::size(kDestructibleKeys));
}

// ---- the numbers the sliders show --------------------------------------------------------------------------

struct MaterialView {
    softbody::SoftPreset preset = softbody::SoftPreset::ClothLight;
    f32 areaDensityKgM2 = 0.0f, compliance = 0.0f, bendCompliance = 0.0f;
    f32 yieldStrain = 0.0f, breakStrain = 0.0f, toughnessJ = 0.0f, hardening = 0.0f;
    u32 iterations = 0;
    f32 damping = 0.0f, vertexRadiusCm = 0.0f;
    f32 dragCoeff = 0.0f, liftCoeff = 0.0f, defaultReachCm = 0.0f;
    softbody::Behavior behavior = softbody::Behavior::Deform;
};

inline MaterialView viewOf(const softbody::SoftAsset& a) {
    MaterialView v;
    v.preset = a.preset;
    v.areaDensityKgM2 = a.material.areaDensityKgM2;
    v.compliance = a.material.compliance;
    v.bendCompliance = a.material.bendCompliance;
    v.yieldStrain = a.material.yieldStrain;
    v.breakStrain = a.material.breakStrain;
    v.toughnessJ = a.material.toughnessJ;
    v.hardening = a.material.hardening;
    v.iterations = a.sim.iterations;
    v.damping = a.sim.damping;
    v.vertexRadiusCm = a.sim.vertexRadiusCm;
    v.dragCoeff = a.cloth.dragCoeff;
    v.liftCoeff = a.cloth.liftCoeff;
    v.defaultReachCm = a.cloth.defaultReachCm;
    v.behavior = a.material.behavior;
    return v;
}

// The preset alone, as the sliders show it before an .ocsoft exists.
inline MaterialView viewOfPreset(bool destructible, softbody::SoftPreset p) {
    return viewOf(softbody::makeSoftAsset(destructible ? softbody::SoftKind::Destructible : softbody::SoftKind::Cloth, p));
}

// ---- Create .ocsoft ----------------------------------------------------------------------------------------

// "Meshes/Door.ocmesh" -> "Meshes/Door.ocsoft": the asset sits beside its mesh and shares its stem.
inline std::string assetPathForMesh(const std::string& meshRel) {
    if (meshRel.empty()) return {};
    std::filesystem::path p(meshRel);
    p.replace_extension(".ocsoft");
    return p.generic_string();
}

// A starter asset for the kind and preset, ready to write.
inline softbody::SoftAsset starterAsset(bool destructible, softbody::SoftPreset p) {
    return softbody::makeSoftAsset(destructible ? softbody::SoftKind::Destructible : softbody::SoftKind::Cloth, p);
}

// Points a record at an asset path (empty clears it).
inline fmt::OcPlacedComponent assignAsset(fmt::OcPlacedComponent r, const std::string& path) {
    setKey(r, "asset", path);
    return r;
}

// What the section shows under the asset row: "" when everything is fine, else the line to show in red.
inline std::string assetProblem(u64 assetId, const std::string& assetPath, bool assetLoads) {
    if (assetId != 0 && assetPath.empty()) return "The asset is not under Content.";
    if (assetId != 0 && !assetLoads) return "The asset would not load; the preset is used (see the log).";
    return {};
}

}  // namespace aver::editor::softmodel
