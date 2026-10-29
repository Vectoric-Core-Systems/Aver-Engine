// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// LineEdit (G5 U2): single-line text entry. Typing, caret and selection, word and line jumps, delete, copy / cut / paste (an in-process clipboard), undo and redo,
// max_length with text_change_rejected, secret characters, placeholder, alignment, horizontal scrolling, mouse caret and drag selection, double-click word
// selection, text_changed / text_submitted. Setting the text from code does not emit text_changed, as in Godot.

namespace Aver.Fabric;

/// <summary>Word and line helpers the text editing controls share.</summary>
internal static class TextOps
{
    internal static bool IsWordChar(char c) => char.IsLetterOrDigit(c) || c == '_';

    /// <summary>The start of the word before <paramref name="index"/> (Ctrl+Left).</summary>
    internal static int PrevWord(string s, int index)
    {
        int i = Math.Clamp(index, 0, s.Length);
        while (i > 0 && !IsWordChar(s[i - 1])) i--;
        while (i > 0 && IsWordChar(s[i - 1])) i--;
        return i;
    }

    /// <summary>The end of the word after <paramref name="index"/> (Ctrl+Right).</summary>
    internal static int NextWord(string s, int index)
    {
        int i = Math.Clamp(index, 0, s.Length);
        while (i < s.Length && !IsWordChar(s[i])) i++;
        while (i < s.Length && IsWordChar(s[i])) i++;
        return i;
    }

    internal static (int From, int To) WordAt(string s, int index)
    {
        int i = Math.Clamp(index, 0, s.Length);
        if (s.Length == 0) return (0, 0);
        if (i == s.Length) i--;
        if (!IsWordChar(s[i])) return (i, Math.Min(s.Length, i + 1));
        int a = i, b = i;
        while (a > 0 && IsWordChar(s[a - 1])) a--;
        while (b < s.Length && IsWordChar(s[b])) b++;
        return (a, b);
    }
}

/// <summary>A single-line text field (Godot's LineEdit).</summary>
public class LineEdit : Control
{
    public new class SignalName : Control.SignalName
    {
        public static readonly StringName TextChanged = "text_changed";
        public static readonly StringName TextSubmitted = "text_submitted";
        public static readonly StringName TextChangeRejected = "text_change_rejected";
        public static readonly StringName EditingToggled = "editing_toggled";
    }

    public event Action<string> TextChanged { add => Connect(SignalName.TextChanged, Callable.From(value)); remove => Disconnect(SignalName.TextChanged, Callable.From(value)); }
    public event Action<string> TextSubmitted { add => Connect(SignalName.TextSubmitted, Callable.From(value)); remove => Disconnect(SignalName.TextSubmitted, Callable.From(value)); }
    public event Action<string> TextChangeRejected { add => Connect(SignalName.TextChangeRejected, Callable.From(value)); remove => Disconnect(SignalName.TextChangeRejected, Callable.From(value)); }
    public event Action<bool> EditingToggled { add => Connect(SignalName.EditingToggled, Callable.From(value)); remove => Disconnect(SignalName.EditingToggled, Callable.From(value)); }

    private string _text = "";
    private string _placeholder = "";
    private int _caret, _selAnchor = -1, _selFrom, _selTo, _maxLength;
    private bool _editable = true, _secret, _editing;
    private string _secretChar = "•";
    private HorizontalAlignment _align = HorizontalAlignment.Left;
    private float _scroll;
    private bool _mouseDown;
    private readonly List<(string Text, int Caret)> _undo = new(), _redo = new();

    public LineEdit()
    {
        FocusMode = FocusModeEnum.All;
        MouseDefaultCursorShape = CursorShape.Ibeam;
    }

    // ---- properties ----

