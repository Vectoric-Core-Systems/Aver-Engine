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

/// <summary>Why a physics call failed, mirroring aver::AbiError in
/// modules/core/include/aver/core/ErrorCodes.hpp. Every value except <see cref="Ok"/> is negative,
/// so <c>(int)code &lt; 0</c> means "failed" even for a code added after this assembly was built.
/// </summary>
public enum PhysicsError
{
    /// <summary>No error was recorded by the last call that records one.</summary>
    Ok = 0,
    /// <summary>The handle is zero, out of range, or names a body that has been removed.</summary>
    BadHandle = -1,
    /// <summary>A required out-parameter was null.</summary>
    NullPointer = -2,
    /// <summary>There is no physics world: Init() was never called, or Shutdown() already ran.</summary>
    NotInitialised = -3,
    /// <summary>An index or count past the end of what exists.</summary>
    OutOfRange = -4,
    /// <summary>A real request this build cannot serve.</summary>
    Unsupported = -5,
    /// <summary>A value that is not a handle and is not legal.</summary>
    InvalidArgument = -6,
    /// <summary>The request was legal and the memory was not there.</summary>
    AllocationFailed = -7,
}
