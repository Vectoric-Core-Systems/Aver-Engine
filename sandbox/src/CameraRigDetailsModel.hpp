#pragma once
// The pure half of the Camera Rig Details section (docs/CAMERAS.md): the preset, mode and curve rows, what the
// COMPONENT CameraRig record keys mean and when a key is at its default (so the canonical record omits it), the record
// <-> CCameraRig mapping the section shows, the layer-mask checklist, and the near-plane warning. Header-only and
// ImGui-free so tests/editor can drive every rule; SandboxComponentDetails.cpp is the widget half.
//
// The live component is read and written through game::applyPlacedComponent / capturePlacedComponent
// (LevelComponents.cpp), so what the section edits is exactly what the level saves and reloads. rigFromRecord and
// recordFromRig here restate that mapping for the panel and the tests; the two must agree (CameraRigRecordTest and
// CameraRigDetailsModelTest pin the same golden strings).
//
// Canonical record: `preset` always written; every other key only when the value differs from the PRESET's value, so a
// VehicleChase record stays short. `target` is `self` (the rig's own entity, the default) or `parent`; anything else is
// runtime. The runtime-only ignoreBody and the OUT fields are never saved.
#include "aver/camera/CameraRigPresets.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace aver::editor::cammodel {

using camera::CameraMode;
using camera::CameraPreset;
using camera::CCameraRig;

// ---- presets, modes, curves ---------------------------------------------------------------------------------------

struct PresetRow { CameraPreset preset; const char* label; const char* key; };
inline constexpr std::array<PresetRow, 7> kPresetRows = {{
    {CameraPreset::Chase,        "Chase",         "chase"},
    {CameraPreset::VehicleChase, "Vehicle chase", "vehiclechase"},
    {CameraPreset::Orbit,        "Orbit",         "orbit"},
    {CameraPreset::FirstPerson,  "First person",  "firstperson"},
    {CameraPreset::Seat,         "Seat",          "seat"},
    {CameraPreset::TopDown,      "Top down",      "topdown"},
    {CameraPreset::Fixed,        "Fixed",         "fixed"},
}};

struct ModeRow { CameraMode mode; const char* label; const char* key; };
inline constexpr std::array<ModeRow, 6> kModeRows = {{
    {CameraMode::Chase,       "Chase",        "chase"},
    {CameraMode::Orbit,       "Orbit",        "orbit"},
    {CameraMode::FirstPerson, "First person", "firstperson"},
    {CameraMode::Seat,        "Seat",         "seat"},
    {CameraMode::TopDown,     "Top down",     "topdown"},
    {CameraMode::Fixed,       "Fixed",        "fixed"},
}};

struct CurveRow { camera::BlendCurve curve; const char* label; const char* key; };
inline constexpr std::array<CurveRow, 4> kCurveRows = {{
    {camera::BlendCurve::EaseInOut, "Ease in-out", "easeinout"},
    {camera::BlendCurve::Linear,    "Linear",      "linear"},
    {camera::BlendCurve::EaseIn,    "Ease in",     "easein"},
    {camera::BlendCurve::EaseOut,   "Ease out",    "easeout"},
}};

