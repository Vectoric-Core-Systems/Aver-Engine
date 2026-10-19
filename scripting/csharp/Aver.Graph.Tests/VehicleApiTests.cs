// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the public vehicle surface that need no native library (docs/plans/wave10/VEHICLES.md section 2.10): CarSpec.ToOcVehicle
// is the .ocvehicle text of the frozen default car (the same four wheels PlanWheels computes, the physics layer's v1 numbers for
// everything else), the HUD's formatting, the enums that mirror native values, and the Entity members' behaviour without a host.
// Driving itself is decided in tests/vehicle.scene (the system) and tests/physics (Jolt).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Aver.Framework;

[AutoRun]
static class VehicleApiTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestDefaultCarTextHasTheV1Numbers();
        failures += TestWheelsMatchThePlan();
        failures += TestNoOptionalRecords();
        failures += TestTextFollowsTheSpecAndIgnoresTheCulture();
        failures += TestHudFormatting();
        failures += TestMirroredEnums();
        failures += TestSpecsAreStable();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);
    private static bool Near(float a, float b, float tol = 1e-3f) => MathF.Abs(a - b) <= tol;

    // One record's tokens: the first word of each line is the key, the rest are its words.
    private static Dictionary<string, List<string[]>> Records(string text)
    {
        var map = new Dictionary<string, List<string[]>>();
        foreach (string raw in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] t = raw.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (t.Length == 0) continue;
            if (!map.TryGetValue(t[0], out var list)) map[t[0]] = list = new List<string[]>();
            list.Add(t);
        }
        return map;
    }

    private static float After(string[] tokens, string key, int offset = 1) =>
        float.Parse(tokens[Array.IndexOf(tokens, key) + offset], CultureInfo.InvariantCulture);

    private static int TestDefaultCarTextHasTheV1Numbers()
    {
        Console.WriteLine("Test: CarSpec.Default.ToOcVehicle() carries the frozen default car's numbers");
        string text = CarSpec.Default.ToOcVehicle();
        if (!text.StartsWith("OCVEHICLE 1\n")) return Fail("the text must begin OCVEHICLE 1");
        var r = Records(text);
        string[] body = r["BODY"][0];
        if (!Near(After(body, "half"), 230f) || !Near(After(body, "half", 2), 95f) || !Near(After(body, "half", 3), 45f)) return Fail("BODY half is 230 95 45");
        if (!Near(After(body, "clearance"), 30f) || !Near(After(body, "mass"), 1500f) || !Near(After(body, "pitchroll"), 60f)) return Fail("BODY clearance 30, mass 1500, pitchroll 60");
        if (!Near(After(body, "com", 3), -10f)) return Fail("the centre of mass is 10 cm below the origin");
        string[] eng = r["ENGINE"][0];
        if (!Near(After(eng, "torque"), 500f) || !Near(After(eng, "minrpm"), 1000f) || !Near(After(eng, "maxrpm"), 6000f)) return Fail("ENGINE 500 N*m, 1000..6000 rpm");
        string[] wc = r["WHEELCLASS"][0];
        if (!Near(After(wc, "radius"), 36f) || !Near(After(wc, "width"), 25f) || !Near(After(wc, "susp"), 30f) || !Near(After(wc, "susp", 2), 50f) ||
            !Near(After(wc, "hz"), 1.6f) || !Near(After(wc, "damping"), 0.5f) || !Near(After(wc, "brake"), 1500f) || !Near(After(wc, "handbrake"), 4000f))
            return Fail("WHEELCLASS carries the default tyre, suspension and brake numbers");
        if (!text.Contains("KIND wheeled")) return Fail("KIND wheeled");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestWheelsMatchThePlan()
    {
        Console.WriteLine("Test: the four WHEEL records are the plan PlanWheels computes (hub = settled centre, front steered, rear driven with the handbrake)");
        CarSpec s = CarSpec.Default;
        var plan = Vehicles.PlanWheels(s);
        var wheels = Records(s.ToOcVehicle())["WHEEL"];
        if (wheels.Count != 4) return Fail($"expected 4 WHEEL records, found {wheels.Count}");
        float sag = Vehicles.SpringSagCm(s.SuspensionHz);
        for (int i = 0; i < 4; i++)
        {
            string[] w = wheels[i];
            float hx = After(w, "hub"), hy = After(w, "hub", 2), hz = After(w, "hub", 3);
            Aver.Scene.Vec3 a = plan[i].AttachmentCm;
            if (!Near(hx, a.X) || !Near(hy, a.Y)) return Fail($"wheel {i}: hub x,y {hx},{hy} differs from the plan {a.X},{a.Y}");
            // The builder lowers the attachment by the sag from the settled hub: attach z = hub z + longest suspension - sag.
            if (!Near(hz + s.SuspensionMaxCm - sag, a.Z, 1e-2f)) return Fail($"wheel {i}: hub z {hz} does not give the plan's attachment height {a.Z}");
            if (!Near(After(w, "steer"), plan[i].MaxSteerDeg)) return Fail($"wheel {i}: steer");
            bool driven = After(w, "driven") != 0f, handbrake = After(w, "handbrake") != 0f;
            if (driven != plan[i].Driven || handbrake != (plan[i].HandBrakeTorque > 0f)) return Fail($"wheel {i}: driven / handbrake flags");
        }
        if (wheels[0][1] != "FL" || wheels[1][1] != "FR" || wheels[2][1] != "RL" || wheels[3][1] != "RR") return Fail("wheel order is front left, front right, rear left, rear right");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestNoOptionalRecords()
    {
        Console.WriteLine("Test: the default car sets nothing the physics layer would not have done (no gearbox, axles, aids, aero) so it drives as before");
        var r = Records(CarSpec.Default.ToOcVehicle());
        foreach (string key in new[] { "CURVE", "GEARBOX", "CENTER", "AXLE", "TYRE", "ASSIST", "AERO", "STEERCURVE", "BOX", "TRACK" })
            if (r.ContainsKey(key)) return Fail($"the default car must not write a {key} record");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestTextFollowsTheSpecAndIgnoresTheCulture()
    {
        Console.WriteLine("Test: a changed spec changes the text, and the text does not depend on the current culture");
        string heavy = (CarSpec.Default with { MassKg = 900f, FrontSteerDeg = 22.5f }).ToOcVehicle();
        var r = Records(heavy);
        if (!Near(After(r["BODY"][0], "mass"), 900f)) return Fail("mass 900 expected");
        if (!Near(After(r["WHEEL"][0], "steer"), 22.5f)) return Fail("steer 22.5 expected");
        CultureInfo old = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = new CultureInfo("de-DE");
            string german = (CarSpec.Default with { SuspensionHz = 1.75f }).ToOcVehicle();
            if (german.Contains("1,75")) return Fail("a comma decimal leaked into the text");
            if (!german.Contains("hz 1.75")) return Fail("the invariant decimal point is expected");
        }
        finally { CultureInfo.CurrentCulture = old; }
        if (CarSpec.Default.ToOcVehicle() != CarSpec.Default.ToOcVehicle()) return Fail("the same spec must give the same text");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestHudFormatting()
    {
        Console.WriteLine("Test: VehicleHud.FormatSpeed and FormatGear golden values");
        string[,] cases =
        {
            { "2778", "100 km/h" }, { "0", "0 km/h" }, { "-2778", "100 km/h" }, { "1000", "36 km/h" }, { "500", "18 km/h" },
        };
        for (int i = 0; i < cases.GetLength(0); i++)
        {
            string got = VehicleHud.FormatSpeed(float.Parse(cases[i, 0], CultureInfo.InvariantCulture));
            if (got != cases[i, 1]) return Fail($"FormatSpeed({cases[i, 0]}) = '{got}', expected '{cases[i, 1]}'");
        }
        if (VehicleHud.FormatSpeed(2778f, miles: true) != "62 mph") return Fail($"FormatSpeed(2778, miles) = '{VehicleHud.FormatSpeed(2778f, true)}', expected '62 mph'");
        if (VehicleHud.FormatGear(-1) != "R" || VehicleHud.FormatGear(0) != "N" || VehicleHud.FormatGear(4) != "4") return Fail("gears read R, N, then the number");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestMirroredEnums()
    {
        Console.WriteLine("Test: the enums that mirror native values keep their numbers");
        if ((int)VehiclePreset.Car != 0 || (int)VehiclePreset.Van != 1 || (int)VehiclePreset.Truck != 2 || (int)VehiclePreset.Bus != 3 ||
            (int)VehiclePreset.Sports != 4 || (int)VehiclePreset.Tank != 5)
            return Fail("VehiclePreset is pinned to CVehicle::preset (car van truck bus sports tank)");
        if ((int)VehicleEventKind.SkidBegin != 1 || (int)VehicleEventKind.Assist != 7 || (int)VehicleEventKind.Entered != 8 || (int)VehicleEventKind.Destroyed != 11)
            return Fail("VehicleEventKind: 1..7 are the physics kinds, 8 and up the system's");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestSpecsAreStable()
    {
        Console.WriteLine("Test: the pedal and tune members refuse an empty key without touching the host");
        Entity none = default;
        if (none.TuneVehicle("", 1f) || none.TuneVehicle(null!, 1f)) return Fail("an empty key must be refused");
        Console.WriteLine("  PASS");
        return 0;
    }
}
