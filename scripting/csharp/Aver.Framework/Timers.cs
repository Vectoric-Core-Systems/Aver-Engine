// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Managed face of the framework timer service. See docs/GAME_TIMERS_EVENTS.md.

using Aver.Scripting;

namespace Aver.Framework;

/// <summary>A timer handle. <see cref="Value"/> 0 is invalid; a stale handle never matches a reused slot.</summary>
public readonly struct TimerHandle : IEquatable<TimerHandle>
{
    /// <summary>The raw ABI handle.</summary>
    public int Value { get; }
    /// <summary>Wraps a raw handle (for graph interop).</summary>
    public TimerHandle(int value) => Value = value;
    /// <summary>True for any non-zero handle; says nothing about whether the timer still runs.</summary>
    public bool IsValid => Value != 0;
    /// <summary>True while the timer is still scheduled.</summary>
    public bool IsActive => Timers.IsActive(this);
    /// <inheritdoc/>
    public bool Equals(TimerHandle other) => Value == other.Value;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is TimerHandle h && Equals(h);
    /// <inheritdoc/>
    public override int GetHashCode() => Value;
    /// <summary>Equality.</summary>
    public static bool operator ==(TimerHandle a, TimerHandle b) => a.Equals(b);
    /// <summary>Inequality.</summary>
    public static bool operator !=(TimerHandle a, TimerHandle b) => !a.Equals(b);
    /// <inheritdoc/>
    public override string ToString() => $"Timer#{Value}";
}

/// <summary>Per-timer clock behaviour.</summary>
[Flags]
public enum TimerFlags
{
    /// <summary>Normal: stops while paused, scaled by time dilation.</summary>
    None = 0,
    /// <summary>Keeps running while the timer service is paused.</summary>
    IgnorePause = 2,
    /// <summary>Runs on real time instead of dilated game time.</summary>
    IgnoreDilation = 4,
}

/// <summary>Frame-safe timers on a pausable, dilatable game clock. Single game thread only.</summary>
/// <remarks>Callbacks may cancel or create timers, including their own. A timer created inside a
/// callback starts counting at the next update, so a zero delay means "next frame". Delivery is
/// ordered by due moment within the frame, then creation order.</remarks>
public static class Timers
{
    private sealed class Record
    {
        public Action<TimerHandle> Callback = null!;
        public int Handle;
        public int Owner;
    }

    private static readonly Dictionary<long, Record> s_records = new();
    private static readonly Dictionary<int, long> s_byHandle = new();
    private static readonly TimerDispatchFn s_dispatch = OnDispatch;   // pinned for the process lifetime
    private static long s_nextToken = 1;
    private static int s_epoch;
    private static bool s_installed;

    private static void EnsureInstalled()
    {
        if (s_installed) return;
        FwTE.aver_fw_set_timer_dispatch(s_dispatch, IntPtr.Zero);
        s_epoch = FwTE.aver_fw_timers_epoch();
        s_installed = true;
    }

    private static void OnDispatch(int handle, long token, IntPtr user)
    {
        // Never let an exception cross the ABI.
        try
        {
            if (s_records.TryGetValue(token, out Record? rec))
                rec.Callback(new TimerHandle(handle));
        }
        catch (Exception ex)
        {
            Log.Error($"[Timers] callback threw: {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            if (FwTE.aver_fw_timer_active(handle) == 0)
                RemoveRecord(token);
        }
    }

    /// <summary>Schedules <paramref name="callback"/>. A repeating timer fires every <paramref name="delay"/>
    /// (first after <paramref name="firstDelay"/> when that is not negative), at most
    /// <paramref name="maxFires"/> times when that is positive. <paramref name="owner"/> is a
    /// <see cref="ClearOwner"/> key, typically an entity handle. Returns an invalid handle on a NaN
    /// delay or a full table.</summary>
    public static TimerHandle Set(float delay, Action<TimerHandle> callback, bool repeat = false,
                                  float firstDelay = -1f, int maxFires = 0,
                                  TimerFlags flags = TimerFlags.None, int owner = 0)
    {
        ArgumentNullException.ThrowIfNull(callback);
        EnsureInstalled();
        int epoch = FwTE.aver_fw_timers_epoch();
        if (epoch != s_epoch) { s_records.Clear(); s_byHandle.Clear(); s_epoch = epoch; }   // native bulk clear orphaned them

        long token = s_nextToken++;
        var rec = new Record { Callback = callback, Owner = owner };
        s_records[token] = rec;
        int native = (int)flags & 6;
        if (repeat) native |= 1;
        int h = FwTE.aver_fw_timer_set(delay, firstDelay, maxFires, native, owner, token);
        if (h == 0) s_records.Remove(token);
        else { rec.Handle = h; s_byHandle[h] = token; }
        return new TimerHandle(h);
    }

    /// <summary>Schedules a callback that does not need its own handle.</summary>
    public static TimerHandle Set(float delay, Action callback, bool repeat = false,
                                  float firstDelay = -1f, int maxFires = 0,
                                  TimerFlags flags = TimerFlags.None, int owner = 0)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return Set(delay, _ => callback(), repeat, firstDelay, maxFires, flags, owner);
    }

