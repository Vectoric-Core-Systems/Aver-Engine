// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The vector, string and entity-pin nodes: their default pins and the runtime helpers their IL calls.
// Palette mirror: sandbox/src/GraphNodeDefsData.hpp. Emission: GraphCompiler.Vector.cs.

using System.Globalization;
using System.Numerics;

namespace Aver.Graph;

internal enum DataNodeKind
{
    /// <summary>No side effect: recomputed per reader under either compiler.</summary>
    Pure,
    /// <summary>Runs when the exec walk reaches it; refused by Compile().</summary>
    Exec,
}

internal sealed class DataNodeSpec
{
    public required string Type { get; init; }
    public required DataNodeKind Kind { get; init; }
    public required (string Name, PinType Type, object? Default)[] Inputs { get; init; }
    public required (string Name, PinType Type)[] Outputs { get; init; }
}

internal static class DataNodes
{
    private static readonly Dictionary<string, DataNodeSpec> s_byType = Build();

    internal static bool TryGet(string type, out DataNodeSpec spec) => s_byType.TryGetValue(type, out spec!);
    internal static bool IsPure(string type) => TryGet(type, out var s) && s.Kind == DataNodeKind.Pure;
    internal static bool IsExec(string type) => TryGet(type, out var s) && s.Kind == DataNodeKind.Exec;

    internal static void AddDefaultPins(Node node, Graph graph)
    {
        if (!TryGet(node.Type, out var s)) return;
        void Add(string name, PinType t, bool isOut) =>
            node.Pins.Add(new Pin { Name = name, Type = t, IsOutput = isOut, NodeId = node.Id });
        if (s.Kind == DataNodeKind.Exec) Add("exec", PinType.Exec, false);
        foreach (var (name, type, dflt) in s.Inputs)
        {
            Add(name, type, false);
            if (dflt != null)
                graph.PinnedValues.Add(new PinnedValue { NodeId = node.Id, PinName = name, Value = dflt, FromPinDefault = true });
        }
        if (s.Kind == DataNodeKind.Exec) Add("then", PinType.Exec, true);
        foreach (var (name, type) in s.Outputs) Add(name, type, true);
    }

    private static PinType ParseType(string t) => t switch
    {
        "float" => PinType.Float,
        "int" => PinType.Int,
        "bool" => PinType.Bool,
        "float3" => PinType.Float3,
        "string" => PinType.String,
        "entity" => PinType.Entity,
        _ => throw new ArgumentException("pin type " + t),
    };

    /// <summary>ins is <c>name:type[=default],...</c>, outs is <c>name:type,...</c>.</summary>
    private static DataNodeSpec Row(string type, DataNodeKind kind, string ins, string outs)
    {
        var inputs = new List<(string, PinType, object?)>();
        foreach (var part in ins.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = part.Split('=', 2);
            var nt = eq[0].Split(':');
            var pt = ParseType(nt[1]);
            object? dflt = null;
            if (eq.Length > 1 && GraphLiterals.TryParse(pt, eq[1], out var parsed)) dflt = parsed;
            inputs.Add((nt[0], pt, dflt));
        }
        var outputs = new List<(string, PinType)>();
        foreach (var part in outs.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var nt = part.Split(':');
            outputs.Add((nt[0], ParseType(nt[1])));
        }
        return new DataNodeSpec { Type = type, Kind = kind, Inputs = inputs.ToArray(), Outputs = outputs.ToArray() };
    }

    private static Dictionary<string, DataNodeSpec> Build()
    {
        var rows = new[]
        {
            Row("ConstVector", DataNodeKind.Pure, "", "value:float3"),
            Row("ConstString", DataNodeKind.Pure, "", "value:string"),
            Row("MakeVector", DataNodeKind.Pure, "x:float,y:float,z:float", "v:float3"),
            Row("BreakVector", DataNodeKind.Pure, "v:float3", "x:float,y:float,z:float"),
            Row("VectorAdd", DataNodeKind.Pure, "a:float3,b:float3", "result:float3"),
            Row("VectorSubtract", DataNodeKind.Pure, "a:float3,b:float3", "result:float3"),
            Row("VectorScale", DataNodeKind.Pure, "v:float3,s:float=1", "result:float3"),
            Row("VectorDot", DataNodeKind.Pure, "a:float3,b:float3", "result:float"),
            Row("VectorCross", DataNodeKind.Pure, "a:float3,b:float3", "result:float3"),
            Row("VectorLength", DataNodeKind.Pure, "v:float3", "result:float"),
            Row("VectorNormalize", DataNodeKind.Pure, "v:float3", "result:float3"),
            Row("VectorLerp", DataNodeKind.Pure, "a:float3,b:float3,t:float", "result:float3"),
            Row("VectorDistance", DataNodeKind.Pure, "a:float3,b:float3", "result:float"),
            Row("StringAppend", DataNodeKind.Pure, "a:string,b:string", "result:string"),
            Row("FloatToString", DataNodeKind.Pure, "a:float", "result:string"),
            Row("IntToString", DataNodeKind.Pure, "a:int", "result:string"),
            Row("BoolToString", DataNodeKind.Pure, "a:bool", "result:string"),
            Row("VectorToString", DataNodeKind.Pure, "v:float3", "result:string"),
            Row("StringEquals", DataNodeKind.Pure, "a:string,b:string", "result:bool"),
            Row("StringLength", DataNodeKind.Pure, "s:string", "result:int"),
            Row("IsValidEntity", DataNodeKind.Pure, "e:entity", "result:bool"),
            Row("GetEntityLocation", DataNodeKind.Pure, "e:entity", "location:float3"),
            Row("SetEntityLocation", DataNodeKind.Exec, "e:entity,location:float3", ""),
            Row("RaycastVector", DataNodeKind.Exec, "origin:float3,direction:float3,maxDist:float=10000", "hit:bool,entity:entity,point:float3"),
            Row("PrintText", DataNodeKind.Exec, "text:string", ""),
        };
        var d = new Dictionary<string, DataNodeSpec>(StringComparer.OrdinalIgnoreCase);
        foreach (var r in rows) d[r.Type] = r;
        return d;
    }
}

/// <summary>The calls the compiled IL makes for the vector and string nodes; static and null-safe so the IL stays one Call each.</summary>
internal static class GraphDataOps
{
    public static float Length(Vector3 v) => v.Length();

    /// <summary>A zero vector normalises to zero, not NaN.</summary>
    public static Vector3 Normalize(Vector3 v)
    {
        float len = v.Length();
        return len > 1e-6f ? v / len : Vector3.Zero;
    }

    /// <summary>Unclamped a + (b - a) * t, like the scalar Lerp node.</summary>
    public static Vector3 Lerp(Vector3 a, Vector3 b, float t) => a + (b - a) * t;

    public static string FloatToString(float f) => f.ToString("R", CultureInfo.InvariantCulture);
    public static string IntToString(int i) => i.ToString(CultureInfo.InvariantCulture);
    public static string BoolToString(bool b) => b ? "true" : "false";

    public static string VectorToString(Vector3 v) =>
        string.Create(CultureInfo.InvariantCulture, $"X={v.X:R} Y={v.Y:R} Z={v.Z:R}");

    public static int StringLength(string? s) => s?.Length ?? 0;
}
