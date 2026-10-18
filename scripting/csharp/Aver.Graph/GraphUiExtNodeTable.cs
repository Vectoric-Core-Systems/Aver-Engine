// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The generic game-UI rows of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2):
// any widget property, list items, selection, the theme and the input state. Every method is in UiExtGraph
// (Aver.Framework/UiExtGraph.cs), a thin wrapper over Widget and UiSystem. These are typed rows beside the older
// UiGraphInterop.UiNodeForGraph dispatcher, which stays as it is. UI scale is not here (graphics.uiScale, Accessibility).
// Palette mirror: sandbox/src/GraphNodeDefsUiExt.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] UiExtTable() => new[]
    {
        Reg("an_setuiproperty", GenericKind.Exec, typeof(UiExtGraph), "SetUiPropertyForGraph", "widget:int,value:float", "success:bool", "p:widget,@property,p:value", "success"),
        Reg("an_setuipropertytext", GenericKind.Exec, typeof(UiExtGraph), "SetUiPropertyTextForGraph", "widget:int", "success:bool", "p:widget,@property,@text?", "success"),
        Reg("an_setuiitems", GenericKind.Exec, typeof(UiExtGraph), "SetUiItemsForGraph", "widget:int", "success:bool", "p:widget,@items?", "success"),
        Reg("an_setuiselected", GenericKind.Exec, typeof(UiExtGraph), "SetUiSelectedForGraph", "widget:int,index:int", "success:bool", "p:widget,p:index", "success"),
        Reg("an_setuitheme", GenericKind.Exec, typeof(UiExtGraph), "SetUiThemeForGraph", "", "success:bool", "@theme", "success"),
        Reg("an_getuiinputstate", GenericKind.Pure, typeof(UiExtGraph), "GetUiInputStateForGraph", "", "pointer:bool,keyboard:bool,pausesGame:bool,showCursor:bool", ">pointer,>keyboard,>pausesGame,>showCursor", null),
    };
}
