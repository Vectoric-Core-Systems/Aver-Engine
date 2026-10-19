#pragma once
// The vehicle spec: everything the .ocvehicle asset says about a car, a truck or a tank, as plain data.
//
// Lengths are cm, masses kg, torques N*m, angles degrees, positions origin-relative in the engine's axes (+X
// forward, +Y right, +Z up). The origin is the BOTTOM CENTRE of the vehicle, where a vehicle mesh's own origin is.
//
// Every default below is the physics ABI's own (physics_vehicle_ext_abi.h, which are Jolt's), so a spec that
// says nothing about the driveline builds the vehicle physics_vehicle_abi.h alone would. Nothing here knows
// about a scene, a mesh or a GPU: Aver.Vehicle is Core + Formats + the physics C ABI.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::vehicle {

inline constexpr u32 kOcVehicleVersion = 1;
inline constexpr usize kMaxWheels = 16;      // the physics ABI's own ceiling
inline constexpr usize kMaxBoxes = 8;
inline constexpr usize kMaxCurvePoints = 32;
inline constexpr usize kMaxTyrePoints = 16;
inline constexpr usize kMaxSteerPoints = 4;

enum class Kind : u8 { Wheeled, Tracked };
// Jolt has no hard lock: Locked is a limited-slip ratio of 1.01.
enum class DiffMode : u8 { Open, LimitedSlip, Locked };

struct BodyDef {
    Vec3 half{220.0f, 90.0f, 35.0f};   // half extents of the chassis box
    f32 clearance = 25.0f;             // the box's underside above the ground the origin touches
    Vec3 com{0.0f, 0.0f, 40.0f};       // centre of mass
    f32 massKg = 1300.0f;
    f32 pitchRollDeg = 60.0f;          // the chassis' up stays within this of the world's; 0 or 180+ = off
};

struct BoxDef {
    f32 cx = 0, cy = 0, cz = 0, hx = 1, hy = 1, hz = 1;
};

struct EngineDef {
    f32 torque = 500.0f, minRpm = 1000.0f, maxRpm = 6000.0f;
    f32 inertia = 0.0f;                // 0 = scaled to the torque as Jolt's default engine is
    f32 damping = 0.2f;                // engine braking
    std::vector<Vec2> curve;           // (rpm / maxRpm, fraction of peak torque); empty = Jolt's curve
};

struct GearboxDef {
    bool present = false;              // a GEARBOX record exists (the writer emits one only then)
    bool manual = false;
    std::vector<f32> forward;          // empty = Jolt's five ratios
    std::vector<f32> reverse;          // positive magnitudes; empty = Jolt's one
    f32 switchTime = 0.5f, clutchRelease = 0.3f, latency = 0.5f;
    f32 up = 0.6f, down = 0.2f;        // shift points as fractions of the rev range
    f32 clutch = 0.0f;                 // 0 = scaled to the torque
};

struct AxleDef {
    f32 finalDrive = 3.42f;
    DiffMode diff = DiffMode::LimitedSlip;   // limited slip at 1.4 is Jolt's default; "open" is spelled out
    f32 split = 0.5f;
    f32 lsd = 1.4f;
    f32 share = 0.0f;                  // torque share between driven axles, 0 = equal
    f32 antiroll = 1000.0f;            // N/m, 0 = no bar
};

struct TyreCurve {
    std::string name;
    std::vector<Vec2> longitudinal;    // (slip ratio, mu)
    std::vector<Vec2> lateral;         // (slip degrees, mu)
};

struct WheelClass {
    std::string name;
    std::string tyre;                  // a TyreCurve name, or empty for Jolt's curves
    f32 radius = 33.0f, width = 22.0f;
    f32 suspMin = 5.0f, suspMax = 30.0f, suspHz = 2.0f, suspDamping = 0.5f;
    f32 brakeNm = 1500.0f, handbrakeNm = 2500.0f;
    f32 inertia = 0.9f, spinDamping = 0.2f;
};

