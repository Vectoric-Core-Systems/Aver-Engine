// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// AverActor: the root gameplay type — one entity handle, the lifecycle hooks, and spawn helpers.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The root gameplay type: something in the world with a transform and a lifecycle.</summary>
public abstract partial class AverActor
{
    /// <summary>This actor's entity. Set by the host at bind; a script reads it, never assigns it.</summary>
    public Entity Self { get; internal set; }

    /// <summary>Runs once when this actor begins playing, for a spawn, a world start or a hot reload.</summary>
    public virtual void OnBeginPlay(BeginReason reason) { }

    /// <summary>Runs once a frame, after the frame's physics steps.<paramref name="dt"/> is
    /// the frame time in seconds. On when the class overrides it; see <see cref="TickEnabled"/>.</summary>
    public virtual void OnTick(float dt) { }

    /// <summary>Runs once per fixed physics step, before that step: 0 to 8 times a
    /// frame. <paramref name="dt"/> is the fixed step. On when the class overrides it; see <see cref="PhysicsTickEnabled"/>.</summary>
    public virtual void OnPhysicsTick(float dt) { }

    /// <summary>Receives each input event first, deepest actor first. Call
    /// <see cref="Input.SetInputAsHandled"/> to stop it there; otherwise it goes to the game UI, then
    /// <see cref="OnUnhandledInput"/>. Runs before the frame's ticks, while the game is not paused.</summary>
    public virtual void OnInput(InputEvent e) { }

    /// <summary>Receives the input events nothing else handled: the place for gameplay
    /// controls, so a click on a menu does not also fire a weapon.</summary>
    public virtual void OnUnhandledInput(InputEvent e) { }

    /// <summary>Whether this actor receives input events. Defaults to true when the class overrides OnInput or
    /// OnUnhandledInput.</summary>
    public bool InputEnabled { get; set; }

    /// <summary>Whether <see cref="OnTick"/> runs. Defaults to true when the class overrides it.</summary>
    public bool TickEnabled { get; set; }

    /// <summary>Whether <see cref="OnPhysicsTick"/> runs. Defaults to true when the class overrides it.</summary>
    public bool PhysicsTickEnabled { get; set; }

    /// <summary>Order among actors in the <see cref="OnTick"/> pass: lower runs first, equal priorities in scene-tree
    /// order (a parent before its children, siblings in the order they were added).</summary>
    public int TickPriority
    {
        get => _tickPriority;
        set { if (_tickPriority != value) { _tickPriority = value; TickOrderChanged?.Invoke(); } }
    }

    /// <summary>Order among actors in the <see cref="OnPhysicsTick"/> pass, as <see cref="TickPriority"/>.</summary>
    public int PhysicsTickPriority
    {
        get => _physicsTickPriority;
        set { if (_physicsTickPriority != value) { _physicsTickPriority = value; TickOrderChanged?.Invoke(); } }
    }

    /// <summary>Whether this actor ticks while the game is paused; <see cref="Aver.Framework.PauseMode.Inherit"/> by default,
    /// which takes the nearest actor above it in the scene tree. A paused game does not step physics.</summary>
    public PauseMode PauseMode
    {
        get => _pauseMode;
        set { if (_pauseMode != value) { _pauseMode = value; TickOrderChanged?.Invoke(); } }
    }

    private int _tickPriority, _physicsTickPriority;
    private PauseMode _pauseMode;

    // An instance the next bind should use instead of constructing one (a node that scripts made with `new`
    // and then added to the tree). The bridge calls TakePending from its bind; any other caller clears it.
    [ThreadStatic] private static AverActor? s_pending;

    internal static void OfferPending(AverActor? actor) => s_pending = actor;

    /// <summary>The instance offered for the bind that is running, when it is a <paramref name="type"/>; otherwise null.
    /// The bridge calls this before it would construct a new instance.</summary>
    public static AverActor? TakePending(Type type)
    {
        AverActor? a = s_pending;
        s_pending = null;
        return a != null && type.IsInstanceOfType(a) ? a : null;
    }

