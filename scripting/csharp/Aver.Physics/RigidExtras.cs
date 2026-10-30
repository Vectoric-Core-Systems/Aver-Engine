// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Rigid-body extras on Aver.Physics: an integrate-forces callback and a custom integrator, constant velocity on static bodies, per-shape
// disable. The native side is physics_body_ext_abi.h and physics_shape_builder_abi.h; docs/PHYSICS_SHAPES.md has the rules. Nothing here is
// on by default, and no imported default is built in: the caller passes every value.

using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Aver.Physics;

/// <summary>What <see cref="Body.SetIntegrateForces"/> hands the callback each fixed step: a copy of the body's state.
/// Write <see cref="LinearVelocity"/> and <see cref="AngularVelocity"/> to change the body's motion; the
/// rest is read-only. Mirrors aver_phys_direct_state, so the field order is the header's.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct DirectBodyState
{
    public int BodyHandle;
    /// <summary>Tracked contacts if the body is contact-monitored (<see cref="Body.SetContactMonitor"/>), else 0.</summary>
    public int ContactCount;
    /// <summary>The game time this step simulates, seconds.</summary>
    public float Dt;
    /// <summary>1/kg; 0 for a kinematic body or one whose translation is frozen.</summary>
    public float InverseMass;
    /// <summary>The body's gravity scale (its value from before the custom integrator was switched on).</summary>
    public float GravityFactor;
    /// <summary>Centre of mass, centimetres.</summary>
    public Float3 Position;
    public Quaternion Rotation;
    /// <summary>cm/s. Assign to change the body's velocity.</summary>
    public Float3 LinearVelocity;
    /// <summary>Radians per second. Assign to change the body's spin.</summary>
    public Float3 AngularVelocity;
    /// <summary>World gravity plus the areas the body is in, cm/s^2, NOT scaled by <see cref="GravityFactor"/> (the total gravity).</summary>
    public Float3 TotalGravity;
    /// <summary>The world-space inverse inertia tensor, row by row, engine axes, 1/(kg cm^2). Zero for a kinematic body.</summary>
    public Float3 InverseInertiaRow0, InverseInertiaRow1, InverseInertiaRow2;

    public readonly Body Body => new(BodyHandle);
}

/// <summary>The integrate-forces callback: called once per fixed step for an awake body, before the solver.</summary>
public delegate void IntegrateForcesHandler(ref DirectBodyState state);

public readonly partial struct Body
{
    // One native trampoline for every body: the body handle in the state picks the handler. Held static so the delegate is never collected.
    private static readonly Dictionary<int, IntegrateForcesHandler> s_integrateHandlers = new();
    private static readonly Native.aver_phys_integrate_forces_fn s_integrateTrampoline = Trampoline;

    private static void Trampoline(ref DirectBodyState state, long user)
    {
        if (s_integrateHandlers.TryGetValue(state.BodyHandle, out var handler)) handler(ref state);
    }

    /// <summary>Calls <paramref name="handler"/> each fixed step while this body is awake; null clears it.
    /// Dynamic or kinematic bodies. The handler may call other physics functions. Remove it before the body goes (a removed body stops
    /// being called, but the managed record stays until cleared here).</summary>
    public bool SetIntegrateForces(IntegrateForcesHandler? handler)
    {
        if (handler is null)
        {
            s_integrateHandlers.Remove(Handle);
            return Native.aver_phys_body_set_integrate_forces(Handle, null, 0) != 0;
        }
        if (Native.aver_phys_body_set_integrate_forces(Handle, s_integrateTrampoline, Handle) == 0) return false;
        s_integrateHandlers[Handle] = handler;
        return true;
    }

    /// <summary>Custom integrator: the engine stops applying gravity and damping to this body, so it moves only by what the
    /// <see cref="SetIntegrateForces"/> handler and collisions do. Turning it off puts the previous gravity factor and dampings back.</summary>
    public bool CustomIntegrator => Native.aver_phys_body_custom_integrator(Handle, out int on) != 0 && on != 0;
    public bool SetCustomIntegrator(bool on) => Native.aver_phys_body_set_custom_integrator(Handle, on ? 1 : 0) != 0;

    /// <summary>Constant linear / angular velocity (a conveyor): bodies touching this static or kinematic body are driven
    /// as if its surface moved at <paramref name="linear"/> (cm/s, world) and spun at <paramref name="angular"/> (rad/s, world axes, about the
    /// body's origin). The body itself does not move. Both zero removes it.</summary>
    public bool SetConstantVelocity(Float3 linear, Float3 angular) =>
        Native.aver_phys_body_set_constant_velocity(Handle, linear.ToArray(), angular.ToArray()) != 0;

    public (Float3 Linear, Float3 Angular) ConstantVelocity
    {
        get
        {
            var l = new float[3]; var a = new float[3];
            return Native.aver_phys_body_constant_velocity(Handle, l, a) != 0
                ? (new Float3(l[0], l[1], l[2]), new Float3(a[0], a[1], a[2]))
                : (Float3.Zero, Float3.Zero);
        }
    }

    /// <summary>Per-shape disable: the shape stops colliding without leaving the body (its index and user data stay; the body's
    /// mass is kept). <paramref name="index"/> is a child of a MUTABLE compound this body owns; -1 (or 0 on a body that is not a compound) is
    /// the whole shape.</summary>
    public bool SetShapeDisabled(int index, bool disabled = true) =>
        Native.aver_phys_body_set_shape_disabled(Handle, index, disabled ? 1 : 0) != 0;

    public bool IsShapeDisabled(int index = -1) => Native.aver_phys_body_shape_disabled(Handle, index, out int off) != 0 && off != 0;
}
