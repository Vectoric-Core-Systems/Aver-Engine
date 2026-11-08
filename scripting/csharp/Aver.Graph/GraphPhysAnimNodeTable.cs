// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The physical animation rows of the game-systems node table (docs/physics/PHYSICAL_ANIMATION.md): hit reactions, whole-body
// and per-bone physical animation, partial-body simulation, death and getting up. Every method is in PhysAnimGraphInterop
// (Aver.Framework/PhysicalAnimation.cs), a thin wrapper over the Entity members. Bones and the profile are named by NODE
// attributes, never pins; an empty bone means the root part (Apply Hit Reaction) or the whole body (Simulate Bone Subtree).
// Palette mirror: sandbox/src/GraphNodeDefsPhysAnim.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] PhysAnimTable() => new[]
    {
        // Impulse in kg*cm/s (about 3000 is a full reaction with the default profile); point and radius only matter for a radial event (radius > 0).
        Reg("applyhitreaction", GenericKind.Exec, typeof(PhysAnimGraphInterop), "ApplyHitReactionForGraph", "entity:int,x:float,y:float,z:float,px:float,py:float,pz:float,damage:float,radius:float", "success:bool", "p:entity,@bone?,p:x,p:y,p:z,p:px,p:py,p:pz,p:damage,p:radius", "success"),
        Reg("setphysicalanimationprofile", GenericKind.Exec, typeof(PhysAnimGraphInterop), "SetPhysicalAnimationProfileForGraph", "entity:int", "success:bool", "p:entity,@profile?", "success"),
        Reg("blendphysicalanimation", GenericKind.Exec, typeof(PhysAnimGraphInterop), "BlendPhysicalAnimationForGraph", "entity:int,weight:float,seconds:float=0.2", "success:bool", "p:entity,p:weight,p:seconds", "success"),
        Reg("simulatebonesubtree", GenericKind.Exec, typeof(PhysAnimGraphInterop), "SimulateBoneSubtreeForGraph", "entity:int", "success:bool", "p:entity,@bone?", "success"),
        Reg("setbonestrength", GenericKind.Exec, typeof(PhysAnimGraphInterop), "SetBoneStrengthForGraph", "entity:int,strength:float", "success:bool", "p:entity,@bone,p:strength", "success"),
        Reg("setboneblend", GenericKind.Exec, typeof(PhysAnimGraphInterop), "SetBoneBlendForGraph", "entity:int,blend:float", "success:bool", "p:entity,@bone,p:blend", "success"),
        Reg("pinbone", GenericKind.Exec, typeof(PhysAnimGraphInterop), "PinBoneForGraph", "entity:int,pinned:bool=true", "success:bool", "p:entity,@bone,p:pinned", "success"),
        Reg("dieasragdoll", GenericKind.Exec, typeof(PhysAnimGraphInterop), "DieAsRagdollForGraph", "entity:int", "success:bool", "p:entity", "success"),
        Reg("getupfromragdoll", GenericKind.Exec, typeof(PhysAnimGraphInterop), "GetUpFromRagdollForGraph", "entity:int", "success:bool", "p:entity", "success"),
        // life: 0 Alive, 1 Dying, 2 Dead, 3 GettingUp. facing: 0 Upright, 1 Supine, 2 Prone, 3 Side. found is false (and the outputs read Alive, Upright, 0) with no ragdoll.
        Reg("getphysicalanimationstate", GenericKind.Pure, typeof(PhysAnimGraphInterop), "GetPhysicalAnimationStateForGraph", "entity:int", "found:bool,life:int,facing:int,reacting:int", "p:entity,>life,>facing,>reacting", "found"),
    };
}
