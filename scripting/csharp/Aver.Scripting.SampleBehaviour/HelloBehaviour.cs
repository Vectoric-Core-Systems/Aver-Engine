// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The smallest sample behaviour: it logs from managed code through the host's log callback.
using Aver.Scripting;

namespace Aver.Scripting.SampleBehaviour;

/// <summary>Logs from managed code at start, once a few updates in, and at shutdown.</summary>
/// <remarks>Staged to <c>bin/SampleScripts/</c>; run <c>Sandbox.exe --scripts SampleScripts</c> to load it.</remarks>
public sealed class HelloBehaviour : AverBehaviour
{
    private int _frames;
    private float _elapsed;

    /// <summary>Logs the runtime version it is hosted on.</summary>
    public override void OnStart()
    {
        Log.Info($"[HelloBehaviour] OnStart from managed code - hosted in-process on {Environment.Version}");
    }

    /// <summary>Counts frames and logs once on the tenth.</summary>
    public override void OnUpdate(float dt)
    {
        ++_frames;
        _elapsed += dt;
        if (_frames == 10)
            Log.Info($"[HelloBehaviour] OnUpdate has run {_frames} times ({_elapsed:F3}s of frame time)");
    }

    /// <summary>Logs how many updates ran.</summary>
    public override void OnShutdown()
    {
        Log.Info($"[HelloBehaviour] OnShutdown after {_frames} update(s)");
    }
}
