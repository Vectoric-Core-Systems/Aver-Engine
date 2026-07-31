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

/// <summary>When in the frame an actor ticks, relative to physics. Pinned to the AVER_FW_TICK_* group ids.</summary>
public enum TickGroup
{
    PrePhysics = 0,
    Physics = 1,
    PostPhysics = 2,
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
