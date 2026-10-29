// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// TextEdit (G5 U2): multi-line text. Lines, a caret (line, column) and a selection; typing, newline, tab, backspace / delete, arrows with a remembered column,
// page and document jumps, word jumps, copy / cut / paste, undo / redo, search, soft wrapping at the width, mouse caret / drag / double click, the wheel,
// text_changed / caret_changed / text_set. No syntax highlighting, no gutters, no minimap, and the scroll bars are not drawn (scroll_vertical and
// scroll_horizontal are the scroll position).

namespace Aver.Fabric;

/// <summary>A multi-line text editor (Godot's TextEdit).</summary>
public class TextEdit : Control
{
    public enum LineWrappingMode { None = 0, Boundary = 1 }

    [Flags]
    public enum SearchFlags { MatchCase = 1, WholeWords = 2, Backwards = 4 }

    public new class SignalName : Control.SignalName
    {
        public static readonly StringName TextSet = "text_set";
        public static readonly StringName TextChanged = "text_changed";
        public static readonly StringName LinesEditedFrom = "lines_edited_from";
        public static readonly StringName CaretChanged = "caret_changed";
    }

    public event Action TextSet { add => Connect(SignalName.TextSet, Callable.From(value)); remove => Disconnect(SignalName.TextSet, Callable.From(value)); }
    public event Action TextChanged { add => Connect(SignalName.TextChanged, Callable.From(value)); remove => Disconnect(SignalName.TextChanged, Callable.From(value)); }
    public event Action CaretChanged { add => Connect(SignalName.CaretChanged, Callable.From(value)); remove => Disconnect(SignalName.CaretChanged, Callable.From(value)); }

    private readonly record struct Pos(int Line, int Col) : IComparable<Pos>
    {
        public int CompareTo(Pos o) => Line != o.Line ? Line.CompareTo(o.Line) : Col.CompareTo(o.Col);
        public static bool operator <(Pos a, Pos b) => a.CompareTo(b) < 0;
        public static bool operator >(Pos a, Pos b) => a.CompareTo(b) > 0;
        public static bool operator <=(Pos a, Pos b) => a.CompareTo(b) <= 0;
        public static bool operator >=(Pos a, Pos b) => a.CompareTo(b) >= 0;
    }

    private List<string> _lines = new() { "" };
    private Pos _caret, _anchor = new(-1, 0), _selFrom, _selTo;
    private bool _hasSel, _mouseDown;
    private float _desiredX = -1f, _scrollH;
    private double _scrollV;
    private string _placeholder = "";
    private bool _editable = true;
    private LineWrappingMode _wrap;
    private readonly List<(List<string> Lines, Pos Caret)> _undo = new(), _redo = new();

    public TextEdit()
    {
        FocusMode = FocusModeEnum.All;
        MouseDefaultCursorShape = CursorShape.Ibeam;
    }

    // ---- properties ----

    public string Text
    {
        get => string.Join("\n", _lines);
        set
        {
            _lines = new List<string>((value ?? "").Replace("\r\n", "\n").Replace('\r', '\n').Split('\n'));
            _caret = default;
            ClearSel();
            _undo.Clear();
            _redo.Clear();
            _scrollV = 0;
            _scrollH = 0;
            QueueRedraw();
            EmitSignal(SignalName.TextSet);
        }
    }

