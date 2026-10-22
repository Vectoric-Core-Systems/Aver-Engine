// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the physics-world Aver Node nodes call (GraphPhysicsWorldNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsPhysicsWorld.hpp; docs/plans/wave11/WORLD_STATE_DEBUG.md section 2.12): a solver setting by name, and
// in-session snapshots for a rewind or a checkpoint. Each is a thin wrapper over Aver.Physics. A node never throws: no physics
// library, no world, an unknown setting or a refused restore answers false/0, logged once per node.
//
// SNAPSHOTS HERE ARE A GRAPH-OWNED TABLE of at most 16 live ones, addressed by a small integer the graph carries in a pin. They
// die with the physics world (a level change that rebuilds it), after which their pins answer false; a stale one is dropped the
// next time a snapshot is taken. StepExact, the profiler, the debug draw and the large-world origin are NOT nodes: stepping from
// a graph breaks the tick contract, and the others are tools or engine plumbing.

using AP = Aver.Physics;

namespace Aver.Framework;

internal static class PhysicsWorldGraph
{
    /// <summary>How many snapshots a graph may hold at once.</summary>
    internal const int MaxSnapshots = 16;

    private static readonly Dictionary<int, AP.PhysicsSnapshot> Snapshots = new();
    private static int s_next = 1;

    /// <summary>AN_PhysicsSetSetting: a live solver setting by the node's setting attribute (velocitySteps, sleepTime, ...).
    /// Clamped into its range; applies to the running world at once.</summary>
    internal static bool SetSettingForGraph(string setting, float value)
    {
        try
        {
            return FamiliesBLog.Ok("AN_PhysicsSetSetting",
                AP.Physics.TryFindSetting(setting ?? string.Empty, out AP.PhysicsSetting key) && AP.Physics.SetSetting(key, value),
                "unknown setting (names are the physics settings table's: velocitySteps, positionSteps, sleepTime, ...), or the value is not a number");
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PhysicsSetSetting", e); }
    }

    /// <summary>AN_PhysicsGetSetting: a solver setting's current value; found is false for an unknown name.</summary>
    internal static bool GetSettingForGraph(string setting, out float value)
    {
        value = 0f;
        try
        {
            if (!AP.Physics.TryFindSetting(setting ?? string.Empty, out AP.PhysicsSetting key)) return false;
            value = AP.Physics.GetSetting(key);
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PhysicsGetSetting", e); }
    }

    /// <summary>Parts as the node's parts attribute spells them: any of all, bodies, contacts, constraints, characters, engine,
    /// statics, separated by commas, plus signs, bars or spaces. Empty means all.</summary>
    internal static bool TryParseParts(string? text, out AP.SnapshotParts parts)
    {
        parts = 0;
        if (string.IsNullOrWhiteSpace(text)) { parts = AP.SnapshotParts.All; return true; }
        foreach (string raw in text.Split(new[] { ',', '+', '|', ' ', ';' }, StringSplitOptions.RemoveEmptyEntries))
        {
            switch (raw.ToLowerInvariant())
            {
                case "all": parts |= AP.SnapshotParts.All; break;
                case "bodies": parts |= AP.SnapshotParts.Bodies; break;
                case "contacts": parts |= AP.SnapshotParts.Contacts; break;
                case "constraints": parts |= AP.SnapshotParts.Constraints; break;
                case "characters": parts |= AP.SnapshotParts.Characters; break;
                case "engine": parts |= AP.SnapshotParts.Engine; break;
                case "statics": parts |= AP.SnapshotParts.Statics; break;
                default: parts = 0; return false;
            }
        }
        return parts != 0;
    }

    /// <summary>AN_PhysicsSnapshot: captures the physics world into the graph's table. snapshot is 0 on failure (no world, an
    /// unknown part, or 16 already held).</summary>
    internal static bool SnapshotForGraph(string parts, out int snapshot)
    {
        snapshot = 0;
        try
        {
            if (!FamiliesBLog.Ok("AN_PhysicsSnapshot", TryParseParts(parts, out AP.SnapshotParts p),
                    "unknown part (use all, bodies, contacts, constraints, characters, engine, statics)"))
                return false;
            DropDead();
            if (!FamiliesBLog.Ok("AN_PhysicsSnapshot", Snapshots.Count < MaxSnapshots,
                    $"{MaxSnapshots} snapshots are already held: release one with AN_PhysicsReleaseSnapshot"))
                return false;
            AP.PhysicsSnapshot? snap = AP.PhysicsSnapshot.Capture(p);
            if (!FamiliesBLog.Ok("AN_PhysicsSnapshot", snap is not null, "there is no physics world to capture")) return false;
            snapshot = s_next++;
            Snapshots[snapshot] = snap!;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PhysicsSnapshot", e); }
    }

    /// <summary>AN_PhysicsRestore: puts the physics world back as the snapshot held it. False (the world untouched) when the
    /// snapshot is gone or the world changed in a way a restore cannot cross: a body, character, vehicle or joint it held was
    /// removed, or a constraint was added or removed.</summary>
    internal static bool RestoreForGraph(int snapshot)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_PhysicsRestore", Snapshots.TryGetValue(snapshot, out AP.PhysicsSnapshot? snap) && snap.IsValid,
                    "no such snapshot (it was released, or the physics world was rebuilt)"))
                return false;
            return FamiliesBLog.Ok("AN_PhysicsRestore", snap!.Restore(),
                $"the world no longer matches the snapshot ({AP.PhysicsSnapshot.LastResult}); nothing was changed");
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PhysicsRestore", e); }
    }