    // Set when the [Editable] values were written early (a node reads its exported values on entering the tree);
    // the bridge's write before OnBeginPlay then skips this instance.
    internal bool EditablesApplied;

    // Set by the scripting bridge: a priority changed, so its walk order is stale.
    internal static Action? TickOrderChanged;

    private static readonly Dictionary<Type, (bool Tick, bool Physics, bool Input)> s_overrides = new();

    /// <summary>Turns each tick on when the concrete class overrides it.</summary>
    protected AverActor()
    {
        Type t = GetType();
        if (!s_overrides.TryGetValue(t, out var o))
        {
            o = (Overrides(t, nameof(OnTick), typeof(float)), Overrides(t, nameof(OnPhysicsTick), typeof(float)),
                 Overrides(t, nameof(OnInput), typeof(InputEvent)) || Overrides(t, nameof(OnUnhandledInput), typeof(InputEvent)));
            s_overrides[t] = o;
        }
        TickEnabled = o.Tick;
        PhysicsTickEnabled = o.Physics;
        InputEnabled = o.Input;
    }

    private static bool Overrides(Type t, string method, Type arg) =>
        t.GetMethod(method, new[] { arg })?.DeclaringType != typeof(AverActor);

    /// <summary>Runs when the actor enters the world, before <see cref="OnBeginPlay"/>. Among
    /// actors placed together a parent enters before its children, then they begin play children first.</summary>
    public virtual void OnEnterTree() { }

    /// <summary>Runs when the actor leaves the world, before <see cref="OnEndPlay"/>; children
    /// leave before their parent.</summary>
    public virtual void OnExitTree() { }

    /// <summary>Runs when the actor's parent in the scene tree changed.</summary>
    public virtual void OnParentChanged() { }

    /// <summary>Runs once a frame, after OnTick, when the actor's world transform moved, if <see cref="NotifyTransform"/>
    /// is on.</summary>
    public virtual void OnTransformChanged() { }

    /// <summary>Whether <see cref="OnTransformChanged"/> is called. Off by default.</summary>
    public bool NotifyTransform { get; set; }

    /// <summary>Destroys this actor at the end of the frame, after every tick and deferred call.
    /// It keeps ticking for the rest of this frame.</summary>
    public void QueueDestroy() => GameThread.QueueDestroy(this);

    /// <summary>Runs once when this actor stops playing.</summary>
    public virtual void OnEndPlay(EndReason reason) { }

    /// <summary>Runs after a hot reload has rebound this instance and restored its native state.</summary>
    public virtual void OnRebound() { }

    /// <summary>Runs when a named event is raised at this actor's entity.
    ///
    /// TWO THINGS RAISE ONE TODAY, and they are deliberately the same hook. An ANIMATION NOTIFY --
    /// a marker placed on a clip in the animation editor, delivered on the frame playback crosses
    /// it -- and a graph's <c>FireEvent</c> node aimed at this entity. Both mean "something
    /// happened, here, now, and it is called <paramref name="name"/>", and a script that wants to
    /// play a footstep sound should not have to care which one delivered it.
    ///
    /// WHY ONE HOOK RATHER THAN A METHOD PER EVENT NAME. Unreal binds notifies by NAME
    /// (<c>AnimNotify_Footstep</c>), resolved by reflection at load. That works there because an
    /// anim notify is a distinct concept with its own class; here an event name is opaque all the
    /// way down -- the format stores a string, GraphHost.Fire branches on nothing, and a project
    /// invents names freely. A reflection-by-name convention would put a silent failure (a typo in
    /// a method name) exactly where this engine has been bitten before, and would still need this
    /// method underneath for the names that are computed rather than written.
    ///
    /// NOT CALLED FOR OnBeginPlay/OnTick/OnEndPlay: those have their own hooks and are driven by
    /// the tick, not raised at an entity.
    ///
    /// Return true if this actor handled it. The value is reported back to whatever raised the
    /// event -- a <c>FireEvent</c> node reads it as its own success -- so returning false from the
    /// default branch of a switch is the honest answer for a name this actor does not know.</summary>
    public virtual bool OnEvent(string name) => false;

