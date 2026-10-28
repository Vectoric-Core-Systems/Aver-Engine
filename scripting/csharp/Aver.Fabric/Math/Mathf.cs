// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Mathf: GodotSharp's math helpers (float and double overloads), with Godot's documented edge cases.

using System.Runtime.CompilerServices;

namespace Aver.Fabric;

/// <summary>Math constants and helpers, named as in GodotSharp.</summary>
public static class Mathf
{
    public const float E = 2.7182818284590452354f;
    public const float Sqrt2 = 1.4142135623730950488f;
    public const float Epsilon = 1e-06f;
    public const float Pi = 3.1415926535897932385f;
    public const float Tau = 6.2831853071795864769f;
    public const float Inf = float.PositiveInfinity;
    public const float NaN = float.NaN;

    [MethodImpl(MethodImplOptions.AggressiveInlining)] public static int Abs(int s) => Math.Abs(s);
    [MethodImpl(MethodImplOptions.AggressiveInlining)] public static float Abs(float s) => Math.Abs(s);
    [MethodImpl(MethodImplOptions.AggressiveInlining)] public static double Abs(double s) => Math.Abs(s);

    public static float Acos(float s) => MathF.Acos(s);
    public static double Acos(double s) => Math.Acos(s);
    public static float Asin(float s) => MathF.Asin(s);
    public static double Asin(double s) => Math.Asin(s);
    public static float Atan(float s) => MathF.Atan(s);
    public static double Atan(double s) => Math.Atan(s);
    public static float Atan2(float y, float x) => MathF.Atan2(y, x);
    public static double Atan2(double y, double x) => Math.Atan2(y, x);
    public static float Cos(float s) => MathF.Cos(s);
    public static double Cos(double s) => Math.Cos(s);
    public static float Cosh(float s) => MathF.Cosh(s);
    public static double Cosh(double s) => Math.Cosh(s);
    public static float Sin(float s) => MathF.Sin(s);
    public static double Sin(double s) => Math.Sin(s);
    public static float Sinh(float s) => MathF.Sinh(s);
    public static double Sinh(double s) => Math.Sinh(s);
    public static float Tan(float s) => MathF.Tan(s);
    public static double Tan(double s) => Math.Tan(s);
    public static float Tanh(float s) => MathF.Tanh(s);
    public static double Tanh(double s) => Math.Tanh(s);
    public static float Sqrt(float s) => MathF.Sqrt(s);
    public static double Sqrt(double s) => Math.Sqrt(s);
    public static float Exp(float s) => MathF.Exp(s);
    public static double Exp(double s) => Math.Exp(s);
    public static float Log(float s) => MathF.Log(s);
    public static double Log(double s) => Math.Log(s);
    public static float Pow(float x, float y) => MathF.Pow(x, y);
    public static double Pow(double x, double y) => Math.Pow(x, y);

    public static float Ceil(float s) => MathF.Ceiling(s);
    public static double Ceil(double s) => Math.Ceiling(s);
    public static int CeilToInt(float s) => (int)MathF.Ceiling(s);
    public static int CeilToInt(double s) => (int)Math.Ceiling(s);
    public static float Floor(float s) => MathF.Floor(s);
    public static double Floor(double s) => Math.Floor(s);
    public static int FloorToInt(float s) => (int)MathF.Floor(s);
    public static int FloorToInt(double s) => (int)Math.Floor(s);

    /// <summary>Rounds half away from zero, as Godot's <c>round</c> does.</summary>
    public static float Round(float s) => MathF.Round(s, MidpointRounding.AwayFromZero);
    public static double Round(double s) => Math.Round(s, MidpointRounding.AwayFromZero);
    public static int RoundToInt(float s) => (int)MathF.Round(s, MidpointRounding.AwayFromZero);
    public static int RoundToInt(double s) => (int)Math.Round(s, MidpointRounding.AwayFromZero);

    public static int Clamp(int value, int min, int max) => value < min ? min : value > max ? max : value;
    public static float Clamp(float value, float min, float max) => value < min ? min : value > max ? max : value;
    public static double Clamp(double value, double min, double max) => value < min ? min : value > max ? max : value;

    public static int Max(int a, int b) => a > b ? a : b;
    public static float Max(float a, float b) => a > b ? a : b;
    public static double Max(double a, double b) => a > b ? a : b;
    public static int Min(int a, int b) => a < b ? a : b;
    public static float Min(float a, float b) => a < b ? a : b;
    public static double Min(double a, double b) => a < b ? a : b;

