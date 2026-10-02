// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Script assembly loading: the collectible context, and loading from an in-memory copy of the file.

using System.Reflection;
using System.Runtime.Loader;

namespace Aver.Scripting.Bridge;

/// <summary>The load context user script assemblies go into. Collectible, so a reload can unload them.</summary>
internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly AssemblyLoadContext _host;

    // Directories LoadScripts has been pointed at. Searched only after the host context declines.
    private readonly List<string> _probeDirectories = new();

    // Every assembly this context has loaded, by full path. Two routes now reach LoadFromFileCopy for
    // the same file: the LoadScripts enumeration, and the probe below.
    //
    // THIS IS NOT WHAT KEEPS THE IDENTITY SINGLE — the runtime already does that, measured: two
    // LoadFromStream calls with the same image into one AssemblyLoadContext return the SAME Assembly
    // object (ReferenceEquals true, one entry in .Assemblies). What this saves is the second
    // File.ReadAllBytes, which for FSharp.Core.dll is 2.4 MB read and discarded on every script load
    // and every hot reload. A modest claim, and the only one that survives measurement.
    private readonly Dictionary<string, Assembly> _loadedByPath = new(StringComparer.OrdinalIgnoreCase);

    /// <param name="host">The context the bridge itself was loaded into; script references resolve out of it.</param>
    public ScriptLoadContext(AssemblyLoadContext host)
        : base(name: "AverScripts", isCollectible: true)
        => _host = host;

    /// <summary>Adds a directory this context may resolve a script's private dependencies from.</summary>
    public void AddProbeDirectory(string directory)
    {
        if (!string.IsNullOrEmpty(directory) && !_probeDirectories.Contains(directory, StringComparer.OrdinalIgnoreCase))
            _probeDirectories.Add(directory);
    }

    /// <summary>Resolves a script's references out of the bridge's own load context, and failing that out
    /// of the scripts directory itself.</summary>
    /// <remarks>THE HOST CONTEXT IS TRIED FIRST AND THAT ORDER IS LOAD-BEARING. `dotnet build -o` copies
    /// Aver.Scripting.dll and Aver.Framework.dll into the scripts folder whatever the .csproj asks for, so
    /// a directory-first probe gives AverBehaviour a second runtime identity and the discovery test then
    /// matches nothing. Not a prediction — measured, by running the two statements in the other order:
    ///
    ///   [INFO ] [Scripting] loaded Aver.Scripting.SampleFSharp.dll: 0 behaviour(s)
    ///   [WARN ] [Scripting] ...FSharpRoundTripBehaviour has lifecycle-shaped methods but does not derive
    ///           from AverBehaviour, so nothing will call them.
    ///
    /// about a class whose declaration is `: AverBehaviour`. That warning is the whole hazard in one line.
    ///
    /// The directory fallback exists because without it a script assembly may not have ANY private
    /// dependency: the host context resolves only what the bridge itself shipped with, and the default
    /// context is not reachable from a hosted component at all. Measured, before this existed, with
    /// FSharp.Core.dll sitting in the same folder as the assembly that needed it:
    ///
    ///   [ERROR] [FSharp] the round trip could not run: FileNotFoundException: Could not load file or
    ///           assembly 'FSharp.Core, Version=10.1.0.0, ...'. The system cannot find the file specified.
    ///
    /// F# is simply the first thing to need this; the limitation was never F#-specific. Resolved copies go
    /// into THIS collectible context rather than the default one, so a reload still unloads them.</remarks>
    protected override Assembly? Load(AssemblyName assemblyName)
    {
        try
        {
            return _host.LoadFromAssemblyName(assemblyName);
        }
        catch
        {
            // Falls through to the directory probe.
        }

        if (string.IsNullOrEmpty(assemblyName.Name))
            return null;

        foreach (string directory in _probeDirectories)
        {
            string candidate = Path.Combine(directory, assemblyName.Name + ".dll");
            if (!File.Exists(candidate))
                continue;
            try
            {
                return LoadFromFileCopy(candidate);
            }
            catch
            {
                // A file that is not a managed assembly, or is for another architecture. Keep looking.
            }
        }
        return null;
    }

    /// <summary>Loads an assembly, and its .pdb when there is one, from an in-memory copy so the file on disk stays unlocked.</summary>
    public Assembly LoadFromFileCopy(string path)
    {
        string full = Path.GetFullPath(path);
        if (_loadedByPath.TryGetValue(full, out Assembly? already))
            return already;

        byte[] image = File.ReadAllBytes(full);
        string pdb = Path.ChangeExtension(full, ".pdb");
        Assembly loaded;
        if (File.Exists(pdb))
        {
            byte[] symbols = File.ReadAllBytes(pdb);
            using var imageStream = new MemoryStream(image);
            using var symbolStream = new MemoryStream(symbols);
            loaded = LoadFromStream(imageStream, symbolStream);
        }
        else
        {
            using var only = new MemoryStream(image);
            loaded = LoadFromStream(only);
        }

        _loadedByPath[full] = loaded;
        return loaded;
    }
}
