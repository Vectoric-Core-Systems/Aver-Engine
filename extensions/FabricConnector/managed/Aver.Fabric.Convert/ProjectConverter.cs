// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// `Aver.Fabric.Convert scripts`: one Godot project's C# scripts into an Aver project, and the report that goes with them.

using System.Text;
using System.Xml.Linq;

namespace Aver.Fabric.Convert;

public sealed class ProjectOptions
{
    /// <summary>The Godot project folder (holds project.godot).</summary>
    public string ProjectDir { get; set; } = "";
    /// <summary>The Aver project folder the scene converter wrote (holds Fabric/scripts.json and Content/).</summary>
    public string OutDir { get; set; } = "";
    public string? ManifestPath { get; set; }
    /// <summary>The folder with Aver.Fabric.dll, or a list of folders separated by <see cref="Path.PathSeparator"/> (default: <see cref="DefaultRefs"/>).</summary>
    public string? RefsDir { get; set; }
    public string? ReportPath { get; set; }
    /// <summary>The engine checkout: when given, a missing Content/Scripts/Scripts.csproj is written with references into it.</summary>
    public string? EnginePath { get; set; }
}

public sealed class ProjectResult
{
    public int ExitCode { get; set; }
    public ConversionResult? Conversion { get; set; }
    public List<string> Written { get; } = new();
    public List<ReportItem> Items { get; } = new();
    public string ReportPath { get; set; } = "";
}

