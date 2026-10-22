// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Jelly, ropes and cables, grab and release, inflatables, soft-body buoyancy and contact policy from a script: the CSoftVolume /
// CSoftRope surface on Entity and the world-level calls on Ropes and SoftShapes. Design: docs/plans/wave11/SOFTBODY_HAIR.md 2.4 and 2.6,
// docs/SOFTBODY.md.
//
// CSoftVolume and CSoftRope are registered at RUNTIME (like CCloth), so adding and configuring them is a field write over the generic
// scene ABI and a host that never registered them gets false, not a throw. What HAPPENS (a grab, a cut, buoyancy, pressure) goes through
// the framework relay (framework_softshape_abi.h) and applies at once, in arrival order. Units are engine centimetres and kilopascal.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>Which end of a rope.</summary>
public enum RopeEnd { Start = 0, End = 1 }

/// <summary>What happens when a soft body meets one particular other body (pinned to AVER_PHYS_SBX_PAIR_*).</summary>
public enum SoftContactMode
{
    /// <summary>The usual response.</summary>
    Normal = 0,
    /// <summary>No response; enter and exit events instead.</summary>
    Sensor = 1,
    /// <summary>The contact is rejected: the pair passes through each other.</summary>
    Ignore = 2,
}

/// <summary>A rope as of the last tick: the polyline length, its strain against the rest length, the segment count, whether it was cut.</summary>
public readonly record struct RopeState(float LengthCm, float Strain, int Segments, bool Cut, bool Awake);

/// <summary>A jelly as of the last tick: current / rest volume, the largest lattice edge strain, the lattice node count.</summary>
public readonly record struct JellyState(float VolumeRatio, float MaxStretch, int Nodes, bool Awake, bool Frozen, bool Failed);

/// <summary>A soft body entering or leaving a sensor. <paramref name="Sensor"/> is the invalid entity when the sensor body belongs
/// to no rigid body component.</summary>
public readonly record struct SoftSensorEvent(Entity Soft, Entity Sensor, bool Entered);

