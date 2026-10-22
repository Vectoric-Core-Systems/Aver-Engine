// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The physical-surface rows of the game-systems node table (docs/plans/wave11/SHAPES_MATERIALS.md, Aver Node family
// "PhysicalSurface": 6 exec rows, 1 pure). Every method is in SurfaceGraph (Aver.Framework/SurfaceGraph.cs), a thin wrapper over
// PhysicalSurfaces, Entity's surface members and the registry. Palette mirror: sandbox/src/GraphNodeDefsPhysicalSurface.hpp.
// Entities are ints; a surface is the int id an_surfaceid and an_landscapesurface produce. The raycast is Exec for the reason the
// existing Raycast is: a cast per pull is too expensive to leave to pure evaluation.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] PhysicalSurfaceTable() => new[]
    {
        Reg("an_raycastsurface", GenericKind.Exec, typeof(SurfaceGraph), "RaycastSurfaceForGraph",
            "originX:float,originY:float,originZ:float,dirX:float,dirY:float,dirZ:float,maxDist:float=10000,layerMask:int=-1",
            "hit:bool,entity:int,pointX:float,pointY:float,pointZ:float,normalX:float,normalY:float,normalZ:float,distance:float,surfaceId:int,friction:float,restitution:float,tag:int",
            "p:originX,p:originY,p:originZ,p:dirX,p:dirY,p:dirZ,p:maxDist,p:layerMask,>entity,>pointX,>pointY,>pointZ,>normalX,>normalY,>normalZ,>distance,>surfaceId,>friction,>restitution,>tag",
            "hit"),
        Reg("an_surfacebeneath", GenericKind.Exec, typeof(SurfaceGraph), "BeneathForGraph", "entity:int,probeCm:float=150",
            "found:bool,surfaceId:int,friction:float,restitution:float,tag:int", "p:entity,p:probeCm,>surfaceId,>friction,>restitution,>tag", "found"),
        Reg("an_surfaceinfo", GenericKind.Pure, typeof(SurfaceGraph), "InfoForGraph", "surfaceId:int",
            "found:bool,friction:float,restitution:float,tag:int", "p:surfaceId,>friction,>restitution,>tag", "found"),
        Reg("an_setbodysurface", GenericKind.Exec, typeof(SurfaceGraph), "SetBodySurfaceForGraph", "entity:int,surfaceId:int,copyPhysics:bool=false",
            "success:bool", "p:entity,p:surfaceId,p:copyPhysics", "success"),
        Reg("an_registersurface", GenericKind.Exec, typeof(SurfaceGraph), "RegisterForGraph", "friction:float=-1,restitution:float=-1,tag:int=0",
            "surfaceId:int,success:bool", "@name,p:friction,p:restitution,p:tag,>surfaceId", "success"),
        Reg("an_tunesurface", GenericKind.Exec, typeof(SurfaceGraph), "TuneForGraph", "surfaceId:int,friction:float,restitution:float,tag:int",
            "success:bool", "p:surfaceId,p:friction,p:restitution,p:tag", "success"),
        Reg("an_setbodyscale", GenericKind.Exec, typeof(SurfaceGraph), "SetBodyScaleForGraph", "entity:int,scaleX:float,scaleY:float,scaleZ:float,keepMass:bool=false",
            "success:bool", "p:entity,p:scaleX,p:scaleY,p:scaleZ,p:keepMass", "success"),
    };
}
