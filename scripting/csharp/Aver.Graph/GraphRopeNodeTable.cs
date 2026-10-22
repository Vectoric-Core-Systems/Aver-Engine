// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The rope rows of the game-systems node table (docs/plans/wave11/SOFTBODY_HAIR.md 2.7). Every method is in RopeGraphInterop
// (Aver.Framework/SoftShapes.cs), a thin wrapper over Ropes and the Entity rope members. A rope crosses the graph as its entity id,
// never a Jolt handle; its ends, length and radius are the CSoftRope component's fields, so a graph and a script and the editor see
// one state. The tube's material is the rope asset's (an optional `asset` attribute on Create Rope names an .ocsoft of kind "rope").
// Palette mirror: sandbox/src/GraphNodeDefsRope.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] RopeTable() => new[]
    {
        Reg("createrope", GenericKind.Exec, typeof(RopeGraphInterop), "CreateRopeForGraph", "x1:float,y1:float,z1:float,x2:float,y2:float,z2:float,length:float=0,radius:float=0", "entity:int,success:bool", "p:x1,p:y1,p:z1,p:x2,p:y2,p:z2,p:length,p:radius,@asset?,>entity", "success"),
        Reg("setropeend", GenericKind.Exec, typeof(RopeGraphInterop), "SetRopeEndForGraph", "rope:int,end:int,anchor:int,ox:float,oy:float,oz:float", "success:bool", "p:rope,p:end,p:anchor,p:ox,p:oy,p:oz", "success"),
        Reg("freeropeend", GenericKind.Exec, typeof(RopeGraphInterop), "FreeRopeEndForGraph", "rope:int,end:int,x:float,y:float,z:float,pinned:bool", "success:bool", "p:rope,p:end,p:x,p:y,p:z,p:pinned", "success"),
        Reg("cutrope", GenericKind.Exec, typeof(RopeGraphInterop), "CutRopeForGraph", "rope:int,x:float,y:float,z:float", "other:int,success:bool", "p:rope,p:x,p:y,p:z,>other", "success"),
        Reg("getrope", GenericKind.Pure, typeof(RopeGraphInterop), "GetRopeForGraph", "rope:int", "found:bool,length:float,strain:float,nodes:int,cut:bool", "p:rope,>length,>strain,>nodes,>cut", "found"),
        Reg("destroyrope", GenericKind.Exec, typeof(RopeGraphInterop), "DestroyRopeForGraph", "rope:int", "success:bool", "p:rope", "success"),
    };
}
