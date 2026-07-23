using Aver.Framework;
using Aver.Scripting;   // Log

namespace Aver.Framework.SampleActor;

/// <summary>
/// The smallest actor that proves the C# actor lifecycle fires end to end.
/// </summary>
/// <remarks>
/// Every line it writes comes from MANAGED code, travels through the host's log callback and comes out of
/// the engine's own log — so seeing these after a <c>--spawn-test BP_TestActor</c> is proof that: the
/// class was declared at load, the managed dispatch is installed, the native spawn edge ran
/// bind -> build_models -> begin_play so an instance exists and <c>OnBeginPlay</c> ran, and
/// <c>aver_fw_tick</c> from the frame loop is reaching <c>OnTick</c> once per frame in the class's group.
/// Nothing here is a mock.
/// </remarks>
[AverClass("BP_TestActor")]
public sealed class TestActor : AverActor
{
    private int _ticks;

    /// <summary>The class recipe: tick in PrePhysics. Run once at load, per class.</summary>
    public static void Configure(ClassBuilder b) => b.Ticks(TickGroup.PrePhysics);

    public override void OnBeginPlay(BeginReason reason) =>
        Log.Info($"[TestActor] OnBeginPlay reason={reason} entity={Self.Handle}");

    public override void OnTick(float dt)
    {
        ++_ticks;
        Log.Info($"[TestActor] OnTick #{_ticks} dt={dt:F4} entity={Self.Handle}");
    }

    public override void OnEndPlay(EndReason reason) =>
        Log.Info($"[TestActor] OnEndPlay reason={reason} after {_ticks} tick(s)");
}
