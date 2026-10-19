// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The cloth and destruction rows of the game-systems node table (docs/SOFTBODY.md). Every method is in
// SoftBodyGraphInterop (Aver.Framework/SoftBodies.cs). Events reach graphs through the named-event seam
// (OnEvent SoftBodyTorn, SoftBodyDetached, SoftBodyDust, SoftBodyImpact, SoftBodyRepaired), so they need no node; Pop Soft Body Event
// gives the position and value those named events do not carry (it reads the queue DrainEvents reads: one consumer should own it).
// Palette mirror: sandbox/src/GraphNodeDefsSoftBody.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] SoftBodyTable() => new[]
    {
        Reg("softbodydamage", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SoftBodyDamageForGraph", "entity:int,x:float,y:float,z:float,radius:float=30,energy:float=100,dirX:float,dirY:float,dirZ:float", "success:bool", "p:entity,p:x,p:y,p:z,p:radius,p:energy,p:dirX,p:dirY,p:dirZ", "success"),
        Reg("softbodyexplode", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SoftBodyExplodeForGraph", "x:float,y:float,z:float,radius:float=300,impulse:float=600,energy:float=500", "touched:int,success:bool", "p:x,p:y,p:z,p:radius,p:impulse,p:energy,>touched", "success"),
        Reg("softbodytear", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SoftBodyTearForGraph", "entity:int,x:float,y:float,z:float,radius:float=20", "success:bool", "p:entity,p:x,p:y,p:z,p:radius", "success"),
        Reg("softbodyrepair", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SoftBodyRepairForGraph", "entity:int", "success:bool", "p:entity", "success"),
        Reg("releasesoftbody", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SoftBodyReleaseForGraph", "entity:int", "success:bool", "p:entity", "success"),
        Reg("setsoftbodywind", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SetSoftBodyWindForGraph", "vx:float,vy:float,vz:float,gust:float=0,gustHz:float=0.3,turbulence:float=0", "success:bool", "p:vx,p:vy,p:vz,p:gust,p:gustHz,p:turbulence", "success"),
        Reg("setsoftbodypreset", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SetSoftBodyPresetForGraph", "entity:int,preset:int=0", "success:bool", "p:entity,p:preset", "success"),
        Reg("getsoftbodystate", GenericKind.Pure, typeof(SoftBodyGraphInterop), "GetSoftBodyStateForGraph", "entity:int", "found:bool,damage:float,maxStrain:float,pieces:int,brokenEdges:int", "p:entity,>damage,>maxStrain,>pieces,>brokenEdges", "found"),
        // Version 2 (wave 10, docs/plans/wave10/SOFTBODY_DEPTH.md 2.13): seven rows appended; the eight above and their pins do not change, so saved graphs keep loading.
        Reg("popsoftbodyevent", GenericKind.Exec, typeof(SoftBodyGraphInterop), "PopSoftBodyEventForGraph", "", "found:bool,kind:int,entity:int,piece:int,x:float,y:float,z:float,value:float,success:bool", ">found,>kind,>entity,>piece,>x,>y,>z,>value", "success"),
        Reg("setsoftbodybudget", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SetSoftBodyBudgetForGraph", "maxPieces:int=48,maxParticles:int=20000,maxRebuilds:int=2,lifetime:float=12", "success:bool", "p:maxPieces,p:maxParticles,p:maxRebuilds,p:lifetime", "success"),
        Reg("clearsoftbodydebris", GenericKind.Exec, typeof(SoftBodyGraphInterop), "ClearSoftBodyDebrisForGraph", "entity:int", "success:bool", "p:entity", "success"),
        Reg("setsoftbodyasleep", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SetSoftBodyAsleepForGraph", "entity:int,asleep:bool", "success:bool", "p:entity,p:asleep", "success"),
        Reg("setsoftbodyeffects", GenericKind.Exec, typeof(SoftBodyGraphInterop), "SetSoftBodyEffectsForGraph", "enabled:bool=true", "success:bool", "p:enabled", "success"),
        Reg("repairallsoftbodies", GenericKind.Exec, typeof(SoftBodyGraphInterop), "RepairAllSoftBodiesForGraph", "", "repaired:int,success:bool", ">repaired", "success"),
        Reg("getsoftbodydents", GenericKind.Pure, typeof(SoftBodyGraphInterop), "GetSoftBodyDentsForGraph", "entity:int", "found:bool,dentedEdges:int,maxSetCm:float,debrisPieces:int", "p:entity,>dentedEdges,>maxSetCm,>debrisPieces", "found"),
    };
}