    public string PlaceholderText { get => _placeholder; set { _placeholder = value ?? ""; QueueRedraw(); } }
    public bool Editable { get => _editable; set { _editable = value; QueueRedraw(); } }
    public LineWrappingMode WrapMode { get => _wrap; set { _wrap = value; QueueRedraw(); } }
    public bool SelectingEnabled { get; set; } = true;
    public bool DeselectOnFocusLossEnabled { get; set; } = true;
    public bool ContextMenuEnabled { get; set; } = true;
    public bool ShortcutKeysEnabled { get; set; } = true;
    public bool MiddleMousePasteEnabled { get; set; } = true;
    public bool VirtualKeyboardEnabled { get; set; } = true;
    public bool CaretBlink { get; set; }
    public float CaretBlinkInterval { get; set; } = 0.65f;
    public bool CaretMoveOnRightClick { get; set; } = true;
    public bool CaretMultiple { get; set; } = true;
    public int CaretType { get; set; }
    public bool EmojiMenuEnabled { get; set; } = true;
    public bool ScrollSmooth { get; set; }
    public float ScrollVScrollSpeed { get; set; } = 80f;
    public int MinimapWidth { get; set; } = 80;
    public bool DragAndDropSelectionEnabled { get; set; } = true;
    public bool UseDefaultWordSeparators { get; set; } = true;
    public bool UseCustomWordSeparators { get; set; }
    public string CustomWordSeparators { get; set; } = "";
    public bool HighlightAllOccurrences { get; set; }
    public bool HighlightCurrentLine { get; set; }
    public bool DrawControlChars { get; set; }
    public bool DrawTabs { get; set; }
    public bool DrawSpaces { get; set; }
    public bool ScrollFitContentHeight { get; set; }
    public bool ScrollFitContentWidth { get; set; }
    public bool ScrollPastEndOfFile { get; set; }
    public bool SmoothScrollEnabled { get; set; }
    public float VScrollSpeed { get; set; } = 80f;
    public bool MinimapDraw { get; set; }
    public bool IndentUseSpaces { get; set; }
    public int IndentSize { get; set; } = 4;
    public Resource? SyntaxHighlighter { get; set; }
    public string Language { get; set; } = "";
    public new TextServer.Direction TextDirection { get; set; } = TextServer.Direction.Inherited;

    public double ScrollVertical { get => _scrollV; set { _scrollV = Math.Max(0, value); QueueRedraw(); } }
    public int ScrollHorizontal { get => (int)_scrollH; set { _scrollH = Math.Max(0, value); QueueRedraw(); } }

    // ---- lines and caret ----

    public int GetLineCount() => _lines.Count;

    public string GetLine(int line) => line >= 0 && line < _lines.Count ? _lines[line] : "";

    public void SetLine(int line, string newText)
    {
        if (line < 0 || line >= _lines.Count) return;
        Snapshot();
        _lines[line] = newText.Replace("\n", "");
        _caret = Clamp(_caret);
        Edited();
    }

    public int GetLineLength(int line) => GetLine(line).Length;

    public int GetCaretLine() => _caret.Line;

    public int GetCaretColumn() => _caret.Col;

    public void SetCaretLine(int line, bool adjustViewport = true) { _caret = Clamp(new Pos(line, _caret.Col)); if (adjustViewport) EnsureCaretVisible(); CaretMoved(); }

    public void SetCaretColumn(int column, bool adjustViewport = true) { _caret = Clamp(new Pos(_caret.Line, column)); _desiredX = -1; if (adjustViewport) EnsureCaretVisible(); CaretMoved(); }

    private Pos Clamp(Pos p)
    {
        int l = Math.Clamp(p.Line, 0, _lines.Count - 1);
        return new Pos(l, Math.Clamp(p.Col, 0, _lines[l].Length));
    }

    private void CaretMoved()
    {
        QueueRedraw();
        EmitSignal(SignalName.CaretChanged);
    }

    // ---- selection ----

    private void ClearSel() { _hasSel = false; _anchor = new Pos(-1, 0); }

    public bool HasSelection() => _hasSel;

    public int GetSelectionFromLine() => _hasSel ? _selFrom.Line : -1;
    public int GetSelectionFromColumn() => _hasSel ? _selFrom.Col : -1;
    public int GetSelectionToLine() => _hasSel ? _selTo.Line : -1;
    public int GetSelectionToColumn() => _hasSel ? _selTo.Col : -1;

    public string GetSelectedText() => _hasSel ? Range(_selFrom, _selTo) : "";

    public void Select(int originLine, int originColumn, int caretLine, int caretColumn)
    {
        if (!SelectingEnabled) return;
        Pos a = Clamp(new Pos(originLine, originColumn)), b = Clamp(new Pos(caretLine, caretColumn));
        _anchor = a;
        _caret = b;
        SetSel(a, b);
        CaretMoved();
    }

