// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The scalar, integer, vector and rotator maths a script and a graph share (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2).
// AverMath is the public surface; MathGraph is the thin per-node wrapper layer the Math rows in
// Aver.Graph/GraphMathNodeTable.cs call (primitives only, nothing here throws).

using Aver.Scene;

namespace Aver.Framework;

/// <summary>Maths beyond <see cref="System.MathF"/>: ranges, interpolation, angles, integer and bit helpers, and
/// vector and rotator conversions in this engine's basis (+X forward, +Y right, +Z up, centimetres, degrees).</summary>
/// <remarks>Division-like operations never produce NaN or Infinity from a zero divisor: they return the neutral
/// value stated on each member, the convention of the graph Divide node.</remarks>
public static class AverMath
{
    /// <summary>Degrees to radians.</summary>
    public const float Deg2Rad = MathF.PI / 180f;
    /// <summary>Radians to degrees.</summary>
    public const float Rad2Deg = 180f / MathF.PI;

    // ---- scalar --------------------------------------------------------------------------------------

    /// <summary>Tangent of an angle in radians.</summary>
    public static float Tan(float a) => MathF.Tan(a);

    /// <summary>Arc sine in radians; the input is clamped to -1..1.</summary>
    public static float Asin(float a) => MathF.Asin(Math.Clamp(a, -1f, 1f));

    /// <summary>Arc cosine in radians; the input is clamped to -1..1.</summary>
    public static float Acos(float a) => MathF.Acos(Math.Clamp(a, -1f, 1f));

    /// <summary>Arc tangent in radians.</summary>
    public static float Atan(float a) => MathF.Atan(a);

    /// <summary>Angle in radians of the point (<paramref name="x"/>, <paramref name="y"/>) from +X.</summary>
    public static float Atan2(float y, float x) => MathF.Atan2(y, x);

    /// <summary>-1, 0 or 1 by the sign of <paramref name="a"/>; NaN gives 0.</summary>
    public static float Sign(float a) => a > 0f ? 1f : (a < 0f ? -1f : 0f);

    /// <summary>The fractional part, <c>a - floor(a)</c>, so a negative input still gives 0 up to 1.</summary>
    public static float Frac(float a) => a - MathF.Floor(a);

    /// <summary>Drops the fractional part, rounding toward zero.</summary>
    public static float Truncate(float a) => MathF.Truncate(a);

    /// <summary>Degrees to radians.</summary>
    public static float DegToRad(float degrees) => degrees * Deg2Rad;

    /// <summary>Radians to degrees.</summary>
    public static float RadToDeg(float radians) => radians * Rad2Deg;

    /// <summary>e raised to <paramref name="a"/>.</summary>
    public static float Exp(float a) => MathF.Exp(a);

    /// <summary>Natural logarithm; zero or negative input gives 0 rather than NaN.</summary>
    public static float Log(float a) => a > 0f ? MathF.Log(a) : 0f;

    /// <summary>Smooth 0 to 1 ramp between two edges (<c>3t^2 - 2t^3</c>). Equal edges give a step at the edge.</summary>
    public static float SmoothStep(float edge0, float edge1, float x)
    {
        if (edge0 == edge1) return x < edge0 ? 0f : 1f;
        float t = Math.Clamp((x - edge0) / (edge1 - edge0), 0f, 1f);
        return t * t * (3f - 2f * t);
    }

    /// <summary>Where <paramref name="value"/> sits between <paramref name="a"/> and <paramref name="b"/> (0 at a, 1 at b),
    /// not clamped. Equal ends give 0.</summary>
    public static float InverseLerp(float a, float b, float value) => a == b ? 0f : (value - a) / (b - a);

    /// <summary>Remaps <paramref name="value"/> from one range to another. An empty input range gives
    /// <paramref name="outMin"/>.</summary>
    public static float MapRange(float value, float inMin, float inMax, float outMin, float outMax, bool clamp = true)
    {
        if (inMin == inMax) return outMin;
        float t = (value - inMin) / (inMax - inMin);
        if (clamp) t = Math.Clamp(t, 0f, 1f);
        return outMin + (outMax - outMin) * t;
    }

