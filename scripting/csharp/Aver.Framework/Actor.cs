using Aver.Scene;   // Vec3

namespace Aver.Framework;

/// <summary>
/// The root gameplay type: something in the world with a transform and a lifecycle. An <see cref="Entity"/>
/// wearing a familiar name — it holds ONE field, the entity handle, and adds no storage of its own. The
/// native side has no idea this hierarchy exists; Actor/Pawn/Controller/GameMode/GameInstance are one
/// mechanism differentiated by a flag bit on a registry row, and the base type you derive is only how the
/// COMPILER decides which flag and which hooks you get.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why a base class, not an attribute, for the hooks.</b> A hook is a <c>virtual</c> override, so a
/// misspelt <c>OnTick</c> is a build error rather than a method that silently never runs — the worst
/// failure for a non-engineer scripter. This is the settled AverBehaviour reasoning
/// (Aver.Scripting/Behaviour.cs) and it holds identically here. Discovery is <c>IsAssignableFrom</c>:
/// the loader finds every non-abstract type deriving <see cref="AverActor"/>.
/// </para>
/// <para>
/// <b>What survives a hot reload, at this surface.</b> <c>[Editable]</c> fields (native storage) and the
/// generated model placements survive — the entity is never destroyed. Plain instance fields do NOT:
/// they restart from their initialisers, because the managed instance is rebuilt. Re-derive any cached
/// state from the surviving native state in <see cref="OnRebound"/>, not in <see cref="OnBeginPlay"/>.
/// </para>
/// <para>
/// <b>Exceptions never cross back into the engine.</b> A throw out of any hook is caught by the host,
/// logged, and that one actor is disabled for the session; the process and every other actor survive.
/// This mirrors the AverBehaviour contract exactly.
/// </para>
/// </remarks>
public abstract class AverActor
{
    /// <summary>This actor's entity. Set by the host at bind; a script reads it, never assigns it.</summary>
    public Entity Self { get; internal set; }

    // --- Lifecycle. Order per frame: begin-play queue drains, then OnTick in tick-group order, then the
    //     destroy queue drains. The reason tells a fresh birth from a reload or a world stop. ---

    /// <summary>
    /// Runs once when this actor begins playing. <paramref name="reason"/> is <b>Spawn</b> for a fresh
    /// creation, <b>Play</b> when the world starts, <b>Reload</b> when a hot-reloaded instance comes back.
    /// Guard spawn-only setup behind <c>reason != BeginReason.Reload</c>.
    /// </summary>
    public virtual void OnBeginPlay(BeginReason reason) { }

    /// <summary>Runs every frame the actor's class is scheduled to tick, in its declared tick group. <paramref name="dt"/> is clamped seconds.</summary>
    public virtual void OnTick(float dt) { }

    /// <summary>
    /// Runs once when this actor stops playing. <paramref name="reason"/> is <b>Reload</b> when the entity
    /// is STAYING (only the managed half is being rebuilt), so release nothing you expect back.
    /// </summary>
    public virtual void OnEndPlay(EndReason reason) { }

    /// <summary>
    /// Runs after a hot reload has rebound this instance and restored its native <c>[Editable]</c> state —
    /// the place to re-derive cached managed state from surviving native state, WITHOUT re-running the
    /// spawn logic in <see cref="OnBeginPlay"/>. Distinct from OnBeginPlay on purpose; overriding
    /// OnBeginPlay but not OnRebound earns one load-time warning.
    /// </summary>
    public virtual void OnRebound() { }

    /// <summary>
    /// Builds the model tree carried inside this actor. The default is empty; the EDITOR overrides it in
    /// the actor's <c>.Designer.cs</c> partial, and only there. The host calls it once, after
    /// <see cref="Self"/> is bound and before <see cref="OnBeginPlay"/>. Hand code never calls or writes
    /// this — a computed placement belongs in a hook, not in the gizmo-editable region.
    /// </summary>
    protected virtual void BuildModels(ActorBuilder builder) { }

    // --- Spawn sugar. Uses the per-thread scratch buffer rather than a fresh float[] per call, matching
    //     the [ThreadStatic] justification the design applies to transform writes (contradiction #8,
    //     resolved: Spawn is rarer than a transform write, but the same rule now covers both). ---

    /// <summary>Spawn an instance of class <paramref name="c"/> at <paramref name="at"/> (centimetres). Class default rotation/scale.</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at)
    {
        float[] p = Entity.Scratch3;
        p[0] = at.X; p[1] = at.Y; p[2] = at.Z;
        return new Entity(Fw.aver_fw_spawn(c.Handle, null, p, null, null));
    }

    /// <summary>Spawn <typeparamref name="T"/> at <paramref name="at"/> — the type carries its own class name.</summary>
    protected static Entity Spawn<T>(Vec3 at) where T : AverActor
        => Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at);
}
