// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Fabric.Convert: the command line. See docs/plans/fabric/G3_SCRIPTS.md.

namespace Aver.Fabric.Convert;

public static class ConvertCli
{
    private const string Usage =
        "usage: Aver.Fabric.Convert scripts --project <godot-dir> --out <aver-project-dir> [--manifest scripts.json] [--refs <managed-dir>[;<bin/Scripting>]]\n" +
        "                                   [--report <FabricReport.jsonl>] [--engine <engine-checkout>]\n" +
        "exit codes: 0 converted and compiled, 1 compile errors or unreadable project, 2 bad usage, 3 reference folder unusable (scripts written, not compiled)";

    public static int Run(string[] args, TextWriter stdout, TextWriter stderr)
    {
        if (args.Length == 0 || args[0] is "-h" or "--help" or "help") { stderr.WriteLine(Usage); return args.Length == 0 ? 2 : 0; }
        if (args[0] != "scripts") { stderr.WriteLine($"unknown command '{args[0]}'\n{Usage}"); return 2; }

        var o = new ProjectOptions();
        for (int i = 1; i < args.Length; i++)
        {
            string a = args[i];
            string? Next() => i + 1 < args.Length ? args[++i] : null;
            switch (a)
            {
                case "--project": o.ProjectDir = Next() ?? ""; break;
                case "--out": o.OutDir = Next() ?? ""; break;
                case "--manifest": o.ManifestPath = Next(); break;
                case "--refs": o.RefsDir = Next(); break;
                case "--report": o.ReportPath = Next(); break;
                case "--engine": o.EnginePath = Next(); break;
                default: stderr.WriteLine($"unknown option '{a}'\n{Usage}"); return 2;
            }
        }
        if (o.ProjectDir.Length == 0 || o.OutDir.Length == 0) { stderr.WriteLine("--project and --out are required\n" + Usage); return 2; }

        ProjectResult r = ProjectConverter.Run(o, stderr);
        int conv = r.Items.Count(i => i.Rating == Rating.Converted), apx = r.Items.Count(i => i.Rating == Rating.Approximated), uns = r.Items.Count(i => i.Rating == Rating.Unsupported);
        stdout.WriteLine("{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fabric-scripts\",\"scripts\":" + (r.Conversion?.Scripts.Count ?? 0) +
                         ",\"compiled\":" + ((r.Conversion?.Compiled ?? false) ? "true" : "false") + ",\"errors\":" + (r.Conversion?.Errors.Count ?? 0) +
                         ",\"converted\":" + conv + ",\"approximated\":" + apx + ",\"unsupported\":" + uns + ",\"report\":" +
                         System.Text.Json.JsonSerializer.Serialize(r.ReportPath) + ",\"exitCode\":" + r.ExitCode + "}");
        return r.ExitCode;
    }
}

internal static class Program
{
    private static int Main(string[] args) => ConvertCli.Run(args, Console.Out, Console.Error);
}
