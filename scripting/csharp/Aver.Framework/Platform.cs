// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Platform: where the project's files are (res://, user://), its manifest's settings, the OS and the engine's
// frame counters -- project settings, OS and engine information for scripts.

using System.Runtime.InteropServices;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>The project's directories and the <c>res://</c> and <c>user://</c> paths.</summary>
public static class Paths
{
    /// <summary>The directory holding the project's manifest; "" before a project opens.</summary>
    public static string Project => Marshal.PtrToStringUTF8(Fw.aver_fw_project_dir()) ?? "";

    /// <summary>The project's content directory, what <c>res://</c> names; "" before a project opens.</summary>
    public static string Content => Marshal.PtrToStringUTF8(Fw.aver_fw_content_dir()) ?? "";

    /// <summary>The player's writable directory for this project, what <c>user://</c> names (created on first use).</summary>
    public static string User
    {
        get
        {
            string d = Marshal.PtrToStringUTF8(Fw.aver_fw_user_dir()) ?? "";
            if (d.Length > 0) Directory.CreateDirectory(d);
            return d;
        }
    }

    /// <summary>An OS path for <paramref name="path"/>: <c>res://a/b</c> under <see cref="Content"/>, <c>user://a/b</c> under
    /// <see cref="User"/>, anything else unchanged.</summary>
    public static string Globalize(string path) => Globalize(path, Content, User);

    internal static string Globalize(string path, string content, string user)
    {
        if (path.StartsWith("res://", StringComparison.Ordinal)) return Join(content, path[6..]);
        if (path.StartsWith("user://", StringComparison.Ordinal)) return Join(user, path[7..]);
        return path;
    }

    private static string Join(string root, string rest) =>
        Path.Combine(root, rest.Replace('/', Path.DirectorySeparatorChar).TrimStart(Path.DirectorySeparatorChar));
}

/// <summary>The project manifest's settings by key, as the text the
/// manifest holds. The manifest is read once, when first asked.</summary>
public static class ProjectSettings
{
    private static Dictionary<string, string>? s_values;
    private static string s_readFrom = "";

    /// <summary>The value of a manifest key (any case), or <paramref name="fallback"/> when it is absent.</summary>
    public static string Get(string key, string fallback = "")
    {
        Load();
        return s_values!.TryGetValue(key, out string? v) ? v : fallback;
    }

    /// <summary>Whether the manifest states the key.</summary>
    public static bool Has(string key) { Load(); return s_values!.ContainsKey(key); }

    private static void Load()
    {
        string dir = Paths.Project;
        if (s_values != null && dir == s_readFrom) return;
        s_readFrom = dir;
        string? file = dir.Length > 0 && Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "*.ocproject").FirstOrDefault() : null;
        s_values = file != null ? Parse(File.ReadAllLines(file)) : new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    // `KEY value...` lines; '#' starts a comment line; the OCPROJECT header is skipped.
    internal static Dictionary<string, string> Parse(IEnumerable<string> lines)
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (string raw in lines)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') continue;
            int sp = line.IndexOfAny(new[] { ' ', '\t' });
            string key = sp < 0 ? line : line[..sp];
            if (key.Equals("OCPROJECT", StringComparison.OrdinalIgnoreCase)) continue;
            d[key] = sp < 0 ? "" : line[(sp + 1)..].Trim();
        }
        return d;
    }
}

/// <summary>The operating system and build (name and features).</summary>
public static class OS
{
    /// <summary>"Windows", "Linux" or "macOS".</summary>
    public static string Name =>
        OperatingSystem.IsWindows() ? "Windows" : OperatingSystem.IsMacOS() ? "macOS" : OperatingSystem.IsLinux() ? "Linux" : "Unknown";

    /// <summary>Whether a feature tag applies: the OS ("windows", "linux", "macos"), "pc", "64", the build ("debug" or
    /// "release") and the host ("editor" or "standalone"). Case-insensitive.</summary>
    public static bool HasFeature(string tag) => tag.ToLowerInvariant() switch
    {
        "windows" => OperatingSystem.IsWindows(),
        "linux" => OperatingSystem.IsLinux(),
        "macos" => OperatingSystem.IsMacOS(),
        "pc" => true,
        "64" => Environment.Is64BitProcess,
        "debug" => IsDebugBuild,
        "release" => !IsDebugBuild,
        "editor" => Fw.aver_fw_is_editor() != 0,
        "standalone" => Fw.aver_fw_is_editor() == 0,
        _ => false,
    };

#if DEBUG
    private const bool IsDebugBuild = true;
#else
    private const bool IsDebugBuild = false;
#endif
}

/// <summary>The engine's frame counters (frames per second, process frames and
/// physics frames). They count from when scripting loaded.</summary>
public static class Engine
{
    private static long s_frames, s_physicsFrames, s_windowStart;
    private static int s_windowFrames;
    private static float s_fps;

