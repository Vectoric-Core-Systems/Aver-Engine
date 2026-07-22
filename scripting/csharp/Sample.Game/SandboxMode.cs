using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;   // Log

namespace Sample.Game;

/// <summary>
/// A sandbox game mode: it names the car as the default pawn and the stock controller to drive it, and
/// spawns a couple of cars when play begins. The pawn and controller are named by STRING on the attribute,
/// so this mode takes no compile-time reference to <see cref="Car"/> at the class-wiring level — the
/// <c>Spawn&lt;Car&gt;</c> below is a separate, optional convenience that does.
/// </summary>
[AverGameMode("GM_Sandbox", DefaultPawnClass = "BP_Car", PlayerControllerClass = "PlayerController")]
public sealed class SandboxMode : AverGameMode
{
    public override void OnBeginPlay(BeginReason reason)
    {
        if (reason == BeginReason.Reload) return;   // spawns already exist; a reload must not double them
        Spawn(ActorClass.Find("BP_Car"), new Vec3(0, 0, 0));   // by name (late-bound)
        Spawn<Car>(new Vec3(400, 0, 0));                       // by type (the type carries its class name)
    }

    public override void OnPostLogin(Entity controller) => Log.Info($"player joined: {controller.Name}");
}
