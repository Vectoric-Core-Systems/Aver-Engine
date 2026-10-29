// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// CanvasItem: what is drawn in UI space (Godot's CanvasItem). G5 U1 gives it the draw API a Control needs (rectangles, style boxes, strings): the
// commands a _Draw pass records are replayed every frame by the GUI into the engine's UI draw list (Gui.DrawAll), so a Control that does not
// change does not run _Draw again. Only 3D and UI are in scope; the 2D canvas is not.

namespace Aver.Fabric;

/// <summary>Anything drawn in UI space (Godot's CanvasItem).</summary>
public class CanvasItem : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Draw = "draw";
        public static readonly StringName VisibilityChanged = "visibility_changed";
        public static readonly StringName Hidden = "hidden";
        public static readonly StringName ItemRectChanged = "item_rect_changed";
    }

    public event Action Draw { add => Connect(SignalName.Draw, Callable.From(value)); remove => Disconnect(SignalName.Draw, Callable.From(value)); }

    public const int NotificationDraw = 30;
    public const int NotificationVisibilityChanged = 31;
    public const int NotificationEnterCanvas = 32;
    public const int NotificationExitCanvas = 33;
    public const int NotificationLocalTransformChanged = 35;
    public const int NotificationTransformChanged = 2000;

    /// <summary>The outline and shadow a string is drawn with (Godot's outline_size / font_outline_color / font_shadow_color / shadow_offset / shadow_outline_size).
    /// All zero draws the plain string.</summary>
    internal readonly struct TextFx
    {
        public float OutlineSize { get; init; }
        public Color OutlineColor { get; init; }
        public Color ShadowColor { get; init; }
        public Vector2 ShadowOffset { get; init; }
        public float ShadowOutlineSize { get; init; }

        public bool Any => (OutlineSize >= 0.5f && OutlineColor.A > 0f) || ShadowColor.A > 0f;
    }

    /// <summary>One recorded draw: a rectangle, a style box, a string or a texture, in the item's own space.</summary>
    internal readonly struct DrawCommand
    {
        public enum Kind { Rect, StyleBox, Text, Texture, PushClip, PopClip }

        public Kind Type { get; init; }
        public Rect2 Rect { get; init; }
        public Color Color { get; init; }
        public UiStyleBoxStruct Box { get; init; }
        public string Text { get; init; }
        public float FontSize { get; init; }
        public string Family { get; init; }
        public HorizontalAlignment Align { get; init; }
        public float Width { get; init; }
        public int Flags { get; init; }
        public TextFx Fx { get; init; }
        public ulong Handle { get; init; }
        public Rect2 Uv { get; init; }
    }

    private bool _visible = true;
    private bool _redraw = true;
    private bool _drawing;
    internal readonly List<DrawCommand> Commands = new();

    public bool Visible
    {
        get => _visible;
        set
        {
            if (_visible == value) return;
            _visible = value;
            OnShownChanged();
            Notification(NotificationVisibilityChanged);
            EmitSignal(SignalName.VisibilityChanged);
            if (!value) EmitSignal(SignalName.Hidden);
        }
    }

    /// <summary>This item's own visible flag changed (before the signal). A <see cref="SubViewportContainer"/> shows or hides its view with it.</summary>
    internal virtual void OnShownChanged() { }

    public Color Modulate { get; set; } = new(1, 1, 1, 1);
    public Color SelfModulate { get; set; } = new(1, 1, 1, 1);
    public int ZIndex { get; set; }
    public bool TopLevel { get; set; }

    public void Show() => Visible = true;
    public void Hide() => Visible = false;

    public bool IsVisibleInTree()
    {
        if (!IsInsideTree()) return false;
        for (Node? n = this; n is CanvasItem ci; n = n.GetParent())
            if (!ci._visible) return false;
        return true;
    }

    /// <summary>The colour multiplier from this item and every CanvasItem above it (their Modulate), times its own SelfModulate.</summary>
    public Color GetCombinedModulate(bool includeSelf = true)
    {
        Color c = includeSelf ? SelfModulate : new Color(1, 1, 1, 1);
        for (Node? n = this; n is CanvasItem ci; n = n.GetParent())
        {
            c = new Color(c.R * ci.Modulate.R, c.G * ci.Modulate.G, c.B * ci.Modulate.B, c.A * ci.Modulate.A);
            if (ci.TopLevel) break;
        }
        return c;
    }

    // ---- drawing ----

    /// <summary>Asks for <see cref="_Draw"/> to run again before the next frame is drawn.</summary>
    public void QueueRedraw() => _redraw = true;

    /// <summary>Called when the item needs drawing: record with the Draw methods.</summary>
    public virtual void _Draw() { }

    internal bool NeedsRedraw => _redraw;

    /// <summary>Runs the draw pass if one was asked for: clears the recorded commands, then _Draw and the draw signal fill them.</summary>
    internal void RunDraw()
    {
        if (!_redraw) return;
        _redraw = false;
        Commands.Clear();
        _drawing = true;
        try
        {
            Notification(NotificationDraw);
            _Draw();
            EmitSignal(SignalName.Draw);
        }
        finally { _drawing = false; }
    }

    private bool CanDraw(string what)
    {
        if (_drawing) return true;
        Aver.Scripting.Log.Error($"[Fabric] {what} can only be called while the item is drawing (_Draw or the draw signal)");
        return false;
    }

    /// <summary>A rectangle. Unfilled draws its four edges <paramref name="width"/> thick (at least one pixel; the edges lie inside the rectangle).</summary>
    public void DrawRect(Rect2 rect, Color color, bool filled = true, float width = -1f, bool antialiased = false)
    {
        if (!CanDraw(nameof(DrawRect))) return;
        if (filled)
        {
            Commands.Add(new DrawCommand { Type = DrawCommand.Kind.Rect, Rect = rect, Color = color });
            return;
        }
        float t = Math.Max(1f, width);
        Vector2 p = rect.Position, s = rect.Size;
        foreach (Rect2 edge in new[] { new Rect2(p, new Vector2(s.X, t)), new Rect2(p + new Vector2(0, s.Y - t), new Vector2(s.X, t)),
                                       new Rect2(p + new Vector2(0, t), new Vector2(t, s.Y - 2 * t)), new Rect2(p + new Vector2(s.X - t, t), new Vector2(t, s.Y - 2 * t)) })
            Commands.Add(new DrawCommand { Type = DrawCommand.Kind.Rect, Rect = edge, Color = color });
    }

    /// <summary>Draws a style box over a rectangle in the item's own space (Godot's draw_style_box).</summary>
    public void DrawStyleBox(StyleBox styleBox, Rect2 rect)
    {
        if (!CanDraw(nameof(DrawStyleBox)) || styleBox == null) return;
        Commands.Add(new DrawCommand { Type = DrawCommand.Kind.StyleBox, Rect = rect, Box = styleBox.ToNative() });
    }

    /// <summary>Draws a string with its baseline starting at <paramref name="pos"/>. The text engine lays it out through the UI library (U2 builds the
    /// text controls on it; the baseline is placed from the font size, 0.8 of it, until the engine reports font metrics here).</summary>
    public void DrawString(Font? font, Vector2 pos, string text, HorizontalAlignment alignment = HorizontalAlignment.Left, float width = -1f, int fontSize = 16, Color? modulate = null)
        => DrawStringOutline(font, pos, text, alignment, width, fontSize, 0, modulate);

    /// <summary>Draws a string with an outline of <paramref name="size"/> pixels (Godot's draw_string_outline); draw the plain string over it.</summary>
    public void DrawStringOutline(Font? font, Vector2 pos, string text, HorizontalAlignment alignment = HorizontalAlignment.Left, float width = -1f, int fontSize = 16, int size = 1, Color? modulate = null)
    {
        if (!CanDraw(nameof(DrawString)) || text.Length == 0) return;
        Color c = modulate ?? new Color(1, 1, 1, 1);
        float lh = UiText.LineHeight(font, fontSize);
        var box = new Rect2(pos - new Vector2(0, UiText.Ascent(font, fontSize)), new Vector2(Math.Max(0f, width), lh));
        AddText(box, text, font, fontSize, c, alignment, size > 0 ? new TextFx { OutlineSize = size, OutlineColor = c } : default, onlyOutline: size > 0);
    }

    /// <summary>Records a string whose top-left corner is the box's: the text controls draw their lines through this.</summary>
    internal void DrawTextLine(Vector2 topLeft, string text, Font? font, float px, Color color, in TextFx fx = default, float width = -1f, HorizontalAlignment align = HorizontalAlignment.Left)
    {
        if (text.Length == 0) return;
        AddText(new Rect2(topLeft, new Vector2(Math.Max(0f, width), UiText.LineHeight(font, px))), text, font, px, color, align, fx, onlyOutline: false);
    }

    private void AddText(Rect2 box, string text, Font? font, float px, Color color, HorizontalAlignment align, TextFx fx, bool onlyOutline)
    {
        Commands.Add(new DrawCommand
        {
            Type = DrawCommand.Kind.Text, Rect = box, Color = onlyOutline ? new Color(color.R, color.G, color.B, 0f) : color, Text = text, FontSize = px,
            Family = font?.Family ?? "", Align = align, Width = box.Size.X, Flags = font?.StyleFlags ?? 0, Fx = fx,
        });
    }

    /// <summary>Clips what is recorded until <see cref="PopClip"/> to a rectangle of the item's own space (the text controls clip their content).</summary>
    internal void PushClip(Rect2 rect)
    {
        if (_drawing) Commands.Add(new DrawCommand { Type = DrawCommand.Kind.PushClip, Rect = rect });
    }

    internal void PopClip()
    {
        if (_drawing) Commands.Add(new DrawCommand { Type = DrawCommand.Kind.PopClip });
    }

    /// <summary>Draws a texture with its top-left corner at <paramref name="position"/> (Godot's draw_texture). A texture the host has not given an engine
    /// handle draws nothing.</summary>
    public void DrawTexture(Texture2D texture, Vector2 position, Color? modulate = null)
        => DrawTextureRect(texture, new Rect2(position, texture.GetSize()), false, modulate);

    public void DrawTextureRect(Texture2D texture, Rect2 rect, bool tile, Color? modulate = null, bool transpose = false)
    {
        if (!CanDraw(nameof(DrawTextureRect))) return;
        Vector2 ts = texture.GetSize();
        var uv = new Rect2(0, 0, 1, 1);
        if (tile && ts.X > 0 && ts.Y > 0) uv = new Rect2(0, 0, rect.Size.X / ts.X, rect.Size.Y / ts.Y);
        Commands.Add(new DrawCommand { Type = DrawCommand.Kind.Texture, Rect = rect, Color = modulate ?? new Color(1, 1, 1, 1), Handle = texture.EngineHandle, Uv = uv });
    }

    /// <summary>Draws a part of a texture (<paramref name="srcRect"/>, in texels) into a rectangle (Godot's draw_texture_rect_region).</summary>
    public void DrawTextureRectRegion(Texture2D texture, Rect2 rect, Rect2 srcRect, Color? modulate = null)
    {
        if (!CanDraw(nameof(DrawTextureRectRegion))) return;
        Vector2 ts = texture.GetSize();
        var uv = ts.X > 0 && ts.Y > 0 ? new Rect2(srcRect.Position.X / ts.X, srcRect.Position.Y / ts.Y, srcRect.Size.X / ts.X, srcRect.Size.Y / ts.Y) : new Rect2(0, 0, 1, 1);
        Commands.Add(new DrawCommand { Type = DrawCommand.Kind.Texture, Rect = rect, Color = modulate ?? new Color(1, 1, 1, 1), Handle = texture.EngineHandle, Uv = uv });
    }

    /// <summary>A line. Only horizontal and vertical lines are drawn (as rectangles); the UI draw list has no thick diagonal line.</summary>
    public void DrawLine(Vector2 from, Vector2 to, Color color, float width = -1f, bool antialiased = false)
    {
        if (!CanDraw(nameof(DrawLine))) return;
        float t = Math.Max(1f, width);
        if (MathF.Abs(from.Y - to.Y) < 0.01f) Commands.Add(new DrawCommand { Type = DrawCommand.Kind.Rect, Rect = new Rect2(MathF.Min(from.X, to.X), from.Y - t / 2f, MathF.Abs(to.X - from.X), t), Color = color });
        else if (MathF.Abs(from.X - to.X) < 0.01f) Commands.Add(new DrawCommand { Type = DrawCommand.Kind.Rect, Rect = new Rect2(from.X - t / 2f, MathF.Min(from.Y, to.Y), t, MathF.Abs(to.Y - from.Y)), Color = color });
    }

    /// <summary>The item's transform to the UI's space. A plain canvas item has none of its own: identity.</summary>
    internal virtual Transform2D GlobalCanvasTransform => Transform2D.Identity;

    /// <summary>Whether this item's children are clipped to its rectangle when drawn.</summary>
    internal virtual bool ClipsChildren => false;

    internal virtual Rect2 ClipRectGlobal => default;
}