    /// <summary>Frames per second, measured over the last whole second of real time.</summary>
    public static float FramesPerSecond => s_fps;

    /// <summary>How many OnTick passes have run.</summary>
    public static long ProcessFrames => s_frames;

    /// <summary>How many OnPhysicsTick passes have run.</summary>
    public static long PhysicsFrames => s_physicsFrames;

    // The bridge's count of each pass. The FPS window is real time, whatever the time scale.
    internal static void NoteProcessFrame()
    {
        ++s_frames;
        long now = Environment.TickCount64;
        if (s_windowStart == 0) s_windowStart = now;
        ++s_windowFrames;
        long span = now - s_windowStart;
        if (span >= 1000)
        {
            s_fps = s_windowFrames * 1000f / span;
            s_windowFrames = 0;
            s_windowStart = now;
        }
    }

    internal static void NotePhysicsFrame() => ++s_physicsFrames;
}

/// <summary>World-space debug lines drawn on top of the frame. Colour is linear RGB in 0..1;
/// a shape lasts <c>seconds</c> of real time, or exactly one frame when 0. Centimetres, engine axes.</summary>
public static class DebugDraw
{
    private static readonly Vec3 White = new(1f, 1f, 1f);

    /// <summary>A line from <paramref name="from"/> to <paramref name="to"/>.</summary>
    public static void Line(Vec3 from, Vec3 to, Vec3? rgb = null, float seconds = 0f)
    {
        Vec3 c = rgb ?? White;
        Fw.aver_fw_debug_line(new[] { from.X, from.Y, from.Z }, new[] { to.X, to.Y, to.Z }, new[] { c.X, c.Y, c.Z }, seconds);
    }

    /// <summary>An axis-aligned box: its twelve edges.</summary>
    public static void Box(Vec3 center, Vec3 halfExtents, Vec3? rgb = null, float seconds = 0f)
    {
        Vec3 Corner(int i) => new(center.X + ((i & 1) != 0 ? halfExtents.X : -halfExtents.X),
                                  center.Y + ((i & 2) != 0 ? halfExtents.Y : -halfExtents.Y),
                                  center.Z + ((i & 4) != 0 ? halfExtents.Z : -halfExtents.Z));
        for (int i = 0; i < 8; ++i)
            for (int bit = 1; bit < 8; bit <<= 1)
                if ((i & bit) == 0) Line(Corner(i), Corner(i | bit), rgb, seconds);
    }

    /// <summary>A sphere as three great circles of <paramref name="segments"/> lines each.</summary>
    public static void Sphere(Vec3 center, float radius, Vec3? rgb = null, float seconds = 0f, int segments = 24)
    {
        segments = Math.Max(segments, 4);
        for (int axis = 0; axis < 3; ++axis)
        {
            Vec3 Point(int k)
            {
                float a = 2f * MathF.PI * k / segments, u = MathF.Cos(a) * radius, v = MathF.Sin(a) * radius;
                return axis switch
                {
                    0 => new Vec3(center.X, center.Y + u, center.Z + v),
                    1 => new Vec3(center.X + u, center.Y, center.Z + v),
                    _ => new Vec3(center.X + u, center.Y + v, center.Z),
                };
            }
            for (int k = 0; k < segments; ++k) Line(Point(k), Point(k + 1), rgb, seconds);
        }
    }
}

/// <summary>The display, for what a script may change at runtime.</summary>
public static class Display
{
    /// <summary>Asks the shipped game to turn vsync on or off; it applies it next frame where the device allows turning
    /// it off. The editor keeps its own vsync preference and ignores the request.</summary>
    public static void SetVSync(bool on) => Fw.aver_fw_request_vsync(on ? 1 : 0);
}

public static partial class Game
{
    /// <summary>Replaces the level at the end of this frame: every actor ends with
    /// <see cref="EndReason.Travel"/> except the GameInstance and <see cref="AverActor.Persistent"/> ones, the new level loads
    /// and its actors begin, then a GameMode, controller and pawn begin as at the start of play. <paramref name="levelPath"/>
    /// is content-relative (or <c>res://</c>). In the editor's Play it is refused with a warning: it would replace the level
    /// being edited.</summary>
    public static void ChangeLevel(string levelPath) => Fw.aver_fw_request_travel(levelPath);
}

public abstract partial class AverActor
{
    /// <summary>Survives level travel, with everything under it (an autoload). It is lifted to the top of the scene
    /// tree when the level changes, and ends with the play session.</summary>
    public bool Persistent
    {
        get => Fw.aver_fw_is_persistent(Self.Handle) != 0;
        set => Fw.aver_fw_set_persistent(Self.Handle, value ? 1 : 0);
    }
}
