// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Godot's CharacterBody3D.move_and_slide on a kinematic rigid body (G2). The native side is physics_kinematic_abi.h; docs/physics/GODOT_KINEMATIC.md has the
// rules. This is a second controller beside CharacterBody (the CharacterVirtual one), which is unchanged. Nothing is on by default: a body has no controller until
// KinematicCharacter.Create, and the engine builds in no Godot default. Create applies the options it is given, KinematicCharacterOptions.Godot unless the
// caller passes its own. Lengths are centimetres, angles radians, +Z up, left-handed (the engine's), so a metres-based Godot value is converted by the caller.

using System;
using System.Collections.Generic;

namespace Aver.Physics;

/// <summary>CharacterBody3D.motion_mode. Values are the header's AVER_PHYS_KCM_*.</summary>
public enum KinematicMotionMode
{
    /// <summary>Contacts are floor, wall or ceiling against up_direction; floor snap, platforms and stop-on-slope apply.</summary>
    Grounded = 0,
    /// <summary>Every contact is a wall and there is no floor logic.</summary>
    Floating = 1,
}

/// <summary>CharacterBody3D.platform_on_leave. Values are the header's AVER_PHYS_KCL_*.</summary>
public enum PlatformOnLeave
{
    /// <summary>The platform's last velocity is added to the body's.</summary>
    AddVelocity = 0,
    /// <summary>The same, but a downward platform velocity is dropped.</summary>
    AddUpwardVelocity = 1,
    DoNothing = 2,
}

/// <summary>What the last MoveAndSlide touched. Values are the header's AVER_PHYS_KCS_*.</summary>
[Flags]
public enum KinematicState
{
    Floor = 1,
    Wall = 2,
    Ceiling = 4,
}

// The numbers of the C blocks, as enums so the parity test pins them. A vector member is the index of its X.
internal enum KinematicParam
{
    MotionMode = 0, MaxSlides = 1, FloorMaxAngle = 2, WallMinSlideAngle = 3, FloorSnapLength = 4, SafeMargin = 5, FloorStopOnSlope = 6,
    FloorConstantSpeed = 7, FloorBlockOnWall = 8, SlideOnCeiling = 9, PlatformOnLeave = 10, Count = 11,
}
internal enum KinematicInfoFloat { FloorNormal = 0, WallNormal = 3, PlatformVelocity = 6, RealVelocity = 9, LastMotion = 12, PositionDelta = 15, Count = 18 }
internal enum KinematicInfoInt { State = 0, PlatformBody = 1, Collisions = 2, Count = 3 }
internal enum KinematicCollisionFloat { Point = 0, Normal = 3, ColliderVelocity = 6, Remainder = 9, Travel = 12, Depth = 15, Angle = 16, Count = 17 }
internal enum KinematicCollisionInt { Collider = 0, Entity = 1, Subshape = 2, Count = 3 }

/// <summary>Every option of a <see cref="KinematicCharacter"/>. <see cref="Godot"/> holds CharacterBody3D's defaults in centimetres; pass your own record
/// (<c>Godot with { MaxSlides = 4 }</c>) to change some.</summary>
public sealed record KinematicCharacterOptions
{
    public KinematicMotionMode MotionMode { get; init; } = KinematicMotionMode.Grounded;
    public int MaxSlides { get; init; } = 6;
    /// <summary>Radians: a surface within this angle of up is floor.</summary>
    public float FloorMaxAngle { get; init; } = 45f * MathF.PI / 180f;
    /// <summary>Radians; floating mode only.</summary>
    public float WallMinSlideAngle { get; init; } = 15f * MathF.PI / 180f;
    public float FloorSnapLengthCm { get; init; } = 10f;
    public float SafeMarginCm { get; init; } = 0.1f;
    public bool FloorStopOnSlope { get; init; } = true;
    public bool FloorConstantSpeed { get; init; }
    public bool FloorBlockOnWall { get; init; } = true;
    public bool SlideOnCeiling { get; init; } = true;
    public PlatformOnLeave PlatformOnLeave { get; init; } = PlatformOnLeave.AddVelocity;
    public uint PlatformFloorLayers { get; init; } = uint.MaxValue;
    public uint PlatformWallLayers { get; init; }
    public Float3 UpDirection { get; init; } = Float3.Up;

    /// <summary>Godot's CharacterBody3D defaults (safe_margin 0.001 m = 0.1 cm, floor_snap_length 0.1 m = 10 cm).</summary>
    public static KinematicCharacterOptions Godot { get; } = new();
}

/// <summary>One collision of the last <see cref="KinematicCharacter.MoveAndSlide"/> (Godot KinematicCollision3D).</summary>
public readonly record struct SlideCollision(Float3 Position, Float3 Normal, Body Collider, int Entity, int SubShape, Float3 ColliderVelocity,
                                              Float3 Remainder, Float3 Travel, float Depth, float Angle);

