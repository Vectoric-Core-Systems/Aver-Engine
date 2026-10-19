// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Soft-body fields from a script: volumes that push, hurt, release the pins of, kill or hold cloth and destructibles, and the
// crash-damage helper VEHICLES calls. Design: docs/SOFTBODY.md "Fields".
//
// A field is either placed (a CSoftField component on an entity, whose transform is the volume's frame: Entity.AddSoftField),
// fired once (SoftFields.Pulse / Impact), started for a while (Start / Stop) or spawned as an entity that removes itself
// (Spawn). Everything that HAPPENS goes through the framework relay and is applied at the soft-body system's next tick, in
// arrival order. A host that never installed the system gets zero, not a throw.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>What a soft field does (pinned to softbody::FieldKind).</summary>
public enum SoftFieldKind { RadialForce = 0, DirectionalForce = 1, Damage = 2, ReleaseAnchors = 3, Kill = 4, Sleep = 5 }

/// <summary>The volume of a soft field (pinned to softbody::FieldShape).</summary>
public enum SoftFieldShape { Sphere = 0, Box = 1, Capsule = 2, Everywhere = 3 }

/// <summary>Which soft bodies a field touches (pinned to the kFieldTarget* masks).</summary>
[Flags] public enum SoftFieldTargets { Cloth = 1, Destructible = 2, Debris = 4, All = 7 }

/// <summary>One field. Extents are local centimetres: a sphere's radius is X; a box's half extents; a capsule's radius is X and
/// half height Z. <paramref name="RotationDegrees"/> is roll (X), pitch (Y), yaw (Z), the editor's convention.</summary>
public readonly record struct SoftFieldSpec(
    SoftFieldKind Kind, SoftFieldShape Shape, Vec3 Centre, Vec3 Extents, Vec3 RotationDegrees = default, Vec3 Direction = default,
    float Strength = 0f, float EnergyJoules = 0f, float Falloff = 1f, float Turbulence = 0f,
    SoftFieldTargets Targets = SoftFieldTargets.All, bool PerSecond = false);

/// <summary>Fields fired from code. Every call is applied at the next soft-body tick; the returned counts are the pieces in
/// reach when the call was made, an upper bound of what that tick touches (0 with no soft-body system).</summary>
public static class SoftFields
{
    /// <summary>Applies the field once at the next tick. Strength is a velocity change in cm/s; energy is total joules.
    /// Returns the pieces in reach now.</summary>
    public static int Pulse(in SoftFieldSpec spec) => FwSoftField.aver_fw_softfield_pulse(SoftFieldWire.Encode(spec, 0f), SoftFieldWire.SpecCount);

    /// <summary>The crash helper: damage energy in a sphere at <paramref name="point"/>, shoved along <paramref name="direction"/>
    /// (zero = radial) with an extra kick of <paramref name="kickCmPerS"/>. Returns the pieces in reach now.</summary>
    public static int Impact(Vec3 point, Vec3 direction, float radiusCm, float energyJoules, float kickCmPerS = 0f) =>
        FwSoftField.aver_fw_softfield_impact(point.X, point.Y, point.Z, direction.X, direction.Y, direction.Z, radiusCm, energyJoules, kickCmPerS);

    /// <summary>Starts a continuous field (strength and energy become per second) that stops after <paramref name="lifetimeSeconds"/>.
    /// Returns a handle, 0 if refused.</summary>
    public static int Start(in SoftFieldSpec spec, float lifetimeSeconds) =>
        FwSoftField.aver_fw_softfield_start(SoftFieldWire.Encode(spec, lifetimeSeconds), SoftFieldWire.SpecCount);

    /// <summary>Stops a field started with Start.</summary>
    public static bool Stop(int handle) => FwSoftField.aver_fw_softfield_stop(handle) != 0;

    /// <summary>Creates an entity with a CSoftField that removes itself after the lifetime; move it to move the field.</summary>
    public static Entity Spawn(in SoftFieldSpec spec, float lifetimeSeconds) =>
        new(FwSoftField.aver_fw_softfield_spawn(SoftFieldWire.Encode(spec, lifetimeSeconds), SoftFieldWire.SpecCount));
}

