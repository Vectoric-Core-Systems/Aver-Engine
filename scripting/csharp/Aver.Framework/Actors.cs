// Entity-to-instance lookup, backed by a resolver the scripting host installs at bootstrap.

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
}
