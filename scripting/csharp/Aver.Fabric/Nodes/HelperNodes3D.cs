// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Small helper nodes of Godot's 3D set: SpringArm3D (children pulled in along +Z by a ray or shape cast), RemoteTransform3D (pushes this
// node's transform onto another node), VisibleOnScreenNotifier3D and VisibleOnScreenEnabler3D (screen_entered / screen_exited from the
// current camera's frustum and a box). Godot's space and units. docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>Moves its direct children along its +Z axis to the first thing a cast finds, up to <see cref="SpringLength"/> (Godot's SpringArm3D):
/// a third-person camera on a boom that shortens in tight spaces. Casts a ray, or <see cref="Shape"/> when one is set, every physics step.</summary>
public class SpringArm3D : Node3D
{
    private float _springLength = 1f, _margin = 0.01f, _hitLength;
    private readonly List<Rid> _excluded = new();

    public float SpringLength { get => _springLength; set { _springLength = value; Process(); } }

    /// <summary>How far in front of the hit the children stop.</summary>
    public float Margin { get => _margin; set { _margin = value; Process(); } }

    public uint CollisionMask { get; set; } = 1;

    /// <summary>A shape swept instead of a ray (a sphere keeps a camera from clipping the wall).</summary>
    public Shape3D? Shape { get; set; }

    public float GetHitLength() => _hitLength;

    public void AddExcludedObject(Rid rid) { if (rid.Id != 0 && !_excluded.Contains(rid)) _excluded.Add(rid); }

    public bool RemoveExcludedObject(Rid rid) => _excluded.Remove(rid);

    public void ClearExcludedObjects() => _excluded.Clear();

    /// <summary>A test seam: the distance along a global segment to the first hit (null for none), used instead of the physics space.</summary>
    internal Func<Vector3, Vector3, float?>? Probe;

    internal override bool UsesInternalPhysicsProcess => true;

    internal override void InternalPhysicsProcess(double delta) => Process();

    private void Process()
    {
        if (!IsInsideTree()) return;
        Transform3D g = GlobalTransform;
        Vector3 dir = g.Basis.Orthonormalized() * new Vector3(0, 0, 1);
        float fraction = 1f;
        if (_springLength > 0f)
        {
            Vector3 motion = dir * _springLength;
            if (Probe != null)
            {
                float? hit = Probe(g.Origin, g.Origin + motion);
                if (hit.HasValue) fraction = Math.Clamp(hit.Value / _springLength, 0f, 1f);
            }
            else if (GetWorld3D()?.DirectSpaceState is { } space)
            {
                if (Shape == null)
                {
                    var q = PhysicsRayQueryParameters3D.Create(g.Origin, g.Origin + motion, CollisionMask, new Collections.Array<Rid>(_excluded));
                    Collections.Dictionary r = space.IntersectRay(q);
                    if (r.Count > 0) fraction = r.Get("position").AsVector3().DistanceTo(g.Origin) / _springLength;
                }
                else
                {
                    var q = new PhysicsShapeQueryParameters3D { Shape = Shape, Transform = g, Motion = motion, CollisionMask = CollisionMask, Exclude = new Collections.Array<Rid>(_excluded) };
                    fraction = space.CastMotion(q)[0];
                }
            }
        }
        _hitLength = _springLength * fraction;
        var place = new Transform3D(Basis.Identity, g.Origin + dir * (_springLength * fraction - _margin));
        foreach (Node child in GetChildren())
        {
            if (child is not Node3D c) continue;
            place.Basis = c.GlobalTransform.Basis;
            c.GlobalTransform = place;
        }
    }

    /// <summary>Runs one pass now (for a script that moves the arm and wants the children placed before the next physics step).</summary>
    public void ForceUpdate() => Process();
}

/// <summary>Copies this node's transform to another node (Godot's RemoteTransform3D): its position, rotation and scale, in global or in local
/// terms, each switchable.</summary>
public class RemoteTransform3D : Node3D
{
    private NodePath _remote = new("..");
    private Node3D? _cache;
    private Transform3D _lastSource;
    private bool _haveLast;

    /// <summary>The node that follows (default: the parent).</summary>
    public NodePath RemotePath { get => _remote; set { _remote = value; ForceUpdateCache(); } }

    /// <summary>True: the global transform is copied; false: the local one.</summary>
    public bool UseGlobalCoordinates { get; set; } = true;

    public bool UpdatePosition { get; set; } = true;
    public bool UpdateRotation { get; set; } = true;
    public bool UpdateScale { get; set; } = true;

    /// <summary>Looks the remote node up again (after the scene changed) and copies the transform to it now.</summary>
    public void ForceUpdateCache()
    {
        _cache = null;
        _haveLast = false;
        Apply();
    }

    internal override bool UsesInternalProcess => true;

    internal override void InternalProcess(double delta) => Apply();

    private Node3D? Remote()
    {
        if (_cache != null && _cache.IsAlive && _cache.IsInsideTree()) return _cache;
        _cache = _remote.IsEmpty || !IsInsideTree() ? null : GetNodeOrNull(_remote) as Node3D;
        return _cache == this ? null : _cache;
    }

