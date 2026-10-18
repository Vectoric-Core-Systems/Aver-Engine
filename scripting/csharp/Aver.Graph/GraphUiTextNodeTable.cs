// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Game UI text rows of the game-systems node table (docs/GAME_UI_TEXT.md section 2.13). Every method is in
// UiTextGraphInterop (Aver.Framework/UiTextGraph.cs); a widget crosses the graph as an int and the font family is a
// NODE attribute (strings are not pins). Font-set loading, atlas settings, prewarming and glyph queries are C#
// only (UiText); the UI language follows the culture nodes.
// Palette mirror: sandbox/src/GraphNodeDefsUiText.hpp. Found by reflection (GameSystemNodes.TableMethods); no list names it.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] UiTextTable() => new[]
    {
        Reg("an_setuifont", GenericKind.Exec, typeof(UiTextGraphInterop), "SetUiFontForGraph", "widget:int,size:float,bold:bool,italic:bool", "success:bool", "p:widget,p:size,p:bold,p:italic,@family?", "success"),
        Reg("an_setuitextstyle", GenericKind.Exec, typeof(UiTextGraphInterop), "SetUiTextStyleForGraph", "widget:int,align:int,dir:int,wrap:bool,ellipsis:bool,maxLines:int,lineSpacing:float=1,letterSpacing:float,rich:bool", "success:bool", "p:widget,p:align,p:dir,p:wrap,p:ellipsis,p:maxLines,p:lineSpacing,p:letterSpacing,p:rich", "success"),
        Reg("an_setuitextcolor", GenericKind.Exec, typeof(UiTextGraphInterop), "SetUiTextColorForGraph", "widget:int,r:float,g:float,b:float,a:float=1", "success:bool", "p:widget,p:r,p:g,p:b,p:a", "success"),
        Reg("an_getuitextsize", GenericKind.Pure, typeof(UiTextGraphInterop), "GetUiTextSizeForGraph", "widget:int", "found:bool,width:float,height:float,lines:int", "p:widget,>width,>height,>lines", "found"),
    };
}
