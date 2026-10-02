// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// The two session-singleton base types: the per-world GameMode and the process-lifetime GameInstance.

namespace Aver.Framework;

/// <summary>The rules of a level while it plays. One live per world; hands out the pawn and controller named on its attribute.</summary>
public abstract class AverGameMode : AverActor
{
    /// <summary>Runs after a player's controller has entered the world.</summary>
    public virtual void OnPostLogin(Entity controller) { }
}

/// <summary>Process-lifetime state that spans worlds. Created once at startup, destroyed at shutdown, never on travel.</summary>
public abstract class AverGameInstance : AverActor
{
}