public readonly partial struct Entity
{
    private const string VolumeType = "CSoftVolume";
    private const string RopeType = "CSoftRope";
    private const int RopeNoStartPin = 0x2, RopeNoEndPin = 0x4;

    // False, not a throw, in a process with no native scene (the bare-process graph tests, a tool).
    private static bool SafeHas(Entity e, string type)
    {
        try { return e.HasComponent(type); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    // The handle is passed in, not captured: a lambda inside a struct cannot touch `this` (CS1673).
    private static bool Relay(int handle, Func<int, int> call)
    {
        try { return call(handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    private bool SetRef(string field, int value)
    {
        try { return FwCameraScene.aver_scene_set_ref(Handle, SceneIds.Field(field), value) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Simulates this entity's closed mesh as a jelly: a tetrahedral lattice inside it that wobbles and keeps its volume, the
    /// mesh following. Needs a mesh renderer; refused on an entity that is already a rope. <paramref name="asset"/> is an .ocsoft path of
    /// kind "jelly" (empty = the jelly preset). The mesh must be closed, or the jelly reports failed and the entity keeps its mesh.</summary>
    public bool AddJelly(string asset = "", SoftPreset preset = SoftPreset.Jelly)
    {
        try
        {
            if (!HasComponent(Component.MeshRenderer) || SafeHas(this, RopeType)) return false;
            if (!SafeHas(this, VolumeType) && !AddComponent(VolumeType)) return false;
            SetInt("CSoftVolume.preset", (int)preset);
            return SetInt64("CSoftVolume.asset", string.IsNullOrEmpty(asset) ? 0 : Assets.ObjectIdOf(asset));
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Makes this entity a rope or cable between two world points, both ends pinned. <paramref name="lengthCm"/> 0 = the distance
    /// between the points times the asset's slack; <paramref name="radiusCm"/> 0 = the asset's. It draws through a generated tube (the entity
    /// is given a renderer if it has none). Refused on an entity that is already a jelly.</summary>
    public bool AddRope(Vec3 start, Vec3 end, string asset = "", float lengthCm = 0f, float radiusCm = 0f)
    {
        try
        {
            if (SafeHas(this, VolumeType)) return false;
            if (!SafeHas(this, RopeType) && !AddComponent(RopeType)) return false;
            SetRef("CSoftRope.startEntity", 0);
            SetRef("CSoftRope.endEntity", 0);
            SetVec3("CSoftRope.startOffset", start);
            SetVec3("CSoftRope.endOffset", end);
            SetFloat("CSoftRope.lengthCm", Math.Max(0f, lengthCm));
            SetFloat("CSoftRope.radiusCm", Math.Max(0f, radiusCm));
            SetInt("CSoftRope.flags", 0);
            return SetInt64("CSoftRope.asset", string.IsNullOrEmpty(asset) ? 0 : Assets.ObjectIdOf(asset));
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Pins a rope end to a point of an entity (<paramref name="offset"/> is in that entity's space), so the rope follows a moving
    /// door, a vehicle or an animated hand. Clears the end's free flag.</summary>
    public bool SetRopeEnd(RopeEnd end, Entity anchor, Vec3 offset)
    {
        if (!SafeHas(this, RopeType)) return false;
        string name = end == RopeEnd.Start ? "start" : "end";
        int bit = end == RopeEnd.Start ? RopeNoStartPin : RopeNoEndPin;
        if (!SetRef("CSoftRope." + name + "Entity", anchor.Handle)) return false;
        SetVec3("CSoftRope." + name + "Offset", offset);
        return SetInt("CSoftRope.flags", GetInt("CSoftRope.flags") & ~bit);
    }

    /// <summary>Puts a rope end at a world point: pinned there, or (<paramref name="pinned"/> false) let go to hang or fall.</summary>
    public bool SetRopeEndFree(RopeEnd end, Vec3 worldPoint, bool pinned)
    {
        if (!SafeHas(this, RopeType)) return false;
        string name = end == RopeEnd.Start ? "start" : "end";
        int bit = end == RopeEnd.Start ? RopeNoStartPin : RopeNoEndPin;
        if (!SetRef("CSoftRope." + name + "Entity", 0)) return false;
        SetVec3("CSoftRope." + name + "Offset", worldPoint);
        int flags = GetInt("CSoftRope.flags");
        return SetInt("CSoftRope.flags", pinned ? flags & ~bit : flags | bit);
    }

    /// <summary>Cuts this rope at its node nearest <paramref name="point"/> (an interior node within about a segment of it). This entity keeps
    /// the start; <paramref name="other"/> is a NEW entity holding the rest; both cut ends are free. False when the point is not on the rope.</summary>
    public bool CutRope(Vec3 point, out Entity other)
    {
        other = default;
        int made = 0;
        bool ok;
        try { made = FwSoftShape.aver_fw_softshape_cut_rope(Handle, point.X, point.Y, point.Z); ok = made > 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { ok = false; }
        if (ok) other = new Entity(made);
        return ok;
    }

    /// <summary>The rope's length, strain and segment count as of the last tick. False when this is not a rope.</summary>
    public bool TryGetRope(out RopeState state)
    {
        state = default;
        try
        {
            if (!SafeHas(this, RopeType)) return false;
            int st = GetInt("CSoftRope.outState");
            state = new RopeState(GetFloat("CSoftRope.outLengthCm"), GetFloat("CSoftRope.outStrain"), st >> 8, (st & 0x10) != 0, (st & 0x1) != 0);
            return true;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>The jelly's volume ratio, stretch and node count as of the last tick. False when this is not a jelly.</summary>
    public bool TryGetJelly(out JellyState state)
    {
        state = default;
        try
        {
            if (!SafeHas(this, VolumeType)) return false;
            int st = GetInt("CSoftVolume.outState");
            state = new JellyState(GetFloat("CSoftVolume.outVolumeRatio"), GetFloat("CSoftVolume.outMaxStretch"), GetInt("CSoftVolume.outNodes"),
                                   (st & 0x1) != 0, (st & 0x2) != 0, (st & 0x8) != 0 || (st & 0x4) != 0);
            return true;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>The inflation pressure of a closed cloth or a jelly, in kilopascal at its rest volume (0 deflates). False for an open mesh,
    /// a negative value or an entity with no such body.</summary>
    public bool SetSoftPressure(float kPa) => Relay(Handle, h => FwSoftShape.aver_fw_softshape_set_pressure(h, kPa));

    /// <summary>Rest and current volume of the soft body in cm^3 (the current one is negative when it is turned inside out).</summary>
    public bool TryGetSoftVolume(out float restCm3, out float nowCm3)
    {
        float[] v = new float[2];
        bool ok = Relay(Handle, h => FwSoftShape.aver_fw_softshape_volume(h, v));
        restCm3 = ok ? v[0] : 0f;
        nowCm3 = ok ? v[1] : 0f;
        return ok;
    }

    /// <summary>Grabs the vertex of this soft body (cloth, jelly or rope) nearest <paramref name="point"/> within <paramref name="maxDistanceCm"/>
    /// and carries it toward <paramref name="target"/> every physics step. Returns a grab id (the vertex index + 1) for
    /// <see cref="MoveSoftGrab"/> and <see cref="ReleaseSoftGrab"/>, 0 when nothing is in reach. Grabbing two vertices is two calls.</summary>
    public int GrabSoftVertex(Vec3 point, float maxDistanceCm, Vec3 target)
    {
        try { return Math.Max(0, FwSoftShape.aver_fw_softshape_grab(Handle, point.X, point.Y, point.Z, maxDistanceCm, target.X, target.Y, target.Z)); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
    }

    /// <summary>Moves a grab's target.</summary>
    public bool MoveSoftGrab(int grab, Vec3 target) => Relay(Handle, h => FwSoftShape.aver_fw_softshape_move_grab(h, grab, target.X, target.Y, target.Z));

    /// <summary>Lets the grabbed vertex go (it gets back the mass it had).</summary>
    public bool ReleaseSoftGrab(int grab) => Relay(Handle, h => FwSoftShape.aver_fw_softshape_release_grab(h, grab));

    /// <summary>Makes the soft body float in the world's water: hydrostatic pressure on a closed body plus quadratic drag. 0 density = off.</summary>
    public bool SetSoftBuoyancy(float fluidDensityKgM3, float drag = 1f) => Relay(Handle, h => FwSoftShape.aver_fw_softshape_set_buoyancy(h, fluidDensityKgM3, drag));

    /// <summary>How much of the soft body was under water on the last step (0..1) and the displaced volume in cm^3.</summary>
    public bool TryGetSoftSubmerged(out float fraction, out float volumeCm3)
    {
        float[] v = new float[2];
        bool ok = Relay(Handle, h => FwSoftShape.aver_fw_softshape_submerged(h, v));
        fraction = ok ? v[0] : 0f;
        volumeCm3 = ok ? v[1] : 0f;
        return ok;
    }

    /// <summary>How the soft body trades momentum with rigid bodies: 0 = infinite mass, 1 = unchanged. A soft scale of 0 pins it against
    /// anything it touches; an other scale of 0 lets a body land on it without being slowed.</summary>
    public bool SetSoftContact(float softScale, float otherScale) => Relay(Handle, h => FwSoftShape.aver_fw_softshape_set_contact(h, softScale, otherScale));

    /// <summary>What happens between this soft body and one other (a rigid body, or another soft body): normal, sensor events only, or ignored.</summary>
    public bool SetSoftPairMode(Entity other, SoftContactMode mode) => Relay(Handle, h => FwSoftShape.aver_fw_softshape_set_pair_mode(h, other.Handle, (int)mode));
}

/// <summary>Ropes and cables at world level.</summary>
public static class Ropes
{
    /// <summary>Creates a rope entity between two world points, both ends pinned. <paramref name="lengthCm"/> 0 = the distance times the asset's slack.
    /// Returns <see cref="Entity.None"/> when the host has no soft shape system.</summary>
    public static Entity Create(Vec3 start, Vec3 end, float lengthCm = 0f, float radiusCm = 0f, string asset = "")
    {
        int made;
        try { made = FwSoftShape.aver_fw_softshape_create_rope(start.X, start.Y, start.Z, end.X, end.Y, end.Z, Math.Max(0f, lengthCm), Math.Max(0f, radiusCm)); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return default; }
        if (made <= 0) return default;
        var rope = new Entity(made);
        if (!string.IsNullOrEmpty(asset)) rope.SetInt64("CSoftRope.asset", Assets.ObjectIdOf(asset));
        return rope;
    }
}

/// <summary>World-level soft shape events.</summary>
public static class SoftShapes
{
    /// <summary>Moves the soft bodies' sensor enter / exit events of the last tick into <paramref name="buffer"/> and returns how many. Pairs set
    /// to <see cref="SoftContactMode.Sensor"/> and Jolt sensor bodies raise them; they also reach graphs as the named events
    /// SoftBodySensorEntered and SoftBodySensorExited at the soft body's entity.</summary>
    public static int DrainSensorEvents(Span<SoftSensorEvent> buffer)
    {
        if (buffer.Length == 0) return 0;
        int max = buffer.Length;
        int[] ints = new int[max * 3];
        int n;
        try { n = FwSoftShape.aver_fw_softshape_drain_sensor_events(ints, max); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
        for (int k = 0; k < n; k++)
            buffer[k] = new SoftSensorEvent(new Entity(ints[k * 3]), new Entity(ints[k * 3 + 1]), ints[k * 3 + 2] != 0);
        return n;
    }
}

/// <summary>The graph-node surface for the wave 11 soft-body rows (SoftBody family version 3): the same calls with the int-entity /
/// primitive-only signatures GraphCompiler's emitted IL needs. Forwarded from the node table (GraphSoftBodyNodeTable.cs).</summary>
internal static partial class SoftBodyGraphInterop
{
    internal static bool SetSoftPressureForGraph(int entity, float kPa) => new Entity(entity).SetSoftPressure(kPa);

    internal static bool GetSoftVolumeForGraph(int entity, out float restCm3, out float nowCm3, out float ratio)
    {
        bool ok = new Entity(entity).TryGetSoftVolume(out restCm3, out nowCm3);
        ratio = ok && restCm3 > 0f ? nowCm3 / restCm3 : 0f;
        return ok;
    }

    /// <summary>Success means a vertex was in reach; <paramref name="grab"/> is its id.</summary>
    internal static bool GrabSoftBodyForGraph(int entity, float x, float y, float z, float reach, float targetX, float targetY, float targetZ, out int grab)
    {
        grab = new Entity(entity).GrabSoftVertex(new Vec3(x, y, z), reach, new Vec3(targetX, targetY, targetZ));
        return grab > 0;
    }

    internal static bool MoveSoftGrabForGraph(int entity, int grab, float x, float y, float z) => new Entity(entity).MoveSoftGrab(grab, new Vec3(x, y, z));

    internal static bool ReleaseSoftGrabForGraph(int entity, int grab) => new Entity(entity).ReleaseSoftGrab(grab);

    internal static bool SetSoftBuoyancyForGraph(int entity, float density, float drag) => new Entity(entity).SetSoftBuoyancy(density, drag);

    internal static bool GetSoftSubmergedForGraph(int entity, out float fraction, out float volumeCm3) =>
        new Entity(entity).TryGetSoftSubmerged(out fraction, out volumeCm3);

    internal static bool SetSoftContactForGraph(int entity, float softScale, float otherScale) => new Entity(entity).SetSoftContact(softScale, otherScale);

    internal static bool SetSoftPairModeForGraph(int entity, int other, int mode) =>
        mode >= 0 && mode <= 2 && new Entity(entity).SetSoftPairMode(new Entity(other), (SoftContactMode)mode);

    internal static bool AddSoftJellyForGraph(int entity, int preset) => new Entity(entity).AddJelly("", (SoftPreset)preset);
}

/// <summary>The graph-node surface for the Rope family (GraphRopeNodeTable.cs). A rope crosses the graph as its entity id, never a Jolt handle.</summary>
internal static class RopeGraphInterop
{
    /// <summary>Success means the rope entity exists; length and radius 0 take the asset's.</summary>
    internal static bool CreateRopeForGraph(float x1, float y1, float z1, float x2, float y2, float z2, float length, float radius, string asset, out int entity)
    {
        Entity e = Ropes.Create(new Vec3(x1, y1, z1), new Vec3(x2, y2, z2), length, radius, asset ?? string.Empty);
        entity = e.Handle;
        return e.IsValid;
    }

    internal static bool SetRopeEndForGraph(int rope, int end, int anchor, float ox, float oy, float oz) =>
        end >= 0 && end <= 1 && new Entity(rope).SetRopeEnd((RopeEnd)end, new Entity(anchor), new Vec3(ox, oy, oz));

    internal static bool FreeRopeEndForGraph(int rope, int end, float x, float y, float z, bool pinned) =>
        end >= 0 && end <= 1 && new Entity(rope).SetRopeEndFree((RopeEnd)end, new Vec3(x, y, z), pinned);

    internal static bool CutRopeForGraph(int rope, float x, float y, float z, out int other)
    {
        bool ok = new Entity(rope).CutRope(new Vec3(x, y, z), out Entity o);
        other = ok ? o.Handle : 0;
        return ok;
    }

    internal static bool GetRopeForGraph(int rope, out float length, out float strain, out int nodes, out bool cut)
    {
        bool ok = new Entity(rope).TryGetRope(out RopeState s);
        length = s.LengthCm;
        strain = s.Strain;
        nodes = s.Segments > 0 ? s.Segments + 1 : 0;
        cut = s.Cut;
        return ok;
    }

    /// <summary>Destroys the rope's entity (its body goes with it). Success means it was a rope.</summary>
    internal static bool DestroyRopeForGraph(int rope)
    {
        var e = new Entity(rope);
        if (!e.TryGetRope(out _)) return false;
        e.Destroy();
        return true;
    }
}
