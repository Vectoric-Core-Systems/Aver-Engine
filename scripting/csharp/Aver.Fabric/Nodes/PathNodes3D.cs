// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Path3D (holds a Curve3D) and PathFollow3D (rides it): progress in metres or as a ratio, looping or clamped, the rotation modes, h/v offset.
// Godot's space and units throughout. docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>A node holding a curve in 3D (Godot's Path3D).</summary>
public class Path3D : Node3D
{
    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName CurveChanged = "curve_changed";
    }

    public event Action CurveChanged { add => Connect(SignalName.CurveChanged, Callable.From(value)); remove => Disconnect(SignalName.CurveChanged, Callable.From(value)); }

    private Curve3D? _curve;

    /// <summary>The path. Anything that changes it (points, handles) emits <c>curve_changed</c>.</summary>
    public Curve3D? Curve
    {
        get => _curve;
        set
        {
            if (ReferenceEquals(_curve, value)) return;
            _curve?.Disconnect(Resource.SignalName.Changed, Callable.From(OnCurveChanged));
            _curve = value;
            _curve?.Connect(Resource.SignalName.Changed, Callable.From(OnCurveChanged));
            OnCurveChanged();
        }
    }

    private void OnCurveChanged() => EmitSignal(SignalName.CurveChanged);
}

/// <summary>Rides the curve of the Path3D it is a child of (Godot's PathFollow3D).</summary>
public class PathFollow3D : Node3D
{
    public enum RotationModeEnum { None = 0, Y = 1, Xy = 2, Xyz = 3, Oriented = 4 }

    private float _progress, _hOffset, _vOffset;
    private bool _cubic = true, _loop = true, _tilt = true;
    private RotationModeEnum _rotation = RotationModeEnum.Xyz;
    private Path3D? _path;

    /// <summary>How far along the baked curve the node is, in metres. Looping wraps it, otherwise it stops at the ends.</summary>
    public float Progress
    {
        get => _progress;
        set
        {
            if (!float.IsFinite(value)) return;
            float requested = value;
            _progress = value;
            if (_path?.Curve is { } c)
            {
                float length = c.GetBakedLength();
                if (_loop && length > 0)
                {
                    _progress = Mathf.PosMod(_progress, length);
                    if (!Mathf.IsZeroApprox(requested) && Mathf.IsZeroApprox(_progress)) _progress = length;
                }
                else _progress = Math.Clamp(_progress, 0f, length);
                UpdateTransform();
            }
        }
    }

    /// <summary>The same as a fraction of the curve's length (0 for an empty or missing curve).</summary>
    public float ProgressRatio
    {
        get
        {
            float length = _path?.Curve?.GetBakedLength() ?? 0f;
            return length > 0 ? _progress / length : 0f;
        }
        set
        {
            if (_path?.Curve == null) { _pendingRatio = value; return; }   // a scene sets it before the node has a curve to measure
            Progress = value * _path.Curve.GetBakedLength();
        }
    }

    private float? _pendingRatio;

    /// <summary>Slides the node sideways (along its own X) off the curve, in metres.</summary>
    public float HOffset { get => _hOffset; set { _hOffset = value; UpdateTransform(); } }

    /// <summary>Slides the node up (along its own Y) off the curve, in metres.</summary>
    public float VOffset { get => _vOffset; set { _vOffset = value; UpdateTransform(); } }

    public RotationModeEnum RotationMode { get => _rotation; set { _rotation = value; UpdateTransform(); } }

    /// <summary>Interpolates the baked points with a cubic instead of a line.</summary>
    public bool CubicInterp { get => _cubic; set { _cubic = value; UpdateTransform(); } }

    /// <summary>Wraps progress past the ends; false stops at them.</summary>
    public bool Loop { get => _loop; set { _loop = value; } }

    /// <summary>Applies the curve's tilt to the rotation (XYZ and Oriented modes).</summary>
    public bool TiltEnabled { get => _tilt; set { _tilt = value; UpdateTransform(); } }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Attach(GetParent() as Path3D);
    }

    internal override void OnExitingTree()
    {
        base.OnExitingTree();
        Attach(null);
    }

    private void Attach(Path3D? path)
    {
        if (ReferenceEquals(_path, path)) return;
        _path?.Disconnect(Path3D.SignalName.CurveChanged, Callable.From(OnCurveChanged));
        _path = path;
        _path?.Connect(Path3D.SignalName.CurveChanged, Callable.From(OnCurveChanged));
        OnCurveChanged();
    }

    private void OnCurveChanged()
    {
        if (_path?.Curve == null) return;
        if (_pendingRatio is float r)
        {
            _pendingRatio = null;
            Progress = r * _path.Curve.GetBakedLength();
            return;
        }
        Progress = _progress;
    }

    private static Basis Look(Vector3 forward, Vector3 up)
    {
        if (forward.LengthSquared() < 1e-12f) return Basis.Identity;
        forward = forward.Normalized();
        if (MathF.Abs(forward.Dot(up.Normalized())) > 0.9999f) up = MathF.Abs(forward.Z) < 0.9f ? new Vector3(0, 0, 1) : new Vector3(1, 0, 0);
        return Basis.LookingAt(forward, up);
    }

    private void UpdateTransform()
    {
        Curve3D? c = _path?.Curve;
        if (c == null) return;
        float length = c.GetBakedLength();
        if (length == 0) return;
        Vector3 pos = c.SampleBaked(_progress, _cubic);
        Transform3D t = Transform;
        Vector3 scale = t.Basis.GetScale();
        if (_rotation != RotationModeEnum.None)
        {
            // The direction of travel: a short look ahead, or behind at the very end.
            float look = MathF.Min(0.01f, length * 0.5f);
            Vector3 forward = _progress + look <= length ? c.SampleBaked(_progress + look, _cubic) - pos : pos - c.SampleBaked(_progress - look, _cubic);
            if (forward.LengthSquared() < 1e-12f) forward = new Vector3(0, 0, -1);
            Basis b;
            switch (_rotation)
            {
                case RotationModeEnum.Y:
                    b = Look(new Vector3(forward.X, 0, forward.Z), Vector3.Up);
                    break;
                case RotationModeEnum.Xy:
                    b = Look(forward, Vector3.Up);
                    break;
                case RotationModeEnum.Oriented:
                    b = Look(forward, c.SampleBakedUpVector(_progress, _tilt));
                    break;
                default:
                {
                    Vector3 up = Vector3.Up;
                    if (_tilt)
                    {
                        Vector3 tilted = c.SampleBakedUpVector(_progress, true);
                        Vector3 flat = c.SampleBakedUpVector(_progress, false);
                        // Roll the world up by the same angle the tilt turned the curve's own up vector.
                        float roll = flat.SignedAngleTo(tilted, forward.Normalized());
                        up = up.Rotated(forward.Normalized(), roll);
                    }
                    b = Look(forward, up);
                    break;
                }
            }
            t.Basis = b;
        }
        else t.Basis = t.Basis.Orthonormalized();
        t.Origin = pos + t.Basis.X * _hOffset + t.Basis.Y * _vOffset;
        t.Basis = t.Basis * Basis.FromScale(scale);
        Transform = t;
    }
}