    public void SelectAll() => Select(0, 0, _lines.Count - 1, _lines[^1].Length);

    public void SelectWordUnderCaret()
    {
        (int a, int b) = TextOps.WordAt(_lines[_caret.Line], _caret.Col);
        Select(_caret.Line, a, _caret.Line, b);
    }

    public void Deselect() { ClearSel(); QueueRedraw(); }

    private void SetSel(Pos a, Pos b)
    {
        _hasSel = a != b;
        _selFrom = a < b ? a : b;
        _selTo = a < b ? b : a;
        QueueRedraw();
    }

    private string Range(Pos from, Pos to)
    {
        if (from.Line == to.Line) return _lines[from.Line][from.Col..to.Col];
        var parts = new List<string> { _lines[from.Line][from.Col..] };
        for (int l = from.Line + 1; l < to.Line; l++) parts.Add(_lines[l]);
        parts.Add(_lines[to.Line][..to.Col]);
        return string.Join("\n", parts);
    }

    private void MoveCaret(Pos to, bool shift)
    {
        to = Clamp(to);
        if (shift && SelectingEnabled)
        {
            if (_anchor.Line < 0) _anchor = _caret;
            SetSel(_anchor, to);
        }
        else ClearSel();
        _caret = to;
        EnsureCaretVisible();
        CaretMoved();
    }

    // ---- editing ----

    private void Snapshot()
    {
        _undo.Add((new List<string>(_lines), _caret));
        if (_undo.Count > 100) _undo.RemoveAt(0);
        _redo.Clear();
    }

    private void Edited()
    {
        QueueRedraw();
        EmitSignal(SignalName.TextChanged);
    }

    private Pos InsertAt(Pos at, string text)
    {
        string[] parts = text.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
        string tail = _lines[at.Line][at.Col..];
        _lines[at.Line] = _lines[at.Line][..at.Col] + parts[0];
        if (parts.Length == 1)
        {
            _lines[at.Line] += tail;
            return new Pos(at.Line, at.Col + parts[0].Length);
        }
        for (int i = 1; i < parts.Length; i++) _lines.Insert(at.Line + i, parts[i]);
        int last = at.Line + parts.Length - 1;
        int col = _lines[last].Length;
        _lines[last] += tail;
        return new Pos(last, col);
    }

    private void RemoveRange(Pos from, Pos to)
    {
        string tail = _lines[to.Line][to.Col..];
        _lines[from.Line] = _lines[from.Line][..from.Col] + tail;
        if (to.Line > from.Line) _lines.RemoveRange(from.Line + 1, to.Line - from.Line);
    }

    /// <summary>Types text at the caret, replacing the selection (Godot's insert_text_at_caret).</summary>
    public void InsertTextAtCaret(string text, int caretIndex = -1)
    {
        Snapshot();
        Pos at = _caret;
        if (_hasSel) { RemoveRange(_selFrom, _selTo); at = _selFrom; ClearSel(); }
        _caret = InsertAt(at, text);
        _desiredX = -1;
        EnsureCaretVisible();
        Edited();
        CaretMoved();
    }

    public void InsertText(string text, int line, int column, bool beforeSelectionBegin = true, bool beforeSelectionEnd = false)
    {
        Snapshot();
        Pos end = InsertAt(Clamp(new Pos(line, column)), text);
        if (_caret.Line == line && _caret.Col >= column) _caret = end;
        Edited();
    }

    public void InsertLineAt(int line, string text)
    {
        Snapshot();
        _lines.Insert(Math.Clamp(line, 0, _lines.Count), text.Replace("\n", ""));
        Edited();
    }

    public void RemoveLineAt(int line, bool moveCaretColumn = true)
    {
        if (line < 0 || line >= _lines.Count) return;
        Snapshot();
        _lines.RemoveAt(line);
        if (_lines.Count == 0) _lines.Add("");
        _caret = Clamp(_caret);
        Edited();
    }

