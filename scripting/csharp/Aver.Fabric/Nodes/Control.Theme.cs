// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Control's theme lookup (G5 U1), Godot's order: the control's own override, then the nearest theme in the node chain (a Control's `theme` from itself up
// through its parents), then the project theme, then the built-in default; inside each theme the type list runs
// theme_type_variation (and the bases it names) before the class chain. The same rules, in C++, are aver/ui/UiThemeSheet.hpp.

namespace Aver.Fabric;

public partial class Control
{
    private Theme? _theme;
    private Action? _themeHandler;
    private string _variation = "";
    private Dictionary<string, Color>? _colorO;
    private Dictionary<string, int>? _constO, _fontSizeO;
    private Dictionary<string, Font>? _fontO;
    private Dictionary<string, Texture2D>? _iconO;
    private Dictionary<string, StyleBox>? _styleO;

    /// <summary>The theme this control and its descendants use before the project's (Godot's theme).</summary>
    public Theme? Theme
    {
        get => _theme;
        set
        {
            if (_theme == value) return;
            if (_theme != null && _themeHandler != null) _theme.Changed -= _themeHandler;
            _theme = value;
            if (_theme != null) _theme.Changed += _themeHandler ??= NotifyThemeChanged;
            NotifyThemeChanged();
        }
    }

    /// <summary>A type of the theme to look this control's items up under before its class (Godot's theme_type_variation).</summary>
    public StringName ThemeTypeVariation
    {
        get => _variation;
        set
        {
            _variation = value.ToString();
            NotifyThemeChanged();
        }
    }

    // The control's Godot class chain: its own class and its bases up to Control, only those Aver.Fabric defines (a script's class is not a theme type).
    private static readonly Dictionary<System.Type, string[]> s_classChains = new();

    // A converted Godot class the runtime has no class for yet (a Button before the widget package) is a Control with `_class` metadata: its theme items are
    // looked up under that Godot class first.
    internal string[] ClassChain()
    {
        string[] native = ChainOf(GetType());
        if (HasMeta("_class"))
        {
            string named = GetMeta("_class").AsString();
            if (named.Length > 0 && Array.IndexOf(native, named) < 0) return new[] { named }.Concat(native).ToArray();
        }
        return native;
    }

    internal static string[] ChainOf(System.Type t)
    {
        lock (s_classChains)
        {
            if (s_classChains.TryGetValue(t, out string[]? known)) return known;
            var list = new List<string>();
            for (System.Type? c = t; c != null && typeof(Control).IsAssignableFrom(c); c = c.BaseType)
                if (c.Assembly == typeof(Control).Assembly) list.Add(c.Name);
            return s_classChains[t] = list.ToArray();
        }
    }

    /// <summary>The Godot class name of this control (the nearest class Aver.Fabric defines).</summary>
    public string GetThemeClassName() => ClassChain() is { Length: > 0 } chain ? chain[0] : nameof(Control);

    /// <summary>The themes that answer for this control, nearest first: each Control's own theme from this one up, the project theme, the default.</summary>
    internal List<Theme> ThemeChain()
    {
        var chain = new List<Theme>();
        for (Node? n = this; n != null; n = n.GetParent())
            if (n is Control c && c._theme != null) chain.Add(c._theme);
        if (ThemeDB.GetProjectTheme() is { } project) chain.Add(project);
        chain.Add(ThemeDB.GetDefaultTheme());
        return chain;
    }

    /// <summary>The type list for a lookup: [variation, the bases it names, the class chain], most specific first, no repeats.</summary>
    internal List<string> ThemeTypes(List<Theme> chain, string themeType)
    {
        string[] classes;
        string variation;
        if (themeType.Length == 0 || themeType == GetThemeClassName())
        {
            classes = ClassChain();
            variation = _variation;
        }
        else
        {
            // Another type's items: that type, the bases a variation of it names, and (for a Godot class) its parents.
            System.Type? known = ClassDB.TypeOf(themeType);
            classes = known != null && typeof(Control).IsAssignableFrom(known) ? ChainOf(known) : System.Array.Empty<string>();
            variation = classes.Length == 0 || classes[0] != themeType ? themeType : "";
        }
        return TypeList(chain, classes, variation);
    }

    internal static List<string> TypeList(List<Theme> chain, string[] classChain, string variation)
    {
        var list = new List<string>();
        void Add(string s) { if (s.Length > 0 && !list.Contains(s)) list.Add(s); }
        string t = variation;
        for (int guard = 0; t.Length > 0 && guard < 16; guard++)
        {
            Add(t);
            string next = "";
            foreach (Theme th in chain)
                if (th.VariationBase(t) is { } b) { next = b; break; }
            t = next;
        }
        foreach (string c in classChain) Add(c);
        return list;
    }

    private bool OwnType(string themeType) => themeType.Length == 0 || themeType == GetThemeClassName();

