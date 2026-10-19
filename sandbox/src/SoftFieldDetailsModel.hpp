#pragma once
// The pure half of the Soft Field Details section and the soft-body Tuning HUD (docs/SOFTBODY.md "Fields", "Tuning"):
// the kind and shape rows, what the COMPONENT SoftField record keys mean and when a key is at its default (so the
// canonical record omits it), the "match the primitive" and "make invisible" helpers, and the Tuning HUD text. Header-only
// and ImGui-free so tests/editor can drive every rule; SoftFieldSection.cpp is the widget half.
//
// The record is read and written by game::applyPlacedComponent / capturePlacedComponent (LevelComponents.cpp, which uses the
// same word tables in Aver.SoftBody's Fields.hpp), so what the section edits is exactly what the level saves and reloads.
//
//   COMPONENT SoftField kind=radial shape=sphere extent=300,300,300 strength=600 energy=0 falloff=1 dir=0,0,1
//                       turbulence=0 targets=cloth,destructible,debris once=0 off=0
//
// Keys at their default are omitted: kind radial, shape sphere, extent 100,100,100, strength 0, energy 0, falloff 1, dir
// zero, turbulence 0, targets the kind's default (everything, debris for Kill), once 0, off 0.
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/softbody/Fields.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>

namespace aver::editor::fieldmodel {

// ---- rows -------------------------------------------------------------------------------------------------

struct KindRow {
    softbody::FieldKind kind;
    const char* label;
    const char* key;
    const char* tip;
};

inline constexpr std::array<KindRow, 6> kKindRows = {{
    {softbody::FieldKind::RadialForce,      "Radial force",      "radial",      "Pushes soft bodies away from the centre (negative strength pulls)."},
    {softbody::FieldKind::DirectionalForce, "Directional force", "directional", "Pushes soft bodies along a direction: a gust, a conveyor."},
    {softbody::FieldKind::Damage,           "Damage",            "damage",      "Deposits energy (J per second) on what is inside, breaking it past its toughness."},
    {softbody::FieldKind::ReleaseAnchors,   "Release anchors",   "release",     "Un-pins what is inside: a door falls off its hinge, a cape off its shoulders."},
    {softbody::FieldKind::Kill,             "Kill",              "kill",        "Shrinks away the debris inside (set targets to include destructibles to remove a whole body)."},
    {softbody::FieldKind::Sleep,            "Sleep",             "sleep",       "Holds what is inside asleep; it resumes when it leaves or the field is disabled."},
}};

struct ShapeRow {
    softbody::FieldShape shape;
    const char* label;
    const char* key;
};

inline constexpr std::array<ShapeRow, 4> kShapeRows = {{
    {softbody::FieldShape::Sphere,     "Sphere",     "sphere"},
    {softbody::FieldShape::Box,        "Box",        "box"},
    {softbody::FieldShape::Capsule,    "Capsule",    "capsule"},
    {softbody::FieldShape::Everywhere, "Everywhere", "all"},
}};

inline const KindRow& kindRow(softbody::FieldKind k) {
    for (const KindRow& r : kKindRows)
        if (r.kind == k) return r;
    return kKindRows[0];
}

inline const ShapeRow& shapeRow(softbody::FieldShape s) {
    for (const ShapeRow& r : kShapeRows)
        if (r.shape == s) return r;
    return kShapeRows[0];
}

// What an extent means for a shape, for the label next to the three fields.
inline const char* extentLabel(softbody::FieldShape s) {
    switch (s) {
        case softbody::FieldShape::Sphere:     return "Radius (x)";
        case softbody::FieldShape::Box:        return "Half extents";
        case softbody::FieldShape::Capsule:    return "Radius (x), half height (z)";
        default:                               return "(unbounded)";
    }
}

// ---- values <-> the record ---------------------------------------------------------------------------------

struct FieldValues {
    softbody::FieldKind kind = softbody::FieldKind::RadialForce;
    softbody::FieldShape shape = softbody::FieldShape::Sphere;
    f32 extent[3] = {100.0f, 100.0f, 100.0f};
    f32 strength = 0.0f, energy = 0.0f, falloff = 1.0f;
    f32 dir[3] = {0.0f, 0.0f, 0.0f};
    f32 turbulence = 0.0f;
    u32 targets = 0;            // 0 = the kind's default
    bool once = false, off = false;

