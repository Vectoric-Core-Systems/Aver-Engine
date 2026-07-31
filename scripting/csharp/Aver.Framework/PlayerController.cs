// AverPlayerController: the actor that possesses a pawn and drives it.

namespace Aver.Framework;

/// <summary>The will that drives a pawn. Deriving it sets the CONTROLLER flag the possess check needs.</summary>
public class AverPlayerController : AverActor
{
    /// <summary>The pawn this controller currently drives, or <see cref="Entity.None"/> if none.</summary>
    public Entity Possessed => new Entity(Fw.aver_fw_controlled_pawn(Self.Handle));

    /// <summary>The possessed pawn as <typeparamref name="T"/>, or null if none or another type.</summary>
    public T? PossessedAs<T>() where T : AverPawn => Actors.Get<T>(Possessed);

    /// <summary>True while this controller drives a pawn.</summary>
    public bool HasPawn => Fw.aver_fw_controlled_pawn(Self.Handle) != 0;

    /// <summary>Takes control of <paramref name="pawn"/>, stealing it from another controller if need be.</summary>
    public bool Possess(Entity pawn) => Fw.aver_fw_possess(Self.Handle, pawn.Handle) != 0;

    /// <summary>Takes control of <paramref name="pawn"/> directly.</summary>
    public bool Possess(AverPawn pawn) => Possess(pawn.Self);

    /// <summary>Releases the currently possessed pawn. False if nothing was possessed.</summary>
    public bool Unpossess() => Fw.aver_fw_unpossess(Self.Handle) != 0;
}
