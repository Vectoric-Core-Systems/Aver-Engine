// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Math family of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): trig, ranges and
// interpolation, integer and bit operations, random draws, vector and rotator helpers. Every method is in MathGraph
// (Aver.Framework/AverMath.cs) over the public AverMath and AverRandom. The random rows are Exec because a pure node is
// recomputed per reader and two readers would get two values; everything else is pure.
// Palette mirror: sandbox/src/GraphNodeDefsMath.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] MathTable() => new[]
    {
        Reg("an_tan", GenericKind.Pure, typeof(MathGraph), "TanForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_asin", GenericKind.Pure, typeof(MathGraph), "AsinForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_acos", GenericKind.Pure, typeof(MathGraph), "AcosForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_atan", GenericKind.Pure, typeof(MathGraph), "AtanForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_atan2", GenericKind.Pure, typeof(MathGraph), "Atan2ForGraph", "y:float,x:float", "result:float", "p:y,p:x", "result"),
        Reg("an_sign", GenericKind.Pure, typeof(MathGraph), "SignForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_frac", GenericKind.Pure, typeof(MathGraph), "FracForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_truncate", GenericKind.Pure, typeof(MathGraph), "TruncateForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_degtorad", GenericKind.Pure, typeof(MathGraph), "DegToRadForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_radtodeg", GenericKind.Pure, typeof(MathGraph), "RadToDegForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_exp", GenericKind.Pure, typeof(MathGraph), "ExpForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_log", GenericKind.Pure, typeof(MathGraph), "LogForGraph", "a:float", "result:float", "p:a", "result"),
        Reg("an_smoothstep", GenericKind.Pure, typeof(MathGraph), "SmoothStepForGraph", "edge0:float,edge1:float,x:float", "result:float", "p:edge0,p:edge1,p:x", "result"),
        Reg("an_inverselerp", GenericKind.Pure, typeof(MathGraph), "InverseLerpForGraph", "a:float,b:float,value:float", "result:float", "p:a,p:b,p:value", "result"),
        Reg("an_maprange", GenericKind.Pure, typeof(MathGraph), "MapRangeForGraph", "value:float,inMin:float=0,inMax:float=1,outMin:float=0,outMax:float=1,clamp:bool=true", "result:float", "p:value,p:inMin,p:inMax,p:outMin,p:outMax,p:clamp", "result"),
        Reg("an_wrap", GenericKind.Pure, typeof(MathGraph), "WrapForGraph", "value:float,min:float,max:float", "result:float", "p:value,p:min,p:max", "result"),
        Reg("an_interpto", GenericKind.Pure, typeof(MathGraph), "InterpToForGraph", "current:float,target:float,deltaTime:float,speed:float", "result:float", "p:current,p:target,p:deltaTime,p:speed", "result"),
        Reg("an_deltaangle", GenericKind.Pure, typeof(MathGraph), "DeltaAngleForGraph", "a:float,b:float", "result:float", "p:a,p:b", "result"),
        Reg("an_lerpangle", GenericKind.Pure, typeof(MathGraph), "LerpAngleForGraph", "a:float,b:float,t:float", "result:float", "p:a,p:b,p:t", "result"),
        Reg("an_nearlyequal", GenericKind.Pure, typeof(MathGraph), "NearlyEqualForGraph", "a:float,b:float,tolerance:float=0.0001", "result:bool", "p:a,p:b,p:tolerance", "result"),
        Reg("an_isfinite", GenericKind.Pure, typeof(MathGraph), "IsFiniteForGraph", "a:float", "result:bool", "p:a", "result"),
        Reg("an_intadd", GenericKind.Pure, typeof(MathGraph), "IntAddForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intsubtract", GenericKind.Pure, typeof(MathGraph), "IntSubtractForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intmultiply", GenericKind.Pure, typeof(MathGraph), "IntMultiplyForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intmin", GenericKind.Pure, typeof(MathGraph), "IntMinForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intmax", GenericKind.Pure, typeof(MathGraph), "IntMaxForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intdivide", GenericKind.Pure, typeof(MathGraph), "IntDivideForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intmodulo", GenericKind.Pure, typeof(MathGraph), "IntModuloForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_intclamp", GenericKind.Pure, typeof(MathGraph), "IntClampForGraph", "a:int,min:int,max:int", "result:int", "p:a,p:min,p:max", "result"),
        Reg("an_intabs", GenericKind.Pure, typeof(MathGraph), "IntAbsForGraph", "a:int", "result:int", "p:a", "result"),
        Reg("an_intsign", GenericKind.Pure, typeof(MathGraph), "IntSignForGraph", "a:int", "result:int", "p:a", "result"),
        Reg("an_intcompare", GenericKind.Pure, typeof(MathGraph), "IntCompareForGraph", "a:int,b:int", "less:bool,equal:bool,greater:bool", "p:a,p:b,>less,>equal,>greater", null),
        Reg("an_bitand", GenericKind.Pure, typeof(MathGraph), "BitAndForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_bitor", GenericKind.Pure, typeof(MathGraph), "BitOrForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_bitxor", GenericKind.Pure, typeof(MathGraph), "BitXorForGraph", "a:int,b:int", "result:int", "p:a,p:b", "result"),
        Reg("an_bitshiftleft", GenericKind.Pure, typeof(MathGraph), "BitShiftLeftForGraph", "a:int,count:int", "result:int", "p:a,p:count", "result"),
        Reg("an_bitshiftright", GenericKind.Pure, typeof(MathGraph), "BitShiftRightForGraph", "a:int,count:int", "result:int", "p:a,p:count", "result"),
        Reg("an_bittest", GenericKind.Pure, typeof(MathGraph), "BitTestForGraph", "value:int,bit:int", "result:bool", "p:value,p:bit", "result"),
        Reg("an_randomfloat", GenericKind.Exec, typeof(MathGraph), "RandomFloatForGraph", "min:float=0,max:float=1", "value:float", "p:min,p:max", "value"),
        Reg("an_randomint", GenericKind.Exec, typeof(MathGraph), "RandomIntForGraph", "min:int=0,max:int=1", "value:int", "p:min,p:max", "value"),
        Reg("an_randombool", GenericKind.Exec, typeof(MathGraph), "RandomBoolForGraph", "chance:float=0.5", "value:bool", "p:chance", "value"),
        Reg("an_randomunitvector", GenericKind.Exec, typeof(MathGraph), "RandomUnitVectorForGraph", "", "x:float,y:float,z:float", ">x,>y,>z", null),
        Reg("an_setrandomseed", GenericKind.Exec, typeof(MathGraph), "SetRandomSeedForGraph", "seed:int", "", "p:seed", null),
        Reg("an_vecnegate", GenericKind.Pure, typeof(MathGraph), "VecNegateForGraph", "ax:float,ay:float,az:float", "x:float,y:float,z:float", "p:ax,p:ay,p:az,>x,>y,>z", null),
        Reg("an_vecmultiply", GenericKind.Pure, typeof(MathGraph), "VecMultiplyForGraph", "ax:float,ay:float,az:float,bx:float,by:float,bz:float", "x:float,y:float,z:float", "p:ax,p:ay,p:az,p:bx,p:by,p:bz,>x,>y,>z", null),
        Reg("an_veclength2d", GenericKind.Pure, typeof(MathGraph), "VecLength2DForGraph", "ax:float,ay:float", "result:float", "p:ax,p:ay", "result"),
        Reg("an_vecdistance2d", GenericKind.Pure, typeof(MathGraph), "VecDistance2DForGraph", "ax:float,ay:float,bx:float,by:float", "result:float", "p:ax,p:ay,p:bx,p:by", "result"),
        Reg("an_vecreflect", GenericKind.Pure, typeof(MathGraph), "VecReflectForGraph", "vx:float,vy:float,vz:float,nx:float,ny:float,nz:float", "x:float,y:float,z:float", "p:vx,p:vy,p:vz,p:nx,p:ny,p:nz,>x,>y,>z", null),
        Reg("an_vecproject", GenericKind.Pure, typeof(MathGraph), "VecProjectForGraph", "ax:float,ay:float,az:float,bx:float,by:float,bz:float", "x:float,y:float,z:float", "p:ax,p:ay,p:az,p:bx,p:by,p:bz,>x,>y,>z", null),
        Reg("an_vecclamplength", GenericKind.Pure, typeof(MathGraph), "VecClampLengthForGraph", "vx:float,vy:float,vz:float,maxLength:float", "x:float,y:float,z:float", "p:vx,p:vy,p:vz,p:maxLength,>x,>y,>z", null),
        Reg("an_vecangle", GenericKind.Pure, typeof(MathGraph), "VecAngleForGraph", "ax:float,ay:float,az:float,bx:float,by:float,bz:float", "result:float", "p:ax,p:ay,p:az,p:bx,p:by,p:bz", "result"),
        Reg("an_rotatortoforward", GenericKind.Pure, typeof(MathGraph), "RotatorToForwardForGraph", "yaw:float,pitch:float", "x:float,y:float,z:float", "p:yaw,p:pitch,>x,>y,>z", null),
        Reg("an_forwardtorotator", GenericKind.Pure, typeof(MathGraph), "ForwardToRotatorForGraph", "x:float,y:float,z:float", "yaw:float,pitch:float", "p:x,p:y,p:z,>yaw,>pitch", null),
        Reg("an_rotatevector", GenericKind.Pure, typeof(MathGraph), "RotateVectorForGraph", "vx:float,vy:float,vz:float,yaw:float,pitch:float,roll:float", "x:float,y:float,z:float", "p:vx,p:vy,p:vz,p:yaw,p:pitch,p:roll,>x,>y,>z", null),
        Reg("an_lookatrotation", GenericKind.Pure, typeof(MathGraph), "LookAtRotationForGraph", "fromX:float,fromY:float,fromZ:float,toX:float,toY:float,toZ:float", "yaw:float,pitch:float", "p:fromX,p:fromY,p:fromZ,p:toX,p:toY,p:toZ,>yaw,>pitch", null),
    };
}
