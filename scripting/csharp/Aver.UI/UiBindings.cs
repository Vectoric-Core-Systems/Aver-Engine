// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Bindings and event routes as a game script sees them: a view-model object, published graph variables, and the methods
// a layout's EVENT records call.
using System.Collections;
using System.ComponentModel;
using System.Globalization;
using System.Reflection;
using System.Text;

namespace Aver.UI;

/// <summary>Feeds the <c>BIND</c> records of opened layouts and runs the <c>EVENT</c> records that name a method.
/// A binding copies one source value to one widget property, one way: <c>vm:Player.Score</c> reads a path on
/// <see cref="ViewModel"/> (properties, fields, <c>[index]</c> on lists and dictionaries); <c>var:Health</c> reads a
/// variable set with <see cref="SetVariable"/> (the graph nodes <c>AN_SetUiVariable</c> publish a graph's variables that
/// way) or answered by <see cref="VariableResolver"/>. A frame-mode binding is read every frame; a <c>mode change</c> one
/// when <see cref="Notify"/> names its path, which a view model that implements <see cref="INotifyPropertyChanged"/> does
/// by itself.</summary>
public static class UiBindings
{
    private static object? s_viewModel;
    private static INotifyPropertyChanged? s_watched;
    private static readonly Dictionary<string, object?> s_variables = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, Action<UiEvent, IReadOnlyList<string>>> s_methods = new(StringComparer.Ordinal);
    private static readonly Dictionary<(Type, string), MemberInfo?> s_members = new();
    private static readonly Native.BindResolveFn s_resolver = NativeResolve;   // static: the native side keeps the pointer
    private static bool s_installed;

    /// <summary>The object <c>vm:</c> paths start from. Setting it re-reads every binding.</summary>
    public static object? ViewModel
    {
        get => s_viewModel;
        set
        {
            if (s_watched != null) s_watched.PropertyChanged -= OnPropertyChanged;
            s_viewModel = value;
            s_watched = value as INotifyPropertyChanged;
            if (s_watched != null) s_watched.PropertyChanged += OnPropertyChanged;
            Install();
            Notify(null);
        }
    }

    /// <summary>Answers <c>var:</c> paths that <see cref="SetVariable"/> has not set (a host that keeps graph variables
    /// elsewhere reads them here). Null for no value.</summary>
    public static Func<string, object?>? VariableResolver { get; set; }

    /// <summary>Called for a route whose action is <c>graph</c>: the target, the route's extra arguments and the event.</summary>
    public static event Action<string, IReadOnlyList<string>, UiEvent>? GraphEvent;

    private static void OnPropertyChanged(object? sender, PropertyChangedEventArgs e) => Notify(e.PropertyName);

    // ---- sources ----------------------------------------------------------------------------------------

    /// <summary>Sets a graph variable (a number, bool or string) and re-reads the bindings of it when it changed.</summary>
    public static void SetVariable(string name, object? value)
    {
        if (string.IsNullOrEmpty(name)) return;
        if (s_variables.TryGetValue(name, out object? old) && Equals(old, value)) return;
        s_variables[name] = value;
        Install();
        Notify(name);
    }

    /// <summary>The variable's value; false when none was set and <see cref="VariableResolver"/> has none.</summary>
    public static bool TryGetVariable(string name, out object? value)
    {
        if (s_variables.TryGetValue(name, out value)) return true;
        value = VariableResolver?.Invoke(name);
        return value != null;
    }

    /// <summary>Removes every published variable.</summary>
    public static void ClearVariables() { s_variables.Clear(); Notify(null); }

    /// <summary>Tells the UI a path changed so its <c>mode change</c> bindings read again (null or empty = everything).</summary>
    public static void Notify(string? path = null)
    {
        try { Native.aver_ui_bindings_changed(path ?? string.Empty); }
        catch (DllNotFoundException) { /* no UI library in this process */ }
    }

    /// <summary>Hands the native UI the resolver. Opening a layout and the setters above do it; calling it again is harmless.</summary>
    public static void Install()
    {
        if (s_installed) return;
        try { Native.aver_ui_set_binding_resolver(s_resolver); s_installed = true; }
        catch (DllNotFoundException) { }
    }

