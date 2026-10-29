// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Control (G5 U1): Godot's UI base node. Layout is Godot's: anchors and offsets against the parent's rectangle, grow directions, a minimum size that
// a container reads, size flags a container obeys. Theme lookup is in Control.Theme.cs, input / focus / tooltips / drag and drop in Control.Gui.cs and
// Gui.cs. The properties a scene stores (layout_mode, anchors_preset, anchor_*, offset_*, grow_*, size_flags_*, theme_override_*) are applied by name.

namespace Aver.Fabric;

/// <summary>A UI element (Godot's Control): a rectangle laid out by anchors and offsets or by a container, with a theme, input, focus, tooltips
/// and drag and drop.</summary>
public partial class Control : CanvasItem
{
    public enum MouseFilterEnum { Stop = 0, Pass = 1, Ignore = 2 }

    public enum FocusModeEnum { None = 0, Click = 1, All = 2 }

    public enum LayoutPreset
    {
        TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3, CenterLeft = 4, CenterTop = 5, CenterRight = 6, CenterBottom = 7, Center = 8,
        LeftWide = 9, TopWide = 10, RightWide = 11, BottomWide = 12, VcenterWide = 13, HcenterWide = 14, FullRect = 15,
    }

    public enum LayoutPresetMode { Minsize = 0, KeepWidth = 1, KeepHeight = 2, KeepSize = 3 }

    [Flags]
    public enum SizeFlags { ShrinkBegin = 0, Fill = 1, Expand = 2, ExpandFill = 3, ShrinkCenter = 4, ShrinkEnd = 8 }

    public enum GrowDirection { Begin = 0, End = 1, Both = 2 }

    public enum CursorShape { Arrow = 0, Ibeam = 1, PointingHand = 2, Cross = 3, Wait = 4, Busy = 5, Drag = 6, CanDrop = 7, Forbidden = 8, Vsize = 9, Hsize = 10, Bdiagsize = 11, Fdiagsize = 12, Move = 13, Vsplit = 14, Hsplit = 15, Help = 16 }

    public enum LayoutDirection { Inherited = 0, ApplicationLocale = 1, Ltr = 2, Rtl = 3, SystemLocale = 4 }

    public enum TextDirection { Inherited = 3, Auto = 0, Ltr = 1, Rtl = 2 }

    public new class SignalName : CanvasItem.SignalName
    {
        public static readonly StringName Resized = "resized";
        public static readonly StringName GuiInput = "gui_input";
        public static readonly StringName MouseEntered = "mouse_entered";
        public static readonly StringName MouseExited = "mouse_exited";
        public static readonly StringName FocusEntered = "focus_entered";
        public static readonly StringName FocusExited = "focus_exited";
        public static readonly StringName SizeFlagsChanged = "size_flags_changed";
        public static readonly StringName MinimumSizeChanged = "minimum_size_changed";
        public static readonly StringName ThemeChanged = "theme_changed";
    }

    public const int NotificationResized = 40;
    public const int NotificationMouseEnter = 41;
    public const int NotificationMouseExit = 42;
    public const int NotificationFocusEnter = 43;
    public const int NotificationFocusExit = 44;
    public const int NotificationThemeChanged = 45;
    public const int NotificationScrollBegin = 47;
    public const int NotificationScrollEnd = 48;
    public const int NotificationLayoutDirectionChanged = 49;
    public const int NotificationMouseEnterSelf = 60;
    public const int NotificationMouseExitSelf = 61;
    public const int NotificationDragBegin = 21;
    public const int NotificationDragEnd = 22;