    public void RemoveText(int fromLine, int fromColumn, int toLine, int toColumn)
    {
        Pos a = Clamp(new Pos(fromLine, fromColumn)), b = Clamp(new Pos(toLine, toColumn));
        if (a > b) (a, b) = (b, a);
        if (a == b) return;
        Snapshot();
        RemoveRange(a, b);
        _caret = Clamp(_caret);
        ClearSel();
        Edited();
    }

    public void DeleteSelection()
    {
        if (!_hasSel) return;
        Snapshot();
        RemoveRange(_selFrom, _selTo);
        _caret = _selFrom;
        ClearSel();
        Edited();
        CaretMoved();
    }

    public void Clear()
    {
        Snapshot();
        _lines = new List<string> { "" };
        _caret = default;
        ClearSel();
        Edited();
    }

    public void Undo()
    {
        if (_undo.Count == 0) return;
        _redo.Add((new List<string>(_lines), _caret));
        (List<string> l, Pos c) = _undo[^1];
        _undo.RemoveAt(_undo.Count - 1);
        _lines = l;
        _caret = Clamp(c);
        ClearSel();
        Edited();
        CaretMoved();
    }

    public void Redo()
    {
        if (_redo.Count == 0) return;
        _undo.Add((new List<string>(_lines), _caret));
        (List<string> l, Pos c) = _redo[^1];
        _redo.RemoveAt(_redo.Count - 1);
        _lines = l;
        _caret = Clamp(c);
        ClearSel();
        Edited();
        CaretMoved();
    }

    public bool HasUndo() => _undo.Count > 0;
    public bool HasRedo() => _redo.Count > 0;
    public void ClearUndoHistory() { _undo.Clear(); _redo.Clear(); }

    public void Copy() { if (_hasSel) UiClipboard.Text = GetSelectedText(); else UiClipboard.Text = _lines[_caret.Line] + "\n"; }

    public void Cut()
    {
        if (!_editable) return;
        Copy();
        if (_hasSel) DeleteSelection();
        else RemoveLineAt(_caret.Line);
    }

    public void Paste()
    {
        if (_editable && UiClipboard.Text.Length > 0) InsertTextAtCaret(UiClipboard.Text);
    }

    /// <summary>Finds <paramref name="text"/> from a position; the result is (column, line) or (-1, -1).</summary>
    public Vector2I Search(string text, uint flags, int fromLine, int fromColumn)
    {
        var cmp = (flags & (uint)SearchFlags.MatchCase) != 0 ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase;
        bool whole = (flags & (uint)SearchFlags.WholeWords) != 0, back = (flags & (uint)SearchFlags.Backwards) != 0;
        if (text.Length == 0) return new Vector2I(-1, -1);
        for (int n = 0; n <= _lines.Count; n++)
        {
            int l = back ? fromLine - n : fromLine + n;
            if (l < 0 || l >= _lines.Count) break;
            string s = _lines[l];
            int col = n == 0 ? fromColumn : back ? s.Length : 0;
            int idx = back ? (col <= 0 ? -1 : s.LastIndexOf(text, Math.Min(s.Length - 1, col - 1), cmp)) : (col > s.Length ? -1 : s.IndexOf(text, col, cmp));
            while (idx >= 0)
            {
                bool ok = !whole || ((idx == 0 || !TextOps.IsWordChar(s[idx - 1])) && (idx + text.Length >= s.Length || !TextOps.IsWordChar(s[idx + text.Length])));
                if (ok) return new Vector2I(idx, l);
                idx = back ? (idx == 0 ? -1 : s.LastIndexOf(text, idx - 1, cmp)) : s.IndexOf(text, idx + 1, cmp);
            }
        }
        return new Vector2I(-1, -1);
    }

    // ---- layout ----

    private Font? FontNow => ThemeFont();
    private int SizeNow => ThemeFontSize();

    private Rect2 Content() => GetThemeStylebox(_editable ? "normal" : "read_only").GetContentRect(LocalRect);

    private float Pitch => UiText.LineHeight(FontNow, SizeNow) + GetThemeConstant("line_spacing");

    private float WrapWidth => _wrap == LineWrappingMode.Boundary ? Content().Size.X : 0f;

