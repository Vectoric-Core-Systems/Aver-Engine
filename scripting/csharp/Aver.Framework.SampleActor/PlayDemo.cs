// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework.SampleActor;

// The sample play session: a GameInstance, a GameMode, a controller and a pawn.
// Pressing Play (or --play-test) spawns all four and possesses the pawn.

/// <summary>Process-lifetime state. Spawned first, torn down last.</summary>
[AverClass("AN_GameInstance")]
public sealed class DemoGameInstance : AverGameInstance
{
    /// <summary>Logs the start of the session.</summary>
    public override void OnBeginPlay(BeginReason reason) => Log.Info($"[DemoGameInstance] OnBeginPlay reason={reason}");

    /// <summary>Logs the end of the session.</summary>
    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoGameInstance] OnEndPlay reason={reason}");
}

/// <summary>The rules for a play session. Names its pawn and controller by string.</summary>
[AverGameMode("AN_GameMode", DefaultPawnClass = "AN_Pawn", PlayerControllerClass = "AN_Controller")]
public sealed class DemoGameMode : AverGameMode
{
    /// <summary>Spawns a rotated sphere and a second sphere attached to it as a child.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        Log.Info($"[DemoGameMode] OnBeginPlay reason={reason} entity={Self.Handle}");
        Sphere? first = Spawn<Sphere>(new Vec3(0f, -3f, 1f), new Rot(45f, 0f, 0f));
        if (first is not null)
        {
            Sphere? child = SpawnAttached<Sphere>(first.Self, new Vec3(0f, 0f, 2f));
            Log.Info($"[DemoGameMode] spawned a rotated sphere + a child; first now has {first.Self.ChildCount} child(ren), child parent='{child?.Self.Parent.Name}'");
        }
    }

    /// <summary>Fires once the player's controller has entered under this mode.</summary>
    public override void OnPostLogin(Entity controller) =>
        Log.Info($"[DemoGameMode] OnPostLogin: controller '{controller.Name}' driving '{controller.As<DemoController>()?.Possessed.Name}', state={Game.State}");

    /// <summary>Logs the end of the session.</summary>
    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoGameMode] OnEndPlay reason={reason}");
}

/// <summary>The player's controller. Possesses the pawn.</summary>
[AverClass("AN_Controller")]
public sealed class DemoController : AverPlayerController
{
    /// <summary>Gives this bare actor a mesh and logs what came back.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        Log.Info($"[DemoController] OnBeginPlay reason={reason} entity={Self.Handle}");
        Self.SetMesh("Meshes/sphere.ocmesh");
        Vec3 rotAsVec = Self.GetVec3("CLocal.rotation");
        Log.Info($"[DemoController] SetMesh on a bare actor -> visible={Self.Visible}; GetVec3(rotation Quat)={rotAsVec}");
    }

    /// <summary>Logs the end of the session.</summary>
    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoController] OnEndPlay reason={reason}");
}

/// <summary>The possessed pawn: a character walked with WASD and turned with the mouse, V toggles
/// first/third person.</summary>
[AverClass("AN_Pawn")]
public sealed class DemoPawn : AverCharacter
{
    private int _ticks;

    /// <summary>The class recipe: the built-in sphere mesh, and a tick in PrePhysics.</summary>
    public static void Configure(ClassBuilder b)
    {
        b.Mesh("Meshes/sphere.ocmesh");
        b.Ticks(TickGroup.PrePhysics);
    }

    /// <summary>Drops the pawn from z=300 and logs the physics state.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        Log.Info($"[DemoPawn] OnBeginPlay reason={reason} entity={Self.Handle}");
        Teleport(new Vec3(0f, 0f, 300f));
        Log.Info($"[DemoPawn] physics: ready={Physics.Ready}, simulated={IsSimulated}, " +
                 $"fixedStep={Physics.FixedStep:F4}s, bodies={Physics.BodyCount}, dropped from z=300");
    }

    /// <summary>Logs which controller took this pawn.</summary>
    public override void OnPossessed(Entity controller) => Log.Info($"[DemoPawn] OnPossessed by '{controller.Name}'");

    /// <summary>Logs the loss of the controller.</summary>
    public override void OnUnpossessed() => Log.Info("[DemoPawn] OnUnpossessed");

    /// <summary>Walks the character from input, toggles the camera view, and traces the descent.</summary>
    public override void OnTick(float dt)
    {
        DriveWithInput(dt);
        if (Input.GetKeyDown(Key.V))
            CameraViewMode = CameraViewMode == CameraView.ThirdPerson ? CameraView.FirstPerson : CameraView.ThirdPerson;

        if (_ticks == 0)
        {
            DemoGameMode? mode = Game.ModeAs<DemoGameMode>();
            DemoController? ctrl = ControllerAs<DemoController>();
            Log.Info($"[DemoPawn] play API: state={Game.State}, mode='{mode?.Self.Name}', " +
                     $"instance='{Game.Instance.Name}', controller='{ctrl?.Self.Name}', worldPos={Self.WorldPosition}");

            const uint Team = 0x1;
            Self.AddTag(Team);
            int spheres = 0; foreach (var _ in Game.ActorsOf<Sphere>()) spheres++;
            int tagged = 0; foreach (var _ in Game.WithTag(Team)) tagged++;
            Log.Info($"[DemoPawn] object API: visible={Self.Visible}, tag set={Self.HasTag(Team)}, " +
                     $"live Spheres={spheres}, entities tagged Team={tagged}");
        }
        ++_ticks;
        if (_ticks <= 3 || _ticks % 15 == 0)
            Log.Info($"[DemoPawn] tick #{_ticks} dt={dt:F4} z={Self.LocalPosition.Z:F1} " +
                     $"vz={Velocity.Z:F1} grounded={IsGrounded}");
    }

    /// <summary>Logs the end of the session and the tick count.</summary>
    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoPawn] OnEndPlay reason={reason} after {_ticks} tick(s)");
}
