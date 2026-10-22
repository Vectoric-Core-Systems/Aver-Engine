// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The physics world's statistics (aver_phys_stats): body counts, the fixed step's wall time, and the update
// errors Jolt reports when a limit is too small. A snapshot, cheap enough to read every frame; all zero with no
// world.

using System;
using System.Collections.Generic;
using System.Text;

namespace Aver.Physics;

/// <summary>The slots of the native statistics array (AVER_PHYS_ST_*).</summary>
public enum PhysicsStat
{
    Bodies = 0,
    MaxBodies = 1,
    Static = 2,
    Dynamic = 3,
    ActiveDynamic = 4,
    Kinematic = 5,
    ActiveKinematic = 6,
    Soft = 7,
    ActiveSoft = 8,
    Constraints = 9,
    Characters = 10,
    Vehicles = 11,
    StepsLastCall = 12,
    StepsTotal = 13,
    StepMsLast = 14,
    StepMsAvg = 15,
    StepMsMax = 16,
    CallMsLast = 17,
    ContactsLastCall = 18,
    UpdateErrors = 19,
    UpdateErrorSteps = 20,
    TempPeakBytes = 21,
    TempSizeBytes = 22,
    WorkerThreads = 23,
    AccumulatorS = 24,
    TopologyEpoch = 25,
    /// <summary>How many statistics there are (AVER_PHYS_ST_COUNT); not a statistic.</summary>
    Count = 26,
}

/// <summary>What Jolt could not hold during a step: contacts were dropped. Raise the matching limit
/// (<see cref="PhysicsConfigKey.MaxContacts"/> or <see cref="PhysicsConfigKey.MaxBodyPairs"/>) and rebuild.</summary>
[Flags]
public enum PhysicsUpdateErrors
{
    None = 0,
    ManifoldCacheFull = 1,
    BodyPairCacheFull = 2,
    ContactConstraintsFull = 4,
}

/// <summary>A snapshot of the world's statistics.</summary>
public readonly struct PhysicsStats
{
    private readonly double[] _v;

    internal PhysicsStats(double[] values) { _v = values; }

    /// <summary>The raw value of one slot (0 for an unknown slot).</summary>
    public double this[PhysicsStat stat] => _v is not null && (int)stat >= 0 && (int)stat < _v.Length ? _v[(int)stat] : 0.0;

    public int Bodies => (int)this[PhysicsStat.Bodies];
    public int MaxBodies => (int)this[PhysicsStat.MaxBodies];
    public int StaticBodies => (int)this[PhysicsStat.Static];
    public int DynamicBodies => (int)this[PhysicsStat.Dynamic];
    public int ActiveDynamicBodies => (int)this[PhysicsStat.ActiveDynamic];
    public int KinematicBodies => (int)this[PhysicsStat.Kinematic];
    public int ActiveKinematicBodies => (int)this[PhysicsStat.ActiveKinematic];
    public int SoftBodies => (int)this[PhysicsStat.Soft];
    public int ActiveSoftBodies => (int)this[PhysicsStat.ActiveSoft];
    public int Constraints => (int)this[PhysicsStat.Constraints];
    public int Characters => (int)this[PhysicsStat.Characters];
    public int Vehicles => (int)this[PhysicsStat.Vehicles];
    /// <summary>Fixed steps the last <c>Physics.Step</c> call ran.</summary>
    public int StepsLastCall => (int)this[PhysicsStat.StepsLastCall];
    /// <summary>Fixed steps since the world was created.</summary>
    public long StepsTotal => (long)this[PhysicsStat.StepsTotal];
    /// <summary>Wall-clock milliseconds of the last solver update.</summary>
    public double StepMsLast => this[PhysicsStat.StepMsLast];
    /// <summary>Smoothed (half-second) average of the solver update.</summary>
    public double StepMsAvg => this[PhysicsStat.StepMsAvg];
    /// <summary>Slowest solver update since <see cref="Physics.ResetStatsWindow"/>.</summary>
    public double StepMsMax => this[PhysicsStat.StepMsMax];
    /// <summary>Wall-clock milliseconds of the whole last <c>Physics.Step</c> call.</summary>
    public double CallMsLast => this[PhysicsStat.CallMsLast];
    public int ContactsLastCall => (int)this[PhysicsStat.ContactsLastCall];
    /// <summary>Contacts Jolt dropped since the window reset; <see cref="PhysicsUpdateErrors.None"/> is healthy.</summary>
    public PhysicsUpdateErrors UpdateErrors => (PhysicsUpdateErrors)(int)this[PhysicsStat.UpdateErrors];
    public long UpdateErrorSteps => (long)this[PhysicsStat.UpdateErrorSteps];
    public long TempPeakBytes => (long)this[PhysicsStat.TempPeakBytes];
    public long TempSizeBytes => (long)this[PhysicsStat.TempSizeBytes];
    /// <summary>Solver worker threads; 0 when single-threaded.</summary>
    public int WorkerThreads => (int)this[PhysicsStat.WorkerThreads];
    /// <summary>Seconds of real time carried toward the next fixed step.</summary>
    public double AccumulatorSeconds => this[PhysicsStat.AccumulatorS];
    public uint TopologyEpoch => (uint)this[PhysicsStat.TopologyEpoch];
}

