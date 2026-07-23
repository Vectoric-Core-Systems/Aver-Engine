using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>
/// Redirects this assembly's P/Invokes into the native <c>Aver.Framework</c> / <c>Aver.Scene</c> DLLs to
/// the copies that sit next to the executable.
/// </summary>
/// <remarks>
/// <para>
/// This is load-bearing, not a nicety. The managed contract assembly <c>Aver.Framework.dll</c> and the
/// native gameplay DLL it P/Invokes into share a FILE NAME (<c>Aver.Framework.dll</c>), and the same is
/// true of <c>Aver.Scene</c>. The managed one is staged in the scripting directory beside the bridge;
/// the native one is staged next to <c>Sandbox.exe</c>. The default DllImport search probes the
/// requesting assembly's OWN directory first, where it would find the managed <c>Aver.Framework.dll</c>
/// (itself) and try to load it as a native library — a guaranteed failure. A registered resolver runs
/// BEFORE that search, so it short-circuits the collision by loading the native DLL from the executable
/// directory explicitly, by full path.
/// </para>
/// <para>
/// Only the two names whose native DLL collides with a managed assembly are redirected; every other
/// P/Invoke (e.g. the render DLLs) returns <see cref="IntPtr.Zero"/> to fall through to the default
/// search, which finds them next to the executable with no help.
/// </para>
/// </remarks>
internal static class NativeResolver
{
    private static int s_installed;

    // CA2255: a ModuleInitializer registering a native-library resolver is exactly the "advanced" case the
    // rule carves out — the resolver must be in place before this assembly's first P/Invoke runs, and there
    // is no earlier hook a hosted (no Main) contract assembly can use. Suppressed deliberately.
#pragma warning disable CA2255
    [ModuleInitializer]
    internal static void Install()
    {
        // The initializer can, in principle, run more than once across contexts; make it idempotent.
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName is not ("Aver.Framework" or "Aver.Scene"))
            return IntPtr.Zero;   // not a colliding name — let the default search handle it

        string? dir = ExecutableDirectory();
        if (dir is null) return IntPtr.Zero;

        string candidate = Path.Combine(dir, libraryName + ".dll");
        return NativeLibrary.TryLoad(candidate, out IntPtr handle) ? handle : IntPtr.Zero;
    }

    // The directory of the process executable (Sandbox.exe), where the native DLLs are staged. Not
    // AppContext.BaseDirectory: for a hostfxr-loaded component that resolves to the bridge's own
    // directory, which is exactly the colliding location we must avoid.
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