inline std::string lowerOf(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

inline const PresetRow& presetRow(CameraPreset p) {
    for (const PresetRow& r : kPresetRows) if (r.preset == p) return r;
    return kPresetRows[0];
}
inline bool presetFromKey(std::string_view key, CameraPreset& out) {
    const std::string l = lowerOf(key);
    for (const PresetRow& r : kPresetRows) if (l == r.key) { out = r.preset; return true; }
    return false;
}
inline const ModeRow& modeRow(CameraMode m) {
    for (const ModeRow& r : kModeRows) if (r.mode == m) return r;
    return kModeRows[0];
}
inline bool modeFromKey(std::string_view key, CameraMode& out) {
    const std::string l = lowerOf(key);
    for (const ModeRow& r : kModeRows) if (l == r.key) { out = r.mode; return true; }
    return false;
}
inline bool curveFromKey(std::string_view key, camera::BlendCurve& out) {
    const std::string l = lowerOf(key);
    for (const CurveRow& r : kCurveRows) if (l == r.key) { out = r.curve; return true; }
    return false;
}
inline const CurveRow& curveRow(camera::BlendCurve c) {
    for (const CurveRow& r : kCurveRows) if (r.curve == c) return r;
    return kCurveRows[0];
}

// ---- record keys ------------------------------------------------------------------------------------------------------

inline void setKey(fmt::OcPlacedComponent& r, const std::string& key, const std::string& value) {
    r.kv.erase(std::remove_if(r.kv.begin(), r.kv.end(), [&](const auto& kv) { return kv.first == key; }), r.kv.end());
    if (!value.empty()) r.kv.emplace_back(key, value);
}

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

inline std::string hexText(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
}

inline bool parseHex(std::string_view text, u32& out) {
    if (text.empty()) return false;
    const std::string s(text);
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (!end || *end != '\0') return false;
    out = static_cast<u32>(v);
    return true;
}

// One value key and the CCameraRig member it writes, in CANONICAL ORDER (the order docs/formats/FORMAT_SPECS.md lists them and
// the order LevelComponents.cpp writes them). A Float is one f32, a Vec3 is x,y,z over three consecutive f32 members, a Curve
// is a BlendCurve written by name, a Mask is a hex layer bitmask.
enum class SlotKind : u8 { Float, Vec3, Curve, Mask };
struct Slot { const char* key; SlotKind kind; usize offset; };
#define AVER_CAMSLOT(k, kind, member) Slot{k, SlotKind::kind, offsetof(CCameraRig, member)}
inline constexpr Slot kSlots[] = {
    AVER_CAMSLOT("arm", Float, armLengthCm), AVER_CAMSLOT("minarm", Float, minArmCm), AVER_CAMSLOT("probe", Float, probeRadiusCm),
    AVER_CAMSLOT("recover", Float, recoverHalfLifeS), AVER_CAMSLOT("socket", Vec3, socketX), AVER_CAMSLOT("pivot", Vec3, pivotX),
    AVER_CAMSLOT("lag", Float, posLagSpeed), AVER_CAMSLOT("lagmax", Float, lagMaxDistCm), AVER_CAMSLOT("rotlag", Float, rotLagSpeed),
    AVER_CAMSLOT("rotlagmax", Float, rotLagMaxDeg), AVER_CAMSLOT("lagstep", Float, lagMaxStepS), AVER_CAMSLOT("ahead", Float, lookAheadS),
    AVER_CAMSLOT("aheadmax", Float, lookAheadMaxCm), AVER_CAMSLOT("pitchmin", Float, pitchMinDeg), AVER_CAMSLOT("pitchmax", Float, pitchMaxDeg),
    AVER_CAMSLOT("yaw", Float, yawDeg), AVER_CAMSLOT("pitch", Float, pitchDeg), AVER_CAMSLOT("recenter", Float, recenterDelayS),
    AVER_CAMSLOT("blend", Float, blendTimeS), AVER_CAMSLOT("curve", Curve, blendCurve), AVER_CAMSLOT("fovkick", Float, fovKickDeg),
    AVER_CAMSLOT("fovkickspeed", Float, fovKickSpeedCmS), AVER_CAMSLOT("fadestart", Float, fadeStartCm), AVER_CAMSLOT("fadeend", Float, fadeEndCm),
    AVER_CAMSLOT("bob", Float, bobAmount), AVER_CAMSLOT("seatyaw", Float, seatYawLimitDeg), AVER_CAMSLOT("seatpitch", Float, seatPitchLimitDeg),
    AVER_CAMSLOT("mask", Mask, collisionMask),
};
#undef AVER_CAMSLOT

// A flag key: `1` (the default) means the bit is CLEAR for the negative-sense flags, `0` for the positive ones.
struct FlagKey { const char* key; u32 bit; bool negative; };
inline constexpr FlagKey kFlagKeys[] = {
    {"collision", camera::kRigNoCollision, true}, {"inheritpitch", camera::kRigNoInheritPitch, true},
    {"inherityaw", camera::kRigNoInheritYaw, true}, {"inheritroll", camera::kRigNoInheritRoll, true},
    {"fade", camera::kRigNoFade, true}, {"lookat", camera::kRigLookAtTarget, false}, {"disabled", camera::kRigDisabled, false},
};
inline constexpr u32 kAuthoredFlags = camera::kRigNoCollision | camera::kRigNoInheritPitch | camera::kRigNoInheritYaw | camera::kRigNoInheritRoll |
                                      camera::kRigNoFade | camera::kRigLookAtTarget | camera::kRigDisabled;

inline f32 floatAt(const CCameraRig& r, usize off) { f32 v; std::memcpy(&v, reinterpret_cast<const unsigned char*>(&r) + off, sizeof v); return v; }
inline void setFloatAt(CCameraRig& r, usize off, f32 v) { std::memcpy(reinterpret_cast<unsigned char*>(&r) + off, &v, sizeof v); }
inline u32 u32At(const CCameraRig& r, usize off) { u32 v; std::memcpy(&v, reinterpret_cast<const unsigned char*>(&r) + off, sizeof v); return v; }
inline void setU32At(CCameraRig& r, usize off, u32 v) { std::memcpy(reinterpret_cast<unsigned char*>(&r) + off, &v, sizeof v); }

// What `target=` means in a level: the rig's own entity (default) or its parent.
enum class TargetKind : u8 { Self, Parent };

// The preset a record names (Chase when absent), false for an unknown key.
inline bool presetOf(const fmt::OcPlacedComponent& r, CameraPreset& out) {
    out = CameraPreset::Chase;
    if (const std::string* v = r.find("preset")) return presetFromKey(*v, out);
    return true;
}

// The rig a record describes, ignoring entity ids: the preset's values with every key applied over them. False on an
// unknown preset, mode or curve (`why` says which).
inline bool rigFromRecord(const fmt::OcPlacedComponent& r, CCameraRig& out, TargetKind& target, std::string* why = nullptr) {
    CameraPreset preset = CameraPreset::Chase;
    if (!presetOf(r, preset)) { if (why) *why = "unknown preset '" + *r.find("preset") + "'"; return false; }
    out = camera::presetRig(preset);
    if (const std::string* v = r.find("mode")) {
        CameraMode m;
        if (!modeFromKey(*v, m)) { if (why) *why = "unknown mode '" + *v + "'"; return false; }
        out.mode = static_cast<u32>(m);
    }
    for (const Slot& k : kSlots) {
        const std::string* v = r.find(k.key);
        if (!v) continue;
        switch (k.kind) {
            case SlotKind::Float: setFloatAt(out, k.offset, numberOf(r, k.key, floatAt(out, k.offset))); break;
            case SlotKind::Vec3: {
                // x,y,z; an axis that does not parse keeps the preset's value.
                usize at = 0;
                for (int axis = 0; axis < 3 && at <= v->size(); ++axis) {
                    const usize comma = v->find(',', at);
                    double d = 0.0;
                    if (parseNumber(std::string_view(*v).substr(at, comma == std::string::npos ? std::string::npos : comma - at), d))
                        setFloatAt(out, k.offset + static_cast<usize>(axis) * sizeof(f32), static_cast<f32>(d));
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
                break;
            }
            case SlotKind::Curve: {
                camera::BlendCurve c;
                if (!curveFromKey(*v, c)) { if (why) *why = "unknown curve '" + *v + "'"; return false; }
                out.blendCurve = static_cast<u32>(c);
                break;
            }
            case SlotKind::Mask: { u32 m = 0; if (parseHex(*v, m)) out.collisionMask = m; break; }
        }
    }
    for (const FlagKey& k : kFlagKeys) {
        if (!r.find(k.key)) continue;
        const bool on = boolOf(r, k.key, k.negative);
        const bool setBit = k.negative ? !on : on;
        out.flags = setBit ? (out.flags | k.bit) : (out.flags & ~k.bit);
    }
    target = TargetKind::Self;
    if (const std::string* v = r.find("target")) target = lowerOf(*v) == "parent" ? TargetKind::Parent : TargetKind::Self;
    return true;
}

// The canonical record of a rig: `preset` always, then every key whose value differs from the PRESET's, in canonical
// order. Authored state only (ignoreBody, the OUT fields and the one-shot flag bits are not part of it).
inline fmt::OcPlacedComponent recordFromRig(const CCameraRig& rig, TargetKind target = TargetKind::Self) {
    fmt::OcPlacedComponent rec;
    rec.type = "CameraRig";
    const CameraPreset preset = rig.preset < camera::kCameraPresetCount ? static_cast<CameraPreset>(rig.preset) : CameraPreset::Chase;
    const CCameraRig base = camera::presetRig(preset);
    rec.kv.emplace_back("preset", presetRow(preset).key);
    if (rig.mode != base.mode) rec.kv.emplace_back("mode", modeRow(static_cast<CameraMode>(rig.mode < camera::kCameraModeCount ? rig.mode : 0)).key);
    if (target == TargetKind::Parent) rec.kv.emplace_back("target", "parent");
    for (const Slot& k : kSlots) {
        switch (k.kind) {
            case SlotKind::Float:
                if (floatAt(rig, k.offset) != floatAt(base, k.offset)) rec.kv.emplace_back(k.key, numText(static_cast<double>(floatAt(rig, k.offset))));
                break;
            case SlotKind::Vec3: {
                bool differs = false;
                for (int axis = 0; axis < 3; ++axis) {
                    const usize off = k.offset + static_cast<usize>(axis) * sizeof(f32);
                    differs = differs || floatAt(rig, off) != floatAt(base, off);
                }
                if (!differs) break;
                std::string v;
                for (int axis = 0; axis < 3; ++axis) {
                    if (axis) v += ',';
                    v += numText(static_cast<double>(floatAt(rig, k.offset + static_cast<usize>(axis) * sizeof(f32))));
                }
                rec.kv.emplace_back(k.key, v);
                break;
            }
            case SlotKind::Curve:
                if (rig.blendCurve != base.blendCurve) rec.kv.emplace_back(k.key, curveRow(camera::curveOf(rig.blendCurve)).key);
                break;
            case SlotKind::Mask:
                if (rig.collisionMask != base.collisionMask) rec.kv.emplace_back(k.key, hexText(rig.collisionMask));
                break;
        }
    }
    for (const FlagKey& k : kFlagKeys) {
        const bool isSet = (rig.flags & k.bit) != 0, baseSet = (base.flags & k.bit) != 0;
        if (isSet == baseSet) continue;
        const bool keyValue = k.negative ? !isSet : isSet;
        rec.kv.emplace_back(k.key, keyValue ? "1" : "0");
    }
    return rec;
}

// Picking a preset in the combo applies its values: the record becomes `preset=<key>` plus what is not a tuning value
// (the target and the layer mask). One undo step, so the whole record is replaced.
inline fmt::OcPlacedComponent applyPreset(const fmt::OcPlacedComponent& r, CameraPreset p) {
    fmt::OcPlacedComponent out;
    out.type = "CameraRig";
    out.kv.emplace_back("preset", presetRow(p).key);
    if (const std::string* t = r.find("target")) out.kv.emplace_back("target", *t);
    if (const std::string* m = r.find("mask")) out.kv.emplace_back("mask", *m);
    return out;
}

// Drops every key at the preset's value and puts the rest in canonical order. Idempotent.
inline fmt::OcPlacedComponent canonicalize(const fmt::OcPlacedComponent& r) {
    CCameraRig rig;
    TargetKind target = TargetKind::Self;
    if (!rigFromRecord(r, rig, target)) return r;   // an unknown value is kept as written: nothing to normalise it against
    fmt::OcPlacedComponent out = recordFromRig(rig, target);
    // Unknown keys survive untouched, after the known ones.
    for (const auto& kv : r.kv) {
        bool known = kv.first == "preset" || kv.first == "mode" || kv.first == "target";
        for (const Slot& k : kSlots) known = known || kv.first == k.key;
        for (const FlagKey& k : kFlagKeys) known = known || kv.first == k.key;
        if (!known) out.kv.push_back(kv);
    }
    return out;
}

// ---- the layer-mask checklist -----------------------------------------------------------------------------------------

// The mask the probe really uses: a rig's own, else the system default (all layers except cloth and proxy).
inline u32 effectiveMask(u32 rigMask, u32 systemDefault) { return rigMask != 0 ? rigMask : systemDefault; }

inline bool layerOn(u32 mask, u32 layer) { return layer < 32 && (mask & (1u << layer)) != 0; }

// Toggles one layer. The checklist starts from the effective mask, so ticking a layer writes an explicit mask.
inline u32 toggleLayer(u32 mask, u32 layer) { return layer < 32 ? (mask ^ (1u << layer)) : mask; }

// ---- warnings -----------------------------------------------------------------------------------------------------------

// The probe radius below which the camera sees through the surface it stopped on (the near plane's corner), or 0 when the
// rig has no probe.
inline f32 nearPlaneWarningRadius(const CCameraRig& rig, f32 nearCm, f32 fovYRad, f32 aspect) {
    const CameraMode m = rig.mode < camera::kCameraModeCount ? static_cast<CameraMode>(rig.mode) : CameraMode::Chase;
    const bool hasProbe = (m == CameraMode::Chase || m == CameraMode::Orbit || m == CameraMode::TopDown) && !(rig.flags & camera::kRigNoCollision);
    return hasProbe ? camera::nearCornerRadiusCm(nearCm, fovYRad, aspect) : 0.0f;
}

inline bool probeTooSmall(const CCameraRig& rig, f32 nearCm, f32 fovYRad, f32 aspect) {
    const f32 need = nearPlaneWarningRadius(rig, nearCm, fovYRad, aspect);
    return need > 0.0f && camera::resolveTuning(rig).probeCm < need;
}

// One line for the Details panel's read-only OUT row in Play.
inline std::string outLine(const CCameraRig& rig) {
    char b[160];
    std::snprintf(b, sizeof b, "arm %.0f cm   fov %.1f%s%s%s%s", static_cast<double>(rig.outArmCm), static_cast<double>(rig.outFovDeg),
                  (rig.outState & camera::kRigStateColliding) ? "   colliding" : "", (rig.outState & camera::kRigStateFaded) ? "   faded" : "",
                  (rig.outState & camera::kRigStateBlending) ? "   blending" : "", (rig.outState & camera::kRigStateLagging) ? "   lagging" : "");
    return b;
}

}  // namespace aver::editor::cammodel
