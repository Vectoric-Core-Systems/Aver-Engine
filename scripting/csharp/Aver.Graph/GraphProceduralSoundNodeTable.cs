// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The real-time sound-graph rows of the game-systems node table (docs/AUDIO.md section 13): play a .ocsnd on the audio thread, set its
// named live inputs, send the note-off. Every method is in ProceduralSoundGraph (Aver.Framework/AudioSoundGraph.Graph.cs), a thin wrapper
// over Audio.PlayGraph and Voice. The sound= attribute is the .ocsnd path; param= is the Parameter node's name. A voice handle is the voice
// pin of AN_PlaySoundGraph or AN_PlaySoundGraphAt and works with the audio-control nodes too (Set Voice Volume, Stop Voice, ...).
// Palette mirror: sandbox/src/GraphNodeDefsProceduralSound.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] ProceduralSoundTable() => new[]
    {
        Reg("an_playsoundgraph", GenericKind.Exec, typeof(ProceduralSoundGraph), "PlaySoundGraphForGraph", "volume:float=1,looping:bool,bus:int,seed:int", "voice:int,success:bool", "@sound,p:volume,p:looping,p:bus,p:seed,>voice", "success"),
        Reg("an_playsoundgraphat", GenericKind.Exec, typeof(ProceduralSoundGraph), "PlaySoundGraphAtForGraph", "x:float,y:float,z:float,volume:float=1,looping:bool,bus:int,innerCm:float=200,outerCm:float=2000,seed:int", "voice:int,success:bool", "@sound,p:x,p:y,p:z,p:volume,p:looping,p:bus,p:innerCm,p:outerCm,p:seed,>voice", "success"),
        Reg("an_setsoundgraphparam", GenericKind.Exec, typeof(ProceduralSoundGraph), "SetSoundGraphParamForGraph", "voice:int,value:float", "success:bool", "p:voice,@param,p:value", "success"),
        Reg("an_noteoffsoundgraph", GenericKind.Exec, typeof(ProceduralSoundGraph), "NoteOffSoundGraphForGraph", "voice:int", "success:bool", "p:voice", "success"),
    };
}
