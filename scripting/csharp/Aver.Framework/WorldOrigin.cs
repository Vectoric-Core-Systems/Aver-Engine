// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The floating origin as scripts see it (framework_origin_abi.h; docs/plans/wave11/LARGE_WORLD.md 2.2, 2.5).
//
// Every position the engine hands a script is origin-relative: when the player has flown a region (16.384 km) from the origin the host moves
// the origin and every position changes by the same amount in one step, at the top of a frame. A field that outlives a frame must therefore
// hold an absolute (Entity.AbsolutePosition, a WorldPos), never a Position. Nothing here throws: with no host (a bare test process, the native
// library absent) the origin is (0, 0), disabled, generation 0, and the conversions are plain arithmetic.

using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework;

/// <summary>The floating origin: where it is, how to convert to and from absolute positions, and a callback for when it moves.</summary>
public static class WorldOrigin
{
    /// <summary>The distance the origin moves in, centimetres: one region (1024 chunks of 16 m = 16.384 km).</summary>
    public const long StepCm = WorldPos.RegionSizeCm;

    private static readonly HashSet<string> s_logged = new();

    private static void LogOnce(string key, string message)
    {
        if (s_logged.Add(key)) Log.Warn(message);
    }

    // No native library, or no entry point in it (an older build): the origin reads as absent.
    private static bool IsMissingNative(Exception e) => e is DllNotFoundException or EntryPointNotFoundException;

    /// <summary>The origin O in centimetres: always a whole number of regions, on X and Y (Z is never shifted). (0, 0) with no host.</summary>
    public static (long X, long Y) Origin
    {
        get
        {
            try
            {
                var v = new long[2];
                return FwOrigin.aver_fw_origin_get(v) != 0 ? (v[0], v[1]) : (0, 0);
            }
            catch (Exception e) when (IsMissingNative(e))
            {
                return (0, 0);
            }
        }
    }

    /// <summary>The region the origin is in, as whole regions along X and Y: <see cref="Origin"/> divided by <see cref="StepCm"/>.</summary>
    public static (int X, int Y) Region
    {
        get
        {
            (long x, long y) = Origin;
            return ((int)(x / StepCm), (int)(y / StepCm));
        }
    }

    /// <summary>Shifts so far, counting level loads: 0 at start. A cache keyed on an origin-relative position is stale when this changes.</summary>
    public static uint Generation
    {
        get
        {
            try { return FwOrigin.aver_fw_origin_generation(); }
            catch (Exception e) when (IsMissingNative(e)) { return 0; }
        }
    }

    /// <summary>True when the origin can shift (the project did not turn it off, and this host supports it).</summary>
    public static bool Enabled
    {
        get
        {
            try { return FwOrigin.aver_fw_origin_enabled() != 0; }
            catch (Exception e) when (IsMissingNative(e)) { return false; }
        }
    }

    /// <summary>The absolute point (centimetres) an origin-relative position names: the add is in double, so it is exact.</summary>
    public static Double3 ToAbsolute(Vec3 local)
    {
        (long ox, long oy) = Origin;
        return new Double3((double)local.X + (double)ox, (double)local.Y + (double)oy, local.Z);
    }

    /// <summary>The origin-relative position of an absolute point: the subtraction is in double and the one rounding is the narrowing.</summary>
    public static Vec3 ToLocal(Double3 absoluteCm)
    {
        (long ox, long oy) = Origin;
        return new Vec3((float)(absoluteCm.X - (double)ox), (float)(absoluteCm.Y - (double)oy), (float)absoluteCm.Z);
    }

    /// <summary>Asks the host to shift the origin by whole regions at the next frame's start, as a camera-driven shift would. Refused (false)
    /// unless the project states <c>WORLD.ALLOWSHIFTREQUEST 1</c>; true means queued, not done. A shift the host defers (a level loading, an
    /// editor gesture) is dropped. For tests and tools: under the default follow-camera policy the next frame rebases back to the camera.</summary>
    public static bool RequestShift(int dxRegions, int dyRegions)
    {
        try { return FwOrigin.aver_fw_origin_request_shift(dxRegions, dyRegions) != 0; }
        catch (Exception e) when (IsMissingNative(e)) { return false; }
    }

    // ---- the change event ---------------------------------------------------------------------------------------------

    internal const string EventName = "world.origin_changed";

    private static Action<Double3, uint>? s_changed;
    private static EventSubscription s_subscription;

    /// <summary>Raised after every committed shift (at the next event flush, once the world is consistent) with the shift in centimetres:
    /// every origin-relative position changed by the NEGATIVE of this vector, and the new <see cref="Generation"/>. Z is always 0.
    /// This is the seam for dropping a cache of origin-relative values; absolute positions need nothing.</summary>
    public static event Action<Double3, uint>? Changed
    {
        add
        {
            s_changed += value;
            if (value == null) return;
            // Subscribed afresh on every add: Events.Shutdown (a script reload) clears every subscription without telling us, so a kept id could be a dead one.
            try
            {
                if (s_subscription.IsValid) Events.Unsubscribe(s_subscription);
                s_subscription = Events.Subscribe(EventName, OnOriginChanged);
            }
            catch (Exception e) when (IsMissingNative(e))
            {
                s_subscription = default;
                LogOnce("subscribe", "[WorldOrigin] the native event bus is not available; Changed will not be raised");
            }
        }
        remove
        {
            s_changed -= value;
            if (s_changed != null || !s_subscription.IsValid) return;
            try { Events.Unsubscribe(s_subscription); }
            catch (Exception e) when (IsMissingNative(e)) { }
            s_subscription = default;
        }
    }

    // Payload ints (framework_origin_abi.h): [0] dxCm, [1] dyCm, [2] generation, [3] originX, [4] originY.
    internal static void OnOriginChanged(EventData e)
    {
        Action<Double3, uint>? handlers = s_changed;
        if (handlers == null) return;
        var delta = new Double3(e.GetInt(0), e.GetInt(1), 0.0);
        uint generation = (uint)e.GetInt(2);
        foreach (Delegate d in handlers.GetInvocationList())
        {
            try { ((Action<Double3, uint>)d)(delta, generation); }
            catch (Exception ex) { Log.Error($"[WorldOrigin] a Changed handler threw: {ex.GetType().Name}: {ex.Message}"); }
        }
    }
}