struct WheelDef {
    std::string name;                  // free text: FL, FR, ...
    std::string wheelClass;
    std::string node;                  // the visual child entity's name
    std::string bone;                  // the skeleton bone's name, for the rig route
    Vec3 hub{0, 0, 33};                // the SETTLED wheel centre (tyre bottom on the origin plane when z == radius)
    f32 steerDeg = 0.0f;
    bool driven = false;
    bool handbrake = false;            // carries the class' handbrake torque
    f32 brakeNm = -1.0f;               // < 0 = the class'
};

struct TrackDef {
    bool present = false;
    std::vector<u32> wheels;           // indices into VehicleSpec::wheels
    u32 driven = 0;
    f32 inertia = 10.0f, damping = 0.5f, brakeNm = 15000.0f, finalDrive = 6.0f, steerGain = 1.6f, pivot = 0.6f;
};

struct AssistDef {
    bool present = false;              // an ASSIST record exists: the driver aids run in the fixed step
    bool abs = false, tcs = false;
    f32 absSlip = 0.25f, absRelease = 8.0f, tcsSlip = 0.35f;
    f32 steerIn = 3.0f, steerOut = 5.0f;
    f32 throttleIn = 6.0f, throttleOut = 8.0f, brakeIn = 8.0f;
    bool reverseBrake = true;
    f32 reverseSpeed = 50.0f, holdBrake = 0.3f, holdSpeed = 30.0f;
    f32 skidSlip = 0.45f, skidAngle = 12.0f;
};

struct SteerCurve {
    std::vector<Vec2> points;          // (speed cm/s, fraction of full lock), up to four
};

struct SeatDef {
    bool present = false;
    Vec3 driver{0, 0, 55};
    Vec3 exit{0, 0, 0};
};

struct AudioDef {
    bool present = false;
    std::string idle;                  // the idle loop (ENGINE <path>)
    std::string high;                  // the high-rev loop, may be empty (one pitched loop)
    f32 idleRpm = 1000.0f, highRpm = 6000.0f;
    f32 pitchLow = 0.8f, pitchHigh = 2.2f;
    std::string skid, impact, shift;
};

struct EffectDef {
    bool present = false;
    std::string skid;                  // .ocparticle path; smoke at skidding wheels
    f32 skidRate = 40.0f;              // particles per second per skidding wheel
    std::string impact;                // sparks
    u32 impactCount = 12;
};

struct DamageDef {
    bool present = false;
    f32 health = 1000.0f;
    f32 crash = 600.0f;                // closing cm/s at which a hit starts to hurt
    f32 energy = 0.5f;
    f32 radius = 80.0f;
    std::string shell;                 // a child entity that carries a Destructible
};

struct CameraDef {
    bool present = false;
    std::vector<std::string> modes;    // chase, hood, cockpit
    f32 arm = 600.0f;
    Vec3 eye{25.0f, -35.0f, 110.0f};
    Vec3 hood{150.0f, 0.0f, 95.0f};
};

struct VehicleSpec {
    std::string name;
    Kind kind = Kind::Wheeled;
    BodyDef body;
    std::vector<BoxDef> boxes;         // when present they replace the body box
    EngineDef engine;
    GearboxDef gearbox;
    bool centerPresent = false;
    DiffMode center = DiffMode::LimitedSlip;
    f32 centerLsd = 1.4f;
    std::vector<AxleDef> axles;        // axle 0 = front (largest x)
    std::vector<TyreCurve> tyres;
    std::vector<WheelClass> classes;
    std::vector<WheelDef> wheels;
    TrackDef tracks[2];                // left, right
    AssistDef assist;
    SteerCurve steer;
    bool aeroPresent = false;
    f32 drag = 0.0f, downforce = 0.0f; // N per (m/s)^2
    SeatDef seat;
    AudioDef audio;
    EffectDef effects;
    DamageDef damage;
    CameraDef camera;
    std::string hud;
};

} // namespace aver::vehicle
