// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Fields rows of the game-systems node table (docs/plans/wave10/SOFTBODY_DEPTH.md 2.13, category "Physics"): soft-body
// fields fired, started, spawned and tuned from a graph. Every method is in SoftFieldGraph (Aver.Framework/SoftFields.cs).
// The `an_` prefix keeps pulse, impact and start free for the plain verbs a user types. Enum values: kind 0 radial, 1 directional,
// 2 damage, 3 release anchors, 4 kill, 5 sleep; shape 0 sphere, 1 box, 2 capsule, 3 everywhere; targets a mask (1 cloth,
// 2 destructible, 4 debris; 7 = all). Node rotation is axis-aligned: a rotated box needs the C# SoftFieldSpec or an entity field.
// Palette mirror: sandbox/src/GraphNodeDefsFields.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] FieldsTable() => new[]
    {
        Reg("an_softfieldpulse", GenericKind.Exec, typeof(SoftFieldGraph), "SoftFieldPulseForGraph", "kind:int,shape:int,x:float,y:float,z:float,ex:float,ey:float,ez:float,dirX:float,dirY:float,dirZ:float,strength:float,energy:float,falloff:float=1,targets:int=7", "reached:int,success:bool", "p:kind,p:shape,p:x,p:y,p:z,p:ex,p:ey,p:ez,p:dirX,p:dirY,p:dirZ,p:strength,p:energy,p:falloff,p:targets,>reached", "success"),
        Reg("an_softfieldimpact", GenericKind.Exec, typeof(SoftFieldGraph), "SoftFieldImpactForGraph", "x:float,y:float,z:float,dirX:float,dirY:float,dirZ:float,radius:float=40,energy:float=500,kick:float=0", "reached:int,success:bool", "p:x,p:y,p:z,p:dirX,p:dirY,p:dirZ,p:radius,p:energy,p:kick,>reached", "success"),
        Reg("an_softfieldstart", GenericKind.Exec, typeof(SoftFieldGraph), "SoftFieldStartForGraph", "kind:int,shape:int,x:float,y:float,z:float,ex:float,ey:float,ez:float,dirX:float,dirY:float,dirZ:float,strength:float,energy:float,falloff:float=1,targets:int=7,lifetime:float=5", "handle:int,success:bool", "p:kind,p:shape,p:x,p:y,p:z,p:ex,p:ey,p:ez,p:dirX,p:dirY,p:dirZ,p:strength,p:energy,p:falloff,p:targets,p:lifetime,>handle", "success"),
        Reg("an_softfieldstop", GenericKind.Exec, typeof(SoftFieldGraph), "SoftFieldStopForGraph", "handle:int", "success:bool", "p:handle", "success"),
        Reg("an_spawnsoftfield", GenericKind.Exec, typeof(SoftFieldGraph), "SpawnSoftFieldForGraph", "kind:int,shape:int,x:float,y:float,z:float,ex:float,ey:float,ez:float,strength:float,energy:float,falloff:float=1,lifetime:float=5", "entity:int,success:bool", "p:kind,p:shape,p:x,p:y,p:z,p:ex,p:ey,p:ez,p:strength,p:energy,p:falloff,p:lifetime,>entity", "success"),
        Reg("an_softfieldsetenabled", GenericKind.Exec, typeof(SoftFieldGraph), "SoftFieldSetEnabledForGraph", "entity:int,enabled:bool=true", "success:bool", "p:entity,p:enabled", "success"),
        Reg("an_softfieldsetstrength", GenericKind.Exec, typeof(SoftFieldGraph), "SoftFieldSetStrengthForGraph", "entity:int,strength:float,energy:float", "success:bool", "p:entity,p:strength,p:energy", "success"),
        Reg("an_getsoftfieldtouched", GenericKind.Pure, typeof(SoftFieldGraph), "GetSoftFieldTouchedForGraph", "entity:int", "found:bool,touched:int", "p:entity,>touched", "found"),
    };
}
