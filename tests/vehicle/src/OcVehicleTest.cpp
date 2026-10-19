// OcVehicleTest -- .ocvehicle, the spec, validation, the presets and the parse cache. No scene, no Jolt.
//
//   * the built-in sedan and tank parse, validate and say what the template says they say,
//   * write -> parse -> write is the same text, for every spec we have,
//   * every refusal of validate() refuses, and the parser's tolerance (unknown records and keys, comments, a
//     newer version) is what the header promises,
//   * specFromBounds reproduces the numbers of the old kPresets table (copied here as golden values) for three
//     mesh sizes, and the tank derives as tracked with five road wheels a side,
//   * the parse cache parses once, caches a failure, and forgets on invalidate.
#include "aver/core/Log.hpp"
#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

using namespace aver;
using namespace aver::vehicle;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

// The smallest legal file: header, BODY, one class, four wheels.
static const char* const kMinimal =
    "OCVEHICLE 1\n"
    "BODY half 200 90 35 clearance 25 com 0 0 40 mass 1300\n"
    "WHEELCLASS street radius 33 width 22\n"
    "WHEEL a class street hub 180 -77 33 steer 30\n"
    "WHEEL b class street hub 180 77 33 steer 30\n"
    "WHEEL c class street hub -180 -77 33 driven 1\n"
    "WHEEL d class street hub -180 77 33 driven 1\n";

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void roundTrip(const VehicleSpec& s, const std::string& label) {
    const std::string t1 = writeOcVehicle(s);
    VehicleSpec back;
    std::string err;
    const bool parsed = parseOcVehicle(t1, back, err);
    check(parsed, label + ": the written text parses (" + err + ")");
    if (!parsed) return;
    const std::string t2 = writeOcVehicle(back);
    check(t1 == t2, label + ": write -> parse -> write is the same text");
    std::string why;
    check(validate(back, why), label + ": and it still validates (" + why + ")");
    check(back.wheels.size() == s.wheels.size() && back.kind == s.kind && back.axles.size() == s.axles.size(), label + ": the shape survives");
}

static void testBuiltins() {
    AVER_INFO("=== the built-in sedan and tank ===");
    const VehicleSpec* sedan = builtinSpec("sedan");
    const VehicleSpec* tank = builtinSpec("Tank");
    check(sedan != nullptr && tank != nullptr, "both exist (case-insensitive)");
    check(builtinSpec("car") == nullptr, "a traffic preset is not a fixed-geometry spec");
    if (!sedan || !tank) return;
    std::string why;
    check(validate(*sedan, why), "the sedan validates: " + why);
    check(validate(*tank, why), "the tank validates: " + why);
    check(sedan->kind == Kind::Wheeled && sedan->wheels.size() == 4 && sedan->axles.size() == 2, "the sedan is a four-wheeler with two axles");
    check(sedan->assist.present && sedan->assist.abs && !sedan->assist.tcs, "ABS on, traction control off");
    check(sedan->engine.curve.size() == 3 && sedan->gearbox.forward.size() == 5 && sedan->gearbox.reverse.size() == 1, "a three-point curve and a five-speed box");
    check(sedan->wheels[2].driven && sedan->wheels[3].driven && !sedan->wheels[0].driven, "rear-wheel drive");
    check(sedan->wheels[0].node == "WheelFL" && sedan->wheels[3].node == "WheelRR", "wheel nodes are named");
    check(!sedan->hud.empty() && sedan->camera.modes.size() == 3 && sedan->camera.modes[1] == "hood", "the HUD and the three camera modes (hood the mode, then hood the point)");
    check(sedan->camera.hood.x == 150.0f && sedan->camera.arm == 600.0f, "the camera numbers parse");
    check(tank->kind == Kind::Tracked && tank->wheels.size() == 10 && tank->tracks[0].present && tank->tracks[1].present, "the tank is tracked with two tracks of five");
    check(tank->tracks[0].driven == 4 && tank->tracks[1].driven == 9, "and the sprockets are the last wheel of each");
    roundTrip(*sedan, "sedan");
    roundTrip(*tank, "tank");
}