    /// <summary>AN_PhysicsReleaseSnapshot: frees a snapshot.</summary>
    internal static bool ReleaseSnapshotForGraph(int snapshot)
    {
        try
        {
            if (!FamiliesBLog.Ok("AN_PhysicsReleaseSnapshot", Snapshots.TryGetValue(snapshot, out AP.PhysicsSnapshot? snap), "no such snapshot"))
                return false;
            snap!.Dispose();
            Snapshots.Remove(snapshot);
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_PhysicsReleaseSnapshot", e); }
    }

    /// <summary>AN_PhysicsStateHash: the 64-bit hash of the physics state as two 32-bit halves (low is the return, high the out).
    /// Two worlds in the same state have the same pair; 0, 0 with no world.</summary>
    internal static int StateHashForGraph(out int high)
    {
        high = 0;
        try
        {
            ulong h = AP.Physics.StateHash();
            high = unchecked((int)(uint)(h >> 32));
            return unchecked((int)(uint)(h & 0xFFFFFFFFUL));
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_PhysicsStateHash", e); return 0; }
    }

    /// <summary>AN_PhysicsStats: bodies (the return), active dynamic bodies, fixed steps run by the last step call, the
    /// milliseconds of the last solver update, and the Jolt update errors (non-zero means contacts were dropped: raise a limit).</summary>
    internal static int StatsForGraph(out int activeDynamic, out int steps, out float stepMs, out int updateErrors)
    {
        activeDynamic = 0; steps = 0; stepMs = 0f; updateErrors = 0;
        try
        {
            AP.PhysicsStats s = AP.Physics.GetStats();
            activeDynamic = s.ActiveDynamicBodies;
            steps = s.StepsLastCall;
            stepMs = (float)s.StepMsLast;
            updateErrors = (int)s.UpdateErrors;
            return s.Bodies;
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_PhysicsStats", e); return 0; }
    }

    // A snapshot that died with its world is dropped, so the table does not fill with ghosts across level changes.
    private static void DropDead()
    {
        List<int>? dead = null;
        foreach (var (id, snap) in Snapshots)
            if (!snap.IsValid) (dead ??= new List<int>()).Add(id);
        if (dead is null) return;
        foreach (int id in dead) Snapshots.Remove(id);
    }

    /// <summary>For tests: forgets every table entry without touching the native side.</summary>
    internal static void ResetForTests() => Snapshots.Clear();

    /// <summary>For tests: how many snapshots the table holds.</summary>
    internal static int HeldForTests => Snapshots.Count;
}