    public string Text
    {
        get => _text;
        set
        {
            value ??= "";
            if (_maxLength > 0 && value.Length > _maxLength) value = value[.._maxLength];
            if (value == _text) return;
            _text = value;
            _caret = Math.Min(_caret, _text.Length);
            ClearSelection();
            _undo.Clear();
            _redo.Clear();
            UpdateMinimumSize();
            QueueRedraw();
        }
    }

    public string PlaceholderText { get => _placeholder; set { _placeholder = value ?? ""; QueueRedraw(); } }
    public bool Editable { get => _editable; set { _editable = value; QueueRedraw(); } }
    public bool Secret { get => _secret; set { _secret = value; QueueRedraw(); } }
    public string SecretCharacter { get => _secretChar; set { _secretChar = value.Length > 0 ? value : "•"; QueueRedraw(); } }
    public int MaxLength { get => _maxLength; set { _maxLength = Math.Max(0, value); if (_maxLength > 0 && _text.Length > _maxLength) Text = _text; } }
    public HorizontalAlignment Alignment { get => _align; set { _align = value; QueueRedraw(); } }
    public bool ExpandToTextLength { get; set; }
    public bool ClearButtonEnabled { get; set; }
    public bool CaretBlink { get; set; }
    public float CaretBlinkInterval { get; set; } = 0.65f;
    public bool CaretForceDisplayed { get; set; }
    public bool CaretMidGrapheme { get; set; }
    public bool SelectingEnabled { get; set; } = true;
    public bool DeselectOnFocusLossEnabled { get; set; } = true;
    public bool SelectAllOnFocus { get; set; }
    public bool ContextMenuEnabled { get; set; } = true;
    public bool ShortcutKeysEnabled { get; set; } = true;
    public bool MiddleMousePasteEnabled { get; set; } = true;
    public bool VirtualKeyboardEnabled { get; set; } = true;
    public bool VirtualKeyboardShowOnFocus { get; set; } = true;
    public int VirtualKeyboardType { get; set; }
    public bool DragAndDropSelectionEnabled { get; set; } = true;
    public Texture2D? RightIcon { get; set; }
    public bool DrawControlChars { get; set; }
    public bool EmojiMenuEnabled { get; set; } = true;
    public bool Flat { get; set; }
    public string Language { get; set; } = "";
    public new TextServer.Direction TextDirection { get; set; } = TextServer.Direction.Inherited;

    public int CaretColumn
    {
        get => _caret;
        set { _caret = Math.Clamp(value, 0, _text.Length); ScrollToCaret(); QueueRedraw(); }
    }

    // ---- selection ----

    private void ClearSelection() { _selAnchor = -1; _selFrom = _selTo = 0; }

    public bool HasSelection() => _selFrom != _selTo;

    public string GetSelectedText() => HasSelection() ? _text[_selFrom.._selTo] : "";

    public int GetSelectionFromColumn() => _selFrom;

    public int GetSelectionToColumn() => _selTo;

    public void Select(int from = 0, int to = -1)
    {
        if (!SelectingEnabled) return;
        if (to < 0) to = _text.Length;
        from = Math.Clamp(from, 0, _text.Length);
        to = Math.Clamp(to, 0, _text.Length);
        _selFrom = Math.Min(from, to);
        _selTo = Math.Max(from, to);
        _selAnchor = from;
        _caret = to;
        QueueRedraw();
    }

    public void SelectAll() => Select(0, _text.Length);

    public void Deselect() { ClearSelection(); QueueRedraw(); }

    private void Extend(int newCaret, bool shift)
    {
        newCaret = Math.Clamp(newCaret, 0, _text.Length);
        if (shift && SelectingEnabled)
        {
            if (_selAnchor < 0) _selAnchor = _caret;
            _selFrom = Math.Min(_selAnchor, newCaret);
            _selTo = Math.Max(_selAnchor, newCaret);
        }
        else ClearSelection();
        _caret = newCaret;
        ScrollToCaret();
        QueueRedraw();
    }

    // ---- editing ----

    public bool IsEditing() => _editing;