    /// <summary>Wraps <paramref name="value"/> into [<paramref name="min"/>, <paramref name="max"/>). An empty or
    /// reversed range gives <paramref name="min"/>.</summary>
    public static float Wrap(float value, float min, float max)
    {
        if (!(max > min)) return min;
        float range = max - min;
        float r = (value - min) - range * MathF.Floor((value - min) / range);
        if (r >= range) r = 0f;
        return min + r;
    }

    /// <summary>One frame of exponential approach to <paramref name="target"/>. A step never overshoots; a speed
    /// of zero or less snaps to the target, a delta time of zero or less stays put.</summary>
    public static float InterpTo(float current, float target, float deltaTime, float speed)
    {
        if (speed <= 0f) return target;
        float dist = target - current;
        if (MathF.Abs(dist) < 1e-8f) return target;
        if (deltaTime <= 0f) return current;
        return current + dist * Math.Clamp(deltaTime * speed, 0f, 1f);
    }

    /// <summary>The signed shortest turn in degrees from <paramref name="a"/> to <paramref name="b"/>, in (-180, 180].</summary>
    public static float DeltaAngle(float a, float b)
    {
        float d = (b - a) % 360f;
        if (d < 0f) d += 360f;
        if (d > 180f) d -= 360f;
        return d;
    }

    /// <summary>Interpolates between two angles in degrees the short way round. <paramref name="t"/> is not clamped and the
    /// result is not wrapped (350 to 10 at 0.5 is 360, not 0), so it stays continuous for a caller that keeps interpolating.</summary>
    public static float LerpAngle(float a, float b, float t) => a + DeltaAngle(a, b) * t;

    /// <summary>True when the two values differ by no more than <paramref name="tolerance"/>.</summary>
    public static bool NearlyEqual(float a, float b, float tolerance = 0.0001f) => MathF.Abs(a - b) <= tolerance;

    /// <summary>True for any value that is not NaN or an infinity.</summary>
    public static bool IsFinite(float a) => float.IsFinite(a);

    // ---- integer and bits ------------------------------------------------------------------------------

    /// <summary>Wrapping addition.</summary>
    public static int IntAdd(int a, int b) => unchecked(a + b);

    /// <summary>Wrapping subtraction.</summary>
    public static int IntSubtract(int a, int b) => unchecked(a - b);

    /// <summary>Wrapping multiplication.</summary>
    public static int IntMultiply(int a, int b) => unchecked(a * b);

    /// <summary>Division truncating toward zero; a zero divisor gives 0.</summary>
    public static int IntDivide(int a, int b)
    {
        if (b == 0) return 0;
        if (b == -1) return unchecked(-a);
        return a / b;
    }

    /// <summary>Remainder whose sign follows the dividend; a zero divisor gives 0.</summary>
    public static int IntModulo(int a, int b) => b == 0 || b == -1 ? 0 : a % b;

    /// <summary><c>min(max(a, min), max)</c>.</summary>
    public static int IntClamp(int a, int min, int max) => Math.Min(Math.Max(a, min), max);

    /// <summary>Absolute value; <see cref="int.MinValue"/> stays negative.</summary>
    public static int IntAbs(int a) => a < 0 ? unchecked(-a) : a;

    /// <summary>-1, 0 or 1.</summary>
    public static int IntSign(int a) => a > 0 ? 1 : (a < 0 ? -1 : 0);

    /// <summary>Shifts left; the count is masked to 0..31.</summary>
    public static int BitShiftLeft(int a, int count) => unchecked(a << (count & 31));

    /// <summary>Shifts right filling with zeros (not the sign bit), so a mask with the top bit set shifts as a mask;
    /// the count is masked to 0..31.</summary>
    public static int BitShiftRight(int a, int count) => unchecked((int)((uint)a >> (count & 31)));

    /// <summary>True when bit <paramref name="bit"/> (0 is the lowest) is set; a bit outside 0..31 is false.</summary>
    public static bool BitTest(int value, int bit) => (uint)bit < 32u && (((uint)value >> bit) & 1u) != 0;

    // ---- vectors ---------------------------------------------------------------------------------------

    /// <summary>Length of (<paramref name="x"/>, <paramref name="y"/>), ignoring Z.</summary>
    public static float Length2D(float x, float y) => MathF.Sqrt(x * x + y * y);

