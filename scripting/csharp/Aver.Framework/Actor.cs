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

    // The host builds an actor's model tree by calling BuildModels once, after Self is bound and before
    // OnBeginPlay. BuildModels is protected (only the editor's generated .Designer.cs overrides it), so
    // the scripting bridge — a separate assembly — reaches it through this internal shim rather than by
    // reflection. Kept next to BuildModels so the pairing is obvious.
    internal void InvokeBuildModels(ActorBuilder builder) => BuildModels(builder);

    // --- Spawn sugar. Uses the per-thread scratch buffer rather than a fresh float[] per call, matching
    //     the [ThreadStatic] justification the design applies to transform writes (contradiction #8,
    //     resolved: Spawn is rarer than a transform write, but the same rule now covers both). ---

    /// <summary>Spawn an instance of class <paramref name="c"/> at <paramref name="at"/> (centimetres),
    /// returning its entity. The dynamic form, for a class chosen at runtime; prefer <see cref="Spawn{T}"/>
    /// when the type is known. Class default rotation/scale.</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at)
    {
        float[] p = Entity.Scratch3;
        p[0] = at.X; p[1] = at.Y; p[2] = at.Z;
        return new Entity(Fw.aver_fw_spawn(c.Handle, null, p, null, null));
    }

    /// <summary>Spawn <typeparamref name="T"/> at <paramref name="at"/> and return the LIVE INSTANCE — the
    /// C# object you can immediately call into — or null if the class is not declared or its script was
    /// disabled. The type carries its own class name. Use <c>.Self</c> for the entity handle.</summary>
    protected static T? Spawn<T>(Vec3 at) where T : AverActor
        => Actors.Get<T>(Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at));

    /// <summary>Spawn <paramref name="c"/> at a position and orientation.</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at, Rot rotation)
    {
        Quat q = rotation.ToQuat();
        return new Entity(Fw.aver_fw_spawn(c.Handle, null,
            new[] { at.X, at.Y, at.Z }, new[] { q.X, q.Y, q.Z, q.W }, null));
    }

    /// <summary>Spawn <paramref name="c"/> with a full transform (position, orientation, scale).</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at, Rot rotation, Vec3 scale)
    {
        Quat q = rotation.ToQuat();
        return new Entity(Fw.aver_fw_spawn(c.Handle, null,
            new[] { at.X, at.Y, at.Z }, new[] { q.X, q.Y, q.Z, q.W }, new[] { scale.X, scale.Y, scale.Z }));
    }

    /// <summary>Spawn <typeparamref name="T"/> at a position and orientation, returning the live instance.</summary>
    protected static T? Spawn<T>(Vec3 at, Rot rotation) where T : AverActor
        => Actors.Get<T>(Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at, rotation));

    /// <summary>Spawn <typeparamref name="T"/> with a full transform, returning the live instance.</summary>
    protected static T? Spawn<T>(Vec3 at, Rot rotation, Vec3 scale) where T : AverActor
        => Actors.Get<T>(Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at, rotation, scale));

    /// <summary>Spawn <typeparamref name="T"/> as a CHILD of <paramref name="parent"/> at a local position,
    /// returning the live instance. Its transform is then relative to the parent.</summary>
    protected static T? SpawnAttached<T>(Entity parent, Vec3 localPosition) where T : AverActor
    {
        Entity e = Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), localPosition);
        if (e.IsValid) e.SetParent(parent);
        return Actors.Get<T>(e);
    }

    /// <summary>Destroy THIS actor now: OnEndPlay -> unbind runs, and the world destroy is deferred to the
    /// next flush. Safe from any hook (a Destroy() inside OnEndPlay does not double-fire).</summary>
    protected void Destroy() => Fw.aver_fw_destroy(Self.Handle);

    /// <summary>Destroy another entity's actor (its full framework teardown), or nothing if it is invalid.</summary>
    protected static void Destroy(Entity other) { if (other.IsValid) Fw.aver_fw_destroy(other.Handle); }

    /// <summary>Destroy another actor, or nothing if it is null.</summary>
    protected static void Destroy(AverActor? other) { if (other is not null) Fw.aver_fw_destroy(other.Self.Handle); }
}
