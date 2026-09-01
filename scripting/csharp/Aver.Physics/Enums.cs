// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// The header's #define groups, as real enums. Values are copied from the ABI headers, not
// renumbered -- an int parameter that can only be one of a handful of values is where a caller's
// off-by-one becomes a body that will not move rather than a compiler error.

namespace Aver.Physics;

/// <summary>Matches JPH::EMotionType's own declared order (physics_abi.h), so this enum and Jolt's
/// cannot drift apart silently.</summary>
public enum MotionType
{
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
}

/// <summary>What a joint motor is doing. Off still respects the joint's limits; it is not the same as
/// having no motor at all.</summary>
public enum MotorState
{
    Off = 0,
    Velocity = 1,
    Position = 2,
}

/// <summary>The six independent axes a <see cref="Joint.SixDof"/> constrains, in the order
/// AVER_PHYS_DOF_* declares them (physics_joints_abi.h): three translations then three rotations.
/// Also selects which axis <see cref="Joint.SetMotor"/>/<c>SetMotorStrength</c>/<c>SetLimits</c> act
/// on for a six-DOF joint; every other joint type ignores it.</summary>
public enum SixDofAxis
{
    TranslationX = 0,
    TranslationY = 1,
    TranslationZ = 2,
    RotationX = 3,
    RotationY = 4,
    RotationZ = 5,
}

/// <summary>Jolt's four-way answer for what a character is standing on, IN JOLT'S OWN DECLARED ORDER
/// (CharacterBase::EGroundState, mirrored by AVER_PHYS_GROUND_* in physics_character_abi.h) so this
/// enum and Jolt's cannot drift apart silently the way <see cref="MotionType"/> already guards
/// against for bodies.</summary>
public enum GroundState
{
    /// <summary>Walking freely.</summary>
    OnGround = 0,
    /// <summary>Touching ground too steep to climb; slides if not held.</summary>
    OnSteepGround = 1,
    /// <summary>Touching something, but not standing on it -- should fall.</summary>
    NotSupported = 2,
    /// <summary>Touching nothing.</summary>
    InAir = 3,
}
