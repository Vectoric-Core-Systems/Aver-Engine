// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// FabricSpace: the one place Godot numbers become Aver numbers and back (the engine boundary).

using Aver.Scene;

namespace Aver.Fabric;

/// <summary>
/// Godot space (metres, +Y up, -Z forward, right-handed) to Aver space (centimetres, +Z up, +X forward, left-handed).
/// This is exactly the map the glTF importer applies (<c>toEngine</c> in modules/formats/src/GltfImport.cpp, also used by the
/// FBX, OBJ and USD importers and documented in docs/ASSET_IMPORT.md): a vector (x, y, z) becomes (-z, x, y), positions and
/// sizes are times 100, a rotation quaternion (x, y, z, w) becomes (z, -x, -y, w), a scale (x, y, z) becomes (z, x, y).
/// Scripts see only Godot numbers; every call into the engine goes through here.
/// </summary>
public static class FabricSpace
{
    /// <summary>Centimetres in a metre: the unit change on positions and sizes.</summary>
    public const float CmPerMetre = 100f;

    /// <summary>A position in metres to Aver centimetres.</summary>
    public static Vec3 ToAverPosition(Vector3 p) => new(-p.Z * CmPerMetre, p.X * CmPerMetre, p.Y * CmPerMetre);

    /// <summary>A position in Aver centimetres to Godot metres.</summary>
    public static Vector3 FromAverPosition(Vec3 p) => new(p.Y / CmPerMetre, p.Z / CmPerMetre, -p.X / CmPerMetre);

    /// <summary>A direction or any unitless vector: the axis map without the unit change.</summary>
    public static Vec3 ToAverDirection(Vector3 d) => new(-d.Z, d.X, d.Y);

    public static Vector3 FromAverDirection(Vec3 d) => new(d.Y, d.Z, -d.X);

    /// <summary>An angular velocity, torque or any rotation axis scaled by an angle: the map of the quaternion's vector part,
    /// (x, y, z) to (z, -x, -y). It is the direction map with the sign the map's determinant (-1) gives a pseudovector, so
    /// "spin about my up axis" keeps meaning the same physical spin once the axes are mirrored.</summary>
    public static Vec3 ToAverAngular(Vector3 w) => new(w.Z, -w.X, -w.Y);

    public static Vector3 FromAverAngular(Vec3 w) => new(-w.Y, -w.Z, w.X);

    /// <summary>A length in metres to centimetres (no axis).</summary>
    public static float ToAverLength(float metres) => metres * CmPerMetre;

    public static float FromAverLength(float cm) => cm / CmPerMetre;

    /// <summary>A rotation quaternion. The axis map has determinant -1, so the vector part takes a sign and w does not.</summary>
    public static Quat ToAverRotation(Quaternion q) => new(q.Z, -q.X, -q.Y, q.W);

    public static Quaternion FromAverRotation(Quat q) => new(-q.Y, -q.Z, q.X, q.W);

    /// <summary>A scale: dimensionless, so it permutes with the axes only.</summary>
    public static Vec3 ToAverScale(Vector3 s) => new(s.Z, s.X, s.Y);

    public static Vector3 FromAverScale(Vec3 s) => new(s.Y, s.Z, s.X);

    /// <summary>A Godot transform as the position, rotation and scale an Aver entity stores.</summary>
    public static void ToAver(Transform3D t, out Vec3 position, out Quat rotation, out Vec3 scale)
    {
        position = ToAverPosition(t.Origin);
        Vector3 s = t.Basis.GetScale();
        rotation = ToAverRotation(t.Basis.GetRotationQuaternion());
        scale = ToAverScale(s);
    }

    /// <summary>An Aver entity's position, rotation and scale as a Godot transform.</summary>
    public static Transform3D FromAver(Vec3 position, Quat rotation, Vec3 scale) =>
        new(Basis.FromQuaternionScale(FromAverRotation(rotation), FromAverScale(scale)), FromAverPosition(position));
}
