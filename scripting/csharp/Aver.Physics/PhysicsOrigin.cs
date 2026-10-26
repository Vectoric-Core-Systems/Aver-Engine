// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The physics half of the floating origin, read-only (physics_origin_abi.h; docs/plans/wave11/LARGE_WORLD.md 2.3). The HOST shifts
// the world: when the camera has flown a region away from the origin it moves the scene and calls the native shift in the same
// slot, so every position this API returns keeps meaning what it meant. Scripts never shift anything; they read these numbers to
// know that a rebase happened and to compare physics logs, because a physics state hash is frame-relative.

namespace Aver.Physics;

/// <summary>What the last origin shift did (aver_phys_origin_stats).</summary>
public readonly struct PhysicsOriginShift
{
    /// <summary>Wall time of the shift in milliseconds.</summary>
    public readonly float Milliseconds;
    /// <summary>Bodies moved (static ones included).</summary>
    public readonly int Bodies;
    public readonly int Characters;
    /// <summary>Anchor bodies of joints to the world that were moved.</summary>
    public readonly int Anchors;
    /// <summary>Pulleys that were rebuilt around their moved fixed points.</summary>
    public readonly int PulleysRebuilt;
    /// <summary>How many of <see cref="Bodies"/> were static.</summary>
    public readonly int StaticsMoved;

    internal PhysicsOriginShift(float ms, int bodies, int characters, int anchors, int pulleys, int statics)
    {
        Milliseconds = ms; Bodies = bodies; Characters = characters; Anchors = anchors; PulleysRebuilt = pulleys; StaticsMoved = statics;
    }
}

/// <summary>The physics world's floating origin: how often it has shifted and how far.</summary>
public static class PhysicsOrigin
{
    /// <summary>Shifts so far in this world: 0 when it never shifted, and after a rebuild.</summary>
    public static uint Generation => Native.aver_phys_origin_generation();

    /// <summary>The sum of every shift since the world was created, in centimetres: this world's origin in the engine's absolute
    /// frame (X, Y, Z; Z is never shifted). Zeros with no world.</summary>
    public static (double X, double Y, double Z) Total
    {
        get
        {
            var v = new double[3];
            Native.aver_phys_origin_total(v);
            return (v[0], v[1], v[2]);
        }
    }

    /// <summary>Wall time of the last shift in milliseconds; 0 when none has run.</summary>
    public static float LastShiftMs => LastShift.Milliseconds;

    /// <summary>The last shift's counts and time; all zero when none has run in this world.</summary>
    public static PhysicsOriginShift LastShift
    {
        get
        {
            Native.aver_phys_origin_stats(out float ms, out int bodies, out int characters, out int anchors, out int pulleys, out int statics);
            return new PhysicsOriginShift(ms, bodies, characters, anchors, pulleys, statics);
        }
    }
}