    public event Action Resized { add => Connect(SignalName.Resized, Callable.From(value)); remove => Disconnect(SignalName.Resized, Callable.From(value)); }
    public event Action MouseEntered { add => Connect(SignalName.MouseEntered, Callable.From(value)); remove => Disconnect(SignalName.MouseEntered, Callable.From(value)); }
    public event Action MouseExited { add => Connect(SignalName.MouseExited, Callable.From(value)); remove => Disconnect(SignalName.MouseExited, Callable.From(value)); }
    public event Action FocusEntered { add => Connect(SignalName.FocusEntered, Callable.From(value)); remove => Disconnect(SignalName.FocusEntered, Callable.From(value)); }
    public event Action FocusExited { add => Connect(SignalName.FocusExited, Callable.From(value)); remove => Disconnect(SignalName.FocusExited, Callable.From(value)); }
    public event Action MinimumSizeChanged { add => Connect(SignalName.MinimumSizeChanged, Callable.From(value)); remove => Disconnect(SignalName.MinimumSizeChanged, Callable.From(value)); }
    public event Action SizeFlagsChanged { add => Connect(SignalName.SizeFlagsChanged, Callable.From(value)); remove => Disconnect(SignalName.SizeFlagsChanged, Callable.From(value)); }
    public event Action ThemeChanged { add => Connect(SignalName.ThemeChanged, Callable.From(value)); remove => Disconnect(SignalName.ThemeChanged, Callable.From(value)); }
    public event Action<InputEvent> GuiInput { add => Connect(SignalName.GuiInput, Callable.From(value)); remove => Disconnect(SignalName.GuiInput, Callable.From(value)); }

    // ---- layout state ----

    private readonly float[] _anchor = new float[4];   // left, top, right, bottom
    private readonly float[] _offset = new float[4];
    private Vector2 _pos, _size;                       // the rectangle the anchors and offsets (or a container) made, in the parent's space
    private Vector2 _customMin, _minCache, _lastMin;
    private bool _minValid, _minQueued;
    private GrowDirection _growH = GrowDirection.End, _growV = GrowDirection.End;
    private SizeFlags _flagsH = SizeFlags.Fill, _flagsV = SizeFlags.Fill;
    private float _stretchRatio = 1f;
    private Vector2 _pivot, _scale = Vector2.One;
    private float _rotation;
    private int _anchorsPreset = -1, _layoutMode;

    /// <summary>The position in the parent's space. Setting it moves the control and rewrites the offsets (anchors stay).</summary>
    public Vector2 Position { get => _pos; set => SetPosition(value); }

    /// <summary>The size, never below the combined minimum size.</summary>
    public Vector2 Size { get => _size; set => SetSize(value); }

    /// <summary>The control's rectangle changed (or an ancestor moved it). A <see cref="SubViewportContainer"/> keeps the view it shows in step with it.</summary>
    internal virtual void OnRectChanged() { }

    public Vector2 CustomMinimumSize
    {
        get => _customMin;
        set
        {
            if (_customMin == value) return;
            _customMin = value;
            UpdateMinimumSize();
        }
    }

    public Vector2 PivotOffset { get => _pivot; set { _pivot = value; NotifyTransformChanged(); } }
    public float Rotation { get => _rotation; set { _rotation = value; NotifyTransformChanged(); } }
    public float RotationDegrees { get => Mathf.RadToDeg(_rotation); set => Rotation = Mathf.DegToRad(value); }
    public Vector2 Scale { get => _scale; set { _scale = value; NotifyTransformChanged(); } }

    public float AnchorLeft { get => _anchor[0]; set => SetAnchor(Side.Left, value); }
    public float AnchorTop { get => _anchor[1]; set => SetAnchor(Side.Top, value); }
    public float AnchorRight { get => _anchor[2]; set => SetAnchor(Side.Right, value); }
    public float AnchorBottom { get => _anchor[3]; set => SetAnchor(Side.Bottom, value); }

    public float OffsetLeft { get => _offset[0]; set => SetOffset(Side.Left, value); }
    public float OffsetTop { get => _offset[1]; set => SetOffset(Side.Top, value); }
    public float OffsetRight { get => _offset[2]; set => SetOffset(Side.Right, value); }
    public float OffsetBottom { get => _offset[3]; set => SetOffset(Side.Bottom, value); }

    public GrowDirection GrowHorizontal { get => _growH; set { _growH = value; SizeChanged(); } }
    public GrowDirection GrowVertical { get => _growV; set { _growV = value; SizeChanged(); } }

    public SizeFlags SizeFlagsHorizontal
    {
        get => _flagsH;
        set
        {
            if (_flagsH == value) return;
            _flagsH = value;
            OnSizeFlagsChanged();
        }
    }

    public SizeFlags SizeFlagsVertical
    {
        get => _flagsV;
        set
        {
            if (_flagsV == value) return;
            _flagsV = value;
            OnSizeFlagsChanged();
        }
    }

