// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Named post-process layers a script drives: saturation fades, flashes, grades. Design: docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.4.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>
/// Script-driven post-process layers, blended over the level's own post volumes.
/// </summary>
/// <remarks>
/// <para>
/// A LAYER is a named set of POSTVOLUME key values (<c>saturation</c>, <c>bloom</c>, <c>lift</c>, ... the keys of the
/// post-volume Details panel, lower-case) applied as one infinite volume. Each frame the host applies the level's
/// volumes, then the layers in the order they were created, then the player's accessibility choices, which always win.
/// A script that never touches this type renders exactly as before.
/// </para>
/// <para>
/// A layer that is given values and never a weight fades in by itself over 0.15 s. Give it a weight with
/// <see cref="SetLayerWeight"/> to fade it in or out over a time you choose, and <see cref="ClearLayer"/> to remove it.
/// Every call returns false instead of throwing: an unknown key, a non-finite value, a full table (16 layers, 32 volume
/// scales) or no host.
/// </para>
/// <para>Layer and volume names are case-sensitive. Passing no layer name means the layer called
/// <see cref="DefaultLayer"/>.</para>
/// </remarks>
public static class PostFx
{
    /// <summary>The layer a call without a name uses.</summary>
    public const string DefaultLayer = "script";

    private static long LayerId(string? layer) => Assets.ObjectIdOf(string.IsNullOrEmpty(layer) ? DefaultLayer : layer);

    /// <summary>Sets one post value on a layer (creating the layer on first use). One-value keys read
    /// <paramref name="v0"/>; the three-value keys (<c>lift</c>, <c>gamma</c>, <c>gain</c>) read all three. On/off keys
    /// switch at 0.5. A key the post-volume table does not have is refused.</summary>
    public static bool SetValue(string? layer, string key, float v0, float v1 = 0f, float v2 = 0f)
    {
        if (string.IsNullOrWhiteSpace(key)) return false;
        return FwWorldFx.aver_fw_post_set(LayerId(layer), Assets.ObjectIdOf(key.Trim().ToLowerInvariant()), v0, v1, v2) != 0;
    }

    /// <summary>Sets one post value on the <see cref="DefaultLayer"/> layer.</summary>
    public static bool SetValue(string key, float v0) => SetValue(null, key, v0);

    /// <summary>Fades a layer to <paramref name="weight"/> (0 to 1) over <paramref name="blendSeconds"/> from wherever it
    /// is now; 0 seconds applies at once. Creates the layer if it does not exist, with no values yet.</summary>
    public static bool SetLayerWeight(string? layer, float weight, float blendSeconds = 0f) =>
        FwWorldFx.aver_fw_post_set_weight(LayerId(layer), weight, blendSeconds) != 0;

    /// <summary>Removes a layer at once (<see cref="SetLayerWeight"/> to 0 first to fade it out). Null clears the
    /// <see cref="DefaultLayer"/> layer. False when there was no such layer.</summary>
    public static bool ClearLayer(string? layer = null) => FwWorldFx.aver_fw_post_clear(LayerId(layer)) != 0;

    /// <summary>Removes every layer a script made.</summary>
    public static void ClearAll() => FwWorldFx.aver_fw_post_clear(0);

    /// <summary>Scales the weight of one of the LEVEL's post volumes, by its authored name, from its current scale
    /// (1 at first) over <paramref name="blendSeconds"/>: 0 mutes it, 1 restores it. The volume need not exist.</summary>
    public static bool SetVolumeWeight(string volume, float weight, float blendSeconds = 0f) =>
        !string.IsNullOrEmpty(volume) && weight >= 0f && FwWorldFx.aver_fw_post_volume_weight(Assets.ObjectIdOf(volume), weight, blendSeconds) != 0;

    /// <summary>Removes the scale on a level volume, so it applies at its authored weight again.</summary>
    public static bool ClearVolumeWeight(string volume) =>
        !string.IsNullOrEmpty(volume) && FwWorldFx.aver_fw_post_volume_weight(Assets.ObjectIdOf(volume), -1f, 0f) != 0;
}
