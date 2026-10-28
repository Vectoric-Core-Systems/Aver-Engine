// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The easing curves Tween uses: Robert Penner's published equations (sine, quint, quart, quad, expo, elastic, cubic, circ,
// bounce, back), written from their definitions as f(t, b, c, d): t elapsed, b begin, c change, d duration. Godot adds a
// "spring" curve to the set; it is written from its documented behaviour (a damped overshoot that settles at the end value).

namespace Aver.Fabric;

internal static class Easing
{
    // The curve's value at t of d, from b by c. In = starts slow, Out = ends slow, InOut and OutIn mirror them in the halves.
    internal static double Run(Tween.TransitionType trans, Tween.EaseType ease, double t, double b, double c, double d)
    {
        if (d <= 0) return b + c;
        switch (ease)
        {
            case Tween.EaseType.In: return In(trans, t, b, c, d);
            case Tween.EaseType.Out: return Out(trans, t, b, c, d);
            case Tween.EaseType.InOut: return InOut(trans, t, b, c, d);
            default:
                // OutIn: the out curve for the first half, the in curve for the second.
                return t < d / 2
                    ? Out(trans, t * 2, b, c / 2, d)
                    : In(trans, t * 2 - d, b + c / 2, c / 2, d);
        }
    }

    private static double In(Tween.TransitionType trans, double t, double b, double c, double d)
    {
        const double Pi = Math.PI;
        switch (trans)
        {
            case Tween.TransitionType.Sine: return -c * Math.Cos(t / d * (Pi / 2)) + c + b;
            case Tween.TransitionType.Quint: { t /= d; return c * t * t * t * t * t + b; }
            case Tween.TransitionType.Quart: { t /= d; return c * t * t * t * t + b; }
            case Tween.TransitionType.Quad: { t /= d; return c * t * t + b; }
            case Tween.TransitionType.Expo: return t == 0 ? b : c * Math.Pow(2, 10 * (t / d - 1)) + b;
            case Tween.TransitionType.Elastic:
            {
                if (t == 0) return b;
                if ((t /= d) == 1) return b + c;
                double p = d * 0.3, a = c, s = p / 4;
                double post = a * Math.Pow(2, 10 * (t -= 1));
                return -(post * Math.Sin((t * d - s) * (2 * Pi) / p)) + b;
            }
            case Tween.TransitionType.Cubic: { t /= d; return c * t * t * t + b; }
            case Tween.TransitionType.Circ: { t /= d; return -c * (Math.Sqrt(1 - t * t) - 1) + b; }
            case Tween.TransitionType.Bounce: return c - BounceOut(d - t, 0, c, d) + b;
            case Tween.TransitionType.Back:
            {
                const double s = 1.70158;
                t /= d;
                return c * t * t * ((s + 1) * t - s) + b;
            }
            case Tween.TransitionType.Spring: return c - SpringOut(d - t, 0, c, d) + b;
            default: return c * t / d + b;   // Linear
        }
    }

    private static double Out(Tween.TransitionType trans, double t, double b, double c, double d)
    {
        const double Pi = Math.PI;
        switch (trans)
        {
            case Tween.TransitionType.Sine: return c * Math.Sin(t / d * (Pi / 2)) + b;
            case Tween.TransitionType.Quint: { t = t / d - 1; return c * (t * t * t * t * t + 1) + b; }
            case Tween.TransitionType.Quart: { t = t / d - 1; return -c * (t * t * t * t - 1) + b; }
            case Tween.TransitionType.Quad: { t /= d; return -c * t * (t - 2) + b; }
            case Tween.TransitionType.Expo: return t == d ? b + c : c * (-Math.Pow(2, -10 * t / d) + 1) + b;
            case Tween.TransitionType.Elastic:
            {
                if (t == 0) return b;
                if ((t /= d) == 1) return b + c;
                double p = d * 0.3, a = c, s = p / 4;
                return a * Math.Pow(2, -10 * t) * Math.Sin((t * d - s) * (2 * Pi) / p) + c + b;
            }
            case Tween.TransitionType.Cubic: { t = t / d - 1; return c * (t * t * t + 1) + b; }
            case Tween.TransitionType.Circ: { t = t / d - 1; return c * Math.Sqrt(1 - t * t) + b; }
            case Tween.TransitionType.Bounce: return BounceOut(t, b, c, d);
            case Tween.TransitionType.Back:
            {
                const double s = 1.70158;
                t = t / d - 1;
                return c * (t * t * ((s + 1) * t + s) + 1) + b;
            }
            case Tween.TransitionType.Spring: return SpringOut(t, b, c, d);
            default: return c * t / d + b;
        }
    }

