// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// A ragdoll: capsule parts joined by swing-twist limits, built from a skeleton's world pose and then
// animated (Kinematic), left to fall (Limp) or pulled toward a pose by motors (Powered). See
// physics_ragdoll_abi.h for the contract: poses are per bone and in world space (centimetres, engine
// axes), bones are ordered parents first, and only a bone with a capsule radius > 0 gets a body --
// the rest ride rigidly on their nearest ancestor that has one.

using System;

namespace Aver.Physics;

/// <summary>What drives a ragdoll's parts. Values match AVER_PHYS_RAGDOLL_* (physics_ragdoll_abi.h).</summary>
public enum RagdollMode
{
    /// <summary>Parts follow the target pose; joints are off. A new ragdoll starts here.</summary>
    Kinematic = 0,
    /// <summary>Simulated, nothing drives it: it falls and folds at its limits.</summary>
    Limp = 1,
    /// <summary>Simulated, with each joint's motor pulling toward the target pose.</summary>
    Powered = 2,
}

/// <summary>One bone of the skeleton a ragdoll is built from. The capsule segment is in the BONE's local
/// frame; a radius of 0 or less means the bone has no part. The three angles are the half-angles of the
/// joint to the nearest ancestor part and are ignored for a bone without a part and for a root part.</summary>
public readonly struct RagdollBone
{
    public int Parent { get; }
    public Float3 CapsuleA { get; }
    public Float3 CapsuleB { get; }
    public float RadiusCm { get; }
    public float Swing1Deg { get; }
    public float Swing2Deg { get; }
    public float TwistDeg { get; }

    public RagdollBone(int parent, Float3 capsuleA, Float3 capsuleB, float radiusCm,
                       float swing1Deg, float swing2Deg, float twistDeg)
    {
        Parent = parent;
        CapsuleA = capsuleA;
        CapsuleB = capsuleB;
        RadiusCm = radiusCm;
        Swing1Deg = swing1Deg;
        Swing2Deg = swing2Deg;
        TwistDeg = twistDeg;
    }

    /// <summary>A bone with no part of its own: it rides on the nearest ancestor that has one.</summary>
    public static RagdollBone Rider(int parent) => new(parent, Float3.Zero, Float3.Zero, 0f, 0f, 0f, 0f);
}

