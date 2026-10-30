// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Node3D: a node with a 3D transform in Godot space. The transform lives in the entity once there is one; scripts see Godot numbers
// (metres, +Y up) and FabricSpace converts at the boundary.

using Aver.Scene;

namespace Aver.Fabric;

/// <summary>A node with a position, rotation and scale in 3D (Godot's Node3D).</summary>
public partial class Node3D : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName VisibilityChanged = "visibility_changed";
    }

    public new class PropertyName : Node.PropertyName
    {
        public static readonly StringName Position = "position";
        public static readonly StringName Rotation = "rotation";
        public static readonly StringName RotationDegrees = "rotation_degrees";
        public static readonly StringName Scale = "scale";
        public static readonly StringName Quaternion = "quaternion";
        public static readonly StringName Basis = "basis";
        public static readonly StringName Transform = "transform";
        public static readonly StringName GlobalPosition = "global_position";
        public static readonly StringName GlobalRotation = "global_rotation";
        public static readonly StringName GlobalRotationDegrees = "global_rotation_degrees";
        public static readonly StringName GlobalTransform = "global_transform";
        public static readonly StringName TopLevel = "top_level";
        public static readonly StringName Visible = "visible";
    }

    public const int NotificationTransformChanged = 2000;
    public const int NotificationEnterWorld = 41;
    public const int NotificationExitWorld = 42;
    public const int NotificationVisibilityChanged = 43;
    public const int NotificationLocalTransformChanged = 44;

    // The local transform as Godot sees it. With an entity behind the node, the entity is the truth and Sync() refreshes these
    // from it; the "pushed" values are the Aver numbers last written, so an unchanged entity returns the exact Godot numbers.
    private Vector3 _pos;
    private Vector3 _scale = Vector3.One;
    private Vector3 _euler;
    private Quaternion _quat = Quaternion.Identity;
    private EulerOrder _order = EulerOrder.Yxz;
    private Vec3 _pushedPos, _pushedScale;
    private Quat _pushedQuat;
    private bool _pushed;
    private bool _topLevel;
    private bool _visible = true;

    private bool Backed => Self.IsValid;

    // Refresh the cached Godot values from the entity if anything else moved it (physics, an animation, the editor).
    private void Sync()
    {
        if (!Backed) return;
        Vec3 p = Self.LocalPosition;
        Quat q = Self.LocalRotation;
        Vec3 s = Self.LocalScale;
        if (!_pushed || p != _pushedPos) { _pos = FabricSpace.FromAverPosition(p); _pushedPos = p; }
        if (!_pushed || !q.Equals(_pushedQuat)) { _quat = FabricSpace.FromAverRotation(q); _euler = Basis.FromQuaternionScale(_quat, Vector3.One).GetEuler(_order); _pushedQuat = q; }
        if (!_pushed || s != _pushedScale) { _scale = FabricSpace.FromAverScale(s); _pushedScale = s; }
        _pushed = true;
    }

    // Write the cached Godot values to the entity.
    private void Push()
    {
        if (!Backed) return;
        _pushedPos = FabricSpace.ToAverPosition(_pos);
        _pushedQuat = FabricSpace.ToAverRotation(_quat);
        _pushedScale = FabricSpace.ToAverScale(_scale);
        _pushed = true;
        Self.SetLocalPosition(_pushedPos);
        Self.SetLocalRotation(_pushedQuat);
        Self.SetLocalScale(_pushedScale);
        OnTransformPushed();
    }

    // A node type that follows its own transform (the sun) reacts here, after the entity holds the new local transform.
    internal virtual void OnTransformPushed() { }

    // The local transform in the form Fw.aver_fw_spawn takes.
    internal void GetLocalForEngine(float[] pos, float[] quat, float[] scale)
    {
        Vec3 p = FabricSpace.ToAverPosition(_pos);
        Quat q = FabricSpace.ToAverRotation(_quat);
        Vec3 s = FabricSpace.ToAverScale(_scale);
        pos[0] = p.X; pos[1] = p.Y; pos[2] = p.Z;
        quat[0] = q.X; quat[1] = q.Y; quat[2] = q.Z; quat[3] = q.W;
        scale[0] = s.X; scale[1] = s.Y; scale[2] = s.Z;
    }

    internal override void CopyEngineState(Node copy)
    {
        if (copy is not Node3D c) return;
        c.Transform = Transform;
        c._visible = _visible;
        c._topLevel = _topLevel;
        c._order = _order;
    }

    // ---- local transform ----

    /// <summary>The position relative to the parent, in metres.</summary>
    public Vector3 Position
    {
        get { Sync(); return _pos; }
        set { Sync(); _pos = value; Push(); }
    }

    /// <summary>The rotation as Euler angles in radians, in <see cref="RotationOrder"/> (Godot's default is YXZ).</summary>
    public Vector3 Rotation
    {
        get { Sync(); return _euler; }
        set
        {
            Sync();
            _euler = value;
            _quat = Basis.FromEuler(value, _order).GetRotationQuaternion();
            Push();
        }
    }

    /// <summary>The rotation in degrees.</summary>
    public Vector3 RotationDegrees
    {
        get => Rotation * (180f / Mathf.Pi);
        set => Rotation = value * (Mathf.Pi / 180f);
    }

    public EulerOrder RotationOrder
    {
        get => _order;
        set { Sync(); _order = value; _euler = new Basis(_quat).GetEuler(_order); }
    }

    public Vector3 Scale
    {
        get { Sync(); return _scale; }
        set { Sync(); _scale = value; Push(); }
    }

    public Quaternion Quaternion
    {
        get { Sync(); return _quat; }
        set { Sync(); _quat = value.Normalized(); _euler = new Basis(_quat).GetEuler(_order); Push(); }
    }

    /// <summary>The rotation and scale, as a Basis (rotation times scale).</summary>
    public Basis Basis
    {
        get { Sync(); return Basis.FromQuaternionScale(_quat, _scale); }
        set
        {
            Sync();
            _scale = value.GetScale();
            _quat = value.Orthonormalized().GetRotationQuaternion();
            _euler = new Basis(_quat).GetEuler(_order);
            Push();
        }
    }

    public Transform3D Transform
    {
        get { Sync(); return new Transform3D(Basis.FromQuaternionScale(_quat, _scale), _pos); }
        set
        {
            Sync();
            _pos = value.Origin;
            _scale = value.Basis.GetScale();
            _quat = value.Basis.Orthonormalized().GetRotationQuaternion();
            _euler = new Basis(_quat).GetEuler(_order);
            Push();
        }
    }

    // ---- global transform ----

    /// <summary>True: this node ignores its parent's transform.</summary>
    public bool TopLevel
    {
        get => _topLevel;
        set
        {
            if (_topLevel == value) return;
            // Inside the tree the node stays where it is: its local transform becomes the global one (or back, relative to the parent).
            Transform3D global = IsInsideTree() ? GlobalTransform : default;
            _topLevel = value;
            if (IsInsideTree()) GlobalTransform = global;
        }
    }

    /// <summary>Godot's set_as_top_level: the same as setting <see cref="TopLevel"/>.</summary>
    public void SetAsTopLevel(bool enable) => TopLevel = enable;

    public bool IsSetAsTopLevel() => _topLevel;

    private Node3D? ParentNode3D => _topLevel ? null : GetParent() as Node3D;

    public Transform3D GlobalTransform
    {
        get
        {
            Transform3D local = Transform;
            Node3D? p = ParentNode3D;
            return p == null ? local : p.GlobalTransform * local;
        }
        set
        {
            Node3D? p = ParentNode3D;
            Transform = p == null ? value : p.GlobalTransform.AffineInverse() * value;
        }
    }

    public Vector3 GlobalPosition
    {
        get => GlobalTransform.Origin;
        set { Transform3D t = GlobalTransform; t.Origin = value; GlobalTransform = t; }
    }

    public Vector3 GlobalRotation
    {
        get => GlobalTransform.Basis.Orthonormalized().GetEuler(_order);
        set
        {
            Transform3D t = GlobalTransform;
            t.Basis = Basis.FromEuler(value, _order) * Basis.FromScale(t.Basis.GetScale());
            GlobalTransform = t;
        }
    }

    public Vector3 GlobalRotationDegrees
    {
        get => GlobalRotation * (180f / Mathf.Pi);
        set => GlobalRotation = value * (Mathf.Pi / 180f);
    }

    public Basis GlobalBasis { get => GlobalTransform.Basis; set { Transform3D t = GlobalTransform; t.Basis = value; GlobalTransform = t; } }

    // ---- moving ----

    /// <summary>Turns the node so its -Z axis points at a global point, keeping its scale (Godot's look_at).</summary>
    public void LookAt(Vector3 target, Vector3? up = null, bool useModelFront = false) =>
        LookAtFromPosition(GlobalPosition, target, up, useModelFront);

    public void LookAtFromPosition(Vector3 position, Vector3 target, Vector3? up = null, bool useModelFront = false)
    {
        Vector3 scale = Scale;
        GlobalTransform = new Transform3D(Basis.LookingAt(target - position, up, useModelFront), position);
        Scale = scale;
    }

    /// <summary>Moves by an offset in this node's own axes.</summary>
    public void Translate(Vector3 offset) { Transform3D t = Transform; Transform = t.TranslatedLocal(offset); }
    public void TranslateObjectLocal(Vector3 offset) => Translate(offset);
    public void GlobalTranslate(Vector3 offset) { Transform3D t = GlobalTransform; t.Origin += offset; GlobalTransform = t; }

    /// <summary>Rotates about an axis given in the parent's space (the basis only; the position stays).</summary>
    public void Rotate(Vector3 axis, float angle) { Transform3D t = Transform; t.Basis = t.Basis.Rotated(axis, angle); Transform = t; }
    public void RotateX(float angle) => Rotate(new Vector3(1, 0, 0), angle);
    public void RotateY(float angle) => Rotate(new Vector3(0, 1, 0), angle);
    public void RotateZ(float angle) => Rotate(new Vector3(0, 0, 1), angle);

    /// <summary>Rotates about one of the node's own axes.</summary>
    public void RotateObjectLocal(Vector3 axis, float angle) { Transform3D t = Transform; t.Basis = t.Basis * new Basis(axis, angle); Transform = t; }

    public void GlobalRotate(Vector3 axis, float angle)
    {
        Transform3D t = GlobalTransform;
        t.Basis = new Basis(axis, angle) * t.Basis;
        GlobalTransform = t;
    }

    public void ScaleObjectLocal(Vector3 scale) { Transform3D t = Transform; t.Basis = t.Basis.ScaledLocal(scale); Transform = t; }

    public void Orthonormalize() { Transform3D t = Transform; t.Basis = t.Basis.Orthonormalized(); Transform = t; }
    public void SetIdentity() => Transform = Transform3D.Identity;

    /// <summary>A global point in this node's local space.</summary>
    public Vector3 ToLocal(Vector3 globalPoint) => GlobalTransform.AffineInverse() * globalPoint;

    /// <summary>A local point in global space.</summary>
    public Vector3 ToGlobal(Vector3 localPoint) => GlobalTransform * localPoint;

    public Node3D? GetParentNode3D() => ParentNode3D;

    /// <summary>The 3D world this node is in: its physics space for queries (<c>GetWorld3D().DirectSpaceState</c>).</summary>
    public World3D? GetWorld3D() => GetViewport()?.World3D;

    /// <summary>Asks for NOTIFICATION_TRANSFORM_CHANGED (sent once a frame when the transform moved).</summary>
    public void SetNotifyTransform(bool enable) => NotifyTransform = enable;
    public bool IsTransformNotificationEnabled() => NotifyTransform;
    public void SetNotifyLocalTransform(bool enable) { }
    public bool IsLocalTransformNotificationEnabled() => false;

    /// <inheritdoc/>
    public override void OnTransformChanged() => _Notification(NotificationTransformChanged);

    // ---- visibility ----

    /// <summary>Whether the node is shown (its own flag; a hidden ancestor hides it too, see <see cref="IsVisibleInTree"/>).</summary>
    public bool Visible
    {
        get => _visible;
        set
        {
            if (_visible == value) return;
            bool before = IsVisibleInTree();
            _visible = value;
            bool after = IsVisibleInTree();
            if (before != after) PropagateVisibility(after);
        }
    }

    public void Show() => Visible = true;
    public void Hide() => Visible = false;

    /// <summary>True when this node and every Node3D ancestor are visible and the node is in the tree.</summary>
    public bool IsVisibleInTree()
    {
        if (!IsInsideTree()) return false;
        for (Node3D? n = this; n != null; n = n.ParentNode3D)
            if (!n._visible) return false;
        return true;
    }

    private void PropagateVisibility(bool effective)
    {
        OnVisibilityApplied(effective);
        _Notification(NotificationVisibilityChanged);
        EmitSignal(SignalName.VisibilityChanged);
        foreach (Node c in ChildrenSnapshot())
            if (c is Node3D n3 && n3._visible) n3.PropagateVisibility(effective);
    }

    // A node type that draws something shows or hides it here.
    internal virtual void OnVisibilityApplied(bool effective) { }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        if (!_visible || ParentNode3D is { _visible: false }) OnVisibilityApplied(IsVisibleInTree());
    }

    // The values the node was spawned with are what the entity holds: reading them back returns the exact Godot numbers.
    internal void MarkSpawned()
    {
        _pushedPos = FabricSpace.ToAverPosition(_pos);
        _pushedQuat = FabricSpace.ToAverRotation(_quat);
        _pushedScale = FabricSpace.ToAverScale(_scale);
        _pushed = true;
    }
}
