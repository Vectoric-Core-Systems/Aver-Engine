// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Skeleton3D (Godot's bone hierarchy and its pose API), SkeletonModifier3D and SkeletonIK3D, BoneAttachment3D. The skeleton is a script-level
// model in Godot's space: bones come from the scene text (bones/N/...) or from AddBone, poses are kept here and animation bone tracks write
// them. It is NOT connected to a skinned mesh in the engine (the renderer's skeleton lives in the animation system's entities and has no
// pose-input seam for scripts); the documented seam for that is in docs/plans/fabric/G5_PARITY.md, A2. docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>A hierarchy of bones with rest and current poses (Godot's Skeleton3D).</summary>
public class Skeleton3D : Node3D
{
    public enum ModifierCallbackModeProcessEnum { Physics = 0, Idle = 1 }

    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName SkeletonUpdated = "skeleton_updated";
        public static readonly StringName PoseUpdated = "pose_updated";
        public static readonly StringName RestUpdated = "rest_updated";
        public static readonly StringName BoneEnabledChanged = "bone_enabled_changed";
        public static readonly StringName BoneListChanged = "bone_list_changed";
        public static readonly StringName ShowRestOnlyChanged = "show_rest_only_changed";
    }

    public event Action SkeletonUpdated { add => Connect(SignalName.SkeletonUpdated, Callable.From(value)); remove => Disconnect(SignalName.SkeletonUpdated, Callable.From(value)); }
    public event Action PoseUpdated { add => Connect(SignalName.PoseUpdated, Callable.From(value)); remove => Disconnect(SignalName.PoseUpdated, Callable.From(value)); }
    public event Action<long> BoneEnabledChanged { add => Connect(SignalName.BoneEnabledChanged, Callable.From(value)); remove => Disconnect(SignalName.BoneEnabledChanged, Callable.From(value)); }

    public const int NotificationUpdateSkeleton = 50;

    private sealed class Bone
    {
        public string Name = "";
        public int Parent = -1;
        public Transform3D Rest = Transform3D.Identity;
        public bool Enabled = true;
        public Vector3 Position;
        public Quaternion Rotation = Quaternion.Identity;
        public Vector3 Scale = Vector3.One;
        public Transform3D Override;
        public float OverrideAmount;
        public bool OverridePersistent;
        public Dictionary<string, Variant>? Meta;
        // Caches, rebuilt from the poses when a pose or the hierarchy changes.
        public Transform3D GlobalRest, Global, GlobalNoOverride;
    }

    private readonly List<Bone> _bones = new();
    private bool _dirtyRest = true, _dirtyPose = true;
    private bool _showRestOnly;
    private float _motionScale = 1f;

    public ModifierCallbackModeProcessEnum ModifierCallbackModeProcess { get; set; } = ModifierCallbackModeProcessEnum.Idle;

    internal override bool UsesInternalProcess => ModifierCallbackModeProcess == ModifierCallbackModeProcessEnum.Idle;
    internal override bool UsesInternalPhysicsProcess => ModifierCallbackModeProcess == ModifierCallbackModeProcessEnum.Physics;

    internal override void InternalProcess(double delta)
    {
        if (ModifierCallbackModeProcess == ModifierCallbackModeProcessEnum.Idle) AdvanceModifiers(delta);
    }

    internal override void InternalPhysicsProcess(double delta)
    {
        if (ModifierCallbackModeProcess == ModifierCallbackModeProcessEnum.Physics) AdvanceModifiers(delta);
    }

    // ---- the bone list ----

    public int GetBoneCount() => _bones.Count;

    /// <summary>Adds a bone with no parent and returns its index; -1 for an empty or taken name.</summary>
    public int AddBone(string name)
    {
        if (name.Length == 0 || FindBone(name) >= 0) return -1;
        _bones.Add(new Bone { Name = name });
        Dirty(true);
        EmitSignal(SignalName.BoneListChanged);
        return _bones.Count - 1;
    }

    public int FindBone(string name)
    {
        for (int i = 0; i < _bones.Count; i++) if (_bones[i].Name == name) return i;
        return -1;
    }

    public string GetBoneName(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Name : "";

    public void SetBoneName(int boneIdx, string name)
    {
        if (!Valid(boneIdx) || name.Length == 0 || (FindBone(name) is int other && other >= 0 && other != boneIdx)) return;
        _bones[boneIdx].Name = name;
    }

    public int GetBoneParent(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Parent : -1;

    /// <summary>Sets the parent of a bone (-1 for none). A parent after the bone in the list is allowed; a cycle is refused.</summary>
    public void SetBoneParent(int boneIdx, int parentIdx)
    {
        if (!Valid(boneIdx) || parentIdx >= _bones.Count || parentIdx == boneIdx) return;
        for (int p = parentIdx; p >= 0; p = _bones[p].Parent) if (p == boneIdx) return;
        _bones[boneIdx].Parent = parentIdx;
        Dirty(true);
    }

    public int[] GetParentlessBones() => Enumerable.Range(0, _bones.Count).Where(i => _bones[i].Parent < 0).ToArray();

    public int[] GetBoneChildren(int boneIdx) => Enumerable.Range(0, _bones.Count).Where(i => _bones[i].Parent == boneIdx).ToArray();

    public void ClearBones()
    {
        _bones.Clear();
        Dirty(true);
        EmitSignal(SignalName.BoneListChanged);
    }

    private bool Valid(int i) => i >= 0 && i < _bones.Count;

    // ---- rest ----

    public Transform3D GetBoneRest(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Rest : Transform3D.Identity;

    public void SetBoneRest(int boneIdx, Transform3D rest)
    {
        if (!Valid(boneIdx)) return;
        _bones[boneIdx].Rest = rest;
        Dirty(true);
        EmitSignal(SignalName.RestUpdated);
    }

    public Transform3D GetBoneGlobalRest(int boneIdx)
    {
        if (!Valid(boneIdx)) return Transform3D.Identity;
        Update();
        return _bones[boneIdx].GlobalRest;
    }

    /// <summary>Re-expresses every rest relative to its parent's, for a skeleton whose rests were given in skeleton space.</summary>
    public void LocalizeRests()
    {
        var globals = Enumerable.Range(0, _bones.Count).Select(GetBoneGlobalRest).ToArray();
        for (int i = 0; i < _bones.Count; i++)
        {
            int p = _bones[i].Parent;
            _bones[i].Rest = p < 0 ? globals[i] : globals[p].AffineInverse() * globals[i];
        }
        Dirty(true);
    }

    // ---- pose ----

    public Vector3 GetBonePosePosition(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Position : default;
    public Quaternion GetBonePoseRotation(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Rotation : Quaternion.Identity;
    public Vector3 GetBonePoseScale(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Scale : Vector3.One;

    public void SetBonePosePosition(int boneIdx, Vector3 position) { if (Valid(boneIdx)) { _bones[boneIdx].Position = position; Dirty(false); } }
    public void SetBonePoseRotation(int boneIdx, Quaternion rotation) { if (Valid(boneIdx)) { _bones[boneIdx].Rotation = rotation.Normalized(); Dirty(false); } }
    public void SetBonePoseScale(int boneIdx, Vector3 scale) { if (Valid(boneIdx)) { _bones[boneIdx].Scale = scale; Dirty(false); } }

    /// <summary>The pose relative to the parent: the rest-relative position, rotation and scale as a transform.</summary>
    public Transform3D GetBonePose(int boneIdx)
    {
        if (!Valid(boneIdx)) return Transform3D.Identity;
        Bone b = _bones[boneIdx];
        return new Transform3D(Basis.FromQuaternionScale(b.Rotation, b.Scale), b.Position);
    }

    public void SetBonePose(int boneIdx, Transform3D pose)
    {
        if (!Valid(boneIdx)) return;
        Bone b = _bones[boneIdx];
        b.Position = pose.Origin;
        b.Scale = pose.Basis.GetScale();
        b.Rotation = pose.Basis.Orthonormalized().GetRotationQuaternion();
        Dirty(false);
    }

    /// <summary>The pose in the skeleton's space (the parent's global pose times this bone's), with any override blended in.</summary>
    public Transform3D GetBoneGlobalPose(int boneIdx)
    {
        if (!Valid(boneIdx)) return Transform3D.Identity;
        Update();
        return _bones[boneIdx].Global;
    }

    /// <summary>The same without the overrides.</summary>
    public Transform3D GetBoneGlobalPoseNoOverride(int boneIdx)
    {
        if (!Valid(boneIdx)) return Transform3D.Identity;
        Update();
        return _bones[boneIdx].GlobalNoOverride;
    }

    /// <summary>Sets the pose so that the bone's global pose becomes <paramref name="pose"/>, given where its parent is now.</summary>
    public void SetBoneGlobalPose(int boneIdx, Transform3D pose)
    {
        if (!Valid(boneIdx)) return;
        int p = _bones[boneIdx].Parent;
        Transform3D local = p < 0 ? pose : GetBoneGlobalPoseNoOverride(p).AffineInverse() * pose;
        SetBonePose(boneIdx, local);
    }

    /// <summary>Makes a bone's global pose follow <paramref name="pose"/> by <paramref name="amount"/> (0..1) until cleared; with <paramref name="persistent"/>
    /// it stays across updates, otherwise it is dropped after the next modifier pass.</summary>
    public void SetBoneGlobalPoseOverride(int boneIdx, Transform3D pose, float amount, bool persistent = false)
    {
        if (!Valid(boneIdx)) return;
        Bone b = _bones[boneIdx];
        b.Override = pose;
        b.OverrideAmount = Math.Clamp(amount, 0f, 1f);
        b.OverridePersistent = persistent;
        Dirty(false);
    }

    public Transform3D GetBoneGlobalPoseOverride(int boneIdx) => Valid(boneIdx) ? _bones[boneIdx].Override : Transform3D.Identity;

    public void ClearBonesGlobalPoseOverride()
    {
        foreach (Bone b in _bones) { b.OverrideAmount = 0; b.OverridePersistent = false; }
        Dirty(false);
    }

    /// <summary>Puts a bone's pose back to its rest.</summary>
    public void ResetBonePose(int boneIdx)
    {
        if (!Valid(boneIdx)) return;
        Bone b = _bones[boneIdx];
        b.Position = b.Rest.Origin;
        b.Rotation = b.Rest.Basis.Orthonormalized().GetRotationQuaternion();
        b.Scale = b.Rest.Basis.GetScale();
        Dirty(false);
    }

    public void ResetBonePoses()
    {
        for (int i = 0; i < _bones.Count; i++) ResetBonePose(i);
    }

    public bool IsBoneEnabled(int boneIdx) => Valid(boneIdx) && _bones[boneIdx].Enabled;

    public void SetBoneEnabled(int boneIdx, bool enabled)
    {
        if (!Valid(boneIdx)) return;
        _bones[boneIdx].Enabled = enabled;
        Dirty(false);
        EmitSignal(SignalName.BoneEnabledChanged, boneIdx);
    }

    public bool ShowRestOnly
    {
        get => _showRestOnly;
        set { _showRestOnly = value; Dirty(false); EmitSignal(SignalName.ShowRestOnlyChanged); }
    }

    public float MotionScale { get => _motionScale; set => _motionScale = value; }

    public void SetBoneMeta(int boneIdx, StringName key, Variant value)
    {
        if (!Valid(boneIdx)) return;
        (_bones[boneIdx].Meta ??= new())[key.ToString()] = value;
    }

    public Variant GetBoneMeta(int boneIdx, StringName key) =>
        Valid(boneIdx) && _bones[boneIdx].Meta != null && _bones[boneIdx].Meta!.TryGetValue(key.ToString(), out Variant v) ? v : default;

    // ---- the caches ----

    private void Dirty(bool rest)
    {
        _dirtyPose = true;
        if (rest) _dirtyRest = true;
    }

    // A bone's parent may come later in the list, so the globals are resolved depth first.
    private void Update()
    {
        if (!_dirtyPose && !_dirtyRest) return;
        int n = _bones.Count;
        var doneRest = new bool[n];
        var donePose = new bool[n];
        for (int i = 0; i < n; i++) ResolveRest(i, doneRest);
        for (int i = 0; i < n; i++) ResolvePose(i, donePose);
        _dirtyPose = _dirtyRest = false;
    }

    private void ResolveRest(int i, bool[] done)
    {
        if (done[i]) return;
        done[i] = true;
        Bone b = _bones[i];
        if (b.Parent >= 0) { ResolveRest(b.Parent, done); b.GlobalRest = _bones[b.Parent].GlobalRest * b.Rest; }
        else b.GlobalRest = b.Rest;
    }

    private void ResolvePose(int i, bool[] done)
    {
        if (done[i]) return;
        done[i] = true;
        Bone b = _bones[i];
        Transform3D local = _showRestOnly || !b.Enabled ? b.Rest : new Transform3D(Basis.FromQuaternionScale(b.Rotation, b.Scale), b.Position);
        Transform3D global;
        if (b.Parent >= 0) { ResolvePose(b.Parent, done); global = _bones[b.Parent].Global * local; }
        else global = local;
        b.GlobalNoOverride = global;
        b.Global = b.OverrideAmount > 0 ? global.InterpolateWith(b.Override, b.OverrideAmount) : global;
    }

    /// <summary>Recomputes the global poses now (they are otherwise recomputed when read) and tells followers.</summary>
    public void ForceUpdateAllBoneTransforms()
    {
        Dirty(true);
        Update();
        EmitSignal(SignalName.PoseUpdated);
        EmitSignal(SignalName.SkeletonUpdated);
    }

    // ---- modifiers ----

    /// <summary>Runs the modifiers that are children of this skeleton, in order, then tells followers (BoneAttachment3D) the pose changed.</summary>
    public void AdvanceModifiers(double delta)
    {
        foreach (Node child in GetChildren())
            if (child is SkeletonModifier3D m && m.Active) m.ProcessModificationWithDelta(delta);
        for (int i = 0; i < _bones.Count; i++)
            if (!_bones[i].OverridePersistent) _bones[i].OverrideAmount = 0;
        Dirty(false);
        EmitSignal(SignalName.PoseUpdated);
        EmitSignal(SignalName.SkeletonUpdated);
    }

    internal void SnapshotPose(out (Vector3 P, Quaternion R, Vector3 S)[] poses)
    {
        poses = new (Vector3, Quaternion, Vector3)[_bones.Count];
        for (int i = 0; i < _bones.Count; i++) poses[i] = (_bones[i].Position, _bones[i].Rotation, _bones[i].Scale);
    }

    internal void BlendPoseWith((Vector3 P, Quaternion R, Vector3 S)[] before, float influence)
    {
        for (int i = 0; i < _bones.Count && i < before.Length; i++)
        {
            Bone b = _bones[i];
            b.Position = before[i].P.Lerp(b.Position, influence);
            b.Rotation = before[i].R.Slerp(b.Rotation, influence).Normalized();
            b.Scale = before[i].S.Lerp(b.Scale, influence);
        }
        Dirty(false);
    }

    // ---- bones as a scene stores them: bones/N/name, parent, rest, enabled, position, rotation, scale ----

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("bones/", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 3 || !int.TryParse(p[1], out int i) || i < 0 || i > 100000) return false;
        while (_bones.Count <= i) _bones.Add(new Bone { Name = "Bone" + _bones.Count });
        Bone b = _bones[i];
        switch (p[2])
        {
            case "name": b.Name = value.AsString(); break;
            case "parent": b.Parent = value.AsInt32(); break;
            case "rest": b.Rest = value.AsTransform3D(); break;
            case "enabled": b.Enabled = value.AsBool(); break;
            case "position": b.Position = value.AsVector3(); break;
            case "rotation": b.Rotation = value.AsQuaternion(); break;
            case "scale": b.Scale = value.AsVector3(); break;
            default: return false;
        }
        Dirty(true);
        return true;
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        if (!name.StartsWith("bones/", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 3 || !int.TryParse(p[1], out int i) || !Valid(i)) return false;
        Bone b = _bones[i];
        switch (p[2])
        {
            case "name": value = b.Name; return true;
            case "parent": value = b.Parent; return true;
            case "rest": value = b.Rest; return true;
            case "enabled": value = b.Enabled; return true;
            case "position": value = b.Position; return true;
            case "rotation": value = b.Rotation; return true;
            case "scale": value = b.Scale; return true;
        }
        return false;
    }
}

/// <summary>A node that edits a skeleton's pose every pass (Godot 4.3's SkeletonModifier3D). Put it under the Skeleton3D.</summary>
public class SkeletonModifier3D : Node3D
{
    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName ModificationProcessed = "modification_processed";
    }

    public event Action ModificationProcessed { add => Connect(SignalName.ModificationProcessed, Callable.From(value)); remove => Disconnect(SignalName.ModificationProcessed, Callable.From(value)); }

    /// <summary>When false the skeleton skips this modifier.</summary>
    public bool Active { get; set; } = true;

    /// <summary>How much of the modification is kept: 0 leaves the pose as it was, 1 keeps all of it.</summary>
    public float Influence { get; set; } = 1f;

    public Skeleton3D? GetSkeleton() => GetParent() as Skeleton3D;

    /// <summary>Overridden by modifiers: edit the skeleton's pose here.</summary>
    public virtual void _ProcessModification() { }

    internal virtual void ProcessModificationWithDelta(double delta)
    {
        Skeleton3D? skel = GetSkeleton();
        if (skel == null || Influence <= 0f) return;
        (Vector3 P, Quaternion R, Vector3 S)[] before = System.Array.Empty<(Vector3, Quaternion, Vector3)>();
        bool blend = Influence < 1f;
        if (blend) skel.SnapshotPose(out before);
        _ProcessModification();
        if (blend) skel.BlendPoseWith(before, Influence);
        EmitSignal(SignalName.ModificationProcessed);
    }
}

/// <summary>Reaches a bone chain towards a target (Godot's SkeletonIK3D): FABRIK on the chain from <see cref="RootBone"/> to <see cref="TipBone"/>,
/// then each bone turned to follow. Approximated against Godot's solver, which differs in how it distributes rotation.</summary>
public class SkeletonIK3D : SkeletonModifier3D
{
    public StringName RootBone { get; set; } = StringName.Empty;
    public StringName TipBone { get; set; } = StringName.Empty;

    /// <summary>The target in the skeleton's parent space... in global space, unless <see cref="TargetNode"/> names a node.</summary>
    public Transform3D Target { get; set; } = Transform3D.Identity;

    public NodePath TargetNode { get; set; } = new();
    public float MinDistance { get; set; } = 0.01f;
    public int MaxIterations { get; set; } = 10;
    public bool UseMagnet { get; set; }
    public Vector3 Magnet { get; set; }

    /// <summary>Godot 4.0's name of <see cref="SkeletonModifier3D.Influence"/>.</summary>
    public float Interpolation { get => Influence; set => Influence = value; }

    private bool _running;

    public SkeletonIK3D() { Active = false; }

    public void Start(bool oneTime = false)
    {
        Active = true;
        _running = true;
        if (oneTime) GetSkeleton()?.AdvanceModifiers(0);
    }

    public void Stop() { Active = false; _running = false; }

    public bool IsRunning() => _running && Active;

    public override void _ProcessModification()
    {
        Skeleton3D? skel = GetSkeleton();
        if (skel == null) return;
        int root = skel.FindBone(RootBone.ToString()), tip = skel.FindBone(TipBone.ToString());
        if (root < 0 || tip < 0) return;
        var chain = new List<int>();
        for (int b = tip; b >= 0; b = skel.GetBoneParent(b)) { chain.Add(b); if (b == root) break; }
        if (chain[^1] != root || chain.Count < 2) return;
        chain.Reverse();

        Transform3D target = Target;
        if (!TargetNode.IsEmpty && GetNodeOrNull(TargetNode) is Node3D tn) target = tn.GlobalTransform;
        Transform3D toSkeleton = skel.GlobalTransform.AffineInverse();
        Transform3D t = toSkeleton * target;

        int n = chain.Count;
        var orig = chain.Select(b => skel.GetBoneGlobalPoseNoOverride(b)).ToArray();
        var pos = orig.Select(o => o.Origin).ToArray();
        var len = new float[n - 1];
        for (int i = 0; i < n - 1; i++) len[i] = pos[i].DistanceTo(pos[i + 1]);
        Vector3 rootPos = pos[0];
        float total = len.Sum();
        Vector3 goal = t.Origin;
        if (rootPos.DistanceTo(goal) >= total)
        {
            Vector3 dir = rootPos.DirectionTo(goal);
            for (int i = 1; i < n; i++) pos[i] = pos[i - 1] + dir * len[i - 1];
        }
        else
        {
            for (int it = 0; it < Math.Max(1, MaxIterations) && pos[n - 1].DistanceTo(goal) > MinDistance; it++)
            {
                pos[n - 1] = goal;
                for (int i = n - 2; i >= 0; i--) pos[i] = pos[i + 1] + pos[i + 1].DirectionTo(pos[i]) * len[i];
                pos[0] = rootPos;
                for (int i = 1; i < n; i++) pos[i] = pos[i - 1] + pos[i - 1].DirectionTo(pos[i]) * len[i - 1];
            }
        }
        if (UseMagnet && n > 2)
        {
            // Pull the middle of the chain towards the magnet, keeping the lengths.
            Vector3 m = toSkeleton * Magnet;
            Vector3 axis = (pos[n - 1] - pos[0]).Normalized();
            for (int i = 1; i < n - 1; i++)
            {
                Vector3 off = pos[i] - (pos[0] + axis * (pos[i] - pos[0]).Dot(axis));
                Vector3 want = m - (pos[0] + axis * (m - pos[0]).Dot(axis));
                if (off.LengthSquared() > 1e-9f && want.LengthSquared() > 1e-9f)
                    pos[i] = pos[0] + axis * (pos[i] - pos[0]).Dot(axis) + want.Normalized() * off.Length();
            }
        }

        // Turn each bone by the shortest arc from where it pointed at its child to where the solved chain points.
        for (int i = 0; i < n; i++)
        {
            Transform3D g = orig[i];
            if (i < n - 1)
            {
                Vector3 from = (orig[i + 1].Origin - orig[i].Origin).Normalized(), to = (pos[i + 1] - pos[i]).Normalized();
                if (from.LengthSquared() > 1e-9f && to.LengthSquared() > 1e-9f && from.Dot(to) < 0.9999999f)
                    g.Basis = new Basis(new Quaternion(from, to)) * g.Basis;
            }
            else if (t.Basis.GetScale().LengthSquared() > 0) g.Basis = t.Basis.Orthonormalized() * Basis.FromScale(g.Basis.GetScale());
            g.Origin = pos[i];
            skel.SetBoneGlobalPose(chain[i], g);
        }
    }
}

/// <summary>Follows one bone of a skeleton (Godot's BoneAttachment3D): its global transform is the bone's global pose in the skeleton's space.</summary>
public class BoneAttachment3D : Node3D
{
    private string _boneName = "";
    private int _boneIdx = -1;

    public bool OverridePose { get; set; }
    public bool UseExternalSkeleton { get; set; }
    public NodePath ExternalSkeleton { get; set; } = new();

    public string BoneName
    {
        get => _boneName;
        set { _boneName = value; _boneIdx = -1; Follow(); }
    }

    public int BoneIdx
    {
        get => _boneIdx;
        set { _boneIdx = value; Skeleton3D? s = TheSkeleton(); if (s != null && value >= 0) _boneName = s.GetBoneName(value); Follow(); }
    }

    internal override bool UsesInternalProcess => true;

    private Skeleton3D? TheSkeleton() => UseExternalSkeleton && !ExternalSkeleton.IsEmpty ? GetNodeOrNull(ExternalSkeleton) as Skeleton3D : GetParent() as Skeleton3D;

    /// <summary>The skeleton it follows.</summary>
    public Skeleton3D? GetSkeleton() => TheSkeleton();

    internal override void InternalProcess(double delta) => Follow();

    /// <summary>Moves the node to the bone now.</summary>
    public void OnSkeletonUpdate() => Follow();

    private void Follow()
    {
        Skeleton3D? s = TheSkeleton();
        if (s == null) return;
        if (_boneIdx < 0 && _boneName.Length > 0) _boneIdx = s.FindBone(_boneName);
        if (_boneIdx < 0) return;
        Transform3D pose = s.GetBoneGlobalPose(_boneIdx);
        if (OverridePose) s.SetBoneGlobalPoseOverride(_boneIdx, GlobalTransformInSkeleton(s), 1f, true);
        else if (GetParent() == s) Transform = pose;
        else GlobalTransform = s.GlobalTransform * pose;
    }

    private Transform3D GlobalTransformInSkeleton(Skeleton3D s) => s.GlobalTransform.AffineInverse() * GlobalTransform;
}