    public float SizeFlagsStretchRatio
    {
        get => _stretchRatio;
        set
        {
            _stretchRatio = Math.Max(0.001f, value);
            OnSizeFlagsChanged();
        }
    }

    private void OnSizeFlagsChanged()
    {
        EmitSignal(SignalName.SizeFlagsChanged);
        (GetParent() as Container)?.QueueSort();
    }

    /// <summary>Godot stores the layout form the editor used (0 position, 1 anchors, 2 container, 3 uncontrolled). Kept; layout works off anchors and offsets.</summary>
    public int LayoutMode { get => _layoutMode; set => _layoutMode = value; }

    /// <summary>The anchor preset the scene recorded. Setting it applies the preset's anchors (offsets stay, as a scene's own offsets follow).</summary>
    public int AnchorsPreset
    {
        get => _anchorsPreset;
        set
        {
            _anchorsPreset = value;
            if (value >= 0 && value <= 15) SetAnchorsPreset((LayoutPreset)value, keepOffsets: true);
        }
    }

    // ---- the rectangle ----

    /// <summary>The rectangle of the parent this control is laid out against: a Control parent's size, else the viewport's visible area (empty for a
    /// parent that is a non-Control canvas item).</summary>
    private Rect2 ParentAnchorableRect()
    {
        Node? p = GetParent();
        if (p is Control pc) return new Rect2(Vector2.Zero, pc._size);
        if (p is CanvasItem) return default;
        return GetViewport() is { } vp ? vp.GetVisibleRect() : default;
    }

    /// <summary>The size of the parent area the anchors refer to.</summary>
    public Vector2 GetParentAreaSize() => ParentAnchorableRect().Size;

    private static float Pr(Rect2 r, int side) => side % 2 == 0 ? r.Size.X : r.Size.Y;

    private void ComputeOffsets(Rect2 rect, float[] offsets)
    {
        Rect2 pr = ParentAnchorableRect();
        offsets[0] = rect.Position.X - _anchor[0] * pr.Size.X;
        offsets[1] = rect.Position.Y - _anchor[1] * pr.Size.Y;
        offsets[2] = rect.Position.X + rect.Size.X - _anchor[2] * pr.Size.X;
        offsets[3] = rect.Position.Y + rect.Size.Y - _anchor[3] * pr.Size.Y;
    }

    private void ComputeAnchors(Rect2 rect)
    {
        Rect2 pr = ParentAnchorableRect();
        Vector2 ps = pr.Size;
        _anchor[0] = ps.X == 0 ? 0f : (rect.Position.X - _offset[0]) / ps.X;
        _anchor[1] = ps.Y == 0 ? 0f : (rect.Position.Y - _offset[1]) / ps.Y;
        _anchor[2] = ps.X == 0 ? 0f : (rect.Position.X + rect.Size.X - _offset[2]) / ps.X;
        _anchor[3] = ps.Y == 0 ? 0f : (rect.Position.Y + rect.Size.Y - _offset[3]) / ps.Y;
    }

    /// <summary>Works the rectangle out again from the anchors, the offsets, the parent's size and the minimum size (Godot's _size_changed).</summary>
    internal void SizeChanged()
    {
        Rect2 pr = ParentAnchorableRect();
        float l = _offset[0] + _anchor[0] * pr.Size.X, t = _offset[1] + _anchor[1] * pr.Size.Y;
        float r = _offset[2] + _anchor[2] * pr.Size.X, b = _offset[3] + _anchor[3] * pr.Size.Y;
        Vector2 np = new(l, t), ns = new(r - l, b - t);
        Vector2 min = GetCombinedMinimumSize();
        if (min.X > ns.X)
        {
            if (_growH == GrowDirection.Begin) np.X += ns.X - min.X;
            else if (_growH == GrowDirection.Both) np.X += 0.5f * (ns.X - min.X);
            ns.X = min.X;
        }
        if (min.Y > ns.Y)
        {
            if (_growV == GrowDirection.Begin) np.Y += ns.Y - min.Y;
            else if (_growV == GrowDirection.Both) np.Y += 0.5f * (ns.Y - min.Y);
            ns.Y = min.Y;
        }
        bool posChanged = np != _pos, sizeChanged = ns != _size;
        _pos = np;
        _size = ns;
        if (!IsInsideTree() || (!posChanged && !sizeChanged)) return;
        if (sizeChanged)
        {
            Notification(NotificationResized);
            EmitSignal(SignalName.Resized);
        }
        EmitSignal(SignalName.ItemRectChanged);
        QueueRedraw();
        if (sizeChanged)
        {
            (this as Container)?.QueueSort();
            foreach (Node c in ChildrenSnapshot()) if (c is Control cc) cc.SizeChanged();
        }
        NotifyTransformChanged();
    }

