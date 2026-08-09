// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The class-registry handle, and the Type-to-class-name resolver the Spawn<T> sugar uses.

using System.Collections.Concurrent;
using System.Reflection;

namespace Aver.Framework;

/// <summary>A handle to a registered class (a registry row), not an instance. 0 == invalid.</summary>
public readonly struct ActorClass : IEquatable<ActorClass>
{
    /// <summary>The raw ABI class handle. 0 is invalid; a live class is always positive.</summary>
    public int Handle { get; }

    internal ActorClass(int handle) => Handle = handle;

    /// <summary>Finds a class by its declared name. Invalid handle if no such class exists yet.</summary>
    public static ActorClass Find(string name) => new(Fw.aver_fw_class_find(name));

    public string Name => Fw.Str(Fw.aver_fw_class_name(Handle));
    public bool IsValid => Handle != 0;

    public bool Equals(ActorClass o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is ActorClass c && Equals(c);
    public override int GetHashCode() => Handle;
    public override string ToString() => IsValid ? $"{Name} (#{Handle})" : $"<invalid #{Handle}>";
}

/// <summary>Resolves a managed <see cref="Type"/> to the class name it declared, and caches the answer.</summary>
public static class ClassNames
{
    private static readonly ConcurrentDictionary<Type, string> s_cache = new();

    /// <summary>The declared class name for <paramref name="t"/>, or the C# type name if it has no marker attribute.</summary>
    public static string Of(Type t) => s_cache.GetOrAdd(t, static type =>
    {
        AverClassAttribute? cls = type.GetCustomAttribute<AverClassAttribute>(inherit: false);
        if (cls is not null) return cls.Name;
        AverGameModeAttribute? gm = type.GetCustomAttribute<AverGameModeAttribute>(inherit: false);
        if (gm is not null) return gm.Name;
        return type.Name;
    });
}
