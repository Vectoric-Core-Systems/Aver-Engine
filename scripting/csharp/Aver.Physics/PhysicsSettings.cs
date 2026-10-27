// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.

// The physics world's configuration (physics_world_abi.h): live solver settings, world limits, rebuild and build facts.
// Units: cm, cm/s, seconds, degrees where the name says so.

using System;

namespace Aver.Physics;

/// <summary>The live solver settings, in the native table's order (AVER_PHYS_WS_*).</summary>
public enum PhysicsSetting
{
    /// <summary>Solver velocity iterations (2..64). Below 2 friction stops working.</summary>
    VelocitySteps = 0,
    PositionSteps = 1,
    /// <summary>Collision steps per fixed step (1..8): the rate at which a fast body is tested.</summary>
    CollisionSteps = 2,
    Baumgarte = 3,
    /// <summary>cm.</summary>
    SpeculativeDistance = 4,
    /// <summary>cm.</summary>
    PenetrationSlop = 5,
    /// <summary>cm.</summary>
    MaxPenetration = 6,
    /// <summary>cm.</summary>
    ManifoldTolerance = 7,
    LinearCastThreshold = 8,
    LinearCastMaxPenetration = 9,
    /// <summary>cm/s.</summary>
    MinRestitutionSpeed = 10,
    /// <summary>Seconds a body must be still before it sleeps.</summary>
    SleepTime = 11,
    /// <summary>cm/s.</summary>
    SleepSpeed = 12,
    /// <summary>1 or 0.</summary>
    AllowSleeping = 13,
    /// <summary>1 or 0. Off is faster and breaks state snapshots and replays.</summary>
    Deterministic = 14,
    WarmStart = 15,
    BodyPairCache = 16,
    ManifoldReduction = 17,
    LargeIslandSplitter = 18,
    CheckActiveEdges = 19,
    MaxPairsInFlight = 20,
    StepListenerBatch = 21,
    StepListenerBatchesPerJob = 22,
    /// <summary>cm.</summary>
    PairCacheDeltaPos = 23,
    /// <summary>Degrees.</summary>
    PairCacheDeltaRot = 24,
    /// <summary>Degrees.</summary>
    ContactNormalDelta = 25,
    /// <summary>cm.</summary>
    LambdaPreserveDist = 26,
    /// <summary>How many settings there are (AVER_PHYS_WS_COUNT); not a setting.</summary>
    Count = 27,
}

/// <summary>The limits a world is built with (AVER_PHYS_CF_*). Fixed for the world's lifetime; a set applies at the next world creation or via RebuildWorld.</summary>
public enum PhysicsConfigKey
{
    MaxBodies = 0,
    MaxBodyPairs = 1,
    MaxContacts = 2,
    /// <summary>The contact scratch allocator, in MiB (PHYSICS.TEMPALLOCMB).</summary>
    TempMb = 3,
    /// <summary>Worker threads: -1 automatic (cores - 2), 0 single-threaded, N = N workers.</summary>
    Threads = 4,
    /// <summary>How many limits there are (AVER_PHYS_CF_COUNT); not a key.</summary>
    Count = 5,
}

/// <summary>Whether the native physics library was built with single or double precision positions.</summary>
public enum PhysicsPrecision
{
    Single = 1,
    Double = 2,
}

/// <summary>Optional features the native physics library was compiled with.</summary>
[Flags]
public enum PhysicsBuildFlags : uint
{
    DebugRenderer = 0x01,
    Profiler = 0x02,
    CrossPlatform = 0x04,
    DeterminismLog = 0x08,
    StatsTracking = 0x10,
    Asserts = 0x20,
}

public static partial class Physics
{
    /// <summary>The setting's current value.</summary>
    public static float GetSetting(PhysicsSetting key) =>
        Native.aver_phys_setting_get((int)key, out float v) != 0 ? v : 0f;