    // The control moved or turned: tell it and every Control below that the global rectangle may have changed.
    private void NotifyTransformChanged()
    {
        if (!IsInsideTree()) return;
        OnRectChanged();
        Notification(NotificationTransformChanged);
        foreach (Node c in ChildrenSnapshot()) if (c is Control cc) cc.NotifyTransformChanged();
    }

    public void SetPosition(Vector2 position, bool keepOffsets = false)
    {
        if (keepOffsets) ComputeAnchors(new Rect2(position, _size));
        else ComputeOffsets(new Rect2(position, _size), _offset);
        SizeChanged();
    }

    public void SetSize(Vector2 size, bool keepOffsets = false)
    {
        Vector2 min = GetCombinedMinimumSize();
        if (size.X < min.X) size.X = min.X;
        if (size.Y < min.Y) size.Y = min.Y;
        if (keepOffsets) ComputeAnchors(new Rect2(_pos, size));
        else ComputeOffsets(new Rect2(_pos, size), _offset);
        SizeChanged();
    }

    /// <summary>Resets the size to the minimum size.</summary>
    public void ResetSize() => SetSize(Vector2.Zero);

    /// <summary>The top-left corner (Godot's set_begin: moves the left and top offsets, leaves the opposite edges).</summary>
    public void SetBegin(Vector2 position)
    {
        _offset[0] = position.X;
        _offset[1] = position.Y;
        SizeChanged();
    }

    public void SetEnd(Vector2 position)
    {
        _offset[2] = position.X;
        _offset[3] = position.Y;
        SizeChanged();
    }

    public Vector2 GetBegin() => new(_offset[0], _offset[1]);
    public Vector2 GetEnd() => new(_offset[2], _offset[3]);

    public void SetAnchor(Side side, float anchor, bool keepOffset = true, bool pushOppositeAnchor = true)
    {
        int s = (int)side, o = (s + 2) % 4;
        Rect2 pr = ParentAnchorableRect();
        float range = Pr(pr, s);
        float previous = _offset[s] + _anchor[s] * range, previousOpposite = _offset[o] + _anchor[o] * range;
        _anchor[s] = anchor;
        if (((s == 0 || s == 1) && _anchor[s] > _anchor[o]) || ((s == 2 || s == 3) && _anchor[s] < _anchor[o]))
        {
            if (pushOppositeAnchor) _anchor[o] = _anchor[s];
            else _anchor[s] = _anchor[o];
        }
        if (!keepOffset)
        {
            _offset[s] = previous - _anchor[s] * range;
            if (pushOppositeAnchor) _offset[o] = previousOpposite - _anchor[o] * range;
        }
        SizeChanged();
    }

    public float GetAnchor(Side side) => _anchor[(int)side];

    public void SetOffset(Side side, float offset)
    {
        _offset[(int)side] = offset;
        SizeChanged();
    }

    public float GetOffset(Side side) => _offset[(int)side];

    // Anchors of each preset: left, top, right, bottom.
    private static readonly float[][] s_presetAnchors =
    {
        new[] { 0f, 0f, 0f, 0f }, new[] { 1f, 0f, 1f, 0f }, new[] { 0f, 1f, 0f, 1f }, new[] { 1f, 1f, 1f, 1f },
        new[] { 0f, .5f, 0f, .5f }, new[] { .5f, 0f, .5f, 0f }, new[] { 1f, .5f, 1f, .5f }, new[] { .5f, 1f, .5f, 1f }, new[] { .5f, .5f, .5f, .5f },
        new[] { 0f, 0f, 0f, 1f }, new[] { 0f, 0f, 1f, 0f }, new[] { 1f, 0f, 1f, 1f }, new[] { 0f, 1f, 1f, 1f },
        new[] { 0f, .5f, 1f, .5f }, new[] { .5f, 0f, .5f, 1f }, new[] { 0f, 0f, 1f, 1f },
    };