    public static float DegToRad(float deg) => deg * (Pi / 180f);
    public static double DegToRad(double deg) => deg * (Math.PI / 180.0);
    public static float RadToDeg(float rad) => rad * (180f / Pi);
    public static double RadToDeg(double rad) => rad * (180.0 / Math.PI);

    public static float Lerp(float from, float to, float weight) => from + (to - from) * weight;
    public static double Lerp(double from, double to, double weight) => from + (to - from) * weight;

    /// <summary>Interpolates between two angles (radians) along the shortest arc.</summary>
    public static float LerpAngle(float from, float to, float weight) => from + AngleDifference(from, to) * weight;
    public static double LerpAngle(double from, double to, double weight) => from + AngleDifference(from, to) * weight;

    /// <summary>The signed difference from <paramref name="from"/> to <paramref name="to"/>, in (-Pi, Pi].</summary>
    public static float AngleDifference(float from, float to)
    {
        float diff = (to - from) % Tau;
        return (2f * diff) % Tau - diff;
    }

    public static double AngleDifference(double from, double to)
    {
        double diff = (to - from) % (Math.PI * 2);
        return (2 * diff) % (Math.PI * 2) - diff;
    }

    public static float InverseLerp(float from, float to, float weight) => (weight - from) / (to - from);
    public static double InverseLerp(double from, double to, double weight) => (weight - from) / (to - from);

    public static float Remap(float value, float inFrom, float inTo, float outFrom, float outTo) =>
        Lerp(outFrom, outTo, InverseLerp(inFrom, inTo, value));
    public static double Remap(double value, double inFrom, double inTo, double outFrom, double outTo) =>
        Lerp(outFrom, outTo, InverseLerp(inFrom, inTo, value));

    public static bool IsEqualApprox(float a, float b)
    {
        if (a == b) return true;
        float tolerance = Epsilon * Math.Abs(a);
        if (tolerance < Epsilon) tolerance = Epsilon;
        return Math.Abs(a - b) < tolerance;
    }

    public static bool IsEqualApprox(double a, double b)
    {
        if (a == b) return true;
        double tolerance = Epsilon * Math.Abs(a);
        if (tolerance < Epsilon) tolerance = Epsilon;
        return Math.Abs(a - b) < tolerance;
    }

    public static bool IsZeroApprox(float s) => Math.Abs(s) < Epsilon;
    public static bool IsZeroApprox(double s) => Math.Abs(s) < Epsilon;
    public static bool IsFinite(float s) => float.IsFinite(s);
    public static bool IsFinite(double s) => double.IsFinite(s);
    public static bool IsInf(float s) => float.IsInfinity(s);
    public static bool IsInf(double s) => double.IsInfinity(s);
    public static bool IsNaN(float s) => float.IsNaN(s);
    public static bool IsNaN(double s) => double.IsNaN(s);

    /// <summary>Moves <paramref name="from"/> toward <paramref name="to"/> by at most <paramref name="delta"/>.</summary>
    public static float MoveToward(float from, float to, float delta) =>
        Math.Abs(to - from) <= delta ? to : from + Math.Sign(to - from) * delta;
    public static double MoveToward(double from, double to, double delta) =>
        Math.Abs(to - from) <= delta ? to : from + Math.Sign(to - from) * delta;

    /// <summary>Modulo whose result takes the sign of the divisor (never negative for a positive divisor).</summary>
    public static int PosMod(int a, int b)
    {
        int r = a % b;
        if (r < 0 && b > 0 || r > 0 && b < 0) r += b;
        return r;
    }

    public static float PosMod(float a, float b)
    {
        float r = a % b;
        if (r < 0 && b > 0 || r > 0 && b < 0) r += b;
        return r;
    }

    public static double PosMod(double a, double b)
    {
        double r = a % b;
        if (r < 0 && b > 0 || r > 0 && b < 0) r += b;
        return r;
    }

    /// <summary>-1, 0 or 1; zero for zero (Godot's <c>sign</c>), and for NaN.</summary>
    public static int Sign(int s) => s == 0 ? 0 : s < 0 ? -1 : 1;
    public static int Sign(float s) => s == 0f ? 0 : s < 0f ? -1 : s > 0f ? 1 : 0;
    public static int Sign(double s) => s == 0.0 ? 0 : s < 0.0 ? -1 : s > 0.0 ? 1 : 0;