static void testFiles() {
    AVER_INFO("=== the template's and the lab's .ocvehicle files ===");
#ifdef AVER_REPO_ROOT
    const std::string root = std::string(AVER_REPO_ROOT) + "/";
    const char* const files[] = {
        "templates/Vehicle/Content/Vehicles/Sedan.ocvehicle", "templates/Vehicle/Content/Vehicles/Tank.ocvehicle",
        "test-content/VehicleLab/Content/Vehicles/Truck6x4.ocvehicle", "test-content/VehicleLab/Content/Vehicles/Hatchback.ocvehicle",
        "test-content/VehicleLab/Content/Vehicles/Tank.ocvehicle", "test-content/VehicleLab/Content/Vehicles/RiggedCar.ocvehicle",
    };
    for (const char* f : files) {
        std::string text;
        if (!readFile(root + f, text)) { check(false, std::string(f) + ": cannot read"); continue; }
        VehicleSpec s;
        std::string err;
        const bool ok = parseOcVehicle(text, s, err);
        check(ok, std::string(f) + ": parses (" + err + ")");
        if (!ok) continue;
        std::string why;
        check(validate(s, why), std::string(f) + ": validates (" + why + ")");
        roundTrip(s, f);
    }
    // The template's two vehicles ARE the engine's built-in sedan and tank: same canonical text once parsed (comments aside).
    for (const char* name : {"sedan", "tank"}) {
        std::string text;
        VehicleSpec s;
        std::string err;
        const std::string file = std::string("templates/Vehicle/Content/Vehicles/") + (std::string(name) == "sedan" ? "Sedan" : "Tank") + ".ocvehicle";
        const VehicleSpec* built = builtinSpec(name);
        if (readFile(root + file, text) && parseOcVehicle(text, s, err) && built)
            check(writeOcVehicle(s) == writeOcVehicle(*built), std::string(file) + " is the built-in '" + name + "'");
        else
            check(false, std::string(file) + ": cannot be compared with the built-in '" + name + "'");
    }
    {
        std::string text;
        VehicleSpec s;
        std::string err;
        if (readFile(root + "test-content/VehicleLab/Content/Vehicles/Truck6x4.ocvehicle", text) && parseOcVehicle(text, s, err))
            check(s.wheels.size() == 6 && s.axles.size() >= 3 && !s.boxes.empty() && s.centerPresent, "the 6x4 truck: six wheels, three axles, a centre differential, BOX records");
    }
#else
    AVER_WARN("=== SKIPPED: built without AVER_REPO_ROOT ===");
#endif
}

static void testParser() {
    AVER_INFO("=== the parser ===");
    VehicleSpec s;
    std::string err;
    check(parseOcVehicle(kMinimal, s, err), "the smallest legal file parses: " + err);
    std::string why;
    check(validate(s, why), "and validates: " + why);
    check(s.wheels.size() == 4 && s.classes.size() == 1 && s.wheels[0].steerDeg == 30.0f, "four wheels, one class, steer read");
    check(s.engine.torque == 500.0f && s.engine.minRpm == 1000.0f && !s.gearbox.present, "everything unsaid is the physics ABI's default");

    const std::string noisy = std::string("# a comment\n\nOCVEHICLE 1   # trailing\nFUTURE thing 1 2 3\n") + (kMinimal + 12) +
                              "ENGINE torque 400 sparkle 9\n";
    VehicleSpec n;
    check(parseOcVehicle(noisy, n, err), "an unknown record, an unknown key, comments and blank lines are tolerated: " + err);
    check(n.engine.torque == 400.0f && n.wheels.size() == 4, "and the known parts still read");

    VehicleSpec x;
    check(!parseOcVehicle("OCVEHICLE 2\nBODY half 1 1 1\n", x, err), "a newer version is refused");
    check(!parseOcVehicle("BODY half 1 1 1\n", x, err), "a file without the header is refused");
    check(!parseOcVehicle("OCVEHICLE 1\nBODY half 1 nope 1\n", x, err) && err.find("line 2") != std::string::npos, "a malformed number refuses the file and names the line: " + err);
    check(!parseOcVehicle("", x, err), "an empty file is refused");

    VehicleSpec c;
    check(parseOcVehicle("OCVEHICLE 1\nCENTER lsd 1.8\nCAMERA modes chase hood arm 500 hood 1 2 3\nAUDIO engine A.ocaudio idle 900 B.ocaudio high 5000 skid S.ocaudio\n", c, err), "centre, camera and audio records parse: " + err);
    check(c.centerPresent && c.center == DiffMode::LimitedSlip && c.centerLsd == 1.8f, "CENTER lsd 1.8");
    check(c.camera.modes.size() == 2 && c.camera.modes[1] == "hood" && c.camera.hood.z == 3.0f, "a mode named hood is a mode, hood followed by numbers is the point");
    check(c.audio.idle == "A.ocaudio" && c.audio.high == "B.ocaudio" && c.audio.idleRpm == 900.0f && c.audio.highRpm == 5000.0f && c.audio.skid == "S.ocaudio", "audio: idle loop, high loop, rpms, skid");
}

