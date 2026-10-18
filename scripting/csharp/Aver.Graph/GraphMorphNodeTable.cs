// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The blend-shape and lip-sync rows of the game-systems node table (docs/plans/wave9/MORPH_TARGETS.md section 2.9).
// Every method is in MorphGraphInterop (Aver.Framework/Morph.cs); target, pose, rig, track and viseme names are NODE
// attributes, never pins, and an empty rig or target means "the default" / "all" where the design says so.
// Palette mirror: sandbox/src/GraphNodeDefsMorph.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] MorphTable() => new[]
    {
        Reg("setmorphweight", GenericKind.Exec, typeof(MorphGraphInterop), "SetMorphWeightForGraph", "entity:int,weight:float,blendSeconds:float=0", "success:bool", "p:entity,@target,p:weight,p:blendSeconds", "success"),
        Reg("setfacepose", GenericKind.Exec, typeof(MorphGraphInterop), "SetFacePoseForGraph", "entity:int,weight:float,blendSeconds:float=0", "success:bool", "p:entity,@pose,p:weight,p:blendSeconds", "success"),
        Reg("clearmorphweights", GenericKind.Exec, typeof(MorphGraphInterop), "ClearMorphWeightsForGraph", "entity:int,blendSeconds:float=0", "success:bool", "p:entity,@target?,p:blendSeconds", "success"),
        Reg("getmorphweight", GenericKind.Pure, typeof(MorphGraphInterop), "GetMorphWeightForGraph", "entity:int", "found:bool,weight:float", "p:entity,@target,>weight", "found"),
        Reg("setfacerig", GenericKind.Exec, typeof(MorphGraphInterop), "SetFaceRigForGraph", "entity:int", "success:bool", "p:entity,@rig?", "success"),
        Reg("playlipsync", GenericKind.Exec, typeof(MorphGraphInterop), "PlayLipSyncForGraph", "entity:int,rate:float=1,startSeconds:float=0,strength:float=1", "success:bool", "p:entity,@track,p:rate,p:startSeconds,p:strength", "success"),
        Reg("stoplipsync", GenericKind.Exec, typeof(MorphGraphInterop), "StopLipSyncForGraph", "entity:int,blendSeconds:float=0.1", "success:bool", "p:entity,p:blendSeconds", "success"),
        Reg("queueviseme", GenericKind.Exec, typeof(MorphGraphInterop), "QueueVisemeForGraph", "entity:int,startSeconds:float=0,durationSeconds:float=0.1,intensity:float=1", "success:bool", "p:entity,@viseme,p:startSeconds,p:durationSeconds,p:intensity", "success"),
        Reg("getlipsyncstate", GenericKind.Pure, typeof(MorphGraphInterop), "GetLipSyncStateForGraph", "entity:int", "playing:bool,time:float,length:float,strength:float", "p:entity,>time,>length,>strength", "playing"),
    };
}