    public static float SmoothStep(float from, float to, float x)
    {
        if (IsEqualApprox(from, to)) return from;
        x = Clamp((x - from) / (to - from), 0f, 1f);
        return x * x * (3f - 2f * x);
    }

    public static double SmoothStep(double from, double to, double x)
    {
        if (IsEqualApprox(from, to)) return from;
        x = Clamp((x - from) / (to - from), 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    /// <summary>Wraps <paramref name="value"/> into [min, max).</summary>
    public static int Wrap(int value, int min, int max)
    {
        int range = max - min;
        if (range == 0) return min;
        return min + ((value - min) % range + range) % range;
    }

    public static float Wrap(float value, float min, float max)
    {
        float range = max - min;
        if (IsZeroApprox(range)) return min;
        float result = value - range * MathF.Floor((value - min) / range);
        if (IsEqualApprox(result, max)) return min;
        return result;
    }

    public static double Wrap(double value, double min, double max)
    {
        double range = max - min;
        if (IsZeroApprox(range)) return min;
        double result = value - range * Math.Floor((value - min) / range);
        if (IsEqualApprox(result, max)) return min;
        return result;
    }

    /// <summary>Rounds to the nearest multiple of <paramref name="step"/>.</summary>
    public static float Snapped(float x, float step) => step != 0f ? MathF.Floor(x / step + 0.5f) * step : x;
    public static double Snapped(double x, double step) => step != 0.0 ? Math.Floor(x / step + 0.5) * step : x;
    public static int Snapped(int x, int step) => step != 0 ? (int)Math.Floor((double)x / step + 0.5) * step : x;

    public static float CubicInterpolate(float from, float to, float pre, float post, float weight)
    {
        float t2 = weight * weight, t3 = t2 * weight;
        return 0.5f * (from * 2f + (-pre + to) * weight + (2f * pre - 5f * from + 4f * to - post) * t2 + (-pre + 3f * from - 3f * to + post) * t3);
    }

    public static double CubicInterpolate(double from, double to, double pre, double post, double weight)
    {
        double t2 = weight * weight, t3 = t2 * weight;
        return 0.5 * (from * 2 + (-pre + to) * weight + (2 * pre - 5 * from + 4 * to - post) * t2 + (-pre + 3 * from - 3 * to + post) * t3);
    }

    public static float BezierInterpolate(float start, float control1, float control2, float end, float t)
    {
        float omt = 1f - t, omt2 = omt * omt, omt3 = omt2 * omt, t2 = t * t, t3 = t2 * t;
        return start * omt3 + control1 * omt2 * t * 3f + control2 * omt * t2 * 3f + end * t3;
    }

    public static double BezierInterpolate(double start, double control1, double control2, double end, double t)
    {
        double omt = 1 - t, omt2 = omt * omt, omt3 = omt2 * omt, t2 = t * t, t3 = t2 * t;
        return start * omt3 + control1 * omt2 * t * 3 + control2 * omt * t2 * 3 + end * t3;
    }

    /// <summary>Godot's <c>ease</c>: curve 1 is linear, above 1 ease-in, below 1 ease-out, negative in-out.</summary>
    public static float Ease(float s, float curve)
    {
        if (s < 0f) s = 0f; else if (s > 1f) s = 1f;
        if (curve > 0f)
        {
            if (curve < 1f) return 1f - MathF.Pow(1f - s, 1f / curve);
            return MathF.Pow(s, curve);
        }
        if (curve < 0f)
        {
            if (s < 0.5f) return MathF.Pow(s * 2f, -curve) * 0.5f;
            return (1f - MathF.Pow(1f - (s - 0.5f) * 2f, -curve)) * 0.5f + 0.5f;
        }
        return 0f;
    }

    public static double Ease(double s, double curve) => Ease((float)s, (float)curve);

    public static int NearestPo2(int value)
    {
        if (value <= 0) return 0;
        value--;
        value |= value >> 1; value |= value >> 2; value |= value >> 4; value |= value >> 8; value |= value >> 16;
        return value + 1;
    }

    /// <summary>The number of decimals needed to write a step size exactly (at most 10).</summary>
    public static int StepDecimals(double step)
    {
        double[] sd = { 0.9999, 0.09999, 0.009999, 0.0009999, 0.00009999, 0.000009999, 0.0000009999, 0.00000009999, 0.000000009999 };
        double abs = Math.Abs(step);
        double decs = abs - (int)abs;
        for (int i = 0; i < sd.Length; i++) if (decs > sd[i]) return i;
        return 0;
    }
}
