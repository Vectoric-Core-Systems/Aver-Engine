namespace Aver.Framework;

// The reason enums cross to the hooks so a script can tell "born" from "reloaded" without inspecting
// global state. Their numeric values are PINNED to the AVER_FW_BEGIN_*/END_* / tick-group #defines in
// framework_abi.h — the C# names carry the meaning, the numbers carry the wire compatibility.

/// <summary>
/// Why <see cref="AverActor.OnBeginPlay"/> is running. The distinction exists because a script must
/// initialise differently for a fresh spawn than for a hot reload of an actor that was already alive.
/// </summary>
/// <remarks>
/// <b>Spawn</b> — the actor was just created (from a level load, an <see cref="AverActor.Spawn"/> call,
/// or an editor drop) while the world is <i>not</i> playing yet. <b>Play</b> — the world transitioned
/// into play and every already-placed actor begins. <b>Reload</b> — the same instance is coming back up
/// after a code hot-reload; its native <c>[Editable]</c> state was parked and restored, so re-running
/// spawn-time setup would be wrong. Pair this with <see cref="AverActor.OnRebound"/>.
/// </remarks>
public enum BeginReason
{
    Spawn = 0,
    Play = 1,
    Reload = 2,
}

/// <summary>
/// Why <see cref="AverActor.OnEndPlay"/> is running. <b>Reload</b> means the entity is <i>staying</i> —
/// only the managed half is being torn down and rebuilt — so a script must not release shared world
/// state it expects to see again.
/// </summary>
public enum EndReason
{
    Destroy = 0,
    Stop = 1,
    Reload = 2,
    Travel = 3,
}

/// <summary>
/// When in the frame an actor ticks, relative to physics. A class declares its group once via
/// <see cref="ClassBuilder.Ticks"/>; the native tick loop walks the groups in order and calls the
/// managed batch once per group. Values are pinned to the AVER_FW_TICK_* group ids.
/// </summary>
public enum TickGroup
{
    PrePhysics = 0,
    Physics = 1,
    PostPhysics = 2,
}

/// <summary>The world's play state, as read from <c>aver_fw_play_state</c>. Editor is the authoring state.</summary>
public enum PlayState
{
    Editor = 0,
    Playing = 1,
    Paused = 2,
}

/// <summary>
/// Registry-row flag bits (framework_abi.h AVER_FW_CLASS_*). A script never sets these by hand — the
/// base type a class derives from and the builder calls it makes decide them, so the compiler, not a
/// magic number, is what says "this is a Pawn". Exposed <c>internal</c> because the base-class plumbing
/// and the discovery pass both need them; kept in one place so they cannot drift from the header.
/// </summary>
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
