// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// PauseRulesTests: the process-mode rules -- resolving Inherit up the tree, and who ticks when paused.

using Aver.Framework;

static class PauseRulesTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("pause rules: the nearest explicit mode wins, Pausable when all inherit", Resolve);
        T.Run("pause rules: who ticks running and paused", Ticks);
    });

    private static void Resolve()
    {
        T.Check("no chain resolves to Pausable", PauseRules.Resolve(Array.Empty<PauseMode>()) == PauseMode.Pausable);
        T.Check("all Inherit resolves to Pausable",
                PauseRules.Resolve(new[] { PauseMode.Inherit, PauseMode.Inherit }) == PauseMode.Pausable);
        T.Check("an actor's own mode beats its parent's",
                PauseRules.Resolve(new[] { PauseMode.Always, PauseMode.Disabled }) == PauseMode.Always);
        T.Check("Inherit takes the nearest explicit ancestor",
                PauseRules.Resolve(new[] { PauseMode.Inherit, PauseMode.WhenPaused, PauseMode.Pausable }) == PauseMode.WhenPaused);
    }

    private static void Ticks()
    {
        T.Check("Pausable ticks running, not paused",
                PauseRules.Ticks(PauseMode.Pausable, false) && !PauseRules.Ticks(PauseMode.Pausable, true));
        T.Check("WhenPaused ticks paused, not running",
                !PauseRules.Ticks(PauseMode.WhenPaused, false) && PauseRules.Ticks(PauseMode.WhenPaused, true));
        T.Check("Always ticks both", PauseRules.Ticks(PauseMode.Always, false) && PauseRules.Ticks(PauseMode.Always, true));
        T.Check("Disabled ticks neither",
                !PauseRules.Ticks(PauseMode.Disabled, false) && !PauseRules.Ticks(PauseMode.Disabled, true));
    }
}