    u32 effectiveTargets() const { return targets ? targets : softbody::fieldDefaultTargets(kind); }
    bool operator==(const FieldValues&) const = default;
};

namespace detail {

inline bool parseNumber(std::string_view s, double& out) {
    if (s.empty()) return false;
    const std::string tmp(s);
    char* end = nullptr;
    const double d = std::strtod(tmp.c_str(), &end);
    if (!end || *end != '\0') return false;
    out = d;
    return true;
}

inline std::string number(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// "1,2,3" -> three floats. False unless exactly three numbers, and then `out` is untouched (a half-read triple is not a default).
inline bool parseTriple(std::string_view s, f32* out) {
    f32 got[3];
    usize start = 0;
    for (int k = 0; k < 3; ++k) {
        const usize comma = s.find(',', start);
        if ((comma == std::string_view::npos) != (k == 2)) return false;
        double d;
        if (!parseNumber(s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start), d)) return false;
        got[k] = static_cast<f32>(d);
        start = comma + 1;
    }
    out[0] = got[0]; out[1] = got[1]; out[2] = got[2];
    return true;
}

inline std::string triple(const f32* v) { return number(v[0]) + "," + number(v[1]) + "," + number(v[2]); }

inline double numberKey(const fmt::OcPlacedComponent& r, std::string_view key, double def) {
    const std::string* v = r.find(key);
    double d = 0.0;
    return v && parseNumber(*v, d) ? d : def;
}

}  // namespace detail

// A record with defaults where it is silent or unreadable.
inline FieldValues read(const fmt::OcPlacedComponent& r) {
    FieldValues v;
    if (const std::string* s = r.find("kind")) softbody::fieldKindFromKey(*s, v.kind);
    if (const std::string* s = r.find("shape")) softbody::fieldShapeFromKey(*s, v.shape);
    if (const std::string* s = r.find("extent")) detail::parseTriple(*s, v.extent);
    if (const std::string* s = r.find("dir")) detail::parseTriple(*s, v.dir);
    v.strength = static_cast<f32>(detail::numberKey(r, "strength", 0.0));
    v.energy = static_cast<f32>(detail::numberKey(r, "energy", 0.0));
    v.falloff = static_cast<f32>(detail::numberKey(r, "falloff", 1.0));
    v.turbulence = static_cast<f32>(detail::numberKey(r, "turbulence", 0.0));
    if (const std::string* s = r.find("targets")) {
        u32 mask = 0;
        if (softbody::fieldTargetsFromText(*s, mask)) v.targets = mask == softbody::fieldDefaultTargets(v.kind) ? 0u : mask;
    }
    v.once = detail::numberKey(r, "once", 0.0) != 0.0;
    v.off = detail::numberKey(r, "off", 0.0) != 0.0;
    return v;
}

// The canonical record: every key at its default omitted, the rest in the order of the example above.
inline fmt::OcPlacedComponent write(const FieldValues& v) {
    fmt::OcPlacedComponent r;
    r.type = "SoftField";
    const auto put = [&](const char* key, std::string value) { r.kv.emplace_back(key, std::move(value)); };
    if (v.kind != softbody::FieldKind::RadialForce) put("kind", kindRow(v.kind).key);
    if (v.shape != softbody::FieldShape::Sphere) put("shape", shapeRow(v.shape).key);
    if (v.extent[0] != 100.0f || v.extent[1] != 100.0f || v.extent[2] != 100.0f) put("extent", detail::triple(v.extent));
    if (v.strength != 0.0f) put("strength", detail::number(v.strength));
    if (v.energy != 0.0f) put("energy", detail::number(v.energy));
    if (v.falloff != 1.0f) put("falloff", detail::number(v.falloff));
    if (v.dir[0] != 0.0f || v.dir[1] != 0.0f || v.dir[2] != 0.0f) put("dir", detail::triple(v.dir));
    if (v.turbulence != 0.0f) put("turbulence", detail::number(v.turbulence));
    if (v.targets != 0 && v.targets != softbody::fieldDefaultTargets(v.kind)) put("targets", softbody::fieldTargetsToText(v.targets));
    if (v.once) put("once", "1");
    if (v.off) put("off", "1");
    return r;
}

// What the Add Component menu creates: a radial sphere of 100 cm with nothing to do yet.
inline fmt::OcPlacedComponent makeNewRecord() { return write(FieldValues{}); }

// Re-spells a hand-written record canonically (drops keys at their default, normalises words and numbers).
inline fmt::OcPlacedComponent canonicalize(const fmt::OcPlacedComponent& r) { return write(read(r)); }

// ---- helpers the section offers ----------------------------------------------------------------------------

// The placed Cube is 100 cm and the Sphere primitive is taken as radius 50 cm, so these extents make the shape coincide with
// the primitive the field sits on (entity scale then scales the volume). Everywhere is unchanged.
inline constexpr f32 kUnitPrimitiveHalfCm = 50.0f;

inline void matchPrimitive(FieldValues& v) {
    switch (v.shape) {
        case softbody::FieldShape::Sphere:
            v.extent[0] = kUnitPrimitiveHalfCm;
            break;
        case softbody::FieldShape::Box:
            v.extent[0] = v.extent[1] = v.extent[2] = kUnitPrimitiveHalfCm;
            break;
        case softbody::FieldShape::Capsule:
            v.extent[0] = kUnitPrimitiveHalfCm;
            v.extent[2] = kUnitPrimitiveHalfCm;
            break;
        default: break;
    }
}

// "Make invisible volume": the primitive stops drawing and stops colliding. Returns whether anything changed.
struct VolumeFlags {
    bool visible = true;
    bool collide = true;
};

inline bool makeInvisibleVolume(VolumeFlags& f) {
    const bool changed = f.visible || f.collide;
    f.visible = false;
    f.collide = false;
    return changed;
}

inline bool isInvisibleVolume(const VolumeFlags& f) { return !f.visible && !f.collide; }

// The OUT readout in Play: "Reached 3 pieces" / "Disabled" / "Fired".
inline std::string outReadout(u32 touched, bool active, bool fired, bool disabled) {
    if (fired) return "Fired once";
    if (disabled) return "Disabled";
    if (!active) return "Idle";
    return "Reaching " + std::to_string(touched) + (touched == 1 ? " piece" : " pieces");
}

// The viewport gizmo colour of a kind (0..1 rgb): radial orange, directional blue, damage red, release yellow, kill magenta, sleep grey.
inline void kindColour(softbody::FieldKind k, f32& r, f32& g, f32& b) {
    switch (k) {
        case softbody::FieldKind::RadialForce:      r = 1.0f; g = 0.55f; b = 0.1f; break;
        case softbody::FieldKind::DirectionalForce: r = 0.2f; g = 0.5f; b = 1.0f; break;
        case softbody::FieldKind::Damage:           r = 1.0f; g = 0.15f; b = 0.15f; break;
        case softbody::FieldKind::ReleaseAnchors:   r = 1.0f; g = 0.9f; b = 0.1f; break;
        case softbody::FieldKind::Kill:             r = 1.0f; g = 0.1f; b = 1.0f; break;
        default:                                    r = 0.6f; g = 0.6f; b = 0.6f; break;
    }
}

// ---- the Tuning HUD ----------------------------------------------------------------------------------------

// A copy of softbody::HitInfo and the frame's Stats, so this header needs no scene module (the overlay fills it).
struct TuningHit {
    bool valid = false;
    u8 kind = 0;                 // 0 impact, 1 explosion, 2 gameplay
    f32 closingCmS = 0.0f, energyJ = 0.0f, radiusCm = 0.0f;
    u32 beamsDamaged = 0, broken = 0;
    f32 maxStrain = 0.0f, maxSetCm = 0.0f;
    u32 tick = 0, pieces = 0, baked = 0;
};

struct TuningFrame {
    u32 pieces = 0, activeParticles = 0, rebuilds = 0, drawBuilds = 0, fxSent = 0, fxDropped = 0;
};

inline const char* depositKindText(u8 kind) {
    switch (kind) {
        case 0:  return "impact";
        case 1:  return "explosion";
        default: return "gameplay";
    }
}

// The lines under the soft body: the last deposit in the units the tuning table (docs/SOFTBODY.md 6.2) talks in.
inline std::string tuningHitText(const TuningHit& h) {
    if (!h.valid) return "last hit: none yet";
    char b[260];
    std::snprintf(b, sizeof b, "last hit (%s, tick %u): %.1f cm/s closing, %.1f J in %.0f cm, %u beams damaged, %u broken; max strain %.4f, max set %.2f cm; %u pieces (%u baked)",
                  depositKindText(h.kind), h.tick, static_cast<double>(h.closingCmS), static_cast<double>(h.energyJ), static_cast<double>(h.radiusCm),
                  h.beamsDamaged, h.broken, static_cast<double>(h.maxStrain), static_cast<double>(h.maxSetCm), h.pieces, h.baked);
    return b;
}

inline std::string tuningFrameText(const TuningFrame& f) {
    char b[200];
    std::snprintf(b, sizeof b, "frame: pieces %u, active particles %u, rebuilds %u, draw builds %u, fx sent %u, fx dropped %u",
                  f.pieces, f.activeParticles, f.rebuilds, f.drawBuilds, f.fxSent, f.fxDropped);
    return b;
}

}  // namespace aver::editor::fieldmodel

