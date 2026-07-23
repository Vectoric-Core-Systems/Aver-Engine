namespace Aver.Framework;

/// <summary>
/// The will that drives a pawn — the translation of input (or an AI policy) into possession of a body.
/// It adds possession CONTROL over <see cref="AverActor"/>: which pawn it holds, and the verbs to take
/// and release one. Deriving this type sets the <c>CONTROLLER</c> flag, the half of the possess check the
/// pawn's <c>PAWN</c> flag completes.
/// </summary>
/// <remarks>
/// <para>
/// <b>Not abstract, unlike the other four base types.</b> A great many games never subclass the
/// controller — the default input-to-pawn plumbing is all they need, and a GameMode names
/// <c>PlayerControllerClass = "PlayerController"</c> by string. So this type is directly instantiable and
/// registers as a usable class in its own right; Actor, Pawn, GameMode and GameInstance are abstract
/// because a bare one of those is never what you want.
/// </para>
/// <para>
/// Possession state is read straight from the world, never cached: <see cref="Possessed"/> asks
/// <c>aver_fw_controlled_pawn</c> each time, so it cannot disagree with the engine after a reparent,
/// destroy or reload.
/// </para>
/// </remarks>
public class AverPlayerController : AverActor
{
    /// <summary>The pawn this controller currently drives, or <see cref="Entity.None"/> if none.</summary>
    public Entity Possessed => new Entity(Fw.aver_fw_controlled_pawn(Self.Handle));

    /// <summary>The possessed pawn as <typeparamref name="T"/>, or null if none or another type.</summary>
    public T? PossessedAs<T>() where T : AverPawn => Actors.Get<T>(Possessed);

    /// <summary>True while this controller drives a pawn.</summary>
    public bool HasPawn => Fw.aver_fw_controlled_pawn(Self.Handle) != 0;

    /// <summary>
    /// Take control of <paramref name="pawn"/>. Returns false if the world rejected it — most often because
    /// the target's class does not carry the <c>PAWN</c> flag, or it is already possessed. On success the
    /// pawn's <see cref="AverPawn.OnPossessed"/> fires.
    /// </summary>
    public bool Possess(Entity pawn) => Fw.aver_fw_possess(Self.Handle, pawn.Handle) != 0;

    /// <summary>Take control of <paramref name="pawn"/> directly. Convenience over the entity overload.</summary>
    public bool Possess(AverPawn pawn) => Possess(pawn.Self);

    /// <summary>Release the currently possessed pawn, firing its <see cref="AverPawn.OnUnpossessed"/>. False if nothing was possessed.</summary>
    public bool Unpossess() => Fw.aver_fw_unpossess(Self.Handle) != 0;
}