/// <summary>A ragdoll in the simulation. A handle, not an object: 0 is invalid.</summary>
public readonly struct Ragdoll : IEquatable<Ragdoll>
{
    public int Handle { get; }
    public Ragdoll(int handle) { Handle = handle; }

    public static Ragdoll None => new(0);
    public bool IsValid => Handle != 0;

    /// <summary>How many ragdolls are live.</summary>
    public static int Count => Native.aver_phys_ragdoll_count();

    /// <summary>Builds a ragdoll in the given pose, in <see cref="RagdollMode.Kinematic"/> with that pose
    /// as its target. <paramref name="totalMassKg"/> is shared between the parts by capsule volume and
    /// every part sits on collision layer <paramref name="userLayer"/> (0..15). Returns
    /// <see cref="None"/> if the world is not running, a value is not finite, a parent does not precede
    /// its child, no bone has a part, or the body limit is reached.</summary>
    public static Ragdoll Create(RagdollBone[] bones, Float3[] positions, Quaternion[] rotations,
                                 float totalMassKg, uint userLayer = 0)
    {
        int n = bones.Length;
        if (positions.Length != n || rotations.Length != n)
            throw new ArgumentException("positions and rotations must each have one element per bone.");
        var parents = new int[n];
        var capsules = new float[7 * n];
        var limits = new float[3 * n];
        for (int i = 0; i < n; i++)
        {
            var b = bones[i];
            parents[i] = b.Parent;
            capsules[7 * i + 0] = b.CapsuleA.X; capsules[7 * i + 1] = b.CapsuleA.Y; capsules[7 * i + 2] = b.CapsuleA.Z;
            capsules[7 * i + 3] = b.CapsuleB.X; capsules[7 * i + 4] = b.CapsuleB.Y; capsules[7 * i + 5] = b.CapsuleB.Z;
            capsules[7 * i + 6] = b.RadiusCm;
            limits[3 * i + 0] = b.Swing1Deg; limits[3 * i + 1] = b.Swing2Deg; limits[3 * i + 2] = b.TwistDeg;
        }
        Pack(positions, rotations, out var pos, out var rot);
        return new(Native.aver_phys_ragdoll_create(n, parents, pos, rot, capsules, limits, totalMassKg, userLayer));
    }

    /// <summary>Removes the ragdoll and all of its parts. False for a dead handle.</summary>
    public bool Destroy() => Native.aver_phys_ragdoll_destroy(Handle) != 0;

    /// <summary>The bone count it was created with (not the part count); 0 for a dead handle.</summary>
    public int BoneCount => Native.aver_phys_ragdoll_bone_count(Handle);

    /// <summary>What drives the parts. Reads Kinematic for a dead handle; setting it on one does nothing.</summary>
    public RagdollMode Mode
    {
        get { int m = Native.aver_phys_ragdoll_mode(Handle); return m < 0 ? RagdollMode.Kinematic : (RagdollMode)m; }
        set => Native.aver_phys_ragdoll_set_mode(Handle, (int)value);
    }

    /// <summary>How hard <see cref="RagdollMode.Powered"/> pulls toward the target, 0 (none) to 1 (stiff);
    /// default 0.5. Relative to what each joint must carry, so it means the same for any size.</summary>
    public bool SetStrength(float strength) => Native.aver_phys_ragdoll_set_strength(Handle, strength) != 0;

    /// <summary>The pose Kinematic and Powered modes aim for, one element per bone, read from the next
    /// step. Also fixes where a bone WITHOUT a part sits relative to the part it rides on.</summary>
    public bool SetTarget(Float3[] positions, Quaternion[] rotations)
    {
        CheckPose(positions, rotations);
        Pack(positions, rotations, out var pos, out var rot);
        return Native.aver_phys_ragdoll_set_target(Handle, pos, rot) != 0;
    }

    /// <summary>Teleports every part to the pose and zeroes its velocity. The pose also becomes the target.</summary>
    public bool SetPose(Float3[] positions, Quaternion[] rotations)
    {
        CheckPose(positions, rotations);
        Pack(positions, rotations, out var pos, out var rot);
        return Native.aver_phys_ragdoll_set_pose(Handle, pos, rot) != 0;
    }

    /// <summary>Reads the current pose into the arrays (one element per bone each). A bone without a part
    /// is its ancestor part's transform times the target pose's offset between the two.</summary>
    public bool GetPose(Float3[] positions, Quaternion[] rotations)
    {
        CheckPose(positions, rotations);
        int n = positions.Length;
        var pos = new float[3 * n];
        var rot = new float[4 * n];
        if (Native.aver_phys_ragdoll_get_pose(Handle, pos, rot) == 0) return false;
        for (int i = 0; i < n; i++)
        {
            positions[i] = new Float3(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
            rotations[i] = new Quaternion(rot[4 * i], rot[4 * i + 1], rot[4 * i + 2], rot[4 * i + 3]);
        }
        return true;
    }

    /// <summary>An impulse (kg*cm/s) at a world point (cm) on the bone's part, or on its nearest ancestor
    /// part. False in Kinematic mode, for a bone with no part above it, or a dead handle.</summary>
    public bool AddImpulse(int bone, Float3 impulse, Float3 worldPoint) =>
        Native.aver_phys_ragdoll_add_impulse(Handle, bone, impulse.X, impulse.Y, impulse.Z, worldPoint.X, worldPoint.Y, worldPoint.Z) != 0;

    /// <summary>Powered strength of one bone's joint (the part the bone is or rides on), 0..1; negative goes back to
    /// the ragdoll-wide <see cref="SetStrength"/>. False for a dead handle, a bad bone or a bone with no part.</summary>
    public bool SetPartStrength(int bone, float strength) => Native.aver_phys_ragdoll_set_part_strength(Handle, bone, strength) != 0;

    /// <summary>Motor damping ratio of one bone's joint (default 1); negative goes back to the default.</summary>
    public bool SetPartDamping(int bone, float damping) => Native.aver_phys_ragdoll_set_part_damping(Handle, bone, damping) != 0;

    /// <summary>Multiplies the torque ceiling of one bone's motor (default 1).</summary>
    public bool SetPartTorqueScale(int bone, float scale) => Native.aver_phys_ragdoll_set_part_torque_scale(Handle, bone, scale) != 0;

    /// <summary>PARTIAL-BODY SIMULATION: a pinned part stays kinematic in Limp and Powered mode (carried to its target
    /// every step) while the unpinned parts simulate and stay joined to it. Pin everything outside a region and only
    /// the region is simulated.</summary>
    public bool SetPartPinned(int bone, bool pinned) => Native.aver_phys_ragdoll_set_part_pinned(Handle, bone, pinned ? 1 : 0) != 0;

    /// <summary>True if the bone's part is pinned; false if not, or for a dead handle, a bad bone or a bone with no part.</summary>
    public bool IsPartPinned(int bone) => Native.aver_phys_ragdoll_part_pinned(Handle, bone) == 1;

    /// <summary>The body of the bone's part, or of the ancestor part it rides on; <see cref="Body.None"/>
    /// if it has neither.</summary>
    public Body BodyOfBone(int bone) => new(Native.aver_phys_ragdoll_body_of_bone(Handle, bone));

    /// <summary>The ragdoll that owns <paramref name="body"/> as a part (<see cref="None"/> if none) and
    /// the bone of that part (-1 if none).</summary>
    public static Ragdoll OfBody(Body body, out int bone)
    {
        int h = Native.aver_phys_ragdoll_of_body(body.Handle, out bone);
        return new(h);
    }

    public bool Equals(Ragdoll other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is Ragdoll r && Equals(r);
    public override int GetHashCode() => Handle;
    public static bool operator ==(Ragdoll a, Ragdoll b) => a.Handle == b.Handle;
    public static bool operator !=(Ragdoll a, Ragdoll b) => a.Handle != b.Handle;

    private static void CheckPose(Float3[] positions, Quaternion[] rotations)
    {
        if (positions.Length != rotations.Length)
            throw new ArgumentException("positions and rotations must have the same length, one element per bone.");
    }

    private static void Pack(Float3[] positions, Quaternion[] rotations, out float[] pos, out float[] rot)
    {
        pos = new float[3 * positions.Length];
        rot = new float[4 * rotations.Length];
        for (int i = 0; i < positions.Length; i++)
        {
            pos[3 * i] = positions[i].X; pos[3 * i + 1] = positions[i].Y; pos[3 * i + 2] = positions[i].Z;
            rot[4 * i] = rotations[i].X; rot[4 * i + 1] = rotations[i].Y; rot[4 * i + 2] = rotations[i].Z; rot[4 * i + 3] = rotations[i].W;
        }
    }
}
