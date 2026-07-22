namespace Aver.Framework;

/// <summary>
/// An actor that can be POSSESSED by a controller — the physical avatar a player or an AI drives. It adds
/// exactly one concept over <see cref="AverActor"/>: the two hooks that fire when possession changes.
/// Deriving this type is what sets the <c>PAWN</c> flag on the class row, and that flag is what makes
/// <c>aver_fw_possess</c> accept this class — so the type you derive, checked by the compiler, is the type
/// safety.
/// </summary>
/// <remarks>
/// A pawn does not hold a reference to its controller as managed state — that would be a second copy of a
/// fact the world already owns and would rot across a reload. Ask the world through
/// <c>aver_fw_controller_of</c> when needed; the hooks below hand you the controller for the moment you
/// care about.
/// </remarks>
public abstract class AverPawn : AverActor
{
    /// <summary>Runs when <paramref name="controller"/> takes control of this pawn.</summary>
    public virtual void OnPossessed(Entity controller) { }

    /// <summary>Runs when this pawn's controller releases it.</summary>
    public virtual void OnUnpossessed() { }
}