    /// <summary>Distance between two points on the ground plane, ignoring Z.</summary>
    public static float Distance2D(float ax, float ay, float bx, float by) => Length2D(ax - bx, ay - by);

    /// <summary>Mirrors <paramref name="v"/> about a surface with the given normal (which need not be unit).
    /// A zero normal returns <paramref name="v"/>.</summary>
    public static Vec3 Reflect(Vec3 v, Vec3 normal)
    {
        Vec3 n = normal.Normalized;
        if (n.IsNearlyZero) return v;
        return v - n * (2f * Vec3.Dot(v, n));
    }

    /// <summary>The part of <paramref name="a"/> along <paramref name="onto"/>. A zero <paramref name="onto"/> gives zero.</summary>
    public static Vec3 Project(Vec3 a, Vec3 onto)
    {
        float bb = Vec3.Dot(onto, onto);
        if (bb < 1e-12f) return Vec3.Zero;
        return onto * (Vec3.Dot(a, onto) / bb);
    }

    /// <summary>Shortens <paramref name="v"/> to at most <paramref name="maxLength"/> keeping its direction. A maximum of
    /// zero or less gives zero.</summary>
    public static Vec3 ClampLength(Vec3 v, float maxLength)
    {
        if (!(maxLength > 0f)) return Vec3.Zero;
        float len = v.Length;
        return len <= maxLength ? v : v * (maxLength / len);
    }

    /// <summary>The angle in degrees between two directions, 0..180. Either being zero gives 0.</summary>
    public static float Angle(Vec3 a, Vec3 b)
    {
        float la = a.Length, lb = b.Length;
        if (la < 1e-6f || lb < 1e-6f) return 0f;
        return MathF.Acos(Math.Clamp(Vec3.Dot(a, b) / (la * lb), -1f, 1f)) * Rad2Deg;
    }

    /// <summary><see cref="InterpTo(float, float, float, float)"/> applied to each component.</summary>
    public static Vec3 InterpTo(Vec3 current, Vec3 target, float deltaTime, float speed) =>
        new(InterpTo(current.X, target.X, deltaTime, speed),
            InterpTo(current.Y, target.Y, deltaTime, speed),
            InterpTo(current.Z, target.Z, deltaTime, speed));

    // ---- rotators ----------------------------------------------------------------------------------------
    // Rot's convention (Aver.Scene/Math.cs): yaw about +Z, pitch about +Y. A positive pitch turns +X toward -Z,
    // so it tips the nose DOWN, the same sign GraphInterop.LookAtForGraph writes.

    /// <summary>The unit direction a <see cref="Rot"/> points along: <c>Rot.ToQuat()</c> applied to <see cref="Vec3.Forward"/>.
    /// Roll does not move it.</summary>
    public static Vec3 ForwardOf(Rot r)
    {
        float y = r.Yaw * Deg2Rad, p = r.Pitch * Deg2Rad;
        float cp = MathF.Cos(p);
        return new Vec3(MathF.Cos(y) * cp, MathF.Sin(y) * cp, -MathF.Sin(p));
    }

    /// <summary>The yaw and pitch (roll 0) that make <see cref="Vec3.Forward"/> point along <paramref name="direction"/>,
    /// the inverse of <see cref="ForwardOf"/>. A zero direction gives zero; straight up or down gives yaw 0.</summary>
    public static Rot RotatorOf(Vec3 direction)
    {
        if (direction.LengthSquared < 1e-12f) return Rot.Zero;
        float horiz = Length2D(direction.X, direction.Y);
        float yaw = horiz < 1e-6f ? 0f : MathF.Atan2(direction.Y, direction.X) * Rad2Deg;
        float pitch = -MathF.Atan2(direction.Z, horiz) * Rad2Deg;
        return new Rot(yaw, pitch, 0f);
    }

    /// <summary>The yaw and pitch (roll 0) that turn an observer at <paramref name="from"/> to face <paramref name="to"/>.</summary>
    public static Rot LookAt(Vec3 from, Vec3 to) => RotatorOf(to - from);

