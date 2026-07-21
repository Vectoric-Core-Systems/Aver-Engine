using Aver.Scripting;

namespace Aver.Scripting.SampleBehaviour;

/// <summary>
/// The smallest thing that proves a script can change what the editor DRAWS.
/// </summary>
/// <remarks>
/// <para>
/// <see cref="HelloBehaviour"/> proves the hosting works; this proves the hosting is worth having.
/// It turns global illumination on a few updates in, through the same <c>aver_voxi_*</c> C ABI the
/// editor's own Project Settings panel drives — and because the CLR is hosted <b>in-process</b>,
/// the P/Invoke resolves to the <c>Aver.Render.Voxi.dll</c> the editor has already loaded. Same
/// module, same settings singleton, same frame. A standalone C# process would get its own copy and
/// change nothing anybody could see; that difference is the whole reason the host exists.
/// </para>
/// <para>
/// The change is measurable and not merely visible. The engine's oracle reads one pixel, and the
/// viewport-centre probe reads <c>raw(90,93,108)</c> with GI off and <c>raw(104,91,104)</c> with GI
/// on — so <c>Sandbox.exe --frames 40 --scripts SampleScripts</c> printing the second of those is
/// the proof, at the raw 8-bit code, that managed code drove the renderer.
/// </para>
/// <para>
/// It asks before it sets. <see cref="Voxi.IsAvailable"/> reports the real device capability, and
/// assigning a feature the GPU or the renderer cannot do leaves the value at Off rather than
/// pretending — so a machine without the hardware for this logs that it declined instead of
/// quietly rendering the same image and claiming otherwise.
/// </para>
/// </remarks>
public sealed class GiSwitchBehaviour : AverBehaviour
{
    // Not frame 0: the point is that this happens to a running editor, from the frame loop, rather
    // than being indistinguishable from a startup default someone set in C++.
    private const int SwitchAtUpdate = 5;

    private int _updates;
    private bool _done;

    public override void OnStart()
    {
        Log.Info($"[GiSwitch] global illumination is {Voxi.GlobalIllumination} at startup "
                 + $"(status: {Voxi.StatusTextOf(VoxiFeature.GlobalIllumination)})");
    }

    public override void OnUpdate(float dt)
    {
        if (_done || ++_updates < SwitchAtUpdate)
            return;
        _done = true;

        if (!Voxi.IsAvailable(VoxiFeature.GlobalIllumination))
        {
            Log.Warn($"[GiSwitch] global illumination is not available here: "
                     + $"{Voxi.StatusTextOf(VoxiFeature.GlobalIllumination)}");
            return;
        }

        Voxi.GlobalIllumination = VoxiQuality.High;
        // Read BACK rather than reporting what was assigned. The setter is honest about refusing,
        // so the only line worth printing is the one the engine agrees with.
        Log.Info($"[GiSwitch] set global illumination to {Voxi.GlobalIllumination} from managed code "
                 + $"at update {_updates} - the viewport changes on the next frame");
    }

    public override void OnShutdown()
    {
        Log.Info($"[GiSwitch] leaving global illumination at {Voxi.GlobalIllumination}");
    }
}
