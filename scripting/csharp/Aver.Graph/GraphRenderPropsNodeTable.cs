// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The light, camera and particle-emitter rows of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md
// section 2.2). Every method is in RenderPropsGraph (Aver.Framework/RenderPropsGraph.cs), a thin wrapper over the public
// Entity members in EntityRenderProps.cs. Palette mirror: sandbox/src/GraphNodeDefsRenderProps.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] RenderPropsTable() => new[]
    {
        Reg("an_setlightintensity", GenericKind.Exec, typeof(RenderPropsGraph), "SetLightIntensityForGraph", "entity:int,lux:float", "success:bool", "p:entity,p:lux", "success"),
        Reg("an_getlightintensity", GenericKind.Pure, typeof(RenderPropsGraph), "GetLightIntensityForGraph", "entity:int", "lux:float,found:bool", "p:entity,>lux", "found"),
        Reg("an_setlightcolor", GenericKind.Exec, typeof(RenderPropsGraph), "SetLightColorForGraph", "entity:int,r:float,g:float,b:float", "success:bool", "p:entity,p:r,p:g,p:b", "success"),
        Reg("an_setlightrange", GenericKind.Exec, typeof(RenderPropsGraph), "SetLightRangeForGraph", "entity:int,rangeCm:float", "success:bool", "p:entity,p:rangeCm", "success"),
        Reg("an_setlightcone", GenericKind.Exec, typeof(RenderPropsGraph), "SetLightConeForGraph", "entity:int,innerDeg:float=20,outerDeg:float=35", "success:bool", "p:entity,p:innerDeg,p:outerDeg", "success"),
        Reg("an_setlightshadows", GenericKind.Exec, typeof(RenderPropsGraph), "SetLightShadowsForGraph", "entity:int,enabled:bool=true", "success:bool", "p:entity,p:enabled", "success"),
        Reg("an_setcamerafov", GenericKind.Exec, typeof(RenderPropsGraph), "SetCameraFovForGraph", "entity:int,fovDeg:float=60", "success:bool", "p:entity,p:fovDeg", "success"),
        Reg("an_getcamerafov", GenericKind.Pure, typeof(RenderPropsGraph), "GetCameraFovForGraph", "entity:int", "fovDeg:float,found:bool", "p:entity,>fovDeg", "found"),
        Reg("an_setcameraclip", GenericKind.Exec, typeof(RenderPropsGraph), "SetCameraClipForGraph", "entity:int,nearCm:float=5,farCm:float=500000", "success:bool", "p:entity,p:nearCm,p:farCm", "success"),
        Reg("an_setcamerapriority", GenericKind.Exec, typeof(RenderPropsGraph), "SetCameraPriorityForGraph", "entity:int,priority:int", "success:bool", "p:entity,p:priority", "success"),
        Reg("an_setemitterenabled", GenericKind.Exec, typeof(RenderPropsGraph), "SetEmitterEnabledForGraph", "entity:int,enabled:bool=true", "success:bool", "p:entity,p:enabled", "success"),
        Reg("an_setemittereffect", GenericKind.Exec, typeof(RenderPropsGraph), "SetEmitterEffectForGraph", "entity:int", "success:bool", "p:entity,@effect", "success"),
        Reg("an_restartemitter", GenericKind.Exec, typeof(RenderPropsGraph), "RestartEmitterForGraph", "entity:int", "success:bool", "p:entity", "success"),
        Reg("an_spawnemitterat", GenericKind.Exec, typeof(RenderPropsGraph), "SpawnEmitterAtForGraph", "x:float,y:float,z:float,lifetime:float=3", "entity:int", "p:x,p:y,p:z,p:lifetime,@effect", "entity"),
    };
}
