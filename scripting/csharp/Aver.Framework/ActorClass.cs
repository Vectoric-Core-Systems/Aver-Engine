using System.Collections.Concurrent;
using System.Reflection;

namespace Aver.Framework;

/// <summary>
/// A handle to a registered class (a registry row), not an instance. Wraps the <c>aver_class</c> id.
/// 0 == invalid, like every other handle. Used to spawn actors and to resolve the by-name links a
/// GameMode carries.
/// </summary>
public readonly struct ActorClass : IEquatable<ActorClass>
{
    /// <summary>The raw ABI class handle. 0 is invalid; a live class is always positive.</summary>
    public int Handle { get; }

    internal ActorClass(int handle) => Handle = handle;

    /// <summary>Find a class by its declared name. Returns an invalid handle if no such class exists yet.</summary>
    public static ActorClass Find(string name) => new(Fw.aver_fw_class_find(name));

    public string Name => Fw.Str(Fw.aver_fw_class_name(Handle));
    public bool IsValid => Handle != 0;

    public bool Equals(ActorClass o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is ActorClass c && Equals(c);
    public override int GetHashCode() => Handle;
    public override string ToString() => IsValid ? $"{Name} (#{Handle})" : $"<invalid #{Handle}>";
}

/// <summary>
/// Resolves a managed <see cref="Type"/> to the class NAME it declared through <c>[AverClass]</c> or
/// <c>[AverGameMode]</c>. This is the bridge between the compiler's world (a C# type) and the registry's
/// world (a string name), used by the <c>Spawn&lt;T&gt;</c> sugar so a script can say
/// <c>Spawn&lt;Spinner&gt;(at)</c> and have the type carry its own class name.
/// </summary>
public static class ClassNames
{
    private static readonly ConcurrentDictionary<Type, string> s_cache = new();

    /// <summary>
    /// The declared class name for <paramref name="t"/>. Falls back to the C# type name when no marker
    /// attribute is present — a class with no <c>[AverClass]</c> registers under its type name, so the
    /// two never disagree.
    /// </summary>
    public static string Of(Type t) => s_cache.GetOrAdd(t, static type =>
    {
        AverClassAttribute? cls = type.GetCustomAttribute<AverClassAttribute>(inherit: false);
        if (cls is not null) return cls.Name;
        AverGameModeAttribute? gm = type.GetCustomAttribute<AverGameModeAttribute>(inherit: false);
        if (gm is not null) return gm.Name;
        return type.Name;
    });
}
