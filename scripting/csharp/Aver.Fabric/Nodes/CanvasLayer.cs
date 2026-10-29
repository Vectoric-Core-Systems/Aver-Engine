// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// CanvasLayer (Godot's CanvasLayer): the node a game's UI hangs from so it is drawn over the 3D view and can be shown and hidden as one. The
// Controls below it are laid out and drawn as they are below any other node; the layer adds its visibility to theirs (CanvasItem.IsVisibleInTree).

namespace Aver.Fabric;

/// <summary>A group of UI drawn together (Godot's CanvasLayer). Its <see cref="Visible"/> flag hides everything drawn on it. The draw order
/// (<see cref="Layer"/>), the offset, rotation and scale are kept as set: the GUI draws every Control in tree order in viewport space and does not apply them.</summary>
public class CanvasLayer : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName VisibilityChanged = "visibility_changed";
    }

    public event Action VisibilityChanged { add => Connect(SignalName.VisibilityChanged, Callable.From(value)); remove => Disconnect(SignalName.VisibilityChanged, Callable.From(value)); }

    private bool _visible = true;
    private float _rotation;
    private Vector2 _offset, _scale = Vector2.One;

    /// <summary>The draw order among layers: higher is drawn over lower. Kept; the GUI draws in tree order.</summary>
    public int Layer { get; set; } = 1;

    public bool Visible
    {
        get => _visible;
        set
        {
            if (_visible == value) return;
            _visible = value;
            EmitSignal(SignalName.VisibilityChanged);
        }
    }

    public void Show() => Visible = true;
    public void Hide() => Visible = false;

    /// <summary>True when this layer and every layer above it is shown and the node is in the tree.</summary>
    public bool IsVisibleInTree()
    {
        if (!IsInsideTree()) return false;
        for (Node? n = this; n != null; n = n.GetParent())
            if (n is CanvasLayer { _visible: false }) return false;
        return true;
    }

    public Vector2 Offset { get => _offset; set => _offset = value; }
    public float Rotation { get => _rotation; set => _rotation = value; }
    public float RotationDegrees { get => _rotation * (180f / Mathf.Pi); set => _rotation = value * (Mathf.Pi / 180f); }
    public Vector2 Scale { get => _scale; set => _scale = value; }
    public bool FollowViewportEnabled { get; set; }
    public float FollowViewportScale { get; set; } = 1f;

    /// <summary>The offset, rotation and scale as one transform.</summary>
    public Transform2D Transform
    {
        get => new(_rotation, _scale, 0f, _offset);
        set { _offset = value.Origin; _rotation = value.GetRotation(); _scale = value.GetScale(); }
    }

    public Transform2D GetFinalTransform() => Transform;
}
