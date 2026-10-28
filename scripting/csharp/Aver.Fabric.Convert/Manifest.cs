// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Fabric/scripts.json, the scene converter's list of scripts and the class each one must become (docs/plans/fabric/G3_SCENES.md),
// and the fallback for a project that has none: scan its .cs files.

using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Aver.Fabric.Convert;

internal sealed class ManifestScript
{
    public string Path = "";
    public string Kind = "";
    public string ClassName = "";
    public bool Exists = true;
    public string Output = "";
}

internal static class Manifest
{
    /// <summary>Reads scripts.json. Null with a message when the file cannot be read or is not the expected shape.</summary>
    public static List<ManifestScript>? Read(string file, out string? error)
    {
        error = null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(file));
            if (!doc.RootElement.TryGetProperty("scripts", out JsonElement arr) || arr.ValueKind != JsonValueKind.Array)
            {
                error = $"{file} has no \"scripts\" array";
                return null;
            }
            var list = new List<ManifestScript>();
            foreach (JsonElement e in arr.EnumerateArray())
            {
                string Str(string name) => e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
                list.Add(new ManifestScript
                {
                    Path = Str("path"), Kind = Str("kind"), ClassName = Str("className"), Output = Str("output"),
                    Exists = !e.TryGetProperty("exists", out JsonElement ex) || ex.ValueKind != JsonValueKind.False,
                });
            }
            return list;
        }
        catch (Exception ex) when (ex is JsonException or IOException or UnauthorizedAccessException)
        {
            error = $"cannot read {file}: {ex.Message}";
            return null;
        }
    }

    /// <summary>The project's .cs files as manifest entries, class names taken from the files, duplicates numbered.</summary>
    public static List<ManifestScript> Scan(string projectDir)
    {
        var list = new List<ManifestScript>();
        var used = new HashSet<string>(StringComparer.Ordinal);
        string root = System.IO.Path.GetFullPath(projectDir);
        foreach (string file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal))
        {
            string rel = System.IO.Path.GetRelativePath(root, file).Replace('\\', '/');
            if (rel.Split('/').Any(seg => seg.StartsWith('.') || seg is "bin" or "obj")) continue;
            string stem = SyntaxUtil.StemOf(rel);
            string name = stem;
            try
            {
                SyntaxNode tree = CSharpSyntaxTree.ParseText(File.ReadAllText(file)).GetRoot();
                ClassDeclarationSyntax? primary = SyntaxUtil.FindPrimaryClass(tree, stem);
                if (primary != null) name = primary.Identifier.ValueText;
            }
            catch (IOException) { }
            string unique = name;
            for (int n = 2; !used.Add(unique); n++) unique = name + n;
            list.Add(new ManifestScript { Path = "res://" + rel, Kind = "csharp", ClassName = unique, Output = "Content/Scripts/" + unique + ".cs" });
        }
        // GDScript: class_name, else the file name in PascalCase.
        foreach (string file in Directory.EnumerateFiles(root, "*.gd", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal))
        {
            string rel = System.IO.Path.GetRelativePath(root, file).Replace('\\', '/');
            if (rel.Split('/').Any(seg => seg.StartsWith('.') || seg is "bin" or "obj")) continue;
            string name = Aver.Fabric.Convert.GdScript.Naming.Pascal(System.IO.Path.GetFileNameWithoutExtension(rel));
            try
            {
                string? cn = Aver.Fabric.Convert.GdScript.GdScriptParser.Parse(File.ReadAllText(file)).Script?.Name;
                if (!string.IsNullOrEmpty(cn)) name = cn!;
            }
            catch (IOException) { }
            string unique = name;
            for (int n = 2; !used.Add(unique); n++) unique = name + n;
            list.Add(new ManifestScript { Path = "res://" + rel, Kind = "gdscript", ClassName = unique, Output = "Content/Scripts/" + unique + ".cs" });
        }
        return list;
    }

    public static string DiskPath(string projectDir, string resPath)
    {
        string rel = resPath.StartsWith("res://", StringComparison.Ordinal) ? resPath["res://".Length..] : resPath;
        return System.IO.Path.Combine(projectDir, rel.Replace('/', System.IO.Path.DirectorySeparatorChar));
    }

    public static string Utf8NoBom(string path, string text)
    {
        File.WriteAllText(path, text, new UTF8Encoding(false));
        return path;
    }
}
