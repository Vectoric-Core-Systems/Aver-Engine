using Aver.Framework;
using Aver.Scripting;

namespace Aver.Framework.SampleActor;

// The sample actor used by --spawn-test: it logs every lifecycle hook it receives.

/// <summary>The smallest actor that proves the C# actor lifecycle fires end to end.</summary>
[AverClass("BP_TestActor")]
public sealed class TestActor : AverActor
{
    private int _ticks;

    /// <summary>The class recipe: tick in PrePhysics. Run once at load, per class.</summary>
    public static void Configure(ClassBuilder b) => b.Ticks(TickGroup.PrePhysics);

    /// <summary>Logs that the instance spawned and bound.</summary>
    public override void OnBeginPlay(BeginReason reason) =>
        Log.Info($"[TestActor] OnBeginPlay reason={reason} entity={Self.Handle}");

    /// <summary>Counts and logs one tick.</summary>
    public override void OnTick(float dt)
    {
        ++_ticks;
        Log.Info($"[TestActor] OnTick #{_ticks} dt={dt:F4} entity={Self.Handle}");
    }

    /// <summary>Logs the end of the session and the tick count.</summary>
    public override void OnEndPlay(EndReason reason) =>
        Log.Info($"[TestActor] OnEndPlay reason={reason} after {_ticks} tick(s)");
}
