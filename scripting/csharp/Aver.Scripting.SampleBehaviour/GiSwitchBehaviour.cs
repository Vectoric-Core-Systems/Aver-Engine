// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A sample behaviour that turns global illumination on from managed code.
using Aver.Scripting;

namespace Aver.Scripting.SampleBehaviour;

/// <summary>Switches global illumination on a few updates in, through the <c>aver_voxi_*</c> ABI.</summary>
public sealed class GiSwitchBehaviour : AverBehaviour
{
    private const int SwitchAtUpdate = 5;

    private int _updates;
    private bool _done;

    /// <summary>Logs the GI setting the editor started with.</summary>
    public override void OnStart()
    {
        Log.Info($"[GiSwitch] global illumination is {Voxi.GlobalIllumination} at startup "
                 + $"(status: {Voxi.StatusTextOf(VoxiFeature.GlobalIllumination)})");
    }

    /// <summary>Counts updates and, once, sets global illumination to High if the device allows it.</summary>
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
        Log.Info($"[GiSwitch] set global illumination to {Voxi.GlobalIllumination} from managed code "
                 + $"at update {_updates} - the viewport changes on the next frame");
    }

    /// <summary>Logs the setting it is leaving behind.</summary>
    public override void OnShutdown()
    {
        Log.Info($"[GiSwitch] leaving global illumination at {Voxi.GlobalIllumination}");
    }
}
