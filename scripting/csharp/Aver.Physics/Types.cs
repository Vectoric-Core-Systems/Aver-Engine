// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// Aver.Physics is a leaf and cannot reference Aver.Scene, so it cannot take or return Aver.Scene's
// Vec3/Quat the way Aver.Framework's existing Physics.cs does. Every method below therefore also
// takes or returns a plain (x, y, z) triple; Float3/Quaternion exist only so a caller does not have
// to spell three floats at every call site when a struct reads better. Named to avoid colliding with
// System.Numerics.Vector3 and with Aver.Scene.Vec3, either of which may be in scope at the same call
// site once Aver.Framework's shim converts between the two.

namespace Aver.Physics;

/// <summary>A position, direction or axis in engine centimetres (or a unitless axis), engine axes:
/// +X forward, +Y right, +Z up, left-handed.</summary>
public readonly struct Float3
{
    public float X { get; }
    public float Y { get; }
    public float Z { get; }

    public Float3(float x, float y, float z) { X = x; Y = y; Z = z; }

    public static Float3 Zero => new(0, 0, 0);
    /// <summary>The engine's own up, (0, 0, 1) -- the default water surface normal and the axis every
    /// capsule/cylinder/tapered-capsule shape stands upright along.</summary>
    public static Float3 Up => new(0, 0, 1);

    /// <summary>Widens to the three-float form every native call actually takes.</summary>
    internal void Deconstruct(out float x, out float y, out float z) { x = X; y = Y; z = Z; }

    /// <summary>Packs into the xyz float[3] the ABI's array-shaped parameters take.</summary>
    internal float[] ToArray() => new[] { X, Y, Z };

    public static Float3 operator +(Float3 a, Float3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Float3 operator -(Float3 a, Float3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Float3 operator *(Float3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);

    public override string ToString() => $"({X}, {Y}, {Z})";
}

/// <summary>A rotation as Jolt and the ABI exchange it: (x, y, z, w), engine axes.</summary>
public readonly struct Quaternion
{
    public float X { get; }
    public float Y { get; }
    public float Z { get; }
    public float W { get; }

    public Quaternion(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }

    public static Quaternion Identity => new(0, 0, 0, 1);

    public override string ToString() => $"({X}, {Y}, {Z}, {W})";
}