public readonly partial struct Entity
{
    /// <summary>Adds a CSoftField to this entity (its transform is the field's frame). Needs nothing else. Strength is the velocity
    /// change in cm/s per second and energy the joules per second, while the field is enabled.</summary>
    public bool AddSoftField(SoftFieldKind kind, SoftFieldShape shape, Vec3 extents, float strength = 0f, float energyJoulesPerSecond = 0f)
    {
        if (!HasComponent("CSoftField") && !AddComponent("CSoftField")) return false;
        bool ok = SetInt("CSoftField.kind", (int)kind) & SetInt("CSoftField.shape", (int)shape) & SetInt("CSoftField.flags", 0) & SetInt("CSoftField.targets", 0);
        ok &= SetFloat("CSoftField.extentX", extents.X) & SetFloat("CSoftField.extentY", extents.Y) & SetFloat("CSoftField.extentZ", extents.Z);
        ok &= SetFloat("CSoftField.strength", strength) & SetFloat("CSoftField.energyJPerS", energyJoulesPerSecond) & SetFloat("CSoftField.falloff", 1f);
        return ok;
    }

    /// <summary>Enables or disables this entity's soft field.</summary>
    public bool SetSoftFieldEnabled(bool enabled)
    {
        if (!HasComponent("CSoftField")) return false;
        int flags = GetInt("CSoftField.flags");
        return SetInt("CSoftField.flags", enabled ? flags & ~1 : flags | 1);
    }

    /// <summary>The pieces this entity's field reached on the last tick.</summary>
    public bool TryGetSoftFieldTouched(out int pieces)
    {
        pieces = 0;
        if (!HasComponent("CSoftField")) return false;
        pieces = GetInt("CSoftField.outTouched");
        return true;
    }

    /// <summary>Destroys this soft body's debris pieces now.</summary>
    public bool ClearSoftDebris() => FwSoftBody.aver_fw_softbody_clear_debris(Handle) != 0;

    /// <summary>Holds (true) or releases (false) this soft body's pieces asleep.</summary>
    public bool SetSoftBodyAsleep(bool asleep) => FwSoftBody.aver_fw_softbody_set_asleep(Handle, asleep ? 1 : 0) != 0;

    /// <summary>Permanent set: dented edges, the largest set in cm, and live debris pieces.</summary>
    public bool TryGetSoftBodyDents(out int dentedEdges, out float maxSetCm, out int debrisPieces)
    {
        float[] f = new float[1];
        int[] i = new int[2];
        bool ok = FwSoftBody.aver_fw_softbody_dents(Handle, f, i) != 0;
        maxSetCm = ok ? f[0] : 0f;
        dentedEdges = ok ? i[0] : 0;
        debrisPieces = ok ? i[1] : 0;
        return ok;
    }
}

/// <summary>The wire form of a <see cref="SoftFieldSpec"/>: the 24 floats of framework_softfield_abi.h.</summary>
internal static class SoftFieldWire
{
    internal const int SpecCount = 24;

    internal static float[] Encode(in SoftFieldSpec s, float lifetimeSeconds)
    {
        var f = new float[SpecCount];
        f[0] = (float)s.Kind;
        f[1] = (float)s.Shape;
        f[2] = (float)(int)s.Targets;
        f[3] = s.PerSecond ? 1f : 0f;
        f[4] = s.Centre.X; f[5] = s.Centre.Y; f[6] = s.Centre.Z;
        f[7] = s.Extents.X; f[8] = s.Extents.Y; f[9] = s.Extents.Z;
        // The ABI lists yaw, pitch, roll; the C# convention is roll (X), pitch (Y), yaw (Z).
        f[10] = s.RotationDegrees.Z; f[11] = s.RotationDegrees.Y; f[12] = s.RotationDegrees.X;
        f[13] = s.Direction.X; f[14] = s.Direction.Y; f[15] = s.Direction.Z;
        f[16] = s.Strength;
        f[17] = s.EnergyJoules;
        f[18] = s.Falloff;
        f[19] = s.Turbulence;
        f[20] = 5f;   // wakeCmS: a sleeping piece wakes only if the field would change a vertex velocity by this much
        f[21] = lifetimeSeconds;
        return f;
    }
}

