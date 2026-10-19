#pragma once
// The pure half of the .ocvehicle editor tab (docs/VEHICLES.md "Editor"): editing a curve without breaking its order, the
// power curve and gear top speeds computed from the spec, the axle layout and the drive-layout label, the validation list, the
// wheel add / remove bookkeeping, and the label rows. Header-only and ImGui-free over aver::vehicle::VehicleSpec so tests/editor
// can drive every rule; VehicleEditor.cpp is the widget half and VehicleEditor.hpp the tab (load, save, dirty, undo).
//
// Everything here restates what the physics builder does with a spec so the editor can show it without building a vehicle:
// the axle grouping is PhysicsVehicle.cpp's (opposite-side wheels within 5 cm of the same x, front = largest x first), the
// default engine curve and gear ratios are Jolt's, and the top speed of a gear is the wheel speed at the rev limit.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehicleSpec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace aver::editor::vehedit {

using vehicle::AxleDef;
using vehicle::DiffMode;
using vehicle::Kind;
using vehicle::VehicleSpec;
using vehicle::WheelDef;

// ---- the tab's sections ------------------------------------------------------------------------------------------------

struct SectionRow { const char* key; const char* label; };
inline constexpr SectionRow kSections[] = {
    {"body", "Body"}, {"engine", "Engine"}, {"gearbox", "Gearbox"}, {"axles", "Axles and drive"}, {"wheels", "Wheels"}, {"tyres", "Tyres"},
    {"assists", "Assists"}, {"aero", "Aero"}, {"audio", "Audio"}, {"effects", "Effects"}, {"damage", "Damage"}, {"camera", "Camera"},
};
inline constexpr usize kSectionCount = sizeof(kSections) / sizeof(kSections[0]);

// ---- curves ------------------------------------------------------------------------------------------------------------

inline constexpr f32 kMinPointGap = 0.01f;   // two points of a curve never sit closer than this in x

// A piecewise-linear value, clamped to the first and last point; 0 for an empty curve.
inline f32 curveValue(const std::vector<Vec2>& pts, f32 x) {
    if (pts.empty()) return 0.0f;
    if (x <= pts.front().x) return pts.front().y;
    if (x >= pts.back().x) return pts.back().y;
    for (usize i = 1; i < pts.size(); ++i) {
        if (x > pts[i].x) continue;
        const f32 span = pts[i].x - pts[i - 1].x;
        const f32 t = span > 0.0f ? (x - pts[i - 1].x) / span : 0.0f;
        return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t;
    }
    return pts.back().y;
}

// Jolt's engine curve, which an empty CURVE stands for: x = rpm / max rpm, y = fraction of the peak torque.
inline std::vector<Vec2> defaultTorqueCurve() { return {Vec2(0.0f, 0.8f), Vec2(0.66f, 1.0f), Vec2(1.0f, 0.8f)}; }
// Jolt's tyre friction curves (an empty TYRE stands for them): slip ratio and slip angle in degrees against mu.
inline std::vector<Vec2> defaultLongitudinalCurve() { return {Vec2(0.0f, 0.0f), Vec2(0.06f, 1.2f), Vec2(0.2f, 1.0f)}; }
inline std::vector<Vec2> defaultLateralCurve() { return {Vec2(0.0f, 0.0f), Vec2(3.0f, 1.2f), Vec2(20.0f, 1.0f)}; }

inline std::vector<Vec2> torqueCurveOf(const VehicleSpec& s) { return s.engine.curve.empty() ? defaultTorqueCurve() : s.engine.curve; }

// Moves point `i` to (x, y) without letting the curve lose its order: x stays kMinPointGap between its neighbours (and inside
// [xMin, xMax]), y inside [0, yMax]. Returns whether the point changed.
inline bool setCurvePoint(std::vector<Vec2>& pts, usize i, f32 x, f32 y, f32 xMin, f32 xMax, f32 yMax) {
    if (i >= pts.size()) return false;
    const f32 lo = i == 0 ? xMin : pts[i - 1].x + kMinPointGap;
    const f32 hi = i + 1 == pts.size() ? xMax : pts[i + 1].x - kMinPointGap;
    const f32 nx = std::min(std::max(x, lo), std::max(lo, hi));
    const f32 ny = std::min(std::max(y, 0.0f), yMax);
    const bool changed = pts[i].x != nx || pts[i].y != ny;
    pts[i] = Vec2(nx, ny);
    return changed;
}

