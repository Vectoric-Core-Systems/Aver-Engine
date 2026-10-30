// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The lifecycle enums the hooks carry, the tick groups, and the registry-row flag bits.

namespace Aver.Framework;

/// <summary>Why <see cref="AverActor.OnBeginPlay"/> is running: a fresh spawn, a world start, or a hot reload.</summary>
public enum BeginReason
{
    Spawn = 0,
    Play = 1,
    Reload = 2,
}

/// <summary>Why <see cref="AverActor.OnEndPlay"/> is running. Reload means the entity is staying.</summary>
public enum EndReason
{
    Destroy = 0,
    Stop = 1,
    Reload = 2,
    Travel = 3,
}

/// <summary>Kept so older scripts compile; see <see cref="ClassBuilder.Ticks"/>. Ticking now follows the
/// OnTick / OnPhysicsTick overrides.</summary>
[Obsolete("Override OnTick or OnPhysicsTick instead.")]
public enum TickGroup
{
    PrePhysics = 0,
    Physics = 1,
    PostPhysics = 2,
}

// The two tick passes. Pinned to the AVER_FW_TICK_* ids in framework_abi.h.
internal enum TickPass
{
    Physics = 0,
    Process = 1,
}

/// <summary>Whether an actor ticks while the game is paused (the process mode).</summary>
public enum PauseMode
{
    /// <summary>Whatever the nearest actor above it in the scene tree uses; Pausable at the top.</summary>
    Inherit = 0,
    /// <summary>Ticks only while the game runs.</summary>
    Pausable = 1,
    /// <summary>Ticks only while the game is paused: a pause menu's logic.</summary>
    WhenPaused = 2,
    /// <summary>Ticks whether or not the game is paused.</summary>
    Always = 3,
    /// <summary>Never ticks.</summary>
    Disabled = 4,
}

/// <summary>The world's play state, as read from <c>aver_fw_play_state</c>.</summary>
public enum PlayState
{
    Editor = 0,
    Playing = 1,
    Paused = 2,
}

/// <summary>Registry-row flag bits. Mirrors the AVER_FW_CLASS_* values in framework_abi.h.</summary>
internal static class ClassFlags
{
    internal const int Ticks = 0x0001;
    internal const int Pawn = 0x0002;
    internal const int Controller = 0x0004;
    internal const int GameMode = 0x0008;
    internal const int GameInstance = 0x0010;
    internal const int Managed = 0x0020;
    internal const int Abstract = 0x0040;
    internal const int TickInEditor = 0x0080;
}
