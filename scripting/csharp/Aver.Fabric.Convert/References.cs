// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The metadata a converted script compiles against: the running .NET's framework plus the Aver.* assemblies a project's Scripts.csproj
// reaches (Aver.Fabric, Aver.Framework and what they reference), read from one folder, bin/Scripting by default.

using System.Reflection;
using Microsoft.CodeAnalysis;

namespace Aver.Fabric.Convert;

internal static class References
{
    // Aver.* assemblies that live beside the runtime but are not part of what a script sees.
    private static readonly HashSet<string> s_notForScripts = new(StringComparer.OrdinalIgnoreCase)
    {
        "Aver.Scripting.Bridge", "Aver.Fabric.Convert", "Aver.Fabric.Tests", "Aver.Design",
    };

    private static readonly Dictionary<string, List<MetadataReference>> s_cache = new(StringComparer.OrdinalIgnoreCase);

    private static bool IsManaged(string path)
    {
        try { AssemblyName.GetAssemblyName(path); return true; }
        catch (Exception ex) when (ex is BadImageFormatException or FileLoadException or IOException) { return false; }
    }

    /// <summary>The references, or null with the reason when the folder does not hold the runtime.</summary>
    public static List<MetadataReference>? Build(string refsDir, out string? problem)
    {
        problem = null;
        string key = Path.GetFullPath(refsDir);
        lock (s_cache) if (s_cache.TryGetValue(key, out var cached)) return cached;

        if (!Directory.Exists(key)) { problem = $"the reference folder {key} does not exist (pass --refs <bin/Scripting>)"; return null; }

        // One path per assembly name: a name in both the framework list and the folder would be two assemblies of one identity.
        var byName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string tpa = (string?)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") ?? "";
        foreach (string p in tpa.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string n = Path.GetFileNameWithoutExtension(p);
            if (n.StartsWith("Microsoft.CodeAnalysis", StringComparison.OrdinalIgnoreCase) || n.StartsWith("Aver.", StringComparison.OrdinalIgnoreCase)) continue;
            byName.TryAdd(n, p);
        }
        foreach (string p in Directory.GetFiles(key, "*.dll"))
        {
            string n = Path.GetFileNameWithoutExtension(p);
            if (!n.StartsWith("Aver.", StringComparison.OrdinalIgnoreCase) || s_notForScripts.Contains(n) || !IsManaged(p)) continue;
            byName[n] = p;
        }
        foreach (string need in new[] { "Aver.Fabric", "Aver.Framework" })
            if (!byName.ContainsKey(need)) { problem = $"{need}.dll was not found in {key} (build the Aver.Scripting.Host target, or pass --refs)"; return null; }

        var refs = byName.Values.Select(p => (MetadataReference)MetadataReference.CreateFromFile(p)).ToList();
        lock (s_cache) s_cache[key] = refs;
        return refs;
    }
}
