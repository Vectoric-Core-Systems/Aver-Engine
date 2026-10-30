// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The converter's value types: a script to convert, the report entries (the schema of docs/plans/fabric/G3_SCENES.md) and the results.

using System.Text.Encodings.Web;
using System.Text.Json;

namespace Aver.Fabric.Convert;

/// <summary>How faithfully an item came across (the same three words as the scene converter's report).</summary>
public enum Rating { Converted, Approximated, Unsupported }

/// <summary>One line of FabricReport.jsonl.</summary>
public sealed class ReportItem
{
    public string Kind { get; init; } = "script";
    public string File { get; init; } = "";
    public string Node { get; init; } = "";
    public string Type { get; init; } = "";
    public string Property { get; init; } = "";
    public Rating Rating { get; init; }
    public string Reason { get; init; } = "";
    public int Line { get; init; }

    private static readonly JsonSerializerOptions s_json = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    private static string Q(string s) => JsonSerializer.Serialize(s, s_json);

    public static string RatingName(Rating r) => r switch { Rating.Converted => "converted", Rating.Approximated => "approximated", _ => "unsupported" };

    /// <summary>The report line, fields in the order the C++ writer uses.</summary>
    public string ToJson() =>
        "{\"schema\":1,\"kind\":" + Q(Kind) + ",\"file\":" + Q(File) + ",\"node\":" + Q(Node) + ",\"type\":" + Q(Type) +
        ",\"property\":" + Q(Property) + ",\"rating\":" + Q(RatingName(Rating)) + ",\"reason\":" + Q(Reason) + ",\"line\":" + Line + "}";
}

/// <summary>A C# script to convert: where it came from, which class the scene converter named for it, where it goes.</summary>
public sealed class ScriptSource
{
    /// <summary>The Godot path, <c>res://scripts/Player.cs</c>.</summary>
    public string ResPath { get; init; } = "";
    public string Text { get; init; } = "";
    /// <summary>The class name Fabric/scripts.json promised (the scenes' placements use it). Empty: keep the declared one.</summary>
    public string ClassName { get; init; } = "";
    /// <summary>Project-relative output path, <c>Content/Scripts/Player.cs</c>.</summary>
    public string Output { get; init; } = "";
    /// <summary>"csharp" (GodotSharp) or "gdscript" (the F4 converter writes the C#).</summary>
    public string Kind { get; init; } = "csharp";
}

/// <summary>One converted script.</summary>
public sealed class ConvertedScript
{
    public string ResPath { get; init; } = "";
    public string Output { get; init; } = "";
    public string Text { get; set; } = "";
    /// <summary>The class the scenes name for this script.</summary>
    public string ClassName { get; init; } = "";
    public List<ReportItem> Items { get; } = new();
    public int ErrorCount { get; set; }
}

/// <summary>A compile error of the converted output.</summary>
public sealed class CompileError
{
    public string ResPath { get; init; } = "";
    public string Output { get; init; } = "";
    public int Line { get; init; }
    public int Column { get; init; }
    public string Id { get; init; } = "";
    public string Message { get; init; } = "";
    /// <summary>The exact member, type or namespace the compiler could not find (the source text at the error).</summary>
    public string Member { get; init; } = "";
    public override string ToString() => $"{Output}({Line},{Column}): error {Id}: {Message}";
}

/// <summary>All of a conversion.</summary>
public sealed class ConversionResult
{
    public List<ConvertedScript> Scripts { get; } = new();
    public List<CompileError> Errors { get; } = new();
    /// <summary>Errors that belong to no file (a missing reference, say).</summary>
    public List<string> GeneralErrors { get; } = new();
    /// <summary>True when the output was compiled (a reference folder was given and held the runtime).</summary>
    public bool Compiled { get; set; }
    /// <summary>The compiled assembly, when it compiled with no errors and the caller asked for it.</summary>
    public byte[]? Assembly { get; set; }
    public bool Success => Compiled && Errors.Count == 0 && GeneralErrors.Count == 0;
    public IEnumerable<ReportItem> Items => Scripts.SelectMany(s => s.Items);
}

/// <summary>What a conversion needs besides the sources.</summary>
public sealed class ConvertSettings
{
    /// <summary>The folder holding Aver.Fabric.dll and Aver.Framework.dll, or several folders separated by <see cref="Path.PathSeparator"/>
    /// (the extension's managed folder, then the engine's bin/Scripting). Null: rewrite only, nothing is compiled.</summary>
    public string? RefsDir { get; init; }
    /// <summary>Keep the compiled assembly in the result (tests load it).</summary>
    public bool KeepAssembly { get; init; }
    /// <summary>The Godot project folder (autoloads are read from its project.godot). Optional.</summary>
    public string? ProjectDir { get; init; }
    /// <summary>The text of Fabric/nodes.json, the scene converter's list of the nodes it placed. With it a GDScript's $Path, get_node("path") and %Unique are typed
    /// from the scenes the script is attached to. Null: no scene is known and they stay untyped.</summary>
    public string? SceneNodes { get; init; }
    /// <summary>Extra C# source the compile sees besides the scripts and the reference folder (tests stub classes the runtime lacks).</summary>
    public IReadOnlyList<(string Path, string Text)>? ExtraSources { get; init; }
}
