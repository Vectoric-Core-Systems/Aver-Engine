using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;   // Log

namespace Sample.Game;

/// <summary>
/// A drivable car. This file is the HAND-WRITTEN half — the behaviour the editor never touches. The models
/// inside the car (body and four wheels) and their transforms live in the editor-owned generated half,
/// <c>Car.Designer.cs</c>; this half only READS them, through the strongly-typed model properties the
/// generated file declares.
/// </summary>
/// <remarks>
/// The class is <c>partial</c> so the two halves compile into one type. Everything gizmo-editable is over
/// there; everything a person reasons about — the steering formula, the speed clamp, the possession log —
/// is here, and is correctly not draggable.
/// </remarks>
[AverClass("BP_Car", Parent = "Pawn")]
public sealed partial class Car : AverPawn
{
    // [Editable] => native storage: shows in Details, serialises, survives Play/Stop and hot reload.
    [Editable] public float MaxSpeedCmPerSec = 1500.0f;             // centimetres per second
    [Editable(Min = 0.0f, Max = 90.0f)] public float MaxSteerDegrees = 35.0f;

    // Plain field: ordinary managed state. It restarts from this initialiser after a hot reload, which is
    // why OnRebound below re-derives nothing for it — zero is the correct post-reload steer.
    private float _steer;

    /// <summary>The class recipe: run once at load, per class, never per instance. Not gizmo-editable.</summary>
    public static void Configure(ClassBuilder b)
    {
        b.Ticks(TickGroup.PrePhysics);
    }

    /// <summary>Set the steer input in [-1, 1]. Called by whatever drives this pawn.</summary>
    public void SetSteer(float normalised) => _steer = Math.Clamp(normalised, -1.0f, 1.0f);

    public override void OnBeginPlay(BeginReason reason)
    {
        if (reason != BeginReason.Reload) _steer = 0.0f;
    }

    public override void OnTick(float dt)
    {
        // Drive along the car's own forward axis (+X rotated by the body's rotation).
        Self.Translate(Self.Forward * (MaxSpeedCmPerSec * dt));

        // Steer the two front wheels — models placed in the generated half, driven here.
        Quat steer = new Rot(_steer * MaxSteerDegrees, 0.0f, 0.0f).ToQuat();
        WheelFL.Entity.SetLocalRotation(steer);
        WheelFR.Entity.SetLocalRotation(steer);
    }

    public override void OnPossessed(Entity controller) => Log.Info($"{controller.Name} took the wheel of {Self.Name}");
}