/// <summary>A kinematic rigid body driven like Godot's CharacterBody3D. A handle (the controller's own); 0 is invalid.</summary>
public readonly struct KinematicCharacter : IEquatable<KinematicCharacter>
{
    public int Handle { get; }
    public KinematicCharacter(int handle) { Handle = handle; }
    public bool IsValid => Native.aver_phys_kc_exists(Handle) != 0;
    public Body Body => new(Native.aver_phys_kc_body(Handle));
    public static int Count => Native.aver_phys_kc_count();

    /// <summary>Puts a controller on a kinematic body and applies <paramref name="options"/> (Godot's defaults when null). Invalid when the body is not kinematic,
    /// is a mesh, heightfield or soft body, or already has a controller.</summary>
    public static KinematicCharacter Create(Body body, KinematicCharacterOptions? options = null)
    {
        int h = Native.aver_phys_kc_create(body.Handle);
        if (h == 0) return default;
        var c = new KinematicCharacter(h);
        c.Apply(options ?? KinematicCharacterOptions.Godot);
        return c;
    }

    /// <summary>Removes the controller; the body stays.</summary>
    public bool Destroy() => Native.aver_phys_kc_destroy(Handle) != 0;

    /// <summary>Sets every option.</summary>
    public void Apply(KinematicCharacterOptions o)
    {
        MotionMode = o.MotionMode;
        MaxSlides = o.MaxSlides;
        FloorMaxAngle = o.FloorMaxAngle;
        WallMinSlideAngle = o.WallMinSlideAngle;
        FloorSnapLengthCm = o.FloorSnapLengthCm;
        SafeMarginCm = o.SafeMarginCm;
        FloorStopOnSlope = o.FloorStopOnSlope;
        FloorConstantSpeed = o.FloorConstantSpeed;
        FloorBlockOnWall = o.FloorBlockOnWall;
        SlideOnCeiling = o.SlideOnCeiling;
        PlatformOnLeave = o.PlatformOnLeave;
        SetPlatformLayers(o.PlatformFloorLayers, o.PlatformWallLayers);
        UpDirection = o.UpDirection;
    }

    float Param(KinematicParam p) => Native.aver_phys_kc_param(Handle, (int)p, out float v) != 0 ? v : 0f;
    void SetParam(KinematicParam p, float v) => Native.aver_phys_kc_set_param(Handle, (int)p, v);

    // ---- CharacterBody3D properties ----
    public KinematicMotionMode MotionMode { get => (KinematicMotionMode)(int)Param(KinematicParam.MotionMode); set => SetParam(KinematicParam.MotionMode, (int)value); }
    public int MaxSlides { get => (int)Param(KinematicParam.MaxSlides); set => SetParam(KinematicParam.MaxSlides, value); }
    public float FloorMaxAngle { get => Param(KinematicParam.FloorMaxAngle); set => SetParam(KinematicParam.FloorMaxAngle, value); }
    public float WallMinSlideAngle { get => Param(KinematicParam.WallMinSlideAngle); set => SetParam(KinematicParam.WallMinSlideAngle, value); }
    public float FloorSnapLengthCm { get => Param(KinematicParam.FloorSnapLength); set => SetParam(KinematicParam.FloorSnapLength, value); }
    public float SafeMarginCm { get => Param(KinematicParam.SafeMargin); set => SetParam(KinematicParam.SafeMargin, value); }
    public bool FloorStopOnSlope { get => Param(KinematicParam.FloorStopOnSlope) != 0f; set => SetParam(KinematicParam.FloorStopOnSlope, value ? 1f : 0f); }
    public bool FloorConstantSpeed { get => Param(KinematicParam.FloorConstantSpeed) != 0f; set => SetParam(KinematicParam.FloorConstantSpeed, value ? 1f : 0f); }
    public bool FloorBlockOnWall { get => Param(KinematicParam.FloorBlockOnWall) != 0f; set => SetParam(KinematicParam.FloorBlockOnWall, value ? 1f : 0f); }
    public bool SlideOnCeiling { get => Param(KinematicParam.SlideOnCeiling) != 0f; set => SetParam(KinematicParam.SlideOnCeiling, value ? 1f : 0f); }
    public PlatformOnLeave PlatformOnLeave { get => (PlatformOnLeave)(int)Param(KinematicParam.PlatformOnLeave); set => SetParam(KinematicParam.PlatformOnLeave, (int)value); }

    public Float3 UpDirection
    {
        get { var u = new float[3]; return Native.aver_phys_kc_up(Handle, u) != 0 ? new Float3(u[0], u[1], u[2]) : Float3.Up; }
        set => Native.aver_phys_kc_set_up(Handle, new[] { value.X, value.Y, value.Z });
    }

    /// <summary>The velocity the script drives (cm/s, world). MoveAndSlide reads it and writes it back slid along what it hit.</summary>
    public Float3 Velocity
    {
        get { var v = new float[3]; return Native.aver_phys_kc_velocity(Handle, v) != 0 ? new Float3(v[0], v[1], v[2]) : Float3.Zero; }
        set => Native.aver_phys_kc_set_velocity(Handle, new[] { value.X, value.Y, value.Z });
    }

    public (uint Floor, uint Wall) PlatformLayers
        => Native.aver_phys_kc_platform_layers(Handle, out uint f, out uint w) != 0 ? (f, w) : (0u, 0u);
    public bool SetPlatformLayers(uint floorLayers, uint wallLayers) => Native.aver_phys_kc_set_platform_layers(Handle, floorLayers, wallLayers) != 0;

    // ---- the call ----
    /// <summary>Godot's move_and_slide for a step of <paramref name="dt"/> seconds. Returns true when at least one collision was recorded.</summary>
    public bool MoveAndSlide(float dt) => Native.aver_phys_kc_move_and_slide(Handle, dt, out int collided) != 0 && collided != 0;

    /// <summary>Forgets the results and the platform (after a teleport). The velocity is kept.</summary>
    public bool Clear() => Native.aver_phys_kc_clear(Handle) != 0;

    // ---- results ----
    public KinematicState State => Native.aver_phys_kc_state(Handle, out int s) != 0 ? (KinematicState)s : default;
    public bool IsOnFloor => (State & KinematicState.Floor) != 0;
    public bool IsOnWall => (State & KinematicState.Wall) != 0;
    public bool IsOnCeiling => (State & KinematicState.Ceiling) != 0;
    public bool IsOnFloorOnly => State == KinematicState.Floor;
    public bool IsOnWallOnly => State == KinematicState.Wall;
    public bool IsOnCeilingOnly => State == KinematicState.Ceiling;

    (float[] F, int[] I) Info()
    {
        var f = new float[(int)KinematicInfoFloat.Count];
        var i = new int[(int)KinematicInfoInt.Count];
        Native.aver_phys_kc_info(Handle, f, f.Length, i, i.Length);
        return (f, i);
    }
    static Float3 V(float[] f, KinematicInfoFloat at) => new(f[(int)at], f[(int)at + 1], f[(int)at + 2]);

    public Float3 GetFloorNormal() => V(Info().F, KinematicInfoFloat.FloorNormal);
    public Float3 GetWallNormal() => V(Info().F, KinematicInfoFloat.WallNormal);
    public Float3 GetPlatformVelocity() => V(Info().F, KinematicInfoFloat.PlatformVelocity);
    public Float3 GetRealVelocity() => V(Info().F, KinematicInfoFloat.RealVelocity);
    public Float3 GetLastMotion() => V(Info().F, KinematicInfoFloat.LastMotion);
    public Float3 GetPositionDelta() => V(Info().F, KinematicInfoFloat.PositionDelta);
    /// <summary>The body carrying this one (get_platform_rid); invalid when none.</summary>
    public Body GetPlatform() => new(Info().I[(int)KinematicInfoInt.PlatformBody]);

    /// <summary>The angle between the floor normal and up, radians; 0 when not on the floor.</summary>
    public float GetFloorAngle()
    {
        if (!IsOnFloor) return 0f;
        var n = GetFloorNormal(); var u = UpDirection;
        return MathF.Acos(Math.Clamp(n.X * u.X + n.Y * u.Y + n.Z * u.Z, -1f, 1f));
    }

    public int GetSlideCollisionCount() => Info().I[(int)KinematicInfoInt.Collisions];

    public SlideCollision? GetSlideCollision(int index)
    {
        var f = new float[(int)KinematicCollisionFloat.Count];
        var i = new int[(int)KinematicCollisionInt.Count];
        if (Native.aver_phys_kc_collision(Handle, index, f, f.Length, i, i.Length) == 0) return null;
        return new SlideCollision(V3(f, KinematicCollisionFloat.Point), V3(f, KinematicCollisionFloat.Normal), new Body(i[(int)KinematicCollisionInt.Collider]),
                                  i[(int)KinematicCollisionInt.Entity], i[(int)KinematicCollisionInt.Subshape], V3(f, KinematicCollisionFloat.ColliderVelocity),
                                  V3(f, KinematicCollisionFloat.Remainder), V3(f, KinematicCollisionFloat.Travel), f[(int)KinematicCollisionFloat.Depth],
                                  f[(int)KinematicCollisionFloat.Angle]);
    }
    static Float3 V3(float[] f, KinematicCollisionFloat at) => new(f[(int)at], f[(int)at + 1], f[(int)at + 2]);

    /// <summary>The last collision of the call, or null when there was none.</summary>
    public SlideCollision? GetLastSlideCollision()
    {
        int n = GetSlideCollisionCount();
        return n > 0 ? GetSlideCollision(n - 1) : null;
    }

    /// <summary>Every collision of the last call, in the order they happened.</summary>
    public IReadOnlyList<SlideCollision> SlideCollisions
    {
        get
        {
            int n = GetSlideCollisionCount();
            var list = new List<SlideCollision>(n);
            for (int k = 0; k < n; k++)
                if (GetSlideCollision(k) is { } c) list.Add(c);
            return list;
        }
    }

    public bool Equals(KinematicCharacter other) => Handle == other.Handle;
    public override bool Equals(object? obj) => obj is KinematicCharacter o && Equals(o);
    public override int GetHashCode() => Handle;
    public static bool operator ==(KinematicCharacter a, KinematicCharacter b) => a.Handle == b.Handle;
    public static bool operator !=(KinematicCharacter a, KinematicCharacter b) => a.Handle != b.Handle;
}
