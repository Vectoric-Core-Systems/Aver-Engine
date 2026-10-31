// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Bone poses from a script: read where every bone of an animated entity ended up and set bones to a pose. Design,
// the order against the control rig, the IK rig and the ragdoll, and the limits: docs/ANIM_POSE_SEAM.md.
//
// RELAYED through the framework like the IK calls are (aver_fw_pose_*): the framework links neither the animation
// module nor the scene's pose system, so a host that installed no provider answers false / 0 / null from every call
// here rather than throwing. Overrides are TRANSIENT (not saved): a script sets them again after a load.

using Aver.Scene;
using System.Text;

namespace Aver.Framework;

/// <summary>A bone's transform: engine units (centimetres, the skeleton's own axes).</summary>
public readonly struct BonePose
{
    public Vec3 Position { get; }
    public Quat Rotation { get; }
    public Vec3 Scale { get; }

    public BonePose(Vec3 position, Quat rotation, Vec3 scale) { Position = position; Rotation = rotation; Scale = scale; }

    public static BonePose Identity => new(Vec3.Zero, Quat.Identity, Vec3.One);

    internal float[] ToFloats() => new[] { Position.X, Position.Y, Position.Z, Rotation.X, Rotation.Y, Rotation.Z, Rotation.W, Scale.X, Scale.Y, Scale.Z };

    internal static BonePose FromFloats(float[] v) =>
        new(new Vec3(v[0], v[1], v[2]), new Quat(v[3], v[4], v[5], v[6]), new Vec3(v[7], v[8], v[9]));
}

/// <summary>Which channels of a bone a pose write sets; the others keep what the animation produced.</summary>
[Flags]
public enum BoneChannels
{
    Position = FwPose.Position,
    Rotation = FwPose.Rotation,
    Scale = FwPose.Scale,
    All = FwPose.All,
}

/// <summary>A bone of an entity's skeleton: its name, parent and rest transform (parent-local).</summary>
public readonly struct BoneInfo
{
    public int Index { get; }
    public string Name { get; }
    public int Parent { get; }
    public BonePose Rest { get; }

    public BoneInfo(int index, string name, int parent, BonePose rest) { Index = index; Name = name; Parent = parent; Rest = rest; }
}

public readonly partial struct Entity
{
    private static long PoseHash(string name) => Assets.ObjectIdOf(name);

    /// <summary>How many bones the entity's skeleton has once it has been posed (0 before the first animation tick, or with no skeleton).</summary>
    public int PosedBoneCount => FwPose.aver_fw_pose_bone_count(Handle);

    /// <summary>The bone with that name, or -1.</summary>
    public int FindBone(string name) => string.IsNullOrEmpty(name) ? -1 : FwPose.aver_fw_pose_find_bone(Handle, PoseHash(name));

    /// <summary>A bone's name, parent (-1 for a root) and rest transform (parent-local), or null for an index the skeleton does not have.</summary>
    public BoneInfo? GetBoneInfo(int bone)
    {
        var name = new byte[128];
        var rest = new float[10];
        if (bone < 0 || FwPose.aver_fw_pose_bone_info(Handle, bone, name, name.Length, out int parent, rest) == 0) return null;
        int len = System.Array.IndexOf(name, (byte)0);
        return new BoneInfo(bone, Encoding.UTF8.GetString(name, 0, len < 0 ? name.Length : len), parent, BonePose.FromFloats(rest));
    }

    /// <summary>The bone's pose after the whole chain of the last tick (animation, rig, IK, script overrides, ragdoll), parent-local, or null.
    /// <paramref name="beforeOverrides"/> gives the pose just before the script's overrides were applied.</summary>
    public BonePose? GetBonePose(int bone, bool beforeOverrides = false) => ReadBone(bone, beforeOverrides ? FwPose.ReadBeforeSeam : 0);

    /// <summary>The same in the skeleton's model space (the entity's own frame).</summary>
    public BonePose? GetBoneModelPose(int bone, bool beforeOverrides = false) =>
        ReadBone(bone, FwPose.ReadModel | (beforeOverrides ? FwPose.ReadBeforeSeam : 0));

    private BonePose? ReadBone(int bone, int flags)
    {
        var v = new float[10];
        return bone >= 0 && FwPose.aver_fw_pose_read(Handle, bone, flags, v) != 0 ? BonePose.FromFloats(v) : null;
    }

    /// <summary>Sets a bone's pose (parent-local) from the next animation tick on and until cleared, over the animation and the rig.
    /// <paramref name="amount"/> blends with the pose underneath (1 replaces it); <paramref name="channels"/> picks what to set;
    /// <paramref name="oneTick"/> drops the override after the tick that applied it. False for a bone the skeleton does not have.</summary>
    public bool SetBonePose(int bone, BonePose pose, float amount = 1f, BoneChannels channels = BoneChannels.All, bool oneTick = false) =>
        bone >= 0 && FwPose.aver_fw_pose_write(Handle, bone, (int)channels | (oneTick ? FwPose.Once : 0), pose.ToFloats(), amount) != 0;

    /// <summary>Sets a bone's pose in the skeleton's model space; its children keep their local transforms and follow.</summary>
    public bool SetBoneModelPose(int bone, BonePose pose, float amount = 1f, BoneChannels channels = BoneChannels.All, bool oneTick = false) =>
        bone >= 0 && FwPose.aver_fw_pose_write(Handle, bone, (int)channels | FwPose.Model | (oneTick ? FwPose.Once : 0), pose.ToFloats(), amount) != 0;

    /// <summary>Sets consecutive bones from <paramref name="firstBone"/> in one call (parent-local, every channel, amount 1).</summary>
    public bool SetBonePoses(int firstBone, IReadOnlyList<BonePose> poses)
    {
        if (firstBone < 0 || poses.Count == 0) return false;
        var v = new float[poses.Count * 10];
        for (int i = 0; i < poses.Count; i++) poses[i].ToFloats().CopyTo(v, i * 10);
        return FwPose.aver_fw_pose_write_range(Handle, firstBone, poses.Count, FwPose.All, v) != 0;
    }

    /// <summary>Drops one bone's override, or every bone's for -1.</summary>
    public bool ClearBonePose(int bone = -1) => FwPose.aver_fw_pose_clear(Handle, bone) != 0;

    /// <summary>The master weight of this entity's overrides (default 1).</summary>
    public bool SetBonePoseWeight(float weight) => FwPose.aver_fw_pose_set_weight(Handle, weight) != 0;

    /// <summary>Switches the entity's overrides off and on without clearing them.</summary>
    public bool SetBonePosesEnabled(bool enabled) => FwPose.aver_fw_pose_set_enabled(Handle, enabled ? 1 : 0) != 0;

    /// <summary>How many bones currently carry an override.</summary>
    public int BonePoseOverrideCount => FwPose.aver_fw_pose_override_count(Handle);
}
