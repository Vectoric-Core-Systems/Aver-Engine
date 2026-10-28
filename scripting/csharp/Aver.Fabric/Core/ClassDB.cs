// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// ClassDB: Godot classes by name. The converter writes class names ("MeshInstance3D", "BoxShape3D"); the runtime builds the object.

namespace Aver.Fabric;

/// <summary>Looks up and instantiates Fabric classes by their Godot name (Godot's ClassDB). Every public class of Aver.Fabric that
/// derives from <see cref="GodotObject"/> is known by its type name; a script class joins with <see cref="Register"/>.</summary>
public static class ClassDB
{
    private static readonly Dictionary<string, System.Type> s_types = new(StringComparer.Ordinal);
    private static bool s_scanned;

    private static void EnsureScanned()
    {
        lock (s_types)
        {
            if (s_scanned) return;
            s_scanned = true;
            foreach (System.Type t in typeof(GodotObject).Assembly.GetTypes())
                if (t.IsPublic && !t.IsNested && typeof(GodotObject).IsAssignableFrom(t)) s_types.TryAdd(t.Name, t);
        }
    }

    /// <summary>Adds a script class under its own name (and an optional other name), so scene values can name it.</summary>
    public static void Register(System.Type type, string? name = null)
    {
        EnsureScanned();
        if (!typeof(GodotObject).IsAssignableFrom(type)) throw new ArgumentException($"{type.Name} is not a GodotObject.", nameof(type));
        lock (s_types) s_types[name ?? type.Name] = type;
    }

    internal static System.Type? TypeOf(string name)
    {
        EnsureScanned();
        lock (s_types) return s_types.TryGetValue(name, out System.Type? t) ? t : null;
    }

    public static bool ClassExists(StringName @class) => TypeOf(@class.ToString()) != null || GDExtensionManager.ClassExists(@class.ToString());

    /// <summary>True for a class that can be made with <see cref="Instantiate"/> (not abstract, with a parameterless constructor).</summary>
    public static bool CanInstantiate(StringName @class)
    {
        System.Type? t = TypeOf(@class.ToString());
        if (t == null) return GDExtensionManager.CanInstantiate(@class.ToString());
        return !t.IsAbstract && t.GetConstructor(System.Type.EmptyTypes) != null;
    }

    /// <summary>A new object of the named class; nil when the class is unknown or abstract. A class a loaded GDExtension plugin
    /// registered (<see cref="GDExtensionManager.Load"/>) is made as the node or object that fronts its native instance.</summary>
    public static Variant Instantiate(StringName @class)
    {
        System.Type? t = TypeOf(@class.ToString());
        if (t == null) return GDExtensionManager.Instantiate(@class.ToString());
        if (!CanInstantiate(@class)) return default;
        return (GodotObject)Activator.CreateInstance(t)!;
    }

    // An object for a scene's Object(Class, "p": v, ...) value. An unknown class is an error the reader reports as bad text.
    internal static Variant Build(string @class, IEnumerable<(string Name, Variant Value)> properties)
    {
        System.Type? t = TypeOf(@class);
        if (t == null || !CanInstantiate(@class)) throw new FormatException($"unknown or abstract class '{@class}'");
        var o = (GodotObject)Activator.CreateInstance(t)!;
        foreach ((string name, Variant value) in properties)
        {
            try
            {
                if (!MemberAccess.TrySet(o, name, value))
                    Aver.Scripting.Log.Warn($"[Fabric] {@class} has no property '{name}'");
            }
            catch (Exception ex)
            {
                Aver.Scripting.Log.Warn($"[Fabric] {@class}: setting '{name}' failed: {ex.GetType().Name}: {ex.Message}");
            }
        }
        return o;
    }

    // A dictionary with a "__class" key stands for an object of that class with the other entries as its properties (the converter
    // writes a physics material this way). Null when the class is unknown or is not assignable to the wanted type.
    internal static object? FromDictionary(Collections.Dictionary d, System.Type wanted)
    {
        if (!d.TryGetValue("__class", out Variant cls)) return null;
        System.Type? t = TypeOf(cls.AsString());
        if (t == null || !wanted.IsAssignableFrom(t) || !CanInstantiate(cls.AsString())) return null;
        var props = new List<(string, Variant)>();
        foreach (KeyValuePair<Variant, Variant> kv in d)
            if (kv.Key.AsString() != "__class") props.Add((kv.Key.AsString(), kv.Value));
        return Build(cls.AsString(), props).AsGodotObject();
    }

    /// <summary>The name of the class a class extends (the nearest registered base), or empty.</summary>
    public static StringName GetParentClass(StringName @class)
    {
        System.Type? t = TypeOf(@class.ToString());
        if (t == null && GDExtensionManager.ClassExists(@class.ToString())) return GDExtensionManager.GetParentClass(@class.ToString());
        for (System.Type? b = t?.BaseType; b != null; b = b.BaseType)
            if (typeof(GodotObject).IsAssignableFrom(b) && TypeOf(b.Name) == b) return b.Name;
        return "";
    }

    /// <summary>True when <paramref name="class"/> is <paramref name="inherits"/> or extends it.</summary>
    public static bool IsParentClass(StringName @class, StringName inherits)
    {
        System.Type? t = TypeOf(@class.ToString()), p = TypeOf(inherits.ToString());
        if (t == null && GDExtensionManager.ClassExists(@class.ToString()))
        {
            // an extension class: walk its native parents; the host's Node/Node3D/... map onto the Fabric types by name
            for (string c = @class.ToString(); c.Length > 0; c = GDExtensionManager.GetParentClass(c))
                if (c == inherits.ToString()) return true;
            return false;
        }
        return t != null && p != null && p.IsAssignableFrom(t);
    }

    /// <summary>Every known class, in no particular order.</summary>
    public static string[] GetClassList()
    {
        EnsureScanned();
        lock (s_types) return s_types.Keys.Concat(GDExtensionManager.GetClassList()).ToArray();
    }

    /// <summary>Every known class that extends <paramref name="class"/> (not including it).</summary>
    public static string[] GetInheritersFromClass(StringName @class)
    {
        System.Type? p = TypeOf(@class.ToString());
        if (p == null) return System.Array.Empty<string>();
        lock (s_types) return s_types.Where(kv => kv.Value != p && p.IsAssignableFrom(kv.Value)).Select(kv => kv.Key).ToArray();
    }
}
