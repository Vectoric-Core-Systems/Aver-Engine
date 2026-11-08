// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// UiBindingsTests: the managed half of UI bindings -- view-model path resolution, published variables, change notification and
// the methods EVENT routes call. No native library is needed: the paths and method calls are plain reflection.

using System.ComponentModel;
using Aver.UI;

static class UiBindingsTests
{
    private sealed class Stats { public int Health = 80; public float Armor { get; set; } = 2.5f; public string Name { get; set; } = "Ada"; }
    private sealed class Model : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        public Stats Player { get; } = new();
        public List<string> Items { get; } = new() { "sword", "shield" };
        public Dictionary<string, int> Counts { get; } = new() { ["gold"] = 120 };
        public bool Dead { get; set; }
        public int Volume;
        public string Last = "";
        public Stats? Missing { get; set; }
        public void Save() => Last = "saved";
        public void SetVolume(float v) => Volume = (int)(v * 100);
        public void Pick(int slot, string what) => Last = what + slot;
        public void Echo(string text) => Last = text;
        public void Raise(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        public Settings Settings { get; } = new();
    }
    private sealed class Settings { public float Level; public void SetLevel(float v) => Level = v; }

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("ui bindings: view-model paths", Paths);
        T.Run("ui bindings: variables", Variables);
        T.Run("ui bindings: methods called by EVENT routes", Methods);
    });

    private static void Paths()
    {
        var m = new Model();
        object? v;
        T.Check("a property", UiBindings.TryResolve(m, "Player.Armor", out v) && v is float f && f == 2.5f);
        T.Check("a field", UiBindings.TryResolve(m, "Player.Health", out v) && (int)v! == 80);
        T.Check("the case may differ when nothing matches exactly", UiBindings.TryResolve(m, "player.name", out v) && (string)v! == "Ada");
        T.Check("a list index", UiBindings.TryResolve(m, "Items[1]", out v) && (string)v! == "shield");
        T.Check("a dictionary key", UiBindings.TryResolve(m, "Counts[gold]", out v) && (int)v! == 120);
        T.Check("an index out of range is no value", !UiBindings.TryResolve(m, "Items[5]", out _));
        T.Check("a missing member is no value", !UiBindings.TryResolve(m, "Player.Nope", out _));
        T.Check("a null on the way is no value", !UiBindings.TryResolve(m, "Missing.Name", out _) && !UiBindings.TryResolve(null, "A", out _) && !UiBindings.TryResolve(m, "", out _));
        UiBindings.ViewModel = m;
        T.Check("vm: paths start at ViewModel", UiBindings.TryResolve("Player.Health", out v) && (int)v! == 80);
        m.Player.Armor = 9f;
        T.Check("and read the live value", UiBindings.TryResolve("Player.Armor", out v) && (float)v! == 9f);
        UiBindings.ViewModel = null;
    }

    private static void Variables()
    {
        UiBindings.ClearVariables();
        UiBindings.SetVariable("Score", 12f);
        T.Check("a published variable reads back", UiBindings.TryGetVariable("Score", out object? v) && (float)v! == 12f);
        T.Check("an unknown one has no value", !UiBindings.TryGetVariable("Other", out _));
        UiBindings.VariableResolver = name => name == "Other" ? true : null;
        T.Check("VariableResolver answers the rest", UiBindings.TryGetVariable("Other", out v) && (bool)v!);
        T.Check("and a set variable wins", UiBindings.TryGetVariable("Score", out v) && (float)v! == 12f);
        UiBindings.VariableResolver = null;
        UiBindings.SetVariable("Flag", true);
        UiBindings.SetVariable("Flag", true);
        T.Check("a bool variable", UiBindings.TryGetVariable("Flag", out v) && (bool)v!);
        UiBindings.ClearVariables();
        T.Check("ClearVariables forgets them", !UiBindings.TryGetVariable("Score", out _));
    }

    private static void Methods()
    {
        var m = new Model();
        UiBindings.ViewModel = m;
        var click = new UiEvent(UiEventKind.Clicked, Widget.None, 0f, 0, "");
        var slide = new UiEvent(UiEventKind.ValueChanged, Widget.None, 0.4f, 0, "");
        var none = Array.Empty<string>();
        T.Check("a method with no parameters", UiBindings.InvokeMethod("Save", none, click) && m.Last == "saved");
        T.Check("a float parameter takes the event's value", UiBindings.InvokeMethod("SetVolume", none, slide) && m.Volume == 40);
        T.Check("a method of a member", UiBindings.InvokeMethod("Settings.SetLevel", none, slide) && m.Settings.Level == 0.4f);
        T.Check("route arguments are converted to the parameters", UiBindings.InvokeMethod("Pick", new[] { "3", "axe" }, click) && m.Last == "axe3");
        T.Check("the wrong number of arguments finds nothing", !UiBindings.InvokeMethod("Pick", new[] { "3" }, click));
        T.Check("a string parameter takes the event's text", UiBindings.InvokeMethod("Echo", none, new UiEvent(UiEventKind.TextChanged, Widget.None, 0f, 0, "hello")) && m.Last == "hello");
        T.Check("an unknown method finds nothing", !UiBindings.InvokeMethod("Nope", none, click) && !UiBindings.InvokeMethod("Missing.Save", none, click));

        int calls = 0;
        UiBindings.RegisterMethod("Save", () => calls++);
        T.Check("a registered handler wins over the view model", UiBindings.InvokeMethod("Save", none, click) && calls == 1 && m.Last != "saved2");
        UiBindings.ClearMethods();
        UiBindings.ViewModel = null;
        T.Check("with no view model only handlers run", !UiBindings.InvokeMethod("Save", none, click));
    }
}
