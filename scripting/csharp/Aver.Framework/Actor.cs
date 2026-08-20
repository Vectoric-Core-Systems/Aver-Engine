// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// AverActor: the root gameplay type — one entity handle, the lifecycle hooks, and spawn helpers.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The root gameplay type: something in the world with a transform and a lifecycle.</summary>
public abstract class AverActor
{
    /// <summary>This actor's entity. Set by the host at bind; a script reads it, never assigns it.</summary>
    public Entity Self { get; internal set; }

    /// <summary>Runs once when this actor begins playing, for a spawn, a world start or a hot reload.</summary>
    public virtual void OnBeginPlay(BeginReason reason) { }

    /// <summary>Runs every frame the actor's class ticks. <paramref name="dt"/> is clamped seconds.</summary>
    public virtual void OnTick(float dt) { }

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