    /// <summary>Adds a binding to the layout <paramref name="widget"/> is in. <paramref name="path"/> may carry a
    /// <c>vm:</c> or <c>var:</c> prefix. False for an unknown widget or property.</summary>
    public static bool Bind(Widget widget, string property, string path, string? format = null, bool onChange = false)
    {
        Install();
        int source = 0;
        if (path.StartsWith("var:", StringComparison.OrdinalIgnoreCase)) { source = 1; path = path.Substring(4); }
        else if (path.StartsWith("vm:", StringComparison.OrdinalIgnoreCase)) path = path.Substring(3);
        return Native.aver_ui_bind(widget.Id, property, source, path, format ?? string.Empty, onChange ? 1 : 0) != 0;
    }

    /// <summary>The number of bindings in the layout at <paramref name="root"/>.</summary>
    public static int Count(Widget root) => Native.aver_ui_binding_count(root.Id);

    /// <summary>Routes one event of a widget (see the EVENT record) to a command, a graph event or a method.</summary>
    public static bool Route(Widget widget, string eventName, UiRouteAction action, string target)
        => Native.aver_ui_event_route(widget.Id, eventName, (int)action, target) != 0;

    // ---- path resolution --------------------------------------------------------------------------------

    /// <summary>Resolves a <c>vm:</c> path against <see cref="ViewModel"/>.</summary>
    public static bool TryResolve(string path, out object? value) => TryResolve(s_viewModel, path, out value);

    /// <summary>Resolves a path against any object: members separated by dots (public properties and fields, matched with
    /// the exact case first and then without), and <c>[n]</c> or <c>[key]</c> on arrays, lists and dictionaries. False when
    /// a step is missing or null.</summary>
    public static bool TryResolve(object? root, string path, out object? value)
    {
        value = null;
        object? cur = root;
        if (cur == null || string.IsNullOrEmpty(path)) return false;
        int i = 0;
        while (i < path.Length)
        {
            if (cur == null) return false;
            if (path[i] == '.') { i++; continue; }
            if (path[i] == '[')
            {
                int close = path.IndexOf(']', i);
                if (close < 0) return false;
                if (!Index(cur, path.Substring(i + 1, close - i - 1), out cur)) return false;
                i = close + 1;
                continue;
            }
            int end = i;
            while (end < path.Length && path[end] != '.' && path[end] != '[') end++;
            if (!Member(cur, path.Substring(i, end - i), out cur)) return false;
            i = end;
        }
        value = cur;
        return cur != null;
    }

    private static bool Member(object owner, string name, out object? value)
    {
        value = null;
        Type type = owner.GetType();
        if (!s_members.TryGetValue((type, name), out MemberInfo? m))
        {
            const BindingFlags flags = BindingFlags.Public | BindingFlags.Instance;
            m = (MemberInfo?)type.GetProperty(name, flags | BindingFlags.IgnoreCase) ?? type.GetField(name, flags | BindingFlags.IgnoreCase);
            if (m is PropertyInfo pi && (pi.GetIndexParameters().Length != 0 || !pi.CanRead)) m = null;
            // An exact-case match wins over a case-insensitive one.
            MemberInfo? exact = (MemberInfo?)type.GetProperty(name, flags) ?? type.GetField(name, flags);
            if (exact is PropertyInfo ep && ep.GetIndexParameters().Length == 0 && ep.CanRead) m = ep;
            else if (exact is FieldInfo) m = exact;
            s_members[(type, name)] = m;
        }
        try
        {
            switch (m)
            {
                case PropertyInfo p: value = p.GetValue(owner); return true;
                case FieldInfo f: value = f.GetValue(owner); return true;
            }
        }
        catch (Exception) { /* a getter that throws is a missing value */ }
        return false;
    }

    private static bool Index(object owner, string key, out object? value)
    {
        value = null;
        key = key.Trim().Trim('"', '\'');
        if (owner is IDictionary dict)
        {
            foreach (DictionaryEntry e in dict)
                if (string.Equals(Convert.ToString(e.Key, CultureInfo.InvariantCulture), key, StringComparison.Ordinal)) { value = e.Value; return true; }
            return false;
        }
        if (!int.TryParse(key, NumberStyles.Integer, CultureInfo.InvariantCulture, out int n) || n < 0) return false;
        if (owner is IList list) { if (n >= list.Count) return false; value = list[n]; return true; }
        if (owner is IEnumerable seq && owner is not string)
        {
            foreach (object? item in seq) { if (n-- == 0) { value = item; return true; } }
        }
        return false;
    }