// Inserts a point at x with the curve's value there. Returns its index, or -1 when the curve is full, x is outside [xMin, xMax] or
// within kMinPointGap of an existing point.
inline int addCurvePoint(std::vector<Vec2>& pts, f32 x, f32 xMin, f32 xMax, usize maxPoints) {
    if (pts.size() >= maxPoints || x < xMin || x > xMax) return -1;
    usize at = 0;
    while (at < pts.size() && pts[at].x < x) ++at;
    if (at < pts.size() && pts[at].x - x < kMinPointGap) return -1;
    if (at > 0 && x - pts[at - 1].x < kMinPointGap) return -1;
    pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(at), Vec2(x, curveValue(pts, x)));
    return static_cast<int>(at);
}

// Removes point `i`; a curve keeps at least two points.
inline bool removeCurvePoint(std::vector<Vec2>& pts, usize i) {
    if (i >= pts.size() || pts.size() <= 2) return false;
    pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

// ---- engine: torque and power ------------------------------------------------------------------------------------------

inline f32 torqueNm(const VehicleSpec& s, f32 rpm) {
    const f32 maxRpm = s.engine.maxRpm > 0.0f ? s.engine.maxRpm : 1.0f;
    return s.engine.torque * curveValue(torqueCurveOf(s), rpm / maxRpm);
}

// Power in kW at `rpm`: torque times angular speed.
inline f32 powerKw(const VehicleSpec& s, f32 rpm) { return torqueNm(s, rpm) * rpm * (kTwoPi / 60.0f) * 0.001f; }

// The power curve the Engine section plots beside the torque curve: (rpm, kW) over minrpm..maxrpm, `samples` points (at least 2).
inline std::vector<Vec2> powerCurve(const VehicleSpec& s, usize samples = 48) {
    std::vector<Vec2> out;
    samples = std::max<usize>(samples, 2);
    for (usize i = 0; i < samples; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(samples - 1);
        const f32 rpm = s.engine.minRpm + (s.engine.maxRpm - s.engine.minRpm) * t;
        out.emplace_back(rpm, powerKw(s, rpm));
    }
    return out;
}

struct EnginePeaks { f32 torqueNm = 0, torqueRpm = 0, powerKw = 0, powerRpm = 0; };
inline EnginePeaks enginePeaks(const VehicleSpec& s, usize samples = 256) {
    EnginePeaks p;
    samples = std::max<usize>(samples, 2);
    for (usize i = 0; i < samples; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(samples - 1);
        const f32 rpm = s.engine.minRpm + (s.engine.maxRpm - s.engine.minRpm) * t;
        const f32 tq = torqueNm(s, rpm), kw = powerKw(s, rpm);
        if (tq > p.torqueNm) { p.torqueNm = tq; p.torqueRpm = rpm; }
        if (kw > p.powerKw) { p.powerKw = kw; p.powerRpm = rpm; }
    }
    return p;
}

// ---- axles and the drive layout ----------------------------------------------------------------------------------------

inline constexpr f32 kAxleToleranceCm = 5.0f;   // PhysicsVehicle.cpp's own

struct AxleSlot { int left = -1, right = -1; };   // wheel indices; -1 = no wheel on that side

// The axles the physics builder will make: wheels on opposite sides within 5 cm of the same x are a pair, front (largest x) first.
inline std::vector<AxleSlot> axleLayout(const VehicleSpec& s) {
    std::vector<AxleSlot> axles;
    std::vector<bool> taken(s.wheels.size(), false);
    for (usize i = 0; i < s.wheels.size(); ++i) {
        if (taken[i]) continue;
        taken[i] = true;
        AxleSlot a;
        (s.wheels[i].hub.y > 0.0f ? a.right : a.left) = static_cast<int>(i);
        for (usize j = i + 1; j < s.wheels.size(); ++j) {
            if (taken[j]) continue;
            const f32 yi = s.wheels[i].hub.y, yj = s.wheels[j].hub.y;
            const bool opposite = (yi < 0.0f && yj > 0.0f) || (yi > 0.0f && yj < 0.0f);
            const bool sameX = std::fabs(s.wheels[j].hub.x - s.wheels[i].hub.x) <= kAxleToleranceCm;
            if (!opposite || !sameX) continue;
            (yj > 0.0f ? a.right : a.left) = static_cast<int>(j);
            taken[j] = true;
            break;
        }
        axles.push_back(a);
    }
    const auto xOf = [&](const AxleSlot& a) { return s.wheels[static_cast<usize>(a.left >= 0 ? a.left : a.right)].hub.x; };
    std::stable_sort(axles.begin(), axles.end(), [&](const AxleSlot& l, const AxleSlot& r) { return xOf(l) > xOf(r); });
    return axles;
}

// The axle a wheel is on (index into axleLayout), or -1.
inline int axleOfWheel(const VehicleSpec& s, usize wheel) {
    const std::vector<AxleSlot> axles = axleLayout(s);
    for (usize i = 0; i < axles.size(); ++i)
        if (axles[i].left == static_cast<int>(wheel) || axles[i].right == static_cast<int>(wheel)) return static_cast<int>(i);
    return -1;
}

// An axle's record: the spec's AXLE n, or the defaults (what the builder uses when there is none).
inline AxleDef axleDef(const VehicleSpec& s, usize axle) { return axle < s.axles.size() ? s.axles[axle] : AxleDef{}; }

// Makes spec.axles as long as the layout, padding with defaults and dropping records past the last axle. Called before an axle
// property is edited, so editing axle 1 of a four-wheeler does not leave a hole.
inline void ensureAxles(VehicleSpec& s) {
    s.axles.resize(axleLayout(s).size());
}

// Which axles carry a driven wheel (one entry per layout axle).
inline std::vector<bool> drivenAxles(const VehicleSpec& s) {
    const std::vector<AxleSlot> axles = axleLayout(s);
    std::vector<bool> out(axles.size(), false);
    for (usize i = 0; i < axles.size(); ++i) {
        const bool l = axles[i].left >= 0 && s.wheels[static_cast<usize>(axles[i].left)].driven;
        const bool r = axles[i].right >= 0 && s.wheels[static_cast<usize>(axles[i].right)].driven;
        out[i] = l || r;
    }
    return out;
}

// "FWD", "RWD", "AWD", "4x4" (all four driven through a locked centre), "6x4" and the like for more than two axles, "Tracked", or a
// description when the driven wheels do not form a layout ("Partial", "Undriven").
inline std::string driveLayout(const VehicleSpec& s) {
    if (s.kind == Kind::Tracked) return "Tracked";
    usize drivenWheels = 0;
    for (const WheelDef& w : s.wheels) drivenWheels += w.driven ? 1 : 0;
    if (drivenWheels == 0) return "Undriven";
    const std::vector<AxleSlot> axles = axleLayout(s);
    const std::vector<bool> driven = drivenAxles(s);
    usize drivenAxleCount = 0;
    bool fullPairs = true;
    for (usize i = 0; i < axles.size(); ++i) {
        if (!driven[i]) continue;
        ++drivenAxleCount;
        const bool l = axles[i].left >= 0 && s.wheels[static_cast<usize>(axles[i].left)].driven;
        const bool r = axles[i].right >= 0 && s.wheels[static_cast<usize>(axles[i].right)].driven;
        const bool paired = axles[i].left >= 0 && axles[i].right >= 0;
        if (paired && !(l && r)) fullPairs = false;
    }
    if (!fullPairs) return "Partial (" + std::to_string(drivenWheels) + " of " + std::to_string(s.wheels.size()) + " wheels)";
    if (axles.size() >= 3) return std::to_string(s.wheels.size()) + "x" + std::to_string(drivenWheels);
    if (axles.size() == 2) {
        if (drivenAxleCount == 2) return s.centerPresent && s.center == DiffMode::Locked ? "4x4" : "AWD";
        return driven[0] ? "FWD" : "RWD";
    }
    return "Single axle";
}

// ---- gearbox -----------------------------------------------------------------------------------------------------------

// Jolt's five forward ratios and its one reverse (a magnitude), which an empty GEARBOX stands for.
inline std::vector<f32> defaultForwardRatios() { return {2.66f, 1.78f, 1.3f, 1.0f, 0.74f}; }
inline std::vector<f32> defaultReverseRatios() { return {2.9f}; }

inline std::vector<f32> forwardRatios(const VehicleSpec& s) { return s.gearbox.forward.empty() ? defaultForwardRatios() : s.gearbox.forward; }
inline std::vector<f32> reverseRatios(const VehicleSpec& s) { return s.gearbox.reverse.empty() ? defaultReverseRatios() : s.gearbox.reverse; }

// The final drive and wheel radius the top speed uses: the first driven axle's and its driven wheel's class radius (a tracked
// vehicle's: the left track's driven wheel). 0 when there is nothing driven.
struct Driveline { f32 finalDrive = 0, radiusCm = 0; };
inline Driveline drivelineOf(const VehicleSpec& s) {
    Driveline d;
    if (s.kind == Kind::Tracked) {
        const vehicle::TrackDef& t = s.tracks[0];
        if (t.present && t.driven < s.wheels.size()) {
            d.finalDrive = t.finalDrive;
            if (const vehicle::WheelClass* c = vehicle::findClass(s, s.wheels[t.driven].wheelClass)) d.radiusCm = c->radius;
        }
        return d;
    }
    const std::vector<AxleSlot> axles = axleLayout(s);
    for (usize i = 0; i < axles.size(); ++i) {
        for (int w : {axles[i].left, axles[i].right}) {
            if (w < 0 || !s.wheels[static_cast<usize>(w)].driven) continue;
            d.finalDrive = axleDef(s, i).finalDrive;
            if (const vehicle::WheelClass* c = vehicle::findClass(s, s.wheels[static_cast<usize>(w)].wheelClass)) d.radiusCm = c->radius;
            return d;
        }
    }
    return d;
}

// The speed (km/h) at the rev limit in a gear whose box ratio is `ratio`: engine speed over (ratio times final drive) is the wheel
// speed, times the radius. 0 for anything that is not positive.
inline f32 topSpeedKmh(f32 ratio, f32 finalDrive, f32 radiusCm, f32 maxRpm) {
    if (!(ratio > 0.0f) || !(finalDrive > 0.0f) || !(radiusCm > 0.0f) || !(maxRpm > 0.0f)) return 0.0f;
    const f32 wheelRadS = maxRpm * (kTwoPi / 60.0f) / (ratio * finalDrive);
    return wheelRadS * radiusCm * 0.036f;   // cm/s to km/h
}

struct GearRow { std::string label; f32 ratio = 0, topKmh = 0; };
inline std::vector<GearRow> gearRows(const VehicleSpec& s) {
    std::vector<GearRow> rows;
    const Driveline d = drivelineOf(s);
    const std::vector<f32> rev = reverseRatios(s), fwd = forwardRatios(s);
    for (usize i = 0; i < rev.size(); ++i)
        rows.push_back({rev.size() == 1 ? "R" : "R" + std::to_string(i + 1), rev[i], topSpeedKmh(rev[i], d.finalDrive, d.radiusCm, s.engine.maxRpm)});
    for (usize i = 0; i < fwd.size(); ++i)
        rows.push_back({std::to_string(i + 1), fwd[i], topSpeedKmh(fwd[i], d.finalDrive, d.radiusCm, s.engine.maxRpm)});
    return rows;
}

// ---- wheels ------------------------------------------------------------------------------------------------------------

inline constexpr usize kMaxWheels = vehicle::kMaxWheels;

// Adds a wheel pair (left and right) at x, mirrored about y = 0 at the widest existing half-track (or 80 cm for an empty spec).
// The class is the first wheel's, else the first class. Returns the index of the left wheel, or -1 when 16 wheels would be exceeded.
inline int addWheelPair(VehicleSpec& s, f32 x) {
    if (s.wheels.size() + 2 > kMaxWheels) return -1;
    f32 halfTrack = 0.0f;
    for (const WheelDef& w : s.wheels) halfTrack = std::max(halfTrack, std::fabs(w.hub.y));
    if (!(halfTrack > 0.0f)) halfTrack = 80.0f;
    WheelDef proto = s.wheels.empty() ? WheelDef{} : s.wheels.front();
    if (s.wheels.empty() && !s.classes.empty()) proto.wheelClass = s.classes.front().name;
    proto.steerDeg = 0.0f;
    proto.driven = false;
    proto.handbrake = false;
    proto.brakeNm = -1.0f;
    proto.node.clear();
    proto.bone.clear();
    const vehicle::WheelClass* c = vehicle::findClass(s, proto.wheelClass);
    const f32 z = c ? c->radius : proto.hub.z;
    const usize first = s.wheels.size();
    WheelDef left = proto, right = proto;
    left.name = "W" + std::to_string(first);
    right.name = "W" + std::to_string(first + 1);
    left.hub = Vec3(x, -halfTrack, z);
    right.hub = Vec3(x, halfTrack, z);
    s.wheels.push_back(left);
    s.wheels.push_back(right);
    return static_cast<int>(first);
}

// Removes wheel `i` and keeps the tracks pointing at the right wheels (a wheel leaves its track; later indices shift down; a track's
// driven wheel that was removed becomes the track's first remaining wheel).
inline bool removeWheel(VehicleSpec& s, usize i) {
    if (i >= s.wheels.size() || s.wheels.size() <= 1) return false;
    s.wheels.erase(s.wheels.begin() + static_cast<std::ptrdiff_t>(i));
    for (vehicle::TrackDef& t : s.tracks) {
        std::vector<u32> kept;
        for (u32 w : t.wheels) {
            if (w == i) continue;
            kept.push_back(w > i ? w - 1 : w);
        }
        const u32 drivenWas = t.driven;
        t.wheels = kept;
        if (drivenWas == i) t.driven = kept.empty() ? 0u : kept.front();
        else t.driven = drivenWas > i ? drivenWas - 1 : drivenWas;
    }
    return true;
}

// Switching a spec to Tracked: the left track is the wheels on the left (y < 0), the right track those on the right, each driving through
// its first wheel. Existing track settings (inertia, brake, ...) are kept; the wheel lists are rebuilt. A wheel on the centre line
// (y == 0) goes to the left track.
inline void autoTracks(VehicleSpec& s) {
    for (int side = 0; side < 2; ++side) {
        vehicle::TrackDef& t = s.tracks[side];
        t.present = true;
        t.wheels.clear();
        for (usize i = 0; i < s.wheels.size(); ++i) {
            const bool right = s.wheels[i].hub.y > 0.0f;
            if (right == (side == 1)) t.wheels.push_back(static_cast<u32>(i));
        }
        t.driven = t.wheels.empty() ? 0u : t.wheels.front();
    }
}

// ---- validation --------------------------------------------------------------------------------------------------------

struct Issue { bool error = false; std::string text; };

// What the tab's validation list shows: vehicle::validate's first problem as an error, then the advisories the builder accepts but
// the author likely did not mean. An empty list is a clean spec.
inline std::vector<Issue> validationList(const VehicleSpec& s) {
    std::vector<Issue> out;
    std::string why;
    if (!vehicle::validate(s, why)) out.push_back({true, why});

    const std::vector<f32> fwd = forwardRatios(s);
    for (usize i = 1; i < fwd.size(); ++i)
        if (fwd[i] >= fwd[i - 1]) { out.push_back({false, "Gear ratios should fall from first to last (gear " + std::to_string(i + 1) + " is not shorter than gear " + std::to_string(i) + ")."}); break; }
    if (!s.engine.curve.empty() && s.engine.curve.front().x > 0.5f)
        out.push_back({false, "The torque curve starts above half the rev range; below it the first point's torque is held."});

    for (usize i = 0; i < s.wheels.size(); ++i) {
        const WheelDef& w = s.wheels[i];
        const vehicle::WheelClass* c = vehicle::findClass(s, w.wheelClass);
        if (c && w.hub.z < c->radius - 1.0f)
            out.push_back({false, "Wheel " + w.name + ": the hub is lower than the tyre radius, so the tyre bottom sits below the origin plane."});
        if (!w.node.empty())
            for (usize j = i + 1; j < s.wheels.size(); ++j)
                if (s.wheels[j].node == w.node) { out.push_back({false, "Wheels " + w.name + " and " + s.wheels[j].name + " both drive the node '" + w.node + "'."}); break; }
    }
    if (s.kind == Kind::Wheeled) {
        const std::vector<AxleSlot> axles = axleLayout(s);
        for (usize i = 0; i < axles.size(); ++i)
            if (axles[i].left < 0 || axles[i].right < 0)
                out.push_back({false, "Axle " + std::to_string(i) + " has a wheel on one side only; its differential has nothing to split."});
        if (s.axles.size() > axles.size())
            out.push_back({false, "There are more AXLE records than axles; the extra ones are ignored."});
    }
    if (s.body.massKg < 100.0f) out.push_back({false, "A mass under 100 kg makes the tyres' load tiny; check the unit (kg)."});
    if (s.audio.present && s.audio.idle.empty() && s.audio.high.empty()) out.push_back({false, "AUDIO names no engine loop, so the vehicle is silent."});
    return out;
}

inline bool hasErrors(const std::vector<Issue>& issues) {
    for (const Issue& i : issues) if (i.error) return true;
    return false;
}

// ---- formatting for the tab -----------------------------------------------------------------------------------------

inline std::string peaksText(const EnginePeaks& p) {
    char b[160];
    std::snprintf(b, sizeof b, "peak torque %.0f N*m at %.0f rpm   peak power %.0f kW (%.0f hp) at %.0f rpm", static_cast<double>(p.torqueNm),
                  static_cast<double>(p.torqueRpm), static_cast<double>(p.powerKw), static_cast<double>(p.powerKw) * 1.34102, static_cast<double>(p.powerRpm));
    return b;
}

inline std::string gearRowText(const GearRow& g) {
    char b[96];
    if (g.topKmh > 0.0f) std::snprintf(b, sizeof b, "%s   ratio %.2f   %.0f km/h at the rev limit", g.label.c_str(), static_cast<double>(g.ratio), static_cast<double>(g.topKmh));
    else                 std::snprintf(b, sizeof b, "%s   ratio %.2f", g.label.c_str(), static_cast<double>(g.ratio));
    return b;
}

}  // namespace aver::editor::vehedit