    private TextLines VisualOf(int line) => TextLines.Build(_lines[line], FontNow, SizeNow, WrapWidth, _wrap == LineWrappingMode.Boundary ? TextLines.Wrap.Smart : TextLines.Wrap.Off);

    // The visual line index at which a logical line starts.
    private int VisualTop(int line)
    {
        int n = 0;
        for (int l = 0; l < line; l++) n += _wrap == LineWrappingMode.Boundary ? VisualOf(l).Count : 1;
        return n;
    }

    private int VisualCount()
    {
        int n = 0;
        for (int l = 0; l < _lines.Count; l++) n += _wrap == LineWrappingMode.Boundary ? VisualOf(l).Count : 1;
        return n;
    }

    /// <summary>The number of lines that fit the view.</summary>
    public int GetVisibleLineCount() => (int)MathF.Max(1f, MathF.Floor(Content().Size.Y / Pitch));

    public int GetFirstVisibleLine() => (int)_scrollV;

    public int GetTotalVisibleLineCount() => VisualCount();

    // (visual line index, x) of a caret position.
    private (int Visual, float X) CaretVisual(Pos p)
    {
        if (_wrap != LineWrappingMode.Boundary) return (p.Line, UiText.Width(FontNow, SizeNow, _lines[p.Line][..p.Col]));
        TextLines v = VisualOf(p.Line);
        int k = 0;
        for (int i = 0; i < v.Count; i++)
        {
            TextLines.Line li = v.Items[i];
            if (p.Col >= li.Start && (p.Col <= li.Start + li.Length || i == v.Count - 1)) { k = i; break; }
        }
        TextLines.Line cur = v.Items[k];
        int upto = Math.Clamp(p.Col - cur.Start, 0, cur.Length);
        return (VisualTop(p.Line) + k, UiText.Width(FontNow, SizeNow, _lines[p.Line].Substring(cur.Start, upto)));
    }

    private void EnsureCaretVisible()
    {
        (int visual, float x) = CaretVisual(_caret);
        int visible = GetVisibleLineCount();
        if (visual < _scrollV) _scrollV = visual;
        else if (visual >= _scrollV + visible) _scrollV = visual - visible + 1;
        if (_wrap == LineWrappingMode.None)
        {
            float w = Content().Size.X;
            if (x - _scrollH > w - 2) _scrollH = x - w + 2;
            if (x - _scrollH < 0) _scrollH = x;
            _scrollH = Math.Max(0f, _scrollH);
        }
        else _scrollH = 0;
        QueueRedraw();
    }