    /// <summary>Rotates <paramref name="v"/> by <paramref name="r"/> (<c>v' = q v q*</c> with <c>q = r.ToQuat()</c>).</summary>
    public static Vec3 Rotate(Vec3 v, Rot r)
    {
        Quat q = r.ToQuat();
        float tx = 2f * (q.Y * v.Z - q.Z * v.Y);
        float ty = 2f * (q.Z * v.X - q.X * v.Z);
        float tz = 2f * (q.X * v.Y - q.Y * v.X);
        return new Vec3(
            v.X + q.W * tx + (q.Y * tz - q.Z * ty),
            v.Y + q.W * ty + (q.Z * tx - q.X * tz),
            v.Z + q.W * tz + (q.X * ty - q.Y * tx));
    }
}

/// <summary>The per-node wrappers behind the Math rows of the node table: primitives in, primitives out, shaped for
/// the generic emitter. Each one calls the public <see cref="AverMath"/> / <see cref="AverRandom"/> member of the same
/// name; none throws.</summary>
internal static class MathGraph
{
    // ---- scalar ------------------------------------------------------------------------------------------

    /// <summary>Tangent of an angle in radians.</summary>
    internal static float TanForGraph(float a) => AverMath.Tan(a);
    /// <summary>Arc sine in radians; the input is clamped to -1..1.</summary>
    internal static float AsinForGraph(float a) => AverMath.Asin(a);
    /// <summary>Arc cosine in radians; the input is clamped to -1..1.</summary>
    internal static float AcosForGraph(float a) => AverMath.Acos(a);
    /// <summary>Arc tangent in radians.</summary>
    internal static float AtanForGraph(float a) => AverMath.Atan(a);
    /// <summary>Angle in radians of the point (x, y) measured from +X.</summary>
    internal static float Atan2ForGraph(float y, float x) => AverMath.Atan2(y, x);
    /// <summary>-1, 0 or 1 by the sign of the input.</summary>
    internal static float SignForGraph(float a) => AverMath.Sign(a);
    /// <summary>The fractional part, a minus floor(a).</summary>
    internal static float FracForGraph(float a) => AverMath.Frac(a);
    /// <summary>Drops the fractional part, rounding toward zero.</summary>
    internal static float TruncateForGraph(float a) => AverMath.Truncate(a);
    /// <summary>Degrees to radians.</summary>
    internal static float DegToRadForGraph(float a) => AverMath.DegToRad(a);
    /// <summary>Radians to degrees.</summary>
    internal static float RadToDegForGraph(float a) => AverMath.RadToDeg(a);
    /// <summary>e raised to the input.</summary>
    internal static float ExpForGraph(float a) => AverMath.Exp(a);
    /// <summary>Natural logarithm; zero or negative input gives 0.</summary>
    internal static float LogForGraph(float a) => AverMath.Log(a);
    /// <summary>Smooth 0 to 1 ramp between two edges; equal edges give a step.</summary>
    internal static float SmoothStepForGraph(float edge0, float edge1, float x) => AverMath.SmoothStep(edge0, edge1, x);
    /// <summary>Where the value sits between a and b, not clamped; equal ends give 0.</summary>
    internal static float InverseLerpForGraph(float a, float b, float value) => AverMath.InverseLerp(a, b, value);
    /// <summary>Remaps a value from one range to another, optionally clamped; an empty input range gives the output minimum.</summary>
    internal static float MapRangeForGraph(float value, float inMin, float inMax, float outMin, float outMax, bool clamp) =>
        AverMath.MapRange(value, inMin, inMax, outMin, outMax, clamp);
    /// <summary>Wraps a value into [min, max); an empty range gives min.</summary>
    internal static float WrapForGraph(float value, float min, float max) => AverMath.Wrap(value, min, max);
    /// <summary>One frame of exponential approach to a target that never overshoots; a speed of zero or less snaps to it.</summary>
    internal static float InterpToForGraph(float current, float target, float deltaTime, float speed) =>
        AverMath.InterpTo(current, target, deltaTime, speed);
    /// <summary>The signed shortest turn in degrees from a to b.</summary>
    internal static float DeltaAngleForGraph(float a, float b) => AverMath.DeltaAngle(a, b);
    /// <summary>Interpolates between two angles in degrees the short way round; the result is not wrapped to 0..360.</summary>
    internal static float LerpAngleForGraph(float a, float b, float t) => AverMath.LerpAngle(a, b, t);
    /// <summary>True when the two values differ by no more than the tolerance.</summary>
    internal static bool NearlyEqualForGraph(float a, float b, float tolerance) => AverMath.NearlyEqual(a, b, tolerance);
    /// <summary>True for any value that is not NaN or an infinity.</summary>
    internal static bool IsFiniteForGraph(float a) => AverMath.IsFinite(a);