    // ---- the native resolver ----------------------------------------------------------------------------

    private const int KindNone = 0, KindNumber = 1, KindBool = 2, KindText = 3;

    private static int NativeResolve(int source, string path, IntPtr outNumber, IntPtr outText, int textCap)
    {
        try
        {
            object? v;
            if (source == 1) { if (!TryGetVariable(path, out v)) return KindNone; }
            else if (!TryResolve(path, out v)) return KindNone;
            return ToNative(v, outNumber, outText, textCap);
        }
        catch (Exception) { return KindNone; }
    }

    private static int ToNative(object? v, IntPtr outNumber, IntPtr outText, int textCap)
    {
        switch (v)
        {
            case null: return KindNone;
            case bool b: Number(outNumber, b ? 1 : 0); return KindBool;
            case string s: return Text(s, outText, textCap);
            case char c: return Text(c.ToString(), outText, textCap);
            case Enum e: return Text(e.ToString(), outText, textCap);
            case IConvertible conv when v is not DateTime:
                try { Number(outNumber, conv.ToDouble(CultureInfo.InvariantCulture)); return KindNumber; }
                catch (Exception) { return Text(v.ToString() ?? string.Empty, outText, textCap); }
            default: return Text(v.ToString() ?? string.Empty, outText, textCap);
        }
    }

    private static void Number(IntPtr outNumber, double d) => System.Runtime.InteropServices.Marshal.WriteInt64(outNumber, BitConverter.DoubleToInt64Bits(d));

    private static int Text(string s, IntPtr outText, int cap)
    {
        if (cap <= 0) return KindNone;
        byte[] bytes = Encoding.UTF8.GetBytes(s);
        int n = Math.Min(bytes.Length, cap - 1);
        // Do not cut a multi-byte character in half.
        while (n > 0 && n < bytes.Length && (bytes[n] & 0xC0) == 0x80) n--;
        System.Runtime.InteropServices.Marshal.Copy(bytes, 0, outText, n);
        System.Runtime.InteropServices.Marshal.WriteByte(outText, n, 0);
        return KindText;
    }

    // ---- methods ----------------------------------------------------------------------------------------

    /// <summary>Registers a handler a <c>method</c> route can name (<c>EVENT Play click method Menu.Start</c>). It wins over
    /// a method of the same name on the view model.</summary>
    public static void RegisterMethod(string name, Action<UiEvent, IReadOnlyList<string>> handler) => s_methods[name] = handler;

    /// <summary>Registers a handler that needs neither the event nor arguments.</summary>
    public static void RegisterMethod(string name, Action handler) => s_methods[name] = (_, _) => handler();

    /// <summary>Removes every registered method.</summary>
    public static void ClearMethods() => s_methods.Clear();

    /// <summary>Forgets everything scripts set up: methods, variables, the view model, <see cref="GraphEvent"/> subscribers
    /// and <see cref="VariableResolver"/>. The script host calls it when scripts unload.</summary>
    public static void ResetForScriptUnload()
    {
        if (s_viewModel != null) ViewModel = null;
        GraphEvent = null;
        VariableResolver = null;
        s_methods.Clear();
        s_members.Clear();   // keyed by script types: holding them keeps the unloaded context alive
        ClearVariables();
    }

