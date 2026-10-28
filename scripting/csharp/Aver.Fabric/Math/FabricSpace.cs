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

    /// <summary>A Godot affine transform as the engine's instance matrix: 12 floats, the row-vector world matrix with its 4th column dropped (three basis rows, then
    /// the translation in centimetres), written at <paramref name="offset"/>. Row i is where the engine's axis i goes, so a shear or a mirror survives.</summary>
    public static void ToAverMatrix12(Transform3D t, float[] dest, int offset = 0)
    {
        // The engine axis u_i, seen from Godot, goes through the Godot basis and comes back through the map.
        Vec3 r0 = ToAverDirection(t.Basis * FromAverDirection(new Vec3(1, 0, 0)));
        Vec3 r1 = ToAverDirection(t.Basis * FromAverDirection(new Vec3(0, 1, 0)));
        Vec3 r2 = ToAverDirection(t.Basis * FromAverDirection(new Vec3(0, 0, 1)));
        Vec3 o = ToAverPosition(t.Origin);
        dest[offset + 0] = r0.X; dest[offset + 1] = r0.Y; dest[offset + 2] = r0.Z;
        dest[offset + 3] = r1.X; dest[offset + 4] = r1.Y; dest[offset + 5] = r1.Z;
        dest[offset + 6] = r2.X; dest[offset + 7] = r2.Y; dest[offset + 8] = r2.Z;
        dest[offset + 9] = o.X; dest[offset + 10] = o.Y; dest[offset + 11] = o.Z;
    }

    /// <summary>The tangent the engine stores for a vertex whose Godot normal and tangent (xyz and handedness) are given (G4 R1's contract): the unit tangent
    /// made orthogonal to the normal, and w = +1/-1 so that cross(N, T) * w points along increasing v. Godot's bitangent cross(N, T) * w points at decreasing v
    /// (the glTF rule), so the direction of increasing v is its negation. All in engine space; a vector with no usable tangent gives all zeros.</summary>
    public static void ToAverTangent(Vector3 normal, Vector3 tangent, float handedness, float[] dest, int offset)
    {
        Vec3 n = ToAverDirection(normal), t = ToAverDirection(tangent);
        Vector3 b = normal.Cross(tangent) * handedness;
        Vec3 dirV = ToAverDirection(-b);
        float d = t.X * n.X + t.Y * n.Y + t.Z * n.Z;
        float ox = t.X - n.X * d, oy = t.Y - n.Y * d, oz = t.Z - n.Z * d;
        float len = MathF.Sqrt(ox * ox + oy * oy + oz * oz);
        if (!(len > 1e-12f)) { dest[offset] = dest[offset + 1] = dest[offset + 2] = dest[offset + 3] = 0f; return; }
        ox /= len; oy /= len; oz /= len;
        float cx = n.Y * oz - n.Z * oy, cy = n.Z * ox - n.X * oz, cz = n.X * oy - n.Y * ox;
        float s = cx * dirV.X + cy * dirV.Y + cz * dirV.Z;
        dest[offset] = ox; dest[offset + 1] = oy; dest[offset + 2] = oz; dest[offset + 3] = s < 0f ? -1f : 1f;
    }

    /// <summary>An Aver entity's position, rotation and scale as a Godot transform.</summary>
    public static Transform3D FromAver(Vec3 position, Quat rotation, Vec3 scale) =>
        new(Basis.FromQuaternionScale(FromAverRotation(rotation), FromAverScale(scale)), FromAverPosition(position));
}