    public void Edit()
    {
        if (_editing) return;
        _editing = true;
        GrabFocus();
        EmitSignal(SignalName.EditingToggled, true);
    }

    public void Unedit()
    {
        if (!_editing) return;
        _editing = false;
        EmitSignal(SignalName.EditingToggled, false);
    }

    public void Clear()
    {
        if (_text.Length == 0) return;
        Replace(0, _text.Length, "", false);
    }

    public void DeleteText(int fromColumn, int toColumn)
    {
        fromColumn = Math.Clamp(fromColumn, 0, _text.Length);
        toColumn = Math.Clamp(toColumn, 0, _text.Length);
        if (fromColumn >= toColumn) return;
        Replace(fromColumn, toColumn, "", false);
    }

    /// <summary>Types text at the caret, replacing the selection (Godot's insert_text_at_caret).</summary>
    public void InsertTextAtCaret(string text)
    {
        int from = HasSelection() ? _selFrom : _caret, to = HasSelection() ? _selTo : _caret;
        Replace(from, to, text, true);
    }

    // Replaces [from, to) with text; the user path respects max_length and records undo, and always emits text_changed.
    private void Replace(int from, int to, string insert, bool caretAfter)
    {
        string before = _text;
        string head = _text[..from], tail = _text[to..];
        if (_maxLength > 0 && head.Length + insert.Length + tail.Length > _maxLength)
        {
            int room = Math.Max(0, _maxLength - head.Length - tail.Length);
            string rejected = insert[Math.Min(room, insert.Length)..];
            insert = insert[..Math.Min(room, insert.Length)];
            if (rejected.Length > 0) EmitSignal(SignalName.TextChangeRejected, rejected);
        }
        string next = head + insert + tail;
        if (next == before) { _caret = from + insert.Length; return; }
        _undo.Add((before, _caret));
        if (_undo.Count > 100) _undo.RemoveAt(0);
        _redo.Clear();
        _text = next;
        _caret = from + insert.Length;
        ClearSelection();
        UpdateMinimumSize();
        ScrollToCaret();
        QueueRedraw();
        EmitSignal(SignalName.TextChanged, _text);
    }

    private void DoUndo()
    {
        if (_undo.Count == 0) return;
        _redo.Add((_text, _caret));
        (string t, int c) = _undo[^1];
        _undo.RemoveAt(_undo.Count - 1);
        _text = t;
        _caret = Math.Min(c, t.Length);
        ClearSelection();
        UpdateMinimumSize();
        QueueRedraw();
        EmitSignal(SignalName.TextChanged, _text);
    }

    private void DoRedo()
    {
        if (_redo.Count == 0) return;
        _undo.Add((_text, _caret));
        (string t, int c) = _redo[^1];
        _redo.RemoveAt(_redo.Count - 1);
        _text = t;
        _caret = Math.Min(c, t.Length);
        ClearSelection();
        UpdateMinimumSize();
        QueueRedraw();
        EmitSignal(SignalName.TextChanged, _text);
    }

    public void Undo() => DoUndo();
    public void Redo() => DoRedo();

    public void Copy() { if (HasSelection() && !_secret) UiClipboard.Text = GetSelectedText(); }

    public void Cut()
    {
        if (!_editable || !HasSelection() || _secret) return;
        UiClipboard.Text = GetSelectedText();
        Replace(_selFrom, _selTo, "", true);
    }

    public void Paste()
    {
        if (!_editable) return;
        string clip = UiClipboard.Text.Replace("\r", "").Replace('\n', ' ');
        if (clip.Length > 0) InsertTextAtCaret(clip);
    }

    // ---- display ----

    private string Shown => _secret ? new string(_secretChar[0], _text.Length) : _text;

    private Font? FontNow => ThemeFont();
    private int SizeNow => ThemeFontSize();

    private float XOf(int col) => UiText.Width(FontNow, SizeNow, Shown[..Math.Clamp(col, 0, _text.Length)]);

