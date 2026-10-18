// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The GameFlow rows of the node table (clock, pause, quit, timer control, project settings): thin wrappers over
// Game, Timers and Settings (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2). A failure returns false/0 and logs
// once, never throws, so a host without the native service (a bare test process) leaves a graph running.

using Aver.Scripting;

namespace Aver.Framework;

/// <summary>Logs a graph-node failure once per call site and lets the wrapper return its neutral value.</summary>
internal static class FlowGuard
{
    private static readonly HashSet<string> s_logged = new(StringComparer.Ordinal);

    /// <summary>Warns the first time <paramref name="what"/> fails; later failures of the same wrapper are silent.</summary>
    internal static void Failed(string what, Exception e)
    {
        if (s_logged.Add(what)) Log.Warn($"[Graph] {what} failed: {e.GetType().Name}: {e.Message}");
    }

    /// <summary>Warns once for a refused argument.</summary>
    internal static void Refused(string what, string reason)
    {
        if (s_logged.Add(what + ":" + reason)) Log.Warn($"[Graph] {what}: {reason}");
    }
}

/// <summary>The game-flow node wrappers. Times are floats on the graph's pins, so a session older than a few hours
/// loses sub-millisecond precision in <see cref="GetTimeForGraph"/>; use the C# <see cref="Timers.Time"/> double for that.</summary>
internal static class GraphGameFlow
{
    // ---- clock and pause -------------------------------------------------------------------------------

    /// <summary>Game time in seconds: dilated and pause-respecting.</summary>
    internal static float GetTimeForGraph()
    {
        try { return (float)Timers.Time; }
        catch (Exception e) { FlowGuard.Failed("GetTime", e); return 0f; }
    }

    /// <summary>Real (unscaled) time in seconds.</summary>
    internal static float GetRealTimeForGraph()
    {
        try { return (float)Timers.RealTime; }
        catch (Exception e) { FlowGuard.Failed("GetRealTime", e); return 0f; }
    }

    /// <summary>The game-time scale, 0..100; 1 when unavailable.</summary>
    internal static float GetTimeDilationForGraph()
    {
        try { return Timers.TimeDilation; }
        catch (Exception e) { FlowGuard.Failed("GetTimeDilation", e); return 1f; }
    }

    /// <summary>Sets the game-time scale (clamped to 0..100 by the timer service). False for a NaN.</summary>
    internal static bool SetTimeDilationForGraph(float value)
    {
        if (float.IsNaN(value)) { FlowGuard.Refused("SetTimeDilation", "value is NaN"); return false; }
        try { Timers.TimeDilation = value; return true; }
        catch (Exception e) { FlowGuard.Failed("SetTimeDilation", e); return false; }
    }

    /// <summary>True while the play session is paused.</summary>
    internal static bool IsGamePausedForGraph()
    {
        try { return Game.IsPaused; }
        catch (Exception e) { FlowGuard.Failed("IsGamePaused", e); return false; }
    }

    /// <summary>Freezes or resumes the play session. False when no session is playing.</summary>
    internal static bool SetGamePausedForGraph(bool paused)
    {
        try { return Game.SetPaused(paused); }
        catch (Exception e) { FlowGuard.Failed("SetGamePaused", e); return false; }
    }

    /// <summary>Requests that the host exit at the next frame boundary; the rest of the frame still runs.</summary>
    internal static void QuitGameForGraph()
    {
        try { Game.Quit(); }
        catch (Exception e) { FlowGuard.Failed("QuitGame", e); }
    }

    // ---- timers ------------------------------------------------------------------------------------------

    /// <summary>Seconds until the timer's next fire; negative for an invalid handle.</summary>
    internal static float TimerRemainingForGraph(int handle)
    {
        try { return Timers.Remaining(new TimerHandle(handle)); }
        catch (Exception e) { FlowGuard.Failed("TimerRemaining", e); return -1f; }
    }

