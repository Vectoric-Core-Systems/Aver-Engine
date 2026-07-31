using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;

namespace Sample.Game;

/// <summary>A sandbox game mode: the car is the default pawn, the stock controller drives it.</summary>
[AverGameMode("GM_Sandbox", DefaultPawnClass = "BP_Car", PlayerControllerClass = "PlayerController")]
public sealed class SandboxMode : AverGameMode
{
    /// <summary>Spawns two cars, one by class name and one by type. Does nothing on a reload.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        if (reason == BeginReason.Reload) return;
        Spawn(ActorClass.Find("BP_Car"), new Vec3(0, 0, 0));
        Spawn<Car>(new Vec3(400, 0, 0));
    }

    /// <summary>Logs the player joining.</summary>
    public override void OnPostLogin(Entity controller) => Log.Info($"player joined: {controller.Name}");
}
