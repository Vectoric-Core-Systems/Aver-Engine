// The .ocvehicle editor tab's rules without a window: editing a curve without losing its order, the torque and power numbers, the
// axle grouping and the drive-layout label (FWD / RWD / AWD / 4x4 / 6x4 / Tracked), each gear's top speed, adding and removing wheels,
// and the validation list. Header-only model (sandbox/src/VehicleEditorModel.hpp) over aver::vehicle::VehicleSpec; the widgets and the
// viewport preview are ImGui and covered by the owner's headed run.
#include "VehicleEditorModel.hpp"

#include "aver/core/Log.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace aver;
namespace ve = aver::editor::vehedit;
using aver::vehicle::VehicleSpec;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

// A bare wheeled spec: one class of radius 33 and a left / right pair at each x. Wheel index = 2 * axleInput + (0 left, 1 right).
static VehicleSpec makeSpec(std::initializer_list<f32> xs) {
    VehicleSpec s;
    vehicle::WheelClass c;
    c.name = "c";
    c.radius = 33.0f;
    s.classes.push_back(c);
    int i = 0;
    for (const f32 x : xs) {
        for (int side = 0; side < 2; ++side) {
            vehicle::WheelDef w;
            w.name = std::string(side ? "R" : "L") + std::to_string(i);
            w.wheelClass = "c";
            w.hub = Vec3(x, side ? 77.0f : -77.0f, 33.0f);
            s.wheels.push_back(w);
        }
        ++i;
    }
    return s;
}

static void drive(VehicleSpec& s, std::initializer_list<usize> wheels) {
    for (vehicle::WheelDef& w : s.wheels) w.driven = false;
    for (const usize i : wheels) s.wheels[i].driven = true;
}

