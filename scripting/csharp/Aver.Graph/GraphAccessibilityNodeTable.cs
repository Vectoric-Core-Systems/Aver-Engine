// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The accessibility rows of the game-systems node table (docs/plans/wave9/ACCESSIBILITY.md section 2.10, category
// "Accessibility"). Every method is in AccessibilityGraphInterop (Aver.Framework/Accessibility.cs); option keys, cues,
// speakers and texts are NODE attributes, never pins. Option changes arrive as AN_OnEvent entries named
// Access.Changed (string key, float value), so they need no node. Palette mirror: sandbox/src/GraphNodeDefsAccessibility.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] AccessibilityTable() => new[]
    {
        Reg("setaccessibilityoption", GenericKind.Exec, typeof(AccessibilityGraphInterop), "SetOptionForGraph", "value:float", "success:bool", "@key,p:value", "success"),
        Reg("getaccessibilityoption", GenericKind.Pure, typeof(AccessibilityGraphInterop), "GetOptionForGraph", "", "value:float,found:bool", "@key,>value", "found"),
        Reg("showcaption", GenericKind.Exec, typeof(AccessibilityGraphInterop), "ShowCaptionForGraph", "x:float,y:float,z:float,positional:bool", "success:bool", "@cue,p:x,p:y,p:z,p:positional", "success"),
        Reg("showcaptiontext", GenericKind.Exec, typeof(AccessibilityGraphInterop), "ShowCaptionTextForGraph", "seconds:float=3,kind:int", "success:bool", "@speaker?,@text,p:seconds,p:kind", "success"),
        Reg("stopcaption", GenericKind.Exec, typeof(AccessibilityGraphInterop), "StopCaptionForGraph", "", "success:bool", "@cue?", "success"),
        Reg("announce", GenericKind.Exec, typeof(AccessibilityGraphInterop), "AnnounceForGraph", "interrupt:bool", "success:bool", "@text,p:interrupt", "success"),
        Reg("stopspeaking", GenericKind.Exec, typeof(AccessibilityGraphInterop), "StopSpeakingForGraph", "", "success:bool", "", "success"),
        Reg("isspeaking", GenericKind.Pure, typeof(AccessibilityGraphInterop), "IsSpeakingForGraph", "", "result:bool", "", "result"),
        Reg("addcamerashake", GenericKind.Exec, typeof(AccessibilityGraphInterop), "AddCameraShakeForGraph", "trauma:float=0.5", "success:bool", "p:trauma", "success"),
        Reg("setcamerabob", GenericKind.Exec, typeof(AccessibilityGraphInterop), "SetCameraBobForGraph", "amount:float,speed:float", "success:bool", "p:amount,p:speed", "success"),
        Reg("setactiontoggleallowed", GenericKind.Exec, typeof(AccessibilityGraphInterop), "SetActionToggleAllowedForGraph", "allowed:bool=true", "success:bool", "@action,p:allowed", "success"),
    };
}
