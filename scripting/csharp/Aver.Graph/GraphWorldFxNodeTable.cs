// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The sun, fog, sky-light and post-layer rows (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.4). Every method is
// in WorldFxGraph (Aver.Framework/WorldFxGraph.cs), a thin wrapper over Sky and PostFx. All are Exec and return
// success. An empty layer means the layer named "script"; an empty environment key releases every override.
// Palette mirror: sandbox/src/GraphNodeDefsWorldFx.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] WorldFxTable() => new[]
    {
        Reg("setsundirection", GenericKind.Exec, typeof(WorldFxGraph), "SetSunDirectionForGraph", "x:float,y:float,z:float", "success:bool", "p:x,p:y,p:z", "success"),
        Reg("setsunintensity", GenericKind.Exec, typeof(WorldFxGraph), "SetSunIntensityForGraph", "lux:float", "success:bool", "p:lux", "success"),
        Reg("setsuncolor", GenericKind.Exec, typeof(WorldFxGraph), "SetSunColorForGraph", "r:float,g:float,b:float,temperatureK:float=0", "success:bool", "p:r,p:g,p:b,p:temperatureK", "success"),
        Reg("setfogdensity", GenericKind.Exec, typeof(WorldFxGraph), "SetFogDensityForGraph", "density:float", "success:bool", "p:density", "success"),
        Reg("setfogcolor", GenericKind.Exec, typeof(WorldFxGraph), "SetFogColorForGraph", "r:float,g:float,b:float", "success:bool", "p:r,p:g,p:b", "success"),
        Reg("setfogshape", GenericKind.Exec, typeof(WorldFxGraph), "SetFogShapeForGraph", "falloff:float,height:float,start:float,maxOpacity:float=1", "success:bool", "p:falloff,p:height,p:start,p:maxOpacity", "success"),
        Reg("setskylight", GenericKind.Exec, typeof(WorldFxGraph), "SetSkyLightForGraph", "multiplier:float=1", "success:bool", "p:multiplier", "success"),
        Reg("clearenvironment", GenericKind.Exec, typeof(WorldFxGraph), "ClearEnvironmentForGraph", "", "success:bool", "@key?", "success"),
        Reg("setpostvalue", GenericKind.Exec, typeof(WorldFxGraph), "SetPostValueForGraph", "v0:float,v1:float,v2:float", "success:bool", "@layer?,@key,p:v0,p:v1,p:v2", "success"),
        Reg("setpostlayerweight", GenericKind.Exec, typeof(WorldFxGraph), "SetPostLayerWeightForGraph", "weight:float=1,blendSeconds:float=0", "success:bool", "@layer?,p:weight,p:blendSeconds", "success"),
        Reg("clearpostlayer", GenericKind.Exec, typeof(WorldFxGraph), "ClearPostLayerForGraph", "", "success:bool", "@layer?", "success"),
        Reg("setpostvolumeweight", GenericKind.Exec, typeof(WorldFxGraph), "SetPostVolumeWeightForGraph", "weight:float,blendSeconds:float", "success:bool", "@volume,p:weight,p:blendSeconds", "success"),
    };
}