    /// <summary>True while the timer is scheduled.</summary>
    internal static bool IsTimerActiveForGraph(int handle)
    {
        try { return Timers.IsActive(new TimerHandle(handle)); }
        catch (Exception e) { FlowGuard.Failed("IsTimerActive", e); return false; }
    }

    /// <summary>Pauses or resumes one timer. False for a handle that is not active.</summary>
    internal static bool PauseTimerForGraph(int handle, bool paused)
    {
        try { return Timers.SetPaused(new TimerHandle(handle), paused); }
        catch (Exception e) { FlowGuard.Failed("PauseTimer", e); return false; }
    }

    /// <summary>Restarts a timer's countdown; a delay of zero or more also becomes its interval, a negative one keeps it.</summary>
    internal static bool ResetTimerForGraph(int handle, float delay)
    {
        try { return Timers.Reset(new TimerHandle(handle), delay); }
        catch (Exception e) { FlowGuard.Failed("ResetTimer", e); return false; }
    }

    /// <summary>How many times the timer has fired; -1 for an invalid handle.</summary>
    internal static int GetTimerFireCountForGraph(int handle)
    {
        try { return Timers.FireCount(new TimerHandle(handle)); }
        catch (Exception e) { FlowGuard.Failed("GetTimerFireCount", e); return -1; }
    }

    // ---- project settings ----------------------------------------------------------------------------------

    /// <summary>The stored float for a key, or the fallback when it is unset, unparsable or the store is closed.</summary>
    internal static float GetSettingFloatForGraph(string key, float fallback)
    {
        try { return Settings.GetFloat(key, fallback); }
        catch (Exception e) { FlowGuard.Failed("GetSettingFloat", e); return fallback; }
    }

    /// <summary>The stored int for a key, or the fallback when it is unset, unparsable or the store is closed.</summary>
    internal static int GetSettingIntForGraph(string key, int fallback)
    {
        try { return Settings.GetInt(key, fallback); }
        catch (Exception e) { FlowGuard.Failed("GetSettingInt", e); return fallback; }
    }

    /// <summary>The stored bool for a key, or the fallback when it is unset, unparsable or the store is closed.</summary>
    internal static bool GetSettingBoolForGraph(string key, bool fallback)
    {
        try { return Settings.GetBool(key, fallback); }
        catch (Exception e) { FlowGuard.Failed("GetSettingBool", e); return fallback; }
    }

    /// <summary>True when the key is present, whatever its stored type.</summary>
    internal static bool HasSettingForGraph(string key)
    {
        try { return Settings.Has(key); }
        catch (Exception e) { FlowGuard.Failed("HasSetting", e); return false; }
    }

    /// <summary>Stores a float under a key. False for an empty key or a closed store.</summary>
    internal static bool SetSettingFloatForGraph(string key, float value)
    {
        try { return Settings.SetFloat(key, value); }
        catch (Exception e) { FlowGuard.Failed("SetSettingFloat", e); return false; }
    }

    /// <summary>Stores an int under a key. False for an empty key or a closed store.</summary>
    internal static bool SetSettingIntForGraph(string key, int value)
    {
        try { return Settings.SetInt(key, value); }
        catch (Exception e) { FlowGuard.Failed("SetSettingInt", e); return false; }
    }

    /// <summary>Stores a bool under a key. False for an empty key or a closed store.</summary>
    internal static bool SetSettingBoolForGraph(string key, bool value)
    {
        try { return Settings.SetBool(key, value); }
        catch (Exception e) { FlowGuard.Failed("SetSettingBool", e); return false; }
    }

    /// <summary>Writes the settings store to disk if anything changed. False when the write was attempted and failed.</summary>
    internal static bool FlushSettingsForGraph()
    {
        try { return Settings.Flush(); }
        catch (Exception e) { FlowGuard.Failed("FlushSettings", e); return false; }
    }
}
