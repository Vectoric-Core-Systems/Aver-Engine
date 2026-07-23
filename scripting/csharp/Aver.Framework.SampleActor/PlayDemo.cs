using Aver.Framework;
using Aver.Scene;      // Vec3
using Aver.Scripting;  // Log

namespace Aver.Framework.SampleActor;

// The smallest project that proves the PLAY lifecycle end to end: a GameInstance, a GameMode that names
// a pawn and a controller, and those two classes. Pressing Play (or running --play-test) spawns the
// GameInstance, then the GameMode, then the GameMode's controller and pawn, and possesses the pawn — so
// every OnBeginPlay below fires, the pawn ticks, and Stop runs every OnEndPlay with reason=Stop. The pawn
// carries the built-in sphere, so a play session is also the first thing to put gameplay geometry on
// screen through the scene-render pass. Nothing here is a mock.

/// <summary>Process-lifetime state. Spawned first, torn down last; here it only announces its lifecycle.</summary>
[AverClass("BP_GameInstance")]
public sealed class DemoGameInstance : AverGameInstance
{
    public override void OnBeginPlay(BeginReason reason) => Log.Info($"[DemoGameInstance] OnBeginPlay reason={reason}");
    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoGameInstance] OnEndPlay reason={reason}");
}

/// <summary>The rules for a play session. Names its pawn and controller by string; the framework resolves
/// and spawns them at begin-play.</summary>
[AverGameMode("BP_GameMode", DefaultPawnClass = "BP_Pawn", PlayerControllerClass = "BP_Controller")]
public sealed class DemoGameMode : AverGameMode
{
    public override void OnBeginPlay(BeginReason reason)
    {
        Log.Info($"[DemoGameMode] OnBeginPlay reason={reason} entity={Self.Handle}");
        // A GameMode that spawns gameplay content when play begins — the same shape as GM_Sandbox spawning
        // its cars. Exercises the spawn overloads: a rotated sphere, then a second sphere ATTACHED to it as
        // a child. Both are NOT session roots, so they are exactly what Stop must still tear down.
        Sphere? first = Spawn<Sphere>(new Vec3(0f, -3f, 1f), new Rot(45f, 0f, 0f));
        if (first is not null)
        {
            Sphere? child = SpawnAttached<Sphere>(first.Self, new Vec3(0f, 0f, 2f));
            Log.Info($"[DemoGameMode] spawned a rotated sphere + a child; first now has {first.Self.ChildCount} child(ren), child parent='{child?.Self.Parent.Name}'");
        }
    }

    /// <summary>Fires once the player's controller has entered under this mode — the classic place to spawn
    /// and possess a pawn (here the framework already did, from the class's DefaultPawnClass wiring).</summary>
    public override void OnPostLogin(Entity controller) =>
        Log.Info($"[DemoGameMode] OnPostLogin: controller '{controller.Name}' driving '{controller.As<DemoController>()?.Possessed.Name}', state={Game.State}");

    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoGameMode] OnEndPlay reason={reason}");
}

/// <summary>The player's controller. Possesses the pawn (the framework does the possess wiring here).</summary>
[AverClass("BP_Controller")]
public sealed class DemoController : AverPlayerController
{
    public override void OnBeginPlay(BeginReason reason)
    {
        Log.Info($"[DemoController] OnBeginPlay reason={reason} entity={Self.Handle}");
        // A controller carries no mesh from its class, so this exercises SetMesh on a BARE actor: the
        // added renderer must come up visible (a Vec3 accessor on the rotation Quat must stay safe too).
        Self.SetMesh("Meshes/sphere.ocmesh");
        Vec3 rotAsVec = Self.GetVec3("CLocal.rotation");   // a Quat field: the arity guard returns Zero, not corruption
        Log.Info($"[DemoController] SetMesh on a bare actor -> visible={Self.Visible}; GetVec3(rotation Quat)={rotAsVec}");
    }

    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoController] OnEndPlay reason={reason}");
}

/// <summary>The possessed pawn. Carries the built-in sphere and ticks, so a play session is visible and
/// its per-frame hook is observable.</summary>
[AverClass("BP_Pawn")]
public sealed class DemoPawn : AverPawn
{
    private int _ticks;

    /// <summary>The class recipe: the built-in sphere mesh, and a tick in PrePhysics.</summary>
    public static void Configure(ClassBuilder b)
    {
        b.Mesh("Meshes/sphere.ocmesh");
        b.Ticks(TickGroup.PrePhysics);
    }

    public override void OnBeginPlay(BeginReason reason) => Log.Info($"[DemoPawn] OnBeginPlay reason={reason} entity={Self.Handle}");

    public override void OnPossessed(Entity controller) => Log.Info($"[DemoPawn] OnPossessed by '{controller.Name}'");

    public override void OnUnpossessed() => Log.Info("[DemoPawn] OnUnpossessed");

    public override void OnTick(float dt)
    {
        if (_ticks == 0)
        {
            // Exercise the gameplay API from a plain pawn, holding no cached references: the session facade
            // (Game), the typed singletons, possession (ControllerAs), and a world transform. By the first
            // tick the controller has possessed this pawn, so the lookup resolves.
            DemoGameMode? mode = Game.ModeAs<DemoGameMode>();
            DemoController? ctrl = ControllerAs<DemoController>();
            Log.Info($"[DemoPawn] play API: state={Game.State}, mode='{mode?.Self.Name}', " +
                     $"instance='{Game.Instance.Name}', controller='{ctrl?.Self.Name}', worldPos={Self.WorldPosition}");

            // Object-model API: tag myself, query the world, and read a component field. Tags auto-add the
            // component; ActorsOf<T> and WithTag scan the live world; Visible reads CMeshRenderer.flags.
            const uint Team = 0x1;
            Self.AddTag(Team);
            int spheres = 0; foreach (var _ in Game.ActorsOf<Sphere>()) spheres++;
            int tagged = 0; foreach (var _ in Game.WithTag(Team)) tagged++;
            Log.Info($"[DemoPawn] object API: visible={Self.Visible}, tag set={Self.HasTag(Team)}, " +
                     $"live Spheres={spheres}, entities tagged Team={tagged}");
        }
        if (++_ticks <= 3) Log.Info($"[DemoPawn] OnTick #{_ticks} dt={dt:F4}");
    }

    public override void OnEndPlay(EndReason reason) => Log.Info($"[DemoPawn] OnEndPlay reason={reason} after {_ticks} tick(s)");
}
