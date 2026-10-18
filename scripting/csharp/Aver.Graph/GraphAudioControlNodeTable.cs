// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The master, bus and live-voice control rows of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md
// section 2.2). Every method is in AudioControlGraph (Aver.Framework/AudioControlGraph.cs), a thin wrapper over Audio and
// Voice. A voice handle is the voice pin of AN_PlayStream, AN_PlayStreamAt or AN_PlayMusic. Palette mirror:
// sandbox/src/GraphNodeDefsAudioControl.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] AudioControlTable() => new[]
    {
        Reg("an_setmastervolume", GenericKind.Exec, typeof(AudioControlGraph), "SetMasterVolumeForGraph", "volume:float=1", "success:bool", "p:volume", "success"),
        Reg("an_getmastervolume", GenericKind.Pure, typeof(AudioControlGraph), "GetMasterVolumeForGraph", "", "volume:float", "", "volume"),
        Reg("an_getbusvolume", GenericKind.Pure, typeof(AudioControlGraph), "GetBusVolumeForGraph", "bus:int", "volume:float", "p:bus", "volume"),
        Reg("an_stopallsounds", GenericKind.Exec, typeof(AudioControlGraph), "StopAllSoundsForGraph", "", "success:bool", "", "success"),
        Reg("an_setvoicevolume", GenericKind.Exec, typeof(AudioControlGraph), "SetVoiceVolumeForGraph", "voice:int,volume:float", "success:bool", "p:voice,p:volume", "success"),
        Reg("an_setvoicepitch", GenericKind.Exec, typeof(AudioControlGraph), "SetVoicePitchForGraph", "voice:int,pitch:float=1", "success:bool", "p:voice,p:pitch", "success"),
        Reg("an_setvoiceposition", GenericKind.Exec, typeof(AudioControlGraph), "SetVoicePositionForGraph", "voice:int,x:float,y:float,z:float", "success:bool", "p:voice,p:x,p:y,p:z", "success"),
        Reg("an_isvoiceplaying", GenericKind.Pure, typeof(AudioControlGraph), "IsVoicePlayingForGraph", "voice:int", "playing:bool", "p:voice", "playing"),
        Reg("an_stopvoice", GenericKind.Exec, typeof(AudioControlGraph), "StopVoiceForGraph", "voice:int", "success:bool", "p:voice", "success"),
        Reg("an_setocclusioncurve", GenericKind.Exec, typeof(AudioControlGraph), "SetOcclusionCurveForGraph", "minCutoffHz:float=800,minVolume:float=0.25", "success:bool", "p:minCutoffHz,p:minVolume", "success"),
        Reg("an_getaudiostats", GenericKind.Pure, typeof(AudioControlGraph), "GetAudioStatsForGraph", "", "activeVoices:int,activeStreams:int,underruns:int", ">activeVoices,>activeStreams,>underruns", null),
    };
}