    // ---- integer and bits --------------------------------------------------------------------------------

    /// <summary>Wrapping integer addition.</summary>
    internal static int IntAddForGraph(int a, int b) => AverMath.IntAdd(a, b);
    /// <summary>Wrapping integer subtraction.</summary>
    internal static int IntSubtractForGraph(int a, int b) => AverMath.IntSubtract(a, b);
    /// <summary>Wrapping integer multiplication.</summary>
    internal static int IntMultiplyForGraph(int a, int b) => AverMath.IntMultiply(a, b);
    /// <summary>The smaller integer.</summary>
    internal static int IntMinForGraph(int a, int b) => Math.Min(a, b);
    /// <summary>The larger integer.</summary>
    internal static int IntMaxForGraph(int a, int b) => Math.Max(a, b);
    /// <summary>Integer division truncating toward zero; a zero divisor gives 0.</summary>
    internal static int IntDivideForGraph(int a, int b) => AverMath.IntDivide(a, b);
    /// <summary>Integer remainder, sign of the dividend; a zero divisor gives 0.</summary>
    internal static int IntModuloForGraph(int a, int b) => AverMath.IntModulo(a, b);
    /// <summary>Clamps an integer into [min, max].</summary>
    internal static int IntClampForGraph(int a, int min, int max) => AverMath.IntClamp(a, min, max);
    /// <summary>Integer absolute value.</summary>
    internal static int IntAbsForGraph(int a) => AverMath.IntAbs(a);
    /// <summary>-1, 0 or 1 by the sign of an integer.</summary>
    internal static int IntSignForGraph(int a) => AverMath.IntSign(a);
    /// <summary>Exact integer comparison (floats lose integers above 2^24).</summary>
    internal static void IntCompareForGraph(int a, int b, out bool less, out bool equal, out bool greater)
    {
        less = a < b; equal = a == b; greater = a > b;
    }
    /// <summary>Bitwise and.</summary>
    internal static int BitAndForGraph(int a, int b) => a & b;
    /// <summary>Bitwise or.</summary>
    internal static int BitOrForGraph(int a, int b) => a | b;
    /// <summary>Bitwise exclusive or.</summary>
    internal static int BitXorForGraph(int a, int b) => a ^ b;
    /// <summary>Shifts left; the count is masked to 0..31.</summary>
    internal static int BitShiftLeftForGraph(int a, int count) => AverMath.BitShiftLeft(a, count);
    /// <summary>Shifts right filling with zeros; the count is masked to 0..31.</summary>
    internal static int BitShiftRightForGraph(int a, int count) => AverMath.BitShiftRight(a, count);
    /// <summary>True when the given bit (0 is the lowest) of the value is set.</summary>
    internal static bool BitTestForGraph(int value, int bit) => AverMath.BitTest(value, bit);

    // ---- random ------------------------------------------------------------------------------------------

    /// <summary>A random float in [min, max) from the shared generator.</summary>
    internal static float RandomFloatForGraph(float min, float max) => AverRandom.NextFloat(min, max);
    /// <summary>A random integer in [min, max], both ends included.</summary>
    internal static int RandomIntForGraph(int min, int max) => AverRandom.NextInt(min, max);
    /// <summary>True with the given probability, 0..1.</summary>
    internal static bool RandomBoolForGraph(float chance) => AverRandom.NextBool(chance);
    /// <summary>A direction uniformly distributed over the unit sphere.</summary>
    internal static void RandomUnitVectorForGraph(out float x, out float y, out float z)
    {
        Vec3 v = AverRandom.UnitVector();
        x = v.X; y = v.Y; z = v.Z;
    }
    /// <summary>Reseeds the shared generator so the same seed repeats the same sequence.</summary>
    internal static void SetRandomSeedForGraph(int seed) => AverRandom.Seed(seed);