    private Rect2 ContentRect()
    {
        StyleBox style = GetThemeStylebox(_editable ? "normal" : "read_only");
        return style.GetContentRect(LocalRect);
    }

    private void ScrollToCaret()
    {
        float w = ContentRect().Size.X;
        if (w <= 0) return;
        float cx = XOf(_caret), total = UiText.Width(FontNow, SizeNow, Shown);
        if (cx - _scroll > w) _scroll = cx - w;
        if (cx - _scroll < 0) _scroll = cx;
        _scroll = Math.Clamp(_scroll, 0f, Math.Max(0f, total - w + 2f));
    }

    internal override Vector2 NativeMinimumSize()
    {
        StyleBox style = GetThemeStylebox("normal");
        Font? font = FontNow;
        int px = SizeNow;
        float w = GetThemeConstant("minimum_character_width") * UiText.Width(font, px, "M");
        if (ExpandToTextLength) w = MathF.Max(w, UiText.Width(font, px, Shown) + 1f);
        return new Vector2(w, UiText.LineHeight(font, px)) + style.GetMinimumSize();
    }

    private int ColumnAt(float x)
    {
        Rect2 c = ContentRect();
        float lineW = UiText.Width(FontNow, SizeNow, Shown);
        float origin = c.Position.X - _scroll;
        if (lineW < c.Size.X)
        {
            if (_align == HorizontalAlignment.Center) origin = c.Position.X + (c.Size.X - lineW) / 2f;
            else if (_align == HorizontalAlignment.Right) origin = c.End.X - lineW;
        }
        float rel = x - origin;
        string s = Shown;
        int best = 0;
        float prev = 0f;
        for (int i = 1; i <= s.Length; i++)
        {
            float w = UiText.Width(FontNow, SizeNow, s[..i]);
            if (rel < (prev + w) / 2f) return best;
            best = i;
            prev = w;
        }
        return best;
    }

