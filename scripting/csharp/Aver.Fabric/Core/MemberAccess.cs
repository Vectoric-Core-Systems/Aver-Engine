// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// MemberAccess: set, get and call a C# member by a Godot-style name (reflection), for Object.Set/Get/Call and scene properties.

using System.Reflection;
using System.Text;

namespace Aver.Fabric;

internal static class MemberAccess
{
    private const BindingFlags All = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;

    /// <summary>"move_speed" to "MoveSpeed"; a name that already has capitals is returned unchanged.</summary>
    internal static string SnakeToPascal(string name)
    {
        if (name.Length == 0) return name;
        var sb = new StringBuilder(name.Length);
        bool up = true;
        foreach (char c in name)
        {
            if (c == '_') { up = true; continue; }
            sb.Append(up ? char.ToUpperInvariant(c) : c);
            up = false;
        }
        return sb.ToString();
    }

    /// <summary>The spellings a Godot name may have in converted C#: as written, PascalCase, camelCase and _camelCase.</summary>
    internal static IEnumerable<string> Candidates(string name)
    {
        yield return name;
        string pascal = SnakeToPascal(name);
        if (pascal != name) yield return pascal;
        if (pascal.Length > 0)
        {
            string camel = char.ToLowerInvariant(pascal[0]) + pascal[1..];
            if (camel != name) yield return camel;
            yield return "_" + camel;
        }
        if (name.StartsWith('_') && name.Length > 1) yield return SnakeToPascal(name[1..]);
    }

    // A level's COMPONENT Props keys are lower-cased when read, so "MoveSpeed" arrives as "movespeed": the last resort matches
    // ignoring case and underscores.
    private static string Fold(string s) => s.Replace("_", "").ToLowerInvariant();

    internal static PropertyInfo? FindProperty(System.Type t, string name)
    {
        foreach (string c in Candidates(name))
        {
            PropertyInfo? p = t.GetProperty(c, All);
            if (p != null) return p;
        }
        string key = Fold(name);
        foreach (PropertyInfo p in t.GetProperties(All))
            if (p.GetIndexParameters().Length == 0 && Fold(p.Name) == key) return p;
        return null;
    }

    internal static FieldInfo? FindField(System.Type t, string name)
    {
        foreach (string c in Candidates(name))
        {
            for (System.Type? k = t; k != null && k != typeof(object); k = k.BaseType)
            {
                FieldInfo? f = k.GetField(c, All | BindingFlags.DeclaredOnly);
                if (f != null) return f;
            }
        }
        string key = Fold(name);
        for (System.Type? k = t; k != null && k != typeof(object); k = k.BaseType)
            foreach (FieldInfo f in k.GetFields(All | BindingFlags.DeclaredOnly))
                if (Fold(f.Name) == key && !f.Name.Contains('<')) return f;
        return null;
    }

    internal static bool HasMember(System.Type t, string name) => FindProperty(t, name) != null || FindField(t, name) != null;

    /// <summary>The declared type of the settable property or field a Godot name maps to, or null.</summary>
    internal static System.Type? SettableType(System.Type t, string name)
    {
        PropertyInfo? p = FindProperty(t, name);
        if (p != null && p.SetMethod != null && p.GetIndexParameters().Length == 0) return p.PropertyType;
        FieldInfo? f = FindField(t, name);
        return f != null && !f.IsInitOnly ? f.FieldType : null;
    }

    /// <summary>Sets a property or field named like a Godot property. False when there is none or it cannot be set.</summary>
    internal static bool TrySet(object target, string name, Variant value)
    {
        System.Type t = target.GetType();
        PropertyInfo? p = FindProperty(t, name);
        if (p != null && p.SetMethod != null && p.GetIndexParameters().Length == 0)
        {
            p.SetValue(target, value.ToType(p.PropertyType));
            return true;
        }
        FieldInfo? f = FindField(t, name);
        if (f != null && !f.IsInitOnly)
        {
            f.SetValue(target, value.ToType(f.FieldType));
            return true;
        }
        return false;
    }

    internal static bool TryGet(object target, string name, out Variant value)
    {
        System.Type t = target.GetType();
        PropertyInfo? p = FindProperty(t, name);
        if (p != null && p.GetMethod != null && p.GetIndexParameters().Length == 0)
        {
            value = Variant.FromObject(p.GetValue(target));
            return true;
        }
        FieldInfo? f = FindField(t, name);
        if (f != null)
        {
            value = Variant.FromObject(f.GetValue(target));
            return true;
        }
        value = default;
        return false;
    }

    internal static MethodInfo? FindMethod(System.Type t, string name, int argc)
    {
        foreach (string c in Candidates(name))
        {
            MethodInfo? best = null;
            for (System.Type? k = t; k != null && k != typeof(object); k = k.BaseType)
            {
                foreach (MethodInfo m in k.GetMethods(All | BindingFlags.DeclaredOnly))
                {
                    if (m.Name != c) continue;
                    ParameterInfo[] ps = m.GetParameters();
                    int required = ps.Count(x => !x.IsOptional);
                    if (argc < required || argc > ps.Length) continue;
                    if (best == null || ps.Length == argc) best = m;
                }
                if (best != null) break;
            }
            if (best != null) return best;
        }
        return null;
    }

    internal static bool HasMethod(System.Type t, string name)
    {
        foreach (string c in Candidates(name))
            for (System.Type? k = t; k != null && k != typeof(object); k = k.BaseType)
                if (k.GetMethods(All | BindingFlags.DeclaredOnly).Any(m => m.Name == c)) return true;
        return false;
    }

    /// <summary>Calls <paramref name="m"/> converting each Variant argument to its parameter type and filling optional ones.</summary>
    internal static Variant Invoke(object? target, MethodInfo m, Variant[] args)
    {
        ParameterInfo[] ps = m.GetParameters();
        var values = new object?[ps.Length];
        for (int i = 0; i < ps.Length; i++)
        {
            if (i < args.Length) values[i] = args[i].ToType(ps[i].ParameterType);
            else values[i] = ps[i].HasDefaultValue ? ps[i].DefaultValue : null;
        }
        try
        {
            object? r = m.Invoke(target, values);
            return r == null ? default : Variant.FromObject(r);
        }
        catch (TargetInvocationException ex) when (ex.InnerException != null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(ex.InnerException).Throw();
            throw;
        }
    }
}
