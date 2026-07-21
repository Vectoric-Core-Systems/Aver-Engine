using Aver.Scripting;

namespace Aver.Scripting.SampleBehaviour;

/// <summary>
/// The smallest thing that proves the boundary works end to end.
/// </summary>
/// <remarks>
/// Every line this writes comes from MANAGED code, travels through the host's log callback and
/// comes out of the engine's own log — so seeing it in the editor's output is proof that the CLR
/// is hosted in-process, that the collectible load context resolved <c>AverBehaviour</c> to the
/// same type the bridge knows, and that the frame loop is driving <c>OnUpdate</c>. Nothing here
/// is a mock.
///
/// It is staged to <c>bin/SampleScripts/</c>, not <c>bin/Scripts/</c>, so a normal editor run does
/// not load it. Run <c>Sandbox.exe --scripts SampleScripts</c> to see it.
/// </remarks>
public sealed class HelloBehaviour : AverBehaviour
{
    private int _frames;
    private float _elapsed;

    public override void OnStart()
    {
        Log.Info($"[HelloBehaviour] OnStart from managed code - hosted in-process on {Environment.Version}");
    }

    public override void OnUpdate(float dt)
    {
        ++_frames;
        _elapsed += dt;
        // Once, a few frames in, rather than every frame: the point is to show the loop is driving
        // this, and a per-frame line would drown the log it is being printed into.
        if (_frames == 10)
            Log.Info($"[HelloBehaviour] OnUpdate has run {_frames} times ({_elapsed:F3}s of frame time)");
    }

    public override void OnShutdown()
    {
        Log.Info($"[HelloBehaviour] OnShutdown after {_frames} update(s)");
    }
}
