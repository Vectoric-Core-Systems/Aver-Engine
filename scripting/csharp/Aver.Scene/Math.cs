namespace Aver.Scene;

/// <summary>
/// A position or direction in the engine's coordinate contract: centimetres, +X forward, +Y right,
/// +Z up, left-handed, row-vector. This is the same contract <c>.ocmap</c>'s PLACE record uses, so a
/// value authored here drops into a level without a second convention to reconcile.
/// </summary>
/// <remarks>
/// The struct is deliberately a plain value with public fields: it is data shipped across the ABI as
/// three floats, never a handle. It carries no behaviour beyond the small amount of vector algebra a
/// script needs at the surface; the authoritative transform maths lives natively in Core's Transform.
/// </remarks>
public struct Vec3 : System.IEquatable<Vec3>
{
    public float X;
    public float Y;
    public float Z;

    public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }

    /// <summary>World up (+Z), per the coordinate contract.</summary>
    public static Vec3 Up => new(0, 0, 1);
    /// <summary>World forward (+X), per the coordinate contract.</summary>
    public static Vec3 Forward => new(1, 0, 0);
    /// <summary>World right (+Y), per the coordinate contract.</summary>
    public static Vec3 Right => new(0, 1, 0);
    public static Vec3 Zero => new(0, 0, 0);
    public static Vec3 One => new(1, 1, 1);

    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vec3 operator *(Vec3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);

    public bool Equals(Vec3 o) => X == o.X && Y == o.Y && Z == o.Z;
    public override bool Equals(object? o) => o is Vec3 v && Equals(v);
    public override int GetHashCode() => System.HashCode.Combine(X, Y, Z);
    public override string ToString() => $"({X}, {Y}, {Z})";
}

/// <summary>
/// A rotation as a unit quaternion (x, y, z, w). This is the wire form the scene stores in
/// <c>CLocal.rotation</c> (FieldKind.Quat, four floats). Authors do not type quaternions by hand —
/// they type degrees through <see cref="Rot"/>; this type exists because the ABI field is a quat.
/// </summary>
public struct Quat : System.IEquatable<Quat>
{
    public float X;
    public float Y;
    public float Z;
    public float W;

    public Quat(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }

    public static Quat Identity => new(0, 0, 0, 1);

    /// <summary>
    /// A rotation of <paramref name="radians"/> about <paramref name="axis"/>. Left-handed, matching
    /// Core's Transform — a positive angle about +Z (Up) yaws forward (+X) towards right (+Y).
    /// </summary>
    public static Quat FromAxisAngle(Vec3 axis, float radians)
    {
        float half = radians * 0.5f;
        float s = System.MathF.Sin(half);
        // axis is assumed unit; the authoring surface only ever passes Vec3.Up/Right/Forward here.
        return new Quat(axis.X * s, axis.Y * s, axis.Z * s, System.MathF.Cos(half));
    }

    public static Quat operator *(Quat a, Quat b) => new(
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    /// <summary>The yaw component in degrees — the inverse of authoring a <see cref="Rot"/> yaw.</summary>
    public float YawDegrees()
    {
        float siny = 2f * (W * Z + X * Y);
        float cosy = 1f - 2f * (Y * Y + Z * Z);
        return System.MathF.Atan2(siny, cosy) * (180f / System.MathF.PI);
    }

    public bool Equals(Quat o) => X == o.X && Y == o.Y && Z == o.Z && W == o.W;
    public override bool Equals(object? o) => o is Quat q && Equals(q);
    public override int GetHashCode() => System.HashCode.Combine(X, Y, Z, W);
}

/// <summary>
/// Rotation as authors write it: three degrees — <see cref="Yaw"/> about Up (+Z), <see cref="Pitch"/>
/// about Right (+Y), <see cref="Roll"/> about Forward (+X). This is the form a viewport gizmo edits
/// and the form the generated <c>.Designer.cs</c> stores, because degrees are what a person reads back
/// off a diff. Composition is intrinsic yaw -> pitch -> roll, matching <c>.ocmap</c>'s PLACE.
/// </summary>
public struct Rot : System.IEquatable<Rot>
{
    public float Yaw;
    public float Pitch;
    public float Roll;

    public Rot(float yaw, float pitch, float roll) { Yaw = yaw; Pitch = pitch; Roll = roll; }

    public static Rot Zero => new(0, 0, 0);

    /// <summary>The quaternion the scene stores. Intrinsic Z(yaw) then Y(pitch) then X(roll).</summary>
    public Quat ToQuat()
    {
        const float d2r = System.MathF.PI / 180f;
        Quat qy = Quat.FromAxisAngle(Vec3.Up, Yaw * d2r);
        Quat qp = Quat.FromAxisAngle(Vec3.Right, Pitch * d2r);
        Quat qr = Quat.FromAxisAngle(Vec3.Forward, Roll * d2r);
        return qy * qp * qr;
    }

    public bool Equals(Rot o) => Yaw == o.Yaw && Pitch == o.Pitch && Roll == o.Roll;
    public override bool Equals(object? o) => o is Rot r && Equals(r);
    public override int GetHashCode() => System.HashCode.Combine(Yaw, Pitch, Roll);
}
