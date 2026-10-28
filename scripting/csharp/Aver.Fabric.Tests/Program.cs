// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Test runner for Aver.Fabric (the Godot-compatible runtime). Returns the failure count.

static class Program
{
    static int Main(string[] args)
    {
        int failures = 0;
        if (args.Length == 1 && args[0] == "gdscript") // dotnet run -c Debug -- gdscript: only the F4a suite
        {
            failures = GdScriptParserTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "gdconvert-tests") // only the F4b suite
        {
            failures = GdScriptConvertTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "gdext") // only the F5 suite
        {
            failures = GdextTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length >= 2 && args[0] == "gdconvert") return GdScriptConvertTests.Debug(args[1..]); // development aid
        failures += MathTests.RunAll();      // Godot-space math and the Aver-space boundary
        failures += VariantTests.RunAll();   // Variant, Godot's text form, collections, StringName/NodePath, GD, reflection by name
        failures += SignalTests.RunAll();    // Callable, signals with every ConnectFlags value, Timer
        failures += NodeTests.RunAll();      // tree API, paths, groups, lifecycle order, processing, exported values
        failures += Node3DTests.RunAll();    // Node3D, the node classes, physics signal routing
        failures += EnvironmentTests.RunAll(); // G4 R3: WorldEnvironment, Environment, Sky, CameraAttributes and what they push to the engine
        failures += SubViewportTests.RunAll(); // G4 R5: SubViewport, ViewportTexture, SubViewportContainer and what they ask the engine for
        failures += EnvFxTests.RunAll();     // G4 R8: FogVolume, ReflectionProbe, LightmapGI, VoxelGI and what they publish to the engine
        failures += GeometryTests.RunAll();// G4 R7: ArrayMesh, SurfaceTool, ImmediateMesh, MultiMesh, GridMap, CSG, Sprite3D, Label3D and what they push to the engine
        failures += SceneValueTests.RunAll(); // ClassDB, Object(...) values, resource and node-reference properties
        failures += TweenTests.RunAll();     // F2b: easing curves, tween sequencing, loops, pause and process modes
        failures += ServiceTests.RunAll();   // F2b: SceneTree frame signals, timers, await, groups, scene changes, ResourceLoader, PackedScene
        failures += InputTests.RunAll();     // F2b: Input and InputMap, InputEvent classes, the engine-event bridge, _Input delivery
        failures += PhysicsWrapperTests.RunAll(); // F2b: Camera3D maths, physics node defaults and conversions, vehicle, queries without a world
        failures += ConvertScriptsTests.RunAll(); // F3: Aver.Fabric.Convert, GodotSharp scripts rewritten, compiled against this runtime and run
        failures += GdScriptParserTests.RunAll(); // F4a: the GDScript tokenizer and parser (typed AST, error recovery)
        failures += GdScriptConvertTests.RunAll(); // F4b: GDScript converted to C#, compiled against this runtime, run against GDScript's results
        failures += GdextTests.RunAll();     // F5: the GDExtension host from C# (wire form always; the native host and test plugin when built)

        Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
        return failures;
    }
}
