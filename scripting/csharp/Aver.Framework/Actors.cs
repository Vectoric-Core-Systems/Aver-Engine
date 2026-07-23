namespace Aver.Framework;

/// <summary>
/// Turns an <see cref="Entity"/> back into the C# object that IS that actor. The scripting host owns the
/// real entity→instance table (it constructs and binds the instances); it installs a resolver here at
/// bootstrap so any script can go from a handle to the live <see cref="AverActor"/> without this assembly
/// taking a reference on the host — the same one-way contract the managed dispatch uses.
/// </summary>
/// <remarks>
/// In a pure-native run (no CLR actors) the resolver is never installed and every lookup returns null,
/// which is exactly what a script that spawned nothing should see. Instances are not handed across a hot
/// reload: the managed half is rebuilt, so a reference cached from before a reload goes stale — re-fetch
/// in <see cref="AverActor.OnRebound"/> rather than holding one.
/// </remarks>
public static class Actors
{
    // Installed once by the scripting bridge at bootstrap: an entity handle -> its live AverActor, or null
    // when the entity is unknown or its script was disabled by a throw. Left null in a host with no
    // managed actors, where Get then returns null.
    internal static System.Func<int, AverActor?>? Resolver;

    /// <summary>The managed actor bound to <paramref name="e"/>, or null if the entity is dead, is not an
    /// actor, or its script was disabled.</summary>
    public static AverActor? Get(Entity e) => e.IsValid ? Resolver?.Invoke(e.Handle) : null;

    /// <summary>The managed actor bound to <paramref name="e"/> as <typeparamref name="T"/>, or null when
    /// it is absent or of another type.</summary>
    public static T? Get<T>(Entity e) where T : AverActor => Get(e) as T;
}
