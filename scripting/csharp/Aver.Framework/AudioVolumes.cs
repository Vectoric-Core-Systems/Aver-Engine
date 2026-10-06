// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Placeable audio volumes: an actor class a level can place by name, so a reverb zone needs no code.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>A box reverb zone. Place it, size it with the entity's scale or the extents below, and the
/// listener hears this reverb inside it and blends in over <see cref="BlendDistanceCm"/> outside.
/// Becomes a <c>CReverbZone</c> when play begins.</summary>
[AverClass("AN_ReverbVolume")]
public sealed class AverReverbVolume : AverActor
{
    /// <summary>Half the box's size along its local X, centimetres.</summary>
    [Editable(Min = 1.0f)] public float HalfExtentX = 500f;
    /// <summary>Half the box's size along its local Y, centimetres.</summary>
    [Editable(Min = 1.0f)] public float HalfExtentY = 500f;
    /// <summary>Half the box's size along its local Z, centimetres.</summary>
    [Editable(Min = 1.0f)] public float HalfExtentZ = 300f;
    /// <summary>Distance outside the surface over which the reverb fades to nothing.</summary>
    [Editable(Min = 0.0f)] public float BlendDistanceCm = 200f;
    /// <summary>Reverb return level, 0 to 1.</summary>
    [Editable(Min = 0.0f, Max = 1.0f)] public float Wet = 0.35f;
    /// <summary>Time for the tail to fall 60 dB, seconds.</summary>
    [Editable(Min = 0.1f, Max = 20.0f)] public float DecaySeconds = 1.8f;
    /// <summary>0 bright to 1 dark.</summary>
    [Editable(Min = 0.0f, Max = 1.0f)] public float Damping = 0.4f;
    /// <summary>Higher wins where zones overlap.</summary>
    [Editable] public float Priority = 0f;

    /// <summary>Attaches the zone component with the values above.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        Self.SetReverbZone(new Vec3(HalfExtentX, HalfExtentY, HalfExtentZ), BlendDistanceCm, Wet,
                           DecaySeconds, Damping, (int)Priority);
    }
}
