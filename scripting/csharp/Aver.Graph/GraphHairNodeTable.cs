// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The strand hair rows of the game-systems node table (docs/plans/wave11/SOFTBODY_HAIR.md 2.20). Every method is in HairGraphInterop
// (Aver.Framework/Hair.cs), a thin wrapper over the Entity hair members. A hair crosses the graph as the entity id of the character that
// carries the CHair component, never a Jolt handle; the groom, the look material and the collider bone are named by attributes (the asset
// name is not a number, and a graph pin carries none). Set Hair Wind is the shared world wind (the soft bodies' wind), so cloth, ropes and
// hair move together.
// Palette mirror: sandbox/src/GraphNodeDefsHair.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] HairTable() => new[]
    {
        Reg("addhair", GenericKind.Exec, typeof(HairGraphInterop), "AddHairForGraph", "entity:int", "success:bool", "p:entity,@asset", "success"),
        // A scale below zero leaves that scale alone, so a node can change just one of the four.
        Reg("sethairparams", GenericKind.Exec, typeof(HairGraphInterop), "SetHairParamsForGraph", "entity:int,width:float=1,density:float=1,stiffness:float=1,wind:float=1", "success:bool", "p:entity,p:width,p:density,p:stiffness,p:wind", "success"),
        Reg("sethairlook", GenericKind.Exec, typeof(HairGraphInterop), "SetHairLookForGraph", "entity:int", "success:bool", "p:entity,@material", "success"),
        Reg("sethairenabled", GenericKind.Exec, typeof(HairGraphInterop), "SetHairEnabledForGraph", "entity:int,on:bool=true", "success:bool", "p:entity,p:on", "success"),
        Reg("teleporthair", GenericKind.Exec, typeof(HairGraphInterop), "TeleportHairForGraph", "entity:int", "success:bool", "p:entity", "success"),
        // shape 0 sphere (sx radius), 1 capsule (sx radius, sy half height), 2 box (sx sy sz half extents); centimetres, in the owner's space.
        Reg("addhaircollider", GenericKind.Exec, typeof(HairGraphInterop), "AddHairColliderForGraph", "entity:int,shape:int=0,sx:float=10,sy:float=0,sz:float=0", "success:bool", "p:entity,p:shape,p:sx,p:sy,p:sz,@bone", "success"),
        Reg("gethair", GenericKind.Pure, typeof(HairGraphInterop), "GetHairForGraph", "entity:int", "found:bool,simulating:bool,lod:int,renderStrands:int,simStrands:int", "p:entity,>simulating,>lod,>renderStrands,>simStrands", "found"),
        Reg("sethairwind", GenericKind.Exec, typeof(HairGraphInterop), "SetHairWindForGraph", "x:float,y:float,z:float,gust:float=0", "success:bool", "p:x,p:y,p:z,p:gust", "success"),
    };
}
