// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The operations the visual-script nodes AN_OpenUiLayout, AN_CreateUiWidget and friends call.
namespace Aver.UI;

/// <summary>What the UI graph nodes run. Widgets and layouts cross the graph as plain ints (a widget
/// id), the way entities do, so a graph can hold them in a VAR. Every method is a no-op returning
/// false for an id that no longer exists.</summary>
/// <remarks>Aver.Framework's <c>GraphInterop</c> forwards its <c>*ForGraph</c> entry points to these;
/// see docs/GAME_UI.md for the node pins.</remarks>
public static class UiGraphNodes
{
    /// <summary>AN_OpenUiLayout: reads an <c>.ocui</c> file and creates its widgets. The layout is its root widget.</summary>
    public static bool OpenLayout(string path, out int layout)
    {
        Widget w = UiSystem.OpenLayout(path);
        layout = (int)w.Id;
        return w.Id != 0;
    }

    /// <summary>AN_CloseUiLayout: destroys a layout's widgets.</summary>
    public static bool CloseLayout(int layout) => new Widget((uint)layout).Destroy();

    /// <summary>AN_FindUiWidget: a named widget inside a layout (0 layout searches everywhere).</summary>
    public static bool FindWidget(int layout, string name, out int widget)
    {
        Widget w = layout == 0 ? Widget.FindAnywhere(name) : new Widget((uint)layout).Find(name);
        widget = (int)w.Id;
        return w.Id != 0;
    }

    /// <summary>AN_CreateUiWidget: a new widget of <paramref name="kind"/> (a <see cref="WidgetKind"/> number)
    /// under <paramref name="parent"/> (0 makes a root).</summary>
    public static bool CreateWidget(int parent, int kind, string name, out int widget)
    {
        Widget w = Widget.Create((WidgetKind)kind, new Widget((uint)parent), name ?? string.Empty);
        widget = (int)w.Id;
        return w.Id != 0;
    }

    /// <summary>AN_SetUiText.</summary>
    public static bool SetText(int widget, string text)
    {
        var w = new Widget((uint)widget);
        if (!w.Exists) return false;
        w.Text = text;
        return true;
    }

    /// <summary>AN_SetUiValue: a slider's or progress bar's value.</summary>
    public static bool SetValue(int widget, float value)
    {
        var w = new Widget((uint)widget);
        if (!w.Exists) return false;
        w.Value = value;
        return true;
    }

    /// <summary>AN_SetUiChecked.</summary>
    public static bool SetChecked(int widget, bool value)
    {
        var w = new Widget((uint)widget);
        if (!w.Exists) return false;
        w.Checked = value;
        return true;
    }

    /// <summary>AN_SetUiVisible.</summary>
    public static bool SetVisible(int widget, bool value)
    {
        var w = new Widget((uint)widget);
        if (!w.Exists) return false;
        w.Visible = value;
        return true;
    }

    /// <summary>AN_SetUiEnabled.</summary>
    public static bool SetEnabled(int widget, bool value)
    {
        var w = new Widget((uint)widget);
        if (!w.Exists) return false;
        w.Enabled = value;
        return true;
    }

    /// <summary>AN_GetUiValue: a widget's value, checked state and selected index at once.</summary>
    public static bool GetValue(int widget, out float value, out bool isChecked, out int selected)
    {
        var w = new Widget((uint)widget);
        value = 0f;
        isChecked = false;
        selected = 0;
        if (!w.Exists) return false;
        value = w.Value;
        isChecked = w.Checked;
        selected = w.Selected;
        return true;
    }

    /// <summary>AN_UiWasClicked: true when the widget was clicked since the last <see cref="UiSystem.Pump"/>.</summary>
    public static bool WasClicked(int widget) => UiSystem.WasClicked(new Widget((uint)widget));

    /// <summary>AN_UiCommandFired: true when a button with this command was activated since the last Pump.</summary>
    public static bool CommandFired(string command) => UiSystem.CommandFired(command);

    /// <summary>AN_SetUiFocus.</summary>
    public static bool SetFocus(int widget) => new Widget((uint)widget).Focus();

    /// <summary>AN_ShowUiMenu: a centred menu of buttons; <paramref name="entries"/> is one "Label|command"
    /// per line.</summary>
    public static bool CreateMenu(string name, string title, string entries, bool modal, out int menu)
    {
        var list = new List<UiSystem.MenuEntry>();
        foreach (string line in (entries ?? string.Empty).Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            int bar = line.IndexOf('|');
            string label = (bar < 0 ? line : line.Substring(0, bar)).TrimEnd('\r');
            string command = bar < 0 ? string.Empty : line.Substring(bar + 1).TrimEnd('\r');
            bool danger = label.StartsWith('!');
            list.Add(new UiSystem.MenuEntry(danger ? label.Substring(1) : label, command, danger));
        }
        Widget w = UiSystem.CreateMenu(name, title ?? string.Empty, list, modal);
        menu = (int)w.Id;
        return w.Id != 0;
    }
}