static VehicleSpec minimal() {
    VehicleSpec s;
    std::string err;
    parseOcVehicle(kMinimal, s, err);
    return s;
}

static void testValidate() {
    AVER_INFO("=== validate refuses ===");
    std::string why;
    {
        VehicleSpec s = minimal();
        s.wheels.clear();
        check(!validate(s, why) && why.find("no wheels") != std::string::npos, "no wheels: " + why);
    }
    {
        VehicleSpec s = minimal();
        s.wheels[1].wheelClass = "ghost";
        check(!validate(s, why) && why.find("ghost") != std::string::npos, "an undefined class: " + why);
    }
    {
        VehicleSpec s = minimal();
        s.classes[0].tyre = "slicks";
        check(!validate(s, why), "an undefined tyre: " + why);
    }
    {
        VehicleSpec s = minimal();
        s.engine.curve = {Vec2(0, 1), Vec2(0.6f, 1), Vec2(0.4f, 1)};
        check(!validate(s, why), "a descending torque curve: " + why);
    }
    {
        VehicleSpec s = minimal();
        for (WheelDef& w : s.wheels) w.driven = false;
        check(!validate(s, why), "a wheeled vehicle with nothing driven: " + why);
    }
    {
        VehicleSpec s = minimal();
        s.kind = Kind::Tracked;
        check(!validate(s, why), "a tracked vehicle without tracks: " + why);
        for (int side = 0; side < 2; ++side) {
            s.tracks[side].present = true;
            s.tracks[side].wheels = side == 0 ? std::vector<u32>{0, 2} : std::vector<u32>{1, 3};
            s.tracks[side].driven = s.tracks[side].wheels[0];
        }
        check(validate(s, why), "and with both tracks it validates: " + why);
        s.tracks[1].driven = 0;
        check(!validate(s, why), "a driven wheel outside its track: " + why);
    }
    {
        VehicleSpec s = minimal();
        while (s.wheels.size() <= kMaxWheels) s.wheels.push_back(s.wheels[0]);
        check(!validate(s, why) && why.find("16") != std::string::npos, "more than 16 wheels: " + why);
    }
    {
        VehicleSpec s = minimal();
        s.wheels[0].hub.x = std::numeric_limits<f32>::quiet_NaN();
        check(!validate(s, why), "a NaN hub: " + why);
        s = minimal();
        s.body.massKg = std::numeric_limits<f32>::infinity();
        check(!validate(s, why), "an infinite mass: " + why);
        s = minimal();
        s.classes[0].radius = -3.0f;
        check(!validate(s, why), "a negative radius: " + why);
    }
}

// The old kPresets table, copied verbatim as golden values: name, mass, radius range, engine, steer, hz, frontDriven.
struct Golden { const char* name; f32 mass, rMin, rMax, engine, steer, hz; bool front; };
static const Golden kGolden[] = {
    {"car",    1300.0f, 28.0f, 38.0f,  500.0f, 35.0f, 2.0f, false},
    {"van",    1900.0f, 33.0f, 40.0f,  600.0f, 35.0f, 2.0f, true},
    {"truck",  6000.0f, 45.0f, 55.0f, 1500.0f, 30.0f, 1.5f, false},
    {"bus",   11000.0f, 48.0f, 55.0f, 2000.0f, 30.0f, 1.5f, false},
    {"sports", 1200.0f, 30.0f, 35.0f,  700.0f, 35.0f, 2.0f, false},
};

