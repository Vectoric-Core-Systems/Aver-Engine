// AverPawn: the actor a controller can possess, and the hooks that fire when possession changes.

namespace Aver.Framework;

/// <summary>An actor that can be possessed by a controller. Deriving it sets the PAWN flag on the class row.</summary>
public abstract class AverPawn : AverActor
{
    /// <summary>Runs when <paramref name="controller"/> takes control of this pawn.</summary>
    public virtual void OnPossessed(Entity controller) { }

    /// <summary>Runs when this pawn's controller releases it.</summary>
    public virtual void OnUnpossessed() { }

    /// <summary>The controller currently possessing this pawn, or <see cref="Entity.None"/>.</summary>
    public Entity Controller => new Entity(Fw.aver_fw_controller_of(Self.Handle));

    /// <summary>The possessing controller as <typeparamref name="T"/>, or null.</summary>
    public T? ControllerAs<T>() where T : AverPlayerController => Actors.Get<T>(Controller);

    /// <summary>True while some controller possesses this pawn.</summary>
    public bool IsPossessed => Fw.aver_fw_controller_of(Self.Handle) != 0;
}