    /// <summary>Tells the control and every Control below it that what a theme lookup returns may have changed.</summary>
    internal void NotifyThemeChanged()
    {
        if (!IsAlive) return;
        Notification(NotificationThemeChanged);
        EmitSignal(SignalName.ThemeChanged);
        UpdateMinimumSize();
        QueueRedraw();
        foreach (Node c in ChildrenSnapshot()) if (c is Control cc) cc.NotifyThemeChanged();
    }

    // ---- lookups ----

    public Color GetThemeColor(StringName name, StringName themeType = default!)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && _colorO != null && _colorO.TryGetValue(n, out Color o)) return o;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (th.TryColor(ty, n, out Color c)) return c;
        return new Color(0, 0, 0, 1);
    }

    public int GetThemeConstant(StringName name, StringName themeType = default!)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && _constO != null && _constO.TryGetValue(n, out int o)) return o;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (th.TryConstant(ty, n, out int c)) return c;
        return 0;
    }

    public Font? GetThemeFont(StringName name, StringName themeType = default!)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && _fontO != null && _fontO.TryGetValue(n, out Font? o)) return o;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (th.TryFont(ty, n, out Font? f)) return f;
        return GetThemeDefaultFont();
    }

    public int GetThemeFontSize(StringName name, StringName themeType = default!)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && _fontSizeO != null && _fontSizeO.TryGetValue(n, out int o)) return o;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (th.TryFontSize(ty, n, out int s)) return s;
        return GetThemeDefaultFontSize();
    }

    public Texture2D? GetThemeIcon(StringName name, StringName themeType = default!)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && _iconO != null && _iconO.TryGetValue(n, out Texture2D? o)) return o;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (th.TryIcon(ty, n, out Texture2D? i)) return i;
        return null;
    }

    private static readonly StyleBoxEmpty s_emptyBox = new();

    /// <summary>The style box the theme gives <paramref name="name"/>; an empty one (never null) when no theme has it.</summary>
    public StyleBox GetThemeStylebox(StringName name, StringName themeType = default!)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && _styleO != null && _styleO.TryGetValue(n, out StyleBox? o)) return o;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (th.TryStyleBox(ty, n, out StyleBox? s) && s != null) return s;
        return s_emptyBox;
    }

    public bool HasThemeColor(StringName name, StringName themeType = default!) => Has(name, themeType, (th, ty, n) => th.TryColor(ty, n, out _), _colorO);
    public bool HasThemeConstant(StringName name, StringName themeType = default!) => Has(name, themeType, (th, ty, n) => th.TryConstant(ty, n, out _), _constO);
    public bool HasThemeFont(StringName name, StringName themeType = default!) => Has(name, themeType, (th, ty, n) => th.TryFont(ty, n, out _), _fontO);
    public bool HasThemeFontSize(StringName name, StringName themeType = default!) => Has(name, themeType, (th, ty, n) => th.TryFontSize(ty, n, out _), _fontSizeO);
    public bool HasThemeIcon(StringName name, StringName themeType = default!) => Has(name, themeType, (th, ty, n) => th.TryIcon(ty, n, out _), _iconO);
    public bool HasThemeStylebox(StringName name, StringName themeType = default!) => Has(name, themeType, (th, ty, n) => th.TryStyleBox(ty, n, out _), _styleO);

    private bool Has<T>(StringName name, StringName themeType, Func<Theme, string, string, bool> test, Dictionary<string, T>? overrides)
    {
        string n = name.ToString(), type = themeType?.ToString() ?? "";
        if (OwnType(type) && overrides != null && overrides.ContainsKey(n)) return true;
        List<Theme> chain = ThemeChain();
        foreach (Theme th in chain)
            foreach (string ty in ThemeTypes(chain, type))
                if (test(th, ty, n)) return true;
        return false;
    }

    /// <summary>The default font: the nearest theme in the chain that set one.</summary>
    public Font? GetThemeDefaultFont()
    {
        foreach (Theme th in ThemeChain()) if (th.DefaultFont != null) return th.DefaultFont;
        return null;
    }

    public int GetThemeDefaultFontSize()
    {
        foreach (Theme th in ThemeChain()) if (th.DefaultFontSize >= 0) return th.DefaultFontSize;
        return ThemeDB.FallbackFontSize;
    }

    public float GetThemeDefaultBaseScale()
    {
        foreach (Theme th in ThemeChain()) if (th.DefaultBaseScale > 0) return th.DefaultBaseScale;
        return 1f;
    }

    // ---- overrides ----

    private void OverrideChanged()
    {
        Notification(NotificationThemeChanged);
        EmitSignal(SignalName.ThemeChanged);
        UpdateMinimumSize();
        QueueRedraw();
    }

    public void AddThemeColorOverride(StringName name, Color color) { (_colorO ??= new())[name.ToString()] = color; OverrideChanged(); }
    public void AddThemeConstantOverride(StringName name, int constant) { (_constO ??= new())[name.ToString()] = constant; OverrideChanged(); }
    public void AddThemeFontOverride(StringName name, Font font) { (_fontO ??= new())[name.ToString()] = font; OverrideChanged(); }
    public void AddThemeFontSizeOverride(StringName name, int fontSize) { (_fontSizeO ??= new())[name.ToString()] = fontSize; OverrideChanged(); }
    public void AddThemeIconOverride(StringName name, Texture2D texture) { (_iconO ??= new())[name.ToString()] = texture; OverrideChanged(); }
    public void AddThemeStyleboxOverride(StringName name, StyleBox stylebox) { (_styleO ??= new())[name.ToString()] = stylebox; OverrideChanged(); }

    public void RemoveThemeColorOverride(StringName name) { if (_colorO?.Remove(name.ToString()) == true) OverrideChanged(); }
    public void RemoveThemeConstantOverride(StringName name) { if (_constO?.Remove(name.ToString()) == true) OverrideChanged(); }
    public void RemoveThemeFontOverride(StringName name) { if (_fontO?.Remove(name.ToString()) == true) OverrideChanged(); }
    public void RemoveThemeFontSizeOverride(StringName name) { if (_fontSizeO?.Remove(name.ToString()) == true) OverrideChanged(); }
    public void RemoveThemeIconOverride(StringName name) { if (_iconO?.Remove(name.ToString()) == true) OverrideChanged(); }
    public void RemoveThemeStyleboxOverride(StringName name) { if (_styleO?.Remove(name.ToString()) == true) OverrideChanged(); }

    public bool HasThemeColorOverride(StringName name) => _colorO?.ContainsKey(name.ToString()) == true;
    public bool HasThemeConstantOverride(StringName name) => _constO?.ContainsKey(name.ToString()) == true;
    public bool HasThemeFontOverride(StringName name) => _fontO?.ContainsKey(name.ToString()) == true;
    public bool HasThemeFontSizeOverride(StringName name) => _fontSizeO?.ContainsKey(name.ToString()) == true;
    public bool HasThemeIconOverride(StringName name) => _iconO?.ContainsKey(name.ToString()) == true;
    public bool HasThemeStyleboxOverride(StringName name) => _styleO?.ContainsKey(name.ToString()) == true;

    // The scene form: theme_override_colors/font_color = Color(...), theme_override_styles/panel = SubResource(...) ...
    private bool TrySetThemeOverride(string name, Variant value)
    {
        const string prefix = "theme_override_";
        if (!name.StartsWith(prefix, StringComparison.Ordinal)) return false;
        int slash = name.IndexOf('/');
        if (slash < 0) return false;
        string kind = name[prefix.Length..slash], item = name[(slash + 1)..];
        switch (kind)
        {
            case "colors": AddThemeColorOverride(item, value.AsColor()); return true;
            case "constants": AddThemeConstantOverride(item, value.AsInt32()); return true;
            case "font_sizes": AddThemeFontSizeOverride(item, value.AsInt32()); return true;
            case "fonts":
                if (value.AsGodotObject() is Resource fr) { AddThemeFontOverride(item, fr as Font ?? new FontFile { ResourcePath = fr.ResourcePath }); return true; }
                return true;
            case "icons":
                if (value.AsGodotObject() is Resource ir)
                {
                    Texture2D? tex = ir as Texture2D ?? ResourceLoader.Load(ir.ResourcePath, typeof(Texture2D)) as Texture2D;
                    if (tex != null) AddThemeIconOverride(item, tex);
                }
                return true;
            case "styles":
                if (value.AsGodotObject() is StyleBox sb) AddThemeStyleboxOverride(item, sb);
                else if (value.AsGodotObject() is Resource sr)
                    Aver.Scripting.Log.Warn($"[Fabric] {name}: '{sr.ResourcePath}' is not a style box");
                return true;
        }
        return false;
    }

    private bool TryGetThemeOverride(string name, out Variant value)
    {
        value = default;
        const string prefix = "theme_override_";
        if (!name.StartsWith(prefix, StringComparison.Ordinal)) return false;
        int slash = name.IndexOf('/');
        if (slash < 0) return false;
        string kind = name[prefix.Length..slash], item = name[(slash + 1)..];
        switch (kind)
        {
            case "colors": if (_colorO != null && _colorO.TryGetValue(item, out Color c)) { value = c; return true; } break;
            case "constants": if (_constO != null && _constO.TryGetValue(item, out int k)) { value = k; return true; } break;
            case "font_sizes": if (_fontSizeO != null && _fontSizeO.TryGetValue(item, out int s)) { value = s; return true; } break;
            case "fonts": if (_fontO != null && _fontO.TryGetValue(item, out Font? f)) { value = Variant.CreateFrom((GodotObject?)f); return true; } break;
            case "icons": if (_iconO != null && _iconO.TryGetValue(item, out Texture2D? i)) { value = Variant.CreateFrom((GodotObject?)i); return true; } break;
            case "styles": if (_styleO != null && _styleO.TryGetValue(item, out StyleBox? b)) { value = Variant.CreateFrom((GodotObject?)b); return true; } break;
        }
        return false;
    }
}
