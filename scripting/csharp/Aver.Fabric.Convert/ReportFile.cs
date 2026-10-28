// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Merges the scripts report into the scene converter's FabricReport.jsonl / FabricReport.md. Script entries for the files converted here
// replace what an earlier run (or the scene converter's "converted by a later package" placeholder) said about the same file, so a rerun
// does not pile up duplicates.

using System.Text;
using System.Text.Json;

namespace Aver.Fabric.Convert;

public static class ReportFile
{
    private const string Begin = "<!-- fabric-scripts-csharp:begin -->";
    private const string End = "<!-- fabric-scripts-csharp:end -->";

    /// <summary>Where the report goes: the explicit path, else the scene converter's file next to (or one folder above) the project.</summary>
    public static string Resolve(string outDir, string? explicitPath)
    {
        if (!string.IsNullOrEmpty(explicitPath)) return Path.GetFullPath(explicitPath);
        string here = Path.Combine(Path.GetFullPath(outDir), "FabricReport.jsonl");
        if (File.Exists(here)) return here;
        string? parent = Directory.GetParent(Path.GetFullPath(outDir).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))?.FullName;
        string above = parent == null ? here : Path.Combine(parent, "FabricReport.jsonl");
        return File.Exists(above) ? above : here;
    }

    /// <summary>Drops the existing script entries for <paramref name="files"/> and appends <paramref name="items"/>. Returns the lines kept.</summary>
    public static int MergeJsonl(string path, ISet<string> files, IEnumerable<ReportItem> items)
    {
        var lines = new List<string>();
        if (File.Exists(path))
        {
            foreach (string line in File.ReadAllLines(path))
            {
                if (line.Trim().Length == 0) continue;
                if (IsSupersededScriptLine(line, files)) continue;
                lines.Add(line);
            }
        }
        int kept = lines.Count;
        lines.AddRange(items.Select(i => i.ToJson()));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path, string.Concat(lines.Select(l => l + "\n")), new UTF8Encoding(false));
        return kept;
    }

    private static bool IsSupersededScriptLine(string line, ISet<string> files)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(line);
            JsonElement r = doc.RootElement;
            return r.TryGetProperty("kind", out JsonElement k) && k.GetString() == "script" &&
                   r.TryGetProperty("file", out JsonElement f) && files.Contains(f.GetString() ?? "");
        }
        catch (JsonException) { return false; }
    }

    /// <summary>Adds (or replaces) the "Scripts" section of FabricReport.md next to the jsonl. Does nothing when there is no .md.</summary>
    public static void MergeMarkdown(string jsonlPath, IReadOnlyList<ReportItem> items)
    {
        string md = Path.ChangeExtension(jsonlPath, ".md");
        if (!File.Exists(md)) return;
        string text = File.ReadAllText(md);
        int a = text.IndexOf(Begin, StringComparison.Ordinal);
        int b = text.IndexOf(End, StringComparison.Ordinal);
        if (a >= 0 && b > a) text = text.Remove(a, b + End.Length - a).TrimEnd() + "\n";

        var sb = new StringBuilder();
        sb.Append('\n').Append(Begin).Append('\n');
        sb.Append(items.Any(i => i.Type == "gdscript" && i.Reason.StartsWith("GDScript to", StringComparison.Ordinal)) ?"## Scripts (GodotSharp and GDScript, Aver.Fabric.Convert)\n\n" : "## Scripts (GodotSharp, Aver.Fabric.Convert)\n\n");
        sb.Append("| Rating | Count |\n|---|---:|\n");
        foreach (Rating r in new[] { Rating.Converted, Rating.Approximated, Rating.Unsupported })
            sb.Append("| ").Append(ReportItem.RatingName(r)).Append(" | ").Append(items.Count(i => i.Rating == r)).Append(" |\n");
        sb.Append('\n');
        foreach (var group in items.GroupBy(i => i.File))
        {
            sb.Append("### ").Append(group.Key.Length == 0 ? "(project)" : group.Key).Append("\n\n");
            foreach (ReportItem i in group)
            {
                sb.Append("- ").Append(ReportItem.RatingName(i.Rating)).Append(' ').Append(i.Type);
                if (i.Property.Length > 0) sb.Append(" `").Append(i.Property).Append('`');
                if (i.Reason.Length > 0) sb.Append(": ").Append(i.Reason.Replace("\n", " "));
                if (i.Line > 0) sb.Append(" [line ").Append(i.Line).Append(']');
                sb.Append('\n');
            }
            sb.Append('\n');
        }
        sb.Append(End).Append('\n');
        File.WriteAllText(md, text.TrimEnd() + "\n" + sb, new UTF8Encoding(false));
    }
}
