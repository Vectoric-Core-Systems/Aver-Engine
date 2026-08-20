// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework.SampleActor;

// The sample actor used by --spawn-test: it logs every lifecycle hook it receives.

/// <summary>The smallest actor that proves the C# actor lifecycle fires end to end.</summary>
[AverClass("AN_TestActor")]
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

    /// <summary>Counts every "OnBeat" it receives and writes the running total into its own X.
    ///
    /// An ANIMATION NOTIFY arrives here exactly as a graph's FireEvent node does -- the same hook,
    /// because both mean "this happened, at this entity, now". Writing the count into the transform
    /// rather than only logging it is what lets a native test READ the result: a log line proves a
    /// method ran, and this codebase has shipped more than one hook that ran and changed nothing.
    ///
    /// Returning false for an unrecognised name is the honest answer, and it is what the caller
    /// reports as its own success -- a FireEvent node aimed at this actor reads it.</summary>
    public override bool OnEvent(string name)
    {
        if (name != "OnBeat") return false;
        ++_beats;
        Log.Info($"[TestActor] OnEvent '{name}' #{_beats} entity={Self.Handle}");
        Vec3 p = Self.LocalPosition;
        p.X = _beats;
        Self.SetLocalPosition(p);
        return true;
    }

    private int _beats;
}
