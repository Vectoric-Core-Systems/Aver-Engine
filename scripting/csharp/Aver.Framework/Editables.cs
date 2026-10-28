// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Editables: an actor's [Editable] fields as text -- listed for the editor, set from a level's per-instance
// values before OnBeginPlay (Godot's exported properties).

using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework;

internal static class Editables
{
    private static readonly Dictionary<Type, FieldInfo[]> s_fields = new();

    // The [Editable] fields of a type, public or not, base classes included, in declaration order.
    internal static FieldInfo[] Of(Type t)
    {
        if (s_fields.TryGetValue(t, out FieldInfo[]? cached)) return cached;
        var list = new List<FieldInfo>();
        for (Type? k = t; k != null && k != typeof(object); k = k.BaseType)
            list.InsertRange(0, k.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly)
                                 .Where(f => f.GetCustomAttribute<EditableAttribute>() != null && !f.IsInitOnly));
        FieldInfo[] fields = list.ToArray();
        s_fields[t] = fields;
        return fields;
    }

    internal static FieldInfo? Find(Type t, string name) =>
        Of(t).FirstOrDefault(f => string.Equals(f.Name, name, StringComparison.OrdinalIgnoreCase));

    // The field's value as the text a level stores: invariant culture, a Vec3 as "x y z".
    internal static string Format(object? value) => value switch
    {
        null => "",
        float f => f.ToString("R", CultureInfo.InvariantCulture),
        double d => d.ToString("R", CultureInfo.InvariantCulture),
        bool b => b ? "true" : "false",
        Vec3 v => string.Create(CultureInfo.InvariantCulture, $"{v.X:R} {v.Y:R} {v.Z:R}"),
        IFormattable fm => fm.ToString(null, CultureInfo.InvariantCulture),
        _ => value.ToString() ?? "",
    };

    // Parses `text` into the field's type, clamps a number to the attribute's Min/Max, and writes it.
    internal static bool TrySet(object target, FieldInfo f, string text, out string why)
    {
        why = "";
        object? value;
        Type t = f.FieldType;
        string s = text.Trim();
        var inv = CultureInfo.InvariantCulture;
        try
        {
            if (t == typeof(string)) value = text;
            else if (t == typeof(bool)) value = s is "1" || s.Equals("true", StringComparison.OrdinalIgnoreCase);
            else if (t == typeof(float)) value = (float)Clamp(f, double.Parse(s, NumberStyles.Float, inv));
            else if (t == typeof(double)) value = Clamp(f, double.Parse(s, NumberStyles.Float, inv));
            else if (t == typeof(int)) value = (int)Clamp(f, long.Parse(s, NumberStyles.Integer, inv));
            else if (t == typeof(long)) value = (long)Clamp(f, long.Parse(s, NumberStyles.Integer, inv));
            else if (t.IsEnum) value = Enum.Parse(t, s, ignoreCase: true);
            else if (t == typeof(Vec3))
            {
                string[] p = s.Split(new[] { ' ', ',' }, StringSplitOptions.RemoveEmptyEntries);
                if (p.Length != 3) { why = "a Vec3 is three numbers"; return false; }
                value = new Vec3(float.Parse(p[0], inv), float.Parse(p[1], inv), float.Parse(p[2], inv));
            }
            else { why = $"a {t.Name} field cannot be set from text"; return false; }
        }
        catch (Exception ex) when (ex is FormatException or OverflowException or ArgumentException)
        {
            why = $"'{text}' is not a {t.Name}";
            return false;
        }
        f.SetValue(target, value);
        return true;
    }

    private static double Clamp(FieldInfo f, double v)
    {
        EditableAttribute a = f.GetCustomAttribute<EditableAttribute>()!;
        if (v < a.Min) v = a.Min;
        if (v > a.Max) v = a.Max;
        return v;
    }

    // Writes the entity's stored per-instance values into the actor's fields. Returns how many applied.
    internal static int ApplyProps(AverActor actor)
    {
        if (actor.EditablesApplied) return 0;
        int entity = actor.Self.Handle;
        int n = Fw.aver_fw_props_count(entity), applied = 0;
        for (int i = 0; i < n; ++i)
        {
            string name = Marshal.PtrToStringUTF8(Fw.aver_fw_props_name(entity, i)) ?? "";
            string value = Marshal.PtrToStringUTF8(Fw.aver_fw_props_value(entity, i)) ?? "";
            FieldInfo? f = Find(actor.GetType(), name);
            if (f == null) { Log.Warn($"[Editable] {actor.GetType().Name} has no [Editable] field '{name}'; its value is kept but unused"); continue; }
            if (TrySet(actor, f, value, out string why)) ++applied;
            else Log.Warn($"[Editable] {actor.GetType().Name}.{f.Name}: {why}");
        }
        return applied;
    }
}
