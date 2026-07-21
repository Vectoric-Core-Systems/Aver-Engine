using System.Reflection;
using System.Runtime.Loader;

namespace Aver.Scripting.Bridge;

/// <summary>
/// The load context user script assemblies go into. <b>Collectible</b>, and that is the whole
/// point of it.
/// </summary>
/// <remarks>
/// <para>
/// An assembly loaded into a non-collectible <see cref="AssemblyLoadContext"/> can never be
/// unloaded for the life of the process — .NET offers no way back. So hot reload is not a feature
/// that can be added later on top of an ordinary loader; it is decided here, at the first load,
/// and the decision cannot be retrofitted. It is made now even though reload itself lands in a
/// later phase.
/// </para>
/// <para>
/// Assemblies are read into memory and loaded from a stream rather than from the path, so the file
/// on disk is not locked. Without that, rebuilding the user's scripts fails with a file-in-use
/// error while the editor is open, which is precisely the workflow reload exists to serve.
/// </para>
/// </remarks>
internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly AssemblyLoadContext _host;

    /// <param name="host">The context the bridge itself was loaded into. See <see cref="Load"/>.</param>
    public ScriptLoadContext(AssemblyLoadContext host)
        : base(name: "AverScripts", isCollectible: true)
        => _host = host;

    /// <summary>
    /// Resolves a script's references out of the bridge's own load context.
    /// </summary>
    /// <remarks>
    /// The obvious implementation — return null, and let resolution fall through to the default
    /// context — is wrong here, and it fails in a way that reads as a missing file rather than as
    /// a load-context mistake. <c>load_assembly_and_get_function_pointer</c> loads a hosted
    /// component into its OWN isolated context, driven by that component's deps.json. So
    /// <c>Aver.Scripting</c> is not in the default context at all: a script referencing it got
    /// "Could not load file or assembly 'Aver.Scripting'" even though the assembly was loaded and
    /// sitting right next to the executable.
    ///
    /// Delegating to the bridge's context also keeps <c>AverBehaviour</c> to exactly ONE runtime
    /// identity. Loading a second copy into this context would leave the discovery test matching
    /// nothing, silently, which is the worse failure of the two.
    /// </remarks>
    protected override Assembly? Load(AssemblyName assemblyName)
    {
        try
        {
            return _host.LoadFromAssemblyName(assemblyName);
        }
        catch
        {
            // Not resolvable by the host either — let the runtime raise its own error against the
            // script that asked for it, which names the assembly.
            return null;
        }
    }

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