    // The caret position under a point of the control.
    private Pos PosAt(Vector2 point)
    {
        Rect2 c = Content();
        int visual = (int)MathF.Floor((point.Y - c.Position.Y) / Pitch + (float)_scrollV);
        visual = Math.Clamp(visual, 0, Math.Max(0, VisualCount() - 1));
        int line = 0, k = 0, acc = 0;
        for (int l = 0; l < _lines.Count; l++)
        {
            int n = _wrap == LineWrappingMode.Boundary ? VisualOf(l).Count : 1;
            if (visual < acc + n) { line = l; k = visual - acc; break; }
            acc += n;
            line = l;
            k = n - 1;
        }
        string s = _lines[line];
        int start = 0, len = s.Length;
        if (_wrap == LineWrappingMode.Boundary)
        {
            TextLines v = VisualOf(line);
            start = v.Items[k].Start;
            len = v.Items[k].Length;
        }
        float rel = point.X - c.Position.X + _scrollH;
        int best = 0;
        float prev = 0f;
        for (int i = 1; i <= len; i++)
        {
            float w = UiText.Width(FontNow, SizeNow, s.Substring(start, i));
            if (rel < (prev + w) / 2f) break;
            best = i;
            prev = w;
        }
        return new Pos(line, start + best);
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
                    Pos at = PosAt(mb.Position);
                    if (mb.DoubleClick && SelectingEnabled) { _caret = at; SelectWordUnderCaret(); _mouseDown = false; }
                    else
                    {
                        _mouseDown = true;
                        ClearSel();
                        _caret = at;
                        _anchor = at;
                        _desiredX = -1;
                        CaretMoved();
                    }
                }
                else _mouseDown = false;
                AcceptEvent();
                break;
            case InputEventMouseMotion mm when _mouseDown && SelectingEnabled:
                MoveCaret(PosAt(mm.Position), true);
                AcceptEvent();
                break;
            case InputEventMouseButton wheel when wheel.Pressed && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown:
                _scrollV = Math.Clamp(_scrollV + (wheel.ButtonIndex == MouseButton.WheelUp ? -3 : 3), 0, Math.Max(0, VisualCount() - 1));
                QueueRedraw();
                AcceptEvent();
                break;
            case InputEventKey { Pressed: true } k:
                if (OnKey(k)) AcceptEvent();
                break;
        }
    }

    private void Vertical(int dir, bool shift, int amount = 1)
    {
        (int visual, float x) = CaretVisual(_caret);
        if (_desiredX < 0) _desiredX = x;
        int target = Math.Clamp(visual + dir * amount, 0, Math.Max(0, VisualCount() - 1));
        Pos p = PosAt(new Vector2(Content().Position.X + _desiredX - _scrollH, Content().Position.Y + (target - (float)_scrollV + 0.5f) * Pitch));
        float keep = _desiredX;
        MoveCaret(p, shift);
        _desiredX = keep;
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
                case Key.Z when _editable: if (shift) Redo(); else Undo(); return true;
                case Key.Y when _editable: Redo(); return true;
            }
        }
        string line = _lines[_caret.Line];
        switch (k.Keycode)
        {
            case Key.Left:
                _desiredX = -1;
                if (_hasSel && !shift) MoveCaret(_selFrom, false);
                else if (ctrl) MoveCaret(_caret.Col > 0 ? new Pos(_caret.Line, TextOps.PrevWord(line, _caret.Col)) : _caret.Line > 0 ? new Pos(_caret.Line - 1, _lines[_caret.Line - 1].Length) : _caret, shift);
                else MoveCaret(_caret.Col > 0 ? new Pos(_caret.Line, _caret.Col - 1) : _caret.Line > 0 ? new Pos(_caret.Line - 1, _lines[_caret.Line - 1].Length) : _caret, shift);
                return true;
            case Key.Right:
                _desiredX = -1;
                if (_hasSel && !shift) MoveCaret(_selTo, false);
                else if (ctrl) MoveCaret(_caret.Col < line.Length ? new Pos(_caret.Line, TextOps.NextWord(line, _caret.Col)) : _caret.Line < _lines.Count - 1 ? new Pos(_caret.Line + 1, 0) : _caret, shift);
                else MoveCaret(_caret.Col < line.Length ? new Pos(_caret.Line, _caret.Col + 1) : _caret.Line < _lines.Count - 1 ? new Pos(_caret.Line + 1, 0) : _caret, shift);
                return true;
            case Key.Up: Vertical(-1, shift); return true;
            case Key.Down: Vertical(1, shift); return true;
            case Key.Pageup: Vertical(-1, shift, GetVisibleLineCount()); return true;
            case Key.Pagedown: Vertical(1, shift, GetVisibleLineCount()); return true;
            case Key.Home: _desiredX = -1; MoveCaret(ctrl ? new Pos(0, 0) : new Pos(_caret.Line, 0), shift); return true;
            case Key.End: _desiredX = -1; MoveCaret(ctrl ? new Pos(_lines.Count - 1, _lines[^1].Length) : new Pos(_caret.Line, line.Length), shift); return true;
        }
        if (!_editable) return false;
        switch (k.Keycode)
        {
            case Key.Enter:
            case Key.KpEnter:
                InsertTextAtCaret("\n");
                return true;
            case Key.Tab when !ctrl:
                InsertTextAtCaret(IndentUseSpaces ? new string(' ', IndentSize) : "\t");
                return true;
            case Key.Backspace:
                if (_hasSel) DeleteSelection();
                else if (_caret.Col > 0 || _caret.Line > 0)
                {
                    Pos from = _caret.Col > 0 ? new Pos(_caret.Line, ctrl ? TextOps.PrevWord(line, _caret.Col) : _caret.Col - 1) : new Pos(_caret.Line - 1, _lines[_caret.Line - 1].Length);
                    RemoveText(from.Line, from.Col, _caret.Line, _caret.Col);
                    _caret = from;
                    EnsureCaretVisible();
                    CaretMoved();
                }
                return true;
            case Key.Delete:
                if (_hasSel) DeleteSelection();
                else if (_caret.Col < line.Length || _caret.Line < _lines.Count - 1)
                {
                    Pos to = _caret.Col < line.Length ? new Pos(_caret.Line, ctrl ? TextOps.NextWord(line, _caret.Col) : _caret.Col + 1) : new Pos(_caret.Line + 1, 0);
                    RemoveText(_caret.Line, _caret.Col, to.Line, to.Col);
                }
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
            case NotificationFocusExit:
                if (DeselectOnFocusLossEnabled) ClearSel();
                _mouseDown = false;
                QueueRedraw();
                break;
            case NotificationFocusEnter:
                QueueRedraw();
                break;
            case NotificationResized:
                EnsureCaretVisible();
                break;
        }
    }

    // ---- drawing ----

    public override void _Draw()
    {
        StyleBox style = GetThemeStylebox(_editable ? "normal" : "read_only");
        DrawStyleBox(style, LocalRect);
        if (HasFocus()) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        Rect2 c = style.GetContentRect(LocalRect);
        Font? font = FontNow;
        int px = SizeNow;
        float lh = UiText.LineHeight(font, px), pitch = Pitch;
        PushClip(c);
        bool empty = _lines.Count == 1 && _lines[0].Length == 0;
        if (empty && _placeholder.Length > 0) DrawTextLine(c.Position, _placeholder, font, px, GetThemeColor("font_placeholder_color"), ThemeTextFx());
        Color color = GetThemeColor(_editable ? "font_color" : "font_readonly_color");
        CanvasItem.TextFx fx = ThemeTextFx();
        int visualIndex = 0;
        int first = (int)_scrollV, last = first + GetVisibleLineCount() + 1;
        for (int l = 0; l < _lines.Count && visualIndex <= last; l++)
        {
            string s = _lines[l];
            TextLines v = _wrap == LineWrappingMode.Boundary ? VisualOf(l) : null!;
            int n = v?.Count ?? 1;
            for (int k = 0; k < n; k++, visualIndex++)
            {
                if (visualIndex < first || visualIndex > last) continue;
                int start = v != null ? v.Items[k].Start : 0, len = v != null ? v.Items[k].Length : s.Length;
                float y = c.Position.Y + (visualIndex - (float)_scrollV) * pitch;
                float x0 = c.Position.X - (v != null ? 0f : _scrollH);
                if (_hasSel && l >= _selFrom.Line && l <= _selTo.Line)
                {
                    int a = l == _selFrom.Line ? Math.Clamp(_selFrom.Col, start, start + len) : start;
                    int b = l == _selTo.Line ? Math.Clamp(_selTo.Col, start, start + len) : start + len;
                    bool nl = l < _selTo.Line && k == n - 1;
                    if (b > a || nl)
                    {
                        float xa = x0 + UiText.Width(font, px, s.Substring(start, a - start)), xb = x0 + UiText.Width(font, px, s.Substring(start, b - start)) + (nl ? px * 0.5f : 0f);
                        DrawRect(new Rect2(xa, y, xb - xa, pitch), GetThemeColor("selection_color"));
                    }
                }
                if (len > 0) DrawTextLine(new Vector2(x0, y), s.Substring(start, len), font, px, color, fx);
            }
        }
        if (HasFocus() && _editable)
        {
            (int cv, float cx) = CaretVisual(_caret);
            DrawRect(new Rect2(c.Position.X + cx - (_wrap == LineWrappingMode.None ? _scrollH : 0f), c.Position.Y + (cv - (float)_scrollV) * pitch, Math.Max(1, GetThemeConstant("caret_width")), lh), GetThemeColor("caret_color"));
        }
        PopClip();
    }
}
