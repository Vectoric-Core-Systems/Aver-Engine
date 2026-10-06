// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The one entry point every Game UI graph node (AN_OpenUiLayout, AN_SetUiText, ...) compiles to.
using Aver.UI;

namespace Aver.Framework;

/// <summary>What the <c>AN_*Ui*</c> graph nodes call. ONE dispatcher rather than a method per node:
/// the compiler pushes the node's type, its string attribute and whatever pins it has, and this
/// routes by type, so adding a node here is a case and a catalog row, never new IL emission.
/// Widgets and layouts cross the graph as plain ints, the way entities do.</summary>
internal static class UiGraphInterop
{
    private static SettingsScreen? s_settings;

    /// <summary>Runs one UI node. <paramref name="op"/> is the node type lower-cased;
    /// <paramref name="s"/> its path=/name=/text= attribute; <paramref name="a"/> the layout, widget or
    /// parent pin; <paramref name="b"/> the kind pin; <paramref name="f"/> the value pin;
    /// <paramref name="flag"/> the checked/visible/enabled pin. Outputs: the int result (layout,
    /// widget, selected), the float (value) and the bool (checked, clicked, fired). The return is the
    /// node's <c>success</c>.</summary>
    internal static bool UiNodeForGraph(string op, string s, int a, int b, float f, bool flag,
                                        out int outInt, out float outFloat, out bool outBool)
    {
        outInt = 0;
        outFloat = 0f;
        outBool = false;
        try
        {
            switch (op)
            {
                case "an_openuilayout": return UiGraphNodes.OpenLayout(s, out outInt);
                case "an_closeuilayout": return UiGraphNodes.CloseLayout(a);
                case "an_finduiwidget": return UiGraphNodes.FindWidget(a, s, out outInt);
                case "an_createuiwidget": return UiGraphNodes.CreateWidget(a, b, s, out outInt);
                case "an_setuitext": return UiGraphNodes.SetText(a, s);
                case "an_setuivalue": return UiGraphNodes.SetValue(a, f);
                case "an_setuichecked": return UiGraphNodes.SetChecked(a, flag);
                case "an_setuivisible": return UiGraphNodes.SetVisible(a, flag);
                case "an_setuienabled": return UiGraphNodes.SetEnabled(a, flag);
                case "an_getuivalue": return UiGraphNodes.GetValue(a, out outFloat, out outBool, out outInt);
                case "an_uiwasclicked": outBool = UiGraphNodes.WasClicked(a); return true;
                case "an_uicommandfired": outBool = UiGraphNodes.CommandFired(s); return true;
                case "an_setuifocus": return UiGraphNodes.SetFocus(a);
                case "an_openuisettings": return OpenSettings(out outInt);
                default: return false;
            }
        }
        catch (DllNotFoundException)
        {
            // No UI library in this build (a headless tool): the node does nothing.
            return false;
        }
    }

    // The default settings screen, created on first use and shown again afterwards. Saved values are
    // applied the first time, so opening it once at startup restores the player's settings.
    private static bool OpenSettings(out int root)
    {
        if (s_settings == null)
        {
            s_settings = UiGameSettings.Create();
            s_settings.ApplyAll();
            s_settings.Build();
        }
        else if (!s_settings.Root.Exists)
        {
            s_settings.Build();
        }
        Widget shown = s_settings.Root;
        shown.Visible = true;
        root = (int)shown.Id;
        return shown.Exists;
    }
}
