using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Scene;

/// <summary>
/// Redirects this assembly's P/Invokes into the native <c>Aver.Scene</c> DLL to the copy that sits next
/// to the executable.
/// </summary>
/// <remarks>
/// The managed contract assembly <c>Aver.Scene.dll</c> and the native world DLL it P/Invokes into share a
/// FILE NAME. The managed one is staged in the scripting directory beside the bridge; the native one is
/// staged next to <c>Sandbox.exe</c>. The default DllImport search probes the requesting assembly's OWN
/// directory first, where it would find the managed <c>Aver.Scene.dll</c> (itself) and try to load it as
/// a native library. A registered resolver runs before that search and loads the native DLL from the
/// executable directory by full path instead. See <c>Aver.Framework/NativeResolver.cs</c> for the same
/// reasoning applied to the framework DLL.
/// </remarks>
internal static class NativeResolver
{
    private static int s_installed;

    // CA2255: registering a native-library resolver before this contract assembly's first P/Invoke is the
    // "advanced" case the rule carves out; there is no earlier hook a hosted assembly can use. Suppressed.
#pragma warning disable CA2255
    [ModuleInitializer]
    internal static void Install()
    {
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName is not ("Aver.Scene" or "Aver.Framework"))
            return IntPtr.Zero;

        string? dir = ExecutableDirectory();
        if (dir is null) return IntPtr.Zero;

        string candidate = Path.Combine(dir, libraryName + ".dll");
        return NativeLibrary.TryLoad(candidate, out IntPtr handle) ? handle : IntPtr.Zero;
    }

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
