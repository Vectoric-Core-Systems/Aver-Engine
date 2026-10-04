// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// avermatc: loads a compiled scripts assembly, runs every [AverMaterial] type's Configure, and
// writes one .ocmat per material.
//     avermatc --assembly <path.dll> --out <dir> [--verify]
using System.Reflection;
using System.Runtime.Loader;
using Aver.Materials;

namespace Aver.MaterialCompiler;

// The avermatc command line tool.
internal static class Program
{
    // Parses the arguments, bakes every material, and returns the process exit code.
    private static int Main(string[] args)
    {
        string? assemblyPath = null;
        string? outDir = null;
        bool verify = false;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--assembly" when i + 1 < args.Length: assemblyPath = args[++i]; break;
                case "--out" when i + 1 < args.Length: outDir = args[++i]; break;
                // Writes nothing; reports what would change and fails if anything is stale.
                case "--verify": verify = true; break;
                default:
                    Console.Error.WriteLine($"avermatc: unrecognised argument '{args[i]}'");
                    return 2;
            }
        }

        if (assemblyPath is null || outDir is null)
        {
            Console.Error.WriteLine("usage: avermatc --assembly <path.dll> --out <dir> [--verify]");
            return 2;
        }
        if (!File.Exists(assemblyPath))
        {
            Console.Error.WriteLine($"avermatc: no such assembly: {assemblyPath}");
            return 1;
        }

        Assembly asm;
        try
        {
            // A load context whose resolver is rooted at the assembly's own directory.
            var ctx = new AssemblyLoadContext("avermatc", isCollectible: false);
            string dir = Path.GetDirectoryName(Path.GetFullPath(assemblyPath))!;
            ctx.Resolving += (c, name) =>
            {
                string candidate = Path.Combine(dir, name.Name + ".dll");
                return File.Exists(candidate) ? c.LoadFromAssemblyPath(candidate) : null;
            };
            asm = ctx.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"avermatc: could not load {assemblyPath}: {e.Message}");
            return 1;
        }

        Type[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException e)
        {
            // Keep the types that did load, and name the ones that did not.
            types = e.Types.Where(t => t is not null).Cast<Type>().ToArray();
            foreach (Exception? le in e.LoaderExceptions)
                if (le is not null) Console.Error.WriteLine($"avermatc: (partial load) {le.Message}");
        }

        var found = types
            .Select(t => (Type: t, Attr: t.GetCustomAttribute<AverMaterialAttribute>()))
            .Where(x => x.Attr is not null)
            .OrderBy(x => x.Attr!.Name, StringComparer.Ordinal)
            .ToList();

        if (found.Count == 0)
        {
            Console.WriteLine("avermatc: no [AverMaterial] types found; nothing to do");
            return 0;
        }

        // Two classes claiming one bound name is an error, not last-one-wins.
        var duplicates = found.GroupBy(x => x.Attr!.Name, StringComparer.Ordinal)
                              .Where(g => g.Count() > 1).ToList();
        if (duplicates.Count > 0)
        {
            foreach (var g in duplicates)
                Console.Error.WriteLine(
                    $"avermatc: '{g.Key}' is declared by {string.Join(" and ", g.Select(x => x.Type.FullName))}");
            return 1;
        }

        Directory.CreateDirectory(outDir);

        int written = 0, unchanged = 0, stale = 0, failed = 0;
        foreach (var (type, attr) in found)
        {
            string name = attr!.Name;
            string text;
            try
            {
                MaterialBuilder b = MaterialBuilder.Run(type, name);
                text = b.Emit(type.FullName ?? type.Name);
            }
            catch (TargetInvocationException e)
            {
                Console.Error.WriteLine($"avermatc: {name}: Configure threw: {e.InnerException?.Message ?? e.Message}");
                ++failed;
                continue;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"avermatc: {name}: {e.Message}");
                ++failed;
                continue;
            }

            string path = Path.Combine(outDir, name + ".ocmat");
            string? existing = File.Exists(path) ? File.ReadAllText(path) : null;

            if (existing == text)
            {
                ++unchanged;
                continue;
            }
            if (verify)
            {
                Console.Error.WriteLine($"avermatc: {name}.ocmat is stale");
                ++stale;
                continue;
            }
            File.WriteAllText(path, text);
            ++written;
        }

        Console.WriteLine(verify
            ? $"avermatc: {found.Count} material(s): {unchanged} up to date, {stale} stale, {failed} failed"
            : $"avermatc: {found.Count} material(s): {written} written, {unchanged} unchanged, {failed} failed");

        if (failed > 0) return 1;
        if (verify && stale > 0) return 1;
        return 0;
    }
}
