#pragma once
// .ocvehicle -- a vehicle as text (OC dialect, UTF-8): body, engine, gearbox, axles, wheels, tracks, driver aids,
// aero, audio, effects, damage, seat, camera and HUD. One record per line, `#` starts a comment, blank lines are
// ignored, the first record is `OCVEHICLE 1`.
//
//   OCVEHICLE 1
//   NAME Sedan                                       text to the end of the line
//   KIND wheeled|tracked
//   BODY half 220 90 35 clearance 25 com 0 0 40 mass 1300 pitchroll 60
//   BOX cx cy cz hx hy hz                            up to 8; when present they replace BODY's box
//   ENGINE torque 500 minrpm 1000 maxrpm 6000 inertia 0.5 damping 0.2
//   CURVE x y x y ...                                rpm / maxrpm ascending in 0..1, fraction of peak torque; 2..32 points
//   GEARBOX auto|manual forward r r ... reverse r ... switch s clutchrelease s latency s up f down f clutch c
//   CENTER open | lsd <ratio> | locked               between the driven axles of an all-wheel drive
//   AXLE <n> final 3.42 diff open|lsd|locked split 0.5 lsd 1.4 share 0 antiroll 1000        axle 0 is the front
//   TYRE <name> long s mu s mu ... lat deg mu deg mu ...
//   WHEELCLASS <name> radius 33 width 22 susp min max hz 2 damping 0.5 brake 1500 handbrake 2500 inertia 0.9 spindamping 0.2 tyre <name>
//   WHEEL <name> class <c> hub x y z steer 35 driven 0|1 handbrake 0|1 brake <Nm> node <entity> bone <bone>
//   TRACK left|right wheels i i ... driven i inertia 10 damping 0.5 brake 15000 final 6 steergain 1.6 pivot 0.6
//   ASSIST abs 1 absslip .. absrelease .. tcs 0 tcsslip .. steerin .. steerout .. throttlein .. throttleout .. brakein ..
//          reversebrake 1 reversespeed .. holdbrake .. holdspeed .. skidslip .. skidangle ..
//   STEERCURVE speed fraction ...                    up to four points, cm/s
//   AERO drag 0.35 downforce 0.8                     N per (m/s)^2
//   SEAT driver x y z exit x y z
//   AUDIO engine <idle loop> idle <rpm> <high loop> high <rpm> pitchlow f pitchhigh f skid <path> impact <path> shift <path>
//   EFFECT skid <path> rate 40 impact <path> count 12
//   DAMAGE health 1000 crash 600 energy 0.5 radius 80 shell <entity>
//   CAMERA modes chase hood cockpit arm 600 eye x y z hood x y z
//   HUD <path>
//
// An unknown record, or an unknown key inside a known one, is ignored with one warning for the file; a newer
// version is refused; a malformed number for a known key refuses the file. Defaults for anything missing are the
// physics ABI's, so the smallest legal file is the header, a BODY, one WHEELCLASS and a WHEEL line per wheel.
#include "aver/vehicle/VehicleSpec.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace aver::vehicle {

// Parses `text`. False with `err` set (line number and reason) on a refused file; `out` is then unspecified.
// Does not validate the vehicle as a whole: call validate() for that (the editor and buildVehicle both do).
bool parseOcVehicle(std::string_view text, VehicleSpec& out, std::string& err);

// Canonical text. parse(write(spec)) is the same spec, and write(parse(write(spec))) is the same text.
std::string writeOcVehicle(const VehicleSpec& spec);

// Whether the spec describes a vehicle the physics ABI will build: at least one and at most 16 wheels, every class
// and tyre resolved, a driven wheel (wheeled) or both tracks with their wheels (tracked), finite numbers in the ABI's
// ranges, ascending curves. On failure `why` names the first problem.
bool validate(const VehicleSpec& spec, std::string& why);

// The class a wheel names, or null.
const WheelClass* findClass(const VehicleSpec& spec, const std::string& name);
const TyreCurve* findTyre(const VehicleSpec& spec, const std::string& name);

// A parse cache keyed by an opaque string (a content path), for hosts: they give it a function that reads a
// file's text and it parses and validates once. A failure is cached too (a missing or invalid file is logged
// once, not every frame). Returned pointers stay valid until clear().
class SpecCache {
public:
    using ReadFn = std::function<bool(const std::string& path, std::string& text)>;
    const VehicleSpec* get(const std::string& path, const ReadFn& read);
    // Forgets one path (the editor saved it) or everything.
    void invalidate(const std::string& path);
    void clear();

private:
    std::map<std::string, std::unique_ptr<VehicleSpec>> entries_;   // a null entry is a cached failure
};

} // namespace aver::vehicle