static void testPresets() {
    AVER_INFO("=== specFromBounds reproduces the traffic presets ===");
    check(presetIndex("car") == 0 && presetIndex("TANK") == 5 && presetIndex("hovercraft") == -1, "preset names resolve (case-insensitive), unknown ones do not");
    check(std::string(presetName(0)) == "car" && std::string(presetName(5)) == "tank" && presetName(6) == nullptr, "and back");
    const Vec3 sizes[3] = {Vec3(440, 180, 140), Vec3(520, 200, 260), Vec3(1200, 260, 330)};
    for (const Golden& g : kGolden) {
        for (const Vec3& sz : sizes) {
            VehicleSpec s;
            const bool ok = specFromBounds(g.name, sz, s);
            const std::string tag = std::string(g.name) + " " + f2s(sz.x) + "x" + f2s(sz.z);
            check(ok, tag + ": derives");
            if (!ok) continue;
            std::string why;
            check(validate(s, why), tag + ": validates (" + why + ")");
            const f32 radius = std::clamp(0.17f * sz.z, g.rMin, g.rMax);
            check(s.body.massKg == g.mass && s.engine.torque == g.engine, tag + ": mass and engine torque");
            check(s.classes.size() == 1 && s.classes[0].radius == radius && s.classes[0].suspHz == g.hz, tag + ": the wheel radius clamp and the spring frequency");
            check(s.wheels.size() == 4 && s.wheels[0].steerDeg == g.steer && s.wheels[2].steerDeg == 0.0f, tag + ": the front pair steers by the preset's angle");
            check(s.wheels[0].driven == g.front && s.wheels[2].driven == !g.front, tag + ": front or rear drive");
            check(s.wheels[2].handbrake && !s.wheels[0].handbrake, tag + ": the handbrake is on the rear axle");
            check(s.body.half.x == 0.5f * sz.x && s.body.half.y == 0.5f * sz.y && s.body.com.z == 0.35f * sz.z, tag + ": the chassis box and centre of mass follow the mesh");
            check(s.wheels[0].hub.z == radius, tag + ": the hub is one radius up");
            check(!s.assist.present && !s.gearbox.present, tag + ": no aids and no gearbox of its own (the traffic behaviour as it was)");
        }
    }
    VehicleSpec t;
    check(specFromBounds("tank", Vec3(700, 340, 200), t), "tank derives");
    std::string why;
    check(t.kind == Kind::Tracked && t.wheels.size() == 10 && t.tracks[0].wheels.size() == 5 && t.tracks[1].wheels.size() == 5, "tracked, five wheels a side");
    check(validate(t, why), "and it validates: " + why);
    roundTrip(t, "derived tank");
    VehicleSpec bad;
    check(!specFromBounds("car", Vec3(0, 180, 140), bad) && !specFromBounds("nope", Vec3(1, 1, 1), bad), "a zero size and an unknown name are refused");
    VehicleSpec car;
    specFromBounds("car", Vec3(440, 180, 140), car);
    roundTrip(car, "derived car");
}

static void testCache() {
    AVER_INFO("=== the parse cache ===");
    SpecCache cache;
    int reads = 0;
    const SpecCache::ReadFn read = [&](const std::string& path, std::string& text) {
        ++reads;
        if (path == "good") { text = kMinimal; return true; }
        if (path == "broken") { text = "OCVEHICLE 1\nBODY half x\n"; return true; }
        return false;
    };
    const VehicleSpec* a = cache.get("good", read);
    const VehicleSpec* b = cache.get("good", read);
    check(a != nullptr && a == b && reads == 1, "a good file is read and parsed once");
    check(cache.get("broken", read) == nullptr && cache.get("broken", read) == nullptr && reads == 2, "a broken file is parsed once and the failure is cached");
    check(cache.get("missing", read) == nullptr && cache.get("missing", read) == nullptr && reads == 3, "so is a missing one");
    cache.invalidate("good");
    check(cache.get("good", read) != nullptr && reads == 4, "invalidate forgets one path");
    cache.clear();
    check(cache.get("good", read) != nullptr && reads == 5, "clear forgets everything");
}

int main() {
    AVER_INFO("=== OcVehicleTest ===");
    testBuiltins();
    testFiles();
    testParser();
    testValidate();
    testPresets();
    testCache();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
