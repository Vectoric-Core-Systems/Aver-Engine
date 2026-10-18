// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the generic game-UI Aver Node nodes call (GraphUiExtNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsUiExt.hpp): any widget property by name, list items, selection, the theme and the input
// state, each a thin wrapper over Widget and UiSystem (Aver.UI). Widgets cross the graph as plain ints, like the existing
// AN_*Ui* nodes. UI scale is deliberately not here: the accessibility option graphics.uiScale owns it.

using Aver.UI;

namespace Aver.Framework;

internal static class UiExtGraph
{
    private const string NoWidget = "the widget does not exist";

    /// <summary>AN_SetUiProperty: sets the property= (a name Widget.Set knows, such as fontSize) to a number.</summary>
    internal static bool SetUiPropertyForGraph(int widget, string property, float value)
    {
        try
        {
            var w = new Widget((uint)widget);
            return FamiliesBLog.Ok("AN_SetUiProperty", float.IsFinite(value) && w.Exists && w.Set(property, value),
                                   NoWidget + ", or the widget refused the property '" + property + "'");
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetUiProperty", e); }
    }

    /// <summary>AN_SetUiPropertyText: sets the property= to the text= (a name or enum word; empty clears it).</summary>
    internal static bool SetUiPropertyTextForGraph(int widget, string property, string text)
    {
        try
        {
            var w = new Widget((uint)widget);
            return FamiliesBLog.Ok("AN_SetUiPropertyText", w.Exists && w.Set(property, text),
                                   NoWidget + ", or the widget refused the property '" + property + "'");
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetUiPropertyText", e); }
    }

    /// <summary>AN_SetUiItems: replaces a choice's or list's options with the items= text, separated by '|'. Empty clears them.</summary>
    internal static bool SetUiItemsForGraph(int widget, string items)
    {
        try
        {
            var w = new Widget((uint)widget);
            if (!FamiliesBLog.Ok("AN_SetUiItems", w.Exists, NoWidget)) return false;
            w.SetItems(string.IsNullOrEmpty(items) ? Array.Empty<string>() : items.Split('|'));
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetUiItems", e); }
    }

    /// <summary>AN_SetUiSelected: a choice's or list's selected index.</summary>
    internal static bool SetUiSelectedForGraph(int widget, int index)
    {
        try
        {
            var w = new Widget((uint)widget);
            if (!FamiliesBLog.Ok("AN_SetUiSelected", w.Exists, NoWidget)) return false;
            w.Selected = index;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetUiSelected", e); }
    }

    /// <summary>AN_SetUiTheme: switches the UI theme by name (dark is the default).</summary>
    internal static bool SetUiThemeForGraph(string theme)
    {
        try
        {
            UiSystem.SetTheme(theme);
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_SetUiTheme", e); }
    }

    /// <summary>AN_GetUiInputState: whether a widget wants the pointer, wants the keyboard, pauses the game or wants the
    /// cursor shown. All false when the UI library is absent.</summary>
    internal static void GetUiInputStateForGraph(out bool pointer, out bool keyboard, out bool pausesGame, out bool showCursor)
    {
        pointer = false; keyboard = false; pausesGame = false; showCursor = false;
        try
        {
            pointer = UiSystem.WantsPointer;
            keyboard = UiSystem.WantsKeyboard;
            pausesGame = UiSystem.PausesGame;
            showCursor = UiSystem.ShowCursor;
        }
        catch (Exception e) { FamiliesBLog.Failed("AN_GetUiInputState", e); }
    }
}