    public void SetAnchorsPreset(LayoutPreset preset, bool keepOffsets = false)
    {
        float[] a = s_presetAnchors[(int)preset];
        for (int i = 0; i < 4; i++) SetAnchor((Side)i, a[i], keepOffsets);
        _anchorsPreset = (int)preset;
    }

    public void SetOffsetsPreset(LayoutPreset preset, LayoutPresetMode resizeMode = LayoutPresetMode.Minsize, int margin = 0)
    {
        Vector2 min = GetCombinedMinimumSize();
        Vector2 ns = _size;
        if (resizeMode == LayoutPresetMode.Minsize || resizeMode == LayoutPresetMode.KeepHeight) ns.X = min.X;
        if (resizeMode == LayoutPresetMode.Minsize || resizeMode == LayoutPresetMode.KeepWidth) ns.Y = min.Y;
        float[] a = s_presetAnchors[(int)preset];
        for (int i = 0; i < 4; i++)
        {
            bool horizontal = i % 2 == 0, far = i >= 2;
            float extent = horizontal ? ns.X : ns.Y;
            // An edge anchored at the start sits `margin` in; at the middle it is half the size either side; at the end it sits `margin` back.
            float v;
            if (!far) v = a[i] == 0f ? margin : a[i] == 0.5f ? -extent / 2f : -extent - margin;
            else v = a[i] == 0f ? extent + margin : a[i] == 0.5f ? extent / 2f : -margin;
            _offset[i] = v;
        }
        SizeChanged();
    }

    public void SetAnchorsAndOffsetsPreset(LayoutPreset preset, LayoutPresetMode resizeMode = LayoutPresetMode.Minsize, int margin = 0)
    {
        SetAnchorsPreset(preset);
        SetOffsetsPreset(preset, resizeMode, margin);
    }

    public Rect2 GetRect() => new(_pos, _size);

    // ---- transforms ----

    /// <summary>Position, then pivoted rotation and scale: the control's transform to its parent's space.</summary>
    public Transform2D GetTransform()
    {
        var rs = new Transform2D(_rotation, _scale, 0f, Vector2.Zero);
        rs.Origin = _pivot - rs.BasisXform(_pivot) + _pos;
        return rs;
    }

    public Transform2D GetGlobalTransform()
    {
        Transform2D t = GetTransform();
        if (!TopLevel && GetParent() is Control p) t = p.GetGlobalTransform() * t;
        return t;
    }

    internal override Transform2D GlobalCanvasTransform => GetGlobalTransform();

    public Transform2D GetGlobalTransformWithCanvas() => GetGlobalTransform();

    public Vector2 GlobalPosition { get => GetGlobalTransform().Origin; set => SetGlobalPosition(value); }

    public void SetGlobalPosition(Vector2 position, bool keepOffsets = false)
    {
        Transform2D inv = !TopLevel && GetParent() is Control p ? p.GetGlobalTransform().AffineInverse() : Transform2D.Identity;
        SetPosition(inv * position, keepOffsets);
    }

    public Rect2 GetGlobalRect() => new(GlobalPosition, _size);

    public Vector2 GetScreenPosition() => GlobalPosition;

    public Rect2 GetScreenRect() => GetGlobalRect();

    public Rect2 GetItemRect() => new(Vector2.Zero, _size);

    // ---- minimum size ----

    /// <summary>The smallest size this control can take: its custom minimum size or what the control itself needs (a container's children, a label's
    /// text), whichever is larger on each axis.</summary>
    public Vector2 GetCombinedMinimumSize()
    {
        if (!_minValid)
        {
            Vector2 own = GetMinimumSize();
            _minCache = new Vector2(Math.Max(_customMin.X, own.X), Math.Max(_customMin.Y, own.Y));
            _minValid = true;
        }
        return _minCache;
    }