public static class ProjectConverter
{
    /// <summary>0 converted and compiled, 1 compile errors or an unreadable project, 3 the reference folder is unusable (scripts are still written, uncompiled).</summary>
    public static ProjectResult Run(ProjectOptions o, TextWriter log)
    {
        var res = new ProjectResult();
        string projectDir = Path.GetFullPath(o.ProjectDir);
        string outDir = Path.GetFullPath(o.OutDir);
        if (!Directory.Exists(projectDir)) { log.WriteLine($"error: the Godot project folder {projectDir} does not exist"); res.ExitCode = 1; return res; }

        // The manifest: the one given, else the scene converter's, else a scan of the project.
        List<ManifestScript> entries;
        string? manifest = o.ManifestPath;
        if (manifest != null && !File.Exists(manifest)) manifest = Path.Combine(outDir, "Fabric", manifest);
        manifest ??= Path.Combine(outDir, "Fabric", "scripts.json");
        if (File.Exists(manifest))
        {
            List<ManifestScript>? read = Manifest.Read(manifest, out string? err);
            if (read == null) { log.WriteLine("error: " + err); res.ExitCode = 1; return res; }
            entries = read;
        }
        else if (o.ManifestPath != null) { log.WriteLine($"error: the manifest {o.ManifestPath} was not found"); res.ExitCode = 1; return res; }
        else { log.WriteLine($"note: no {manifest}; scanning {projectDir} for .cs and .gd files"); entries = Manifest.Scan(projectDir); }

        var sources = new List<ScriptSource>();
        var handled = new HashSet<string>(StringComparer.Ordinal);
        foreach (ManifestScript e in entries.Where(e => e.Kind is "csharp" or "gdscript"))
        {
            handled.Add(e.Path);
            string type = e.Kind == "gdscript" ? "gdscript" : "csharp";
            string disk = Manifest.DiskPath(projectDir, e.Path);
            if (!e.Exists || !File.Exists(disk))
            {
                res.Items.Add(new ReportItem { File = e.Path, Node = e.ClassName, Type = type, Rating = Rating.Unsupported, Reason = "the script file does not exist in the Godot project" });
                continue;
            }
            sources.Add(new ScriptSource
            {
                ResPath = e.Path, Text = File.ReadAllText(disk), ClassName = e.ClassName, Kind = e.Kind == "gdscript" ? "gdscript" : "csharp",
                Output = e.Output.Length > 0 ? e.Output : "Content/Scripts/" + (e.ClassName.Length > 0 ? e.ClassName : SyntaxUtil.StemOf(e.Path)) + ".cs",
            });
        }
        res.Items.AddRange(PackageReferences(projectDir, handled));

        string? refs = o.RefsDir ?? DefaultRefs();
        // The scene converter's list of the nodes it placed sits beside the manifest.
        string nodesFile = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(manifest)) ?? outDir, "nodes.json");
        string? sceneNodes = null;
        try { if (File.Exists(nodesFile)) sceneNodes = File.ReadAllText(nodesFile); }
        catch (IOException) { }
        ConversionResult conv = ScriptConverter.Convert(sources, new ConvertSettings { RefsDir = refs, ProjectDir = projectDir, SceneNodes = sceneNodes });
        res.Conversion = conv;

        foreach (ConvertedScript s in conv.Scripts)
        {
            string target = Path.Combine(outDir, s.Output.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            Manifest.Utf8NoBom(target, s.Text);
            res.Written.Add(target);
        }
        if (o.EnginePath != null && sources.Count > 0) WriteScriptsProject(outDir, o.EnginePath, refs, res.Written);

        res.Items.AddRange(conv.Items);
        foreach (string g in conv.GeneralErrors)
            res.Items.Add(new ReportItem { Type = "csharp", Rating = Rating.Unsupported, Reason = g });

        // The report: the scene converter's file, with this run's script entries in place of any earlier ones.
        res.ReportPath = ReportFile.Resolve(outDir, o.ReportPath);
        var files = new HashSet<string>(res.Items.Select(i => i.File).Where(f => f.Length > 0).Concat(handled), StringComparer.Ordinal);
        ReportFile.MergeJsonl(res.ReportPath, files, res.Items);
        ReportFile.MergeMarkdown(res.ReportPath, res.Items);

        foreach (CompileError e in conv.Errors) log.WriteLine(e.ToString());
        foreach (string g in conv.GeneralErrors) log.WriteLine("error: " + g);
        res.ExitCode = conv.GeneralErrors.Count > 0 ? 3 : conv.Errors.Count > 0 ? 1 : 0;
        return res;
    }

    /// <summary>The extension's managed folder beside the tool's own folder (Extensions/FabricConnector/tools), then the engine's bin/Scripting three
    /// levels up (Extensions/FabricConnector/tools to the executable's folder).</summary>
    public static string DefaultRefs() =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "managed")) + Path.PathSeparator +
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "Scripting"));

    // NuGet packages the Godot project's .csproj asks for: an Aver project restores offline, so they are the author's to add.
    private static IEnumerable<ReportItem> PackageReferences(string projectDir, ISet<string> handled)
    {
        var items = new List<ReportItem>();
        foreach (string proj in Directory.EnumerateFiles(projectDir, "*.csproj", SearchOption.TopDirectoryOnly))
        {
            string res = "res://" + Path.GetFileName(proj);
            try
            {
                foreach (XElement pr in XDocument.Load(proj).Descendants().Where(x => x.Name.LocalName == "PackageReference"))
                {
                    string name = (string?)pr.Attribute("Include") ?? "";
                    if (name.Length == 0 || name.StartsWith("Godot", StringComparison.OrdinalIgnoreCase)) continue;
                    handled.Add(res);
                    items.Add(new ReportItem
                    {
                        File = res, Type = "PackageReference", Property = name, Rating = Rating.Unsupported,
                        Reason = $"NuGet package {name} {(string?)pr.Attribute("Version")} is not carried over: Aver restores offline from third_party/nuget, so add the assembly to Scripts.csproj by hand",
                    });
                }
            }
            catch (Exception ex) when (ex is System.Xml.XmlException or IOException) { }
        }
        return items;
    }

    private static void WriteScriptsProject(string outDir, string engine, string refs, List<string> written)
    {
        string dir = Path.Combine(outDir, "Content", "Scripts");
        string csproj = Path.Combine(dir, "Scripts.csproj");
        if (File.Exists(csproj)) return;
        Directory.CreateDirectory(dir);
        var sb = new StringBuilder();
        sb.Append("<Project Sdk=\"Microsoft.NET.Sdk\">\n\n");
        sb.Append("  <!-- Written by Aver.Fabric.Convert for a converted Godot project. Tools > Compile Scripts builds this file; the converted scripts\n");
        sb.Append("       compile against Aver.Fabric (the Godot-compatible node layer, from the installed Fabric Connector extension) and Aver.Framework. -->\n");
        sb.Append("  <PropertyGroup>\n    <TargetFramework>net10.0</TargetFramework>\n    <Nullable>enable</Nullable>\n    <ImplicitUsings>enable</ImplicitUsings>\n  </PropertyGroup>\n\n");
        sb.Append("  <ItemGroup>\n");
        foreach (string name in new[] { "Aver.Scripting", "Aver.Framework", "Aver.UI", "Aver.Materials" })
        {
            string target = Path.Combine(engine, "scripting", "csharp", name, name + ".csproj");
            sb.Append("    <ProjectReference Include=\"").Append(Path.GetRelativePath(dir, target).Replace('/', '\\')).Append("\" />\n");
        }
        sb.Append("  </ItemGroup>\n\n");
        // Aver.Fabric is the extension's file, not an engine project: a HintPath to the installed assembly, relative to this folder like the
        // engine references. Private=false because the host loads that one assembly (a copy in Binaries/Scripts would be a second Aver.Fabric).
        string? fabric = References.Find(refs, "Aver.Fabric.dll");
        if (fabric != null)
            sb.Append("  <ItemGroup>\n    <Reference Include=\"Aver.Fabric\">\n      <HintPath>").Append(Path.GetRelativePath(dir, fabric).Replace('/', '\\'))
              .Append("</HintPath>\n      <Private>false</Private>\n    </Reference>\n  </ItemGroup>\n\n");
        else
            sb.Append("  <!-- Aver.Fabric.dll was not found when this project was converted: add a Reference to Extensions\\FabricConnector\\managed\\Aver.Fabric.dll. -->\n\n");
        sb.Append("  <ItemGroup>\n    <Compile Include=\"..\\Materials\\**\\*.cs\" />\n  </ItemGroup>\n\n</Project>\n");
        Manifest.Utf8NoBom(csproj, sb.ToString());
        written.Add(csproj);
    }
}
