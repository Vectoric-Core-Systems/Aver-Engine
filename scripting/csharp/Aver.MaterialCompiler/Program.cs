// avermatc — the material compiler.
//
// C# in Content/Materials is the SOURCE; .ocmat under Binaries is the build output; the engine only
// ever reads the latter. This is the step in between: load the assembly those sources compiled into,
// find every [AverMaterial] type, run its Configure, and write one .ocmat per material.
//
//     avermatc --assembly <path.dll> --out <dir> [--verify]
//
// A console tool rather than something inside the editor, so a material can be baked by a build
// script, by CI, and by a machine with no GPU. The editor invokes it; it does not depend on the
// editor.
using System.Reflection;
using System.Runtime.Loader;
using Aver.Materials;

namespace Aver.MaterialCompiler;

internal static class Program
{
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
                // Writes nothing and reports what WOULD change. For a build that wants to fail when
                // the checked-in output is stale, rather than quietly fixing it.
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
            // A load CONTEXT with a resolver rooted at the assembly's own directory, so its
            // references (Aver.Materials, and whatever else a project's scripts pull in) are found
            // beside it. Assembly.LoadFrom would probe this tool's directory instead and fail on the
            // first reference the tool does not itself carry.
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
            // Partial results are still useful: one broken type should not hide every good material
            // in the assembly. The ones that failed are named so the cause is findable.
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
            // Not an error. A project may legitimately have no C# materials yet, and failing here
            // would make adding the compiler to a build break every project that has not adopted it.
            Console.WriteLine("avermatc: no [AverMaterial] types found; nothing to do");
            return 0;
        }

        // Duplicate bound names are refused OUTRIGHT rather than last-one-wins. Two classes claiming
        // M_Crate would produce one file whose contents depend on reflection order, which is not
        // stable and not something anybody could debug from the output.
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
                // Configure threw. Unwrapped, because the reflection wrapper is noise and the inner
                // message is the one that names the mistake.
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
            // Newline-normalised on write so the output is byte-identical whatever platform ran the
            // compiler. A file that differs only in line endings between two machines is a diff in
            // every commit and a cache miss in every build.
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