    /// <summary>Releases any native resource this actor owns. Host-driven, after OnEndPlay, whatever the reason.</summary>
    internal virtual void OnUnbound() { }

    /// <summary>Builds the model tree carried inside this actor. Overridden only by the editor's .Designer.cs partial.</summary>
    protected virtual void BuildModels(ActorBuilder builder) { }

    /// <summary>Calls <see cref="BuildModels"/> on behalf of the scripting bridge assembly.</summary>
    internal void InvokeBuildModels(ActorBuilder builder) => BuildModels(builder);

    /// <summary>Spawns an instance of class <paramref name="c"/> at <paramref name="at"/> and returns its entity.</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at)
    {
        float[] p = Entity.Scratch3;
        p[0] = at.X; p[1] = at.Y; p[2] = at.Z;
        return new Entity(Fw.aver_fw_spawn(c.Handle, null, p, null, null));
    }

    /// <summary>Spawns <typeparamref name="T"/> at <paramref name="at"/> and returns the live instance, or null.</summary>
    protected static T? Spawn<T>(Vec3 at) where T : AverActor
        => Actors.Get<T>(Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at));

    /// <summary>Spawns <paramref name="c"/> at a position and orientation.</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at, Rot rotation)
    {
        Quat q = rotation.ToQuat();
        return new Entity(Fw.aver_fw_spawn(c.Handle, null,
            new[] { at.X, at.Y, at.Z }, new[] { q.X, q.Y, q.Z, q.W }, null));
    }

    /// <summary>Spawns <paramref name="c"/> with a full transform (position, orientation, scale).</summary>
    protected static Entity Spawn(ActorClass c, Vec3 at, Rot rotation, Vec3 scale)
    {
        Quat q = rotation.ToQuat();
        return new Entity(Fw.aver_fw_spawn(c.Handle, null,
            new[] { at.X, at.Y, at.Z }, new[] { q.X, q.Y, q.Z, q.W }, new[] { scale.X, scale.Y, scale.Z }));
    }

    /// <summary>Spawns <typeparamref name="T"/> at a position and orientation, returning the live instance.</summary>
    protected static T? Spawn<T>(Vec3 at, Rot rotation) where T : AverActor
        => Actors.Get<T>(Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at, rotation));

    /// <summary>Spawns <typeparamref name="T"/> with a full transform, returning the live instance.</summary>
    protected static T? Spawn<T>(Vec3 at, Rot rotation, Vec3 scale) where T : AverActor
        => Actors.Get<T>(Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at, rotation, scale));

    /// <summary>Spawns <typeparamref name="T"/> as a child of <paramref name="parent"/>; destroys it if the attach is rejected.</summary>
    protected static T? SpawnAttached<T>(Entity parent, Vec3 localPosition) where T : AverActor
    {
        Entity e = Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), localPosition);
        if (!e.IsValid) return null;
        if (!e.SetParent(parent)) { e.Destroy(); return null; }
        return Actors.Get<T>(e);
    }

    /// <summary>Destroys this actor: OnEndPlay and unbind run now, the world destroy is deferred.</summary>
    protected void Destroy() => Fw.aver_fw_destroy(Self.Handle);

    /// <summary>Destroys another entity's actor, or nothing if it is invalid.</summary>
    protected static void Destroy(Entity other) { if (other.IsValid) Fw.aver_fw_destroy(other.Handle); }

    /// <summary>Destroys another actor, or nothing if it is null.</summary>
    protected static void Destroy(AverActor? other) { if (other is not null) Fw.aver_fw_destroy(other.Self.Handle); }
}