    internal void Apply()
    {
        Node3D? n = Remote();
        if (n == null) return;
        Transform3D src = UseGlobalCoordinates ? GlobalTransform : Transform;
        if (_haveLast && src.IsEqualApprox(_lastSource) && n.IsInsideTree()) { /* still copy: the remote may have moved */ }
        _lastSource = src;
        _haveLast = true;
        if (UpdatePosition && UpdateRotation && UpdateScale)
        {
            if (UseGlobalCoordinates) n.GlobalTransform = src; else n.Transform = src;
            return;
        }
        Transform3D dst = UseGlobalCoordinates ? n.GlobalTransform : n.Transform;
        Vector3 scale = dst.Basis.GetScale();
        Basis rot = dst.Basis.Orthonormalized();
        if (UpdateRotation) rot = src.Basis.Orthonormalized();
        if (UpdateScale) scale = src.Basis.GetScale();
        dst.Basis = rot * Basis.FromScale(scale);
        if (UpdatePosition) dst.Origin = src.Origin;
        if (UseGlobalCoordinates) n.GlobalTransform = dst; else n.Transform = dst;
    }
}

/// <summary>Reports when a box is on the current camera's screen (Godot's VisibleOnScreenNotifier3D): <c>screen_entered</c> and <c>screen_exited</c>.</summary>
public class VisibleOnScreenNotifier3D : Node3D
{
    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName ScreenEntered = "screen_entered";
        public static readonly StringName ScreenExited = "screen_exited";
    }

    public event Action ScreenEntered { add => Connect(SignalName.ScreenEntered, Callable.From(value)); remove => Disconnect(SignalName.ScreenEntered, Callable.From(value)); }
    public event Action ScreenExited { add => Connect(SignalName.ScreenExited, Callable.From(value)); remove => Disconnect(SignalName.ScreenExited, Callable.From(value)); }

    private bool _onScreen;

    /// <summary>The box, in the node's own space.</summary>
    public Aabb Aabb { get; set; } = new(new Vector3(-1, -1, -1), new Vector3(2, 2, 2));

    public bool IsOnScreen() => _onScreen;

    internal override bool UsesInternalProcess => true;

    internal override void InternalProcess(double delta) => Update();

    /// <summary>A test seam: the camera used instead of the current one.</summary>
    internal Camera3D? CameraOverride;

    /// <summary>Tests the box against the camera's frustum now and emits the signal if it changed.</summary>
    internal void Update()
    {
        bool now = IsInsideTree() && Visible && BoxOnScreen();
        if (now == _onScreen) return;
        _onScreen = now;
        OnScreenChanged(now);
        EmitSignal(now ? SignalName.ScreenEntered : SignalName.ScreenExited);
    }

    internal virtual void OnScreenChanged(bool onScreen) { }

    private bool BoxOnScreen()
    {
        Camera3D? cam = CameraOverride ?? GetViewport()?.GetCamera3D();
        if (cam == null) return false;
        Transform3D toView = cam.GetCameraTransform().AffineInverse() * GlobalTransform;
        Projection proj = cam.GetCameraProjection();
        var clip = new Vector4[8];
        for (int i = 0; i < 8; i++)
        {
            Vector3 corner = Aabb.Position + new Vector3((i & 1) != 0 ? Aabb.Size.X : 0, (i & 2) != 0 ? Aabb.Size.Y : 0, (i & 4) != 0 ? Aabb.Size.Z : 0);
            Vector3 v = toView * corner;
            clip[i] = proj * new Vector4(v.X, v.Y, v.Z, 1f);
        }
        // Off screen when every corner is outside the same clip plane.
        bool Outside(Func<Vector4, bool> out_) => clip.All(out_);
        if (Outside(c => c.X < -c.W) || Outside(c => c.X > c.W) || Outside(c => c.Y < -c.W) || Outside(c => c.Y > c.W) ||
            Outside(c => c.Z < -c.W) || Outside(c => c.Z > c.W)) return false;
        return true;
    }
}

/// <summary>Turns another node on only while a box is on screen (Godot's VisibleOnScreenEnabler3D): the node's process mode is
/// <see cref="EnableMode"/>'s while it is seen and Disabled otherwise.</summary>
public class VisibleOnScreenEnabler3D : VisibleOnScreenNotifier3D
{
    public enum EnableModeEnum { Inherit = 0, Always = 1, WhenPaused = 2 }

    private NodePath _target = new("..");

    // The enabler keeps watching while the node it enables is off (it is usually a child of it).
    public VisibleOnScreenEnabler3D() { ProcessMode = ProcessModeEnum.Always; }

    public EnableModeEnum EnableMode { get; set; } = EnableModeEnum.Inherit;

    /// <summary>The node to enable (default: the parent).</summary>
    public NodePath EnableNodePath { get => _target; set => _target = value; }

    internal override void OnScreenChanged(bool onScreen)
    {
        Node? n = _target.IsEmpty ? null : GetNodeOrNull(_target);
        if (n == null || n == this) return;
        n.ProcessMode = onScreen
            ? EnableMode switch { EnableModeEnum.Always => ProcessModeEnum.Always, EnableModeEnum.WhenPaused => ProcessModeEnum.WhenPaused, _ => ProcessModeEnum.Inherit }
            : ProcessModeEnum.Disabled;
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        // Start disabled: the node is off until the first pass sees the box.
        Node? n = _target.IsEmpty ? null : GetNodeOrNull(_target);
        if (n != null && n != this && !IsOnScreen()) n.ProcessMode = ProcessModeEnum.Disabled;
    }
}