    /// <summary>What the control needs by itself: its own sizing rule and a script's <see cref="_GetMinimumSize"/>, largest per axis.</summary>
    public Vector2 GetMinimumSize()
    {
        Vector2 a = NativeMinimumSize(), b = _GetMinimumSize();
        return new Vector2(Math.Max(a.X, b.X), Math.Max(a.Y, b.Y));
    }

    /// <summary>A script's override of the minimum size.</summary>
    public virtual Vector2 _GetMinimumSize() => Vector2.Zero;

    /// <summary>The minimum size a built-in class asks for (containers, panels, labels ...).</summary>
    internal virtual Vector2 NativeMinimumSize() => Vector2.Zero;

    /// <summary>Tells the control its minimum size may have changed (a label's text, a stylebox's margins). The caches above it are dropped at once; the
    /// size is brought up to date, the parent container told and <c>minimum_size_changed</c> emitted at the end of the frame.</summary>
    public void UpdateMinimumSize()
    {
        if (!IsInsideTree()) { _minValid = false; return; }
        for (Control? c = this; c != null && c._minValid; c = !c.TopLevel ? c.GetParent() as Control : null) c._minValid = false;
        if (!IsVisibleInTree() || _minQueued) return;
        _minQueued = true;
        Gui.Defer(RefreshMinimumSize);
    }

    private void RefreshMinimumSize()
    {
        _minQueued = false;
        if (!IsAlive) return;
        Vector2 min = GetCombinedMinimumSize();
        if (min == _lastMin) return;
        _lastMin = min;
        SizeChanged();
        EmitSignal(SignalName.MinimumSizeChanged);
        (GetParent() as Container)?.OnChildMinimumSizeChanged();
    }

    // ---- tree hooks ----

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        _minValid = false;
        _lastMin = GetCombinedMinimumSize();
        SizeChanged();
        Gui.Enter(this);
        (this as Container)?.QueueSort();
        Notification(NotificationThemeChanged);
        QueueRedraw();
    }

    internal override void OnExitingTree()
    {
        Gui.Exit(this);
        base.OnExitingTree();
    }

    internal override void OnShownChanged()
    {
        base.OnShownChanged();
        (GetParent() as Container)?.QueueSort();
        if (!Visible) Gui.ControlHidden(this);
        else UpdateMinimumSize();
        QueueRedraw();
    }

    // ---- clipping ----

    /// <summary>Children are clipped to the control's rectangle when drawn and cannot receive the pointer outside it.</summary>
    public bool ClipContents { get; set; }

    internal override bool ClipsChildren => ClipContents;

    internal override Rect2 ClipRectGlobal => GetGlobalTransform() * new Rect2(Vector2.Zero, _size);

    // ---- other stored settings ----

    public LayoutDirection LayoutDirectionMode { get; set; } = LayoutDirection.Inherited;
    public TextDirection TextDirectionMode { get; set; } = TextDirection.Inherited;
    public bool AutoTranslate { get; set; } = true;
    public CursorShape MouseDefaultCursorShape { get; set; } = CursorShape.Arrow;
    public bool MouseForcePassScrollEvents { get; set; } = true;

    public CursorShape GetCursorShape(Vector2 position = default) => MouseDefaultCursorShape;

    public bool IsLayoutRtl() => LayoutDirectionMode == LayoutDirection.Rtl;

    /// <summary>The scene stores the layout direction as <c>layout_direction</c>; the member is named apart from the enum type.</summary>
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name == "layout_direction") { LayoutDirectionMode = (LayoutDirection)value.AsInt32(); return true; }
        if (name == "text_direction") { TextDirectionMode = (TextDirection)value.AsInt32(); return true; }
        // stored for the text classes (G5 U2: the text engine decides the structure of a line)
        if (name is "structured_text_bidi_override" or "structured_text_bidi_override_options") { SetMeta(name, value); return true; }
        if (TrySetThemeOverride(name, value)) return true;
        return base.TryDynamicSet(name, value);
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        if (TryGetThemeOverride(name, out value)) return true;
        return base.TryDynamicGet(name, out value);
    }

    internal static void ThemeChangedEverywhere()
    {
        // NotifyThemeChanged reaches the whole subtree, so only the controls that hang from a non-Control are started here.
        foreach (Control c in Gui.AllControls()) if (c.GetParent() is not Control) c.NotifyThemeChanged();
    }
}
