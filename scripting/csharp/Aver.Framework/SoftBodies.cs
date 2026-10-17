// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Cloth and destruction from a script: the CCloth / CDestructible surface on Entity, the world wind and explosions on
// SoftBodies, and the polled event stream. Design: docs/SOFTBODY.md.
//
// Components are registered at RUNTIME (like CMotionMatcher), so adding and configuring them is a field write over
// the generic scene ABI and a host that never registered them gets false, not a throw. Everything that HAPPENS
// (damage, cuts, repair, release, wind) goes through the framework relay and is applied at the soft-body system's
// next tick, in arrival order.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The material presets (pinned to softbody::SoftPreset in Material.hpp).</summary>
public enum SoftPreset { ClothLight = 0, ClothHeavy = 1, Metal = 2, Wood = 3, Glass = 4 }

/// <summary>What deposited the energy (pinned to softbody::DepositKind).</summary>
public enum SoftDamageKind { Impact = 0, Explosion = 1, Gameplay = 2 }

/// <summary>Pinned to softbody::SoftEventKind.</summary>
public enum SoftEventKind { Impact, Torn, Detached, Dust, Repaired, Released }

/// <summary>One thing that happened to a soft body. <paramref name="Seq"/> is a per-system counter, the
/// replication key. For Torn, <paramref name="Beam"/> is the first broken edge and <paramref name="Value"/> how many
/// broke; for Impact, Value is the closing speed (cm/s); for Detached, the mass (kg); for Dust, the particle count.</summary>
public readonly record struct SoftEvent(SoftEventKind Kind, int Entity, int Piece, int Beam, Vec3 Point, float Value, int Seq);

/// <summary>What an entity's soft body is doing.</summary>
public readonly record struct SoftBodyState(float Damage01, float MaxStrain, int Pieces, int BrokenEdges, bool Awake, bool Frozen);

public readonly partial struct Entity
{
    /// <summary>Simulates this entity's mesh as cloth (pinned, painted or ruled), skinned to its skeleton if it
    /// has one. Adds CSoftBody (the marker the draw seam keys on) and CCloth. Needs a mesh; refused on an entity
    /// that is already Destructible (one body per entity root).</summary>
    public bool AddCloth(SoftPreset preset = SoftPreset.ClothLight, string asset = "")
    {
        if (!HasComponent(Component.MeshRenderer) || HasComponent("CDestructible")) return false;
        if (!HasComponent("CSoftBody") && !AddComponent("CSoftBody")) return false;
        if (!HasComponent("CCloth") && !AddComponent("CCloth")) return false;
        SetInt("CCloth.preset", (int)preset);
        return SetInt64("CCloth.asset", string.IsNullOrEmpty(asset) ? 0 : Assets.ObjectIdOf(asset));
    }

    /// <summary>Makes this entity breakable: dents under hits, tears into pieces past its material's limits.</summary>
    public bool AddDestructible(SoftPreset preset = SoftPreset.Metal, string asset = "")
    {
        if (!HasComponent(Component.MeshRenderer) || HasComponent("CCloth")) return false;
        if (!HasComponent("CSoftBody") && !AddComponent("CSoftBody")) return false;
        if (!HasComponent("CDestructible") && !AddComponent("CDestructible")) return false;
        SetInt("CDestructible.preset", (int)preset);
        return SetInt64("CDestructible.asset", string.IsNullOrEmpty(asset) ? 0 : Assets.ObjectIdOf(asset));
    }

    /// <summary>Changes the material preset of this entity's Cloth or Destructible (it rebuilds on the next tick).</summary>
    public bool SetSoftPreset(SoftPreset preset) => FwSoftBody.aver_fw_softbody_set_preset(Handle, (int)preset) != 0;

    /// <summary>Deposits <paramref name="energyJoules"/> in a sphere at <paramref name="point"/>: damage that breaks
    /// edges past their toughness and a kick that dents what yields. A zero <paramref name="direction"/> is radial.</summary>
    public bool ApplySoftDamage(Vec3 point, float radiusCm, float energyJoules, Vec3 direction = default,
                                SoftDamageKind kind = SoftDamageKind.Gameplay) =>
        FwSoftBody.aver_fw_softbody_damage(Handle, point.X, point.Y, point.Z, direction.X, direction.Y, direction.Z,
                                           radiusCm, energyJoules, (int)kind) != 0;

    /// <summary>A scripted cut: breaks the edges whose middle is within <paramref name="radiusCm"/> of the point.</summary>
    public bool TearSoftBody(Vec3 point, float radiusCm) =>
        FwSoftBody.aver_fw_softbody_tear(Handle, point.X, point.Y, point.Z, radiusCm) != 0;

    /// <summary>Back to intact: pieces gone, dents out, tears closed.</summary>
    public bool RepairSoftBody() => FwSoftBody.aver_fw_softbody_repair(Handle) != 0;

    /// <summary>Unpins a cloth: a cape falls off its shoulders.</summary>
    public bool ReleaseCloth() => FwSoftBody.aver_fw_softbody_release(Handle) != 0;

    /// <summary>The soft body's damage, strain and piece count as of the last tick. False when it has none.</summary>
    public bool TryGetSoftBodyState(out SoftBodyState state)
    {
        float[] f = new float[2];
        int[] i = new int[3];
        if (FwSoftBody.aver_fw_softbody_state(Handle, f, i) == 0)
        {
            state = default;
            return false;
        }
        state = new SoftBodyState(f[0], f[1], i[0], i[1], (i[2] & 1) != 0, (i[2] & 2) != 0);
        return true;
    }
}