    // ---- vectors -----------------------------------------------------------------------------------------

    /// <summary>The negated vector.</summary>
    internal static void VecNegateForGraph(float ax, float ay, float az, out float x, out float y, out float z)
    {
        x = -ax; y = -ay; z = -az;
    }
    /// <summary>Component-wise product of two vectors.</summary>
    internal static void VecMultiplyForGraph(float ax, float ay, float az, float bx, float by, float bz,
                                             out float x, out float y, out float z)
    {
        x = ax * bx; y = ay * by; z = az * bz;
    }
    /// <summary>Length of (x, y) ignoring Z.</summary>
    internal static float VecLength2DForGraph(float ax, float ay) => AverMath.Length2D(ax, ay);
    /// <summary>Distance between two points ignoring Z.</summary>
    internal static float VecDistance2DForGraph(float ax, float ay, float bx, float by) => AverMath.Distance2D(ax, ay, bx, by);
    /// <summary>Mirrors a vector about a surface normal that need not be unit.</summary>
    internal static void VecReflectForGraph(float vx, float vy, float vz, float nx, float ny, float nz,
                                            out float x, out float y, out float z)
    {
        Vec3 r = AverMath.Reflect(new Vec3(vx, vy, vz), new Vec3(nx, ny, nz));
        x = r.X; y = r.Y; z = r.Z;
    }
    /// <summary>The part of a along b; a zero b gives zero.</summary>
    internal static void VecProjectForGraph(float ax, float ay, float az, float bx, float by, float bz,
                                            out float x, out float y, out float z)
    {
        Vec3 r = AverMath.Project(new Vec3(ax, ay, az), new Vec3(bx, by, bz));
        x = r.X; y = r.Y; z = r.Z;
    }
    /// <summary>Shortens a vector to at most the given length, keeping its direction.</summary>
    internal static void VecClampLengthForGraph(float vx, float vy, float vz, float maxLength,
                                                out float x, out float y, out float z)
    {
        Vec3 r = AverMath.ClampLength(new Vec3(vx, vy, vz), maxLength);
        x = r.X; y = r.Y; z = r.Z;
    }
    /// <summary>The angle in degrees between two vectors; a zero vector gives 0.</summary>
    internal static float VecAngleForGraph(float ax, float ay, float az, float bx, float by, float bz) =>
        AverMath.Angle(new Vec3(ax, ay, az), new Vec3(bx, by, bz));

    // ---- rotators ----------------------------------------------------------------------------------------

    /// <summary>The unit direction a yaw and pitch (degrees) point along.</summary>
    internal static void RotatorToForwardForGraph(float yaw, float pitch, out float x, out float y, out float z)
    {
        Vec3 r = AverMath.ForwardOf(new Rot(yaw, pitch, 0f));
        x = r.X; y = r.Y; z = r.Z;
    }
    /// <summary>The yaw and pitch in degrees that point along a direction.</summary>
    internal static void ForwardToRotatorForGraph(float x, float y, float z, out float yaw, out float pitch)
    {
        Rot r = AverMath.RotatorOf(new Vec3(x, y, z));
        yaw = r.Yaw; pitch = r.Pitch;
    }
    /// <summary>Rotates a vector by yaw, pitch and roll in degrees.</summary>
    internal static void RotateVectorForGraph(float vx, float vy, float vz, float yaw, float pitch, float roll,
                                              out float x, out float y, out float z)
    {
        Vec3 r = AverMath.Rotate(new Vec3(vx, vy, vz), new Rot(yaw, pitch, roll));
        x = r.X; y = r.Y; z = r.Z;
    }
    /// <summary>The yaw and pitch in degrees that turn an observer at one point to face another.</summary>
    internal static void LookAtRotationForGraph(float fromX, float fromY, float fromZ, float toX, float toY, float toZ,
                                                out float yaw, out float pitch)
    {
        Rot r = AverMath.LookAt(new Vec3(fromX, fromY, fromZ), new Vec3(toX, toY, toZ));
        yaw = r.Yaw; pitch = r.Pitch;
    }
}
