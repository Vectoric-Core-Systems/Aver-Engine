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
        if (args.Length == 1 && args[0] == "script-typing") // only the scripts package suite (typed node access, the runtime classes it needed)
        {
            failures = ScriptTypingTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "animation") // only the G5 A1 suite
        {
            failures = AnimationTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "audio") // only the G5 S1 suite
        {
            failures = AudioTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "navigation") // only the G5 N1 suite
        {
            failures = NavigationTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "network") // only the G6 NET1 suite
        {
            failures = NetworkTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "multiplayer") // only the G6 NET2 suites
        {
            failures = MultiplayerTests.RunAll() + ReplicationTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "ui") // only the G5 U1 suite
        {
            failures = UiTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "widgets") // only the G5 U2 suites
        {
            failures = WidgetTests.RunAll() + RichTextTests.RunAll();
            Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
            return failures;
        }
        if (args.Length == 1 && args[0] == "animtree") // only the G5 A2 suites
        {
            failures = CurvePathTests.RunAll() + AnimationTreeTests.RunAll();
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
        failures += ShaderTests.RunAll();    // G6 SH2: Shader, ShaderMaterial, global shader parameters and what they push to the engine's material library
        failures += EnvShaderTests.RunAll(); // G6 SH3: sky, fog and particles shaders, GPUParticles3D, VisualShader and what they register with the engine
        failures += GeometryTests.RunAll();// G4 R7: ArrayMesh, SurfaceTool, ImmediateMesh, MultiMesh, GridMap, CSG, Sprite3D, Label3D and what they push to the engine
        failures += SceneValueTests.RunAll(); // ClassDB, Object(...) values, resource and node-reference properties
        failures += TweenTests.RunAll();     // F2b: easing curves, tween sequencing, loops, pause and process modes
        failures += ServiceTests.RunAll();   // F2b: SceneTree frame signals, timers, await, groups, scene changes, ResourceLoader, PackedScene
        failures += InputTests.RunAll();     // F2b: Input and InputMap, InputEvent classes, the engine-event bridge, _Input delivery
        failures += PhysicsWrapperTests.RunAll(); // F2b: Camera3D maths, physics node defaults and conversions, vehicle, queries without a world
        failures += AnimationTests.RunAll(); // G5 A1: Animation, AnimationLibrary, AnimationPlayer over the engine's property-track player
        failures += CurvePathTests.RunAll(); // G5 A2: Expression, Curve, Curve3D, Gradient, PathFollow3D, Skeleton3D, SpringArm3D, RemoteTransform3D, on-screen notifiers
        failures += AnimationTreeTests.RunAll(); // G5 A2: AnimationTree over the engine's property-track player
        failures += AudioTests.RunAll();     // G5 S1: AudioServer, bus effects, audio streams, AudioStreamPlayer(3D), AudioListener3D over the engine's mixer (offline)
        failures += NavigationTests.RunAll(); // G5 N1: navigation maps, regions, links, obstacles, agents, baking, AStar3D / AStar2D / AStarGrid2D over Aver.Nav.Abi
        failures += NetworkTests.RunAll();    // G6 NET1: Variant binary encoding, StreamPeerBuffer, IP, UDP / TCP peers, ENetConnection and the multiplayer peers over Aver.Net.Abi (loopback only)
        failures += MultiplayerTests.RunAll(); // G6 NET2: SceneMultiplayer, RPC, authority over ENet loopback, a lossy UDP proxy and an in-memory link (branches of one tree)
        failures += ReplicationTests.RunAll(); // G6 NET2: MultiplayerSpawner, MultiplayerSynchronizer, SceneReplicationConfig, a converted GDScript with @rpc
        failures += UiTests.RunAll();         // G5 U1: Control layout, containers, themes, style boxes, stretch, the GUI (input, focus, tooltips, drag and drop) over Aver.UI.Abi
        failures += WidgetTests.RunAll();     // G5 U2: Label, Range, buttons, menus, dialogs, LineEdit, TextEdit, ItemList, Tree, tabs, layout widgets, colour picker, fonts
        failures += RichTextTests.RunAll();   // G5 U2: BBCode, RichTextLabel layout, effects, meta and hints
        failures += ConvertScriptsTests.RunAll(); // F3: Aver.Fabric.Convert, GodotSharp scripts rewritten, compiled against this runtime and run
        failures += GdScriptParserTests.RunAll(); // F4a: the GDScript tokenizer and parser (typed AST, error recovery)
        failures += GdScriptConvertTests.RunAll(); // F4b: GDScript converted to C#, compiled against this runtime, run against GDScript's results
        failures += ScriptTypingTests.RunAll(); // scripts package: typed $Path / get_node / %Unique / instantiate, built-in constructors, the runtime classes two demos needed
        failures += GdextTests.RunAll();     // F5: the GDExtension host from C# (wire form always; the native host and test plugin when built)

        Console.WriteLine(failures == 0 ? $"\nAll tests passed ({T.Checks} checks)." : $"\n{failures} check(s) failed.");
        return failures;
    }
}
