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
#include <cmath>
#include <cstddef>
#include <cstdio>
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

// ---- wave 10: Caps, Effects, Proxy and Debris subsections (docs/SOFTBODY.md) ------------------------------------

struct CapModeRow { softbody::CapMode mode; const char* label; };
inline constexpr std::array<CapModeRow, 3> kCapModeRows = {{
    {softbody::CapMode::Auto, "Auto (on for a destructible)"},
    {softbody::CapMode::Off,  "Off"},
    {softbody::CapMode::On,   "On"},
}};

struct BackSkinRow { softbody::BackSkin mode; const char* label; const char* tip; };
inline constexpr std::array<BackSkinRow, 4> kBackSkinRows = {{
    {softbody::BackSkin::Auto,   "Auto",   "Every piece of an open sheet; only the debris of a closed mesh."},
    {softbody::BackSkin::Off,    "Off",    "Walls only: a hollow shell."},
    {softbody::BackSkin::Debris, "Debris", "Debris pieces only."},
    {softbody::BackSkin::All,    "All",    "Every piece, the root included (doubles its vertices)."},
}};

inline const char* capModeLabel(softbody::CapMode m) {
    for (const CapModeRow& r : kCapModeRows)
        if (r.mode == m) return r.label;
    return kCapModeRows[0].label;
}

inline const BackSkinRow& backSkinRow(softbody::BackSkin b) {
    for (const BackSkinRow& r : kBackSkinRows)
        if (r.mode == b) return r;
    return kBackSkinRows[0];
}

// "0.60 cm (the glass preset)" or "1.25 cm": what the thickness field shows.
inline std::string capThicknessText(const softbody::SoftAsset& a) {
    char b[80];
    if (a.caps.thicknessCm >= 0.0f) std::snprintf(b, sizeof b, "%.2f cm", static_cast<double>(a.caps.thicknessCm));
    else std::snprintf(b, sizeof b, "%.2f cm (the %s preset)", static_cast<double>(softbody::capThicknessCm(a)), softbody::presetKey(a.preset));
    return b;
}

struct FxSlotRow { softbody::FxSlot slot; const char* label; const char* tip; };
inline constexpr std::array<FxSlotRow, softbody::kFxSlotCount> kFxSlotRows = {{
    {softbody::FxSlot::Impact, "Impact",  "A rigid body hit it (closing speed above the slot's minimum)."},
    {softbody::FxSlot::Tear,   "Tear",    "Edges broke."},
    {softbody::FxSlot::Detach, "Detach",  "A piece came off."},
    {softbody::FxSlot::Dust,   "Dust",    "Fragments too small to be pieces became particles (a mesh-particle effect makes chunks)."},
    {softbody::FxSlot::Release,"Release", "Pins were released, or the body was repaired."},
}};

// Adds a sound to a slot: false when the path is empty, already there, or the slot already holds the most it can.
inline bool addSound(softbody::FxSlotParams& s, const std::string& path) {
    if (path.empty() || s.sounds.size() >= softbody::kMaxFxSounds) return false;
    for (const std::string& x : s.sounds)
        if (x == path) return false;
    s.sounds.push_back(path);
    return true;
}