    private static double InOut(Tween.TransitionType trans, double t, double b, double c, double d)
    {
        const double Pi = Math.PI;
        switch (trans)
        {
            case Tween.TransitionType.Sine: return -c / 2 * (Math.Cos(Pi * t / d) - 1) + b;
            case Tween.TransitionType.Quint:
            {
                t /= d / 2;
                if (t < 1) return c / 2 * t * t * t * t * t + b;
                t -= 2;
                return c / 2 * (t * t * t * t * t + 2) + b;
            }
            case Tween.TransitionType.Quart:
            {
                t /= d / 2;
                if (t < 1) return c / 2 * t * t * t * t + b;
                t -= 2;
                return -c / 2 * (t * t * t * t - 2) + b;
            }
            case Tween.TransitionType.Quad:
            {
                t /= d / 2;
                if (t < 1) return c / 2 * t * t + b;
                --t;
                return -c / 2 * (t * (t - 2) - 1) + b;
            }
            case Tween.TransitionType.Expo:
            {
                if (t == 0) return b;
                if (t == d) return b + c;
                t /= d / 2;
                if (t < 1) return c / 2 * Math.Pow(2, 10 * (t - 1)) + b;
                return c / 2 * (-Math.Pow(2, -10 * (t - 1)) + 2) + b;
            }
            case Tween.TransitionType.Elastic:
            {
                if (t == 0) return b;
                if ((t /= d / 2) == 2) return b + c;
                double p = d * (0.3 * 1.5), a = c, s = p / 4;
                if (t < 1)
                {
                    double post = a * Math.Pow(2, 10 * (t -= 1));
                    return -0.5 * (post * Math.Sin((t * d - s) * (2 * Pi) / p)) + b;
                }
                double post2 = a * Math.Pow(2, -10 * (t -= 1));
                return post2 * Math.Sin((t * d - s) * (2 * Pi) / p) * 0.5 + c + b;
            }
            case Tween.TransitionType.Cubic:
            {
                t /= d / 2;
                if (t < 1) return c / 2 * t * t * t + b;
                t -= 2;
                return c / 2 * (t * t * t + 2) + b;
            }
            case Tween.TransitionType.Circ:
            {
                t /= d / 2;
                if (t < 1) return -c / 2 * (Math.Sqrt(1 - t * t) - 1) + b;
                t -= 2;
                return c / 2 * (Math.Sqrt(1 - t * t) + 1) + b;
            }
            case Tween.TransitionType.Bounce:
                return t < d / 2 ? (c - BounceOut(d - t * 2, 0, c, d)) * 0.5 + b : BounceOut(t * 2 - d, 0, c, d) * 0.5 + c * 0.5 + b;
            case Tween.TransitionType.Back:
            {
                double s = 1.70158 * 1.525;
                t /= d / 2;
                if (t < 1) return c / 2 * (t * t * ((s + 1) * t - s)) + b;
                t -= 2;
                return c / 2 * (t * t * ((s + 1) * t + s) + 2) + b;
            }
            case Tween.TransitionType.Spring:
                return t < d / 2 ? (c - SpringOut(d - t * 2, 0, c, d)) * 0.5 + b : SpringOut(t * 2 - d, 0, c, d) * 0.5 + c * 0.5 + b;
            default: return c * t / d + b;
        }
    }

    private static double BounceOut(double t, double b, double c, double d)
    {
        if ((t /= d) < 1 / 2.75) return c * (7.5625 * t * t) + b;
        if (t < 2 / 2.75) { t -= 1.5 / 2.75; return c * (7.5625 * t * t + 0.75) + b; }
        if (t < 2.5 / 2.75) { t -= 2.25 / 2.75; return c * (7.5625 * t * t + 0.9375) + b; }
        t -= 2.625 / 2.75;
        return c * (7.5625 * t * t + 0.984375) + b;
    }

    // Overshoots the end, rings and settles on it.
    private static double SpringOut(double t, double b, double c, double d)
    {
        t /= d;
        double s = 1.0 - t;
        t = (Math.Sin(t * Math.PI * (0.2 + 2.5 * t * t * t)) * Math.Pow(s, 2.2) + t) * (1.0 + 1.2 * s);
        return c * t + b;
    }
}
