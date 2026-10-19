// The Vehicle Details section without a window: the preset rows, the canonical COMPONENT Vehicle record (keys at their default are
// omitted) and its round trip with the values the section shows, what picking a preset or an asset writes, the live-tune table, the
// Create .ocvehicle path, the attach rules of the Add Component entry and the formatting of the Play-time block. Header-only model
// (sandbox/src/VehicleDetailsModel.hpp) plus the Add Component table; the widgets themselves are ImGui and covered by the owner's headed run.
#include "PlacedComponentModel.hpp"
#include "VehicleDetailsModel.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;
namespace vm = aver::editor::vehmodel;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string show(const fmt::OcPlacedComponent& r) {
    std::string s = r.type;
    for (const auto& kv : r.kv) s += " " + kv.first + "=" + kv.second;
    return s;
}

static fmt::OcPlacedComponent record(std::initializer_list<std::pair<const char*, const char*>> kv) {
    fmt::OcPlacedComponent r;
    r.type = "Vehicle";
    for (const auto& p : kv) r.kv.emplace_back(p.first, p.second);
    return r;
}

static bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

int main() {
    AVER_INFO("VehicleDetailsModelTest");

    AVER_INFO("the preset rows");
    {
        check(vm::kPresetRows.size() == 6, "six presets: car, van, truck, bus, sports, tank");
        bool ordered = true;
        for (usize i = 0; i < vm::kPresetRows.size(); ++i) ordered = ordered && vm::kPresetRows[i].preset == static_cast<u32>(i);
        check(ordered, "the rows are in CVehicle::preset order, so a combo index is the value");
        u32 p = 99;
        check(vm::presetFromKey("Sports", p) && p == 4 && vm::presetFromKey("TANK", p) && p == 5, "keys read case-insensitively");
        check(!vm::presetFromKey("hovercraft", p), "an unknown key is reported");
        check(std::string(vm::presetRow(2).key) == "truck" && std::string(vm::presetRow(2).label) == "Truck", "keys and labels");
        check(std::string(vm::presetRow(77).key) == "car", "an out-of-range preset reads as the car");
    }

    AVER_INFO("Add Component: the kind, its creation default and the attach rules");
    {
        const PlacedKind* k = findPlacedKind("Vehicle");
        check(k && std::string(k->componentName) == "CVehicle" && k->builtinId == 0, "Vehicle is a runtime-registered kind");
        if (k) {
            const auto exts = placedAssetExts(*k);
            check(exts.size() == 1 && std::string(exts[0]) == ".ocvehicle", "it takes an .ocvehicle asset");
            check(show(makeNewRecord(*k)) == "Vehicle preset=car", "a new record writes preset=car: " + show(makeNewRecord(*k)));
            check(show(makeNewRecord(*k)) == show(vm::newRecord()), "the menu's record is the model's canonical new record");

            AttachContext ctx;
            ctx.levelOwned = true;
            std::string why;
            check(!canAttach(ctx, *k, &why) && why.find("Needs a mesh") != std::string::npos, "a vehicle needs a placed mesh");
            ctx.hasMesh = true;
            check(canAttach(ctx, *k), "a placed mesh may carry one");
            ctx.alreadyPresent = true;
            check(!canAttach(ctx, *k, &why) && why.find("already") != std::string::npos, "but only one");
            ctx.alreadyPresent = false;
            ctx.legacyOcmap = true;
            check(!canAttach(ctx, *k), "and not in a legacy .ocmap");
            ctx.legacyOcmap = false;

            ctx.hasVehicleToken = true;
            check(!canAttach(ctx, *k, &why) && why.find("traffic car") != std::string::npos, "a traffic car (vehicle token) is refused with the reason: " + why);
            ctx.hasVehicleToken = false;
            ctx.hasRigidBody = true;
            check(!canAttach(ctx, *k, &why) && why.find("Rigid Body") != std::string::npos, "a rigid body is refused: " + why);
            ctx.hasRigidBody = false;
            ctx.hasObjectAnim = true;
            check(!canAttach(ctx, *k, &why) && why.find("object animation") != std::string::npos, "an object animation is refused: " + why);
            ctx.hasObjectAnim = false;
            ctx.hasDestructible = true;
            check(!canAttach(ctx, *k, &why) && why.find("Destructible") != std::string::npos, "a Destructible on the same entity is refused: " + why);
            ctx.hasDestructible = false;
            check(canAttach(ctx, *k), "and with the conflicts gone it is allowed again");

            // The symmetric rule: a Destructible may not be added to a vehicle.
            if (const PlacedKind* d = findPlacedKind("Destructible")) {
                AttachContext vctx;
                vctx.levelOwned = true;
                vctx.hasMesh = true;
                vctx.hasVehicle = true;
                check(!canAttach(vctx, *d, &why) && why.find("Vehicle") != std::string::npos, "a Destructible is refused on a vehicle: " + why);
            }
        }
        check(vm::attachRefusal(vm::ConflictFacts{}) == nullptr, "no conflicts, no refusal");
        vm::ConflictFacts both;
        both.hasVehicleToken = true;
        both.hasRigidBody = true;
        check(std::string(vm::attachRefusal(both)).find("traffic car") != std::string::npos, "when two conflict the traffic-car reason comes first");
    }

    AVER_INFO("the canonical record");
    {
        vm::VehicleValues v;
        check(show(vm::recordFromValues(v)) == "Vehicle preset=car", "the defaults are just the preset");
        v.asset = "Vehicles/Sedan.ocvehicle";
        check(show(vm::recordFromValues(v)) == "Vehicle asset=Vehicles/Sedan.ocvehicle", "with an asset the preset is not written");
        v.preset = 5;
        check(show(vm::recordFromValues(v)) == "Vehicle asset=Vehicles/Sedan.ocvehicle preset=tank", "...unless it is not the car");
        v = vm::VehicleValues{};
        v.assetHex = "00ff00ff00ff00ff";
        check(show(vm::recordFromValues(v)) == "Vehicle asset#=00ff00ff00ff00ff", "an id with no path is written as asset#");

        v = vm::VehicleValues{};
        v.preset = 2;
        v.power = 1.5f; v.grip = 0.8f; v.brake = 2.0f; v.damage = 0.5f;
        v.noActions = v.noAudio = v.noEffects = v.noWheels = v.indestructible = true;
        const std::string full = "Vehicle preset=truck power=1.5 grip=0.8 brake=2 damage=0.5 noactions=1 noaudio=1 noeffects=1 nowheels=1 indestructible=1";
        check(show(vm::recordFromValues(v)) == full, "every key, in canonical order: " + show(vm::recordFromValues(v)));

        vm::VehicleValues back;
        check(vm::valuesFromRecord(vm::recordFromValues(v), back) && back == v, "valuesFromRecord inverts recordFromValues");

        vm::VehicleValues odd;
        odd.power = 0.0f;   // the system reads a zero scale as 1
        odd.grip = -3.0f;
        odd.brake = 1.0f;
        check(show(vm::recordFromValues(odd)) == "Vehicle preset=car", "a scale of 1 (or one the system reads as 1) is not written");

        std::string why;
        vm::VehicleValues dummy;
        check(!vm::valuesFromRecord(record({{"preset", "sled"}}), dummy, &why) && why.find("sled") != std::string::npos, "an unknown preset is reported: " + why);
        check(vm::valuesFromRecord(record({}), dummy) && dummy.preset == 0 && dummy.power == 1.0f && !dummy.noActions, "an empty record is the defaults");
    }

    AVER_INFO("canonicalize, applyPreset, applyAsset");
    {
        const auto messy = record({{"noaudio", "0"}, {"power", "1.0"}, {"preset", "CAR"}, {"grip", "1"}, {"frob", "7"}});
        check(show(vm::canonicalize(messy)) == "Vehicle preset=car frob=7", "default-valued keys drop, unknown keys survive after the known ones: " + show(vm::canonicalize(messy)));
        check(vm::canonicalize(vm::canonicalize(messy)) == vm::canonicalize(messy), "canonicalize is idempotent");
        const auto bad = record({{"preset", "sled"}, {"power", "2"}});
        check(vm::canonicalize(bad) == bad, "a record with an unknown preset is kept as written");
        check(show(vm::canonicalize(record({{"power", "3"}, {"asset", "Vehicles/A.ocvehicle"}}))) == "Vehicle asset=Vehicles/A.ocvehicle power=3", "an asset record keeps its scale");

        const auto fromAsset = record({{"asset", "Vehicles/A.ocvehicle"}, {"power", "2"}});
        check(show(vm::applyPreset(fromAsset, 5)) == "Vehicle preset=tank power=2", "picking a preset drops the asset and keeps the scales: " + show(vm::applyPreset(fromAsset, 5)));
        check(show(vm::applyPreset(record({{"preset", "bus"}}), 99)) == "Vehicle preset=car", "an out-of-range preset becomes the car");

        check(show(vm::applyAsset(record({{"preset", "bus"}}), "Vehicles/B.ocvehicle")) == "Vehicle asset=Vehicles/B.ocvehicle preset=bus",
              "assigning an asset keeps a non-car preset: " + show(vm::applyAsset(record({{"preset", "bus"}}), "Vehicles/B.ocvehicle")));
        check(show(vm::applyAsset(record({{"asset", "Vehicles/A.ocvehicle"}, {"preset", "bus"}}), "")) == "Vehicle preset=bus", "clearing the asset falls back to the preset");
        vm::VehicleValues withAsset;
        withAsset.asset = "x.ocvehicle";
        check(vm::presetIsIgnored(withAsset) && !vm::presetIsIgnored(vm::VehicleValues{}), "the preset is ignored exactly while an asset is set");
    }

    AVER_INFO("the live-tune table");
    {
        const char* const want[] = {"power", "brake", "grip", "suspension", "steer", "drag", "downforce", "abs", "tcs", "manual", "collisionperiod"};
        bool same = vm::kTuneRows.size() == 11;
        for (usize i = 0; same && i < vm::kTuneRows.size(); ++i) same = std::string(vm::kTuneRows[i].key) == want[i];
        check(same, "the rows are the eleven keys VehiclePhysicsSystem::tune accepts (docs/VEHICLES.md 2.12.3), in that order");
        check(vm::findTuneRow("GRIP") && vm::findTuneRow("CollisionPeriod") && !vm::findTuneRow("turbo"), "keys are case-insensitive; unknown ones are not rows");
        const vm::TuneRow& period = *vm::findTuneRow("collisionperiod");
        check(vm::clampTune(period, 9.7f) == 8.0f && vm::clampTune(period, 0.2f) == 1.0f && vm::clampTune(period, 2.4f) == 2.0f, "the wheel-cast period is a whole number in 1..8");
        const vm::TuneRow& steer = *vm::findTuneRow("steer");
        check(vm::clampTune(steer, 5.0f) == 2.0f && vm::clampTune(steer, 0.0f) == 0.25f, "steering lock scales 0.25..2");
        const vm::TuneRow& abs = *vm::findTuneRow("abs");
        check(abs.toggle && vm::clampTune(abs, 3.0f) == 1.0f && vm::clampTune(abs, -1.0f) == 0.0f, "ABS is a 0 / 1 toggle");
        check(vm::clampScale(0.0f) == 1.0f && vm::clampScale(100.0f) == vm::kScaleMax && vm::clampScale(0.001f) == vm::kScaleMin, "a section scale of zero reads as 1; others clamp to the slider range");
    }

    AVER_INFO("Create .ocvehicle");
    {
        check(vm::createAssetPath("Meshes/Car.ocmesh", std::vector<std::string>{}) == "Meshes/Car.ocvehicle", "beside the mesh, named after it");
        check(vm::createAssetPath("Car.ocmesh", std::vector<std::string>{}) == "Car.ocvehicle", "a mesh at the Content root");
        check(vm::createAssetPath("Meshes\\Car.ocmesh", std::vector<std::string>{}) == "Meshes/Car.ocvehicle", "backslashes are read as slashes");
        const std::vector<std::string> taken = {"Meshes/Car.ocvehicle", "meshes/car_2.OCVEHICLE", "Other/Car.ocvehicle"};
        check(vm::createAssetPath("Meshes/Car.ocmesh", taken) == "Meshes/Car_3.ocvehicle", "never over an existing file (case-insensitive): " + vm::createAssetPath("Meshes/Car.ocmesh", taken));
        check(vm::createAssetPath("Meshes/", std::vector<std::string>{}).empty() && vm::createAssetPath("", std::vector<std::string>{}).empty(), "a mesh with no name has no path");
        check(vm::createAssetPath("A/B.ocmesh", std::function<bool(const std::string&)>([](const std::string& p) { return p == "A/B.ocvehicle"; })) == "A/B_2.ocvehicle",
              "the predicate form agrees");
        check(vm::stemOf("Meshes/Car.v2.ocmesh") == "Car.v2" && vm::dirOf("Meshes/Car.ocmesh") == "Meshes" && vm::dirOf("Car.ocmesh").empty(), "stem and directory");
    }

    AVER_INFO("the Play-time block");
    {
        check(vm::speedText(2778.0f) == "100 km/h" && vm::speedText(-2778.0f) == "100 km/h" && vm::speedText(0.0f) == "0 km/h", "speed is km/h: 2778 cm/s is 100");
        check(near(vm::speedKmh(1000.0f), 36.0) && near(vm::speedKmh(-1000.0f), 36.0), "...from the magnitude");
        check(vm::gearText(-1) == "R" && vm::gearText(0) == "N" && vm::gearText(4) == "4", "gears read R, N, then the number");
        check(near(vm::rpmFraction(3500.0f, 1000.0f, 6000.0f), 0.5) && vm::rpmFraction(100.0f, 1000.0f, 6000.0f) == 0.0f && vm::rpmFraction(9000.0f, 1000.0f, 6000.0f) == 1.0f &&
                  vm::rpmFraction(2000.0f, 5000.0f, 5000.0f) == 0.0f,
              "the rev bar clamps between idle and the limit, and a degenerate range is empty");

        vm::WheelReading w;
        w.contact = true; w.slipLong = 0.12f; w.slipLatDeg = 4.0f; w.loadN = 3200.0f; w.compressionCm = 6.5f; w.friction = 1.0f;
        auto cells = vm::wheelCells(2, w);
        check(cells[0] == "W2" && cells[1] == "yes" && cells[2] == "12%" && cells[3] == "4.0 deg" && cells[4] == "3.2 kN" && cells[5] == "6.5 cm" && cells[6] == "1.00" && cells[7].empty(),
              "a wheel row: " + cells[0] + " | " + cells[1] + " | " + cells[2] + " | " + cells[3] + " | " + cells[4] + " | " + cells[5] + " | " + cells[6]);
        w.skidding = true;
        check(vm::wheelCells(0, w)[7] == "SKID", "a skidding wheel says so");
        vm::WheelReading air;
        air.compressionCm = 0.0f;
        cells = vm::wheelCells(1, air);
        check(cells[1] == "air" && cells[2] == "-" && cells[4] == "-", "a wheel in the air shows dashes, not stale slip");
        check(vm::kWheelColumns.size() == cells.size(), "one cell per column");

        vm::LiveReading r;
        r.speedCmS = 2778.0f; r.gear = 3; r.rpm = 3120.0f; r.health01 = 0.75f; r.manual = true; r.abs = true; r.tcs = true;
        check(vm::lampText(r) == "ABS TCS", "lamps: " + vm::lampText(r));
        r.flipped = true; r.destroyed = true;
        check(vm::lampText(r) == "ABS TCS FLIPPED WRECKED", "...flipped and wrecked too");
        check(vm::outLine(r) == "100 km/h   gear 3 (manual)   3120 rpm   health 75%", "the summary line: " + vm::outLine(r));

        vm::LiveReading ok;
        auto b = vm::buttonsFor(true, false, ok);
        check(b.drive && !b.exit && b.flip && !b.repair && b.stop, "an intact parked vehicle can be driven, not repaired");
        vm::LiveReading hurt;
        hurt.health01 = 0.4f;
        b = vm::buttonsFor(true, true, hurt);
        check(!b.drive && b.exit && b.repair, "a hurt driven one can be exited and repaired");
        vm::LiveReading wreck;
        wreck.destroyed = true;
        b = vm::buttonsFor(true, false, wreck);
        check(!b.drive && b.repair, "a wreck cannot be driven but can be repaired");
        b = vm::buttonsFor(false, false, ok);
        check(!b.drive && !b.exit && !b.flip && !b.repair && !b.stop, "nothing applies to a vehicle that is not built");
    }

    if (g_failures) { AVER_ERROR("VehicleDetailsModelTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("VehicleDetailsModelTest: all checks passed");
    return 0;
}