int main() {
    AVER_INFO("VehicleEditorModelTest");

    const vehicle::VehicleSpec* sedanPtr = vehicle::builtinSpec("sedan");
    const vehicle::VehicleSpec* tankPtr = vehicle::builtinSpec("tank");
    check(sedanPtr && tankPtr, "the built-in sedan and tank specs are there");
    if (!sedanPtr || !tankPtr) return 1;
    const VehicleSpec sedan = *sedanPtr, tank = *tankPtr;

    AVER_INFO("the sections");
    {
        check(ve::kSectionCount == 12, "twelve sections");
        bool unique = true;
        for (usize i = 0; i < ve::kSectionCount; ++i)
            for (usize j = i + 1; j < ve::kSectionCount; ++j)
                unique = unique && std::string(ve::kSections[i].key) != ve::kSections[j].key;
        check(unique, "with distinct keys");
    }

    AVER_INFO("curves");
    {
        const std::vector<Vec2> def = ve::defaultTorqueCurve();
        check(def.size() == 3 && def[1].x == 0.66f && def[1].y == 1.0f, "the default torque curve is Jolt's: (0, .8) (.66, 1) (1, .8)");
        check(near(ve::curveValue(def, 0.33f), 0.9) && near(ve::curveValue(def, -1.0f), 0.8) && near(ve::curveValue(def, 2.0f), 0.8), "linear between points, held at the ends");
        check(ve::curveValue({}, 0.5f) == 0.0f, "an empty curve is zero");

        std::vector<Vec2> pts = def;
        check(ve::setCurvePoint(pts, 1, 1.5f, 2.0f, 0.0f, 1.0f, 1.25f) && near(pts[1].x, 0.99) && near(pts[1].y, 1.25), "a point cannot pass its neighbour (stays 0.01 short) or exceed the top");
        pts = def;
        check(ve::setCurvePoint(pts, 1, -1.0f, -1.0f, 0.0f, 1.0f, 1.25f) && near(pts[1].x, 0.01) && pts[1].y == 0.0f, "...nor pass the other one, nor go below zero");
        pts = def;
        check(!ve::setCurvePoint(pts, 0, -0.5f, 0.8f, 0.0f, 1.0f, 1.25f) && pts[0].x == 0.0f, "the first point clamps to the left edge; a move that changes nothing reports false");
        pts = def;
        check(ve::setCurvePoint(pts, 2, 5.0f, 0.5f, 0.0f, 1.0f, 1.25f) && pts[2].x == 1.0f && pts[2].y == 0.5f, "the last point clamps to the right edge");
        check(!ve::setCurvePoint(pts, 9, 0.5f, 0.5f, 0.0f, 1.0f, 1.25f), "an index past the end is refused");

        pts = def;
        const int at = ve::addCurvePoint(pts, 0.33f, 0.0f, 1.0f, 32);
        check(at == 1 && pts.size() == 4 && near(pts[1].y, 0.9), "a point added at x takes the curve's value there");
        pts = def;
        check(ve::addCurvePoint(pts, 0.005f, 0.0f, 1.0f, 32) == -1 && ve::addCurvePoint(pts, 0.655f, 0.0f, 1.0f, 32) == -1, "...but not within 0.01 of another");
        check(ve::addCurvePoint(pts, 0.33f, 0.0f, 1.0f, 3) == -1, "...nor past the point cap");
        check(ve::addCurvePoint(pts, 1.5f, 0.0f, 1.0f, 32) == -1 && ve::addCurvePoint(pts, -0.5f, 0.0f, 1.0f, 32) == -1, "...nor outside the range");
        check(pts.size() == 3, "a refused add changes nothing");

        pts = def;
        check(ve::removeCurvePoint(pts, 1) && pts.size() == 2 && !ve::removeCurvePoint(pts, 0) && pts.size() == 2, "a curve keeps at least two points");
        check(!ve::removeCurvePoint(pts, 7), "an index past the end is refused");
    }

    AVER_INFO("torque and power");
    {
        VehicleSpec s;   // the defaults: 500 N*m, 1000..6000 rpm, no custom curve
        check(near(ve::torqueNm(s, 3960.0f), 500.0, 0.01), "the default curve peaks at 66% of the rev limit");
        check(near(ve::powerKw(s, 3960.0f), 500.0 * 3960.0 * (2.0 * 3.14159265358979 / 60.0) / 1000.0, 0.05), "power is torque times angular speed");
        const ve::EnginePeaks p = ve::enginePeaks(s);
        check(p.torqueNm > 499.0f && p.torqueRpm > 3800.0f && p.torqueRpm < 4100.0f, "peak torque of 500 N*m near 3960 rpm");
        check(near(p.powerRpm, 6000.0, 1.0) && near(p.powerKw, 0.8 * 500.0 * 6000.0 * (2.0 * 3.14159265358979 / 60.0) / 1000.0, 0.5), "peak power is at the rev limit with this curve (251 kW)");
        const std::vector<Vec2> curve = ve::powerCurve(s, 48);
        check(curve.size() == 48 && curve.front().x == 1000.0f && curve.back().x == 6000.0f, "the plotted power curve spans idle to the limit");
        check(ve::powerCurve(s, 0).size() == 2, "and has at least two samples");
        s.engine.curve = {Vec2(0.0f, 1.0f), Vec2(1.0f, 0.0f)};
        check(near(ve::torqueNm(s, 3000.0f), 250.0), "a custom curve replaces the default: half the revs, half the torque on a falling line");
        check(ve::peaksText(ve::EnginePeaks{500.0f, 3960.0f, 251.0f, 6000.0f}) == "peak torque 500 N*m at 3960 rpm   peak power 251 kW (337 hp) at 6000 rpm",
              "the peaks line: " + ve::peaksText(ve::EnginePeaks{500.0f, 3960.0f, 251.0f, 6000.0f}));
    }

    AVER_INFO("axles");
    {
        VehicleSpec s = makeSpec({180.0f, -180.0f});
        auto axles = ve::axleLayout(s);
        check(axles.size() == 2 && axles[0].left == 0 && axles[0].right == 1 && axles[1].left == 2 && axles[1].right == 3, "front axle first");
        s = makeSpec({-180.0f, 180.0f});   // listed rear first
        axles = ve::axleLayout(s);
        check(axles.size() == 2 && axles[0].left == 2 && axles[0].right == 3 && axles[1].left == 0 && axles[1].right == 1, "...whatever order the wheels were written in");
        s = makeSpec({180.0f});
        s.wheels[1].hub.x = 183.0f;
        check(ve::axleLayout(s).size() == 1, "wheels within 5 cm of the same x are one axle");
        s.wheels[1].hub.x = 190.0f;
        check(ve::axleLayout(s).size() == 2, "...farther apart they are not");
        s = makeSpec({0.0f});
        s.wheels.pop_back();
        axles = ve::axleLayout(s);
        check(axles.size() == 1 && axles[0].left == 0 && axles[0].right == -1, "a lone wheel is an axle with one side");

        check(ve::axleOfWheel(sedan, 0) == 0 && ve::axleOfWheel(sedan, 1) == 0 && ve::axleOfWheel(sedan, 2) == 1 && ve::axleOfWheel(sedan, 3) == 1 && ve::axleOfWheel(sedan, 9) == -1,
              "the sedan's front wheels are axle 0 and its rear wheels axle 1");
        check(ve::axleDef(VehicleSpec{}, 3).finalDrive == 3.42f && ve::axleDef(VehicleSpec{}, 3).diff == vehicle::DiffMode::LimitedSlip, "an axle with no record has the engine's defaults");

        s = makeSpec({180.0f, -180.0f});
        ve::ensureAxles(s);
        check(s.axles.size() == 2, "ensureAxles pads to the layout");
        s.axles.resize(4);
        ve::ensureAxles(s);
        check(s.axles.size() == 2, "...and drops records past the last axle");
    }

    AVER_INFO("the drive layout label");
    {
        VehicleSpec s = makeSpec({180.0f, -180.0f});
        drive(s, {});
        check(ve::driveLayout(s) == "Undriven", "no driven wheel");
        drive(s, {0, 1});
        check(ve::driveLayout(s) == "FWD", "front axle driven: FWD");
        drive(s, {2, 3});
        check(ve::driveLayout(s) == "RWD", "rear axle driven: RWD");
        drive(s, {0, 1, 2, 3});
        check(ve::driveLayout(s) == "AWD", "both driven: AWD");
        s.centerPresent = true;
        s.center = vehicle::DiffMode::LimitedSlip;
        check(ve::driveLayout(s) == "AWD", "...with a limited-slip centre");
        s.center = vehicle::DiffMode::Locked;
        check(ve::driveLayout(s) == "4x4", "...with a locked centre it is a 4x4");
        drive(s, {3});
        check(ve::driveLayout(s) == "Partial (1 of 4 wheels)", "one wheel of an axle: partial");

        s = makeSpec({200.0f, 0.0f, -200.0f});
        drive(s, {2, 3, 4, 5});
        check(ve::driveLayout(s) == "6x4", "three axles, the rear two driven: 6x4");
        drive(s, {0, 1, 2, 3, 4, 5});
        check(ve::driveLayout(s) == "6x6", "...all of them: 6x6");

        check(ve::driveLayout(sedan) == "RWD", "the built-in sedan is RWD");
        check(ve::driveLayout(tank) == "Tracked", "the built-in tank is tracked");
        const std::vector<bool> driven = ve::drivenAxles(sedan);
        check(driven.size() == 2 && !driven[0] && driven[1], "drivenAxles: the sedan's rear");
    }

    AVER_INFO("gears and top speed");
    {
        const ve::Driveline d = ve::drivelineOf(sedan);
        check(near(d.finalDrive, 3.42) && near(d.radiusCm, 33.0), "the sedan's driveline: final drive 3.42, 33 cm wheels");
        const std::vector<ve::GearRow> rows = ve::gearRows(sedan);
        check(rows.size() == 6 && rows[0].label == "R" && rows[1].label == "1" && rows[5].label == "5", "reverse, then five gears");
        const double fourth = 6000.0 * (2.0 * 3.14159265358979 / 60.0) / 3.42 * 33.0 * 0.036;   // ratio 1.0 at the rev limit, km/h
        check(near(rows[4].ratio, 1.0) && near(rows[4].topKmh, fourth, 0.3), "fourth (1.0) tops out at about 218 km/h: " + std::to_string(rows[4].topKmh));
        check(near(rows[1].topKmh, fourth / 2.66, 0.3), "first gear is the ratio shorter");
        check(near(rows[0].ratio, 2.9) && near(rows[0].topKmh, fourth / 2.9, 0.3), "reverse too");
        check(ve::gearRowText(rows[4]) == "4   ratio 1.00   218 km/h at the rev limit", "the row text: " + ve::gearRowText(rows[4]));

        const ve::Driveline t = ve::drivelineOf(tank);
        check(near(t.finalDrive, 6.0) && near(t.radiusCm, 40.0), "the tank's: the left track's final drive 6 and its 40 cm wheel");
        const std::vector<ve::GearRow> tr = ve::gearRows(tank);
        check(tr.size() == 6 && tr[0].label == "R1" && tr[1].label == "R2" && tr[2].label == "1", "the tank has two reverse ratios and four gears");
        check(near(tr[5].topKmh, 3500.0 * (2.0 * 3.14159265358979 / 60.0) / 6.0 * 40.0 * 0.036, 0.3), "its top gear: 88 km/h");

        VehicleSpec bare = makeSpec({180.0f, -180.0f});
        drive(bare, {});
        check(ve::gearRows(bare).size() == 6 && ve::gearRows(bare)[1].topKmh == 0.0f, "nothing driven: ratios shown, no speeds");
        check(ve::gearRowText(ve::gearRows(bare)[1]) == "1   ratio 2.66", "...and the row text drops the speed");
        check(ve::topSpeedKmh(0.0f, 3.0f, 33.0f, 6000.0f) == 0.0f && ve::topSpeedKmh(1.0f, 0.0f, 33.0f, 6000.0f) == 0.0f, "a ratio or final drive of zero gives no speed");
        check(ve::forwardRatios(bare).size() == 5 && ve::reverseRatios(bare).size() == 1, "an empty gearbox shows the engine's five ratios and one reverse");
    }

    AVER_INFO("wheels");
    {
        VehicleSpec s = makeSpec({180.0f, -180.0f});
        const int first = ve::addWheelPair(s, -400.0f);
        check(first == 4 && s.wheels.size() == 6, "an axle adds two wheels and says where they start");
        check(s.wheels[4].name == "W4" && s.wheels[5].name == "W5" && near(s.wheels[4].hub.y, -77.0) && near(s.wheels[5].hub.y, 77.0) &&
                  near(s.wheels[4].hub.x, -400.0) && near(s.wheels[4].hub.z, 33.0),
              "mirrored at the widest track, hub one radius up");
        check(!s.wheels[4].driven && s.wheels[4].steerDeg == 0.0f && s.wheels[4].node.empty(), "undriven, unsteered, no visual node yet");
        check(ve::axleLayout(s).size() == 3, "and it is a third axle");

        VehicleSpec big = makeSpec({0.0f, 40.0f, 80.0f, 120.0f, 160.0f, 200.0f, 240.0f, 280.0f});
        check(big.wheels.size() == 16 && ve::addWheelPair(big, 320.0f) == -1 && big.wheels.size() == 16, "sixteen wheels is the most");

        VehicleSpec t = tank;
        check(ve::removeWheel(t, 2) && t.wheels.size() == 9, "a wheel can be removed");
        check(t.tracks[0].wheels == std::vector<u32>({0, 1, 2, 3}) && t.tracks[0].driven == 3, "the left track forgets it and renumbers (the driven wheel 4 became 3)");
        check(t.tracks[1].wheels == std::vector<u32>({4, 5, 6, 7, 8}) && t.tracks[1].driven == 8, "the right track's indices shift down");
        t = tank;
        check(ve::removeWheel(t, 4) && t.tracks[0].wheels == std::vector<u32>({0, 1, 2, 3}) && t.tracks[0].driven == 0, "removing a track's driven wheel makes its first wheel the driver");
        std::string why;
        check(vehicle::validate(t, why), "...and the result is still a valid vehicle: " + why);
        VehicleSpec one = makeSpec({0.0f});
        one.wheels.pop_back();
        check(!ve::removeWheel(one, 0) && !ve::removeWheel(t, 99), "the last wheel and an index past the end stay");

        VehicleSpec fresh = makeSpec({100.0f, -100.0f});
        ve::autoTracks(fresh);
        check(fresh.tracks[0].present && fresh.tracks[1].present && fresh.tracks[0].wheels == std::vector<u32>({0, 2}) && fresh.tracks[1].wheels == std::vector<u32>({1, 3}) &&
                  fresh.tracks[0].driven == 0 && fresh.tracks[1].driven == 1,
              "autoTracks puts the left wheels on the left track and the right ones on the right");
    }

    AVER_INFO("validation");
    {
        check(ve::validationList(sedan).empty(), "the built-in sedan is clean");
        check(ve::validationList(tank).empty(), "...and so is the tank");

        std::vector<ve::Issue> issues = ve::validationList(VehicleSpec{});
        check(!issues.empty() && issues[0].error && issues[0].text.find("no wheels") != std::string::npos && ve::hasErrors(issues), "an empty spec's first problem is validate()'s: " + (issues.empty() ? std::string("none") : issues[0].text));

        VehicleSpec s = sedan;
        s.gearbox.forward = {1.0f, 2.0f, 3.0f};
        issues = ve::validationList(s);
        check(issues.size() == 1 && !issues[0].error && issues[0].text.find("Gear ratios") != std::string::npos, "ratios that do not fall are a note, not an error");
        check(!ve::hasErrors(issues), "hasErrors ignores notes");

        s = sedan;
        s.wheels[0].hub.z = 20.0f;
        issues = ve::validationList(s);
        check(issues.size() == 1 && issues[0].text.find("below the origin plane") != std::string::npos, "a hub lower than its tyre radius is flagged");

        s = sedan;
        s.wheels[1].node = s.wheels[0].node;
        issues = ve::validationList(s);
        check(issues.size() == 1 && issues[0].text.find("both drive the node") != std::string::npos, "two wheels on one visual node are flagged");

        s = sedan;
        ve::removeWheel(s, 3);
        issues = ve::validationList(s);
        bool oneSided = false;
        for (const ve::Issue& i : issues) oneSided = oneSided || i.text.find("one side only") != std::string::npos;
        check(oneSided, "an axle with a wheel on one side only is flagged");

        s = sedan;
        s.axles.resize(3);
        issues = ve::validationList(s);
        check(issues.size() == 1 && issues[0].text.find("more AXLE records") != std::string::npos, "AXLE records past the last axle are flagged");

        s = sedan;
        s.body.massKg = 40.0f;
        issues = ve::validationList(s);
        check(issues.size() == 1 && issues[0].text.find("100 kg") != std::string::npos, "a mass under 100 kg is flagged as a likely unit slip");

        s = sedan;
        s.audio.idle.clear();
        s.audio.high.clear();
        issues = ve::validationList(s);
        check(issues.size() == 1 && issues[0].text.find("silent") != std::string::npos, "an AUDIO record with no loop is flagged");

        s = sedan;
        s.engine.curve = {Vec2(0.7f, 1.0f), Vec2(1.0f, 0.8f)};
        issues = ve::validationList(s);
        check(issues.size() == 1 && issues[0].text.find("half the rev range") != std::string::npos, "a torque curve that starts late is flagged");
    }

    if (g_failures) { AVER_ERROR("VehicleEditorModelTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("VehicleEditorModelTest: all checks passed");
    return 0;
}
