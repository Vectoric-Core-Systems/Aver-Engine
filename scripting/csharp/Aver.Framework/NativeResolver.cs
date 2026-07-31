// DllImport resolver that loads the native Aver.Framework / Aver.Scene DLLs from the executable directory.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Redirects this assembly's P/Invokes past the managed DLLs of the same name to the native ones beside the executable.</summary>
internal static class NativeResolver
{
    private static int s_installed;

    // CA2255 suppressed: the resolver must be registered before this assembly's first P/Invoke, and a
    // hosted contract assembly has no earlier hook.
#pragma warning disable CA2255
    /// <summary>Registers the resolver. Idempotent.</summary>
    [ModuleInitializer]
    internal static void Install()
    {
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    /// <summary>Loads the two colliding library names by full path; zero for everything else.</summary>
    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName is not ("Aver.Framework" or "Aver.Scene"))
            return IntPtr.Zero;

        string? dir = ExecutableDirectory();
        if (dir is null) return IntPtr.Zero;

        string candidate = Path.Combine(dir, libraryName + ".dll");
        return NativeLibrary.TryLoad(candidate, out IntPtr handle) ? handle : IntPtr.Zero;
    }

    /// <summary>The directory of the process executable, where the native DLLs are staged. Null if unknown.</summary>
    private static string? ExecutableDirectory()
    {
        try
        {
            string? path = Process.GetCurrentProcess().MainModule?.FileName;
            return path is null ? null : Path.GetDirectoryName(path);
        }
        catch
        {
            return null;
        }
    }
}
