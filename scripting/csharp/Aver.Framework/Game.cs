// Static access to the running play session: state, singletons, lookup and world queries.

namespace Aver.Framework;

/// <summary>The running play session, reached statically from any script. Read-only: the host starts and stops it.</summary>
public static class Game
{
    /// <summary>The world's play state — Editor, Playing or Paused.</summary>
    public static PlayState State => (PlayState)Fw.aver_fw_play_state();

    /// <summary>True while a session is actively ticking.</summary>
    public static bool IsPlaying => State == PlayState.Playing;

    /// <summary>True while a session exists but is frozen.</summary>
    public static bool IsPaused => State == PlayState.Paused;

    /// <summary>True while a session exists at all — Playing or Paused.</summary>
    public static bool HasSession => State != PlayState.Editor;

    /// <summary>The current GameMode's entity, or <see cref="Entity.None"/> in editor.</summary>
    public static Entity Mode => new(Fw.aver_fw_game_mode());

    /// <summary>The GameInstance entity, or <see cref="Entity.None"/> in editor.</summary>
    public static Entity Instance => new(Fw.aver_fw_game_instance());

    /// <summary>Player 0's controller.</summary>
    public static Entity LocalPlayerController => GetPlayerController(0);

    /// <summary>The controller for a 0-based player index, or <see cref="Entity.None"/>.</summary>
    public static Entity GetPlayerController(int playerIndex = 0) => new(Fw.aver_fw_player_controller(playerIndex));

    /// <summary>The pawn a given player currently possesses, or <see cref="Entity.None"/>.</summary>
    public static Entity GetPlayerPawn(int playerIndex = 0) =>
        new(Fw.aver_fw_controlled_pawn(Fw.aver_fw_player_controller(playerIndex)));

    /// <summary>The current GameMode as <typeparamref name="T"/>, or null.</summary>
    public static T? ModeAs<T>() where T : AverGameMode => Actors.Get<T>(Mode);

    /// <summary>The GameInstance as <typeparamref name="T"/>, or null.</summary>
    public static T? InstanceAs<T>() where T : AverGameInstance => Actors.Get<T>(Instance);

    /// <summary>A player's controller as <typeparamref name="T"/>, or null.</summary>
    public static T? PlayerControllerAs<T>(int playerIndex = 0) where T : AverPlayerController =>
        Actors.Get<T>(GetPlayerController(playerIndex));

    /// <summary>A player's possessed pawn as <typeparamref name="T"/>, or null.</summary>
    public static T? PlayerPawnAs<T>(int playerIndex = 0) where T : AverPawn => Actors.Get<T>(GetPlayerPawn(playerIndex));

    /// <summary>The live entity named <paramref name="name"/> (first match), or <see cref="Entity.None"/>. A linear scan.</summary>
    public static Entity Find(string name) => new(SceneNative.aver_scene_find(name));

    /// <summary>The actor named <paramref name="name"/> as <typeparamref name="T"/>, or null.</summary>
    public static T? FindActor<T>(string name) where T : AverActor => Actors.Get<T>(Find(name));

    /// <summary>Every live actor in the world; plain scene entities excluded. Do not spawn or destroy while enumerating.</summary>
    public static IEnumerable<Entity> AllActors()
    {
        int n = SceneNative.aver_scene_count();
        for (int i = 0; i < n; i++)
        {
            Entity e = new(SceneNative.aver_scene_at(i));
            if (e.IsActor) yield return e;
        }
    }

    /// <summary>Every live actor whose managed instance is a <typeparamref name="T"/>.</summary>
    public static IEnumerable<T> ActorsOf<T>() where T : AverActor
    {
        foreach (Entity e in AllActors())
            if (Actors.Get<T>(e) is { } actor)
                yield return actor;
    }

    /// <summary>Every live entity carrying all of the tag bits in <paramref name="mask"/>.</summary>
    public static IEnumerable<Entity> WithTag(uint mask)
    {
        int n = SceneNative.aver_scene_count();
        for (int i = 0; i < n; i++)
        {
            Entity e = new(SceneNative.aver_scene_at(i));
            if (e.IsValid && e.HasTag(mask)) yield return e;
        }
    }
}
