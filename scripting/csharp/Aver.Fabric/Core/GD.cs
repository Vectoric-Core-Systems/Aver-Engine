// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GD: the global helpers GodotSharp scripts call (Print, Randf, Load ...), RefCounted, Resource and RandomNumberGenerator.

using System.Text;
using System.Text.RegularExpressions;
using Aver.Scripting;

namespace Aver.Fabric;

/// <summary>Global functions (GodotSharp's GD).</summary>
public static partial class GD
{
    // Where Print goes. Null means the engine log; a test sets it to capture lines.
    internal static Action<string>? PrintSink;
    internal static Action<string>? ErrorSink;

    private static string Join(object?[] what, string sep)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < what.Length; i++)
        {
            if (i > 0) sb.Append(sep);
            sb.Append(Str(what[i]));
        }
        return sb.ToString();
    }

    /// <summary>Prints its arguments joined with nothing between them, as Godot's print() does in C#.</summary>
    public static void Print(params object?[] what) => Emit(Join(what, ""));

    /// <summary>Prints its arguments separated by spaces.</summary>
    public static void PrintS(params object?[] what) => Emit(Join(what, " "));

    /// <summary>Prints its arguments separated by tabs.</summary>
    public static void PrintT(params object?[] what) => Emit(Join(what, "\t"));

    /// <summary>Prints without a line ending.</summary>
    public static void PrintRaw(params object?[] what) => Emit(Join(what, ""));

    /// <summary>Prints with BBCode tags removed ("[color=red]hi[/color]" prints "hi").</summary>
    public static void PrintRich(params object?[] what) => Emit(StripBBCode(Join(what, "")));

    /// <summary>Prints to the error stream.</summary>
    public static void PrintErr(params object?[] what) => EmitError(Join(what, ""));

    public static void PushError(params object?[] what) => EmitError(Join(what, ""));
    public static void PushWarning(params object?[] what)
    {
        string s = Join(what, "");
        if (PrintSink != null) PrintSink("WARNING: " + s);
        else Log.Warn(s);
    }

    private static void Emit(string s)
    {
        if (PrintSink != null) PrintSink(s);
        else Log.Info(s);
    }

    private static void EmitError(string s)
    {
        if (ErrorSink != null) ErrorSink(s);
        else Log.Error(s);
    }

    private static readonly Regex s_bbcode = new(@"\[/?[a-zA-Z_#][^\[\]]*\]", RegexOptions.Compiled);

    internal static string StripBBCode(string s) => s_bbcode.Replace(s, "");

    /// <summary>The text of any value the way Godot's str() makes it (Variants print their Godot form).</summary>
    public static string Str(params object?[] what)
    {
        if (what.Length == 1) return One(what[0]);
        var sb = new StringBuilder();
        foreach (object? o in what) sb.Append(One(o));
        return sb.ToString();
    }

    private static string One(object? o) => o switch
    {
        null => "<null>",
        string s => s,
        bool b => b ? "true" : "false",
        float f => Variant.FormatFloat(f),
        double d => Variant.FormatFloat(d),
        Variant v => v.AsString(),
        _ => Variant.TryFrom(o, out Variant v2) ? v2.AsString() : o.ToString() ?? "",
    };

    public static string VarToStr(Variant var) => var.VariantType == Variant.Type.String ? "\"" + var.AsString() + "\"" : var.AsString();

    // ---- randomness: the shared PCG32 generator of G1 (Aver.Framework.AverRandom) ----

    public static float Randf() => Aver.Framework.AverRandom.Generator.Randf();
    public static uint Randi() => Aver.Framework.AverRandom.Generator.Randi();
    public static double RandRange(double from, double to) => from + (to - from) * Aver.Framework.AverRandom.Generator.Randf();
    public static int RandRange(int from, int to) => Aver.Framework.AverRandom.Generator.RandiRange(from, to);
    public static double Randfn(double mean, double deviation) => Aver.Framework.AverRandom.Generator.Randfn((float)mean, (float)deviation);
    public static void Randomize() => Aver.Framework.AverRandom.Generator.Randomize();
    public static void Seed(ulong seed) => Aver.Framework.AverRandom.Generator.Seed = seed;

    /// <summary>A random number from a seed, advancing the seed (Godot's rand_from_seed).</summary>
    public static uint RandFromSeed(ref ulong seed)
    {
        var rng = new Aver.Framework.RandomNumberGenerator(seed);
        uint r = rng.Randi();
        seed = rng.State;
        return r;
    }

    // ---- objects and resources ----

    public static GodotObject? InstanceFromId(ulong instanceId) => GodotObject.InstanceFromId(instanceId);

    /// <summary>Loads a resource by path (see <see cref="ResourceLoader"/>). Two loads of one path are the same object.</summary>
    public static Resource Load(string path) => ResourceLoader.Load(path) ?? throw new InvalidOperationException($"Cannot load '{path}'.");

    public static T Load<T>(string path) where T : class => (T)(object)ResourceLoader.Load(path, typeof(T))!;

    /// <summary>GDScript's preload: the resource, loaded once and kept for the run; a file that is not there is an error.</summary>
    public static T Preload<T>(string path) where T : class => ResourceLoader.Preload<T>(path);

    public static Resource Preload(string path) => ResourceLoader.Preload<Resource>(path);

    public static int Hash(Variant var) => var.GetHashCode();
}

/// <summary>Object kept alive by reference (Godot's RefCounted); in C# the garbage collector does that.</summary>
public class RefCounted : GodotObject
{
}

/// <summary>A seedable PCG32 generator (Godot's RandomNumberGenerator), on the G1 implementation.</summary>
public class RandomNumberGenerator : RefCounted
{
    private readonly Aver.Framework.RandomNumberGenerator _g = new();

    public ulong Seed { get => _g.Seed; set => _g.Seed = value; }
    public ulong State { get => _g.State; set => _g.State = value; }
    public void Randomize() => _g.Randomize();
    public float Randf() => _g.Randf();
    public float RandfRange(float from, float to) => _g.RandfRange(from, to);
    public float Randfn(float mean = 0f, float deviation = 1f) => _g.Randfn(mean, deviation);
    public uint Randi() => _g.Randi();
    public int RandiRange(int from, int to) => _g.RandiRange(from, to);
}