/// <summary>One scope of the profiler table: a Jolt stage or one of the engine's fixed-step stages. Rows come depth first.</summary>
/// <param name="Name">The scope's name ("PhysicsStep", "Update", "FindCollisions", ...).</param>
/// <param name="InclusiveMs">Milliseconds per fixed step, averaged over the last published window, children included.</param>
/// <param name="ExclusiveMs">Milliseconds per fixed step spent in the scope itself.</param>
/// <param name="MaxMs">The largest per-step inclusive time of any call in the window.</param>
/// <param name="CallsPerStep">How often the scope ran per fixed step.</param>
/// <param name="Parent">Index of the parent row, -1 for a root.</param>
/// <param name="Depth">Nesting depth, 0 for a root.</param>
public readonly record struct ProfileRow(string Name, double InclusiveMs, double ExclusiveMs, double MaxMs, double CallsPerStep, int Parent, int Depth);

public static partial class Physics
{
    /// <summary>Turns the profiler on or off. False when the native library was built without the hook
    /// (<see cref="PhysicsBuildFlags.Profiler"/>). While off a profile scope costs one branch; the table freezes when disabled.</summary>
    public static bool ProfileEnable(bool on) => Native.aver_phys_profile_enable(on ? 1 : 0) != 0;

    /// <summary>True while the profiler is collecting.</summary>
    public static bool ProfileEnabled => Native.aver_phys_profile_is_enabled() != 0;

    /// <summary>Drops the profile table and restarts its window.</summary>
    public static void ProfileReset() => Native.aver_phys_profile_reset();

    /// <summary>The last published profile table, depth first. Empty until the profiler has run for about five fixed steps.
    /// Jolt's job scopes appear under one "Worker jobs" row whose times are CPU time summed over the worker threads.</summary>
    public static IReadOnlyList<ProfileRow> ProfileRows()
    {
        int n = Native.aver_phys_profile_count();
        var rows = new List<ProfileRow>(n > 0 ? n : 0);
        var name = new byte[128];
        var v = new double[4];
        var pd = new int[2];
        for (int i = 0; i < n; i++)
        {
            if (Native.aver_phys_profile_get(i, name, name.Length, v, pd) == 0) break;
            int len = Array.IndexOf(name, (byte)0);
            rows.Add(new ProfileRow(Encoding.UTF8.GetString(name, 0, len < 0 ? name.Length : len), v[0], v[1], v[2], v[3], pd[0], pd[1]));
        }
        return rows;
    }

    /// <summary>The three largest scopes of the last window as one line ("Name 1.20 ms | ..."), empty without a table.</summary>
    public static string ProfileSummary()
    {
        int need = Native.aver_phys_profile_summary(null, 0);
        if (need <= 1) return string.Empty;
        var buf = new byte[need];
        Native.aver_phys_profile_summary(buf, buf.Length);
        return Encoding.UTF8.GetString(buf, 0, need - 1);
    }

    /// <summary>The world's statistics now. All zero with no world.</summary>
    public static PhysicsStats GetStats()
    {
        var v = new double[(int)PhysicsStat.Count];
        Native.aver_phys_stats(v, v.Length);
        return new PhysicsStats(v);
    }

    /// <summary>Clears the maximum step time, the temp-memory peak and the update-error fields.</summary>
    public static bool ResetStatsWindow() => Native.aver_phys_stats_reset_window() != 0;

    /// <summary>The diagnostics preset's reports (AVER_PHYSICS_DIAGNOSTICS builds only): 1 broad-phase, 2 per-body simulation,
    /// 3 narrow-phase (to the engine log), 4 Jolt's internal profiler as HTML in the working directory. False in any other build.</summary>
    public static bool DiagReport(int kind) => Native.aver_phys_diag_report(kind) != 0;

    /// <summary>Where Jolt writes its determinism log, detlog.txt (diagnostics preset only; null for the working directory). Applies to
    /// the next world created. False in any other build.</summary>
    public static bool DiagLogDir(string? directory) => Native.aver_phys_diag_log_dir(directory) != 0;
}
