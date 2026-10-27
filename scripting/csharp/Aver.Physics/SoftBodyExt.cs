// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Thin wrapper over physics_softbody_ext_abi.h, in engine units (cm, kg, kPa, engine axes). Game scripts use Entity and SoftShapes in Aver.Framework.

using System;

namespace Aver.Physics;

/// <summary>Indices of the extra parameter array (AVER_PHYS_SBX_P_*, physics_softbody_ext_abi.h). NaN = leave as is.</summary>
public enum SoftBodyExtParam
{
    PressureKPa = 0,
    SkinEnabled = 1,
    SkinDistanceScale = 2,
    CollisionGroup = 3,
    SoftInvMassScale = 4,
    OtherInvMassScale = 5,
    OtherInvInertiaScale = 6,
    FluidDensity = 7,
    FluidDrag = 8,
    BuoyFlags = 9,
    /// <summary>How many extra parameters there are (AVER_PHYS_SBX_P_COUNT); not one.</summary>
    Count = 10,
}

/// <summary>What happens when a soft body meets one particular other body.</summary>
public enum SoftPairMode
{
    Normal = 0,
    /// <summary>No response; enter and exit events instead.</summary>
    Sensor = 1,
    /// <summary>The contact is rejected.</summary>
    Ignore = 2,
}

/// <summary>Where the water of a buoyant soft body comes from.</summary>
[Flags]
public enum BuoyFlags
{
    /// <summary>The host water sampler, else the global water plane.</summary>
    WorldWater = 1,
    /// <summary>The plane given to <see cref="SoftBodyHandle.SetWaterPlane"/>, which wins.</summary>
    OwnPlane = 2,
}

/// <summary>Long range attachments made from the closest pinned vertex.</summary>
public enum LraMode
{
    Off = 0,
    Euclidean = 1,
    Geodesic = 2,
}

/// <summary>The bend constraints Jolt's own generator makes.</summary>
public enum BendType
{
    None = 0,
    Distance = 1,
    Dihedral = 2,
}

/// <summary>Which staged items <see cref="SoftBodyHandle.Slots"/> maps to their final index.</summary>
public enum SoftSlotKind
{
    Volume = 0,
    Lra = 1,
    Rod = 2,
    RodPair = 3,
}

/// <summary>A soft body entering or leaving a sensor, as reported after a step.</summary>
public readonly record struct SoftBodySensorEvent(int SoftBody, int Sensor, bool Entered);

public sealed partial class SoftBodyBuilder
{
    private readonly float[] _extParams = NewExtParams();
    private bool _extDirty;

    private static float[] NewExtParams()
    {
        var p = new float[(int)SoftBodyExtParam.Count];
        Array.Fill(p, float.NaN);
        return p;
    }

    /// <summary>Tetrahedra (4 vertex indices each); the rest volume is measured at finish. Compliance 0 is incompressible.</summary>
    public bool Volumes(int[] tet4, float[]? compliance = null) =>
        _id != 0 && Native.aver_phys_sbx_builder_volumes(_id, tet4, compliance, tet4.Length / 4) != 0;

    /// <summary>Explicit long range attachments: {pinned vertex, free vertex} pairs and the longest allowed distance of each (cm).</summary>
    public bool Lra(int[] pairs2, float[] maxDistCm) =>
        _id != 0 && Native.aver_phys_sbx_builder_lra(_id, pairs2, maxDistCm, pairs2.Length / 2) != 0;

    /// <summary>A long range attachment from the closest pinned vertex to every free one. The multiplier is 1.01 for 1% slack.</summary>
    public bool LraAuto(LraMode mode, float distanceMultiplier = 1.0f) =>
        _id != 0 && Native.aver_phys_sbx_builder_lra_auto(_id, (int)mode, distanceMultiplier) != 0;

    /// <summary>Cosserat rods (2 vertices each) and the pairs of rods (indices within this call) joined by a bend-twist constraint. A rod no pair names is refused.</summary>
    public bool Rods(int[] ab, float[]? stretch, int[] rodPairs2, float[]? bendTwist) =>
        _id != 0 && Native.aver_phys_sbx_builder_rods(_id, ab, stretch, ab.Length / 2, rodPairs2, bendTwist, rodPairs2.Length / 2) != 0;

