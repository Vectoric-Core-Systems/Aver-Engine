// Script assembly loading: the collectible context, and loading from an in-memory copy of the file.

using System.Reflection;
using System.Runtime.Loader;

namespace Aver.Scripting.Bridge;

/// <summary>The load context user script assemblies go into. Collectible, so a reload can unload them.</summary>
internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly AssemblyLoadContext _host;

    /// <param name="host">The context the bridge itself was loaded into; script references resolve out of it.</param>
    public ScriptLoadContext(AssemblyLoadContext host)
        : base(name: "AverScripts", isCollectible: true)
        => _host = host;

    /// <summary>Resolves a script's references out of the bridge's own load context.</summary>
    protected override Assembly? Load(AssemblyName assemblyName)
    {
        try
        {
            return _host.LoadFromAssemblyName(assemblyName);
        }
        catch
        {
            return null;
        }
    }

    /// <summary>Loads an assembly, and its .pdb when there is one, from an in-memory copy so the file on disk stays unlocked.</summary>
    public Assembly LoadFromFileCopy(string path)
    {
        byte[] image = File.ReadAllBytes(path);
        string pdb = Path.ChangeExtension(path, ".pdb");
        if (File.Exists(pdb))
        {
            byte[] symbols = File.ReadAllBytes(pdb);
            using var imageStream = new MemoryStream(image);
            using var symbolStream = new MemoryStream(symbols);
            return LoadFromStream(imageStream, symbolStream);
        }
        using var only = new MemoryStream(image);
        return LoadFromStream(only);
    }
}
