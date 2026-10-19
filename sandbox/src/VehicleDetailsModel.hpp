#pragma once
// The pure half of the Vehicle Details section (docs/VEHICLES.md "Editor"): the preset rows, what the COMPONENT Vehicle record
// keys mean and when a key is at its default (so the canonical record omits it), the record <-> values mapping the section
// shows, the live-tune table, the Create .ocvehicle path, the attach conflicts, and the formatting of the Play-time block (speed in
// km/h, the gear letter, the per-wheel table). Header-only and ImGui-free so tests/editor can drive every rule;
// VehicleDetailsSection.cpp is the widget half.
//
// The live component is read and written through game::applyPlacedComponent / capturePlacedComponent (LevelComponents.cpp), so
// what the section edits is exactly what the level saves and reloads. valuesFromRecord and recordFromValues here restate that
// mapping for the panel and the tests; the two must agree (tests/editor VehicleDetailsModelTest and the level record test pin the
// same golden strings).
//
// Canonical record, keys in this order and only when not at their default:
//   asset=<content path>        (or asset#=<16 hex> for an id the editor cannot turn back into a path)
//   preset=<car|van|truck|bus|sports|tank>   always when there is no asset; with an asset only when it is not `car`
//   power= grip= brake= damage=              scales, default 1
//   noactions= noaudio= noeffects= nowheels= indestructible=   1 when set
// Runtime state (driver, health, gear, damage taken) is never saved: a loaded level starts with an intact, unoccupied vehicle.
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor::vehmodel {

// ---- presets ----------------------------------------------------------------------------------------------------------

struct PresetRow { u32 preset; const char* label; const char* key; const char* tip; };
// In CVehicle::preset order (0 car .. 5 tank), so a combo index is the value.
inline constexpr std::array<PresetRow, 6> kPresetRows = {{
    {0, "Car",    "car",    "A saloon: rear-wheel drive, four wheels sized from the mesh."},
    {1, "Van",    "van",    "Front-wheel drive, heavier than a car."},
    {2, "Truck",  "truck",  "Heavy, rear-wheel drive, softer springs."},
    {3, "Bus",    "bus",    "The heaviest wheeled preset."},
    {4, "Sports", "sports", "Light and powerful."},
    {5, "Tank",   "tank",   "Tracked, five road wheels a side, pivot steering."},
}};

inline std::string lowerOf(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

inline const PresetRow& presetRow(u32 preset) {
    for (const PresetRow& r : kPresetRows) if (r.preset == preset) return r;
    return kPresetRows[0];
}
inline bool presetFromKey(std::string_view key, u32& out) {
    const std::string l = lowerOf(key);
    for (const PresetRow& r : kPresetRows) if (l == r.key) { out = r.preset; return true; }
    return false;
}

// ---- record keys ------------------------------------------------------------------------------------------------------

inline bool parseNumber(std::string_view text, double& out) {
    if (text.empty()) return false;
    const std::string s(text);
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0') return false;
    out = d;
    return true;
}

inline f32 numberOf(const fmt::OcPlacedComponent& r, std::string_view key, f32 def) {
    const std::string* v = r.find(key);
    double d = 0.0;
    return v && parseNumber(*v, d) ? static_cast<f32>(d) : def;
}

inline bool boolOf(const fmt::OcPlacedComponent& r, std::string_view key, bool def) {
    const std::string* v = r.find(key);
    double d = 0.0;
    return v && parseNumber(*v, d) ? d != 0.0 : def;
}

inline std::string numText(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// What a COMPONENT Vehicle record says.
struct VehicleValues {
    std::string asset;        // content-relative .ocvehicle path, empty = none
    std::string assetHex;     // asset#= : an id with no known path (only when `asset` is empty)
    u32 preset = 0;           // used when there is no asset
    f32 power = 1.0f, grip = 1.0f, brake = 1.0f, damage = 1.0f;
    bool noActions = false, noAudio = false, noEffects = false, noWheels = false, indestructible = false;
    bool operator==(const VehicleValues&) const = default;
};

inline bool hasAsset(const VehicleValues& v) { return !v.asset.empty() || !v.assetHex.empty(); }

// A scale at its default (1), or one the system reads as 1 anyway (zero or less), is not written.
inline bool scaleIsDefault(f32 v) { return !(v > 0.0f) || v == 1.0f; }

// The values a record describes, with every key applied over the defaults. False on an unknown preset (`why` says which).
inline bool valuesFromRecord(const fmt::OcPlacedComponent& r, VehicleValues& out, std::string* why = nullptr) {
    out = VehicleValues{};
    if (const std::string* v = r.find("asset")) out.asset = *v;
    if (out.asset.empty()) if (const std::string* v = r.find("asset#")) out.assetHex = *v;
    if (const std::string* v = r.find("preset")) {
        if (!presetFromKey(*v, out.preset)) { if (why) *why = "unknown preset '" + *v + "'"; return false; }
    }
    out.power = numberOf(r, "power", 1.0f);
    out.grip = numberOf(r, "grip", 1.0f);
    out.brake = numberOf(r, "brake", 1.0f);
    out.damage = numberOf(r, "damage", 1.0f);
    out.noActions = boolOf(r, "noactions", false);
    out.noAudio = boolOf(r, "noaudio", false);
    out.noEffects = boolOf(r, "noeffects", false);
    out.noWheels = boolOf(r, "nowheels", false);
    out.indestructible = boolOf(r, "indestructible", false);
    return true;
}

// The canonical record of a set of values.
inline fmt::OcPlacedComponent recordFromValues(const VehicleValues& v) {
    fmt::OcPlacedComponent rec;
    rec.type = "Vehicle";
    if (!v.asset.empty()) rec.kv.emplace_back("asset", v.asset);
    else if (!v.assetHex.empty()) rec.kv.emplace_back("asset#", v.assetHex);
    const u32 preset = v.preset < kPresetRows.size() ? v.preset : 0u;
    if (!hasAsset(v) || preset != 0) rec.kv.emplace_back("preset", presetRow(preset).key);
    if (!scaleIsDefault(v.power)) rec.kv.emplace_back("power", numText(static_cast<double>(v.power)));
    if (!scaleIsDefault(v.grip)) rec.kv.emplace_back("grip", numText(static_cast<double>(v.grip)));
    if (!scaleIsDefault(v.brake)) rec.kv.emplace_back("brake", numText(static_cast<double>(v.brake)));
    if (!scaleIsDefault(v.damage)) rec.kv.emplace_back("damage", numText(static_cast<double>(v.damage)));
    if (v.noActions) rec.kv.emplace_back("noactions", "1");
    if (v.noAudio) rec.kv.emplace_back("noaudio", "1");
    if (v.noEffects) rec.kv.emplace_back("noeffects", "1");
    if (v.noWheels) rec.kv.emplace_back("nowheels", "1");
    if (v.indestructible) rec.kv.emplace_back("indestructible", "1");
    return rec;
}

inline bool isKnownKey(std::string_view k) {
    static constexpr std::string_view known[] = {"asset", "asset#", "preset", "power", "grip", "brake", "damage",
                                                 "noactions", "noaudio", "noeffects", "nowheels", "indestructible"};
    for (std::string_view n : known) if (k == n) return true;
    return false;
}

// recordFromValues of `values`, then the record's keys this model does not know, untouched and in file order.
inline fmt::OcPlacedComponent withUnknownKeys(const fmt::OcPlacedComponent& from, const VehicleValues& values) {
    fmt::OcPlacedComponent out = recordFromValues(values);
    for (const auto& kv : from.kv) if (!isKnownKey(kv.first)) out.kv.push_back(kv);
    return out;
}

// What a new record from Add Component is: preset=car (the creation default; the canonical form of a record with no asset writes it).
inline fmt::OcPlacedComponent newRecord() { return recordFromValues(VehicleValues{}); }

// Drops every key at its default and puts the rest in canonical order. Idempotent. A record with an unknown preset is kept as written.
inline fmt::OcPlacedComponent canonicalize(const fmt::OcPlacedComponent& r) {
    VehicleValues v;
    if (!valuesFromRecord(r, v)) return r;
    return withUnknownKeys(r, v);
}

// Picking a preset in the combo: the preset becomes the vehicle's source, so an asset is dropped. One undo step.
inline fmt::OcPlacedComponent applyPreset(const fmt::OcPlacedComponent& r, u32 preset) {
    VehicleValues v;
    valuesFromRecord(r, v);
    v.asset.clear();
    v.assetHex.clear();
    v.preset = preset < kPresetRows.size() ? preset : 0u;
    return withUnknownKeys(r, v);
}

// Assigning an .ocvehicle: the asset becomes the source; the preset is left as it was (it is ignored while an asset is set). An empty
// path clears the asset and falls back to the preset.
inline fmt::OcPlacedComponent applyAsset(const fmt::OcPlacedComponent& r, const std::string& path) {
    VehicleValues v;
    valuesFromRecord(r, v);
    v.asset = path;
    v.assetHex.clear();
    return withUnknownKeys(r, v);
}

// The preset combo is shown but does nothing while an asset is set.
inline bool presetIsIgnored(const VehicleValues& v) { return hasAsset(v); }

// ---- the live-tune table ------------------------------------------------------------------------------------------------

// The keys VehiclePhysicsSystem::tune accepts (also an_vehicletune and Entity.TuneVehicle), with the range the slider offers. A toggle
// is 0 or 1 and starts "from the spec"; `integer` rows are whole numbers.
struct TuneRow { const char* key; const char* label; f32 min, max, def; bool toggle, integer; const char* tip; };
inline constexpr std::array<TuneRow, 11> kTuneRows = {{
    {"power",           "Power",            0.0f,  4.0f, 1.0f, false, false, "Engine torque scale."},
    {"brake",           "Brakes",           0.0f,  4.0f, 1.0f, false, false, "Brake torque scale."},
    {"grip",            "Grip",             0.1f,  4.0f, 1.0f, false, false, "Tyre friction scale (the contact's friction, both axes)."},
    {"suspension",      "Suspension",       0.25f, 4.0f, 1.0f, false, false, "Spring frequency scale."},
    {"steer",           "Steering lock",    0.25f, 2.0f, 1.0f, false, false, "Maximum steer angle scale."},
    {"drag",            "Drag",             0.0f,  4.0f, 1.0f, false, false, "Aerodynamic drag scale."},
    {"downforce",       "Downforce",        0.0f,  4.0f, 1.0f, false, false, "Aerodynamic downforce scale."},
    {"abs",             "ABS",              0.0f,  1.0f, 1.0f, true,  false, "Anti-lock braking, overriding the spec."},
    {"tcs",             "Traction control", 0.0f,  1.0f, 1.0f, true,  false, "Traction control, overriding the spec."},
    {"manual",          "Manual gearbox",   0.0f,  1.0f, 0.0f, true,  false, "Manual gear changes (Drive.ShiftUp / ShiftDown)."},
    {"collisionperiod", "Wheel-cast period", 1.0f, 8.0f, 1.0f, false, true,  "Run the wheel casts every Nth physics step (1 = every step)."},
}};

inline const TuneRow* findTuneRow(std::string_view key) {
    const std::string l = lowerOf(key);
    for (const TuneRow& r : kTuneRows) if (l == r.key) return &r;
    return nullptr;
}

inline f32 clampTune(const TuneRow& row, f32 value) {
    const f32 v = std::min(std::max(value, row.min), row.max);
    return row.integer ? std::round(v) : v;
}

// ---- scales in the Details section ------------------------------------------------------------------------------------

inline constexpr f32 kScaleMin = 0.05f, kScaleMax = 4.0f;
inline f32 clampScale(f32 v) { return !(v > 0.0f) ? 1.0f : std::min(std::max(v, kScaleMin), kScaleMax); }

// ---- Create .ocvehicle ------------------------------------------------------------------------------------------------

inline std::string slashed(std::string_view p) {
    std::string o(p);
    std::replace(o.begin(), o.end(), '\\', '/');
    return o;
}

// The directory part of a content-relative path ("Meshes/Car.ocmesh" -> "Meshes"; "" for none) and the stem of its file name.
inline std::string dirOf(std::string_view path) {
    const std::string p = slashed(path);
    const usize slash = p.rfind('/');
    return slash == std::string::npos ? std::string{} : p.substr(0, slash);
}
inline std::string stemOf(std::string_view path) {
    const std::string p = slashed(path);
    const usize slash = p.rfind('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const usize dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

// Where Create .ocvehicle writes for a mesh: beside it, named after it (`Meshes/Car.ocmesh` -> `Meshes/Car.ocvehicle`), then `Car_2`,
// `Car_3`, ... never over a file `exists` reports. Empty for a mesh path that has no name.
inline std::string createAssetPath(const std::string& meshRel, const std::function<bool(const std::string&)>& exists) {
    const std::string stem = stemOf(meshRel);
    if (stem.empty()) return {};
    const std::string dir = dirOf(meshRel);
    for (u32 n = 1; n < 1000; ++n) {
        std::string name = stem + (n == 1 ? std::string{} : "_" + std::to_string(n)) + ".ocvehicle";
        const std::string path = dir.empty() ? name : dir + "/" + name;
        if (!exists || !exists(path)) return path;
    }
    return {};
}

// The same over a listing of the content-relative paths that exist (case-insensitive).
inline std::string createAssetPath(const std::string& meshRel, const std::vector<std::string>& listing) {
    return createAssetPath(meshRel, std::function<bool(const std::string&)>([&](const std::string& candidate) {
        const std::string l = lowerOf(candidate);
        for (const std::string& e : listing) if (lowerOf(slashed(e)) == l) return true;
        return false;
    }));
}

// ---- attach conflicts ---------------------------------------------------------------------------------------------------

struct ConflictFacts {
    bool hasRigidBody = false;       // a CRigidBody: the vehicle is its own rigid body
    bool hasVehicleToken = false;    // the placement has a `vehicle` token (a traffic car)
    bool hasObjectClip = false;      // an object-clip CAnimator (the placement plays a transform clip)
    bool hasDestructible = false;    // a CDestructible: one body per entity root
};

// The one-line reason a Vehicle cannot be attached, or null when nothing conflicts. The mesh and the generic rules (already present,
// legacy .ocmap, prefab, ...) are canAttach's.
inline const char* attachRefusal(const ConflictFacts& f) {
    if (f.hasVehicleToken) return "This placement is a traffic car (vehicle token); remove the token before adding a Vehicle.";
    if (f.hasRigidBody) return "A Vehicle is its own rigid body; remove the Rigid Body first.";
    if (f.hasObjectClip) return "This placement plays an object animation, which would fight the vehicle's pose; remove it first.";
    if (f.hasDestructible) return "This entity is Destructible; a vehicle's chassis is one body. Put the Destructible on a child (the spec's DAMAGE shell).";
    return nullptr;
}

// ---- the Play-time block ------------------------------------------------------------------------------------------------

inline constexpr f32 kCmSToKmh = 0.036f;
inline f32 speedKmh(f32 cmPerS) { return std::fabs(cmPerS) * kCmSToKmh; }

inline std::string speedText(f32 cmPerS) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f km/h", static_cast<double>(speedKmh(cmPerS)));
    return b;
}

inline std::string gearText(i32 gear) {
    if (gear < 0) return "R";
    if (gear == 0) return "N";
    return std::to_string(gear);
}

// RPM as a 0..1 bar position between idle and the rev limit.
inline f32 rpmFraction(f32 rpm, f32 minRpm, f32 maxRpm) {
    if (!(maxRpm > minRpm)) return 0.0f;
    return std::min(std::max((rpm - minRpm) / (maxRpm - minRpm), 0.0f), 1.0f);
}

// What the panel reads from Telemetry, as plain values.
struct LiveReading {
    f32 speedCmS = 0, forwardCmS = 0, rpm = 0;
    i32 gear = 0, gearCount = 0;
    bool manual = false, shifting = false;
    f32 throttle = 0, brake = 0, steer = 0, handbrake = 0;
    i32 wheelsOnGround = 0, wheelCount = 0;
    f32 health01 = 1.0f;
    bool abs = false, tcs = false, flipped = false, destroyed = false;
};

struct WheelReading {
    bool contact = false, skidding = false;
    f32 slipLong = 0, slipLatDeg = 0, loadN = 0, compressionCm = 0, friction = 0;
};

inline constexpr std::array<const char*, 8> kWheelColumns = {"Wheel", "Ground", "Slip long", "Slip lat", "Load", "Compress", "Friction", "Skid"};

inline std::string fmtG(const char* fmtText, double v) {
    char b[48];
    std::snprintf(b, sizeof b, fmtText, v);
    return b;
}

// One row of the per-wheel table, a string per kWheelColumns entry. Slip ratio is a percentage, load kN, compression cm.
inline std::array<std::string, 8> wheelCells(u32 index, const WheelReading& w) {
    return {
        "W" + std::to_string(index),
        w.contact ? "yes" : "air",
        w.contact ? fmtG("%.0f%%", static_cast<double>(w.slipLong) * 100.0) : std::string("-"),
        w.contact ? fmtG("%.1f deg", static_cast<double>(w.slipLatDeg)) : std::string("-"),
        w.contact ? fmtG("%.1f kN", static_cast<double>(w.loadN) * 0.001) : std::string("-"),
        fmtG("%.1f cm", static_cast<double>(w.compressionCm)),
        w.contact ? fmtG("%.2f", static_cast<double>(w.friction)) : std::string("-"),
        w.skidding ? "SKID" : "",
    };
}

// The lamps beside the gear: "ABS", "TCS", "FLIPPED", "WRECKED" as they apply, space-separated.
inline std::string lampText(const LiveReading& r) {
    std::string s;
    const auto add = [&](bool on, const char* text) { if (on) { if (!s.empty()) s += ' '; s += text; } };
    add(r.abs, "ABS");
    add(r.tcs, "TCS");
    add(r.flipped, "FLIPPED");
    add(r.destroyed, "WRECKED");
    return s;
}

// One line for the Details panel's summary in Play.
inline std::string outLine(const LiveReading& r) {
    char b[160];
    std::snprintf(b, sizeof b, "%s   gear %s%s   %.0f rpm   health %.0f%%", speedText(r.speedCmS).c_str(), gearText(r.gear).c_str(),
                  r.manual ? " (manual)" : "", static_cast<double>(r.rpm), static_cast<double>(r.health01) * 100.0);
    return b;
}

// Which of the section's buttons apply: Drive needs a built, living, unoccupied vehicle; Repair needs a hurt or wrecked one; Flip a built one.
struct ButtonState { bool drive = false, exit = false, flip = false, repair = false, stop = false; };
inline ButtonState buttonsFor(bool built, bool hasDriver, const LiveReading& r) {
    ButtonState s;
    s.drive = built && !hasDriver && !r.destroyed;
    s.exit = built && hasDriver;
    s.flip = built;
    s.repair = built && (r.health01 < 1.0f || r.destroyed);
    s.stop = built;
    return s;
}

}  // namespace aver::editor::vehmodel

// ---- the widget half's seam ---------------------------------------------------------------------------------------------
// VehicleDetailsSection.cpp draws the Vehicle section's body. It cannot reach SandboxApp's undo helpers, asset picker or Content
// Browser, so the Details code hands it these callbacks (each a thin wrapper over what the Cloth section already uses).
namespace aver::editor {

struct VehicleSectionEnv {
    bool playing = false;
    // Run `mutate` as ONE undo step when `changed` is true (SandboxApp's instantEdit / gestureEdit for the Vehicle kind). Call them
    // straight after the widget: gestureEdit reads the last item's activation.
    std::function<void(bool changed, const std::function<void()>& mutate)> instantEdit;
    std::function<void(bool changed, const std::function<void()>& mutate)> gestureEdit;
    // Draws the .ocvehicle path row (current path, drop target, Change...) and applies a new path through the applier.
    std::function<void(const char* key, const char* label, u64 currentAssetId)> assetRow;
    // Content-relative path of a content id ("" when it is not under Content), and whether a content-relative file exists.
    std::function<std::string(u64)> pathOfId;
    std::function<bool(const std::string&)> fileExists;
    // Writes `text` to the content-relative `rel`, adds it to the Content Browser and assigns it to the vehicle (one undo step).
    // False (with the status line set) when it could not.
    std::function<bool(const std::string& rel, const std::string& text)> createAsset;
    // Opens the .ocvehicle in its editor tab.
    std::function<void(const std::string& rel)> openEditor;
    std::function<void(const std::string&)> setStatus;
    // The content-relative path of the entity's mesh ("" when there is none), for the Create .ocvehicle name.
    std::string meshRel;
    // The entity that gets in when Drive is pressed: the Play pawn, or 0 for a ghost driver the section makes and removes itself.
    std::function<u32()> driverEntity;
};

#if AVER_WITH_IMGUI
// The Vehicle section's body for entity `e` (nothing when it has no CVehicle). `world` is the live scene (scene::World*).
void drawVehicleSection(const VehicleSectionEnv& env, void* worldPtr, u32 entity);
#endif

}  // namespace aver::editor
