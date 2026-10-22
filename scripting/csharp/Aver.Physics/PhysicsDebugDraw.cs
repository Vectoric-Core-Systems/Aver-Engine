// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Jolt's debug renderer as data: ask for a set of overlays, get coloured lines (and a few text labels) in engine centimetres
// and axes, ready for whatever line drawing a tool has. For tools and tests -- the packaged game has no line-overlay path.
// A native library built without the debug renderer reports Available == false and every call returns nothing.

using System;
using System.Collections.Generic;
using System.Text;

namespace Aver.Physics;

/// <summary>What a collect draws (AVER_PHYS_DBG_*). OR them together.</summary>
[Flags]
public enum PhysicsDebugFlags : uint
{
    /// <summary>Wireframe shapes of bodies within the radius (meshes and heightfields are <see cref="StaticTriangles"/>).</summary>
    Bodies = 0x00000001,
    /// <summary>World-space bounding boxes.</summary>
    Bounds = 0x00000002,
    /// <summary>Centre-of-mass axes (AVER_PHYS_DBG_COM).</summary>
    Com = 0x00000004,
    Velocity = 0x00000008,
    /// <summary>Sleep-test spheres and timers.</summary>
    Sleep = 0x00000010,
    /// <summary>Mass and the box of equivalent inertia.</summary>
    Mass = 0x00000020,
    /// <summary>Joint frames, ragdoll joints, vehicle wheels and suspension.</summary>
    Constraints = 0x00000040,
    Limits = 0x00000080,
    /// <summary>Contact points and normals of the last fixed step; needs <see cref="PhysicsDebugDraw.Arm"/>.</summary>
    Contacts = 0x00000100,
    /// <summary>Supporting faces found by the narrow phase; needs <see cref="PhysicsDebugDraw.Arm"/>.</summary>
    SupportFaces = 0x00000200,
    SoftVertices = 0x00000400,
    /// <summary>Soft body edge, bend, volume, skin and LRA constraints.</summary>
    SoftEdges = 0x00000800,
    Characters = 0x00001000,
    /// <summary>Static mesh and heightfield triangles near the camera; needs a radius.</summary>
    StaticTriangles = 0x00002000,
    /// <summary>Where continuous collision ran; needs <see cref="PhysicsDebugDraw.Arm"/>.</summary>
    LinearCast = 0x00004000,
    Labels = 0x00008000,
    /// <summary>Bodies coloured by island.</summary>
    Islands = 0x00010000,
}

/// <summary>How bodies are coloured (AVER_PHYS_DBG_SCHEME_*).</summary>
public enum PhysicsDebugScheme
{
    Motion = 0,
    Sleep = 1,
    Layer = 2,
    Shape = 3,
    Island = 4,
    Instance = 5,
    /// <summary>How many schemes there are; not a scheme.</summary>
    Count = 6,
}

/// <summary>One text label of a collect: where, what colour (0..1), what it says.</summary>
public readonly record struct DebugLabel(Float3 Position, Float3 Color, string Text);

/// <summary>Jolt's debug renderer as data. Main thread, between steps.</summary>
public static class PhysicsDebugDraw
{
    private static float[] s_lines = new float[12 * 1024];
    private static int s_lineCount;
    private static readonly List<DebugLabel> s_labels = new();

    /// <summary>True when the native library was built with the debug renderer.</summary>
    public static bool Available => Native.aver_phys_debug_available() != 0;

    /// <summary>Chooses how bodies are coloured.</summary>
    public static bool SetColorScheme(PhysicsDebugScheme scheme) => Native.aver_phys_debug_set_color_scheme((int)scheme) != 0;

    /// <summary>Runs the draw calls for <paramref name="flags"/> within <paramref name="radiusCm"/> of
    /// <paramref name="camera"/> and returns the line count (at most <paramref name="maxLines"/>). Zero with no world, no flags or
    /// no renderer. The result is read through <see cref="Lines"/>, <see cref="Labels"/> and <see cref="Truncated"/>.</summary>
    public static int Collect(PhysicsDebugFlags flags, Float3 camera, float radiusCm = 4000f, int maxLines = 60000)
    {
        s_labels.Clear();
        s_lineCount = 0;
        int n = Native.aver_phys_debug_collect((uint)flags, camera.ToArray(), radiusCm, maxLines);
        if (n <= 0) return 0;

        if (s_lines.Length < n * 12) s_lines = new float[n * 12];
        s_lineCount = Native.aver_phys_debug_lines(s_lines, n);

        int labels = Native.aver_phys_debug_label_count();
        var xyz = new float[3];
        var rgb = new float[3];
        var text = new byte[256];
        for (int i = 0; i < labels; i++)
        {
            if (Native.aver_phys_debug_label_get(i, xyz, rgb, text, text.Length) == 0) break;
            int len = Array.IndexOf(text, (byte)0);
            s_labels.Add(new DebugLabel(new Float3(xyz[0], xyz[1], xyz[2]), new Float3(rgb[0], rgb[1], rgb[2]),
                                        Encoding.UTF8.GetString(text, 0, len < 0 ? text.Length : len)));
        }
        return s_lineCount;
    }

    /// <summary>The last collect's lines: 12 floats each, (x y z r g b) of the start then of the end, engine cm, colours 0..1.</summary>
    public static ReadOnlySpan<float> Lines => new ReadOnlySpan<float>(s_lines, 0, s_lineCount * 12);

    /// <summary>The last collect's text labels.</summary>
    public static IReadOnlyList<DebugLabel> Labels => s_labels;

    /// <summary>True if the last collect stopped at its line budget.</summary>
    public static bool Truncated => Native.aver_phys_debug_truncated() != 0;

    /// <summary>Asks the fixed step to capture contacts, supporting faces and continuous-collision markers (the
    /// <see cref="PhysicsDebugFlags.Contacts"/>, <see cref="PhysicsDebugFlags.SupportFaces"/> and
    /// <see cref="PhysicsDebugFlags.LinearCast"/> bits; others ignored, none disarms). Call it every frame the overlay is on.
    /// Returns the bits now armed.</summary>
    public static PhysicsDebugFlags Arm(PhysicsDebugFlags flags) => (PhysicsDebugFlags)(uint)Native.aver_phys_debug_arm((uint)flags);
}