    // ---- input ----

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left:
                if (mb.Pressed)
                {
                    GrabFocus();
                    if (mb.DoubleClick && SelectingEnabled) { (int a, int b) = TextOps.WordAt(_text, ColumnAt(mb.Position.X)); Select(a, b); _mouseDown = false; }
                    else
                    {
                        _mouseDown = true;
                        ClearSelection();
                        _caret = ColumnAt(mb.Position.X);
                        _selAnchor = _caret;
                        QueueRedraw();
                    }
                }
                else _mouseDown = false;
                AcceptEvent();
                break;
            case InputEventMouseMotion mm when _mouseDown && SelectingEnabled:
                Extend(ColumnAt(mm.Position.X), true);
                AcceptEvent();
                break;
            case InputEventKey { Pressed: true } k:
                if (OnKey(k)) AcceptEvent();
                break;
        }
    }

    private bool OnKey(InputEventKey k)
    {
        bool ctrl = k.CtrlPressed || k.MetaPressed, shift = k.ShiftPressed;
        if (ctrl && ShortcutKeysEnabled)
        {
            switch (k.Keycode)
            {
                case Key.A: SelectAll(); return true;
                case Key.C: Copy(); return true;
                case Key.X: Cut(); return true;
                case Key.V: Paste(); return true;
                case Key.Z when _editable: if (shift) DoRedo(); else DoUndo(); return true;
                case Key.Y when _editable: DoRedo(); return true;
            }
        }
        switch (k.Keycode)
        {
            case Key.Left: Extend(HasSelection() && !shift ? _selFrom : ctrl ? TextOps.PrevWord(_text, _caret) : _caret - 1, shift); return true;
            case Key.Right: Extend(HasSelection() && !shift ? _selTo : ctrl ? TextOps.NextWord(_text, _caret) : _caret + 1, shift); return true;
            case Key.Home: Extend(0, shift); return true;
            case Key.End: Extend(_text.Length, shift); return true;
            case Key.Enter:
            case Key.KpEnter:
                EmitSignal(SignalName.TextSubmitted, _text);
                return true;
            case Key.Escape when _editing: Unedit(); return true;
        }
        if (!_editable) return false;
        switch (k.Keycode)
        {
            case Key.Backspace:
                if (HasSelection()) Replace(_selFrom, _selTo, "", true);
                else if (_caret > 0) { int from = ctrl ? TextOps.PrevWord(_text, _caret) : _caret - 1; Replace(from, _caret, "", true); }
                return true;
            case Key.Delete:
                if (HasSelection()) Replace(_selFrom, _selTo, "", true);
                else if (_caret < _text.Length) { int to = ctrl ? TextOps.NextWord(_text, _caret) : _caret + 1; Replace(_caret, to, "", true); }
                return true;
        }
        if (k.Unicode >= 32 && k.Unicode != 127 && !(ctrl && !k.AltPressed))
        {
            InsertTextAtCaret(char.ConvertFromUtf32((int)k.Unicode));
            return true;
        }
        return false;
    }

    public override void _Notification(int what)
    {
        switch (what)
        {
            case NotificationFocusEnter:
                if (SelectAllOnFocus) SelectAll();
                if (!_editing) { _editing = true; EmitSignal(SignalName.EditingToggled, true); }
                QueueRedraw();
                break;
            case NotificationFocusExit:
                if (DeselectOnFocusLossEnabled) ClearSelection();
                _mouseDown = false;
                if (_editing) { _editing = false; EmitSignal(SignalName.EditingToggled, false); }
                QueueRedraw();
                break;
            case NotificationResized:
                ScrollToCaret();
                break;
        }
    }

    // ---- drawing ----

    public override void _Draw()
    {
        StyleBox style = GetThemeStylebox(_editable ? "normal" : "read_only");
        if (!Flat) DrawStyleBox(style, LocalRect);
        if (HasFocus()) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        Rect2 c = style.GetContentRect(LocalRect);
        Font? font = FontNow;
        int px = SizeNow;
        float lh = UiText.LineHeight(font, px);
        string shown = Shown;
        float y = c.Position.Y + (c.Size.Y - lh) / 2f;
        PushClip(c);
        if (shown.Length == 0)
        {
            if (_placeholder.Length > 0) DrawTextLine(new Vector2(c.Position.X, y), _placeholder, font, px, GetThemeColor("font_placeholder_color"), ThemeTextFx());
        }
        else
        {
            float lineW = UiText.Width(font, px, shown);
            float origin = c.Position.X - _scroll;
            if (lineW < c.Size.X)
            {
                if (_align == HorizontalAlignment.Center) origin = c.Position.X + (c.Size.X - lineW) / 2f;
                else if (_align == HorizontalAlignment.Right) origin = c.End.X - lineW;
            }
            if (HasSelection())
            {
                float x0 = origin + XOf(_selFrom), x1 = origin + XOf(_selTo);
                DrawRect(new Rect2(x0, c.Position.Y, x1 - x0, c.Size.Y), GetThemeColor("selection_color"));
            }
            DrawTextLine(new Vector2(origin, y), shown, font, px, GetThemeColor(_editable ? "font_color" : "font_uneditable_color"), ThemeTextFx());
            if (HasFocus() && _editable) DrawRect(new Rect2(origin + XOf(_caret), c.Position.Y + (c.Size.Y - lh) / 2f, Math.Max(1, GetThemeConstant("caret_width")), lh), GetThemeColor("caret_color"));
        }
        if (shown.Length == 0 && HasFocus() && _editable)
        {
            float cx = _align == HorizontalAlignment.Center ? c.Position.X + c.Size.X / 2f : _align == HorizontalAlignment.Right ? c.End.X : c.Position.X;
            DrawRect(new Rect2(cx, y, Math.Max(1, GetThemeConstant("caret_width")), lh), GetThemeColor("caret_color"));
        }
        PopClip();
    }
}
