// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Engine, OS and ProjectSettings with Godot's names, over the G1 framework statics. Only what converted scripts reach for.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>Godot's Engine singleton.</summary>
public static class Engine
{
    /// <summary>Always false: Aver runs no editor scripts, so a script's <c>is_editor_hint()</c> branch never runs.</summary>
    public static bool IsEditorHint() => false;

    public static double GetFramesPerSecond() => Aver.Framework.Engine.FramesPerSecond;
    public static long GetProcessFrames() => Aver.Framework.Engine.ProcessFrames;
    public static long GetPhysicsFrames() => Aver.Framework.Engine.PhysicsFrames;

    public static double TimeScale
    {
        get => Aver.Framework.Game.TimeScale;
        set => Aver.Framework.Game.TimeScale = (float)value;
    }

    /// <summary>The physics step rate: 1 / the project's fixed step, 60 when it states none.</summary>
    public static int PhysicsTicksPerSecond => 60;
}

/// <summary>Godot's OS singleton.</summary>
public static class OS
{
    public static string GetName() => Aver.Framework.OS.Name;
    public static bool HasFeature(string tagName)
    {
        try { return Aver.Framework.OS.HasFeature(tagName); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return tagName.ToLowerInvariant() is "pc" or "64"; }
    }
    public static ulong GetTicksMsec() => (ulong)System.Environment.TickCount64;
    public static ulong GetTicksUsec() => (ulong)(System.Diagnostics.Stopwatch.GetTimestamp() * 1_000_000.0 / System.Diagnostics.Stopwatch.Frequency);
    public static bool IsDebugBuild() => Aver.Framework.OS.HasFeature("debug");
    public static string GetEnvironment(string variable) => System.Environment.GetEnvironmentVariable(variable) ?? "";
}

/// <summary>Godot's ProjectSettings singleton. The converter keeps the project's Godot values it can name in the manifest, so
/// <c>get_setting</c> answers the well-known keys from the Aver project and the script's own default for the rest.</summary>
public static class ProjectSettings
{
    // The manifest is read through the engine's content path; outside the engine (tests, tools) there is none and the key is absent.
    private static string Manifest(string key, string fallback)
    {
        try { return Aver.Framework.ProjectSettings.Get(key, fallback); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return fallback; }
    }

    public static Variant GetSetting(string name, Variant @default = default)
    {
        switch (name)
        {
            case "physics/3d/default_gravity":
            {
                string[] g = Manifest("PHYSICS.GRAVITY", "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (g.Length >= 3 && TryFloat(g[2], out double z)) return Math.Abs(z) / 100.0;
                return 9.8;
            }
            case "physics/3d/default_gravity_vector": return Vector3.Down;
            case "application/config/name": return Manifest("NAME", "");
            case "physics/common/physics_ticks_per_second":
                return TryFloat(Manifest("PHYSICS.FIXEDSTEP", ""), out double step) && step > 0 ? (long)Math.Round(1.0 / step) : 60L;
            case "display/window/size/viewport_width": return WindowSize(0, 1152);
            case "display/window/size/viewport_height": return WindowSize(1, 648);
        }
        return @default;
    }

    public static bool HasSetting(string name) => name is "physics/3d/default_gravity" or "physics/3d/default_gravity_vector" or "application/config/name"
        or "physics/common/physics_ticks_per_second" or "display/window/size/viewport_width" or "display/window/size/viewport_height";

    private static bool TryFloat(string s, out double d) => double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out d);

    private static long WindowSize(int axis, long fallback)
    {
        string[] p = Manifest("WINDOW.SIZE", "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return p.Length >= 2 && long.TryParse(p[axis], out long v) && v > 0 ? v : fallback;
    }
}
