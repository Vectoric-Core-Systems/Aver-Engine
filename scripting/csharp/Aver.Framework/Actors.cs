// Entity-to-instance lookup, backed by a resolver the scripting host installs at bootstrap,
// plus the public spawn entry point for callers that are not themselves actors.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>Turns an <see cref="Entity"/> back into the C# object that is that actor.</summary>
public static class Actors
{
    // Installed once by the scripting bridge at bootstrap; left null in a host with no managed actors.
    internal static System.Func<int, AverActor?>? Resolver;

    /// <summary>The managed actor bound to <paramref name="e"/>, or null if it is dead, not an actor, or disabled.</summary>
    public static AverActor? Get(Entity e) => e.IsValid ? Resolver?.Invoke(e.Handle) : null;

    /// <summary>The managed actor bound to <paramref name="e"/> as <typeparamref name="T"/>, or null.</summary>
    public static T? Get<T>(Entity e) where T : AverActor => Get(e) as T;

    /// <summary>Spawns <paramref name="c"/> at a position, orientation and scale, from outside an actor.</summary>
    /// <remarks>
    /// AverActor already has Spawn overloads, but they are <c>protected</c> — reachable only from
    /// inside an actor — and <c>aver_fw_spawn</c> itself is internal to this assembly. So there was
    /// no way for any OTHER assembly to spawn anything, which a generator that produces placements
    /// and hands them to the engine has to be able to do.
    ///
    /// Deliberately takes a <see cref="Quat"/> rather than a <see cref="Rot"/>: a generator computes
    /// orientations, it does not author them in degrees, and converting to Euler and back would
    /// lose precision on the way through for no reason.
    /// </remarks>
    public static Entity Spawn(ActorClass c, Vec3 at, Quat rotation, Vec3 scale)
        => new(Fw.aver_fw_spawn(c.Handle, null,
                                new[] { at.X, at.Y, at.Z },
                                new[] { rotation.X, rotation.Y, rotation.Z, rotation.W },
                                new[] { scale.X, scale.Y, scale.Z }));

    /// <summary>Spawns <paramref name="c"/> at a position, unrotated and unscaled.</summary>
    public static Entity Spawn(ActorClass c, Vec3 at)
        => new(Fw.aver_fw_spawn(c.Handle, null, new[] { at.X, at.Y, at.Z }, null, null));
}
