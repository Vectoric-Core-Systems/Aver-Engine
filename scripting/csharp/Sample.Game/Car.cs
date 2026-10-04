// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;

namespace Sample.Game;

/// <summary>A drivable car. This is the hand-written half; the models and their transforms live in
/// the editor-owned <c>Car.Designer.cs</c>, which this half only reads.</summary>
[AverClass("AN_Car", Parent = "Pawn")]
public sealed partial class Car : AverPawn
{
    [Editable] public float MaxSpeedCmPerSec = 1500.0f;
    [Editable(Min = 0.0f, Max = 90.0f)] public float MaxSteerDegrees = 35.0f;

    private float _steer;

    /// <summary>The class recipe: run once at load, per class, never per instance.</summary>
    public static void Configure(ClassBuilder b)
    {
        b.Ticks(TickGroup.PrePhysics);
    }

    /// <summary>Set the steer input in [-1, 1]. Called by whatever drives this pawn.</summary>
    public void SetSteer(float normalised) => _steer = Math.Clamp(normalised, -1.0f, 1.0f);

    /// <summary>Centres the steering, except on a hot reload.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        if (reason != BeginReason.Reload) _steer = 0.0f;
    }

    /// <summary>Drives the car along its forward axis and turns the two front wheels.</summary>
    public override void OnTick(float dt)
    {
        Self.Translate(Self.Forward * (MaxSpeedCmPerSec * dt));

        Quat steer = new Rot(_steer * MaxSteerDegrees, 0.0f, 0.0f).ToQuat();
        WheelFL.Entity.SetLocalRotation(steer);
        WheelFR.Entity.SetLocalRotation(steer);
    }

    /// <summary>Logs which controller took the wheel.</summary>
    public override void OnPossessed(Entity controller) => Log.Info($"{controller.Name} took the wheel of {Self.Name}");
}
