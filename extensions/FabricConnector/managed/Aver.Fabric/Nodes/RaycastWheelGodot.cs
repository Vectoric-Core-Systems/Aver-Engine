// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Godot's VehicleBody3D / VehicleWheel3D values (metres, left-positive steering, brake impulses, Godot's axes) converted to the raycast vehicle's
// numbers (Aver.Physics.RaycastWheelSettings: centimetres, kg*cm/s^2, Aver's axes). Moved here from Aver.Physics: it is Fabric-only.

using System;
using Aver.Physics;

namespace Aver.Fabric;

public static class RaycastWheelGodot
{
    /// <summary>Godot's axes (+X right, +Y up, -Z forward) to Aver's (+Y right, +Z up, +X forward) for a direction or a position.</summary>
    public static Float3 FromGodotAxes(Float3 g) => new(-g.Z, g.X, g.Y);

    /// <summary>The settings of a Godot VehicleWheel3D. Vectors are chassis-local in Godot's axes; lengths in metres; <paramref name="maxForceN"/> in newtons;
    /// stiffness and damping as the node holds them. No Godot default is built in: the caller passes what the scene says. If the Godot version in use scales
    /// damping by a ratio, run it through <see cref="RaycastWheelSettings.DampingFromRatio"/> first.</summary>
    public static RaycastWheelSettings FromGodot(Float3 connectionPoint, Float3 suspensionDirection, Float3 axle, float restLength, float suspensionTravel,
        float stiffness, float dampingCompression, float dampingRelaxation, float maxForceN, float radius, float frictionSlip, float rollInfluence,
        bool useAsTraction, bool useAsSteering, uint collisionMask = 0xFFFFFFFFu)
    {
        const float cm = 100f;
        var c = FromGodotAxes(connectionPoint);
        return new RaycastWheelSettings
        {
            ConnectionPoint = new Float3(c.X * cm, c.Y * cm, c.Z * cm),
            SuspensionDirection = FromGodotAxes(suspensionDirection),
            Axle = FromGodotAxes(axle),
            RestLength = restLength * cm,
            MaxTravel = suspensionTravel * cm,
            Stiffness = stiffness,
            DampingCompression = dampingCompression,
            DampingRelaxation = dampingRelaxation,
            MaxForce = maxForceN * cm,
            Radius = radius * cm,
            FrictionSlip = frictionSlip,
            RollInfluence = rollInfluence,
            UseAsTraction = useAsTraction,
            UseAsSteering = useAsSteering,
            CollisionMask = collisionMask,
        };
    }

    /// <summary>Godot's engine_force (newtons) to the ABI's kg*cm/s^2.</summary>
    public static float EngineForceFromGodot(float newtons) => newtons * 100f;
    /// <summary>Godot's brake is an IMPULSE applied every physics step; the ABI takes a force: impulse x ticks per second x 100 (cm).</summary>
    public static float BrakeFromGodot(float brakeImpulsePerStep, float physicsTicksPerSecond) => brakeImpulsePerStep * physicsTicksPerSecond * 100f;
    /// <summary>Godot's steering is positive to the LEFT; the ABI's is positive toward the axle side (right).</summary>
    public static float SteeringFromGodot(float radians) => -radians;
}