    /// <summary>Runs a method route: a registered handler by that name, else a public method on <see cref="ViewModel"/>
    /// ("Save", or "Settings.SetVolume" for a method of a member). With route arguments the method takes that many
    /// parameters (converted from the text); without, it takes none or one, filled from the event: a float or double
    /// from its value, an int from its index, a bool from value != 0, a string from its text, or the <see cref="UiEvent"/>
    /// itself. False when nothing was found or it threw.</summary>
    public static bool InvokeMethod(string target, IReadOnlyList<string> args, UiEvent ev)
    {
        if (s_methods.TryGetValue(target, out var handler))
        {
            try { handler(ev, args); return true; } catch (Exception) { return false; }
        }
        if (s_viewModel == null) return false;
        int dot = target.LastIndexOf('.');
        object? owner = s_viewModel;
        string name = target;
        if (dot >= 0 && !TryResolve(s_viewModel, target.Substring(0, dot), out owner)) return false;
        if (dot >= 0) name = target.Substring(dot + 1);
        if (owner == null) return false;
        foreach (MethodInfo m in owner.GetType().GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            if (!string.Equals(m.Name, name, StringComparison.Ordinal)) continue;
            ParameterInfo[] ps = m.GetParameters();
            object?[]? call = args.Count > 0 ? FromArgs(ps, args) : FromEvent(ps, ev);
            if (call == null) continue;
            try { m.Invoke(owner, call); return true; } catch (Exception) { return false; }
        }
        return false;
    }

    private static object?[]? FromArgs(ParameterInfo[] ps, IReadOnlyList<string> args)
    {
        if (ps.Length != args.Count) return null;
        var call = new object?[ps.Length];
        for (int i = 0; i < ps.Length; i++)
        {
            try { call[i] = Convert.ChangeType(args[i], ps[i].ParameterType, CultureInfo.InvariantCulture); }
            catch (Exception) { return null; }
        }
        return call;
    }

    private static object?[]? FromEvent(ParameterInfo[] ps, UiEvent ev)
    {
        if (ps.Length == 0) return Array.Empty<object?>();
        if (ps.Length != 1) return null;
        Type t = ps[0].ParameterType;
        if (t == typeof(UiEvent)) return new object?[] { ev };
        if (t == typeof(float)) return new object?[] { ev.Value };
        if (t == typeof(double)) return new object?[] { (double)ev.Value };
        if (t == typeof(int)) return new object?[] { ev.Index };
        if (t == typeof(bool)) return new object?[] { ev.Value != 0f };
        if (t == typeof(string)) return new object?[] { ev.Text };
        if (t == typeof(Widget)) return new object?[] { ev.Widget };
        return null;
    }

    // ---- routed events ----------------------------------------------------------------------------------

    private static readonly byte[] s_targetBuf = new byte[512], s_argsBuf = new byte[1024], s_textBuf = new byte[1024];

    internal static string Decode(byte[] buf)
    {
        int n = Array.IndexOf(buf, (byte)0);
        return Encoding.UTF8.GetString(buf, 0, n < 0 ? buf.Length : n);
    }

    /// <summary>Delivers the graph and method routes the last frame fired. <see cref="UiSystem.Pump"/> calls this.</summary>
    internal static int PumpRoutes(Action<string> graphFired)
    {
        int count = 0;
        while (count < 256 && Native.aver_ui_route_poll(out int action, out uint widget, out int type, out float value, out int index,
                   s_targetBuf, s_targetBuf.Length, s_argsBuf, s_argsBuf.Length, s_textBuf, s_textBuf.Length) != 0)
        {
            count++;
            string target = Decode(s_targetBuf);
            string argText = Decode(s_argsBuf);
            IReadOnlyList<string> args = argText.Length == 0 ? Array.Empty<string>() : argText.Split('\n');
            var ev = new UiEvent((UiEventKind)type, new Widget(widget), value, index, Decode(s_textBuf));
            if (action == (int)UiRouteAction.Graph)
            {
                graphFired(target);
                try { GraphEvent?.Invoke(target, args, ev); } catch (Exception) { }
            }
            else
            {
                InvokeMethod(target, args, ev);
            }
        }
        return count;
    }
}

/// <summary>What an <c>EVENT</c> route does. Mirrors the native <c>UiRouteAction</c>.</summary>
public enum UiRouteAction
{
    /// <summary>Raises a <see cref="UiEventKind.Command"/> event with the target as its text.</summary>
    Command = 0,
    /// <summary>Raises a graph event: <see cref="UiBindings.GraphEvent"/> and the node <c>AN_UiGraphEventFired</c>.</summary>
    Graph = 1,
    /// <summary>Calls a method: a registered handler or a method of the view model.</summary>
    Method = 2,
}