/// <summary>World-level cloth and destruction: wind, explosions, budgets and the event stream.</summary>
public static class SoftBodies
{
    /// <summary>The world wind in cm/s, independent of the sky's cloud drift. A gust adds a sinusoid along the wind
    /// direction at <paramref name="gustHz"/>; turbulence adds a deterministic noise (no clock, no random).</summary>
    public static bool SetWind(Vec3 velocityCmPerS, float gustCmPerS = 0f, float gustHz = 0.3f, float turbulence = 0f) =>
        FwSoftBody.aver_fw_softbody_set_wind(velocityCmPerS.X, velocityCmPerS.Y, velocityCmPerS.Z, gustCmPerS, gustHz, turbulence) != 0;

    /// <summary>A radial shove and damage to every soft body in reach. Returns how many entities are in reach.</summary>
    public static int Explode(Vec3 centre, float radiusCm, float impulseCmPerS, float energyJoules) =>
        FwSoftBody.aver_fw_softbody_explode(centre.X, centre.Y, centre.Z, radiusCm, impulseCmPerS, energyJoules);

    /// <summary>Caps: live debris pieces, simulated particles, rebuilds per frame and how long debris lives.</summary>
    public static bool SetBudget(int maxPieces = 48, int maxParticles = 20000, int maxRebuildsPerFrame = 2, float debrisLifetimeSeconds = 12f) =>
        FwSoftBody.aver_fw_softbody_set_budget(maxPieces, maxParticles, maxRebuildsPerFrame, debrisLifetimeSeconds) != 0;

    /// <summary>Moves the pending events into <paramref name="buffer"/> and returns how many. The same events
    /// also reach graphs as the named events SoftBodyImpact, SoftBodyTorn, SoftBodyDetached, SoftBodyDust and
    /// SoftBodyRepaired.</summary>
    public static int DrainEvents(Span<SoftEvent> buffer)
    {
        if (buffer.Length == 0) return 0;
        int max = buffer.Length;
        int[] ints = new int[max * 5];
        float[] floats = new float[max * 4];
        int n = FwSoftBody.aver_fw_softbody_drain_events(ints, floats, max);
        for (int k = 0; k < n; k++)
            buffer[k] = new SoftEvent((SoftEventKind)ints[k * 5], ints[k * 5 + 1], ints[k * 5 + 2], ints[k * 5 + 3],
                                      new Vec3(floats[k * 4], floats[k * 4 + 1], floats[k * 4 + 2]), floats[k * 4 + 3], ints[k * 5 + 4]);
        return n;
    }
}

/// <summary>The graph-node surface for the soft-body nodes: the same calls with the int-entity / primitive-only
/// signatures GraphCompiler's emitted IL needs. Forwarded from the node table (GraphSoftBodyNodeTable.cs).</summary>
internal static class SoftBodyGraphInterop
{
    /// <summary>A zero direction (all three) is radial.</summary>
    internal static bool SoftBodyDamageForGraph(int entity, float x, float y, float z, float radius, float energy, float dirX, float dirY, float dirZ) =>
        new Entity(entity).ApplySoftDamage(new Vec3(x, y, z), radius, energy, new Vec3(dirX, dirY, dirZ));

    internal static bool SoftBodyExplodeForGraph(float x, float y, float z, float radius, float impulse, float energy, out int touched)
    {
        touched = SoftBodies.Explode(new Vec3(x, y, z), radius, impulse, energy);
        return true;
    }

    internal static bool SoftBodyTearForGraph(int entity, float x, float y, float z, float radius) =>
        new Entity(entity).TearSoftBody(new Vec3(x, y, z), radius);

    internal static bool SoftBodyRepairForGraph(int entity) => new Entity(entity).RepairSoftBody();

    internal static bool SoftBodyReleaseForGraph(int entity) => new Entity(entity).ReleaseCloth();

    internal static bool SetSoftBodyWindForGraph(float vx, float vy, float vz, float gust, float gustHz, float turbulence) =>
        SoftBodies.SetWind(new Vec3(vx, vy, vz), gust, gustHz, turbulence);

    internal static bool SetSoftBodyPresetForGraph(int entity, int preset) => new Entity(entity).SetSoftPreset((SoftPreset)preset);

    internal static bool GetSoftBodyStateForGraph(int entity, out float damage, out float maxStrain, out int pieces, out int brokenEdges)
    {
        bool ok = new Entity(entity).TryGetSoftBodyState(out SoftBodyState st);
        damage = st.Damage01;
        maxStrain = st.MaxStrain;
        pieces = st.Pieces;
        brokenEdges = st.BrokenEdges;
        return ok;
    }
}
