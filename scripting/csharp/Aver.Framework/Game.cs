namespace Aver.Framework;

/// <summary>
/// The running play session, reached statically from any script. In EDITOR every handle is
/// <see cref="Entity.None"/> and <see cref="State"/> is <see cref="PlayState.Editor"/>; between begin-play
/// and Stop these address the GameInstance, GameMode and player controllers the framework spawned.
/// </summary>
/// <remarks>
/// Read-only on purpose: STARTING and STOPPING a session is the host's job (the editor's Play/Stop bar, or
/// a headless --play-test), not a script's — a script that could end the world from a tick hook is a
/// footgun, and the lifecycle is owned one layer up. Scripts read the session here and act through their
/// own actors. Every accessor is a thin read of the framework's session singletons, so it is always
/// current: cache the RESULT within a frame if you like, but never across one.
/// </remarks>
public static class Game
{
    /// <summary>The world's play state — Editor, Playing or Paused.</summary>
    public static PlayState State => (PlayState)Fw.aver_fw_play_state();

    /// <summary>True while a session is actively ticking (Playing, not Paused, not Editor).</summary>
    public static bool IsPlaying => State == PlayState.Playing;

    /// <summary>True while a session exists but is frozen.</summary>
    public static bool IsPaused => State == PlayState.Paused;

    /// <summary>True while a session exists at all — Playing OR Paused, as opposed to EDITOR authoring.</summary>
    public static bool HasSession => State != PlayState.Editor;

    /// <summary>The current GameMode's entity, or <see cref="Entity.None"/> in EDITOR.</summary>
    public static Entity Mode => new(Fw.aver_fw_game_mode());

    /// <summary>The GameInstance entity, or <see cref="Entity.None"/> in EDITOR.</summary>
    public static Entity Instance => new(Fw.aver_fw_game_instance());

    /// <summary>Player 0's controller — the local player in a single-player session.</summary>
    public static Entity LocalPlayerController => GetPlayerController(0);

    /// <summary>The controller for a given 0-based player index, or <see cref="Entity.None"/>.</summary>
    public static Entity GetPlayerController(int playerIndex = 0) => new(Fw.aver_fw_player_controller(playerIndex));

    /// <summary>The pawn a given player currently possesses, or <see cref="Entity.None"/>.</summary>
    public static Entity GetPlayerPawn(int playerIndex = 0) =>
        new(Fw.aver_fw_controlled_pawn(Fw.aver_fw_player_controller(playerIndex)));

    // --- Typed accessors: the managed instance behind each singleton, or null. ---

    /// <summary>The current GameMode as <typeparamref name="T"/>, or null.</summary>
    public static T? ModeAs<T>() where T : AverGameMode => Actors.Get<T>(Mode);

    /// <summary>The GameInstance as <typeparamref name="T"/>, or null.</summary>
    public static T? InstanceAs<T>() where T : AverGameInstance => Actors.Get<T>(Instance);

    /// <summary>A player's controller as <typeparamref name="T"/>, or null.</summary>
    public static T? PlayerControllerAs<T>(int playerIndex = 0) where T : AverPlayerController =>
        Actors.Get<T>(GetPlayerController(playerIndex));

    /// <summary>A player's possessed pawn as <typeparamref name="T"/>, or null.</summary>
    public static T? PlayerPawnAs<T>(int playerIndex = 0) where T : AverPawn => Actors.Get<T>(GetPlayerPawn(playerIndex));

    // --- Lookup. ---

    /// <summary>The live entity named <paramref name="name"/> (first match), or <see cref="Entity.None"/>.
    /// A linear scan — for resolving a known actor by name, not a per-frame query.</summary>
    public static Entity Find(string name) => new(SceneNative.aver_scene_find(name));

    /// <summary>The actor named <paramref name="name"/> as <typeparamref name="T"/>, or null.</summary>
    public static T? FindActor<T>(string name) where T : AverActor => Actors.Get<T>(Find(name));
}