    /// <summary>Sets a setting (clamped into its range, whole numbers for counts and flags). False for an unknown key or a NaN.
    /// Applies to the running world at once, and survives a rebuild.</summary>
    public static bool SetSetting(PhysicsSetting key, float value) => Native.aver_phys_setting_set((int)key, value) != 0;

    /// <summary>Puts a setting back to Jolt's default.</summary>
    public static bool ResetSetting(PhysicsSetting key) => Native.aver_phys_setting_reset((int)key) != 0;

    /// <summary>Puts every setting back to its default.</summary>
    public static void ResetAllSettings() => Native.aver_phys_setting_reset(-1);

    /// <summary>The smallest and largest legal value and the default.</summary>
    public static (float Min, float Max, float Default) SettingRange(PhysicsSetting key) =>
        Native.aver_phys_setting_range((int)key, out float lo, out float hi, out float def) != 0 ? (lo, hi, def) : (0f, 0f, 0f);

    /// <summary>The setting's name as the console and the project file spell it: <c>velocitySteps</c>,
    /// <c>PHYSICS.VELOCITYSTEPS</c> in the manifest.</summary>
    public static string SettingName(PhysicsSetting key)
    {
        string n = key.ToString();
        return n.Length == 0 ? n : char.ToLowerInvariant(n[0]) + n.Substring(1);
    }

    /// <summary>Finds a setting by its name, ignoring case.</summary>
    public static bool TryFindSetting(string name, out PhysicsSetting key)
    {
        int k = Native.aver_phys_setting_find(name);
        key = (PhysicsSetting)(k < 0 ? 0 : k);
        return k >= 0;
    }

    /// <summary>Sets a limit the NEXT world is built with (works with or without a world). Clamped; false for an unknown key or a NaN.</summary>
    public static bool SetConfig(PhysicsConfigKey key, float value) => Native.aver_phys_config_set((int)key, value) != 0;

    /// <summary>The pending value of a limit: what the next world will use.</summary>
    public static float GetConfig(PhysicsConfigKey key) =>
        Native.aver_phys_config_get((int)key, out float v) != 0 ? v : 0f;

    /// <summary>What the running world was built with; null with no world.</summary>
    public static float? GetActiveConfig(PhysicsConfigKey key) =>
        Native.aver_phys_config_active((int)key, out float v) != 0 ? v : null;

    /// <summary>The smallest and largest legal value of a limit and its default.</summary>
    public static (float Min, float Max, float Default) ConfigRange(PhysicsConfigKey key) =>
        Native.aver_phys_config_range((int)key, out float lo, out float hi, out float def) != 0 ? (lo, hi, def) : (0f, 0f, 0f);

    /// <summary>True when a world exists and was built with other limits than the pending ones.</summary>
    public static bool ConfigNeedsRebuild => Native.aver_phys_config_differs() != 0;

    /// <summary>True when <see cref="RebuildWorld"/> would be accepted: no world, or a world with no bodies, characters, joints, ragdolls or vehicles.</summary>
    public static bool WorldIsEmpty => Native.aver_phys_world_empty() != 0;

    /// <summary>Creates the world again with the pending limits. Refused (false) unless the world is empty; settings and shape handles survive.</summary>
    public static bool RebuildWorld() => Native.aver_phys_rebuild() != 0;

    /// <summary>Single or double precision positions in the native library.</summary>
    public static PhysicsPrecision Precision => (PhysicsPrecision)Native.aver_phys_precision();

    /// <summary>The optional features the native library has.</summary>
    public static PhysicsBuildFlags BuildFlags => (PhysicsBuildFlags)Native.aver_phys_build_flags();

    /// <summary>Jolt's configuration string plus the state layout version. Two peers compare this before they exchange physics state.</summary>
    public static string BuildString
    {
        get
        {
            int need = Native.aver_phys_build_string(null, 0);
            if (need <= 1) return string.Empty;
            byte[] buf = new byte[need];
            Native.aver_phys_build_string(buf, need);
            return System.Text.Encoding.UTF8.GetString(buf, 0, need - 1);
        }
    }
}
