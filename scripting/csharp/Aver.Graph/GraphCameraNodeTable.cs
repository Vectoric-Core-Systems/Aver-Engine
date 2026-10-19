// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The gameplay camera rows of the game-systems node table (docs/CAMERAS.md, Aver Node family "Camera": 20 exec rows, 3 pure).
// Every method is in CameraGraph (Aver.Framework/CameraGraph.cs), a thin wrapper over the public members in CameraRig.cs and
// EntityCameraRig.cs. Palette mirror: sandbox/src/GraphNodeDefsCamera.hpp. Enums cross as ints: CameraMode, CameraRigPreset
// and CameraBlendCurve (see CameraRig.cs); entities are ints. Events (CameraViewTargetChanged, CameraBlendFinished,
// CameraCut) reach a graph through AN_OnEvent.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] CameraTable() => new[]
    {
        Reg("an_cameraaddrig", GenericKind.Exec, typeof(CameraGraph), "AddRigForGraph", "entity:int,preset:int=0,target:int=0", "success:bool", "p:entity,p:preset,p:target", "success"),
        Reg("an_camerasetmode", GenericKind.Exec, typeof(CameraGraph), "SetModeForGraph", "entity:int,mode:int=0", "success:bool", "p:entity,p:mode", "success"),
        Reg("an_camerasettarget", GenericKind.Exec, typeof(CameraGraph), "SetTargetForGraph", "entity:int,target:int", "success:bool", "p:entity,p:target", "success"),
        Reg("an_camerasetviewtarget", GenericKind.Exec, typeof(CameraGraph), "SetViewTargetForGraph", "entity:int,blendSeconds:float=0.5,curve:int=0", "success:bool", "p:entity,p:blendSeconds,p:curve", "success"),
        Reg("an_cameraclearviewtarget", GenericKind.Exec, typeof(CameraGraph), "ClearViewTargetForGraph", "blendSeconds:float=0.5,curve:int=0", "success:bool", "p:blendSeconds,p:curve", "success"),
        Reg("an_cameracut", GenericKind.Exec, typeof(CameraGraph), "CutForGraph", "entity:int=0", "success:bool", "p:entity", "success"),
        Reg("an_camerasetarm", GenericKind.Exec, typeof(CameraGraph), "SetArmForGraph", "entity:int,lengthCm:float=450,probeRadiusCm:float=12,collide:bool=true", "success:bool", "p:entity,p:lengthCm,p:probeRadiusCm,p:collide", "success"),
        Reg("an_camerasetsocket", GenericKind.Exec, typeof(CameraGraph), "SetSocketForGraph", "entity:int,x:float,y:float,z:float", "success:bool", "p:entity,p:x,p:y,p:z", "success"),
        Reg("an_camerasetpivot", GenericKind.Exec, typeof(CameraGraph), "SetPivotForGraph", "entity:int,x:float,y:float,z:float", "success:bool", "p:entity,p:x,p:y,p:z", "success"),
        Reg("an_camerasetlag", GenericKind.Exec, typeof(CameraGraph), "SetLagForGraph", "entity:int,posSpeed:float=10,rotSpeed:float=0,maxDistCm:float=0", "success:bool", "p:entity,p:posSpeed,p:rotSpeed,p:maxDistCm", "success"),
        Reg("an_camerasetlookahead", GenericKind.Exec, typeof(CameraGraph), "SetLookAheadForGraph", "entity:int,seconds:float=0.25,maxCm:float=150", "success:bool", "p:entity,p:seconds,p:maxCm", "success"),
        Reg("an_camerasetlimits", GenericKind.Exec, typeof(CameraGraph), "SetLimitsForGraph", "entity:int,pitchMinDeg:float=-80,pitchMaxDeg:float=80", "success:bool", "p:entity,p:pitchMinDeg,p:pitchMaxDeg", "success"),
        Reg("an_cameraaddlook", GenericKind.Exec, typeof(CameraGraph), "AddLookForGraph", "entity:int,yawDeg:float,pitchDeg:float", "success:bool", "p:entity,p:yawDeg,p:pitchDeg", "success"),
        Reg("an_camerasetfade", GenericKind.Exec, typeof(CameraGraph), "SetFadeForGraph", "entity:int,startCm:float=140,endCm:float=70", "success:bool", "p:entity,p:startCm,p:endCm", "success"),
        Reg("an_camerapushfovkick", GenericKind.Exec, typeof(CameraGraph), "PushFovKickForGraph", "deltaDeg:float=8,inSeconds:float=0.05,holdSeconds:float=0.1,outSeconds:float=0.4", "id:int", "p:deltaDeg,p:inSeconds,p:holdSeconds,p:outSeconds", "id"),
        Reg("an_camerapushzoom", GenericKind.Exec, typeof(CameraGraph), "PushZoomForGraph", "fovScale:float=0.6,blendSeconds:float=0.2", "id:int", "p:fovScale,p:blendSeconds", "id"),
        Reg("an_camerapunch", GenericKind.Exec, typeof(CameraGraph), "PunchForGraph", "pitchDeg:float,yawDeg:float,rollDeg:float,recoverSeconds:float=0.3", "id:int", "p:pitchDeg,p:yawDeg,p:rollDeg,p:recoverSeconds", "id"),
        Reg("an_cameraremovemodifier", GenericKind.Exec, typeof(CameraGraph), "RemoveModifierForGraph", "id:int,outSeconds:float=-1", "success:bool", "p:id,p:outSeconds", "success"),
        Reg("an_cameraclearmodifiers", GenericKind.Exec, typeof(CameraGraph), "ClearModifiersForGraph", "tag:int=0", "count:int", "p:tag", "count"),
        Reg("an_camerashakeat", GenericKind.Exec, typeof(CameraGraph), "ShakeAtForGraph", "x:float,y:float,z:float,radiusCm:float=1500,intensity:float=0.6", "success:bool", "p:x,p:y,p:z,p:radiusCm,p:intensity", "success"),
        Reg("an_cameragetpose", GenericKind.Pure, typeof(CameraGraph), "GetPoseForGraph", "", "x:float,y:float,z:float,yawDeg:float,pitchDeg:float,rollDeg:float,fovDeg:float,found:bool", ">x,>y,>z,>yawDeg,>pitchDeg,>rollDeg,>fovDeg", "found"),
        Reg("an_cameragetrigstate", GenericKind.Pure, typeof(CameraGraph), "GetRigStateForGraph", "entity:int", "armCm:float,colliding:bool,active:bool,blending:bool,found:bool", "p:entity,>armCm,>colliding,>active,>blending", "found"),
        Reg("an_cameragetviewtarget", GenericKind.Pure, typeof(CameraGraph), "GetViewTargetForGraph", "", "entity:int,blending:bool,blendAlpha:float", ">blending,>blendAlpha", "entity"),
    };
}