/// <summary>The graph-node surface for the Fields nodes: the same calls with the int-entity / primitive-only signatures
/// GraphCompiler's emitted IL needs. Forwarded from the node table (GraphFieldsNodeTable.cs). A bad kind or shape, or a failed
/// call, returns false and zeros; nothing throws.</summary>
internal static class SoftFieldGraph
{
    private static bool Valid(int kind, int shape) => kind is >= 0 and <= 5 && shape is >= 0 and <= 3;

    private static SoftFieldSpec Make(int kind, int shape, float x, float y, float z, float ex, float ey, float ez, float dirX, float dirY, float dirZ,
                                      float strength, float energy, float falloff, int targets) =>
        new((SoftFieldKind)kind, (SoftFieldShape)shape, new Vec3(x, y, z), new Vec3(ex, ey, ez), default, new Vec3(dirX, dirY, dirZ),
            strength, energy, falloff, 0f, (SoftFieldTargets)(targets & 7), false);

    /// <summary>Success means at least one piece was in reach.</summary>
    internal static bool SoftFieldPulseForGraph(int kind, int shape, float x, float y, float z, float ex, float ey, float ez, float dirX, float dirY, float dirZ,
                                                float strength, float energy, float falloff, int targets, out int reached)
    {
        reached = 0;
        if (!Valid(kind, shape)) return false;
        reached = SoftFields.Pulse(Make(kind, shape, x, y, z, ex, ey, ez, dirX, dirY, dirZ, strength, energy, falloff, targets));
        return reached > 0;
    }

    internal static bool SoftFieldImpactForGraph(float x, float y, float z, float dirX, float dirY, float dirZ, float radius, float energy, float kick, out int reached)
    {
        reached = SoftFields.Impact(new Vec3(x, y, z), new Vec3(dirX, dirY, dirZ), radius, energy, kick);
        return reached > 0;
    }

    internal static bool SoftFieldStartForGraph(int kind, int shape, float x, float y, float z, float ex, float ey, float ez, float dirX, float dirY, float dirZ,
                                                float strength, float energy, float falloff, int targets, float lifetime, out int handle)
    {
        handle = 0;
        if (!Valid(kind, shape)) return false;
        handle = SoftFields.Start(Make(kind, shape, x, y, z, ex, ey, ez, dirX, dirY, dirZ, strength, energy, falloff, targets), lifetime);
        return handle != 0;
    }

    internal static bool SoftFieldStopForGraph(int handle) => SoftFields.Stop(handle);

    internal static bool SpawnSoftFieldForGraph(int kind, int shape, float x, float y, float z, float ex, float ey, float ez,
                                                float strength, float energy, float falloff, float lifetime, out int entity)
    {
        entity = 0;
        if (!Valid(kind, shape)) return false;
        entity = SoftFields.Spawn(Make(kind, shape, x, y, z, ex, ey, ez, 0f, 0f, 0f, strength, energy, falloff, 0), lifetime).Handle;
        return entity != 0;
    }

    internal static bool SoftFieldSetEnabledForGraph(int entity, bool enabled) => new Entity(entity).SetSoftFieldEnabled(enabled);

    internal static bool SoftFieldSetStrengthForGraph(int entity, float strength, float energy)
    {
        var e = new Entity(entity);
        if (!e.HasComponent("CSoftField")) return false;
        return e.SetFloat("CSoftField.strength", strength) & e.SetFloat("CSoftField.energyJPerS", energy);
    }

    internal static bool GetSoftFieldTouchedForGraph(int entity, out int touched)
    {
        bool ok = new Entity(entity).TryGetSoftFieldTouched(out int pieces);
        touched = pieces;
        return ok;
    }
}