// ---- the widget half's seam --------------------------------------------------------------------------------
// SoftFieldSection.cpp draws the Soft Field section and the Caps / Effects / Proxy / Debris subsections of the Cloth and
// Destructible sections. It cannot reach SandboxApp's undo helpers or asset pickers, so the Details code hands it these
// callbacks (each a thin wrapper over what the existing soft-body section already uses).
namespace aver::editor {

struct SoftSectionEnv {
    bool playing = false;
    std::string contentDir;                                                           // the project's Content folder
    // Run `mutate` as ONE undo step when `changed` is true (SandboxApp's instantEdit / gestureEdit for the section's kind).
    std::function<void(bool changed, const std::function<void()>& mutate)> instantEdit;
    std::function<void(bool changed, const std::function<void()>& mutate)> gestureEdit;
    // A content-relative path picker row: draws `label`, returns true when the user chose a new path (written to `path`).
    std::function<bool(const char* label, std::string& path, const char* ext)> pickAsset;
    std::function<void(const std::string&)> setStatus;                                // the status line (cbStatus_)
    // Soft Field only: the host's way of making the placed primitive invisible and non-colliding (a placement edit with
    // its own undo entry), and whether it already is. Null hides the button.
    std::function<bool()> makeInvisibleVolume;
    std::function<bool()> isInvisibleVolume;
};

#if AVER_WITH_IMGUI
// The Soft Field section for entity `e` (nothing when it has no CSoftField). `world` is the live scene.
void drawSoftFieldSection(const SoftSectionEnv& env, void* worldPtr, u32 entity);
// The four subsections of a Cloth / Destructible with a loaded .ocsoft. `editable` is false in Play or with no asset;
// edits go to `asset` and, when one finishes, are saved to absAssetPath. Returns true when the asset was saved.
//   meshRel: the entity's mesh (content-relative), for the suggested proxy path; overCap and proxyRunning: the OUT state bits.
bool drawSoftBodyDepthSections(const SoftSectionEnv& env, softbody::SoftAsset& asset, const std::string& absAssetPath, bool editable,
                               bool destructible, const std::string& meshRel, bool overCap, bool proxyRunning);
#endif

}  // namespace aver::editor