    /// <summary>Cancels a timer. Safe inside any callback. False if it was not active.</summary>
    public static bool Clear(TimerHandle h)
    {
        bool was = FwTE.aver_fw_timer_cancel(h.Value) != 0;
        if (was && s_byHandle.TryGetValue(h.Value, out long token)) RemoveRecord(token);
        return was;
    }

    /// <summary>Cancels every timer created with <paramref name="owner"/>. Owner 0 is a no-op. Returns the count.</summary>
    public static int ClearOwner(int owner)
    {
        int n = FwTE.aver_fw_timer_cancel_owner(owner);
        if (n > 0)
            foreach (long token in s_records.Where(kv => kv.Value.Owner == owner).Select(kv => kv.Key).ToList())
                RemoveRecord(token);
        return n;
    }

    /// <summary>Cancels every timer.</summary>
    public static void ClearAll()
    {
        FwTE.aver_fw_timers_clear();
        s_records.Clear();
        s_byHandle.Clear();
        s_epoch = FwTE.aver_fw_timers_epoch();
    }

    /// <summary>True while the timer is scheduled.</summary>
    public static bool IsActive(TimerHandle h) => FwTE.aver_fw_timer_active(h.Value) != 0;
    /// <summary>Seconds until the next fire; negative for an invalid handle.</summary>
    public static float Remaining(TimerHandle h) => FwTE.aver_fw_timer_remaining(h.Value);
    /// <summary>How many times the timer has fired; -1 for an invalid handle.</summary>
    public static int FireCount(TimerHandle h) => FwTE.aver_fw_timer_fire_count(h.Value);
    /// <summary>Pauses or resumes one timer.</summary>
    public static bool SetPaused(TimerHandle h, bool paused) => FwTE.aver_fw_timer_set_paused(h.Value, paused ? 1 : 0) != 0;
    /// <summary>True when this timer is individually paused.</summary>
    public static bool IsPaused(TimerHandle h) => FwTE.aver_fw_timer_is_paused(h.Value) != 0;
    /// <summary>Restarts the countdown; a non-negative <paramref name="delay"/> also becomes the new interval.</summary>
    public static bool Reset(TimerHandle h, float delay = -1f) => FwTE.aver_fw_timer_reset(h.Value, delay) != 0;

    /// <summary>Pauses every timer that does not carry <see cref="TimerFlags.IgnorePause"/>.</summary>
    public static bool Paused
    {
        get => FwTE.aver_fw_timers_paused() != 0;
        set => FwTE.aver_fw_timers_set_paused(value ? 1 : 0);
    }

    /// <summary>Game-time scale, clamped to [0, 100]. Affects every timer without <see cref="TimerFlags.IgnoreDilation"/>.</summary>
    public static float TimeDilation
    {
        get => FwTE.aver_fw_timers_dilation();
        set => FwTE.aver_fw_timers_set_dilation(value);
    }

    /// <summary>Dilated, pause-respecting game time in seconds.</summary>
    public static double Time => FwTE.aver_fw_timers_time_us() / 1.0e6;
    /// <summary>Unscaled time in seconds.</summary>
    public static double RealTime => FwTE.aver_fw_timers_real_time_us() / 1.0e6;
    /// <summary>Scheduled timer count.</summary>
    public static int Count => FwTE.aver_fw_timers_count();

    /// <summary>Host call: advances the clock by the UNSCALED frame delta. Dilation and pause are applied inside.</summary>
    public static void Update(float rawDt) => FwTE.aver_fw_timers_update(rawDt);

    /// <summary>Detaches from the native service. Call before the scripting load context is unloaded.</summary>
    public static void Shutdown()
    {
        if (!s_installed) return;
        FwTE.aver_fw_set_timer_dispatch(null, IntPtr.Zero);
        s_records.Clear();
        s_byHandle.Clear();
        s_installed = false;
    }

    private static void RemoveRecord(long token)
    {
        if (s_records.Remove(token, out Record? rec)) s_byHandle.Remove(rec.Handle);
    }
}
