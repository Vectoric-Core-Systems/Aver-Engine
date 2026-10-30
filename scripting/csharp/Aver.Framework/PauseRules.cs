// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// PauseRules: resolving an actor's PauseMode through the scene tree, and whether it ticks.

namespace Aver.Framework;

// The process-mode rules, kept free of the scene so they test without a host.
internal static class PauseRules
{
    // The first mode that is not Inherit, walking from the actor up to the root; Pausable when every one inherits.
    internal static PauseMode Resolve(IEnumerable<PauseMode> selfToRoot)
    {
        foreach (PauseMode m in selfToRoot)
            if (m != PauseMode.Inherit) return m;
        return PauseMode.Pausable;
    }

    // Whether an actor with this resolved mode ticks in a running or a paused game.
    internal static bool Ticks(PauseMode resolved, bool paused) => resolved switch
    {
        PauseMode.Always => true,
        PauseMode.WhenPaused => paused,
        PauseMode.Disabled => false,
        _ => !paused,
    };
}
