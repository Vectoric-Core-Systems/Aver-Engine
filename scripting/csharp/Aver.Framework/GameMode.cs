namespace Aver.Framework;

/// <summary>
/// The rules of a level while it is playing — what pawn a joining player gets, which controller drives it,
/// and the win/score/spawn logic on top. A GameMode is itself an actor (it has a lifecycle and ticks), and
/// there is exactly one live per world. It adds one hook over <see cref="AverActor"/>: the moment a player
/// joins.
/// </summary>
/// <remarks>
/// The pawn and controller it hands out are named BY STRING on <see cref="AverGameModeAttribute"/>, not by
/// <c>typeof</c>, and resolved at seal — so a GameMode never takes a compile-time reference to the classes
/// it spawns, which is what lets those classes live in other assemblies or load later. Read the current
/// mode with <c>aver_fw_game_mode</c>; it is a per-world singleton, replaced on travel.
/// </remarks>
public abstract class AverGameMode : AverActor
{
    /// <summary>Runs after a player's controller has entered the world — the place to spawn and possess its pawn.</summary>
    public virtual void OnPostLogin(Entity controller) { }
}

/// <summary>
/// Process-lifetime state that spans worlds — the one object that outlives a level load. Save slots, the
/// signed-in player, audio settings, a session's RNG seed: things that must NOT reset when the world
/// travels. It is an actor for uniformity of lifecycle, but it is created once at startup and destroyed
/// once at shutdown, never on travel, and there is exactly one.
/// </summary>
/// <remarks>
/// It adds nothing over <see cref="AverActor"/> on purpose: the concept it contributes is LIFETIME, not
/// new verbs. A game subclasses it and puts the cross-world state in <c>[Editable]</c> or plain fields;
/// because the instance is never destroyed between worlds, even plain fields persist across a travel
/// (though not across a code hot reload — see <see cref="AverActor.OnRebound"/>). Reach it with
/// <c>aver_fw_game_instance</c>.
/// </remarks>
public abstract class AverGameInstance : AverActor
{
}
