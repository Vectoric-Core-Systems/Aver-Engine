// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Test discovery: a test class marked [AutoRun] with `public static int RunAll()` (the house contract: it
// returns its failure count) runs without a line in Program.cs, so a new node family adds a file and edits
// nothing shared. A class is either listed in Program.cs or marked [AutoRun]; one that is both runs once,
// from its Program.cs line, and says so.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.RegularExpressions;

[AttributeUsage(AttributeTargets.Class)]
sealed class AutoRunAttribute : Attribute {}

static class AutoRun
{
    /// <summary>Runs RunAll() on every [AutoRun] class, by name, and returns the failure sum. A class without a
    /// usable RunAll, or one that throws, counts as one failure.</summary>
    public static int RunDiscovered()
    {
        var types = typeof(AutoRunAttribute).Assembly.GetTypes()
            .Where(t => t.IsClass && t.GetCustomAttribute<AutoRunAttribute>() != null)
            .OrderBy(t => t.FullName, StringComparer.Ordinal)
            .ToList();
        var listed = ListedInProgram();
        int failures = 0;
        foreach (var t in types)
        {
            if (listed.Contains(t.Name))
            {
                Console.WriteLine($"Note: {t.Name} is marked [AutoRun] and also listed in Program.cs; it ran from there.");
                continue;
            }
            var run = t.GetMethod("RunAll", BindingFlags.Public | BindingFlags.Static, null, Type.EmptyTypes, null);
            if (run == null || run.ReturnType != typeof(int))
            {
                Console.WriteLine($"  FAIL: {t.Name} is marked [AutoRun] but has no `public static int RunAll()`");
                ++failures;
                continue;
            }
            try { failures += (int)run.Invoke(null, null)!; }
            catch (TargetInvocationException e)
            {
                Console.WriteLine($"  FAIL: {t.Name}.RunAll threw {e.InnerException?.GetType().Name}: {e.InnerException?.Message}");
                ++failures;
            }
        }
        return failures;
    }

    // Class names Program.cs already runs (`X.RunAll()`), read from its source when the suite is started from the
    // project folder (dotnet run, as ctest does). Without the file nothing is skipped: the rule above is then the
    // only guard.
    private static HashSet<string> ListedInProgram()
    {
        var names = new HashSet<string>();
        foreach (var dir in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
        {
            for (var d = new DirectoryInfo(dir); d != null; d = d.Parent)
            {
                var p = Path.Combine(d.FullName, "Program.cs");
                if (!File.Exists(p) || !File.Exists(Path.Combine(d.FullName, "Aver.Graph.Tests.csproj"))) continue;
                foreach (var line in File.ReadAllLines(p))
                {
                    int c = line.IndexOf("//", StringComparison.Ordinal);
                    foreach (Match m in Regex.Matches(c < 0 ? line : line[..c], @"\b(\w+)\.RunAll\s*\(\s*\)"))
                        names.Add(m.Groups[1].Value);
                }
                return names;
            }
        }
        return names;
    }
}