    /// <summary>A chain of rods through vertices firstVertex .. firstVertex + vertexCount - 1 (at least 3).</summary>
    public bool RodChain(int firstVertex, int vertexCount, float stretch = 0, float bendTwist = 0) =>
        _id != 0 && Native.aver_phys_sbx_builder_rod_chain(_id, firstVertex, vertexCount, stretch, bendTwist) != 0;

    /// <summary>Jolt's CreateConstraints over the staged faces. Five numbers per vertex attribute: compliance, shear compliance, bend compliance (negative = no bends), LRA mode, LRA multiplier. Refused with staged edges or bends.</summary>
    public bool Auto(float[] attrs5, BendType bend, float angleToleranceDeg = 8) =>
        _id != 0 && Native.aver_phys_sbx_builder_auto(_id, attrs5, attrs5.Length / 5, (int)bend, angleToleranceDeg) != 0;

    public void Set(SoftBodyExtParam param, float value)
    {
        _extParams[(int)param] = value;
        _extDirty = true;
    }

    private void FlushExtension()
    {
        if (_extDirty && _id != 0) Native.aver_phys_sbx_builder_params(_id, _extParams, _extParams.Length);
    }
}

public readonly partial struct SoftBodyHandle
{
    /// <summary>The version of the soft-body extension surface.</summary>
    public static int ExtAbiVersion => Native.aver_phys_sbx_abi_version();

    /// <summary>Pressure in kPa at rest volume; 0 = off. False for an open or inside-out mesh.</summary>
    public bool SetPressure(float kPa) => Native.aver_phys_sbx_set_pressure(Handle, kPa) != 0;

    public float Pressure
    {
        get
        {
            Native.aver_phys_sbx_get_pressure(Handle, out float kPa);
            return kPa;
        }
    }

    /// <summary>Rest and current volume (cm^3); the current one is negative when the shape is inside out.</summary>
    public (float AtRest, float Now) Volume()
    {
        Native.aver_phys_sbx_volume(Handle, out float rest, out float now);
        return (rest, now);
    }

    /// <summary>Skin constraints on or off, and their max distance multiplier (0 = hard skin).</summary>
    public bool SetSkin(bool enabled, float distanceScale = 1) => Native.aver_phys_sbx_set_skin(Handle, enabled ? 1 : 0, distanceScale) != 0;

    /// <summary>Per-vertex inverse mass (0 = pinned). Updates the rods that touch the vertices.</summary>
    public bool SetInvMass(ReadOnlySpan<int> vertices, ReadOnlySpan<float> invMass) =>
        vertices.Length == invMass.Length && Native.aver_phys_sbx_set_inv_mass(Handle, vertices.ToArray(), invMass.ToArray(), vertices.Length) != 0;

    /// <summary>Carries a vertex to a world point each fixed step (grab, anchor). One way: the point feels nothing.</summary>
    public bool Attach(int vertex, Float3 target, float maxSpeedCmS = 2000) =>
        Native.aver_phys_sbx_attach(Handle, vertex, target.ToArray(), maxSpeedCmS) != 0;

    /// <summary>Carries a vertex to a point of another body (offset in that body's own frame).</summary>
    public bool AttachToBody(int vertex, int otherBody, Float3 localOffset, float maxSpeedCmS = 2000) =>
        Native.aver_phys_sbx_attach_to_body(Handle, vertex, otherBody, localOffset.ToArray(), maxSpeedCmS) != 0;

    public bool MoveAttachment(int vertex, Float3 target) => Native.aver_phys_sbx_move_attachment(Handle, vertex, target.ToArray()) != 0;

    /// <summary>Lets a vertex go. A negative restore mass gives it back what it had before it was attached.</summary>
    public bool Detach(int vertex, float restoreInvMass = -1) => Native.aver_phys_sbx_detach(Handle, vertex, restoreInvMass) != 0;

    public int AttachmentCount => Native.aver_phys_sbx_attachment_count(Handle);

    /// <summary>Inverse mass scales for every contact with a rigid body (0 = infinite mass, 1 = unchanged).</summary>
    public bool SetContactPolicy(float softScale, float otherScale, float otherInertiaScale) =>
        Native.aver_phys_sbx_set_contact_policy(Handle, softScale, otherScale, otherInertiaScale) != 0;

    public bool SetPairPolicy(int otherBody, SoftPairMode mode, float soft = 1, float other = 1, float inertia = 1) =>
        Native.aver_phys_sbx_set_pair_policy(Handle, otherBody, (int)mode, soft, other, inertia) != 0;

    public bool ClearPairPolicy(int otherBody) => Native.aver_phys_sbx_clear_pair_policy(Handle, otherBody) != 0;

    /// <summary>The soft body ignores every rigid body whose group id equals this (a ragdoll's handle). 0 = none.</summary>
    public bool SetCollisionGroup(int group) => Native.aver_phys_sbx_set_collision_group(Handle, group) != 0;

    /// <summary>Hydrostatic pressure on a closed mesh and quadratic drag on any mesh, against the world water or a plane of its own.</summary>
    public bool SetBuoyancy(float fluidDensity, float drag = 1, BuoyFlags flags = BuoyFlags.WorldWater) =>
        Native.aver_phys_sbx_set_buoyancy(Handle, fluidDensity, drag, (int)flags) != 0;

    /// <summary>The body's own water plane; it is used when <see cref="BuoyFlags.OwnPlane"/> is set.</summary>
    public bool SetWaterPlane(Float3 point, Float3 normal, Float3 current) =>
        Native.aver_phys_sbx_set_water_plane(Handle, point.ToArray(), normal.ToArray(), current.ToArray()) != 0;

    /// <summary>The last step's submerged fraction (0..1) and displaced volume (cm^3, closed meshes).</summary>
    public (float Fraction, float VolumeCm3) Submerged()
    {
        Native.aver_phys_sbx_submerged(Handle, out float fraction, out float volume);
        return (fraction, volume);
    }

    /// <summary>The final index of each staged item of a kind (Optimize reorders them).</summary>
    public int[] Slots(SoftSlotKind kind)
    {
        int count = Native.aver_phys_sbx_slots(Handle, (int)kind, null, 0);
        if (count <= 0) return Array.Empty<int>();
        var slots = new int[count];
        Native.aver_phys_sbx_slots(Handle, (int)kind, slots, count);
        return slots;
    }

    public int RodCount => Native.aver_phys_sbx_rod_count(Handle);

    /// <summary>World orientation of rods firstRod .. (engine +X rotated by it is the rod's direction) and their angular velocity (rad/s). Fills as many as both spans hold (an empty angular span is skipped). Returns the number written.</summary>
    public int RodState(int firstRod, Span<Quaternion> rotation, Span<Float3> angularVelocity)
    {
        int n = rotation.Length;
        if (angularVelocity.Length > 0) n = Math.Min(n, angularVelocity.Length);
        if (n <= 0) return 0;
        var q = new float[n * 4];
        var w = angularVelocity.Length > 0 ? new float[n * 3] : null;
        int written = Native.aver_phys_sbx_rod_state(Handle, firstRod, n, q, w);
        for (int i = 0; i < written; i++)
        {
            rotation[i] = new Quaternion(q[i * 4], q[i * 4 + 1], q[i * 4 + 2], q[i * 4 + 3]);
            if (w != null) angularVelocity[i] = new Float3(w[i * 3], w[i * 3 + 1], w[i * 3 + 2]);
        }
        return written;
    }

    /// <summary>Sensor enters and exits reported by the last <c>Physics.Step</c>.</summary>
    public static int DrainSensorEvents(Span<SoftBodySensorEvent> buffer)
    {
        int n = Math.Min(buffer.Length, Native.aver_phys_sbx_sensor_count());
        for (int i = 0; i < n; i++)
        {
            if (Native.aver_phys_sbx_sensor_get(i, out int soft, out int sensor, out int entered) == 0) return i;
            buffer[i] = new SoftBodySensorEvent(soft, sensor, entered != 0);
        }
        return n;
    }
}
