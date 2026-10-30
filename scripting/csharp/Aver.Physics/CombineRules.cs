// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The material combine rule (physics_surface_abi.h): what a contact applies when two materials meet. Jolt's rule is the
// default; the material rule (friction min or max when rough, bounce a clamped sum that an absorbent side subtracts
// from) is opt-in, per world, with ROUGH / ABSORBENT flags per body.

namespace Aver.Physics;

/// <summary>How two materials combine in a contact. Values are the header's AVER_PHYS_COMBINE_*.</summary>
public enum CombineMode
{
    /// <summary>Friction sqrt(f1 f2), restitution max(r1, r2): Jolt's own rules and the default.</summary>
    Jolt = 0,
    /// <summary>Friction min(f1, f2) (max if either side is <see cref="CombineFlags.Rough"/>), restitution clamp(r1 + r2, 0, 1)
    /// (a side marked <see cref="CombineFlags.Absorbent"/> subtracts instead of adding).</summary>
    Material = 1,
}

/// <summary>Per-body flags used by <see cref="CombineMode.Material"/> only. Values are the header's AVER_PHYS_BODY_COMBINE_*.</summary>
[System.Flags]
public enum CombineFlags
{
    /// <summary>Any pair with this body takes the larger friction.</summary>
    Rough = 1,
    /// <summary>This body subtracts its restitution from the pair's sum instead of adding it.</summary>
    Absorbent = 2,
}

public static partial class Physics
{
    /// <summary>The world's combine rule. Set it once after the world exists (the project key PHYSICS.COMBINE does); a new
    /// world starts on <see cref="CombineMode.Jolt"/>. Setting is refused (false) with no world or while a step runs.</summary>
    public static CombineMode CombineRule
    {
        get { int m = Native.aver_phys_combine_mode(); return m == 1 ? CombineMode.Material : CombineMode.Jolt; }
    }

    /// <summary>Selects the combine rule; false for no world, an unknown mode, or while the world is stepping.</summary>
    public static bool SetCombineRule(CombineMode mode) => Native.aver_phys_set_combine_mode((int)mode) != 0;
}

public readonly partial struct Body
{
    /// <summary>The body's material flags (rough, absorbent); none (0) for a dead handle.
    /// Stored in every mode, used only by <see cref="CombineMode.Material"/>.</summary>
    public CombineFlags CombineFlags
    {
        get { int f = Native.aver_phys_body_combine_flags(Handle); return f < 0 ? default : (Aver.Physics.CombineFlags)f; }
    }

    /// <summary>Sets the body's rough / absorbent flags. False for a dead handle or while the world is stepping.</summary>
    public bool SetCombineFlags(CombineFlags flags) => Native.aver_phys_body_set_combine_flags(Handle, (int)flags) != 0;
}
