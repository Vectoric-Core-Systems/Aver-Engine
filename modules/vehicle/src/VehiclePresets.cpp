// The built-in vehicles: the traffic presets' derivation from a mesh's bounds (moved here from
// world::VehicleSystem, unchanged), the tank, and the two fixed-geometry specs the editor offers.
//
// THE NUMBERS BELOW ARE THE OLD kPresets TABLE AND CONSTANTS, character for character, and the expressions are
// written in the same operator order: a spec built from specFromBounds must hand the physics ABI the same floats the
// old buildVehicle did (tests/vehicle compares them bit for bit).
#include "aver/vehicle/VehiclePresets.hpp"

#include "aver/vehicle/OcVehicle.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace aver::vehicle {

namespace {

struct Preset {
    const char* name;
    f32 massKg;
    f32 radiusMinCm, radiusMaxCm;   // the wheel radius is 0.17 * height, clamped to this range
    f32 engineNm;
    f32 maxSteerDeg;
    f32 suspensionHz;
    bool frontDriven;               // false = the rear axle drives
};

constexpr Preset kPresets[] = {
    {"car",    1300.0f, 28.0f, 38.0f,  500.0f, 35.0f, 2.0f, false},
    {"van",    1900.0f, 33.0f, 40.0f,  600.0f, 35.0f, 2.0f, true},
    {"truck",  6000.0f, 45.0f, 55.0f, 1500.0f, 30.0f, 1.5f, false},
    {"bus",   11000.0f, 48.0f, 55.0f, 2000.0f, 30.0f, 1.5f, false},
    {"sports", 1200.0f, 30.0f, 35.0f,  700.0f, 35.0f, 2.0f, false},
};

constexpr f32 kRadiusPerHeight = 0.17f;        // wheel radius = this * height, then clamped by the preset
constexpr f32 kWheelWidthPerRadius = 0.7f;     // a tyre is about 0.7 radii wide
constexpr f32 kChassisHeight = 0.55f;          // the chassis box is this fraction of the mesh's height ...
constexpr f32 kComHeight = 0.35f;              // ... with its centre of mass this fraction up from the origin
constexpr f32 kClearancePerRadius = 0.6f;      // ground clearance under the chassis box
constexpr f32 kAxleInsetRadii = 1.2f;          // each axle sits this many radii in from its end of the body
constexpr f32 kTrackInsetWheelWidths = 0.6f;   // each wheel centre sits this many tyre widths in from the side
constexpr f32 kSuspensionMinCm = 5.0f;
constexpr f32 kSuspensionMaxCm = 30.0f;
constexpr f32 kSuspensionDamping = 0.5f;
constexpr f32 kBrakeDecelMs2 = 9.0f;           // the stop the brakes could hold if the tyres could grip it
constexpr f32 kHandbrakeFactor = 1.5f;         // rear-axle handbrake, as a multiple of the service brake
constexpr f32 kEngineMinRpm = 1000.0f;
constexpr f32 kEngineMaxRpm = 6000.0f;
constexpr f32 kMaxPitchRollDeg = 60.0f;

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool positive(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.x > 0.0f && v.y > 0.0f && v.z > 0.0f;
}

// A four-wheeler from the old traffic derivation.
void deriveCar(const Preset& pr, const Vec3& size, VehicleSpec& s) {
    const f32 length = size.x, width = size.y, height = size.z;
    const f32 radius = std::clamp(kRadiusPerHeight * height, pr.radiusMinCm, pr.radiusMaxCm);
    const f32 wheelWidth = kWheelWidthPerRadius * radius;

    s.name = pr.name;
    s.kind = Kind::Wheeled;
    s.body.half = Vec3(0.5f * length, 0.5f * width, 0.5f * kChassisHeight * height);
    s.body.clearance = kClearancePerRadius * radius;
    s.body.com = Vec3(0.0f, 0.0f, kComHeight * height);
    s.body.massKg = pr.massKg;
    s.body.pitchRollDeg = kMaxPitchRollDeg;

    // The axle and track clamps only matter for a mesh too short or too narrow for the formula; they keep the four
    // wheels a sane rectangle instead of crossed over.
    const f32 axleX = std::max(0.5f * length - kAxleInsetRadii * radius, 0.1f * length);
    const f32 trackY = std::max(0.5f * width - kTrackInsetWheelWidths * wheelWidth, 0.25f * width);
    // A full-friction stop shared by four wheels: torque = m a r / 4.
    const f32 brake = pr.massKg * kBrakeDecelMs2 * (radius * 0.01f) * 0.25f;

    WheelClass c;
    c.name = "road";
    c.radius = radius;
    c.width = wheelWidth;
    c.suspMin = kSuspensionMinCm;
    c.suspMax = kSuspensionMaxCm;
    c.suspHz = pr.suspensionHz;
    c.suspDamping = kSuspensionDamping;
    c.brakeNm = brake;
    c.handbrakeNm = kHandbrakeFactor * brake;
    s.classes.push_back(c);

    struct W { const char* name; f32 x, y; bool front; };
    const W ws[4] = {{"FL", axleX, -trackY, true}, {"FR", axleX, trackY, true},
                     {"RL", -axleX, -trackY, false}, {"RR", -axleX, trackY, false}};
    for (const W& w : ws) {
        WheelDef d;
        d.name = w.name;
        d.wheelClass = "road";
        d.hub = Vec3(w.x, w.y, radius);
        d.steerDeg = w.front ? pr.maxSteerDeg : 0.0f;
        d.driven = w.front == pr.frontDriven;
        d.handbrake = !w.front;
        s.wheels.push_back(d);
    }
    s.engine.torque = pr.engineNm;
    s.engine.minRpm = kEngineMinRpm;
    s.engine.maxRpm = kEngineMaxRpm;
    s.axles.assign(2, AxleDef{});
}

// A tank: five road wheels a side. UNVERIFIED starting numbers, like every tuning value in the vehicle docs.
void deriveTank(const Vec3& size, VehicleSpec& s) {
    const f32 length = size.x, width = size.y, height = size.z;
    const f32 radius = std::clamp(0.22f * height, 25.0f, 60.0f);
    const f32 wheelWidth = 0.9f * radius;

    s.name = "tank";
    s.kind = Kind::Tracked;
    s.body.half = Vec3(0.5f * length, 0.5f * width, 0.25f * height);
    s.body.clearance = 0.8f * radius;
    s.body.com = Vec3(0.0f, 0.0f, 0.3f * height);
    s.body.massKg = 38000.0f;
    s.body.pitchRollDeg = 70.0f;

    const f32 endX = std::max(0.5f * length - 1.2f * radius, 0.1f * length);
    const f32 trackY = std::max(0.5f * width - 0.6f * wheelWidth, 0.25f * width);

    WheelClass c;
    c.name = "road";
    c.radius = radius;
    c.width = wheelWidth;
    c.suspMin = 5.0f;
    c.suspMax = 35.0f;
    c.suspHz = 1.8f;
    c.suspDamping = 0.6f;
    c.brakeNm = 0.0f;
    c.handbrakeNm = 0.0f;
    c.inertia = 5.0f;
    s.classes.push_back(c);

    for (int side = 0; side < 2; ++side) {
        TrackDef& t = s.tracks[side];
        t.present = true;
        t.brakeNm = 60000.0f;
        for (int i = 0; i < 5; ++i) {
            WheelDef d;
            d.name = std::string(side == 0 ? "L" : "R") + std::to_string(i);
            d.wheelClass = "road";
            d.hub = Vec3(-endX + (2.0f * endX) * static_cast<f32>(i) / 4.0f, side == 0 ? -trackY : trackY, radius);
            d.driven = (i == 4);
            t.wheels.push_back(static_cast<u32>(s.wheels.size()));
            s.wheels.push_back(d);
        }
        t.driven = t.wheels.back();
    }
    s.engine.torque = 6000.0f;   // a pivot has to beat ten wheels of lateral track grip (TrackedVehicleTest)
    s.engine.minRpm = 800.0f;
    s.engine.maxRpm = 3500.0f;
}

// ---- the fixed-geometry specs, as the text the Vehicle template ships ---------------------------------

const char* const kSedanText =
    "OCVEHICLE 1\n"
    "NAME Sedan\n"
    "KIND wheeled\n"
    "BODY half 220 90 35 clearance 25 com 0 0 40 mass 1300 pitchroll 60\n"
    "ENGINE torque 500 minrpm 1000 maxrpm 6000 inertia 0.5 damping 0.2\n"
    "CURVE 0 0.8 0.66 1 1 0.8\n"
    "GEARBOX auto forward 2.66 1.78 1.3 1 0.74 reverse 2.9 switch 0.5 clutchrelease 0.3 latency 0.5 up 0.6 down 0.2 clutch 10\n"
    "AXLE 0 final 3.42 diff open split 0.5 lsd 1.4 share 0 antiroll 1000\n"
    "AXLE 1 final 3.42 diff open split 0.5 lsd 1.4 share 0 antiroll 800\n"
    "TYRE street long 0 0 0.06 1.2 0.2 1 lat 0 0 3 1.2 20 1\n"
    "WHEELCLASS street radius 33 width 22 susp 5 30 hz 2 damping 0.5 brake 1500 handbrake 2500 inertia 0.9 spindamping 0.2 tyre street\n"
    "WHEEL FL class street hub 180 -77 33 steer 35 driven 0 handbrake 0 node WheelFL\n"
    "WHEEL FR class street hub 180 77 33 steer 35 driven 0 handbrake 0 node WheelFR\n"
    "WHEEL RL class street hub -180 -77 33 steer 0 driven 1 handbrake 1 node WheelRL\n"
    "WHEEL RR class street hub -180 77 33 steer 0 driven 1 handbrake 1 node WheelRR\n"
    "ASSIST abs 1 absslip 0.25 absrelease 8 tcs 0 tcsslip 0.35 steerin 3 steerout 5 throttlein 6 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.3 holdspeed 30 skidslip 0.45 skidangle 12\n"
    "STEERCURVE 0 1 1500 0.6 3500 0.35\n"
    "AERO drag 0.35 downforce 0.8\n"
    "SEAT driver 20 -35 55 exit -20 -140 0\n"
    "AUDIO engine Sounds/Engine_Idle.wav idle 1000 Sounds/Engine_High.wav high 6000 pitchlow 0.8 pitchhigh 2.2 skid Sounds/Skid.wav impact Sounds/Crash.wav\n"
    "EFFECT skid Particles/TyreSmoke.ocparticle rate 40 impact Particles/Sparks.ocparticle count 12\n"
    "DAMAGE health 1000 crash 600 energy 0.5 radius 80\n"
    "CAMERA modes chase hood cockpit arm 600 eye 25 -35 110 hood 150 0 95\n"
    "HUD UI/VehicleHud.ocui\n";

const char* const kTankText =
    "OCVEHICLE 1\n"
    "NAME Tank\n"
    "KIND tracked\n"
    "BODY half 350 170 45 clearance 35 com 0 0 70 mass 38000 pitchroll 70\n"
    "ENGINE torque 6000 minrpm 800 maxrpm 3500 inertia 0 damping 0.4\n"
    "GEARBOX auto forward 4 3 2 1 reverse 4 3 switch 0.6 clutchrelease 0.4 latency 0.5 up 0.6 down 0.2 clutch 0\n"
    "WHEELCLASS road radius 40 width 50 susp 5 35 hz 1.8 damping 0.6 brake 0 handbrake 0 inertia 5 spindamping 0.2\n"
    "WHEEL L0 class road hub -280 -130 40 steer 0 driven 0 handbrake 0 node WheelL0\n"
    "WHEEL L1 class road hub -140 -130 40 steer 0 driven 0 handbrake 0 node WheelL1\n"
    "WHEEL L2 class road hub 0 -130 40 steer 0 driven 0 handbrake 0 node WheelL2\n"
    "WHEEL L3 class road hub 140 -130 40 steer 0 driven 0 handbrake 0 node WheelL3\n"
    "WHEEL L4 class road hub 280 -130 40 steer 0 driven 1 handbrake 0 node WheelL4\n"
    "WHEEL R0 class road hub -280 130 40 steer 0 driven 0 handbrake 0 node WheelR0\n"
    "WHEEL R1 class road hub -140 130 40 steer 0 driven 0 handbrake 0 node WheelR1\n"
    "WHEEL R2 class road hub 0 130 40 steer 0 driven 0 handbrake 0 node WheelR2\n"
    "WHEEL R3 class road hub 140 130 40 steer 0 driven 0 handbrake 0 node WheelR3\n"
    "WHEEL R4 class road hub 280 130 40 steer 0 driven 1 handbrake 0 node WheelR4\n"
    "TRACK left wheels 0 1 2 3 4 driven 4 inertia 10 damping 0.5 brake 60000 final 6 steergain 1.6 pivot 0.6\n"
    "TRACK right wheels 5 6 7 8 9 driven 9 inertia 10 damping 0.5 brake 60000 final 6 steergain 1.6 pivot 0.6\n"
    "ASSIST abs 0 absslip 0.25 absrelease 8 tcs 0 tcsslip 0.35 steerin 2 steerout 4 throttlein 4 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.5 holdspeed 30 skidslip 0.45 skidangle 12\n"
    "SEAT driver 60 0 120 exit -100 -260 0\n"
    "AUDIO engine Sounds/Engine_Idle.wav idle 800 Sounds/Engine_High.wav high 3500 pitchlow 0.6 pitchhigh 1.6 impact Sounds/Crash.wav\n"
    "EFFECT impact Particles/Sparks.ocparticle count 16 rate 0\n"
    "DAMAGE health 3000 crash 900 energy 0.3 radius 120\n"
    "CAMERA modes chase arm 900 eye 60 0 170 hood 250 0 130\n"
    "HUD UI/VehicleHud.ocui\n";

std::optional<VehicleSpec> loadStatic(const char* text, const char* what) {
    VehicleSpec s;
    std::string err;
    if (!parseOcVehicle(text, s, err)) {
        AVER_ERROR("[Vehicle] built-in spec '{}' does not parse: {}", what, err);
        return std::nullopt;
    }
    return s;
}

}  // namespace

const char* presetName(u32 preset) {
    if (preset < 5) return kPresets[preset].name;
    return preset == 5 ? "tank" : nullptr;
}

int presetIndex(std::string_view name) {
    const std::string n = lower(name);
    for (u32 i = 0; i < kPresetCount; ++i)
        if (n == presetName(i)) return static_cast<int>(i);
    return -1;
}

const VehicleSpec* builtinSpec(std::string_view name) {
    const std::string n = lower(name);
    // Function-local statics: built once on first use, alive until the process ends.
    static const std::optional<VehicleSpec> sedan = loadStatic(kSedanText, "sedan");
    static const std::optional<VehicleSpec> tank = loadStatic(kTankText, "tank");
    if (n == "sedan") return sedan ? &*sedan : nullptr;
    if (n == "tank") return tank ? &*tank : nullptr;
    return nullptr;
}

bool specFromBounds(std::string_view preset, const Vec3& sizeCm, VehicleSpec& out) {
    const int idx = presetIndex(preset);
    if (idx < 0 || !positive(sizeCm)) return false;
    out = VehicleSpec{};
    if (idx == 5) deriveTank(sizeCm, out);
    else deriveCar(kPresets[idx], sizeCm, out);
    return true;
}

}  // namespace aver::vehicle
