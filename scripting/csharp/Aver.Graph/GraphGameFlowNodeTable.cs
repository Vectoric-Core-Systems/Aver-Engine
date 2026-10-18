// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The GameFlow family of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): the game clock,
// pause and quit, timer control, and the project settings store. Every method is in GraphGameFlow
// (Aver.Framework/GraphGameFlow.cs) over Game, Timers and Settings; settings keys are NODE attributes.
// Palette mirror: sandbox/src/GraphNodeDefsGameFlow.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] GameFlowTable() => new[]
    {
        Reg("an_gettime", GenericKind.Pure, typeof(GraphGameFlow), "GetTimeForGraph", "", "seconds:float", "", "seconds"),
        Reg("an_getrealtime", GenericKind.Pure, typeof(GraphGameFlow), "GetRealTimeForGraph", "", "seconds:float", "", "seconds"),
        Reg("an_gettimedilation", GenericKind.Pure, typeof(GraphGameFlow), "GetTimeDilationForGraph", "", "value:float", "", "value"),
        Reg("an_settimedilation", GenericKind.Exec, typeof(GraphGameFlow), "SetTimeDilationForGraph", "value:float=1", "success:bool", "p:value", "success"),
        Reg("an_isgamepaused", GenericKind.Pure, typeof(GraphGameFlow), "IsGamePausedForGraph", "", "paused:bool", "", "paused"),
        Reg("an_setgamepaused", GenericKind.Exec, typeof(GraphGameFlow), "SetGamePausedForGraph", "paused:bool", "success:bool", "p:paused", "success"),
        Reg("an_quitgame", GenericKind.Exec, typeof(GraphGameFlow), "QuitGameForGraph", "", "", "", null),
        Reg("an_timerremaining", GenericKind.Pure, typeof(GraphGameFlow), "TimerRemainingForGraph", "handle:int", "seconds:float", "p:handle", "seconds"),
        Reg("an_istimeractive", GenericKind.Pure, typeof(GraphGameFlow), "IsTimerActiveForGraph", "handle:int", "active:bool", "p:handle", "active"),
        Reg("an_pausetimer", GenericKind.Exec, typeof(GraphGameFlow), "PauseTimerForGraph", "handle:int,paused:bool", "success:bool", "p:handle,p:paused", "success"),
        Reg("an_resettimer", GenericKind.Exec, typeof(GraphGameFlow), "ResetTimerForGraph", "handle:int,delay:float=-1", "success:bool", "p:handle,p:delay", "success"),
        Reg("an_gettimerfirecount", GenericKind.Pure, typeof(GraphGameFlow), "GetTimerFireCountForGraph", "handle:int", "count:int", "p:handle", "count"),
        Reg("an_getsettingfloat", GenericKind.Pure, typeof(GraphGameFlow), "GetSettingFloatForGraph", "fallback:float", "value:float", "@key,p:fallback", "value"),
        Reg("an_getsettingint", GenericKind.Pure, typeof(GraphGameFlow), "GetSettingIntForGraph", "fallback:int", "value:int", "@key,p:fallback", "value"),
        Reg("an_getsettingbool", GenericKind.Pure, typeof(GraphGameFlow), "GetSettingBoolForGraph", "fallback:bool", "value:bool", "@key,p:fallback", "value"),
        Reg("an_hassetting", GenericKind.Pure, typeof(GraphGameFlow), "HasSettingForGraph", "", "has:bool", "@key", "has"),
        Reg("an_setsettingfloat", GenericKind.Exec, typeof(GraphGameFlow), "SetSettingFloatForGraph", "value:float", "success:bool", "@key,p:value", "success"),
        Reg("an_setsettingint", GenericKind.Exec, typeof(GraphGameFlow), "SetSettingIntForGraph", "value:int", "success:bool", "@key,p:value", "success"),
        Reg("an_setsettingbool", GenericKind.Exec, typeof(GraphGameFlow), "SetSettingBoolForGraph", "value:bool", "success:bool", "@key,p:value", "success"),
        Reg("an_flushsettings", GenericKind.Exec, typeof(GraphGameFlow), "FlushSettingsForGraph", "", "success:bool", "", "success"),
    };
}