inline bool removeSound(softbody::FxSlotParams& s, usize index) {
    if (index >= s.sounds.size()) return false;
    s.sounds.erase(s.sounds.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// The Proxy subsection's advice line: "" when nothing needs saying.
//   overCap: the mesh welds to more particles than sim.maxParticles allows and no proxy cage replaced it.
inline std::string proxyAdvice(bool overCap, bool proxyRunning, const std::string& proxyPath, bool trifactorAvailable) {
    if (proxyRunning) return "Simulating the proxy cage " + proxyPath + "; the render mesh follows it.";
    if (overCap) {
        std::string s = "This mesh is over the particle cap, so it is not simulated and keeps its authored shape.";
        s += trifactorAvailable ? " Create a proxy cage to simulate a simplified copy of it." : " Creating a proxy cage needs the Trifactor module in this build.";
        return s;
    }
    if (!proxyPath.empty()) return "The proxy " + proxyPath + " is named but not in use (it may still be loading, or it failed to bind: see the log).";
    return {};
}

// The one-line readout under the Play controls: which of the OUT state bits are set (the numbers are softbody::kState*).
inline std::string stateText(bool awake, bool frozen, bool released, bool assetStale, bool overCap, bool proxy) {
    std::string s = frozen ? "frozen" : (awake ? "awake" : "asleep");
    if (released) s += ", released";
    if (assetStale) s += ", asset not found";
    if (overCap) s += ", over the particle cap";
    if (proxy) s += ", on a proxy cage";
    return s;
}

// "proxy" suggested path: "Meshes/Door.ocmesh" -> "Meshes/Door.proxy.ocmesh", beside the mesh.
inline std::string proxyPathForMesh(const std::string& meshRel) {
    if (meshRel.empty()) return {};
    std::filesystem::path p(meshRel);
    const std::string stem = p.stem().string();
    p.replace_filename(stem + ".proxy.ocmesh");
    return p.generic_string();
}

// ---- wave 11: the Solver, Inflate and Group subsections of a Cloth (docs/SOFTBODY.md "Fast cloth and inflatables") ----------------

struct SolverRow { softbody::ClothSolver solver; const char* label; const char* tip; };
inline constexpr std::array<SolverRow, 2> kSolverRows = {{
    {softbody::ClothSolver::Cage, "Cage", "The beams and bends of the mesh: it can dent and tear."},
    {softbody::ClothSolver::Fast, "Fast", "Jolt makes its own edges, shear edges, bends and long range attachments: the cheap path for flags, curtains and capes. No dents, no tears."},
}};

struct BendRow { softbody::ClothBend bend; const char* label; };
inline constexpr std::array<BendRow, 3> kBendRows = {{
    {softbody::ClothBend::None, "None"},
    {softbody::ClothBend::Distance, "Distance"},
    {softbody::ClothBend::Dihedral, "Dihedral"},
}};

struct LraRow { softbody::ClothLra lra; const char* label; };
inline constexpr std::array<LraRow, 3> kLraRows = {{
    {softbody::ClothLra::Off, "Off"},
    {softbody::ClothLra::Euclidean, "Euclidean"},
    {softbody::ClothLra::Geodesic, "Geodesic"},
}};

inline const char* solverLabel(softbody::ClothSolver s) {
    for (const SolverRow& r : kSolverRows)
        if (r.solver == s) return r.label;
    return kSolverRows[0].label;
}
inline const char* bendLabel(softbody::ClothBend b) {
    for (const BendRow& r : kBendRows)
        if (r.bend == b) return r.label;
    return kBendRows[1].label;
}
inline const char* lraLabel(softbody::ClothLra l) {
    for (const LraRow& r : kLraRows)
        if (r.lra == l) return r.label;
    return kLraRows[0].label;
}

// The line under the Solver combo: "" when nothing needs saying.
inline std::string solverAdvice(softbody::ClothSolver solver, bool tearable) {
    if (solver == softbody::ClothSolver::Fast && tearable) return "A fast cloth cannot tear: Tearable is ignored (the asset is refused until it is off).";
    if (solver == softbody::ClothSolver::Fast) return "Jolt's constraints: no dents and no tears. Long range attachments keep the cloth from stretching away from its pins.";
    return {};
}

// The line under the Inflate block: "" when fine. closedKnown is false before the system has looked at the mesh (not built yet).
inline std::string inflateAdvice(f32 pressureKPa, bool closedKnown, bool closed, bool hasAsset) {
    if (!hasAsset) return "Pressure is a property of the .ocsoft asset: create one first.";
    if (pressureKPa > 0.0f && closedKnown && !closed) return "Inflate needs a closed mesh (every edge shared by two triangles): this one simulates as plain cloth.";
    if (pressureKPa > 0.0f) return "Pressure in kPa at the rest volume: about 3 for a beach ball, 12 for a basketball. A cloth with inextensible edges only bulges toward a sphere.";
    return {};
}

// A pressure the runtime accepts (what the relay op takes).
inline bool pressureOk(f32 kPa) { return std::isfinite(kPa) && kPa >= 0.0f && kPa <= 100000.0f; }

}  // namespace aver::editor::softmodel
