// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the vehicle nodes (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2; family version 2 is
// docs/plans/wave10/VEHICLES.md section 2.11): the 20 rows in GraphVehicleNodeTable.cs (the 6 original ones unchanged, 14 by entity), every
// method on VehicleGraph, and the CarSpec / wheel plan that Vehicles.CreateCar builds. Bare-process
// scope like MorphNodeTests: the wiring the table owns plus the pure description of the default car, not Jolt's driving (tests/physics
// does that). Where the physics library is absent CreateCar answers Car.None, which the last test checks.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class VehicleNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_vehiclecreatecar", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "yaw:float:in", "massKg:float:in", "then:exec:out", "vehicle:int:out", "entity:int:out" }, new[] { "massKg=1500" }),
        new("an_vehiclesetinput", GenericKind.Exec, new[] { "exec:exec:in", "vehicle:int:in", "throttle:float:in", "steer:float:in", "brake:float:in", "handbrake:float:in", "then:exec:out", "success:bool:out" }),
        new("an_vehiclegetstate", GenericKind.Pure, new[] { "vehicle:int:in", "found:bool:out", "speedCmS:float:out", "x:float:out", "y:float:out", "z:float:out", "yaw:float:out" }),
        new("an_vehiclesetpose", GenericKind.Exec, new[] { "exec:exec:in", "vehicle:int:in", "x:float:in", "y:float:in", "z:float:in", "yaw:float:in", "then:exec:out", "success:bool:out" }),
        new("an_vehicledestroy", GenericKind.Exec, new[] { "exec:exec:in", "vehicle:int:in", "then:exec:out", "success:bool:out" }),
        new("an_vehiclewheelcontact", GenericKind.Pure, new[] { "vehicle:int:in", "index:int:in", "contact:bool:out" }),
        // Version 2 (wave 10): by entity.
        new("an_vehiclespawn", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "yaw:float:in", "then:exec:out", "vehicle:int:out", "entity:int:out", "success:bool:out" }),
        new("an_vehicledrive", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "throttle:float:in", "steer:float:in", "brake:float:in", "handbrake:float:in", "then:exec:out", "success:bool:out" }),
        new("an_vehicleshift", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "delta:int:in", "then:exec:out", "success:bool:out" }),
        new("an_vehiclesetgear", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "gear:int:in", "then:exec:out", "success:bool:out" }),
        new("an_vehiclegearmode", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "manual:bool:in", "then:exec:out", "success:bool:out" }),
        new("an_vehicleenter", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "driver:int:in", "then:exec:out", "success:bool:out" }),
        new("an_vehicleexit", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "driver:int:out", "success:bool:out" }),
        new("an_vehicleflip", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "liftCm:float:in", "then:exec:out", "success:bool:out" }, new[] { "liftCm=50" }),
        new("an_vehicletune", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "value:float:in", "then:exec:out", "success:bool:out" }),
        new("an_vehiclerepair", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "amount:float:in", "then:exec:out", "success:bool:out" }, new[] { "amount=1" }),
        new("an_vehicletelemetry", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "speedCmS:float:out", "rpm:float:out", "gear:int:out", "throttle:float:out", "steer:float:out", "brake:float:out", "wheelsOnGround:int:out", "health:float:out", "abs:bool:out", "tcs:bool:out" }),
        new("an_vehiclewheelstate", GenericKind.Pure, new[] { "entity:int:in", "index:int:in", "found:bool:out", "contact:bool:out", "slipLong:float:out", "slipLatDeg:float:out", "loadN:float:out", "compressionCm:float:out", "angVelRadS:float:out", "surfaceFriction:float:out", "skidding:bool:out" }),
        new("an_vehicledriver", GenericKind.Pure, new[] { "entity:int:in", "driver:int:out" }),
        new("an_vehiclefromentity", GenericKind.Pure, new[] { "entity:int:in", "vehicle:int:out", "found:bool:out" }),
    };

    // The six rows of family version 1, which must keep their keys, pins and defaults for saved graphs.
    private static readonly string[] OriginalSix =
    {
        "an_vehiclecreatecar", "an_vehiclesetinput", "an_vehiclegetstate", "an_vehiclesetpose", "an_vehicledestroy", "an_vehiclewheelcontact",
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("vehicle", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("vehicle", Rows);
        failures += NodeRowKit.CheckIlValid("vehicle", types);
        failures += NodeRowKit.CheckCompilerSplit("vehicle", types);
        failures += TestVersionTwoLeavesTheSixAlone();
        failures += TestSpawnDriveChain();
        failures += TestEntityRowsDoNotThrowWithoutAProvider();
        failures += TestDriveChain();
        failures += TestReadsFeedLaterNodes();
        failures += TestDefaultCarSpecIsTheFrozenOne();
        failures += TestDefaultCarHasFourWheelsAndTheRightAxles();
        failures += TestWheelsRestOnTheGround();
        failures += TestAxleRule();
        failures += TestWithoutPhysicsThereIsNoCar();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);
    private static bool Near(float a, float b, float tol = 1e-3f) => MathF.Abs(a - b) <= tol;

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the vehicle rows are exactly the designed set of 20, 6 pure reads and 14 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "VehicleGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 20) return Fail($"expected 20 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        var wantPure = new[] { "an_vehicledriver", "an_vehiclefromentity", "an_vehiclegetstate", "an_vehicletelemetry", "an_vehiclewheelcontact", "an_vehiclewheelstate" };
        if (!pure.SequenceEqual(wantPure)) return Fail("only the six reads may be pure");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The original six rows are listed first, with the pins and defaults they shipped with, and the table lists them first too.
    private static int TestVersionTwoLeavesTheSixAlone()
    {
        Console.WriteLine("Test: the six original rows are still the first six, in their original order, with their original pins");
        var tableOrder = GameSystemNodes.All.Where(s => s.Owner.Name == "VehicleGraph").Select(s => s.Type).Take(6).ToArray();
        if (!tableOrder.SequenceEqual(OriginalSix)) return Fail($"the table starts [{string.Join(", ", tableOrder)}]");
        if (!Rows.Take(6).Select(r => r.Type).SequenceEqual(OriginalSix)) return Fail("the test's own first six rows drifted");
        var create = Rows[0];
        if (create.Pins.Length != 9 || create.Pins[5] != "massKg:float:in") return Fail("an_vehiclecreatecar keeps its nine pins");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Spawn by asset attribute, drive by entity, read telemetry by entity, in one exec chain that compiles.
    private static int TestSpawnDriveChain()
    {
        Console.WriteLine("Test: Spawn's entity feeds Drive, Shift, Tune and Telemetry in one exec chain, and compiles");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE s AN_VehicleSpawn asset=car\nNODE d AN_VehicleDrive\nNODE sh AN_VehicleShift\nNODE tu AN_VehicleTune key=power\nNODE tl AN_VehicleTelemetry\nNODE br Branch\nNODE pa Print\n" +
                            "LINK tick.exec s.exec\nLINK s.then d.exec\nLINK s.entity d.entity\nLINK d.then sh.exec\nLINK s.entity sh.entity\nLINK sh.then tu.exec\nLINK s.entity tu.entity\n" +
                            "LINK tu.then br.exec\nLINK s.entity tl.entity\nLINK tl.found br.cond\nLINK br.true pa.exec\nLINK tl.speedCmS pa.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // With no vehicle system in the process every entity row answers false/0, logged once, and nothing throws.
    private static int TestEntityRowsDoNotThrowWithoutAProvider()
    {
        Console.WriteLine("Test: the Entity vehicle members answer false rather than throw when the host has no vehicle system");
        Entity none = default;
        try
        {
            if (none.SetVehicleInput(1f, 0f)) return Fail("SetVehicleInput on no entity must be false");
            if (none.TryGetVehicleTelemetry(out _) || none.TryGetWheelTelemetry(0, out _)) return Fail("telemetry on no vehicle must be false");
            if (none.ShiftVehicle(1) || none.SetVehicleGear(1) || none.SetVehicleManualGearbox(true) || none.FlipVehicle() || none.RepairVehicle() ||
                none.DamageVehicle(0.5f) || none.EnterVehicle(none) || none.ExitVehicle() || none.TuneVehicle("power", 2f))
                return Fail("a vehicle call on no vehicle must be false");
            if (none.TuneVehicle("", 1f)) return Fail("an empty key is false");
            if (none.VehicleDriver.IsValid || none.VehicleWheelCount != 0) return Fail("no driver and no wheels");
            if (Vehicles.Spawn("car", new Aver.Scene.Vec3(0, 0, 0)).IsValid) { Console.WriteLine("  SKIP: a vehicle system is live in this process"); return 0; }
            if (Vehicles.DrainEvents(new VehicleEvent[4]) != 0) return Fail("no events without a provider");
            if (Vehicles.DrainEvents(Span<VehicleEvent>.Empty) != 0) return Fail("an empty buffer drains nothing");
        }
        catch (Exception e)
        {
            return Fail($"a vehicle call must answer false, not throw, without a provider: {e.GetType().Name}: {e.Message}");
        }
        Console.WriteLine("  PASS");
        return 0;
    }

    // Create, drive, move, destroy: the vehicle handle from Create feeds every later node.
    private static int TestDriveChain()
    {
        Console.WriteLine("Test: Create Car's vehicle pin feeds Set Input, Set Pose and Destroy in one exec chain, and compiles");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE c AN_VehicleCreateCar\nNODE i AN_VehicleSetInput\nNODE p AN_VehicleSetPose\nNODE d AN_VehicleDestroy\nNODE pe PrintInt\n" +
                            "LINK tick.exec c.exec\nLINK c.then i.exec\nLINK c.vehicle i.vehicle\nLINK i.then p.exec\nLINK c.vehicle p.vehicle\nLINK p.then d.exec\nLINK c.vehicle d.vehicle\n" +
                            "LINK d.then pe.exec\nLINK c.entity pe.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestReadsFeedLaterNodes()
    {
        Console.WriteLine("Test: Get State and Wheel Contact feed a Branch and prints without being in the exec chain");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE c AN_VehicleCreateCar\nNODE gs AN_VehicleGetState\nNODE wc AN_VehicleWheelContact\nNODE br Branch\nNODE pa Print\nNODE pb Print\n" +
                            "LINK tick.exec c.exec\nLINK c.then br.exec\nLINK c.vehicle gs.vehicle\nLINK c.vehicle wc.vehicle\nLINK wc.contact br.cond\nLINK br.true pa.exec\nLINK gs.speedCmS pa.value\n" +
                            "LINK pa.then pb.exec\nLINK gs.yaw pb.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The frozen constants of section 2.2; a change here is a change to every car in every project.
    private static int TestDefaultCarSpecIsTheFrozenOne()
    {
        Console.WriteLine("Test: CarSpec.Default carries the frozen constants");
        CarSpec s = CarSpec.Default;
        bool ok = Near(s.HalfExtentsCm.X, 230f) && Near(s.HalfExtentsCm.Y, 95f) && Near(s.HalfExtentsCm.Z, 45f) && Near(s.GroundClearanceCm, 30f) &&
                  Near(s.CentreOfMassCm.X, 0f) && Near(s.CentreOfMassCm.Y, 0f) && Near(s.CentreOfMassCm.Z, -10f) && Near(s.MassKg, 1500f) &&
                  Near(s.WheelbaseCm, 280f) && Near(s.TrackCm, 160f) && Near(s.WheelRadiusCm, 36f) && Near(s.WheelWidthCm, 25f) &&
                  Near(s.SuspensionMinCm, 30f) && Near(s.SuspensionMaxCm, 50f) && Near(s.SuspensionHz, 1.6f) && Near(s.SuspensionDamping, 0.5f) &&
                  Near(s.FrontSteerDeg, 30f) && Near(s.BrakeTorque, 1500f) && Near(s.HandBrakeTorque, 4000f) && Near(s.EngineTorque, 500f) &&
                  Near(s.EngineMinRpm, 1000f) && Near(s.EngineMaxRpm, 6000f) && Near(s.MaxPitchRollDeg, 60f);
        if (!ok) return Fail("a CarSpec.Default constant differs from the design");
        if (CarSpec.Default != s) return Fail("CarSpec.Default is not a stable value");
        CarSpec heavy = s with { MassKg = 900f };
        if (!Near(heavy.MassKg, 900f) || !Near(heavy.WheelbaseCm, 280f) || heavy == s) return Fail("a with expression must change only the named member");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Four wheels; the front pair steers and has no handbrake, the rear pair is driven and carries it.
    private static int TestDefaultCarHasFourWheelsAndTheRightAxles()
    {
        Console.WriteLine("Test: the default car plans 4 wheels in 2 axles, front steered, rear driven with the handbrake, one differential");
        var wheels = Vehicles.PlanWheels(CarSpec.Default);
        if (wheels.Length != 4) return Fail($"expected 4 wheels, planned {wheels.Length}");
        if (Vehicles.CountAxles(wheels) != 2) return Fail($"expected 2 axles, found {Vehicles.CountAxles(wheels)}");
        if (Vehicles.CountDifferentials(wheels) != 1) return Fail($"expected 1 differential (the driven rear axle), found {Vehicles.CountDifferentials(wheels)}");
        var front = wheels.Where(w => w.AttachmentCm.X > 0f).ToArray();
        var rear = wheels.Where(w => w.AttachmentCm.X < 0f).ToArray();
        if (front.Length != 2 || rear.Length != 2) return Fail("two wheels per axle expected");
        if (front.Any(w => !Near(w.MaxSteerDeg, 30f) || w.Driven || w.HandBrakeTorque != 0f)) return Fail("the front wheels steer 30 degrees, are not driven and have no handbrake");
        if (rear.Any(w => w.MaxSteerDeg != 0f || !w.Driven || !Near(w.HandBrakeTorque, 4000f))) return Fail("the rear wheels are fixed, driven and carry the 4000 N*m handbrake");
        if (wheels.Any(w => !Near(w.BrakeTorque, 1500f))) return Fail("every wheel brakes with 1500 N*m");
        if (!Near(front[0].AttachmentCm.X, 140f) || !Near(rear[0].AttachmentCm.X, -140f)) return Fail("wheels sit half a wheelbase either side of the origin");
        if (wheels.Count(w => w.AttachmentCm.Y < 0f) != 2 || wheels.Any(w => !Near(MathF.Abs(w.AttachmentCm.Y), 80f))) return Fail("wheels sit half a track either side of Y = 0");
        // Every wheel must satisfy the physics layer's own validity rules, or Vehicle.AddWheel returns -1.
        foreach (var w in wheels)
            if (w.RadiusCm <= 0f || w.WidthCm <= 0f || w.SuspensionMinCm < 0f || w.SuspensionMaxCm <= w.SuspensionMinCm || w.SuspensionHz <= 0f ||
                w.SuspensionDamping < 0f || w.BrakeTorque < 0f || w.HandBrakeTorque < 0f || w.MaxSteerDeg < 0f || w.MaxSteerDeg > 89f)
                return Fail("a planned wheel would be refused by AddWheel");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The ride-height rule of physics_vehicle_abi.h: lower the attachment by the spring sag so the wheel centre settles one radius up.
    private static int TestWheelsRestOnTheGround()
    {
        Console.WriteLine("Test: attachment height = tyre radius + longest suspension - spring sag, so the wheel centre rests one radius above the origin");
        CarSpec s = CarSpec.Default;
        float sag = Vehicles.SpringSagCm(s.SuspensionHz);
        if (!Near(sag, 981f / MathF.Pow(2f * MathF.PI * 1.6f, 2f), 1e-2f) || sag < 8f || sag > 12f) return Fail($"sag of a 1.6 Hz spring is {sag} cm, expected about 9.7");
        foreach (var w in Vehicles.PlanWheels(s))
        {
            float restingCentre = w.AttachmentCm.Z - (w.SuspensionMaxCm - sag);
            if (!Near(restingCentre, s.WheelRadiusCm, 1e-2f)) return Fail($"a wheel centre rests at {restingCentre}, expected the {s.WheelRadiusCm} cm tyre radius");
        }
        if (Vehicles.SpringSagCm(0f) != 0f || Vehicles.SpringSagCm(-1f) != 0f) return Fail("a non-positive frequency sags nothing");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Two wheels agreeing in X within 5 cm on opposite sides of Y = 0 are an axle; a wheel with no partner is its own.
    private static int TestAxleRule()
    {
        Console.WriteLine("Test: the axle rule pairs wheels within 5 cm in X across Y = 0 and leaves a lone wheel its own axle");
        WheelPlan W(float x, float y, bool driven) => new(new Aver.Scene.Vec3(x, y, 70f), 36f, 25f, 30f, 50f, 1.6f, 0.5f, 0f, 1000f, 0f, driven);
        var wheels = new[] { W(100f, -80f, false), W(104f, 80f, false), W(-100f, -80f, true) };
        int[] axle = Vehicles.AxleOf(wheels);
        if (axle[0] != axle[1] || axle[2] == axle[0]) return Fail("the first two wheels pair (4 cm apart, opposite sides); the third does not");
        if (Vehicles.CountAxles(wheels) != 2) return Fail("two axles expected");
        if (Vehicles.CountDifferentials(wheels) != 1) return Fail("only the axle with the driven wheel gets a differential");
        var far = new[] { W(100f, -80f, true), W(110f, 80f, true) };
        if (Vehicles.CountAxles(far) != 2) return Fail("wheels 10 cm apart in X are two axles");
        var sameSide = new[] { W(100f, 80f, true), W(100f, 60f, true) };
        if (Vehicles.CountAxles(sameSide) != 2) return Fail("two wheels on the same side never pair");
        if (Vehicles.CountAxles(Array.Empty<WheelPlan>()) != 0) return Fail("no wheels, no axles");
        Console.WriteLine("  PASS");
        return 0;
    }

    // No physics library, no car: the same Car.None a refused build returns, and nothing throws.
    private static int TestWithoutPhysicsThereIsNoCar()
    {
        Console.WriteLine("Test: CreateCar answers Car.None without the physics library; Car.None is invalid with handle 0");
        if (Car.None.IsValid || Car.None.Handle != 0) return Fail("Car.None must be invalid with handle 0");
        if (Vehicles.TryGet(123456, out _)) return Fail("an unknown handle must not resolve");
        try
        {
            Car car = Vehicles.CreateCar(new Aver.Scene.Vec3(0, 0, 0), 0f);
            if (car.IsValid) { Console.WriteLine("  SKIP: a physics world is live in this process"); car.Destroy(); return 0; }
            Console.WriteLine("  PASS");
        }
        catch (Exception e)
        {
            return Fail($"CreateCar must answer Car.None, not throw, when the physics library is missing: {e.GetType().Name}");
        }
        return 0;
    }
}
